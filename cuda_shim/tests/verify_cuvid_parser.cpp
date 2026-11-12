// Verify the CUVID parser chain against a real Annex-B H.264 stream.
// Parsing only; does not submit to VAAPI. The decode callback records the
// CUVIDPICPARAMS contents so the caller can assert real SPS-derived fields.

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>
#include <ffnvcodec/dynlink_nvcuvid.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

extern "C" {
CUresult CUDAAPI cuvidCreateVideoParser(CUvideoparser *parser, CUVIDPARSERPARAMS *params);
CUresult CUDAAPI cuvidParseVideoData(CUvideoparser parser, CUVIDSOURCEDATAPACKET *packet);
CUresult CUDAAPI cuvidDestroyVideoParser(CUvideoparser parser);
}

static int sequence_count = 0;
static int decode_count = 0;
static int display_count = 0;
static int sequence_width = 0;
static int sequence_height = 0;
static int decode_mbs_w = 0;
static int decode_mbs_h = 0;
static unsigned int decode_bitstream_len = 0;
static int decode_intra = 0;
static int decode_intra_first = -1;
static int decode_num_slices = 0;
static int decode_bit_depth = -1;
static int decode_chroma = -1;

int CUDAAPI cb_sequence(void *, CUVIDEOFORMAT *format) {
    ++sequence_count;
    sequence_width = format->coded_width;
    sequence_height = format->coded_height;
    decode_chroma = format->chroma_format;
    decode_bit_depth = format->bit_depth_luma_minus8;
    std::printf("  [cb_sequence] %dx%d chroma=%d bitdepth=%d\n",
                sequence_width, sequence_height, decode_chroma, decode_bit_depth);
    return 4; // decode surfaces requested by the host
}

int CUDAAPI cb_decode(void *, CUVIDPICPARAMS *pic) {
    ++decode_count;
    decode_mbs_w = pic->PicWidthInMbs;
    decode_mbs_h = pic->FrameHeightInMbs;
    decode_bitstream_len = pic->nBitstreamDataLen;
    decode_intra = pic->intra_pic_flag;
    if (decode_intra_first == -1) {
        decode_intra_first = decode_intra;
    }
    decode_num_slices = static_cast<int>(pic->nNumSlices);
    std::printf("  [cb_decode] #%d mbs=%dx%d len=%u intra=%d slices=%d\n",
                decode_count, decode_mbs_w, decode_mbs_h,
                decode_bitstream_len, decode_intra, decode_num_slices);
    return 1;
}

int CUDAAPI cb_display(void *, CUVIDPARSERDISPINFO *disp) {
    ++display_count;
    std::printf("  [cb_display] #%d pic_idx=%d ts=%lld\n",
                display_count, disp->picture_index, (long long)disp->timestamp);
    return 1;
}

static bool read_file(const char *path, std::vector<uint8_t> *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamsize n = in.tellg();
    in.seekg(0, std::ios::beg);
    out->resize(static_cast<std::size_t>(n));
    return in.read(reinterpret_cast<char *>(out->data()), n).gcount() == n;
}

int main(int argc, char **argv) {
    const char *stream = argc > 1 ? argv[1] : "../testdata/h264/test_320x240.h264";
    std::vector<uint8_t> data;
    if (!read_file(stream, &data)) {
        std::fprintf(stderr, "FAIL: cannot read %s\n", stream);
        return 2;
    }
    std::printf("=== CUVID parser test: %s (%zu bytes) ===\n", stream, data.size());

    CUVIDPARSERPARAMS params = {};
    params.CodecType = cudaVideoCodec_H264;
    params.ulMaxNumDecodeSurfaces = 4;
    params.ulMaxDisplayDelay = 0;
    params.pfnSequenceCallback = cb_sequence;
    params.pfnDecodePicture = cb_decode;
    params.pfnDisplayPicture = cb_display;

    CUvideoparser parser = nullptr;
    if (cuvidCreateVideoParser(&parser, &params) != CUDA_SUCCESS ||
        !parser) {
        std::fprintf(stderr, "FAIL: parser create\n");
        return 1;
    }

    // Feed in the entire file as one packet (Annex-B).
    CUVIDSOURCEDATAPACKET packet = {};
    packet.payload = data.data();
    packet.payload_size = data.size();
    packet.flags = CUVID_PKT_TIMESTAMP;
    packet.timestamp = 0;
    if (cuvidParseVideoData(parser, &packet) != CUDA_SUCCESS) {
        std::fprintf(stderr, "FAIL: parse\n");
        cuvidDestroyVideoParser(parser);
        return 1;
    }

    // EOS flush.
    CUVIDSOURCEDATAPACKET eos = {};
    eos.payload_size = 0;
    eos.flags = CUVID_PKT_ENDOFSTREAM;
    if (cuvidParseVideoData(parser, &eos) != CUDA_SUCCESS) {
        std::fprintf(stderr, "FAIL: EOS flush\n");
        cuvidDestroyVideoParser(parser);
        return 1;
    }

    cuvidDestroyVideoParser(parser);

    std::printf("\n=== results ===\n"
                "  sequence: %d decode: %d display: %d\n",
                sequence_count, decode_count, display_count);

    int fail = 0;
    if (sequence_count < 1) { std::fprintf(stderr, "FAIL: sequence never called\n"); fail = 1; }
    if (decode_count < 1) { std::fprintf(stderr, "FAIL: decode never called\n"); fail = 1; }
    if (sequence_width != 320 || sequence_height != 240) {
        std::fprintf(stderr, "FAIL: expected 320x240 got %dx%d\n", sequence_width, sequence_height);
        fail = 1;
    }
    if (decode_mbs_w != 20 || decode_mbs_h != 15) {
        std::fprintf(stderr, "FAIL: expected mbs 20x15 got %dx%d\n", decode_mbs_w, decode_mbs_h);
        fail = 1;
    }
    if (decode_bit_depth != 0) {
        std::fprintf(stderr, "FAIL: expected 8-bit (0) got %d\n", decode_bit_depth);
        fail = 1;
    }
    if (decode_chroma != cudaVideoChromaFormat_420) {
        std::fprintf(stderr, "FAIL: expected chroma 420 got %d\n", decode_chroma);
        fail = 1;
    }
    if (decode_intra_first != 1) {
        std::fprintf(stderr, "FAIL: expected first picture intra\n");
        fail = 1;
    }
    if (decode_bitstream_len == 0) {
        std::fprintf(stderr, "FAIL: empty bitstream\n");
        fail = 1;
    }

    if (fail) return 1;
    std::printf("\nPASS: CUVID parser chain (real SPS->PPS->slice->callbacks)\n");
    return 0;
}
