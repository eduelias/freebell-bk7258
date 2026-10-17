# Night light, ring indicator, light sensor (v0.18-v0.19, `src/night.c`)

## Light sensor

A 3-pin part by the camera flex, powered from the camera rail (GPIO 28, held on at boot since
v0.18.x), read on **ADC14 = GPIO 12**. About 700 mV in a lit room, about 2100 mV covered: higher =
darker. `ring dark MV` sets the threshold above which the firmware considers it dark (KV
`db_dark_mv`, code default 1000 mV; `ring chan ADC` picks another channel, KV `db_light_chan`).
Reports carry `light_mv`.

Testing without touching the sensor: `ring dark 800` makes a lit room (about 900 mV) count as dark;
set it back afterwards.

## White LED arbiter (GPIO 44)

The white fill LED (the board's "ir led" in the shipped firmware's naming, but it is pure white) is
on while any source holds it:

- `CAM`: camera open and the light sensor reads dark, sampled every 20 s, hysteresis `night hyst`
  = 400 mV (KV `db_night_hyst`) and a 60 s minimum on-time so the LED's own reflection cannot make
  it flap; released when light returns or the camera closes.
- `MOTION`: 20 s after a motion event in the dark, so Home Assistant's snapshot is usable.
- `FX`: 30 s on a button press in the dark (part of the ring effects, `ring fx on|off`).

`led white on|off` is a manual override, `led white auto` releases it. `night auto on|off` (KV
`db_night_auto`, default on) turns the automatic sources off.

## Blue ring (GPIO 45)

The four blue LEDs around the button (the night locator light). One short blink at boot, a slow
double blink every 3 s while there is no IP, off once connected. The 8 blinks of a press always
win. `led ring on|off|auto` overrides / releases; `night ring on|off` (KV `db_night_ring`, default
on) silences the pattern.

GPIO 45 was first thought to be the IR illuminator; a camera showed the blue ring instead. The IR
LED above the camera lens is still unmapped, and with the white night light it is not needed.

## Verified on hardware (v0.19.0)

Camera night light on at the next 20 s sample, released exactly 60 s after it came on once the
light is back, off at once on `cam close`; motion light 20 s and extended by a second motion; press
light 30 s; the LED's own reflection lowers the sensor by only about 40 mV in a lit room (400 mV
hysteresis is ample); heap flat at 126-129 KB; the boot blink is visible on a camera.
`night status` prints all of the above in one line; `sys info` prints fw, uptime, ip, free heap,
reset reason, light and battery mV. Procedure:
[`../testplans/TESTPLAN-polish.md`](../testplans/TESTPLAN-polish.md).
