// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/unistd.h>

#include <wifi_sensing.h>

#include "collector.h"

LOG_MODULE_REGISTER(collector);

// clang-format off
const struct device *g_adc                = DEVICE_DT_GET(ADC_NODE);
const struct adc_channel_cfg g_adc_cfgs[ADC_CHANNEL_COUNT] = {
    DT_FOREACH_CHILD_SEP(ADC_NODE, ADC_CHANNEL_CFG_DT, (,))
};
// clang-format on

struct in_addr g_ap_addr;
uint8_t g_mac_addr[MAC_ADDR_LEN];

K_SEM_DEFINE(g_sensing_sem, 0, 1);

int udp_socket_open(struct in_addr dest_addr) {
  int sock;
  char ip_str[INET_ADDRSTRLEN];

  struct sockaddr_in server_addr = {
      .sin_family = AF_INET,
      .sin_port   = htons(CONFIG_UDP_PORT),
      .sin_addr   = dest_addr,
  };

  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    LOG_ERR("Failed to create UDP socket: %d", errno);
    return sock;
  }

  if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    LOG_ERR("Failed to connect UDP socket: %d", errno);
    close(sock);
    return -1;
  }

  inet_ntop(AF_INET, &dest_addr, ip_str, sizeof(ip_str));
  LOG_INF("UDP socket registered to %s:%d", ip_str, CONFIG_UDP_PORT);

  return sock;
}

static void adc_init(void) {
  if (!device_is_ready(g_adc)) {
    LOG_ERR("ADC device controller is not ready\n");
  }

  for (size_t i = 0; i < ADC_CHANNEL_COUNT; i++) {
    int err = adc_channel_setup(g_adc, &g_adc_cfgs[i]);

    if (err < 0) {
      LOG_ERR("Could not setup channel #%d: %d\n", g_adc_cfgs[i].channel_id, err);
    }
  }

  // Only setup first ADC sensor (for now)
  g_adc_sequence.channels |= BIT(g_adc_cfgs[0].channel_id);
}

int main(void) {
  LOG_INF("Starting Wi-Fi sensor");

  // Enable environmental sensors and integrated (communication) antenna
  adc_init();
  enable_antenna(INT_ANT);

  net_register(&g_ap_addr);

  wifi_csi_init();
  ap_connect(g_mac_addr);

  return 0;
}
