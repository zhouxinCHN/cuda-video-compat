# CUVID / NVENC ↔ VAAPI 兼容映射表（自研 NVDEC 兼容 SDK）

> 版本：v0.1（初稿，供工程师开工）
> 生成时间：2026-10-28
> 基线：nv-codec-headers 12.1（dynlink 版）+ libva 1.14.0 + Mesa 26（radeonsi / VCN5）
> 目标：源码级重编译后能跑（不要求严格二进制兼容）
> 上位文档：[docs/architecture/cuda-compat-overview.md](cuda-compat-overview.md)（HIPIFY + shim 双轨架构，本文属"轨 2：shim 库"）

---

## 1. 产物与 SONAME（先定命名）

FFmpeg 的 `dynlink_loader.h` 按**固定名字** dlopen，你的 shim 产物必须满足：

| 你要提供的 .so | 覆盖的接口 | 内部依赖 |
|---|---|---|
| `libcuda.so.1` | CUDA driver API（`cuCtxGetCurrent`/`cuMemAlloc`/`cuMemcpyDtoH`…） | HIP 运行时（`libamdhip64`） |
| `libnvcuvid.so.1` | CUVID 解码 + parser（两层，见 §3） | `libcuda.so.1` + `libva` |
| `libnvidia-encode.so.1` | NVENC 编码（`nvEncodeAPI.h` 函数表） | `libcuda.so.1` + `libva` |

> 符号按名 `dlsym` 查找，无版本脚本要求；只需导出同名符号即可。`libcuda.so.1` 是另外两个的前置依赖，**必须三件套一起出**。

---

## 2. 底层解码器（cuviddec.h）→ libva 函数映射

### 2.1 函数映射总表

| CUVID 函数（`tcuvid*`） | 映射到 libva | 备注 |
|---|---|---|
| `cuvidGetDecoderCaps(CUVIDDECODECAPS*)` | `vaInitialize` + `vaQueryConfigProfiles` + `vaQueryConfigEntrypoints` + 静态 caps 表（§5） | 按 codec×chroma×bitdepth 查表回填 |
| `cuvidCreateDecoder(CUvideodecoder*, CUVIDDECODECREATEINFO*)` | `vaCreateConfig` + `vaCreateContext` + `vaCreateSurfaces` | 结构体字段映射见 §4.1 |
| `cuvidDestroyDecoder` | `vaDestroySurfaces` + `vaDestroyContext` + `vaDestroyConfig` | |
| `cuvidDecodePicture(h, CUVIDPICPARAMS*)` | `vaBeginPicture` + `vaRenderPicture`(SPS/PPS/slice) + `vaEndPicture` | 见 §4.2 |
| `cuvidGetDecodeStatus(h, nPicIdx, CUVIDGETDECODESTATUS*)` | shim 内部 surface 状态表 + `vaSyncSurface` | VAAPI 无逐帧查询，shim 自维护 |
| `cuvidReconfigureDecoder` | 销毁重建 context（VAAPI 无动态 reconfigure） | 仅限分辨率/目标尺寸变化 |
| `cuvidMapVideoFrame64(h, idx, *dptr, *pitch, CUVIDPROCPARAMS*)` | `vaSyncSurface` + `vaDeriveImage`/`vaGetImage`；若带 target 尺寸则接 VPP | 见 §4.3，后处理语义坑 |
| `cuvidUnmapVideoFrame64` | `vaReleaseBuffer` / 标记 surface 可复用 | |
| `cuvidCtxLockCreate/Destroy/Lock/Unlock` | mutex 包装，对接 HIP context lock | 与视频无关，纯 host 临界区 |

> `cuvidMapVideoFrame`/`UnmapVideoFrame`（32 位指针版）仅在 `!__CUVID_DEVPTR64` 时定义；64 位平台一律走 `MapVideoFrame64`，shim 只需实现 64 位版（宏已自动重定向）。

### 2.2 CUresult 错误码

返回类型统一 `CUresult`（`CUDA_SUCCESS=0`）。shim 需定义完整 `CUDA_ERROR_*` 枚举，与 libva 的 `VAStatus` 做双向映射：
- 不支持 → `CUDA_ERROR_NOT_SUPPORTED (201)`
- 参数非法 → `CUDA_ERROR_INVALID_VALUE (1)`
- 内存不足 → `CUDA_ERROR_OUT_OF_MEMORY (2)`

---

## 3. 上层 parser / source（nvcuvid.h）—— 独立模块，工作量最大

### 3.1 接口与回调

| API / 回调 | 说明 |
|---|---|
| `cuvidCreateVideoParser(CUvideoparser*, CUVIDPARSERPARAMS*)` | 创建 parser 状态机，注册 5 个回调 |
| `cuvidParseVideoData(obj, CUVIDSOURCEDATAPACKET*)` | 喂裸码流，同步触发回调 |
| `cuvidDestroyVideoParser` | 销毁 |
| `cuvidCreateVideoSource(W)/DestroyVideoSource` | demuxer 层，可选（多数应用自己 demux，仅用 parser） |
| `cuvidSet/GetVideoSourceState`、`cuvidGetSourceVideoFormat/AudioFormat` | source 层，可选 |
| `PFNVIDSEQUENCECALLBACK` | 码流头变化时回调，应用在此调 `cuvidCreateDecoder` |
| `PFNVIDDECODECALLBACK` | 每帧就绪回调，应用在此调 `cuvidDecodePicture` |
| `PFNVIDDISPLAYCALLBACK` | 显示顺序回调 |
| `PFNVIDOPPOINTCALLBACK` | AV1 可伸缩码流 operating point 选择 |
| `PFNVIDSEIMSGCALLBACK` | SEI 消息 |

### 3.2 核心工作：bitstream parser

`cuvidParseVideoData` 内部必须把 Annex-B 裸码流 parse 成各 codec 的 `CUVID*PICPARAMS` 全量结构体（§4.2）。**复用 FFmpeg 的 parser**（`libavcodec/h264_parser.c` / `hevc_parser.c` / `vp9_parser.c` / `av1_parser.c`）是可行路径：FFmpeg 的 parser 输出 SPS/PPS/slice 数据，你再填充 CUVID 结构体字段。

> 这是与底层 `cuviddec` 解耦的独立模块，也是整个 nvdec 兼容里**工作量占比最大**的部分。建议单独立项评估。

---

## 4. 结构体字段映射要点

### 4.1 `CUVIDDECODECREATEINFO` → VAAPI

| CUVID 字段 | 映射 |
|---|---|
| `CodecType` + `ChromaFormat` + `bitDepthMinus8` | 三者共同决定 `VAProfile`（§6.1 查表） |
| `ulWidth`/`ulHeight` | surface 尺寸 |
| `ulNumDecodeSurfaces` | surface 数量（`vaCreateSurfaces` 的 `num_surfaces`，建议 = min_num_decode_surfaces + 3） |
| `OutputFormat` | `VA_RT_FORMAT`（§6.2 查表） |
| `ulCreationFlags` | 仅 `cudaVideoCreate_PreferCUVID` 支持；`PreferCUDA`/`PreferDXVA` 返回不支持 |
| `DeinterlaceMode` | Weave→直通；Bob/Adaptive→`VAProcFilterDeinterlacing`（VPP 层，GFX 实现） |
| `display_area` / `target_rect` | VPP 的 crop / scale（若应用设了非零值） |
| `ulTargetWidth/Height` | VPP scale 目标尺寸（偶数对齐） |
| `vidLock` | 空/可选，非空则转 HIP context lock |

### 4.2 `CUVIDPICPARAMS` → `VAPictureParameterBuffer*` + `VASliceParameterBuffer*`

**方向关键**：CUVID 结构体是"全量头信息"，VAAPI 只需 SPS/PPS + slice offset。所以是"parse 全量 → 填 CUVID（回调用）→ 同时抽 VAAPI 需要的子集喂 VLD"。

| codec | CUVID 载体 | VAAPI picture buffer | VAAPI slice buffer |
|---|---|---|---|
| H264 | `CUVIDH264PICPARAMS`（60+ 字段） | `VAPictureParameterBufferH264` | `VASliceParameterBufferH264` |
| HEVC | `CUVIDHEVCPICPARAMS`（100+ 字段） | `VAPictureParameterBufferHEVC` | `VASliceParameterBufferHEVC` |
| VP9 | `CUVIDVP9PICPARAMS` | `VAPictureParameterBufferVP9` | `VASliceParameterBufferVP9` |
| AV1 | `CUVIDAV1PICPARAMS`（200+ 字段） | `VAPictureParameterBufferAV1` | `VASliceParameterBufferAV1` |
| MPEG2 | `CUVIDMPEG2PICPARAMS` | `VAPictureParameterBufferMPEG2` | `VASliceParameterBufferMPEG2` |
| VC1 | `CUVIDVC1PICPARAMS` | `VAPictureParameterBufferVC1` | `VASliceParameterBufferVC1` |
| JPEG | `CUVIDJPEGPICPARAMS` | `VAPictureParameterBufferJPEGBaseline` | `VASliceParameterBufferJPEGBaseline` |

> AV1 的 film grain（`apply_grain` + 14 组 scaling points）VAAPI 侧在 `VAPictureParameterBufferAV1` 的 film_grain 子结构里，需逐字段对齐，是 AV1 映射里最容易错的一块。

### 4.3 `cuvidMapVideoFrame64` 后处理语义（关键坑）

`CUVIDPROCPARAMS` 字段按需实现：
- 仅解码输出 → `vaSyncSurface` + `vaDeriveImage`（零拷贝路径则 surface import，见报告坑 1）
- `progressive_frame`/`top_field_first`/`second_field` → VPP deinterlace 参数
- `raw_input_dptr`/`raw_output_dptr` → raw YUV 扩展，先不支持（返回错误）
- `histogram_dptr` → 先回 `NULL`（`bIsHistogramSupported=0`）
- `output_stream` → 对接 HIP stream（异步同步点）

---

## 5. caps 回填表（`cuvidGetDecoderCaps`）

`bIsSupported` 按 codec×chroma×bitdepth 查下表（数据来自你两份报告的实测矩阵，VCN5 侧为 FFmpeg 层结论）：

| codec | chroma | bitdepth | bIsSupported |
|---|---|---|:---:|
| H264 | 420 | 8 | 1 |
| H264 | 422 / 444 | 8 | 1（VCN 领先 NVDEC） |
| H264 | 420 | 10 | 1（VCN 领先） |
| HEVC | 420 | 8 / 10 / 12 | 1 |
| HEVC | 422 / 444 | 8 | 1 |
| AV1 | 420 | 8 / 10 | 1 |
| VP9 | 420 | 8 / 10 | 1 |
| MPEG2 | 420 / 422 | 8 | 1 |

回填值（**标注待实测**，先以 RTX 4060 参考值占位）：
- `nMaxWidth=4096, nMaxHeight=4096, nMaxMBCount=65536, nMinWidth=48, nMinHeight=16`
- `nOutputFormatMask`：按支持位深置 NV12/P016/YUV444/YUV444_16Bit 位
- `bIsHistogramSupported=0`

---

## 6. 枚举映射速查

### 6.1 `cudaVideoCodec` → `VAProfile`

| cudaVideoCodec | VAProfile（按 chroma/bitdepth 再细分） |
|---|---|
| MPEG1 | VAProfileMPEG2Simple（VA 无独立 MPEG1） |
| MPEG2 | VAProfileMPEG2Simple / Main |
| MPEG4 | VAProfileMPEG4Simple / AdvancedSimple / Main |
| VC1 | VAProfileVC1Simple / Main / Advanced |
| H264 | VAProfileH264Main / High / ConstrainedBaseline |
| H264_SVC / H264_MVC | VAProfileH264MultiviewHigh / StereoHigh（或回不支持） |
| HEVC | VAProfileHEVCMain / Main10 / Main12 / 422_10 / 422_12 / 444 / 444_10 / 444_12 |
| VP8 | VAProfileVP8Version0_3 |
| VP9 | VAProfileVP9Profile0（8bit）/ Profile2（10bit） |
| AV1 | VAProfileAV1Profile0（8bit）/ Profile1（10bit） |
| JPEG | VAProfileJPEGBaseline |

### 6.2 `cudaVideoSurfaceFormat` → `VA_RT_FORMAT`

| cudaVideoSurfaceFormat | VA_RT_FORMAT |
|---|---|
| NV12 | YUV420 |
| P016 | YUV420_10（10bit）/ YUV420_12（12bit） |
| YUV444 | YUV444 |
| YUV444_16Bit | YUV444_10 / YUV444_12 |

### 6.3 `cudaVideoChromaFormat` → `VA_RT_FORMAT` 基类

| chroma | 8bit | 10bit | 12bit |
|---|---|---|---|
| Monochrome | YUV400 | — | — |
| 420 | YUV420 | YUV420_10 | YUV420_12 |
| 422 | YUV422 | YUV422_10 | YUV422_12 |
| 444 | YUV444 | YUV444_10 | YUV444_12 |

---

## 7. NVENC 映射（libnvidia-encode.so.1）

入口：`NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST*, uint32_t version)` 返回 34 项函数指针表。

| 函数表项 | 映射 | 优先级 |
|---|---|---|
| `nvEncOpenEncodeSession(Ex)` | `vaCreateConfig(VAEntrypointEncSlice)` + `vaCreateContext` | P0 |
| `nvEncGetEncodeGUIDs / GetEncodeProfileGUIDs` | `vaQueryConfigProfiles` → GUID 表回填 | P0 |
| `nvEncGetEncodePresetConfig(Ex)` | preset GUID → `VAEncMiscParameterRateControl`/QualityLevel | P0 |
| `nvEncInitializeEncoder` | `vaCreateSurfaces` + `VAEncSequenceParameterBuffer*` + `VAEncPictureParameterBuffer*` | P0 |
| `nvEncCreateInputBuffer / MapInputResource` | `NV_ENC_INPUT_PTR`（CUDA ptr）→ VASurfaceID | P0 |
| `nvEncEncodePicture` | `vaBeginPicture` + `vaRenderPicture` + `vaEndPicture` | P0 |
| `nvEncLockBitstream / UnlockBitstream` | `VACodedBufferSegment` + `vaMapBuffer` | P0 |
| `nvEncGetEncodeCaps` | 回填 `NV_ENC_CAPS`（VCN 真实能力） | P1 |
| 其余（Stats/SequenceParams/MEOnly/AsyncEvent/MVBuffer/Lookahead/RestoreState） | `NV_ENC_ERR_UNIMPLEMENTED` | P2 |

**关键差异**：
- GUID 是二进制字节串，应用 `memcmp` 匹配，必须从 `nvEncodeAPI.h` 逐字节抄（`NV_ENC_CODEC_H264_GUID`/`NV_ENC_PRESET_P4_GUID` 等）。
- `nvEncMapInputResource` 是 zero-copy 高效路径，对应 VAAPI 需 surface import（`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME`）。
- `NVENCSTATUS` 枚举全量定义，未实现项返回 `NV_ENC_ERR_UNIMPLEMENTED`。

---

## 8. 与已有报告的衔接

- 解码格式矩阵、caps 回填依据：`docs/architecture/nvdec-vaapi-comparison.md` §4。
- 后处理（VPP/去隔行/histogram）能力边界：`docs/architecture/npp-vpe-comparison.md` §3-4（VPE 无去隔行、无 histogram，去隔行靠 GFX）。
- zero-copy surface 路径（坑 1）在本表 §4.3，与方案正文 M3 阶段对齐。

## 9. 待办状态（2026-10-28 更新）

1. [ ] VCN5 真实 caps（max width/height/MBCount/min 尺寸）实测，替换 §5 的 4060 占位值。
   **→ 本机是 RTX 4060（NVIDIA），无 AMD GPU，需 RX 9070 机器跑。** 可先用 NVIDIA 机写 `vaQueryConfigAttributes` 探测脚本模板，拿到 AMD 机直接跑。
2. [x] FFmpeg `h264_cuvid` 是否用缩放 → **默认不缩放**（结论见 §10.3）。
3. [x] parser 选型 → **绕不开，必须内嵌**（结论见 §10.4，修正 §3.2 的形态）。
4. [x] `libcuda.so.1` 范围 → **约 82 个必需符号，硬约束**（结论见 §10.2，修正 §1 的"最小子集"）。

---

## 11. 附录 C：libcuda.so.1 shim 实现状态（2026-10-29）

### 11.1 已完成的验证

| 项目 | 状态 | 说明 |
|---|---|---|
| 项目骨架 | ✅ | `cuda_shim/`（include/src/test + CMakeLists.txt） |
| `libcuda.so.1` 编译 | ✅ | g++ 直编（本机无 cmake），仅 hipCtx* 系列 deprecation warning |
| 82 个必需符号导出 | ✅ | `verify_symbols` 全通过（模拟 FFmpeg `cuda_load_functions` 的 dlsym 清单） |
| HIP 运行时链接 | ✅ | `libamdhip64.so.7` 已正确依赖，无 undefined symbol |
| 功能冒烟测试 | ✅ | `test_func`：cuInit→100(NO_DEVICE，本机 NVIDIA 无 AMD GPU)、cuDriverGetVersion→0(ver 70226015)、cuDeviceGetCount→100(count=0)，**错误码映射与 HIP 调用链路正确** |

### 11.2 关键技术决策（已定）

1. **头文件策略**：`<ffnvcodec/dynlink_cuda.h>`（CUDA 类型，MIT）与 `<hip/hip_runtime_api.h>` 可在同一编译单元共存，需 `-D__HIP_PLATFORM_AMD__`。已实测通过。
2. **类型映射**：`CUdevice==hipDevice_t(int)` 直接相等；`CUcontext↔hipCtx_t` 用 reinterpret_cast；`CUdeviceptr(unsigned long long)↔hipDeviceptr_t(void*)` 用 uintptr_t 中转。
3. **错误码**：显式 switch 映射，不用数值巧合。但实测发现 HIP 有意对齐 CUDA 数值（hipErrorNotSupported=801 等），属额外保险。
4. **链接**：`-lamdhip64` 必须放在源文件后且用全路径（默认 `--as-needed` 会丢弃）。

### 11.3 编译命令（无 cmake 环境）

```bash
g++ -std=c++17 -shared -fPIC -D__HIP_PLATFORM_AMD__ \
  -I cuda_shim/include -I $HOME/nv-codec-headers/include -I /opt/rocm/include \
  cuda_shim/src/libcuda.cpp -o libcuda.so.1 \
  /opt/rocm/lib/libamdhip64.so
```

### 11.4 待办（下一步）

1. [ ] AMD 机器上复测功能冒烟（应有 GPU，cuDeviceGetCount>0、cuMemAlloc 成功）。
2. [ ] 开始 `libnvcuvid.so.1` 底层 12 个符号（cuvidCreateDecoder→vaCreateConfig 等）。
3. [ ] 独立开发 bitstream parser 模块（H264 优先，复用 FFmpeg parser）。
4. [ ] 可选：装 cmake 让 CMakeLists.txt 生效（或改用 Makefile）。

### 10.1 加载机制结论

- FFmpeg **二进制不链接** libcuda（`ldd` 无、`nm -D` 无 `cuInit` 符号）。
- 全部通过 `compat/cuda/dynlink_loader.h` → `#include <ffnvcodec/dynlink_loader.h>`（即 `$HOME/nv-codec-headers/include/ffnvcodec/`）→ `dlopen + dlsym`。
- LIBNAME 硬编码：`libcuda.so.1` / `libnvcuvid.so.1` / `libnvidia-encode.so.1`（Linux）。
- 加载时机：解码器 `init` 时（`cuvid_load_functions`/`cuda_load_functions`/`nvenc_load_functions`），**一次性 dlsym 全部符号**。

**推论**：提供同名 .so + 导出必需符号，即可让 FFmpeg 加载，**无需改 FFmpeg 源码、无需重编译 FFmpeg**。

### 10.2 `libcuda.so.1` 符号面硬约束（修正 §1）

`cuda_load_functions` 用 `LOAD_SYMBOL`（**必需**，缺失即 `ret=-1` 失败）加载 **82 个符号**，用 `LOAD_SYMBOL_OPT`（可选）加载约 20 个。**即使你的场景只用 nvdec/nvenc，这 82 个必需符号也必须全部导出**（哪怕 stub 返回 `CUDA_ERROR_NOT_SUPPORTED`）。

82 个必需符号分组：

| 组 | 符号 | 你的实现策略 |
|---|---|---|
| 设备/上下文（22） | cuInit/cuDriverGetVersion/cuDeviceGetCount/cuDeviceGet/cuDeviceGetAttribute/cuDeviceGetName/cuDeviceComputeCapability/cuCtxCreate/cuCtxSynchronize/cuCtxGetStreamPriorityRange/cuCtxGetCurrent/cuCtxSetLimit/cuCtxPushCurrent/cuCtxPopCurrent/cuCtxDestroy/cuCtxGetDevice + PrimaryCtx 5 个 | **HIP 直接映射**（hipInit/hipCtxCreate/…） |
| 内存（28） | cuMemAlloc/cuMemAllocPitch/cuMemAllocManaged/cuMemFree/cuMemHostAlloc/cuMemFreeHost/cuMemAllocAsync/cuMemFreeAsync + Memset 6 + Memcpy 10 | **HIP 直接映射** |
| 流/事件（12） | cuStreamCreate/…/cuEventRecord | **HIP 直接映射** |
| **launch/模块/JIT（10）** | cuLaunchKernel/cuLaunchHostFunc/cuLink*/cuModuleLoadData/cuModuleUnload/cuModuleGetFunction/cuModuleGetGlobal | **stub**（视频路径不走），但 NPP kernel 库自己要用，届时接 HIP |
| **texture/array（6）** | cuTexObjectCreate/Destroy/cuArrayCreate/cuArray3DCreate/cuArrayDestroy/cuArray3DGetDescriptor | **stub** 返回 NOT_SUPPORTED |
| **GL interop（7）** | cuGLGetDevices/cuGraphicsGLRegisterImage/cuGraphicsUnregisterResource/cuGraphicsMapResources/cuGraphicsUnmapResources/cuGraphicsSubResourceGetMappedArray/cuGraphicsResourceGetMappedPointer | **stub**（或后续接 HIP-GL interop） |

> 注意：`LOAD_SYMBOL` 里 `cuGraphicsResourceGetMappedPointer` 的 dlsym 字符串是 `"cuGraphicsResourceGetMappedPointer_v2"`（header 有拼写差异，按字符串导出即可）。

### 10.3 缩放路径结论（待办 #2）

| 解码器 | 文件 | ulTargetWidth 语义 |
|---|---|---|
| `h264_cuvid` 等 | `cuviddec.c` | `(avctx->width + 1) & ~1`，默认 = **crop 后尺寸对齐偶数，非缩放**。仅当用户传 `-resize WxH`（`resize_expr`）时才真缩放 |
| `h264_nvdec` 等（hwaccel） | `nvdec.c` | `ulTargetWidth = coded_width`，**完全不缩放** |

**结论**：Map 阶段**默认不需要接 VPP 缩放**。crop 通过 `display_area` 在 `cuvidCreateDecoder` 时配置；真缩放只在显式 `-resize` 时发生，第一阶段可先不支持（返回 `CUDA_ERROR_NOT_SUPPORTED`）。

### 10.4 parser 选型结论（待办 #3，修正 §3.2）

**核心接口差异**：CUVID 的 NVDEC 硬件"吃完整码流自解析"——FFmpeg `nvdec.c` 调 `cuvidDecodePicture` 时 `CUVIDPICPARAMS` **只填 4 个 bitstream 字段**（`nBitstreamDataLen`/`pBitstreamData`/`nNumSlices`/`pSliceDataOffsets`），`CodecSpecific`（`CUVIDH264PICPARAMS` 等）**留空**，SPS/PPS/slice header 由硬件自己解析。

而 VAAPI 的 VLD **必须由 host 预解析**成 `VAPictureParameterBuffer*` + `VASliceParameterBuffer*` + slice data。

**因此**：shim 内部必须有 H264/HEVC/VP9/AV1 的 bitstream parser，把"完整 slice 码流"转成 VAAPI 参数结构体。**这个 parser 绕不开**（无论走 cuviddec parser 层还是 nvdec 底层）。

复用方案：FFmpeg 的 `h264_parser`/`hevc_parser`/`vp9_parser`/`av1_parser`（LGPL，可静态链接/借鉴），其输出正是 SPS/PPS/slice 数据，填充 `VAPictureParameterBuffer*` 即可。这是独立模块，工作量最大，建议单独立项。

### 10.5 三个 shim 的符号面汇总

| .so | 必需符号数 | 备注 |
|---|---|---|
| `libcuda.so.1` | 82（+20 可选） | 全部导出，一半映射 HIP、一半 stub |
| `libnvcuvid.so.1` | 22 | `cuvidGetDecoderCaps` 可选，其余全必需（含 source 层 7 个 + parser 3 个） |
| `libnvidia-encode.so.1` | 2 | 仅 `NvEncodeAPICreateInstance` + `NvEncodeAPIGetMaxSupportedVersion`，最简 |

### 10.6 parser 复用 FFmpeg 的落地路径（关键细化）

**parser 的两个职责**（缺一不可）：
1. **帧切分**：把 Annex-B 裸码流切成一个个 frame（识别 start code / NAL 边界）。
2. **SPS/PPS/slice header 解析 → VAAPI 结构体**：`VAPictureParameterBufferH264` 等。

**坑：`av_parser_parse2`（FFmpeg 公开 API）只做职责 1，不暴露 SPS/PPS 结构。** 它输出的是 `AVPacket`（切好的帧），SPS/PPS 解析在 FFmpeg 的 **decoder 内部**（`h264_ps.c` 的 `ff_h264_decode_seq_parameter_set` 等），而 VAAPI 参数填充在 **`vaapi_h264.c`**（`fill_vaapi_pic`/`fill_vaapi_ReferenceFrames` 等）。

**结论**：parser 复用不能只用公开 API，必须复用 FFmpeg 的**解析 + 填充逻辑**。但**两个环节的复用方式不同**（这是关键修正）：

| 环节 | FFmpeg 对应代码 | 链接属性 | 复用方式 |
|---|---|---|---|
| **SPS/PPS 解析** | `h264_ps.c` 的 `ff_h264_decode_seq_parameter_set` / `ff_h264_decode_picture_parameter_set` | **非 static（ff_ 内部公共函数）** | ✅ 静态链接 `libavcodec.a` 直接调用，或提取源码 |
| **VAAPI 参数填充** | `vaapi_h264.c` 的 `fill_vaapi_pic`/`fill_vaapi_ReferenceFrames` 等 | **全是 static，且依赖 `H264Context`（整个 decoder 上下文）** | ❌ 不能直接链接/提取，**移植其字段映射逻辑自写** |

**修正说明**：早期版本建议"直接提取 `vaapi_h264.c`"**不可行**——它的填充函数是 static（不导出），且 `#include "h264dec.h"` 拖入整个 decoder 上下文。正确做法是**解析用 FFmpeg（链接/提取），填充参考 vaapi_h264.c 的字段映射自写**。

### 推荐落地路径（已确认采用，shim 开源）

**静态链接 `libavcodec.a` + 移植 VAAPI 填充逻辑**：

```
Annex-B 裸码流
  → ff_h2645_packet_split()        # h2645_parse.c，切 NAL unit
  → GetBitContext 包住 SPS/PPS NAL
  → ff_h264_decode_seq_parameter_set()   # h264_ps.c，解析 SPS → H264ParamSets
  → ff_h264_decode_picture_parameter_set() # 解析 PPS
  → 自写 SPS/PPS → VAPictureParameterBufferH264 填充
     （参考 vaapi_h264.c 的字段映射，静态函数逻辑移植进 shim）
  → VASliceParameterBufferH264 + slice data
```

**优点**：
- 解析用 FFmpeg 现成的、经过验证的代码（静态链接，ff_ 符号在 .a 里可用）。
- 填充逻辑参考 vaapi_h264.c 的字段映射（机械对应，工作量小），不拖入 H264Context 依赖。
- shim 开源满足 LGPL（静态链接也要求开源/提供目标文件，已确认可接受）。

**依赖**：`libavcodec.a`（需 FFmpeg 编译时生成静态库）+ 头文件 `h264_ps.h`（SPS/PPS/H264ParamSets）、`h2645_parse.h`、`get_bits.h`、`golomb.h`。

**HEVC/VP9/AV1 同理**：
- 解析：`hevc_ps.c`（`ff_hevc_decode_nal_sps` 等）、`vp9.c`、`av1_parse.c`。
- 填充：参考 `vaapi_hevc.c`/`vaapi_vp9.c`/`vaapi_av1.c` 的字段映射自写。
