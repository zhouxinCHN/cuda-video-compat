#include "cuvid_parser.h"

#include <cstring>
#include <algorithm>

extern "C" {
#include "libavcodec/golomb.h"
}

namespace cuvid_shim {
namespace {

constexpr CUresult kNotSupported = static_cast<CUresult>(801);

// H.264 NAL types used by the parser.
constexpr int NAL_SLICE = 1;
constexpr int NAL_SLICE_IDR = 5;
constexpr int NAL_SPS = 7;
constexpr int NAL_PPS = 8;

} // namespace

CuvidParser::CuvidParser(const CUVIDPARSERPARAMS &params) : params_(params) {
    avctx_.codec_id = AV_CODEC_ID_H264;
}

CuvidParser::~CuvidParser() {
    if (ps_.sps_list[0] || ps_.pps_list[0]) ff_h264_ps_uninit(&ps_);
}

CUresult CuvidParser::parse(const CUVIDSOURCEDATAPACKET &packet) {
    if (packet.payload_size == 0) {
        pump_display_queue();
        return CUDA_SUCCESS;
    }
    if (!packet.payload) return CUDA_ERROR_INVALID_VALUE;

    buffer_.insert(buffer_.end(), packet.payload, packet.payload + packet.payload_size);
    CUresult result = parse_packet_nal(packet.timestamp);
    buffer_.clear();
    return result;
}

CUresult CuvidParser::parse_packet_nal(int64_t timestamp) {
    if (buffer_.empty()) return CUDA_SUCCESS;

    H2645Packet pkt = {};
    int ret = ff_h2645_packet_split(&pkt, buffer_.data(), static_cast<int>(buffer_.size()),
                                    nullptr, 0, AV_CODEC_ID_H264, 0);
    if (ret < 0) return CUDA_ERROR_INVALID_VALUE;

    for (int i = 0; i < pkt.nb_nals; ++i) {
        H2645NAL &nal = pkt.nals[i];
        switch (nal.type) {
        case NAL_SPS: {
            GetBitContext gb = nal.gb;
            ret = ff_h264_decode_seq_parameter_set(&gb, &avctx_, &ps_, 0);
            if (ret < 0) { ff_h2645_packet_uninit(&pkt); return kNotSupported; }
            for (int k = 0; k < MAX_SPS_COUNT; ++k) {
                if (ps_.sps_list[k]) { sps_ = ps_.sps_list[k]; break; }
            }
            ps_ready_ = false;
            // Stash SPS NAL bytes (Annex-B, data = unescaped w/o start code; feed it
            // to the Vulkan backend's cache once PPS also arrives).
            pending_sps_.assign(nal.data, nal.data + nal.size);
            if (!pending_sps_.empty() && !pending_pps_.empty())
                VkSequenceCache::store(pending_sps_.data(), pending_sps_.size(),
                                       pending_pps_.data(), pending_pps_.size());
            break;
        }
        case NAL_PPS: {
            GetBitContext gb = nal.gb;
            ret = ff_h264_decode_picture_parameter_set(&gb, &avctx_, &ps_, nal.size_bits);
            if (ret < 0) { ff_h2645_packet_uninit(&pkt); return kNotSupported; }
            for (int k = 0; k < MAX_PPS_COUNT; ++k) {
                if (ps_.pps_list[k]) { pps_ = ps_.pps_list[k]; break; }
            }
            ps_ready_ = sps_ && pps_;
            pending_pps_.assign(nal.data, nal.data + nal.size);
            if (!pending_sps_.empty() && !pending_pps_.empty())
                VkSequenceCache::store(pending_sps_.data(), pending_sps_.size(),
                                       pending_pps_.data(), pending_pps_.size());
            break;
        }
        case NAL_SLICE:
        case NAL_SLICE_IDR: {
            if (!ps_ready_) break; // wait for SPS/PPS
            if (!sequence_sent_) {
                CUresult result = send_sequence();
                if (result != CUDA_SUCCESS) { ff_h2645_packet_uninit(&pkt); return result; }
            }
            if (!num_decode_surfaces_) { ff_h2645_packet_uninit(&pkt); return CUDA_SUCCESS; }

            // 解析 slice header 获取 frame_num 和 POC
            int frame_num = 0;
            int poc = 0;
            bool is_reference = true;
            parse_slice_header(nal.data, nal.size, &frame_num, &poc, &is_reference);

            CUresult result = send_picture(nal.data, nal.size, timestamp,
                                           frame_num, poc, is_reference,
                                           nal.type == NAL_SLICE_IDR);
            if (result != CUDA_SUCCESS) { ff_h2645_packet_uninit(&pkt); return result; }
            break;
        }
        default:
            break; // AUD / SEI / filler ignored
        }
    }

    ff_h2645_packet_uninit(&pkt);
    return CUDA_SUCCESS;
}

CUresult CuvidParser::send_sequence() {
    if (!params_.pfnSequenceCallback) return kNotSupported;
    if (!sps_) return kNotSupported;

    CUVIDEOFORMAT format = {};
    format.codec = cudaVideoCodec_H264;
    format.chroma_format = sps_->chroma_format_idc == 1
                               ? cudaVideoChromaFormat_420
                               : (sps_->chroma_format_idc == 2
                                      ? cudaVideoChromaFormat_422
                                      : cudaVideoChromaFormat_444);
    format.bit_depth_luma_minus8 = sps_->bit_depth_luma - 8;
    format.bit_depth_chroma_minus8 = sps_->bit_depth_chroma - 8;
    format.coded_width = sps_->mb_width * 16;
    format.coded_height = sps_->mb_height * 16;
    format.display_area.left = static_cast<short>(sps_->crop_left);
    format.display_area.top = static_cast<short>(sps_->crop_top);
    format.display_area.right = static_cast<short>(sps_->crop_right);
    format.display_area.bottom = static_cast<short>(sps_->crop_bottom);
    format.frame_rate.numerator = 30000;
    format.frame_rate.denominator = 1001;
    format.progressive_sequence = sps_->frame_mbs_only_flag;

    int ret = params_.pfnSequenceCallback(params_.pUserData, &format);
    if (ret <= 0) return kNotSupported;
    num_decode_surfaces_ = static_cast<unsigned int>(ret);
    sequence_sent_ = true;
    return CUDA_SUCCESS;
}

CUresult CuvidParser::parse_slice_header(const uint8_t *nal_data, std::size_t nal_size,
                                         int *frame_num, int *poc, bool *is_reference) {
    if (nal_size < 2) {
        *frame_num = 0;
        *poc = 0;
        *is_reference = true;
        return CUDA_SUCCESS;
    }

    // 提取 nal_ref_idc (bits 5-6 of first byte)
    *is_reference = ((nal_data[1] >> 5) & 0x3) != 0;

    // 使用 FFmpeg 解析 slice header
    H2645Packet pkt = {};
    int ret = ff_h2645_packet_split(&pkt, nal_data, nal_size, nullptr, 0, AV_CODEC_ID_H264, 0);
    if (ret < 0 || pkt.nb_nals == 0) {
        *frame_num = 0;
        *poc = 0;
        return CUDA_SUCCESS;
    }

    GetBitContext gb;
    init_get_bits(&gb, pkt.nals[0].data + 1, pkt.nals[0].size * 8);

    // first_mb_in_slice (ue)
    get_ue_golomb(&gb);
    // slice_type (ue)
    get_ue_golomb(&gb);
    // pic_parameter_set_id (ue)
    get_ue_golomb(&gb);
    // frame_num (u(v))
    *frame_num = get_bits(&gb, sps_->log2_max_frame_num);

    // 计算 POC
    if (sps_->poc_type == 0) {
        int poc_lsb = get_bits(&gb, sps_->log2_max_poc_lsb);
        int max_poc_lsb = 1 << sps_->log2_max_poc_lsb;

        // POC MSB 回绕处理
        if (poc_lsb < prev_poc_lsb_ && (prev_poc_lsb_ - poc_lsb) >= (max_poc_lsb / 2)) {
            prev_poc_msb_ += max_poc_lsb;
        } else if (poc_lsb > prev_poc_lsb_ && (poc_lsb - prev_poc_lsb_) > (max_poc_lsb / 2)) {
            prev_poc_msb_ -= max_poc_lsb;
        }
        *poc = prev_poc_msb_ + poc_lsb;
        prev_poc_lsb_ = poc_lsb;
    } else if (sps_->poc_type == 2) {
        // POC type 2: 基于 frame_num 推导
        *poc = *frame_num * 2;
    } else {
        *poc = *frame_num * 2; // 默认
    }

    prev_frame_num_ = *frame_num;

    ff_h2645_packet_uninit(&pkt);
    return CUDA_SUCCESS;
}

CUresult CuvidParser::send_picture(const uint8_t *nal_data, std::size_t nal_size,
                                   int64_t timestamp, int frame_num, int poc,
                                   bool is_reference, bool is_idr) {
    if (!params_.pfnDecodePicture || !sps_ || !pps_) return kNotSupported;

    // IDR 帧清空 DPB
    if (is_idr) {
        dpb_.clear();
        prev_frame_num_ = 0;
        prev_poc_lsb_ = 0;
        prev_poc_msb_ = 0;
    }

    CUVIDPICPARAMS pic = {};
    pic.PicWidthInMbs = sps_->mb_width;
    pic.FrameHeightInMbs = sps_->mb_height;
    pic.CurrPicIdx = static_cast<int>(next_picture_index_);
    pic.field_pic_flag = 0;
    pic.bottom_field_flag = 0;
    pic.second_field = 0;
    pic.ref_pic_flag = is_reference ? 1 : 0;
    pic.intra_pic_flag = is_idr ? 1 : 0;
    pic.nBitstreamDataLen = static_cast<unsigned int>(nal_size);
    pic.pBitstreamData = const_cast<uint8_t *>(nal_data);
    pic.nNumSlices = 1;
    static const unsigned int zero_offset = 0;
    pic.pSliceDataOffsets = &zero_offset;

    auto &h264 = pic.CodecSpecific.h264;
    h264.log2_max_frame_num_minus4 = sps_->log2_max_frame_num - 4;
    h264.pic_order_cnt_type = sps_->poc_type;
    h264.log2_max_pic_order_cnt_lsb_minus4 = sps_->log2_max_poc_lsb - 4;
    h264.delta_pic_order_always_zero_flag = sps_->delta_pic_order_always_zero_flag;
    h264.frame_mbs_only_flag = sps_->frame_mbs_only_flag;
    h264.direct_8x8_inference_flag = sps_->direct_8x8_inference_flag;
    h264.num_ref_frames = sps_->ref_frame_count;
    h264.residual_colour_transform_flag = 0;
    h264.bit_depth_luma_minus8 = static_cast<unsigned char>(sps_->bit_depth_luma - 8);
    h264.bit_depth_chroma_minus8 = static_cast<unsigned char>(sps_->bit_depth_chroma - 8);
    h264.qpprime_y_zero_transform_bypass_flag = 0;
    h264.entropy_coding_mode_flag = pps_->cabac;
    h264.pic_order_present_flag = pps_->pic_order_present;
    h264.num_ref_idx_l0_active_minus1 = pps_->ref_count[0] - 1;
    h264.num_ref_idx_l1_active_minus1 = pps_->ref_count[1] - 1;
    h264.weighted_pred_flag = pps_->weighted_pred;
    h264.weighted_bipred_idc = pps_->weighted_bipred_idc;
    h264.pic_init_qp_minus26 = pps_->init_qp - 26;
    h264.deblocking_filter_control_present_flag = pps_->deblocking_filter_parameters_present;
    h264.redundant_pic_cnt_present_flag = pps_->redundant_pic_cnt_present;
    h264.transform_8x8_mode_flag = pps_->transform_8x8_mode;
    h264.MbaffFrameFlag = sps_->mb_aff;
    h264.constrained_intra_pred_flag = pps_->constrained_intra_pred;
    h264.chroma_qp_index_offset = pps_->chroma_qp_index_offset[0];
    h264.second_chroma_qp_index_offset = pps_->chroma_qp_index_offset[1];
    h264.ref_pic_flag = is_reference ? 1 : 0;
    h264.frame_num = frame_num;
    h264.CurrFieldOrderCnt[0] = poc;
    h264.CurrFieldOrderCnt[1] = poc;

    // 填充 DPB
    for (int i = 0; i < 16; ++i) {
        if (i < static_cast<int>(dpb_.size())) {
            h264.dpb[i].PicIdx = dpb_[i].picture_index;
            h264.dpb[i].FrameIdx = dpb_[i].frame_num;
            h264.dpb[i].is_long_term = dpb_[i].is_long_term ? 1 : 0;
            h264.dpb[i].not_existing = 0;
            h264.dpb[i].used_for_reference = dpb_[i].is_reference ? 1 : 0;
            h264.dpb[i].FieldOrderCnt[0] = dpb_[i].poc;
            h264.dpb[i].FieldOrderCnt[1] = dpb_[i].poc;
        } else {
            h264.dpb[i].PicIdx = -1;
            h264.dpb[i].FrameIdx = 0;
            h264.dpb[i].is_long_term = 0;
            h264.dpb[i].not_existing = 0;
            h264.dpb[i].used_for_reference = 0;
            h264.dpb[i].FieldOrderCnt[0] = 0;
            h264.dpb[i].FieldOrderCnt[1] = 0;
        }
    }

    if (!params_.pfnDecodePicture(params_.pUserData, &pic)) return kNotSupported;

    // 更新 DPB
    if (is_reference) {
        update_dpb(next_picture_index_, frame_num, poc, is_reference, timestamp);
    }

    PendingPicture entry;
    entry.picture_index = static_cast<int>(next_picture_index_);
    entry.timestamp = timestamp;
    display_queue_.push_back(entry);

    next_picture_index_ = (next_picture_index_ + 1) % num_decode_surfaces_;

    pump_display_queue();
    return CUDA_SUCCESS;
}

void CuvidParser::update_dpb(int picture_index, int frame_num, int poc,
                              bool is_reference, int64_t timestamp) {
    // 如果 DPB 已满，移除最旧的参考帧
    if (dpb_.size() >= sps_->ref_frame_count) {
        dpb_.erase(dpb_.begin());
    }

    DpbEntry entry;
    entry.picture_index = picture_index;
    entry.frame_num = frame_num;
    entry.poc = poc;
    entry.is_long_term = false;
    entry.is_reference = is_reference;
    entry.timestamp = timestamp;
    dpb_.push_back(entry);
}

void CuvidParser::build_reference_lists(const CUVIDPICPARAMS &pic,
                                        std::vector<int> &list0,
                                        std::vector<int> &list1) {
    list0.clear();
    list1.clear();

    // 从 DPB 中收集参考帧，按 POC 排序
    std::vector<std::pair<int, int>> ref_frames; // (poc, dpb_index)
    for (int i = 0; i < static_cast<int>(dpb_.size()); ++i) {
        if (dpb_[i].is_reference) {
            ref_frames.push_back({dpb_[i].poc, i});
        }
    }

    // 按 POC 排序
    std::sort(ref_frames.begin(), ref_frames.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    int curr_poc = pic.CodecSpecific.h264.CurrFieldOrderCnt[0];

    // List0: 当前帧之前的参考帧（按 POC 降序）
    for (int i = static_cast<int>(ref_frames.size()) - 1; i >= 0; --i) {
        if (ref_frames[i].first < curr_poc) {
            list0.push_back(ref_frames[i].second);
        }
    }
    // 然后是当前帧之后的参考帧（按 POC 升序）
    for (int i = 0; i < static_cast<int>(ref_frames.size()); ++i) {
        if (ref_frames[i].first > curr_poc) {
            list0.push_back(ref_frames[i].second);
        }
    }

    // List1: 当前帧之后的参考帧（按 POC 升序）
    for (int i = 0; i < static_cast<int>(ref_frames.size()); ++i) {
        if (ref_frames[i].first > curr_poc) {
            list1.push_back(ref_frames[i].second);
        }
    }
    // 然后是当前帧之前的参考帧（按 POC 降序）
    for (int i = static_cast<int>(ref_frames.size()) - 1; i >= 0; --i) {
        if (ref_frames[i].first < curr_poc) {
            list1.push_back(ref_frames[i].second);
        }
    }
}

void CuvidParser::pump_display_queue() {
    if (!params_.pfnDisplayPicture || display_queue_.empty()) return;
    for (auto &entry : display_queue_) {
        CUVIDPARSERDISPINFO disp = {};
        disp.picture_index = entry.picture_index;
        disp.timestamp = entry.timestamp;
        disp.progressive_frame = 1;
        disp.top_field_first = 1;
        if (!params_.pfnDisplayPicture(params_.pUserData, &disp)) return;
    }
    display_queue_.clear();
}

} // namespace cuvid_shim
