#!/usr/bin/env python3
"""compare_yuv.py ref.i420 hw.nv12 [frames]  - exact comparison of reference I420 and NV12 hardware output"""
import sys
W, H = 1280, 720
Y = W * H; F = Y * 3 // 2
ref = open(sys.argv[1], "rb"); hw = open(sys.argv[2], "rb")
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 100
bad = 0; n = 0
while n < limit:
    a = ref.read(F); b = hw.read(F)
    if len(a) < F or len(b) < F: break
    uv = bytearray(Y // 2); uv[0::2] = a[Y:Y + Y // 4]; uv[1::2] = a[Y + Y // 4:]
    if a[:Y] != b[:Y] or bytes(uv) != b[Y:]:
        bad += 1
        if bad <= 3:
            dy = sum(1 for i in range(Y) if a[i] != b[i])
            print(f"frame {n}: {dy} luma pixels differ")
    n += 1
print(f"compared {n} frames, {bad} not bit-exact")
