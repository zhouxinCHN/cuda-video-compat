#ifndef CUDA_SHIM_CUVID_PARSER_H
#define CUDA_SHIM_CUVID_PARSER_H

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>
#include <ffnvcodec/dynlink_nvcuvid.h>

#include <cstdint>
#include <vector>

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
    unsigned int next_picture_index_ = 0;

    struct PendingPicture {
        int picture_index = -1;
        int64_t timestamp = 0;
    };
    std::vector<PendingPicture> display_queue_;

    // Byte buffer accumulated across packets.
    std::vector<uint8_t> buffer_;

    // DPB (Decoded Picture Buffer) state for P/B frame support.
    struct DpbEntry {
        int picture_index = -1;
        int frame_num = 0;
        int poc = 0;
        bool is_long_term = false;
        bool is_reference = false;
        int64_t timestamp = 0;
    };
    std::vector<DpbEntry> dpb_;
    int prev_frame_num_ = 0;
    int prev_poc_lsb_ = 0;
    int prev_poc_msb_ = 0;

    // Stashed SPS/PPS NAL bytes, forwarded to VkSequenceCache once both are seen.
    // This is the seam between the CUVID parser (which sees SPS/PPS) and the
    // Vulkan backend (which needs them to build its session). See docs/architecture/vulkan-backend.md.
    std::vector<uint8_t> pending_sps_;
    std::vector<uint8_t> pending_pps_;

    // Internal helpers.
    CUresult parse_packet_nal(int64_t timestamp);
    CUresult send_sequence();
    CUresult parse_slice_header(const uint8_t *nal_data, std::size_t nal_size,
                                int *frame_num, int *poc, bool *is_reference);
    CUresult send_picture(const uint8_t *nal_data, std::size_t nal_size,
                          int64_t timestamp, int frame_num, int poc,
                          bool is_reference, bool is_idr);
    void update_dpb(int picture_index, int frame_num, int poc, bool is_reference, int64_t timestamp);
    void build_reference_lists(const CUVIDPICPARAMS &pic, std::vector<int> &list0, std::vector<int> &list1);
    void pump_display_queue();
};

} // namespace cuvid_shim

#endif // CUDA_SHIM_CUVID_PARSER_H
