# Vulkan Video 后端（CUVID shim 基础架构）

> 状态：**已在 AMD 验证机跑通**（RX 7900 XTX / RADV，VCN < 5），四路 P/B 码流解码结果
> 逐帧逐平面位精确。B 帧挂死（VCN ring timeout）的根因与修复见 §8，调试过程见
> [B 帧挂死调试留档](../verification/vulkan-bframe-hang-debug-log.md)。仍需 `RADV_PERFTEST=video_decode`。
>
> 下文 §1–§7 是交付时的架构说明（保留原貌）；§8 是上机验证后的结论与修复清单。
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

- **代价**：`cuvid_parser.cpp` 需要加两段 `store()`（见 §5）。因为 parser 与 decoder 之间
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
| 1 | **编译/运行验证** | ~~本机（NVIDIA RTX 4060）无 AMD GPU，无法链接运行~~ → **已在 AMD 验证机完成**，见 §8 |
| 2 | `cuvidReconfigureDecoder` | 当前 stub（与 VAAPI 后端一致），Vulkan 需销毁重建 session，未接 |
| 3 | 异步同步（sync_file 桥接） | 首期主机同步够用，异步桥接需 ROCm 协同（TODO §4.2），不阻塞 |
| 4 | 多流 / seek / flush | 单流单序列已覆盖；flush（`payload_size==0`）走 parser 的 `pump_display_queue(0, true)`（按 POC 吐空重排序队列），VK 后端不感知 |
| 5 | HEVC/VP9/AV1 | 本次只有 H264（对齐 P0 范围）；Std 转换与 slice 解析按 codec 扩展 |
| 6 | transfer-only 队列族导出 | 现走 compute 队列族；SDMA 路径的多平面 pitch 另有上游疑点，见调试留档 §3 |

---

## 5. 接缝实现（已完成）

`cuvid_parser.cpp` 的 `parse_packet_nal()` 里，SPS/PPS 两个 case 各做一次「stash + 双就绪则整体 store」：

```cpp
// NAL_SPS case（解析成功后）: 存**原始字节**（保留防竞争字节 0x03）
{
    const uint8_t *src = nal.raw_data ? nal.raw_data : nal.data;
    const std::size_t n = nal.raw_data ? static_cast<std::size_t>(nal.raw_size)
                                       : static_cast<std::size_t>(nal.size);
    pending_sps_.assign(src, src + n);
}
if (!pending_sps_.empty() && !pending_pps_.empty())
    VkSequenceCache::store(pending_sps_.data(), pending_sps_.size(),
                           pending_pps_.data(), pending_pps_.size());
// NAL_PPS case 对称。
```

**字节语义（关键，容易踩）**：

| 字段 | 含义 |
|---|---|
| `nal.data` / `nal.size` | **已去转义（EPB 已剥）**的 RBSP，拷在 FFmpeg 的 rbsp buffer 里 |
| `nal.raw_data` / `nal.raw_size` | NAL header 起始、**不含 start code**、**EPB 原样保留**的原始字节 |

必须存 `raw_data`。存 `nal.data` 时，剥掉 0x03 之后可能残留 `00 00 01` 序列，再被
`ff_h2645_extract_rbsp` 当成下一个 start code 截断，SPS/PPS 解析就崩
（现象：`Overread VUI by 8 bits` / `sps_id 0 out of range`）。
`raw_data` 为空（无 EPB、`data == raw_data` 的快速路径）时回退到 `data` 即可。

`VkSequenceCache::store()` 内部给 SPS/PPS 各**前置 4 字节 start code**
（`00 00 00 01`），使 `VulkanH264Decoder::ensure_session()`/`decode()` 里的
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

## 7. 验证入口（AMD 机上已跑通）

```bash
cd cuda_shim
make libnvcuvid.so.1 verify_kernel_read

# 冒烟：建 VK decoder + map 单帧（无像素校验）
CUVID_SHIM_BACKEND=vk ./verify_cuvid_session

# 端到端：parser→decoder→map→HIP 读，逐帧 dump NV12 做像素比对
RADV_PERFTEST=video_decode CUVID_SHIM_BACKEND=vk \
  CUVID_DUMP_NV12_DIR=/tmp/vkd LD_LIBRARY_PATH=. \
  ./verify_kernel_read ../testdata/h264/test_p_b.h264
```

- `RADV_PERFTEST=video_decode` **必开**，否则 RADV 不暴露 `VK_KHR_video_queue/decode/h264_decode`
  与 video-decode 队列族，`vkCreateInstance` 直接失败（日志 `FAIL: no video decode queue family` / 错误 801）。
- `verify_kernel_read` 的 `OK`/`Ysum`/`UVsum` 只验"kernel 读到的与 CPU 读到的是同一块内存"，
  **不校验画面**；画面正确性要用 `CUVID_DUMP_NV12_DIR` dump 出来，和软解参考逐帧逐平面比
  （注意 pitch 跨行 + NV12 交织），命令见
  [B 帧挂死调试留档 §5](../verification/vulkan-bframe-hang-debug-log.md#5-验证命令可复现)。
- 排障开关：`CUVID_SHIM_DEBUG_VK=1` 每帧打印解码参数；`CUVID_SHIM_NOCOPY=1` 只解码不导出
  （用来分离"解码参数问题"与"多队列同步/导出问题"）。

---

## 8. 上机验证结论（RX 7900 XTX / RADV）

### 8.1 根因：视频会话内存 use-after-free

**违反的 Vulkan 规定**：绑定到 `VkVideoSessionKHR` 的 `VkDeviceMemory` 必须一直有效到
`vkDestroyVideoSessionKHR`。原实现在 `vkBindVideoSessionMemoryKHR` 成功后**立刻**
`vkFreeMemory`（当时的想法是"bind 完就没用了"）。

为什么偏偏第 4 帧才炸：

- RADV 的 `radv_BindVideoSessionMemoryKHR` → `copy_bind()` 只把 `radv_device_memory*`
  **裸指针**存进 `vid->sessionctx` / `vid->ctx` / `vid->intra_only_dpb`
  （`src/amd/vulkan/radv_video.c`），里面是 **VCN 固件自己的 context buffer**；
- 每帧解码 `RDECODE_CMD_CONTEXT_BUFFER` 都用 `vid->ctx.mem->bo` 的地址重新编程；
- 这块内存 free 之后回到 allocator，被后续分配（图像、bitstream buffer、query pool……）复用；
  前几帧碰巧还没被写坏，到第 4 帧固件上下文已被覆盖 → 固件跑飞 → VCN ring timeout → mode1 reset。

FFmpeg 的做法是把它放在 `common->mem[]` 里，只有 `ff_vk_video_common_uninit()`
（session 销毁之后）才释放。

### 8.2 修复清单

| # | 修复 | 位置 |
|---|---|---|
| 1 | **会话内存所有权**：新增 `session_mems_`，bind 成功后转移所有权，`vkDestroyVideoSessionKHR` 之后才 `vkFreeMemory`（§8.1 的根因） | `vulkan_h264_decoder.{h,cpp}` `ensure_session()`/`reset()` |
| 2 | **DPB 图不带 `TRANSFER_DST`**：改为 `VIDEO_DECODE_DPB \| TRANSFER_SRC`（RADV 对 VCN < 5 有"DPB 与 decode 目标分开分配"的约束；导出拷贝只需要 SRC） | 同上 |
| 3 | **多平面导出的 texel 单位**：NV12 plane 1 的 texel 是 2 字节，`imageExtent`/`bufferRowLength`/`bufferImageHeight` 必须按 plane 1 的 texel 计（`{W/2,H/2,1}` / `pitch/2` / `H/2`）；旧代码把整行 `pitch`（字节）当 texel 传，radv 按超宽范围搬 → 色度错位。参考 FFmpeg `hwcontext_vulkan.c:get_plane_buf()` 的 `bufferRowLength /= desc->comp[i].step` | `decode()` 导出拷贝 |
| 4 | **DPB 槽位升序分配**：旧 `free_dpb_slots_` 是 LIFO，第一帧就拿 slot 16，而 `rvcn_dec_message_avc_t::ref_frame_list[]` 只有 16 项、TIER_3 槽位掩码 16 位；改为升序取最小空闲槽，编号落在 `0..N-1` | `vulkan_h264_decoder.cpp` |
| 5 | **barrier 覆盖 setup 槽与全部参考槽**：进解码前统一转到 `VIDEO_DECODE_DPB` 布局，`oldLayout` 用每个图像自己跟踪的 `layout`，消除"上一帧留成 `TRANSFER_SRC_OPTIMAL`"的隐式未定义行为 | 同上 |
| 6 | **SPS/PPS 存 `raw_data`**（保留 EPB），否则重新切分时 `00 00 01` 被误判为 start code，解析崩 | `cuvid_parser.cpp`（见 §5） |
| 7 | **可诊断性（保留在代码里）**：`VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR` 解码状态查询（受 `queryResultStatusSupport` 门控）；`vkWaitForFences` 30s 超时 + 检查返回值，`VK_ERROR_DEVICE_LOST` 立即置 `device_lost_` 停止提交；`CUVID_SHIM_DEBUG_VK=1` 每帧打印 `#N type= poc= fn= ref= rc0= rc1= setup_slot= refs= dst_idx= dst_img= setup_img= free={...}` | `vulkan_h264_decoder.{h,cpp}` |

### 8.3 验证结果

| 码流 | 帧数 | Vulkan | VAAPI |
|---|---|---|---|
| `test_p_b_5.h264` | 9 | 9/9 bit-exact，errors=0 | 9/9 bit-exact |
| `test_p_b_7.h264` | 13 | 13/13 bit-exact，errors=0 | 13/13 bit-exact |
| `test_p_main.h264` | 30 | 30/30 bit-exact，errors=0 | 30/30 bit-exact |
| `test_p_66.h264` | 30 | 30/30 bit-exact，errors=0 | 30/30 bit-exact |

> 早期文档里"VAAPI 后端输出平坦 0x80"是**测量方法错误**（把 pitch 跨行的 NV12 当 planar
> 去比），VAAPI 后端同样逐帧位精确。同理"色度 PSNR u/v ≈ 6 dB"也是测量问题。
> 细节见 [调试留档 §2](../verification/vulkan-bframe-hang-debug-log.md#2-重要教训两个输出错误其实是测量方法错了)。
