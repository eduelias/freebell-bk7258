# Audio: speaker, microphone, two-way talk

## Engine (v0.13, `aud_engine.h`)

One DMA engine feeds the on-chip DAC and drains the on-chip ADC; `spk`, `mic`, the ring chime and
`talk.c` share it through the `s_spk_busy` / `s_mic_busy` exclusion.

**Speaker**: DAC -> external class-D amplifier, enable = **GPIO 46** (active high, parked low at
boot). DMA feeds the DAC FIFO (16-bit mono writes) from a 16 KB ring; 0 underruns measured. The
amplifier clips hard at the SDK default DAC gain (0x2d = 0 dB): a 1 kHz sine came out as 3 kHz (the
3rd harmonic 13x the fundamental; the dangling speaker has no bass). The **default gain is 0x1c**,
the loudest level that stays clean; raise it with `spk cfg gain N` once the speaker is back in its
enclosure. GPIO 46 was found with a GPIO sweep while a microphone listened: a 1 kHz tone scored 108
with 46 high versus < 1 for every other pin; left floating the amplifier leaks a faint output.

**Microphone**: the electret capsule is MIC1 (left channel, low 16 bits of the FIFO word).
Quality is excellent: a 1 kHz tone from a laptop speaker is a pure sine (Goertzel/rms 0.71), floor
rms about 90 at gain 0x3f with the amplifier off. The ADC runs at **2x the configured rate** whatever
you configure (8000 -> 16 kHz, 16000 -> 32 kHz, APLL or XTAL): keep `rate 8000` and treat captures
as **16 kHz**. Use gain 0x3f for speech.

**Amplifier pop** (v0.19): the DAC + DMA start on zeros (buffer zeroed) 50 ms before the PA is
enabled, and the PA is disabled on zeros 50 ms before the DAC stops. The pop could not be measured
either way with a microphone 40 cm away (no transient above the room floor within 200 ms of PA
on/off on either build); by ear it is gone for tones, "possible" is still the honest status.

### Raw TCP streaming (`spk net`, `mic rec`)

The device connects *out* to a TCP peer: `tools/audio_relay.py` is the one-shot peer (run it on a
host that accepts inbound connections): `python3 tools/audio_relay.py recv 5100 mic.raw` then
`mic rec 4000 <host> 5100`; `python3 tools/audio_relay.py send 5101 hello_8k.raw` then
`spk net <host> 5101` (s16le mono 8 kHz). `tools/mic_raw.py cap.raw --seconds 4 --wav out.wav`
exports a raw capture (32-bit FIFO words) as WAV. 0 overruns at 64 KB/s.

## Two-way audio over HTTP (v0.23, `project/main/talk.c`)

Half-duplex intercom on the camera HTTP server (port 8080). Nothing runs until a request arrives.

| Route | What |
|---|---|
| `POST /speak` | body = raw s16le mono 8 kHz **or** a PCM 16-bit WAV (1-2 channels, any rate >= 8 kHz; 16/44.1/48 kHz are box-average resampled on the device, left channel of stereo). Content-Length or chunked. Replies **after playback**: `{"ok":true,"source":"post","ms":4239,"in_rate":16000,"underruns":0}`; 415 for 8-bit or non-PCM input. |
| `GET /speak?tone=HZ&ms=MS` | test tone (-12 dBFS) |
| `GET /speak?url=http://IP[:port]/file.wav` | the device fetches and plays the file (IPv4 literal, plain http); 502 `fetch failed` on a 404. This is the Home Assistant path: HA serves `/config/www/doorbell/*.wav` at `/local/doorbell/`. |
| `GET /mic.wav[?seconds=N]` | 16 kHz mono s16le WAV from the microphone: live (chunked, or until-close for HTTP/1.0) until the client disconnects, or N seconds with an exact Content-Length (N <= 600). The first 200 ms are silence (ADC start transient: about 60 ms full scale + 110 ms DC settle, measured). |
| `GET /talk` | tiny browser page: listen, tone buttons, upload a WAV, play a URL |

Half duplex: while anything plays on the speaker (talk, chime, `spk`) `/mic.wav` emits silence, so
a listener never hears the speaker (no feedback). One speaker user and one microphone listener at a
time; a second gets **503** (`speaker busy` / `microphone busy`). A `/speak` during the ring chime
gets 503 and the chime is skipped during a `/speak`. `/speak` replies only when playback ends, so
HTTP clients need a timeout >= the clip length. `/snapshot.jpg`, `/stream` and RTSP keep working
alongside (one HTTP client slot per audio stream; the client thread stack is 6 KB).

Settings: `talk gain N|auto` (DAC gain for `/speak` only, KV `db_talk_gain`, `auto` = follow
`spk cfg gain`; above 0x1c the amplifier clips), `talk micgain N` (ADC gain for `/mic.wav`, KV
`db_talk_micgain`, default 0x3f; 0 = follow `mic cfg gain`), `talk max S` (1..120, default 30,
KV `db_talk_max`: the rest of the body is discarded), `talk stop` (abort the running `/speak` and
`/mic.wav`; `spk stop` also aborts a `/speak`).

### Verified on the device (v0.23.0/0.23.1)

Tone = clean 1 kHz with no audible amplifier click; 16 k/8 k raw/chunked/48 k-stereo POSTs all play
4239 ms for a 4.24 s clip with 0 underruns and the right pitch (a 1200..3500 Hz tone ladder came out
identical at all three rates); `url=` from HA 200/502; `/mic.wav?seconds=5` = exactly 5.000 s at
16 kHz, a 1 kHz test tone pure, floor rms about 50; the stream is zeros while the device's own tone
plays; both 503s; `talk stop` ends a 10.5 s POST in 4.3 s with a 200; `talk max 3` truncates to
`ms:3000`; settings survive a reboot; snapshot 200 in 80 ms during a stream + tone. Quality is
speech grade (mono, box-average resampling). Heap 111 KB idle. Full log:
[`../testplans/TESTPLAN-talk.md`](../testplans/TESTPLAN-talk.md).

### Home Assistant

`rest_command.doorbell_speak_url` (GET `/speak?url=`, timeout 60), `rest_command.doorbell_tone`,
`shell_command.doorbell_tts_wav` (`/config/doorbell_tts_wav.py`: `POST /api/tts_get_url` with a TTS
entity and a long-lived token in `/config/.doorbell_ha_token`, then the core container's ffmpeg ->
16 kHz mono WAV under `/config/www/doorbell/`), `script.doorbell_say` (`message`, `lang`) and
`script.doorbell_play` (`file` under `/config/www/doorbell/`). `shell_command` needs a core restart
the first time (no reload service); scripts and rest commands reload. Listening from HA:
`http://<device-ip>:8080/mic.wav` in VLC/ffplay, or an iframe card on `/talk` (the camera entity has
no audio). Files: [`../homeassistant/`](../homeassistant/README.md).
