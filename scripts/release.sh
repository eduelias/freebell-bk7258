#!/usr/bin/env bash
# Build, publish the OTA image where the device can fetch it, and update the doorbell over Wi-Fi.
#   scripts/release.sh             # build + publish + OTA
#   scripts/release.sh --no-build  # publish the current image + OTA
#   scripts/release.sh --dry-run   # print what would run (build script, image, publish command,
#                                  # OTA URL) and exit before any copy; combine with --no-build
# Settings come from the environment or ~/.config/doorbell-bk7258/env (scripts/env.example):
#   BUILD         build-avdk.sh (default, camera flavour) or build.sh
#   FW_DIR        where the build put doorbell-fw_<ver>_app_pack.rbl
#   OTA_URL_BASE  HTTP base URL the device fetches from, e.g. http://<ha-ip>:8123/local/doorbell
#   OTA_PUBLISH   shell command that copies the image there; {rbl} = full path, {name} = basename,
#                 e.g. "scp {rbl} user@ha:/config/www/doorbell/"
#   DOORBELL_HOST / DOORBELL_TOKEN[_FILE]  for the `ota` command sent by tools/dbcmd.py
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
. "$HERE/scripts/env.sh"
FW_DIR=${FW_DIR:-$HOME/data/doorbell-bk7258/firmware}
BUILD=${BUILD:-build-avdk.sh}
OTA_URL_BASE=${OTA_URL_BASE:-}
OTA_PUBLISH=${OTA_PUBLISH:-}

build=1 dry=0
for arg in "$@"; do
	case "$arg" in
		--no-build) build=0 ;;
		--dry-run) dry=1 ;;
		-h|--help) sed -n '2,15p' "$0"; exit 0 ;;
		*) echo "unknown option: $arg (use --no-build, --dry-run)" >&2; exit 2 ;;
	esac
done

case "$BUILD" in
	build.sh|build-avdk.sh) ;;
	*) echo "BUILD must be build-avdk.sh or build.sh (got '$BUILD')" >&2; exit 2 ;;
esac

if [ $build = 1 ] && [ $dry = 0 ]; then
	"$HERE/scripts/$BUILD"
fi

ver=$(sed -nE 's/^#define FW_VERSION "([^"]+)"/\1/p' "$HERE/project/main/app_main.c")
rbl="$FW_DIR/doorbell-fw_${ver}_app_pack.rbl"
name=$(basename "$rbl")

if [ -z "$OTA_URL_BASE" ] || [ -z "$OTA_PUBLISH" ]; then
	cat >&2 <<MSG
release.sh needs two settings (environment or ~/.config/doorbell-bk7258/env, see scripts/env.example):
  OTA_URL_BASE  the HTTP base URL the device downloads from, e.g. http://<ha-ip>:8123/local/doorbell
  OTA_PUBLISH   a shell command that copies the image to where that URL is served; {rbl} is replaced
                by the image path and {name} by its file name, e.g. "scp {rbl} user@ha:/config/www/doorbell/"
MSG
	exit 2
fi

publish=${OTA_PUBLISH//\{rbl\}/$(printf %q "$rbl")}
publish=${publish//\{name\}/$name}
url="${OTA_URL_BASE%/}/$name"

if [ $dry = 1 ]; then
	echo "dry run: nothing is copied or sent"
	echo "  build:    $([ $build = 1 ] && echo "scripts/$BUILD" || echo "skipped (--no-build)")"
	echo "  image:    $rbl ($([ -s "$rbl" ] && echo "$(stat -f %z "$rbl" 2>/dev/null || stat -c %s "$rbl") bytes" || echo "missing"))"
	echo "  publish:  $publish"
	echo "  verify:   curl -sf -o /dev/null $url"
	echo "  trigger:  python3 tools/dbcmd.py ota $url"
	python3 "$HERE/tools/dbcmd.py" --dry-run ota "$url" 2>&1 | sed 's/^/            /' || true
	exit 0
fi

[ -s "$rbl" ] || { echo "missing $rbl (run without --no-build, or check FW_DIR)" >&2; exit 1; }

echo "publishing $name ..."
eval "$publish"
if curl -sf -o /dev/null "$url"; then
	echo "served at $url"
else
	echo "not served at $url after publishing: check OTA_URL_BASE and OTA_PUBLISH" >&2
	exit 1
fi

echo "triggering OTA on the doorbell ..."
python3 "$HERE/tools/dbcmd.py" ota "$url"
echo "download takes ~30 s, then the device reboots into $ver; check with: python3 tools/dbcmd.py hook test"
