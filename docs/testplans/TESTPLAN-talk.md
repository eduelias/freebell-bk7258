# TESTPLAN — talk: two-way audio over HTTP (branch `feat/talk`, integrated as fw 0.23.0 / 0.23.1)

Half-duplex intercom on the existing camera HTTP server (port 8080). New file
`project/main/talk.c` (+ `talk.h`, `aud_engine.h`); hooks in `app_main.c`: an engine API block
(`aud_spk_open/write/close`, `aud_mic_open/read/close`, `app_reply`) after `cli_mic_cmd`, a
`talk_http_handle()` call at the top of `cam_http_client_task`, the `talk` command, `talk_init()`
in `user_app_main`, HTTP client thread stack 4096 -> 6144. Nothing in the camera path changed.

Routes (all plain HTTP on the LAN, device IP 192.168.2.89):

| Route | What |
|---|---|
| `POST /speak` | body = raw s16le mono 8 kHz **or** a WAV (PCM 16-bit, 1-2 ch, any rate >= 8 kHz: 16/44.1/48 kHz are resampled on the device). Content-Length or chunked. Plays through the DAC + amplifier, replies **after playback** with JSON `{"ok":true,"ms":...}`. Max `talk max` seconds (default 30), the rest is discarded. |
| `GET /speak?tone=HZ&ms=MS` | test tone (50 Hz..4 kHz, -12 dBFS like `spk tone`) |
| `GET /speak?url=http://IP[:port]/file.wav` | the device fetches the file (IPv4 literal only, no DNS) and plays it. This is the Home Assistant path: HA writes a WAV under `/config/www/doorbell/`, the device pulls it from `http://192.168.2.80:8123/local/doorbell/...`. |
| `GET /mic.wav` | live WAV, 16 kHz mono s16le, chunked (HTTP/1.1) or until-close (HTTP/1.0), until the client disconnects |
| `GET /mic.wav?seconds=N` | finite WAV with a correct Content-Length (N <= 600) |
| `GET /talk` | tiny browser page: listen, tone buttons, upload a WAV, play a URL |

Half duplex: whenever the speaker is in use (talk, the ring chime, `spk ...`), `/mic.wav` emits
silence instead of mic samples. One speaker user and one mic listener at a time; a second gets
**503**. `/stream` and `/snapshot.jpg` are untouched and work concurrently.

Commands (`python3 tools/dbcmd.py ...`), all KV-persisted with safe defaults:

| Command | Default | Meaning |
|---|---|---|
| `talk status` | | state (idle/speaking/listening), counters, last HTTP status, settings |
| `talk gain N\|auto` | `auto` (= `spk cfg gain`, 0x1c) | DAC gain for /speak only (`db_talk_gain`, 0 = auto). Above 0x1c the amplifier clips. |
| `talk micgain N` | `0x3f` | ADC gain for /mic.wav (`db_talk_micgain`); 0 = follow `mic cfg gain` |
| `talk max S` | `30` (1..120) | max seconds per /speak (`db_talk_max`) |
| `talk stop` | | abort the current /speak and /mic.wav (also `spk stop` aborts a /speak) |

## 0. Pre-flight (read-only)

```bash
cd ~/reps/home/iot/wt-talk          # or the integrated checkout
python3 tools/dbcmd.py talk status  # expect: "[talk] idle | speak: 0 calls ... | gain auto=0x1c micgain 0x3f max 30 s"
curl -s http://192.168.2.89:8080/   # the camera index still answers
```

UART (if the FT232 is back): at boot a line `[talk] http://IP:8080/speak /mic.wav /talk: gain auto=0x1c micgain 0x3f max 30 s`.

## 1. Speaker: tone (device only, no files)

```bash
curl -s 'http://192.168.2.89:8080/speak?tone=1000&ms=700'
# -> after ~0.8 s: {"ok":true,"source":"tone","ms":700,"in_bytes":0,"in_rate":8000,"dma_frames":12,"underruns":0}
```
Observe: a clean 1 kHz beep from the speaker; **no loud click** when the amp switches (the DAC
starts on zeros, the PA comes on 20 ms later and goes off before the DAC stops). Laptop-mic check:
record with `ffmpeg -f avfoundation -i ':1' -t 3 -ac 1 -ar 16000 tone.wav` in Terminal.app and run
`tools/mac_ear.py tone.wav` (expect a 1 kHz dominant, like `spk tone`).
UART: `[talk] tone 1000 Hz: 5600 samples`.
Then `python3 tools/dbcmd.py talk status` -> `speak: 1 calls, last 700 ms`.

## 2. Speaker: POST a WAV / raw PCM

```bash
# 16 kHz WAV from any source (ffmpeg -ar 16000 -ac 1), or 8 kHz raw:
say -o hello.aiff "Hello, this is the doorbell"            # macOS TTS
ffmpeg -y -loglevel error -i hello.aiff -ac 1 -ar 16000 -f wav hello16k.wav
curl -s -m 60 --data-binary @hello16k.wav -H 'Content-Type: audio/wav' http://192.168.2.89:8080/speak
# -> {"ok":true,"source":"post","ms":1900,"in_bytes":60844,"in_rate":16000,...}  (replies when playback ends)

ffmpeg -y -loglevel error -i hello.aiff -ac 1 -ar 8000 -f s16le hello8k.raw
curl -s --data-binary @hello8k.raw http://192.168.2.89:8080/speak      # raw path, same result

# chunked upload (what `ffmpeg ... | curl --data-binary @-` does):
ffmpeg -loglevel error -i hello.aiff -ac 1 -ar 16000 -f wav - | curl -s --data-binary @- http://192.168.2.89:8080/speak

# 48 kHz stereo straight from a music file: resampled on the device (box average), speech is fine
ffmpeg -y -loglevel error -i hello.aiff -ac 2 -ar 48000 -f wav hello48.wav
curl -s --data-binary @hello48.wav http://192.168.2.89:8080/speak      # in_rate 48000

# rejected input: 8-bit or non-PCM WAV, or < 8 kHz -> 415 with a hint
ffmpeg -y -loglevel error -i hello.aiff -ac 1 -ar 8000 -acodec pcm_u8 bad.wav
curl -s -w ' [%{http_code}]\n' --data-binary @bad.wav http://192.168.2.89:8080/speak   # [415]
```
Expect intelligible speech at the `spk cfg gain` loudness. `underruns` should be 0 (a few are
harmless: Wi-Fi hiccups during a 16 KB ring refill). UART: `[talk] speak: 60844 body bytes (plain,
16000 Hz x1) -> 15200 samples = 1900 ms in 1950 ms`.

Limit: `python3 tools/dbcmd.py talk max 3`, POST a 10 s clip -> plays 3 s, JSON `"ms":3000`, UART
says `truncated at max`. Restore: `talk max 30`.

Busy: start a 10 s POST, then `curl 'http://192.168.2.89:8080/speak?tone=1000&ms=200'` ->
`{"ok":false,...,"status":503,...,"hint":"speaker busy"}`. Press the doorbell button during a /speak:
the blue ring blinks but the chime is skipped (`ringfx_task` sees `s_spk_busy`) — expected.

Abort: during a long POST run `python3 tools/dbcmd.py talk stop` -> playback stops within ~1 s
(the ring drains), the POST gets a 200 with the shorter `ms`.

## 3. Speaker: pull from a URL (the HA path)

```bash
# put a file on HA (from the bench host): scp hello16k.wav root@192.168.2.80:/config/www/doorbell/say.wav
curl -s -m 60 'http://192.168.2.89:8080/speak?url=http://192.168.2.80:8123/local/doorbell/say.wav'
# -> {"ok":true,"source":"url","ms":1900,...}
curl -s 'http://192.168.2.89:8080/speak?url=http://192.168.2.80:8123/local/doorbell/nope.wav'
# -> {"ok":false,"source":"url","status":502,...,"hint":"fetch failed"}; UART: "[talk] url: ... -> HTTP 404"
```
HA's `/local/` needs no token. Only `http://` with an IPv4 literal.

## 4. Microphone

```bash
# finite file, then look at it on the workstation
curl -s -m 20 -o mic5.wav 'http://192.168.2.89:8080/mic.wav?seconds=5'   # ~5 s, 160044 bytes
ffprobe mic5.wav                      # pcm_s16le, 16000 Hz, mono, 5.00 s
python3 -c "import wave;w=wave.open('mic5.wav');print(w.getframerate(),w.getnframes()/w.getframerate())"
afplay mic5.wav                        # talk near the doorbell while recording, hear yourself
```
Tone check with the laptop as the voice: `afplay` a 1 kHz tone (or `tools/mac_ear.py` rig) near the
device during the 5 s, then `python3 tools/mic_raw.py`-style analysis or
`ffmpeg -i mic5.wav -af "astats" -f null -` — expect the same quality as `mic rec` (pure sine,
floor rms ~90 at gain 0x3f).

Live listening:
```bash
ffplay -nodisp -autoexit 'http://192.168.2.89:8080/mic.wav'         # ~0.5-1 s latency
vlc 'http://192.168.2.89:8080/mic.wav'                               # VLC: Media > Open Network Stream
open 'http://192.168.2.89:8080/talk'                                 # browser: press play on the audio element
```
Stop the client -> within 5 s UART prints `[talk] mic.wav: N bytes in P pieces (0 muted) over T ms,
F dma frames, 0 overruns` and `talk status` goes back to `idle`. A second listener while one is
active gets `503 microphone busy` (Chrome may open two connections for one `<audio>`; if the page
player stalls, use VLC/ffplay — this is a browser quirk, not a device fault).

## 5. Half duplex

```bash
ffplay -nodisp 'http://192.168.2.89:8080/mic.wav' &          # listener running
curl -s 'http://192.168.2.89:8080/speak?tone=1000&ms=2000'   # 2 s tone
```
Expect: the listener goes **silent** during the tone (no feedback/echo), audio resumes after. UART
line of the mic stream shows `(N muted)` > 0. Also press the doorbell button: the chime is muted
in the stream the same way. Note: the DAC opens with the XTAL clock while the ADC runs on APLL
(same as `spk`/`mic` today); the ADC clock may switch for the duration — harmless while muted, and
the measured 2x rate holds on both clocks. If the mic stream sounds wrong **after** a /speak, that
is the thing to look at (`mic cfg clk xtal` makes both sides use the same clock).

## 6. Camera unaffected

While a `/mic.wav` listener and a `/speak` run: `curl -s -o snap.jpg http://192.168.2.89:8080/snapshot.jpg`
and HA's camera card keep working (client limit is 6; talk uses one slot per stream).

## 7. Settings persist

```bash
python3 tools/dbcmd.py talk gain 0x18     # [talk] speaker gain 0x18
python3 tools/dbcmd.py talk micgain 0x30
python3 tools/dbcmd.py talk max 20
python3 tools/dbcmd.py reboot             # (or wait for the next OTA)
python3 tools/dbcmd.py talk status        # shows gain 0x18 micgain 0x30 max 20 s
python3 tools/dbcmd.py talk gain auto; python3 tools/dbcmd.py talk micgain 0x3f; python3 tools/dbcmd.py talk max 30
```

## Home Assistant integration (ready to apply)

All files on the HA box (`ssh root@<ha-ip>`).
After editing YAML: `ha core check`, then **Developer tools > YAML > reload** "Scripts",
"Shell commands" and "REST commands" (rest_command/shell_command reload exists in 2026.x; a core
restart also works).

### A. `/config/packages/doorbell_bk7258.yaml` — append

```yaml
rest_command:
  doorbell_speak_url:
    # the device pulls the WAV itself; replies after playback, so allow the clip length
    url: "http://192.168.2.89:8080/speak?url={{ url | urlencode }}"
    method: get
    timeout: 60
  doorbell_tone:
    url: "http://192.168.2.89:8080/speak?tone={{ hz | default(1000) }}&ms={{ ms | default(500) }}"
    method: get
    timeout: 20

shell_command:
  # message -> TTS mp3 (HA's own TTS API) -> 16 kHz mono WAV in /config/www/doorbell/<name>.wav
  doorbell_tts_wav: >-
    python3 /config/doorbell_tts_wav.py "{{ message }}" "{{ lang | default('en') }}" "{{ name | default('say') }}"

script:
  doorbell_say:
    alias: "Doorbell: say (TTS through the doorbell speaker)"
    mode: queued
    fields:
      message: {description: "Text to speak", example: "Please leave the parcel at the door", required: true}
      lang: {description: "TTS language", example: "nl", default: "en"}
    sequence:
      - service: shell_command.doorbell_tts_wav
        data: {message: "{{ message }}", lang: "{{ lang | default('en') }}", name: "say"}
      - service: rest_command.doorbell_speak_url
        data: {url: "http://192.168.2.80:8123/local/doorbell/say.wav"}

  doorbell_play:
    alias: "Doorbell: play a prerecorded WAV"
    fields:
      file: {description: "file name under /config/www/doorbell/", example: "leave_parcel.wav", required: true}
    sequence:
      - service: rest_command.doorbell_speak_url
        data: {url: "http://192.168.2.80:8123/local/doorbell/{{ file }}"}
```

Prerecorded messages need no TTS at all: convert on the workstation with
`ffmpeg -i in.mp3 -ac 1 -ar 16000 -f wav leave_parcel.wav`, copy to `/config/www/doorbell/`, call
`script.doorbell_play` with `file: leave_parcel.wav`.

### B. `/config/doorbell_tts_wav.py` (new, `chmod 755`)

Uses HA's `POST /api/tts_get_url` (needs a long-lived token: copy a long-lived token to
`/config/.doorbell_ha_token`, `chmod 600`) and the `ffmpeg` that ships in the HA core container.
Check the TTS entity id first (Developer tools > States, filter `tts.`): the default below is the
Google Translate entity; set `TTS_ENTITY` accordingly.

```python
#!/usr/bin/env python3
"""doorbell_tts_wav.py MESSAGE [LANG] [NAME]: HA TTS -> /config/www/doorbell/NAME.wav (16 kHz mono s16le)."""
import json, os, subprocess, sys, urllib.request
TTS_ENTITY = os.environ.get("DOORBELL_TTS_ENTITY", "tts.google_translate_en_com")
msg = sys.argv[1]; lang = sys.argv[2] if len(sys.argv) > 2 else "en"; name = sys.argv[3] if len(sys.argv) > 3 else "say"
token = open("/config/.doorbell_ha_token").read().strip()
req = urllib.request.Request("http://localhost:8123/api/tts_get_url",
    data=json.dumps({"engine_id": TTS_ENTITY, "message": msg, "language": lang}).encode(),
    headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
path = json.load(urllib.request.urlopen(req, timeout=30))["path"]   # "/api/tts_proxy/<hash>.mp3"
mp3 = urllib.request.urlopen("http://localhost:8123" + path, timeout=30).read()
os.makedirs("/config/www/doorbell", exist_ok=True)
out = f"/config/www/doorbell/{name}.wav"
subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", "pipe:0", "-ac", "1", "-ar", "16000", "-f", "wav", out],
               input=mp3, check=True)
print(out, os.path.getsize(out))
```

Test from the HA box: `python3 /config/doorbell_tts_wav.py "Hello from Home Assistant" en` ->
prints the path and size; then
`curl -s 'http://192.168.2.89:8080/speak?url=http://192.168.2.80:8123/local/doorbell/say.wav'`.
Then from HA: Developer tools > Actions > `script.doorbell_say` with `message: Hello`.

### C. Listening from HA

The camera entity `camera.doorbell_bk7258` has no audio (MJPEG). For listening use
`http://192.168.2.89:8080/mic.wav` in VLC / a browser, or a Lovelace **iframe/webpage card** pointing
at `http://192.168.2.89:8080/talk` (plain HTTP: open HA over http, or allow mixed content). A
`media_player.play_media` with that URL on a Chromecast/Sonos also works for a few seconds of
listening (they buffer, expect 2-5 s latency; stop it to free the mic for the next client).

Optional: add buttons to the mobile dashboard:
```yaml
- type: button
  name: "Doorbell: leave parcel"
  tap_action: {action: call-service, service: script.doorbell_play, data: {file: leave_parcel.wav}}
- type: button
  name: "Doorbell: beep"
  tap_action: {action: call-service, service: rest_command.doorbell_tone}
```

## Rollback

- Firmware: OTA the previous image (`doorbell-fw_0.18.5_app_pack.rbl`); talk keeps no state
  besides the three KV keys (`db_talk_gain`, `db_talk_micgain`, `db_talk_max`), harmless if left.
- Nothing starts on boot: no listener threads, no audio hardware touched until a request arrives.
- HA: remove the appended block from the package, delete `/config/doorbell_tts_wav.py` and
  `/config/.doorbell_ha_token`, reload.

## Known limits / notes for the integrator

- /speak replies after playback (the body is consumed at real-time speed through the 16 KB ring),
  so HTTP clients need a timeout >= the clip length (rest_command `timeout: 60`).
- Mono only on output (left channel of stereo input), box-average resampling: fine for speech,
  not hi-fi. Max 120 s per clip (`talk max`), no DNS in `url=`.
- A `/speak` while the ring chime plays returns 503 (chime = ~4 s); the chime is skipped if a
  /speak is running. The engine's mutual exclusion is `s_spk_busy`, unchanged.
- Each HTTP client thread grew from 4 KB to 6 KB stack (deepest path: url fetch + WAV parser +
  lwIP send inside the client thread); 6 clients max = 36 KB worst case.
- Host-side test harness (the real `talk.c` compiled against libc with a fake engine) passed 22
  cases: 8/16/44.1/48 kHz WAVs, stereo, LIST chunks, raw, chunked uploads, 100-continue, 415,
  max truncation, tone, finite/infinite/HTTP-1.0 mic, half-duplex mute, busy 503 both ways, url
  200/404, 431. Not yet run on the device (rule: no device state changes from this branch).

## Results 2026-10-03 (integrator, device at 192.168.2.89, fw 0.23.0 then 0.23.1)

Merge `feat/talk` -> main as v0.23.0 (6ccbeed; conflicts in both CMakeLists, includes, FW_VERSION,
the HTTP hook ahead of `cam_want_jpeg()`, 6 KB client stack + `power_activity()`), build 99 s clean,
OTA 21:43:46 -> back ~21:44:30. UART0 was dead all along (FTDI replays a frozen buffer), so UART
lines were not checked; everything below is HTTP/`talk status`/laptop-mic/bench-host evidence.

| Step | Result |
|---|---|
| 0 pre-flight | `talk status` idle, gain auto=0x1c micgain 0x3f max 30 s; index answers once the camera is open |
| 1 tone | `{"ok":true,"ms":700,"dma_frames":13,"underruns":0}` in 0.97 s; laptop recording: both bursts a clean 991-1000 Hz dominant, silence floor rms 6, no transient at PA on/off |
| 2 POST | 16 k WAV, 8 k raw, chunked 16 k, 48 k stereo: all `ms 4239` for the 4.24 s clip, 0 underruns, `in_rate` right; u8 WAV -> 415 with the hint. Pitch check: a 400..3500 Hz tone ladder POSTed at 16 k mono / 48 k stereo / 8 k came out identical (1195, 1600, 2006, 2389, 3001, 3513 Hz in order; the bare speaker + Mac mic lose the 400/800 steps) |
| 2 limit / busy / abort | `talk max 3` + 10.5 s clip -> `ms:3000` (3.2 s); tone during a POST -> 503 `speaker busy`; `talk stop` 3 s into a 10.5 s POST -> POST 200 after 4.3 s |
| 3 url | `/speak?url=http://192.168.2.80:8123/local/doorbell/test_hello.wav` -> 200 `ms 4239 in_rate 16000`; missing file -> 502 `fetch failed` in 27 ms |
| 4 mic | `?seconds=5`: 160044 B, pcm_s16le 16000 Hz mono, exactly 5.000 s; a 1 kHz tone from the laptop is the dominant in every 250 ms bin, floor rms ~50, DC ~650. **Found**: the first ~170 ms were a full-scale pop (60 ms at +32767, then a DC settle) -> v0.23.1 sends the first 200 ms as silence (f4e7326); verified: 0-200 ms all zero, then a ~100-peak tail. Live: ffmpeg client 4 s of the chunked stream OK, HTTP/1.0 (`curl -0`) OK, `talk status` idle right after the client leaves; second listener -> 503 `microphone busy` |
| 5 half duplex | bench-host `curl` listener for 16 s: the laptop's 1 kHz tone is heard (8.75-13.0 s of stream), then pure zeros while the device played its own 2 s tone; no echo of the device tone in the stream |
| 6 camera | snapshot 200 (34 KB) in 79 ms during the listener + tone; after everything RTSP `ffprobe` h264 1280x720 and snapshot still fine |
| 7 persist | gain 0x18 / micgain 0x30 / max 20 survived the 0.23.1 OTA reboot; restored to auto / 0x3f / 30 |
| HA | package appended (backup `.bak-20261003-talk`), `doorbell_tts_wav.py` + token file installed, `ha core check` OK, rest_command/script reload OK, `shell_command` has no reload service -> `ha core restart` (API back < 10 s). `shell_command.doorbell_tts_wav` with `return_response`: `say.wav 162894 B` = 16 kHz mono 5.09 s; `script.doorbell_say` 200 in 3.2 s, `rest_command.doorbell_tone` 200, `script.doorbell_play test_hello.wav` 200 in 4.5 s; the device counted the three `/speak` calls |

Not done: listening by a human ear (quality judgement), the ring-button interplay (needs a press),
UART log lines. Heap 115.5 KB (0.22.3) -> 111 KB (0.23.1) idle.
