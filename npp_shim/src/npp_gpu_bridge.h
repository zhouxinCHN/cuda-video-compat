#ifndef CUDA_SHIM_NPP_GPU_BRIDGE_H
#define CUDA_SHIM_NPP_GPU_BRIDGE_H

// Plain C++ boundary between the NPP ABI layer and HIP device code.
//
// NVIDIA's NPP headers include CUDA runtime/vector types; HIP headers define
// the same type names differently. They cannot share a translation unit. The
// host NPP files therefore include only CUDA/NPP headers and call these plain
// POD-based bridge functions, whose implementations are compiled by hipcc.

#include <cstdint>

namespace npp_gpu {

enum Status {
    OK = 0,
    InvalidArgument = 1,
    RuntimeError = 2,
};

struct ResizeJob {
    const std::uint8_t *src;
    int src_step;
    int src_x;
    int src_y;
    int src_width;
    int src_height;
    std::uint8_t *dst;
    int dst_step;
    int dst_x;
    int dst_y;
    int dst_width;
    int dst_height;
    double x_factor;
    double y_factor;
    double x_shift;
    double y_shift;
    int interpolation;
};

int launch_resize(const ResizeJob &job);

int launch_p2p3_uv(const std::uint8_t *src_uv, int src_uv_step,
                   std::uint8_t *dst_u, int dst_u_step,
                   std::uint8_t *dst_v, int dst_v_step,
                   int width, int height);

int launch_p3p2(const std::uint8_t *src_u, int src_u_step,
                const std::uint8_t *src_v, int src_v_step,
                std::uint8_t *dst_uv, int dst_uv_step,
                int width, int height);

int launch_copy_y(const std::uint8_t *src, int src_step,
                  std::uint8_t *dst, int dst_step,
                  int width, int height);

} // namespace npp_gpu

#endif // CUDA_SHIM_NPP_GPU_BRIDGE_H
