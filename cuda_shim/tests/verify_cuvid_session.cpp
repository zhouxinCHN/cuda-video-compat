// Smoke-test CUVID capability and VAAPI session creation.
// This does not submit a picture or validate decoded pixels.

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>

#include <cstdio>

extern "C" {
CUresult CUDAAPI cuvidGetDecoderCaps(CUVIDDECODECAPS *caps);
CUresult CUDAAPI cuvidCreateDecoder(CUvideodecoder *decoder, CUVIDDECODECREATEINFO *info);
CUresult CUDAAPI cuvidDestroyDecoder(CUvideodecoder decoder);
}

int main() {
    CUVIDDECODECAPS caps = {};
    caps.eCodecType = cudaVideoCodec_H264;
    caps.eChromaFormat = cudaVideoChromaFormat_420;
    caps.nBitDepthMinus8 = 0;
    if (cuvidGetDecoderCaps(&caps) != CUDA_SUCCESS || !caps.bIsSupported) {
        std::fprintf(stderr, "FAIL: H.264/NV12 capability query\n");
        return 1;
    }

    CUVIDDECODECREATEINFO info = {};
    info.ulWidth = 320;
    info.ulHeight = 240;
    info.ulNumDecodeSurfaces = 4;
    info.CodecType = cudaVideoCodec_H264;
    info.ChromaFormat = cudaVideoChromaFormat_420;
    info.bitDepthMinus8 = 0;
    info.OutputFormat = cudaVideoSurfaceFormat_NV12;
    info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    info.ulTargetWidth = 320;
    info.ulTargetHeight = 240;
    info.ulNumOutputSurfaces = 1;
    info.ulMaxWidth = 320;
    info.ulMaxHeight = 240;

    CUvideodecoder decoder = nullptr;
    CUresult result = cuvidCreateDecoder(&decoder, &info);
    if (result != CUDA_SUCCESS || !decoder) {
        std::fprintf(stderr, "FAIL: cuvidCreateDecoder, result=%d\n", result);
        return 1;
    }
    if (cuvidDestroyDecoder(decoder) != CUDA_SUCCESS) {
        std::fprintf(stderr, "FAIL: cuvidDestroyDecoder\n");
        return 1;
    }
    std::puts("PASS: CUVID capability and VAAPI LINEAR NV12 session (no pixel validation)");
    return 0;
}
