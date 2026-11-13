# FFmpeg 宿主双后端验收基线

> 由 `tests/quick-verify/run_ffmpeg_compare.sh` 自动生成。
> 生成时间：2026-11-13T14:33:06+08:00
> 代码提交：`a8ddff304c765ff73267885f702d3363265dfc91`
> FFmpeg：`/home/mark/ffmpeg-7.1/ffmpeg`
> FFmpeg 构建树：`/home/mark/ffmpeg-7.1`
> 后端：`vaapi vk`（同级候选，无默认主线）
> 模式：`nvdec-safe nvdec-unsafe cuvid-cpu cuvid-cuda`

## 结果

| 后端 | 模式 | 码流 | 帧数 | 像素 | PTS/顺序 | D2D | fd first→last (max) | RSS max KiB | 结果 | 原因 |
|---|---|---|---:|---|---|---:|---|---:|---|---|
| vaapi | nvdec-safe | test_pts_320x240.mp4 | 30/30 | PASS | PASS | 60 | 3→6 (13) | 268828 | **PASS** | ok |
| vaapi | nvdec-unsafe | test_pts_320x240.mp4 | 30/30 | PASS | PASS | 0 | 3→5 (21) | 269200 | **PASS** | ok |
| vaapi | cuvid-cpu | test_pts_320x240.mp4 | 30/30 | PASS | PASS | 0 | 3→5 (12) | 251336 | **PASS** | ok |
| vaapi | cuvid-cuda | test_pts_320x240.mp4 | 30/30 | PASS | PASS | 90 | 3→5 (12) | 251864 | **PASS** | ok |
| vk | nvdec-safe | test_pts_320x240.mp4 | 30/30 | PASS | PASS | 60 | 3→6 (26) | 285876 | **PASS** | ok |
| vk | nvdec-unsafe | test_pts_320x240.mp4 | 30/30 | PASS | PASS | 0 | 3→5 (36) | 286780 | **PASS** | ok |
| vk | cuvid-cpu | test_pts_320x240.mp4 | 30/30 | PASS | PASS | 0 | 3→6 (13) | 259456 | **PASS** | ok |
| vk | cuvid-cuda | test_pts_320x240.mp4 | 30/30 | PASS | PASS | 90 | 3→7 (12) | 259096 | **PASS** | ok |

## 判据与边界

- PASS 必须同时满足：FFmpeg 退出 0、帧数一致、输出大小一致、逐字节像素一致、D2D 计数可读；输入含 PTS 时还要求 PTS/显示顺序一致。
- 裸 Annex-B P0 码流没有输入 PTS，PTS 一栏记 `NA(no-input-pts)`；输出与软件参考逐帧逐字节一致即为显示顺序证据。
- `nvdec-unsafe` 在 `REQUIRE_NVDEC_ZEROCOPY=1` 时还要求 D2D=0。
- `nvdec-safe` 的 D2D 是 FFmpeg 可写帧/回读路径的真实行为，只记录，不能作为零拷贝证据。
- D2D 只统计 shim 的 `cuMemcpy2D*`/`cuMemcpyDtoD*` 调用，不含驱动内部拷贝或 CPU 回读。
- fd/RSS 是短时采样的首值、末值和峰值，不是长稳泄漏结论。
- 结论只适用于本机、当前提交、当前 FFmpeg 构建和所列码流，不代替目标平台及业务验收。
