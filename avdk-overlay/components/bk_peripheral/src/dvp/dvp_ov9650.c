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

int ov9650_set_fps(frame_fps_t fps)
{
	LOGI("%s %d (fixed at 15)\n", __func__, fps);
	return fps == FPS15 ? 0 : -1;
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
