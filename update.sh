#!/usr/bin/env bash
# Updates sysdvr-lite on a board where install.sh has been run before:
# pulls the latest code, installs any build package that is missing (for example if you removed it),
# rebuilds, installs the new binary and restarts the service.
# It does not touch the CMA setting or your service options (decoder, audio device), which live in
# /etc/default/sysdvr-lite. A board installed before that file existed gets it filled from its old unit.
#
# A board installed with install.sh --binary gets the prebuilt binary of the latest release instead of a build.
#
# Usage: sudo ./update.sh [--no-pull] [--build]
#   --no-pull  rebuild the current checkout only          --build  build from source even if installed with --binary
set -euo pipefail

PULL=1
FORCE_BUILD=0
PREFIX=/usr/local
UNIT=sysdvr-lite.service

while [ $# -gt 0 ]; do
    case "$1" in
        --no-pull) PULL=0; shift ;;
        --build)   FORCE_BUILD=1; shift ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

die()  { echo "error: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }

# packages that are not installed (dpkg status "installed") out of the given names
missing_packages() {
    local p
    for p in "$@"; do
        [ "$(dpkg-query -W -f='${db:Status-Status}' "$p" 2>/dev/null || true)" = installed ] || echo "$p"
    done
}

# installs what is missing from the given names, does nothing when everything is there
ensure_packages() {
    local missing
    mapfile -t missing < <(missing_packages "$@")
    [ "${#missing[@]}" -gt 0 ] || { echo "all packages are installed"; return; }
    echo "missing packages: ${missing[*]}"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update || die "apt-get update failed: check the internet connection and the system clock"
    apt-get install -y --no-install-recommends "${missing[@]}" || die "could not install: ${missing[*]}"
}

[ "$(id -u)" -eq 0 ] || die "run as root: sudo ./update.sh"
[ "$(findmnt -no FSTYPE / 2>/dev/null)" != overlay ] || die "the read-only root is on (tools/readonly-root.sh): nothing written now would survive a reboot. Run: sudo tools/readonly-root.sh disable, reboot, run this again, then enable it and reboot"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "/etc/systemd/system/$UNIT" ] || die "the service is not installed, run ./install.sh first"
[ -f "$HERE/packages.txt" ] || die "$HERE/packages.txt not found: this is not a complete sysdvr-lite checkout"

if [ "$PULL" -eq 1 ]; then
    step "Fetching the latest version"
    [ -d "$HERE/.git" ] || die "$HERE is not a git checkout (use --no-pull to just rebuild)"
    ensure_packages git ca-certificates
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

BINARY=0
[ -f "$HERE/.binary-install" ] && [ "$FORCE_BUILD" -eq 0 ] && BINARY=1

# read after the pull: the new version may need more packages
if [ "$BINARY" -eq 1 ]; then
    step "Installing the prebuilt binary"
    "$HERE/tools/fetch-binary.sh"
else
step "Checking the build packages"
[ -f "$HERE/packages.txt" ] || die "$HERE/packages.txt not found after the pull"
mapfile -t PACKAGES < <(grep -vE '^[[:space:]]*(#|$)' "$HERE/packages.txt")
[ "${#PACKAGES[@]}" -gt 0 ] || die "$HERE/packages.txt lists no packages"
ensure_packages "${PACKAGES[@]}"

step "Building"
cmake -S "$HERE" -B "$HERE/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$HERE/build"

step "Installing"
cmake --install "$HERE/build" --prefix "$PREFIX"
fi
OPTS_FILE=/etc/default/sysdvr-lite
UNIT_DST="/etc/systemd/system/$UNIT"
if [ ! -f "$OPTS_FILE" ]; then       # installed before the options file existed: keep the options of the old unit
    OPTS="$(sed -n 's#^ExecStart=[^ ]*sysdvr-lite[[:space:]]*##p' "$UNIT_DST" | head -1)"
    [ -n "$OPTS" ] || OPTS="--decoder v4l2 --drm --audio-device plughw:CARD=HDMI,DEV=0"
    printf '# Options of the sysdvr-lite service (all options: sysdvr-lite --help).\n# After editing: sudo systemctl restart sysdvr-lite\nSYSDVR_OPTS="%s"\n' "$OPTS" > "$OPTS_FILE"
    echo "service options moved to $OPTS_FILE: $OPTS"
fi
if ! cmp -s "$HERE/tools/$UNIT" "$UNIT_DST"; then
    install -m 644 "$HERE/tools/$UNIT" "$UNIT_DST"
    echo "service unit updated"
fi
systemctl daemon-reload
systemctl restart "$UNIT"
sleep 2
if systemctl is-active --quiet "$UNIT"; then
    echo "Done, the service is running. Logs: journalctl -u sysdvr-lite -f"
else
    echo "warning: the service is not active, see: journalctl -u sysdvr-lite -n 30" >&2
    exit 1
fi
