/* talk.c — half-duplex two-way audio over HTTP for the doorbell (feature "talk").
 *
 * Runs inside the camera HTTP server's per-client thread (4 KB stack: every buffer here is on the
 * heap). Speaker input is streamed into the DMA engine's 16 KB ring as it arrives, so a clip is
 * consumed in real time and the HTTP reply comes when playback has finished; the microphone side
 * reads the ADC DMA ring and sends WAV until the client goes away. See talk.h for the routes. */
#include <common/bk_include.h>

#if (CONFIG_SYS_CPU0)
#include <os/os.h>
#include <os/str.h>
#include <os/mem.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "aud_engine.h"
#include "talk.h"

extern int bk_set_env_enhance(const char *key, const void *value, int value_len);
extern int bk_get_env_enhance(const char *key, void *value, int value_len);

#define KV_TALK_GAIN "db_talk_gain"       /* DAC gain for /speak; "0" = follow `spk cfg gain` */
#define KV_TALK_MICGAIN "db_talk_micgain" /* ADC gain for /mic.wav (0x3f = speech, README) */
#define KV_TALK_MAX "db_talk_max"         /* max seconds played per /speak */

#define TALK_MAX_DEFAULT_S 30
#define TALK_MAX_LIMIT_S 120
#define TALK_MIC_MAX_S 600
#define TALK_HDR_MAX 1024                 /* request header buffer */
#define TALK_IO_CHUNK 2048                /* socket read size while playing */
#define TALK_MIC_WORDS 4096               /* bytes of 32-bit mic words per /mic.wav chunk (64 ms at 16 kHz) */
#define TALK_URL_MAX 160

static uint16_t s_talk_gain;                 /* 0 = auto (spk cfg) */
static uint32_t s_talk_micgain = 0x3f;
static uint32_t s_talk_max_s = TALK_MAX_DEFAULT_S;
static volatile bool s_talk_speaking, s_talk_listening, s_talk_abort;
static uint32_t s_speak_calls, s_speak_in_bytes, s_speak_last_ms, s_speak_last_rate;
static uint32_t s_mic_calls, s_mic_out_bytes;
static int s_talk_last_err;                  /* last HTTP status we answered with */

/* ------------------------------------------------------------------ small helpers */

static int talk_send_all(int sock, const void *p, int n)
{
	int off = 0, r;
	while (off < n && (r = send(sock, (const uint8_t *)p + off, n - off > 2048 ? 2048 : n - off, 0)) > 0)
		off += r;
	return off == n ? 0 : -1;
}

static int http_reply(int sock, int code, const char *ctype, const char *body)
{
	char hdr[224];
	const char *text = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" :
			   code == 411 ? "Length Required" : code == 415 ? "Unsupported Media Type" :
			   code == 431 ? "Request Header Fields Too Large" : code == 502 ? "Bad Gateway" :
			   code == 503 ? "Service Unavailable" : "Error";
	int bl = (int)os_strlen(body);
	int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
			 "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", code, text, ctype, bl);

	s_talk_last_err = code;
	if (talk_send_all(sock, hdr, n))
		return -1;
	return talk_send_all(sock, body, bl);
}

/* value of a header line (case-insensitive name), or NULL; *hdrs is NUL-terminated */
static const char *hdr_value(const char *hdrs, const char *name)
{
	size_t nl = os_strlen(name);
	const char *p = os_strstr(hdrs, "\r\n");

	while (p) {
		p += 2;
		if (os_strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
			p += nl + 1;
			while (*p == ' ' || *p == '\t')
				p++;
			return p;
		}
		p = os_strstr(p, "\r\n");
	}
	return NULL;
}

/* query parameter from a request target ("/speak?tone=1000&ms=500"); copies the raw value */
static int query_get(const char *target, const char *key, char *out, int cap)
{
	const char *q = os_strchr(target, '?');
	size_t kl = os_strlen(key);

	out[0] = 0;
	if (!q)
		return 0;
	q++;
	while (*q && *q != ' ') {
		const char *e = q;
		while (*e && *e != '&' && *e != ' ')
			e++;
		if ((size_t)(e - q) > kl && os_strncmp(q, key, kl) == 0 && q[kl] == '=') {
			int n = (int)(e - q - kl - 1);
			if (n > cap - 1)
				n = cap - 1;
			os_memcpy(out, q + kl + 1, n);
			out[n] = 0;
			return 1;
		}
		if (!*e || *e == ' ')
			break;
		q = e + 1;
	}
	return 0;
}

static uint32_t query_uint(const char *target, const char *key, uint32_t def)
{
	char v[16];
	return query_get(target, key, v, sizeof(v)) ? (uint32_t)os_strtoul(v, NULL, 10) : def;
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void url_decode(char *s)
{
	char *w = s;
	for (; *s; s++) {
		if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
			*w++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
			s += 2;
		} else {
			*w++ = *s == '+' ? ' ' : *s;
		}
	}
	*w = 0;
}

static void kv_set_u32(const char *key, uint32_t v)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%u", v);
	bk_set_env_enhance(key, buf, os_strlen(buf));
}

static bool kv_get_u32(const char *key, uint32_t *v)
{
	char buf[16];
	int n = bk_get_env_enhance(key, buf, sizeof(buf) - 1);
	if (n <= 0 || n >= (int)sizeof(buf))
		return false;
	buf[n] = 0;
	*v = (uint32_t)os_strtoul(buf, NULL, 0);
	return true;
}

/* ------------------------------------------------------------------ body reader
 * Yields the raw bytes of an HTTP message body from a socket: a Content-Length, "until close", or
 * a chunked transfer encoding (curl sends chunked when piping stdin into --data-binary). `pre` is
 * what the caller already received past the header terminator. */
struct body {
	int sock;
	int32_t left;            /* bytes remaining for Content-Length bodies, -1 = until close */
	bool chunked, done, err;
	uint32_t chunk_left;     /* chunked: payload bytes left in the current chunk */
	int cstate;              /* 0 = expect size line, 1 = in data, 2 = CRLF after data */
	const uint8_t *pre;
	int pre_n;
	uint32_t total;
};

static int body_raw(struct body *b, uint8_t *dst, int cap)
{
	int n;
	if (b->pre_n > 0) {
		n = b->pre_n < cap ? b->pre_n : cap;
		os_memcpy(dst, b->pre, n);
		b->pre += n;
		b->pre_n -= n;
		return n;
	}
	n = recv(b->sock, dst, cap, 0);
	return n < 0 ? 0 : n;   /* timeout or error = end of body */
}

/* returns > 0 bytes, 0 at the end of the body (or on a dead connection) */
static int body_read(struct body *b, uint8_t *dst, int cap)
{
	int n;

	if (b->done)
		return 0;
	if (!b->chunked) {
		if (b->left == 0) {
			b->done = true;
			return 0;
		}
		if (b->left > 0 && cap > b->left)
			cap = b->left;
		n = body_raw(b, dst, cap);
		if (n <= 0) {
			b->done = true;
			return 0;
		}
		if (b->left > 0)
			b->left -= n;
		b->total += n;
		return n;
	}
	for (;;) {
		if (b->cstate == 0) {       /* "<hex>[;ext]\r\n": read a byte at a time, it is a dozen bytes per 16 KB */
			uint32_t size = 0;
			int digits = 0, guard = 0;
			uint8_t c;
			bool ext = false;
			for (;;) {
				if (body_raw(b, &c, 1) != 1 || ++guard > 64) {
					b->done = b->err = true;
					return 0;
				}
				if (c == '\n')
					break;
				if (c == '\r' || ext)
					continue;
				if (c == ';') {
					ext = true;
					continue;
				}
				if (hexval((char)c) < 0) {
					b->done = b->err = true;
					return 0;
				}
				size = size * 16 + (uint32_t)hexval((char)c);
				digits++;
			}
			if (!digits || size == 0) {   /* last chunk; trailers (if any) are ignored */
				b->done = true;
				return 0;
			}
			b->chunk_left = size;
			b->cstate = 1;
		}
		if (b->cstate == 1) {
			int want = (uint32_t)cap < b->chunk_left ? cap : (int)b->chunk_left;
			n = body_raw(b, dst, want);
			if (n <= 0) {
				b->done = b->err = true;
				return 0;
			}
			b->chunk_left -= n;
			b->total += n;
			if (b->chunk_left == 0)
				b->cstate = 2;
			return n;
		}
		/* cstate 2: swallow the CRLF after the chunk payload */
		{
			uint8_t crlf[2];
			int got = 0;
			while (got < 2) {
				int r = body_raw(b, crlf + got, 2 - got);
				if (r <= 0) {
					b->done = b->err = true;
					return 0;
				}
				got += r;
			}
			b->cstate = 0;
		}
	}
}

/* ------------------------------------------------------------------ PCM input: WAV or raw -> s16 mono at the DAC rate
 * A streaming parser: bytes arrive in arbitrary pieces. WAV = RIFF/WAVE chunks, we honour "fmt "
 * (PCM 16-bit, 1-2 channels, any rate >= the DAC rate) and "data" and skip the rest (LIST etc.).
 * Anything that does not start with RIFF....WAVE is raw s16le mono at the DAC rate. Resampling is
 * a box average of the input samples per output period (Q16 step), which is exact decimation for
 * 16 -> 8 kHz and a reasonable anti-aliased downsample for 44.1/48 kHz speech. */
enum { W_PROBE, W_CHUNK, W_FMT, W_SKIP, W_DATA, W_DONE };
struct pcm_in {
	int state;
	uint8_t hold[64];
	uint32_t hold_n, need;       /* header bytes being collected / bytes to skip */
	uint32_t chunk_size;
	uint32_t data_left;          /* 0xffffffff = until the end of the body */
	uint32_t in_rate, channels, frame_bytes, out_rate;
	bool fmt_ok;
	uint8_t part[4];             /* partial input frame across pieces */
	uint32_t part_n;
	int32_t acc;                 /* resampler */
	uint32_t acc_n, pos_q16, step_q16;
	uint8_t *obuf;               /* s16le output staging */
	uint32_t obuf_n, obuf_cap;
	uint32_t out_samples, max_samples;
	bool full, stopped;
	int err;                     /* HTTP status when the input is unusable */
};

static int pcm_init(struct pcm_in *c, uint32_t out_rate, uint32_t max_samples)
{
	os_memset(c, 0, sizeof(*c));
	c->out_rate = out_rate;
	c->max_samples = max_samples;
	c->need = 12;
	c->obuf_cap = 1024;
	c->obuf = os_malloc(c->obuf_cap);
	return c->obuf ? 0 : -1;
}

static void pcm_free(struct pcm_in *c)
{
	if (c->obuf)
		os_free(c->obuf);
	c->obuf = NULL;
}

static int pcm_set_format(struct pcm_in *c, uint32_t rate, uint32_t channels)
{
	if (rate < c->out_rate || rate > 96000 || channels < 1 || channels > 2)
		return -1;
	c->in_rate = rate;
	c->channels = channels;
	c->frame_bytes = 2 * channels;
	c->step_q16 = (uint32_t)(((uint64_t)rate << 16) / c->out_rate);
	c->fmt_ok = true;
	return 0;
}

static void pcm_flush_out(struct pcm_in *c)
{
	if (c->obuf_n && !c->stopped) {
		if (aud_spk_write(c->obuf, c->obuf_n) != 0)
			c->stopped = true;
	}
	c->obuf_n = 0;
}

static void pcm_emit(struct pcm_in *c, int16_t v)
{
	if (c->full || c->stopped)
		return;
	c->obuf[c->obuf_n++] = (uint8_t)v;
	c->obuf[c->obuf_n++] = (uint8_t)((uint16_t)v >> 8);
	c->out_samples++;
	if (c->obuf_n >= c->obuf_cap)
		pcm_flush_out(c);
	if (c->out_samples >= c->max_samples) {
		pcm_flush_out(c);
		c->full = true;
	}
}

static void pcm_samples(struct pcm_in *c, const uint8_t *p, uint32_t n)
{
	while (n) {
		int16_t s;
		if (c->part_n) {   /* finish a frame split across pieces */
			while (c->part_n < c->frame_bytes && n) {
				c->part[c->part_n++] = *p++;
				n--;
			}
			if (c->part_n < c->frame_bytes)
				return;
			s = (int16_t)(c->part[0] | (c->part[1] << 8));
			c->part_n = 0;
		} else if (n < c->frame_bytes) {
			os_memcpy(c->part, p, n);
			c->part_n = n;
			return;
		} else {
			s = (int16_t)(p[0] | (p[1] << 8));   /* left channel */
			p += c->frame_bytes;
			n -= c->frame_bytes;
		}
		if (c->full)
			continue;
		c->acc += s;
		c->acc_n++;
		c->pos_q16 += 65536;
		if (c->pos_q16 >= c->step_q16) {
			c->pos_q16 -= c->step_q16;
			pcm_emit(c, (int16_t)(c->acc / (int32_t)c->acc_n));
			c->acc = 0;
			c->acc_n = 0;
		}
	}
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* collect `need` header bytes into hold; returns 1 when complete */
static int pcm_collect(struct pcm_in *c, const uint8_t **p, uint32_t *n)
{
	uint32_t take = c->need - c->hold_n;
	if (take > *n)
		take = *n;
	os_memcpy(c->hold + c->hold_n, *p, take);
	c->hold_n += take;
	*p += take;
	*n -= take;
	return c->hold_n == c->need;
}

static void pcm_feed(struct pcm_in *c, const uint8_t *p, uint32_t n)
{
	while (n && !c->err && c->state != W_DONE) {
		switch (c->state) {
		case W_PROBE:
			if (!pcm_collect(c, &p, &n))
				return;
			if (os_memcmp(c->hold, "RIFF", 4) == 0 && os_memcmp(c->hold + 8, "WAVE", 4) == 0) {
				c->state = W_CHUNK;
				c->need = 8;
				c->hold_n = 0;
			} else {   /* raw s16le mono at the DAC rate */
				pcm_set_format(c, c->out_rate, 1);
				c->data_left = 0xffffffffu;
				c->state = W_DATA;
				pcm_samples(c, c->hold, c->hold_n);
				c->hold_n = 0;
			}
			break;
		case W_CHUNK:
			if (!pcm_collect(c, &p, &n))
				return;
			c->chunk_size = le32(c->hold + 4);
			c->hold_n = 0;
			if (os_memcmp(c->hold, "fmt ", 4) == 0) {
				if (c->chunk_size < 16 || c->chunk_size > 64) {
					c->err = 415;
					return;
				}
				c->need = c->chunk_size;
				c->state = W_FMT;
			} else if (os_memcmp(c->hold, "data", 4) == 0) {
				if (!c->fmt_ok) {
					c->err = 415;
					return;
				}
				c->data_left = c->chunk_size == 0 ? 0xffffffffu : c->chunk_size;
				c->state = W_DATA;
			} else {
				c->need = c->chunk_size + (c->chunk_size & 1);
				c->state = c->need ? W_SKIP : W_CHUNK;
				if (c->state == W_CHUNK)
					c->need = 8;
			}
			break;
		case W_FMT:
			if (!pcm_collect(c, &p, &n))
				return;
			{
				uint32_t tag = le16(c->hold), ch = le16(c->hold + 2), rate = le32(c->hold + 4), bits = le16(c->hold + 14);
				if ((tag != 1 && tag != 0xfffe) || bits != 16 || pcm_set_format(c, rate, ch) != 0) {
					c->err = 415;
					return;
				}
			}
			c->hold_n = 0;
			c->need = c->chunk_size & 1;
			c->state = c->need ? W_SKIP : W_CHUNK;
			if (c->state == W_CHUNK)
				c->need = 8;
			break;
		case W_SKIP: {
			uint32_t take = c->need < n ? c->need : n;
			p += take;
			n -= take;
			c->need -= take;
			if (!c->need) {
				c->state = W_CHUNK;
				c->need = 8;
			}
			break;
		}
		case W_DATA: {
			uint32_t take = c->data_left < n ? c->data_left : n;
			pcm_samples(c, p, take);
			p += take;
			n -= take;
			if (c->data_left != 0xffffffffu) {
				c->data_left -= take;
				if (!c->data_left)
					c->state = W_DONE;   /* trailing chunks (LIST, id3) are ignored */
			}
			break;
		}
		default:
			return;
		}
	}
}

/* ------------------------------------------------------------------ speaker: play a body */

/* Pulls the body through the parser into the speaker. Returns the HTTP status to answer with and
 * fills *out_ms with the played duration. The speaker must already be open. */
static int speak_play_body(struct body *b, uint32_t *out_ms, uint32_t *in_bytes, uint32_t *in_rate)
{
	struct pcm_in c;
	uint8_t *buf = os_malloc(TALK_IO_CHUNK);
	int n, status = 200;
	uint32_t t0 = rtos_get_time();

	*out_ms = 0;
	*in_bytes = 0;
	*in_rate = 0;
	if (!buf)
		return 500;
	if (pcm_init(&c, aud_spk_rate(), s_talk_max_s * aud_spk_rate()) != 0) {
		os_free(buf);
		return 500;
	}
	while (!s_talk_abort && (n = body_read(b, buf, TALK_IO_CHUNK)) > 0) {
		if (!c.err && !c.full)
			pcm_feed(&c, buf, n);
		if (c.stopped || ((c.full || c.err) && b->total > 4u * 1024 * 1024))
			break;   /* `spk stop`, or a client that keeps sending long past the limit / after a format error */
	}
	if (c.state == W_PROBE && c.hold_n) {   /* tiny raw body (< 12 bytes): still play it */
		pcm_set_format(&c, c.out_rate, 1);
		c.state = W_DATA;
		pcm_samples(&c, c.hold, c.hold_n);
	}
	if (c.err)
		status = c.err;
	else if (b->err)
		status = 400;
	else if (!c.fmt_ok)
		status = 415;
	pcm_flush_out(&c);
	*out_ms = (uint32_t)((uint64_t)c.out_samples * 1000 / c.out_rate);
	*in_bytes = b->total;
	*in_rate = c.in_rate;
	os_printf("[talk] speak: %u body bytes (%s, %u Hz x%u) -> %u samples = %u ms in %u ms%s%s\r\n", b->total,
		  b->chunked ? "chunked" : "plain", c.in_rate, c.channels, c.out_samples, *out_ms, rtos_get_time() - t0,
		  c.full ? ", truncated at max" : "", c.stopped ? ", stopped" : "");
	pcm_free(&c);
	os_free(buf);
	return status;
}

static void speak_reply(int sock, int status, uint32_t ms, uint32_t in_bytes, uint32_t in_rate, const char *what)
{
	char body[192];
	uint32_t df, ur;

	aud_spk_stats(&df, &ur);
	if (status == 200)
		snprintf(body, sizeof(body), "{\"ok\":true,\"source\":\"%s\",\"ms\":%u,\"in_bytes\":%u,\"in_rate\":%u,\"dma_frames\":%u,\"underruns\":%u}\n",
			 what, ms, in_bytes, in_rate, df, ur);
	else
		snprintf(body, sizeof(body), "{\"ok\":false,\"source\":\"%s\",\"status\":%d,\"ms\":%u,\"in_bytes\":%u,\"hint\":\"%s\"}\n", what, status, ms, in_bytes,
			 status == 415 ? "need PCM 16-bit WAV (>= 8 kHz, 1-2 ch) or raw s16le mono 8 kHz" :
			 status == 503 ? "speaker busy" : status == 502 ? "fetch failed" : "bad request");
	http_reply(sock, status, "application/json", body);
}

static int speak_open(int sock, const char *what)
{
	int rc = aud_spk_open(s_talk_gain);

	if (rc == -1) {
		speak_reply(sock, 503, 0, 0, 0, what);
		return -1;
	}
	if (rc < 0) {
		char b[64];
		snprintf(b, sizeof(b), "{\"ok\":false,\"status\":500,\"dac\":%d}\n", rc);
		http_reply(sock, 500, "application/json", b);
		return -1;
	}
	s_talk_speaking = true;
	s_talk_abort = false;
	s_speak_calls++;
	return 0;
}

static void speak_close(uint32_t ms, uint32_t in_bytes, uint32_t in_rate)
{
	aud_spk_close();
	s_talk_speaking = false;
	s_speak_last_ms = ms;
	s_speak_in_bytes += in_bytes;
	s_speak_last_rate = in_rate;
}

/* POST /speak: the body is the audio */
static void speak_post(int sock, const char *hdrs, const uint8_t *pre, int pre_n, bool http10)
{
	struct body b = {.sock = sock, .left = -1, .pre = pre, .pre_n = pre_n};
	const char *v;
	uint32_t ms, in_bytes, in_rate;
	int status;

	if ((v = hdr_value(hdrs, "Transfer-Encoding")) && os_strncasecmp(v, "chunked", 7) == 0)
		b.chunked = true;
	else if ((v = hdr_value(hdrs, "Content-Length")))
		b.left = (int32_t)os_strtoul(v, NULL, 10);
	else if (!http10) {
		http_reply(sock, 411, "text/plain", "Content-Length or chunked body required\n");
		return;
	}
	if (speak_open(sock, "post") != 0)
		return;
	if ((v = hdr_value(hdrs, "Expect")) && os_strncasecmp(v, "100-continue", 12) == 0)
		talk_send_all(sock, "HTTP/1.1 100 Continue\r\n\r\n", 25);
	status = speak_play_body(&b, &ms, &in_bytes, &in_rate);
	speak_close(ms, in_bytes, in_rate);
	speak_reply(sock, status, ms, in_bytes, in_rate, "post");
}

/* GET /speak?tone=HZ&ms=MS */
static void speak_tone(int sock, uint32_t hz, uint32_t ms)
{
	uint32_t rate = aud_spk_rate(), total, i = 0, phase = 0, step;
	uint8_t *buf;
	int16_t *s;

	if (hz < 50 || hz > rate / 2)
		hz = 1000;
	if (ms < 10)
		ms = 500;
	if (ms > s_talk_max_s * 1000)
		ms = s_talk_max_s * 1000;
	if (speak_open(sock, "tone") != 0)
		return;
	buf = os_malloc(1024);
	if (buf) {
		static int16_t sine[64];
		static bool sine_ok;
		if (!sine_ok) {
			for (int k = 0; k < 64; k++)
				sine[k] = (int16_t)(sinf(2.0f * 3.14159265f * (float)k / 64.0f) * 8191.0f);   /* -12 dBFS like `spk tone` */
			sine_ok = true;
		}
		total = ms * (rate / 1000);
		step = (uint32_t)(((uint64_t)hz * 64u * 65536u) / rate);
		s = (int16_t *)buf;
		while (i < total && !s_talk_abort) {
			uint32_t k, n = total - i < 512 ? total - i : 512;
			for (k = 0; k < n; k++) {
				s[k] = sine[(phase >> 16) & 63];
				phase += step;
			}
			if (aud_spk_write(buf, 2 * n) != 0)
				break;
			i += n;
		}
		os_free(buf);
	}
	speak_close(i * 1000 / rate, 0, rate);
	os_printf("[talk] tone %u Hz: %u samples\r\n", hz, i);
	speak_reply(sock, 200, i * 1000 / rate, 0, rate, "tone");
}

/* GET /speak?url=http://host[:port]/path : fetch with a minimal HTTP/1.0 client and play */
static void speak_url(int sock, char *url)
{
	char host[48], req[256], *path, *colon, *hp;
	uint16_t port = 80;
	int fs, n, hl = 0, status = 502;
	uint32_t ms = 0, in_bytes = 0, in_rate = 0;
	uint8_t *hdr;
	struct sockaddr_in sa = {0};
	struct timeval tv = {.tv_sec = 10, .tv_usec = 0};

	url_decode(url);
	if (os_strncmp(url, "http://", 7) != 0) {
		http_reply(sock, 400, "text/plain", "url must be http://IP[:port]/path\n");
		return;
	}
	hp = url + 7;
	path = os_strchr(hp, '/');
	if (!path) {
		http_reply(sock, 400, "text/plain", "url needs a path\n");
		return;
	}
	n = (int)(path - hp);
	if (n >= (int)sizeof(host))
		n = sizeof(host) - 1;
	os_memcpy(host, hp, n);
	host[n] = 0;
	if ((colon = os_strchr(host, ':'))) {
		*colon = 0;
		port = (uint16_t)os_strtoul(colon + 1, NULL, 10);
	}
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = inet_addr(host);
	if (sa.sin_addr.s_addr == INADDR_NONE) {
		http_reply(sock, 400, "text/plain", "url host must be an IPv4 address (no DNS)\n");
		return;
	}
	hdr = os_malloc(TALK_HDR_MAX);
	if (!hdr) {
		http_reply(sock, 500, "text/plain", "no memory\n");
		return;
	}
	fs = socket(AF_INET, SOCK_STREAM, 0);
	if (fs < 0) {
		os_free(hdr);
		http_reply(sock, 500, "text/plain", "socket\n");
		return;
	}
	setsockopt(fs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fs, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (connect(fs, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		os_printf("[talk] url: connect %s:%u failed\r\n", host, port);
		goto fail;
	}
	n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: doorbell-talk\r\nConnection: close\r\n\r\n", path, host);
	if (talk_send_all(fs, req, n))
		goto fail;
	/* response header */
	while (hl < TALK_HDR_MAX - 1) {
		n = recv(fs, hdr + hl, TALK_HDR_MAX - 1 - hl, 0);
		if (n <= 0)
			break;
		hl += n;
		hdr[hl] = 0;
		if (os_strstr((char *)hdr, "\r\n\r\n"))
			break;
	}
	{
		char *end = hl ? os_strstr((char *)hdr, "\r\n\r\n") : NULL;
		int code;
		if (!end || hl < 12) {
			os_printf("[talk] url: no response header from %s:%u\r\n", host, port);
			goto fail;
		}
		code = (int)os_strtoul((char *)hdr + 9, NULL, 10);   /* "HTTP/1.x NNN" */
		if (os_strncmp((char *)hdr, "HTTP/1.", 7) != 0 || code != 200) {
			os_printf("[talk] url: %s:%u%s -> HTTP %d\r\n", host, port, path, code);
			goto fail;
		}
		end[2] = 0;   /* keep the final CRLF for hdr_value's line scan */
		{
			struct body b = {.sock = fs, .left = -1, .pre = (uint8_t *)end + 4, .pre_n = hl - (int)(end + 4 - (char *)hdr)};
			const char *v;
			if ((v = hdr_value((char *)hdr, "Transfer-Encoding")) && os_strncasecmp(v, "chunked", 7) == 0)
				b.chunked = true;
			else if ((v = hdr_value((char *)hdr, "Content-Length")))
				b.left = (int32_t)os_strtoul(v, NULL, 10);
			if (speak_open(sock, "url") != 0) {
				close(fs);
				os_free(hdr);
				return;
			}
			status = speak_play_body(&b, &ms, &in_bytes, &in_rate);
			speak_close(ms, in_bytes, in_rate);
		}
	}
	close(fs);
	os_free(hdr);
	speak_reply(sock, status, ms, in_bytes, in_rate, "url");
	return;
fail:
	close(fs);
	os_free(hdr);
	speak_reply(sock, 502, 0, 0, 0, "url");
}

/* ------------------------------------------------------------------ microphone: /mic.wav */

static void wav_header(uint8_t *h, uint32_t rate, uint32_t data_bytes)
{
	uint32_t riff = data_bytes == 0xffffffffu ? 0xffffffffu : data_bytes + 36, br = rate * 2;
	os_memcpy(h, "RIFF", 4);
	h[4] = riff; h[5] = riff >> 8; h[6] = riff >> 16; h[7] = riff >> 24;
	os_memcpy(h + 8, "WAVEfmt ", 8);
	h[16] = 16; h[17] = 0; h[18] = 0; h[19] = 0;    /* fmt size */
	h[20] = 1; h[21] = 0;                           /* PCM */
	h[22] = 1; h[23] = 0;                           /* mono */
	h[24] = rate; h[25] = rate >> 8; h[26] = rate >> 16; h[27] = rate >> 24;
	h[28] = br; h[29] = br >> 8; h[30] = br >> 16; h[31] = br >> 24;
	h[32] = 2; h[33] = 0;                           /* block align */
	h[34] = 16; h[35] = 0;                          /* bits */
	os_memcpy(h + 36, "data", 4);
	h[40] = data_bytes; h[41] = data_bytes >> 8; h[42] = data_bytes >> 16; h[43] = data_bytes >> 24;
}

static int send_piece(int sock, bool chunked, const uint8_t *p, uint32_t n)
{
	char pre[12];
	if (!n)
		return 0;
	if (chunked) {
		int pl = snprintf(pre, sizeof(pre), "%x\r\n", n);
		if (talk_send_all(sock, pre, pl))
			return -1;
	}
	if (talk_send_all(sock, p, (int)n))
		return -1;
	return chunked ? talk_send_all(sock, "\r\n", 2) : 0;
}

static void mic_get(int sock, uint32_t seconds, bool http10)
{
	uint32_t rate = aud_mic_rate(), limit, sent = 0, idle = 0, muted = 0, pieces = 0, t0;
	bool chunked = !http10 && seconds == 0;
	uint8_t *words, *pcm, hdr[44];
	char http[224];
	int rc, n;

	if (seconds > TALK_MIC_MAX_S)
		seconds = TALK_MIC_MAX_S;
	limit = seconds ? seconds * rate * 2 : 0xffffffffu;
	rc = aud_mic_open(s_talk_micgain);
	if (rc == -1) {
		http_reply(sock, 503, "text/plain", "microphone busy\n");
		return;
	}
	if (rc < 0) {
		http_reply(sock, 500, "text/plain", "adc init failed\n");
		return;
	}
	words = os_malloc(TALK_MIC_WORDS);
	pcm = os_malloc(TALK_MIC_WORDS / 2);
	if (!words || !pcm) {
		aud_mic_close();
		if (words) os_free(words);
		if (pcm) os_free(pcm);
		http_reply(sock, 500, "text/plain", "no memory\n");
		return;
	}
	s_talk_listening = true;
	s_talk_abort = false;
	s_mic_calls++;
	s_talk_last_err = 200;
	if (seconds)
		n = snprintf(http, sizeof(http), "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %u\r\n"
			     "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", 44 + limit);
	else
		n = snprintf(http, sizeof(http), "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\n%s"
			     "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
			     chunked ? "Transfer-Encoding: chunked\r\n" : "");
	wav_header(hdr, rate, seconds ? limit : 0xffffffffu);
	t0 = rtos_get_time();
	if (talk_send_all(sock, http, n) || send_piece(sock, chunked, hdr, sizeof(hdr)))
		goto done;
	while (sent < limit && !s_talk_abort) {
		uint32_t got = aud_mic_read(words, TALK_MIC_WORDS), samples, k;
		bool mute;
		if (!got) {
			if (++idle > 300)   /* 3 s without a DMA frame: the capture died */
				break;
			rtos_delay_milliseconds(10);
			continue;
		}
		idle = 0;
		samples = got / 4;
		if (samples * 2 > limit - sent)
			samples = (limit - sent) / 2;
		mute = aud_spk_busy();   /* half duplex: silence while the speaker plays (talk, chime, spk) */
		for (k = 0; k < samples; k++) {   /* lo16 of each FIFO word = MIC1 */
			pcm[2 * k] = mute ? 0 : words[4 * k];
			pcm[2 * k + 1] = mute ? 0 : words[4 * k + 1];
		}
		if (mute)
			muted++;
		if (send_piece(sock, chunked, pcm, samples * 2))
			break;
		sent += samples * 2;
		pieces++;
	}
	if (chunked)
		talk_send_all(sock, "0\r\n\r\n", 5);
done:
	aud_mic_close();
	s_talk_listening = false;
	s_mic_out_bytes += sent;
	{
		uint32_t df, ovr;
		aud_mic_stats(&df, &ovr);
		os_printf("[talk] mic.wav: %u bytes in %u pieces (%u muted) over %u ms, %u dma frames, %u overruns%s\r\n", sent, pieces,
			  muted, rtos_get_time() - t0, df, ovr, s_talk_abort ? ", aborted" : "");
	}
	os_free(words);
	os_free(pcm);
}

/* ------------------------------------------------------------------ browser page */
static const char s_talk_page[] =
	"<!doctype html><html><head><meta name=viewport content='width=device-width'><title>doorbell talk</title></head>"
	"<body style='font-family:sans-serif'><h3>doorbell talk</h3>"
	"<p>Listen: <audio controls preload=none src='/mic.wav'></audio> (one listener at a time; VLC: open /mic.wav)</p>"
	"<p><button onclick=\"go('/speak?tone=1000&ms=500')\">tone 1 kHz</button> "
	"<button onclick=\"go('/speak?tone=440&ms=1500')\">tone 440 Hz</button></p>"
	"<p>Play a WAV/raw file: <input type=file id=f> <button onclick=\"send()\">send</button></p>"
	"<p>Fetch and play: <input id=u size=50 value='http://192.168.2.80:8123/local/doorbell/say.wav'> "
	"<button onclick=\"go('/speak?url='+encodeURIComponent(u.value))\">play url</button></p>"
	"<pre id=o></pre><script>"
	"function show(r){r.text().then(t=>o.textContent=r.status+' '+t)}"
	"function go(p){o.textContent='...';fetch(p).then(show)}"
	"function send(){if(!f.files[0])return;o.textContent='sending';"
	"fetch('/speak',{method:'POST',body:f.files[0]}).then(show)}"
	"</script></body></html>";

/* ------------------------------------------------------------------ entry points */

int talk_http_handle(int sock, const char *req, int n)
{
	char *hdr, *end, target[TALK_URL_MAX + 16];
	int hl = 0, tl;
	bool post, http10;
	const char *sp, *sp2;

	if (n <= 0)
		return 0;
	post = os_strncmp(req, "POST /speak", 11) == 0;
	if (!post && os_strncmp(req, "GET /speak", 10) != 0 && os_strncmp(req, "GET /mic.wav", 12) != 0 && os_strncmp(req, "GET /talk", 9) != 0)
		return 0;
	hdr = os_malloc(TALK_HDR_MAX);
	if (!hdr) {
		http_reply(sock, 500, "text/plain", "no memory\n");
		return 1;
	}
	if (n > TALK_HDR_MAX - 1)
		n = TALK_HDR_MAX - 1;
	os_memcpy(hdr, req, n);
	hl = n;
	hdr[hl] = 0;
	while (!(end = os_strstr(hdr, "\r\n\r\n")) && hl < TALK_HDR_MAX - 1) {   /* the rest of the header */
		int r = recv(sock, hdr + hl, TALK_HDR_MAX - 1 - hl, 0);
		if (r <= 0)
			break;
		hl += r;
		hdr[hl] = 0;
	}
	if (!end) {
		http_reply(sock, hl >= TALK_HDR_MAX - 1 ? 431 : 400, "text/plain", "bad request header\n");
		os_free(hdr);
		return 1;
	}
	/* request target and version */
	sp = os_strchr(hdr, ' ');
	sp2 = sp ? os_strchr(sp + 1, ' ') : NULL;
	tl = sp2 ? (int)(sp2 - sp - 1) : 0;
	if (!sp2 || tl <= 0 || tl >= (int)sizeof(target)) {
		http_reply(sock, tl >= (int)sizeof(target) ? 414 : 400, "text/plain", "bad request line\n");
		os_free(hdr);
		return 1;
	}
	os_memcpy(target, sp + 1, tl);
	target[tl] = 0;
	http10 = os_strncmp(sp2 + 1, "HTTP/1.0", 8) == 0;
	end[2] = 0;   /* header block ends with one CRLF; body bytes start at end + 4 */

	if (os_strncmp(target, "/talk", 5) == 0) {
		http_reply(sock, 200, "text/html", s_talk_page);
	} else if (os_strncmp(target, "/mic.wav", 8) == 0) {
		mic_get(sock, query_uint(target, "seconds", 0), http10);
	} else if (post) {
		speak_post(sock, hdr, (const uint8_t *)end + 4, hl - (int)(end + 4 - hdr), http10);
	} else {
		char url[TALK_URL_MAX];
		if (query_get(target, "url", url, sizeof(url)))
			speak_url(sock, url);
		else if (query_get(target, "tone", url, sizeof(url)))
			speak_tone(sock, (uint32_t)os_strtoul(url, NULL, 10), query_uint(target, "ms", 500));
		else
			http_reply(sock, 400, "text/plain", "POST /speak (raw s16le mono 8 kHz or WAV) | GET /speak?tone=HZ&ms=MS | GET /speak?url=http://IP[:port]/file.wav | GET /mic.wav[?seconds=N] | GET /talk\n");
	}
	os_free(hdr);
	return 1;
}

void talk_init(void)
{
	uint32_t v;

	if (kv_get_u32(KV_TALK_GAIN, &v))
		s_talk_gain = (uint16_t)v;
	if (kv_get_u32(KV_TALK_MICGAIN, &v))
		s_talk_micgain = v;
	if (kv_get_u32(KV_TALK_MAX, &v) && v >= 1 && v <= TALK_MAX_LIMIT_S)
		s_talk_max_s = v;
	os_printf("[talk] http://IP:8080/speak /mic.wav /talk: gain %s0x%x micgain 0x%x max %u s\r\n",
		  s_talk_gain ? "" : "auto=", s_talk_gain ? s_talk_gain : aud_spk_gain(), s_talk_micgain, s_talk_max_s);
}

void cli_talk_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	if (argc >= 2 && os_strcmp(argv[1], "status") == 0) {
		app_reply("[talk] %s%s%s | speak: %u calls, last %u ms (in %u Hz), %u bytes in | mic: %u calls, %u bytes out, %u Hz | last http %d | gain %s0x%x micgain 0x%x max %u s\r\n",
			  s_talk_speaking ? "speaking" : "", s_talk_listening ? (s_talk_speaking ? "+listening" : "listening") : "",
			  !s_talk_speaking && !s_talk_listening ? "idle" : "", s_speak_calls, s_speak_last_ms, s_speak_last_rate,
			  s_speak_in_bytes, s_mic_calls, s_mic_out_bytes, aud_mic_rate(), s_talk_last_err,
			  s_talk_gain ? "" : "auto=", s_talk_gain ? s_talk_gain : aud_spk_gain(), s_talk_micgain, s_talk_max_s);
	} else if (argc >= 3 && os_strcmp(argv[1], "gain") == 0) {
		uint32_t g = os_strcmp(argv[2], "auto") == 0 ? 0 : (uint32_t)os_strtoul(argv[2], NULL, 0);
		if (g > 0x3f) {
			app_reply("[talk] gain 0..0x3f or auto (0x1c = loudest clean level, above 0x1c the amplifier clips)\r\n");
			return;
		}
		s_talk_gain = (uint16_t)g;
		kv_set_u32(KV_TALK_GAIN, g);
		app_reply("[talk] speaker gain %s0x%x\r\n", g ? "" : "auto = spk cfg ", g ? g : aud_spk_gain());
	} else if (argc >= 3 && os_strcmp(argv[1], "micgain") == 0) {
		uint32_t g = (uint32_t)os_strtoul(argv[2], NULL, 0);
		if (g > 0x3f) {
			app_reply("[talk] micgain 0..0x3f\r\n");
			return;
		}
		s_talk_micgain = g;
		kv_set_u32(KV_TALK_MICGAIN, g);
		app_reply("[talk] mic gain 0x%x (next /mic.wav)\r\n", g);
	} else if (argc >= 3 && os_strcmp(argv[1], "max") == 0) {
		uint32_t s = (uint32_t)os_strtoul(argv[2], NULL, 10);
		if (s < 1 || s > TALK_MAX_LIMIT_S) {
			app_reply("[talk] max 1..%u seconds\r\n", TALK_MAX_LIMIT_S);
			return;
		}
		s_talk_max_s = s;
		kv_set_u32(KV_TALK_MAX, s);
		app_reply("[talk] max %u s per /speak\r\n", s);
	} else if (argc >= 2 && os_strcmp(argv[1], "stop") == 0) {
		s_talk_abort = true;
		app_reply("[talk] stop requested (speaking %d listening %d)\r\n", s_talk_speaking, s_talk_listening);
	} else {
		app_reply("talk status | gain N|auto | micgain N | max SECONDS | stop   (http://IP:8080/speak /mic.wav /talk)\r\n");
	}
}

#endif /* CONFIG_SYS_CPU0 */
