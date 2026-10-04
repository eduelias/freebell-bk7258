/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: microSD storage interface. */
/* sdcard.h — optional microSD storage for the doorbell (CPU0 only).
 *
 * SD host = the BK7258 SDIO host in 1-bit mode on GPIO 2 (CLK) / 3 (CMD) / 4 (DAT0) (the SDK
 * default pin group, which the stock firmware shared). GPIO 6 = card detect (input, pull-up,
 * 0 = card present; the stock firmware reads it the same way). The slot is powered from the camera
 * supply (GPIO 28 rail) and the SDK init needs the media pipeline up, so a mount opens the camera;
 * the card is mounted only while needed (event -> mount -> save -> `sd keep S` window -> umount).
 * GPIO 13 is the battery ADC only (`sd pwr` default none). FatFs volume "1:" (components/fatfs
 * disk_io.c maps the SD card to physical drive 1). Everything that touches the card runs in one
 * low-priority worker task; the ring and motion paths only enqueue a job and never wait.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Called once from user_app_main() (CPU0): loads the KV settings and starts the worker. */
void sd_init(void);

/* Queue an automatic snapshot for "ring" / "motion". Non-blocking, safe from any task. */
void sd_on_event(const char *event);

/* The `sd` command (see TESTPLAN-sd.md). */
void cli_sd_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv);

/* True while GPIO `gpio` is the card power pin and must not be touched: driven high (card
 * powered) or released less than ~10 s ago (the battery ADC15 node on GPIO 13 needs that long to
 * settle). power.c / app_main.c return the last valid battery reading then. */
bool sd_adc_pin_blocked(int gpio);
/* True while the card power pin is driven high. */
bool sd_card_powered(void);
/* power.c, before deep sleep: unmount + power the card off (blocks up to 4 s); no mounts after. */
void sd_prepare_sleep(void);

/* Wall-clock seconds since the Unix epoch (UTC), 0 when no time source has been seen yet. */
uint32_t sd_time_now(void);
/* Local time of day (SNTP/manual clock + `sd tz`); false while the clock is unsynced (power.c schedule). */
bool app_time_local(int *hh, int *mm);
/* Current `sd keep` / `sd auto` settings (policy.c compares before applying). */
uint32_t sd_keep_get(void);
bool sd_auto_get(void);

/* ---- provided by app_main.c ---- */
/* Make the camera pipeline ready for the card (JPEG camera open; 0 = open, -1 = H.264 held by an
 * RTSP viewer, which is just as good: the card only needs the media pipeline powered). */
int app_sd_cam_ready(void);
bool app_cam_is_open(void);
/* Copy the latest camera JPEG into dst (opens the camera on demand, waits up to wait_ms for a
 * frame). Returns the length, 0 when no frame arrived, <0 on error / no camera in this build. */
int app_cam_grab(uint8_t *dst, uint32_t cap, uint32_t wait_ms);
/* True once the station interface has an IPv4 address. */
bool app_net_up(void);
/* REPLY() for code outside app_main.c: UART + the TCP command reply buffer. */
void app_reply(const char *fmt, ...);
