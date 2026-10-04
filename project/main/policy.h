/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: policy pull, volume, RAM log and the authenticated HTTP command endpoint (fw 0.25). */
/* policy.c (CPU0 only):
 *  - RAM log: an 8 KB ring fed by applog_printf() (applog.h redirects our os_printf there);
 *    `log [N]` replies the last N bytes (<= 600), GET /log?t=<token> streams the whole ring.
 *  - Volume: `vol [0-100]` (KV db_vol, default 100) -> DAC gain 0..0x1c, linear; used by the chime
 *    and by talk when `talk gain auto`; `spk cfg gain N` stays a raw override.
 *  - Policy pull: `pwr policy URL|auto|off|pull` (KV db_pwr_policy, default auto =
 *    http://<hook host>:<hook port>/local/doorbell/policy.json). Fetched once per boot right after
 *    the IP, before the boot report; a flat JSON object with a whitelist of keys (day_start, day_end,
 *    day_min, night_min, wake_pir, pir_sens, dark_mv, volume, night_auto, keep_s, sd_auto, rev) is
 *    applied through the same command paths as the shell (so it persists in KV). Never sleep on/off,
 *    hook or token. The boot report carries "policy":"applied|none|error" and "policy_rev".
 *  - HTTP: GET /cmd?t=<token>&c=<url-encoded command> on :8080 runs the shared dispatcher and
 *    returns the REPLY text as text/plain (403 without the right token, also when no token is set). */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ---- RAM log ---- */
#define APPLOG_SIZE 8192
void applog_write(const char *p, int n);                       /* ISR-safe append */
uint32_t applog_total(void);                                   /* bytes ever written (monotonic) */
int  applog_tail(char *dst, int cap, int n);                   /* copy the last n bytes (<= cap - 1), NUL-terminated; returns the length */
void cli_log_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv);

/* ---- volume ---- */
#define VOL_GAIN_MAX 0x1c   /* the loudest clean DAC gain measured 2026-10-03 (above it the amplifier clips) */
void     vol_init(void);                                       /* KV db_vol */
uint32_t vol_get(void);                                        /* 0..100 */
uint16_t vol_gain(void);                                       /* DAC gain for the current volume, 0..VOL_GAIN_MAX */
void cli_vol_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv);

/* ---- policy pull ---- */
void policy_init(void);                                        /* KV db_pwr_policy */
void policy_boot_fetch(void);                                  /* reporter task, once per boot, right after the IP (blocks <= ~3 s) */
int  policy_json(char *buf, int cap);                          /* appends ',"policy":"...","policy_rev":"..."' for the boot report */
void policy_cli(int argc, char **argv);                        /* `pwr policy [URL|auto|off|pull]` (argv[1] == "policy") */

/* ---- HTTP (cam_http_client_task calls it after talk_http_handle): 1 = handled, caller closes the socket ---- */
int  policy_http_handle(int sock, const char *req, int n);

/* ---- hooks implemented in app_main.c ---- */
int  app_exec_quiet(const char *cmd, bool lock);               /* run one command line through the dispatcher without reply capture; 0 = known command */
int  app_exec(const char *cmd, char *out, int cap);            /* run one command line, REPLY text copied to out (serialised with the TCP channel) */
void app_reply_raw(const char *s);                             /* append to the TCP/HTTP reply only (not logged); UART when no reply is being captured */
const char *app_cmd_token(void);                               /* the `tok set` secret ("" = channel disabled) */
bool app_hook_hostport(char *host, int cap, int *port);        /* host and port of the configured webhook, false when unset */
uint32_t app_dark_mv(void);                                    /* `ring dark` threshold */
