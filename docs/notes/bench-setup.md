# Author's bench setup

Nothing in the repository depends on this setup; it is described so that the test plans and the
notes make sense, and so you can map them onto your own bench.

| Role | What the author used | What you need |
|---|---|---|
| Build / flash workstation | an Apple Silicon laptop: podman with Rosetta, Python 3.11, a CP210x/CH340 USB serial adapter at `/dev/cu.usbserial-*` | any Linux/macOS machine with podman or docker, Python 3 + pyserial, a 3.3 V USB-UART adapter (`SERIAL_PORT` if it is not auto-detected) |
| "Bench host" | a Raspberry Pi on the same LAN (also the only host allowed to SSH into the Home Assistant box) | optional: any always-on Linux host on the LAN with ffmpeg and curl. Used for TCP audio peers (`tools/audio_relay.py`), long RTSP soaks (`systemd-run --user ...`) and as a jump host to HA |
| Why a second host | the laptop's firewall blocks incoming connections to Python, so the device can neither fetch OTA images from it nor connect to a TCP audio peer on it | if your workstation accepts inbound connections you do not need one |
| Home Assistant | Home Assistant OS on a Raspberry Pi 5, `<ha-ip>:8123`; `/config/www/` is served at `http://<ha-ip>:8123/local/` without a token, which makes HA the OTA image host and the WAV host for `/speak?url=` | any HA install reachable from the device; `OTA_URL_BASE=http://<ha-ip>:8123/local/doorbell`, `OTA_PUBLISH="scp {rbl} root@<ha-ip>:/config/www/doorbell/"` (or via the jump host: `scp {rbl} user@bench:/tmp/ && ssh user@bench scp /tmp/{name} root@<ha-ip>:/config/www/doorbell/`) |
| Tokens | `DOORBELL_TOKEN_FILE` pointing at a dotenv with `TOKEN=...`; HA long-lived token in `~/.ha_token` on whichever host runs `tools/ha_generic_camera.py` | same, see `scripts/env.example` |
| Out-of-tree data | `~/data/doorbell-bk7258/`: the 8 MB stock dump and its disassembly, `firmware/` for built images (`FW_DIR`), `avdk-build/` for the build tree (`OUT`), pin-mapping session logs, audio captures | `FW_DIR` and `OUT` anywhere outside the repo; keep your dump directory private |

## Audio and LED rig

- Ear: `ffmpeg -f avfoundation -i ":1" -t 8 -ac 1 -ar 16000 out.wav` records the laptop
  microphone (on macOS run it from Terminal.app once to get the microphone permission).
  `tools/mac_ear.py rec.wav --labels ...` analyses a recording that has a 500 Hz marker beep before
  every test (peak/rms per bin, dominant frequency by Goertzel).
- Voice: `afplay` plays test tones and markers at the device; any player works.
- Eyes: the laptop camera (`imagesnap` in a loop, or `tools/led_by_camera.py`) to see which LED a
  `pin blink N` lights. `tools/led_by_light.py` tried to use the board's own light sensor and supply
  sense as the eyes; neither reacts to the LEDs on this board.
- Mic captures from the device: `mic rec MS HOST PORT` streams raw FIFO words to a TCP peer
  (`tools/audio_relay.py recv PORT file.raw` on the bench host), `tools/mic_raw.py cap.raw --seconds 4
  --wav out.wav` exports them.

## Typical session

```bash
python3 tools/serial_capture.py 115200 60 boot.log      # watch UART0 for a minute
python3 tools/shell.py sys info                          # one command over UART
python3 tools/dbcmd.py sys info                          # same over TCP (needs tok set once)
scripts/release.sh                                       # build, publish, OTA, no cable
ffplay -rtsp_transport tcp rtsp://<device-ip>:554/live   # watch the stream
```
