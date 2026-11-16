# FFmpeg scale_npp 端到端矩阵 — NVIDIA golden（2026-11-16）

> Golden：真实 NVIDIA NPP **11.5.1.107**（RTX 4060），未使用仓库 `npp_shim`。
> FFmpeg：**7.1**，`--enable-nonfree --enable-libnpp`，链接系统真实 NPP。
> 对照：AMD candidate 目录 [NPP_FFmpeg_scale_npp_AMD_2026-11-16](../NPP_FFmpeg_scale_npp_AMD_2026-11-16/README.md)。

代码基线：`1aa6c6b02dd2ac20d2962dff38aa5e02fdda53b9`

## 结论

- **40/40 case 通过**，每个 case 连续运行 2 次，输出 SHA256 完全一致。
- 合计 **26,307,120 bytes** 原始输出，`runtime=cuda`。
- 输入参考帧摘要与 AMD 侧一致：
  `19ec213dafde22863f5abdb53e4b91f668264c244bcf6e5cdcebd83aa4b6caf6`。
- 以 `--hashes-only` 与 AMD candidate 的 `manifest.tsv` 比较：
  **40/40 case、26,307,120 bytes 的 SHA256 全部一致**，端到端矩阵的 NVIDIA
  golden 与逐字节对照至此补齐。

矩阵为 5 组目标尺寸（160x120、214x118、640x480、320x240、100x100）
× 4 种插值模式（`nn`、`linear`、`cubic`、`default`）
× 2 种表面格式（`nv12` 走 P2P3 → resize → P3P2；`yuv420p` 逐平面 P3）。

## 环境

| 项目 | 实际值 |
|---|---|
| GPU | NVIDIA GeForce RTX 4060（Compute Capability 8.9，8188 MiB） |
| NVIDIA 驱动 | 595.99.02（`nvidia-smi` 报告 CUDA 支持级别 13.2） |
| NPP | 11.5.1.107（Ubuntu 包 `libnpp*11 11.5.1.107~11.5.1-1ubuntu1`） |
| NPP 头文件 | `/usr/include`（`NPP_VER_MAJOR=11`、`MINOR=5`、`PATCH=1`、`BUILD=107`） |
| NPP 运行库 | `/usr/lib/x86_64-linux-gnu/libnpp*.so.11.5.1.107` |
| CUDA Toolkit / nvcc | 11.5 / 11.5.119 |
| FFmpeg | 7.1，gcc 11.4.0（Ubuntu 22.04） |

驱动报告的 CUDA 13.2 是支持级别；本矩阵实际使用 CUDA 11.5 头文件、工具和
NPP 运行库。

## FFmpeg 构建（nonfree，仅本机验证，不可再分发）

该构建启用 `libnpp`，FFmpeg 将其标记为 **nonfree / unredistributable**，
只用于本机生成 golden；构建产物、NVIDIA 头文件和库均未提交进仓库。

```bash
./configure \
  --prefix=/tmp/ffmpeg-npp-install \
  --disable-static --enable-shared --disable-doc \
  --enable-nonfree --enable-libnpp \
  --enable-cuda --enable-cuvid --enable-nvdec --enable-ffnvcodec \
  --disable-filter=scale2ref_npp --disable-filter=transpose_npp \
  --disable-filter=sharpen_npp \
  --extra-cflags=-I/usr/include \
  --extra-ldflags="-L/usr/lib/x86_64-linux-gnu"

make -j"$(nproc)"
```

配置结尾为 `License: nonfree and unredistributable`。链接检查确认解析到真实
NPP，未出现仓库 `npp_shim`：

```text
libnppig.so.11  => /lib/x86_64-linux-gnu/libnppig.so.11
libnppicc.so.11 => /lib/x86_64-linux-gnu/libnppicc.so.11
libnppc.so.11   => /lib/x86_64-linux-gnu/libnppc.so.11
```

完整配置行与运行时链接见 [environment.log](environment.log)。

## 生成与校验命令

```bash
FFMPEG_BUILD_DIR=/tmp/ffmpeg-7.1-npp-src NPP_RUNTIME=cuda \
  OUT_DIR=/tmp/scale-npp-nvidia tests/npp/run_ffmpeg_scale_npp.sh

# 输出：
# RESULT: PASS (40 cases, 26307120 bytes, runtime=cuda)

python3 tests/npp/compare_ffmpeg_scale_npp.py \
  /tmp/scale-npp-nvidia docs/evidence/NPP_FFmpeg_scale_npp_AMD_2026-11-16 \
  --hashes-only
# PASS(hash-only): 40 cases and 26307120 bytes have identical SHA256
```

`source.sha256` 为 `19ec213d...b6caf6`，与 AMD candidate 完全一致，说明两端
输入帧相同。

## libnppc 核心 API 探针

`nppc_core_probe` 链接真实 `libnppc.so.11`（`/usr/lib/x86_64-linux-gnu`）。
完整输出见 [nppc_core_probe.log](nppc_core_probe.log)。

**需要记录的版本差异**：真实 NPP 11.5.1.107 的 `nppGetLibVersion()` 返回
`11.5.1`（`{major=11, minor=5, build=1}`），因此按项目既有命令使用的
`--expect-lib-version 11.5.107` 会得到 1 个失败（仅版本字符串检查），其余
设备、流、stream-context 检查全部通过；改用实际字符串
`--expect-lib-version 11.5.1` 后 `RESULT: PASS (0 failures)`。

头文件宏为 `PATCH=1 / BUILD=107`，包版本为 `11.5.1.107`，与安装介质一致；
差异仅在于 `NppLibraryVersion` 的第三个字段。仓库 shim 目前虚拟化为
`{11, 5, 107}`（见 `npp_shim/src/nppc_core.cpp`），与真实库返回值不同。
本轮按约束未修改 shim 或 AMD 侧代码。FFmpeg 的 `configure` 只检查
`nppGetLibVersion` 符号存在，不比较返回值，因此不影响本矩阵 golden 的生成。

## 文件

- `manifest.tsv`：40 个 case 的元数据与 SHA256
- `source.sha256`：输入帧摘要
- `SHA256SUMS`：全部 raw 输出（含 `source-reference.yuv`）摘要
- `environment.log`：FFmpeg 配置/链接、主机、NPP 包与头文件版本
- `logs/`：每个 case 的 FFmpeg 日志与 `scale_npp` 帮助输出
- `nppc_core_probe.log`：`libnppc` 核心 API 探针结果（两种版本检查）

**raw 输出不入库**：所有 `.yuv` 文件（含 `source-reference.yuv`）仅保留摘要，
可用上述命令由 `tests/npp/run_ffmpeg_scale_npp.sh` 重新生成。
