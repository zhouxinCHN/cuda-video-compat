# tests — 验证与实验

所有测试都以“能复现、能判读、能定位”为目标；没有 GPU 的测试放在 `parser/`，需要 AMD GPU / ROCm 的放在其余目录。

| 目录 | 用途 | 运行环境 |
|---|---|---|
| `parser/` | FFmpeg parser 复用、NPP 参考输出 | CPU（需 FFmpeg 7.1 构建产物） |
| `zerocopy/` | VAAPI / Vulkan Video 解码 → HIP 读，逐字节像素校验 | AMD GPU + ROCm + Mesa |
| `sync/` | S1–S8 同步协议、HIP semaphore / DRM syncobj 探测 | AMD GPU |
| `vulkan-video/` | Vulkan Video 能力探测（basic/full） | AMD GPU + RADV |
| `quick-verify/` | mapped pointer kernel 读取、FFmpeg 双路径像素/零拷贝 | AMD GPU + FFmpeg 7.1 |

## 常用命令

```bash
# 1. parser 复用（CPU）
cd tests/parser
make -f verify_parser_reuse.make
./verify_parser_reuse ../../testdata/h264/test_320x240.h264

# 2. VAAPI 解码 → HIP 读（AMD）
cd tests/zerocopy
make -f verify_decode_to_hip.make run-b

# 3. Vulkan Video 解码 → 拷贝 → HIP 读（AMD）
make -f verify_vk_decode_to_hip.make run-b

# 4. 同步协议（AMD）
cd tests/sync
make run

# 5. Quick Verify（AMD）
cd tests/quick-verify
BACKENDS=vaapi ./run_kernel_read.sh
BACKENDS=vaapi ./run_ffmpeg_compare.sh
```

## 当前结果

2026-11-12 AMD 验证机：两个后端 × 四路码流，每后端 82/82 帧逐字节一致（合计 164 帧）；
`verify_kernel_read` errors=0；`make check` / `check_func` / parser / session 全 PASS。
详见 [P0 结果记录](../docs/evidence/p0-2026-11-12.md)。

## 注意

- `vulkan-video/full/` 依赖系统 Vulkan-Headers 版本；当前以 AMD 验证机的 Mesa 25.x 头为准，
  其他 Vulkan SDK 版本可能需要适配。
- `tests/zerocopy/` 的参考 YUV 不提交，`make` 会自动生成。

## 约定

- 测试码流统一放在 `testdata/h264/`，参考 YUV 不提交，由 Makefile 用 FFmpeg 生成。
- 需要外部 FFmpeg 7.1 的测试通过 `FFMPEG=/path/to/ffmpeg-7.1`、`FFMPEG_BIN=ffmpeg` 覆盖路径。
- 新增测试请同时更新对应说明，并在 PR 中写清楚：验证了什么、判据是什么、不能证明什么。
