# FFmpeg scale_npp 端到端矩阵 — AMD candidate（2026-11-16）

> Candidate：AMD HIP shim（RX 7900 XTX / ROCm 7.2.4）
> 构建头文件：CUDA/NPP 12.0.1.104；运行时 ABI：`libnpp*.so.11`
> FFmpeg：7.1，`--enable-nonfree --enable-libnpp`，链接本仓库 `npp_shim`
> 对照目标：同脚本在 NVIDIA 主机（真实 NPP 11.5.1.107）产出的 golden 目录

## 结果

**AMD 侧 40/40 case 执行成功，且每个 case 重复 2 次输出逐字节一致（确定性通过）。**

矩阵为 5 组目标尺寸 × 4 种插值模式 × 2 种表面格式：

- 目标尺寸：160x120、214x118、640x480、320x240（1:1）、100x100
- 插值：`nn`、`linear`、`cubic`、`default`（省略 `interp_algo`）
- 格式：`nv12`（走 P2P3 → resize → P3P2）、`yuv420p`（逐平面 resize，P3）

合计 26,307,120 bytes 原始输出。`manifest.tsv` 记录每个 case 的几何、
格式、插值、帧数、字节数和 SHA256；`SHA256SUMS` 是全部 raw 输出的摘要。

**NVIDIA golden 尚未生成**，因此本目录只证明 AMD 侧可重复执行与自洽性，
不构成与 NVIDIA 的逐字节结论。NVIDIA 侧运行同一脚本后，用
`tests/npp/compare_ffmpeg_scale_npp.py` 比较两个目录即可给出端到端结论。

## 输入口径

- 输入为 FFmpeg `testsrc` 经 `format=yuv420p` 后的帧，不经过解码器，
  用于把 NPP 数值与解码/封装差异隔离。
- `source-reference.yuv` 是同一帧序列的纯软件渲染结果，其 SHA256 记录在
  `source.sha256`。NVIDIA 主机必须得到相同摘要，否则说明两端输入帧不同，
  不能据此判断 NPP。
- 参考输入摘要：`19ec213dafde22863f5abdb53e4b91f668264c244bcf6e5cdcebd83aa4b6caf6`
- 每 case 使用 `-threads 1`，并要求 `REPEAT=2` 两次运行摘要一致。

## 已知限制

rawvideo 文件输入 + `hwupload_cuda` 在本机仍会出现跨帧数据损坏
（只有 lavfi 输入稳定），本轮按 lavfi 口径验证。该问题属于独立的
cuda_shim 上传/拷贝同步缺陷，未在本轮修改；端到端脚本因此不使用 rawvideo
输入，详见 `tests/npp/README.md`。

## 文件

- `manifest.tsv`：40 个 case 的元数据与 SHA256
- `source.sha256`：输入帧摘要
- `SHA256SUMS`：全部 raw 输出摘要（raw 本体不入库）
- `environment.log`：FFmpeg 版本、NPP/ROCm 链接路径和 LD_LIBRARY_PATH
- `logs/`：每个 case 的 FFmpeg 日志与 `scale_npp` 帮助输出
- `nppc_core_probe.log`：`libnppc` 核心 API 探针结果（同一 shim 构建）

raw 输出不归档，可用 `tests/npp/run_ffmpeg_scale_npp.sh` 重新生成。
