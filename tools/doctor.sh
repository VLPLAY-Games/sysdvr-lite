#!/usr/bin/env bash
# Collects what is needed to debug a problem with sysdvr-lite: the board, the system, USB, decoder,
# display, audio and the service log. It only reads, changes nothing, and leaves out the host name,
# IP and MAC addresses and Wi-Fi names.
#
# Usage: sudo ./tools/doctor.sh > doctor.txt     (then attach doctor.txt to a GitHub issue)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
have() { command -v "$1" >/dev/null 2>&1; }
sec()  { printf '\n===== %s =====\n' "$*"; }
run()  { if have "$1"; then "$@" 2>&1; else echo "($1 not installed)"; fi; }

sec "sysdvr-lite"
BIN="$(command -v sysdvr-lite || true)"; [ -n "$BIN" ] || BIN="$ROOT/build/sysdvr-lite"
if [ -x "$BIN" ]; then "$BIN" --version 2>&1; else echo "binary not found"; fi
[ -d "$ROOT/.git" ] && echo "checkout: $ROOT ($(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null))"

sec "board and system"
echo "model:  $({ tr -d '\0' < /proc/device-tree/model; } 2>/dev/null || echo unknown)"
echo "kernel: $(uname -srm)"
. /etc/os-release 2>/dev/null && echo "os:     ${PRETTY_NAME:-unknown}"
[ -f /etc/armbian-release ] && grep -E '^(BOARD|VERSION|BRANCH)=' /etc/armbian-release
echo "time:   $(date -u '+%Y-%m-%d %H:%M UTC'), ntp synchronized: $(timedatectl show -p NTPSynchronized --value 2>/dev/null || echo unknown)"
grep -E '^(MemTotal|MemAvailable|CmaTotal|CmaFree):' /proc/meminfo
echo "cmdline cma: $(grep -o 'cma=[^ ]*' /proc/cmdline || echo 'not set')"

sec "service"
echo "active:  $(systemctl is-active sysdvr-lite 2>/dev/null || true)"
echo "enabled: $(systemctl is-enabled sysdvr-lite 2>/dev/null || true)"
grep -h '^ExecStart=' /etc/systemd/system/sysdvr-lite.service 2>/dev/null || echo "unit file not found"
echo "options (/etc/default/sysdvr-lite):"; grep -h '^SYSDVR_OPTS=' /etc/default/sysdvr-lite 2>/dev/null || echo "  not set, the default of the unit is used"

sec "log (last 40 lines)"
if have journalctl; then
    journalctl -u sysdvr-lite -n 40 --no-pager --no-hostname -o short-iso 2>&1 || echo "(run with sudo to read the journal)"
else
    echo "(journalctl not installed)"
fi

sec "USB (SysDVR is 18d1:4ee0)"
if have lsusb; then lsusb -d 18d1:4ee0 || echo "not connected"; else echo "(lsusb not installed)"; fi

sec "video decoder nodes"
# shellcheck disable=SC2012
ls -l /dev/video* /dev/media* 2>&1 | sed 's/^/  /'
if have v4l2-ctl; then v4l2-ctl --list-devices 2>&1 | sed 's/^/  /'; fi

sec "display (DRM)"
# shellcheck disable=SC2012
ls /dev/dri 2>&1 | sed 's/^/  /'
for c in /sys/class/drm/card*-*; do
    [ -e "$c/status" ] || continue
    echo "  $(basename "$c"): $(cat "$c/status")"
    [ "$(cat "$c/status")" = connected ] && head -6 "$c/modes" 2>/dev/null | sed 's/^/      mode /'
done

sec "audio"
run aplay -l | sed 's/^/  /'

sec "packages that are missing"
if have dpkg-query && [ -f "$ROOT/packages.txt" ]; then
    miss=0
    while read -r p; do
        [ -n "$p" ] && [ "${p#\#}" = "$p" ] || continue
        [ "$(dpkg-query -W -f='${db:Status-Status}' "$p" 2>/dev/null)" = installed ] || { echo "  $p"; miss=1; }
    done < "$ROOT/packages.txt"
    [ "$miss" -eq 1 ] || echo "  none"
else
    echo "  (dpkg or packages.txt not available)"
fi
