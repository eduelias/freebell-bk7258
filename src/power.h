/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: battery / charger / deep sleep interface. */
/* Battery, charger and deep sleep for the BK7258 doorbell (feat/power).
 *
 * Everything that can put the device to sleep is OFF by default and only enabled through
 * `pwr sleep on` (KV-persisted). After every boot the device stays fully awake for at least
 * POWER_BOOT_HOLD_MS (Wi-Fi, TCP channel, HTTP) before the first automatic sleep.
 *
 * fw 0.25: the RTC wake interval follows a day/night schedule (`pwr sched`, local time from the
 * SNTP clock + `sd tz`, light sensor as the fallback) with battery derating; `pwr sched off`
 * restores the fixed `snap_min`. `pwr policy` (policy.c) pulls settings from Home Assistant.
 *
 * Only compiled for CPU0 (the file is listed for every core; the body is guarded). */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define POWER_BOOT_HOLD_MS     (120 * 1000)   /* no automatic sleep before this uptime */
#define POWER_SLEEPNOW_GRACE_MS (10 * 1000)   /* `pwr sleepnow`: let the TCP reply out first */

/* ---- implemented in power.c ---- */
void power_init(void);                  /* load KV, configure the status pins, decode the wake reason, start the task */
void power_activity(void);              /* any user-visible activity (command, HTTP client, press, motion): restarts the idle timer; ISR-safe */
int  power_json(char *buf, int cap);    /* appends ',"batt_mv":N,"batt_pct":N,"usb":0|1,"charging":0|1,"sleep_min":N,"wake_pir":0|1' for the report JSON */
bool power_take_wake_event(char *reason, int cap);   /* true once after a deep-sleep wake: reason = button|pir|usb|uart|rtc|gpioNN */
bool power_wake_pending(void);          /* a "wake" report is still waiting for Wi-Fi */
uint32_t power_batt_mv(void);           /* battery voltage (median of 3 ADC readings x divider ratio) */
bool power_hold9(void);                 /* keep the GPIO 9 sensor rail high through deep sleep (`pwr hold9`, fw 0.26.1; default off since 0.26.2) */
void cli_pwr_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv);
/* The next RTC wake interval in minutes and why (day/night by clock or light sensor, fixed
 * snap_min, battery derating). Reads the battery and, without a synced clock, the light sensor
 * (~100 ms, toggles GPIO 28 when the camera is closed). Also refreshes the low-battery flag that
 * forces the PIR wake off. */
uint32_t power_wake_interval(char *why, int cap);
void power_sched_params(bool *on, int *day_start_min, int *day_end_min, uint32_t *day_min, uint32_t *night_min);
bool power_wake_pir_get(void);          /* the `pwr wake_pir` setting (the <15 % derating may still force it off) */

/* ---- small hooks implemented in app_main.c (keep merges with other features trivial) ---- */
int  db_kv_get(const char *key, char *out, int cap);
void db_kv_set(const char *key, const char *val);
uint32_t db_adc_mv(uint32_t chan);
int  db_pin_cfg(uint32_t pin, int io_mode, int pull_mode);   /* gpio_io_mode_t / gpio_pull_mode_t */
void db_reply(const char *fmt, ...);    /* REPLY(): UART + the TCP command reply buffer */
bool app_power_busy(void);              /* camera/audio session, HTTP client, OTA, pending report */
bool app_power_got_ip(void);
void app_power_prepare_sleep(void);     /* amp off, LEDs off, camera closed, sensor rails low */
void app_power_wake_input(uint32_t pin);/* the pin that woke us from deep sleep (53 = ring, 24 = motion) */
void app_wifi_reconnect(void);          /* after a failed sleep attempt */
uint32_t app_power_pir_sens(void);      /* PIR sensitivity bits (pir.c), held on GPIO 25/26 through the sleep */
bool app_light_is_dark(void);           /* light sensor above the `ring dark` threshold */
