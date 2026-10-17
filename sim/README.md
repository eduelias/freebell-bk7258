# Virtual doorbell (`sim/`)

A software stand-in for the doorbell so the tools and the Home Assistant integration can be
tried, demonstrated and tested without the board: `tools/dbcmd.py`, curl, HA's generic camera
and the webhook automation all talk to it exactly as they talk to the firmware.

Python 3.11+, standard library only. [Pillow](https://pypi.org/project/pillow/) is optional:
with it the camera frames are proper rendered 720p pictures, without it a built-in pure-Python
encoder produces a coarse but valid 1280x720 baseline JPEG (text and a moving block).

```bash
python3 sim/virtual_doorbell.py                  # TCP 2323, HTTP 8080, control 2324, token "sim"
python3 sim/virtual_doorbell.py --help
```

The start-up banner prints the token, the ports, the state file and the webhook target.
Stop it with Ctrl-C.

## What it speaks

| Interface | Firmware | Simulator |
|---|---|---|
| TCP command channel | port 2323, one line `<token> <command>`, reply `ok` / `unknown` / `denied` + REPLY() text, close after 100 ms | identical framing, same reply strings as the firmware sources (`cmd.c`, `net.c`, `camera.c`, ...) / `night.c` / `pir.c` / `power.c` / `rtsp.c`; token from `--token`, `$DOORBELL_TOKEN`, a stored `tok set`, else `sim` |
| HTTP | port 8080: `/` status page, `/snapshot.jpg`, `/stream` (MJPEG), feat/talk `/speak` `/mic.wav` `/talk`, fw 0.25 `/cmd?t=TOKEN&c=COMMAND` and `/log?t=TOKEN` | same routes, status codes, headers and bodies (`503 camera not open`, `503 camera busy (H.264/RTSP)`, `multipart/x-mixed-replace; boundary=frame`, talk JSON, `403 denied`), plus `/sim/<command>` |
| Webhook | `POST` JSON to the KV hook on boot, ring, motion, wake, test, heartbeat (5 min) | same JSON (`event fw ip mv light_mv uptime_s motion_count ring_count pir_on pir_sens batt_mv batt_pct usb charging sleep_min wake_pir [reason] [policy policy_rev]`), same timing; `pwr policy` really GETs and applies a policy.json |
| RTSP | `rtsp://IP:554/live`, own H.264 server + PCMA microphone track + ONVIF backchannel (fw 0.26) | optional stand-in: `--rtsp` + ffmpeg serve a test pattern with a PCMA sine track at `rtsp://HOST:8554/live` for one client after `rtsp on` (no backchannel; FFmpeg 8.0's `-rtsp_flags listen` is broken, use an older ffmpeg). `talk aec|vocmic|voc` and the `rtsp status` audio line are modelled |

The reported firmware version is the `FW_VERSION` of `project/main/version.h` next to the
script (override with `--fw-version`); an `ota` bumps it to the version in the file name.

## Driving it

`tools/dbcmd.py` works unchanged. Point it at the simulator with environment variables or the
(new, optional) leading flags:

```bash
export DOORBELL_HOST=127.0.0.1 DOORBELL_TOKEN=sim         # DOORBELL_PORT if you moved the TCP port
python3 tools/dbcmd.py sys info
python3 tools/dbcmd.py --host 127.0.0.1 --port 2323 --token sim pwr status
```

The physical world is simulated with `sim` commands over the same channel, over HTTP, or typed
on the simulator's stdin when it runs in a terminal:

| `sim ...` | Effect |
|---|---|
| `sim press` | button GPIO 53: debounce 0.8 s, `ring_count`, chime + 8 blue blinks (+ white light 30 s when dark), `ring` report (max one per 10 s); wakes a sleeping device (reason `button`) |
| `sim motion` | PIR edge GPIO 24 through pir.c's filter (off / 5 s warm-up / cooldown), `motion` report, night light 20 s when dark; wakes a sleeping device when `pwr wake_pir on` |
| `sim light MV` | light sensor (ADC14): ~740 mV lit room, ~1800-2100 mV covered; dark above `ring dark MV` (default 1400) |
| `sim batt MV` | battery (ADC15 x `pwr ratio`): 3886 mV = 72 % with the firmware's table |
| `sim usb on\|off`, `sim charging on\|off` | GPIO 19 / 15; plugging USB wakes a sleeping device (reason `usb`) |
| `sim time SPEED` | run the simulated clock faster: heartbeat, cooldowns, boot hold, sleep and reboot durations all scale |
| `sim heartbeat`, `sim wake [REASON]` | force the 5 min heartbeat now / end a sleep |
| `sim status`, `sim help` | one-line state, command list |

```bash
curl http://127.0.0.1:8080/sim/press
curl http://127.0.0.1:8080/sim/light/1800            # also /sim/light%201800 and /sim/light?mv=1800
```

The `sim` commands are also accepted on the **control port** (`--sim-port`, default TCP + 1), which
stays open while the "device" is asleep or rebooting and its real ports are down:
`python3 tools/dbcmd.py --port 2324 sim press`.

## Behaviour that is modelled

- **Boot**: Wi-Fi + IP ~1 s after power-on, `boot` report, 120 s boot hold before any automatic
  sleep, PIR edges blanked for 5 s, blue ring boot blink.
- **Ring**: button debounce, local effects (bell = speaker busy ~4 s, ring LED blinks, white light
  when dark), one `ring` report per 10 s while `ring_count` counts every press.
- **PIR**: enable / sensitivity bits / cooldown / warm-up exactly as `pir.c`, `motion test`.
- **Night light arbiter** (`night.c`): camera-open-and-dark source sampled every 20 s with the
  400 mV hysteresis and 60 s minimum on-time, motion source 20 s, press source 30 s, manual
  `led white on|off|auto`; every LED change is a log line.
- **Camera**: opens on the first HTTP request (first frame after ~1 s), `cam open|close|stat|mode`,
  JPEG/H.264 mode policy (`cam mode auto|jpeg|h264`: `/stream` answers 503 in H.264 mode, the
  last JPEG is still served), frames at `--fps` with a timestamp and a moving element.
- **Power** (`power.c`): `pwr status` fields, battery percent table, `pwr sleep on` automatic sleep
  when on battery and idle (or `force`), `pwr sleepnow` (10 s grace); asleep = TCP/HTTP/RTSP ports
  closed; wake by RTC (`snap_min`, 360 min keepalive), button, PIR (`wake_pir`) or USB plug; then
  `boot` + `wake` reports with `reason`, `reset 0x3/0x4 (deepsleep-gpio/rtc)` in `sys info`.
- **Reboot / OTA**: `reboot` drops every connection for ~6 s; `ota URL` downloads the file
  (non-empty, `.rbl`), logs `[ota] result 0 (rebooting)`, is down ~35 s, comes back as
  `doorbell-fw_<ver>_app_pack.rbl`'s version with `reset 0x18 (ota)`. Failures log `result -4`
  and keep the device up.
- **KV settings** persist in the state file (`--state`, default `~/.virtual_doorbell.json`) under
  the firmware's key names (`db_hook`, `db_pir_cd`, `db_dark_mv`, `db_pwr_*`, ...), together with
  the simulated sensors and the firmware version.
- **Talk** (feat/talk): `POST /speak` raw s16le 8 kHz or PCM WAV (415 otherwise), `GET /speak?tone=`,
  `?url=`, `/mic.wav[?seconds=N]` (16 kHz synthetic tone, chunked when endless), `/talk` page,
  `talk status|gain|micgain|max|stop`. Playback takes real (scaled) time and keeps the speaker
  busy, like the DAC does. `--audio` additionally plays it through the host (afplay/aplay/ffplay).
- **SD** (feat/sd): an always-present 7.5 GB card, `sd status|mount|umount|format|ls|save|auto`,
  auto-saves a file name per ring/motion.

### Not simulated

- No real H.264: without `--rtsp` + ffmpeg `rtsp on` only flips the setting and `rtsp status`
  stays `stopped`; with it, ffmpeg serves a test pattern to one client and the camera mode does
  not follow RTSP clients (the stand-in cannot report them).
- No audio hardware: `spk`, `mic`, `talk` keep the busy flags and timing, the bytes are synthetic.
- `pin`, `mem`, `cam probe`, `wifi set` only log (as on the device most of their output is UART-only).
- One TCP command at a time (the firmware's listener has a backlog of 1 as well), no ADC noise
  model beyond a few mV of jitter, no heap or crash behaviour.

## Home Assistant

Run the simulator on a host HA can reach (the HA box itself, a Pi next to it, or your laptop on
the same LAN; a macOS firewall usually blocks incoming connections to Python, so the laptop
works for local tests but not for HA).

1. **Camera**: Settings -> Devices -> Add integration -> *Generic Camera*,
   still image `http://<host>:8080/snapshot.jpg`, stream `http://<host>:8080/stream`,
   frame rate 5, no authentication. Or from a host with an HA token in `~/.ha_token`:
   `python3 tools/ha_generic_camera.py add --name "Virtual doorbell" --still http://<host>:8080/snapshot.jpg --stream http://<host>:8080/stream`
2. **Webhook**: point the simulator at your automation's webhook id
   (`python3 tools/dbcmd.py hook set <ha-ip>:8123/api/webhook/<id>` or `--hook`); like the
   firmware it starts with no target and skips every report (one `hook not set` log line) until
   one is set. Then reuse the
   automation / template sensors from the top-level README ("Prototype: Wi-Fi + Home Assistant
   webhook" and "Battery, charger and deep sleep"). `sim press` then produces the ring
   notification and the `camera.snapshot`, `sim batt 3500` moves `sensor.doorbell_bk7258_battery`.
3. Nothing in the simulator calls HA on its own except those webhook POSTs.

## Tests

```bash
pip install pytest pillow          # pillow optional
python3 -m pytest sim/tests        # ~35 s
```

The suite starts simulators on ephemeral ports (one per module at real time, fresh ones at 20x
for sleep/reboot/OTA), a webhook sink and a local OTA file server, and checks dbcmd.py
round-trips, reply strings, snapshot/stream validity, the report JSON, state persistence, the
sleep and OTA cycles. `test_contract.py` parses `s_doorbell_commands[]` (and any other
`struct cli_command` table under `project/main/`) and fails when the firmware grows a command the
simulator does not handle. `.github/workflows/sim-tests.yml` runs it on push.

## Options

```
--tcp-port 2323  --http-port 8080  --sim-port TCP+1  --rtsp-port 8554  --bind 0.0.0.0
--token TOKEN    --state ~/.virtual_doorbell.json   --hook HOST:PORT/PATH   --fps 5
--fw-version X   --ip A.B.C.D   --time SPEED   --rtsp   --audio   --log-level INFO
```

Every command and HTTP request is one log line, in the same `[tag]` style as the UART log.
