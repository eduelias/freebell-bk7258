/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: private interface between the application modules (app_main.c, cmd.c, board_io.c, bringup.c,
 * net.c, audio_engine.c, camera.c, cam_http.c, ringfx.c). The feature files (pir.c, power.c, talk.c, ...) use their
 * own headers and the db_* / app_* hooks instead. */
#ifndef DOORBELL_APP_INTERNAL_H
#define DOORBELL_APP_INTERNAL_H

#include "bk_private/bk_init.h"
#include "bk_private/bk_cli.h"
#include <components/system.h>
#include <os/os.h>
#include <os/str.h>
#include <os/mem.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <components/shell_task.h>
#include <driver/gpio.h>
#include <driver/adc.h>
#if (CONFIG_SYS_CPU0)
#include <errno.h>
#include <modules/wifi.h>
#include <modules/wifi_types.h>
#include <components/event.h>
#include <components/netif.h>
#include <components/netif_types.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <driver/aud_dac.h>
#include <driver/aud_adc.h>
#include <driver/aud_dac_types.h>
#include <driver/aud_adc_types.h>
#include <driver/flash.h>
#include <driver/dma.h>
#include "board.h"   /* every GPIO / ADC channel of the board (boards/<board>.h) */
#include "pir.h"
#include "power.h"   /* feat/power: battery, charger, deep sleep */
#include "aud_engine.h"   /* engine API implemented below, used by talk.c */
#include "talk.h"         /* two-way audio over HTTP (/speak, /mic.wav) */
#include "audio_voc.h"    /* full-duplex voice engine (SDK aud_intf voice mode, AEC) for the RTSP audio tracks */
#include "sdcard.h"   /* optional microSD storage (feature "sd") */
#include "policy.h"   /* fw 0.25: policy pull, volume, RAM log, GET /cmd (policy.c) */
#endif
#ifdef DOORBELL_AVDK   /* all cores: media_service_init() runs on CPU0/1/2 */
#include "media_service.h"
#if (CONFIG_SYS_CPU0)   /* the multimedia component (media_app) is only linked into the CPU0 image */
#include "media_app.h"
#include "rtsp.h"
#include <driver/media_types.h>
#include <driver/i2c.h>
#include <driver/i2c_types.h>
#if CONFIG_DVP_CAMERA
#include <driver/video_common_driver.h>
#endif
#endif
#endif
#include <stdarg.h>
#include "night.h"   /* LED arbiter, night light, ring indicator, `night` / `sys` commands (night.c) */
#include "applog.h"  /* last: our os_printf lines go to the RAM log ring (policy.c); CPU0 only */

#include "version.h"   /* FW_NAME, FW_VERSION (the build scripts read it too) */

/* Pins our own commands must never reconfigure: UART0 console (RX/TX). */
#define PIN_IS_CONSOLE(p) ((p) == 10 || (p) == 11)
#define PIN_COUNT 56

/* Per-pin config registers: SOC_AON_GPIO_REG_BASE (include/soc/bk7258/reg_base.h, CONFIG_SPE=1 so
 * SOC_ADDR_OFFSET is 0), one 32-bit word per pin. Bits (soc/gpio_struct.h): 0 input value,
 * 1 output value, 2 input_en, 3 output_en, 4 pull up(1)/down(0), 5 pull_en, 6 second-function en,
 * 7 input monitor, 8-9 drive strength, 10-13 interrupt. */
#define AON_GPIO_BASE 0x44000400u
#define PIN_CFG(p) (*(volatile uint32_t *)(AON_GPIO_BASE + 4u * (p)))

extern void user_app_main(void);
extern void rtos_set_user_app_entry(beken_thread_function_t entry);
extern float saradc_calculate(uint16_t adc_val);
/* In libdriver (middleware/driver/gpio), not in a public header: releases a pin from the SDK
 * device map, otherwise bk_gpio_set_config() fails with BK_ERR_GPIO_INTERNAL_USED (-0x2009). */
extern bk_err_t gpio_dev_unmap(gpio_id_t gpio_id);
extern bk_err_t gpio_dev_map(gpio_id_t gpio_id, gpio_dev_t dev);
/* EasyFlash KV (components/easy_flash/bk_ef.h); value_len bytes, returns len read / EF_NO_ERR */
extern int bk_set_env_enhance(const char *key, const void *value, int value_len);
extern int bk_get_env_enhance(const char *key, void *value, int value_len);

/* ---- shared between the application modules (definitions in the file named on each block) ---- */
#define CLI_ARGS char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv

/* cmd.c: one command's reply text for the TCP channel / GET /cmd (the UART still gets everything) */
extern char s_reply[640];
extern bool s_reply_on;
#define REPLY(...) do { os_printf(__VA_ARGS__); if (s_reply_on) { size_t _l = os_strlen(s_reply); \
	if (_l < sizeof(s_reply) - 1) snprintf(s_reply + _l, sizeof(s_reply) - _l, __VA_ARGS__); } } while (0)
void cmd_register(void);
void cmd_init(void);
void cmd_start_tcp(void);

/* board_io.c */
bk_err_t pin_configure(uint32_t p, gpio_io_mode_t io, gpio_pull_mode_t pull);
int kv_get_str(const char *key, char *out, int cap);
void kv_set_str(const char *key, const char *val);
uint32_t adc_read_mv(uint32_t chan);

/* bringup.c: board mapping tools */
void cli_pin_cmd(CLI_ARGS);
void cli_mem_cmd(CLI_ARGS);
void cli_adc_read_cmd(CLI_ARGS);
void cli_cam_probe(int argc, char **argv);

/* net.c: Wi-Fi, webhook reports, button/PIR events */
extern char s_ip[16];
extern volatile bool s_got_ip;
void net_load_settings(void);
void net_start_events(void);
void net_start_reporter(void);
void net_connect(void);
bool net_ota_busy(void);
bool net_reports_pending(void);
void net_wake_input(uint32_t pin);
void cli_wifi_cmd(CLI_ARGS);
void cli_ota_cmd(CLI_ARGS);
void cli_hook_cmd(CLI_ARGS);

/* audio_engine.c */
extern volatile bool s_spk_busy;
extern char s_net_host[32];   /* test rig peer (`spk net`, `mic rec`, `cam push`) */
extern uint16_t s_net_port;
void pa_set(int on);
void spk_start(uint32_t hz, uint32_t ms, uint32_t addr, uint32_t len);
int rig_tcp_connect(const char *host, uint16_t port);
bool audio_busy(void);
void cli_spk_cmd(CLI_ARGS);
void cli_mic_cmd(CLI_ARGS);

/* ringfx.c: light sensor, ring effects, LEDs */
extern uint32_t s_dark_mv;          /* light mV above this = dark */
extern uint32_t s_light_chan;
#define LIGHT_ADC_CHAN s_light_chan
extern volatile bool s_fx_running;
uint32_t light_read_mv(void);
void ringfx_start(void);
void ringfx_init(void);
void ringfx_leds_off(void);
bool ringfx_busy(void);
void cli_ring_cmd(CLI_ARGS);
void cli_led_cmd(CLI_ARGS);

#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
/* camera.c */
#ifdef CONFIG_JPEG_FRAME_SIZE
#define CAM_BUF_MAX (CONFIG_JPEG_FRAME_SIZE)
#else
#define CAM_BUF_MAX (150 * 1024)
#endif
extern bool s_cam_open;
extern volatile bool s_cam_opening;          /* cam_open_locked() in progress: light_read_mv() leaves the camera rail alone */
extern volatile uint32_t s_light_last_mv;
/* the latest JPEG, for cam_http.c: buffer + metadata under s_cam_mutex */
extern uint8_t *s_cam_buf;
extern volatile uint32_t s_cam_len, s_cam_frames, s_cam_seq, s_cam_w, s_cam_h;
extern beken_mutex_t s_cam_mutex;
int cam_copy_latest(uint8_t *dst, uint32_t cap, uint32_t *seq);
int cam_want_jpeg(void);
uint32_t cam_settle_wait(void);
void cam_rails(int on);
void camera_init(void);
void camera_close_for_sleep(void);
void cli_cam_cmd(CLI_ARGS);
void cli_rtsp_cmd(CLI_ARGS);

/* cam_http.c */
void cam_http_start(void);
bool cam_http_busy(void);
#endif

#endif
