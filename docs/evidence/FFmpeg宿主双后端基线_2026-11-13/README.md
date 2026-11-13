# FFmpeg 宿主双后端验收基线（2026-11-13）

> 验证机器：AMD 验证机，Radeon RX 7900 XTX / ROCm 7.2.4 / VAAPI 1.20 / RADV。
> 代码基线：提交 `a8ddff3` + 本目录生成时的修复（工作区状态、脚本和 shim SHA256 见 `environment.log`）。
> FFmpeg：源码树 `~/ffmpeg-7.1` 构建产物；`ffmpeg` / `ffprobe` 的 SHA256 见 `environment.log`。
> 后端定位：VAAPI 与 Vulkan Video 是同级候选后端；领导尚未拍板，本次矩阵对两者使用相同判据。

## 目的

建立可信、可复现、不会误报 PASS 的 FFmpeg 宿主演收入口：

- `h264_nvdec` 按 hwaccel 调用，不误当 decoder；
- 同时覆盖 NVDEC 默认安全路径、NVDEC 零 D2D 路径、CUVID CPU 输出、CUVID CUDA 输出；
- 固定 FFmpeg 构建树，避免二进制实际加载系统 `/usr/local/lib`；
- 每个组合检查退出码、帧数、大小、逐字节像素、D2D 和资源采样；
- 缺少必需能力、全部跳过、拷贝计数缺失均为 FAIL。

## 结果

**32 RUN / 32 PASS / 0 FAIL / 0 SKIP。**

| 后端 | `nvdec-safe` | `nvdec-unsafe` | `cuvid-cpu` | `cuvid-cuda` |
|---|---|---|---|---|
| VAAPI | 4/4 PASS；D2D = 2/帧 | 4/4 PASS；D2D=0 | 4/4 PASS；D2D=0 | 4/4 PASS；D2D=3/帧 |
| Vulkan | 4/4 PASS；D2D = 2/帧 | 4/4 PASS；D2D=0 | 4/4 PASS；D2D=0 | 4/4 PASS；D2D=3/帧 |

四路码流：`test_p_b_5`（9 帧）、`test_p_b_7`（13 帧）、`test_p_main`（30 帧）、`test_p_66`（30 帧），合计 82 帧/后端组合。

## 两个失败项的根因与修复

### 1. VAAPI `nvdec-unsafe` 长无 B 流缺帧

现象：`test_p_main` / `test_p_66` 只输出 28/30 帧，从第 22 帧后数据不一致，FFmpeg 仍退出 0。

根因：`unsafe_output` 下映射帧被下游和 H.264 线程持有，FFmpeg 的 NVDEC frame pool 耗尽，trace 报：

```text
[h264] No decoder surfaces left
[h264] Decoding error: Invalid data found when processing input
```

默认 surface 数为 22；`-extra_hw_frames` 曾因 shim 硬限制 32 直接返回 `NOT_SUPPORTED`。

修复：

- VAAPI/Vulkan 后端的 surface 上限从 32 放宽到 64；
- 验收脚本的 `nvdec-unsafe` 使用 `-extra_hw_frames 16`（共 38 surfaces），下游持有帧时不再耗尽；
- 运行时仍由 `vaCreateSurfaces`/Vulkan 图像创建验证驱动是否支持。

这不是 VAAPI 解码参数错误，也不是 D2D 计数误差；`nvdec-safe` 因为立即复制、提前释放映射帧而掩盖了池耗尽。

### 2. Vulkan `h264_nvdec` map 返回 `CUDA_ERROR_INVALID_VALUE`

根因：`h264_nvdec` 不调用 `cuvidParseVideoData`，shim parser 从未看到 SPS/PPS；`VkSequenceCache` 为空。Vulkan 后端旧逻辑在无参数集时返回成功并跳过解码，未创建 session/export，随后 `cuvidMapVideoFrame64` 只能返回 `INVALID_VALUE`。

修复：

- 新增 `h264_direct_params.cpp`，从 `CUVIDH264PICPARAMS` 合成最小 Annex-B SPS/PPS，按 decoder 实例保存；
- Vulkan 后端优先使用 CuvidParser 的原始 SPS/PPS，缺失时使用合成参数集；
- 无 session/参数集时返回 `CUDA_ERROR_NOT_SUPPORTED`，不再伪成功；
- 识别 `h264_nvdec` 已带 Annex-B start code 的 slice，避免重复补起始码。

## 必须保留的边界

1. **默认 NVDEC 路径不是零拷贝**：`nvdec-safe` 的 D2D=2/帧来自 FFmpeg 安全可写帧/回读路径，只记录。
2. **零拷贝仍依赖宿主 surface 配置**：`nvdec-unsafe` 使用 `-extra_hw_frames 16`；真实客户应用需确认线程/队列和显存预算。默认不配置时会复现 frame pool 耗尽。
3. **CUVID D2D 是 FFmpeg 行为**：`cuvid-cuda` 的 3/帧来自 `cuviddec.c` CUDA frame 拷贝加 `hwdownload`。
4. **Vulkan 参数集是合成的**：不支持多 slice、VUI、crop、POC type 1、非默认缩放矩阵；当前 SPS/PPS ID 固定 0。这些路径必须显式失败或后续实现。
5. **PTS**：P0 裸 Annex-B 无输入 PTS，报告为 `NA(no-input-pts)`；带 PTS 的容器见 [PTS 顺序验证](../FFmpeg_PTS顺序_2026-11-13/README.md)。
6. **资源数据不是长稳结论**：fd/RSS 是短时 `/proc` 采样，只用于基线。
7. **未覆盖**：多 slice、跨 packet、crop、seek/flush、动态分辨率、多流、1080p/4K、目标平台、业务性能。

## 文件说明

- `summary.md`：自动生成的人类可读矩阵。
- `summary.tsv`：逐组合机器可读结果。
- `commands.log`：完整 FFmpeg 调用命令。
- `environment.log`：系统、Git 状态、FFmpeg 配置与 ldd、驱动/工具版本、输入和产物 SHA256。
- `reference__*.log/.pts`：软件参考日志与 PTS。
- `<backend>__<mode>__<stream>.log/.pts`：每组合完整 stderr 和抽取的 PTS。

目录不含 NV12/YUV 或 FFmpeg 源码/二进制。复现入口：

```bash
FFMPEG=$HOME/ffmpeg-7.1/ffmpeg \
  EVIDENCE_DIR=docs/evidence/ffmpeg-host-$(date +%F) \
  ./tests/quick-verify/run_ffmpeg_compare.sh
```

## 后续

1. 在真实业务线程/队列配置下复测 `unsafe_output`，确定额外 surface 的预算和默认值。
2. 扩展 Vulkan 合成参数集：多 slice、crop/VUI、POC type 1、非默认缩放矩阵。
3. 增加 1080p/4K、多流、seek/flush、30 分钟稳定性。
4. 在自研目标平台复跑同一矩阵。
