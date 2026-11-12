# 文档索引

本目录按“架构 / 验证 / 证据”三类存放。所有相对链接均以本文件所在目录为基准。

## 仓库结构

- [目录结构与迁移对照](STRUCTURE.md)

## 架构与方案

| 文档 | 内容 |
|---|---|
| [CUDA 兼容层总体方案](architecture/cuda-compat-overview.md) | 双轨（HIPIFY + shim）、组件边界、总体路线 |
| [方案整合版](architecture/cuda-compat-plan-integrated.md) | 完整技术方案、数据通路、同步、Vulkan Video、风险与遗留项 |
| [方案决策实施版](architecture/cuda-compat-plan-decisions.md) | 三种落地路线、阶段计划、验收口径、未完成工作 |
| [方案优化版](architecture/cuda-compat-plan-optimized.md) | 成本/范围优化后的方案版本 |
| [CUDA 解码链路内存与同步模型](architecture/decode-memory-sync-model.md) | UVA vs UVM、`cuvidMapVideoFrame64`、VAAPI/Vulkan 导出导入与同步 |
| [CUVID → VAAPI 映射表](architecture/cuvid-vaapi-mapping.md) | CUVID API/结构体到 VAAPI 的逐项映射 |
| [API 覆盖基线](architecture/api-coverage.md) | P0 符号面、FFmpeg 直接调用、libcuda 范围 |
| [Vulkan 后端说明](architecture/vulkan-backend.md) | `CUVID_SHIM_BACKEND=vk` 的架构、SPS/PPS 接缝、B 帧挂死根因与修复（§8）、未完成项 |
| [Vulkan Video 方案评估](architecture/vulkan-video-evaluation.md) | Vulkan Video 能力、约束与可行性 |
| [Vulkan Video 调研 TODO](architecture/vulkan-video-todo.md) | P0 拆解、验证矩阵与后续事项 |
| [HIPIFY 调研](architecture/hipify-research.md) | HIPIFY 能力边界与使用方式 |
| [NPP 兼容方案](architecture/npp-compat-plan.md) | NPP 头文件、SONAME、函数范围与法律边界 |
| [NPP / VPE 后处理对比](architecture/npp-vpe-comparison.md) | NPP 与 VAAPI VPP 的逐项能力/性能对比 |
| [NVDEC / VAAPI 对比](architecture/nvdec-vaapi-comparison.md) | 解码能力、格式矩阵与测试结果 |

## 验证与实验

| 文档 | 内容 |
|---|---|
| [零拷贝验证实验](verification/zero-copy-experiment.md) | 实验 1/2/3、DMA-BUF 导出导入、边界定位 |
| [最小验证实验套件](verification/minimal-test-suite.md) | 实验 A–E 的目录、运行方式与判据 |
| [零拷贝闭环验证](verification/zero-copy-closed-loop.md) | VAAPI 解码闭环与 HIP 读写外部内存 |
| [Quick Verify](verification/quick-verify.md) | mapped pointer kernel 读取、FFmpeg 双路径像素、零拷贝计数 |
| [AMD 验证机交接](verification/handoff-amd.md) | Vulkan 后端在 AMD 机上的构建、运行与排障 |
| [Vulkan B 帧挂死调试留档](verification/vulkan-bframe-hang-debug-log.md) | VCN ring timeout 的现场、已排除假设、测量陷阱、可复现验证命令 |

## 证据

历史结果与日志放在 [evidence/](evidence/README.md)。这些是**历史记录**，不是当前 CI 的自动产物；
其中的绝对路径已脱敏为 `<repo>`，运行时请以新目录结构为准。

## 阅读建议

1. 先看 [总体方案](architecture/cuda-compat-overview.md) 和 [内存与同步模型](architecture/decode-memory-sync-model.md)。
2. 再看 [方案决策实施版](architecture/cuda-compat-plan-decisions.md) 的验证边界与未完成工作。
3. 需要动手时看 [Quick Verify](verification/quick-verify.md) 和 [AMD 交接](verification/handoff-amd.md)。
