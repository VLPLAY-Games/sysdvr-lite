#!/usr/bin/env bash
# Updates sysdvr-lite on a board where install.sh has been run before:
# pulls the latest code, rebuilds, installs the new binary and restarts the service.
# It does not touch packages, the CMA setting or your service options (decoder, audio device).
#
# Usage: sudo ./update.sh [--no-pull]     (--no-pull: rebuild the current checkout only)
set -euo pipefail

PULL=1
PREFIX=/usr/local
UNIT=sysdvr-lite.service

while [ $# -gt 0 ]; do
    case "$1" in
        --no-pull) PULL=0; shift ;;
        -h|--help) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

die()  { echo "error: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }

[ "$(id -u)" -eq 0 ] || die "run as root: sudo ./update.sh"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
command -v cmake >/dev/null || die "cmake not found, run ./install.sh first"
[ -f "/etc/systemd/system/$UNIT" ] || die "the service is not installed, run ./install.sh first"

if [ "$PULL" -eq 1 ]; then
    step "Fetching the latest version"
    [ -d "$HERE/.git" ] || die "$HERE is not a git checkout (use --no-pull to just rebuild)"
    OLD="$(git -C "$HERE" rev-parse --short HEAD)"
    # run git as the owner of the checkout, so files do not become root-owned
    OWNER="$(stat -c %U "$HERE")"
    if [ "$OWNER" != root ] && [ "$(id -u)" -eq 0 ]; then
        sudo -u "$OWNER" git -C "$HERE" pull --ff-only
    else
        git -C "$HERE" pull --ff-only
    fi
    NEW="$(git -C "$HERE" rev-parse --short HEAD)"
    if [ "$OLD" = "$NEW" ]; then echo "already up to date ($NEW)"; else echo "updated $OLD -> $NEW"; fi
fi

step "Building"
cmake -S "$HERE" -B "$HERE/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$HERE/build"

step "Installing"
cmake --install "$HERE/build" --prefix "$PREFIX"
systemctl daemon-reload
systemctl restart "$UNIT"
sleep 2
if systemctl is-active --quiet "$UNIT"; then
    echo "Done, the service is running. Logs: journalctl -u sysdvr-lite -f"
else
    echo "warning: the service is not active, see: journalctl -u sysdvr-lite -n 30" >&2
    exit 1
fi
