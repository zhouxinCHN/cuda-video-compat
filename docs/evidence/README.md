# evidence — 历史验证结果

这里保存历史实验的原始结果、日志与报告，供复核结论时追溯。它们**不是** CI 自动产物，
也不代表当前代码在任意机器上的最新结果。

## 目录

- `results/`：NPP / NVDEC / VAAPI 对比结果（JSON、Markdown、日志、SHA256、小体积样本）。
- `p0-nvidia-hardware.log`：NVIDIA 参考机硬件、驱动与工具链信息。
- `p0-nvidia-npp-check.log`：NVIDIA 参考机的 NPP 头文件/库/包清单（环境证据，不是参考输出）。
- `P0闭环验证结果_2026-11-12.md`：AMD 验证机 P0 双后端 × 四路码流结果汇总（码流已入库；原始命令/日志仍待补）。
- `FFmpeg宿主双后端基线_2026-11-13/`：四路码流 × VAAPI/VK × 四种宿主模式的 32 组合基线；含自动摘要、完整命令、环境、每组合日志/PTS 和逐项结果。
- `FFmpeg_PTS顺序_2026-11-13/`：带 PTS/DTS 的 30 帧 MP4 输入，验证 VAAPI/Vulkan 八种模式的严格 PTS/显示顺序比较。
- `NPP_StageA_本地集成_2026-11-13/`：NPP shim 真实头文件构建、FFmpeg `scale_npp` 接入、双后端 P2P3/P3P2 逐字节联合链路；NVIDIA resize golden 仍缺。

## 约定

- 归档时保留原始命令、环境（GPU/驱动/ROCm/Mesa/FFmpeg 版本）和判据。
- 绝对路径已脱敏为 `<repo>`；复现时按当前目录结构替换。
- 小体积参考样本可以提交；大体积 YUV/码流请放 `testdata/` 或用脚本生成，不重复提交。
- 不要用“程序跑通”代替“像素/数值一致”；证据里要写清楚验证边界。
