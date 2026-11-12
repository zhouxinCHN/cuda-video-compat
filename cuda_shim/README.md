# cuda_shim — `libcuda.so.1` + `libnvcuvid.so.1`

CUDA 驱动 API 与 CUVID 解码 API 的兼容层：

- `libcuda.so.1`：设备/上下文/内存/流/事件映射到 HIP；kernel launch、module/JIT、GL interop 等返回明确的不支持错误。
- `libnvcuvid.so.1`：CUVID parser、decoder、`cuvidMapVideoFrame64` / `cuvidUnmapVideoFrame64`；
  默认 VAAPI 后端，设置 `CUVID_SHIM_BACKEND=vk` 切到 Vulkan Video 后端。

实现与约束见：

- [CUDA 解码链路内存与同步模型](../docs/architecture/decode-memory-sync-model.md)
- [Vulkan 后端说明](../docs/architecture/vulkan-backend.md)
- [API 覆盖基线](../docs/architecture/api-coverage.md)
- [AMD 验证机交接](../docs/verification/handoff-amd.md)

## 构建

依赖 ROCm、libva/libdrm、Vulkan loader、FFmpeg 7.1 的解析对象、nv-codec-headers；Vulkan 后端还需要 Mesa 的 `vk_video/*` 头。

```bash
cd cuda_shim

make -j"$(nproc)" libcuda.so.1 libnvcuvid.so.1 \
  ROCM_DIR=/opt/rocm \
  FFMPEG_DIR=/path/to/ffmpeg-7.1 \
  NVCODEC_DIR=/path/to/nv-codec-headers/include \
  MESA_DIR=/path/to/mesa \
  VULKAN_LIB=-l:libvulkan.so.1
```

常用验证目标：

```bash
make check                    # 符号面（模拟 FFmpeg dlsym）
make check_func               # libcuda 功能子集（47 项）
make verify_cuvid_session     # CUVID 会话创建（无像素）
make verify_cuvid_parser      # parser 回调（无解码）
make verify_kernel_read       # mapped dev_ptr 的 HIP kernel 读取 + event 顺序
```

`verify_kernel_read` 是设备端测试，必须用 `hipcc`；如果 ROCm clang 找不到系统 libstdc++，
用 `HIPCC_EXTRA` 补路径（见 [Quick Verify](../docs/verification/quick-verify.md)）。

## 运行

```bash
export LD_LIBRARY_PATH=$PWD:/path/to/ffmpeg-7.1/libavutil:${LD_LIBRARY_PATH:-}

./verify_symbols ./libcuda.so.1
./verify_cuvid_symbols ./libnvcuvid.so.1
./test_func_full

CUVID_SHIM_BACKEND=vaapi ./verify_kernel_read ../testdata/h264/test_p_b.h264
CUVID_SHIM_BACKEND=vk    ./verify_kernel_read ../testdata/h264/test_p_b.h264
```

`CUDA_SHIM_TRACE_COPY=1` 会统计 D2D 的 `cuMemcpy2D*` / `cuMemcpyDtoD*` 次数，进程退出时打印汇总；
用于区分 `h264_nvdec` 的真零拷贝与 `h264_cuvid` 的 FFmpeg 内部拷贝。

## 状态与边界

- VAAPI 后端的真实 decode/map 像素链路尚未完成正式验收。
- Vulkan 后端的 `h264_nvdec`、多流、seek/flush、动态分辨率仍未验收。
- 异步跨栈 fence 未实现；当前是主机同步基线。
- 本目录的测试是可行性验证，不是生产验收。
