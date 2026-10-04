# Battery, charger and deep sleep (v0.21 - v0.25, `project/main/power.c`)

## Pins (from the shipped firmware's battery code)

**GPIO 19 = USB present (high)**, **GPIO 15 = charging (high, together with 19 high)**,
**battery = ADC15 on GPIO 13, x2 divider** (ADC9 on GPIO 7 is a regulated rail, not the battery).
GPIO 13 is not driven by default (0.24.2 briefly treated it as the SD power enable; it is not, see
sdcard.md). If `sd pwr 13` is ever set, `power_batt_mv()` returns the last valid median while the
pin is driven and for ~10 s after it is released, and `pwr status` shows `(cached: sd power)`. Before
deep sleep `power_do_sleep()` -> `app_power_prepare_sleep()` -> `sd_prepare_sleep()` unmounts the card.
Deep sleep uses the SDK's pm v2 (SoC reset on wake) and is **off by default**.

## Settings

| Command | What it does | KV, default |
|---|---|---|
| `pwr status` | batt mV / % (raw ADC x ratio), usb, chg, all settings, uptime, boot-hold left, idle s, busy, boot/wake reason | |
| `pwr sleep on\|off` | automatic sleep when on battery and idle; `off` also cancels a pending `sleepnow` | `db_pwr_sleep`, off |
| `pwr sleep force on\|off` | also sleep while USB is present | `db_pwr_force`, off |
| `pwr idle S` | idle seconds before sleeping (min 20) | `db_pwr_idle`, 60 |
| `pwr wake_pir on\|off` | PIR (GPIO 24 rising) wakes; GPIO 50/25/26 held through the sleep | `db_pwr_wpir`, off |
| `pwr snap_min M` | RTC wake every M min (0 = 360 min keepalive only); only used with `pwr sched off` | `db_pwr_snap`, 0 |
| `pwr sched START END DAY NIGHT` / `sched off` / `sched on` / `sched` | day/night RTC intervals (0.25, below) | `db_pwr_sched`, `07:00 22:00 15 60` |
| `pwr policy URL\|auto\|off\|pull` | boot-time settings pull from HA (0.25, below; `policy.c`) | `db_pwr_policy`, auto |
| `pwr sleepnow` | sleep in 10 s regardless of USB/idle/boot hold (test) | |
| `pwr chan N` / `pwr ratio PCT` | battery ADC channel / divider x0.01 | `db_pwr_bchan` 15, `db_pwr_bratio` 200 |

Report JSON carries `batt_mv`, `batt_pct`, `usb`, `charging`, `sleep_min` (the interval the next
sleep will arm) and `wake_pir` (effective); the event `wake` with `reason`
(`button|pir|usb|uart|rtc|gpioNN`) follows every deep-sleep wake (right after the `boot` report).

## Smart RTC schedule (0.25.0)

`power_wake_interval()` decides the RTC alarm at every sleep (and is shown by `pwr status` /
`pwr sched` as `next N min (why)`):

1. `pwr sched off`: the fixed `snap_min` (0 = 360 min keepalive), "fixed snap_min".
2. Otherwise local time from the SNTP clock + `sd tz` (`app_time_local()` in sdcard.c): inside
   `DAY_START <= t < DAY_END` (a window across midnight works) the day interval, else the night one;
   "day/night by clock HH:MM".
3. Clock unsynced (no NTP yet, e.g. the first boot after a power loss): the light sensor decides with
   the `ring dark` threshold, "day/night by light sensor (clock unsynced)". Reading it toggles GPIO 28
   for ~80 ms when the camera is closed.
4. Battery derating (both modes): < 30 % doubles the interval; < 15 % forces at least 180 min **and
   the PIR wake off** for that sleep (`pwr status`: `wake_pir on (FORCED OFF: battery)`, JSON
   `wake_pir: 0`). Clamped to 1..1440 min.

Defaults 07:00-22:00, 15 min by day, 60 by night = about 70 wakes a day; each wake is ~3 min awake
(120 s boot hold + 60 s idle), so the schedule is the main battery knob. Home Assistant sets the
two intervals through the policy file (`input_number.doorbell_day_interval` / `_night_interval`);
`pwr sched off` on the device is respected by the policy (the schedule keys are skipped).

## Policy pull (0.25.0, `policy.c`)

Once per boot, right after the IP and before the boot report, the device GETs a flat JSON object
(3 s timeout, <= 1 KB body, IPv4 literal host) from `pwr policy`'s URL: `auto` (default) =
`http://<hook host>:<hook port>/local/doorbell/policy.json`, i.e. a file in HA's `/config/www/doorbell/`.
Whitelisted keys and the command each one goes through (so the value persists in KV exactly as if
typed): `volume` -> `vol`, `wake_pir` -> `pwr wake_pir`, `pir_sens` (0-2) -> `pir sens`, `dark_mv`
-> `ring dark`, `night_auto` -> `night auto`, `keep_s` -> `sd keep`, `sd_auto` -> `sd auto`,
`day_start day_end day_min night_min` -> one merged `pwr sched`. A key is applied only when it
differs from the current setting (no flash write otherwise); `sleep`, `hook`, `token` and anything
else in the file are ignored. `rev` (string or number) is remembered and reported. Log line:
`[policy] rev 1791130801: 2 changed 6 same | volume=60 sched=...`; `pwr policy` shows the URL and the
last result; `pwr policy pull` refetches now (HA's automation calls it over `GET /cmd` when a helper
changes, which only works while the device is awake). The boot report carries `policy` =
`applied` (200 + JSON object, even with nothing to change) / `none` (off, or no hook) / `error`
(connect failed, HTTP != 200, no JSON) and `policy_rev`.

Format of `policy.json` as written by `docs/homeassistant/doorbell_policy.py`:

```json
{"rev":1791130801,"day_start":"07:00","day_end":"22:00","day_min":15,"night_min":60,
 "wake_pir":1,"pir_sens":0,"dark_mv":1400,"volume":100,"night_auto":1}
```

Wake sources: button (GPIO 53, falling), USB plug (19, rising; since 0.25.0 the pin is an input
with pull-down through the sleep so a floating pin cannot look "present" and the plug is a clean
rising edge; pull-disable again at boot), console RX (10, SDK static map), PIR (24, rising, optional,
forced off below 15 % battery), RTC alarm (interval from the schedule above).

Policy for automatic sleep: never before 120 s uptime, never on USB unless `force`, never while
busy (an HTTP client including HA's stream, spk/mic, ring effects, OTA, a pending report), only
after `idle` seconds without a command, HTTP request, press or motion. Before sleeping: amplifier
off, LEDs off, camera closed, GPIO 28/5/6/9 low, `bk_wifi_sta_stop`. A crash/watchdog boot inhibits
automatic sleep; a sleep that does not happen backs out with a 10 min backoff.

## Verified on hardware (v0.21.0/0.21.1)

`pwr sleepnow` + `snap_min 1`: off the LAN 12 s after the command, back 60 s later, boot reason
`deepsleep rtc`, Wi-Fi + IP within about 2 s of the reset, HA received `boot` and `wake reason rtc`
140 ms apart and wrote `wake_latest.jpg` 5 s after the wake. Three unattended sleep/RTC-wake cycles
in a row. Settings survive a reboot. The HA sensors (`sensor.doorbell_bk7258_battery`,
`_battery_voltage`, `_light_level`, `_last_event`, `binary_sensor.doorbell_bk7258_usb_power`,
`_charging`) follow every report through the `doorbell_bk7258_report` event. Automatic (idle) sleep
was tested with `force on`. Log: [`../testplans/TESTPLAN-power.md`](../testplans/TESTPLAN-power.md).

## Caveats and open items

1. **ADC readings are bimodal**: `mv` (ADC9) alternates between about 2994 and 3069 and `batt_mv`
   between about 3886 and 3982 (2.5 %, same direction, within one report sequence), most likely a
   ratiometric ADC following supply sag under Wi-Fi/camera load. Calibrate `pwr ratio` against a
   multimeter before trusting the percentage. Worse (found 2026-10-04 with 0.25.0): a second burst
   of three reads right after the first comes back ~160 mV lower on the cell (1903 -> ~1743 mV at
   the pin; `pwr chan 15` printed "1903 mV x2.00 = 3486 mV"), the divider cannot recharge the
   SARADC sample capacitor that fast. `batt_adc_mv()` therefore reuses one median for 2 s, so a status
   line, a report and the sleep decision all see the same value.
2. **USB detect polarity unconfirmed**: GPIO 19 follows the internal pull (`pin in 19 down` -> usb
   0, `up`/`none` -> 1), so `usb 1` on the bench may be a floating pin rather than a charger. It
   needs to be seen with and without USB on a device with the charger board connected. The 0.25.0
   pull-down-through-sleep (`power_do_sleep()`) is likewise **unverified** until a charger is at
   hand; if a USB plug no longer wakes the device, that change is the first suspect.
3. **Charging polarity unconfirmed** for the same reason (GPIO 15).
4. **PIR wake unproven**: see [pir.md](pir.md). GPIO 50 is owned by `pir.c` (`pir off` keeps it
   low); `power.c` only holds it high through the sleep when `wake_pir` is on.
5. **Button and USB wake** not yet exercised end to end (the RTC path is).
