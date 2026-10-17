/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: camera HTTP server on port 8080: /snapshot.jpg, /stream, /, and the talk/cmd/log routes. */
/* Plain HTTP for Home Assistant (split out of app_main.c in 0.28.0) */
#include "app_internal.h"

#if (CONFIG_SYS_CPU0)
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
/* ---- camera HTTP server (AVDK build): GET /snapshot.jpg = latest JPEG, GET /stream = MJPEG
 * (multipart/x-mixed-replace), GET / = status. Plain HTTP on the LAN for Home Assistant's
 * generic/mjpeg camera; the frame is copied out of the shared buffer first so the camera callback
 * is never blocked by a slow client. ---- */
#define CAM_HTTP_PORT 8080
#define CAM_HTTP_MAX_CLIENTS 6   /* HA keeps the MJPEG stream open; leave room for snapshots and a browser */
static volatile int s_cam_http_clients;

#endif
#endif
#if (CONFIG_SYS_CPU0)
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
static int send_all(int sock, const void *p, int n)
{
	int off = 0, r;
	while (off < n && (r = send(sock, (const uint8_t *)p + off, n - off > 4096 ? 4096 : n - off, 0)) > 0)
		off += r;
	return off == n ? 0 : -1;
}

static void cam_http_client_task(beken_thread_arg_t arg)
{
	int sock = (int)(intptr_t)arg, n;
	char req[384], hdr[192];   /* 384: GET /cmd?t=<token>&c=<url-encoded command> must fit in the first read */
	uint8_t *buf = NULL;
	uint32_t seq = 0, last = 0, settle = 0, s_buf_cap = 0;
	struct timeval tv = {.tv_sec = 5, .tv_usec = 0};

	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));   /* a stalled stream client is dropped, not held forever */
	n = recv(sock, req, sizeof(req) - 1, 0);
	if (n <= 0)
		goto out;
	req[n] = 0;
	if (talk_http_handle(sock, req, n))   /* /speak, /mic.wav, /talk (talk.c): handled, just close */
		goto out;
	if (policy_http_handle(sock, req, n))   /* /cmd?t=&c=, /log?t= (policy.c): handled, just close */
		goto out;
	if (os_strncmp(req, "GET /stream", 11) == 0 || os_strncmp(req, "GET /snapshot", 13) == 0) {
		if (cam_want_jpeg() == 0) {   /* open on demand (or switch back from H.264); the first frame takes ~1 s */
			for (int w = 0; w < 300 && s_cam_len == 0; w++)
				rtos_delay_milliseconds(10);
			settle = cam_settle_wait();   /* cold open: wait for the exposure to settle (X-Settle-Ms) */
		} else if (os_strncmp(req, "GET /stream", 11) == 0 || !s_cam_len) {
			n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 24\r\nConnection: close\r\n\r\ncamera busy (H.264/RTSP)");
			send_all(sock, hdr, n);
			goto out;
		}
		/* else: the snapshot is the last JPEG taken before the switch to H.264 (stale but useful) */
	}
	if (!s_cam_mutex || !s_cam_buf) {
		n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 15\r\nConnection: close\r\n\r\ncamera not open");
		send_all(sock, hdr, n);
		goto out;
	}
	/* Client buffer: the MJPEG stream needs the full size; a snapshot gets the current frame plus
	 * a margin (retried at full size if the frame grew), so three concurrent snapshots (HA's wake
	 * photo, its preview and a tool) fit the 512 KB PSRAM heap next to the 150 KB frame buffer
	 * and the 128 KB RTSP ring. 150 KB each left room for one: the others got a closed socket. */
	{
		uint32_t cap = os_strncmp(req, "GET /stream", 11) == 0 || !s_cam_len ? CAM_BUF_MAX : s_cam_len + 16 * 1024;
		if (cap > CAM_BUF_MAX)
			cap = CAM_BUF_MAX;
		buf = psram_malloc(cap);
		if (!buf) {
			n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 11\r\nConnection: close\r\n\r\nbusy (mem)\n");
			send_all(sock, hdr, n);
			goto out;
		}
		s_buf_cap = cap;
	}
	if (os_strncmp(req, "GET /stream", 11) == 0) {
		n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
			     "Cache-Control: no-cache\r\nX-Settle-Ms: %u\r\nConnection: close\r\n\r\n", (unsigned)settle);
		if (send_all(sock, hdr, n))
			goto out;
		for (;;) {
			for (int w = 0; w < 200 && s_cam_seq == last; w++)   /* wait up to 2 s for a new frame */
				rtos_delay_milliseconds(10);
			if (s_cam_seq == last)
				continue;
			n = cam_copy_latest(buf, CAM_BUF_MAX, &seq);
			if (n <= 0)
				continue;
			last = seq;
			int hl = snprintf(hdr, sizeof(hdr), "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", n);
			if (send_all(sock, hdr, hl) || send_all(sock, buf, n) || send_all(sock, "\r\n", 2))
				break;
		}
	} else if (os_strncmp(req, "GET /snapshot", 13) == 0) {
		n = cam_copy_latest(buf, s_buf_cap, &seq);
		if (n <= 0 && s_cam_len > s_buf_cap) {   /* the frame grew past the margin: once more at full size */
			os_free(buf);
			buf = psram_malloc(CAM_BUF_MAX);
			if (buf)
				n = cam_copy_latest(buf, CAM_BUF_MAX, &seq);
		}
		if (n > 0) {
			int hl = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n"
					  "Cache-Control: no-cache\r\nX-Settle-Ms: %u\r\nConnection: close\r\n\r\n", n, (unsigned)settle);
			if (!send_all(sock, hdr, hl))
				send_all(sock, buf, n);
		} else {
			n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 12\r\nConnection: close\r\n\r\nno frame yet");
			send_all(sock, hdr, n);
		}
	} else {
		char body[160];
		int bl = snprintf(body, sizeof(body), "<html><body>doorbell " FW_VERSION " cam open=%d frames=%u %ux%u<br>"
				  "<a href=/snapshot.jpg>snapshot.jpg</a> <a href=/stream>stream</a></body></html>",
				  s_cam_open, s_cam_frames, s_cam_w, s_cam_h);
		n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", bl);
		if (!send_all(sock, hdr, n))
			send_all(sock, body, bl);
	}
out:
	if (buf)
		os_free(buf);
	close(sock);
	s_cam_http_clients--;
	rtos_delete_thread(NULL);
}

static void cam_http_task(beken_thread_arg_t arg)
{
	struct sockaddr_in sa = {0};
	int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;

	if (ls < 0)
		goto out;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(CAM_HTTP_PORT);
	sa.sin_addr.s_addr = INADDR_ANY;
	if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(ls, 2) < 0) {
		os_printf("[cam] http bind/listen failed\r\n");
		close(ls);
		goto out;
	}
	os_printf("[cam] http server on port %d\r\n", CAM_HTTP_PORT);
	for (;;) {
		int c = accept(ls, NULL, NULL);
		beken_thread_t t;
		if (c < 0)
			continue;
		if (s_cam_http_clients >= CAM_HTTP_MAX_CLIENTS) {
			close(c);
			continue;
		}
		s_cam_http_clients++;
		power_activity();
		if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "camhttp", cam_http_client_task, 6144, (beken_thread_arg_t)(intptr_t)c) != BK_OK) {   /* 6 KB: talk.c (url fetch + WAV parser) runs in this thread */
			s_cam_http_clients--;
			close(c);
		}
	}
out:
	rtos_delete_thread(NULL);
}

#endif
#endif

#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
/* ---- module API ---- */
void cam_http_start(void)
{
	beken_thread_t http;

	rtos_create_thread(&http, BEKEN_APPLICATION_PRIORITY, "camhttp", cam_http_task, 4096, NULL);
}

bool cam_http_busy(void)
{
	return s_cam_http_clients > 0;
}
#endif
