// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <inttypes.h>

#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lora.h>
#include <wifi_sensing.h>

#include "relay.h"

#define LORA_TX_PRIORITY   4
#define LORA_TX_STACK_SIZE 1024

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LORA_NODE), "No default LoRa radio specified in DT");

LOG_MODULE_DECLARE(relay);

static void lora_tx_thread(void *p1, void *p2, void *p3);

K_THREAD_DEFINE(lora_tx_tid, LORA_TX_STACK_SIZE, lora_tx_thread, NULL, NULL, NULL, LORA_TX_PRIORITY,
                0, 0);

static void lora_tx_thread(void *p1 __unused, void *p2 __unused, void *p3 __unused) {
  // lora_ack_t ack;
  // struct k_timer ack_timer;
  csi_item_t *item;

  lora_data_t data = {
      .hdr =
          {
              .magic = LORA_MAGIC,
              .id    = LORA_DATA_ID,
          },
  };

  // k_timer_init(&ack_timer, NULL, NULL);

  while (true) {
    item = (csi_item_t *)k_fifo_get(&g_lora_fifo, K_FOREVER);

    const msg_t *const msg = (msg_t *)item->data;

    switch (msg->hdr.id) {
    case MSG_ID_SENSORS: {
      const sensor_data_t *const pkt = (sensor_data_t *)&msg->payload.sensor;

      memcpy((uint8_t *)&data.payload.sensor, (uint8_t *)pkt, SENSOR_DATA_SIZE);
      data.hdr.type = MSG_ID_SENSORS;

      int ret = lora_send(g_lora_dev, (uint8_t *)&data, LORA_HDR_SIZE + SENSOR_DATA_SIZE);
      if (ret < 0) {
        LOG_ERR("LoRa sensor send failed");
      }
      break;
    }
    case MSG_ID_CSI: {
      const csi_data_t *const pkt = (csi_data_t *)&msg->payload.csi;
      for (size_t i = 0; i < pkt->count; i++) {
        memcpy((uint8_t *)&data.payload.csi, (uint8_t *)&pkt->data[i], CSI_DATA_SIZE);
        data.hdr.type = MSG_ID_CSI;

        int ret = lora_send(g_lora_dev, (uint8_t *)&data, LORA_HDR_SIZE + CSI_DATA_SIZE);
        if (ret < 0) {
          LOG_ERR("LoRa CSI send failed");
        }
      }
      break;
    }
    default: {
      LOG_WRN("Unrecognized message received");
      break;
    }
    }

    if (atomic_dec(&item->refs) == 1) {
      k_mem_slab_free(&g_csi_slab, (void *)item);
    }

    /* TODO: ACK request */

    /*
    k_timer_start(&ack_timer, K_SECONDS(2), K_FOREVER);

    int8_t snr;
    int16_t rssi;
    k_timeout_t timeout;
    while ((timeout.ticks = k_timer_remaining_ticks(&ack_timer)) > 0) {
      int ret = lora_recv(g_lora_dev, (uint8_t *)&ack, sizeof(ack), timeout, &rssi, &snr);
      if (ret < 0) {
        LOG_ERR("LoRa recv failed (%d)", ret);
        continue;
      }

      if (ack.magic != LORA_MAGIC || ack.id != LORA_ACK_ID) {
        LOG_INF("LoRa message not intended for device");
        continue;
      }

      // TODO: Evaluate what to resend (bitmap)
      break;
    }
    */
  }
}
