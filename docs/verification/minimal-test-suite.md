# 最小验证实验套件（遗留风险证伪/确认）

本目录是《docs/architecture/cuda-compat-plan-integrated.md》遗留问题与风险点的最小验证用例实现。目标：**以最低成本证伪或确认文档 §4 / §1.7 / §7.2 标注的"待验证 / 设计假设 / 遗留"项**。

## 实验总览

| 实验 | 风险点 | 文档出处 | 运行环境 | 状态 |
|---|---|---|---|---|
| A | parser 复用（SPS/PPS/填充） | §4.6 / §7.3 源文件已丢 | 本机 CPU | ✅ 已跑通 8/8 |
| B | FFmpeg 干净(LGPL)构建 .o 可提取 | §4.6 法律 / §4.14 依赖 | 本机 CPU | ✅ 已跑通 |
| C | 真实解码→HIP 读端到端 | §1.7第1步 / §4.2-A4 / R1 | AMD GPU | ✅ I-only 11/11；I/P 帧版 12/12；I/P/B 帧版 12/12；2026-11-12 P0 四路码流双后端各 82/82 逐字节一致 |
| D | HIP 写→VAAPI 编码端到端 | §1.7第2步 / §4.10 / R2 | AMD GPU | 编译通过，pipeline 10/10 但输出码流异常（暂停，先做 E） |
| E | 目标平台可移植性 5 项探测 | §4.9 / §1.7第0步 / R6 | AMD/目标平台 | ✅ 已跑通 6/6 |

## 目录布局

```
tests/parser/                    # 本机 CPU 实验（无 GPU 依赖）
  verify_parser_reuse.cpp        # 实验 A
  verify_parser_reuse.make
  run_lgpl_build_check.sh        # 实验 B
  npp_p0_reference.cpp           # NPP 参考输出

tests/zerocopy/                  # AMD GPU 实验（需 ROCm + Mesa）
  verify_decode_to_hip.cpp       # 实验 C
  verify_decode_to_hip.make
  verify_hip_to_encode.cpp       # 实验 D
  verify_hip_to_encode.make
  run_hip_encode_compare.py      # 实验 D 像素比对脚本
  verify_portability.cpp         # 实验 E
  verify_portability.make
  verify_vk_decode_to_hip.cpp    # Vulkan Video P0 闭环

testdata/h264/                   # 集中管理的 H.264 测试码流
  test_320x240.h264              # I-only
  test_p_320x240.h264            # I/P
  test_b_320x240.h264            # I/P/B
  test_p_b.h264                  # shim 快速验证用 I/P/B
```

## 本机（CPU）运行 —— 实验 A / B

```bash
cd tests/parser

# 实验 A：parser 复用复现（8/8 PASS）
make -f verify_parser_reuse.make
./verify_parser_reuse ../../testdata/h264/test_320x240.h264

# 实验 B：FFmpeg 干净 LGPL 构建下 8 个 .o 可提取
./run_lgpl_build_check.sh
```

实验 A/B 的关键结论：
- 8 个 `.o`（`h2645_parse/h264_ps/h2645_vui/h2645data/h264data/golomb/mathtables/refstruct`）可静态链接，不拖入编解码器依赖。
- 这 8 个源文件**全部 LGPL 许可**；干净构建（`CONFIG_GPL=0/CONFIG_NONFREE=0/CONFIG_PIC=1`）下 8 个 `.o` 仍可生成并重链接 → 法律路线成立。
- SPS/PPS 从 `sps_list[]/pps_list[]` 取（非 `ps.sps/ps.pps`）；PPS 字段名 `cabac`/`deblocking_filter_parameters_present` 已规避文档踩坑。
- **新踩坑**：编译 FFmpeg 内部头需 `#define HAVE_AV_CONFIG_H 1`（否则 `golomb.h` 的 `SUINT` 未定义）；该宏已在实验 C 中标注。

## AMD 机器运行 —— 实验 C / D / E

前提：AMD GPU + ROCm/HIP + Mesa radeonsi(libva) + ffmpeg（生成参考帧/软解）。

```bash
cd tests/zerocopy

# 实验 C：真实解码→HIP 读（§1.7 第1步）
make -f verify_decode_to_hip.make
make -f verify_decode_to_hip.make run      # I-only 30 帧
make -f verify_decode_to_hip.make run-p    # I+P 帧（1 IDR + 29 P，验证 DPB 参考复用）
make -f verify_decode_to_hip.make run-b    # I+P+B 帧（1 I + 10 P + 19 B，验证 B 帧 + 显示序重排）
#   测试码流已固定在 testdata/h264/；需要重新生成时见 testdata/README.md

# 实验 D：HIP 写→VAAPI 编码（§1.7 第2步）
make -f verify_hip_to_encode.make
make -f verify_hip_to_encode.make run

# 实验 E：可移植性 5 项探测（§1.7 第0步止损阀）
make -f verify_portability.make
./verify_portability
```

注意：`verify_decode_to_hip.make` / `verify_hip_to_encode.make` 里的 `FFMPEG`（源码目录）、`ROCM_DIR`、`FFMPEG_BIN` 需按目标机器实际路径覆盖：

```bash
make -f verify_decode_to_hip.make FFMPEG=/path/to/ffmpeg-src ROCM_DIR=/opt/rocm FFMPEG_BIN=/usr/bin/ffmpeg
```

## 每个实验的证伪判据

- **A**：链接失败（依赖扩散）或 SPS/PPS 字段错误 → §4.6 结论需修正。
- **B**：干净构建下任一 `.o` 缺失或重链接失败 → 分发路线需改。
- **C**：多帧各 plane 逐字节一致 + 无 fd/内存泄漏 + 无非法访问（= §1.7 第1步原文）。
- **D**：回读与 HIP 输入在编码量化内逐字节一致。
- **E**：任一项缺失 → 对应零拷贝/同步链路降级，触发 §1.7 第0步范围复审。

## 已知限制与待办

- 2026-11-12 P0 结果：四路码流（`test_p_b_5`、`test_p_b_7`、`test_p_main`、`test_p_66`）在 VAAPI/Vulkan 两个后端上各 82/82 帧逐字节一致；`verify_kernel_read` errors=0。四路码流已入库 `testdata/h264/`，原始命令/日志仍待归档，详见 [P0 结果记录](../evidence/p0-2026-11-12.md)。


- 实验 C 已升级为**I/P/B 帧版**：在 I/P 帧（1 IDR + 29 P）基础上，新增 B 帧支持——`direct_spatial_mv_pred` 解析（ref_count 前）、双参考列表按 POC 排序（list0=[POC<cur 降序]+[POC>cur 升序]，list1 反之）、`weighted_bipred_idc` 检测、**程序内显示序重排**（解码序收集全部帧 → 按 POC 稳定排序 → 与显示序软解参考逐帧比对）。范围：frame_mbs_only=1、poc_type=0/2、weighted_pred=0、无场/MBAFF。长期参考、crop、seek 仍是后续。
- 实验 C 修掉的单帧版 3 个 bug（profile 映射 / separate layers 导出 / pitch 逐行拷贝）在 I/P/B 帧版中全部保留修复。
- **I/P/B 帧版新增踩坑（AMD 实测）**：
  1. **`log2_max_pic_order_cnt_lsb_minus4` 少减 4**：FFmpeg 的 `sps->log2_max_poc_lsb` 存的是完整位数（`h264_ps.c:403` `t+4`，t 才是 minus4 值），VAAPI 字段名虽含 minus4 但硬件按 `值+4` 解读。直接传完整值会让硬件按 12 bit 读 POC LSB，slice 数据整体错位 4 bit——**连 IDR 帧都错**，不是只有 B 帧错。已改为 `sps->log2_max_poc_lsb - 4`。
  2. **DPB 滑动窗口误删参考帧**：滑动窗口（§8.2.5.1）只在 `nal_ref_idc != 0`（参考帧）解码后执行。旧代码 else 分支对 B 帧（非参考帧）也执行了滑动窗口，导致第一个 B 帧解完误删 DPB 里最旧的 IDR，第二个 B 帧只剩 1 个参考帧 → 局部像素错（差异约 5-15%，非全错）。已改为 `else if (si.nal_ref_idc != 0)`。
  3. 补填 VAAPI `VAPictureParameterBufferH264` 的 `frame_num`、`pic_order_present_flag`、`reference_pic_flag` 字段（对齐 FFmpeg `vaapi_h264.c`，虽非本次根因但属规范字段）。
- **I/P 帧版两个关键坑（AMD 实测踩到）**：
  1. `h264_vaapi` 编码器在 `bframes=0` 时**强制 poc_type=2**（`vaapi_encode_h264.c:378`: `sps->pic_order_cnt_type = ctx->max_b_depth ? 0 : 2`）。所以带 P 帧的测试码流用 VAAPI 生成时必然是 poc_type=2，不是默认假设的 poc_type=0。代码已支持 poc_type=0/2 双路径。
  2. AMD 机器无 libx264，`-preset` 参数报 `Unrecognized option 'preset'`。改用 `h264_vaapi -profile:v 77` 生成 Main profile I/P 码流（`make ... run-p X264_ENC=...` 可配）。
- 实验 D 是**最小 VAAPI 编码器**，覆盖 CQP + packed headers 的 I 帧单帧；码控/GOP/B 帧语义是 §4.16 后续。
- 实验 C/D 的像素正确性依赖 AMD 机器实际运行确认；本机（NVIDIA 卡）只能验证编译通过 + 码流/参考文件逻辑正确（已核验 30 帧全 IDR）。
- 实验 C 的 slice header 手工解析尚未覆盖 `poc_type==1`（delta POC 全字段），遇到非 trivial 码流需补全（已注释标注）。
