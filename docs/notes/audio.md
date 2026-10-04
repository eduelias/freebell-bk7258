# Audio: speaker, microphone, two-way talk, full duplex over RTSP

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

## Full duplex over RTSP (v0.26, `project/main/audio_voc.c`, `rtsp.c`)

The raw engine above is half duplex: a listener hears silence while the speaker plays, because
the microphone would otherwise feed the speaker back. For real two-way talk the firmware now has a
second engine, Beken's **`aud_intf` voice mode** (`bk_avdk/components/multimedia`, the same code
the reference doorbell projects use):

- CPU1 (where the camera/H.264 pipeline already runs) owns the ADC (MIC1, differential) and DAC at
  **8 kHz**, runs the **AEC** (`libaec`, reference = the decoded speaker PCM as it is queued into the
  DAC ring, `aec_proc` per 20 ms frame), **noise suppression** and **G.711 A-law** encode/decode.
  CPU0 only sees A-law bytes: `bk_aud_intf_drv_init` -> `set_mode(VOICE)` -> `voc_init` ->
  `voc_start`; mic frames arrive in a callback on the SDK task `aud_tras` as **320 bytes = 40 ms**;
  speaker frames go in with `bk_aud_intf_write_spk_data()` into a ring of `frame_num` x 320 bytes
  (we use 10, prefilled with 4 frames = 160 ms of silence at start; a write that does not fit is
  dropped silently). Everything crosses the cores through shared PSRAM rings; the API calls are
  synchronous mailbox messages (~ms each).
- The SDK's own amplifier control (`CONFIG_AUD_TRAS_DAC_PA_CTRL`) is **off** for CPU1: its pin is
  GPIO 5 (`AUD_DAC_PA_CTRL_GPIO` default), which on this board is a camera rail. `audio_voc.c`
  switches the real PA (GPIO 46) 50 ms after `voc_start` and 50 ms before `voc_stop`.
- Format on the wire: RTP payload type 8 (PCMA), 8 kHz clock, 160 bytes per 20 ms packet. The SDK
  delivers 40 ms per callback, which rtsp.c splits into two packets. Mic latency device -> client is
  therefore ~40-60 ms plus the network; speaker latency client -> DAC is the ring prefill (160 ms)
  plus whatever the sender buffers.
- Settings: `talk aec on|off` (KV `db_talk_aec`, default on; `bk_aud_intf_voc_aec_ctrl` switches it
  live, so the two can be compared within one session), `talk vocmic N` (KV `db_voc_mic`, ADC gain
  for voice mode, default 0x3f like `/mic.wav`), the speaker gain follows `spk cfg gain` (0x1c:
  above it the amplifier clips). AEC parameters are the SDK defaults (`ec_depth` 20, `TxRxThr` 30,
  `TxRxFlr` 6, `ref_scale` 0, `ns_level` 2, `ns_para` 1, `mic_delay` 0).

### One engine at a time

`aud_raw_busy()` (chime, `spk`, `mic`, /speak, /mic.wav on the raw path) blocks `voc_start`; the
raw opens refuse while voc is starting or stopping. **While voc runs** the raw users are routed
through two taps so nothing stops working during a call:

| User | Raw engine idle | voc engine running (RTSP audio client or `talk voc on`) |
|---|---|---|
| `/speak`, `spk tone|play|net`, ring chime | DAC + DMA ring as before | voc **speaker tap**: PCM 8 kHz -> A-law, paced against the wall clock (<= 200 ms ahead), backchannel frames dropped while the tap is open |
| `/mic.wav` | ADC ring, 16 kHz words | voc **mic tap**: decoded A-law as the same 32-bit word format, **8 kHz** WAV header |
| `mic rec`, `mic level` | ADC direct | refused (`[mic] busy (voc engine active)`) |

The voc engine is started by rtsp.c when the first client sets up an audio track (SETUP of
`track1` or `track2`), retried every 500 ms while the chime holds the hardware, and stopped when
the last client leaves (`rtsp off` stops it too). `talk voc on` starts it by hand (mic frames are
dropped until an RTSP client joins); rtsp.c uses but does not stop a manually started engine.

### RTSP tracks (rtsp.c)

```
m=video 0 RTP/AVP 96   a=rtpmap:96 H264/90000 ... a=control:track0
m=audio 0 RTP/AVP 8    a=rtpmap:8 PCMA/8000  a=ptime:20  a=recvonly  a=control:track1
m=audio 0 RTP/AVP 8    a=rtpmap:8 PCMA/8000  a=ptime:20  a=sendonly  a=control:track2   (only with Require: www.onvif.org/ver20/backchannel)
```

Directions are from the client's point of view (ONVIF Streaming Spec 5.3): go2rtc treats a media
without a direction as one it receives and `a=sendonly` as the one it sends into, so the microphone
track must **not** say `sendonly`. Each track has its own transport: TCP interleaved channels (the
backchannel RTP comes back as `$`-framed packets on its channel, parsed in `process_rx`) or UDP
(server ports `6970 + 6*slot + 2*track`; the backchannel's server port receives). PLAY/PAUSE are
aggregate; `RTP-Info` lists every set-up track. go2rtc numbers interleaved channels by media index
(0-1 video, 2-3 mic, 4-5 backchannel); `tools/rtsp_backchannel.py` does the same.

### Measured on the device (2026-10-04, fw 0.26.0)

1 kHz tone at -12 dBFS through the backchannel, speaker gain 0x1c, mic gain 0x3f, recorded from the
mic track (`tools/rtsp_backchannel.py --tone 1000 --seconds 3 --record x.wav --analyze 1000`):

| | 1 kHz in the mic track | broadband rms |
|---|---|---|
| `talk aec off` | **-10.2 dBFS** (the speaker sits ~10 cm from the capsule) | -11.1 dBFS |
| `talk aec on` | **-100.6 / -109.2 dBFS** (two runs; floor) | -37.5 / -55.2 dBFS |

Attenuation at the tone: > 90 dB (floor-limited); broadband 26-44 dB. The canceller gates the
residual, i.e. the mic goes nearly silent while the far end plays: fine for a doorbell, double-talk
is still to be judged with a person at the door. Timing: first mic RTP packet 225-230 ms after
PLAY, 0 lost packets in all runs; speaker path = 160 ms prefill + sender buffering. Heap 85-88 KB
with the camera open and a session live (0.25.0: ~97 KB idle). Full log:
[`../testplans/TESTPLAN-duplex.md`](../testplans/TESTPLAN-duplex.md).
