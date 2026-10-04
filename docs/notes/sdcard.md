# microSD storage (v0.24.x, `project/main/sdcard.c`)

Optional: a FAT32/exFAT microSD card in the slot gets a JPEG per ring and motion
(`/doorbell/YYYYMMDD-HHMMSS-<event>.jpg`, local time once SNTP has synced, `up<uptime>-...`
before). The card is mounted only around a save (0.24.3): an event opens the camera, mounts, writes,
keeps the card for `sd keep S` (30 s) after the last save, unmounts. Without a card nothing touches
the bus (`cd` = GPIO 6 reads 1).

## Hardware

SD host = the BK7258 SDIO host, 1-bit, **GPIO 2 = CLK, 3 = CMD, 4 = DAT0** (SDK default pin group,
shared with the stock SDK build). **GPIO 6 = card detect**: input with pull-up, 0 = card inserted
(stock firmware disassembly 2026-10-04, verified on the board).

**Card power = the camera supply (GPIO 28 rail).** Verified 2026-10-04 with the camera closed: GPIO 28
low -> `sd probe` gets no CMD8 answer (bus lines clamped low); `pin out 28 1` -> CMD8 `0x1AA`, CMD55
`0x120`, with GPIO 13 released, driven high or driven low alike. **GPIO 13 does not power the card**:
it is the battery ADC15 node only. The stock's SDK glue drives it as `SDCARD_LDO_CTRL_GPIO` via
`bk_gpio_ctrl_external_ldo(GPIO_CTRL_LDO_MODULE_SDIO, GPIO_13, HIGH)` (SDK default pin for a board
Beken never had), 0.24.2 copied that and the first card tests "worked" only because HA snapshots had
the camera open. `sd pwr PIN` stays for other boards, default `none`; a stored 6 or 13 is migrated.

**Second dependency: the SDK's `bk_sd_card_init()` only completes while the media pipeline is up.**
With the camera closed and GPIO 28 held high the card answers every 100 kHz probe command, but the
init times out (`-14594`) at some later step, with or without pre-conditioning, with or without the
speaker (audio domain) running. With the camera open it completes in ~100 ms. Register dump camera
closed vs open: sys `cpu_clk_div_mode1` `0x44100037` -> `0x44100030` (core divider 7 -> 0, i.e. the
CPU runs at 1/8 clock when nothing votes), power domains `0x600001c8` -> `0x60000148` (`pwd_vidp`
off -> on), `jpeg_cken` + reg 0x0d clocks on; the SDIO clock select itself (XTAL, reg 0x09) is the
same in both. 0.24.3 therefore opens the JPEG camera (`app_sd_cam_ready()` = `cam_want_jpeg()`; the
save needs the frame anyway) and votes `PM_DEV_ID_SDIO` -> 320 MHz before the init; `sd pre` bits 16
and 32 disable either for experiments (results in "Status" below).

SDIO pin setup (`sd_pins_apply()`) reproduces the SDK's `GPIO_DEFAULT_DEV_CONFIG` rows for GPIO 2/3/4
exactly: release the pin (`gpio_dev_unmap`), `bk_gpio_set_config(GPIO_IO_DISABLE, PULL_UP_EN)`, then
`gpio_sdio_one_line_sel(mode 0)` (the SDK's own `sdio_host_init_gpio()` call; it maps only
CLK/CMD/DATA0 = GPIO 2/3/4, never DATA1-3 = GPIO 5/10/11, so the console UART is safe), pull-up
again (the map clears `pull_en`) and drive strength 3. Expected config word `0x378` (`pin cfg 2`).

## Design

One low-priority worker owns the card; ring/motion only enqueue a job and never block. `ring test`
queues an SD snapshot like a real press. On-demand mount (`sd_do_save` -> `sd_do_mount` ->
`sd_card_bringup`): camera ready, CPU vote, power pin (if any) + 500 ms, SDIO pins, pre-condition
(`sd_precondition`: fresh host driver at 100 kHz, CMD0, CMD0, CMD8 0x1AA; a silent card gets one
power cycle and a second look, then fails fast with "silent" instead of the SDK's 128 blind CMD8
retries), `bk_sd_card_init` (host driver deinit/init first = fresh ISR + completion queue; one retry
after a power cycle), `f_mount`, `mkdir /doorbell`. The 200 KB PSRAM frame buffer lives only for one
save (holding it starved the :8080 server, 0.24.0/1). After each save the keep window restarts
(`sd keep S`, KV `db_sd_keep`, default 30 s, min 5); when it ends the worker unmounts, deinits the
card, releases the vote and the power pin. `sd mount` pins the card ("manual" in `sd status`) until
`sd umount`.

**Automatic saves (0.24.4):** a ring/motion job carries the tick of its event. The worker never
runs one before uptime 3 s (`SD_EARLY_MS`: after a deep-sleep button wake the ring is handled at
~0.5 s, before `media_service_init()`/the camera exist, and the SDK card init fails); such a job is
*deferred* to 3 s. A mount or camera-frame failure is *retried* with 2/4/8 s backoff (3 retries),
not dropped; deferred/retried jobs wait in 3 worker-side slots (not in the queue) and keep the
event name and tick, so the file name is the event's time (`up<N>-ring.jpg` while the clock is
unsynced, the real stamp if SNTP finished before the retry succeeded). A fresh event while a retry
is pending joins it (one mount for both). Only when a chain has given up (`fail` +1) do fresh
events skip the card for 60 s. Everything not saved is a counted `drop` with the reason in
`sd status`: `duplicate +N ms` (same event within 2 s, e.g. a bounce past the ISR debounce or
`ring test` twice), `gave up Ns ago`, `no card (cd=1)`, `sleeping`, `retry slots full`, `queue
full`. `sd save` (manual) is unchanged: synchronous, one attempt. `sd_prepare_sleep()` (called from `app_power_prepare_sleep()`) unmounts before deep sleep.
FAT timestamps are real thanks to SDK patch 0008; SDK patch 0009 makes the host driver's response
wait (30 ms) outlast the 25 ms hardware timeout so the ISR queue cannot get out of step. A tiny SNTP
client (UDP/123, needs DNS for host names) runs once the network is up.

## Commands

| Command | What it does | KV, default |
|---|---|---|
| `sd` / `sd status` | line 1: card in/out (`cd`), `cam` open, CPU `vote` held, power `pin` state, mounted / `manual` / `keep N s left`, last mount capacity/free/time + mount ok/fail counts, cold save time, saved/fail/drop (+ last drop reason), defer/retry counts, pending retries, last file; line 2: auto, keep, pwr, pre, pins, time + source, NTP stats, BUSY/SLEEP, last worker result; line 3: `bringup:` trace of the last mount (cam/vote/pre results, init return and ms) | |
| `sd mount` / `sd umount [keep]` | mount (see "Design") and pin the card until `sd umount`; `umount keep` leaves the power pin high | |
| `sd save [EVENT]` | mount on demand if needed, write the latest camera JPEG as `/doorbell/<stamp>-EVENT.jpg`, restart the keep window | |
| `sd keep S` | seconds the card stays mounted after the last save (min 5) | `db_sd_keep`, 30 |
| `sd pre FLAGS [GAP_MS]` | pre-condition tuning: 1 also CMD55, 2 also CMD1, 4 skip the pre-condition, 8 power cycle first, 16 do not open the camera, 32 do not vote the CPU clock; GAP = ms between the pre-condition's host deinit and `bk_sd_card_init` | `db_sd_pre`, 0,10 |
| `sd ls [DIR] [SKIP]` | list `1:/doorbell` (or DIR relative to `1:/`), 6 entries per reply, `SKIP` pages | |
| `sd format` | prints a token; `sd format TOKEN` within 120 s runs `f_mkfs` (FAT32 <= 32 GB, exFAT above) in the background, then remounts | |
| `sd auto on\|off` | auto-save on ring and motion | `db_sd_auto`, on |
| `sd pwr PIN\|none` | optional card power GPIO for other boards (refuses 6 = card detect, the SDIO pins and 10/11); driven high while the card is in use, low 200 ms then released after. **Not needed on this board** (the slot is on the camera supply; 13 is the battery ADC). `sd pins` (GPIO 14/15/16 group of <= 0.24.1) is gone: it double-mapped the SDIO functions and GPIO 15 is the charging input | `db_sd_pwr` none (a stored 6 or 13 is migrated to none) |
| `sd power on\|off` | drive the power pin directly and pin it (diagnostic; refused while the card is in use) | |
| `sd probe [cmd\|pins\|lines\|regs]` | host-level diagnostics without FatFs, run in the worker. `pins`: apply the SDIO pins and reply the AON config word / function-select nibble of CLK/CMD/DAT0, GPIO 6 (+ level) and GPIO 13 (+ level), plus the sys clock registers. `lines`: each SDIO pin as a plain input with pull-up then pull-down (1/0 = floating, 0/0 = held low = unpowered card or short, 1/1 = held high). `cmd` (default): power pin high 500 ms if set, pins, `bk_sdio_host_init` 100 kHz 1-bit, SDIO host registers, then CMD0, CMD0, CMD8 (0x1AA), CMD55, CMD1 (MMC), each with its return code (`-14594` = response timeout), response word or raw status register, `LATE` when the response arrived after the driver's wait; `regs` = init + registers only. Needs the card unmounted and the camera open (or `pin out 28 1`), otherwise the card is unpowered and every command but CMD0 times out | |
| `sd ntp HOST` / `sd sync` | SNTP server / query now | `db_sd_ntp`, pool.ntp.org |
| `sd tz MIN` / `sd time EPOCH` | minutes east of UTC for names and FAT timestamps (e.g. Amsterdam 120 in summer, 60 in winter) / set the clock by hand | `db_sd_tz`, 0 |

## Status (2026-10-04 evening, v0.24.4)

First real deep-sleep button wake on 0.24.3: wake reason, chime, HA boot/wake/ring webhooks and the HA
ring photo all fine, but `sd status` said `saved 1 fail 1 drop 2`: the ring save failed, two entries were
dropped, only a PIR motion at +70 s saved. Root cause: `power_init()` -> `app_power_wake_input()` sets
`s_pend_fx`, the reporter task's first loop runs `sd_on_event("ring")` at ~0.5 s uptime, the worker
mounts at once, but `media_service_init()` has not run yet (it follows `bk_init()` in `main()`), so the
camera cannot open, the card has no supply (camera rail) and `bk_sd_card_init` fails -> `fail 1` and
the old "skip the card for 60 s after a failed mount" rule; the next two automatic events inside that
minute (PIR edges while the visitor stood there) were counted as `drop` without a reason. Fix = the
"Automatic saves" paragraph in Design. 2 builds (111/96 s, -Werror clean), 1 OTA (back in 42 s).

Tests (owner present, battery, sleep off; `tools/dbcmd.py` only):
- Post-boot ring (`reboot`, `ring test` the instant the TCP channel answered = uptime 3-4 s; the
  channel is never up before ~3 s so the <3 s deferral itself could only be exercised through the
  same slot mechanism as "joins"): 4/4 saved, cold save 1854-2058 ms (mount ~1.56 s incl. camera
  open), fail 0, drop 0, `up00000003-ring.jpg` (-1/-2 suffixes across reboots: uptime stamps collide)
  and `20261004-164449-ring.jpg` once NTP had already synced (stamp = event time, "event +2 s").
- Dedupe: `ring test` twice 1.3 s apart -> one save, `drop 1 (ring: duplicate +1330 ms)`. `motion test`
  twice is already filtered by pir.c (10 s cooldown / 5.5 s warm-up after boot).
- Induced failure (`sd umount`, `cam close`, `sd pre 16` = card unpowered like the wake): attempts at
  T+0/+3/+8/+17 s (950 ms each, `init -14594`), a second ring at T+5 joined (`defer 1`), give-up at
  T+18 -> `fail 1`, joined ring `drop (ring: gave up 0s ago)`, a ring at T+30 `drop (ring: gave up 11s
  ago)`, `retry 3`. `sd pre 0 10` restored, reboot, next ring saved normally.
- HTTP `/snapshot.jpg` 200 (0.05-0.10 s) during the keep window; heap 106.8-107.2 KB across 5 reboots.

## Status (2026-10-04, v0.24.3)

Everything through `tools/dbcmd.py` (no UART), card B (new 32 GB, FAT as shipped), device on battery
with sleep off. 3 builds (96-109 s), 3 OTAs (device back in ~60 s each).

- **Post-boot mount fixed, 4/4** (one OTA boot + three `reboot`s, cold `sd save` right after boot
  with the defaults, no manual probe): mount 1554-1566 ms, of which ~1.2 s is the camera open, cold
  save 1840-2042 ms end to end. Warm cold-saves (camera already open) mount in ~250-750 ms and save
  in ~420-960 ms; a save on a mounted card takes 40-75 ms.
- **What was wrong** (0.24.2 and build 1 of 0.24.3): the card has no power unless the camera supply
  (GPIO 28) is up, and the SDK init only completes with the camera open. Every "it works after an
  `sd probe`" today was an HA snapshot or an HTTP request opening the camera in between; GPIO 13 never
  mattered (see "Hardware"). Isolation runs, camera closed: GPIO 28 low -> card silent; 28 high alone,
  28+5, 28+5 with 13 low, 28+13 high: card answers CMD8/CMD55 at 100 kHz, `bk_sd_card_init` still
  `-14594` after ~4 s per attempt (also while a speaker tone ran = audio domain up); camera open ->
  init OK, with or without the pre-condition, with or without the CPU vote. CPU vote alone with 28
  held high: still fails. Register dump camera closed vs open in "Hardware".
- Keep window: `sd status` shows `mounted keep N s left`, then `unmounted`, vote released; the camera
  stays open afterwards (its own lifetime, unchanged). HTTP `/snapshot.jpg` 200 during the keep window
  (0.04-0.19 s) and after. `motion test` -> HA push + `...-motion.jpg` on the card. Heap 107.4-107.8 KB
  with the camera open, 110.5 KB closed, stable across the reboots.
- Battery: with `sd pwr none` GPIO 13 is never touched; `pwr status` adc15 1899-1903 mV at boot,
  1898 mV during a mount, 1895 mV 15 s after the keep window (3790-3806 mV), never `(cached)`.
- SDK patch 0009 (`SDIO_CMD_WAIT_TIME` 4 -> 30 ms) is in the build; it did not fix the mount by itself
  but removes the out-of-step completion queue seen on 0.24.2 (every command "timing out" in 84 ms).
- Device settings after the session: `db_sd_pwr` none (the hand-set 13 was migrated at boot), auto on,
  keep 30, pre 0,10, tz 120, rtsp on, cam mode auto, sleep off.

Open: *why* the SDK init needs the camera (VIDP power domain vs the full CPU clock; the 320 MHz vote
alone was not enough, the VIDP vote was not tried); a board photo around the slot and the GPIO 28
regulator; the camera keeps running after a save (pre-existing behaviour, costs battery: an idle
camera close would also unpower the card, which is fine when unmounted); an HTTP listing of the card
(`/sd/`) and an `sd` field in the report JSON; `sd format` needs a pinned mount (`sd mount` first).
