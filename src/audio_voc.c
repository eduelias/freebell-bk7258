/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: full-duplex voice engine on the SDK's aud_intf voice mode (AEC + NS on CPU1). */
/* audio_voc.c — see audio_voc.h. What the SDK does for us (bk_avdk components/multimedia, v2.0.1):
 *   - CPU1 runs ADC (MIC1, differential) and DAC at 8 kHz with 20 ms frames, the AEC (reference =
 *     the decoded speaker PCM), NS, and G.711 encode/decode; CPU0 only sees A-law bytes.
 *   - mic: CPU1 writes encoded frames into a shared ring; the CPU0 task "aud_tras" calls our
 *     tx callback with 320 bytes (40 ms) at a time.
 *   - spk: bk_aud_intf_write_spk_data() appends to a decoder ring of frame_num x 320 bytes; the
 *     DAC side pulls 160 bytes per 20 ms and plays silence on underrun. A write that does not fit
 *     is dropped silently (BK_OK), so the speaker tap paces itself against the wall clock.
 *   - the SDK's own PA control (CONFIG_AUD_TRAS_DAC_PA_CTRL) is off for CPU1: it would drive GPIO 5,
 *     a camera rail on this board; the amplifier (GPIO 46) is switched here through aud_pa_set(). */
#include <common/bk_include.h>
#include "audio_voc.h"

#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
#include <os/os.h>
#include <os/str.h>
#include <os/mem.h>
#include <stdio.h>
#include <string.h>
#include "aud_intf.h"
#include "aud_intf_types.h"
#include <modules/g711.h>
#include "aud_engine.h"
#include "applog.h"   /* last: our os_printf lines go to the RAM log ring (policy.c) */

extern int bk_set_env_enhance(const char *key, const void *value, int value_len);
extern int bk_get_env_enhance(const char *key, void *value, int value_len);

#define KV_VOC_AEC "db_talk_aec"      /* "1"/"0", default on */
#define KV_VOC_MIC "db_voc_mic"       /* ADC gain for voice mode, default 0x3f (talk.c's speech gain) */

#define VOC_FRAME_NUM 10              /* speaker ring: 10 x 320 B = 400 ms max buffering */
#define VOC_FIFO_FRAMES 4             /* silence prefill before the DAC starts: 4 x 320 B = 160 ms */
#define VOC_PA_SETTLE_MS 50
#define VOC_MIC_DEFAULT_GAIN 0x3f
#define TAP_LEAD_BYTES 1600           /* the speaker tap may run 200 ms ahead of real time */
#define MICTAP_WORDS (1024)           /* 128 ms of 32-bit words at 8 kHz (/mic.wav drains it every 10 ms) */

enum { VOC_OFF = 0, VOC_STARTING, VOC_ON, VOC_STOPPING };

static volatile int s_state;
static volatile voc_mic_cb_t s_sink;
static bool s_aec = true;
static uint32_t s_mic_gain = VOC_MIC_DEFAULT_GAIN;
static int s_last_err, s_last_step;
static uint32_t s_starts, s_start_ms;
static volatile uint32_t s_mic_cbs, s_mic_bytes, s_spk_frames, s_spk_bytes, s_spk_drop_tap, s_spk_err;
/* speaker tap */
static volatile bool s_tap;
static uint32_t s_tap_t0, s_tap_bytes, s_tap_frames, s_tap_late, s_tap_part_n;
static uint8_t s_tap_part[VOC_FRAME_BYTES * 2];   /* partial PCM frame across writes (320 B = 160 samples) */
static uint8_t s_tap_enc[VOC_FRAME_BYTES];
/* microphone tap */
static volatile bool s_mictap;
static uint8_t s_mictap_ring[MICTAP_WORDS * 4];
static volatile uint32_t s_mt_w, s_mt_r, s_mt_frames, s_mt_overruns;   /* byte indices, monotonic */

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

/* ---- mic frames from CPU1 (SDK task "aud_tras", 2 KB stack): copy out, never block ---- */
static int voc_tx_mic(unsigned char *data, unsigned int size)
{
	voc_mic_cb_t sink = s_sink;

	s_mic_cbs++;
	s_mic_bytes += size;
	if (s_mictap) {   /* decode into the raw engine's word format for /mic.wav */
		uint32_t free = sizeof(s_mictap_ring) - (s_mt_w - s_mt_r), i;
		if (free >= size * 4) {
			for (i = 0; i < size; i++) {
				int16_t v = (int16_t)alaw2linear(data[i]);
				uint32_t idx = (s_mt_w + 4 * i) % sizeof(s_mictap_ring);
				s_mictap_ring[idx] = (uint8_t)v;
				s_mictap_ring[idx + 1] = (uint8_t)((uint16_t)v >> 8);
				s_mictap_ring[idx + 2] = 0;
				s_mictap_ring[idx + 3] = 0;
			}
			s_mt_w += size * 4;
			s_mt_frames++;
		} else {
			s_mt_overruns++;
		}
	}
	if (sink)
		sink(data, size);
	return (int)size;
}

/* ---- lifecycle ---- */

static void voc_unwind(int step)
{
	if (step >= 3)
		bk_aud_intf_voc_deinit();
	if (step >= 2)
		bk_aud_intf_set_mode(AUD_INTF_WORK_MODE_NULL);
	if (step >= 1)
		bk_aud_intf_drv_deinit();
}

int voc_start(voc_mic_cb_t sink)
{
	aud_intf_drv_setup_t drv = DEFAULT_AUD_INTF_DRV_SETUP_CONFIG();
	aud_intf_voc_setup_t voc = DEFAULT_AUD_INTF_VOC_SETUP_CONFIG();
	bk_err_t ret;
	int step = 0;

	GLOBAL_INT_DECLARATION();

	GLOBAL_INT_DISABLE();   /* the raw engine claims its hardware in the same kind of section (app_main.c) */
	if (s_state == VOC_ON) {
		if (!s_sink && sink)
			s_sink = sink;
		GLOBAL_INT_RESTORE();
		return 0;
	}
	if (s_state != VOC_OFF || aud_raw_busy()) {
		GLOBAL_INT_RESTORE();
		return -1;
	}
	s_state = VOC_STARTING;
	GLOBAL_INT_RESTORE();

	s_sink = sink;
	s_mic_cbs = s_mic_bytes = s_spk_frames = s_spk_bytes = s_spk_drop_tap = s_spk_err = 0;
	drv.work_mode = AUD_INTF_WORK_MODE_NULL;
	drv.aud_intf_tx_mic_data = voc_tx_mic;
	drv.aud_intf_rx_spk_data = NULL;   /* CPU1 would call this CPU0 pointer directly: must stay NULL */
	if ((ret = bk_aud_intf_drv_init(&drv)) != BK_OK)
		goto fail;
	step = 1;
	if ((ret = bk_aud_intf_set_mode(AUD_INTF_WORK_MODE_VOICE)) != BK_OK)
		goto fail;
	step = 2;
	voc.aec_enable = s_aec;
	voc.samp_rate = VOC_RATE;
	voc.data_type = AUD_INTF_VOC_DATA_TYPE_G711A;
	voc.mic_gain = (uint8_t)(s_mic_gain ? s_mic_gain : VOC_MIC_DEFAULT_GAIN);
	voc.spk_gain = aud_spk_gain();              /* 0x1c: above it the amplifier clips (audio.md) */
	voc.spk_mode = AUD_DAC_WORK_MODE_DIFFEN;
	voc.mic_type = AUD_INTF_MIC_TYPE_BOARD;
	voc.spk_type = AUD_INTF_SPK_TYPE_BOARD;
	voc.mic_mode = AUD_ADC_MODE_DIFFEN;        /* ignored by the SDK (always MIC1 differential) */
	voc.frame_num = VOC_FRAME_NUM;
	voc.fifo_frame_num = VOC_FIFO_FRAMES;
	if ((ret = bk_aud_intf_voc_init(voc)) != BK_OK)
		goto fail;
	step = 3;
	if ((ret = bk_aud_intf_voc_start()) != BK_OK)
		goto fail;
	rtos_delay_milliseconds(VOC_PA_SETTLE_MS);   /* DAC runs on the silence prefill first, then the PA */
	aud_pa_set(1);
	s_starts++;
	s_start_ms = rtos_get_time();
	s_last_err = 0;
	s_state = VOC_ON;
	os_printf("[voc] on: 8 kHz G711A, aec %s, mic gain 0x%x spk gain 0x%x, ring %u x 320 B (prefill %u)\r\n",
		  s_aec ? "on" : "off", voc.mic_gain, voc.spk_gain, VOC_FRAME_NUM, VOC_FIFO_FRAMES);
	return 0;
fail:
	os_printf("[voc] start failed at step %d: %d\r\n", step + 1, ret);
	s_last_err = ret;
	s_last_step = step + 1;
	voc_unwind(step);
	s_sink = NULL;
	s_state = VOC_OFF;
	return -2;
}

void voc_set_sink(voc_mic_cb_t sink)
{
	s_sink = sink;
}

void voc_stop(void)
{
	GLOBAL_INT_DECLARATION();

	GLOBAL_INT_DISABLE();
	if (s_state != VOC_ON) {
		GLOBAL_INT_RESTORE();
		return;
	}
	s_state = VOC_STOPPING;
	GLOBAL_INT_RESTORE();
	s_sink = NULL;
	s_tap = false;      /* a tap writer sees -1 on its next write */
	s_mictap = false;
	aud_pa_set(0);      /* amplifier off on whatever plays, then the DAC: no stop transient */
	rtos_delay_milliseconds(VOC_PA_SETTLE_MS);
	bk_aud_intf_voc_stop();
	voc_unwind(3);
	os_printf("[voc] off after %u s: mic %u cb / %u B, spk %u frames / %u B (%u dropped for a tap)\r\n",
		  (rtos_get_time() - s_start_ms) / 1000, s_mic_cbs, s_mic_bytes, s_spk_frames, s_spk_bytes, s_spk_drop_tap);
	s_state = VOC_OFF;
}

bool voc_active(void) { return s_state == VOC_ON; }
bool voc_busy(void) { return s_state != VOC_OFF; }

int voc_feed_spk(const uint8_t *g711, uint32_t len)
{
	if (s_state != VOC_ON)
		return -1;
	if (s_tap) {
		s_spk_drop_tap++;
		return 0;
	}
	if (bk_aud_intf_write_spk_data((uint8_t *)g711, len) != BK_OK)
		s_spk_err++;
	s_spk_frames++;
	s_spk_bytes += len;
	return 0;
}

/* ---- settings ---- */

void voc_set_aec(bool on)
{
	s_aec = on;
	kv_set_u32(KV_VOC_AEC, on ? 1 : 0);
	if (s_state == VOC_ON)
		bk_aud_intf_voc_aec_ctrl(on);
}

bool voc_aec(void) { return s_aec; }

void voc_set_mic_gain(uint32_t gain)
{
	s_mic_gain = gain;
	kv_set_u32(KV_VOC_MIC, gain);
	if (s_state == VOC_ON)
		bk_aud_intf_set_mic_gain((uint8_t)(gain ? gain : VOC_MIC_DEFAULT_GAIN));
}

uint32_t voc_mic_gain(void) { return s_mic_gain ? s_mic_gain : VOC_MIC_DEFAULT_GAIN; }

void voc_set_spk_gain(uint32_t gain)
{
	if (s_state == VOC_ON)
		bk_aud_intf_set_spk_gain(gain);
}

/* ---- speaker tap: PCM 8 kHz from the raw users, A-law encoded and paced ---- */

int voc_tap_open(void)
{
	GLOBAL_INT_DECLARATION();

	GLOBAL_INT_DISABLE();
	if (s_state != VOC_ON || s_tap) {
		GLOBAL_INT_RESTORE();
		return -1;
	}
	s_tap = true;
	GLOBAL_INT_RESTORE();
	s_tap_t0 = rtos_get_time();
	s_tap_bytes = s_tap_frames = s_tap_late = s_tap_part_n = 0;
	return 0;
}

static int tap_emit_frame(void)
{
	uint32_t i, allowed;

	for (i = 0; i < VOC_FRAME_BYTES; i++)
		s_tap_enc[i] = linear2alaw((int16_t)(s_tap_part[2 * i] | (s_tap_part[2 * i + 1] << 8)));
	/* pace: never more than TAP_LEAD_BYTES ahead of real time, or the SDK ring drops the frame */
	for (;;) {
		if (!s_tap)
			return -1;
		allowed = (rtos_get_time() - s_tap_t0) * (VOC_RATE / 1000) + TAP_LEAD_BYTES;
		if (s_tap_bytes + VOC_FRAME_BYTES <= allowed)
			break;
		rtos_delay_milliseconds(10);
	}
	if (bk_aud_intf_write_spk_data(s_tap_enc, VOC_FRAME_BYTES) != BK_OK)
		s_tap_late++;
	s_tap_bytes += VOC_FRAME_BYTES;
	s_tap_frames++;
	return 0;
}

int voc_tap_write(const uint8_t *s16le, uint32_t bytes)
{
	while (bytes) {
		uint32_t take = sizeof(s_tap_part) - s_tap_part_n;
		if (take > bytes)
			take = bytes;
		if (!s_tap)
			return -1;
		os_memcpy(s_tap_part + s_tap_part_n, s16le, take);
		s_tap_part_n += take;
		s16le += take;
		bytes -= take;
		if (s_tap_part_n == sizeof(s_tap_part)) {
			s_tap_part_n = 0;
			if (tap_emit_frame() != 0)
				return -1;
		}
	}
	return 0;
}

void voc_tap_close(void)
{
	uint32_t i;

	if (s_tap && s_tap_part_n >= 2) {   /* flush the last partial frame, zero padded */
		os_memset(s_tap_part + s_tap_part_n, 0, sizeof(s_tap_part) - s_tap_part_n);
		s_tap_part_n = 0;
		tap_emit_frame();
	}
	for (i = 0; i < 100 && s_tap; i++) {   /* drain: wait until real time caught up with what we queued */
		if ((rtos_get_time() - s_tap_t0) * (VOC_RATE / 1000) >= s_tap_bytes + VOC_FIFO_FRAMES * 320)
			break;
		rtos_delay_milliseconds(10);
	}
	s_tap = false;
}

bool voc_tap_open_p(void) { return s_tap; }
void voc_tap_stats(uint32_t *frames, uint32_t *late) { *frames = s_tap_frames; *late = s_tap_late; }

/* ---- microphone tap ---- */

int voc_mic_tap_open(void)
{
	GLOBAL_INT_DECLARATION();

	GLOBAL_INT_DISABLE();
	if (s_state != VOC_ON || s_mictap) {
		GLOBAL_INT_RESTORE();
		return -1;
	}
	s_mt_w = s_mt_r = 0;
	s_mt_frames = s_mt_overruns = 0;
	s_mictap = true;
	GLOBAL_INT_RESTORE();
	return 0;
}

uint32_t voc_mic_tap_read(uint8_t *dst, uint32_t cap)
{
	uint32_t n = s_mt_w - s_mt_r, idx, first;

	if (n > cap)
		n = cap;
	n &= ~3u;
	if (!n)
		return 0;
	idx = s_mt_r % sizeof(s_mictap_ring);
	first = sizeof(s_mictap_ring) - idx < n ? sizeof(s_mictap_ring) - idx : n;
	os_memcpy(dst, s_mictap_ring + idx, first);
	if (n > first)
		os_memcpy(dst + first, s_mictap_ring, n - first);
	s_mt_r += n;
	return n;
}

void voc_mic_tap_close(void) { s_mictap = false; }
void voc_mic_tap_stats(uint32_t *frames, uint32_t *overruns) { *frames = s_mt_frames; *overruns = s_mt_overruns; }

/* ---- init / status ---- */

void voc_init(void)
{
	uint32_t v;

	if (kv_get_u32(KV_VOC_AEC, &v))
		s_aec = v != 0;
	if (kv_get_u32(KV_VOC_MIC, &v) && v <= 0x3f)
		s_mic_gain = v;
	os_printf("[voc] aud_intf voice mode ready: aec %s, mic gain 0x%x (rtsp audio tracks start it)\r\n",
		  s_aec ? "on" : "off", voc_mic_gain());
}

int voc_status(char *buf, size_t cap)
{
	static const char *const st[] = {"off", "starting", "on", "stopping"};
	int n = snprintf(buf, cap, "[voc] %s aec %s mic 0x%x spk 0x%x | mic %u cb %u B | spk %u frames %u B drop-tap %u err %u | tap %s %u frames %u late | mictap %s %u ovr | starts %u",
			 st[s_state & 3], s_aec ? "on" : "off", voc_mic_gain(), aud_spk_gain(), s_mic_cbs, s_mic_bytes, s_spk_frames, s_spk_bytes,
			 s_spk_drop_tap, s_spk_err, s_tap ? "open" : "-", s_tap_frames, s_tap_late, s_mictap ? "open" : "-", s_mt_overruns, s_starts);
	if (s_last_err && n < (int)cap)
		n += snprintf(buf + n, cap - n, " | last err %d at step %d", s_last_err, s_last_step);
	if (s_state == VOC_ON && n < (int)cap)
		n += snprintf(buf + n, cap - n, " | up %u s", (rtos_get_time() - s_start_ms) / 1000);
	if (n < (int)cap)
		n += snprintf(buf + n, cap - n, "\r\n");
	return n;
}

#else /* not the AVDK CPU0 image: no voice engine */
#include <stdio.h>
void voc_init(void) {}
int voc_start(voc_mic_cb_t sink) { (void)sink; return -2; }
void voc_set_sink(voc_mic_cb_t sink) { (void)sink; }
void voc_stop(void) {}
bool voc_active(void) { return false; }
bool voc_busy(void) { return false; }
int voc_feed_spk(const uint8_t *g711, uint32_t len) { (void)g711; (void)len; return -1; }
void voc_set_aec(bool on) { (void)on; }
bool voc_aec(void) { return true; }
void voc_set_mic_gain(uint32_t gain) { (void)gain; }
uint32_t voc_mic_gain(void) { return 0x3f; }
void voc_set_spk_gain(uint32_t gain) { (void)gain; }
int voc_tap_open(void) { return -1; }
int voc_tap_write(const uint8_t *s16le, uint32_t bytes) { (void)s16le; (void)bytes; return -1; }
void voc_tap_close(void) {}
bool voc_tap_open_p(void) { return false; }
void voc_tap_stats(uint32_t *frames, uint32_t *late) { *frames = *late = 0; }
int voc_mic_tap_open(void) { return -1; }
uint32_t voc_mic_tap_read(uint8_t *dst, uint32_t cap) { (void)dst; (void)cap; return 0; }
void voc_mic_tap_close(void) {}
void voc_mic_tap_stats(uint32_t *frames, uint32_t *overruns) { *frames = *overruns = 0; }
int voc_status(char *buf, size_t cap) { return snprintf(buf, cap, "[voc] not built (bare SDK image)\r\n"); }
#endif
