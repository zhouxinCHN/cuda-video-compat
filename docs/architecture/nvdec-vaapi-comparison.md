# NVDEC 与 VAAPI 视频解码能力对比报告

## 1. 测试背景与目的

### 1.1 背景

规划自研硬件解码 SDK，功能上兼容 NVIDIA NVDEC。初步技术路线为通过 VAAPI 实现后端驱动。为评估该路线的可行性，需对比 NVDEC 与 VAAPI 在视频解码格式支持上的能力差异。

### 1.2 目的

摸清 NVDEC 与 VAAPI 在解码格式支持（编码格式 × 色度采样 × 位深）上的能力差异，为自研 SDK 的后端选型提供依据。

### 1.3 测试环境

| 项目 | NVIDIA 侧 | AMD 侧 |
|------|----------|--------|
| GPU | NVIDIA GeForce RTX 4060 | AMD Radeon RX 9070 |
| 解码硬件 | NVDEC | VCN5 |
| 驱动版本 | 595.99.02 | radeonsi (Mesa 26) |
| 访问接口 | CUVID / NVDEC API + FFmpeg | VAAPI + FFmpeg |
| FFmpeg | 6.1（自编译，含 cuvid/nvdec/vaapi/nvenc/libx264/libx265） | 6.1 |
| NVDEC SDK | libnvcuvid.so.595 + nv-codec-headers | libva（VAAPI 的接口即 libva 本身） |

---

## 2. 测试方法

### 2.1 测试层次

本测试分两个层次，分别回答不同的问题：

| 层次 | 实现方式 | 回答的问题 |
|------|---------|-----------|
| **FFmpeg 封装层** | `h264_cuvid` / `-hwaccel vaapi` | 该格式"能不能解"（功能支持性） |
| **SDK 直测层** | NVDEC cuvid API 直接调用 | 纯硬件解码性能（fps），排除 FFmpeg 封装开销 |

> 说明：测试分两层的原因——
> - **NVDEC** 的 CUVID API 与 FFmpeg 封装差异较大（需自行管理 CUDA context、parser、解码 surface 等），如果目标为"兼容 NVDEC 功能"，直接调用 CUVID API 有助于摸清 NVDEC 的接口形态，故单独写 SDK 直测脚本。
> - **VAAPI** 的 SDK 即 libva 本身，FFmpeg 的 `-hwaccel vaapi` 底层就是直接调用 libva，再单独写一遍调 libva 的脚本没有增量价值，故 VAAPI 仅测 FFmpeg 层。

### 2.2 测试矩阵维度

- **编码格式**：H.264、HEVC（H.265）、AV1、VP9、MPEG-2
- **色度采样**：4:2:0、4:2:2、4:4:4
- **位深**：8bit、10bit、12bit

共 **15 个格式组合**。

### 2.3 测试视频规格

- 分辨率：1920×1080（SDK 直测）；320×240（FFmpeg 功能测试，加速）
- 时长：5 秒（150 帧 @ 30fps）
- 生成方式：硬件编码器（h264_nvenc / hevc_nvenc / av1_nvenc / av1_vaapi）+ 软件编码器（libx264 / libx265 / libvpx-vp9，用于生成 NVDEC 硬件不支持的 4:2:2 等格式）

---

## 3. 测试命令与 SDK

### 3.1 NVIDIA FFmpeg 层测试命令

使用 CUVID 解码器进行格式支持测试：

```bash
# H.264
ffmpeg -c:v h264_cuvid -i test_h264.mp4 -f null -
# HEVC
ffmpeg -c:v hevc_cuvid -i test_h265.mp4 -f null -
# VP9
ffmpeg -c:v vp9_cuvid -i test_vp9.webm -f null -
# AV1
ffmpeg -c:v av1_cuvid -i test_av1.mp4 -f null -
# MPEG-2
ffmpeg -c:v mpeg2_cuvid -i test_mpeg2.mpg -f null -
```

### 3.2 NVIDIA NVDEC SDK 直测

**SDK 名称**：NVIDIA Video Decode API（CUVID），NVDEC 硬件解码接口。

**依赖**：
- `libnvcuvid.so`（NVIDIA 驱动自带）
- `nv-codec-headers`（头文件，来自 FFmpeg 项目）

**测试程序**：`nvdec_benchmark`（parser 模式）

**数据流**：
```
FFmpeg demux（裸码流） → cuvidCreateVideoParser → cuvidParseVideoData → cuvidDecodePicture
```

**关键命令**：
```bash
# 1. demux 出裸码流
ffmpeg -i in.mp4 -c:v copy -bsf:v h264_mp4toannexb -f h264 out.h264

# 2. SDK 直测
./nvdec_benchmark --input out.h264 --codec h264 --json result.json
```

### 3.3 AMD FFmpeg 层测试命令

使用 VAAPI 硬件加速进行格式支持测试：

```bash
# 通用 VAAPI 解码（自动选择对应硬件解码器）
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i test.mp4 -f null -
```

### 3.4 测试视频生成命令

```bash
# 通用格式（硬件编码）
ffmpeg -f lavfi -i testsrc=duration=5:size=1920x1080:rate=30 \
    -vf "format=yuv420p" -c:v h264_nvenc -preset p4 -b:v 5M test_h264.mp4

# 4:2:2 格式（NVENC 不支持，用软件编码器）
ffmpeg -f lavfi -i testsrc=duration=5:size=1920x1080:rate=30 \
    -vf "format=yuv422p" -c:v libx264 -preset ultrafast test_h264_422.mp4

# AV1（AMD 侧，用 VAAPI 硬件编码）
ffmpeg -y -vaapi_device /dev/dri/renderD128 \
    -f lavfi -i testsrc=duration=1:size=320x240:rate=30 \
    -vf 'format=nv12,hwupload' \
    -c:v av1_vaapi test_av1.mp4
```

---

## 4. 格式支持矩阵

### 4.1 总对比表

| 格式 | 色度采样 | 位深 | NVDEC | VAAPI | 差异 |
|------|---------|------|:-----:|:-----:|------|
| H.264 | 4:2:0 | 8bit | ✅ | ✅ | 一致 |
| H.264 | 4:2:2 | 8bit | ❌ | ✅ | **VAAPI 领先** |
| H.264 | 4:4:4 | 8bit | ❌ | ✅ | **VAAPI 领先** |
| H.264 | 4:2:0 | 10bit | ❌ | ✅ | **VAAPI 领先** |
| HEVC | 4:2:0 | 8bit | ✅ | ✅ | 一致 |
| HEVC | 4:2:2 | 8bit | ❌ | ✅ | **VAAPI 领先** |
| HEVC | 4:4:4 | 8bit | ✅ | ✅ | 一致 |
| HEVC | 4:2:0 | 10bit | ✅ | ✅ | 一致 |
| HEVC | 4:2:0 | 12bit | ✅ | ✅ | 一致 |
| AV1 | 4:2:0 | 8bit | ✅ | ✅ | 一致 |
| AV1 | 4:2:0 | 10bit | ✅ | ✅ | 一致 |
| VP9 | 4:2:0 | 8bit | ✅ | ✅ | 一致 |
| VP9 | 4:2:0 | 10bit | ✅ | ✅ | 一致 |
| MPEG-2 | 4:2:0 | 8bit | ✅ | ✅ | 一致 |
| MPEG-2 | 4:2:2 | 8bit | ❌ | ✅ | **VAAPI 领先** |

### 4.2 分协议详表

#### H.264（AVC）

| 色度采样 | 位深 | NVDEC | VAAPI |
|---------|------|:-----:|:-----:|
| 4:2:0 | 8bit | ✅ | ✅ |
| 4:2:2 | 8bit | ❌ | ✅ |
| 4:4:4 | 8bit | ❌ | ✅ |
| 4:2:0 | 10bit | ❌ | ✅ |

#### HEVC（H.265）

| 色度采样 | 位深 | NVDEC | VAAPI |
|---------|------|:-----:|:-----:|
| 4:2:0 | 8bit | ✅ | ✅ |
| 4:2:2 | 8bit | ❌ | ✅ |
| 4:4:4 | 8bit | ✅ | ✅ |
| 4:2:0 | 10bit | ✅ | ✅ |
| 4:2:0 | 12bit | ✅ | ✅ |

#### AV1

| 色度采样 | 位深 | NVDEC | VAAPI |
|---------|------|:-----:|:-----:|
| 4:2:0 | 8bit | ✅ | ✅ |
| 4:2:0 | 10bit | ✅ | ✅ |

#### VP9

| 色度采样 | 位深 | NVDEC | VAAPI |
|---------|------|:-----:|:-----:|
| 4:2:0 | 8bit | ✅ | ✅ |
| 4:2:0 | 10bit | ✅ | ✅ |

#### MPEG-2

| 色度采样 | 位深 | NVDEC | VAAPI |
|---------|------|:-----:|:-----:|
| 4:2:0 | 8bit | ✅ | ✅ |
| 4:2:2 | 8bit | ❌ | ✅ |

### 4.3 能力差异汇总

| 指标 | 数量 |
|------|:----:|
| 一致 | 10 项 |
| VAAPI 领先 | 5 项 |
| NVDEC 领先 | 0 项 |

**VAAPI 领先的 5 项全部是专业视频工作流格式**：

| 格式 | 说明 |
|------|------|
| H.264 4:2:2 | 广播级专业视频（采编/新闻）主力格式 |
| H.264 4:4:4 | 无损/近无损后期制作 |
| H.264 10bit | 高色深 H.264 |
| HEVC 4:2:2 | 专业 HEVC |
| MPEG-2 4:2:2 | 传统广播格式 |

> 注：NVDEC 的 4:2:2 / 4:4:4 / H.264 10bit 不支持，是 NVIDIA 消费级显卡（GeForce/RTX 4060）的硬件限制。NVIDIA 将这部分能力保留给专业卡（Quadro / RTX A 系列）。AMD 的 VCN5 解码器未做此阉割，消费级 RX 9070 即支持。

---

## 5. NVDEC SDK 直测性能数据

以下为 NVIDIA RTX 4060 上 NVDEC SDK 直测（parser 模式）的性能数据。测试规格：1920×1080，150 帧，30fps 源。

| 格式 | 色度采样 | 位深 | 帧数 | 平均 fps | 状态 |
|------|---------|------|:----:|:--------:|:----:|
| H.264 | 4:2:0 | 8bit | 150 | 828 | ✅ |
| H.264 | 4:2:2 | 8bit | 0 | — | ❌ 解码器创建失败 |
| H.264 | 4:4:4 | 8bit | 0 | — | ❌ 解码器创建失败 |
| H.264 | 4:2:0 | 10bit | 0 | — | ❌ 解码器创建失败 |
| HEVC | 4:2:0 | 8bit | 150 | 1553 | ✅ |
| HEVC | 4:2:2 | 8bit | 0 | — | ❌ 解码器创建失败 |
| HEVC | 4:4:4 | 8bit | 150 | 983 | ✅ |
| HEVC | 4:2:0 | 10bit | 150 | 1576 | ✅ |
| HEVC | 4:2:0 | 12bit | 150 | 1536 | ✅ |
| AV1 | 4:2:0 | 8bit | 150 | 1445 | ✅ |
| AV1 | 4:2:0 | 10bit | 150 | 1459 | ✅ |
| VP9 | 4:2:0 | 8bit | 1 | 22 | ⚠️ 仅 1 帧 |
| VP9 | 4:2:0 | 10bit | 1 | 22 | ⚠️ 仅 1 帧 |
| MPEG-2 | 4:2:0 | 8bit | 150 | 817 | ✅ |
| MPEG-2 | 4:2:2 | 8bit | 0 | — | ❌ 解码器创建失败 |

> 注：
> - 「解码器创建失败」即 `cuvidCreateDecoder` 返回错误，与 FFmpeg 层的「不支持」结论一致，均为 NVDEC 硬件限制。
> - VP9 仅解出 1 帧，是 SDK 直测程序"一次性喂入"方式对 VP9 帧边界不友好所致（FFmpeg 逐帧喂入则正常），不影响格式支持性结论。

### 5.1 SDK 直测技术要点

NVDEC SDK 直测过程中发现的 3 个关键技术点（均在 `nvdec_benchmark.cpp` 中实现）：

1. **必须先初始化 CUDA context**：否则 `cuvidGetDecoderCaps` / `cuvidCreateDecoder` 返回 `CUDA_ERROR_NOT_SUPPORTED (201)`。
2. **sequence 回调必须返回 surface 数**：返回 `ulNumDecodeSurfaces`（min + 3），而非固定 1，否则 parser 内部索引错乱，导致大量帧解码失败。
3. **OutputFormat 需按 chroma/bit_depth 动态选择**：4:4:4 用 `YUV444`/`YUV444_16Bit`，10/12bit 用 `P016`，否则 4:4:4 会误判为不支持。

---

## 6. 附录

### 6.1 测试脚本清单

| 脚本 | 说明 |
|------|------|
| [test_vaapi_nvdec.sh](../../benchmarks/scripts/test_vaapi_nvdec.sh) | FFmpeg 层格式矩阵测试（NVIDIA/AMD 通用，`--nvidia` / `--amd` 参数） |
| [test_nvdec_sdk.sh](../../benchmarks/scripts/test_nvdec_sdk.sh) | NVDEC SDK 直测一键脚本（格式矩阵 × SDK 解码） |
| [merge_decode_compare.sh](../../benchmarks/scripts/merge_decode_compare.sh) | 合并 NVDEC + VAAPI 结果生成对比报告 |
| [nvdec_benchmark.cpp](../../benchmarks/nvdec/nvdec_benchmark.cpp) | NVDEC SDK 解码测试程序源码（CUVID parser 模式） |
| [build_nvdec_benchmark.sh](../../benchmarks/nvdec/build_nvdec_benchmark.sh) | nvdec_benchmark 编译脚本 |

### 6.2 SDK 名称与参考

| SDK / 库 | 说明 | 来源 |
|----------|------|------|
| NVIDIA Video Decode API（CUVID） | NVDEC 硬件解码接口 | NVIDIA 驱动自带 `libnvcuvid.so` |
| nv-codec-headers | CUVID 头文件（动态加载版） | [FFmpeg/nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) |
| VAAPI（libva） | 跨厂商视频加速标准接口 | `libva-dev`（Linux 发行版） |

### 6.3 原始数据文件

| 文件 | 说明 |
|------|------|
| [vaapi_nvdec_nvidia_results.md](../evidence/results/vaapi_nvdec_nvidia_results.md) | NVIDIA FFmpeg 层测试结果 |
| [vaapi_nvdec_amd_results.md](../evidence/results/vaapi_nvdec_amd_results.md) | AMD FFmpeg 层测试结果 |
| [nvdec_sdk_results.json](../evidence/results/nvdec_sdk_results.json) | NVDEC SDK 直测结果（JSON） |
| [nvdec_vaapi_decode_compare.md](../evidence/results/nvdec_vaapi_decode_compare.md) | 合并后的对比报告 |

### 6.4 复现步骤

1. **NVIDIA 侧**（RTX 4060 机器）：
   ```bash
   ./benchmarks/scripts/test_vaapi_nvdec.sh --nvidia   # FFmpeg 层
   ./benchmarks/scripts/test_nvdec_sdk.sh               # SDK 直测
   ```
2. **AMD 侧**（RX 9070 机器）：
   ```bash
   ./benchmarks/scripts/test_vaapi_nvdec.sh --amd      # FFmpeg 层
   ```
3. **合并**（任一侧，需两份结果文件）：
   ```bash
   ./benchmarks/scripts/merge_decode_compare.sh
   ```
