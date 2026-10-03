/* talk.h — half-duplex two-way audio over HTTP (feature "talk", see TESTPLAN-talk.md).
 *
 * Served by the camera HTTP server on port 8080 (cam_http_client_task in app_main.c calls
 * talk_http_handle() first; it returns 1 when it consumed the request):
 *   POST /speak                 body = raw s16le mono 8 kHz, or a WAV (PCM 16-bit, mono/stereo,
 *                               any rate >= 8 kHz, resampled on the fly); plays through the speaker
 *   GET  /speak?tone=HZ&ms=MS   test tone
 *   GET  /speak?url=http://host[:port]/file.wav   the device fetches and plays the file (for HA)
 *   GET  /mic.wav[?seconds=N]   WAV stream (16 kHz mono s16le) from the microphone; infinite
 *                               (chunked) until the client disconnects, or N seconds
 *   GET  /talk                  tiny browser page (listen, tone, upload a WAV)
 * Half duplex: while anything plays on the speaker (talk, chime, `spk`), /mic.wav emits silence.
 * Settings (KV): `talk gain N|auto`, `talk micgain N`, `talk max SECONDS`. CPU0 only. */
#pragma once

void talk_init(void);                                  /* load KV settings; call once from user_app_main */
int talk_http_handle(int sock, const char *req, int n); /* 1 = handled (caller just closes the socket) */
void cli_talk_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv);
