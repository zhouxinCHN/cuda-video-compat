# hip-decode-20261119 — Vulkan Video H.264 线性解码 + HIP 读取验证

> 本目录是**驱动能力探针**，不是生产实现。它回答两个问题：
>
> 1. Vulkan Video 能否把 H.264 直接解码进 `VK_IMAGE_TILING_LINEAR` 的 image，
>    导出 OPAQUE_FD 后被 HIP 当作普通设备指针读取？（原 demo 的结论，本次保留回归）
> 2. 在目标栈上，哪些 **profile / chroma / 位深 / 分辨率 / 码流特性** 能这样解，
>    哪些被驱动拒绝？（本次新增的覆盖矩阵）

结果记录见 `validation.txt`；结论只对记录里的硬件、驱动和码流负责。

## 1. 验证内容

- **输出布局**：解码输出 image 用 `VK_IMAGE_TILING_LINEAR` +
  `VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR` + external memory 创建，
  `vkGetMemoryFdKHR(OPAQUE_FD)` 导出，`hipImportExternalMemory` 导入后用 kernel 读取。
  DPB image 仍是 OPTIMAL（参考帧不需要给 HIP 看）。
- **码流解析**：复用 FFmpeg 的 `ff_h2645_packet_split`、SPS/PPS 解析、
  `ff_h264_parse_ref_count`、`ff_h264_pred_weight_table`、`ff_h264_init_poc`
  （只做解析，解码全部走 `vkCmdDecodeVideoKHR`）。
- **DPB**：I/P/B、多参考、滑动窗、MMCO 1、IDR 清空；POC type 0/2；
  每帧把全部参考槽 + setup 槽（`slotIndex = -1` 的非激活绑定）提交给驱动。
- **逐帧逐字节比较**：参考由 `ffmpeg -pix_fmt <fmt> -f rawvideo` 生成（显示序）；
  解码帧按 `(GOP, POC)` 排序后比较。crop 流按 SPS 的显示区域比较（输出是 coded 尺寸）。
- **能力探测**：`./decode --probe` 打印 profile × chroma × 位深 的
  `vkGetPhysicalDeviceVideoCapabilitiesKHR` 结果和 decode-DST 格式列表。

## 2. 构建与运行

依赖：

| 组件 | 用途 |
|---|---|
| Mesa Vulkan 头（`MESA_DIR`，默认 `$HOME/mesa`） | `vulkan_core.h` + `vk_video/*` |
| FFmpeg 7.1 构建树（`FFMPEG_DIR`，默认 `$HOME/ffmpeg-7.1`） | 9 个解析 `.o` + `libavutil.so`（与 `cuda_shim/Makefile` 同一组） |
| ROCm（`hipcc`） | HIP 导入与比较 kernel |
| libx264（8-bit 构建即可） | `gen_stream` 生成测试码流 |
| `ffmpeg` 二进制 | 软件参考 |

```bash
./run.sh                    # 全部矩阵
CASES="profiles" ./run.sh   # 只跑 profile/特性组（profiles resolutions chroma depth fixture）
KEEP=1 ./run.sh             # 保留临时构建目录和每个用例的日志
```

退出码：0 = 没有 FAIL（SKIP 允许）；1 = 有 FAIL；2 = 环境/参数错误。

`decode` 单独用法：

```bash
./decode <stream.h264> <reference.raw> <ref-format>   # PASS/FAIL/SKIP(77)
./decode --probe                                        # 驱动能力矩阵
```

`ref-format`：`nv12` `p010le` `yuv420p` `yuv422p` `yuv444p` `yuv420p10le`
`yuv422p10le` `yuv444p10le`。参考必须是**显示尺寸**，帧数必须等于解码帧数。

## 3. 覆盖矩阵

| 维度 | 用例 | 说明 |
|---|---|---|
| Profile | Baseline（Constrained）、Main、High | 8-bit 4:2:0 可生成；High10/High422/High444 看驱动 |
| 熵编码 | CAVLC（Baseline）、CABAC（Main/High） | |
| 预测 | I/P/B、B-pyramid、多参考（4/8/16）、weighted pred/bipred、8x8 transform | |
| 结构 | 多 slice（4 slices/picture）、IDR 周期、open-GOP、intra refresh、AUD | |
| 滤波 | deblocking 开/关 | |
| 分辨率 | 96x96、176x144、320x240、640x368、854x480（crop）、1280x720、1920x1080（coded 1088 + crop）、3840x2160 | 覆盖非 16 对齐、宽/高 crop、最小到 4K |
| chroma | 4:2:0、4:2:2、4:4:4 | 8-bit 生成；422/444 需要驱动支持 |
| 位深 | 8-bit（NV12/P010 路径）、10-bit（P010） | 10-bit 需要 high-depth libx264 才能生成 |
| 其它 | 原始 640x368 Baseline 单 slice `fixture.h264` | 原 demo 的固定回归样本 |

## 4. 本机实测（RX 7900 XTX / RADV NAVI31 / Mesa 25.2.8 / ROCm 7.2.4）

`validation.txt` 的结论：**PASS=20，SKIP=3，FAIL=0**（validation layer 开启，无 VUID 报错）。

能力探测摘要：

| profile / chroma / 位深 | 结果 |
|---|---|
| Baseline / Main / High × 4:2:0 × 8-bit | 支持（dpb=17，activeRefs=16，64x64..4096x4096，DST 格式仅 NV12） |
| 任意 profile × 4:2:2 或 4:4:4 × 8-bit | caps 可能返回成功，但 decode-DST 格式列表只有 NV12 → `NO-DST-FORMAT`，demo 按 SKIP 处理 |
| 任意 profile × 10-bit | `VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR` |
| High 10 / High 4:2:2 / High 4:4:4（任何 chroma/位深） | `VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR` |

结论：**这块 GPU/驱动组合只支持 H.264 8-bit 4:2:0 的 Vulkan Video 解码**；
High10/422/444、10-bit 在驱动层就被拒绝，与兼容层 Vulkan 后端“只做 NV12 8-bit”的边界一致。
SKIP 的三个用例分别是 4:2:2、4:4:4（驱动拒绝）和 10-bit（本机 libx264 是 8-bit 构建）。

## 5. 未覆盖 / 已知限制

- **10-bit 没有实跑**：本机 libx264 是 8-bit 构建，无法生成 10-bit 码流。demo 已实现
  P010 输出、10-bit 参考解析和比较；在有 high-depth libx264 或外部 10-bit 码流的机器上
  `./decode stream ref p010le` 可直接验证（前提是驱动支持）。
- 4:2:2 / 4:4:4 只验证到“驱动明确拒绝”，没有可跑的驱动。
- 未覆盖的协议面：SP/SI slice、interlaced/MBAFF、POC type 1（解析已支持但无生成器）、
  MMCO 2–6 / 长期参考、FMO/ASO、`separate_colour_plane_flag`、12/14-bit、
  同一码流内 SPS/PPS 切换（demo 要求重建 session）。
- 只跑单进程单码流；性能、并发、长稳不在本探针范围（性能见 `docs/当前能力矩阵.md` 的未闭环项）。

## 6. 文件

| 文件 | 作用 |
|---|---|
| `decode.cpp` | Vulkan 解码 + 能力探测 + HIP 导入/比较；支持 P/B/多 slice/crop/多平面格式 |
| `gen_stream.c` | libx264 码流生成器（profile/chroma/位深/slices/refs/B-pyramid/…） |
| `run.sh` | 构建两个工具 + 运行矩阵 + 汇总 PASS/SKIP/FAIL |
| `fixture.h264` | 原 demo 的 640x368 Baseline 单 slice 固定样本，保留作回归 |
| `validation.txt` | 本机一次完整运行的记录（环境、能力矩阵、用例结果）；仓库忽略 `*.log`，原始输出以 `.txt` 跟踪 |
