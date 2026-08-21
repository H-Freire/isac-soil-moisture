// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <inttypes.h>

#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lora.h>

#include "hub.h"

#define LORA_RX_PRIORITY   4
#define LORA_RX_STACK_SIZE 4096

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LORA_NODE), "No default LoRa radio specified in DT");

LOG_MODULE_DECLARE(hub);

static void lora_rx_thread(void *p1, void *p2, void *p3);

K_THREAD_DEFINE(lora_rx_tid, LORA_RX_STACK_SIZE, lora_rx_thread, NULL, NULL, NULL, LORA_RX_PRIORITY,
                0, 0);

static void lora_rx_thread(void *p1 __unused, void *p2 __unused, void *p3 __unused) {
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
      pkt = &g_csi_msg_buf[i];

      int len = lora_recv(g_lora_dev, (uint8_t *)pkt, sizeof(*pkt), timeout, &rssi, &snr);

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

          memcpy(&g_sensor_msg_buf[s_sensor_msg_idx], (uint8_t *)pkt,
                 LORA_HDR_SIZE + SENSOR_DATA_SIZE);
          s_sensor_msg_idx =
              (s_sensor_msg_idx + LORA_HDR_SIZE + SENSOR_DATA_SIZE) % sizeof(g_sensor_msg_buf);

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

    g_csi_msg_count    = csi_msg_count;
    g_sensor_msg_count = sensor_msg_count;
    k_sem_give(&g_google_sem);
  }
}
