#!/usr/bin/env bash
# Installs the prebuilt sysdvr-lite binary from the latest GitHub release instead of building it on the board:
# no compiler and no development packages, only the run-time libraries. For Debian 13 (Armbian trixie) arm64.
#
# The release also carries a .sha256 file with the checksum of the archive; when it is there the archive is checked
# against it (this catches a damaged or cut download; it does not prove who published the release, that is what
# the HTTPS connection to GitHub is for). Releases made before 1.2.1 have no checksum: a warning is printed then.
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
    [ ! -f "$SRC.sha256" ] || cp "$SRC.sha256" "$tmp/archive.sha256"
else
    command -v curl >/dev/null || { apt-get update && apt-get install -y --no-install-recommends curl ca-certificates; }
    echo "downloading $SRC"
    curl -fsSL --retry 3 -o "$tmp/archive.tar.gz" "$SRC" || die "download failed (is there a release with a prebuilt binary yet?)"
    curl -fsSL --retry 3 -o "$tmp/archive.sha256" "$SRC.sha256" 2>/dev/null || rm -f "$tmp/archive.sha256"
fi
if [ -f "$tmp/archive.sha256" ]; then
    expected="$(awk 'NR == 1 {print $1}' "$tmp/archive.sha256")"
    actual="$(sha256sum "$tmp/archive.tar.gz" | awk '{print $1}')"
    [ -n "$expected" ] && [ "$expected" = "$actual" ] || die "the checksum of the archive does not match (expected ${expected:-none}, got $actual): the download is damaged, try again"
    echo "checksum ok ($actual)"
else
    echo "warning: this release has no checksum file, the archive is not verified" >&2
fi
tar -xzf "$tmp/archive.tar.gz" -C "$tmp"
[ -f "$tmp/sysdvr-lite" ] && [ -f "$tmp/runtime-packages.txt" ] || die "unexpected archive contents"

mapfile -t PACKAGES < <(grep -vE '^[[:space:]]*(#|$)' "$tmp/runtime-packages.txt")
[ "${#PACKAGES[@]}" -gt 0 ] || die "the archive lists no packages"
apt-get update
apt-get install -y --no-install-recommends "${PACKAGES[@]}"

install -m 755 "$tmp/sysdvr-lite" "$PREFIX/bin/sysdvr-lite"
"$PREFIX/bin/sysdvr-lite" --version | head -1 || die "the installed binary does not start"
