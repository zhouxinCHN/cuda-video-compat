#ifndef CUDA_SHIM_CUVID_PARSER_H
#define CUDA_SHIM_CUVID_PARSER_H

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>
#include <ffnvcodec/dynlink_nvcuvid.h>

#include <cstdint>
#include <vector>

#include "h264_dpb_adapter.h"
#include "vk_sequence_cache.h"

// FFmpeg internal H.264 parameter-set types.
extern "C" {
#define HAVE_AV_CONFIG_H 1
#include "libavcodec/avcodec.h"
#include "libavcodec/h2645_parse.h"
#include "libavcodec/h264_ps.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/codec_id.h"
}

namespace cuvid_shim {

class CuvidParser {
public:
    CuvidParser(const CUVIDPARSERPARAMS &params);
    ~CuvidParser();

    CuvidParser(const CuvidParser &) = delete;
    CuvidParser &operator=(const CuvidParser &) = delete;

    // Parse an incoming data packet. payload_size == 0 triggers flush.
    CUresult parse(const CUVIDSOURCEDATAPACKET &packet);

private:
    CUVIDPARSERPARAMS params_;

    // FFmpeg parameter-set state.
    AVCodecContext avctx_ = {};
    H264ParamSets ps_ = {};
    const SPS *sps_ = nullptr;
    const PPS *pps_ = nullptr;
    bool ps_ready_ = false;
    bool sequence_sent_ = false;
    unsigned int num_decode_surfaces_ = 0;

    // Shared backend-independent POC/DPB/output state.
    H264Dpb dpb_;
    bool dpb_configured_ = false;

    // Byte buffer accumulated across packets.
    std::vector<uint8_t> buffer_;

    // Slices of the picture currently being assembled. The first slice supplies
    // picture-level POC/MMCO syntax; every slice remains in the raw bitstream.
    struct PendingSlicePicture {
        int64_t timestamp = 0;
        H264Slice syntax = {};
    };
    std::vector<uint8_t> slice_buffer_;
    std::vector<uint32_t> slice_offsets_;
    PendingSlicePicture pending_picture_;
    bool pending_picture_active_ = false;

    // Stashed SPS/PPS NAL bytes, forwarded to VkSequenceCache once both are seen.
    std::vector<uint8_t> pending_sps_;
    std::vector<uint8_t> pending_pps_;

    CUresult parse_packet_nal(int64_t timestamp);
    CUresult send_sequence();
    CUresult configure_dpb();
    CUresult parse_slice_header(const H2645NAL &nal, H264Slice *slice, int *first_mb);
    CUresult flush_picture();
    CUresult send_picture(const uint8_t *nal_data, std::size_t nal_size);
    CUresult emit_outputs(bool flush);
};

} // namespace cuvid_shim

#endif // CUDA_SHIM_CUVID_PARSER_H
