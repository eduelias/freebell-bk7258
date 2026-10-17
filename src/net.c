/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: Wi-Fi, webhook reports, button/motion events, reporter task, hook/wifi/ota commands. */
/* Everything that talks to Home Assistant (split out of app_main.c in 0.28.0) */
#include "app_internal.h"

#if (CONFIG_SYS_CPU0)

/* ---- doorbell prototype: Wi-Fi + Home Assistant webhook ---- */
/* PIR (GPIO 24 output, 50 enable, 25/26 sensitivity) lives in pir.c since v0.19.0 */
#define PIN_BUTTON BOARD_PIN_BUTTON   /* externally pulled high, drops on press. The same line keys the on-board 433 MHz chime TX:
                         * driving it low (as the SDK's boot pin map did until patch 0010) rings every paired chime */
#define PIN_KEY2   BOARD_PIN_KEY2     /* second key in the stock firmware (long press); not wired on this board, parked as input + pull-up */
#define HEARTBEAT_MS (5 * 60 * 1000)
#define KV_SSID "db_ssid"
#define KV_PASS "db_pass"
#define KV_HOOK "db_hook"   /* "host:port/path" */
#define HOOK_DEFAULT ""   /* no webhook until `hook set HOST:PORT/PATH` (stored in KV) */

static char s_ssid[WIFI_SSID_STR_LEN], s_pass[WIFI_PASSWORD_LEN], s_hook[128];
static bool s_hook_unset_logged;   /* "hook not set" is logged once per boot, not per report */
volatile bool s_got_ip;   /* read by night.c through app_main.c */
static volatile bool s_pend_motion, s_pend_ring, s_pend_boot, s_pend_test;
static volatile bool s_pend_fx;
#endif
#if (CONFIG_SYS_CPU0)

static volatile uint32_t s_last_motion_ms, s_last_ring_ms, s_motion_count, s_ring_count;
char s_ip[16] = "0.0.0.0";
#endif
#if (CONFIG_SYS_CPU0)

bool app_net_up(void)
{
	return s_got_ip;
}
#endif
#if (CONFIG_SYS_CPU0)

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
	char json[448], pwr[128];
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
		 "\"uptime_s\":%u,\"motion_count\":%u,\"ring_count\":%u,\"pir_on\":%u,\"pir_sens\":%u,\"sd\":\"%s\"%s%s}",
		 event, s_ip, adc_read_mv(BOARD_ADC_SUPPLY), light_read_mv(), rtos_get_time() / 1000, s_motion_count, s_ring_count,
		 pir_is_on() ? 1u : 0u, pir_sens_get(), sd_health(), pwr, extra ? extra : "");
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
			char pol[64];
			s_pend_boot = false;
			policy_boot_fetch();   /* once per boot: GET policy.json from HA and apply it (<= ~3 s), so the boot report can say so */
			policy_json(pol, sizeof(pol));
			report_ex("boot", pol);
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

	/* 53: the SDK's boot pin map (patched, patch 0010) already leaves it input + pull-up; this keeps
	 * exactly that (pin_configure() unmaps any peripheral function and keeps the pull-up) */
	pin_configure(PIN_BUTTON, GPIO_INPUT_ENABLE, GPIO_PULL_UP_EN);
	bk_gpio_disable_output(PIN_BUTTON);
	pin_configure(PIN_KEY2, GPIO_INPUT_ENABLE, GPIO_PULL_UP_EN);   /* stock parks its second key the same way (0.26.2) */
	bk_gpio_disable_output(PIN_KEY2);
	bk_gpio_register_isr(PIN_BUTTON, button_isr);
	bk_gpio_set_interrupt_type(PIN_BUTTON, GPIO_INT_TYPE_FALLING_EDGE);
	bk_gpio_enable_interrupt(PIN_BUTTON);
}

void cli_wifi_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
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

void cli_ota_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
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
	rescue_clear();   /* the new image starts with a clean crash count (the SDK reboots on success, we do not return) */
	os_printf("[ota] downloading %s\r\n", s_ota_url);
	/* run in its own thread so the shell / TCP channel reply at once; the device reboots on success */
	ret = rtos_create_thread(&ota_thread, BEKEN_APPLICATION_PRIORITY, "ota", ota_task, 8192, NULL);
	if (ret != BK_OK)
		os_printf("[ota] thread failed %d\r\n", ret);
}

void cli_hook_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
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
#endif
#if (CONFIG_SYS_CPU0)
/* host and port of the webhook ("HOST:PORT/PATH"), for the policy URL default */
bool app_hook_hostport(char *host, int cap, int *port)
{
	const char *colon = os_strchr(s_hook, ':');

	if (!s_hook[0] || !colon || colon - s_hook >= cap)
		return false;
	os_memcpy(host, s_hook, colon - s_hook);
	host[colon - s_hook] = 0;
	*port = (int)os_strtoul(colon + 1, NULL, 10);
	return *port > 0;
}

#endif
#if (CONFIG_SYS_CPU0)
bool app_power_got_ip(void)
{
	return s_got_ip;
}

#endif
#if (CONFIG_SYS_CPU0)

void app_wifi_reconnect(void)
{
	wifi_connect();
}
#endif

#if (CONFIG_SYS_CPU0)
/* ---- module API (app_main.c and the power hooks) ---- */
void net_load_settings(void)
{
	kv_get_str(KV_SSID, s_ssid, sizeof(s_ssid));
	kv_get_str(KV_PASS, s_pass, sizeof(s_pass));
	if (!kv_get_str(KV_HOOK, s_hook, sizeof(s_hook)))
		os_strlcpy(s_hook, HOOK_DEFAULT, sizeof(s_hook));
}

void net_start_events(bool inputs)   /* inputs: the button + PIR (not in rescue mode) */
{
	bk_event_register_cb(EVENT_MOD_NETIF, EVENT_ID_ALL, netif_event_cb, NULL);
	bk_event_register_cb(EVENT_MOD_WIFI, EVENT_ID_ALL, wifi_event_cb, NULL);
	if (inputs)
		inputs_init();
}

/* POST a ready-made JSON body to the webhook (rescue.c); the HTTP status or -errno */
int net_post(const char *json)
{
	if (!s_got_ip)
		return -ENOTCONN;
	if (!s_hook[0])
		return -ENOENT;
	return http_post_json(s_hook, json);
}

void net_start_reporter(void)
{
	beken_thread_t reporter;

	rtos_create_thread(&reporter, BEKEN_APPLICATION_PRIORITY, "reporter",
			   reporter_task, 4096, NULL);
}

void net_connect(void)
{
	os_printf("[cfg] ssid '%s' hook %s\r\n", s_ssid, s_hook[0] ? s_hook : "not set");
	if (s_ssid[0])
		wifi_connect();
	else
		os_printf("[wifi] no credentials: use `wifi set SSID PASS`\r\n");
}

bool net_ota_busy(void)
{
	return ota_thread != NULL;
}

/* a report (or the local ring effect) is queued and the network is up: let it go out before sleeping */
bool net_reports_pending(void)
{
	return s_got_ip && (s_pend_boot || s_pend_ring || s_pend_motion || s_pend_test || s_pend_fx);
}

/* deep-sleep wake by a GPIO (power.c -> app_power_wake_input) */
void net_wake_input(uint32_t pin)
{
	uint32_t now = rtos_get_time();

	if (pin == PIN_BUTTON) {
		s_ring_count++;
		s_last_ring_ms = now;
		s_last_ring_report_ms = now;
		s_pend_fx = true;    /* chime + ring LEDs from flash, before Wi-Fi is up */
		s_pend_ring = true;  /* reported once the IP arrives */
	} else if (pin == BOARD_PIN_PIR_OUT) {   /* pir.c owns the pin; here only the wake reason */
		s_motion_count++;
		s_last_motion_ms = now;
		s_pend_motion = true;
	}
}
#endif
