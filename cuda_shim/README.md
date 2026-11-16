# cuda_shim — `libcuda.so.1` + `libnvcuvid.so.1`

CUDA 驱动 API 与 CUVID 解码 API 的兼容层：

- `libcuda.so.1`：设备/上下文/内存/流/事件映射到 HIP；kernel launch、module/JIT、GL interop 等返回明确的不支持错误。
- `libnvcuvid.so.1`：CUVID parser、decoder、`cuvidMapVideoFrame64` / `cuvidUnmapVideoFrame64`；当前运行时默认 VAAPI，设置 `CUVID_SHIM_BACKEND=vk` 切到 Vulkan Video 后端。该实现默认值不代表方案选型；两个后端仍是同级候选，等待正式决策。

实现与约束见：

- [内存与同步模型](../docs/architecture/内存与同步模型.md)
- [Vulkan 后端实现](../docs/architecture/Vulkan后端实现.md)
- [CUVID 到 VAAPI 映射表与 P0 API 覆盖](../docs/architecture/CUVID到VAAPI映射表.md)
- [验证手册](../docs/verification/验证手册.md)

## 构建

依赖 ROCm、libva/libdrm、Vulkan loader、FFmpeg 7.1 的 9 个解析对象、nv-codec-headers；Vulkan 后端还需要 Mesa 的 `vk_video/*` 头。

```bash
cd cuda_shim

make -j"$(nproc)" libcuda.so.1 libnvcuvid.so.1 \
  ROCM_DIR=/opt/rocm \
  FFMPEG_DIR=/path/to/ffmpeg-7.1 \
  NVCODEC_DIR=/path/to/nv-codec-headers/include \
  MESA_DIR=/path/to/mesa \
  VULKAN_LIB=-l:libvulkan.so.1
```

常用验证目标：

```bash
make check                    # 符号面（模拟 FFmpeg dlsym）
make check_func               # libcuda 功能子集（47 项）
make verify_cuvid_session     # CUVID 会话创建（无像素）
make verify_cuvid_parser      # parser 回调（无解码）
make verify_kernel_read       # mapped dev_ptr 的 HIP kernel 读取 + event 顺序
```

`verify_kernel_read` 是设备端测试，必须用 `hipcc`；如果 ROCm clang 找不到系统 libstdc++，用 `HIPCC_EXTRA` 补路径（见[验证手册](../docs/verification/验证手册.md)）。

## 运行

```bash
export LD_LIBRARY_PATH=$PWD:/path/to/ffmpeg-7.1/libavutil:${LD_LIBRARY_PATH:-}

./verify_symbols ./libcuda.so.1
./verify_cuvid_symbols ./libnvcuvid.so.1
./test_func_full

CUVID_SHIM_BACKEND=vaapi ./verify_kernel_read ../testdata/h264/test_p_b.h264

# 验证机脚本显式设置；是否需要因 Mesa 版本而异，见验证手册
RADV_PERFTEST=video_decode CUVID_SHIM_BACKEND=vk \
  ./verify_kernel_read ../testdata/h264/test_p_b.h264
```

`CUDA_SHIM_TRACE_COPY=1` 会统计 D2D 的 `cuMemcpy2D*` / `cuMemcpyDtoD*` 次数，进程退出时打印汇总；用于区分 `h264_nvdec` 的真零拷贝与 `h264_cuvid` 的 FFmpeg 内部拷贝。

排障/验证开关（都不影响默认路径）：

| 变量 | 作用 |
|---|---|
| `RADV_PERFTEST=video_decode` | 验证机脚本显式设置；部分 Mesa 版本可能默认暴露视频队列族，尚未做同版本 A/B 对照 |
| `CUVID_SHIM_DEBUG_VK=1` | 每帧打印解码参数（type/poc/参考槽/setup 槽/DPB 空闲表） |
| `CUVID_SHIM_NOCOPY=1` | 只解码不导出，分离“解码参数问题”与“同步/导出问题” |
| `CUVID_SHIM_TRACE_PARAMS=1` | 打印每次 `cuvidDecodePicture` 的 frame_num/POC/DPB，用于参考帧问题定位 |
| `CUVID_SHIM_TRACE_SURF=1` | 打印解码目标面与 map/unmap 顺序 |
| `CUVID_DUMP_NV12_DIR=<dir>` | 每帧 dump NV12，用于和软解参考逐帧逐平面比对 |

## 状态与边界

- 解码正确性、B 帧根因与验证结果以 [Vulkan 后端实现 §8](../docs/architecture/Vulkan后端实现.md#8-上机验证结论rx-7900-xtx--radv) 和 [P0 闭环验证结果](../docs/evidence/P0闭环验证结果_2026-11-12.md) 为准。
- 四路验证码流中 `test_p_b_5` / `test_p_b_7` 含 B 帧，`test_p_main` / `test_p_66` 是无 B 帧对照。
- FFmpeg 宿主矩阵 2026-11-13 修复后为 32/32 PASS：两后端 `nvdec-unsafe` 均为 D2D=0 且像素一致。零拷贝需要 `-extra_hw_frames`，shim 接受最多 64 个 decode surface。详见[宿主基线](../docs/evidence/FFmpeg宿主双后端基线_2026-11-13/README.md)。
- Vulkan 的 `h264_nvdec` 会从 `CUVIDH264PICPARAMS` 合成最小 SPS/PPS。多 slice、1080p 裁剪和 MP4 seek 已在 cuvid/nvdec × 双后端共 12 个组合通过；多流、动态分辨率、长期参考和 POC type 1 尚未覆盖；两个后端同级。
- parser 按 `first_mb_in_slice == 0` 组装多 slice picture 并提交 `nNumSlices`/`pSliceDataOffsets`；新 SPS 尺寸变化会重新触发 sequence callback 并清空 DPB。CUVID 级宿主重建解码器时，320×240 → 640×480 双后端 22/22 帧通过；FFmpeg CLI 仍会因固定 `AVHWFramesContext` 拒绝中途改分辨率。
- `VkSequenceCache` 为每线程单槽：单进程 8 路并发（每路一个线程）双后端逐字节通过，同一线程内交错驱动多个 decoder 不支持。
- `cuvidMapVideoFrame64` 在目标高度小于 coded 高度（如 1080 vs 1088）时重排成 target-height NV12 视图；unmap 时同步设备，避免异步 D2D 拷贝与 VA 面复用竞争。
- 异步跨栈 fence 未实现；当前是主机同步基线。
- 含 host 内存的 `cuMemcpy2DAsync` 走同步 `hipMemcpyParam2D`：CUDA 对 pageable 内存的“异步”拷贝在返回前已完成 staging，调用方可以立即复用源缓冲；直接转 `hipMemcpyParam2DAsync` 会让 FFmpeg 在上传后回收源帧时产生跨帧损坏。设备到设备拷贝仍保持异步。回归测试为 `tests/npp/verify_hwupload_roundtrip.sh`。
- 本目录的测试是可行性验证，不是生产验收。
