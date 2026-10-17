#!/usr/bin/env bash
# Build the AVDK flavour (camera-capable) of the firmware: same app sources, built in Beken's
# bk_avdk tree (release/v2.0.1) with our bk_idk mounted as its SDK. Settings (SDK, AVDK, OUT,
# FW_DIR) come from the environment or ~/.config/doorbell-bk7258/env (see scripts/env.example).
# The project is mounted as /avdk/projects/doorbell; output lands in $OUT/doorbell/...
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
. "$HERE/scripts/env.sh"
SDK=${SDK:-$HOME/reps/oss/bk_idk}
AVDK=${AVDK:-$HOME/reps/oss/bk_avdk}
OUT=${OUT:-$HOME/data/doorbell-bk7258/avdk-build}
FW_DIR=${FW_DIR:-$HOME/data/doorbell-bk7258/firmware}
IMAGE=docker.io/bekencorp/armino-idk:1.5
# container runtime: CONTAINER_RUNTIME=podman|docker, else podman if installed, else docker
RUNTIME=${CONTAINER_RUNTIME:-}
if [ -z "$RUNTIME" ]; then
	if command -v podman >/dev/null 2>&1; then RUNTIME=podman
	elif command -v docker >/dev/null 2>&1; then RUNTIME=docker
	else echo "need podman or docker to run $IMAGE" >&2; exit 1; fi
fi

for p in "$HERE"/patches/*.patch; do
	name=$(basename "$p")
	if git -C "$SDK" apply --reverse --check "$p" 2>/dev/null; then echo "patch already applied: $name"
	elif git -C "$SDK" apply --check "$p" 2>/dev/null; then git -C "$SDK" apply "$p" && echo "patch applied: $name"
	else echo "patch does not apply to $SDK: $name" >&2; exit 1; fi
done
# AVDK-side changes: overlay files (new sources) + patches (edits to existing files)
cp -R "$HERE/avdk-overlay/." "$AVDK/"
for p in "$HERE"/patches-avdk/*.patch; do
	[ -e "$p" ] || continue
	name=$(basename "$p")
	if git -C "$AVDK" apply --reverse --check "$p" 2>/dev/null; then echo "avdk patch already applied: $name"
	elif git -C "$AVDK" apply --check "$p" 2>/dev/null; then git -C "$AVDK" apply "$p" && echo "avdk patch applied: $name"
	else echo "avdk patch does not apply: $name" >&2; exit 1; fi
done
# macOS/Windows podman runs the containers in a VM ("machine"); Linux podman has none
if [ "$RUNTIME" = podman ] && [ -n "$(podman machine list --format '{{.Name}}' 2>/dev/null)" ]; then
	podman machine inspect --format '{{.State}}' 2>/dev/null | grep -q running || podman machine start
fi
mkdir -p "$OUT" "$FW_DIR"
start=$(date +%s)
"$RUNTIME" run --platform linux/amd64 --rm \
	-v "$AVDK":/avdk \
	-v "$SDK":/avdk/bk_idk \
	-v "$HERE/project-avdk":/avdk/projects/doorbell \
	-v "$HERE/project/main":/avdk/projects/doorbell/shared \
	-v "$OUT":/avdk/build \
	-w /avdk -u "$(id -u)" \
	"$IMAGE" make bk7258 PROJECT=doorbell
echo "build took $(( $(date +%s) - start ))s"
ver=$(sed -nE 's/^#define FW_VERSION "([^"]+)"/\1/p' "$HERE/project/main/version.h")
cp "$OUT/doorbell/bk7258/all-app.bin" "$FW_DIR/doorbell-fw_${ver}_avdk_all-app.bin"
cp "$OUT/doorbell/bk7258/encrypt/app_pack.rbl" "$FW_DIR/doorbell-fw_${ver}_app_pack.rbl"
ln -sf "doorbell-fw_${ver}_app_pack.rbl" "$FW_DIR/doorbell-fw_latest_app_pack.rbl"
echo "ota image: $FW_DIR/doorbell-fw_${ver}_app_pack.rbl"
