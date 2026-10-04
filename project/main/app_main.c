/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: main application: Wi-Fi, webhook reports, TCP command channel, camera HTTP server, audio engine, OTA, CLI. */
#include "bk_private/bk_init.h"
#include "bk_private/bk_cli.h"
#include <components/system.h>
#include <os/os.h>
#include <os/str.h>
#include <os/mem.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <components/shell_task.h>
#include <driver/gpio.h>
#include <driver/adc.h>
#if (CONFIG_SYS_CPU0)
#include <errno.h>
#include <modules/wifi.h>
#include <modules/wifi_types.h>
#include <components/event.h>
#include <components/netif.h>
#include <components/netif_types.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <driver/aud_dac.h>
#include <driver/aud_adc.h>
#include <driver/aud_dac_types.h>
#include <driver/aud_adc_types.h>
#include <driver/flash.h>
#include <driver/dma.h>
#include "pir.h"
#include "power.h"   /* feat/power: battery, charger, deep sleep */
#include "aud_engine.h"   /* engine API implemented below, used by talk.c */
#include "talk.h"         /* two-way audio over HTTP (/speak, /mic.wav) */
#include "audio_voc.h"    /* full-duplex voice engine (SDK aud_intf voice mode, AEC) for the RTSP audio tracks */
#include "sdcard.h"   /* optional microSD storage (feature "sd") */
#endif
#ifdef DOORBELL_AVDK   /* all cores: media_service_init() runs on CPU0/1/2 */
#include "media_service.h"
#if (CONFIG_SYS_CPU0)   /* the multimedia component (media_app) is only linked into the CPU0 image */
#include "media_app.h"
#include "rtsp.h"
#include <driver/media_types.h>
#include <driver/i2c.h>
#include <driver/i2c_types.h>
#if CONFIG_DVP_CAMERA
#include <driver/video_common_driver.h>
#endif
#endif
#endif
#include <stdarg.h>
#include "night.h"   /* LED arbiter, night light, ring indicator, `night` / `sys` commands (night.c) */

#define FW_NAME "doorbell-bk7258-fw"
/* ---- reply buffer: command output for the TCP channel (UART still gets everything) ---- */
static char s_reply[640];   /* one command's REPLY() text; `sd status` is three lines (0.24.4: 320 cut the trace off) */
static bool s_reply_on;
#define REPLY(...) do { os_printf(__VA_ARGS__); if (s_reply_on) { size_t _l = os_strlen(s_reply); \
	if (_l < sizeof(s_reply) - 1) snprintf(s_reply + _l, sizeof(s_reply) - _l, __VA_ARGS__); } } while (0)

/* REPLY() for other source files (night.c, power.c) */
void db_reply(const char *fmt, ...)
{
	char buf[300];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	REPLY("%s", buf);
}

#define FW_VERSION "0.26.0"

/* Pins our own commands must never reconfigure: UART0 console (RX/TX). */
#define PIN_IS_CONSOLE(p) ((p) == 10 || (p) == 11)
#define PIN_COUNT 56

/* Per-pin config registers: SOC_AON_GPIO_REG_BASE (include/soc/bk7258/reg_base.h, CONFIG_SPE=1 so
 * SOC_ADDR_OFFSET is 0), one 32-bit word per pin. Bits (soc/gpio_struct.h): 0 input value,
 * 1 output value, 2 input_en, 3 output_en, 4 pull up(1)/down(0), 5 pull_en, 6 second-function en,
 * 7 input monitor, 8-9 drive strength, 10-13 interrupt. */
#define AON_GPIO_BASE 0x44000400u
#define PIN_CFG(p) (*(volatile uint32_t *)(AON_GPIO_BASE + 4u * (p)))

extern void user_app_main(void);
extern void rtos_set_user_app_entry(beken_thread_function_t entry);
extern float saradc_calculate(uint16_t adc_val);
/* In libdriver (middleware/driver/gpio), not in a public header: releases a pin from the SDK
 * device map, otherwise bk_gpio_set_config() fails with BK_ERR_GPIO_INTERNAL_USED (-0x2009). */
extern bk_err_t gpio_dev_unmap(gpio_id_t gpio_id);
extern bk_err_t gpio_dev_map(gpio_id_t gpio_id, gpio_dev_t dev);
/* EasyFlash KV (components/easy_flash/bk_ef.h); value_len bytes, returns len read / EF_NO_ERR */
extern int bk_set_env_enhance(const char *key, const void *value, int value_len);
extern int bk_get_env_enhance(const char *key, void *value, int value_len);

/* ---- pin: GPIO mapping helpers (os_printf, so they work at any log level) ---- */

static void pin_print_all(const char *label)
{
	char line[PIN_COUNT + 1];
	int p;

	for (p = 0; p < PIN_COUNT; p++)
		line[p] = bk_gpio_get_input(p) ? '1' : '0';
	line[PIN_COUNT] = 0;
	os_printf("%s %s\r\n", label, line);
}

static void pin_usage(void)
{
	REPLY("pin all | mux | dump [N] | cfg N | get N | in N [up|down|none] | hiz N | out N 0|1 | raw N HEX | blink N [count] | watch SECONDS\r\n");
}

/* One pin's full state: AON cfg word (bits: 2 in_en, 3 out DISABLE (active low), 4 pull up/down,
 * 5 pull_en, 6 second-function enable, 8-9 drive strength) and the system-controller function
 * select nibble (SOC_SYS_REG_BASE + 0x30*4 + pin/8*4, 4 bits per pin: which column of the pin's
 * gpio_map.h row is routed; e.g. GPIO 2 sel 1 = SDIO_HOST_CLK). */
static void pin_reply_cfg(uint32_t p)
{
	uint32_t v = PIN_CFG(p);
	uint32_t sel = (*(volatile uint32_t *)(0x44010000u + 0x30u * 4u + 4u * (p / 8)) >> (4u * (p % 8))) & 0xfu;

	REPLY("PIN %u cfg %08x: lvl %u in_en %u out_en %u pull %s func %u cap %u sel %u\r\n", p, v, v & 1, (v >> 2) & 1,
	      ((v >> 3) & 1) ^ 1, (v & BIT(5)) ? ((v & BIT(4)) ? "up" : "down") : "off", (v >> 6) & 1, (v >> 8) & 3, sel);
}

/* Reclaim the pin from any peripheral function and configure it. */
static bk_err_t pin_configure(uint32_t p, gpio_io_mode_t io, gpio_pull_mode_t pull)
{
	gpio_config_t cfg = {
		.io_mode = io,
		.pull_mode = pull,
		.func_mode = GPIO_SECOND_FUNC_DISABLE,
	};

	gpio_dev_unmap(p);
	return bk_gpio_set_config(p, &cfg);
}

static void cli_pin_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	uint32_t p = 0, i;

	if (argc < 2) {
		pin_usage();
		return;
	}
	if (argc >= 3)
		p = os_strtoul(argv[2], NULL, 10);

	if (os_strcmp(argv[1], "all") == 0) {
		pin_print_all("PINS");
		return;
	}
	if (os_strcmp(argv[1], "mux") == 0) {
		/* 8 pins per line: MUX <first pin>: <cfg word low byte> x8 */
		for (i = 0; i < PIN_COUNT; i += 8) {
			os_printf("MUX %02u: %02x %02x %02x %02x %02x %02x %02x %02x\r\n", i,
				  PIN_CFG(i) & 0xff, PIN_CFG(i + 1) & 0xff, PIN_CFG(i + 2) & 0xff,
				  PIN_CFG(i + 3) & 0xff, PIN_CFG(i + 4) & 0xff, PIN_CFG(i + 5) & 0xff,
				  PIN_CFG(i + 6) & 0xff, PIN_CFG(i + 7) & 0xff);
		}
		return;
	}
	if (os_strcmp(argv[1], "dump") == 0) {
		/* no pin: all rows on the UART; with a pin: that pin's row of 8 in the reply too */
		if (argc >= 3 && p < PIN_COUNT) {
			i = p & ~7u;
			REPLY("CFG %02u: %08x %08x %08x %08x %08x %08x %08x %08x\r\n", i,
			      PIN_CFG(i), PIN_CFG(i + 1), PIN_CFG(i + 2), PIN_CFG(i + 3),
			      PIN_CFG(i + 4), PIN_CFG(i + 5), PIN_CFG(i + 6), PIN_CFG(i + 7));
			return;
		}
		for (i = 0; i < PIN_COUNT; i += 8)
			os_printf("CFG %02u: %08x %08x %08x %08x %08x %08x %08x %08x\r\n", i,
				  PIN_CFG(i), PIN_CFG(i + 1), PIN_CFG(i + 2), PIN_CFG(i + 3),
				  PIN_CFG(i + 4), PIN_CFG(i + 5), PIN_CFG(i + 6), PIN_CFG(i + 7));
		REPLY("CFG: 7 rows on the UART; `pin dump N` replies N's row, `pin cfg N` decodes one pin\r\n");
		return;
	}
	/* "watch" takes seconds, not a pin: check it before the pin-range test. */
	if (argc < 3 || (os_strcmp(argv[1], "watch") != 0 && p >= PIN_COUNT)) {
		pin_usage();
		return;
	}

	if (os_strcmp(argv[1], "get") == 0) {
		REPLY("PIN %u = %d (cfg %02x)\r\n", p, bk_gpio_get_input(p) ? 1 : 0, PIN_CFG(p) & 0xff);
	} else if (os_strcmp(argv[1], "cfg") == 0) {
		pin_reply_cfg(p);
	} else if (os_strcmp(argv[1], "watch") == 0) {
		/* argv[2] is seconds here: poll every 5 ms, print each transition. */
		uint32_t secs = p, t0 = rtos_get_time(), n = 0;
		char prev[PIN_COUNT], cur;

		for (i = 0; i < PIN_COUNT; i++)
			prev[i] = bk_gpio_get_input(i) ? 1 : 0;
		pin_print_all("WATCH start");
		while (rtos_get_time() - t0 < secs * 1000) {
			for (i = 0; i < PIN_COUNT; i++) {
				cur = bk_gpio_get_input(i) ? 1 : 0;
				if (cur != prev[i]) {
					os_printf("EDGE %u ms pin %u %d->%d\r\n", rtos_get_time() - t0, i, prev[i], cur);
					prev[i] = cur;
					n++;
				}
			}
			rtos_delay_milliseconds(5);
		}
		os_printf("WATCH end, %u edges\r\n", n);
	} else if (PIN_IS_CONSOLE(p)) {
		REPLY("PIN %u is the console UART, refusing\r\n", p);
	} else if (os_strcmp(argv[1], "in") == 0) {
		const char *pull = argc >= 4 ? argv[3] : "none";
		gpio_pull_mode_t pm = GPIO_PULL_DISABLE;
		bk_err_t err;

		if (os_strcmp(pull, "up") == 0)
			pm = GPIO_PULL_UP_EN;
		else if (os_strcmp(pull, "down") == 0)
			pm = GPIO_PULL_DOWN_EN;
		err = pin_configure(p, GPIO_INPUT_ENABLE, pm);
		bk_gpio_disable_output(p);
		rtos_delay_milliseconds(5);
		REPLY("PIN %u in %s = %d (cfg %02x, err %d)\r\n", p, pull, bk_gpio_get_input(p) ? 1 : 0,
		      PIN_CFG(p) & 0xff, err);
	} else if (os_strcmp(argv[1], "raw") == 0 && argc >= 4) {
		/* write the AON GPIO config word verbatim (hex), e.g. to restore a boot-time state */
		uint32_t v = os_strtoul(argv[3], NULL, 16);

		gpio_dev_unmap(p);
		PIN_CFG(p) = v;
		REPLY("PIN %u raw %08x -> now %08x\r\n", p, v, PIN_CFG(p));
	} else if (os_strcmp(argv[1], "hiz") == 0) {
		bk_err_t err = pin_configure(p, GPIO_IO_DISABLE, GPIO_PULL_DISABLE);

		bk_gpio_disable_output(p);
		bk_gpio_disable_input(p);
		REPLY("PIN %u hiz (cfg %02x, err %d)\r\n", p, PIN_CFG(p) & 0xff, err);
	} else if (os_strcmp(argv[1], "out") == 0 && argc >= 4) {
		int v = os_strtoul(argv[3], NULL, 10) ? 1 : 0;

		pin_configure(p, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		bk_gpio_disable_input(p);
		if (v)
			bk_gpio_set_output_high(p);
		else
			bk_gpio_set_output_low(p);
		REPLY("PIN %u out %d (cfg %02x)\r\n", p, v, PIN_CFG(p) & 0xff);
	} else if (os_strcmp(argv[1], "blink") == 0) {
		uint32_t count = argc >= 4 ? os_strtoul(argv[3], NULL, 10) : 6;

		pin_configure(p, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		for (i = 0; i < count; i++) {
			bk_gpio_set_output_high(p);
			rtos_delay_milliseconds(300);
			bk_gpio_set_output_low(p);
			rtos_delay_milliseconds(300);
		}
		os_printf("PIN %u blinked %u times, left low\r\n", p, count);
	} else {
		pin_usage();
	}
}

/* ---- adc: one calibrated reading of a SARADC channel (same sequence as volt_detect.c) ---- */

/* mem HEXADDR [N]: N (<= 16) raw 32-bit reads. Debug only (CPU1 counters such as media_debug
 * live in shared SRAM and are otherwise only visible on the dead UART); a bad address faults. */
static void cli_mem_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	uint32_t addr, n = 4, i;
	char line[160];
	int l = 0;

	if (argc < 2) {
		REPLY("mem HEXADDR [NWORDS<=16]  (raw 32-bit reads, debug only)\r\n");
		return;
	}
	addr = os_strtoul(argv[1], NULL, 16) & ~3U;
	if (argc > 2)
		n = os_strtoul(argv[2], NULL, 10);
	if (n < 1)
		n = 1;
	if (n > 16)
		n = 16;
	for (i = 0; i < n && l < (int)sizeof(line) - 10; i++)
		l += snprintf(line + l, sizeof(line) - l, "%s%08x", i ? " " : "", (unsigned)*(volatile uint32_t *)(addr + 4 * i));
	REPLY("[mem] %08x: %s\r\n", (unsigned)addr, line);
}

static void cli_adc_read_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	adc_config_t config = {0};
	uint16_t raw = 0;
	uint32_t chan;
	bk_err_t err;

	if (argc < 2) {
		os_printf("adc CHANNEL\r\n");
		return;
	}
	chan = os_strtoul(argv[1], NULL, 10);

	err = bk_adc_acquire();
	if (err != BK_OK) {
		os_printf("ADC%u: acquire failed %d\r\n", chan, err);
		return;
	}
	err = bk_adc_init(chan);
	if (err == BK_OK) {
		config.chan = chan;
		config.adc_mode = ADC_CONTINUOUS_MODE;
		config.src_clk = ADC_SCLK_XTAL_26M;
		config.clk = 203125;
		config.saturate_mode = ADC_SATURATE_MODE_3;
		config.steady_ctrl = 7;
		config.adc_filter = 0;
		config.sample_rate = 0;
		err = bk_adc_set_config(&config);
	}
	if (err == BK_OK)
		err = bk_adc_enable_bypass_clalibration();
	if (err == BK_OK)
		err = bk_adc_start();
	if (err == BK_OK)
		err = bk_adc_read(&raw, 1000);
	if (err == BK_OK)
		REPLY("ADC%u raw=%u mv=%u\r\n", chan, raw, (uint32_t)(saradc_calculate(raw) * 1000));
	else
		REPLY("ADC%u: failed %d (-0x%x)\r\n", chan, err, -err);
	bk_adc_stop();
	bk_adc_deinit(chan);
	bk_adc_release();
}

#if (CONFIG_SYS_CPU0)

/* ---- doorbell prototype: Wi-Fi + Home Assistant webhook ---- */
/* PIR (GPIO 24 output, 50 enable, 25/26 sensitivity) lives in pir.c since v0.19.0 */
#define PIN_BUTTON 53   /* externally pulled high, expected to drop on press */
#define HEARTBEAT_MS (5 * 60 * 1000)
#define KV_SSID "db_ssid"
#define KV_PASS "db_pass"
#define KV_HOOK "db_hook"   /* "host:port/path" */
#define HOOK_DEFAULT ""   /* no webhook until `hook set HOST:PORT/PATH` (stored in KV) */

static char s_ssid[WIFI_SSID_STR_LEN], s_pass[WIFI_PASSWORD_LEN], s_hook[128];
static bool s_hook_unset_logged;   /* "hook not set" is logged once per boot, not per report */
static volatile bool s_got_ip, s_pend_motion, s_pend_ring, s_pend_boot, s_pend_test;
static volatile bool s_pend_fx;
static uint32_t s_dark_mv = 1000;     /* light sensor mV ABOVE this = dark (LDR pulls up in the dark; stock threshold ~2000/4095 raw) */
static bool s_fx_enabled = true;
#define KV_DARK "db_dark_mv"
#define KV_FX "db_fx"
static uint32_t s_light_chan = 14;   /* LDR on GPIO 12 = ADC14 (stock), powered by GPIO 28; higher = darker */
#define LIGHT_ADC_CHAN s_light_chan
#define KV_LIGHT_CHAN "db_light_chan"
#define WHITE_LIGHT_MS 30000
static void ringfx_start(void);
static uint32_t light_read_mv(void);
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
static bool s_cam_open;   /* camera state, used by the light reading too */
#endif

static volatile uint32_t s_last_motion_ms, s_last_ring_ms, s_motion_count, s_ring_count;
static char s_ip[16] = "0.0.0.0";

/* Pristine boot config of every GPIO (tools/boot_pincfg.txt, low byte). Restored before
 * `reboot` because the AON GPIO block survives a soft reset and the ROM samples a strap. */
static const uint8_t s_boot_cfg[PIN_COUNT] = {
	0x78,0x78,0x78,0x78,0x78,0x09,0x48,0x48,0x09,0x09,0x78,0x78,0x48,0x48,0x48,0x48,
	0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,
	0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,
	0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48};

static int kv_get_str(const char *key, char *out, int cap)
{
	int n = bk_get_env_enhance(key, out, cap - 1);

	if (n <= 0 || n >= cap) {
		out[0] = 0;
		return 0;
	}
	out[n] = 0;
	return n;
}

static void kv_set_str(const char *key, const char *val)
{
	bk_set_env_enhance(key, val, os_strlen(val));
}

/* REPLY() for modules outside this file (talk.c, sdcard.c) */
void app_reply(const char *fmt, ...)
{
	char tmp[320];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	REPLY("%s", tmp);
}

bool app_net_up(void)
{
	return s_got_ip;
}

static uint32_t adc_read_mv(uint32_t chan)
{
	static uint32_t s_adc15_last;
	adc_config_t config = {0};
	uint16_t raw = 0;
	uint32_t mv = 0;

	/* ADC15 lives on GPIO 13; with `sd pwr 13` (not the default: the pin turned out not to power
	 * the card) bk_adc_init() would remap the driven pin, and the node needs ~10 s to settle after
	 * it is released. Report the last reading meanwhile (power.c marks it "cached"). */
	if (chan == 15 && sd_adc_pin_blocked(13))
		return s_adc15_last;
	if (bk_adc_acquire() != BK_OK)
		return 0;
	if (bk_adc_init(chan) == BK_OK) {
		config.chan = chan;
		config.adc_mode = ADC_CONTINUOUS_MODE;
		config.src_clk = ADC_SCLK_XTAL_26M;
		config.clk = 203125;
		config.saturate_mode = ADC_SATURATE_MODE_3;
		config.steady_ctrl = 7;
		if (bk_adc_set_config(&config) == BK_OK && bk_adc_enable_bypass_clalibration() == BK_OK
		    && bk_adc_start() == BK_OK && bk_adc_read(&raw, 1000) == BK_OK)
			mv = (uint32_t)(saradc_calculate(raw) * 1000);
	}
	bk_adc_stop();
	bk_adc_deinit(chan);
	bk_adc_release();
	if (chan == 15)
		s_adc15_last = mv;
	return mv;
}

/* Minimal HTTP/1.1 POST over a blocking lwIP socket. Returns the HTTP status or -errno. */
static int http_post_json(const char *hook, const char *json)
{
	char host[64], path[96], req[768], resp[64];
	const char *colon = os_strchr(hook, ':'), *slash = colon ? os_strchr(colon, '/') : NULL;
	int port, sock, n, status = -1, hl;
	struct sockaddr_in sa = {0};
	struct timeval tv = {.tv_sec = 5, .tv_usec = 0};

	if (!colon || !slash || colon - hook >= (int)sizeof(host) || os_strlen(slash) >= sizeof(path))
		return -EINVAL;
	os_memcpy(host, hook, colon - hook);
	host[colon - hook] = 0;
	port = os_strtoul(colon + 1, NULL, 10);
	os_strcpy(path, slash);

	hl = snprintf(req, sizeof(req),
		      "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
		      "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
		      path, host, (int)os_strlen(json), json);
	if (hl <= 0 || hl >= (int)sizeof(req))
		return -E2BIG;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return -ENOMEM;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = inet_addr(host);
	if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0 && send(sock, req, hl, 0) == hl) {
		n = recv(sock, resp, sizeof(resp) - 1, 0);
		if (n > 9) {
			resp[n] = 0;
			status = os_strtoul(resp + 9, NULL, 10);   /* "HTTP/1.1 200 OK" */
		}
	} else {
		status = -ECONNREFUSED;
	}
	close(sock);
	return status;
}

/* extra: NULL or more JSON members starting with a comma (e.g. ",\"reason\":\"rtc\"") */
static int report_ex(const char *event, const char *extra)
{
	char json[384], pwr[96];
	int st;

	if (!s_got_ip)
		return -ENOTCONN;
	if (!s_hook[0]) {   /* no target configured: skip the POST, say so once */
		if (!s_hook_unset_logged) {
			s_hook_unset_logged = true;
			os_printf("[hook] %s skipped: hook not set (hook set HOST:PORT/PATH)\r\n", event);
		}
		return -ENOENT;
	}
	power_json(pwr, sizeof(pwr));   /* ,"batt_mv":..,"batt_pct":..,"usb":..,"charging":.. */
	snprintf(json, sizeof(json),
		 "{\"event\":\"%s\",\"fw\":\"" FW_VERSION "\",\"ip\":\"%s\",\"mv\":%u,\"light_mv\":%u,"
		 "\"uptime_s\":%u,\"motion_count\":%u,\"ring_count\":%u,\"pir_on\":%u,\"pir_sens\":%u%s%s}",
		 event, s_ip, adc_read_mv(9), light_read_mv(), rtos_get_time() / 1000, s_motion_count, s_ring_count,
		 pir_is_on() ? 1u : 0u, pir_sens_get(), pwr, extra ? extra : "");
	st = http_post_json(s_hook, json);
	os_printf("[hook] %s -> %d\r\n", event, st);
	return st;
}

static int report(const char *event)
{
	return report_ex(event, NULL);
}

/* ISR context (from pir.c, after its cooldown/warm-up filter): flag the reporter task. */
static void motion_cb(void)
{
	s_last_motion_ms = rtos_get_time();
	s_motion_count++;
	s_pend_motion = true;
	power_activity();
}

static volatile uint32_t s_last_ring_report_ms;

static void button_isr(gpio_id_t id)
{
	uint32_t now = rtos_get_time();

	if (now - s_last_ring_ms > 800) {
		s_last_ring_ms = now;
		s_ring_count++;
		power_activity();
		s_pend_fx = true;   /* local feedback on every press: chime, ring LEDs, light when dark */
		if (now - s_last_ring_report_ms > 10000) {   /* one notification per 10 s */
			s_last_ring_report_ms = now;
			s_pend_ring = true;
		}
	}
}

static bk_err_t netif_event_cb(void *arg, event_module_t module, int event_id, void *event_data)
{
	netif_ip4_config_t cfg = {0};

	if (event_id == EVENT_NETIF_GOT_IP4) {
		if (bk_netif_get_ip4_config(NETIF_IF_STA, &cfg) == BK_OK)
			os_strlcpy(s_ip, cfg.ip, sizeof(s_ip));
		s_got_ip = true;
		s_pend_boot = true;
		os_printf("[wifi] got ip %s\r\n", s_ip);
	}
	return BK_OK;
}

static bk_err_t wifi_event_cb(void *arg, event_module_t module, int event_id, void *event_data)
{
	if (event_id == EVENT_WIFI_STA_DISCONNECTED) {
		s_got_ip = false;
		os_printf("[wifi] disconnected\r\n");
	}
	return BK_OK;
}

static bk_err_t wifi_connect(void)
{
	wifi_sta_config_t cfg = {0};

	if (!s_ssid[0])
		return BK_ERR_PARAM;
	os_strlcpy(cfg.ssid, s_ssid, sizeof(cfg.ssid));
	os_strlcpy(cfg.password, s_pass, sizeof(cfg.password));
	cfg.auto_reconnect_count = 0;      /* keep retrying */
	cfg.auto_reconnect_timeout = 0;
	BK_LOG_ON_ERR(bk_wifi_sta_set_config(&cfg));
	return bk_wifi_sta_start();
}

static void reporter_task(beken_thread_arg_t arg)
{
	uint32_t last_hb = rtos_get_time();

	while (1) {
		if (s_pend_boot) {
			s_pend_boot = false;
			report("boot");
			last_hb = rtos_get_time();
		}
		if (s_pend_fx) {
			s_pend_fx = false;
			ringfx_start();
			sd_on_event("ring");   /* queued snapshot to the SD card, never blocks */
		}
		if (s_got_ip && s_pend_ring) {   /* wait for Wi-Fi: a press that woke us from deep sleep arrives before the IP */
			s_pend_ring = false;
			report("ring");
		}
		if (s_got_ip && s_pend_motion) {
			s_pend_motion = false;
			night_on_motion();   /* white light for 20 s when dark, before the snapshot HA will take */
			sd_on_event("motion");   /* queued SD snapshot, never blocks */
			report("motion");
		}
		if (s_got_ip && power_wake_pending()) {
			char reason[12], extra[32];
			power_take_wake_event(reason, sizeof(reason));
			snprintf(extra, sizeof(extra), ",\"reason\":\"%s\"", reason);
			report_ex("wake", extra);
		}
		if (s_pend_test) {
			s_pend_test = false;
			report("test");
		}
		if (s_got_ip && rtos_get_time() - last_hb > HEARTBEAT_MS) {
			last_hb = rtos_get_time();
			report("heartbeat");
		}
		pir_poll();   /* GPIO 24 level poll: catches edges the ISR missed (see pir.c) */
		rtos_delay_milliseconds(50);
	}
}

static void inputs_init(void)
{
	pir_init(motion_cb);   /* GPIO 24 ISR + GPIO 50/25/26 from KV (pir.c) */

	pin_configure(PIN_BUTTON, GPIO_INPUT_ENABLE, GPIO_PULL_UP_EN);
	bk_gpio_disable_output(PIN_BUTTON);
	bk_gpio_register_isr(PIN_BUTTON, button_isr);
	bk_gpio_set_interrupt_type(PIN_BUTTON, GPIO_INT_TYPE_FALLING_EDGE);
	bk_gpio_enable_interrupt(PIN_BUTTON);
}

static void cli_wifi_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	wifi_link_status_t st = {0};

	if (argc >= 4 && os_strcmp(argv[1], "set") == 0) {
		os_strlcpy(s_ssid, argv[2], sizeof(s_ssid));
		os_strlcpy(s_pass, argv[3], sizeof(s_pass));
		kv_set_str(KV_SSID, s_ssid);
		kv_set_str(KV_PASS, s_pass);
		os_printf("[wifi] saved ssid '%s' (%u-char password), connecting\r\n", s_ssid, os_strlen(s_pass));
		wifi_connect();
	} else if (argc >= 2 && os_strcmp(argv[1], "clear") == 0) {
		s_ssid[0] = s_pass[0] = 0;
		kv_set_str(KV_SSID, "");
		kv_set_str(KV_PASS, "");
		os_printf("[wifi] credentials cleared\r\n");
	} else if (argc >= 2 && os_strcmp(argv[1], "connect") == 0) {
		os_printf("[wifi] connect -> %d\r\n", wifi_connect());
	} else {
		bk_wifi_sta_get_link_status(&st);
		REPLY("[wifi] ssid '%s' state %d rssi %d ip %s got_ip %d\r\n", s_ssid, st.state, st.rssi, s_ip, s_got_ip);
	}
}

extern int bk_http_ota_download(const char *url);
static char s_ota_url[160];
static beken_thread_t ota_thread;

static void ota_task(beken_thread_arg_t arg)
{
	int ret = bk_http_ota_download(s_ota_url);

	os_printf("[ota] result %d%s\r\n", ret, ret == 0 ? " (rebooting)" : "");
	ota_thread = NULL;
	rtos_delete_thread(NULL);
}

static void cli_ota_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	int ret;

	if (argc < 2) {
		os_printf("ota http://HOST:PORT/app_pack.rbl\r\n");
		return;
	}
	if (!s_got_ip) {
		os_printf("[ota] no network\r\n");
		return;
	}
	os_strlcpy(s_ota_url, argv[1], sizeof(s_ota_url));
	os_printf("[ota] downloading %s\r\n", s_ota_url);
	/* run in its own thread so the shell / TCP channel reply at once; the device reboots on success */
	ret = rtos_create_thread(&ota_thread, BEKEN_APPLICATION_PRIORITY, "ota", ota_task, 8192, NULL);
	if (ret != BK_OK)
		os_printf("[ota] thread failed %d\r\n", ret);
}

static void cli_hook_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	if (argc >= 3 && os_strcmp(argv[1], "set") == 0) {
		os_strlcpy(s_hook, argv[2], sizeof(s_hook));
		kv_set_str(KV_HOOK, s_hook);
	} else if (argc >= 2 && os_strcmp(argv[1], "test") == 0) {
		s_pend_test = true;
	} else if (argc >= 2 && os_strcmp(argv[1], "ring") == 0) {
		s_ring_count++;
		s_pend_ring = true;
	} else if (argc >= 2 && os_strcmp(argv[1], "motion") == 0) {
		s_motion_count++;
		s_pend_motion = true;
	}
	REPLY("[hook] %s  (hook set HOST:PORT/PATH | test | ring | motion)\r\n", s_hook[0] ? s_hook : "not set");
}

/* ---- PIR (pir.c): settings + simulated motion edge; the text comes back here for REPLY ---- */
static void cli_pir_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	char out[256];

	pir_command(argc, argv, out, sizeof(out));
	REPLY("%s", out);
}

static void cli_motion_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	char out[160];

	motion_command(argc, argv, out, sizeof(out));
	REPLY("%s", out);
}





/* ---- audio: speaker tone / stock chimes from flash, mic level ---- */
#define AUDIO_RATE 8000
static volatile bool s_spk_busy, s_spk_stop;
static beken_thread_t s_spk_thread;
static uint32_t s_spk_hz, s_spk_ms, s_spk_addr, s_spk_len;
/* runtime audio config (spk cfg / mic cfg); PA = amplifier enable, SDK reference design uses GPIO 5 */
static int s_pa_pin = 46;   /* speaker amplifier enable, active high (found by sweep 2026-10-03) */
static aud_dac_chl_t s_dac_chl = AUD_DAC_CHL_L;
static aud_dac_work_mode_t s_dac_mode = AUD_DAC_WORK_MODE_DIFFEN;
static uint16_t s_dac_gain = 0x1c;   /* 0x2d = 0 dB clips the amplifier hard; 0x1c is the loudest clean level measured 2026-10-03 */
static aud_adc_chl_t s_adc_chl = AUD_ADC_CHL_L;
static aud_adc_mode_t s_adc_mode = AUD_ADC_MODE_DIFFEN;
static aud_mic_id_t s_mic_id = AUD_MIC_MIC1;
static uint32_t s_adc_gain = 0x2d;
static uint32_t s_dac_rate = AUDIO_RATE, s_adc_rate = AUDIO_RATE;
static aud_clk_t s_dac_clk = AUD_CLK_XTAL, s_adc_clk = AUD_CLK_APLL;   /* SDK defaults; both measured off (DAC 3x, ADC 2x) */   /* configured rates; the hardware may run at another (measure!) */

/* ---- network audio (test rig): mic -> TCP as raw 32-bit FIFO words, TCP -> speaker as s16le 8 kHz mono.
 * The peer is a plain TCP listener (tools/audio_relay.py on a LAN host); the device always connects out. */
static char s_net_host[32];
static uint16_t s_net_port, s_spk_net_port;
static uint32_t s_net_ms;
static volatile bool s_mic_busy;

static int tcp_connect(const char *host, uint16_t port)
{
	struct sockaddr_in sa = {0};
	struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
	int sock = socket(AF_INET, SOCK_STREAM, 0);

	if (sock < 0)
		return -1;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = inet_addr(host);
	if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		close(sock);
		return -1;
	}
	return sock;
}

static void pa_set(int on)
{
	if (s_pa_pin < 0)
		return;
	pin_configure(s_pa_pin, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	if (on)
		bk_gpio_set_output_high(s_pa_pin);
	else
		bk_gpio_set_output_low(s_pa_pin);
}

static const int16_t s_sine32[32] = {0,6393,12539,18204,23170,27245,30273,32137,32767,32137,30273,27245,23170,
	18204,12539,6393,0,-6393,-12539,-18204,-23170,-27245,-30273,-32137,-32767,-32137,-30273,-27245,-23170,-18204,-12539,-6393};

/* ---- DMA audio engine (v0.13.0): the DAC and ADC FIFOs are served by DMA in repeat mode over a
 * double buffer (2 x AUD_FRAME); the finish interrupt fires per half. Threads only touch the byte
 * rings s_spk_ring / s_mic_ring, so Wi-Fi preemption no longer glitches the audio. ---- */
#define AUD_FRAME 1024                 /* bytes per DMA half: 512 s16 samples = 64 ms at 8 kHz */
#define PA_SETTLE_MS 50                /* silence played before the PA is enabled / after it is disabled (pop fix) */
#define SPK_RING (16 * 1024)
#define MIC_RING (32 * 1024)
static uint8_t *s_dac_dma_buf, *s_adc_dma_buf;
static dma_id_t s_dac_dma = DMA_ID_MAX, s_adc_dma = DMA_ID_MAX;
static uint8_t s_spk_ring[SPK_RING], s_mic_ring[MIC_RING];
static volatile uint32_t s_spk_w, s_spk_r, s_mic_w, s_mic_r;   /* monotonic byte indices */
static volatile uint32_t s_dac_half, s_adc_half, s_spk_underrun, s_mic_overrun, s_adc_frames, s_dac_frames;
static volatile bool s_spk_feeding;

static void ring_copy_out(uint8_t *dst, const uint8_t *ring, uint32_t size, uint32_t idx, uint32_t n)
{
	uint32_t first = size - idx < n ? size - idx : n;
	os_memcpy(dst, ring + idx, first);
	if (n > first)
		os_memcpy(dst + first, ring, n - first);
}

static void ring_copy_in(uint8_t *ring, uint32_t size, uint32_t idx, const uint8_t *src, uint32_t n)
{
	uint32_t first = size - idx < n ? size - idx : n;
	os_memcpy(ring + idx, src, first);
	if (n > first)
		os_memcpy(ring, src + first, n - first);
}

static void dac_fill_half(uint32_t half)
{
	uint8_t *dst = s_dac_dma_buf + half * AUD_FRAME;
	uint32_t avail = s_spk_w - s_spk_r, n = avail < AUD_FRAME ? avail : AUD_FRAME;

	ring_copy_out(dst, s_spk_ring, SPK_RING, s_spk_r % SPK_RING, n);
	s_spk_r += n;
	if (n < AUD_FRAME) {
		os_memset(dst + n, 0, AUD_FRAME - n);
		if (s_spk_feeding)
			s_spk_underrun++;
	}
}

static void dac_dma_isr(dma_id_t id)
{
	dac_fill_half(s_dac_half);   /* the half the DMA just finished reading */
	s_dac_half ^= 1;
	s_dac_frames++;
}

static void adc_dma_isr(dma_id_t id)
{
	const uint8_t *src = s_adc_dma_buf + s_adc_half * AUD_FRAME;

	if (MIC_RING - (s_mic_w - s_mic_r) >= AUD_FRAME) {
		ring_copy_in(s_mic_ring, MIC_RING, s_mic_w % MIC_RING, src, AUD_FRAME);
		s_mic_w += AUD_FRAME;
	} else {
		s_mic_overrun++;
	}
	s_adc_half ^= 1;
	s_adc_frames++;
}

static void dma_port(dma_port_config_t *pc, dma_dev_t dev, dma_data_width_t w, uint32_t start, uint32_t end)
{
	pc->dev = dev;
	pc->width = w;
	pc->addr_inc_en = DMA_ADDR_INC_ENABLE;
	pc->addr_loop_en = DMA_ADDR_LOOP_ENABLE;
	pc->start_addr = start;
	pc->end_addr = end;
}

/* dir 0 = DAC (memory -> DAC FIFO, 16-bit mono writes), 1 = ADC (ADC FIFO -> memory, 32-bit words) */
static int aud_dma_start(int dir)
{
	dma_config_t c = {0};
	uint32_t fifo;
	uint8_t **buf = dir ? &s_adc_dma_buf : &s_dac_dma_buf;
	dma_id_t *id = dir ? &s_adc_dma : &s_dac_dma;

	if (!*buf)
		*buf = os_malloc(2 * AUD_FRAME);
	if (!*buf)
		return -1;
	os_memset(*buf, 0, 2 * AUD_FRAME);   /* the first two DAC halves play before the ISR refills them: silence, not heap garbage */
	bk_dma_driver_init();
	*id = bk_dma_alloc(DMA_DEV_AUDIO);
	if (*id >= DMA_ID_MAX)
		return -2;
	c.mode = DMA_WORK_MODE_REPEAT;
	c.chan_prio = 1;
	c.trans_type = DMA_TRANS_DEFAULT;
	if (dir) {
		bk_aud_adc_get_fifo_addr(&fifo);
		dma_port(&c.src, DMA_DEV_AUDIO_RX, DMA_DATA_WIDTH_32BITS, fifo, fifo + 4);
		dma_port(&c.dst, DMA_DEV_DTCM, DMA_DATA_WIDTH_32BITS, (uint32_t)*buf, (uint32_t)*buf + 2 * AUD_FRAME);
	} else {
		bk_aud_dac_get_fifo_addr(&fifo);
		dma_port(&c.src, DMA_DEV_DTCM, DMA_DATA_WIDTH_32BITS, (uint32_t)*buf, (uint32_t)*buf + 2 * AUD_FRAME);
		dma_port(&c.dst, DMA_DEV_AUDIO, DMA_DATA_WIDTH_16BITS, fifo, fifo + 2);
	}
	if (bk_dma_init(*id, &c) != BK_OK)
		return -3;
	bk_dma_set_transfer_len(*id, AUD_FRAME);
#if (CONFIG_SPE)
	bk_dma_set_dest_sec_attr(*id, DMA_ATTR_SEC);
	bk_dma_set_src_sec_attr(*id, DMA_ATTR_SEC);
#endif
	bk_dma_register_isr(*id, NULL, dir ? adc_dma_isr : dac_dma_isr);
	bk_dma_enable_finish_interrupt(*id);
	return bk_dma_start(*id) == BK_OK ? 0 : -4;
}

static void aud_dma_stop(int dir)
{
	dma_id_t *id = dir ? &s_adc_dma : &s_dac_dma;

	if (*id < DMA_ID_MAX) {
		bk_dma_stop(*id);
		bk_dma_disable_finish_interrupt(*id);
		bk_dma_deinit(*id);
		bk_dma_free(DMA_DEV_AUDIO, *id);
		*id = DMA_ID_MAX;
	}
}

/* producer side: append s16le bytes to the speaker ring, blocking while it is full */
static void spk_ring_write(const uint8_t *p, uint32_t n)
{
	while (n && !s_spk_stop) {
		uint32_t free = SPK_RING - (s_spk_w - s_spk_r), chunk;
		if (!free) {
			rtos_delay_milliseconds(5);
			continue;
		}
		chunk = n < free ? n : free;
		ring_copy_in(s_spk_ring, SPK_RING, s_spk_w % SPK_RING, p, chunk);
		s_spk_w += chunk;
		p += chunk;
		n -= chunk;
	}
}

/* While the voc engine (audio_voc.c) owns the hardware for an RTSP audio session, `spk` and the
 * chime play through its speaker tap instead of the raw DAC path, so a ring during a live call is
 * still heard at the door. */
static bool s_spk_voc;

static void spk_out(const uint8_t *p, uint32_t n)
{
	if (s_spk_voc) {
		if (voc_tap_write(p, n) != 0)
			s_spk_stop = true;
	} else {
		spk_ring_write(p, n);
	}
}

static void spk_task(beken_thread_arg_t arg)
{
	aud_dac_config_t cfg = DEFAULT_AUD_DAC_CONFIG();
	uint8_t buf[512];
	uint32_t i, n, phase = 0, step, total = 0;
	int rc;

	if (s_spk_voc) {
		if (voc_tap_open() != 0) {
			os_printf("[spk] voc tap busy (a /speak is playing): skipped\r\n");
			goto out;
		}
		goto feed;
	}
	cfg.samp_rate = s_dac_rate;
	cfg.clk_src = s_dac_clk;
	cfg.dac_chl = s_dac_chl;
	cfg.work_mode = s_dac_mode;
	cfg.dac_gain = s_dac_gain;
	s_spk_w = s_spk_r = 0;
	s_dac_half = 0;
	s_spk_underrun = s_dac_frames = 0;
	s_spk_feeding = false;   /* the pre-roll below plays zeros on purpose: don't count it as underruns */
	if (bk_aud_dac_init(&cfg) != BK_OK) {
		os_printf("[spk] dac init failed\r\n");
		goto out;
	}
	/* Amplifier pop fix (v0.19): start the DAC + DMA first (the ring is empty, so the DMA streams
	 * zeros = the DAC's idle level), let the analog output settle, and only then enable the PA. */
	if ((rc = aud_dma_start(0)) != 0 || bk_aud_dac_start() != BK_OK) {
		os_printf("[spk] dma/dac start failed (%d)\r\n", rc);
		aud_dma_stop(0);
		bk_aud_dac_deinit();
		pa_set(0);
		goto out;
	}
	rtos_delay_milliseconds(PA_SETTLE_MS);
	pa_set(1);
	rtos_delay_milliseconds(PA_SETTLE_MS);   /* the amplifier's own turn-on transient, on silence */
	s_spk_feeding = true;
feed:
	if (s_spk_net_port) {   /* s16le mono PCM streamed from a TCP peer */
		int sock = tcp_connect(s_net_host, s_spk_net_port), r;
		if (sock < 0)
			os_printf("[spk] connect %s:%u failed\r\n", s_net_host, s_spk_net_port);
		while (sock >= 0 && !s_spk_stop && (r = recv(sock, buf, sizeof(buf), 0)) > 0) {
			spk_out(buf, r);
			total += r / 2;
		}
		if (sock >= 0)
			close(sock);
		s_spk_net_port = 0;
	} else if (s_spk_addr) {   /* s16le mono PCM from flash (stock chimes) */
		for (i = 0; i < s_spk_len && !s_spk_stop; i += n) {
			n = s_spk_len - i < sizeof(buf) ? s_spk_len - i : sizeof(buf);
			bk_flash_read_bytes(s_spk_addr + i, buf, n);
			spk_out(buf, n);
			total += n / 2;
		}
	} else {                   /* tone: 32-step sine, 12-bit phase accumulator */
		step = (s_spk_hz * 32u * 4096u) / s_dac_rate;
		for (i = 0; i < s_spk_ms * (s_dac_rate / 1000) && !s_spk_stop; i += n) {
			n = 0;
			while (n < sizeof(buf) / 2 && i + n < s_spk_ms * (s_dac_rate / 1000)) {
				int16_t v = s_sine32[(phase >> 12) & 31] / 4;
				buf[2 * n] = (uint8_t)v;
				buf[2 * n + 1] = (uint8_t)(v >> 8);
				phase += step;
				n++;
			}
			spk_out(buf, 2 * n);
			total += n;
		}
	}
	if (s_spk_voc) {
		voc_tap_close();
		os_printf("[spk] done via voc tap, %u samples\r\n", total);
		goto out;
	}
	s_spk_feeding = false;
	for (i = 0; i < 100 && s_spk_w != s_spk_r && !s_spk_stop; i++)   /* let the DMA drain the ring */
		rtos_delay_milliseconds(10);
	rtos_delay_milliseconds(2 * AUD_FRAME / 2 * 1000 / s_dac_rate);   /* plus the two halves in flight */
	/* pop fix: the DMA is now streaming zeros again; drop the PA on silence, then stop the DAC */
	pa_set(0);
	rtos_delay_milliseconds(PA_SETTLE_MS);
	aud_dma_stop(0);
	bk_aud_dac_stop();
	bk_aud_dac_deinit();
	os_printf("[spk] done, %u samples, %u dma frames, %u underruns\r\n", total, s_dac_frames, s_spk_underrun);
out:
	s_spk_voc = false;
	s_spk_busy = false;
	s_spk_thread = NULL;
	rtos_delete_thread(NULL);
}

static void spk_start(uint32_t hz, uint32_t ms, uint32_t addr, uint32_t len)
{
	GLOBAL_INT_DECLARATION();

	GLOBAL_INT_DISABLE();
	if (s_spk_busy || (voc_busy() && !voc_active())) {   /* voc starting or stopping: nobody owns the DAC yet */
		GLOBAL_INT_RESTORE();
		REPLY("[spk] busy\r\n");
		return;
	}
	s_spk_busy = true;
	s_spk_voc = voc_active();   /* live RTSP audio session: play through the voc speaker tap */
	GLOBAL_INT_RESTORE();
	s_spk_hz = hz; s_spk_ms = ms; s_spk_addr = addr; s_spk_len = len; s_spk_stop = false;
	if (rtos_create_thread(&s_spk_thread, BEKEN_APPLICATION_PRIORITY, "spk", spk_task, 4096, NULL) != BK_OK) {
		s_spk_busy = false;
		REPLY("[spk] thread failed\r\n");
	}
}

static void cli_spk_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	if (argc >= 2 && os_strcmp(argv[1], "tone") == 0) {
		uint32_t hz = argc >= 3 ? os_strtoul(argv[2], NULL, 10) : 1000;
		uint32_t ms = argc >= 4 ? os_strtoul(argv[3], NULL, 10) : 1000;
		REPLY("[spk] tone %u Hz %u ms\r\n", hz, ms);
		spk_start(hz, ms, 0, 0);
	} else if (argc >= 3 && os_strcmp(argv[1], "play") == 0) {
		/* stock Tuya chimes still in flash (resource FS at 0x600000): raw s16le 8 kHz mono */
		uint32_t addr = 0, len = 0;
		if (os_strcmp(argv[2], "beepa") == 0) { addr = 0x601000; len = 23180; }
		else if (os_strcmp(argv[2], "beepb") == 0) { addr = 0x607000; len = 26104; }
		else if (os_strcmp(argv[2], "bell") == 0) { addr = 0x60e000 + 16; len = 66450 - 16; }
		else if (argc >= 4) { addr = os_strtoul(argv[2], NULL, 16); len = os_strtoul(argv[3], NULL, 10); }
		if (!addr) { REPLY("spk play beepa|beepb|bell|HEXADDR LEN\r\n"); return; }
		REPLY("[spk] play 0x%x len %u\r\n", addr, len);
		spk_start(0, 0, addr, len);
	} else if (argc >= 4 && os_strcmp(argv[1], "net") == 0) {
		if (s_spk_busy) { REPLY("[spk] busy\r\n"); return; }
		os_strncpy(s_net_host, argv[2], sizeof(s_net_host) - 1);
		s_spk_net_port = os_strtoul(argv[3], NULL, 10);
		REPLY("[spk] stream from %s:%u\r\n", s_net_host, s_spk_net_port);
		spk_start(0, 0, 0, 0);
	} else if (argc >= 2 && os_strcmp(argv[1], "stop") == 0) {
		s_spk_stop = true;
		REPLY("[spk] stop requested\r\n");
	} else if (argc >= 3 && os_strcmp(argv[1], "pa") == 0) {
		s_pa_pin = os_strcmp(argv[2], "off") == 0 ? -1 : (int)os_strtoul(argv[2], NULL, 10);
		REPLY("[spk] pa pin %d\r\n", s_pa_pin);
	} else if (argc >= 2 && os_strcmp(argv[1], "cfg") == 0) {
		for (int i = 2; i + 1 < argc; i += 2) {
			if (!os_strcmp(argv[i], "chl"))
				s_dac_chl = !os_strcmp(argv[i + 1], "R") ? AUD_DAC_CHL_R : !os_strcmp(argv[i + 1], "LR") ? AUD_DAC_CHL_LR : AUD_DAC_CHL_L;
			else if (!os_strcmp(argv[i], "mode"))
				s_dac_mode = !os_strcmp(argv[i + 1], "se") ? AUD_DAC_WORK_MODE_SIGNAL_END : AUD_DAC_WORK_MODE_DIFFEN;
			else if (!os_strcmp(argv[i], "gain"))
				s_dac_gain = os_strtoul(argv[i + 1], NULL, 0);
			else if (!os_strcmp(argv[i], "rate"))
				s_dac_rate = os_strtoul(argv[i + 1], NULL, 10);
			else if (!os_strcmp(argv[i], "clk"))
				s_dac_clk = !os_strcmp(argv[i + 1], "apll") ? AUD_CLK_APLL : AUD_CLK_XTAL;
		}
		REPLY("[spk] cfg chl %d mode %d gain 0x%x rate %u clk %s pa %d\r\n", s_dac_chl, s_dac_mode, s_dac_gain, s_dac_rate, s_dac_clk == AUD_CLK_APLL ? "apll" : "xtal", s_pa_pin);
	} else {
		REPLY("spk tone [HZ] [MS] | play beepa|beepb|bell|ADDR LEN | net HOST PORT | stop | pa PIN|off | cfg [chl L|R|LR] [mode diff|se] [gain N] [rate HZ] [clk xtal|apll]  (busy %d)\r\n", s_spk_busy);
	}
}

static void micnet_task(beken_thread_arg_t arg)
{
	aud_adc_config_t cfg = DEFAULT_AUD_ADC_CONFIG();
	uint32_t sent = 0, t0, t1 = 0, idx, n;
	int sock = tcp_connect(s_net_host, s_net_port), rc;

	if (sock < 0) {
		os_printf("[mic] connect %s:%u failed\r\n", s_net_host, s_net_port);
		goto out;
	}
	cfg.samp_rate = s_adc_rate;
	cfg.clk_src = s_adc_clk;
	cfg.adc_chl = s_adc_chl;
	cfg.adc_mode = s_adc_mode;
	cfg.adc_gain = s_adc_gain;
	if (bk_aud_adc_init(&cfg) != BK_OK) {
		os_printf("[mic] adc init failed\r\n");
		close(sock);
		goto out;
	}
	bk_aud_adc_set_mic_mode(s_mic_id, s_adc_mode);
	s_mic_w = s_mic_r = 0;
	s_adc_half = 0;
	s_mic_overrun = s_adc_frames = 0;
	if ((rc = aud_dma_start(1)) != 0 || bk_aud_adc_start() != BK_OK) {
		os_printf("[mic] dma/adc start failed (%d)\r\n", rc);
		aud_dma_stop(1);
		bk_aud_adc_deinit();
		close(sock);
		goto out;
	}
	t0 = rtos_get_time();
	for (;;) {
		bool running = rtos_get_time() - t0 < s_net_ms;
		if (!running && s_adc_dma < DMA_ID_MAX) {   /* time is up: stop capturing, then drain */
			bk_aud_adc_stop();
			aud_dma_stop(1);
			t1 = rtos_get_time();
		}
		if (s_mic_w == s_mic_r) {
			if (!running)
				break;
			rtos_delay_milliseconds(5);
			continue;
		}
		idx = s_mic_r % MIC_RING;
		n = s_mic_w - s_mic_r;
		if (n > MIC_RING - idx)
			n = MIC_RING - idx;
		if (n > 4096)
			n = 4096;
		if (send(sock, &s_mic_ring[idx], n, 0) != (int)n)
			break;
		s_mic_r += n;
		sent += n;
	}
	if (s_adc_dma < DMA_ID_MAX) {
		bk_aud_adc_stop();
		aud_dma_stop(1);
		t1 = rtos_get_time();
	}
	bk_aud_adc_deinit();
	close(sock);
	os_printf("[mic] streamed %u bytes to %s:%u: %u dma frames in %u ms = %u words/s, %u overruns\r\n",
		  sent, s_net_host, s_net_port, s_adc_frames, t1 - t0,
		  (t1 - t0) ? (uint32_t)((uint64_t)s_adc_frames * AUD_FRAME / 4 * 1000 / (t1 - t0)) : 0, s_mic_overrun);
out:
	s_mic_busy = false;
	rtos_delete_thread(NULL);
}

static void cli_mic_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	aud_adc_config_t cfg = DEFAULT_AUD_ADC_CONFIG();
	uint32_t ms = argc >= 3 ? os_strtoul(argv[2], NULL, 10) : 2000, st, w, n = 0, t0;
	uint32_t hz = argc >= 4 ? os_strtoul(argv[3], NULL, 10) : 1000;
	/* the FIFO word carries two 16-bit halves (L low, R high); report both, we don't know which
	 * input the board's electret sits on */
	struct { int32_t peak; uint64_t sq; int64_t sum; float g1, g2; } ch[2] = {{0}};
	/* Goertzel at hz: how much of the signal is a tone (a Mac playing hz into the mic) vs noise */
	float gc = 2.0f * cosf(2.0f * 3.14159265f * (float)hz / (float)AUDIO_RATE), g0;

	if (argc >= 2 && os_strcmp(argv[1], "cfg") == 0) {
		for (int i = 2; i + 1 < argc; i += 2) {
			if (!os_strcmp(argv[i], "mic"))
				s_mic_id = !os_strcmp(argv[i + 1], "2") ? AUD_MIC_MIC2 : !os_strcmp(argv[i + 1], "both") ? AUD_MIC_BOTH : AUD_MIC_MIC1;
			else if (!os_strcmp(argv[i], "mode"))
				s_adc_mode = !os_strcmp(argv[i + 1], "se") ? AUD_ADC_MODE_SIGNAL_END : AUD_ADC_MODE_DIFFEN;
			else if (!os_strcmp(argv[i], "gain"))
				s_adc_gain = os_strtoul(argv[i + 1], NULL, 0);
			else if (!os_strcmp(argv[i], "chl"))
				s_adc_chl = !os_strcmp(argv[i + 1], "R") ? AUD_ADC_CHL_R : !os_strcmp(argv[i + 1], "LR") ? AUD_ADC_CHL_LR : AUD_ADC_CHL_L;
			else if (!os_strcmp(argv[i], "rate"))
				s_adc_rate = os_strtoul(argv[i + 1], NULL, 10);
			else if (!os_strcmp(argv[i], "clk"))
				s_adc_clk = !os_strcmp(argv[i + 1], "apll") ? AUD_CLK_APLL : AUD_CLK_XTAL;
		}
		REPLY("[mic] cfg mic %d mode %d gain 0x%x chl %d rate %u clk %s\r\n", s_mic_id, s_adc_mode, s_adc_gain, s_adc_chl, s_adc_rate, s_adc_clk == AUD_CLK_APLL ? "apll" : "xtal");
		return;
	}
	if (argc >= 5 && os_strcmp(argv[1], "rec") == 0) {
		beken_thread_t t;
		if (s_mic_busy || voc_busy()) { REPLY("[mic] busy%s\r\n", voc_busy() ? " (voc engine active: rtsp audio / talk voc)" : ""); return; }
		s_net_ms = os_strtoul(argv[2], NULL, 10);
		if (s_net_ms > 60000) s_net_ms = 60000;
		os_strncpy(s_net_host, argv[3], sizeof(s_net_host) - 1);
		s_net_port = os_strtoul(argv[4], NULL, 10);
		s_mic_busy = true;
		REPLY("[mic] rec %u ms -> %s:%u (raw 32-bit FIFO words, lo16 = L, hi16 = R)\r\n", s_net_ms, s_net_host, s_net_port);
		if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "micnet", micnet_task, 4096, NULL) != BK_OK) {
			s_mic_busy = false;
			REPLY("[mic] thread failed\r\n");
		}
		return;
	}
	if (argc < 2 || os_strcmp(argv[1], "level") != 0) {
		REPLY("mic rec MS HOST PORT | mic level [MS] [HZ] | cfg [mic 1|2|both] [mode diff|se] [gain N] [chl L|R|LR] [rate HZ] [clk xtal|apll]\r\n");
		return;
	}
	if (s_mic_busy || voc_busy()) {
		REPLY("[mic] busy%s\r\n", voc_busy() ? " (voc engine active)" : "");
		return;
	}
	if (ms > 4000)
		ms = 4000;
	cfg.samp_rate = s_adc_rate;
	cfg.clk_src = s_adc_clk;
	cfg.adc_chl = s_adc_chl;
	cfg.adc_mode = s_adc_mode;
	cfg.adc_gain = s_adc_gain;
	if (bk_aud_adc_init(&cfg) != BK_OK) {
		REPLY("[mic] adc init failed\r\n");
		return;
	}
	bk_aud_adc_set_mic_mode(s_mic_id, s_adc_mode);
	if (bk_aud_adc_start() != BK_OK) {
		REPLY("[mic] adc start failed\r\n");
		bk_aud_adc_deinit();
		return;
	}
	t0 = rtos_get_time();
	while (rtos_get_time() - t0 < ms) {
		bk_aud_adc_get_status(&st);
		if (st & AUD_ADCL_FIFO_EMPTY_MASK) {
			rtos_delay_milliseconds(1);
			continue;
		}
		bk_aud_adc_get_fifo_data(&w);
		n++;
		for (int c = 0; c < 2; c++) {
			int16_t v = (int16_t)(c ? (w >> 16) : (w & 0xffff));
			ch[c].sum += v;
			ch[c].sq += (int64_t)v * v;
			if (v > ch[c].peak) ch[c].peak = v;
			if (-v > ch[c].peak) ch[c].peak = -v;
			g0 = (float)v + gc * ch[c].g1 - ch[c].g2;
			ch[c].g2 = ch[c].g1;
			ch[c].g1 = g0;
		}
	}
	bk_aud_adc_stop();
	bk_aud_adc_deinit();
	if (n) {
		REPLY("[mic] %u samples in %u ms", n, ms);
		for (int c = 0; c < 2; c++) {
			int32_t mean = (int32_t)(ch[c].sum / (int64_t)n);
			uint32_t rms = 0, x = (uint32_t)(ch[c].sq / n);
			for (uint32_t b = 1u << 15; b; b >>= 1)   /* integer sqrt */
				if ((rms + b) * (rms + b) <= x) rms += b;
			float gm = sqrtf(ch[c].g1 * ch[c].g1 + ch[c].g2 * ch[c].g2 - gc * ch[c].g1 * ch[c].g2) / (float)n;
			REPLY(" | %s: peak %d rms %u dc %d tone%u %d", c ? "hi16" : "lo16", ch[c].peak, rms, mean, hz, (int)gm);
		}
		REPLY("\r\n");
	} else {
		REPLY("[mic] no samples (FIFO never filled)\r\n");
	}
}

/* ---- engine API for talk.c (aud_engine.h): the same DAC/ADC + DMA rings as `spk`/`mic`, but opened
 * and closed by the caller's thread (the HTTP client thread) instead of spk_task / micnet_task.
 * Mutual exclusion is the existing s_spk_busy / s_mic_busy, so the ring chime, `spk tone`, `mic rec`
 * and talk never drive the hardware at the same time. ---- */
/* While the voc engine runs (RTSP audio session / `talk voc on`), the talk.c users go through its
 * taps: /speak -> voc speaker tap (A-law encoded, AEC reference included), /mic.wav -> voc mic tap
 * (decoded, 8 kHz). s_spk_busy / s_mic_busy are set either way, so the chime, `spk` and `mic` see
 * the same exclusion as before. */
static bool s_talk_spk_voc, s_talk_mic_voc;

bool aud_raw_busy(void) { return s_spk_busy || s_mic_busy; }
void aud_pa_set(int on) { pa_set(on); }

int aud_spk_open(uint16_t gain)
{
	aud_dac_config_t cfg = DEFAULT_AUD_DAC_CONFIG();
	int rc;
	GLOBAL_INT_DECLARATION();

	GLOBAL_INT_DISABLE();
	if (s_spk_busy || (voc_busy() && !voc_active())) {
		GLOBAL_INT_RESTORE();
		return -1;
	}
	s_spk_busy = true;
	s_talk_spk_voc = voc_active();
	GLOBAL_INT_RESTORE();
	s_spk_stop = false;
	if (s_talk_spk_voc) {
		if (voc_tap_open() != 0) {
			s_spk_busy = false;
			return -1;
		}
		if (gain)
			voc_set_spk_gain(gain);
		return 0;
	}
	cfg.samp_rate = s_dac_rate;
	cfg.clk_src = s_dac_clk;
	cfg.dac_chl = s_dac_chl;
	cfg.work_mode = s_dac_mode;
	cfg.dac_gain = gain ? gain : s_dac_gain;
	s_spk_w = s_spk_r = 0;
	s_dac_half = 0;
	s_spk_underrun = s_dac_frames = 0;
	s_spk_feeding = true;
	if (bk_aud_dac_init(&cfg) != BK_OK) {
		s_spk_busy = false;
		return -2;
	}
	if (!s_dac_dma_buf)
		s_dac_dma_buf = os_malloc(2 * AUD_FRAME);
	if (s_dac_dma_buf)
		os_memset(s_dac_dma_buf, 0, 2 * AUD_FRAME);   /* no stale audio from the last playback before the PA comes on */
	if ((rc = aud_dma_start(0)) != 0 || bk_aud_dac_start() != BK_OK) {
		os_printf("[talk] dma/dac start failed (%d)\r\n", rc);
		aud_dma_stop(0);
		bk_aud_dac_deinit();
		s_spk_busy = false;
		return -3;
	}
	rtos_delay_milliseconds(20);   /* DAC runs on silence first, then the amplifier: softer pop */
	pa_set(1);
	return 0;
}

int aud_spk_write(const uint8_t *s16le, uint32_t bytes)
{
	if (s_talk_spk_voc)
		return voc_tap_write(s16le, bytes) != 0 || s_spk_stop ? -1 : 0;
	spk_ring_write(s16le, bytes);
	return s_spk_stop ? -1 : 0;
}

void aud_spk_close(void)
{
	uint32_t i;

	if (s_talk_spk_voc) {
		voc_tap_close();
		voc_set_spk_gain(s_dac_gain);   /* back to the session's gain */
		s_talk_spk_voc = false;
		s_spk_busy = false;
		return;
	}
	s_spk_feeding = false;
	for (i = 0; i < 250 && s_spk_w != s_spk_r && !s_spk_stop; i++)   /* drain the ring (<= 1 s of audio) */
		rtos_delay_milliseconds(10);
	rtos_delay_milliseconds(2 * AUD_FRAME / 2 * 1000 / s_dac_rate);   /* plus the two DMA halves in flight */
	pa_set(0);                     /* amplifier off before the DAC stops: no stop transient on the speaker */
	rtos_delay_milliseconds(20);
	aud_dma_stop(0);
	bk_aud_dac_stop();
	bk_aud_dac_deinit();
	s_spk_busy = false;
}

bool aud_spk_busy(void) { return s_spk_busy; }
uint32_t aud_spk_rate(void) { return s_dac_rate; }
uint16_t aud_spk_gain(void) { return s_dac_gain; }
void aud_spk_stats(uint32_t *dma_frames, uint32_t *underruns)
{
	if (s_talk_spk_voc) {
		voc_tap_stats(dma_frames, underruns);
		return;
	}
	*dma_frames = s_dac_frames;
	*underruns = s_spk_underrun;
}

int aud_mic_open(uint32_t gain)
{
	aud_adc_config_t cfg = DEFAULT_AUD_ADC_CONFIG();
	int rc;
	GLOBAL_INT_DECLARATION();

	GLOBAL_INT_DISABLE();
	if (s_mic_busy || (voc_busy() && !voc_active())) {
		GLOBAL_INT_RESTORE();
		return -1;
	}
	s_mic_busy = true;
	s_talk_mic_voc = voc_active();
	GLOBAL_INT_RESTORE();
	if (s_talk_mic_voc) {
		if (voc_mic_tap_open() != 0) {
			s_mic_busy = false;
			return -1;
		}
		return 0;
	}
	cfg.samp_rate = s_adc_rate;
	cfg.clk_src = s_adc_clk;
	cfg.adc_chl = s_adc_chl;
	cfg.adc_mode = s_adc_mode;
	cfg.adc_gain = gain ? gain : s_adc_gain;
	if (bk_aud_adc_init(&cfg) != BK_OK) {
		s_mic_busy = false;
		return -2;
	}
	bk_aud_adc_set_mic_mode(s_mic_id, s_adc_mode);
	s_mic_w = s_mic_r = 0;
	s_adc_half = 0;
	s_mic_overrun = s_adc_frames = 0;
	if ((rc = aud_dma_start(1)) != 0 || bk_aud_adc_start() != BK_OK) {
		os_printf("[talk] dma/adc start failed (%d)\r\n", rc);
		aud_dma_stop(1);
		bk_aud_adc_deinit();
		s_mic_busy = false;
		return -3;
	}
	return 0;
}

uint32_t aud_mic_read(uint8_t *dst, uint32_t cap)
{
	uint32_t n = s_mic_w - s_mic_r;

	if (s_talk_mic_voc)
		return voc_mic_tap_read(dst, cap);
	if (n > cap)
		n = cap;
	n &= ~3u;   /* whole 32-bit FIFO words */
	if (!n)
		return 0;
	ring_copy_out(dst, s_mic_ring, MIC_RING, s_mic_r % MIC_RING, n);
	s_mic_r += n;
	return n;
}

void aud_mic_close(void)
{
	if (s_talk_mic_voc) {
		voc_mic_tap_close();
		s_talk_mic_voc = false;
		s_mic_busy = false;
		return;
	}
	bk_aud_adc_stop();
	aud_dma_stop(1);
	bk_aud_adc_deinit();
	s_mic_busy = false;
}

bool aud_mic_busy(void) { return s_mic_busy; }
/* raw: measured 2026-10-03, the ADC delivers twice the configured rate; voc tap: decoded 8 kHz */
uint32_t aud_mic_rate(void) { return s_talk_mic_voc || (!s_mic_busy && voc_active()) ? VOC_RATE : 2 * s_adc_rate; }
void aud_mic_stats(uint32_t *dma_frames, uint32_t *overruns)
{
	if (s_talk_mic_voc) {
		voc_mic_tap_stats(dma_frames, overruns);
		return;
	}
	*dma_frames = s_adc_frames;
	*overruns = s_mic_overrun;
}

/* ---- TCP command channel: the same commands as the UART shell, over the LAN ----
 * One line per connection: "<token> <command...>\n". The token is the KV string db_token
 * (set once over serial: `tok set SECRET`); without a stored token the channel is disabled.
 * Replies are "ok", "denied" or "unknown"; command output still goes to UART0. */
#define KV_TOKEN "db_token"
#define TCP_CMD_PORT 2323
static char s_token[48];
static const struct cli_command s_doorbell_commands[];
static unsigned s_doorbell_command_count;

static int run_line(char *line)
{
	char *argv[12];
	int argc = 0;
	unsigned i;

	for (char *t = strtok(line, " \t\r\n"); t && argc < 12; t = strtok(NULL, " \t\r\n"))
		argv[argc++] = t;
	if (!argc)
		return -1;
	for (i = 0; i < s_doorbell_command_count; i++)
		if (os_strcmp(argv[0], s_doorbell_commands[i].name) == 0) {
			s_doorbell_commands[i].function(NULL, 0, argc, argv);
			return 0;
		}
	return -1;
}

static void tcp_cmd_task(beken_thread_arg_t arg)
{
	struct sockaddr_in sa = {0};
	int ls, cs, n, one = 1;
	char line[192], *cmd;
	struct timeval tv = {.tv_sec = 5, .tv_usec = 0};

	ls = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(TCP_CMD_PORT);
	sa.sin_addr.s_addr = INADDR_ANY;
	if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(ls, 1) < 0) {
		os_printf("[tcp] bind/listen failed\r\n");
		close(ls);
		rtos_delete_thread(NULL);
		return;
	}
	os_printf("[tcp] command channel on port %d\r\n", TCP_CMD_PORT);
	while (1) {
		cs = accept(ls, NULL, NULL);
		if (cs < 0)
			continue;
		setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		n = recv(cs, line, sizeof(line) - 1, 0);
		if (n > 0) {
			line[n] = 0;
			cmd = os_strchr(line, ' ');
			if (!s_token[0] || !cmd || os_strncmp(line, s_token, cmd - line) != 0
			    || os_strlen(s_token) != (size_t)(cmd - line)) {
				send(cs, "denied\n", 7, 0);
			} else {
				cmd++;
				os_printf("[tcp] %s\r\n", cmd);
				power_activity();
				s_reply[0] = 0;
				s_reply_on = true;
				if (run_line(cmd) == 0)
					send(cs, "ok\n", 3, 0);
				else
					send(cs, "unknown\n", 8, 0);
				s_reply_on = false;
				if (s_reply[0])
					send(cs, s_reply, os_strlen(s_reply), 0);
			}
		}
		rtos_delay_milliseconds(100);
		close(cs);
	}
}

#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
/* ---- camera (AVDK build only): DVP sensor -> hardware JPEG on CPU1, frames delivered here by the
 * media transfer task; we keep the latest JPEG in PSRAM and can push it to a TCP peer ---- */
/* CPU1 never emits a JPEG larger than its own frame buffer (CONFIG_JPEG_FRAME_SIZE = 150 KB). The
 * CPU0 PSRAM heap is only 512 KB and also holds one such buffer per HTTP client plus the 128 KB
 * RTSP ring, so size this exactly. */
#ifdef CONFIG_JPEG_FRAME_SIZE
#define CAM_BUF_MAX (CONFIG_JPEG_FRAME_SIZE)
#else
#define CAM_BUF_MAX (150 * 1024)
#endif
static uint8_t *s_cam_buf;
static volatile uint32_t s_cam_len, s_cam_frames, s_cam_seq, s_cam_w, s_cam_h, s_cam_drop, s_cam_last_ms;
static beken_mutex_t s_cam_mutex;

/* ---- encoder mode (v0.19.0): the DVP pipeline runs ONE hardware encoder at a time, JPEG (HTTP
 * snapshot/MJPEG) or H.264 (RTSP). Who gets it is decided by `cam mode` (KV db_cam_mode):
 *   auto (default): consumer driven. An RTSP client switches the camera to H.264; an HTTP request
 *                   switches it back to JPEG unless an RTSP client is playing (then the snapshot
 *                   is the last JPEG taken before the switch, /stream answers 503).
 *   jpeg:           never H.264 (RTSP DESCRIBE answers 503)   - the pre-0.19 behaviour.
 *   h264:           never JPEG (HTTP serves the stale JPEG / 503); use the RTSP stream in HA.
 * A switch costs a close + open (~0.5 s) and the first frame of the new mode (~1 s). ---- */
enum { CAM_PREF_AUTO = 0, CAM_PREF_JPEG = 1, CAM_PREF_H264 = 2 };
static int s_cam_pref = CAM_PREF_AUTO;
static bool s_cam_h264;                      /* mode of the open camera */
static volatile uint32_t s_cam_h264_frames;  /* H.264 access units since the last open */
static beken_mutex_t s_cam_ctl;              /* serialises open/close/switch (init in user_app_main) */
/* H.264 rate control (KV db_h264_q "IMB PMB"): target bits per 16x16 macroblock for I and P
 * frames. 720p = 3600 MB, GOP = 30 frames (CONFIG_H264_P_FRAME_CNT=29 on CPU1 = IDR every 2 s at
 * 15 fps): 120/24 ~ 54 KB I + 11 KB P = ~1.5 Mbit/s nominal, less on a static scene. The SDK's own
 * preset (quality level 2: 160/60) is ~3.5 Mbit/s. Keep IMB <= 290 (I-frame must fit the 128 KB
 * CPU1 buffer). */
static uint32_t s_h264_imb = 120, s_h264_pmb = 24;
static bool s_h264_q_pending;                /* apply after the first frame (the SDK preset is applied there) */
static uint16_t s_rtsp_port = RTSP_DEFAULT_PORT;
static bool s_rtsp_on;                       /* KV db_rtsp, default off */
#define KV_CAM_MODE "db_cam_mode"
#define KV_H264_Q "db_h264_q"
#define KV_RTSP "db_rtsp"
#define KV_RTSP_PORT "db_rtsp_port"

static void cam_frame_cb(frame_buffer_t *frame)
{
	if (!frame || !frame->frame || frame->length == 0)
		return;
	if (frame->fmt == PIXEL_FMT_H264) {   /* H.264 access unit (Annex-B) -> RTSP ring; nothing else wants it */
		s_cam_h264_frames++;
		s_cam_frames++;
		s_cam_last_ms = rtos_get_time();
		rtsp_feed_frame(frame->frame, frame->length, frame->timestamp);
		return;
	}
	if (frame->length > CAM_BUF_MAX || !s_cam_buf)
		return;
	if (rtos_trylock_mutex(&s_cam_mutex) != BK_OK) {   /* a snapshot is being read out: skip this frame */
		s_cam_drop++;
		return;
	}
	os_memcpy(s_cam_buf, frame->frame, frame->length);
	s_cam_len = frame->length;
	s_cam_seq = frame->sequence;
	s_cam_w = frame->width;
	s_cam_h = frame->height;
	s_cam_frames++;
	s_cam_last_ms = rtos_get_time();
	rtos_unlock_mutex(&s_cam_mutex);
}

static void camsnap_task(beken_thread_arg_t arg)
{
	int sock = tcp_connect(s_net_host, s_net_port);
	uint32_t len = 0, off = 0;
	int n;

	if (sock < 0) {
		os_printf("[cam] connect %s:%u failed\r\n", s_net_host, s_net_port);
		goto out;
	}
	rtos_lock_mutex(&s_cam_mutex);
	len = s_cam_len;
	while (off < len && (n = send(sock, s_cam_buf + off, len - off > 4096 ? 4096 : len - off, 0)) > 0)
		off += n;
	rtos_unlock_mutex(&s_cam_mutex);
	close(sock);
	os_printf("[cam] snapshot %u/%u bytes -> %s:%u\r\n", off, len, s_net_host, s_net_port);
out:
	rtos_delete_thread(NULL);
}


#if CONFIG_DVP_CAMERA
/* cam probe [PWR_PIN ...]: power the sensor rails (default GPIO 9 + 28), run MCLK, report the idle
 * level of every candidate I2C pin (pull-ups present = 1), then scan I2C0 (20/21) and I2C1 on the
 * pin groups 42/43 and 14/15 for ACKs. GPIO 0/1 (the console UART) are left alone. Runs in its own
 * thread; results go to the UART log and the next `cam probe` reply buffer is not used. */
static int s_probe_pins[8], s_probe_n;

static int i2c_scan(i2c_id_t id, const char *label)
{
	i2c_config_t cfg = {.baud_rate = I2C_BAUD_RATE_100KHZ, .addr_mode = I2C_ADDR_MODE_7BIT};
	uint8_t b;
	int found = 0;
	char line[160];
	int l = snprintf(line, sizeof(line), "[cam] %s:", label);

	bk_i2c_init(id, &cfg);
	for (uint32_t a = 0x08; a < 0x78; a++) {
		i2c_mem_param_t mp = {.dev_addr = a, .mem_addr = 0x00, .mem_addr_size = I2C_MEM_ADDR_SIZE_8BIT, .data = &b, .data_size = 1, .timeout_ms = 5};
		if (bk_i2c_memory_read(id, &mp) == BK_OK && l < (int)sizeof(line) - 8) {
			l += snprintf(line + l, sizeof(line) - l, " 0x%02x", a);
			found++;
		}
	}
	bk_i2c_deinit(id);
	os_printf("%s%s\r\n", line, found ? "" : " none");
	return found;
}

static const int s_i2c_cand[6] = {20, 21, 42, 43, 14, 15};   /* I2C0, I2C1 group 42/43, I2C1 group 14/15 */

static void cam_idle_levels(int *lv)
{
	for (int i = 0; i < 6; i++) {
		pin_configure(s_i2c_cand[i], GPIO_INPUT_ENABLE, GPIO_PULL_DISABLE);
		lv[i] = bk_gpio_get_input(s_i2c_cand[i]);
	}
}

static void cam_probe_task(beken_thread_arg_t arg)
{
	int lv[6];

	for (int i = 0; i < s_probe_n; i++) {
		pin_configure(s_probe_pins[i], GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		bk_gpio_set_output_high(s_probe_pins[i]);
	}
	rtos_delay_milliseconds(50);
	bk_video_gpio_init(DVP_GPIO_CLK);
	bk_video_dvp_mclk_enable(YUV_MODE);
	rtos_delay_milliseconds(50);
	cam_idle_levels(lv);
	os_printf("[cam] probe: %d rail pins high, MCLK on; idle 20=%d 21=%d 42=%d 43=%d 14=%d 15=%d\r\n",
		  s_probe_n, lv[0], lv[1], lv[2], lv[3], lv[4], lv[5]);
	/* only scan a pair that shows pull-ups on both lines: a floating bus hangs the I2C engine */
	if (lv[0] && lv[1])
		i2c_scan(I2C_ID_0, "I2C0 (20/21)");
	if (lv[2] && lv[3]) {
		gpio_dev_unmap(42); gpio_dev_map(42, GPIO_DEV_I2C1_SCL);
		gpio_dev_unmap(43); gpio_dev_map(43, GPIO_DEV_I2C1_SDA);
		i2c_scan(I2C_ID_1, "I2C1 (42/43)");
		gpio_dev_unmap(42); gpio_dev_unmap(43);
	}
	if (lv[4] && lv[5]) {
		gpio_dev_unmap(14); gpio_dev_map(14, GPIO_DEV_I2C1_SCL);
		gpio_dev_unmap(15); gpio_dev_map(15, GPIO_DEV_I2C1_SDA);
		i2c_scan(I2C_ID_1, "I2C1 (14/15)");
		gpio_dev_unmap(14); gpio_dev_unmap(15);
	}
	bk_video_dvp_mclk_disable();
	os_printf("[cam] probe done\r\n");
	rtos_delete_thread(NULL);
}

/* cam pwrscan: drive every free GPIO high, then low, and watch the I2C candidate pins for pull-ups
 * appearing (the sensor board's pull-ups sit on its own supply): finds the camera power switch and
 * the I2C pair together, with no I2C traffic. GPIO 9 and 28 stay high throughout. */
static void cam_pwrscan_task(beken_thread_arg_t arg)
{
	static const int sweep[] = {2, 3, 4, 5, 6, 8, 12, 13, 16, 17, 18, 19, 22, 23, 25, 26, 40, 41, 47, 48, 49, 50, 51, 52, 54, 55};
	int lv[6], base[6];

	pin_configure(9, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE); bk_gpio_set_output_high(9);
	pin_configure(28, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE); bk_gpio_set_output_high(28);
	bk_video_gpio_init(DVP_GPIO_CLK);
	bk_video_dvp_mclk_enable(YUV_MODE);
	rtos_delay_milliseconds(50);
	cam_idle_levels(base);
	os_printf("[cam] pwrscan baseline 20=%d 21=%d 42=%d 43=%d 14=%d 15=%d\r\n", base[0], base[1], base[2], base[3], base[4], base[5]);
	for (unsigned i = 0; i < sizeof(sweep) / sizeof(sweep[0]); i++) {
		int p = sweep[i];
		for (int level = 1; level >= 0; level--) {
			pin_configure(p, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
			if (level) bk_gpio_set_output_high(p); else bk_gpio_set_output_low(p);
			rtos_delay_milliseconds(80);
			cam_idle_levels(lv);
			if (os_memcmp(lv, base, sizeof(lv)))
				os_printf("[cam] pwrscan GPIO %d=%d -> 20=%d 21=%d 42=%d 43=%d 14=%d 15=%d  <==\r\n", p, level, lv[0], lv[1], lv[2], lv[3], lv[4], lv[5]);
		}
		pin_configure(p, GPIO_INPUT_ENABLE, GPIO_PULL_DISABLE);   /* back to hi-Z */
		rtos_delay_milliseconds(20);
	}
	bk_video_dvp_mclk_disable();
	os_printf("[cam] pwrscan done\r\n");
	rtos_delete_thread(NULL);
}

/* ---- software (bit-banged) I2C on any two GPIOs: safe probe of the sensor bus (the SDK's I2C1
 * driver is pinned to GPIO 0/1 and resets this board). Open-drain emulated: 1 = input + pull-up,
 * 0 = driven low. Slow (~10 kHz) but every sensor tolerates that. ---- */
static int s_bb_scl = 42, s_bb_sda = 43;
static void bb_wait(void) { for (volatile int i = 0; i < 300; i++) ; }
static void bb_set(int pin, int v)
{
	if (v) {
		pin_configure(pin, GPIO_INPUT_ENABLE, GPIO_PULL_UP_EN);
	} else {
		pin_configure(pin, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		bk_gpio_set_output_low(pin);
	}
}
static void bb_start(void) { bb_set(s_bb_sda, 1); bb_set(s_bb_scl, 1); bb_wait(); bb_set(s_bb_sda, 0); bb_wait(); bb_set(s_bb_scl, 0); bb_wait(); }
static void bb_stop(void) { bb_set(s_bb_sda, 0); bb_wait(); bb_set(s_bb_scl, 1); bb_wait(); bb_set(s_bb_sda, 1); bb_wait(); }
static int bb_write(uint8_t b)   /* returns 1 on ACK */
{
	int ack;
	for (int i = 7; i >= 0; i--) {
		bb_set(s_bb_sda, (b >> i) & 1); bb_wait();
		bb_set(s_bb_scl, 1); bb_wait();
		bb_set(s_bb_scl, 0); bb_wait();
	}
	bb_set(s_bb_sda, 1); bb_wait();
	bb_set(s_bb_scl, 1); bb_wait();
	ack = !bk_gpio_get_input(s_bb_sda);
	bb_set(s_bb_scl, 0); bb_wait();
	return ack;
}
static uint8_t bb_read(int ack)
{
	uint8_t b = 0;
	bb_set(s_bb_sda, 1);
	for (int i = 7; i >= 0; i--) {
		bb_wait(); bb_set(s_bb_scl, 1); bb_wait();
		b |= (bk_gpio_get_input(s_bb_sda) & 1) << i;
		bb_set(s_bb_scl, 0);
	}
	bb_set(s_bb_sda, ack ? 0 : 1); bb_wait();
	bb_set(s_bb_scl, 1); bb_wait();
	bb_set(s_bb_scl, 0); bb_wait();
	bb_set(s_bb_sda, 1);
	return b;
}
static int bb_reg_read(uint8_t addr, uint32_t reg, int reg16, uint8_t *out)
{
	int ok;
	bb_start();
	ok = bb_write(addr << 1);
	if (ok && reg16) ok = bb_write(reg >> 8);
	if (ok) ok = bb_write(reg & 0xff);
	if (ok) { bb_stop(); bb_start(); ok = bb_write((addr << 1) | 1); }   /* SCCB: stop, then a fresh start */
	if (ok) *out = bb_read(0);
	bb_stop();
	return ok;
}

static void cam_bb_task(beken_thread_arg_t arg)
{
	uint8_t v, found[8];
	int n = 0;
	char line[160];
	int l;

	bk_video_gpio_init(DVP_GPIO_CLK);
	bk_video_dvp_mclk_enable(YUV_MODE);
	rtos_delay_milliseconds(30);
	bb_set(s_bb_scl, 1); bb_set(s_bb_sda, 1); bb_wait();
	l = snprintf(line, sizeof(line), "[cam] bb scan SCL=%d SDA=%d (idle scl=%d sda=%d):", s_bb_scl, s_bb_sda,
		     bk_gpio_get_input(s_bb_scl), bk_gpio_get_input(s_bb_sda));
	for (uint32_t a = 0x08; a < 0x78; a++) {
		int ack;
		bb_start(); ack = bb_write(a << 1); bb_stop();
		if (ack) {
			if (l < (int)sizeof(line) - 8) l += snprintf(line + l, sizeof(line) - l, " 0x%02x", a);
			if (n < 8) found[n++] = a;
		}
	}
	os_printf("%s%s\r\n", line, n ? "" : " none");
	for (int i = 0; i < n && n < 20; i++) {   /* a real device answers at one or two addresses, not all */
		uint8_t a = found[i];
		l = snprintf(line, sizeof(line), "[cam] 0x%02x:", a);
		static const struct { const char *name; uint32_t reg; int r16; } ids[] = {
			{"f0", 0xf0, 0}, {"f1", 0xf1, 0}, {"00", 0x00, 0}, {"0a", 0x0a, 0}, {"0b", 0x0b, 0}, {"3107", 0x3107, 1}, {"3108", 0x3108, 1}, {"0001", 0x0001, 1}, {"0002", 0x0002, 1}};
		for (unsigned k = 0; k < sizeof(ids) / sizeof(ids[0]); k++)
			if (bb_reg_read(a, ids[k].reg, ids[k].r16, &v)) l += snprintf(line + l, sizeof(line) - l, " r%s=%02x", ids[k].name, v);
		os_printf("%s\r\n", line);
	}
	bb_set(s_bb_scl, 1); bb_set(s_bb_sda, 1);
	bk_video_dvp_mclk_disable();
	os_printf("[cam] bb done\r\n");
	rtos_delete_thread(NULL);
}

/* GPIO 6 used to be in this list as a "camera LDO enable": it is the microSD card-detect input
 * (stock firmware, verified 2026-10-04) and belongs to sdcard.c now. */
static void cam_rails(int on)
{
	static const int rails[] = {9, 28, 5};
	for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
		pin_configure(rails[i], GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		if (on) bk_gpio_set_output_high(rails[i]); else bk_gpio_set_output_low(rails[i]);
	}
}

/* The camera's own supplies: GPIO 28 (sensor power, also feeds the LDR divider) and the LDO
 * enable 5. Dropped at every close so the next open is a real power-on reset of the OV9650:
 * a close followed by a reopen within a few seconds otherwise leaves the sensor silent (no
 * frames, "camera wedge", 2026-10-03) and only a rail cycle or a reboot brings it back. GPIO 9
 * (light sensor / LED rail) is left alone. */
static uint32_t s_cam_off_ms, s_cam_open_ms;
static void cam_power(int on)
{
	static const int pins[] = {28, 5};
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		pin_configure(pins[i], GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		if (on) bk_gpio_set_output_high(pins[i]); else bk_gpio_set_output_low(pins[i]);
	}
	if (!on)
		s_cam_off_ms = rtos_get_time();
}

static void cli_cam_probe(int argc, char **argv)
{
	beken_thread_t t;

	s_probe_pins[0] = 9; s_probe_pins[1] = 28; s_probe_n = 2;
	for (int i = 2; i < argc && s_probe_n < 8; i++)
		s_probe_pins[s_probe_n++] = os_strtoul(argv[i], NULL, 10);
	if (argc >= 3 && os_strcmp(argv[2], "rails") == 0) {   /* cam probe rails on|off : GPIO 9, 28, 5, 6 */
		cam_rails(argc >= 4 && os_strcmp(argv[3], "on") == 0);
		REPLY("[cam] rails 9/28/5/6 %s\r\n", argc >= 4 && os_strcmp(argv[3], "on") == 0 ? "high" : "low");
		return;
	}
	if (argc >= 5 && os_strcmp(argv[2], "bb") == 0) {   /* cam probe bb SCL SDA : software I2C scan */
		s_bb_scl = os_strtoul(argv[3], NULL, 10);
		s_bb_sda = os_strtoul(argv[4], NULL, 10);
		REPLY("[cam] bb scan on SCL=%d SDA=%d started, see UART\r\n", s_bb_scl, s_bb_sda);
		if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "cambb", cam_bb_task, 4096, NULL) != BK_OK)
			REPLY("[cam] thread failed\r\n");
		return;
	}
	if (argc >= 3 && os_strcmp(argv[2], "scan") == 0) {   /* cam probe scan = power-pin sweep */
		REPLY("[cam] pwrscan started, see UART (~6 s)\r\n");
		if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "campwr", cam_pwrscan_task, 4096, NULL) != BK_OK)
			REPLY("[cam] thread failed\r\n");
		return;
	}
	REPLY("[cam] probe started (%d rail pins), see UART\r\n", s_probe_n);
	if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "camprobe", cam_probe_task, 4096, NULL) != BK_OK)
		REPLY("[cam] thread failed\r\n");
}
#endif /* CONFIG_DVP_CAMERA */

/* Open the sensor in JPEG or H.264 mode (1280x720 only for H.264: the encoder needs height % 16
 * == 0 and the OV9650 table has 720p). Caller holds s_cam_ctl. */
static int cam_open_locked(uint32_t w, uint32_t h, bool h264)
{
	media_camera_device_t dev = {0};
	int ret;

	if (s_cam_open)
		return s_cam_h264 == h264 ? BK_OK : BK_ERR_STATE;
	if (!s_cam_buf)
		s_cam_buf = psram_malloc(CAM_BUF_MAX);
	if (!s_cam_buf)
		return BK_ERR_NO_MEM;
	if (!s_cam_mutex)
		rtos_init_mutex(&s_cam_mutex);
#if CONFIG_DVP_CAMERA
	if (s_cam_off_ms) {   /* a real power-off: 250 ms left 3/8 cold opens silent or undetected */
		uint32_t off = rtos_get_time() - s_cam_off_ms;
		if (off < 800)
			rtos_delay_milliseconds(800 - off);
	}
	cam_rails(1);   /* GPIO 9, 28 (camera power, active high per the stock firmware), 5 */
	rtos_delay_milliseconds(150);
	/* stock bk_dvp_camera_enumerate: GPIO 52 low 100 ms, high 100 ms, low (sensor PWDN/RESET pulse) */
	pin_configure(52, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	bk_gpio_set_output_low(52);
	rtos_delay_milliseconds(100);
	bk_gpio_set_output_high(52);
	rtos_delay_milliseconds(100);
	bk_gpio_set_output_low(52);
	rtos_delay_milliseconds(50);
#endif
	dev.type = DVP_CAMERA;
	dev.mode = h264 ? H264_MODE : JPEG_MODE;
	dev.fmt = h264 ? PIXEL_FMT_H264 : PIXEL_FMT_JPEG;
	dev.info.resolution.width = w;
	dev.info.resolution.height = h;
	dev.info.fps = FPS15;
	s_cam_h264_frames = 0;
	ret = media_app_camera_open(&dev);
	if (ret != BK_OK) {
#if CONFIG_DVP_CAMERA
		cam_power(0);   /* undetected cold sensor: make the retry a real power cycle */
#endif
		return ret;
	}
	/* one registration at a time: the format selects which CPU1 frame queue is forwarded */
	ret = media_app_register_read_frame_callback(dev.fmt, cam_frame_cb);
	s_cam_open = (ret == BK_OK);
	s_cam_h264 = s_cam_open && h264;
	s_h264_q_pending = s_cam_h264;
	s_cam_open_ms = rtos_get_time();
	s_cam_last_ms = s_cam_open_ms;
	os_printf("[cam] open %ux%u %s -> %d\r\n", w, h, h264 ? "h264" : "jpeg", ret);
	return ret;
}

/* Caller holds s_cam_ctl. The last JPEG stays in s_cam_buf (stale snapshot while in H.264). */
static void cam_close_locked(void)
{
	int ret;

	media_app_unregister_read_frame_callback();
	ret = media_app_camera_close(DVP_CAMERA);
	os_printf("[cam] close (%s) -> %d\r\n", s_cam_h264 ? "h264" : "jpeg", ret);
	s_cam_open = false;
	s_cam_h264 = false;
	s_h264_q_pending = false;
#if CONFIG_DVP_CAMERA
	cam_power(0);   /* power-on reset at the next open, see cam_power() */
#endif
}

/* Push our H.264 rate-control targets to the CPU1 encoder. Only valid once the first frame is
 * out: bk_h264_init() queues the SDK preset and the encoder ISR applies it at the first frame,
 * and bk_h264_set_base_config() refuses while that is pending. */
static int cam_h264_apply_q(void)
{
	compress_ratio_t r = {0};
	int ret = BK_FAIL, tries;

	if (!s_cam_open || !s_cam_h264)
		return BK_ERR_STATE;
	r.mode = H264_MODE;
	r.enable = 1;
	r.qp.init_qp = 30;
	r.qp.i_min_qp = 10;
	r.qp.i_max_qp = 45;
	r.qp.p_min_qp = 10;
	r.qp.p_max_qp = 45;
	r.imb_bits = s_h264_imb;
	r.pmb_bits = s_h264_pmb;
	for (tries = 0; tries < 3 && ret != BK_OK; tries++) {
		ret = media_app_set_compression_ratio(&r);
		if (ret != BK_OK)
			rtos_delay_milliseconds(150);
	}
	os_printf("[cam] h264 q imb %u pmb %u -> %d\r\n", s_h264_imb, s_h264_pmb, ret);
	return ret;
}

/* Caller holds s_cam_ctl. A cold OV9650 sometimes comes up silent (open -> 0, never a frame):
 * treat "open for 4 s and no frame for 4 s" as stalled and power-cycle it before use. */
static bool cam_stalled_locked(void)
{
	uint32_t now = rtos_get_time();

	if (!s_cam_open || now - s_cam_open_ms < 4000 || now - s_cam_last_ms < 4000)
		return false;
	os_printf("[cam] stalled (%s, no frame for %u ms): power cycle\r\n", s_cam_h264 ? "h264" : "jpeg", (unsigned)(now - s_cam_last_ms));
	cam_close_locked();
	return true;
}

/* HTTP wants JPEG frames. 0 = the camera is (now) in JPEG mode, -1 = stay/503 (see cam mode). */
static int cam_want_jpeg(void)
{
	int ret = 0, playing = 0;

	rtos_lock_mutex(&s_cam_ctl);
	cam_stalled_locked();
	if (s_cam_open && !s_cam_h264)
		goto out;
	if (s_cam_pref == CAM_PREF_H264) {
		ret = -1;
		goto out;
	}
	if (s_cam_open && s_cam_h264) {
		rtsp_clients(&playing);
		if (playing > 0) {   /* auto: a live RTSP viewer keeps H.264 */
			ret = -1;
			goto out;
		}
		cam_close_locked();
	}
	ret = cam_open_locked(1280, 720, false);
out:
	rtos_unlock_mutex(&s_cam_ctl);
	return ret;
}

/* ---- rtsp.c callbacks: the RTSP server wants H.264 ---- */
int rtsp_cam_acquire(void)
{
	int ret = 0, w;

	rtos_lock_mutex(&s_cam_ctl);
	if (s_cam_pref == CAM_PREF_JPEG) {
		ret = -1;
		goto out;
	}
	cam_stalled_locked();
	for (int attempt = 0; attempt < 2; attempt++) {   /* a silent or undetected cold sensor gets one power cycle */
		if (s_cam_open && !s_cam_h264)
			cam_close_locked();
		if (!s_cam_open)
			ret = cam_open_locked(1280, 720, true);
		if (ret == 0) {
			for (w = 0; w < 300 && s_cam_h264_frames == 0; w++)   /* first access unit ~1 s after open */
				rtos_delay_milliseconds(10);
			if (s_cam_h264_frames)
				break;
			os_printf("[cam] h264: no frame within 3 s\r\n");
			ret = -2;
		}
		if (s_cam_open)
			cam_close_locked();
	}
	if (ret != 0)
		goto out;
	if (s_h264_q_pending) {
		rtos_delay_milliseconds(100);   /* let the encoder ISR consume the SDK preset first */
		if (cam_h264_apply_q() == BK_OK)
			s_h264_q_pending = false;
	}
out:
	rtos_unlock_mutex(&s_cam_ctl);
	return ret;
}

void rtsp_cam_release(void)
{
	rtos_lock_mutex(&s_cam_ctl);
	if (s_cam_open && s_cam_h264)
		cam_close_locked();
	rtos_unlock_mutex(&s_cam_ctl);
}

void rtsp_cam_idr(void)
{
	if (s_cam_open && s_cam_h264)
		media_app_h264_regenerate_idr(DVP_CAMERA);
}

/* ---- camera HTTP server (AVDK build): GET /snapshot.jpg = latest JPEG, GET /stream = MJPEG
 * (multipart/x-mixed-replace), GET / = status. Plain HTTP on the LAN for Home Assistant's
 * generic/mjpeg camera; the frame is copied out of the shared buffer first so the camera callback
 * is never blocked by a slow client. ---- */
#define CAM_HTTP_PORT 8080
#define CAM_HTTP_MAX_CLIENTS 6   /* HA keeps the MJPEG stream open; leave room for snapshots and a browser */
static volatile int s_cam_http_clients;

static int cam_copy_latest(uint8_t *dst, uint32_t cap, uint32_t *seq)
{
	uint32_t len;
	rtos_lock_mutex(&s_cam_mutex);
	len = s_cam_len;
	if (len && len <= cap) {
		os_memcpy(dst, s_cam_buf, len);
		*seq = s_cam_seq;
	} else {
		len = 0;
	}
	rtos_unlock_mutex(&s_cam_mutex);
	return (int)len;
}

/* For sdcard.c: latest JPEG, opening the camera on demand (first frame takes ~1 s). */
/* For sdcard.c: the microSD slot is on the camera supply and the SDK card init only completes with
 * the media pipeline up (2026-10-04), so a mount brings the JPEG camera up first. */
int app_sd_cam_ready(void)
{
	return cam_want_jpeg();
}

bool app_cam_is_open(void)
{
	return s_cam_open;
}

int app_cam_grab(uint8_t *dst, uint32_t cap, uint32_t wait_ms)
{
	uint32_t seq = 0, t0 = rtos_get_time();
	int n;

	if (cam_want_jpeg() != 0 && !s_cam_len)   /* H.264 busy (RTSP viewer): fall back to the last JPEG if any */
		return -1;
	if (!s_cam_mutex || !s_cam_buf)
		return -2;
	while ((n = cam_copy_latest(dst, cap, &seq)) <= 0 && rtos_get_time() - t0 < wait_ms)
		rtos_delay_milliseconds(50);
	return n;
}

static int send_all(int sock, const void *p, int n)
{
	int off = 0, r;
	while (off < n && (r = send(sock, (const uint8_t *)p + off, n - off > 4096 ? 4096 : n - off, 0)) > 0)
		off += r;
	return off == n ? 0 : -1;
}

static void cam_http_client_task(beken_thread_arg_t arg)
{
	int sock = (int)(intptr_t)arg, n;
	char req[256], hdr[192];
	uint8_t *buf = NULL;
	uint32_t seq = 0, last = 0;
	struct timeval tv = {.tv_sec = 5, .tv_usec = 0};

	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));   /* a stalled stream client is dropped, not held forever */
	n = recv(sock, req, sizeof(req) - 1, 0);
	if (n <= 0)
		goto out;
	req[n] = 0;
	if (talk_http_handle(sock, req, n))   /* /speak, /mic.wav, /talk (talk.c): handled, just close */
		goto out;
	if (os_strncmp(req, "GET /stream", 11) == 0 || os_strncmp(req, "GET /snapshot", 13) == 0) {
		if (cam_want_jpeg() == 0) {   /* open on demand (or switch back from H.264); the first frame takes ~1 s */
			for (int w = 0; w < 300 && s_cam_len == 0; w++)
				rtos_delay_milliseconds(10);
		} else if (os_strncmp(req, "GET /stream", 11) == 0 || !s_cam_len) {
			n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 24\r\nConnection: close\r\n\r\ncamera busy (H.264/RTSP)");
			send_all(sock, hdr, n);
			goto out;
		}
		/* else: the snapshot is the last JPEG taken before the switch to H.264 (stale but useful) */
	}
	if (!s_cam_mutex || !s_cam_buf) {
		n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 15\r\nConnection: close\r\n\r\ncamera not open");
		send_all(sock, hdr, n);
		goto out;
	}
	buf = psram_malloc(CAM_BUF_MAX);
	if (!buf)
		goto out;
	if (os_strncmp(req, "GET /stream", 11) == 0) {
		n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
			     "Cache-Control: no-cache\r\nConnection: close\r\n\r\n");
		if (send_all(sock, hdr, n))
			goto out;
		for (;;) {
			for (int w = 0; w < 200 && s_cam_seq == last; w++)   /* wait up to 2 s for a new frame */
				rtos_delay_milliseconds(10);
			if (s_cam_seq == last)
				continue;
			n = cam_copy_latest(buf, CAM_BUF_MAX, &seq);
			if (n <= 0)
				continue;
			last = seq;
			int hl = snprintf(hdr, sizeof(hdr), "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", n);
			if (send_all(sock, hdr, hl) || send_all(sock, buf, n) || send_all(sock, "\r\n", 2))
				break;
		}
	} else if (os_strncmp(req, "GET /snapshot", 13) == 0) {
		n = cam_copy_latest(buf, CAM_BUF_MAX, &seq);
		if (n > 0) {
			int hl = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n"
					  "Cache-Control: no-cache\r\nConnection: close\r\n\r\n", n);
			if (!send_all(sock, hdr, hl))
				send_all(sock, buf, n);
		} else {
			n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 12\r\nConnection: close\r\n\r\nno frame yet");
			send_all(sock, hdr, n);
		}
	} else {
		char body[160];
		int bl = snprintf(body, sizeof(body), "<html><body>doorbell " FW_VERSION " cam open=%d frames=%u %ux%u<br>"
				  "<a href=/snapshot.jpg>snapshot.jpg</a> <a href=/stream>stream</a></body></html>",
				  s_cam_open, s_cam_frames, s_cam_w, s_cam_h);
		n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", bl);
		if (!send_all(sock, hdr, n))
			send_all(sock, body, bl);
	}
out:
	if (buf)
		os_free(buf);
	close(sock);
	s_cam_http_clients--;
	rtos_delete_thread(NULL);
}

static void cam_http_task(beken_thread_arg_t arg)
{
	struct sockaddr_in sa = {0};
	int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;

	if (ls < 0)
		goto out;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(CAM_HTTP_PORT);
	sa.sin_addr.s_addr = INADDR_ANY;
	if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(ls, 2) < 0) {
		os_printf("[cam] http bind/listen failed\r\n");
		close(ls);
		goto out;
	}
	os_printf("[cam] http server on port %d\r\n", CAM_HTTP_PORT);
	for (;;) {
		int c = accept(ls, NULL, NULL);
		beken_thread_t t;
		if (c < 0)
			continue;
		if (s_cam_http_clients >= CAM_HTTP_MAX_CLIENTS) {
			close(c);
			continue;
		}
		s_cam_http_clients++;
		power_activity();
		if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "camhttp", cam_http_client_task, 6144, (beken_thread_arg_t)(intptr_t)c) != BK_OK) {   /* 6 KB: talk.c (url fetch + WAV parser) runs in this thread */
			s_cam_http_clients--;
			close(c);
		}
	}
out:
	rtos_delete_thread(NULL);
}

static void cli_cam_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
#if CONFIG_DVP_CAMERA
	if (argc >= 2 && os_strcmp(argv[1], "probe") == 0) {
		cli_cam_probe(argc, argv);
		return;
	}
#endif
	if (argc >= 2 && os_strcmp(argv[1], "open") == 0) {
		/* cam open | open h264 | open W H | open W H h264 */
		bool h264 = os_strcmp(argv[argc - 1], "h264") == 0;
		int dims = argc - 2 - (h264 ? 1 : 0), ret;
		uint32_t w = dims >= 2 ? os_strtoul(argv[2], NULL, 10) : 1280, h = dims >= 2 ? os_strtoul(argv[3], NULL, 10) : 720;
		rtos_lock_mutex(&s_cam_ctl);
		if (s_cam_open && s_cam_h264 != h264)
			cam_close_locked();
		ret = cam_open_locked(w, h, h264);
		rtos_unlock_mutex(&s_cam_ctl);
		REPLY("[cam] open %ux%u %s -> %d%s\r\n", w, h, h264 ? "h264" : "jpeg", ret, s_cam_open ? " (open)" : "");
	} else if (argc >= 2 && os_strcmp(argv[1], "close") == 0) {
		rtos_lock_mutex(&s_cam_ctl);
		cam_close_locked();
		rtos_unlock_mutex(&s_cam_ctl);
		REPLY("[cam] closed\r\n");
	} else if (argc >= 2 && os_strcmp(argv[1], "stat") == 0) {
		static const char *const pref[] = {"auto", "jpeg", "h264"};
		REPLY("[cam] open %d %s (mode %s) frames %u h264 %u dropped %u last jpeg %u bytes %ux%u seq %u age %u ms\r\n", s_cam_open,
		      s_cam_open ? (s_cam_h264 ? "h264" : "jpeg") : "-", pref[s_cam_pref], s_cam_frames, s_cam_h264_frames,
		      s_cam_drop, s_cam_len, s_cam_w, s_cam_h, s_cam_seq, s_cam_last_ms ? rtos_get_time() - s_cam_last_ms : 0);
	} else if (argc >= 3 && os_strcmp(argv[1], "mode") == 0) {
		if (os_strcmp(argv[2], "auto") == 0)
			s_cam_pref = CAM_PREF_AUTO;
		else if (os_strcmp(argv[2], "jpeg") == 0)
			s_cam_pref = CAM_PREF_JPEG;
		else if (os_strcmp(argv[2], "h264") == 0)
			s_cam_pref = CAM_PREF_H264;
		else {
			REPLY("cam mode auto|jpeg|h264\r\n");
			return;
		}
		kv_set_str(KV_CAM_MODE, argv[2]);
		REPLY("[cam] mode %s (applies at the next open/switch)\r\n", argv[2]);
	} else if (argc >= 4 && os_strcmp(argv[1], "snap") == 0) {
		beken_thread_t t;
		if (!s_cam_len) { REPLY("[cam] no frame yet\r\n"); return; }
		os_strncpy(s_net_host, argv[2], sizeof(s_net_host) - 1);
		s_net_port = os_strtoul(argv[3], NULL, 10);
		REPLY("[cam] sending %u bytes to %s:%u\r\n", s_cam_len, s_net_host, s_net_port);
		if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "camsnap", camsnap_task, 4096, NULL) != BK_OK)
			REPLY("[cam] thread failed\r\n");
	} else {
		REPLY("cam open [W H] [h264] | close | stat | mode auto|jpeg|h264 | snap HOST PORT | http://IP:8080/snapshot.jpg /stream | probe [PWR_PIN ...] | probe scan | probe rails on|off | probe bb SCL SDA\r\n");
	}
}

/* ---- rtsp: H.264 live stream server (rtsp.c), rtsp://IP:554/live ---- */
static void cli_rtsp_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	char buf[320];

	if (argc >= 2 && os_strcmp(argv[1], "on") == 0) {
		int ret = rtsp_start(s_rtsp_port);
		s_rtsp_on = true;
		kv_set_str(KV_RTSP, "1");
		REPLY("[rtsp] on, port %u -> %d (persisted; clients switch the camera to H.264 on demand)\r\n", s_rtsp_port, ret);
	} else if (argc >= 2 && os_strcmp(argv[1], "off") == 0) {
		rtsp_stop();
		s_rtsp_on = false;
		kv_set_str(KV_RTSP, "0");
		REPLY("[rtsp] off (persisted; server stops within ~1 s, camera released if in H.264)\r\n");
	} else if (argc >= 2 && os_strcmp(argv[1], "status") == 0) {
		rtsp_status(buf, sizeof(buf));
		REPLY("%s", buf);
		rtsp_status_audio(buf, sizeof(buf));
		REPLY("%s", buf);
		if (rtsp_status_clients(buf, sizeof(buf)) > 0)
			REPLY("%s", buf);
	} else if (argc >= 3 && os_strcmp(argv[1], "port") == 0) {
		uint32_t p = os_strtoul(argv[2], NULL, 10);
		if (p == 0 || p > 65535) {
			REPLY("[rtsp] bad port\r\n");
			return;
		}
		s_rtsp_port = (uint16_t)p;
		kv_set_str(KV_RTSP_PORT, argv[2]);
		REPLY("[rtsp] port %u (takes effect at the next `rtsp off` + `rtsp on`, or reboot)\r\n", s_rtsp_port);
	} else if (argc >= 4 && os_strcmp(argv[1], "q") == 0) {
		uint32_t imb = os_strtoul(argv[2], NULL, 10), pmb = os_strtoul(argv[3], NULL, 10);
		if (imb < 10 || imb > 290 || pmb < 4 || pmb > 1000) {
			REPLY("[rtsp] q IMB(10..290) PMB(4..1000) bits per macroblock; 720p = 3600 MB, 15 fps, IDR every 2 s\r\n");
			return;
		}
		s_h264_imb = imb;
		s_h264_pmb = pmb;
		snprintf(buf, sizeof(buf), "%u %u", imb, pmb);
		kv_set_str(KV_H264_Q, buf);
		s_h264_q_pending = true;
		if (s_cam_open && s_cam_h264 && s_cam_h264_frames) {
			rtos_lock_mutex(&s_cam_ctl);
			if (cam_h264_apply_q() == BK_OK)
				s_h264_q_pending = false;
			rtos_unlock_mutex(&s_cam_ctl);
		}
		REPLY("[rtsp] q imb %u pmb %u (~%u kbit/s nominal)%s\r\n", imb, pmb, (imb + 29 * pmb) * 3600 / 2000,
		      s_h264_q_pending ? ", applied at the next H.264 open" : ", applied");
	} else if (argc >= 2 && os_strcmp(argv[1], "idr") == 0) {
		rtsp_request_idr();
		REPLY("[rtsp] IDR requested\r\n");
	} else {
		REPLY("rtsp on|off | status | port N | q IMB PMB | idr   (now %s, port %u, q %u/%u, cam mode %d)\r\n",
		      rtsp_running() ? "running" : "stopped", s_rtsp_port, s_h264_imb, s_h264_pmb, s_cam_pref);
	}
}
#endif /* DOORBELL_AVDK */

/* ---- LEDs (found 2026-10-03 with the Mac camera): GPIO 44 = white fill LED, GPIO 45 = the four blue
 * button-ring LEDs (night locator light), both active high. The IR LED above the camera is still unmapped. ---- */
#define PIN_LED_WHITE 44
#define PIN_LED_RING 45
static void cli_led_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	int which = -1, state;

	if (argc >= 3 && os_strcmp(argv[1], "white") == 0)
		which = NIGHT_LED_WHITE;
	else if (argc >= 3 && os_strcmp(argv[1], "ring") == 0)
		which = NIGHT_LED_RING;
	if (which < 0 || (os_strcmp(argv[2], "on") && os_strcmp(argv[2], "off") && os_strcmp(argv[2], "auto"))) {
		REPLY("led white|ring on|off|auto   (on/off = manual override of the night light / ring pattern, auto = release)\r\n");
		return;
	}
	/* manual on/off overrides the automatic night light / ring indicator until `auto` or a reboot (night.c) */
	state = os_strcmp(argv[2], "auto") == 0 ? -1 : os_strcmp(argv[2], "on") == 0;
	night_led_manual(which, state);
	REPLY("[led] %s %s (GPIO %d%s)\r\n", argv[1], argv[2], which == NIGHT_LED_WHITE ? PIN_LED_WHITE : PIN_LED_RING,
	      state < 0 ? ", automatic" : ", manual override: `led ... auto` to release");
}

static void cli_tok_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	if (argc >= 3 && os_strcmp(argv[1], "set") == 0) {
		os_strlcpy(s_token, argv[2], sizeof(s_token));
		kv_set_str(KV_TOKEN, s_token);
		os_printf("[tok] token set (%u chars); TCP channel active after reboot\r\n", os_strlen(s_token));
	} else if (argc >= 2 && os_strcmp(argv[1], "clear") == 0) {
		s_token[0] = 0;
		kv_set_str(KV_TOKEN, "");
		os_printf("[tok] cleared\r\n");
	} else {
		os_printf("[tok] %s  (tok set SECRET | clear)\r\n", s_token[0] ? "set" : "not set");
	}
}
#endif /* CONFIG_SYS_CPU0 */

/* ---- reboot: lets the host reach ROM download mode without touching the board ---- */

static void cli_reboot_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	uint32_t i;

	os_printf("rebooting (restoring boot GPIO config)\r\n");
#if (CONFIG_SYS_CPU0)
	for (i = 0; i < PIN_COUNT; i++) {
		if (PIN_IS_CONSOLE(i))
			continue;
		gpio_dev_unmap(i);
		PIN_CFG(i) = s_boot_cfg[i];
	}
#else
	(void)i;
#endif
	rtos_delay_milliseconds(50);
	bk_reboot();
}

#if (CONFIG_SYS_CPU0)
/* ---- ring effects: what the doorbell itself does on a press, independent of Wi-Fi ---- */
static volatile bool s_fx_running;

/* the LDR divider is powered by the camera supply (GPIO 28): raise it for the reading when the camera is off */
static uint32_t light_read_mv(void)
{
	uint32_t mv;
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
	bool cam = s_cam_open;   /* camera open = GPIO 28 already high and must stay high */
#else
	bool cam = false;
#endif

	if (!cam) {
		pin_configure(28, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		bk_gpio_set_output_high(28);
		rtos_delay_milliseconds(80);
	}
	mv = adc_read_mv(LIGHT_ADC_CHAN);
	if (!cam)
		bk_gpio_set_output_low(28);
	return mv;
}

static void ringfx_task(beken_thread_arg_t arg)
{
	uint32_t light = light_read_mv();
	bool dark = light > s_dark_mv;
	rtos_delay_milliseconds(20);

	os_printf("[fx] ring: light %u mV (%s)\r\n", light, dark ? "dark" : "day");
	if (!s_spk_busy)
		spk_start(0, 0, 0x60e000 + 16, 66450 - 16);   /* stock bell chime from flash */
	pin_configure(PIN_LED_RING, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	if (dark)
		night_white_set(NIGHT_SRC_FX, true);   /* via the arbiter: the night light may hold it on afterwards */
	for (int i = 0; i < 8; i++) {   /* blue ring: 8 blinks over ~2.4 s (night.c leaves the pin alone while s_fx_running) */
		bk_gpio_set_output_high(PIN_LED_RING);
		rtos_delay_milliseconds(150);
		bk_gpio_set_output_low(PIN_LED_RING);
		rtos_delay_milliseconds(150);
	}
	if (dark) {
		rtos_delay_milliseconds(WHITE_LIGHT_MS - 2400);
		night_white_set(NIGHT_SRC_FX, false);
	}
	s_fx_running = false;
	rtos_delete_thread(NULL);
}

static void ringfx_start(void)
{
	beken_thread_t t;

	if (!s_fx_enabled || s_fx_running)
		return;
	s_fx_running = true;
	if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "ringfx", ringfx_task, 3072, NULL) != BK_OK)
		s_fx_running = false;
}

static void cli_ring_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	char buf[16];

	if (argc >= 2 && os_strcmp(argv[1], "test") == 0) {
		uint32_t mv = light_read_mv();   /* read before the effects thread touches GPIO 28 */
		ringfx_start();
		sd_on_event("ring");   /* same as a real press: queued SD snapshot when a card is mounted */
		REPLY("[ring] effects started (light %u mV, dark above %u -> %s)\r\n", mv, s_dark_mv, mv > s_dark_mv ? "white light on" : "day, no light");
	} else if (argc >= 3 && os_strcmp(argv[1], "dark") == 0) {
		s_dark_mv = os_strtoul(argv[2], NULL, 10);
		snprintf(buf, sizeof(buf), "%u", s_dark_mv);
		kv_set_str(KV_DARK, buf);
		REPLY("[ring] dark threshold %u mV\r\n", s_dark_mv);
	} else if (argc >= 3 && os_strcmp(argv[1], "chan") == 0) {   /* ring chan N : ADC channel of the light sensor */
		s_light_chan = os_strtoul(argv[2], NULL, 10);
		snprintf(buf, sizeof(buf), "%u", s_light_chan);
		kv_set_str(KV_LIGHT_CHAN, buf);
		REPLY("[ring] light sensor on ADC%u, now %u mV\r\n", s_light_chan, light_read_mv());
	} else if (argc >= 3 && os_strcmp(argv[1], "fx") == 0) {
		s_fx_enabled = os_strcmp(argv[2], "on") == 0;
		kv_set_str(KV_FX, s_fx_enabled ? "1" : "0");
		REPLY("[ring] local effects %s\r\n", s_fx_enabled ? "on" : "off");
	} else {
		REPLY("ring test | dark MV | chan ADC | fx on|off   (light now %u mV, dark > %u, fx %d)\r\n", light_read_mv(), s_dark_mv, s_fx_enabled);
	}
}

/* ---- hooks for power.c (feat/power): thin wrappers so power.c stays a separate file ---- */
int db_kv_get(const char *key, char *out, int cap) { return kv_get_str(key, out, cap); }
void db_kv_set(const char *key, const char *val) { kv_set_str(key, val); }
uint32_t db_adc_mv(uint32_t chan) { return adc_read_mv(chan); }
int db_pin_cfg(uint32_t pin, int io_mode, int pull_mode)
{
	return pin_configure(pin, (gpio_io_mode_t)io_mode, (gpio_pull_mode_t)pull_mode);
}

bool app_power_got_ip(void)
{
	return s_got_ip;
}

/* anything that must finish before the device may sleep */
bool app_power_busy(void)
{
	bool busy = s_spk_busy || s_mic_busy || s_fx_running || ota_thread != NULL || voc_busy();

#if defined(DOORBELL_AVDK)
	busy = busy || s_cam_http_clients > 0;   /* a snapshot or MJPEG stream client (HA) */
	busy = busy || rtsp_clients(NULL) > 0;   /* a live RTSP viewer / two-way audio session */
#endif
	if (s_got_ip && (s_pend_boot || s_pend_ring || s_pend_motion || s_pend_test || s_pend_fx))
		busy = true;
	return busy;
}

/* amp off, LEDs off, camera closed, sensor rails low: everything we switched on */
void app_power_prepare_sleep(void)
{
	static const uint8_t rails[] = {28, 5, 6, 9};
	unsigned i;

	pa_set(0);
	sd_prepare_sleep();   /* unmount the card (its supply is the camera rail cut below) */
	pin_configure(PIN_LED_WHITE, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	bk_gpio_set_output_low(PIN_LED_WHITE);
	pin_configure(PIN_LED_RING, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	bk_gpio_set_output_low(PIN_LED_RING);
#if defined(DOORBELL_AVDK)
	if (s_cam_open) {
		/* through the camera owner's lock so an RTSP acquire in flight cannot reopen it half-way */
		rtos_lock_mutex(&s_cam_ctl);
		cam_close_locked();
		rtos_unlock_mutex(&s_cam_ctl);
		rtos_delay_milliseconds(200);
	}
#endif
	for (i = 0; i < sizeof(rails); i++) {
		pin_configure(rails[i], GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		bk_gpio_set_output_low(rails[i]);
	}
}

/* called from power_init() when the SoC was woken from deep sleep by a GPIO */
void app_power_wake_input(uint32_t pin)
{
	uint32_t now = rtos_get_time();

	if (pin == PIN_BUTTON) {
		s_ring_count++;
		s_last_ring_ms = now;
		s_last_ring_report_ms = now;
		s_pend_fx = true;    /* chime + ring LEDs from flash, before Wi-Fi is up */
		s_pend_ring = true;  /* reported once the IP arrives */
	} else if (pin == 24) {   /* PIR output (pir.c owns the pin; here only the wake reason) */
		s_motion_count++;
		s_last_motion_ms = now;
		s_pend_motion = true;
	}
}

void app_wifi_reconnect(void)
{
	wifi_connect();
}

uint32_t app_power_pir_sens(void)
{
	return pir_sens_get();
}
#endif /* CONFIG_SYS_CPU0 */

static const struct cli_command s_doorbell_commands[] = {
	{"pin", "pin all|mux|dump|cfg|get|in|out|hiz|blink|watch ... (GPIO mapping)", cli_pin_cmd},
	{"adc", "adc CHANNEL (one calibrated SARADC reading)", cli_adc_read_cmd},
	{"mem", "mem HEXADDR [N] (raw 32-bit reads, debug)", cli_mem_cmd},
	{"reboot", "reset the SoC (ROM download mode is reachable right after)", cli_reboot_cmd},
#if (CONFIG_SYS_CPU0)
	{"wifi", "wifi set SSID PASS | clear | connect | (status)", cli_wifi_cmd},
	{"hook", "hook set HOST:PORT/PATH | test | ring | motion | (show)", cli_hook_cmd},
	{"pir", "pir on|off | sens 0-3 | cooldown MS | status (PIR GPIO 24/50/25/26)", cli_pir_cmd},
	{"motion", "motion test (simulated PIR edge through the ISR handler)", cli_motion_cmd},
	{"ota", "ota URL (download app_pack.rbl over HTTP, then reboot)", cli_ota_cmd},
	{"tok", "tok set SECRET | clear (token for the TCP command channel, port 2323)", cli_tok_cmd},
	{"led", "led white|ring on|off|auto (white fill LED GPIO 44 / blue button ring GPIO 45)", cli_led_cmd},
	{"night", "night auto on|off | ring on|off | hyst MV | status (night light, ring indicator)", night_cli_cmd},
	{"sys", "sys info (fw, uptime, ip, heap, reset reason, light/battery mV)", sys_cli_cmd},
#if (CONFIG_SYS_CPU0)
	{"ring", "ring test | dark MV | fx on|off (local chime, ring LEDs, light when dark)", cli_ring_cmd},
	{"pwr", "pwr status | sleep on|off | idle S | wake_pir on|off | snap_min M | sleepnow (battery, charger, deep sleep)", cli_pwr_cmd},
	{"sd", "sd status | mount | umount | format | ls [DIR] | save [EVENT] | auto on|off | keep S | probe (microSD)", cli_sd_cmd},
#endif
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
	{"cam", "cam open [W H] [h264] | close | stat | mode auto|jpeg|h264 | snap HOST PORT (AVDK build)", cli_cam_cmd},
	{"rtsp", "rtsp on|off | status | port N | q IMB PMB | idr (H.264 live stream rtsp://IP:554/live)", cli_rtsp_cmd},
#endif
	{"spk", "spk tone [HZ] [MS] | play beepa|beepb|bell|ADDR LEN | net HOST PORT | stop", cli_spk_cmd},
	{"mic", "mic level [MS] [HZ] | rec MS HOST PORT (on-board ADC mic)", cli_mic_cmd},
	{"talk", "talk status | gain N|auto | micgain N | max S | stop | aec on|off | vocmic N | voc on|off (two-way audio: HTTP :8080 /speak /mic.wav, RTSP voc engine)", cli_talk_cmd},
#endif
};
#if (CONFIG_SYS_CPU0)
static unsigned s_doorbell_command_count = sizeof(s_doorbell_commands) / sizeof(s_doorbell_commands[0]);
#endif

/* ---- app ---- */

static void heartbeat_task(beken_thread_arg_t arg)
{
	uint32_t n = 0;

	while (1) {
		os_printf("[" FW_NAME "] alive %u, uptime %u ms\r\n", n++, rtos_get_time());
		rtos_delay_milliseconds(30000);
	}
}

void user_app_main(void)
{
	beken_thread_t heartbeat, reporter, tcpcmd;

	os_printf("\r\n==== " FW_NAME " " FW_VERSION ": hello from our own firmware ====\r\n");
	cli_register_commands(s_doorbell_commands,
			      sizeof(s_doorbell_commands) / sizeof(s_doorbell_commands[0]));
	rtos_create_thread(&heartbeat, BEKEN_APPLICATION_PRIORITY, "heartbeat",
			   heartbeat_task, 2048, NULL);

#if (CONFIG_SYS_CPU0)
	pa_set(0);   /* amplifier enable floats at boot and half-turns the speaker on: park it low */
	voc_init();  /* KV: db_talk_aec / db_voc_mic; the engine itself starts with the first RTSP audio track */
	pin_configure(9, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);   /* sensor rail: light sensor (ADC10) reads ~25 mV dead without it */
	bk_gpio_set_output_high(9);
#if defined(DOORBELL_AVDK)
	{
		beken_thread_t http;
		char v[24], *e;

		rtos_init_mutex(&s_cam_ctl);
		if (kv_get_str(KV_CAM_MODE, v, sizeof(v)))
			s_cam_pref = os_strcmp(v, "jpeg") == 0 ? CAM_PREF_JPEG : os_strcmp(v, "h264") == 0 ? CAM_PREF_H264 : CAM_PREF_AUTO;
		if (kv_get_str(KV_H264_Q, v, sizeof(v))) {
			uint32_t a = os_strtoul(v, &e, 10), b = os_strtoul(e, NULL, 10);
			if (a >= 10 && a <= 290 && b >= 4 && b <= 1000) {
				s_h264_imb = a;
				s_h264_pmb = b;
			}
		}
		if (kv_get_str(KV_RTSP_PORT, v, sizeof(v)) && os_strtoul(v, NULL, 10) && os_strtoul(v, NULL, 10) <= 65535)
			s_rtsp_port = (uint16_t)os_strtoul(v, NULL, 10);
		if (kv_get_str(KV_RTSP, v, sizeof(v)))
			s_rtsp_on = v[0] == '1';
		rtos_create_thread(&http, BEKEN_APPLICATION_PRIORITY, "camhttp", cam_http_task, 4096, NULL);
		if (s_rtsp_on)   /* default off: nothing changes for the camera until `rtsp on` */
			rtsp_start(s_rtsp_port);
	}
#endif
	pin_configure(PIN_LED_WHITE, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	bk_gpio_set_output_low(PIN_LED_WHITE);
	pin_configure(PIN_LED_RING, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	bk_gpio_set_output_low(PIN_LED_RING);
	kv_get_str(KV_SSID, s_ssid, sizeof(s_ssid));
	{
		char v[16];
		if (kv_get_str(KV_DARK, v, sizeof(v)))
			s_dark_mv = os_strtoul(v, NULL, 10);
		if (kv_get_str(KV_FX, v, sizeof(v)))
			s_fx_enabled = v[0] != '0';
		if (kv_get_str(KV_LIGHT_CHAN, v, sizeof(v)))
			s_light_chan = os_strtoul(v, NULL, 10);
	}
	talk_init();   /* KV: db_talk_gain / db_talk_micgain / db_talk_max */
	kv_get_str(KV_PASS, s_pass, sizeof(s_pass));
	if (!kv_get_str(KV_HOOK, s_hook, sizeof(s_hook)))
		os_strlcpy(s_hook, HOOK_DEFAULT, sizeof(s_hook));
	{
		/* night.c: LED arbiter + night light + ring indicator; before inputs_init() so a press
		 * (ringfx -> night_white_set) never runs ahead of it */
		static const night_env_t env = {
			.fw_version = FW_VERSION,
			.ip = s_ip,
			.got_ip = &s_got_ip,
#if defined(DOORBELL_AVDK)
			.cam_open = &s_cam_open,
#else
			.cam_open = NULL,
#endif
			.fx_running = &s_fx_running,
			.dark_mv = &s_dark_mv,
			.light_read_mv = light_read_mv,
			.adc_read_mv = adc_read_mv,
		};
		night_init(&env);
	}
	bk_event_register_cb(EVENT_MOD_NETIF, EVENT_ID_ALL, netif_event_cb, NULL);
	bk_event_register_cb(EVENT_MOD_WIFI, EVENT_ID_ALL, wifi_event_cb, NULL);
	inputs_init();
	power_init();   /* before the reporter: a button wake from deep sleep must chime at once, before Wi-Fi */
	sd_init();   /* microSD worker: card mounted on demand (ring/motion/`sd save`, opens the camera), unmounted when idle */
	rtos_create_thread(&reporter, BEKEN_APPLICATION_PRIORITY, "reporter",
			   reporter_task, 4096, NULL);
	kv_get_str(KV_TOKEN, s_token, sizeof(s_token));
	if (s_token[0])
		rtos_create_thread(&tcpcmd, BEKEN_APPLICATION_PRIORITY, "tcpcmd",
				   tcp_cmd_task, 4096, NULL);
	else
		os_printf("[tcp] disabled (no token: `tok set SECRET`)\r\n");
	os_printf("[cfg] ssid '%s' hook %s\r\n", s_ssid, s_hook[0] ? s_hook : "not set");
	if (s_ssid[0])
		wifi_connect();
	else
		os_printf("[wifi] no credentials: use `wifi set SSID PASS`\r\n");
#else
	(void)reporter;
	(void)tcpcmd;
#endif
}

int main(void)
{
#if (CONFIG_SYS_CPU0)
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
#endif
	bk_init();
#ifdef DOORBELL_AVDK
	media_service_init();   /* CPU0: media app (mailbox client); CPU1: media major (camera/JPEG); CPU2: minor */
#endif

	return 0;
}
