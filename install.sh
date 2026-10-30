#!/usr/bin/env bash
# One-shot installer for sysdvr-lite.
#
# SUPPORTED ONLY: Orange Pi Zero 2W running Armbian (Debian based).
# It installs the build dependencies, raises the CMA pool to 128 MB, builds and installs the
# client and enables it as a systemd service that starts at boot and waits for the Switch.
#
# Usage: sudo ./install.sh [--audio-device ALSA_DEVICE] [--decoder v4l2|ffmpeg] [--no-cma] [--yes]
set -euo pipefail

AUDIO_DEVICE="plughw:CARD=HDMI,DEV=0"
DECODER="v4l2"
SET_CMA=1
ASSUME_YES=0
PREFIX=/usr/local
ENV_FILE="${SYSDVR_ENV_FILE:-/boot/armbianEnv.txt}"   # override only for testing

while [ $# -gt 0 ]; do
    case "$1" in
        --audio-device) AUDIO_DEVICE="${2:?missing value}"; shift 2 ;;
        --decoder)      DECODER="${2:?missing value}"; shift 2 ;;
        --no-cma)       SET_CMA=0; shift ;;
        --yes|-y)       ASSUME_YES=1; shift ;;
        -h|--help)      sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

die()  { echo "error: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }

[ "$(id -u)" -eq 0 ] || die "run as root: sudo ./install.sh"
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
    echo "This will install packages, edit $ENV_FILE (CMA) and enable the sysdvr-lite service."
    read -r -p "Continue? [y/N] " ans
    case "$ans" in y|Y|yes) ;; *) echo "aborted"; exit 1 ;; esac
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---- packages -------------------------------------------------------------------------------
step "Installing packages (needs internet access)"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config v4l-utils alsa-utils \
    libusb-1.0-0-dev libavcodec-dev libavutil-dev libasound2-dev \
    libdrm-dev libgbm-dev libegl-dev libgles-dev libgl1-mesa-dri libegl-mesa0

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
echo "Logs:      journalctl -u sysdvr-lite -f"
if [ "$REBOOT_NEEDED" -eq 1 ]; then
    echo
    echo "The CMA size changed: reboot now to apply it:  sudo reboot"
else
    echo "Plug in the Switch (SysDVR in USB mode), the picture should appear on the HDMI display."
fi
