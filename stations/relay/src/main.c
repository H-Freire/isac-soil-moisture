// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_mgmt.h>

#include "lora.h"

#include "relay.h"

#define NET_EVENTS                                                                                 \
  (NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_STA_CONNECTED |                             \
   NET_EVENT_WIFI_AP_STA_DISCONNECTED)

LOG_MODULE_REGISTER(relay);

const struct device *g_lora_dev = DEVICE_DT_GET(LORA_NODE);

static struct net_mgmt_event_callback s_net_cb;

K_EVENT_DEFINE(g_ap_events);

K_FIFO_DEFINE(g_lora_fifo);
K_FIFO_DEFINE(g_storage_fifo);
K_MEM_SLAB_DEFINE_TYPE(g_csi_slab, csi_item_t, (MAX_MSGS / MAX_MSG_PAYLOAD));

static void net_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
                              struct net_if *iface) {
  switch (mgmt_event) {
  case NET_EVENT_WIFI_AP_ENABLE_RESULT: {
    k_event_post(&g_ap_events, NET_READY);
    LOG_INF("AP Mode is enabled. Waiting for station to connect");
    break;
  }
  case NET_EVENT_WIFI_AP_STA_CONNECTED: {
    LOG_INF("Station connected");
    break;
  }
  case NET_EVENT_WIFI_AP_STA_DISCONNECTED: {
    LOG_INF("Station disconnected");
    break;
  }
  }
}

int main(void) {
  LOG_INF("Starting LoRa <-> Wi-Fi gateway");

  net_mgmt_init_event_callback(&s_net_cb, net_event_handler, NET_EVENTS);
  net_mgmt_add_event_callback(&s_net_cb);

  sd_card_init();
  ap_init();
  lora_init(g_lora_dev, true);

  return 0;
}
