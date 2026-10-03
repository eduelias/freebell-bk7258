/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: LED arbiter / night light / ring indicator interface. */
/* night.c: LED arbiter, night light, blue-ring indicator patterns, `night` and `sys` commands.
 * CPU0 only (everything here is a no-op / absent on CPU1 and CPU2). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Same as app_main.c's REPLY(): print to UART0 and append to the TCP command reply. Implemented in
 * app_main.c because the reply buffer lives there. */
void db_reply(const char *fmt, ...);

/* Everything night.c needs from app_main.c, as pointers to the existing statics (all stay valid
 * for the life of the firmware; cam_open / fx_running may be NULL when the build has no camera). */
typedef struct {
	const char *fw_version;
	const char *ip;                     /* dotted quad string, "0.0.0.0" while offline */
	const volatile bool *got_ip;        /* Wi-Fi up with an IP */
	const bool *cam_open;               /* camera (GPIO 28) is on: light sensor readable any time */
	const volatile bool *fx_running;    /* ring effects of a press are driving the ring LEDs */
	const uint32_t *dark_mv;            /* light sensor mV above this = dark (KV db_dark_mv) */
	uint32_t (*light_read_mv)(void);    /* may toggle GPIO 28 and take ~100 ms when the camera is closed */
	uint32_t (*adc_read_mv)(uint32_t chan);
} night_env_t;

/* white LED (GPIO 44) sources; the LED is on while any source is set (unless a manual override is active) */
#define NIGHT_SRC_CAM    0x01   /* camera open + dark (night.c) */
#define NIGHT_SRC_MOTION 0x02   /* motion while dark, 20 s (night.c) */
#define NIGHT_SRC_FX     0x04   /* ring effects of a press (app_main.c ringfx_task) */

#define NIGHT_LED_WHITE 0
#define NIGHT_LED_RING  1

void night_init(const night_env_t *env);          /* loads KV settings, starts the night task */
void night_white_set(uint8_t src, bool on);       /* set/clear one white LED source */
void night_on_motion(void);                        /* call from the reporter thread on a motion event */
/* `led white|ring on|off|auto`: state 1/0 = manual override, -1 = back to automatic. */
void night_led_manual(int which, int state);

void night_cli_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv);
void sys_cli_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv);
