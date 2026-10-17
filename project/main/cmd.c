/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: command plumbing: reply buffer, TCP command channel, shared dispatcher, token, reboot, command table. */
/* REPLY()/db_reply()/app_reply(), app_exec() for the TCP channel, GET /cmd and the policy pull (split out of app_main.c in 0.28.0) */
#include "app_internal.h"

/* ---- reply buffer: command output for the TCP channel (UART still gets everything) ---- */
char s_reply[640];     /* one command's REPLY() text; `sd status` is three lines (0.24.4: 320 cut the trace off) */
bool s_reply_on;

/* REPLY() for other source files (night.c, power.c) */
void db_reply(const char *fmt, ...)
{
	char buf[640];   /* = sizeof(s_reply); 300 cut the 0.25 `pwr status` line */
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	REPLY("%s", buf);
}
#if (CONFIG_SYS_CPU0)

/* Pristine boot config of every GPIO (tools/boot_pincfg.txt, low byte). Restored before
 * `reboot` because the AON GPIO block survives a soft reset and the ROM samples a strap. */
static const uint8_t s_boot_cfg[PIN_COUNT] = {
	0x78,0x78,0x78,0x78,0x78,0x09,0x48,0x48,0x09,0x09,0x78,0x78,0x48,0x48,0x48,0x48,
	0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,
	0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48,
	0x48,0x48,0x48,0x48,0x48,0x48,0x48,0x48};

#endif
#if (CONFIG_SYS_CPU0)

/* REPLY() for modules outside this file (talk.c, sdcard.c) */
void app_reply(const char *fmt, ...)
{
	char tmp[320];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	REPLY("%s", tmp);
}
#endif
#if (CONFIG_SYS_CPU0)

/* ---- PIR (pir.c): settings + simulated motion edge; the text comes back here for REPLY ---- */
static void cli_pir_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	char out[256];

	pir_command(argc, argv, out, sizeof(out));
	REPLY("%s", out);
}

static void cli_motion_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	char out[160];

	motion_command(argc, argv, out, sizeof(out));
	REPLY("%s", out);
}





#endif
#if (CONFIG_SYS_CPU0)

/* ---- TCP command channel: the same commands as the UART shell, over the LAN ----
 * One line per connection: "<token> <command...>\n". The token is the KV string db_token
 * (set once over serial: `tok set SECRET`); without a stored token the channel is disabled.
 * Replies are "ok", "denied" or "unknown"; command output still goes to UART0. */
#define KV_TOKEN "db_token"
#define TCP_CMD_PORT 2323
static char s_token[48];
static const struct cli_command s_doorbell_commands[];
static unsigned s_doorbell_command_count;

static int run_line(char *line)
{
	char *argv[12];
	int argc = 0;
	unsigned i;

	for (char *t = strtok(line, " \t\r\n"); t && argc < 12; t = strtok(NULL, " \t\r\n"))
		argv[argc++] = t;
	if (!argc)
		return -1;
	for (i = 0; i < s_doorbell_command_count; i++)
		if (os_strcmp(argv[0], s_doorbell_commands[i].name) == 0) {
			s_doorbell_commands[i].function(NULL, 0, argc, argv);
			return 0;
		}
	return -1;
}

/* ---- shared dispatcher (fw 0.25): the TCP channel, GET /cmd (policy.c) and the policy pull all run
 * commands through here. s_cmd_mutex serialises the reply buffer between the TCP task and the HTTP
 * client threads. ---- */
static beken_mutex_t s_cmd_mutex;

int app_exec(const char *cmd, char *out, int cap)
{
	char line[192];
	int rc;

	os_strlcpy(line, cmd, sizeof(line));
	rtos_lock_mutex(&s_cmd_mutex);
	power_activity();
	s_reply[0] = 0;
	s_reply_on = true;
	rc = run_line(line);
	s_reply_on = false;
	if (out && cap > 0)
		os_strlcpy(out, s_reply, cap);
	rtos_unlock_mutex(&s_cmd_mutex);
	return rc;
}

/* A command run on behalf of another one (policy apply): its REPLY text goes to the UART/log only,
 * the outer command's reply stays intact. lock = not already inside app_exec() (boot-time apply). */
int app_exec_quiet(const char *cmd, bool lock)
{
	char line[192];
	bool was;
	int rc;

	os_strlcpy(line, cmd, sizeof(line));
	if (lock)
		rtos_lock_mutex(&s_cmd_mutex);
	was = s_reply_on;
	s_reply_on = false;
	rc = run_line(line);
	s_reply_on = was;
	if (lock)
		rtos_unlock_mutex(&s_cmd_mutex);
	return rc;
}

/* Reply text that must not be logged (the `log` command quoting the log) */
void app_reply_raw(const char *str)
{
	if (s_reply_on) {
		size_t l = os_strlen(s_reply);
		if (l < sizeof(s_reply) - 1)
			os_strlcpy(s_reply + l, str, sizeof(s_reply) - l);
	} else {
		bk_printf("%s", str);
	}
}

const char *app_cmd_token(void)
{
	return s_token;
}

static void tcp_cmd_task(beken_thread_arg_t arg)
{
	struct sockaddr_in sa = {0};
	int ls, cs, n, one = 1;
	char line[192], *cmd, *out;
	struct timeval tv = {.tv_sec = 5, .tv_usec = 0};

	ls = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(TCP_CMD_PORT);
	sa.sin_addr.s_addr = INADDR_ANY;
	if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(ls, 1) < 0) {
		os_printf("[tcp] bind/listen failed\r\n");
		close(ls);
		rtos_delete_thread(NULL);
		return;
	}
	os_printf("[tcp] command channel on port %d\r\n", TCP_CMD_PORT);
	out = os_malloc(sizeof(s_reply));
	while (1) {
		struct sockaddr_in peer = {0};
		socklen_t pl = sizeof(peer);

		cs = accept(ls, (struct sockaddr *)&peer, &pl);
		if (cs < 0)
			continue;
		setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		n = recv(cs, line, sizeof(line) - 1, 0);
		if (n > 0) {
			line[n] = 0;
			cmd = os_strchr(line, ' ');
			if (!s_token[0] || !cmd || os_strncmp(line, s_token, cmd - line) != 0
			    || os_strlen(s_token) != (size_t)(cmd - line)) {
				os_printf("[tcp] %s: denied\r\n", inet_ntoa(peer.sin_addr));
				send(cs, "denied\n", 7, 0);
			} else {
				cmd++;
				os_printf("[tcp] %s: %s\r\n", inet_ntoa(peer.sin_addr), cmd);   /* 0.25.0: the peer, so an unexpected poller (every command restarts the idle timer) can be traced */
				if (out)
					out[0] = 0;
				if (app_exec(cmd, out, sizeof(s_reply)) == 0)
					send(cs, "ok\n", 3, 0);
				else
					send(cs, "unknown\n", 8, 0);
				if (out && out[0])
					send(cs, out, os_strlen(out), 0);
			}
		}
		rtos_delay_milliseconds(100);
		close(cs);
	}
}

#endif
#if (CONFIG_SYS_CPU0)
static void cli_tok_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	if (argc >= 3 && os_strcmp(argv[1], "set") == 0) {
		os_strlcpy(s_token, argv[2], sizeof(s_token));
		kv_set_str(KV_TOKEN, s_token);
		os_printf("[tok] token set (%u chars); TCP channel active after reboot\r\n", os_strlen(s_token));
	} else if (argc >= 2 && os_strcmp(argv[1], "clear") == 0) {
		s_token[0] = 0;
		kv_set_str(KV_TOKEN, "");
		os_printf("[tok] cleared\r\n");
	} else {
		os_printf("[tok] %s  (tok set SECRET | clear)\r\n", s_token[0] ? "set" : "not set");
	}
}
#endif

/* ---- reboot: lets the host reach ROM download mode without touching the board ---- */

static void cli_reboot_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	uint32_t i;

	os_printf("rebooting (restoring boot GPIO config)\r\n");
#if (CONFIG_SYS_CPU0)
	for (i = 0; i < PIN_COUNT; i++) {
		if (PIN_IS_CONSOLE(i))
			continue;
		gpio_dev_unmap(i);
		PIN_CFG(i) = s_boot_cfg[i];
	}
#else
	(void)i;
#endif
	rtos_delay_milliseconds(50);
	bk_reboot();
}


static const struct cli_command s_doorbell_commands[] = {
	{"pin", "pin all|mux|dump|cfg|get|in|out|hiz|blink|watch ... (GPIO mapping)", cli_pin_cmd},
	{"adc", "adc CHANNEL (one calibrated SARADC reading)", cli_adc_read_cmd},
#if DOORBELL_BRINGUP
	{"mem", "mem HEXADDR [N] (raw 32-bit reads, debug)", cli_mem_cmd},
#endif
	{"reboot", "reset the SoC (ROM download mode is reachable right after)", cli_reboot_cmd},
#if (CONFIG_SYS_CPU0)
	{"wifi", "wifi set SSID PASS | clear | connect | (status)", cli_wifi_cmd},
	{"hook", "hook set HOST:PORT/PATH | test | ring | motion | (show)", cli_hook_cmd},
	{"pir", "pir on|off | sens 0-3 | cooldown MS | status (PIR GPIO 24/50/25/26)", cli_pir_cmd},
	{"motion", "motion test (simulated PIR edge through the ISR handler)", cli_motion_cmd},
	{"ota", "ota URL (download app_pack.rbl over HTTP, then reboot)", cli_ota_cmd},
	{"tok", "tok set SECRET | clear (token for the TCP command channel, port 2323)", cli_tok_cmd},
	{"led", "led white|ring on|off|auto (white fill LED GPIO 44 / blue button ring GPIO 45)", cli_led_cmd},
	{"night", "night auto on|off | ring on|off | hyst MV | status (night light, ring indicator)", night_cli_cmd},
	{"sys", "sys info (fw, uptime, ip, heap, reset reason, light/battery mV)", sys_cli_cmd},
#if (CONFIG_SYS_CPU0)
	{"ring", "ring test | dark MV | fx on|off (local chime, ring LEDs, light when dark)", cli_ring_cmd},
	{"pwr", "pwr status | sleep on|off | idle S | wake_pir on|off | snap_min M | sched ... | policy ... | sleepnow (battery, charger, deep sleep)", cli_pwr_cmd},
	{"sd", "sd status | mount | umount | format | ls [DIR] | save [EVENT] | auto on|off | keep S | probe (microSD)", cli_sd_cmd},
	{"vol", "vol [0-100] (speaker volume: chime and talk `gain auto`; KV)", cli_vol_cmd},
	{"log", "log [N<=600] | stat (last N bytes of the RAM log; GET /log?t=TOKEN for all 8 KB)", cli_log_cmd},
#endif
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
	{"cam", "cam open [W H] [h264] | close | stat | mode auto|jpeg|h264 | settle [N [MS [PCT]]] | scene auto|day|night|stock | exp [0-11] | ircut day|night | snap HOST PORT (AVDK build)", cli_cam_cmd},
	{"rtsp", "rtsp on|off | status | port N | q IMB PMB | idr (H.264 live stream rtsp://IP:554/live)", cli_rtsp_cmd},
#endif
	{"spk", "spk tone [HZ] [MS] | play beepa|beepb|bell|ADDR LEN | net HOST PORT | stop", cli_spk_cmd},
	{"mic", "mic level [MS] [HZ] | rec MS HOST PORT (on-board ADC mic)", cli_mic_cmd},
	{"talk", "talk status | gain N|auto | micgain N | max S | stop | aec on|off | vocmic N | voc on|off (two-way audio: HTTP :8080 /speak /mic.wav, RTSP voc engine)", cli_talk_cmd},
#endif
};
#if (CONFIG_SYS_CPU0)
static unsigned s_doorbell_command_count = sizeof(s_doorbell_commands) / sizeof(s_doorbell_commands[0]);
#endif

/* ---- module API ---- */
void cmd_register(void)   /* all cores: the UART shell gets every command of this image */
{
	cli_register_commands(s_doorbell_commands, sizeof(s_doorbell_commands) / sizeof(s_doorbell_commands[0]));
}

#if (CONFIG_SYS_CPU0)
void cmd_init(void)
{
	rtos_init_mutex(&s_cmd_mutex);
}

void cmd_start_tcp(void)
{
	beken_thread_t tcpcmd;

	kv_get_str(KV_TOKEN, s_token, sizeof(s_token));
	if (s_token[0])
		rtos_create_thread(&tcpcmd, BEKEN_APPLICATION_PRIORITY, "tcpcmd",
				   tcp_cmd_task, 4096, NULL);
	else
		os_printf("[tcp] disabled (no token: `tok set SECRET`)\r\n");
}
#endif
