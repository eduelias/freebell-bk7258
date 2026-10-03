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
6. **OTA hosting (optional, recommended).** The same `/config/www/doorbell/` folder is a convenient
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
and the event `doorbell_bk7258_report` for your own automations. Snapshots are written to
`/config/www/doorbell/last_ring.jpg`, `last_motion.jpg` and `wake_latest.jpg`.

Listening to the doorbell's microphone from HA: an iframe card on `http://<device-ip>:8080/talk`,
or `http://<device-ip>:8080/mic.wav` in VLC/ffplay (the camera entity has no audio).
