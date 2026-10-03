#include "bk_private/bk_init.h"
#include "bk_private/bk_cli.h"
#include <components/system.h>
#include <os/os.h>
#include <os/str.h>
#include <os/mem.h>
#include <string.h>
#include <stdio.h>
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
#endif

#define FW_NAME "doorbell-bk7258-fw"
#define FW_VERSION "0.9.2"

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
	os_printf("pin all | mux | dump | get N | in N [up|down|none] | hiz N | out N 0|1 | raw N HEX | blink N [count] | watch SECONDS\r\n");
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
		for (i = 0; i < PIN_COUNT; i += 8)
			os_printf("CFG %02u: %08x %08x %08x %08x %08x %08x %08x %08x\r\n", i,
				  PIN_CFG(i), PIN_CFG(i + 1), PIN_CFG(i + 2), PIN_CFG(i + 3),
				  PIN_CFG(i + 4), PIN_CFG(i + 5), PIN_CFG(i + 6), PIN_CFG(i + 7));
		return;
	}
	/* "watch" takes seconds, not a pin: check it before the pin-range test. */
	if (argc < 3 || (os_strcmp(argv[1], "watch") != 0 && p >= PIN_COUNT)) {
		pin_usage();
		return;
	}

	if (os_strcmp(argv[1], "get") == 0) {
		os_printf("PIN %u = %d (cfg %02x)\r\n", p, bk_gpio_get_input(p) ? 1 : 0, PIN_CFG(p) & 0xff);
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
		os_printf("PIN %u is the console UART, refusing\r\n", p);
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
		os_printf("PIN %u in %s = %d (cfg %02x, err %d)\r\n", p, pull, bk_gpio_get_input(p) ? 1 : 0,
			  PIN_CFG(p) & 0xff, err);
	} else if (os_strcmp(argv[1], "raw") == 0 && argc >= 4) {
		/* write the AON GPIO config word verbatim (hex), e.g. to restore a boot-time state */
		uint32_t v = os_strtoul(argv[3], NULL, 16);

		gpio_dev_unmap(p);
		PIN_CFG(p) = v;
		os_printf("PIN %u raw %08x -> now %08x\r\n", p, v, PIN_CFG(p));
	} else if (os_strcmp(argv[1], "hiz") == 0) {
		bk_err_t err = pin_configure(p, GPIO_IO_DISABLE, GPIO_PULL_DISABLE);

		bk_gpio_disable_output(p);
		bk_gpio_disable_input(p);
		os_printf("PIN %u hiz (cfg %02x, err %d)\r\n", p, PIN_CFG(p) & 0xff, err);
	} else if (os_strcmp(argv[1], "out") == 0 && argc >= 4) {
		int v = os_strtoul(argv[3], NULL, 10) ? 1 : 0;

		pin_configure(p, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		bk_gpio_disable_input(p);
		if (v)
			bk_gpio_set_output_high(p);
		else
			bk_gpio_set_output_low(p);
		os_printf("PIN %u out %d (cfg %02x)\r\n", p, v, PIN_CFG(p) & 0xff);
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
		os_printf("ADC%u raw=%u mv=%u\r\n", chan, raw, (uint32_t)(saradc_calculate(raw) * 1000));
	else
		os_printf("ADC%u: failed %d (-0x%x)\r\n", chan, err, -err);
	bk_adc_stop();
	bk_adc_deinit(chan);
	bk_adc_release();
}

#if (CONFIG_SYS_CPU0)
/* ---- doorbell prototype: Wi-Fi + Home Assistant webhook ---- */
#define PIN_PIR 24      /* active-high motion output, 2-4 s pulses */
#define PIN_BUTTON 53   /* externally pulled high, expected to drop on press */
#define HEARTBEAT_MS (5 * 60 * 1000)
#define KV_SSID "db_ssid"
#define KV_PASS "db_pass"
#define KV_HOOK "db_hook"   /* "host:port/path" */
#define HOOK_DEFAULT "192.168.2.80:8123/api/webhook/doorbell_bk7258_9f3c"

static char s_ssid[WIFI_SSID_STR_LEN], s_pass[WIFI_PASSWORD_LEN], s_hook[128];
static volatile bool s_got_ip, s_pend_motion, s_pend_ring, s_pend_boot, s_pend_test;
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

static uint32_t adc_read_mv(uint32_t chan)
{
	adc_config_t config = {0};
	uint16_t raw = 0;
	uint32_t mv = 0;

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
	return mv;
}

/* Minimal HTTP/1.1 POST over a blocking lwIP socket. Returns the HTTP status or -errno. */
static int http_post_json(const char *hook, const char *json)
{
	char host[64], path[96], req[512], resp[64];
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

static int report(const char *event)
{
	char json[192];
	int st;

	if (!s_got_ip)
		return -ENOTCONN;
	snprintf(json, sizeof(json),
		 "{\"event\":\"%s\",\"fw\":\"" FW_VERSION "\",\"ip\":\"%s\",\"mv\":%u,"
		 "\"uptime_s\":%u,\"motion_count\":%u,\"ring_count\":%u}",
		 event, s_ip, adc_read_mv(9), rtos_get_time() / 1000, s_motion_count, s_ring_count);
	st = http_post_json(s_hook, json);
	os_printf("[hook] %s -> %d\r\n", event, st);
	return st;
}

static void pir_isr(gpio_id_t id)
{
	uint32_t now = rtos_get_time();

	if (now - s_last_motion_ms > 3000) {
		s_last_motion_ms = now;
		s_motion_count++;
		s_pend_motion = true;
	}
}

static volatile uint32_t s_last_ring_report_ms;

static void button_isr(gpio_id_t id)
{
	uint32_t now = rtos_get_time();

	if (now - s_last_ring_ms > 800) {
		s_last_ring_ms = now;
		s_ring_count++;
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
		if (s_pend_ring) {
			s_pend_ring = false;
			report("ring");
		}
		if (s_pend_motion) {
			s_pend_motion = false;
			report("motion");
		}
		if (s_pend_test) {
			s_pend_test = false;
			report("test");
		}
		if (s_got_ip && rtos_get_time() - last_hb > HEARTBEAT_MS) {
			last_hb = rtos_get_time();
			report("heartbeat");
		}
		rtos_delay_milliseconds(50);
	}
}

static void inputs_init(void)
{
	pin_configure(PIN_PIR, GPIO_INPUT_ENABLE, GPIO_PULL_DISABLE);
	bk_gpio_disable_output(PIN_PIR);
	bk_gpio_register_isr(PIN_PIR, pir_isr);
	bk_gpio_set_interrupt_type(PIN_PIR, GPIO_INT_TYPE_RISING_EDGE);
	bk_gpio_enable_interrupt(PIN_PIR);

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
		os_printf("[wifi] ssid '%s' state %d rssi %d ip %s got_ip %d\r\n", s_ssid, st.state, st.rssi, s_ip, s_got_ip);
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
	os_printf("[hook] %s  (hook set HOST:PORT/PATH | test | ring | motion)\r\n", s_hook);
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
				if (run_line(cmd) == 0)
					send(cs, "ok\n", 3, 0);
				else
					send(cs, "unknown\n", 8, 0);
			}
		}
		rtos_delay_milliseconds(100);
		close(cs);
	}
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

static const struct cli_command s_doorbell_commands[] = {
	{"pin", "pin all|mux|get|in|out|blink|watch ... (GPIO mapping)", cli_pin_cmd},
	{"adc", "adc CHANNEL (one calibrated SARADC reading)", cli_adc_read_cmd},
	{"reboot", "reset the SoC (ROM download mode is reachable right after)", cli_reboot_cmd},
#if (CONFIG_SYS_CPU0)
	{"wifi", "wifi set SSID PASS | clear | connect | (status)", cli_wifi_cmd},
	{"hook", "hook set HOST:PORT/PATH | test | ring | motion | (show)", cli_hook_cmd},
	{"ota", "ota URL (download app_pack.rbl over HTTP, then reboot)", cli_ota_cmd},
	{"tok", "tok set SECRET | clear (token for the TCP command channel, port 2323)", cli_tok_cmd},
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
	kv_get_str(KV_SSID, s_ssid, sizeof(s_ssid));
	kv_get_str(KV_PASS, s_pass, sizeof(s_pass));
	if (!kv_get_str(KV_HOOK, s_hook, sizeof(s_hook)))
		os_strlcpy(s_hook, HOOK_DEFAULT, sizeof(s_hook));
	bk_event_register_cb(EVENT_MOD_NETIF, EVENT_ID_ALL, netif_event_cb, NULL);
	bk_event_register_cb(EVENT_MOD_WIFI, EVENT_ID_ALL, wifi_event_cb, NULL);
	inputs_init();
	rtos_create_thread(&reporter, BEKEN_APPLICATION_PRIORITY, "reporter",
			   reporter_task, 4096, NULL);
	kv_get_str(KV_TOKEN, s_token, sizeof(s_token));
	if (s_token[0])
		rtos_create_thread(&tcpcmd, BEKEN_APPLICATION_PRIORITY, "tcpcmd",
				   tcp_cmd_task, 4096, NULL);
	else
		os_printf("[tcp] disabled (no token: `tok set SECRET`)\r\n");
	os_printf("[cfg] ssid '%s' hook %s\r\n", s_ssid, s_hook);
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

	return 0;
}
