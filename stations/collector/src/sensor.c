// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <math.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/posix/sys/socket.h>

#include <wifi_sensing.h>

#include "collector.h"

#define SENSOR_PRIORITY   5
#define SENSOR_STACK_SIZE 2048

#define SENSOR_SAMPLES       10
#define SENSOR_INTERVAL_MSEC 1000

#define CHANNEL_VREF(node_id) DT_PROP_OR(node_id, zephyr_vref_mv, 3300)

BUILD_ASSERT(SENSOR_SAMPLES >= 1, "Invalid count of sensor samples");

LOG_MODULE_DECLARE(collector);

// clang-format off
static const uint32_t s_vrefs[] = {
    DT_FOREACH_CHILD_SEP(ADC_NODE, CHANNEL_VREF, (,))
};
// clang-format on

struct adc_sequence g_adc_sequence = {
    .buffer_size = sizeof(uint16_t),
    .resolution  = ADC_RES,
};

static void sensor_thread(void *, void *, void *);

K_THREAD_DEFINE(sensor_tid, SENSOR_STACK_SIZE, sensor_thread, NULL, NULL, NULL, SENSOR_PRIORITY, 0,
                0);

static void sensor_thread(void *p1 __unused, void *p2 __unused, void *p3 __unused) {
  msg_t msg = {
      .hdr.id = MSG_ID_SENSORS,
  };
  sensor_data_t *pkt = &msg.payload.sensor;

  uint16_t adc_reading[SENSOR_SAMPLES];

  k_event_wait(&g_net_events, NET_READY, false, K_FOREVER);
  memcpy(pkt->mac, g_mac_addr, sizeof(pkt->mac));

  int sock = udp_socket_open(g_ap_addr);

  int ret;
  while (true) {
    // Wait for up to 1.5x sensing windows before sensing independently and flag packet on timeout
    pkt->timeout = (k_sem_take(&g_sensing_sem, K_SECONDS(3 * SENSING_INTERVAL_SEC / 2)) != 0);

    uint32_t std  = 0;
    uint32_t mean = 0;
    for (size_t i = 0; i < SENSOR_SAMPLES; i++) {
      g_adc_sequence.buffer = &adc_reading[i];

      ret = adc_read(g_adc, &g_adc_sequence);
      if (ret < 0) {
        LOG_ERR("Could not read sensor: %d", ret);
      }
      uint16_t value = adc_reading[i];

      LOG_DBG("Sensor digital reading %d: %d/%d", i, value, 2 << g_adc_sequence.resolution);

      mean += value;
      std += value * value;

      k_sleep(K_MSEC(SENSOR_INTERVAL_MSEC));
    }

    /*
     * mean = E(x)
     * std² = E{[x - E(x)]²} => E(x²) - E(x)²
     *
     * std_mean = std / sqrt(N)
     */
    std /= SENSOR_SAMPLES;
    mean /= SENSOR_SAMPLES;

    std = (uint32_t)sqrtf(((float)std - mean * mean) / (SENSOR_SAMPLES - 1));

    size_t count           = 0;
    uint32_t filtered_mean = 0;
    for (size_t i = 0; i < SENSOR_SAMPLES; i++) {
      uint16_t value = adc_reading[i];

      if (value >= mean - std && value <= mean + std) {
        filtered_mean += value;
        count++;
      }
    }

    if (count) {
      filtered_mean /= count;
    }

    ret = adc_raw_to_millivolts(s_vrefs[0], g_adc_cfgs[0].gain, g_adc_sequence.resolution,
                                &filtered_mean);
    if (ret < 0) {
      LOG_ERR("Could not convert ADC reading to millivolts: %d", ret);
    }

    pkt->moisture = (uint16_t)filtered_mean;

    ret = send(sock, &msg, MSG_HDR_SIZE + SENSOR_DATA_SIZE, 0);
    if (ret < 0) {
      LOG_ERR("Failed to send sensor UDP packet: %d", errno);
    } else {
      LOG_INF("Sent %d bytes to AP", ret);
    }

    // Ignore any possible signals given while executing thread
    k_sem_reset(&g_sensing_sem);
  }
}
