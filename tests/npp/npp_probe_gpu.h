#ifndef CUDA_SHIM_NPP_PROBE_GPU_H
#define CUDA_SHIM_NPP_PROBE_GPU_H

#include <cstddef>

// Plain GPU-runtime boundary for the NPP probe. NPP/CUDA headers and HIP
// headers define conflicting vector types and cannot share a translation unit;
// the probe host includes only NPP headers and calls these wrappers.

int probe_gpu_malloc(void **pointer, std::size_t size);
int probe_gpu_free(void *pointer);
int probe_gpu_copy_h2d(void *destination, const void *source, std::size_t size);
int probe_gpu_copy_d2h(void *destination, const void *source, std::size_t size);
int probe_gpu_sync(void);

#endif // CUDA_SHIM_NPP_PROBE_GPU_H
