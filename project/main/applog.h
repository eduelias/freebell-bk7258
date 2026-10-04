/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: route this firmware's own os_printf() lines into the RAM log ring (policy.c). */
/* applog.h: include it LAST in every project/main source file (after the SDK and module headers).
 *
 * The SDK's async log path (shell_log_out_port) has no output callback, so the ring captures what
 * this firmware prints itself: every os_printf()/REPLY() in app_main.c, power.c, night.c, pir.c,
 * talk.c, sdcard.c, rtsp.c and policy.c. SDK-internal BK_LOG* lines still go to the UART only.
 * CPU0 only: on CPU1/CPU2 os_printf stays the SDK's bk_printf. `log [N]` and GET /log?t= read it. */
#pragma once
#include <common/bk_include.h>   /* sdkconfig.h: CONFIG_SYS_CPU0 */

#if (CONFIG_SYS_CPU0)
void applog_printf(const char *fmt, ...);   /* vsnprintf -> ring + bk_printf (policy.c) */
#undef os_printf
#define os_printf applog_printf
#endif
