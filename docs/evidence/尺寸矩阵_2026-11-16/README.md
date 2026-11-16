# 1080p / 4K 解码矩阵（2026-11-16）

> 环境：AMD RX 7900 XTX / ROCm 7.2.4 / FFmpeg 7.1（链接本仓库 `cuda_shim`）
> 入口：`tests/quick-verify/run_size_matrix.sh`

## 结果

**8/8 通过**：1080p（30 帧，I/P/B）与 4K（5 帧，I/P/B）× `cuvid`/`nvdec` ×
VAAPI/VK，逐帧 framemd5 与软件解码完全一致。

| 配置 | 帧数 | 耗时 |
|---|---|---|
| 1080p cuvid VAAPI | 30 | 484 ms |
| 1080p cuvid VK | 30 | 491 ms |
| 1080p nvdec VAAPI | 30 | 608 ms |
| 1080p nvdec VK | 30 | 658 ms |
| 4K cuvid VAAPI | 5 | 2476 ms |
| 4K cuvid VK | 5 | 544 ms |
| 4K nvdec VAAPI | 5 | 585 ms |
| 4K nvdec VK | 5 | 1485 ms |

耗时是整条 FFmpeg 进程（含启动、软件参考无关的解码和 framemd5 输出）的墙钟
时间，只用于说明量级，**不是**性能基线。4K cuvid VAAPI 的首次运行包含
一次性初始化开销。

码流在脚本运行时用 libx264 生成（`testsrc2`），不进入仓库。

## 边界

- 只验证 8-bit 4:2:0 H.264 的正确性；HEVC/VP9/AV1、10-bit 不在 P0。
- 4K 只有 5 帧，用于尺寸/缓冲边界，不代表 4K 长稳。
- 长稳与吞吐基线见 `run_stability.sh`（30 分钟 1/8 路）和后续性能报告。

## 文件

- `summary.txt`：8 个组合的帧数与耗时
- `*.log`：每个组合的 FFmpeg 日志
