#!/bin/sh
# Run on the Orange Pi Zero 2W and send the output back. Read-only, changes nothing.
echo "== kernel / os"; uname -a; cat /etc/os-release | head -3
echo "== video/media nodes"; ls -l /dev/video* /dev/media* /dev/cedar_dev /dev/dri/* 2>&1
echo "== modules"; lsmod | grep -i -E "cedrus|sunxi|cedar|v4l2|videobuf|hantro|rockchip" 
echo "== dmesg"; dmesg 2>/dev/null | grep -i -E "cedrus|cedar|video|ve:|sunxi-ve" | tail -30
echo "== v4l2 devices"
if command -v v4l2-ctl >/dev/null; then
  for d in /dev/video*; do echo "--- $d"; v4l2-ctl -d "$d" --all 2>&1 | head -40; v4l2-ctl -d "$d" --list-formats-out 2>&1; v4l2-ctl -d "$d" --list-formats 2>&1; done
else echo "v4l2-ctl missing (apt install v4l-utils)"; fi
echo "== ffmpeg"; command -v ffmpeg && ffmpeg -hide_banner -decoders 2>/dev/null | grep -i -E "h264|v4l2"; ffmpeg -hide_banner -hwaccels 2>/dev/null
echo "== cpu"; lscpu | head -12; cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null
echo "== display"; for c in /sys/class/drm/card*-*; do echo "$c: $(cat "$c"/status 2>/dev/null)"; done
