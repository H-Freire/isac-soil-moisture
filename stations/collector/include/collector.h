// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <wifi_sensing.h>

#define PACKED __attribute__((packed))

#define ADC_RES           12
#define ADC_NODE          DT_ALIAS(adc0)
#define ADC_CHANNEL_COUNT DT_CHILD_NUM(ADC_NODE)

#define RF_SWITCH_NODE DT_NODELABEL(rf_switch)

typedef enum {
  INT_ANT,
  EXT_ANT,
} antenna_e;

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(ADC_NODE), "ADC controller disabled");

static const struct gpio_dt_spec s_ant_sel = GPIO_DT_SPEC_GET(RF_SWITCH_NODE, select_gpios);

extern const struct device *g_adc;
extern const struct adc_channel_cfg g_adc_cfgs[ADC_CHANNEL_COUNT];

extern struct in_addr g_ap_addr;
extern struct k_sem g_sensing_sem;
extern uint8_t g_mac_addr[MAC_ADDR_LEN];
extern struct adc_sequence g_adc_sequence;

void wifi_csi_init(void);
int udp_socket_open(struct in_addr dest_addr);

static inline void enable_antenna(antenna_e antenna) {
  LOG_MODULE_DECLARE(collector);

  if (!gpio_is_ready_dt(&s_ant_sel)) {
    LOG_ERR("Antenna select GPIO not ready");
    return;
  }

  gpio_pin_set_dt(&s_ant_sel, antenna);
}
