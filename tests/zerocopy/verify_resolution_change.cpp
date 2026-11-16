// CUVID-level dynamic-resolution host.
//
// FFmpeg's CLI keeps a fixed AVHWFramesContext, so it cannot decode a stream
// whose dimensions change mid-stream even though the shim now re-issues the
// sequence callback. This host mimics a player: it creates and destroys the
// CUVID decoder on every sequence change, decodes the stream and dumps each
// displayed frame as tightly packed NV12. run_resolution_change.sh compares
// the dumps with the software decoder.
//
// The shim's own CUVID parser is used (cuvidCreateVideoParser), so this also
// covers SPS change detection and DPB reset.
//
// Build: make -f verify_resolution_change.make (or the zerocopy Makefile).

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>
#include <ffnvcodec/dynlink_nvcuvid.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
CUresult cuvidCreateVideoParser(CUvideoparser *obj, CUVIDPARSERPARAMS *params);
CUresult cuvidDestroyVideoParser(CUvideoparser obj);
CUresult cuvidParseVideoData(CUvideoparser obj, CUVIDSOURCEDATAPACKET *packet);
CUresult cuvidCreateDecoder(CUvideodecoder *handle, CUVIDDECODECREATEINFO *info);
CUresult cuvidDestroyDecoder(CUvideodecoder handle);
CUresult cuvidDecodePicture(CUvideodecoder handle, CUVIDPICPARAMS *picture);
CUresult cuvidMapVideoFrame64(CUvideodecoder handle, int picture_index,
                              unsigned long long *dev_ptr, unsigned int *pitch,
                              CUVIDPROCPARAMS *proc_params);
CUresult cuvidUnmapVideoFrame64(CUvideodecoder handle, unsigned long long dev_ptr);
}

// Implemented in verify_resolution_change_hip.cpp (compiled by hipcc).
extern "C" int shim_read_plane(const void *device, void *host, size_t pitch,
                               int width, int height);

namespace {

struct State {
    CUvideodecoder decoder = nullptr;
    int coded_width = 0;
    int coded_height = 0;
    int width = 0;
    int height = 0;
    int frames = 0;
    std::string out_dir;
};

State g_state;

int CUDAAPI on_sequence(void *, CUVIDEOFORMAT *format) {
    const int width = format->display_area.right - format->display_area.left;
    const int height = format->display_area.bottom - format->display_area.top;
    if (width <= 0 || height <= 0) return 0;

    int surfaces = format->min_num_decode_surfaces + 3;
    if (surfaces < 2) surfaces = 2;
    if (surfaces > 32) surfaces = 32;

    const bool changed = g_state.decoder &&
                         (format->coded_width != g_state.coded_width ||
                          format->coded_height != g_state.coded_height ||
                          width != g_state.width || height != g_state.height);
    if (changed) {
        // The shim flushes its display queue before invoking this callback, so
        // no picture of the old sequence can still be mapped.
        std::fprintf(stderr, "SEQUENCE CHANGE %dx%d -> %dx%d\n",
                     g_state.width, g_state.height, width, height);
        cuvidDestroyDecoder(g_state.decoder);
        g_state.decoder = nullptr;
    }

    if (!g_state.decoder) {
        CUVIDDECODECREATEINFO info = {};
        info.ulWidth = format->coded_width;
        info.ulHeight = format->coded_height;
        info.ulNumDecodeSurfaces = static_cast<unsigned long>(surfaces);
        info.CodecType = format->codec;
        info.ChromaFormat = format->chroma_format;
        info.OutputFormat = cudaVideoSurfaceFormat_NV12;
        info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
        info.ulTargetWidth = static_cast<unsigned long>(width);
        info.ulTargetHeight = static_cast<unsigned long>(height);
        info.ulMaxWidth = 4096;
        info.ulMaxHeight = 4096;
        info.display_area.left = format->display_area.left;
        info.display_area.top = format->display_area.top;
        info.display_area.right = format->display_area.right;
        info.display_area.bottom = format->display_area.bottom;
        if (cuvidCreateDecoder(&g_state.decoder, &info) != CUDA_SUCCESS) {
            std::fprintf(stderr, "cuvidCreateDecoder failed for %dx%d\n", width, height);
            g_state.decoder = nullptr;
            return 0;
        }
        g_state.coded_width = static_cast<int>(format->coded_width);
        g_state.coded_height = static_cast<int>(format->coded_height);
        g_state.width = width;
        g_state.height = height;
        std::fprintf(stderr, "SEQUENCE %dx%d (coded %dx%d, %d surfaces)\n",
                     width, height, g_state.coded_width, g_state.coded_height, surfaces);
    }
    return surfaces;
}

int CUDAAPI on_decode(void *, CUVIDPICPARAMS *picture) {
    if (!g_state.decoder) return 0;
    return cuvidDecodePicture(g_state.decoder, picture) == CUDA_SUCCESS ? 1 : 0;
}

int CUDAAPI on_display(void *, CUVIDPARSERDISPINFO *info) {
    if (!g_state.decoder) return 0;

    CUVIDPROCPARAMS params = {};
    params.progressive_frame = info->progressive_frame;
    unsigned long long device = 0;
    unsigned int pitch = 0;
    CUresult result = cuvidMapVideoFrame64(g_state.decoder, info->picture_index,
                                           &device, &pitch, &params);
    if (result != CUDA_SUCCESS) {
        std::fprintf(stderr, "cuvidMapVideoFrame64 failed for picture %d: %d\n",
                     info->picture_index, (int)result);
        return 0;
    }

    const int w = g_state.width;
    const int h = g_state.height;
    std::vector<unsigned char> frame(static_cast<size_t>(w) * h * 3 / 2);

    const bool ok =
        shim_read_plane(reinterpret_cast<const void *>(device), frame.data(), pitch,
                        w, h) == 0 &&
        shim_read_plane(reinterpret_cast<const void *>(device + pitch * h),
                        frame.data() + static_cast<size_t>(w) * h, pitch, w, h / 2) == 0;
    cuvidUnmapVideoFrame64(g_state.decoder, device);
    if (!ok) {
        std::fprintf(stderr, "HIP read failed for picture %d\n", info->picture_index);
        return 0;
    }

    char name[512];
    std::snprintf(name, sizeof(name), "%s/frame-%03d-%dx%d.nv12",
                  g_state.out_dir.c_str(), g_state.frames, w, h);
    FILE *file = std::fopen(name, "wb");
    if (!file) {
        std::fprintf(stderr, "cannot write %s\n", name);
        return 0;
    }
    const bool written = std::fwrite(frame.data(), 1, frame.size(), file) == frame.size();
    std::fclose(file);
    if (!written) {
        std::fprintf(stderr, "short write to %s\n", name);
        return 0;
    }
    ++g_state.frames;
    return 1;
}

bool read_file(const char *path, std::vector<unsigned char> *data) {
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return false;
    }
    data->resize(static_cast<size_t>(size));
    const bool ok = std::fread(data->data(), 1, data->size(), file) == data->size();
    std::fclose(file);
    return ok;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <input.h264> <output-dir>\n", argv[0]);
        return 2;
    }
    g_state.out_dir = argv[2];

    std::vector<unsigned char> stream;
    if (!read_file(argv[1], &stream)) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    CUVIDPARSERPARAMS parser_params = {};
    parser_params.CodecType = cudaVideoCodec_H264;
    parser_params.ulMaxNumDecodeSurfaces = 32;
    parser_params.ulMaxDisplayDelay = 0;
    parser_params.pfnSequenceCallback = on_sequence;
    parser_params.pfnDecodePicture = on_decode;
    parser_params.pfnDisplayPicture = on_display;

    CUvideoparser parser = nullptr;
    if (cuvidCreateVideoParser(&parser, &parser_params) != CUDA_SUCCESS) {
        std::fprintf(stderr, "cuvidCreateVideoParser failed\n");
        return 2;
    }

    CUVIDSOURCEDATAPACKET packet = {};
    packet.payload = stream.data();
    packet.payload_size = static_cast<unsigned long>(stream.size());
    CUresult result = cuvidParseVideoData(parser, &packet);
    if (result == CUDA_SUCCESS) {
        CUVIDSOURCEDATAPACKET eos = {};
        eos.flags = CUVID_PKT_ENDOFSTREAM;
        result = cuvidParseVideoData(parser, &eos);
    }

    if (g_state.decoder) cuvidDestroyDecoder(g_state.decoder);
    cuvidDestroyVideoParser(parser);

    if (result != CUDA_SUCCESS) {
        std::fprintf(stderr, "decode failed: %d\n", (int)result);
        return 1;
    }
    std::printf("RESULT: %d frames decoded\n", g_state.frames);
    return 0;
}
