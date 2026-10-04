# doorbell-bk7258-fw

Open firmware for the Tuya "Smart Visual Doorbell" built on the Beken **BK7258** (board
`IP108_MAIN_V05`, Tuya product IP10B), plus the Home Assistant glue that turns it into a fully
local video doorbell: ring and motion events to a webhook, JPEG snapshots and an H.264 RTSP stream,
two-way audio over HTTP, chime and night light, battery reporting and deep sleep, optional microSD
recording, and over-the-air updates. It is built on Beken's open Armino SDK (bk_idk + bk_avdk,
`release/v2.0.1`) with a handful of patches and an OV9650 camera driver.

Why: the author's unit shipped without a Tuya licence and could never be paired, and the licence
can only be issued by the OEM. Rather than e-waste it, the stock image was dumped, the board was
reverse-engineered from that image and from the bench, and TuyaOS was replaced. Status: **works,
in daily use by the author since 2026-10-03** (v0.24.0). It is a one-person hobby project; read
the two sections below before you flash anything.

## Legal and safety

- Replacing the vendor firmware **voids the warranty** and **removes the device from the Tuya app
  forever** (the Tuya licence lives in the flash region this firmware does not use, but there is no
  supported way back except your own full dump).
- Brick risk is low: the BK7258's download mode lives in mask ROM and is always reachable over the
  UART header, whatever is in flash ([docs/notes/flashing.md](docs/notes/flashing.md)).
- The board carries a **3.7 V Li-ion pack**: do not short it, do not run it with a swollen cell,
  unplug battery *and* USB before touching the header, and keep the device mains-free as designed.
- Your stock dump contains **device-specific data** (Tuya licence, RF calibration, Wi-Fi
  credentials). Keep it private; never commit or upload it.
- Licence: Apache-2.0 ([LICENSE](LICENSE)); third-party credits in [NOTICE](NOTICE).

## Security model

This firmware is designed for a trusted home LAN behind NAT and must never be exposed to the
internet or to a network you do not trust. Everything speaks plain HTTP/TCP: the command channel on
TCP 2323 is protected only by a shared token sent in clear text; the camera, RTSP stream,
microphone (`/mic.wav`) and speaker (`/speak`) on ports 8080/554 have no authentication at all, so
any device on the same network can watch, listen or talk through the doorbell; the token-protected
`ota` command fetches an image from any HTTP URL you give it; ring/motion events are posted to a
Home Assistant webhook whose id is the only secret. Put the doorbell on an isolated VLAN with
firewall rules that allow only your Home Assistant host to reach ports 2323/8080/554, keep the
webhook `local_only`, choose a long random token and webhook id, and rotate both if they leak.
There is no secure boot, no TLS, and no protection against someone with physical access to the
UART header.

Hardening ideas (not implemented): the shared token on the HTTP endpoints too; `/speak` and
`/mic.wav` as an opt-in (`talk on|off`); an IP allow-list for 2323/8080/554; restricting `ota` and
`/speak?url=` to a configured base URL.

## Hardware

| Part | Detail |
|---|---|
| Board | `IP108_MAIN_V05`, Tuya product IP10B; battery doorbell with camera on an FPC, PIR, speaker, microphone, white fill LED, blue button ring, light sensor, IR-cut coil, microSD slot |
| SoC | Beken BK7258 (BK7236 family, Tuya "T5"), 3 cores: CPU0 application, CPU1 camera/JPEG/H.264, CPU2 media minor |
| Flash | GigaDevice GD25WQ64E, 8 MB (MID 0x1765C8) |
| PSRAM | AP Memory APS128XXO-OB9, 16 MB (id 0x8D08) |
| Camera | OmniVision OV9650, 1280x720, DVP, I2C1 at 0x30 |
| Power | 3.7 V Li-ion pack, charger IC, USB-C for charging only |

**UART header**: the 4-pin header next to the battery connector, marked `5V TX RX GND`, is UART0:
BootROM download mode and the firmware's log/shell, 115200 8N1, **3.3 V logic**. Device `TX` ->
adapter RXD, adapter TXD -> device `RX`, GND -> GND, leave `5V` unconnected. A loose GND shows up as
streams of `FF`. *(`docs/img/`: add a photo of the UART header.)*

## Features

| Area | What works |
|---|---|
| Network | Wi-Fi station (credentials in the device's KV flash, auto-reconnect), JSON reports to a Home Assistant webhook: `boot`, `ring`, `motion`, `heartbeat` (5 min), `wake` (with `reason`), `test`; each carries battery, light, USB/charging, counters |
| Doorbell | button on GPIO 53 with debounce and a 10 s report rate limit; bell chime on the speaker; blue ring blinks; white night light when dark; PIR on/off, sensitivity and cooldown |
| Camera | JPEG snapshot `http://<device-ip>:8080/snapshot.jpg`, MJPEG `/stream`, H.264 720p over RTSP `rtsp://<device-ip>:554/live` (TCP/UDP, 3 clients) |
| Audio | half-duplex two-way audio over HTTP: `POST /speak` (raw/WAV), `/speak?url=`, `/speak?tone=`, `/mic.wav`, a `/talk` browser page |
| Power | battery voltage/percent, USB and charging state, deep sleep with wake on button, PIR, USB, UART or RTC alarm (periodic snapshot) |
| Storage | optional microSD: a JPEG per ring/motion with SNTP-synced names |
| Updates | OTA over Wi-Fi from any HTTP URL (`ota`), cable-free `scripts/release.sh`; UART flashing via the ROM for the first time and as a last resort |
| Control | serial shell on UART0 and the same commands over TCP 2323 with a token (`tools/dbcmd.py`) |
| Development | `sim/`: a virtual doorbell with the same TCP/HTTP/webhook interfaces, pytest suite, CI |

## Quick start

Prerequisites: podman (or docker; on Apple Silicon podman needs Rosetta) and the
`bekencorp/armino-idk:1.5` image; clones of
[bk_idk](https://github.com/bekencorp/bk_idk) and [bk_avdk](https://github.com/bekencorp/bk_avdk)
at `release/v2.0.1` (`git clone --depth 1 --branch release/v2.0.1 ...`); Python 3 with `pyserial`
(plus `pillow` and `pytest` for the simulator); `ffmpeg`/`ffplay` for RTSP; `curl`; a 3.3 V USB-UART
adapter.

```bash
mkdir -p ~/.config/doorbell-bk7258 && cp scripts/env.example ~/.config/doorbell-bk7258/env   # then edit: SDK, AVDK, FW_DIR, DOORBELL_HOST, HA_URL, OTA_*
scripts/build-avdk.sh                                   # camera-capable build (~11 min emulated); scripts/build.sh = bare SDK flavour
```

**First flash over UART** (once; afterwards everything is OTA):

1. Unplug battery and USB. Wire the header (3.3 V adapter, no 5 V). Run `scripts/flash.sh --write`.
2. When the tool waits for the BootROM: connect the battery and press the doorbell button. A real
   power-on is the only way into the ROM window on a stock or kick-less image; pulling the battery
   does **not** power-cycle the board while USB is plugged in.
3. The tool verifies every sector and leaves the chip parked in the ROM: `python3 tools/bkreboot.py`
   boots the image and prints 30 s of log. From now on `scripts/flash.sh --write --kick` is
   hands-free (the firmware's `reboot` reopens the ROM window).

**Provision over serial** (`python3 tools/shell.py <cmd>`, or `tools/shell.py -i` for a session):

```bash
wifi set <SSID> <password>          # stored in KV, auto-connects on every boot
tok set <long-random-secret>        # enables the TCP command channel; put the same value in DOORBELL_TOKEN
hook set <ha-ip>:8123/api/webhook/<webhook-id>   # see Home Assistant below; prints "hook not set" until done
sys info                            # fw, ip, heap, reset reason, light/battery mV
```

From here on use the LAN: `python3 tools/dbcmd.py sys info`, and `scripts/release.sh` builds,
publishes the `.rbl` where `OTA_URL_BASE` serves it and runs `ota` on the device (`--no-build` to
ship the current image). Worked example with the author's addresses: `DOORBELL_HOST=192.168.2.89`,
`HA_URL=http://192.168.2.80:8123`, `OTA_URL_BASE=http://192.168.2.80:8123/local/doorbell`,
`OTA_PUBLISH="scp {rbl} root@192.168.2.80:/config/www/doorbell/"`.

## Configuration

One optional dotenv file, `~/.config/doorbell-bk7258/env` (template: `scripts/env.example`). Every
key is also honoured as a real environment variable, and CLI flags override both.

| Key | Meaning |
|---|---|
| `DOORBELL_HOST` | device IP or hostname |
| `DOORBELL_TOKEN` | TCP command token (the value given to `tok set`); or `DOORBELL_TOKEN_FILE` pointing at a file containing `TOKEN=...` |
| `SERIAL_PORT` | UART adapter (default: the first `/dev/cu.usbserial-*` or `/dev/ttyUSB*`) |
| `FW_DIR` | where built images land (default `~/data/doorbell-bk7258/firmware`) |
| `OUT` | build directory |
| `SDK`, `AVDK` | paths to the bk_idk and bk_avdk clones |
| `HA_URL` | e.g. `http://<ha-ip>:8123` (for `tools/ha_generic_camera.py`) |
| `HA_TOKEN_FILE` | long-lived HA token file (default `~/.ha_token`) |
| `OTA_URL_BASE` | HTTP base URL the *device* can fetch images from, e.g. `http://<ha-ip>:8123/local/doorbell` |
| `OTA_PUBLISH` | shell command that copies the image where `OTA_URL_BASE` serves it; `{rbl}` and `{name}` are substituted, e.g. `scp {rbl} user@ha:/config/www/doorbell/` |
| `BUILD` | which build script `release.sh` runs: `build-avdk.sh` (default) or `build.sh` |

Tools: `tools/dbcmd.py [--host H] [--port P] [--token T] CMD...` (TCP), `tools/shell.py` /
`tools/serial_capture.py` / `tools/bkreboot.py` / `tools/bkwrite.py` (serial, accept `--port`),
`tools/ha_generic_camera.py` (HA config flow), `tools/audio_relay.py`, `tools/mic_raw.py`.

## Command reference

Same commands on the UART shell and over TCP 2323 (`<token> <command>` on one line; reply `ok` /
`unknown` / `denied` followed by the command's text). Settings marked KV persist in the device's
flash and survive OTA.

| Command | Forms | KV (default) |
|---|---|---|
| `wifi` | `set SSID PASS` \| `status` \| `connect` \| `clear` | ssid/pass |
| `hook` | `show` \| `set HOST:PORT/PATH` \| `test` \| `ring` \| `motion` | `db_hook` (unset) |
| `tok` | `set SECRET` \| `clear` (disables TCP 2323) | token |
| `ota` | `ota URL` (fetch `app_pack.rbl` over HTTP, install, reboot) | |
| `sys` | `info` (fw, uptime, ip, heap, reset reason, light/battery mV) | |
| `reboot` | reset the SoC; the ROM download window is reachable right after | |
| `ring` | `test` \| `dark MV` \| `chan ADC` \| `fx on\|off` (chime + LEDs on a press) | `db_dark_mv` (1000), `db_light_chan`, fx on |
| `led` | `white\|ring on\|off\|auto` (manual override / release) | |
| `night` | `auto on\|off` \| `ring on\|off` \| `hyst MV` \| `status` | `db_night_auto` (on), `db_night_ring` (on), `db_night_hyst` (400) |
| `pir` | `on\|off` \| `sens 0-3` \| `cooldown MS` \| `status` | `db_pir_on` (on), `db_pir_sens` (1), `db_pir_cd` (10000) |
| `motion` | `test` (simulated PIR edge through the ISR path) | |
| `pwr` | `status` \| `sleep on\|off` \| `sleep force on\|off` \| `idle S` \| `wake_pir on\|off` \| `snap_min M` \| `sleepnow` \| `chan ADC` \| `ratio PCT` | `db_pwr_sleep` (off), `db_pwr_force` (off), `db_pwr_idle` (60), `db_pwr_wpir` (off), `db_pwr_snap` (0), `db_pwr_bchan` (15), `db_pwr_bratio` (200) |
| `cam` | `open [W H] [h264]` \| `close` \| `stat` \| `mode auto\|jpeg\|h264` \| `snap HOST PORT` \| `probe [PWR_PIN ...]` \| `probe scan` \| `probe rails on\|off` \| `probe bb SCL SDA` | mode (auto) |
| `rtsp` | `on\|off` \| `status` \| `port N` \| `q IMB PMB` \| `idr` | on/off (off), port (554), q (120/24) |
| `spk` | `tone [HZ] [MS]` \| `play beepa\|beepb\|bell\|ADDR LEN` \| `net HOST PORT` \| `stop` \| `pa PIN\|off` \| `cfg [chl L\|R\|LR] [mode diff\|se] [gain N] [rate HZ] [clk xtal\|apll]` | gain 0x1c, PA 46 |
| `mic` | `level [MS] [HZ]` \| `rec MS HOST PORT` \| `cfg [mic 1\|2\|both] [mode diff\|se] [gain N] [chl L\|R\|LR] [rate HZ] [clk xtal\|apll]` | |
| `talk` | `status` \| `gain N\|auto` \| `micgain N` \| `max S` \| `stop` | `db_talk_gain` (auto), `db_talk_micgain` (0x3f), `db_talk_max` (30) |
| `sd` | `status` \| `mount` \| `umount [off]` \| `format [TOKEN]` \| `ls [DIR] [SKIP]` \| `save [EVENT]` \| `auto on\|off` \| `pwr PIN\|none` \| `pins 1\|0` \| `power on\|off` \| `ntp HOST` \| `tz MIN` \| `time EPOCH` \| `sync` | `db_sd_auto` (on), `db_sd_pwr` (6), `db_sd_pins` (1), `db_sd_ntp` (pool.ntp.org), `db_sd_tz` (0) |
| `pin` | `all` \| `mux` \| `dump` \| `get N` \| `in N [up\|down\|none]` \| `hiz N` \| `out N 0\|1` \| `raw N HEX` \| `blink N [count]` \| `watch SECONDS` (refuses 10/11) | |
| `adc` | `adc CHANNEL` (one calibrated SARADC reading) | |
| `mem` | `mem HEXADDR [N<=16]` (raw 32-bit reads; a bad address faults) | |

Report JSON (every event):

```json
{"event":"ring","fw":"0.24.0","ip":"<device-ip>","mv":3068,"light_mv":812,"uptime_s":5121,
 "motion_count":4,"ring_count":1,"pir_on":1,"pir_sens":1,"batt_mv":3940,"batt_pct":78,"usb":0,"charging":0}
```

plus `"reason":"button|pir|usb|uart|rtc|gpioNN"` on `wake`.

### HTTP and RTSP endpoints

| Endpoint | What |
|---|---|
| `GET :8080/` | one-line status page (`503 camera not open` until the first camera request) |
| `GET :8080/snapshot.jpg` | latest JPEG (opens the camera on demand, ~1 s for the first frame) |
| `GET :8080/stream` | MJPEG (`multipart/x-mixed-replace`); 503 while an RTSP client plays |
| `POST :8080/speak` | play raw s16le 8 kHz or a PCM WAV; replies after playback |
| `GET :8080/speak?tone=HZ&ms=MS` / `?url=http://...` | test tone / fetch and play a WAV |
| `GET :8080/mic.wav[?seconds=N]` | 16 kHz mono WAV from the microphone, live or N seconds |
| `GET :8080/talk` | browser page for listen / tone / upload / URL |
| `rtsp://<device-ip>:554/live` | H.264 720p (`rtsp on` first; TCP transport recommended) |

## Home Assistant

1. Create the webhook automation with your own random `<webhook-id>` and a Text helper for the
   last report; `hook set <ha-ip>:8123/api/webhook/<webhook-id>` on the device.
2. Add a *Generic Camera*: still `http://<device-ip>:8080/snapshot.jpg`, stream
   `rtsp://<device-ip>:554/live`, RTSP transport `tcp` (UI or `tools/ha_generic_camera.py`).
3. Install the package (notify group with your phones, battery/light sensors, low-battery alert).
4. Optional: the talk scripts (`script.doorbell_say` via HA TTS, `script.doorbell_play`), which
   need a token file on the HA host and `doorbell_tts_wav.py` in `/config`.
5. Optional: use `/config/www/doorbell/` as the OTA image host.

Step-by-step instructions, the sanitized YAML and the helper script:
[docs/homeassistant/](docs/homeassistant/README.md).

## Pin map

| GPIO | Function | Notes |
|---|---|---|
| 0, 1 | camera I2C1 SCL/SDA | sensor at 0x30 |
| 2, 3, 4 | SD CLK / CMD / DAT0 | SDIO 1-bit, SDK pin group 1 |
| 5 | camera-related rail | cut on camera close; exact function unverified |
| 6 | SD card detect | input with pull-up, 0 = card inserted (was wrongly listed as SD power until 0.24.1) |
| 7 (ADC9) | regulated rail sense | reported as `mv` |
| 8 (ADC10) | unknown | 0.30 V, stable; unverified |
| 9 | sensor/LED rail enable | active high |
| 10, 11 | UART0 | console, never touch |
| 12 (ADC14) | light sensor | ~700 mV lit, ~2100 mV dark; higher = darker |
| 13 (ADC15) | battery voltage | x2 divider |
| 15 | charging status | from the shipped firmware; polarity unverified |
| 19 | USB present | from the shipped firmware; polarity unverified (follows the internal pull on the bench) |
| 24 | PIR output | rising edge, 2-4 s hold |
| 25, 26 | PIR sensitivity bit 0 / 1 | |
| 28 | camera power enable | active high; also powers the light sensor |
| 44 | white fill LED | active high (night light) |
| 45 | blue button ring LEDs | active high |
| 46 | speaker amplifier enable | active high |
| 50 | PIR enable | active high |
| 52 | camera PWDN/RESET | pulsed low-high-low before open |
| 53 | doorbell button | active low, pull-up |
| 54, 55 | IR-cut filter coil | day / night pulse; unused by this firmware |

Unmapped: the IR illuminator above the lens. Method and raw observations:
[docs/notes/pin-mapping.md](docs/notes/pin-mapping.md).

## Virtual device

`sim/virtual_doorbell.py` is a software stand-in for the board: the same TCP command channel, HTTP
routes (synthetic 720p frames, talk endpoints) and webhook JSON, with the firmware's reply strings,
PIR cooldown, night-light arbiter, sleep/wake cycle, OTA version bump and KV persistence modelled.
`python3 sim/virtual_doorbell.py` (token `sim`), then `python3 tools/dbcmd.py --host 127.0.0.1
--token sim sys info`; `python3 -m pytest sim/tests` runs the suite (also in CI). Details:
[sim/README.md](sim/README.md).

## Repository layout

| Path | What |
|---|---|
| `project/main/` | the application: `app_main.c` (Wi-Fi, webhook, TCP channel, HTTP server, camera, OTA, audio engine, commands), `night.c`, `pir.c`, `power.c`, `rtsp.c`, `talk.c`, `sdcard.c`; `vendor_flash*.{c,h}` are Beken's partition table (see NOTICE) |
| `project/`, `project-avdk/` | build glue and Kconfig for the bare-SDK and the AVDK (camera) flavours; both compile the same `project/main` |
| `patches/`, `patches-avdk/`, `avdk-overlay/` | changes to bk_idk / bk_avdk, applied by the build scripts ([docs/notes/sdk-patches.md](docs/notes/sdk-patches.md)); the OV9650 driver lives in the overlay |
| `scripts/` | `build.sh`, `build-avdk.sh`, `flash.sh`, `release.sh`, `env.example` |
| `tools/` | host-side Python: ROM flasher (`bkwrite.py`, `bkread.py`, `bkdump.py`, `bkreboot.py`, `linkcheck.py`), serial (`shell.py`, `serial_capture.py`), LAN (`dbcmd.py`), HA (`ha_generic_camera.py`), audio and pin-mapping helpers |
| `sim/` | virtual doorbell + tests |
| `docs/homeassistant/` | HA automation, package, TTS helper, setup guide |
| `docs/notes/` | technical notes: [flashing](docs/notes/flashing.md), [camera](docs/notes/camera.md), [gpio-irq](docs/notes/gpio-irq.md), [pir](docs/notes/pir.md), [audio](docs/notes/audio.md), [night-light](docs/notes/night-light.md), [power](docs/notes/power.md), [sdcard](docs/notes/sdcard.md), [pin-mapping](docs/notes/pin-mapping.md), [sdk-patches](docs/notes/sdk-patches.md), [bench-setup](docs/notes/bench-setup.md) |
| `docs/testplans/` | per-feature test plans with measured results ([index](docs/testplans/README.md)) |
| `CHANGELOG.md` | per-version summary |

## Known issues

- **Frame rate follows the sensor**: about 9 fps in daylight, about 3 fps in the dark (the OV9650
  night mode in the shipped configuration halves the rate as exposure grows). RTSP probes take
  5-11 s and HA's still-from-stream is marginal, which is why the camera entity keeps the still URL.
- **White LED flash at reset**: the white LED (GPIO 44) lights briefly on every reset, before the
  firmware runs and configures the pin; cosmetic, cause not investigated.
- **Camera close + quick reopen wedge**: fixed in v0.22.1-0.22.3 by power-cycling the camera rails
  on close; if you still see a silent camera, `cam probe rails off` then `on` recovers it.
- **Deep sleep is partly verified**: RTC wake and idle sleep are tested; button, PIR and USB wake
  and the USB/charging polarity are pending ([docs/notes/power.md](docs/notes/power.md)). Sleep is
  off by default.
- **Battery ADC is bimodal** (about 2.5 % swing under load): calibrate `pwr ratio` against a meter.
- **Amplifier click**: the DAC now starts on zeros before the PA; a residual click is possible and
  was not measurable on the bench.
- **microSD writes** are implemented but were not yet exercised with a card when v0.24.0 shipped.
- `/snapshot.jpg` right after an RTSP session can return the previous JPEG once.

## Contributing and credits

Issues and pull requests are welcome, in particular reports from other IP10B/IP108 boards (pin
differences, sensor variants), a photo for `docs/img/`, and the hardening items above. Keep
hardware-touching changes behind a test plan like the ones in `docs/testplans/`; the simulator
suite must stay green. Built on Beken's Armino SDK and AVDK; the OV9650 driver follows Beken's
`dvp_ov2640.c` and programs the sensor configuration the shipped firmware uses. Full credits and
licences of the included third-party files: [NOTICE](NOTICE).

## Licence

Apache License 2.0, copyright 2026 Eduardo Elias Saleh. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
