/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: full-duplex voice engine (SDK aud_intf voice mode, AEC + NS on CPU1). */
/* audio_voc.h — the "voc" audio engine: Beken's aud_intf voice mode. The ADC/DAC, the acoustic
 * echo canceller, noise suppression and the G.711 codec run on CPU1 (multimedia/aud/aud_tras_drv.c);
 * CPU0 talks to it over the media mailbox and gets encoded microphone frames in a callback.
 *
 * Format: 8 kHz, G.711 A-law (RTP payload type 8), 160 bytes per 20 ms. The SDK delivers mic data in
 * 320-byte pieces (40 ms) and keeps a speaker jitter ring of `frame_num` x 320 bytes.
 *
 * Arbitration with the raw DMA engine in app_main.c (chime, `spk`, `mic`, /speak, /mic.wav):
 * exactly one engine owns the audio hardware. voc_start() refuses while the raw engine is busy
 * (aud_raw_busy()); the raw opens refuse while voc_busy(). While voc runs, the raw users are routed
 * through the "taps" below (app_main.c's aud_spk_* / aud_mic_* dispatch there), so /speak, the chime
 * and /mic.wav keep working during a live RTSP audio session.
 *
 * Users: rtsp.c (first audio track set up -> voc_start, last client gone -> voc_stop), talk.c
 * (`talk aec|vocmic|voc` commands, status). CPU0, AVDK build only; stubs otherwise. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VOC_RATE 8000
#define VOC_FRAME_BYTES 160          /* one 20 ms RTP frame of G.711 */
#define VOC_PT 8                     /* PCMA */

/* Encoded microphone frames (G.711 A-law, 8 kHz). Called on the SDK's "aud_tras" task (2 KB stack,
 * every 40 ms with 320 bytes): copy and signal, never block. */
typedef void (*voc_mic_cb_t)(const uint8_t *g711, uint32_t len);

void voc_init(void);                       /* KV settings; call once early in user_app_main */
/* 0 = running (also when it already ran: the sink is set if none was), -1 = starting/stopping or the
 * raw engine is busy (try again later), < -1 = SDK error (see voc_status). */
int voc_start(voc_mic_cb_t sink);
void voc_set_sink(voc_mic_cb_t sink);      /* NULL = drop mic frames (manual `talk voc on` mode) */
void voc_stop(void);
bool voc_active(void);                     /* fully started */
bool voc_busy(void);                       /* anything but off: the raw engine must stay away */
/* Speaker frames from the network (G.711 A-law, any multiple of 160 bytes). Dropped while a speaker
 * tap (chime, /speak) is open. 0 = queued/dropped, -1 = engine not running. */
int voc_feed_spk(const uint8_t *g711, uint32_t len);
/* Live AEC switch (KV db_talk_aec, default on) and the voc mic gain (KV db_voc_mic, 0 = 0x3f). */
void voc_set_aec(bool on);
bool voc_aec(void);
void voc_set_mic_gain(uint32_t gain);
uint32_t voc_mic_gain(void);
void voc_set_spk_gain(uint32_t gain);      /* applied live when running (0x1c = loudest clean level) */

/* Speaker tap: s16le mono 8 kHz PCM into the voc speaker path (A-law encoded here, paced to real
 * time). One tap at a time. voc_tap_write() returns -1 when the engine went away. */
int voc_tap_open(void);
int voc_tap_write(const uint8_t *s16le, uint32_t bytes);
void voc_tap_close(void);
bool voc_tap_open_p(void);
void voc_tap_stats(uint32_t *frames, uint32_t *late);
/* Microphone tap: decoded mic PCM as 32-bit words (lo16 = sample, hi16 = 0, the raw engine's word
 * format) so talk.c's /mic.wav reads it unchanged, at VOC_RATE. */
int voc_mic_tap_open(void);
uint32_t voc_mic_tap_read(uint8_t *dst, uint32_t cap);
void voc_mic_tap_close(void);
void voc_mic_tap_stats(uint32_t *frames, uint32_t *overruns);

/* one line for `talk status` / `rtsp status`; returns the length */
int voc_status(char *buf, size_t cap);
