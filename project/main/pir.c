/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: PIR motion sensor: enable, sensitivity, edge ISR + level poller, cooldown. */
/* PIR motion sensor control (feat/motion, merged as v0.20.0).
 *
 * Pins (stock tuya_pir.c, recovered from the flash dump with capstone, 2026-10-03):
 *   GPIO 24  PIR output, active-high pulses of 2-4 s; stock: input, pull-up, rising-edge IRQ
 *   GPIO 50  "pir en":   stock inits it as push-pull output HIGH and never changes it
 *   GPIO 25  "pir set1": stock app_pir_set_sens(n) writes GPIO 25 = n & 1
 *   GPIO 26  "pir set0": stock app_pir_set_sens(n) writes GPIO 26 = (n >> 1) & 1
 * All three control pins are initialised from the same config {PUSH_PULL, OUTPUT, HIGH}
 * (3 bytes 04 01 01 at 0x020172d5), then set_sens(app_pir_sens) is applied.
 * Stock KV defaults: app_pir_sens = 1 (one byte), app_pir_enable = 1. Its "set pir enable" only
 * enables/disables the GPIO 24 IRQ (tkl_gpio_irq_enable/disable), GPIO 50 stays high.
 * Stock debounce: a new "pir triggered" is reported only if 30 s (0x752f ms) passed since the
 * last one ("pir ignore, now(%u) last(%u)" otherwise); it polls the level every 100 ms after
 * the IRQ and reports "pir triggered: 0" when the output drops again.
 *
 * Ours: GPIO 50 high while enabled (low when `pir off`, which also disables the IRQ), the
 * two sensitivity bits as in the stock, a KV cooldown (default 10 s) applied in the ISR, and a
 * short warm-up blanking after boot / enable so a settling sensor cannot spam reports.
 * The accepted edge runs the callback given to pir_init() (app_main: motion_count++ and the
 * reporter posts "motion" to Home Assistant). Everything here is CPU0 only.
 */
#include <common/bk_include.h>   /* sdkconfig.h: CONFIG_SYS_CPU0 */
#if (CONFIG_SYS_CPU0)   /* GPIO ISR + easyflash KV exist on CPU0 only; the file is compiled for all three cores */
#include "pir.h"
#include <os/os.h>
#include <os/str.h>
#include <os/mem.h>
#include <stdio.h>
#include <string.h>
#include <driver/gpio.h>

#define PIR_PIN_OUT 24
#define PIR_PIN_EN 50
#define PIR_PIN_SET1 25   /* sensitivity bit 0 */
#define PIR_PIN_SET0 26   /* sensitivity bit 1 */

#define KV_PIR_ON "db_pir_on"       /* "1" / "0", default on (stock default, and what v0.18 did implicitly) */
#define KV_PIR_SENS "db_pir_sens"   /* "0".."3", default 1 (stock default) */
#define KV_PIR_CD "db_pir_cd"       /* cooldown ms, default 10000 (stock: 30000) */

#define PIR_SENS_DEFAULT 1
#define PIR_SENS_MAX 3
#define PIR_CD_DEFAULT_MS 10000
#define PIR_CD_MIN_MS 500
#define PIR_CD_MAX_MS 600000
#define PIR_WARMUP_MS 5000   /* ignore edges this long after boot or `pir on` */

/* libdriver (middleware/driver/gpio), not in a public header: release a pin from the SDK device
 * map so bk_gpio_set_config() does not fail with BK_ERR_GPIO_INTERNAL_USED. */
extern bk_err_t gpio_dev_unmap(gpio_id_t gpio_id);
/* EasyFlash KV (components/easy_flash/bk_ef.h) */
extern int bk_set_env_enhance(const char *key, const void *value, int value_len);
extern int bk_get_env_enhance(const char *key, void *value, int value_len);

static bool s_on = true;
static uint32_t s_sens = PIR_SENS_DEFAULT;
static uint32_t s_cd_ms = PIR_CD_DEFAULT_MS;
static pir_motion_cb_t s_cb;
/* touched from the ISR */
static volatile uint32_t s_count, s_edges, s_ignored, s_last_ms, s_armed_at_ms, s_missed;
static int s_en_level = -1;   /* what we drive on GPIO 50 (an output with its input buffer off reads back 1) */

/* ---- KV helpers (private copies: app_main's are static) ---- */

static uint32_t kv_get_u32(const char *key, uint32_t def)
{
	char buf[16];
	int n = bk_get_env_enhance(key, buf, sizeof(buf) - 1);

	if (n <= 0 || n >= (int)sizeof(buf))
		return def;
	buf[n] = 0;
	return (uint32_t)os_strtoul(buf, NULL, 10);
}

static void kv_set_u32(const char *key, uint32_t v)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%u", v);
	bk_set_env_enhance(key, buf, os_strlen(buf));
}

/* ---- pins ---- */

static void pin_out(uint32_t p, int level)
{
	gpio_config_t cfg = {
		.io_mode = GPIO_OUTPUT_ENABLE,
		.pull_mode = GPIO_PULL_DISABLE,
		.func_mode = GPIO_SECOND_FUNC_DISABLE,
	};

	gpio_dev_unmap(p);
	bk_gpio_set_config(p, &cfg);
	bk_gpio_disable_input(p);
	if (level)
		bk_gpio_set_output_high(p);
	else
		bk_gpio_set_output_low(p);
}

static void pir_apply_sens(void)
{
	pin_out(PIR_PIN_SET1, s_sens & 1);
	pin_out(PIR_PIN_SET0, (s_sens >> 1) & 1);
}

/* GPIO 50 + IRQ follow s_on. Never blocks (called from the shell / TCP command task). */
static void pir_apply_enable(void)
{
	s_armed_at_ms = rtos_get_time();
	if (s_on) {
		pin_out(PIR_PIN_EN, 1);
		s_en_level = 1;
		pir_apply_sens();
		bk_gpio_enable_interrupt(PIR_PIN_OUT);
	} else {
		bk_gpio_disable_interrupt(PIR_PIN_OUT);
		pin_out(PIR_PIN_EN, 0);
		s_en_level = 0;
	}
}

/* ---- the edge handler: ISR and `motion test` share it ---- */

static enum pir_edge_result pir_handle_edge(void)
{
	uint32_t now = rtos_get_time();

	s_edges++;
	if (!s_on)
		return PIR_EDGE_OFF;
	if (now - s_armed_at_ms < PIR_WARMUP_MS) {
		s_ignored++;
		return PIR_EDGE_WARMUP;
	}
	if (s_count && now - s_last_ms < s_cd_ms) {
		s_ignored++;
		return PIR_EDGE_COOLDOWN;
	}
	s_last_ms = now;
	s_count++;
	if (s_cb)
		s_cb();
	return PIR_EDGE_ACCEPTED;
}

static void pir_isr(gpio_id_t id)
{
	(void)id;
	pir_handle_edge();
}

/* ---- public ---- */

void pir_init(pir_motion_cb_t cb)
{
	gpio_config_t in = {
		.io_mode = GPIO_INPUT_ENABLE,
		.pull_mode = GPIO_PULL_DISABLE,   /* the board drives the line; idle low measured */
		.func_mode = GPIO_SECOND_FUNC_DISABLE,
	};

	s_cb = cb;
	s_on = kv_get_u32(KV_PIR_ON, 1) != 0;
	s_sens = kv_get_u32(KV_PIR_SENS, PIR_SENS_DEFAULT);
	if (s_sens > PIR_SENS_MAX)
		s_sens = PIR_SENS_DEFAULT;
	s_cd_ms = kv_get_u32(KV_PIR_CD, PIR_CD_DEFAULT_MS);
	if (s_cd_ms < PIR_CD_MIN_MS || s_cd_ms > PIR_CD_MAX_MS)
		s_cd_ms = PIR_CD_DEFAULT_MS;

	gpio_dev_unmap(PIR_PIN_OUT);
	bk_gpio_set_config(PIR_PIN_OUT, &in);
	bk_gpio_disable_output(PIR_PIN_OUT);
	bk_gpio_register_isr(PIR_PIN_OUT, pir_isr);
	bk_gpio_set_interrupt_type(PIR_PIN_OUT, GPIO_INT_TYPE_RISING_EDGE);
	pir_apply_enable();   /* GPIO 50, sensitivity bits, IRQ enable if on */
	os_printf("[pir] %s, sens %u (GPIO25=%u GPIO26=%u), cooldown %u ms, warm-up %u ms\r\n",
		  s_on ? "on" : "off", s_sens, s_sens & 1, (s_sens >> 1) & 1, s_cd_ms, PIR_WARMUP_MS);
}

bool pir_is_on(void)
{
	return s_on;
}

uint32_t pir_sens_get(void)
{
	return s_sens;
}

uint32_t pir_cooldown_ms(void)
{
	return s_cd_ms;
}

uint32_t pir_count(void)
{
	return s_count;
}

enum pir_edge_result pir_trigger(void)
{
	return pir_handle_edge();
}

/* Found 2026-10-03 on v0.20.0: with the camera open (CPU1 up) not one PIR pulse reached the ISR,
 * because every core enabled the shared GPIO interrupt towards itself and CPU1's gpio_isr()
 * cleared the status first (SDK patch 0007 restricts GPIO interrupts to CPU0). This poller is the
 * belt to that brace: a 0->1 transition that did not move s_edges since the previous poll is fed
 * through the same handler and counted in `missed` (PIR pulses last 2-4 s, so 50 ms is plenty). */
void pir_poll(void)
{
	static int last_level = -1;
	static uint32_t edges_seen;
	int level;

	if (!s_on) {
		last_level = -1;
		edges_seen = s_edges;
		return;
	}
	level = bk_gpio_get_input(PIR_PIN_OUT) ? 1 : 0;
	if (last_level == 0 && level == 1 && s_edges == edges_seen) {
		s_missed++;
		pir_handle_edge();
	}
	last_level = level;
	edges_seen = s_edges;
}

uint32_t pir_missed(void)
{
	return s_missed;
}

/* ---- commands ---- */

static void pir_status(char *out, size_t cap)
{
	uint32_t now = rtos_get_time(), last = s_last_ms, ago = s_count ? now - last : 0;
	uint32_t left = (s_count && now - last < s_cd_ms) ? s_cd_ms - (now - last) : 0;

	snprintf(out, cap,
		 "[pir] %s sens=%u (GPIO25=%u GPIO26=%u) cooldown=%u ms count=%u last=%u ms ago "
		 "(cooldown left %u ms) edges=%u ignored=%u missed=%u GPIO24=%d GPIO50=%d\r\n",
		 s_on ? "on" : "off", s_sens, s_sens & 1, (s_sens >> 1) & 1, s_cd_ms, s_count, ago, left,
		 s_edges, s_ignored, s_missed, bk_gpio_get_input(PIR_PIN_OUT) ? 1 : 0, s_en_level);
}

void pir_command(int argc, char **argv, char *out, size_t cap)
{
	uint32_t v;

	if (argc >= 2 && (os_strcmp(argv[1], "on") == 0 || os_strcmp(argv[1], "off") == 0)) {
		s_on = os_strcmp(argv[1], "on") == 0;
		kv_set_u32(KV_PIR_ON, s_on ? 1 : 0);
		pir_apply_enable();
		snprintf(out, cap, "[pir] %s (GPIO50=%d, IRQ %s, edges ignored for %u ms)\r\n",
			 s_on ? "on" : "off", s_on ? 1 : 0, s_on ? "enabled" : "disabled",
			 s_on ? PIR_WARMUP_MS : 0);
	} else if (argc >= 3 && os_strcmp(argv[1], "sens") == 0) {
		v = os_strtoul(argv[2], NULL, 10);
		if (v > PIR_SENS_MAX) {
			snprintf(out, cap, "[pir] sens must be 0-%u (stock used 0/1/2, default 1)\r\n", PIR_SENS_MAX);
			return;
		}
		s_sens = v;
		kv_set_u32(KV_PIR_SENS, s_sens);
		if (s_on)
			pir_apply_sens();
		snprintf(out, cap, "[pir] sens %u -> GPIO25=%u GPIO26=%u%s\r\n", s_sens, s_sens & 1,
			 (s_sens >> 1) & 1, s_on ? "" : " (applied when `pir on`)");
	} else if (argc >= 3 && os_strcmp(argv[1], "cooldown") == 0) {
		v = os_strtoul(argv[2], NULL, 10);
		if (v < PIR_CD_MIN_MS || v > PIR_CD_MAX_MS) {
			snprintf(out, cap, "[pir] cooldown must be %u-%u ms\r\n", PIR_CD_MIN_MS, PIR_CD_MAX_MS);
			return;
		}
		s_cd_ms = v;
		kv_set_u32(KV_PIR_CD, s_cd_ms);
		snprintf(out, cap, "[pir] cooldown %u ms\r\n", s_cd_ms);
	} else if (argc >= 2 && os_strcmp(argv[1], "status") == 0) {
		pir_status(out, cap);
	} else if (argc >= 2) {
		snprintf(out, cap, "pir on|off | sens 0-3 | cooldown MS | status\r\n");
	} else {
		pir_status(out, cap);
	}
}

void motion_command(int argc, char **argv, char *out, size_t cap)
{
	enum pir_edge_result r;
	uint32_t now, left;

	if (argc < 2 || os_strcmp(argv[1], "test") != 0) {
		snprintf(out, cap, "motion test   (simulated PIR edge through the ISR handler: cooldown, count, HA report)\r\n");
		return;
	}
	r = pir_trigger();
	now = rtos_get_time();
	switch (r) {
	case PIR_EDGE_ACCEPTED:
		snprintf(out, cap, "[motion] simulated edge accepted: count %u, report queued\r\n", s_count);
		break;
	case PIR_EDGE_OFF:
		snprintf(out, cap, "[motion] ignored: pir off (`pir on`)\r\n");
		break;
	case PIR_EDGE_WARMUP:
		left = PIR_WARMUP_MS - (now - s_armed_at_ms);
		snprintf(out, cap, "[motion] ignored: warm-up, %u ms left\r\n", left);
		break;
	case PIR_EDGE_COOLDOWN:
	default:
		left = s_cd_ms - (now - s_last_ms);
		snprintf(out, cap, "[motion] ignored: cooldown, %u ms left (count %u)\r\n", left, s_count);
		break;
	}
}
#endif /* CONFIG_SYS_CPU0 */
