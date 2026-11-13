#ifndef CUDA_SHIM_NPP_COMMON_H
#define CUDA_SHIM_NPP_COMMON_H

// Host-side helpers for the NPP compatibility libraries.
//
// This header must NOT include HIP headers. NVIDIA NPP headers include CUDA
// runtime/vector types, which conflict with HIP's same-named vector types in a
// single translation unit. HIP code lives behind npp_gpu_bridge.h and is
// compiled separately.

#include <nppi.h>

#include "npp_gpu_bridge.h"

#include <cmath>

namespace npp_shim {

inline bool positive_finite(double value) {
    return std::isfinite(value) && value > 0.0;
}

inline NppStatus status_from_gpu(int status) {
    if (status == npp_gpu::OK) return NPP_SUCCESS;
    if (status == npp_gpu::InvalidArgument) return NPP_BAD_ARGUMENT_ERROR;
    return NPP_CUDA_KERNEL_EXECUTION_ERROR;
}

inline bool valid_rect(const NppiRect &rect) {
    return rect.width > 0 && rect.height > 0 && rect.x >= 0 && rect.y >= 0;
}

inline bool valid_size(const NppiSize &size) {
    return size.width > 0 && size.height > 0;
}

} // namespace npp_shim

#endif // CUDA_SHIM_NPP_COMMON_H
