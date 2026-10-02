# doorbell-bk7258-fw

Custom firmware for a refunded Tuya "Smart Visual Doorbell" (board `IP108_MAIN_V05`, Tuya product
IP10B) that never paired: it shipped without a Tuya licence, and the licence can only be issued by
the OEM that owns its PID. So this replaces TuyaOS with our own firmware on Beken's open Armino SDK.

Teardown notes, the verified 8 MB stock flash backup and its analysis live outside the repo in
`~/data/doorbell-bk7258/` (the backup contains device-specific data: never commit or upload it).

## Hardware

| Part | Detail |
|---|---|
| SoC | Beken BK7258 (BK7236 family, Tuya "T5"), 3 cores |
| Flash | GigaDevice GD25WQ64E, 8 MB (MID 0x1765C8) |
| PSRAM | AP Memory APS128XXO-OB9, 16 MB (id 0x8D08) |
| Power | 3.7 V Li-ion pack, charger IC; camera on FPC, PIR, speaker |

Serial: the 4-pin header **next to the battery connector** (`5V TX RX GND`) is UART0. It carries the
BootROM download mode and our firmware's log/shell, both at 115200 8N1, 3.3 V logic.
Device `TX` -> adapter RXD, adapter TXD -> device `RX`, GND -> GND. Leave `5V` unconnected.
A loose GND makes the line float and shows up as streams of `FF` bytes.

## Build

Needs podman (with Rosetta on Apple Silicon) and the SDK checkout at `~/reps/oss/bk_idk`
(`git clone --depth 1 --branch release/v2.0.1 https://github.com/bekencorp/bk_idk.git`).

```bash
scripts/build.sh
```

This applies `patches/*.patch` to the SDK if needed, mounts `project/` over the SDK's
`projects/app`, runs `make bk7258` in `bekencorp/armino-idk:1.5`, and copies the image to
`~/data/doorbell-bk7258/firmware/doorbell-fw_latest.bin`. A full build takes ~11 min under emulation.

## Flash

```bash
scripts/flash.sh            # dry run: compares sectors, writes nothing
scripts/flash.sh --write    # flash the latest build
```

The tool waits for the BootROM: press the doorbell button (battery connected) when it says so.
It only clears the flash's write-protection bits (keeping quad-enable), skips identical sectors,
reads back every written sector, restores protection, and refuses `0x7C0000-0x800000`
(stock per-device KV and RF calibration) unless `--allow-tail`.

To boot the new image, unplug and replug the battery. Watch the log with
`python3 tools/serial_capture.py 115200 60 boot.bin`.

### Restore the stock Tuya firmware

```bash
cd tools && python3 bkwrite.py ~/data/doorbell-bk7258/doorbell_flash_20261002.bin --start 0x0 --write --allow-tail
```

The BK7258's download mode lives in ROM, so no flash content can brick it.

## SDK patches

- `0001-psram-use-configured-chip-id.patch`: with no cached PSRAM id, the stock driver probes the
  8 MB APS6408L first, which hangs this board right after the shell prompt. With
  `CONFIG_PSRAM_AUTO_DETECT=n` and `CONFIG_PSRAM_APS128XXO_OB9=y` (in
  `project/config/bk7258/config`) it now uses the configured part instead.

## Status

- [x] Stock flash backed up and verified; read-only and write tools
- [x] SDK sample app builds, flashes and boots to the shell prompt
- [x] PSRAM fix verified (2026-10-02): boots past the old hang, banner + 5 s heartbeat on UART0,
      shell answers (`help` lists 79 commands: wifi `net sta`, `ping`, `iperf`, `ble`, `pm`,
      `memshow`, `mqttsend`), `psram_malloc 65536` returns 0x60700020
- [ ] Map GPIOs: LEDs, button, PIR, chime, battery sense, power hold
- [ ] Wi-Fi join, then MQTT/HTTP to Home Assistant (ring, motion, battery)
- [ ] Camera (DVP/MIPI sensor on the FPC) and JPEG/H.264 snapshot or stream
