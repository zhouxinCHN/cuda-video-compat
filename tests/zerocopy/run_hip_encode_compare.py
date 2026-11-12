#!/usr/bin/env python3
# run_hip_encode_compare.py
# 实验 D 的像素比对：软解回读 /tmp/hip_to_encode_dec.yuv，与源图案（Y 竖直渐变 0..255, UV=128）比对。

import sys

W, H = 320, 240
data = open('/tmp/hip_to_encode_dec.yuv', 'rb').read()
y = data[:W * H]
uv = data[W * H: W * H * 3 // 2]

# 期望图案：Y 竖直渐变，UV=128
exp_y = bytes((r * 255 // (H - 1)) for r in range(H) for _ in range(W))
exp_uv = bytes([128]) * (W * H // 2)


def diff(a, b):
    return sum(1 for x, y in zip(a, b) if x != y)


dy = diff(y, exp_y)
duv = diff(uv, exp_uv)
total = len(y) + len(uv)
print(f"  Y差异={dy}/{len(y)}  UV差异={duv}/{len(uv)}  总差异率={(dy + duv) / total * 100:.3f}%")
if (dy + duv) / total < 0.02:
    print("  [PASS] HIP 写→编码→软解回读，与源图案一致（编码量化内 <2%）")
    sys.exit(0)
else:
    print("  [FAIL] 差异超限")
    sys.exit(1)
