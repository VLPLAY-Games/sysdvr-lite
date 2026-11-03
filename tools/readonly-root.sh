#!/usr/bin/env bash
# Optional protection of the SD card against a sudden power cut: the root filesystem is mounted read-only and
# everything written while the system runs goes to memory (overlayroot) and is gone at the next boot. The card is
# then not written at all while the board runs, so unplugging the board cannot damage it.
#
#   sudo tools/readonly-root.sh enable    install overlayroot and switch it on from the next boot
#   sudo tools/readonly-root.sh disable   switch it off from the next boot
#   tools/readonly-root.sh status
#
# While it is on, NOTHING you change survives a reboot: settings in /etc/default/sysdvr-lite, the Wi-Fi password, a
# new password, packages, updates. To change something, run `disable`, reboot, change it, run `enable`, reboot again.
# (install.sh and update.sh refuse to run while it is on, so that they do not report a success that is lost.)
#
# If the board does not boot after `enable`: put the SD card in a computer, open the first partition and add
# overlayroot=disabled to the extraargs= line of /boot/armbianEnv.txt.
set -euo pipefail

CONF=/etc/overlayroot.conf
die() { echo "error: $*" >&2; exit 1; }
active()     { [ "$(findmnt -no FSTYPE / 2>/dev/null)" = overlay ]; }
configured() { grep -Eq '^[[:space:]]*overlayroot="?tmpfs' "$CONF" 2>/dev/null; }

status() {
    if active; then echo "read-only root: ON (the system runs on a memory overlay, changes are lost at reboot)"
    else echo "read-only root: off"; fi
    if configured; then echo "next boot: on"; else echo "next boot: off"; fi
}

case "${1:-status}" in
status) status ;;
enable)
    [ "$(id -u)" -eq 0 ] || die "run as root: sudo $0 enable"
    command -v apt-get >/dev/null || die "this needs a Debian based system"
    [ -f /boot/armbianEnv.txt ] || echo "warning: this is not an Armbian image; the initramfs step below may need a different command" >&2
    if active; then die "it is already on; run '$0 disable' and reboot first if you want to change something"; fi
    echo "==> Installing overlayroot"
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends overlayroot
    echo "==> Configuring it"
    [ -f "$CONF.sysdvr-lite.bak" ] || { [ ! -f "$CONF" ] || cp "$CONF" "$CONF.sysdvr-lite.bak"; }
    if grep -q '^[[:space:]]*overlayroot=' "$CONF" 2>/dev/null; then
        sed -i 's|^[[:space:]]*overlayroot=.*|overlayroot="tmpfs:swap=0,recurse=0"|' "$CONF"
    else
        echo 'overlayroot="tmpfs:swap=0,recurse=0"' >> "$CONF"
    fi
    echo "==> Rebuilding the initramfs"
    update-initramfs -u
    sync
    echo
    echo "Done. The root filesystem will be read-only from the next boot:  sudo reboot"
    echo "To change anything later: sudo $0 disable, reboot, change it, sudo $0 enable, reboot."
    ;;
disable)
    [ "$(id -u)" -eq 0 ] || die "run as root: sudo $0 disable"
    if active; then
        command -v overlayroot-chroot >/dev/null || die "overlayroot-chroot not found"
        overlayroot-chroot sed -i 's|^[[:space:]]*overlayroot=.*|overlayroot=""|' "$CONF"
    elif [ -f "$CONF" ]; then
        sed -i 's|^[[:space:]]*overlayroot=.*|overlayroot=""|' "$CONF"
    fi
    sync
    echo "Switched off from the next boot:  sudo reboot"
    ;;
*) die "usage: $0 enable|disable|status" ;;
esac
