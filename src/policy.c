/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: RAM log ring, speaker volume, boot-time policy pull from Home Assistant, GET /cmd and /log. */
/* See policy.h for the overview. Everything here is CPU0 only. */
#include <common/bk_include.h>
#include "policy.h"

#if (CONFIG_SYS_CPU0)
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <os/os.h>
#include <os/str.h>
#include <os/mem.h>
#include <components/system.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "power.h"    /* db_kv_get/db_kv_set/db_reply, power_sched_params(), power_wake_pir_get() */
#include "pir.h"      /* pir_sens_get() */
#include "night.h"    /* night_auto_get() */
#include "sdcard.h"   /* sd_keep_get(), sd_auto_get(), app_net_up() */
#include "aud_engine.h"  /* aud_spk_gain(): the effective DAC gain (vol or the raw override) */
#include "audio_voc.h"   /* voc_set_spk_gain(): apply a volume change to a live RTSP audio session */
#include "applog.h"   /* last: our own os_printf lines go to the ring too */

/* ------------------------------------------------------------------ RAM log */
static char s_log[APPLOG_SIZE];
static volatile uint32_t s_log_w;   /* monotonic write index */

void applog_write(const char *p, int n)
{
	uint32_t idx, first;
	GLOBAL_INT_DECLARATION();

	if (n <= 0)
		return;
	if (n > APPLOG_SIZE) {   /* keep the tail of an oversized block */
		p += n - APPLOG_SIZE;
		n = APPLOG_SIZE;
	}
	GLOBAL_INT_DISABLE();
	idx = s_log_w % APPLOG_SIZE;
	first = APPLOG_SIZE - idx < (uint32_t)n ? APPLOG_SIZE - idx : (uint32_t)n;
	os_memcpy(s_log + idx, p, first);
	if ((uint32_t)n > first)
		os_memcpy(s_log, p + first, n - first);
	s_log_w += n;
	GLOBAL_INT_RESTORE();
}

uint32_t applog_total(void)
{
	return s_log_w;
}

/* Copy the last n bytes of the ring (at most what is stored and cap - 1). Unlocked read: a line
 * being written at the same moment may show up torn, which is fine for a diagnostic dump. */
int applog_tail(char *dst, int cap, int n)
{
	uint32_t w = s_log_w, have = w < APPLOG_SIZE ? w : APPLOG_SIZE, start, idx, first;

	if (cap <= 1 || n <= 0) {
		if (cap > 0)
			dst[0] = 0;
		return 0;
	}
	if ((uint32_t)n > have)
		n = (int)have;
	if (n > cap - 1)
		n = cap - 1;
	start = w - (uint32_t)n;
	idx = start % APPLOG_SIZE;
	first = APPLOG_SIZE - idx < (uint32_t)n ? APPLOG_SIZE - idx : (uint32_t)n;
	os_memcpy(dst, s_log + idx, first);
	if ((uint32_t)n > first)
		os_memcpy(dst + first, s_log, n - first);
	dst[n] = 0;
	return n;
}

/* os_printf() of every src file lands here (applog.h): format once, ring + UART */
void applog_printf(const char *fmt, ...)
{
	char buf[200];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if (n >= (int)sizeof(buf))
		n = sizeof(buf) - 1;
	applog_write(buf, n);
	bk_printf("%s", buf);
}

#define LOG_REPLY_MAX 600   /* the TCP reply buffer is 640 */

void cli_log_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	char *buf;
	int n = 300;

	if (argc >= 2) {
		if (os_strcmp(argv[1], "stat") == 0) {
			db_reply("[log] %u bytes written, %u in the %u-byte ring (log [N<=%u] | GET /log?t=TOKEN)\r\n",
				 s_log_w, s_log_w < APPLOG_SIZE ? s_log_w : APPLOG_SIZE, APPLOG_SIZE, LOG_REPLY_MAX);
			return;
		}
		n = (int)os_strtoul(argv[1], NULL, 10);
		if (n <= 0 || n > LOG_REPLY_MAX)
			n = LOG_REPLY_MAX;
	}
	buf = os_malloc(LOG_REPLY_MAX + 1);
	if (!buf) {
		db_reply("[log] no memory\r\n");
		return;
	}
	applog_tail(buf, LOG_REPLY_MAX + 1, n);
	app_reply_raw(buf);   /* not through os_printf: the log must not quote itself */
	os_free(buf);
}

/* ------------------------------------------------------------------ volume */
#define KV_VOL "db_vol"
static uint32_t s_vol = 100;

uint32_t vol_get(void)
{
	return s_vol;
}

uint16_t vol_gain(void)
{
	return (uint16_t)((s_vol * VOL_GAIN_MAX + 50) / 100);
}

void vol_init(void)
{
	char v[8];

	if (db_kv_get(KV_VOL, v, sizeof(v))) {
		uint32_t n = os_strtoul(v, NULL, 10);
		if (n <= 100)
			s_vol = n;
	}
}

void cli_vol_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	if (argc >= 2) {
		char *e;
		uint32_t n = os_strtoul(argv[1], &e, 10);
		char b[8];

		if (*e || n > 100 || !argv[1][0]) {
			db_reply("vol [0-100]   (speaker volume: chime and talk `gain auto`; now %u%% = dac gain 0x%x)\r\n", s_vol, vol_gain());
			return;
		}
		s_vol = n;
		snprintf(b, sizeof(b), "%u", n);
		db_kv_set(KV_VOL, b);
		voc_set_spk_gain(aud_spk_gain());   /* live two-way audio session follows the volume */
	}
	db_reply("[vol] %u%% -> dac gain 0x%x (chime + talk auto; `spk cfg gain N` overrides raw)\r\n", s_vol, vol_gain());
}

/* ------------------------------------------------------------------ policy pull */
#define KV_POLICY "db_pwr_policy"   /* "auto" (default) | "off" | http://IP[:port]/path */
#define POLICY_PATH "/local/doorbell/policy.json"
#define POLICY_TIMEOUT_S 3
#define POLICY_RESP_MAX 1536          /* headers + a body of <= 1 KB */
#define POLICY_BODY_MAX 1024

static char s_policy_cfg[96] = "auto";
static const char *s_policy_result = "none";   /* applied | none | error */
static char s_policy_rev[24];
static char s_policy_detail[72] = "not fetched";
static uint32_t s_policy_at_ms;
static bool s_policy_fetched;                  /* once per boot */

void policy_init(void)
{
	char v[96];

	if (db_kv_get(KV_POLICY, v, sizeof(v)) && v[0])
		os_strlcpy(s_policy_cfg, v, sizeof(s_policy_cfg));
}

/* Resolve the effective URL (auto = hook host:port + POLICY_PATH). Returns false when there is none. */
static bool policy_url(char *out, int cap)
{
	char host[48];
	int port;

	if (os_strcmp(s_policy_cfg, "off") == 0)
		return false;
	if (os_strcmp(s_policy_cfg, "auto") == 0) {
		if (!app_hook_hostport(host, sizeof(host), &port))
			return false;
		snprintf(out, cap, "http://%s:%d" POLICY_PATH, host, port);
		return true;
	}
	os_strlcpy(out, s_policy_cfg, cap);
	return true;
}

static int send_all(int sock, const void *p, int n)
{
	int off = 0, r;
	while (off < n && (r = send(sock, (const uint8_t *)p + off, n - off > 1024 ? 1024 : n - off, 0)) > 0)
		off += r;
	return off == n ? 0 : -1;
}

/* Minimal HTTP/1.0 GET (IPv4 literal host, like /speak?url=). Body (<= cap - 1) copied to body,
 * returns the HTTP status, 0 when no valid response, -errno on a connection problem. */
static int http_get(const char *url, char *body, int cap)
{
	char host[48], path[96], req[200], *resp, *end;
	const char *hp, *slash, *colon;
	int port = 80, n, sock, len = 0, status = 0;
	struct sockaddr_in sa = {0};
	struct timeval tv = {.tv_sec = POLICY_TIMEOUT_S, .tv_usec = 0};

	body[0] = 0;
	if (os_strncmp(url, "http://", 7) != 0)
		return -EINVAL;
	hp = url + 7;
	slash = os_strchr(hp, '/');
	if (!slash || slash - hp >= (int)sizeof(host) || os_strlen(slash) >= sizeof(path))
		return -EINVAL;
	os_memcpy(host, hp, slash - hp);
	host[slash - hp] = 0;
	os_strcpy(path, slash);
	if ((colon = os_strchr(host, ':'))) {
		port = (int)os_strtoul(colon + 1, NULL, 10);
		host[colon - host] = 0;
	}
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = inet_addr(host);
	if (sa.sin_addr.s_addr == INADDR_NONE)
		return -EINVAL;   /* no DNS here: the hook host is an IP */
	resp = os_malloc(POLICY_RESP_MAX);
	if (!resp)
		return -ENOMEM;
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		os_free(resp);
		return -ENOMEM;
	}
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: doorbell-policy\r\nConnection: close\r\n\r\n", path, host);
	if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0 || send_all(sock, req, n) != 0) {
		close(sock);
		os_free(resp);
		return -ECONNREFUSED;
	}
	while (len < POLICY_RESP_MAX - 1) {
		n = recv(sock, resp + len, POLICY_RESP_MAX - 1 - len, 0);
		if (n <= 0)
			break;
		len += n;
	}
	close(sock);
	resp[len] = 0;
	if (len > 12 && os_strncmp(resp, "HTTP/1.", 7) == 0) {
		status = (int)os_strtoul(resp + 9, NULL, 10);
		end = os_strstr(resp, "\r\n\r\n");
		if (end) {
			n = len - (int)(end + 4 - resp);
			if (n > cap - 1)
				n = cap - 1;
			os_memcpy(body, end + 4, n);
			body[n] = 0;
		}
	}
	os_free(resp);
	return status;
}

/* Tolerant flat-JSON lookup: the value of "key" (string without quotes, or the bare token of a
 * number/true/false), 1 when found. Nested objects/arrays are not understood, which is fine for the
 * policy file (one flat object). */
static int json_get(const char *j, const char *key, char *out, int cap)
{
	size_t kl = os_strlen(key);
	const char *p = j;

	out[0] = 0;
	while ((p = os_strchr(p, '"'))) {
		const char *k = p + 1;
		if (os_strncmp(k, key, kl) == 0 && k[kl] == '"') {
			const char *v = k + kl + 1;
			int n = 0;
			while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')
				v++;
			if (*v != ':') {
				p = k;
				continue;
			}
			v++;
			while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')
				v++;
			if (*v == '"') {
				v++;
				while (*v && *v != '"' && n < cap - 1) {
					if (*v == '\\' && v[1])
						v++;
					out[n++] = *v++;
				}
			} else {
				while (*v && *v != ',' && *v != '}' && *v != ']' && *v != ' ' && *v != '\r' && *v != '\n' && *v != '\t' && n < cap - 1)
					out[n++] = *v++;
			}
			out[n] = 0;
			return 1;
		}
		p = k;
	}
	return 0;
}

static bool json_bool(const char *v, bool *out)
{
	if (os_strcmp(v, "1") == 0 || os_strcmp(v, "true") == 0 || os_strcmp(v, "on") == 0) { *out = true; return true; }
	if (os_strcmp(v, "0") == 0 || os_strcmp(v, "false") == 0 || os_strcmp(v, "off") == 0) { *out = false; return true; }
	return false;
}

static bool json_uint(const char *v, uint32_t lo, uint32_t hi, uint32_t *out)
{
	char *e;
	uint32_t n;

	if (!v[0])
		return false;
	n = os_strtoul(v, &e, 10);
	if (*e == '.')   /* "60.0" from a Home Assistant input_number */
		os_strtoul(e + 1, &e, 10);
	if (*e || n < lo || n > hi)
		return false;
	*out = n;
	return true;
}

static bool parse_hhmm(const char *s, int *out)
{
	char *e;
	uint32_t h, m;

	h = os_strtoul(s, &e, 10);
	if (*e != ':' || e == s)
		return false;
	m = os_strtoul(e + 1, &e, 10);
	if (*e || h > 23 || m > 59)
		return false;
	*out = (int)(h * 60 + m);
	return true;
}

typedef struct {
	int changed, same, bad;
	char what[96];
} apply_stat_t;

static void note(apply_stat_t *st, const char *key, const char *val)
{
	size_t l = os_strlen(st->what);
	if (l < sizeof(st->what) - 2)
		snprintf(st->what + l, sizeof(st->what) - l, "%s%s=%s", l ? " " : "", key, val);
}

static void apply_cmd(apply_stat_t *st, const char *key, const char *val, const char *cmd, bool nested)
{
	if (app_exec_quiet(cmd, !nested) == 0) {
		st->changed++;
		note(st, key, val);
	} else {
		st->bad++;
	}
}

/* Apply the whitelisted keys that differ from the device's current settings, via the shell
 * commands (persisting in KV). nested = called from inside a command (`pwr policy pull`). */
static void policy_apply(const char *body, apply_stat_t *st, bool nested)
{
	char v[32], cmd[64];
	uint32_t n;
	bool b;

	os_memset(st, 0, sizeof(*st));
	if (json_get(body, "volume", v, sizeof(v))) {
		if (json_uint(v, 0, 100, &n)) {
			if (n != vol_get()) { snprintf(cmd, sizeof(cmd), "vol %u", n); apply_cmd(st, "volume", v, cmd, nested); } else st->same++;
		} else st->bad++;
	}
	if (json_get(body, "wake_pir", v, sizeof(v))) {
		if (json_bool(v, &b)) {
			if (b != power_wake_pir_get()) { snprintf(cmd, sizeof(cmd), "pwr wake_pir %s", b ? "on" : "off"); apply_cmd(st, "wake_pir", v, cmd, nested); } else st->same++;
		} else st->bad++;
	}
	if (json_get(body, "pir_sens", v, sizeof(v))) {
		if (json_uint(v, 0, 2, &n)) {
			if (n != pir_sens_get()) { snprintf(cmd, sizeof(cmd), "pir sens %u", n); apply_cmd(st, "pir_sens", v, cmd, nested); } else st->same++;
		} else st->bad++;
	}
	if (json_get(body, "dark_mv", v, sizeof(v))) {
		if (json_uint(v, 100, 4000, &n)) {
			if (n != app_dark_mv()) { snprintf(cmd, sizeof(cmd), "ring dark %u", n); apply_cmd(st, "dark_mv", v, cmd, nested); } else st->same++;
		} else st->bad++;
	}
	if (json_get(body, "night_auto", v, sizeof(v))) {
		if (json_bool(v, &b)) {
			if (b != night_auto_get()) { snprintf(cmd, sizeof(cmd), "night auto %s", b ? "on" : "off"); apply_cmd(st, "night_auto", v, cmd, nested); } else st->same++;
		} else st->bad++;
	}
	if (json_get(body, "keep_s", v, sizeof(v))) {
		if (json_uint(v, 5, 3600, &n)) {
			if (n != sd_keep_get()) { snprintf(cmd, sizeof(cmd), "sd keep %u", n); apply_cmd(st, "keep_s", v, cmd, nested); } else st->same++;
		} else st->bad++;
	}
	if (json_get(body, "sd_auto", v, sizeof(v))) {
		if (json_bool(v, &b)) {
			if (b != sd_auto_get()) { snprintf(cmd, sizeof(cmd), "sd auto %s", b ? "on" : "off"); apply_cmd(st, "sd_auto", v, cmd, nested); } else st->same++;
		} else st->bad++;
	}
	/* schedule: the four keys merge into one `pwr sched`; an explicit `pwr sched off` on the device wins */
	{
		bool on, diff = false, any = false;
		int start, end, t;
		uint32_t dm, nm;

		power_sched_params(&on, &start, &end, &dm, &nm);
		if (json_get(body, "day_start", v, sizeof(v))) { any = true; if (parse_hhmm(v, &t)) { if (t != start) { start = t; diff = true; } } else st->bad++; }
		if (json_get(body, "day_end", v, sizeof(v))) { any = true; if (parse_hhmm(v, &t)) { if (t != end) { end = t; diff = true; } } else st->bad++; }
		if (json_get(body, "day_min", v, sizeof(v))) { any = true; if (json_uint(v, 1, 1440, &n)) { if (n != dm) { dm = n; diff = true; } } else st->bad++; }
		if (json_get(body, "night_min", v, sizeof(v))) { any = true; if (json_uint(v, 1, 1440, &n)) { if (n != nm) { nm = n; diff = true; } } else st->bad++; }
		if (any && !on) {
			note(st, "sched", "skipped(off)");
		} else if (diff) {
			snprintf(cmd, sizeof(cmd), "pwr sched %02d:%02d %02d:%02d %u %u", start / 60, start % 60, end / 60, end % 60, dm, nm);
			snprintf(v, sizeof(v), "%02d:%02d-%02d:%02d/%u/%u", start / 60, start % 60, end / 60, end % 60, dm, nm);
			apply_cmd(st, "sched", v, cmd, nested);
		} else if (any) {
			st->same++;
		}
	}
}

static void policy_fetch(bool nested)
{
	char url[128], *body;
	int st;
	apply_stat_t ap;

	s_policy_at_ms = rtos_get_time();
	s_policy_rev[0] = 0;
	if (!policy_url(url, sizeof(url))) {
		s_policy_result = "none";
		os_strlcpy(s_policy_detail, os_strcmp(s_policy_cfg, "off") == 0 ? "off" : "no url (hook not set)", sizeof(s_policy_detail));
		os_printf("[policy] %s\r\n", s_policy_detail);
		return;
	}
	if (!app_net_up()) {
		s_policy_result = "error";
		os_strlcpy(s_policy_detail, "no network", sizeof(s_policy_detail));
		return;
	}
	body = os_malloc(POLICY_BODY_MAX);
	if (!body) {
		s_policy_result = "error";
		os_strlcpy(s_policy_detail, "no memory", sizeof(s_policy_detail));
		return;
	}
	st = http_get(url, body, POLICY_BODY_MAX);
	if (st != 200 || !os_strchr(body, '{')) {
		s_policy_result = "error";
		if (st > 0)
			snprintf(s_policy_detail, sizeof(s_policy_detail), "HTTP %d%s", st, st == 200 ? " (no JSON object)" : "");
		else
			snprintf(s_policy_detail, sizeof(s_policy_detail), "%s (%d)", st == 0 ? "no response" : "connect failed", st);
		os_printf("[policy] %s: %s\r\n", url, s_policy_detail);
		os_free(body);
		return;
	}
	json_get(body, "rev", s_policy_rev, sizeof(s_policy_rev));
	policy_apply(body, &ap, nested);
	os_free(body);
	s_policy_result = "applied";
	snprintf(s_policy_detail, sizeof(s_policy_detail), "%d changed %d same%s%s", ap.changed, ap.same,
		 ap.bad ? " BAD " : "", ap.bad ? "values" : "");
	os_printf("[policy] rev %s: %s%s%s\r\n", s_policy_rev[0] ? s_policy_rev : "-", s_policy_detail,
		  ap.what[0] ? " | " : "", ap.what);
}

void policy_boot_fetch(void)
{
	if (s_policy_fetched)
		return;
	s_policy_fetched = true;
	policy_fetch(false);
}

int policy_json(char *buf, int cap)
{
	return snprintf(buf, cap, ",\"policy\":\"%s\",\"policy_rev\":\"%s\"", s_policy_result, s_policy_rev);
}

void policy_cli(int argc, char **argv)
{
	char url[128];

	if (argc >= 3) {
		if (os_strcmp(argv[2], "pull") == 0) {
			policy_fetch(true);
		} else if (os_strcmp(argv[2], "auto") == 0 || os_strcmp(argv[2], "off") == 0 || os_strncmp(argv[2], "http://", 7) == 0) {
			os_strlcpy(s_policy_cfg, argv[2], sizeof(s_policy_cfg));
			db_kv_set(KV_POLICY, s_policy_cfg);
		} else {
			db_reply("pwr policy [auto | off | http://IP[:port]/path | pull]   (auto = http://<hook host>:<hook port>" POLICY_PATH ")\r\n");
			return;
		}
	}
	{
		bool off = os_strcmp(s_policy_cfg, "off") == 0, is_auto = os_strcmp(s_policy_cfg, "auto") == 0, have = !off && policy_url(url, sizeof(url));
		db_reply("[policy] %s%s | last: %s%s%s (%s)%s%u s ago\r\n",
			 off ? "off" : have ? url : "none (auto: hook not set)", have && is_auto ? " (auto)" : "",
			 s_policy_result, s_policy_rev[0] ? " rev " : "", s_policy_rev, s_policy_detail,
			 s_policy_at_ms ? " " : " never, ", s_policy_at_ms ? (rtos_get_time() - s_policy_at_ms) / 1000 : 0);
	}
}

/* ------------------------------------------------------------------ HTTP: /cmd and /log */
static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void url_decode(char *s)
{
	char *w = s;

	while (*s) {
		if (*s == '+') {
			*w++ = ' ';
			s++;
		} else if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
			*w++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
			s += 3;
		} else {
			*w++ = *s++;
		}
	}
	*w = 0;
}

/* raw value of a query parameter from "/cmd?t=..&c=.." (same rules as talk.c's query_get) */
static int query_get(const char *target, const char *key, char *out, int cap)
{
	const char *q = os_strchr(target, '?');
	size_t kl = os_strlen(key);

	out[0] = 0;
	if (!q)
		return 0;
	q++;
	while (*q) {
		const char *e = q;
		while (*e && *e != '&')
			e++;
		if ((size_t)(e - q) > kl && os_strncmp(q, key, kl) == 0 && q[kl] == '=') {
			int n = (int)(e - q - kl - 1);
			if (n > cap - 1)
				n = cap - 1;
			os_memcpy(out, q + kl + 1, n);
			out[n] = 0;
			return 1;
		}
		if (!*e)
			break;
		q = e + 1;
	}
	return 0;
}

static void http_text(int sock, int code, const char *body, int bl)
{
	char hdr[160];
	int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
			 "Cache-Control: no-cache\r\nConnection: close\r\n\r\n", code,
			 code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : "Error", bl);

	if (send_all(sock, hdr, n) == 0 && bl > 0)
		send_all(sock, body, bl);
}

static bool token_ok(const char *target)
{
	char t[64];
	const char *tok = app_cmd_token();

	if (!tok[0])
		return false;   /* no token = endpoint disabled */
	if (!query_get(target, "t", t, sizeof(t)))
		return false;
	url_decode(t);
	return os_strcmp(t, tok) == 0;
}

int policy_http_handle(int sock, const char *req, int n)
{
	char target[320];
	const char *sp, *sp2;
	int tl;

	if (n <= 0)
		return 0;
	if (os_strncmp(req, "GET /cmd", 8) != 0 && os_strncmp(req, "GET /log", 8) != 0)
		return 0;
	sp = os_strchr(req, ' ');
	sp2 = sp ? os_strchr(sp + 1, ' ') : NULL;
	if (!sp2) {   /* request line not complete in the first read */
		http_text(sock, 400, "bad request line\n", 17);
		return 1;
	}
	tl = (int)(sp2 - sp - 1);
	if (tl <= 0 || tl >= (int)sizeof(target)) {
		http_text(sock, 400, "request too long\n", 17);
		return 1;
	}
	os_memcpy(target, sp + 1, tl);
	target[tl] = 0;
	if (!(os_strncmp(target, "/cmd", 4) == 0 && (target[4] == '?' || target[4] == 0))
	    && !(os_strncmp(target, "/log", 4) == 0 && (target[4] == '?' || target[4] == 0)))
		return 0;
	if (!token_ok(target)) {
		os_printf("[http] %.4s: denied\r\n", target);
		http_text(sock, 403, "denied\n", 7);
		return 1;
	}
	if (target[1] == 'l') {   /* /log: the whole ring, oldest first */
		uint32_t w = s_log_w, have = w < APPLOG_SIZE ? w : APPLOG_SIZE, start = w - have, off = 0;
		char hdr[128];
		int hl = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %u\r\n"
				  "Cache-Control: no-cache\r\nConnection: close\r\n\r\n", have);
		if (send_all(sock, hdr, hl) != 0)
			return 1;
		while (off < have) {
			uint32_t idx = (start + off) % APPLOG_SIZE, chunk = APPLOG_SIZE - idx;
			if (chunk > have - off)
				chunk = have - off;
			if (chunk > 1024)
				chunk = 1024;
			if (send_all(sock, s_log + idx, (int)chunk) != 0)
				break;
			off += chunk;
		}
		return 1;
	}
	{   /* /cmd?c=...: the shared dispatcher, REPLY text back */
		char cmd[160], *out;
		int rc;

		if (!query_get(target, "c", cmd, sizeof(cmd)) || !cmd[0]) {
			http_text(sock, 400, "c=<command> missing\n", 20);
			return 1;
		}
		url_decode(cmd);
		out = os_malloc(640);
		if (!out) {
			http_text(sock, 500, "no memory\n", 10);
			return 1;
		}
		os_printf("[http] cmd: %s\r\n", cmd);
		rc = app_exec(cmd, out, 640);
		if (rc != 0)
			http_text(sock, 404, "unknown command\n", 16);
		else
			http_text(sock, 200, out, (int)os_strlen(out));
		os_free(out);
	}
	return 1;
}

#else
typedef int policy_c_not_built_on_this_cpu_t;
#endif /* CONFIG_SYS_CPU0 */
