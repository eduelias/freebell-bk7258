# Camera: OV9650, JPEG/H.264 pipeline, RTSP findings

## Sensor

OmniVision **OV9650** (1280x720, nominally 15 fps) on I2C1 = GPIO 0/1 at address 0x30, power on
GPIO 28 (active high), PWDN/RESET pulse on GPIO 52 (low, 100 ms high, low) before open, MCLK
24 MHz. The driver (`avdk-overlay/components/bk_peripheral/src/dvp/dvp_ov9650.c`) follows the
structure of Beken's `dvp_ov2640.c`; the register configuration it programs is the one the shipped
firmware uses, recovered from the device's own flash. The sensor is SCCB: a register read must be
write + STOP + read; the SDK's repeated-start helper fails on it, hence the driver's own
`ov9650_sccb_read`. SDK patch 0006 adds `ID_OV9650` to the sensor-id enum and AVDK patch 0001
registers the driver.

First-frame log on CPU1: `ov9650_detect, id: 0x9652`, `auto detect success, dvp camera
name:ov9650`, then about 9 fps of ~33 KB JPEGs. CPU1 info logs are compiled out (log level 2):
use `BK_LOGW` to see driver prints.

The AVDK build splits the work: CPU0 runs the application and the media client, CPU1 the
camera/JPEG/H.264 pipeline, CPU2 the media minor.

## HTTP (v0.17)

`http://<device-ip>:8080/snapshot.jpg` and an MJPEG stream at `/stream`; both open the camera on
first use (first frame after about 1 s). The server allows 6 clients and drops stalled ones
(v0.17.1: Home Assistant holds the MJPEG stream open). `/` is a one-line status page and answers
`503 camera not open` until the first snapshot/stream request. `cam open [W H] | close | stat |
snap HOST PORT | probe ...` are the manual controls.

## H.264 over RTSP (v0.22, `rtsp.c`)

`rtsp://<device-ip>:554/live`: an in-firmware RTSP/1.0 server with RFC 6184 packetization (TCP
interleaved and UDP unicast, up to 3 clients), fed by the BK7258 hardware H.264 encoder (1280x720
Baseline, IDR with SPS/PPS every 30 frames = `CONFIG_H264_P_FRAME_CNT=29` on CPU1, plus an IDR on
every PLAY). The DVP pipeline runs one encoder at a time, so the camera has a mode: JPEG (HTTP) or
H.264 (RTSP), chosen by `cam mode auto|jpeg|h264`. In `auto` an RTSP client switches to H.264 and
an HTTP request switches back to JPEG unless an RTSP client is playing; then `/snapshot.jpg` serves
the last JPEG and `/stream` answers 503. The first `/snapshot.jpg` after a switch back can still be
the previous JPEG (the handler does not wait for a new frame). `rtsp q IMB PMB` sets bits per
macroblock for I/P frames (default 120/24, about 1.5 Mbit/s nominal, applied live).

TCP is the robust transport: `ffplay -rtsp_transport tcp rtsp://<device-ip>:554/live`. UDP showed
`RTP: missed 6 packets` in 12 s on Wi-Fi; TCP lost 0 in a 20 min soak.

### Frame rate is the sensor's, not the stream's

The OV9650 with the shipped register configuration gives about 8-9 fps in daylight (and right after
a power-on reset) and about 3 fps in a dark room: COM11 (`0x3b = 0xa9`) enables night mode, which
halves the frame rate as exposure grows. Measured on an evening: H.264 and JPEG both 2.9-6 fps,
identical at 800x480, and the CPU1 `media_debug` counters show `isr_jpeg == fps_wifi`, so nothing
is lost in the CPU1 -> CPU0 hand-off. Consequences: ffmpeg/ffprobe need 5-11 s to probe the
stream, and Home Assistant's still-from-stream takes about 7 s (its 10 s timeout is marginal at
3 fps). Options not taken yet: disable night mode in the sensor table (constant ~9 fps, darker night
picture) or a GOP of about 10 frames.

### Camera power-cycle on close (v0.22.1-0.22.3)

A `cam close` followed by a reopen within a few seconds (exactly what the JPEG <-> H.264 switch
does, and what Home Assistant's generic camera does after any close) used to leave the pipeline
wedged: `cam stat` said open but the frame counter never moved, later `media_app_camera_open`
returned -1, every HTTP request got an instant connection close, and `cam close` itself blocked the
command task for about 15 s. Only a reboot cleared it; a reopen 7 s after the close worked once,
1-5 s did not.

Fix: `cam_close_locked()` now cuts the camera rails (GPIO 28/5/6) on every close, and the next
open waits 800 ms of power-off plus 150 ms settle before the GPIO 52 pulse, so every reopen is a
sensor power-on reset. 250 ms/60 ms was not enough (3 of 8 cold opens silent or undetected);
800/150 gave 7 of 8 and the remaining case self-heals: a camera open for 4 s without a frame is
power-cycled at the next use (`cam_stalled_locked()`), `rtsp_cam_acquire()` retries a silent open
once, and a failed `media_app_camera_open` powers the sensor off so the retry is a real power cycle
(0.22.3). A JPEG -> H.264 switch takes about 2.1 s. `cam probe rails off` + `on` does the same by
hand.

### Home Assistant camera entry: still URL plus RTSP stream

A stream-only generic camera entry (stills taken from the RTSP stream) was tried first and
rejected: at ~3 fps HA's still-from-stream (go2rtc -> ffmpeg) needs about 7 s and hit its 10 s
timeout half the time, which would also break the `camera.snapshot` ring/motion photos. The entry
therefore keeps **both** URLs: still `http://<device-ip>:8080/snapshot.jpg`, stream
`rtsp://<device-ip>:554/live` with `rtsp_transport: tcp` (HA 2026.9 keeps it under `advanced`),
framerate 5. With the still URL the thumbnail/snapshot takes 45 ms (the first one after a live view
about 2.4 s, because the camera switches back to JPEG; while someone is watching it is the last
JPEG). The HA live view uses go2rtc (WebRTC/HLS) and shows up in `rtsp status` as one TCP client
from the HA host. `tools/ha_generic_camera.py options ENTRY_ID --still ... --stream ...
--rtsp-transport tcp --framerate 5` sets this over the config-flow API.

## Debug: reading CPU1 counters from CPU0

`mem HEXADDR [N]` reads raw 32-bit words. CPU1's `media_debug_t` counters are reachable from CPU0
through the `transfer_log` pointer in the CPU0 map: `grep transfer_log $OUT/doorbell/bk7258/app.map`,
then `mem <that address> 1` gives the struct address; word 0 = `isr_jpeg | isr_h264 << 16`, word 4
high half = `fps_wifi`, words 6/7 = last JPEG/H.264 length. A bad address faults, so this is a
bench tool only.
