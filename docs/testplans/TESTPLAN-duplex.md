# TESTPLAN — duplex: full-duplex two-way audio over RTSP (branch `feat/duplex-audio`, fw 0.26.0)

Two-way talk with echo cancellation, the way a commercial doorbell does it, instead of the
half-duplex HTTP intercom of 0.23: the RTSP stream gains a G.711 microphone track and an ONVIF
backchannel track into the speaker, so go2rtc (Home Assistant's streaming engine) gives two-way
audio without any custom glue. New file `project/main/audio_voc.c` (+ `audio_voc.h`); `rtsp.c`
grew from one to three tracks; `app_main.c` arbitrates the two audio engines; `talk.c` got the
`talk aec|vocmic|voc` commands; `tools/rtsp_backchannel.py` is the host-side test client.

## What was built

| Piece | What |
|---|---|
| voc engine (`audio_voc.c`) | Beken's `aud_intf` **voice mode**: ADC/DAC, AEC (reference = decoded speaker PCM), NS and G.711 run on **CPU1** (`components/multimedia/aud/aud_tras_drv.c`), CPU0 drives it over the media mailbox. 8 kHz, G.711 A-law, 20 ms frames; mic frames reach CPU0 in 320-byte (40 ms) callbacks on the SDK task `aud_tras`; speaker frames go into a 10 x 320 B ring (prefilled with 4 frames = 160 ms of silence at start). PA (GPIO 46) switched from CPU0 50 ms after start / before stop; the SDK's own PA control is off for CPU1 (it would drive GPIO 5, a camera rail). |
| arbitration | one engine owns the hardware: `voc_start` refuses while the raw engine is busy (chime, `spk`, `mic`, /speak, /mic.wav on the raw path), the raw opens refuse while voc is starting/stopping. **While voc runs, the raw users go through taps**: /speak, `spk tone|play|net` and the ring chime -> voc speaker tap (PCM -> A-law, paced to real time, backchannel frames dropped meanwhile); /mic.wav -> voc mic tap (decoded, **8 kHz** WAV instead of 16 kHz). |
| RTSP | SDP: `track0` H264/90000, `track1` PCMA/8000 `a=recvonly` (mic), `track2` PCMA/8000 `a=sendonly` (backchannel) only when the DESCRIBE has `Require: www.onvif.org/ver20/backchannel`. Per-track transport (TCP interleaved or UDP), aggregate PLAY/PAUSE, `RTP-Info` per track. Mic RTP: one 160-byte packet per 20 ms, PT 8, 8 kHz clock. Backchannel RTP (interleaved `$` frames on its channel, or its UDP server port) -> `voc_feed_spk`. The voc engine starts when the first client sets up an audio track (retried every 500 ms while the chime holds the hardware), stops when the last one leaves. `rtsp status` has an audio line and per-track client state. |
| commands | `talk aec on|off` (KV `db_talk_aec`, default on, live), `talk vocmic N` (KV `db_voc_mic`, default 0x3f, live), `talk voc on|off` (manual engine for bench tests), `talk status` shows `engine raw|voc` + the `[voc]` line. |
| sleep | a connected RTSP client and a running voc engine count as busy (`app_power_busy`). |
| sim | `talk aec|vocmic|voc`, `rtsp status` audio line, the ffmpeg stand-in advertises a PCMA track. |

Why PCMA at 8 kHz: RTP payload type 8 needs no negotiation, the SDK's AEC frame is 20 ms at 8 or
16 kHz, G.711 is what go2rtc and browsers (WebRTC PCMA) speak natively, and the doorbell speaker
has no bandwidth above 4 kHz anyway.

Why `a=recvonly` on the mic track and not `a=sendonly`: RTSP SDP directions are written from the
client's point of view. go2rtc (`pkg/rtsp/helpers.go` UnmarshalSDP) treats a media without a
direction as "I receive" and a media with `a=sendonly` as the one it sends into (the backchannel).
Marking the mic track `sendonly` would make go2rtc try to talk into it.

## Test plan (bench host = du7, `192.168.2.68`; device `192.168.2.89`; HA `192.168.2.80`)

The device is mounted outside on battery with deep sleep on: it is awake ~3 min after each RTC
wake (every 15 min) and after a button/PIR event. Every test below was run inside such a window;
speaker tests kept to <= 3 s.

1. **Build + sim**: `-Werror` build of the AVDK image; `pytest sim/tests` green (contract test:
   every firmware command has a simulator handler; `rtsp status` / `talk status` reply formats).
2. **OTA** `scripts/release.sh --no-build` in the first minute of a window; `sys info` = 0.26.0;
   heap; `talk status` shows `engine raw`, `[voc] off aec on`.
3. **SDP**: `ffprobe rtsp://IP:554/live` -> h264 + pcm_alaw; `tools/rtsp_backchannel.py --describe`
   (with the Require header) -> three medias with the right directions; `--no-require` -> two.
4. **Mic track**: `ffmpeg -rtsp_transport tcp -i rtsp://IP:554/live -t 10 -vn out.wav` records
   10 s of 8 kHz A-law -> WAV; noise floor; `rtsp status` shows `voc on (rtsp)`, `clients 1`, mic
   frames; the camera keeps streaming (video packets, `cam stat`).
5. **Echo path exists**: while recording the mic track, `GET /speak?tone=1000&ms=1500` (goes
   through the voc speaker tap): the tone is in the recording.
6. **AEC**: `tools/rtsp_backchannel.py --tone 1000 --seconds 3 --record X.wav --analyze 1000` with
   `talk aec off` and with `talk aec on`: Goertzel level of 1 kHz in the recording during the tone
   window; attenuation = difference; target >= 15 dB.
7. **Arbitration**: `ring test` during a session (chime through the tap, heard in the mic track,
   `[spk] done via voc tap`); `/speak` 503 only when a tap is already open; `/mic.wav` during a
   session = 8 kHz WAV; `talk voc off` while a client is connected -> restarted within 0.5 s.
8. **Everything else still works**: `/snapshot.jpg` 200 during a session; `hook test`; TCP
   commands; heap before/during/after; the device goes back to sleep after the client leaves
   (`pwr status` busy flag follows the RTSP client).
9. **Home Assistant**: go2rtc's view of the stream (producer with 3 tracks incl. the backchannel);
   HA frontend state (listen-only WebRTC player, no microphone button in 2026.9: see results);
   ring push with the `Answer` action.

## Results (2026-10-04, device at the front door on battery, bench host du7)

Builds: 4 (662 s full, ~100 s incremental), all clean under `-Werror`; `pytest sim/tests` 35 green.
OTAs: 2 (build 3 = c96e64b+merge, build 4 = buffer trims). Gate: device on 0.25.0 and a80e754 on
origin/main before the first OTA; origin/main merged into the branch first (conflicts in app_main.c,
rtsp.c, CMakeLists, docs; `vol` now also drives the voice engine's speaker gain live).

| # | Test | Result |
|---|---|---|
| 2 | OTA, boot | 0.26.0 up 0 s after the download; heap at boot **86.9 KB** (build 4; build 3 76.5 KB before the trims; 0.25.0 97.5 KB: the new statics are the 3.2 KB mic ring, 4 KB mic tap, 3 x 1.5 KB RTSP rx buffers, tap staging). `talk status`: `engine raw`, `[voc] off aec on mic 0x3f spk 0x1c`. |
| 3 | SDP | `ffprobe`: `h264 1280x720` + `pcm_alaw 8000`. `--describe` with Require: three medias `video (no direction) track0`, `audio recvonly PCMA/8000 track1`, `audio sendonly PCMA/8000 track2`; without Require: two. SDP excerpt below. ffprobe's audio SETUP alone started and stopped the voice engine (`starts 1`, 126 mic callbacks). |
| 4 | Mic track | `ffmpeg -rtsp_transport tcp ... -t 8 -vn mic_ffmpeg.wav` = 8.00 s of 8 kHz s16: floor **-38..-54 dBFS rms** (quiet street, evening); no packet loss in any run (`lost 0`); first mic RTP packet **225-230 ms after PLAY** (voc already running since SETUP; SDK prefill + first 40 ms callback). Video kept flowing (5-6 fps, 410-520 kbps) in every session. |
| 5 | Echo path | `/speak?tone=1000&ms=1500` during the ffmpeg recording went through the voc speaker tap (`tap 75 frames 0 late`, reply after 1.5 s); with the AEC on the tone is only -58 dBFS in the recording (the raw path would have been near full scale: see 6). |
| 6 | **AEC** | `rtsp_backchannel.py --tone 1000 --seconds 3 --record --analyze 1000`, tone at -12 dBFS through the backchannel, mic gain 0x3f, speaker gain 0x1c (`vol 100`). **AEC off: 1 kHz at -10.2 dBFS, rms -11.1 dBFS** (the speaker is ~10 cm from the capsule: the echo is near full scale). **AEC on: 1 kHz at -100.6 dBFS (build 3) / -109.2 dBFS (build 4), rms -37.5 / -55.2 dBFS.** Attenuation at 1 kHz **> 90 dB (floor-limited)**, broadband **26-44 dB**. The SDK's canceller also gates the residual (the mic goes near-silent while the far end plays), so double-talk (a visitor speaking while the house talks) is untested until someone stands at the door. 150/150 backchannel frames arrived (`back in 150 pkts 24000 B`), `drop-tap 0 err 0`. |
| 7 | Arbitration | `ring test` during a session: chime through the tap (`tap 208 frames 0 late` = the whole 4.15 s bell), `[spk] done via voc tap`; `/mic.wav?seconds=2` during a session: 200, **8000 Hz** header, 32044 B; `spk tone 800 300` after the session: raw engine again (`engine raw`, `starts` unchanged). `/speak` during a session: 200 via the tap. Voice engine start/stop per session: `starts 5` over the run, `voc fails 0` (no chime collision happened; the 500 ms retry is untested). |
| 8 | Rest of the device | `/snapshot.jpg` during an audio-only session: **503 camera busy** (pre-existing: DESCRIBE puts the camera in H.264 and no JPEG had been taken since boot; HA keeps a stale JPEG otherwise). `hook test` ok; TCP commands ok throughout; heap **77.5-77.9 KB** (build 3) / **84.9-88.0 KB** (build 4) idle and during sessions, stable over ~10 min; `pwr status` `busy 0` after the clients left, `sleep on`, the device went back to its RTC cycle. Cold start note: right after an OTA reboot the first H.264 open took > 3 s to deliver a frame (`[cam] h264: no frame within 3 s`, pre-existing open timeout), the DESCRIBE 1-2 s later succeeded. |
| 9 | **go2rtc / HA** | HA 2026.9.4 embedded go2rtc 1.9.14 (debug UI enabled temporarily): stream `camera.doorbell_bk7258` producer `rtsp://192.168.2.89:554/live` -> medias **`video, recvonly, H264`**, **`audio, recvonly, PCMA/8000`**, **`audio, sendonly, PCMA/8000`**: go2rtc recognised the backchannel as its audio sink without any option. HA frontend: the WebRTC player is listen-only (`addTransceiver("audio", {direction: "recvonly"})`), **no microphone button exists in 2026.9**; talking back needs go2rtc's `webrtc.html` (`media=video+audio+microphone`) or the WebRTC custom card. Ring push: `Answer` action + `clickAction: entityId:camera.doorbell_bk7258`, `importance: high` added to the live automation (`ha core check` ok, reloaded). |

SDP as served (DESCRIBE with the Require header):

```
v=0
o=- 12213 1 IN IP4 0.0.0.0
s=doorbell-bk7258
t=0 0
a=control:*
a=range:npt=now-
m=video 0 RTP/AVP 96
c=IN IP4 0.0.0.0
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1;profile-level-id=420029;sprop-parameter-sets=Z0IAKeNQCgC2QgAAB9AAAMAA0YAAmJgAL/L3gAE=,aM48gA==
a=framerate:15
a=control:track0
m=audio 0 RTP/AVP 8
c=IN IP4 0.0.0.0
a=rtpmap:8 PCMA/8000
a=ptime:20
a=recvonly
a=control:track1
m=audio 0 RTP/AVP 8
c=IN IP4 0.0.0.0
a=rtpmap:8 PCMA/8000
a=ptime:20
a=sendonly
a=control:track2
```

Open items: double-talk quality and the AEC parameters (`ec_depth`, `TxRxThr`) with a person at the
door; UDP transport for the audio tracks (code path present, untested; go2rtc and HA use TCP);
the 500 ms voc retry while the chime holds the hardware; the ffmpeg 8 stand-in in the simulator.
