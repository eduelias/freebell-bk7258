/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: minimal RTSP/1.0 + RTP server: H.264 (RFC 6184) video, G.711 microphone track,
 * ONVIF backchannel (two-way audio) for the hardware encoder and the voc audio engine. */
/* rtsp.c - minimal RTSP/1.0 + RTP server. See rtsp.h for the overview.
 *
 * Threads:  "rtsp"  listener (select on the listening socket, 200 ms tick for housekeeping:
 *                   fps/kbps window, rate-limited IDR requests, voc engine start/stop);
 *           "rtspc" one per client: RTSP control on the TCP socket (select, zero timeout
 *                   while playing), the RTP sender for that client (video + microphone) and the
 *                   backchannel receiver (interleaved on the same socket, or its own UDP port).
 * Producers: rtsp_feed_frame() runs on the media transfer task (2 KB stack: keep locals small);
 *            rtsp_voc_mic() runs on the SDK's "aud_tras" task (2 KB) every 40 ms with 320 bytes of
 *            G.711 (audio_voc.c): one memcpy into a small ring + semaphores.
 *
 * Frame ring: one PSRAM byte ring + 32 descriptors. The producer writes each access unit
 * contiguously (wrapping to 0 when it would not fit at the end), invalidating the descriptors
 * it overwrites. A client marks the frame it is sending as "busy"; the producer never overwrites
 * a frame whose sequence is >= the oldest busy one - if that is the only space left it drops the
 * new frame instead and requests an IDR, so a stuck client can only ever hurt itself and the
 * stream resynchronises at the next IDR. A client that finds its next frame gone jumps to the
 * newest frame and waits for a keyframe.
 *
 * Tracks (SDP, in this order): track0 video H264/90000; track1 audio PCMA/8000 a=recvonly (the
 * microphone, server -> client); track2 audio PCMA/8000 a=sendonly (the backchannel, client ->
 * speaker), advertised only when the DESCRIBE carries `Require: www.onvif.org/ver20/backchannel`
 * (ONVIF Streaming Spec 5.3; go2rtc sends it by default). Directions are written from the client's
 * point of view, which is what go2rtc expects: a media without a direction is "we receive", and an
 * `a=sendonly` media is the one it sends into. Each track has its own transport (TCP interleaved
 * channels or a UDP port pair), a client may set up any subset; PLAY/PAUSE are aggregate.
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
#include "audio_voc.h"
#include "applog.h"   /* last: our os_printf lines go to the RAM log ring (policy.c) */

#define RTSP_MAX_CLIENTS 3
#define RTSP_RING_SIZE (128 * 1024)      /* ~1 s of video at 1 Mbit/s; PSRAM heap on CPU0 is only 512 KB */
#define RTSP_FRAME_MAX (96 * 1024)       /* larger access units are dropped (CPU1 buffer is 128 KB) */
#define RTSP_NDESC 32
#define RTSP_MTU 1400                    /* RTP packet size incl. the 12-byte header */
#define RTSP_RTP_PAYLOAD (RTSP_MTU - 12)
#define RTSP_RX_MAX 1536                 /* one RTSP request, or one interleaved backchannel frame (<= 1.5 KB) */
#define RTSP_TX_BUF 8192                 /* per-client staging buffer (TCP: ~5 packets per send) */
#define RTSP_UDP_BASE_PORT 6970          /* server_port for UDP clients: base + 6 * slot + 2 * track */
#define RTSP_SESSION_TIMEOUT_S 60        /* advertised; UDP sessions are dropped after it + 15 s without a request */
#define RTSP_PT 96                       /* H.264 (dynamic) */
#define RTSP_MAX_NALS 24                 /* per access unit (SPS, PPS, slices; the encoder emits a handful) */
#define RTSP_TRACKS 3
#define TRK_VIDEO 0
#define TRK_MIC 1
#define TRK_BACK 2
#define RTSP_ARING 3200                  /* microphone ring: 400 ms of G.711 at 8 kHz; must be a multiple of 160 */
#define RTSP_BC_BUF 1500                 /* UDP backchannel datagram buffer (per client, on demand) */
#define RTSP_VOC_RETRY_MS 500            /* retry voc_start while the raw engine (chime) is busy */
#define ONVIF_BACKCHANNEL "www.onvif.org/ver20/backchannel"

typedef struct {
	uint32_t seq, off, len, ts90;
	uint8_t key, valid;
} rtsp_desc_t;

typedef struct {
	uint8_t setup, tcp, chan;     /* chan = interleaved RTP channel (RTCP = chan + 1, ignored) */
	int udp;                      /* RTP/UDP socket (sender for video/mic, receiver for the backchannel) or -1 */
	struct sockaddr_in peer;      /* UDP destination */
	uint16_t seq;                 /* next RTP sequence number */
	uint32_t ssrc, ts;            /* ts: next RTP timestamp (audio: 8 kHz samples) */
	uint32_t pkts, bytes;         /* sent (video, mic) or received (backchannel) */
} rtsp_track_t;

typedef struct {
	int sock;                     /* RTSP control socket; also carries interleaved RTP */
	rtsp_track_t trk[RTSP_TRACKS];
	uint8_t used, stop, playing, need_key, idx, audio_ref;
	uint32_t session;
	uint32_t next_seq;            /* ring sequence we want next (0 = not started) */
	uint32_t busy_seq;            /* ring sequence being sent right now (0 = none) */
	uint32_t aud_r;               /* microphone ring read index (bytes, monotonic) */
	uint32_t last_rx_ms, start_ms;
	uint32_t sent_frames, sent_bytes, lost, udp_err, aud_lost, bc_bad;
	beken_semaphore_t sem;        /* "a new frame / audio is in the ring" (count 1) */
	uint8_t *tx;                  /* RTSP_TX_BUF */
	uint8_t *bc;                  /* RTSP_BC_BUF, UDP backchannel only */
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
/* audio: microphone ring (G.711 from audio_voc.c) and the voc engine lifecycle */
static uint8_t s_aring[RTSP_ARING];
static volatile uint32_t s_aw;        /* monotonic write index, multiple of 160 */
static volatile uint32_t s_aud_in, s_aud_in_bytes, s_bc_pkts, s_bc_bytes;
static volatile int s_aud_refs;       /* clients with an audio track set up */
static bool s_voc_ours;               /* we started the voc engine (vs `talk voc on`) */
static uint32_t s_voc_try_ms, s_voc_fail;
static int s_voc_last;

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

static void ring_copy_out(uint8_t *dst, const uint8_t *ring, uint32_t size, uint32_t idx, uint32_t n)
{
	uint32_t first = size - idx < n ? size - idx : n;
	os_memcpy(dst, ring + idx, first);
	if (n > first)
		os_memcpy(dst + first, ring, n - first);
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
		if (s_cl[i].used && s_cl[i].playing && s_cl[i].trk[TRK_VIDEO].setup)
			rtos_set_semaphore(&s_cl[i].sem);   /* fails harmlessly when already signalled */
	rtos_unlock_mutex(&s_lock);
}

void rtsp_request_idr(void)
{
	s_idr_req = 1;
}

/* ---- producer: microphone frames from the voc engine (SDK "aud_tras" task, 2 KB stack) ---- */

static void rtsp_voc_mic(const uint8_t *g711, uint32_t len)
{
	uint32_t n = len - len % VOC_FRAME_BYTES, idx, first, i;

	if (!n)
		return;
	if (n > RTSP_ARING)
		n = RTSP_ARING;
	idx = s_aw % RTSP_ARING;
	first = RTSP_ARING - idx < n ? RTSP_ARING - idx : n;
	os_memcpy(s_aring + idx, g711, first);
	if (n > first)
		os_memcpy(s_aring, g711 + first, n - first);
	s_aw += n;
	s_aud_in++;
	s_aud_in_bytes += n;
	for (i = 0; i < RTSP_MAX_CLIENTS; i++)   /* no mutex here: a stale read only costs one 20 ms wait */
		if (s_cl[i].used && s_cl[i].playing && s_cl[i].trk[TRK_MIC].setup)
			rtos_set_semaphore(&s_cl[i].sem);
}

/* ---- RTP packetization (RFC 6184, packetization-mode=1; RFC 3551 PCMA) ---- */

static void rtp_header(uint8_t *p, rtsp_track_t *t, uint8_t pt, int marker, uint32_t ts)
{
	p[0] = 0x80;
	p[1] = pt | (marker ? 0x80 : 0);
	p[2] = t->seq >> 8;
	p[3] = t->seq & 0xff;
	p[4] = ts >> 24;
	p[5] = ts >> 16;
	p[6] = ts >> 8;
	p[7] = ts;
	p[8] = t->ssrc >> 24;
	p[9] = t->ssrc >> 16;
	p[10] = t->ssrc >> 8;
	p[11] = t->ssrc;
	t->seq++;
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
static uint8_t *rtp_slot(rtsp_client_t *c, rtsp_track_t *t, int *fill)
{
	if (t->tcp) {
		if (*fill + 4 + RTSP_MTU > RTSP_TX_BUF && tcp_flush(c, fill) < 0)
			return NULL;
		return c->tx + *fill + 4;
	}
	return c->tx;
}

static int rtp_commit(rtsp_client_t *c, rtsp_track_t *t, int *fill, int plen, int *udp_burst)
{
	int n = 12 + plen;

	t->pkts++;
	t->bytes += n;
	if (t->tcp) {
		uint8_t *h = c->tx + *fill;
		h[0] = '$';
		h[1] = t->chan;
		h[2] = n >> 8;
		h[3] = n & 0xff;
		*fill += 4 + n;
		return 0;
	}
	for (int tries = 0; tries < 6; tries++) {
		int r = sendto(t->udp, c->tx, n, 0, (struct sockaddr *)&t->peer, sizeof(t->peer));
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
	rtsp_track_t *t = &c->trk[TRK_VIDEO];
	int n = split_nals(au, len, start, nlen, RTSP_MAX_NALS), i, last = -1, fill = 0, burst = 0;

	for (i = 0; i < n; i++) {
		uint8_t ty = au[start[i]] & 0x1f;
		if (ty == 6 || ty == 9 || ty == 0 || ty > 23)   /* SEI (the SDK's private one), AUD, reserved: not sent */
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
			if (!(p = rtp_slot(c, t, &fill)))
				return -1;
			rtp_header(p, t, RTSP_PT, marker_nal, ts);
			os_memcpy(p + 12, nal, nl);
			if (rtp_commit(c, t, &fill, nl, &burst) < 0)
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
			if (!(p = rtp_slot(c, t, &fill)))
				return -1;
			rtp_header(p, t, RTSP_PT, marker_nal && end, ts);
			p[12] = (nal[0] & 0xe0) | 28;
			p[13] = (nal[0] & 0x1f) | (pos == 1 ? 0x80 : 0) | (end ? 0x40 : 0);
			os_memcpy(p + 14, nal + pos, chunk);
			if (rtp_commit(c, t, &fill, 2 + chunk, &burst) < 0)
				return -1;
			pos += chunk;
		}
	}
	if (t->tcp)
		return tcp_flush(c, &fill);
	return 0;
}

/* 1 = a frame was sent, 0 = nothing new, -1 = send error */
static int send_next_frame(rtsp_client_t *c)
{
	rtsp_desc_t d = {0};
	int r;

	if (!c->trk[TRK_VIDEO].setup)
		return 0;
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

/* Microphone: every 160 bytes in the ring past the client's read index become one 20 ms PCMA
 * packet. 1 = something sent, 0 = nothing pending, -1 = send error. */
static int send_audio(rtsp_client_t *c)
{
	rtsp_track_t *t = &c->trk[TRK_MIC];
	uint32_t avail = s_aw - c->aud_r;
	int fill = 0, burst = 0, sent = 0;

	if (!t->setup || avail < VOC_FRAME_BYTES)
		return 0;
	if (avail > RTSP_ARING) {   /* we fell behind by more than the ring: skip to the newest 200 ms */
		c->aud_lost += (avail - RTSP_ARING + 1600) / VOC_FRAME_BYTES;
		c->aud_r = s_aw - 1600;
		avail = 1600;
	}
	while (avail >= VOC_FRAME_BYTES) {
		uint8_t *p = rtp_slot(c, t, &fill);
		if (!p)
			return -1;
		rtp_header(p, t, VOC_PT, sent == 0 && t->pkts == 0, t->ts);
		ring_copy_out(p + 12, s_aring, RTSP_ARING, c->aud_r % RTSP_ARING, VOC_FRAME_BYTES);
		if (rtp_commit(c, t, &fill, VOC_FRAME_BYTES, &burst) < 0)
			return -1;
		c->aud_r += VOC_FRAME_BYTES;
		t->ts += VOC_FRAME_BYTES;   /* 8 kHz clock, one byte per sample */
		avail -= VOC_FRAME_BYTES;
		sent++;
	}
	if (t->tcp && tcp_flush(c, &fill) < 0)
		return -1;
	return 1;
}

/* Backchannel: one RTP packet from the client (interleaved or UDP) -> the voc speaker. */
static void rtp_in(rtsp_client_t *c, const uint8_t *p, int n)
{
	rtsp_track_t *t = &c->trk[TRK_BACK];
	int off = 12, cc, pt;

	if (n < 12 || (p[0] >> 6) != 2) {
		c->bc_bad++;
		return;
	}
	cc = p[0] & 0x0f;
	off += 4 * cc;
	if (p[0] & 0x10) {   /* header extension */
		if (n < off + 4)
			goto bad;
		off += 4 + 4 * ((p[off + 2] << 8) | p[off + 3]);
	}
	if (p[0] & 0x20 && n > off)   /* padding */
		n -= p[n - 1];
	if (n <= off)
		goto bad;
	pt = p[1] & 0x7f;
	if (pt != VOC_PT) {   /* we only offered PCMA; anything else is not for the speaker */
		c->bc_bad++;
		return;
	}
	t->pkts++;
	t->bytes += n;
	s_bc_pkts++;
	s_bc_bytes += n - off;
	voc_feed_spk(p + off, n - off);
	return;
bad:
	c->bc_bad++;
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

static int do_describe(rtsp_client_t *c, int cseq, const char *req)
{
	char extra[192], url[64], sps64[96] = "", pps64[96] = "", *sdp;
	uint8_t sps[64], pps[64];
	const char *rq = hdr_find(req, "Require");
	bool backchannel = rq && strstr(rq, ONVIF_BACKCHANNEL) != NULL;
	int sl, pl, w, n, r;
	const int cap = 1024;

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
	sdp = os_malloc(cap);
	if (!sdp)
		return reply(c, 500, "Internal Server Error", cseq, NULL, NULL, NULL);
	n = snprintf(sdp, cap,
		     "v=0\r\no=- %u 1 IN IP4 0.0.0.0\r\ns=doorbell-bk7258\r\nt=0 0\r\na=control:*\r\na=range:npt=now-\r\n"
		     "m=video 0 RTP/AVP %d\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:%d H264/90000\r\n"
		     "a=fmtp:%d packetization-mode=1",
		     (unsigned)(now_ms() | 1), RTSP_PT, RTSP_PT, RTSP_PT);
	if (sl >= 4 && pl && n < cap)
		n += snprintf(sdp + n, cap - n, ";profile-level-id=%02X%02X%02X;sprop-parameter-sets=%s,%s",
			      sps[1], sps[2], sps[3], sps64, pps64);
	if (n < cap)
		n += snprintf(sdp + n, cap - n, "\r\na=framerate:15\r\na=control:track0\r\n"
			      "m=audio 0 RTP/AVP %d\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:%d PCMA/8000\r\na=ptime:20\r\na=recvonly\r\na=control:track1\r\n",
			      VOC_PT, VOC_PT);
	if (backchannel && n < cap)
		n += snprintf(sdp + n, cap - n, "m=audio 0 RTP/AVP %d\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:%d PCMA/8000\r\na=ptime:20\r\na=sendonly\r\na=control:track2\r\n",
			      VOC_PT, VOC_PT);
	if (n >= cap) {
		os_free(sdp);
		return reply(c, 500, "Internal Server Error", cseq, NULL, NULL, NULL);
	}
	snprintf(extra, sizeof(extra), "Content-Base: %s/\r\n", url);
	r = reply(c, 200, "OK", cseq, extra, "application/sdp", sdp);
	os_free(sdp);
	os_printf("[rtsp] #%u describe%s\r\n", c->idx, backchannel ? " (onvif backchannel)" : "");
	return r;
}

/* track index from the request line "SETUP rtsp://host/live/trackN RTSP/1.0"; 0 when absent */
static int req_track(const char *req)
{
	const char *eol = strstr(req, "\r\n"), *p = strstr(req, "/track");

	if (!p || (eol && p > eol) || p[6] < '0' || p[6] > '9')
		return 0;
	return p[6] - '0';
}

static void track_close_udp(rtsp_track_t *t)
{
	if (t->udp >= 0)
		close(t->udp);
	t->udp = -1;
}

static int do_setup(rtsp_client_t *c, int cseq, const char *req)
{
	char tp[160], extra[160];
	int a = 0, b = 0, ti = req_track(req);
	rtsp_track_t *t;

	if (ti >= RTSP_TRACKS)
		return reply(c, 404, "Not Found", cseq, NULL, NULL, NULL);
	t = &c->trk[ti];
	hdr_copy(req, "Transport", tp, sizeof(tp));
	if (strstr(tp, "multicast"))
		return reply(c, 461, "Unsupported Transport", cseq, NULL, NULL, NULL);
	if (!c->session)
		c->session = mix32(now_ms() ^ (c->idx << 24) ^ 0x5bd1e995U) | 1;
	track_close_udp(t);
	t->ssrc = mix32(c->session ^ 0x9e3779b9U ^ ((uint32_t)ti << 8)) | 1;
	t->ts = ti == TRK_VIDEO ? 0 : mix32(t->ssrc);
	if (strstr(tp, "RTP/AVP/TCP")) {
		t->tcp = 1;
		t->chan = tp_param(tp, "interleaved=", &a, &b) ? (uint8_t)a : (uint8_t)(2 * ti);
		snprintf(extra, sizeof(extra), "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u;ssrc=%08X\r\n", t->chan, t->chan + 1, (unsigned)t->ssrc);
	} else {
		struct sockaddr_in me = {0}, peer;
		socklen_t pl = sizeof(peer);
		int one = 1, sport = RTSP_UDP_BASE_PORT + 6 * c->idx + 2 * ti;

		if (!tp_param(tp, "client_port=", &a, &b) || a <= 0 || a > 65535)
			return reply(c, 461, "Unsupported Transport", cseq, NULL, NULL, NULL);
		if (getpeername(c->sock, (struct sockaddr *)&peer, &pl) != 0)
			return reply(c, 500, "Internal Server Error", cseq, NULL, NULL, NULL);
		if (ti == TRK_BACK && !c->bc) {
			c->bc = os_malloc(RTSP_BC_BUF);
			if (!c->bc)
				return reply(c, 500, "Internal Server Error", cseq, NULL, NULL, NULL);
		}
		t->udp = socket(AF_INET, SOCK_DGRAM, 0);
		if (t->udp < 0)
			return reply(c, 500, "Internal Server Error", cseq, NULL, NULL, NULL);
		setsockopt(t->udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		me.sin_family = AF_INET;
		me.sin_port = htons(sport);
		me.sin_addr.s_addr = INADDR_ANY;
		if (bind(t->udp, (struct sockaddr *)&me, sizeof(me)) < 0) {
			track_close_udp(t);
			return reply(c, 500, "Internal Server Error", cseq, NULL, NULL, NULL);
		}
		t->peer.sin_family = AF_INET;
		t->peer.sin_port = htons((uint16_t)a);
		t->peer.sin_addr = peer.sin_addr;
		t->tcp = 0;
		snprintf(extra, sizeof(extra), "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d;ssrc=%08X\r\n",
			 a, b, sport, sport + 1, (unsigned)t->ssrc);
	}
	t->setup = 1;
	if (ti != TRK_VIDEO && !c->audio_ref) {
		c->audio_ref = 1;
		rtos_lock_mutex(&s_lock);
		s_aud_refs++;
		rtos_unlock_mutex(&s_lock);
		s_voc_try_ms = 0;   /* housekeeping starts the voc engine within its next tick */
	}
	os_printf("[rtsp] #%u setup track%d %s\r\n", c->idx, ti, t->tcp ? "tcp" : "udp");
	return reply(c, 200, "OK", cseq, extra, NULL, NULL);
}

static int do_play(rtsp_client_t *c, int cseq)
{
	char extra[320], url[64];
	uint32_t ts = 0;
	int n, i, any = 0;

	for (i = 0; i < RTSP_TRACKS; i++)
		any |= c->trk[i].setup;
	if (!any)
		return reply(c, 455, "Method Not Valid in This State", cseq, NULL, NULL, NULL);
	rtos_lock_mutex(&s_lock);
	c->next_seq = s_seq ? s_seq : 1;   /* start at the newest frame, wait for a keyframe */
	c->need_key = 1;
	c->busy_seq = 0;
	if (s_seq && s_desc[s_seq % RTSP_NDESC].valid)
		ts = s_desc[s_seq % RTSP_NDESC].ts90;
	c->aud_r = s_aw;                   /* microphone: start with the next frame */
	c->playing = 1;
	rtos_unlock_mutex(&s_lock);
	if (c->trk[TRK_VIDEO].setup)
		s_idr_req = 1;
	local_url(c, url, sizeof(url));
	n = snprintf(extra, sizeof(extra), "Range: npt=now-\r\nRTP-Info: ");
	for (i = 0; i < RTSP_TRACKS && n < (int)sizeof(extra); i++) {
		if (!c->trk[i].setup)
			continue;
		n += snprintf(extra + n, sizeof(extra) - n, "%surl=%s/track%d;seq=%u;rtptime=%u", extra[n - 1] == ' ' ? "" : ",", url, i,
			      c->trk[i].seq, (unsigned)(i == TRK_VIDEO ? ts : c->trk[i].ts));
	}
	if (n < (int)sizeof(extra))
		n += snprintf(extra + n, sizeof(extra) - n, "\r\n");
	if (n >= (int)sizeof(extra))
		return reply(c, 500, "Internal Server Error", cseq, NULL, NULL, NULL);
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
		r = do_describe(c, cseq, req);
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

/* Consume whatever complete messages sit in c->rx: RTSP requests or '$' interleaved frames
 * (backchannel RTP on its channel -> the speaker; RTCP and anything else dropped). */
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
				c->bc_bad++;
				return 0;
			}
			if (c->rxlen < total)
				return 0;
			if (c->trk[TRK_BACK].setup && c->trk[TRK_BACK].tcp && (uint8_t)c->rx[1] == c->trk[TRK_BACK].chan)
				rtp_in(c, (const uint8_t *)c->rx + 4, total - 4);
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
	int one = 1, i, udp_only = 1;

	setsockopt(c->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(c->sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	c->last_rx_ms = c->start_ms = now_ms();
	os_printf("[rtsp] #%u %s connected\r\n", c->idx, c->peer);

	while (!c->stop && s_run) {
		fd_set rf;
		struct timeval st = {.tv_sec = 0, .tv_usec = c->playing ? 0 : 50000};
		int r, maxfd = c->sock, bu = c->trk[TRK_BACK].setup && !c->trk[TRK_BACK].tcp ? c->trk[TRK_BACK].udp : -1;

		FD_ZERO(&rf);
		FD_SET(c->sock, &rf);
		if (bu >= 0) {
			FD_SET(bu, &rf);
			if (bu > maxfd)
				maxfd = bu;
		}
		r = select(maxfd + 1, &rf, NULL, NULL, &st);
		if (r > 0) {
			if (FD_ISSET(c->sock, &rf)) {
				int n = recv(c->sock, c->rx + c->rxlen, sizeof(c->rx) - 1 - c->rxlen, 0);
				if (n <= 0)
					break;
				c->rxlen += n;
				c->last_rx_ms = now_ms();
				if (process_rx(c) < 0)
					break;
			}
			if (bu >= 0 && FD_ISSET(bu, &rf) && c->bc) {
				int n = recvfrom(bu, c->bc, RTSP_BC_BUF, 0, NULL, NULL);
				if (n > 0)
					rtp_in(c, c->bc, n);
			}
		} else if (r < 0) {
			break;
		}
		if (c->playing) {
			int a;
			r = send_next_frame(c);
			if (r < 0) {
				os_printf("[rtsp] #%u send failed\r\n", c->idx);
				break;
			}
			a = send_audio(c);
			if (a < 0) {
				os_printf("[rtsp] #%u audio send failed\r\n", c->idx);
				break;
			}
			if (r == 0 && a == 0)
				rtos_get_semaphore(&c->sem, 20);
		}
		udp_only = 1;
		for (i = 0; i < RTSP_TRACKS; i++)
			if (c->trk[i].setup && c->trk[i].tcp)
				udp_only = 0;
		if (udp_only && c->session && now_ms() - c->last_rx_ms > (RTSP_SESSION_TIMEOUT_S + 15) * 1000U)
			break;   /* UDP client went away without TEARDOWN */
	}

	os_printf("[rtsp] #%u %s closed: %u frames %u KB lost %u udp_err %u | mic %u pkts lost %u | back %u pkts bad %u\r\n", c->idx, c->peer,
		  (unsigned)c->sent_frames, (unsigned)(c->sent_bytes / 1024), (unsigned)c->lost, (unsigned)c->udp_err,
		  (unsigned)c->trk[TRK_MIC].pkts, (unsigned)c->aud_lost, (unsigned)c->trk[TRK_BACK].pkts, (unsigned)c->bc_bad);
	close(c->sock);
	for (i = 0; i < RTSP_TRACKS; i++)
		track_close_udp(&c->trk[i]);
	rtos_lock_mutex(&s_lock);
	c->playing = 0;
	c->busy_seq = 0;
	if (c->audio_ref)
		s_aud_refs--;
	c->audio_ref = 0;
	c->used = 0;
	s_nclients--;
	rtos_unlock_mutex(&s_lock);
	rtos_deinit_semaphore(&c->sem);
	if (c->tx)
		os_free(c->tx);
	c->tx = NULL;
	if (c->bc)
		os_free(c->bc);
	c->bc = NULL;
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
			int k;
			c = &s_cl[i];
			os_memset(c, 0, sizeof(*c));
			c->used = 1;
			c->idx = i;
			c->sock = cs;
			for (k = 0; k < RTSP_TRACKS; k++)
				c->trk[k].udp = -1;
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

/* The voc engine follows the audio tracks: started when the first client sets one up (retried
 * every 500 ms while the chime / a raw `spk` holds the hardware), stopped when the last one leaves.
 * A manually started engine (`talk voc on`) is used but not stopped. */
static void voc_housekeeping(void)
{
	bool want = s_aud_refs > 0;
	uint32_t now = now_ms();

	if (want && !voc_active()) {
		if (now - s_voc_try_ms >= RTSP_VOC_RETRY_MS) {
			int r;
			s_voc_try_ms = now;
			r = voc_start(rtsp_voc_mic);
			if (r == 0) {
				s_voc_ours = true;
				s_voc_fail = 0;
			} else {
				if (s_voc_fail++ == 0 || r != s_voc_last)
					os_printf("[rtsp] voc start -> %d (%s), retrying\r\n", r, r == -1 ? "raw engine busy" : "sdk error");
			}
			s_voc_last = r;
		}
	} else if (want) {
		voc_set_sink(rtsp_voc_mic);   /* running (ours or manual): make sure the mic reaches us */
		if (!s_voc_ours && s_voc_last != 1) {
			s_voc_last = 1;
		}
	} else if (voc_active() && s_voc_ours) {
		voc_stop();
		s_voc_ours = false;
	} else if (voc_active()) {
		voc_set_sink(NULL);
	}
	if (!want && !voc_active())
		s_voc_ours = false;
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
	voc_housekeeping();
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
	os_printf("[rtsp] server on port %u (" RTSP_PATH ": H.264 + PCMA mic, ONVIF backchannel)\r\n", s_port);
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
	if (s_voc_ours) {
		voc_stop();
		s_voc_ours = false;
	}
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
	s_aud_refs = 0;
	rtos_unlock_mutex(&s_lock);
	s_port = port ? port : RTSP_DEFAULT_PORT;
	s_run = true;
	ret = rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "rtsp", rtsp_task, 4096, NULL);   /* 4 KB: voc_start runs here */
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

int rtsp_audio_clients(void)
{
	return s_aud_refs;
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

int rtsp_status_audio(char *buf, size_t cap)
{
	return snprintf(buf, cap, "[rtsp] audio: voc %s%s aec %s | clients %d | mic in %u frames %u B | back in %u pkts %u B | voc fails %u\r\n",
			voc_active() ? "on" : "off", voc_active() ? (s_voc_ours ? " (rtsp)" : " (manual)") : "", voc_aec() ? "on" : "off",
			s_aud_refs, (unsigned)s_aud_in, (unsigned)s_aud_in_bytes, (unsigned)s_bc_pkts, (unsigned)s_bc_bytes, (unsigned)s_voc_fail);
}

int rtsp_status_clients(char *buf, size_t cap)
{
	int i, n = 0;

	buf[0] = 0;
	for (i = 0; i < RTSP_MAX_CLIENTS && n < (int)cap; i++) {
		rtsp_client_t *c = &s_cl[i];
		if (!c->used)
			continue;
		n += snprintf(buf + n, cap - n, "[rtsp] #%d %s v:%s a:%s b:%s %s %u frames %u KB lost %u udp_err %u | mic %u pkts lost %u | back %u pkts bad %u | %u s\r\n", i, c->peer,
			      c->trk[TRK_VIDEO].setup ? (c->trk[TRK_VIDEO].tcp ? "tcp" : "udp") : "-",
			      c->trk[TRK_MIC].setup ? (c->trk[TRK_MIC].tcp ? "tcp" : "udp") : "-",
			      c->trk[TRK_BACK].setup ? (c->trk[TRK_BACK].tcp ? "tcp" : "udp") : "-",
			      c->playing ? "playing" : "idle",
			      (unsigned)c->sent_frames, (unsigned)(c->sent_bytes / 1024), (unsigned)c->lost, (unsigned)c->udp_err,
			      (unsigned)c->trk[TRK_MIC].pkts, (unsigned)c->aud_lost, (unsigned)c->trk[TRK_BACK].pkts, (unsigned)c->bc_bad,
			      (unsigned)((now_ms() - c->start_ms) / 1000));
	}
	return n;
}

#else /* not the AVDK CPU0 image */
typedef int rtsp_c_not_built_for_this_core;
#endif
