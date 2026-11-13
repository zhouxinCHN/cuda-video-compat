# testdata — 测试码流

只提交小体积、可长期复现的合成 H.264 码流。P0 主体使用 Annex-B；另有一个小体积 MP4 专门验证带 PTS/DTS 的显示顺序。参考 YUV 不提交，按需生成。

| 文件 | 内容 | 帧数 |
|---|---|---|
| `h264/test_320x240.h264` | I-only，320×240，Constrained Baseline | 30 |
| `h264/test_p_320x240.h264` | I/P，320×240（1 IDR + 29 P），Main | 30 |
| `h264/test_b_320x240.h264` | I/P/B，320×240（1 I + 10 P + 19 B），Main | 30 |
| `h264/test_p_b.h264` | I/P/B，320×240，Main，Level 2.0，`max_num_ref_frames=4`，shim 快速验证用 | 30 |
| `h264/test_p_b_5.h264` | I/P/B，320×240，Main，显示序 `I B B B P` 交替 | 9 |
| `h264/test_p_b_7.h264` | 同上，更长（3 个 P 周期） | 13 |
| `h264/test_p_main.h264` | I/P（无 B），320×240，Main，Level 2.0 | 30 |
| `h264/test_p_66.h264` | I/P（无 B），320×240，Constrained Baseline（profile_idc 66） | 30 |
| `h264/test_pts_320x240.mp4` | High，320×240，30 fps，含 B 帧；用于严格 PTS/显示顺序回归 | 30 |

后四个是 **B 帧验证组**：`test_p_b_5` / `test_p_b_7` 覆盖“解码序前几帧参考槽数递增 / 参考表修改”的路径；
`test_p_main` 与 `test_p_66` 是无 B 帧对照组（两种 profile）。这四个码流就是
[Vulkan 后端实现 §8.3](../docs/architecture/Vulkan后端实现.md#83-验证结果) 里那组
逐帧逐平面位精确结果的输入，复现命令见[验证手册](../docs/verification/验证手册.md) §3.5。

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

`test_pts_320x240.mp4` 的 SHA256 为
`ed3c36920f1998120560614b6c9457ae53d61256bf08df6e66432a6fae03129d`。
该文件只用于 PTS 顺序回归；P0 像素验收仍以四路裸 Annex-B 为主体。

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

# 带 PTS/DTS 的 PTS 顺序容器（需要 VAAPI H.264 编码器）
ffmpeg -y -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i "testsrc2=size=320x240:rate=30:duration=1" \
  -vf "format=nv12,hwupload" -c:v h264_vaapi -bf 2 -g 30 -qp 24 \
  test_pts_320x240.mp4
```

重新生成后请同步更新[验证手册](../docs/verification/验证手册.md)中的说明与结果；MP4 的 SHA256 和 PTS 结果也应更新。
