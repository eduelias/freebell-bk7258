#!/usr/bin/env bash
# Build doorbell-bk7258-fw with Beken's armino-idk container (podman, x86-64 via Rosetta).
# Our project is mounted over the SDK's projects/app slot so the stock build flow applies.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
SDK=${SDK:-$HOME/reps/oss/bk_idk}
OUT=${OUT:-$HOME/data/bk_idk-build}
FW_DIR=${FW_DIR:-$HOME/data/doorbell-bk7258/firmware}
IMAGE=docker.io/bekencorp/armino-idk:1.5

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

podman machine inspect --format '{{.State}}' 2>/dev/null | grep -q running || podman machine start

mkdir -p "$OUT" "$FW_DIR"
start=$(date +%s)
podman run --platform linux/amd64 --rm \
	-v "$SDK":/armino \
	-v "$HERE/project":/armino/projects/app \
	-v "$OUT":/armino/build \
	-w /armino -u "$(id -u)" \
	"$IMAGE" make bk7258 PROJECT=app
echo "build took $(( $(date +%s) - start ))s"

stamp=$(date +%Y%m%d-%H%M)
rev=$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo nogit)
dst="$FW_DIR/doorbell-fw_${stamp}_${rev}.bin"
cp "$OUT/app/bk7258/all-app.bin" "$dst"
ln -sf "$(basename "$dst")" "$FW_DIR/doorbell-fw_latest.bin"
ver=$(sed -nE 's/^#define FW_VERSION "([^"]+)"/\1/p' "$HERE/project/main/app_main.c")
cp "$OUT/app/bk7258/encrypt/app_pack.rbl" "$FW_DIR/doorbell-fw_${ver}_app_pack.rbl"
ln -sf "doorbell-fw_${ver}_app_pack.rbl" "$FW_DIR/doorbell-fw_latest_app_pack.rbl"
echo "ota image: $FW_DIR/doorbell-fw_${ver}_app_pack.rbl"
echo "image: $dst ($(stat -f %z "$dst") bytes)"
