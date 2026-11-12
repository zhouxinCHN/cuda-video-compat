# CUDA SDK 兼容层总体方案（整合版）

> 版本：v1.37（Vulkan Video 后端接入 CUVID shim 并通过 FFmpeg 宿主验证）
> 整合时间：2026-11-07
> 整合范围：当前聚焦 NVDEC/CUVID 解码兼容与 FFmpeg 实际使用的 NPP 功能；编码、完整 CUDA 生态和用户应用兼容不属于当前交付承诺。本文保留编码及 HIPIFY 内容作为历史技术参考。
> 目标读者：架构评审、工程实施、风险决策

---

## 目录

0. [决策摘要](#决策摘要)
1. [背景与目标](#一背景与目标)
2. [总体架构：双轨并行](#二总体架构双轨并行)
3. [初步方案与选型（详述）](#三初步方案与选型详述)
4. [风险点、验证与结论](#四风险点验证与结论)
5. [总体结论与目标数据通路](#五总体结论与目标数据通路)
6. [实施路线图](#六实施路线图)
7. [附录](#七附录)

---

## 决策摘要

**当前建议**：以 **CUVID shim→VAAPI + NPP shim→HIP** 为当前交付主线。P0 范围、宿主、API 清单和验收口径已形成评审基线；实验 C/E 与 NPP NVIDIA 参考基线已通过；CUVID parser 已复用 FFmpeg 解析并在 AMD 验证机通过 I-only 与 I/P/B 解析测试。端到端解码链路（parser→decode→map→HIP 读）和 FFmpeg 双路径仍未闭合，因此 **P0 尚未整体通过**，不得据此承诺生产可用、完整 CUDA SDK 兼容或确定性交付周期。

| 关键结论 | 证据等级 | 当前决策含义 |
|---|---|---|
| VAAPI 可作为 NVDEC 兼容后端候选 | **AMD-PoC** | 选定格式及 AMD 测试支持主路线，不代表全部设备和码流能力已覆盖 |
| DMA-BUF 句柄/资源链路可打通 | **AMD-PoC** | 具备零拷贝设计基础；当前只将解码侧纳入交付，不据此承诺编码或其他链路 |
| 线性 surface 可作为共享资源的优先路径 | **AMD-PoC** | 当前平台内容正确且未见明显性能回退，目标平台须复测 |
| 主机同步可作为首期基线 | **AMD-PoC** | 已有顺序与资源管理测试，真实并发流水线仍需验证 |
| 真实解码→HIP 读底层数据通路 | **AMD-PoC** | 实验 C 已闭合 libva 真实解码→LINEAR surface→DMA-BUF→HIP 逐字节校验；该结果不代表 CUVID shim 或 FFmpeg `h264_cuvid`/`h264_nvdec` 已验收 |
| DRM sync_file 跨栈异步同步 | **设计假设** | 基础原语可用，真实跨引擎 fence 传递、排序及异常处理未完成 |
| CUVID shim 当前中间状态 | **AMD-PoC（符号/session/parser/后端）** | 21 个必需符号及 `cuvidGetDecoderCaps` 可解析；VAAPI LINEAR NV12 session 创建/销毁通过；parser 复用 FFmpeg 解析，I-only 与 I/P/B 码流各 30 帧解析回调通过；Vulkan Video 后端已实现并接入（`CUVID_SHIM_BACKEND=vk` 切换），I/IP/IPB 各 30 帧逐字节一致 + fd 无泄漏。真实 decode/map 像素链路以 Vulkan 后端闭合，VAAPI 后端及 FFmpeg 双路径像素验收仍未完成 |
| NPP NVIDIA 参考基线 | **NVIDIA 参考机已验证** | RTX 4060 + NPP 11.5.1.107 已生成 3 个 P0 函数的确定性参考输出；后续 AMD shim 须逐字节一致 |
| 平台及结论移植边界 | **平台约束已锁定，交付平台仍待验证** | 当前平台锁定 GFX ISA 并全面支持 AMD 私有 HSA API；AMD 验证机结果仍不得替代后续实际交付平台的完整验收 |

**证据等级定义**：

- **AMD-PoC**：已在文档所列 AMD 硬件和软件环境完成对应概念验证，仅证明已测项目与平台。
- **目标平台已验证**：已在目标厂商平台按交付场景验证；当前尚无此类结论。
- **设计假设**：有架构依据或局部实验支撑，但关键端到端链路尚未实现。
- **待验证**：尚无充分证据，必须作为后续阶段通过条件或待办跟踪。

全文中的“PASS”仅表示对应测试项通过，不自动升级为端到端、完整 API、生产质量或目标平台验收通过。

**本次 P0 评审已确认**：

1. 当前只承诺约定范围内的 **NVDEC/CUVID 解码 API + NPP 功能 API**；NVENC、编码链路和其他 CUDA 领域不作承诺。
2. P0 仅以官方源码自编译的 **FFmpeg 7.1** 为验收宿主，覆盖 `h264_cuvid` 与 `h264_nvdec`；用户应用及其 NPP 清单移至 P1。
3. 采用源码级重编译，不要求现有 FFmpeg 二进制不重编译运行；但 FFmpeg 自身 `dlopen/dlsym` 所需符号面仍必须满足。
4. P0 性能只记录不设通过阈值；功能覆盖 1080p/4K、1 路和 8 路进程，分别执行 30 分钟资源稳定性检查。
5. **当前状态**：P0-6 已恢复实现，parser 解析链路（复用 FFmpeg）在 AMD 验证机通过 I-only 与 I/P/B 测试；CUVID shim 新增 **Vulkan Video 解码后端**（`CUVID_SHIM_BACKEND=vk`）并完成 I/IP/IPB 各 30 帧逐字节一致 + fd 无泄漏验收，FFmpeg `h264_cuvid` 宿主对 I/P/B 码流 30 帧 0 decode error 跑通；`h264_nvdec` 零拷贝与 VAAPI 后端像素链路仍未闭合；P0-9 原始日志与复现材料仍需继续归档。

---

## 一、背景与目标

### 1.1 行业背景：CUDA 不只是一个 SDK，而是一个生态锁定

NVIDIA 用 CUDA 把"硬件 + 驱动 + SDK + 用户代码"绑成了一个完整生态。一个基于 CUDA 开发的产品，通常同时依赖三层东西：

1. **用户自研 CUDA kernel**：`__global__` 函数、`cudaMalloc`、`kernel<<<>>>` 等，是用户自己的计算逻辑。
2. **NVIDIA 闭源加速库**：视频解码（NVDEC/CUVID）、视频编码（NVENC）、图像处理（NPP）、线性代数（cuBLAS）等，是 NVIDIA 写好、以二进制分发的库。
3. **CUDA driver/runtime**：`libcuda.so`、`libcudart.so`，是上面两层共同的地基。

这三层**深度绑定 NVIDIA 的驱动和硬件**：CUVID 解码路径通常由 parser 组件解析码流，再由 NVDEC 专用视频引擎执行解码；NVENC 通过一张函数指针表驱动硬件编码；NPP 则是一组预编译的 CUDA kernel。它们都不是"换家编译器就能跑"的普通代码。

### 1.2 新厂商的困境

一家新 GPU 厂商（本项目），其**用户态驱动基于 Mesa 开发**，硬件 ISA 为 GFX/AMDGPU 系。硬件本身能跑，但面临一个现实问题：

> 用户手里已经有一批基于 CUDA SDK 开发的产品，**在 NVIDIA 之外跑不起来**。

让用户把产品**全部重写**成新厂商的自有 SDK，成本极高、周期极长，商业上基本不可接受。因此必须做**兼容层**，让用户的 CUDA 产品以尽量低的改动代价跑在新硬件上。

### 1.3 兼容的两个层次，以及本项目的选择

| 层次 | 含义 | 代价 |
|---|---|---|
| **二进制兼容（drop-in）** | 用户的 `.so`/可执行文件不换，直接替换系统库跑 | 最难，要求 ABI 完全一致 |
| **源码级重编译** | 用户源码基本不改，用兼容层的头文件/库重新编译链接 | 可行，改动小 |

**本项目明确选择"源码级重编译"**：不要求完整 CUDA 生态的二进制 drop-in，只要求用户重编译后能跑。这一选择大幅降低了 ABI 兼容压力，但仍需保证**函数签名、结构体布局、错误码、版本语义**一致。

**ABI 场景边界**：P0 只要求宿主和用户使用授权头文件与 shim 源码重编译，不承诺现有 FFmpeg 等预编译二进制直接替换运行。需要注意，即使 FFmpeg 自身被重新编译，其 CUVID/CUDA 路径仍通过 `dlopen/dlsym` 加载 shim，因此固定 SONAME、必需符号、函数签名、回调和相关结构体布局仍须与构建时锁定的 nv-codec-headers 一致。该要求不等于完整 CUDA 二进制 drop-in。

### 1.4 项目目标与已确认约束

当前交付聚焦两个 CUDA SDK 领域及必要的内部支撑：

| 领域 | NVIDIA 对应物 | 当前承诺边界 |
|---|---|---|
| **NVDEC/CUVID** | `libnvcuvid.so.1`（CUVID 解码 + parser） | 约定清单内的 H.264/NV12 解码接口，不承诺完整 CUVID |
| **NPP** | `libnpp*.so.11` | FFmpeg 7.1 实际调用的 3 个函数及必要 nppc，不承诺完整 11 子库 |
| **libcuda 内部支撑** | `libcuda.so.1` | 仅提供 FFmpeg 解码和 NPP 联合链路必需的符号与功能子集，不作为独立完整兼容产品 |

已确认约束：

| 约束 | 结论 |
|---|---|
| 兼容层级 | **源码级重编译**，不要求现有二进制 drop-in |
| 平台与 OS | GFX ISA，全面支持 AMD 私有 HSA API；AMD 验证机为 Ubuntu 24.04.2 + RX 7900 XTX |
| 实施节奏 | **分批实施**；P0 先以 FFmpeg 为宿主，用户应用移至 P1 |
| FFmpeg | 官方源码自编译 **7.1**；P0 只记录版本号和构建配置，不声称 Git commit 级复现 |
| nv-codec-headers | commit `610325d019ad75248aa6e7abe0f4909d2ac6bc69`（`n12.1.14.1-1-g610325d`） |
| NPP ABI/参考 | **CUDA/NPP 11.5.1.107**，SONAME=`libnpp*.so.11`；NVIDIA RTX 4060 为参考环境 |
| NPP 法律路线 | **路线 A**：shim 只产 `.so`，构建/用户使用另行提供的授权 CUDA SDK 头文件，发布物不含专有头文件 |
| parser 复用 | 复用 FFmpeg 内部解析对象 + 自写 VAAPI 填充；静态对象采用独立 LGPL 干净构建 |
| 当前非目标 | NVENC/编码、HIPIFY 用户 kernel、完整 CUDA/NPP、其他 codec/格式及未测用户应用 |

### 1.5 典型目标场景与关键技术问题

当前交付的目标数据通路：

```
CUVID/NVDEC 解码 → NPP 图像处理 → HIP 侧读取或使用
```

这条通路横跨**两套驱动栈**：Mesa（管视频解码）与 ROCm/HIP（管 NPP 和后续计算）。于是引出本方案最核心的两个技术问题：

1. **零拷贝**：解码出的图像在 Mesa 管理的显存里，NPP/CUDA 计算在 HIP 管理的显存里，两套栈之间的数据能否**不经过 CPU 拷贝**直接共享？
2. **同步**：视频引擎和计算引擎是两套独立硬件队列，跨栈共享内存后，**谁先谁后、何时回收**如何保证正确？

这两个问题是方案成立与否的关键，也是本方案投入大量真机验证的原因。

### 1.6 P0 交付范围与验收基线（已确认）

| 维度 | P0 已确认范围 | 边界/验收口径 |
|---|---|---|
| OS/平台 | Ubuntu 24.04；GFX ISA，支持 AMD 私有 HSA API | AMD 验证机具体环境见 §4.19；其他组合不自动视为受支持 |
| 兼容方式 | 源码级重编译 | 不承诺现有预编译二进制直接运行；FFmpeg 内部动态加载符号面仍须满足 |
| 验证宿主 | 官方源码自编译 FFmpeg 7.1 | P0 仅记录版本号和构建配置；用户应用及其 NPP 清单移至 P1 |
| 解码格式 | H.264、4:2:0、8-bit、NV12 | 不扩展至其他 codec、位深或色度格式 |
| 码流能力 | I/P/B、DPB、crop、seek/flush、动态分辨率、长期参考 | 场/MBAFF 遇到时明确返回不支持；不静默输出错误帧 |
| FFmpeg 路径 | `h264_cuvid` 与 `h264_nvdec` | `h264_nvdec` 验证零拷贝 HIP 读取；`h264_cuvid` 可包含设备内拷贝，单独记录 |
| CUVID API | 22 个动态加载符号形态；FFmpeg 直接使用的 9 个功能接口 | 清单见 `docs/architecture/api-coverage.md`；source 等未使用功能不得伪成功 |
| NPP API | `nppiResizeSqrPixel_8u_C1R`、`nppiYCbCr420_8u_P2P3R`、`nppiYCbCr420_8u_P3P2R` 及必要 nppc | CUDA/NPP 11.5 ABI；与 NVIDIA 11.5.1.107 参考输出逐字节一致 |
| libcuda | FFmpeg 解码与 NPP 联合链路所需内部支撑 | 不作为独立完整 CUDA driver API 交付 |
| 数据通路 | LINEAR surface + DMA-BUF + HIP 导入，同进程 | 运行时校验 plane、pitch、offset、modifier 和 size；map 成功不等于像素正确 |
| 同步 | `vaSyncSurface` + HIP 同步的主机同步基线 | DRM 异步 fence 不属于 P0 |
| 功能场景 | 1080p、4K；基础解码、seek/flush、crop/动态分辨率、1 路和 8 路 FFmpeg 进程 | 保留 320×240 快速回归；输出逐帧校验 |
| 稳定性 | 1 路和 8 路分别运行 30 分钟 | 无崩溃、死锁、非法访问或持续资源泄漏，退出后资源回到基线 |
| 性能 | 记录吞吐、p50/p95/p99 延迟、CPU/显存/fd 等 | **P0 只记录不设通过阈值，不得宣称性能达标** |

#### 当前非目标

- NVENC、视频编码及 HIP→VAAPI 编码链路。
- HEVC、AV1、VP9、MPEG-2、P010、10/12-bit、4:2:2、4:4:4。
- 完整 CUDA 二进制 drop-in、完整 driver/runtime、完整 CUVID 或完整 NPP。
- 任意用户应用、任意 CUDA kernel 的 HIPIFY 自动翻译，以及用户应用的 NPP 调用覆盖。
- 场/MBAFF、DRM 异步 fence、Vulkan Video、多 GPU 和跨进程 DMA-BUF 传递。
- 以 P0 结果替代生产性能、长稳、安全、运维和实际交付平台验收。

### 1.7 最小完整流程验证的阶段通过条件

| 阶段 | 必交付证据 | 通过标准 | 未通过处理 |
|---|---|---|---|
| **第 0 步：范围与 AMD 底层能力确认** | §1.6 评审基线；AMD 验证机 HIP/VAAPI 共存、DMA-BUF、LINEAR NV12 和实验 C/E 记录 | 范围与验收口径已确认；实验 C/E 已在 AMD 验证机重跑通过。**本步已通过，但不代表 P0 整体通过** | 后续实际交付平台变化时须重新执行能力门禁 |
| **第 1A 步：真实解码→HIP 读底层数据通路** | 同进程 libva 真实解码→LINEAR surface→DMA-BUF→HIP 读取的测试程序与日志 | 多帧 NV12 各 plane 的 HIP 读取结果，与软解参考逐字节一致；无 fd/内存泄漏及非法访问。**已由实验 C 在 AMD 验证机闭合** | 底层通路已通过；不得据此替代第 1B 步的 CUVID shim 与宿主验收 |
| **第 1B 步：CUVID shim 与 FFmpeg 集成** | 真实 CUVID decoder/parser/map/unmap 路径，以及 FFmpeg `h264_cuvid`、`h264_nvdec` 双路径测试程序与日志 | 两条路径分别完成多帧像素、帧数、显示顺序、pitch、flush 和资源检查；`h264_nvdec` 另须证明 HIP 直接读取的像素正确 | **进行中**；parser 解析链路已通过，真实 decode/map 像素及 FFmpeg 双路径待验证，不以符号导出或 parser 回调通过代替本步通过 |
| **第 2 步：CUVID→NPP 联合业务链路** | 在第 1B 步基础上，将 CUVID 解码输出接入 FFmpeg 实际使用的 3 个 NPP P0 函数及必要 nppc 接口，保存各阶段输出与日志 | 解码结果正确；NPP 与 NVIDIA NPP 11.5.1.107 参考输出逐字节一致；ROI、step、plane、stream 和资源生命周期符合范围要求 | **待完成**；不扩展编码、其他 codec、其他 NPP 函数或用户应用承诺 |

补充要求：

1. 第 1A、1B、2 步必须校验实际像素，不得以句柄创建、import/map 成功或进程退出码代替。
2. 测试须覆盖参考帧复用、surface 回收及正常 flush；P0 覆盖 1080p/4K、1 路和 8 路 FFmpeg 进程，并分别执行 30 分钟资源稳定性检查。
3. 主机同步是第 1A、1B、2 步的基线；异步 fence 独立验证，不阻塞上述功能检查。
4. 对声明零拷贝的 `h264_nvdec` 路径，若只能退回 CPU 拷贝完成链路，应标记为零拷贝目标未通过；`h264_cuvid` 自身的设备内拷贝路径单独记录，不混报为零拷贝证据。

---

## 二、总体架构：双轨并行

针对 §1.1 的三层依赖，兼容策略自然分成两条**互补（非互斥）**的路线：

```
用户 CUDA 产品源码
        │
        ├─ A. 调用 NVIDIA 的库（cuvid / nvenc / NPP / cuBLAS…）
        │      └─ 轨 2：自研 shim 库，重编译时链接到自研实现
        │
        └─ B. 用户自研 CUDA kernel（__global__、cudaMalloc、kernel<<<>>>）
               └─ 轨 1：HIPIFY 把 CUDA 源码翻译成 HIP 源码，hipcc 编译
```

| 轨道 | 工具/产物 | 管什么 |
|---|---|---|
| **轨 1：HIPIFY** | HIPIFY 翻译工具 + HIP 运行时 | 用户自己写的 CUDA kernel（计算逻辑） |
| **轨 2：shim 库** | libcuda / libnvcuvid / libnvidia-encode / libnpp* | NVIDIA 库的 API（用户调用的现成库） |

### 2.1 HIPIFY 是什么、为什么它做不了 NVDEC/NVENC/NPP

**HIPIFY** 是 ROCm 提供的源码翻译工具（两个形态：`hipify-clang` 基于 Clang AST、`hipify-perl` 基于正则），把 CUDA 源码里的 API/关键字替换成 HIP 等价物。它的前提是**"存在 HIP 对应物"**：

| 调用 | HIP 对应物 | HIPIFY 能否翻译 |
|---|---|---|
| `cudaMalloc` | `hipMalloc`（HIP 运行时有） | ✅ |
| `cublasGemm` | `hipblasGemm`（ROCm 有 hipBLAS） | ✅ |
| `cuvidCreateDecoder` | **无直接 HIP 对应物**，不能仅靠 HIPIFY 完成 | ❌ |
| `nvEncEncodePicture` | **无直接 HIP 对应物**，不能仅靠 HIPIFY 完成 | ❌ |
| `nppiResize` | **无直接 HIP 对应物**，不能仅靠 HIPIFY 完成 | ❌ |

**结论**：NVDEC/NVENC/NPP 是 NVIDIA 闭源库，HIPIFY 无法把这些库本身翻译成 HIP 实现。在本项目保留 CUDA API、避免重写业务代码的约束下，需要自研 shim 库承接这些调用；用户自研 kernel 则走 HIPIFY。

### 2.2 组件清单

| 产物 | 当前覆盖接口 | 实现路径 | 状态 |
|---|---|---|---|
| `libcuda.so.1` | FFmpeg 动态加载所需符号面及实际功能子集 | 已找回的 HIP 映射实现 + 显式不支持 stub | 源码已归档至项目 `cuda_shim/`；82 符号及 47 项历史功能结果不代表完整 ABI |
| `libnvcuvid.so.1` | 22 个符号形态；目标为 H.264/NV12 | → VAAPI（libva）或 Vulkan Video（运行时可选），parser 复用 FFmpeg | 21 个必需符号及 caps 可解析、VAAPI session 通过、parser I-only 与 I/P/B 解析通过；Vulkan Video 后端已实现并接入（`CUVID_SHIM_BACKEND=vk`），I/IP/IPB 各 30 帧逐字节一致 + fd 无泄漏，FFmpeg `h264_cuvid` 宿主跑通；VAAPI 后端真实解码/map 像素及 FFmpeg 双路径未验收 |
| `libnpp*.so.11` | FFmpeg 7.1 实际使用的 3 个函数及必要 nppc | HIP kernel 重实现 | NVIDIA 11.5.1.107 参考基线已建立；AMD shim 尚未实现 |

> 注：轨 1 HIPIFY 和 NVENC 仅保留为总体技术背景，不属于当前 P0 交付组件。

### 2.3 P0 接口覆盖矩阵（已确认）

接口状态必须区分：**已导出**仅表示能找到符号；**功能已验证**表示已测调用和数据正确；**语义/ABI 已验证**还要求错误码、边界条件、回调、结构体和调用约定通过对照测试。三者不得互相替代。

| 组件 | 加载或链接要求 | P0 必需范围 | 当前状态 | 未支持接口的行为 |
|---|---|---|---|---|
| `libcuda.so.1` | FFmpeg 动态加载清单要求符号可解析；宿主按源码重编译场景构建 | FFmpeg 7.1 解码及 NPP 联合链路实际调用子集；逐项清单见 `docs/architecture/api-coverage.md` | 源码已找回并归档；82 符号和 47 项历史功能结果不构成完整语义/ABI 验收 | stub 不得返回成功或伪造结果；按 CUDA 语义返回明确的不支持错误并记录诊断 |
| `libnvcuvid.so.1` | 固定 SONAME；满足 nv-codec-headers 动态加载表 | 22 个符号形态；FFmpeg 直接功能调用 9 个；`h264_cuvid` 与 `h264_nvdec` 双路径 | 21 个必需符号及 caps 可解析；VAAPI LINEAR NV12 session 通过；parser 复用 FFmpeg 已通过 I-only 与 I/P/B 解析；Vulkan Video 后端已实现并接入（`CUVID_SHIM_BACKEND=vk`），I/IP/IPB 各 30 帧逐字节一致 + fd 无泄漏，FFmpeg `h264_cuvid` 宿主 30 帧 0 decode error；VAAPI 后端真实 decode/map 像素及 FFmpeg 双路径未验收 | 不支持的 codec、格式、场/MBAFF 和 source 功能须显式失败；不得创建“成功但不可用”的能力假象 |
| `libnpp*.so.11` | 编译时链接，SONAME=`libnpp*.so.11`；使用另行提供的授权 CUDA/NPP 11.5 头文件 | FFmpeg 7.1 `scale_npp` 的 3 个函数及必要 nppc | NVIDIA 11.5.1.107 参考输出已生成；AMD HIP kernel、stream/ROI/ABI 和联合链路尚未验证 | 未实现函数返回符合 NPP 语义的不支持错误；不得暗示完整 11 子库可用 |

P0 接口清单已按**符号及版本、宿主调用证据、函数签名/相关结构体、实现状态、测试用例和错误码映射**整理；恢复实施时须同步更新责任人和新增测试证据。占位或显式不支持可用于控制范围，但不得计作功能兼容完成。

---

## 三、初步方案与选型（详述）

项目初期评估了三条可行路线，下面分别说明原理、做法、优缺点和定位。

### 3.1 方案一：HIPIFY 翻译 + 自研 shim 映射 VAAPI（主选）

#### 原理

用户自研 kernel 交给 HIPIFY 翻译成 HIP；NVIDIA 闭源库则由我们自研一组 **shim 动态库**，在目标范围内提供与 NVIDIA 声明一致的 API 和约定的 SONAME，内部把调用**翻译到 Mesa 的 VAAPI（libva）**去完成真正的硬件编解码。

#### 各组件怎么落地

| shim | 兼容对象 | 内部实现 |
|---|---|---|
| `libcuda.so.1` | CUDA driver API | 大部分转发 HIP 运行时；视频用不到的 launch/GL 等打 stub |
| `libnvcuvid.so.1` | CUVID 解码 + parser | 底层 → `vaCreateConfig/Context/Surfaces`（VAAPI）或 Vulkan Video session（`CUVID_SHIM_BACKEND=vk`）；parser 复用 FFmpeg 解析 + 自写 VAAPI 填充 |
| `libnvidia-encode.so.1` | NVENC 函数表 | 34 项函数表 → `VAEntrypointEncSlice` 编码 |
| `libnpp*.so.11` | NPP 11 子库 | HIP kernel 逐函数重实现 |

**关键点：FFmpeg 的加载机制决定了 shim 的形态。** FFmpeg 不直接链接 libcuda，而是 `dlopen + dlsym`，并**一次性 dlsym 全部 82 个必需符号**——所以 shim 必须用固定 SONAME、并导出完整符号面，缺一个符号就加载失败。NPP 则相反，是**编译时链接**，SONAME 必须是 `libnpp*.so.11`。

#### 数据通路

```
CUVID/NVENC 调用 → shim → VAAPI(libva) → Mesa radeonsi → 视频引擎(VCN)
用户 kernel      → HIPIFY → HIP kernel → ROCm 运行时 → 计算引擎
两栈之间         → DMA-BUF 共享内存（零拷贝）
```

#### 优缺点

- **优点**：VAAPI 驱动成熟、与 CUVID 概念接近；**在前期选定的 15 组格式及测试平台上，VAAPI 未发现相对 RTX 4060 NVDEC 的能力缺口，5 项支持范围更广**（§4.1）；零拷贝可经 DMA-BUF，且两个边界的句柄链路已实测打通（§4.2）。
- **缺点**：VAAPI 的 VLD 需要 host 预解析码流，**parser 绕不开**（§4.6）；跨栈同步没有现成公共 fence 接口，需自行桥接（§4.3）。

#### 定位

**主线，近期落地。**

### 3.2 方案二：基于 Mesa 提供新 frontends

#### 原理与做法

不走"外挂 shim 翻译"的路，而是**直接在 Mesa 里新增一套 frontend**，让它原生暴露贴近 CUDA 语义的接口（类似 Mesa 已有的 `frontends/va`、`frontends/vdpau`、`frontends/omx` 的做法），从驱动内部直接对接 CUDA 兼容层。

#### 优缺点

- **优点**：理论上能做到最贴近 CUDA 的行为，少一层翻译，性能和行为可控性最好。
- **缺点**：
  - 需要**侵入 Mesa 上游**，维护一个长期 fork 或推动上游接受，成本极高；
  - 与厂商驱动**强耦合**，每换一个 Mesa 版本都要重新适配；
  - 上游接受度、法律/许可（CUDA 语义进 Mesa）都不确定。

#### 定位

**不采用。** 维护成本与不确定性远超收益。

### 3.3 方案三：编解码映射到 Vulkan Video

#### 原理

shim 把 CUVID/NVENC 调用翻译到 **Khronos Vulkan Video** 扩展族（`VK_KHR_video_queue`、`VK_KHR_video_decode/encode_queue`、`VK_KHR_video_decode_h264/h265/av1`、`VK_KHR_video_encode_h264/h265`）。Vulkan Video 把硬件编解码纳入 Vulkan 的显式内存、显式同步模型，编解码输出就是 `VkImage`，可通过 `VK_KHR_external_memory_fd` 导出 DMA-BUF 与 HIP 共享。

#### 优缺点

- **优点**：
  - **跨平台开放标准**（Linux/Windows/Android），对"基于 Mesa 写驱动的新厂商"更友好（RADV 可扩展）；
  - 显式内存/同步模型与 CUDA 的 import/export 语义**天然对齐**；
  - 内存导出是**标准 API**（`vkGetMemoryFdKHR`），比 HIP 侧绕 HSA 私有导出干净。
- **缺点**（实测确认，见 §4.4）：
  - 视频格式**只支持 OPTIMAL tiling，不支持 LINEAR**——解码输出是 tiled，HIP 不能按线性 NV12 直接读，**零拷贝受限**；
  - **本次测试的 HIP 版本不支持导入外部 semaphore**（`hipImportExternalSemaphore` 返回 operation not supported）——HIP semaphore 路径不可用，当前只能采用主机同步；其他 fence 候选机制见 §4.12。

#### 定位

**Tier 2 备选**：中长期、或有跨平台需求时再投入；启用前必须通过严格的能力探测。

### 3.4 选型结论与降级链

| 方案 | 定位 |
|---|---|
| **方案一（HIPIFY + VAAPI shim）** | **主线**，近期落地 |
| 方案二（Mesa 新 frontend） | 不采用 |
| **方案三（Vulkan Video）** | **Tier 2 备选**，中长期 / 跨平台需求时再投入 |

#### VAAPI vs Vulkan Video 选型对比总表

| 维度 | VAAPI（主线） | Vulkan Video（Tier 2 备选） | 证据 |
|---|---|---|---|
| 驱动成熟度 | 很成熟（radeonsi libva） | 较新，Mesa RADV 解码近年才落地，编码尤其滞后 | §4.1、§4.4 实测 |
| 标准 / 跨平台 | Linux 专用 | Khronos 开放标准，跨平台（Linux/Windows/Android） | §3.3 |
| 解码格式能力 | 15 组对比 0 缺口、5 项领先 | 完整（H264/HEVC，8K 上限） | §4.1、§4.4-V2 |
| 零拷贝（显存直通） | ✅ `DRM_FORMAT_MOD_LINEAR` 可出线性，HIP 直读 | ❌ 规范锁死 OPTIMAL，须 compute shader 转换 | §4.11、§4.4-V3 |
| 跨栈同步 | 主机同步已闭环；异步走 sync_file 桥接候选 | 同样受限：HIP 不认外部 semaphore，走 sync_file 桥接（同 §4.12） | §4.3、§4.12 |
| 与 CUDA 语义接近度 | 一般（独立抽象） | 更高（显式内存/同步模型与 CUDA import/export 对齐） | §3.3 |
| 头文件 / 法律 | 全开源（libva） | 全开源（Vulkan-Headers，Apache 2.0） | §4.8 对照 |
| FFmpeg 宿主 | `h264_cuvid`/`h264_nvdec`（成熟） | 宿主同 `h264_cuvid`/`h264_nvdec`；VK 后端已接回 `h264_cuvid` 并跑通（30 帧 0 error）；`h264_vulkan` 仅环境冒烟 | §4.4 澄清 |
| 维护成本 | 低（成熟稳定） | 高（分块转换、驱动年轻、编码未成熟） | §6.5 成本清单 |
| 定位 | **主线，近期落地** | **Tier 2 备选，中长期 / 跨平台需求时再投入** | §3.4 |

编码端采用多级降级：

```
Tier 1  VAAPI 编码（默认，成熟稳定）
Tier 2  Vulkan Video 编码（探测启用，可一键回退 Tier 1）
Tier 3  软件编码 x264/x265（运行库、codec、输入格式及资源满足时可配置兜底）
```

设计原则：**Vulkan Video 编码永远不是默认**；能力探测**运行时动态**（查真实能力表，不只看扩展字符串）；软件编码作为**可配置的条件兜底**，启用前检查运行库、codec、输入格式和可用资源，不承诺所有场景均可用。降级启用或失败必须记录并向上层返回明确状态，不得静默改变用户请求的编码语义。

---

## 四、风险点、验证与结论

本章按"**风险是什么 → 怎么验证 → 结论**"组织，覆盖当前已识别的主要技术风险及其实测结果；§4.14 另行汇总安全、依赖和运维交付风险。

> 实测分两批，硬件不同：
> - **第一批（能力对比）**：NVIDIA RTX 4060（NVDEC）vs AMD RX 9070（VCN5）。
> - **第二批（数据通路/同步/Vulkan）**：AMD RX 7900 XTX（RADV NAVI31），ROCm/HIP + Mesa radeonsi（libva 1.20）+ Vulkan 1.4.318。试验程序索引见 §7.3；运行前须核对源文件完整性和具体依赖，不统一承诺可直接 `make && make run`。

### 4.1 风险：VAAPI 能不能当 NVDEC 的解码后端？（格式能力是否覆盖）

**风险**：如果 VAAPI 在编码格式 × 色度采样 × 位深上有缺口，"用 VAAPI 兼容 NVDEC"从根上就不成立。

**怎么验证**：第一批做了两层对比测试——
- **FFmpeg 封装层**（`h264_cuvid` vs `-hwaccel vaapi`）：回答"该格式能不能解"。
- **SDK 直测层**（NVDEC cuvid API 直调，parser 模式）：回答纯硬件解码性能。
- **矩阵**：H.264/HEVC/AV1/VP9/MPEG-2 × 420/422/444 × 8/10/12bit，共 **15 个组合**。

**结果**：

| 统计 | 数量 |
|---|---|
| 一致 | 10 项 |
| **VAAPI 领先** | **5 项** |
| NVDEC 领先 | **0 项** |

VAAPI 领先的 5 项全是专业格式（H.264 422/444/10bit、HEVC 422、MPEG-2 422）——这是 **NVIDIA 消费级卡（RTX 4060）的硬件阉割**，VCN5 消费级 RX 9070 即支持。SDK 直测性能：HEVC 420 8bit 达 1553fps、AV1 1445fps、H.264 828fps（1080p）。

**结论**：**在本次选定的 15 组格式及 RTX 4060 / RX 9070 测试平台上，VAAPI 未发现相对 NVDEC 的能力缺口，并在 5 个专业格式上支持范围更广**，从功能支持性上支持 VAAPI 作为 NVDEC 兼容后端候选。该结果不外推至全部 NVIDIA 设备、所有 profile/level 及码流特性，也不代表目标厂商平台；后续仍需按首期目标范围补充矩阵验证。
**附带收获（对 shim 实现有参考价值）**：① 本次调用路径要求先初始化 CUDA context；② 本次实验的 sequence 回调采用 surface 数 `min+3`，生产实现仍需按 codec、参考帧和输出延迟验证；③ OutputFormat 应按 chroma/bit_depth 动态选择。

### 4.2 风险：两套驱动栈能否零拷贝共享内存？（数据通路）

**风险**：这是方案成立的最高优先问题，又分四个子问题——
- A1 HIP 与 VAAPI 能否同进程共存、经 DMA-BUF 互导内存？
- A2 HIP 内存能否导出 DMA-BUF（HIP 无标准导出 API）？
- A3 tiled/线性布局：VAAPI 默认 surface 是 tiled+DCC，HIP 需线性 NV12，能否对上？
- A4 导入成功 ≠ 真实编解码可用。

**怎么验证**：`tests/zerocopy/` 下三个实验 + 两轮定位程序。
- **实验1**：HIP+VAAPI 同进程共存。
- **实验2**：边界1 `vaExportSurfaceHandle` → `hipImportExternalMemory` → `GetMappedBuffer`。
- **实验3**：边界2 `hsa_amd_portable_export_dmabuf` → `vaCreateSurfaces(DRM_PRIME)`。

**结果与定位**：
- 实验1 ✅：共存无冲突。
- 实验2 ✅：HIP 导入并映射成功（**仅验证 import+map 链路；真实解码后的像素校验纳入 §1.7 第 1A 步**）。
- 实验3 初次 ❌：导出 fd 成功，但 `vaCreateSurfaces` 返回 `status=2 (resource allocation failed)`。
  - 定位（retry_exp3）：发现 VAAPI 自建 surface 用 **tiled+DCC** 布局（modifier `0x200000028a01f04`，320×240 NV12 占 512KB），非线性；该轮程序自身有 bug（separate-layers 误当 composed）。
  - 再定位（retry_exp3_v2）：**R3 往返自检 PASS**（证明导入代码正确），**R4 线性导入**测不同 pitch：

    | pitch | 结果 |
    |---|---|
    | 256 / 512 / 1024 | ✅ PASS |
    | 320（非 256 对齐） | ❌ FAIL |

**结论**：
- **A1/A2 成立**：两栈可共存、可互导，HIP 导出经 HSA `hsa_amd_portable_export_dmabuf`。
- **A3 当前实验失败的直接原因是 pitch 对齐**：不是"VCN 不吃线性内存"，而是原实验 pitch=320 未按本机要求 **256 对齐**。**当前 AMD 平台上边界2的句柄/资源链路可行，实现时可采用 pitch 256 对齐**。该数值不外推至其他平台；生产实现仍须依据运行时能力查询和实际导出的 plane pitch、offset、modifier 及 size 创建和导入资源。
- **A4 解码底层数据通路已闭合，CUVID/FFmpeg 兼容层未闭合**：实验 C 已证明 libva 真实解码后的像素可由 HIP 正确读取；CUVID shim、FFmpeg 双路径及 NPP 联合链路仍须独立验收。
  - **实验 C（§1.7 第 1A 步）已通过**：AMD 验证机已重跑 I-only、I/P、I/P/B 三种码流，均为 30 帧逐字节一致，并通过池复用、flush 和 fd 检查。该结果仅覆盖 libva→HIP 数据通路。
  - **CUVID shim 与 FFmpeg 双路径（§1.7 第 1B 步）仍待完成**：parser 解析链路（复用 FFmpeg）已在 AMD 验证机通过 I-only 与 I/P/B 测试；真实 decode/map 像素、错误语义和 FFmpeg 双路径仍未验收。符号导出、session 创建或 parser 回调通过均不等于本步通过。编码验证不属于当前解码调研范围。

**A4 解码向实现约束（实验 C 实测踩坑，shim `cuvidCreateDecoder`/`cuvidMapVideoFrame64` 必须遵守）**：

1. **profile 映射不能照抄 VAAPI 枚举名**：`VAProfileH264Baseline` 已废弃，radeonsi 只暴露 `VAProfileH264ConstrainedBaseline`，`profile_idc=66`（Baseline）必须映射到后者，否则 `vaCreateConfig` 失败。
2. **导出的 NV12 可能是 separate layers**：`VADRMPRIMESurfaceDescriptor.num_layers` 可能等于 2，UV 平面在 `layers[1].offset`（本例 offset=122880），而非 composed 的 `layers[0].pitch[1]`。shim 必须运行时检查 layer 结构，不能假设 composed；`vaExportSurfaceHandle` 的 SEPARATE/COMPOSED 两种形态都要处理。
3. **Y/UV 都必须按 pitch 逐行拷贝**：本例 pitch=512 ≠ 宽 320，整块 `hipMemcpy` 会错位；`cuvidMapVideoFrame64` 返回的 plane 指针/pitch 必须逐行拷贝，不能一次性搬整块。

**风险**：视频引擎与计算引擎是两套独立队列，共享 DMA-BUF 后**不会自动排序**；且 DPB 参考、输出映射、编码输入三种引用的释放时机不同，处理不好会花屏、UAF、死锁或泄漏。

**怎么验证**：`tests/sync/verify_sync`，S1–S8（不校验像素，只验证顺序与资源管理）。
- S1 解码后 HIP 读前 `vaSyncSurface`；S2 HIP 后编码前 `hipEventSynchronize`；S3 surface 池复用；S4 DPB 生命周期（I-P-P-B）；S5 flush/seek；S6 pending 时 destroy；S7 多线程 push/pop；S8 1000 帧长稳查 fd 泄漏。

**结果**：S1–S8 **全部 PASS**；S8 fd 数初始=10、最终=10，无泄漏。

**结论**：
- **主机侧同步基线可行**：`vaSyncSurface` + `hipEventSynchronize` 能保证顺序；资源回收需区分 DPB/映射/编码三种引用。
- **异步跨栈同步形成候选路线**（§4.12）：本次测试的 HIP 版本不支持外部 semaphore；DRM syncobj/sync_file 基础原语实测可用，但 VAAPI 视频引擎与 ROCm 计算引擎之间的真实 fence 传递和排序尚未验证。

### 4.4 风险：Vulkan Video 能否作为编解码后端？（后端选型）

**风险**：方案三是否成立，取决于 Vulkan Video 的队列、codec 能力、格式/tiling、会话对象、以及跨 HIP 的内存与 semaphore 是否真正可用。

**怎么验证**：两个程序——
- 基础版 `tests/vulkan-video/basic`：扩展枚举 + 普通 RGBA 图像的 Vulkan→HIP 零拷贝。
- 完整版 `tests/vulkan-video/full`：V1 队列、V2 codec 能力、V3 格式、V4 会话对象、V6 外部信号量。

**结果**：
- 基础版：默认配置下 9 个 `VK_KHR_video_*` 扩展枚举"不支持"，但普通 RGBA 图像 **Vulkan→HIP 零拷贝实测 PASS**。
- 完整版：

  | 验证项 | 结果 | 关键发现 |
  |---|---|---|
  | V1 视频队列 | ✅ | DECODE/ENCODE 队列族**默认就可用**，无需 RADV_PERFTEST |
  | V2 编解码能力 | ✅ | H264 解码 4096×4096/DPB17/DISTINCT；HEVC 8192×4352；H264 编码 CBR/VBR/CQP |
  | V3 视频格式 | ⚠️ | 支持 NV12，但**只支持 OPTIMAL tiling，不支持 LINEAR** |
  | V4 会话对象 | ✅ | 会话可创建、内存需求可查、可销毁 |
  | V6 外部信号量 | ❌ | Vulkan 可导出 fd，但 **`hipImportExternalSemaphore` = operation not supported** |

**结论**：
- Vulkan Video **编解码能力完整**，可作后端候选；
- **零拷贝受限**：OPTIMAL/tiled 输出，HIP 不能按线性直接读，需额外 GPU 转换；
- **异步同步受限**：本次测试的 HIP 版本不支持外部 semaphore，当前只能采用主机同步；其他 fence 候选机制见 §4.12；
- 综合定为 **Tier 2 备选**，VAAPI 仍为主线。

**结论补充（P0 端到端闭环，2026-11-06）**：V4/V6 之后，P0 要求的"Vulkan 解码 → 拷贝 → 导出 → HIP 读"整条链路已落地并实测**全部通过**（`tests/zerocopy/verify_vk_decode_to_hip`，AMD RX 7900 XTX）：I-only / I/P / I/P+B 三种码流各 30 帧，HIP 读回像素与软件解码参考**逐字节一致**，fd 计数与显存无泄漏。关键实现点：① decode 队列族无 TRANSFER 能力，`vkCmdCopyImageToBuffer` 须在 transfer 队列跑，两队列间用 semaphore（sync2）桥接，image 采用 CONCURRENT 共享；② RADV（VCN<5）要求 DPB 与 output 图像分离，DPB 图只带 `VIDEO_DECODE_DPB` 用法；③ `pSetupReferenceSlot` 每帧必填（含 B 帧，RADV 硬性要求非空），非参考帧用临时 slot；④ 驱动内部按 FrameNum+POC 自行排参考列表，应用**无需手写 RefPicList0/1 重排**，但需自行维护 active 参考集合（MMCO+滑动窗口+IDR 清空）以填 `pReferenceSlots`；⑤ 码流 buffer 按 Annex-B（含 start code）单 slice 复用，`srcBufferRange` 对齐 `minBitstreamBufferSizeAlignment`。该结论使 Vulkan Video 从"能力可作后端"推进到"解码数据通路已证实可用"，但**仍未接回 CUVID shim / FFmpeg 宿主**，故 Tier 2 备选定位不变。

**结论补充（CUVID shim Vulkan 后端接入宿主，2026-11-07）**：上一结论"未接回宿主"已闭合——`cuda_shim/` 的 `libnvcuvid.so.1` 新增 **Vulkan Video 解码后端**（`vulkan_h264_decoder.{h,cpp}`），与 VAAPI 后端并列于 `DecoderBackend` 接口之下，用环境变量 `CUVID_SHIM_BACKEND=vk` 运行时切换（默认仍 VAAPI）。SPS/PPS 由 shim 的 `CuvidParser` 解析后经进程级 `VkSequenceCache`（前置 start code）交给后端，后端在 SPS/PPS 就绪后懒建 Vulkan session。AMD RX 7900 XTX 实测：I/IP/IPB 三种码流各 30 帧逐字节一致 + fd 无泄漏；FFmpeg `h264_cuvid` 宿主对 I/P/B 码流 30 帧 0 decode error 跑通。仍属 **Tier 2 备选**，未升级为主线：默认后端仍是 VAAPI；Vulkan 路径的 `h264_nvdec` 零拷贝、多流/seek/flush、crop、动态分辨率及 HEVC/VP9/AV1 均未在此后端验收；`cuvidReconfigureDecoder` 仍为 stub。

**结论补充（V3 OPTIMAL tiling 的结构性分析，2026-11-05）**：V3 只报告 OPTIMAL、不支持 LINEAR，**不是 RADV bug、不是"等新驱动可修"，而是两层硬约束叠加，短期不会消失**：

1. **规范层**：Vulkan Video 规范要求解码输出图与参考帧（DPB）必须以 `VK_IMAGE_TILING_OPTIMAL` 创建，API 无"请求 LINEAR"字段。因此 RADV 能力查询只会报 `imageTiling=OPTIMAL`。
2. **寻址层**：tiled（含 swizzle、可能带 DCC）的"解"只有纹理单元在图像采样指令下自动完成；HIP/CUDA 的普通设备指针是**平坦线性地址**，硬件不套 swizzle，故 `hipImportExternalMemory` 导入 tiled 显存后按线性读**必然错位**。external memory 只解决"指向同一块物理内存"，不改变内存内数据排列——两个问题是正交的。此外 OPTIMAL 布局对上层**不透明**，VK 不暴露 tiling 规则，即便想用纹理方式读也无法配置纹理描述符。

**因此三条候选路线取舍**：

| 路线 | 结论 |
|---|---|
| 改 Mesa 出 LINEAR | **不推荐**：违反 Vulkan Video 规范，validation layer 拒绝、进不了上游，只能永久维护私有 fork |
| 改 `hipImportExternalMemory` 读 tiled | **走不通**：tiling 在硬件寻址层、且 VK 布局不透明，非接口层可解；且 CUDA 语义本无 tiled 概念，改了也用不上 |
| 接受一次 tiled→linear 转换 | **推荐**：用 `vkCmdCopyImageToBuffer`（硬件拷贝引擎）把解码 image 直接拷到线性 `VkBuffer`，再经 `vkGetMemoryFdKHR`→`hipImportExternalMemory` 交给 HIP；合规、可上游、无需 fork，且无需自写 compute shader |

**转换实现要点（勘误后，2026-11-05）**：`vkCmdCopyImageToBuffer` 对源 image 的 tiling **没有 LINEAR 限制，OPTIMAL 也可用**——它要求的是源处于 `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` *布局/状态*（与 tiling 是两回事）。满足以下条件即可直接拷贝，无需 compute shader：
1. image 创建时带 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`；
2. 解码完成后经 barrier 将 layout 转到 `TRANSFER_SRC_OPTIMAL`；
3. 按 plane 分别设置复制区域（`VkBufferImageCopy`），处理 pitch 与 offset；
4. 拷贝完成依赖（semaphore/fence）传递给 HIP 侧等待。

NV12 两 plane 分别拷贝到 linear `VkBuffer`（HIP 拿到即线性 buffer）；linear pitch 沿用 §4.2 的 256 对齐 + UV offset=pitch×H 结论。转换成本为每帧一次 GPU 拷贝（4K 约几十~一两百微秒，须在目标平台量化，见 §4.17 组 C）。参考实现：FFmpeg `hwcontext_vulkan.c` 的 `vulkan_transfer_frame()`（按平面设复制区 + barrier）、GStreamer `vkdownload.c`（转 TRANSFER_SRC_OPTIMAL + 多平面）、AMD `vulkan_interop`（可导出 buffer → fd → `hipImportExternalMemory`/`hipExternalMemoryGetMappedBuffer`）。

**改 ROCm 的投入排序（针对"可改 ROCm 源码"假设）**：tiling 一条价值低（转换成本仍在、CUDA 用不上）；真正值得改的是**异步同步**——接 `VK_KHR_external_fence_fd`→sync_file→syncobj→HIP queue wait fence，解锁两引擎全异步流水线（详见 §4.12 结论补充）。

**验收宿主澄清（VK 作为后端时）**：Vulkan Video 只是把 shim 内部的解码翻译目标从 VAAPI 换成 VK，`libnvcuvid.so.1` 的符号面与宿主路径**不变**——验收宿主仍是 FFmpeg `h264_cuvid` / `h264_nvdec`（对标 §1.6「FFmpeg 路径」），**并非** `h264_vulkan`。`h264_vulkan` 是 FFmpeg 原生 VK 解码器，绕过 shim 直接调 VK API，仅可作环境冒烟（确认 RADV 的 VK 解码可用）或 shim VK 后端实现的语义参考，不能作为兼容层验收证据。据此，shim 应抽象一层「解码后端」（后端1=VAAPI、后端2=Vulkan Video，运行时可选），宿主与符号面保持一致，仅内部后端可换——与 §3.4 的编码降级链同构。**该抽象已落地**：`cuda_shim/` 的 `DecoderBackend` 接口 + `backend_dispatch` 工厂实现运行时切换（`CUVID_SHIM_BACKEND=vk`），Vulkan 后端已接回 `h264_cuvid` 宿主跑通（§4.4 结论补充 2026-11-07）。

### 4.5 风险：ABI / 符号面能否支撑源码重编译及动态加载场景？（ABI）

**风险**：FFmpeg dynlink 一次性 dlsym **82 个必需 libcuda 符号**，缺一个即加载失败；NPP 编译时链接要求 SONAME=`libnpp*.so.11`；CUDA 错误码/版本/Compute Capability 若直接透传 ROCm 值会误导应用。若支持不重编译的 FFmpeg 宿主程序，仅导出同名符号还不够，还须验证实际使用的函数签名、回调约定、结构体布局和 NVENC 函数表 ABI。

**怎么验证**：`cuda_shim/` 的 `verify_symbols` 模拟 FFmpeg `cuda_load_functions` 的 dlsym 清单逐一检查；并做功能冒烟。

**结果**：82 个必需符号**全部导出正常**；HIP 运行时链接正确；冒烟显示错误码映射链路正确（本机无 AMD 卡时 cuInit→100 NO_DEVICE）。

**结论**：
- **当前动态加载符号清单已闭合**：82 必需符号已齐，`libnvcuvid` 22、`libnvidia-encode` 2 入口 + 34 函数表的形态已明确。符号清单闭合不等于完整 ABI 闭合，后者仍需按目标场景验证。
- **注意（遗留）**：符号导出 ≠ 功能完成；CUDA **版本号、Compute Capability、错误码需虚拟化**（如 `CUDA_ERROR_NOT_SUPPORTED`=**801** 而非 201，`hipDriverGetVersion` 返回值不能直接当 CUDA 版本）。

**功能真实性实测（AMD 机器，`make check_func`）**：`test_func_full.cpp` 覆盖设备/上下文/内存/2D 拷贝/流事件/primary ctx，带数据校验，**47 项 PASS / 0 FAIL**。

实测中暴露并修复了 4 类真实缺陷（这些是"符号面通"绝对发现不了的）：

| # | 问题 | 根因 | 修复 |
|---|---|---|---|
| 1 | `cuDriverGetVersion` 返回 `70253211` | 直接透传 ROCm 版本 | 返回 CUDA 语义 **12000** |
| 2 | `cuCtxSynchronize` 返回 NotSupported | `hipCtxSynchronize` 在 ROCm 7.x 已废弃 | 改用 `hipDeviceSynchronize()` |
| 3 | `cuDevicePrimaryCtxGetState` 返回 INVALID_DEVICE | CUDA 真实签名第一个参数是**按值** `CUdevice`，原声明成指针 | 改按值传 |
| 4 | 构建/链接问题 | 缺 `libcuda.so` 开发软链；ROCm 路径写死 7.2.0（实际 7.2.4）；test 目标未依赖库 | Makefile 自动探测 ROCm、建软链、加依赖 |

**结论**：libcuda.so.1 **当前已测功能子集可用**（数据校验通过），可作为 cuvid/NVENC shim 的继续开发基础，但不能据此认定完整 driver API 或完整 ABI 已兼容。同时印证了一个重要原则——**符号面验证、功能验证和 ABI 验证是三件事**，必须分别执行。

### 4.6 风险：parser/DPB 复杂度是否被低估？（解码核心工作量）

**风险**：CUVID 解码路径通常由 parser 组件完成码流解析，`CUVIDPICPARAMS` 同时携带解析后的图像参数和压缩码流；VAAPI VLD 则要求 shim 在 host 侧完成解析，并填充 `VAPictureParameterBuffer*` + `VASliceParameterBuffer*`。**parser 绕不开**，且不只是 SPS/PPS——还有 slice header、参考表、POC、MMCO、DPB 状态机、B 帧重排。

**怎么验证**：直接核实 FFmpeg 源码的可复用性。
- `av_parser_parse2` 公开 API **只做帧切分，不暴露 SPS/PPS**；
- SPS/PPS 解析在 `h264_ps.c` 的 `ff_h264_decode_seq_parameter_set` 等（**非 static，可静态链接**）；
- VAAPI 填充在 `vaapi_h264.c`，但其 `fill_vaapi_pic`/`fill_vaapi_ReferenceFrames` **全是 static 且依赖 H264Context**，不能直接提取。

**结论**：
- **复用路径确定**：静态链接 `libavcodec.a` 做解析 + **自写 VAAPI 填充**（参考 `vaapi_h264.c` 字段映射移植），不能"直接提取 vaapi_h264.c"。
- **工作量定调**：parser/DPB 是整个 nvdec 兼容里**工作量最大**的部分，需单独立项、分阶段（H264 I-only → P/B → 场/长期参考）。
- **法律注意**：复用 FFmpeg 须用干净构建（`--disable-gpl --disable-nonfree --enable-pic`），本机现有 `libavcodec.a` 是 `CONFIG_GPL=1 / CONFIG_NONFREE=1`，不能直接当 LGPL 依赖分发。

**可行性实测（本机，`make parser_reuse`）**：`tests/parser/verify_parser_reuse.cpp` 用真实 H264 码流验证：

| 步骤 | 结果 |
|---|---|
| 静态链接 `ff_h2645_packet_split` / `ff_h264_decode_seq_parameter_set` / `ff_h264_decode_picture_parameter_set` | ✅ 全部可链接调用 |
| 切 NAL（91 个） | ✅ |
| 解析 SPS（30 个），字段正确 | ✅ profile=66(Baseline)、320×240、8bit、4:2:0 |
| 解析 PPS（30 个） | ✅ |
| 自填 `VAPictureParameterBufferH264` | ✅ |

**实测踩到的 4 个关键坑（直接给 shim 实现参考）**：
1. **不能链整个 `libavcodec.a`**：会拖入所有编解码器的外部依赖（aom/zlib/…）导致链接失败。**只提取 8 个 .o**：`h2645_parse/h264_ps/h2645_vui/h2645data/h264data/golomb/mathtables/refstruct`。
2. **`AVCodecContext*` 不能传 NULL**：内部会解引用 `avctx->codec_tag`/`flags2`，必段错误。栈上 zero-init 一个即可（避免链 `avcodec.o` 拖入大量依赖）。
3. **SPS/PPS 要从 `sps_list[]`/`pps_list[]` 取**，不是 `ps.sps`/`ps.pps`——后者只在 PPS/slice 解析时才赋值（初版踩坑：解析成功却取不到）。
4. **`PPS` 字段名与直觉不同**：`cabac`（非 entropy_coding_mode_flag）、`deblocking_filter_parameters_present`（非 deblocking_filter_control_present_flag）。

**结论**：**parser 复用路线技术可行**——解析层用 FFmpeg 现成代码，填充层自写。这是第 1B 步 CUVID shim 开发的技术起点，不等同于 shim 已完成。

**VAAPI config/context 创建部分（已在 AMD 机器随实验 C 验证）**：`verify_decode_to_hip` 实测 `vaCreateConfig`（ConstrainedBaseline）→ `vaCreateContext`（LINEAR surface）→ `vaBeginPicture`/`vaRenderPicture`/`vaEndPicture` → `vaSyncSurface` 全链路成功，产出可被 HIP 逐字节读取的像素。实验 C 的 3 个实现约束（profile 映射、separate layers、pitch 逐行）已汇总到 §4.2。

**补充第 5 个坑（本机 parser 复现测试踩到，供 shim 构建参考）**：编译 FFmpeg 内部头时必须先 `#define HAVE_AV_CONFIG_H 1`，否则 `libavutil/common.h` 不会 include `internal.h`，`golomb.h` 里的 `SUINT` 未定义导致编译失败（FFmpeg 内部编译由 `config.h` 提供该宏，外部集成需显式补上）。

**P/B 帧 DPB 参考复用已随实验 C 闭合（§7.2 P1 核心）**：`verify_decode_to_hip` 在 AMD 平台实测三种码流全部 30 帧逐字节一致——I-only（11/11）、I/P 帧（12/12）、**I/P/B 帧（12/12，1 I + 10 P + 19 B，含显示序重排）**。验证了：
- **复用层**：`ff_h264_init_poc`（POC，含 poc_type=0/2 双路径）+ `ff_h264_parse_ref_count`（ref count 语法），只需在 8 个 `.o` 基础上加 `h264_parse.o`。
- **手写层**（FFmpeg 对应实现耦合 `H264Context` 不可轻量复用）：最小 DPB 状态机（MMCO_SHORT2UNUSED + 滑动窗口 + IDR 清空 + surface 池复用）+ RefPicList0/1 构建（P 帧按 frame_num 降序，B 帧按 POC 排序）+ list0/1 重排 + 显示序重排（解码序收集 → 按 POC 稳定排序 → 与显示序参考比对）。
- **关键坑**：
  ① `h264_vaapi` 在 `bframes=0` 时强制 `poc_type=2`（`vaapi_encode_h264.c:378`），`bframes>0` 时走 `poc_type=0`；② 目标机无 libx264，用 `h264_vaapi -profile:v 77` 生成 Main profile 码流；③ **`log2_max_pic_order_cnt_lsb_minus4` 少减 4**：FFmpeg `sps->log2_max_poc_lsb` 存完整位数（`h264_ps.c` `t+4`），VAAPI 字段名含 minus4 且硬件按 `值+4` 解读，直接传完整值会让硬件按多 4 bit 读 POC LSB，slice 数据整体错位——poc_type=0 下连 IDR 帧都错；④ **滑动窗口误删参考帧**：滑动窗口（H.264 §8.2.5.1）只在 `nal_ref_idc != 0` 的参考帧解码后执行，旧代码对 B 帧（非参考帧）也执行了，导致首个 B 帧误删 DPB 最旧 IDR，后续 B 帧缺参考（差异 5-15%）。
- **边界结论**：I/P/B 帧 + DPB + 双参考列表 + 显示序重排已证明正确；长期参考、场/MBAFF、crop、seek 仍需独立实现验证。

### 4.7 风险：NVENC 语义能否对齐？（编码核心工作量）

**风险**：NVENC 不只是"调一次编码"，还有 SPS/PPS 生成、输出延迟、时间戳重排、EOS flush、码率控制/GOP 模型差异。

**怎么验证**：核实 FFmpeg `nvenc.c` 的真实调用面与 VAAPI 能力。
- FFmpeg 基础路径就会调 `nvEncOpenEncodeSessionEx`、`GetEncodePresetConfig(Ex)`、`InitializeEncoder`、**`SetIOCudaStreams`**、`CreateBitstreamBuffer`、**`GetSequenceParams`**、register/map/unmap resource、EOS 时再次 `EncodePicture`、`GetLastErrorString`。
- B 帧/lookahead 下 `NV_ENC_ERR_NEED_MORE_INPUT` 是**正常态**而非失败。

**结论**：
- **P0 范围比预想宽**：`SetIOCudaStreams`、`GetSequenceParams` 等不能简单归入"其余未实现"。
- **SPS/PPS 需自理**：VAAPI 不一定自动产出 Annex-B 头，需查 `VAConfigAttribEncPackedHeaders`，必要时 shim 自行生成。
  - **已被实验 D 实证**：`verify_hip_to_encode` 首次带 `VAConfigAttribEncPackedHeaders` 建 config 失败后静默回退丢弃该属性，编码 buffer 只剩 slice NAL（仅 1 个起始码），无法被独立解码器解析。**教训**：shim 编码路径必须「先 `vaGetConfigAttributes` 查 `VAConfigAttribEncPackedHeaders` 真实能力 → 再建 config」，不支持时走 §4.7 的「shim 自生成 SPS/PPS」分支，**不得静默回退**（违反 §3.4「禁止静默改变编码语义」）。
- **必须处理输出延迟与 EOS 重排**，不能假设一帧输入立即出一帧。
- 编码后端按 Tier 降级（§3.4）。

### 4.8 风险：NPP 的法律与函数语义能否兼容？（NPP）

**风险**：NPP 头文件是**专有许可**，不能复制再分发；函数语义（ROI/scratch/_Host/pointer mode/stream context）需精确对齐，且 NPP 逐像素数值行为要一致。

**怎么验证**：核实 FFmpeg `vf_scale_npp.c` 的真实依赖 + 头文件许可。
- FFmpeg `scale_npp` 实际只用 **3 个 NPP 函数**：`nppiResizeSqrPixel_8u_C1R`、`nppiYCbCr420_8u_P2P3R`、`nppiYCbCr420_8u_P3P2R`。
- NPP 头文件标有 PROPRIETARY，不能像 nv-codec-headers（MIT）那样直接包含。

**结论与 P0 证据**：
- **法律走路线 A**：shim 只产 `.so`，构建时使用另行提供的授权 CUDA/NPP 11.5 头文件，产物不含专有头文件。ABI 仍须验证调用约定、结构体布局、packing、版本宏和编译器 ABI。
- **P0 函数清单已由 FFmpeg 7.1 静态扫描确认**：仅上述 3 个 NPP 函数及必要 nppc，不再以用户应用的未知调用扩大 P0。
- **NVIDIA 参考基线已建立**：RTX 4060、驱动 595.99.02、CUDA Driver API 13020、CUDA Runtime/NPP 11.5（NPP 11.5.1.107）。`tests/parser/npp_p0_reference.cpp` 已执行三个函数，P2P3→P3P2 往返逐字节一致，参考文件及 SHA256 归档于 `docs/evidence/results/npp-nvidia-11.5/`。
- **AMD 实现尚未验收**：NPP 主体计划用 HIP kernel 重实现；后续 AMD 输出必须与 NVIDIA 参考逐字节一致，并补齐 ROI、step、padding、stream 和错误语义矩阵。

### 4.9 风险：AMD 实测结论能否移植到实际交付平台？（可移植性）

**风险**：私有 HSA API 可用性曾是关键假设。当前平台约束已明确为 **GFX ISA 且全面支持 AMD 私有 HSA API**，因此不再把“私有接口是否存在”列为 P0 阻塞假设；但 VCN、tiling/DCC、surface、fence、驱动版本和性能行为仍不能跨平台直接外推。

**当前验证**：
- AMD 验证机已按 Ubuntu 24.04.2 环境重跑实验 E 和实验 C，结果均通过。
- 实验 C 只证明 libva→HIP 解码数据通路；实验 E 只证明能力探测项，不替代 CUVID/FFmpeg 或实际交付验收。

**结论**：
- AMD 结果可定位为当前架构与 AMD 验证环境的 PoC，不再因私有 HSA 接口缺失而否决主线。
- 若后续实际交付硬件、驱动或软件栈变化，仍须重新验证 HIP/VAAPI 共存、DMA-BUF、LINEAR surface、真实像素、主机同步及性能；P010 等未纳入 P0 的格式不作承诺。

### 4.10 风险：解码/编码内容链路是否真正正确？（闭环验证）

**风险**：前面 §4.2 只证明"句柄能互导"，没证明"导过去的数据内容是对的"。需要确认 VAAPI 解码出的像素、以及经 VAAPI 编码再解码回来的像素，都与参考一致——否则零拷贝毫无意义。

**怎么验证**：`tests/zerocopy/` 下两个闭环脚本（`make loop-decode` / `make loop-encode`），都用确定性输入 + 逐字节比对。

- **解码闭环**（`run_decode_loop.sh`）：
  1. 生成 H264 测试码流；
  2. **VAAPI 硬件解码** vs **软件解码** 同一帧，逐字节对比。

- **编码闭环**（`run_encode_loop.sh`）：
  1. 生成确定性 NV12 图案（代表 HIP 输出，`testsrc2` 首帧）；
  2. VAAPI `h264_vaapi`（hwupload + QP18）真实编码；
  3. 软件解码回读，与源图案逐字节对比。

**结果**（AMD RX 7900 XTX 实测）：

| 验证 | 结果 |
|---|---|
| VAAPI 硬解 vs 软解逐字节 | ✅ **完全一致**（320×240 NV12） |
| 源图案 → h264_vaapi 编码 → 解码回读 | ✅ 可完成编码并解码回读；差异字节为 **253/115200 = 0.22%** |
| 编码画质 | 输出 12002 bytes；当前仅记录字节差异占比，尚缺 PSNR/SSIM 等画质评价 |

**结论**：
- **本次 320×240 NV12 样本中，VAAPI 解码结果与软件解码逐字节一致**，证明该已测场景的内容正确性；其他 codec、格式和码流仍需按矩阵验证。
- **VAAPI 编码及解码回读链路已跑通**。0.22% 仅表示回读结果中发生变化的字节占比，不反映像素差值幅度，不能单独作为“近无损”或画质达标的依据；后续需补充 PSNR/SSIM、码率、QP 及主观画质评价。
- **仍缺的一步**：把两半拼进**同一进程**（libva 解码→线性 surface→导出 fd→HIP 读），以及反向（HIP 写→HSA 导出→VAAPI 直接编码）。**这不是脚本能做的**——dma-buf fd 绑定进程，跨进程无法传递，**这一拼接正是 shim 自身（M2/M4）的实现范围**。
- **该拼接的解码底层通路（M2）已由实验 C 完整闭合**：`verify_decode_to_hip` 已在同进程完成 libva 解码→LINEAR surface→导出 fd→HIP 读→逐帧逐字节一致，§1.7 第 1A 步通过；CUVID shim 与 FFmpeg 集成仍属第 1B 步，编码不属于当前调研范围。
- **重要发现**：Mesa 提供 `AMD_DEBUG=notiling`（全局，`si_pipe.c:71`）和 `AMD_DEBUG=novideotiling`（仅视频，`si_pipe.c:132`）两个开关，可强制 radeonsi **禁用视频 tiling、走线性布局**。这直接可用于验证"线性 vs tiled 的性能代价"，无需写代码。

### 4.11 风险：线性 surface 会不会拖慢解码？（线性 vs tiled 布局选型）

**风险**：Mesa 源码显示线性解码可能禁用 UDT（`radeon_vcn_dec.c` 的 `rvcn_dec_can_use_udt` 要求 tiling），若线性解码明显变慢，"线性直通"就不是好选择，需改走"tiled 解码 + VPP 转线性"。

**怎么验证**：`tests/zerocopy/run_linear_vs_tiled.sh`（`make linear`），对比两种模式下：
1. 导出的 DRM modifier（是否真变线性 0x0）；
2. 解码内容是否仍与软件解码逐字节一致；
3. 解码耗时差。

**结果**（AMD RX 7900 XTX，320×240）：

| 维度 | tiled（默认） | 线性（`AMD_DEBUG=notiling`） |
|---|---|---|
| 导出的 modifier | `0x200000028a01f04`（tiled+DCC） | **`0x0`（线性）** |
| 解码 vs 软件逐字节 | ✅ 一致 | ✅ 一致 |
| 解码耗时（2s 码流） | 0.15s | 0.15s（**+0.0%**） |

**结果补充（多分辨率）**：`run_multires_linear.sh`（`make multires`）在 4 个分辨率（320×240 / 640×360 / 1920×1080 / 3840×2160）下验证：

| 分辨率 | tiled 耗时 | linear 耗时 | 内容正确 |
|---|---|---|---|
| 320×240 | 0.16s | 0.15s | ✅ |
| 640×360 | 0.17s | 0.17s | ✅ |
| 1920×1080 | 0.29s | 0.29s | ✅ |
| 3840×2160 | 0.70s | 0.69s | ✅ |

**结论**：
- **当前 AMD 测试条件下，线性直通是首选验证路径**：modifier 变为线性、解码内容正确，且在本次 320×240 至 4K 测试中未观察到明显耗时回退。此前"线性可能拖慢解码（UDT 禁用）"的担心未在本轮测试中出现，但现有数据尚未记录重复次数、统计波动及多路并发结果，不能外推为普遍性能结论。
- **决策**：共享 surface 优先采用**线性直通**（解码输出即线性，直接导出给 HIP），以避免"tiled+VPP 转换"增加一次 GPU 操作；目标平台仍需复测功能与性能，不达标时回退到 tiled 解码加 GPU 转换。
- **⚠️ 生产实现注意**：上述用的是 `AMD_DEBUG` **全局 debug 开关**，不能作为生产依赖。生产应改用 **`VASurfaceAttribDRMFormatModifiers` 主动限定 `DRM_FORMAT_MOD_LINEAR`** 创建 surface，这一步需在 shim 的 `cuvidCreateDecoder` 里实现并验证（见 §7.2）。

**生产创建路径验证（当前 AMD 平台）**：`verify_modifier_linear.cpp`（`make modifier-linear`）用 `VASurfaceAttribDRMFormatModifiers=[LINEAR]` 主动创建 surface，实测：
- `vaCreateSurfaces(modifier=[LINEAR])` ✅ 成功；
- 导出 modifier = **`0x0`（DRM_FORMAT_MOD_LINEAR）**，pitch=512（Y/UV 都 512）、Y 偏移 0、UV 偏移 122880；
- `vaCreateContext` 用该线性 surface 创建成功，**可作解码输出/DPB 面**。

**→ 当前平台创建路径确认**：不依赖 `AMD_DEBUG` debug 开关，主动声明 modifier=LINEAR 可得到线性解码 surface，且 context 创建成功。实验 C 已闭合底层真实解码像素验证；CUVID shim 与 FFmpeg 集成仍按 §1.7 第 1B 步独立验收。

### 4.12 风险：异步跨栈同步能否建立？（fence 通路探测）

**风险**：视频引擎与计算引擎是两套独立队列，主机同步（`vaSyncSurface`/`hipEventSynchronize`）可提供顺序基线，但可能增加等待并限制吞吐，实际影响须按 §4.17 测量。若想全异步流水，需要"视频引擎 fence → HIP 等待 / HIP 完成 → 视频引擎等待"的 GPU 级通路。之前已确认 HIP 外部 semaphore（OpaqueFd）失败，尚未确认是否有替代。

**怎么验证**：`tests/sync/verify_semaphore_async.cpp`（`make semaphore`），探测：
1. HIP `timelineSemaphoreInteropSupported` 真实值 + `hipImportExternalSemaphore(TimelineSemaphoreFd)`；
2. DRM syncobj 能力（`DRM_CAP_SYNCOBJ`/`TIMELINE`）；
3. syncobj ↔ sync_file 双向 ioctl 通路。

**结果**（AMD RX 7900 XTX）：

| 探测项 | 结果 |
|---|---|
| `timelineSemaphoreInteropSupported` | **0**（不支持） |
| `hipImportExternalSemaphore(TimelineSemaphoreFd)` | `invalid argument`（API 在 Linux 不实现） |
| `DRM_CAP_SYNCOBJ` / `DRM_CAP_SYNCOBJ_TIMELINE` | **1 / 1**（都支持） |
| syncobj 创建 | ✅ |
| syncobj → sync_file 导出（`EXPORT_SYNC_FILE`） | ✅ fd=6 |
| sync_file → syncobj 导入（`IMPORT_SYNC_FILE`） | ✅ handle=2 |
| 导入后 WAIT(timeout=0) 立即 signal | ✅ sync_file 导入后的等待/信号原语可用 |

**结论**：
- **本次测试的 ROCm/HIP 版本不支持 Linux 外部 semaphore**（字段=0 + API 不实现），因此本方案的异步同步设计不采用 HIP 侧 semaphore；其他版本和平台仍需单独探测。
- **DRM syncobj/sync_file 基础原语实测可用**（含 timeline syncobj），为构建跨栈 fence 提供了候选机制。当前实验尚未验证 VAAPI 视频引擎与 ROCm 计算引擎之间的真实 fence 传递、执行排序和异常处理。
- **异步跨栈同步的优先候选路线**：用 DRM sync_file 桥接 VAAPI（视频引擎 fence）与 ROCm（计算引擎）。需要 Mesa 侧把 `pipe_fence` 导出为 sync_file（`si_fence.c` 有基础）+ ROCr 侧把 sync_file 接入 HIP 等待——这是**厂商协同项**，不是现成 HIP API。待端到端顺序、超时、异常恢复及性能测试通过后再确定最终路线。
- **近期策略**：保留主机同步基线（功能正确）；把"DRM sync_file 桥接异步"列为厂商/Mesa 协同开发项。

**结论补充（Vulkan Video 侧，2026-11-05）**：Vulkan Video 走异步同步不应修 `hipImportExternalSemaphore` 本身——Vulkan semaphore 导出的是 **opaque fd**（RADV 私有格式），跨栈到 ROCm 后不互认，硬修死胡同。正确做法与 VAAPI 侧一致：走 `VK_KHR_external_fence_fd` 把 `VkFence` 导出成**标准 sync_file**（非 opaque）→ 经 `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE` 导入成 amdgpu syncobj（内核已支持）→ ROCm 侧把该 syncobj 挂成 HIP queue 的 wait fence。缺的是 ROCr 用户态这一环（sync_file/syncobj → hsa_signal wait），属厂商协同项。分层：amdgpu 内核、Mesa 导出**基本不用改**；核心在 ROCr 的队列提交逻辑，需与 Mesa fence 导出语义对齐。

### 4.13 风险：10-bit (P010) 布局与零拷贝？（多格式扩展）

**风险**：NV12 8-bit 已验证线性直通，但 10-bit（P010）是 NVDEC/NVENC 真实产品大量使用的格式（HDR、专业视频），其布局（pitch/offset/每像素字节数）可能与 NV12 不同，需独立验证。

**怎么验证**：`verify_p010_linear.cpp`（`make p010`）：`VA_RT_FORMAT_YUV420_10` + modifier=[LINEAR] 创建 → 导出检查布局 → HIP 导入。

**结果**（AMD RX 7900 XTX，320×240）：

| 项 | 值 |
|---|---|
| fourcc | **P010** |
| modifier | 0x0（线性） |
| Y plane | offset=0, **pitch=768** |
| UV plane | offset=184320, pitch=768 |
| object size | 276480（每像素 2 字节） |
| HIP 导入 | ✅ 成功 |

**当前 AMD 样本的布局规律**（供实现与后续测试参考）：
- **P010 pitch = W×2 向上对齐到 256 的倍数**（320×2=640 → 768）；与 NV12"W 对齐到 256"同构，基数翻倍（16bit 容器）。
- UV 偏移 = `pitch × H`（与 NV12 同构）。
- 每像素 2 字节（10bit 存 16bit）。

**结论**：
- **当前 AMD 平台上，P010 线性 surface 的创建、导出及 HIP 导入链路已打通**；真实 P010 解码后由 HIP 读取并校验像素仍待端到端验证。
- **当前样本呈现一致的布局规律**：`pitch = 对齐256(每像素字节数 × W)`，NV12=1B/px、P010=2B/px，UV 偏移为 `pitch×H`。该规律可作为本机实现和后续测试的参考，不能替代运行时查询；shim 应使用实际导出的各 plane pitch、offset、modifier 和 size。
- **目标平台复测是交付前的硬性前提**。

### 4.14 高优先级交付风险补充

本节补充前述机制实验未覆盖的安全、依赖和运维风险。“待验证”表示尚未提供对应证据，不表示已经发现产品缺陷。各项责任角色应在第 0 步明确到具体人员或团队。

| 类别 | 风险及影响 | 当前缺口 | 必须补充的验证或措施 | 处理阶段 |
|---|---|---|---|---|
| 安全：不可信码流 | 畸形、截断或恶意构造的码流可能触发 parser 崩溃、越界或异常资源消耗 | 当前主要验证正常码流解析 | 建立畸形/截断/随机变异样本；对 parser 和 VAAPI 参数填充执行 fuzzing 及 ASan/UBSan；失败时须返回受控错误，不得崩溃或无限处理 | 第 1 步开始，第 3 步验收 |
| 安全：尺寸与资源 | 超大分辨率、异常 plane 参数或大量会话可能造成整数溢出、显存耗尽和资源争抢 | 尚未记录尺寸检查及资源配额设计 | 对宽、高、pitch、offset、size 使用可检查溢出的计算；校验 plane 边界；按上下文/会话设置 surface、显存和队列配额，并测试拒绝与释放路径 | 第 1 步开始，第 3 步验收 |
| 安全：DMA-BUF 与 fd | fd 所有权不清、异常路径未关闭或跨进程误用可能导致泄漏、越权访问或 UAF | 已有正常路径 fd 计数测试，异常与多进程场景不足 | 明确创建、导入、映射、关闭的所有权；适用时设置 close-on-exec；覆盖异常退出、重复关闭、pending 引用和进程退出后的资源回收 | 第 1～3 步 |
| 依赖：FFmpeg 与内部接口 | 静态对象裁剪依赖 FFmpeg 内部符号和结构，版本升级可能造成构建失败或语义变化；依赖漏洞也需跟踪 | 当前验证绑定特定源码形态，缺少版本策略 | 固定并记录 FFmpeg 版本与构建配置；建立内部符号兼容测试、依赖清单/SBOM、许可证检查和漏洞升级流程；每次升级重跑 parser 对照测试 | 第 0 步建立，持续执行 |
| 部署：库与驱动冲突 | shim 的 SONAME、加载路径或驱动版本不匹配可能误加载系统/NVIDIA 库，造成启动失败或难以定位的运行错误 | 尚未形成安装和共存矩阵 | 明确安装目录、soname、开发软链及 loader 路径策略；测试与 NVIDIA 库共存、不同 Mesa/ROCm 版本组合和升级回退；不得未经验证覆盖系统库 | 第 2 步设计，第 3 步验收 |
| 运维：诊断不足 | 跨 CUVID/NVENC、VAAPI、HIP 和 DMA-BUF 的错误难以定位，现场只能看到笼统失败 | 当前以实验日志为主，缺少统一诊断约定 | 统一记录阶段、会话、codec、返回码、surface/fence 状态和降级原因；增加队列等待、映射失败、资源用量等指标；默认不记录原始码流等敏感内容 | 第 1 步建立基础日志，第 3 步完善 |
| 运维：异常恢复 | 驱动错误、设备复位、等待超时或中途退出可能造成死锁、资源残留和服务不可用 | 已有 flush/destroy 基础测试，故障注入不足 | 注入 VAAPI/HIP 错误、超时、设备丢失和取消；验证 flush、seek、EOS、重建会话及进程退出清理；所有等待须有可诊断的超时与恢复策略 | 第 2～3 步 |
| 性能：同步与降级回退 | 主机同步、线性布局或软件兜底可能使吞吐、延迟或 CPU 占用不满足业务要求 | P0 尚无完整流程和多路并发基线；按评审决定，P0 只记录不设性能通过阈值 | P0 记录 CPU 拷贝与主机同步零拷贝等数据；P1 根据真实业务确定阈值，未达标时触发架构复审 | P0 记录，P1 判定 |

### 4.15 NPP 与 HIPIFY 专项验证方案

以下为**待执行验证设计**，不代表已有测试结果。NPP 参考环境、HIPIFY 工具版本、代表性用户源码及性能阈值须在第 0 步记录。

#### NPP P0 数值与语义验证

参考结果为同版本 NVIDIA NPP 在约定 NVIDIA 平台上的输出；测试须记录 CUDA SDK、驱动、GPU、输入参数和构建选项。

| 验证项 | 方法 | 通过标准 |
|---|---|---|
| 三个 P0 函数基础正确性 | 对零值、极值、固定种子随机值、渐变和棋盘格输入，分别执行 NVIDIA NPP 与 HIP shim | 所有输出 plane 逐像素比较；以逐字节一致为目标，差异不得仅以“视觉效果接近”放行 |
| Resize 坐标与舍入 | 覆盖放大、缩小、非整数比例、奇数尺寸、常见及非对齐 ROI，重点检查 `SqrPixel` 坐标映射和边界像素 | 坐标、舍入及边缘结果与参考一致；若存在架构相关差异，须列出模式、差值和评审结论，不得自行放宽容差 |
| 平面转换与 ROI | 对 `P2P3/P3P2` 检查 Y/UV plane 顺序、ROI 原点、宽高、src/dst step 和非连续内存 | 输出 plane、ROI 外保护区域及 padding 均符合预期，不得越界写或破坏相邻数据 |
| stream 与异步行为 | 覆盖默认/非默认 stream、事件等待及连续提交，验证主机读取前的同步 | 无提前读取、跨流竞争和结果不稳定；重复执行结果一致 |
| 参数与错误语义 | 对空指针、非法 ROI/step、不支持格式及资源失败进行对照 | 返回值和输出状态符合约定的 NPP 语义；不得返回成功但留下未初始化输出 |
| 完整流程联合验证 | 将解码输出交给三个 P0 函数，再传入代表性用户 kernel | 与 §1.7 第 2 步共同通过，保留逐阶段参考输出以便定位差异来源 |

#### HIPIFY 代表性 kernel 验证

| 验证项 | 方法 | 通过标准 |
|---|---|---|
| 代表性输入选择 | 第 0 步选定真实用户源码，记录所用 CUDA API、kernel 特征、数学库、编译选项及输入数据 | 不用单一 `cudaMalloc` 示例代替真实负载；无法提供的源码须明确替代依据 |
| 翻译过程可复现 | 固定 HIPIFY、hipcc 和 ROCm 版本，保存命令、工具报告、源码差异及人工修改说明 | 从同一源码快照可重复得到同一 HIP 实现；人工修改不能只存在于开发者本机 |
| 构建与接口完整性 | 检查翻译日志、编译警告、未支持 API、内联 PTX/特殊 intrinsic（如有）及链接依赖 | 未支持能力必须显式报告；不得静默丢弃计算逻辑或以错误实现替代 |
| 数值正确性 | 使用固定输入对比 CUDA 参考与 HIP 输出，覆盖边界尺寸、空/极值输入及重复执行 | 按 kernel 类型在第 0 步定义逐位或容差标准；未定义标准不得判定通过 |
| 内存与并发 | 验证分配/释放、流和事件、越界保护及多批次提交 | 无越界、泄漏、未同步读取和非确定性错误 |
| 完整流程与性能 | 当前仅接入 NVDEC/CUVID→NPP，记录端到端正确性和性能；HIPIFY 用户 kernel 与 NVENC 属于历史专项参考 | 通过 §1.7 第 2 步；P0 性能只记录不设阈值，不得将单项翻译或函数结果记作完整业务兼容 |

### 4.16 NVENC 语义与验收矩阵

以下为**待执行验证设计**。不同硬件编码器可以产生不同码流，因此验收重点是接口语义、请求配置、帧数与顺序、时间戳、码流合规性和画质，不要求与 NVIDIA 输出逐字节相同。

| 语义项 | 验证方法 | 通过标准 | 阶段 |
|---|---|---|---|
| 初始化与能力查询 | 覆盖 session 创建、preset/config、`SetIOCudaStreams`、`GetSequenceParams` 及不支持能力查询 | 返回能力反映真实后端；不支持项显式失败；函数表、错误码及资源释放符合目标 ABI/语义 | 第 2 步 |
| 输入资源与生命周期 | 使用 HIP 生成的确定性 NV12，检查各 plane、pitch、ROI，并在编码pending期间复用/销毁输入进行测试 | 导入 surface 回读与 HIP 输入逐字节一致；编码完成前不回收输入；无越界、花屏和 UAF | 第 2 步 |
| I/P 与 GOP | 使用可识别帧号的序列检查 IDR、I/P、GOP 长度及强制关键帧请求 | 实际输出符合已声明配置；无法映射的 GOP 或帧类型不得静默替换 | 第 2 步 |
| 码率控制 | 分别验证 CQP、CBR、VBR 的能力查询、参数映射和实际输出；记录测试窗口及码率 | CQP 行为及码率结果达到第 0 步确定的阈值；后端模型差异须显式记录，不宣称与 NVENC 完全一致 | 第 2～3 步 |
| SPS/PPS 与码流头 | 检查 `VAConfigAttribEncPackedHeaders`、`GetSequenceParams`、Annex-B 头及必要时的 shim 自生成路径 | 输出可被独立解码器正确解析；序列参数与实际编码配置一致，不重复、不缺失、不使用过期头 | 第 2 步 |
| 输出延迟与 `NEED_MORE_INPUT` | 在 I/P、B 帧及不同缓冲深度下记录每次输入、输出和返回值 | `NV_ENC_ERR_NEED_MORE_INPUT` 仅作为正常缓冲状态处理；不丢帧、不误报失败，最终输出帧数正确 | 第 2～3 步 |
| 时间戳与 B 帧重排 | 为每帧设置可追踪 PTS，检查输出顺序、DTS/PTS 关联和参考关系 | 输出顺序及时间戳符合 NVENC 约定；重排不造成帧与输入错配 | 第 3 步 |
| EOS、flush 与取消 | 分别在空输入、缓冲中有帧、连续 flush、取消及销毁场景下测试 | EOS 能排空应输出帧；重复/异常操作有定义行为；无死锁、残留输出或资源泄漏 | 第 2～3 步 |
| seek 与配置切换 | 测试 seek、分辨率/GOP/码率变化及重新初始化 | 不输出旧会话残留帧；必要的新序列头和参考帧状态正确；不支持的热切换显式报错 | 第 3 步 |
| 错误映射与诊断 | 注入非法句柄、不支持格式、资源耗尽、VAAPI 失败和同步超时 | 映射到约定的 NVENC 错误；`GetLastErrorString` 和日志能定位阶段，不返回伪成功 | 第 2～3 步 |
| 画质、性能与并发 | 使用固定内容测量 PSNR/SSIM、码率、吞吐、延迟、CPU 占用及多会话长稳 | 达到第 0 步确定的阈值；保存与源输入、直接 VAAPI 路径及约定参考编码器的可比结果 | 第 3 步 |

每项测试须保存配置、输入/输出帧号和时间戳、关键返回值、序列头及解码校验结果；日志默认不保存用户原始视频内容。

### 4.17 完整流程性能测试方法

以下为**待执行验证设计**。主要对照必须使用同一目标平台、相同输入和相同业务处理；NVIDIA 平台结果只作外部参考，不作为零拷贝收益的直接基线。

#### 对比组

| 组别 | 数据通路 | 用途 |
|---|---|---|
| A：CPU 拷贝基线 | VAAPI 解码→CPU 回读→HIP 输入→CPU 回传→VAAPI 编码 | 衡量完整 D2H/H2D、同步和拷贝成本；必须实现真实业务路径，不能只测 `memcpy` |
| B：主机同步零拷贝 | LINEAR DMA-BUF + `vaSyncSurface`/`hipEventSynchronize` | 首期主方案，和 A 比较收益及瓶颈 |
| C：tiled + GPU 转换 | tiled 解码→GPU 转成 HIP 可读布局→后续流程 | 当线性布局在目标平台回退时，与 B 比较转换成本 |
| D：异步 fence 增强 | DMA-BUF + DRM fence 桥接 | 仅在未来实现后，与 B 比较主机等待减少量 |
| E：软件条件兜底 | 软件编码或评审批准的其他降级路径 | 单独记录功能可用性及 CPU、吞吐和延迟代价，不与硬件路径混报 |

#### 测量指标与场景

| 维度 | 必须记录的内容 |
|---|---|
| 吞吐 | 解码 fps、编码 fps、完整流程 fps；多路并发时同时记录单路和总吞吐 |
| 延迟 | 单帧端到端延迟及各阶段等待时间，报告 p50/p95/p99 和最大值，不只报告平均值 |
| 阶段成本 | VAAPI 等待、HIP kernel/event、NPP、导入/映射、CPU 回读/回传、编码等待分别计时 |
| 资源 | CPU 占用、进程内存、显存/surface 数、fd 数；启动资源与稳态资源分开 |
| 初始化成本 | 首次 context/session、内存导入和 kernel 加载单独报告，不得混入稳态帧成本后隐藏 |
| 场景矩阵 | P0 覆盖 1080p/4K、NV12、I/P/B、1 路和 8 路及 seek/flush；P010 和其他格式不属于 P0 |
| 结果有效性 | 同步记录输出正确性和画质；配置、帧数或业务处理不同的结果不得直接比较 |

#### 测试流程

1. 固定硬件、Mesa/ROCm/libva/FFmpeg 版本、构建参数、输入码流和编码配置，记录电源/时钟等可能影响结果的环境。
2. 分离冷启动、预热和正式测量；固定并记录预热量、正式帧数、重复次数和并发规模。
3. 使用单调时钟记录每帧阶段时间；HIP 内部使用适当的 GPU event，避免只用整段脚本墙钟时间推断各阶段成本。
4. 性能日志采用缓冲后汇总，另行测量插桩开销；不得因逐帧同步打印改变流水线行为。
5. 保存每轮原始数据，报告样本量、波动和异常值处理规则；不得只挑最优一轮。
6. 在第 0 步确定相对 A 的吞吐、延迟和资源验收阈值。B 未达标时先定位同步、布局或映射瓶颈；是否需要 D 由实测决定，不预先假设异步一定更快。

### 4.18 版本、发布与回退要求

以下为**待执行交付约束**，面向首次可运维发布，不表示当前 shim 已具备这些能力。

| 主题 | 最小交付要求 | 验收方式 |
|---|---|---|
| 版本兼容矩阵 | 记录目标硬件、kernel、Mesa/libva、ROCm/HIP、FFmpeg、CUDA SDK 头文件、编译器及 glibc 版本；区分已验证、可试用和不支持 | 每个声明支持的组合有安装、加载和完整流程测试记录；未测组合不得写成正式支持 |
| 发布物与构建信息 | 发布 shim `.so`、SONAME/软链、默认配置、能力诊断工具、版本清单、构建标识、SBOM/许可证和对应测试报告 | 从干净环境按发布说明可重复安装；每个二进制可追溯至源码和构建配置 |
| 安装与加载隔离 | 明确安装目录、loader 路径及优先级，不未经验证覆盖系统或 NVIDIA 库；开发软链与运行库分开管理 | 通过 `dlopen`/链接测试确认实际加载路径和 SONAME；错误路径能给出明确诊断 |
| 与 NVIDIA 共存 | 分别定义同机共存和同进程共存是否支持；不得默认承诺同进程混用同一 CUDA 符号 | 覆盖同机不同进程测试；若不支持同进程共存，启动检查和使用说明必须明确限制 |
| 运行时能力检查 | 启动或诊断命令检查 HIP/VAAPI 设备、codec、DMA-BUF、LINEAR modifier、同步及软件兜底条件 | 检查结果与实际初始化一致；不支持项在创建会话前或可定位的失败点显式报告 |
| 配置与降级 | 后端、软件兜底及实验特性的开关有默认值、优先级和生效记录；禁止静默改变用户请求的编码语义 | 测试默认、显式启用、能力不满足和回退失败路径；日志能说明实际选择及原因 |
| 日志与健康检查 | 启动记录 shim/驱动版本；运行记录会话、阶段、错误码、队列/fence 状态和资源异常；提供可执行健康检查 | 能用日志定位加载、解码、映射、计算、编码和同步故障；默认不记录原始码流等敏感内容 |
| 升级与回退 | 保留上一版本库和配置；升级前完成兼容检查，升级后执行健康检查；定义失败时停止升级和恢复步骤 | 在测试环境完成升级→失败注入→回退→完整流程复测；回退不依赖重新下载未保存的旧版本 |
| 发布判定 | 仅发布已通过相应范围测试的 API、codec 和平台组合；实验功能单独标识 | 发布清单与测试证据一一对应；符号导出、PoC 或单项 PASS 不得作为生产发布依据 |

回退触发阈值（崩溃、错误率、吞吐、延迟或资源异常）须在第 0 步或发布评审中确定；触发后应停止使用新版本并恢复已验证版本，而不是仅记录告警继续运行。

### 4.19 统一测试环境与复现记录

#### 当前环境汇总

| 环境 | 硬件 | 已锁定/记录的软件环境 | 当前证据与限制 |
|---|---|---|---|
| **AMD 验证机** | Radeon RX 7900 XTX，gfx1100/Navi31，24GB | Ubuntu 24.04.2；kernel 6.11.0-17；ROCm 7.2.4 / HIP 7.2.53211；Mesa 25.2.8；libva 2.20.0（VA-API 1.20）；FFmpeg 7.1 官方源码自编译 | 实验 C/E 已重跑通过；CUVID 符号和 VAAPI session 通过。现有 FFmpeg 构建未包含 `h264_cuvid`/`h264_nvdec`/NPP，P0 宿主构建及原始日志仍待补齐 |
| **NVIDIA 参考机** | GeForce RTX 4060，Compute Capability 8.9 | Ubuntu 22.04.5；驱动 595.99.02；CUDA Driver API 13020；CUDA Runtime/NPP 11.5（NPP 11.5.1.107） | 3 个 P0 NPP 函数参考输出已生成并校验；仅作 NVIDIA 行为/数值参考，不是 AMD 交付环境 |
| **构建依赖** | 与上述构建环境配套 | nv-codec-headers commit `610325d019ad75248aa6e7abe0f4909d2ac6bc69`；libcuda 源码已从 `cuda_shim` 导入项目 `cuda_shim/` | CUDA/NPP 11.5 头文件由使用方另行提供，不进入发布物；FFmpeg 按评审决定仅记录 7.1 版本及构建配置，不声称 Git commit 级复现 |
| **历史能力对比批次** | NVIDIA RTX 4060；AMD RX 9070（VCN5） | 精确驱动和构建环境未完整记录 | 仅作为历史格式能力参考；不得与当前双机环境混写为同一测试环境 |

NPP 参考输出 SHA256：
- `p2p3.yuv`：`7800549c7f3c4a3e54ef28c53cc9d84a228341717e8fd656d0d30bc84607c85a`
- `roundtrip.nv12`：`1899758496662f141280cc689b854d27c240d57cab23ed23dccba1ae56d08569`
- `resize_4x4.gray`：`70cdde3ccdccbcfe2eeca52d8deca9cbca06b7b3612c69a31e0e677002eb8e6d`

不同批次不得共用“测试环境相同”的默认假设。单项实验如有额外依赖或环境差异，应在该实验记录中覆盖说明。

#### 每项实验的复现记录模板

| 字段 | 必须记录的内容 |
|---|---|
| 实验标识与目的 | 唯一 test ID、验证假设、对应章节和预期结论 |
| 源码与构建 | 目录、源码版本/commit 或归档位置、构建命令、编译选项及依赖版本 |
| 运行环境 | 硬件、kernel、驱动、Mesa/ROCm/libva/CUDA/FFmpeg/Vulkan 等实际版本 |
| 输入数据 | 码流或图案的生成命令、参数、版本及校验值；用户数据默认不随公开报告分发 |
| 执行方式 | 工作目录、完整命令、环境变量、并发参数、预热和测量轮数 |
| 原始结果 | 日志、逐帧校验、性能原始样本、fd/内存记录及文件归档位置 |
| 结果判定 | PASS/FAIL/环境不满足/测试无效分别记录，不得将环境缺失误记为功能通过或失败 |
| 证据与限制 | AMD-PoC/目标平台已验证等等级、未覆盖场景、已知偏差及后续事项 |

新增实验必须在提交结果时同时提供上述记录；存量实验按 §7.2 第 0 步补齐。建议按 `docs/evidence/results/<test-id>/<环境或版本>/` 归档原始结果，正文只保留结论及链接，避免实验过程继续膨胀。

---

## 五、总体结论与目标数据通路

### 5.1 关键结论与证据状态

| 主题 | 当前结论 | 证据等级 | 未闭合事项 |
|---|---|---|---|
| 后端与当前格式 | 主线采用 VAAPI；当前交付只承诺 H.264/NV12，不由历史 15 组格式对比推导完整覆盖 | **AMD-PoC** | P0 码流特性及实际交付平台复测（§4.1、§4.4） |
| 解码跨栈资源与布局 | HIP+VAAPI 可共存，LINEAR NV12 + DMA-BUF 是优先路径 | **AMD-PoC** | CUVID map 语义、实际布局及各平台查询；P010 不属于 P0（§4.2、§4.11） |
| 底层解码数据通路 | 实验 C 已在 AMD 验证机完成 I-only、I/P、I/P/B 多帧逐字节校验 | **AMD-PoC（第 1A 步通过）** | 不替代 CUVID shim 和 FFmpeg 双路径验收 |
| 主机同步 | `vaSyncSurface` + HIP 同步可作为首期基线 | **AMD-PoC** | 真实 1/8 路流水线、30 分钟资源稳定性及异常恢复 |
| CUVID shim | 符号面、VAAPI LINEAR NV12 session 可用，parser 复用 FFmpeg 解析 I-only 与 I/P/B 通过；Vulkan Video 后端已接入并完成 I/IP/IPB 各 30 帧逐字节一致 + FFmpeg `h264_cuvid` 宿主跑通 | **AMD-PoC（符号/session/parser/后端）** | VAAPI 后端真实 decode/map 像素、`h264_nvdec` 零拷贝、错误语义和 FFmpeg 双路径 |
| NPP | NVIDIA 11.5.1.107 的 3 个 P0 函数参考输出已建立 | **NVIDIA 参考机已验证** | AMD HIP 实现、逐字节对照、ABI 和 CUVID→NPP 联合链路 |
| libcuda | 源码已找回并归档，历史 82 符号/47 项功能结果仅作子集参考 | **AMD-PoC（历史子集）** | FFmpeg 7.1 实际功能、动态加载及错误语义验收 |
| parser 复用 | FFmpeg 静态解析与实验 C 的 I/P/B、DPB、显示重排已验证 | **AMD-PoC** | 长期参考、crop、seek/flush、动态分辨率及 CUVID 集成；场/MBAFF 明确不支持 |
| Vulkan Video 备选后端 | VK 解码→`vkCmdCopyImageToBuffer` 拷贝→`vkGetMemoryFdKHR` 导出→HIP 读，I/IP/IPB 各 30 帧逐字节一致、无泄漏；已作为 `libnvcuvid.so.1` 可选后端接入（`CUVID_SHIM_BACKEND=vk`）并通过 FFmpeg `h264_cuvid` 宿主 30 帧 0 error | **AMD-PoC（第 1A 步等价闭环 + 后端接入宿主）** | `h264_nvdec` 零拷贝、多流/seek/flush、crop、动态分辨率、HEVC/VP9/AV1 未验收；异步同步仍缺；Tier 2 备选定位不变 |
| 当前交付业务链路 | NVDEC/CUVID→NPP 尚无完整实现证据 | **待验证** | 第 1B 步 CUVID/FFmpeg 与第 2 步 NPP 联合链路均未通过，不包含编码 |

### 5.2 结论使用边界

- 当前结果支持继续投入主路线，**不支持**宣称完整格式覆盖、完整 CUDA SDK 兼容或目标平台生产可用。
- DMA-BUF import/map、surface 创建和同步测试通过，**不等于** CUVID shim 或 FFmpeg 宿主链路已经通过；实验 C 已完成多帧 I/P/B 的 libva→HIP 数据通路验证，但 CUVID shim、FFmpeg 双路径及 NPP 联合链路仍未闭合。
- AMD 平台的性能、布局和私有 HSA 接口结论，**不得直接替代**目标平台验收；生产性能、长稳、安全和运维能力仍需单独验证。

### 5.3 当前目标数据通路（待 CUVID/NPP 联合验证）

```
CUVID / NVDEC 解码
       │
       ▼
VAAPI LINEAR NV12 surface
       │  vaExportSurfaceHandle
       ▼
DMA-BUF → hipImportExternalMemory / GetMappedBuffer
       │
       ▼
HIP 可见的 NV12 帧 → NPP HIP kernel → HIP 侧输出
```

- **底层边界已通过**：实验 C 已证明 VAAPI 真实解码后的像素可由 HIP 逐字节正确读取。
- **CUVID 边界未通过**：必须由 `cuvidCreateDecoder`、parser/`CUVIDPICPARAMS`、`cuvidMapVideoFrame64` 和 unmap 的真实调用重新证明；session 创建不能代替。
- **布局门禁**：只有 Y/UV 能由同一 HIP 映射基址及合法 pitch/offset 表达时，才可声明 mapped-frame 零拷贝；其他布局须显式降级且不得混报。
- **NPP 联合验证**：NPP P0 函数使用同栈 HIP 指针；须与 NVIDIA 11.5.1.107 参考逐字节一致，并验证输入 surface 在 map 生命周期内不被提前回收。
- **同步**：采用主机同步基线；异步 fence 不属于 P0。
- **FFmpeg 路径**：`h264_nvdec` 用于零拷贝 HIP 读取验收；`h264_cuvid` 输出 CUDA 路径自身可能增加 `cuMemcpy2DAsync` 设备内拷贝，单独记录。

---

## 六、实施路线图

### 6.1 开发顺序与阶段通过条件

| 阶段 | 主要开发内容 | 当前状态/通过条件 |
|---|---|---|
| **第 0 步：范围与 AMD 底层能力确认** | 冻结 §1.6 范围、FFmpeg/API 清单和验收口径；重跑实验 C/E | **已通过**；不代表 P0 整体通过 |
| **第 1A 步：底层真实解码到 HIP** | 实验 C 验证 libva、H.264 parser、LINEAR surface 和 HIP 数据通路 | **已通过**；仅代表底层数据通路 |
| **第 1B 步：CUVID 与 FFmpeg 双路径** | 实现 libnvcuvid decoder/parser/map/unmap；接入 `h264_cuvid` 和 `h264_nvdec` | **进行中**；符号/session/parser 已通过，真实像素和双路径未验收 |
| **第 2 步：CUVID→NPP 联合链路** | 实现 3 个 NPP P0 函数及必要 nppc，接入 CUVID 解码输出 | **待完成**；NVIDIA 参考基线已具备，AMD 输出须逐字节一致 |
| **P1：复杂场景与交付验证** | 完成 crop、seek/flush、动态分辨率、长期参考、用户应用、异常恢复、安全、部署及生产性能验收 | 按 §1.6 范围完成；P010、其他 codec/NPP/API 仅在新增需求评审后扩展 |

P0 性能只记录不设通过阈值；P1 再根据真实业务确定性能门槛。异步 fence、Vulkan Video 和 HIPIFY 用户 kernel 均为独立后续工作，不替代 CUVID/NPP 主机同步验收。

### 关键实现约束（实测得来）

- 共享 surface 优先用**线性布局**；当前 AMD 实验采用 pitch 256 对齐，生产实现须依据运行时能力和实际导出布局。
- 同步用主机同步基线；资源回收区分 DPB、mapped frame 和 NPP 输入引用，unmap/flush 前不得提前释放。
- 能力查询**运行时动态**，不静态套用 AMD caps 到其他实际交付平台。
- 当前只实现 CUVID 解码与清单内 NPP；NVENC、Vulkan Video 和其他 codec/API 不进入 P0 验收。

---

## 七、附录

### 7.1 关键 API 参考

| 能力 | API | 备注 |
|---|---|---|
| VAAPI 导出 | `vaExportSurfaceHandle(DRM_PRIME_2)` | 默认 tiled+DCC；共享需线性 |
| HIP 导入 | `hipImportExternalMemory(OpaqueFd)` + `hipExternalMemoryGetMappedBuffer` | 仅 OpaqueFd，无 DmaBufFd |
| HIP 导出 | `hsa_amd_portable_export_dmabuf`（HSA 私有） | HIP 标准无导出；返回 fd+offset |
| VAAPI 导入 | `vaCreateSurfaces(DRM_PRIME_2, MOD_LINEAR, pitch)` | 当前 AMD 实验采用 **pitch 256 对齐**；其他平台须运行时查询 |
| 主机同步 | `vaSyncSurface(2)` / `hipEventSynchronize` | 跨栈基线 |
| 异步同步 | Vulkan 外部 semaphore | **本次测试的 HIP 版本不支持导入**；其他 fence 候选机制见 §4.12 |

### 7.2 遗留问题与处理阶段

优先级含义：**P0**＝进入大规模开发前必须完成；**P1**＝首期交付前必须按评审范围完成；**P2**＝独立增强或后续路线，不阻塞首期主机同步方案。

| 项目 | 完成标准 | 当前状态 |
|---|---|---|
| **P0-1 交付范围** | §1.6 范围、码流特性、NPP 和非目标形成评审结论 | **✅ 已完成** |
| **P0-2 宿主及 API 清单** | FFmpeg 7.1 静态调用面、22 个 CUVID 符号、libcuda 子集和 3 个 NPP 函数形成清单 | **✅ 已完成**，见 `docs/architecture/api-coverage.md` |
| **P0-3 兼容/ABI 边界** | 仅承诺源码重编译；明确 FFmpeg 内部动态加载仍需符号与 ABI 一致 | **✅ 已完成** |
| **P0-4 AMD 底层能力门禁** | 实验 C/E 在 AMD 验证机重跑通过 | **✅ 已完成** |
| **P0-5 证据口径** | 区分 libva→HIP 第 1A 步与 CUVID/FFmpeg 第 1B 步，清理单帧/P-B 等矛盾 | **✅ 已完成** |
| **P0-6 CUVID shim 最小集成** | 真实 CUVID decode/parser/map/unmap、多帧像素、生命周期和资源检查通过 | **进行中**；符号、VAAPI session、parser（I-only + I/P/B）已验证；Vulkan Video 后端已实现并接入（`CUVID_SHIM_BACKEND=vk`），I/IP/IPB 各 30 帧逐字节一致 + fd 无泄漏，FFmpeg `h264_cuvid` 宿主跑通；VAAPI 后端真实 decode/map 像素与 `h264_nvdec` 零拷贝未通过 |
| **P0-7 FFmpeg 双路径联调** | `h264_cuvid`/`h264_nvdec` 分别通过基础、crop/seek/动态分辨率、1080p/4K、1/8 路和 30 分钟测试 | **待 P0-6 完成后执行** |
| **P0-8 NPP 参考基线** | NVIDIA 环境、3 个函数参考输出、哈希和逐字节标准固定 | **✅ 已完成**；AMD NPP 实现尚未完成 |
| **P0-9 环境与复现材料** | 双机环境、源码、命令、输入、原始日志和结果归档；FFmpeg P0 构建材料齐全 | **进行中**；环境已记录，原始日志及 AMD P0 宿主构建待补 |
| **P0-10 验收指标** | 1080p/4K、1/8 路、30 分钟、NPP 逐字节及“性能只记录”口径确定 | **✅ 已完成** |
| **P1 用户应用与生产交付** | 用户应用及 NPP 清单、真实业务性能、长稳、安全、部署和回退验收 | 待 P0 完成后执行 |
| **P2 后续增强** | P010、其他 codec/NPP/API、异步 fence、Vulkan Video、HIPIFY 等按新需求立项 | 不属于当前交付承诺 |

### 7.3 试验程序索引

以下程序须在与实验相匹配的 AMD/NVIDIA、ROCm/CUDA、Mesa 及依赖环境中运行，不统一承诺均可直接构建或运行。各项结论仅适用于已记录的环境和测试项。

| 目录/状态 | 程序或命令 | 验证内容 |
|---|---|---|
| `tests/zerocopy/` | `verify_zerocopy` | 实验1/2/3（共存、边界1、边界2） |
| `tests/zerocopy/` | `retry_exp3` / `retry_exp3_v2` | 边界2 失败定位（pitch 对齐根因） |
| `tests/zerocopy/` | `make loop-decode` | 解码闭环：VAAPI 硬解 vs 软解逐字节 |
| `tests/zerocopy/` | `make loop-encode` | 编码闭环：图案→VAAPI 编码→解码回读 |
| `tests/zerocopy/` | `hip_read_surface` | HIP 读写外部 fd（像素往返工具） |
| `tests/zerocopy/` | `make linear`（`run_linear_vs_tiled.sh`） | 线性/tiled modifier、内容正确性及耗时对比 |
| `tests/zerocopy/` | `make multires`（`run_multires_linear.sh`） | 320×240 至 4K 多分辨率线性验证 |
| `tests/zerocopy/` | `make modifier-linear`（`verify_modifier_linear.cpp`） | 主动限定 LINEAR modifier 的生产路径验证 |
| `tests/zerocopy/` | `make p010`（`verify_p010_linear.cpp`） | P010 线性 surface 创建、导出及 HIP 导入 |
| `tests/zerocopy/` | `make -f verify_decode_to_hip.make run` / `run-p` / `run-b` | **实验 C**：libva 真实解码→HIP 读端到端像素校验（I-only / I/P / I/P/B 三种码流均 30 帧逐字节一致，§1.7 第 1A 步 + P/B 帧 DPB 复用 + 显示序重排闭合） |
| `tests/zerocopy/` | `make -f verify_hip_to_encode.make run` | **实验 D**：HIP 写→VAAPI 编码端到端（程序链路 10/10，码流缺 SPS/PPS 待修） |
| `tests/zerocopy/` | `make -f verify_portability.make run` | **实验 E**：目标平台 5 项能力止损阀探测（AMD 6/6，待目标平台跑） |
| `tests/sync/` | `verify_sync` | 同步 S1–S8 |
| `tests/sync/` | `make semaphore`（`verify_semaphore_async.cpp`） | HIP semaphore 能力及 DRM syncobj/sync_file 基础原语 |
| `tests/vulkan-video/basic/` | `verify_vulkan_video` | Vulkan Video 基础 + 通用零拷贝 |
| `tests/vulkan-video/full/` | `verify_vulkan_video_full` | Vulkan Video V1–V4、V6 |
| `tests/zerocopy/` | `make -f verify_vk_decode_to_hip.make run-b` / `run-p` / `run-i` | **Vulkan Video P0 闭环**：VK 解码→拷贝→导出→HIP 读端到端逐字节校验（I/IP/IPB 各 30 帧一致 + 无 fd/显存泄漏） |
| `cuda_shim/`（源码已找回并归档） | `make check_func` / `test_func_full.cpp` | libcuda 历史 47 项功能测试；源码取自 `cuda_shim` |
| `cuda_shim/` | `make libnvcuvid.so.1 verify_cuvid_symbols verify_cuvid_session verify_cuvid_parser` | CUVID 21 必需符号 + caps、SONAME、VAAPI LINEAR NV12 session 和 parser 复用 FFmpeg 解析（I-only + I/P/B）；**不含 VAAPI 后端真实解码像素验收** |
| `cuda_shim/` | `CUVID_SHIM_BACKEND=vk ./verify_vk_decode_e2e test_{,_p_,_b_}320x240.h264 ref_*` | **Vulkan Video 后端端到端**：CUVID 表面下切 VK 解码，I/IP/IPB 各 30 帧逐字节一致 + fd 无泄漏；FFmpeg `h264_cuvid` 宿主 30 帧 0 decode error |
| `cuda_shim/` | `docs/architecture/api-coverage.md` | FFmpeg 7.1 CUVID/libcuda/NPP 的 P0 接口覆盖基线 |
| `benchmarks/scripts/collect_ffmpeg_api.py` | `python3 benchmarks/scripts/collect_ffmpeg_api.py /path/to/ffmpeg-7.1` | FFmpeg 实际 CUVID/CUDA/NPP 静态调用面采集 |
| `tests/parser/npp_p0_reference.cpp` | 链接 `nppig/nppicc/nppc/cudart` 后运行 | NVIDIA NPP 11.5 三个 P0 函数参考输出及 P2P3/P3P2 往返校验 |
| `tests/parser/verify_parser_reuse.cpp`（已补回） | `make -f verify_parser_reuse.make` | FFmpeg parser 复用及 VAAPI 参数填充验证（本机 8/8 PASS，补齐 §4.6 缺失源文件） |

### 7.4 子文档索引（详细技术参考）

| 文档 | 内容 |
|---|---|
| `docs/architecture/nvdec-vaapi-comparison.md` | 解码格式矩阵实测（第一批） |
| `docs/architecture/cuvid-vaapi-mapping.md` | CUVID/NVENC ↔ libva 映射 + parser 落地 + libcuda 状态 |
| `docs/architecture/npp-compat-plan.md` | NPP 路线 A、11 子库、HIP kernel、分阶段 |
| `docs/architecture/hipify-research.md` | HIPIFY 原理与架构 |
| `docs/architecture/vulkan-video-evaluation.md` | Vulkan Video 评估 + 实测 |
| `docs/verification/zero-copy-experiment.md` | 零拷贝实验设计 |

### 7.5 版本历史（摘要）

- **v0.1（2026-10-28/29）**：初稿，形成双轨架构及各子方案。
- **v1.0～v1.9（2026-11-02～03）**：整合 NVDEC/VAAPI 格式对比、DMA-BUF、线性布局、P010、同步、Vulkan、libcuda 和 parser 等 AMD 概念验证结果；各结论的适用范围以正文最新口径为准。
- **v1.10～v1.18（2026-11-03）**：完成 P0 文档优化：修正技术表述和过度承诺，增加证据等级、决策摘要、首期范围、阶段通过条件及接口/数据通路边界，重排遗留问题和实验索引。
- **v1.19～v1.25（2026-11-03）**：完成首轮 P1 文档补充：增加交付风险、接口覆盖矩阵、NPP/HIPIFY/NVENC 验证方案、性能基线、版本发布与回退要求及统一复现模板。
- **v1.26（2026-11-03）**：压缩版本历史，删除与正文重复的旧结论和逐版本细节；技术方案及验证要求不变。
- **v1.27（2026-11-03）**：完成全文一致性检查；交叉引用、目录、表格和代码块检查通过，修正 HIPIFY/shim 范围、HIP semaphore、解码内容、LINEAR 创建路径及主机同步性能等剩余绝对化口径。
- **v1.28（2026-11-03）**：回写最小验证实验套件结果——①实验 C（真实解码→HIP 读）单帧 I-only 端到端逐字节一致（9/9），§4.2-A4 解码向闭合并沉淀 3 个实现约束（profile 映射/separate layers/pitch 逐行）；②实验 D 实证 §4.7「SPS/PPS 需自理」预警（packed headers 静默回退导致缺头）；③实验 E 5 项止损阀程序就绪（AMD 6/6，待目标平台）；④实验 A/B（parser 复用复现 8/8 + 干净 LGPL 构建 .o 可提取）补回缺失源文件并确认法律路线；§5.1、§7.2、§7.3 同步更新。
- **v1.29（2026-11-03）**：实验 C 多帧版通过（30 帧逐字节一致 + 池复用 + flush 幂等 + fd 无泄漏，11/11 PASS）→ **§1.7 第 1 步（真实解码→HIP 读）判定通过**；全文"单帧闭合"口径升级为"第 1 步完整闭合"。
- **v1.30（2026-11-03）**：实验 C I/P 帧版通过（1 IDR + 29 P 帧，Main profile/CABAC，30 帧逐字节一致，11/11 PASS）→ **P 帧 DPB 参考复用闭合（§7.2 P1 核心）**；复用 `ff_h264_init_poc`/`ff_h264_parse_ref_count`（加 `h264_parse.o`），手写最小 DPB 状态机 + RefPicList0；记录两个坑（VAAPI 编码器 bframes=0 强制 poc_type=2；目标机无 libx264）。
- **v1.31（2026-11-04）**：实验 C I/P/B 帧版通过（1 I + 10 P + 19 B，30 帧逐字节一致，12/12 PASS）→ **B 帧 + 双参考列表 + 显示序重排闭合**；新增记录两个根因坑（`log2_max_pic_order_cnt_lsb_minus4` 少减 4 导致 POC LSB 错位；DPB 滑动窗口误删参考帧）+ 一个规范坑（补填 `frame_num`/`pic_order_present_flag`/`reference_pic_flag`）；§4.6、§7.2 同步更新。
- **v1.32（2026-11-04）**：完成 P0 范围与证据基线回写——交付收敛为 CUVID 解码 + FFmpeg 实际使用的 3 个 NPP 函数；确认源码重编译、FFmpeg 7.1 双路径、CUDA/NPP 11.5、1080p/4K、1/8 路及 30 分钟验收口径；记录双机环境、NVIDIA NPP 参考输出、CUVID 符号/session 中间结果；将实验 C 明确限定为第 1A 步，P0-6/P0-7 标记暂停，不再把编码、用户应用或完整 CUDA 兼容计入当前承诺。
- **v1.33（2026-11-04）**：恢复 CUVID shim 实现，parser 改为复用 FFmpeg 内部解析（`ff_h2645_packet_split` + `ff_h264_decode_seq_parameter_set`/`ff_h264_decode_picture_parameter_set`），替代原手写 SPS 位解析；`libnvcuvid.so.1` 直接链接 FFmpeg 7.1 的 8 个解析对象与共享 libavutil。修复三处缺陷——起始码边界、`nal_ref_idc` 读错字节、空 DPB 的 `erase` 未定义行为；parser 在 AMD 验证机通过 I-only 与 I/P/B 码流各 30 帧解析测试。P0-6 由「暂停」改为「进行中」，真实 decode/map 像素链路与 FFmpeg 双路径仍未验收。
- **v1.34（2026-11-05）**：补充 Vulkan Video 两个短板的结论收口——① §4.4 明确 V3「只支持 OPTIMAL tiling」为**规范锁死 + 硬件寻址**双重结构性约束（非 bug、非等驱动可解），并给出三条候选路线取舍：改 Mesa 出 LINEAR（违规、永久 fork，不推荐）、改 `hipImportExternalMemory` 读 tiled（硬件寻址层 + VK 布局不透明，走不通）、接受一次 compute shader 转换（合规可上游，推荐），附转换实现要点与成本口径；② §4.12 明确异步同步不应修 `hipImportExternalSemaphore`（opaque fd 跨栈不互认，死胡同），正解走 `VK_KHR_external_fence_fd`→sync_file→syncobj→HIP queue wait fence 的标准桥接，缺口在 ROCr 用户态；③ 澄清 VK 作为后端时验收宿主仍是 `h264_cuvid`/`h264_nvdec`（非 `h264_vulkan`，后者仅环境冒烟/语义参考），shim 应抽象解码后端层；④ 同步更新优化版 v2.1。
- **v1.35（2026-11-05）**：勘误 §4.4「转换实现要点」——`vkCmdCopyImageToBuffer` 对源 image **无 LINEAR 限制，OPTIMAL 也可用**（要求的是 `TRANSFER_SRC_OPTIMAL` layout，非 tiling）。转换首选方案由「自写 compute shader」更正为「`vkCmdCopyImageToBuffer` 硬件拷贝」，并补三个参考实现（FFmpeg `vulkan_transfer_frame`、GStreamer `vkdownload.c`、AMD `vulkan_interop`）与四条实现条件；同步更新优化版 v2.2。
- **v1.36（2026-11-06）**：回写 Vulkan Video P0 闭环结论——`tests/zerocopy/verify_vk_decode_to_hip`（对标 VAAPI 版实验 C）在 AMD RX 7900 XTX 跑通「VK 解码→`vkCmdCopyImageToBuffer` 拷贝→`vkGetMemoryFdKHR` 导出→`hipImportExternalMemory` 读回」，I/IP/IPB 各 30 帧逐字节一致 + 无 fd/显存泄漏。沉淀 5 个实现约束：decode/transfer 双队列 + semaphore 桥接、DPB/output 图分离（VCN<5）、`pSetupReferenceSlot` 每帧必填（B 帧用临时 slot）、免手写 RefPicList 重排（驱动自行排）但需维护 active 参考集合、码流 buffer Annex-B 单 slice 复用 + range 对齐。§4.4、§5.1、§7.3 同步更新；Vulkan Video 维持 Tier 2 备选（未接回 CUVID/FFmpeg 宿主）。
- **v1.37（2026-11-07）**：回写 CUVID shim Vulkan Video 后端接入结论——`cuda_shim/` 的 `libnvcuvid.so.1` 新增 `vulkan_h264_decoder.{h,cpp}`，经 `DecoderBackend` 接口 + `backend_dispatch` 工厂与 VAAPI 后端并列，`CUVID_SHIM_BACKEND=vk` 运行时切换（默认 VAAPI）；SPS/PPS 由 `CuvidParser` 经 `VkSequenceCache`（前置 start code）转给后端懒建 session。AMD RX 7900 XTX 实测：I/IP/IPB 各 30 帧逐字节一致 + fd 无泄漏，FFmpeg `h264_cuvid` 宿主 30 帧 0 decode error。§4.4、§5.1、§7.3 同步更新；仍为 Tier 2 备选——默认 VAAPI，`h264_nvdec` 零拷贝、多流/seek/flush、crop、动态分辨率及 HEVC/VP9/AV1 未在此后端验收。
