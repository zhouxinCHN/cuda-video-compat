# Vulkan Video 后端（CUVID shim 基础架构）

> 状态：**代码基础架构完成，未编译未跑**（待 AMD 验证机）。
> 目标：在 CUVID shim（`cuda_shim/`）里为 `libnvcuvid.so.1` 增加一个 **Vulkan Video 解码后端**，
> 作为 VAAPI 主线的 **Tier-2 备选**，并让 FFmpeg 宿主（`h264_cuvid` / `h264_nvdec`）**源码不改、不重编**
> 就能通过环境变量切到这条后端。

---

## 1. 一句话架构

```
FFmpeg (h264_cuvid / h264_nvdec)
    │  dlopen + dlsym  libnvcuvid.so.1（符号不变）
    ▼
libnvcuvid.cpp  ───  DecoderBackend 接口（backend_dispatch 按 CUVID_SHIM_BACKEND 选后端）
                       ├── VaapiBackend   → VaapiH264Decoder   (Tier-1, 默认)
                       └── VulkanBackend  → VulkanH264Decoder  (Tier-2, 新增)
```

`libnvcuvid.cpp` 只依赖 `DecoderBackend` 接口，**后端无关**。切后端只影响 `make_backend()`，
不碰 CUVID 表面那一层（`cuvidCreateDecoder` / `cuvidDecodePicture` / `cuvidMapVideoFrame64` 等符号全部保留）。

---

## 2. 新增/改动文件

| 文件 | 作用 |
|---|---|
| `src/decoder_backend.h` | `DecoderBackend` 接口（initialize/decode/map_frame/coded_*/reset） |
| `src/backend_dispatch.h` / `.cpp` | `make_backend()`：读 `CUVID_SHIM_BACKEND`（`vk`/`vaapi`），默认 vaapi |
| `src/vulkan_h264_decoder.h` / `.cpp` | Vulkan Video 解码后端（核心新增） |
| `src/vk_sequence_cache.h` | 进程级 SPS/PPS NAL 字节暂存（parser ↔ decoder 的接缝，前置 start code） |
| `src/hip_frame_mapper.h` / `.cpp` | 新增 `map_fd()`：直接导入已导出的 dma-buf fd（Vulkan 路径无 VAAPI descriptor） |
| `src/cuvid_parser.cpp` / `.h` | 加 SPS/PPS stash + 双就绪后 `VkSequenceCache::store()`（接缝写入侧） |
| `src/libnvcuvid.cpp` | 改用 `DecoderBackend`（去掉对 `VaapiH264Decoder` 的直接依赖） |

`VaapiH264Decoder` 本身**不改**。

---

## 3. 关键设计决策

### 3.1 SPS/PPS 从哪来（与 VAAPI 后端的本质差异）

VAAPI VLD 的 SPS/PPS 由 `cuvid_parser.cpp` 里的 `CuvidParser` 解析后**填进每帧的
`CUVIDPICPARAMS.CodecSpecific.h264`**，逐帧送到 `cuvidDecodePicture`。而 Vulkan Video 需要
SPS/PPS **在创建 session / session parameters 时一次性给出**，但 CUVID 宿主（FFmpeg nvdec/cuvid）
只把 **slice NAL** 喂给 `cuvidDecodePicture`——SPS/PPS 根本不经过这个接口。

**解法**：`VkSequenceCache`（进程级单槽缓存）。`CuvidParser` 看到 SPS/PPS 时 `store()`；
`VulkanH264Decoder` 在 `ensure_session()` / `decode()` 里 `fetch()`。

- **代价**：`cuvid_parser.cpp` 需要加两行 `store()`（见 §5 待办）。因为 parser 与 decoder 之间
  唯一通道是宿主的 `pfnSequenceCallback`，无直接句柄，进程级缓存是最小侵入的接缝。
- **局限**：单槽只支撑单流单解码器（FFmpeg 单实例场景够用）。多流需按 `CUvideodecoder` 键控，
  留到 P1-2。

### 3.2 懒建 session

`cuvidCreateDecoder` 时 SPS/PPS 可能还没到（宿主先建 decoder，再喂流）。所以：
- `initialize()` 只校验参数 + 建 Vulkan instance/device/队列（这些不依赖 SPS/PPS）；
- `ensure_session()` 在 SPS/PPS 就绪后建 video session / session parameters / 图像池 / 导出 buffer / 同步对象；
- `decode()` 每次先 `ensure_session()`，若 SPS/PPS 仍未到则**返回成功并静默跳过**（等 parser 喂流）。

这使后端对宿主的建 decoder→喂流顺序**鲁棒**。

### 3.3 复用 P0 已验证的硬核细节（原样搬）

`verify_vk_decode_to_hip.cpp`（P0，已在 AMD 机上逐字节通过）里那些容易踩坑的点，全部原样搬入：

1. **双队列**：decode 队列族无 TRANSFER，copy 必须在 transfer 队列族；图像/缓冲 `CONCURRENT` 共享 + sync2 semaphore 桥接。
2. **`pSetupReferenceSlot` 每帧非 NULL**（含 B 帧，用临时 DPB slot，解码后回收）。
3. **DPB 分离**：DPB 图只带 `VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR`，不混 DST（RADV radv_video.c:1184）。
4. **Annex-B + start code**：码流 buffer 前补 `00 00 00 01`，`pSliceOffsets[0]=0`。
5. **导出链路**：`vkCmdCopyImageToBuffer`（barrier 转 `TRANSFER_SRC_OPTIMAL`，逐平面 Y/UV）→
   `vkGetMemoryFdKHR`(OPAQUE_FD) → HIP import。
6. **SPS/PPS → Std 转换**：`fill_sps_std`/`fill_pps_std`（含 scaling list 8x8 顺序 `{0,3,1,4,2,5}`、
   designated-initializer 字段序、`aspect_ratio_idc` cast）。
7. **主机同步**：`vkWaitForFences(copyFence)` 后再导出/HIP 读（异步 sync_file 桥接属并行项，不阻塞）。

### 3.4 逐帧 slice 解析

`decode()` 收到的 `CUVIDPICPARAMS.pBitstreamData` 是**单 slice NAL**（FFmpeg nvdec 每次送一个 slice）。
用 FFmpeg `ff_h2645_packet_split` 包住它 → 取 slice NAL → `parse_slice_header`（自写，P0 版移植）→
`ff_h264_init_poc` 算 POC → 填 `StdVideoDecodeH264PictureInfo`。

`CUVIDPICPARAMS` 里 SPS/PPS 派生字段（`log2_max_frame_num_minus4` 等）这里**不依赖**——直接从
`VkSequenceCache` 重新 parse SPS/PPS 拿 `SPS*`/`PPS*`（单流下便宜、无状态漂移）。

### 3.5 map_frame 与 fd 所有权

`decode()` 末尾 `vkGetMemoryFdKHR` 导出 fd 存进 `exports_[PicIdx].fd`；
`map_frame()` 把 fd **转移**给 `HipFrameMapping::map_fd()`（`exp.fd = -1` 后移交，`HipFrameMapping::reset()`
负责 close）。这样 fd 生命周期与 VAAPI 后端一致（mapping 析构即 close），无泄漏。

`HipFrameMapping::map_fd()` 是新增的 Vulkan 专用入口：跳过 VAAPI 的 `VADRMPRIMESurfaceDescriptor`，
直接导入一个已导出的 fd，并假定 Y/UV 连续（UV offset = pitch × height，正是 P0 `vkCmdCopyImageToBuffer`
写出的布局）。

---

## 4. 尚未完成（明确不在本次交付范围）

| # | 项 | 说明 |
|---|---|---|
| 1 | **编译/运行验证** | 本机（NVIDIA RTX 4060）无 AMD GPU + 无 libvulkan video 命令，无法链接运行；已做 `-fsyntax-only` 语法自检（0 error） |
| 2 | `cuvidReconfigureDecoder` | 当前 stub（与 VAAPI 后端一致），Vulkan 需销毁重建 session，未接 |
| 3 | 异步同步（sync_file 桥接） | 首期主机同步够用，异步桥接需 ROCm 协同（TODO §4.2），不阻塞 |
| 4 | 多流 / seek / flush | 单流单序列已覆盖；flush（`payload_size==0`）当前走 parser 的 `pump_display_queue`，VK 后端不感知，未专项测 |
| 5 | HEVC/VP9/AV1 | 本次只有 H264（对齐 P0 范围）；Std 转换与 slice 解析按 codec 扩展 |

---

## 5. 接缝实现（已完成）

`cuvid_parser.cpp` 的 `parse_packet_nal()` 里，SPS/PPS 两个 case 各做一次「stash + 双就绪则整体 store」：

```cpp
// NAL_SPS case（解析成功后）:
pending_sps_.assign(nal.data, nal.data + nal.size);
if (!pending_sps_.empty() && !pending_pps_.empty())
    VkSequenceCache::store(pending_sps_.data(), pending_sps_.size(),
                           pending_pps_.data(), pending_pps_.size());
// NAL_PPS case 对称。
```

**字节语义（关键）**：`nal.data` 是 **NAL header 起始、无 start code** 的（已去转义）字节，
`nal.raw_data` 是含 start code 的原始字节。`VkSequenceCache::store()` 内部给 SPS/PPS 各**前置
4 字节 start code**（`00 00 00 01`），使 `VulkanH264Decoder::ensure_session()`/`decode()` 里的
`ff_h2645_packet_split` 能直接重新切分识别——与 P0 码流 buffer 的写法一致。

---

## 6. 构建

`cuda_shim/Makefile` 需把新源加入 `libnvcuvid.so.1`（见 Makefile 改动）：

- `NVCUVID_SRC` 增加 `vulkan_h264_decoder.cpp backend_dispatch.cpp`；
- 编译加 `-I$(MESA)/include`（Vulkan 头，含 `vk_video/*` std 头）；
- 链接加 `-lvulkan`；
- FFmpeg 复用对象沿用现有 9 个 .o（`h264_ps`/`h2645_parse`/…/`refstruct`）+ libavutil。

> 注意：`vulkan_h264_decoder.cpp` 用了 `ff_h264_init_poc` / `ff_h264_parse_ref_count`，
> 这两个符号在 `h264_parse.o` 里（现有 Makefile 的 `FF_OBJS` 已含 `h264_parse.o`），无需新增。

---

## 7. 验证入口（待 AMD 机）

```bash
cd cuda_shim
make libnvcuvid.so.1
CUVID_SHIM_BACKEND=vk ./verify_cuvid_session          # 建 VK decoder + map 单帧（无像素校验）
# 端到端像素校验建议新增一个 test，喂 test_320x240.h264 走 parser→decoder→map，对比软解参考
```

`verify_cuvid_session` 现有代码不变（走同一 CUVID 表面），把 `CUVID_SHIM_BACKEND=vk` 传进去即可切后端。
