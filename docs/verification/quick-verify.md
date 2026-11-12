# 快速验证：mapped dev_ptr 的 kernel 可访问性 + FFmpeg 双路径像素/零拷贝

这三项是为补齐《docs/architecture/decode-memory-sync-model.md》验证矩阵里最容易快速闭合的空白：

| 验证 | 补的空白 | 通过判据 |
|---|---|---|
| `verify_kernel_read` | 实验 C 只证明 `hipMemcpy` 能读，未证明 kernel 能直接寻址 mapped `dev_ptr` | 30 帧 GPU kernel 的 Y/UV checksum 与 CPU 对同一指针回读一致；`hipEventSynchronize` 后才 unmap |
| `run_kernel_read.sh` | 同步只做过 S1/S2 协议模拟 | 真实 decode→map→kernel→event→unmap 跑通，VAAPI/VK 两个后端 |
| `run_ffmpeg_compare.sh` | VAAPI CUVID shim 像素链路未闭合；`h264_nvdec` 零拷贝未验收 | `h264_nvdec`/`h264_cuvid` 输出与软解参考逐字节一致；`h264_nvdec` 在 VAAPI 后端 D2D 拷贝次数为 0 |

> 注意：`verify_kernel_read` 的 GPU/CPU checksum 读的是同一块 mapped 显存，所以它证明的是“kernel 能按 pitch 正确寻址这块显存”，不是“解码像素一定正确”。像素正确性仍以实验 C / `verify_vk_decode_to_hip` 为准。`h264_cuvid` 的 D2D 拷贝是 FFmpeg `cuviddec.c` 自身行为，不是 shim 引入的。

## 一、本次改动

开发机（NVIDIA RTX 4060）上新增/修改的文件：

```text
cuda_shim/src/libcuda.cpp                      # CUDA_SHIM_TRACE_COPY：D2D 拷贝计数
cuda_shim/tests/verify_kernel_read.cpp     # kernel 可访问性 + 真实 event 顺序
cuda_shim/Makefile                        # verify_kernel_read 目标、HIPCC/VULKAN_LIB 变量
tests/quick-verify/run_kernel_read.sh           # 两个后端跑 verify_kernel_read
tests/quick-verify/run_ffmpeg_compare.sh        # FFmpeg 像素 + 零拷贝计数
docs/verification/quick-verify.md                    # 本文件
```

## 二、拷贝到 AMD 机器

在开发机执行（`amd` 换成你的 SSH 主机别名）：

```bash
REPORT=/path/to/repo
rsync -av "$REPORT/cuda_shim" "$REPORT/tests/quick-verify" "$REPORT/testdata" \
  amd:~/cuda-video-compat/
```

这会同步整个 `cuda_shim`（源码、Makefile）、`tests/quick-verify` 和 `testdata`。只覆盖同名文件，不会删除 AMD 机器上的其他内容；如果你只想传改动文件，至少确保下面这些到了：

```text
cuda_shim/src/libcuda.cpp
cuda_shim/tests/verify_kernel_read.cpp
cuda_shim/Makefile
testdata/h264/test_p_b.h264
testdata/h264/test_320x240.h264
tests/quick-verify/
```

如果 AMD 机器上已经有上一版的 `cuda_shim`、`ffmpeg-7.1`、`nv-codec-headers`、`mesa`，只需要同步上面这些文件；**不需要**再拷整个 FFmpeg/Mesa 源码树。构建依赖沿用 `docs/verification/handoff-amd.md` 里的最小集：

- `~/ffmpeg-7.1/libavcodec/*.h`（全量头）+ 9 个 `.o`：`h2645_parse/h264_ps/h2645_vui/h2645data/h264data/h264_parse/golomb/mathtables/refstruct`
- `~/ffmpeg-7.1/libavutil/*.h` + `libavutil.so`
- `~/nv-codec-headers/include`
- `~/mesa/include`（Vulkan Video std 头；只编 VAAPI 也需要它，因为 `libnvcuvid` 同时包含 Vulkan 后端）
- 系统包：`libva-dev`、`libva-drm`、`libdrm-dev`、`libvulkan-dev`（或至少有 `libvulkan.so.1`）、`zlib1g-dev`

如果 AMD 机器从未准备过上述依赖，按 `docs/verification/handoff-amd.md` §1.2 的 rsync 命令先做一遍。

## 三、在 AMD 机器上构建

```bash
cd cuda_shim

make -j"$(nproc)" libcuda.so.1 libnvcuvid.so.1 test_func_full \
  ROCM_DIR=/opt/rocm \
  FFMPEG_DIR=$HOME/ffmpeg-7.1 \
  NVCODEC_DIR=$HOME/nv-codec-headers/include \
  MESA_DIR=$HOME/mesa \
  VULKAN_LIB=-l:libvulkan.so.1
```

说明：

- `VULKAN_LIB=-l:libvulkan.so.1` 用于只有运行时 `libvulkan.so.1`、没有 `libvulkan.so` 开发软链的机器。装了 `libvulkan-dev` 时可以省略，默认就是 `-lvulkan`。
- 某些 ROCm clang 找不到系统 libstdc++ 时（报 `Could not find standard C++ header 'cmath'`），编译 kernel 测试要补：

```bash
make verify_kernel_read \
  ROCM_DIR=/opt/rocm \
  HIPCC_EXTRA="-I/usr/include/c++/11 -I/usr/include/x86_64-linux-gnu/c++/11 -L/usr/lib/gcc/x86_64-linux-gnu/11"
```

- 验证机是 RX 7900 XTX（gfx1100）时，建议显式指定目标架构，避免 hipcc 默认编成别的 gfx 导致 kernel 加载失败：

```bash
make verify_kernel_read \
  ROCM_DIR=/opt/rocm \
  HIPCC_EXTRA="--offload-arch=gfx1100"
```

  两种 `HIPCC_EXTRA` 可以合并；`--offload-arch` 按实际 `rocminfo | grep gfx` 输出调整。

- `verify_kernel_read` 必须用 `hipcc`（设备端 kernel），所以没有放进 `make all`；单独构建即可。

## 四、运行

### 1. 先确认 FFmpeg 有两条解码器

```bash
ffmpeg -hide_banner -decoders | grep -E 'h264_(cuvid|nvdec)'
```

如果没有输出，说明 AMD 机器上的 `ffmpeg` 不是目标版本；用 `FFMPEG=ffmpeg` 或重新编译 FFmpeg 7.1。

### 2. mapped dev_ptr kernel 读取 + event 顺序

```bash
cd cuda_shim
export LD_LIBRARY_PATH=$PWD:$HOME/ffmpeg-7.1/libavutil:${LD_LIBRARY_PATH:-}

./verify_kernel_read ../testdata/h264/test_p_b.h264
CUVID_SHIM_BACKEND=vk ./verify_kernel_read ../testdata/h264/test_p_b.h264
```

或一次跑两个后端：

```bash
../tests/quick-verify/run_kernel_read.sh
```

预期：每帧打印 `Ysum/UVsum ... OK`，最后 `PASS: mapped dev_ptr is kernel-addressable ... (30 frames)`。VK 后端如果机器没有 Vulkan Video 解码，会明确失败；可先只跑 `BACKENDS=vaapi`。

### 3. FFmpeg 双路径像素 + 零拷贝

```bash
cd /path/to/repo
./tests/quick-verify/run_ffmpeg_compare.sh
```

只跑 VAAPI（先排除 VK 环境问题）：

```bash
BACKENDS=vaapi ./tests/quick-verify/run_ffmpeg_compare.sh
```

保留中间文件排查：

```bash
KEEP=1 ./tests/quick-verify/run_ffmpeg_compare.sh
```

脚本会自己生成软解参考 `reference.nv12`，然后对每个组合做：

1. `ffmpeg -c:v h264_nvdec` / `h264_cuvid` 解 30 帧；
2. `hwdownload,format=nv12` 回读为 NV12；
3. `cmp` 与软解参考逐字节比较；
4. 从 `CUDA_SHIM_TRACE_COPY=1` 的 stderr 汇总里解析 D2D 拷贝次数。

预期结果：

| 组合 | 像素 | D2D 拷贝 | 说明 |
|---|---|---|---|
| `vaapi/h264_nvdec` | 一致 | **0** | 真正的零拷贝；若 >0 则说明 nvdec 路径有额外拷贝 |
| `vaapi/h264_cuvid` | 一致 | 每帧固定次数 | FFmpeg `cuviddec.c` 自己 `cuMemcpy2DAsync` 到 CUDA frame，属预期 |
| `vk/h264_cuvid` | 一致 | 每帧固定次数 | 同上；VK 后端已有 30 帧记录 |
| `vk/h264_nvdec` | 可能失败 | — | 已知缺口；脚本默认按 SKIP 处理，`ALLOW_VK_NVDEC_FAIL=0` 可改为硬失败 |

### 4. 零拷贝计数器自检（可选）

```bash
cd cuda_shim
CUDA_SHIM_TRACE_COPY=1 ./test_func_full 2>&1 | grep copy-trace
```

`test_func_full` 里有一次 DEVICE→DEVICE 的 `cuMemcpy2DAsync`，所以应看到 `D2D=1` 左右；如果一行都没有，说明环境变量没生效或进程没有正常退出。

## 五、结果怎么记

建议直接记录三行原始证据，不要只写“通过”：

```text
verify_kernel_read vaapi: frames_ok=30 errors=0 PASS
verify_kernel_read vk   : frames_ok=30 errors=0 PASS
run_ffmpeg_compare      : vaapi/nvdec pixel=identical D2D=0; vaapi/cuvid pixel=identical D2D=NN
```

如果其中任何一项失败，优先保留 `KEEP=1` 的 `WORK` 目录、FFmpeg stderr 和 `verify_kernel_read` 的完整输出；这些是定位 shim 问题的最小证据。

## 六、常见问题

| 现象 | 处理 |
|---|---|
| `Cannot load libnvcuvid.so.1` | `LD_LIBRARY_PATH` 必须把 `cuda_shim` 放在系统路径前面；`ls -l libnvcuvid.so.1` 确认 soname |
| `verify_kernel_read` 在 `cuvidCreateDecoder` 失败 | VK 后端先看 `vulkaninfo \| grep -i video`；VAAPI 先看 `vainfo` |
| `hipcc` 报找不到 `cmath`/`cstdlib` | 加 `HIPCC_EXTRA`（见上） |
| `cannot find -lvulkan` | `sudo apt install libvulkan-dev`，或 `VULKAN_LIB=-l:libvulkan.so.1` |
| FFmpeg 输出与软解不一致 | 先把 `KEEP=1` 的 raw 文件用 `cmp -l` 看首个差异；再检查是否走了 `h264_cuvid`（它本来会多一次 D2D） |
| 没有 `copy-trace` 行 | 确认 `CUDA_SHIM_TRACE_COPY=1` 且 FFmpeg 正常退出；必要时把 `cuMemcpy2DAsync_v2` 的计数改成逐次打印做临时排查 |

## 七、这次验证的边界

- 这次只补“kernel 可寻址”和“FFmpeg 宿主双路径像素/零拷贝计数”，没有补多流、seek/flush、P010/奇数分辨率、NPP、异步 fence。
- `verify_kernel_read` 逐帧做 GPU checksum + CPU 回读，适合验证正确性，不适合做性能结论；map/unmap 的耗时和 1/8 路 30 分钟稳定性仍需单独测。
- VK 的 `h264_nvdec` 直通路径这次只做“先跑、记录行为”，如果失败，先按已知缺口记录，不在这次改动范围内修。
