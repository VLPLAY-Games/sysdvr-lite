#!/usr/bin/env bash
# Packs a built sysdvr-lite for the release page: the stripped binary and the list of Debian packages it
# needs at run time (installed by tools/fetch-binary.sh). Run on the machine that built it (Debian 13 arm64).
#
# Usage: tools/pack-release.sh BUILD_DIR OUTPUT.tar.gz
set -euo pipefail

BIN="${1:?build directory}/sysdvr-lite"
OUT="$(realpath -m "${2:?output file}")"
[ -x "$BIN" ] || { echo "error: $BIN not found" >&2; exit 1; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
cp "$BIN" "$tmp/sysdvr-lite"
strip "$tmp/sysdvr-lite"
{
    echo "# Debian packages the prebuilt sysdvr-lite needs. The last three are not linked: the GPU driver is"
    echo "# loaded at run time and the sound tools are used by the service and by doctor.sh"
    for lib in $(readelf -d "$tmp/sysdvr-lite" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
        ldd "$tmp/sysdvr-lite" | awk -v l="$lib" '$1 == l {print $3}'
    done | xargs -r readlink -f | xargs -r dpkg -S 2>/dev/null | cut -d: -f1 | sort -u
    echo libgl1-mesa-dri; echo libegl-mesa0; echo alsa-utils
} > "$tmp/runtime-packages.txt"
tar -C "$tmp" -czf "$OUT" sysdvr-lite runtime-packages.txt
echo "packed $OUT"; grep -vc '^#' "$tmp/runtime-packages.txt" | sed 's/^/runtime packages: /'
