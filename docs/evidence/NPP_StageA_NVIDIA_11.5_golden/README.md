# NPP Stage A NVIDIA 11.5 golden

生成日期：2026-11-13

代码基线：`d514339975dd4f81c63237e73fd8feac90178522`

## 结论

- 使用真实 NVIDIA NPP **11.5.1.107** 头文件和运行库生成，未使用 AMD shim。
- `manifest.tsv` 含 **32 条**有效条目（24 条 resize、8 条颜色转换），对应 **32 个** raw 文件；无缺失或额外 raw 文件。
- 每个 raw 文件均非空且小于 2 MiB，最大文件为 138240 bytes。
- 在同一输出目录连续运行两次，`manifest.tsv` 和全部 raw 文件逐一 SHA256 相同。
- 本目录只记录 NVIDIA golden 的生成与可重复性；尚未执行 AMD shim candidate 比较，不代表 Stage A 全部验收条件已经通过。

## 环境

| 项目 | 实际值 |
|---|---|
| GPU | NVIDIA GeForce RTX 4060 |
| NVIDIA 驱动 | 595.99.02 |
| NPP | 11.5.1.107 |
| NPP Ubuntu 包版本 | 11.5.1.107~11.5.1-1ubuntu1 |
| CUDA Toolkit / nvcc | 11.5.1 / 11.5.119 |
| `/usr/include/cuda.h` | `CUDA_VERSION 11050` |
| `nvidia-smi` 驱动 CUDA 支持级别 | 13.2 |
| 探针运行库 | 系统 `libnppc.so.11`、`libnppig.so.11`、`libnppicc.so.11`、`libcudart.so.11.0` |

完整命令输出、NPP 头文件版本宏和动态链接解析见 [environment.log](environment.log)。驱动报告的 CUDA 13.2 是支持级别；本次构建实际使用 CUDA 11.5 头文件、工具和运行库。

## 生成与校验命令

```bash
git switch main && git pull --ff-only

make -C tests/npp clean all GPU_RUNTIME=cuda \
  NPP_INCLUDE_DIR=/usr/include \
  CUDA_INCLUDE_DIR=/usr/include \
  CUDA_LIB_DIR=/usr/lib/x86_64-linux-gnu \
  NPP_LIB_DIR=/usr/lib/x86_64-linux-gnu

mkdir -p docs/evidence/NPP_StageA_NVIDIA_11.5_golden

# 连续执行两次；比较两轮 manifest.tsv 与 raw 文件的 SHA256。
(cd tests/npp && ./npp_stage_a_probe --output ../../docs/evidence/NPP_StageA_NVIDIA_11.5_golden)
(cd tests/npp && ./npp_stage_a_probe --output ../../docs/evidence/NPP_StageA_NVIDIA_11.5_golden)

(cd docs/evidence/NPP_StageA_NVIDIA_11.5_golden && sha256sum -c SHA256SUMS)
```

两轮输出的 SHA256 清单比较无差异。完整性检查还逐条解析了 manifest 的第 4 列，确认 32 个引用文件全部存在，且 raw 文件集合与引用集合完全相同。

## 文件 SHA256

`SHA256SUMS` 记录 `manifest.tsv` 和全部 32 个 raw 文件的逐文件摘要；该清单自身的 SHA256 为：

```text
af15b7b4cd35f150c900b4da55c772ee330b42078755127cf3ebd5ff03b96424  SHA256SUMS
```

```text
047c01c78a488bd8bed1d6e3d24aff74be06b31b2bbdcca602843cf1c99fba06  manifest.tsv
33bec90e4bf96e2fdf794312bc65dc94ab2ebb6006261c6aa86b8e5b78fe3d85  p2p3__full_320x240__dst.yuv
7e2854521fab1f5a2d01b2e0d3b8d63c1b46f378287f3862073b99abe8e5cf72  p2p3__full_320x240__src.nv12
4637b9f23c8f9bb2ecc7a18a8506de5200a30664e7bf3a16ed2f9fd1c13a2179  p2p3__roi_288x224__dst.yuv
7e2854521fab1f5a2d01b2e0d3b8d63c1b46f378287f3862073b99abe8e5cf72  p2p3__roi_288x224__src.nv12
1d0f6b7d2223565e44e5a029490ec7b6729ce8df4f77435c63418b213b364548  p3p2__full_320x240__dst.nv12
5329c140cf798fc33fa6dc4c4c866305209e56471b94eecd4f908b808dabd796  p3p2__full_320x240__src.planar
ec38c3a8e8d2b58e81181b1ee14fe0a0f400b7d1c22804d250e96e60a9be1306  p3p2__roi_288x224__dst.nv12
5329c140cf798fc33fa6dc4c4c866305209e56471b94eecd4f908b808dabd796  p3p2__roi_288x224__src.planar
4eaa97bec50e4fb87bf304958234858525f1e9b69885e629b941de6d0709bf12  resize__full_320x240_to_160x120__cubic__dst.gray
a7912af2c2fe781317fb187ec56abb19bb17a6674df9a3f6ee7bf5d0900079ae  resize__full_320x240_to_160x120__cubic__src.gray
e1fbcf32ed91766599421b5b4c5257ac533ad0a9f2492cf61d76f34140f87e50  resize__full_320x240_to_160x120__linear__dst.gray
a7912af2c2fe781317fb187ec56abb19bb17a6674df9a3f6ee7bf5d0900079ae  resize__full_320x240_to_160x120__linear__src.gray
6d9ad3bf96f4f037b55b7fed32b411757a659a69b311daf46f0cf53993a4ed73  resize__full_320x240_to_160x120__nn__dst.gray
a7912af2c2fe781317fb187ec56abb19bb17a6674df9a3f6ee7bf5d0900079ae  resize__full_320x240_to_160x120__nn__src.gray
48af4d44b9418e2aa3cadc189e10cd83c3270b2936e0d1db0696ddf7a8b0180f  resize__full_320x240_to_480x270__cubic__dst.gray
a7912af2c2fe781317fb187ec56abb19bb17a6674df9a3f6ee7bf5d0900079ae  resize__full_320x240_to_480x270__cubic__src.gray
2223b3fd5fbabd33156163566f479af949e105446cff37be065cdd6bcc11d136  resize__full_320x240_to_480x270__linear__dst.gray
a7912af2c2fe781317fb187ec56abb19bb17a6674df9a3f6ee7bf5d0900079ae  resize__full_320x240_to_480x270__linear__src.gray
3994c716f69828ca712606230a9e8944a19100ecf8c626b120e38ee14704cca0  resize__full_320x240_to_480x270__nn__dst.gray
a7912af2c2fe781317fb187ec56abb19bb17a6674df9a3f6ee7bf5d0900079ae  resize__full_320x240_to_480x270__nn__src.gray
bf100313ff5ebdfbefefc6191fe110c4e832d7aeb30d6f523ccfa820ecba3f63  resize__roi_288x224_to_144x112__cubic__dst.gray
a7912af2c2fe781317fb187ec56abb19bb17a6674df9a3f6ee7bf5d0900079ae  resize__roi_288x224_to_144x112__cubic__src.gray
51c1827d6a00647e6e468d4f0bca0b71085b2a41f31b9b5e72f3bbc5641712e8  resize__roi_288x224_to_144x112__linear__dst.gray
a7912af2c2fe781317fb187ec56abb19bb17a6674df9a3f6ee7bf5d0900079ae  resize__roi_288x224_to_144x112__linear__src.gray
98fe5a714110a25119bdb68b8d38aa32d5e384f17c7bb48f3bf71daf861629ee  resize__roi_288x224_to_144x112__nn__dst.gray
a7912af2c2fe781317fb187ec56abb19bb17a6674df9a3f6ee7bf5d0900079ae  resize__roi_288x224_to_144x112__nn__src.gray
41e2ffca41279496811d702e2f0e00e18d188d7036a8ab6f0222e438baf77b79  resize__small_64x48_to_37x29_offset__cubic__dst.gray
fa073057a6127aeeea6d3fd65547f05d919411f6cab268b7084c7e28cbc0eca1  resize__small_64x48_to_37x29_offset__cubic__src.gray
26990b8fe2a81ed91d8feec84070e5a2a57365280328c0e1ea357adf85a74acb  resize__small_64x48_to_37x29_offset__linear__dst.gray
fa073057a6127aeeea6d3fd65547f05d919411f6cab268b7084c7e28cbc0eca1  resize__small_64x48_to_37x29_offset__linear__src.gray
0408acebacbde6f924a16f8d9e9b85eb5c62923c315792271994fe3ad51c6688  resize__small_64x48_to_37x29_offset__nn__dst.gray
fa073057a6127aeeea6d3fd65547f05d919411f6cab268b7084c7e28cbc0eca1  resize__small_64x48_to_37x29_offset__nn__src.gray
```

## 合规与范围

本次未构建 nonfree FFmpeg，未修改 `npp_shim` 或 resize 数值，也未复制或提交 NVIDIA 头文件、库和驱动。后续 candidate 生成后可运行：

```bash
python3 tests/npp/compare_stage_a.py \
  docs/evidence/NPP_StageA_NVIDIA_11.5_golden \
  /path/to/npp-candidate --verbose
```
