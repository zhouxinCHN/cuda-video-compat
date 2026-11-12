# NPP 兼容层完整方案（HIP 实现）

> 版本：v0.1（方案稿，未写实现代码）
> 生成时间：2026-10-29
> 目标：让基于 NPP 的 CUDA 应用，在自研驱动环境上**重编译后能跑**（不要求严格二进制兼容）。
> 实现底座：HIP kernel（落在 GFX 通用计算引擎），**不依赖 VPE 硬件**。
> 上位文档：[docs/architecture/cuda-compat-overview.md](cuda-compat-overview.md)（HIPIFY + shim 双轨架构，本文属"轨 2：shim 库"）

---

## 1. 定位与边界

NPP（NVIDIA Performance Primitives）是 CUDA 通用图像处理原语库，本质是**一组 GPU kernel**。与 VPE（视频色彩专用硬件管线）能力重叠很少（你报告结论：26 项里仅 6 项重叠）。

**因此 NPP 兼容层 = 用 HIP kernel 逐函数重实现 NPP API，跑在通用计算引擎上。** VPE 只在极少数重叠项上可选 offload，主体不碰。

---

## 2. 法律/许可路线（动工前必须定）

### 2.1 核心问题：NPP 头文件是专有许可

| 组件 | 头文件许可 | 能否复制进 shim |
|---|---|---|
| cuvid/nvenc | nv-codec-headers = **MIT** | ✅ 可直接 `#include` |
| **NPP（nppi*.h/npps*.h/nppdefs.h）** | **NVIDIA 专有（PROPRIETARY）** | ❌ 不能复制/再分发 |

NPP 头文件明确写了 "PROPRIETARY ... reproduction or disclosure ... without the express [permission]"。**不能像 cuvid 那样直接复制头文件。**

### 2.2 两条可行路线

**路线 A：用户自带 CUDA SDK 头文件（推荐，零法律风险）**

- shim **只提供 `.so` 实现**，不提供任何 NPP 头文件。
- 用户重编译时，`#include <nppi.h>` 用的是**他自己 CUDA SDK 里的头文件**（他有合法授权），链接时指向我们的 `libnpp*.so`。
- 我们完全不碰 NVIDIA 头文件，只保证**函数签名/ABI 兼容**。
- 这是 cuDNN/cuBLAS 等第三方兼容层的通行做法。

**路线 B：clean-room 重声明（需法务确认）**

- 自己写一套 `nppi.h`，**只声明函数签名**（不写 NVIDIA 的注释/文档/实现）。
- 依据"函数签名/接口不受版权保护"原则，但**函数名、参数顺序、结构体布局必须与 NVIDIA 完全一致**（ABI 兼容要求）。
- 法律风险比路线 A 高，需法务出具意见。

**建议：路线 A。** shim 里只写 `extern "C"` 的函数定义，头文件用用户自带的。

### 2.3 路线 A 的落地细节（已确认采用）

路线 A 下，**"shim 构建时也要用 NPP 头文件"和"shim 产物不含 NPP 头文件"是两件不冲突的事**，这是路线 A 合法且可行的关键：

| 环节 | 用什么 NPP 头文件 | 合法性 |
|---|---|---|
| **shim 构建（编译期）** | 构建机上已授权的 CUDA SDK 的 `nppi.h`（`#include <nppi.h>`） | ✅ 合法：使用方有授权，仅用于编译，不再分发 |
| **shim 产物（分发）** | **只含 `.so`，不含任何 NPP 头文件** | ✅ 零法律风险 |
| **用户编译应用** | 用户自己 CUDA SDK 的 `nppi.h`，链接 shim 的 `.so` | ✅ 用户有授权 |

**为什么 ABI 能保证一致**：shim 的 `.cpp` 里 `#include <nppi.h>`（构建机的 CUDA SDK 头文件），函数定义的签名直接来自该头文件；用户编译时也用 NVIDIA 的 `nppi.h`（同一套签名）。两边签名天然一致，ABI 兼容。

**注意**：
- shim 构建机需要装 CUDA SDK（仅为头文件）。本机 `/usr/include/nppi*.h` 已有（NPP 11.5），可用于构建。
- shim 的 `.so` 导出符号名 = NVIDIA NPP 的函数名（`nppiResizeSqrPixel_8u_C1R` 等），C 链接（`extern "C"`）。
- **不需要**自己维护一份 NPP 头文件——这是路线 A 相对路线 B 的最大省事点。

---

## 3. 库组织与 SONAME

### 3.1 NPP 子库清单（11 个 .so）

| 子库 | 职责 | FFmpeg 是否链接 |
|---|---|---|
| `libnppc.so.11` | core（初始化/版本/stream context/错误码） | ✅ |
| `libnpps.so.11` | signal processing（信号处理） | |
| `libnppig.so.11` | geometry transforms（几何变换/缩放/旋转） | ✅ |
| `libnppicc.so.11` | color conversion（色彩转换） | ✅ |
| `libnppidei.so.11` | deinterlacing（去隔行） | ✅（链接但函数未用） |
| `libnppif.so.11` | filtering（滤波） | ✅（链接但函数未用） |
| `libnppim.so.11` | morphological（形态学） | |
| `libnppial.so.11` | arithmetic/logical（算术逻辑） | |
| `libnppitc.so.11` | threshold/compare（阈值比较） | |
| `libnppist.so.11` | statistics（统计） | |
| `libnppisu.so.11` | support functions（支持） | |

### 3.2 SONAME 规则

- NPP 是**编译时链接**（与 cuvid/nvenc 的 dynlink 不同）。
- FFmpeg 二进制 `ldd` 直接依赖 `libnppig.so.11` 等（**带 CUDA 大版本号 .11**）。
- shim 产物的 SONAME 必须是 `libnpp<子库>.so.11`，否则 FFmpeg 加载不到。
- 部署时放 `LD_LIBRARY_PATH` 或 `ldconfig` 路径，**优先级要高于系统 CUDA NPP**（或干脆没有系统 NPP）。

### 3.3 最小可行库集（MVP）

让 FFmpeg `scale_npp` 跑通，只需实现 **3 个非空库 + 2 个占位库**：

| 库 | 需实现的函数 | 说明 |
|---|---|---|
| `libnppc.so.11` | `nppGetStreamContext`/`nppSetStream`/`nppGetVersion` 等初始化函数 | 核心，必须 |
| `libnppig.so.11` | `nppiResizeSqrPixel_8u_C1R` | scale_npp 缩放 |
| `libnppicc.so.11` | `nppiYCbCr420_8u_P2P3R` + `nppiYCbCr420_8u_P3P2R` | NV12↔planar 转换 |
| `libnppidei.so.11` | 占位（导出空符号集即可） | FFmpeg 链接但未调用 |
| `libnppif.so.11` | 占位 | 同上 |

---

## 4. 函数签名坑（NPP 比 nvdec 更繁琐的地方）

NPP 不是"实现 kernel 就完事"，**辅助参数语义必须全对齐**，否则编译不过或运行错。

### 4.1 NppStreamContext（现代 NPP 核心）

现代 NPP（CUDA 11+）大量函数用 `NppStreamContext` 而非裸 `CUstream`：

```c
typedef struct {
    void*  hStream;         // CUDA stream
    int    nCudaDeviceId;
    int    nMultiProcessorCount;
    int    nMaxThreadsPerMultiProcessor;
    int    nMaxThreadsPerBlock;
    size_t nSharedMemPerBlock;
    int    nCudaDevAttrComputeCapabilityMajor;
    int    nCudaDevAttrComputeCapabilityMinor;
    unsigned int nStreamFlags;
    int    nReserved0;
} NppStreamContext;
```

- shim 的 `nppGetStreamContext` 要**用 HIP 填充这些字段**（hipGetDeviceProperties/hipDeviceGetAttribute）。
- 旧式 API（裸 `CUstream`）也要支持（部分老函数只有 stream 版本）。

### 4.2 ROI / mask / scratch buffer

- **ROI**：大量函数带 `NppiRect oSrcROI/oDstROI`（感兴趣区域），实现时要按 ROI 裁剪，不是全图。
- **mask**：部分函数带 `Npp8u* pMask`（像素级掩码）。
- **scratch buffer**：很多函数要求先 `nppi<Op>GetBufferSize(...)` 查询，再分配 scratch 传入。shim 要实现配套的 GetBufferSize（返回 HIP kernel 实际需要的临时显存大小，可为 0 若不需要）。
- **_Host 版本**：`nppi*_Host_*` 是 CPU 实现，可直接在 host 上算（不走 GPU）。
- **pointer mode**：`NppPointerMode`（device vs host pointer），影响入参指针的解释。

### 4.3 基础类型（必须先对齐）

任何 NPP 函数都依赖这些类型，ABI 必须一致：
- `NppStatus`（错误码枚举，`NPP_SUCCESS=0` 等）
- `Npp8u/Npp16u/Npp32f/Npp64f` 等标量类型（`uint8_t/uint16_t/float/double`）
- `NppiSize`（`{int width, height}`）
- `NppiRect`（`{int x, y, width, height}`）
- `NppStreamContext`（见 4.1）

---

## 5. 分阶段实现路线

### 阶段 P0：FFmpeg 依赖最小集（先跑通 scale_npp）

| 函数 | 所属库 | HIP kernel 要点 |
|---|---|---|
| `nppGetStreamContext`/`nppSetStream`/`nppGetVersion` | nppc | host 侧填结构体 |
| `nppiResizeSqrPixel_8u_C1R` | nppig | 每线程一目标像素，反向映射源坐标 + 双线性插值，处理 ROI |
| `nppiYCbCr420_8u_P2P3R` | nppicc | NV12（2 平面）→ YUV420（3 平面），UV 去交织 |
| `nppiYCbCr420_8u_P3P2R` | nppicc | YUV420（3 平面）→ NV12（2 平面），UV 交织 |

> 这 3 个 NPP 函数 + nppc 初始化，就能让 FFmpeg `scale_npp` 完整跑通。

### 阶段 P1：重叠 6 项（解码→推理前处理主链路）

缩放（多插值）、水平镜像、YUV420/NV12→RGB/BGR、位深转换、阈值、AbsDiff/AddWeighted。

### 阶段 P2：NPP 独有 16 项

任意旋转/仿射/透视、垂直镜像、形态学、统计、滤波、YUV422→RGB、RGB→YUV420。

### 差异化（可选，不进 NPP 兼容层）

VPE 的 HDR 强项（色调映射/3D LUT/色域/伽马）作为独立扩展 API 暴露（NPP 没有这些）。

---

## 6. HIP kernel 实现要点

### 6.1 launch 路径（关键）

**NPP 库不要走 `libcuda.so.1` 的 `cuLaunchKernel`（那是 stub）。** NPP 的 `.so` 直接链 `libamdhip64.so`，用 HIP 的：

- `hipLaunchKernel`（运行时 API），或
- `hipModuleLaunchKernel`（driver API，kernel 编译成 HSACO 后加载）。

NPP kernel 用 HIP C++ 写（`.hip` 源文件），hipcc 编译成 device code，打进各 `libnpp*.so`。

### 6.2 通用 kernel 模式

- **逐像素 kernel**：颜色转换、位深、阈值、AbsDiff —— 每线程一像素，读源写目标，处理 ROI/pitch。
- **几何 kernel**：缩放/旋转/仿射 —— 反向映射 + 插值（线性/立方）。
- **规约 kernel**：统计（均值/直方图）—— block 内规约 + 原子/二阶段规约。
- **滑动窗口 kernel**：滤波/形态学 —— shared memory tile + halo。

### 6.3 设备指针一致性

NPP 的入参是 **CUDA 设备指针（`Npp8u*`）**。HIP 设备指针（`hipDeviceptr_t`）与之类型兼容（都是 GPU 虚拟地址）。**不需要额外转换**，直接当 HIP 指针用。

---

## 7. 与 nvdec 的衔接（zero-copy 链路）

典型链路：NVDEC 解码输出 NV12 → NPP 缩放/转色 → 推理。

```
cuvidDecodePicture (libva 解码到 VAAPI surface)
    → cuvidMapVideoFrame64（surface → HIP 设备指针）
    → NPP kernel 直接读该指针（NV12）
    → NPP 输出（HIP 设备指针）
```

关键依赖：`cuvidMapVideoFrame64` 返回的必须是**可被 HIP kernel 直接寻址的设备指针**。这就是之前确认的 zero-copy 链路：

```
VAAPI surface → vaExportSurfaceHandle(DRM_PRIME) → fd
    → hipImportExternalMemory(OpaqueFd) → hipExternalMemoryGetMappedBuffer → HIP 设备指针
```

**若 zero-copy 未就绪**，先走拷贝（`vaGetImage`→host→`hipMemcpyHtoD`），功能正确但性能有损。

---

## 8. 验证方法

| 阶段 | 验证 | 硬件 |
|---|---|---|
| P0 | FFmpeg `scale_npp`（`-vf scale_npp=...`）跑通 NV12 缩放/转色 | AMD 机器 |
| P1/P2 | 自写 NPP 测试程序（对照 NVIDIA NPP 输出，逐函数比对像素） | AMD + NVIDIA 对照 |
| zero-copy | `nvdec_benchmark` 解码 → NPP 处理链路 | AMD 机器 |

---

## 9. 风险与待办

1. **法务路线**（路线 A/B）必须先定——决定 shim 是否含任何 NPP 头文件。
2. **NppStreamContext 字段映射**：hipGetDeviceProperties 能否填全所有字段（尤其 MultiProcessorCount/ComputeCapability）需实测。
3. **scratch buffer / GetBufferSize 语义**：逐函数确认是否需要真实临时显存。
4. **NPP 12 子库全覆盖**：P0 只需 5 个库，完整兼容需 11 个库全部产出（占位库也要导出符号，否则链接失败）。
