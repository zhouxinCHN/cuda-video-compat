# NPP Stage B AMD shim vs NVIDIA 11.5 golden（2026-11-17）

> Golden：[NPP Stage B NVIDIA 11.5](../NPP_StageB_NVIDIA_11.5_golden/README.md)
> Candidate：当前 AMD HIP shim（RX 7900 XTX / ROCm 7.2.4）
> 代码基线：`231b440` + 本轮 Lanczos 实现（NPP 0.01 表 + 线性插值）

## Lanczos

### 探针（API 级）

`--stage b --skip cubic2p_* ,super`，4 个几何全部执行：

| 几何 | 差异字节 | 总字节 |
|---|---|---|
| `full_320x240_to_160x120`（0.5x） | 0 | 23,040 |
| `full_320x240_to_480x270`（1.5x） | **1** | 138,240 |
| `roi_288x224_to_144x112`（0.5x ROI） | 0 | 17,920 |
| `small_64x48_to_37x29_offset`（非整数） | 0 | 2,048 |

合计 181,248 字节中 1 字节不同（值差 1）。该差异来自 NPP 常数表的
末尾精度：NPP 用 0.01 距离网格表 + 线性插值，shim 用同一公式现场生成表，
个别表项与 NPP 的浮点结果差 1 ulp，在恰好 .5 边界处翻 1。

### FFmpeg 端到端

`ALGOS=lanczos`、5 个目标尺寸 × nv12/yuv420p：

| 几何 | 结果 |
|---|---|
| 160x120、320x240、640x480（整数比） | **逐字节一致** |
| 214x118、100x100（非整数比） | SHA256 不同 |

非整数比 case 的差异尚未定量：N 卡只归档了 manifest/SHA，未归档 raw。
下一步需要 N 卡把 `ffmpeg-lanczos` 的 raw `.yuv` 归档（约 6.6 MB）或在 N 卡
直接与 AMD raw 做逐字节 diff，确认是否同样只是 1–2 个像素。

## CUBIC2P

NVIDIA NPP 11.5.1.107 **运行库不支持** cubic2p：探针与 FFmpeg 均返回
`-22 (NPP_INTERPOLATION_ERROR)`，库里没有对应内核。shim 已实现三个 kernel，
但默认与 11.5 行为对齐（返回 `NPP_INTERPOLATION_ERROR`）；设置
`NPP_SHIM_ENABLE_CUBIC2P=1` 才启用。没有 11.5 golden，数值一致性无法验证。

## SUPER

NPP 11.5 支持 SUPER（仅双轴下采样），golden 已有 3 个几何；shim **尚未实现**，
当前返回显式错误。0.5x 下采样已确认是 2×2 均值，但 64x48→37x29 的非整数
下采样与简单面积/中心平均都不一致，需要继续反推 NPP 的 supersampling 内核。

## 复现

```bash
make -C tests/npp GPU_RUNTIME=shim NPP_INCLUDE_DIR=/usr/include \
  NPP_LIB_DIR=../../npp_shim ROCM_DIR=/opt/rocm
./tests/npp/npp_stage_a_probe --output /tmp/sb-amd --stage b \
  --skip cubic2p_bspline,cubic2p_catmullrom,cubic2p_b05c03,super

FFMPEG_BUILD_DIR=/path/to/ffmpeg NPP_RUNTIME=shim OUT_DIR=/tmp/ff-lanczos-amd \
  GEOMETRIES="160x120 214x118 640x480 320x240 100x100" ALGOS="lanczos" \
  tests/npp/run_ffmpeg_scale_npp.sh
```

## 文件

- `compare.log`：探针逐几何差异、FFmpeg 逐 case 哈希结果、环境
- raw 输出不归档，可由上述命令重新生成
