// HIP implementation of the libnppc device queries.
//
// Compiled by hipcc: this file must not include NVIDIA NPP headers, whose
// CUDA vector types conflict with HIP's.

#include "npp_gpu_bridge.h"

#include <hip/hip_runtime.h>

#include <cstring>

namespace npp_gpu {

int query_device(int device_id, DeviceInfo *info) {
    if (!info) return InvalidArgument;

    hipDeviceProp_t prop{};
    int device = device_id;
    if (device < 0) {
        if (hipGetDevice(&device) != hipSuccess) return RuntimeError;
    }
    if (hipGetDeviceProperties(&prop, device) != hipSuccess) return RuntimeError;

    info->device_id = device;
    info->multi_processor_count = prop.multiProcessorCount;
    info->max_threads_per_multi_processor = prop.maxThreadsPerMultiProcessor;
    info->max_threads_per_block = prop.maxThreadsPerBlock;
    info->shared_mem_per_block = prop.sharedMemPerBlock;
    // Same values cuda_shim reports for cuDeviceComputeCapability.
    info->compute_capability_major = prop.major;
    info->compute_capability_minor = prop.minor;
    std::memset(info->name, 0, sizeof(info->name));
    std::strncpy(info->name, prop.name, sizeof(info->name) - 1);
    return OK;
}

int device_synchronize() {
    return hipDeviceSynchronize() == hipSuccess ? OK : RuntimeError;
}

} // namespace npp_gpu
