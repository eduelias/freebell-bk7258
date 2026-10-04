# Changelog

Versions are the `FW_VERSION` in `project/main/app_main.c` (also in every report JSON and in
`sys info`). Dates are 2026 unless stated; most of this was written between 2026-10-01 and
2026-10-03.

## 0.24.3
- **microSD power corrected** (2026-10-04): the slot is on the **camera supply (GPIO 28)**, GPIO 13 is the
  battery ADC only (0.24.2's "GPIO 13 powers the card" came from the stock SDK glue and HA snapshots
  keeping the camera open). The SDK's `bk_sd_card_init` also only completes with the media pipeline up
  (CPU at full clock, VIDP on): with the camera closed every 100 kHz probe command works but the init
  times out. A mount now opens the JPEG camera first (`app_sd_cam_ready()`) and votes
  `PM_DEV_ID_SDIO` -> 320 MHz. `sd pwr` default **none** (a stored 6 or 13 is migrated).
- On-demand mount: nothing is mounted while idle; ring/motion/`sd save` mount, save and keep the card for
  `sd keep S` (`db_sd_keep`, default 30 s, min 5) after the last save, then unmount, deinit and release
  the vote. `sd mount` pins the card ("manual") until `sd umount`; `sd umount keep` leaves a power pin
  high. Boot auto-mount and the 10-minute re-detect are gone; a failed on-demand mount makes automatic
  events skip the card for 60 s. `sd_prepare_sleep()` is called from `app_power_prepare_sleep()`.
- Pre-condition before the SDK init (`sd_precondition`: fresh host at 100 kHz, CMD0 x2, CMD8): a silent
  card fails in ~1 s instead of the SDK's 128 blind CMD8 retries. `sd pre FLAGS [GAP_MS]` (`db_sd_pre`)
  tunes it (1 CMD55, 2 CMD1, 4 skip, 8 power cycle first, 16 no camera, 32 no vote). SDK patch **0009**:
  `SDIO_CMD_WAIT_TIME` 4 -> 30 ms so the response wait outlasts the 25 ms hardware timeout.
- `sd status`: card in/out (cd), cam, vote, pin, mounted/manual/keep left, last-mount capacity/free/
  time and counts, cold save time, saved/fail/drop, last file, `bringup:` trace of the last mount.
- power.c: with a power pin on GPIO 13 the battery read is cached (`(cached: sd power)`) while the pin
  is driven and 10 s after; not active with the default `none`.

## 0.24.2
- microSD bring-up (2026-10-04): **GPIO 13 is the card power enable** (what the stock SDK glue drove via
  `bk_gpio_ctrl_external_ldo`), **GPIO 6 is card detect** (input, pull-up, 0 = inserted), not power.
  `sd pwr` default is now `none` (a stored 6 is migrated); `sd pwr 13` drives the pin exactly like the
  SDK helper and holds it while the card is in use; GPIO 6 removed from the camera rails; `sd status`
  shows `cd=0|1`. The battery ADC15 read (same pin) returns the last value while GPIO 13 is held.
- `sd_pins_apply()` reproduces the SDK default SDIO rows (`GPIO_IO_DISABLE` + pull-up, then
  `gpio_sdio_one_line_sel(mode 0)`, capacity 3); config word `0x378` verified.
- `sd probe [cmd|pins|lines|regs]`: FatFs-free diagnostics (pin config words and function select,
  bus lines floating/held, SDIO host registers, CMD0/CMD0/CMD8/CMD55/CMD1 with return codes).
- `pin get/in/out/hiz/raw` and `pin dump N` reply over TCP; new `pin cfg N` decodes one pin's AON word
  and system function-select nibble.
- `sd_do_save` frees its 200 KB PSRAM frame buffer after each save: holding it starved the :8080 HTTP
  server's own buffer and every request got an empty reply after the first card write.
- `sd pins` (GPIO 14/15/16 group) removed: it double-mapped the SDIO functions and GPIO 15 is the
  charging input. Mount: skipped when `cd=1`, 500 ms power settle, SDIO host driver deinit/init before
  `bk_sd_card_init`, one retry after a power cycle. `sd probe lines` refuses while the card is in use.
- First successful card mount and writes on the device (32 GB card B, FAT as shipped). Open: the
  auto-mount right after a reboot still fails until an `sd probe` has run (docs/notes/sdcard.md).

## 0.24.0
- Optional microSD storage (`sdcard.c`): SDIO host + FatFs, a JPEG per ring/motion under
  `/doorbell/`, SNTP time with `sd tz`, `sd` command family; SDK patch 0008 (real FAT timestamps).
- Virtual device (`sim/virtual_doorbell.py`), pytest suite and GitHub Actions workflow;
  `tools/dbcmd.py --host/--port/--token`.
- Merge fix: single `app_reply`, `app_cam_grab` via `cam_want_jpeg`. No-card path, NTP and tz
  verified on the device.

## 0.23.1
- `/mic.wav` sends the first 200 ms as silence (ADC start transient measured at ~60 ms full scale +
  ~110 ms DC settle).

## 0.23.0
- Two-way audio over HTTP (`talk.c`): `POST /speak` (raw or WAV, resampled on the device),
  `/speak?tone=`, `/speak?url=`, `/mic.wav` live or fixed length, `/talk` page; `talk` command.
- Home Assistant talk scripts (`script.doorbell_say` via TTS, `script.doorbell_play`).

## 0.22.3
- Power the camera off when `media_app_camera_open` fails so the acquire retry is a real power
  cycle.

## 0.22.2
- Camera power-off 800 ms + 150 ms settle before the PWDN/RESET pulse (250/60 left 3 of 8 cold
  opens silent); stall self-heal (open 4 s without a frame -> power cycle); acquire retries a silent
  open once.

## 0.22.1
- Cut the camera rails (GPIO 28/5/6) on every close so a reopen is a sensor power-on reset: fixes
  the close/reopen wedge hit by the JPEG <-> H.264 switch. `mem HEXADDR [N]` peek command. HA
  helper nests `rtsp_transport` under `advanced`.

## 0.22.0
- H.264 live stream over RTSP (`rtsp.c`): own RTSP/1.0 server, RFC 6184, TCP interleaved + UDP,
  up to 3 clients; camera encoder mode policy `cam mode auto|jpeg|h264`; CPU1 GOP/SEI/buffer config.

## 0.21.1
- Hold the PIR sensitivity bits (GPIO 25/26) through deep sleep when `wake_pir` is on.

## 0.21.0
- Battery, charger and deep sleep (`power.c`): battery on ADC15 x2, USB/charging on GPIO 19/15,
  pm v2 deep sleep with button/USB/UART/PIR/RTC wake, `pwr` command; `batt_mv batt_pct usb
  charging` in every report and the `wake` event with `reason`. HA template sensors + low-battery
  automation.

## 0.20.2
- GPIO interrupts on CPU0 only (SDK patch 0007): CPU1 used to clear CPU0's PIR/button edges while
  the camera was open. PIR level poller as a safety net; `pir status` shows the driven GPIO 50.

## 0.20.0
- PIR control (`pir.c`): GPIO 50 enable, GPIO 25/26 sensitivity, KV cooldown, `pir` and `motion
  test` commands; ring/motion snapshot pushes in the HA webhook automation.

## 0.19.0
- Night light arbiter on the white LED (camera + dark, motion, press), blue ring Wi-Fi indicator,
  amplifier pop fix (DAC starts on zeros before the PA), `night` and `sys info` commands, HTTP `/`
  is a status page.

## 0.18.x
- Ring effects on a press (bell chime, blue ring blinks, white light when dark; `ring test|dark|fx`).
- 0.18.5: light sensor on ADC14 powered via GPIO 28, dark = above threshold, `light_mv` in reports.

## 0.17.x
- 0.17.0: HTTP camera server on 8080 (`/snapshot.jpg`, MJPEG `/stream`, opens the camera on
  demand); `tools/ha_generic_camera.py` for the HA generic-camera config flow.
- 0.17.1: 6 HTTP clients (HA holds the MJPEG stream), stalled clients dropped.

## 0.15 - 0.16.1
- Camera works: AVDK build flavour (`project-avdk/`, `scripts/build-avdk.sh`), OV9650 driver port
  with the sensor configuration the shipped firmware uses, `cam` command; pin map from the shipped
  firmware (camera power/reset, IR-cut, PIR, battery, SD).

## 0.14.x
- `led` command for the white fill LED (GPIO 44); 0.14.1: GPIO 45 is the blue button ring, not IR.

## 0.13
- Audio DMA engine for DAC and ADC, rate/clock options, clean default gain (0x1c), analysis tools.

## 0.10 - 0.12
- Speaker amplifier enable found (GPIO 46), mic tone metering, raw TCP audio streaming
  (`spk net`, `mic rec`).

## 0.9.x
- TCP command channel on 2323 with a shared token (`tok set`); OTA images served from Home
  Assistant; `scripts/release.sh` for cable-free releases (SDK patch 0004 for HA's empty `Server:`
  header).

## 0.8.x
- `ota URL` command (SDK HTTP OTA client into the `ota` partition); ring reports rate-limited to
  one per 10 s; button confirmed on GPIO 53.

## 0.7.0
- Wi-Fi station with credentials in KV (`wifi set`), JSON reports to a Home Assistant webhook on
  boot/ring/motion/heartbeat (`hook`), pin-mapping tools.

## Before 0.7
- 0.2.0: `reboot` command restores the boot GPIO config so the ROM download window is reachable
  (`flash.sh --kick`). 0.4.0: `pin` commands reclaim pins from peripheral functions. 0.1: SDK sample
  app builds, flashes and boots past the PSRAM hang (SDK patch 0001); flash read/write tools and a
  verified backup of the shipped firmware.
