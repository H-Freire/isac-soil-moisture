// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <inttypes.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>

#include "wifi_sensing.h"

#define NET_EVENT_IPV4_MASK (NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL)
#define NET_EVENT_WIFI_MASK (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_COMPLETE)

LOG_MODULE_REGISTER(wifi_sense);

static void net_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
                              struct net_if *iface);

const uint8_t g_broadcast_addr[MAC_ADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

K_EVENT_DEFINE(g_net_events);

static struct in_addr *s_ap_addr;
static struct net_mgmt_event_callback s_net_ipv4_cb;
static struct net_mgmt_event_callback s_net_wifi_cb;

void ap_connect(uint8_t *mac_addr) {
  struct net_if *iface     = net_if_get_wifi_sta();
  struct net_linkaddr *mac = net_if_get_link_addr(iface);

  if (mac_addr) {
    memcpy(mac_addr, mac->addr, MAC_ADDR_LEN);
  }

  struct wifi_connect_req_params sta_config = {
      .ssid        = CONFIG_WIFI_SSID,
      .ssid_length = strlen(CONFIG_WIFI_SSID),
#if (CONFIG_WIFI_CHANNEL == 0)
      .channel = WIFI_CHANNEL_ANY,
#else
      .channel = CONFIG_WIFI_CHANNEL,
#endif
#if CONFIG_WIFI_WPA_ENTERPRISE
      .security          = WIFI_SECURITY_TYPE_EAP_TTLS_MSCHAPV2,
      .eap_identity      = CONFIG_WIFI_EAP_ID,
      .eap_id_length     = strlen(CONFIG_WIFI_EAP_ID),
      .eap_password      = CONFIG_WIFI_EAP_PASSWD,
      .eap_passwd_length = strlen(CONFIG_WIFI_EAP_PASSWD),
#else
      .security   = WIFI_SECURITY_TYPE_PSK,
      .psk        = CONFIG_WIFI_PSK,
      .psk_length = strlen(CONFIG_WIFI_PSK),
#endif
  };

  LOG_INF("Connecting to SSID: %s\n", sta_config.ssid);
  int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &sta_config, sizeof(sta_config));
  if (ret) {
    LOG_ERR("NET_REQUEST_WIFI_CONNECT failed, err: %d", ret);
  }
}

void net_register(struct in_addr *ap_addr) {
  s_ap_addr = ap_addr;

  net_mgmt_init_event_callback(&s_net_ipv4_cb, net_event_handler, NET_EVENT_IPV4_MASK);
  net_mgmt_add_event_callback(&s_net_ipv4_cb);

  net_mgmt_init_event_callback(&s_net_wifi_cb, net_event_handler, NET_EVENT_WIFI_MASK);
  net_mgmt_add_event_callback(&s_net_wifi_cb);
}

static void net_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
                              struct net_if *iface) {
  const struct wifi_status *status = (const struct wifi_status *)cb->info;
  switch (mgmt_event) {

  case NET_EVENT_WIFI_CONNECT_RESULT: {
    if (status && status->status) {
      LOG_ERR("Failed to connect to %s", CONFIG_WIFI_SSID);
    } else {
      LOG_INF("Connected to %s", CONFIG_WIFI_SSID);
    }
    break;
  }

  case NET_EVENT_WIFI_DISCONNECT_COMPLETE: {
    LOG_INF("Disconnected from %s", CONFIG_WIFI_SSID);
  }

  case NET_EVENT_IPV4_ADDR_ADD: {
    char addr_str[NET_IPV4_ADDR_LEN];

    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
      struct net_if_addr_ipv4 *if_addr = &iface->config.ip.ipv4->unicast[i];

      if (if_addr->ipv4.is_used && if_addr->ipv4.address.in_addr.s_addr != 0) {
        net_addr_ntop(AF_INET, &if_addr->ipv4.address.in_addr, addr_str, sizeof(addr_str));

        LOG_INF("Interface %d got IPv4 %s", net_if_get_by_iface(iface), addr_str);
      }
    }

    if (s_ap_addr) {
      *s_ap_addr = iface->config.ip.ipv4->gw;
    }
    k_event_post(&g_net_events, NET_READY);
    break;
  }

  case NET_EVENT_IPV4_ADDR_DEL: {
    LOG_INF("Interface %d lost its IPv4 address", net_if_get_by_iface(iface));
    k_event_clear(&g_net_events, NET_READY);
    break;
  }
  }
}
