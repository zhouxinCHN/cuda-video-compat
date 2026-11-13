#include "npp_probe_gpu.h"

#include <cuda_runtime_api.h>

int probe_gpu_malloc(void **pointer, std::size_t size) {
    return static_cast<int>(cudaMalloc(pointer, size));
}

int probe_gpu_free(void *pointer) {
    return static_cast<int>(cudaFree(pointer));
}

int probe_gpu_copy_h2d(void *destination, const void *source, std::size_t size) {
    return static_cast<int>(cudaMemcpy(destination, source, size,
                                       cudaMemcpyHostToDevice));
}

int probe_gpu_copy_d2h(void *destination, const void *source, std::size_t size) {
    return static_cast<int>(cudaMemcpy(destination, source, size,
                                       cudaMemcpyDeviceToHost));
}

int probe_gpu_sync(void) {
    return static_cast<int>(cudaDeviceSynchronize());
}
