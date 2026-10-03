#!/usr/bin/env bash
# Build, publish the OTA image to Home Assistant, and update the doorbell over Wi-Fi.
#   scripts/release.sh            # build + publish + OTA
#   scripts/release.sh --no-build # publish the current image + OTA
# HA serves /config/www/ at http://192.168.2.80:8123/local/ ; the HA box is reachable only from
# du7, so the image goes Mac -> du7 -> HA. The OTA is triggered over the TCP command channel
# (tools/dbcmd.py), which needs the token stored once with `tok set` over serial.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
FW_DIR=${FW_DIR:-$HOME/data/doorbell-bk7258/firmware}
HA_URL_BASE=${HA_URL_BASE:-http://192.168.2.80:8123/local/doorbell}
DU7=${DU7:-esaleh@192.168.2.68}
HA=${HA:-root@192.168.2.80}

[ "${1:-}" = "--no-build" ] || "$HERE/scripts/build.sh"

ver=$(sed -nE 's/^#define FW_VERSION "([^"]+)"/\1/p' "$HERE/project/main/app_main.c")
rbl="$FW_DIR/doorbell-fw_${ver}_app_pack.rbl"
[ -s "$rbl" ] || { echo "missing $rbl" >&2; exit 1; }
name=$(basename "$rbl")

echo "publishing $name to HA ..."
scp -q "$rbl" "$DU7:/tmp/$name"
ssh "$DU7" "ssh $HA 'mkdir -p /config/www/doorbell' && scp -q /tmp/$name $HA:/config/www/doorbell/ && rm -f /tmp/$name"
curl -sf -o /dev/null "$HA_URL_BASE/$name" && echo "served at $HA_URL_BASE/$name"

echo "triggering OTA on the doorbell ..."
python3 "$HERE/tools/dbcmd.py" ota "$HA_URL_BASE/$name"
echo "download takes ~30 s, then the device reboots into $ver; check with: python3 tools/dbcmd.py hook test"
