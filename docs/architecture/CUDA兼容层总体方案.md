# CUDA 兼容层总体方案（总纲）

> 版本：v2.0
> 文档日期：2026-11-12
> 目标：让基于 CUDA SDK 开发的产品，在自研驱动环境（Mesa + HIP）上**源码级重编译后能跑**，落到非 NVIDIA GPU；不要求严格二进制 drop-in。
> 职责：本文是**架构与导航**文档，只回答“整体怎么分层、各组件边界是什么、当前走到哪、细节去哪看”。技术证据、决策成本、验证步骤分别由整合版、决策实施版、验证手册承担，本文不重复。

---

## 1. 一句话架构

```text
用户 CUDA 产品源码
    │
    ├─ 用户自研 CUDA kernel ──→ 轨 1：HIPIFY 翻译成 HIP ──→ hipcc + HIP 运行时
    │
    └─ 调用 NVIDIA 现成库 ────→ 轨 2：同名 shim 库
                                  ├─ libcuda.so.1
                                  ├─ libnvcuvid.so.1 → VAAPI（默认）/ Vulkan Video（Tier 2）
                                  ├─ libnpp*.so.11（P0 只覆盖 3 个函数）
                                  └─ libnvidia-encode.so.1（不在 P0）
```

两条轨道解决的是不同问题：

| 轨道 | 管什么 | 当前定位 |
|---|---|---|
| **轨 1：HIPIFY** | 用户自己写的 `__global__` kernel、`cudaMalloc`、kernel launch | 保留的能力，**不属于 P0 交付组件**；客户有自研 kernel 时再启用 |
| **轨 2：shim 库** | 用户调用的 NVIDIA 库 API（CUVID / libcuda / NPP） | **P0 主体**；解码链路已通，NPP 与编码未验收 |

HIPIFY 不能翻译 NVDEC/NVENC/NPP，因为 ROCm 没有对应库；这些必须由 shim 重实现。详见 [HIPIFY 调研](HIPIFY调研.md)。

---

## 2. 范围与不承诺项

**P0 范围**：H.264、8-bit、4:2:0、NV12 的 NVDEC/CUVID 解码；FFmpeg `h264_cuvid` / `h264_nvdec` 宿主；FFmpeg `scale_npp` 实际使用的 3 个 NPP 函数。

**明确不承诺**：

- NVENC 与编码数据通路；
- H.264 以外的 codec、非 NV12、非 8-bit 4:2:0；
- 场/MBAFF、完整 CUDA/NPP 二进制 drop-in、任意现有应用兼容；
- 目标平台性能达标；P0 只记录性能，不设通过阈值。

符号范围与逐项映射见 [CUVID 到 VAAPI 映射表与 P0 API 覆盖](CUVID到VAAPI映射表.md)。

---

## 3. 组件边界

| 组件 | 职责 | 当前状态 |
|---|---|---|
| `libcuda.so.1` | 设备、上下文、内存、流/事件等 driver API 的 HIP 映射 | 82 个必需符号可加载；功能子集 47 项通过；**不构成完整 ABI/UVM 验收** |
| `libnvcuvid.so.1` | CUVID parser、decoder、`cuvidMapVideoFrame64` / `cuvidUnmapVideoFrame64` | VAAPI 与 Vulkan 双后端；P0 四路码流各 82/82 帧逐字节一致 |
| NPP shim | NPP 3 个 P0 函数的 HIP kernel 实现 | 方案与 NVIDIA 参考输出就绪；**AMD 侧尚未写实现，联合链路未通过** |
| `libnvidia-encode.so.1` | NVENC 编码 | **不在 P0**；旧映射表仅作历史参考 |

解码后端：

- **VAAPI（默认，Tier 1）**：线性 surface + dma-buf + HIP 导入，真零拷贝；
- **Vulkan Video（Tier 2）**：解码输出必须是 OPTIMAL tiling，需一次 `vkCmdCopyImageToBuffer` 转线性，无 CPU 中转但不是严格零拷贝。

两条路径的内存与同步契约见[内存与同步模型](内存与同步模型.md)。

---

## 4. 已确认的关键决策

| 决策点 | 结论 |
|---|---|
| 兼容层级 | 源码级重编译；不承诺严格二进制兼容 |
| 默认解码后端 | VAAPI；Vulkan Video 为 Tier 2 备选，默认关闭 |
| parser | 复用 FFmpeg 7.1 的 9 个解析对象 + `libavutil`；VAAPI 参数填充逻辑自写 |
| NPP 头文件 | 路线 A：shim 只产 `.so`，用户自带授权 CUDA SDK 头文件 |
| 零拷贝 | 边界 1（VAAPI→HIP）与边界 2（HIP→VAAPI）均已验证；编码链路未验收 |
| 同步基线 | 主机同步；跨栈异步 fence 未落地，sync_file/syncobj 仅为候选 |
| HIPIFY | 保留但不属于 P0；客户有自研 kernel 时再评估 |

---

## 5. 当前状态（2026-11-12）

已通过：

- 两个后端 × 四路码流，每后端 82/82 帧与软件参考逐字节一致；
- `verify_kernel_read` 两个后端 `errors=0`；
- Vulkan B 帧挂死（VCN ring timeout）根因已定位并修复；
- parser 复用、libcuda 功能子集、同步协议 S1–S8 通过。

未通过/未覆盖：

- FFmpeg 宿主双路径（`h264_cuvid` / `h264_nvdec`）的正式像素与零拷贝验收；
- NPP 联合链路、编码链路；
- 多流、seek/flush、动态分辨率、P010/奇数分辨率；
- 目标平台、1/8 路 30 分钟稳定性、真实业务性能。

完整结果与“能/不能证明什么”见 [P0 闭环验证结果](../evidence/P0闭环验证结果_2026-11-12.md)。

---

## 6. 路线图

```text
P0（当前）：
  ✅ libcuda 符号面与功能子集
  ✅ CUVID parser + VAAPI 解码/映射/释放 + Vulkan 后端
  ✅ 双后端四路码流逐字节闭环
  🔄 FFmpeg 宿主双路径验收
  ⬜ NPP 3 函数实现与联合链路

P1：
  ⬜ 多流 / seek / flush / 动态分辨率
  ⬜ 同步异步化（sync_file/syncobj，需 ROCm 协同）
  ⬜ NPP 其余重叠能力

P2：
  ⬜ 编码链路重估；NVENC 不在当前承诺
  ⬜ 目标平台性能与稳定性
```

决策依据、成本与阶段门禁见[方案决策实施版](CUDA兼容层方案_决策实施版.md)；完整技术证据与历史版本见[方案整合版](CUDA兼容层方案_整合版.md)。

---

## 7. 文档导航

| 文档 | 唯一职责 |
|---|---|
| [方案整合版](CUDA兼容层方案_整合版.md) | 技术事实、实验证据、数据通路、同步、ABI、遗留项与版本历史 |
| [方案决策实施版](CUDA兼容层方案_决策实施版.md) | 路线选型、成本/周期、人力、法务、验收门禁 |
| [方案优化版](CUDA兼容层方案_优化版.md) | 管理层速读：一页纸结论、当前状态、剩余缺口 |
| [内存与同步模型](内存与同步模型.md) | UVA/UVM、map 契约、VAAPI/Vulkan 导出导入、同步基线、零拷贝结论 |
| [CUVID 到 VAAPI 映射表与 P0 API 覆盖](CUVID到VAAPI映射表.md) | 符号范围、函数/结构体映射、parser 复用、libcuda 状态 |
| [NVDEC 与 VAAPI 解码对比](NVDEC与VAAPI解码对比.md) | 历史格式矩阵与 NVDEC 直测性能 |
| [NPP 兼容方案与 VPE 对比](NPP兼容方案与VPE对比.md) | NPP 能力、VPE 对比、许可、库组织与实现 |
| [Vulkan Video 方案评估](Vulkan_Video方案评估.md) | Vulkan 能力探测、编码风险、选型依据 |
| [Vulkan 后端实现](Vulkan后端实现.md) | 后端架构、SPS/PPS 接缝、B 帧根因、验证与移植 |
| [HIPIFY 调研](HIPIFY调研.md) | HIPIFY 能力边界与使用方式 |
| [验证手册](../verification/验证手册.md) | 环境、构建、快速验证、实验 A–E、判据、排障 |
