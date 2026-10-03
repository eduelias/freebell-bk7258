/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: minimal RTSP/1.0 + RTP/H.264 (RFC 6184) server for the hardware encoder. */
/* rtsp.c - minimal RTSP/1.0 + RTP/H.264 (RFC 6184) server. See rtsp.h for the overview.
 *
 * Threads:  "rtsp"  listener (select on the listening socket, 200 ms tick for housekeeping:
 *                   fps/kbps window, rate-limited IDR requests);
 *           "rtspc" one per client: RTSP control on the TCP socket (select, zero timeout
 *                   while playing) and the RTP sender for that client.
 * Producer: rtsp_feed_frame() runs on the media transfer task (2 KB stack: keep locals small).
 *
 * Frame ring: one PSRAM byte ring + 32 descriptors. The producer writes each access unit
 * contiguously (wrapping to 0 when it would not fit at the end), invalidating the descriptors
 * it overwrites. A client marks the frame it is sending as "busy"; the producer never overwrites
 * a frame whose sequence is >= the oldest busy one - if that is the only space left it drops the
 * new frame instead and requests an IDR, so a stuck client can only ever hurt itself and the
 * stream resynchronises at the next IDR. A client that finds its next frame gone jumps to the
 * newest frame and waits for a keyframe.
 *
 * Only built for the AVDK CPU0 image (the camera lives there); empty otherwise. */
#include <common/bk_include.h>

#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)

#include <os/os.h>
#include <os/str.h>
#include <os/mem.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "rtsp.h"

#define RTSP_MAX_CLIENTS 3
#define RTSP_RING_SIZE (128 * 1024)      /* ~1 s of video at 1 Mbit/s; PSRAM heap on CPU0 is only 512 KB */
#define RTSP_FRAME_MAX (96 * 1024)       /* larger access units are dropped (CPU1 buffer is 128 KB) */
#define RTSP_NDESC 32
#define RTSP_MTU 1400                    /* RTP packet size incl. the 12-byte header */
#define RTSP_RTP_PAYLOAD (RTSP_MTU - 12)
#define RTSP_RX_MAX 1024                 /* one RTSP request */
#define RTSP_TX_BUF 8192                 /* per-client staging buffer (TCP: ~5 packets per send) */
#define RTSP_UDP_BASE_PORT 6970          /* server_port for UDP clients: base + 2 * slot */
#define RTSP_SESSION_TIMEOUT_S 60        /* advertised; UDP sessions are dropped after it + 15 s without a request */
#define RTSP_PT 96
#define RTSP_MAX_NALS 24                 /* per access unit (SPS, PPS, slices; the encoder emits a handful) */

typedef struct {
	uint32_t seq, off, len, ts90;
	uint8_t key, valid;
} rtsp_desc_t;

typedef struct {
	int sock;                     /* RTSP control socket; also carries interleaved RTP */
	int udp;                      /* RTP/UDP socket or -1 */
	struct sockaddr_in peer_rtp;  /* UDP destination */
	uint8_t used, stop, setup, playing, tcp, chan, need_key, idx;
	uint16_t rtp_seq;
	uint32_t ssrc, session;
	uint32_t next_seq;            /* ring sequence we want next (0 = not started) */
	uint32_t busy_seq;            /* ring sequence being sent right now (0 = none) */
	uint32_t last_rx_ms, start_ms;
	uint32_t sent_frames, sent_bytes, lost, udp_err;
	beken_semaphore_t sem;        /* "a new frame is in the ring" (count 1) */
	uint8_t *tx;                  /* RTSP_TX_BUF */
	int rxlen;
	char rx[RTSP_RX_MAX];
	char peer[16];
} rtsp_client_t;

static rtsp_client_t s_cl[RTSP_MAX_CLIENTS];
static beken_mutex_t s_lock;          /* ring metadata + client table */
static uint8_t *s_ring;
static uint32_t s_head;               /* next write offset */
static rtsp_desc_t s_desc[RTSP_NDESC];
static uint32_t s_seq;                /* sequence of the newest frame in the ring (0 = none) */
static volatile bool s_run, s_running;
static uint16_t s_port = RTSP_DEFAULT_PORT;
static int s_nclients;
static uint8_t s_sps[64], s_pps[64];
static volatile uint8_t s_sps_len, s_pps_len;
static volatile uint32_t s_idr_req, s_idr_last_ms;
/* stats */
static volatile uint32_t s_in_frames, s_in_bytes, s_in_key, s_drop_busy, s_drop_big, s_last_frame_ms;
static uint32_t s_win_frames, s_win_bytes, s_win_ms, s_fps10, s_kbps;

/* ---- small helpers ---- */

static uint32_t now_ms(void)
{
	return rtos_get_time();
}

static uint32_t mix32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

static void ip_str(uint32_t addr_be, char *out, size_t cap)
{
	uint32_t a = ntohl(addr_be);
	snprintf(out, cap, "%u.%u.%u.%u", (unsigned)(a >> 24) & 255, (unsigned)(a >> 16) & 255, (unsigned)(a >> 8) & 255, (unsigned)a & 255);
}

static int send_all(int sock, const void *p, int n)
{
	int off = 0, r;

	while (off < n) {
		r = send(sock, (const uint8_t *)p + off, n - off, 0);
		if (r <= 0)
			return -1;
		off += r;
	}
	return 0;
}

static int b64(const uint8_t *in, int n, char *out, int cap)
{
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int i, o = 0;

	if (cap < ((n + 2) / 3) * 4 + 1)
		return 0;
	for (i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < n)
			v |= (uint32_t)in[i + 1] << 8;
		if (i + 2 < n)
			v |= in[i + 2];
		out[o++] = tbl[(v >> 18) & 63];
		out[o++] = tbl[(v >> 12) & 63];
		out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < n ? tbl[v & 63] : '=';
	}
	out[o] = 0;
	return o;
}

/* Annex-B: position of the next 00 00 01 at or after `from`, or len. */
static uint32_t find_start(const uint8_t *b, uint32_t len, uint32_t from)
{
	uint32_t i;

	for (i = from; i + 2 < len; i++)
		if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
			return i;
	return len;
}

/* Split an access unit into NAL units (start/length without start codes). Returns the count. */
static int split_nals(const uint8_t *b, uint32_t len, uint32_t *start, uint32_t *nlen, int cap)
{
	uint32_t p = find_start(b, len, 0), s, e;
	int n = 0;

	while (p < len && n < cap) {
		s = p + 3;
		e = find_start(b, len, s);
		p = e;
		while (e > s && b[e - 1] == 0)   /* 4-byte start codes / trailing zeros belong to the next one */
			e--;
		if (e > s) {
			start[n] = s;
			nlen[n] = e - s;
			n++;
		}
	}
	return n;
}

/* ---- producer: called from the camera frame callback ---- */

void rtsp_feed_frame(const uint8_t *data, uint32_t len, uint32_t ts_ms)
{
	uint32_t off, end, seq, busy_min = 0, i;
	bool key = false, conflict = false;
	rtsp_desc_t *d;

	if (!s_running || !s_ring || !data || len == 0)
		return;
	if (len > RTSP_FRAME_MAX) {
		s_drop_big++;
		return;
	}
	/* NAL scan: cache SPS/PPS for the SDP, detect IDR frames */
	{
		uint32_t p = find_start(data, len, 0), s, e;
		while (p < len) {
			s = p + 3;
			e = find_start(data, len, s);
			p = e;
			while (e > s && data[e - 1] == 0)
				e--;
			if (e <= s)
				continue;
			switch (data[s] & 0x1f) {
			case 5:
				key = true;
				break;
			case 7:
				if (e - s <= sizeof(s_sps)) {
					rtos_lock_mutex(&s_lock);
					os_memcpy(s_sps, data + s, e - s);
					s_sps_len = e - s;
					rtos_unlock_mutex(&s_lock);
				}
				break;
			case 8:
				if (e - s <= sizeof(s_pps)) {
					rtos_lock_mutex(&s_lock);
					os_memcpy(s_pps, data + s, e - s);
					s_pps_len = e - s;
					rtos_unlock_mutex(&s_lock);
				}
				break;
			default:
				break;
			}
		}
	}

	seq = s_seq + 1;
	rtos_lock_mutex(&s_lock);
	off = s_head;
	if (off + len > RTSP_RING_SIZE)
		off = 0;
	end = off + len;
	for (i = 0; i < RTSP_MAX_CLIENTS; i++)
		if (s_cl[i].used && s_cl[i].busy_seq && (!busy_min || s_cl[i].busy_seq < busy_min))
			busy_min = s_cl[i].busy_seq;
	d = &s_desc[seq % RTSP_NDESC];
	if (busy_min && d->valid && d->seq >= busy_min)
		conflict = true;
	for (i = 0; i < RTSP_NDESC && !conflict; i++) {
		rtsp_desc_t *o = &s_desc[i];
		if (o->valid && o->off < end && o->off + o->len > off && busy_min && o->seq >= busy_min)
			conflict = true;
	}
	if (conflict) {
		for (i = 0; i < RTSP_MAX_CLIENTS; i++)   /* the stream now has a gap: everyone waits for the next IDR */
			if (s_cl[i].used)
				s_cl[i].need_key = 1;
		rtos_unlock_mutex(&s_lock);
		s_drop_busy++;
		s_idr_req = 1;
		return;
	}
	for (i = 0; i < RTSP_NDESC; i++) {
		rtsp_desc_t *o = &s_desc[i];
		if (o->valid && o->off < end && o->off + o->len > off)
			o->valid = 0;
	}
	d->valid = 0;
	rtos_unlock_mutex(&s_lock);

	os_memcpy(s_ring + off, data, len);

	rtos_lock_mutex(&s_lock);
	d->seq = seq;
	d->off = off;
	d->len = len;
	d->ts90 = (ts_ms ? ts_ms : now_ms()) * 90U;
	d->key = key;
	d->valid = 1;
	s_head = end;
	s_seq = seq;
	s_in_frames++;
	s_in_bytes += len;
	if (key)
		s_in_key++;
	s_last_frame_ms = now_ms();
	for (i = 0; i < RTSP_MAX_CLIENTS; i++)
		if (s_cl[i].used && s_cl[i].playing)
			rtos_set_semaphore(&s_cl[i].sem);   /* fails harmlessly when already signalled */
	rtos_unlock_mutex(&s_lock);
}

void rtsp_request_idr(void)
{
	s_idr_req = 1;
}

/* ---- RTP packetization (RFC 6184, packetization-mode=1) ---- */

static void rtp_header(uint8_t *p, rtsp_client_t *c, int marker, uint32_t ts)
{
	p[0] = 0x80;
	p[1] = RTSP_PT | (marker ? 0x80 : 0);
	p[2] = c->rtp_seq >> 8;
	p[3] = c->rtp_seq & 0xff;
	p[4] = ts >> 24;
	p[5] = ts >> 16;
	p[6] = ts >> 8;
	p[7] = ts;
	p[8] = c->ssrc >> 24;
	p[9] = c->ssrc >> 16;
	p[10] = c->ssrc >> 8;
	p[11] = c->ssrc;
	c->rtp_seq++;
}

/* Queue one RTP packet (payload already placed at pkt+12 .. pkt+12+plen by the caller through
 * rtp_slot()). TCP: staged in c->tx with the 4-byte interleaved header, flushed when full.
 * UDP: sent right away. Returns 0 or -1 (fatal for the client). */
static int tcp_flush(rtsp_client_t *c, int *fill)
{
	if (*fill == 0)
		return 0;
	if (send_all(c->sock, c->tx, *fill) < 0)
		return -1;
	c->sent_bytes += *fill;
	*fill = 0;
	return 0;
}

/* Where the next packet goes: returns a pointer to 12 + RTSP_RTP_PAYLOAD bytes of room. */
static uint8_t *rtp_slot(rtsp_client_t *c, int *fill)
{
	if (c->tcp) {
		if (*fill + 4 + RTSP_MTU > RTSP_TX_BUF && tcp_flush(c, fill) < 0)
			return NULL;
		return c->tx + *fill + 4;
	}
	return c->tx;
}

static int rtp_commit(rtsp_client_t *c, int *fill, int plen, int *udp_burst)
{
	int n = 12 + plen;

	if (c->tcp) {
		uint8_t *h = c->tx + *fill;
		h[0] = '$';
		h[1] = c->chan;
		h[2] = n >> 8;
		h[3] = n & 0xff;
		*fill += 4 + n;
		return 0;
	}
	for (int tries = 0; tries < 6; tries++) {
		int r = sendto(c->udp, c->tx, n, 0, (struct sockaddr *)&c->peer_rtp, sizeof(c->peer_rtp));
		if (r == n) {
			c->sent_bytes += n;
			if (++(*udp_burst) % 8 == 0)   /* pace IDR bursts a little: lwIP has few pbufs */
				rtos_delay_milliseconds(1);
			return 0;
		}
		if (r < 0 && (errno == ENOMEM || errno == EAGAIN || errno == EWOULDBLOCK)) {
			rtos_delay_milliseconds(2);
			continue;
		}
		break;
	}
	c->udp_err++;   /* lost packet: the receiver conceals until the next IDR */
	return 0;
}

static int send_access_unit(rtsp_client_t *c, const uint8_t *au, uint32_t len, uint32_t ts)
{
	uint32_t start[RTSP_MAX_NALS], nlen[RTSP_MAX_NALS];
	int n = split_nals(au, len, start, nlen, RTSP_MAX_NALS), i, last = -1, fill = 0, burst = 0;

	for (i = 0; i < n; i++) {
		uint8_t t = au[start[i]] & 0x1f;
		if (t == 6 || t == 9 || t == 0 || t > 23)   /* SEI (the SDK's private one), AUD, reserved: not sent */
			nlen[i] = 0;
		else
			last = i;
	}
	if (last < 0)
		return 0;
	for (i = 0; i <= last; i++) {
		const uint8_t *nal = au + start[i];
		uint32_t nl = nlen[i], pos;
		int marker_nal = (i == last);
		uint8_t *p;

		if (nl == 0)
			continue;
		if (nl <= RTSP_RTP_PAYLOAD) {   /* single NAL unit packet */
			if (!(p = rtp_slot(c, &fill)))
				return -1;
			rtp_header(p, c, marker_nal, ts);
			os_memcpy(p + 12, nal, nl);
			if (rtp_commit(c, &fill, nl, &burst) < 0)
				return -1;
			continue;
		}
		/* FU-A fragments */
		for (pos = 1; pos < nl;) {
			uint32_t chunk = nl - pos;
			int end;
			if (chunk > (uint32_t)RTSP_RTP_PAYLOAD - 2)
				chunk = RTSP_RTP_PAYLOAD - 2;
			end = (pos + chunk == nl);
			if (!(p = rtp_slot(c, &fill)))
				return -1;
			rtp_header(p, c, marker_nal && end, ts);
			p[12] = (nal[0] & 0xe0) | 28;
			p[13] = (nal[0] & 0x1f) | (pos == 1 ? 0x80 : 0) | (end ? 0x40 : 0);
			os_memcpy(p + 14, nal + pos, chunk);
			if (rtp_commit(c, &fill, 2 + chunk, &burst) < 0)
				return -1;
			pos += chunk;
		}
	}
	if (c->tcp)
		return tcp_flush(c, &fill);
	return 0;
}

/* 1 = a frame was sent, 0 = nothing new, -1 = send error */
static int send_next_frame(rtsp_client_t *c)
{
	rtsp_desc_t d = {0};
	int r;

	rtos_lock_mutex(&s_lock);
	for (;;) {
		rtsp_desc_t *p;
		if (c->next_seq == 0 || c->next_seq > s_seq) {
			rtos_unlock_mutex(&s_lock);
			return 0;
		}
		p = &s_desc[c->next_seq % RTSP_NDESC];
		if (!p->valid || p->seq != c->next_seq) {   /* overwritten before we got to it: resync at the newest frame */
			c->lost++;
			c->next_seq = s_seq;
			c->need_key = 1;
			s_idr_req = 1;
			continue;
		}
		if (c->need_key && !p->key) {
			c->next_seq++;
			continue;
		}
		c->need_key = 0;
		d = *p;
		c->busy_seq = d.seq;
		break;
	}
	rtos_unlock_mutex(&s_lock);

	r = send_access_unit(c, s_ring + d.off, d.len, d.ts90);

	rtos_lock_mutex(&s_lock);
	c->busy_seq = 0;
	c->next_seq = d.seq + 1;
	rtos_unlock_mutex(&s_lock);
	if (r < 0)
		return -1;
	c->sent_frames++;
	return 1;
}

/* ---- RTSP request handling ---- */

/* value of header `name` (case-insensitive), skipping blanks; NULL if absent */
static const char *hdr_find(const char *req, const char *name)
{
	size_t nl = os_strlen(name);
	const char *p = req;

	while (p && *p) {
		const char *line = p;
		const char *eol = strstr(line, "\r\n");
		if (!eol)
			break;
		if ((size_t)(eol - line) > nl && os_strncasecmp(line, name, nl) == 0 && line[nl] == ':') {
			p = line + nl + 1;
			while (*p == ' ' || *p == '\t')
				p++;
			return p;
		}
		p = eol + 2;
	}
	return NULL;
}

static int hdr_copy(const char *req, const char *name, char *out, size_t cap)
{
	const char *v = hdr_find(req, name);
	size_t n = 0;

	out[0] = 0;
	if (!v)
		return 0;
	while (v[n] && v[n] != '\r' && n < cap - 1)
		n++;
	os_memcpy(out, v, n);
	out[n] = 0;
	return (int)n;
}

/* "key=a-b" or "key=a" inside a Transport string */
static int tp_param(const char *tp, const char *key, int *a, int *b)
{
	const char *p = strstr(tp, key);

	if (!p)
		return 0;
	p += os_strlen(key);
	*a = (int)os_strtoul(p, (char **)&p, 10);
	*b = *p == '-' ? (int)os_strtoul(p + 1, NULL, 10) : *a + 1;
	return 1;
}

static int reply(rtsp_client_t *c, int code, const char *text, int cseq, const char *extra, const char *ctype, const char *body)
{
	char *o = (char *)c->tx;
	int blen = body ? (int)os_strlen(body) : 0, n;

	n = snprintf(o, RTSP_TX_BUF, "RTSP/1.0 %d %s\r\nCSeq: %d\r\nServer: doorbell-bk7258\r\n%s", code, text, cseq, extra ? extra : "");
	if (c->session && n < RTSP_TX_BUF)
		n += snprintf(o + n, RTSP_TX_BUF - n, "Session: %08x;timeout=%d\r\n", (unsigned)c->session, RTSP_SESSION_TIMEOUT_S);
	if (blen && n < RTSP_TX_BUF)
		n += snprintf(o + n, RTSP_TX_BUF - n, "Content-Type: %s\r\nContent-Length: %d\r\n", ctype, blen);
	if (n < RTSP_TX_BUF)
		n += snprintf(o + n, RTSP_TX_BUF - n, "\r\n");
	if (blen && n + blen < RTSP_TX_BUF) {
		os_memcpy(o + n, body, blen);
		n += blen;
	}
	if (n >= RTSP_TX_BUF)
		return -1;
	return send_all(c->sock, o, n);
}

static int local_url(rtsp_client_t *c, char *out, size_t cap)
{
	struct sockaddr_in me;
	socklen_t ml = sizeof(me);
	char ip[16] = "0.0.0.0";

	if (getsockname(c->sock, (struct sockaddr *)&me, &ml) == 0)
		ip_str(me.sin_addr.s_addr, ip, sizeof(ip));
	return snprintf(out, cap, "rtsp://%s:%u" RTSP_PATH, ip, s_port);
}

static int do_describe(rtsp_client_t *c, int cseq)
{
	char sdp[640], extra[192], url[64], sps64[96] = "", pps64[96] = "";
	uint8_t sps[64], pps[64];
	int sl, pl, w, n;

	if (rtsp_cam_acquire() != 0)
		return reply(c, 503, "Service Unavailable", cseq, NULL, NULL, NULL);
	for (w = 0; w < 60 && (s_sps_len == 0 || s_pps_len == 0); w++)   /* first IDR carries SPS+PPS */
		rtos_delay_milliseconds(50);
	rtos_lock_mutex(&s_lock);
	sl = s_sps_len;
	pl = s_pps_len;
	os_memcpy(sps, s_sps, sizeof(sps));
	os_memcpy(pps, s_pps, sizeof(pps));
	rtos_unlock_mutex(&s_lock);
	if (sl)
		b64(sps, sl, sps64, sizeof(sps64));
	if (pl)
		b64(pps, pl, pps64, sizeof(pps64));
	local_url(c, url, sizeof(url));
	n = snprintf(sdp, sizeof(sdp),
		     "v=0\r\no=- %u 1 IN IP4 0.0.0.0\r\ns=doorbell-bk7258\r\nt=0 0\r\na=control:*\r\na=range:npt=now-\r\n"
		     "m=video 0 RTP/AVP %d\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:%d H264/90000\r\n"
		     "a=fmtp:%d packetization-mode=1",
		     (unsigned)(now_ms() | 1), RTSP_PT, RTSP_PT, RTSP_PT);
	if (sl >= 4 && pl && n < (int)sizeof(sdp))
		n += snprintf(sdp + n, sizeof(sdp) - n, ";profile-level-id=%02X%02X%02X;sprop-parameter-sets=%s,%s",
			      sps[1], sps[2], sps[3], sps64, pps64);
	if (n < (int)sizeof(sdp))
		n += snprintf(sdp + n, sizeof(sdp) - n, "\r\na=framerate:15\r\na=control:track0\r\n");
	if (n >= (int)sizeof(sdp))
		return reply(c, 500, "Internal Server Error", cseq, NULL, NULL, NULL);
	snprintf(extra, sizeof(extra), "Content-Base: %s/\r\n", url);
	return reply(c, 200, "OK", cseq, extra, "application/sdp", sdp);
}

static int do_setup(rtsp_client_t *c, int cseq, const char *req)
{
	char tp[160], extra[160];
	int a = 0, b = 0;

	hdr_copy(req, "Transport", tp, sizeof(tp));
	if (c->setup)   /* only one track: a second SETUP is a client bug */
		return reply(c, 459, "Aggregate Operation Not Allowed", cseq, NULL, NULL, NULL);
	if (strstr(tp, "multicast"))
		return reply(c, 461, "Unsupported Transport", cseq, NULL, NULL, NULL);
	if (!c->session)
		c->session = mix32(now_ms() ^ (c->idx << 24) ^ 0x5bd1e995U) | 1;
	c->ssrc = mix32(c->session ^ 0x9e3779b9U) | 1;
	if (strstr(tp, "RTP/AVP/TCP")) {
		c->tcp = 1;
		c->chan = tp_param(tp, "interleaved=", &a, &b) ? (uint8_t)a : 0;
		snprintf(extra, sizeof(extra), "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u;ssrc=%08X\r\n", c->chan, c->chan + 1, (unsigned)c->ssrc);
	} else {
		struct sockaddr_in me = {0}, peer;
		socklen_t pl = sizeof(peer);
		int one = 1, sport = RTSP_UDP_BASE_PORT + 2 * c->idx;

		if (!tp_param(tp, "client_port=", &a, &b) || a <= 0 || a > 65535)
			return reply(c, 461, "Unsupported Transport", cseq, NULL, NULL, NULL);
		if (getpeername(c->sock, (struct sockaddr *)&peer, &pl) != 0)
			return reply(c, 500, "Internal Server Error", cseq, NULL, NULL, NULL);
		c->udp = socket(AF_INET, SOCK_DGRAM, 0);
		if (c->udp < 0)
			return reply(c, 500, "Internal Server Error", cseq, NULL, NULL, NULL);
		setsockopt(c->udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		me.sin_family = AF_INET;
		me.sin_port = htons(sport);
		me.sin_addr.s_addr = INADDR_ANY;
		if (bind(c->udp, (struct sockaddr *)&me, sizeof(me)) < 0) {
			close(c->udp);
			c->udp = -1;
			return reply(c, 500, "Internal Server Error", cseq, NULL, NULL, NULL);
		}
		c->peer_rtp.sin_family = AF_INET;
		c->peer_rtp.sin_port = htons((uint16_t)a);
		c->peer_rtp.sin_addr = peer.sin_addr;
		c->tcp = 0;
		snprintf(extra, sizeof(extra), "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d;ssrc=%08X\r\n",
			 a, b, sport, sport + 1, (unsigned)c->ssrc);
	}
	c->setup = 1;
	return reply(c, 200, "OK", cseq, extra, NULL, NULL);
}

static int do_play(rtsp_client_t *c, int cseq)
{
	char extra[160], url[64];
	uint32_t ts = 0;

	if (!c->setup)
		return reply(c, 455, "Method Not Valid in This State", cseq, NULL, NULL, NULL);
	rtos_lock_mutex(&s_lock);
	c->next_seq = s_seq ? s_seq : 1;   /* start at the newest frame, wait for a keyframe */
	c->need_key = 1;
	c->busy_seq = 0;
	if (s_seq && s_desc[s_seq % RTSP_NDESC].valid)
		ts = s_desc[s_seq % RTSP_NDESC].ts90;
	c->playing = 1;
	rtos_unlock_mutex(&s_lock);
	s_idr_req = 1;
	local_url(c, url, sizeof(url));
	snprintf(extra, sizeof(extra), "Range: npt=now-\r\nRTP-Info: url=%s/track0;seq=%u;rtptime=%u\r\n", url, c->rtp_seq, (unsigned)ts);
	return reply(c, 200, "OK", cseq, extra, NULL, NULL);
}

/* Handle one complete request at c->rx (NUL-terminated header block). <0 = close the client. */
static int handle_request(rtsp_client_t *c, char *req)
{
	char method[16], sess[24];
	const char *p = req, *v;
	int cseq = 0, ml = 0, r;

	while (*p && *p != ' ' && ml < (int)sizeof(method) - 1)
		method[ml++] = *p++;
	method[ml] = 0;
	if ((v = hdr_find(req, "CSeq")))
		cseq = (int)os_strtoul(v, NULL, 10);
	hdr_copy(req, "Session", sess, sizeof(sess));
	if (sess[0] && c->session && os_strtoul(sess, NULL, 16) != c->session)
		return reply(c, 454, "Session Not Found", cseq, NULL, NULL, NULL);

	if (os_strcmp(method, "OPTIONS") == 0)
		r = reply(c, 200, "OK", cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n", NULL, NULL);
	else if (os_strcmp(method, "DESCRIBE") == 0)
		r = do_describe(c, cseq);
	else if (os_strcmp(method, "SETUP") == 0)
		r = do_setup(c, cseq, req);
	else if (os_strcmp(method, "PLAY") == 0)
		r = do_play(c, cseq);
	else if (os_strcmp(method, "PAUSE") == 0) {
		c->playing = 0;
		r = reply(c, 200, "OK", cseq, NULL, NULL, NULL);
	} else if (os_strcmp(method, "TEARDOWN") == 0) {
		c->playing = 0;
		reply(c, 200, "OK", cseq, NULL, NULL, NULL);
		return -1;
	} else if (os_strcmp(method, "GET_PARAMETER") == 0 || os_strcmp(method, "SET_PARAMETER") == 0)
		r = reply(c, 200, "OK", cseq, NULL, NULL, NULL);
	else
		r = reply(c, 501, "Not Implemented", cseq, NULL, NULL, NULL);
	return r;
}

/* Consume whatever complete messages sit in c->rx: RTSP requests or '$' interleaved RTCP. */
static int process_rx(rtsp_client_t *c)
{
	for (;;) {
		char *end;
		int hl, cl = 0, total;
		const char *v;

		if (c->rxlen <= 0)
			return 0;
		c->rx[c->rxlen] = 0;
		if (c->rx[0] == '$') {
			if (c->rxlen < 4)
				return 0;
			total = 4 + (((uint8_t)c->rx[2] << 8) | (uint8_t)c->rx[3]);
			if (total > (int)sizeof(c->rx) - 1) {   /* bigger than we buffer: drop everything we have */
				c->rxlen = 0;
				return 0;
			}
			if (c->rxlen < total)
				return 0;
			os_memmove(c->rx, c->rx + total, c->rxlen - total);
			c->rxlen -= total;
			continue;
		}
		end = strstr(c->rx, "\r\n\r\n");
		if (!end) {
			if (c->rxlen >= (int)sizeof(c->rx) - 1)
				c->rxlen = 0;   /* garbage, not RTSP */
			return 0;
		}
		hl = end + 4 - c->rx;
		if ((v = hdr_find(c->rx, "Content-Length")))
			cl = (int)os_strtoul(v, NULL, 10);
		if (cl < 0 || hl + cl > (int)sizeof(c->rx) - 1) {
			c->rxlen = 0;
			return -1;
		}
		total = hl + cl;
		if (c->rxlen < total)
			return 0;
		end[2] = 0;   /* terminate the header block (keep one CRLF so hdr_find sees the last line) */
		if (handle_request(c, c->rx) < 0)
			return -1;
		os_memmove(c->rx, c->rx + total, c->rxlen - total);
		c->rxlen -= total;
	}
}

/* ---- client thread ---- */

static void client_task(beken_thread_arg_t arg)
{
	rtsp_client_t *c = (rtsp_client_t *)arg;
	struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
	int one = 1;

	setsockopt(c->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(c->sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	c->last_rx_ms = c->start_ms = now_ms();
	os_printf("[rtsp] #%u %s connected\r\n", c->idx, c->peer);

	while (!c->stop && s_run) {
		fd_set rf;
		struct timeval st = {.tv_sec = 0, .tv_usec = c->playing ? 0 : 50000};
		int r;

		FD_ZERO(&rf);
		FD_SET(c->sock, &rf);
		r = select(c->sock + 1, &rf, NULL, NULL, &st);
		if (r > 0) {
			int n = recv(c->sock, c->rx + c->rxlen, sizeof(c->rx) - 1 - c->rxlen, 0);
			if (n <= 0)
				break;
			c->rxlen += n;
			c->last_rx_ms = now_ms();
			if (process_rx(c) < 0)
				break;
		} else if (r < 0) {
			break;
		}
		if (c->playing) {
			r = send_next_frame(c);
			if (r < 0) {
				os_printf("[rtsp] #%u send failed\r\n", c->idx);
				break;
			}
			if (r == 0)
				rtos_get_semaphore(&c->sem, 20);
		}
		if (!c->tcp && c->setup && now_ms() - c->last_rx_ms > (RTSP_SESSION_TIMEOUT_S + 15) * 1000U)
			break;   /* UDP client went away without TEARDOWN */
	}

	os_printf("[rtsp] #%u %s closed: %u frames %u KB lost %u udp_err %u\r\n", c->idx, c->peer,
		  (unsigned)c->sent_frames, (unsigned)(c->sent_bytes / 1024), (unsigned)c->lost, (unsigned)c->udp_err);
	close(c->sock);
	if (c->udp >= 0)
		close(c->udp);
	rtos_lock_mutex(&s_lock);
	c->playing = 0;
	c->busy_seq = 0;
	c->used = 0;
	s_nclients--;
	rtos_unlock_mutex(&s_lock);
	rtos_deinit_semaphore(&c->sem);
	if (c->tx)
		os_free(c->tx);
	c->tx = NULL;
	rtos_delete_thread(NULL);
}

static void accept_client(int ls)
{
	struct sockaddr_in peer;
	socklen_t pl = sizeof(peer);
	int cs = accept(ls, (struct sockaddr *)&peer, &pl), i;
	rtsp_client_t *c = NULL;
	beken_thread_t t;

	if (cs < 0)
		return;
	rtos_lock_mutex(&s_lock);
	for (i = 0; i < RTSP_MAX_CLIENTS; i++)
		if (!s_cl[i].used) {
			c = &s_cl[i];
			os_memset(c, 0, sizeof(*c));
			c->used = 1;
			c->idx = i;
			c->sock = cs;
			c->udp = -1;
			s_nclients++;
			break;
		}
	rtos_unlock_mutex(&s_lock);
	if (!c) {
		static const char busy[] = "RTSP/1.0 503 Service Unavailable\r\nCSeq: 0\r\n\r\n";
		send(cs, busy, sizeof(busy) - 1, 0);
		close(cs);
		return;
	}
	ip_str(peer.sin_addr.s_addr, c->peer, sizeof(c->peer));
	c->tx = psram_malloc(RTSP_TX_BUF);
	if (!c->tx)
		c->tx = os_malloc(RTSP_TX_BUF);
	if (!c->tx || rtos_init_semaphore(&c->sem, 1) != BK_OK ||
	    rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "rtspc", client_task, 5120, c) != BK_OK) {
		os_printf("[rtsp] client setup failed (memory)\r\n");
		if (c->sem)
			rtos_deinit_semaphore(&c->sem);
		if (c->tx)
			os_free(c->tx);
		close(cs);
		rtos_lock_mutex(&s_lock);
		c->tx = NULL;
		c->used = 0;
		s_nclients--;
		rtos_unlock_mutex(&s_lock);
	}
}

static void housekeeping(void)
{
	uint32_t now = now_ms();

	if (now - s_win_ms >= 1000) {
		uint32_t f = s_in_frames, b = s_in_bytes, dt = now - s_win_ms;
		s_fps10 = (f - s_win_frames) * 10000U / dt;
		s_kbps = (uint32_t)(((uint64_t)(b - s_win_bytes) * 8000U) / dt / 1000U);
		s_win_frames = f;
		s_win_bytes = b;
		s_win_ms = now;
	}
	if (s_idr_req && now - s_idr_last_ms > 500) {
		s_idr_req = 0;
		s_idr_last_ms = now;
		rtsp_cam_idr();
	}
}

static void rtsp_task(beken_thread_arg_t arg)
{
	struct sockaddr_in sa = {0};
	int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1, i, w;

	if (ls < 0)
		goto out;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(s_port);
	sa.sin_addr.s_addr = INADDR_ANY;
	if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(ls, 2) < 0) {
		os_printf("[rtsp] bind/listen on %u failed\r\n", s_port);
		close(ls);
		goto out;
	}
	os_printf("[rtsp] server on port %u (" RTSP_PATH ")\r\n", s_port);
	s_running = true;
	while (s_run) {
		fd_set rf;
		struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};

		FD_ZERO(&rf);
		FD_SET(ls, &rf);
		if (select(ls + 1, &rf, NULL, NULL, &tv) > 0)
			accept_client(ls);
		housekeeping();
	}
	close(ls);
	s_running = false;   /* producer stops feeding; clients drain */
	for (i = 0; i < RTSP_MAX_CLIENTS; i++)
		s_cl[i].stop = 1;
	for (w = 0; w < 60 && s_nclients > 0; w++)
		rtos_delay_milliseconds(50);
	rtsp_cam_release();
	os_printf("[rtsp] stopped\r\n");
out:
	s_running = false;
	s_run = false;
	rtos_delete_thread(NULL);
}

/* ---- public control ---- */

int rtsp_start(uint16_t port)
{
	beken_thread_t t;
	int ret;

	if (s_run)
		return BK_OK;
	if (s_running) {   /* a previous `rtsp off` is still winding down: try again in a second */
		os_printf("[rtsp] still stopping\r\n");
		return BK_ERR_BUSY;
	}
	if (!s_lock && rtos_init_mutex(&s_lock) != BK_OK)
		return BK_ERR_NO_MEM;
	if (!s_ring)
		s_ring = psram_malloc(RTSP_RING_SIZE);
	if (!s_ring) {
		os_printf("[rtsp] no PSRAM for the frame ring\r\n");
		return BK_ERR_NO_MEM;
	}
	rtos_lock_mutex(&s_lock);
	os_memset(s_desc, 0, sizeof(s_desc));
	s_head = 0;
	s_seq = 0;
	s_sps_len = s_pps_len = 0;
	s_in_frames = s_in_bytes = s_in_key = s_drop_busy = s_drop_big = 0;
	s_win_frames = s_win_bytes = s_fps10 = s_kbps = 0;
	s_win_ms = now_ms();
	rtos_unlock_mutex(&s_lock);
	s_port = port ? port : RTSP_DEFAULT_PORT;
	s_run = true;
	ret = rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "rtsp", rtsp_task, 3072, NULL);
	if (ret != BK_OK)
		s_run = false;
	return ret;
}

void rtsp_stop(void)
{
	s_run = false;
}

bool rtsp_running(void)
{
	return s_running;
}

int rtsp_clients(int *playing)
{
	int i, n = 0, p = 0;

	for (i = 0; i < RTSP_MAX_CLIENTS; i++)
		if (s_cl[i].used) {
			n++;
			if (s_cl[i].playing)
				p++;
		}
	if (playing)
		*playing = p;
	return n;
}

int rtsp_status(char *buf, size_t cap)
{
	int p, n = rtsp_clients(&p);

	return snprintf(buf, cap, "[rtsp] %s port %u clients %d/%d (%d playing) in %u.%u fps %u kbps frames %u key %u drop busy %u big %u sps %u pps %u age %u ms\r\n",
			s_running ? "running" : (s_run ? "starting" : "stopped"), s_port, n, RTSP_MAX_CLIENTS, p,
			(unsigned)s_fps10 / 10, (unsigned)s_fps10 % 10, (unsigned)s_kbps, (unsigned)s_in_frames, (unsigned)s_in_key,
			(unsigned)s_drop_busy, (unsigned)s_drop_big, s_sps_len, s_pps_len,
			s_last_frame_ms ? (unsigned)(now_ms() - s_last_frame_ms) : 0);
}

int rtsp_status_clients(char *buf, size_t cap)
{
	int i, n = 0;

	buf[0] = 0;
	for (i = 0; i < RTSP_MAX_CLIENTS && n < (int)cap; i++) {
		rtsp_client_t *c = &s_cl[i];
		if (!c->used)
			continue;
		n += snprintf(buf + n, cap - n, "[rtsp] #%d %s %s %s %u frames %u KB lost %u udp_err %u %u s\r\n", i, c->peer,
			      c->setup ? (c->tcp ? "tcp" : "udp") : "new", c->playing ? "playing" : "idle",
			      (unsigned)c->sent_frames, (unsigned)(c->sent_bytes / 1024), (unsigned)c->lost, (unsigned)c->udp_err,
			      (unsigned)((now_ms() - c->start_ms) / 1000));
	}
	return n;
}

#else /* not the AVDK CPU0 image */
typedef int rtsp_c_not_built_for_this_core;
#endif
