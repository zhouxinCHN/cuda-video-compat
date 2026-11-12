# testdata — 测试码流

只提交小体积、可长期复现的 H.264 Annex-B 码流；参考 YUV 不提交，按需生成。

| 文件 | 内容 | 帧数 |
|---|---|---|
| `h264/test_320x240.h264` | I-only，320×240 | 30 |
| `h264/test_p_320x240.h264` | I/P，320×240（1 IDR + 29 P） | 30 |
| `h264/test_b_320x240.h264` | I/P/B，320×240（1 I + 10 P + 19 B） | 30 |
| `h264/test_p_b.h264` | I/P/B，320×240，shim 快速验证用 | 30 |

## 生成参考 YUV

参考文件不进入仓库。以 `tests/zerocopy/` 为例：

```bash
cd tests/zerocopy
make -f verify_decode_to_hip.make ref_320x240.yuv ref_p_320x240.yuv ref_b_320x240.yuv
# 或直接跑对应 case，Makefile 会自动生成
make -f verify_decode_to_hip.make run-b
```

等价的 FFmpeg 命令：

```bash
ffmpeg -y -i ../../testdata/h264/test_p_320x240.h264 -pix_fmt nv12 -f rawvideo ref_p_320x240.yuv
```

## 重新生成测试码流

码流已随仓库提交，一般不需要重新生成。若必须重建，使用 FFmpeg 7.1：

```bash
# I/P（VAAPI 编码器在 bframes=0 时通常产出 poc_type=2）
ffmpeg -y -f lavfi -i "testsrc2=size=320x240:rate=30:duration=1" \
  -vf format=yuv420p -c:v libx264 -preset ultrafast \
  -x264-params "keyint=30:bframes=0:scenecut=0" test_p_320x240.h264

# I/P/B
ffmpeg -y -f lavfi -i "testsrc2=size=320x240:rate=30:duration=1" \
  -vf format=yuv420p -c:v libx264 -preset ultrafast \
  -x264-params "keyint=30:bframes=2:scenecut=0" test_b_320x240.h264
```

重新生成后请同步更新 [最小验证实验套件](../docs/verification/minimal-test-suite.md) 中的说明与结果。
