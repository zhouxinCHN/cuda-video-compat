#include "cuvid_parser.h"

#include <cstdio>
#include <cstdlib>

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
        // EOS：把重排序队列里剩下的画面按 POC 顺序全部吐出去
        pump_display_queue(0, true);
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
            // Stash SPS NAL bytes for the Vulkan backend's cache.
            // 必须存**原始字节**（含防竞争字节 0x03）：nal.data 是去掉 EPB 的
            // RBSP，去掉 0x03 之后可能残留 00 00 01 这样的序列，被
            // ff_h2645_extract_rbsp 当成下一个 start code 直接截断，SPS 解析
            // 就崩了（"Overread VUI by 8 bits" / "sps_id 0 out of range"）。
            // raw_data/raw_size 才是 NAL 头起始、EPB 原样的字节。
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
            if (!ps_ready_) break; // wait for SPS/PPS
            if (!sequence_sent_) {
                CUresult result = send_sequence();
                if (result != CUDA_SUCCESS) { ff_h2645_packet_uninit(&pkt); return result; }
            }
            if (!num_decode_surfaces_) { ff_h2645_packet_uninit(&pkt); return CUDA_SUCCESS; }

            // 解析 slice header 获取 frame_num 和 POC（用去掉防竞争字节的 RBSP）
            int frame_num = 0;
            int poc = 0;
            bool is_reference = true;
            const bool is_idr = nal.type == NAL_SLICE_IDR;
            parse_slice_header(nal.data, nal.size, is_idr, &frame_num, &poc, &is_reference);

            // 但送给后端的码流必须是**原始字节**（含防竞争字节 0x03）：VA API 规范
            // 明确要求 slice data buffer 是 original bitstream，硬件自己解析
            // （slice_data_bit_offset 才按"去 EPB 后"计位）。H2645NAL 的
            // raw_data/raw_size 正好是 NAL 头起始的原始字节。
            const uint8_t *payload = nal.raw_data ? nal.raw_data : nal.data;
            const std::size_t payload_size =
                nal.raw_data ? static_cast<std::size_t>(nal.raw_size)
                             : static_cast<std::size_t>(nal.size);
            if (std::getenv("CUVID_SHIM_DEBUG_NAL")) {
                std::fprintf(stderr,
                             "SHIM NAL type=%d size=%zu payload=%zu first=%02x%02x%02x%02x "
                             "fn=%d poc=%d ref=%d ts=%lld\n",
                             (int)nal.type, (std::size_t)nal.size, payload_size,
                             payload_size > 0 ? payload[0] : 0, payload_size > 1 ? payload[1] : 0,
                             payload_size > 2 ? payload[2] : 0, payload_size > 3 ? payload[3] : 0,
                             frame_num, poc, (int)is_reference, (long long)timestamp);
            }
            CUresult result = send_picture(payload, payload_size, timestamp,
                                           frame_num, poc, is_reference, is_idr);
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
    // FFmpeg 的 SPS 里 crop_* 已经是"像素偏移"，而 CUVIDEOFORMAT.display_area
    // 是矩形坐标（含右/下边界）。以前直接填 crop_right/crop_bottom，
    // 无裁剪的流就变成 left=top=right=bottom=0，FFmpeg 的 cuviddec 据此算出
    // 0x0 的画面尺寸，直接 av_hwframe_ctx_init 失败（Picture size 0x0 is invalid）。
    format.display_area.left = static_cast<short>(sps_->crop_left);
    format.display_area.top = static_cast<short>(sps_->crop_top);
    format.display_area.right = static_cast<short>(format.coded_width - sps_->crop_right);
    format.display_area.bottom = static_cast<short>(format.coded_height - sps_->crop_bottom);
    format.frame_rate.numerator = 30000;
    format.frame_rate.denominator = 1001;
    format.progressive_sequence = sps_->frame_mbs_only_flag;
    // min_num_decode_surfaces：宿主（FFmpeg cuviddec）拿这个值 +3 当
    // ulNumDecodeSurfaces 用。以前这里留 0，FFmpeg 就退回默认的 5 个解码面，
    // 而本流要同时占用「4 个参考帧 + 重排序期间还没显示的 B 帧 + 当前解码面」，
    // 面池一紧就只能覆盖还在参考的帧，输出会退化成参考帧的拷贝。
    // NVDEC 的口径是"DPB 参考帧数 + 当前解码面 + 重排序余量"。
    int min_surfaces = static_cast<int>(sps_->ref_frame_count) + 3;
    if (const char *override_value = std::getenv("CUVID_SHIM_MIN_SURFACES"))
        min_surfaces = std::atoi(override_value);
    if (min_surfaces < 2) min_surfaces = 2;
    if (min_surfaces > 32) min_surfaces = 32;
    format.min_num_decode_surfaces = static_cast<unsigned char>(min_surfaces);

    int ret = params_.pfnSequenceCallback(params_.pUserData, &format);
    if (ret <= 0) return kNotSupported;
    num_decode_surfaces_ = static_cast<unsigned int>(ret);
    sequence_sent_ = true;
    return CUDA_SUCCESS;
}

CUresult CuvidParser::parse_slice_header(const uint8_t *nal_data, std::size_t nal_size,
                                         bool is_idr, int *frame_num, int *poc,
                                         bool *is_reference) {
    if (nal_size < 2) {
        *frame_num = 0;
        *poc = 0;
        *is_reference = true;
        return CUDA_SUCCESS;
    }

    // nal_data 是 ff_h2645_packet_split 切出来的裸 NAL（Annex-B 的 start code 已剥掉、
    // 防竞争字节已去掉），byte 0 就是 NAL header，slice header 从 byte 1 开始。
    // 不要在这里再调一次 ff_h2645_packet_split：它要求输入带 start code，
    // 对单个裸 NAL 一定失败并打印 "No start code is found."。
    std::size_t start_code_size = 0;
    if (nal_size >= 4 && nal_data[0] == 0 && nal_data[1] == 0 && nal_data[2] == 0 &&
        nal_data[3] == 1) {
        start_code_size = 4;
    } else if (nal_size >= 3 && nal_data[0] == 0 && nal_data[1] == 0 && nal_data[2] == 1) {
        start_code_size = 3;
    }
    if (start_code_size + 1 >= nal_size) {
        *frame_num = 0;
        *poc = 0;
        return CUDA_SUCCESS;
    }

    // nal_ref_idc = bits 5-6 of the NAL header byte
    *is_reference = ((nal_data[start_code_size] >> 5) & 0x3) != 0;

    GetBitContext gb;
    if (init_get_bits(&gb, nal_data + start_code_size + 1,
                      static_cast<int>((nal_size - start_code_size - 1) * 8)) < 0) {
        *frame_num = 0;
        *poc = 0;
        return CUDA_SUCCESS;
    }

    // first_mb_in_slice (ue)
    get_ue_golomb(&gb);
    // slice_type (ue)
    get_ue_golomb(&gb);
    // pic_parameter_set_id (ue)
    get_ue_golomb(&gb);
    // frame_num (u(v))
    *frame_num = get_bits(&gb, sps_->log2_max_frame_num);

    // IDR 的 POC 恒为 0，并且要清掉 POC/frame_num 的历史状态；
    // 否则 IDR 帧会带着上一段流的 prev_poc_msb_ 算出一个假的 POC
    //（本流实测 IDR 算成了 32），B 帧的 direct 预测和 DPB 排序全跟着错。
    if (is_idr) {
        prev_poc_lsb_ = 0;
        prev_poc_msb_ = 0;
        prev_frame_num_ = *frame_num;
        *poc = 0;
        return CUDA_SUCCESS;
    }

    // 非 IDR：frame_num 之后是 field_pic_flag / bottom_field_flag（仅隔行流）
    if (!sps_->frame_mbs_only_flag) {
        const int field_pic_flag = get_bits1(&gb);
        if (field_pic_flag) get_bits1(&gb); // bottom_field_flag
    }

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

    return CUDA_SUCCESS;
}

CUresult CuvidParser::send_picture(const uint8_t *nal_data, std::size_t nal_size,
                                   int64_t timestamp, int frame_num, int poc,
                                   bool is_reference, bool is_idr) {
    if (!params_.pfnDecodePicture || !sps_ || !pps_) return kNotSupported;

    // IDR 开启新的 GOP：先把上一段还在重排序队列里的画面全部按 POC 顺序
    // 显示出去，再清空 DPB。否则全是 IDR 的流（test_320x240.h264 的每帧 POC
    // 都是 0）会一直攒到 EOS 才吐，6 个 decode surface 早被反复覆盖，输出
    // 只剩最后几个面。IDR 是硬同步点，它之前的所有画面都已定序，可以安全 flush。
    if (is_idr) {
        pump_display_queue(0, true);
        dpb_.clear();
        prev_frame_num_ = 0;
        prev_poc_lsb_ = 0;
        prev_poc_msb_ = 0;
    }

    // 选输出 surface：不能简单地 0,1,2,... 轮转 32 个槽位，原因见
    // pick_decode_surface() 的注释。
    const unsigned int surface_index = pick_decode_surface();
    if (std::getenv("CUVID_SHIM_DEBUG_DPB")) {
        std::fprintf(stderr, "SHIM DPBSTATE poc=%d fn=%d ref=%d -> idx=%u dpb:", poc, frame_num,
                     (int)is_reference, surface_index);
        for (const auto &e : dpb_) std::fprintf(stderr, " [%d:poc%d]", e.picture_index, e.poc);
        std::fprintf(stderr, " disp:");
        for (const auto &e : display_queue_) std::fprintf(stderr, " [%d:poc%d]", e.picture_index, e.poc);
        std::fprintf(stderr, "\n");
    }

    CUVIDPICPARAMS pic = {};
    pic.PicWidthInMbs = sps_->mb_width;
    pic.FrameHeightInMbs = sps_->mb_height;
    pic.CurrPicIdx = static_cast<int>(surface_index);
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

    if (std::getenv("CUVID_SHIM_TRACE_SURF")) {
        std::fprintf(stderr, "SHIM DECODE idx=%u fn=%d poc=%d ref=%d idr=%d\n",
                     surface_index, frame_num, poc, (int)is_reference, (int)is_idr);
    }

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
    // 逆量化缩放矩阵：不放进来，VA 驱动的 IQ matrix 就是全 0，
    // 反量化系数被打成 0 → 每帧只剩帧内预测，输出平坦 128 灰。
    // PPS 没有 scaling list 时 FFmpeg 里已经是平坦默认值（16），直接拷即可。
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 16; ++j) h264.WeightScale4x4[i][j] = pps_->scaling_matrix4[i][j];
    }
    for (int j = 0; j < 64; ++j) {
        h264.WeightScale8x8[0][j] = pps_->scaling_matrix8[0][j];
        h264.WeightScale8x8[1][j] = pps_->scaling_matrix8[3][j];   // 与 FFmpeg vaapi 映射一致
    }
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
        update_dpb(static_cast<int>(surface_index), frame_num, poc, is_reference, timestamp);
    }

    PendingPicture entry;
    entry.picture_index = static_cast<int>(surface_index);
    entry.timestamp = timestamp;
    entry.poc = poc;
    // CUVID 的 pfnDisplayPicture 必须按**显示顺序**回调，不是解码顺序。
    // 以前这里是解完一帧立刻回调，P/B 交织的流（本测试流就是 IDR,P,B,B,B,...）
    // 出来的顺序就是解码顺序，和真实 NVDEC 不一致，宿主（FFmpeg cuviddec）
    // 拿到的帧序列也就是乱的。
    // 这里按 POC 有序插入，再把 "POC 小于刚解出的这一帧" 的都放出去——
    // 对渐进流来说这正好等价于 POC 升序输出，而且不会因为等待重排序把
    // surface 无限期占住（surface_in_use() 会把这些待显示帧也算进去）。
    auto insert_position = display_queue_.begin();
    while (insert_position != display_queue_.end() && insert_position->poc <= poc) {
        ++insert_position;
    }
    display_queue_.insert(insert_position, entry);

    pump_display_queue(poc);
    return CUDA_SUCCESS;
}

bool CuvidParser::surface_in_use(unsigned int index) const {
    for (const auto &entry : dpb_) {
        if (entry.picture_index == static_cast<int>(index)) return true;
    }
    for (const auto &entry : display_queue_) {
        if (entry.picture_index == static_cast<int>(index)) return true;
    }
    return false;
}

// 选一个可以安全写入的输出 surface。
//
// 为什么不是简单的 next_picture_index_ = (i + 1) % ulNumDecodeSurfaces：
// radeonsi 的 VCN 解码器给每个 context 维护一张 render_pic_list（DPB 槽位表，
// 见 mesa radeonsi/radeon_vcn_dec.c），H.264 只用到 ARRAY_SIZE(ref)+1 = 17 项。
// 每个"新的解码目标 surface"占一个槽位，槽位释放条件很苛刻；一旦申请表满，
// mesa 拿不到目标槽位（decoded_pic_idx = 0xff），就会另外分配一个内部 DPB 面
// 让固件写进去——我们传进去的 VA surface 从此一个字节都不写。
// 实测本机（gfx1100 / mesa 25.2）：第 16 个不同的输出面开始丢，表现为
// vaSyncSurface 之后整面全 0。FFmpeg 的 vaapi 解码不会踩到，是因为它的
// 输出面池小、面是循环复用的。
//
// 所以这里只在实际需要的面数（参考帧 + 当前帧 + 一点余量）里轮转：
//   * 不超过 kMaxLiveSurfaces（实测安全上限是 15，这里取 12）；
//   * 跳过还在 DPB 里当参考、或者还没显示出去的槽位；
//   * 万一窗口内全被占住，再退化成整个槽位空间里找空闲。
unsigned int CuvidParser::pick_decode_surface() {
    static constexpr unsigned int kMaxLiveSurfaces = 12;
    const unsigned int total = num_decode_surfaces_;
    if (total == 0) return 0;

    unsigned int window = total < kMaxLiveSurfaces ? total : kMaxLiveSurfaces;
    // 至少能装下全部参考帧加当前帧，否则必然覆盖到还在参考的帧
    const unsigned int needed = static_cast<unsigned int>(dpb_.size()) + 1;
    if (window < needed) window = total < needed ? total : needed;

    for (unsigned int i = 0; i < window; ++i) {
        const unsigned int index = (next_picture_index_ + i) % window;
        if (surface_in_use(index)) continue;
        next_picture_index_ = (index + 1) % window;
        return index;
    }
    for (unsigned int index = 0; index < total; ++index) {
        if (surface_in_use(index)) continue;
        next_picture_index_ = (index + 1) % total;
        return index;
    }
    const unsigned int index = next_picture_index_ % total;
    next_picture_index_ = (index + 1) % total;
    return index;
}

void CuvidParser::update_dpb(int picture_index, int frame_num, int poc,
                              bool is_reference, int64_t timestamp) {
    // num_ref_frames == 0 的流（例如只含 IDR 的 baseline 流）不保留参考帧。
    // 必须在这里直接返回：以前不判断就走到下面的 erase(dpb_.begin())，
    // 在空 vector 上 erase 会把 _M_finish 减一个元素，容器立刻损坏，
    // 下一次 push_back 就段错误（test_320x240.h264 实测就是这个崩的）。
    if (!sps_ || sps_->ref_frame_count <= 0) return;
    // 如果 DPB 已满，移除最旧的参考帧
    if (!dpb_.empty() && dpb_.size() >= static_cast<std::size_t>(sps_->ref_frame_count)) {
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

void CuvidParser::pump_display_queue(int newest_poc, bool flush_all) {
    if (!params_.pfnDisplayPicture || display_queue_.empty()) return;

    // display_queue_ 始终按 POC 升序排列。队列里所有 POC 严格小于
    // "最新解出那一帧"（newest_poc，调用方传入）的条目都已经是最终顺序，
    // 可以放出去；flush_all（EOS）时全部放出去。
    // 注意不能用 display_queue_.back().poc 当边界：队尾是队列里 POC 最大的
    // 那一帧（可能早就是队列里等着的），用它当边界会立刻把还在等重排序的
    // 画面放出去。
    size_t emit_count = 0;
    if (flush_all) {
        emit_count = display_queue_.size();
    } else {
        while (emit_count < display_queue_.size() &&
               display_queue_[emit_count].poc < newest_poc) {
            ++emit_count;
        }
    }
    if (!emit_count) return;

    std::vector<PendingPicture> emit(display_queue_.begin(),
                                     display_queue_.begin() + static_cast<long>(emit_count));
    display_queue_.erase(display_queue_.begin(),
                         display_queue_.begin() + static_cast<long>(emit_count));

    for (auto &entry : emit) {
        CUVIDPARSERDISPINFO disp = {};
        disp.picture_index = entry.picture_index;
        disp.timestamp = entry.timestamp;
        disp.progressive_frame = 1;
        disp.top_field_first = 1;
        if (std::getenv("CUVID_SHIM_TRACE_SURF")) {
            std::fprintf(stderr, "SHIM DISPLAY idx=%d poc=%d\n",
                         disp.picture_index, entry.poc);
        }
        // 回调返回 0 表示宿主要求停下来；此时剩下的条目留在队列里，下次再吐
        if (!params_.pfnDisplayPicture(params_.pUserData, &disp)) return;
    }
}

} // namespace cuvid_shim
