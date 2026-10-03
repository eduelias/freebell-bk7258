/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: RTSP server interface. */
/* rtsp.h - minimal RTSP/1.0 server for the camera's hardware H.264 stream (AVDK build, CPU0).
 *
 * rtsp://<ip>:<port>/live  - one video track, H264/90000, RFC 6184 packetization-mode=1
 * (single NAL + FU-A), transports RTP/AVP/TCP (interleaved) and RTP/AVP (UDP unicast).
 *
 * The camera itself is owned by app_main.c: rtsp.c asks for it through rtsp_cam_acquire() and
 * feeds every H.264 access unit it receives (Annex-B byte stream, as produced by the BK7258
 * encoder on CPU1) into a PSRAM ring with rtsp_feed_frame(). Each client has its own thread
 * that packetizes straight out of that ring, so a slow client never blocks the camera callback
 * or the other clients. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RTSP_DEFAULT_PORT 554
#define RTSP_PATH "/live"

/* ---- implemented in rtsp.c ---- */

/* Start the listener thread on `port` (idempotent; returns 0 or a BK error). Allocates the
 * frame ring (128 KB PSRAM) once; it is kept until reboot. */
int rtsp_start(uint16_t port);
/* Ask the server to stop: it closes all clients, releases the camera (rtsp_cam_release) and
 * exits within ~1 s. Asynchronous. */
void rtsp_stop(void);
bool rtsp_running(void);
/* Connected clients; *playing (may be NULL) gets the number of them currently in PLAY. */
int rtsp_clients(int *playing);
/* Called from the camera frame callback with one complete H.264 access unit (Annex-B).
 * Cheap: a NAL scan + one memcpy into the ring; never blocks on a client. Must be called from
 * a task context (the media transfer task), not from an ISR. */
void rtsp_feed_frame(const uint8_t *data, uint32_t len, uint32_t ts_ms);
/* Ask the encoder for an IDR at the next opportunity (rate limited to one per 500 ms). */
void rtsp_request_idr(void);
/* One-line summary / per-client lines for the `rtsp status` command. Return the length. */
int rtsp_status(char *buf, size_t cap);
int rtsp_status_clients(char *buf, size_t cap);

/* ---- implemented by app_main.c (the camera owner) ---- */

/* Make sure the camera runs in H.264 mode and has produced its first frame (opens it, or
 * switches it from JPEG). May block for a few seconds. 0 = ok, <0 = refused or failed. */
int rtsp_cam_acquire(void);
/* The server stopped: close the camera if it is in H.264 mode so HTTP can reopen it as JPEG. */
void rtsp_cam_release(void);
/* Regenerate an IDR frame now (media_app_h264_regenerate_idr). */
void rtsp_cam_idr(void);
