# H.264 复杂解码场景验收（2026-11-16）

> 环境：AMD Radeon RX 7900 XTX / ROCm 7.2.4 / FFmpeg 7.1（链接本仓库 `cuda_shim`）
> 后端：VAAPI 与 Vulkan Video 同级候选，全部组合分别运行
> 入口：`tests/quick-verify/run_decode_scenarios.sh`

## 结果

**12/12 通过**：3 个场景 × 2 条宿主路径 × 2 个后端，逐帧 framemd5 与
FFmpeg 软件解码完全一致（yuv420p，`-fps_mode passthrough`）。

| 场景 | 码流 | 覆盖点 | 帧数 |
|---|---|---|---|
| multi-slice | `test_slices_320x240.h264`（High，4 slices/picture） | 一个 picture 多个 slice，CUVID `nNumSlices`/`pSliceDataOffsets` | 12 |
| crop | `test_crop_1080p.h264`（1920×1080，coded 1088） | SPS 裁剪、目标高度与 coded 高度不一致时的 NV12 chroma 偏移 | 6 |
| seek/flush | `test_pts_320x240.mp4`（含 B 帧），`-ss 0.5` | 输入 seek 后的 flush/重建和参考帧恢复 | 5 |

宿主路径：

- `cuvid`：`-c:v h264_cuvid`，走 shim 自己的 CUVID parser；
- `nvdec`：`-hwaccel nvdec -hwaccel_output_format cuda`，FFmpeg 自行组装
  Annex-B、直接填 `CUVIDPICPARAMS`。

两后端均通过，没有把任一后端当作默认主线。

## 本轮修复

1. **多 slice**：parser 原来把每个 slice NAL 当作一帧提交（12 帧流只出 2 帧）。
   现在按 `first_mb_in_slice == 0` 组装 picture，slice 带 start code 拼接，
   一次提交 `nNumSlices` 和 `pSliceDataOffsets`。VAAPI 与 Vulkan 均支持。
2. **裁剪**：coded 1088 / display 1080 时 chroma 实际在 `pitch*1088`，而 CUVID
   客户端按 `pitch*1080` 读取，1080p 的色度整体错位。`cuvidMapVideoFrame64`
   现在在目标高度与 coded 高度不同时重排为 target-height NV12 视图。
3. **Vulkan 多 slice**：命令缓冲只提交第一个 slice（`srcBufferRange` 也只覆盖
   一个 NAL）。现在上传整个 access unit，`sliceCount`/`pSliceOffsets` 来自
   `CUVIDPICPARAMS`，并按完整范围提交。
4. **Vulkan 参数集来源**：`h264_cuvid` 的 slice 现在自带 start code，原逻辑
   会误判为 `h264_nvdec` 而跳过进程级 SPS/PPS cache。改为优先使用 parser
   缓存的参数集。
5. **映射生命周期**：客户端对映射帧发起异步 D2D 拷贝后立即 unmap，VA 面可能
   随即被重新解码，导致 seek 后个别 B 帧被撕裂。unmap 时同步设备，确保拷贝
   完成后再释放映射（P0 基线本来就是主机同步）。
6. **序列变化检测**：新 SPS 尺寸/裁剪与当前不同时，重新触发 sequence
   callback 并清空 DPB 状态。FFmpeg CLI 的 `h264_cuvid` 路径仍会因
   `AVHWFramesContext` 固定尺寸而报错（见限制），该状态机为后续宿主重建
   解码器准备。

## 已知限制

- **动态分辨率**：shim 已能在 SPS 变化时发出新的 sequence callback；但
  FFmpeg CLI 在 `h264_cuvid` 下不允许中途更换已初始化的 `AVHWFramesContext`，
  报 `AVHWFramesContext is already initialized with incompatible parameters`。
  `-hwaccel nvdec` 路径干净报错退出，不再挂死。需要宿主自行重建帧池/解码器
  （真实业务播放器通常如此），该宿主级用例尚未建立。
- **长期参考帧（MMCO 5）**：parser 的 DPB 目前只填 short-term 条目，未解析
  MMCO；本轮也没有可生成 LTR 的编码器（x264 不支持）。该场景未覆盖，不能
  宣称可用。
- crop 场景只在 VAAPI/VK 的目标高度重排路径验证；重排会引入一次 D2D 拷贝，
  非裁剪流（coded == target）不受影响，仍为零拷贝映射。

## 文件

- `summary.txt`：12 个组合的 PASS 记录
- `environment.log`：FFmpeg 版本与 shim 链接
- `streams.sha256`：三路输入码流摘要
- `logs/`：每个组合的参考/候选解码日志
- raw YUV 不归档，脚本可重新生成
