#include "h264_picture_translate.h"

#include <cstdio>
#include <cstdlib>

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
    // pred_weight_table()：只有 P 且 weighted_pred_flag、或 B 且 weighted_bipred_idc==1
    // 时才出现在 slice header 里。VAAPI 要原样交给驱动，否则 P/B 帧无法解码。
    int luma_log2_weight_denom = 0;
    int chroma_log2_weight_denom = 0;
    bool luma_weight_l0_flag = false;
    bool chroma_weight_l0_flag = false;
    bool luma_weight_l1_flag = false;
    bool chroma_weight_l1_flag = false;
    int luma_weight_l0[32] = {};
    int luma_offset_l0[32] = {};
    int chroma_weight_l0[32][2] = {};
    int chroma_offset_l0[32][2] = {};
    int luma_weight_l1[32] = {};
    int luma_offset_l1[32] = {};
    int chroma_weight_l1[32][2] = {};
    int chroma_offset_l1[32][2] = {};
    struct Modification {
        uint32_t operation = 3;
        uint32_t value = 0;
    };
    std::vector<Modification> modifications[2];
};

bool parse_slice_header(const uint8_t *data, std::size_t size,
                        const CUVIDPICPARAMS &picture, SliceHeader *output) {
    if (size < 2 || !data) return false;
    // 两种输入都要接受：
    //   - Annex-B：0x000001 / 0x00000001 前缀 + NAL header
    //   - cuvid_parser 传进来的裸 NAL：ff_h2645_packet_split 输出的 RBSP，
    //     已剥掉 start code 且去掉了防竞争字节，byte 0 就是 NAL header，
    //     slice header 从 byte 1 开始（见 h2645_parse.c）。
    std::size_t start_code_size = 0;
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        start_code_size = 4;
    } else if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        start_code_size = 3;
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
            if (std::getenv("CUVID_SHIM_DEBUG_MODS"))
                std::fprintf(stderr, "SHIM MODS list=%d count=%d bitpos=%zu\n", list,
                             output->ref_count[list], bits.bit_position());
            if (!bits.read_bit(&value)) return false;
            if (!value) continue;
            while (true) {
                uint32_t operation = 0;
                if (!bits.read_ue(&operation)) return false;
                if (operation == 3) break;
                if (operation > 2 || !bits.read_ue(&value)) return false;
                output->modifications[list].push_back({operation, value});
                if (std::getenv("CUVID_SHIM_DEBUG_MODS"))
                    std::fprintf(stderr, "SHIM MOD list=%d idx=%zu op=%u val=%u bitpos=%zu\n",
                                 list, output->modifications[list].size() - 1, operation, value,
                                 bits.bit_position());
                if (output->modifications[list].size() > 32) return false;
            }
        }
        const bool has_pred_weight =
            (output->slice_type == 0 && h264.weighted_pred_flag) ||
            (output->slice_type == 1 && h264.weighted_bipred_idc == 1);
        const bool implicit_bipred =
            output->slice_type == 1 && h264.weighted_bipred_idc == 2;
        if (implicit_bipred) {
            // weighted_bipred_idc == 2 是"隐式权重"：码流里没有 pred_weight_table，
            // 权重由驱动按参考帧 POC 距离推导。但 log2_weight_denom 仍然要传，
            // 而且必须是 5 —— FFmpeg 在这条路径上就是把它写成 5
            //（h264_slice.c: implicit_weight_table() 里的 pwt 默认值）。
            // 传 0 的话驱动推出来的权重全是 1<<0，B 帧的加权预测等于关掉，
            // 实测表现是整帧退化成 L0 参考帧的拷贝。
            output->luma_log2_weight_denom = 5;
            output->chroma_log2_weight_denom = 5;
        }
        if (has_pred_weight) {
            // pred_weight_table() —— 4:2:0 时 ChromaArrayType != 0，chroma 部分存在。
            if (!bits.read_ue(&value) || value > 7) return false;
            output->luma_log2_weight_denom = static_cast<int>(value);
            if (!bits.read_ue(&value) || value > 7) return false;
            output->chroma_log2_weight_denom = static_cast<int>(value);
            const int weight_lists = output->slice_type == 1 ? 2 : 1;
            for (int list = 0; list < weight_lists; ++list) {
                const int count = output->ref_count[list];
                if (count <= 0 || count > 32) return false;
                for (int i = 0; i < count; ++i) {
                    int32_t weight = 0;
                    int32_t offset = 0;
                    if (!bits.read_bit(&value)) return false;
                    if (value) {
                        if (!bits.read_se(&weight) || !bits.read_se(&offset)) return false;
                        if (list == 0) {
                            output->luma_weight_l0_flag = true;
                            output->luma_weight_l0[i] = weight;
                            output->luma_offset_l0[i] = offset;
                        } else {
                            output->luma_weight_l1_flag = true;
                            output->luma_weight_l1[i] = weight;
                            output->luma_offset_l1[i] = offset;
                        }
                    }
                    if (!bits.read_bit(&value)) return false;
                    if (value) {
                        for (int j = 0; j < 2; ++j) {
                            if (!bits.read_se(&weight) || !bits.read_se(&offset)) return false;
                            if (list == 0) {
                                output->chroma_weight_l0_flag = true;
                                output->chroma_weight_l0[i][j] = weight;
                                output->chroma_offset_l0[i][j] = offset;
                            } else {
                                output->chroma_weight_l1_flag = true;
                                output->chroma_weight_l1[i][j] = weight;
                                output->chroma_offset_l1[i][j] = offset;
                            }
                        }
                    }
                }
            }
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
    // FFmpeg 的 ref_list 会补零到 ref_count 并用空槽位表示"缺失的参考帧"
    // （见 h264_refs.c h264_initialise_ref_list 里的 memset）。照搬这个语义，
    // 否则 ref_count 比 DPB 实际参考帧多时整帧会失败。
    bool valid = true;
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
    if (std::getenv("CUVID_SHIM_DEBUG_DPB")) {
        std::fprintf(stderr, "SHIM DPB cur idx=%d fn=%d poc=%d stype=%d:",
                     picture.CurrPicIdx, slice.frame_num,
                     std::min(h264.CurrFieldOrderCnt[0], h264.CurrFieldOrderCnt[1]),
                     slice.slice_type);
        for (int i = 0; i < 16; ++i)
            if (h264.dpb[i].PicIdx >= 0)
                std::fprintf(stderr, " [%d]=pic%d/fn%d/poc%d/lt%d/ref%d/ne%d", i,
                             h264.dpb[i].PicIdx, h264.dpb[i].FrameIdx,
                             std::min(h264.dpb[i].FieldOrderCnt[0], h264.dpb[i].FieldOrderCnt[1]),
                             (int)h264.dpb[i].is_long_term, (int)h264.dpb[i].used_for_reference,
                             (int)h264.dpb[i].not_existing);
        std::fprintf(stderr, "\n");
    }
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
        const int ref_count = slice.ref_count[list_index];
        if (ref_count < 0 || ref_count > 32) return false;
        // 截断/补零到 ref_count：与 FFmpeg 的 h264_initialise_ref_list 一致。
        if (list->size() > static_cast<std::size_t>(ref_count)) list->resize(ref_count);
        while (list->size() < static_cast<std::size_t>(ref_count)) {
            list->push_back(ReferenceKey{false, -1, false});
        }

        int prediction = slice.frame_num;
        for (std::size_t index = 0; index < slice.modifications[list_index].size(); ++index) {
            if (index >= list->size()) break;   // 多余的重排命令忽略（FFmpeg 报 overflow 后继续）
            const auto &modification = slice.modifications[list_index][index];
            ReferenceKey key;
            if (modification.operation == 0 || modification.operation == 1) {
                const int delta = static_cast<int>(modification.value) + 1;
                prediction += modification.operation == 0 ? -delta : delta;
                prediction %= max_picture_num;
                if (prediction < 0) prediction += max_picture_num;
                key = {false, prediction, true};
            } else {
                key = {true, static_cast<int>(modification.value), true};
            }
            // 目标必须真的在 DPB 里（FFmpeg 也是按 frame_num 去 short_ref 里找）。
            if (key.valid) {
                const bool in_dpb = std::any_of(
                    std::begin(h264.dpb), std::end(h264.dpb), [&](const CUVIDH264DPBENTRY &entry) {
                        return entry.PicIdx >= 0 && !entry.not_existing &&
                               entry.used_for_reference &&
                               static_cast<bool>(entry.is_long_term) == key.long_term &&
                               entry.FrameIdx == key.value;
                    });
                if (!in_dpb) key = ReferenceKey{false, -1, false};   // 缺失参考帧：留空槽
            }

            // FFmpeg 的重排语义：从 index 起找同键项，找到就把该项左移到 index，
            // 中间元素整体右移；找不到就在 index 处插入一份（右移覆盖末尾）。
            std::size_t slot = list->size();
            if (key.valid) {
                for (std::size_t i = index; i < list->size(); ++i) {
                    if ((*list)[i].valid && (*list)[i].long_term == key.long_term &&
                        (*list)[i].value == key.value) {
                        slot = i;
                        break;
                    }
                }
            }
            if (slot != list->size()) {
                for (std::size_t i = slot; i > index; --i) (*list)[i] = (*list)[i - 1];
            } else {
                for (std::size_t i = list->size() - 1; i > index; --i) (*list)[i] = (*list)[i - 1];
            }
            (*list)[index] = key;
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
        if (!key.valid) continue;   // 缺失参考帧：该槽位保持 VA_PICTURE_H264_INVALID
        auto found = std::find_if(std::begin(h264.dpb), std::end(h264.dpb), [&](const auto &entry) {
            return entry.PicIdx >= 0 && entry.is_long_term == key.long_term && entry.FrameIdx == key.value;
        });
        if (found == std::end(h264.dpb) || found->PicIdx < 0 ||
            static_cast<std::size_t>(found->PicIdx) >= surfaces.size()) {
            if (std::getenv("CUVID_SHIM_DEBUG_DPB")) {
                std::fprintf(stderr, "DBG fill_reflist: idx=%d key lt=%d val=%d not in dpb\n",
                             index, (int)key.long_term, key.value);
            }
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
    parameters.CurrPic.flags = input.ref_pic_flag ? VA_PICTURE_H264_SHORT_TERM_REFERENCE : 0;
    parameters.CurrPic.TopFieldOrderCnt = h264.CurrFieldOrderCnt[0];
    parameters.CurrPic.BottomFieldOrderCnt = h264.CurrFieldOrderCnt[1];
    // 未被 DPB 使用的槽必须显式标 INVALID，否则驱动会把 0 值槽当成有效短参考。
    for (int i = 0; i < 16; ++i) {
        parameters.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        parameters.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
        parameters.ReferenceFrames[i].frame_idx = 0;
        parameters.ReferenceFrames[i].TopFieldOrderCnt = 0;
        parameters.ReferenceFrames[i].BottomFieldOrderCnt = 0;
    }
    for (const auto &entry : h264.dpb) {
        if (entry.PicIdx < 0) continue;
        if (static_cast<std::size_t>(entry.PicIdx) >= surfaces.size()) return CUDA_ERROR_INVALID_VALUE;
    }
    {
        std::vector<const CUVIDH264DPBENTRY *> entries;
        for (int i = 0; i < 16; ++i)
            if (h264.dpb[i].PicIdx >= 0) entries.push_back(&h264.dpb[i]);
        const char *order = std::getenv("CUVID_SHIM_REF_ORDER");
        if (order && !std::strcmp(order, "desc"))
            std::sort(entries.begin(), entries.end(),
                      [](const CUVIDH264DPBENTRY *a, const CUVIDH264DPBENTRY *b) {
                          return a->FrameIdx > b->FrameIdx;
                      });
        if (const char *limit = std::getenv("CUVID_SHIM_REF_LIMIT")) {
            const std::size_t n = static_cast<std::size_t>(std::atoi(limit));
            if (entries.size() > n) entries.resize(n);
        }
        // VA 要求 ReferenceFrames 从 [0] 起连续填充，中间出现 INVALID 会被驱动截断。
        for (std::size_t i = 0; i < entries.size() && i < 16; ++i)
            parameters.ReferenceFrames[i] = to_va_picture(*entries[i], surfaces);
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

    if (std::getenv("CUVID_SHIM_DEBUG_DUMP")) {
        std::fprintf(stderr,
                     "SHIM PIC bd=%d/%d nref=%d wh=%d/%d seq[cid=%d rct=%d gap=%d fmo=%d mbaff=%d "
                     "d8=%d mlb=%d l2fn=%d poc=%d l2poc=%d dpo=%d] pic[qp=%d qs=%d cqp=%d cqp2=%d "
                     "cabac=%d wp=%d wb=%d t8=%d fp=%d cip=%d pop=%d dfc=%d rpc=%d rpf=%d] fn=%d\n",
                     parameters.bit_depth_luma_minus8, parameters.bit_depth_chroma_minus8,
                     parameters.num_ref_frames, parameters.picture_width_in_mbs_minus1,
                     parameters.picture_height_in_mbs_minus1,
                     parameters.seq_fields.bits.chroma_format_idc,
                     parameters.seq_fields.bits.residual_colour_transform_flag,
                     parameters.seq_fields.bits.gaps_in_frame_num_value_allowed_flag,
                     parameters.seq_fields.bits.frame_mbs_only_flag,
                     parameters.seq_fields.bits.mb_adaptive_frame_field_flag,
                     parameters.seq_fields.bits.direct_8x8_inference_flag,
                     parameters.seq_fields.bits.MinLumaBiPredSize8x8,
                     parameters.seq_fields.bits.log2_max_frame_num_minus4,
                     parameters.seq_fields.bits.pic_order_cnt_type,
                     parameters.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4,
                     parameters.seq_fields.bits.delta_pic_order_always_zero_flag,
                     parameters.pic_init_qp_minus26, parameters.pic_init_qs_minus26,
                     parameters.chroma_qp_index_offset, parameters.second_chroma_qp_index_offset,
                     parameters.pic_fields.bits.entropy_coding_mode_flag,
                     parameters.pic_fields.bits.weighted_pred_flag,
                     parameters.pic_fields.bits.weighted_bipred_idc,
                     parameters.pic_fields.bits.transform_8x8_mode_flag,
                     parameters.pic_fields.bits.field_pic_flag,
                     parameters.pic_fields.bits.constrained_intra_pred_flag,
                     parameters.pic_fields.bits.pic_order_present_flag,
                     parameters.pic_fields.bits.deblocking_filter_control_present_flag,
                     parameters.pic_fields.bits.redundant_pic_cnt_present_flag,
                     parameters.pic_fields.bits.reference_pic_flag, parameters.frame_num);
        std::fprintf(stderr, "SHIM CURPIC id=%u idx=%d flags=0x%x toc=%d boc=%d\n",
                     parameters.CurrPic.picture_id, parameters.CurrPic.frame_idx,
                     parameters.CurrPic.flags, parameters.CurrPic.TopFieldOrderCnt,
                     parameters.CurrPic.BottomFieldOrderCnt);
        for (int i = 0; i < 16; ++i)
            if (parameters.ReferenceFrames[i].flags != VA_PICTURE_H264_INVALID)
                std::fprintf(stderr, "SHIM REF[%d] id=%u idx=%d flags=0x%x toc=%d boc=%d\n", i,
                             parameters.ReferenceFrames[i].picture_id,
                             parameters.ReferenceFrames[i].frame_idx,
                             parameters.ReferenceFrames[i].flags,
                             parameters.ReferenceFrames[i].TopFieldOrderCnt,
                             parameters.ReferenceFrames[i].BottomFieldOrderCnt);
    }

    if (const char *raw_path = std::getenv("CUVID_SHIM_DUMP_RAW")) {
        if (FILE *f = std::fopen(raw_path, "a")) {
            const auto *bytes = reinterpret_cast<const uint8_t *>(&parameters);
            std::fprintf(f, "PIC %d ", input.CurrPicIdx);
            for (std::size_t i = 0; i < sizeof(parameters); ++i) std::fprintf(f, "%02x", bytes[i]);
            std::fprintf(f, "\n");
            std::fclose(f);
        }
    }
    // 反量化矩阵必须完整填：4x4 六个表 + 8x8 两个表。
    // 以前只填了 8x8，4x4 保持全 0；VCN 拿到全 0 的 4x4 缩放表后残差恒为 0，
    // 解出来的帧只剩预测值——I 帧是平坦 0x80，P/B 帧就是参考帧的拷贝。
    std::memcpy(output->inverse_quantizer.ScalingList4x4, h264.WeightScale4x4,
                sizeof(output->inverse_quantizer.ScalingList4x4));
    std::memcpy(output->inverse_quantizer.ScalingList8x8[0], h264.WeightScale8x8[0],
                sizeof(output->inverse_quantizer.ScalingList8x8[0]));
    std::memcpy(output->inverse_quantizer.ScalingList8x8[1], h264.WeightScale8x8[1],
                sizeof(output->inverse_quantizer.ScalingList8x8[1]));
    // 宿主万一没填（CUVID 不做校验），退回到 H.264 默认单位矩阵（全 16），
    // 而不是把全 0 交给驱动——全 0 缩放表会让残差整体变成 0。
    {
        auto all_zero = [](const uint8_t *p, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i)
                if (p[i] != 0) return false;
            return true;
        };
        uint8_t *const list4x4 = &output->inverse_quantizer.ScalingList4x4[0][0];
        if (all_zero(list4x4, sizeof(output->inverse_quantizer.ScalingList4x4)))
            std::memset(list4x4, 16, sizeof(output->inverse_quantizer.ScalingList4x4));
        for (int i = 0; i < 2; ++i) {
            uint8_t *const list8x8 = output->inverse_quantizer.ScalingList8x8[i];
            if (all_zero(list8x8, sizeof(output->inverse_quantizer.ScalingList8x8[i])))
                std::memset(list8x8, 16, sizeof(output->inverse_quantizer.ScalingList8x8[i]));
        }
    }

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
            slice.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
            slice.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
            slice.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
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
        // pred_weight_table() 结果。VA API 要的是"推断后的"权重（7.4.3.2）：
        // 对应 flag 为 0 时也必须写入默认值 1<<denom / 0，不能留 0。
        slice.luma_log2_weight_denom = static_cast<uint8_t>(header.luma_log2_weight_denom);
        slice.chroma_log2_weight_denom = static_cast<uint8_t>(header.chroma_log2_weight_denom);
        slice.luma_weight_l0_flag = header.luma_weight_l0_flag;
        slice.chroma_weight_l0_flag = header.chroma_weight_l0_flag;
        slice.luma_weight_l1_flag = header.luma_weight_l1_flag;
        slice.chroma_weight_l1_flag = header.chroma_weight_l1_flag;
        for (int i = 0; i < 32; ++i) {
            slice.luma_weight_l0[i] = static_cast<int16_t>(header.luma_weight_l0[i]);
            slice.luma_offset_l0[i] = static_cast<int16_t>(header.luma_offset_l0[i]);
            slice.luma_weight_l1[i] = static_cast<int16_t>(header.luma_weight_l1[i]);
            slice.luma_offset_l1[i] = static_cast<int16_t>(header.luma_offset_l1[i]);
            for (int j = 0; j < 2; ++j) {
                slice.chroma_weight_l0[i][j] = static_cast<int16_t>(header.chroma_weight_l0[i][j]);
                slice.chroma_offset_l0[i][j] = static_cast<int16_t>(header.chroma_offset_l0[i][j]);
                slice.chroma_weight_l1[i][j] = static_cast<int16_t>(header.chroma_weight_l1[i][j]);
                slice.chroma_offset_l1[i][j] = static_cast<int16_t>(header.chroma_offset_l1[i][j]);
            }
        }
        if (header.slice_type != 2) {
            const int luma_denom = 1 << header.luma_log2_weight_denom;
            const int chroma_denom = 1 << header.chroma_log2_weight_denom;
            if (!header.luma_weight_l0_flag) {
                for (int i = 0; i < header.ref_count[0] && i < 32; ++i) {
                    slice.luma_weight_l0[i] = static_cast<int16_t>(luma_denom);
                    slice.luma_offset_l0[i] = 0;
                }
            }
            if (!header.chroma_weight_l0_flag) {
                for (int i = 0; i < header.ref_count[0] && i < 32; ++i) {
                    for (int j = 0; j < 2; ++j) {
                        slice.chroma_weight_l0[i][j] = static_cast<int16_t>(chroma_denom);
                        slice.chroma_offset_l0[i][j] = 0;
                    }
                }
            }
            if (header.slice_type == 1 && !header.luma_weight_l1_flag) {
                for (int i = 0; i < header.ref_count[1] && i < 32; ++i) {
                    slice.luma_weight_l1[i] = static_cast<int16_t>(luma_denom);
                    slice.luma_offset_l1[i] = 0;
                }
            }
            if (header.slice_type == 1 && !header.chroma_weight_l1_flag) {
                for (int i = 0; i < header.ref_count[1] && i < 32; ++i) {
                    for (int j = 0; j < 2; ++j) {
                        slice.chroma_weight_l1[i][j] = static_cast<int16_t>(chroma_denom);
                        slice.chroma_offset_l1[i][j] = 0;
                    }
                }
            }
        }
        if (std::getenv("CUVID_SHIM_DEBUG_DUMP")) {
            std::fprintf(stderr,
                         "SHIM SLICE size=%u off=%u flag=%u bitoff=%d fmb=%d stype=%d dsp=%d "
                         "l0=%d l1=%d cabac=%d qpd=%d dblk=%d a=%d b=%d l2w=%d c2w=%d "
                         "lw0=%d cw0=%d lw1=%d cw1=%d\n",
                         slice.slice_data_size, slice.slice_data_offset, slice.slice_data_flag,
                         slice.slice_data_bit_offset, slice.first_mb_in_slice, slice.slice_type,
                         slice.direct_spatial_mv_pred_flag, slice.num_ref_idx_l0_active_minus1,
                         slice.num_ref_idx_l1_active_minus1, slice.cabac_init_idc,
                         slice.slice_qp_delta, slice.disable_deblocking_filter_idc,
                         slice.slice_alpha_c0_offset_div2, slice.slice_beta_offset_div2,
                         slice.luma_log2_weight_denom, slice.chroma_log2_weight_denom,
                         slice.luma_weight_l0_flag, slice.chroma_weight_l0_flag,
                         slice.luma_weight_l1_flag, slice.chroma_weight_l1_flag);
            std::fprintf(stderr, "SHIM BUF size=%u head=", input.nBitstreamDataLen);
            for (unsigned int i = 0; i < 24 && i < input.nBitstreamDataLen; ++i)
                std::fprintf(stderr, "%02x", input.pBitstreamData[i]);
            std::fprintf(stderr, "\n");
        }
        if (std::getenv("CUVID_SHIM_DEBUG_RPL")) {
            std::fprintf(stderr, "SHIM RPL pic=%d stype=%d fn=%d | L0:", input.CurrPicIdx,
                         header.slice_type, header.frame_num);
            for (int i = 0; i < 32 && i <= slice.num_ref_idx_l0_active_minus1; ++i)
                std::fprintf(stderr, " %u", slice.RefPicList0[i].picture_id);
            std::fprintf(stderr, " | L1:");
            for (int i = 0; i < 32 && header.slice_type == 1 && i <= slice.num_ref_idx_l1_active_minus1; ++i)
                std::fprintf(stderr, " %u", slice.RefPicList1[i].picture_id);
            std::fprintf(stderr, "\n");
        }
        if (const char *raw_path = std::getenv("CUVID_SHIM_DUMP_RAW")) {
            if (FILE *f = std::fopen(raw_path, "a")) {
                const auto *bytes = reinterpret_cast<const uint8_t *>(&slice);
                std::fprintf(f, "SLICE %d ", input.CurrPicIdx);
                for (std::size_t i = 0; i < sizeof(slice); ++i) std::fprintf(f, "%02x", bytes[i]);
                std::fprintf(f, "\n");
                std::fclose(f);
            }
        }
        output->slices.push_back(slice);
    }
    // 只把「这张图真的会用到」的参考帧写进 VA ReferenceFrames。
    //
    // 以前这里直接把 DPB 里所有还活着的参考帧都填进去。实测（320x240 的
    // IBPBP 流）这样会让 mesa radeonsi 的 VCN 解码出错：从第 3 个 B 帧起，
    // 输出退化成某个参考帧的逐像素拷贝，之后所有帧跟着错。
    // 对照 FFmpeg 自己的 VAAPI 解码路径（同一个流、同一个驱动、同样线性面），
    // 它在 ReferenceFrames 里只给它当前 DPB 里「仍在用」的那几个参考帧，
    // 数量和集合都比我们少一个；按引用列表收缩后，30/30 帧与软解逐字节一致。
    //
    // 所以按 slice 的 RefPicList0/1 取并集（保持首次出现顺序，按 picture_id 去重），
    // 没有引用列表的图（I/IDR slice）保持原有的 DPB 列表不动。
    {
        std::vector<VAPictureH264> used;
        auto add = [&](const VAPictureH264 &pic) {
            if (pic.picture_id == VA_INVALID_SURFACE) return;
            for (const auto &existing : used)
                if (existing.picture_id == pic.picture_id) return;
            used.push_back(pic);
        };
        for (const auto &slice : output->slices) {
            for (int j = 0; j < 32; ++j) {
                const VAPictureH264 *lists[2] = {&slice.RefPicList0[j], &slice.RefPicList1[j]};
                for (const VAPictureH264 *candidate : lists) {
                    if (candidate->flags == VA_PICTURE_H264_INVALID) continue;
                    for (int k = 0; k < 16; ++k) {
                        const VAPictureH264 &p = parameters.ReferenceFrames[k];
                        if (p.flags == VA_PICTURE_H264_INVALID) continue;
                        if (p.picture_id == candidate->picture_id) add(p);
                    }
                }
            }
        }
        if (!used.empty()) {
            for (int i = 0; i < 16; ++i) {
                parameters.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
                parameters.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
                parameters.ReferenceFrames[i].frame_idx = 0;
                parameters.ReferenceFrames[i].TopFieldOrderCnt = 0;
                parameters.ReferenceFrames[i].BottomFieldOrderCnt = 0;
            }
            for (std::size_t i = 0; i < used.size() && i < 16; ++i)
                parameters.ReferenceFrames[i] = used[i];
        }
    }
    return CUDA_SUCCESS;
}

} // namespace cuvid_shim
