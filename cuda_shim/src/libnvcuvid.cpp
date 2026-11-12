// libnvcuvid.so.1 — CUVID compatibility surface.
// Unsupported operations fail explicitly; symbol availability is not a claim
// that the corresponding decoder or parser functionality is complete.

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>
#include <ffnvcodec/dynlink_nvcuvid.h>

#include "hip_frame_mapper.h"
#include "backend_dispatch.h"
#include "cuvid_parser.h"

#include <memory>
#include <mutex>
#include <unordered_map>

#ifndef CUDA_ERROR_OUT_OF_MEMORY
#define CUDA_ERROR_OUT_OF_MEMORY static_cast<CUresult>(2)
#endif
#ifndef CUDA_ERROR_NOT_SUPPORTED
#define CUDA_ERROR_NOT_SUPPORTED static_cast<CUresult>(801)
#endif

namespace {

using cuvid_shim::HipFrameMapping;
using cuvid_shim::DecoderBackend;

struct DecoderState {
    explicit DecoderState(const CUVIDDECODECREATEINFO &create_info)
        : info(create_info), statuses(create_info.ulNumDecodeSurfaces),
          backend(cuvid_shim::make_backend()) {}

    CUVIDDECODECREATEINFO info;
    std::unique_ptr<DecoderBackend> backend;
    std::mutex mutex;
    std::unordered_map<unsigned long long, HipFrameMapping> mappings;
    std::unordered_map<int, unsigned long long> mapped_indices;
    std::vector<CUVIDGETDECODESTATUS> statuses;
};

std::mutex decoders_mutex;
std::unordered_map<CUvideodecoder, std::shared_ptr<DecoderState>> decoders;

std::shared_ptr<DecoderState> get_decoder(CUvideodecoder handle) {
    std::lock_guard<std::mutex> guard(decoders_mutex);
    auto it = decoders.find(handle);
    return it == decoders.end() ? nullptr : it->second;
}

struct ParserState {
    explicit ParserState(const CUVIDPARSERPARAMS &params)
        : parser(params) {}

    cuvid_shim::CuvidParser parser;
    std::mutex mutex;
};

std::mutex parsers_mutex;
std::unordered_map<CUvideoparser, std::shared_ptr<ParserState>> parsers;

std::shared_ptr<ParserState> get_parser(CUvideoparser handle) {
    std::lock_guard<std::mutex> guard(parsers_mutex);
    auto it = parsers.find(handle);
    return it == parsers.end() ? nullptr : it->second;
}

struct ContextLock {
    explicit ContextLock(CUcontext context) : context(context) {}
    CUcontext context;
    std::recursive_mutex mutex;
};

CUresult unsupported() {
    return CUDA_ERROR_NOT_SUPPORTED;
}

} // namespace

extern "C" {

CUresult CUDAAPI cuvidGetDecoderCaps(CUVIDDECODECAPS *caps) {
    if (!caps) return CUDA_ERROR_INVALID_VALUE;
    const bool supported = caps->eCodecType == cudaVideoCodec_H264 &&
                           caps->eChromaFormat == cudaVideoChromaFormat_420 &&
                           caps->nBitDepthMinus8 == 0;
    caps->bIsSupported = supported ? 1 : 0;
    caps->nNumNVDECs = supported ? 1 : 0;
    caps->nOutputFormatMask = supported ? (1U << cudaVideoSurfaceFormat_NV12) : 0;
    caps->nMaxWidth = supported ? 4096 : 0;
    caps->nMaxHeight = supported ? 4096 : 0;
    caps->nMaxMBCount = supported ? 4096U * 4096U / 256U : 0;
    caps->nMinWidth = supported ? 16 : 0;
    caps->nMinHeight = supported ? 16 : 0;
    caps->bIsHistogramSupported = 0;
    caps->nCounterBitDepth = 0;
    caps->nMaxHistogramBins = 0;
    return CUDA_SUCCESS;
}

CUresult CUDAAPI cuvidCreateDecoder(CUvideodecoder *handle, CUVIDDECODECREATEINFO *info) {
    if (!handle || !info) return CUDA_ERROR_INVALID_VALUE;
    *handle = nullptr;
    try {
        auto state = std::make_shared<DecoderState>(*info);
        CUresult result = state->backend->initialize(*info);
        if (result != CUDA_SUCCESS) return result;
        CUvideodecoder new_handle = reinterpret_cast<CUvideodecoder>(state.get());
        {
            std::lock_guard<std::mutex> guard(decoders_mutex);
            decoders.emplace(new_handle, state);
        }
        *handle = new_handle;
        return CUDA_SUCCESS;
    } catch (const std::bad_alloc &) {
        return CUDA_ERROR_OUT_OF_MEMORY;
    }
}

CUresult CUDAAPI cuvidDestroyDecoder(CUvideodecoder handle) {
    if (!handle) return CUDA_ERROR_INVALID_VALUE;
    std::shared_ptr<DecoderState> state;
    {
        std::lock_guard<std::mutex> guard(decoders_mutex);
        auto it = decoders.find(handle);
        if (it == decoders.end()) return CUDA_ERROR_INVALID_VALUE;
        state = it->second;
        decoders.erase(it);
    }
    std::lock_guard<std::mutex> guard(state->mutex);
    state->mappings.clear();
    state->mapped_indices.clear();
    state->backend->reset();
    return CUDA_SUCCESS;
}

CUresult CUDAAPI cuvidDecodePicture(CUvideodecoder handle, CUVIDPICPARAMS *picture) {
    auto state = get_decoder(handle);
    if (!state || !picture) return CUDA_ERROR_INVALID_VALUE;
    std::lock_guard<std::mutex> guard(state->mutex);
    if (picture->CurrPicIdx < 0 ||
        static_cast<std::size_t>(picture->CurrPicIdx) >= state->statuses.size()) {
        return CUDA_ERROR_INVALID_VALUE;
    }
    if (state->mapped_indices.count(picture->CurrPicIdx)) return CUDA_ERROR_INVALID_VALUE;
    state->statuses[picture->CurrPicIdx].decodeStatus = cuvidDecodeStatus_InProgress;
    CUresult result = state->backend->decode(*picture);
    if (result != CUDA_SUCCESS) {
        state->statuses[picture->CurrPicIdx].decodeStatus = cuvidDecodeStatus_Error;
    }
    return result;
}

CUresult CUDAAPI cuvidGetDecodeStatus(CUvideodecoder handle, int picture_index,
                                      CUVIDGETDECODESTATUS *status) {
    auto state = get_decoder(handle);
    if (!state || !status) return CUDA_ERROR_INVALID_VALUE;
    std::lock_guard<std::mutex> guard(state->mutex);
    if (picture_index < 0 || static_cast<std::size_t>(picture_index) >= state->statuses.size()) {
        return CUDA_ERROR_INVALID_VALUE;
    }
    *status = state->statuses[picture_index];
    return CUDA_SUCCESS;
}

CUresult CUDAAPI cuvidReconfigureDecoder(CUvideodecoder, CUVIDRECONFIGUREDECODERINFO *) {
    return unsupported();
}

CUresult CUDAAPI cuvidMapVideoFrame64(CUvideodecoder handle, int picture_index,
                                      unsigned long long *dev_ptr, unsigned int *pitch,
                                      CUVIDPROCPARAMS *proc_params) {
    auto state = get_decoder(handle);
    if (!state || !dev_ptr || !pitch) return CUDA_ERROR_INVALID_VALUE;
    *dev_ptr = 0;
    *pitch = 0;
    if (proc_params && (!proc_params->progressive_frame || proc_params->second_field ||
                        proc_params->unpaired_field)) {
        return CUDA_ERROR_NOT_SUPPORTED;
    }

    std::lock_guard<std::mutex> guard(state->mutex);
    if (state->mapped_indices.count(picture_index)) {
        return CUDA_ERROR_INVALID_VALUE;
    }

    HipFrameMapping mapping;
    CUresult result = state->backend->map_frame(picture_index, &mapping);
    if (result != CUDA_SUCCESS) return result;
    if (!mapping.has_contiguous_nv12_layout()) return CUDA_ERROR_NOT_SUPPORTED;

    const unsigned long long pointer = mapping.device_pointer();
    const unsigned int output_pitch = mapping.pitch();
    state->mappings.emplace(pointer, std::move(mapping));
    state->mapped_indices.emplace(picture_index, pointer);
    state->statuses[picture_index].decodeStatus = cuvidDecodeStatus_Success;
    *dev_ptr = pointer;
    *pitch = output_pitch;
    return CUDA_SUCCESS;
}

CUresult CUDAAPI cuvidUnmapVideoFrame64(CUvideodecoder handle, unsigned long long dev_ptr) {
    auto state = get_decoder(handle);
    if (!state || !dev_ptr) return CUDA_ERROR_INVALID_VALUE;
    std::lock_guard<std::mutex> guard(state->mutex);
    auto mapping = state->mappings.find(dev_ptr);
    if (mapping == state->mappings.end()) return CUDA_ERROR_INVALID_VALUE;
    for (auto it = state->mapped_indices.begin(); it != state->mapped_indices.end(); ++it) {
        if (it->second == dev_ptr) {
            state->mapped_indices.erase(it);
            break;
        }
    }
    state->mappings.erase(mapping);
    return CUDA_SUCCESS;
}

CUresult CUDAAPI cuvidCtxLockCreate(CUvideoctxlock *lock, CUcontext context) {
    if (!lock) return CUDA_ERROR_INVALID_VALUE;
    try {
        *lock = reinterpret_cast<CUvideoctxlock>(new ContextLock(context));
        return CUDA_SUCCESS;
    } catch (const std::bad_alloc &) {
        *lock = nullptr;
        return CUDA_ERROR_OUT_OF_MEMORY;
    }
}

CUresult CUDAAPI cuvidCtxLockDestroy(CUvideoctxlock lock) {
    if (!lock) return CUDA_ERROR_INVALID_VALUE;
    delete reinterpret_cast<ContextLock *>(lock);
    return CUDA_SUCCESS;
}

CUresult CUDAAPI cuvidCtxLock(CUvideoctxlock lock, unsigned int) {
    if (!lock) return CUDA_ERROR_INVALID_VALUE;
    reinterpret_cast<ContextLock *>(lock)->mutex.lock();
    return CUDA_SUCCESS;
}

CUresult CUDAAPI cuvidCtxUnlock(CUvideoctxlock lock, unsigned int) {
    if (!lock) return CUDA_ERROR_INVALID_VALUE;
    reinterpret_cast<ContextLock *>(lock)->mutex.unlock();
    return CUDA_SUCCESS;
}

CUresult CUDAAPI cuvidCreateVideoSource(CUvideosource *source, const char *, CUVIDSOURCEPARAMS *) {
    if (!source) return CUDA_ERROR_INVALID_VALUE;
    *source = nullptr;
    return unsupported();
}

CUresult CUDAAPI cuvidCreateVideoSourceW(CUvideosource *source, const wchar_t *, CUVIDSOURCEPARAMS *) {
    if (!source) return CUDA_ERROR_INVALID_VALUE;
    *source = nullptr;
    return unsupported();
}

CUresult CUDAAPI cuvidDestroyVideoSource(CUvideosource) { return unsupported(); }
CUresult CUDAAPI cuvidSetVideoSourceState(CUvideosource, cudaVideoState) { return unsupported(); }
cudaVideoState CUDAAPI cuvidGetVideoSourceState(CUvideosource obj) {
    return obj ? cudaVideoState_Started : cudaVideoState_Error;
}
CUresult CUDAAPI cuvidGetSourceVideoFormat(CUvideosource, CUVIDEOFORMAT *, unsigned int) {
    return unsupported();
}
CUresult CUDAAPI cuvidGetSourceAudioFormat(CUvideosource, CUAUDIOFORMAT *, unsigned int) {
    return unsupported();
}

CUresult CUDAAPI cuvidCreateVideoParser(CUvideoparser *parser, CUVIDPARSERPARAMS *params) {
    if (!parser || !params) return CUDA_ERROR_INVALID_VALUE;
    *parser = nullptr;
    try {
        auto state = std::make_shared<ParserState>(*params);
        CUvideoparser new_handle = reinterpret_cast<CUvideoparser>(state.get());
        {
            std::lock_guard<std::mutex> guard(parsers_mutex);
            parsers.emplace(new_handle, state);
        }
        *parser = new_handle;
        return CUDA_SUCCESS;
    } catch (const std::bad_alloc &) {
        return CUDA_ERROR_OUT_OF_MEMORY;
    }
}

CUresult CUDAAPI cuvidParseVideoData(CUvideoparser parser_handle, CUVIDSOURCEDATAPACKET *packet) {
    auto state = get_parser(parser_handle);
    if (!state || !packet) return CUDA_ERROR_INVALID_VALUE;
    std::lock_guard<std::mutex> guard(state->mutex);
    return state->parser.parse(*packet);
}

CUresult CUDAAPI cuvidDestroyVideoParser(CUvideoparser parser_handle) {
    if (!parser_handle) return CUDA_ERROR_INVALID_VALUE;
    std::shared_ptr<ParserState> state;
    {
        std::lock_guard<std::mutex> guard(parsers_mutex);
        auto it = parsers.find(parser_handle);
        if (it == parsers.end()) return CUDA_ERROR_INVALID_VALUE;
        state = it->second;
        parsers.erase(it);
    }
    std::lock_guard<std::mutex> guard(state->mutex);
    return CUDA_SUCCESS;
}

} // extern "C"
