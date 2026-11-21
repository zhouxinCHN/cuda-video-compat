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
make check                    # 符号面（模拟 FFmpeg dlsym）+ DPB 纯组件测试
make check_h264_dpb           # progressive H.264 DPB 纯组件测试（仅 C++17）
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
| `CUVID_SHIM_DEBUG_VK=1` | 每帧打印解码参数（type/poc/参考槽/setup 槽/DPB 空闲表），以及查询到的 tiling、DPB/output 分离能力和流水线深度 |
| `CUVID_SHIM_NOCOPY=1` | 只解码不做导出拷贝，分离“解码参数问题”与“同步/导出问题”；映射仍可成功，但读到的是未被拷贝更新的 buffer |
| `CUVID_SHIM_PIPELINE_DEPTH=<n>` | Vulkan 后端允许同时在飞的帧数（1–8，默认 4）；设为 1 退回逐帧同步行为。环满时 `decode()` 会回收最早的在飞帧 |
| `CUVID_SHIM_VK_TILING=linear\|optimal` | 验证用：强制输出/DPB 图像的 tiling，绕过驱动 format query 的宣告，用来端到端验证 LINEAR 退化路径；不是生产开关 |
| `CUVID_SHIM_TRACE_PARAMS=1` | 打印每次 `cuvidDecodePicture` 的 frame_num/POC/DPB，用于参考帧问题定位 |
| `CUVID_SHIM_TRACE_SURF=1` | 打印解码目标面与 map/unmap 顺序 |
| `CUVID_DUMP_NV12_DIR=<dir>` | 每帧 dump NV12，用于和软解参考逐帧逐平面比对 |

## 后端无关 DPB 与语法适配（第一 / 二阶段）

`src/h264_dpb.{h,cpp}` 参考 `rocm-systems/projects/rocdecode` 的 AVC parser，实现 progressive frame 的 POC 0/1/2、MMCO 1–6、短期/长期参考、滑动窗口、参考列表重排和 POC 顺序输出。它只接收已解析的语法结构，不依赖 VAAPI、Vulkan、HIP 或 FFmpeg。

`src/h264_dpb_adapter.{h,cpp}` 把 FFmpeg SPS/PPS/slice header 或 `CUVIDH264PICPARAMS` 转成统一模型。CUVID 直接路径使用 `CurrFieldOrderCnt`，不会因缺少 POC1 cycle offsets 而重算错误的 POC。

```bash
make -C cuda_shim check_h264_dpb          # 纯状态机，仅 C++17
make -C cuda_shim check_h264_dpb_adapter  # 语法适配 + 真实码流差分，需 FFmpeg 7.1
```

适配测试覆盖合成 POC1/MMCO/List0/List1 slice header，以及 `testdata/h264` 的 12 路码流、242 帧 POC/参考列表/DPB 检查。可用 `H264_STREAM=test_p_b.h264` 限定单文件定位。

2B-1 已让 `CuvidParser` 使用共享 DPB，并在同步 `pfnDecodePicture` 作用域内通过 `H264DecodeContext` 向 VAAPI 提供共享参考列表。`H264SurfaceTracker` 将 surface 从 display 一直保持到 unmap，关闭 display→map 窗口期的复用。

2B-2 已在同一 context 下用共享 DPB 驱动 Vulkan reference slots：`setup_shared_dpb()` 负责 picture_index→slot 映射和长期参考标记，不再重复执行本地 POC/MMCO。两个后端的现有宿主、PTS、复杂场景、动态分辨率和多流回归均通过。nvdec 没有该 context 时仍走原 CUVID DPB/本地状态机；现有 242 帧差分语料中 LTR、MMCO5 和真实 POC1 均为 0，因此这些能力仍需专用码流做端到端验收。

## 状态与边界

当前实现、验证覆盖与未闭环项统一见[当前能力矩阵](../docs/当前能力矩阵.md)，本文件不再维护状态副本。

实现约束见[内存与同步模型](../docs/architecture/内存与同步模型.md)、[Vulkan 后端实现](../docs/architecture/Vulkan后端实现.md)和[CUVID 到 VAAPI 映射表](../docs/architecture/CUVID到VAAPI映射表.md)；历史验证记录见 [evidence](../docs/evidence/README.md)。本目录测试是可行性验证，不是生产验收。
