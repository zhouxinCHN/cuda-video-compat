# CUVID / VAAPI 兼容映射表与 P0 API 覆盖基线

> 版本：v1.0（合并原《CUVID / NVENC ↔ VAAPI 兼容映射表》与《P0 API 覆盖基线》）
> 文档日期：2026-11-12
> 目标：源码级重编译后能跑，不要求严格二进制兼容。
> 基线：P0 验证机为 RX 7900 XTX / Mesa 25.2.8 / libva 2.20.0（VA-API 1.20）/ 自编译 FFmpeg 7.1；nv-codec-headers 为 `n12.1.14.1-1-g610325d`。历史格式对比机为 RX 9070 / Mesa 26 / FFmpeg 6.1，两者不要混用。
> 上位文档：[CUDA 兼容层总体方案](CUDA兼容层总体方案.md)（HIPIFY + shim 双轨架构，本文属“轨 2：shim 库”）。
> 职责：本文是**符号范围契约**（哪些 API 在 P0 内、哪些明确不承诺）与**字段级翻译手册**的唯一权威文档。内存/同步语义见[内存与同步模型](内存与同步模型.md)，格式能力见[NVDEC 与 VAAPI 解码对比](NVDEC与VAAPI解码对比.md)。

---

## 0. P0 API 覆盖基线

> 状态：符号范围已确认。parser 复用 FFmpeg 解析已在 AMD 验证机通过 I-only 与 I/P/B 测试；P0 双后端四路码流逐字节通过，`verify_kernel_read` errors=0（见 [P0 结果](../evidence/P0闭环验证结果_2026-11-12.md)）；FFmpeg 宿主双路径（`h264_cuvid` / `h264_nvdec`）尚未正式验收。

### 0.1 CUVID 动态加载符号面

Linux 64 位构建使用 `cuvidMapVideoFrame64` / `cuvidUnmapVideoFrame64`。除 `cuvidGetDecoderCaps` 为可选加载外，下列其余符号必须满足宿主动态加载要求；**符号可解析不等于功能已实现**。

**Decoder / mapping / synchronization**

- `cuvidGetDecoderCaps`
- `cuvidCreateDecoder`
- `cuvidDestroyDecoder`
- `cuvidDecodePicture`
- `cuvidGetDecodeStatus`
- `cuvidReconfigureDecoder`
- `cuvidMapVideoFrame64`（非 64 位形态为 `cuvidMapVideoFrame`）
- `cuvidUnmapVideoFrame64`（非 64 位形态为 `cuvidUnmapVideoFrame`）
- `cuvidCtxLockCreate`
- `cuvidCtxLockDestroy`
- `cuvidCtxLock`
- `cuvidCtxUnlock`

**Video source**

P0 不使用 video source 功能，但加载表要求这些符号；实现不得让调用方误以为 source 已受支持。

- `cuvidCreateVideoSource`
- `cuvidCreateVideoSourceW`
- `cuvidDestroyVideoSource`
- `cuvidSetVideoSourceState`
- `cuvidGetVideoSourceState`
- `cuvidGetSourceVideoFormat`
- `cuvidGetSourceAudioFormat`

**Parser**

- `cuvidCreateVideoParser`
- `cuvidParseVideoData`
- `cuvidDestroyVideoParser`

### 0.2 FFmpeg 直接 CUVID 功能调用

`h264_cuvid`：

- `cuvidGetDecoderCaps`
- `cuvidCreateDecoder`
- `cuvidDestroyDecoder`
- `cuvidDecodePicture`
- `cuvidCreateVideoParser`
- `cuvidParseVideoData`
- `cuvidDestroyVideoParser`
- `cuvidMapVideoFrame64`
- `cuvidUnmapVideoFrame64`

`h264_nvdec`：

- `cuvidGetDecoderCaps`
- `cuvidCreateDecoder`
- `cuvidDestroyDecoder`
- `cuvidDecodePicture`
- `cuvidMapVideoFrame64`
- `cuvidUnmapVideoFrame64`

两条宿主路径必须分别验收：`h264_cuvid` 使用 shim parser 回调；`h264_nvdec` 直接提供 FFmpeg 已解析的 `CUVIDPICPARAMS`。

### 0.3 libcuda 范围

保留 nv-codec-headers 动态加载所需符号面；P0 只承诺 FFmpeg 解码和 NPP 联合链路实际使用的功能子集。未实现接口必须返回明确的不支持错误。

FFmpeg 7.1 静态扫描发现的直接调用：

`cuInit`、`cuDeviceGetCount`、`cuDeviceGet`、`cuDeviceGetAttribute`、`cuDeviceGetUuid`、`cuDevicePrimaryCtxRetain`、`cuDevicePrimaryCtxRelease`、`cuDevicePrimaryCtxSetFlags`、`cuDevicePrimaryCtxGetState`、`cuCtxCreate`、`cuCtxDestroy`、`cuCtxGetCurrent`、`cuCtxPushCurrent`、`cuCtxPopCurrent`、`cuMemAlloc`、`cuMemFree`、`cuMemcpy2DAsync`、`cuStreamSynchronize`。

实际导出名称按加载表使用 `_v2` 后缀。NPP 联合链路所需的其他接口在联调时补充，不能由本列表推定完整 CUDA driver API 兼容。

### 0.4 NPP P0

FFmpeg 7.1 `vf_scale_npp.c` 的直接 NPP 调用固定为：

- `nppiResizeSqrPixel_8u_C1R`
- `nppiYCbCr420_8u_P2P3R`
- `nppiYCbCr420_8u_P3P2R`

另需提供上述调用及 FFmpeg 构建/运行必需的 nppc 接口。P0 使用 CUDA/NPP 11.5 ABI，SONAME 为 `libnpp*.so.11`，不承诺完整 11 个子库。NVIDIA 参考环境为 RTX 4060、驱动 595.99.02、CUDA Runtime 11.5、NPP 11.5.1.107；P0 输出以同版本参考为准，要求逐字节一致。NPP 的实现与许可细节见 [NPP 兼容方案与 VPE 对比](NPP兼容方案与VPE对比.md)。

### 0.5 不纳入 P0 的承诺

- NVENC 和编码数据通路。
- H.264 以外的 codec、非 NV12、非 8-bit 4:2:0。
- 场/MBAFF；遇到时必须明确返回不支持。
- 完整 CUDA/NPP 二进制 drop-in、完整 NPP、任意用户应用兼容。
- 性能达标：P0 只记录性能，不设性能通过阈值。

---

## 1. 产物与 SONAME

FFmpeg 的 `dynlink_loader.h` 按固定名字 `dlopen`：

| 产物 | 覆盖的接口 | 内部依赖 | P0 范围 |
|---|---|---|---|
| `libcuda.so.1` | CUDA driver API（`cuCtxGetCurrent` / `cuMemAlloc` / `cuMemcpy2DAsync` 等） | HIP 运行时（`libamdhip64`） | ✅ 是 |
| `libnvcuvid.so.1` | CUVID 解码 + parser | `libcuda.so.1` + `libva` + `libvulkan` | ✅ 是 |
| `libnvidia-encode.so.1` | NVENC 编码（`nvEncodeAPI.h` 函数表） | `libcuda.so.1` + `libva` | ❌ 否，不在 P0 |

符号按名 `dlsym` 查找，无版本脚本要求；只需导出同名符号。`libcuda.so.1` 是 `libnvcuvid.so.1` 的前置依赖。

---

## 2. 底层解码器（`cuviddec.h`）→ libva 函数映射

### 2.1 函数映射总表

| CUVID 函数（`tcuvid*`） | 映射到 libva | 备注 |
|---|---|---|
| `cuvidGetDecoderCaps(CUVIDDECODECAPS*)` | `vaInitialize` + `vaQueryConfigProfiles` + `vaQueryConfigEntrypoints` + 静态 caps 表（§5） | 按 codec×chroma×bitdepth 查表回填 |
| `cuvidCreateDecoder(CUvideodecoder*, CUVIDDECODECREATEINFO*)` | `vaCreateConfig` + `vaCreateContext` + `vaCreateSurfaces` | 结构体字段映射见 §4.1 |
| `cuvidDestroyDecoder` | `vaDestroySurfaces` + `vaDestroyContext` + `vaDestroyConfig` | |
| `cuvidDecodePicture(h, CUVIDPICPARAMS*)` | `vaBeginPicture` + `vaRenderPicture`(SPS/PPS/slice) + `vaEndPicture` | 见 §4.2 |
| `cuvidGetDecodeStatus(h, nPicIdx, CUVIDGETDECODESTATUS*)` | shim 内部 surface 状态表 + `vaSyncSurface` | VAAPI 无逐帧查询，shim 自维护 |
| `cuvidReconfigureDecoder` | 销毁重建 context（VAAPI 无动态 reconfigure） | 仅限分辨率/目标尺寸变化；当前未作为 P0 验收项 |
| `cuvidMapVideoFrame64(h, idx, *dptr, *pitch, CUVIDPROCPARAMS*)` | `vaSyncSurface` + surface 导出/导入；若带 target 尺寸则接 VPP | 见 §4.3 |
| `cuvidUnmapVideoFrame64` | 销毁导入对象 / 标记 surface 可复用 | |
| `cuvidCtxLockCreate/Destroy/Lock/Unlock` | 宿主递归 mutex | **不是 HIP context lock**；只保护 shim 内部临界区 |

> `cuvidMapVideoFrame` / `cuvidUnmapVideoFrame`（32 位指针版）仅在 `!__CUVID_DEVPTR64` 时定义；64 位平台一律走 `MapVideoFrame64`。

### 2.2 CUresult 错误码

返回类型统一 `CUresult`（`CUDA_SUCCESS=0`）。shim 定义完整 `CUDA_ERROR_*` 枚举，与 `VAStatus` 做双向映射：

- 不支持 → `CUDA_ERROR_NOT_SUPPORTED (201)`
- 参数非法 → `CUDA_ERROR_INVALID_VALUE (1)`
- 内存不足 → `CUDA_ERROR_OUT_OF_MEMORY (2)`

---

## 3. 上层 parser / source（`nvcuvid.h`）

### 3.1 接口与回调

| API / 回调 | 说明 |
|---|---|
| `cuvidCreateVideoParser(CUvideoparser*, CUVIDPARSERPARAMS*)` | 创建 parser 状态机，注册 5 个回调 |
| `cuvidParseVideoData(obj, CUVIDSOURCEDATAPACKET*)` | 喂裸码流，同步触发回调 |
| `cuvidDestroyVideoParser` | 销毁 |
| `cuvidCreateVideoSource(W)` / `DestroyVideoSource` | demuxer 层，可选 |
| `cuvidSet/GetVideoSourceState`、`cuvidGetSourceVideoFormat/AudioFormat` | source 层，可选 |
| `PFNVIDSEQUENCECALLBACK` | 码流头变化时回调，应用在此调 `cuvidCreateDecoder` |
| `PFNVIDDECODECALLBACK` | 每帧就绪回调，应用在此调 `cuvidDecodePicture` |
| `PFNVIDDISPLAYCALLBACK` | 显示顺序回调 |
| `PFNVIDOPPOINTCALLBACK` | AV1 可伸缩码流 operating point 选择 |
| `PFNVIDSEIMSGCALLBACK` | SEI 消息 |

### 3.2 parser 是 P0 的硬需求

CUVID 的 NVDEC 硬件可以“吃完整码流自解析”：FFmpeg `nvdec.c` 调 `cuvidDecodePicture` 时，`CUVIDPICPARAMS` 只填 4 个 bitstream 字段（`nBitstreamDataLen` / `pBitstreamData` / `nNumSlices` / `pSliceDataOffsets`），`CodecSpecific` 留空。

而 VAAPI 的 VLD 必须由 host 预解析成 `VAPictureParameterBuffer*` + `VASliceParameterBuffer*` + slice data。因此 shim 内部必须有 H.264/HEVC/VP9/AV1 的 bitstream parser，**这个 parser 绕不开**。

---

## 4. 结构体字段映射要点

### 4.1 `CUVIDDECODECREATEINFO` → VAAPI

| CUVID 字段 | 映射 |
|---|---|
| `CodecType` + `ChromaFormat` + `bitDepthMinus8` | 三者共同决定 `VAProfile`（§6.1 查表） |
| `ulWidth` / `ulHeight` | surface 尺寸 |
| `ulNumDecodeSurfaces` | `vaCreateSurfaces` 的 `num_surfaces`，建议 = min_num_decode_surfaces + 3 |
| `OutputFormat` | `VA_RT_FORMAT`（§6.2 查表） |
| `ulCreationFlags` | 仅 `cudaVideoCreate_PreferCUVID` 支持；`PreferCUDA` / `PreferDXVA` 返回不支持 |
| `DeinterlaceMode` | Weave→直通；Bob/Adaptive→`VAProcFilterDeinterlacing`（VPP 层） |
| `display_area` / `target_rect` | VPP 的 crop / scale（应用设了非零值时） |
| `ulTargetWidth/Height` | VPP scale 目标尺寸（偶数对齐） |
| `vidLock` | 当前不作为功能开关；shim 用宿主 mutex 保护内部状态，不映射 HIP context lock |

### 4.2 `CUVIDPICPARAMS` → `VAPictureParameterBuffer*` + `VASliceParameterBuffer*`

**方向关键**：CUVID 结构体是“全量头信息”，VAAPI 只需 SPS/PPS + slice offset。所以是“parse 全量 → 填 CUVID（回调用）→ 同时抽 VAAPI 需要的子集喂 VLD”。

| codec | CUVID 载体 | VAAPI picture buffer | VAAPI slice buffer |
|---|---|---|---|
| H264 | `CUVIDH264PICPARAMS`（60+ 字段） | `VAPictureParameterBufferH264` | `VASliceParameterBufferH264` |
| HEVC | `CUVIDHEVCPICPARAMS`（100+ 字段） | `VAPictureParameterBufferHEVC` | `VASliceParameterBufferHEVC` |
| VP9 | `CUVIDVP9PICPARAMS` | `VAPictureParameterBufferVP9` | `VASliceParameterBufferVP9` |
| AV1 | `CUVIDAV1PICPARAMS`（200+ 字段） | `VAPictureParameterBufferAV1` | `VASliceParameterBufferAV1` |
| MPEG2 | `CUVIDMPEG2PICPARAMS` | `VAPictureParameterBufferMPEG2` | `VASliceParameterBufferMPEG2` |
| VC1 | `CUVIDVC1PICPARAMS` | `VAPictureParameterBufferVC1` | `VASliceParameterBufferVC1` |
| JPEG | `CUVIDJPEGPICPARAMS` | `VAPictureParameterBufferJPEGBaseline` | `VASliceParameterBufferJPEGBaseline` |

> P0 只实现 H.264；上表其余 codec 是字段对照参考。AV1 的 film grain（`apply_grain` + 14 组 scaling points）在 `VAPictureParameterBufferAV1` 的 film_grain 子结构里，是 AV1 映射里最容易错的一块。

### 4.3 `cuvidMapVideoFrame64` 后处理语义

`CUVIDPROCPARAMS` 字段按需实现：

- 仅解码输出 → `vaSyncSurface` + 线性 surface 导出（零拷贝路径见[内存与同步模型](内存与同步模型.md)）。
- `progressive_frame` / `top_field_first` / `second_field` → VPP deinterlace 参数；场模式当前返回不支持。
- `raw_input_dptr` / `raw_output_dptr` → raw YUV 扩展，先不支持（返回错误）。
- `histogram_dptr` → 先回 `NULL`（`bIsHistogramSupported=0`）。
- `output_stream` → **当前未真正接入**；map 是同步的，比 CUDA 的异步 map 语义更强，但性能语义不同。

---

## 5. caps 回填表（`cuvidGetDecoderCaps`）

`bIsSupported` 按 codec×chroma×bitdepth 查表。下表是历史能力对比结论，**不等同于 P0 验证机实测**；P0 只承诺 H.264 / 8-bit / 4:2:0 / NV12。

| codec | chroma | bitdepth | bIsSupported |
|---|---|---|---|
| H264 | 420 | 8 | 1 |
| H264 | 422 / 444 | 8 | 1（历史对比机 VCN5 结论） |
| H264 | 420 | 10 | 1（历史对比机 VCN5 结论） |
| HEVC | 420 | 8 / 10 / 12 | 1 |
| HEVC | 422 / 444 | 8 | 1 |
| AV1 | 420 | 8 / 10 | 1 |
| VP9 | 420 | 8 / 10 | 1 |
| MPEG2 | 420 / 422 | 8 | 1 |

回填值（**AMD 侧 caps 尚未实测**，当前以 RTX 4060 参考值占位）：

- `nMaxWidth=4096, nMaxHeight=4096, nMaxMBCount=65536, nMinWidth=48, nMinHeight=16`
- `nOutputFormatMask`：按支持位深置 NV12/P016/YUV444/YUV444_16Bit 位
- `bIsHistogramSupported=0`

> 待办：在目标 AMD 平台运行 `vaQueryConfigAttributes` 探测，替换占位值。在此之前，caps 只用于“不误报支持”，不能作为性能/格式承诺。

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

## 7. NVENC（不在 P0）

`libnvidia-encode.so.1` 和 34 项 `NV_ENCODE_API_FUNCTION_LIST` 映射不在 P0 范围。旧版本文档中的逐项映射表已不再维护，编码链路的现状见[内存与同步模型](内存与同步模型.md) §3.5：编码方向只验证了组件，链路未验收。

---

## 8. 相关文档

- 解码格式矩阵与历史对比：`docs/architecture/NVDEC与VAAPI解码对比.md` §4。
- 后处理（VPP/去隔行/histogram）能力边界：`docs/architecture/NPP兼容方案与VPE对比.md`。
- 零拷贝 surface 路径：`docs/architecture/内存与同步模型.md` §3。
- P0 结果与证据边界：`docs/evidence/P0闭环验证结果_2026-11-12.md`。

---

## 9. 附录：libcuda 实现状态

| 项目 | 状态 | 说明 |
|---|---|---|
| 项目骨架 | ✅ | `cuda_shim/`（include/src/tests + Makefile/CMakeLists.txt） |
| `libcuda.so.1` 编译 | ✅ | g++ 直编 |
| 82 个必需符号导出 | ✅ | `verify_symbols` 全通过（模拟 FFmpeg `cuda_load_functions` 的 dlsym 清单） |
| HIP 运行时链接 | ✅ | `libamdhip64` 正确依赖，无 undefined symbol |
| 功能子集 | ✅ | `test_func_full` 47 PASS / 0 FAIL；覆盖设备、上下文、内存、2D 拷贝、流/事件、primary ctx |
| 完整 ABI / UVM | ❌ | 不承诺完整 driver API/ABI 与 NVIDIA UVM 语义 |

关键实现决策：

1. **头文件策略**：`<ffnvcodec/dynlink_cuda.h>` 与 `<hip/hip_runtime_api.h>` 可在同一编译单元共存，需 `-D__HIP_PLATFORM_AMD__`。
2. **类型映射**：`CUdevice == hipDevice_t(int)`；`CUcontext ↔ hipCtx_t` 用 `reinterpret_cast`；`CUdeviceptr ↔ hipDeviceptr_t` 用 `uintptr_t` 中转。
3. **错误码**：显式 switch 映射，不依赖数值巧合。
4. **链接**：`-lamdhip64` 放在源文件后且用全路径（默认 `--as-needed` 会丢弃）。

---

## 10. 关键结论速查

### 10.1 加载机制

- FFmpeg 二进制不链接 libcuda（`ldd` 无、`nm -D` 无 `cuInit` 符号）。
- 全部通过 `compat/cuda/dynlink_loader.h` → `#include <ffnvcodec/dynlink_loader.h>` → `dlopen + dlsym`。
- LIBNAME 硬编码：`libcuda.so.1` / `libnvcuvid.so.1` / `libnvidia-encode.so.1`（Linux）。
- 加载时机：解码器 `init` 时一次性 `dlsym` 全部符号。

**推论**：提供同名 `.so` + 导出必需符号，即可让已编译的 FFmpeg 在这条 dlopen 路径上加载，无需改 FFmpeg 源码或重编译。**注意边界**：P0 宿主是自编译 FFmpeg 7.1，且 NPP 是编译时链接；本推论不适用于 NPP，也不承诺任意现有二进制 drop-in。

### 10.2 `libcuda.so.1` 符号面硬约束

`cuda_load_functions` 用 `LOAD_SYMBOL`（必需，缺失即失败）加载 **82 个符号**，用 `LOAD_SYMBOL_OPT`（可选）加载约 20 个。即使场景只用 nvdec，这 82 个必需符号也必须全部导出（哪怕 stub 返回 `CUDA_ERROR_NOT_SUPPORTED`）。

| 组 | 符号 | 实现策略 |
|---|---|---|
| 设备/上下文（22） | `cuInit`/`cuDriverGetVersion`/`cuDeviceGetCount`/`cuDeviceGet`/`cuDeviceGetAttribute`/`cuDeviceGetName`/`cuDeviceComputeCapability`/`cuCtxCreate`/`cuCtxSynchronize`/`cuCtxGetStreamPriorityRange`/`cuCtxGetCurrent`/`cuCtxSetLimit`/`cuCtxPushCurrent`/`cuCtxPopCurrent`/`cuCtxDestroy`/`cuCtxGetDevice` + PrimaryCtx 5 个 | HIP 直接映射 |
| 内存（28） | `cuMemAlloc`/`cuMemAllocPitch`/`cuMemAllocManaged`/`cuMemFree`/`cuMemHostAlloc`/`cuMemFreeHost`/`cuMemAllocAsync`/`cuMemFreeAsync` + Memset 6 + Memcpy 10 | HIP 直接映射 |
| 流/事件（12） | `cuStreamCreate` … `cuEventRecord` | HIP 直接映射 |
| launch/模块/JIT（10） | `cuLaunchKernel`/`cuLaunchHostFunc`/`cuLink*`/`cuModuleLoadData`/`cuModuleUnload`/`cuModuleGetFunction`/`cuModuleGetGlobal` | stub（视频路径不走） |
| texture/array（6） | `cuTexObjectCreate`/`Destroy`/`cuArrayCreate`/`cuArray3DCreate`/`cuArrayDestroy`/`cuArray3DGetDescriptor` | stub 返回 NOT_SUPPORTED |
| GL interop（7） | `cuGLGetDevices`/`cuGraphicsGLRegisterImage`/`cuGraphicsUnregisterResource`/`cuGraphicsMapResources`/`cuGraphicsUnmapResources`/`cuGraphicsSubResourceGetMappedArray`/`cuGraphicsResourceGetMappedPointer` | stub |

> `LOAD_SYMBOL` 里 `cuGraphicsResourceGetMappedPointer` 的 dlsym 字符串是 `"cuGraphicsResourceGetMappedPointer_v2"`，按字符串导出即可。

### 10.3 缩放路径

| 解码器 | 文件 | `ulTargetWidth` 语义 |
|---|---|---|
| `h264_cuvid` 等 | `cuviddec.c` | `(avctx->width + 1) & ~1`，默认 = crop 后尺寸对齐偶数，非缩放；仅当用户传 `-resize WxH` 时才真缩放 |
| `h264_nvdec` 等（hwaccel） | `nvdec.c` | `ulTargetWidth = coded_width`，完全不缩放 |

**结论**：map 阶段默认不需要 VPP 缩放。crop 通过 `display_area` 在 `cuvidCreateDecoder` 时配置；真缩放只在显式 `-resize` 时发生，P0 返回 `CUDA_ERROR_NOT_SUPPORTED`。

### 10.4 parser 复用 FFmpeg 的落地路径

parser 有两个职责：

1. **帧切分**：把 Annex-B 裸码流切成 frame（识别 start code / NAL 边界）。
2. **SPS/PPS/slice header 解析 → VAAPI 结构体**。

**坑**：`av_parser_parse2`（FFmpeg 公开 API）只做职责 1，不暴露 SPS/PPS 结构。SPS/PPS 解析在 decoder 内部（`h264_ps.c` 的 `ff_h264_decode_seq_parameter_set` 等），VAAPI 参数填充在 `vaapi_h264.c`（`fill_vaapi_pic` / `fill_vaapi_ReferenceFrames` 等）。

| 环节 | FFmpeg 对应代码 | 链接属性 | 复用方式 |
|---|---|---|---|
| SPS/PPS 解析 | `h264_ps.c` 的 `ff_h264_decode_seq_parameter_set` / `ff_h264_decode_picture_parameter_set` | 非 static（`ff_` 内部公共函数） | ✅ 静态链接 FFmpeg 对象直接调用 |
| VAAPI 参数填充 | `vaapi_h264.c` 的 `fill_vaapi_pic` / `fill_vaapi_ReferenceFrames` | 全是 static，且依赖 `H264Context` | ❌ 不能直接链接/提取，移植字段映射逻辑自写 |

早期建议“直接提取 `vaapi_h264.c`”不可行：填充函数是 static 且 `#include "h264dec.h"` 拖入整个 decoder 上下文。正确做法是**解析用 FFmpeg，填充参考 `vaapi_h264.c` 的字段映射自写**。

落地路径：

```text
Annex-B 裸码流
  → ff_h2645_packet_split()        # h2645_parse.c，切 NAL unit
  → GetBitContext 包住 SPS/PPS NAL
  → ff_h264_decode_seq_parameter_set()   # h264_ps.c，解析 SPS → H264ParamSets
  → ff_h264_decode_picture_parameter_set() # 解析 PPS
  → 自写 SPS/PPS → VAPictureParameterBufferH264 填充
  → VASliceParameterBufferH264 + slice data
```

`libnvcuvid.so.1` 链接 FFmpeg 7.1 的 **9 个解析对象**：`h2645_parse` / `h264_ps` / `h2645_vui` / `h2645data` / `h264data` / `h264_parse` / `golomb` / `mathtables` / `refstruct`，以及共享 `libavutil.so`。

### 10.5 三个 shim 的符号面汇总

| .so | 必需符号数 | 备注 |
|---|---|---|
| `libcuda.so.1` | 82（+20 可选） | 全部导出，一半映射 HIP、一半 stub |
| `libnvcuvid.so.1` | 22 | `cuvidGetDecoderCaps` 可选，其余全必需（含 source 层 7 个 + parser 3 个） |
| `libnvidia-encode.so.1` | 2 | 仅 `NvEncodeAPICreateInstance` + `NvEncodeAPIGetMaxSupportedVersion`；不在 P0 |
