# Vulkan Video 后端调研与实现 TODO

> **状态更新（2026-11-12）**：本文的端到端通路（Vulkan 解码 → HIP 读像素）**已跑通并位精确**
> （RX 7900 XTX / RADV），后续事项与遗留项以
> [Vulkan 后端说明 §8](../architecture/vulkan-backend.md#8-上机验证结论rx-7900-xtx--radv) 为准。
> 下文保留调研时的问题拆解与验证矩阵，作为过程记录。
>
> 本文写给接手本任务的工程师（人或 AI）。请**先完整读完再动手**，按优先级顺序执行。
> 所有结论、术语背景、已有代码位置，都已写清，可直接开始工作。

---

## 0. 一句话目标

验证并落地一条 **Vulkan Video 解码 → HIP 共享显存 → HIP 读取像素** 的端到端数据通路，作为 VAAPI 主线之外的 **Tier 2 备选后端**。

> 背景：VAAPI 主线（解码 + 线性直通 + NPP）已基本打通。Vulkan Video 目前只做了「能力探测」（V1~V6），尚未跑通真实解码到 HIP 的数据链路。本 TODO 就是补齐这条链路。

---

## 1. 结论先行（已经确认、无需再调研的部分）

以下结论**已定论**，接手者不用重新验证，直接采信：

| 结论 | 说明 |
|---|---|
| **导出（image→HIP 共享）不再是风险点** | 有标准解法 + 成熟参考实现，见 §3 |
| **同步不再是风险点** | 有标准桥接方案（主机同步先行，异步桥接并行），见 §4 |
| `vkCmdCopyImageToBuffer` **支持 OPTIMAL 源** | 重要勘误：它要求源 image 处于 `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` *布局/状态*，而非 `VK_IMAGE_TILING_LINEAR`。OPTIMAL tiling 的解码输出可以直接拷贝，无需自写 compute shader |
| Vulkan Video 解码输出**只支持 OPTIMAL tiling** | 规范锁死，不是 bug，短期不会变；因此「多一次拷贝」是固定成本，接受它 |
| HIP 无法导入 Vulkan 的 **opaque semaphore** | 死胡同，不要在这条路上花时间，改用 §4 的 sync_file 桥接 |

---

## 2. 目标数据通路（要落地实现并验证的就是这条）

```
Vulkan 解码输出 image（OPTIMAL tiling，NV12）
    │  解码完成依赖（semaphore/fence）
    │  image layout 从 VIDEO_DECODE_DST → TRANSFER_SRC_OPTIMAL（barrier）
    │  vkCmdCopyImageToBuffer：分别拷贝 Y 平面、UV 平面
    ▼
可导出的 GPU buffer（约定 pitch、平面 offset 的线性 NV12）
    │  vkGetMemoryFdKHR  →  dma-buf fd
    │  hipImportExternalMemory  →  hipExternalMemoryGetMappedBuffer
    ▼
HIP 设备指针：等拷贝完成后，HIP 读回逐字节校验（vs 软解参考）
```

**验收标准（核心，一条都不能少）**：
1. HIP 读回的 NV12 像素与软件解码参考**逐字节一致**。
2. 覆盖 I-only / I/P / I/P+B 三种码流，各 ≥30 帧（码流文件见 §6）。
3. 无 fd 泄漏、无显存泄漏（结束前后 fd 计数一致）。

---

## 3. 导出（拷贝）链路 —— 正确做法与参考实现

### 3.1 为什么用 `vkCmdCopyImageToBuffer`（而非 compute shader）

- 它是显卡**硬件拷贝引擎**，快、标准、无需维护 shader。
- 源 image 用 OPTIMAL tiling 完全没问题，**前提**是满足四个条件：
  1. image 创建时带 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`（解码输出用途之外，再 OR 上这个 bit）。
  2. 拷贝前用 `VkImageMemoryBarrier` 把 layout 转到 `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`。
  3. 用 `VkBufferImageCopy` 按 plane 分别设复制区域，**逐平面拷贝**（NV12 是 Y 一平面、UV 交错一平面），并正确设置 pitch / offset。
  4. 拷贝命令完成后，用一个 semaphore/fence 把「拷贝完成」信号传给 HIP 侧等待（见 §4）。

### 3.2 参考实现（已核实，可直接仿写）

| 项目 | 文件/函数 | 可借鉴什么 |
|---|---|---|
| **FFmpeg** | `hwcontext_vulkan.c` → `vulkan_transfer_frame()` | 按平面设 `VkBufferImageCopy`、建 barrier、调 `vkCmdCopyImageToBuffer`，处理行距(pitch)与 offset |
| **GStreamer** | `vkdownload.c` | 把 image 转成 `TRANSFER_SRC_OPTIMAL`，多平面分别处理，再 `vkCmdCopyImageToBuffer` |
| **AMD 官方** | `vulkan_interop` 样例 | 创建**可导出**的 Vulkan buffer → `vkGetMemoryFdKHR` 导出 fd → `hipImportExternalMemory` + `hipExternalMemoryGetMappedBuffer` 得到 HIP 指针 |

> 关键词：`vulkan_transfer_frame`、`vkdownload`、`vulkan_interop`。找不到源码时，可直接搜索这三个名字定位。

### 3.3 可导出 buffer 的关键点

- 目标 buffer 分配内存时，memory 必须带**可导出**能力：查询支持 `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT` 且能导出 fd 的 memory type index（用 `vkGetPhysicalDeviceImageFormatProperties2` / external memory 能力查询确认）。
- 导出句柄类型用 `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`。
- HIP 侧导入用 `hipExternalMemoryHandleTypeOpaqueFd`（现有 `hip_read_surface.cpp` 已封装，见 §6）。

---

## 4. 同步链路 —— 正确做法

### 4.1 两条线，并行推进，互不阻塞

| 阶段 | 方案 | 说明 |
|---|---|---|
| **首期（功能正确优先）** | **主机同步** | Vulkan 侧 `vkQueueWaitIdle`（或 fence wait）确保解码+拷贝完成 → HIP 侧 `hipDeviceSynchronize` 后读取。简单、正确、够用 |
| **进阶（性能优化）** | **异步桥接** | 让 Vulkan 完成信号直接驱动 HIP 队列等待，无 CPU 介入 |

### 4.2 异步桥接的正确路线（不要走 semaphore 死胡同）

```
Vulkan 侧：VkFence 完成
    │  VK_KHR_external_fence_fd：vkGetFenceFdKHR 导出 SYNC_FD（标准格式，非 opaque）
    ▼
sync_file fd
    │  DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE → 导入成 amdgpu syncobj
    ▼
syncobj（amdgpu 内核跨 API 同步原语，已实测支持：DRM_CAP_SYNCOBJ_TIMELINE=1）
    │  ROCm 侧：把 syncobj 挂成 HIP queue 的 wait fence
    ▼
HIP kernel/拷贝自动等待，全程无 CPU 参与
```

**改动归属（重要，判断工作量用）**：
- amdgpu 内核 syncobj：**已支持，不用改**。
- Mesa/RADV 导出 SYNC_FD fence：**基本不用改**（`VK_KHR_external_fence_fd` 已支持）。
- **ROCm/ROCr 用户态**：需补「sync_file/syncobj → hsa_signal，作为 queue 提交的 wait fence」这一环——**这是主要缺口，属厂商协同项**。

> 结论：异步同步**首期不阻塞**。先做主机同步跑通功能，异步桥接作为并行/后续项，需 ROCm 侧配合。

---

## 5. 待办清单（按优先级，逐项有验收标准）

### P0 —— 必做，决定 VK 后端能否落地

**P0-1 落地「导出拷贝链路」并验证**
- 目标：在 AMD 验证机实现 §2 的整条链路（解码→拷贝→导出→HIP 导入）。
- 动作：
  - 新建程序（建议 `tests/vulkan-video/full/` 下新增，或独立 `tests/zerocopy/verify_vk_decode_to_hip.cpp`），复用现有 V4 的 session 创建逻辑，补上「真实解码 + 拷贝 + 导出」。
  - 按 §3 实现：TRANSFER_SRC usage + barrier 转 layout + 逐平面 `vkCmdCopyImageToBuffer` + 可导出 buffer + `vkGetMemoryFdKHR`。
- 验收：拷贝出的 linear buffer 内容，用 HIP 读回后与软解参考**逐字节一致**（先跑通 I-only 一帧即可标记本项通过，多帧/多码流留给 P0-3）。

**P0-2 落地「同步桥接」并验证（首期主机同步即可）**
- 目标：确保解码+拷贝完成后 HIP 才读，无花屏/竞态。
- 动作：首期用 `vkQueueWaitIdle` + `hipDeviceSynchronize`；异步桥接（§4.2）标记为并行跟进项，不阻塞。
- 验收：连续多帧读回稳定正确，无偶发错帧。

**P0-3 端到端像素校验（对标 VAAPI 实验 C，核心证据）**
- 目标：Vulkan 解码 I / IP / IPB 三种码流各 ≥30 帧，HIP 读回逐字节 vs 软解参考。
- 动作：复用 `tests/zerocopy/` 现有码流和参考文件（见 §6），软解参考可用 FFmpeg 生成。
- 验收：三码流全逐字节一致 + 无 fd/显存泄漏。

> P0-1 + P0-2 + P0-3 本质是一条串联链路，可合并写进**一个程序**，一次跑通即覆盖全部。程序结构建议对标现有 `tests/zerocopy/verify_decode_to_hip.cpp`（VAAPI 版），只是把后端从 VAAPI 换成 Vulkan Video。

### P1 —— P0 通过后，工程收口

| # | 待办 | 验收 |
|---|---|---|
| P1-1 | 会话/DPB/资源生命周期对齐：VK 后端的 session、参考帧缓存(DPB)、map/unmap、错误语义对齐 CUVID 的 decoder/map/unmap 接口 | 创建/复用/释放不崩溃、不泄漏，错误码语义正确 |
| P1-2 | 接入验收宿主：shim 的 VK 后端接回 FFmpeg `h264_cuvid`/`h264_nvdec`（宿主不变，只换后端） | 双路径各自完成多帧像素/帧数/顺序/pitch/flush 校验 |
| P1-3 | 稳定性：多帧、seek/flush、1080p/4K、1/8 路、30 分钟 | 无崩溃、死锁、持续泄漏 |

### P2 —— 价值判断与长期（有真实需求再投入）

| # | 待办 | 说明 |
|---|---|---|
| P2-1 | 编码能力重估 | 最新 Mesa 上重查 RADV 编码（profile、码率控制 CBR/VBR/CQP）。VK 编码仍是生态最弱一环，大概率只能软件兜底 |
| P2-2 | 跨平台需求确认 | 与业务确认有无 Windows/移动端交付。VK 差异化价值几乎全在「跨平台」，无此需求则投入理由大幅减弱（**建议尽早并行做，成本极低**） |
| P2-3 | 性能对照 | VK（拷贝+同步）vs VAAPI（linear 直通）在目标平台的吞吐/延迟/显存占用，量化「多一次拷贝」的真实代价 |

---

## 6. 已有资源（可直接复用，不要重复造）

### 6.1 可复用的代码

| 路径 | 内容 | 复用点 |
|---|---|---|
| `tests/vulkan-video/full/verify_vulkan_video_full.cpp` | V1~V6 能力探测（已跑通） | V1 队列、V2 能力、V3 格式、V4 session 创建逻辑可照搬 |
| `tests/zerocopy/verify_decode_to_hip.cpp` + `.make` | **VAAPI 版**端到端「解码→HIP 读逐字节校验」（已闭环 I/IP/IPB） | **程序结构直接对标**，把 VAAPI 段换成 Vulkan Video 即可 |
| `tests/zerocopy/hip_read_surface.cpp` | HIP 导入 fd → 读 Y/UV → 写文件 | `hip_import_fd()` 封装直接复用（OpaqueFd 导入 + GetMappedBuffer） |
| `tests/zerocopy/verify_modifier_linear.cpp` | VAAPI linear 创建路径 | 参考其 pitch 256 对齐、UV offset 约定 |

### 6.2 测试码流与参考

| 文件 | 说明 |
|---|---|
| `testdata/h264/test_320x240.h264` | I-only 码流 |
| `testdata/h264/test_p_320x240.h264` | I/P 码流 |
| `testdata/h264/test_b_320x240.h264` | I/P/B 码流 |
| `tests/zerocopy/ref_*_320x240.yuv` | 对应软解参考（NV12 320×240） |

> 软解参考生成：`ffmpeg -i test_xxx.h264 -f rawvideo -pix_fmt nv12 ref.yuv`（注意 320×240）。

### 6.3 环境信息（AMD 验证机）

- 硬件：Radeon RX 7900 XTX（gfx1100/Navi31，24GB）
- OS：Ubuntu 24.04.2，kernel 6.11.0-17
- 软件：ROCm 7.2.4 / HIP 7.2.53211；Mesa 25.2.8；Vulkan 1.4.318
- 依赖：`libvulkan-dev`、`vulkan-tools`（装法见 `docs/architecture/vulkan-video-evaluation.md` §五）

### 6.4 从 VAAPI 版 `verify_decode_to_hip.cpp` 改造到 VK 版的改动清单

**核心原则：`verify_decode_to_hip.cpp`（768 行）可以拆成「后端无关」和「VAAPI 专属」两半。后端无关部分原样保留，VAAPI 专属部分逐段替换成 Vulkan Video 等价物。**

#### A. 原样保留（后端无关，与 VAAPI/VK 无关，不要改）

| 段落（行号参考） | 内容 | 说明 |
|---|---|---|
| 42–53 | FFmpeg parser 复用头 + `HAVE_AV_CONFIG_H` | 切 NAL、SPS/PPS/slice header 解析的依赖 |
| 66–75 | `read_file()` | 读码流 |
| 77–90 | `sps_dimensions()` / `pick_profile()` | 注意：`pick_profile` 是 VAAPI 专属（VAProfile 枚举），**VK 版要换成 VK 的 profile 映射** |
| 133–266 | `parse_slice_header()` | 纯语法解析，产出 SliceInfo，VK 版照样要填 `StdVideoDecodeH264PictureInfo`，复用 |
| 285–291 | `count_fds()` | fd 泄漏检查，通用 |
| 311–345 | 复用 FFmpeg 切 NAL + 收集 slice | 通用 |
| 386–401 | 读软解参考文件 | 通用 |
| 708–736 | 显示序重排 + 逐字节比对 | 通用（注意见下方「DPB 关键差异」） |
| 738–760 | flush 幂等检查 + fd 检查 + 清理 | 通用（VAAPI 专属清理替换） |

#### B. 逐段替换（VAAPI 专属 → Vulkan Video 等价物）

| VAAPI 现实现（行号） | 要换成什么 |
|---|---|
| 303–309 `vaGetDisplayDRM`/`vaInitialize` | Vulkan instance/device 创建（照搬 `verify_vulkan_video_full.cpp` 的 main 里那套），启用 `VK_KHR_video_queue`、`VK_KHR_video_decode_h264`、`VK_KHR_external_memory_fd` 等扩展 |
| 348–368 `vaCreateSurfaces`(LINEAR pool) | 创建 **VkVideoSessionKHR**（照搬 V4 的 session 创建逻辑）+ 分配 DPB/output 的 VkImage 池（解码输出 image 用 OPTIMAL tiling，带 `TRANSFER_SRC` usage） |
| 374–384 `vaCreateConfig/Context` | 无直接对应；VK 是 session + 每帧 `vkCmdDecodeVideoKHR` |
| 407–487 DPB 状态机 + RefPicList 构建（手写） | **见下方「DPB 关键差异」** |
| 489–579 填 `VAPictureParameterBufferH264`/`VAIQMatrixBuffer`/`VASliceParameterBuffer`/`VASliceDataBuffer` | 填 **`StdVideoDecodeH264PictureInfo`**（在 `VkVideoDecodeInfoKHR.pNext` 链上）+ `VkVideoReferenceSlotInfoKHR`（参考帧 slot 索引），SPS/PPS 走 session 创建时的 `StdVideoH264SequenceParameterSet`/`StdVideoH264PictureParameterSet` |
| 581–592 `vaBeginPicture`/`vaRenderPicture`/`vaEndPicture`/`vaSyncSurface` | `vkCmdBeginVideoCodingScopeKHR` → `vkCmdDecodeVideoKHR` → `vkCmdEndVideoCodingScopeKHR`，同步用 fence + `vkWaitForFences`（首期主机同步） |
| 594–619 `vaExportSurfaceHandle` | **`vkCmdCopyImageToBuffer`**（先 barrier 把解码输出 image 转 `TRANSFER_SRC_OPTIMAL`，逐 plane 拷到可导出 buffer）→ `vkGetMemoryFdKHR` 导出 fd |
| 621–647 HIP 导入 + 逐行读 | **基本复用**，fd 来源从 `desc.objects[o].fd` 换成 `vkGetMemoryFdKHR` 的 fd；linear pitch/offset 约定照旧（256 对齐，UV offset=pitch×H） |

#### C. DPB 关键差异（最容易搞错，务必理解）

- **VAAPI VLD**：应用**手动管理**参考帧——手写 DPB 状态机做 surface 复用，并显式填 `ReferenceFrames[]` 和 `RefPicList0/1[]`。所以 `verify_decode_to_hip.cpp` 里有大量手写 DPB/ref-list/reorder 代码（407–487、658–706 行）。
- **Vulkan Video**：**驱动内部管理 DPB**。应用只需要：
  1. 为每帧分配一个 **DPB slot 索引**（`VkVideoReferenceSlotInfoKHR`），并把当前帧的参考关系通过 `StdVideoDecodeH264PictureInfo` 的 reference 字段告诉驱动；
  2. 硬件按 SPS/PPS + 参考信息自行维护参考帧，**不需要应用手写滑动窗口/MMCO/RefPicList 重排**。

**结论**：VAAPI 版里那套「手写 DPB 状态机 + RefPicList 构建 + 显示序重排」**大部分可以删掉**，但 `parse_slice_header`（产出每帧 slice 类型、frame_num、ref 信息）和 POC 计算**仍要保留**，用来正确填 `StdVideoDecodeH264PictureInfo`。显示序（POC）重排的**比对逻辑**仍保留（因为软解参考是显示序），但重排本身只需按 POC 排序，不再需要 VAAPI 那套 list 重排。

> 建议先查 Vulkan 官方 `video_decode_h264` 样例（Khronos Vulkan-Samples / Mesa 测试里的 `h264_decoder`），它对「session + DPB slot + StdVideoDecodeH264PictureInfo 填充」有完整示范，比从 VAAPI 版硬改更直接。

---

## 7. 关键口径提醒（避免走弯路）

1. **不要**试图让 Vulkan 解码输出 LINEAR —— 规范锁死，改 Mesa 是永久 fork，已否定。
2. **不要**改 `hipImportExternalMemory` 去"读 tiled" —— tiling 在硬件寻址层，改接口无效。
3. **不要**在 semaphore（opaque fd）上花时间 —— 跨栈不互认，死胡同，改走 §4.2 的 sync_file 桥接。
4. **记住**：有方案 ≠ 已验证。本文所有「不再风险点」指的是"实现路径明确 + 有成熟参考"，但 **AMD 机器上的真实像素级验证（P0-3）仍然没做**，这是 VK 后端转正的唯一还缺的证据。

---

## 8. 产出物约定

完成后请输出：
1. 一个可编译运行的验证程序 + Makefile（建议放进 `tests/zerocopy/` 或 `tests/vulkan-video/full/`）。
2. 运行日志（含逐字节对比结果、fd 计数、PASS/FAIL 汇总）。
3. 一份简短结果说明：哪些项通过、哪些未通过、卡点在哪。

> 相关背景文档：`docs/architecture/vulkan-video-evaluation.md`（评估+实测）、`docs/architecture/cuda-compat-plan-integrated.md` §4.4/§4.12（结论收口）、`docs/architecture/cuda-compat-plan-optimized.md` §4.5（通俗版）。
