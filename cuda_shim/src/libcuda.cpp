// cuda_shim/src/libcuda.cpp
// libcuda.so.1 —— CUDA driver API → HIP 运行时的 shim。
//
// 目标：让 FFmpeg（走 dynlink_loader.h 的 dlopen+dlsym）能加载本库并拿到
//       cuda_load_functions() 里 LOAD_SYMBOL（必需）要求的 82 个符号。
//
// 实现策略：
//   - 设备/上下文/内存/流/事件  → 直接映射到 HIP 对应函数。
//   - launch/module/JIT/texture/GL interop → stub，返回 CUDA_ERROR_NOT_SUPPORTED。
//     （视频解码路径用不到这些；后续 NPP kernel 库需要时再接 HIP。）
//
// 注意：
//   - 所有导出符号必须是 extern "C" + 默认可见性，且名字精确匹配 dynlink_loader.h
//     里的 dlsym 字符串（含 _v2 后缀）。
//   - 错误码用 cuda_shim_types.h 的显式映射，不用数值巧合。

#include "cuda_shim_types.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

// 仅用于验证“零拷贝”声明：CUDA_SHIM_TRACE_COPY=1 时统计 D2D 拷贝次数，
// 进程退出时打印一行汇总。默认关闭，正常路径无额外输出。
// 只看 DEVICE→DEVICE，避免把 FFmpeg hwdownload 的 DEVICE→HOST 拷贝算进去。
namespace {

std::atomic<unsigned long long> g_copy2d_d2d{0};
std::atomic<unsigned long long> g_memcpy_dtod{0};

bool copy_trace_enabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("CUDA_SHIM_TRACE_COPY");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

void print_copy_trace() {
    std::fprintf(stderr,
                 "[cuda-shim] copy-trace: cuMemcpy2D* D2D=%llu cuMemcpyDtoD*=%llu\n",
                 g_copy2d_d2d.load(std::memory_order_relaxed),
                 g_memcpy_dtod.load(std::memory_order_relaxed));
}

struct CopyTraceAtExit {
    CopyTraceAtExit() {
        if (copy_trace_enabled()) std::atexit(print_copy_trace);
    }
};

CopyTraceAtExit g_copy_trace_at_exit;

} // namespace

extern "C" {

// ===========================================================================
// 一、初始化
// ===========================================================================
CUresult cuInit(unsigned int Flags) {
    return hip_err_to_cuda(hipInit(Flags));
}

CUresult cuDriverGetVersion(int *driverVersion) {
    // 不能直接透传 hipDriverGetVersion（返回 ROCm 版本如 70253211），
    // 会误导按 CUDA 版本分支的应用/库。返回 CUDA 语义版本。
    if (!driverVersion) return CUDA_ERROR_INVALID_VALUE;
    *driverVersion = 12000;   // 声明为 CUDA 12.0 语义
    return CUDA_SUCCESS;
}

// ===========================================================================
// 二、设备枚举
// ===========================================================================
CUresult cuDeviceGetCount(int *count) {
    return hip_err_to_cuda(hipGetDeviceCount(count));
}

CUresult cuDeviceGet(CUdevice *device, int ordinal) {
    return hip_err_to_cuda(hipDeviceGet(device, ordinal));
}

CUresult cuDeviceGetAttribute(int *pi, CUdevice_attribute attrib, CUdevice dev) {
    // CUDA 的 CUdevice_attribute 与 HIP 的 hipDeviceAttribute_t 名称高度一致，
    // 但底层数值体系不同（CUDA 用固定数值，HIP 用枚举顺序）。必须逐项映射。
    hipDeviceAttribute_t hattr;
    switch (attrib) {
    case CU_DEVICE_ATTRIBUTE_CLOCK_RATE:             hattr = hipDeviceAttributeClockRate; break;
    case CU_DEVICE_ATTRIBUTE_TEXTURE_ALIGNMENT:      hattr = hipDeviceAttributeTextureAlignment; break;
    case CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT:   hattr = hipDeviceAttributeMultiprocessorCount; break;
    case CU_DEVICE_ATTRIBUTE_INTEGRATED:             hattr = hipDeviceAttributeIntegrated; break;
    case CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY:    hattr = hipDeviceAttributeCanMapHostMemory; break;
    case CU_DEVICE_ATTRIBUTE_COMPUTE_MODE:           hattr = hipDeviceAttributeComputeMode; break;
    case CU_DEVICE_ATTRIBUTE_CONCURRENT_KERNELS:     hattr = hipDeviceAttributeConcurrentKernels; break;
    case CU_DEVICE_ATTRIBUTE_PCI_BUS_ID:             hattr = hipDeviceAttributePciBusId; break;
    case CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID:          hattr = hipDeviceAttributePciDeviceId; break;
    case CU_DEVICE_ATTRIBUTE_TCC_DRIVER:             hattr = hipDeviceAttributeTccDriver; break;
    case CU_DEVICE_ATTRIBUTE_MEMORY_CLOCK_RATE:      hattr = hipDeviceAttributeMemoryClockRate; break;
    case CU_DEVICE_ATTRIBUTE_GLOBAL_MEMORY_BUS_WIDTH:hattr = hipDeviceAttributeMemoryBusWidth; break;
    case CU_DEVICE_ATTRIBUTE_ASYNC_ENGINE_COUNT:     hattr = hipDeviceAttributeAsyncEngineCount; break;
    case CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING:     hattr = hipDeviceAttributeUnifiedAddressing; break;
    case CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID:          hattr = hipDeviceAttributePciDomainId; break;
    case CU_DEVICE_ATTRIBUTE_TEXTURE_PITCH_ALIGNMENT:hattr = hipDeviceAttributeTexturePitchAlignment; break;
    case CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR:hattr = hipDeviceAttributeComputeCapabilityMajor; break;
    case CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR:hattr = hipDeviceAttributeComputeCapabilityMinor; break;
    case CU_DEVICE_ATTRIBUTE_MANAGED_MEMORY:         hattr = hipDeviceAttributeManagedMemory; break;
    case CU_DEVICE_ATTRIBUTE_MULTI_GPU_BOARD:        hattr = hipDeviceAttributeIsMultiGpuBoard; break;
    // MULTI_GPU_BOARD_GROUP_ID 无 HIP 对应，回 0。
    case CU_DEVICE_ATTRIBUTE_MULTI_GPU_BOARD_GROUP_ID: *pi = 0; return CUDA_SUCCESS;
    default:                                          return CUDA_ERROR_INVALID_VALUE;
    }
    return hip_err_to_cuda(hipDeviceGetAttribute(pi, hattr, dev));
}

CUresult cuDeviceGetName(char *name, int len, CUdevice dev) {
    return hip_err_to_cuda(hipDeviceGetName(name, len, dev));
}

CUresult cuDeviceGetUuid(CUuuid *uuid, CUdevice dev) {
    return hip_err_to_cuda(hipDeviceGetUuid(reinterpret_cast<hipUUID*>(uuid), dev));
}

CUresult cuDeviceGetUuid_v2(CUuuid *uuid, CUdevice dev) {
    return cuDeviceGetUuid(uuid, dev);
}

CUresult cuDeviceGetLuid(char *luid, unsigned int *deviceNodeMask, CUdevice dev) {
    (void)luid; (void)deviceNodeMask; (void)dev;
    return CUDA_ERROR_NOT_SUPPORTED;  // Linux 上无 LUID 概念
}

CUresult cuDeviceGetByPCIBusId(CUdevice *dev, const char *pciBusId) {
    return hip_err_to_cuda(hipDeviceGetByPCIBusId(reinterpret_cast<int*>(dev), pciBusId));
}

CUresult cuDeviceGetPCIBusId(char *pciBusId, int len, CUdevice dev) {
    return hip_err_to_cuda(hipDeviceGetPCIBusId(pciBusId, len, dev));
}

CUresult cuDeviceComputeCapability(int *major, int *minor, CUdevice dev) {
    return hip_err_to_cuda(hipDeviceComputeCapability(major, minor, dev));
}

// ===========================================================================
// 三、上下文管理
// ===========================================================================
CUresult cuCtxCreate_v2(CUcontext *pctx, unsigned int flags, CUdevice dev) {
    hipCtx_t hctx;
    hipError_t e = hipCtxCreate(&hctx, flags, dev);
    if (e == hipSuccess && pctx) *pctx = hip_ctx_to_cu(hctx);
    return hip_err_to_cuda(e);
}

CUresult cuCtxSynchronize(void) {
    // hipCtxSynchronize 在 ROCm 7.x 已废弃，返回 NotSupported。改用设备级同步。
    return hip_err_to_cuda(hipDeviceSynchronize());
}

CUresult cuCtxGetStreamPriorityRange(int *leastPriority, int *greatestPriority) {
    return hip_err_to_cuda(hipDeviceGetStreamPriorityRange(leastPriority, greatestPriority));
}

CUresult cuCtxGetCurrent(CUcontext *pctx) {
    hipCtx_t hctx;
    hipError_t e = hipCtxGetCurrent(&hctx);
    if (e == hipSuccess && pctx) *pctx = hip_ctx_to_cu(hctx);
    return hip_err_to_cuda(e);
}

CUresult cuCtxSetLimit(CUlimit limit, size_t value) {
    hipLimit_t hlim;
    switch (limit) {
    case CU_LIMIT_STACK_SIZE:           hlim = hipLimitStackSize; break;
    case CU_LIMIT_PRINTF_FIFO_SIZE:     hlim = hipLimitPrintfFifoSize; break;
    case CU_LIMIT_MALLOC_HEAP_SIZE:     hlim = hipLimitMallocHeapSize; break;
    // DEV_RUNTIME_* 无 HIP 对应
    case CU_LIMIT_DEV_RUNTIME_SYNC_DEPTH:
    case CU_LIMIT_DEV_RUNTIME_PENDING_LAUNCH_COUNT:
        return CUDA_ERROR_NOT_SUPPORTED;
    default: return CUDA_ERROR_INVALID_VALUE;
    }
    return hip_err_to_cuda(hipDeviceSetLimit(hlim, value));
}

CUresult cuCtxPushCurrent_v2(CUcontext pctx) {
    return hip_err_to_cuda(hipCtxPushCurrent(cu_ctx_to_hip(pctx)));
}

CUresult cuCtxPopCurrent_v2(CUcontext *pctx) {
    hipCtx_t hctx;
    hipError_t e = hipCtxPopCurrent(&hctx);
    if (e == hipSuccess && pctx) *pctx = hip_ctx_to_cu(hctx);
    return hip_err_to_cuda(e);
}

CUresult cuCtxDestroy_v2(CUcontext ctx) {
    return hip_err_to_cuda(hipCtxDestroy(cu_ctx_to_hip(ctx)));
}

CUresult cuCtxGetDevice(CUdevice *device) {
    return hip_err_to_cuda(hipCtxGetDevice(device));
}

CUresult cuDevicePrimaryCtxRetain(CUcontext *pctx, CUdevice dev) {
    hipCtx_t hctx;
    hipError_t e = hipDevicePrimaryCtxRetain(&hctx, dev);
    if (e == hipSuccess && pctx) *pctx = hip_ctx_to_cu(hctx);
    return hip_err_to_cuda(e);
}

CUresult cuDevicePrimaryCtxRelease(CUdevice dev) {
    return hip_err_to_cuda(hipDevicePrimaryCtxRelease(dev));
}

CUresult cuDevicePrimaryCtxSetFlags(CUdevice dev, unsigned int flags) {
    return hip_err_to_cuda(hipDevicePrimaryCtxSetFlags(dev, flags));
}

CUresult cuDevicePrimaryCtxGetState(CUdevice dev, unsigned int *flags, int *active) {
    return hip_err_to_cuda(hipDevicePrimaryCtxGetState(dev, flags, active));
}

CUresult cuDevicePrimaryCtxReset(CUdevice dev) {
    return hip_err_to_cuda(hipDevicePrimaryCtxReset(dev));
}

// ===========================================================================
// 四、内存管理
// ===========================================================================
CUresult cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize) {
    void *p = nullptr;
    hipError_t e = hipMalloc(&p, bytesize);
    if (e == hipSuccess && dptr) *dptr = hip_ptr_to_cu(p);
    return hip_err_to_cuda(e);
}

CUresult cuMemAllocPitch_v2(CUdeviceptr *dptr, size_t *pPitch,
                            size_t WidthInBytes, size_t Height, unsigned int ElementSizeBytes) {
    (void)ElementSizeBytes;  // HIP 的 hipMallocPitch 无此参数
    void *p = nullptr;
    hipError_t e = hipMallocPitch(&p, pPitch, WidthInBytes, Height);
    if (e == hipSuccess && dptr) *dptr = hip_ptr_to_cu(p);
    return hip_err_to_cuda(e);
}

CUresult cuMemAllocManaged(CUdeviceptr *dptr, size_t bytesize, unsigned int flags) {
    void *p = nullptr;
    hipError_t e = hipMallocManaged(&p, bytesize, flags);
    if (e == hipSuccess && dptr) *dptr = hip_ptr_to_cu(p);
    return hip_err_to_cuda(e);
}

CUresult cuMemFree_v2(CUdeviceptr dptr) {
    return hip_err_to_cuda(hipFree(cu_ptr_to_hip(dptr)));
}

CUresult cuMemHostAlloc(void **pp, size_t bytesize, unsigned int flags) {
    return hip_err_to_cuda(hipHostMalloc(pp, bytesize, flags));
}

CUresult cuMemFreeHost(void *p) {
    return hip_err_to_cuda(hipHostFree(p));
}

CUresult cuMemAllocAsync(CUdeviceptr *dptr, size_t bytesize, CUstream hStream) {
    void *p = nullptr;
    hipError_t e = hipMallocAsync(&p, bytesize, reinterpret_cast<hipStream_t>(hStream));
    if (e == hipSuccess && dptr) *dptr = hip_ptr_to_cu(p);
    return hip_err_to_cuda(e);
}

CUresult cuMemFreeAsync(CUdeviceptr dptr, CUstream hStream) {
    return hip_err_to_cuda(hipFreeAsync(cu_ptr_to_hip(dptr), reinterpret_cast<hipStream_t>(hStream)));
}

// ===========================================================================
// 五、Memset
// ===========================================================================
CUresult cuMemsetD8Async(CUdeviceptr dstDevice, unsigned char uc, size_t N, CUstream hStream) {
    return hip_err_to_cuda(hipMemsetD8Async(cu_ptr_to_hip(dstDevice), uc, N, reinterpret_cast<hipStream_t>(hStream)));
}
CUresult cuMemsetD16Async(CUdeviceptr dstDevice, unsigned short us, size_t N, CUstream hStream) {
    return hip_err_to_cuda(hipMemsetD16Async(cu_ptr_to_hip(dstDevice), us, N, reinterpret_cast<hipStream_t>(hStream)));
}
CUresult cuMemsetD32Async(CUdeviceptr dstDevice, unsigned int ui, size_t N, CUstream hStream) {
    return hip_err_to_cuda(hipMemsetD32Async(cu_ptr_to_hip(dstDevice), ui, N, reinterpret_cast<hipStream_t>(hStream)));
}
CUresult cuMemsetD2D8Async(CUdeviceptr dstDevice, size_t dstPitch, unsigned char uc, size_t Width, size_t Height, CUstream hStream) {
    return hip_err_to_cuda(hipMemsetD2D8Async(cu_ptr_to_hip(dstDevice), dstPitch, uc, Width, Height, reinterpret_cast<hipStream_t>(hStream)));
}
CUresult cuMemsetD2D16Async(CUdeviceptr dstDevice, size_t dstPitch, unsigned short us, size_t Width, size_t Height, CUstream hStream) {
    return hip_err_to_cuda(hipMemsetD2D16Async(cu_ptr_to_hip(dstDevice), dstPitch, us, Width, Height, reinterpret_cast<hipStream_t>(hStream)));
}
CUresult cuMemsetD2D32Async(CUdeviceptr dstDevice, size_t dstPitch, unsigned int ui, size_t Width, size_t Height, CUstream hStream) {
    return hip_err_to_cuda(hipMemsetD2D32Async(cu_ptr_to_hip(dstDevice), dstPitch, ui, Width, Height, reinterpret_cast<hipStream_t>(hStream)));
}

// ===========================================================================
// 六、内存拷贝
// ===========================================================================
CUresult cuMemcpy(CUdeviceptr dst, CUdeviceptr src, size_t bytesize) {
    // CUDA 的 cuMemcpy 是 device-to-device
    return hip_err_to_cuda(hipMemcpy(cu_ptr_to_hip(dst), cu_ptr_to_hip(src), bytesize, hipMemcpyDeviceToDevice));
}
CUresult cuMemcpyAsync(CUdeviceptr dst, CUdeviceptr src, size_t bytesize, CUstream hStream) {
    return hip_err_to_cuda(hipMemcpyAsync(cu_ptr_to_hip(dst), cu_ptr_to_hip(src), bytesize, hipMemcpyDeviceToDevice, reinterpret_cast<hipStream_t>(hStream)));
}

static hipError_t cuda_memcpy2d_to_hip(const CUDA_MEMCPY2D *pcopy, hip_Memcpy2D &hcopy) {
    hcopy = hip_Memcpy2D();
    // src
    hcopy.srcXInBytes   = pcopy->srcXInBytes;
    hcopy.srcY          = pcopy->srcY;
    hcopy.srcPitch      = pcopy->srcPitch;
    hcopy.srcHost       = pcopy->srcHost;
    hcopy.srcDevice     = cu_ptr_to_hip(pcopy->srcDevice);
    switch (pcopy->srcMemoryType) {
    case CU_MEMORYTYPE_HOST:   hcopy.srcMemoryType = hipMemoryTypeHost; break;
    case CU_MEMORYTYPE_DEVICE: hcopy.srcMemoryType = hipMemoryTypeDevice; break;
    case CU_MEMORYTYPE_ARRAY:  hcopy.srcArray = reinterpret_cast<hipArray_t>(pcopy->srcArray);
                               hcopy.srcMemoryType = hipMemoryTypeArray; break;
    default: return hipErrorInvalidValue;
    }
    // dst
    hcopy.dstXInBytes   = pcopy->dstXInBytes;
    hcopy.dstY          = pcopy->dstY;
    hcopy.dstPitch      = pcopy->dstPitch;
    hcopy.dstHost       = pcopy->dstHost;
    hcopy.dstDevice     = cu_ptr_to_hip(pcopy->dstDevice);
    switch (pcopy->dstMemoryType) {
    case CU_MEMORYTYPE_HOST:   hcopy.dstMemoryType = hipMemoryTypeHost; break;
    case CU_MEMORYTYPE_DEVICE: hcopy.dstMemoryType = hipMemoryTypeDevice; break;
    case CU_MEMORYTYPE_ARRAY:  hcopy.dstArray = reinterpret_cast<hipArray_t>(pcopy->dstArray);
                               hcopy.dstMemoryType = hipMemoryTypeArray; break;
    default: return hipErrorInvalidValue;
    }
    hcopy.WidthInBytes  = pcopy->WidthInBytes;
    hcopy.Height        = pcopy->Height;
    return hipSuccess;
}

CUresult cuMemcpy2D_v2(const CUDA_MEMCPY2D *pcopy) {
    if (copy_trace_enabled() && pcopy &&
        pcopy->srcMemoryType == CU_MEMORYTYPE_DEVICE &&
        pcopy->dstMemoryType == CU_MEMORYTYPE_DEVICE) {
        g_copy2d_d2d.fetch_add(1, std::memory_order_relaxed);
    }
    hip_Memcpy2D h;
    hipError_t e = cuda_memcpy2d_to_hip(pcopy, h);
    if (e != hipSuccess) return hip_err_to_cuda(e);
    return hip_err_to_cuda(hipMemcpyParam2D(&h));
}
CUresult cuMemcpy2DAsync_v2(const CUDA_MEMCPY2D *pcopy, CUstream hStream) {
    if (copy_trace_enabled() && pcopy &&
        pcopy->srcMemoryType == CU_MEMORYTYPE_DEVICE &&
        pcopy->dstMemoryType == CU_MEMORYTYPE_DEVICE) {
        g_copy2d_d2d.fetch_add(1, std::memory_order_relaxed);
    }
    hip_Memcpy2D h;
    hipError_t e = cuda_memcpy2d_to_hip(pcopy, h);
    if (e != hipSuccess) return hip_err_to_cuda(e);
    return hip_err_to_cuda(hipMemcpyParam2DAsync(&h, reinterpret_cast<hipStream_t>(hStream)));
}

CUresult cuMemcpyHtoD_v2(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount) {
    return hip_err_to_cuda(hipMemcpyHtoD(cu_ptr_to_hip(dstDevice), srcHost, ByteCount));
}
CUresult cuMemcpyHtoDAsync_v2(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount, CUstream hStream) {
    return hip_err_to_cuda(hipMemcpyHtoDAsync(cu_ptr_to_hip(dstDevice), srcHost, ByteCount, reinterpret_cast<hipStream_t>(hStream)));
}
CUresult cuMemcpyDtoH_v2(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount) {
    return hip_err_to_cuda(hipMemcpyDtoH(dstHost, cu_ptr_to_hip(srcDevice), ByteCount));
}
CUresult cuMemcpyDtoHAsync_v2(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount, CUstream hStream) {
    return hip_err_to_cuda(hipMemcpyDtoHAsync(dstHost, cu_ptr_to_hip(srcDevice), ByteCount, reinterpret_cast<hipStream_t>(hStream)));
}
CUresult cuMemcpyDtoD_v2(CUdeviceptr dstDevice, CUdeviceptr srcDevice, size_t ByteCount) {
    if (copy_trace_enabled()) g_memcpy_dtod.fetch_add(1, std::memory_order_relaxed);
    return hip_err_to_cuda(hipMemcpyDtoD(cu_ptr_to_hip(dstDevice), cu_ptr_to_hip(srcDevice), ByteCount));
}
CUresult cuMemcpyDtoDAsync_v2(CUdeviceptr dstDevice, CUdeviceptr srcDevice, size_t ByteCount, CUstream hStream) {
    if (copy_trace_enabled()) g_memcpy_dtod.fetch_add(1, std::memory_order_relaxed);
    return hip_err_to_cuda(hipMemcpyDtoDAsync(cu_ptr_to_hip(dstDevice), cu_ptr_to_hip(srcDevice), ByteCount, reinterpret_cast<hipStream_t>(hStream)));
}

// ===========================================================================
// 七、错误查询
// ===========================================================================
CUresult cuGetErrorName(CUresult error, const char **pstr) {
    // HIP 无对应，静态映射常见错误码名字
    static const struct { CUresult c; const char *n; } tbl[] = {
        {CUDA_SUCCESS, "CUDA_SUCCESS"},
        {CUDA_ERROR_INVALID_VALUE, "CUDA_ERROR_INVALID_VALUE"},
        {CUDA_ERROR_OUT_OF_MEMORY, "CUDA_ERROR_OUT_OF_MEMORY"},
        {CUDA_ERROR_NOT_INITIALIZED, "CUDA_ERROR_NOT_INITIALIZED"},
        {CUDA_ERROR_NO_DEVICE, "CUDA_ERROR_NO_DEVICE"},
        {CUDA_ERROR_INVALID_DEVICE, "CUDA_ERROR_INVALID_DEVICE"},
        {CUDA_ERROR_INVALID_CONTEXT, "CUDA_ERROR_INVALID_CONTEXT"},
        {CUDA_ERROR_NOT_SUPPORTED, "CUDA_ERROR_NOT_SUPPORTED"},
        {CUDA_ERROR_NOT_READY, "CUDA_ERROR_NOT_READY"},
        {CUDA_ERROR_UNKNOWN, "CUDA_ERROR_UNKNOWN"},
    };
    for (auto &e : tbl) if (e.c == error) { *pstr = e.n; return CUDA_SUCCESS; }
    *pstr = "CUDA_ERROR_UNKNOWN";
    return CUDA_ERROR_INVALID_VALUE;
}
CUresult cuGetErrorString(CUresult error, const char **pstr) {
    return cuGetErrorName(error, pstr);
}

// ===========================================================================
// 八、流与事件
// ===========================================================================
CUresult cuStreamCreate(CUstream *phStream, unsigned int flags) {
    return hip_err_to_cuda(hipStreamCreateWithFlags(reinterpret_cast<hipStream_t*>(phStream), flags));
}
CUresult cuStreamCreateWithPriority(CUstream *phStream, unsigned int flags, int priority) {
    return hip_err_to_cuda(hipStreamCreateWithPriority(reinterpret_cast<hipStream_t*>(phStream), flags, priority));
}
CUresult cuStreamQuery(CUstream hStream) {
    return hip_err_to_cuda(hipStreamQuery(reinterpret_cast<hipStream_t>(hStream)));
}
CUresult cuStreamSynchronize(CUstream hStream) {
    return hip_err_to_cuda(hipStreamSynchronize(reinterpret_cast<hipStream_t>(hStream)));
}
CUresult cuStreamDestroy_v2(CUstream hStream) {
    return hip_err_to_cuda(hipStreamDestroy(reinterpret_cast<hipStream_t>(hStream)));
}
CUresult cuStreamAddCallback(CUstream hStream, CUstreamCallback *callback, void *userdata, unsigned int flags) {
    (void)flags;
    return hip_err_to_cuda(hipStreamAddCallback(reinterpret_cast<hipStream_t>(hStream),
        reinterpret_cast<hipStreamCallback_t>(callback), userdata, 0));
}
CUresult cuStreamWaitEvent(CUstream hStream, CUevent hEvent, unsigned int flags) {
    (void)flags;
    return hip_err_to_cuda(hipStreamWaitEvent(reinterpret_cast<hipStream_t>(hStream),
        reinterpret_cast<hipEvent_t>(hEvent), 0));
}
CUresult cuEventCreate(CUevent *phEvent, unsigned int flags) {
    return hip_err_to_cuda(hipEventCreateWithFlags(reinterpret_cast<hipEvent_t*>(phEvent), flags));
}
CUresult cuEventDestroy_v2(CUevent hEvent) {
    return hip_err_to_cuda(hipEventDestroy(reinterpret_cast<hipEvent_t>(hEvent)));
}
CUresult cuEventSynchronize(CUevent hEvent) {
    return hip_err_to_cuda(hipEventSynchronize(reinterpret_cast<hipEvent_t>(hEvent)));
}
CUresult cuEventQuery(CUevent hEvent) {
    return hip_err_to_cuda(hipEventQuery(reinterpret_cast<hipEvent_t>(hEvent)));
}
CUresult cuEventRecord(CUevent hEvent, CUstream hStream) {
    return hip_err_to_cuda(hipEventRecord(reinterpret_cast<hipEvent_t>(hEvent), reinterpret_cast<hipStream_t>(hStream)));
}

// ===========================================================================
// 九、launch / module / JIT —— 视频路径用不到，stub
// ===========================================================================
CUresult cuLaunchKernel(CUfunction, unsigned int, unsigned int, unsigned int,
                        unsigned int, unsigned int, unsigned int, unsigned int,
                        CUstream, void **, void **) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuLaunchHostFunc(CUstream, CUhostFn *, void *) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuLinkCreate(unsigned int, CUjit_option *, void **, CUlinkState *) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuLinkAddData(CUlinkState, CUjitInputType, void *, size_t, const char *,
                       unsigned int, CUjit_option *, void **) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuLinkComplete(CUlinkState, void **, size_t *) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuLinkDestroy(CUlinkState) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuModuleLoadData(CUmodule *, const void *) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuModuleUnload(CUmodule) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuModuleGetFunction(CUfunction *, CUmodule, const char *) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuModuleGetGlobal(CUdeviceptr *, size_t *, CUmodule, const char *) {
    return CUDA_ERROR_NOT_SUPPORTED;
}

// ===========================================================================
// 十、texture / array —— stub
// ===========================================================================
CUresult cuTexObjectCreate(CUtexObject *, const CUDA_RESOURCE_DESC *, const CUDA_TEXTURE_DESC *, const CUDA_RESOURCE_VIEW_DESC *) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuTexObjectDestroy(CUtexObject) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuArrayCreate_v2(CUarray *, const CUDA_ARRAY_DESCRIPTOR *) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuArray3DCreate_v2(CUarray *, const CUDA_ARRAY3D_DESCRIPTOR *) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuArrayDestroy(CUarray) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuArray3DGetDescriptor_v2(CUDA_ARRAY3D_DESCRIPTOR *, CUarray) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuArrayGetPlane(CUarray *, CUarray, unsigned int) {
    return CUDA_ERROR_NOT_SUPPORTED;
}

// ===========================================================================
// 十一、GL interop —— stub
// ===========================================================================
CUresult cuGLGetDevices_v2(unsigned int *, CUdevice *, unsigned int, CUGLDeviceList) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuGraphicsGLRegisterImage(CUgraphicsResource *, GLuint, GLenum, unsigned int) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuGraphicsUnregisterResource(CUgraphicsResource) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuGraphicsMapResources(unsigned int, CUgraphicsResource *, CUstream) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuGraphicsUnmapResources(unsigned int, CUgraphicsResource *, CUstream) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuGraphicsSubResourceGetMappedArray(CUarray *, CUgraphicsResource, unsigned int, unsigned int) {
    return CUDA_ERROR_NOT_SUPPORTED;
}
CUresult cuGraphicsResourceGetMappedPointer_v2(CUdeviceptr *, size_t *, CUgraphicsResource) {
    return CUDA_ERROR_NOT_SUPPORTED;
}

} // extern "C"
