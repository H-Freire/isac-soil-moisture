// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#define WIFI_PRIORITY    3
#define PROBE_PRIORITY   3
#define WIFI_STACK_SIZE  1024
#define PROBE_STACK_SIZE 1024

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/sys/atomic.h>

#include <esp_err.h>
#include <esp_wifi.h>

#include <wifi_sensing.h>

#include "relay.h"

#define WIFI_RX_PRIORITY   3
#define WIFI_TX_PRIORITY   3
#define WIFI_RX_STACK_SIZE 1024
#define WIFI_TX_STACK_SIZE 1024

BUILD_ASSERT(sizeof(CONFIG_WIFI_SSID) > 1,
             "CONFIG_WIFI_SSID is empty. Please set it in conf file.");

BUILD_ASSERT(sizeof(CONFIG_WIFI_SSID) <= 32,
             "CONFIG_WIFI_SSID is too long (> 32). Please shorten it in conf file.");

BUILD_ASSERT(sizeof(CONFIG_WIFI_PSK) > 1, "CONFIG_WIFI_PSK is empty. Please set it in conf file.");

LOG_MODULE_DECLARE(relay);

static struct net_if *s_iface;

static void wifi_rx_thread(void *p1, void *p2, void *p3);
static void wifi_tx_thread(void *p1, void *p2, void *p3);

K_THREAD_DEFINE(wifi_rx_tid, WIFI_RX_STACK_SIZE, wifi_rx_thread, NULL, NULL, NULL, WIFI_RX_PRIORITY,
                0, 0);
K_THREAD_DEFINE(wifi_tx_tid, WIFI_TX_STACK_SIZE, wifi_tx_thread, NULL, NULL, NULL, WIFI_TX_PRIORITY,
                0, 0);

void ap_init(void) {
  s_iface = net_if_get_wifi_sap();

  /*wifi_tx_rate_config_t phy_config = {
    .phymode = WIFI_PHY_MODE_11G,
    .rate    = WIFI_PHY_RATE_6M,
  };*/

  if (esp_wifi_config_80211_tx_rate(WIFI_IF_AP, WIFI_PHY_RATE_6M) != ESP_OK) {
    LOG_ERR("Failed to set phy rate for probing frames");
    return;
  }

  struct wifi_connect_req_params ap_config = {
      .ssid        = CONFIG_WIFI_SSID,
      .ssid_length = strlen(CONFIG_WIFI_SSID),
      .psk         = CONFIG_WIFI_PSK,
      .psk_length  = strlen(CONFIG_WIFI_PSK),
      .channel     = CONFIG_WIFI_CHANNEL,
      .security    = WIFI_SECURITY_TYPE_PSK,
  };
  int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, s_iface, &ap_config, sizeof(ap_config));
  if (ret) {
    LOG_ERR("NET_REQUEST_WIFI_AP_ENABLE failed, err: %d", ret);
    return;
  }

  struct in_addr pool_start;
  net_addr_pton(AF_INET, "192.168.5.2", &pool_start);

  if (net_dhcpv4_server_start(s_iface, &pool_start) < 0) {
    LOG_ERR("Failed to start DHCP server");
  } else {
    LOG_INF("DHCP server started successfully");
  }
}

static void wifi_rx_thread(void *p1 __unused, void *p2 __unused, void *p3 __unused) {
  int sock;

  struct sockaddr_in bind_addr = {
      .sin_family      = AF_INET,
      .sin_port        = htons(CONFIG_UDP_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };

  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    LOG_ERR("Failed to create UDP socket");
    return;
  }

  if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    LOG_ERR("Failed to bind UDP socket");
    return;
  }

  LOG_INF("UDP server listening on port %d", CONFIG_UDP_PORT);

  while (true) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Allocate shared memory slab for received CSI data
    csi_item_t *item;
    k_mem_slab_alloc(&g_csi_slab, (void **)&item, K_FOREVER);

    ssize_t len = recvfrom(sock, &item->data, sizeof(item->data), 0,
                           (struct sockaddr *)&client_addr, &client_addr_len);

    if (len < 0) {
      LOG_ERR("LoRa send failed (%zd)", len);
      continue;
    }

    const uint16_t id = ((msg_t *)item->data)->hdr.id;
    if (id != MSG_ID_CSI && id != MSG_ID_SENSORS) {
      LOG_WRN("Unexpected message type");
      continue;
    }

    // Send shared allocated memory to both storage and lora threads (reference counting)
    atomic_set(&item->refs, 2);

    k_fifo_put(&g_storage_fifo, item);
    k_fifo_put(&g_lora_fifo, item);
  }
}

static void wifi_tx_thread(void *p1 __unused, void *p2 __unused, void *p3 __unused) {
  static uint8_t msg[sizeof(wifi_action_frame_t) + MSG_HDR_SIZE + PROBE_DATA_SIZE];

  k_event_wait(&g_ap_events, NET_READY, false, K_FOREVER);
  const struct net_linkaddr *link_addr = net_if_get_link_addr(s_iface);

  wifi_action_frame_t *const frame = (wifi_action_frame_t *)&msg;

  *frame = (wifi_action_frame_t){
      .frame_ctrl.subtype = ACTION_FRAME_SUBTYPE, // Action frame
      .category           = 127,                  // Vendor Specific
      .action             = 1,                    // Arbitrary Action Detail
  };
  memcpy(frame->da, g_broadcast_addr, MAC_ADDR_LEN);
  memcpy(frame->sa, link_addr->addr, MAC_ADDR_LEN);
  memcpy(frame->bss, link_addr->addr, MAC_ADDR_LEN);

  msg_hdr_t *const hdr = (msg_hdr_t *)frame->body;
  hdr->id              = MSG_ID_PROBE;

  probe_data_t *const probe = (probe_data_t *)&((msg_t *)frame->body)->payload.probe;

  int64_t next_wake_time = k_uptime_get();
  while (true) {
    for (size_t i = 0; i < SENSING_WINDOW_SIZE; i++) {
      next_wake_time += SENSING_PERIOD_MSEC;

      esp_err_t ret = esp_wifi_80211_tx(WIFI_IF_AP, msg, sizeof(msg), true);

      if (ret != ESP_OK) {
        LOG_ERR("Failed to send probe frame: %d", ret);
      } else {
        LOG_INF("Probe frame sent");
      }

      // Advance sequence for next measerument request
      probe->seq++;
      k_sleep(K_TIMEOUT_ABS_MS(next_wake_time));
    }

    k_sleep(K_MSEC(1000 * SENSING_INTERVAL_SEC - SENSING_WINDOW_SIZE * SENSING_PERIOD_MSEC));
  }
}
