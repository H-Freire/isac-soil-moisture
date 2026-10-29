// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>
//
// SPDX-License-Identifier: MIT

#include <errno.h>
#include <time.h>

#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/timeutil.h>

#include "relay.h"

#define RTC_SYNC_INTERVAL K_HOURS(24)

LOG_MODULE_DECLARE(relay);

static const struct device *s_rtc_dev = DEVICE_DT_GET(DT_ALIAS(rtc));

static int current_utc(struct tm *utc);
static int rtc_sync_system_time(void);
static void rtc_sync_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(rtc_sync_work, rtc_sync_work_handler);

int rtc_sync(void) {
  int ret = rtc_sync_system_time();

  if (ret < 0) {
    LOG_ERR("Initial RTC synchronization failed: %d", ret);
    return ret;
  }

  return k_work_reschedule(&rtc_sync_work, RTC_SYNC_INTERVAL);
}

uint64_t get_timestamp() {
  struct timespec ts;

  if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) < 0) {
    return 0;
  } else {
    return (uint64_t)ts.tv_sec;
  }
}

int get_current_utc(char *buf, size_t buf_size) {
  int ret;
  struct tm utc;

  // YYYY-MM-DDThh:mm:ss => at least 20 bytes (with null terminator)
  if (!buf || buf_size < 20) {
    return -EINVAL;
  }

  ret = current_utc(&utc);
  if (ret < 0) {
    return ret;
  }

  return snprintf(buf, buf_size, "%04d-%02d-%02dT%02d:%02d:%02d", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
}

static int current_utc(struct tm *utc) {
  struct timespec now;

  if (!utc) {
    return -EINVAL;
  }

  int ret = sys_clock_gettime(SYS_CLOCK_REALTIME, &now);
  if (ret < 0) {
    return ret;
  }

  if (gmtime_r(&now.tv_sec, utc) == NULL) {
    return -EOVERFLOW;
  }

  return 0;
}

static int rtc_sync_system_time(void) {
  int ret;
  time_t epoch;
  struct tm *tm;
  struct timespec ts;
  struct rtc_time rtc_time;

  if (!device_is_ready(s_rtc_dev)) {
    return -ENODEV;
  }

  ret = rtc_get_time(s_rtc_dev, &rtc_time);

  if (ret < 0) {
    return ret;
  }

  /* DS3231 stores years from 2000 through 2099.
   * struct rtc_time.tm_year is years since 1900
   */
  if (rtc_time.tm_year < 100 || rtc_time.tm_year > 199) {
    return -EINVAL;
  }

  tm    = rtc_time_to_tm(&rtc_time);
  epoch = timeutil_timegm(tm);

  if (epoch == (time_t)-1) {
    return -ERANGE;
  }

  ts.tv_sec  = epoch;
  ts.tv_nsec = 0; // DS3231 has one-second time resolution

  return sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
}

static void rtc_sync_work_handler(struct k_work *work) {
  int ret = rtc_sync_system_time();

  if (ret < 0) {
    LOG_WRN("RTC synchronization failed: %d", ret);
  }

  // Schedule the next synchronization
  (void)k_work_reschedule(&rtc_sync_work, RTC_SYNC_INTERVAL);
}
