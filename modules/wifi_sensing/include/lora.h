// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: CC-BY-SA-4.0

#pragma once

#include <inttypes.h>

#include <zephyr/device.h>

#include "wifi_sensing.h"

#define LORA_MAGIC     0xDEADC0DE
#define LORA_DATA_SIZE sizeof(lora_data_t)

enum {
  LORA_DATA_ID    = 0x0987,
  LORA_ACK_ID     = 0x1234,
  LORA_ACK_REQ_ID = 0xABCD,
};

typedef uint16_t lora_msg_id_t;

typedef struct {
  uint32_t magic;
  lora_msg_id_t id;
  unsigned bitmap : 10;
} __attribute__((packed)) lora_ack_t;

typedef struct {
  uint32_t magic;
  lora_msg_id_t id;
  sensor_data_t payload;
} __attribute__((packed)) lora_data_t;

int lora_init(const struct device *lora_dev, bool tx);
