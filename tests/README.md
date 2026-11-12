# tests — 验证与实验

所有测试都以“能复现、能判读、能定位”为目标。`parser/` 里除 `npp_p0_reference.cpp` 外均为 CPU 测试；`npp_p0_reference.cpp` 需要 NVIDIA CUDA/NPP 环境。其余目录需要 AMD GPU / ROCm。

| 目录 | 用途 | 运行环境 |
|---|---|---|
| `parser/` | FFmpeg parser 复用、NPP 参考输出 | CPU；`npp_p0_reference.cpp` 需 CUDA/NPP |
| `zerocopy/` | VAAPI / Vulkan Video 解码 → HIP 读，逐字节像素校验 | AMD GPU + ROCm + Mesa |
| `sync/` | S1–S8 同步协议、HIP semaphore / DRM syncobj 探测 | AMD GPU |
| `vulkan-video/` | Vulkan Video 能力探测（basic/full） | AMD GPU + RADV |
| `quick-verify/` | mapped pointer kernel 读取、FFmpeg 双路径像素/零拷贝脚本 | AMD GPU + FFmpeg 7.1 |

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

完整的环境准备、判据与排障见[验证手册](../docs/verification/验证手册.md)。

## 当前结果

2026-11-12 AMD 验证机：P0 双后端四路码流全部通过，`verify_kernel_read` errors=0，`make check` / `check_func` / parser / session 全 PASS。数字与边界详见 [P0 闭环验证结果](../docs/evidence/P0闭环验证结果_2026-11-12.md)。

## 注意

- `vulkan-video/full/` 依赖系统 Vulkan-Headers 版本；当前以 AMD 验证机的 Mesa 25.2.8 头为准，其他 Vulkan SDK 版本可能需要适配。
- `tests/zerocopy/` 的参考 YUV 不提交，Makefile 会自动生成。
- `FFMPEG` 在 Makefile 里表示 FFmpeg 源码目录；在 `tests/quick-verify/run_ffmpeg_compare.sh` 里表示 ffmpeg 可执行文件名。两者同名但语义不同。

## 约定

- 测试码流统一放在 `testdata/h264/`，参考 YUV 不提交，由 Makefile 用 FFmpeg 生成。
- 需要外部 FFmpeg 7.1 的 Makefile 测试通过 `FFMPEG=/path/to/ffmpeg-7.1`、`FFMPEG_BIN=ffmpeg` 覆盖路径；quick-verify 脚本用 `FFMPEG=ffmpeg` 指定二进制。
- 新增测试请同时更新对应说明，并在 PR 中写清楚：验证了什么、判据是什么、不能证明什么。
