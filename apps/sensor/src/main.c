// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: CC-BY-SA-4.0

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

#include <esp_err.h>
#include <esp_wifi.h>

#include "wifi_sensing.h"

#define UDP_PRIORITY      4
#define SENSOR_PRIORITY   5
#define UDP_STACK_SIZE    2048
#define SENSOR_STACK_SIZE 2048

#define QUEUE_SIZE 30
#define MSG_COUNT  (SENSING_WINDOW_SIZE / MAX_MSG_PAYLOAD)

#define SENSOR_SAMPLES    1
#define ADC_NODE          DT_ALIAS(adc0)
#define RF_SWITCH_NODE    DT_NODELABEL(rf_switch)
#define ADC_CHANNEL_COUNT ARRAY_SIZE(s_adc_cfgs)

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(ADC_NODE), "ADC controller disabled");
BUILD_ASSERT(SENSOR_SAMPLES >= 1, "Invalid count of sensor samples");

enum ant_type {
  INT_ANT,
  EXT_ANT,
};

typedef struct {
  unsigned : 32; /**< reserved */
  unsigned : 32; /**< reserved */
  unsigned : 32; /**< reserved */
  unsigned : 32; /**< reserved */
  unsigned : 32; /**< reserved */
#if CONFIG_IDF_TARGET_ESP32S2
  unsigned : 32; /**< reserved */
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C5 ||       \
    CONFIG_IDF_TARGET_ESP32C6
  unsigned : 16; /**< reserved */
  unsigned fft_gain : 8;
  unsigned agc_gain : 8;
  unsigned : 32; /**< reserved */
#endif
  unsigned : 32; /**< reserved */
#if CONFIG_IDF_TARGET_ESP32S2
  signed : 8;    /**< reserved */
  unsigned : 24; /**< reserved */
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C5
  unsigned : 32; /**< reserved */
  unsigned : 32; /**< reserved */
  unsigned : 32; /**< reserved */
#endif
  unsigned : 32; /**< reserved */
} __attribute__((packed)) wifi_rx_ctrl_phy_t;

LOG_MODULE_REGISTER(sensor);

static void udp_client_thread(void *, void *, void *);
static void sensors_read_thread(void *, void *, void *);

// clang-format off
static const struct device *s_adc = DEVICE_DT_GET(ADC_NODE);
static const struct gpio_dt_spec s_ant_sel = GPIO_DT_SPEC_GET(RF_SWITCH_NODE, select_gpios);
static const struct adc_channel_cfg s_adc_cfgs[] = {
    DT_FOREACH_CHILD_SEP(ADC_NODE, ADC_CHANNEL_CFG_DT, (,))
};
static const struct adc_sequence_options opts = {
  .interval_us     = 0,
  .extra_samplings = SENSOR_SAMPLES - 1,
};
// clang-format on

static struct in_addr s_ap_addr;
static uint8_t s_mac_addr[MAC_ADDR_LEN];

static uint16_t adc_reading[SENSOR_SAMPLES][ADC_CHANNEL_COUNT];
static struct adc_sequence s_adc_sequence = {
    .buffer      = adc_reading,
    .buffer_size = sizeof(adc_reading),
    .calibrate   = true,
    .resolution  = 12,
    .options     = &opts,
};

K_TIMER_DEFINE(s_sensing_timer, NULL, NULL);
K_MSGQ_DEFINE(s_sensor_msgq, SENSOR_DATA_SIZE, QUEUE_SIZE, 4);

K_THREAD_DEFINE(udp_client_id, UDP_STACK_SIZE, udp_client_thread, NULL, NULL, NULL, UDP_PRIORITY, 0,
                0);
K_THREAD_DEFINE(sensors_read_id, SENSOR_STACK_SIZE, sensors_read_thread, NULL, NULL, NULL,
                SENSOR_PRIORITY, 0, 0);

void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info) {
  sensor_data_t frame;

  if (!info || !info->buf) {
    LOG_WRN("<invalid arg> wifi_csi_cb. info: %p %" PRIu16 " %" PRIu16, info->buf, info->len,
            info->rx_seq);
    return;
  }
  const wifi_pkt_rx_ctrl_t *rx_ctrl       = &info->rx_ctrl;
  const wifi_action_frame_t *action_frame = (wifi_action_frame_t *)info->hdr;

  if (action_frame->frame_ctrl.subtype != ACTION_FRAME_SUBTYPE) {
    LOG_INF("Non action frame, discarding...");
    return;
  }

  if (!IS_BROADCAST_ADDR(action_frame->da)) {
    LOG_INF("Frame from neighboring node, discarding...");
    return;
  }

  if (((probe_msg_t *)&action_frame->body)->magic != MSG_ID_PROBE) {
    LOG_INF("Miscellaneous action frame, discarding...");
    return;
  }

  memcpy(frame.csi, info->buf, info->len);
  frame.rssi = rx_ctrl->rssi;
  frame.agc  = ((wifi_rx_ctrl_phy_t *)info)->agc_gain;
  frame.seq  = ((probe_msg_t *)&action_frame->body)->seq;

  LOG_INF("Received CSI frame with size %" PRIu16 ". RSSI: %d dBm. gain: %d, seq: %u", info->len,
          frame.rssi, frame.agc, frame.seq);

  if (!k_msgq_put(&s_sensor_msgq, &frame, K_NO_WAIT)) {
    LOG_INF("CSI frame collected [%d dBm]", frame.rssi);
  } else {
    LOG_ERR("Failed to send CSI frame");
  }
}

static inline void enable_antenna(enum ant_type antenna) {
  if (!gpio_is_ready_dt(&s_ant_sel)) {
    LOG_ERR("Antenna select GPIO not ready");
    return;
  }

  gpio_pin_set_dt(&s_ant_sel, antenna);
}

static void udp_client_thread(void *, void *, void *) {
  int sock;

  static uint8_t msg_buf[MSG_COUNT][SENSOR_MSG_SIZE + MAX_MSG_PAYLOAD * SENSOR_DATA_SIZE];

  k_event_wait(&g_net_events, NET_READY, false, K_FOREVER);

  struct sockaddr_in server_addr = {
      .sin_family = AF_INET,
      .sin_port   = htons(CONFIG_UDP_PORT),
      .sin_addr   = s_ap_addr,
  };

  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    LOG_ERR("Failed to create UDP socket: %d", errno);
    return;
  }

  if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    LOG_ERR("Failed to connect UDP socket: %d", errno);
    return;
  }

  {
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &s_ap_addr, ip_str, sizeof(ip_str));
    LOG_INF("UDP socket registered to %s:%d", ip_str, CONFIG_UDP_PORT);
  }

  esp_wifi_set_csi(true);

  while (true) {
    uint8_t count = 0;
    for (size_t tx = 0; tx < MSG_COUNT; tx++) {
      sensor_msg_t *const msg = (sensor_msg_t *)msg_buf[tx];

      for (size_t i = 0; i < MAX_MSG_PAYLOAD; i++) {
        // Reception timed out (given the sensing window size)
        if (k_msgq_get(&s_sensor_msgq, &msg->payload[i],
                       K_SECONDS(SENSING_WINDOW_SIZE / SENSING_RATE_HZ))) {
          count += i > 0;
          goto transmit;
        }
        memcpy(msg->payload[i].mac, s_mac_addr, MAC_ADDR_LEN);
        msg->count++;
      }
      count++;
    }
  transmit:
    esp_wifi_set_csi(false);
    enable_antenna(INT_ANT);

    // Start timer with a margin of one sensing window
    k_timer_start(&s_sensing_timer,
                  K_SECONDS(SENSING_INTERVAL_SEC - (SENSING_WINDOW_SIZE / SENSING_RATE_HZ + 1)),
                  K_NO_WAIT);

    for (size_t i = 0; i < count; i++) {
      k_event_wait(&g_net_events, NET_READY, false, K_FOREVER);

      int ret = send(sock, msg_buf[i],
                     SENSOR_MSG_SIZE + ((sensor_msg_t *)msg_buf[i])->count * SENSOR_DATA_SIZE, 0);

      if (ret < 0) {
        LOG_ERR("Failed to send UDP packet: %d", errno);
      } else {
        LOG_INF("Sent %d bytes to AP", ret);
      }
    }

    // Switch back to external antenna right before next sensing window
    k_timer_status_sync(&s_sensing_timer);

    enable_antenna(EXT_ANT);
    esp_wifi_set_csi(true);
  }
}

static void sensors_read_thread(void *, void *, void *) {
  while (true) {
    int err = adc_read(s_adc, &s_adc_sequence);
    if (err < 0) {
      LOG_ERR("Could not read sensors: %d", err);
    }

    /* TODO:
     * - Take mean of digital reading and convert to raw millivolts
     * - Trigger this thread once per SENSING_WINDOW
     * - Send sensor data
     */
  }
}

static void adc_init(void) {
  if (!device_is_ready(s_adc)) {
    LOG_ERR("ADC device controller is not ready\n");
  }

  for (size_t i = 0; i < ADC_CHANNEL_COUNT; i++) {
    int err = adc_channel_setup(s_adc, &s_adc_cfgs[i]);

    if (err < 0) {
      LOG_ERR("Could not setup channel #%d (%d)\n", s_adc_cfgs[i].channel_id, err);
    } else {
      s_adc_sequence.channels |= BIT(s_adc_cfgs[i].channel_id);
    }
  }
}

static void wifi_csi_init(void) {
  const wifi_promiscuous_filter_t filter = {
      .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT,
  };

  wifi_csi_config_t csi_config = {
      .enable             = true,
      .acquire_csi_legacy = true,
  };

  if (esp_wifi_set_promiscuous(true)) {
    LOG_ERR("Failed to enable promiscuous mode");
    return;
  }

  if (esp_wifi_set_promiscuous_filter(&filter)) {
    LOG_ERR("Failed to set promiscuous filter");
    return;
  }

  if (esp_wifi_set_csi_config(&csi_config)) {
    LOG_ERR("Failed to set CSI config");
    return;
  }

  if (esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, NULL)) {
    LOG_ERR("Failed to register CSI callback");
    return;
  }

  esp_wifi_set_csi(false);
  LOG_INF("CSI init complete...");
}

int main(void) {
  LOG_INF("Starting Wi-Fi sensor");

  // Enable environmental sensors and integrated (communication) antenna
  adc_init();
  gpio_pin_set_dt(&s_ant_sel, INT_ANT);

  net_register(&s_ap_addr);

  wifi_csi_init();
  ap_connect(s_mac_addr);

  return 0;
}
