// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#pragma once

#include <inttypes.h>

#include <zephyr/device.h>

#include "wifi_sensing.h"

#define LORA_MAGIC    0xDEADC0DE
#define LORA_HDR_SIZE sizeof(lora_msg_hdr_t)

enum {
  LORA_DATA_ID    = 0x0987,
  LORA_ACK_ID     = 0x1234,
  LORA_ACK_REQ_ID = 0xABCD,
};

typedef struct {
  uint32_t magic;
  uint16_t id;
  uint16_t type;
} PACKED lora_msg_hdr_t;

typedef struct {
  lora_msg_hdr_t hdr;

  unsigned bitmap : 10;
} PACKED lora_ack_t;

typedef struct {
  lora_msg_hdr_t hdr;

  union {
    wifi_metrics_t csi;
    sensor_data_t sensor;
  } payload;
} PACKED lora_data_t;

int lora_init(const struct device *lora_dev, bool tx);
