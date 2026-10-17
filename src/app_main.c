/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: startup (module init order), heartbeat, and the hooks power.c calls around deep sleep. */
#include "app_internal.h"

#if (CONFIG_SYS_CPU0)
/* anything that must finish before the device may sleep */
bool app_power_busy(void)
{
	bool busy = audio_busy() || ringfx_busy() || net_ota_busy() || voc_busy();

#if defined(DOORBELL_AVDK)
	busy = busy || cam_http_busy();          /* a snapshot or MJPEG stream client (HA) */
	busy = busy || rtsp_clients(NULL) > 0;   /* a live RTSP viewer / two-way audio session */
#endif
	if (net_reports_pending())
		busy = true;
	return busy;
}

/* amp off, LEDs off, camera closed, sensor rails low: everything we switched on */
void app_power_prepare_sleep(void)
{
	static const uint8_t rails[] = {BOARD_PIN_CAM_PWR, BOARD_PIN_CAM_LDO, BOARD_PIN_SD_DETECT, BOARD_PIN_SENSOR_RAIL};   /* SD_DETECT: see docs (card-detect input driven low in sleep) */
	unsigned i;

	pa_set(0);
	sd_prepare_sleep();   /* unmount the card (its supply is the camera rail cut below) */
	ringfx_leds_off();
#if defined(DOORBELL_AVDK)
	camera_close_for_sleep();
#endif
	rescue_mark_healthy();   /* reaching a normal deep sleep proves this boot healthy */
	for (i = 0; i < sizeof(rails); i++) {
		if (rails[i] == BOARD_PIN_SENSOR_RAIL && power_hold9())   /* `pwr hold9 on` (default off): leave the sensor rail up through the sleep */
			continue;
		pin_configure(rails[i], GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		bk_gpio_set_output_low(rails[i]);
	}
}

/* called from power_init() when the SoC was woken from deep sleep by a GPIO */
void app_power_wake_input(uint32_t pin)
{
	net_wake_input(pin);
}

uint32_t app_power_pir_sens(void)
{
	return pir_sens_get();
}
#endif /* CONFIG_SYS_CPU0 */

/* ---- app ---- */

static void heartbeat_task(beken_thread_arg_t arg)
{
	uint32_t n = 0;

	while (1) {
		os_printf("[" FW_NAME "] alive %u, uptime %u ms\r\n", n++, rtos_get_time());
#if (CONFIG_SYS_CPU0)
		rescue_tick();   /* 90 s up: clear the crash count */
#endif
		rtos_delay_milliseconds(30000);
	}
}

/* Start order matters where noted; pure KV reads can move freely between the steps. */
void user_app_main(void)
{
	beken_thread_t heartbeat;

	os_printf("\r\n==== " FW_NAME " " FW_VERSION ": hello from our own firmware ====\r\n");
	cmd_register();   /* all cores */
	rtos_create_thread(&heartbeat, BEKEN_APPLICATION_PRIORITY, "heartbeat",
			   heartbeat_task, 2048, NULL);

#if (CONFIG_SYS_CPU0)
	if (rescue_boot_check()) {   /* 3 crash resets in a row (or `rescue on`): Wi-Fi, TCP channel and OTA only */
		rescue_start();
		return;
	}
	pa_set(0);   /* amplifier enable floats at boot and half-turns the speaker on: park it low */
	voc_init();  /* KV: db_talk_aec / db_voc_mic; the engine itself starts with the first RTSP audio track */
	pin_configure(BOARD_PIN_SENSOR_RAIL, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);   /* sensor rail: light sensor reads ~25 mV dead without it */
	bk_gpio_set_output_high(BOARD_PIN_SENSOR_RAIL);
#if defined(DOORBELL_AVDK)
	camera_init();       /* KV camera/RTSP settings, the camera HTTP server, RTSP if enabled */
#endif
	ringfx_init();       /* LEDs parked off; KV: dark threshold, fx, light channel */
	talk_init();         /* KV: db_talk_gain / db_talk_micgain / db_talk_max */
	vol_init();          /* KV: db_vol (policy.c) */
	net_load_settings(); /* KV: Wi-Fi credentials, webhook */
	policy_init();       /* KV: db_pwr_policy (the auto URL derives from the hook: after net_load_settings) */
	cmd_init();          /* the dispatcher lock: before the reporter can run a policy pull */
	{
		/* night.c: LED arbiter + night light + ring indicator; before the inputs so a press
		 * (ringfx -> night_white_set) never runs ahead of it */
		static const night_env_t env = {
			.fw_version = FW_VERSION,
			.ip = s_ip,
			.got_ip = &s_got_ip,
#if defined(DOORBELL_AVDK)
			.cam_open = &s_cam_open,
#else
			.cam_open = NULL,
#endif
			.fx_running = &s_fx_running,
			.dark_mv = &s_dark_mv,
			.light_read_mv = light_read_mv,
			.adc_read_mv = adc_read_mv,
		};
		night_init(&env);
	}
	net_start_events(true);   /* netif/Wi-Fi event callbacks, then the button and PIR inputs */
	power_init();        /* before the reporter: a button wake from deep sleep must chime at once, before Wi-Fi */
	sd_init();           /* microSD worker: card mounted on demand (ring/motion/`sd save`, opens the camera), unmounted when idle */
	net_start_reporter();
	cmd_start_tcp();     /* TCP 2323, only with a token set */
	net_connect();
#endif
}

int main(void)
{
#if (CONFIG_SYS_CPU0)
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
#endif
	bk_init();
#ifdef DOORBELL_AVDK
	media_service_init();   /* CPU0: media app (mailbox client); CPU1: media major (camera/JPEG); CPU2: minor */
#endif

	return 0;
}
