# NPP 与 VPE 视频后处理能力对比报告（源码级）

> 文档版本：v1.0
> 生成时间：2026-10-23
> 分析基线：Mesa 25.3.5（`src/amd/vpelib/` + `src/gallium/drivers/radeonsi/si_vpe.c` + `src/gallium/frontends/va/`）；NPP 11.5.107（RTX 4060 实测）

---

## 1. 分析背景与方法

本报告对比两种视频后处理技术栈的能力差异：

| 技术栈 | 定位 | 分析方法 |
|--------|------|---------|
| **NPP**（NVIDIA Performance Primitives） | NVIDIA CUDA 通用图像处理原语库 | 26 项 API 直测（`npp_benchmark`） |
| **VPE**（Video Processor Engine） | AMD 独立视频后处理硬件 IP | Mesa 源码分析（vpelib 库 + radeonsi 驱动 + VAAPI 前端） |

> 说明：VPE 的权威能力清单不在黑盒测试里，而在 `vpe_caps` 结构体（`vpe_types.h`）和 `vpe10/vpe11_resource.c` 的实际填充中。源码分析能拿到「硬件支持什么」的精确答案。
>
> 报告结构：第 2 章先单独梳理 NPP 能力（8 大类 26 项直测），第 3 章单独梳理 VPE 能力（源码证据），第 4 章补充 VPE 与 VAAPI 暴露面的鸿沟，第 5 章才做两者的横向对比。

---

## 2. NPP 能力清单（26 项 API 直测）

NPP（NVIDIA Performance Primitives）是 NVIDIA 的 CUDA 通用图像处理**原语库**，本质是一组在 GPU 上执行的 CUDA kernel 集合，覆盖图像处理、信号处理、颜色转换等领域。它由 12 个子库构成（`nppi`/`npps`/`nppc`/`nppig`/`nppim`/`nppitc`/`nppial`/`nppist`/`nppidei`/`nppisu`/`nppif`/`nppicc`）。

本报告的 NPP 能力来自 `npp_benchmark` 直测（RTX 4060，NPP 11.5.107，1920×1080 @ 50 次迭代），26 项 API **全部成功**，按 8 大类组织：

### 2.1 几何变换（7 项）

| 能力 | NPP API | 实测耗时 |
|------|---------|---------|
| 缩放（灰度，线性插值） | `nppiResizeSqrPixel_8u_C1R` | 0.010 ms |
| 缩放（RGB，立方插值） | `nppiResizeSqrPixel_8u_C3R` | 0.037 ms |
| 水平镜像 | `nppiMirror_8u_C1R`（HORIZONTAL） | 0.006 ms |
| 垂直镜像 | `nppiMirror_8u_C1R`（VERTICAL） | 0.006 ms |
| 旋转 45° | `nppiRotate_8u_C1R` | 0.039 ms |
| 仿射变换 | `nppiWarpAffine_8u_C1R` | 0.011 ms |
| 透视变换 | `nppiWarpPerspective_8u_C1R` | 0.012 ms |

> NPP 的几何变换支持**任意角度**（旋转/仿射/透视），这是其区别于固定角度硬件单元的核心优势。

### 2.2 滤波（5 项）

| 能力 | NPP API | 实测耗时 |
|------|---------|---------|
| 锐化（3x3 核） | `nppiFilterBorder_8u_C1R` | 0.125 ms |
| 中值滤波 3x3 | `nppiFilterMedian_8u_C1R` | 0.182 ms |
| 高斯滤波 3x3 | `nppiFilterGauss_8u_C1R` | 0.012 ms |
| Sobel 水平边缘 | `nppiFilterSobelHoriz_8u_C1R` | 0.011 ms |
| Canny 边缘检测 | `nppiFilterCannyBorder_8u_C1R` | 0.118 ms |

### 2.3 色彩转换（5 项）

| 能力 | NPP API | 实测耗时 |
|------|---------|---------|
| YUV420 → RGB（平面） | `nppiYUV420ToRGB_8u_P3C3R` | 0.026 ms |
| NV12 → RGB（半平面） | `nppiNV12ToRGB_8u_P2C3R` | 0.019 ms |
| NV12 → BGR（半平面） | `nppiNV12ToBGR_8u_P2C3R` | 0.019 ms |
| YUV422 → RGB（4:2:2） | `nppiYUV422ToRGB_8u_P3C3R` | 0.028 ms |
| RGB → YUV420（平面） | `nppiRGBToYUV420_8u_C3P3R` | 0.030 ms |

### 2.4 位深转换（2 项）

| 能力 | NPP API | 实测耗时 |
|------|---------|---------|
| 8u → 16u | `nppiConvert_8u16u_C1R` | 0.029 ms |
| 8u → 32f | `nppiConvert_8u32f_C1R` | 0.027 ms |

### 2.5 算术 / 融合（2 项）

| 能力 | NPP API | 实测耗时 |
|------|---------|---------|
| 帧差（运动检测） | `nppiAbsDiff_8u_C1R` | 0.006 ms |
| 加权叠加（8u→32f） | `nppiAddWeighted_8u32f_C1IR` | 0.020 ms |

### 2.6 形态学（2 项）

| 能力 | NPP API | 实测耗时 |
|------|---------|---------|
| 膨胀 3x3 | `nppiDilate_8u_C1R` | 0.097 ms |
| 腐蚀 3x3 | `nppiErode_8u_C1R` | 0.037 ms |

### 2.7 统计（2 项）

| 能力 | NPP API | 实测耗时 |
|------|---------|---------|
| 均值 / 标准差 | `nppiMean_StdDev_8u_C1R` | 0.170 ms |
| 直方图（256 级） | `nppiHistogramEven_8u_C1R` | 0.014 ms |

### 2.8 阈值（1 项）

| 能力 | NPP API | 实测耗时 |
|------|---------|---------|
| 阈值（>128→255） | `nppiThreshold_Val_8u_C1R` | 0.006 ms |

### 2.9 NPP 的格式盲区（关键发现）

NPP 色彩转换存在**格式盲区**，这是与 VPE 对比的重要差异点：

| 盲区 | 说明 |
|------|------|
| **RGB → NV12 不存在** | 只有 `nppiRGBToYUV420`（平面 YUV420），无 RGB→NV12（半平面）。编码前处理（NV12 输入）需额外格式转换 |
| **AddWeighted 无纯 8u 版本** | 只有 `nppiAddWeighted_8u32f_C1IR`（8u 源 → 32f 目标 in-place），无纯 8u 的快速融合 |

> 这两处盲区意味着：NPP 覆盖了「解码输出（NV12）→ 显示/推理（RGB）」的**正向**链路，但「显示/推理（RGB）→ 编码输入（NV12）」的**反向**链路缺失，需要额外转换步骤。

---

## 3. VPE 硬件能力清单（源码证据）

VPE 是 AMD 的独立视频后处理硬件，从 DRAM 到 DRAM 处理视频，目的是把后处理从图形引擎（GFX）卸载下来省功耗。三层架构：

```
VAAPI 前端 (gallium/frontends/va)      ← 对外暴露面（libva 接口）
        ↓
VPE 驱动后端 (radeonsi/si_vpe.c)       ← VPP 参数 → VPE 参数映射
        ↓
VPELIB 库 (amd/vpelib)                 ← 硬件描述符/命令构造
        ↓
VPE 硬件（固件执行）
```

### 3.1 完整能力清单

能力来自 `vpe_caps` 结构体 + `check_*` 校验函数，分四类：

#### 几何变换

| 能力 | 源码证据 | 状态 |
|------|---------|------|
| 缩放（polyphase 多抽头） | `scaling_info`，最大上采样 64x（`max_upscale_factor=64000`） | ✅ |
| 下采样 | 4:1（VPE 1.0）~ 6:1（VPE 1.1） | ✅ |
| 几何缩放（多 pass） | `vpe_geometric_scaling_feature_skip` | ✅ |
| 水平镜像 | `h_mirror_support=1`，`check_h_mirror_support` 放行 | ✅ **（输出端实现）** |
| 垂直镜像 | `v_mirror_support=0` | ❌ |
| 旋转 90/180/270 | `rotation_support=0` | ❌ |

#### 色彩管理

| 能力 | 源码证据 | 状态 |
|------|---------|------|
| CSC（色彩空间转换） | `pre_csc=1, post_csc=1, ocsc=1` | ✅ |
| 伽马校正 | ROM 曲线：sRGB、BT.2020、Gamma2.2、PQ、HLG | ✅ |
| 色域重映射 | `gamut_remap=1` | ✅ |
| 色调映射（HDR↔SDR） | `vpe_tonemap_params` + `color.c` 白点增益路径 | ✅ |
| 3D LUT | `hw_3dlut=1`，`lut_size=LUT_BUFFER_SIZE` | ✅ |
| 色彩调整（亮度/对比/色相/饱和） | `vpe_color_adjust` 结构 + `color_cs.c` 实现 | ⚠️ **硬件支持，驱动未接** |
| 色度键控（color key） | `color_key=1` | ✅ |
| 亮度键控（luma key） | `luma_key=0` | ❌ |

#### 混合

| 能力 | 源码证据 | 状态 |
|------|---------|------|
| global alpha 混合 | `mpc.global_alpha=1` | ⚠️ **硬件支持，VAAPI 层声明禁用** |

#### 视频专用处理

| 能力 | 源码证据 | 状态 |
|------|---------|------|
| 去隔行 | VPE 硬件无此 IP，VAAPI 前端由 GFX compositor 实现 | ❌（不在 VPE） |
| 降噪 | 无对应模块 | ❌ |
| 锐化 | 无对应模块 | ❌ |

### 3.2 像素格式能力

来自 `plane_caps`（`vpe10/vpe11_resource.c`）：

| 格式 | 输入 | 输出 |
|------|:----:|:----:|
| ARGB8888 / XRGB8888 打包 | ✅ | ✅ |
| FP16（浮点 RGB 16bit） | ❌ | ✅ |
| NV12（4:2:0 8bit） | ✅ | ❌ |
| P010（4:2:0 10bit） | ✅ | ❌ |
| P016（4:2:0 16bit） | ❌ | ❌ |
| AYUV（4:4:4 8bit） | ❌ | ❌ |
| YUY2（4:2:2 8bit） | ❌ | ❌ |

> **关键限制**：VPE 输出只支持 RGB/FP16，**不能输出 YUV**（NV12/P010 输出标记为 0）。这意味着 VPE 做后处理后的结果若要继续走视频编码，需要额外格式转换。

### 3.3 VPE 版本差异

由 `vpe_resource_parse_ip_version`（`resource.c`）按硬件版本号区分：

| VPE 硬件版本 | IP Level | 架构 | 差异 |
|-------------|---------|------|------|
| VPE 6.1.0 / 6.1.3 | 1.0 | 单管线 | 下采样 4:1 |
| VPE 6.1.1 / 6.1.2 | 1.1 | 双管线 + collaboration mode | 下采样 6:1 |

两代功能矩阵基本一致，仅下采样比率不同。

---

## 4. 关键发现：VPE 硬件能力 ≫ VAAPI 暴露面

这是本报告最重要的结论。**VPE 硬件支持的能力，远多于 Mesa VAAPI 前端实际暴露给应用的能力。**

### 4.1 VAAPI 前端（Mesa radeonsi）实际暴露什么

`surface.c` 的 `vlVaQueryVideoProcFilters` 只返回**一种滤镜**：

```c
filters[num++] = VAProcFilterDeinterlacing;   // ← 唯一
```

其余滤镜查询全部拒绝：

```c
case VAProcFilterNoiseReduction:      // 降噪
case VAProcFilterSharpening:          // 锐化
case VAProcFilterColorBalance:        // 色彩平衡
case VAProcFilterSkinToneEnhancement: // 肤色增强
    return VA_STATUS_ERROR_UNIMPLEMENTED;
```

`si_get.c` 的方向/混合能力声明：

```c
case PIPE_VIDEO_CAP_VPP_ORIENTATION_MODES:
    return PIPE_VIDEO_VPP_FLIP_HORIZONTAL;  // 只有水平翻转
case PIPE_VIDEO_CAP_VPP_BLEND_MODES:
    return PIPE_VIDEO_VPP_BLEND_MODE_NONE;  // 无混合
```

### 4.2 能力鸿沟对照表

| 能力 | VPE 硬件 | VAAPI 暴露 | 鸿沟 |
|------|:--------:|:----------:|:----:|
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

> **核心结论**：VPE 硬件最擅长的**色调映射、3D LUT、色域重映射、伽马校正**（HDR 视频处理的关键能力），通过标准 VAAPI 接口**完全用不到**。只有绕过 VAAPI、直接调用 `vpelib`（像 `si_vpe.c` 那样），才能释放这些能力。

---

## 5. NPP vs VPE 对比

### 5.1 定位差异

| 维度 | NPP | VPE |
|------|-----|-----|
| 技术形态 | CUDA 通用图像处理**原语库** | 独立视频后处理**硬件 IP** |
| 编程模型 | 逐 API 调用，GPU kernel | 参数描述 + 固件命令流 |
| 面向场景 | AI 推理前处理 + 通用图像 | 视频播放/转码色彩与缩放管线 |
| 数据流 | 任意 CUDA buffer | DRAM→DRAM，固定管线 |

### 5.2 完整逐项对照（26 项全映射）

将第 2 章的 26 项 NPP API 逐项映射到 VPE 能力，归属三类：🔵 重叠（两者都有）、🟢 NPP 独有（VPE 无）、🟠 VPE 近似（语义接近但不等价）。

| # | NPP 能力 | NPP API | VPE 对应能力 | 归属 |
|---|---------|---------|-------------|------|
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
| 13 | YUV420→RGB | `nppiYUV420ToRGB_8u_P3C3R` | CSC（色彩空间转换） | 🔵 重叠 |
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

### 5.3 归纳：重叠 / 独有 / 近似

#### 5.3.1 重叠能力（6 项，NPP API → VPE 能力）

| 能力 | NPP | VPE |
|------|-----|-----|
| 缩放 | 多插值算法（线性/立方） | polyphase 多抽头 |
| 水平镜像 | `nppiMirror` | 输出端 CDC backend |
| 色彩转换（YUV420/NV12→RGB/BGR） | `nppi*ToRGB/BGR` | CSC（硬件色彩空间转换） |

> 注意：NPP 的色彩转换比 VPE 的 CSC **覆盖更宽**——VPE 的 CSC 受限于输入格式（只吃 NV12/P010/ARGB），而 NPP 额外支持 YUV422→RGB；且 NPP 能做 RGB→YUV420 反向转换，VPE 输出根本不能回 YUV。

#### 5.3.2 NPP 独有（16 项，VPE 硬件完全无此能力）

| 类别 | NPP API | VPE 缺失原因 |
|------|---------|-------------|
| 垂直镜像 | `nppiMirror_8u_C1R`（VERTICAL） | `v_mirror_support=0` |
| 任意角度旋转 | `nppiRotate_8u_C1R` | `rotation_support=0` |
| 仿射/透视变换 | `nppiWarpAffine` / `nppiWarpPerspective` | 无对应硬件单元 |
| 滤波（锐化/中值/高斯） | `nppiFilterBorder` / `nppiFilterMedian` / `nppiFilterGauss` | 无滤波模块 |
| 边缘检测（Sobel/Canny） | `nppiFilterSobelHoriz` / `nppiFilterCannyBorder` | 无 |
| 形态学（膨胀/腐蚀） | `nppiDilate` / `nppiErode` | 无 |
| 帧差（运动检测） | `nppiAbsDiff` | 无 |
| 统计（均值/标准差/直方图） | `nppiMean_StdDev` / `nppiHistogramEven` | 无 |
| YUV422→RGB | `nppiYUV422ToRGB` | 输入格式不支持 4:2:2 |
| RGB→YUV420 | `nppiRGBToYUV420` | 输出格式不支持 YUV |

#### 5.3.3 VPE 近似（4 项，语义接近但不等价）

| NPP 能力 | VPE 近似能力 | 差异 |
|---------|-------------|------|
| 位深 8u→16u | P010 10bit 输入 | VPE 无显式位深转换 API，仅隐含支持部分位深 |
| 位深 8u→32f | FP16 输出 | 32f（32bit 浮点）≠ FP16（16bit 浮点），精度不同 |
| 加权叠加 | global alpha 混合 | 语义接近，但 VPE 此能力硬件支持、VAAPI 层已禁用 |
| 阈值 | color key / luma key | 都是「范围判据」，但 NPP 是像素值阈值，VPE 是色度/亮度键控 |

### 5.4 VPE 独有（NPP 无）

| 能力 | VPE 实现 |
|------|---------|
| 色调映射 HDR↔SDR | 硬件 3D LUT + 白点增益路径 |
| 伽马校正（PQ/HLG/BT2020） | 硬件 ROM 曲线 |
| 色域重映射 | gamut remap 单元 |
| 色度键控（color key） | dpp color key |

> 对比小结：**NPP 是「通用图像处理工具箱」，VPE 是「视频色彩专用管线」**。26 项 NPP API 中，与 VPE 真正重叠的仅 6 项（缩放/水平镜像/基础色彩转换），16 项是 NPP 独有（滤波/形态学/几何变换/统计/反向转换），4 项 VPE 仅近似支持。反过来，VPE 独有 HDR 色彩管理（色调映射/伽马/色域/键控）是 NPP 完全没有的。

---

## 6. 对自研 SDK 的启示

结合本公司目标（自研硬件解码 SDK 兼容 NVDEC，初步通过 VAAPI 实现后端驱动）：

1. **走 VAAPI 后端会丢失 VPE 色彩管理强项**。VPE 的色调映射/3D LUT/色域重映射/伽马校正——这些正是视频后处理区别于通用图像处理的核心能力——通过标准 VAAPI 接口无法调用。若 SDK 的定位是「兼容 NVDEC 解码功能」而非「完整视频后处理」，这个丢失可接受；若要覆盖后处理，需评估直接调 `vpelib` 的可行性。

2. **NVDEC 解码输出的 NV12/P010，恰好是 VPE 的输入格式**（`nv12/p010` 输入 ✅）。这为「NVDEC 兼容解码 + VPE 后处理」的链路提供了天然衔接。

3. **VPE 输出不能回 YUV**（只出 RGB/FP16），若后处理结果要继续编码，需要格式转换环节，这会引入额外开销。

4. **VPE 不支持旋转/垂直翻转/去隔行（硬件层）**，与 NVDEC（纯解码，无后处理）不同，也与 NPP（旋转/镜像齐全）不同。设计兼容层时需明确：这些能力要么软件实现，要么依赖 GFX 引擎。

---

## 7. 附录：关键源码位置

| 内容 | 文件 |
|------|------|
| VPE 能力结构体定义 | `src/amd/vpelib/inc/vpe_types.h`（`vpe_caps`） |
| VPE 1.0 能力填充 | `src/amd/vpelib/src/chip/vpe10/vpe10_resource.c` |
| VPE 1.1 能力填充 | `src/amd/vpelib/src/chip/vpe11/vpe11_resource.c` |
| 镜像/旋转校验 | `vpe10_check_mirror_rotation_support`（`vpe10_resource.c:1253`） |
| 水平镜像支持 | `vpe10_check_h_mirror_support`（`vpe10_resource.c:535`） |
| VPE 驱动后端（VPP→VPE 映射） | `src/gallium/drivers/radeonsi/si_vpe.c` |
| VAAPI 滤镜查询 | `vlVaQueryVideoProcFilters`（`frontends/va/surface.c:1327`） |
| VAAPI 滤镜处理 | `frontends/va/postproc.c` |
| VPE 能力声明 | `src/gallium/drivers/radeonsi/si_get.c`（VPP 分支） |
| 色彩调整实现 | `src/amd/vpelib/src/core/color_cs.c` |
| 白点增益（HDR） | `vpe_color_update_whitepoint`（`core/color.c:1020`） |

---

## 8. 附录：测试脚本与结果文件

### 8.1 脚本清单

| 脚本 | 说明 |
|------|------|
| [npp_benchmark.cpp](../../benchmarks/npp/npp_benchmark.cpp) | NPP 直测程序源码（26 项 API，CUDA event 计时） |
| [build_npp_benchmark.sh](../../benchmarks/npp/build_npp_benchmark.sh) | NPP 直测程序编译脚本 |
| [vpp_benchmark.c](../../benchmarks/vpp/vpp_benchmark.c) | VAAPI VPP 直测程序源码（libva 直接调用） |
| [build_vpp_benchmark.sh](../../benchmarks/vpp/build_vpp_benchmark.sh) | VPP 直测程序编译脚本 |
| [test_npp_vpp.sh](../../benchmarks/scripts/test_npp_vpp.sh) | NPP/VPP 后处理操作矩阵测试（`--nvidia` / `--amd` 参数） |

### 8.2 结果文件

| 文件 | 说明 |
|------|------|
| [npp_results.json](../evidence/results/npp_results.json) | NPP 26 项直测结果（RTX 4060，全部成功） |
| [npp_vpp_nvidia_results.md](../evidence/results/npp_vpp_nvidia_results.md) | NVIDIA 侧 FFmpeg 层后处理操作矩阵结果 |

> 注：AMD 侧 VPP 结果（`npp_vpp_amd_results.md` / `vpp_results.json`）待 AMD 机器跑完 `test_npp_vpp.sh --amd` 与 `vpp_benchmark` 后补齐。
