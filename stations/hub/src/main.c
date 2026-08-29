// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>

#include <lora.h>
#include <wifi_sensing.h>

#include "hub.h"

LOG_MODULE_REGISTER(hub);

const struct device *g_lora_dev = DEVICE_DT_GET(LORA_NODE);

size_t g_sensor_msg_count, g_csi_msg_count;
uint8_t g_sensor_msg_buf[MAX_DEVICES * (LORA_HDR_SIZE + SENSOR_DATA_SIZE)];
lora_data_t g_csi_msg_buf[MAX_MSGS] __attribute__((section(".noinit.buf")));

K_SEM_DEFINE(g_google_sem, 0, 1);

int main(void) {
  LOG_INF("Starting hub");

  net_register(NULL);

  ap_connect(NULL);
  lora_init(g_lora_dev, false);
}
