#include "h264_picture_translate.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace cuvid_shim {
namespace {

constexpr CUresult kNotSupported = static_cast<CUresult>(801);

class BitReader {
public:
    BitReader(const uint8_t *data, std::size_t size) : data_(data), size_(size) {}

    bool read_bit(unsigned int *value) {
        if (bit_position_ >= size_ * 8) return false;
        *value = (data_[bit_position_ / 8] >> (7 - bit_position_ % 8)) & 1U;
        ++bit_position_;
        return true;
    }

    bool read_bits(unsigned int count, uint32_t *value) {
        if (count > 32 || bit_position_ + count > size_ * 8) return false;
        *value = 0;
        for (unsigned int i = 0; i < count; ++i) {
            unsigned int bit = 0;
            if (!read_bit(&bit)) return false;
            *value = (*value << 1) | bit;
        }
        return true;
    }

    bool read_ue(uint32_t *value) {
        unsigned int zeros = 0;
        unsigned int bit = 0;
        while (read_bit(&bit) && bit == 0) {
            if (++zeros > 31) return false;
        }
        if (bit_position_ == 0 || bit != 1) return false;
        if (zeros == 0) {
            *value = 0;
            return true;
        }
        uint32_t suffix = 0;
        if (!read_bits(zeros, &suffix)) return false;
        *value = ((1U << zeros) - 1U) + suffix;
        return true;
    }

    bool read_se(int32_t *value) {
        uint32_t code = 0;
        if (!read_ue(&code)) return false;
        if (code & 1U) {
            *value = static_cast<int32_t>((code + 1U) / 2U);
        } else {
            *value = -static_cast<int32_t>(code / 2U);
        }
        return true;
    }

    std::size_t bit_position() const { return bit_position_; }

private:
    const uint8_t *data_;
    std::size_t size_;
    std::size_t bit_position_ = 0;
};

struct SliceHeader {
    int nal_ref_idc = 0;
    int nal_type = 0;
    int slice_type = 0;
    int first_mb = 0;
    int frame_num = 0;
    int direct_spatial = 0;
    int ref_count[2] = {0, 0};
    int cabac_init_idc = 0;
    int qp_delta = 0;
    int deblocking_idc = 0;
    int alpha_div2 = 0;
    int beta_div2 = 0;
    int bit_offset = 0;
    struct Modification {
        uint32_t operation = 3;
        uint32_t value = 0;
    };
    std::vector<Modification> modifications[2];
};

bool parse_slice_header(const uint8_t *data, std::size_t size,
                        const CUVIDPICPARAMS &picture, SliceHeader *output) {
    if (size < 4 || !data) return false;
    std::size_t start_code_size = 0;
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        start_code_size = 4;
    } else if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        start_code_size = 3;
    } else {
        return false;
    }
    if (start_code_size + 1 >= size) return false;

    const uint8_t nal_header = data[start_code_size];
    output->nal_ref_idc = (nal_header >> 5) & 3;
    output->nal_type = nal_header & 0x1f;
    if ((nal_header & 0x80) || (output->nal_type != 1 && output->nal_type != 5)) return false;

    std::vector<uint8_t> rbsp;
    rbsp.reserve(size - start_code_size - 1);
    unsigned int zeros = 0;
    for (std::size_t i = start_code_size + 1; i < size; ++i) {
        const uint8_t byte = data[i];
        if (zeros >= 2 && byte == 3) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }

    const auto &h264 = picture.CodecSpecific.h264;
    BitReader bits(rbsp.data(), rbsp.size());
    uint32_t value = 0;
    int32_t signed_value = 0;
    if (!bits.read_ue(&value) || value > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    output->first_mb = static_cast<int>(value);
    if (!bits.read_ue(&value)) return false;
    if (value > 9) return false;
    value %= 5;
    if (value == 0) output->slice_type = 0; // P
    else if (value == 1) output->slice_type = 1; // B
    else if (value == 2) output->slice_type = 2; // I
    else return false; // SP/SI are outside the P0 scope.

    if (!bits.read_ue(&value)) return false; // pic_parameter_set_id
    if (!bits.read_bits(h264.log2_max_frame_num_minus4 + 4, &value)) return false;
    output->frame_num = static_cast<int>(value);
    if (!h264.frame_mbs_only_flag || h264.MbaffFrameFlag || picture.field_pic_flag) return false;
    if (output->nal_type == 5 && !bits.read_ue(&value)) return false; // idr_pic_id

    if (h264.pic_order_cnt_type == 0) {
        if (!bits.read_bits(h264.log2_max_pic_order_cnt_lsb_minus4 + 4, &value)) return false;
        if (h264.pic_order_present_flag && !picture.bottom_field_flag && !bits.read_se(&signed_value)) {
            return false;
        }
    } else if (h264.pic_order_cnt_type != 2) {
        return false;
    }
    if (h264.redundant_pic_cnt_present_flag && !bits.read_ue(&value)) return false;
    if (output->slice_type == 1) {
        if (!bits.read_bit(&value)) return false;
        output->direct_spatial = static_cast<int>(value);
    }

    if (output->slice_type != 2) {
        const int list_count = output->slice_type == 1 ? 2 : 1;
        output->ref_count[0] = h264.num_ref_idx_l0_active_minus1 + 1;
        output->ref_count[1] = h264.num_ref_idx_l1_active_minus1 + 1;
        if (!bits.read_bit(&value)) return false;
        if (value) {
            if (!bits.read_ue(&value)) return false;
            output->ref_count[0] = static_cast<int>(value) + 1;
            if (list_count == 2) {
                if (!bits.read_ue(&value)) return false;
                output->ref_count[1] = static_cast<int>(value) + 1;
            }
        }
        for (int list = 0; list < list_count; ++list) {
            if (output->ref_count[list] <= 0 || output->ref_count[list] > 32) return false;
            if (!bits.read_bit(&value)) return false;
            if (!value) continue;
            while (true) {
                uint32_t operation = 0;
                if (!bits.read_ue(&operation)) return false;
                if (operation == 3) break;
                if (operation > 2 || !bits.read_ue(&value)) return false;
                output->modifications[list].push_back({operation, value});
                if (output->modifications[list].size() > 32) return false;
            }
        }
        if ((output->slice_type == 0 && h264.weighted_pred_flag) ||
            (output->slice_type == 1 && h264.weighted_bipred_idc == 1)) {
            return false;
        }
    }

    if (output->nal_ref_idc) {
        if (output->nal_type == 5) {
            if (!bits.read_bit(&value) || !bits.read_bit(&value)) return false;
        } else {
            if (!bits.read_bit(&value)) return false;
            if (value) {
                while (true) {
                    uint32_t operation = 0;
                    if (!bits.read_ue(&operation)) return false;
                    if (operation == 0) break;
                    if (operation > 6) return false;
                    if (operation == 5) continue;
                    if (!bits.read_ue(&value)) return false;
                    if (operation == 3 || operation == 6) {
                        if (!bits.read_ue(&value)) return false;
                    }
                }
            }
        }
    }

    if (output->slice_type != 2 && h264.entropy_coding_mode_flag) {
        if (!bits.read_ue(&value) || value > 2) return false;
        output->cabac_init_idc = static_cast<int>(value);
    }
    if (!bits.read_se(&output->qp_delta)) return false;
    if (h264.deblocking_filter_control_present_flag) {
        if (!bits.read_ue(&value) || value > 2) return false;
        output->deblocking_idc = static_cast<int>(value);
        if (value != 1) {
            if (!bits.read_se(&output->alpha_div2) || !bits.read_se(&output->beta_div2)) return false;
        }
    }
    output->bit_offset = static_cast<int>(8 + bits.bit_position());
    return output->bit_offset <= std::numeric_limits<uint16_t>::max();
}

struct ReferenceKey {
    bool long_term = false;
    int value = 0;
};

int picture_order_count(const CUVIDH264DPBENTRY &entry) {
    return std::min(entry.FieldOrderCnt[0], entry.FieldOrderCnt[1]);
}

VAPictureH264 to_va_picture(const CUVIDH264DPBENTRY &entry,
                            const std::vector<VASurfaceID> &surfaces) {
    VAPictureH264 output = {};
    output.picture_id = surfaces[entry.PicIdx];
    output.frame_idx = entry.FrameIdx;
    output.flags = entry.is_long_term ? VA_PICTURE_H264_LONG_TERM_REFERENCE
                                      : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    output.TopFieldOrderCnt = entry.FieldOrderCnt[0];
    output.BottomFieldOrderCnt = entry.FieldOrderCnt[1];
    return output;
}

bool build_reference_lists(const CUVIDPICPARAMS &picture, const SliceHeader &slice,
                           std::vector<ReferenceKey> *list0,
                           std::vector<ReferenceKey> *list1) {
    const auto &h264 = picture.CodecSpecific.h264;
    std::vector<const CUVIDH264DPBENTRY *> short_terms;
    std::vector<const CUVIDH264DPBENTRY *> long_terms;
    for (const auto &entry : h264.dpb) {
        if (entry.PicIdx < 0 || entry.not_existing || !entry.used_for_reference) continue;
        if (entry.is_long_term) long_terms.push_back(&entry);
        else short_terms.push_back(&entry);
    }
    std::sort(long_terms.begin(), long_terms.end(), [](const auto *left, const auto *right) {
        return left->FrameIdx < right->FrameIdx;
    });

    if (slice.slice_type == 0) {
        std::sort(short_terms.begin(), short_terms.end(), [](const auto *left, const auto *right) {
            return left->FrameIdx > right->FrameIdx;
        });
        for (const auto *entry : short_terms) list0->push_back({false, entry->FrameIdx});
        for (const auto *entry : long_terms) list0->push_back({true, entry->FrameIdx});
    } else if (slice.slice_type == 1) {
        const int current_poc = std::min(h264.CurrFieldOrderCnt[0], h264.CurrFieldOrderCnt[1]);
        std::vector<const CUVIDH264DPBENTRY *> before;
        std::vector<const CUVIDH264DPBENTRY *> after;
        for (const auto *entry : short_terms) {
            if (picture_order_count(*entry) < current_poc) before.push_back(entry);
            else after.push_back(entry);
        }
        std::sort(before.begin(), before.end(), [](const auto *left, const auto *right) {
            return picture_order_count(*left) > picture_order_count(*right);
        });
        std::sort(after.begin(), after.end(), [](const auto *left, const auto *right) {
            return picture_order_count(*left) < picture_order_count(*right);
        });
        for (const auto *entry : before) list0->push_back({false, entry->FrameIdx});
        for (const auto *entry : after) list0->push_back({false, entry->FrameIdx});
        for (const auto *entry : after) list1->push_back({false, entry->FrameIdx});
        for (const auto *entry : before) list1->push_back({false, entry->FrameIdx});
        for (const auto *entry : long_terms) {
            list0->push_back({true, entry->FrameIdx});
            list1->push_back({true, entry->FrameIdx});
        }
    }

    const int max_picture_num = 1 << (h264.log2_max_frame_num_minus4 + 4);
    for (int list_index = 0; list_index < 2; ++list_index) {
        auto *list = list_index == 0 ? list0 : list1;
        int prediction = slice.frame_num;
        for (std::size_t index = 0; index < slice.modifications[list_index].size(); ++index) {
            if (index >= list->size()) return false;
            const auto &modification = slice.modifications[list_index][index];
            ReferenceKey key;
            if (modification.operation == 0) {
                prediction -= static_cast<int>(modification.value) + 1;
                prediction = (prediction + max_picture_num) % max_picture_num;
                key = {false, prediction};
            } else if (modification.operation == 1) {
                prediction += static_cast<int>(modification.value) + 1;
                prediction %= max_picture_num;
                key = {false, prediction};
            } else {
                key = {true, static_cast<int>(modification.value)};
            }
            auto found = std::find_if(list->begin() + index, list->end(), [&](const ReferenceKey &candidate) {
                return candidate.long_term == key.long_term && candidate.value == key.value;
            });
            if (found == list->end()) return false;
            ReferenceKey selected = *found;
            list->erase(found);
            list->insert(list->begin() + index, selected);
        }
    }
    return true;
}

bool fill_reference_list(VAPictureH264 *destination, const std::vector<ReferenceKey> &keys,
                         int count, const CUVIDPICPARAMS &picture,
                         const std::vector<VASurfaceID> &surfaces) {
    if (count < 0 || count > 32 || static_cast<std::size_t>(count) > keys.size()) return false;
    const auto &h264 = picture.CodecSpecific.h264;
    for (int index = 0; index < count; ++index) {
        const auto &key = keys[index];
        auto found = std::find_if(std::begin(h264.dpb), std::end(h264.dpb), [&](const auto &entry) {
            return entry.PicIdx >= 0 && entry.is_long_term == key.long_term && entry.FrameIdx == key.value;
        });
        if (found == std::end(h264.dpb) || found->PicIdx < 0 ||
            static_cast<std::size_t>(found->PicIdx) >= surfaces.size()) {
            return false;
        }
        destination[index] = to_va_picture(*found, surfaces);
    }
    return true;
}

} // namespace

CUresult translate_h264_picture(const CUVIDPICPARAMS &input,
                                const std::vector<VASurfaceID> &surfaces,
                                VaH264Picture *output) {
    if (!output || !input.pBitstreamData || !input.pSliceDataOffsets ||
        input.nNumSlices == 0 || input.nNumSlices > 4096 || input.CurrPicIdx < 0 ||
        static_cast<std::size_t>(input.CurrPicIdx) >= surfaces.size() ||
        input.PicWidthInMbs <= 0 || input.FrameHeightInMbs <= 0) {
        return CUDA_ERROR_INVALID_VALUE;
    }
    const auto &h264 = input.CodecSpecific.h264;
    if (!h264.frame_mbs_only_flag || h264.MbaffFrameFlag || input.field_pic_flag ||
        h264.fmo_aso_enable || h264.bit_depth_luma_minus8 != 0 ||
        h264.bit_depth_chroma_minus8 != 0 || h264.num_ref_frames < 0 ||
        h264.num_ref_frames > 16) {
        return kNotSupported;
    }

    *output = {};
    auto &parameters = output->parameters;
    parameters.CurrPic.picture_id = surfaces[input.CurrPicIdx];
    parameters.CurrPic.frame_idx = h264.frame_num;
    parameters.CurrPic.flags = 0;
    parameters.CurrPic.TopFieldOrderCnt = h264.CurrFieldOrderCnt[0];
    parameters.CurrPic.BottomFieldOrderCnt = h264.CurrFieldOrderCnt[1];
    for (int i = 0; i < 16; ++i) parameters.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
    for (const auto &entry : h264.dpb) {
        if (entry.PicIdx < 0) continue;
        if (static_cast<std::size_t>(entry.PicIdx) >= surfaces.size()) return CUDA_ERROR_INVALID_VALUE;
    }
    for (int i = 0; i < 16; ++i) {
        const auto &entry = h264.dpb[i];
        if (entry.PicIdx >= 0) parameters.ReferenceFrames[i] = to_va_picture(entry, surfaces);
    }

    parameters.picture_width_in_mbs_minus1 = static_cast<uint16_t>(input.PicWidthInMbs - 1);
    parameters.picture_height_in_mbs_minus1 = static_cast<uint16_t>(input.FrameHeightInMbs - 1);
    parameters.bit_depth_luma_minus8 = h264.bit_depth_luma_minus8;
    parameters.bit_depth_chroma_minus8 = h264.bit_depth_chroma_minus8;
    parameters.num_ref_frames = static_cast<uint8_t>(h264.num_ref_frames);
    parameters.seq_fields.bits.chroma_format_idc = 1;
    parameters.seq_fields.bits.frame_mbs_only_flag = h264.frame_mbs_only_flag;
    parameters.seq_fields.bits.mb_adaptive_frame_field_flag = h264.MbaffFrameFlag;
    parameters.seq_fields.bits.direct_8x8_inference_flag = h264.direct_8x8_inference_flag;
    parameters.seq_fields.bits.log2_max_frame_num_minus4 = h264.log2_max_frame_num_minus4;
    parameters.seq_fields.bits.pic_order_cnt_type = h264.pic_order_cnt_type;
    parameters.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = h264.log2_max_pic_order_cnt_lsb_minus4;
    parameters.seq_fields.bits.delta_pic_order_always_zero_flag = h264.delta_pic_order_always_zero_flag;
    parameters.pic_fields.bits.entropy_coding_mode_flag = h264.entropy_coding_mode_flag;
    parameters.pic_fields.bits.weighted_pred_flag = h264.weighted_pred_flag;
    parameters.pic_fields.bits.weighted_bipred_idc = h264.weighted_bipred_idc;
    parameters.pic_fields.bits.transform_8x8_mode_flag = h264.transform_8x8_mode_flag;
    parameters.pic_fields.bits.constrained_intra_pred_flag = h264.constrained_intra_pred_flag;
    parameters.pic_fields.bits.pic_order_present_flag = h264.pic_order_present_flag;
    parameters.pic_fields.bits.deblocking_filter_control_present_flag = h264.deblocking_filter_control_present_flag;
    parameters.pic_fields.bits.redundant_pic_cnt_present_flag = h264.redundant_pic_cnt_present_flag;
    parameters.pic_fields.bits.reference_pic_flag = input.ref_pic_flag;
    parameters.pic_init_qp_minus26 = static_cast<int8_t>(h264.pic_init_qp_minus26);
    parameters.pic_init_qs_minus26 = h264.pic_init_qs_minus26;
    parameters.chroma_qp_index_offset = static_cast<int8_t>(h264.chroma_qp_index_offset);
    parameters.second_chroma_qp_index_offset = static_cast<int8_t>(h264.second_chroma_qp_index_offset);
    parameters.frame_num = h264.frame_num;

    std::memcpy(output->inverse_quantizer.ScalingList4x4, h264.WeightScale4x4,
                sizeof(output->inverse_quantizer.ScalingList4x4));
    std::memcpy(output->inverse_quantizer.ScalingList8x8[0], h264.WeightScale8x8[0],
                sizeof(output->inverse_quantizer.ScalingList8x8[0]));
    std::memcpy(output->inverse_quantizer.ScalingList8x8[1], h264.WeightScale8x8[1],
                sizeof(output->inverse_quantizer.ScalingList8x8[1]));

    output->slices.reserve(input.nNumSlices);
    for (unsigned int index = 0; index < input.nNumSlices; ++index) {
        const uint32_t offset = input.pSliceDataOffsets[index];
        const uint32_t end = index + 1 < input.nNumSlices
                                 ? input.pSliceDataOffsets[index + 1]
                                 : input.nBitstreamDataLen;
        if (offset >= end || end > input.nBitstreamDataLen) return CUDA_ERROR_INVALID_VALUE;
        SliceHeader header;
        if (!parse_slice_header(input.pBitstreamData + offset, end - offset, input, &header)) {
            return kNotSupported;
        }

        std::vector<ReferenceKey> list0;
        std::vector<ReferenceKey> list1;
        if (header.slice_type != 2 &&
            !build_reference_lists(input, header, &list0, &list1)) {
            return CUDA_ERROR_INVALID_VALUE;
        }

        VASliceParameterBufferH264 slice = {};
        slice.slice_data_size = end - offset;
        slice.slice_data_offset = offset;
        slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        slice.slice_data_bit_offset = static_cast<uint16_t>(header.bit_offset);
        slice.first_mb_in_slice = header.first_mb;
        slice.slice_type = header.slice_type;
        slice.direct_spatial_mv_pred_flag = header.direct_spatial;
        slice.num_ref_idx_l0_active_minus1 = header.slice_type == 2 ? 0 : header.ref_count[0] - 1;
        slice.num_ref_idx_l1_active_minus1 = header.slice_type == 1 ? header.ref_count[1] - 1 : 0;
        slice.cabac_init_idc = header.cabac_init_idc;
        slice.slice_qp_delta = static_cast<int8_t>(header.qp_delta);
        slice.disable_deblocking_filter_idc = header.deblocking_idc;
        slice.slice_alpha_c0_offset_div2 = static_cast<int8_t>(header.alpha_div2);
        slice.slice_beta_offset_div2 = static_cast<int8_t>(header.beta_div2);
        for (int i = 0; i < 32; ++i) {
            slice.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
            slice.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
        }
        if (header.slice_type != 2) {
            if (!fill_reference_list(slice.RefPicList0, list0, header.ref_count[0], input, surfaces)) {
                return CUDA_ERROR_INVALID_VALUE;
            }
            if (header.slice_type == 1 &&
                !fill_reference_list(slice.RefPicList1, list1, header.ref_count[1], input, surfaces)) {
                return CUDA_ERROR_INVALID_VALUE;
            }
        }
        output->slices.push_back(slice);
    }
    return CUDA_SUCCESS;
}

} // namespace cuvid_shim
