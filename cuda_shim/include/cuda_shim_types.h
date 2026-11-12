// cuda_shim/include/cuda_shim_types.h
// CUDA driver API 类型 → HIP 类型的映射与 shim 内部类型定义。
//
// 关键设计：
//  1. CUDA 的类型（CUdevice/CUcontext/...）定义在 <ffnvcodec/dynlink_cuda.h>，这是
//     nv-codec-headers 里的 MIT 头文件，可以直接 #include。
//  2. HIP 的类型定义在 <hip/hip_runtime_api.h>，需要 -D__HIP_PLATFORM_AMD__。
//  3. 两者可以在同一编译单元共存（已实测），但 shim 内部要统一：
//     - CUDA 的 CUdevice == int，HIP 的 hipDevice_t == int，直接相等。
//     - CUcontext/hipCtx_t 都是不透明指针，内部实现一致，可以强转。
//     - CUdeviceptr 是 unsigned long long，hipDeviceptr_t 是 void*，需转换。
//
// 错误码是最坑的一块：CUDA 和 HIP 的数值体系完全不同，必须显式映射，不能直接 cast。

#ifndef CUDA_SHIM_TYPES_H
#define CUDA_SHIM_TYPES_H

#include <ffnvcodec/dynlink_cuda.h>  // CUDA driver API 类型 + CUresult 枚举
#include <hip/hip_runtime_api.h>      // HIP 运行时 + hipError_t

// ---------------------------------------------------------------------------
// CUDA 错误码常量补充（dynlink_cuda.h 只定义了 5 个，这里补全 shim 需要的）
// ---------------------------------------------------------------------------
// 注意：dynlink_cuda.h 里 CUresult 是 cudaError_enum 枚举类型（不是 int），
// 补充常量必须 static_cast 成 CUresult 才能被 switch/return 接受。
#ifndef CUDA_ERROR_OUT_OF_MEMORY
#define CUDA_ERROR_OUT_OF_MEMORY        static_cast<CUresult>(2)
#endif
#ifndef CUDA_ERROR_NOT_INITIALIZED
#define CUDA_ERROR_NOT_INITIALIZED      static_cast<CUresult>(3)
#endif
#ifndef CUDA_ERROR_NO_DEVICE
#define CUDA_ERROR_NO_DEVICE            static_cast<CUresult>(100)
#endif
#ifndef CUDA_ERROR_INVALID_DEVICE
#define CUDA_ERROR_INVALID_DEVICE       static_cast<CUresult>(101)
#endif
#ifndef CUDA_ERROR_INVALID_CONTEXT
#define CUDA_ERROR_INVALID_CONTEXT      static_cast<CUresult>(201)
#endif
#ifndef CUDA_ERROR_NOT_SUPPORTED
#define CUDA_ERROR_NOT_SUPPORTED        static_cast<CUresult>(801)
#endif
#ifndef CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES
#define CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES static_cast<CUresult>(701)
#endif

// ---------------------------------------------------------------------------
// CUDA 错误码 → HIP 错误码 映射（显式，不用数值巧合）
// ---------------------------------------------------------------------------
static inline hipError_t cuda_err_to_hip(CUresult r) {
    switch (r) {
    case CUDA_SUCCESS:                     return hipSuccess;
    case CUDA_ERROR_INVALID_VALUE:         return hipErrorInvalidValue;
    case CUDA_ERROR_NOT_READY:             return hipErrorNotReady;
    case CUDA_ERROR_LAUNCH_TIMEOUT:        return hipErrorLaunchTimeOut;
    case CUDA_ERROR_UNKNOWN:               return hipErrorUnknown;
    // CUDA 完整错误码体系见 dynlink_cuda.h 只定义了 5 个；实际 CUDA 有上百个。
    // 这里对 shim 用不到的错误码统一映射到 hipErrorUnknown，后续按需补。
    default:                               return hipErrorUnknown;
    }
}

// HIP 错误码 → CUDA 错误码 映射（返回给调用者的方向）
static inline CUresult hip_err_to_cuda(hipError_t e) {
    switch (e) {
    case hipSuccess:                       return CUDA_SUCCESS;
    case hipErrorInvalidValue:             return CUDA_ERROR_INVALID_VALUE;
    case hipErrorOutOfMemory:              return CUDA_ERROR_OUT_OF_MEMORY;
    case hipErrorNotInitialized:           return CUDA_ERROR_NOT_INITIALIZED;
    case hipErrorNoDevice:                 return CUDA_ERROR_NO_DEVICE;
    case hipErrorInvalidDevice:            return CUDA_ERROR_INVALID_DEVICE;
    case hipErrorInvalidContext:           return CUDA_ERROR_INVALID_CONTEXT;
    case hipErrorNotSupported:             return CUDA_ERROR_NOT_SUPPORTED;
    case hipErrorNotReady:                 return CUDA_ERROR_NOT_READY;
    case hipErrorLaunchOutOfResources:     return CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES;
    default:                               return CUDA_ERROR_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// 类型强转工具（CUDA 不透明指针 ↔ HIP 不透明指针）
// ---------------------------------------------------------------------------
// CUcontext(CUctx_st*) 和 hipCtx_t(ihipCtx_t*) 都是驱动内部 context 句柄。
// ROCm 内部实现里两者指向同一对象，但这是实现细节；安全做法是显式 cast。
static inline hipCtx_t cu_ctx_to_hip(CUcontext c)   { return reinterpret_cast<hipCtx_t>(c); }
static inline CUcontext hip_ctx_to_cu(hipCtx_t h)   { return reinterpret_cast<CUcontext>(h); }

// CUdeviceptr(unsigned long long) ↔ hipDeviceptr_t(void*)
static inline hipDeviceptr_t cu_ptr_to_hip(CUdeviceptr p) {
    return reinterpret_cast<hipDeviceptr_t>(static_cast<uintptr_t>(p));
}
static inline CUdeviceptr hip_ptr_to_cu(hipDeviceptr_t p) {
    return static_cast<CUdeviceptr>(reinterpret_cast<uintptr_t>(p));
}

// CUdevice(int) == hipDevice_t(int)，直接相等，无需转换函数。

#endif // CUDA_SHIM_TYPES_H
