#!/usr/bin/env bash
# One-shot installer for sysdvr-lite.
#
# SUPPORTED ONLY: Orange Pi Zero 2W running Armbian (Debian based).
# It syncs the clock if needed, installs git and the build dependencies, clones the repository
# (when run through curl), raises the CMA pool to 128 MB, builds and installs the client and
# enables it as a systemd service that starts at boot and waits for the Switch.
#
# From a checkout:  sudo ./install.sh [options]
# Straight from the web, nothing downloaded beforehand:
#   curl -fsSL https://raw.githubusercontent.com/VLPLAY-Games/sysdvr-lite/main/install.sh | sudo bash -s -- [options]
#
# Options:
#   --audio-device ALSA_DEVICE   default plughw:CARD=HDMI,DEV=0
#   --decoder v4l2|ffmpeg        default v4l2 (hardware)
#   --dir PATH                   where the repository is cloned when run through curl (default /opt/sysdvr-lite)
#   --no-cma                     do not touch the CMA setting
#   --yes, -y                    do not ask for confirmation (required when there is no terminal)

# The whole script is one function: bash must read it completely before running anything, otherwise
# commands that read stdin would swallow the rest of the script when it is piped from curl.
main() {
set -euo pipefail

AUDIO_DEVICE="plughw:CARD=HDMI,DEV=0"
DECODER="v4l2"
SET_CMA=1
ASSUME_YES=0
PREFIX=/usr/local
REPO_URL="${SYSDVR_REPO:-https://github.com/VLPLAY-Games/sysdvr-lite}"
INSTALL_DIR=/opt/sysdvr-lite
ENV_FILE="${SYSDVR_ENV_FILE:-/boot/armbianEnv.txt}"   # override only for testing
APT_OPTS=()

die()  { echo "error: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }
usage() {
    if [ -f "${BASH_SOURCE[0]:-}" ]; then sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    else echo "see the header of install.sh or the README: $REPO_URL"; fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --audio-device) AUDIO_DEVICE="${2:?missing value}"; shift 2 ;;
        --decoder)      DECODER="${2:?missing value}"; shift 2 ;;
        --dir)          INSTALL_DIR="${2:?missing value}"; shift 2 ;;
        --no-cma)       SET_CMA=0; shift ;;
        --yes|-y)       ASSUME_YES=1; shift ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || die "run as root (sudo)"
[ "$DECODER" = v4l2 ] || [ "$DECODER" = ffmpeg ] || die "--decoder must be v4l2 or ffmpeg"

# ---- platform check -------------------------------------------------------------------------
step "Checking the platform"
MODEL="$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || true)"
case "$MODEL" in
    *"Orange Pi Zero 2W"*|*"OrangePi Zero 2W"*|*"Orange Pi Zero2W"*) ;;
    *) die "this installer only supports the Orange Pi Zero 2W (detected: '${MODEL:-unknown}')" ;;
esac
[ -f /etc/armbian-release ] || [ -f /boot/armbianEnv.txt ] || die "this installer only supports Armbian"
. /etc/os-release
[ "${ID:-}" = debian ] || [ "${ID_LIKE:-}" = debian ] || die "this installer only supports Debian based Armbian images (found ${PRETTY_NAME:-unknown})"
[ "$(uname -m)" = aarch64 ] || die "expected an arm64 system"
echo "Board: $MODEL / ${PRETTY_NAME:-} / kernel $(uname -r)"

if [ "$ASSUME_YES" -ne 1 ]; then
    echo "This will sync the clock, install packages, edit $ENV_FILE (CMA) and enable the sysdvr-lite service."
    # when piped from curl stdin is the script, so the answer is read from the terminal
    { : </dev/tty; } 2>/dev/null || die "no terminal to ask on, run again with --yes"
    read -r -p "Continue? [y/N] " ans </dev/tty || ans=""
    case "$ans" in y|Y|yes) ;; *) echo "aborted"; exit 1 ;; esac
fi

# ---- clock ----------------------------------------------------------------------------------
# The board has no battery clock: after a power loss the date is wrong and TLS certificates and
# apt repository metadata are rejected as "not yet valid".
clock_synced() { [ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null || true)" = yes ]; }

step "Checking the system clock"
if clock_synced; then
    echo "synchronized: $(date)"
else
    echo "not synchronized ($(date)), enabling network time"
    if ! timedatectl set-ntp true 2>/dev/null; then
        # no time service installed: apt cannot be trusted with the wrong date, so skip its date check once
        DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Check-Date=false -o Acquire::Check-Valid-Until=false update || true
        DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Check-Date=false -y --no-install-recommends install systemd-timesyncd || true
        timedatectl set-ntp true 2>/dev/null || true
    fi
    for _ in $(seq 30); do clock_synced && break; sleep 2; done
    if clock_synced; then
        echo "synchronized: $(date)"
    else
        echo "warning: the clock is still not synchronized (no internet?), continuing with the date check of apt disabled" >&2
        APT_OPTS=(-o Acquire::Check-Date=false -o Acquire::Check-Valid-Until=false)
    fi
fi

# ---- source ---------------------------------------------------------------------------------
# Run from a checkout: build that checkout. Run through curl / process substitution: clone it.
HERE=""
SELF="${BASH_SOURCE[0]:-}"
if [ -n "$SELF" ] && [ -f "$SELF" ]; then
    SELF_DIR="$(cd "$(dirname "$SELF")" && pwd)"
    if [ -f "$SELF_DIR/CMakeLists.txt" ] && [ -f "$SELF_DIR/packages.txt" ]; then HERE="$SELF_DIR"; fi
fi

step "Updating the package lists (needs internet access)"
export DEBIAN_FRONTEND=noninteractive
apt-get "${APT_OPTS[@]}" update

if [ -z "$HERE" ]; then
    if ! command -v git >/dev/null; then
        step "Installing git"
        apt-get "${APT_OPTS[@]}" install -y --no-install-recommends git ca-certificates
    fi
    step "Getting the source into $INSTALL_DIR"
    if [ -d "$INSTALL_DIR/.git" ]; then
        git -C "$INSTALL_DIR" pull --ff-only
    elif [ -e "$INSTALL_DIR" ] && [ -n "$(ls -A "$INSTALL_DIR" 2>/dev/null)" ]; then
        die "$INSTALL_DIR exists and is not a sysdvr-lite checkout, pick another place with --dir"
    else
        git clone --depth 1 "$REPO_URL" "$INSTALL_DIR"
    fi
    HERE="$INSTALL_DIR"
fi
echo "Source: $HERE ($(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo 'not a git checkout'))"

# ---- packages -------------------------------------------------------------------------------
step "Installing packages"
[ -f "$HERE/packages.txt" ] || die "$HERE/packages.txt not found: this is not a complete sysdvr-lite checkout"
mapfile -t PACKAGES < <(grep -vE '^[[:space:]]*(#|$)' "$HERE/packages.txt")
[ "${#PACKAGES[@]}" -gt 0 ] || die "$HERE/packages.txt lists no packages"
apt-get "${APT_OPTS[@]}" install -y --no-install-recommends "${PACKAGES[@]}"

# ---- CMA ------------------------------------------------------------------------------------
REBOOT_NEEDED=0
if [ "$SET_CMA" -eq 1 ]; then
    step "Setting the CMA pool to 128 MB"
    [ -f "$ENV_FILE" ] || die "$ENV_FILE not found"
    if grep -Eq '^extraargs=.*\bcma=' "$ENV_FILE"; then
        if grep -Eq '^extraargs=.*\bcma=128M\b' "$ENV_FILE"; then
            echo "already set"
        else
            [ -f "$ENV_FILE.sysdvr-lite.bak" ] || cp "$ENV_FILE" "$ENV_FILE.sysdvr-lite.bak"
            sed -i -E '/^extraargs=/ s/\bcma=[0-9]+[MmKkGg]?/cma=128M/' "$ENV_FILE"
            REBOOT_NEEDED=1
        fi
    else
        [ -f "$ENV_FILE.sysdvr-lite.bak" ] || cp "$ENV_FILE" "$ENV_FILE.sysdvr-lite.bak"
        if grep -q '^extraargs=' "$ENV_FILE"; then
            sed -i '/^extraargs=/ s/$/ cma=128M/' "$ENV_FILE"
        else
            echo 'extraargs=cma=128M' >> "$ENV_FILE"
        fi
        REBOOT_NEEDED=1
    fi
    [ ! -f "$ENV_FILE.sysdvr-lite.bak" ] || echo "backup: $ENV_FILE.sysdvr-lite.bak"
fi

# ---- build and install ----------------------------------------------------------------------
step "Building"
BUILD_DIR="$HERE/build"
cmake -S "$HERE" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"
cmake --install "$BUILD_DIR" --prefix "$PREFIX"

# ---- service --------------------------------------------------------------------------------
step "Installing the systemd service"
EXEC="$PREFIX/bin/sysdvr-lite --decoder $DECODER --drm --audio-device $AUDIO_DEVICE"
cat > /etc/systemd/system/sysdvr-lite.service <<UNIT
[Unit]
Description=SysDVR lite client (Switch over USB -> HDMI)
After=systemd-udevd.service sound.target
Wants=sound.target

[Service]
ExecStart=$EXEC
Restart=always
RestartSec=2
Nice=-5

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload
systemctl enable sysdvr-lite.service
if [ "$REBOOT_NEEDED" -eq 0 ]; then
    systemctl restart sysdvr-lite.service
fi

# ---- summary --------------------------------------------------------------------------------
step "Done"
[ -e /dev/video0 ] || echo "warning: /dev/video0 not found, the hardware decoder (cedrus) is not available; use --decoder ffmpeg"
echo "Service:   sysdvr-lite (enabled, starts at boot)"
echo "Command:   $EXEC"
echo "Source:    $HERE   (update later with: sudo $HERE/update.sh)"
echo "Logs:      journalctl -u sysdvr-lite -f"
if [ "$REBOOT_NEEDED" -eq 1 ]; then
    echo
    echo "The CMA size changed: reboot now to apply it:  sudo reboot"
else
    echo "Plug in the Switch (SysDVR in USB mode), the picture should appear on the HDMI display."
fi
}

main "$@"
