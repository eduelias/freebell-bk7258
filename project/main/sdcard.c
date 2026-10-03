/* sdcard.c — optional microSD storage (doorbell-bk7258-fw, feature "sd").
 *
 * Hardware (stock firmware + SDK defaults, see TESTPLAN-sd.md for the verification steps):
 *   SDIO host, 1-bit mode: GPIO 2 = CLK, GPIO 3 = CMD, GPIO 4 = DAT0 (SDK default pin group 1,
 *   GPIO_DEFAULT_DEV_CONFIG in middleware/soc/bk7258/soc/gpio_map.h; the stock ran the same SDK
 *   drivers: its strings contain bk_sd_card_init / sdio_host / mount on sdcard).
 *   Card power: GPIO 6, active high (stock ty_app_sdcard.c). We only ever raise it (it is also one
 *   of the rails cam_rails() raises for the camera), never lower it unless explicitly asked.
 *   Fallback pin group 0 (GPIO 14/15/16) selectable with `sd pins 0`.
 * Software: SDK sd_card driver (CONFIG_SDCARD, CONFIG_SDIO_HOST) + FatFs (CONFIG_FATFS,
 *   volume "1:" = DISK_NUMBER_SDIO_SD in components/fatfs/disk_io.c). CONFIG_SDCARD_POWER_GPIO_CTRL
 *   is turned OFF in the project config: with it the SDK would drive GPIO 13 (SDCARD_LDO_CTRL_GPIO)
 *   and power the card down from a timer after 10 s idle.
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

#if (CONFIG_SDCARD && CONFIG_FATFS)
#define SD_HAVE_HOST 1
#include <driver/sd_card.h>
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

#define KV_SD_AUTO "db_sd_auto"   /* "1" (default) / "0": save a JPEG on ring and motion */
#define KV_SD_PWR  "db_sd_pwr"    /* card power GPIO, default 6; "none" */
#define KV_SD_PINS "db_sd_pins"   /* "1" (default) = GPIO 2/3/4, "0" = GPIO 14/15/16 */
#define KV_SD_NTP  "db_sd_ntp"    /* SNTP server, default pool.ntp.org */
#define KV_SD_TZ   "db_sd_tz"     /* minutes east of UTC for file names, default 0 */

#define SD_VOL "1:"
#define SD_DIR SD_VOL "/doorbell"
#define SD_JPG_MAX (200 * 1024)         /* = CAM_BUF_MAX in app_main.c */
#define SD_MKFS_WORK (32 * 1024)
#define SD_WORKER_PRIO 8                /* BEKEN_APPLICATION_PRIORITY = 7; higher number = lower priority (native 9 - n) */
#define SD_WORKER_STACK 4096
#define SD_QUEUE_LEN 6
#define SD_BOOT_MOUNT_DELAY_MS 8000     /* let Wi-Fi/camera init first */
#define SD_AUTOMOUNT_RETRY_MS (60 * 1000)
#define SD_REDETECT_MS (10 * 60 * 1000) /* card inserted later: probe again every 10 min while auto is on */
#define SD_NTP_FIRST_MS 15000
#define SD_NTP_RETRY_MS (60 * 1000)
#define SD_NTP_RESYNC_MS (6 * 3600 * 1000)
#define SD_TIME_REBASE_MS (3600 * 1000)
#define SD_FORMAT_TOKEN_TTL_MS (120 * 1000)
#define NTP_UNIX_OFFSET 2208988800UL

enum { JOB_MOUNT = 1, JOB_UMOUNT, JOB_UMOUNT_OFF, JOB_FORMAT, JOB_SAVE, JOB_AUTOSAVE, JOB_NTP };

typedef struct {
	uint32_t id;
	uint8_t type;
	char arg[16];
} sd_job_t;

/* settings */
static bool s_auto = true;
static int s_pwr_pin = 6;
static bool s_pins_group0;
static char s_ntp_host[40] = "pool.ntp.org";
static int s_tz_min;

/* state */
static beken_queue_t s_queue;
static beken_thread_t s_thread;
static beken_mutex_t s_fs_mutex;
static volatile uint32_t s_job_seq, s_job_done_id;
static volatile bool s_present, s_mounted, s_busy;
static uint32_t s_sectors, s_free_mb;
static volatile uint32_t s_saved, s_save_fail, s_dropped;
static char s_last_file[48];
static char s_result[112] = "idle";
static uint32_t s_last_automount_ms;
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

/* ---- pins and power ---- */

static void sd_power(int on)
{
	gpio_config_t cfg = {
		.io_mode = GPIO_OUTPUT_ENABLE,
		.pull_mode = GPIO_PULL_DISABLE,
		.func_mode = GPIO_SECOND_FUNC_DISABLE,
	};

	if (s_pwr_pin < 0)
		return;
	gpio_dev_unmap((gpio_id_t)s_pwr_pin);
	bk_gpio_set_config((gpio_id_t)s_pwr_pin, &cfg);
	if (on)
		bk_gpio_set_output_high((gpio_id_t)s_pwr_pin);
	else
		bk_gpio_set_output_low((gpio_id_t)s_pwr_pin);
}

static void sd_pins(uint8_t out[3])
{
	static const uint8_t g1[3] = {2, 3, 4}, g0[3] = {14, 15, 16};

	os_memcpy(out, s_pins_group0 ? g0 : g1, 3);
}

/* Same as the SDK's sdio_host_init_gpio() (compiled out with CONFIG_GPIO_DEFAULT_SET_SUPPORT):
 * map CLK/CMD/DAT0, pull-ups, drive strength 3. Idempotent. */
static void sd_pins_apply(void)
{
	static const gpio_dev_t dev[3] = {GPIO_DEV_SDIO_HOST_CLK, GPIO_DEV_SDIO_HOST_CMD, GPIO_DEV_SDIO_HOST_DATA0};
	uint8_t p[3];

	sd_pins(p);
	for (int i = 0; i < 3; i++) {
		gpio_dev_unmap(p[i]);
		gpio_dev_map(p[i], dev[i]);
		bk_gpio_pull_up(p[i]);
		bk_gpio_set_capacity(p[i], 3);
	}
}

/* ---- card operations (worker task; s_fs_mutex held around every FatFs call) ---- */

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

static int sd_do_mount(void)
{
	uint32_t t0 = rtos_get_time();
	bk_err_t ret;
	FRESULT fr;

	if (s_mounted)
		return 0;
	sd_power(1);
	rtos_delay_milliseconds(50);
	sd_pins_apply();
	rtos_lock_mutex(&s_fs_mutex);
	ret = bk_sd_card_init();
	if (ret != BK_OK) {
		s_present = false;
		rtos_unlock_mutex(&s_fs_mutex);
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
		rtos_unlock_mutex(&s_fs_mutex);
		sd_set_result(nofs ? "card %u MB has no FAT filesystem (fr=%d): `sd format`" : "card %u MB: mount failed fr=%d",
			      mb, fr);
		return -2;
	}
	s_mounted = true;
	fr = f_mkdir(SD_DIR);
	if (fr != FR_OK && fr != FR_EXIST)
		os_printf("[sd] mkdir %s fr=%d\r\n", SD_DIR, fr);
	sd_update_free_locked();
	rtos_unlock_mutex(&s_fs_mutex);
	sd_set_result("mounted %u MB, %u MB free (%u ms)", s_sectors / 2048, s_free_mb, rtos_get_time() - t0);
	return 0;
}

static void sd_do_umount(bool power_off)
{
	rtos_lock_mutex(&s_fs_mutex);
	if (s_mounted)
		f_mount(NULL, SD_VOL, 0);
	s_mounted = false;
	s_present = false;
	bk_sd_card_deinit();
	rtos_unlock_mutex(&s_fs_mutex);
	if (power_off)
		sd_power(0);
	sd_set_result(power_off ? "unmounted, power pin low" : "unmounted");
}

static void sd_do_format(void)
{
	uint32_t t0 = rtos_get_time();
	void *work;
	FRESULT fr;

	sd_power(1);
	rtos_delay_milliseconds(50);
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

static int sd_do_save(const char *event, bool automatic)
{
	uint32_t t0 = rtos_get_time(), off = 0;
	char path[72];
	FRESULT fr = FR_OK;
	int n, i;

	if (!s_mounted) {
		if (!automatic) {
			sd_set_result("save: not mounted (`sd mount`)");
			return -1;
		}
		if (!s_auto || rtos_get_time() - s_last_automount_ms < SD_AUTOMOUNT_RETRY_MS)
			return -1;
		s_last_automount_ms = rtos_get_time();
		if (sd_do_mount() != 0)
			return -1;
	}
	if (!s_jpg)
		s_jpg = psram_malloc(SD_JPG_MAX);
	if (!s_jpg) {
		sd_set_result("save: no PSRAM for the frame");
		return -1;
	}
#ifdef DU7_AVDK
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
	default: break;
	}
	s_busy = false;
}

static void sd_worker(beken_thread_arg_t arg)
{
	uint32_t t_start = rtos_get_time(), now;
	uint32_t last_ntp_try = t_start - SD_NTP_RETRY_MS + SD_NTP_FIRST_MS;
	uint32_t last_redetect = t_start, last_rebase = t_start;
	bool boot_mount_done = false;
	sd_job_t j;

	while (1) {
		if (rtos_pop_from_queue(&s_queue, &j, 1000) == BK_OK) {
			sd_run_job(&j);
			s_job_done_id = j.id;
			continue;
		}
		now = rtos_get_time();
#if SD_HAVE_HOST
		if (!boot_mount_done && now - t_start > SD_BOOT_MOUNT_DELAY_MS) {
			boot_mount_done = true;
			last_redetect = now;
			if (s_auto) {
				s_busy = true;
				sd_do_mount();
				s_busy = false;
			}
		} else if (boot_mount_done && s_auto && !s_mounted && now - last_redetect > SD_REDETECT_MS) {
			last_redetect = now;
			s_busy = true;
			sd_do_mount();
			s_busy = false;
		}
#else
		(void)boot_mount_done;
		(void)last_redetect;
#endif
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
		s_pwr_pin = (p >= 0 && p < 56 && p != 10 && p != 11) ? p : -1;
	}
	if (kv_get(KV_SD_PINS, v, sizeof(v)))
		s_pins_group0 = v[0] == '0';
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
	os_printf("[sd] %s, auto %s, pins %s, pwr %d, ntp %s, tz %d\r\n", SD_HAVE_HOST ? "sd host + fatfs" : "NO SD HOST IN THIS BUILD",
		  s_auto ? "on" : "off", s_pins_group0 ? "14/15/16" : "2/3/4", s_pwr_pin, s_ntp_host, s_tz_min);
}

/* True while the card power pin must stay high (card seen/mounted on that GPIO): app_main.c's
 * cam_power(0) skips GPIO 6 then, because GPIO 6 is both a camera LDO enable and the card power. */
bool sd_holds_gpio(int gpio)
{
	return s_pwr_pin == gpio && (s_present || s_mounted);
}

void sd_on_event(const char *event)
{
	if (!s_auto || !s_queue)
		return;
	sd_submit(JOB_AUTOSAVE, event, 0);
}

static void sd_cmd_status(void)
{
	char ts[24];
	uint8_t p[3] = {2, 3, 4};

	sd_time_str(ts, sizeof(ts));
#if SD_HAVE_HOST
	sd_pins(p);
#endif
	app_reply("[sd] %s card %s %s cap %u MB free %u MB auto %s saved %u fail %u drop %u last %s\r\n",
		  SD_HAVE_HOST ? "" : "NO-HOST-BUILD", s_present ? "present" : "absent", s_mounted ? "mounted" : "unmounted",
		  s_sectors / 2048, s_free_mb, s_auto ? "on" : "off", s_saved, s_save_fail, s_dropped, s_last_file[0] ? s_last_file : "-");
	app_reply("[sd] pins %u/%u/%u pwr %d time %s (%s, ntp %u/%u %s tz %d)%s: %s\r\n", p[0], p[1], p[2], s_pwr_pin, ts, s_time_src,
		  s_ntp_ok, s_ntp_tries, s_ntp_host, s_tz_min, s_busy ? " BUSY" : "", s_result);
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
		id = sd_submit(JOB_MOUNT, NULL, 100);
		app_reply(id && sd_wait(id, 4000) ? "[sd] %s\r\n" : "[sd] mount still running: %s\r\n", s_result);
	} else if (os_strcmp(argv[1], "umount") == 0) {
		id = sd_submit(argc >= 3 && os_strcmp(argv[2], "off") == 0 ? JOB_UMOUNT_OFF : JOB_UMOUNT, NULL, 100);
		app_reply(id && sd_wait(id, 4000) ? "[sd] %s\r\n" : "[sd] umount still running: %s\r\n", s_result);
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
		app_reply(id && sd_wait(id, 5500) ? "[sd] %s\r\n" : "[sd] save still running: %s\r\n", s_result);
	} else if (argc >= 3 && os_strcmp(argv[1], "auto") == 0) {
		s_auto = os_strcmp(argv[2], "on") == 0;
		kv_set(KV_SD_AUTO, s_auto ? "1" : "0");
		app_reply("[sd] auto-save on ring/motion %s\r\n", s_auto ? "on" : "off");
	} else if (argc >= 3 && os_strcmp(argv[1], "pwr") == 0) {
		int p = os_strcmp(argv[2], "none") == 0 ? -1 : sd_atoi(argv[2]);
		if (p >= 56 || p == 10 || p == 11) {
			app_reply("[sd] bad pin\r\n");
			return;
		}
		s_pwr_pin = p < 0 ? -1 : p;
		kv_set(KV_SD_PWR, s_pwr_pin < 0 ? "none" : argv[2]);
		app_reply("[sd] power pin %d (takes effect at the next mount)\r\n", s_pwr_pin);
	} else if (argc >= 3 && os_strcmp(argv[1], "pins") == 0) {
		s_pins_group0 = argv[2][0] == '0';
		kv_set(KV_SD_PINS, s_pins_group0 ? "0" : "1");
		app_reply("[sd] SDIO pins %s (umount + mount to apply)\r\n", s_pins_group0 ? "group 0 = GPIO 14/15/16" : "group 1 = GPIO 2/3/4");
	} else if (argc >= 3 && os_strcmp(argv[1], "power") == 0) {
#if SD_HAVE_HOST
		sd_power(os_strcmp(argv[2], "on") == 0);
		app_reply("[sd] power pin %d driven %s\r\n", s_pwr_pin, os_strcmp(argv[2], "on") == 0 ? "high" : "low");
#else
		app_reply("[sd] no SD host in this build\r\n");
#endif
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
		app_reply("sd status | mount | umount [off] | format [TOKEN] | ls [DIR] [SKIP] | save [EVENT] | auto on|off | "
			  "pwr PIN|none | pins 1|0 | power on|off | ntp HOST | tz MIN | time EPOCH | sync\r\n");
	}
}

#endif /* CONFIG_SYS_CPU0 */
