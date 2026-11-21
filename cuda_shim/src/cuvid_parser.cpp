#include "cuvid_parser.h"

#include "h264_decode_context.h"
#include "h264_surface_tracker.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "libavcodec/golomb.h"
}

namespace cuvid_shim {
namespace {

constexpr CUresult kNotSupported = static_cast<CUresult>(801);
constexpr CUresult kOutOfMemory = static_cast<CUresult>(2);
constexpr int NAL_SLICE = 1;
constexpr int NAL_SLICE_IDR = 5;
constexpr int NAL_SPS = 7;
constexpr int NAL_PPS = 8;

CUresult dpb_error(H264DpbStatus status) {
    switch (status) {
    case H264DpbStatus::kOk: return CUDA_SUCCESS;
    case H264DpbStatus::kUnsupported: return kNotSupported;
    case H264DpbStatus::kNoFreeSurface:
    case H264DpbStatus::kDpbOverflow:
        return kOutOfMemory;
    case H264DpbStatus::kInvalidArgument:
    case H264DpbStatus::kReferenceNotFound:
        return CUDA_ERROR_INVALID_VALUE;
    }
    return CUDA_ERROR_UNKNOWN;
}

} // namespace

CuvidParser::CuvidParser(const CUVIDPARSERPARAMS &params) : params_(params) {
    avctx_.codec_id = AV_CODEC_ID_H264;
}

CuvidParser::~CuvidParser() {
    if (ps_.sps_list[0] || ps_.pps_list[0]) ff_h264_ps_uninit(&ps_);
}

CUresult CuvidParser::parse(const CUVIDSOURCEDATAPACKET &packet) {
    if (packet.payload_size == 0) {
        CUresult result = flush_picture();
        if (result != CUDA_SUCCESS) return result;
        if (dpb_configured_) {
            const H264DpbStatus status = dpb_.drain();
            if (status != H264DpbStatus::kOk) return dpb_error(status);
        }
        return emit_outputs(true);
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
            // Snapshot fields before FFmpeg replaces the old RefStruct SPS.
            int old_mb_width = 0, old_mb_height = 0;
            int old_crop[4] = {0, 0, 0, 0};
            int old_chroma = 0, old_depth_luma = 0, old_depth_chroma = 0;
            const bool had_sps = sps_ != nullptr;
            if (had_sps) {
                old_mb_width = sps_->mb_width;
                old_mb_height = sps_->mb_height;
                old_crop[0] = sps_->crop_left;
                old_crop[1] = sps_->crop_right;
                old_crop[2] = sps_->crop_top;
                old_crop[3] = sps_->crop_bottom;
                old_chroma = sps_->chroma_format_idc;
                old_depth_luma = sps_->bit_depth_luma;
                old_depth_chroma = sps_->bit_depth_chroma;
            }

            GetBitContext gb = nal.gb;
            ret = ff_h264_decode_seq_parameter_set(&gb, &avctx_, &ps_, 0);
            if (ret < 0) { ff_h2645_packet_uninit(&pkt); return kNotSupported; }
            const SPS *new_sps = nullptr;
            for (int k = 0; k < MAX_SPS_COUNT; ++k) {
                if (ps_.sps_list[k]) { new_sps = ps_.sps_list[k]; break; }
            }
            const bool sequence_changed =
                had_sps && new_sps &&
                (new_sps->mb_width != old_mb_width ||
                 new_sps->mb_height != old_mb_height ||
                 new_sps->crop_left != old_crop[0] ||
                 new_sps->crop_right != old_crop[1] ||
                 new_sps->crop_top != old_crop[2] ||
                 new_sps->crop_bottom != old_crop[3] ||
                 new_sps->chroma_format_idc != old_chroma ||
                 new_sps->bit_depth_luma != old_depth_luma ||
                 new_sps->bit_depth_chroma != old_depth_chroma);
            if (sequence_changed) {
                CUresult result = flush_picture();
                if (result != CUDA_SUCCESS) { ff_h2645_packet_uninit(&pkt); return result; }
                if (dpb_configured_) {
                    const H264DpbStatus status = dpb_.drain();
                    if (status != H264DpbStatus::kOk) {
                        ff_h2645_packet_uninit(&pkt);
                        return dpb_error(status);
                    }
                }
                result = emit_outputs(true);
                if (result != CUDA_SUCCESS) { ff_h2645_packet_uninit(&pkt); return result; }
                num_decode_surfaces_ = 0;
                sequence_sent_ = false;
                dpb_configured_ = false;
            }
            sps_ = new_sps;
            ps_ready_ = false;
            {
                const uint8_t *src = nal.raw_data ? nal.raw_data : nal.data;
                const std::size_t n = nal.raw_data ? static_cast<std::size_t>(nal.raw_size)
                                                   : static_cast<std::size_t>(nal.size);
                pending_sps_.assign(src, src + n);
            }
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
            {
                const uint8_t *src = nal.raw_data ? nal.raw_data : nal.data;
                const std::size_t n = nal.raw_data ? static_cast<std::size_t>(nal.raw_size)
                                                   : static_cast<std::size_t>(nal.size);
                pending_pps_.assign(src, src + n);
            }
            if (!pending_sps_.empty() && !pending_pps_.empty())
                VkSequenceCache::store(pending_sps_.data(), pending_sps_.size(),
                                       pending_pps_.data(), pending_pps_.size());
            break;
        }
        case NAL_SLICE:
        case NAL_SLICE_IDR: {
            if (!ps_ready_) break;
            if (!sequence_sent_) {
                CUresult result = send_sequence();
                if (result != CUDA_SUCCESS) { ff_h2645_packet_uninit(&pkt); return result; }
            }
            if (!num_decode_surfaces_) { ff_h2645_packet_uninit(&pkt); return CUDA_SUCCESS; }

            H264Slice syntax;
            int first_mb = 0;
            CUresult result = parse_slice_header(nal, &syntax, &first_mb);
            if (result != CUDA_SUCCESS) { ff_h2645_packet_uninit(&pkt); return result; }

            const uint8_t *payload = nal.raw_data ? nal.raw_data : nal.data;
            const std::size_t payload_size =
                nal.raw_data ? static_cast<std::size_t>(nal.raw_size)
                             : static_cast<std::size_t>(nal.size);
            if (std::getenv("CUVID_SHIM_DEBUG_NAL")) {
                std::fprintf(stderr,
                             "SHIM NAL type=%d size=%zu payload=%zu first=%02x%02x%02x%02x "
                             "fn=%u poc_lsb=%u ref=%d ts=%lld\n",
                             (int)nal.type, (std::size_t)nal.size, payload_size,
                             payload_size > 0 ? payload[0] : 0, payload_size > 1 ? payload[1] : 0,
                             payload_size > 2 ? payload[2] : 0, payload_size > 3 ? payload[3] : 0,
                             syntax.frame_num, syntax.poc_lsb, (int)syntax.is_reference,
                             (long long)timestamp);
            }
            if (first_mb == 0) {
                result = flush_picture();
                if (result != CUDA_SUCCESS) { ff_h2645_packet_uninit(&pkt); return result; }
                pending_picture_.timestamp = timestamp;
                pending_picture_.syntax = std::move(syntax);
                pending_picture_active_ = true;
            } else if (!pending_picture_active_) {
                if (std::getenv("CUVID_SHIM_DEBUG_NAL")) {
                    std::fprintf(stderr,
                                 "SHIM NAL continuation slice without picture start (first_mb=%d)\n",
                                 first_mb);
                }
                break;
            }

            static const uint8_t start_code[4] = {0, 0, 0, 1};
            slice_offsets_.push_back(static_cast<uint32_t>(slice_buffer_.size()));
            slice_buffer_.insert(slice_buffer_.end(), start_code, start_code + 4);
            slice_buffer_.insert(slice_buffer_.end(), payload, payload + payload_size);
            break;
        }
        default:
            break;
        }
    }

    ff_h2645_packet_uninit(&pkt);
    return flush_picture();
}

CUresult CuvidParser::send_sequence() {
    if (!params_.pfnSequenceCallback || !sps_) return kNotSupported;

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
    format.display_area.right = static_cast<short>(format.coded_width - sps_->crop_right);
    format.display_area.bottom = static_cast<short>(format.coded_height - sps_->crop_bottom);
    format.frame_rate.numerator = 30000;
    format.frame_rate.denominator = 1001;
    format.progressive_sequence = sps_->frame_mbs_only_flag;
    int min_surfaces = static_cast<int>(sps_->ref_frame_count) + 3;
    if (const char *override_value = std::getenv("CUVID_SHIM_MIN_SURFACES"))
        min_surfaces = std::atoi(override_value);
    min_surfaces = std::clamp(min_surfaces, 2, 32);
    format.min_num_decode_surfaces = static_cast<unsigned char>(min_surfaces);

    int ret = params_.pfnSequenceCallback(params_.pUserData, &format);
    if (ret <= 0) return kNotSupported;
    num_decode_surfaces_ = static_cast<unsigned int>(ret);
    CUresult result = configure_dpb();
    if (result != CUDA_SUCCESS) return result;
    sequence_sent_ = true;
    return CUDA_SUCCESS;
}

CUresult CuvidParser::configure_dpb() {
    if (!sps_) return kNotSupported;
    H264Sequence sequence;
    H264DpbStatus status = h264_sequence_from_ffmpeg(
        *sps_, h264_dpb_capacity_from_ffmpeg(*sps_), &sequence);
    if (status != H264DpbStatus::kOk) return dpb_error(status);
    status = dpb_.reset(sequence);
    if (status != H264DpbStatus::kOk) return dpb_error(status);
    dpb_configured_ = true;
    return CUDA_SUCCESS;
}

CUresult CuvidParser::parse_slice_header(const H2645NAL &nal, H264Slice *slice,
                                         int *first_mb) {
    if (!slice || !first_mb || !sps_ || !pps_) return CUDA_ERROR_INVALID_VALUE;
    GetBitContext first = nal.gb;
    *first_mb = static_cast<int>(get_ue_golomb_long(&first));
    if (get_bits_left(&first) < 0) return CUDA_ERROR_INVALID_VALUE;

    H264SliceParseParams params;
    H264DpbStatus status = h264_slice_params_from_ffmpeg(*sps_, *pps_, &params);
    if (status != H264DpbStatus::kOk) return dpb_error(status);
    status = parse_h264_slice_header(nal.gb, nal.type, nal.ref_idc, params, slice);
    return dpb_error(status);
}

CUresult CuvidParser::flush_picture() {
    if (!pending_picture_active_ || slice_buffer_.empty()) {
        slice_buffer_.clear();
        slice_offsets_.clear();
        pending_picture_active_ = false;
        return CUDA_SUCCESS;
    }

    CUresult result = send_picture(slice_buffer_.data(), slice_buffer_.size());
    slice_buffer_.clear();
    slice_offsets_.clear();
    pending_picture_active_ = false;
    return result;
}

CUresult CuvidParser::send_picture(const uint8_t *nal_data, std::size_t nal_size) {
    if (!params_.pfnDecodePicture || !sps_ || !pps_ || !dpb_configured_) {
        return kNotSupported;
    }
    const H264Slice &syntax = pending_picture_.syntax;
    H264Dpb rollback = dpb_;

    int surface_index = -1;
    H264DpbStatus status = dpb_.choose_surface(
        num_decode_surfaces_, H264SurfaceTracker::snapshot(this, num_decode_surfaces_),
        &surface_index);
    if (status != H264DpbStatus::kOk) return dpb_error(status);

    H264Picture current;
    status = dpb_.begin_picture(syntax, surface_index, pending_picture_.timestamp, &current);
    if (status != H264DpbStatus::kOk) return dpb_error(status);

    // IDR/MMCO boundaries drain the preceding POC sequence before new decode.
    if (syntax.is_idr) {
        CUresult result = emit_outputs(true);
        if (result != CUDA_SUCCESS) {
            dpb_ = std::move(rollback);
            return result;
        }
    }

    CUVIDPICPARAMS pic = {};
    pic.PicWidthInMbs = sps_->mb_width;
    pic.FrameHeightInMbs = sps_->mb_height;
    pic.CurrPicIdx = current.picture_index;
    pic.field_pic_flag = 0;
    pic.bottom_field_flag = 0;
    pic.second_field = 0;
    pic.ref_pic_flag = syntax.is_reference ? 1 : 0;
    pic.intra_pic_flag = syntax.type == H264SliceType::kI ? 1 : 0;
    pic.nBitstreamDataLen = static_cast<unsigned int>(nal_size);
    pic.pBitstreamData = const_cast<uint8_t *>(nal_data);
    pic.nNumSlices = static_cast<unsigned int>(slice_offsets_.size());
    if (pic.nNumSlices == 0) return CUDA_ERROR_INVALID_VALUE;
    pic.pSliceDataOffsets = slice_offsets_.data();

    auto &h264 = pic.CodecSpecific.h264;
    h264.log2_max_frame_num_minus4 = sps_->log2_max_frame_num - 4;
    h264.pic_order_cnt_type = sps_->poc_type;
    h264.log2_max_pic_order_cnt_lsb_minus4 = sps_->poc_type == 0 ? sps_->log2_max_poc_lsb - 4 : 0;
    h264.delta_pic_order_always_zero_flag = sps_->delta_pic_order_always_zero_flag;
    h264.frame_mbs_only_flag = sps_->frame_mbs_only_flag;
    h264.direct_8x8_inference_flag = sps_->direct_8x8_inference_flag;
    h264.num_ref_frames = sps_->ref_frame_count;
    h264.bit_depth_luma_minus8 = static_cast<unsigned char>(sps_->bit_depth_luma - 8);
    h264.bit_depth_chroma_minus8 = static_cast<unsigned char>(sps_->bit_depth_chroma - 8);
    h264.entropy_coding_mode_flag = pps_->cabac;
    h264.pic_order_present_flag = pps_->pic_order_present;
    h264.num_ref_idx_l0_active_minus1 = pps_->ref_count[0] - 1;
    h264.num_ref_idx_l1_active_minus1 = pps_->ref_count[1] - 1;
    h264.weighted_pred_flag = pps_->weighted_pred;
    h264.weighted_bipred_idc = pps_->weighted_bipred_idc;
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 16; ++j) h264.WeightScale4x4[i][j] = pps_->scaling_matrix4[i][j];
    }
    for (int j = 0; j < 64; ++j) {
        h264.WeightScale8x8[0][j] = pps_->scaling_matrix8[0][j];
        h264.WeightScale8x8[1][j] = pps_->scaling_matrix8[3][j];
    }
    h264.pic_init_qp_minus26 = pps_->init_qp - 26;
    h264.deblocking_filter_control_present_flag = pps_->deblocking_filter_parameters_present;
    h264.redundant_pic_cnt_present_flag = pps_->redundant_pic_cnt_present;
    h264.transform_8x8_mode_flag = pps_->transform_8x8_mode;
    h264.MbaffFrameFlag = sps_->mb_aff;
    h264.constrained_intra_pred_flag = pps_->constrained_intra_pred;
    h264.chroma_qp_index_offset = pps_->chroma_qp_index_offset[0];
    h264.second_chroma_qp_index_offset = pps_->chroma_qp_index_offset[1];
    h264.ref_pic_flag = syntax.is_reference ? 1 : 0;
    h264.frame_num = current.frame_num;
    h264.CurrFieldOrderCnt[0] = current.top_field_order_cnt;
    h264.CurrFieldOrderCnt[1] = current.bottom_field_order_cnt;

    for (int i = 0; i < 16; ++i) h264.dpb[i].PicIdx = -1;
    int dpb_index = 0;
    for (const H264Picture &entry : dpb_.pictures()) {
        if (entry.reference == H264ReferenceType::kUnused || dpb_index >= 16) continue;
        auto &output = h264.dpb[dpb_index++];
        output.PicIdx = entry.picture_index;
        output.FrameIdx = entry.reference == H264ReferenceType::kLongTerm
                              ? static_cast<int>(entry.long_term_frame_idx)
                              : entry.frame_num;
        output.is_long_term = entry.reference == H264ReferenceType::kLongTerm ? 1 : 0;
        output.not_existing = 0;
        output.used_for_reference = 1;
        output.FieldOrderCnt[0] = entry.top_field_order_cnt;
        output.FieldOrderCnt[1] = entry.bottom_field_order_cnt;
    }

    if (std::getenv("CUVID_SHIM_TRACE_SURF")) {
        std::fprintf(stderr, "SHIM DECODE idx=%d fn=%d poc=%d ref=%d idr=%d\n",
                     current.picture_index, current.frame_num, current.poc,
                     (int)syntax.is_reference, (int)syntax.is_idr);
    }

    bool decoded = false;
    {
        const H264DecodeContext context{&dpb_, &syntax, &current, this};
        ScopedH264DecodeContext scope(context);
        decoded = params_.pfnDecodePicture(params_.pUserData, &pic) != 0;
    }
    if (!decoded) {
        dpb_ = std::move(rollback);
        return kNotSupported;
    }

    status = dpb_.finish_picture(syntax, &current);
    if (status != H264DpbStatus::kOk) {
        dpb_ = std::move(rollback);
        return dpb_error(status);
    }
    return emit_outputs(false);
}

CUresult CuvidParser::emit_outputs(bool flush) {
    if (!dpb_configured_ || !params_.pfnDisplayPicture) return CUDA_SUCCESS;
    std::vector<H264Output> outputs = dpb_.take_outputs(params_.ulMaxDisplayDelay, flush);
    for (const H264Output &output : outputs) {
        CUVIDPARSERDISPINFO disp = {};
        disp.picture_index = output.picture_index;
        disp.timestamp = output.timestamp;
        disp.progressive_frame = 1;
        disp.top_field_first = 1;
        if (std::getenv("CUVID_SHIM_TRACE_SURF")) {
            std::fprintf(stderr, "SHIM DISPLAY idx=%d poc=%d\n", output.picture_index,
                         output.poc);
        }
        H264SurfaceTracker::mark(this, output.picture_index, true);
        if (!params_.pfnDisplayPicture(params_.pUserData, &disp)) {
            H264SurfaceTracker::mark(this, output.picture_index, false);
            return kNotSupported;
        }
    }
    return CUDA_SUCCESS;
}

} // namespace cuvid_shim
