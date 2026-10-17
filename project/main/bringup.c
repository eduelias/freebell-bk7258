/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: bring-up tools for a new board: pin/mem/adc commands, camera power + I2C probes, bit-banged I2C. */
/* Used to map the board; not needed in daily use (split out of app_main.c in 0.28.0) */
#include "app_internal.h"


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

/* mem HEXADDR [N]: N (<= 16) raw 32-bit reads. Debug only (CPU1 counters such as media_debug
 * live in shared SRAM and are otherwise only visible on the dead UART); a bad address faults. */
void cli_mem_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	uint32_t addr, n = 4, i;
	char line[160];
	int l = 0;

	if (argc < 2) {
		REPLY("mem HEXADDR [NWORDS<=16]  (raw 32-bit reads, debug only)\r\n");
		return;
	}
	addr = os_strtoul(argv[1], NULL, 16) & ~3U;
	if (argc > 2)
		n = os_strtoul(argv[2], NULL, 10);
	if (n < 1)
		n = 1;
	if (n > 16)
		n = 16;
	for (i = 0; i < n && l < (int)sizeof(line) - 10; i++)
		l += snprintf(line + l, sizeof(line) - l, "%s%08x", i ? " " : "", (unsigned)*(volatile uint32_t *)(addr + 4 * i));
	REPLY("[mem] %08x: %s\r\n", (unsigned)addr, line);
}

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
#if (CONFIG_SYS_CPU0)
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
#if CONFIG_DVP_CAMERA
/* cam probe [PWR_PIN ...]: power the sensor rails (default GPIO 9 + 28), run MCLK, report the idle
 * level of every candidate I2C pin (pull-ups present = 1), then scan I2C0 (20/21) and I2C1 on the
 * pin groups 42/43 and 14/15 for ACKs. GPIO 0/1 (the console UART) are left alone. Runs in its own
 * thread; results go to the UART log and the next `cam probe` reply buffer is not used. */
static int s_probe_pins[8], s_probe_n;

static int i2c_scan(i2c_id_t id, const char *label)
{
	i2c_config_t cfg = {.baud_rate = I2C_BAUD_RATE_100KHZ, .addr_mode = I2C_ADDR_MODE_7BIT};
	uint8_t b;
	int found = 0;
	char line[160];
	int l = snprintf(line, sizeof(line), "[cam] %s:", label);

	bk_i2c_init(id, &cfg);
	for (uint32_t a = 0x08; a < 0x78; a++) {
		i2c_mem_param_t mp = {.dev_addr = a, .mem_addr = 0x00, .mem_addr_size = I2C_MEM_ADDR_SIZE_8BIT, .data = &b, .data_size = 1, .timeout_ms = 5};
		if (bk_i2c_memory_read(id, &mp) == BK_OK && l < (int)sizeof(line) - 8) {
			l += snprintf(line + l, sizeof(line) - l, " 0x%02x", a);
			found++;
		}
	}
	bk_i2c_deinit(id);
	os_printf("%s%s\r\n", line, found ? "" : " none");
	return found;
}

static const int s_i2c_cand[6] = {20, 21, 42, 43, 14, 15};   /* I2C0, I2C1 group 42/43, I2C1 group 14/15 */

static void cam_idle_levels(int *lv)
{
	for (int i = 0; i < 6; i++) {
		pin_configure(s_i2c_cand[i], GPIO_INPUT_ENABLE, GPIO_PULL_DISABLE);
		lv[i] = bk_gpio_get_input(s_i2c_cand[i]);
	}
}

static void cam_probe_task(beken_thread_arg_t arg)
{
	int lv[6];

	for (int i = 0; i < s_probe_n; i++) {
		pin_configure(s_probe_pins[i], GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		bk_gpio_set_output_high(s_probe_pins[i]);
	}
	rtos_delay_milliseconds(50);
	bk_video_gpio_init(DVP_GPIO_CLK);
	bk_video_dvp_mclk_enable(YUV_MODE);
	rtos_delay_milliseconds(50);
	cam_idle_levels(lv);
	os_printf("[cam] probe: %d rail pins high, MCLK on; idle 20=%d 21=%d 42=%d 43=%d 14=%d 15=%d\r\n",
		  s_probe_n, lv[0], lv[1], lv[2], lv[3], lv[4], lv[5]);
	/* only scan a pair that shows pull-ups on both lines: a floating bus hangs the I2C engine */
	if (lv[0] && lv[1])
		i2c_scan(I2C_ID_0, "I2C0 (20/21)");
	if (lv[2] && lv[3]) {
		gpio_dev_unmap(42); gpio_dev_map(42, GPIO_DEV_I2C1_SCL);
		gpio_dev_unmap(43); gpio_dev_map(43, GPIO_DEV_I2C1_SDA);
		i2c_scan(I2C_ID_1, "I2C1 (42/43)");
		gpio_dev_unmap(42); gpio_dev_unmap(43);
	}
	if (lv[4] && lv[5]) {
		gpio_dev_unmap(14); gpio_dev_map(14, GPIO_DEV_I2C1_SCL);
		gpio_dev_unmap(15); gpio_dev_map(15, GPIO_DEV_I2C1_SDA);
		i2c_scan(I2C_ID_1, "I2C1 (14/15)");
		gpio_dev_unmap(14); gpio_dev_unmap(15);
	}
	bk_video_dvp_mclk_disable();
	os_printf("[cam] probe done\r\n");
	rtos_delete_thread(NULL);
}

/* cam pwrscan: drive every free GPIO high, then low, and watch the I2C candidate pins for pull-ups
 * appearing (the sensor board's pull-ups sit on its own supply): finds the camera power switch and
 * the I2C pair together, with no I2C traffic. GPIO 9 and 28 stay high throughout. */
static void cam_pwrscan_task(beken_thread_arg_t arg)
{
	static const int sweep[] = {2, 3, 4, 5, 6, 8, 12, 13, 16, 17, 18, 19, 22, 23, 25, 26, 40, 41, 47, 48, 49, 50, 51, 52, 54, 55};
	int lv[6], base[6];

	pin_configure(BOARD_PIN_SENSOR_RAIL, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE); bk_gpio_set_output_high(BOARD_PIN_SENSOR_RAIL);
	pin_configure(BOARD_PIN_CAM_PWR, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE); bk_gpio_set_output_high(BOARD_PIN_CAM_PWR);
	bk_video_gpio_init(DVP_GPIO_CLK);
	bk_video_dvp_mclk_enable(YUV_MODE);
	rtos_delay_milliseconds(50);
	cam_idle_levels(base);
	os_printf("[cam] pwrscan baseline 20=%d 21=%d 42=%d 43=%d 14=%d 15=%d\r\n", base[0], base[1], base[2], base[3], base[4], base[5]);
	for (unsigned i = 0; i < sizeof(sweep) / sizeof(sweep[0]); i++) {
		int p = sweep[i];
		for (int level = 1; level >= 0; level--) {
			pin_configure(p, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
			if (level) bk_gpio_set_output_high(p); else bk_gpio_set_output_low(p);
			rtos_delay_milliseconds(80);
			cam_idle_levels(lv);
			if (os_memcmp(lv, base, sizeof(lv)))
				os_printf("[cam] pwrscan GPIO %d=%d -> 20=%d 21=%d 42=%d 43=%d 14=%d 15=%d  <==\r\n", p, level, lv[0], lv[1], lv[2], lv[3], lv[4], lv[5]);
		}
		pin_configure(p, GPIO_INPUT_ENABLE, GPIO_PULL_DISABLE);   /* back to hi-Z */
		rtos_delay_milliseconds(20);
	}
	bk_video_dvp_mclk_disable();
	os_printf("[cam] pwrscan done\r\n");
	rtos_delete_thread(NULL);
}

/* ---- software (bit-banged) I2C on any two GPIOs: safe probe of the sensor bus (the SDK's I2C1
 * driver is pinned to GPIO 0/1 and resets this board). Open-drain emulated: 1 = input + pull-up,
 * 0 = driven low. Slow (~10 kHz) but every sensor tolerates that. ---- */
static int s_bb_scl = 42, s_bb_sda = 43;
static void bb_wait(void) { for (volatile int i = 0; i < 300; i++) ; }
static void bb_set(int pin, int v)
{
	if (v) {
		pin_configure(pin, GPIO_INPUT_ENABLE, GPIO_PULL_UP_EN);
	} else {
		pin_configure(pin, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		bk_gpio_set_output_low(pin);
	}
}
static void bb_start(void) { bb_set(s_bb_sda, 1); bb_set(s_bb_scl, 1); bb_wait(); bb_set(s_bb_sda, 0); bb_wait(); bb_set(s_bb_scl, 0); bb_wait(); }
static void bb_stop(void) { bb_set(s_bb_sda, 0); bb_wait(); bb_set(s_bb_scl, 1); bb_wait(); bb_set(s_bb_sda, 1); bb_wait(); }
static int bb_write(uint8_t b)   /* returns 1 on ACK */
{
	int ack;
	for (int i = 7; i >= 0; i--) {
		bb_set(s_bb_sda, (b >> i) & 1); bb_wait();
		bb_set(s_bb_scl, 1); bb_wait();
		bb_set(s_bb_scl, 0); bb_wait();
	}
	bb_set(s_bb_sda, 1); bb_wait();
	bb_set(s_bb_scl, 1); bb_wait();
	ack = !bk_gpio_get_input(s_bb_sda);
	bb_set(s_bb_scl, 0); bb_wait();
	return ack;
}
static uint8_t bb_read(int ack)
{
	uint8_t b = 0;
	bb_set(s_bb_sda, 1);
	for (int i = 7; i >= 0; i--) {
		bb_wait(); bb_set(s_bb_scl, 1); bb_wait();
		b |= (bk_gpio_get_input(s_bb_sda) & 1) << i;
		bb_set(s_bb_scl, 0);
	}
	bb_set(s_bb_sda, ack ? 0 : 1); bb_wait();
	bb_set(s_bb_scl, 1); bb_wait();
	bb_set(s_bb_scl, 0); bb_wait();
	bb_set(s_bb_sda, 1);
	return b;
}
static int bb_reg_read(uint8_t addr, uint32_t reg, int reg16, uint8_t *out)
{
	int ok;
	bb_start();
	ok = bb_write(addr << 1);
	if (ok && reg16) ok = bb_write(reg >> 8);
	if (ok) ok = bb_write(reg & 0xff);
	if (ok) { bb_stop(); bb_start(); ok = bb_write((addr << 1) | 1); }   /* SCCB: stop, then a fresh start */
	if (ok) *out = bb_read(0);
	bb_stop();
	return ok;
}

static void cam_bb_task(beken_thread_arg_t arg)
{
	uint8_t v, found[8];
	int n = 0;
	char line[160];
	int l;

	bk_video_gpio_init(DVP_GPIO_CLK);
	bk_video_dvp_mclk_enable(YUV_MODE);
	rtos_delay_milliseconds(30);
	bb_set(s_bb_scl, 1); bb_set(s_bb_sda, 1); bb_wait();
	l = snprintf(line, sizeof(line), "[cam] bb scan SCL=%d SDA=%d (idle scl=%d sda=%d):", s_bb_scl, s_bb_sda,
		     bk_gpio_get_input(s_bb_scl), bk_gpio_get_input(s_bb_sda));
	for (uint32_t a = 0x08; a < 0x78; a++) {
		int ack;
		bb_start(); ack = bb_write(a << 1); bb_stop();
		if (ack) {
			if (l < (int)sizeof(line) - 8) l += snprintf(line + l, sizeof(line) - l, " 0x%02x", a);
			if (n < 8) found[n++] = a;
		}
	}
	os_printf("%s%s\r\n", line, n ? "" : " none");
	for (int i = 0; i < n && n < 20; i++) {   /* a real device answers at one or two addresses, not all */
		uint8_t a = found[i];
		l = snprintf(line, sizeof(line), "[cam] 0x%02x:", a);
		static const struct { const char *name; uint32_t reg; int r16; } ids[] = {
			{"f0", 0xf0, 0}, {"f1", 0xf1, 0}, {"00", 0x00, 0}, {"0a", 0x0a, 0}, {"0b", 0x0b, 0}, {"3107", 0x3107, 1}, {"3108", 0x3108, 1}, {"0001", 0x0001, 1}, {"0002", 0x0002, 1}};
		for (unsigned k = 0; k < sizeof(ids) / sizeof(ids[0]); k++)
			if (bb_reg_read(a, ids[k].reg, ids[k].r16, &v)) l += snprintf(line + l, sizeof(line) - l, " r%s=%02x", ids[k].name, v);
		os_printf("%s\r\n", line);
	}
	bb_set(s_bb_scl, 1); bb_set(s_bb_sda, 1);
	bk_video_dvp_mclk_disable();
	os_printf("[cam] bb done\r\n");
	rtos_delete_thread(NULL);
}

#endif
#endif
#endif
#if (CONFIG_SYS_CPU0)
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
#if CONFIG_DVP_CAMERA
void cli_cam_probe(int argc, char **argv)
{
	beken_thread_t t;

	s_probe_pins[0] = 9; s_probe_pins[1] = 28; s_probe_n = 2;
	for (int i = 2; i < argc && s_probe_n < 8; i++)
		s_probe_pins[s_probe_n++] = os_strtoul(argv[i], NULL, 10);
	if (argc >= 3 && os_strcmp(argv[2], "rails") == 0) {   /* cam probe rails on|off : GPIO 9, 28, 5, 6 */
		cam_rails(argc >= 4 && os_strcmp(argv[3], "on") == 0);
		REPLY("[cam] rails 9/28/5/6 %s\r\n", argc >= 4 && os_strcmp(argv[3], "on") == 0 ? "high" : "low");
		return;
	}
	if (argc >= 5 && os_strcmp(argv[2], "bb") == 0) {   /* cam probe bb SCL SDA : software I2C scan */
		s_bb_scl = os_strtoul(argv[3], NULL, 10);
		s_bb_sda = os_strtoul(argv[4], NULL, 10);
		REPLY("[cam] bb scan on SCL=%d SDA=%d started, see UART\r\n", s_bb_scl, s_bb_sda);
		if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "cambb", cam_bb_task, 4096, NULL) != BK_OK)
			REPLY("[cam] thread failed\r\n");
		return;
	}
	if (argc >= 3 && os_strcmp(argv[2], "scan") == 0) {   /* cam probe scan = power-pin sweep */
		REPLY("[cam] pwrscan started, see UART (~6 s)\r\n");
		if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "campwr", cam_pwrscan_task, 4096, NULL) != BK_OK)
			REPLY("[cam] thread failed\r\n");
		return;
	}
	REPLY("[cam] probe started (%d rail pins), see UART\r\n", s_probe_n);
	if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "camprobe", cam_probe_task, 4096, NULL) != BK_OK)
		REPLY("[cam] thread failed\r\n");
}
#endif
#endif
#endif
