#ifndef CUDA_SHIM_H264_DPB_ADAPTER_H
#define CUDA_SHIM_H264_DPB_ADAPTER_H

// Converts FFmpeg/CUVID syntax into the backend-independent H264Dpb model.
// This layer parses only fields needed for POC, reference lists and DPB marking;
// VAAPI/Vulkan translators continue to parse their codec-specific parameters.

#include "h264_dpb.h"

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>

extern "C" {
#define HAVE_AV_CONFIG_H 1
#include "libavcodec/get_bits.h"
#include "libavcodec/h264_ps.h"
}

namespace cuvid_shim {

struct H264SliceParseParams {
    uint32_t log2_max_frame_num = 4;
    uint32_t log2_max_poc_lsb = 4;
    uint32_t poc_type = 0;
    bool delta_pic_order_always_zero = false;
    bool frame_mbs_only = true;
    bool mbaff = false;
    bool pic_order_present = false;
    bool redundant_pic_cnt_present = false;
    bool weighted_pred = false;
    uint32_t weighted_bipred_idc = 0;
    uint32_t chroma_format_idc = 1;
    uint32_t default_ref_count[2] = {1, 1};

    bool has_poc_override = false;
    int32_t poc_override[2] = {0, 0};
};

// max_dpb_frames is explicit because CUVID lacks level/DPB-capacity fields.
H264DpbStatus h264_sequence_from_ffmpeg(const SPS &sps, uint32_t max_dpb_frames,
                                        H264Sequence *sequence);
H264DpbStatus h264_sequence_from_cuvid(const CUVIDH264PICPARAMS &h264,
                                       uint32_t max_dpb_frames,
                                       H264Sequence *sequence);

// Uses max_dec_frame_buffering when present, otherwise FFmpeg's level-derived
// num_reorder_frames/ref_frame_count. The result is clamped to 1..16.
uint32_t h264_dpb_capacity_from_ffmpeg(const SPS &sps);

H264DpbStatus h264_slice_params_from_ffmpeg(const SPS &sps, const PPS &pps,
                                            H264SliceParseParams *params);
H264DpbStatus h264_slice_params_from_cuvid(const CUVIDH264PICPARAMS &h264,
                                           H264SliceParseParams *params);

// `gb` starts at first_mb_in_slice (after the NAL header and emulation-prevention
// removal). It is passed by value, so the caller's bit position is unchanged.
// The backing buffer must have AV_INPUT_BUFFER_PADDING_SIZE readable padding
// bytes, as required by FFmpeg's GetBitContext readers.
H264DpbStatus parse_h264_slice_header(GetBitContext gb, int nal_type,
                                      int nal_ref_idc,
                                      const H264SliceParseParams &params,
                                      H264Slice *slice);

} // namespace cuvid_shim

#endif // CUDA_SHIM_H264_DPB_ADAPTER_H
