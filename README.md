# CUDA 解码兼容层（VAAPI / Vulkan Video）

在非 NVIDIA GPU 上，为**重新编译**的 CUDA 视频应用提供 NVDEC / CUVID / NPP 兼容层：用户的 CUDA kernel 由 HIPIFY 转为 HIP，NVIDIA 库调用由 `libcuda.so.1`、`libnvcuvid.so.1`、`libnpp*.so.11` 接住；解码后端可在 **VAAPI** 与 **Vulkan Video** 之间切换。

> 当前定位：**P0 双后端数据通路已验证，业务链路尚未验收**；不是生产可用版本。请先阅读[验证边界](#验证边界)。

## 仓库结构

```text
.
├── README.md                  # 本文件
├── NOTICE.md                  # 商标与关联关系声明
├── LICENSE-TBD.md             # 许可证待定，暂未授予开源许可
├── CONTRIBUTING.md            # 贡献指南
├── SECURITY.md                # 安全问题报告
├── CODE_OF_CONDUCT.md         # 行为准则
├── THIRD_PARTY.md             # 第三方依赖与许可证
├── docs/
│   ├── README.md              # 文档索引
│   ├── 仓库结构与迁移.md       # 目录树、旧路径映射、迁移自检
│   ├── architecture/          # 架构、方案、API 映射、内存模型
│   ├── verification/          # 验证手册（环境/构建/实验/排障）
│   └── evidence/              # 结果、日志与原始数据
├── cuda_shim/                 # 解码兼容层：libcuda + libnvcuvid
│   ├── include/               # CUDA 类型映射
│   ├── src/                   # VAAPI / Vulkan Video 后端、DMA-BUF 映射
│   ├── tests/                 # 组件内测试（符号、会话、parser、kernel 读取）
│   ├── Makefile
│   └── CMakeLists.txt
├── npp_shim/                  # NPP Stage A：libnppc/libnppig/libnppicc + 占位库
│   ├── src/                   # HIP resize / NV12↔YUV420P 与核心占位
│   └── Makefile
├── tests/
│   ├── parser/                # FFmpeg parser 复用、NPP 参考输出
│   ├── zerocopy/              # VAAPI / Vulkan Video → HIP 端到端像素校验
│   ├── sync/                  # 同步协议与异步 fence 现状探测
│   ├── vulkan-video/          # Vulkan Video 能力探测（basic / full）
│   ├── npp/                   # NPP Stage A 参考/候选探针与逐字节比较
│   └── quick-verify/          # mapped pointer kernel 读取、FFmpeg 双路径脚本
├── benchmarks/                # NVDEC / VAAPI / NPP / VPP 对比测试
├── testdata/h264/             # 小体积 H.264 测试码流
├── scripts/                   # 仓库维护脚本（链接检查等）
└── .github/                   # CI（Shell/Python/文档链接/大文件检查）与 PR 模板
```

## 快速开始

### 1. 依赖

- Linux + ROCm 7.x（HIP runtime 与头文件）
- `libva-dev`、`libva-drm`、`libdrm-dev`
- Vulkan loader；Vulkan Video 后端还需要 Mesa/RADV 与 `vk_video/*` 头
- FFmpeg **7.1** 源码的构建产物：`libavutil.so` + 9 个解析对象（`h2645_parse` / `h264_ps` / `h2645_vui` / `h2645data` / `h264data` / `h264_parse` / `golomb` / `mathtables` / `refstruct`）
- `nv-codec-headers`（CUDA / CUVID 头）
- FFmpeg 7.1 可执行文件（仅跑宿主路径验证时需要 `h264_cuvid` decoder 与 `nvdec` hwaccel；`h264_nvdec` 不是独立 decoder）

FFmpeg、Mesa、nv-codec-headers 都**不随仓库分发**，按外部依赖处理；详见 [THIRD_PARTY.md](THIRD_PARTY.md) 和[验证手册](docs/verification/验证手册.md)。

### 2. 构建兼容层

```bash
cd cuda_shim
make -j"$(nproc)" libcuda.so.1 libnvcuvid.so.1 \
  ROCM_DIR=/opt/rocm \
  FFMPEG_DIR=/path/to/ffmpeg-7.1 \
  NVCODEC_DIR=/path/to/nv-codec-headers/include \
  MESA_DIR=/path/to/mesa \
  VULKAN_LIB=-l:libvulkan.so.1
```

`VULKAN_LIB=-l:libvulkan.so.1` 适用于只有运行时 `libvulkan.so.1`、没有 `libvulkan.so` 开发软链的机器；装了 `libvulkan-dev` 时可以省略。

### 3. 跑快速验证

```bash
make -C cuda_shim verify_kernel_read \
  ROCM_DIR=/opt/rocm \
  HIPCC_EXTRA="--offload-arch=gfx1100"

cd tests/quick-verify
./run_kernel_read.sh
FFMPEG=$HOME/ffmpeg-7.1/ffmpeg ./run_ffmpeg_compare.sh
```

两个后端同级，完整宿主矩阵默认同时检查 `vaapi` 与 `vk`；定位时可临时用 `BACKENDS=vaapi` 或 `BACKENDS=vk`，但不能把另一种后端的失败视为可接受跳过。

预期结果、完整实验套件与排障见[验证手册](docs/verification/验证手册.md)。

## 两条后端路径

| 维度 | VAAPI | Vulkan Video |
|---|---|---|
| 解码输出 | 显式 `DRM_FORMAT_MOD_LINEAR` | 规范要求 `VK_IMAGE_TILING_OPTIMAL` |
| 显存共享 | surface → dma-buf → HIP 导入，真零拷贝 | image → GPU `vkCmdCopyImageToBuffer` → 线性 buffer → HIP 导入 |
| 额外拷贝 | `h264_nvdec` 零拷贝模式 0 次（需额外 surface；默认安全路径会被 FFmpeg 再拷贝）；`h264_cuvid` 有 FFmpeg 自身的 D2D 拷贝 | 每帧 1 次 GPU 内拷贝 |
| 同步基线 | `vaSyncSurface` + HIP event | Vulkan semaphore/fence + HIP event |
| 异步跨栈 fence | 未落地 | 未落地；需 sync_file/syncobj 与 ROCr 协同 |

技术细节见[内存与同步模型](docs/architecture/内存与同步模型.md)与 [Vulkan 后端实现](docs/architecture/Vulkan后端实现.md)。

## 验证边界

已记录的结果（完整证据见 [P0 闭环验证结果](docs/evidence/P0闭环验证结果_2026-11-12.md)）：

- **2026-11-12 AMD P0 闭环**：两个后端 × 四路码流，每后端 82/82 帧与软件参考逐字节一致（合计 164 帧）；`verify_kernel_read` 两个后端 `errors=0`；`make check` / `check_func` / parser / session 全部 PASS。
- Vulkan B 帧挂死（VCN ring timeout）根因已定位并修复（视频会话内存 use-after-free），见 [Vulkan 后端实现 §8](docs/architecture/Vulkan后端实现.md#8-上机验证结论rx-7900-xtx--radv)。
- `libcuda` 功能子集 47 PASS / 0 FAIL（历史记录）。
- VAAPI 真实解码 → HIP 读：I / I-P / I-P-B 各 30 帧逐字节一致。
- 同步协议 S1–S8 通过；S8 1000 帧文件句柄稳定。
- Vulkan Video 解码 → 拷贝 → 导出 → HIP 读：三类码流各 30 帧逐字节一致，无 fd/显存泄漏。
- **2026-11-13 FFmpeg 宿主双后端基线**：四路码流 × VAAPI/VK × 四种宿主模式共 32 组合，修复后 32 PASS / 0 FAIL / 0 SKIP。两后端的 `nvdec-unsafe` 均达到 D2D=0 且逐字节一致；带 PTS 容器的双后端 8/8 组合通过。零拷贝要求 `h264_nvdec` 分配额外 surface（`-extra_hw_frames`）。完整证据见 [宿主基线](docs/evidence/FFmpeg宿主双后端基线_2026-11-13/README.md)和 [PTS 顺序验证](docs/evidence/FFmpeg_PTS顺序_2026-11-13/README.md)。

仍**未**完成的验收项：

- FFmpeg 宿主结果扩展到真实业务应用、不同线程/队列配置和长时间运行；
- 多流、seek/flush、动态分辨率、P010/奇数分辨率；
- 异步跨栈 fence；
- NPP 联合链路：Stage A shim、独立 FFmpeg `scale_npp`、双后端 P2P3/P3P2 已打通并逐字节一致；resize 的 NN/linear/cubic 尚无 NVIDIA golden，最终覆盖全部 interp_algo。FFmpeg 将 libnpp 标记 nonfree，发布路径未决；编码链路（实验 D 输出异常，暂停）；
- 目标平台、1/8 路 30 分钟稳定性、真实业务性能；
- P0 原始运行命令、stdout/环境版本仍需归档。

## 文档

- [文档索引](docs/README.md)
- [总体方案](docs/architecture/CUDA兼容层总体方案.md)：总体架构、技术选型、成本人力、排期 Gate、法务/客户边界与长期 SDK 规划
- [技术实现、验证与风险边界](docs/architecture/CUDA兼容层方案_整合版.md)
- [仓库结构与迁移](docs/仓库结构与迁移.md)
- [内存与同步模型](docs/architecture/内存与同步模型.md)
- [CUVID 到 VAAPI 映射表与 P0 API 覆盖](docs/architecture/CUVID到VAAPI映射表.md)
- [Vulkan 后端实现](docs/architecture/Vulkan后端实现.md)
- [验证手册](docs/verification/验证手册.md)
- [P0 闭环验证结果](docs/evidence/P0闭环验证结果_2026-11-12.md)

## 商标与关联关系

本项目是独立的兼容层研究项目，**与 NVIDIA 无隶属、赞助或背书关系**。CUDA、NVIDIA、NVDEC、NPP、NVENC 等名称仅用于描述 API 兼容性；仓库不包含 NVIDIA 专有源码、二进制、SDK 头文件或驱动文件。详见 [NOTICE.md](NOTICE.md)。

## 许可证

许可证尚未确定：见 [LICENSE-TBD.md](LICENSE-TBD.md)。在许可证确定前，仓库不授予开源许可；第三方依赖的许可证见 [THIRD_PARTY.md](THIRD_PARTY.md)。

## 贡献与安全

- [CONTRIBUTING.md](CONTRIBUTING.md)
- [SECURITY.md](SECURITY.md)
- [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)
