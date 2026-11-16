# NPP Stage A AMD candidate vs NVIDIA 11.5 golden（2026-11-13）

> Golden：`docs/evidence/NPP_StageA_NVIDIA_11.5_golden/`（RTX 4060 / NPP 11.5.1.107）
> Candidate：当前 AMD HIP shim（RX 7900 XTX / ROCm 7.2.4）
> 构建头文件：本机 CUDA/NPP 12.0.1.104（**最终构建基线，无需 11.5 头文件重建**）；运行时 ABI/golden 为 NPP 11.5.1.107
> 代码基线：`fc1091d` + 本轮 resize 语义修正

## 结果

**PASS：32/32 manifest 条目、2,490,624 bytes 逐字节一致。**

- 24 个 resize 条目（4 组 geometry × NN/linear/cubic）
- 8 个颜色转换条目（P2P3/P3P2 × full/ROI）
- padding、ROI 外区域和输入 raw 全部比对
- golden 自身 `SHA256SUMS` 校验通过
- 差异为 0；`compare.log` 是完整逐文件 PASS + SHA256 记录

FFmpeg 联合链路：

- `scale_npp` 的 NN/linear/cubic 均成功执行（5 帧/模式）
- 默认 `interp_algo`（cubic）输出与显式 cubic 完全一致
- 解码 → NPP P2P3 → hwdownload：VAAPI/VK 均为 9/9 帧，逐字节一致
- yuv420p → hwupload → NPP P3P2 → hwdownload：5/5 帧，逐字节一致

## 反推出的 NPP 语义

通过 NPP 11.5 golden 与 NPP 12 PTX 交叉确认：

1. 源坐标使用**绝对目的坐标**映射，destination ROI 只决定写入区域；
2. source ROI 是裁剪窗口，ROI 外目的像素保持原值；
3. 写入条件使用源 ROI 的半像素边界；
4. 采样邻域 clamp 到完整源图；
5. linear 使用 float32 计算，输出采用 NPP 的带符号 0.5 + 向零截断舍入；
6. `NPPI_INTER_CUBIC` 不是 Catmull-Rom，而是 NPP 的 4-tap fast-cubic 权重：

```text
w0 = -u(1-u)(2-u)/6
w1 = (1-u²)(2-u)/2
w2 = u(u+1)(2-u)/2
w3 = -u(1-u)(1+u)/6
```

该公式在 NPP 的 0.5x 下采样处与 Catmull-Rom 等价，因此早期只用 0.5x 样例会误判为 Catmull-Rom。

## 合规与边界

- FFmpeg 的 `libnpp` 仍被上游标记 **nonfree**；本地 `scale_npp` 构建不可再分发。
- 最终构建头文件固定为 CUDA/NPP 12.0.1.104；由于 P0 函数 ABI 未变，shim 对 NPP 11.5 golden 全量匹配。**不再要求**在 NPP 11.5 头文件环境下重建；11.5.1.107 仅作为运行时 ABI/SONAME 与数值 baseline。
- Stage A 只覆盖 NN/linear/cubic；FFmpeg 其余 `interp_algo` 和 NPP 其余 interpolation flags 尚未实现。
- 真实业务线程/队列、1080p/4K、长稳和目标平台仍未验收。

## 文件

- `compare.log`：32 条完整 PASS 和 golden SHA256
- `candidate-sha256.log`：AMD candidate 逐文件摘要
- `candidate-manifest.tsv`：AMD candidate 几何清单
- `environment.log`：Git、NPP 包、shim 符号、FFmpeg 依赖和源码 hash
- `ffmpeg-resize-{nn,linear,cubic,default}.log`
- `ffmpeg-p2p3-{vaapi,vk}.log`、`ffmpeg-p3p2.log`
- `ffmpeg-output-sha256.log`

目录不包含 candidate raw 或构建产物；可由 `tests/npp/npp_stage_a_probe` 重新生成。
