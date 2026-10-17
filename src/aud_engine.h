/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: DMA audio engine (DAC/ADC rings in app_main.c) shared with the feature files. */
/* aud_engine.h — the DMA audio engine in app_main.c, exposed to feature files (talk.c).
 *
 * The engine (v0.13.x) serves the DAC and ADC FIFOs by DMA from two byte rings. spk_task /
 * micnet_task (the `spk` / `mic` commands) are one user; these calls let another thread (the HTTP
 * client thread for /speak and /mic.wav) own the speaker or the microphone for a while, with the
 * same mutual exclusion (`s_spk_busy` / `s_mic_busy`) so the ring chime, `spk` and `mic` never
 * fight over the hardware. CPU0 only. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Speaker. aud_spk_open() inits the DAC (gain 0 = the `spk cfg gain` value), starts the DMA on a
 * zeroed buffer and only then enables the amplifier. Returns 0, -1 when the speaker is busy
 * (chime / `spk` / another talk), < -1 on a driver error. The caller must call aud_spk_close(),
 * which drains the ring (blocks up to ~2 s), switches the amplifier off first and stops the DAC. */
int aud_spk_open(uint16_t gain);
/* Append s16le mono samples at aud_spk_rate(); blocks while the 16 KB ring is full.
 * Returns -1 when `spk stop` was requested (the caller should finish up). */
int aud_spk_write(const uint8_t *s16le, uint32_t bytes);
void aud_spk_close(void);
bool aud_spk_busy(void);          /* any speaker user, including the ring chime */
uint32_t aud_spk_rate(void);      /* configured DAC sample rate (8000) */
uint16_t aud_spk_gain(void);      /* the `spk cfg gain` default (0x1c) */
void aud_spk_stats(uint32_t *dma_frames, uint32_t *underruns);

/* Microphone. aud_mic_open() inits the ADC (gain 0 = the `mic cfg gain` value) and starts the DMA.
 * Returns 0, -1 when busy (`mic rec`), < -1 on a driver error. aud_mic_read() copies out whole
 * 32-bit FIFO words (lo16 = MIC1 = the board's electret, hi16 = MIC2/unused), returns bytes. */
int aud_mic_open(uint32_t gain);
uint32_t aud_mic_read(uint8_t *dst, uint32_t cap);
void aud_mic_close(void);
bool aud_mic_busy(void);
uint32_t aud_mic_rate(void);      /* real sample rate of the words: 2x the configured ADC rate (measured) */
void aud_mic_stats(uint32_t *dma_frames, uint32_t *overruns);

/* Arbitration with the voc engine (audio_voc.c): true while the raw engine drives the DAC or ADC
 * (chime, `spk`, `mic`, /speak, /mic.wav on the raw path). The raw opens above refuse while
 * voc_busy(); while voc runs they are routed through its taps instead. */
bool aud_raw_busy(void);
/* Speaker amplifier enable (GPIO 46, active high): voc switches it around its start/stop. */
void aud_pa_set(int on);

/* Command output: os_printf + the TCP command channel's REPLY buffer (640 bytes per command). */
void app_reply(const char *fmt, ...);
