# 文档索引

本目录按“架构 / 验证 / 证据”三类存放。每份文档只负责一个主题；同一结论不在多处重复维护，其他文档用链接引用。所有相对链接以本文件所在目录为基准。

## 仓库结构

- [仓库结构与迁移](仓库结构与迁移.md)：目录树、旧路径映射、迁移自检。

## 架构与方案

### 总体方案（按职责分工）

| 文档 | 唯一职责 |
|---|---|
| [CUDA 兼容层总体方案](architecture/CUDA兼容层总体方案.md) | 管理层摘要 + 架构导航：结论、范围、状态、路线、成本摘要、风险与下一步 |
| [方案整合版](architecture/CUDA兼容层方案_整合版.md) | 技术事实、实验证据、数据通路、同步、ABI、遗留项与版本历史 |
| [方案决策实施版](architecture/CUDA兼容层方案_决策实施版.md) | 路线选型、成本/周期、人力、法务、验收门禁与长期 SDK 规划 |

### 专题文档（当前权威）

| 文档 | 唯一职责 |
|---|---|
| [内存与同步模型](architecture/内存与同步模型.md) | UVA vs UVM、`cuvidMapVideoFrame64` 契约、VAAPI/Vulkan 导出导入、同步基线、零拷贝验证结论 |
| [CUVID 到 VAAPI 映射表与 P0 API 覆盖](architecture/CUVID到VAAPI映射表.md) | P0 符号范围与不承诺项、CUVID/VAAPI 函数与结构体映射、parser 复用、libcuda 状态 |
| [NVDEC 与 VAAPI 解码对比](architecture/NVDEC与VAAPI解码对比.md) | 历史格式能力矩阵、NVDEC 直测性能、环境差异 |
| [NPP 兼容方案与 VPE 对比](architecture/NPP兼容方案与VPE对比.md) | NPP 26 项能力、VPE 能力与 VAAPI 暴露面、许可路线、库组织与实现方案 |
| [Vulkan Video 方案评估](architecture/Vulkan_Video方案评估.md) | Vulkan Video 能力探测、编码风险、VAAPI/Vulkan 选型依据 |
| [Vulkan 后端实现](architecture/Vulkan后端实现.md) | 后端架构、SPS/PPS 接缝、B 帧 UAF 根因与修复、验证结果、移植指南 |
| [HIPIFY 调研](architecture/HIPIFY调研.md) | HIPIFY 能力边界与使用方式 |

## 验证

| 文档 | 唯一职责 |
|---|---|
| [验证手册](verification/验证手册.md) | 环境准备、构建、快速验证、实验 A–E、判据、排障、证据记录 |
| [P0 闭环验证结果（2026-11-12）](evidence/P0闭环验证结果_2026-11-12.md) | 四路码流 × VAAPI/Vulkan 的逐字节结果、修复摘要、能/不能证明什么 |

## 证据

历史结果与日志放在 [evidence/](evidence/README.md)。这些是历史记录，不是当前 CI 的自动产物；其中部分绝对路径已脱敏为 `<repo>`，运行时以新目录结构为准。

## 阅读建议

1. 先看[内存与同步模型](architecture/内存与同步模型.md)和 [CUVID 到 VAAPI 映射表](architecture/CUVID到VAAPI映射表.md)，建立数据通路与 API 边界。
2. 需要判断当前状态时看 [P0 闭环验证结果](evidence/P0闭环验证结果_2026-11-12.md)；总体方案、整合版、决策版分别负责架构、证据和决策，不再各自维护状态表。
3. 需要动手时看[验证手册](verification/验证手册.md)。
4. 需要扩展 Vulkan 后端时看 [Vulkan 后端实现](architecture/Vulkan后端实现.md) 与 [Vulkan Video 方案评估](architecture/Vulkan_Video方案评估.md)。
