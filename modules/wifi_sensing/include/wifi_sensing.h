// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>
#include <zephyr/net/socket.h>

#define MAC_ADDR_LEN 6
#define MACSTR       "%02x-%02x-%02x-%02x-%02x-%02x"

#define ACTION_FRAME_SUBTYPE 0b1101

#define NET_READY 0x01

#define MAX_DEVICES      4
#define MAX_MSG_PAYLOAD  10
#define SENSOR_MSG_SIZE  sizeof(sensor_msg_t)
#define SENSOR_DATA_SIZE sizeof(sensor_data_t)
#define PROBE_MSG_SIZE   sizeof(probe_msg_t)

#define SENSING_RATE_HZ      10
#define SENSING_WINDOW_SIZE  100
#define SENSING_INTERVAL_SEC (10 * 60)
#define SENSING_PERIOD_MSEC  (1000 / SENSING_RATE_HZ)
#define SENSING_WINDOW_SEC   (SENSING_WINDOW_SIZE / SENSING_RATE_HZ)

#if (MAX_MSG_PAYLOAD > 10) || (MAX_MSG_PAYLOAD < 1)
#error "MAX_MSG_PAYLOAD must be in the [1, 10] interval"
#elif (SENSING_WINDOW_SIZE % MAX_MSG_PAYLOAD)
#error "SENSING_WINDOW_SIZE must be a multiple of MAX_MSG_PAYLOAD"
#endif

#define MAX_MSGS (MAX_DEVICES * SENSING_WINDOW_SIZE)

#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]

#define IS_BROADCAST_ADDR(addr) (memcmp(addr, g_broadcast_addr, MAC_ADDR_LEN) == 0)

enum {
  MSG_ID_CSI     = 0xDEAD,
  MSG_ID_PROBE   = 0xC0DE,
  MSG_ID_SENSORS = 0xBEEF,
};

typedef uint16_t msg_id_t;

typedef struct {
  uint32_t seq;
  msg_id_t magic;
} __attribute__((packed)) probe_msg_t;

typedef struct {
  uint32_t seq;
  uint8_t mac[MAC_ADDR_LEN];

  uint8_t agc;
  int8_t rssi;
  int8_t csi[128];
} __attribute__((packed)) csi_data_t;

typedef struct {
  uint16_t temp;
  uint16_t moisture;

  // Whether the data collection happened because of a timeout
  bool timeout;
} __attribute__((packed)) sensor_data_t;

typedef struct {
  msg_id_t id;
  uint8_t count;

  csi_data_t payload[];
} __attribute__((packed)) sensor_msg_t;

typedef struct {
  /** Frame control fields */
  struct {
    unsigned : 2;
    unsigned type : 2;
    unsigned subtype : 4;
    unsigned to_ds : 1;
    unsigned from_ds : 1;
    unsigned : 1;
    unsigned retry : 1;
    unsigned pwr_mgmt : 1;
    unsigned : 3;
  } __attribute__((packed)) frame_ctrl;

  uint16_t duration_id;

  /** Device addressing and sequencing */
  uint8_t da[6];
  uint8_t sa[6];
  uint8_t bss[6];
  uint16_t seq_ctrl;

  /** Vendor-specific action header */
  uint8_t category;
  uint8_t action;

  /** Action frame payload */
  uint8_t body[];
} __attribute__((packed)) wifi_action_frame_t;

extern const uint8_t g_broadcast_addr[MAC_ADDR_LEN];

extern struct k_event g_net_events;

void ap_connect(uint8_t *mac_addr);
void net_register(struct in_addr *ap_addr);
