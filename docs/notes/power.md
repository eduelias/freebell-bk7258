# Battery, charger and deep sleep (v0.21, `project/main/power.c`)

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
| `pwr snap_min M` | RTC wake every M min (0 = 360 min keepalive only) | `db_pwr_snap`, 0 |
| `pwr sleepnow` | sleep in 10 s regardless of USB/idle/boot hold (test) | |
| `pwr chan N` / `pwr ratio PCT` | battery ADC channel / divider x0.01 | `db_pwr_bchan` 15, `db_pwr_bratio` 200 |

Report JSON carries `batt_mv`, `batt_pct`, `usb`, `charging`; the event `wake` with `reason`
(`button|pir|usb|uart|rtc|gpioNN`) follows every deep-sleep wake (right after the `boot` report).

Wake sources: button (GPIO 53, falling), USB plug (19, rising), console RX (10, SDK static map),
PIR (24, rising, optional), RTC alarm.

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
   multimeter before trusting the percentage.
2. **USB detect polarity unconfirmed**: GPIO 19 follows the internal pull (`pin in 19 down` -> usb
   0, `up`/`none` -> 1), so `usb 1` on the bench may be a floating pin rather than a charger. It
   needs to be seen with and without USB on a device with the charger board connected.
3. **Charging polarity unconfirmed** for the same reason (GPIO 15).
4. **PIR wake unproven**: see [pir.md](pir.md). GPIO 50 is owned by `pir.c` (`pir off` keeps it
   low); `power.c` only holds it high through the sleep when `wake_pir` is on.
5. **Button and USB wake** not yet exercised end to end (the RTC path is).
