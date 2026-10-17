# Flashing over UART and the BootROM download window

The BK7258's download mode lives in mask ROM, so no flash content can brick the chip: as long as
you can reach UART0 you can always write a new image. Everything below is about *getting into*
that window and what the tools do once there.

## Wiring

The 4-pin header next to the battery connector (`5V TX RX GND`) is UART0: BootROM download mode
and the firmware's log/shell, both 115200 8N1, 3.3 V logic. Device `TX` -> adapter RXD, adapter
TXD -> device `RX`, GND -> GND. Leave `5V` unconnected. A loose GND makes the line float and shows
up as streams of `FF` bytes; a slipped adapter-TX wire shows up as "the ROM is not answering".

## Opening the ROM window

`scripts/flash.sh` (a wrapper around `tools/bkwrite.py`) waits for the BootROM to answer its
link-check. What opens the window:

- **A real power-on** with battery *and* USB unplugged first, then connect the battery and press
  the doorbell button when the tool says so. Pulling the battery does **not** power-cycle the
  board while the USB charger is connected.
- **`scripts/flash.sh --write --kick`** with firmware v0.2.0 or later running: the tool sends our
  `reboot` command (`bk_reboot()`, the NMI-watchdog reset path with the reboot tag), the ROM
  answers link-check within a second, and the flash proceeds hands-free. `reboot` restores the
  pristine boot GPIO configuration first so the window stays reachable.

What does *not* open it (do not bother): the Beken bkreg reboot frame (ignored by the SDK shell),
a deep-sleep RTC wake-up (resets the chip but the ROM skips the window), the doorbell button while
the firmware runs (soft reset, same), and the SDK's `memset` crash trick (`--kick-wdt`; the SDK
command silently refuses).

## What the writer does

`tools/bkwrite.py` clears only the flash's write-protection bits (keeping quad-enable), skips
identical sectors, reads back every written sector, restores protection, and refuses to touch
`0x7C0000-0x800000` (the stock per-device KV area and RF calibration) unless `--allow-tail`.
`scripts/flash.sh` without `--write` is a dry run that only compares sectors.

After flashing the chip stays parked in the BootROM (the link-check traffic keeps it there). Boot
the new image with `python3 tools/bkreboot.py` (ROM `CMD_Reboot` 0x0E/0xA5, then it prints 30 s of
boot log; `--quiet` to skip the log). Watch the log any time with
`python3 tools/serial_capture.py 115200 60 boot.log`.

## Restoring the stock firmware

```bash
cd tools && python3 bkwrite.py <your dump directory>/doorbell_flash_full.bin --start 0x0 --write --allow-tail
```

Only do this with *your own* dump of *this* device: the tail holds its RF calibration and Tuya KV
data. Keep the dump private.

## Partitions and the AVDK build

The flash layout the application assumes (`src/vendor_flash_partition.h`) is mirrored by
the packaging CSVs in `project/config/bk7258/*.csv`. After touching those, delete
`$OUT/doorbell/bk7258_cp1` and `_cp2`, otherwise the CPU1 image keeps its old flash origin and
never boots (`cp0 boot cp1 time out`). The same applies after changing Kconfig options in
`project/config/bk7258_cp1/config`: delete `$OUT/doorbell/bk7258_cp1`, `_cp2` and `bk7258`
before rebuilding. The OTA image (`app_pack.rbl`) carries all three cores.

The build rewrites `project/config/bk7258/config` (sorts keys, strips comments), so option choices
are documented in `docs/notes/sdk-patches.md`, not in that file.

## OTA

Each build also produces `doorbell-fw_<ver>_app_pack.rbl` (compressed + encrypted application,
about 0.9 MB) in `$FW_DIR`. `ota URL` uses the SDK's HTTP OTA client: it streams the file into the
`ota` partition (0x286000, 1428 KB) and reboots; the bootloader installs it, about 35 s end to end
on a LAN. The bootloader only switches after a complete, verified download, so a bad or
interrupted download leaves the running image in place; worst case the UART + ROM path above still
works. SDK patch 0004 is needed when the image host is Home Assistant (its aiohttp sends a `Server:`
header with an empty value, which the stock header parser rejected with `[ota] result -4`).
