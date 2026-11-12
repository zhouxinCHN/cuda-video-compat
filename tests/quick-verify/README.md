# quick-verify

快速验证两组最容易暴露问题的链路：

- `run_kernel_read.sh`：真实 decode → map → HIP kernel 读取 mapped `dev_ptr` → `hipEventSynchronize` → unmap。
- `run_ffmpeg_compare.sh`：FFmpeg `h264_nvdec` / `h264_cuvid` 像素闭环 + D2D 拷贝计数。

完整说明、依赖、预期结果与排障见
[docs/verification/quick-verify.md](../../docs/verification/quick-verify.md)。

```bash
# 从仓库根目录
BACKENDS=vaapi ./tests/quick-verify/run_kernel_read.sh
BACKENDS=vaapi ./tests/quick-verify/run_ffmpeg_compare.sh
```
