// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#pragma once

#include <inttypes.h>

#include <lora.h>

#define LORA_NODE DT_ALIAS(lora0)

extern const struct device *g_lora_dev;

// server_tx_thread
extern struct k_sem g_google_sem;
extern lora_data_t g_csi_msg_buf[MAX_MSGS];
extern size_t g_sensor_msg_count, g_csi_msg_count;
extern uint8_t g_sensor_msg_buf[MAX_DEVICES * (LORA_HDR_SIZE + SENSOR_DATA_SIZE)];
