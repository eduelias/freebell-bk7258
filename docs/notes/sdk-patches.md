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

(There is no 0005; it was folded into another change before release.)

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
