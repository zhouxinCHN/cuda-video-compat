# benchmarks — NVDEC / VAAPI / NPP / VPP 对比

这些程序用于能力矩阵和性能对比，不是兼容层构建的一部分。结果历史归档在 [`docs/evidence/results/`](../docs/evidence/results/)。

| 目录/文件 | 用途 |
|---|---|
| `nvdec/nvdec_benchmark.cpp` | NVDEC SDK 直测（CUVID parser 模式） |
| `nvdec/build_nvdec_benchmark.sh` | NVDEC benchmark 编译脚本 |
| `npp/npp_benchmark.cpp` | NPP 26 项 API 直测（CUDA event 计时） |
| `npp/build_npp_benchmark.sh` | NPP benchmark 编译脚本 |
| `vpp/vpp_benchmark.c` | VAAPI VPP 直测（libva） |
| `vpp/build_vpp_benchmark.sh` | VPP benchmark 编译脚本 |
| `scripts/test_vaapi_nvdec.sh` | FFmpeg 层格式矩阵（NVIDIA/AMD） |
| `scripts/test_nvdec_sdk.sh` | NVDEC SDK 一键测试 |
| `scripts/test_npp_vpp.sh` | NPP/VPP 后处理操作矩阵 |
| `scripts/merge_decode_compare.sh` | 合并 NVDEC + VAAPI 结果 |
| `scripts/collect_ffmpeg_api.py` | 采集 FFmpeg 实际 CUVID/CUDA/NPP 调用面 |

## 运行

这些脚本依赖 NVIDIA CUDA/NPP 或 AMD VAAPI/ROCm 环境。编译脚本直接调用 `nvcc` / 系统编译器，不读取 `ROCM_DIR` / `CUDA_DIR` 之类的配置变量；测试脚本按 `--nvidia` / `--amd` / `--quick` / `--json` 参数选择机器。

```bash
cd benchmarks/scripts
./test_vaapi_nvdec.sh --nvidia
./test_vaapi_nvdec.sh --amd
./merge_decode_compare.sh
```

性能结果必须用完整业务链路测量；单项硬件解码速度不能替代业务收益。
