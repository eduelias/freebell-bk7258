#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <components/shell_task.h>

#define FW_NAME "doorbell-bk7258-fw"
#define FW_VERSION "0.1.0"

extern void user_app_main(void);
extern void rtos_set_user_app_entry(beken_thread_function_t entry);

/* Prints a line every 5 s so a running system is visible on UART0
 * even when nobody types into the shell. */
static void heartbeat_task(beken_thread_arg_t arg)
{
	uint32_t n = 0;

	while (1) {
		os_printf("[" FW_NAME "] alive %u, uptime %u ms\r\n", n++, rtos_get_time());
		rtos_delay_milliseconds(5000);
	}
}

void user_app_main(void)
{
	beken_thread_t heartbeat;

	os_printf("\r\n==== " FW_NAME " " FW_VERSION ": hello from our own firmware ====\r\n");
	rtos_create_thread(&heartbeat, BEKEN_APPLICATION_PRIORITY, "heartbeat",
			   heartbeat_task, 2048, NULL);
}

int main(void)
{
#if (CONFIG_SYS_CPU0)
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
#endif
	bk_init();

	return 0;
}
