#!/usr/bin/env bash
# Build on a remote x86-64 host instead of emulating the container locally: rsync this working
# tree there, run scripts/build-avdk.sh (or scripts/build.sh with --bare) on the remote with the
# REMOTE_* paths as its SDK/AVDK/OUT/FW_DIR, then copy the images back into the local FW_DIR and
# refresh doorbell-fw_latest_app_pack.rbl exactly like a local build would.
#   scripts/build-remote.sh                 # AVDK (camera) flavour, same as scripts/build-avdk.sh
#   scripts/build-remote.sh --bare          # bare SDK flavour, same as scripts/build.sh
#   scripts/build-remote.sh --no-sync-back  # build only, leave the images on the remote
#   scripts/build-remote.sh --shell         # interactive shell on the remote, in the synced repo
# Settings come from the environment or ~/.config/doorbell-bk7258/env (scripts/env.example):
#   REMOTE_BUILD_HOST  user@host of the Linux build host (required; podman or docker installed,
#                      bk_idk + bk_avdk cloned at release/v2.0.1, see docs/notes/remote-build.md)
#   REMOTE_BUILD_PORT  ssh port (default 22)
#   REMOTE_REPO_DIR    where the working tree is mirrored (default reps/home/iot/doorbell-bk7258-fw)
#   REMOTE_SDK / REMOTE_AVDK / REMOTE_OUT / REMOTE_FW_DIR
#                      the remote SDK, AVDK, build and image directories (defaults mirror the local
#                      ones: reps/oss/bk_idk, reps/oss/bk_avdk, data/doorbell-bk7258/avdk-build,
#                      data/doorbell-bk7258/firmware); relative paths are under the remote $HOME
#   REMOTE_WAKE        optional local shell command run once when the remote port does not answer
#                      (e.g. start a WSL distro through the Windows side), then retried for 60 s
#   FW_DIR             local directory the images are copied back to
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
. "$HERE/scripts/env.sh"
REMOTE_BUILD_HOST=${REMOTE_BUILD_HOST:-}
REMOTE_BUILD_PORT=${REMOTE_BUILD_PORT:-22}
REMOTE_REPO_DIR=${REMOTE_REPO_DIR:-reps/home/iot/doorbell-bk7258-fw}
REMOTE_SDK=${REMOTE_SDK:-reps/oss/bk_idk}
REMOTE_AVDK=${REMOTE_AVDK:-reps/oss/bk_avdk}
REMOTE_OUT=${REMOTE_OUT:-}
REMOTE_FW_DIR=${REMOTE_FW_DIR:-data/doorbell-bk7258/firmware}
REMOTE_WAKE=${REMOTE_WAKE:-}
FW_DIR=${FW_DIR:-$HOME/data/doorbell-bk7258/firmware}

bare=0 sync_back=1 shell=0
for arg in "$@"; do
	case "$arg" in
		--bare) bare=1 ;;
		--no-sync-back) sync_back=0 ;;
		--shell) shell=1 ;;
		-h|--help) sed -n '2,22p' "$0"; exit 0 ;;
		*) echo "unknown option: $arg (use --bare, --no-sync-back, --shell)" >&2; exit 2 ;;
	esac
done

[ -n "$REMOTE_BUILD_HOST" ] || {
	echo "REMOTE_BUILD_HOST is not set (environment or ~/.config/doorbell-bk7258/env, see scripts/env.example)" >&2
	exit 2
}
if [ $bare = 1 ]; then
	REMOTE_OUT=${REMOTE_OUT:-data/bk_idk-build}
	script=build.sh
else
	REMOTE_OUT=${REMOTE_OUT:-data/doorbell-bk7258/avdk-build}
	script=build-avdk.sh
fi
# Remote paths are pasted into a remote shell line: keep them to plain path characters. A leading
# ~ or a relative path is resolved against the remote $HOME; rsync wants the path without ~.
for v in REMOTE_REPO_DIR REMOTE_SDK REMOTE_AVDK REMOTE_OUT REMOTE_FW_DIR; do
	case "${!v}" in
		*[!A-Za-z0-9_./~-]*|'') echo "$v='${!v}' must be a plain path (no spaces or shell characters)" >&2; exit 2 ;;
	esac
done
rpath() {	# path for rsync (relative to remote $HOME when not absolute)
	case "$1" in /*) printf %s "$1" ;; '~/'*) printf %s "${1#\~/}" ;; '~') printf %s "" ;; *) printf %s "$1" ;; esac
}
spath() {	# path for the remote shell ($HOME-relative made explicit)
	case "$1" in /*) printf %s "$1" ;; '~'*) printf '$HOME%s' "${1#\~}" ;; *) printf '$HOME/%s' "$1" ;; esac
}
repo_r=$(rpath "$REMOTE_REPO_DIR") repo_s=$(spath "$REMOTE_REPO_DIR")
fw_r=$(rpath "$REMOTE_FW_DIR")

control="/tmp/doorbell-remote-%C"   # short: macOS $TMPDIR exceeds the socket path limit
sshopts=(-p "$REMOTE_BUILD_PORT" -o BatchMode=yes -o ConnectTimeout=10
	-o ControlMaster=auto -o ControlPath="$control" -o ControlPersist=120)
rssh() { ssh "${sshopts[@]}" "$REMOTE_BUILD_HOST" "$@"; }
rsync_e="ssh ${sshopts[*]}"

# 1. reach the host (wake it once if asked), make sure the repo dir exists
if ! rssh "mkdir -p $repo_s $(spath "$REMOTE_FW_DIR")" 2>/dev/null; then
	if [ -n "$REMOTE_WAKE" ]; then
		echo "remote $REMOTE_BUILD_HOST:$REMOTE_BUILD_PORT not answering, running REMOTE_WAKE ..."
		eval "$REMOTE_WAKE" || true
		for _ in $(seq 1 30); do
			sleep 2
			rssh "mkdir -p $repo_s $(spath "$REMOTE_FW_DIR")" 2>/dev/null && break
		done
	fi
	rssh "mkdir -p $repo_s $(spath "$REMOTE_FW_DIR")" || {
		echo "cannot reach $REMOTE_BUILD_HOST port $REMOTE_BUILD_PORT (ssh with key auth)" >&2; exit 1; }
fi

if [ $shell = 1 ]; then
	exec ssh -t "${sshopts[@]}" "$REMOTE_BUILD_HOST" "cd $repo_s && exec \${SHELL:-bash} -l"
fi

# 2. mirror the working tree (no .git: the build does not need it; no build outputs or sim caches)
start=$(date +%s)
changed=$(rsync -az --delete --out-format='%n' \
	--exclude .git --exclude 'wt-*/' --exclude build/ --exclude __pycache__/ --exclude .pytest_cache/ \
	--exclude .virtual_doorbell.json --exclude '*.bin' --exclude '*.rbl' --exclude '*.log' \
	--exclude '*.wav' --exclude '*.raw' --exclude '*.env' --exclude .DS_Store \
	-e "$rsync_e" "$HERE/" "$REMOTE_BUILD_HOST:$repo_r/" | grep -cv '/$' || true)
echo "synced to $REMOTE_BUILD_HOST:$REMOTE_REPO_DIR ($changed files changed)"

# 3. build there; the remote script applies the patches to the remote SDK/AVDK and names the images
rev=$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo nogit)
rssh "cd $repo_s && SDK=$(spath "$REMOTE_SDK") AVDK=$(spath "$REMOTE_AVDK") OUT=$(spath "$REMOTE_OUT") \
	FW_DIR=$(spath "$REMOTE_FW_DIR") GIT_REV=$rev scripts/$script"

# 4. bring the images home and point the latest symlink at them
ver=$(sed -nE 's/^#define FW_VERSION "([^"]+)"/\1/p' "$HERE/project/main/version.h")
if [ $sync_back = 1 ]; then
	mkdir -p "$FW_DIR"
	files=("doorbell-fw_${ver}_app_pack.rbl")
	if [ $bare = 1 ]; then
		latest=$(rssh "readlink $(spath "$REMOTE_FW_DIR")/doorbell-fw_latest.bin" || true)
		[ -n "$latest" ] && files+=("$latest")
	else
		files+=("doorbell-fw_${ver}_avdk_all-app.bin")
	fi
	for f in "${files[@]}"; do
		rsync -az -e "$rsync_e" "$REMOTE_BUILD_HOST:$fw_r/$f" "$FW_DIR/$f"
	done
	ln -sf "doorbell-fw_${ver}_app_pack.rbl" "$FW_DIR/doorbell-fw_latest_app_pack.rbl"
	[ $bare = 1 ] && [ -n "${latest:-}" ] && ln -sf "$latest" "$FW_DIR/doorbell-fw_latest.bin"
	echo "ota image: $FW_DIR/doorbell-fw_${ver}_app_pack.rbl ($(stat -c %s "$FW_DIR/doorbell-fw_${ver}_app_pack.rbl" 2>/dev/null || stat -f %z "$FW_DIR/doorbell-fw_${ver}_app_pack.rbl") bytes)"
else
	echo "images left on $REMOTE_BUILD_HOST:$REMOTE_FW_DIR (--no-sync-back)"
fi
echo "remote build round trip $(( $(date +%s) - start ))s"
