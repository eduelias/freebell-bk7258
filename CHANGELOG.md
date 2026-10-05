# Changelog

Versions are the `FW_VERSION` in `project/main/app_main.c` (also in every report JSON and in
`sys info`). Dates are 2026 unless stated; most of this was written between 2026-10-01 and
2026-10-03.

## 0.27.0
- **Daylight snapshots after a cold open are no longer white** (2026-10-05). Every camera open
  now reads the light sensor and passes a scene hint (`day`/`night`) to the CPU1 sensor driver in
  the spare high bits of `media_camera_device_t.info.fps` (a bit-mask enum; CPU1 copies the struct
  through the mailbox unchanged and calls `set_fps()` with it right after the init table, no SDK
  patch; the enum is one byte, so bit 7 = night, bit 6 = day, five bits = a start-exposure
  class). Day: AEC/AGC off, COM11 night mode off, AGC ceiling 2x, AE target 0x78/0x68, a start
  exposure from the light reading (1 row on a sunny day; `cam exp N` to fix one, KV `db_cam_exp`)
  and unity gain, and the AEC/AGC take over **one second later** from a CPU1 thread: enabled on a
  correctly exposed stream the engine stays put, while any AEC-on start overshoots to ~130 rows on
  the dark start-up frames and needs ~4 s to come back (writing the registers with AEC enabled did
  nothing at all, measured); night: the shipped table plus the stock firmware's
  black-and-white registers (`0x3a=0x11`, `0x67/0x68=0x80`). `cam scene auto|day|night|stock`
  (KV `db_cam_scene`) overrides the reading; `stock` restores the 0.26 behaviour. CPU1 leaves a
  register read-back and a 15 s AEC trace in two globals readable with `mem` (addresses in
  `docs/notes/camera.md`).
- Fixed on the way: the wake report's `light_read_mv()` could drop GPIO 28 under an open in
  progress (a cold wake read 31 mV and would have picked `day` at night); `/snapshot.jpg` clients
  allocate the current frame size instead of 150 KB each, and answer `503 busy (mem)` instead of
  closing the socket when PSRAM is short (HA's wake photo + preview + a third client). The IR-cut coil is pulsed (GPIO 54 day / 55 night, 300 ms, overlapping the open's own
  delays) when the scene differs from the last pulse (KV `db_ircut`); `cam ircut day|night` by hand.
- **Exposure settle gate**: `/snapshot.jpg`, the first MJPEG frame and the SD save wait after an
  open until `cam settle N` frames arrived (default 16) or, after N/2 frames, three consecutive
  JPEGs are within `PCT` % in size (4), capped at `MS` after the open (4500; KV `db_cam_settle`
  "N MS PCT"). The
  wait is reported in the `X-Settle-Ms` response header and in `cam stat` (`scene`, `open_frames`,
  `settled <how>@<ms>`, `wait`, `settle`, `ircut` fields). Measurements before/after in
  `docs/notes/camera.md`.
- tools: **remote builder** (2026-10-04). `scripts/build-remote.sh` mirrors the working tree to an
  x86-64 host over ssh/rsync, runs `scripts/build-avdk.sh` (or `build.sh` with `--bare`) there and
  copies the images back into `FW_DIR`; flags `--no-sync-back`, `--shell`; keys `REMOTE_BUILD_HOST`,
  `REMOTE_BUILD_PORT`, `REMOTE_REPO_DIR`, `REMOTE_SDK`, `REMOTE_AVDK`, `REMOTE_OUT`,
  `REMOTE_FW_DIR`, `REMOTE_WAKE` in `scripts/env.example`; `release.sh` accepts
  `BUILD=build-remote.sh`. `build.sh`/`build-avdk.sh` now run under docker as well as podman
  (`CONTAINER_RUNTIME` to force one; the podman-machine start only runs where a machine exists),
  `build.sh` takes `GIT_REV` for the image name and uses a portable `stat`. The author's remote is a
  WSL2 Ubuntu-24.04 on a Windows 11 desktop (`docs/notes/remote-build.md`): full build 41 s,
  incremental 15 s, against ~11 min / ~100 s emulated on the laptop.

## 0.26.2
- **Radio chimes no longer ring at every boot/wake** (2026-10-04, SDK patch
  `0010-gpio-default-map-doorbell.patch`). The SDK's boot-time pin map (`GPIO_DEFAULT_DEV_CONFIG`,
  written by `bk_gpio_driver_init()` at every reset, before our code) had GPIO 53 = the doorbell
  button = the 433 MHz chime transmitter's key line as an LCD_B2 peripheral output; the idle
  peripheral held it low until `inputs_init()` reclaimed it, which is a button press to the
  transmitter. The patched table follows the stock firmware's (disassembled): 53 input + pull-up,
  6/7 hi-Z, 27-39 peripheral functions off until the camera driver maps them, 25/26/50 keep their
  level through low power. CPU1/CPU2 apply no table (SDK defconfig), like the stock. Story in
  `docs/notes/pin-mapping.md`, row-by-row in `docs/notes/sdk-patches.md`.
- `pwr hold9` default **off** (0.26.1 shipped it on): the rail theory it tested is superseded; a
  stored `db_pwr_hold9=1` still applies until `pwr hold9 off`.
- GPIO 51 (the stock firmware's second key, not wired here) parked as input + pull-up at boot.
- Simulator: `pwr hold9 on|off`.

## 0.26.1
- `pwr hold9 on|off` (KV `db_pwr_hold9`, default on in this version): keep the GPIO 9 sensor rail
  high through deep sleep (`bk_gpio_register_lowpower_keep_status`) instead of dropping it in
  `app_power_prepare_sleep()`. First attempt at the chimes ringing at every wake, on the theory that
  the rail's power cycle keyed the 433 MHz transmitter; it did not stop them (the cause was the boot
  pin map, see 0.26.2).

## 0.26.0
- **Full-duplex two-way audio over RTSP with echo cancellation** (2026-10-04, feature "duplex"). New
  `project/main/audio_voc.c`: the SDK's `aud_intf` voice mode (ADC/DAC, AEC, noise suppression and
  G.711 on CPU1, API on CPU0 over the media mailbox) as a second audio engine, 8 kHz G.711 A-law in
  20 ms frames. The RTSP stream now has three tracks: `track0` H.264, `track1` PCMA/8000 microphone
  (`a=recvonly`), and, when the client asks with `Require: www.onvif.org/ver20/backchannel` (go2rtc
  does), `track2` PCMA/8000 `a=sendonly` = the ONVIF backchannel into the speaker. Each track has its
  own transport (TCP interleaved or UDP), aggregate PLAY/PAUSE. The engine starts with the first
  audio track set up (retried every 0.5 s while the chime holds the hardware) and stops with the last
  client. The SDK's PA control is off for CPU1 (`CONFIG_AUD_TRAS_DAC_PA_CTRL=n`: it would drive
  GPIO 5, a camera rail); CPU0 switches the real amplifier pin around start/stop.
- **One engine at a time, nothing lost**: while the voc engine runs, `/speak`, `spk tone|play|net`
  and the ring chime play through its speaker tap (PCM -> A-law, paced to real time; the far end is
  muted for that moment) and `/mic.wav` reads its microphone tap (an **8 kHz** WAV while a session
  is live, 16 kHz otherwise). The half-duplex "mute the mic while the speaker plays" rule only
  applies to the raw engine; with the AEC on, both directions run at once.
- Commands: `talk aec on|off` (KV `db_talk_aec`, default on, switches live), `talk vocmic N` (KV
  `db_voc_mic`, ADC gain for the voice engine, default 0x3f), `talk voc on|off` (start the engine by
  hand for bench tests), `talk status` shows `engine raw|voc` plus a `[voc]` line with frame counters;
  `rtsp status` gained an audio line (engine, clients, mic/backchannel counters) and per-track client
  state (`v:tcp a:tcp b:tcp`).
- Sleep: a connected RTSP client and a running voice engine count as busy.
- Measured: echo of a -12 dBFS 1 kHz tone in the mic track -10 dBFS with the AEC off, below the floor
  (-100 dBFS, broadband -37..-55 dBFS) with it on; first mic packet 225 ms after PLAY; go2rtc (HA
  2026.9.4) lists the producer as `video recvonly H264`, `audio recvonly PCMA/8000`, `audio sendonly
  PCMA/8000`. Heap at boot 87 KB (0.25.0: 97 KB; 10 KB of new rings/buffers).
- `tools/rtsp_backchannel.py`: stdlib RTSP client that does what go2rtc does (DESCRIBE with the
  Require header, SETUP mic + backchannel interleaved, PLAY, 20 ms G.711 frames from a tone or a WAV)
  and records the mic track with a Goertzel/rms analysis, for the echo-attenuation measurement.
- Simulator: the new `talk` subcommands and reply lines; the ffmpeg RTSP stand-in advertises a
  PCMA/8000 track (note: `ffmpeg -rtsp_flags listen` is broken in FFmpeg 8.0 on macOS; the stand-in
  needs an older ffmpeg).
- Home Assistant: `docs/homeassistant/` two-way audio notes (go2rtc view, the HA frontend is still
  listen-only) and an `Answer` action on the ring push.

## 0.25.0
- **Smart RTC schedule** (2026-10-04, `power.c`): `pwr sched DAY_START DAY_END DAY_MIN NIGHT_MIN`
  (KV `db_pwr_sched`, default `07:00 22:00 15 60`, on by default) picks the RTC wake interval from
  the local time (SNTP clock + `sd tz`, `app_time_local()` in sdcard.c) or, while the clock is
  unsynced, from the light sensor (dark = night, the `ring dark` threshold). Battery derating:
  < 30 % doubles the interval, < 15 % forces 180 min and the PIR wake off (`pwr status` shows
  `(FORCED OFF: battery)`). `pwr sched off` = the fixed `snap_min` of before. `pwr status` and
  `pwr sched` show the computed next interval and why; every report carries `sleep_min` and the
  effective `wake_pir`.
- **Policy pull** (`policy.c`): `pwr policy URL|auto|off|pull` (KV `db_pwr_policy`, default auto =
  `http://<hook host>:<hook port>/local/doorbell/policy.json`). Once per boot, right after the IP and
  before the boot report, the device GETs the file (3 s timeout, <= 1 KB) and applies the
  whitelisted keys `day_start day_end day_min night_min wake_pir pir_sens dark_mv volume night_auto
  keep_s sd_auto` through the same commands as the shell (persisting in KV), only where the value
  differs. Never sleep on/off, hook or token. One summary line in the log; the boot report carries
  `"policy":"applied|none|error"` and `"policy_rev"` (the file's `rev`). `pwr sched off` on the
  device wins over the schedule keys.
- `vol 0-100` (KV `db_vol`, default 100 = the clean maximum 0x1c): linear DAC gain for the chime
  and for talk with `gain auto`; `spk cfg gain N` is a raw override, `spk cfg gain auto` releases it.
- `GET /cmd?t=TOKEN&c=COMMAND` on :8080 runs the shared dispatcher (`app_exec()`, now also behind
  the TCP channel, serialised by a mutex) and returns the reply as text/plain; 403 without the right
  token or with no token set. `GET /log?t=TOKEN` streams the RAM log.
- RAM log: the firmware's own `os_printf` lines (every project/main file, via `applog.h`) go to an
  8 KB ring; `log [N<=600]`, `log stat`. The SDK's async log path has no output hook, so SDK-internal
  lines are UART-only. The TCP channel logs the peer address with every command (`[tcp] 192.168.2.66:
  sys info`): every command restarts the idle timer, so an unexpected poller keeps the device awake.
- Battery read reused for 2 s (`batt_adc_mv()`): a second ADC burst right after the first reads the
  cell ~160 mV low (the divider cannot recharge the sample capacitor), which made the schedule derate to
  "2 %" in the first 0.25.0 build. `db_reply()` buffer 300 -> 640 (the longer `pwr status` was cut).
- USB wake: GPIO 19 is configured as input with pull-down (and kept so through the sleep) before
  deep sleep, pull-disable again at boot. Unverified until a charger is at hand.
- Home Assistant: seven input helpers, `doorbell_policy.py` + `shell_command.doorbell_policy_write`
  (helpers -> policy.json with `rev` = timestamp), automation `doorbell_bk7258_policy`,
  `rest_command.doorbell_cmd` (URL with the token from `secrets.yaml`), `sensor.doorbell_bk7258_policy`,
  policy fields in the webhook fan-out and the boot logbook line, setting tiles on the dashboard.
- Simulator: `vol`, `log`, `pwr sched`, `pwr policy` (a real GET + apply), `/cmd`, `/log`, the new
  report fields; tests for `/cmd` auth, the volume mapping, the schedule and a policy pull.

## 0.24.4
- **microSD: the ring that wakes the device from deep sleep is saved** (2026-10-04). First real wake on
  0.24.3: the ring job ran at ~0.5 s uptime, before the media pipeline existed, the mount failed
  (`fail 1`) and the old "skip the card for 60 s after a failed mount" rule then dropped the next
  events (`drop 2`). Now automatic saves are deferred until uptime 3 s, a mount/frame failure is
  retried 3 times with 2/4/8 s backoff (jobs keep the event's tick: file name = event time, `up<N>`
  before SNTP), a fresh event during a pending retry joins it, and the 60 s skip only follows an
  exhausted chain. The same event twice within 2 s saves once. Every drop has a reason in `sd status`
  (`drop N (ring: duplicate +812 ms)`), plus `defer`/`retry`/`pending` counters.
- TCP command reply buffer 320 -> 640 bytes: `sd status` (3 lines) was cut off after line 2, the
  `bringup:` trace never reached `dbcmd.py`.

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
