/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: optional microSD storage: mount, snapshots on events, SNTP time for FAT timestamps. */
/* sdcard.c — optional microSD storage (doorbell-bk7258-fw, feature "sd").
 *
 * Hardware (stock firmware + SDK defaults, see TESTPLAN-sd.md for the verification steps):
 *   SDIO host, 1-bit mode: GPIO 2 = CLK, GPIO 3 = CMD, GPIO 4 = DAT0 (SDK default pin group 1,
 *   GPIO_DEFAULT_DEV_CONFIG in middleware/soc/bk7258/soc/gpio_map.h; the stock ran the same SDK
 *   drivers: its strings contain bk_sd_card_init / sdio_host / mount on sdcard).
 *   GPIO 6 = card detect (stock firmware: input with pull-up, 0 = card present; verified on the
 *   board 2026-10-04). Never driven. Card power: the slot hangs off the CAMERA supply (GPIO 28 rail,
 *   verified 2026-10-04: bus lines clamped low and no CMD8 answer while GPIO 28 is low, answers with
 *   it high, whatever GPIO 13 does). GPIO 13 is the battery ADC15 node only: the stock SDK glue drove
 *   it as "SD LDO enable" and 0.24.2 copied that, but it has no effect on the card. `sd pwr PIN`
 *   remains for other boards (default none); when set, power.c caches the battery read while the pin
 *   is driven and for SD_ADC_SETTLE_MS after.
 *   Second dependency: the SDK's bk_sd_card_init() only completes while the media pipeline is up
 *   (camera open: CPU at full clock + VIDP domain; with it down the 100 kHz probe commands work but
 *   the init times out, -14594). So a mount opens the camera (JPEG) first, which the save needs
 *   anyway, and votes PM_DEV_ID_SDIO -> 320 MHz for the duration.
 * Software: SDK sd_card driver (CONFIG_SDCARD, CONFIG_SDIO_HOST) + FatFs (CONFIG_FATFS,
 *   volume "1:" = DISK_NUMBER_SDIO_SD in components/fatfs/disk_io.c). CONFIG_SDCARD_POWER_GPIO_CTRL
 *   is turned OFF in the project config: with it the SDK would drive GPIO 13 (SDCARD_LDO_CTRL_GPIO)
 *   and power the card down from a timer after 10 s idle.
 *
 * Power policy (0.24.3): nothing is mounted while idle. A ring/motion event or `sd save` opens the
 *   camera, votes the CPU clock, pre-conditions the bus (host at 100 kHz, CMD0 x2, CMD8: a silent
 *   card fails fast instead of the SDK's 128 blind CMD8 retries), inits, mounts, saves, then keeps
 *   the card mounted for `sd keep S` (default 30 s) after the last save so bursts share one mount;
 *   then umount, card deinit, vote released, power pin (if any) released. `sd mount` pins it
 *   ("manual") until `sd umount`. power.c calls sd_prepare_sleep() before deep sleep. The camera's
 *   own lifetime is not touched: when it closes while the card is mounted the card loses its supply,
 *   the next write fails and the card is dropped; the next event mounts again.
 *
 * Threads: one low-priority worker owns every card operation (mount/umount/format/save/NTP).
 *   The ring/motion paths only push a job into its queue (never block). `sd ls` and `sd status`
 *   run in the caller with a 500 ms mutex timeout.
 * Time: a tiny SNTP client (UDP/123, KV db_sd_ntp, default pool.ntp.org) once the network is up,
 *   re-synced every 6 h; `sd time EPOCH` sets it by hand. File names use local time via
 *   db_sd_tz (minutes east of UTC, default 0); without a time source the name carries the uptime.
 */
#include <common/bk_include.h>
#include "sdcard.h"

typedef int sdcard_translation_unit_is_never_empty_t;

#if (CONFIG_SYS_CPU0)

#include <os/os.h>
#include <os/str.h>
#include <os/mem.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <driver/gpio.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include <modules/pm.h>   /* bk_pm_module_vote_cpu_freq(): the SDK card init only works at full CPU clock */

#if (CONFIG_SDCARD && CONFIG_FATFS)
#define SD_HAVE_HOST 1
#include <driver/sd_card.h>
#include <driver/sdio_host.h>
#include "ff.h"
#else
#define SD_HAVE_HOST 0
#endif

/* EasyFlash KV (components/easy_flash/bk_ef.h), same helpers app_main.c uses */
extern int bk_set_env_enhance(const char *key, const void *value, int value_len);
extern int bk_get_env_enhance(const char *key, void *value, int value_len);
/* libdriver (middleware/driver/gpio), not in a public header */
extern bk_err_t gpio_dev_unmap(gpio_id_t gpio_id);
extern bk_err_t gpio_dev_map(gpio_id_t gpio_id, gpio_dev_t dev);
/* middleware/driver/bk7258/gpio_driver.c: maps exactly CLK/CMD/DATA0 of one SDIO pin group
 * (GPIO_SDIO_ONE_LINE_MAP_TABLE: mode 0 = GPIO 2/3/4, mode 1 = GPIO 14/15/16; it never touches
 * DATA1-3 = GPIO 5/10/11, so the console UART on 10/11 is safe). What the SDK's own
 * sdio_host_init_gpio() calls. */
extern bk_err_t gpio_sdio_one_line_sel(int mode);
#if SD_HAVE_HOST
/* middleware/driver/sdio_host/sdio_host_driver.c, "SDK only" but what bk_sd_card_init() does */
extern void bk_sdio_clk_gate_config(uint32_t enable);
#endif

/* Register windows for the diagnostics (include/soc/bk7258/reg_base.h, soc/gpio_struct.h,
 * soc/sys_struct.h, soc/sdio_struct.h; CONFIG_SPE=1 so SOC_ADDR_OFFSET is 0). */
#define SD_AON_GPIO_CFG(p) (*(volatile uint32_t *)(0x44000400u + 4u * (p)))
#define SD_SYS_GPIO_FUNC_SEL(p) ((*(volatile uint32_t *)(0x44010000u + 0x30u * 4u + 4u * ((p) / 8))) >> (4u * ((p) % 8)) & 0xfu)
#define SD_SYS_CLK_DIV_MODE2 (*(volatile uint32_t *)(0x44010000u + 0x9u * 4u))      /* cksel/ckdiv_sdio */
#define SD_SYS_DEV_CLK_EN    (*(volatile uint32_t *)(0x44010000u + 0xcu * 4u))      /* sdio_cken */
#define SD_SDIO_REG(n)       (*(volatile uint32_t *)(0x458d0000u + 4u * (n)))       /* only while the host is initialised */

#define SD_CD_PIN 6                   /* card detect, input with pull-up, 0 = card present */

#define KV_SD_AUTO "db_sd_auto"   /* "1" (default) / "0": save a JPEG on ring and motion */
#define KV_SD_PWR  "db_sd_pwr"    /* card power GPIO, default 13; "none" = no pin; a stored 6 (pre-0.24.2) is migrated to 13 */
#define KV_SD_KEEP "db_sd_keep"   /* seconds the card stays powered + mounted after the last save, default 30, min 5 */
#define KV_SD_PRE  "db_sd_pre"    /* "FLAGS,GAP_MS": pre-conditioning before bk_sd_card_init (SD_PRE_*), default "3,1000" */
/* KV "db_sd_pins" (<= 0.24.1, SDIO pin group) is gone: GPIO 14/15/16 would double-map the SDIO functions
 * and GPIO 15 is the USB-present input (power.c). The slot is on GPIO 2/3/4, proven 2026-10-04. */
#define KV_SD_NTP  "db_sd_ntp"    /* SNTP server, default pool.ntp.org */
#define KV_SD_TZ   "db_sd_tz"     /* minutes east of UTC for file names, default 0 */

#define SD_VOL "1:"
#define SD_DIR SD_VOL "/doorbell"
#define SD_JPG_MAX (200 * 1024)         /* = CAM_BUF_MAX in app_main.c */
#define SD_MKFS_WORK (32 * 1024)
#define SD_WORKER_PRIO 8                /* BEKEN_APPLICATION_PRIORITY = 7; higher number = lower priority (native 9 - n) */
#define SD_WORKER_STACK 4096
#define SD_QUEUE_LEN 6
#define SD_AUTOMOUNT_RETRY_MS (60 * 1000)   /* after a failed on-demand mount, auto events skip the card for this long */
#define SD_PWR_DEFAULT_PIN (-1)         /* none: GPIO 13 turned out to be the battery ADC only (2026-10-04), the slot is on the camera supply */
#define SD_PWR_SETTLE_MS 500            /* card VDD ramp + card power-up after the pin goes high (the stock waited too) */
#define SD_PWR_OFF_PULSE_MS 200         /* pin driven low before it is released: a definite "off" edge for the enable */
#define SD_PWR_CYCLE_MS 100             /* off time inside a mount retry */
#define SD_ADC_SETTLE_MS 10000          /* battery ADC15 node recovery after GPIO 13 is released (measured ~10 s) */
#define SD_PRE_CMD55 1                  /* pre-condition also sends CMD55 (what `sd probe` does) */
#define SD_PRE_CMD1  2                  /* ... and CMD1 (MMC op cond: times out on an SD card, what `sd probe` does) */
#define SD_PRE_SKIP  4                  /* no pre-conditioning at all (bare bk_sd_card_init) */
#define SD_PRE_CYCLE 8                  /* power cycle before the first attempt */
#define SD_PRE_NOCAM 16                 /* do not open the camera first (test: the card is on the camera supply) */
#define SD_PRE_NOVOTE 32                /* do not vote the CPU to 320 MHz first (test) */
#define SD_PRE_DEFAULT_FLAGS 0          /* CMD0 x2 + CMD8 only, camera opened, CPU voted */
#define SD_PRE_DEFAULT_GAP_MS 10        /* host deinit -> bk_sd_card_init gap */
#define SD_KEEP_DEFAULT_S 30
#define SD_KEEP_MIN_S 5
#define SD_NTP_FIRST_MS 15000
#define SD_NTP_RETRY_MS (60 * 1000)
#define SD_NTP_RESYNC_MS (6 * 3600 * 1000)
#define SD_TIME_REBASE_MS (3600 * 1000)
#define SD_FORMAT_TOKEN_TTL_MS (120 * 1000)
#define NTP_UNIX_OFFSET 2208988800UL

enum { JOB_MOUNT = 1, JOB_UMOUNT, JOB_UMOUNT_OFF, JOB_FORMAT, JOB_SAVE, JOB_AUTOSAVE, JOB_NTP, JOB_PROBE };

typedef struct {
	uint32_t id;
	uint8_t type;
	char arg[16];
} sd_job_t;

/* settings */
static bool s_auto = true;
static int s_pwr_pin = SD_PWR_DEFAULT_PIN;   /* -1 = none */
static volatile bool s_pwr_on;          /* power pin currently driven high */
static volatile uint32_t s_pwr_release_ms;   /* tick when the pin was last released (ADC settle), 0 = never driven */
static uint32_t s_keep_s = SD_KEEP_DEFAULT_S;
static uint32_t s_pre_flags = SD_PRE_DEFAULT_FLAGS, s_pre_gap_ms = SD_PRE_DEFAULT_GAP_MS;
static bool s_cpu_voted;                /* PM_DEV_ID_SDIO vote held while the card is in use */
static char s_ntp_host[40] = "pool.ntp.org";
static int s_tz_min;

/* state */
static beken_queue_t s_queue;
static beken_thread_t s_thread;
static beken_mutex_t s_fs_mutex;
static volatile uint32_t s_job_seq, s_job_done_id;
static volatile bool s_present, s_mounted, s_busy;
static volatile bool s_pinned;          /* `sd mount` / `sd power on`: stay powered + mounted until `sd umount` */
static volatile bool s_sleep;           /* sd_prepare_sleep() ran: no more mounts */
static uint32_t s_keep_until_ms;        /* worker: power the card down once this tick has passed (0 = no window) */
static uint32_t s_sectors, s_free_mb;   /* from the last successful mount (shown while unmounted too) */
static volatile uint32_t s_saved, s_save_fail, s_dropped;
static uint32_t s_mounts, s_mount_fail, s_last_mount_ms, s_last_cold_save_ms;
static char s_last_file[48];
static char s_result[112] = "idle";
static char s_probe[300];               /* `sd probe` output, filled by the worker, replied by the caller */
static char s_trace[150];               /* last card bring-up: pre-condition results, init return codes, timings */
static uint32_t s_last_mount_fail_ms;
static uint8_t *s_jpg;
static uint32_t s_fmt_token, s_fmt_token_ms;

/* wall clock */
static volatile bool s_time_valid;
static uint32_t s_time_epoch, s_time_tick_ms, s_time_sync_ms;
static const char *s_time_src = "none";
static uint32_t s_ntp_tries, s_ntp_ok;

static void sd_set_result(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(s_result, sizeof(s_result), fmt, ap);
	va_end(ap);
	os_printf("[sd] %s\r\n", s_result);
}

static int kv_get(const char *key, char *out, int cap)
{
	int n = bk_get_env_enhance(key, out, cap - 1);

	if (n <= 0 || n >= cap) {
		out[0] = 0;
		return 0;
	}
	out[n] = 0;
	return n;
}

static void kv_set(const char *key, const char *val)
{
	bk_set_env_enhance(key, val, os_strlen(val));
}

static int sd_atoi(const char *s)
{
	int neg = 0, v = 0;

	if (*s == '-') { neg = 1; s++; } else if (*s == '+') { s++; }
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return neg ? -v : v;
}

/* ---- wall clock ---- */

uint32_t sd_time_now(void)
{
	if (!s_time_valid)
		return 0;
	return s_time_epoch + (rtos_get_time() - s_time_tick_ms) / 1000;
}

static void sd_time_set(uint32_t epoch, const char *src)
{
	s_time_tick_ms = rtos_get_time();
	s_time_epoch = epoch;
	s_time_sync_ms = s_time_tick_ms;
	s_time_src = src;
	s_time_valid = true;
}

/* days since 1970-01-01 -> civil date (Howard Hinnant's civil_from_days, 32-bit) */
static void sd_civil(uint32_t secs, int *y, int *m, int *d, int *hh, int *mm, int *ss)
{
	uint32_t days = secs / 86400, rem = secs % 86400;
	uint32_t z = days + 719468;
	uint32_t era = z / 146097;
	uint32_t doe = z - era * 146097;
	uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	uint32_t yy = yoe + era * 400;
	uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint32_t mp = (5 * doy + 2) / 153;

	*hh = (int)(rem / 3600);
	*mm = (int)((rem % 3600) / 60);
	*ss = (int)(rem % 60);
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*m = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y = (int)(yy + (*m <= 2 ? 1 : 0));
}

static void sd_time_str(char *out, int cap)
{
	uint32_t now = sd_time_now();
	int y, m, d, hh, mm, ss;

	if (!now) {
		snprintf(out, cap, "unsynced");
		return;
	}
	sd_civil(now + (uint32_t)(s_tz_min * 60), &y, &m, &d, &hh, &mm, &ss);
	snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d", y, m, d, hh, mm, ss);
}

/* Minimal SNTP (RFC 4330) client: one request, 3 s timeout. Runs in the worker only. */
static int sd_ntp_sync(void)
{
	struct sockaddr_in sa;
	struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
	uint8_t pkt[48];
	uint32_t ip, secs;
	int s, n;

	s_ntp_tries++;
	ip = inet_addr(s_ntp_host);
	if (ip == INADDR_NONE || ip == 0) {
		struct hostent *he = gethostbyname(s_ntp_host);
		if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
			os_printf("[sd] ntp: dns failed for %s\r\n", s_ntp_host);
			return -1;
		}
		os_memcpy(&ip, he->h_addr_list[0], 4);
	}
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return -2;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	os_memset(pkt, 0, sizeof(pkt));
	pkt[0] = 0x23;   /* LI 0, version 4, mode 3 (client) */
	os_memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(123);
	sa.sin_addr.s_addr = ip;
	if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&sa, sizeof(sa)) != (int)sizeof(pkt)) {
		close(s);
		return -3;
	}
	n = recvfrom(s, pkt, sizeof(pkt), 0, NULL, NULL);
	close(s);
	if (n < 48 || (pkt[0] & 0x07) != 4 || pkt[1] == 0) {   /* mode 4 = server; stratum 0 = kiss-of-death */
		os_printf("[sd] ntp: no valid reply from %s (%d)\r\n", s_ntp_host, n);
		return -4;
	}
	secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) | ((uint32_t)pkt[42] << 8) | pkt[43];
	if (secs < NTP_UNIX_OFFSET + 1600000000UL)   /* before 2020: bogus */
		return -5;
	sd_time_set(secs - NTP_UNIX_OFFSET, "ntp");
	s_ntp_ok++;
	{
		char ts[24];
		sd_time_str(ts, sizeof(ts));
		os_printf("[sd] ntp: time %s (tz %d min)\r\n", ts, s_tz_min);
	}
	return 0;
}

#if SD_HAVE_HOST

/* FatFs asks for the time stamp of every created/modified file; patch 0007 makes the SDK's
 * fixed-2017 version weak so this one wins. */
DWORD get_fattime(void)
{
	uint32_t now = sd_time_now();
	int y, m, d, hh, mm, ss;

	if (!now)
		return ((DWORD)(2017 - 1980) << 25) | ((DWORD)5 << 21) | ((DWORD)1 << 16);
	sd_civil(now + (uint32_t)(s_tz_min * 60), &y, &m, &d, &hh, &mm, &ss);
	return ((DWORD)(y - 1980) << 25) | ((DWORD)m << 21) | ((DWORD)d << 16)
	       | ((DWORD)hh << 11) | ((DWORD)mm << 5) | ((DWORD)ss >> 1);
}

static FATFS s_fs;
static FIL s_fil;
#endif /* SD_HAVE_HOST */

/* ---- pins and power (plain GPIO: compiled in every build) ---- */

/* Drive the power pin like the SDK's bk_gpio_ctrl_external_ldo(GPIO_CTRL_LDO_MODULE_SDIO, pin, level)
 * (middleware/driver/gpio/gpio_driver_base.c): release the pin from any peripheral, no pull, no
 * interrupt, drive strength 0, input off, output on, level. */
static void sd_pin_drive(gpio_id_t p, int high)
{
	gpio_dev_unmap(p);
	bk_gpio_disable_output(p);
	bk_gpio_disable_input(p);
	bk_gpio_disable_pull(p);
	bk_gpio_disable_interrupt(p);
	bk_gpio_set_capacity(p, 0);
	bk_gpio_enable_output(p);
	if (high)
		bk_gpio_set_output_high(p);
	else
		bk_gpio_set_output_low(p);
}

/* on: pin high (the caller waits SD_PWR_SETTLE_MS). off: pin low for SD_PWR_OFF_PULSE_MS, then released
 * to hi-Z (no output, no input, no pull) = the boot state in which the pin is just the battery ADC15
 * node; s_pwr_release_ms starts the SD_ADC_SETTLE_MS blackout for battery reads. Blocks up to 200 ms:
 * worker or command task only. */
static void sd_power(int on)
{
	gpio_id_t p;

	if (s_pwr_pin < 0)
		return;
	p = (gpio_id_t)s_pwr_pin;
	if (on) {
		sd_pin_drive(p, 1);
		s_pwr_on = true;
		return;
	}
	if (s_pwr_on) {
		sd_pin_drive(p, 0);
		rtos_delay_milliseconds(SD_PWR_OFF_PULSE_MS);
	}
	gpio_dev_unmap(p);
	bk_gpio_disable_output(p);
	bk_gpio_disable_input(p);
	bk_gpio_disable_pull(p);
	s_pwr_on = false;
	s_pwr_release_ms = rtos_get_time() ? rtos_get_time() : 1;
}

bool sd_card_powered(void)
{
	return s_pwr_pin >= 0 && s_pwr_on;
}

bool sd_adc_pin_blocked(int gpio)
{
	if (s_pwr_pin != gpio)
		return false;
	if (s_pwr_on)
		return true;
	return s_pwr_release_ms && rtos_get_time() - s_pwr_release_ms < SD_ADC_SETTLE_MS;
}

/* GPIO 6: card detect. Input with pull-up, 0 = card present (stock firmware, verified). */
static void sd_cd_init(void)
{
	gpio_config_t cfg = {
		.io_mode = GPIO_INPUT_ENABLE,
		.pull_mode = GPIO_PULL_UP_EN,
		.func_mode = GPIO_SECOND_FUNC_DISABLE,
	};

	gpio_dev_unmap(SD_CD_PIN);
	bk_gpio_set_config(SD_CD_PIN, &cfg);
	bk_gpio_disable_output(SD_CD_PIN);
}

static int sd_cd_level(void)
{
	return bk_gpio_get_input(SD_CD_PIN) ? 1 : 0;
}

static void sd_pins(uint8_t out[3])
{
	static const uint8_t g1[3] = {2, 3, 4};

	os_memcpy(out, g1, 3);
}

/* Reproduce the SDK's GPIO_DEFAULT_DEV_CONFIG rows for the SDIO pins (gpio_map.h:
 * SECOND_FUNC_ENABLE, GPIO_DEV_SDIO_HOST_*, GPIO_IO_DISABLE, GPIO_PULL_UP_EN, CAPACITY_3) the way
 * sdio_host_init_gpio() does it (unmap, gpio_sdio_one_line_sel, pull-up, capacity 3). Order
 * matters: gpio_hal_map_check() keys off the func-enable bit, so set_config() must run on the
 * released pin before the map. Idempotent. */
static void sd_pins_apply(void)
{
	static const gpio_dev_t dev[3] = {GPIO_DEV_SDIO_HOST_CLK, GPIO_DEV_SDIO_HOST_CMD, GPIO_DEV_SDIO_HOST_DATA0};
	gpio_config_t cfg = {
		.io_mode = GPIO_IO_DISABLE,
		.pull_mode = GPIO_PULL_UP_EN,
		.func_mode = GPIO_SECOND_FUNC_DISABLE,
	};
	uint8_t p[3];
	int i;

	sd_pins(p);
	for (i = 0; i < 3; i++) {
		gpio_dev_unmap(p[i]);
		bk_gpio_set_config(p[i], &cfg);
	}
	/* mode 0 = GPIO 2/3/4 (mode 1 = GPIO 14/15/16 must never be selected on this board, see above) */
	if (gpio_sdio_one_line_sel(0) != BK_OK)
		for (i = 0; i < 3; i++)
			gpio_dev_map(p[i], dev[i]);
	for (i = 0; i < 3; i++) {
		bk_gpio_pull_up(p[i]);          /* the map cleared pull_en */
		bk_gpio_set_capacity(p[i], 3);
	}
}

/* ---- `sd probe`: host-level diagnostics without FatFs (worker task) ---- */

static int sd_probe_add(int n, const char *fmt, ...)
{
	va_list ap;

	if (n >= (int)sizeof(s_probe) - 1)
		return n;
	va_start(ap, fmt);
	n += vsnprintf(s_probe + n, sizeof(s_probe) - n, fmt, ap);
	va_end(ap);
	return n < (int)sizeof(s_probe) ? n : (int)sizeof(s_probe) - 1;
}

/* AON cfg word + sys func-select nibble of one pin: "N=cfg/sel" */
static int sd_probe_pin(int n, uint32_t pin)
{
	return sd_probe_add(n, " %u=%03x/%u", pin, SD_AON_GPIO_CFG(pin) & 0xfff, SD_SYS_GPIO_FUNC_SEL(pin));
}

static void sd_probe_pins(void)
{
	uint8_t p[3];
	int n = 0;

	sd_pins(p);
	sd_pins_apply();
	n = sd_probe_add(n, "[sd] cfg (aon/sel; ok=378/x):");
	n = sd_probe_pin(n, p[0]);
	n = sd_probe_pin(n, p[1]);
	n = sd_probe_pin(n, p[2]);
	n = sd_probe_add(n, " cd");
	n = sd_probe_pin(n, SD_CD_PIN);
	n = sd_probe_add(n, " lvl%d", sd_cd_level());
	n = sd_probe_add(n, " 13");
	n = sd_probe_pin(n, 13);
	n = sd_probe_add(n, " lvl%d pwr%d%s", bk_gpio_get_input(13) ? 1 : 0, s_pwr_pin, s_pwr_on ? " on" : "");
	n = sd_probe_add(n, " | sys clkdiv2 %08x devclk %08x (sdio_cken bit?)\r\n", SD_SYS_CLK_DIV_MODE2, SD_SYS_DEV_CLK_EN);
}

/* Is a line floating (follows our pull), held low (clamped: unpowered card / short) or held
 * high? Each SDIO pin as a plain input with pull-up then pull-down. Pins re-applied after. */
static void sd_probe_lines(void)
{
	static const char *name[3] = {"CLK", "CMD", "DAT0"};
	gpio_config_t cfg = {.io_mode = GPIO_INPUT_ENABLE, .pull_mode = GPIO_PULL_UP_EN, .func_mode = GPIO_SECOND_FUNC_DISABLE};
	uint8_t p[3];
	int n = 0, i, up, dn;

	if (s_mounted || s_present) {   /* would yank the pins from under the host */
		snprintf(s_probe, sizeof(s_probe), "[sd] probe lines: card in use, `sd umount` first\r\n");
		return;
	}
	sd_pins(p);
	n = sd_probe_add(n, "[sd] lines (up/dn: 1/0 floating, 0/0 held low, 1/1 held high):");
	for (i = 0; i < 3; i++) {
		gpio_dev_unmap(p[i]);
		cfg.pull_mode = GPIO_PULL_UP_EN;
		bk_gpio_set_config(p[i], &cfg);
		bk_gpio_disable_output(p[i]);
		rtos_delay_milliseconds(5);
		up = bk_gpio_get_input(p[i]) ? 1 : 0;
		cfg.pull_mode = GPIO_PULL_DOWN_EN;
		bk_gpio_set_config(p[i], &cfg);
		rtos_delay_milliseconds(5);
		dn = bk_gpio_get_input(p[i]) ? 1 : 0;
		n = sd_probe_add(n, " %s(%u) %d/%d", name[i], p[i], up, dn);
	}
	sd_pins_apply();
	n = sd_probe_add(n, " | pwr%d%s cd%d\r\n", s_pwr_pin, s_pwr_on ? " on" : "", sd_cd_level());
}

#if SD_HAVE_HOST
/* One command on the (initialised) host. Returns the wait result; *rsp = R1/R3/R7 argument on
 * success. Retries the wait once after 30 ms so a late response (ISR message after the driver's
 * 4 ms wait) is visible: *late = true then. */
static bk_err_t sd_host_cmd(uint32_t idx, uint32_t arg, int rsp_type, bool crc, uint32_t *rsp, bool *late)
{
	sdio_host_cmd_cfg_t c = {.cmd_index = idx, .argument = arg, .response = rsp_type, .wait_rsp_timeout = 2500, .crc_check = crc};
	bk_err_t r;

	*late = false;
	bk_sdio_host_send_command(&c);
	r = bk_sdio_host_wait_cmd_response(idx);
	if (r != BK_OK) {
		rtos_delay_milliseconds(30);
		if (bk_sdio_host_wait_cmd_response(idx) == BK_OK) {
			*late = true;
			r = BK_OK;
		}
	}
	*rsp = r == BK_OK ? bk_sdio_host_get_cmd_rsp_argument(SDIO_HOST_RSP0) : 0;
	return r;
}

static int sd_probe_cmd1(int n, const char *tag, uint32_t idx, uint32_t arg, int rsp, bool crc)
{
	uint32_t r1 = 0;
	bool late;
	bk_err_t r = sd_host_cmd(idx, arg, rsp, crc, &r1, &late);

	if (r == BK_OK)
		return sd_probe_add(n, " %s %s r=%08x", tag, late ? "LATE" : "ok", r1);
	return sd_probe_add(n, " %s %d st=%06x", tag, r, SD_SDIO_REG(0x0d) & 0xffffff);
}

/* Host at 100 kHz 1-bit with a fresh driver (queue + ISR). Caller: bk_sdio_host_deinit() after. */
static bk_err_t sd_host_bringup(void)
{
	sdio_host_config_t hc = {.clock_freq = SDIO_HOST_CLK_100K, .bus_width = SDIO_HOST_BUS_WIDTH_1LINE};
	bk_err_t r;

	bk_sdio_host_driver_deinit();
	bk_sdio_host_driver_init();
	bk_sdio_clk_gate_config(1);
	r = bk_sdio_host_init(&hc);
	if (r == BK_OK)
		rtos_delay_milliseconds(30);   /* >= 74 clocks before the first command */
	return r;
}

/* Pre-condition the card for bk_sd_card_init(): CMD0, CMD0, CMD8 (0x1AA) at 100 kHz, host deinit.
 * Why: after a reboot the card is still powered (GPIO 13 released does not drop its VDD for a
 * long time) and sits in transfer state; the SDK's init sends ONE CMD0 and then 128 blind CMD8
 * retries, and a card that missed that first CMD0 (first command after the pins are re-mapped)
 * never answers CMD8 (illegal outside idle state) -> -14594 after 0.7-3.7 s. A second CMD0 50 ms
 * later always brought it back to idle (2026-10-04: "sd probe then sd mount" worked 3/3 while a
 * plain mount failed 4/4 after every reboot). Returns 0 when CMD8 echoed 0x1AA (card idle, SD v2),
 * 1 when CMD8 got no/odd answer (v1 card or still hung), <0 host failure. */
static int sd_trace_add(int n, const char *fmt, ...)
{
	va_list ap;

	if (n >= (int)sizeof(s_trace) - 1)
		return n;
	va_start(ap, fmt);
	n += vsnprintf(s_trace + n, sizeof(s_trace) - n, fmt, ap);
	va_end(ap);
	return n < (int)sizeof(s_trace) ? n : (int)sizeof(s_trace) - 1;
}

static int sd_precondition(uint32_t flags, uint32_t *cmd8, int tn)
{
	uint32_t r55 = 0, r1 = 0;
	bool late;
	int r = 1;
	bk_err_t e55 = 0, e1 = 0;

	*cmd8 = 0;
	if (sd_host_bringup() != BK_OK)
		return -1;
	sd_host_cmd(0, 0, SDIO_HOST_CMD_RSP_NONE, false, cmd8, &late);
	rtos_delay_milliseconds(50);
	sd_host_cmd(0, 0, SDIO_HOST_CMD_RSP_NONE, false, cmd8, &late);
	rtos_delay_milliseconds(50);
	if (sd_host_cmd(8, 0x1AA, SDIO_HOST_CMD_RSP_SHORT, true, cmd8, &late) == BK_OK && (*cmd8 & 0xfff) == 0x1AA)
		r = 0;
	if (flags & SD_PRE_CMD55) {
		rtos_delay_milliseconds(10);
		e55 = sd_host_cmd(55, 0, SDIO_HOST_CMD_RSP_SHORT, true, &r55, &late);
	}
	if (flags & SD_PRE_CMD1) {
		rtos_delay_milliseconds(10);
		e1 = sd_host_cmd(1, 0x40FF8000, SDIO_HOST_CMD_RSP_SHORT, false, &r1, &late);
	}
	bk_sdio_host_deinit();
	sd_trace_add(tn, "c8=%x", *cmd8 & 0xfff);
	if (flags & SD_PRE_CMD55)
		sd_trace_add((int)os_strlen(s_trace), e55 == BK_OK ? " c55=%x" : " c55=to", r55);
	if (flags & SD_PRE_CMD1)
		sd_trace_add((int)os_strlen(s_trace), e1 == BK_OK ? " c1=%x" : " c1=to", r1);
	return r;
}

/* CMD0 x2 (clock free-running in between), CMD8, CMD55, CMD1 (MMC); host regs; deinit. */
static void sd_probe_cmd(bool regs_only)
{
	uint32_t t0 = rtos_get_time();
	bool was_off = s_pwr_pin >= 0 && !s_pwr_on;
	bk_err_t r;
	int n = 0;

	if (s_mounted || s_present) {
		snprintf(s_probe, sizeof(s_probe), "[sd] probe: card in use, `sd umount` first\r\n");
		return;
	}
	if (was_off) {
		sd_power(1);
		rtos_delay_milliseconds(SD_PWR_SETTLE_MS);
	}
	sd_pins_apply();
	r = sd_host_bringup();
	n = sd_probe_add(n, "[sd] probe host_init %d", r);
	if (r != BK_OK) {
		sd_probe_add(n, "\r\n");
		if (was_off)
			sd_power(0);
		return;
	}
	n = sd_probe_add(n, " regs id %08x ctl %08x cfg %08x st %06x sys %08x/%08x", SD_SDIO_REG(0), SD_SDIO_REG(2),
			 SD_SDIO_REG(0x11), SD_SDIO_REG(0x0d) & 0xffffff, SD_SYS_CLK_DIV_MODE2, SD_SYS_DEV_CLK_EN);
	if (!regs_only) {
		n = sd_probe_add(n, " |");
		n = sd_probe_cmd1(n, "CMD0", 0, 0, SDIO_HOST_CMD_RSP_NONE, false);
		rtos_delay_milliseconds(50);
		n = sd_probe_cmd1(n, "CMD0", 0, 0, SDIO_HOST_CMD_RSP_NONE, false);
		rtos_delay_milliseconds(50);
		n = sd_probe_cmd1(n, "CMD8", 8, 0x1AA, SDIO_HOST_CMD_RSP_SHORT, true);
		rtos_delay_milliseconds(10);
		n = sd_probe_cmd1(n, "CMD55", 55, 0, SDIO_HOST_CMD_RSP_SHORT, true);
		rtos_delay_milliseconds(10);
		n = sd_probe_cmd1(n, "CMD1", 1, 0x40FF8000, SDIO_HOST_CMD_RSP_SHORT, false);
		n = sd_probe_add(n, " | st %06x", SD_SDIO_REG(0x0d) & 0xffffff);
	}
	bk_sdio_host_deinit();
	if (was_off && !s_pinned)
		sd_power(0);   /* leave the pin as found: idle = unpowered, battery ADC usable */
	sd_probe_add(n, " (%u ms)%s\r\n", rtos_get_time() - t0, was_off && !s_pinned ? " pwr off" : "");
}
#endif /* SD_HAVE_HOST */

static void sd_do_probe(const char *what)
{
	if (os_strcmp(what, "pins") == 0)
		sd_probe_pins();
	else if (os_strcmp(what, "lines") == 0)
		sd_probe_lines();
#if SD_HAVE_HOST
	else if (os_strcmp(what, "regs") == 0)
		sd_probe_cmd(true);
	else
		sd_probe_cmd(false);
#else
	else
		snprintf(s_probe, sizeof(s_probe), "[sd] no SD host in this build\r\n");
#endif
}

#if SD_HAVE_HOST
/* ---- card operations (worker task; s_fs_mutex held around every FatFs call) ---- */

/* bk_sd_card_init() with a freshly initialised SDIO host driver. The SDK driver completes
 * commands through an RTOS message queue fed by its ISR, with a 4 ms software wait against a
 * 25 ms hardware response timeout (2500 clocks at 100 kHz): a burst of unanswered CMD8 retries
 * (unpowered or hung card) leaves the queue out of step and every later command "times out" at
 * once (-14594 after ~84 ms, CMD0 included) until something re-syncs it. Seen 2026-10-04 after the
 * boot-time mount. Deinit/init recreates the queue and re-registers the ISR. */
static bk_err_t sd_card_init_fresh(void)
{
	bk_sdio_host_driver_deinit();
	bk_sdio_host_driver_init();
	return bk_sd_card_init();
}

static void sd_update_free_locked(void)
{
	DWORD nclst = 0;
	FATFS *fs = NULL;

	if (f_getfree(SD_VOL, &nclst, &fs) == FR_OK && fs)
		s_free_mb = (uint32_t)(((uint64_t)nclst * fs->csize) / 2048);
}

/* Drop everything after an I/O error so the next attempt re-probes the card. */
static void sd_drop_card(void)
{
	if (s_mounted)
		f_mount(NULL, SD_VOL, 0);
	s_mounted = false;
	s_present = false;
	bk_sd_card_deinit();
}

/* power pin low SD_PWR_CYCLE_MS then high + settle (a pause without a power pin) */
static void sd_power_cycle(void)
{
	if (s_pwr_pin >= 0) {
		sd_power(0);
		rtos_delay_milliseconds(SD_PWR_CYCLE_MS);
		sd_power(1);
		rtos_delay_milliseconds(SD_PWR_SETTLE_MS);
	} else {
		rtos_delay_milliseconds(300);
	}
}

/* Power on (if needed), SDIO pins, pre-condition (s_pre_flags), gap, bk_sd_card_init; on failure one
 * power cycle and a second attempt with the full pre-condition. Mutex held by the caller. The
 * trace (`sd status`) shows what each step returned. */
static bk_err_t sd_card_bringup(uint32_t *pre_ms)
{
	uint32_t cmd8, t0 = rtos_get_time(), t1;
	bk_err_t ret = BK_FAIL;
	int pre, attempt, tn = 0;

	s_trace[0] = 0;
	if (!(s_pre_flags & SD_PRE_NOCAM)) {
#ifdef DOORBELL_AVDK
		int c = app_sd_cam_ready();   /* JPEG camera open (or H.264 already running for RTSP) */
		tn = sd_trace_add(tn, "cam%d(%u) ", c, rtos_get_time() - t0);
#endif
	}
	if (!(s_pre_flags & SD_PRE_NOVOTE) && !s_cpu_voted) {
		bk_pm_module_vote_cpu_freq(PM_DEV_ID_SDIO, PM_CPU_FRQ_320M);
		s_cpu_voted = true;
		tn = sd_trace_add(tn, "vote ");
	}
	if (s_pwr_pin >= 0 && !s_pwr_on) {
		sd_power(1);
		rtos_delay_milliseconds(SD_PWR_SETTLE_MS);
		tn = sd_trace_add(tn, "pwr ");
	}
	if (s_pre_flags & SD_PRE_CYCLE) {
		sd_power_cycle();
		tn = sd_trace_add(tn, "cyc ");
	}
	for (attempt = 0; attempt < 2; attempt++) {
		uint32_t flags = attempt ? (SD_PRE_CMD55 | SD_PRE_CMD1) : s_pre_flags;
		uint32_t gap = attempt && s_pre_gap_ms < SD_PRE_DEFAULT_GAP_MS ? SD_PRE_DEFAULT_GAP_MS : s_pre_gap_ms;

		sd_pins_apply();
		pre = 0;
		if (!(flags & SD_PRE_SKIP)) {
			tn = sd_trace_add(tn, "pre%d ", attempt + 1);
			pre = sd_precondition(flags, &cmd8, tn);
			tn = (int)os_strlen(s_trace);
			tn = sd_trace_add(tn, " r%d gap%u | ", pre, gap);
			if (pre != 0 && attempt == 0) {   /* silent card: power cycle and look again before the slow SDK path */
				sd_power_cycle();
				tn = sd_trace_add(tn, "cyc ");
				continue;
			}
			if (pre != 0) {
				ret = BK_ERR_SDIO_HOST_CMD_RSP_TIMEOUT;
				tn = sd_trace_add(tn, "silent");
				break;
			}
			rtos_delay_milliseconds(gap);
		}
		*pre_ms = rtos_get_time() - t0;
		t1 = rtos_get_time();
		ret = sd_card_init_fresh();
		tn = sd_trace_add(tn, "init %d %ums", ret, rtos_get_time() - t1);
		if (ret == BK_OK)
			return BK_OK;
		os_printf("[sd] init %d after precondition %d, attempt %d\r\n", ret, pre, attempt + 1);
		if (attempt == 0) {
			sd_power_cycle();
			tn = sd_trace_add(tn, " | cyc ");
		}
	}
	return ret;
}

static int sd_do_mount(void)
{
	uint32_t t0 = rtos_get_time(), pre_ms = 0;
	bk_err_t ret;
	FRESULT fr;

	if (s_mounted)
		return 0;
	if (s_sleep) {
		sd_set_result("sleeping: no mount");
		return -1;
	}
	if (sd_cd_level()) {   /* GPIO 6 = 1: slot empty, do not even clock the bus */
		s_present = false;
		sd_set_result("no card (cd=1)");
		return -1;
	}
	rtos_lock_mutex(&s_fs_mutex);
	ret = sd_card_bringup(&pre_ms);
	if (ret != BK_OK) {
		s_present = false;
		s_mount_fail++;
		s_last_mount_fail_ms = rtos_get_time() ? rtos_get_time() : 1;
		rtos_unlock_mutex(&s_fs_mutex);
		sd_power(0);
		if (s_cpu_voted) {
			bk_pm_module_vote_cpu_freq(PM_DEV_ID_SDIO, PM_CPU_FRQ_DEFAULT);
			s_cpu_voted = false;
		}
		sd_set_result("no card (init %d, %u ms)", ret, rtos_get_time() - t0);
		return -1;
	}
	s_present = true;
	s_sectors = bk_sd_card_get_card_size();
	fr = f_mount(&s_fs, SD_VOL, 1);
	if (fr != FR_OK) {
		uint32_t mb = s_sectors / 2048;
		bool nofs = (fr == FR_NO_FILESYSTEM);
		s_mounted = false;
		f_mount(NULL, SD_VOL, 0);
		bk_sd_card_deinit();
		if (!nofs)
			s_present = false;
		s_mount_fail++;
		s_last_mount_fail_ms = rtos_get_time() ? rtos_get_time() : 1;
		rtos_unlock_mutex(&s_fs_mutex);
		sd_power(0);
		sd_set_result(nofs ? "card %u MB has no FAT filesystem (fr=%d): `sd format`" : "card %u MB: mount failed fr=%d",
			      mb, fr);
		return -2;
	}
	s_mounted = true;
	s_mounts++;
	s_last_mount_ms = rtos_get_time() - t0;
	fr = f_mkdir(SD_DIR);
	if (fr != FR_OK && fr != FR_EXIST)
		os_printf("[sd] mkdir %s fr=%d\r\n", SD_DIR, fr);
	sd_update_free_locked();
	rtos_unlock_mutex(&s_fs_mutex);
	s_keep_until_ms = rtos_get_time() + s_keep_s * 1000;
	sd_set_result("mounted %u MB, %u MB free (%u ms, pre %u ms)", s_sectors / 2048, s_free_mb, s_last_mount_ms, pre_ms);
	return 0;
}

/* Unmount, card + host deinit, power off (pin released). `power_off` false keeps the pin high
 * (diagnostics: `sd umount keep`). */
static void sd_do_umount(bool power_off)
{
	rtos_lock_mutex(&s_fs_mutex);
	if (s_mounted)
		f_mount(NULL, SD_VOL, 0);
	s_mounted = false;
	s_present = false;
	bk_sd_card_deinit();
	rtos_unlock_mutex(&s_fs_mutex);
	s_keep_until_ms = 0;
	if (power_off) {
		sd_power(0);
		if (s_cpu_voted) {
			bk_pm_module_vote_cpu_freq(PM_DEV_ID_SDIO, PM_CPU_FRQ_DEFAULT);
			s_cpu_voted = false;
		}
	}
	sd_set_result(power_off ? "unmounted, power off" : "unmounted, power kept");
}

static void sd_do_format(void)
{
	uint32_t t0 = rtos_get_time();
	void *work;
	FRESULT fr;

	if (s_pwr_pin >= 0 && !s_pwr_on) {
		sd_power(1);
		rtos_delay_milliseconds(100);
	}
	sd_pins_apply();
	work = psram_malloc(SD_MKFS_WORK);
	if (!work) {
		sd_set_result("format: no memory");
		return;
	}
	rtos_lock_mutex(&s_fs_mutex);
	if (s_mounted)
		f_mount(NULL, SD_VOL, 0);
	s_mounted = false;
	/* FM_ANY: FAT32 up to 32 GB, exFAT above (FF_FS_EXFAT=1); MBR partition so a PC reads it */
	fr = f_mkfs(SD_VOL, FM_ANY, 0, work, SD_MKFS_WORK);
	rtos_unlock_mutex(&s_fs_mutex);
	psram_free(work);
	if (fr != FR_OK) {
		sd_drop_card();
		sd_set_result("format failed fr=%d (%u ms)", fr, rtos_get_time() - t0);
		return;
	}
	os_printf("[sd] formatted in %u ms\r\n", rtos_get_time() - t0);
	bk_sd_card_deinit();   /* re-probe cleanly */
	if (sd_do_mount() == 0)
		sd_set_result("formatted (%u s) and mounted, %u MB free", (rtos_get_time() - t0) / 1000, s_free_mb);
}

static void sd_sanitize(char *out, int cap, const char *in)
{
	int n = 0;

	for (; *in && n < cap - 1; in++) {
		char c = *in;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
			out[n++] = c;
	}
	if (!n)
		out[n++] = 'x';
	out[n] = 0;
}

static void sd_make_name(char *out, int cap, const char *event, int suffix)
{
	char stamp[24], ev[13];
	uint32_t now = sd_time_now();

	sd_sanitize(ev, sizeof(ev), event);
	if (now) {
		int y, m, d, hh, mm, ss;
		sd_civil(now + (uint32_t)(s_tz_min * 60), &y, &m, &d, &hh, &mm, &ss);
		snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", y, m, d, hh, mm, ss);
	} else {
		snprintf(stamp, sizeof(stamp), "up%08u", rtos_get_time() / 1000);
	}
	if (suffix)
		snprintf(out, cap, SD_DIR "/%s-%s-%d.jpg", stamp, ev, suffix);
	else
		snprintf(out, cap, SD_DIR "/%s-%s.jpg", stamp, ev);
}

static int sd_do_save_inner(const char *event, bool automatic)
{
	uint32_t t0 = rtos_get_time(), off = 0;
	char path[72];
	FRESULT fr = FR_OK;
	int n, i;

	if (!s_mounted) {   /* on demand: power on, pre-condition, init, mount */
		if (automatic && (!s_auto || s_sleep))
			return -1;
		if (automatic && s_last_mount_fail_ms && rtos_get_time() - s_last_mount_fail_ms < SD_AUTOMOUNT_RETRY_MS) {
			s_dropped++;   /* no/odd card: do not spend seconds on every event */
			return -1;
		}
		if (sd_do_mount() != 0) {
			s_save_fail++;
			return -1;
		}
	}
	if (!s_jpg)
		s_jpg = psram_malloc(SD_JPG_MAX);
	if (!s_jpg) {
		sd_set_result("save: no PSRAM for the frame");
		return -1;
	}
#ifdef DOORBELL_AVDK
	n = app_cam_grab(s_jpg, SD_JPG_MAX, automatic ? 3000 : 4000);
#else
	n = -100;   /* bare SDK build: no camera pipeline */
#endif
	if (n <= 0) {
		s_save_fail++;
		sd_set_result("save: no camera frame (%d)", n);
		return -1;
	}
	rtos_lock_mutex(&s_fs_mutex);
	for (i = 0; i < 10; i++) {   /* two saves within one second: -1, -2, ... */
		sd_make_name(path, sizeof(path), event, i);
		fr = f_open(&s_fil, path, FA_CREATE_NEW | FA_WRITE);
		if (fr != FR_EXIST)
			break;
	}
	if (fr != FR_OK) {
		rtos_unlock_mutex(&s_fs_mutex);
		s_save_fail++;
		if (fr == FR_DISK_ERR || fr == FR_NOT_READY || fr == FR_INT_ERR || fr == FR_NO_FILESYSTEM) {
			rtos_lock_mutex(&s_fs_mutex);
			sd_drop_card();
			rtos_unlock_mutex(&s_fs_mutex);
			sd_set_result("save: open failed fr=%d, card dropped (removed?)", fr);
		} else {
			sd_set_result("save: open %s failed fr=%d", path, fr);
		}
		return -1;
	}
	while (off < (uint32_t)n) {
		UINT chunk = (uint32_t)n - off > 16384 ? 16384 : (uint32_t)n - off, bw = 0;
		fr = f_write(&s_fil, s_jpg + off, chunk, &bw);
		if (fr != FR_OK || bw != chunk)
			break;
		off += bw;
	}
	f_close(&s_fil);
	if (fr != FR_OK || off != (uint32_t)n) {
		f_unlink(path);
		s_save_fail++;
		if (fr == FR_DISK_ERR || fr == FR_NOT_READY || fr == FR_INT_ERR)
			sd_drop_card();
		rtos_unlock_mutex(&s_fs_mutex);
		sd_set_result("save: write failed fr=%d after %u/%d bytes%s", fr, off, n, s_mounted ? " (card full?)" : ", card dropped");
		return -1;
	}
	sd_update_free_locked();
	rtos_unlock_mutex(&s_fs_mutex);
	s_saved++;
	os_strlcpy(s_last_file, path + sizeof(SD_DIR), sizeof(s_last_file));   /* skip "1:/doorbell/" */
	sd_set_result("saved %s (%d B, %u ms, %u MB free)", s_last_file, n, rtos_get_time() - t0, s_free_mb);
	return 0;
}

/* The 200 KB PSRAM frame buffer lives only for the duration of one save: holding it (0.24.0/1 did)
 * starved cam_http_client_task's own psram_malloc(CAM_BUF_MAX) and every HTTP request on :8080
 * died with an empty reply after the first card write (found 2026-10-04 with the first real card). */
static int sd_do_save(const char *event, bool automatic)
{
	uint32_t t0 = rtos_get_time();
	bool cold = !s_mounted;
	int r = sd_do_save_inner(event, automatic);

	if (s_jpg) {
		psram_free(s_jpg);
		s_jpg = NULL;
	}
	if (s_mounted)
		s_keep_until_ms = rtos_get_time() + s_keep_s * 1000;   /* bursts share one mount */
	if (cold && r == 0)
		s_last_cold_save_ms = rtos_get_time() - t0;
	return r;
}

/* `sd ls [DIR] [SKIP]`: runs in the caller (TCP/UART task), bounded by the 320-byte reply buffer */
static void sd_cmd_ls(const char *dir, uint32_t skip)
{
	static FILINFO fno;   /* 280 bytes with LFN: keep it off the 4 KB command task stack */
	char path[64];
	DIR d;
	FRESULT fr;
	uint32_t n = 0, shown = 0;

	if (!s_mounted) {
		app_reply("[sd] not mounted\r\n");
		return;
	}
	if (!dir)
		os_strlcpy(path, SD_DIR, sizeof(path));
	else if (dir[0] == '1' && dir[1] == ':')
		os_strlcpy(path, dir, sizeof(path));
	else
		snprintf(path, sizeof(path), SD_VOL "/%s", dir[0] == '/' ? dir + 1 : dir);
	if (rtos_lock_mutex_timeout(&s_fs_mutex, 500) != BK_OK) {
		app_reply("[sd] busy (%s)\r\n", s_result);
		return;
	}
	fr = f_opendir(&d, path);
	if (fr != FR_OK) {
		rtos_unlock_mutex(&s_fs_mutex);
		app_reply("[sd] opendir %s fr=%d\r\n", path, fr);
		return;
	}
	while (f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
		n++;
		if (n <= skip || shown >= 6)
			continue;
		shown++;
		app_reply("%s%s %u\r\n", fno.fname, (fno.fattrib & AM_DIR) ? "/" : "", (uint32_t)fno.fsize);
	}
	f_closedir(&d);
	rtos_unlock_mutex(&s_fs_mutex);
	app_reply("[sd] %s: %u entries, shown %u after %u (sd ls DIR SKIP)\r\n", path, n, shown, skip);
}

#endif /* SD_HAVE_HOST */

/* ---- worker ---- */

static uint32_t sd_submit(int type, const char *arg, uint32_t timeout_ms)
{
	sd_job_t j;

	if (!s_queue)
		return 0;
	os_memset(&j, 0, sizeof(j));
	j.type = (uint8_t)type;
	if (arg)
		os_strlcpy(j.arg, arg, sizeof(j.arg));
	j.id = ++s_job_seq;
	if (!j.id)
		j.id = ++s_job_seq;
	if (rtos_push_to_queue(&s_queue, &j, timeout_ms) != BK_OK) {
		s_dropped++;
		return 0;
	}
	return j.id;
}

/* wait until job `id` finished; false on timeout (the job keeps running) */
static bool sd_wait(uint32_t id, uint32_t ms)
{
	uint32_t t0 = rtos_get_time();

	while ((int32_t)(s_job_done_id - id) < 0) {
		if (rtos_get_time() - t0 >= ms)
			return false;
		rtos_delay_milliseconds(20);
	}
	return true;
}

static void sd_run_job(const sd_job_t *j)
{
	s_busy = true;
	switch (j->type) {
#if SD_HAVE_HOST
	case JOB_MOUNT: sd_do_mount(); break;
	case JOB_UMOUNT: sd_do_umount(false); break;
	case JOB_UMOUNT_OFF: sd_do_umount(true); break;
	case JOB_FORMAT: sd_do_format(); break;
	case JOB_SAVE: sd_do_save(j->arg, false); break;
	case JOB_AUTOSAVE: sd_do_save(j->arg, true); break;
#endif
	case JOB_NTP: sd_ntp_sync(); break;
	case JOB_PROBE: sd_do_probe(j->arg); break;
	default: break;
	}
	s_busy = false;
}

/* keep window over (or sleep requested) and nobody pinned the card: power it down */
static void sd_idle_check(uint32_t now)
{
#if SD_HAVE_HOST
	bool in_use = s_mounted || s_present || s_pwr_on || s_cpu_voted;

	if (!in_use || (s_pinned && !s_sleep))
		return;
	if (!s_sleep && s_keep_until_ms && (int32_t)(now - s_keep_until_ms) < 0)
		return;
	s_busy = true;
	sd_do_umount(true);
	s_busy = false;
#else
	(void)now;
#endif
}

static void sd_worker(beken_thread_arg_t arg)
{
	uint32_t t_start = rtos_get_time(), now;
	uint32_t last_ntp_try = t_start - SD_NTP_RETRY_MS + SD_NTP_FIRST_MS;
	uint32_t last_rebase = t_start;
	sd_job_t j;

	while (1) {
		if (rtos_pop_from_queue(&s_queue, &j, 1000) == BK_OK) {
			sd_run_job(&j);
			s_job_done_id = j.id;
			continue;
		}
		now = rtos_get_time();
		sd_idle_check(now);
		if (app_net_up() && now - last_ntp_try > SD_NTP_RETRY_MS
		    && (!s_time_valid || now - s_time_sync_ms > SD_NTP_RESYNC_MS)) {
			last_ntp_try = now;
			sd_ntp_sync();
		}
		if (s_time_valid && now - last_rebase > SD_TIME_REBASE_MS) {   /* keep the tick delta small (32-bit ms wrap) */
			uint32_t e = sd_time_now();
			s_time_tick_ms = now;
			s_time_epoch = e;
			last_rebase = now;
		}
	}
}

/* ---- public ---- */

void sd_init(void)
{
	char v[40];

	if (kv_get(KV_SD_AUTO, v, sizeof(v)))
		s_auto = v[0] != '0';
	if (kv_get(KV_SD_PWR, v, sizeof(v))) {
		int p = os_strcmp(v, "none") == 0 ? -1 : sd_atoi(v);
		if (p == SD_CD_PIN || p == 13) {   /* 6 (< 0.24.2) is card detect, 13 (0.24.2 hand setting) is the battery ADC */
			kv_set(KV_SD_PWR, "none");
			p = -1;
		}
		s_pwr_pin = (p >= 0 && p < 56 && p != 10 && p != 11) ? p : -1;
	}
	if (kv_get(KV_SD_KEEP, v, sizeof(v))) {
		int k = sd_atoi(v);
		s_keep_s = k < SD_KEEP_MIN_S ? SD_KEEP_MIN_S : (uint32_t)k;
	}
	if (kv_get(KV_SD_PRE, v, sizeof(v))) {
		char *c = strchr(v, ',');
		s_pre_flags = (uint32_t)sd_atoi(v) & 0x3f;
		if (c)
			s_pre_gap_ms = (uint32_t)sd_atoi(c + 1);
		if (s_pre_gap_ms > 5000)
			s_pre_gap_ms = 5000;
	}
	sd_cd_init();
	if (kv_get(KV_SD_NTP, v, sizeof(v)))
		os_strlcpy(s_ntp_host, v, sizeof(s_ntp_host));
	if (kv_get(KV_SD_TZ, v, sizeof(v)))
		s_tz_min = sd_atoi(v);
	if (rtos_init_mutex(&s_fs_mutex) != BK_OK || rtos_init_queue(&s_queue, "sdq", sizeof(sd_job_t), SD_QUEUE_LEN) != BK_OK) {
		os_printf("[sd] init failed (mutex/queue)\r\n");
		s_queue = NULL;
		return;
	}
	if (rtos_create_thread(&s_thread, SD_WORKER_PRIO, "sdcard", sd_worker, SD_WORKER_STACK, NULL) != BK_OK) {
		os_printf("[sd] worker thread failed\r\n");
		return;
	}
	os_printf("[sd] %s, auto %s, keep %u s, pins 2/3/4, pwr %d (card on the camera supply, mounted on demand), ntp %s, tz %d\r\n",
		  SD_HAVE_HOST ? "sd host + fatfs" : "NO SD HOST IN THIS BUILD", s_auto ? "on" : "off", s_keep_s, s_pwr_pin,
		  s_ntp_host, s_tz_min);
}

/* power.c, before deep sleep (power task context): unmount + power off through the worker so no
 * FatFs call is cut mid-way; waits up to 4 s, then releases the pin regardless. */
void sd_prepare_sleep(void)
{
	uint32_t id;

	s_sleep = true;
	if (s_queue && (s_mounted || s_present || s_pwr_on)) {
		id = sd_submit(JOB_UMOUNT_OFF, NULL, 100);
		if (id)
			sd_wait(id, 4000);
	}
	if (s_pwr_on)
		sd_power(0);
}

void sd_on_event(const char *event)
{
	if (!s_auto || !s_queue)
		return;
	sd_submit(JOB_AUTOSAVE, event, 0);
}

static void sd_cmd_status(void)
{
	char ts[24], keep[24] = "";
	uint8_t p[3] = {2, 3, 4};
	uint32_t now = rtos_get_time();
	int cd = sd_cd_level();

#if SD_HAVE_HOST
	sd_pins(p);
#endif
	sd_time_str(ts, sizeof(ts));
	if (s_pinned)
		os_strlcpy(keep, " manual", sizeof(keep));
	else if (s_mounted && s_keep_until_ms && (int32_t)(now - s_keep_until_ms) < 0)
		snprintf(keep, sizeof(keep), " keep %u s left", (s_keep_until_ms - now + 999) / 1000);
#ifdef DOORBELL_AVDK
	int cam = app_cam_is_open() ? 1 : 0;
#else
	int cam = -1;
#endif
	app_reply("[sd] %scard %s (cd=%d) cam %d vote %d pin %s%s %s%s | last mount %u MB, %u MB free, %u ms (%u ok %u fail) cold save %u ms | "
		  "saved %u fail %u drop %u last %s\r\n",
		  SD_HAVE_HOST ? "" : "NO-HOST-BUILD ", cd ? "out" : "in", cd, cam, s_cpu_voted ? 1 : 0,
		  s_pwr_pin < 0 ? "none" : (s_pwr_on ? "on" : "off"),
		  (s_pwr_pin >= 0 && !s_pwr_on && sd_adc_pin_blocked(s_pwr_pin)) ? " (adc settling)" : "",
		  s_mounted ? "mounted" : (s_present ? "present" : "unmounted"), keep,
		  s_sectors / 2048, s_free_mb, s_last_mount_ms, s_mounts, s_mount_fail, s_last_cold_save_ms,
		  s_saved, s_save_fail, s_dropped, s_last_file[0] ? s_last_file : "-");
	app_reply("[sd] auto %s keep %u s pwr %d pre %u,%u pins %u/%u/%u time %s (%s, ntp %u/%u %s tz %d)%s%s: %s\r\n",
		  s_auto ? "on" : "off", s_keep_s, s_pwr_pin, s_pre_flags, s_pre_gap_ms, p[0], p[1], p[2], ts, s_time_src, s_ntp_ok,
		  s_ntp_tries, s_ntp_host, s_tz_min, s_busy ? " BUSY" : "", s_sleep ? " SLEEP" : "", s_result);
	if (s_trace[0])
		app_reply("[sd] bringup: %s\r\n", s_trace);
}

void cli_sd_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	char buf[16];
	uint32_t id;

	if (argc < 2 || os_strcmp(argv[1], "status") == 0) {
		sd_cmd_status();
		return;
	}
	if (!s_queue) {
		app_reply("[sd] not initialised\r\n");
		return;
	}
	if (os_strcmp(argv[1], "mount") == 0) {
		s_pinned = true;   /* stays mounted + powered until `sd umount` */
		id = sd_submit(JOB_MOUNT, NULL, 100);
		app_reply(id && sd_wait(id, 8000) ? "[sd] %s (manual: pinned until `sd umount`)\r\n" : "[sd] mount still running: %s\r\n", s_result);
	} else if (os_strcmp(argv[1], "umount") == 0) {
		s_pinned = false;
		id = sd_submit(argc >= 3 && os_strcmp(argv[2], "keep") == 0 ? JOB_UMOUNT : JOB_UMOUNT_OFF, NULL, 100);
		app_reply(id && sd_wait(id, 4000) ? "[sd] %s\r\n" : "[sd] umount still running: %s\r\n", s_result);
	} else if (argc >= 3 && os_strcmp(argv[1], "pre") == 0) {
		s_pre_flags = (uint32_t)sd_atoi(argv[2]) & 0x3f;
		if (argc >= 4)
			s_pre_gap_ms = (uint32_t)sd_atoi(argv[3]);
		if (s_pre_gap_ms > 5000)
			s_pre_gap_ms = 5000;
		snprintf(buf, sizeof(buf), "%u,%u", s_pre_flags, s_pre_gap_ms);
		kv_set(KV_SD_PRE, buf);
		app_reply("[sd] pre-condition flags %u (1 cmd55, 2 cmd1, 4 skip, 8 power cycle first, 16 no camera, 32 no cpu vote) gap %u ms\r\n", s_pre_flags, s_pre_gap_ms);
	} else if (argc >= 3 && os_strcmp(argv[1], "keep") == 0) {
		int k = sd_atoi(argv[2]);
		s_keep_s = k < SD_KEEP_MIN_S ? SD_KEEP_MIN_S : (uint32_t)k;
		snprintf(buf, sizeof(buf), "%u", s_keep_s);
		kv_set(KV_SD_KEEP, buf);
		app_reply("[sd] keep the card mounted %u s after the last save\r\n", s_keep_s);
	} else if (os_strcmp(argv[1], "format") == 0) {
		uint32_t now = rtos_get_time();
		if (argc >= 3 && s_fmt_token && now - s_fmt_token_ms < SD_FORMAT_TOKEN_TTL_MS
		    && os_strtoul(argv[2], NULL, 16) == s_fmt_token) {
			s_fmt_token = 0;
			if (!s_present && !s_mounted)
				app_reply("[sd] no card seen yet; `sd mount` first (format needs an inserted card)\r\n");
			else if (sd_submit(JOB_FORMAT, NULL, 100))
				app_reply("[sd] formatting (FM_ANY) in the background; poll `sd status`\r\n");
			else
				app_reply("[sd] queue full\r\n");
		} else {
			s_fmt_token = ((now * 2654435761u) >> 16) & 0xffff;
			if (!s_fmt_token)
				s_fmt_token = 1;
			s_fmt_token_ms = now;
			app_reply("[sd] ERASES THE CARD. Confirm within 120 s with: sd format %04x\r\n", s_fmt_token);
		}
	} else if (os_strcmp(argv[1], "ls") == 0) {
#if SD_HAVE_HOST
		sd_cmd_ls(argc >= 3 ? argv[2] : NULL, argc >= 4 ? os_strtoul(argv[3], NULL, 10) : 0);
#else
		app_reply("[sd] no SD host in this build\r\n");
#endif
	} else if (os_strcmp(argv[1], "save") == 0) {
		id = sd_submit(JOB_SAVE, argc >= 3 ? argv[2] : "manual", 100);
		app_reply(id && sd_wait(id, 12000) ? "[sd] %s\r\n" : "[sd] save still running: %s\r\n", s_result);
	} else if (argc >= 3 && os_strcmp(argv[1], "auto") == 0) {
		s_auto = os_strcmp(argv[2], "on") == 0;
		kv_set(KV_SD_AUTO, s_auto ? "1" : "0");
		app_reply("[sd] auto-save on ring/motion %s\r\n", s_auto ? "on" : "off");
	} else if (argc >= 3 && os_strcmp(argv[1], "pwr") == 0) {
		int p = os_strcmp(argv[2], "none") == 0 ? -1 : sd_atoi(argv[2]);
		uint8_t sp[3] = {2, 3, 4};
#if SD_HAVE_HOST
		sd_pins(sp);
#endif
		if (p >= 56 || p == 10 || p == 11 || p == SD_CD_PIN || p == sp[0] || p == sp[1] || p == sp[2]) {
			app_reply("[sd] bad pin (6 = card detect, %u/%u/%u = SDIO, 10/11 = console)\r\n", sp[0], sp[1], sp[2]);
			return;
		}
		if (s_pwr_on && s_pwr_pin != p)
			sd_power(0);   /* release the old pin */
		s_pwr_pin = p < 0 ? -1 : p;
		s_pwr_release_ms = 0;
		kv_set(KV_SD_PWR, s_pwr_pin < 0 ? "none" : argv[2]);
		app_reply("[sd] power pin %d (takes effect at the next mount)\r\n", s_pwr_pin);
	} else if (os_strcmp(argv[1], "probe") == 0) {
		s_probe[0] = 0;
		id = sd_submit(JOB_PROBE, argc >= 3 ? argv[2] : "cmd", 100);
		if (id && sd_wait(id, 6000))
			app_reply("%s", s_probe[0] ? s_probe : "[sd] probe: nothing\r\n");
		else
			app_reply("[sd] probe still running (%s)\r\n", s_result);
	} else if (os_strcmp(argv[1], "pins") == 0) {
		app_reply("[sd] SDIO pins are fixed at GPIO 2/3/4 on this board (group 0 = 14/15/16 would double-map the host and steal the USB-present input)\r\n");
	} else if (argc >= 3 && os_strcmp(argv[1], "power") == 0) {
		bool on = os_strcmp(argv[2], "on") == 0;
		if (s_mounted || s_present) {
			app_reply("[sd] card in use: `sd umount` first\r\n");
			return;
		}
		s_pinned = on;   /* a manually powered pin is not released by the keep window */
		sd_power(on);
		app_reply("[sd] power pin %d %s\r\n", s_pwr_pin, on ? "driven high (pinned: `sd power off` releases)" : "released");
	} else if (argc >= 3 && os_strcmp(argv[1], "ntp") == 0) {
		os_strlcpy(s_ntp_host, argv[2], sizeof(s_ntp_host));
		kv_set(KV_SD_NTP, s_ntp_host);
		app_reply("[sd] ntp server %s (`sd sync` to try now)\r\n", s_ntp_host);
	} else if (argc >= 3 && os_strcmp(argv[1], "tz") == 0) {
		s_tz_min = sd_atoi(argv[2]);
		snprintf(buf, sizeof(buf), "%d", s_tz_min);
		kv_set(KV_SD_TZ, buf);
		app_reply("[sd] tz offset %d min\r\n", s_tz_min);
	} else if (argc >= 3 && os_strcmp(argv[1], "time") == 0) {
		uint32_t e = os_strtoul(argv[2], NULL, 10);
		char ts[24];
		if (e < 1600000000UL) {
			app_reply("[sd] time EPOCH: unix seconds (UTC), e.g. $(date +%%s)\r\n");
			return;
		}
		sd_time_set(e, "manual");
		sd_time_str(ts, sizeof(ts));
		app_reply("[sd] time set: %s (tz %d)\r\n", ts, s_tz_min);
	} else if (os_strcmp(argv[1], "sync") == 0) {
		char ts[24];
		id = sd_submit(JOB_NTP, NULL, 100);
		if (id && sd_wait(id, 6000)) {
			sd_time_str(ts, sizeof(ts));
			app_reply("[sd] ntp %s: time %s (%u/%u ok)\r\n", s_ntp_host, ts, s_ntp_ok, s_ntp_tries);
		} else {
			app_reply("[sd] ntp still running\r\n");
		}
	} else {
		app_reply("sd status | mount | umount [keep] | format [TOKEN] | ls [DIR] [SKIP] | save [EVENT] | auto on|off | keep S | "
			  "pre FLAGS [GAP_MS] | pwr PIN|none | power on|off | probe [cmd|pins|lines|regs] | ntp HOST | tz MIN | time EPOCH | sync\r\n");
	}
}

#endif /* CONFIG_SYS_CPU0 */
