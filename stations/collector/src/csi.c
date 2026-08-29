// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <stdint.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/posix/sys/socket.h>

#include <esp_wifi.h>

#include <wifi_sensing.h>

#include "collector.h"

#define CSI_PRIORITY   4
#define CSI_STACK_SIZE 2048

#define QUEUE_SIZE 40
#define MSG_COUNT  (SENSING_WINDOW_SIZE / MAX_MSG_PAYLOAD)

typedef struct {
  unsigned : 32; /**< reserved */
  unsigned : 32; /**< reserved */
  unsigned : 32; /**< reserved */
  unsigned : 32; /**< reserved */
  unsigned : 32; /**< reserved */
#if CONFIG_IDF_TARGET_ESP32S2
  unsigned : 32; /**< reserved */
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C5 ||       \
    CONFIG_IDF_TARGET_ESP32C6
  unsigned : 16; /**< reserved */
  unsigned fft_gain : 8;
  unsigned agc_gain : 8;
  unsigned : 32; /**< reserved */
#endif
  unsigned : 32; /**< reserved */
#if CONFIG_IDF_TARGET_ESP32S2
  signed : 8;    /**< reserved */
  unsigned : 24; /**< reserved */
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C5
  unsigned : 32; /**< reserved */
  unsigned : 32; /**< reserved */
  unsigned : 32; /**< reserved */
#endif
  unsigned : 32; /**< reserved */
} PACKED wifi_rx_ctrl_phy_t;

LOG_MODULE_DECLARE(collector);

static void csi_thread(void *, void *, void *);

K_TIMER_DEFINE(s_sensing_timer, NULL, NULL);
K_MSGQ_DEFINE(s_csi_msgq, CSI_DATA_SIZE, QUEUE_SIZE, 4);

K_THREAD_DEFINE(csi_tid, CSI_STACK_SIZE, csi_thread, NULL, NULL, NULL, CSI_PRIORITY, 0, 0);

static void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info) {
  wifi_metrics_t frame;

  if (!info || !info->buf) {
    LOG_WRN("<invalid arg> wifi_csi_cb. info: %p %" PRIu16 " %" PRIu16, info->buf, info->len,
            info->rx_seq);
    return;
  }
  const wifi_pkt_rx_ctrl_t *rx_ctrl       = &info->rx_ctrl;
  const wifi_action_frame_t *action_frame = (wifi_action_frame_t *)info->hdr;
  const msg_t *msg                        = (msg_t *)&action_frame->body;

  if (action_frame->frame_ctrl.subtype != ACTION_FRAME_SUBTYPE) {
    LOG_INF("Non action frame, discarding...");
    return;
  }

  if (!IS_BROADCAST_ADDR(action_frame->da)) {
    LOG_INF("Frame from neighboring node, discarding...");
    return;
  }

  if (msg->hdr.id != MSG_ID_PROBE) {
    LOG_INF("Miscellaneous action frame, discarding...");
    return;
  }
  const probe_data_t *probe_msg = (probe_data_t *)&msg->payload.probe;

  memcpy(frame.csi, info->buf, info->len);
  frame.rssi = rx_ctrl->rssi;
  frame.agc  = ((wifi_rx_ctrl_phy_t *)info)->agc_gain;
  frame.seq  = probe_msg->seq;

  LOG_INF("Received CSI frame with size %" PRIu16 ". RSSI: %d dBm. gain: %d, seq: %u", info->len,
          frame.rssi, frame.agc, frame.seq);

  if (!k_msgq_put(&s_csi_msgq, &frame, K_NO_WAIT)) {
    LOG_INF("CSI frame collected [%d dBm]", frame.rssi);
  } else {
    LOG_ERR("Failed to send CSI frame");
  }

  k_sem_give(&g_sensing_sem);
}

void wifi_csi_init(void) {
  const wifi_promiscuous_filter_t filter = {
      .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT,
  };

  wifi_csi_config_t csi_config = {
      .enable             = true,
      .acquire_csi_legacy = true,
  };

  if (esp_wifi_set_promiscuous(true)) {
    LOG_ERR("Failed to enable promiscuous mode");
    return;
  }

  if (esp_wifi_set_promiscuous_filter(&filter)) {
    LOG_ERR("Failed to set promiscuous filter");
    return;
  }

  if (esp_wifi_set_csi_config(&csi_config)) {
    LOG_ERR("Failed to set CSI config");
    return;
  }

  if (esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, NULL)) {
    LOG_ERR("Failed to register CSI callback");
    return;
  }

  esp_wifi_set_csi(false);
  LOG_INF("CSI init complete...");
}

static void csi_thread(void *p1 __unused, void *p2 __unused, void *p3 __unused) {
  static uint8_t msg_buf[MSG_COUNT][MSG_HDR_SIZE + MAX_MSG_PAYLOAD * CSI_DATA_SIZE];

  k_event_wait(&g_net_events, NET_READY, false, K_FOREVER);

  int sock = udp_socket_open(g_ap_addr);

  while (true) {
    enable_antenna(EXT_ANT);
    esp_wifi_set_csi(true);

    uint8_t count = 0;
    for (size_t tx = 0; tx < MSG_COUNT; tx++) {
      msg_t *const msg = (msg_t *)msg_buf[tx];
      msg->hdr.id      = MSG_ID_CSI;
      count++;

      csi_data_t *const pkt = (csi_data_t *)&msg->payload.csi;
      pkt->count            = 0;
      for (size_t i = 0; i < MAX_MSG_PAYLOAD; i++) {
        // Reception timeout after one sensing window
        k_timeout_t timeout = i ? K_SECONDS(SENSING_WINDOW_SEC) : K_FOREVER;

        if (k_msgq_get(&s_csi_msgq, &pkt->data[i], timeout)) {
          goto transmit;
        }
        memcpy(pkt->data[i].mac, g_mac_addr, MAC_ADDR_LEN);
        pkt->count++;
      }
    }
  transmit:
    esp_wifi_set_csi(false);
    enable_antenna(INT_ANT);

    // Start timer with a margin of two sensing window
    k_timer_start(&s_sensing_timer, K_SECONDS(SENSING_INTERVAL_SEC - (3 * SENSING_WINDOW_SEC)),
                  K_NO_WAIT);

    for (size_t i = 0; i < count; i++) {
      k_event_wait(&g_net_events, NET_READY, false, K_FOREVER);

      int ret = send(sock, msg_buf[i],
                     MSG_HDR_SIZE + ((msg_t *)msg_buf[i])->payload.csi.count * CSI_DATA_SIZE, 0);

      if (ret < 0) {
        LOG_ERR("Failed to send CSI UDP packet: %d", errno);
      } else {
        LOG_INF("Sent %d bytes to AP", ret);
      }
    }

    // Block until next sensing window
    k_timer_status_sync(&s_sensing_timer);
  }
}
