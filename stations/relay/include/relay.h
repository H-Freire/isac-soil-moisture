// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#pragma once

#include <inttypes.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <wifi_sensing.h>

#define LORA_NODE DT_ALIAS(lora0)

typedef struct {
  void *fifo_reserved;
  atomic_t refs;
  uint8_t data[MSG_HDR_SIZE + MAX_MSG_PAYLOAD * CSI_DATA_SIZE];
} csi_item_t;

extern const struct device *g_lora_dev;

extern struct k_event g_ap_events;
extern struct k_fifo g_lora_fifo;
extern struct k_fifo g_storage_fifo;
extern struct k_mem_slab g_csi_slab;

void ap_init(void);
void sd_card_init(void);
