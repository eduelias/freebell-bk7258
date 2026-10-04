# microSD storage (v0.24.x, `project/main/sdcard.c`)

Optional: a FAT32/exFAT microSD card in the slot gets a JPEG per ring and motion
(`/doorbell/YYYYMMDD-HHMMSS-<event>.jpg`, local time once SNTP has synced, `up<uptime>-...`
before). Without a card the only activity is one probe 8 s after boot and every 10 min (a few CMD0
timeouts, about 100 ms).

## Hardware

SD host = the BK7258 SDIO host, 1-bit, **GPIO 2 = CLK, 3 = CMD, 4 = DAT0** (SDK default pin group,
shared with the stock SDK build). **GPIO 6 = card detect**: input with pull-up, 0 = card inserted
(stock firmware disassembly 2026-10-04, verified on the board; until 0.24.1 it was wrongly treated
as the card power pin and driven high by the camera rail code). Card power: **no pin by default**.
The stock's SDK glue (`disk_io.c` with `CONFIG_SDCARD_POWER_GPIO_CTRL=y`) drives **GPIO 13** high via
`bk_gpio_ctrl_external_ldo(GPIO_CTRL_LDO_MODULE_SDIO, GPIO_13, HIGH)` before every card init; GPIO 13
is also the battery ADC15 input, so whether it really powers the slot is unproven. `sd pwr 13`
reproduces that drive exactly (unmap, no pull, drive strength 0, output high, held while the card
is in use) and the battery read is skipped while the pin is held. FatFs volume `1:`. Our build has
`CONFIG_SDCARD_POWER_GPIO_CTRL=n` and calls `bk_sd_card_init` itself.

SDIO pin setup (`sd_pins_apply()`) reproduces the SDK's `GPIO_DEFAULT_DEV_CONFIG` rows for GPIO 2/3/4
exactly: release the pin (`gpio_dev_unmap`), `bk_gpio_set_config(GPIO_IO_DISABLE, PULL_UP_EN)`, then
`gpio_sdio_one_line_sel(mode 0)` (the SDK's own `sdio_host_init_gpio()` call; it maps only
CLK/CMD/DATA0 = GPIO 2/3/4, never DATA1-3 = GPIO 5/10/11, so the console UART is safe), pull-up
again (the map clears `pull_en`) and drive strength 3. Expected config word `0x378` (`pin cfg 2`).

## Design

One low-priority worker owns the card; ring/motion only enqueue a job and never block. `ring test`
queues an SD snapshot like a real press. FAT timestamps are real thanks to SDK patch 0008 (the
SDK's fixed-2017 `get_fattime()` becomes weak so `sdcard.c` can supply the SNTP time). A tiny SNTP
client (UDP/123, needs DNS for host names) runs once the network is up.

## Commands

| Command | What it does | KV, default |
|---|---|---|
| `sd` / `sd status` | card present/mounted, capacity/free MB, auto, saved/fail/drop counts, last file; pins, power pin, time + source, NTP stats, BUSY, last worker result | |
| `sd mount` / `sd umount [off]` | mount: `cd` must read 0 (else "no card (cd=1)" without touching the bus), power pin high + 500 ms if it was off, SDIO pins, SDIO host driver deinit/init (fresh ISR + completion queue, see "Status"), `bk_sd_card_init` with one retry after 300 ms, `f_mount`, mkdir `/doorbell` / unmount; `off` also lowers the power pin | |
| `sd save [EVENT]` | write the latest camera JPEG (opens the camera on demand) as `/doorbell/<stamp>-EVENT.jpg` | |
| `sd ls [DIR] [SKIP]` | list `1:/doorbell` (or DIR relative to `1:/`), 6 entries per reply, `SKIP` pages | |
| `sd format` | prints a token; `sd format TOKEN` within 120 s runs `f_mkfs` (FAT32 <= 32 GB, exFAT above) in the background, then remounts | |
| `sd auto on\|off` | auto-save on ring and motion (also remounts at most once a minute when a card appears) | `db_sd_auto`, on |
| `sd pwr PIN\|none` | card power GPIO (refuses 6 = card detect, the SDIO pins and 10/11); applied at the next mount. **13 on this board.** `sd pins` (the GPIO 14/15/16 group of <= 0.24.1) is gone: selecting it double-mapped the SDIO functions, so the host kept listening on the empty pins after switching back, and GPIO 15 is the charging input | `db_sd_pwr` none (a stored 6 from <= 0.24.1 is migrated to none) |
| `sd power on\|off` | drive the power pin directly (diagnostic) | |
| `sd probe [cmd\|pins\|lines\|regs]` | host-level diagnostics without FatFs, run in the worker. `pins`: apply the SDIO pins and reply the AON config word / function-select nibble of CLK/CMD/DAT0, GPIO 6 (+ level) and GPIO 13 (+ level), plus the sys clock registers. `lines`: each SDIO pin as a plain input with pull-up then pull-down (1/0 = floating, 0/0 = held low = unpowered card or short, 1/1 = held high). `cmd` (default): power pin high 500 ms if set, pins, `bk_sdio_host_init` 100 kHz 1-bit, SDIO host registers, then CMD0, CMD0, CMD8 (0x1AA), CMD55, CMD1 (MMC), each with its return code (`-14594` = response timeout), response word or raw status register, `LATE` when the response arrived after the driver's 4 ms wait; `regs` = init + registers only. Needs the card unmounted | |
| `sd ntp HOST` / `sd sync` | SNTP server / query now | `db_sd_ntp`, pool.ntp.org |
| `sd tz MIN` / `sd time EPOCH` | minutes east of UTC for names and FAT timestamps (e.g. Amsterdam 120 in summer, 60 in winter) / set the clock by hand | `db_sd_tz`, 0 |

## Status (2026-10-04, v0.24.2, first card ever mounted)

Bring-up session with two cards (card A, blank; swapped mid-session for card B, a new 32 GB), no UART,
everything through `tools/dbcmd.py`:

- **Pin mux was never the problem.** `pin cfg 2/3/4` = `0x378`, function select 1 = `SDIO_HOST_CLK/CMD/DATA0`
  (identical to the SDK default rows); the SDIO block reads id `0x5344494f` ("SDIO"), `sdio_cken`
  (bit 22 of sys reg 0x0c) comes on with `bk_sdio_host_init`, so the clock/power domain is fine too.
- **The card was unpowered.** `sd probe lines` with GPIO 13 at its boot state: CLK/CMD/DAT0 all read 0
  with pull-up *and* pull-down = clamped by an unpowered card; the SDK then "succeeds" CMD8 with an
  all-zero response (CRC7 of zeros is 0) and times out on CMD55 (`-14594` after ~630 ms). Without a
  card the same lines float and the SDK ends in `-14600` (bogus ACMD41 loop) after ~1170 ms.
- **GPIO 13 high powers the card.** `sd pwr 13` + `sd power on`: lines become CMD/DAT0 1/1 (pulled up by
  the card), CLK 1/0 (floating); `sd probe`: CMD8 `0x1AA`, CMD55 R1 `0x120`, CMD1 timeout (SD, not
  MMC); `sd mount`: 29820 MB, 29808 MB free, FAT as shipped; `sd save` writes 30-58 KB JPEGs in
  40-74 ms. Pin 13 low does *not* visibly cut the power within 60 s (card still answers CMD8 with
  `sd pwr none`), but a 2 s low pulse does reset a hung card (see the mount retry). The ADC15 node
  on the same pin reads ~1.9 V at rest, 240 mV right after being driven low, back to 1.9 V within
  10 s: a slow RC node, so battery reads are skipped (last value kept) while the pin is held.
- **Boot (open):** the auto-mount 8 s after every reboot failed on all 4 boots today (CMD8 retries,
  `-14594` after 0.7-3.7 s), and manual `sd mount` right after a boot failed too (`-14594`/`-14601`),
  with or without the power cycle the retry now does (pin low 2 s, high 500 ms). What recovered it
  every time (3/3): one `sd probe` (host init, CMD0, CMD0, CMD8, CMD55, CMD1, host deinit) followed by
  `sd mount` -> mounted in 114 ms; and once an `sd umount off` + 2 s + `sd mount`. The card answers the
  probe normally even while `bk_sd_card_init` keeps failing, so the difference is in the SDK init path
  (one CMD0 then 128 blind CMD8 retries every 4 ms against a 25 ms hardware timeout, which stacks
  completion messages). **Next change:** run the probe's command sequence as a pre-conditioning step
  in `sd_do_mount` before `bk_sd_card_init` (or patch `SDIO_CMD_WAIT_TIME` in the SDK), then re-test
  the boot mount. Until then, after a reboot: `sd probe` then `sd mount`.
- **Two firmware bugs found with the first real card:** (1) `sd_do_save` kept its 200 KB PSRAM frame
  buffer forever, starving `cam_http_client_task`'s `psram_malloc` so every :8080 request died with an
  empty reply after the first save (now freed per save; verified three saves with HTTP 200 between);
  (2) `sd pins 0` double-mapped GPIO 14/15/16 to the SDIO functions, the host kept reading CMD on
  GPIO 15 after switching back, and 15 is power.c's charging input (option removed).
- SDK quirk (not patched): the host driver completes commands through an RTOS queue fed by its ISR with
  a 4 ms software wait against a 25 ms hardware response timeout; after a burst of unanswered CMD8
  retries the queue was out of step (stale response words, every wait "timing out" in 84 ms). The
  mount now deinit/inits the host driver before `bk_sd_card_init` (fresh queue + ISR). A proper fix
  would be a patch raising `SDIO_CMD_WAIT_TIME` in `sdio_host_driver.c` above the hardware timeout.

Open: a photo of the board around U? (which regulator GPIO 13 enables); whether to drop the card power
between saves so the battery ADC works (the stock's SDK glue powered down after 10 s idle); an HTTP
listing of the card (`/sd/`) and an `sd` field in the report JSON. Device settings after the session:
`db_sd_pwr` = 13 (set by hand; the firmware default stays `none`), auto on, tz 120.
