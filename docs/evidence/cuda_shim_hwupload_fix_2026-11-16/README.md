# cuda_shim hwupload 跨帧损坏修复（2026-11-16）

> 环境：AMD Radeon RX 7900 XTX / ROCm 7.2.4 / FFmpeg 7.1（链接本仓库 `libcuda.so.1`）
> 代码基线：`352919c` + 本轮 `libcuda.cpp` 修复
> 回归测试：`tests/npp/verify_hwupload_roundtrip.sh`

## 现象

FFmpeg `rawvideo` 文件输入 + `hwupload_cuda` + `hwdownload` 时，5 帧 yuv420p
片段出现确定性跨帧损坏：frame 0 正常，frame 1 的 Y 平面被写入色度值
（例如源值 16 变成 145/106）。与 `-threads 1`、`-filter_threads 1`、
`-thread_type none` 均无关；lavfi 输入因时序不同通常不触发。

## 根因

FFmpeg `hwcontext_cuda` 的上传路径调用 `cuMemcpy2DAsync`，源是 pageable host
内存，随后立即回收源帧。CUDA 对 pageable 内存的“异步”拷贝在返回前已完成
staging，调用方可以复用源缓冲；shim 直接转成 `hipMemcpyParam2DAsync`，HIP
可能在调用返回后仍读取 host 缓冲，FFmpeg 此时已把下一帧写入同一地址，导致
上传内容被覆盖。

## 修复

`cuda_shim/src/libcuda.cpp` 的 `cuMemcpy2DAsync_v2`：只要源或目的是
`CU_MEMORYTYPE_HOST`，改用同步 `hipMemcpyParam2D`；设备到设备拷贝仍走
`hipMemcpyParam2DAsync`。这与 CUDA 对 pageable host 内存的语义一致，代价是
上传/下载不再与后续 GPU 工作重叠。

## 回归结果

`verify_hwupload_roundtrip.sh` 用软件 `format=$fmt` 渲染同一源帧作为参考，
要求 CUDA 上传/下载往返逐字节一致，`yuv420p` 与 `nv12` 各重复 3 次：

| shim | 结果 |
|---|---|
| 修复前（仅回退该 hunk） | **FAIL**：yuv420p 3/3 与参考不同；nv12 3/3 通过（竞态与时序相关，通过不代表正确） |
| 修复后 | **PASS**：6/6 往返逐字节一致 |

- `verify-hwupload-fixed.log`：修复后完整输出
- `verify-hwupload-prefix-shim.log`：修复前同一测试的输出
- `environment.log`：FFmpeg 版本与 `libcuda` 链接路径

修复后 `scale_npp` 端到端矩阵（lavfi 口径）仍为 40/40、26,307,120 bytes，
且与 NVIDIA golden 的逐 case SHA256 一致。

## 边界

- 修复只覆盖 2D 拷贝；`cuMemcpyHtoDAsync` / `cuMemcpyDtoHAsync` 对 pageable
  内存仍是异步转发，尚无触发案例，后续按同一语义收紧。
- 修复使上传/下载完全同步，性能影响未测量；若后续需要重叠，应先识别 pinned
  内存再选择异步路径。
- 本目录不包含 shim 二进制或 raw YUV；测试可用脚本重新生成。
