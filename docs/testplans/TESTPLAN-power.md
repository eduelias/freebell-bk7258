# Test plan: battery, charger and deep sleep (feat/power, fw 0.19.0)

Feature file: `project/main/power.c` (+ `power.h`), hooks in `app_main.c` (`pwr` command, report JSON
fields, wake handling). Nothing sleeps unless `pwr sleep on` or `pwr sleepnow` is sent; both are OFF
after a fresh flash/OTA (KV `db_pwr_*` keys, all defaults safe).

## What was learned from the stock firmware (tuya_battery.c / tuya_pir.c, capstone on the logical dump)

| Signal | GPIO | Stock config | Polarity | Verified today |
|---|---|---|---|---|
| USB power present | 19 | input, floating (`tkl_gpio_init(19)` before "usb power gpio init failed") | high = USB plugged | pending (needs `pwr status` on the new image: expect `usb 1` on the charger) |
| Charger status | 15 | input, pull-up ("battery charge gpio init failed") | charging = GPIO15 high **and** GPIO19 high | 15 reads low with the pack at 4.02 V on USB = charge complete, consistent |
| Battery sense | ADC15 = GPIO 13 | tuya ADC ch 15, 5 samples, median, **battery mV = 2 x ADC mV**, valid 2501..4299 mV | | `adc 15` = 2012 mV -> **4024 mV** (ADC9/GPIO 7 = 2995 mV is a regulated rail, not the battery) |
| PIR enable / set0 / set1 | 50 / 26 / 25 | push-pull outputs driven **high** at init | | fw now drives 50 high at boot (stock behaviour) |
| Stock SoC % table (discharging) | | 4000:100 3900:80 3800:60 3700:40 3650:20 3600:10 3550:0; charging table ~+90 mV; shutdown at 3540 mV | | ours: finer curve 4150:100 .. 3400:0, -80 mV while charging |

Deep sleep on the BK7258: `bk_pm_sleep_mode_set(PM_MODE_DEEP_SLEEP)` arms it, the idle task enters
it once the BT/MAC/audio/video power domains are off (power.c waits for the drivers, then votes the
rest off itself). Wake = full SoC reset; the SDK reports `RESET_SOURCE_DEEPPS_GPIO` (+ the pin via
`bk_gpio_get_wakeup_gpio_id()`) or `RESET_SOURCE_DEEPPS_RTC`. All GPIOs are in the AON domain, so
button (53) and PIR (24) can both wake it (dynamic wake map, 4 slots: 53, 19, optional 24). The
SDK's static map also contains **GPIO 10 = UART0 RX (falling edge)**: typing anything on the serial
console wakes the device too (reported as `wake reason uart`). RAM is lost in deep sleep (counters
restart; settings are in KV).

## Commands (all over `python3 tools/dbcmd.py ...`, replies come back on the TCP channel)

| Command | Effect | KV | Default |
|---|---|---|---|
| `pwr status` | one line: battery mV / % / raw ADC, usb, charging, all settings, uptime, boot-hold left, idle time, busy flag, boot reason, wake reason | | |
| `pwr sleep on|off` | automatic sleep when on battery and idle. `off` also cancels a pending `sleepnow` | `db_pwr_sleep` | **off** |
| `pwr sleep force on|off` | also sleep while USB is present | `db_pwr_force` | off |
| `pwr idle S` | idle seconds before sleeping (min 20) | `db_pwr_idle` | 60 |
| `pwr wake_pir on|off` | PIR (GPIO 24 rising) is a wake source; GPIO 50 is held high through the sleep | `db_pwr_wpir` | off |
| `pwr snap_min M` | RTC wake every M minutes for a scheduled snapshot (0 = only the 360 min keepalive wake) | `db_pwr_snap` | 0 |
| `pwr sleepnow` | sleep in 10 s with the configured wake sources, regardless of USB/idle/boot hold (explicit test) | | |
| `pwr chan N`, `pwr ratio PCT` | battery ADC channel / divider ratio x0.01 (calibration only) | `db_pwr_bchan`, `db_pwr_bratio` | 15, 200 |

Report JSON gains `"batt_mv","batt_pct","usb","charging"` on every event, and a new event
`"wake"` with `"reason":"button|pir|usb|uart|rtc|gpioNN"` after every deep-sleep wake (sent as soon as
the IP arrives; `ring`/`motion` are now also held until the IP arrives instead of being dropped).

Policy when `pwr sleep on`: never before 120 s of uptime (every boot, including wakes), never while
USB is present (unless force), never while busy (HTTP client incl. HA's MJPEG stream, speaker, mic,
ring effects, OTA, a report waiting for Wi-Fi), and only after `idle` seconds without a command,
HTTP request, press or motion. Before sleeping: amp off, LEDs off, camera closed, GPIO 28/5/6/9 low,
`bk_wifi_sta_stop()`, GPIO 46 held low through the sleep. If the deep sleep does not happen within
10 s the firmware backs out, reconnects Wi-Fi and refuses automatic sleep for 10 min. If the boot
reason is a crash/watchdog, automatic sleep is inhibited for that boot (`pwr status` shows
`INHIBITED crash boot`) so a bug in the sleep path cannot become a reboot loop on battery.

## Recovery (read before step 3)

- A sleeping device wakes on: button press, USB plug-in (rising edge on GPIO 19), any byte on the
  UART0 console (GPIO 10), the RTC timer (`snap_min`, else 360 min). So the normal recovery is
  **press the button** or **plug USB**.
- If it does not come back at all: battery **and** USB out for 5 s, battery in, USB in. With the
  FTDI connected, watch `python3 tools/serial_capture.py 115200 60 boot.bin` for the banner and the
  `[pwr] boot:` line. `tools/bkwrite.py --kick` cannot reach the ROM while the chip is asleep; after
  a power cycle the normal `scripts/flash.sh --write --kick` path works again.
- Disable everything again: `pwr sleep off` (sticks immediately, also within the 120 s boot hold),
  then `pwr snap_min 0`, `pwr wake_pir off`, `pwr sleep force off`. Rollback = OTA of the previous
  image (`doorbell-fw_0.18.5_app_pack.rbl` is still on HA); the `db_pwr_*` KV keys are ignored by it.
- The owner is needed only for step 6 (unplug USB) and for a power cycle if a wake fails.

## Steps (least risky first)

Observe with: `dbcmd.py` replies, UART (`tools/serial_capture.py`), HA `input_text.doorbell_bk7258_last_report`
and the new sensors (after the HA changes below), the laptop camera for LEDs / laptop mic for the chime.

1. **Battery & charger readout (no risk).** `pwr status` -> expect `batt ~4020 mV ~9x% (adc15 ~2012 mV x2.00) usb 1 chg 0 | sleep off ... | boot ... `.
   `hook test` -> HA last report contains `"batt_mv":40xx,"batt_pct":..,"usb":1,"charging":0`.
   Cross-check: `adc 15` x 2 = batt_mv. If `usb` reads 0 on the charger, GPIO 19 polarity is wrong:
   stop here and report (sleep policy depends on it).
2. **Settings persist.** `pwr idle 45`, `pwr snap_min 1`, `pwr status`, `reboot` (or OTA), `pwr status` again -> same values, `sleep off`.
3. **RTC wake, USB plugged, 1 minute (first sleep).** `pwr snap_min 1` then `pwr sleepnow`.
   Expected: reply `[pwr] sleeping in 10 s ...`; UART: `[pwr] sleeping (sleepnow)`, `[pwr] camera close -> 0`
   (if it was open), `[pwr] entering deep sleep`, then silence; the device drops off the LAN
   (`ping 192.168.2.89` stops). ~60 s later it reboots: banner, `[pwr] boot: deepsleep rtc wake=rtc`,
   Wi-Fi joins, HA receives `boot` and `wake` (`reason: rtc`, battery fields). Because `sleep` is
   still off it stays awake. Measure the sleep-to-IP time (target < 10 s).
   If UART shows `[pwr] deep sleep did not happen` (and which module stayed powered), the device
   stays up: collect the line and stop; nothing else to recover.
4. **Button wake + chime before Wi-Fi.** `pwr snap_min 30`, `pwr sleepnow`, wait for silence, press
   the button. Expected: chime + blue ring within ~2 s of the press (UART: `[pwr] boot: deepsleep gpio
   wake=button`, then `[fx] ring`), HA gets `wake reason button` **and** `ring` (notify.doorbell_family
   fires) once the IP is up. A laptop mic can time the chime vs the press.
5. **USB-plug wake and console wake.** `pwr sleepnow`; after silence unplug and replug USB -> boot
   with `wake=usb`. Again `pwr sleepnow`, then press Enter on the serial console -> `wake=uart`.
   (Both are the recovery paths; confirm them before testing on battery.)
6. **Automatic sleep on battery (owner unplugs USB).** `pwr idle 30`, `pwr sleep on`, `pwr status`
   (expect `hold N s` counting down after a fresh boot). Unplug USB. Expected: `usb 0` in `pwr status`;
   30 s after the last command/HTTP request (and after the boot hold) UART shows `[pwr] sleeping (idle)`
   and the device sleeps. HA's generic camera: if the HA stream is open (`cam stat` clients > 0) the
   device will **not** sleep (`busy 1`) - that is by design; disable the HA camera stream preload or
   wait for HA to drop it. Press the button -> chime, wake, ring report, awake 120 s + idle, sleeps again.
   Plug USB back in -> wakes (`usb`), stays awake while USB present.
7. **PIR wake.** `pwr wake_pir on`, `pwr sleepnow`, after silence wave at the PIR -> `wake=pir` +
   `motion` report. Note the false-wake rate overnight (PIR showed spontaneous pulses on 10-02); if it
   is too high leave `wake_pir off`.
8. **Scheduled snapshot via HA.** With the HA automation change below: `pwr snap_min 15`, `pwr sleep on`,
   on battery. Every ~15 min: `wake reason rtc` -> HA `camera.snapshot` writes `/config/www/doorbell/wake_latest.jpg`
   (http://192.168.2.80:8123/local/doorbell/wake_latest.jpg), device sleeps again ~2.5 min later.
   Each cycle costs ~2.5 min awake (120 s boot hold + idle), so 15 min is the sensible minimum.
9. **Battery curve sanity.** Leave it on battery for a few hours with `snap_min 30`; the `batt_mv`
   series in HA should fall slowly and monotonically; `charging` must go to 1 when USB returns with a
   partially drained pack (this is the one polarity not yet seen: if it stays 0 while clearly
   charging, GPIO 15 is active-low and `charging()` in power.c needs inverting).

## Home Assistant changes (apply on the HA box, then `ha core check`)

### 1. `/config/automations.yaml`, automation `doorbell_bk7258_webhook`: add the event fan-out and the wake branch

Insert as the **first** action (before `input_text.set_value`):

```yaml
  - event: doorbell_bk7258_report
    event_data:
      event: "{{ trigger.json.event }}"
      reason: "{{ trigger.json.reason | default('') }}"
      fw: "{{ trigger.json.fw | default('') }}"
      ip: "{{ trigger.json.ip | default('') }}"
      uptime_s: "{{ trigger.json.uptime_s | default(0) }}"
      light_mv: "{{ trigger.json.light_mv | default(0) }}"
      batt_mv: "{{ trigger.json.batt_mv | default(none) }}"
      batt_pct: "{{ trigger.json.batt_pct | default(none) }}"
      usb: "{{ trigger.json.usb | default(none) }}"
      charging: "{{ trigger.json.charging | default(none) }}"
```

Add to the `choose:` list (next to the `ring`/`motion`/`boot` branches):

```yaml
    - conditions:
      - condition: template
        value_template: '{{ trigger.json.event == "wake" }}'
      sequence:
      - action: logbook.log
        data:
          name: Doorbell BK7258
          message: woke up ({{ trigger.json.reason }}), battery {{ trigger.json.batt_pct | default("?") }} % / {{ trigger.json.batt_mv | default("?") }} mV
      - if:
        - condition: template
          value_template: '{{ trigger.json.reason == "rtc" }}'
        then:
        - delay: "00:00:03"
        - action: camera.snapshot
          target:
            entity_id: camera.doorbell_bk7258
          data:
            filename: /config/www/doorbell/wake_latest.jpg
          continue_on_error: true
```

Then `ha core check` and Developer tools -> YAML -> Automations (or `automation.reload`).

### 2. `/config/packages/doorbell_bk7258.yaml`: append sensors + low-battery alert

```yaml
template:
  - trigger:
      - trigger: event
        event_type: doorbell_bk7258_report
    sensor:
      - name: Doorbell BK7258 battery
        unique_id: doorbell_bk7258_battery_pct
        unit_of_measurement: "%"
        device_class: battery
        state_class: measurement
        state: "{{ trigger.event.data.batt_pct if trigger.event.data.batt_pct is not none else this.state }}"
      - name: Doorbell BK7258 battery voltage
        unique_id: doorbell_bk7258_battery_mv
        unit_of_measurement: mV
        device_class: voltage
        state_class: measurement
        state: "{{ trigger.event.data.batt_mv if trigger.event.data.batt_mv is not none else this.state }}"
      - name: Doorbell BK7258 light level
        unique_id: doorbell_bk7258_light_mv
        unit_of_measurement: mV
        state_class: measurement
        state: "{{ trigger.event.data.light_mv }}"
      - name: Doorbell BK7258 last event
        unique_id: doorbell_bk7258_last_event
        state: "{{ trigger.event.data.event }}"
        attributes:
          reason: "{{ trigger.event.data.reason }}"
          fw: "{{ trigger.event.data.fw }}"
          ip: "{{ trigger.event.data.ip }}"
          uptime_s: "{{ trigger.event.data.uptime_s }}"
    binary_sensor:
      - name: Doorbell BK7258 USB power
        unique_id: doorbell_bk7258_usb
        device_class: plug
        state: "{{ (trigger.event.data.usb | int(0)) == 1 if trigger.event.data.usb is not none else this.state == 'on' }}"
      - name: Doorbell BK7258 charging
        unique_id: doorbell_bk7258_charging
        device_class: battery_charging
        state: "{{ (trigger.event.data.charging | int(0)) == 1 if trigger.event.data.charging is not none else this.state == 'on' }}"

automation:
  - id: doorbell_bk7258_low_battery
    alias: "Doorbell BK7258: low battery"
    triggers:
      - trigger: numeric_state
        entity_id: sensor.doorbell_bk7258_battery
        below: 15
        for: "00:10:00"
    conditions:
      - condition: state
        entity_id: binary_sensor.doorbell_bk7258_usb_power
        state: "off"
    actions:
      - action: notify.doorbell_family
        data:
          title: Doorbell
          message: "Doorbell battery {{ states('sensor.doorbell_bk7258_battery') }} % - charge it"
    mode: single
```

`template:` and `automation:` in a package need a full `ha core restart` the first time (new
platforms); afterwards `template.reload` / `automation.reload` suffice. `/config/www` is in HA's
default `allowlist_external_dirs`, so `camera.snapshot` can write there.

## Risks / things to watch

- GPIO 19 / 15 polarity is taken from the stock code; step 1 (usb=1 on the charger) and step 9
  (charging=1 with a drained pack) confirm it. Until then `pwr sleep on` must not be left enabled
  unattended, because `usb` wrong = sleeping on USB / never sleeping on battery.
- The idle task must actually enter deep sleep: the four power domains (BT, MAC, audio, video) must
  be off. The firmware votes them off itself if the drivers did not; if `deep sleep did not happen`
  appears, the UART line names the module and the device just stays awake.
- HA keeps the MJPEG stream open on the generic camera -> `busy 1` forever -> no automatic sleep.
  Either accept (USB-powered install) or let HA fetch stills only.
- `pwr sleepnow` during a speaker/mic session leaves the audio domain on -> sleep fails -> 10 min backoff.
- UART RX is a wake source: a floating console header could cause spurious wakes (the SDK pulls it up).

## Results 2026-10-03 (integrator, fw 0.21.0 -> 0.21.1 on main, UART0 dead so everything via dbcmd/HA)

| Step | Result |
|---|---|
| 1 battery/charger readout | `pwr status`: `batt 3886 mV 72% (adc15 1943 mV x2.00) usb 1 chg 0`; `hook test` -> HA last report had `batt_mv 3888, batt_pct 72, usb 1, charging 0`; HA sensors populated through the event fan-out. **But** GPIO 19 follows the internal pull (`pin in 19 down` -> usb 0, `up`/`none` -> usb 1): the pin may be floating, i.e. USB may not be plugged at all. GPIO 15 reads 0 with pull-down, none and pull-up = externally driven low (consistent with "not charging"). The pack read 4024 mV at 16:00 (implementer) and 3886 mV at 19:20 after 3 h of HA camera streaming, then flat for 20 min with the camera closed. `adc 15` on its own printed 2006 mV while `pwr status` (median of 3, back-to-back) printed 1943: the readings are bimodal (see caveat in README). Charging polarity still unobserved. |
| 2 persist | `pwr idle 45`, `pwr snap_min 1`, `reboot` -> both values back after the reboot. OK |
| 3 RTC wake 1 min | `pwr sleepnow` 19:21:41 -> off the LAN 19:21:54 (+12 s), back 19:22:54 (+72 s = 60 s sleep + ~2 s boot-to-IP), `pwr status`: `boot deepsleep rtc wake=rtc`. HA: `boot` 17:22:53.658Z, `wake reason rtc` .798Z (140 ms later), logbook "woke up (rtc), battery 72 % / 3886 mV", `wake_latest.jpg` (74 KB) written 19:22:58. **PASS** |
| 4 button wake + chime | needs the owner (device left cycling, see below) |
| 5 USB-plug / console wake | needs the owner (USB); console RX wire is dead |
| 6 automatic sleep | tested with `pwr sleep force on`, `idle 20`, `snap_min 10` (HA camera idle, `busy 0`): see the timeline below |
| 7 PIR wake | `wake_pir on`, `pir sens 2` (bare board self-triggers ~6/min awake), `snap_min 2`, `sleepnow` 19:24:08 -> off 19:24:20, back **19:26:22 by RTC**, `wake=rtc`, GPIO24 already pulsing again 5 s after boot. Rail check while awake: edges/min with GPIO 9 low 4, GPIO 28 low 5, GPIO 5+6 low 5 (baseline 6) -> the PIR stays powered through the sleep; `bk_gpio_register_wakeup_source` cannot fail here (4 slots, 3 used). Conclusion: the awake "self-triggering" is most likely EMI from the running board and the PIR is quiet while the SoC sleeps; a human wave is needed. v0.21.1 additionally holds GPIO 25/26 (sensitivity bits) through the sleep. **UNPROVEN** |
| 8 scheduled snapshot | HA half proven in step 3 (`wake_latest.jpg` +5 s). Device half = step 6 cycle. |
| 9 battery curve | not started (needs hours on battery + a drained pack for `charging 1`) |

Timeline of the unattended cycle (`sleep on`, `force on`, `idle 20`, `snap_min 10`, `wake_pir on`,
hook on HA, started 19:36:39): filled in below.
- 19:36:14 boot (OTA 0.21.1), 19:36:39 settings applied, **19:38:16 idle sleep** (= end of boot hold, last activity 19:37:06), **19:48:16 RTC wake** (600 s), HA `boot` + `wake rtc` at 19:48:16, `wake_latest.jpg` 19:48:21, **19:50:16 idle sleep** again (hold 120 s + the HA snapshot/pwr status activity at 19:48:2x -> idle 20 s reached before the hold ended). Each cycle: 2 min awake / 10 min asleep, like clockwork.
- 20:00:17 RTC wake #2 (HA logged boot + wake), 20:02:19 idle sleep #3, 20:12:19 RTC wake #3. Three
  full cycles, every edge within 1-4 s of the prediction, battery flat at 3884-3890 mV throughout.
  No owner action happened in the window (button / PIR / USB wake still unproven), so at 20:12:29
  (inside the boot hold) everything was switched back: `sleep off`, `force off`, `wake_pir off`,
  `idle 60`, `snap_min 0`. Device left awake on 0.21.1 with the defaults.
