# Test plan: PIR control + motion snapshots in Home Assistant (feat/motion, fw 0.19.0)

Branch `feat/motion`, worktree `~/reps/home/iot/wt-motion`. New file `project/main/pir.c` (+ `pir.h`),
hooks in `app_main.c` (`pir_init()` from `inputs_init()`, `motion_cb()` replaces `pir_isr()`, report JSON,
two command-table entries). Build: `OUT=~/data/doorbell-bk7258/avdk-build-motion FW_DIR=~/data/doorbell-bk7258/firmware-motion scripts/build-avdk.sh`
(the rbl lands in `~/data/doorbell-bk7258/firmware-motion/`; the shared `firmware/` dir and its
`_latest` symlink were deliberately left alone).

All device commands below are `python3 tools/dbcmd.py ...` from the repo
root (TCP 2323); every command REPLYs its text, so UART is optional. Nothing here needs the owner.

## What changed on the device

| Command | Effect | KV key / default |
|---|---|---|
| `pir` / `pir status` | one line: on/off, sens + the two GPIO levels, cooldown, count, ms since last, cooldown left, raw edges, ignored edges, live GPIO 24 and GPIO 50 levels | - |
| `pir on` / `pir off` | on: GPIO 50 high, sensitivity bits applied, GPIO 24 IRQ enabled (edges ignored for a 5 s warm-up); off: IRQ disabled, GPIO 50 low | `db_pir_on`, default **on** |
| `pir sens 0-3` | GPIO 25 = bit 0, GPIO 26 = bit 1 (stock `app_pir_set_sens`); stock used 0/1/2 | `db_pir_sens`, default **1** (stock default: GPIO25=1, GPIO26=0) |
| `pir cooldown MS` | min ms between accepted motions (500..600000); applied in the ISR | `db_pir_cd`, default **10000** (stock: 30000) |
| `motion test` | simulated PIR edge through the same handler as the ISR; replies accepted / cooldown left / warm-up left / pir off | - |
| `hook motion` | unchanged: forces a `motion` report without touching the PIR counters | - |

Report JSON (all events) gains `"pir_on":0|1` and `"pir_sens":0..3`; `light_mv` was already there.
`motion_count` in the JSON is app_main's counter (PIR-accepted edges + `hook motion`); `pir status`
`count` is the PIR module's own (hardware + `motion test`). They match unless `hook motion` was used.

Stock facts the defaults come from (capstone on `<your dump directory>/doorbell_flash_logical.bin`,
`tuya_pir.c` init at 0x02151c68, set_sens at 0x021519a4, set_enable at 0x02151e40): GPIO 50/26/25 are
initialised push-pull OUTPUT HIGH from the 3-byte config at 0x020172d5 (`04 01 01`), then
`set_sens(app_pir_sens)` writes GPIO25 = n&1, GPIO26 = (n>>1)&1. KV defaults `app_pir_sens`=1,
`app_pir_enable`=1. Stock "enable" only gates the GPIO 24 IRQ (GPIO 50 is never lowered); ours also
drops GPIO 50 on `pir off` (assumed PIR supply/enable, saves battery; see risks). Stock cooldown 30 s.
Which sensitivity value is the *most* sensitive is not known from the code; see test 6.

Important: PIR-triggered motion has **never** been confirmed with our firmware (NOTES 2026-10-02 22:47:
"PIR ISR path still unverified"); only passive GPIO 24 pulses were seen with `pin watch`. Test 3 is the
first real check.

## Device tests (after OTA of the 0.19.0 rbl)

Observe on the workstation: `dbcmd.py` replies. In HA: `input_text.doorbell_bk7258_last_report` (Settings >
Helpers, or Developer tools > States), the persistent notifications, and the phones.

1. **Boot state** - `dbcmd.py pir status`
   Expect: `[pir] on sens=1 (GPIO25=1 GPIO26=0) cooldown=10000 ms count=0 last=0 ms ago (cooldown left 0 ms) edges=0 ignored=0 GPIO24=0 GPIO50=1`.
   `GPIO50=1` proves the enable is driven; `GPIO24=0` is the idle PIR output. On UART the boot log has
   `[pir] on, sens 1 (GPIO25=1 GPIO26=0), cooldown 10000 ms, warm-up 5000 ms` right after `[cfg] ssid ...`.
   Also `dbcmd.py hook test` -> HA `last_report` shows `..."pir_on":1,"pir_sens":1}` at the end.

2. **Simulated motion** (wait >5 s after boot) - `dbcmd.py motion test`
   Expect reply `[motion] simulated edge accepted: count 1, report queued`; UART `[hook] motion -> 200`
   within ~1 s; HA `last_report` = `{"event":"motion",...}`; persistent notification "Motion at HH:MM:SS";
   with the HA change below also a phone notification with the snapshot (see HA tests).
   Immediately again: `dbcmd.py motion test` -> `[motion] ignored: cooldown, NNNN ms left (count 1)`.
   `dbcmd.py pir status` -> `count=1 ... edges=2 ignored=1`.

3. **Real PIR edge** - wait 10 s, then wave a hand 20-50 cm in front of the PIR window (the small white
   dome next to the camera; the board is disassembled, so point the sensor side at yourself) once.
   Expect within 1-3 s: `pir status` `count` +1, `edges` +1, HA motion report. If `count` stays 0 but
   `edges` also stays 0, check `dbcmd.py pin get 24` while waving (should read 1 for 2-4 s): if the pin
   moves but `edges` does not, the ISR did not fire (bk_gpio_register_isr order) - fall back to
   `pin watch 20` + a wave to see the raw edge. Repeated waves inside 10 s only bump `edges`/`ignored`.
   Note: a hand-waved PIR can emit a second rising edge when the pulse re-triggers; that is what the
   cooldown absorbs.

4. **Cooldown setting** - `dbcmd.py pir cooldown 3000` -> `[pir] cooldown 3000 ms`; two `motion test`
   4 s apart both accepted; `pir cooldown 100` -> refused (`500-600000 ms`); restore
   `dbcmd.py pir cooldown 10000`. Persistence: `dbcmd.py reboot` (device reboots, ~10 s) then
   `pir status` shows the saved value.

5. **Off / on** - `dbcmd.py pir off` -> `[pir] off (GPIO50=0, IRQ disabled, ...)`; `pir status` ->
   `off ... GPIO50=0`; `motion test` -> `[motion] ignored: pir off`; wave: no edges counted (`edges`
   unchanged - the IRQ is disabled, so edges are not even seen). `hook test` -> JSON has `"pir_on":0`.
   `dbcmd.py pir on` -> `GPIO50=1`; `motion test` within 5 s -> `ignored: warm-up, N ms left`; after
   5 s -> accepted. Watch for spurious motion reports in the first minute after `pir on` (a PIR that was
   really unpowered needs tens of seconds to settle - if you see them, raise `PIR_WARMUP_MS` in pir.c).
   Persistence: `pir off`, `reboot`, `pir status` -> still off, `GPIO50=0`. Then `pir on`.

6. **Sensitivity** - `dbcmd.py pir sens 0` / `2` / `3` -> replies show the GPIO pair; `pir status`
   mirrors it. Functional ordering (optional, 5 min): for each of 0/1/2 walk towards the sensor from
   4 m and note the distance at which `edges` increments (wait out the cooldown or set `pir cooldown
   500` for this). Record the result in the README table. Restore `pir sens 1`. `pir sens 4` -> refused.

7. **Regression** - button press still gives the ring effects and the ring report; `ring test`,
   `led`, `cam stat`, `spk tone 1000 300` unchanged. Heartbeat JSON (every 5 min) carries the two
   new fields; its length is ~190 bytes, inside the 256-byte buffer.

## Home Assistant changes (apply on the HA box)

Files live on HA at `/config/automations.yaml`; edit through the SSH add-on as root
(`ssh root@<ha-ip>` from a host that can reach it).
Back up first: `cp /config/automations.yaml /config/automations.yaml.bak-$(date +%Y%m%d)-motionsnap`.
Create the snapshot dir once: `mkdir -p /config/www/doorbell` (exists already; it holds the OTA rbls).
`/config/www` is inside HA's default `allowlist_external_dirs`, so `camera.snapshot` may write there.
The phones are Android (`mobile_app_<your_phone>`), so the `image:` key is the
right one (companion docs: `data.image` works on both platforms; iOS alternatively takes
`attachment: {url: ...}`). `/config/www/x.jpg` is served as `/local/x.jpg`; the `?v=` suffix defeats
the companion app's image cache for a filename that is reused every time.

Replace the `ring` and `motion` sequences of automation `doorbell_bk7258_webhook` with these
(indentation matches the existing file; `mode: queued` stays so a ring during a motion snapshot is
not lost):

```yaml
    - conditions:
      - condition: template
        value_template: '{{ trigger.json.event == "ring" }}'
      sequence:
      - action: persistent_notification.create
        data:
          notification_id: doorbell_ring
          title: Doorbell
          message: Ring at {{ now().strftime("%H:%M:%S") }} (battery {{ trigger.json.mv | default("?") }} mV)
      - action: notify.doorbell_family
        data:
          title: Doorbell
          message: Someone is at the door ({{ now().strftime("%H:%M") }})
          data:
            ttl: 0
            priority: high
            channel: doorbell
            tag: doorbell_ring
        continue_on_error: true
      # second message with the picture, so the first push is not delayed by the camera (~2-4 s)
      - action: camera.snapshot
        target:
          entity_id: camera.doorbell_bk7258
        data:
          filename: /config/www/doorbell/last_ring.jpg
        continue_on_error: true
      - action: notify.doorbell_family
        data:
          title: Doorbell
          message: At the door, {{ now().strftime("%H:%M:%S") }}
          data:
            ttl: 0
            priority: high
            channel: doorbell
            tag: doorbell_ring_photo
            image: /local/doorbell/last_ring.jpg?v={{ now().timestamp() | int }}
        continue_on_error: true
    - conditions:
      - condition: template
        value_template: '{{ trigger.json.event == "motion" }}'
      sequence:
      - action: persistent_notification.create
        data:
          notification_id: doorbell_motion
          title: Doorbell
          message: Motion at {{ now().strftime("%H:%M:%S") }} (light {{ trigger.json.light_mv | default("?") }} mV, pir sens {{ trigger.json.pir_sens | default("?") }})
      - action: camera.snapshot
        target:
          entity_id: camera.doorbell_bk7258
        data:
          filename: /config/www/doorbell/last_motion.jpg
        continue_on_error: true
      - action: notify.doorbell_family
        data:
          title: Doorbell motion
          message: Motion at {{ now().strftime("%H:%M:%S") }}
          data:
            channel: doorbell_motion
            tag: doorbell_motion
            image: /local/doorbell/last_motion.jpg?v={{ now().timestamp() | int }}
        continue_on_error: true
```

Then: `ha core check` (must print no error), `ha core restart` is NOT needed; reload with
`curl -s -X POST -H "Authorization: Bearer $(cat ~/.ha_token)" http://192.168.2.80:8123/api/services/automation/reload`
from the HA box (or Developer tools > YAML > Automations). Verify with
`curl -s -H "Authorization: Bearer $(cat ~/.ha_token)" http://192.168.2.80:8123/api/states/automation.doorbell_bk7258_webhook_events`
(`"state":"on"`).

Timing note for `camera.snapshot`: HA fetches `http://192.168.2.89:8080/snapshot.jpg` with a 10 s
timeout. The device opens the camera on the first HTTP request (~1-1.6 s to the first JPEG; measured
1.6 s for an 88 KB 720p frame), so the snapshot normally arrives 2-4 s after the event. If HA already
holds the MJPEG stream (camera entity open in a dashboard) the camera is warm and the snapshot is
instant. The device-side report is posted from the reporter task, which is what triggers the
automation, so there is no device change needed; a future `pir cam on` could pre-open the camera on
the edge to shave the 1.6 s.

### HA tests

H1. `dbcmd.py motion test` -> within ~5 s both phones get "Doorbell motion" with a picture; on HA
    `ls -l /config/www/doorbell/last_motion.jpg` is fresh (~30-90 KB) and
    `http://192.168.2.80:8123/local/doorbell/last_motion.jpg` opens in a browser.
H2. Press the button -> first phone push "Someone is at the door" immediately (as today), a second
    one with the picture 2-5 s later; `last_ring.jpg` fresh. The automation trace (Settings >
    Automations > Doorbell BK7258 > Traces) shows `camera.snapshot` taking 1-4 s.
H3. Camera failure path: `dbcmd.py cam close` then unplug nothing - instead simulate a slow camera by
    `dbcmd.py motion test` while `/stream` is being pulled by 6 clients, or just check the trace:
    with `continue_on_error: true` a snapshot timeout logs an error and the text notification is still
    sent (with the previous picture, hence the `?v=` cache-buster so it is at least not a stale cached one).
H4. The `tag:` keys make a repeated motion replace the previous notification on Android instead of
    stacking; ring and ring-photo use different tags so both stay visible.

## Rollback

- Device: `pir on`, `pir sens 1`, `pir cooldown 10000` restore the defaults (or OTA the previous
  0.18.5 rbl from `/config/www/doorbell/`; KV keys `db_pir_*` are then simply unused).
- HA: `cp /config/automations.yaml.bak-<date>-motionsnap /config/automations.yaml` + automation reload.

## Risks / open points

- `pir off` lowers GPIO 50. The stock never does that (it only masks the IRQ), so whether GPIO 50 is
  a PIR supply enable or something else sharing a rail is inferred from its name only. If `pir off`
  has a side effect (camera, light sensor), change `pir_apply_enable()` to keep GPIO 50 high and only
  disable the IRQ. The default is on, so nothing changes for the owner unless he uses `pir off`.
- Sensitivity direction (0 vs 2 = more sensitive) unknown until test 6.
- The warm-up blanking (5 s) is a guess; the stock has none because it never cuts the sensor.
- `motion_count` vs `pir status count` diverge if `hook motion` is used (documented above).
