#!/usr/bin/env bash
# Builds the patched SysDVR sysmodule (USB only, console screen watchdog) and packs it:
#
#   dist/exefs.nsp                    only the sysmodule: replaces the exefs.nsp of an installed SysDVR
#   dist/SysDVR-USB-screenwatch.zip   everything for a console that has no SysDVR yet: unzip it to the root of the SD card
#
# Needs devkitA64 and libnx from devkitPro (DEVKITPRO defaults to /opt/devkitpro) and python3.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
TID=00FF0000A53BB665

cd "$HERE/sysmodule"
make clean >/dev/null
make -j"$(nproc)"
[ -f sysmodule.nsp ] || { echo "error: sysmodule.nsp was not built" >&2; exit 1; }
# the build tags of the binary (the strings are compressed in the .nsp, so look at the .elf)
grep -aq SYSDVR_BUILD_USB_ONLY sysmodule.elf || { echo "error: this is not the USB-only build" >&2; exit 1; }
grep -aq SYSDVR_PATCH_SCREEN_WATCHDOG sysmodule.elf || { echo "error: the screen watchdog is missing from the build" >&2; exit 1; }

mkdir -p "$HERE/dist"
cp sysmodule.nsp "$HERE/dist/exefs.nsp"

# the SD card layout of a SysDVR install, with fixed timestamps so that the same sources give the same archive
python3 - "$HERE" "$TID" <<'PY'
import sys, zipfile
here, tid = sys.argv[1], sys.argv[2]
base = f"atmosphere/contents/{tid}"
files = [
    (f"{base}/exefs.nsp", open(f"{here}/dist/exefs.nsp", "rb").read()),
    (f"{base}/toolbox.json", open(f"{here}/sysmodule/toolbox.usb.json", "rb").read()),
    (f"{base}/flags/boot2.flag", b".\n"),
]
with zipfile.ZipFile(f"{here}/dist/SysDVR-USB-screenwatch.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for name, data in files:
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        info.external_attr = 0o644 << 16
        z.writestr(info, data)
PY

echo
(cd "$HERE/dist" && sha256sum exefs.nsp SysDVR-USB-screenwatch.zip && ls -l exefs.nsp SysDVR-USB-screenwatch.zip | awk '{print $5, $9}')
