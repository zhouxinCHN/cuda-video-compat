// Generate deterministic NVIDIA NPP reference output for the three FFmpeg P0 APIs.
// Build on the NVIDIA reference host; see the accompanying P0 instructions.

#include <cuda_runtime.h>
#include <nppcore.h>
#include <nppi_color_conversion.h>
#include <nppi_geometry_transforms.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool check_cuda(cudaError_t status, const char *operation) {
    if (status == cudaSuccess) return true;
    std::cerr << "CUDA error at " << operation << ": "
              << cudaGetErrorString(status) << "\n";
    return false;
}

bool check_npp(NppStatus status, const char *operation) {
    if (status == NPP_SUCCESS) return true;
    std::cerr << "NPP error at " << operation << ": " << status << "\n";
    return false;
}

template <typename T>
bool write_buffer(const std::filesystem::path &path, const T *data, size_t size) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(data), size);
    return output.good();
}

bool write_planes(const std::filesystem::path &path,
                  const std::vector<Npp8u *> &planes,
                  const std::vector<NppiSize> &sizes,
                  int step) {
    std::ofstream output(path, std::ios::binary);
    for (size_t plane = 0; plane < planes.size(); ++plane) {
        for (int row = 0; row < sizes[plane].height; ++row) {
            const char *begin = reinterpret_cast<const char *>(
                planes[plane] + static_cast<size_t>(row) * step);
            output.write(begin, sizes[plane].width);
        }
    }
    return output.good();
}

} // namespace

int main(int argc, char **argv) {
    const std::filesystem::path output_dir =
        argc > 1 ? argv[1] : "npp-reference-output";
    std::error_code error;
    std::filesystem::create_directories(output_dir, error);
    if (error) {
        std::cerr << "Cannot create output directory: " << error.message() << "\n";
        return 1;
    }

    int driver_version = 0;
    int runtime_version = 0;
    if (!check_cuda(cudaDriverGetVersion(&driver_version), "cudaDriverGetVersion") ||
        !check_cuda(cudaRuntimeGetVersion(&runtime_version), "cudaRuntimeGetVersion")) {
        return 1;
    }
    std::cout << "CUDA driver API version: " << driver_version << "\n"
              << "CUDA runtime version: " << runtime_version << "\n";

    constexpr int width = 8;
    constexpr int height = 8;
    constexpr int step = 8;
    constexpr int chroma_height = height / 2;
    constexpr size_t plane_bytes = static_cast<size_t>(step) * height;

    std::vector<Npp8u> y_plane(plane_bytes, 0);
    std::vector<Npp8u> uv_plane(plane_bytes, 0);
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            y_plane[static_cast<size_t>(row) * step + col] =
                static_cast<Npp8u>(row * 16 + col * 3);
        }
    }
    for (int row = 0; row < chroma_height; ++row) {
        for (int col = 0; col < width; col += 2) {
            const size_t offset = static_cast<size_t>(row) * step + col;
            uv_plane[offset] = static_cast<Npp8u>(32 + row * 17 + col * 5);
            uv_plane[offset + 1] = static_cast<Npp8u>(224 - row * 13 - col * 7);
        }
    }

    Npp8u *device_y = nullptr;
    Npp8u *device_uv = nullptr;
    Npp8u *device_dst[3] = {nullptr, nullptr, nullptr};
    Npp8u *device_roundtrip[2] = {nullptr, nullptr};
    Npp8u *device_resize = nullptr;
    Npp8u *host_dst[3] = {nullptr, nullptr, nullptr};
    Npp8u *host_roundtrip[2] = {nullptr, nullptr};
    Npp8u *host_resize = nullptr;
    int result = 1;

    auto cleanup = [&] {
        cudaFree(device_y);
        cudaFree(device_uv);
        for (auto *pointer : device_dst) cudaFree(pointer);
        for (auto *pointer : device_roundtrip) cudaFree(pointer);
        cudaFree(device_resize);
        for (auto *pointer : host_dst) cudaFreeHost(pointer);
        for (auto *pointer : host_roundtrip) cudaFreeHost(pointer);
        cudaFreeHost(host_resize);
    };

    if (!check_cuda(cudaMalloc(reinterpret_cast<void **>(&device_y), plane_bytes),
                    "cudaMalloc(device_y)") ||
        !check_cuda(cudaMalloc(reinterpret_cast<void **>(&device_uv), plane_bytes),
                    "cudaMalloc(device_uv)") ||
        !check_cuda(cudaMemcpy(device_y, y_plane.data(), plane_bytes, cudaMemcpyHostToDevice),
                    "cudaMemcpy(device_y)") ||
        !check_cuda(cudaMemcpy(device_uv, uv_plane.data(), plane_bytes, cudaMemcpyHostToDevice),
                    "cudaMemcpy(device_uv)")) {
        cleanup();
        return 1;
    }

    for (auto *&pointer : device_dst) {
        if (!check_cuda(cudaMalloc(reinterpret_cast<void **>(&pointer), plane_bytes),
                        "cudaMalloc(P2P3 output)")) {
            cleanup();
            return 1;
        }
    }
    for (auto *&pointer : device_roundtrip) {
        if (!check_cuda(cudaMalloc(reinterpret_cast<void **>(&pointer), plane_bytes),
                        "cudaMalloc(P3P2 output)")) {
            cleanup();
            return 1;
        }
    }

    const NppiSize roi{width, height};
    int p2p3_dst_steps[3] = {step, step, step};
    if (!check_npp(nppiYCbCr420_8u_P2P3R(
                       device_y, step, device_uv, step,
                       device_dst, p2p3_dst_steps, roi),
                   "nppiYCbCr420_8u_P2P3R")) {
        cleanup();
        return 1;
    }

    const Npp8u *p3_src[3] = {device_dst[0], device_dst[1], device_dst[2]};
    int p3p2_src_steps[3] = {step, step, step};
    if (!check_npp(nppiYCbCr420_8u_P3P2R(
                       p3_src, p3p2_src_steps,
                       device_roundtrip[0], step, device_roundtrip[1], step, roi),
                   "nppiYCbCr420_8u_P3P2R")) {
        cleanup();
        return 1;
    }

    if (!check_cuda(cudaMalloc(reinterpret_cast<void **>(&device_resize), 4 * 4),
                    "cudaMalloc(resize)")) {
        cleanup();
        return 1;
    }
    const NppiSize source_size{width, height};
    const NppiRect source_roi{0, 0, width, height};
    const NppiRect destination_roi{0, 0, 4, 4};
    if (!check_npp(nppiResizeSqrPixel_8u_C1R(
                       device_y, source_size, step, source_roi,
                       device_resize, 4, destination_roi,
                       0.5, 0.5, 0.0, 0.0, NPPI_INTER_NN),
                   "nppiResizeSqrPixel_8u_C1R")) {
        cleanup();
        return 1;
    }

    if (!check_cuda(cudaDeviceSynchronize(), "NPP synchronization")) {
        cleanup();
        return 1;
    }

    for (auto *&pointer : host_dst) {
        if (!check_cuda(cudaMallocHost(reinterpret_cast<void **>(&pointer), plane_bytes),
                        "cudaMallocHost(P2P3 output)")) {
            cleanup();
            return 1;
        }
    }
    for (auto *&pointer : host_roundtrip) {
        if (!check_cuda(cudaMallocHost(reinterpret_cast<void **>(&pointer), plane_bytes),
                        "cudaMallocHost(P3P2 output)")) {
            cleanup();
            return 1;
        }
    }
    if (!check_cuda(cudaMallocHost(reinterpret_cast<void **>(&host_resize), 4 * 4),
                    "cudaMallocHost(resize)")) {
        cleanup();
        return 1;
    }

    for (int plane = 0; plane < 3; ++plane) {
        if (!check_cuda(cudaMemcpy(host_dst[plane], device_dst[plane], plane_bytes,
                                   cudaMemcpyDeviceToHost), "P2P3 readback")) {
            cleanup();
            return 1;
        }
    }
    for (int plane = 0; plane < 2; ++plane) {
        if (!check_cuda(cudaMemcpy(host_roundtrip[plane], device_roundtrip[plane], plane_bytes,
                                   cudaMemcpyDeviceToHost), "P3P2 readback")) {
            cleanup();
            return 1;
        }
    }
    if (!check_cuda(cudaMemcpy(host_resize, device_resize, 4 * 4, cudaMemcpyDeviceToHost),
                    "Resize readback")) {
        cleanup();
        return 1;
    }

    const std::vector<NppiSize> planar_sizes = {
        {width, height}, {width / 2, chroma_height}, {width / 2, chroma_height}
    };
    const std::vector<NppiSize> packed_sizes = {{width, height}, {width, chroma_height}};
    if (!write_planes(output_dir / "p2p3.yuv",
                      {host_dst[0], host_dst[1], host_dst[2]}, planar_sizes, step) ||
        !write_planes(output_dir / "roundtrip.nv12",
                      {host_roundtrip[0], host_roundtrip[1]}, packed_sizes, step) ||
        !write_buffer(output_dir / "resize_4x4.gray", host_resize, 4 * 4)) {
        std::cerr << "Failed to write reference output\n";
        cleanup();
        return 1;
    }

    bool roundtrip_matches = true;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const size_t offset = static_cast<size_t>(row) * step + col;
            if (host_roundtrip[0][offset] != y_plane[offset]) {
                roundtrip_matches = false;
            }
        }
    }
    for (int row = 0; row < chroma_height; ++row) {
        for (int col = 0; col < width; ++col) {
            const size_t offset = static_cast<size_t>(row) * step + col;
            if (host_roundtrip[1][offset] != uv_plane[offset]) {
                roundtrip_matches = false;
            }
        }
    }

    std::cout << "P2P3/P3P2 roundtrip: "
              << (roundtrip_matches ? "PASS" : "FAIL") << "\n"
              << "Reference output: " << output_dir << "\n";
    result = roundtrip_matches ? 0 : 1;
    cleanup();
    return result;
}
