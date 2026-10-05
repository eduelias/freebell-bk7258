/* SPDX-License-Identifier: Apache-2.0 */
/* doorbell-bk7258-fw: OmniVision OV9650 DVP sensor driver for the Beken AVDK. Register configuration
 * recovered from the device's shipped firmware for interoperability. */
// OV9650 DVP sensor driver for the Beken AVDK, written for the Tuya IP108 doorbell (BK7258).
// The register table is the one the vendor firmware programs (extracted from the stock image);
// the structure follows dvp_ov2640.c. The sensor sits on I2C1 (GPIO 0/1) at 0x30, power on GPIO 28
// (active high) and a PWDN/RESET pulse on GPIO 52 is applied by the application before open.

#include <driver/int.h>
#include <os/os.h>
#include <os/mem.h>
#include <driver/gpio.h>
#include <driver/gpio_types.h>
#include <driver/jpeg_enc.h>
#include <driver/jpeg_enc_types.h>
#include <driver/dvp_camera.h>
#include <driver/dvp_camera_types.h>
#include <driver/i2c.h>
#include "dvp_sensor_devices.h"

#define OV9650_WRITE_ADDRESS (0x60)
#define OV9650_READ_ADDRESS  (0x61)
#define TAG "ov9650"
#define LOGI(...) BK_LOGI(TAG, ##__VA_ARGS__)
#include <components/log.h>
#define OV9650_CHIP_PID (0x96)
#define OV9650_CHIP_VER (0x52)

/* SCCB (OmniVision) does not support a repeated START: a register read must be two transactions,
 * write(reg) + STOP, then START + read(1). The SDK's combined memory read fails on this sensor. */
static int ov9650_sccb_read(uint8_t reg, uint8_t *value)
{
	int ret = bk_i2c_master_write(CONFIG_DVP_CAMERA_I2C_ID, (OV9650_WRITE_ADDRESS >> 1), &reg, 1, 200);
	if (ret != BK_OK)
		return ret;
	return bk_i2c_master_read(CONFIG_DVP_CAMERA_I2C_ID, (OV9650_WRITE_ADDRESS >> 1), value, 1, 200);
}
#define SENSOR_I2C_READ(reg, value) \
	do { ov9650_sccb_read(reg, value); } while (0)
#define SENSOR_I2C_WRITE(reg, value) \
	do { dvp_camera_i2c_write_uint8((OV9650_WRITE_ADDRESS >> 1), reg, value); } while (0)

static bool ov9650_read_flag = false;

/* 1280x720 YUYV, as programmed by the vendor firmware (141 entries, last one is a terminator) */
const uint8_t sensor_ov9650_init_table[][2] =
{
	{0x6b, 0x0a}, {0x6a, 0x41}, {0x3b, 0xa9}, {0x13, 0xe7}, {0x01, 0x80}, {0x02, 0x80},
	{0x00, 0x80}, {0xa1, 0x41}, {0x10, 0x06}, {0x39, 0x50}, {0x38, 0x93}, {0x37, 0x81},
	{0x35, 0x91}, {0x0e, 0xa0}, {0x1e, 0x34}, {0xa8, 0x80}, {0x04, 0x00}, {0x0c, 0x00},
	{0x0d, 0x00}, {0x18, 0xbd}, {0x17, 0x1d}, {0x32, 0xad}, {0x03, 0x12}, {0x1a, 0x81},
	{0x19, 0x01}, {0x14, 0x1a}, {0x15, 0x02}, {0x3f, 0xf2}, {0x41, 0x02}, {0x42, 0xc1},
	{0x1b, 0x00}, {0x16, 0x06}, {0x33, 0xe2}, {0x34, 0xbf}, {0x96, 0x04}, {0x3a, 0x00},
	{0x8e, 0x00}, {0x3c, 0x77}, {0x8b, 0x06}, {0x94, 0x88}, {0x95, 0x88}, {0x40, 0xc1},
	{0x29, 0x3f}, {0x0f, 0x42}, {0x3d, 0x92}, {0x69, 0x40}, {0x5c, 0xb9}, {0x5d, 0x96},
	{0x5e, 0x10}, {0x59, 0xc0}, {0x5a, 0xaf}, {0x5b, 0x55}, {0x43, 0xf0}, {0x44, 0x10},
	{0x45, 0x68}, {0x46, 0x96}, {0x47, 0x60}, {0x48, 0x80}, {0x5f, 0xe0}, {0x60, 0x8c},
	{0x61, 0x20}, {0xa5, 0xd9}, {0xa4, 0x74}, {0x8d, 0x02}, {0x4f, 0x3a}, {0x50, 0x3d},
	{0x51, 0x03}, {0x52, 0x12}, {0x53, 0x26}, {0x54, 0x38}, {0x55, 0x40}, {0x56, 0x40},
	{0x57, 0x40}, {0x58, 0x0d}, {0x8c, 0x23}, {0x3e, 0x02}, {0xa9, 0xb8}, {0xaa, 0x92},
	{0xab, 0x0a}, {0x8f, 0xdf}, {0x90, 0x00}, {0x91, 0x00}, {0x9f, 0x00}, {0xa0, 0x00},
	{0x3a, 0x00}, {0x24, 0x98}, {0x25, 0x88}, {0x26, 0xc4}, {0x2a, 0x00}, {0x2b, 0x00},
	{0x2d, 0x00}, {0x6c, 0xf0}, {0x6d, 0xf0}, {0x6e, 0xf0}, {0x6f, 0xe0}, {0x70, 0xa0},
	{0x71, 0x70}, {0x72, 0x60}, {0x73, 0x50}, {0x74, 0x48}, {0x75, 0x40}, {0x76, 0x34},
	{0x77, 0x20}, {0x78, 0x14}, {0x79, 0x14}, {0x7a, 0x14}, {0x7b, 0x14}, {0x7c, 0x0f},
	{0x7d, 0x1e}, {0x7e, 0x3c}, {0x7f, 0x74}, {0x80, 0x88}, {0x81, 0x96}, {0x82, 0xa2},
	{0x83, 0xac}, {0x84, 0xb5}, {0x85, 0xbd}, {0x86, 0xca}, {0x87, 0xd2}, {0x88, 0xdc},
	{0x89, 0xe6}, {0x8a, 0xf0}, {0x11, 0x80}, {0x12, 0x00}, {0x0c, 0x00}, {0x0d, 0x00},
	{0x18, 0xbe}, {0x17, 0x1e}, {0x32, 0x80}, {0x03, 0x12}, {0x1a, 0x5b}, {0x19, 0x01},
	{0x2a, 0x10}, {0x2b, 0x34}, {0x6a, 0x6d}, {0x64, 0x15}, {0x65, 0x30}, {0x66, 0x01},
	{0x92, 0x82}, {0x93, 0x00}, {0xff, 0xff},
};

bool ov9650_detect(void)
{
	uint8_t pid = 0, ver = 0;

	SENSOR_I2C_READ(0x0A, &pid);
	SENSOR_I2C_READ(0x0B, &ver);
	BK_LOGW(TAG, "%s, id: 0x%02X%02X (expect 0x9652)\n", __func__, pid, ver);
	return pid == OV9650_CHIP_PID && ver == OV9650_CHIP_VER;
}

void ov9650_read_register(uint8_t addr, uint8_t data)
{
	uint8_t value = 0;

	if (ov9650_read_flag) {
		SENSOR_I2C_READ(addr, &value);
		LOGI("addr:0x%02x, wr:0x%02x, rd:0x%02x\r\n", addr, data, value);
	}
}

int ov9650_init(void)
{
	uint32_t size = sizeof(sensor_ov9650_init_table) / sizeof(sensor_ov9650_init_table[0]);

	LOGI("%s, %u registers\n", __func__, size);
	for (uint32_t i = 0; i < size; i++) {
		if (sensor_ov9650_init_table[i][0] == 0xff && sensor_ov9650_init_table[i][1] == 0xff)
			break;
		SENSOR_I2C_WRITE(sensor_ov9650_init_table[i][0], sensor_ov9650_init_table[i][1]);
		ov9650_read_register(sensor_ov9650_init_table[i][0], sensor_ov9650_init_table[i][1]);
		rtos_delay_milliseconds(1);   /* the vendor driver pauses after every write */
	}
	return 0;
}

int ov9650_set_ppi(media_ppi_t ppi)
{
	/* the init table already selects 1280x720; the vendor driver accepts 1280x720 and 800x480 */
	LOGI("%s %ux%u\n", __func__, ppi >> 16, ppi & 0xffff);
	return (ppi == PPI_1280X720 || ppi == PPI_800X480) ? 0 : -1;
}

/* Scene hint from CPU0 (fw 0.27): the application packs it into media_camera_device_t.info.fps,
 * which dvp_camera.c (bk_idk) copies verbatim through the mailbox and hands to set_fps() right
 * after init() programmed the table. frame_fps_t is a byte-sized bit-mask enum (FPS15 = bit 2 is
 * the only real rate of this sensor), so the other 7 bits are free: bit 7 = night, bit 6 = day,
 * bits 0-1 and 3-5 = a 5-bit start-exposure class (0 = driver default). Same encoding in
 * project/main/app_main.c. */
#define OV9650_SCENE_DAY   (1u << 6)
#define OV9650_SCENE_NIGHT (1u << 7)
#define OV9650_FPS_MASK    0x04u
#define OV9650_EXP_CLASS(v) (((((v) >> 3) & 7u) << 2) | ((v) & 3u))
/* class 1..11 = start exposure 1..1024 rows with AEC on at once; 0 = 4 rows; class 16..27 = the
 * same rows but AEC/AGC stay OFF for the first second (the trace thread enables them), an
 * experiment lever: the engine restarts from ~130 rows whenever AEC is enabled (measured). */
#define OV9650_EXP_MANUAL(c) ((c) >= 16u)
#define OV9650_EXP_LINES(c) (OV9650_EXP_MANUAL(c) ? (1u << (((c) - 16u) > 10u ? 10u : ((c) - 16u))) : (c) ? (1u << (((c) > 11u ? 11u : (c)) - 1u)) : 4u)

/* Debug words readable from CPU0 with `mem <addr> N` (addresses: grep ov9650_dbg / ov9650_trace
 * in the bk7258_cp1 app.map; flash XIP is mapped identically for both cores, SRAM too):
 *   ov9650_dbg[0] = 0x965000xx with xx = the fps word received, [1] = COM8<<24|COM11<<16|COM9<<8|AECH
 *   read back after the sequence, [2] = AECHM<<24|COM1<<16|GAIN<<8|AEW, [3] = start exposure lines.
 *   ov9650_trace[i] = AEC lines | GAIN<<16 | COM8<<24, sampled every 500 ms for 15 s after a day
 *   open (a short-lived CPU1 thread), so the converged exposure for a light level can be read. */
volatile uint32_t ov9650_dbg[4];
volatile uint32_t ov9650_trace[32];

static uint32_t ov9650_read_exposure(uint8_t *gain, uint8_t *com8)
{
	uint8_t h = 0, l = 0, m = 0, g = 0, c = 0;

	SENSOR_I2C_READ(0x10, &h);
	SENSOR_I2C_READ(0x04, &l);
	SENSOR_I2C_READ(0xa1, &m);
	SENSOR_I2C_READ(0x00, &g);
	SENSOR_I2C_READ(0x13, &c);
	if (gain)
		*gain = g;
	if (com8)
		*com8 = c;
	return ((uint32_t)(m & 0x3f) << 10) | ((uint32_t)h << 2) | (l & 3);
}

static bool ov9650_manual_first;
static volatile uint32_t ov9650_gen;   /* bumped per open: a trace thread of a previous open stops and never touches the new one */

static void ov9650_trace_task(beken_thread_arg_t arg)
{
	uint32_t gen = (uint32_t)arg;

	for (uint32_t i = 0; i < 30 && gen == ov9650_gen; i++) {
		uint8_t g, c;
		uint32_t e = ov9650_read_exposure(&g, &c);
		ov9650_trace[i] = (e & 0xffff) | ((uint32_t)g << 16) | ((uint32_t)c << 24);
		if (i == 1 && ov9650_manual_first && gen == ov9650_gen) {   /* manual-first class: AEC/AGC on after ~1 s */
			SENSOR_I2C_WRITE(0x13, 0xe7);
			ov9650_manual_first = false;
		}
		rtos_delay_milliseconds(500);
	}
	ov9650_trace[30] = 0xd0ded0de;
	rtos_delete_thread(NULL);
}

/* Day: the stock table starts the sensor in night mode (COM11 0x3b bit 7) with the AEC engine at a
 * long exposure (AECHM 0xa1 = 0x41 -> AEC[10]) and gain 0x80, and in daylight the loop needs ~10 s
 * of frames to come down (2026-10-05: 93 % of pixels clipped at +2 s, 1 % at +13 s). Writing the
 * exposure registers while AEC is enabled does nothing (the engine owns them; measured: no change
 * at all with v1 of this code), so: AEC/AGC off (AWB stays on), night mode off, lower AGC ceiling
 * and AE target, a start exposure chosen by CPU0 from the light sensor, gain 1x, then AEC/AGC on
 * again so the loop starts from there instead of from white. The loop moves about one stop per
 * 0.5 s either way (trace 2026-10-05 11:25: 64 -> 130 -> 65 -> 17 -> 9 -> 5 -> 2 -> 1 row at
 * 663 mV, then gain 0x0f -> 0 over 5 s), so the start is biased short: a sunny scene is right
 * at once and an overcast one is a stop or three up, which keeps the detail a white frame loses. */
static void ov9650_day(uint32_t lines, bool manual_first)
{
	static const uint8_t pre[][2] = {
		{0x13, 0xe2},   /* COM8: fast algorithm + unlimited step + banding + AWB, AEC/AGC OFF */
		{0x3b, 0x29},   /* COM11: night mode off, banding filter as in the table */
		{0x14, 0x0a},   /* COM9: AGC ceiling 2x (table: 4x) */
		{0x24, 0x78},   /* AEW: AE upper target (table 0x98) */
		{0x25, 0x68},   /* AEB: AE lower target (table 0x88) */
		{0xa1, 0x00},   /* AECHM: exposure[15:10] = 0 (table 0x41) */
	};
	uint8_t v10 = (lines >> 2) & 0xff, v04 = lines & 3, g, c;

	for (uint32_t i = 0; i < sizeof(pre) / sizeof(pre[0]); i++) {
		SENSOR_I2C_WRITE(pre[i][0], pre[i][1]);
		rtos_delay_milliseconds(1);
	}
	SENSOR_I2C_WRITE(0x10, v10);   /* AECH: exposure[9:2] */
	rtos_delay_milliseconds(1);
	SENSOR_I2C_WRITE(0x04, v04);   /* COM1: exposure[1:0] */
	rtos_delay_milliseconds(1);
	SENSOR_I2C_WRITE(0x00, 0x00);  /* GAIN: 1x (table 0x80) */
	rtos_delay_milliseconds(1);
	ov9650_manual_first = manual_first;
	if (!manual_first)
		SENSOR_I2C_WRITE(0x13, 0xe7);  /* COM8: AEC/AGC/AWB on (manual-first: the trace thread does this after ~1 s) */
	rtos_delay_milliseconds(1);
	{
		uint8_t r13 = 0, r3b = 0, r14 = 0, r10 = 0, ra1 = 0, r04 = 0, r00 = 0, r24 = 0;
		SENSOR_I2C_READ(0x13, &r13); SENSOR_I2C_READ(0x3b, &r3b); SENSOR_I2C_READ(0x14, &r14); SENSOR_I2C_READ(0x10, &r10);
		SENSOR_I2C_READ(0xa1, &ra1); SENSOR_I2C_READ(0x04, &r04); SENSOR_I2C_READ(0x00, &r00); SENSOR_I2C_READ(0x24, &r24);
		ov9650_dbg[1] = ((uint32_t)r13 << 24) | ((uint32_t)r3b << 16) | ((uint32_t)r14 << 8) | r10;
		ov9650_dbg[2] = ((uint32_t)ra1 << 24) | ((uint32_t)r04 << 16) | ((uint32_t)r00 << 8) | r24;
	}
	ov9650_dbg[3] = lines;
	(void)ov9650_read_exposure(&g, &c);
	{
		beken_thread_t t;
		for (uint32_t i = 0; i < 32; i++)
			ov9650_trace[i] = 0;
		rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "ov9650tr", ov9650_trace_task, 1024, (beken_thread_arg_t)ov9650_gen);
	}
}

/* Night: the table as shipped (night mode on, AGC 4x, bright target) plus the registers the stock
 * day/night switch wrote when it moved the IR-cut filter to night: fixed U/V = black and white
 * (TSLB 0x3a bit 4 + MANU/MANV 0x67/0x68 = 0x80). */
static const uint8_t ov9650_night_regs[][2] = {
	{0x3a, 0x11}, {0x67, 0x80}, {0x68, 0x80},
};

int ov9650_set_fps(frame_fps_t fps)
{
	uint32_t v = (uint32_t)fps & 0xff;

	ov9650_gen++;   /* retire the trace thread of the previous open before anything is programmed */
	ov9650_manual_first = false;
	ov9650_dbg[0] = 0x96500000u | v;
	ov9650_dbg[1] = ov9650_dbg[2] = ov9650_dbg[3] = 0;
	if (v & OV9650_SCENE_DAY) {
		ov9650_day(OV9650_EXP_LINES(OV9650_EXP_CLASS(v)), OV9650_EXP_MANUAL(OV9650_EXP_CLASS(v)));
	} else if (v & OV9650_SCENE_NIGHT) {
		for (uint32_t i = 0; i < sizeof(ov9650_night_regs) / sizeof(ov9650_night_regs[0]); i++) {
			SENSOR_I2C_WRITE(ov9650_night_regs[i][0], ov9650_night_regs[i][1]);
			rtos_delay_milliseconds(1);
		}
	}
	BK_LOGW(TAG, "%s 0x%02x: scene %s, start %u lines (class %u), sensor fixed at 15\n", __func__, v,
		(v & OV9650_SCENE_DAY) ? "day" : (v & OV9650_SCENE_NIGHT) ? "night" : "stock",
		(v & OV9650_SCENE_DAY) ? OV9650_EXP_LINES(OV9650_EXP_CLASS(v)) : 0, OV9650_EXP_CLASS(v));
	return (v & OV9650_FPS_MASK) ? 0 : -1;
}

int ov9650_dump(media_ppi_t ppi)
{
	uint8_t value;
	uint32_t size = sizeof(sensor_ov9650_init_table) / sizeof(sensor_ov9650_init_table[0]);

	for (uint32_t i = 0; i < size; i++) {
		SENSOR_I2C_READ(sensor_ov9650_init_table[i][0], &value);
		LOGI("0x%02x = 0x%02x (wrote 0x%02x)\r\n", sensor_ov9650_init_table[i][0], value, sensor_ov9650_init_table[i][1]);
	}
	return 0;
}

void ov9650_read_enable(bool enable)
{
	ov9650_read_flag = enable;
}

const dvp_sensor_config_t dvp_sensor_ov9650 =
{
	.name = "ov9650",
	.clk = MCLK_24M,
	.fmt = PIXEL_FMT_YUYV,
	.vsync = SYNC_HIGH_LEVEL,
	.hsync = SYNC_HIGH_LEVEL,
	.def_ppi = PPI_1280X720,
	.def_fps = FPS15,
	.fps_cap = FPS15,
	.ppi_cap = PPI_CAP_1280X720,
	.id = ID_OV9650,
	.address = (OV9650_WRITE_ADDRESS >> 1),
	.init = ov9650_init,
	.detect = ov9650_detect,
	.set_ppi = ov9650_set_ppi,
	.set_fps = ov9650_set_fps,
	.power_down = NULL,
	.dump_register = ov9650_dump,
	.read_register = ov9650_read_enable,
};
