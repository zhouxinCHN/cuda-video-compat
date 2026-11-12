// test_func_full.cpp
// libcuda.so.1 功能真实性测试（不只是符号面，带数据校验）
//
// 覆盖 FFmpeg/视频路径真正会用到的核心函数：
//   [1] 设备/上下文：cuInit, cuCtxCreate, cuCtxPush/Pop, primary ctx
//   [2] 内存：cuMemAlloc, cuMemAllocPitch, HtoD/DtoH/DtoD, cuMemcpy2DAsync(多平面)
//   [3] 流/事件：cuStreamCreate, cuEventRecord/Synchronize, cuStreamSynchronize
//   [4] 数据正确性：写已知图案 → 读回 → 校验
//   [5] 版本/属性：cuDriverGetVersion（是否合理 CUDA 版本而非 ROCm 版本）
//
// 链接 libcuda.so.1（模拟 FFmpeg 方式），在 AMD 机器上运行。
// 运行：make && ./test_func_full

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#include <ffnvcodec/dynlink_cuda.h>

extern "C" {
CUresult cuInit(unsigned int);
CUresult cuDriverGetVersion(int*);
CUresult cuDeviceGetCount(int*);
CUresult cuDeviceGet(CUdevice*, int);
CUresult cuDeviceGetName(char*, int, CUdevice);
CUresult cuDeviceGetAttribute(int*, CUdevice_attribute, CUdevice);
CUresult cuDeviceComputeCapability(int*, int*, CUdevice);
CUresult cuCtxCreate_v2(CUcontext*, unsigned int, CUdevice);
CUresult cuCtxDestroy_v2(CUcontext);
CUresult cuCtxPushCurrent_v2(CUcontext);
CUresult cuCtxPopCurrent_v2(CUcontext*);
CUresult cuCtxGetDevice(CUdevice*);
CUresult cuCtxSynchronize(void);
CUresult cuMemAlloc_v2(CUdeviceptr*, size_t);
CUresult cuMemAllocPitch_v2(CUdeviceptr*, size_t*, size_t, size_t, unsigned int);
CUresult cuMemFree_v2(CUdeviceptr);
CUresult cuMemcpyHtoD_v2(CUdeviceptr, const void*, size_t);
CUresult cuMemcpyDtoH_v2(void*, CUdeviceptr, size_t);
CUresult cuMemcpyDtoD_v2(CUdeviceptr, CUdeviceptr, size_t);
CUresult cuMemcpy2DAsync_v2(const CUDA_MEMCPY2D*, CUstream);
CUresult cuMemsetD8Async(CUdeviceptr, unsigned char, size_t, CUstream);
CUresult cuStreamCreate(CUstream*, unsigned int);
CUresult cuStreamDestroy_v2(CUstream);
CUresult cuStreamSynchronize(CUstream);
CUresult cuEventCreate(CUevent*, unsigned int);
CUresult cuEventRecord(CUevent, CUstream);
CUresult cuEventSynchronize(CUevent);
CUresult cuEventDestroy_v2(CUevent);
CUresult cuDevicePrimaryCtxRetain(CUcontext*, CUdevice);
CUresult cuDevicePrimaryCtxRelease(CUdevice);
CUresult cuDevicePrimaryCtxGetState(CUdevice, unsigned int*, int*);
CUresult cuGetErrorString(CUresult, const char**);
}

static const char* cuGetErrorString_safe(CUresult r) {
    const char* s = nullptr;
    if (cuGetErrorString(r, &s) == CUDA_SUCCESS && s) return s;
    return "?";
}

static int g_pass = 0, g_fail = 0;
#define CHECK_CU(expr, label) do { \
    CUresult _r = (expr); \
    if (_r != CUDA_SUCCESS) { \
        printf("  [FAIL] %s: %d (%s)\n", label, (int)_r, cuGetErrorString_safe(_r)); \
        g_fail++; \
    } else { \
        printf("  [PASS] %s\n", label); \
        g_pass++; \
    } \
} while (0)

int main() {
    printf("===========================================================\n");
    printf(" libcuda.so.1 功能真实性测试（AMD 机器）\n");
    printf("===========================================================\n");

    CUdevice dev = 0;
    CHECK_CU(cuInit(0), "cuInit(0)");
    int devCount = 0;
    CHECK_CU(cuDeviceGetCount(&devCount), "cuDeviceGetCount");
    if (devCount == 0) {
        printf("  [FAIL] 无 AMD GPU，无法做功能测试\n");
        return 1;
    }
    printf("  [info] 设备数=%d\n", devCount);
    CHECK_CU(cuDeviceGet(&dev, 0), "cuDeviceGet(0)");

    char name[256];
    CHECK_CU(cuDeviceGetName(name, sizeof(name), dev), "cuDeviceGetName");
    printf("  [info] 设备名: %s\n", name);

    int major=0, minor=0;
    CHECK_CU(cuDeviceComputeCapability(&major, &minor, dev), "cuDeviceComputeCapability");
    printf("  [info] ComputeCapability=%d.%d\n", major, minor);

    int ver = 0;
    CHECK_CU(cuDriverGetVersion(&ver), "cuDriverGetVersion");
    printf("  [info] DriverVersion=%d (%d.%d)\n", ver, ver/1000, (ver%1000)/10);
    // 期望是 CUDA 语义版本（如 12000 表示 12.0），不应是 ROCm 的 7.2.x
    if (ver > 1000 && ver < 100000) {
        printf("  [info]  版本形态看起来是 CUDA 语义 (如 120xx/110xx)\n");
    } else {
        printf("  [WARN] 版本形态异常：%d，可能直接透传了 ROCm 版本\n", ver);
    }

    // ---- 上下文 ----
    CUcontext ctx = nullptr;
    CHECK_CU(cuCtxCreate_v2(&ctx, 0, dev), "cuCtxCreate_v2");
    CHECK_CU(cuCtxPushCurrent_v2(ctx), "cuCtxPushCurrent_v2");
    CUdevice ctxDev = 0;
    CHECK_CU(cuCtxGetDevice(&ctxDev), "cuCtxGetDevice");
    if (ctxDev == dev) { printf("  [PASS] ctx device == dev\n"); g_pass++; }
    else { printf("  [FAIL] ctx device=%d != dev=%d\n", (int)ctxDev, (int)dev); g_fail++; }
    CHECK_CU(cuCtxSynchronize(), "cuCtxSynchronize");
    CUcontext popped = nullptr;
    CHECK_CU(cuCtxPopCurrent_v2(&popped), "cuCtxPopCurrent_v2");

    // ---- 内存分配 + 数据正确性 ----
    const size_t N = 4096;
    CUdeviceptr dptr = 0;
    CHECK_CU(cuMemAlloc_v2(&dptr, N), "cuMemAlloc_v2(4096)");

    std::vector<unsigned char> host(N), back(N);
    for (size_t i = 0; i < N; i++) host[i] = (unsigned char)(i * 31);
    CHECK_CU(cuMemcpyHtoD_v2(dptr, host.data(), N), "cuMemcpyHtoD_v2");
    CHECK_CU(cuMemcpyDtoH_v2(back.data(), dptr, N), "cuMemcpyDtoH_v2");

    bool mem_ok = (memcmp(host.data(), back.data(), N) == 0);
    printf("  [%s] HtoD→DtoH 数据一致 (%zu bytes)\n", mem_ok ? "PASS" : "FAIL", N);
    mem_ok ? g_pass++ : g_fail++;

    // DtoD
    CUdeviceptr d2 = 0;
    CHECK_CU(cuMemAlloc_v2(&d2, N), "cuMemAlloc_v2(4096)#2");
    CHECK_CU(cuMemcpyDtoD_v2(d2, dptr, N), "cuMemcpyDtoD_v2");
    std::vector<unsigned char> back2(N);
    CHECK_CU(cuMemcpyDtoH_v2(back2.data(), d2, N), "cuMemcpyDtoH_v2#2");
    bool d2d_ok = (memcmp(host.data(), back2.data(), N) == 0);
    printf("  [%s] DtoD 数据一致\n", d2d_ok ? "PASS" : "FAIL");
    d2d_ok ? g_pass++ : g_fail++;
    CHECK_CU(cuMemFree_v2(d2), "cuMemFree_v2#2");

    // ---- cuMemAllocPitch（NV12 平面场景）----
    const size_t W = 320, H = 240;
    CUdeviceptr dpitched = 0;
    size_t dpitch = 0;
    CHECK_CU(cuMemAllocPitch_v2(&dpitched, &dpitch, W, H, 1), "cuMemAllocPitch_v2(320x240)");
    printf("  [info] pitch=%zu\n", dpitch);
    if (dpitch >= W && (dpitch % 256) == 0) {
        printf("  [PASS] pitch=%zu >= W=%zu 且 256 对齐\n", dpitch, W); g_pass++;
    } else {
        printf("  [FAIL] pitch=%zu 异常\n", dpitch); g_fail++;
    }
    CHECK_CU(cuMemFree_v2(dpitched), "cuMemFree_v2(pitched)");

    // ---- cuMemcpy2DAsync（NV12 Y 平面多行拷贝，模拟 FFmpeg nvdec 拷贝路径）----
    // 模拟：decode surface(pitch) → CUDA hwframe(另一次分配)，逐 plane 拷贝
    CUdeviceptr srcSurf = 0, dstHw = 0;
    size_t srcPitch = 512, dstPitch = 768;  // 不同 pitch，验证 2D 拷贝跨 pitch
    CHECK_CU(cuMemAlloc_v2(&srcSurf, srcPitch * H), "cuMemAlloc(src)");
    CHECK_CU(cuMemAlloc_v2(&dstHw, dstPitch * H), "cuMemAlloc(dst)");

    // 填 src 为已知图案（每行递增）
    std::vector<unsigned char> srcHost(srcPitch * H), srcBack(srcPitch*H);
    for (size_t y = 0; y < H; y++)
        for (size_t x = 0; x < srcPitch; x++)
            srcHost[y*srcPitch + x] = (unsigned char)(x + y);
    CHECK_CU(cuMemcpyHtoD_v2(srcSurf, srcHost.data(), srcHost.size()), "cuMemcpyHtoD(src)");

    // cuMemcpy2DAsync：src(320 宽, srcPitch 512) → dst(320 宽, dstPitch 768)
    CUDA_MEMCPY2D cpy = {};
    cpy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.srcDevice = srcSurf;
    cpy.srcPitch = srcPitch;
    cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.dstDevice = dstHw;
    cpy.dstPitch = dstPitch;
    cpy.WidthInBytes = W;
    cpy.Height = H;
    CHECK_CU(cuMemcpy2DAsync_v2(&cpy, nullptr), "cuMemcpy2DAsync(Y平面 320宽 pitch转换)");

    // 读回并校验每行前 320 字节正确
    std::vector<unsigned char> dstBack(dstPitch * H);
    CHECK_CU(cuMemcpyDtoH_v2(dstBack.data(), dstHw, dstBack.size()), "cuMemcpyDtoH(dst)");
    bool plane_ok = true;
    for (size_t y = 0; y < H && plane_ok; y++)
        for (size_t x = 0; x < W; x++)
            if (dstBack[y*dstPitch + x] != (unsigned char)(x + y)) { plane_ok = false; break; }
    printf("  [%s] cuMemcpy2DAsync 跨 pitch 拷贝数据正确（每行前 %zu 字节）\n",
           plane_ok ? "PASS" : "FAIL", W);
    plane_ok ? g_pass++ : g_fail++;
    CHECK_CU(cuMemFree_v2(srcSurf), "cuMemFree(src)");
    CHECK_CU(cuMemFree_v2(dstHw), "cuMemFree(dst)");

    // ---- 流/事件 ----
    CUstream stream = nullptr;
    CHECK_CU(cuStreamCreate(&stream, 0), "cuStreamCreate");
    CUevent ev = nullptr;
    CHECK_CU(cuEventCreate(&ev, 0), "cuEventCreate");

    // 在 stream 上做 memset + record
    CHECK_CU(cuMemsetD8Async(dptr, 0x5A, N, stream), "cuMemsetD8Async");
    CHECK_CU(cuEventRecord(ev, stream), "cuEventRecord");
    CHECK_CU(cuEventSynchronize(ev), "cuEventSynchronize");
    CHECK_CU(cuStreamSynchronize(stream), "cuStreamSynchronize");

    // 校验 memset 生效
    std::vector<unsigned char> mset(N);
    CHECK_CU(cuMemcpyDtoH_v2(mset.data(), dptr, N), "cuMemcpyDtoH(memset校验)");
    bool ms_ok = true;
    for (size_t i = 0; i < N; i++) if (mset[i] != 0x5A) { ms_ok = false; break; }
    printf("  [%s] cuMemsetD8Async(0x5A) 生效\n", ms_ok ? "PASS" : "FAIL");
    ms_ok ? g_pass++ : g_fail++;

    CHECK_CU(cuEventDestroy_v2(ev), "cuEventDestroy");
    CHECK_CU(cuStreamDestroy_v2(stream), "cuStreamDestroy");

    // ---- primary context ----
    // 注意：CUDA 真实签名是 cuDevicePrimaryCtxGetState(CUdevice dev, ...)，
    // 第一个参数按值传，不是指针。
    CUcontext primCtx = nullptr;
    CHECK_CU(cuDevicePrimaryCtxRetain(&primCtx, dev), "cuDevicePrimaryCtxRetain");
    unsigned int flags = 0; int active = 0;
    CHECK_CU(cuDevicePrimaryCtxGetState(dev, &flags, &active), "cuDevicePrimaryCtxGetState");
    CHECK_CU(cuDevicePrimaryCtxRelease(dev), "cuDevicePrimaryCtxRelease");

    // ---- 清理 ----
    CHECK_CU(cuMemFree_v2(dptr), "cuMemFree");
    CHECK_CU(cuCtxDestroy_v2(ctx), "cuCtxDestroy");

    printf("\n========== 汇总 ==========\n");
    printf("  PASS: %d   FAIL: %d\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("  >>> libcuda.so.1 功能真实可用（数据校验通过）\n");
    } else {
        printf("  >>> 存在失败项，需修复\n");
    }
    return g_fail == 0 ? 0 : 1;
}
