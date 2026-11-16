# 多流并发与动态分辨率验收（2026-11-16）

> 环境：AMD Radeon RX 7900 XTX / ROCm 7.2.4 / FFmpeg 7.1（链接本仓库 `cuda_shim`）
> 后端：VAAPI 与 Vulkan Video 同级候选，分别运行

## 1. 动态分辨率

FFmpeg CLI 无法在固定 `AVHWFramesContext` 下切换分辨率，因此用 CUVID 级宿主
`tests/zerocopy/verify_resolution_change` 模拟播放器：每次 sequence callback
尺寸变化时销毁并重建 CUVID decoder，SPS/PPS 由 shim 的 parser 解析。

码流为 11 帧 320×240 + 11 帧 640×480。宿主把每个显示帧 dump 为紧凑 NV12，
与软件解码逐帧比较。

| 后端 | 结果 |
|---|---|
| VAAPI | **PASS**：22/22 帧一致，日志含 `SEQUENCE CHANGE 320x240 -> 640x480` |
| Vulkan | **PASS**：22/22 帧一致，日志含 `SEQUENCE CHANGE 320x240 -> 640x480` |

这说明 shim 在宿主重建解码器时可以完成序列切换；FFmpeg CLI 的限制仍单独记录，
不是 shim 的序列状态机问题。

## 2. 八路并发

单个 FFmpeg 进程同时打开 8 路输入（320×240 与 640×480 交替），每路一个
decoder、各自线程，逐帧与软件解码比较：

| 后端 | 结果 |
|---|---|
| VAAPI | **PASS**：8 路全部一致 |
| Vulkan | **PASS**：8 路全部一致 |

过程中发现并修复了 `VkSequenceCache` 的进程级单槽问题：原来第二路流的
SPS/PPS 会覆盖第一路，导致 Vulkan 下第一路解码错误。缓存改为线程局部后，
每路 decoder 在自己的线程内取到正确参数集。

## 复现

```bash
make -C tests/zerocopy ROCM_DIR=/opt/rocm verify_resolution_change
FFMPEG_BUILD_DIR=/path/to/ffmpeg tests/zerocopy/run_resolution_change.sh

FFMPEG_BUILD_DIR=/path/to/ffmpeg tests/quick-verify/run_multi_stream.sh
```

## 文件

- `dynamic-resolution-summary.txt`、`dynres-{vaapi,vk}.log`
- `multi-stream-summary.txt`、`multistream-{vaapi,vk}.log`
- `streams.sha256`：两路动态分辨率输入段摘要

raw NV12 / 帧 dump 不入库，脚本可重新生成。

## 边界

- 多流测试是 8 路、秒级；1/8 路 30 分钟长稳和性能基线尚未执行。
- `VkSequenceCache` 是“每线程单槽”，同一线程内交错驱动多个 decoder 的宿主
  仍不支持；FFmpeg 的一路一线程模型满足该约束。
- 动态分辨率只验证到 CUVID 宿主层；FFmpeg CLI 仍会报 hwframes 参数不兼容。
