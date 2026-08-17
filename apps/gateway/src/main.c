// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/atomic.h>

#include <esp_err.h>
#include <esp_wifi.h>
#include <ff.h>

#include "lora.h"
#include "wifi_sensing.h"

// TODO: Possibly increase stack sizes
#define WIFI_PRIORITY      3
#define LORA_PRIORITY      4
#define PROBE_PRIORITY     3
#define STORAGE_PRIORITY   5
#define WIFI_STACK_SIZE    1024
#define LORA_STACK_SIZE    1024
#define PROBE_STACK_SIZE   1024
#define STORAGE_STACK_SIZE 1024

#define DISK_DRIVE_NAME "SD"
#define DISK_MOUNT_PT   "/" DISK_DRIVE_NAME ":"

#define LORA_NODE DT_ALIAS(lora0)

BUILD_ASSERT(sizeof(CONFIG_WIFI_SSID) > 1,
             "CONFIG_WIFI_SSID is empty. Please set it in conf file.");

BUILD_ASSERT(sizeof(CONFIG_WIFI_SSID) <= 32,
             "CONFIG_WIFI_SSID is too long (> 32). Please shorten it in conf file.");

BUILD_ASSERT(sizeof(CONFIG_WIFI_PSK) > 1, "CONFIG_WIFI_PSK is empty. Please set it in conf file.");

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LORA_NODE), "No default LoRa radio specified in DT");

typedef struct {
  bool is_open;
  uint32_t key;
  struct fs_file_t file;
} file_node_t;

typedef struct {
  void *fifo_reserved;
  atomic_t refs;
  uint8_t data[SENSOR_MSG_SIZE + MAX_MSG_PAYLOAD * SENSOR_DATA_SIZE];
} csi_item_t;

LOG_MODULE_REGISTER(gateway);

static void wifi_thread(void *, void *, void *);
static void lora_thread(void *, void *, void *);
static void probe_thread(void *, void *, void *);
static void storage_thread(void *, void *, void *);

static const struct device *s_lora_dev = DEVICE_DT_GET(LORA_NODE);

static struct net_if *s_iface;
static struct net_mgmt_event_callback s_net_cb;

static FATFS fat_fs;
static struct fs_mount_t mp = {
    .type      = FS_FATFS,
    .fs_data   = &fat_fs,
    .mnt_point = DISK_MOUNT_PT,
};

K_EVENT_DEFINE(net_events);

K_FIFO_DEFINE(lora_fifo);
K_FIFO_DEFINE(storage_fifo);
K_MEM_SLAB_DEFINE_STATIC_TYPE(s_csi_slab, csi_item_t, MAX_MSGS / MAX_MSG_PAYLOAD);

K_THREAD_DEFINE(wifi_thread_id, WIFI_STACK_SIZE, wifi_thread, NULL, NULL, NULL, WIFI_PRIORITY, 0,
                0);
K_THREAD_DEFINE(lora_thread_id, LORA_STACK_SIZE, lora_thread, NULL, NULL, NULL, LORA_PRIORITY, 0,
                0);
K_THREAD_DEFINE(probe_thread_id, PROBE_STACK_SIZE, probe_thread, NULL, NULL, NULL, PROBE_PRIORITY,
                0, 0);
K_THREAD_DEFINE(storage_thread_id, STORAGE_STACK_SIZE, storage_thread, NULL, NULL, NULL,
                STORAGE_PRIORITY, 0, 0);

static void wifi_thread(void *, void *, void *) {
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
    csi_item_t *pkt;
    k_mem_slab_alloc(&s_csi_slab, (void **)&pkt, K_FOREVER);

    ssize_t len = recvfrom(sock, &pkt->data, sizeof(pkt->data), 0, (struct sockaddr *)&client_addr,
                           &client_addr_len);

    if (len < 0) {
      LOG_ERR("LoRa send failed (%zd)", len);
      continue;
    }

    if ((len - 1) % sizeof(sensor_data_t)) {
      LOG_ERR("Unexpected message size");
      continue;
    }

    // Send shared allocated memory to both storage and lora threads (reference counting)
    atomic_set(&pkt->refs, 2);

    k_fifo_put(&storage_fifo, pkt);
    k_fifo_put(&lora_fifo, pkt);
  }
}

static void lora_thread(void *, void *, void *) {
  // lora_ack_t ack;
  // struct k_timer ack_timer;
  csi_item_t *pkt;

  lora_data_t data = {
      .magic = LORA_MAGIC,
      .id    = LORA_DATA_ID,
  };

  // k_timer_init(&ack_timer, NULL, NULL);

  while (true) {
    pkt = (csi_item_t *)k_fifo_get(&lora_fifo, K_FOREVER);

    const sensor_msg_t *const msg = (sensor_msg_t *)pkt->data;
    for (size_t i = 0; i < msg->count; i++) {
      memcpy((uint8_t *)&data.payload, (uint8_t *)&msg->payload[i], SENSOR_DATA_SIZE);

      int ret = lora_send(s_lora_dev, (uint8_t *)&data, LORA_DATA_SIZE);
      if (ret < 0) {
        LOG_ERR("LoRa send failed");
      }
    }

    if (atomic_dec(&pkt->refs) == 1) {
      k_mem_slab_free(&s_csi_slab, (void *)pkt);
    }

    /* TODO: ACK request */

    /*
    k_timer_start(&ack_timer, K_SECONDS(2), K_FOREVER);

    int8_t snr;
    int16_t rssi;
    k_timeout_t timeout;
    while ((timeout.ticks = k_timer_remaining_ticks(&ack_timer)) > 0) {
      int ret = lora_recv(s_lora_dev, (uint8_t *)&ack, sizeof(ack), timeout, &rssi, &snr);
      if (ret < 0) {
        LOG_ERR("LoRa recv failed (%d)", ret);
        continue;
      }

      if (ack.magic != LORA_MAGIC || ack.id != LORA_ACK_ID) {
        LOG_INF("LoRa message not intended for device");
        continue;
      }

      // TODO: Evaluate what to resend (bitmap)
      break;
    }
    */
  }
}

static void probe_thread(void *, void *, void *) {
  static uint8_t msg[sizeof(wifi_action_frame_t) + PROBE_MSG_SIZE];

  k_event_wait(&net_events, NET_READY, false, K_FOREVER);
  const struct net_linkaddr *link_addr = net_if_get_link_addr(s_iface);

  wifi_action_frame_t *const frame = (wifi_action_frame_t *)&msg;
  probe_msg_t *const probe         = (probe_msg_t *)&frame->body;

  *frame = (wifi_action_frame_t){
      .frame_ctrl.subtype = ACTION_FRAME_SUBTYPE, // Action frame
      .category           = 127,                  // Vendor Specific
      .action             = 1,                    // Arbitrary Action Detail
  };
  memcpy(frame->da, g_broadcast_addr, MAC_ADDR_LEN);
  memcpy(frame->sa, link_addr->addr, MAC_ADDR_LEN);
  memcpy(frame->bss, link_addr->addr, MAC_ADDR_LEN);

  *probe = (probe_msg_t){
      .magic = MSG_ID_PROBE,
  };

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

static void storage_thread(void *, void *, void *) {
  csi_item_t *pkt;
  file_node_t open_files[MAX_DEVICES] = {};

  while (true) {
    pkt = k_fifo_get(&storage_fifo, K_FOREVER);

    const sensor_msg_t *const sensor_msg          = (const sensor_msg_t *)pkt->data;
    const uint8_t (*const mac_addr)[MAC_ADDR_LEN] = &sensor_msg->payload[0].mac;

    uint32_t dev_key =
        (*mac_addr[2] << 24) | (*mac_addr[3] << 16) | (*mac_addr[4] << 8) | *mac_addr[5];

    int8_t free_idx   = -1;
    int8_t target_idx = -1;

    /* Search for an open file */
    for (size_t i = 0; i < MAX_DEVICES; i++) {
      if (open_files[i].is_open && open_files[i].key == dev_key) {
        target_idx = i;
        break;
      }
      if (!open_files[i].is_open && free_idx == -1) {
        free_idx = i;
      }
    }

    /* Create new file (none open for the given device) */
    if (target_idx == -1 && free_idx != -1) {
      char file_path[32];
      snprintf(file_path, sizeof(file_path), DISK_MOUNT_PT "/" MACSTR ".bin", MAC2STR(*mac_addr));

      fs_file_t_init(&open_files[free_idx].file);

      if (fs_open(&open_files[free_idx].file, file_path, FS_O_CREATE | FS_O_APPEND | FS_O_WRITE) ==
          0) {
        open_files[free_idx].is_open = true;
        open_files[free_idx].key     = dev_key;
        target_idx                   = free_idx;

        LOG_INF("Opened new file stream for " MACSTR " at index %d", MAC2STR(*mac_addr),
                target_idx);
      } else {
        LOG_ERR("Failed to open file: %s", file_path);
        continue;
      }
    } else {
      LOG_ERR("Max devices reached. Cannot open new file");
      continue;
    }

    fs_write(&open_files[target_idx].file, sensor_msg->payload,
             sensor_msg->count * SENSOR_DATA_SIZE);

    if (atomic_dec(&pkt->refs) == 1) {
      k_mem_slab_free(&s_csi_slab, (void *)pkt);
    }

    fs_sync(&open_files[target_idx].file);
  }
}

static void net_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
                              struct net_if *iface) {
  switch (mgmt_event) {
  case NET_EVENT_WIFI_AP_ENABLE_RESULT: {
    k_event_post(&net_events, NET_READY);
    LOG_INF("AP Mode is enabled. Waiting for station to connect");
    break;
  }
  }
}

static void sd_init(void) {
  static const char *disk_pdrv = DISK_DRIVE_NAME;

  if (disk_access_ioctl(disk_pdrv, DISK_IOCTL_CTRL_INIT, NULL)) {
    LOG_ERR("Failed to init " DISK_DRIVE_NAME);
    return;
  }

  if (fs_mount(&mp)) {
    LOG_ERR(DISK_DRIVE_NAME " mount failed");
    return;
  }

  LOG_INF(DISK_DRIVE_NAME " mounted successfully");
}

static void ap_init(void) {
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

int main(void) {
  LOG_INF("Starting LoRa <-> Wi-Fi gateway");

  net_mgmt_init_event_callback(&s_net_cb, net_event_handler, NET_EVENT_WIFI_AP_ENABLE_RESULT);
  net_mgmt_add_event_callback(&s_net_cb);

  sd_init();
  ap_init();
  lora_init(s_lora_dev, true);

  return 0;
}
