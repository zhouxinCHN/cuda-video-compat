// HIP implementation behind libnppig. Compiled by hipcc, no NPP/CUDA headers.
//
// NPPI_INTER_CUBIC was reverse-engineered from the NPP 11.5 golden output and
// NPP 12 PTX. It is not the standard Catmull-Rom kernel: NPP uses a 4-tap
// "fast cubic" weight polynomial. The weights are reproduced below in float32
// with the same operation order to preserve byte-exact results.

#include "npp_gpu_bridge.h"

#include <hip/hip_runtime.h>

#include <cmath>

namespace {

__device__ __forceinline__ int clamp_index(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

__device__ __forceinline__ void npp_fast_cubic_weights(float u, float w[4]) {
    // Equivalent to the operation order in NPP's SampleFastCubicImage kernel.
    const float t = u + 1.0f;
    const float a = t - 1.0f; // u
    const float b = t - 2.0f; // u - 1
    const float c = t - 3.0f; // u - 2
    w[0] = -((c / -3.0f) * (a * (b * -0.5f)));
    w[1] = -((t * b) * (c * -0.5f));
    w[2] = -(c * ((t * 0.5f) * a));
    w[3] = b * ((t / 3.0f) * (a * 0.5f));
}

__device__ __forceinline__ std::uint8_t round_npp(float value) {
    value = fminf(255.0f, fmaxf(0.0f, value));
    // NPP adds a signed 0.5 with round-toward-zero and truncates.
    return static_cast<std::uint8_t>(
        static_cast<unsigned int>(truncf(value + copysignf(0.5f, value))));
}

template <int Mode> // 1=NN, 2=linear, 4=cubic (NPP interpolation values)
__global__ void resize_kernel(npp_gpu::ResizeJob job) {
    const int dx = blockIdx.x * blockDim.x + threadIdx.x;
    const int dy = blockIdx.y * blockDim.y + threadIdx.y;
    if (dx >= job.dst_width || dy >= job.dst_height) return;

    const float sx = fmaf(static_cast<float>(job.dst_x + dx),
                          job.x_scale, job.x_offset);
    const float sy = fmaf(static_cast<float>(job.dst_y + dy),
                          job.y_scale, job.y_offset);
    const int src_x1 = job.src_x + job.src_width - 1;
    const int src_y1 = job.src_y + job.src_height - 1;

    // NPP writes a destination pixel only if its mapped coordinate is inside
    // the source ROI's half-pixel footprint; other pixels are left untouched.
    if (sx < static_cast<float>(job.src_x) - 0.5f ||
        sx > static_cast<float>(src_x1) + 0.5f ||
        sy < static_cast<float>(job.src_y) - 0.5f ||
        sy > static_cast<float>(src_y1) + 0.5f) {
        return;
    }

    const int full_x1 = job.src_full_width - 1;
    const int full_y1 = job.src_full_height - 1;
    const int bx = static_cast<int>(floorf(sx));
    const int by = static_cast<int>(floorf(sy));
    const float fx = sx - static_cast<float>(bx);
    const float fy = sy - static_cast<float>(by);
    float value = 0.0f;

    if (Mode == 1) {
        const int x = clamp_index(static_cast<int>(floorf(sx + 0.5f)), 0, full_x1);
        const int y = clamp_index(static_cast<int>(floorf(sy + 0.5f)), 0, full_y1);
        value = job.src[static_cast<long long>(y) * job.src_step + x];
    } else if (Mode == 2) {
        const int x0 = clamp_index(bx, 0, full_x1);
        const int x1 = clamp_index(bx + 1, 0, full_x1);
        const int y0 = clamp_index(by, 0, full_y1);
        const int y1 = clamp_index(by + 1, 0, full_y1);
        const float wx0 = 1.0f - fx;
        const float top =
            fmaf(wx0, job.src[static_cast<long long>(y0) * job.src_step + x0],
                 fx * job.src[static_cast<long long>(y0) * job.src_step + x1]);
        const float bottom =
            fmaf(wx0, job.src[static_cast<long long>(y1) * job.src_step + x0],
                 fx * job.src[static_cast<long long>(y1) * job.src_step + x1]);
        value = fmaf(1.0f - fy, top, fy * bottom);
    } else {
        float wx[4];
        float wy[4];
        npp_fast_cubic_weights(fx, wx);
        npp_fast_cubic_weights(fy, wy);
        int xi[4];
        int yi[4];
        for (int i = 0; i < 4; ++i) {
            xi[i] = clamp_index(bx - 1 + i, 0, full_x1);
            yi[i] = clamp_index(by - 1 + i, 0, full_y1);
        }

        float rows[4];
        for (int j = 0; j < 4; ++j) {
            const std::uint8_t *row =
                job.src + static_cast<long long>(yi[j]) * job.src_step;
            float h = wx[0] * row[xi[0]];
            h = fmaf(wx[1], row[xi[1]], h);
            h = fmaf(wx[2], row[xi[2]], h);
            h = fmaf(wx[3], row[xi[3]], h);
            rows[j] = h;
        }
        value = wy[0] * rows[0];
        value = fmaf(wy[1], rows[1], value);
        value = fmaf(wy[2], rows[2], value);
        value = fmaf(wy[3], rows[3], value);
    }

    const long long output =
        static_cast<long long>(job.dst_y + dy) * job.dst_step + job.dst_x + dx;
    job.dst[output] = round_npp(value);
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
