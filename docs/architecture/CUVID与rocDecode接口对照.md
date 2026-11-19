# CUVID 与 rocDecode 接口对照

> 版本：v1.0
> 文档日期：2026-11-19
> 职责：记录 CUVID API 与 ROCm rocDecode 的逐项对照，作为[总体方案](CUDA兼容层总体方案.md) §1.2 “路线一：功能层兼容（应用可改代码）”的选型依据。本文只描述两侧公开接口与语义差异，不维护项目状态；当前实现与验证状态见[当前能力矩阵](../当前能力矩阵.md)。
> 接口事实来源：CUVID 侧为 ffnvcodec `n12.1.14.1` 的 `dynlink_cuviddec.h` / `dynlink_nvcuvid.h`（与 `cuda_shim` 使用的头文件一致）；rocDecode 侧为 ROCm 7.2.4 随 `rocdecode-dev` 1.7.0.70204 发布的 `rocdecode.h` / `rocparser.h` / `roc_bitstream_reader.h`。两端口径以对应版本头文件为准，换 ROCm 版本需重新核对。

---

## 1. 一句话结论

**rocDecode 的公开 API 几乎是 CUVID 的“改名克隆”**（decoder / parser / decode status / reconfigure 一一对应），但 **H.264 参数体是 VA-API 形状**（头文件注明 “same as VAPictureParameterBufferH264”）。因此：

- 接口层：CUVID→rocDecode 的胶水代码远少于 CUVID→Vulkan Video；
- 参数层：`CUVIDH264PICPARAMS → RocdecAvcPicParams + 逐片 slice params + IQ matrix` 与现有 `VaapiH264Decoder` 的 CUVID→VA-API 翻译同构，可复用；
- 行为层：取帧从“map/unmap + 单基址”变成“阻塞取帧 + 三平面指针”，Deinterlace、Histogram、CtxLock、video source、32 位 map 都没有对应。

## 2. 函数级对照

CUVID 公开 24 个函数；rocDecode 1.7 的公开头文件声明 16 个（另有样例使用、但公共头未声明的 `rocDecGetVideoFrameHost`，使用前需按目标 ROCm 核对）。

| 能力 | CUVID | rocDecode | 结论 |
|---|---|---|---|
| 能力查询 | `cuvidGetDecoderCaps` | `rocDecGetDecoderCaps` | 1:1，字段有差异 |
| 建/销毁解码器 | `cuvidCreateDecoder` / `cuvidDestroyDecoder` | `rocDecCreateDecoder` / `rocDecDestroyDecoder` | 1:1，去掉 CUDA 专属字段 |
| 解一帧 | `cuvidDecodePicture` | `rocDecDecodeFrame` | 外壳 1:1；参数体为 VA-API 形状 |
| 解码状态 | `cuvidGetDecodeStatus` | `rocDecGetDecodeStatus` | 枚举值相同，rocDecode 多 `Displaying=10` |
| 重配置 | `cuvidReconfigureDecoder` | `rocDecReconfigureDecoder` | 1:1 |
| 取帧 | `cuvidMapVideoFrame64` + `cuvidUnmapVideoFrame64` | `rocDecGetVideoFrame` | 2:1；3 平面指针 + 3 pitch、阻塞、无 unmap |
| 32 位取帧 | `cuvidMapVideoFrame` + `cuvidUnmapVideoFrame` | 无 | 只有 64 位指针 |
| Parser | `cuvidCreateVideoParser` / `cuvidParseVideoData` / `cuvidDestroyVideoParser` | `rocDecCreateVideoParser` / `rocDecParseVideoData` / `rocDecDestroyVideoParser` | 1:1，回调模型一致 |
| 上下文锁 | `cuvidCtxLockCreate` / `Destroy` / `Lock` / `Unlock` | 无 | CUDA context 锁；HIP 无对应 |
| Video source（旧 API） | `cuvidCreateVideoSource` / `W` / `DestroyVideoSource` / `SetVideoSourceState` / `GetVideoSourceState` / `GetSourceVideoFormat` / `GetSourceAudioFormat` | 无 | NVIDIA 已弃用；本仓库 shim 亦为 stub |
| 错误名 | 无（用 `CUresult`） | `rocDecGetErrorName` | rocDecode 独有 |
| 码流读取器 | 无 | `rocDecCreateBitstreamReader` / `GetBitstreamCodecType` / `GetBitstreamBitDepth` / `GetBitstreamPicData` / `DestroyBitstreamReader` | rocDecode 独有，给样例用的 elementary stream 读取器 |

> 本仓库 `libnvcuvid.so.1` 目前导出 22 个 CUVID 符号（21 个必需 + `cuvidGetDecoderCaps`），即上表 24 个中缺 32 位 map/unmap；其余按原语义 stub 或实现。

## 3. 结构体与字段

### 3.1 `CUVIDDECODECREATEINFO` ↔ `RocDecoderCreateInfo`

同名同义的字段：宽高、`ulNumDecodeSurfaces`、codec/chroma、`bitDepthMinus8`、`ulIntraDecodeOnly`、`ulMaxWidth/Height`、`display_area`、`OutputFormat`、`ulTargetWidth/Height`、`ulNumOutputSurfaces`、`target_rect`。

| CUVID 有、rocDecode 无 | 影响 |
|---|---|
| `ulCreationFlags`（Default/PreferCUDA/PreferDXVA/PreferCUVID） | HIP 下无意义 |
| `vidLock`（CUcontext 锁） | 由 shim 自身的用例锁替代 |
| `enableHistogram` | rocDecode 不支持直方图 |
| `DeinterlaceMode`（Weave/Bob/Adaptive） | **rocDecode 没有对应字段**，只有 proc params 的 `progressive_frame/top_field_first` 提示 |

rocDecode 额外有 `device_id`（多 GPU 选择）。

### 3.2 `CUVIDDECODECAPS` ↔ `RocdecDecodeCaps`

相同的输入项（codec/chroma/bit depth）和大部分输出项（`bIsSupported`/`nNumNVDECs` ↔ `is_supported`/`num_decoders`、`nOutputFormatMask` ↔ `output_format_mask`、最大/最小宽高）。差异：

- rocDecode 多 `device_id`；
- rocDecode **没有** `nMaxMBCount`，也**没有**直方图三项（`bIsHistogramSupported` / `nCounterBitDepth` / `nMaxHistogramBins`）。

### 3.3 `CUVIDPICPARAMS` ↔ `RocdecPicParams`

外壳一一对应：`CurrPicIdx` ↔ `curr_pic_idx`、field 标志、`ref_pic_flag`、`intra_pic_flag`、`nBitstreamDataLen` ↔ `bitstream_data_len`、`pBitstreamData` ↔ `bitstream_data`、`nNumSlices` ↔ `num_slices`。

两个关键差异：

| 项 | CUVID | rocDecode |
|---|---|---|
| 图画尺寸 | `PicWidthInMbs` / `FrameHeightInMbs`（宏块） | `pic_width` / `pic_height`（像素） |
| 片数据 | `pSliceDataOffsets`（偏移数组） | `slice_params.avc[]`：每片一个 VA-API 形状的 `RocdecAvcSliceParams`（偏移/大小/bit offset/first_mb/slice_type/ref idx/ref list/加权表/cabac/deblock/QP） |
| 图画参数 | `CodecSpecific.h264`（`CUVIDH264PICPARAMS`：`dpb[16]`、SPS/PPS 派生字段、缩放矩阵、加权预测） | `pic_params.avc`（`RocdecAvcPicParams`：`curr_pic` + `ref_frames[16]` + `seq_fields`/`pic_fields` 位域 + `iq_matrix`；头文件注明与 VA-API 一致） |

也就是说：**CUVID 的接口形状 → VA-API 的参数形状**，翻译工作量与现有 VAAPI 后端的 CUVID→VA-API 翻译同构。

### 3.4 取帧：`CUVIDPROCPARAMS` ↔ `RocdecProcParams`

| CUVID | rocDecode |
|---|---|
| 返回 1 个基址 + 1 个 pitch | 返回 `dev_mem_ptr[3]`（Y/U/V 平面）+ `horizontal_pitch[3]` |
| `second_field` / `unpaired_field` | 无（只有 `progressive_frame` / `top_field_first`） |
| `output_stream`（CUDA stream） | 无，`rocDecGetVideoFrame` 是**阻塞调用**，等帧就绪 |
| `histogram_dptr` | 无 |
| 必须 `cuvidUnmapVideoFrame64` 配对 | **无 unmap**；帧与 `pic_idx` 绑定，`pic_idx` 复用或解码器销毁时回收 |
| `raw_input_dptr`（CUdeviceptr） | `raw_input_dptr` / `raw_output_dptr`（HIP 指针）+ pitch/format |

对 API 兼容层的直接影响：`cuvidMapVideoFrame64` 约定返回**一个** `CUdeviceptr` + 一个 pitch；只有 `plane[1] == plane[0] + pitch*H`（NV12 连续）时才能自然满足，否则需要合成或拷贝。现有 `HipFrameMapping` 的“连续 NV12 + uv_offset = pitch*H”假设在这里同样受限。

### 3.5 Parser：`CUVIDPARSERPARAMS` ↔ `RocdecParserParams`

| CUVID | rocDecode |
|---|---|
| `CodecType`、`ulMaxNumDecodeSurfaces`、`ulClockRate`、`ulErrorThreshold`、`ulMaxDisplayDelay`、`bAnnexb`、`pUserData` | 全部存在，仅命名风格不同 |
| `pfnSequenceCallback(CUVIDEOFORMAT*)` | `pfn_sequence_callback(RocdecVideoFormat*)` |
| `pfnDecodePicture(CUVIDPICPARAMS*)` | `pfn_decode_picture(RocdecPicParams*)` |
| `pfnDisplayPicture(CUVIDPARSERDISPINFO*)` | `pfn_display_picture(RocdecParserDispInfo*)` |
| `pfnGetSEIMsg` | `pfn_get_sei_msg` |
| `pfnGetOperatingPoint`（AV1） | 无（保留位） |
| `CUVIDSOURCEDATAPACKET`（flags/payload/timestamp） | `RocdecSourceDataPacket`，**flags 数值完全一致**（EOS=1、TIMESTAMP=2、DISCONTINUITY=4、ENDOFPICTURE=8、NOTIFY_EOS=0x10） |
| 回调返回值语义（0 失败、≥1 成功、>1 覆盖 DPB 大小） | 文档描述一致 |

## 4. 枚举与错误码

| 枚举 | 对照 |
|---|---|
| chroma | 0=Monochrome、1=420、2=422、3=444，数值一致 |
| surface format | NV12=0、P016=1、YUV444=2、YUV444_16Bit=3，前四项与 CUVID 完全一致；rocDecode 额外 YUV420/420_16Bit/422/422_16Bit（4–7），`output_format_mask` 兼容 |
| codec | **数值不一致**：CUVID `H264=4`、`VC1=3`、`HEVC=8`、`VP8=9`、`VP9=10`、`AV1=11`；rocDecode `AVC=3`、`HEVC=4`、`AV1=5`、`VP8=6`、`VP9=7`、`JPEG=8`。必须按名字映射，不能直接传值 |
| decode status | 0/1/2/8/9 完全一致；rocDecode 多 `Displaying=10` |
| 错误码 | CUVID 返回 `CUresult`（0、1、2、400、801…）；rocDecode 返回 `rocDecStatus`（0、-1…-8），需要映射表 |

## 5. 行为差异与接入影响

1. **取帧模型**：阻塞取帧、三平面指针、无 unmap；与 CUVID 的 map/unmap + 单基址不一一对应。
2. **缺失能力**：Deinterlace（Bob/Adaptive）、Histogram、32 位 map、CtxLock、video source、AV1 operating point。本项目 shim 对这些本来就返回不支持或是 stub，缺口与现状对齐。
3. **能力上限**：rocDecode 底层是 VA-API/Mesa，与本项目 VAAPI 后端同源；[LINEAR 直出探针](../../hip-decode-20261119/README.md) 测到的 H.264 8-bit 4:2:0 边界不会因换 rocDecode 而扩大。
4. **输出布局**：`rocDecGetVideoFrame` 只给平面指针和 pitch，头文件未承诺线性；能否被 HIP 直接按线性读取，需在目标机实测（接入前第一个验证点）。
5. **可只用解码器**：本项目已有 `CuvidParser` + `CUVIDPICPARAMS`，可以不使用 rocDecode 自带 parser，直接翻译成 `RocdecPicParams` 调 `rocDecDecodeFrame`，避免与它的 parser/display 队列冲突。
6. **FFmpeg 宿主**：FFmpeg 7.1 没有 rocDecode hwaccel（本机核对：`libavutil` 无 HIP pixfmt/hwcontext，`libavcodec` 无 rocDecode 代码）。路线一要么应用直接调 rocDecode，要么自行给 FFmpeg 增加解码后端。

## 6. 若作为兼容层后端的工作量

- **接口胶水**：比 Vulkan 后端少——parser 三件套、解码/状态/重配置均 1:1，取帧是单个阻塞函数，不需要 fence/信号量流水线。
- **参数翻译**：`CUVIDH264PICPARAMS → RocdecAvcPicParams + RocdecAvcSliceParams[] + RocdecAvcIQMatrix`，与现有 VAAPI 翻译同构，可复用/改造。
- **必须补充**：错误码映射、单指针合成（或改帧映射按平面处理）、`Displaying` 状态语义、无 unmap 后的回收时机、输出布局实测。
- **仍然需要**：`libcuda`/`libnvcuvid` 门面、NPP shim、FFmpeg 宿主支持；rocDecode 只替换解码后端，不解决这些。
- **建议**：若作为路线二的第三个后端候选，先做 `RocDecodeH264Decoder` spike，与 `VaapiH264Decoder` 并列跑现有 `verify_kernel_read` + dump 对比，重点验证输出 plane 指针是否线性、`curr_pic_idx`/DPB 语义和取帧阻塞时序。

## 7. 版本与口径注意

- 本文按 rocDecode **1.7.0** 头文件整理；ROCm 7.2.4 的 `rocprofiler-sdk` 内还带一份 vendored 的 0.10 头，**没有 parser API**，不能拿来对照。
- 1.7 样例调用 `rocDecGetVideoFrameHost`，但公共头未声明，属于样例/头文件版本不一致；以实际 `rocdecode.h` 为准。
- CUVID 侧以 ffnvcodec `n12.1.14.1` 为准；NV 官方 `nvcuvid.h` 可能多出版本相关符号，需按客户实际头文件核对。
