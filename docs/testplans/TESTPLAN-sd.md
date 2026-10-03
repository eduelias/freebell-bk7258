# TESTPLAN — feature `sd` (optional microSD storage), branch `feat/sd`

Firmware: `doorbell-fw_0.19.3_app_pack.rbl` built from this worktree (the integrator renumbers).
All commands below are `python3 tools/dbcmd.py <cmd>` from the repo (pyenv python
`~/.pyenv/versions/3.11.13/bin/python3`), unless marked UART. Replies come back over TCP (the
`REPLY()` text, max 320 bytes); the UART shows the same plus `[sd] ...` worker lines and the
SDK's `sd_card` / `sdio_host` / `fatfs` logs.

## What was implemented

- `project/main/sdcard.c` + `sdcard.h` (new), hooks in `app_main.c`: `#include "sdcard.h"`,
  `sd_init()` in `user_app_main()`, `sd_on_event("ring"|"motion")` in `reporter_task`, helpers
  `app_cam_grab()` (latest JPEG, opens the camera on demand), `app_net_up()`, `app_reply()`,
  command table entry `sd`. FW_VERSION 0.19.3.
- SD host: SDK `sd_card` driver over the BK7258 SDIO host, 1-bit mode, **GPIO 2 = CLK, 3 = CMD,
  4 = DAT0** (SDK default pin group `GPIO_DEFAULT_DEV_CONFIG`; the stock firmware ran the same SDK
  drivers - its strings contain `bk_sd_card_init`, `sdio_host`, `mount on sdcard`, `ty_app_sdcard.c`).
  Card power **GPIO 6 active high** (stock `ty_app_sdcard.c`; it is also one of the four rails
  `cam_rails()` raises, so it is high whenever the camera has been opened). We never lower it unless
  asked (`sd umount off`, `sd power off`).
- FatFs volume `1:` (`components/fatfs/disk_io.c` maps the SD card to physical drive 1), directory
  `1:/doorbell`, files `YYYYMMDD-HHMMSS-<event>.jpg` (local time = UTC + `sd tz` minutes) or
  `up<uptime_s>-<event>.jpg` before the first time sync. FAT directory timestamps are real too
  (SDK patch `patches/0007-fatfs-weak-get-fattime.patch` makes the SDK's fixed-2017 `get_fattime()`
  weak; `sdcard.c` supplies the real one).
- Time: built-in SNTP client (UDP/123) to `sd ntp HOST` (default `pool.ntp.org`, needs DNS), first
  try ~15 s after the IP is up, retry every 60 s, re-sync every 6 h; `sd time EPOCH` sets it by
  hand; `sd sync` forces a query.
- One low-priority worker task (`sdcard`, Beken priority 8 = below all app tasks, 4 KB stack) owns
  mount/umount/format/save/NTP via a 6-slot queue. Ring/motion only enqueue (`rtos_push_to_queue`
  with timeout 0) and never block; `sd ls`/`sd status` run in the caller behind a 500 ms mutex
  timeout ("busy" otherwise).
- Config change in both flavours (`project-avdk/config/bk7258/config`, `project/config/bk7258/config`):
  `CONFIG_SDCARD_POWER_GPIO_CTRL=n`. With the SDK default (=y) the FatFs glue would drive **GPIO 13**
  (`SDCARD_LDO_CTRL_GPIO`) high on every disk init and power the card down from a timer after 10 s
  idle. GPIO 13 has no known role on this board; we do not want to touch it.

### Commands (all KV-persisted settings have safe defaults)

| Command | Effect | KV |
|---|---|---|
| `sd` / `sd status` | two lines: card present/mounted, capacity/free MB, auto, saved/fail/drop counts, last file; pins, power pin, time + source, NTP stats, BUSY flag, last worker result | |
| `sd mount` | power pin high, map SDIO pins, `bk_sd_card_init`, `f_mount("1:")`, mkdir `/doorbell`; waits up to 4 s for the result | |
| `sd umount [off]` | `f_mount(NULL)` + `bk_sd_card_deinit`; `off` also drives the power pin low | |
| `sd format` | prints a 4-hex-digit token; `sd format TOKEN` within 120 s runs `f_mkfs("1:", FM_ANY)` in the background (FAT32 <= 32 GB, exFAT above), then remounts | |
| `sd ls [DIR] [SKIP]` | lists `1:/doorbell` (or `DIR`, relative to `1:/`), 6 entries per call, `SKIP` to page | |
| `sd save [EVENT]` | grab the latest JPEG (opens the camera if needed) and write it; waits up to 5.5 s | |
| `sd auto on\|off` | automatic save on ring and motion (also retries a mount at most once a minute when a card appears) | `db_sd_auto` default `1` |
| `sd pwr PIN\|none` | card power GPIO | `db_sd_pwr` default `6` |
| `sd pins 1\|0` | SDIO pin group: 1 = GPIO 2/3/4 (default), 0 = GPIO 14/15/16 | `db_sd_pins` default `1` |
| `sd power on\|off` | drive the power pin directly (diagnostic) | |
| `sd ntp HOST` | SNTP server (name or IP) | `db_sd_ntp` default `pool.ntp.org` |
| `sd tz MIN` | minutes east of UTC for file names/timestamps (Amsterdam: 120 in summer, 60 in winter) | `db_sd_tz` default `0` |
| `sd time EPOCH` | set the clock by hand (unix seconds UTC) | |
| `sd sync` | SNTP query now, waits up to 6 s | |

Boot behaviour: 8 s after boot the worker tries one mount when `sd auto` is on (default). With no
card that is a handful of CMD0 timeouts (expect `no card (init -N, ~100 ms)` in `sd status`), after
which it retries every 10 minutes and on every ring/motion (at most once a minute). Nothing else is
touched; the rest of the firmware is unchanged when no card is present.

## Test plan (integrator)

Prerequisites: device on 192.168.2.89 running the merged image; a microSD card (FAT32 formatted on a
PC is the easy path; a blank card can be formatted on the device). Owner-only steps are marked.

### T1 - no-regression without a card (first, before any card is inserted)
1. `sd status` -> `card absent unmounted cap 0 MB ... auto on ...`, second line
   `pins 2/3/4 pwr 6 time ... : no card (init -<n>, <ms> ms)` once 8 s of uptime have passed.
   Expected: init time well under 1 s, firmware otherwise normal (`ring`, `cam stat`, HTTP snapshot).
2. UART (optional): the boot banner is followed by `[sd] sd host + fatfs, auto on, pins 2/3/4, pwr 6,
   ntp pool.ntp.org, tz 0`, later `[sd] no card (init ...)` and, ~15 s after the IP,
   `[sd] ntp: time 2026-10-03 ...`.
3. `ring test` -> chime/LEDs as before; `sd status` shows `drop 0` and no new save attempts
   (not mounted + the 60 s automount gate: at most one `no card` line per minute of events).
4. `sd sync` -> `[sd] ntp pool.ntp.org: time 2026-10-03 HH:MM:SS (1/1 ok)`. If DNS fails on this
   LAN: `sd ntp 192.168.2.80` is NOT an NTP server; use a router/pool IP, e.g. `sd ntp 162.159.200.1`
   (time.cloudflare.com), or `sd time $(date +%s)`.
5. `sd tz 120` (CEST) -> file names in local time from now on.

### T2 - card insert (owner: insert a FAT32 microSD, power can stay on)
1. `sd mount` -> `[sd] mounted <cap> MB, <free> MB free (<ms> ms)`. On UART the SDK prints
   `sd card init ok`-style lines from `sd_card` and `sdcard sector cnt=<n>` from FatFs.
   - `no card (init ...)` with a card inserted: the pin or power guess is wrong. Try in order:
     `sd power on` then `sd mount` again (GPIO 6 should already be high after any camera use;
     `pin get 6` to check); `sd pins 0` + `sd umount` + `sd mount` (SDIO group 0 = GPIO 14/15/16);
     `sd pwr none` if GPIO 6 turns out to be something else. Report which combination worked.
   - `card N MB has no FAT filesystem (fr=13)`: card is present but blank -> T4.
2. `sd ls` -> `[sd] 1:/doorbell: 0 entries ...` (directory was just created).
3. `sd save test` -> `[sd] saved 20261003-171500-test.jpg (88123 B, 450 ms, 29123 MB free)`
   (camera cold start adds ~1-2 s; `still running` means it exceeded 5.5 s: run `sd status` a few
   seconds later).
4. `sd ls` -> the file with its size; `sd status` -> `saved 1 fail 0`.

### T3 - automatic snapshots
1. `ring test` (hooked like a real press; `hook ring` only sends the webhook) or the real button
   -> within ~2 s `sd ls` shows `...-ring.jpg`; the chime/LED timing is unchanged (worker is lower
   priority than `ringfx`).
2. Wave at the PIR -> `...-motion.jpg` (PIR is debounced 3 s in the ISR; the save happens before the
   HA webhook POST is sent).
3. Two presses within one second -> second file gets `-1` suffix (no overwrite).
4. `sd auto off`, press -> no new file; `sd auto on`.

### T4 - format (destructive; only on a scratch card)
1. `sd format` -> `[sd] ERASES THE CARD. Confirm within 120 s with: sd format 3f2a`.
2. `sd format 3f2a` -> `formatting (FM_ANY) in the background; poll sd status`. A 32 GB card takes
   roughly 10-60 s at 1-bit/13 MHz; `sd status` shows `BUSY` and `sd ls` answers `busy` meanwhile,
   then `formatted (N s) and mounted, M MB free`.
3. Wrong/expired token -> a fresh token is printed instead; nothing is erased.

### T5 - hot removal / reinsertion (owner)
1. Pull the card, `sd save` -> `save: open failed fr=1/3, card dropped (removed?)` (or write
   failed); `sd status` -> `card absent unmounted`.
2. Reinsert; next ring/motion (or `sd mount`) remounts (auto path: at most once a minute).
3. Card contents on a PC: `doorbell/` folder, JPEGs open, timestamps match the file names.

### T6 - stress / coexistence
1. Keep the HA MJPEG stream open (camera.doorbell_bk7258 live view) and run 5x `sd save` -> all
   saved, stream keeps running (the save copies the frame out of the shared buffer under the same
   mutex the HTTP server uses).
2. `mic level 1000` / `spk tone 1000 300` during a save -> no audio glitches expected (SDIO is CPU
   FIFO driven, no DMA channel taken).
3. `memshow` (UART) before/after 20 saves -> no heap growth (frame buffer is one 200 KB PSRAM
   block, FatFs LFN buffers are freed per call).

### Rollback
- `sd auto off` stops all automatic card activity; `sd umount off` releases the card and lowers
  GPIO 6 (note: GPIO 6 is also raised by the camera open sequence, so the camera path will raise it
  again). No other firmware behaviour depends on the feature; OTA back to the previous image removes
  it entirely. KV keys left behind (`db_sd_*`) are harmless.

## Home Assistant

None required. Optional: expose the last worker result by adding `"sd":"<result>"` to the report
JSON - not done here to keep the merge trivial. Suggested follow-up once the card works: a
`sd http` listing endpoint on the existing :8080 server so HA can link to saved snapshots.

## Not verified on hardware (no device changes allowed in this worktree)

- SDIO pin group and power pin are the best-supported guess from the SDK default map, the boot pin
  state (GPIO 0-4 carry pull-ups at boot = SDIO/I2C default functions) and the stock strings; T2
  step 1 covers the alternatives.
- GPIO 6 might also feed something the camera uses (pwrscan 2026-10-03 saw GPIO 42 follow it); that
  is why the code never lowers it on its own.
- Unformatted/exFAT cards: `f_mount` returns FR_NO_FILESYSTEM for a blank card (handled); exFAT is
  compiled in (FF_FS_EXFAT=1), so >32 GB cards formatted on a PC should mount too.

## Results 2026-10-03 22:17 (v0.24.0 on the device, no card inserted)

Merged as `fd3f1fa`; two merge fixes before it built: the duplicate `app_reply()` (talk and sd
both added one) and `app_cam_grab()` calling the removed `cam_do_open()` (now `cam_want_jpeg()`,
falling back to the last JPEG while an RTSP viewer holds H.264). OTA via `release.sh --no-build`,
device back in ~50 s.

- T1 no-card path PASS: UART `[sd] sd host + fatfs, auto on, pins 2/3/4, pwr 6` at boot, probe
  at +8 s = `sd card unsupport voltage` + `acmd41 retry time out`, `[sd] no card (init -14600,
  1170 ms)`; `sd status` shows `card absent unmounted ... no card (init -14600, 1170 ms)`.
- NTP PASS: first sync 15 s after IP, `sd sync` -> `2/2 ok`; `sd tz 120` applied immediately.
- `sd save test` without a card -> `save: not mounted (` + "`sd mount`" + `)`, no crash.
- Camera `cam close` + HTTP snapshot x3 -> 200 in 1.4-2.0 s each (rail power-cycle path with
  `sd_holds_gpio()` inactive because no card), frames flowing after; heap 108 KB flat; pir/pwr/rtsp
  status unchanged. No fault/assert lines on UART.
- Not run tonight (owner asleep, would push phone notifications): ring/motion auto-save. T2-T5 need
  a card.
