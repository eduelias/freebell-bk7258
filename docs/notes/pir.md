# PIR motion sensor (v0.20, `src/pir.c`)

## Pins (from the shipped firmware's PIR code)

GPIO 24 = PIR output (rising-edge interrupt), GPIO 50 = PIR enable (the shipped firmware drives it
high and never lowers it), GPIO 25 = sensitivity bit 0, GPIO 26 = sensitivity bit 1
(`set_sens(n)`: 25 = n & 1, 26 = n >> 1 & 1; shipped default n = 1, exposed values 0/1/2).

## Behaviour

- `pir on`: GPIO 50 high + interrupt enabled, edges blanked for 5 s (warm-up). `pir off`:
  interrupt off, GPIO 50 low. KV `db_pir_on`, default on.
- `pir sens 0-3`: the two sensitivity bits. KV `db_pir_sens`, default 1.
- `pir cooldown MS`: minimum gap between accepted motions, enforced in the ISR. KV `db_pir_cd`,
  default 10000 (the shipped firmware used 30 s).
- `motion test`: a simulated edge through the ISR handler (counts, cooldown, `motion` report).
- `pir status` prints on/off, sens and the GPIO 25/26 levels, cooldown, count, ms since the last
  motion, raw/ignored edges, live GPIO 24/50 and `missed=N` (see
  [gpio-irq.md](gpio-irq.md)). Reports carry `pir_on` and `pir_sens` next to `light_mv`.

## Sensitivity on the bench

Bare board on a desk, nobody in the room, camera closed, 150 s per setting, counting
self-triggers: `sens 0` 0 pulses, `sens 1` 2, `sens 3` 12, `sens 2` 15 (with the camera open
`sens 1` went up to 10 per 2 min). So 0 is the least and 2 the most sensitive; the shipped firmware
exposed 0/1/2. `sens 0` still detects a person: waving over the board at about 50 cm gave 4 edges in
a minute, each one a push with a picture. While the board lies open on a desk `pir sens 0` is the
sane setting; in the enclosure, pointed at a doorstep, the code default (1) and the 10 s cooldown
are the only thing between you and a picture push per cooldown.

Caveat for deep-sleep wake: the PIR keeps pulsing with the other rails down, so it is powered
through the sleep and the wake-source registration succeeds, but a 2 min sleep at `sens 2` produced
no PIR wake although the bare board self-triggers every ~10 s while awake. That noise is probably
EMI from the running board; a real PIR wake needs a person in front of the sensor and is still on
the to-do list ([power.md](power.md)).

## Home Assistant side timings

With the webhook automation in `docs/homeassistant/`: `ring` -> persistent notification + text
push (tag `doorbell_ring`), then `camera.snapshot` to `/config/www/doorbell/last_ring.jpg` and a
second push with `image: /local/doorbell/last_ring.jpg?v=<ts>` (tag `doorbell_ring_photo`);
`motion` -> persistent notification (light_mv, pir_sens), snapshot to `last_motion.jpg`, one push
with the image (channel/tag `doorbell_motion`). All snapshot/notify steps have
`continue_on_error`. Measured: snapshot written 1.6 s after the webhook (5.7 s for the first one
after a boot), notify step done at +1.9 s. Procedure and results:
[`../testplans/TESTPLAN-motion.md`](../testplans/TESTPLAN-motion.md).

## Historical: the camera close/reopen wedge

During the motion tests (v0.20.x) a `cam close` while Home Assistant polled the camera wedged the
pipeline (stale JPEG, `media_app_camera_open` -1, HTTP connections closed at once, reboot-only).
Fixed in v0.22.1-0.22.3 by power-cycling the camera rails on close; see
[camera.md](camera.md#camera-power-cycle-on-close-v0221-0223).
