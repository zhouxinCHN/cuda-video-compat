# NPP Stage B NVIDIA 11.5 golden（Lanczos / Super）

生成日期：2026-11-17

代码基线：`231b440a627577391e8700adb09f2e6286f8bfc6`（含本轮
`--stage b` 只输出 resize 用例的修正）

> Golden：真实 NVIDIA NPP **11.5.1.107**（RTX 4060），未使用仓库 `npp_shim`。
> 探针只覆盖 NPP 11.5 运行库实际实现的 Stage B 插值：`LANCZOS` 与 `SUPER`。
> `CUBIC2P_*` 在本版本运行库中不可用，详见下文限制。

## 环境

| 项目 | 实际值 |
|---|---|
| GPU | NVIDIA GeForce RTX 4060（Compute Capability 8.9，8188 MiB） |
| NVIDIA 驱动 | 595.99.02（`nvidia-smi` 报告的 CUDA 支持级别 13.2） |
| NPP | 11.5.1.107（Ubuntu 包 `libnpp*11 11.5.1.107~11.5.1-1ubuntu1`） |
| NPP 头文件 | `/usr/include`（`NPP_VER_MAJOR=11`、`MINOR=5`、`PATCH=1`、`BUILD=107`） |
| NPP 运行库 | `/usr/lib/x86_64-linux-gnu/libnpp*.so.11.5.1.107` |
| CUDA Toolkit / nvcc | 11.5 / 11.5.119 |
| 系统 | Linux 6.8.0-138-generic x86_64 |

探针动态解析到系统真实 NPP：

```text
libnppig.so.11  => /usr/lib/x86_64-linux-gnu/libnppig.so.11
libnppicc.so.11 => /usr/lib/x86_64-linux-gnu/libnppicc.so.11
libnppc.so.11   => /usr/lib/x86_64-linux-gnu/libnppc.so.11
libcudart.so.11.0 => /usr/lib/x86_64-linux-gnu/libcudart.so.11.0
```

完整输出见 [environment.log](environment.log) 与
[nppc_core_probe.log](nppc_core_probe.log)。`nppGetLibVersion` 返回
`11.5.1`，与 11.5.1.107 包一致。

## 构建与生成命令

```bash
make -C tests/npp clean all GPU_RUNTIME=cuda \
  NPP_INCLUDE_DIR=/usr/include \
  CUDA_INCLUDE_DIR=/usr/include \
  CUDA_LIB_DIR=/usr/lib/x86_64-linux-gnu \
  NPP_LIB_DIR=/usr/lib/x86_64-linux-gnu

mkdir -p /tmp/npp-golden-b
(cd tests/npp && ./npp_stage_a_probe --output /tmp/npp-golden-b \
  --stage b --skip cubic2p_bspline,cubic2p_catmullrom,cubic2p_b05c03)
```

`--stage b` 只运行 Stage B resize 用例，不再把 Stage A 的 P2P3/P3P2 颜色
转换写入 Stage B 清单。`--skip` 显式排除 NPP 11.5 运行库不支持的
`cubic2p_*`，避免生成半途失败的清单。

## 探针条目

`manifest.tsv` 共 **14 条**（7 个 resize 用例 × src/dst），对应 **14 个**
raw `.gray` 文件：

| 几何 | Lanczos | Super |
|---|---|---|
| `full_320x240_to_160x120`（0.5x 下采样） | ✅ | ✅ |
| `full_320x240_to_480x270`（上采样） | ✅ | 跳过（见下） |
| `roi_288x224_to_144x112`（源 ROI，0.5x 下采样） | ✅ | ✅ |
| `small_64x48_to_37x29_offset`（奇数目标尺寸 + 目标 ROI 偏移） | ✅ | ✅ |

- 7 个用例 = Lanczos 4 + Super 3。
- 同一输出目录连续运行两次，`manifest.tsv` 与全部 raw 文件 SHA256 完全
  一致。
- Stage A golden 未改动：`--stage a` 仍为 32 条，与本目录同机的
  [Stage A golden](../NPP_StageA_NVIDIA_11.5_golden/README.md) 逐字节一致。

### SUPER 几何范围

NPP 的 supersampling 要求 **两个轴都下采样**。探针据此跳过上采样几何：

- 运行：`320x240 → 160x120`、`288x224 → 144x112`、`64x48 → 37x29`
  （实际目标 ROI 为 37×29，带 (4,3) 偏移）；
- 跳过：`320x240 → 480x270`（上采样）。

因此 SUPER 只出现在 3 个下采样用例中，且全部成功，没有出现失败。

## CUBIC2P 限制（重要）

头文件 `nppi_geometry_transforms.h` 和在线文档把
`NPPI_INTER_CUBIC2P_BSPLINE/CATMULLROM/B05C03` 列为
`nppiResizeSqrPixel_8u_C1R` 支持的插值，但 **NPP 11.5.1.107 运行库并未
实现**：

- 探针 `--stage b`（不 skip）在第一个 cubic2p 用例即失败：
  `resize failed: full_320x240_to_160x120 cubic2p_bspline status=-22`
  （`-22 = NPP_INTERPOLATION_ERROR`），见
  [cubic2p-unsupported.log](cubic2p-unsupported.log)；
- 直接调用 `nppiResizeSqrPixel_8u_C1R`、`_Ctx` 和 `nppiResize_8u_C1R`，
  在下采样、等尺寸、上采样三种比例下均返回 `-22`；
- `libnppig.so.11.5.1.107` 中不存在任何 cubic2p 内核符号/字符串。

结论：**cubic2p golden 无法在 NPP 11.5.1.107 上生成**。本目录只归档
Lanczos 与 Super，未把 cubic2p 纳入 11.5 基线。若后续需要 cubic2p 的
NVIDIA 参考，必须换用实现了该插值的更新 NPP 版本，并单独建立 golden。

## FFmpeg scale_npp 端到端

FFmpeg 6.1，`--enable-nonfree --enable-libnpp ... --enable-gpl
--enable-libx264`，动态解析到系统真实 NPP（非 shim）。输入为 `testsrc`
经 `format=yuv420p` 的 5 帧，每 case 连续运行 2 次且 SHA256 一致。

```bash
GEOMETRIES="160x120 214x118 640x480 320x240 100x100" \
ALGOS="lanczos" NPP_RUNTIME=cuda OUT_DIR=/tmp/scale-npp-b \
  tests/npp/run_ffmpeg_scale_npp.sh

GEOMETRIES="160x120 214x118 100x100" \
ALGOS="super" NPP_RUNTIME=cuda OUT_DIR=/tmp/scale-npp-super \
  tests/npp/run_ffmpeg_scale_npp.sh
```

| 矩阵 | 结果 | 说明 |
|---|---|---|
| [ffmpeg-lanczos](ffmpeg-lanczos/) | **10/10 PASS**，6,576,780 bytes | 5 几何 × nv12/yuv420p |
| [ffmpeg-super](ffmpeg-super/) | **6/6 PASS**，816,780 bytes | 仅 3 个下采样几何 |
| [ffmpeg-cubic2p-unsupported](ffmpeg-cubic2p-unsupported/) | 16 PASS / 24 FAIL | 失败均为 `NPP resize error: -22`，`exit 171` |

cubic2p 失败矩阵中，唯 6 个 `320x240` 等尺寸用例“通过”，因为 FFmpeg 对
同尺寸不触发实际缩放；其余 24 个非等尺寸用例全部失败。该目录仅作为
NPP 11.5 限制证据归档，不是可用 golden。

两个成功矩阵的输入参考帧摘要一致：

```text
19ec213dafde22863f5abdb53e4b91f668264c244bcf6e5cdcebd83aa4b6caf6
```

该值与已归档的 Stage A FFmpeg golden 相同，说明输入帧一致。

## 文件

探针 golden：

- `manifest.tsv`：14 条 resize 清单；
- `resize__*.gray`：14 个 raw 输出（src/dst）；
- `SHA256SUMS`：`manifest.tsv` 与全部 raw 文件的摘要；
- `environment.log`：GPU/驱动/NPP/CUDA、构建与生成命令、动态链接；
- `nppc_core_probe.log`：`libnppc` 版本与设备属性探测；
- `cubic2p-unsupported.log`：NPP 11.5 拒绝 cubic2p 的探针输出。

FFmpeg 矩阵（raw `.yuv` / `source-reference.yuv` 不入库）：

- `ffmpeg-lanczos/`、`ffmpeg-super/`：`manifest.tsv`、`source.sha256`、
  `SHA256SUMS`、`environment.log`、`logs/`；
- `ffmpeg-cubic2p-unsupported/`：同上，附 40 个 case 的日志，作为限制证据。

## 合规边界

- 仓库不包含 NVIDIA 头文件、库或 FFmpeg 构建产物。
- FFmpeg 的 `libnpp` 构建是 **nonfree / unredistributable**，仅本机生成
  golden 使用，不随驱动分发。
- 本轮未修改 AMD 侧 shim 代码。
