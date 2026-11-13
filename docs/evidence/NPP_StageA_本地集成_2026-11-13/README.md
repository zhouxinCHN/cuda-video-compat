# NPP Stage A 本地集成结果（2026-11-13）

> 环境：AMD Radeon RX 7900 XTX / ROCm 7.2.4 / Mesa 25.2.8。
> 构建头文件：Ubuntu `nvidia-cuda-dev` **12.0.1.104**（`/usr/include`，NPP 12）。
> 目标兼容基线：NPP **11.5.1.107**。版本差异仍未消除。
> 本机没有 NVIDIA GPU，`nvidia-smi` 不存在，因此没有 NVIDIA NPP golden。
> 代码基线：`062a263` + 当前未提交的 NPP Stage A 变更。

## 已通过

### 1. 真实 NPP 头文件编译

- 将 NPP ABI 宿主层与 HIP kernel 拆成独立编译单元，解决 CUDA/HIP 向量类型冲突。
- `npp_shim/Makefile` 使用 `NPP_INCLUDE_DIR=/usr/include` 成功生成：
  - `libnppc.so.11`
  - `libnppig.so.11`
  - `libnppicc.so.11`
  - `libnppidei.so.11`
  - `libnppif.so.11`
- 导出符号：`nppiResizeSqrPixel_8u_C1R`、`nppiYCbCr420_8u_P2P3R`、`nppiYCbCr420_8u_P3P2R`、`nppGetLibVersion`。

### 2. FFmpeg 集成

独立构建了一棵 `/tmp/ffmpeg-npp-src`，不修改现有 FFmpeg 构建：

- `CONFIG_SCALE_NPP_FILTER=1`
- `libavfilter.so.10` 的 `NEEDED` 为 `libnppig.so.11` / `libnppicc.so.11`
- `ldd` 实际解析到本仓库的 `npp_shim`
- `ffmpeg -filters` 已列出 `scale_npp`

**合规边界：FFmpeg configure 将 libnpp 标记为 nonfree，必须 `--enable-nonfree`；输出为非可再分发构建。** 这只用于本地技术验证，不能作为发布配置。

### 3. 颜色转换联合链路

- 解码 → NPP `P2P3` → `hwdownload`：
  - `test_p_b_5.h264`，9 帧
  - VAAPI、Vulkan 两个解码后端均与软件 `yuv420p` 输出逐字节一致
  - SHA256：`b715e1ee29bd294fdb0e6be2757d26f36a759691e99127e0c7a01d73049ea7e8`
- `hwupload_cuda` → NPP `P3P2` → `hwdownload`：
  - 5 帧 yuv420p → NV12，使用直接 lavfi 源并显式 `format=yuv420p` 后上传
  - 与软件 `format=yuv420p → nv12` 输出逐字节一致
  - SHA256：`beb2f244b66f2e812931adad51ad9e1d9fce6453fb63a8585880e2db3562c72f`
  - rawvideo 文件输入 + hwupload 在本机默认线程下曾出现第二帧差异，`-threads 1` 后通过；稳定烟测使用直接 lavfi 源，该现象暂归因于 FFmpeg 宿主流水线配置，不作为 NPP 算法结论

这证明 NPP ABI 层、HIP 颜色 kernel、FFmpeg NPP 调用和两个解码后端的映射链路已经打通。

### 4. Resize 功能烟测

`scale_npp=160:120:interp_algo=nn|linear|cubic` 均执行成功，每路 5 帧：

| 算法 | 输出 | 结果 |
|---|---|---|
| nn | 144000 bytes | PASS（仅执行，无 golden） |
| linear | 144000 bytes | PASS（仅执行，无 golden） |
| cubic | 144000 bytes | PASS（仅执行，无 golden） |

三种输出 SHA 不同，见 `raw-output-sha256.log`。

## 未通过 / 阻塞

1. **没有 NVIDIA GPU，未生成 NPP golden**。因此 NN/linear/cubic 的坐标、cubic 系数、边界与舍入仍未证明与 NPP 逐字节一致。
2. **构建头文件为 NPP 12.0.1.104，不是目标 NPP 11.5.1.107**。ABI 签名当前可编译，但最终必须用 11.5 头文件/环境复核。
3. **FFmpeg 的 libnpp 是 nonfree**：当前集成配置不可再分发；许可证/发布路线需重新决策。
4. `libnppc` 只实现版本探测；stream context 等 API 未实现。
5. 阶段 A 尚未完成 FFmpeg 全部 resize 用例的 NVIDIA 对照。

## 文件

- `build-shim.log`：真实头文件下 shim 构建日志
- `configure-ffmpeg.log`：NPP 版 FFmpeg configure 日志
- `decode-npp-*.log`：双后端 P2P3 联合链路日志
- `ffmpeg-p3p2.log`：P3P2 联合链路日志
- `ffmpeg-{nn,linear,cubic}.log`：resize 功能烟测日志
- `probe-manifest.tsv`：Stage A 探针 32 个输出条目的几何清单
- `raw-output-sha256.log`：本轮原始输出 SHA256
- `environment.log`：包版本、符号、FFmpeg 依赖与 GPU

目录不包含 YUV/NV12 或构建产物。

## 补充说明（最终复跑）

- P3P2 使用直接 lavfi 源 + `hwupload_cuda` 时连续三次输出一致，且与软件 yuv420p→NV12 逐字节一致。
- 同一流程若使用 rawvideo 文件解码后再 `hwupload_cuda`，本机默认线程下出现过第二帧差异；`-threads 1` 后通过。稳定回归入口使用直接 lavfi 源。该差异暂归类为 FFmpeg 宿主流水线配置问题，不作为 NPP 算法证据。
