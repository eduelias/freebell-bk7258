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

## Exposure at a cold open: scene hint + settle gate (v0.27.0)

### Problem (measured 2026-10-05 10:15, sunny morning, light sensor 659 mV)

Every open is a sensor power-on reset (see above), and the shipped register table starts the
OV9650 in **night mode** (COM11 `0x3b = 0xa9`, bit 7: frame-rate reduction as exposure grows),
with a long initial exposure (`0xa1 = 0x41` sets AEC[10], `0x10 = 0x06`), gain `0x00 = 0x80`, the
AGC ceiling at 4x (`0x14 = 0x1a`) and a bright AE target (AEW/AEB `0x24/0x25 = 0x98/0x88`). In
daylight the first frames are white and, at the 2-3 fps night mode gives while exposure is long,
the AEC loop needs about 10 s to come down. `/snapshot.jpg` mean luma / % of pixels >= 250 against
time since the open, fw 0.26.2:

| after open | mean luma | clipped |
|---|---|---|
| +2 s | 252 | 93 % |
| +6 s | 158 | 14 % |
| +9 s | 146 | 3 % |
| +13 s | 127 | 1 % (good picture, natural colours) |

Home Assistant takes the ring/motion/wake photo 2-5 s after the wake, so those were white.
Same scene, same script as the after-measurement below (RTC wake 10:49, 654 mV, the first
request opens the camera and is answered 2.1 s later with the first frame), fw 0.26.2:

| after open | +2.1 s | +3.8 s | +4.5 s | +5.2 s | +7.3 s | +8.1 s | +11.0 s | +13.9 s |
|---|---|---|---|---|---|---|---|---|
| mean luma | 252 | 224 | 190 | 163 | 156 | 149 | 142 | 128 |
| clipped | 94 % | 56 % | 42 % | 15 % | 7 % | 3.6 % | 1.1 % | 0.4 % |

70 JPEGs in the first 14 s (5 fps average: night mode starts slow), heap 87 KB with the camera
open and nothing else running.

### Scene hint to CPU1 (no SDK patch)

CPU0 cannot write sensor registers (the camera, I2C and the sensor driver live on CPU1) and the
AVDK mailbox has no "write sensor register" message. What it does have: `media_app_camera_open()`
sends the `media_camera_device_t` by pointer (`EVENT_CAM_DVP_OPEN_IND`), CPU1 `camera_act.c`
memcpy's it into `camera_info.device`, `dvp_camera.c` (bk_idk) copies it again and, right after
`current_sensor->init()` programmed the table, calls `current_sensor->set_fps(device->info.fps)`.
`frame_fps_t` is a bit-mask enum (FPS5..FPS30 = bits 0-5) and **one byte wide** with this
toolchain (`arm-none-eabi-gcc` 10.3 defaults to `-fshort-enums`; checked), and the OV9650 runs at
a fixed 15 fps (bit 2), so the other seven bits are a free channel into the driver, carried
through both copies unchanged: bit 7 = night, bit 6 = day, bits 0-1 and 3-5 = a 5-bit
**start-exposure class** (`OV9650_EXP_ENCODE()`, same macros in `src/camera.c` and
`avdk-overlay/.../dvp_ov9650.c`). `cam_open_locked()` reads the light sensor once the camera rail
is up (GPIO 28 feeds the LDR divider, so this costs nothing; the reading is retried when it says
< 100 mV = divider off, and falls back to the last plausible value - see the race below), picks
`night` above the `ring dark` threshold, maps the mV to a start-exposure class for `day`
(`cam_exp_class()`, rows = 1 << (class - 1): < 750 mV -> 4 rows, < 1000 -> 16, < 1250 -> 64,
else 256; `cam exp 1-11` fixes a class, KV `db_cam_exp`) and packs it all into `dev.info.fps`. `ov9650_set_fps()` logs it on
CPU1 and programs, after the table:

| scene | after the table | effect |
|---|---|---|
| day | `0x13=0xe2` COM8 AEC+AGC **off** (fast algorithm, unlimited step, banding, AWB kept); `0x3b=0x29` COM11 night mode off; `0x14=0x0a` AGC ceiling 2x; `0x24/0x25=0x78/0x68` AE target; `0xa1=0x00`, `0x10=lines>>2`, `0x04=lines&3` start exposure; `0x00=0x00` gain 1x; `0x13=0xe7` AEC+AGC back on | the AEC loop starts from a daylight exposure instead of from white |
| night | `0x3a=0x11`, `0x67=0x80`, `0x68=0x80` | the shipped table (night mode, AGC 4x) plus the stock day/night switch's black-and-white (fixed U/V) |
| stock | nothing | 0.26 behaviour (`cam scene stock`) |

The AEC-off/on bracket is the whole point: a first version of this code wrote the same registers
with AEC enabled and changed **nothing** (A/B `cam scene stock` vs `day` in one window, 11:07:
curves identical to the frame) - the engine owns AECH/COM1/AECHM/GAIN while COM8 bit 0/2 are set
(the Linux `ov9650.c` driver clears them before a manual exposure too), and in daylight night mode
never engages anyway (it stretches exposure beyond a frame only in the dark). Register layout from
that driver: exposure = AECHM[5:0]<<10 | AECH<<2 | COM1[1:0] in row times, row time =
2 x 1520 / f_int (about 127 us at 24 MHz, so 64 lines ~ 8 ms ~ 1/125 s).

`cam scene auto|day|night|stock` (KV `db_cam_scene`) forces a scene; `cam stat` shows the scene
of the open camera, the light reading it was based on and the exposure class sent.

**Checking CPU1 from CPU0.** Nothing on CPU1 is visible without UART, so the driver leaves two
globals that `mem` can read (SRAM is one bus for both cores; addresses from the cp1 `app.map`,
`grep ov9650_dbg`): `ov9650_dbg[0]` = `0x965000xx` with the fps word received, `[1]` =
COM8<<24 | COM11<<16 | COM9<<8 | AECH read back after the sequence, `[2]` = AECHM<<24 |
COM1<<16 | GAIN<<8 | AEW, `[3]` = start lines; `ov9650_trace[0..29]` = AEC lines | GAIN<<16 |
COM8<<24 sampled every 500 ms for 15 s after a day open by a short-lived CPU1 thread (1 KB
stack), `[30] = 0xd0ded0de` when done - the converged exposure for a light level, i.e. the data
to tune `cam_exp_class()` with. The flash image can be checked the same way: `mem 021c47a8 4`
on the first 0.27 build returned the day register table, which is how "the OTA does update
CPU1" was established (the rbl = app + app1 + app2 from `encrypt/pack/configuration.json`).

**Race fixed on the way.** At a cold wake the first open read the light sensor as 31 mV: the
wake report's `light_read_mv()` in the reporter thread raises GPIO 28, reads, and drops it again
when the camera is not open yet - under the open that had just raised the rails. `s_cam_opening`
now keeps it from touching the pin, the open re-reads a bogus value and falls back to the last
plausible reading, so a night wake cannot be mistaken for day.

**Snapshot client buffers.** The first `/snapshot.jpg` at a cold wake used to get a closed socket
when HA's own wake photo and preview were in flight: each client allocated a 150 KB PSRAM buffer
and the 512 KB heap (minus the 150 KB frame buffer and the 128 KB RTSP ring) fits one. A snapshot
now allocates the current frame plus 16 KB (full size again only if the frame grew) and an
allocation failure answers `503 busy (mem)`.

**IR-cut coil**: the stock firmware pulses GPIO 54 high 300 ms for day and GPIO 55 for night
(`docs/notes/pin-mapping.md`). 0.27 does the same at an open whose scene differs from the last
pulse (KV `db_ircut`; unknown at first boot, so the first open pulses). The pulse starts right
after the light reading and ends after the GPIO 52 reset pulse, so it adds nothing to the open
time. `cam ircut day|night` pulses by hand. Pulsing from CPU0 in daylight made no visible
difference (colours were natural before and after), so the filter was most likely already in the
day position; the owner can check the night side with `cam stat` (`ircut night`), `night status`
and a snapshot after dusk (B/W expected with the white LED on).

### Settle gate

JPEG consumers (`/snapshot.jpg`, the first `/stream` frame, the SD save in `app_cam_grab()`) wait
after an open until the AEC has settled: `cam settle N [MS [PCT]]` (KV `db_cam_settle`, default
`16 4500 4`) = settled after N frames since the open, or earlier (but not before N/2 frames) once
three consecutive JPEGs are within PCT % of each other in size (a weak proxy: 46.7 KB at 49 %
clipped against 45.7 KB when good, hence the N/2 floor; the sensor exposes no AEC-done flag),
capped at MS after the open (open ~1 s + 4.5 s stays well inside HA's 10 s snapshot timeout). The
wait the request actually spent is in the `X-Settle-Ms` response header; `cam stat` shows
`open_frames`, `settled <how>@<ms after open>` (`n` frames, `s` stable size, `t` cap, `-` not yet),
`wait <ms> x<count>` and the three settings. In H.264 mode the gate is off (RTSP has its own
first-frame logic).

### Measured on the way (2026-10-05, same sunny scene, same script, times from the first request)

Each row: mean luma / % clipped of `/snapshot.jpg`. The first request opens the camera (the open
itself is ~0.4 s of rail/reset delays plus CPU1 init, first frame ~1 s later) and blocks in the
settle gate, so its row also shows `X-Settle-Ms`.

| build | first served frame | then | AEC trace (rows, 0.5 s steps) |
|---|---|---|---|
| 0.26.2 baseline, 10:49 | +2.1 s 252 / 94 % | +3.8 s 224/56, +5.2 s 163/15, +8.1 s 149/3.6, +11.0 s 142/1.1, +13.9 s 128/0.4 | - |
| v1 (registers written with AEC on), 11:07 cold wake; A/B `scene stock` vs `day` | +3.5 s 239 / 65 % (settle 1262) | identical to the baseline curve, stock and day alike | - |
| v3 (AEC off -> 64 rows -> on), 11:25 | +4.3 s 245 / 81 % (settle 1266) | +5.3 s 157/1.1, +8.1 s 150/0.4 | 64, 130, 130, 65, 17, 9, 5, 2, 1, then gain 0x0f -> 0 over 5 s |
| v4 (AEC off -> 4 rows -> on), 11:42 | +3.5 s 246 / 85 % (settle 1260) | +4.7 s 210/49, +5.4 s 164/0.7, +8.2 s 157/0.2 | 4, 130, 65, 33, 17, 5, 3, 1, then gain 0x0f -> 0 |
| v5 `cam exp 16` manual-first 1 row, 12:00 | **+4.3 s 147 / 0.5 %** (settle 2322: the gate's N/2 floor, picture good from the first frame) | 147/0.4 flat | 1, 1 (AEC off), then AEC on: 1, 1, 1 ... gain 0 (no restart) |
| v5 `cam exp 17` manual-first 2 rows | +4.3 s 180 / 5.1 % | +5.2 s 173/3.0, +8.7 s 148/0.8 | 2, 2, then 1 with gain 0x0f -> 0 |
| v5 `cam exp 0` = AEC-on 4 rows, gate 20 | +5.5 s 186 / 10 % (settle 3420) | +7.1 s 177/4.8, +8.5 s 167/2.8 | as v4 |

Read: writing the exposure with AEC on does nothing (v1); with the AEC-off bracket the start is
honoured for one frame but the engine then restarts at ~130 rows (v3/v4: the dark start-up frames
make it overshoot) and halves once per ~2 frames (~5 fps) down to 1 row, so an AEC-on start is
good only ~4 s after the open whatever the start value, though the lower target/ceiling already
halve the baseline's tail (clipped < 1.5 % at +5.3 s instead of +11 s). Enabled a second later on
a correctly exposed stream the engine stays put (v5 `exp 16`): that is the 0.27.0 day default,
with the start rows from the light reading (< 700 mV -> 1 row, measured at 515-663 mV; 4/16/64/
256 rows for dimmer readings, a guess that degrades to the ~4 s AEC path, never to white).

### Shipped default (manual-first, `settle 16 4500 4`), fw 0.27.0

| run | first served frame | then | `cam stat` | heap |
|---|---|---|---|---|
| 12:07 open after the OTA reboot (632 mV, exp class 16 = 1 row) | +3.4 s **161 / 1.9 %** (`X-Settle-Ms` 1264, gate `s@1950 ms` = size-stable at the N/2 floor) | 161 / 1.9 % flat to +8.5 s | `scene day (auto, 632 mV, exp 16) ... ircut day` | 87.7 KB |
| 12:07 `cam close` + reopen (647 mV) | +3.9 s 161 / 2.4 % (settle 1960) | flat | same | 87.1 KB |
| 12:42:32 **RTC deep-sleep wake** (`reset deepsleep-rtc`, 642 mV, overcast by now) | +3.4 s **132 / 0.9 %** (settle 1266, `s@1950 ms`) | flat; HA's own `wake_latest.jpg` written 4 s after the wake: 132 / 0.9 % (this morning: 250 / 91 %) | `scene day (auto, 642 mV, exp 16) ... ircut day`, trace 1 row then gain 1 -> 3 | 87.4 KB |
| 12:59:33 RTC deep-sleep wake (633 mV) | +3.4 s **159 / 0.1 %** (settle 1254; HA's request took the gate first, `wait 460 ms x2`) | flat; HA `wake_latest.jpg` 159 / 0.1 % | same, trace 1 row / gain 0 | 87.3 KB |

CPU1 trace for both: 1 row with AEC off, AEC on after 1 s, 1 row / gain 0 from then on - the
engine does not restart when enabled on a correctly exposed stream. The first frame itself
(~1 s after the open) is already right; what HA sees is bounded by the gate's N/2 floor (8 frames,
~1.9 s) and the open (~1 s), i.e. about 3 s after its request, inside the target.

## Debug: reading CPU1 counters from CPU0

`mem HEXADDR [N]` reads raw 32-bit words. CPU1's `media_debug_t` counters are reachable from CPU0
through the `transfer_log` pointer in the CPU0 map: `grep transfer_log $OUT/doorbell/bk7258/app.map`,
then `mem <that address> 1` gives the struct address; word 0 = `isr_jpeg | isr_h264 << 16`, word 4
high half = `fps_wifi`, words 6/7 = last JPEG/H.264 length. A bad address faults, so this is a
bench tool only.
