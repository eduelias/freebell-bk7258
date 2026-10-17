#!/usr/bin/env bash
# Build freebell-bk7258 with Beken's armino-idk container (podman or docker; x86-64, emulated
# through Rosetta on Apple Silicon, native on an x86-64 Linux host, see scripts/build-remote.sh).
# Our project is mounted over the SDK's projects/app slot so the stock build flow applies.
# Settings (SDK, OUT, FW_DIR) come from the environment or ~/.config/doorbell-bk7258/env
# (see scripts/env.example). This is the camera-less flavour; scripts/build-avdk.sh is the
# one the device runs.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
. "$HERE/scripts/env.sh"
SDK=${SDK:-$HOME/reps/oss/bk_idk}
OUT=${OUT:-$HOME/data/bk_idk-build}
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
	if git -C "$SDK" apply --reverse --check "$p" 2>/dev/null; then
		echo "patch already applied: $name"
	elif git -C "$SDK" apply --check "$p" 2>/dev/null; then
		git -C "$SDK" apply "$p" && echo "patch applied: $name"
	else
		echo "patch does not apply to $SDK: $name" >&2
		exit 1
	fi
done

# macOS/Windows podman runs the containers in a VM ("machine"); Linux podman has none
if [ "$RUNTIME" = podman ] && [ -n "$(podman machine list --format '{{.Name}}' 2>/dev/null)" ]; then
	podman machine inspect --format '{{.State}}' 2>/dev/null | grep -q running || podman machine start
fi

mkdir -p "$OUT" "$FW_DIR"
start=$(date +%s)
"$RUNTIME" run --platform linux/amd64 --rm \
	-v "$SDK":/armino \
	-v "$HERE/project":/armino/projects/app \
	-v "$OUT":/armino/build \
	-w /armino -u "$(id -u)" \
	"$IMAGE" make bk7258 PROJECT=app
echo "build took $(( $(date +%s) - start ))s"

stamp=$(date +%Y%m%d-%H%M)
rev=${GIT_REV:-$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo nogit)}   # GIT_REV: set by build-remote.sh (no .git on the remote)
dst="$FW_DIR/doorbell-fw_${stamp}_${rev}.bin"
cp "$OUT/app/bk7258/all-app.bin" "$dst"
ln -sf "$(basename "$dst")" "$FW_DIR/doorbell-fw_latest.bin"
ver=$(sed -nE 's/^#define FW_VERSION "([^"]+)"/\1/p' "$HERE/project/main/version.h")
cp "$OUT/app/bk7258/encrypt/app_pack.rbl" "$FW_DIR/doorbell-fw_${ver}_app_pack.rbl"
ln -sf "doorbell-fw_${ver}_app_pack.rbl" "$FW_DIR/doorbell-fw_latest_app_pack.rbl"
echo "ota image: $FW_DIR/doorbell-fw_${ver}_app_pack.rbl"
echo "image: $dst ($(stat -c %s "$dst" 2>/dev/null || stat -f %z "$dst") bytes)"
