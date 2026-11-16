# tests — 验证与实验

所有测试都以“能复现、能判读、能定位”为目标。`parser/` 里除 `npp_p0_reference.cpp` 外均为 CPU 测试；`npp_p0_reference.cpp` 需要 NVIDIA CUDA/NPP 环境。其余目录需要 AMD GPU / ROCm。

| 目录 | 用途 | 运行环境 |
|---|---|---|
| `parser/` | FFmpeg parser 复用、NPP 参考输出 | CPU；`npp_p0_reference.cpp` 需 CUDA/NPP |
| `zerocopy/` | VAAPI / Vulkan Video 解码 → HIP 读，逐字节像素校验；CUVID 级动态分辨率宿主 | AMD GPU + ROCm + Mesa |
| `sync/` | S1–S8 同步协议、HIP semaphore / DRM syncobj 探测 | AMD GPU |
| `vulkan-video/` | Vulkan Video 能力探测（basic/full） | AMD GPU + RADV |
| `quick-verify/` | mapped pointer kernel 读取、FFmpeg 双路径像素/零拷贝、多 slice/裁剪/seek 场景、8 路并发脚本 | AMD GPU + FFmpeg 7.1 |
| `npp/` | NPP Stage A 参考/候选探针、`libnppc` 核心 API 探针、FFmpeg `scale_npp` 端到端矩阵与逐字节比较 | NVIDIA NPP 或 AMD NPP shim + 授权 NPP 头文件；端到端矩阵另需启用 `libnpp` 的 FFmpeg |

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

# 动态分辨率（CUVID 宿主重建 decoder）
make -C tests/zerocopy verify_resolution_change
FFMPEG_BUILD_DIR=$HOME/ffmpeg-7.1 tests/zerocopy/run_resolution_change.sh
```

完整的环境准备、判据与排障见[验证手册](../docs/verification/验证手册.md)。

## 当前结果

2026-11-12 AMD 验证机：P0 双后端四路码流全部通过，`verify_kernel_read` errors=0，`make check` / `check_func` / parser / session 全 PASS。数字与边界详见 [P0 闭环验证结果](../docs/evidence/P0闭环验证结果_2026-11-12.md)。

2026-11-13 FFmpeg 宿主矩阵修复后：32 RUN / 32 PASS / 0 FAIL / 0 SKIP，双后端 `nvdec-unsafe` 均 D2D=0 且像素一致。结果与原始日志见 [FFmpeg 宿主双后端基线](../docs/evidence/FFmpeg宿主双后端基线_2026-11-13/README.md)。另有带 PTS 的 30 帧 MP4 严格顺序验证，双后端共 8 种模式 8 PASS / 0 FAIL，见 [PTS 顺序验证](../docs/evidence/FFmpeg_PTS顺序_2026-11-13/README.md)。

2026-11-16 复杂解码场景：多 slice、1080p 裁剪、MP4 seek 三个场景 × cuvid/nvdec × 双后端 **12/12 通过**，逐帧与软件解码一致。见 [解码复杂场景](../docs/evidence/解码复杂场景_2026-11-16/README.md)。

2026-11-16 动态分辨率与多流：CUVID 宿主动态切换 320×240 → 640×480，双后端各 22/22 帧一致；单进程 8 路并发解码，双后端全部与软件一致。同时修复了 `VkSequenceCache` 的进程级单槽串流问题。见 [多流与动态分辨率](../docs/evidence/多流与动态分辨率_2026-11-16/README.md)。长期参考帧仍有限制。

NPP Stage A：用最终基线 CUDA/NPP 12.0.1.104 头文件构建成功（无需 11.5 头文件重建），独立 FFmpeg `scale_npp` 已加载；AMD candidate 与 NVIDIA NPP 11.5.1.107 golden 的 32/32 条目逐字节一致，P2P3/P3P2 在双解码后端也逐字节一致。`libnppc` 核心 API 已补全并通过 `nppc_core_probe`；FFmpeg `scale_npp` 端到端 40 case 在 NVIDIA 真实 NPP 与 AMD shim 两侧 40/40 SHA256 一致。rawvideo + `hwupload_cuda` 的跨帧损坏已修复，`verify_hwupload_roundtrip.sh` 6/6 通过。详见 [AMD vs golden](../docs/evidence/NPP_StageA_AMD_11.5对比_2026-11-13/README.md)、[FFmpeg scale_npp AMD](../docs/evidence/NPP_FFmpeg_scale_npp_AMD_2026-11-16/README.md)、[NVIDIA golden](../docs/evidence/NPP_FFmpeg_scale_npp_NVIDIA_2026-11-16/README.md) 和 [hwupload 修复](../docs/evidence/cuda_shim_hwupload_fix_2026-11-16/README.md)。

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
