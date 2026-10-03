# microSD storage (v0.24, `project/main/sdcard.c`)

Optional: a FAT32/exFAT microSD card in the slot gets a JPEG per ring and motion
(`/doorbell/YYYYMMDD-HHMMSS-<event>.jpg`, local time once SNTP has synced, `up<uptime>-...`
before). Without a card the only activity is one probe 8 s after boot and every 10 min (a few CMD0
timeouts, about 100 ms).

## Hardware

SD host = the BK7258 SDIO host, 1-bit, **GPIO 2 = CLK, 3 = CMD, 4 = DAT0** (SDK default pin group,
shared with the stock SDK build), card power **GPIO 6 active high** (from the shipped firmware's SD
code; GPIO 6 is also a camera LDO enable, so `cam_power(0)` leaves it high while a card is
present). FatFs volume `1:`. The SDK's own power-GPIO helper is disabled
(`CONFIG_SDCARD_POWER_GPIO_CTRL=n`: it would drive GPIO 13 and power the card down from a 10 s idle
timer).

## Design

One low-priority worker owns the card; ring/motion only enqueue a job and never block. `ring test`
queues an SD snapshot like a real press. FAT timestamps are real thanks to SDK patch 0008 (the
SDK's fixed-2017 `get_fattime()` becomes weak so `sdcard.c` can supply the SNTP time). A tiny SNTP
client (UDP/123, needs DNS for host names) runs once the network is up.

## Commands

| Command | What it does | KV, default |
|---|---|---|
| `sd` / `sd status` | card present/mounted, capacity/free MB, auto, saved/fail/drop counts, last file; pins, power pin, time + source, NTP stats, BUSY, last worker result | |
| `sd mount` / `sd umount [off]` | mount (power pin high, SDIO pins, `bk_sd_card_init`, `f_mount`, mkdir `/doorbell`) / unmount; `off` also lowers the power pin | |
| `sd save [EVENT]` | write the latest camera JPEG (opens the camera on demand) as `/doorbell/<stamp>-EVENT.jpg` | |
| `sd ls [DIR] [SKIP]` | list `1:/doorbell` (or DIR relative to `1:/`), 6 entries per reply, `SKIP` pages | |
| `sd format` | prints a token; `sd format TOKEN` within 120 s runs `f_mkfs` (FAT32 <= 32 GB, exFAT above) in the background, then remounts | |
| `sd auto on\|off` | auto-save on ring and motion (also remounts at most once a minute when a card appears) | `db_sd_auto`, on |
| `sd pwr PIN\|none` / `sd pins 1\|0` | card power GPIO / SDIO pin group (1 = GPIO 2/3/4, 0 = GPIO 14/15/16); applied at the next mount | `db_sd_pwr` 6, `db_sd_pins` 1 |
| `sd power on\|off` | drive the power pin directly (diagnostic) | |
| `sd ntp HOST` / `sd sync` | SNTP server / query now | `db_sd_ntp`, pool.ntp.org |
| `sd tz MIN` / `sd time EPOCH` | minutes east of UTC for names and FAT timestamps (e.g. Amsterdam 120 in summer, 60 in winter) / set the clock by hand | `db_sd_tz`, 0 |

## Status

Verified on the device (v0.24.0, no card inserted): the no-card path, NTP sync and `sd tz`. Writing
to an actual card is implemented but was not yet exercised on the device when v0.24.0 was tagged;
see [`../testplans/TESTPLAN-sd.md`](../testplans/TESTPLAN-sd.md). Not done yet: an HTTP listing of
the card (a `/sd/` endpoint on :8080) and an `sd` field in the report JSON.
