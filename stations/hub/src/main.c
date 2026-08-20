// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/netdb.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/unistd.h>

#include "lora.h"
#include "wifi_sensing.h"

#include "ca_certificate.h"

#define LORA_PRIORITY     4
#define LORA_STACK_SIZE   4096
#define GOOGLE_PRIORITY   5
#define GOOGLE_STACK_SIZE 4096

#define HTTPS_PORT 443

#define CONTENT_TYPE      "application/octet-stream"
#define RESPONSE_BUF_SIZE 32

#define LORA_NODE DT_ALIAS(lora0)

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LORA_NODE), "No default LoRa radio specified in DT");

LOG_MODULE_REGISTER(sink);

static void lora_thread(void *, void *, void *);
static void google_thread(void *, void *, void *);

static const struct device *s_lora_dev = DEVICE_DT_GET(LORA_NODE);

K_SEM_DEFINE(google_sem, 0, 1);

static size_t s_sensor_msg_count, s_csi_msg_count;
static uint8_t s_sensor_msg_buf[MAX_DEVICES * (LORA_HDR_SIZE + SENSOR_DATA_SIZE)];
static lora_data_t s_csi_msg_buf[MAX_MSGS] __attribute__((section(".noinit.buf")));

K_THREAD_DEFINE(lora_thread_id, LORA_STACK_SIZE, lora_thread, NULL, NULL, NULL, LORA_PRIORITY, 0,
                0);
K_THREAD_DEFINE(google_thread_id, GOOGLE_STACK_SIZE, google_thread, NULL, NULL, NULL,
                GOOGLE_PRIORITY, 0, 0);

static int http_response_cb(struct http_response *rsp, enum http_final_call final_data,
                            void *user_data) {
  static int response_len;
  static char response_buf[RESPONSE_BUF_SIZE];

  if (rsp->body_found && rsp->body_frag_len > 0) {
    size_t copy = MIN(rsp->body_frag_len, RESPONSE_BUF_SIZE - response_len - 1);
    memcpy(&response_buf[response_len], rsp->body_frag_start, copy);

    response_len += copy;
    response_buf[response_len] = '\0';
  }

  if (final_data == HTTP_DATA_FINAL) {
    LOG_DBG("HTTP response: %s", response_buf);

    if (!strcmp(response_buf, "OK")) {
      LOG_INF("Data upload successful");
    } else {
      LOG_ERR("Upload failed");
    }

    response_len = 0;
  }

  return 0;
}

static int upload_to_google(int sock, const uint8_t *data, const size_t len) {
  static const char *const headers[] = {
      "Content-Type: " CONTENT_TYPE "\r\n",
      "X-API-Key: " CONFIG_GOOGLE_SCRIPT_SECRET "\r\n",
      NULL,
  };

  if (!len) {
    return 0;
  }

  struct http_request req = {
      .method        = HTTP_POST,
      .url           = CONFIG_GOOGLE_SCRIPT_URL,
      .host          = CONFIG_GOOGLE_SCRIPT_HOST,
      .protocol      = "HTTP/1.1",
      .header_fields = headers,
      .payload       = data,
      .payload_len   = len,
      .response      = http_response_cb,
  };

  // Wait 10 seconds (10_000 milliseconds) for response
  return http_client_req(sock, &req, 10000, NULL);
}

static void lora_thread(void *, void *, void *) {
  int8_t snr;
  int16_t rssi;

  k_event_wait(&g_net_events, NET_READY, false, K_FOREVER);

  while (true) {
    // lora_ack_t ack;
    // Non-null starting value to execute the loop once before activating the timer
    k_timeout_t timeout = {
        .ticks = 1,
    };
    struct k_timer recv_timer;

    k_timer_init(&recv_timer, NULL, NULL);

    lora_data_t *pkt;
    size_t csi_msg_count    = 0;
    size_t sensor_msg_count = 0;
    for (size_t i = 0; i < MAX_MSGS && timeout.ticks > 0; i++) {
      pkt = &s_csi_msg_buf[i];

      int len = lora_recv(s_lora_dev, (uint8_t *)pkt, sizeof(*pkt), timeout, &rssi, &snr);

      if (len < 0) {
        LOG_ERR("LoRa recv failed (%d)", len);
        continue;
      }

      if (pkt->hdr.magic != LORA_MAGIC) {
        LOG_ERR("LoRa message not intended for device");
        continue;
      }

      switch (pkt->hdr.id) {
      case LORA_DATA_ID: {
        if (!i) {
          k_timer_start(&recv_timer, K_SECONDS(SENSING_INTERVAL_SEC - 120), K_FOREVER);
        }

        if (pkt->hdr.type == MSG_ID_SENSORS) {
          static size_t s_sensor_msg_idx;

          memcpy(&s_sensor_msg_buf[s_sensor_msg_idx], (uint8_t *)pkt,
                 LORA_HDR_SIZE + SENSOR_DATA_SIZE);
          s_sensor_msg_idx =
              (s_sensor_msg_idx + LORA_HDR_SIZE + SENSOR_DATA_SIZE) % sizeof(s_sensor_msg_buf);

          i--;
          sensor_msg_count++;
        } else { // MSG_ID_CSI
          csi_msg_count++;
        }
        break;
      }
      case LORA_ACK_REQ_ID:
      default:
        break;
      }

      LOG_DBG("LoRa RX RSSI: %d dBm, SNR: %d db", rssi, snr);
      LOG_HEXDUMP_DBG(&pkt->payload, len, "Sensor msg payload");
    }

    s_csi_msg_count    = csi_msg_count;
    s_sensor_msg_count = sensor_msg_count;
    k_sem_give(&google_sem);
  }
}

static void google_thread(void *, void *, void *) {
  k_event_wait(&g_net_events, NET_READY, false, K_FOREVER);

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
  if (sock < 0) {
    LOG_ERR("Failed to create sock (%d)", -errno);
  }

  tls_credential_add(TLS_CA_TAG, TLS_CREDENTIAL_CA_CERTIFICATE, g_google_ca, sizeof(g_google_ca));

  const sec_tag_t sec_tags[] = {
      TLS_CA_TAG,
  };
  if (setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, sec_tags, sizeof(sec_tags)) < 0) {
    LOG_ERR("Failed to set TLS_SEC_TAG_LIST (%d)", -errno);
    close(sock);
  }

  if (setsockopt(sock, SOL_TLS, TLS_HOSTNAME, CONFIG_GOOGLE_SCRIPT_HOST,
                 strlen(CONFIG_GOOGLE_SCRIPT_HOST)) < 0) {
    LOG_ERR("Failed to set TLS_HOSTNAME (%d)", -errno);
    close(sock);
  }

  {
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    struct addrinfo *results;
    if (getaddrinfo(CONFIG_GOOGLE_SCRIPT_HOST, NULL, &hints, &results)) {
      LOG_ERR("Failed to resolve host (%d)", -EHOSTUNREACH);
      close(sock);
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(HTTPS_PORT),
        .sin_addr   = ((struct sockaddr_in *)results->ai_addr)->sin_addr,
    };

    freeaddrinfo(results);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      LOG_ERR("Failed to connect sock (%d)", -errno);
      close(sock);
    }
  }

  while (true) {
    if (k_sem_take(&google_sem, K_FOREVER)) {
      continue;
    }

    upload_to_google(sock, (uint8_t *)s_csi_msg_buf,
                     s_csi_msg_count * (LORA_HDR_SIZE + CSI_DATA_SIZE));
    upload_to_google(sock, s_sensor_msg_buf,
                     s_sensor_msg_count * (LORA_HDR_SIZE + SENSOR_DATA_SIZE));
  }
}

int main(void) {
  LOG_INF("Starting LoRa sink");

  net_register(NULL);

  ap_connect(NULL);
  lora_init(s_lora_dev, false);
}
