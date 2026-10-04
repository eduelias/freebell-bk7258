/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: battery and charger readout, deep sleep policy and wake sources. */
/* Battery, charger and deep sleep (feat/power). See power.h and TESTPLAN-power.md.
 *
 * Hardware facts (stock tuya_battery.c / tuya_pir.c disassembly, 2026-10-03, verified where noted):
 *  - GPIO 19 = USB power present, input, floating, high = USB plugged ("usb power gpio").
 *  - GPIO 15 = charger status, input with pull-up; stock: charging = (GPIO15 == 1) && (GPIO19 == 1).
 *    Reads low today with the pack at ~4.02 V on USB (= charge complete), consistent.
 *  - Battery sense = tuya ADC channel 15 = GPIO 13 (ADC15), battery mV = 2 x ADC mV (1:1 divider;
 *    the stock doubles the reading and accepts 2501..4299 mV). Measured 2012 mV -> 4024 mV. ADC9
 *    (GPIO 7, ~2990 mV) is a regulated rail, not the battery.
 *  - GPIO 50 = PIR enable, GPIO 25/26 = PIR sensitivity: the stock drives all three HIGH at init.
 *  - Stock SoC table (discharging): 4000->100 3900->80 3800->60 3700->40 3650->20 3600->10 3550->0 %,
 *    low-battery shutdown at 3540 mV. We use a finer, slightly more conservative curve below.
 *
 * Deep sleep (SDK pm v2): bk_pm_sleep_mode_set(PM_MODE_DEEP_SLEEP) arms it; the idle task enters it
 * once the BTSP/WIFIP_MAC/AUDP/VIDP power domains are voted off. Wake = SoC reset: the SDK records
 * RESET_SOURCE_DEEPPS_GPIO (+ bk_gpio_get_wakeup_gpio_id()) or RESET_SOURCE_DEEPPS_RTC. Every GPIO
 * of the BK7258 sits in the AON domain and can be a wake source (dynamic map, max 4). */
#include <common/bk_include.h>   /* sdkconfig.h: CONFIG_SYS_CPU0 must be visible before the guard below */
#include "power.h"

#if (CONFIG_SYS_CPU0)
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <os/os.h>
#include <os/str.h>
#include <components/system.h>
#include <driver/gpio.h>
#include <driver/aon_rtc.h>
#include <modules/pm.h>
#include <modules/wifi.h>
#include "sdcard.h"   /* sd_adc_pin_blocked(): only with `sd pwr 13` set (not the default) is the ADC node driven; app_time_local() */
#include "policy.h"   /* `pwr policy ...` (policy.c) */
#include "applog.h"   /* last: our os_printf lines go to the RAM log ring */

#define PIN_USB_PRESENT 19
#define PIN_CHARGING    15
#define PIN_PIR_ENABLE  50
#define PIN_PIR_SENS0   25   /* pir.c: 25 = sens bit 0, 26 = bit 1 */
#define PIN_PIR_SENS1   26
#define PIN_PWR_BUTTON  53
#define PIN_PWR_PIR     24
#define PIN_PWR_AMP     46
#define PIN_UART_RX     10   /* in the SDK's static wake map (falling edge): console input wakes the device too */

#define KV_PWR_SLEEP  "db_pwr_sleep"    /* "1" = automatic sleep enabled (default off) */
#define KV_PWR_FORCE  "db_pwr_force"    /* "1" = sleep even while USB is present (default off) */
#define KV_PWR_IDLE   "db_pwr_idle"     /* idle seconds before sleeping (default 60, min 20) */
#define KV_PWR_WPIR   "db_pwr_wpir"     /* "1" = PIR (GPIO 24) is a wake source (default off) */
#define KV_PWR_SNAP   "db_pwr_snap"     /* RTC wake every N minutes for a snapshot (default 0 = keepalive only) */
#define KV_PWR_BCHAN  "db_pwr_bchan"    /* battery ADC channel (default 15) */
#define KV_PWR_BRATIO "db_pwr_bratio"   /* divider ratio in percent (default 200 = x2.00) */
#define KV_PWR_HOLD9  "db_pwr_hold9"    /* fw 0.26.1: keep the GPIO 9 sensor rail up through deep sleep. Default OFF since 0.26.2: the chimes were keyed by the boot pin map on GPIO 53 (SDK patch 0010), not by this rail, and the stock never drives 9 in sleep */
#define KV_PWR_SCHED  "db_pwr_sched"    /* fw 0.25: "HH:MM HH:MM DAY_MIN NIGHT_MIN" or "off" (default 07:00 22:00 15 60) */

#define POWER_KEEPALIVE_MIN   360       /* RTC wake when snap_min is 0, so a sleeping device still reports twice a day */
#define POWER_IDLE_MIN_S      20
#define POWER_SNAP_MAX_MIN    1440
#define POWER_FAIL_BACKOFF_MS (10 * 60 * 1000)
#define POWER_TASK_PERIOD_MS  1000

static bool s_sleep_en, s_force, s_wake_pir, s_inhibit;
static bool s_hold9;   /* default off (0.26.2); `pwr hold9 on` keeps GPIO 9 high through the sleep */
bool power_hold9(void) { return s_hold9; }
static uint32_t s_idle_s = 60, s_snap_min, s_bchan = 15, s_bratio = 200;
/* smart schedule (fw 0.25): day/night RTC intervals, local time from the SNTP clock + `sd tz`,
 * light sensor when unsynced; `pwr sched off` = the fixed snap_min */
static bool s_sched_on = true;
static int s_day_start = 7 * 60, s_day_end = 22 * 60;   /* minutes after local midnight */
static uint32_t s_day_min = 15, s_night_min = 60;
static volatile bool s_low_batt;                         /* < 15 %: PIR wake forced off (set by power_wake_interval) */
#define POWER_LOW_BATT_PCT     15
#define POWER_LOW_BATT_MIN     180
#define POWER_DERATE_PCT       30
static volatile uint32_t s_last_activity_ms;
static volatile uint32_t s_sleep_at_ms;      /* pending `pwr sleepnow` deadline, 0 = none */
static uint32_t s_fail_until_ms;
static uint32_t s_boot_reason;
static char s_wake_reason[12];
static volatile bool s_wake_pending;
static volatile bool s_sleeping;             /* the sleep sequence is running */
static const char *s_inhibit_why = "";

/* ---- KV helpers ---- */
static void kv_set_u32(const char *key, uint32_t v)
{
	char b[12];
	snprintf(b, sizeof(b), "%u", v);
	db_kv_set(key, b);
}

static bool kv_get_u32(const char *key, uint32_t *out)
{
	char b[16];
	if (!db_kv_get(key, b, sizeof(b)))
		return false;
	*out = os_strtoul(b, NULL, 10);
	return true;
}

static bool kv_get_bool(const char *key, bool dflt)
{
	char b[8];
	if (!db_kv_get(key, b, sizeof(b)))
		return dflt;
	return b[0] == '1';
}

/* ---- battery / charger ---- */
static bool usb_present(void)
{
	return bk_gpio_get_input(PIN_USB_PRESENT) != 0;
}

static bool charging(void)
{
	return usb_present() && bk_gpio_get_input(PIN_CHARGING) != 0;
}

/* The battery ADC15 node is GPIO 13. With an `sd pwr 13` setting (not the default: the pin turned
 * out not to power the card, sdcard.c) the node is driven while the card is in use and needs ~10 s
 * to settle after release; the last valid median is returned then and `pwr status` shows "(cached)". */
static uint32_t s_batt_last_adc;
static bool s_batt_cached;
static uint32_t s_batt_read_ms;      /* tick of the last real burst: one reading per 2 s (below) */
#define BATT_REUSE_MS 2000

/* One median per 2 s. The divider cannot recharge the SARADC's sample capacitor between back-to-back
 * bursts: a second burst right after the first reads ~160 mV lower on the cell (`pwr chan 15` showed
 * "1903 mV x2.00 = 3486 mV", and 0.25.0's first build derated the schedule to 2 % from that), so every
 * caller inside one status line / report / sleep decision shares the same reading. */
static uint32_t batt_adc_mv(void)
{
	uint32_t a, b, c, t, now = rtos_get_time();

	if (s_bchan == 15 && sd_adc_pin_blocked(13)) {
		s_batt_cached = true;
		return s_batt_last_adc;
	}
	if (s_batt_last_adc && s_batt_read_ms && now - s_batt_read_ms < BATT_REUSE_MS)
		return s_batt_last_adc;
	a = db_adc_mv(s_bchan);
	b = db_adc_mv(s_bchan);
	c = db_adc_mv(s_bchan);
	/* median of three */
	if (a > b) { t = a; a = b; b = t; }
	if (b > c) { t = b; b = c; c = t; }
	if (a > b) { t = a; a = b; b = t; }
	if (b) {
		s_batt_last_adc = b;
		s_batt_read_ms = now ? now : 1;
	}
	s_batt_cached = false;
	return b;
}

uint32_t power_batt_mv(void)
{
	return batt_adc_mv() * s_bratio / 100;
}

/* Conservative single-cell Li-ion curve (resting voltage, light load). While charging the terminal
 * voltage reads ~80 mV above the resting value, so it is corrected before the lookup. */
static uint32_t batt_pct(uint32_t mv, bool chg)
{
	static const uint16_t tbl[][2] = {
		{4150, 100}, {4080, 95}, {4000, 88}, {3920, 78}, {3850, 66}, {3790, 55}, {3740, 45},
		{3700, 35}, {3660, 25}, {3620, 15}, {3570, 8}, {3500, 3}, {3400, 0},
	};
	const int n = sizeof(tbl) / sizeof(tbl[0]);
	int i;

	if (chg)
		mv = mv > 80 ? mv - 80 : 0;
	if (mv >= tbl[0][0])
		return 100;
	for (i = 1; i < n; i++) {
		if (mv >= tbl[i][0]) {
			uint32_t hi_mv = tbl[i - 1][0], lo_mv = tbl[i][0], hi_p = tbl[i - 1][1], lo_p = tbl[i][1];
			return lo_p + (hi_p - lo_p) * (mv - lo_mv) / (hi_mv - lo_mv);
		}
	}
	return 0;
}

int power_json(char *buf, int cap)
{
	bool chg = charging();
	uint32_t mv = power_batt_mv();
	uint32_t next = power_wake_interval(NULL, 0);

	return snprintf(buf, cap, ",\"batt_mv\":%u,\"batt_pct\":%u,\"usb\":%u,\"charging\":%u,\"sleep_min\":%u,\"wake_pir\":%u",
			mv, batt_pct(mv, chg), usb_present() ? 1 : 0, chg ? 1 : 0, next, (s_wake_pir && !s_low_batt) ? 1 : 0);
}

/* ---- smart RTC schedule (fw 0.25) ---- */
static bool sched_is_day(int t)
{
	if (s_day_start == s_day_end)
		return true;
	if (s_day_start < s_day_end)
		return t >= s_day_start && t < s_day_end;
	return t >= s_day_start || t < s_day_end;   /* a day window across midnight */
}

uint32_t power_wake_interval(char *why, int cap)
{
	char w[64], d[40] = "";
	uint32_t min, mv = power_batt_mv(), pct = batt_pct(mv, charging());
	int hh, mm;

	if (!s_sched_on) {
		min = s_snap_min ? s_snap_min : POWER_KEEPALIVE_MIN;
		snprintf(w, sizeof(w), "fixed snap_min%s", s_snap_min ? "" : " (keepalive)");
	} else if (app_time_local(&hh, &mm)) {
		bool day = sched_is_day(hh * 60 + mm);
		min = day ? s_day_min : s_night_min;
		snprintf(w, sizeof(w), "%s by clock %02d:%02d", day ? "day" : "night", hh, mm);
	} else {
		bool dark = app_light_is_dark();
		min = dark ? s_night_min : s_day_min;
		snprintf(w, sizeof(w), "%s by light (no clock)", dark ? "night" : "day");
	}
	s_low_batt = pct < POWER_LOW_BATT_PCT;
	if (s_low_batt) {
		if (min < POWER_LOW_BATT_MIN)
			min = POWER_LOW_BATT_MIN;
		snprintf(d, sizeof(d), ", batt %u%%<%u: %u min, pir wake off", pct, POWER_LOW_BATT_PCT, POWER_LOW_BATT_MIN);
	} else if (pct < POWER_DERATE_PCT) {
		min *= 2;
		snprintf(d, sizeof(d), ", batt %u%%<%u: x2", pct, POWER_DERATE_PCT);
	}
	if (min < 1)
		min = 1;
	if (min > POWER_SNAP_MAX_MIN)
		min = POWER_SNAP_MAX_MIN;
	if (why && cap > 0)
		snprintf(why, cap, "%s%s", w, d);
	return min;
}

void power_sched_params(bool *on, int *day_start_min, int *day_end_min, uint32_t *day_min, uint32_t *night_min)
{
	if (on) *on = s_sched_on;
	if (day_start_min) *day_start_min = s_day_start;
	if (day_end_min) *day_end_min = s_day_end;
	if (day_min) *day_min = s_day_min;
	if (night_min) *night_min = s_night_min;
}

bool power_wake_pir_get(void)
{
	return s_wake_pir;
}

static bool parse_hhmm(const char *str, int *out)
{
	char *e;
	uint32_t h = os_strtoul(str, &e, 10), m;

	if (e == str || *e != ':')
		return false;
	m = os_strtoul(e + 1, &e, 10);
	if (*e || h > 23 || m > 59)
		return false;
	*out = (int)(h * 60 + m);
	return true;
}

/* "HH:MM HH:MM DAY NIGHT" (sched on) or "off"; false = unparsable (settings untouched) */
static bool sched_parse(const char *str)
{
	char buf[40], *tok[4];
	int n = 0, st, en;
	uint32_t dm, nm;

	if (os_strcmp(str, "off") == 0) {
		s_sched_on = false;
		return true;
	}
	os_strlcpy(buf, str, sizeof(buf));
	for (char *t = strtok(buf, " "); t && n < 4; t = strtok(NULL, " "))
		tok[n++] = t;
	if (n != 4 || !parse_hhmm(tok[0], &st) || !parse_hhmm(tok[1], &en))
		return false;
	dm = os_strtoul(tok[2], NULL, 10);
	nm = os_strtoul(tok[3], NULL, 10);
	if (dm < 1 || dm > POWER_SNAP_MAX_MIN || nm < 1 || nm > POWER_SNAP_MAX_MIN)
		return false;
	s_sched_on = true;
	s_day_start = st;
	s_day_end = en;
	s_day_min = dm;
	s_night_min = nm;
	return true;
}

static void sched_str(char *buf, int cap)
{
	if (!s_sched_on)
		snprintf(buf, cap, "off (fixed snap_min %u)", s_snap_min);
	else
		snprintf(buf, cap, "%02d:%02d-%02d:%02d day %u night %u", s_day_start / 60, s_day_start % 60,
			 s_day_end / 60, s_day_end % 60, s_day_min, s_night_min);
}

/* ---- activity / wake bookkeeping ---- */
void power_activity(void)
{
	s_last_activity_ms = rtos_get_time();
}

bool power_take_wake_event(char *reason, int cap)
{
	if (!s_wake_pending)
		return false;
	s_wake_pending = false;
	os_strlcpy(reason, s_wake_reason, cap);
	return true;
}

bool power_wake_pending(void)
{
	return s_wake_pending;
}

static const char *reset_reason_str(uint32_t r)
{
	switch (r) {
	case RESET_SOURCE_POWERON: return "power on";
	case RESET_SOURCE_REBOOT: return "reboot";
	case RESET_SOURCE_WATCHDOG: return "watchdog";
	case RESET_SOURCE_DEEPPS_GPIO: return "deepsleep gpio";
	case RESET_SOURCE_DEEPPS_RTC: return "deepsleep rtc";
	case RESET_SOURCE_DEEPPS_USB: return "deepsleep usb";
	case RESET_SOURCE_NMI_WDT: return "nmi watchdog";
	case RESET_SOURCE_HARD_FAULT: return "hard fault";
	default: return "other";
	}
}

static bool reset_reason_is_crash(uint32_t r)
{
	return r == RESET_SOURCE_WATCHDOG || r == RESET_SOURCE_HARD_FAULT || r == RESET_SOURCE_MPU_FAULT
	       || r == RESET_SOURCE_BUS_FAULT || (r >= RESET_SOURCE_CRASH_ILLEGAL_JUMP && r <= RESET_SOURCE_CRASH_ASSERT);
}

/* ---- the sleep sequence ---- */
static void rtc_alarm_cb(aon_rtc_id_t id, uint8_t *name_p, void *param)
{
	/* Only reached if the deep sleep did not happen (otherwise the SoC resets): nothing to do, the
	 * failure path below has already cleaned up. ISR context: no printing. */
}

static bk_err_t arm_rtc_wake(uint32_t minutes)
{
	alarm_info_t alarm = {
		.name = "pwrwake",
		.period_tick = (rtc_tick_t)((double)minutes * 60000.0 * (double)AON_RTC_MS_TICK_CNT),   /* ~32.768 ticks per ms */
		.period_cnt = 1,
		.callback = rtc_alarm_cb,
		.param_p = NULL,
	};

	bk_alarm_unregister(AON_RTC_ID_1, (uint8_t *)"pwrwake");
	return bk_alarm_register(AON_RTC_ID_1, &alarm);
}

static void keep_pin_in_sleep(uint32_t pin, bool high)
{
	gpio_config_t cfg = {
		.io_mode = GPIO_OUTPUT_ENABLE,
		.pull_mode = high ? GPIO_PULL_UP_EN : GPIO_PULL_DOWN_EN,   /* the SDK encodes the held level in the pull field */
		.func_mode = GPIO_SECOND_FUNC_DISABLE,
	};
	bk_gpio_register_lowpower_keep_status(pin, &cfg);
}

static const char *module_name(pm_power_module_name_e m)
{
	switch (m) {
	case PM_POWER_MODULE_NAME_BTSP: return "bt";
	case PM_POWER_MODULE_NAME_WIFIP_MAC: return "mac";
	case PM_POWER_MODULE_NAME_AUDP: return "audio";
	case PM_POWER_MODULE_NAME_VIDP: return "video";
	default: return "?";
	}
}

/* Returns only if the deep sleep did not happen. */
static void power_do_sleep(const char *why)
{
	const pm_power_module_name_e need_off[] = {PM_POWER_MODULE_NAME_BTSP, PM_POWER_MODULE_NAME_WIFIP_MAC,
						     PM_POWER_MODULE_NAME_AUDP, PM_POWER_MODULE_NAME_VIDP};
	char swhy[96];
	uint32_t minutes = power_wake_interval(swhy, sizeof(swhy)), i, t;
	bool wake_pir = s_wake_pir && !s_low_batt;   /* < 15 %: the PIR wake is forced off (reported in pwr status + the JSON) */
	bk_err_t err;

	s_sleeping = true;
	os_printf("[pwr] sleeping (%s): wake on button%s%s, rtc in %u min (%s), batt %u mV\r\n", why,
		  wake_pir ? " + pir" : "", ", usb plug, uart rx", minutes, swhy, power_batt_mv());

	/* 1. peripherals off: amp, microSD (sd_prepare_sleep), LEDs, camera, sensor rails (app side), then Wi-Fi */
	app_power_prepare_sleep();
	bk_wifi_sta_stop();
	rtos_delay_milliseconds(500);

	/* 2. pins held through the sleep: amp enable low (floating leaks audio + current), PIR enabled */
	keep_pin_in_sleep(PIN_PWR_AMP, false);
	if (s_hold9)
		keep_pin_in_sleep(9, true);   /* optional (off by default): sensor rail stays up through the sleep; a 0.26.1 experiment, see docs/notes/power.md */
	/* USB present (GPIO 19) is a floating input while awake; pull it DOWN through the sleep so a
	 * charger's rising edge is a clean wake (0.25.0; UNVERIFIED until a charger is at hand: the
	 * pin's polarity itself is still unconfirmed, see docs/notes/power.md). power_init() restores
	 * pull-disable after the wake reset. */
	{
		gpio_config_t usb_cfg = {
			.io_mode = GPIO_INPUT_ENABLE,
			.pull_mode = GPIO_PULL_DOWN_EN,
			.func_mode = GPIO_SECOND_FUNC_DISABLE,
		};
		db_pin_cfg(PIN_USB_PRESENT, GPIO_INPUT_ENABLE, GPIO_PULL_DOWN_EN);
		bk_gpio_register_lowpower_keep_status(PIN_USB_PRESENT, &usb_cfg);
	}
	if (wake_pir) {
		uint32_t sens = app_power_pir_sens();
		keep_pin_in_sleep(PIN_PIR_ENABLE, true);
		keep_pin_in_sleep(PIN_PIR_SENS0, (sens & 1) != 0);   /* the stock keeps the sensitivity bits driven; floating = undefined sensitivity */
		keep_pin_in_sleep(PIN_PIR_SENS1, (sens & 2) != 0);
	}

	/* 3. wake sources: GPIO (button falling, USB plug rising, PIR rising if enabled) + RTC */
	bk_gpio_register_wakeup_source(PIN_PWR_BUTTON, GPIO_INT_TYPE_FALLING_EDGE);
	bk_gpio_register_wakeup_source(PIN_USB_PRESENT, GPIO_INT_TYPE_RISING_EDGE);
	if (wake_pir)
		bk_gpio_register_wakeup_source(PIN_PWR_PIR, GPIO_INT_TYPE_RISING_EDGE);
	else
		bk_gpio_unregister_wakeup_source(PIN_PWR_PIR);
	bk_pm_wakeup_source_set(PM_WAKEUP_SOURCE_INT_GPIO, NULL);
	err = arm_rtc_wake(minutes);
	if (err == BK_OK) {
		bk_pm_wakeup_source_set(PM_WAKEUP_SOURCE_INT_RTC, NULL);
	} else {
		os_printf("[pwr] rtc alarm failed %d: sleeping with GPIO wake only\r\n", err);
	}

	/* 4. the four power domains the pm module requires off; wait for the drivers, then vote ourselves */
	for (t = 0; t < 6; t++) {
		bool all_off = true;
		for (i = 0; i < sizeof(need_off) / sizeof(need_off[0]); i++)
			if (bk_pm_module_power_state_get(need_off[i]) == 0)   /* 0 = powered */
				all_off = false;
		if (all_off)
			break;
		rtos_delay_milliseconds(500);
	}
	for (i = 0; i < sizeof(need_off) / sizeof(need_off[0]); i++) {
		if (bk_pm_module_power_state_get(need_off[i]) == 0) {
			os_printf("[pwr] %s still powered: voting it off\r\n", module_name(need_off[i]));
			bk_pm_module_vote_power_ctrl(need_off[i], PM_POWER_MODULE_STATE_OFF);
		}
	}

	/* 5. arm deep sleep; the idle task enters it (we never return from the loop below) */
	os_printf("[pwr] entering deep sleep\r\n");
	rtos_delay_milliseconds(50);
	bk_pm_sleep_mode_set(PM_MODE_DEEP_SLEEP);
	for (t = 0; t < 20; t++)
		rtos_delay_milliseconds(500);

	/* 6. still here: back out, stay awake for a while, reconnect */
	os_printf("[pwr] deep sleep did not happen: staying awake %u min\r\n", POWER_FAIL_BACKOFF_MS / 60000);
	bk_pm_sleep_mode_set(PM_MODE_DEFAULT);
	bk_alarm_unregister(AON_RTC_ID_1, (uint8_t *)"pwrwake");
	bk_gpio_unregister_wakeup_source(PIN_USB_PRESENT);
	bk_gpio_unregister_lowpower_keep_status(PIN_USB_PRESENT);
	db_pin_cfg(PIN_USB_PRESENT, GPIO_INPUT_ENABLE, GPIO_PULL_DISABLE);
	if (wake_pir)
		bk_gpio_unregister_wakeup_source(PIN_PWR_PIR);
	s_fail_until_ms = rtos_get_time() + POWER_FAIL_BACKOFF_MS;
	s_sleep_at_ms = 0;
	app_wifi_reconnect();
	power_activity();
	s_sleeping = false;
}

static void power_task(beken_thread_arg_t arg)
{
	while (1) {
		uint32_t now;

		rtos_delay_milliseconds(POWER_TASK_PERIOD_MS);
		now = rtos_get_time();
		if (s_sleep_at_ms && (int32_t)(now - s_sleep_at_ms) >= 0) {
			s_sleep_at_ms = 0;
			power_do_sleep("sleepnow");
			continue;
		}
		if (!s_sleep_en || s_inhibit || now < POWER_BOOT_HOLD_MS)
			continue;
		if (s_fail_until_ms && (int32_t)(now - s_fail_until_ms) < 0)
			continue;
		if (usb_present() && !s_force)
			continue;
		if (app_power_busy() || (s_wake_pending && app_power_got_ip())) {
			s_last_activity_ms = now;
			continue;
		}
		if (now - s_last_activity_ms < s_idle_s * 1000)
			continue;
		power_do_sleep("idle");
	}
}

void power_init(void)
{
	beken_thread_t t;
	uint32_t v;

	s_sleep_en = kv_get_bool(KV_PWR_SLEEP, false);
	s_force = kv_get_bool(KV_PWR_FORCE, false);
	s_wake_pir = kv_get_bool(KV_PWR_WPIR, false);
	s_hold9 = kv_get_bool(KV_PWR_HOLD9, false);
	if (kv_get_u32(KV_PWR_IDLE, &v) && v >= POWER_IDLE_MIN_S)
		s_idle_s = v;
	if (kv_get_u32(KV_PWR_SNAP, &v) && v <= POWER_SNAP_MAX_MIN)
		s_snap_min = v;
	if (kv_get_u32(KV_PWR_BCHAN, &v) && v < 16)
		s_bchan = v;
	if (kv_get_u32(KV_PWR_BRATIO, &v) && v >= 100 && v <= 400)
		s_bratio = v;
	{
		char sv[40];
		if (db_kv_get(KV_PWR_SCHED, sv, sizeof(sv)) && !sched_parse(sv))
			os_printf("[pwr] bad %s '%s': using the default schedule\r\n", KV_PWR_SCHED, sv);
	}

	/* status inputs as the stock configures them (GPIO 50 = PIR enable is owned by pir.c) */
	db_pin_cfg(PIN_USB_PRESENT, GPIO_INPUT_ENABLE, GPIO_PULL_DISABLE);
	db_pin_cfg(PIN_CHARGING, GPIO_INPUT_ENABLE, GPIO_PULL_UP_EN);

	/* why did we boot? */
	s_boot_reason = bk_misc_get_reset_reason();
	if (s_boot_reason == RESET_SOURCE_DEEPPS_GPIO) {
		uint32_t pin = bk_gpio_get_wakeup_gpio_id();
		if (pin == PIN_PWR_BUTTON)
			os_strlcpy(s_wake_reason, "button", sizeof(s_wake_reason));
		else if (pin == PIN_PWR_PIR)
			os_strlcpy(s_wake_reason, "pir", sizeof(s_wake_reason));
		else if (pin == PIN_USB_PRESENT)
			os_strlcpy(s_wake_reason, "usb", sizeof(s_wake_reason));
		else if (pin == PIN_UART_RX)
			os_strlcpy(s_wake_reason, "uart", sizeof(s_wake_reason));
		else
			snprintf(s_wake_reason, sizeof(s_wake_reason), "gpio%u", pin);
		s_wake_pending = true;
		app_power_wake_input(pin);   /* button -> chime + ring report before Wi-Fi is up */
	} else if (s_boot_reason == RESET_SOURCE_DEEPPS_RTC) {
		os_strlcpy(s_wake_reason, "rtc", sizeof(s_wake_reason));
		s_wake_pending = true;
	} else if (reset_reason_is_crash(s_boot_reason)) {
		/* a crash in the sleep sequence must not become a reboot loop on battery */
		s_inhibit = true;
		s_inhibit_why = "crash boot";
	}
	power_activity();
	{
		char sv[48];
		sched_str(sv, sizeof(sv));
		os_printf("[pwr] boot: %s%s%s | batt %u mV usb %d chg %d | sleep %s idle %u s wake_pir %s snap_min %u force %s sched %s%s\r\n",
			  reset_reason_str(s_boot_reason), s_wake_pending ? " wake=" : "", s_wake_pending ? s_wake_reason : "",
			  power_batt_mv(), usb_present(), charging(), s_sleep_en ? "on" : "off", s_idle_s, s_wake_pir ? "on" : "off",
			  s_snap_min, s_force ? "on" : "off", sv, s_inhibit ? " (auto sleep inhibited: crash boot)" : "");
	}
	if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "power", power_task, 4096, NULL) != BK_OK)
		os_printf("[pwr] task failed\r\n");
}

/* ---- command ---- */
static bool parse_onoff(const char *s, bool *out)
{
	if (os_strcmp(s, "on") == 0 || os_strcmp(s, "1") == 0) { *out = true; return true; }
	if (os_strcmp(s, "off") == 0 || os_strcmp(s, "0") == 0) { *out = false; return true; }
	return false;
}

static void pwr_status(void)
{
	uint32_t now = rtos_get_time(), adc = batt_adc_mv(), mv = adc * s_bratio / 100;
	bool chg = charging();
	uint32_t hold = now < POWER_BOOT_HOLD_MS ? (POWER_BOOT_HOLD_MS - now) / 1000 : 0;
	char sv[48], why[96];
	uint32_t next = power_wake_interval(why, sizeof(why));

	sched_str(sv, sizeof(sv));
	db_reply("[pwr] batt %u mV %u%% (adc%u %u mV x%u.%02u)%s usb %d chg %d | sleep %s force %s idle %u s wake_pir %s%s snap_min %u"
		 " | sched %s -> next %u min (%s) | up %u s hold %u s idle %u s busy %d%s%s%s | boot %s%s%s\r\n",
		 mv, batt_pct(mv, chg), s_bchan, adc, s_bratio / 100, s_bratio % 100, s_batt_cached ? " (cached: sd power)" : "", usb_present(), chg,
		 s_sleep_en ? "on" : "off", s_force ? "on" : "off", s_idle_s, s_wake_pir ? "on" : "off",
		 s_wake_pir && s_low_batt ? " (FORCED OFF: battery)" : "", s_snap_min, sv, next, why,
		 now / 1000, hold, (now - s_last_activity_ms) / 1000, app_power_busy(),
		 s_inhibit ? " INHIBITED " : "", s_inhibit ? s_inhibit_why : "",
		 s_sleep_at_ms ? " sleepnow pending" : (s_fail_until_ms && (int32_t)(now - s_fail_until_ms) < 0 ? " backoff" : ""),
		 reset_reason_str(s_boot_reason), s_wake_reason[0] ? " wake=" : "", s_wake_reason);
}

void cli_pwr_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	bool b;
	uint32_t v;

	if (argc < 2 || os_strcmp(argv[1], "status") == 0) {
		pwr_status();
		return;
	}
	if (os_strcmp(argv[1], "sleep") == 0 && argc >= 4 && os_strcmp(argv[2], "force") == 0 && parse_onoff(argv[3], &b)) {
		s_force = b;
		db_kv_set(KV_PWR_FORCE, b ? "1" : "0");
		db_reply("[pwr] sleep force %s (sleep even on USB)\r\n", b ? "on" : "off");
	} else if (os_strcmp(argv[1], "sleep") == 0 && argc >= 3 && parse_onoff(argv[2], &b)) {
		s_sleep_en = b;
		db_kv_set(KV_PWR_SLEEP, b ? "1" : "0");
		if (!b)
			s_sleep_at_ms = 0;   /* also cancels a pending sleepnow */
		db_reply("[pwr] automatic sleep %s%s\r\n", b ? "on" : "off",
			 b && rtos_get_time() < POWER_BOOT_HOLD_MS ? " (first sleep after the 120 s boot hold)" : "");
	} else if (os_strcmp(argv[1], "idle") == 0 && argc >= 3) {
		v = os_strtoul(argv[2], NULL, 10);
		if (v < POWER_IDLE_MIN_S)
			v = POWER_IDLE_MIN_S;
		s_idle_s = v;
		kv_set_u32(KV_PWR_IDLE, v);
		db_reply("[pwr] idle %u s\r\n", v);
	} else if (os_strcmp(argv[1], "hold9") == 0 && argc >= 3 && parse_onoff(argv[2], &b)) {
		s_hold9 = b;
		db_kv_set(KV_PWR_HOLD9, b ? "1" : "0");
		db_reply("[pwr] GPIO 9 rail %s through deep sleep\r\n", b ? "held high" : "dropped");
	} else if (os_strcmp(argv[1], "wake_pir") == 0 && argc >= 3 && parse_onoff(argv[2], &b)) {
		s_wake_pir = b;
		db_kv_set(KV_PWR_WPIR, b ? "1" : "0");
		db_reply("[pwr] PIR wake %s\r\n", b ? "on" : "off");
	} else if (os_strcmp(argv[1], "snap_min") == 0 && argc >= 3) {
		v = os_strtoul(argv[2], NULL, 10);
		if (v > POWER_SNAP_MAX_MIN)
			v = POWER_SNAP_MAX_MIN;
		s_snap_min = v;
		kv_set_u32(KV_PWR_SNAP, v);
		db_reply("[pwr] rtc wake every %u min%s%s\r\n", v ? v : POWER_KEEPALIVE_MIN, v ? "" : " (keepalive only)",
			 s_sched_on ? " (used only with `pwr sched off`; the schedule is on)" : "");
	} else if (os_strcmp(argv[1], "sleepnow") == 0) {
		if (s_sleeping) {
			db_reply("[pwr] sleep sequence already running\r\n");
			return;
		}
		s_sleep_at_ms = rtos_get_time() + POWER_SLEEPNOW_GRACE_MS;
		if (!s_sleep_at_ms)
			s_sleep_at_ms = 1;
		db_reply("[pwr] sleeping in %u s: wake on button%s, usb plug, uart rx, rtc in %u min (cancel: pwr sleep off)\r\n",
			 POWER_SLEEPNOW_GRACE_MS / 1000, s_wake_pir && !s_low_batt ? ", pir" : "", power_wake_interval(NULL, 0));
	} else if (os_strcmp(argv[1], "chan") == 0 && argc >= 3) {
		v = os_strtoul(argv[2], NULL, 10);
		if (v < 16) {
			s_bchan = v;
			kv_set_u32(KV_PWR_BCHAN, v);
		}
		db_reply("[pwr] battery on ADC%u: %u mV x%u.%02u = %u mV\r\n", s_bchan, batt_adc_mv(), s_bratio / 100, s_bratio % 100, power_batt_mv());
	} else if (os_strcmp(argv[1], "ratio") == 0 && argc >= 3) {
		v = os_strtoul(argv[2], NULL, 10);
		if (v >= 100 && v <= 400) {
			s_bratio = v;
			kv_set_u32(KV_PWR_BRATIO, v);
		}
		db_reply("[pwr] divider ratio x%u.%02u -> %u mV\r\n", s_bratio / 100, s_bratio % 100, power_batt_mv());
	} else if (os_strcmp(argv[1], "sched") == 0) {
		char sv[48], why[96], joined[40];
		if (argc == 3 && (os_strcmp(argv[2], "off") == 0 || os_strcmp(argv[2], "on") == 0)) {
			s_sched_on = os_strcmp(argv[2], "on") == 0;
			if (s_sched_on)
				snprintf(joined, sizeof(joined), "%02d:%02d %02d:%02d %u %u", s_day_start / 60, s_day_start % 60,
					 s_day_end / 60, s_day_end % 60, s_day_min, s_night_min);
			db_kv_set(KV_PWR_SCHED, s_sched_on ? joined : "off");
		} else if (argc >= 6) {
			snprintf(joined, sizeof(joined), "%s %s %s %s", argv[2], argv[3], argv[4], argv[5]);
			if (!sched_parse(joined)) {
				db_reply("pwr sched DAY_START DAY_END DAY_MIN NIGHT_MIN (HH:MM HH:MM 1-1440 1-1440, e.g. 07:00 22:00 15 60) | sched off | sched on\r\n");
				return;
			}
			db_kv_set(KV_PWR_SCHED, joined);
		} else if (argc != 2) {
			db_reply("pwr sched DAY_START DAY_END DAY_MIN NIGHT_MIN | sched off | sched on | sched\r\n");
			return;
		}
		sched_str(sv, sizeof(sv));
		v = power_wake_interval(why, sizeof(why));
		db_reply("[pwr] sched %s -> next rtc wake in %u min (%s)\r\n", sv, v, why);
	} else if (os_strcmp(argv[1], "policy") == 0) {
		policy_cli(argc, argv);
	} else {
		db_reply("pwr status | sleep on|off | sleep force on|off | idle S | wake_pir on|off | snap_min M | sched START END DAY NIGHT|off | policy [URL|auto|off|pull] | hold9 on|off | sleepnow | chan ADC | ratio PCT\r\n");
	}
}

#else
typedef int power_c_not_built_on_this_cpu_t;   /* CPU1/CPU2: no audio/Wi-Fi/pm here, keep the translation unit non-empty */
#endif /* CONFIG_SYS_CPU0 */
