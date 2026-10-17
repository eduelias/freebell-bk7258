/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: GPIO, ADC and KV helpers shared by every module, and the `pin` / `adc` diagnostics. */
/* pin_configure(), adc_read_mv(), kv_get_str()/kv_set_str() and the db_* wrappers for the feature files (split out of app_main.c in 0.28.0) */
#include "app_internal.h"


/* Reclaim the pin from any peripheral function and configure it. */
bk_err_t pin_configure(uint32_t p, gpio_io_mode_t io, gpio_pull_mode_t pull)
{
	gpio_config_t cfg = {
		.io_mode = io,
		.pull_mode = pull,
		.func_mode = GPIO_SECOND_FUNC_DISABLE,
	};

	gpio_dev_unmap(p);
	return bk_gpio_set_config(p, &cfg);
}
#if (CONFIG_SYS_CPU0)
int kv_get_str(const char *key, char *out, int cap)
{
	int n = bk_get_env_enhance(key, out, cap - 1);

	if (n <= 0 || n >= cap) {
		out[0] = 0;
		return 0;
	}
	out[n] = 0;
	return n;
}

void kv_set_str(const char *key, const char *val)
{
	bk_set_env_enhance(key, val, os_strlen(val));
}
#endif
#if (CONFIG_SYS_CPU0)

uint32_t adc_read_mv(uint32_t chan)
{
	static uint32_t s_adc15_last;
	adc_config_t config = {0};
	uint16_t raw = 0;
	uint32_t mv = 0;

	/* ADC15 lives on GPIO 13; with `sd pwr 13` (not the default: the pin turned out not to power
	 * the card) bk_adc_init() would remap the driven pin, and the node needs ~10 s to settle after
	 * it is released. Report the last reading meanwhile (power.c marks it "cached"). */
	if (chan == 15 && sd_adc_pin_blocked(13))
		return s_adc15_last;
	if (bk_adc_acquire() != BK_OK)
		return 0;
	if (bk_adc_init(chan) == BK_OK) {
		config.chan = chan;
		config.adc_mode = ADC_CONTINUOUS_MODE;
		config.src_clk = ADC_SCLK_XTAL_26M;
		config.clk = 203125;
		config.saturate_mode = ADC_SATURATE_MODE_3;
		config.steady_ctrl = 7;
		if (bk_adc_set_config(&config) == BK_OK && bk_adc_enable_bypass_clalibration() == BK_OK
		    && bk_adc_start() == BK_OK && bk_adc_read(&raw, 1000) == BK_OK)
			mv = (uint32_t)(saradc_calculate(raw) * 1000);
	}
	bk_adc_stop();
	bk_adc_deinit(chan);
	bk_adc_release();
	if (chan == 15)
		s_adc15_last = mv;
	return mv;
}
#endif
#if (CONFIG_SYS_CPU0)
int db_kv_get(const char *key, char *out, int cap) { return kv_get_str(key, out, cap); }
void db_kv_set(const char *key, const char *val) { kv_set_str(key, val); }
uint32_t db_adc_mv(uint32_t chan) { return adc_read_mv(chan); }
int db_pin_cfg(uint32_t pin, int io_mode, int pull_mode)
{
	return pin_configure(pin, (gpio_io_mode_t)io_mode, (gpio_pull_mode_t)pull_mode);
}

#endif

/* ---- pin: GPIO mapping helpers (os_printf, so they work at any log level) ---- */

static void pin_print_all(const char *label)
{
	char line[PIN_COUNT + 1];
	int p;

	for (p = 0; p < PIN_COUNT; p++)
		line[p] = bk_gpio_get_input(p) ? '1' : '0';
	line[PIN_COUNT] = 0;
	os_printf("%s %s\r\n", label, line);
}

static void pin_usage(void)
{
	REPLY("pin all | mux | dump [N] | cfg N | get N | in N [up|down|none] | hiz N | out N 0|1 | raw N HEX | blink N [count] | watch SECONDS\r\n");
}

/* One pin's full state: AON cfg word (bits: 2 in_en, 3 out DISABLE (active low), 4 pull up/down,
 * 5 pull_en, 6 second-function enable, 8-9 drive strength) and the system-controller function
 * select nibble (SOC_SYS_REG_BASE + 0x30*4 + pin/8*4, 4 bits per pin: which column of the pin's
 * gpio_map.h row is routed; e.g. GPIO 2 sel 1 = SDIO_HOST_CLK). */
static void pin_reply_cfg(uint32_t p)
{
	uint32_t v = PIN_CFG(p);
	uint32_t sel = (*(volatile uint32_t *)(0x44010000u + 0x30u * 4u + 4u * (p / 8)) >> (4u * (p % 8))) & 0xfu;

	REPLY("PIN %u cfg %08x: lvl %u in_en %u out_en %u pull %s func %u cap %u sel %u\r\n", p, v, v & 1, (v >> 2) & 1,
	      ((v >> 3) & 1) ^ 1, (v & BIT(5)) ? ((v & BIT(4)) ? "up" : "down") : "off", (v >> 6) & 1, (v >> 8) & 3, sel);
}

void cli_pin_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	uint32_t p = 0, i;

	if (argc < 2) {
		pin_usage();
		return;
	}
	if (argc >= 3)
		p = os_strtoul(argv[2], NULL, 10);

	if (os_strcmp(argv[1], "all") == 0) {
		pin_print_all("PINS");
		return;
	}
	if (os_strcmp(argv[1], "mux") == 0) {
		/* 8 pins per line: MUX <first pin>: <cfg word low byte> x8 */
		for (i = 0; i < PIN_COUNT; i += 8) {
			os_printf("MUX %02u: %02x %02x %02x %02x %02x %02x %02x %02x\r\n", i,
				  PIN_CFG(i) & 0xff, PIN_CFG(i + 1) & 0xff, PIN_CFG(i + 2) & 0xff,
				  PIN_CFG(i + 3) & 0xff, PIN_CFG(i + 4) & 0xff, PIN_CFG(i + 5) & 0xff,
				  PIN_CFG(i + 6) & 0xff, PIN_CFG(i + 7) & 0xff);
		}
		return;
	}
	if (os_strcmp(argv[1], "dump") == 0) {
		/* no pin: all rows on the UART; with a pin: that pin's row of 8 in the reply too */
		if (argc >= 3 && p < PIN_COUNT) {
			i = p & ~7u;
			REPLY("CFG %02u: %08x %08x %08x %08x %08x %08x %08x %08x\r\n", i,
			      PIN_CFG(i), PIN_CFG(i + 1), PIN_CFG(i + 2), PIN_CFG(i + 3),
			      PIN_CFG(i + 4), PIN_CFG(i + 5), PIN_CFG(i + 6), PIN_CFG(i + 7));
			return;
		}
		for (i = 0; i < PIN_COUNT; i += 8)
			os_printf("CFG %02u: %08x %08x %08x %08x %08x %08x %08x %08x\r\n", i,
				  PIN_CFG(i), PIN_CFG(i + 1), PIN_CFG(i + 2), PIN_CFG(i + 3),
				  PIN_CFG(i + 4), PIN_CFG(i + 5), PIN_CFG(i + 6), PIN_CFG(i + 7));
		REPLY("CFG: 7 rows on the UART; `pin dump N` replies N's row, `pin cfg N` decodes one pin\r\n");
		return;
	}
	/* "watch" takes seconds, not a pin: check it before the pin-range test. */
	if (argc < 3 || (os_strcmp(argv[1], "watch") != 0 && p >= PIN_COUNT)) {
		pin_usage();
		return;
	}

	if (os_strcmp(argv[1], "get") == 0) {
		REPLY("PIN %u = %d (cfg %02x)\r\n", p, bk_gpio_get_input(p) ? 1 : 0, PIN_CFG(p) & 0xff);
	} else if (os_strcmp(argv[1], "cfg") == 0) {
		pin_reply_cfg(p);
	} else if (os_strcmp(argv[1], "watch") == 0) {
		/* argv[2] is seconds here: poll every 5 ms, print each transition. */
		uint32_t secs = p, t0 = rtos_get_time(), n = 0;
		char prev[PIN_COUNT], cur;

		for (i = 0; i < PIN_COUNT; i++)
			prev[i] = bk_gpio_get_input(i) ? 1 : 0;
		pin_print_all("WATCH start");
		while (rtos_get_time() - t0 < secs * 1000) {
			for (i = 0; i < PIN_COUNT; i++) {
				cur = bk_gpio_get_input(i) ? 1 : 0;
				if (cur != prev[i]) {
					os_printf("EDGE %u ms pin %u %d->%d\r\n", rtos_get_time() - t0, i, prev[i], cur);
					prev[i] = cur;
					n++;
				}
			}
			rtos_delay_milliseconds(5);
		}
		os_printf("WATCH end, %u edges\r\n", n);
	} else if (PIN_IS_CONSOLE(p)) {
		REPLY("PIN %u is the console UART, refusing\r\n", p);
	} else if (os_strcmp(argv[1], "in") == 0) {
		const char *pull = argc >= 4 ? argv[3] : "none";
		gpio_pull_mode_t pm = GPIO_PULL_DISABLE;
		bk_err_t err;

		if (os_strcmp(pull, "up") == 0)
			pm = GPIO_PULL_UP_EN;
		else if (os_strcmp(pull, "down") == 0)
			pm = GPIO_PULL_DOWN_EN;
		err = pin_configure(p, GPIO_INPUT_ENABLE, pm);
		bk_gpio_disable_output(p);
		rtos_delay_milliseconds(5);
		REPLY("PIN %u in %s = %d (cfg %02x, err %d)\r\n", p, pull, bk_gpio_get_input(p) ? 1 : 0,
		      PIN_CFG(p) & 0xff, err);
	} else if (os_strcmp(argv[1], "raw") == 0 && argc >= 4) {
		/* write the AON GPIO config word verbatim (hex), e.g. to restore a boot-time state */
		uint32_t v = os_strtoul(argv[3], NULL, 16);

		gpio_dev_unmap(p);
		PIN_CFG(p) = v;
		REPLY("PIN %u raw %08x -> now %08x\r\n", p, v, PIN_CFG(p));
	} else if (os_strcmp(argv[1], "hiz") == 0) {
		bk_err_t err = pin_configure(p, GPIO_IO_DISABLE, GPIO_PULL_DISABLE);

		bk_gpio_disable_output(p);
		bk_gpio_disable_input(p);
		REPLY("PIN %u hiz (cfg %02x, err %d)\r\n", p, PIN_CFG(p) & 0xff, err);
	} else if (os_strcmp(argv[1], "out") == 0 && argc >= 4) {
		int v = os_strtoul(argv[3], NULL, 10) ? 1 : 0;

		pin_configure(p, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		bk_gpio_disable_input(p);
		if (v)
			bk_gpio_set_output_high(p);
		else
			bk_gpio_set_output_low(p);
		REPLY("PIN %u out %d (cfg %02x)\r\n", p, v, PIN_CFG(p) & 0xff);
	} else if (os_strcmp(argv[1], "blink") == 0) {
		uint32_t count = argc >= 4 ? os_strtoul(argv[3], NULL, 10) : 6;

		pin_configure(p, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		for (i = 0; i < count; i++) {
			bk_gpio_set_output_high(p);
			rtos_delay_milliseconds(300);
			bk_gpio_set_output_low(p);
			rtos_delay_milliseconds(300);
		}
		os_printf("PIN %u blinked %u times, left low\r\n", p, count);
	} else {
		pin_usage();
	}
}

/* ---- adc: one calibrated reading of a SARADC channel (same sequence as volt_detect.c) ---- */
void cli_adc_read_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	adc_config_t config = {0};
	uint16_t raw = 0;
	uint32_t chan;
	bk_err_t err;

	if (argc < 2) {
		os_printf("adc CHANNEL\r\n");
		return;
	}
	chan = os_strtoul(argv[1], NULL, 10);

	err = bk_adc_acquire();
	if (err != BK_OK) {
		os_printf("ADC%u: acquire failed %d\r\n", chan, err);
		return;
	}
	err = bk_adc_init(chan);
	if (err == BK_OK) {
		config.chan = chan;
		config.adc_mode = ADC_CONTINUOUS_MODE;
		config.src_clk = ADC_SCLK_XTAL_26M;
		config.clk = 203125;
		config.saturate_mode = ADC_SATURATE_MODE_3;
		config.steady_ctrl = 7;
		config.adc_filter = 0;
		config.sample_rate = 0;
		err = bk_adc_set_config(&config);
	}
	if (err == BK_OK)
		err = bk_adc_enable_bypass_clalibration();
	if (err == BK_OK)
		err = bk_adc_start();
	if (err == BK_OK)
		err = bk_adc_read(&raw, 1000);
	if (err == BK_OK)
		REPLY("ADC%u raw=%u mv=%u\r\n", chan, raw, (uint32_t)(saradc_calculate(raw) * 1000));
	else
		REPLY("ADC%u: failed %d (-0x%x)\r\n", chan, err, -err);
	bk_adc_stop();
	bk_adc_deinit(chan);
	bk_adc_release();
}
