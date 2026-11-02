#!/usr/bin/env bash
# Installs the prebuilt sysdvr-lite binary from the latest GitHub release instead of building it on the board:
# no compiler and no development packages, only the run-time libraries. For Debian 13 (Armbian trixie) arm64.
#
# Usage: sudo tools/fetch-binary.sh [URL or file]     (default: the latest release)
set -euo pipefail

ASSET=sysdvr-lite-linux-arm64-debian13.tar.gz
SRC="${1:-https://github.com/VLPLAY-Games/sysdvr-lite/releases/latest/download/$ASSET}"
PREFIX=/usr/local

die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root: sudo $0"
# shellcheck disable=SC1091
. /etc/os-release
[ "${VERSION_ID:-}" = 13 ] && { [ "${ID:-}" = debian ] || [ "${ID_LIKE:-}" = debian ]; } || die "the prebuilt binary is made for Debian 13 (trixie), build from source instead (see the README)"
[ "$(uname -m)" = aarch64 ] || die "expected an arm64 system"

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
export DEBIAN_FRONTEND=noninteractive
if [ -f "$SRC" ]; then
    cp "$SRC" "$tmp/archive.tar.gz"
else
    command -v curl >/dev/null || { apt-get update && apt-get install -y --no-install-recommends curl ca-certificates; }
    echo "downloading $SRC"
    curl -fsSL --retry 3 -o "$tmp/archive.tar.gz" "$SRC" || die "download failed (is there a release with a prebuilt binary yet?)"
fi
tar -xzf "$tmp/archive.tar.gz" -C "$tmp"
[ -f "$tmp/sysdvr-lite" ] && [ -f "$tmp/runtime-packages.txt" ] || die "unexpected archive contents"

mapfile -t PACKAGES < <(grep -vE '^[[:space:]]*(#|$)' "$tmp/runtime-packages.txt")
[ "${#PACKAGES[@]}" -gt 0 ] || die "the archive lists no packages"
apt-get update
apt-get install -y --no-install-recommends "${PACKAGES[@]}"

install -m 755 "$tmp/sysdvr-lite" "$PREFIX/bin/sysdvr-lite"
"$PREFIX/bin/sysdvr-lite" --version | head -1 || die "the installed binary does not start"
