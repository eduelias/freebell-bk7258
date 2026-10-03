/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: microSD storage interface. */
/* sdcard.h — optional microSD storage for the doorbell (CPU0 only).
 *
 * SD host = the BK7258 SDIO host in 1-bit mode on GPIO 2 (CLK) / 3 (CMD) / 4 (DAT0) (the SDK
 * default pin group, which the stock firmware shared), card power on GPIO 6 (stock
 * ty_app_sdcard.c), FatFs volume "1:" (components/fatfs disk_io.c maps the SD card to physical
 * drive 1). Everything that touches the card runs in one low-priority worker task; the ring and
 * motion paths only enqueue a job and never wait.
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

/* True while GPIO `gpio` is the card power pin and a card is present/mounted (cam_power(0) must
 * then leave it high). */
bool sd_holds_gpio(int gpio);

/* Wall-clock seconds since the Unix epoch (UTC), 0 when no time source has been seen yet. */
uint32_t sd_time_now(void);

/* ---- provided by app_main.c ---- */
/* Copy the latest camera JPEG into dst (opens the camera on demand, waits up to wait_ms for a
 * frame). Returns the length, 0 when no frame arrived, <0 on error / no camera in this build. */
int app_cam_grab(uint8_t *dst, uint32_t cap, uint32_t wait_ms);
/* True once the station interface has an IPv4 address. */
bool app_net_up(void);
/* REPLY() for code outside app_main.c: UART + the TCP command reply buffer. */
void app_reply(const char *fmt, ...);
