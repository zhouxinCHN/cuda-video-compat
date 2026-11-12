# 零拷贝验证实验（ROCm + Mesa 同 GPU 内存互操作）

> 版本：v1.0
> 生成时间：2026-10-29
> 目的：在写大量 shim 代码之前，验证"ROCm（HIP）和 Mesa（VAAPI）能否在同一块 AMD GPU 上通过 DMA-BUF 互操作内存"，决定整个方案走零拷贝还是拷贝。
> 硬件：AMD RX 7600（RDNA3/gfx1102）即可，无需 RX 9070。
> 预计耗时：1-2 天。
> 上位文档：[docs/architecture/cuda-compat-overview.md](../architecture/cuda-compat-overview.md) §7

---

## 1. 为什么先做这个实验

整个"解码→NPP→CUDA计算→编码"流程里，数据要在 **Mesa（VAAPI 内存）和 ROCm（HIP 内存）两套驱动栈**之间流动。两个跨栈边界能否零拷贝，取决于：

1. **ROCm 和 Mesa 能否同时驱动同一块 GPU**（同进程共存 + DMA-BUF 互操作）。
2. **HIP 设备内存能否导出为 DMA-BUF fd**（边界 2 的命门）。

这两个问题不验证清楚就动工，后面可能整体返工。**本实验用最小成本先回答它们。**

---

## 2. 环境准备

| 项 | 要求 |
|---|---|
| GPU | AMD RX 7600（RDNA3） |
| 内核驱动 | amdgpu（ROCm 和 Mesa 共用） |
| ROCm | 已安装（本方案用 7.2.0，含 `libamdhip64.so`） |
| Mesa | radeonsi（含 VAAPI 前端，`libva` + `libva-drm`） |
| 设备节点 | `/dev/dri/renderD128`（VAAPI 用） |
| 头文件 | `hip/hip_runtime_api.h`、`va/va.h`、`va/va_drm.h` |

---

## 3. 实验 0：环境确认

**目的**：确认 ROCm 和 Mesa 各自都能识别并驱动这块 GPU。

| 步骤 | 命令/操作 | 通过标准 |
|---|---|---|
| 0.1 ROCm 识别 | `hipGetDeviceCount`（或 `rocminfo`） | 返回 ≥1，能拿到 RX 7600 |
| 0.2 VAAPI 识别 | `vainfo`（或 `vaInitialize` on renderD128） | 能列出 radeonsi 的 profile |
| 0.3 设备节点 | `ls /dev/dri/` | 有 `renderD128` |

> 若 0.1 失败：RX 7600 的 ROCm 支持需排查（gfx1102 在 ROCm 支持列表里，但个别版本需注意）。
> 若 0.2 失败：Mesa VAAPI 未装或驱动未加载。

---

## 4. 实验 1：ROCm + Mesa 同进程共存

**目的**：验证同一进程里同时初始化 HIP 和 VAAPI 不冲突（这是所有后续工作的前提）。

**操作序列**：
```c
hipInit(0);                       // ROCm 初始化
hipGetDevice(&dev, 0);            // 拿 HIP 设备
VADisplay dpy = vaGetDisplayDRM(drm_fd);  // 打开 /dev/dri/renderD128
vaInitialize(dpy, &maj, &min);    // VAAPI 初始化
hipMalloc(&p, size);              // HIP 分配内存
vaCreateSurfaces(dpy, ...);       // VAAPI 创建 surface
```

**通过标准**：以上全部成功，无崩溃、无驱动错误、无上下文冲突。

**关键检查点**：
- HIP 的 GPU 内存分配和 VAAPI 的 surface 分配能否在同一进程里各自成功。
- 是否有"设备已被另一驱动栈占用"之类的错误。

> ⚠️ 若共存失败：整个"HIP + VAAPI 同进程"方案要重新评估（可能需拆成两个进程 + IPC，或改架构）。**这是最先要过的关。**

---

## 5. 实验 2：边界 1（VAAPI surface → HIP，解码→NPP 方向）

**目的**：验证 VAAPI 解码输出的 surface，能被 HIP kernel 零拷贝读取。

**操作序列**：
```c
// 1. VAAPI 侧：创建 surface 并导出 DRM PRIME fd
vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, w, h, &surf, 1, ...);
VASurfaceAttribExternalBuffers ext;
vaExportSurfaceHandle(dpy, surf,
    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
    VA_EXPORT_SURFACE_READ_WRITE, &ext);   // 得到 ext.buffers[i].fd（DMA-BUF）

// 2. HIP 侧：导入该 fd
hipExternalMemoryHandleDesc desc = {};
desc.type = hipExternalMemoryHandleTypeOpaqueFd;
desc.handle.fd = ext.buffers[0].fd;
desc.size = <surface 字节数>;
hipExternalMemory_t extMem;
hipImportExternalMemory(&extMem, &desc);          // ← 关键调用

// 3. 映射成 HIP 设备指针
hipExternalMemoryBufferDesc bufDesc = {};
bufDesc.offset = 0; bufDesc.size = desc.size;
hipDeviceptr_t hipPtr;
hipExternalMemoryGetMappedBuffer(&hipPtr, extMem, &bufDesc);

// 4. 用 HIP kernel 读 hipPtr，校验像素
```

**通过标准**：
- `vaExportSurfaceHandle` 成功导出 DMA-BUF fd。
- `hipImportExternalMemory(OpaqueFd)` 成功。
- HIP kernel 能读到 surface 里的正确像素内容。

**关键检查点**：
- `hipImportExternalMemory` 是否接受 VAAPI 导出的 DMA-BUF fd（这是 ROCm 与 Mesa 内存互操作的核心验证）。
- 读到的像素是否正确（说明真的零拷贝共享了物理内存，而不是读到垃圾）。

> ✅ 若通过：**解码→NPP 方向的零拷贝成立。**

---

## 6. 实验 3：边界 2（HIP 内存 → VAAPI，CUDA 计算→编码方向）

**目的**：验证 HIP 分配的设备内存，能被 VAAPI 当作编码输入 surface 零拷贝使用。**这是方案里最不确定的一步。**

**操作序列**：
```c
// 1. HIP 分配内存
hipMalloc(&hipPtr, size);

// 2. 把 HIP 内存导出为 DMA-BUF fd   ← 关键，路径不明确，需探索
//    候选路径：
//    a) hipMemGetHandleForAddressRange（HIP IPC handle，可能不是 DMA-BUF）
//    b) 通过 libdrm 的 amdgpu_bo_export（绕 HIP，直接用 amdgpu 内核接口）
//    c) ROCr/HSA 的内存导出接口

// 3. VAAPI 侧：导入该 fd 作为 surface
VASurfaceAttrib attribs[2];
attribs[0].type = VASurfaceAttribMemoryType;
attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
attribs[1].value.value.p = &ext;   // ext.buffers[i].fd 指向上一步的 fd
vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, w, h, &surf, 1, attribs, 2);

// 4. 用该 surface 做 VAAPI 编码，校验输出
```

**通过标准**：
- HIP 内存能导出为 DMA-BUF fd。
- VAAPI 能导入该 fd 创建 surface。
- 编码输出正确（说明 HIP 写入的数据被 VAAPI 正确读到）。

**关键检查点**：
- **HIP 内存导出 DMA-BUF 的路径是否存在**（HIP 标准 API 可能不直接支持，需走 libdrm/amdgpu）。
- VAAPI 导入外部内存后能否正常编码。

> ⚠️ 若 HIP 导出 DMA-BUF 走不通：**边界 2 只能走拷贝**（hipMemcpyDtoH → 上传 surface）。这不致命，但要提前知道，把"编码前的一次拷贝"计入性能预算。

---

## 7. 实验 4：端到端数据正确性（可选，实验 2/3 通过后）

**目的**：把"解码→处理→编码"串起来，验证整条数据通路的像素正确性。

**操作**：
```
H264 码流 → VAAPI 解码到 surface
  → （边界1）HIP 读 surface，做个简单处理（如反相/灰度）
  → （边界2）处理后的 HIP 内存作为 VAAPI 编码输入
  → VAAPI 编码输出 H264
  → 解码输出验证像素符合预期
```

**通过标准**：最终输出的视频内容 = 输入视频经处理后的内容，无花屏、无错位、无色彩错误。

---

## 8. 结果判定与后续决策

| 实验结果 | 结论 | 对方案的影响 |
|---|---|---|
| 实验 1 失败 | ROCm/Mesa 不能同进程共存 | **方案级风险**，需重新评估架构（可能拆进程或换路线） |
| 实验 1✅ + 实验 2✅ | 边界 1 零拷贝成立 | 解码→NPP 零拷贝可行 |
| 实验 3✅ | 边界 2 零拷贝成立 | 全链路零拷贝可行，性能最优 |
| 实验 3❌（HIP 导出不了 DMA-BUF） | 边界 2 只能拷贝 | 编码前加一次拷贝，功能可行、性能有损，提前计入预算 |

**决策原则**：
- 只要实验 1、2 通过，方案主体（解码→NPP→计算）就能走通。
- 实验 3 决定"最后一公里"是零拷贝还是拷贝，不阻塞主线开发，但影响性能设计。

---

## 9. 关键技术参考（已核实）

| 能力 | 依据 |
|---|---|
| HIP 支持 `hipImportExternalMemory(OpaqueFd)` | `/opt/rocm/include/hip/hip_runtime_api.h` |
| Mesa 支持 `vaExportSurfaceHandle(DRM_PRIME)` | `mesa/src/gallium/frontends/va/surface.c` |
| HIP `hipExternalMemoryGetMappedBuffer` 存在 | 同 HIP 头文件 |
| **HIP 导出 DMA-BUF 的解法** | **HSA `hsa_amd_portable_export_dmabuf`**（`/opt/rocm/include/hsa/hsa_ext_amd.h`），HIP 标准 API 无导出，走 HSA |
| 注意：HIP import 只有 `OpaqueFd`，无 `DmaBufFd` | `hipExternalMemoryHandleType` 枚举 |

---

## 10. 可执行验证程序（已就绪）

已提供现成的可执行程序，在 AMD 机器上**一条命令**跑完三个实验：

```
目录：`tests/zerocopy/`
文件：
  verify_zerocopy.cpp   # 主程序（实验1/2/3 全包含）
  Makefile              # 一键构建+运行
```

**在 AMD 机器上的操作**：

```bash
cd /path/to/repo/tests/zerocopy

# 1. 构建（若 ROCm 不在 /opt/rocm，用 ROCM_DIR=... 覆盖）
make

# 2. 运行
make run        # 或直接 ./verify_zerocopy
```

**输出解读**：
- 每个实验输出 `[PASS]` / `[FAIL]` 及原因。
- 末尾 `========== 汇总 ==========` 给总 PASS/FAIL 数和结论。
- **预期关键结果**：
  - 实验1 应 PASS（若 FAIL，ROCm/Mesa 共存有问题，方案级风险）。
  - 实验2 应 PASS（VAAPI→HIP import 是标准路径）。
  - 实验3 是关键——`hsa_amd_portable_export_dmabuf` 是否成功决定边界2能否零拷贝。

**程序已验证**：在本机（NVIDIA 无 AMD GPU）编译链接通过、错误处理健壮（优雅 FAIL 并报原因）；在 AMD 机器上应直接编译运行。

> 注意：实验3 里 `vaCreateSurfaces` 导入 HIP 导出的 DMA-BUF，若驱动对 NV12 的 pitch/modifier 有特殊要求，可能需要微调 `desc` 的填充（pitch、drm_format_modifier）。这是实验现场可能需要调试的点。
