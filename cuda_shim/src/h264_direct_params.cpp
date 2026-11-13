#include "h264_direct_params.h"

#include <cstring>
#include <limits>
#include <utility>

namespace cuvid_shim {
namespace {

class BitWriter {
public:
    void bit(unsigned int value) {
        if (bit_position_ == 0) bytes_.push_back(0);
        if (value & 1U)
            bytes_.back() |= static_cast<uint8_t>(0x80U >> bit_position_);
        bit_position_ = (bit_position_ + 1) & 7;
    }

    bool bits(unsigned int count, uint32_t value) {
        if (count > 32) return false;
        for (unsigned int i = count; i > 0; --i) bit((value >> (i - 1)) & 1U);
        return true;
    }

    bool ue(uint32_t value) {
        if (value == std::numeric_limits<uint32_t>::max()) return false;
        const uint32_t code = value + 1;
        unsigned int width = 0;
        for (uint32_t v = code; v > 1; v >>= 1) ++width;
        if (width > 31) return false;
        for (unsigned int i = 0; i < width; ++i) bit(0);
        return bits(width + 1, code);
    }

    bool se(int32_t value) {
        // H.264 9.1: 0,-1,1,-2,2... -> 0,1,2,3,4...
        const int64_t code = value <= 0
            ? static_cast<int64_t>(-2) * value
            : static_cast<int64_t>(2) * value - 1;
        if (code < 0 || code >= std::numeric_limits<uint32_t>::max()) return false;
        return ue(static_cast<uint32_t>(code));
    }

    void rbsp_trailing_bits() {
        bit(1);
        while (bit_position_ != 0) bit(0);
    }

    const std::vector<uint8_t> &rbsp() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    unsigned int bit_position_ = 0;
};

bool all_default_scaling(const CUVIDH264PICPARAMS &h264) {
    for (const auto &matrix : h264.WeightScale4x4)
        for (uint8_t value : matrix)
            if (value != 16) return false;
    for (const auto &matrix : h264.WeightScale8x8)
        for (uint8_t value : matrix)
            if (value != 16) return false;
    return true;
}

int infer_profile_idc(const CUVIDH264PICPARAMS &h264) {
    if (h264.transform_8x8_mode_flag || h264.qpprime_y_zero_transform_bypass_flag)
        return 100; // High
    if (h264.entropy_coding_mode_flag) return 77; // Main
    return 66;                                    // Baseline
}

int infer_level_idc(int width_mbs, int height_mbs) {
    const int mb_count = width_mbs * height_mbs;
    if (mb_count <= 8192) return 40;  // 1080p and below
    if (mb_count <= 22080) return 50; // up to 4096x2304
    return 51;
}

std::vector<uint8_t> annex_b_nal(unsigned int nal_ref_idc, unsigned int nal_type,
                                 const std::vector<uint8_t> &rbsp) {
    std::vector<uint8_t> nal;
    nal.reserve(rbsp.size() + 8);
    nal.insert(nal.end(), {0x00, 0x00, 0x00, 0x01});
    nal.push_back(static_cast<uint8_t>((nal_ref_idc << 5) | nal_type));

    // Emulation prevention: insert 0x03 after any 00 00 followed by <= 03.
    unsigned int zeros = 0;
    for (uint8_t byte : rbsp) {
        if (zeros >= 2 && byte <= 3) {
            nal.push_back(0x03);
            zeros = 0;
        }
        nal.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return nal;
}

bool build_sps(const CUVIDPICPARAMS &picture, std::vector<uint8_t> *out) {
    const auto &h264 = picture.CodecSpecific.h264;
    if (picture.PicWidthInMbs <= 0 || picture.FrameHeightInMbs <= 0 ||
        h264.bit_depth_luma_minus8 != 0 || h264.bit_depth_chroma_minus8 != 0 ||
        !h264.frame_mbs_only_flag || h264.MbaffFrameFlag ||
        picture.field_pic_flag || h264.fmo_aso_enable ||
        h264.residual_colour_transform_flag ||
        h264.log2_max_frame_num_minus4 < 0 || h264.log2_max_frame_num_minus4 > 12 ||
        h264.num_ref_frames < 0 || h264.num_ref_frames > 16 ||
        (h264.pic_order_cnt_type != 0 && h264.pic_order_cnt_type != 2)) {
        return false;
    }
    if (h264.pic_order_cnt_type == 0 &&
        (h264.log2_max_pic_order_cnt_lsb_minus4 < 0 ||
         h264.log2_max_pic_order_cnt_lsb_minus4 > 12)) {
        return false;
    }
    if (!all_default_scaling(h264)) {
        // The ABI carries the matrices, but their scan order differs across
        // host implementations. Do not silently generate a wrong list.
        return false;
    }

    const int profile_idc = infer_profile_idc(h264);
    const int level_idc = infer_level_idc(picture.PicWidthInMbs, picture.FrameHeightInMbs);
    BitWriter bits;
    if (!bits.bits(8, static_cast<uint32_t>(profile_idc)) ||
        !bits.bits(8, 0) || // constraint flags + reserved
        !bits.bits(8, static_cast<uint32_t>(level_idc)) ||
        !bits.ue(0)) { // seq_parameter_set_id
        return false;
    }

    if (profile_idc == 100) {
        if (!bits.ue(1) || // chroma_format_idc = 4:2:0
            !bits.ue(static_cast<uint32_t>(h264.bit_depth_luma_minus8)) ||
            !bits.ue(static_cast<uint32_t>(h264.bit_depth_chroma_minus8)) ||
            !bits.bits(1, h264.qpprime_y_zero_transform_bypass_flag ? 1 : 0) ||
            !bits.bits(1, 0)) { // seq_scaling_matrix_present_flag
            return false;
        }
    }

    if (!bits.ue(static_cast<uint32_t>(h264.log2_max_frame_num_minus4)) ||
        !bits.ue(static_cast<uint32_t>(h264.pic_order_cnt_type))) {
        return false;
    }
    if (h264.pic_order_cnt_type == 0) {
        if (!bits.ue(static_cast<uint32_t>(h264.log2_max_pic_order_cnt_lsb_minus4)))
            return false;
    }

    if (!bits.ue(static_cast<uint32_t>(h264.num_ref_frames)) ||
        !bits.bits(1, 0) || // gaps_in_frame_num_value_allowed_flag
        !bits.ue(static_cast<uint32_t>(picture.PicWidthInMbs - 1)) ||
        !bits.ue(static_cast<uint32_t>(picture.FrameHeightInMbs - 1)) ||
        !bits.bits(1, 1) || // frame_mbs_only_flag
        !bits.bits(1, h264.direct_8x8_inference_flag ? 1 : 0) ||
        !bits.bits(1, 0) || // frame_cropping_flag (not exposed by CUVID)
        !bits.bits(1, 0)) { // vui_parameters_present_flag (not exposed by CUVID)
        return false;
    }
    bits.rbsp_trailing_bits();
    *out = annex_b_nal(3, 7, bits.rbsp());
    return !out->empty();
}

bool build_pps(const CUVIDPICPARAMS &picture, std::vector<uint8_t> *out) {
    const auto &h264 = picture.CodecSpecific.h264;
    if (h264.num_ref_idx_l0_active_minus1 < 0 ||
        h264.num_ref_idx_l0_active_minus1 > 31 ||
        h264.num_ref_idx_l1_active_minus1 < 0 ||
        h264.num_ref_idx_l1_active_minus1 > 31 ||
        h264.weighted_bipred_idc < 0 || h264.weighted_bipred_idc > 2 ||
        h264.pic_init_qp_minus26 < -26 || h264.pic_init_qp_minus26 > 25 ||
        h264.chroma_qp_index_offset < -12 || h264.chroma_qp_index_offset > 12 ||
        h264.second_chroma_qp_index_offset < -12 ||
        h264.second_chroma_qp_index_offset > 12) {
        return false;
    }

    BitWriter bits;
    if (!bits.ue(0) || // pic_parameter_set_id
        !bits.ue(0) || // seq_parameter_set_id
        !bits.bits(1, h264.entropy_coding_mode_flag ? 1 : 0) ||
        !bits.bits(1, h264.pic_order_present_flag ? 1 : 0) ||
        !bits.ue(0) || // num_slice_groups_minus1
        !bits.ue(static_cast<uint32_t>(h264.num_ref_idx_l0_active_minus1)) ||
        !bits.ue(static_cast<uint32_t>(h264.num_ref_idx_l1_active_minus1)) ||
        !bits.bits(1, h264.weighted_pred_flag ? 1 : 0) ||
        !bits.bits(2, static_cast<uint32_t>(h264.weighted_bipred_idc)) ||
        !bits.se(h264.pic_init_qp_minus26) ||
        !bits.se(picture.CodecSpecific.h264.pic_init_qs_minus26) ||
        !bits.se(h264.chroma_qp_index_offset) ||
        !bits.bits(1, h264.deblocking_filter_control_present_flag ? 1 : 0) ||
        !bits.bits(1, h264.constrained_intra_pred_flag ? 1 : 0) ||
        !bits.bits(1, h264.redundant_pic_cnt_present_flag ? 1 : 0) ||
        !bits.bits(1, h264.transform_8x8_mode_flag ? 1 : 0) ||
        !bits.bits(1, 0) || // pic_scaling_matrix_present_flag
        !bits.se(h264.second_chroma_qp_index_offset)) {
        return false;
    }
    bits.rbsp_trailing_bits();
    *out = annex_b_nal(3, 8, bits.rbsp());
    return !out->empty();
}

} // namespace

bool synthesize_h264_parameter_sets(const CUVIDPICPARAMS &picture,
                                    std::vector<uint8_t> *sps_annex_b,
                                    std::vector<uint8_t> *pps_annex_b) {
    if (!sps_annex_b || !pps_annex_b) return false;
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    if (!build_sps(picture, &sps) || !build_pps(picture, &pps)) return false;
    *sps_annex_b = std::move(sps);
    *pps_annex_b = std::move(pps);
    return true;
}

} // namespace cuvid_shim
