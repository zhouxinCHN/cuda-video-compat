# tests — 验证与实验

所有测试都以“能复现、能判读、能定位”为目标。`parser/` 里除 `npp_p0_reference.cpp` 外均为 CPU 测试；`npp_p0_reference.cpp` 需要 NVIDIA CUDA/NPP 环境。其余目录需要 AMD GPU / ROCm。

| 目录 | 用途 | 运行环境 |
|---|---|---|
| `parser/` | FFmpeg parser 复用、NPP 参考输出 | CPU；`npp_p0_reference.cpp` 需 CUDA/NPP |
| `zerocopy/` | VAAPI / Vulkan Video 解码 → HIP 读，逐字节像素校验；CUVID 级动态分辨率宿主 | AMD GPU + ROCm + Mesa |
| `sync/` | S1–S8 同步协议、HIP semaphore / DRM syncobj 探测 | AMD GPU |
| `vulkan-video/` | Vulkan Video 能力探测（basic/full） | AMD GPU + RADV |
| `quick-verify/` | mapped pointer kernel 读取、FFmpeg 双路径像素/零拷贝、多 slice/裁剪/seek、8 路并发、1080p/4K 矩阵、30 分钟长稳脚本 | AMD GPU + FFmpeg 7.1 |
| `npp/` | NPP Stage A/B 参考/候选探针、`libnppc` 核心 API 探针、FFmpeg `scale_npp` 端到端矩阵与逐字节比较 | NVIDIA NPP 或 AMD NPP shim + 授权 NPP 头文件；端到端矩阵另需启用 `libnpp` 的 FFmpeg |

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

# 5. NPP Stage A（NVIDIA 生成 golden / AMD shim 生成 candidate）
# 详见 tests/npp/README.md；必须先提供授权 NPP 头文件
make -C tests/npp GPU_RUNTIME=cuda NPP_INCLUDE_DIR=/path/to/cuda/include \
  CUDA_LIB_DIR=/path/to/cuda/lib64 NPP_LIB_DIR=/path/to/cuda/lib64
(cd tests/npp && ./npp_stage_a_probe --output /tmp/npp-golden)

# 6. libnppc 核心 API 探针 + FFmpeg scale_npp 端到端矩阵
make -C tests/npp GPU_RUNTIME=cuda NPP_INCLUDE_DIR=/path/to/cuda/include \
  NPP_LIB_DIR=/path/to/cuda/lib64 run-core
FFMPEG_BUILD_DIR=/path/to/ffmpeg-npp-build NPP_RUNTIME=cuda \
  OUT_DIR=/tmp/scale-npp-nvidia tests/npp/run_ffmpeg_scale_npp.sh
python3 tests/npp/compare_ffmpeg_scale_npp.py /tmp/scale-npp-nvidia /tmp/scale-npp-amd

# 7. Quick Verify（AMD）
cd tests/quick-verify
./run_kernel_read.sh

# 四路码流 × 两个同级候选后端 × 四种宿主模式
FFMPEG=$HOME/ffmpeg-7.1/ffmpeg ./run_ffmpeg_compare.sh

# 多 slice / 裁剪 / seek 场景 × cuvid/nvdec × 双后端
FFMPEG_BUILD_DIR=$HOME/ffmpeg-7.1 ./run_decode_scenarios.sh

# 8 路并发解码（单进程）
FFMPEG_BUILD_DIR=$HOME/ffmpeg-7.1 ./run_multi_stream.sh

# 1080p / 4K 正确性矩阵
FFMPEG_BUILD_DIR=$HOME/ffmpeg-7.1 ./run_size_matrix.sh

# 1/8 路 30 分钟长稳（默认 DURATION_SECONDS=1800）
FFMPEG_BUILD_DIR=$HOME/ffmpeg-7.1 ./run_stability.sh

# 动态分辨率（CUVID 宿主重建 decoder）
make -C tests/zerocopy verify_resolution_change
FFMPEG_BUILD_DIR=$HOME/ffmpeg-7.1 tests/zerocopy/run_resolution_change.sh
```

完整的环境准备、判据与排障见[验证手册](../docs/verification/验证手册.md)。

## 当前结果

当前实现、各测试已覆盖范围与未闭环项统一见[当前能力矩阵](../docs/当前能力矩阵.md)。本目录只维护测试入口和使用方法；历史批次输出见 [evidence](../docs/evidence/README.md)。

## 注意

- `vulkan-video/full/` 依赖系统 Vulkan-Headers 版本；当前以 AMD 验证机的 Mesa 25.2.8 头为准，其他 Vulkan SDK 版本可能需要适配。
- `tests/zerocopy/` 的参考 YUV 不提交，Makefile 会自动生成。
- `FFMPEG` 在 Makefile 里表示 FFmpeg 源码目录；在 `tests/quick-verify/run_ffmpeg_compare.sh` 里表示 ffmpeg 可执行文件。两者同名但语义不同。
- FFmpeg 7.1 中 `h264_nvdec` 是 hwaccel，不是 decoder；宿主脚本必须使用 `-hwaccel nvdec`，并固定加载 FFmpeg 构建树中的库。
- VAAPI 与 Vulkan Video 是同级候选后端，验证脚本不会把任意一方的失败自动记为 SKIP。

## 约定

- 测试码流统一放在 `testdata/h264/`，参考 YUV 不提交，由 Makefile 用 FFmpeg 生成。
- 需要外部 FFmpeg 7.1 的 Makefile 测试通过 `FFMPEG=/path/to/ffmpeg-7.1`、`FFMPEG_BIN=ffmpeg` 覆盖路径；quick-verify 脚本用 `FFMPEG=ffmpeg` 指定二进制。
- 新增测试请同时更新对应说明，并在 PR 中写清楚：验证了什么、判据是什么、不能证明什么。
