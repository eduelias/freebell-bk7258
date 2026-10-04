/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: LED arbiter, night light and blue-ring indicator patterns, `night`/`led`/`sys` commands. */
/* Night light, LED arbiter, blue-ring indicator patterns, `night` and `sys` commands (fw v0.19).
 *
 * - White fill LED (GPIO 44, active high) is owned here: callers set/clear a *source* bit
 *   (camera-dark, motion-dark, press effects) and the LED follows the OR of them, unless
 *   `led white on|off` forces it (`led white auto` hands it back).
 * - Blue button ring (GPIO 45): one short blink at boot, a slow double-blink every 3 s while
 *   Wi-Fi has no IP, off once connected. The press effects (ringfx_task in app_main.c) drive the
 *   pin directly and always win: the pattern tick does not touch the pin while they run and
 *   restores its own state afterwards. `led ring on|off` overrides the pattern, `led ring auto`
 *   restores it; `night ring off` (KV) silences the pattern altogether.
 * - Night light: while the camera is open the light sensor is sampled every 20 s and the white
 *   LED is held on when dark (mV > db_dark_mv), released when light returns (with hysteresis and a
 *   minimum on-time so the LED's own reflection cannot make it flap) or the camera closes.
 *   On motion while dark the white LED is on for 20 s so the snapshot is usable.
 *
 * Settings (KV, all have safe defaults): db_night_auto (1), db_night_ring (1), db_night_hyst (400).
 */
#include <os/os.h>
#include <os/str.h>
#include <os/mem.h>
#include <string.h>
#include <stdio.h>
#include <driver/gpio.h>
#include <components/system.h>
#include "night.h"
#include "applog.h"   /* last: our os_printf lines go to the RAM log ring (policy.c) */

#if (CONFIG_SYS_CPU0)

#define PIN_LED_WHITE 44
#define PIN_LED_RING 45

#define KV_NIGHT_AUTO "db_night_auto"
#define KV_NIGHT_RING "db_night_ring"
#define KV_NIGHT_HYST "db_night_hyst"

#define NIGHT_TICK_MS      50       /* pattern/expiry tick */
#define NIGHT_SAMPLE_MS    20000    /* light sensor period while the camera is open */
#define NIGHT_MOTION_MS    20000    /* white light after motion in the dark */
#define NIGHT_MIN_ON_MS    60000    /* camera night light stays at least this long once on */
#define NIGHT_HYST_DEFAULT 400      /* off again only below dark_mv - hyst (reflection of the LED itself) */
#define RING_BOOT_BLINK_MS 150
#define RING_PERIOD_MS     3000     /* double blink: 100 on, 150 off, 100 on, rest off */

extern bk_err_t gpio_dev_unmap(gpio_id_t gpio_id);
extern int bk_set_env_enhance(const char *key, const void *value, int value_len);
extern int bk_get_env_enhance(const char *key, void *value, int value_len);

static night_env_t s_env;
static beken_mutex_t s_mutex;
static bool s_auto = true, s_ring_en = true;
static uint32_t s_hyst_mv = NIGHT_HYST_DEFAULT;

/* white LED state (under s_mutex) */
static uint8_t s_white_mask;
static int s_white_manual = -1;
static bool s_white_out;
/* ring LED state (under s_mutex) */
static int s_ring_manual = -1;
static bool s_ring_out, s_ring_fx_seen = true;   /* true = pin may have been changed by someone else: rewrite */
static uint32_t s_t0;
/* night-light bookkeeping (night task + reporter thread; simple flags) */
static volatile uint32_t s_motion_until, s_last_mv, s_last_sample_ms, s_cam_on_since;
static volatile bool s_cam_dark, s_prev_cam;
static volatile uint32_t s_samples;

/* ---- helpers ---- */

static void led_write(uint32_t pin, bool on)
{
	gpio_config_t cfg = {
		.io_mode = GPIO_OUTPUT_ENABLE,
		.pull_mode = GPIO_PULL_DISABLE,
		.func_mode = GPIO_SECOND_FUNC_DISABLE,
	};

	gpio_dev_unmap(pin);
	bk_gpio_set_config(pin, &cfg);
	if (on)
		bk_gpio_set_output_high(pin);
	else
		bk_gpio_set_output_low(pin);
}

static int kv_get(const char *key, char *out, int cap)
{
	int n = bk_get_env_enhance(key, out, cap - 1);

	if (n <= 0 || n >= cap) {
		out[0] = 0;
		return 0;
	}
	out[n] = 0;
	return n;
}

static void kv_set(const char *key, const char *val)
{
	bk_set_env_enhance(key, val, os_strlen(val));
}

static bool flag(const volatile bool *p)
{
	return p && *p;
}

static bool cflag(const bool *p)
{
	return p && *p;
}

static uint32_t dark_mv(void)
{
	return s_env.dark_mv ? *s_env.dark_mv : 1400;
}

/* call with s_mutex held */
static void white_apply(void)
{
	bool want = s_white_manual >= 0 ? s_white_manual != 0 : s_white_mask != 0;

	if (want != s_white_out) {
		s_white_out = want;
		led_write(PIN_LED_WHITE, want);
	}
}

/* ring pattern value at time t (ms since the task started) */
static bool ring_pattern(uint32_t t)
{
	uint32_t ph;

	if (!s_ring_en)
		return false;
	if (t < RING_BOOT_BLINK_MS)              /* one short blink: the firmware is up */
		return true;
	if (flag(s_env.got_ip))
		return false;
	ph = t % RING_PERIOD_MS;                 /* no IP: slow double blink */
	return ph < 100 || (ph >= 250 && ph < 350);
}

/* call with s_mutex held; the press effects own the pin while they run */
static void ring_apply(uint32_t t)
{
	bool want;

	if (flag(s_env.fx_running)) {
		s_ring_fx_seen = true;
		return;
	}
	want = s_ring_manual >= 0 ? s_ring_manual != 0 : ring_pattern(t);
	if (want != s_ring_out || s_ring_fx_seen) {
		s_ring_out = want;
		s_ring_fx_seen = false;
		led_write(PIN_LED_RING, want);
	}
}

/* ---- public API ---- */

void night_white_set(uint8_t src, bool on)
{
	if (!s_mutex)
		return;
	rtos_lock_mutex(&s_mutex);
	if (on)
		s_white_mask |= src;
	else
		s_white_mask &= ~src;
	white_apply();
	rtos_unlock_mutex(&s_mutex);
}

void night_led_manual(int which, int state)
{
	if (!s_mutex)
		return;
	rtos_lock_mutex(&s_mutex);
	if (which == NIGHT_LED_WHITE) {
		s_white_manual = state < 0 ? -1 : (state != 0);
		white_apply();
	} else {
		s_ring_manual = state < 0 ? -1 : (state != 0);
		ring_apply(rtos_get_time() - s_t0);
	}
	rtos_unlock_mutex(&s_mutex);
}

/* Reporter thread, on a motion event (before the webhook POST, which reads the light too). */
void night_on_motion(void)
{
	uint32_t now = rtos_get_time(), mv;

	if (!s_mutex || !s_auto || !s_env.light_read_mv)
		return;
	if (s_motion_until) {   /* already lit for a previous motion: just extend, a reading now would see our own light */
		s_motion_until = now + NIGHT_MOTION_MS;
		return;
	}
	mv = s_env.light_read_mv();
	s_last_mv = mv;
	if (mv > dark_mv()) {
		s_motion_until = now + NIGHT_MOTION_MS;
		night_white_set(NIGHT_SRC_MOTION, true);
		os_printf("[night] motion in the dark (%u mV): white light %u s\r\n", mv, NIGHT_MOTION_MS / 1000);
	}
}

/* ---- the task: 50 ms tick for the ring pattern, motion expiry and the camera night light ---- */

static void night_task(beken_thread_arg_t arg)
{
	s_t0 = rtos_get_time();
	for (;;) {
		uint32_t now = rtos_get_time();
		bool cam = cflag(s_env.cam_open);

		/* motion light expiry */
		if (s_motion_until && (int32_t)(now - s_motion_until) >= 0) {
			s_motion_until = 0;
			night_white_set(NIGHT_SRC_MOTION, false);
		}
		/* camera open/close edges */
		if (cam != s_prev_cam) {
			s_prev_cam = cam;
			if (cam) {
				s_last_sample_ms = now - NIGHT_SAMPLE_MS;   /* sample right away */
			} else if (s_cam_dark) {
				s_cam_dark = false;
				night_white_set(NIGHT_SRC_CAM, false);
				os_printf("[night] camera closed: white light off\r\n");
			}
		}
		if (!s_auto && s_cam_dark) {
			s_cam_dark = false;
			night_white_set(NIGHT_SRC_CAM, false);
		}
		/* light sampling while streaming: GPIO 28 is already high, so light_read_mv() is a plain ADC read */
		if (cam && s_auto && s_env.light_read_mv && now - s_last_sample_ms >= NIGHT_SAMPLE_MS) {
			uint32_t mv = s_env.light_read_mv(), thr = dark_mv();

			s_last_sample_ms = now;
			s_last_mv = mv;
			s_samples++;
			if (!s_cam_dark && mv > thr) {
				s_cam_dark = true;
				s_cam_on_since = now;
				night_white_set(NIGHT_SRC_CAM, true);
				os_printf("[night] dark while streaming (%u mV > %u): white light on\r\n", mv, thr);
			} else if (s_cam_dark && mv + s_hyst_mv < thr && now - s_cam_on_since >= NIGHT_MIN_ON_MS) {
				s_cam_dark = false;
				night_white_set(NIGHT_SRC_CAM, false);
				os_printf("[night] light is back (%u mV < %u): white light off\r\n", mv, thr - s_hyst_mv);
			}
		}
		/* ring indicator */
		rtos_lock_mutex(&s_mutex);
		ring_apply(now - s_t0);
		rtos_unlock_mutex(&s_mutex);

		rtos_delay_milliseconds(NIGHT_TICK_MS);
	}
}

bool night_auto_get(void)
{
	return s_auto;
}

void night_init(const night_env_t *env)
{
	beken_thread_t t;
	char v[16];

	s_env = *env;
	if (kv_get(KV_NIGHT_AUTO, v, sizeof(v)))
		s_auto = v[0] != '0';
	if (kv_get(KV_NIGHT_RING, v, sizeof(v)))
		s_ring_en = v[0] != '0';
	if (kv_get(KV_NIGHT_HYST, v, sizeof(v)))
		s_hyst_mv = os_strtoul(v, NULL, 10);
	rtos_init_mutex(&s_mutex);
	led_write(PIN_LED_WHITE, false);
	led_write(PIN_LED_RING, false);
	if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "night", night_task, 3072, NULL) != BK_OK)
		os_printf("[night] thread failed\r\n");
	os_printf("[night] auto %d ring %d hyst %u mV\r\n", s_auto, s_ring_en, s_hyst_mv);
}

/* ---- commands ---- */

void night_cli_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	char buf[16];

	if (argc >= 3 && os_strcmp(argv[1], "auto") == 0) {
		s_auto = os_strcmp(argv[2], "on") == 0;
		kv_set(KV_NIGHT_AUTO, s_auto ? "1" : "0");
		if (!s_auto && s_motion_until) {
			s_motion_until = 0;
			night_white_set(NIGHT_SRC_MOTION, false);
		}
		db_reply("[night] auto %s\r\n", s_auto ? "on" : "off");
	} else if (argc >= 3 && os_strcmp(argv[1], "ring") == 0) {
		s_ring_en = os_strcmp(argv[2], "on") == 0;
		kv_set(KV_NIGHT_RING, s_ring_en ? "1" : "0");
		db_reply("[night] ring indicator %s\r\n", s_ring_en ? "on" : "off");
	} else if (argc >= 3 && os_strcmp(argv[1], "hyst") == 0) {
		s_hyst_mv = os_strtoul(argv[2], NULL, 10);
		snprintf(buf, sizeof(buf), "%u", s_hyst_mv);
		kv_set(KV_NIGHT_HYST, buf);
		db_reply("[night] hysteresis %u mV (off below %u)\r\n", s_hyst_mv, dark_mv() > s_hyst_mv ? dark_mv() - s_hyst_mv : 0);
	} else if (argc >= 2 && os_strcmp(argv[1], "status") == 0) {
		uint32_t now = rtos_get_time();
		uint32_t mv = s_env.light_read_mv ? s_env.light_read_mv() : 0;
		uint32_t left = s_motion_until && (int32_t)(s_motion_until - now) > 0 ? (s_motion_until - now + 999) / 1000 : 0;

		db_reply("[night] auto %d ring %d hyst %u | light %u mV (last %u, dark > %u) cam %d cam_dark %d samples %u motion_left %u s"
			 " | white src 0x%x manual %d out %d | ring manual %d out %d wifi %d fx %d\r\n",
			 s_auto, s_ring_en, s_hyst_mv, mv, s_last_mv, dark_mv(), cflag(s_env.cam_open), s_cam_dark, s_samples, left,
			 s_white_mask, s_white_manual, s_white_out, s_ring_manual, s_ring_out, flag(s_env.got_ip), flag(s_env.fx_running));
	} else {
		db_reply("night auto on|off | ring on|off | hyst MV | status   (auto %d ring %d hyst %u)\r\n", s_auto, s_ring_en, s_hyst_mv);
	}
}

static const char *reset_reason_str(uint32_t r)
{
	switch (r) {
	case RESET_SOURCE_POWERON: return "power-on";
	case RESET_SOURCE_REBOOT: return "reboot";
	case RESET_SOURCE_WATCHDOG: return "watchdog";
	case RESET_SOURCE_NMI_WDT: return "nmi-wdt";
	case RESET_SOURCE_OTA_REBOOT: return "ota";
	case RESET_SOURCE_DEEPPS_GPIO: return "deepsleep-gpio";
	case RESET_SOURCE_DEEPPS_RTC: return "deepsleep-rtc";
	case RESET_SOURCE_SUPER_DEEP: return "super-deep";
	case RESET_SOURCE_UNKNOWN: return "unknown";
	default: return r >= RESET_SOURCE_CRASH_ILLEGAL_JUMP && r <= RESET_SOURCE_DEFAULT_EXCEPTION ? "crash" : "other";
	}
}

void sys_cli_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	if (argc >= 2 && os_strcmp(argv[1], "info") == 0) {
		uint32_t rr = bk_misc_get_reset_reason();
		uint32_t up = rtos_get_time() / 1000;
		uint32_t light = s_env.light_read_mv ? s_env.light_read_mv() : 0;
		uint32_t batt = s_env.adc_read_mv ? s_env.adc_read_mv(9) : 0;

		db_reply("[sys] fw %s uptime %us (%uh%02um) ip %s wifi %d heap %u B reset 0x%x (%s) light %u mV batt %u mV cam %d\r\n",
			 s_env.fw_version ? s_env.fw_version : "?", up, up / 3600, (up / 60) % 60, s_env.ip ? s_env.ip : "?",
			 flag(s_env.got_ip), (unsigned)rtos_get_free_heap_size(), rr, reset_reason_str(rr), light, batt, cflag(s_env.cam_open));
	} else {
		db_reply("sys info\r\n");
	}
}

#endif /* CONFIG_SYS_CPU0 */
