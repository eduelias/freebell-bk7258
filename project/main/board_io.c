/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: GPIO, ADC and KV helpers shared by every module. */
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
