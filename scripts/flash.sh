#!/usr/bin/env bash
# Flash an image to the doorbell over the BootROM (UART0 header next to the battery).
# Dry run unless --write is passed. Press the doorbell button when asked.
#   scripts/flash.sh                       # dry run with the latest build
#   scripts/flash.sh --write               # flash the latest build
#   scripts/flash.sh IMAGE --write         # flash a specific image
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
FW_DIR=${FW_DIR:-$HOME/data/doorbell-bk7258/firmware}

img="$FW_DIR/doorbell-fw_latest.bin"
if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then
	img=$1; shift
fi

cd "$HERE/tools"
exec python3 bkwrite.py "$img" --start 0x0 "$@"
