# P0 API 覆盖基线

> 状态：接口范围已确认；parser 复用 FFmpeg 解析已在 AMD 验证机通过 I-only 与 I/P/B 测试；P0 双后端四路码流逐字节通过（VAAPI 82/82、Vulkan 82/82），`verify_kernel_read` errors=0；FFmpeg 宿主双路径（`h264_cuvid` / `h264_nvdec`）尚未正式验收。
> 宿主：官方 FFmpeg 7.1 源码，自编译；P0 不承诺不重编译的现有二进制。
> nv-codec-headers：`610325d019ad75248aa6e7abe0f4909d2ac6bc69`（`n12.1.14.1-1-g610325d`）。
> parser 实现：复用 FFmpeg `ff_h2645_packet_split` + `ff_h264_decode_seq_parameter_set` + `ff_h264_decode_picture_parameter_set`；`libnvcuvid.so.1` 链接 FFmpeg 7.1 的 8 个解析对象（`h2645_parse/h264_ps/h2645_vui/h2645data/h264data/golomb/mathtables/refstruct`）与共享 `libavutil.so`。

## 1. CUVID 动态加载符号面

Linux 64 位构建使用 `cuvidMapVideoFrame64` / `cuvidUnmapVideoFrame64`。除 `cuvidGetDecoderCaps` 为可选加载外，下列其余符号必须满足宿主动态加载要求；符号可解析不等于功能已实现。

### Decoder / mapping / synchronization

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

### Video source

P0 不使用 video source 功能，但加载表要求这些符号。实现不得让调用方误以为 source 已受支持。

- `cuvidCreateVideoSource`
- `cuvidCreateVideoSourceW`
- `cuvidDestroyVideoSource`
- `cuvidSetVideoSourceState`
- `cuvidGetVideoSourceState`
- `cuvidGetSourceVideoFormat`
- `cuvidGetSourceAudioFormat`

### Parser

- `cuvidCreateVideoParser`
- `cuvidParseVideoData`
- `cuvidDestroyVideoParser`

## 2. FFmpeg 直接 CUVID 功能调用

### `h264_cuvid`

- `cuvidGetDecoderCaps`
- `cuvidCreateDecoder`
- `cuvidDestroyDecoder`
- `cuvidDecodePicture`
- `cuvidCreateVideoParser`
- `cuvidParseVideoData`
- `cuvidDestroyVideoParser`
- `cuvidMapVideoFrame64`
- `cuvidUnmapVideoFrame64`

### `h264_nvdec`

- `cuvidGetDecoderCaps`
- `cuvidCreateDecoder`
- `cuvidDestroyDecoder`
- `cuvidDecodePicture`
- `cuvidMapVideoFrame64`
- `cuvidUnmapVideoFrame64`

两条路径必须分别验收：`h264_cuvid` 使用 shim parser 回调；`h264_nvdec` 直接提供 FFmpeg 已解析的 `CUVIDPICPARAMS`。

## 3. libcuda 范围

保留 nv-codec-headers 动态加载所需符号面；P0 只承诺 FFmpeg 解码和 NPP 联合链路实际使用的功能子集。未实现接口必须返回明确的不支持错误。

FFmpeg 7.1 静态扫描发现的直接调用：

- `cuInit`
- `cuDeviceGetCount`
- `cuDeviceGet`
- `cuDeviceGetAttribute`
- `cuDeviceGetUuid`
- `cuDevicePrimaryCtxRetain`
- `cuDevicePrimaryCtxRelease`
- `cuDevicePrimaryCtxSetFlags`
- `cuDevicePrimaryCtxGetState`
- `cuCtxCreate`
- `cuCtxDestroy`
- `cuCtxGetCurrent`
- `cuCtxPushCurrent`
- `cuCtxPopCurrent`
- `cuMemAlloc`
- `cuMemFree`
- `cuMemcpy2DAsync`
- `cuStreamSynchronize`

实际导出名称按加载表使用 `_v2` 后缀。NPP 联合链路所需的其他接口在联调时补充到清单，不能由本列表推定完整 CUDA driver API 兼容。

## 4. NPP P0

FFmpeg 7.1 `vf_scale_npp.c` 的直接 NPP 调用固定为：

- `nppiResizeSqrPixel_8u_C1R`
- `nppiYCbCr420_8u_P2P3R`
- `nppiYCbCr420_8u_P3P2R`

另需提供上述调用及 FFmpeg 构建/运行必需的 nppc 接口。P0 使用 CUDA/NPP 11.5 ABI，SONAME 为 `libnpp*.so.11`，不承诺完整 11 个子库。

NVIDIA 参考环境：RTX 4060、驱动 595.99.02、CUDA Runtime 11.5、NPP 11.5.1.107。所有支持函数的 P0 输出以 NVIDIA 同版本参考为准，要求逐字节一致。

## 5. 不纳入 P0 的承诺

- NVENC 和编码数据通路。
- H.264 以外的 codec、非 NV12、非 8-bit 4:2:0。
- 场/MBAFF；遇到时必须明确返回不支持。
- 完整 CUDA/NPP 二进制 drop-in、完整 NPP、任意用户应用兼容。
- 性能达标：P0 只记录性能，不设性能通过阈值。
