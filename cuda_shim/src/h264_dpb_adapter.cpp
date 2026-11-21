#include "h264_dpb_adapter.h"

#include <algorithm>
#include <limits>

extern "C" {
#include "libavcodec/golomb.h"
}

namespace cuvid_shim {
namespace {

class SliceBits {
public:
    explicit SliceBits(GetBitContext gb) : gb_(gb) {}

    bool bit(uint32_t *value) { return bits(1, value); }

    bool bits(unsigned int count, uint32_t *value) {
        if (!value || count > 32 || get_bits_left(&gb_) < static_cast<int>(count)) {
            ok_ = false;
            return false;
        }
        *value = get_bits(&gb_, count);
        return true;
    }

    bool ue(uint32_t *value) {
        if (!value || get_bits_left(&gb_) < 1) {
            ok_ = false;
            return false;
        }
        *value = get_ue_golomb_long(&gb_);
        if (get_bits_left(&gb_) < 0) {
            ok_ = false;
            return false;
        }
        return true;
    }

    bool se(int32_t *value) {
        if (!value || get_bits_left(&gb_) < 1) {
            ok_ = false;
            return false;
        }
        *value = get_se_golomb(&gb_);
        if (get_bits_left(&gb_) < 0) {
            ok_ = false;
            return false;
        }
        return true;
    }

    bool ok() const { return ok_; }

private:
    GetBitContext gb_;
    bool ok_ = true;
};

H264DpbStatus validate_parse_params(const H264SliceParseParams &params) {
    if (params.log2_max_frame_num < 4 || params.log2_max_frame_num > 16 ||
        (params.poc_type == 0 &&
         (params.log2_max_poc_lsb < 4 || params.log2_max_poc_lsb > 16)) ||
        (params.poc_type != 0 && params.log2_max_poc_lsb > 16) ||
        params.poc_type > 2 || params.weighted_bipred_idc > 2 ||
        params.default_ref_count[0] > kH264MaxReferenceListSize ||
        params.default_ref_count[1] > kH264MaxReferenceListSize) {
        return H264DpbStatus::kInvalidArgument;
    }
    if (!params.frame_mbs_only || params.mbaff) return H264DpbStatus::kUnsupported;
    return H264DpbStatus::kOk;
}

bool skip_pred_weight_table(SliceBits *bits, const H264Slice &slice,
                            const H264SliceParseParams &params) {
    const bool has_table =
        (slice.type == H264SliceType::kP && params.weighted_pred) ||
        (slice.type == H264SliceType::kB && params.weighted_bipred_idc == 1);
    if (!has_table) return bits->ok();

    uint32_t value = 0;
    int32_t signed_value = 0;
    if (!bits->ue(&value) || value > 7) return false;
    if (params.chroma_format_idc != 0 && (!bits->ue(&value) || value > 7)) return false;

    const int list_count = slice.type == H264SliceType::kB ? 2 : 1;
    for (int list = 0; list < list_count; ++list) {
        for (uint32_t index = 0; index < slice.num_ref_idx_active[list]; ++index) {
            if (!bits->bit(&value)) return false;
            if (value && (!bits->se(&signed_value) || !bits->se(&signed_value))) return false;
            if (params.chroma_format_idc != 0) {
                // One chroma_weight_lX_flag covers both Cb and Cr. When set,
                // two weight/offset pairs follow; there is no second flag.
                if (!bits->bit(&value)) return false;
                if (value) {
                    for (int component = 0; component < 2; ++component) {
                        if (!bits->se(&signed_value) || !bits->se(&signed_value)) {
                            return false;
                        }
                    }
                }
            }
        }
    }
    return bits->ok();
}

bool parse_ref_list_modifications(SliceBits *bits, H264Slice *slice) {
    const int list_count = slice->type == H264SliceType::kB ? 2 : 1;
    for (int list = 0; list < list_count; ++list) {
        uint32_t flag = 0;
        if (!bits->bit(&flag)) return false;
        if (!flag) continue;
        for (uint32_t index = 0;; ++index) {
            uint32_t idc = 0;
            if (!bits->ue(&idc)) return false;
            if (idc == 3) break;
            // The terminating entry may follow at most active-count operations.
            if (idc > 2 || index >= slice->num_ref_idx_active[list]) return false;
            H264RefListModification modification;
            modification.idc = idc;
            if (idc < 2) {
                if (!bits->ue(&modification.abs_diff_pic_num_minus1)) return false;
            } else if (!bits->ue(&modification.long_term_pic_num)) {
                return false;
            }
            slice->modifications[list].push_back(modification);
        }
    }
    return bits->ok();
}

bool parse_mmco(SliceBits *bits, H264Slice *slice) {
    if (!slice->is_reference) return bits->ok();
    uint32_t value = 0;
    if (slice->is_idr) {
        if (!bits->bit(&value)) return false;
        slice->no_output_of_prior_pics = value != 0;
        if (!bits->bit(&value)) return false;
        slice->long_term_reference_flag = value != 0;
        return bits->ok();
    }

    if (!bits->bit(&value)) return false;
    slice->adaptive_ref_pic_marking = value != 0;
    if (!slice->adaptive_ref_pic_marking) return bits->ok();

    // rocDecode's corrected bound: at most 66 operations plus the terminator.
    constexpr std::size_t kMaxMmcoEntries = 67;
    for (std::size_t index = 0; index < kMaxMmcoEntries; ++index) {
        uint32_t opcode = 0;
        if (!bits->ue(&opcode)) return false;
        if (opcode == 0) return bits->ok();
        if (opcode > 6 || index + 1 == kMaxMmcoEntries) return false;

        H264Mmco operation;
        operation.opcode = opcode;
        switch (opcode) {
        case 1:
            if (!bits->ue(&operation.difference_of_pic_nums_minus1)) return false;
            break;
        case 2:
            if (!bits->ue(&operation.long_term_pic_num)) return false;
            break;
        case 3:
            if (!bits->ue(&operation.difference_of_pic_nums_minus1) ||
                !bits->ue(&operation.long_term_frame_idx)) {
                return false;
            }
            break;
        case 4:
            if (!bits->ue(&operation.max_long_term_frame_idx_plus1)) return false;
            break;
        case 5:
            break;
        case 6:
            if (!bits->ue(&operation.long_term_frame_idx)) return false;
            break;
        default:
            return false;
        }
        slice->mmco.push_back(operation);
    }
    return false;
}

} // namespace

H264DpbStatus h264_sequence_from_ffmpeg(const SPS &sps, uint32_t max_dpb_frames,
                                        H264Sequence *sequence) {
    if (!sequence || sps.log2_max_frame_num < 0 || sps.log2_max_poc_lsb < 0 ||
        sps.poc_cycle_length < 0 || sps.poc_cycle_length > 256 ||
        sps.ref_frame_count < 0) {
        return H264DpbStatus::kInvalidArgument;
    }
    H264Sequence converted;
    converted.log2_max_frame_num = static_cast<uint32_t>(sps.log2_max_frame_num);
    converted.log2_max_poc_lsb = static_cast<uint32_t>(sps.log2_max_poc_lsb);
    converted.poc_type = static_cast<uint32_t>(sps.poc_type);
    converted.offset_for_ref_frame.assign(
        sps.offset_for_ref_frame, sps.offset_for_ref_frame + sps.poc_cycle_length);
    converted.offset_for_non_ref_pic = sps.offset_for_non_ref_pic;
    converted.offset_for_top_to_bottom_field = sps.offset_for_top_to_bottom_field;
    converted.max_num_ref_frames = static_cast<uint32_t>(sps.ref_frame_count);
    converted.max_dpb_frames = max_dpb_frames;
    *sequence = std::move(converted);
    H264Dpb validator;
    return validator.reset(*sequence);
}

H264DpbStatus h264_sequence_from_cuvid(const CUVIDH264PICPARAMS &h264,
                                       uint32_t max_dpb_frames,
                                       H264Sequence *sequence) {
    if (!sequence || h264.log2_max_frame_num_minus4 < 0 ||
        (h264.pic_order_cnt_type == 0 && h264.log2_max_pic_order_cnt_lsb_minus4 < 0) ||
        (h264.pic_order_cnt_type != 0 && h264.log2_max_pic_order_cnt_lsb_minus4 < -4) ||
        h264.num_ref_frames < 0) {
        return H264DpbStatus::kInvalidArgument;
    }
    H264Sequence converted;
    converted.log2_max_frame_num = static_cast<uint32_t>(h264.log2_max_frame_num_minus4 + 4);
    converted.log2_max_poc_lsb = h264.pic_order_cnt_type == 0
        ? static_cast<uint32_t>(h264.log2_max_pic_order_cnt_lsb_minus4 + 4)
        : 0;
    converted.poc_type = static_cast<uint32_t>(h264.pic_order_cnt_type);
    // CUVID lacks POC1 cycle offsets; parse params carry a POC override instead.
    converted.max_num_ref_frames = static_cast<uint32_t>(h264.num_ref_frames);
    converted.max_dpb_frames = max_dpb_frames;
    *sequence = std::move(converted);
    H264Dpb validator;
    return validator.reset(*sequence);
}

uint32_t h264_dpb_capacity_from_ffmpeg(const SPS &sps) {
    int capacity = sps.max_dec_frame_buffering;
    if (capacity <= 0) capacity = std::max(sps.num_reorder_frames, sps.ref_frame_count);
    return static_cast<uint32_t>(std::clamp(capacity, 1,
                                            static_cast<int>(kH264MaxReferenceFrames)));
}

H264DpbStatus h264_slice_params_from_ffmpeg(const SPS &sps, const PPS &pps,
                                            H264SliceParseParams *params) {
    if (!params || sps.log2_max_frame_num < 0 || sps.log2_max_poc_lsb < 0 ||
        pps.ref_count[0] > kH264MaxReferenceListSize ||
        pps.ref_count[1] > kH264MaxReferenceListSize) {
        return H264DpbStatus::kInvalidArgument;
    }
    H264SliceParseParams converted;
    converted.log2_max_frame_num = static_cast<uint32_t>(sps.log2_max_frame_num);
    converted.log2_max_poc_lsb = static_cast<uint32_t>(sps.log2_max_poc_lsb);
    converted.poc_type = static_cast<uint32_t>(sps.poc_type);
    converted.delta_pic_order_always_zero = sps.delta_pic_order_always_zero_flag != 0;
    converted.frame_mbs_only = sps.frame_mbs_only_flag != 0;
    converted.mbaff = sps.mb_aff != 0;
    converted.pic_order_present = pps.pic_order_present != 0;
    converted.redundant_pic_cnt_present = pps.redundant_pic_cnt_present != 0;
    converted.weighted_pred = pps.weighted_pred != 0;
    converted.weighted_bipred_idc = static_cast<uint32_t>(pps.weighted_bipred_idc);
    converted.chroma_format_idc = static_cast<uint32_t>(sps.chroma_format_idc);
    converted.default_ref_count[0] = pps.ref_count[0];
    converted.default_ref_count[1] = pps.ref_count[1];
    *params = converted;
    return validate_parse_params(*params);
}

H264DpbStatus h264_slice_params_from_cuvid(const CUVIDH264PICPARAMS &h264,
                                           H264SliceParseParams *params) {
    if (!params || h264.log2_max_frame_num_minus4 < 0 ||
        (h264.pic_order_cnt_type == 0 && h264.log2_max_pic_order_cnt_lsb_minus4 < 0) ||
        (h264.pic_order_cnt_type != 0 && h264.log2_max_pic_order_cnt_lsb_minus4 < -4) ||
        h264.num_ref_idx_l0_active_minus1 < -1 || h264.num_ref_idx_l0_active_minus1 > 31 ||
        h264.num_ref_idx_l1_active_minus1 < -1 || h264.num_ref_idx_l1_active_minus1 > 31 ||
        h264.weighted_bipred_idc < 0) {
        return H264DpbStatus::kInvalidArgument;
    }
    H264SliceParseParams converted;
    converted.log2_max_frame_num = static_cast<uint32_t>(h264.log2_max_frame_num_minus4 + 4);
    converted.log2_max_poc_lsb = h264.pic_order_cnt_type == 0
        ? static_cast<uint32_t>(h264.log2_max_pic_order_cnt_lsb_minus4 + 4)
        : 0;
    converted.poc_type = static_cast<uint32_t>(h264.pic_order_cnt_type);
    converted.delta_pic_order_always_zero = h264.delta_pic_order_always_zero_flag != 0;
    converted.frame_mbs_only = h264.frame_mbs_only_flag != 0;
    converted.mbaff = h264.MbaffFrameFlag != 0;
    converted.pic_order_present = h264.pic_order_present_flag != 0;
    converted.redundant_pic_cnt_present = h264.redundant_pic_cnt_present_flag != 0;
    converted.weighted_pred = h264.weighted_pred_flag != 0;
    converted.weighted_bipred_idc = static_cast<uint32_t>(h264.weighted_bipred_idc);
    // The P0 CUVID path is NV12/4:2:0, so chroma weights are present.
    converted.chroma_format_idc = 1;
    converted.default_ref_count[0] =
        static_cast<uint32_t>(h264.num_ref_idx_l0_active_minus1 + 1);
    converted.default_ref_count[1] =
        static_cast<uint32_t>(h264.num_ref_idx_l1_active_minus1 + 1);
    converted.has_poc_override = true;
    converted.poc_override[0] = h264.CurrFieldOrderCnt[0];
    converted.poc_override[1] = h264.CurrFieldOrderCnt[1];
    *params = converted;
    return validate_parse_params(*params);
}

H264DpbStatus parse_h264_slice_header(GetBitContext gb, int nal_type,
                                      int nal_ref_idc,
                                      const H264SliceParseParams &params,
                                      H264Slice *slice) {
    if (!slice || (nal_type != 1 && nal_type != 5) || nal_ref_idc < 0 ||
        nal_ref_idc > 3) {
        return H264DpbStatus::kInvalidArgument;
    }
    const H264DpbStatus valid = validate_parse_params(params);
    if (valid != H264DpbStatus::kOk) return valid;

    H264Slice parsed;
    parsed.is_idr = nal_type == 5;
    parsed.is_reference = nal_ref_idc != 0;
    parsed.has_poc_override = params.has_poc_override;
    parsed.poc_override[0] = params.poc_override[0];
    parsed.poc_override[1] = params.poc_override[1];
    SliceBits bits(gb);
    uint32_t value = 0;

    if (!bits.ue(&value)) return H264DpbStatus::kInvalidArgument; // first_mb_in_slice
    if (!bits.ue(&value) || value > 9) return H264DpbStatus::kInvalidArgument;
    value %= 5;
    if (value == 0) {
        parsed.type = H264SliceType::kP;
    } else if (value == 1) {
        parsed.type = H264SliceType::kB;
    } else if (value == 2) {
        parsed.type = H264SliceType::kI;
    } else {
        return H264DpbStatus::kUnsupported; // SP/SI
    }
    if (!bits.ue(&value)) return H264DpbStatus::kInvalidArgument; // pps_id
    if (!bits.bits(params.log2_max_frame_num, &value)) {
        return H264DpbStatus::kInvalidArgument;
    }
    parsed.frame_num = value;
    if (parsed.is_idr && !bits.ue(&parsed.idr_pic_id)) {
        return H264DpbStatus::kInvalidArgument;
    }

    if (params.poc_type == 0) {
        if (!bits.bits(params.log2_max_poc_lsb, &parsed.poc_lsb)) {
            return H264DpbStatus::kInvalidArgument;
        }
        if (params.pic_order_present && !bits.se(&parsed.delta_poc_bottom)) {
            return H264DpbStatus::kInvalidArgument;
        }
    } else if (params.poc_type == 1) {
        if (!params.delta_pic_order_always_zero) {
            if (!bits.se(&parsed.delta_poc[0])) return H264DpbStatus::kInvalidArgument;
            if (params.pic_order_present && !bits.se(&parsed.delta_poc[1])) {
                return H264DpbStatus::kInvalidArgument;
            }
        }
    }

    if (params.redundant_pic_cnt_present && !bits.ue(&value)) {
        return H264DpbStatus::kInvalidArgument;
    }
    if (parsed.type == H264SliceType::kB && !bits.bit(&value)) {
        return H264DpbStatus::kInvalidArgument;
    }

    if (parsed.type != H264SliceType::kI) {
        const int list_count = parsed.type == H264SliceType::kB ? 2 : 1;
        for (int list = 0; list < list_count; ++list) {
            parsed.num_ref_idx_active[list] = params.default_ref_count[list];
        }
        if (!bits.bit(&value)) return H264DpbStatus::kInvalidArgument;
        if (value) {
            for (int list = 0; list < list_count; ++list) {
                if (!bits.ue(&value) || value >= kH264MaxReferenceListSize) {
                    return H264DpbStatus::kInvalidArgument;
                }
                parsed.num_ref_idx_active[list] = value + 1;
            }
        }
        if (!parse_ref_list_modifications(&bits, &parsed)) {
            return H264DpbStatus::kInvalidArgument;
        }
        if (!skip_pred_weight_table(&bits, parsed, params)) {
            return H264DpbStatus::kInvalidArgument;
        }
    }

    if (!parse_mmco(&bits, &parsed)) return H264DpbStatus::kInvalidArgument;
    if (!bits.ok()) return H264DpbStatus::kInvalidArgument;
    *slice = std::move(parsed);
    return H264DpbStatus::kOk;
}

} // namespace cuvid_shim
