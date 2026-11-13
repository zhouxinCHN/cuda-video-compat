// HIP implementation behind libnppig. Compiled by hipcc, no NPP/CUDA headers.

#include "npp_gpu_bridge.h"

#include <hip/hip_runtime.h>

#include <cmath>

namespace {

__device__ __forceinline__ int clamp_index(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

__device__ __forceinline__ double cubic_weight(double distance) {
    // Catmull-Rom cubic convolution (a = -0.5). Exact NPP coefficient and
    // rounding are pending NVIDIA golden comparison.
    const double x = fabs(distance);
    if (x <= 1.0) return (1.5 * x - 2.5) * x * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

__device__ __forceinline__ double source_coord(int dst_index, int dst_origin,
                                                int src_origin, double factor,
                                                double shift) {
    return src_origin +
           (static_cast<double>(dst_index - dst_origin) + 0.5) / factor -
           0.5 + shift;
}

__device__ __forceinline__ std::uint8_t sample_pixel(const std::uint8_t *src,
                                                      int step, int x, int y) {
    return src[static_cast<long long>(y) * step + x];
}

template <int Mode> // 1=NN, 2=linear, 4=cubic (NPP interpolation values)
__global__ void resize_kernel(npp_gpu::ResizeJob job) {
    const int dx = blockIdx.x * blockDim.x + threadIdx.x;
    const int dy = blockIdx.y * blockDim.y + threadIdx.y;
    if (dx >= job.dst_width || dy >= job.dst_height) return;

    const double sx = source_coord(job.dst_x + dx, job.dst_x, job.src_x,
                                   job.x_factor, job.x_shift);
    const double sy = source_coord(job.dst_y + dy, job.dst_y, job.src_y,
                                   job.y_factor, job.y_shift);
    const int src_x1 = job.src_x + job.src_width - 1;
    const int src_y1 = job.src_y + job.src_height - 1;
    const double clamped_sx = fmin(fmax(sx, static_cast<double>(job.src_x)),
                                   static_cast<double>(src_x1));
    const double clamped_sy = fmin(fmax(sy, static_cast<double>(job.src_y)),
                                   static_cast<double>(src_y1));

    double value = 0.0;
    if (Mode == 1) {
        const int x = clamp_index(static_cast<int>(floor(clamped_sx + 0.5)),
                                  job.src_x, src_x1);
        const int y = clamp_index(static_cast<int>(floor(clamped_sy + 0.5)),
                                  job.src_y, src_y1);
        value = sample_pixel(job.src, job.src_step, x, y);
    } else if (Mode == 2) {
        const int x0 = static_cast<int>(floor(clamped_sx));
        const int y0 = static_cast<int>(floor(clamped_sy));
        const int x1 = clamp_index(x0 + 1, job.src_x, src_x1);
        const int y1 = clamp_index(y0 + 1, job.src_y, src_y1);
        const int ix0 = clamp_index(x0, job.src_x, src_x1);
        const int iy0 = clamp_index(y0, job.src_y, src_y1);
        const double fx = clamped_sx - x0;
        const double fy = clamped_sy - y0;
        const double top =
            sample_pixel(job.src, job.src_step, ix0, iy0) * (1.0 - fx) +
            sample_pixel(job.src, job.src_step, x1, iy0) * fx;
        const double bottom =
            sample_pixel(job.src, job.src_step, ix0, y1) * (1.0 - fx) +
            sample_pixel(job.src, job.src_step, x1, y1) * fx;
        value = top * (1.0 - fy) + bottom * fy;
    } else {
        const int x0 = static_cast<int>(floor(clamped_sx));
        const int y0 = static_cast<int>(floor(clamped_sy));
        for (int j = -1; j <= 2; ++j) {
            const int iy = clamp_index(y0 + j, job.src_y, src_y1);
            const double wy = cubic_weight(clamped_sy - static_cast<double>(y0 + j));
            for (int i = -1; i <= 2; ++i) {
                const int ix = clamp_index(x0 + i, job.src_x, src_x1);
                const double wx = cubic_weight(clamped_sx - static_cast<double>(x0 + i));
                value += sample_pixel(job.src, job.src_step, ix, iy) * wx * wy;
            }
        }
    }

    value = fmin(255.0, fmax(0.0, floor(value + 0.5)));
    const long long output =
        static_cast<long long>(job.dst_y + dy) * job.dst_step + job.dst_x + dx;
    job.dst[output] = static_cast<std::uint8_t>(value);
}

int finish() {
    hipError_t error = hipGetLastError();
    if (error == hipSuccess) error = hipDeviceSynchronize();
    if (error == hipSuccess) return npp_gpu::OK;
    if (error == hipErrorInvalidValue) return npp_gpu::InvalidArgument;
    return npp_gpu::RuntimeError;
}

int prepare() {
    // FFmpeg's input frame may have been produced on a non-default CUDA/HIP
    // stream. Stage A is a host-sync baseline, so synchronize before reading.
    const hipError_t error = hipDeviceSynchronize();
    return error == hipSuccess ? npp_gpu::OK : npp_gpu::RuntimeError;
}

} // namespace

namespace npp_gpu {

int launch_resize(const ResizeJob &job) {
    if (!job.src || !job.dst || job.dst_width <= 0 || job.dst_height <= 0)
        return InvalidArgument;
    int status = prepare();
    if (status != OK) return status;
    const dim3 block(16, 16);
    const dim3 grid((job.dst_width + block.x - 1) / block.x,
                    (job.dst_height + block.y - 1) / block.y);
    switch (job.interpolation) {
    case 1:
        resize_kernel<1><<<grid, block>>>(job);
        break;
    case 2:
        resize_kernel<2><<<grid, block>>>(job);
        break;
    case 4:
        resize_kernel<4><<<grid, block>>>(job);
        break;
    default:
        return InvalidArgument;
    }
    return finish();
}

} // namespace npp_gpu
