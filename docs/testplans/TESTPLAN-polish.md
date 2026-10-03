# Test plan: polish (night light, indicator patterns, amp pop, `sys info`) — branch `feat/polish`

Firmware: `project/main/night.c` + hooks in `app_main.c` (built as v0.19.0 on this branch; the
integrator may renumber). Nothing here changes the partition table, Wi-Fi, OTA or the camera path.

All commands below are `python3 tools/dbcmd.py <command>` from the repo
(Python 3.11+), unless marked UART. Replies are the `REPLY()` text.

## 0. What changed (for the reviewer)

| Area | Before (v0.18.5) | After |
|---|---|---|
| Speaker PA (GPIO 46) | PA on before the DAC/DMA started; DAC stopped with the PA still on; first two DMA halves = heap garbage | DMA buffer zeroed; DAC+DMA run on zeros 50 ms, then PA on, 50 ms, then audio; at the end PA off on zeros, 50 ms, then DAC stop |
| White LED (GPIO 44) | driven directly by `led white` and `ringfx_task` | owned by the arbiter in night.c: ON = any of sources CAM (streaming + dark), MOTION (20 s), FX (press, 30 s); `led white on/off` = manual override, `led white auto` releases |
| Blue ring (GPIO 45) | only `led ring` and the 8 press blinks | + boot blink (150 ms) and a double blink every 3 s while there is no IP; off when connected; press blinks still win; `led ring on/off/auto`; `night ring off` silences the pattern |
| New commands | — | `night auto on\|off`, `night ring on\|off`, `night hyst MV`, `night status`, `sys info` |
| New KV keys | — | `db_night_auto` (default 1), `db_night_ring` (default 1), `db_night_hyst` (default 400 mV) |

Defaults are safe: nothing sleeps, nothing can lock the owner out; a missing/garbled KV value falls
back to the defaults above.

## 1. Flash / OTA

```bash
scripts/release.sh --no-build      # publishes doorbell-fw_latest_app_pack.rbl to HA and OTAs it
```

Expected within ~45 s: UART `==== doorbell-bk7258-fw 0.19.0 ...`, then `[night] auto 1 ring 1 hyst 400 mV`.
On the device: **one short blue blink** right at boot, then a slow **double blink** until Wi-Fi has
an IP (normally 1-3 s), then the ring stays off. HA receives the usual `boot` webhook.

## 2. `sys info`

```bash
python3 tools/dbcmd.py sys info
```

Expected: `[sys] fw 0.19.0 uptime 73s (0h01m) ip 192.168.2.89 wifi 1 heap NNNNN B reset 0x18 (ota) light ~700 mV batt ~2990 mV cam 0`.
`reset` after an OTA is `0x18 (ota)` or `0x1 (reboot)`; after `reboot` it is `0x10 (nmi-wdt)` or `0x1`;
a cold power-up gives `0x0 (power-on)`. `heap` should be stable across repeated calls (no leak:
call it 10 times, the value may wobble by a few hundred bytes but not trend down).
Note: `light` toggles GPIO 28 for ~80 ms when the camera is closed (same as `ring`).

## 3. Amplifier pop

Rig: laptop microphone (ffmpeg avfoundation), device ~30 cm from the laptop.

```bash
# Terminal.app tab:
ffmpeg -y -f avfoundation -i ':1' -t 8 -ac 1 -ar 16000 pop_v0190.wav
# Mac, 1 s later:
python3 tools/dbcmd.py spk play bell
```

Expected: the bell with **no click** before it starts and none after it ends. UART shows
`[spk] done, 33217 samples, N dma frames, 0 underruns` (underruns must stay 0: the pre-roll is no
longer counted, the count only covers the audio itself).
Compare with a recording of the same command on v0.18.5 if one exists
(`tools/mac_ear.py` can show the envelope: the old build has a spike ~50 ms before the chime).
Also check `spk tone 1000 500` still sounds identical in level (gain 0x1c unchanged) and
`ring test` (bell + 8 blue blinks) still works. The press-to-chime latency grows by exactly 100 ms
(2 x `PA_SETTLE_MS`), by design.

If a residual click remains at PA-on: raise `PA_SETTLE_MS` (app_main.c) to 100; if the click is at
PA-off, the amplifier needs its input muted earlier — try `bk_aud_dac_mute()` right before
`pa_set(0)` (left out on purpose: toggling the analog mute while the PA is on can itself click).

## 4. Ring indicator (Wi-Fi state)

1. Watch the ring LEDs with a camera (`imagesnap` loop, or by eye).
2. `python3 tools/dbcmd.py night status` -> `... ring 1 ... ring manual -1 out 0 wifi 1 fx 0`.
3. Simulate loss of Wi-Fi without touching credentials: power-cycle the access point, or on the
   device over **UART only** (the TCP channel dies with the link): `wifi connect` is harmless;
   the clean way is to block the device's MAC on the router for a minute.
   Expected while disconnected: double blink (100 ms on, 150 off, 100 on) every 3 s; back to off
   within one tick (50 ms) of `[wifi] got ip`.
4. Press the button while the pattern is blinking: the 8 fast blinks of the press run untouched,
   then the pattern resumes (`s_fx_running` gate).
5. `led ring on` -> solid on, pattern suspended (`night status`: `ring manual 1`);
   `led ring off` -> off; `led ring auto` -> pattern back.
6. `night ring off` -> pattern silenced (KV, survives reboot: reboot and check there is no boot
   blink); `night ring on` to restore.

## 5. Night light while streaming

Needs the light sensor covered (finger/tape over the 3-pin part by the camera flex: ~1800-2100 mV).

```bash
python3 tools/dbcmd.py night status          # light ~700 mV, cam 0, cam_dark 0
curl -s -o /dev/null http://192.168.2.89:8080/snapshot.jpg   # opens the camera (stays open)
python3 tools/dbcmd.py night status          # cam 1, samples 1 (sampled immediately on open)
```

1. Cover the sensor. Within <= 20 s UART prints `[night] dark while streaming (NNNN mV > 1400): white light on`
   and the white LED is on; `night status` -> `cam_dark 1 ... white src 0x1 ... out 1`.
   `camera.doorbell_bk7258` in HA shows a lit scene.
2. **Reflection check (important)**: with the LED on and the sensor still covered, read
   `night status` twice 20 s apart; `last` is the value sampled with the LED on. It must stay above
   `dark - hyst` (= 1000 mV by default), otherwise the LED would cycle (60 s on / 20 s off). If the
   LED's own light pulls the reading below 1000 mV, raise the hysteresis: `night hyst 600`.
3. Uncover the sensor. The LED stays on until the 60 s minimum on-time has passed, then goes off
   at the next sample (`[night] light is back (NNN mV < 1000): white light off`).
4. Cover again, wait for the LED, then `cam close` -> LED off immediately
   (`[night] camera closed: white light off`), `night status` -> `cam 0 cam_dark 0`.
5. `night auto off` (KV) -> with the camera open and the sensor covered nothing happens;
   `night auto on` restores (next sample within 20 s).
6. `led white on` / `led white off` / `led white auto`: override, then release; `night status`
   shows `white manual 1/0/-1`.

## 6. Motion light

With the camera **closed** and the sensor covered:

```bash
python3 tools/dbcmd.py hook motion
```

Expected: within ~0.2 s UART `[night] motion in the dark (NNNN mV): white light 20 s`, the white
LED is on, `night status` -> `white src 0x2 ... motion_left 19 s`, the usual `motion` webhook follows
(HA's snapshot, if the automation takes one, is lit). LED off after 20 s. A second `hook motion`
inside those 20 s extends the timer without a new reading (the LED would fool the sensor).
Repeat with the sensor uncovered: no light, no `[night]` line.
Real PIR motion (wave in front of the device) behaves the same via `reporter_task`.

## 7. Press effects still correct

- Day (sensor uncovered): `ring test` -> bell + 8 blue blinks, no white light.
- Dark: `ring test` -> bell + blinks + white light 30 s (source FX). If the camera night light is
  also holding the LED (source CAM), the LED stays on after the 30 s (OR of sources). If a motion
  light overlaps, same.

## 8. Regression / soak

- `cam stat`, `/stream` in a browser for 5 minutes with the sensor alternately covered: no reboot,
  `sys info` heap stable, `night status` `samples` grows by ~3/min.
- `wifi status`, `hook test`, heartbeat webhooks every 5 min keep arriving.
- UART: no `[night] thread failed`.

## 9. Rollback

- Behavioural: `night auto off`, `night ring off` (both KV) turn the new automatic behaviour off
  while keeping the firmware; `led white auto` / `led ring auto` clear manual overrides.
- Full: OTA the previous image: `python3 tools/dbcmd.py ota http://192.168.2.80:8123/local/doorbell/doorbell-fw_0.18.5_app_pack.rbl`
  (the old KV keys are untouched; the new `db_night_*` keys are simply ignored by older firmware).

## 10. Home Assistant

No YAML changes are required. Optional, if you want the device's own view of things in HA, the
`sys info` reply is not pushed anywhere; the webhook payload is unchanged (`light_mv` was already
there). Nothing to reload.
