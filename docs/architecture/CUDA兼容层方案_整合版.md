# CUDA 兼容层技术实现、验证证据与风险边界（方案整合版）

> 版本：v4.5
> 文档日期：2026-11-13
> 技术事实截止：2026-11-12
> 职责：本文是**技术实现、验证证据与风险边界的唯一权威文档**，负责共享架构、数据通路、同步、ABI、parser、NPP、VAAPI/Vulkan/Mesa 后端实现，以及验证方法、实现缺口、防御性风险、追溯、实验证据和历史版本。
> 不负责：路线选型、成本、人力、排期、Gate 和法务决策见[总体方案](CUDA兼容层总体方案.md)；字段级翻译见[CUVID 到 VAAPI 映射表](CUVID到VAAPI映射表.md)；内存/同步契约见[内存与同步模型](内存与同步模型.md)；复现步骤见[验证手册](../verification/验证手册.md)。
> 读者：架构评审、工程实施、测试与风险决策。

> **后端决策状态（2026-11-13）**：VAAPI 与 Vulkan Video 尚未经领导决策，二者是同级候选。本文中的“VAAPI 默认/Tier 2”技术现状不等于路线批准；验证、缺陷修复与验收必须对两者使用同一口径，Vulkan 失败不得自动 SKIP。

---

## 0. 证据口径

### 0.1 证据等级

| 等级 | 定义 |
|---|---|
| **AMD-PoC** | 已在文档所列 AMD 硬件和软件环境完成对应概念验证，仅证明已测项目与平台 |
| **NVIDIA 参考机已验证** | 已在 NVIDIA 参考环境生成并固化参考结果，用于数值/行为对照 |
| **目标平台已验证** | 已在目标厂商平台按交付场景验证；当前尚无此类结论 |
| **设计假设** | 有架构依据或局部实验支撑，但关键端到端链路尚未实现 |
| **待验证** | 尚无充分证据，必须作为后续阶段通过条件或待办跟踪 |

全文中的“PASS”仅表示对应测试项通过，不自动升级为端到端、完整 API、生产质量或目标平台验收通过。

### 0.2 结论摘要

| 关键结论 | 证据等级 | 含义 |
|---|---|---|
| VAAPI 可作为 NVDEC 兼容后端候选 | AMD-PoC | 选定格式及 AMD 测试支持主路线，不代表全部设备和码流能力已覆盖 |
| DMA-BUF 句柄/资源链路可打通 | AMD-PoC | 具备零拷贝设计基础；只把解码侧纳入当前交付 |
| 线性 surface 可作为共享资源优先路径 | AMD-PoC | 当前平台内容正确且未见明显性能回退，目标平台须复测 |
| 主机同步可作为首期基线 | AMD-PoC | 顺序与资源管理测试通过；真实并发流水线仍需验证 |
| 真实解码→HIP 读底层数据通路 | AMD-PoC | 实验 C 已闭合；不代表 CUVID shim 或 FFmpeg 宿主已验收 |
| DRM sync_file 跨栈异步同步 | 设计假设 | 基础原语可用，真实跨引擎 fence 传递/排序/异常未完成 |
| CUVID shim 数据通路 | AMD-PoC | 双后端四路码流各 82/82 逐字节；`verify_kernel_read` errors=0；FFmpeg 宿主矩阵修复后 32/32 PASS，含双后端零 D2D 与严格 PTS |
| NPP NVIDIA 参考基线 | NVIDIA 参考机已验证 | 3 个 P0 函数旧参考输出已固化；Stage A 新探针和 AMD HIP 实现已落地。P2P3/P3P2 双后端逐字节通过；resize NVIDIA golden 尚待生成 |
| NPP AMD Step A 集成 | AMD-PoC（部分） | 真实 NPP 头文件下 shim 构建成功；独立 nonfree FFmpeg 已加载 `scale_npp`；FFmpeg libnpp 被标记 nonfree，发布方案未决 |
| Vulkan Video 后端 | AMD-PoC | 解码→拷贝→导出→HIP 读取通过，已接入 CUVID；h264_nvdec 使用合成 SPS/PPS；与 VAAPI 同级候选 |
| 目标平台 | 待验证 | AMD 结果不能替代实际交付平台验收 |

### 0.3 测试环境

| 环境 | 硬件 | 软件 | 用途与限制 |
|---|---|---|---|
| **AMD 验证机** | Radeon RX 7900 XTX，gfx1100/Navi31，24GB | Ubuntu 24.04.2；kernel 6.11.0-17；ROCm 7.2.4 / HIP 7.2.53211；Mesa 25.2.8；libva 2.20.0（VA-API 1.20）；FFmpeg 7.1 官方源码自编译，含 `h264_cuvid`、`nvdec` hwaccel | 实验 C/E、P0、Vulkan/HIP 数据通路、FFmpeg 宿主 32 组合；NPP Stage A 仅本地自检，未接入 FFmpeg |
| **NVIDIA 参考机** | GeForce RTX 4060，Compute Capability 8.9 | Ubuntu 22.04.5；驱动 595.99.02；CUDA Driver API 13020；CUDA Runtime/NPP 11.5（NPP 11.5.1.107） | 3 个 P0 NPP 函数参考输出；不是 AMD 交付环境 |
| **历史格式对比机** | NVIDIA RTX 4060；AMD RX 9070（VCN5） | 精确驱动和构建环境未完整记录 | 仅作历史格式能力参考；不得与 P0 环境混写 |

不同批次不得共用“测试环境相同”的默认假设。单项实验如有额外依赖或环境差异，应在该实验记录中覆盖说明。

---

## 1. 当前状态：P0 闭环结果

### 1.1 结果

2026-11-12 AMD 验证机，两个后端 × 四路码流，合计 164 帧（每后端 82 帧），HIP 读回与软件解码参考逐字节一致：

| 码流 | 帧数 | Vulkan | VAAPI |
|---|---:|---|---|
| `testdata/h264/test_p_b_5.h264` | 9 | 9/9 bit-exact，errors=0 | 9/9 bit-exact |
| `testdata/h264/test_p_b_7.h264` | 13 | 13/13 bit-exact，errors=0 | 13/13 bit-exact |
| `testdata/h264/test_p_main.h264` | 30 | 30/30 bit-exact，errors=0 | 30/30 bit-exact |
| `testdata/h264/test_p_66.h264` | 30 | 30/30 bit-exact，errors=0 | 30/30 bit-exact |
| **合计** | **82** | **82/82** | **82/82** |

补充检查：

- `verify_kernel_read`：两个后端全部 `errors=0`。
- `make check`：PASS。
- `make check_func`：PASS。
- `make verify_cuvid_parser`：PASS。
- `make verify_cuvid_session`：PASS。

完整记录、复现命令和证据边界见 [P0 闭环验证结果](../evidence/P0闭环验证结果_2026-11-12.md)。

### 1.2 这批结果证明什么

- VAAPI 与 Vulkan Video 两条数据通路都能完成真实解码、显存导出/导入和 HIP 读取。
- 四路码流在两个后端上都与软件参考逐字节一致，覆盖 I/P/B、不同 profile 和无 B 帧对照组。
- `verify_kernel_read` 通过，说明 map 返回的 `dev_ptr` 能被 HIP kernel 直接按 pitch 寻址，并且 `hipEventSynchronize` 后再 unmap 的顺序正确。
- libcuda 符号面、功能子集、CUVID parser 与会话创建均通过。

### 1.3 这批结果不能证明什么

- FFmpeg 宿主组已补测：四路 320×240 码流、两后端、四种模式 32/32 PASS；`h264_nvdec` 零 D2D 依赖 `-extra_hw_frames`。这不等于真实业务线程/队列配置已验收。
- 尚未覆盖多流、seek/flush、动态分辨率、P010/奇数分辨率。
- 尚未覆盖自研目标平台、1/8 路 30 分钟稳定性、真实业务性能。
- `verify_kernel_read` 的 checksum 只证明“kernel 可寻址 + event 顺序”，不独立证明解码像素正确；像素正确性由四路码流表格证明。

### 1.4 P0 技术范围与验收边界

下表是实现和验证的范围基线，不代表表中所有项目已经通过；各阶段状态见 [§11.1](#111-阶段通过条件)。

| 维度 | P0 已确认范围 | 边界/验收口径 |
|---|---|---|
| OS/平台 | Ubuntu 24.04；GFX ISA，支持 AMD 私有 HSA API | AMD 验证机为 RX 7900 XTX；其他组合不自动视为受支持 |
| 验证宿主 | 官方源码自编译 FFmpeg 7.1 | 覆盖 `h264_cuvid` decoder 与 `nvdec` hwaccel；当前 320×240 四路矩阵通过；用户真实应用和业务场景移至 P1 |
| 解码格式 | H.264、4:2:0、8-bit、NV12 | 不扩展至其他 codec、位深或色度格式 |
| 码流能力 | I/P/B、DPB、crop、seek/flush、动态分辨率、长期参考 | 场/MBAFF 遇到时明确返回不支持；不静默输出错误帧 |
| CUVID API | 22 个动态加载符号形态；FFmpeg 直接使用的 9 个功能接口 | 清单见[CUVID 到 VAAPI 映射表](CUVID到VAAPI映射表.md) |
| NPP API | `nppiResizeSqrPixel_8u_C1R`、`nppiYCbCr420_8u_P2P3R`、`nppiYCbCr420_8u_P3P2R` 及必要 nppc | CUDA/NPP 11.5 ABI；resize 插值先做 FFmpeg 核心 cubic/NN/linear，再扩到全部 FFmpeg 选项和 NPP flags；与 NVIDIA 11.5.1.107 逐字节一致 |
| libcuda | FFmpeg 解码与 NPP 联合链路所需内部支撑 | 不作为独立完整 CUDA driver API 交付 |
| 数据通路 | LINEAR surface + DMA-BUF + HIP 导入，同进程 | 运行时校验 plane、pitch、offset、modifier 和 size |
| 同步 | `vaSyncSurface` + HIP 同步的主机同步基线 | DRM 异步 fence 不属于 P0 |
| 功能场景 | 1080p、4K；基础解码、seek/flush、crop/动态分辨率、1 路和 8 路 | 保留 320×240 快速回归；输出逐帧校验 |
| 稳定性 | 1 路和 8 路分别运行 30 分钟 | 无崩溃、死锁、非法访问或持续资源泄漏 |
| 性能 | 记录吞吐、p50/p95/p99 延迟、CPU/显存/fd | **P0 只记录不设通过阈值，不得宣称性能达标** |

### 1.5 当前非目标

- NVENC、视频编码及 HIP→VAAPI 编码链路。
- HEVC、AV1、VP9、MPEG-2、P010、10/12-bit、4:2:2、4:4:4。
- 完整 CUDA 二进制 drop-in、完整 driver/runtime、完整 CUVID 或完整 NPP。
- 任意用户应用、任意 CUDA kernel 的 HIPIFY 自动翻译，以及用户应用的 NPP 调用覆盖。
- 场/MBAFF、DRM 异步 fence、多 GPU 和跨进程 DMA-BUF 传递。
- Vulkan Video 与 VAAPI 为同级候选，尚未经书面选型；其底层解码和 FFmpeg 宿主 P0 矩阵已验证，但真实业务、复杂场景和目标平台仍未验收，不能视为已批准的 P0 交付承诺。
- 以 P0 结果替代生产性能、长稳、安全、运维和实际交付平台验收。

---

## 2. 底层数据通路与内存/同步实现及证据

> 完整契约见[内存与同步模型](内存与同步模型.md)。本节统一记录共享分层、帧映射和同步实现，以及对应实验事实与边界。

### 2.1 共享架构与后端边界

客户自研 CUDA kernel、`cudaMalloc` 和 kernel launch 由 HIPIFY 转成 HIP 后经 hipcc/HIP runtime 执行，保留该能力但不属于 P0 交付组件；NVIDIA 现成库由同名 shim 承接。HIPIFY 不能翻译 NVDEC/NVENC/NPP，因为 ROCm 没有对应库，这些 API 必须由 shim 重实现，详见 [HIPIFY 调研](HIPIFY调研.md)。

```text
┌──────────────────────────────────────────────────────────────┐
│ 宿主层：FFmpeg 7.1 h264_cuvid / h264_nvdec；客户应用          │
│ 加载方式：libcuda/libnvcuvid 走 dlopen+dlsym；NPP 走编译期链接 │
├──────────────────────────────────────────────────────────────┤
│ ABI 层：libcuda.so.1 / libnvcuvid.so.1 / libnpp*.so.11       │
│ 固定 SONAME、符号面、错误码、结构体布局、调用约定             │
├──────────────────────────────────────────────────────────────┤
│ CUVID 门面：句柄/状态/ctx lock/解码/映射/查询                 │
├──────────────────────────────────────────────────────────────┤
│ 解析层：CuvidParser（FFmpeg 9 个解析对象 + 自写状态机）        │
├──────────────────────────────────────────────────────────────┤
│ 后端接口：DecoderBackend（initialize/decode/map_frame/reset） │
│   ├─ VaapiBackend   → VaapiH264Decoder                       │
│   ├─ VulkanBackend  → VulkanH264Decoder                      │
│   └─（未来）Mesa frontend backend                             │
├──────────────────────────────────────────────────────────────┤
│ 帧映射层：HipFrameMapping（VA poll/export；VK fd import）      │
├──────────────────────────────────────────────────────────────┤
│ 计算/驱动层：HIP/HSA；Mesa radeonsi / RADV；VCN 视频引擎       │
└──────────────────────────────────────────────────────────────┘
```

分层的意义是**共享逻辑只写一遍，后端只实现差异**。parser/DPB、CUVID 门面、帧映射、同步契约、构建和诊断属于共享层；VAAPI、Vulkan、Mesa 只负责“解码会话 + 输出布局 + 导出方式”。两条现有路径最终都返回 `dev_ptr + pitch` 给 HIP kernel/NPP，并在 unmap 时执行 `hipDestroyExternalMemory + close(fd)`；VAAPI 直接导出 LINEAR surface，Vulkan 则先经 GPU 内 tiled→linear 拷贝再导出线性 buffer。

| 组件 | 职责 | 关键源码 | 当前状态 |
|---|---|---|---|
| `libcuda.so.1` | 设备/上下文/内存/流/事件映射到 HIP；launch/module/JIT/texture/GL stub | `cuda_shim/src/libcuda.cpp` | 82 符号导出；47 项功能子集 PASS；完整 ABI 未验收 |
| `libnvcuvid.so.1` | CUVID 符号面、句柄、解码、map/unmap、parser | `cuda_shim/src/libnvcuvid.cpp` | 21 必需符号 + caps；P0 四路双后端宿主矩阵 32/32；多 slice/长尾未验收 |
| `DecoderBackend` | 后端抽象与运行时选择 | `cuda_shim/src/decoder_backend.h`、`backend_dispatch.cpp` | 已落地；`CUVID_SHIM_BACKEND=vaapi / vk` |
| `VaapiH264Decoder` | VAAPI 解码、LINEAR surface | `cuda_shim/src/vaapi_h264_decoder.cpp` | H.264/420/8bit/NV12 可用；profile 映射与部分语义待补 |
| `VulkanH264Decoder` | Vulkan Video 解码、拷贝、导出 | `cuda_shim/src/vulkan_h264_decoder.cpp` | H.264 数据通路可用；多流/reconfigure 未覆盖 |
| `CuvidParser` | SPS/PPS/slice 解析、DPB、显示序 | `cuda_shim/src/cuvid_parser.cpp` | I/P/B 四路码流通过；多流/seek/crop 待补 |
| `HipFrameMapping` | VAAPI/VK 导出 fd → HIP 导入、布局门禁 | `cuda_shim/src/hip_frame_mapper.cpp` | VAAPI/VK 路径均通过；fd 所有权已处理 |
| `libnpp*.so.11` | 3 个 NPP 函数 + nppc | Stage A HIP 实现和 5 个 SONAME 库已落地 | ⚠️ 本地色度自检通过；NVIDIA golden、FFmpeg 链接和联合链路未通过 |

### 2.2 零拷贝可行性实验

| 问题 | 结论 | 关键证据 |
|---|---|---|
| HIP 与 VAAPI 能否同进程共存 | ✅ 已证实 | 实验 1；所有零拷贝工作的前提 |
| 边界 1：VAAPI surface → HIP | ✅ 已证实 | `vaExportSurfaceHandle(DRM_PRIME_2)` → `hipImportExternalMemory(OpaqueFd)` → `GetMappedBuffer` |
| 边界 2：HIP → VAAPI | ✅ 已证实 | HIP 侧 `hsa_amd_portable_export_dmabuf` → `vaCreateSurfaces(DRM_PRIME_2)` |
| 边界 2 的坑 | 线性 DMA-BUF 的 pitch 必须按平台要求对齐 | 当前 AMD 平台为 256；不能写死 |
| 同进程“解码→HIP 读→编码”闭环 | 解码→HIP 读已闭环；编码方向组件可行、链路未验收 | 实验 C；编码不属于当前解码调研范围 |

边界 2 定位过程：

- 初次失败：导出 fd 成功，但 `vaCreateSurfaces` 返回 `status=2 (resource allocation failed)`。
- 定位：VAAPI 自建 surface 为 tiled+DCC（modifier `0x200000028a01f04`），非线性；原程序把 separate-layers 误当 composed。
- 再定位：R3 往返自检 PASS；R4 线性导入测不同 pitch：

| pitch | 结果 |
|---|---|
| 256 / 512 / 1024 | ✅ PASS |
| 320（非 256 对齐） | ❌ FAIL |

**结论**：边界 2 失败的直接原因是 pitch 对齐，不是“VCN 不吃线性内存”。生产实现必须运行时查询，不能写死。

### 2.3 实验 C：真实解码 → HIP 读

`tests/zerocopy/verify_decode_to_hip.cpp` 在 AMD 平台重跑 I-only、I/P、I/P/B 三种码流，均为 30 帧逐字节一致，并通过池复用、flush 和 fd 检查。

| 码流 | 帧数 | 检查项 | 结果 |
|---|---:|---:|---|
| I-only | 30 | 11/11 | 逐字节一致 |
| I/P | 30 | 12/12 | 逐字节一致，含参考帧复用 |
| I/P/B | 30 | 12/12 | 逐字节一致，含 B 帧显示序重排 |
| flush | — | 所有 surface | 幂等检查通过 |
| fd | 1000 帧级 | 初始/最终 | 无泄漏 |

布局实测：LINEAR，Y pitch=512、offset=0；UV pitch=512、offset=122880（= `pitch * height`），同一 object，满足连续 NV12 条件。

**实验 C 证明**：libva 真实解码后的像素，经过线性 surface 导出和 dma-buf 导入，可以被 HIP 逐字节正确读取；线性直通和 pitch/offset 处理成立。

**实验 C 不证明**：它不是 `libnvcuvid.so.1` 的端到端验证，没有经过 `cuvidCreateDecoder`/`cuvidDecodePicture`/`cuvidMapVideoFrame64` 的 shim 路径，也没有经过 FFmpeg 的 `h264_cuvid`/`h264_nvdec` 宿主。

### 2.4 实验 C 沉淀的实现约束

1. **profile 映射不能照抄 VAAPI 枚举名**：`VAProfileH264Baseline` 已废弃，radeonsi 只暴露 `VAProfileH264ConstrainedBaseline`；Baseline（`profile_idc=66`）必须映射到后者，否则 `vaCreateConfig` 失败。
2. **导出的 NV12 可能是 separate layers**：`VADRMPRIMESurfaceDescriptor.num_layers` 可能等于 2，UV 在 `layers[1].offset`；shim 必须运行时检查 layer 结构，不能假设 composed。
3. **Y/UV 都必须按 pitch 逐行拷贝**：pitch 可能大于宽度（本例 512 vs 320），整块 `hipMemcpy` 会错位；`cuvidMapVideoFrame64` 返回的 plane 指针/pitch 必须逐行处理。
4. **线性创建不能依赖 `AMD_DEBUG`**：生产应使用 `VASurfaceAttribDRMFormatModifiers=[DRM_FORMAT_MOD_LINEAR]`；实测 `vaCreateSurfaces` 成功、导出 modifier=0x0、pitch=512、Y offset=0、UV offset=122880，`vaCreateContext` 成功。
5. **P010 布局**（非 P0）：pitch = `W×2` 向上对齐 256（320×2=640→768）；UV offset = `pitch×H`；每像素 2 字节。仅作当前 AMD 样本参考，不能替代运行时查询。

### 2.5 线性 vs tiled

`tests/zerocopy/run_linear_vs_tiled.sh` 与 `run_multires_linear.sh` 结果：

| 维度 | tiled（默认） | 线性（`AMD_DEBUG=notiling`） |
|---|---|---|
| 导出的 modifier | `0x200000028a01f04`（tiled+DCC） | `0x0`（线性） |
| 解码 vs 软件逐字节 | ✅ 一致 | ✅ 一致 |
| 320×240 解码耗时 | 0.16s | 0.15s |
| 640×360 | 0.17s | 0.17s |
| 1920×1080 | 0.29s | 0.29s |
| 3840×2160 | 0.70s | 0.69s |

**结论**：当前 AMD 测试条件下，线性直通未观察到明显耗时回退；但现有数据未记录重复次数、统计波动和多路并发，不能外推为普遍性能结论。生产实现必须用 `VASurfaceAttribDRMFormatModifiers` 主动限定 LINEAR，不能用 debug 开关。

### 2.6 帧映射统一实现与布局门禁

`cuvidMapVideoFrame64` 必须返回一个 HIP kernel 能直接使用的设备指针，且该指针在 unmap 前保持有效、内容完整。这里起作用的是 **UVA + map/unmap 契约**，不是 `cudaMallocManaged` 的 UVM。

**VAAPI 路径：**

1. `vaSyncSurface` 等待解码完成。
2. `vaExportSurfaceHandle(DRM_PRIME_2, READ_WRITE)`；先试 `SEPARATE_LAYERS`，失败回退 `COMPOSED_LAYERS`。
3. 检查 `fourcc == NV12`，读取 `num_objects/num_layers/pitch/offset/modifier/size`。
4. 对每个 object 执行 `hipImportExternalMemory(OpaqueFd)` + `hipExternalMemoryGetMappedBuffer`。
5. 计算 Y/UV 基址；校验 `y_object == uv_object` 且 `uv_offset == pitch * height`；否则返回 `CUDA_ERROR_NOT_SUPPORTED`。
6. 返回 `dev_ptr + pitch`；unmap 时销毁导入对象并 close fd。

**Vulkan 路径：**

1. `decode()` 末尾已主机等待 `copy_fence`。
2. `vkGetMemoryFdKHR(OPAQUE_FD)` 导出线性 buffer fd。
3. `map_frame()` 把 fd 交给 `HipFrameMapping::map_fd()`。
4. `map_fd()` 只做 HIP 导入、`range_valid` 校验，并假定 Y/UV 连续（UV offset = pitch × height），这正是 `vkCmdCopyImageToBuffer` 写出的布局。

`HipFrameMapping::range_valid` 检查 offset/pitch/rows/row_bytes/object_size，防止整数溢出和越界。只有 Y/UV 能由同一 HIP 映射基址及合法 pitch/offset 表达时，才声明 mapped-frame 零拷贝；其他布局显式降级，不得混报。

当前 P0 四路码流上，两后端 `h264_nvdec` 零拷贝均为 D2D=0 且逐字节一致；宿主必须为 `unsafe_output` 分配额外 surface（测试使用 `-extra_hw_frames 16`，shim 上限 64），否则 FFmpeg frame pool 会报 `No decoder surfaces left`。`h264_cuvid` 的 `cuMemcpy2DAsync` 是 FFmpeg 自身行为，必须单独记录。跨进程 DMA-BUF、多 GPU 不在 P0；生产实现必须运行时查询 pitch/offset/modifier，不能写死 256 对齐。

### 2.7 同步协议 S1–S8

`tests/sync/verify_sync` 验证顺序与资源管理，不校验像素：

| 用例 | 内容 | 结果 |
|---|---|---|
| S1 | 解码后 HIP 读前 `vaSyncSurface` | PASS |
| S2 | HIP 后编码前 `hipEventSynchronize` | PASS |
| S3 | surface 池复用 | PASS |
| S4 | DPB 生命周期（I-P-P-B） | PASS |
| S5 | pending 时 flush | PASS |
| S6 | pending 时 destroy | PASS |
| S7 | 多线程 push/pop | PASS |
| S8 | 1000 帧长稳查 fd 泄漏 | PASS；fd 初始=10、最终=10 |

**边界**：S1/S2 是“模拟提交 + 等待原语”的协议验证，不是真实解码/编码；S3–S7 主要验证池复用、DPB 引用、flush/destroy 和多线程；真实像素由实验 C 单独证明。该测试不代表 1 路/8 路 30 分钟业务稳定性已经通过。

### 2.8 同步基线与三类引用

| 边界 | 做法 |
|---|---|
| 解码 → map | VAAPI：`vaSyncSurface`；Vulkan：`vkWaitForFences(copy_fence)` |
| map → HIP kernel | map 之后提交 kernel |
| HIP kernel → 编码/回读 | `hipEventRecord` + `hipEventSynchronize`（或 stream 同步） |
| 多线程访问 | `cuvidCtxLock/Unlock`（宿主递归 mutex） |
| `output_stream` | 未真正接入；map 是同步的，比 CUDA 异步 map 语义更强 |

资源回收必须同时考虑三类引用：

- **DPB 引用**：参考帧在参考列表里，不能被新解码覆盖；
- **mapped frame 引用**：map 到 unmap 之间，不能被复用；
- **NPP/计算输入引用**：下游 kernel 未读完，不能提前 unmap。

三类引用任一未释放，surface 就不能回收。`tests/sync` 的 S3–S6 围绕这个协议验证；`cuvidDecodePicture` 还必须拒绝写入已 map 的 slot。

### 2.9 异步 fence 探测

`tests/sync/verify_semaphore_async.cpp`：

| 探测项 | 结果 |
|---|---|
| `timelineSemaphoreInteropSupported` | 0（不支持） |
| `hipImportExternalSemaphore(TimelineSemaphoreFd)` | invalid argument（Linux 不实现） |
| `DRM_CAP_SYNCOBJ` / `DRM_CAP_SYNCOBJ_TIMELINE` | 1 / 1 |
| syncobj 创建 | ✅ |
| syncobj → sync_file 导出 | ✅ fd=6 |
| sync_file → syncobj 导入 | ✅ handle=2 |
| 导入后 WAIT(timeout=0) 立即 signal | ✅ |

**结论**：

- 当前 ROCm/HIP 不支持 Linux 外部 semaphore；异步同步设计不采用 HIP 侧 semaphore。
- DRM syncobj/sync_file 基础原语可用，为跨栈 fence 提供候选。
- 尚未验证 VAAPI 视频引擎与 ROCm 计算引擎之间的真实 fence 传递、排序和异常处理；属于厂商/Mesa/ROCr 协同项。
- Vulkan 侧不应修 `hipImportExternalSemaphore`（opaque fd 跨栈不互认）；正解是 `VK_KHR_external_fence_fd` → sync_file → syncobj → HIP queue wait fence。

候选桥接路径如下，但尚未落地，属于厂商/Mesa/ROCr 协同项，不阻塞 P0 功能检查：

```text
VAAPI/Vulkan fence → external fence fd / sync_file
  → DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE → amdgpu syncobj
  → ROCr 把 syncobj 挂成 HIP queue wait fence
```

---

## 3. 加载机制、ABI 与公共门面实现

### 3.1 FFmpeg 的加载方式

- FFmpeg 二进制不链接 libcuda（`ldd` 无、`nm -D` 无 `cuInit`）。
- 全部通过 `compat/cuda/dynlink_loader.h` → `#include <ffnvcodec/dynlink_loader.h>` → `dlopen + dlsym`。
- LIBNAME 硬编码：`libcuda.so.1` / `libnvcuvid.so.1` / `libnvidia-encode.so.1`（Linux）。
- 解码器 `init` 时一次性 `dlsym` 全部符号。
- `cuda_load_functions` 用 `LOAD_SYMBOL` 加载 **82 个必需符号**，用 `LOAD_SYMBOL_OPT` 加载约 20 个可选符号。即使只用 nvdec，82 个必需符号也必须导出。
- NPP 则是**编译时链接**，不适用上述 dlopen 推论。

本项目采用源码级重编译兼容，不承诺现有预编译二进制直接替换运行。即使 FFmpeg 被重新编译，其 CUVID/CUDA 路径仍通过 `dlopen/dlsym` 加载 shim，因此固定 SONAME、必需符号、函数签名、回调和相关结构体布局仍须与构建时锁定的头文件一致；这不等于完整 CUDA 二进制 drop-in。

### 3.2 共享加载与 SONAME 契约

| 产物 | 加载方式 | 硬约束 |
|---|---|---|
| `libcuda.so.1` | FFmpeg `dlopen + dlsym` | 82 个必需符号全部导出；缺失任一即加载失败 |
| `libnvcuvid.so.1` | FFmpeg `dlopen + dlsym` | 固定 SONAME；22 个符号形态；`cuvidGetDecoderCaps` 可选 |
| `libnpp*.so.11` | 编译期链接 | SONAME 必须是 `libnpp<子库>.so.11`；导出符号名与 NVIDIA 一致 |

### 3.3 符号面结果与错误语义

| .so | 必需符号数 | 结果 |
|---|---|---|
| `libcuda.so.1` | 82（+20 可选） | 全部导出正常；`verify_symbols` PASS |
| `libnvcuvid.so.1` | 22 | 21 必需符号 + `cuvidGetDecoderCaps` 可解析；`verify_cuvid_symbols` PASS |
| `libnvidia-encode.so.1` | 2 | 形态明确；不在 P0 |

**符号导出 ≠ 功能完成**。CUDA 版本号、Compute Capability、错误码需要虚拟化。返回类型统一 `CUresult`（`CUDA_SUCCESS=0`）；不支持的能力必须返回 `CUDA_ERROR_NOT_SUPPORTED`（**801，不是 201**），不得返回伪成功。`VAStatus`/`hipError_t` 到 `CUresult` 的映射集中在 `cuda_shim/include/cuda_shim_types.h` 和 `hip_frame_mapper.cpp`，实现时使用显式 switch，不依赖数值巧合。

### 3.4 libcuda 实现与功能子集

`libcuda` 是 HIP 运行时的薄映射 + 显式不支持 stub：

| 组 | 代表符号 | 实现策略 |
|---|---|---|
| 设备/上下文（22） | `cuInit`、`cuDeviceGet`、`cuCtxCreate`、`cuCtxGetCurrent`、PrimaryCtx 5 个 | HIP 直接映射 |
| 内存（28） | `cuMemAlloc`、`cuMemAllocPitch`、`cuMemHostAlloc`、`cuMemcpy2DAsync`、Memset/Memcpy | HIP 直接映射 |
| 流/事件（12） | `cuStreamCreate`、`cuStreamSynchronize`、`cuEventRecord`、`cuStreamWaitEvent` | HIP 直接映射 |
| launch/模块/JIT（10） | `cuLaunchKernel`、`cuModuleLoadData`、`cuLink*` | stub；视频路径不走 |
| texture/array（6） | `cuTexObjectCreate`、`cuArrayCreate` | stub `NOT_SUPPORTED` |
| GL interop（7） | `cuGraphicsGLRegisterImage` 等 | stub |

实现约束：

1. 用 `nv-codec-headers` 的 `dynlink_cuda.h` 定义 ABI，同一编译单元包含 HIP 头时加 `-D__HIP_PLATFORM_AMD__`。
2. 设备/上下文/内存/流/事件逐项映射；句柄用 `reinterpret_cast`，`CUdeviceptr` 用 `uintptr_t` 中转。
3. 版本号、Compute Capability 等语义值需虚拟化，不能直接透传 HIP/ROCm 值。
4. 未实现能力返回 `CUDA_ERROR_NOT_SUPPORTED`；不得返回成功或伪造结果。

`test_func_full.cpp` 覆盖设备/上下文/内存/2D 拷贝/流事件/primary ctx，带数据校验：**47 项 PASS / 0 FAIL**（历史记录）。实测暴露并修复的缺陷：

| # | 问题 | 根因 | 修复 |
|---|---|---|---|
| 1 | `cuDriverGetVersion` 返回 `70253211` | 直接透传 ROCm 版本 | 返回 CUDA 语义 12000 |
| 2 | `cuCtxSynchronize` 返回 NotSupported | `hipCtxSynchronize` 在 ROCm 7.x 废弃 | 改用 `hipDeviceSynchronize()`（语义更强） |
| 3 | `cuDevicePrimaryCtxGetState` 返回 INVALID_DEVICE | CUDA 真实签名第一个参数按值传 `CUdevice`，原声明成指针 | 改按值传 |
| 4 | 构建/链接问题 | 缺 `libcuda.so` 开发软链；ROCm 路径写死；test 目标未依赖库 | Makefile 自动探测、建软链、加依赖 |

**结论**：符号面、功能、ABI 是三件事，必须分别验证；82 符号/47 项功能是历史子集，不代表完整 driver API/ABI。`cuLaunchKernel` 是 stub，CUDA 计算不走 `libcuda`，由 HIPIFY 后的 HIP runtime 执行。UVM 只做 `hipMallocManaged` 薄映射，未按 NVIDIA UVM 语义验证；默认流语义、IPC/VMM/外部 semaphore 不在已验收范围。

### 3.5 libnvcuvid 门面与关键 API

`libnvcuvid` 只承担三项公共职责：

1. **符号面**：导出 22 个 CUVID 符号，满足 FFmpeg `dynlink_loader` 一次性 dlsym。
2. **句柄与状态**：`DecoderState` 持有后端、mutex、映射表、状态表；parser 句柄持有 `CuvidParser`。
3. **契约转换**：把 `CUVIDDECODECREATEINFO`/`CUVIDPICPARAMS`/`CUVIDPROCPARAMS` 转成后端调用，把 `dev_ptr + pitch` 返回给宿主。

| API | 行为 | 当前状态 |
|---|---|---|
| `cuvidGetDecoderCaps` | H.264/420/8bit caps；其他返回不支持 | 静态 caps，未按目标平台 `vaQueryConfigAttributes` 动态探测 |
| `cuvidCreateDecoder` | 建 `DecoderState`，调 `backend->initialize` | 通过 |
| `cuvidDecodePicture` | 检查 `CurrPicIdx`、已 map 状态，调 `backend->decode` | 通过 |
| `cuvidGetDecodeStatus` | 返回内部状态表 | 当前不主动 `vaSyncSurface`；与映射表文档描述有差异，列入缺口 |
| `cuvidReconfigureDecoder` | 返回 `NOT_SUPPORTED` | 两个后端均为 stub |
| `cuvidMapVideoFrame64` | 检查场参数、调 `backend->map_frame`、布局门禁 | 通过 |
| `cuvidUnmapVideoFrame64` | 删除 mapping，析构时释放导入对象和 fd | 通过 |
| source 层 7 个符号 | 返回 `NOT_SUPPORTED` | 加载表要求存在，功能不承诺 |
| `cuvidCtxLock*` | 宿主递归 mutex | 通过 |

### 3.6 句柄、map/unmap 生命周期与线程配置

- 用 `extern "C"` 导出符号，签名与 nv-codec-headers 完全一致；用 `std::unordered_map` 维护 decoder 句柄 → `DecoderState`，创建/销毁加全局 mutex。
- 每个 `CUvideodecoder` 对应一个 `DecoderState`：后端实例、mutex、mappings、mapped_indices、statuses；内部 mutex 串行化 decode/map/unmap。
- `map` 成功后记录 `mapped_indices[PicIdx] = dev_ptr`；`cuvidDecodePicture` 拒绝解码到已 map 的 slot，并返回 `INVALID_VALUE`，避免下游还在读时被写回。
- `cuvidMapVideoFrame64` 返回 `dev_ptr + pitch`；unmap 前该指针有效，surface 不得复用。
- `cuvidUnmapVideoFrame64` 按 `dev_ptr` 找到并删除 mapping，析构时 `hipDestroyExternalMemory` 并 `close(fd)`。
- `cuvidCtxLock/Unlock` 是宿主递归 mutex，不是 HIP context lock；只保护 shim 内部临界区。
- `CUVID_SHIM_BACKEND` 选择后端；`CUVID_SHIM_DRM_NODE` 指定 DRM 节点；其余调试开关见[附录 A](#附录-a环境变量与诊断开关)。
- 所有不支持路径返回明确错误，source 层不得伪造成功；配置只影响诊断/实验，不得静默改变用户请求的处理语义或像素结果。

---

## 4. parser / DPB 实现与证据

### 4.1 为什么必须自建 parser

NVDEC 硬件可以吃完整码流自解析：FFmpeg `nvdec.c` 调 `cuvidDecodePicture` 时，`CUVIDPICPARAMS` 只填 bitstream 字段，`CodecSpecific` 留空。而 VAAPI 的 VLD 必须由 host 预解析成 `VAPictureParameterBuffer*` + `VASliceParameterBuffer*` + slice data；Vulkan Video 也需要 SPS/PPS、slice header 和 POC。因此 parser 绕不开。

解析层复用 FFmpeg，填充层自写：SPS/PPS → `CUVIDPICPARAMS.CodecSpecific.h264` / `VAPictureParameterBufferH264`；slice header → `VASliceParameterBufferH264` / `StdVideoDecodeH264PictureInfo`。只链接 9 个解析 `.o` 和共享 `libavutil.so`，不要链整个 `libavcodec.a`。

### 4.2 复用路径

| 环节 | FFmpeg 对应代码 | 复用方式 |
|---|---|---|
| SPS/PPS 解析 | `h264_ps.c` 的 `ff_h264_decode_seq_parameter_set` / `ff_h264_decode_picture_parameter_set` | ✅ 静态链接 FFmpeg 对象 |
| VAAPI 参数填充 | `vaapi_h264.c` 的 `fill_vaapi_pic` / `fill_vaapi_ReferenceFrames` | ❌ 全是 static 且依赖 `H264Context`，不能直接提取；移植字段映射自写 |

`libnvcuvid.so.1` 链接 FFmpeg 7.1 的 **9 个解析对象**：`h2645_parse` / `h264_ps` / `h2645_vui` / `h2645data` / `h264data` / `h264_parse` / `golomb` / `mathtables` / `refstruct`，以及共享 `libavutil.so`。

### 4.3 本机验证

`tests/parser/verify_parser_reuse.cpp`：

| 步骤 | 结果 |
|---|---|
| 静态链接 `ff_h2645_packet_split` / `ff_h264_decode_seq_parameter_set` / `ff_h264_decode_picture_parameter_set` | ✅ |
| 切 NAL（91 个） | ✅ |
| 解析 SPS（30 个），字段正确 | ✅ profile=66（Baseline）、320×240、8bit、4:2:0 |
| 解析 PPS（30 个） | ✅ |
| 自填 `VAPictureParameterBufferH264` | ✅ |

### 4.4 状态机与公共实现约束

```text
Annex-B 数据包
  → 累积 buffer → ff_h2645_packet_split
  → 识别 SPS/PPS/AUD/SEI/slice
  → SPS/PPS 就绪 → pfnSequenceCallback(CUVIDEOFORMAT)
  → slice：parse_slice_header → frame_num / POC / is_reference / is_idr
  → pick_decode_surface（跳过 DPB 与待显示队列占用的槽）
  → 填 CUVIDPICPARAMS（含 DPB 快照）→ pfnDecodePicture
  → update_dpb（参考帧）
  → 插入 display_queue（按 POC 有序）→ pfnDisplayPicture
  → EOS/IDR flush → pump_display_queue
```

1. **surface 数量**：`min_num_decode_surfaces = ref_frame_count + 3`，由宿主 +3 使用。面池太小会导致参考帧被覆盖，输出退化为参考帧拷贝。
2. **surface 选择**：不要简单轮转 32 个槽。radeonsi VCN 的 `render_pic_list` 有上限，实测第 16 个不同输出面开始丢；当前实现把活跃窗口限制为 12，并跳过 DPB/待显示槽。这是 **VAAPI 后端约束**；当前 parser 由两个后端共享，Vulkan 后端也继承了该限制，后续可按后端配置，不应把它当作 Vulkan 的必要约束。
3. **POC**：IDR 必须清 `prev_poc_msb/lsb` 和 DPB；poc_type=0 做 MSB 回绕；poc_type=2 用 `frame_num*2`。`log2_max_pic_order_cnt_lsb_minus4` 必须减 4，传完整位数会让硬件多读 4 bit。
4. **DPB**：只对 `nal_ref_idc != 0` 的参考帧执行滑动窗口；对 B 帧也执行会误删参考帧。MMCO、长期参考、IDR 清空要分开处理。
5. **显示序**：`pfnDisplayPicture` 必须按显示顺序回调，不是解码顺序；用 POC 有序插入队列，只放出 POC 小于“刚解出帧”的条目；EOS/IDR 时 flush。
6. **SPS/PPS 字节语义**：Vulkan 的 `VkSequenceCache` 必须存 `nal.raw_data`（保留 EPB），存去转义后的 `nal.data` 会让 `00 00 01` 被误判为 start code。
7. FFmpeg 内部头和字段访问的编译约束见 [§4.5](#45-实测踩坑)。

### 4.5 实测踩坑

1. **不能链整个 `libavcodec.a`**：会拖入所有编解码器的外部依赖（aom/zlib/…）导致链接失败；只提取 9 个 `.o`。
2. **`AVCodecContext*` 不能传 NULL**：内部会解引用 `avctx->codec_tag`/`flags2`；栈上 zero-init 一个即可。
3. **SPS/PPS 要从 `sps_list[]`/`pps_list[]` 取**，不是 `ps.sps`/`ps.pps`（后者只在 PPS/slice 解析时才赋值）。
4. **PPS 字段名与直觉不同**：`cabac`（非 `entropy_coding_mode_flag`）、`deblocking_filter_parameters_present`（非 `deblocking_filter_control_present_flag`）。
5. **必须 `#define HAVE_AV_CONFIG_H 1`**：否则 `libavutil/common.h` 不会 include `internal.h`，`golomb.h` 里的 `SUINT` 未定义。

### 4.6 I/P/B 与 DPB 复用

实验 C 的 I/P/B 版验证：

- I-only（11/11）、I/P（12/12）、I/P/B（12/12，1 I + 10 P + 19 B，含显示序重排）。
- 复用层：`ff_h264_init_poc`（POC，poc_type=0/2）+ `ff_h264_parse_ref_count`。
- 手写层：最小 DPB 状态机（MMCO_SHORT2UNUSED + 滑动窗口 + IDR 清空 + surface 池复用）+ RefPicList0/1 构建（P 按 frame_num 降序，B 按 POC 排序）+ list0/1 重排 + 显示序重排。

关键坑：

1. `h264_vaapi` 在 `bframes=0` 时强制 `poc_type=2`，`bframes>0` 时走 `poc_type=0`。
2. 目标机无 libx264 时，用 `h264_vaapi -profile:v 77` 生成 Main profile 码流。
3. **`log2_max_pic_order_cnt_lsb_minus4` 少减 4**：FFmpeg `sps->log2_max_poc_lsb` 存完整位数，VAAPI 字段名含 minus4 且硬件按 `值+4` 解读；直接传完整值会让 slice 数据整体错位，poc_type=0 下连 IDR 都错。
4. **滑动窗口误删参考帧**：滑动窗口只在 `nal_ref_idc != 0` 的参考帧解码后执行；对 B 帧也执行会误删 DPB 最旧 IDR，后续 B 帧缺参考（差异 5-15%）。
5. **surface 选择**：radeonsi VCN 的 `render_pic_list` 有上限；实测第 16 个不同输出面开始丢，表现为 `vaSyncSurface` 后整面全 0。当前 parser 把活跃窗口限制为 12，并跳过 DPB/待显示槽。
6. **IQ matrix 必须填**：不填会让驱动反量化系数为 0，输出平坦 128 灰。
7. **IDR 要 flush 显示队列并清 DPB**：否则全 IDR 流会攒到 EOS，surface 被反复覆盖。
8. **`pfnDisplayPicture` 必须按显示顺序**：按解码顺序回调会让 B 帧序列错乱。

**边界结论**：I/P/B + DPB + 双参考列表 + 显示序重排已证明正确；长期参考、场/MBAFF、crop、seek 仍需独立实现验证。

### 4.7 当前实现边界

- 只实现 H.264；HEVC/VP9/AV1 的 CUVID 结构体映射表保留在[CUVID 到 VAAPI 映射表](CUVID到VAAPI映射表.md)，但未实现。
- 场/MBAFF 明确返回不支持。
- 多流、seek/flush、crop、动态分辨率、长期参考未验收。
- `CuvidParser` 当前是单序列状态；多流需要按 decoder/stream 键控。

---

## 5. Vulkan Video 实现与证据

> 后端实现、根因、修复清单和移植指南见[Vulkan 后端实现](Vulkan后端实现.md)；能力评估与编码风险见[Vulkan Video 方案评估](Vulkan_Video方案评估.md)。本节合并公共实现之外的 Vulkan 差异、实测结果与未验收边界。

### 5.1 定位、前置条件与组件

- **当前定位**：与 VAAPI 同级的候选后端；运行时通过 `CUVID_SHIM_BACKEND=vk` 显式选择，不代表方案选型。有跨平台潜力，但多一次 GPU 内拷贝。路线决策见[总体方案](CUDA兼容层总体方案.md)。
- **前置条件**：Vulkan loader；`VK_KHR_video_queue`、`VK_KHR_video_decode_h264`、`VK_KHR_video_maintenance1`、`VK_KHR_external_memory_fd`；decode + transfer 队列族；支持 NV12 和 OPTIMAL tiling；HIP 可导入 OPAQUE_FD。
- **不适用**：驱动不暴露视频队列/解码扩展；需要严格零拷贝；编码路径不在 P0。

| 模块 | 职责 | 关键源码 |
|---|---|---|
| `VulkanH264Decoder` | instance/device/session/图像/命令/导出 | `cuda_shim/src/vulkan_h264_decoder.cpp` |
| `VkSequenceCache` | parser ↔ decoder 的 SPS/PPS 交接 | `cuda_shim/src/vk_sequence_cache.h` |
| `CuvidParser` | SPS/PPS stash、slice/POC | `cuda_shim/src/cuvid_parser.cpp` |
| `HipFrameMapping::map_fd` | fd → HIP 导入 | `cuda_shim/src/hip_frame_mapper.cpp` |
| `backend_dispatch` | `CUVID_SHIM_BACKEND=vk` 选择 | `cuda_shim/src/backend_dispatch.cpp` |

### 5.2 能力探测（V1–V6）

| 验证项 | 结果 | 关键发现 |
|---|---|---|
| V1 视频队列 | ✅ | DECODE/ENCODE 队列族默认可用；是否需 `RADV_PERFTEST` 未做同版本 A/B |
| V2 编解码能力 | ✅ | H264 解码 4096×4096/DPB17/DISTINCT；HEVC 8192×4352；H264 编码 CBR/VBR/CQP |
| V3 视频格式 | ⚠️ | 支持 NV12，但只支持 OPTIMAL tiling，不支持 LINEAR |
| V4 会话对象 | ✅ | 会话可创建、内存需求可查、可销毁 |
| V6 外部信号量 | ❌ | Vulkan 可导出 fd，但 `hipImportExternalSemaphore` = operation not supported |

结论：Vulkan Video **编解码能力完整**，可作与 VAAPI 同级的候选；OPTIMAL/tiled 输出使严格零拷贝受限，HIP 不能按线性直接读，需额外 GPU 转换；当前 HIP 不支持外部 semaphore，只能主机同步，sync_file/syncobj 是候选。选型前不设降级关系。

### 5.3 结构性约束

V3 只报告 OPTIMAL、不支持 LINEAR，是两层硬约束叠加：

1. **规范层**：Vulkan Video 要求解码输出图与 DPB 以 `VK_IMAGE_TILING_OPTIMAL` 创建，API 无“请求 LINEAR”字段。
2. **寻址层**：tiled（含 swizzle、可能带 DCC）的“解”只有纹理单元能做；HIP/CUDA 普通设备指针是平坦线性地址，导入 tiled 显存后按线性读必然错位。external memory 只解决“指向同一块物理内存”，不改变内存内数据排列。

候选技术路径的边界：

| 路线 | 结论 |
|---|---|
| 改 Mesa 出 LINEAR | 不推荐：违反规范、validation layer 拒绝、进不了上游，只能永久维护私有 fork |
| 改 `hipImportExternalMemory` 读 tiled | 走不通：tiling 在硬件寻址层，且 VK 布局不透明 |
| 接受一次 tiled→linear 转换 | 当前实现：`vkCmdCopyImageToBuffer` 硬件拷贝，合规、可上游、无需自写 compute shader |

### 5.4 端到端闭环与调用时序

`tests/zerocopy/verify_vk_decode_to_hip` 在 AMD RX 7900 XTX 跑通“VK 解码→`vkCmdCopyImageToBuffer`→`vkGetMemoryFdKHR`→`hipImportExternalMemory` 读回”，I/IP/IPB 各 30 帧逐字节一致，fd/显存无泄漏。

已验证的关键实现点：

1. decode 队列族无 TRANSFER 能力，`vkCmdCopyImageToBuffer` 须在 transfer 队列跑，两队列间用 semaphore（sync2）桥接，image 用 CONCURRENT 共享。
2. RADV（VCN<5）要求 DPB 与 output 图像分离，DPB 图只带 `VIDEO_DECODE_DPB` 用法。
3. `pSetupReferenceSlot` 每帧必填（含 B 帧），非参考帧用临时 slot。
4. 驱动内部按 FrameNum+POC 自行排参考列表，应用无需手写 RefPicList0/1 重排，但需维护 active 参考集合（MMCO+滑动窗口+IDR 清空）以填 `pReferenceSlots`。
5. 码流 buffer 按 Annex-B（含 start code）单 slice 复用，`srcBufferRange` 对齐 `minBitstreamBufferSizeAlignment`。

完整调用时序：

```text
cuvidCreateDecoder
  → initialize(): 校验参数；建 instance/device；选 decode/transfer 队列
  → 不建 session（SPS/PPS 未到）

cuvidParseVideoData
  → CuvidParser 解析 SPS/PPS → VkSequenceCache::store(raw_data)
  → pfnSequenceCallback / pfnDecodePicture

cuvidDecodePicture
  → ensure_session(): SPS/PPS 就绪后建 video session/session params/
    图像池/导出 buffer/同步对象
  → parse_slice_header + POC
  → 填 StdVideoDecodeH264PictureInfo + reference slots
  → vkCmdBeginVideoCodingScope / vkCmdDecodeVideoKHR / vkCmdEnd...
  → decode queue 提交，signal decode_done

decode() 末尾
  → transfer queue wait decode_done
  → image barrier → vkCmdCopyImageToBuffer（Y/UV）
  → signal copy_done + copy_fence
  → vkWaitForFences(copy_fence)
  → vkGetMemoryFdKHR(OPAQUE_FD) 存入 exports_[PicIdx].fd

cuvidMapVideoFrame64
  → map_frame(): fd 转给 HipFrameMapping::map_fd()
  → hipImportExternalMemory + GetMappedBuffer
  → dev_ptr + pitch
```

### 5.5 会话创建与内存所有权

1. `vkCreateVideoSessionKHR`；`vkGetVideoSessionMemoryRequirementsKHR`。
2. 按 memory type 分配 `VkDeviceMemory`；`vkBindVideoSessionMemoryKHR`。
3. **绑定内存必须存活到 `vkDestroyVideoSessionKHR`**：RADV 把 `radv_device_memory*` 裸指针存进 session context，每帧用其 BO 地址编程固件；提前 `vkFreeMemory` 会被后续分配复用，导致 VCN ring timeout。
4. 当前实现用 `session_mems_` 持有内存，`reset()` 在销毁 session 后才释放。
5. 创建后做一次 `VK_VIDEO_CODING_CONTROL_RESET`。

### 5.6 SPS/PPS 交接与 Std 结构

存在两条来源：

- **`h264_cuvid`**：`CuvidParser` 看到 SPS/PPS 时 stash 原始 `nal.raw_data`（保留 EPB），两者就绪后写入 `VkSequenceCache`；后端优先使用这份原始参数集。
- **`h264_nvdec`**：FFmpeg 自己解析 H.264 且只调用 `cuvidDecodePicture`，shim parser 完全收不到 SPS/PPS。后端从 `CUVIDH264PICPARAMS` 重建一份最小 Annex-B SPS/PPS（`h264_direct_params.cpp`），保存在 decoder 实例内，避免进程级单槽 cache 串流。CUVID ABI 不含 profile constraint、精确 level、VUI、crop、SPS/PPS ID 和 POC type 1 cycle；当前按可用字段推断，不支持的组合显式失败。

两条来源最终都用 `ff_h2645_packet_split` 重新解析，填 `StdVideoH264SequenceParameterSet` / `PictureParameterSet`。`fill_sps_std` / `fill_pps_std` 的约束：scaling list 8x8 顺序 `{0,3,1,4,2,5}`；注意 designated-initializer 字段序和 `aspect_ratio_idc` cast。

当前合成路径的边界：

- 只支持 4:2:0/8-bit、progressive、POC type 0/2、默认缩放矩阵、单 slice；
- 假定 SPS/PPS ID 为 0；profile 根据 CABAC/8x8/缩放矩阵推断，level 按宏块规模选择；
- 不携带 VUI/crop，因此 crop、色彩元数据和 POC type 1 等场景不能据此声明兼容；
- 若第一个参数集之后发生变化，当前不会重建 session。

`h264_cuvid` 路径仍使用单槽进程 cache；多流需按 `CUvideodecoder` 键控（P1）。`h264_nvdec` 合成参数集属于 decoder 实例，不受该单槽限制。

### 5.7 图像、DPB 与每帧解码

- output 图像和 DPB 图像都必须是 `VK_IMAGE_TILING_OPTIMAL`（规范锁死）。
- DPB 图只带 `VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR`，不混 `DST`；RADV VCN<5 要求 DPB 与 output 分离。
- `pSetupReferenceSlot` 每帧必填，含 B 帧；非参考帧用临时 DPB slot，解码后回收。
- DPB 槽位**升序分配**最小空闲槽，避免越过固件 `ref_frame_list[]` / 槽位掩码范围。
- barrier 覆盖 setup 槽和全部参考槽；`oldLayout` 用每个图像自己跟踪的 layout。
- 驱动内部按 FrameNum/POC 排参考列表；应用只需维护 active 参考集合（MMCO + 滑动窗口 + IDR 清空）来填 `pReferenceSlots`。

每帧处理：

1. 解析 slice header、POC；`ff_h264_init_poc` / `ff_h264_parse_ref_count` 可复用。
2. 填 `StdVideoDecodeH264PictureInfo`（`pic_order_cnt`、`frame_num`、`is_intra`、`primary_pic_type` 等）。
3. 选 output image（按 `CurrPicIdx`）；选 DPB slot；填 `VkVideoReferenceSlotInfoKHR`。
4. `vkCmdBeginVideoCodingScopeKHR` → `vkCmdDecodeVideoKHR` → `vkCmdEndVideoCodingScopeKHR`。
5. decode queue 提交，signal `decode_done`。
6. 可选 `VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR` 查询解码状态；`VK_ERROR_DEVICE_LOST` 立即置 `device_lost_` 并快速失败。

### 5.8 拷贝、导出与 HIP 导入

1. transfer queue 等待 `decode_done`。
2. `VkImageMemoryBarrier` 把 output image 转到 `TRANSFER_SRC_OPTIMAL`。
3. `vkCmdCopyImageToBuffer` 按 plane 拷贝到线性 `VkBuffer`：
   - Y plane：`imageExtent = {W, H, 1}`，`bufferRowLength = pitch`，`bufferImageHeight = H`。
   - UV plane：texel 是 2 字节，`imageExtent = {W/2, H/2, 1}`，`bufferRowLength = pitch/2`，`bufferImageHeight = H/2`。**这是最容易错的点。**
4. 提交 transfer，signal `copy_done` + fence；主机 `vkWaitForFences`。
5. `vkGetMemoryFdKHR(OPAQUE_FD)` 导出 fd；buffer 大小 `pitch * H * 3 / 2`，pitch 按 `optimalBufferCopyRowPitchAlignment` 对齐。
6. `HipFrameMapping::map_fd()` 导入；UV offset = `pitch * height`。

### 5.9 后端接入宿主

`cuda_shim/` 的 `libnvcuvid.so.1` 新增 Vulkan Video 解码后端，与 VAAPI 后端并列于 `DecoderBackend` 接口下，用 `CUVID_SHIM_BACKEND=vk` 运行时切换（默认 VAAPI）。CUVID 符号面不变，环境变量只切内部后端。

- SPS/PPS 由 `CuvidParser` 解析后经进程级 `VkSequenceCache`（前置 start code）交给后端；后端在 SPS/PPS 就绪后懒建 Vulkan session。
- 2026-11-12 P0 双后端四路码流各 82/82 逐字节一致。
- 验收宿主仍是 `h264_cuvid` / `h264_nvdec`，不是 `h264_vulkan`。`h264_vulkan` 是 FFmpeg 原生 VK 解码器，绕过 shim，只能作环境冒烟或语义参考。
- 当前 320×240 P0 四路矩阵中，`h264_cuvid` 与 `h264_nvdec` 在两后端均通过；`h264_nvdec` 的 Vulkan 会话依赖合成参数集，限制见 [§5.6](#56-spspps-交接与-std-结构)。这不等于真实业务线程/队列已验收。

### 5.10 B 帧挂死根因与修复

根因：绑定到 `VkVideoSessionKHR` 的 `VkDeviceMemory` 在 `vkBindVideoSessionMemoryKHR` 成功后立刻 `vkFreeMemory`，违反“必须存活到 session 销毁”的规定。RADV 把 `radv_device_memory*` 裸指针存进 session context，每帧用其 BO 地址编程固件；内存被后续分配复用后，第 4 帧左右固件上下文被覆盖，VCN ring timeout / mode1 reset。

修复清单：

| # | 修复 |
|---|---|
| 1 | 会话内存所有权：`session_mems_`，bind 后转移，`vkDestroyVideoSessionKHR` 后才释放 |
| 2 | DPB 图不带 `TRANSFER_DST`，改为 `VIDEO_DECODE_DPB \| TRANSFER_SRC` |
| 3 | 多平面导出按 plane texel 单位计算 `imageExtent` / `bufferRowLength` / `bufferImageHeight` |
| 4 | DPB 槽位升序分配，避免越过固件参考表范围 |
| 5 | barrier 覆盖 setup 槽与全部参考槽 |
| 6 | SPS/PPS 存 `raw_data`（保留 EPB） |
| 7 | 解码状态 query、fence 超时、`CUVID_SHIM_DEBUG_VK` 诊断 |

### 5.11 同步、资源释放与错误诊断

- Vulkan 内部：decode queue → semaphore → transfer queue → fence；主机等待。
- Vulkan → HIP：`decode()` 末尾已主机等待；`map_frame` 只导入，不需要额外 HIP 等待。
- 计算 → 后续：HIP event + 主机等待。
- 异步候选：`VK_KHR_external_fence_fd` → sync_file → `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE` → amdgpu syncobj → ROCr 挂 HIP queue；未落地，属厂商协同项。
- fd 所有权：`decode()` 导出存 `exports_`；`map_frame()` 转移给 mapping，`reset()` close；无泄漏。
- `vkWaitForFences` 30s 超时并检查返回值；`VK_ERROR_DEVICE_LOST` 置 `device_lost_`。
- `CUVID_SHIM_NOCOPY=1` 只解码不导出，分离“解码参数问题”与“多队列同步/导出问题”；`CUVID_SHIM_DEBUG_VK=1` 每帧打印 type/poc/参考槽/setup 槽/DPB 空闲表。
- `CUVID_DUMP_NV12_DIR` dump 后与软解参考逐帧逐平面比，注意 pitch 跨行 + NV12 交织。

### 5.12 测量陷阱

1. **“色度 PSNR u/v ≈ 6 dB”是测量错误**：把 pitch 跨行的 NV12 当 planar yuv420p 比。正确做法：亮度 `y*pitch`；色度从 `pitch*H + y*pitch` 开始，U/V 交织，参考需先交织或分别比 `dump[0::2]==U`、`dump[1::2]==V`。
2. **“VAAPI 后端输出平坦 0x80”同样是测量错误**：按正确方法重测，VAAPI 四路码流同样逐帧位精确。
3. **`verify_kernel_read` 的 `OK/Ysum/UVsum` 不校验画面**：只证明 GPU kernel 与 CPU 读到同一块内存；画面正确性必须 dump NV12 后与软解参考逐帧比。
4. 已排除的假设：H.264 语义层、DPB 图未写、`used_for_reference_flags` 差异、`RADV_DEBUG=videoarraypath`、`CUVID_SHIM_EXP=dpbsrc`、SDMA 拷贝路径——均与 B 帧挂死无关。

### 5.13 当前边界与实现顺序

Vulkan 路径的 `h264_nvdec` 零拷贝已在四路 P0 码流上通过（需额外 surface）；多 slice、多流/seek/flush、crop、动态分辨率及 HEVC/VP9/AV1 均未在此后端验收；`cuvidReconfigureDecoder` 仍为 stub。两个后端继续使用同一验收口径，路线决策见[总体方案](CUDA兼容层总体方案.md)。

后续实现/补证顺序：

1. **能力探测**：instance/device、decode/transfer 队列族、`VK_KHR_video_*`、NV12/OPTIMAL、external memory fd；用 `verify_vulkan_video_full`。
2. **建会话**：`VkVideoSessionKHR`、内存需求/绑定/所有权、session parameters；先不接宿主，用 `verify_cuvid_session` 冒烟。
3. **SPS/PPS 交接**：`CuvidParser` stash `raw_data` → `VkSequenceCache` → `StdVideoH264*`；验证 SPS/PPS 解析不漂移。
4. **图像/DPB**：output/DPB 分离、layout 跟踪、DPB 槽位升序、reference slots。
5. **每帧解码**：slice/POC、`vkCmdDecodeVideoKHR`、decode queue、状态 query/fence。
6. **拷贝/导出**：transfer queue + semaphore、barrier、`vkCmdCopyImageToBuffer`（plane texel 单位）、`vkGetMemoryFdKHR`。
7. **HIP 导入**：`map_fd`、pitch/offset 校验、`verify_kernel_read`、dump 对比。
8. **接宿主**：`CUVID_SHIM_BACKEND=vk` 跑 `h264_cuvid`/`h264_nvdec`；补齐 reconfigure、多流 cache 键控。
9. **平台/性能**：目标平台能力探测、转换成本量化、双后端回归。

---

## 6. NPP 实现与证据

### 6.1 NVIDIA 能力直测

`benchmarks/npp/npp_benchmark.cpp` 在 RTX 4060、NPP 11.5.1.107、1920×1080 @ 50 次迭代下直测 26 项 API，全部成功。能力分布：

- 几何变换 7 项、滤波 5 项、色彩转换 5 项、位深转换 2 项、算术/融合 2 项、形态学 2 项、统计 2 项、阈值 1 项。
- NPP 的格式盲区：无 RGB→NV12（只有 RGB→YUV420 平面）；AddWeighted 无纯 8u 版本。

### 6.2 P0 函数与参考基线

FFmpeg 7.1 `vf_scale_npp.c` 实际只调用 3 个 NPP 函数：

- `nppiResizeSqrPixel_8u_C1R`
- `nppiYCbCr420_8u_P2P3R`
- `nppiYCbCr420_8u_P3P2R`

NVIDIA 参考环境：RTX 4060、驱动 595.99.02、CUDA Driver API 13020、CUDA Runtime/NPP 11.5（NPP 11.5.1.107）。`tests/parser/npp_p0_reference.cpp` 已执行三个函数，P2P3→P3P2 往返逐字节一致。

参考输出 SHA256：

- `p2p3.yuv`：`7800549c7f3c4a3e54ef28c53cc9d84a228341717e8fd656d0d30bc84607c85a`
- `roundtrip.nv12`：`1899758496662f141280cc689b854d27c240d57cab23ed23dccba1ae56d08569`
- `resize_4x4.gray`：`70cdde3ccdccbcfe2eeca52d8deca9cbca06b7b3612c69a31e0e677002eb8e6d`

### 6.3 shim 设计与 ABI 约束

NPP 兼容层用 HIP kernel 逐函数重实现 NPP API，跑在 GFX 通用计算引擎上。P0 只实现 FFmpeg `scale_npp` 实际使用的 3 个函数 + nppc：

| 函数 | 所属库 | 实现要点 |
|---|---|---|
| `nppiResizeSqrPixel_8u_C1R` | `libnppig.so.11` | 每线程一目标像素；按 `interp_algo` 做反向映射与滤波；阶段 A 为 cubic/NN/linear，最终覆盖全部 NPP 插值模式；处理 ROI/step/pitch |
| `nppiYCbCr420_8u_P2P3R` | `libnppicc.so.11` | NV12（2 plane）→ YUV420（3 plane）；UV 去交织 |
| `nppiYCbCr420_8u_P3P2R` | `libnppicc.so.11` | YUV420（3 plane）→ NV12（2 plane）；UV 交织 |
| `nppc` 初始化/stream | `libnppc.so.11` | `nppGetStreamContext` / `nppSetStream` / `nppGetVersion`；用 HIP 设备属性填 `NppStreamContext` |

关键约束：

1. **SONAME**：必须是 `libnpp<子库>.so.11`；FFmpeg 是编译期链接，`ldd` 直接依赖。
2. **头文件与发布边界**：路线 A——shim 只产 `.so`，构建时使用另行提供的授权 CUDA/NPP 11.5 头文件；发布物不含专有头文件。法务结论以[总体方案](CUDA兼容层总体方案.md)为准。
3. **ABI**：`NppStatus`、`Npp8u/Npp16u/Npp32f/Npp64f`、`NppiSize`、`NppiRect`、`NppStreamContext` 必须一致。
4. **数值**：输出必须与 NVIDIA 11.5.1.107 参考逐字节一致；不得用“视觉效果接近”放行。
5. **launch 路径**：NPP 的 `.so` 直接链 `libamdhip64.so`，用 `hipLaunchKernel`/`hipModuleLaunchKernel`，不要走 `libcuda` 的 `cuLaunchKernel` stub。
6. **最小库集合**：`libnppc`、`libnppig`、`libnppicc` 非空；`libnppidei`、`libnppif` 占位（FFmpeg 链接但未调用）。

### 6.4 NPP 验证矩阵（设计/待验证）

| 验证项 | 方法 | 通过标准 |
|---|---|---|
| 3 函数基础正确性 | 零值、极值、固定种子随机值、渐变、棋盘格输入 | 所有输出 plane 逐像素比较；以逐字节一致为目标，差异不得仅以“视觉效果接近”放行 |
| Resize 插值分阶段 | 先 cubic/NN/linear，再 FFmpeg cubic2p/Lanczos/super，最后 NPP 全部 interpolation flags | 每个已声明算法分别建立 NVIDIA golden；未实现算法显式返回不支持，不能静默换算法 |
| Resize 坐标与舍入 | 放大、缩小、非整数比例、奇数尺寸、常见及非对齐 ROI | 坐标、舍入及边缘结果与参考一致；架构相关差异须列出模式、差值和评审结论 |
| 平面转换与 ROI | `P2P3/P3P2` 检查 Y/UV plane 顺序、ROI 原点、宽高、src/dst step、非连续内存 | 输出 plane、ROI 外保护区域和 padding 符合预期；不得越界写或破坏相邻数据 |
| stream 与异步 | 默认/非默认 stream、事件等待、连续提交 | 无提前读取、跨流竞争和结果不稳定；重复执行一致 |
| 参数与错误语义 | 空指针、非法 ROI/step、不支持格式、资源失败 | 返回值和输出状态符合 NPP 语义；不得返回成功但留下未初始化输出 |
| 完整联合链路 | 解码输出交给 3 个 P0 函数，再传入 HIP/NPP 下游 | 与第 2 步共同通过；保留逐阶段参考输出以便定位差异 |

### 6.5 当前边界

- NVIDIA 参考基线已建立（RTX 4060 + NPP 11.5.1.107，3 个函数输出与 SHA256 归档）；现有 resize golden 只有 8×8、nearest 样例，不足以覆盖阶段 A。
- AMD NPP Stage A shim 已实现 NN/linear/cubic 三个 resize 模式及两种 4:2:0 平面转换；真实 CUDA/NPP 12 头文件下构建成功，独立 FFmpeg 已加载 `scale_npp`。
- FFmpeg 联合链路：P2P3/P3P2 在 VAAPI/Vulkan 两个解码后端均与软件转换逐字节一致；resize 模式只完成功能烟测，尚无 NVIDIA golden。
- **FFmpeg 将 libnpp 标记 nonfree**：`--enable-libnpp` 必须配 `--enable-nonfree`，当前集成本地构建不可再分发。发布/客户交付路径需要法务和产品重新决策。
- 头文件版本为 CUDA 12.0.1.104，目标 NPP 11.5.1.107 的最终 ABI/golden 复核未完成。
- `libnppc` 当前只提供 `nppGetLibVersion` 链接探测；完整 stream/version 接口待后续增量。
- 3 个函数必须与 NVIDIA 11.5.1.107 参考逐字节一致；ROI、step、padding、stream、错误语义需补齐。
- resize 插值范围最终为全部 NPP interpolation flag；实施顺序是 FFmpeg 核心 cubic/NN/linear → FFmpeg 其余选项 → NPP 全部 flags。未实现模式显式失败。
- NPP 头文件是专有许可，采用路线 A（用户自带授权头文件，发布物只含 `.so`）；当前开发机没有这些头文件/NPP 库，构建前需由使用方提供。

---

## 7. 历史格式能力与性能

> 完整矩阵与测试命令见[NVDEC 与 VAAPI 解码对比](NVDEC与VAAPI解码对比.md)。本节只保留影响路线判断的结论。

历史批次：NVIDIA RTX 4060（NVDEC）vs AMD RX 9070（VCN5），Mesa 26 / FFmpeg 6.1，15 组格式。

| 统计 | 数量 |
|---|---|
| 一致 | 10 项 |
| VAAPI 领先 | 5 项（H.264 422/444/10bit、HEVC 422、MPEG-2 422） |
| NVDEC 领先 | 0 项 |

VAAPI 领先的 5 项全是专业格式，原因是 NVIDIA 消费级卡硬件阉割；VCN5 消费级 RX 9070 即支持。

SDK 直测性能（1080p）：HEVC 420 8bit 1553fps、AV1 1445fps、H.264 828fps。

**边界**：该矩阵只作路线参考，P0 只承诺 H.264 / 8-bit / 4:2:0 / NV12；不能外推到全部 NVIDIA 设备、所有 profile/level 或目标平台。

---

## 8. VAAPI 转接实现

### 8.1 定位、前置条件与组件

- **当前定位**：与 Vulkan 同级的候选后端；运行时默认为 VAAPI 只是实现选择，不代表路线选型。路线决策见[总体方案](CUDA兼容层总体方案.md)。
- **前置条件**：DRM render node 可打开；libva 可初始化；目标 profile/entrypoint 可用；LINEAR DRM modifier 可创建并导出；HIP 与 VAAPI 可同进程共存。
- **不适用**：非 Linux、需要跨平台、需要 VPE 的 HDR/3D LUT 等未通过 VAAPI 暴露的能力。

| 模块 | 职责 | 关键源码 |
|---|---|---|
| `VaapiH264Decoder` | VA config/context/surface、解码提交 | `cuda_shim/src/vaapi_h264_decoder.cpp` |
| `h264_picture_translate` | `CUVIDPICPARAMS` → `VAPictureParameterBufferH264` / `VASliceParameterBufferH264` | `cuda_shim/src/h264_picture_translate.cpp` |
| `CuvidParser` | SPS/PPS/slice、DPB、显示序 | `cuda_shim/src/cuvid_parser.cpp` |
| `HipFrameMapping` | surface 导出、HIP 导入、布局门禁 | `cuda_shim/src/hip_frame_mapper.cpp` |
| `libnvcuvid` | 符号面、句柄、map/unmap | `cuda_shim/src/libnvcuvid.cpp` |

### 8.2 端到端调用时序

```text
宿主 cuvidCreateDecoder
  → 校验 H.264/420/8bit/NV12/Weave
  → open DRM node → vaGetDisplayDRM → vaInitialize
  → vaCreateSurfaces(LINEAR NV12, num=ulNumDecodeSurfaces)
  → vaGetConfigAttributes → vaCreateConfig → vaCreateContext

宿主 cuvidParseVideoData（h264_cuvid）
  → CuvidParser 切 NAL、解析 SPS/PPS
  → pfnSequenceCallback(CUVIDEOFORMAT)
  → slice → parse_slice_header → pfnDecodePicture(CUVIDPICPARAMS)

宿主 cuvidDecodePicture
  → translate_h264_picture（参数/IQ/slice/参考帧）
  → vaCreateBuffer(Picture/IQ/SliceParam/SliceData)
  → vaBeginPicture → vaRenderPicture × N → vaEndPicture

宿主 cuvidMapVideoFrame64
  → vaSyncSurface
  → vaExportSurfaceHandle(DRM_PRIME_2, LINEAR)
  → hipImportExternalMemory → GetMappedBuffer
  → 布局门禁 → dev_ptr + pitch

宿主 HIP kernel / NPP
  → 读 dev_ptr

宿主 cuvidUnmapVideoFrame64
  → 销毁 mapping → hipDestroyExternalMemory + close(fd)
```

### 8.3 `cuvidCreateDecoder` 字段映射

以下是目标映射；当前实现与目标的差距见 [§13.4](#134-实现缺口台账)。

| CUVID 字段 | VAAPI 映射 | 实现要点 |
|---|---|---|
| `CodecType` + `ChromaFormat` + `bitDepthMinus8` | `VAProfile` | H.264：Baseline→`VAProfileH264ConstrainedBaseline`，Main→`VAProfileH264Main`，High→`VAProfileH264High`；不能照抄 `VAProfileH264Baseline`（已废弃） |
| `ulWidth/ulHeight` | surface 尺寸 | 当前实现用 `ulWidth/Height`；目标实现应区分 coded/display/target 尺寸 |
| `ulNumDecodeSurfaces` | `vaCreateSurfaces` 数量 | host 用 `min_num_decode_surfaces + 3`；不要超过后端安全上限 |
| `OutputFormat` | `VA_RT_FORMAT` | NV12→`VA_RT_FORMAT_YUV420`；P010→`YUV420_10`（P0 不承诺） |
| `ulCreationFlags` | 能力标志 | 只支持 `cudaVideoCreate_PreferCUVID`；其他返回不支持 |
| `DeinterlaceMode` | VPP | Weave 直通；Bob/Adaptive 走 VPP（P0 不支持场） |
| `display_area` / `target_rect` | VPP crop/scale | P0 默认不缩放；`h264_cuvid` 的 `-resize` 返回不支持 |
| `vidLock` | 宿主 mutex | 不映射 HIP context lock |

### 8.4 parser / DPB / VAAPI 参数填充

公共 parser 状态机见 [§4](#4-parser--dpb-实现与证据)，VAAPI 侧还需完成：

1. **SPS/PPS**：`ff_h2645_packet_split` + `ff_h264_decode_seq_parameter_set` / `ff_h264_decode_picture_parameter_set`。
2. **slice header**：自写解析 `first_mb_in_slice`、`slice_type`、`pps_id`、`frame_num`、POC、参考列表重排、deblocking 参数、`bit_offset`。
3. **`VAPictureParameterBufferH264`**：
   - `CurrPic`：`picture_id = surfaces[CurrPicIdx]`，`frame_idx = frame_num`，flags 按 `ref_pic_flag`。
   - `ReferenceFrames[16]`：从 `CUVIDH264DPBENTRY` 填；未用槽显式置 `VA_INVALID_SURFACE` + `VA_PICTURE_H264_INVALID`，否则驱动把 0 值槽当有效短参考。
   - `seq_fields` / `pic_fields`：flags 必须按 FFmpeg `vaapi_h264.c` 的语义逐项填；`log2_max_pic_order_cnt_lsb_minus4` 要减 4。
   - `num_ref_frames`、`frame_num`、QP 偏移等。
4. **`VASliceParameterBufferH264`**：slice 数量、slice data 偏移、`slice_type`、`ref_pic_list`、重排命令、pred weight table、deblocking、`bit_offset`。
5. **IQ matrix**：`WeightScale4x4/8x8` 必须填；不填会让驱动 IQ matrix 全 0，输出平坦 128 灰。
6. **DPB 状态机**：IDR 清空；参考帧滑动窗口；MMCO；长期参考；显示序重排。当前 parser 用 `dpb_` + `display_queue_` 维护，`h264_picture_translate` 再把快照转成 VA 结构。

### 8.5 解码提交

```text
vaCreateBuffer(VAPictureParameterBufferType)
vaCreateBuffer(VAIQMatrixBufferType)
vaCreateBuffer(VASliceParameterBufferType, n slices)
vaCreateBuffer(VASliceDataBufferType, nBitstreamDataLen)
vaBeginPicture(context, surface)
vaRenderPicture(...) × buffers
vaEndPicture(context)
vaDestroyBuffer(...) × buffers
```

每步检查 `VAStatus`；失败返回 `CUDA_ERROR_UNKNOWN` 或 `NOT_SUPPORTED`，不得继续提交半成品。

### 8.6 映射、导出与同步

映射的公共实现见 [§2.6](#26-帧映射统一实现与布局门禁)。VAAPI 侧必须保留以下约束：

1. `vaSyncSurface` 确保解码完成。
2. `vaExportSurfaceHandle` 先 `SEPARATE_LAYERS`，失败回退 `COMPOSED_LAYERS`。
3. 检查 `fourcc == VA_FOURCC_NV12`；读取 `num_objects`、`num_layers`、`object_index`、`pitch`、`offset`、`size`。
4. 对每个 object 做 HIP import/map。
5. 校验 Y/UV 同一 object、pitch 相同、`uv_offset == pitch * height`；否则不支持。
6. 返回 `dev_ptr + pitch`。

线性 surface 的 pitch 可能是 512（宽 320），不能整块 `hipMemcpy`，必须逐行处理；导出的 NV12 可能是 separate layers（`num_layers=2`，UV 在 `layers[1].offset`），不能假设 composed；边界 2（HIP→VAAPI）的 pitch 需要 256 对齐（当前 AMD 平台），生产必须运行时查询。

解码→map 使用 `vaSyncSurface`，map 后提交 kernel，kernel→后续使用 `hipEventRecord` + `hipEventSynchronize`。unmap 销毁 mapping、`hipDestroyExternalMemory`、close fd；DPB/mapped/NPP 三类引用分别计数，`cuvidDecodePicture` 拒绝写已 map 的 slot。当前 `output_stream` 未接入，map 是同步语义。

### 8.7 宿主集成：`h264_cuvid` 与 `h264_nvdec`

| 路径 | 行为 | 验收要点 |
|---|---|---|
| `h264_cuvid` | 使用 shim parser；FFmpeg 自己分配 CUDA frame，逐平面 `cuMemcpy2DAsync` | 像素、帧数、显示顺序；设备内拷贝单独记录，不宣称零拷贝 |
| `h264_nvdec` | 直接提供 FFmpeg 已解析的 `CUVIDPICPARAMS`；map 后直接把 `devptr + offset` 作为 `frame->data` | 零拷贝目标路径；HIP 直接读取像素正确；用 `CUDA_SHIM_TRACE_COPY=1` 区分 D2D 次数 |

### 8.8 错误处理、能力探测与降级

- 启动/建会话前探测：DRM 节点、libva 版本、profile/entrypoint、LINEAR modifier、DMA-BUF、HIP 导入能力。
- 不支持：返回 `CUDA_ERROR_NOT_SUPPORTED`；不得静默改为 CPU 拷贝或软件解码。
- 失败诊断：记录 VAStatus、profile、surface 数、pitch/offset、map 阶段。
- 降级：目标平台不支持 LINEAR 时，回退 tiled 解码 + GPU 转换，作为明确降级路径单独记录，不混报零拷贝；若仍不满足业务，重新评估后端，决策见[总体方案](CUDA兼容层总体方案.md)。
- 诊断变量包括 `CUVID_SHIM_DEBUG_DPB`、`CUVID_SHIM_TRACE_SURF`、`CUVID_SHIM_DEBUG_VASUM`、`CUVID_SHIM_PLAIN_SURFACES`，完整说明见[附录 A](#附录-a环境变量与诊断开关)。

### 8.9 实现与补证顺序

1. **冻结 API/版本**：确认 nv-codec-headers、FFmpeg 7.1、libva 版本；跑 `make check` 确认符号面。
2. **建 VAAPI 会话**：profile/format 映射、LINEAR surface 池、config/context；先用 `verify_cuvid_session` 冒烟。
3. **打通 parser→参数**：SPS/PPS、slice header、DPB、`VAPictureParameterBufferH264` / `VASliceParameterBufferH264`；用 `verify_cuvid_parser`。
4. **打通解码**：`vaBegin/Render/EndPicture`；用实验 C 或 dump 对比验证真实像素。
5. **打通映射**：`vaExportSurfaceHandle` → HIP import → `dev_ptr + pitch`；用 `verify_kernel_read` 验证 kernel 可寻址。
6. **接宿主**：`h264_cuvid`（parser 回调 + `cuMemcpy2DAsync`）与 `h264_nvdec`（直接 `CUVIDPICPARAMS` + 零拷贝）；分别验收。
7. **补长尾**：crop、seek/flush、动态分辨率、多流、P010/奇数分辨率按批准范围处理；P010 不属于当前 P0。
8. **稳定性/性能**：1/8 路 30 分钟、资源/fd、吞吐/延迟基线。
9. **目标平台**：目标平台能力检查、全量回归、发布检查；Gate 定义见[总体方案](CUDA兼容层总体方案.md)。

---

## 9. Mesa 新增 frontends 设计（本期排除）

> 本章全部是**设计假设，不是已实现方案**。当前没有同口径实现证据，本期不纳入候选、排期和交付承诺。保留设计只为未来重评提供技术边界；成本、人力和路线决策见[总体方案](CUDA兼容层总体方案.md)。

### 9.1 定位与接口选择

该方案不走“外挂 shim 翻译”，而是在 Mesa 内新增接近 CUDA/CUVID 语义的 frontend，让驱动直接承接视频解码入口。NPP 和客户计算代码仍需 HIP 或其他组件，不能由 Mesa frontend 解决。

| 路线 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **A. 伪装 libnvcuvid** | 在 Mesa 内实现 CUVID 兼容入口，导出同名符号 | 客户/FFmpeg 改动最小 | 需要处理专有头文件/ABI；Mesa 上游几乎不可能接受 CUDA 语义 |
| **B. 新 native frontend** | 定义自有视频 frontend API，客户改用新接口 | 接口干净、可上游 | 不是源码级 CUDA 兼容，客户要改调用 |

若未来立项，技术评估优先 B：CUDA 语义进 Mesa 上游的接受度极低，A 只能作为私有 fork 的过渡。最终选型仍由[总体方案](CUDA兼容层总体方案.md)管理。

### 9.2 在 Mesa 中的位置与数据通路

```text
Mesa
├── src/gallium/frontends/va/        # 现有 VAAPI 前端（参考）
├── src/gallium/frontends/vdpau/     # 现有 VDPAU 前端（参考）
├── src/gallium/frontends/omx/       # 现有 OMX 前端（参考）
└── src/gallium/frontends/<new>/     # 新增 CUDA/CUVID 风格 frontend
        ↓ pipe_video_context
    src/gallium/drivers/radeonsi/    # VCN 解码、surface、fence
        ↓
    VCN 硬件
```

```text
客户/FFmpeg → 新 frontend（会话、surface、码流）
  → pipe_video_context / radeonsi VCN
  → 解码输出 surface
  → frontend 直接导出 DMA-BUF（dmabuf handle）
  → HIP 导入
```

与 VAAPI 方案的区别是少一层 libva 抽象，frontend 可直接管理 surface 和 fence；但仍需处理 tiling/linear、pitch/offset、同步。

### 9.3 内存、同步与计算边界

- **内存**：frontend 需要暴露“创建线性 surface / 导出 DMA-BUF”的能力；如果只能出 tiled，仍需 GPU 转换。
- **同步**：`pipe_fence` → sync_file；需要与 ROCr 协同，把 syncobj 挂成 HIP queue wait fence。
- **资源生命周期**：session/surface/DPB/mapped 三类引用与现有模型一致。
- **NPP**：仍需 HIP kernel 重实现，Mesa frontend 不解决图像处理。
- **客户自研 kernel**：仍需 HIPIFY，Mesa frontend 不解决计算代码。

因此该方案只减少“解码入口”的一层翻译，不减少 NPP/客户 kernel 的工作量。其潜在收益仅是设计假设：行为控制力强、存在性能优化空间、可形成平台级长期能力；当前没有同口径实现证据支撑这些收益。

### 9.4 上游、分支、构建与 CI

- **上游**：先提 RFC，证明接口通用、符合 Mesa 方向；需要长期驱动团队和 CI。
- **私有 fork**：若上游不接受，需要跟随 Mesa 升级维护；每换一个 Mesa 版本都要适配。
- **版本矩阵**：Mesa、内核、固件、ROCm、FFmpeg 组合需要独立回归。
- **构建**：Mesa 内新增 frontend 编译单元、导出库、头文件；需要驱动 CI。
- **测试**：单元测试、像素测试、同步测试、多流/seek、性能、上游回归。
- **发布**：Mesa 版本与自研驱动绑定；需要明确支持矩阵和回退策略。

### 9.5 本期排除与重评触发

本期排除的技术理由：缺少同口径实现证据，无法与 VAAPI/Vulkan 按同一验收标准比较；需要长期驱动团队和 Mesa 维护能力；不能自动解决 NPP 和客户计算代码；测试和责任边界扩大到驱动层。周期与近期导入目标的决策见[总体方案](CUDA兼容层总体方案.md)。

任一条件满足时可重新评估：

- 跨平台（Windows/Android/多 GPU 厂商）成为明确业务需求；
- 公司建立稳定驱动团队，并能承担 Mesa 上游/分支维护；
- VAAPI/Vulkan 方案在目标平台遇到无法绕过的性能或能力瓶颈；
- 上游对通用视频 frontend 明确表达接受意愿；
- 自有 SDK 需要平台级统一视频入口。

### 9.6 若立项的实施顺序

1. **接口决策**：确定 A（伪装 libnvcuvid）或 B（新 native frontend）；完成上游/私有分支策略和法务评估，相关决策见[总体方案](CUDA兼容层总体方案.md)。
2. **frontend 骨架**：在 Mesa 新增 frontend 目录，接通 `pipe_video_context` 和 radeonsi VCN；先跑通会话/销毁。
3. **surface/导出**：线性 surface 或可导出 DMA-BUF；验证 HIP 导入。
4. **码流/参数**：SPS/PPS/slice/DPB 与现有 parser 的对接；复用 VAAPI/Vulkan 的参数翻译经验。
5. **同步**：`pipe_fence` → sync_file / syncobj → HIP；先主机同步，后异步。
6. **宿主/测试**：FFmpeg 或自研测试程序接入；像素、资源、多流、seek、性能。
7. **上游/CI**：RFC、上游评审、驱动 CI、版本矩阵；不通过则评估私有 fork 的长期成本，成本决策见[总体方案](CUDA兼容层总体方案.md)。
8. **NPP/客户 kernel**：并行确认仍由 HIP/HIPIFY 承担，不纳入 frontend 范围。

---

## 10. 构建、打包与诊断

### 10.1 构建

`cuda_shim/Makefile` 产出 `libcuda.so.1` 与 `libnvcuvid.so.1`：

- `libcuda.so.1`：g++ 直编，链 `libamdhip64.so`，带 SONAME 和 `libcuda.so` 开发软链。
- `libnvcuvid.so.1`：链接 VAAPI、DRM、Vulkan、9 个 FFmpeg 解析对象、`libavutil`、HIP；带 SONAME 和开发软链。
- VAAPI 依赖 libva、libva-drm、libdrm、HIP、FFmpeg 9 个解析对象；可执行 `make libnvcuvid.so.1`。
- Vulkan 构建在 `NVCUVID_SRC` 增加 `vulkan_h264_decoder.cpp`、`backend_dispatch.cpp`，加 `-I$(MESA_DIR)/include`、`-lvulkan` 和 FFmpeg 9 个 `.o`。
- Vulkan 头来自 `MESA_DIR/include`；只有 `libvulkan.so.1` 时用 `VULKAN_LIB=-l:libvulkan.so.1`，该变量可按机器覆盖。

### 10.2 打包、安装、共存与回退

1. 发布 shim `.so`、SONAME/软链、默认配置、能力诊断工具、版本清单、构建标识、SBOM/许可证和测试报告。
2. 明确安装目录、loader 路径和优先级；不得未经验证覆盖系统或 NVIDIA 库。
3. 与 NVIDIA 库共存：分别定义“同机不同进程”和“同进程混用”是否支持；不得默认承诺同进程混用同一 CUDA 符号。
4. 升级/回退：保留上一版本库和配置；升级前做兼容检查，升级后跑健康检查。
5. VAAPI/Vulkan 的部署都须明确 SONAME、loader 路径及与 NVIDIA 库共存策略；发布矩阵和回退尚未形成，见 [§13.4](#134-实现缺口台账) G12。

### 10.3 诊断与敏感信息边界

统一记录阶段、会话、codec、返回码、surface/fence 状态和降级原因；默认不记录原始码流等敏感内容。VA 失败须记录 VAStatus、profile、surface 数、pitch/offset、map 阶段；VK 须记录超时、设备丢失、参考/setup 槽和拷贝/导出阶段。关键调试开关见[附录 A](#附录-a环境变量与诊断开关)。

---

## 11. 验证方法与验收设计

> 本章的“通过标准”均为设计/待验证口径；只有“当前状态”或证据章节明确列出的项目才是已验证。Gate 的阶段和发布决策定义见[总体方案](CUDA兼容层总体方案.md)，具体复现命令见[验证手册](../verification/验证手册.md)。

### 11.1 阶段通过条件

| 阶段 | 必交付证据 | 通过标准 | 当前状态 |
|---|---|---|---|
| **第 0 步：范围与底层能力确认** | 范围/验收口径；AMD 验证机 HIP/VAAPI 共存、DMA-BUF、LINEAR NV12、实验 C/E | 范围确认；底层能力门禁通过 | ✅ 已通过，不代表 P0 整体通过 |
| **第 1A 步：真实解码→HIP 读** | 同进程 libva 解码→LINEAR surface→DMA-BUF→HIP 读取的程序与日志 | 多帧 NV12 各 plane 逐字节一致；无 fd/内存泄漏 | ✅ 已由实验 C 闭合 |
| **第 1B 步：CUVID shim 与 FFmpeg 集成** | 真实 CUVID decoder/parser/map/unmap；`h264_cuvid`、`h264_nvdec` 双路径 | 两条路径多帧像素、帧数、显示顺序、pitch、flush、资源检查通过；`h264_nvdec` 证明 HIP 直接读取像素正确 | ⚠️ P0 四路双后端 32/32 通过；多 slice、真实业务配置和长稳待补 |
| **第 2 步：CUVID→NPP 联合业务链路** | 3 个 NPP P0 函数 + 必要 nppc；解码输出接入 NPP | 解码正确；NPP 与 NVIDIA 11.5.1.107 参考逐字节一致；ROI/step/plane/stream/生命周期符合范围 | ⚠️ FFmpeg 与双后端 P2P3/P3P2 联合链路逐字节通过；resize NVIDIA golden、11.5 ABI、nonfree 发布路径待完成 |
| **P1：复杂场景与交付验证** | crop、seek/flush、动态分辨率、长期参考、用户应用、异常恢复、安全、部署、生产性能 | 按 [§1.4](#14-p0-技术范围与验收边界) 范围通过；P010、其他 codec/NPP/API 仅在新增需求评审后扩展 | ❌ 未开始 |

补充要求：

1. 第 1A、1B、2 步必须校验实际像素，不得以句柄创建、import/map 成功或进程退出码代替。
2. 测试须覆盖参考帧复用、surface 回收及正常 flush；P0 覆盖 1080p/4K、1 路和 8 路，并分别执行 30 分钟资源稳定性检查。
3. 主机同步是第 1A、1B、2 步基线；异步 fence 独立验证，不阻塞功能检查。
4. 对声明零拷贝的 `h264_nvdec` 路径，若只能退回 CPU 拷贝完成链路，应标记为零拷贝目标未通过；`h264_cuvid` 自身的设备内拷贝路径单独记录，不混报为零拷贝证据。

### 11.2 双后端验收表

| 验证项 | VAAPI 入口与通过标准 | Vulkan 入口与通过标准 | 当前证据 |
|---|---|---|---|
| 符号面 | `make check`；82 + 22 符号可解析 | 同一 CUVID/CUDA 符号面 | PASS，见 [§3.3](#33-符号面结果与错误语义) |
| CUVID 会话 | `verify_cuvid_session`；VAAPI LINEAR NV12 session 创建/销毁 | `verify_cuvid_session`；VK session 冒烟 | P0 PASS；不代表完整会话语义 |
| parser | `verify_cuvid_parser`；I/P/B 解析通过 | 共用 `CuvidParser`，另验证 SPS/PPS cache 交接 | P0 PASS；多流/长尾未验收 |
| mapped 指针 | `verify_kernel_read`；`errors=0`，event 顺序正确 | 同一检查，覆盖 VK 导出 buffer | 两后端均 `errors=0`；不独立证明像素 |
| 像素 | `verify_decode_to_hip` / dump 对比；四路码流逐字节一致 | `verify_vk_decode_to_hip` / dump 对比；四路码流逐字节一致，fd/显存无泄漏 | 两后端各 82/82；底层 VK I/IP/IPB 各 30 帧也已通过 |
| FFmpeg 双路径 | `h264_cuvid` / `h264_nvdec`；像素、帧数、顺序、零拷贝计数 | 同一宿主双路径，而非 `h264_vulkan` | ✅ P0 四路双后端 32/32（零拷贝需额外 surface）；长尾未覆盖 |
| 复杂场景 | seek/flush、crop、动态分辨率、1/8 路 30 分钟，按 [§1.4](#14-p0-技术范围与验收边界) | 同左，另含多流 cache 键控和 reconfigure | ❌ 未覆盖 |
| 目标平台 | 运行时能力门禁、全量回归、发布检查 | 视频扩展/队列、转换成本、平台矩阵 | ❌ 未跑 |

NPP 的设计验证项集中在 [§6.4](#64-npp-验证矩阵设计待验证)，不在这里复制第二套标准。

### 11.3 性能与稳定性验证方法

P0 只记录性能，不设通过阈值；进入二期前由产品和客户确定门槛，门槛决策见[总体方案](CUDA兼容层总体方案.md)。对比组如下；其中编码和软件降级只用于对照测量，不扩大 [§1.5](#15-当前非目标) 的交付范围。

| 组 | 数据通路 | 用途 |
|---|---|---|
| A | VAAPI 解码→CPU 回读→HIP 输入→CPU 回传→VAAPI 编码 | CPU 拷贝基线，衡量完整 D2H/H2D、同步和拷贝成本 |
| B | LINEAR DMA-BUF + `vaSyncSurface`/`hipEventSynchronize` | 首期主方案，和 A 比较收益及瓶颈 |
| C | tiled 解码→GPU 转换成 HIP 可读布局 | 线性布局在目标平台回退时的对照 |
| D | DMA-BUF + DRM fence 桥接 | 异步实现后对比主机等待减少量 |
| E | 软件编码或评审批准的其他降级路径 | 单独记录功能可用性及 CPU/吞吐/延迟代价 |

必须记录：吞吐（解码/编码/完整流程 fps，多路时单路和总吞吐）、延迟（p50/p95/p99/最大值）、阶段成本（VAAPI 等待、HIP kernel/event、NPP、导入/映射、CPU 回读/回传、编码等待）、资源（CPU、进程内存、显存/surface、fd）、初始化成本；场景覆盖 1080p/4K、NV12、I/P/B、1/8 路、seek/flush。同步记录输出正确性和画质；配置、帧数或业务处理不同的结果不得直接比较。

稳定性须让 1 路和 8 路分别运行 30 分钟，检查无崩溃、死锁、非法访问或持续资源泄漏；S1–S8 的 1000 帧 fd 检查不等于这项业务长稳验收。

### 11.4 当前补证顺序

> VAAPI 与 Vulkan Video 同级，不以“先完成 VAAPI 再评估 Vulkan”作为资源优先级；以下 1–4 均要求两后端使用同一口径，最终选型由[总体方案](CUDA兼容层总体方案.md)的书面决策完成。

1. 收敛 FFmpeg 宿主矩阵的失败项：VAAPI `nvdec-unsafe` 生命周期、Vulkan NVDEC 的 map 失败；保持 `cuvid-cpu/cuda`、默认安全路径和 PTS/顺序回归；
2. 实现 NPP 3 个函数并打通“解码 → NPP”联合链路；
3. 在目标平台复跑底层能力门禁（VAAPI/Vulkan、HIP、DMA-BUF、线性/可导出布局）；
4. 补多流、seek/flush、crop、动态分辨率、1/8 路 30 分钟；
5. 基于同一验收结果作出后端选型；决策前不设默认主线或可跳过后端。

---

## 12. “能证明 / 不能证明”总表

| 证据 | 能证明 | 不能证明 |
|---|---|---|
| 实验 1/2/3 | HIP 与 VAAPI 同进程共存；DMA-BUF 双向句柄/资源链路可行 | 真实编解码像素 |
| 实验 C | libva 真实解码→LINEAR→DMA-BUF→HIP 逐字节正确；I/P/B/DPB/显示序 | CUVID shim 端到端；FFmpeg 宿主 |
| S1–S8 | 主机同步顺序、池复用、DPB 引用、flush/destroy、多线程、fd 长稳 | 真实码流像素；30 分钟业务稳定性 |
| libcuda 82/47 | 符号面、功能子集可用 | 完整 driver API/ABI、完整 UVM |
| parser 复用 | SPS/PPS/slice/POC/DPB 可复用 FFmpeg 解析 | 长期参考、场/MBAFF、crop、seek |
| Vulkan V1–V6 | 队列/codec/会话能力；tiling/同步限制 | 编码稳定性、跨平台矩阵 |
| Vulkan P0 | VK 解码→拷贝→导出→HIP 读取逐字节；h264_nvdec 四路宿主通过 | 多 slice、多流、crop、动态分辨率 |
| P0 双后端 82/82 | 两条后端数据通路和 mapped kernel 读取 | FFmpeg 双路径、NPP、业务链路、目标平台 |
| NPP NVIDIA 参考 | 3 函数参考输出已固化 | AMD 实现与逐字节一致 |

---

## 13. 风险、实现缺口与开放问题

### 13.1 交付关键问题

| ID | 风险 | 影响 | 当前证据 | 缓解 | 阶段 |
|---|---|---|---|---|---|
| R1 | FFmpeg 宿主结果扩展到真实业务 | 320×240 矩阵通过不等于生产线程/队列和长稳 | 32/32 P0 矩阵、双后端零 D2D、严格 PTS | 真实应用、多 slice、1080p/4K、多流和长稳 | 1B–P1 |
| R2 | NPP 数值/ABI/头文件边界 | 3 函数必须逐字节一致；头文件专有；FFmpeg libnpp 标记 nonfree | Stage A 已集成；P2P3/P3P2 逐字节通过；resize golden 未生成 | 11.5 golden 对照；nonfree 发布方案法务评审见[总体方案](CUDA兼容层总体方案.md) | 第 2 步 |
| R3 | 目标平台差异 | AMD 布局/驱动/性能不能直接外推 | 仅 AMD 验证机 | 目标平台能力检查；运行时能力探测；Gate 见[总体方案](CUDA兼容层总体方案.md) | 0/1B |
| R4 | 长尾场景 | 多流/seek/flush/crop/动态分辨率/长期参考 | 未覆盖 | 场景矩阵；缺陷收敛 | P1 |

### 13.2 工程实现约束

| ID | 风险 | 影响 | 当前证据 | 缓解 | 阶段 |
|---|---|---|---|---|---|
| R5 | 同步异步化 | 主机同步限制吞吐 | S1–S8 通过；异步未落地 | 保留主机基线；sync_file 协同项 | P1/P2 |
| R6 | 不支持的码流/格式 | 花屏/静默错误 | 场/MBAFF 显式失败 | 明确返回不支持；不伪成功 | 持续 |
| R8 | 依赖/版本漂移 | FFmpeg 内部对象、Mesa/ROCm 升级 | 绑定特定版本 | 固定版本；SBOM；回归；许可证义务按[总体方案](CUDA兼容层总体方案.md)评审 | 持续 |

后端特有约束和退出边界：

- **VAAPI**：profile/字段映射错误会导致花屏；pitch/offset/separate layers 处理错误会破坏布局；surface 池不足会覆盖参考帧；方案 Linux-only；FFmpeg 内部对象带来版本漂移与 LGPL 义务评审要求（法务结论见[总体方案](CUDA兼容层总体方案.md)）。目标平台无 LINEAR 或 DMA-BUF 导入能力时，只能明确降级 tiled + GPU 转换；仍不满足业务时重新评估后端。
- **Vulkan**：tiled→linear 多一次拷贝；双队列 semaphore 生命周期、DPB 槽位/参考集合和驱动版本差异需持续回归；`reconfigure` 未实现，h264_cuvid 的序列 cache 仍为单槽；h264_nvdec 依赖合成的参数集，只覆盖单 slice/默认缩放矩阵等 P0 子集。目标平台无视频队列/解码扩展或转换成本不可接受时，按同级评审结果决定，具体路线决策见[总体方案](CUDA兼容层总体方案.md)。
- **Mesa frontends**：上游接受度低，长期 fork 维护和 Mesa 驱动回归面是核心风险；NPP/客户 kernel 仍需独立投入，无法形成近期交付。本章设计不能当作已具备平台能力。

### 13.3 低概率防御与运维边界

这些项目必须保留防御设计，但“低概率”不是已量化的发生概率，也不降低畸形输入、安全或设备异常项的验收要求。

| ID | 风险 | 影响 | 当前证据 | 缓解 | 阶段 |
|---|---|---|---|---|---|
| R7 | 安全：畸形码流/尺寸/fd | parser 崩溃、越界、泄漏 | 正常码流为主 | fuzz/ASan/UBSan；尺寸检查；fd 所有权 | 1B–P1 |
| R9 | 部署/库冲突 | 误加载系统/NVIDIA 库 | 未形成矩阵 | 安装目录/SONAME/loader 策略；共存测试 | P1 |
| R10 | 商业承诺放大 | “CUDA 软件都能跑” | — | 对外只描述已验收范围 | 发布 |
| R11 | 运维：诊断不足 | 跨 CUVID/VAAPI/HIP/DMA-BUF 的错误难以定位 | 以实验日志为主 | 统一记录阶段/会话/codec/返回码/surface/fence 状态；默认不记录原始码流 | 1B–P1 |
| R12 | 运维：异常恢复 | 驱动错误、设备复位、等待超时导致死锁/残留 | flush/destroy 基础测试 | 注入错误/超时/设备丢失；等待有界超时与恢复策略 | P1 |

### 13.4 实现缺口台账

| # | 目标设计 | 当前代码 | 缺口 | 影响 | 验收标准 |
|---|---|---|---|---|---|
| G1 | H.264 profile 映射：Baseline→ConstrainedBaseline、Main→Main、High→High | `vaapi_h264_decoder.cpp` 固定 `VAProfileH264High` | Baseline/Main 未按 profile 选择 | Baseline/Main 码流兼容性未证实 | 三类 profile 建会话/解码逐字节 |
| G2 | `cuvidGetDecodeStatus` 反映真实解码完成/错误 | 只查内部状态表，不主动 `vaSyncSurface` | 状态语义偏弱 | 宿主可能读到未完成状态 | 状态与 `vaSyncSurface` 一致 |
| G3 | `cuvidReconfigureDecoder` 支持分辨率/目标尺寸变化 | 两后端均返回 `NOT_SUPPORTED` | 动态分辨率不可用 | 长尾场景失败 | destroy/recreate 或后端等价实现 |
| G4 | `CUVIDPROCPARAMS.output_stream` 接入 | 未接入，map 同步 | 异步 map 语义缺失 | 性能语义与 CUDA 不同 | 明确实现或文档化限制 |
| G5 | 场/MBAFF 显式不支持 | 已显式返回不支持 | 无 | 符合 P0 边界 | 错误码/日志 |
| G6 | 多流 parser/VkSequenceCache 键控 | 单序列/单槽 | 多流不可用 | 8 路/多实例风险 | 多 decoder 并发逐字节 |
| G7 | seek/flush/crop/动态分辨率 | 部分 flush；crop/动态分辨率未验收 | 长尾场景缺口 | P1 阻塞 | 场景矩阵 |
| G8 | NPP 3 函数 + nppc | Stage A 已构建；FFmpeg `scale_npp` 已加载；颜色转换联合链路逐字节通过 | resize NVIDIA golden、11.5 ABI、nonfree 发布路径、nppc API | 阶段 A 未完全验收 | 逐字节 + 联合链路 |
| G9 | FFmpeg 双路径验收 | 两后端 CUVID CPU/CUDA、NVDEC 安全/零拷贝、PTS 容器全部通过 | 真实业务线程/队列、多 slice、长稳未覆盖 | 生产交付前继续扩展 | 双路径像素/帧数/顺序/资源 |
| G10 | 目标平台能力门禁 | 仅 AMD 验证机 | 目标平台未跑 | 结论不可外推 | 目标平台能力检查通过；Gate 定义见[总体方案](CUDA兼容层总体方案.md) |
| G11 | 异步跨栈 fence | 未实现 | 只有主机同步 | 高吞吐受限 | sync_file/syncobj 端到端 |
| G12 | 发布物/安装/诊断/回退 | 有构建和部分诊断 | 发布矩阵/安装/回退未形成 | 生产交付缺口 | 发布检查单 |

### 13.5 开放问题

1. 首批客户是否都能提供源码、构建环境和业务回归条件？
2. 跨平台（Windows/Android）是否是明确业务需求？决定 Vulkan 的战略权重，路线决策见[总体方案](CUDA兼容层总体方案.md)。
3. 目标平台是否具备 LINEAR surface、DMA-BUF 导入和所需 profile？
4. NPP 逐字节一致是否在目标平台/编译器组合下仍成立？
5. `h264_nvdec` 零拷贝在 P0 四路通过，但需要宿主分配额外 surface；真实应用能否接受这一约束，仍需客户配置和长稳验证。
6. 异步 fence 的 ROCr 用户态缺口由谁承担、何时排期？排期决策见[总体方案](CUDA兼容层总体方案.md)。
7. 是否需要在 P1 扩展 P010/其他 codec/NPP 函数？如需要，重新估算，成本与周期见[总体方案](CUDA兼容层总体方案.md)。

---

## 14. 追溯矩阵

下表把验收项追溯到组件、源码/测试和证据。Gate 列仅保留追溯标签，Gate 定义和通过决策以[总体方案](CUDA兼容层总体方案.md)为准。

| 验收项 | API/组件 | 源码/测试 | 证据 | Gate |
|---|---|---|---|---|
| 符号面 | libcuda 82 / libnvcuvid 22 | `verify_symbols`、`verify_cuvid_symbols` | P0 结果 | G1/G3 |
| libcuda 功能子集 | 设备/上下文/内存/流/事件 | `test_func_full` | 47 PASS 历史记录 | G3 |
| CUVID 会话 | `cuvidCreateDecoder` | `verify_cuvid_session` | P0 PASS | 1B |
| parser | SPS/PPS/slice/DPB | `verify_cuvid_parser` | I/P/B PASS | 1B |
| mapped 指针 | `cuvidMapVideoFrame64` | `verify_kernel_read` | `errors=0` | 1B |
| VAAPI 像素 | VAAPI 解码→导出→HIP | `verify_decode_to_hip` | 四路 82/82 | 1A/1B |
| Vulkan 像素 | VK 解码→拷贝→导出→HIP | `verify_vk_decode_to_hip` | 四路 82/82 | 1A/1B |
| FFmpeg 双路径 | `h264_cuvid`/`h264_nvdec` | `run_ffmpeg_compare.sh` | ✅ P0 四路双后端 32/32；PTS 8/8 | 1B |
| 零拷贝计数 | `h264_nvdec` D2D=0 | `CUDA_SHIM_TRACE_COPY=1` | ✅ 双后端四路 D2D=0（需额外 surface） | 1B |
| NPP 参考 | 3 函数 | `npp_p0_reference.cpp` | NVIDIA 11.5.1.107 已固化 | 第 2 步 |
| NPP AMD | 3 函数 + nppc | Stage A + 独立 FFmpeg | ⚠️ 颜色转换逐字节；resize golden 未验收；nonfree 发布未决 | 第 2 步 |
| 联合链路 | 解码→NPP | 待集成 | ⚠️ Stage A 本地自检，未接 FFmpeg | 第 2 步 |
| 复杂场景 | seek/crop/动态分辨率/多流 | 验证手册场景矩阵 | ❌ 未覆盖 | P1 |
| 稳定性 | 1/8 路 30 分钟 | 验证手册 | ❌ 未覆盖 | G3 |
| 目标平台 | 全部 | Gate G1 | ❌ 未跑 | G1/G4 |

---

## 15. 版本历史（摘要）

- **v0.1（2026-10-28/29）**：初稿，形成双轨架构及各子方案。
- **v1.0～v1.9（2026-11-02～03）**：整合 NVDEC/VAAPI 格式对比、DMA-BUF、线性布局、P010、同步、Vulkan、libcuda 和 parser 等 AMD 概念验证结果。
- **v1.10～v1.18（2026-11-03）**：P0 文档优化：修正技术表述和过度承诺，增加证据等级、决策摘要、首期范围、阶段通过条件及接口/数据通路边界。
- **v1.19～v1.25（2026-11-03）**：首轮 P1 文档补充：交付风险、接口覆盖矩阵、NPP/HIPIFY/NVENC 验证方案、性能基线、版本发布与回退要求、复现模板。
- **v1.26（2026-11-03）**：压缩版本历史，删除重复旧结论。
- **v1.27（2026-11-03）**：全文一致性检查；修正 HIPIFY/shim 范围、HIP semaphore、线性创建路径及主机同步性能等口径。
- **v1.28（2026-11-03）**：实验 C 单帧 I-only 通过；沉淀 profile/separate layers/pitch 逐行三个约束；实验 D 实证 packed headers 静默回退；实验 E 程序就绪；实验 A/B 补回源文件。
- **v1.29（2026-11-03）**：实验 C 多帧通过（30 帧逐字节 + 池复用 + flush + fd 无泄漏）。
- **v1.30（2026-11-03）**：实验 C I/P 通过（1 IDR + 29 P），P 帧 DPB 参考复用闭合。
- **v1.31（2026-11-04）**：实验 C I/P/B 通过（1 I + 10 P + 19 B），B 帧双参考列表 + 显示序重排闭合。
- **v1.32（2026-11-04）**：P0 范围与证据基线回写；交付收敛为 CUVID 解码 + 3 个 NPP 函数；记录双机环境与 NVIDIA NPP 参考。
- **v1.33（2026-11-04）**：CUVID shim parser 改为复用 FFmpeg 内部解析；修复起始码边界、`nal_ref_idc`、空 DPB `erase` UB；I/P/B 解析测试通过。
- **v1.34（2026-11-05）**：Vulkan OPTIMAL tiling 结构性约束；三条候选路线取舍；异步同步正解 sync_file 桥接；VK 后端验收宿主澄清。
- **v1.35（2026-11-05）**：勘误 `vkCmdCopyImageToBuffer` 对源 image 无 LINEAR 限制；转换首选硬件拷贝，补参考实现。
- **v1.36（2026-11-06）**：Vulkan P0 闭环（I/IP/IPB 各 30 帧逐字节）；沉淀双队列/DPB 分离/`pSetupReferenceSlot` 等 5 个实现约束。
- **v1.37（2026-11-07）**：CUVID shim Vulkan 后端接入；`DecoderBackend` + `CUVID_SHIM_BACKEND=vk`；I/IP/IB 各 30 帧逐字节。
- **v1.38（2026-11-12）**：P0 双后端闭环（VAAPI 82/82、Vulkan 82/82，`verify_kernel_read` errors=0）；修复 Vulkan B 帧 UAF。
- **v2.0（2026-11-12）**：四份方案文档整合；本文收敛为技术事实与证据。
- **v3.0（2026-11-12）**：三份方案文档重切；本文定位为“技术事实与验证证据”，目标架构与实现设计移至[总体方案](CUDA兼容层总体方案.md)。
- **v4.5（2026-11-13）**：真实 CUDA/NPP 12 头文件构建 Stage A；FFmpeg `scale_npp` 独立 nonfree 构建加载 shim；P2P3/P3P2 双后端逐字节；补充 nonfree 与 11.5 golden 边界。
- **v4.4（2026-11-13）**：落地 NPP Stage A HIP shim、五库 SONAME、参考/候选探针和比较器；本地自检通过，NVIDIA golden 与 FFmpeg 集成未完成。
- **v4.3（2026-11-13）**：明确 NPP resize 插值最终覆盖全部 NPP flags，按 cubic/NN/linear → FFmpeg 全选项 → NPP 全模式分阶段实现和验收。
- **v4.2（2026-11-13）**：修复 NVDEC unsafe 的 frame pool 耗尽与 Vulkan h264_nvdec 缺失参数集；FFmpeg 宿主矩阵更新为 32/32，PTS 8/8。
- **v4.1（2026-11-13）**：标注后端尚未决策、VAAPI/Vulkan 同级；补 FFmpeg 宿主 32 组合基线（22 PASS / 10 FAIL）状态。
- **v4.0（2026-11-13）**：按主题归并公共实现、VAAPI/Vulkan/Mesa 后端、构建诊断、阶段与后端验收、性能稳定性方法、风险/缺口/开放问题和追溯矩阵；本文调整为“技术实现、验证证据与风险边界”权威文档，技术事实截止日及 P0 数字和状态不变。

---

## 16. 证据与源码索引

### 16.1 关键实现源码

| 主题 | 文件 |
|---|---|
| CUDA driver → HIP | `cuda_shim/src/libcuda.cpp` |
| CUVID 符号面/句柄/map | `cuda_shim/src/libnvcuvid.cpp` |
| parser/DPB/显示序 | `cuda_shim/src/cuvid_parser.cpp`、`cuvid_parser.h` |
| CUVID → VAAPI 参数翻译 | `cuda_shim/src/h264_picture_translate.cpp` |
| VAAPI 解码 | `cuda_shim/src/vaapi_h264_decoder.cpp` |
| Vulkan 解码/拷贝/导出 | `cuda_shim/src/vulkan_h264_decoder.cpp` |
| h264_nvdec 参数集合成 | `cuda_shim/src/h264_direct_params.cpp` |
| 后端选择 | `cuda_shim/src/backend_dispatch.cpp` |
| SPS/PPS 交接 | `cuda_shim/src/vk_sequence_cache.h` |
| 帧映射/布局门禁 | `cuda_shim/src/hip_frame_mapper.cpp` |
| NPP Stage A HIP 实现 | `npp_shim/src/nppig_resize.cpp`、`npp_shim/src/nppicc_color.cpp` |
| NPP 参考/候选探针 | `tests/npp/npp_stage_a_probe.cpp`、`tests/npp/compare_stage_a.py` |
| 符号/会话/parser/kernel 读取测试 | `cuda_shim/tests/` |
| VAAPI/VK 端到端像素验证 | `tests/zerocopy/` |
| 同步协议 | `tests/sync/` |
| NPP 旧参考 | `tests/parser/npp_p0_reference.cpp` |
| NPP Stage A 探针 | `tests/npp/` |
| 构建 | `cuda_shim/Makefile`、`npp_shim/Makefile` |

### 16.2 实验程序索引

以下程序须在与实验相匹配的 AMD/NVIDIA、ROCm/CUDA、Mesa 及依赖环境中运行，不统一承诺均可直接构建或运行。各项结论仅适用于已记录的环境和测试项。

| 目录/状态 | 程序或命令 | 验证内容 |
|---|---|---|
| `tests/zerocopy/` | `verify_zerocopy` | 实验 1/2/3（共存、边界 1、边界 2） |
| `tests/zerocopy/` | `retry_exp3` / `retry_exp3_v2` | 边界 2 失败定位（pitch 对齐根因） |
| `tests/zerocopy/` | `make loop-decode` | 解码闭环：VAAPI 硬解 vs 软解逐字节 |
| `tests/zerocopy/` | `make loop-encode` | 编码闭环：图案→VAAPI 编码→解码回读 |
| `tests/zerocopy/` | `hip_read_surface` | HIP 读写外部 fd（像素往返工具） |
| `tests/zerocopy/` | `make linear` | 线性/tiled modifier、内容正确性及耗时对比 |
| `tests/zerocopy/` | `make multires` | 320×240 至 4K 多分辨率线性验证 |
| `tests/zerocopy/` | `make modifier-linear` | 主动限定 LINEAR modifier 的生产路径验证 |
| `tests/zerocopy/` | `make p010` | P010 线性 surface 创建、导出及 HIP 导入 |
| `tests/zerocopy/` | `make -f verify_decode_to_hip.make run` / `run-p` / `run-b` | 实验 C：libva 真实解码→HIP 读端到端像素校验 |
| `tests/zerocopy/` | `make -f verify_hip_to_encode.make run` | 实验 D：HIP 写→VAAPI 编码端到端 |
| `tests/zerocopy/` | `make -f verify_portability.make run` | 实验 E：目标平台 5 项能力止损阀探测 |
| `tests/zerocopy/` | `make -f verify_vk_decode_to_hip.make run-b` / `run-p` / `run-i` | Vulkan P0：VK 解码→拷贝→导出→HIP 读 |
| `tests/sync/` | `verify_sync` | 同步 S1–S8 |
| `tests/sync/` | `make semaphore` | HIP semaphore 能力及 DRM syncobj/sync_file 基础原语 |
| `tests/vulkan-video/basic/` | `verify_vulkan_video` | Vulkan Video 基础 + 通用零拷贝 |
| `tests/vulkan-video/full/` | `verify_vulkan_video_full` | Vulkan Video V1–V4、V6 |
| `cuda_shim/` | `make check` / `check_func` | 符号面（模拟 FFmpeg dlsym）/ libcuda 47 项功能 |
| `cuda_shim/` | `make verify_cuvid_session` / `verify_cuvid_parser` | CUVID 会话创建 / parser 回调 |
| `cuda_shim/` | `make verify_kernel_read` | mapped `dev_ptr` 的 HIP kernel 读取 + event 顺序 |
| `tests/parser/` | `make -f verify_parser_reuse.make` | FFmpeg parser 复用及 VAAPI 参数填充 |
| `tests/parser/` | `npp_p0_reference` | NVIDIA NPP 11.5 三个 P0 函数参考输出 |
| `benchmarks/` | `npp_benchmark` / `vpp_benchmark` / `nvdec_benchmark` | NPP/VPP/NVDEC 能力与性能直测 |
| `benchmarks/scripts/` | `collect_ffmpeg_api.py` | FFmpeg 实际 CUVID/CUDA/NPP 静态调用面采集 |

### 16.3 证据文件

| 证据 | 位置 |
|---|---|
| P0 双后端四路码流结果 | `docs/evidence/P0闭环验证结果_2026-11-12.md` |
| NPP NVIDIA 参考输出与 SHA256 | `docs/evidence/results/npp-nvidia-11.5/`、`npp-nvidia-11.5-sha256.log` |
| NPP 26 项直测结果 | `docs/evidence/results/npp_results.json` |
| NVDEC/VAAPI 格式矩阵 | `docs/architecture/NVDEC与VAAPI解码对比.md`、`docs/evidence/results/` |
| 实验 C | `tests/zerocopy/verify_decode_to_hip.cpp` / `.make` |
| Vulkan P0 | `tests/zerocopy/verify_vk_decode_to_hip.cpp` / `.make` |
| 同步 S1–S8 | `tests/sync/verify_sync.cpp` |
| 异步 fence 探测 | `tests/sync/verify_semaphore_async.cpp` |
| parser 复用 | `tests/parser/verify_parser_reuse.cpp` / `.make` |
| libcuda 功能子集 | `cuda_shim/tests/test_func_full.cpp` |
| 符号面 | `cuda_shim/tests/verify_symbols.cpp`、`verify_cuvid_symbols.cpp` |
| CUVID 会话/parser | `cuda_shim/tests/verify_cuvid_session.cpp`、`verify_cuvid_parser.cpp` |
| mapped 指针 | `cuda_shim/tests/verify_kernel_read.cpp` |
| Vulkan 后端根因/修复 | `docs/architecture/Vulkan后端实现.md` §8 |
| 复现步骤与排障 | `docs/verification/验证手册.md` |

---

## 附录 A：环境变量与诊断开关

| 变量 | 作用 |
|---|---|
| `CUVID_SHIM_BACKEND=vaapi / vk` | 选择解码后端，默认 vaapi |
| `CUVID_SHIM_DRM_NODE` | 指定 DRM render node，默认 `/dev/dri/renderD128` |
| `CUVID_SHIM_DEBUG_VK=1` | 每帧打印 Vulkan 解码参数 |
| `CUVID_SHIM_NOCOPY=1` | Vulkan 只解码不导出 |
| `CUVID_DUMP_NV12_DIR` | dump NV12 帧用于逐字节比对 |
| `CUVID_SHIM_DEBUG_DPB` | 打印 DPB/参考列表状态 |
| `CUVID_SHIM_TRACE_SURF` | 打印 surface 分配/decode/map/unmap |
| `CUVID_SHIM_DEBUG_VASUM` | 绕过 HIP 直接读 VA surface，定位“解码没写”还是“导入读错” |
| `CUVID_SHIM_PLAIN_SURFACES` | 建普通 VA surface，用于对比 modifier 问题 |
| `CUDA_SHIM_TRACE_COPY=1` | 统计 D2D 拷贝次数，区分 `h264_nvdec` 零拷贝与 `h264_cuvid` D2D |
| `RADV_PERFTEST=video_decode` | 验证机脚本显式设置；是否需要因 Mesa 版本而异 |
