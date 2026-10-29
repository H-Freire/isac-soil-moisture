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
  csi_item_t *item;

  lora_data_t data = {
      .hdr =
          {
              .magic = LORA_MAGIC,
              .id    = LORA_DATA_ID,
          },
  };

  while (true) {
    item = (csi_item_t *)k_fifo_get(&g_lora_fifo, K_FOREVER);

    const msg_t *const msg = (msg_t *)item->data;

    switch (msg->hdr.id) {
    case MSG_ID_SENSORS: {
      const sensor_data_t *const pkt = (sensor_data_t *)&msg->payload.sensor;

      memcpy((uint8_t *)&data.payload.sensor, (uint8_t *)pkt, SENSOR_DATA_SIZE);
      data.hdr.type      = MSG_ID_SENSORS;
      data.timestamp_sec = get_timestamp();

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
        data.hdr.type      = MSG_ID_CSI;
        data.timestamp_sec = get_timestamp();

        int ret = lora_send(g_lora_dev, (uint8_t *)&data, LORA_HDR_SIZE + CSI_DATA_SIZE);
        if (ret < 0) {
          LOG_ERR("LoRa CSI send failed");
        } else {
          LOG_INF("LoRa CSI sent");
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
  }
}
