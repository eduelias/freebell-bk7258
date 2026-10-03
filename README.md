# doorbell-bk7258-fw

Custom firmware for a refunded Tuya "Smart Visual Doorbell" (board `IP108_MAIN_V05`, Tuya product
IP10B) that never paired: it shipped without a Tuya licence, and the licence can only be issued by
the OEM that owns its PID. So this replaces TuyaOS with our own firmware on Beken's open Armino SDK.

Teardown notes, the verified 8 MB stock flash backup and its analysis live outside the repo in
`~/data/doorbell-bk7258/` (the backup contains device-specific data: never commit or upload it).

## Hardware

| Part | Detail |
|---|---|
| SoC | Beken BK7258 (BK7236 family, Tuya "T5"), 3 cores |
| Flash | GigaDevice GD25WQ64E, 8 MB (MID 0x1765C8) |
| PSRAM | AP Memory APS128XXO-OB9, 16 MB (id 0x8D08) |
| Power | 3.7 V Li-ion pack, charger IC; camera on FPC, PIR, speaker |

Serial: the 4-pin header **next to the battery connector** (`5V TX RX GND`) is UART0. It carries the
BootROM download mode and our firmware's log/shell, both at 115200 8N1, 3.3 V logic.
Device `TX` -> adapter RXD, adapter TXD -> device `RX`, GND -> GND. Leave `5V` unconnected.
A loose GND makes the line float and shows up as streams of `FF` bytes.

## Build

Needs podman (with Rosetta on Apple Silicon) and the SDK checkout at `~/reps/oss/bk_idk`
(`git clone --depth 1 --branch release/v2.0.1 https://github.com/bekencorp/bk_idk.git`).

```bash
scripts/build.sh
```

This applies `patches/*.patch` to the SDK if needed, mounts `project/` over the SDK's
`projects/app`, runs `make bk7258` in `bekencorp/armino-idk:1.5`, and copies the image to
`~/data/doorbell-bk7258/firmware/doorbell-fw_latest.bin`. A full build takes ~11 min under emulation.

### Camera flavour (AVDK)

The camera pipeline lives in Beken's audio/video kit, not in the bare SDK. Clone it next to the SDK
(`git clone --depth 1 --branch release/v2.0.1 https://github.com/bekencorp/bk_avdk ~/reps/oss/bk_avdk`)
and build with:

```bash
scripts/build-avdk.sh
```

Same app sources (`project/main`), built from `project-avdk/` inside the kit: CPU0 runs the app and
the media client, CPU1 the camera/JPEG pipeline, CPU2 the media minor. The script copies
`avdk-overlay/` (our OV9650 driver) into the kit and applies `patches-avdk/`. The packaging files in
`project-avdk/config/bk7258/*.csv` mirror `vendor_flash.c`; after touching them delete
`~/data/doorbell-bk7258/avdk-build/du7/bk7258_cp1` and `_cp2`, otherwise the CPU1 image keeps its
old flash origin and never boots ("cp0 boot cp1 time out"). The OTA image carries all three cores.

## Camera

Sensor: OmniVision **OV9650** (1280x720, 15 fps) on I2C1 = GPIO 0/1 at 0x30, power GPIO 28 (active
high), PWDN/RESET pulse on GPIO 52 (low, 100 ms high, low) before open, MCLK 24 MHz. Its register
table was recovered from the stock firmware (`~/data/doorbell-bk7258/ov9650_init_table_stock.txt`).
The sensor is SCCB: register reads must be write + STOP + read; the SDK's repeated-start helper
fails on it, which is why the driver has its own `ov9650_sccb_read`.

- `cam open [W H] | close | stat | snap HOST PORT | probe ...` over the shell or `tools/dbcmd.py`.
- HTTP on port 8080 (v0.17.0): `http://192.168.2.89:8080/snapshot.jpg` and an MJPEG stream at
  `/stream`. Both open the camera on first use (first frame after ~1 s).
- Home Assistant: `tools/ha_generic_camera.py` (run on du7 with a long-lived token in `~/.ha_token`)
  creates a *generic* camera entry from those two URLs.
- First frame log on CPU1: `ov9650_detect, id: 0x9652`, `auto detect success, dvp camera name:ov9650`,
  then ~9 fps of ~33 KB JPEGs. CPU1 info logs are compiled out (log level 2): use `BK_LOGW` to see
  driver prints.

### H.264 live stream over RTSP (v0.22, `rtsp.c`)

`rtsp://192.168.2.89:554/live`: our own RTSP/1.0 server with RFC 6184 packetization (TCP
interleaved and UDP unicast, up to 3 clients), fed by the BK7258 hardware H.264 encoder (1280x720
Baseline, IDR with SPS/PPS every 30 frames = `CONFIG_H264_P_FRAME_CNT=29` on CPU1, plus an IDR on
every PLAY). The DVP pipeline runs one encoder at a time, so the camera has a mode: JPEG (HTTP) or
H.264 (RTSP), chosen by `cam mode auto|jpeg|h264` (default `auto`: an RTSP client switches to
H.264, an HTTP request switches back to JPEG unless an RTSP client is playing; then `/snapshot.jpg`
is the last JPEG and `/stream` answers 503). The first `/snapshot.jpg` after a switch back can still
be the previous JPEG (the handler does not wait for a new frame).

- `rtsp on|off` (KV, default **off**; the house device has it **on** since 2026-10-03 because the
  HA camera entry streams from it), `rtsp status` (clients, fps, kbps, drops, SPS/PPS cached),
  `rtsp port N`, `rtsp q IMB PMB` (bits per macroblock for I/P frames, default 120/24 =
  ~1.5 Mbit/s nominal, applied live), `rtsp idr`.
- Test: `ffplay -rtsp_transport tcp rtsp://192.168.2.89:554/live` (TCP is the robust transport;
  UDP showed `RTP: missed 6 packets` in 12 s on Wi-Fi). Details and results: `TESTPLAN-rtsp.md`.
- **Frame rate is the sensor's, not the stream's**: the OV9650 with the stock register table gives
  ~8-9 fps in daylight / right after a power-on reset and ~3 fps in a dark room (COM11 `0x3b=0xa9`
  night mode halves the rate as exposure grows). Measured 2026-10-03 evening: H.264 and JPEG both
  2.9-6 fps, identical at 800x480, CPU1 `media_debug` `isr_jpeg == fps_wifi` (nothing is lost in the
  CPU1->CPU0 hand-off). Consequence: ffmpeg/ffprobe need 5-11 s to probe the stream and HA's
  still-from-stream takes ~7 s (its 10 s timeout is marginal at 3 fps). Fix candidates: disable
  night mode in the sensor table (constant ~9 fps, darker night picture) or a GOP of ~10 frames.
- **Camera power-cycle on close (v0.22.1-0.22.3)**: `cam_close_locked()` drops GPIO 28/5/6 and the
  next open waits for 800 ms of power-off plus 150 ms settle before the GPIO 52 pulse. Without it a
  close followed by a reopen within a few seconds (exactly what the JPEG<->H.264 switch does) left
  the sensor silent ("camera wedge", reboot-only before; now also cleared by `cam probe rails off`
  + `on`). 250 ms/60 ms was not enough (3/8 cold opens silent or undetected); 800/150 gave 7/8 and
  the remaining case is self-healed: a camera open for 4 s without a frame is power-cycled at the
  next use (`cam_stalled_locked()`), `rtsp_cam_acquire()` retries a silent open once, and a failed
  `media_app_camera_open` powers the sensor off so the retry is a real power cycle (0.22.3). A
  JPEG->H.264 switch now takes ~2.1 s.
- Home Assistant: the generic camera entry `01M40YN6CC2SS5DFP8X8Y35ZRN` ("Doorbell BK7258") keeps
  **both URLs** since 2026-10-03: still `http://192.168.2.89:8080/snapshot.jpg`, `stream_source
  rtsp://192.168.2.89:554/live`, `advanced.rtsp_transport tcp`, framerate 5 ("option B" of the test
  plan). Stream-only was tried first and rejected: at ~3 fps HA's still-from-stream (go2rtc ->
  ffmpeg) needs ~7 s and hit its 10 s timeout half the time, which would also break the
  `camera.snapshot` ring/motion photos. With the still URL the thumbnail/snapshot is 45 ms (first
  one after a live view ~2.4 s because the camera switches back to JPEG; while someone is watching
  it is the last JPEG). Set with `~/.venv-ha/bin/python ~/ha_generic_camera.py options ENTRY_ID
  --still http://192.168.2.89:8080/snapshot.jpg --stream rtsp://192.168.2.89:554/live
  --rtsp-transport tcp --framerate 5` on du7 (HA 2026.9 keeps `rtsp_transport` inside `advanced`).
  The UI live view uses go2rtc (WebRTC/HLS) and `rtsp status` shows 192.168.2.80 as a TCP client.
- Kconfig changes on CPU1 (`project-avdk/config/bk7258_cp1/config`): after switching to/from this
  version in an existing build dir, delete `$OUT/du7/bk7258_cp1`, `_cp2` and `bk7258` first.
- `mem HEXADDR [N]` reads raw 32-bit words (debug): CPU1's `media_debug_t` counters are reachable
  from CPU0 through the `transfer_log` pointer in the CPU0 map (`grep transfer_log $OUT/du7/bk7258/app.map`,
  then `mem <that address> 1` gives the struct address; word 0 = `isr_jpeg | isr_h264<<16`, word 4
  high half = `fps_wifi`, word 6/7 = last JPEG/H.264 length).

## Flash

```bash
scripts/flash.sh            # dry run: compares sectors, writes nothing
scripts/flash.sh --write    # flash the latest build
```

The tool waits for the BootROM: press the doorbell button (battery connected) when it says so.
With firmware v0.2.0+ running, **`scripts/flash.sh --write --kick` is fully hands-free**: it sends
our `reboot` command (`bk_reboot()`, the NMI-watchdog path with the reboot tag), the ROM answers
link-check within a second, and the flash proceeds (proven 2026-10-02 19:23). Things that do
*not* open the ROM window, so don't bother: the Beken bkreg reboot frame (ignored by the SDK
shell), a deep-sleep RTC wake-up (`pm 2 1 8 9 12 8000 0 1000000000` resets the chip but the ROM
skips the window), the doorbell button while the firmware runs (soft reset, same), and
`--kick-wdt` (the SDK `memset` command silently refuses). From a kick-less image the only way in
is a real power-on with battery **and** USB unplugged first, and check the hooks: a slipped
adapter-TX wire cost an hour of "the ROM is not answering".
It only clears the flash's write-protection bits (keeping quad-enable), skips identical sectors,
reads back every written sector, restores protection, and refuses `0x7C0000-0x800000`
(stock per-device KV and RF calibration) unless `--allow-tail`.

After flashing the chip stays parked in the BootROM. Boot the new image with

```bash
python3 tools/bkreboot.py        # ROM CMD_Reboot, then prints 30 s of boot log
```

(Pulling the battery does **not** power-cycle the board while the USB charger is connected.)
Watch the log any time with `python3 tools/serial_capture.py 115200 60 boot.bin`.

### Restore the stock Tuya firmware

```bash
cd tools && python3 bkwrite.py ~/data/doorbell-bk7258/doorbell_flash_20261002.bin --start 0x0 --write --allow-tail
```

The BK7258's download mode lives in ROM, so no flash content can brick it.

Note: the build rewrites `project/config/bk7258/config` (sorts keys, strips comments), so
document option choices here, not in that file.

## Prototype: Wi-Fi + Home Assistant webhook (firmware v0.7.0)

The device joins Wi-Fi with credentials stored in its own KV flash (never compiled in), then
POSTs JSON to a Home Assistant webhook (`local_only`, no token) on boot, ring, motion, every
5 min (heartbeat) and on request:

```json
{"event":"ring","fw":"0.7.0","ip":"192.168.x.y","mv":3068,"uptime_s":123,"motion_count":4,"ring_count":1}
```

| Command | What it does |
|---|---|
| `wifi set SSID PASS` | save credentials to KV and connect (auto-connects on every boot afterwards) |
| `wifi status` / `wifi connect` / `wifi clear` | link state, RSSI, IP / reconnect / forget |
| `hook show` | current target, default `192.168.2.80:8123/api/webhook/doorbell_bk7258_9f3c` |
| `hook set HOST:PORT/PATH` | change the target (saved to KV) |
| `hook test` / `hook ring` / `hook motion` | fire a report without touching the hardware |

Inputs: PIR on GPIO 24 (rising edge; since v0.20 `pir.c` with cooldown/enable/sensitivity) and button on GPIO 53 (falling edge, pull-up,
0.8 s debounce). `reboot` now restores the pristine boot GPIO config first so the ROM download
window stays reachable.

HA side (created 2026-10-02, on the box): automation `Doorbell BK7258: webhook events`
(`automation.doorbell_bk7258_webhook_events`) writes every payload to
`input_text.doorbell_bk7258_last_report`, and on `ring` sends `notify.mobile_app_sm_s908b` plus a
persistent notification; `motion` -> persistent notification; `boot` -> logbook entry.

From the Mac: `python3 tools/shell.py wifi set <ssid> <password>` (see `tools/shell.py -i`).

## OTA over Wi-Fi (firmware v0.8.0+)

Each build also produces an OTA image, `doorbell-fw_<ver>_app_pack.rbl` (compressed + encrypted
app, ~0.9 MB, copied next to the flash images by `scripts/build.sh`). The firmware's `ota URL`
command uses the SDK's HTTP OTA client: it streams the file into the `ota` partition (0x286000,
1428 KB) and reboots; the bootloader installs it (~35 s end to end on the LAN).

**Image host: Home Assistant.** Files in `/config/www/doorbell/` on the HA box are served at
`http://192.168.2.80:8123/local/doorbell/<file>` with no token, always on, already reachable by
the device. SDK patch `0004` is required for this: HA's aiohttp sends `Server:` with an empty
value, which the stock header parser rejected (`[ota] result -4`). The Mac is *not* a usable host
while its firewall blocks incoming connections to Python (`socketfilterfw --listapps`).

**Hands-free release** (needs the TCP command channel, below):

```bash
scripts/release.sh        # build -> copy rbl Mac -> du7 -> HA -> `ota` over TCP -> device reboots
```

Recovery if an image is bad: the bootloader only switches after a complete, verified download;
worst case the UART + ROM path (`scripts/flash.sh`) still works.

## TCP command channel (firmware v0.9.0+)

Port 2323, one line per connection: `<token> <command>`, reply `ok` / `unknown` / `denied`.
Command output still goes to UART0; the reply only tells you the command was accepted. The token
is stored once over serial (`tok set SECRET`, kept in KV; `tok clear` disables the channel), the
Mac keeps it in `~/data/doorbell-bk7258/tcp_token.env`. All shell commands are available:

```bash
python3 tools/dbcmd.py hook ring
python3 tools/dbcmd.py ota http://192.168.2.80:8123/local/doorbell/doorbell-fw_0.9.1_app_pack.rbl
python3 tools/dbcmd.py reboot
```

It is plain TCP on the home LAN with a shared secret: fine for a prototype, not for the internet.

Ring reports are rate-limited to one per 10 s (button debounce stays 0.8 s; `ring_count` counts
every press).

## Our shell commands

The SDK's `gpio`/`adc_test` commands print through `CLI_LOGI`, which is compiled out in this
build (`CFG_LOG_LEVEL=2`), so the firmware adds its own `os_printf`-based commands on UART0:

| Command | What it does |
|---|---|
| `pin all` | one line with the level of all 56 GPIOs (`PINS 0101...`); pins read `1` until input-enabled |
| `pin mux` | per-pin config byte from the AON GPIO registers (bit2 in-en, bit3 out-en, bit4/5 pull, bit6 peripheral function) |
| `pin get N` | read one pin (+ its config byte) |
| `pin in N [up\|down\|none]` | reclaim N from any peripheral function, make it a true input (output stage off) with that pull, print the level |
| `pin hiz N` | reclaim N and leave it high-impedance (no input, no output, no pull) |
| `pin out N 0\|1` | reclaim N and drive it |
| `pin blink N [count]` | reclaim N, toggle at 300 ms (LED hunting), leave it low |
| `pin watch SECONDS` | poll all pins every 5 ms on the device and print each edge with a timestamp |
| `adc CHANNEL` | one calibrated SARADC reading (raw + mV) |
| `mem HEXADDR [N]` | N (<= 16) raw 32-bit reads, debug only (a bad address faults) |

`pin in/out/blink` refuse GPIO 10/11 (the console). Host-side helpers in `tools/`:
`pinmap_session.py` (mux / pull classification / scan / watch / ADC sweep), `led_hunt.py`
(interactive: blinks one candidate pin at a time and asks what you saw), `led_by_light.py`
(tries to use an ADC as the eyes; on this board neither the light sensor nor the supply sense
reacts to the LEDs, so it found nothing), `pinscan.py` (older, one pin per command).

## Pin map (work in progress, 2026-10-02)

From `tools/pinmap_session.py` (`~/data/doorbell-bk7258/pinmap_session*.txt`). Important: at boot
the SDK's default pin map leaves almost every pin owned by a peripheral function (`pin mux` shows
bit 6 set), and reads/pulls on such pins are meaningless. Firmware v0.4.0+ reclaims a pin with
`gpio_dev_unmap()` + `bk_gpio_set_config(..., GPIO_SECOND_FUNC_DISABLE)`; results from v0.2/v0.3
sessions (before that worked) are noise, except the ADC readings.

| GPIO | Finding | Guess |
|---|---|---|
| 7 (ADC9) | 3.07 V on the ADC, stable across sessions | battery / supply sense (divider) |
| 8 (ADC10) | 0.30 V on the ADC, stable | light sensor or PIR analog |
| 9 | active-high enable: high = light sensor alive (303 mV), low/hi-Z = dead (24 mV) | sensor / LED rail power enable |
| 5, 8 | boot as plain GPIO like 9, no effect on the sensor rail | LED or other plain GPIO |
| 24 | idle low; 8 isolated high pulses of 2-4 s in 8 min at irregular intervals with nobody at the device (`pinmap_watch10/11_passive.txt`) | **PIR motion output**, active-high, 2-4 s hold (one wave in front of it will confirm) |
| 53 | **doorbell button, confirmed 2026-10-03** (active-low, ~0.3 s presses; 46 edges / 13 `ring` posts in a 45 s press test) | button |
| 44 | **white fill LED enable (2026-10-03, camera-verified)**: high = white LED on (big flare), low/hi-Z = off | white LED, active high (`led white on`) |
| 45 (ADC11) | **blue button-ring LEDs (2026-10-03, camera-verified, user-identified)**: high = the four blue LEDs around the button light up (night locator light), low/hi-Z = off. Not infrared: the IR LED sits above the camera lens and is still unmapped. The 2026-10-02 "kills the white LED" reading was a coincidence | ring LEDs, active high (`led ring on`) |
| 46 | **speaker amplifier enable, confirmed 2026-10-03** (active-high; GPIO sweep with the Mac mic listening: 1 kHz tone at 108 with 46 high vs <1 for the other pins; left floating it leaks a faint output) | PA enable, firmware default `spk pa 46` |
| 12 (ADC14) | **light sensor** (3-pin part by the camera flex, powered from GPIO 28): ~700 mV lit room, ~2100 mV covered, higher = darker; `ring dark MV` sets the night threshold | light sensor |
| 0, 1 | I2C1 SCL/SDA to the camera sensor (SDK default pins; the stock firmware uses them too) | camera I2C |
| 28 | camera power enable, active high (stock `.data`: pin 28, level 1) | camera power |
| 52 | sensor PWDN/RESET: stock pulses it low, 100 ms high, low before probing | camera reset |
| 54, 55 | IR-cut filter coil: stock pulses 55 high 300 ms for night, 54 high 300 ms for day (plus sensor B/W registers) | IR-cut, unused with the white night light |
| 44 | **white fill LED** on the LED board (stock calls it "ir led"; Mac camera shows pure white, no IR) — used as the night light | white LED, active high (`led white on`) |
| 50, 25, 26 | PIR enable and sensitivity set0/set1 (stock `tuya_pir.c`) | PIR control |
| 15, 19 | battery/charger status lines (stock `tuya_battery.c`; 15 is externally low) | battery |
| 6 | SD card (stock `ty_app_sdcard.c`) | SD card |
| 10, 11 | UART0 console | never touch |
| others | see the latest `pinmap_session*.txt` (v0.4.0 reclaim) | |

Open: which pin is the button, the PIR, the LEDs (`pin blink` on 5/8/9 first), the chime, the
charger status, and the power-hold line.

## PIR motion (v0.20.0, `project/main/pir.c`)

Pins from the stock `tuya_pir.c` (disassembled 2026-10-03): GPIO 24 = PIR output (rising-edge ISR),
GPIO 50 = PIR enable (stock drives it high and never lowers it), GPIO 25 = sensitivity bit 0,
GPIO 26 = sensitivity bit 1 (stock `set_sens(n)`: 25 = n&1, 26 = n>>1&1; stock default n=1).

| Command | What it does | KV, default |
|---|---|---|
| `pir status` | on/off, sens + GPIO 25/26 levels, cooldown, count, ms since last, raw/ignored edges, live GPIO 24/50 | |
| `pir on\|off` | enable: GPIO 50 high + IRQ on (edges blanked 5 s); off: IRQ off, GPIO 50 low | `db_pir_on`, on |
| `pir sens 0-3` | sensitivity bits (stock used 0/1/2; which end is "more" is untested) | `db_pir_sens`, 1 |
| `pir cooldown MS` | minimum gap between accepted motions, enforced in the ISR (stock: 30 s) | `db_pir_cd`, 10000 |
| `motion test` | simulated edge through the ISR handler: counts, cooldown, `motion` report to HA | |

`pir status` also prints `missed=N`: edges that the 50 ms level poller (`pir_poll()` from the
reporter task) caught because the GPIO ISR did not. Reports carry `"pir_on"` and `"pir_sens"`
next to `light_mv`.

**GPIO interrupts and the camera (found 2026-10-03, fixed by SDK patch 0007).** With the camera
open (CPU1 up) not a single PIR pulse reached the ISR on v0.20.0: every core runs
`bk_gpio_driver_init()`, which enables the one shared GPIO interrupt towards *its own* CPU and
installs a `gpio_isr()` that clears every pending status bit, handler or not, so CPU1 cleared
CPU0's edges first (the button on GPIO 53 was exposed to the same theft). Patch 0007 registers the
GPIO ISR and enables the interrupt on CPU0 only; verified on v0.20.1 with the camera streaming:
every GPIO 24 pulse counted, `missed=0`. The poller stays as a safety net.

**PIR behaviour on the bench (2026-10-03, bare board on the desk, Mac camera as witness).**
Self-triggering with nobody in the room, camera closed, 150 s per setting: `sens 0` 0 pulses,
`sens 1` 2, `sens 3` 12, `sens 2` 15 (with the camera open `sens 1` went up to 10 per 2 min), so
0 is the least and 2 the most sensitive; stock exposed 0/1/2. `sens 0` still detects a person: the
owner waving over the board at ~50 cm gave 4 edges in a minute (each a push with a picture). The
device is left at `pir sens 0` while it lies open on the desk (code default stays 1 = stock); the
only thing between `sens 1` and a picture push every cooldown is the cooldown itself.

HA side (applied 2026-10-03, `automations.yaml` automation `doorbell_bk7258_webhook`, backup
`automations.yaml.bak-20261003-motionsnap`): `ring` -> persistent notification + text push
(tag `doorbell_ring`), then `camera.snapshot` to `/config/www/doorbell/last_ring.jpg` and a second
push with `image: /local/doorbell/last_ring.jpg?v=<ts>` (tag `doorbell_ring_photo`); `motion` ->
persistent notification (light_mv, pir_sens), snapshot to `last_motion.jpg`, one push with the
image (channel/tag `doorbell_motion`). All snapshot/notify steps have `continue_on_error`.
Measured: snapshot written 1.6 s after the webhook (5.7 s for the first one after a boot),
notify step done at +1.9 s. Full procedure and tests in `TESTPLAN-motion.md`.

**Do not `cam close` while Home Assistant polls the camera (found 2026-10-03, open issue).** HA's
generic camera re-opens the camera within seconds of a close; a close followed by a quick reopen
leaves the pipeline wedged: `cam stat` says open but the frame counter never moves (stale JPEG
served), later `media_app_camera_open` returns -1, every HTTP request (even `/`) gets an instant
connection close (`psram_malloc` in the client task fails, or the client slots are eaten by
threads stuck in the open), and `cam close` itself blocks the command task for ~15 s. Heap is
unaffected. Only a reboot clears it. A reopen 7 s after the close worked once, 1-5 s did not.
Suspect: PSRAM frame buffers / CPU1 teardown still in flight when the next open arrives. Fix
candidates: serialise open/close, wait for the close to settle before reopening, reset
`s_cam_len` on close so a stale frame is never served.

## Audio (v0.13.x: DMA engine, both directions working)

Measured with the Mac as ear/voice on 2026-10-03 (`tools/mac_ear.py`, `tools/mic_raw.py`):

- **Speaker**: DAC -> external amplifier, enable = **GPIO 46** (active high, parked low at boot).
  DMA feeds the DAC FIFO (16-bit mono writes) from a 16 KB ring; 0 underruns. The amplifier clips
  hard at the SDK default DAC gain (0x2d = 0 dB): a 1 kHz sine came out as 3 kHz (3rd harmonic 13x
  the fundamental, the dangling speaker has no bass). **Default gain is now 0x1c**, the loudest level
  that stays clean; raise with `spk cfg gain N` once the speaker is back in its enclosure.
  Commands: `spk tone [HZ] [MS]`, `spk play beepa|beepb|bell|ADDR LEN`, `spk net HOST PORT`
  (s16le mono 8 kHz from a TCP peer), `spk stop`, `spk pa PIN|off`,
  `spk cfg [chl L|R|LR] [mode diff|se] [gain N] [rate HZ] [clk xtal|apll]`.
- **Mic**: the electret capsule is MIC1 (left, lo16 of the FIFO word). Excellent: a 1 kHz tone from
  the Mac is a pure sine (Goertzel/rms 0.71), floor rms ~90 at gain 0x3f with the amp off. The ADC
  runs at **2x the configured rate** whatever you configure (8000 -> 16 kHz, 16000 -> 32 kHz, APLL or
  XTAL): keep `rate 8000` and treat captures as **16 kHz**. Commands: `mic level [MS] [HZ]`
  (peak/rms/dc + tone estimate for both halves), `mic rec MS HOST PORT` (raw 32-bit FIFO words to a
  TCP peer, DMA, 0 overruns at 64 KB/s), `mic cfg [chl L|R|LR] [mic 1|2|both] [mode diff|se] [gain N]
  [rate HZ] [clk xtal|apll]`. Use gain 0x3f for speech.
- **Relay**: the device connects out; `tools/audio_relay.py` is the one-shot TCP peer, run on du7
  (the Mac firewall blocks incoming): `systemd-run --user --unit=relay-recv python3 ~/audio_relay.py
  recv 5100 /tmp/mic.raw` then `mic rec 4000 192.168.2.68 5100`; `... send 5101 /tmp/hello_8k.raw` then
  `spk net 192.168.2.68 5101`. `tools/mic_raw.py cap.raw --seconds 4 --wav out.wav` exports a capture.
- **Mac rig**: `ffmpeg -f avfoundation -i ":1"` records the Mac mic (run it in Terminal.app for the
  microphone permission), `afplay` plays tones/markers; `tools/mac_ear.py rec.wav --labels ...`
  analyses a recording that has a 500 Hz marker beep before every test.

## Two-way audio over HTTP (v0.23, `project/main/talk.c`)

Half-duplex intercom on the camera HTTP server (port 8080). Nothing runs until a request arrives;
the DAC/ADC engine from the Audio section is shared with `spk`/`mic`/the ring chime through the
same `s_spk_busy`/`s_mic_busy` exclusion (`aud_engine.h`).

| Route | What |
|---|---|
| `POST /speak` | body = raw s16le mono 8 kHz **or** a PCM 16-bit WAV (1-2 ch, any rate >= 8 kHz; 16/44.1/48 kHz are box-average resampled on the device, left channel of stereo). Content-Length or chunked. Replies **after playback**: `{"ok":true,"source":"post","ms":4239,"in_rate":16000,"underruns":0}`; 415 for 8-bit/non-PCM input. |
| `GET /speak?tone=HZ&ms=MS` | test tone (-12 dBFS) |
| `GET /speak?url=http://IP[:port]/file.wav` | the device fetches and plays the file (IPv4 literal, plain http; the Home Assistant path: HA serves `/config/www/doorbell/*.wav` at `/local/doorbell/`). 502 `fetch failed` on a 404. |
| `GET /mic.wav[?seconds=N]` | 16 kHz mono s16le WAV from the mic: live (chunked, or until-close for HTTP/1.0) until the client disconnects, or N seconds with an exact Content-Length (N <= 600). The first 200 ms are silence (ADC start transient). |
| `GET /talk` | tiny browser page: listen, tone buttons, upload a WAV, play a URL |

Half duplex: while anything plays on the speaker (talk, chime, `spk`) `/mic.wav` emits silence,
so a listener never hears the speaker (no feedback). One speaker user and one mic listener at a
time, a second gets **503** (`speaker busy` / `microphone busy`); a `/speak` during the ring chime
gets 503 and the chime is skipped during a `/speak`. `/speak` replies only when playback ends, so
HTTP clients need a timeout >= the clip length. `/snapshot.jpg`, `/stream` and RTSP keep working
alongside (one HTTP client slot per audio stream; the client thread stack is 6 KB).

| Command | Default | Meaning |
|---|---|---|
| `talk status` | | state idle/speaking/listening, call counters, bytes, last HTTP status, settings |
| `talk gain N\|auto` | `auto` (= `spk cfg gain`, 0x1c) | DAC gain for `/speak` only (KV `db_talk_gain`); above 0x1c the amplifier clips |
| `talk micgain N` | `0x3f` | ADC gain for `/mic.wav` (KV `db_talk_micgain`); 0 = follow `mic cfg gain` |
| `talk max S` | `30` (1..120) | max seconds per `/speak`, the rest of the body is discarded (KV `db_talk_max`) |
| `talk stop` | | abort the running `/speak` and `/mic.wav` (`spk stop` also aborts a `/speak`) |

Verified on the device 2026-10-03 (v0.23.0/0.23.1, Mac microphone as the ear, du7 as a
listener): tone = clean 1 kHz with no amp click (DAC starts on zeros, PA 20 ms later, off before
the DAC stops); 16 k/8 k raw/chunked/48 k-stereo POSTs all play 4239 ms for a 4.24 s clip with 0
underruns and the right pitch (a 1200..3500 Hz tone ladder came out identical at all three
rates); `url=` from HA 200/502; `/mic.wav?seconds=5` = exactly 5.000 s at 16 kHz, 1 kHz test tone
pure, floor rms ~50; the stream is zeros while the device's own tone plays; both 503s; `talk stop`
ends a 10.5 s POST in 4.3 s with a 200; `talk max 3` truncates to `ms:3000`; settings survive a
reboot; snapshot 200 in 80 ms during a stream + tone. Quality is speech grade (mono, box-average
resampling). Heap 111 KB idle on 0.23.1.

Home Assistant (applied 2026-10-03 in `/config/packages/doorbell_bk7258.yaml`, exact YAML and
the helper in `TESTPLAN-talk.md`): `rest_command.doorbell_speak_url` (GET `/speak?url=`, timeout
60), `rest_command.doorbell_tone`, `shell_command.doorbell_tts_wav` (`/config/doorbell_tts_wav.py`:
`POST /api/tts_get_url` with `tts.google_translate_en_com` and a long-lived token in
`/config/.doorbell_ha_token`, then the core container's ffmpeg -> 16 kHz mono WAV under
`/config/www/doorbell/`), `script.doorbell_say` (`message`, `lang`: TTS -> `say.wav` -> device
pulls it) and `script.doorbell_play` (`file` under `/config/www/doorbell/`). `shell_command`
needs a core restart the first time (no reload service); scripts/rest commands reload. Listening
from HA: `http://192.168.2.89:8080/mic.wav` in VLC/ffplay, or an iframe card on `/talk` (the camera
entity has no audio).

## Night light, ring indicator, amp pop (v0.19, `project/main/night.c`)

- **White LED (GPIO 44) arbiter**: the LED is on while any source holds it: `CAM` (camera open and
  the light sensor reads dark, sampled every 20 s, hysteresis `night hyst` = 400 mV and a 60 s
  minimum on-time so the LED's own reflection cannot make it flap; released when light returns or
  the camera closes), `MOTION` (20 s after a motion event in the dark, so HA's snapshot is usable),
  `FX` (30 s on a press in the dark, as before). `led white on|off` is a manual override,
  `led white auto` releases it. `night auto on|off` (KV `db_night_auto`, default on) turns the
  automatic sources off.
- **Blue ring (GPIO 45)**: one short blink at boot, a slow double blink every 3 s while there is
  no IP, off once connected. The 8 blinks of a press always win. `led ring on|off|auto` overrides
  / releases; `night ring on|off` (KV `db_night_ring`, default on) silences the pattern.
- **Amplifier pop**: the DAC + DMA now start on zeros (buffer zeroed) 50 ms before the PA
  (GPIO 46) is enabled, and the PA is disabled on zeros 50 ms before the DAC stops.
- `night status` (all of the above in one line) and `sys info` (fw, uptime, ip, free heap, reset
  reason, light and battery mV). Note `http://192.168.2.89:8080/` answers `503 camera not open`
  until the first snapshot/stream request; use `sys info` to read the running version.
- Testing without touching the sensor: `ring dark 800` makes a lit room (~900 mV) count as dark,
  `ring dark 1400` restores the default. Verified on hardware 2026-10-03 (v0.19.0,
  `TESTPLAN-polish.md` sections 2-8): camera night light on at the next 20 s sample, released
  exactly 60 s after it came on once the light is back, off at once on `cam close`; motion light
  20 s and extended by a second motion; press light 30 s; the LED's own reflection lowers the
  sensor by only ~40 mV in a lit room (hysteresis 400 mV is ample); heap flat at ~126-129 KB; the
  boot blink is visible on the Mac camera. The amp pop could not be measured either way with the
  Mac mic 40 cm away (no transient above the room floor within 200 ms of PA on/off on either
  build), so the by-ear check is still the owner's.

## Battery, charger and deep sleep (v0.21, `project/main/power.c`)

From the stock `tuya_battery.c`: **GPIO 19 = USB present (high)**, **GPIO 15 = charging (high, with
19 high)**, **battery = ADC15 on GPIO 13, x2 divider** (ADC9 is a regulated rail). Deep sleep (SDK pm
v2, SoC reset on wake) is **off by default**.

| Command | What it does | KV, default |
|---|---|---|
| `pwr status` | batt mV / % (raw ADC x ratio), usb, chg, all settings, uptime, boot-hold left, idle s, busy, boot/wake reason | |
| `pwr sleep on\|off` | automatic sleep when on battery and idle; `off` also cancels a pending `sleepnow` | `db_pwr_sleep`, off |
| `pwr sleep force on\|off` | also sleep while USB is present | `db_pwr_force`, off |
| `pwr idle S` | idle seconds before sleeping (min 20) | `db_pwr_idle`, 60 |
| `pwr wake_pir on\|off` | PIR (GPIO 24 rising) wakes; GPIO 50/25/26 held through the sleep | `db_pwr_wpir`, off |
| `pwr snap_min M` | RTC wake every M min (0 = 360 min keepalive) | `db_pwr_snap`, 0 |
| `pwr sleepnow` | sleep in 10 s regardless of USB/idle/boot hold (test) | |
| `pwr chan N` / `pwr ratio PCT` | battery ADC channel / divider x0.01 | `db_pwr_bchan` 15, `db_pwr_bratio` 200 |

Report JSON carries `batt_mv`, `batt_pct`, `usb`, `charging`; a new event `wake` with `reason`
(`button|pir|usb|uart|rtc|gpioNN`) follows every deep-sleep wake. Wake sources: button (53, falling),
USB plug (19, rising), console RX (10, SDK static map), PIR (24, rising, optional), RTC alarm. Policy
for automatic sleep: never before 120 s uptime, never on USB unless `force`, never while busy (HTTP
client incl. HA's stream, spk/mic, ring fx, OTA, pending report), only after `idle` seconds without a
command/HTTP request/press/motion. Before sleeping: amp off, LEDs off, camera closed, GPIO 28/5/6/9
low, `bk_wifi_sta_stop`. A crash/watchdog boot inhibits automatic sleep; a sleep that does not happen
backs out with a 10 min backoff. Full procedure and HA snippets: `TESTPLAN-power.md`.

**Verified on hardware 2026-10-03 (v0.21.0/0.21.1):** `pwr sleepnow` + `snap_min 1`: off the LAN
12 s after the command, back 60 s later, boot reason `deepsleep rtc`, Wi-Fi + IP within ~2 s of the
reset, HA received `boot` and `wake reason rtc` 140 ms apart and wrote `wake_latest.jpg` 5 s after the
wake. Settings survive a reboot. HA sensors (`sensor.doorbell_bk7258_battery`, `_battery_voltage`,
`_light_level`, `_last_event`, `binary_sensor.doorbell_bk7258_usb_power`, `_charging`) follow every
report through the `doorbell_bk7258_report` event. Automatic (idle) sleep tested with `force on`
(see TESTPLAN results). **Caveats:** (1) the ADC readings are bimodal: `mv` (ADC9) alternates between
~2994 and ~3069 and `batt_mv` between ~3886 and ~3982 (2.5 %, same direction, within one report
sequence), most likely a ratiometric ADC following supply sag under Wi-Fi/camera load - calibrate
`pwr ratio` against a multimeter before trusting the percent; (2) GPIO 19 follows the internal pull
(`pin in 19 down` -> usb 0, `up`/`none` -> 1), so "usb 1" on the bench may be a floating pin, not a
charger - the polarity is unconfirmed until the device is seen with and without USB; (3) PIR wake:
the PIR keeps pulsing with GPIO 9, 28, 5, 6 low (so it is powered through the sleep) and the wake
source registration succeeds, but a 2 min sleep at `pir sens 2` produced no PIR wake although the
bare board "self-triggers" every ~10 s while awake - that noise is probably EMI from the running
board, so a real PIR wake needs a person in front of the sensor; (4) GPIO 50 is owned by `pir.c`
(`pir off` keeps it low), power.c only holds it high through the sleep when `wake_pir` is on.

## SDK patches

- `0001-psram-use-configured-chip-id.patch`: with no cached PSRAM id, the stock driver probes the
  8 MB APS6408L first, which hangs this board right after the shell prompt. With
  `CONFIG_PSRAM_AUTO_DETECT=n` and `CONFIG_PSRAM_APS128XXO_OB9=y` (in
  `project/config/bk7258/config`) it now uses the configured part instead.
- `0002-cli-enable-adc-commands.patch`: `CLI_CFG_ADC` is hard-coded 0 in `cli_config.h`; set it
  to 1 on CPU0 so `adc_driver` / `adc_test` exist (battery sensing).
- `0003-cli-register-gpio-adc-in-release.patch`: the whole driver CLI block in `cli_main.c`
  (`gpio`, `uart`, `spi`, `i2c`, `timer`, `flash`, `reg`, `adc`) sits behind
  `CONFIG_DEBUG_FIRMWARE`, which only Beken's internal build defines (as a `-D` flag; there is
  no Kconfig symbol, so the defconfig's `=n` is silently dropped). Declaring the symbol and
  setting it `y` does enable the block, but then `cli_reg` references `*_struct_dump` helpers
  that compile to nothing, because `hal_config.h` tests the flag before `sdkconfig.h` is in
  scope, and the link fails. So instead this patch registers just `cli_gpio_init()` and
  `cli_adc_init()` unconditionally when `CONFIG_DEBUG_FIRMWARE` is off.

- `0007-gpio-irq-cpu0-only.patch`: `bk_gpio_driver_init()` registers the GPIO ISR and enables
  the GPIO interrupt towards the CPU only when built for CPU0. The GPIO interrupt status is one
  shared register set; on CPU1/CPU2 the stock init made those cores clear CPU0's pending edges
  (PIR, button) whenever they were up, i.e. whenever the camera was open.

## Status

- [x] Stock flash backed up and verified; read-only and write tools
- [x] SDK sample app builds, flashes and boots to the shell prompt
- [x] PSRAM fix verified (2026-10-02): boots past the old hang, banner + 5 s heartbeat on UART0,
      shell answers (`help` lists 79 commands: wifi `net sta`, `ping`, `iperf`, `ble`, `pm`,
      `memshow`, `mqttsend`), `psram_malloc 65536` returns 0x60700020
- [~] Map GPIOs: GPIO 9 = sensor-rail enable (found); ADC readings known; LEDs, button, PIR,
      chime, battery sense, power hold still open (LED hunt needs eyes: `pin blink N`)
- [x] Wi-Fi join (credentials in KV, auto-reconnect, ~1 s after boot) and HTTP webhook to
      Home Assistant: boot/motion/test verified end-to-end on 2026-10-02 22:41 over a throwaway
      hotspot on du7 (`doorbell-test`); HA automation + `input_text` helper live
- [ ] Switch to the real home Wi-Fi (`tools/shell.py wifi set SSID PASS`), then tear down the du7
      hotspot (`nmcli con down doorbell-test; nmcli con delete doorbell-test`, kill the DHCP-only
      dnsmasq, drop the MASQUERADE/FORWARD rules)
- [ ] Confirm the button really is GPIO 53 (press it, expect a `ring` report) and tune PIR debounce
- [ ] Camera (DVP/MIPI sensor on the FPC) and JPEG/H.264 snapshot or stream
