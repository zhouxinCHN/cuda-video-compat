#include "npp_probe_gpu.h"

#include <hip/hip_runtime_api.h>

int probe_gpu_malloc(void **pointer, std::size_t size) {
    return static_cast<int>(hipMalloc(pointer, size));
}

int probe_gpu_free(void *pointer) {
    return static_cast<int>(hipFree(pointer));
}

int probe_gpu_copy_h2d(void *destination, const void *source, std::size_t size) {
    return static_cast<int>(hipMemcpy(destination, source, size,
                                      hipMemcpyHostToDevice));
}

int probe_gpu_copy_d2h(void *destination, const void *source, std::size_t size) {
    return static_cast<int>(hipMemcpy(destination, source, size,
                                      hipMemcpyDeviceToHost));
}

int probe_gpu_sync(void) {
    return static_cast<int>(hipDeviceSynchronize());
}
