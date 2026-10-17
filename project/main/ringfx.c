/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: light sensor, ring effects (chime + LEDs on a press), ring/led commands. */
/* Night threshold and light channel from KV (split out of app_main.c in 0.28.0) */
#include "app_internal.h"

#if (CONFIG_SYS_CPU0)
uint32_t s_dark_mv = 1000;     /* light sensor mV ABOVE this = dark (LDR pulls up in the dark; stock threshold ~2000/4095 raw) */
static bool s_fx_enabled = true;
#define KV_DARK "db_dark_mv"
#define KV_FX "db_fx"
uint32_t s_light_chan = BOARD_ADC_LIGHT;   /* LDR on GPIO 12 = ADC14 (stock), powered by GPIO 28; higher = darker */
#define KV_LIGHT_CHAN "db_light_chan"
#define WHITE_LIGHT_MS 30000
#endif
#if (CONFIG_SYS_CPU0)
/* ---- LEDs (found 2026-10-03 with the Mac camera): GPIO 44 = white fill LED, GPIO 45 = the four blue
 * button-ring LEDs (night locator light), both active high. The IR LED above the camera is still unmapped. ---- */
#define PIN_LED_WHITE BOARD_PIN_LED_WHITE
#define PIN_LED_RING BOARD_PIN_LED_RING
void cli_led_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	int which = -1, state;

	if (argc >= 3 && os_strcmp(argv[1], "white") == 0)
		which = NIGHT_LED_WHITE;
	else if (argc >= 3 && os_strcmp(argv[1], "ring") == 0)
		which = NIGHT_LED_RING;
	if (which < 0 || (os_strcmp(argv[2], "on") && os_strcmp(argv[2], "off") && os_strcmp(argv[2], "auto"))) {
		REPLY("led white|ring on|off|auto   (on/off = manual override of the night light / ring pattern, auto = release)\r\n");
		return;
	}
	/* manual on/off overrides the automatic night light / ring indicator until `auto` or a reboot (night.c) */
	state = os_strcmp(argv[2], "auto") == 0 ? -1 : os_strcmp(argv[2], "on") == 0;
	night_led_manual(which, state);
	REPLY("[led] %s %s (GPIO %d%s)\r\n", argv[1], argv[2], which == NIGHT_LED_WHITE ? PIN_LED_WHITE : PIN_LED_RING,
	      state < 0 ? ", automatic" : ", manual override: `led ... auto` to release");
}

#endif
#if (CONFIG_SYS_CPU0)
/* ---- ring effects: what the doorbell itself does on a press, independent of Wi-Fi ---- */
volatile bool s_fx_running;

/* the LDR divider is powered by the camera supply (GPIO 28): raise it for the reading when the camera is off */
uint32_t light_read_mv(void)
{
	uint32_t mv;
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
	bool cam = s_cam_open || s_cam_opening;   /* camera open(ing) = GPIO 28 already high and must stay high */
#else
	bool cam = false;
#endif

	if (!cam) {
		pin_configure(BOARD_PIN_CAM_PWR, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		bk_gpio_set_output_high(BOARD_PIN_CAM_PWR);
		rtos_delay_milliseconds(80);
	}
	mv = adc_read_mv(LIGHT_ADC_CHAN);
	if (!cam)
		bk_gpio_set_output_low(BOARD_PIN_CAM_PWR);
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
	if (mv >= 100)
		s_light_last_mv = mv;
#endif
	return mv;
}

static void ringfx_task(beken_thread_arg_t arg)
{
	uint32_t light = light_read_mv();
	bool dark = light > s_dark_mv;
	rtos_delay_milliseconds(20);

	os_printf("[fx] ring: light %u mV (%s)\r\n", light, dark ? "dark" : "day");
	if (!s_spk_busy)
		spk_start(0, 0, 0x60e000 + 16, 66450 - 16);   /* stock bell chime from flash */
	pin_configure(PIN_LED_RING, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	if (dark)
		night_white_set(NIGHT_SRC_FX, true);   /* via the arbiter: the night light may hold it on afterwards */
	for (int i = 0; i < 8; i++) {   /* blue ring: 8 blinks over ~2.4 s (night.c leaves the pin alone while s_fx_running) */
		bk_gpio_set_output_high(PIN_LED_RING);
		rtos_delay_milliseconds(150);
		bk_gpio_set_output_low(PIN_LED_RING);
		rtos_delay_milliseconds(150);
	}
	if (dark) {
		rtos_delay_milliseconds(WHITE_LIGHT_MS - 2400);
		night_white_set(NIGHT_SRC_FX, false);
	}
	s_fx_running = false;
	rtos_delete_thread(NULL);
}

void ringfx_start(void)
{
	beken_thread_t t;

	if (!s_fx_enabled || s_fx_running)
		return;
	s_fx_running = true;
	if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "ringfx", ringfx_task, 3072, NULL) != BK_OK)
		s_fx_running = false;
}

void cli_ring_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	char buf[16];

	if (argc >= 2 && os_strcmp(argv[1], "test") == 0) {
		uint32_t mv = light_read_mv();   /* read before the effects thread touches GPIO 28 */
		ringfx_start();
		sd_on_event("ring");   /* same as a real press: queued SD snapshot when a card is mounted */
		REPLY("[ring] effects started (light %u mV, dark above %u -> %s)\r\n", mv, s_dark_mv, mv > s_dark_mv ? "white light on" : "day, no light");
	} else if (argc >= 3 && os_strcmp(argv[1], "dark") == 0) {
		s_dark_mv = os_strtoul(argv[2], NULL, 10);
		snprintf(buf, sizeof(buf), "%u", s_dark_mv);
		kv_set_str(KV_DARK, buf);
		REPLY("[ring] dark threshold %u mV\r\n", s_dark_mv);
	} else if (argc >= 3 && os_strcmp(argv[1], "chan") == 0) {   /* ring chan N : ADC channel of the light sensor */
		s_light_chan = os_strtoul(argv[2], NULL, 10);
		snprintf(buf, sizeof(buf), "%u", s_light_chan);
		kv_set_str(KV_LIGHT_CHAN, buf);
		REPLY("[ring] light sensor on ADC%u, now %u mV\r\n", s_light_chan, light_read_mv());
	} else if (argc >= 3 && os_strcmp(argv[1], "fx") == 0) {
		s_fx_enabled = os_strcmp(argv[2], "on") == 0;
		kv_set_str(KV_FX, s_fx_enabled ? "1" : "0");
		REPLY("[ring] local effects %s\r\n", s_fx_enabled ? "on" : "off");
	} else {
		REPLY("ring test | dark MV | chan ADC | fx on|off   (light now %u mV, dark > %u, fx %d)\r\n", light_read_mv(), s_dark_mv, s_fx_enabled);
	}
}

/* ---- hooks for power.c / policy.c: thin wrappers so they stay separate files ---- */
bool app_light_is_dark(void) { return light_read_mv() > s_dark_mv; }
uint32_t app_dark_mv(void) { return s_dark_mv; }

#endif

#if (CONFIG_SYS_CPU0)
/* ---- module API ---- */
void ringfx_leds_off(void)
{
	pin_configure(PIN_LED_WHITE, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	bk_gpio_set_output_low(PIN_LED_WHITE);
	pin_configure(PIN_LED_RING, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	bk_gpio_set_output_low(PIN_LED_RING);
}

void ringfx_init(void)
{
	char v[16];

	ringfx_leds_off();
	if (kv_get_str(KV_DARK, v, sizeof(v)))
		s_dark_mv = os_strtoul(v, NULL, 10);
	if (kv_get_str(KV_FX, v, sizeof(v)))
		s_fx_enabled = v[0] != '0';
	if (kv_get_str(KV_LIGHT_CHAN, v, sizeof(v)))
		s_light_chan = os_strtoul(v, NULL, 10);
}

bool ringfx_busy(void)
{
	return s_fx_running;
}
#endif
