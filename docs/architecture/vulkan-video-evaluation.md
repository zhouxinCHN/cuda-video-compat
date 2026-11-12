# Vulkan Video 编解码方案评估 + 零拷贝验证

> 本文回答两个问题：
> 1. **用 Vulkan Video 接口实现编解码（替代 VAAPI 路线）可行吗？**
> 2. **Vulkan Video ↔ HIP 能否零拷贝？**（附可执行验证 demo）

---

## 一、结论先行

| 问题 | 结论 |
|---|---|
| Vulkan Video 能否实现编解码？ | **可行**，且是比 VAAPI 更「跨厂商」的开放标准，但生态和驱动成熟度目前不如 VAAPI |
| 能否与 HIP 零拷贝？ | **可以**，通路清晰：`VK_KHR_external_memory_fd` 导出 dma-buf ↔ `hipImportExternalMemory` 导入，两边对称 |
| 现在（本机）能否验证？ | 不能 —— 本机缺 Vulkan 头文件、loader 开发包、`vulkaninfo`；demo 已写好，装依赖即可在 AMD 机器跑 |

---

## 二、Vulkan Video 是什么

Vulkan 是 Khronos 的低层图形/计算 API（OpenGL/OpenCL 的继任者）。**Vulkan Video 是 2021 年起的扩展族**，把硬件视频编解码纳入 Vulkan 的显式同步、显式内存管理模型：

```
┌─────────────────────────────────────────────────────────────┐
│                        应用（用户 CUDA SDK 产品）             │
├─────────────────────────────────────────────────────────────┤
│  我们的 shim 库（libnvcuvid.so.1 / libnvidia-encode.so.1）  │
├─────────────────────────────────────────────────────────────┤
│        Vulkan Video 扩展（API 层）                          │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────────────┐ │
│  │ video_queue  │ │decode_queue  │ │  encode_queue        │ │
│  │ (基础)       │ │(h264/h265/av1)│ │(h264/h265)          │ │
│  └──────────────┘ └──────────────┘ └──────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│        Mesa RADV / ANV / 驱动实现（硬件）                    │
└─────────────────────────────────────────────────────────────┘
```

关键扩展名（demo 的「实验B」就是枚举这些）：

| 扩展 | 作用 |
|---|---|
| `VK_KHR_video_queue` | 视频编解码队列（基础，必选） |
| `VK_KHR_video_decode_queue` | 解码队列 |
| `VK_KHR_video_encode_queue` | 编码队列 |
| `VK_KHR_video_decode_h264` / `_h265` / `_av1` | 具体编码格式解码 |
| `VK_KHR_video_encode_h264` / `_h265` | 具体编码格式编码 |
| `VK_KHR_video_maintenance1` | 维护扩展（DPB/参数更新，很多实现依赖） |
| `VK_KHR_external_memory` / `_fd` | **外部内存导出（零拷贝基础）** |

---

## 三、Vulkan Video vs VAAPI 对比（选型依据）

| 维度 | VAAPI（已选方案） | Vulkan Video（本方案） |
|---|---|---|
| 标准组织 | Linux 生态（Intel/AMD 主导） | Khronos（跨平台：Linux/Windows/Android） |
| 驱动成熟度 | **很成熟**，AMD/Intel/NVIDIA(经 libva) 都有 | 较新，**Mesa RADV 自 2023 才逐步落地**，编码尤其滞后 |
| 与 HIP/ROCm 零拷贝 | 需 `vaExportSurfaceHandle` + `hipImportExternalMemory`（单向为主） | `VK_KHR_external_memory_fd`，**双向对称、导出更标准** |
| 与 CUDA 生态语义接近度 | 一般（VAAPI 是独立抽象） | **更高**：Vulkan 显式内存模型与 CUDA 的 import/export 语义天然对齐 |
| 头文件/法律 | 全开源（libva） | 全开源（Vulkan-Headers，Apache 2.0） |
| 对新厂商 GPU 友好度 | 依赖厂商写 libva 驱动 | 依赖厂商写 Vulkan driver（**用户态驱动基于 Mesa 时，RADV 可扩展**） |
| FFmpeg 现成支持 | `h264_vaapi` / `h264_vulkan` | `h264_vulkan`（已有，但更年轻、bug 多） |

**结论**：两条路**都保留**。VAAPI 是「近期落地快、驱动成熟」的选择；Vulkan Video 是「长期更标准、跨厂商、与 CUDA 内存模型更对齐」的选择。建议：
- **短期**：NVENC/NVDEC shim 仍映射到 VAAPI（成熟、可控）。
- **中长期**：抽象一层「硬件编解码后端」，后端1=VAAPI，后端2=Vulkan Video；零拷贝统一走 dma-buf。

---

## 四、零拷贝通路（Vulkan Video 版本）

```
        Vulkan 解码（VkVideoDecode）                 HIP 计算
  ┌──────────────────────────────┐          ┌──────────────────────┐
  │  VkImage (NV12/decoded out)  │          │  HIP 设备内存        │
  │  VkDeviceMemory              │          │  (hipExternalMemory) │
  └──────────────┬───────────────┘          └──────────┬───────────┘
                 │  VK_KHR_external_memory_fd            │ hipImportExternalMemory
                 │  vkGetMemoryFdKHR ──> dma-buf fd ──>   │ (OpaqueFd)
                 └───────────────────────────────────────┘
                         零拷贝：同一块物理内存，双方共享
```

与 VAAPI 版本的关键区别：**Vulkan 的导出是标准的一部分**（`VK_KHR_external_memory_fd`，Khronos 定义），不像 HIP 侧导出还要绕 HSA 私有 API（`hsa_amd_portable_export_dmabuf`）。所以：
- **Vulkan → HIP**（解码结果给 HIP 计算）：干净，`vkGetMemoryFdKHR` 是标准 API。
- **HIP → Vulkan**（HIP 算完给 Vulkan 编码）：仍要走 `hsa_amd_portable_export_dmabuf`（HIP 无标准导出），这是 AMD 私有 API，**这一半和 VAAPI 方案共用同一解法**。

---

## 五、demo 说明（verify_vulkan_video.cpp）

三个实验：

| 实验 | 内容 | 通过条件 |
|---|---|---|
| 实验0 | 环境探测：Vulkan instance/device + HIP 初始化，同进程共存 | 都能初始化 |
| 实验B | 枚举设备支持的 Vulkan Video 扩展（纯字符串，不依赖版本敏感 struct） | 列出 h264/h265/av1 的 decode/encode 支持情况 |
| 实验A | 零拷贝：`vkAllocateMemory`(带 fd 导出) → `vkGetMemoryFdKHR` → `hipImportExternalMemory` → `hipMemset`/`hipMemcpy` 读写验证 | HIP 直接读写 Vulkan 的 DEVICE_LOCAL 内存，数据一致 |

**在 AMD 机器上运行**：

```bash
sudo apt install libvulkan-dev vulkan-tools   # Vulkan 头文件 + loader + vulkaninfo

cd $REPO_ROOT/tests/vulkan-video/basic
make                 # 编译（ROCM 路径不对时 make ROCM_DIR=...）
make run             # 运行，或 ./verify_vulkan_video
```

**预期**（AMD RX 7600 + Mesa RADV，需较新 Mesa ≥ 23.x）：

- 实验0 全 PASS。
- 实验B：`VK_KHR_video_decode_h264/h265` 大概率 PASS（RADV 解码较成熟）；`av1` 和 `encode_*` 视 Mesa 版本而定，**encode 很可能 FAIL**（RADV 编码支持最晚、最不稳）——这是 Vulkan Video 路线的核心短板。
- 实验A：`VK_KHR_external_memory_fd` 在 RADV 上成熟，应能 PASS；若 FAIL，优先检查 `memoryTypeIndex`（是否真有带导出能力的 DEVICE_LOCAL 内存类型，RADV 一般都有）。

**现场排查提示**：
1. 实验A 若 `vkAllocateMemory` 返回失败，先跑 `vulkaninfo` 看 `VK_KHR_external_memory_fd` 是否在支持列表。
2. `vkGetMemoryFdKHR` 需 `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`，RADV 支持。
3. HIP 导入后 `hipMemset` 失败，可能是外部内存 flag 或 size 对齐问题，demo 里已按 `memReq.size` 严格对齐。

---

## 六、Vulkan Video 做编码：失败风险与降级策略（细化）

> 本节专门拆解「用 Vulkan Video 实现编码（替代 VAAPI 编码）」这条路的风险，
> 以及每一级失败时怎么降级。核心结论：**编码是 Vulkan Video 生态里最弱的一环，
> 必须设计成多级可降级，不能作为唯一依赖。**

### 6.1 编码能力的细粒度分层（风险不是"有/无"，而是"到哪一层"）

Vulkan Video 编码要真正跑起来，需要**逐层都成立**。任何一层断掉，编码就不可用：

| 层 | 检查项 | RADV 现状（2024~2026） | 风险等级 |
|---|---|---|---|
| L1 驱动支持 | `VK_KHR_video_encode_queue` + `VK_KHR_video_encode_h264/h265` 是否被驱动宣称 | 近年 RADV 才逐步合入 encode 支持，**h265 encode 更晚** | **高** |
| L2 能力完整 | 查询 `VkVideoCapabilities`：目标 profile、分辨率、码率控制（CBR/VBR）是否在能力表内 | 能力表覆盖不全，很多 profile 缺位 | **高** |
| L3 语义对齐 | 码率控制模型（`VkVideoEncodeRateControlInfoKHR`）、量化参数、GOP 结构 是否满足 NVENC 语义 | Vulkan 的 RC 模型比 NVENC 粗，**对齐成本高** | 中 |
| L4 稳定性 | 连续编码多帧不出错、无内存泄漏、性能达标 | 编码路径测试少，bug 多 | **高** |
| L5 零拷贝 | encode 输出的 VkImage 能否走 `external_memory_fd` 与 HIP 互通 | 相对成熟（与解码共用） | 低 |

**关键判断**：解码可能 L1~L2 就 PASS，但编码很可能卡在 **L1/L2（驱动根本没宣称，或宣称了但能力表残缺）**，而不是 L3/L4 这种"能跑但没调好"的问题。所以风险主要不是"调优难"，而是"根本跑不起来"。

### 6.2 失败模式矩阵

| # | 失败现象 | 根因 | 影响 | 判定来源 |
|---|---|---|---|---|
| F1 | `vkCreateDevice` 失败 | 驱动不含 encode 扩展 | 编码完全不可用 | demo 实验B `encode_*` FAIL |
| F2 | 能力查询返回不支持目标 profile | 能力表残缺 | 该 profile 不可编码 | `vkGetPhysicalDeviceVideoCapabilitiesKHR` |
| F3 | `vkCreateVideoSession` 失败 | RC/参数组合不合法 | 无法建编码会话 | 运行时报错 |
| F4 | encode 输出画质/码率不符合 NVENC 预期 | RC 模型差异 | 需重调参数映射 | 画质对比测试 |
| F5 | 编码连续跑 N 帧后崩溃/泄漏 | 驱动 bug | 生产不可用 | 长稳测试 |
| F6 | encode 输出内存无法导出 fd | 驱动未给 encode 图加 export 能力 | 编码结果回不了 HIP（退化） | demo 实验A 在 encode 用法位图上复测 |

### 6.3 多级降级策略（fallback 链）

编码后端按优先级从「最理想」到「最保险」排成一条链，运行时按能力探测**自动选级**：

```
      ┌─────────────────────────────────────────────────────────────┐
      │          libnvidia-encode.so.1 编码后端选择                  │
      └─────────────────────────────────────────────────────────────┘
                                │
            ┌───────────────────┼───────────────────┐
            ▼                   ▼                   ▼
    [Tier 1]             [Tier 2]             [Tier 3]
  VAAPI 编码           Vulkan Video 编码      纯软件编码
  (成熟, 首选)          (实验性, 探测启用)     (兜底, 永远可用)
```

| Tier | 后端 | 启用条件 | 零拷贝 | 备注 |
|---|---|---|---|---|
| **Tier 1** | VAAPI encode | VAAPI 驱动宣称支持目标 profile | 可（vaExportSurfaceHandle） | **默认首选**，成熟稳定 |
| **Tier 2** | Vulkan Video encode | 6.1 表 L1~L2 全 PASS 才启用 | 可（external_memory_fd） | **默认关闭**，靠开关打开 |
| **Tier 3** | 软件编码（x264/x265/FFmpeg SW） | 前两者都不可用 | 无（需 CPU↔GPU 拷贝） | 性能差，但**保证功能可用** |

**关键设计原则**：

1. **Vulkan Video 编码永远不是默认**。默认走 VAAPI（Tier 1），Vulkan Video 只是「探测到更强能力时」的备选或 A/B 开关，绝不让它单点承担生产编码。
2. **能力探测是运行时动态的**，不是编译期写死。用 `vkGetPhysicalDeviceVideoCapabilitiesKHR` 查询真实能力，而不是只看扩展字符串——避免 F2（宣称了但能力残缺）。
3. **软件编码兜底是必须的**。即使 VAAPI 和 Vulkan 都不可用，也要能出帧，保证产品功能闭环（代价是性能，换可靠性）。

### 6.4 决策树（拿到 AMD 机器 demo 结果后怎么决策）

```
实验B 结果
  ├─ decode_*  PASS → 解码零拷贝路线可推进（实验A 验证）
  │
  └─ encode_*  ?
       ├─ FAIL（很可能是这个） → 编码锁死 VAAPI（Tier 1）+ 软件兜底（Tier 3）
       │                         Vulkan Video 编码仅标记为「未来观察」，不投入
       │
       └─ PASS（意外顺利） → 仍先跑 VAAPI 为主，Vulkan Video 编码做 A/B
                              对比（画质/码率/性能），通过后再考虑 Tier 2 放行
```

**一句话**：无论 demo 的 encode 结果如何，**VAAPI 都是编码的首选和默认**。Vulkan Video encode 只有当它「不仅宣称支持、且能力表完整、且画质码率对齐 NVENC」时，才允许作为 Tier 2 放行，且要保留一键回退 Tier 1 的开关。

### 6.5 若强行用 Vulkan Video 编码，需额外投入（成本清单）

如果未来决定投入 Tier 2，以下都是 VAAPI 方案**不需要**、Vulkan Video 方案**必须**的额外工作：

1. **RC 模型映射**：NVENC 的 `NV_ENC_RC_PARAMS`（CBR/VBR/CQ/低延迟模式）↔ Vulkan 的 `VkVideoEncodeRateControlInfoKHR` 语义不完全对应，需逐项映射 + 校准。
2. **GOP/参考帧结构对齐**：NVENC 的 GOP 长度、B 帧、参考帧策略 ↔ Vulkan 的 `VkVideoEncodeUsageInfoKHR` / DPB 管理，对齐成本高。
3. **session 状态机**：Vulkan 编码是显式 session + `vkCmdEncodeVideoKHR` 命令录制，比 VAAPI 的同步接口复杂，状态机要自写。
4. **多格式矩阵测试**：h264/h265 × 多种 profile/分辨率/码率 × 长稳，测试面大。
5. **驱动 bug 应对**：RADV 编码路径 bug 多，需保持跟进 Mesa 版本，可能要给 Mesa 上游提 patch（成本不可控）。

**结论**：Vulkan Video 编码的「通用性/标准性」红利，当前被「成熟度差 + 语义对齐成本高」抵消。**收益不明确时不投入**，把省下的精力放在「VAAPI 编码做稳 + 解码零拷贝打通」上。

---

## 七、AMD 机器实测结果（RX 7900 XTX）

> 实测时间：2026-11-02  
> 硬件：AMD Radeon RX 7900 XTX (RADV NAVI31)  
> Vulkan 版本：1.4.318  
> 验证程序：`tests/vulkan-video/full/verify_vulkan_video_full`

### 7.1 实测结果汇总

| 验证项 | 结果 | 关键发现 |
|---|---|---|
| **V1: 视频队列** | ✅ PASS | `VIDEO_DECODE` 和 `VIDEO_ENCODE` 队列族**默认就可用**（不需要 `RADV_PERFTEST`） |
| **V2: 编解码能力** | ✅ PASS | H264/HEVC 解码能力完整，H264 编码能力完整 |
| **V3: 视频格式** | ⚠️ 部分 PASS | 支持 NV12，但**只支持 OPTIMAL tiling**，不支持 LINEAR |
| **V4: 会话对象** | ✅ PASS | 视频会话可以创建，内存需求可以查询 |
| **V6: 外部信号量** | ❌ FAIL | HIP 不支持导入 Vulkan 导出的外部 semaphore |

### 7.2 详细结果

#### V1: 视频队列（默认就可用）

```
queue family[0]: flags=GRAPHICS COMPUTE TRANSFER SPARSE  queueCount=1
queue family[1]: flags=COMPUTE TRANSFER SPARSE  queueCount=4
queue family[2]: flags=VIDEO_DECODE  queueCount=1
queue family[3]: flags=VIDEO_ENCODE  queueCount=1
queue family[4]: flags=SPARSE  queueCount=1
```

**关键发现**：之前假设需要设置 `RADV_PERFTEST=video_decode,video_encode` 才能启用视频队列，实测发现**默认就暴露了**。这推翻了之前的假设，说明 RADV 的视频支持比预期更成熟。

#### V2: 编解码能力（完整）

**H264 解码**：
- maxCodedExtent: 4096x4096
- maxDpbSlots: 17
- maxActiveReferencePictures: 16
- DPB and output: DISTINCT（DPB 和输出图像分离）

**HEVC 解码**：
- maxCodedExtent: 8192x4352（支持 8K）
- maxLevelIdc: 8（High tier）

**H264 编码**：
- maxCodedExtent: 4096x4096
- rateControlModes: 0x7（CBR + VBR + CQP）
- maxRateControlLayers: 4
- maxBitrate: 1,000,000,000 bps（1 Gbps）
- maxQualityLevels: 2

**结论**：编解码能力**完全满足** NVDEC/NVENC shim 的需求。

#### V3: 视频格式（只支持 OPTIMAL tiling）

```
format[0]: vkFormat=1000156003 (NV12) imageTiling=0 (OPTIMAL) imageUsage=0x1000
```

**关键发现**：
- ✅ 支持 NV12（`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`）
- ❌ **不支持 LINEAR tiling**（imageTiling=0 表示 OPTIMAL）

**影响**：
- Vulkan Video 解码输出的 surface 是 OPTIMAL tiling（tiled + DCC）
- HIP 无法直接按线性 NV12 读取 Vulkan Video 解码输出
- 需要额外的 GPU 转换步骤（Vulkan compute shader 或 VPP）

**这是 Vulkan Video 零拷贝路线的核心障碍**。

#### V4: 会话对象（可以创建）

```
视频会话创建成功
视频会话内存需求查询成功，共 2 个绑定
  bind[0]: size=131072 memoryTypeBits=0x7ff
  bind[1]: size=28200960 memoryTypeBits=0x7ff
视频会话销毁成功
```

**结论**：会话对象模型可以落地，没有 crash 或 validation error。

#### V6: 外部信号量（HIP 不支持）

```
Vulkan semaphore 导出 fd=6 成功
hipImportExternalSemaphore: operation not supported
```

**关键发现**：
- Vulkan 可以导出 semaphore 为 fd
- **但 HIP 不支持导入外部 semaphore**（`hipImportExternalSemaphore` 返回 `operation not supported`）

**影响**：
- Vulkan ↔ HIP 的异步同步（通过外部 semaphore）**不可用**
- 只能走主机同步（`vaSyncSurface` + `hipEventSynchronize`）
- 性能会受影响，但功能可用

### 7.3 实测结论

#### Vulkan Video 编解码能力：✅ 可用

- 队列、codec profile、分辨率、rate control 都支持
- 会话对象可以创建
- 可以作为编解码后端候选

#### 零拷贝：❌ 受限

- 视频格式只支持 OPTIMAL tiling，不支持 LINEAR
- HIP 无法直接读取 Vulkan Video 解码输出的 tiled surface
- 需要额外的 GPU 转换（Vulkan compute shader 或 VPP）

#### 异步同步：❌ 不可用

- HIP 不支持导入外部 semaphore
- 只能走主机同步（`vaSyncSurface` + `hipEventSynchronize`）
- 性能会受影响，但功能可用

### 7.4 对方案的影响

#### Vulkan Video 可以作为编解码后端，但需要接受：

1. **解码输出是 tiled**，需要转换才能给 HIP 读取
   - 方案 A：在 Vulkan 侧用 compute shader 做 tiled→linear 转换
   - 方案 B：用 VPP（Video Post-Processing）做转换
   - 两种方案都会增加 GPU 开销，不是严格零拷贝

2. **同步只能走主机侧**，不能全异步
   - 性能损失：每帧需要主机等待
   - 但功能可用，不会阻塞

#### VAAPI 仍然是更好的选择

对比 Vulkan Video 的实测结果，VAAPI 的优势更明显：

| 维度 | VAAPI | Vulkan Video |
|---|---|---|
| 零拷贝 | ✅ 支持 LINEAR tiling | ❌ 只支持 OPTIMAL tiling |
| 异步同步 | ✅ 可以通过 fence 实现 | ❌ 只能主机同步 |
| 驱动成熟度 | ✅ 成熟 | ⚠️ 较新，但实测能力完整 |
| 跨平台 | ❌ Linux 专用 | ✅ 跨平台 |

**结论**：
- **短期**：VAAPI 是首选（零拷贝 + 异步同步）
- **中长期**：Vulkan Video 可以作为备选后端（需要接受 tiled 转换 + 主机同步）
- **如果坚持用 Vulkan Video**：需要实现 tiled→linear 转换层，并接受主机同步的性能损失

### 7.5 下一步建议

1. **优先完成 VAAPI 编解码 shim**（零拷贝 + 异步同步）
2. **Vulkan Video 作为 Tier 2 备选**（仅在 VAAPI 不可用时启用）
3. **如果未来需要跨平台支持**：投入 Vulkan Video 后端开发，但需要实现转换层

---

## 八、当前状态与下一步

- [x] 已写 `verify_vulkan_video.cpp` + `Makefile`（基础验证）
- [x] 已写 `verify_vulkan_video_full.cpp` + `Makefile`（完整验证 V1-V4, V6）
- [x] 已细化「Vulkan Video 编码」失败风险与多级降级策略（§6）
- [x] **已在 AMD 机器完成实测**（RX 7900 XTX，结果见 §7）
- [x] 实测结论：编解码能力完整，但零拷贝受限（只支持 OPTIMAL tiling），异步同步不可用
- [ ] 根据实测结果，决定是否投入 Vulkan Video 后端开发

**总风险定调**：Vulkan Video **编解码能力完整**，但**零拷贝和异步同步受限**。VAAPI 仍然是首选（零拷贝 + 异步同步），Vulkan Video 作为 Tier 2 备选（需要接受 tiled 转换 + 主机同步）。
