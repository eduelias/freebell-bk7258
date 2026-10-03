# patches-avdk/ — changes to Beken's bk_avdk (audio/video kit)

Applied to a checkout of [bekencorp/bk_avdk](https://github.com/bekencorp/bk_avdk), branch
`release/v2.0.1` (Apache-2.0), the tree that `scripts/build-avdk.sh` uses for the camera
flavour (`AVDK`, default `~/reps/oss/bk_avdk`). The script first copies `avdk-overlay/` over the
checkout (new files only: our OV9650 driver, `components/bk_peripheral/src/dvp/dvp_ov9650.c`),
then applies every `*.patch` here the same idempotent way as `patches/` (already applied ->
reported, applies cleanly -> applied, anything else -> the build stops). Nothing is committed in
the kit; `git -C $AVDK status` shows the overlay file as untracked and the patch as a diff.

| Patch | Files | What it does |
|---|---|---|
| `0001-register-ov9650.patch` | `components/bk_peripheral/include/dvp_sensor_devices.h`, `src/dvp/config.cmake`, `src/dvp/dvp_sensor_devices.c` | Declare `dvp_sensor_ov9650`, add `dvp_ov9650.c` to the DVP driver sources, and put the OV9650 first in `dvp_sensor_configs[]` so it is probed before the OV2640, which shares I2C address 0x30. |

The sensor id it relies on (`ID_OV9650`) comes from `patches/0006-sensor-id-ov9650.patch` in the
SDK tree. The register table in `dvp_ov9650.c` was recovered from the device's shipped firmware
for interoperability.
