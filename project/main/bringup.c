/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: bring-up tools for mapping a new board: `mem` (raw reads), `cam probe` (camera rails, I2C scans,
 * power scan, bit-banged I2C). Compiled only with DOORBELL_BRINGUP 1 (features.h); `pin` and `adc` stay in every
 * build (board_io.c). Split out of app_main.c in 0.28.0. */
#include "app_internal.h"

#if DOORBELL_BRINGUP
/* ---- mem: raw memory reads (bring-up only: CPU1 counters live in shared SRAM) ---- */

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
#endif /* DOORBELL_BRINGUP */
