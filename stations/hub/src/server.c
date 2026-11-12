// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/thread_stack.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/netdb.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/util.h>

#include "ca_certificate.h"
#include "hub.h"
#include "wifi_sensing.h"

#define SERVER_TX_PRIORITY   5
#define SERVER_TX_STACK_SIZE 4096

#define HTTPS_PORT 443

#define CONTENT_TYPE      "text/plain"
#define RESPONSE_BUF_SIZE 32

#define BASE64_IN_SIZE  384
#define BASE64_OUT_SIZE ROUND_UP(4 * BASE64_IN_SIZE / 3, 4)

typedef struct {
  const uint8_t *data;
  size_t len;
} upload_ctx_t;

LOG_MODULE_DECLARE(hub);

static void server_tx_thread(void *p1, void *p2, void *p3);

static struct k_thread s_server_tx_thread;

struct z_thread_stack_element __attribute__((section(".bss")))
__aligned(ARCH_THREAD_STACK_OBJ_ALIGN(ARCH_THREAD_STACK_SIZE_ADJUST((SERVER_TX_STACK_SIZE) +
                                                                    K_THREAD_STACK_RESERVED)))
    server_tx_stack[K_THREAD_STACK_LEN(SERVER_TX_STACK_SIZE)];

void create_server_thread(void) {
  k_thread_create(&s_server_tx_thread, server_tx_stack, K_THREAD_STACK_SIZEOF(server_tx_stack),
                  server_tx_thread, NULL, NULL, NULL, SERVER_TX_PRIORITY, 0, K_NO_WAIT);
}

static int http_response_cb(struct http_response *rsp, enum http_final_call final_data,
                            void *user_data) {
  LOG_INF("HTTP status: %u (%s)", rsp->http_status_code, rsp->http_status);

  /*
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
      LOG_ERR("Upload failed: %s", response_buf);
    }

    response_len = 0;
  }*/

  return 0;
}

static int payload_cb(int sock, struct http_request *req __unused, void *user_data) {
  // Zephyr's Base64 encoding uses NULL-terminated strings
  uint8_t encoded[BASE64_OUT_SIZE + 1U];

  size_t total_sent   = 0;
  size_t input_offset = 0;
  upload_ctx_t *ctx   = (upload_ctx_t *)user_data;

  if (ctx == NULL || (ctx->data == NULL && ctx->len != 0)) {
    return -EINVAL;
  }

  size_t encoded_len;
  while (input_offset < ctx->len) {
    size_t sent_offset = 0;
    size_t input_len   = MIN(ctx->len - input_offset, BASE64_IN_SIZE);

    int ret =
        base64_encode(encoded, sizeof(encoded), &encoded_len, ctx->data + input_offset, input_len);

    if (ret < 0) {
      LOG_ERR("Insufficient memory for Base64 encoding");
      return ret;
    }

    while (sent_offset < encoded_len) {
      ssize_t sent = send(sock, encoded + sent_offset, encoded_len - sent_offset, 0);

      if (sent < 0) {
        LOG_ERR("Failed to send with socket (%d)", -errno);
        return -errno;
      }

      sent_offset += (size_t)sent;
      total_sent += (size_t)sent;
    }

    input_offset += input_len;
  }

  return (int)total_sent;
}

static int server_upload_data(int sock, const uint8_t *data, const size_t len) {
  static uint8_t recv_buf[512];
  static const char *const headers[] = {
      "Content-Type: " CONTENT_TYPE "\r\n",
      NULL,
  };

  upload_ctx_t ctx = {
      .data = data,
      .len  = len,
  };

  if (!len) {
    return 0;
  }

  struct http_request req = {
      .method        = HTTP_POST,
      .host          = SERVER_HOST,
      .url           = CONFIG_SERVER_URL "?api-key=" CONFIG_SERVER_SECRET,
      .protocol      = "HTTP/1.1",
      .header_fields = headers,
      .payload_cb    = payload_cb,
      .payload_len   = ROUND_UP(4 * len / 3, 4),
      .recv_buf      = recv_buf,
      .recv_buf_len  = sizeof(recv_buf),
      .response      = http_response_cb,
  };

  // Wait 10 seconds (10_000 milliseconds) for response
  return http_client_req(sock, &req, 1000 * SENSING_INTERVAL_SEC / 2, &ctx);
}

static void server_tx_thread(void *p1 __unused, void *p2 __unused, void *p3 __unused) {
  int ret = tls_credential_add(TLS_CA_TAG, TLS_CREDENTIAL_CA_CERTIFICATE, g_google_ca,
                               sizeof(g_google_ca));
  if (ret < 0) {
    LOG_ERR("Add TLS credential failed (%d)", ret);
  }

  k_event_wait(&g_net_events, NET_READY, false, K_FOREVER);

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
  if (sock < 0) {
    LOG_ERR("Failed to create sock (%d)", -errno);
  }

  const sec_tag_t sec_tags[] = {
      TLS_CA_TAG,
  };
  if (setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, sec_tags, sizeof(sec_tags)) < 0) {
    LOG_ERR("Failed to set TLS_SEC_TAG_LIST (%d)", -errno);
    close(sock);
  }

  if (setsockopt(sock, SOL_TLS, TLS_HOSTNAME, SERVER_HOST, strlen(SERVER_HOST)) < 0) {
    LOG_ERR("Failed to set TLS_HOSTNAME (%d)", -errno);
    close(sock);
  }

  {
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    struct addrinfo *results;
    int ret = getaddrinfo(SERVER_HOST, NULL, &hints, &results);
    if (ret) {
      LOG_ERR("Failed to resolve host (%d)", ret == EAI_SYSTEM ? -errno : ret);
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
    if (k_sem_take(&g_google_sem, K_FOREVER)) {
      continue;
    }

    LOG_INF("Sem given");

    ret = server_upload_data(sock, (uint8_t *)g_csi_msg_buf, MAX_MSGS * sizeof(g_csi_msg_buf[0]));
    if (ret < 0) {
      LOG_ERR("No response from Google Apps Script (%d)", ret);
    } else {
      LOG_INF("Sent %d bytes to Google Apps Script", ret);
    }
    /* server_upload_data(sock, g_sensor_msg_buf,
                       g_sensor_msg_count * (LORA_HDR_SIZE + 8 + SENSOR_DATA_SIZE)); */
  }
}
