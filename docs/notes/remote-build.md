# Remote builder

`scripts/build-remote.sh` runs the normal build on another machine: it rsyncs the working tree
(without `.git`, build outputs and simulator caches) to `REMOTE_BUILD_HOST`, runs
`scripts/build-avdk.sh` there (or `scripts/build.sh` with `--bare`) with the `REMOTE_*` paths as
that script's `SDK`/`AVDK`/`OUT`/`FW_DIR`, and copies `doorbell-fw_<ver>_app_pack.rbl` and
`doorbell-fw_<ver>_avdk_all-app.bin` back into the local `FW_DIR`, refreshing
`doorbell-fw_latest_app_pack.rbl`. The remote runs the *same* script, so the SDK/AVDK patches are
applied there the same idempotent way and the images are named the same; `scripts/release.sh`
accepts `BUILD=build-remote.sh`.

Why: the `bekencorp/armino-idk:1.5` image is x86-64 only. On an Apple Silicon laptop podman runs
it through Rosetta, a full AVDK build takes ~11 min and an incremental one ~100 s. On a modest
x86-64 desktop the same build is native. Measured on the author's remote (below, an i7-9700, 8
cores, Docker Desktop's engine in WSL2): full build **41 s**, incremental (touch
`project/main/app_main.c`) **15 s**, round trip from the laptop included; the bare flavour (`--bare`)
builds from scratch in 48 s.

The remote needs: Linux (a WSL2 distro counts), docker or podman usable by the login user, git,
rsync, ssh key login, clones of bk_idk and bk_avdk at `release/v2.0.1`, the image pulled. Nothing
from this repository is installed there: each run mirrors the working tree, so the remote always
builds exactly what the laptop has, committed or not. The firmware repo itself is never cloned on
the remote.

## Settings

In `~/.config/doorbell-bk7258/env` (see `scripts/env.example`):

| Key | Meaning | Default |
|---|---|---|
| `REMOTE_BUILD_HOST` | `user@host` (required) | - |
| `REMOTE_BUILD_PORT` | ssh port | `22` |
| `REMOTE_REPO_DIR` | where the tree is mirrored | `reps/home/iot/doorbell-bk7258-fw` |
| `REMOTE_SDK`, `REMOTE_AVDK` | bk_idk / bk_avdk clones on the remote | `reps/oss/bk_idk`, `reps/oss/bk_avdk` |
| `REMOTE_OUT` | build tree on the remote | `data/doorbell-bk7258/avdk-build` (`data/bk_idk-build` with `--bare`) |
| `REMOTE_FW_DIR` | where the remote script leaves its images | `data/doorbell-bk7258/firmware` |
| `REMOTE_WAKE` | local shell command run once when the ssh port does not answer, then retried for 60 s | - |

Relative paths are under the remote `$HOME`. Flags: `--bare`, `--no-sync-back`, `--shell` (login
shell on the remote in the mirrored repo, for poking at `$REMOTE_OUT` or the SDK). The script
multiplexes its ssh connections (`ControlMaster`, socket in `/tmp`), so a run costs one key
exchange.

## The author's remote: Windows 11 desktop, WSL2 Ubuntu-24.04 (set up 2026-10-04)

An always-on Windows 11 Home PC on the LAN (i7-9700, 32 GB, 1.9 TB SSD) that already ran Ollama,
Docker Desktop 4.88 and WSL 2.9 with two Ubuntu distros, reachable through Windows' own OpenSSH
server (`ssh user@pc`, key auth, login shell `cmd.exe`). Nothing was rebooted and Ollama, Docker
Desktop and the owner's distros were left as they were. What was done, in order:

1. **Pick the distro.** `wsl -l -v` listed `Ubuntu-20.04` (default, the owner's conda/CUDA
   environment), `Ubuntu-24.04` (systemd on, 945 GB free, Docker Desktop WSL integration already
   providing a linux/amd64 `docker` CLI) and `docker-desktop`. Ubuntu-24.04 was chosen; the older
   one was not touched.
2. **User.** Inside Ubuntu-24.04 (as root through `wsl -d Ubuntu-24.04 -u root`): `useradd -m -s
   /bin/bash -u 1000 du7`, groups `sudo` and `docker`, `/etc/sudoers.d/du7` with `NOPASSWD`, the
   laptop's `~/.ssh/id_ed25519.pub` in `/home/du7/.ssh/authorized_keys`, and `[user]
   default=du7` appended to `/etc/wsl.conf`. Docker Desktop's integration socket
   (`/var/run/docker.sock`, group `docker`) then works for `du7` without anything else installed:
   `docker info` reports the Desktop engine, 8 CPUs, 16 GB (the VM's share).
3. **sshd on port 2222.** `apt-get install openssh-server rsync git`. The `.wslconfig` on the
   Windows side already had `networkingMode=mirrored`, which means WSL shares the PC's IP *and
   port space*: port 22 is Windows' sshd, so Ubuntu's socket-activated `ssh.socket` fails to bind
   and the package postinst aborts. Fix: drop-in `/etc/ssh/sshd_config.d/60-wsl-2222.conf`
   (`Port 2222`, `PasswordAuthentication no`, `KbdInteractiveAuthentication no`, `PermitRootLogin
   no`), `systemctl mask ssh.socket`, `dpkg --configure -a`, `systemctl enable --now ssh.service`.
   The ssh target is therefore the PC's own address, port 2222: `ssh -p 2222 du7@<pc-ip>` lands in
   WSL, port 22 lands in Windows. No `netsh portproxy` is involved.
4. **Firewall.** With mirrored networking, inbound connections to WSL are filtered by the Hyper-V
   firewall (default inbound block for the WSL VM creator); the PC already had such rules for two
   other WSL services, so one more was added from an elevated PowerShell (the ssh session of an
   administrator account is elevated):
   `New-NetFirewallHyperVRule -Name WSL-sshd-2222 -DisplayName "WSL sshd 2222 (doorbell build host)"
   -Direction Inbound -VMCreatorId "{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}" -Protocol TCP
   -LocalPorts 2222 -Action Allow`, plus a plain host rule `New-NetFirewallRule -Name
   WSL-sshd-2222-host ... -LocalPort 2222 -Action Allow -Profile Any` (the LAN is a "Public"
   profile there). Through `cmd.exe` the GUID braces get eaten; pass PowerShell as
   `-EncodedCommand` (UTF-16LE base64) when scripting it over ssh.
5. **Keeping the distro up.** WSL stops a distro a few seconds after its last Windows-started
   process exits, systemd or not, and sshd goes with it. The owner already had a logon-triggered
   scheduled task for this distro (`du7-wsl-ml-logon`: `wsl.exe -d Ubuntu-24.04 -u root -e sleep
   infinity`, run as the interactive user), which is exactly the keep-alive needed: with
   `ssh.service` enabled, sshd comes up whenever the distro does, so a reboot plus logon restores
   the builder. The task was found idle (the distro had been shut down since) and started with
   `schtasks /run /tn du7-wsl-ml-logon`. If port 2222 ever stops answering, that command through
   the Windows sshd brings it back, which is what `REMOTE_WAKE` is for:
   `REMOTE_WAKE="ssh du7@<pc-ip> schtasks /run /tn du7-wsl-ml-logon"`. Equivalent on a PC without
   such a task: `schtasks /create /tn wsl-keepalive /sc onlogon /rl highest /tr "C:\Windows\System32\wsl.exe -d Ubuntu-24.04 -u root -e sleep infinity"`.
6. **SDKs and image**, as `du7` over the new port: `git clone --depth 1 --branch release/v2.0.1
   https://github.com/bekencorp/bk_idk` and `.../bk_avdk` under `~/reps/oss/` (HEADs identical to
   the laptop's clones: bk_idk `650e754e`, bk_avdk `6a800053`), `docker pull
   docker.io/bekencorp/armino-idk:1.5`, `mkdir -p ~/data/doorbell-bk7258/{avdk-build,firmware}`.
7. **Laptop side.** `REMOTE_BUILD_HOST=du7@<pc-ip>`, `REMOTE_BUILD_PORT=2222`, `REMOTE_WAKE` as
   above in `~/.config/doorbell-bk7258/env`; the other `REMOTE_*` keys are at their defaults.
   `scripts/build-remote.sh` then does everything; `BUILD=build-remote.sh scripts/release.sh`
   ships the result.

## What to expect and what is fragile

- The first run mirrors ~120 files and applies all patches to the remote SDK; later runs sync only
  what changed and report "patch already applied" like a local build.
- The remote's `git -C $REMOTE_SDK diff` shows the applied patch set, the same as locally.
- Docker Desktop must be running in the Windows user session: the `docker` CLI inside
  Ubuntu-24.04 is Desktop's integration, there is no daemon of its own. The PC starts Desktop at
  logon (the owner's `dockerstart` task). `docker-ce` inside the distro would remove that
  dependency at the cost of a second daemon; not done.
- The distro must be running (point 5). `REMOTE_WAKE` covers the common case; a Windows reboot
  without an interactive logon leaves nothing to wake.
- Mirrored networking is what makes port 2222 reachable without port forwarding; if
  `.wslconfig` ever loses `networkingMode=mirrored`, use `netsh interface portproxy add v4tov4
  listenport=2222 connectaddress=<wsl-ip> connectport=2222` instead (the WSL IP changes per boot).
- `scripts/build.sh` names its bare image after the git revision; the remote has no `.git`, so
  `build-remote.sh` passes the laptop's revision as `GIT_REV`.
- The container runs as the remote uid (`-u 1000`); the build and firmware directories on the
  remote belong to `du7`, like `$OUT` belongs to you locally.
