# tests — 验证与实验

所有测试都以“能复现、能判读、能定位”为目标。`parser/` 里除 `npp_p0_reference.cpp` 外均为 CPU 测试；`npp_p0_reference.cpp` 需要 NVIDIA CUDA/NPP 环境。其余目录需要 AMD GPU / ROCm。

| 目录 | 用途 | 运行环境 |
|---|---|---|
| `parser/` | FFmpeg parser 复用、NPP 参考输出 | CPU；`npp_p0_reference.cpp` 需 CUDA/NPP |
| `zerocopy/` | VAAPI / Vulkan Video 解码 → HIP 读，逐字节像素校验 | AMD GPU + ROCm + Mesa |
| `sync/` | S1–S8 同步协议、HIP semaphore / DRM syncobj 探测 | AMD GPU |
| `vulkan-video/` | Vulkan Video 能力探测（basic/full） | AMD GPU + RADV |
| `quick-verify/` | mapped pointer kernel 读取、FFmpeg 双路径像素/零拷贝脚本 | AMD GPU + FFmpeg 7.1 |
| `npp/` | NPP Stage A 参考/候选探针（3 函数、NN/linear/cubic）与逐字节比较 | NVIDIA NPP 或 AMD NPP shim + 授权 NPP 头文件 |

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

# 6. Quick Verify（AMD）
cd tests/quick-verify
./run_kernel_read.sh

# 四路码流 × 两个同级候选后端 × 四种宿主模式
FFMPEG=$HOME/ffmpeg-7.1/ffmpeg ./run_ffmpeg_compare.sh
```

完整的环境准备、判据与排障见[验证手册](../docs/verification/验证手册.md)。

## 当前结果

2026-11-12 AMD 验证机：P0 双后端四路码流全部通过，`verify_kernel_read` errors=0，`make check` / `check_func` / parser / session 全 PASS。数字与边界详见 [P0 闭环验证结果](../docs/evidence/P0闭环验证结果_2026-11-12.md)。

2026-11-13 FFmpeg 宿主矩阵修复后：32 RUN / 32 PASS / 0 FAIL / 0 SKIP，双后端 `nvdec-unsafe` 均 D2D=0 且像素一致。结果与原始日志见 [FFmpeg 宿主双后端基线](../docs/evidence/FFmpeg宿主双后端基线_2026-11-13/README.md)。另有带 PTS 的 30 帧 MP4 严格顺序验证，双后端共 8 种模式 8 PASS / 0 FAIL，见 [PTS 顺序验证](../docs/evidence/FFmpeg_PTS顺序_2026-11-13/README.md)。

NPP Stage A：真实 NPP 头文件下 shim 构建成功，独立 FFmpeg `scale_npp` 已加载；P2P3/P3P2 在双解码后端逐字节一致。resize 三算法尚未取得 NVIDIA golden，详见 [NPP Stage A 本地集成](../docs/evidence/NPP_StageA_本地集成_2026-11-13/README.md)。

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
