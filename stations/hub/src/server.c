// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/netdb.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/unistd.h>

#include "ca_certificate.h"
#include "hub.h"

#define SERVER_TX_PRIORITY   5
#define SERVER_TX_STACK_SIZE 4096

#define HTTPS_PORT 443

#define CONTENT_TYPE      "application/octet-stream"
#define RESPONSE_BUF_SIZE 32

LOG_MODULE_DECLARE(hub);

static void server_tx_thread(void *p1, void *p2, void *p3);

K_THREAD_DEFINE(server_tx_tid, SERVER_TX_STACK_SIZE, server_tx_thread, NULL, NULL, NULL,
                SERVER_TX_PRIORITY, 0, 0);

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

static int server_upload_data(int sock, const uint8_t *data, const size_t len) {
  static const char *const headers[] = {
      "Content-Type: " CONTENT_TYPE "\r\n",
      "X-API-Key: " CONFIG_SERVER_SECRET "\r\n",
      NULL,
  };

  if (!len) {
    return 0;
  }

  struct http_request req = {
      .method        = HTTP_POST,
      .url           = CONFIG_SERVER_URL,
      .host          = CONFIG_SERVER_HOST,
      .protocol      = "HTTP/1.1",
      .header_fields = headers,
      .payload       = data,
      .payload_len   = len,
      .response      = http_response_cb,
  };

  // Wait 10 seconds (10_000 milliseconds) for response
  return http_client_req(sock, &req, 10000, NULL);
}

static void server_tx_thread(void *p1 __unused, void *p2 __unused, void *p3 __unused) {
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

  if (setsockopt(sock, SOL_TLS, TLS_HOSTNAME, CONFIG_SERVER_HOST, strlen(CONFIG_SERVER_HOST)) < 0) {
    LOG_ERR("Failed to set TLS_HOSTNAME (%d)", -errno);
    close(sock);
  }

  {
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    struct addrinfo *results;
    if (getaddrinfo(CONFIG_SERVER_HOST, NULL, &hints, &results)) {
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
    if (k_sem_take(&g_google_sem, K_FOREVER)) {
      continue;
    }

    server_upload_data(sock, (uint8_t *)g_csi_msg_buf,
                       g_csi_msg_count * (LORA_HDR_SIZE + CSI_DATA_SIZE));
    server_upload_data(sock, g_sensor_msg_buf,
                       g_sensor_msg_count * (LORA_HDR_SIZE + SENSOR_DATA_SIZE));
  }
}
