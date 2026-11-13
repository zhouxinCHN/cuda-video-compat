# Vulkan Video 后端实现与移植指南

> 版本：v1.3
> 文档日期：2026-11-13
> 版本说明：v1.3 增加 `h264_nvdec` 参数集合成、显式失败和双后端宿主 32/32 结果；v1.2 标注后端同级及修复前基线。
> 状态：**已在 AMD 验证机跑通**（RX 7900 XTX / RADV，VCN < 5）。四路码流（9/13/30/30 帧）在 Vulkan 与 VAAPI 两个后端均逐帧逐平面位精确；FFmpeg 宿主双后端矩阵 32/32，Vulkan `h264_nvdec` 通过从 CUVID 字段合成 SPS/PPS 接入，零拷贝需额外 surface。
> 职责：本文是 Vulkan 后端的实现、接缝、根因、验证与移植指南的唯一权威文档。能力评估与编码风险见 [Vulkan Video 方案评估](Vulkan_Video方案评估.md)；内存/同步语义见[内存与同步模型](内存与同步模型.md)；宿主结果见 [FFmpeg 宿主双后端基线](../evidence/FFmpeg宿主双后端基线_2026-11-13/README.md)。
> 环境注意：验证脚本统一显式设置 `RADV_PERFTEST=video_decode`。已有探测显示部分 Mesa 版本默认暴露视频队列族，但尚未做同版本 A/B 对照；在复测前保留显式设置作为保守做法。

---

## 1. 一句话架构

```text
FFmpeg (h264_cuvid / h264_nvdec)
    │  dlopen + dlsym  libnvcuvid.so.1（符号不变）
    ▼
libnvcuvid.cpp ─── DecoderBackend 接口（backend_dispatch 按 CUVID_SHIM_BACKEND 选后端）
                     ├── VaapiBackend  → VaapiH264Decoder  (同级候选 A)
                     └── VulkanBackend → VulkanH264Decoder (同级候选 B)
```

`libnvcuvid.cpp` 只依赖 `DecoderBackend` 接口，后端无关。切后端只影响 `make_backend()`，不碰 CUVID 表面那一层（`cuvidCreateDecoder` / `cuvidDecodePicture` / `cuvidMapVideoFrame64` 等符号全部保留）。

## 2. 新增/改动文件

| 文件 | 作用 |
|---|---|
| `src/decoder_backend.h` | `DecoderBackend` 接口（initialize/decode/map_frame/coded_*/reset） |
| `src/backend_dispatch.h` / `.cpp` | `make_backend()`：读 `CUVID_SHIM_BACKEND`（`vk`/`vaapi`），默认 vaapi |
| `src/vulkan_h264_decoder.h` / `.cpp` | Vulkan Video 解码后端（核心新增） |
| `src/h264_direct_params.h` / `.cpp` | 从 `CUVIDH264PICPARAMS` 合成最小 Annex-B SPS/PPS（h264_nvdec 路径） |
| `src/vk_sequence_cache.h` | 进程级 SPS/PPS NAL 字节暂存（parser ↔ decoder 的接缝，前置 start code） |
| `src/hip_frame_mapper.h` / `.cpp` | 新增 `map_fd()`：直接导入已导出的 dma-buf fd |
| `src/cuvid_parser.cpp` / `.h` | 加 SPS/PPS stash + 双就绪后 `VkSequenceCache::store()` |
| `src/libnvcuvid.cpp` | 改用 `DecoderBackend` |

`VaapiH264Decoder` 本身不改。

## 3. 关键设计决策

### 3.1 SPS/PPS 从哪来

Vulkan Video 需要 SPS/PPS 在创建 session / session parameters 时一次性给出，但两条 FFmpeg 宿主路径提供信息的方式不同：

- **`h264_cuvid`**：`CuvidParser` 能看到 SPS/PPS。它把原始 NAL（保留 EPB）写入进程级单槽 `VkSequenceCache`，后端优先使用这份字节，profile/VUI/crop 保真。
- **`h264_nvdec`**：FFmpeg 自己解析参数集，只通过 `cuvidDecodePicture` 交出解析后的 `CUVIDH264PICPARAMS` 和 slice 数据，shim parser 完全不参与。`VkSequenceCache` 因此为空，旧实现会在 `decode()` 里返回成功但从未建立 session，map 时表现为 `CUDA_ERROR_INVALID_VALUE`。

现在 `h264_nvdec` 的兜底是 `h264_direct_params.cpp`：从 `CUVIDH264PICPARAMS` 构造一份最小 Annex-B SPS/PPS，存放在 decoder 实例内。后端先尝试原始 cache，失败后用合成参数集。合成字段限制见 §3.6。

- 进程级 cache 单槽只支撑 `h264_cuvid` 单流；多流仍需按 `CUvideodecoder` 键控（P1）。
- `h264_nvdec` 的合成参数集按 decoder 保存，不使用进程级单槽。
- 没有参数集时 `decode()` 必须返回明确错误，不能伪成功。

### 3.2 懒建 session

`cuvidCreateDecoder` 时 SPS/PPS 可能还没到（宿主先建 decoder，再喂流）。所以：

- `initialize()` 只校验参数 + 建 Vulkan instance/device/队列（不依赖 SPS/PPS）；
- `ensure_session()` 在 SPS/PPS 就绪后建 video session / session parameters / 图像池 / 导出 buffer / 同步对象；
- `decode()` 每次先准备参数集并调用 `ensure_session()`；`h264_nvdec` 会从当前 `CUVIDPICPARAMS` 合成参数集。仍无法建 session 时返回 `CUDA_ERROR_NOT_SUPPORTED`，不再静默成功。

这使后端对宿主的建 decoder→喂流顺序鲁棒。

### 3.3 复用 P0 已验证的硬核细节

`verify_vk_decode_to_hip.cpp`（已在 AMD 机上逐字节通过）里容易踩坑的点，全部原样搬入：

1. **双队列**：decode 队列族无 TRANSFER，copy 必须在 transfer 队列族；图像/缓冲 `CONCURRENT` 共享 + sync2 semaphore 桥接。
2. **`pSetupReferenceSlot` 每帧非 NULL**（含 B 帧，用临时 DPB slot，解码后回收）。
3. **DPB 分离**：DPB 图只带 `VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR`，不混 DST（RADV `radv_video.c`）。
4. **Annex-B + start code**：h264_cuvid 传入裸 NAL，需要补 `00 00 00 01`；h264_nvdec 的 `pBitstreamData` 已由 FFmpeg 组装为 Annex-B，不能再重复补。两条路径都要求当前 P0 单 slice，多 slice 显式拒绝。
5. **导出链路**：`vkCmdCopyImageToBuffer`（barrier 转 `TRANSFER_SRC_OPTIMAL`，逐平面 Y/UV）→ `vkGetMemoryFdKHR(OPAQUE_FD)` → HIP import。
6. **SPS/PPS → Std 转换**：`fill_sps_std` / `fill_pps_std`（含 scaling list 8x8 顺序 `{0,3,1,4,2,5}`、designated-initializer 字段序、`aspect_ratio_idc` cast）。
7. **主机同步**：`vkWaitForFences(copyFence)` 后再导出/HIP 读；异步 sync_file 桥接属并行项，不阻塞。

### 3.4 逐帧 slice 解析

`decode()` 收到的 `CUVIDPICPARAMS.pBitstreamData` 是单 slice NAL（FFmpeg nvdec 每次送一个 slice）。用 FFmpeg `ff_h2645_packet_split` 包住它 → 取 slice NAL → `parse_slice_header`（自写，P0 版移植）→ `ff_h264_init_poc` 算 POC → 填 `StdVideoDecodeH264PictureInfo`。

`CUVIDPICPARAMS` 里 SPS/PPS 派生字段（`log2_max_frame_num_minus4` 等）这里不依赖——两条来源最终都会重新 parse 成 `SPS*`/`PPS*`。原始 cache 保留完整参数集；合成路径只有 CUVID 可见字段。

### 3.5 map_frame 与 fd 所有权

`decode()` 末尾 `vkGetMemoryFdKHR` 导出 fd 存进 `exports_[PicIdx].fd`；`map_frame()` 把 fd 转移给 `HipFrameMapping::map_fd()`（`exp.fd = -1` 后移交，`HipFrameMapping::reset()` 负责 close）。fd 生命周期与 VAAPI 后端一致（mapping 析构即 close），无泄漏。

`HipFrameMapping::map_fd()` 是 Vulkan 专用入口：跳过 VAAPI 的 `VADRMPRIMESurfaceDescriptor`，直接导入已导出的 fd，并假定 Y/UV 连续（UV offset = pitch × height，正是 `vkCmdCopyImageToBuffer` 写出的布局）。

### 3.6 `h264_nvdec` 参数集合成与限制

`h264_direct_params.cpp` 写出的 SPS/PPS 由以下字段构成：

- SPS：profile（按 CABAC/8x8/缩放矩阵推断）、level（按宏块规模推断）、参考帧数、frame_num/POC 位数、POC type 0/2、MB 尺寸、frame_mbs_only、direct8x8；无 crop、无 VUI。
- PPS：熵编码、POC present、默认参考数、加权预测、QP 偏移、deblocking/constrained/redundant、transform8x8；scaling matrix 必须为默认值。

显式限制：

1. 仅 4:2:0/8-bit、progressive、POC type 0/2；
2. SPS/PPS ID 固定 0；
3. 非默认缩放矩阵无法可靠还原，直接返回不支持；
4. crop、VUI、profile constraint、精确 level 不可恢复；
5. 单 slice；多 slice 当前返回不支持，不能只解第一片；
6. 序列变化后不会重建 session，`reconfigure` 仍是 stub。

这些限制写成代码中的显式失败，不允许静默输出错误帧。P0 四路码流已通过，更复杂的码流需扩展 ABI 旁路或宿主接口。

## 4. 明确不在本次交付范围

| # | 项 | 说明 |
|---|---|---|
| 1 | `cuvidReconfigureDecoder` | 当前 stub（与 VAAPI 后端一致），Vulkan 需销毁重建 session，未接 |
| 2 | 异步同步（sync_file 桥接） | 首期主机同步够用；异步桥接需 ROCm 协同，不阻塞 |
| 3 | 多流 / seek / 动态分辨率 | 单流单序列已覆盖；多流需按 decoder 键控 `VkSequenceCache` 与合成参数集 |
| 4 | HEVC/VP9/AV1 | 本次只有 H264（对齐 P0 范围）；Std 转换与 slice 解析按 codec 扩展 |
| 5 | transfer-only 队列族导出 | 现走 compute 队列族；SDMA 路径的多平面 pitch 另有上游疑点 |
| 6 | 多 slice / crop / VUI / POC type 1 / 非默认缩放矩阵 | 当前显式不支持；P0 四路单 slice 已通过 |

## 5. 接缝实现

`cuvid_parser.cpp` 的 `parse_packet_nal()` 里，SPS/PPS 两个 case 各做一次「stash + 双就绪则整体 store」：

```cpp
// NAL_SPS case（解析成功后）: 存原始字节（保留防竞争字节 0x03）
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

**字节语义（关键）**：

| 字段 | 含义 |
|---|---|
| `nal.data` / `nal.size` | 已去转义（EPB 已剥）的 RBSP，拷在 FFmpeg 的 rbsp buffer 里 |
| `nal.raw_data` / `nal.raw_size` | NAL header 起始、不含 start code、EPB 原样保留的原始字节 |

必须存 `raw_data`。存 `nal.data` 时，剥掉 0x03 之后可能残留 `00 00 01` 序列，再被 `ff_h2645_extract_rbsp` 当成下一个 start code 截断，SPS/PPS 解析就崩（现象：`Overread VUI by 8 bits` / `sps_id 0 out of range`）。`raw_data` 为空（无 EPB、`data == raw_data` 的快速路径）时回退到 `data`。

`VkSequenceCache::store()` 内部给 SPS/PPS 各前置 4 字节 start code（`00 00 00 01`），使 `ensure_session()` / `decode()` 里的 `ff_h2645_packet_split` 能直接重新切分识别。

## 6. 构建

`cuda_shim/Makefile`：

- `NVCUVID_SRC` 增加 `vulkan_h264_decoder.cpp backend_dispatch.cpp`；
- 编译加 `-I$(MESA_DIR)/include`（Vulkan 头，含 `vk_video/*` std 头）；
- 链接加 `-lvulkan`；
- FFmpeg 复用对象沿用 **9 个 `.o`**（`h2645_parse` / `h264_ps` / `h2645_vui` / `h2645data` / `h264data` / `h264_parse` / `golomb` / `mathtables` / `refstruct`）+ `libavutil`。

`vulkan_h264_decoder.cpp` 用到 `ff_h264_init_poc` / `ff_h264_parse_ref_count`，这两个符号在 `h264_parse.o` 里，`FF_OBJS` 已包含，无需新增。

## 7. 验证入口

```bash
cd cuda_shim
make libnvcuvid.so.1 verify_kernel_read

# 冒烟：建 VK decoder + map 单帧（无像素校验）
CUVID_SHIM_BACKEND=vk ./verify_cuvid_session

# 端到端：parser→decoder→map→HIP 读，逐帧 dump NV12 做像素比对
LD_LIBRARY_PATH=. CUVID_SHIM_BACKEND=vk RADV_PERFTEST=video_decode \
  CUVID_DUMP_NV12_DIR=/tmp/vkd ./verify_kernel_read ../testdata/h264/test_p_b_5.h264
```

- `verify_kernel_read` 的 `OK` / `Ysum` / `UVsum` 只验“kernel 读到的与 CPU 读到的是同一块内存”，**不校验画面**；画面正确性要用 `CUVID_DUMP_NV12_DIR` dump 出来，和软解参考逐帧逐平面比（注意 pitch 跨行 + NV12 交织）。
- 排障开关：`CUVID_SHIM_DEBUG_VK=1` 每帧打印解码参数；`CUVID_SHIM_NOCOPY=1` 只解码不导出（分离“解码参数问题”与“多队列同步/导出问题”）。
- `RADV_PERFTEST=video_decode` 在验证机脚本中显式设置；是否需要已存在版本差异，见文档头部的环境注意。

## 8. 上机验证结论（RX 7900 XTX / RADV）

### 8.1 根因：视频会话内存 use-after-free

违反的 Vulkan 规定：绑定到 `VkVideoSessionKHR` 的 `VkDeviceMemory` 必须一直有效到 `vkDestroyVideoSessionKHR`。原实现在 `vkBindVideoSessionMemoryKHR` 成功后立刻 `vkFreeMemory`。

为什么第 4 帧才炸：

- RADV 的 `radv_BindVideoSessionMemoryKHR` → `copy_bind()` 只把 `radv_device_memory*` 裸指针存进 `vid->sessionctx` / `vid->ctx` / `vid->intra_only_dpb`（`src/amd/vulkan/radv_video.c`），里面是 VCN 固件自己的 context buffer；
- 每帧解码 `RDECODE_CMD_CONTEXT_BUFFER` 都用 `vid->ctx.mem->bo` 的地址重新编程；
- 这块内存 free 之后回到 allocator，被后续分配（图像、bitstream buffer、query pool……）复用；前几帧碰巧还没被写坏，到第 4 帧固件上下文已被覆盖 → 固件跑飞 → VCN ring timeout → mode1 reset。

FFmpeg 的做法是把它放在 `common->mem[]` 里，只有 `ff_vk_video_common_uninit()`（session 销毁之后）才释放。

### 8.2 修复清单

| # | 修复 | 位置 |
|---|---|---|
| 1 | **会话内存所有权**：新增 `session_mems_`，bind 成功后转移所有权，`vkDestroyVideoSessionKHR` 之后才 `vkFreeMemory` | `vulkan_h264_decoder.{h,cpp}` `ensure_session()`/`reset()` |
| 2 | **DPB 图不带 `TRANSFER_DST`**：改为 `VIDEO_DECODE_DPB \| TRANSFER_SRC` | 同上 |
| 3 | **多平面导出的 texel 单位**：NV12 plane 1 的 texel 是 2 字节，`imageExtent`/`bufferRowLength`/`bufferImageHeight` 必须按 plane 1 的 texel 计（`{W/2,H/2,1}` / `pitch/2` / `H/2`） | `decode()` 导出拷贝 |
| 4 | **DPB 槽位升序分配**：旧 `free_dpb_slots_` 是 LIFO，第一帧就拿 slot 16，超出固件 16 项参考表；改为升序取最小空闲槽 | `vulkan_h264_decoder.cpp` |
| 5 | **barrier 覆盖 setup 槽与全部参考槽**：进解码前统一转到 `VIDEO_DECODE_DPB` 布局，`oldLayout` 用每个图像自己跟踪的 layout | 同上 |
| 6 | **SPS/PPS 存 `raw_data`**（保留 EPB），否则重新切分时 `00 00 01` 被误判为 start code | `cuvid_parser.cpp`（见 §5） |
| 7 | **可诊断性**：`VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR` 解码状态查询；`vkWaitForFences` 30s 超时 + 检查返回值，`VK_ERROR_DEVICE_LOST` 立即置 `device_lost_`；`CUVID_SHIM_DEBUG_VK=1` 每帧打印参数 | `vulkan_h264_decoder.{h,cpp}` |

### 8.3 验证结果

| 码流 | 帧数 | Vulkan | VAAPI |
|---|---:|---|---|
| `testdata/h264/test_p_b_5.h264` | 9 | 9/9 bit-exact，errors=0 | 9/9 bit-exact |
| `testdata/h264/test_p_b_7.h264` | 13 | 13/13 bit-exact，errors=0 | 13/13 bit-exact |
| `testdata/h264/test_p_main.h264` | 30 | 30/30 bit-exact，errors=0 | 30/30 bit-exact |
| `testdata/h264/test_p_66.h264` | 30 | 30/30 bit-exact，errors=0 | 30/30 bit-exact |

合计 82 帧/后端。早期“各 ≥30 帧”的验收口径是估算；实际以四路码流全部逐字节一致为准。

### 8.4 测量陷阱（重要教训）

1. **“色度 PSNR u/v ≈ 6 dB”是测量错误**：把 pitch 跨行的 NV12 当成 planar yuv420p 比。正确做法：亮度 `y*pitch`；色度从 `pitch*H + y*pitch` 开始，U/V 交织，参考需先交织或分别比 `dump[0::2]==U`、`dump[1::2]==V`。按此重测 Y/U/V 逐字节相同。
2. **“VAAPI 后端输出平坦 0x80”同样是测量错误**：按正确方法重测，VAAPI 四路码流同样逐帧位精确。
3. **`verify_kernel_read` 的 `OK/Ysum/UVsum` 不校验画面**：只证明 GPU kernel 与 CPU 读到同一块内存；`frames_ok>=10` 只代表流程完整。画面正确性必须 dump NV12 后与软解参考逐帧比。
4. **已排除的假设（避免重复尝试）**：H.264 语义层（slice/POC/ref_count 与 `trace_headers` 一致）、DPB 图未写、`used_for_reference_flags` 差异、`RADV_DEBUG=videoarraypath`、`CUVID_SHIM_EXP=dpbsrc`、SDMA 拷贝路径——均与 B 帧挂死无关。根因是 §8.1 的会话内存 UAF。
5. **SDMA 路径的上游疑点**：`radv_sdma_get_buf_surf()` 里 `pitch = bufferRowLength * texel_scale` 对多平面 plane 1 可能少算一倍。这是潜在上游 bug，与本次问题无关；若将来走 transfer-only 队列族需先绕开。

## 9. 移植指南（从 VAAPI 验证程序到 Vulkan 后端）

### 9.1 目标数据通路

```text
Vulkan 解码输出 image（OPTIMAL tiling，NV12）
    │  解码完成依赖（semaphore/fence）
    │  image layout 从 VIDEO_DECODE_DST → TRANSFER_SRC_OPTIMAL（barrier）
    │  vkCmdCopyImageToBuffer：分别拷贝 Y 平面、UV 平面
    ▼
可导出的 GPU buffer（约定 pitch、平面 offset 的线性 NV12）
    │  vkGetMemoryFdKHR → dma-buf fd
    │  hipImportExternalMemory → hipExternalMemoryGetMappedBuffer
    ▼
HIP 设备指针：等拷贝完成后，HIP 读回逐字节校验（vs 软解参考）
```

验收标准：

1. HIP 读回的 NV12 像素与软件解码参考逐字节一致。
2. 覆盖 I-only / I/P / I+P+B 三类码流；实际以 `testdata/h264/` 四路码流全部通过为准。
3. 无 fd 泄漏、无显存泄漏（结束前后 fd 计数一致）。

### 9.2 拷贝链路

使用 `vkCmdCopyImageToBuffer`，不要自写 compute shader。源 image 用 OPTIMAL tiling 完全没问题，前提是：

1. image 创建时带 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`；
2. 拷贝前用 `VkImageMemoryBarrier` 转到 `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`；
3. 用 `VkBufferImageCopy` 按 plane 分别设复制区域，正确处理 pitch/offset；
4. 拷贝完成后用 semaphore/fence 把“拷贝完成”信号传给 HIP 侧。

参考实现：FFmpeg `hwcontext_vulkan.c` → `vulkan_transfer_frame()`；GStreamer `vkdownload.c`；AMD `vulkan_interop` 样例。

可导出 buffer：查询支持导出 fd 的 memory type；句柄类型 `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`；HIP 侧 `hipExternalMemoryHandleTypeOpaqueFd`。

### 9.3 同步链路

| 阶段 | 方案 | 说明 |
|---|---|---|
| 首期（功能正确优先） | 主机同步 | Vulkan 侧 fence wait / `vkQueueWaitIdle`；HIP 读前 `hipDeviceSynchronize` |
| 进阶（性能优化） | 异步桥接 | Vulkan 完成信号直接驱动 HIP 队列等待，无 CPU 介入 |

异步桥接正确路线：

```text
VkFence 完成
  → VK_KHR_external_fence_fd：vkGetFenceFdKHR 导出 SYNC_FD
  → sync_file fd
  → DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE → amdgpu syncobj
  → ROCm/ROCr 把 syncobj 挂成 HIP queue 的 wait fence
```

改动归属：amdgpu 内核 syncobj 已支持；Mesa/RADV 导出 SYNC_FD 基本不用改；**ROCm/ROCr 用户态是主要缺口**，属厂商协同项。首期不阻塞。

### 9.4 从 VAAPI 版改造的改动清单

核心原则：`verify_decode_to_hip.cpp` 可拆成“后端无关”和“VAAPI 专属”两半。

**原样保留**：FFmpeg parser 复用头、`read_file()`、`parse_slice_header()`、`count_fds()`、NAL 切分、软解参考读取、显示序重排与逐字节比对、flush/fd 检查。

**逐段替换**：

| VAAPI 实现 | 替换为 |
|---|---|
| `vaGetDisplayDRM` / `vaInitialize` | Vulkan instance/device 创建，启用 `VK_KHR_video_queue`、`VK_KHR_video_decode_h264`、`VK_KHR_external_memory_fd` |
| `vaCreateSurfaces`（LINEAR pool） | `VkVideoSessionKHR` + DPB/output `VkImage` 池（OPTIMAL，带 `TRANSFER_SRC`） |
| `vaCreateConfig/Context` | 无直接对应；VK 是 session + 每帧 `vkCmdDecodeVideoKHR` |
| 手写 DPB 状态机 + RefPicList | 驱动内部管理 DPB；应用只分配 DPB slot 索引并填 `VkVideoReferenceSlotInfoKHR` / `StdVideoDecodeH264PictureInfo` |
| `VAPictureParameterBufferH264` 等 | `StdVideoDecodeH264PictureInfo` + session 创建时的 `StdVideoH264SequenceParameterSet` / `PictureParameterSet` |
| `vaBegin/Render/EndPicture` + `vaSyncSurface` | `vkCmdBeginVideoCodingScopeKHR` → `vkCmdDecodeVideoKHR` → `vkCmdEndVideoCodingScopeKHR` + fence |
| `vaExportSurfaceHandle` | `vkCmdCopyImageToBuffer` → `vkGetMemoryFdKHR` |
| HIP 导入 + 逐行读 | 基本复用，fd 来源换成 `vkGetMemoryFdKHR` |

**DPB 关键差异**：

- VAAPI VLD：应用手动管理参考帧，手写 DPB 状态机、`ReferenceFrames[]`、`RefPicList0/1[]`、显示序重排。
- Vulkan Video：驱动内部管理 DPB。应用只需为每帧分配 DPB slot，并通过 `StdVideoDecodeH264PictureInfo` 告诉驱动参考关系；硬件按 SPS/PPS + 参考信息自行维护。VAAPI 版里大部分 DPB/RefPicList 代码可以删掉，但 `parse_slice_header` 和 POC 计算仍要保留，用来填 Std 结构。

### 9.5 可复用资源

| 路径 | 内容 |
|---|---|
| `tests/vulkan-video/full/verify_vulkan_video_full.cpp` | V1~V6 能力探测 |
| `tests/zerocopy/verify_decode_to_hip.cpp` + `.make` | VAAPI 版端到端结构 |
| `tests/zerocopy/hip_read_surface.cpp` | HIP 导入 fd → 读 Y/UV → 写文件 |
| `tests/zerocopy/verify_modifier_linear.cpp` | VAAPI linear 创建、pitch 256 对齐、UV offset 约定 |
| `testdata/h264/` | 四路验证码流 |

软解参考生成：`ffmpeg -i <stream> -f rawvideo -pix_fmt nv12 ref.yuv`（按实际宽高读取）。

### 9.6 不要做的事

1. 不要试图让 Vulkan 解码输出 LINEAR——规范锁死，改 Mesa 是永久 fork。
2. 不要改 `hipImportExternalMemory` 去“读 tiled”——tiling 在硬件寻址层，改接口无效。
3. 不要在 opaque semaphore 上花时间——跨栈不互认，死胡同，改走 sync_file 桥接。
4. 不要把 `verify_kernel_read` 的 `OK/Ysum` 当成画面正确性证据。

## 10. 相关源码线索

- `cuda_shim/src/vulkan_h264_decoder.cpp`：`ensure_session()`（会话内存所有权）、`decode()`（参数集来源、参考槽、双队列、导出拷贝）、`parse_slice_header()`。
- `cuda_shim/src/vulkan_h264_decoder.h`：`session_mems_`、`dpb_images_`、`free_dpb_slots_`、`outputs_`、`exports_`、`direct_sps_/direct_pps_`、`device_lost_`、`decode_query_`。
- `cuda_shim/src/h264_direct_params.cpp`：`CUVIDH264PICPARAMS` → 最小 Annex-B SPS/PPS；不支持的字段组合显式返回 false。
- `cuda_shim/src/vaapi_h264_decoder.cpp`：VAAPI 后端（同样位精确）。
- `cuda_shim/tests/verify_kernel_read.cpp`：`CUVID_DUMP_NV12_DIR` 支持点。
- Mesa（只读）：`src/amd/vulkan/radv_video.c`、`src/amd/vulkan/meta/radv_meta_copy.c`、`src/amd/common/ac_vcn_dec.h`。
- FFmpeg 7.1（只读）：`libavcodec/vulkan_h264.c`、`libavcodec/vulkan_decode.c`、`libavutil/hwcontext_vulkan.c`、`libavutil/vulkan.c`。
