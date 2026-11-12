# AMD 验证机交接说明（给接管的 AI / 工程师阅读）

> 目标：在 AMD 验证机（RX 7900 XTX，gfx1100，ROCm 7.2，Mesa 25.x RADV）上，
> 把 `cuda_shim` 的 **Vulkan Video 后端** 从「代码基础架构」推进到「编译 + 运行验证」。
> 本文写清了：代码从哪来、怎么编译、架构是什么、已知坑、下一步怎么做。
> **先完整读完再动手。**

---

## 0. 一句话现状

- `libnvcuvid.so.1`（CUVID shim）原本只有 VAAPI 后端。现已新增 **Vulkan Video 后端**
  （`src/vulkan_h264_decoder.{h,cpp}`），通过 `DecoderBackend` 接口与 VAAPI 后端并列，
  用环境变量 `CUVID_SHIM_BACKEND=vk` 切换。
- 开发机是 **NVIDIA RTX 4060**，无 AMD GPU、无 libvulkan 的 video 命令导出，
  所以**只做了 `-fsyntax-only` 语法自检（7 个编译单元全部 0 error）**，**从未编译链接、从未运行**。
- 你要做的是：在 AMD 机上 `make libnvcuvid.so.1` 编译通过 → 跑通解码 → 逐字节校验。

---

## 1. 需要拷贝的东西（从开发机 → AMD 机）

### 1.1 必拷：代码本体

```
cuda_shim/                         # 整个目录（约 1.5M）
├── Makefile                       # 已改：加 vulkan 后端源 + -I mesa + -lvulkan
├── README.md                      # 组件构建/运行入口
├── include/cuda_shim_types.h
├── src/
│   ├── libnvcuvid.cpp             # 已改：改用 DecoderBackend 接口
│   ├── libcuda.cpp
│   ├── vaapi_h264_decoder.{h,cpp}   # VAAPI 后端（未改）
│   ├── h264_picture_translate.{h,cpp}
│   ├── hip_frame_mapper.{h,cpp}     # 已改：新增 map_fd()（Vulkan 路径用）
│   ├── cuvid_parser.{h,cpp}         # 已改：加 SPS/PPS stash → VkSequenceCache::store()
│   ├── decoder_backend.h            # 新增：后端接口
│   ├── backend_dispatch.{h,cpp}     # 新增：后端工厂（CUVID_SHIM_BACKEND 切换）
│   ├── vulkan_h264_decoder.{h,cpp}  # 新增：Vulkan Video 后端（核心，~1000 行）
│   └── vk_sequence_cache.h          # 新增：SPS/PPS 进程级缓存
└── tests/                         # 组件内验证程序（verify_cuvid_session 等）
```

### 1.2 必拷：FFmpeg **7.1** 头文件 + 复用 .o + libavutil（关键）

> ⚠️ 新代码按 **ffmpeg-7.1 的 API** 写的，**不能**用 ffmpeg-6.1：
> 7.1 的 `ff_h2645_packet_split()` 是 **8 参数**（`flags`），6.1 是 **11 参数**（`is_nalff/nal_length_size/small_padding/use_ref`）。
> 且 7.1 的 `SPS.log2_max_poc_lsb` 已含 +4（6.1 也含 +4，这点一致），但 `H2645NAL` 字段/`h264_parse.h` 差异仍在。
> **结论：要么拷 7.1，要么让接管的 AI 把代码改回 6.1 签名，二选一，建议拷 7.1。**

拷贝 ffmpeg-7.1 的这些（**不用拷整个 623M 源码树**）：

```
ffmpeg-7.1/config.h                       # h264_parse.h 等头会 #include "config.h"
ffmpeg-7.1/config_components.h            # 部分头依赖
ffmpeg-7.1/libavcodec/*.h                 # 全量头（8M；只拷 .h，不要 .o/.a/.so）
ffmpeg-7.1/libavutil/*.h                  # 全量头（1.2M；只拷 .h）
ffmpeg-7.1/libavcodec/h2645_parse.o
ffmpeg-7.1/libavcodec/h264_ps.o
ffmpeg-7.1/libavcodec/h2645_vui.o
ffmpeg-7.1/libavcodec/h2645data.o
ffmpeg-7.1/libavcodec/h264data.o
ffmpeg-7.1/libavcodec/h264_parse.o        # 新增：提供 ff_h264_init_poc / ff_h264_parse_ref_count
ffmpeg-7.1/libavcodec/golomb.o
ffmpeg-7.1/libavcodec/mathtables.o
ffmpeg-7.1/libavcodec/refstruct.o
ffmpeg-7.1/libavutil/libavutil.so         # 3.6M，soname .so.59
```

> 这 9 个 .o 就是 Makefile 里 `FF_OBJS` 的完整列表，`h264_parse.o` 是这次**新增**进去的
> （之前 VAAPI 版只有 8 个，没 `h264_parse.o`）。

拷贝方式（在开发机执行，假设目标机有 ssh 别名 `amd`）：

```bash
cd /path/to/repo
rsync -av cuda_shim/ amd:~/cuda_shim/                # 代码本体
rsync -av $HOME/ffmpeg-7.1/config.h \
           $HOME/ffmpeg-7.1/config_components.h \
           amd:~/ffmpeg-7.1/                          # 确保目标目录存在
rsync -av --include='*/' --include='*.h' --exclude='*' \
           $HOME/ffmpeg-7.1/libavcodec/ amd:~/ffmpeg-7.1/libavcodec/
rsync -av --include='*/' --include='*.h' --exclude='*' \
           $HOME/ffmpeg-7.1/libavutil/ amd:~/ffmpeg-7.1/libavutil/
# 9 个 .o + libavutil.so
rsync -av \
  $HOME/ffmpeg-7.1/libavcodec/h2645_parse.o \
  $HOME/ffmpeg-7.1/libavcodec/h264_ps.o \
  $HOME/ffmpeg-7.1/libavcodec/h2645_vui.o \
  $HOME/ffmpeg-7.1/libavcodec/h2645data.o \
  $HOME/ffmpeg-7.1/libavcodec/h264data.o \
  $HOME/ffmpeg-7.1/libavcodec/h264_parse.o \
  $HOME/ffmpeg-7.1/libavcodec/golomb.o \
  $HOME/ffmpeg-7.1/libavcodec/mathtables.o \
  $HOME/ffmpeg-7.1/libavcodec/refstruct.o \
  $HOME/ffmpeg-7.1/libavutil/libavutil.so \
  amd:~/ffmpeg-7.1/    # 注意按相对路径放对位置（libavcodec/ 下放 .o）
```

### 1.3 必拷：nv-codec-headers 头（小，440K）

```
nv-codec-headers/include/            # 含 ffnvcodec/dynlink_cuda.h、dynlink_cuviddec.h、dynlink_nvcuvid.h
```

### 1.4 Vulkan 头：AMD 机上通常已有，二选一

- **优先用 AMD 机自带的** `libvulkan-dev`（系统 `/usr/include/vulkan`，含 `vk_video/*` std 头）。
  若 AMD 机的系统 vulkan 头版本 ≥ 1.4.318 就够用（P0 当时用的 Mesa 自带 1.4.328）。
- **兜底**：拷贝开发机的 `mesa/include/`（5.9M，含 `vulkan/vulkan.h` + `vk_video/vulkan_video_codec_h264std.h` 等）。
  Makefile 里 `MESA_DIR ?= $HOME/mesa`，改指向实际路径即可。

### 1.5 测试码流 + 参考（跑端到端校验用，可选但建议）

```
testdata/h264/test_320x240.h264     # I-only
testdata/h264/test_p_320x240.h264   # I/P
testdata/h264/test_b_320x240.h264   # I/P/B
```

> 参考 YUV 不随仓库提交；在 `tests/zerocopy/` 下运行
> `make -f verify_decode_to_hip.make ref_320x240.yuv ref_p_320x240.yuv ref_b_320x240.yuv`
> 会自动生成，详见 `testdata/README.md`。


### 1.6 不用拷

- 整个 `ffmpeg-7.1` 源码树（623M）、`mesa` 全树（1.1G）——只要 1.2/1.4 列的最小集。
- `tests/zerocopy/verify_vk_decode_to_hip.cpp`（P0 PoC）——它在 AMD 机上**已经存在并通过**，
  新后端只是把它内嵌进 shim，无需再拷。

---

## 2. AMD 机上的编译

```bash
cd cuda_shim
# 覆盖路径（按实际）
make libnvcuvid.so.1 \
     ROCM_DIR=/opt/rocm \
     NVCODEC_DIR=$HOME/nv-codec-headers/include \
     FFMPEG_DIR=$HOME/ffmpeg-7.1 \
     MESA_DIR=$HOME/mesa        # 或系统 vulkan 头路径
```

期望：生成 `libnvcuvid.so.1`（+软链 `libnvcuvid.so`），无 undefined symbol。

**编译若报错，优先查这几处（已知风险点）**：

1. **Vulkan 头路径**：`-I$(MESA_DIR)/include` 必须能看到 `vulkan/vulkan_core.h` 里的
   `VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR` 等（Mesa 25.x 自带 1.4.328 有）。
   若用系统 `libvulkan-dev`，把 `MESA_DIR` 指向 `/usr`（即 `-I/usr/include`），并确认版本。
2. **ffmpeg 头版本不匹配**：`ff_h2645_packet_split` 报参数数量错 → 说明用了 6.1，见 §1.2 二选一。
3. **`h264_parse.o` 缺**：链接报 `ff_h264_init_poc` undefined → 确认 9 个 .o 都拷了且路径对。
4. **`CUDA_ERROR_*` 未定义**：本代码里已用 `#ifndef` 兜底补了 `OUT_OF_MEMORY`，其余走
   `cuda_shim_types.h`（`libnvcuvid.cpp` 用）与 `vulkan_h264_decoder.cpp` 内部兜底，应该不会再出。

---

## 3. 架构速读（接管前必懂）

```
FFmpeg (h264_cuvid / h264_nvdec)  — dlopen+dlsym，源码不改不重编
    │
libnvcuvid.cpp  ── DecoderBackend 接口 ──┬─ VaapiBackend  → VaapiH264Decoder   (Tier-1 默认)
                                         └─ VulkanBackend → VulkanH264Decoder  (Tier-2 新增)
```

- `CUVID_SHIM_BACKEND` 未设 / 非 `vk` → 走 VAAPI；设 `=vk` → 走 Vulkan。
- **SPS/PPS 接缝**：CUVID 宿主只把 slice NAL 喂给 `cuvidDecodePicture`，SPS/PPS 由 shim 自己的
  `CuvidParser`（`cuvid_parser.cpp`）解析后，通过 `VkSequenceCache`（进程级单槽）转给
  `VulkanH264Decoder`。parser 存 **NAL header 起、无 start code** 的字节，`VkSequenceCache::store()`
  内部给 SPS/PPS 各前置 `00 00 00 01`，后端再 `ff_h2645_packet_split` 重新切分。
- **懒建 session**：`cuvidCreateDecoder` 时 SPS/PPS 可能还没到 → `initialize` 只建
  instance/device/队列；`ensure_session()` 在 SPS/PPS 就绪后建 video session + 图像池 +
  导出 buffer + 同步对象；`decode()` 每次先 `ensure_session()`。
- **P0 已验证的硬核细节原样搬**（详见 `src/vulkan_h264_decoder.cpp` 注释）：
  双队列、`pSetupReferenceSlot` 每帧非 NULL、DPB 分离、Annex-B + start code、
  `vkCmdCopyImageToBuffer`→`vkGetMemoryFdKHR`→HIP import、主机同步。
- 逐帧 `decode()` 里：FFmpeg 重解析 SPS/PPS + `parse_slice_header` + `ff_h264_init_poc`
  （POC 状态 `poc_ctx_` **跨帧持久**，IDR 重置）→ 填 `StdVideoDecodeH264PictureInfo` → 解码 → 拷贝 → 导出 fd。

---

## 4. 运行验证（三步）

### 4.1 冒烟（不校验像素，先看能不能建 VK session）

```bash
cd cuda_shim
make verify_cuvid_session   # 现有测试程序，走同一 CUVID 表面
CUVID_SHIM_BACKEND=vk ./verify_cuvid_session
```

期望：`PASS`（能建 H.264/NV12 解码器）。注意 `verify_cuvid_session` 目前只建 decoder 不喂流，
SPS/PPS 到不了，VK 后端会「建 instance/device 成功、session 未建」——属预期，不算失败。

### 4.2 端到端像素校验（真正的验收）

需要一个**新测试程序**：喂 `test_b_320x240.h264` 走
`cuvidParseVideoData`（parser 回调）→ `cuvidDecodePicture` → `cuvidMapVideoFrame64` → 读 HIP 指针 →
对比 `ref_b_320x240.yuv`。结构对标 `cuda_shim/tests/verify_cuvid_parser.cpp`（现有 parser 测试），
在 `pfnDecodePicture` 回调里 decode、在 display 回调里 map+compare。

验收标准（对齐 TODO P0）：
1. I / IP / IPB 三码流各 ≥30 帧，HIP 读回逐字节 vs 软解参考一致。
2. fd 计数前后一致（无泄漏）。

### 4.3 用 ffmpeg 直接跑（终极验证，宿主无感）

```bash
export CUVID_SHIM_BACKEND=vk
export LD_LIBRARY_PATH=$HOME/cuda_shim:$LD_LIBRARY_PATH
ffmpeg -hwaccel cuda -i test_b_320x240.h264 -f null -
# 或走 h264_cuvid 解码器路径
```

> 需要 `libcuda.so.1` 也已就位（`make libcuda.so.1`），FFmpeg 通过 dynlink 一次性 dlsym
> 82 个 CUDA 符号 + 22 个 CUVID 符号，缺一即 fail。

---

## 5. 已知限制 / 未做（接手时明确，别当成 bug 反复查）

| # | 项 | 状态 |
|---|---|---|
| 1 | `cuvidReconfigureDecoder` | stub（与 VAAPI 一致）；Vulkan 需销毁重建 session |
| 2 | 异步同步（sync_file 桥接） | 未做；首期主机同步 `vkWaitForFences` 够用 |
| 3 | 多流 / seek / flush | 单流单序列；`VkSequenceCache` 单槽，多流需键控（P1-2） |
| 4 | HEVC / VP9 / AV1 | 只有 H264（对齐 P0） |
| 5 | `poc_type=1`、加权预测、场编码、SP/SI | `parse_slice_header` 里显式 return not-supported（与 P0 同边界） |

---

## 6. 建议的下一步（按优先级）

1. **编译**：`make libnvcuvid.so.1`，修掉 §2 的路径/版本问题。
2. **冒烟**：`CUVID_SHIM_BACKEND=vk ./verify_cuvid_session` 不崩。
3. **写端到端测试**（§4.2），跑 I-only 一帧先通，再 IP / IPB 各 30 帧 + fd 泄漏检查。
4. 通了之后，把结论回写 `../architecture/vulkan-backend.md` 的「状态」和两个方案文档（`../architecture/cuda-compat-plan-integrated.md` / `cuda-compat-plan-optimized.md`），
   并标注证据等级（AMD-PoC）。

---

## 7. 若卡住，先看这些文件

- `../architecture/vulkan-backend.md` — 架构 + 关键决策（§3 是决策，§5 是接缝）。
- `src/vulkan_h264_decoder.cpp` — 实现，顶部注释列了所有「已对照 RADV 核实的坑」。
- `src/decoder_backend.h` / `backend_dispatch.cpp` — 接口 + 工厂。
- `src/vk_sequence_cache.h` — SPS/PPS 缓存（start code 前置逻辑在这里）。
- `src/cuvid_parser.cpp` — parser 侧 stash + store 的位置（`NAL_SPS`/`NAL_PPS` 两个 case）。

> 上游背景（可选读）：`../docs/architecture/vulkan-video-todo.md`（P0 定义）、
> `../tests/zerocopy/verify_vk_decode_to_hip.cpp`（P0 已验证的单程序 PoC，本后端就是把它内嵌进 shim）。
