# Test plan: H.264 + RTSP live stream (feat/rtsp, fw 0.19.0)

Branch `feat/rtsp`, merged into main 2026-10-03 as v0.22.0 (fixes v0.22.1/0.22.2). **Results at the
end of this file.**

## What changed

- New `project/main/rtsp.c` + `rtsp.h`: our own RTSP/1.0 server (OPTIONS, DESCRIBE, SETUP, PLAY,
  PAUSE, TEARDOWN, GET/SET_PARAMETER), RTP/H.264 per RFC 6184 (single NAL + FU-A, packetization-
  mode 1, 90 kHz clock from the CPU1 frame timestamp), transports `RTP/AVP/TCP` (interleaved) and
  `RTP/AVP` (UDP unicast, server ports 6970+). URL `rtsp://192.168.2.89:554/live` (any path is
  accepted). Up to 3 clients, each with its own sender thread fed from a 128 KB PSRAM ring; a slow
  client only hurts itself (it resyncs at the next IDR). SEI/AUD NALs are not forwarded.
- Camera (app_main.c): the DVP pipeline runs ONE hardware encoder at a time, so the camera now has
  a mode: JPEG (HTTP snapshot/MJPEG, as before) or H.264 (RTSP). `cam frame_cb` dispatches on
  `frame->fmt`. Policy = `cam mode` (KV `db_cam_mode`, default `auto`): RTSP clients switch the
  camera to H.264 on demand; an HTTP request switches back to JPEG unless an RTSP client is playing
  (then `/snapshot.jpg` returns the last JPEG taken before the switch and `/stream` answers 503
  `camera busy (H.264/RTSP)`). `jpeg` = never H.264 (DESCRIBE -> 503). `h264` = never JPEG.
- CPU1 encoder config (`project-avdk/config/bk7258_cp1/config`, mirrored in cp0):
  `CONFIG_H264_P_FRAME_CNT=29` (IDR + SPS/PPS every 30 frames = 2 s at 15 fps),
  `CONFIG_H264_ADD_SELF_DEFINE_SEI=n` (no 96-byte private SEI per frame),
  `CONFIG_H264_FRAME_SIZE=131072` (720p I-frames fit). **Kconfig change: delete
  `$OUT/du7/bk7258_cp1`, `_cp2` and `bk7258` before the first build of this branch in an existing
  OUT dir** (same lesson as the partition CSVs; a fresh OUT is fine).
- Rate control: `rtsp q IMB PMB` (KV `db_h264_q`, default `120 24`) = target bits per 16x16
  macroblock for I and P frames, applied through `media_app_set_compression_ratio()` after the
  first frame of every H.264 open. 120/24 is ~1.5 Mbit/s nominal (54 KB I + 11 KB P), typically
  less on a static doorstep; the SDK's own preset (160/60) would be ~3.5 Mbit/s.
- `CAM_BUF_MAX` is now `CONFIG_JPEG_FRAME_SIZE` (150 KB) instead of 200 KB: the CPU0 PSRAM heap is
  512 KB and now holds the JPEG buffer, one 150 KB copy per HTTP client, the 128 KB ring and 8 KB
  per RTSP client.
- No change to reports/webhook JSON, audio, ring effects, OTA or partitions.

## Commands

| Command | Effect | Persisted |
|---|---|---|
| `rtsp on` | start the server on the configured port; clients may switch the camera to H.264 | KV `db_rtsp=1` (default off) |
| `rtsp off` | stop within ~1 s, drop clients, close the camera if it is in H.264 | KV `db_rtsp=0` |
| `rtsp status` | state, port, clients/playing, incoming fps + kbps, frames/keyframes, drops, SPS/PPS cached, last-frame age; one line per client (peer, tcp/udp, playing, frames, KB, lost, udp_err, age) | - |
| `rtsp port N` | listening port (554 default; 8554 if HA's go2rtc or anything else objects to a privileged-looking port - the device does not care) | KV `db_rtsp_port`, applies at next `rtsp on` |
| `rtsp q IMB PMB` | rate-control targets (10..290 / 4..1000); applied live if H.264 is running | KV `db_h264_q` |
| `rtsp idr` | force an IDR (also happens automatically on PLAY and after a drop) | - |
| `cam mode auto|jpeg|h264` | encoder policy (see above) | KV `db_cam_mode`, default auto |
| `cam open [W H] [h264]` | manual open in JPEG (default) or H.264 (switches if needed) | - |
| `cam close` / `cam stat` | close; `stat` now shows the open mode, policy and H.264 frame count | - |

All commands go through `~/.pyenv/versions/3.11.13/bin/python3 tools/dbcmd.py <cmd...>`.

## Step 0: flash and baseline (nothing enabled yet)

```bash
scripts/release.sh --no-build        # after a build of this branch; device reboots in ~35 s
python3 tools/dbcmd.py hook test     # fw "0.19.0" in the HA input_text helper
python3 tools/dbcmd.py rtsp status   # expect: "[rtsp] stopped port 554 clients 0/3 ..."
python3 tools/dbcmd.py cam stat      # expect: open 0 - (mode auto) ...
curl -s -o /tmp/s.jpg -w '%{http_code} %{size_download}\n' http://192.168.2.89:8080/snapshot.jpg
```
Expected: 200 and a 30-90 KB JPEG, exactly as 0.18.x (RTSP is off by default; the only behavioural
change with RTSP off is the 150 KB buffer cap, which CPU1 can never exceed).

## Step 1: enable RTSP, probe with ffprobe from du7 (TCP)

```bash
python3 tools/dbcmd.py rtsp on
# on du7 (pi_exec host du7, or ssh esaleh@192.168.2.68):
ffprobe -v error -rtsp_transport tcp -show_streams rtsp://192.168.2.89:554/live 2>&1 | egrep 'codec_name|width|height|r_frame_rate|profile'
```
Expected: `codec_name=h264`, `width=1280`, `height=720`, frame rate ~15/1, profile Baseline/Main
(whatever the BK7258 encoder's SPS says; `profile_idc` 0x42/0x4d). The first DESCRIBE takes 1-3 s
(camera open + first IDR). UART (if alive) shows `[cam] open 1280x720 h264 -> 0`,
`[cam] h264 q imb 120 pmb 24 -> 0`, `[rtsp] #0 192.168.2.68 connected`, and on CPU1 the usual
`auto detect success, dvp camera name:ov9650`.

Then record 10 s and check continuity:
```bash
ffmpeg -v warning -rtsp_transport tcp -i rtsp://192.168.2.89:554/live -t 10 -c copy -y /tmp/db.mp4
ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames,avg_frame_rate /tmp/db.mp4
python3 tools/dbcmd.py rtsp status
```
Expected: ~150 frames, no `corrupt`/`missing picture` warnings beyond the first second; `rtsp
status` shows `in 15.0 fps`, kbps in the 600-1600 range, `drop busy 0 big 0`, `key` growing by one
every 2 s, the client line with `tcp playing`, `lost 0`. Copy `/tmp/db.mp4` to the Mac and play it
(or `ffplay -rtsp_transport tcp rtsp://192.168.2.89:554/live` on the Mac directly: it has ffmpeg).

## Step 2: UDP transport and multiple clients

```bash
# du7: UDP (ffmpeg's default tries UDP first)
ffmpeg -v warning -i rtsp://192.168.2.89:554/live -t 10 -c copy -y /tmp/db_udp.mp4
python3 tools/dbcmd.py rtsp status     # client line says "udp"; udp_err should stay small (< 1 %)
# Mac + du7 at the same time (2 clients), then a 3rd; a 4th gets "503 Service Unavailable"
```
Expected: both play; `clients 2/3 (2 playing)`. If UDP shows many `udp_err` or artefacts while TCP
is clean, note it and prefer TCP everywhere (HA/go2rtc uses TCP by default).

## Step 3: camera mode policy and HTTP coexistence

```bash
# while an RTSP client is playing:
curl -s -o /tmp/stale.jpg -w '%{http_code}\n' http://192.168.2.89:8080/snapshot.jpg   # 200 (stale JPEG) or 503 if none was ever taken
curl -s -w '%{http_code}\n' http://192.168.2.89:8080/stream | head -c 100                   # 503 camera busy
# stop all RTSP clients, wait 2 s:
curl -s -o /tmp/fresh.jpg -w '%{http_code} %{size_download}\n' http://192.168.2.89:8080/snapshot.jpg  # 200, ~1.5 s (switch back to JPEG)
python3 tools/dbcmd.py cam stat        # open 1 jpeg (mode auto)
# reconnect ffplay: switches back to H.264 within ~2 s
python3 tools/dbcmd.py cam mode h264   # then snapshot -> stale/503 even with no RTSP client
python3 tools/dbcmd.py cam mode jpeg   # then ffprobe -> "503 Service Unavailable" on DESCRIBE
python3 tools/dbcmd.py cam mode auto
```
The `ring` light-sensor reading is unaffected (GPIO 28 is high while the camera is open in either
mode; `light_read_mv()` only raises it itself when the camera is closed).

## Step 4: rate/quality tuning

```bash
python3 tools/dbcmd.py rtsp q 100 16     # ~1.0 Mbit/s nominal, applied live
python3 tools/dbcmd.py rtsp q 160 40     # richer; watch "big" drops in rtsp status (must stay 0)
python3 tools/dbcmd.py rtsp q 120 24     # default
```
Judge with ffplay on the Mac: blockiness on motion (wave a hand) vs `kbps` in `rtsp status`. Any
`drop big` means an access unit exceeded 96 KB: lower IMB. Any `drop busy` with one LAN client means
that client stalled (TCP backpressure): check Wi-Fi, not the device.

## Step 5: Home Assistant

Option A (recommended): stream-only generic camera, stills come from the stream (no mode flapping).
`tools/ha_generic_camera.py` (this branch) learned `options` (options flow of an existing entry),
an optional `--still` and `--rtsp-transport`. Copy it to du7 and run there (token in `~/.ha_token`):
```bash
scp tools/ha_generic_camera.py esaleh@192.168.2.68:~/ha_generic_camera.py
# on du7:
~/.venv-ha/bin/python ~/ha_generic_camera.py list                       # entry_id of "Doorbell BK7258"
~/.venv-ha/bin/python ~/ha_generic_camera.py options ENTRY_ID --stream rtsp://192.168.2.89:554/live --rtsp-transport tcp
# (if the options flow rejects an empty still URL, delete + add instead:)
~/.venv-ha/bin/python ~/ha_generic_camera.py delete ENTRY_ID
~/.venv-ha/bin/python ~/ha_generic_camera.py add --stream rtsp://192.168.2.89:554/live --rtsp-transport tcp
```
The script prints any unknown step's schema; the HA 2026.9 generic flow may want `rtsp_transport`
inside the nested `advanced` section instead of top level - adjust the `data` dict if it complains.
Same thing by hand: HA UI -> Settings -> Devices & services -> Generic Camera "Doorbell BK7258" ->
Configure: Still image URL empty, Stream source `rtsp://192.168.2.89:554/live`, RTSP transport tcp.
Option B (keep both URLs): still `http://192.168.2.89:8080/snapshot.jpg`, stream the RTSP URL. Works
with `cam mode auto`, but HA refreshes the still every 10 s, so when nobody is watching the camera
flips to JPEG and the first viewer waits ~2-3 s for H.264 again; while someone watches, the still
is the stale JPEG. Fine for a doorbell, just know why the picture looks old.

Verify: open the camera entity, click the live view (HA uses go2rtc / the stream integration,
TCP transport by default); expect video within ~3 s; `rtsp status` shows the HA host as a client.
Camera settings "Preload stream" keeps the RTSP session open permanently (camera then stays in
H.264; snapshots stale in option B; choose option A).

No YAML change is strictly required. If you prefer YAML over the UI entry, replace the generic
camera entry with (in `/config/configuration.yaml`, then `ha core check` + restart):
```yaml
camera:
  - platform: generic
    name: Doorbell BK7258
    stream_source: rtsp://192.168.2.89:554/live
    rtsp_transport: tcp
    verify_ssl: false
```
(and delete the UI entry so the entity id does not get a `_2` suffix). The webhook automation
`doorbell_bk7258_webhook` and `notify.doorbell_family` are untouched. A nice follow-up for the ring
branch: `camera.snapshot` after the notification (works from the stream in option A) or
`camera.record` for a 10 s clip.

## Step 6: soak and off

```bash
# leave HA (or ffplay) streaming for 30 min, then:
python3 tools/dbcmd.py rtsp status      # drop busy/big still 0, lost 0, no reboot (uptime via hook test)
python3 tools/dbcmd.py rtsp off         # server stops, camera closed; HA stream errors until re-enabled
curl -s -o /dev/null -w '%{http_code}\n' http://192.168.2.89:8080/snapshot.jpg   # 200 again (JPEG)
python3 tools/dbcmd.py rtsp on
```

## Rollback

- `rtsp off` + `cam mode jpeg` restores the 0.18.x behaviour at runtime (both persisted).
- Or OTA the previous image: `scripts/release.sh --no-build` after `ln -sf doorbell-fw_0.18.5_app_pack.rbl
  ~/data/doorbell-bk7258/firmware/doorbell-fw_latest_app_pack.rbl` (or point `ota` at it directly).
- KV keys this feature adds: `db_rtsp`, `db_rtsp_port`, `db_h264_q`, `db_cam_mode`; harmless if left.

## Known limits / things to watch

- One encoder at a time is a hardware pipeline fact (JPEG_MODE vs H264_MODE on the DVP path;
  the only dual-output modes add a raw YUV frame for an LCD, not a second compressed stream).
  A periodic JPEG while in H.264 would mean a close/open cycle (~1.5 s gap in the stream) per
  snapshot, so it is not done; HA should take stills from the stream.
- UDP: lwIP has 10 pbufs and a 50 KB heap; an IDR burst is paced (1 ms per 8 packets) and retried
  on ENOMEM, but packet loss is possible under Wi-Fi contention. TCP interleaved is the robust path.
- PSRAM budget: the 128 KB ring is allocated at the first `rtsp on` and kept until reboot (freeing
  it under a live producer is not worth the race). `rtsp on` prints `no PSRAM for the frame ring`
  if the heap is short; `[rtsp] client setup failed (memory)` for per-client buffers.
- Audio is not in the SDP (video only). A second `m=audio` track (G.711 from the MIC path) would be
  the next step; the ring/packetizer are format-agnostic enough to add it.
- The listener thread calls `media_app_h264_regenerate_idr()` (sync mailbox to CPU1) at most every
  500 ms; the transfer task callback never calls into media_app.
- UART shows `[cam] open ... h264`, `[cam] h264 q ...`, `[rtsp] #n ... connected/closed` lines;
  nothing new on CPU1 except the encoder's own prints.

## Results 2026-10-03 (integrator, fw 0.22.0 -> 0.22.2, dark room, UART dead)

- Build: the branch built clean at the first try after the merge (625 s full build; Kconfig picked
  up: `CONFIG_H264_P_FRAME_CNT=29`, `H264_ADD_SELF_DEFINE_SEI` off, `H264_FRAME_SIZE=131072` on
  both CPUs). OTA via `release.sh --no-build` three times (0.22.0 / 0.22.1 / 0.22.2), ~90 s each.
- Step 0: RTSP off by default, `cam stat` `open 0 - (mode auto)`, snapshot 200 / 74 KB / 1.6 s. OK.
- Step 1 (TCP, Mac and du7): ffprobe `h264 Baseline level 41 1280x720 yuv420p`, SPS/PPS cached
  (`sps 30 pps 4`), 10 s recordings play; but **~3 fps**, not 15: PTS spacing a steady 358-362 ms,
  `rtsp status in 2.9-3.0 fps`, 600 kbps. Same rate at 800x480 and in JPEG mode; CPU1 `media_debug`
  (`isr_jpeg == fps_wifi`) shows the encoder itself completes ~3 fps (5-8 fps right after a sensor
  power-on reset, ~9 fps in daylight per the 16:35 notes) => OV9650 night-mode frame-rate reduction,
  not the RTSP path. ffmpeg also warns `time_scale/num_units_in_tick invalid (0/1000)` (encoder VUI;
  cosmetic) and one `Non-monotonic DTS` for the IDR+first P pair at start.
- Step 2: UDP from du7 works (35 frames / 12 s) with `RTP: missed 6 packets`; TCP lost 0 always.
  Three concurrent TCP clients (`3/3 (3 playing)`, lost 0 each, 157/152/113 frames), a 4th gets 503.
- Step 3: during play snapshot 200 (stale JPEG, 47 ms), `/stream` 503; after the clients stop the
  camera is back in JPEG in 2.4 s (the first snapshot is still the old JPEG); `cam mode jpeg` ->
  DESCRIBE 503; `cam mode h264` -> snapshot stale, `/stream` 503; back to `auto`. OK.
- Step 4: `rtsp q 160 40` applied live, `drop big 0`; back to 120/24. OK.
- Step 5: HA entry first switched to stream-only (option A) with the fixed helper (`rtsp_transport`
  must sit inside `advanced`): `camera_proxy` still from the stream 200 / 115-144 KB in ~7 s twice,
  500 at the 10 s timeout twice (go2rtc/ffmpeg probing at 3 fps), `camera_proxy_stream` 0 bytes in
  15 s. **Final: option B** (still `/snapshot.jpg` + RTSP stream, TCP): still 2.4 s right after a
  stream (mode switch) then 0.17 s, ffprobe on the RTSP URL still fine. Live view by eye: owner.
- **Wedge**: the JPEG<->H.264 switch (close + immediate reopen) hit the pre-existing camera wedge
  (sensor silent, `open -> 0`, no frames) on the 8th switch; `cam probe rails off` + `on` cleared it
  without a reboot. v0.22.1 cuts GPIO 28/5/6 on every close (250 ms off / 60 ms settle: 7/8 ok ->
  3/8 bad in a cold-open loop), v0.22.2 uses 800 ms / 150 ms (7/8 ok) + stall self-heal + one retry
  in acquire: 6/6 DESCRIBEs with HA still-polls in between, the one silent open was repaired by the
  next DESCRIBE (7.8 s). Switch cost ~2.1 s.
- Step 6: 20 min TCP soak from du7 (`systemd-run --user --unit rtsp-soak`, 21:11-21:31): no reboot
  (uptime 1852 s), ~3600 frames, `drop busy 2 big 0`, lost 0, ffmpeg log empty, heap 109 K with the
  client and back to 115 K after it left (no leak). v0.22.3 (power off on a failed open) then 6/6
  cold opens, DESCRIBE ok, heap 115.5 K.
- Not done: UI live view by eye, audio track, daylight frame-rate check, `rtsp port` change.
