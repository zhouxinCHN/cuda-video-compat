// HIP side of verify_resolution_change: read one NV12 plane from the mapped
// CUVID picture. Kept in its own translation unit because ffnvcodec and HIP
// headers define conflicting types.

#include <hip/hip_runtime_api.h>

#include <cstddef>

extern "C" int shim_read_plane(const void *device, void *host, std::size_t pitch,
                               int width, int height) {
    const auto *src = static_cast<const char *>(device);
    auto *dst = static_cast<char *>(host);
    for (int row = 0; row < height; ++row) {
        if (hipMemcpy(dst + static_cast<std::size_t>(row) * width,
                      src + static_cast<std::size_t>(row) * pitch,
                      static_cast<std::size_t>(width),
                      hipMemcpyDeviceToHost) != hipSuccess) {
            return -1;
        }
    }
    return 0;
}
