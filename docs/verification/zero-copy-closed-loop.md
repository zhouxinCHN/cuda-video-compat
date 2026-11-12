# 闭环验证说明（解码 / 编码）

## 目标

把"句柄/机制通"升级为"真实功能正确"，回答两个核心问题：

1. **解码闭环**：VAAPI 真实解码的输出，能否经 DMA-BUF 导出给 HIP 正确读取？
2. **编码闭环**：HIP 写入的数据，能否经 DMA-BUF 导入给 VAAPI 正确编码？

## 关键认知（为什么闭环=shim 的里程碑）

真正的"解码→HIP→编码"全程零拷贝，必须发生在**单个进程**内：

```
libva 解码 → 导出 fd → HIP import → HIP 处理 → HSA export → VAAPI 编码
```

这本质上就是 **shim（libnvcuvid.so.1 + libnvidia-encode.so.1）自己要实现的路径**。
独立的小工具无法跨进程传递 dma-buf fd（fd 生命周期绑定进程）。所以：

> **"端到端像素闭环"不是一个独立脚本，而是 shim 实现的第一个里程碑（M2/M4）。**

## 已验证的组件（每一段都有人证）

| 环节 | 验证 | 结果 |
|---|---|---|
| VAAPI 解码内容正确性 | `run_decode_loop.sh` 步骤2（FFmpeg vaapi 硬解 vs 软解逐字节） | ✅ |
| HIP↔外部内存像素往返 | `verify_zerocopy` 实验 A（HIP 写 Vulkan/DMA-BUF 内存读回全 0xAB） | ✅ |
| 线性 DMA-BUF 可被 VAAPI import | `retry_exp3_v2` R4（pitch 256/512/1024 全部成功） | ✅ |
| HIP 可导出 dma-buf | `verify_zerocopy` 实验3（HSA 导出 fd 成功） | ✅ |
| VAAPI 内容→编码→解码正确性 | `run_encode_loop.sh`（图案→h264_vaapi→解码→<1% 差异） | ✅ |
| 边界2 失败根因 | pitch 未 256 对齐（320 失败） | ✅ 已定位 |

## 唯一缺口（= shim M2 的第一个任务）

把以上各段**串进一个进程**：

> 用 libva 解码 1 帧到线性 surface → 导出 fd → HIP import 读像素 → 与软件解码参考帧对比

这就是 `libnvcuvid.so.1` 的 `cuvidCreateDecoder → cuvidDecodePicture → cuvidMapVideoFrame64`
最小实现，可作为 shim 的第一个可测里程碑。

## 脚本用法（AMD 机器）

```bash
cd /path/to/repo/tests/zerocopy
make                     # 编译全部工具
make loop-decode         # 解码闭环：FFmpeg vaapi 硬解 vs 软解 + 组件就绪检查
make loop-encode         # 编码闭环：图案 → h264_vaapi → 解码 → 逐帧对比
```

### 需要的示例输入

- `run_decode_loop.sh`：默认用 testsrc 生成 H264；也可 `./run_decode_loop.sh in.h264`
- `run_encode_loop.sh`：同上

### 前置条件

- `/dev/dri/renderD128` 存在且有权限（`sudo chmod 666 /dev/dri/renderD128` 可测）
- `ffmpeg` 支持 `-hwaccel vaapi` 与 `h264_vaapi` 编码器
- ROCm/HIP 已安装（`hip_read_surface` 用到）

## 分层建议

真正写 shim 时按这个顺序验收：

1. **M2-a**：libva 解码 1 帧线性 surface → 导出 → HIP 读 → 与软解对比（本闭环补上）
2. **M2-b**：加 parser（H264 I-only）→ 自填 VAAPI 参数 → 多帧
3. **M4**：HIP 写 → HSA 导出 → VAAPI 编码 1 帧 → 回读对比
4. **V-C 稳定性**：B 帧 + crop + seek/flush + 长稳