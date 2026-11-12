# NPP 兼容方案与 VPE 后处理对比

> 版本：v1.0（合并原《NPP 兼容层完整方案》与《NPP 与 VPE 视频后处理能力对比报告》）
> 文档日期：2026-11-12
> 目标：让基于 NPP 的 CUDA 应用在自研驱动环境上重编译后能跑；P0 只覆盖 FFmpeg `scale_npp` 实际使用的 3 个函数。
> 实现底座：HIP kernel（落在 GFX 通用计算引擎），不依赖 VPE 硬件。
> 职责：本文是 NPP 能力边界、VPE 对比、许可路线、库组织与实现方案的唯一权威文档。P0 符号范围见 [CUVID 到 VAAPI 映射表](CUVID到VAAPI映射表.md) §0.4。
> 状态：方案与 NVIDIA 侧能力实测已完成；**NPP shim 尚未写实现代码，P0 联合链路未通过**。

---

# 第一部分　NPP 与 VPE 能力对比

## 1. 分析背景与方法

| 技术栈 | 定位 | 分析方法 |
|---|---|---|
| **NPP**（NVIDIA Performance Primitives） | NVIDIA CUDA 通用图像处理原语库 | 26 项 API 直测（`npp_benchmark`） |
| **VPE**（Video Processor Engine） | AMD 独立视频后处理硬件 IP | Mesa 源码分析（vpelib + radeonsi + VAAPI 前端） |

VPE 的权威能力清单不在黑盒测试里，而在 `vpe_caps` 结构体和 `vpe10/vpe11_resource.c` 的实际填充中。

## 2. NPP 能力清单（26 项 API 直测）

NPP 是 NVIDIA 的 CUDA 通用图像处理原语库，本质是一组 GPU kernel，覆盖图像处理、信号处理、颜色转换等领域。上游提供 **11 个 `.so` 子库**（`nppc` / `npps` / `nppig` / `nppim` / `nppitc` / `nppial` / `nppist` / `nppidei` / `nppisu` / `nppif` / `nppicc`）；`nppi` 是伞形头文件，不是独立 `.so`。

直测环境：RTX 4060、NPP 11.5.1.107、1920×1080 @ 50 次迭代。26 项 API 全部成功。

### 2.1 几何变换（7 项）

| 能力 | NPP API | 实测耗时 |
|---|---|---|
| 缩放（灰度，线性插值） | `nppiResizeSqrPixel_8u_C1R` | 0.010 ms |
| 缩放（RGB，立方插值） | `nppiResizeSqrPixel_8u_C3R` | 0.037 ms |
| 水平镜像 | `nppiMirror_8u_C1R`（HORIZONTAL） | 0.006 ms |
| 垂直镜像 | `nppiMirror_8u_C1R`（VERTICAL） | 0.006 ms |
| 旋转 45° | `nppiRotate_8u_C1R` | 0.039 ms |
| 仿射变换 | `nppiWarpAffine_8u_C1R` | 0.011 ms |
| 透视变换 | `nppiWarpPerspective_8u_C1R` | 0.012 ms |

NPP 的几何变换支持任意角度，这是其区别于固定角度硬件单元的核心优势。

### 2.2 滤波（5 项）

| 能力 | NPP API | 实测耗时 |
|---|---|---|
| 锐化（3x3 核） | `nppiFilterBorder_8u_C1R` | 0.125 ms |
| 中值滤波 3x3 | `nppiFilterMedian_8u_C1R` | 0.182 ms |
| 高斯滤波 3x3 | `nppiFilterGauss_8u_C1R` | 0.012 ms |
| Sobel 水平边缘 | `nppiFilterSobelHoriz_8u_C1R` | 0.011 ms |
| Canny 边缘检测 | `nppiFilterCannyBorder_8u_C1R` | 0.118 ms |

### 2.3 色彩转换（5 项）

| 能力 | NPP API | 实测耗时 |
|---|---|---|
| YUV420 → RGB（平面） | `nppiYUV420ToRGB_8u_P3C3R` | 0.026 ms |
| NV12 → RGB（半平面） | `nppiNV12ToRGB_8u_P2C3R` | 0.019 ms |
| NV12 → BGR（半平面） | `nppiNV12ToBGR_8u_P2C3R` | 0.019 ms |
| YUV422 → RGB（4:2:2） | `nppiYUV422ToRGB_8u_P3C3R` | 0.028 ms |
| RGB → YUV420（平面） | `nppiRGBToYUV420_8u_C3P3R` | 0.030 ms |

### 2.4 位深转换（2 项）

| 能力 | NPP API | 实测耗时 |
|---|---|---|
| 8u → 16u | `nppiConvert_8u16u_C1R` | 0.029 ms |
| 8u → 32f | `nppiConvert_8u32f_C1R` | 0.027 ms |

### 2.5 算术 / 融合（2 项）

| 能力 | NPP API | 实测耗时 |
|---|---|---|
| 帧差（运动检测） | `nppiAbsDiff_8u_C1R` | 0.006 ms |
| 加权叠加（8u→32f） | `nppiAddWeighted_8u32f_C1IR` | 0.020 ms |

### 2.6 形态学（2 项）

| 能力 | NPP API | 实测耗时 |
|---|---|---|
| 膨胀 3x3 | `nppiDilate_8u_C1R` | 0.097 ms |
| 腐蚀 3x3 | `nppiErode_8u_C1R` | 0.037 ms |

### 2.7 统计（2 项）

| 能力 | NPP API | 实测耗时 |
|---|---|---|
| 均值 / 标准差 | `nppiMean_StdDev_8u_C1R` | 0.170 ms |
| 直方图（256 级） | `nppiHistogramEven_8u_C1R` | 0.014 ms |

### 2.8 阈值（1 项）

| 能力 | NPP API | 实测耗时 |
|---|---|---|
| 阈值（>128→255） | `nppiThreshold_Val_8u_C1R` | 0.006 ms |

### 2.9 NPP 的格式盲区

| 盲区 | 说明 |
|---|---|
| **RGB → NV12 不存在** | 只有 `nppiRGBToYUV420`（平面 YUV420），无 RGB→NV12（半平面）。编码前处理需额外格式转换 |
| **AddWeighted 无纯 8u 版本** | 只有 `nppiAddWeighted_8u32f_C1IR`（8u 源 → 32f 目标 in-place），无纯 8u 快速融合 |

NPP 覆盖了“解码输出（NV12）→ 显示/推理（RGB）”的正向链路，但“显示/推理（RGB）→ 编码输入（NV12）”的反向链路缺失。

## 3. VPE 硬件能力清单（源码证据）

VPE 是 AMD 的独立视频后处理硬件，从 DRAM 到 DRAM 处理视频，目的是把后处理从 GFX 卸载下来省功耗。三层架构：

```text
VAAPI 前端 (gallium/frontends/va)      ← 对外暴露面（libva 接口）
        ↓
VPE 驱动后端 (radeonsi/si_vpe.c)       ← VPP 参数 → VPE 参数映射
        ↓
VPELIB 库 (amd/vpelib)                 ← 硬件描述符/命令构造
        ↓
VPE 硬件（固件执行）
```

### 3.1 完整能力清单

#### 几何变换

| 能力 | 源码证据 | 状态 |
|---|---|---|
| 缩放（polyphase 多抽头） | `scaling_info`，最大上采样 64x（`max_upscale_factor=64000`） | ✅ |
| 下采样 | 4:1（VPE 1.0）~ 6:1（VPE 1.1） | ✅ |
| 几何缩放（多 pass） | `vpe_geometric_scaling_feature_skip` | ✅ |
| 水平镜像 | `h_mirror_support=1`，`check_h_mirror_support` 放行 | ✅（输出端实现） |
| 垂直镜像 | `v_mirror_support=0` | ❌ |
| 旋转 90/180/270 | `rotation_support=0` | ❌ |

#### 色彩管理

| 能力 | 源码证据 | 状态 |
|---|---|---|
| CSC（色彩空间转换） | `pre_csc=1, post_csc=1, ocsc=1` | ✅ |
| 伽马校正 | ROM 曲线：sRGB、BT.2020、Gamma2.2、PQ、HLG | ✅ |
| 色域重映射 | `gamut_remap=1` | ✅ |
| 色调映射（HDR↔SDR） | `vpe_tonemap_params` + `color.c` 白点增益路径 | ✅ |
| 3D LUT | `hw_3dlut=1`，`lut_size=LUT_BUFFER_SIZE` | ✅ |
| 色彩调整（亮度/对比/色相/饱和） | `vpe_color_adjust` 结构 + `color_cs.c` 实现 | ⚠️ 硬件支持，驱动未接 |
| 色度键控（color key） | `color_key=1` | ✅ |
| 亮度键控（luma key） | `luma_key=0` | ❌ |

#### 混合

| 能力 | 源码证据 | 状态 |
|---|---|---|
| global alpha 混合 | `mpc.global_alpha=1` | ⚠️ 硬件支持，VAAPI 层声明禁用 |

#### 视频专用处理

| 能力 | 源码证据 | 状态 |
|---|---|---|
| 去隔行 | VPE 硬件无此 IP，VAAPI 前端由 GFX compositor 实现 | ❌（不在 VPE） |
| 降噪 | 无对应模块 | ❌ |
| 锐化 | 无对应模块 | ❌ |

### 3.2 像素格式能力

来自 `plane_caps`（`vpe10/vpe11_resource.c`）：

| 格式 | 输入 | 输出 |
|---|:---:|:---:|
| ARGB8888 / XRGB8888 打包 | ✅ | ✅ |
| FP16（浮点 RGB 16bit） | ❌ | ✅ |
| NV12（4:2:0 8bit） | ✅ | ❌ |
| P010（4:2:0 10bit） | ✅ | ❌ |
| P016（4:2:0 16bit） | ❌ | ❌ |
| AYUV（4:4:4 8bit） | ❌ | ❌ |
| YUY2（4:2:2 8bit） | ❌ | ❌ |

**关键限制**：VPE 输出只支持 RGB/FP16，不能输出 YUV。VPE 后处理结果若要继续走视频编码，需要额外格式转换。

### 3.3 VPE 版本差异

| VPE 硬件版本 | IP Level | 架构 | 差异 |
|---|---|---|---|
| VPE 6.1.0 / 6.1.3 | 1.0 | 单管线 | 下采样 4:1 |
| VPE 6.1.1 / 6.1.2 | 1.1 | 双管线 + collaboration mode | 下采样 6:1 |

两代功能矩阵基本一致，仅下采样比率不同。

## 4. 关键发现：VPE 硬件能力 ≫ VAAPI 暴露面

VPE 硬件支持的能力远多于 Mesa VAAPI 前端实际暴露给应用的能力。

### 4.1 VAAPI 前端实际暴露什么

`surface.c` 的 `vlVaQueryVideoProcFilters` 只返回一种滤镜：

```c
filters[num++] = VAProcFilterDeinterlacing;   // ← 唯一
```

其余滤镜查询全部拒绝：`NoiseReduction`、`Sharpening`、`ColorBalance`、`SkinToneEnhancement` 均返回 `VA_STATUS_ERROR_UNIMPLEMENTED`。`si_get.c` 的方向/混合能力也只声明水平翻转、无混合。

### 4.2 能力鸿沟对照表

| 能力 | VPE 硬件 | VAAPI 暴露 | 鸿沟 |
|---|:---:|:---:|---|
| 缩放 | ✅ | ✅ | — |
| 去隔行 | ❌（GFX 实现） | ✅ | VAAPI 反而多（软件栈补） |
| 水平翻转 | ✅ | ✅ | — |
| 垂直翻转 | ❌ | ❌ | — |
| 旋转 | ❌ | ❌ | — |
| CSC | ✅ | ✅（隐式） | — |
| **色调映射 HDR↔SDR** | ✅ | ❌ | **VPE 强项被埋没** |
| **3D LUT** | ✅ | ❌ | **VPE 强项被埋没** |
| **色域重映射** | ✅ | ❌ | **VPE 强项被埋没** |
| **伽马校正（PQ/HLG）** | ✅ | ❌ | **VPE 强项被埋没** |
| 色彩调整（亮度/对比/色相/饱和） | ⚠️ 未接 | ❌ | 双双缺失 |
| 混合 | ⚠️ 硬件支持 | ❌ 禁用 | 双双缺失 |
| 降噪 / 锐化 | ❌ | ❌ | 双双缺失 |

**核心结论**：VPE 最擅长的色调映射、3D LUT、色域重映射、伽马校正，通过标准 VAAPI 接口完全用不到。只有绕过 VAAPI、直接调用 `vpelib`，才能释放这些能力。

## 5. NPP vs VPE 对比

### 5.1 定位差异

| 维度 | NPP | VPE |
|---|---|---|
| 技术形态 | CUDA 通用图像处理原语库 | 独立视频后处理硬件 IP |
| 编程模型 | 逐 API 调用，GPU kernel | 参数描述 + 固件命令流 |
| 面向场景 | AI 推理前处理 + 通用图像 | 视频播放/转码色彩与缩放管线 |
| 数据流 | 任意 CUDA buffer | DRAM→DRAM，固定管线 |

### 5.2 完整逐项对照（26 项全映射）

🔵 重叠（两者都有）、🟢 NPP 独有（VPE 无）、🟠 VPE 近似（语义接近但不等价）。

| # | NPP 能力 | NPP API | VPE 对应能力 | 归属 |
|---|---|---|---|---|
| 1 | 缩放（灰度·线性） | `nppiResizeSqrPixel_8u_C1R` | 缩放（polyphase） | 🔵 重叠 |
| 2 | 缩放（RGB·立方） | `nppiResizeSqrPixel_8u_C3R` | 缩放（polyphase） | 🔵 重叠 |
| 3 | 水平镜像 | `nppiMirror_8u_C1R` | 水平镜像（输出端实现） | 🔵 重叠 |
| 4 | 垂直镜像 | `nppiMirror_8u_C1R` | 无（`v_mirror_support=0`） | 🟢 NPP 独有 |
| 5 | 旋转 45° | `nppiRotate_8u_C1R` | 无（`rotation_support=0`） | 🟢 NPP 独有 |
| 6 | 仿射变换 | `nppiWarpAffine_8u_C1R` | 无 | 🟢 NPP 独有 |
| 7 | 透视变换 | `nppiWarpPerspective_8u_C1R` | 无 | 🟢 NPP 独有 |
| 8 | 锐化（3x3 核） | `nppiFilterBorder_8u_C1R` | 无 | 🟢 NPP 独有 |
| 9 | 中值滤波 3x3 | `nppiFilterMedian_8u_C1R` | 无 | 🟢 NPP 独有 |
| 10 | 高斯滤波 3x3 | `nppiFilterGauss_8u_C1R` | 无 | 🟢 NPP 独有 |
| 11 | Sobel 边缘 | `nppiFilterSobelHoriz_8u_C1R` | 无 | 🟢 NPP 独有 |
| 12 | Canny 边缘 | `nppiFilterCannyBorder_8u_C1R` | 无 | 🟢 NPP 独有 |
| 13 | YUV420→RGB | `nppiYUV420ToRGB_8u_P3C3R` | CSC | 🔵 重叠 |
| 14 | NV12→RGB | `nppiNV12ToRGB_8u_P2C3R` | CSC（NV12 输入 ✅） | 🔵 重叠 |
| 15 | NV12→BGR | `nppiNV12ToBGR_8u_P2C3R` | CSC | 🔵 重叠 |
| 16 | YUV422→RGB | `nppiYUV422ToRGB_8u_P3C3R` | 无（输入不支持 4:2:2） | 🟢 NPP 独有 |
| 17 | RGB→YUV420 | `nppiRGBToYUV420_8u_C3P3R` | 无（输出不支持 YUV） | 🟢 NPP 独有 |
| 18 | 位深 8u→16u | `nppiConvert_8u16u_C1R` | 近似（P010 10bit 输入） | 🟠 VPE 近似 |
| 19 | 位深 8u→32f | `nppiConvert_8u32f_C1R` | 近似（FP16 输出） | 🟠 VPE 近似 |
| 20 | 帧差 AbsDiff | `nppiAbsDiff_8u_C1R` | 无 | 🟢 NPP 独有 |
| 21 | 加权叠加 | `nppiAddWeighted_8u32f_C1IR` | 近似（global alpha，硬件支持未暴露） | 🟠 VPE 近似 |
| 22 | 膨胀 3x3 | `nppiDilate_8u_C1R` | 无 | 🟢 NPP 独有 |
| 23 | 腐蚀 3x3 | `nppiErode_8u_C1R` | 无 | 🟢 NPP 独有 |
| 24 | 均值/标准差 | `nppiMean_StdDev_8u_C1R` | 无 | 🟢 NPP 独有 |
| 25 | 直方图 | `nppiHistogramEven_8u_C1R` | 无 | 🟢 NPP 独有 |
| 26 | 阈值 | `nppiThreshold_Val_8u_C1R` | 近似（color key / luma key） | 🟠 VPE 近似 |

**统计**：🔵 重叠 6 项 · 🟢 NPP 独有 16 项 · 🟠 VPE 近似 4 项（合计 26 项）。

### 5.3 归纳

**重叠 6 项**：缩放、水平镜像、基础色彩转换（YUV420/NV12→RGB/BGR）。NPP 的色彩转换覆盖更宽（额外支持 YUV422→RGB、RGB→YUV420），VPE 的 CSC 受输入/输出格式限制。

**NPP 独有 16 项**：垂直镜像、任意角度旋转、仿射/透视、滤波（锐化/中值/高斯）、边缘检测（Sobel/Canny）、形态学、帧差、统计、YUV422→RGB、RGB→YUV420。VPE 硬件没有这些单元。

**VPE 近似 4 项**：位深 8u→16u（P010 10bit 输入）、8u→32f（FP16 输出）、加权叠加（global alpha 未暴露）、阈值（color key/luma key）。

**VPE 独有（NPP 无）**：色调映射 HDR↔SDR、伽马校正（PQ/HLG/BT2020）、色域重映射、色度键控。

> 对比小结：NPP 是“通用图像处理工具箱”，VPE 是“视频色彩专用管线”。26 项 NPP API 中，与 VPE 真正重叠的仅 6 项，16 项 NPP 独有，4 项 VPE 仅近似支持；VPE 独有 HDR 色彩管理。

## 6. 对自研 SDK 的启示

1. **走 VAAPI 后端会丢失 VPE 色彩管理强项**。色调映射/3D LUT/色域重映射/伽马校正无法通过标准 VAAPI 调用。若 SDK 只定位为兼容 NVDEC 解码，这个丢失可接受；若要覆盖后处理，需评估直接调 `vpelib`。
2. **NVDEC 解码输出的 NV12/P010 恰好是 VPE 的输入格式**，为“兼容解码 + VPE 后处理”提供天然衔接。
3. **VPE 输出不能回 YUV**（只出 RGB/FP16），后处理结果要继续编码需额外格式转换。
4. **VPE 硬件不支持旋转/垂直翻转/去隔行**；这些能力要么软件实现，要么依赖 GFX。

---

# 第二部分　NPP 兼容层实现方案

## 7. 定位与边界

NPP 兼容层 = 用 HIP kernel 逐函数重实现 NPP API，跑在通用计算引擎上。VPE 只在极少数重叠项上可选 offload，主体不碰。

P0 只实现 FFmpeg `scale_npp` 需要的 3 个函数 + nppc 初始化；其余 23 项为 P1/P2。**当前没有 NPP shim 源码，P0 联合链路未通过。**

## 8. 法律/许可路线

### 8.1 核心问题：NPP 头文件是专有许可

| 组件 | 头文件许可 | 能否复制进 shim |
|---|---|---|
| cuvid/nvenc | nv-codec-headers = MIT | ✅ 可直接 `#include` |
| **NPP（`nppi*.h` / `npps*.h` / `nppdefs.h`）** | **NVIDIA 专有（PROPRIETARY）** | ❌ 不能复制/再分发 |

NPP 头文件明确写明不得复制/披露。不能像 cuvid 那样直接复制头文件。

### 8.2 两条可行路线

**路线 A：用户自带 CUDA SDK 头文件（推荐，零法律风险）**

- shim 只提供 `.so` 实现，不提供任何 NPP 头文件。
- 用户重编译时 `#include <nppi.h>` 使用他自己 CUDA SDK 里的头文件，链接时指向 shim 的 `libnpp*.so`。
- shim 只保证函数签名/ABI 兼容。这是 cuDNN/cuBLAS 等第三方兼容层的通行做法。

**路线 B：clean-room 重声明（需法务确认）**

- 自己写一套 `nppi.h`，只声明函数签名，不写 NVIDIA 的注释/文档/实现。
- 依据“函数签名/接口不受版权保护”原则，但函数名、参数顺序、结构体布局必须与 NVIDIA 完全一致。
- 法律风险高于路线 A，需法务出具意见。

**采用路线 A。**

### 8.3 路线 A 的落地细节

| 环节 | 用什么 NPP 头文件 | 合法性 |
|---|---|---|
| shim 构建（编译期） | 构建机上已授权的 CUDA SDK 的 `nppi.h` | ✅ 使用方有授权，仅用于编译，不再分发 |
| shim 产物（分发） | 只含 `.so`，不含任何 NPP 头文件 | ✅ 零法律风险 |
| 用户编译应用 | 用户自己 CUDA SDK 的 `nppi.h`，链接 shim 的 `.so` | ✅ 用户有授权 |

shim 的 `.cpp` 里 `#include <nppi.h>`，函数定义签名直接来自该头文件；用户编译时也用同一套签名，ABI 天然一致。shim 导出符号名 = NVIDIA NPP 函数名，C 链接（`extern "C"`）。不需要自己维护一份 NPP 头文件。

## 9. 库组织与 SONAME

### 9.1 NPP 子库清单（11 个 `.so`）

| 子库 | 职责 | FFmpeg 是否链接 |
|---|---|---|
| `libnppc.so.11` | core（初始化/版本/stream context/错误码） | ✅ |
| `libnpps.so.11` | signal processing | |
| `libnppig.so.11` | geometry transforms | ✅ |
| `libnppicc.so.11` | color conversion | ✅ |
| `libnppidei.so.11` | deinterlacing | ✅（链接但函数未用） |
| `libnppif.so.11` | filtering | ✅（链接但函数未用） |
| `libnppim.so.11` | morphological | |
| `libnppial.so.11` | arithmetic/logical | |
| `libnppitc.so.11` | threshold/compare | |
| `libnppist.so.11` | statistics | |
| `libnppisu.so.11` | support functions | |

### 9.2 SONAME 规则

- NPP 是**编译时链接**（与 cuvid/nvenc 的 dynlink 不同）。
- FFmpeg 二进制 `ldd` 直接依赖 `libnppig.so.11` 等（带 CUDA 大版本号 `.11`）。
- shim 产物的 SONAME 必须是 `libnpp<子库>.so.11`，否则 FFmpeg 加载不到。
- 部署时放 `LD_LIBRARY_PATH` 或 `ldconfig` 路径，优先级要高于系统 CUDA NPP。

### 9.3 最小可行库集（MVP）

让 FFmpeg `scale_npp` 跑通，只需 3 个非空库 + 2 个占位库：

| 库 | 需实现的函数 | 说明 |
|---|---|---|
| `libnppc.so.11` | `nppGetStreamContext` / `nppSetStream` / `nppGetVersion` 等 | 核心，必须 |
| `libnppig.so.11` | `nppiResizeSqrPixel_8u_C1R` | scale_npp 缩放 |
| `libnppicc.so.11` | `nppiYCbCr420_8u_P2P3R` + `nppiYCbCr420_8u_P3P2R` | NV12↔planar 转换 |
| `libnppidei.so.11` | 占位（导出空符号集） | FFmpeg 链接但未调用 |
| `libnppif.so.11` | 占位 | 同上 |

## 10. 函数签名坑

### 10.1 `NppStreamContext`

现代 NPP（CUDA 11+）大量函数用 `NppStreamContext` 而非裸 `CUstream`：

```c
typedef struct {
    void*  hStream;
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

shim 的 `nppGetStreamContext` 要用 HIP 填充这些字段（`hipGetDeviceProperties` / `hipDeviceGetAttribute`）。旧式裸 `CUstream` API 也要支持。

### 10.2 ROI / mask / scratch buffer

- **ROI**：大量函数带 `NppiRect oSrcROI/oDstROI`，实现时要按 ROI 裁剪，不是全图。
- **mask**：部分函数带 `Npp8u* pMask`。
- **scratch buffer**：很多函数要求先 `nppi<Op>GetBufferSize(...)` 查询，再分配 scratch 传入；配套 GetBufferSize 返回 HIP kernel 实际需要的临时显存大小（可为 0）。
- **Host 版本**：`nppi*_Host_*` 是 CPU 实现，可直接在 host 上算。
- **pointer mode**：`NppPointerMode`（device vs host pointer）影响入参指针解释。

### 10.3 基础类型

ABI 必须一致：`NppStatus`、`Npp8u/Npp16u/Npp32f/Npp64f`、`NppiSize`、`NppiRect`、`NppStreamContext`。

## 11. 分阶段实现路线

### P0：FFmpeg 依赖最小集

| 函数 | 所属库 | HIP kernel 要点 |
|---|---|---|
| `nppGetStreamContext` / `nppSetStream` / `nppGetVersion` | nppc | host 侧填结构体 |
| `nppiResizeSqrPixel_8u_C1R` | nppig | 每线程一目标像素，反向映射源坐标 + 双线性插值，处理 ROI |
| `nppiYCbCr420_8u_P2P3R` | nppicc | NV12（2 平面）→ YUV420（3 平面），UV 去交织 |
| `nppiYCbCr420_8u_P3P2R` | nppicc | YUV420（3 平面）→ NV12（2 平面），UV 交织 |

这 3 个 NPP 函数 + nppc 初始化，就能让 FFmpeg `scale_npp` 完整跑通。

### P1：重叠 6 项

缩放（多插值）、水平镜像、YUV420/NV12→RGB/BGR、位深转换、阈值、AbsDiff/AddWeighted。

### P2：NPP 独有 16 项

任意旋转/仿射/透视、垂直镜像、形态学、统计、滤波、YUV422→RGB、RGB→YUV420。

### 差异化（可选，不进 NPP 兼容层）

VPE 的 HDR 强项（色调映射/3D LUT/色域/伽马）作为独立扩展 API 暴露。

## 12. HIP kernel 实现要点

### 12.1 launch 路径

**NPP 库不要走 `libcuda.so.1` 的 `cuLaunchKernel`（stub）。** NPP 的 `.so` 直接链 `libamdhip64.so`，用 `hipLaunchKernel` 或 `hipModuleLaunchKernel`。NPP kernel 用 HIP C++ 写，hipcc 编译成 device code，打进各 `libnpp*.so`。

### 12.2 通用 kernel 模式

- **逐像素 kernel**：颜色转换、位深、阈值、AbsDiff——每线程一像素，读源写目标，处理 ROI/pitch。
- **几何 kernel**：缩放/旋转/仿射——反向映射 + 插值。
- **规约 kernel**：统计（均值/直方图）——block 内规约 + 原子/二阶段规约。
- **滑动窗口 kernel**：滤波/形态学——shared memory tile + halo。

### 12.3 设备指针一致性

NPP 入参是 CUDA 设备指针（`Npp8u*`）。HIP 设备指针（`hipDeviceptr_t`）与之类型兼容（都是 GPU 虚拟地址），不需要额外转换。

## 13. 与解码链路的衔接

典型链路：NVDEC 解码输出 NV12 → NPP 缩放/转色 → 推理。

```text
cuvidDecodePicture（libva 解码到 VAAPI surface）
    → cuvidMapVideoFrame64（surface → HIP 设备指针）
    → NPP kernel 直接读该指针（NV12）
    → NPP 输出（HIP 设备指针）
```

关键依赖：`cuvidMapVideoFrame64` 返回可被 HIP kernel 直接寻址的设备指针。零拷贝细节见[内存与同步模型](内存与同步模型.md) §3。若零拷贝未就绪，先走拷贝（`vaGetImage` → host → `hipMemcpyHtoD`），功能正确但性能有损。

## 14. 验证方法

| 阶段 | 验证 | 硬件 | 当前状态 |
|---|---|---|---|
| NVIDIA 参考 | 26 项 NPP API 直测 | RTX 4060 | ✅ 全部成功，结果见 `docs/evidence/results/npp_results.json` |
| P0 | FFmpeg `scale_npp` 跑通 NV12 缩放/转色 | AMD | ❌ shim 未实现 |
| P1/P2 | 自写 NPP 测试程序，对照 NVIDIA 输出逐函数比对像素 | AMD + NVIDIA | ❌ 未开始 |
| zero-copy | 解码 → NPP 处理链路 | AMD | ❌ 未开始 |

NVIDIA 侧 FFmpeg 后处理操作矩阵结果见 `docs/evidence/results/npp_vpp_nvidia_results.md`；AMD 侧 VPP 结果（`npp_vpp_amd_results.md` / `vpp_results.json`）仍未补齐。

## 15. 风险与待办

1. **法务路线**已选路线 A；落地时确认 shim 产物不含任何 NPP 头文件。
2. **NppStreamContext 字段映射**：`hipGetDeviceProperties` 能否填全所有字段（尤其 MultiProcessorCount/ComputeCapability）需实测。
3. **scratch buffer / GetBufferSize 语义**：逐函数确认是否需要真实临时显存。
4. **NPP 库完整性**：P0 只需 5 个库（3 非空 + 2 占位）；完整兼容需 11 个 `.so` 全部产出（占位库也要导出符号，否则链接失败）。
5. **P0 联合链路**：NPP shim 尚未实现，当前 P0 结果不包含 NPP 路径。

## 16. 附录：关键源码与结果位置

| 内容 | 文件 |
|---|---|
| VPE 能力结构体 | `src/amd/vpelib/inc/vpe_types.h`（`vpe_caps`） |
| VPE 1.0 / 1.1 能力填充 | `src/amd/vpelib/src/chip/vpe10|vpe11/vpe*_resource.c` |
| VPE 驱动后端 | `src/gallium/drivers/radeonsi/si_vpe.c` |
| VAAPI 滤镜查询 | `vlVaQueryVideoProcFilters`（`frontends/va/surface.c`） |
| NPP 直测程序 | `benchmarks/npp/npp_benchmark.cpp` |
| VPP 直测程序 | `benchmarks/vpp/vpp_benchmark.c` |
| NPP/VPP 矩阵脚本 | `benchmarks/scripts/test_npp_vpp.sh` |
| NPP 结果 | `docs/evidence/results/npp_results.json` |
| NVIDIA VPP 结果 | `docs/evidence/results/npp_vpp_nvidia_results.md` |
