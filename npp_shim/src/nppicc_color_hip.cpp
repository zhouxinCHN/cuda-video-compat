// HIP implementation behind libnppicc. Compiled by hipcc, no NPP/CUDA headers.

#include "npp_gpu_bridge.h"

#include <hip/hip_runtime.h>

namespace {

__global__ void copy_y_kernel(const std::uint8_t *src, int src_step,
                              std::uint8_t *dst, int dst_step,
                              int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    dst[static_cast<long long>(y) * dst_step + x] =
        src[static_cast<long long>(y) * src_step + x];
}

__global__ void nv12_to_planar_uv_kernel(const std::uint8_t *src_uv,
                                         int src_step,
                                         std::uint8_t *dst_u, int dst_u_step,
                                         std::uint8_t *dst_v, int dst_v_step,
                                         int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const long long src_offset = static_cast<long long>(y) * src_step + 2 * x;
    dst_u[static_cast<long long>(y) * dst_u_step + x] = src_uv[src_offset];
    dst_v[static_cast<long long>(y) * dst_v_step + x] = src_uv[src_offset + 1];
}

__global__ void planar_to_nv12_uv_kernel(const std::uint8_t *src_u,
                                         int src_u_step,
                                         const std::uint8_t *src_v,
                                         int src_v_step,
                                         std::uint8_t *dst_uv, int dst_step,
                                         int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const long long dst_offset = static_cast<long long>(y) * dst_step + 2 * x;
    dst_uv[dst_offset] = src_u[static_cast<long long>(y) * src_u_step + x];
    dst_uv[dst_offset + 1] =
        src_v[static_cast<long long>(y) * src_v_step + x];
}

dim3 grid_for(int width, int height) {
    const dim3 block(16, 16);
    return dim3((width + block.x - 1) / block.x,
                (height + block.y - 1) / block.y);
}

int finish() {
    hipError_t error = hipGetLastError();
    if (error == hipSuccess) error = hipDeviceSynchronize();
    if (error == hipSuccess) return npp_gpu::OK;
    if (error == hipErrorInvalidValue) return npp_gpu::InvalidArgument;
    return npp_gpu::RuntimeError;
}

int prepare() {
    // FFmpeg reaches NPP after cuMemcpy2DAsync on its own CUDA/HIP stream.
    // Stage A uses a host-sync baseline, so make prior cross-stream work
    // visible before the default-stream NPP kernel reads its input.
    const hipError_t error = hipDeviceSynchronize();
    return error == hipSuccess ? npp_gpu::OK : npp_gpu::RuntimeError;
}

} // namespace

namespace npp_gpu {

int launch_copy_y(const std::uint8_t *src, int src_step,
                  std::uint8_t *dst, int dst_step,
                  int width, int height) {
    if (!src || !dst || width <= 0 || height <= 0) return InvalidArgument;
    int status = prepare();
    if (status != OK) return status;
    copy_y_kernel<<<grid_for(width, height), dim3(16, 16)>>>(
        src, src_step, dst, dst_step, width, height);
    return finish();
}

int launch_p2p3_uv(const std::uint8_t *src_uv, int src_uv_step,
                   std::uint8_t *dst_u, int dst_u_step,
                   std::uint8_t *dst_v, int dst_v_step,
                   int width, int height) {
    if (!src_uv || !dst_u || !dst_v ||
        width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        return InvalidArgument;
    }
    int status = prepare();
    if (status != OK) return status;
    nv12_to_planar_uv_kernel<<<grid_for(width / 2, height / 2), dim3(16, 16)>>>(
        src_uv, src_uv_step, dst_u, dst_u_step, dst_v, dst_v_step,
        width / 2, height / 2);
    return finish();
}

int launch_p3p2(const std::uint8_t *src_u, int src_u_step,
                const std::uint8_t *src_v, int src_v_step,
                std::uint8_t *dst_uv, int dst_uv_step,
                int width, int height) {
    if (!src_u || !src_v || !dst_uv ||
        width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        return InvalidArgument;
    }
    int status = prepare();
    if (status != OK) return status;
    planar_to_nv12_uv_kernel<<<grid_for(width / 2, height / 2), dim3(16, 16)>>>(
        src_u, src_u_step, src_v, src_v_step, dst_uv, dst_uv_step,
        width / 2, height / 2);
    return finish();
}

} // namespace npp_gpu
