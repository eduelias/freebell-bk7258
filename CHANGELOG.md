# Changelog

Versions are the `FW_VERSION` in `project/main/app_main.c` (also in every report JSON and in
`sys info`). Dates are 2026 unless stated; most of this was written between 2026-10-01 and
2026-10-03.

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
