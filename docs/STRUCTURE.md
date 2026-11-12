# 仓库目录结构（迁移对照）

本文用于把另一台机器上的旧目录整理成相同结构。当前仓库按“组件 / 测试 / 文档 / 证据 / 基准 / 测试数据”分层，**不再把 FFmpeg 源码、编译产物、参考 YUV 放进仓库**。

## 最终目录树

```text
.
├── README.md
├── LICENSE-TBD.md
├── CONTRIBUTING.md
├── SECURITY.md
├── CODE_OF_CONDUCT.md
├── THIRD_PARTY.md
├── .gitignore
├── .gitattributes
├── .github/workflows/ci.yml
│
├── docs/
│   ├── README.md
│   ├── STRUCTURE.md                  # 本文件
│   ├── architecture/
│   │   ├── cuda-compat-overview.md
│   │   ├── cuda-compat-plan-integrated.md
│   │   ├── cuda-compat-plan-decisions.md
│   │   ├── cuda-compat-plan-optimized.md
│   │   ├── decode-memory-sync-model.md
│   │   ├── cuvid-vaapi-mapping.md
│   │   ├── api-coverage.md
│   │   ├── vulkan-backend.md
│   │   ├── vulkan-video-evaluation.md
│   │   ├── vulkan-video-todo.md
│   │   ├── hipify-research.md
│   │   ├── npp-compat-plan.md
│   │   ├── npp-vpe-comparison.md
│   │   └── nvdec-vaapi-comparison.md
│   ├── verification/
│   │   ├── zero-copy-experiment.md
│   │   ├── minimal-test-suite.md
│   │   ├── zero-copy-closed-loop.md
│   │   ├── quick-verify.md
│   │   ├── handoff-amd.md
│   │   └── vulkan-bframe-hang-debug-log.md
│   └── evidence/
│       ├── README.md
│       ├── p0-nvidia-hardware.log
│       ├── p0-nvidia-npp-check.log
│       └── results/...
│
├── cuda_shim/
│   ├── README.md
│   ├── Makefile
│   ├── CMakeLists.txt
│   ├── include/cuda_shim_types.h
│   ├── src/*.cpp, *.h
│   └── tests/
│       ├── test_func.cpp
│       ├── test_func_full.cpp
│       ├── verify_symbols.cpp
│       ├── verify_cuvid_symbols.cpp
│       ├── verify_cuvid_session.cpp
│       ├── verify_cuvid_parser.cpp
│       └── verify_kernel_read.cpp
│
├── tests/
│   ├── README.md
│   ├── parser/
│   │   ├── verify_parser_reuse.cpp / .make
│   │   ├── run_lgpl_build_check.sh
│   │   └── npp_p0_reference.cpp
│   ├── zerocopy/
│   │   ├── verify_decode_to_hip.cpp / .make
│   │   ├── verify_vk_decode_to_hip.cpp / .make
│   │   ├── verify_hip_to_encode.cpp / .make
│   │   ├── verify_portability.cpp / .make
│   │   ├── verify_modifier_linear.cpp
│   │   ├── verify_p010_linear.cpp
│   │   ├── verify_zerocopy.cpp
│   │   ├── retry_exp3.cpp / retry_exp3_v2.cpp
│   │   ├── hip_read_surface.cpp
│   │   ├── run_*.sh
│   │   └── run_hip_encode_compare.py
│   ├── sync/
│   │   ├── Makefile
│   │   ├── verify_sync.cpp
│   │   └── verify_semaphore_async.cpp
│   ├── vulkan-video/
│   │   ├── basic/  (Makefile + verify_vulkan_video.cpp)
│   │   └── full/   (Makefile + verify_vulkan_video_full.cpp)
│   └── quick-verify/
│       ├── README.md
│       ├── run_kernel_read.sh
│       ├── run_ffmpeg_compare.sh
│       └── run_all.sh
│
├── benchmarks/
│   ├── README.md
│   ├── npp/     (npp_benchmark.cpp + build script)
│   ├── nvdec/   (nvdec_benchmark.cpp + build script)
│   ├── vpp/     (vpp_benchmark.c + build script)
│   └── scripts/ (test/merge/collect 脚本)
│
├── testdata/
│   ├── README.md
│   └── h264/
│       ├── test_320x240.h264
│       ├── test_p_320x240.h264
│       ├── test_b_320x240.h264
│       ├── test_p_b.h264
│       ├── test_p_b_5.h264          # B 帧回归组
│       ├── test_p_b_7.h264
│       ├── test_p_main.h264
│       └── test_p_66.h264
│
└── scripts/
    └── check_links.py
```

## 旧路径 → 新路径

| 旧路径 | 新路径 / 处理 |
|---|---|
| `CUDA兼容层总体方案.md` | `docs/architecture/cuda-compat-overview.md` |
| `CUDA兼容层方案_整合版.md` | `docs/architecture/cuda-compat-plan-integrated.md` |
| `CUDA兼容层方案_决策实施版.md` | `docs/architecture/cuda-compat-plan-decisions.md` |
| `CUDA兼容层方案_优化版.md` | `docs/architecture/cuda-compat-plan-optimized.md` |
| `CUDA解码链路_内存管理与同步模型.md` | `docs/architecture/decode-memory-sync-model.md` |
| `CUVID_VAAPI_映射表.md` | `docs/architecture/cuvid-vaapi-mapping.md` |
| `HIPIFY_调研说明.md` | `docs/architecture/hipify-research.md` |
| `NPP_兼容方案.md` | `docs/architecture/npp-compat-plan.md` |
| `NPP_VPE_后处理对比报告.md` | `docs/architecture/npp-vpe-comparison.md` |
| `NVDEC_VAAPI_对比报告.md` | `docs/architecture/nvdec-vaapi-comparison.md` |
| `Vulkan_Video_方案评估.md` | `docs/architecture/vulkan-video-evaluation.md` |
| `Vulkan_Video_调研TODO.md` | `docs/architecture/vulkan-video-todo.md` |
| `cuda_shim/API_COVERAGE.md` | `docs/architecture/api-coverage.md` |
| `cuda_shim/VULKAN_BACKEND.md` | `docs/architecture/vulkan-backend.md` |
| `cuda_shim/HANDOFF_AMD.md` | `docs/verification/handoff-amd.md` |
| `cuda_shim/PROGRESS_B_FRAME_DEBUG.md` | 结论并入 `docs/architecture/vulkan-backend.md` §8，过程留档 `docs/verification/vulkan-bframe-hang-debug-log.md` |
| `零拷贝验证实验.md` | `docs/verification/zero-copy-experiment.md` |
| `最小验证实验套件.md` | `docs/verification/minimal-test-suite.md` |
| `zerocopy_test/README_闭环验证.md` | `docs/verification/zero-copy-closed-loop.md` |
| `quick_verify/README.md` | `docs/verification/quick-verify.md` |
| `quick_verify/*.sh` | `tests/quick-verify/*.sh` |
| `test/verify_parser_reuse.*` | `tests/parser/verify_parser_reuse.*` |
| `test/npp_p0_reference.cpp` | `tests/parser/npp_p0_reference.cpp` |
| `test/run_lgpl_build_check.sh` | `tests/parser/run_lgpl_build_check.sh` |
| `cuda_shim/test/*` | `cuda_shim/tests/*` |
| `cuda_shim/test_func*.cpp` | `cuda_shim/tests/test_func*.cpp` |
| `zerocopy_test/*` | `tests/zerocopy/*` |
| `sync_test/*` | `tests/sync/*` |
| `vulkan_video_test/*` | `tests/vulkan-video/basic/*` |
| `vulkan_video_test_full/*` | `tests/vulkan-video/full/*` |
| `scripts/npp_benchmark.cpp` 等 | `benchmarks/npp/`、`benchmarks/nvdec/`、`benchmarks/vpp/` |
| `scripts/test_*.sh` 等 | `benchmarks/scripts/` |
| `results/*` | `docs/evidence/results/*` |
| `p0-*.log` | `docs/evidence/*` |
| `test_*.h264`（多处副本） | `testdata/h264/`（去重） |
| `/tmp/test_p_b_5.h264`、`test_p_b_7.h264`、`test_p_main.h264`、`test_p_66.h264`（B 帧验证用，原先只在临时目录） | `testdata/h264/`（随修复一起入库，保证验证可复现） |

## 不进入仓库的内容

| 内容 | 处理 |
|---|---|
| `test/ffmpeg-lgpl-src/`（约 107MB） | 移到仓库外；用 `FFMPEG_SRC` / `FFMPEG` 指向外部 FFmpeg 7.1 构建 |
| `zerocopy_test_back/`（旧备份） | 移到仓库外；内容已由 `tests/zerocopy/` 覆盖 |
| `*.so`、`*.so.1`、`verify_*` 可执行文件、`test_func*` 可执行文件 | 由 Makefile 重新构建；`.gitignore` 排除 |
| `ref_*.yuv`（约 16MB） | 由 `tests/zerocopy/*.make` 用 FFmpeg 生成；`.gitignore` 排除 |
| `.claude/settings.local.json` | 本地工具配置；`.gitignore` 排除 |

## 在另一台机器上复刻

如果另一台机器已有旧目录，建议**新建目录后按上表移动**，不要直接删除旧文件：

```bash
OLD=/path/to/old/NVDEC_VAAPI_对比报告
NEW=/path/to/new/cuda-video-compat
mkdir -p "$NEW"

# 1. 拷贝源码/测试/文档/测试码流
rsync -av "$OLD/cuda_shim" "$OLD/zerocopy_test" "$OLD/sync_test" \
  "$OLD/vulkan_video_test" "$OLD/vulkan_video_test_full" \
  "$OLD/quick_verify" "$OLD/scripts" "$OLD/test" \
  "$OLD"/*.md "$OLD"/p0-*.log "$OLD"/results "$NEW/"

# 2. 按上表重命名/归类（可参考当前仓库的最终树）
# 3. 删除或移走 NEW/test/ffmpeg-lgpl-src、NEW/zerocopy_test_back、
#    NEW 下的 *.so/*.so.1/verify_*/test_func*、ref_*.yuv
```

如果另一台机器直接 `git clone` 当前仓库，则不需要上面的拷贝步骤，只需要按
[README](../README.md) 准备外部 FFmpeg / ROCm / Mesa 依赖。

## 迁移后的自检

```bash
python3 scripts/check_links.py
find . -type f -size +2M -print          # 应为空
bash -n tests/quick-verify/*.sh tests/zerocopy/*.sh
```

更完整的构建与运行方式见：

- [README](../README.md)
- [tests/README](../tests/README.md)
- [Quick Verify](verification/quick-verify.md)
- [AMD 交接](verification/handoff-amd.md)
