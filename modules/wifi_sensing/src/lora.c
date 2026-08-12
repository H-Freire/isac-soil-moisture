// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/logging/log.h>

#include "lora.h"

LOG_MODULE_DECLARE(wifi_sense);

#define LORA_DEFAULT_CONFIG()                                                                      \
  {                                                                                                \
      .frequency    = 915000000,                                                                   \
      .bandwidth    = BW_125_KHZ,                                                                  \
      .datarate     = SF_7,                                                                        \
      .coding_rate  = CR_4_5,                                                                      \
      .preamble_len = 8,                                                                           \
      .tx_power     = 14,                                                                          \
  }

int lora_init(const struct device *lora_dev, bool tx) {
  if (!device_is_ready(lora_dev)) {
    LOG_ERR("LoRa device not ready");
    return -ENODEV;
  }

  struct lora_modem_config config = LORA_DEFAULT_CONFIG();

  if (tx) {
    config.tx = true;
  }

  int ret = lora_config(lora_dev, &config);
  if (ret < 0) {
    LOG_ERR("LoRa config failed");
    return ret;
  }

  return 0;
}
