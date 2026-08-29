// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>

#include <ff.h>

#include <wifi_sensing.h>

#include "relay.h"

#define STORAGE_PRIORITY   5
#define STORAGE_STACK_SIZE 1024

#define DISK_DRIVE_NAME "SD"
#define DISK_MOUNT_PT   "/" DISK_DRIVE_NAME ":"

typedef struct {
  bool is_open;
  uint32_t key;
  struct fs_file_t file;
} file_node_t;

LOG_MODULE_DECLARE(relay);

static FATFS s_fat_fs;
static struct fs_mount_t s_mp = {
    .type      = FS_FATFS,
    .fs_data   = &s_fat_fs,
    .mnt_point = DISK_MOUNT_PT,
};

static void storage_thread(void *p1, void *p2, void *p3);

K_THREAD_DEFINE(storage_tid, STORAGE_STACK_SIZE, storage_thread, NULL, NULL, NULL, STORAGE_PRIORITY,
                0, 0);

void sd_card_init(void) {
  static const char *disk_pdrv = DISK_DRIVE_NAME;

  if (disk_access_ioctl(disk_pdrv, DISK_IOCTL_CTRL_INIT, NULL)) {
    LOG_ERR("Failed to init " DISK_DRIVE_NAME);
    return;
  }

  if (fs_mount(&s_mp)) {
    LOG_ERR(DISK_DRIVE_NAME " mount failed");
    return;
  }

  LOG_INF(DISK_DRIVE_NAME " mounted successfully");
}

static void storage_thread(void *p1 __unused, void *p2 __unused, void *p3 __unused) {
  csi_item_t *pkt;
  // Lower half corresponds to CSI files, while the upper half corresponds to reference sensors
  file_node_t open_files[2 * MAX_DEVICES] = {};

  while (true) {
    void *write_data;
    size_t write_size, start_idx, end_idx;
    const uint8_t (*mac_addr)[MAC_ADDR_LEN];

    pkt = k_fifo_get(&g_storage_fifo, K_FOREVER);

    const msg_t *const msg = (const msg_t *)pkt->data;
    switch (msg->hdr.id) {
    case MSG_ID_CSI: {
      start_idx  = 0;
      end_idx    = MAX_DEVICES;
      write_data = (void *)msg->payload.csi.data;
      write_size = msg->payload.csi.count * CSI_DATA_SIZE;
      mac_addr   = &msg->payload.csi.data[0].mac;
      break;
    }
    case MSG_ID_SENSORS: {
      start_idx  = MAX_DEVICES;
      end_idx    = 2 * MAX_DEVICES;
      write_data = (void *)&msg->payload.sensor;
      write_size = SENSOR_DATA_SIZE;
      mac_addr   = &msg->payload.sensor.mac;
      break;
    }
    default: {
      LOG_WRN("Unexpected message type");
      continue;
    }
    }

    uint32_t dev_key =
        (*mac_addr[2] << 24) | (*mac_addr[3] << 16) | (*mac_addr[4] << 8) | *mac_addr[5];

    int8_t free_idx   = -1;
    int8_t target_idx = -1;

    /* Search for an open file */
    for (size_t i = start_idx; i < end_idx; i++) {
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
      char file_path[40];

      /*
       * Filename for MAC address 01:23:45:67:89
       *
       * CSI    file: 01-23-45-67-89_csi.bin
       * Sensor file: 01-23-45-67-89_sensor.bin
       *
       */
      snprintf(file_path, sizeof(file_path),
               DISK_MOUNT_PT "/" MACSTR "%s"
                             ".bin",
               MAC2STR(*mac_addr), free_idx < MAX_DEVICES ? "_csi" : "_sensor");

      fs_file_t_init(&open_files[free_idx].file);

      if (fs_open(&open_files[free_idx].file, file_path, FS_O_CREATE | FS_O_APPEND | FS_O_WRITE) ==
          0) {
        target_idx                   = free_idx;
        open_files[free_idx].key     = dev_key;
        open_files[free_idx].is_open = true;

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

    fs_write(&open_files[target_idx].file, write_data, write_size);

    if (atomic_dec(&pkt->refs) == 1) {
      k_mem_slab_free(&g_csi_slab, (void *)pkt);
    }

    fs_sync(&open_files[target_idx].file);
  }
}
