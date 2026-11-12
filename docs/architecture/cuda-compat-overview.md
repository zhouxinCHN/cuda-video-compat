# CUDA 兼容层总体方案（总纲）

> 版本：v0.1
> 生成时间：2026-10-29
> 目标：让基于 CUDA SDK 开发的产品，在自研驱动环境（Mesa + HIP）上**重编译后能跑**，落到自研 GPU（非 N 卡）。
> 兼容层级：**源码级重编译**（不要求严格二进制 drop-in）。

---

## 1. 核心架构：双轨并行

用户的 CUDA 产品代码分两类，**两条兼容路线都要保留**，不是二选一：

```
用户 CUDA 产品源码
        │
        ├─ A. 调用 NVIDIA 的库（cuvid / nvenc / NPP / cuBLAS…）
        │      └─ 轨 2：自研 shim 库，重编译时链接到自研实现
        │
        └─ B. 用户自研 CUDA kernel（__global__ 函数、cudaMalloc、kernel<<<>>>）
               └─ 轨 1：HIPIFY 把 CUDA 源码翻译成 HIP 源码，hipcc 编译
```

| 轨道 | 工具/产物 | 管什么 |
|---|---|---|
| **轨 1：HIPIFY** | HIPIFY 翻译工具 + HIP 运行时 | **用户自己写的 CUDA kernel**（计算逻辑） |
| **轨 2：shim 库** | libcuda.so.1 / libnvcuvid.so.1 / libnvidia-encode.so.1 / libnpp*.so | **NVIDIA 库的 API**（用户调用的现成库） |

---

## 2. 为什么 NVDEC/NVENC/NPP 不能靠 HIPIFY（关键认知）

HIPIFY 是**源码翻译工具**，前提是"存在 HIP 对应物"：

| 调用 | HIP 对应物 | HIPIFY 能否翻译 |
|---|---|---|
| `cudaMalloc` | `hipMalloc`（HIP 运行时有） | ✅ |
| `cublasGemm` | `hipblasGemm`（ROCm 有 hipBLAS） | ✅ |
| `cuvidCreateDecoder` | **无**（ROCm 无视频解码 HIP 库） | ❌ |
| `nvEncEncodePicture` | **无**（ROCm 无编码 HIP 库） | ❌ |
| `nppiResize` | **无**（ROCm 无 NPP 等价库） | ❌ |

**所以 NVDEC/NVENC/NPP 必须自研 shim 库**：
- 用户对库的调用 → 链接到自研 shim（不是 HIPIFY 翻译）。
- NPP 尤其要"替 NVIDIA 把库重写一遍"（NPP 闭源，无源码可翻译，只能 HIP kernel 重实现）。

**HIPIFY 只负责"用户自研 kernel"那部分**，与 shim 库配合，不冲突。

---

## 3. 组件清单

### 3.1 轨 2：shim 库（本项目主体）

| 产物 | 覆盖接口 | 实现路径 | 状态 |
|---|---|---|---|
| `libcuda.so.1` | CUDA driver API（82 必需符号） | 62 映射 HIP + 20 stub | ✅ 已实现+验证 |
| `libnvcuvid.so.1` | cuvid 解码（底层 12 + parser/source 层） | → VAAPI（libva）/ Vulkan Video，parser 复用 FFmpeg | P0 数据通路已验证（双后端四路码流 82/82） |
| `libnvidia-encode.so.1` | nvEncodeAPI（2 入口 + 34 函数表） | → VAAPI（EncSlice） | 方案已定 |
| `libnpp*.so.11`（11 子库） | NPP API（nppi/npps） | HIP kernel 重实现 | 方案已定 |

### 3.2 轨 1：HIPIFY（用户 kernel 翻译）

- 用 `third_party/HIPIFY`（ROCm HIPIFY 源码）。
- 把用户 `.cu` 里的 CUDA kernel + runtime 调用翻译成 HIP。
- 翻译后的 HIP 代码链接 HIP 运行时 + 上面的 shim 库。

---

## 4. 关键决策（已确认）

| 决策点 | 结论 |
|---|---|
| 兼容层级 | 源码级重编译（不要求严格二进制） |
| GPU ISA | GFX/AMDGPU 系 |
| NPP 头文件路线 | **路线 A**：shim 只产 `.so`，用户自带 CUDA SDK 头文件（构建时用授权头文件，产物不含） |
| parser 复用 | **静态链接 libavcodec.a 解析 SPS/PPS + 移植 vaapi_h264.c 字段映射自写 VAAPI 填充**（vaapi_h264.c 是 static 不能直接提取） |
| HIPIFY 与 shim | **双轨都保留**（用户场景未定，自研 kernel 与库调用都可能存在） |

---

## 5. 子文档索引

| 文档 | 内容 |
|---|---|
| [docs/architecture/nvdec-vaapi-comparison.md](nvdec-vaapi-comparison.md) | 解码格式矩阵实测（NVDEC vs VAAPI） |
| [docs/architecture/npp-vpe-comparison.md](npp-vpe-comparison.md) | NPP vs VPE 能力对比 |
| [docs/architecture/cuvid-vaapi-mapping.md](cuvid-vaapi-mapping.md) | cuvid/nvenc → libva 映射 + parser 落地路径 + libcuda 实现状态 |
| [docs/architecture/npp-compat-plan.md](npp-compat-plan.md) | NPP 兼容层完整方案（路线 A、库组织、HIP kernel、分阶段） |

---

## 6. 分阶段路线图（双轨视角）

```
轨 2（shim，主线）：
  M1  libcuda.so.1（✅ 已完成）
  M2  parser 模块（静态链接 libavcodec，H264 优先）+ libnvcuvid 底层
  M3  libnvcuvid parser 层集成 + FFmpeg h264_cuvid 端到端
  M4  libnvidia-encode 编码 shim
  M5  libnpp* P0（FFmpeg scale_npp 3 函数）→ P1/P2

轨 1（HIPIFY，按需并行）：
  当确认目标用户有自研 CUDA kernel 时启动；
  用 HIPIFY 翻译用户 kernel，与 shim 库联合验证。
```

**当前重心在轨 2**（shim 库是 NVIDIA 库兼容的唯一路径）；轨 1（HIPIFY）保留，待用户场景明确后启动。

---

## 7. 数据通路与零拷贝（关键架构问题）

### 7.1 CUDA 计算部分用什么

用户的典型流程是 **解码 → NPP → CUDA 计算 → 编码**。其中"CUDA 计算"（AI 推理、自定义图像处理等）是**用户自己写的 kernel**，不是调用 NVIDIA 的库，所以：

- **用 HIPIFY 把用户的 CUDA kernel 翻译成 HIP kernel**（轨 1），在 HIP 运行时上跑。
- **不用 shim**——shim 是给"NVIDIA 现成的库"做兼容的；用户自研 kernel 没有"库"可兼容，只能翻译源码。

### 7.2 数据通路：两套驱动栈之间的流动

```
解码(nvdec)      NPP           CUDA计算        编码(nvenc)
   │              │               │               │
   ▼              ▼               ▼               ▼
VAAPI surface → HIP 设备内存 → HIP 设备内存  →  VAAPI surface
（Mesa 管理）   （ROCm 管理）  （ROCm 管理）    （Mesa 管理）
     │              │               │               │
     └─ 边界1 ──────┘               └───── 边界2 ───┘
      （跨栈）        （同栈 HIP→HIP）     （跨栈）
```

| 环节 | 数据在哪 | 是否同栈 | 零拷贝难度 |
|---|---|---|---|
| 解码 → NPP | VAAPI surface（Mesa）→ HIP 内存（ROCm） | **跨栈** | 需打通（边界 1） |
| NPP → CUDA 计算 | HIP 内存 → HIP 内存 | **同栈** | ✅ **天然零拷贝**（同一 HIP 设备指针直接用） |
| CUDA 计算 → 编码 | HIP 内存（ROCm）→ VAAPI surface（Mesa） | **跨栈** | 需打通（边界 2） |

**NPP → CUDA 计算是"免费"的零拷贝**（同为 HIP 内存）；真正要打通的是两个**跨栈边界**（Mesa VAAPI 内存 ↔ ROCm HIP 内存）。

### 7.3 零拷贝技术路径

**边界 1：解码 → NPP（VAAPI surface → HIP 内存）**
```
VAAPI surface
  → vaExportSurfaceHandle(DRM_PRIME)      [Mesa 已支持]
  → DRM PRIME fd
  → hipImportExternalMemory(OpaqueFd)     [HIP 已支持]
  → hipExternalMemoryGetMappedBuffer
  → HIP 设备指针（NPP kernel 直接读）
```
技术上成立（Mesa 支持 DRM PRIME export、HIP 支持 OpaqueFd import，均已核实）。

**边界 2：CUDA 计算 → 编码（HIP 内存 → VAAPI surface）**
```
HIP 设备内存
  → 导出为 DMA-BUF fd                     [??? 待验证]
  → vaCreateSurfaces(DRM_PRIME) 导入为 VAAPI surface
  → VAAPI 编码
```
**关键不确定性在"HIP 内存导出为 DMA-BUF fd"这一步**：HIP 有 import（hipImportExternalMemory），但 export 路径不直接。

### 7.4 最大风险点（须优先验证）

零拷贝方案的可行性，卡在两个**未实测的底层问题**：

1. **ROCm 和 Mesa 能否同时驱动同一块 GPU？** HIP（ROCr）和 VAAPI（radeonsi）是两个独立用户态驱动栈，同基于 amdgpu 内核驱动。同一进程里同时用 HIP 和 VAAPI，能否共存、内存能否通过 DMA-BUF 互操作？**这是整个方案成立的前提。**
2. **HIP 设备内存能否导出为 DMA-BUF fd？**（边界 2 的关键）若 HIP 只能 import 不能 export，"CUDA 计算 → 编码"就没法 zero-copy。

### 7.5 降级方案（零拷贝不通时）

退回**拷贝路径**（功能正确，性能有损）：
```
解码→NPP：vaGetImage(surface→host) + hipMemcpyHtoD(host→HIP)
NPP→CUDA：零拷贝（同栈，不变）
CUDA→编码：hipMemcpyDtoH(HIP→host) + 上传到 VAAPI surface
```
1080p NV12 一帧约 3MB，跨栈拷贝增加延迟，但功能可行。

### 7.6 动工前验证实验（建议第一个做）

**"ROCm + Mesa 同 GPU 零拷贝验证"**——在 AMD 机器上，用 HIP 分配一块内存、用 VAAPI 创建一个 surface，验证两者能否通过 DMA-BUF 互导。

- 这直接决定整个方案是"零拷贝"还是"拷贝"。
- 一两天可完成，**强烈建议在写大量 shim 代码之前先做**。
- 若验证通过，零拷贝路径成立；若不通，提前转向拷贝设计，避免返工。
- **可执行实验清单见 [docs/verification/zero-copy-experiment.md](../verification/zero-copy-experiment.md)**（含环境准备、5 个实验、检查点、结果判定，RX 7600 即可做）。
