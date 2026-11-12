# Vulkan 后端 B 帧挂死：调试过程留档（2026-11-12 结案）

> 本文是**过程留档**：现场、排除掉的假设、踩过的测量坑、可复现的验证命令。
> 结论与最终修复清单已并入 [Vulkan 后端说明](../architecture/vulkan-backend.md) §8，
> 两边不重复维护。
>
> 环境：RX 7900 XTX（gfx1100 / Navi 31，VCN < 5）/ ROCm 7.2.4 / Mesa 25.x RADV。

---

## 1. 现场：修复前到底发生了什么

触发条件：`cuda_shim` Vulkan Video 后端（`CUVID_SHIM_BACKEND=vk`）解码到第 4 个画面
（码流里第一个非参考 B 帧）时 VCN ring timeout / GPU mode1 reset。

- 解码序前 3 帧（IDR + P + B[参考帧]）出图。
- 第 4 个解码帧（`slice_type=B`、`nal_ref_idc=0`、`frame_num=3`、`poc=2`、L0=1/L1=2、
  参考槽数首次到 3）提交后：
  - `vkWaitForFences` 30s 超时 → `VK_ERROR_DEVICE_LOST`；
  - 内核 `ring vcn_unified_1 timeout, signaled seq=N, emitted seq=N+1`，
    随后 `GPU mode1 reset`。
- **关键判据**：这一轮的 dmesg **没有** `VM_L2_PROTECTION_FAULT` / page fault。
  也就是说固件不是"访问了非法地址"，而是**自己卡死在一次 decode 上**。
  （更早一轮不同形态的崩溃里出现过 `GCVM_L2_PROTECTION_FAULT_STATUS` / `0x00007fff006ec000`，
  那属于另一种问题，不能拿来解释这一轮，已作废。）
- 同一驱动、同一码流，FFmpeg 7.1 原生 `h264_vulkan`（`dedicated_dpb` +
  `separate_references`）30 帧全对 → **驱动没问题，是 shim 侧 bug**。这条前提贯穿全程。

根因分析见 [vulkan-backend.md §8.1](../architecture/vulkan-backend.md#81-根因视频会话内存-use-after-free)：
绑定到 `VkVideoSessionKHR` 的 `VkDeviceMemory` 被提前 `vkFreeMemory`。

---

## 2. 重要教训：两个"输出错误"其实是测量方法错了

这一轮有一半时间花在追杀两个不存在的 bug，记录在案以免重犯。

1. **"色度是垃圾，PSNR u/v ≈ 6 dB"**（Vulkan 后端）
   实际原因是把导出缓冲**当成 planar yuv420p** 去和 planar 参考比。
   正确做法是：
   - 缓冲区是 **pitch 跨行**的 NV12：亮度 `y*pitch`，色度从 `pitch*H + y*pitch` 开始；
   - 色度是 **U/V 交织**（NV12），参考的 yuv420p 要先交织再比，或分别比
     `dump[0::2]==U`、`dump[1::2]==V`。
   按此重测：**Y/U/V 逐字节完全相同**。
2. **"VAAPI 后端输出平坦 0x80"**
   同样是逐平面比较方式的问题；按上面方法重测，VAAPI 后端四路码流同样
   **逐帧位精确**，不需要"另案处理"。

**另外必须记住**：`cuda_shim/tests/verify_kernel_read.cpp` 的 `OK` / `Ysum` / `UVsum` 只验证
"GPU kernel 读到的与 CPU 读到的是同一块内存"，**不校验画面内容**；
`frames_ok>=10` 也只是流程完整性。要判断画面正确性必须用
`CUVID_DUMP_NV12_DIR=<dir>` 把每帧 NV12 dump 出来，再和
`ffmpeg -i <stream> -f rawvideo -pix_fmt yuv420p` 的软解结果逐帧比。

---

## 3. 已排除的假设（负结果，避免以后重复尝试）

- **H.264 语义层**：`parse_slice_header()`（slice_type / POC / ref_count / MMCO）、
  POC 类型、`max_num_ref_frames` 全部与 `trace_headers` 真值一致（见附录 A）。
  注意 PPS 真值是 `l0=2 / l1=0`，而每个 slice 都用
  `num_ref_idx_active_override_flag=1` 覆盖，所以日志里 `rc1=1` 不是 bug。
- **"shim 解码写进输出图、而 setup 槽指向 DPB 图，导致 DPB 图从未被写"**：
  不成立。FFmpeg 同样把 `dstPictureResource.imageViewBinding` 指向
  `img_view_out`（`vulkan_h264.c`）；RADV TIER_2 是从 **setup 槽的图**取
  `dpbCurrLo/Hi`，两者行为一致。
- **`used_for_reference_flags` 差异**：shim 置 `top/bottom_field_flag=1`、
  FFmpeg 帧模式置 0，RADV 两种写法都得到 `3 << (2i)`，无差异。
- **`RADV_DEBUG=videoarraypath`**（关掉 TIER_2、走静态 DPB 老路径）：
  仍挂 → 问题不在 TIER_2 的 slot 语义。
- **`CUVID_SHIM_EXP=dpbsrc`**（改从 DPB 图导出色度）：更糟，且 luma 也坏了；
  作为诊断手段无效（该开关已删除）。
- **`isref` / `pocbias` / `nosetup` / `sortrefs` 等参数对照实验**：
  全部无正向效果（已删除，默认路径即正确路径）。
- **"shim 走了 RADV 的 SDMA 拷贝路径"**：实测 `transfer_qf_ = qf0`
  （`GRAPHICS|COMPUTE|TRANSFER`），走的是 `compute_copy_image_to_memory`，
  与 FFmpeg 相同；SDMA 路径（仅 transfer-only 队列族）没被用到。
  但 radv `radv_sdma_get_buf_surf()` 里 `pitch = bufferRowLength * texel_scale`
  对多平面 plane 1 可能少算一倍，属于**潜在**上游 bug，与本次问题无关。

---

## 4. 收尾记录

已完成：

- 删除实验开关 `CUVID_SHIM_EXP`（isref/pocbias/nosetup/sortrefs/dpbsrc）及其全部分支
  （默认路径本身就是正确路径），删除队列族临时打印。保留三样仍然有用的东西：
  `CUVID_SHIM_DEBUG_VK=1` 每帧日志、`CUVID_SHIM_NOCOPY=1`（只解码不导出，
  用来分离"解码参数问题"与"多队列同步/导出问题"）、
  `CUVID_DUMP_NV12_DIR=<dir>`（逐帧 dump 做正确性验证）。
- 回退 FFmpeg 侧全部临时补丁并重新编译：`libavcodec/vulkan_h264.c` 的 `H264VK_TRACE` 块、
  `libavcodec/h264_refs.c` 的 `FFVA MOD` 块、`libavcodec/vaapi_h264.c` 的
  `dbg_dump_pic` / `/tmp/ffva.log` 块（从 `/tmp/vaapi_h264.c.bak` 恢复）。
  外面那份 FFmpeg 7.1 构建树已增量重编，树内源码与二进制一致；系统里用于产参考的
  `/usr/local/bin/ffmpeg` 本来就是干净的。

待办（非阻塞，随手可收拾）：

- `fill_sps_std` / `fill_pps_std` 在 flag=false 时把 `pOffsetForRefFrame` /
  `pScalingLists` / `pSequenceParameterSetVui` 置 NULL——与 FFmpeg 语义等价，
  但显式置 NULL 更安全（`vk_video_deep_copy_h264_*` 按 flag 门控，目前不触发）。
- 若以后要在 transfer-only 队列族上做导出拷贝，需先绕开 `radv_sdma_get_buf_surf`
  的多平面 pitch 问题（或改用 compute 队列族，即当前做法）。

---

## 5. 验证命令（可复现）

四个码流都已在仓库里：`testdata/h264/test_p_b_5.h264`（9 帧）、`test_p_b_7.h264`（13 帧）、
`test_p_main.h264`（30 帧）、`test_p_66.h264`（30 帧）。

```bash
# 1) 解码（两个后端各一次），同时 dump 每帧 NV12
cd cuda_shim
LD_LIBRARY_PATH=. CUVID_SHIM_BACKEND=vk RADV_PERFTEST=video_decode \
  CUVID_DUMP_NV12_DIR=/tmp/vkd ./verify_kernel_read ../testdata/h264/test_p_b_5.h264

LD_LIBRARY_PATH=. CUVID_SHIM_BACKEND=vaapi \
  CUVID_DUMP_NV12_DIR=/tmp/vad ./verify_kernel_read ../testdata/h264/test_p_b_5.h264

# 2) 软解参考（显示序，planar yuv420p）
ffmpeg -v error -y -i ../testdata/h264/test_p_b_5.h264 -f rawvideo -pix_fmt yuv420p /tmp/ref.yuv

# 3) 逐帧逐平面比较（注意 pitch 跨行 + NV12 交织）
python3 - <<'PY'
import glob,re
W,H,pitch=320,240,None
ref=open('/tmp/ref.yuv','rb').read()
for i,f in enumerate(sorted(glob.glob('/tmp/vkd/*.nv12'))):
    pitch=int(re.search(r'_p(\d+)\.nv12',f).group(1))
    d=open(f,'rb').read()
    Y=bytearray(W*H); C=bytearray(W*H//2)
    for y in range(H):   Y[y*W:(y+1)*W]=d[y*pitch:y*pitch+W]
    for y in range(H//2):C[y*W:(y+1)*W]=d[pitch*H+y*pitch:pitch*H+y*pitch+W]
    r=ref[i*W*H*3//2:(i+1)*W*H*3//2]
    print(i+1, Y==r[:W*H], C[0::2]==r[W*H:W*H*5//4], C[1::2]==r[W*H*5//4:])
PY
```

修复后的实测（`RADV_PERFTEST=video_decode` 必开，否则 RADV 不暴露
`VK_KHR_video_queue/decode/h264_decode` 与 video-decode 队列族，`vkCreateInstance`
直接失败，现象是 shim 日志 `FAIL: no video decode queue family` / 错误 801）：

| 码流 | 帧数 | Vulkan | VAAPI |
|---|---|---|---|
| `testdata/h264/test_p_b_5.h264` | 9 | 9/9 bit-exact，errors=0 | 9/9 bit-exact |
| `testdata/h264/test_p_b_7.h264` | 13 | 13/13 bit-exact，errors=0 | 13/13 bit-exact |
| `testdata/h264/test_p_main.h264` | 30 | 30/30 bit-exact，errors=0 | 30/30 bit-exact |
| `testdata/h264/test_p_66.h264` | 30 | 30/30 bit-exact，errors=0 | 30/30 bit-exact |

---

## 附录 A：码流结构（`test_p_main` / `test_p_b_*`，`trace_headers` 真值）

- 320x240，Main profile，level 2.0。SPS：`max_num_ref_frames=4`、
  `log2_max_frame_num=4`、`pic_order_cnt_type=0`、`log2_max_pic_order_cnt_lsb=6`、
  `frame_mbs_only=1`；PPS：`num_ref_idx_l0_default_active_minus1=2`（默认 3）、
  `l1=0`（默认 1），每 slice 用 override 覆盖。

| 解码序 | NAL | type | nal_ref_idc | frame_num | poc_lsb | L0/L1 active | 参考表修改 |
|---|---|---|---|---|---|---|---|
| #00 | IDR | I | 3 | 0 | 0 | – | – |
| #01 | non-IDR | P | 2 | 1 | 8 | 1 / 0 | – |
| #02 | non-IDR | B | **2**（参考帧） | 2 | 4 | 1 / 1 | 无 |
| #03 | non-IDR | B | **0** | 3 | 2 | 1 / **2** | 无 |
| #04 | non-IDR | B | 0 | 3 | 6 | 2 / 1 | 无 |
| #05 | non-IDR | P | 2 | 3 | 16 | **4** | **L0 有修改** |

shim 日志与真值逐字段一致（修复前后都一样，说明语义层从来不是问题）：

```
type=1 poc=0 fn=0 ref=3 rc0=0 rc1=0 setup_slot=0  refs=0   ← #00
type=2 poc=8 fn=1 ref=2 rc0=1 rc1=1 setup_slot=1  refs=1   ← #01
type=3 poc=4 fn=2 ref=2 rc0=1 rc1=1 setup_slot=2  refs=2   ← #02
type=3 poc=2 fn=3 ref=0 rc0=1 rc1=2 setup_slot=3  refs=3   ← #03（修复前挂在这一帧）
```

---

## 附录 B：相关源码线索

- `cuda_shim/src/vulkan_h264_decoder.cpp`
  - `ensure_session()`：instance/device/队列族/video session/session params/图像池/RESET，
    以及**会话内存所有权**
  - `decode()`：参考槽构建（升序槽位）、setup 槽、slice 写入、`vkCmdDecodeVideoKHR`、
    decode 队列 + transfer 队列双提交（semaphore/fence）、导出拷贝（平面单位换算）
  - `parse_slice_header()`：已确认与码流真值一致
- `cuda_shim/src/vulkan_h264_decoder.h`：`session_mems_`、`dpb_images_`、`free_dpb_slots_`、
  `outputs_`、`exports_`、`device_lost_`、`decode_query_`
- `cuda_shim/src/vaapi_h264_decoder.cpp`：VAAPI 后端（当前同样位精确）
- `cuda_shim/tests/verify_kernel_read.cpp`：`CUVID_DUMP_NV12_DIR` 的支持点
- Mesa（只读参考）：`src/amd/vulkan/radv_video.c`（`get_h264_msg`、TIER_2 DPB、
  `copy_bind` 的裸指针语义）、`src/amd/vulkan/meta/radv_meta_copy.c`
  （拷贝路径选择）、`src/amd/common/ac_vcn_dec.h`（固件消息结构）
- FFmpeg 7.1（只读参考）：`libavcodec/vulkan_h264.c`、`libavcodec/vulkan_decode.c`、
  `libavutil/hwcontext_vulkan.c`、`libavutil/vulkan.c`
