// NPP Stage A reference/candidate probe.
//
// The same source runs on:
//   * NVIDIA NPP 11.5.1.107 -> generates golden output;
//   * the AMD NPP shim        -> generates candidate output.
//
// It covers nppiResizeSqrPixel_8u_C1R (NN, linear, cubic),
// nppiYCbCr420_8u_P2P3R and nppiYCbCr420_8u_P3P2R, including padded pitches and
// a non-zero ROI. compare_stage_a.py performs the byte-for-byte comparison.

#include <nppi.h>

#include "npp_probe_gpu.h"

#define GPU_MALLOC probe_gpu_malloc
#define GPU_MEMCPY_H2D probe_gpu_copy_h2d
#define GPU_MEMCPY_D2H probe_gpu_copy_d2h
#define GPU_SYNC probe_gpu_sync

static void gpu_free(void *pointer) {
    if (pointer) static_cast<void>(probe_gpu_free(pointer));
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct ResizeCase {
    const char *name;
    int src_width;
    int src_height;
    int src_pitch;
    int src_roi_x;
    int src_roi_y;
    int src_roi_width;
    int src_roi_height;
    int dst_width;
    int dst_height;
    int dst_pitch;
    int dst_roi_x;
    int dst_roi_y;
};

struct ColorCase {
    const char *name;
    int width;
    int height;
    int roi_x;
    int roi_y;
    int roi_width;
    int roi_height;
};

struct ResizeAlgorithm {
    const char *name;
    int value;
};

const ResizeAlgorithm kAlgorithms[] = {
    {"nn", NPPI_INTER_NN},
    {"linear", NPPI_INTER_LINEAR},
    {"cubic", NPPI_INTER_CUBIC},
};

const ResizeCase kResizeCases[] = {
    // FFmpeg-style full-frame resize, padded source and destination rows.
    {"full_320x240_to_160x120", 320, 240, 384, 0, 0, 320, 240,
     160, 120, 192, 0, 0},
    // Non-integer scale factor.
    {"full_320x240_to_480x270", 320, 240, 384, 0, 0, 320, 240,
     480, 270, 512, 0, 0},
    // Non-zero source ROI, half-size destination.
    {"roi_288x224_to_144x112", 320, 240, 384, 16, 8, 288, 224,
     144, 112, 160, 0, 0},
    // Odd destination dimensions and a non-zero destination ROI.
    {"small_64x48_to_37x29_offset", 64, 48, 80, 0, 0, 64, 48,
     37, 29, 64, 4, 3},
};

const ColorCase kColorCases[] = {
    {"full_320x240", 320, 240, 0, 0, 320, 240},
    {"roi_288x224", 320, 240, 16, 8, 288, 224},
};

std::uint8_t deterministic_byte(int x, int y, int plane, int seed) {
    return static_cast<std::uint8_t>((x * 37 + y * 53 + x * y * 11 +
                                      plane * 71 + seed * 19 + 13) & 0xff);
}

bool gpu_ok(int status, const char *operation) {
    if (status == 0) return true;
    std::cerr << "GPU error at " << operation << ": " << status << "\n";
    return false;
}

bool write_file(const std::filesystem::path &path,
                const std::vector<std::uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

void append_manifest(std::ofstream &manifest, const std::string &function,
                     const std::string &case_name, const std::string &variant,
                     const std::string &file, int width, int height, int pitch,
                     int roi_x, int roi_y, int roi_width, int roi_height) {
    manifest << function << '\t' << case_name << '\t' << variant << '\t'
             << file << '\t' << width << '\t' << height << '\t' << pitch << '\t'
             << roi_x << '\t' << roi_y << '\t' << roi_width << '\t'
             << roi_height << '\n';
}

std::vector<std::uint8_t> make_plane(int width, int height, int pitch,
                                     int plane, int seed) {
    std::vector<std::uint8_t> data(static_cast<std::size_t>(pitch) * height, 0xa5);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            data[static_cast<std::size_t>(y) * pitch + x] =
                deterministic_byte(x, y, plane, seed);
    return data;
}

std::vector<std::uint8_t> make_nv12(int width, int height, int y_pitch,
                                    int uv_pitch, int seed) {
    std::vector<std::uint8_t> data(
        static_cast<std::size_t>(y_pitch) * height +
        static_cast<std::size_t>(uv_pitch) * (height / 2), 0xa5);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            data[static_cast<std::size_t>(y) * y_pitch + x] =
                deterministic_byte(x, y, 0, seed);
    for (int y = 0; y < height / 2; ++y) {
        for (int x = 0; x < width; x += 2) {
            const std::size_t offset =
                static_cast<std::size_t>(y_pitch) * height +
                static_cast<std::size_t>(y) * uv_pitch + x;
            data[offset] = deterministic_byte(x / 2, y, 1, seed);
            data[offset + 1] = deterministic_byte(x / 2, y, 2, seed);
        }
    }
    return data;
}

bool run_resize_case(const ResizeCase &test, const ResizeAlgorithm &algorithm,
                     const std::filesystem::path &output_dir,
                     std::ofstream &manifest) {
    const std::string prefix =
        std::string("resize__") + test.name + "__" + algorithm.name;
    const std::vector<std::uint8_t> source =
        make_plane(test.src_width, test.src_height, test.src_pitch, 0, 3);
    const int dst_full_width = test.dst_roi_x + test.dst_width;
    const int dst_full_height = test.dst_roi_y + test.dst_height;
    std::vector<std::uint8_t> destination(
        static_cast<std::size_t>(test.dst_pitch) * dst_full_height, 0xa5);

    const std::string source_name = prefix + "__src.gray";
    const std::string output_name = prefix + "__dst.gray";
    if (!write_file(output_dir / source_name, source)) return false;

    Npp8u *device_src = nullptr;
    Npp8u *device_dst = nullptr;
    if (!gpu_ok(GPU_MALLOC(reinterpret_cast<void **>(&device_src), source.size()),
                "cudaMalloc/resize src") ||
        !gpu_ok(GPU_MALLOC(reinterpret_cast<void **>(&device_dst), destination.size()),
                "cudaMalloc/resize dst") ||
        !gpu_ok(GPU_MEMCPY_H2D(device_src, source.data(), source.size()),
                "copy resize src") ||
        !gpu_ok(GPU_MEMCPY_H2D(device_dst, destination.data(), destination.size()),
                "copy resize dst sentinel")) {
        gpu_free(device_src);
        gpu_free(device_dst);
        return false;
    }

    const NppiSize source_size{test.src_width, test.src_height};
    const NppiRect source_roi{
        test.src_roi_x, test.src_roi_y, test.src_roi_width, test.src_roi_height};
    const NppiRect destination_roi{
        test.dst_roi_x, test.dst_roi_y, test.dst_width, test.dst_height};
    const NppStatus status = nppiResizeSqrPixel_8u_C1R(
        device_src, source_size, test.src_pitch, source_roi,
        device_dst, test.dst_pitch, destination_roi,
        static_cast<double>(test.dst_width) / test.src_roi_width,
        static_cast<double>(test.dst_height) / test.src_roi_height,
        0.0, 0.0, algorithm.value);

    bool ok = status == NPP_SUCCESS &&
              gpu_ok(GPU_SYNC(), "sync resize") &&
              gpu_ok(GPU_MEMCPY_D2H(destination.data(), device_dst, destination.size()),
                     "readback resize dst") &&
              write_file(output_dir / output_name, destination);
    gpu_free(device_src);
    gpu_free(device_dst);
    if (!ok) {
        std::cerr << "resize failed: " << test.name << " " << algorithm.name
                  << " status=" << status << "\n";
        return false;
    }

    append_manifest(manifest, "resize", test.name, algorithm.name, source_name,
                    test.src_width, test.src_height, test.src_pitch,
                    test.src_roi_x, test.src_roi_y, test.src_roi_width,
                    test.src_roi_height);
    append_manifest(manifest, "resize", test.name, algorithm.name, output_name,
                    dst_full_width, dst_full_height, test.dst_pitch,
                    test.dst_roi_x, test.dst_roi_y, test.dst_width,
                    test.dst_height);
    return true;
}

bool run_p2p3_case(const ColorCase &test, const std::filesystem::path &output_dir,
                   std::ofstream &manifest, int seed) {
    const int y_pitch = 384;
    const int uv_pitch = 384;
    const int full_width = test.width;
    const int full_height = test.height;
    const std::vector<std::uint8_t> source =
        make_nv12(full_width, full_height, y_pitch, uv_pitch, seed);

    const int dst_y_pitch = 384;
    const int dst_u_pitch = 192;
    const int dst_v_pitch = 192;
    std::vector<std::uint8_t> dst_y(
        static_cast<std::size_t>(dst_y_pitch) * full_height, 0xa5);
    std::vector<std::uint8_t> dst_u(
        static_cast<std::size_t>(dst_u_pitch) * (full_height / 2), 0xa5);
    std::vector<std::uint8_t> dst_v(
        static_cast<std::size_t>(dst_v_pitch) * (full_height / 2), 0xa5);

    const std::string base = std::string("p2p3__") + test.name;
    const std::string source_name = base + "__src.nv12";
    const std::string output_name = base + "__dst.yuv";
    if (!write_file(output_dir / source_name, source)) return false;

    Npp8u *device_src = nullptr;
    Npp8u *device_y = nullptr;
    Npp8u *device_u = nullptr;
    Npp8u *device_v = nullptr;
    if (!gpu_ok(GPU_MALLOC(reinterpret_cast<void **>(&device_src), source.size()),
                "cudaMalloc/p2p3 src") ||
        !gpu_ok(GPU_MALLOC(reinterpret_cast<void **>(&device_y), dst_y.size()),
                "cudaMalloc/p2p3 Y") ||
        !gpu_ok(GPU_MALLOC(reinterpret_cast<void **>(&device_u), dst_u.size()),
                "cudaMalloc/p2p3 U") ||
        !gpu_ok(GPU_MALLOC(reinterpret_cast<void **>(&device_v), dst_v.size()),
                "cudaMalloc/p2p3 V") ||
        !gpu_ok(GPU_MEMCPY_H2D(device_src, source.data(), source.size()),
                "copy p2p3 src") ||
        !gpu_ok(GPU_MEMCPY_H2D(device_y, dst_y.data(), dst_y.size()),
                "copy p2p3 dst Y sentinel") ||
        !gpu_ok(GPU_MEMCPY_H2D(device_u, dst_u.data(), dst_u.size()),
                "copy p2p3 dst U sentinel") ||
        !gpu_ok(GPU_MEMCPY_H2D(device_v, dst_v.data(), dst_v.size()),
                "copy p2p3 dst V sentinel")) {
        gpu_free(device_src);
        gpu_free(device_y);
        gpu_free(device_u);
        gpu_free(device_v);
        return false;
    }

    Npp8u *dst_planes[3] = {device_y, device_u, device_v};
    int dst_steps[3] = {dst_y_pitch, dst_u_pitch, dst_v_pitch};
    const Npp8u *src_y = device_src +
        static_cast<std::size_t>(test.roi_y) * y_pitch + test.roi_x;
    const Npp8u *src_uv = device_src +
        static_cast<std::size_t>(y_pitch) * full_height +
        static_cast<std::size_t>(test.roi_y / 2) * uv_pitch + test.roi_x;
    NppiSize roi{test.roi_width, test.roi_height};
    const NppStatus status = nppiYCbCr420_8u_P2P3R(
        src_y, y_pitch, src_uv, uv_pitch, dst_planes, dst_steps, roi);

    bool ok = status == NPP_SUCCESS &&
              gpu_ok(GPU_SYNC(), "sync p2p3") &&
              gpu_ok(GPU_MEMCPY_D2H(dst_y.data(), device_y, dst_y.size()),
                     "readback p2p3 Y") &&
              gpu_ok(GPU_MEMCPY_D2H(dst_u.data(), device_u, dst_u.size()),
                     "readback p2p3 U") &&
              gpu_ok(GPU_MEMCPY_D2H(dst_v.data(), device_v, dst_v.size()),
                     "readback p2p3 V");
    gpu_free(device_src);
    gpu_free(device_y);
    gpu_free(device_u);
    gpu_free(device_v);
    if (!ok) {
        std::cerr << "p2p3 failed: " << test.name << " status=" << status << "\n";
        return false;
    }

    std::vector<std::uint8_t> destination;
    destination.insert(destination.end(), dst_y.begin(), dst_y.end());
    destination.insert(destination.end(), dst_u.begin(), dst_u.end());
    destination.insert(destination.end(), dst_v.begin(), dst_v.end());
    if (!write_file(output_dir / output_name, destination)) return false;

    append_manifest(manifest, "p2p3", test.name, "src", source_name,
                    full_width, full_height, y_pitch, test.roi_x, test.roi_y,
                    test.roi_width, test.roi_height);
    append_manifest(manifest, "p2p3", test.name, "dst", output_name,
                    full_width, full_height, dst_y_pitch, 0, 0,
                    test.roi_width, test.roi_height);
    return true;
}

bool run_p3p2_case(const ColorCase &test, const std::filesystem::path &output_dir,
                   std::ofstream &manifest, int seed) {
    const int full_width = test.width;
    const int full_height = test.height;
    const int y_pitch = 384;
    const int u_pitch = 192;
    const int v_pitch = 192;
    std::vector<std::uint8_t> src_y(
        static_cast<std::size_t>(y_pitch) * full_height, 0xa5);
    std::vector<std::uint8_t> src_u(
        static_cast<std::size_t>(u_pitch) * (full_height / 2), 0xa5);
    std::vector<std::uint8_t> src_v(
        static_cast<std::size_t>(v_pitch) * (full_height / 2), 0xa5);
    for (int y = 0; y < full_height; ++y)
        for (int x = 0; x < full_width; ++x)
            src_y[static_cast<std::size_t>(y) * y_pitch + x] =
                deterministic_byte(x, y, 0, seed);
    for (int y = 0; y < full_height / 2; ++y) {
        for (int x = 0; x < full_width / 2; ++x) {
            src_u[static_cast<std::size_t>(y) * u_pitch + x] =
                deterministic_byte(x, y, 1, seed);
            src_v[static_cast<std::size_t>(y) * v_pitch + x] =
                deterministic_byte(x, y, 2, seed);
        }
    }

    const int dst_y_pitch = 384;
    const int dst_uv_pitch = 384;
    std::vector<std::uint8_t> dst_y(
        static_cast<std::size_t>(dst_y_pitch) * full_height, 0xa5);
    std::vector<std::uint8_t> dst_uv(
        static_cast<std::size_t>(dst_uv_pitch) * (full_height / 2), 0xa5);

    const std::string base = std::string("p3p2__") + test.name;
    const std::string source_name = base + "__src.planar";
    const std::string output_name = base + "__dst.nv12";
    std::vector<std::uint8_t> source;
    source.insert(source.end(), src_y.begin(), src_y.end());
    source.insert(source.end(), src_u.begin(), src_u.end());
    source.insert(source.end(), src_v.begin(), src_v.end());
    if (!write_file(output_dir / source_name, source)) return false;

    Npp8u *device_y = nullptr;
    Npp8u *device_u = nullptr;
    Npp8u *device_v = nullptr;
    Npp8u *device_dst_y = nullptr;
    Npp8u *device_dst_uv = nullptr;
    const std::size_t y_offset =
        static_cast<std::size_t>(test.roi_y) * y_pitch + test.roi_x;
    const std::size_t u_offset =
        static_cast<std::size_t>(test.roi_y / 2) * u_pitch + test.roi_x / 2;
    if (!gpu_ok(GPU_MALLOC(reinterpret_cast<void **>(&device_y), src_y.size()),
                "cudaMalloc/p3p2 Y") ||
        !gpu_ok(GPU_MALLOC(reinterpret_cast<void **>(&device_u), src_u.size()),
                "cudaMalloc/p3p2 U") ||
        !gpu_ok(GPU_MALLOC(reinterpret_cast<void **>(&device_v), src_v.size()),
                "cudaMalloc/p3p2 V") ||
        !gpu_ok(GPU_MALLOC(reinterpret_cast<void **>(&device_dst_y), dst_y.size()),
                "cudaMalloc/p3p2 dst Y") ||
        !gpu_ok(GPU_MALLOC(reinterpret_cast<void **>(&device_dst_uv), dst_uv.size()),
                "cudaMalloc/p3p2 dst UV") ||
        !gpu_ok(GPU_MEMCPY_H2D(device_y, src_y.data(), src_y.size()),
                "copy p3p2 Y") ||
        !gpu_ok(GPU_MEMCPY_H2D(device_u, src_u.data(), src_u.size()),
                "copy p3p2 U") ||
        !gpu_ok(GPU_MEMCPY_H2D(device_v, src_v.data(), src_v.size()),
                "copy p3p2 V") ||
        !gpu_ok(GPU_MEMCPY_H2D(device_dst_y, dst_y.data(), dst_y.size()),
                "copy p3p2 dst Y sentinel") ||
        !gpu_ok(GPU_MEMCPY_H2D(device_dst_uv, dst_uv.data(), dst_uv.size()),
                "copy p3p2 dst UV sentinel")) {
        gpu_free(device_y);
        gpu_free(device_u);
        gpu_free(device_v);
        gpu_free(device_dst_y);
        gpu_free(device_dst_uv);
        return false;
    }

    const Npp8u *src_planes[3] = {device_y + y_offset, device_u + u_offset,
                                  device_v + u_offset};
    int src_steps[3] = {y_pitch, u_pitch, v_pitch};
    NppiSize roi{test.roi_width, test.roi_height};
    const NppStatus status = nppiYCbCr420_8u_P3P2R(
        src_planes, src_steps,
        device_dst_y + static_cast<std::size_t>(test.roi_y) * dst_y_pitch +
            test.roi_x,
        dst_y_pitch,
        // NV12 UV is byte-interleaved: luma x maps to byte offset x, not x/2.
        device_dst_uv + static_cast<std::size_t>(test.roi_y / 2) * dst_uv_pitch +
            test.roi_x,
        dst_uv_pitch, roi);

    bool ok = status == NPP_SUCCESS &&
              gpu_ok(GPU_SYNC(), "sync p3p2") &&
              gpu_ok(GPU_MEMCPY_D2H(dst_y.data(), device_dst_y, dst_y.size()),
                     "readback p3p2 Y") &&
              gpu_ok(GPU_MEMCPY_D2H(dst_uv.data(), device_dst_uv, dst_uv.size()),
                     "readback p3p2 UV");
    gpu_free(device_y);
    gpu_free(device_u);
    gpu_free(device_v);
    gpu_free(device_dst_y);
    gpu_free(device_dst_uv);
    if (!ok) {
        std::cerr << "p3p2 failed: " << test.name << " status=" << status << "\n";
        return false;
    }

    std::vector<std::uint8_t> destination;
    destination.insert(destination.end(), dst_y.begin(), dst_y.end());
    destination.insert(destination.end(), dst_uv.begin(), dst_uv.end());
    if (!write_file(output_dir / output_name, destination)) return false;

    append_manifest(manifest, "p3p2", test.name, "src", source_name,
                    full_width, full_height, y_pitch, test.roi_x, test.roi_y,
                    test.roi_width, test.roi_height);
    append_manifest(manifest, "p3p2", test.name, "dst", output_name,
                    full_width, full_height, dst_y_pitch, 0, 0,
                    test.roi_width, test.roi_height);
    return true;
}

} // namespace

int main(int argc, char **argv) {
    std::filesystem::path output_dir = "npp-stage-a-output";
    if (argc > 2 && std::strcmp(argv[1], "--output") == 0) {
        output_dir = argv[2];
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0] << " [--output DIR]\n";
        return 2;
    }

    std::error_code error;
    std::filesystem::create_directories(output_dir, error);
    if (error) {
        std::cerr << "cannot create " << output_dir << ": " << error.message() << "\n";
        return 2;
    }

    std::ofstream manifest(output_dir / "manifest.tsv");
    if (!manifest) {
        std::cerr << "cannot create manifest\n";
        return 2;
    }

    for (const auto &test : kResizeCases)
        for (const auto &algorithm : kAlgorithms)
            if (!run_resize_case(test, algorithm, output_dir, manifest)) return 1;

    for (const auto &test : kColorCases) {
        if (!run_p2p3_case(test, output_dir, manifest, 11)) return 1;
        if (!run_p3p2_case(test, output_dir, manifest, 23)) return 1;
    }

    std::cout << "NPP Stage A outputs written to " << output_dir << "\n";
    return 0;
}
