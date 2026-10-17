/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: boot-loop protection ("rescue mode", 0.28.1).
 *
 * The bootloader installs an OTA image over the running one and has no rollback, so an image that crashes
 * at every boot would need the UART header. Instead: a crash or watchdog reset increments a KV counter, a
 * boot that runs RESCUE_HEALTHY_MS (or reaches a normal deep sleep) clears it, and after RESCUE_AFTER crash
 * resets in a row the firmware starts in rescue mode: Wi-Fi, the token-protected TCP channel and the OTA
 * client only, a "rescue" webhook report, and only the commands that need nothing else (RESCUE_CMDS). The
 * camera, audio, microSD, PIR, night light, RTSP, policy and deep sleep stay off, so a bug in any of them
 * cannot crash it again; a fixed image goes in with `ota URL`.
 *
 * What it cannot cover: a crash before user_app_main() (SDK init, Wi-Fi driver), in the Wi-Fi/TCP/OTA path
 * itself, or on CPU1/CPU2 only. Faults on CPU0 reboot through the SDK trap handler with a crash reset reason;
 * hangs reboot through the interrupt / task / NMI watchdogs (CONFIG_INT_WDT, CONFIG_TASK_WDT, CONFIG_NMI_WDT_EN).
 * The counter is written only on a crash and on the first healthy boot after one: no flash wear from the
 * ~60 normal deep-sleep wakes a day. */
#include "app_internal.h"

#if (CONFIG_SYS_CPU0)
#define KV_CRASHES "db_crashes"   /* consecutive crash resets */
#define KV_RESCUE  "db_rescue"    /* "1" = `rescue on`: start in rescue mode until `rescue off` / an OTA */
#define RESCUE_AFTER 3
#define RESCUE_HEALTHY_MS (90 * 1000)

static const char *const RESCUE_CMDS[] = {"rescue", "ota", "reboot", "wifi", "hook", "tok"};

static bool s_active, s_forced, s_healthy;
static void report_task(beken_thread_arg_t arg);
static uint32_t s_crashes, s_reason;

static bool is_crash(uint32_t r)
{
	return r == RESET_SOURCE_WATCHDOG || r == RESET_SOURCE_NMI_WDT ||
	       (r >= RESET_SOURCE_CRASH_ILLEGAL_JUMP && r <= RESET_SOURCE_CRASH_ASSERT) ||
	       (r >= RESET_SOURCE_HARD_FAULT && r <= RESET_SOURCE_DEFAULT_EXCEPTION);
}

static uint32_t kv_u32(const char *key)
{
	char v[12];

	return kv_get_str(key, v, sizeof(v)) ? (uint32_t)os_strtoul(v, NULL, 10) : 0;
}

static void kv_put_u32(const char *key, uint32_t n)
{
	char v[12];

	snprintf(v, sizeof(v), "%u", n);
	kv_set_str(key, v);
}

/* First thing in user_app_main() on CPU0. True = start in rescue mode. */
bool rescue_boot_check(void)
{
	s_reason = bk_misc_get_reset_reason();
	s_crashes = kv_u32(KV_CRASHES);
	if (is_crash(s_reason))
		kv_put_u32(KV_CRASHES, ++s_crashes);
	s_forced = kv_u32(KV_RESCUE) == 1;
	s_active = s_forced || s_crashes >= RESCUE_AFTER;
	if (s_crashes || s_active)
		os_printf("[rescue] reset 0x%x, %u crash reset(s) in a row%s\r\n", s_reason, s_crashes,
			  s_active ? (s_forced ? ": RESCUE MODE (forced)" : ": RESCUE MODE") : "");
	return s_active;
}

bool rescue_active(void)
{
	return s_active;
}

uint32_t rescue_crashes(void)
{
	return s_crashes;
}

/* The boot proved healthy (RESCUE_HEALTHY_MS of uptime, or a normal deep sleep): clear the count. Never
 * in rescue mode, where the device is up only because the normal start was skipped. */
void rescue_mark_healthy(void)
{
	if (s_active || s_healthy)
		return;
	s_healthy = true;
	if (s_crashes) {
		s_crashes = 0;
		kv_put_u32(KV_CRASHES, 0);
		os_printf("[rescue] healthy boot: crash count cleared\r\n");
	}
}

/* heartbeat (every 30 s) */
void rescue_tick(void)
{
	if (rtos_get_time() >= RESCUE_HEALTHY_MS)
		rescue_mark_healthy();
}

/* Before an OTA download and on `rescue off`: the next image starts with a clean slate. */
void rescue_clear(void)
{
	s_crashes = 0;
	kv_put_u32(KV_CRASHES, 0);
	if (s_forced) {
		s_forced = false;
		kv_put_u32(KV_RESCUE, 0);
	}
}

/* TCP channel filter in rescue mode */
bool rescue_allows(const char *cmd)
{
	unsigned i;

	if (!s_active)
		return true;
	for (i = 0; i < sizeof(RESCUE_CMDS) / sizeof(RESCUE_CMDS[0]); i++)
		if (os_strcmp(cmd, RESCUE_CMDS[i]) == 0)
			return true;
	return false;
}

/* The minimal start: everything the OTA path needs and nothing else (order as in user_app_main()). */
void rescue_start(void)
{
	pa_set(0);            /* the amplifier enable floats at boot and half-turns the speaker on */
	ringfx_leds_off();
	net_load_settings();
	cmd_init();
	net_start_events(false);   /* netif/Wi-Fi callbacks only: no button, no PIR */
	cmd_start_tcp();
	{
		beken_thread_t t;

		rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "rescuerep", report_task, 3072, NULL);
	}
	net_connect();
}

static void report_task(beken_thread_arg_t arg)
{
	char json[200];
	int i;

	for (i = 0; i < 120 && !s_got_ip; i++)   /* up to 2 min for Wi-Fi */
		rtos_delay_milliseconds(1000);
	snprintf(json, sizeof(json), "{\"event\":\"rescue\",\"fw\":\"" FW_VERSION "\",\"ip\":\"%s\",\"crashes\":%u,"
		 "\"reset_reason\":%u,\"forced\":%u}", s_ip, s_crashes, s_reason, s_forced ? 1u : 0u);
	os_printf("[rescue] report -> %d\r\n", net_post(json));
	rtos_delete_thread(NULL);
}

static void crash_task(beken_thread_arg_t arg)
{
	void (*volatile nowhere)(void) = NULL;

	rtos_delay_milliseconds(500);   /* let the TCP reply go out first */
	os_printf("[rescue] crash test: calling NULL\r\n");
	nowhere();                     /* the SDK's trap handler: dump, then reboot with a crash reset reason */
	rtos_delete_thread(NULL);
}

void cli_rescue_cmd(CLI_ARGS)
{
	beken_thread_t t;

	if (argc >= 2 && os_strcmp(argv[1], "on") == 0) {
		kv_put_u32(KV_RESCUE, 1);
		REPLY("[rescue] on: the next boot starts in rescue mode (`reboot` now; `rescue off` or an OTA leaves it)\r\n");
	} else if (argc >= 2 && os_strcmp(argv[1], "off") == 0) {
		rescue_clear();
		REPLY("[rescue] off: crash count cleared%s\r\n", s_active ? "; `reboot` to start normally" : "");
	} else if (argc >= 2 && os_strcmp(argv[1], "crash") == 0) {
		REPLY("[rescue] crash test in 0.5 s (counts as crash reset %u of %u)\r\n", s_crashes + 1, RESCUE_AFTER);
		rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "crashtest", crash_task, 1024, NULL);
	} else if (argc < 2 || os_strcmp(argv[1], "status") == 0) {
		REPLY("[rescue] %s | reset 0x%x | %u crash reset(s) in a row, rescue after %u | forced %s | fw " FW_VERSION
		      " up %u s ip %s | allowed in rescue: rescue ota reboot wifi hook tok\r\n",
		      s_active ? "RESCUE MODE" : "normal", s_reason, s_crashes, RESCUE_AFTER, s_forced ? "on" : "off",
		      rtos_get_time() / 1000, s_ip);
	} else {
		REPLY("rescue [status] | on | off | crash   (boot-loop protection, see src/rescue.c)\r\n");
	}
}
#endif
