# FFmpeg 宿主 PTS / 显示顺序验证（2026-11-13）

> 输入：`testdata/h264/test_pts_320x240.mp4`，H.264 High、320×240、30 fps、30 帧、含 B 帧，63,423 bytes。
> SHA256：`ed3c36920f1998120560614b6c9457ae53d61256bf08df6e66432a6fae03129d`
> 环境与代码同 [FFmpeg 宿主双后端基线](../FFmpeg宿主双后端基线_2026-11-13/README.md)。

## 目的

P0 四路裸 Annex-B 码流没有输入 PTS；像素一致虽能证明输出顺序，但不能验证 CUVID 对容器时间戳的传递。本组使用带有效 PTS/DTS 的 MP4，启用脚本严格 PTS 分支：候选输出的完整 `(frame number, pts, pts_time)` 序列必须与软件参考一致。

## 结果

VAAPI + Vulkan 共 8 组合：**8 RUN / 8 PASS / 0 FAIL / 0 SKIP**。

| 后端 | `nvdec-safe` | `nvdec-unsafe` | `cuvid-cpu` | `cuvid-cuda` |
|---|---|---|---|---|
| VAAPI | PASS，PTS 一致 | PASS，PTS 一致，D2D=0 | PASS，PTS 一致 | PASS，PTS 一致 |
| Vulkan | PASS，PTS 一致 | PASS，PTS 一致，D2D=0 | PASS，PTS 一致 | PASS，PTS 一致 |

修复前 `nvdec-unsafe` 因 frame pool 耗尽只输出 29/30 帧并报 PTS 不一致；修复后通过。Vulkan `h264_nvdec` 使用合成参数集建立 session 后也能严格保持 PTS/显示顺序。

## 结论与边界

- 通过项证明 CUVID 显示时间戳与软件解码路径一致，且修复没有改变输出顺序。
- 容器由 `h264_vaapi` 在 AMD 验证机合成，仅用于时间戳/顺序回归，不代表目标平台编码质量或性能。
- 只覆盖单 slice、默认缩放矩阵和 POC type 0；其他参数集限制见主基线。
- 原始命令、环境、逐组合日志和 PTS 见本目录；不包含 YUV。

复现：

```bash
BACKENDS='vaapi vk' STREAMS=test_pts_320x240.mp4 \
  FFMPEG=$HOME/ffmpeg-7.1/ffmpeg \
  EVIDENCE_DIR=docs/evidence/ffmpeg-pts-$(date +%F) \
  ./tests/quick-verify/run_ffmpeg_compare.sh
```
