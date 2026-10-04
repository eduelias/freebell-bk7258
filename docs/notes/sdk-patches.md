# SDK and AVDK patches

Applied by `scripts/build.sh` / `scripts/build-avdk.sh` to the `release/v2.0.1` checkouts of
bk_idk (`patches/`) and bk_avdk (`patches-avdk/`, plus the added files in `avdk-overlay/`). All of
them are derivative works of Beken's Apache-2.0 code (see `NOTICE`).

## bk_idk (`patches/`)

- `0001-psram-use-configured-chip-id.patch`: with no cached PSRAM id, the stock driver probes the
  8 MB APS6408L first, which hangs this board right after the shell prompt. With
  `CONFIG_PSRAM_AUTO_DETECT=n` and `CONFIG_PSRAM_APS128XXO_OB9=y` (in
  `project/config/bk7258/config`) it uses the configured part instead.
- `0002-cli-enable-adc-commands.patch`: `CLI_CFG_ADC` is hard-coded 0 in `cli_config.h`; set it to
  1 on CPU0 so `adc_driver` / `adc_test` exist (battery sensing).
- `0003-cli-register-gpio-adc-in-release.patch`: the whole driver CLI block in `cli_main.c`
  (`gpio`, `uart`, `spi`, `i2c`, `timer`, `flash`, `reg`, `adc`) sits behind
  `CONFIG_DEBUG_FIRMWARE`, which only Beken's internal build defines (as a `-D` flag; there is no
  Kconfig symbol, so a defconfig `=n` is silently dropped). Declaring the symbol and setting it `y`
  does enable the block, but then `cli_reg` references `*_struct_dump` helpers that compile to
  nothing, because `hal_config.h` tests the flag before `sdkconfig.h` is in scope, and the link
  fails. So this patch registers just `cli_gpio_init()` and `cli_adc_init()` unconditionally when
  `CONFIG_DEBUG_FIRMWARE` is off.
- `0004-httpc-tolerate-empty-header-values.patch`: `httpclient_response_parse()` rejected a header
  line whose value is empty (`sscanf` matched one field). Home Assistant's aiohttp sends `Server: `
  with an empty value, so every OTA fetched from HA failed with `ERROR_HTTP` (`[ota] result -4`).
  The patch treats an empty value as valid.
- `0006-sensor-id-ov9650.patch`: adds `ID_OV9650` to `sensor_id_t` in
  `include/driver/dvp_camera_types.h` so the AVDK driver can register under its own id.
- `0007-gpio-irq-cpu0-only.patch`: `bk_gpio_driver_init()` registers the GPIO ISR and enables the
  GPIO interrupt towards the CPU only when built for CPU0. The interrupt status is one shared
  register set; on CPU1/CPU2 the stock init made those cores clear CPU0's pending edges (PIR,
  button) whenever they were up, i.e. whenever the camera was open. Story in
  [gpio-irq.md](gpio-irq.md).
- `0008-fatfs-weak-get-fattime.patch`: `components/fatfs/ff.c` returns a fixed 2017-05-01 for every
  FAT timestamp; the patch only adds `__attribute__((weak))` so `sdcard.c` can supply the SNTP time.
- `0009-sdio-cmd-wait-outlasts-hw-timeout.patch`: `bk_sdio_host_wait_cmd_response()` pops the ISR's
  completion message with a 4 ms timeout while the hardware response timeout at the 100 kHz init
  clock is 2500 clocks = 25 ms. A response that lands between 4 and 25 ms is queued after the caller
  has returned "timeout"; the next command then pops the stale message at once and every later wait
  is out of step (seen 2026-10-04 after a burst of unanswered CMD8 retries: CMD0 "timing out" in
  84 ms, stale response words). The patch raises the wait to 30 ms (one define). Normal commands are
  unaffected (the message is already queued when the wait starts); only failing commands take 30 ms
  instead of 4 ms per retry. Story in [sdcard.md](sdcard.md).
- `0010-gpio-default-map-doorbell.patch`: `GPIO_DEFAULT_DEV_CONFIG` in
  `middleware/soc/bk7258/soc/gpio_map.h` is the pin map `bk_gpio_driver_init()` ->
  `gpio_hal_default_map_init()` writes on CPU0 at every reset (cold boot, warm reboot and every
  deep-sleep wake, which is a SoC reset on the BK7258), before any application code. The SDK default
  is the Beken LCD/camera dev-kit map and left GPIO 53 as an **LCD_B2 peripheral output**. On the
  doorbell GPIO 53 is the button line, which also keys the on-board 433 MHz chime transmitter: the
  peripheral drove it low for the few hundred ms until `inputs_init()` reclaimed it, so all paired
  radio chimes rang once at every boot and every wake (found 2026-10-04 by disassembling the stock
  firmware's own table; medium-high confidence until the owner confirms silence). The patch makes
  the table the stock one where it differs, row by row:
  - GPIO 53: `SECOND_FUNC_DISABLE, GPIO_DEV_INVALID, INPUT_ENABLE, PULL_UP_EN, INT_DISABLE,
    INT_TYPE_LOW_LEVEL, LOW_POWER_KEEP_INPUT_STATUS, DRIVER_CAPACITY_3` (was `FUNC_ENABLE, LCD_B2,
    IO_DISABLE, PULL_DISABLE, ..., DISCARD, CAPACITY_1`).
  - GPIO 6, 7: `FUNC_DISABLE, GPIO_DEV_INVALID, IO_DISABLE, PULL_DISABLE` (were I2S1 CLK/SYNC
    outputs; 6 is the SD card detect, 7 the ADC9 rail sense).
  - GPIO 27-39: `SECOND_FUNC_DISABLE` (dev names kept for documentation; 38 -> `GPIO_DEV_INVALID`
    as in the stock). Safe for the camera: `bk_dvp_camera_open()` calls
    `bk_video_gpio_init(DVP_GPIO_ALL)` (`middleware/driver/camera/dvp_camera.c`), which
    `gpio_dev_unmap()`s + `gpio_dev_map()`s 27 and 29-39 from `camera_gpio_map_table` in
    `video_common_driver.c` on every open; 28 (I2S1_MCLK in the SDK table) is the camera/SD/light
    sensor power enable here and is unmapped and driven by `cam_rails()` on CPU0 and by
    `CONFIG_CAMERA_CTRL_POWER_GPIO_ID=28` on CPU1.
  - GPIO 25, 26, 50 (PIR sensitivity/enable): `low_power_io_ctrl` `DISCARD` ->
    `GPIO_LOW_POWER_KEEP_OUTPUT_STATUS` (= 3, the stock value). `gpio_hal_map_init()` still applies
    the row's `IO_DISABLE` afterwards, so the boot state is unchanged; the effect is that
    `gpio_lowpower_keep_status_init()` marks the three pins as "keep" so a deep sleep holds whatever
    `pir.c` drove instead of floating them. `power.c` already registers the same dynamically when
    `wake_pir` is on; this covers the `wake_pir off` case too (belt and braces).
  - Every other row is the SDK's (the stock's rows 0-5, 8-24, 40-49, 51, 52, 54, 55 are identical
    to the SDK's).

  Core scope: `CONFIG_GPIO_DEFAULT_SET_SUPPORT` is `y` only in `bk7258.defconfig` (CPU0) and `n` in
  `bk7258_cp1.defconfig` / `bk7258_cp2.defconfig`; `project-avdk/config/bk7258_cp{1,2}/config` do
  not override it, so CPU1/CPU2 apply no table at all, exactly like the stock cp1/cp2. With the
  default map off, the I2C driver maps its own pins (`i2c_init_gpio()` -> `I2C_SET_PIN`, so the
  sensor's I2C1 on CPU1 works today without any table) and the SDIO driver likewise. No change to
  the cp1/cp2 configs was needed, and the `gpio_hal_default_map_init()` interrupt-clear loop runs
  only on CPU0 inside `bk_init()`, long before `inputs_init()` registers the button and PIR ISRs,
  so it cannot undo them. Hardware note: `gpio_hal_map_init()` only *unmaps* a pin's function when
  the row has `SECOND_FUNC_ENABLE`; a `FUNC_DISABLE` row leaves the function-select bit as it was
  (0 after a cold boot; whatever `gpio_dev_unmap()` left after a warm reboot or a sleep).

`0005-dvp-camera-mclk-settle.patch` was only captured on 2026-10-03 (it had lived as an uncommitted change in the author's SDK tree): a 600 ms settle after MCLK starts in `bk_dvp_camera_enumerate()`, which the OV9650 needs before it answers on SCCB.

## bk_avdk (`patches-avdk/` and `avdk-overlay/`)

- `patches-avdk/0001-register-ov9650.patch`: declares `dvp_sensor_ov9650` in
  `dvp_sensor_devices.h` and adds it to the sensor list so auto-detection finds it.
- `avdk-overlay/components/bk_peripheral/src/dvp/dvp_ov9650.c`: the OV9650 driver (new file,
  copied into the AVDK tree by the build script). See [camera.md](camera.md#sensor).

## Kconfig choices worth knowing

- CPU0 (`project/config/bk7258/config`, rewritten by the build: sorted, comments stripped):
  `CONFIG_PSRAM_AUTO_DETECT=n`, `CONFIG_PSRAM_APS128XXO_OB9=y`, `CFG_LOG_LEVEL=2`,
  `CONFIG_SDCARD_POWER_GPIO_CTRL=n`.
- CPU1 (`project-avdk/config/bk7258_cp1/config`): `CONFIG_H264_P_FRAME_CNT=29` (GOP 30), H.264
  SEI/buffer sizes for 720p. After changing anything here delete `$OUT/doorbell/bk7258_cp1`,
  `_cp2` and `bk7258` before rebuilding.
