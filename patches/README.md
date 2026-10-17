# patches/ — changes to Beken's bk_idk (Armino SDK)

Applied to a checkout of [bekencorp/bk_idk](https://github.com/bekencorp/bk_idk), branch
`release/v2.0.1` (Apache-2.0), the tree that `scripts/build.sh` mounts
as the SDK (`SDK`, default `~/reps/oss/bk_idk`). The script applies every `*.patch` here on each
run, idempotently: a patch that already applies in reverse is reported as "already applied", one
that applies cleanly is applied, anything else aborts the build (the SDK checkout has drifted).
Nothing is committed in the SDK tree; `git -C $SDK diff` shows the applied set and
`git -C $SDK checkout .` removes it.

Each hunk carries a `doorbell-bk7258-fw` comment at the change site (the project's name before 0.28;
kept, because the build detects an applied patch by reversing it).

| Patch | File | What it does |
|---|---|---|
| `0001-psram-use-configured-chip-id.patch` | `middleware/driver/psram/psram_driver.c` | With PSRAM auto-detect off, use the configured APS128XXO-OB9 id instead of probing (probing hangs on this board). |
| `0002-cli-enable-adc-commands.patch` | `components/bk_cli/cli_config.h` | Turn on the SDK `adc` CLI on CPU0 (used by the pin-mapping and light-sensor tools). |
| `0003-cli-register-gpio-adc-in-release.patch` | `components/bk_cli/cli_main.c` | Register the `gpio`/`adc` CLIs in release builds too, without `CONFIG_DEBUG_FIRMWARE` (which does not link). |
| `0004-httpc-tolerate-empty-header-values.patch` | `components/http/utils_httpc.c` | OTA client: accept response headers with an empty value (`Server: ` from Home Assistant's aiohttp) and skip unparsable header lines instead of failing the download. |
| `0005-dvp-camera-mclk-settle.patch` | `middleware/driver/camera/dvp_camera.c` | Wait 600 ms after enabling MCLK before probing the sensor over I2C (the stock firmware waits 1 s; the OV9650 does not answer immediately). |
| `0006-sensor-id-ov9650.patch` | `include/driver/dvp_camera_types.h` | Add `ID_OV9650` to `sensor_id_t` (the AVDK driver in `avdk-overlay/` uses it). |
| `0007-gpio-irq-cpu0-only.patch` | `middleware/driver/gpio/gpio_driver_base.c` | Register the GPIO ISR and route the GPIO interrupt only on CPU0. The status register is shared between cores and CPU1's handler cleared CPU0's edges (PIR, button) whenever the camera pipeline was running. |
| `0008-fatfs-weak-get-fattime.patch` | `components/fatfs/ff.c` | Make `get_fattime()` weak so the application can supply the SNTP time for FAT timestamps (`sdcard.c`). |
| `0009-sdio-cmd-wait-outlasts-hw-timeout.patch` | `middleware/driver/sdio_host/sdio_host_driver.c` | `SDIO_CMD_WAIT_TIME` 4 -> 30 ms: the software wait for a command response now outlasts the hardware response timeout (25 ms at the 100 kHz init clock), so a late response can no longer be queued by the ISR after the caller gave up and desynchronise every later command. |
| `0010-gpio-default-map-doorbell.patch` | `middleware/soc/bk7258/soc/gpio_map.h` | Boot-time pin map (`GPIO_DEFAULT_DEV_CONFIG`, applied by `bk_gpio_driver_init()` on CPU0 at every reset) aligned with the doorbell's stock firmware: GPIO 53 (button = the 433 MHz chime TX key line) input + pull-up instead of an LCD_B2 output that rang every paired chime at each boot/wake; GPIO 6/7 hi-Z; GPIO 27-39 peripheral function off until the camera driver maps them; GPIO 25/26/50 keep their driven level through low power. |

About the numbering: `0005` was originally an I2C1-pin remap that turned out to be wrong and was
reverted during the camera bring-up; the number was then reused for the MCLK settle delay, which
had lived only in the SDK working tree until it was captured here. Apply order does not matter
(the patches touch different files).

All patches are needed (0005 and 0006 are the camera ones; before 0.28 a camera-less build used the
others alone). 0010 changes an SDK header that
almost everything includes: expect a full rebuild (about 11 min) the first time it is applied.
