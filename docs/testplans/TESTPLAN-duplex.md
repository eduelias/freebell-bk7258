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

## Results

(filled in below after the device run)
