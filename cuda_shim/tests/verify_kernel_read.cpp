// verify_kernel_read.cpp
// 验证 cuvidMapVideoFrame64 返回的 dev_ptr 不只是“能被 hipMemcpy 读”，
// 而是能被 HIP kernel 直接按 pitch 寻址；同时验证 map→kernel→event→unmap
// 的真实同步顺序。
//
// 链路：CUVID parser → callback decode → callback display → map → HIP kernel
//       → hipEventRecord/Synchronize → hipMemcpy 回读 → CPU checksum → unmap。
//
// 支持后端：CUVID_SHIM_BACKEND=vaapi|vk（由 libnvcuvid 内部选择）。
// 运行：
//   CUVID_SHIM_BACKEND=vaapi ./verify_kernel_read test_p_b.h264
//   CUVID_SHIM_BACKEND=vk    ./verify_kernel_read test_p_b.h264
//
// 判据：每一帧 GPU kernel 的 Y/UV checksum 与 CPU 对同一 mapped 指针回读的
//       checksum 一致；至少 10 帧；无 decode/map/kernel 错误。像素本身是否
//       正确由 verify_decode_to_hip / verify_vk_decode_to_hip 负责，本程序
//       只证明 mapped 设备指针的 kernel 可访问性和事件顺序。

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>
#include <ffnvcodec/dynlink_nvcuvid.h>

#include <hip/hip_runtime.h>

// ROCm's amd_hip_runtime.h does `#define CUDA_SUCCESS hipSuccess`, which would
// otherwise turn the CUresult comparisons below into enum-vs-hipError_t.
// The CUVID enum was already parsed from dynlink_cuda.h before this include.
#ifdef CUDA_SUCCESS
#undef CUDA_SUCCESS
#endif

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

extern "C" {
CUresult CUDAAPI cuvidCreateDecoder(CUvideodecoder *, CUVIDDECODECREATEINFO *);
CUresult CUDAAPI cuvidDestroyDecoder(CUvideodecoder);
CUresult CUDAAPI cuvidDecodePicture(CUvideodecoder, CUVIDPICPARAMS *);
CUresult CUDAAPI cuvidMapVideoFrame64(CUvideodecoder, int, unsigned long long *,
                                      unsigned int *, CUVIDPROCPARAMS *);
CUresult CUDAAPI cuvidUnmapVideoFrame64(CUvideodecoder, unsigned long long);
CUresult CUDAAPI cuvidCreateVideoParser(CUvideoparser *, CUVIDPARSERPARAMS *);
CUresult CUDAAPI cuvidParseVideoData(CUvideoparser, CUVIDSOURCEDATAPACKET *);
CUresult CUDAAPI cuvidDestroyVideoParser(CUvideoparser);
}

namespace {

struct TestState {
    CUvideodecoder decoder = nullptr;
    int width = 0;
    int height = 0;
    int sequences = 0;
    int decodes = 0;
    int displays = 0;
    int frames_ok = 0;
    int errors = 0;
    hipStream_t stream = nullptr;
    hipEvent_t done = nullptr;
    unsigned long long *d_sums = nullptr;
};

TestState g;

// One block, 256 threads; shared-memory reduction avoids relying on 64-bit
// atomicAdd support in every ROCm/HIP target.
__global__ void checksum_nv12_kernel(const unsigned char *base, int pitch,
                                     int width, int height,
                                     unsigned long long *sums) {
    __shared__ unsigned long long sy[256];
    __shared__ unsigned long long suv[256];

    const int tid = threadIdx.x;
    const int stride = blockDim.x;
    unsigned long long ysum = 0;
    unsigned long long uvsum = 0;

    for (int row = 0; row < height; ++row) {
        const unsigned char *p = base + static_cast<size_t>(row) * pitch;
        for (int x = tid; x < width; x += stride) ysum += p[x];
    }

    const unsigned char *uv = base + static_cast<size_t>(pitch) * height;
    const int uv_height = height / 2;
    for (int row = 0; row < uv_height; ++row) {
        const unsigned char *p = uv + static_cast<size_t>(row) * pitch;
        for (int x = tid; x < width; x += stride) uvsum += p[x];
    }

    sy[tid] = ysum;
    suv[tid] = uvsum;
    __syncthreads();
    for (int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (tid < s) {
            sy[tid] += sy[tid + s];
            suv[tid] += suv[tid + s];
        }
        __syncthreads();
    }
    if (tid == 0) {
        sums[0] = sy[0];
        sums[1] = suv[0];
    }
}

void checksum_host(const unsigned char *base, int pitch, int width, int height,
                   unsigned long long out[2]) {
    out[0] = 0;
    out[1] = 0;
    for (int row = 0; row < height; ++row) {
        const unsigned char *p = base + static_cast<size_t>(row) * pitch;
        for (int x = 0; x < width; ++x) out[0] += p[x];
    }
    const unsigned char *uv = base + static_cast<size_t>(pitch) * height;
    const int uv_height = height / 2;
    for (int row = 0; row < uv_height; ++row) {
        const unsigned char *p = uv + static_cast<size_t>(row) * pitch;
        for (int x = 0; x < width; ++x) out[1] += p[x];
    }
}

bool read_file(const char *path, std::vector<unsigned char> *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamsize n = in.tellg();
    if (n <= 0) return false;
    in.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(n));
    return in.read(reinterpret_cast<char *>(out->data()), n).gcount() == n;
}

int CUDAAPI cb_sequence(void *, CUVIDEOFORMAT *format) {
    if (!format || format->coded_width <= 0 || format->coded_height <= 0) {
        std::fprintf(stderr, "FAIL: invalid sequence format\n");
        ++g.errors;
        return 0;
    }

    if (!g.decoder) {
        g.width = static_cast<int>(format->coded_width);
        g.height = static_cast<int>(format->coded_height);

        CUVIDDECODECREATEINFO info = {};
        info.ulWidth = format->coded_width;
        info.ulHeight = format->coded_height;
        info.ulNumDecodeSurfaces = 32;
        info.CodecType = cudaVideoCodec_H264;
        info.ChromaFormat = cudaVideoChromaFormat_420;
        info.bitDepthMinus8 = 0;
        info.ulMaxWidth = format->coded_width;
        info.ulMaxHeight = format->coded_height;
        info.OutputFormat = cudaVideoSurfaceFormat_NV12;
        info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
        info.ulTargetWidth = format->coded_width;
        info.ulTargetHeight = format->coded_height;
        info.ulNumOutputSurfaces = 1;

        const CUresult result = cuvidCreateDecoder(&g.decoder, &info);
        if (result != CUDA_SUCCESS || !g.decoder) {
            std::fprintf(stderr, "FAIL: cuvidCreateDecoder result=%d\n",
                         static_cast<int>(result));
            ++g.errors;
            return 0;
        }
        std::printf("  [sequence] %dx%d, decoder created\n", g.width, g.height);
    } else if (g.width != static_cast<int>(format->coded_width) ||
               g.height != static_cast<int>(format->coded_height)) {
        std::fprintf(stderr, "FAIL: resolution change %dx%d -> %ux%u unsupported in this test\n",
                     g.width, g.height, format->coded_width, format->coded_height);
        ++g.errors;
        return 0;
    }

    ++g.sequences;
    return 32;
}

int CUDAAPI cb_decode(void *, CUVIDPICPARAMS *picture) {
    if (!g.decoder || !picture) {
        ++g.errors;
        return 0;
    }
    const CUresult result = cuvidDecodePicture(g.decoder, picture);
    if (result != CUDA_SUCCESS) {
        std::fprintf(stderr, "FAIL: cuvidDecodePicture idx=%d result=%d\n",
                     picture->CurrPicIdx, static_cast<int>(result));
        ++g.errors;
        return 0;
    }
    ++g.decodes;
    return 1;
}

int CUDAAPI cb_display(void *, CUVIDPARSERDISPINFO *disp) {
    if (!g.decoder || !disp) {
        ++g.errors;
        return 0;
    }
    ++g.displays;

    CUVIDPROCPARAMS proc = {};
    proc.progressive_frame = 1;

    unsigned long long dev_ptr = 0;
    unsigned int pitch = 0;
    CUresult result = cuvidMapVideoFrame64(g.decoder, disp->picture_index,
                                           &dev_ptr, &pitch, &proc);
    if (result != CUDA_SUCCESS || dev_ptr == 0 || pitch < static_cast<unsigned>(g.width)) {
        std::fprintf(stderr,
                     "FAIL: map idx=%d result=%d ptr=0x%llx pitch=%u width=%d\n",
                     disp->picture_index, static_cast<int>(result), dev_ptr, pitch, g.width);
        ++g.errors;
        return 0;
    }

    const auto base = reinterpret_cast<const unsigned char *>(
        static_cast<uintptr_t>(dev_ptr));

    hipMemsetAsync(g.d_sums, 0, 2 * sizeof(unsigned long long), g.stream);
    hipLaunchKernelGGL(checksum_nv12_kernel, dim3(1), dim3(256), 0, g.stream,
                       base, static_cast<int>(pitch), g.width, g.height, g.d_sums);
    const hipError_t launch_err = hipGetLastError();
    if (launch_err != hipSuccess) {
        std::fprintf(stderr, "FAIL: kernel launch: %s\n", hipGetErrorString(launch_err));
        ++g.errors;
        cuvidUnmapVideoFrame64(g.decoder, dev_ptr);
        return 0;
    }

    // 真实顺序：kernel 在 stream 上完成，再 unmap / 复用 surface。
    // 这就是“计算→释放解码槽”的边界，不能只靠主机 sleep。
    hipEventRecord(g.done, g.stream);
    const hipError_t sync_err = hipEventSynchronize(g.done);
    if (sync_err != hipSuccess) {
        std::fprintf(stderr, "FAIL: hipEventSynchronize: %s\n",
                     hipGetErrorString(sync_err));
        ++g.errors;
        cuvidUnmapVideoFrame64(g.decoder, dev_ptr);
        return 0;
    }

    unsigned long long gpu_sums[2] = {};
    hipMemcpy(gpu_sums, g.d_sums, sizeof(gpu_sums), hipMemcpyDeviceToHost);

    const size_t total = static_cast<size_t>(pitch) * (g.height + g.height / 2);
    std::vector<unsigned char> host(total);
    bool copy_ok = true;
    for (int row = 0; row < g.height && copy_ok; ++row) {
        copy_ok = hipMemcpy(host.data() + static_cast<size_t>(row) * pitch,
                            base + static_cast<size_t>(row) * pitch, pitch,
                            hipMemcpyDeviceToHost) == hipSuccess;
    }
    for (int row = 0; row < g.height / 2 && copy_ok; ++row) {
        const size_t dst_off = static_cast<size_t>(g.height + row) * pitch;
        copy_ok = hipMemcpy(host.data() + dst_off,
                            base + dst_off, pitch,
                            hipMemcpyDeviceToHost) == hipSuccess;
    }

    unsigned long long cpu_sums[2] = {};
    if (copy_ok) checksum_host(host.data(), static_cast<int>(pitch), g.width, g.height, cpu_sums);

    // 可选：把每帧按显示顺序 dump 成 NV12（pitch==width 时就是标准 yuv420p），
    // 用来和 ffmpeg 软解结果做逐帧逐平面对比。CUVID_DUMP_NV12_DIR=<dir>
    if (copy_ok) {
        const char *dump_dir = std::getenv("CUVID_DUMP_NV12_DIR");
        if (dump_dir && *dump_dir) {
            const size_t frame_bytes = static_cast<size_t>(pitch) * (g.height + g.height / 2);
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/dec_%04d_%dx%d_p%u.nv12", dump_dir, g.displays,
                          g.width, g.height, pitch);
            std::ofstream out(path, std::ios::binary);
            if (out) out.write(reinterpret_cast<const char *>(host.data()),
                               static_cast<std::streamsize>(frame_bytes));
        }
    }

    if (!copy_ok || gpu_sums[0] == 0 || gpu_sums[1] == 0 ||
        gpu_sums[0] != cpu_sums[0] || gpu_sums[1] != cpu_sums[1]) {
        std::fprintf(stderr,
                     "FAIL: frame %d checksum mismatch gpu=(%llu,%llu) cpu=(%llu,%llu)\n",
                     g.displays, gpu_sums[0], gpu_sums[1], cpu_sums[0], cpu_sums[1]);
        ++g.errors;
    } else {
        ++g.frames_ok;
        if (g.frames_ok <= 3 || g.frames_ok % 10 == 0) {
            std::printf("  [display %d] idx=%d pitch=%u Ysum=%llu UVsum=%llu OK\n",
                        g.frames_ok, disp->picture_index, pitch,
                        gpu_sums[0], gpu_sums[1]);
        }
    }

    result = cuvidUnmapVideoFrame64(g.decoder, dev_ptr);
    if (result != CUDA_SUCCESS) {
        std::fprintf(stderr, "FAIL: unmap result=%d\n", static_cast<int>(result));
        ++g.errors;
        return 0;
    }
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    const char *stream_path = argc > 1 ? argv[1] : "../testdata/h264/test_p_b.h264";
    std::vector<unsigned char> data;
    if (!read_file(stream_path, &data)) {
        std::fprintf(stderr, "FAIL: cannot read %s\n", stream_path);
        return 2;
    }

    std::printf("=== mapped dev_ptr kernel read: %s (%zu bytes) ===\n",
                stream_path, data.size());
    const char *backend = std::getenv("CUVID_SHIM_BACKEND");
    std::printf("  backend: %s\n", backend && *backend ? backend : "vaapi(default)");

    if (hipSetDevice(0) != hipSuccess) {
        std::fprintf(stderr, "FAIL: hipSetDevice\n");
        return 2;
    }
    if (hipStreamCreate(&g.stream) != hipSuccess ||
        hipEventCreateWithFlags(&g.done, hipEventDisableTiming) != hipSuccess ||
        hipMalloc(reinterpret_cast<void **>(&g.d_sums),
                  2 * sizeof(unsigned long long)) != hipSuccess) {
        std::fprintf(stderr, "FAIL: HIP resource creation\n");
        return 2;
    }

    CUVIDPARSERPARAMS params = {};
    params.CodecType = cudaVideoCodec_H264;
    params.ulMaxNumDecodeSurfaces = 32;
    params.ulMaxDisplayDelay = 0;
    params.pfnSequenceCallback = cb_sequence;
    params.pfnDecodePicture = cb_decode;
    params.pfnDisplayPicture = cb_display;

    CUvideoparser parser = nullptr;
    if (cuvidCreateVideoParser(&parser, &params) != CUDA_SUCCESS || !parser) {
        std::fprintf(stderr, "FAIL: cuvidCreateVideoParser\n");
        return 2;
    }

    CUVIDSOURCEDATAPACKET packet = {};
    packet.payload = data.data();
    packet.payload_size = static_cast<unsigned long>(data.size());
    packet.flags = CUVID_PKT_TIMESTAMP;
    packet.timestamp = 0;
    CUresult result = cuvidParseVideoData(parser, &packet);
    if (result == CUDA_SUCCESS) {
        CUVIDSOURCEDATAPACKET eos = {};
        eos.payload_size = 0;
        eos.flags = CUVID_PKT_ENDOFSTREAM;
        result = cuvidParseVideoData(parser, &eos);
    }

    cuvidDestroyVideoParser(parser);
    if (g.decoder) cuvidDestroyDecoder(g.decoder);
    hipFree(g.d_sums);
    hipEventDestroy(g.done);
    hipStreamDestroy(g.stream);

    std::printf("\n=== results ===\n"
                "  sequence=%d decode=%d display=%d frames_ok=%d errors=%d\n",
                g.sequences, g.decodes, g.displays, g.frames_ok, g.errors);

    if (result != CUDA_SUCCESS) {
        std::fprintf(stderr, "FAIL: parse result=%d\n", static_cast<int>(result));
        return 1;
    }
    if (g.frames_ok < 10 || g.frames_ok != g.displays || g.errors != 0) {
        std::fprintf(stderr, "FAIL: expected >=10 frames and no errors\n");
        return 1;
    }

    std::printf("PASS: mapped dev_ptr is kernel-addressable and event-ordered "
                "(%d frames)\n", g.frames_ok);
    return 0;
}
