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

Inputs: PIR on GPIO 24 (rising edge, 3 s debounce) and button on GPIO 53 (falling edge, pull-up,
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
