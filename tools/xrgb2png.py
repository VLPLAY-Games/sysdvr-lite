#!/usr/bin/env python3
"""Turns the raw frame saved by SYSDVR_DUMP into a PNG (standard library only, runs on the board too).

  sudo systemctl stop sysdvr-lite
  sudo SYSDVR_DUMP=/tmp/frame.xrgb sysdvr-lite --decoder v4l2 --drm --audio-device plughw:CARD=HDMI,DEV=0
  (Ctrl+C after a few seconds, the 60th frame is saved)  then:
  python3 tools/xrgb2png.py /tmp/frame.xrgb screenshot.png [width height]

The frame is 1280x720 XRGB8888 (BGRX in memory) unless you give another size.
"""
import struct
import sys
import zlib


def main():
    src, dst = sys.argv[1], sys.argv[2]
    w, h = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (1280, 720)
    data = open(src, "rb").read()
    if len(data) < w * h * 4:
        sys.exit(f"{src}: {len(data)} bytes, expected at least {w * h * 4} for {w}x{h}")
    rows = bytearray()
    for y in range(h):
        rows.append(0)                                              # filter type: none
        line = data[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]   # BGRX -> RGB
        rows += rgb

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))
    open(dst, "wb").write(png)
    print(f"{dst}: {w}x{h}, {len(png) // 1024} KiB")


if __name__ == "__main__":
    main()
