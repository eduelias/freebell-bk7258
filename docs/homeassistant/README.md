# Home Assistant integration

Everything the doorbell needs on the Home Assistant side, as sanitized copies of the author's live
configuration (Home Assistant 2026.9). Replace the placeholders before use:

| Placeholder | Meaning |
|---|---|
| `<device-ip>` | the doorbell's IP or hostname (give it a DHCP reservation) |
| `<ha-ip>` | the Home Assistant host as seen from the doorbell |
| `<webhook-id>` | your own random webhook id (step 1) |
| `mobile_app_<your_phone>` | your phone's `notify.mobile_app_*` service(s) |

Files:

- [`automations/doorbell_bk7258_webhook.yaml`](automations/doorbell_bk7258_webhook.yaml): the webhook
  automation. Fans every report out as the event `doorbell_bk7258_report`, keeps the last payload in
  a text helper, and on `ring`/`motion` pushes a notification followed by a camera snapshot; `boot`
  and `wake` go to the logbook (an RTC wake also takes a snapshot).
- [`packages/doorbell_bk7258.yaml`](packages/doorbell_bk7258.yaml): the package: `notify.doorbell_family`
  group, battery/voltage/light/last-event template sensors and USB/charging binary sensors fed by
  the event, a low-battery automation, and the talk integration (`rest_command`, `shell_command`,
  `script.doorbell_say`, `script.doorbell_play`).
- [`doorbell_tts_wav.py`](doorbell_tts_wav.py): helper run by `shell_command.doorbell_tts_wav` inside
  the HA core container: text -> HA TTS -> 16 kHz mono WAV under `/config/www/doorbell/`.
- [`doorbell_policy.py`](doorbell_policy.py) (fw 0.25): helper run by `shell_command.doorbell_policy_write`:
  the seven setting helpers -> `/config/www/doorbell/policy.json` (with `rev` = timestamp), which the
  device fetches and applies on every boot.

## Setup

1. **Webhook automation.** Generate a random id, for example
   `python3 -c "import secrets;print('doorbell_'+secrets.token_hex(8))"`. Create a Text helper
   `input_text.doorbell_bk7258_last_report` (Settings > Devices & services > Helpers > Text, max
   length 255). Append `automations/doorbell_bk7258_webhook.yaml` to `automations.yaml` (or create a
   new automation, switch to YAML mode and paste it), put your id in `webhook_id`, and reload
   automations. The webhook is `local_only: true`; keep it that way.
2. **Point the device at it.** Over serial or `tools/dbcmd.py`:
   `hook set <ha-ip>:8123/api/webhook/<webhook-id>`, then `hook test`. The text helper shows the
   JSON within a second, and `boot` reports appear in the logbook after every reboot.
3. **Camera entity.** Turn the stream on once on the device (`rtsp on`, saved in KV), then create a
   *Generic Camera* with still image URL `http://<device-ip>:8080/snapshot.jpg`, stream source
   `rtsp://<device-ip>:554/live`, RTSP transport `tcp`, frame rate 5, no authentication, "verify
   SSL" off. Either through the UI (Settings > Devices & services > Add integration > Generic
   Camera) or with the helper from a host that can reach HA (long-lived token in `~/.ha_token`,
   `HA_URL=http://<ha-ip>:8123`):

   ```bash
   python3 tools/ha_generic_camera.py add --name "Doorbell BK7258" \
       --still http://<device-ip>:8080/snapshot.jpg --stream rtsp://<device-ip>:554/live \
       --rtsp-transport tcp --framerate 5
   python3 tools/ha_generic_camera.py list        # entry ids; `options ENTRY_ID ...` edits, `delete ENTRY_ID` removes
   ```

   The automation expects the entity to be `camera.doorbell_bk7258` (rename it if HA picked
   another id). Why both URLs and not stream-only: at the sensor's ~3 fps in the dark, HA's
   still-from-stream needs about 7 s and times out half the time; the still URL answers in 45 ms.
   See [`../notes/camera.md`](../notes/camera.md).
4. **Package.** Make sure `configuration.yaml` has

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

   copy `packages/doorbell_bk7258.yaml` to `/config/packages/`, edit the phones in
   `notify.doorbell_family` and the two IP placeholders, run the configuration check and restart
   Home Assistant once (`shell_command` has no reload service; scripts and rest commands reload).
   The sensors appear after the first report (`hook test`).
5. **Talk (optional).** Copy `doorbell_tts_wav.py` to `/config/doorbell_tts_wav.py` (`chmod +x`).
   Create a long-lived access token for a dedicated user and store it in
   `/config/.doorbell_ha_token` (`chmod 600`; it never leaves the HA host). Make sure a TTS entity
   exists; the helper defaults to `tts.google_translate_en_com` (override with the environment
   variable `DOORBELL_TTS_ENTITY` in the `shell_command`, or edit the script). `mkdir -p
   /config/www/doorbell`. Then try `script.doorbell_say` with `message: "Hello"`: the WAV lands in
   `/config/www/doorbell/say.wav` and the device fetches it from `http://<ha-ip>:8123/local/doorbell/`.
   `script.doorbell_play` plays any WAV you put in that folder (16 kHz mono is best; the device
   resamples 16/44.1/48 kHz).
6. **Settings from HA (fw 0.25, optional).** Create the helpers (Settings > Devices & services >
   Helpers, or over the websocket `input_number/create` etc.; the entity ids must match the package):
   `input_number.doorbell_volume` (0-100, step 5, %), `input_select.doorbell_pir_sens` (options `0 1 2`),
   `input_boolean.doorbell_pir_wake`, `input_number.doorbell_day_interval` (5-120 min),
   `input_number.doorbell_night_interval` (15-240 min), `input_number.doorbell_dark_mv` (500-2500 mV),
   `input_boolean.doorbell_night_light`; set them to the device's current values (`vol`, `pir status`,
   `pwr status`, `ring`, `night status` tell you). Copy `doorbell_policy.py` to `/config/` (`chmod 755`).
   Put the device's TCP token into `/config/.doorbell_tcp_token` (`chmod 600`) and into `secrets.yaml`
   as the full command URL:

   ```yaml
   doorbell_cmd_url: "http://<device-ip>:8080/cmd?t=<tcp-token>&c={{ cmd | urlencode }}"
   ```

   The package brings `shell_command.doorbell_policy_write`, `rest_command.doorbell_cmd`, the
   automation `doorbell_bk7258_policy` (any helper change -> rewrite the file -> `pwr policy pull`
   over `/cmd` in case the device is awake) and `sensor.doorbell_bk7258_policy` (state `applied` /
   `none` / `error`, attribute `rev`, from the boot report). Reload rest/shell commands, automations
   and templates (all have reload services in 2026.9), then call `shell_command.doorbell_policy_write`
   once so the first boot after the 0.25 OTA finds a file; `curl http://<ha-ip>:8123/local/doorbell/policy.json`
   must return the JSON. The device's default `pwr policy auto` derives the URL from the webhook host,
   so nothing is configured on the device. The webhook automation's fan-out and the boot logbook line
   carry `policy`, `policy_rev`, `sleep_min`, `wake_pir` (see the automation file).
7. **OTA hosting (optional, recommended).** The same `/config/www/doorbell/` folder is a convenient
   image host: `OTA_URL_BASE=http://<ha-ip>:8123/local/doorbell` and
   `OTA_PUBLISH="scp {rbl} root@<ha-ip>:/config/www/doorbell/"` in `~/.config/doorbell-bk7258/env`
   make `scripts/release.sh` publish and flash over the air.

## Worked example

The author's bench: device `192.168.2.89`, Home Assistant `192.168.2.80`.

```bash
python3 tools/dbcmd.py hook set 192.168.2.80:8123/api/webhook/doorbell_0123456789abcdef
python3 tools/dbcmd.py hook test
python3 tools/ha_generic_camera.py add --name "Doorbell BK7258" \
    --still http://192.168.2.89:8080/snapshot.jpg --stream rtsp://192.168.2.89:554/live \
    --rtsp-transport tcp --framerate 5
curl -o /dev/null -w '%{http_code}\n' http://192.168.2.89:8080/snapshot.jpg
```

## Entities you get

`sensor.doorbell_bk7258_battery` (%), `sensor.doorbell_bk7258_battery_voltage` (mV),
`sensor.doorbell_bk7258_light_level` (mV, higher = darker), `sensor.doorbell_bk7258_last_event`
(attributes `reason`, `fw`, `ip`, `uptime_s`), `binary_sensor.doorbell_bk7258_usb_power`,
`binary_sensor.doorbell_bk7258_charging`, `input_text.doorbell_bk7258_last_report`,
`camera.doorbell_bk7258`, `notify.doorbell_family`, `script.doorbell_say`, `script.doorbell_play`,
`sensor.doorbell_bk7258_policy` (fw 0.25) and the event `doorbell_bk7258_report` for your own
automations; the seven setting helpers of step 6 and the `rest_command.doorbell_cmd` action
(`data: {cmd: "vol 60"}`, reply text in `response_variable`, only while the device is awake). Snapshots are written to
`/config/www/doorbell/last_ring.jpg`, `last_motion.jpg` and `wake_latest.jpg`.

## Two-way audio (fw 0.26+)

The RTSP stream carries a PCMA microphone track and an ONVIF backchannel into the speaker, with the
SDK's echo canceller on the device ([`../notes/audio.md`](../notes/audio.md)). Home Assistant 2026.9
streams cameras through its embedded **go2rtc**, which supports ONVIF backchannels out of the box:
for an `rtsp://` source without options it sends `Require: www.onvif.org/ver20/backchannel` on
DESCRIBE and sets up the `a=sendonly` track as its audio sink. Nothing needs to be configured for
that: the generic camera's stream source `rtsp://<device-ip>:554/live` is registered by HA as the
go2rtc stream `camera.doorbell_bk7258`, and HA adds a second producer
(`ffmpeg:camera.doorbell_bk7258#audio=opus`) so browsers get the microphone as Opus over WebRTC.
Only if you give the source `#` options yourself (e.g. a `streams:` entry in `/config/go2rtc.yaml`
with `#media=video+audio`) must you add `#backchannel=1`, because go2rtc turns the backchannel off
as soon as any option is present.

What you get where:

| Client | Listen (mic track) | Talk (backchannel) |
|---|---|---|
| HA frontend camera more-info / picture card (WebRTC) | yes (unmute the player) | **no**: the HA frontend's WebRTC player only adds a `recvonly` audio transceiver (`ha-web-rtc-player.ts`), there is no microphone button in 2026.9 |
| go2rtc's own UI (`debug_ui` in `configuration.yaml`, port 11984, basic auth) `webrtc.html?src=camera.doorbell_bk7258&media=video+audio+microphone` | yes | yes (browser microphone, HTTPS or localhost) |
| [WebRTC Camera custom card](https://github.com/AlexxIT/WebRTC) with `media: video,audio,microphone` | yes | yes |
| `tools/rtsp_backchannel.py`, VLC/ffmpeg (listen only) | yes | tool only |

Checking go2rtc's view of the stream (optional, needs a core restart twice): add to `configuration.yaml`

```yaml
go2rtc:
  debug_ui: true
  username: <pick one>
  password: <pick one>
```

restart, then `curl -u user:pass http://<ha-ip>:11984/api/streams` while someone views the camera:
the `camera.doorbell_bk7258` producer lists the three medias (`video, recvonly, H264`; `audio,
recvonly, PCMA/8000`; `audio, sendonly, PCMA/8000`). Remove the three lines and restart again
afterwards: the debug UI exposes every stream source (with credentials) to the LAN.

### "Answer" on the ring push

The ring notification carries an `Answer` action that opens the camera's more-info dialog in the
Companion app (live video + microphone audio, listen-only as noted above); Android gets the
`doorbell` channel at high importance so it rings through Do Not Disturb exceptions you grant it:

```yaml
      - action: notify.doorbell_family
        data:
          title: Doorbell
          message: Someone is at the door ({{ now().strftime("%H:%M") }})
          data:
            ttl: 0
            priority: high
            channel: doorbell
            importance: high
            tag: doorbell_ring
            clickAction: entityId:camera.doorbell_bk7258
            actions:
              - action: DOORBELL_ANSWER
                title: Answer
                uri: entityId:camera.doorbell_bk7258
```

`entityId:<entity>` is the Companion app's deep link to an entity's more-info dialog (both Android
and iOS); replace it with a dashboard path (`/dashboard-prototype1/home`) if you prefer a view with
a WebRTC card. The full automation is in
[`automations/doorbell_bk7258_webhook.yaml`](automations/doorbell_bk7258_webhook.yaml).

Listening without the stream: an iframe card on `http://<device-ip>:8080/talk`, or
`http://<device-ip>:8080/mic.wav` in VLC/ffplay (8 kHz while an RTSP audio session is live, 16 kHz
otherwise).
