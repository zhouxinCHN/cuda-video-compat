# FFmpeg 宿主双后端验收基线

> 由 `tests/quick-verify/run_ffmpeg_compare.sh` 自动生成。
> 生成时间：2026-11-13T14:32:44+08:00
> 代码提交：`a8ddff304c765ff73267885f702d3363265dfc91`
> FFmpeg：`/home/mark/ffmpeg-7.1/ffmpeg`
> FFmpeg 构建树：`/home/mark/ffmpeg-7.1`
> 后端：`vaapi vk`（同级候选，无默认主线）
> 模式：`nvdec-safe nvdec-unsafe cuvid-cpu cuvid-cuda`

## 结果

| 后端 | 模式 | 码流 | 帧数 | 像素 | PTS/顺序 | D2D | fd first→last (max) | RSS max KiB | 结果 | 原因 |
|---|---|---|---:|---|---|---:|---|---:|---|---|
| vaapi | nvdec-safe | test_p_b_5.h264 | 9/9 | PASS | NA(no-input-pts) | 18 | 3→5 (13) | 266804 | **PASS** | ok |
| vaapi | nvdec-unsafe | test_p_b_5.h264 | 9/9 | PASS | NA(no-input-pts) | 0 | 3→6 (20) | 246108 | **PASS** | ok |
| vaapi | cuvid-cpu | test_p_b_5.h264 | 9/9 | PASS | NA(no-input-pts) | 0 | 3→10 (12) | 250196 | **PASS** | ok |
| vaapi | cuvid-cuda | test_p_b_5.h264 | 9/9 | PASS | NA(no-input-pts) | 27 | 3→11 (13) | 251104 | **PASS** | ok |
| vk | nvdec-safe | test_p_b_5.h264 | 9/9 | PASS | NA(no-input-pts) | 18 | 3→6 (19) | 278628 | **PASS** | ok |
| vk | nvdec-unsafe | test_p_b_5.h264 | 9/9 | PASS | NA(no-input-pts) | 0 | 3→5 (19) | 278912 | **PASS** | ok |
| vk | cuvid-cpu | test_p_b_5.h264 | 9/9 | PASS | NA(no-input-pts) | 0 | 3→5 (13) | 257792 | **PASS** | ok |
| vk | cuvid-cuda | test_p_b_5.h264 | 9/9 | PASS | NA(no-input-pts) | 27 | 3→5 (13) | 258576 | **PASS** | ok |
| vaapi | nvdec-safe | test_p_b_7.h264 | 13/13 | PASS | NA(no-input-pts) | 26 | 3→6 (12) | 261500 | **PASS** | ok |
| vaapi | nvdec-unsafe | test_p_b_7.h264 | 13/13 | PASS | NA(no-input-pts) | 0 | 3→5 (21) | 267036 | **PASS** | ok |
| vaapi | cuvid-cpu | test_p_b_7.h264 | 13/13 | PASS | NA(no-input-pts) | 0 | 3→10 (13) | 252296 | **PASS** | ok |
| vaapi | cuvid-cuda | test_p_b_7.h264 | 13/13 | PASS | NA(no-input-pts) | 39 | 3→5 (12) | 247888 | **PASS** | ok |
| vk | nvdec-safe | test_p_b_7.h264 | 13/13 | PASS | NA(no-input-pts) | 26 | 3→5 (23) | 282184 | **PASS** | ok |
| vk | nvdec-unsafe | test_p_b_7.h264 | 13/13 | PASS | NA(no-input-pts) | 0 | 3→0 (23) | 281408 | **PASS** | ok |
| vk | cuvid-cpu | test_p_b_7.h264 | 13/13 | PASS | NA(no-input-pts) | 0 | 3→6 (12) | 257784 | **PASS** | ok |
| vk | cuvid-cuda | test_p_b_7.h264 | 13/13 | PASS | NA(no-input-pts) | 39 | 3→5 (13) | 258596 | **PASS** | ok |
| vaapi | nvdec-safe | test_p_main.h264 | 30/30 | PASS | NA(no-input-pts) | 60 | 3→6 (13) | 268376 | **PASS** | ok |
| vaapi | nvdec-unsafe | test_p_main.h264 | 30/30 | PASS | NA(no-input-pts) | 0 | 3→5 (20) | 267840 | **PASS** | ok |
| vaapi | cuvid-cpu | test_p_main.h264 | 30/30 | PASS | NA(no-input-pts) | 0 | 3→5 (12) | 250944 | **PASS** | ok |
| vaapi | cuvid-cuda | test_p_main.h264 | 30/30 | PASS | NA(no-input-pts) | 90 | 3→5 (12) | 251248 | **PASS** | ok |
| vk | nvdec-safe | test_p_main.h264 | 30/30 | PASS | NA(no-input-pts) | 60 | 3→9 (25) | 284804 | **PASS** | ok |
| vk | nvdec-unsafe | test_p_main.h264 | 30/30 | PASS | NA(no-input-pts) | 0 | 3→5 (34) | 285408 | **PASS** | ok |
| vk | cuvid-cpu | test_p_main.h264 | 30/30 | PASS | NA(no-input-pts) | 0 | 3→6 (12) | 258668 | **PASS** | ok |
| vk | cuvid-cuda | test_p_main.h264 | 30/30 | PASS | NA(no-input-pts) | 90 | 3→7 (12) | 258828 | **PASS** | ok |
| vaapi | nvdec-safe | test_p_66.h264 | 30/30 | PASS | NA(no-input-pts) | 60 | 3→6 (12) | 268448 | **PASS** | ok |
| vaapi | nvdec-unsafe | test_p_66.h264 | 30/30 | PASS | NA(no-input-pts) | 0 | 3→0 (21) | 268688 | **PASS** | ok |
| vaapi | cuvid-cpu | test_p_66.h264 | 30/30 | PASS | NA(no-input-pts) | 0 | 3→6 (12) | 250644 | **PASS** | ok |
| vaapi | cuvid-cuda | test_p_66.h264 | 30/30 | PASS | NA(no-input-pts) | 90 | 3→5 (12) | 252624 | **PASS** | ok |
| vk | nvdec-safe | test_p_66.h264 | 30/30 | PASS | NA(no-input-pts) | 60 | 3→0 (25) | 286008 | **PASS** | ok |
| vk | nvdec-unsafe | test_p_66.h264 | 30/30 | PASS | NA(no-input-pts) | 0 | 3→5 (34) | 285460 | **PASS** | ok |
| vk | cuvid-cpu | test_p_66.h264 | 30/30 | PASS | NA(no-input-pts) | 0 | 3→6 (12) | 259212 | **PASS** | ok |
| vk | cuvid-cuda | test_p_66.h264 | 30/30 | PASS | NA(no-input-pts) | 90 | 3→5 (12) | 259388 | **PASS** | ok |

## 判据与边界

- PASS 必须同时满足：FFmpeg 退出 0、帧数一致、输出大小一致、逐字节像素一致、D2D 计数可读；输入含 PTS 时还要求 PTS/显示顺序一致。
- 裸 Annex-B P0 码流没有输入 PTS，PTS 一栏记 `NA(no-input-pts)`；输出与软件参考逐帧逐字节一致即为显示顺序证据。
- `nvdec-unsafe` 在 `REQUIRE_NVDEC_ZEROCOPY=1` 时还要求 D2D=0。
- `nvdec-safe` 的 D2D 是 FFmpeg 可写帧/回读路径的真实行为，只记录，不能作为零拷贝证据。
- D2D 只统计 shim 的 `cuMemcpy2D*`/`cuMemcpyDtoD*` 调用，不含驱动内部拷贝或 CPU 回读。
- fd/RSS 是短时采样的首值、末值和峰值，不是长稳泄漏结论。
- 结论只适用于本机、当前提交、当前 FFmpeg 构建和所列码流，不代替目标平台及业务验收。
