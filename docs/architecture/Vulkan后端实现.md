# Vulkan Video 后端实现与移植指南

> 版本：v1.4
> 文档日期：2026-11-19
> 版本说明：v1.4 增加能力查询/解码流水线（§3.7、§8.5）和 LINEAR 直出探针记录（§8.6），修正“解码输出只能 OPTIMAL”的旧结论；v1.3 增加 `h264_nvdec` 参数集合成、显式失败和双后端宿主 32/32 结果；v1.2 标注后端同级及修复前基线。
> 当前实现、验证覆盖与未闭环项统一见[当前能力矩阵](../当前能力矩阵.md)。本文只维护 Vulkan 实现、接缝、历史根因与移植指南，不维护阶段状态副本。
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
| `src/vk_sequence_cache.h` | 每线程 SPS/PPS NAL 字节暂存（parser ↔ decoder 的接缝，前置 start code） |
| `src/hip_frame_mapper.h` / `.cpp` | 新增 `map_fd()`：直接导入已导出的 dma-buf fd |
| `src/cuvid_parser.cpp` / `.h` | 加 SPS/PPS stash + 双就绪后 `VkSequenceCache::store()` |
| `src/libnvcuvid.cpp` | 改用 `DecoderBackend` |

`VaapiH264Decoder` 本身不改。

## 3. 关键设计决策

### 3.1 SPS/PPS 从哪来

Vulkan Video 需要 SPS/PPS 在创建 session / session parameters 时一次性给出，但两条 FFmpeg 宿主路径提供信息的方式不同：

- **`h264_cuvid`**：`CuvidParser` 能看到 SPS/PPS。它把原始 NAL（保留 EPB）写入每线程单槽 `VkSequenceCache`，后端优先使用这份字节，profile/VUI/crop 保真。
- **`h264_nvdec`**：FFmpeg 自己解析参数集，只通过 `cuvidDecodePicture` 交出解析后的 `CUVIDH264PICPARAMS` 和 slice 数据，shim parser 完全不参与。`VkSequenceCache` 因此为空，旧实现会在 `decode()` 里返回成功但从未建立 session，map 时表现为 `CUDA_ERROR_INVALID_VALUE`。

现在 `h264_nvdec` 的兜底是 `h264_direct_params.cpp`：从 `CUVIDH264PICPARAMS` 构造一份最小 Annex-B SPS/PPS，存放在 decoder 实例内。后端先尝试原始 cache，失败后用合成参数集。合成字段限制见 §3.6。

- cache 为每线程单槽：FFmpeg 一路解码器一个线程，单进程 8 路并发已验证；同一线程内交错驱动多个 decoder 仍不支持，需要按 decoder 键控（P1）。
- `h264_nvdec` 的合成参数集按 decoder 保存，不使用该单槽。
- 没有参数集时 `decode()` 必须返回明确错误，不能伪成功。

### 3.2 懒建 session

`cuvidCreateDecoder` 时 SPS/PPS 可能还没到（宿主先建 decoder，再喂流）。所以：

- `initialize()` 只校验参数 + 建 Vulkan instance/device/队列（不依赖 SPS/PPS）；
- `ensure_session()` 在 SPS/PPS 就绪后建 video session / session parameters / 图像池 / 导出 buffer / 同步对象；
- `decode()` 每次先准备参数集并调用 `ensure_session()`；`h264_nvdec` 会从当前 `CUVIDPICPARAMS` 合成参数集。仍无法建 session 时返回 `CUDA_ERROR_NOT_SUPPORTED`，不再静默成功。

这使后端对宿主的建 decoder→喂流顺序鲁棒。

### 3.3 复用 P0 已验证的硬核细节

`verify_vk_decode_to_hip.cpp`（已在 AMD 机上逐字节通过）里容易踩坑的点，全部原样搬入：

1. **双队列**：decode 队列族无 TRANSFER，copy 必须在 transfer 队列族；图像/缓冲 `CONCURRENT` 共享 + sync2 semaphore 桥接。decode 队列族自己能 copy 时（`cross_queue_ == false`）只用一个队列族，图像用 `EXCLUSIVE`，不再需要跨队列 semaphore。
2. **`pSetupReferenceSlot` 每帧非 NULL**（含 B 帧，用临时 DPB slot，解码后回收）。
3. **DPB 分离**：DPB 图只带 `VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR`，不混 DST（RADV `radv_video.c`）。
4. **Annex-B + start code**：h264_cuvid 传入裸 NAL，需要补 `00 00 00 01`；h264_nvdec 的 `pBitstreamData` 已由 FFmpeg 组装为 Annex-B，不能再重复补。多 slice access unit 必须完整上传并提交全部 slice offset，不能只解第一片。
5. **导出链路**：`vkCmdCopyImageToBuffer`（barrier 转 `TRANSFER_SRC_OPTIMAL`，逐平面 Y/UV）→ `vkGetMemoryFdKHR(OPAQUE_FD)` → HIP import。
6. **SPS/PPS → Std 转换**：`fill_sps_std` / `fill_pps_std`（含 scaling list 8x8 顺序 `{0,3,1,4,2,5}`、designated-initializer 字段序、`aspect_ratio_idc` cast）。
7. **主机同步**：fence 等待、解码状态查询与导出 fd 都放在 `map_frame()` 的 `finish_frame()` 里；`decode()` 只提交不等待（见 §3.5）。异步 sync_file 桥接属并行项，不阻塞。

### 3.4 逐帧 slice 解析与共享 DPB

`decode()` 收到的 `CUVIDPICPARAMS.pBitstreamData` 是一个 access unit，可包含一个或多个 slice，仍用 FFmpeg `ff_h2645_packet_split` 切分并按 `nNumSlices` / `pSliceDataOffsets` 提交完整 slice 集合。

当前有两条状态路径：

- **CUVID parser 路径**：`CuvidParser` 在同步 `pfnDecodePicture` 作用域内提供 `H264DecodeContext`。后端直接使用共享 `H264Dpb` 的 POC、参考集合和 MMCO 结果，`setup_shared_dpb()` 维护 picture_index→Vulkan slot 映射并填写长期参考标记，不再重复执行本地 POC/MMCO。
- **nvdec 直接路径**：没有该 context，暂时保留自写 `parse_slice_header`、`ff_h264_init_poc` 和本地 DPB 状态机，后续单独迁移。

`CUVIDPICPARAMS` 里 SPS/PPS 派生字段（`log2_max_frame_num_minus4` 等）这里不依赖——两条来源最终都会重新 parse 成 `SPS*`/`PPS*`。原始 cache 保留完整参数集；合成路径只有 CUVID 可见字段。

### 3.5 map_frame 与 fd 所有权

`decode()` 提交后不等待；`map_frame()` 先调用 `finish_frame(PicIdx)`：等该帧的 fence、读该环槽的解码状态 query、`vkGetMemoryFdKHR` 导出 fd 存进 `exports_[PicIdx].fd`，再把 fd 转移给 `HipFrameMapping::map_fd()`（`exp.fd = -1` 后移交，`HipFrameMapping::reset()` 负责 close）。fd 生命周期与 VAAPI 后端一致（mapping 析构即 close），无泄漏。

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
5. 序列变化后不会重建 session，`reconfigure` 仍是 stub。

这些参数集限制必须显式失败，不允许静默输出错误帧；多 slice access unit 则按完整码流和全部 slice offset 提交。当前已验证场景与剩余边界见[当前能力矩阵](../当前能力矩阵.md)。

### 3.7 能力查询与解码流水线

**能力查询（`ensure_session()`）**：不再硬编码 NV12 + OPTIMAL，所有前提先问驱动，不满足就显式失败并在 `CUVID_SHIM_DEBUG_VK=1` 下打印原因。

| 查询 | 行为 |
|---|---|
| `vkGetPhysicalDeviceVideoFormatPropertiesKHR`（DECODE_DST / DPB 各一次） | 只接受 NV12；优先 OPTIMAL，只有 LINEAR 时退回 LINEAR（导出拷贝对两种 tiling 都成立）。没有任何 NV12 条目 → `CUDA_ERROR_NOT_SUPPORTED`。`CUVID_SHIM_VK_TILING=linear\|optimal` 可绕过宣告强制 tiling，用于在只宣告 OPTIMAL 的驱动上验证 LINEAR 退化路径，不是生产开关 |
| `VkVideoDecodeCapabilitiesKHR::flags` | 必须支持 `DPB_AND_OUTPUT_DISTINCT`。COINCIDE-only 驱动需要输出面兼作参考面（另一套 slot 记账），当前不实现，显式拒绝 |
| `maxActiveReferencePictures` | 驱动报 0 时按 1 处理（否则所有带参考帧的提交都非法） |
| `vkGetPhysicalDeviceExternalBufferProperties(OPAQUE_FD)` | 导出 buffer 必须 `EXPORTABLE`；分配链 `VkExportMemoryAllocateInfo → VkMemoryDedicatedAllocateInfo` |
| memory type | 先找满足属性的类型，找不到再退到 `memoryTypeBits` 里任意类型；bitstream buffer 要求 `HOST_VISIBLE+HOST_COHERENT`，不降级 |
| 队列族 | decode 队列族自己能 copy 时用一个队列族（`EXCLUSIVE` 图像、无 semaphore）；否则才用第二个队列族（`CONCURRENT` + decode→copy semaphore） |

**流水线（`decode()` 不阻塞）**：每个在飞帧占一个环槽（`FrameRing`：decode/transfer 命令缓冲、`decode_done` semaphore、`copy_fence`、bitstream buffer、query 下标）。`decode()` 只记录并提交；`map_frame()` 的 `finish_frame()` 完成 fence 等待、逐帧状态检查和 fd 导出。`pending_` 每个输出面最多一个在飞帧。环深由 `CUVID_SHIM_PIPELINE_DEPTH` 控制（1–8，默认 4，上限是 `ulNumDecodeSurfaces`）：

- 同一 picture index 被再次解码：先 join 旧帧（该帧不会再被 map）；
- 环槽用尽：join 全部在飞帧后重试——这是 `decode()` 里唯一可能阻塞的路径，行为退回旧的逐帧同步；
- 提交失败或 fence 超时：置 `device_lost_`，后续提交直接失败（fail fast），避免在半提交状态下复用环槽；
- `CUVID_SHIM_NOCOPY=1`：不记录 transfer，`decode()` 内立即 join（诊断模式，map 仍可成功但 buffer 未被拷贝更新）。

当前交付范围、已验证场景和未闭环项统一见[当前能力矩阵](../当前能力矩阵.md)。本文只补充 Vulkan 特有的实现约束：参数集合成的信息保真边界见 §3.6，同线程多 decoder 限制见 §3.1，transfer-only/SDMA 路径的多平面 pitch 疑点见 §8.4。

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

### 8.5 2026-11-19 能力查询与流水线改造回归（RX 7900 XTX / RADV）

改动：§3.7 的能力查询与 `decode()` 去阻塞。回归口径（Vulkan 后端）：

| 项 | 结果 |
|---|---|
| `verify_kernel_read` + dump 对比软解 | 六路码流（`test_p_b_5`/`test_p_b_7`/`test_p_main`/`test_p_66`/`test_slices_320x240`/`test_crop_1080p`）逐帧逐字节一致；crop 按显示高度比较（dump 为 coded 高度） |
| FFmpeg 宿主矩阵（`run_ffmpeg_compare.sh BACKENDS=vk`） | 16/16 PASS，覆盖 `nvdec-safe`/`nvdec-unsafe`/`cuvid-cpu`/`cuvid-cuda` × 四路码流；`nvdec-unsafe` 的 D2D 仍为 0 |
| 复杂场景（`run_decode_scenarios.sh BACKENDS=vk`） | 6/6：多 slice / 1080p crop / MP4 seek × cuvid / nvdec |
| 多流（`run_multi_stream.sh BACKENDS=vk STREAMS=8`） | 8 路并发与软解一致 |
| `CUVID_SHIM_PIPELINE_DEPTH` = 1 / 4 / 8 | 三种深度均逐帧一致 |
| `CUVID_SHIM_VK_TILING=linear`（强制输出+DPB 线性，绕过宣告） | `test_p_b_7`/`test_p_main`/`test_slices_320x240`/`test_crop_1080p` 仍逐字节一致，说明 LINEAR 退化分支的拷贝与 barrier 正确 |
| `CUVID_SHIM_NOCOPY=1` | 13/13 解码与映射成功，校验和如预期不匹配（buffer 未被拷贝更新） |
| 临时大尺寸（`h264_vaapi` 生成，非归档码流） | 1080p 30 帧（coded 1088，含 B 帧）与 4K 6 帧均逐字节一致 |

未覆盖：COINCIDE-only 驱动与“只宣告 LINEAR”的驱动在本机不存在，对应分支只有能力拒绝/退化逻辑，未在真实设备上执行；性能收益（decode 与 export 拷贝重叠）尚未做同口径 A/B，矩阵“性能”一项仍是未闭环。

### 8.6 LINEAR 直出探针（`hip-decode-20261119/`）

与后端实现分开的一次驱动能力验证：把解码输出面直接建成 LINEAR 并确认 HIP 能直接读，不经过本后端的 tiled→linear 拷贝。

- 输出面：`VK_IMAGE_TILING_LINEAR` + `VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR` + external memory（dedicated），`vkGetMemoryFdKHR(OPAQUE_FD)` → `hipImportExternalMemory`；DPB 仍是 OPTIMAL。
- 码流侧：复用 FFmpeg 的 SPS/PPS/slice-header/POC 解析，真实 I/P/B + 多参考 DPB；输出按 `(GOP, POC)` 显示序与 ffmpeg 软解逐帧逐字节比较，crop 流按显示区域比较。
- 结果（RX 7900 XTX / RADV / Mesa 25.2.8，validation layer 开启）：**20 PASS / 3 SKIP / 0 FAIL**。覆盖 Baseline/Main/High × 8-bit 4:2:0，B-pyramid、refs=16、4 slices、intra-refresh、open-GOP、weighted、deblock off、AUD，96x96–4K，854x480 与 1080p 的 crop。
- 驱动能力边界：`vkGetPhysicalDeviceVideoFormatPropertiesKHR` 只宣告 OPTIMAL（LINEAR 属“可用但未宣告”）；High 10 / High 4:2:2 / High 4:4:4 以及任何 10-bit 返回 `VK_ERROR_VIDEO_PROFILE_*_NOT_SUPPORTED`；4:2:2/4:4:4 即使 caps 通过，也没有对应的 decode-DST 格式，探针按 SKIP 处理。
- 未覆盖：10-bit（本机 libx264 为 8-bit 构建；探针已实现 P010 输出与比较）、MMCO 长期参考、SP/SI、interlaced/MBAFF、POC type 1、FMO/ASO、同一流内 SPS/PPS 切换。
- 与本后端的关系：探针只证明驱动通路。后端仍走“tiled 解码 + GPU 内拷贝”（§3.7 的 LINEAR 只是拷贝路径的 tiling 回退）；改直出需要 `HipFrameMapping` 支持逐平面 offset，并做运行时能力探测，见[当前能力矩阵](../当前能力矩阵.md)和 [validation.txt](../../hip-decode-20261119/validation.txt)。

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
- Vulkan Video：驱动内部管理 DPB。应用只需为每帧分配 DPB slot，并通过 `StdVideoDecodeH264PictureInfo` 告诉驱动参考关系；硬件按 SPS/PPS + 参考信息自行维护。CUVID parser 路径现在由共享 `H264Dpb` 统一提供这些参考关系，nvdec 直接路径才保留后端本地 `parse_slice_header` 和 POC 计算。

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

1. 不要把“format query 只宣告 OPTIMAL”当成“硬件不能 LINEAR”。`hip-decode-20261119` 已在 AMD 机上证明：用 `VK_KHR_video_maintenance_1` 的 profile-independent 图像 + external memory 可以把 H.264 8-bit 4:2:0（Baseline/Main/High）直接解码进 LINEAR 输出面，导出 OPAQUE_FD 后 HIP 直接读取，96x96–4K、P/B/多 slice/crop 逐字节一致。但该能力不在驱动宣告内，跨驱动不可移植；要用它必须做运行时探测（见探针的 `--probe`），不能当成规范保证，也不能直接替换当前 tiled+拷贝实现（`HipFrameMapping` 需要按逐个平面 offset 映射）。
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
