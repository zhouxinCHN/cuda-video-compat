#include "vulkan_h264_decoder.h"

#include "hip_frame_mapper.h"
#include "vk_sequence_cache.h"

#include <vulkan/vulkan.h>
#include <vk_video/vulkan_video_codec_h264std.h>
#include <vk_video/vulkan_video_codec_h264std_decode.h>

#include <hip/hip_runtime_api.h>

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <algorithm>
#include <unistd.h>

// FFmpeg parser reuse (compile-time headers; link-time 9 .o + libavutil.a).
extern "C" {
#define HAVE_AV_CONFIG_H 1
#include "libavcodec/avcodec.h"
#include "libavcodec/h2645_parse.h"
#include "libavcodec/h264_ps.h"
#include "libavcodec/h264_parse.h"   // ff_h264_init_poc / ff_h264_parse_ref_count
#include "libavcodec/h264.h"
#include "libavcodec/golomb.h"
#include "libavcodec/mpegutils.h"    // PICT_FRAME
#include "libavcodec/mathops.h"      // ff_zigzag_scan / ff_zigzag_direct
}

namespace cuvid_shim {
namespace {

constexpr CUresult kNotSupported = static_cast<CUresult>(801);
#ifndef CUDA_ERROR_OUT_OF_MEMORY
#define CUDA_ERROR_OUT_OF_MEMORY static_cast<CUresult>(2)
#endif

// --- Vulkan video command PFNs (radv doesn't export these via the loader) ---
#define DECL_PFN(name) static PFN_##name p##name = nullptr
DECL_PFN(vkGetPhysicalDeviceVideoCapabilitiesKHR);
DECL_PFN(vkGetPhysicalDeviceVideoFormatPropertiesKHR);
DECL_PFN(vkCreateVideoSessionKHR);
DECL_PFN(vkDestroyVideoSessionKHR);
DECL_PFN(vkGetVideoSessionMemoryRequirementsKHR);
DECL_PFN(vkBindVideoSessionMemoryKHR);
DECL_PFN(vkCreateVideoSessionParametersKHR);
DECL_PFN(vkDestroyVideoSessionParametersKHR);
DECL_PFN(vkCmdBeginVideoCodingKHR);
DECL_PFN(vkCmdEndVideoCodingKHR);
DECL_PFN(vkCmdControlVideoCodingKHR);
DECL_PFN(vkCmdDecodeVideoKHR);
DECL_PFN(vkGetMemoryFdKHR);
#undef DECL_PFN

bool load_video_pfns(VkInstance instance, VkDevice device) {
    // vkGetPhysicalDevice* 虽然形参是 VkPhysicalDevice，但在 Vulkan 里属于
    // instance 级命令，必须用 vkGetInstanceProcAddr 查；用 vkGetDeviceProcAddr
    // 会拿到空指针。radv 又不通过 loader 导出这些入口，所以之前整条链路
    // 直接在这里失败（cuvidCreateDecoder 返回 801）。
#define LOAD_INST(nm) p##nm = reinterpret_cast<PFN_##nm>(vkGetInstanceProcAddr(instance, #nm)); \
    if (!p##nm) return false;
#define LOAD_DEV(nm) p##nm = reinterpret_cast<PFN_##nm>(vkGetDeviceProcAddr(device, #nm)); \
    if (!p##nm) return false;
    LOAD_INST(vkGetPhysicalDeviceVideoCapabilitiesKHR);
    LOAD_INST(vkGetPhysicalDeviceVideoFormatPropertiesKHR);
    LOAD_DEV(vkCreateVideoSessionKHR);
    LOAD_DEV(vkDestroyVideoSessionKHR);
    LOAD_DEV(vkGetVideoSessionMemoryRequirementsKHR);
    LOAD_DEV(vkBindVideoSessionMemoryKHR);
    LOAD_DEV(vkCreateVideoSessionParametersKHR);
    LOAD_DEV(vkDestroyVideoSessionParametersKHR);
    LOAD_DEV(vkCmdBeginVideoCodingKHR);
    LOAD_DEV(vkCmdEndVideoCodingKHR);
    LOAD_DEV(vkCmdControlVideoCodingKHR);
    LOAD_DEV(vkCmdDecodeVideoKHR);
    LOAD_DEV(vkGetMemoryFdKHR);
#undef LOAD_INST
#undef LOAD_DEV
    return true;
}

// --- SPS/PPS -> Std conversion (from P0 verify_vk_decode_to_hip.cpp) ---

StdVideoH264LevelIdc convert_to_vk_level_idc(int level_idc) {
    switch (level_idc) {
    case 10: return STD_VIDEO_H264_LEVEL_IDC_1_0;
    case 11: return STD_VIDEO_H264_LEVEL_IDC_1_1;
    case 12: return STD_VIDEO_H264_LEVEL_IDC_1_2;
    case 13: return STD_VIDEO_H264_LEVEL_IDC_1_3;
    case 20: return STD_VIDEO_H264_LEVEL_IDC_2_0;
    case 21: return STD_VIDEO_H264_LEVEL_IDC_2_1;
    case 22: return STD_VIDEO_H264_LEVEL_IDC_2_2;
    case 30: return STD_VIDEO_H264_LEVEL_IDC_3_0;
    case 31: return STD_VIDEO_H264_LEVEL_IDC_3_1;
    case 32: return STD_VIDEO_H264_LEVEL_IDC_3_2;
    case 40: return STD_VIDEO_H264_LEVEL_IDC_4_0;
    case 41: return STD_VIDEO_H264_LEVEL_IDC_4_1;
    case 42: return STD_VIDEO_H264_LEVEL_IDC_4_2;
    case 50: return STD_VIDEO_H264_LEVEL_IDC_5_0;
    case 51: return STD_VIDEO_H264_LEVEL_IDC_5_1;
    case 52: return STD_VIDEO_H264_LEVEL_IDC_5_2;
    default: return STD_VIDEO_H264_LEVEL_IDC_5_1;
    }
}

const int kScalingList8Order[6] = { 0, 3, 1, 4, 2, 5 };

void fill_sps_std(const SPS *sps, StdVideoH264ScalingLists *sl,
                  StdVideoH264SequenceParameterSetVui *vui,
                  StdVideoH264SequenceParameterSet *out) {
    std::memset(sl, 0, sizeof(*sl));
    std::memset(vui, 0, sizeof(*vui));
    std::memset(out, 0, sizeof(*out));

    sl->scaling_list_present_mask = sps->scaling_matrix_present_mask;
    sl->use_default_scaling_matrix_mask = 0;
    for (int i = 0; i < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_LISTS; i++)
        for (int j = 0; j < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_ELEMENTS; j++)
            sl->ScalingList4x4[i][j] = sps->scaling_matrix4[i][ff_zigzag_scan[j]];
    for (int i = 0; i < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_LISTS; i++)
        for (int j = 0; j < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_ELEMENTS; j++)
            sl->ScalingList8x8[i][j] = sps->scaling_matrix8[kScalingList8Order[i]][ff_zigzag_direct[j]];

    *vui = (StdVideoH264SequenceParameterSetVui){
        .flags = (StdVideoH264SpsVuiFlags){
            .aspect_ratio_info_present_flag = sps->vui.aspect_ratio_info_present_flag,
            .overscan_info_present_flag = sps->vui.overscan_info_present_flag,
            .overscan_appropriate_flag = sps->vui.overscan_appropriate_flag,
            .video_signal_type_present_flag = sps->vui.video_signal_type_present_flag,
            .video_full_range_flag = sps->vui.video_full_range_flag,
            .color_description_present_flag = sps->vui.colour_description_present_flag,
            .chroma_loc_info_present_flag = sps->vui.chroma_loc_info_present_flag,
            .timing_info_present_flag = sps->timing_info_present_flag,
            .fixed_frame_rate_flag = sps->fixed_frame_rate_flag,
            .bitstream_restriction_flag = sps->bitstream_restriction_flag,
            .nal_hrd_parameters_present_flag = sps->nal_hrd_parameters_present_flag,
            .vcl_hrd_parameters_present_flag = sps->vcl_hrd_parameters_present_flag,
        },
        .aspect_ratio_idc = static_cast<StdVideoH264AspectRatioIdc>(sps->vui.aspect_ratio_idc),
        .sar_width = static_cast<uint16_t>(sps->vui.sar.num),
        .sar_height = static_cast<uint16_t>(sps->vui.sar.den),
        .video_format = static_cast<uint8_t>(sps->vui.video_format),
        .colour_primaries = static_cast<uint8_t>(sps->vui.colour_primaries),
        .transfer_characteristics = static_cast<uint8_t>(sps->vui.transfer_characteristics),
        .matrix_coefficients = static_cast<uint8_t>(sps->vui.matrix_coeffs),
        .num_units_in_tick = sps->num_units_in_tick,
        .time_scale = sps->time_scale,
        .max_num_reorder_frames = static_cast<uint8_t>(sps->num_reorder_frames),
        .max_dec_frame_buffering = static_cast<uint8_t>(sps->max_dec_frame_buffering),
        .chroma_sample_loc_type_top_field = static_cast<uint8_t>(sps->vui.chroma_sample_loc_type_top_field),
        .chroma_sample_loc_type_bottom_field = static_cast<uint8_t>(sps->vui.chroma_sample_loc_type_bottom_field),
    };

    *out = (StdVideoH264SequenceParameterSet){
        .flags = (StdVideoH264SpsFlags){
            .constraint_set0_flag = (sps->constraint_set_flags >> 0) & 0x1,
            .constraint_set1_flag = (sps->constraint_set_flags >> 1) & 0x1,
            .constraint_set2_flag = (sps->constraint_set_flags >> 2) & 0x1,
            .constraint_set3_flag = (sps->constraint_set_flags >> 3) & 0x1,
            .constraint_set4_flag = (sps->constraint_set_flags >> 4) & 0x1,
            .constraint_set5_flag = (sps->constraint_set_flags >> 5) & 0x1,
            .direct_8x8_inference_flag = sps->direct_8x8_inference_flag,
            .mb_adaptive_frame_field_flag = sps->mb_aff,
            .frame_mbs_only_flag = sps->frame_mbs_only_flag,
            .delta_pic_order_always_zero_flag = sps->delta_pic_order_always_zero_flag,
            .separate_colour_plane_flag = sps->residual_color_transform_flag,
            .gaps_in_frame_num_value_allowed_flag = sps->gaps_in_frame_num_allowed_flag,
            .qpprime_y_zero_transform_bypass_flag = sps->transform_bypass,
            .frame_cropping_flag = sps->crop,
            .seq_scaling_matrix_present_flag = sps->scaling_matrix_present,
            .vui_parameters_present_flag = sps->vui_parameters_present_flag,
        },
        .profile_idc = static_cast<StdVideoH264ProfileIdc>(sps->profile_idc),
        .level_idc = convert_to_vk_level_idc(sps->level_idc),
        .chroma_format_idc = static_cast<StdVideoH264ChromaFormatIdc>(sps->chroma_format_idc),
        .seq_parameter_set_id = static_cast<uint8_t>(sps->sps_id),
        .bit_depth_luma_minus8 = static_cast<uint8_t>(sps->bit_depth_luma - 8),
        .bit_depth_chroma_minus8 = static_cast<uint8_t>(sps->bit_depth_chroma - 8),
        .log2_max_frame_num_minus4 = static_cast<uint8_t>(sps->log2_max_frame_num - 4),
        .pic_order_cnt_type = static_cast<StdVideoH264PocType>(sps->poc_type),
        .offset_for_non_ref_pic = sps->offset_for_non_ref_pic,
        .offset_for_top_to_bottom_field = sps->offset_for_top_to_bottom_field,
        .log2_max_pic_order_cnt_lsb_minus4 = static_cast<uint8_t>(sps->poc_type ? 0 : sps->log2_max_poc_lsb - 4),
        .num_ref_frames_in_pic_order_cnt_cycle = static_cast<uint8_t>(sps->poc_cycle_length),
        .max_num_ref_frames = static_cast<uint8_t>(sps->ref_frame_count),
        .pic_width_in_mbs_minus1 = static_cast<uint32_t>(sps->mb_width - 1),
        .pic_height_in_map_units_minus1 = static_cast<uint32_t>(sps->mb_height / (2 - sps->frame_mbs_only_flag) - 1),
        .frame_crop_left_offset = sps->crop_left,
        .frame_crop_right_offset = sps->crop_right,
        .frame_crop_top_offset = sps->crop_top,
        .frame_crop_bottom_offset = sps->crop_bottom,
        .pOffsetForRefFrame = sps->offset_for_ref_frame,
        .pScalingLists = sl,
        .pSequenceParameterSetVui = vui,
    };
}

void fill_pps_std(const PPS *pps, StdVideoH264ScalingLists *sl,
                  StdVideoH264PictureParameterSet *out) {
    std::memset(sl, 0, sizeof(*sl));
    std::memset(out, 0, sizeof(*out));

    sl->scaling_list_present_mask = pps->pic_scaling_matrix_present_mask;
    sl->use_default_scaling_matrix_mask = 0;
    for (int i = 0; i < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_LISTS; i++)
        for (int j = 0; j < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_ELEMENTS; j++)
            sl->ScalingList4x4[i][j] = pps->scaling_matrix4[i][ff_zigzag_scan[j]];
    for (int i = 0; i < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_LISTS; i++)
        for (int j = 0; j < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_ELEMENTS; j++)
            sl->ScalingList8x8[i][j] = pps->scaling_matrix8[kScalingList8Order[i]][ff_zigzag_direct[j]];

    *out = (StdVideoH264PictureParameterSet){
        .flags = (StdVideoH264PpsFlags){
            .transform_8x8_mode_flag = pps->transform_8x8_mode,
            .redundant_pic_cnt_present_flag = pps->redundant_pic_cnt_present,
            .constrained_intra_pred_flag = pps->constrained_intra_pred,
            .deblocking_filter_control_present_flag = pps->deblocking_filter_parameters_present,
            .weighted_pred_flag = pps->weighted_pred,
            .bottom_field_pic_order_in_frame_present_flag = pps->pic_order_present,
            .entropy_coding_mode_flag = pps->cabac,
            .pic_scaling_matrix_present_flag = pps->pic_scaling_matrix_present_flag,
        },
        .seq_parameter_set_id = static_cast<uint8_t>(pps->sps_id),
        .pic_parameter_set_id = static_cast<uint8_t>(pps->pps_id),
        .num_ref_idx_l0_default_active_minus1 = static_cast<uint8_t>(pps->ref_count[0] - 1),
        .num_ref_idx_l1_default_active_minus1 = static_cast<uint8_t>(pps->ref_count[1] - 1),
        .weighted_bipred_idc = static_cast<StdVideoH264WeightedBipredIdc>(pps->weighted_bipred_idc),
        .pic_init_qp_minus26 = static_cast<int8_t>(pps->init_qp - 26),
        .pic_init_qs_minus26 = static_cast<int8_t>(pps->init_qs - 26),
        .chroma_qp_index_offset = static_cast<int8_t>(pps->chroma_qp_index_offset[0]),
        .second_chroma_qp_index_offset = static_cast<int8_t>(pps->chroma_qp_index_offset[1]),
        .pScalingLists = sl,
    };
}

// --- slice header parse (from P0; fills StdVideoDecodeH264PictureInfo inputs) ---

struct MMCOOp { int opcode; int short_pic_num; int long_arg; };

struct SliceInfo {
    int nal_type = 0;
    int nal_ref_idc = 0;
    int slice_type_nos = 0;   // AV_PICTURE_TYPE_I/P/B
    int frame_num = 0;
    int idr_pic_id = 0;
    int field_pic_flag = 0;
    int bottom_field_flag = 0;
    int poc_lsb = 0;
    int delta_poc_bottom = 0;
    int delta_poc[2] = {0, 0};
    int nb_mmco = 0;
    MMCOOp mmco[32] = {};
    int explicit_ref_marking = 0;
    int ref_count[2] = {0, 0};
};

bool parse_slice_header(const H2645NAL *nal, const SPS *sps, const PPS *pps, SliceInfo *si) {
    auto dbg = [](const char *s) { if (std::getenv("CUVID_SHIM_DEBUG_VK")) std::fprintf(stderr, "[vk-slice] %s\n", s); };
    std::memset(si, 0, sizeof(*si));
    GetBitContext gb = nal->gb;
    si->nal_type = nal->type;
    si->nal_ref_idc = nal->ref_idc;

    get_ue_golomb_long(&gb);
    unsigned st = get_ue_golomb_31(&gb);
    if (st > 9) { dbg("bad slice_type"); return false; }
    if (st > 4) st -= 5;
    static const int to_pict[5] = { AV_PICTURE_TYPE_P, AV_PICTURE_TYPE_B, AV_PICTURE_TYPE_I,
                                    AV_PICTURE_TYPE_SP, AV_PICTURE_TYPE_SI };
    si->slice_type_nos = to_pict[st] & 3;
    if (si->slice_type_nos == AV_PICTURE_TYPE_SP || si->slice_type_nos == AV_PICTURE_TYPE_SI)
        { dbg("SP/SI unsupported"); return false; }   // SP/SI out of scope

    get_ue_golomb(&gb);   // pic_parameter_set_id
    si->frame_num = get_bits(&gb, sps->log2_max_frame_num);
    if (!sps->frame_mbs_only_flag) { dbg("interlaced unsupported"); return false; }   // interlaced unsupported

    if (nal->type == H264_NAL_IDR_SLICE)
        si->idr_pic_id = get_ue_golomb_long(&gb);

    if (sps->poc_type == 0) {
        si->poc_lsb = get_bits(&gb, sps->log2_max_poc_lsb);
        if (pps->pic_order_present == 1)
            si->delta_poc_bottom = get_se_golomb(&gb);
    } else if (sps->poc_type != 2) {
        dbg("unsupported poc_type"); return false;
    }

    if (pps->redundant_pic_cnt_present) get_ue_golomb(&gb);
    if (si->slice_type_nos == AV_PICTURE_TYPE_B) get_bits1(&gb);   // direct_spatial_mv_pred

    if (si->slice_type_nos != AV_PICTURE_TYPE_I) {
        int list_count = 0;
        if (ff_h264_parse_ref_count(&list_count, si->ref_count, &gb, pps, si->slice_type_nos,
                                    PICT_FRAME, nullptr) < 0) {
            dbg("ref count parse failed"); return false;
        }
        for (int list = 0; list < list_count; list++) {
            if (!get_bits1(&gb)) continue;
            for (int idx = 0; ; idx++) {
                unsigned op = get_ue_golomb_31(&gb);
                if (op == 3) break;
                if (idx >= si->ref_count[list] || op > 2) { dbg("ref mod overrun"); return false; }
                get_ue_golomb_long(&gb);
            }
        }
        // pred_weight_table()：码流里真正有这张表只有两种情况（H.264 7.3.3.2）：
        //   P + weighted_pred_flag、或 B + weighted_bipred_idc==1。
        // 权重的最终值驱动（radv）会自己从 slice header 里再解析一遍，这里只需
        // 把读指针跳过这张表，让它落到 dec_ref_pic_marking() 的正确位置。
        if ((pps->weighted_pred && si->slice_type_nos == AV_PICTURE_TYPE_P) ||
            (pps->weighted_bipred_idc == 1 && si->slice_type_nos == AV_PICTURE_TYPE_B)) {
            H264PredWeightTable pwt;
            if (ff_h264_pred_weight_table(&gb, sps, si->ref_count, si->slice_type_nos,
                                          &pwt, PICT_FRAME, nullptr) < 0) {
                dbg("pred weight table parse failed"); return false;
            }
        }
    }

    if (nal->ref_idc) {
        if (nal->type == H264_NAL_IDR_SLICE) {
            skip_bits1(&gb);
            if (get_bits1(&gb)) { si->mmco[0].opcode = MMCO_LONG; si->mmco[0].long_arg = 0; si->nb_mmco = 1; }
            si->explicit_ref_marking = 1;
        } else {
            si->explicit_ref_marking = get_bits1(&gb);
            if (si->explicit_ref_marking) {
                for (int i = 0; i < 32; i++) {
                    unsigned op = get_ue_golomb_31(&gb);
                    if (op == MMCO_END) break;
                    if (op == MMCO_SHORT2UNUSED) {
                        si->mmco[i].opcode = op;
                        si->mmco[i].short_pic_num =
                            (si->frame_num - get_ue_golomb_long(&gb) - 1) & ((1 << sps->log2_max_frame_num) - 1);
                    } else {
                        dbg("unsupported MMCO"); return false;   // only MMCO_SHORT2UNUSED
                    }
                    si->nb_mmco = i + 1;
                }
            }
        }
    }
    return true;
}

// --- Vulkan error names (只用于日志，避免把 VkResult 当数字猜) ---

const char *vk_result_str(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR: return "VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR: return "VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR: return "VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR: return "VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR: return "VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR";
    default: return "VK_ERROR_<other>";
    }
}

const char *vk_query_status_str(uint32_t s) {
    switch (static_cast<int32_t>(s)) {
    case VK_QUERY_RESULT_STATUS_ERROR_KHR: return "ERROR";
    case VK_QUERY_RESULT_STATUS_NOT_READY_KHR: return "NOT_READY";
    case VK_QUERY_RESULT_STATUS_COMPLETE_KHR: return "COMPLETE";
    case VK_QUERY_RESULT_STATUS_INSUFFICIENT_BITSTREAM_BUFFER_RANGE_KHR: return "INSUFFICIENT_BITSTREAM_BUFFER_RANGE";
    default: return "UNKNOWN";
    }
}

// --- memory type helpers ---

uint32_t pick_memory_type(VkPhysicalDevice pd, uint32_t type_bits,
                          VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
        if ((type_bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & wanted))
            return t;
    return 0;
}

} // namespace

// ===========================================================================
// Lifecycle
// ===========================================================================

VulkanH264Decoder::~VulkanH264Decoder() { reset(); }

void VulkanH264Decoder::reset() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (copy_fence_) vkDestroyFence(device_, copy_fence_, nullptr);
        if (decode_query_) vkDestroyQueryPool(device_, decode_query_, nullptr);
        if (decode_done_) vkDestroySemaphore(device_, decode_done_, nullptr);
        if (copy_done_) vkDestroySemaphore(device_, copy_done_, nullptr);
        if (decode_pool_) vkDestroyCommandPool(device_, decode_pool_, nullptr);
        if (transfer_pool_) vkDestroyCommandPool(device_, transfer_pool_, nullptr);
        if (bitstream_buf_) vkDestroyBuffer(device_, bitstream_buf_, nullptr);
        if (bitstream_mem_) vkFreeMemory(device_, bitstream_mem_, nullptr);
        for (auto &e : exports_) {
            if (e.fd >= 0) close(e.fd);
            if (e.buf) vkDestroyBuffer(device_, e.buf, nullptr);
            if (e.mem) vkFreeMemory(device_, e.mem, nullptr);
        }
        for (auto &o : outputs_) {
            if (o.view) vkDestroyImageView(device_, o.view, nullptr);
            if (o.image) vkDestroyImage(device_, o.image, nullptr);
            if (o.mem) vkFreeMemory(device_, o.mem, nullptr);
        }
        for (auto &d : dpb_images_) {
            if (d.view) vkDestroyImageView(device_, d.view, nullptr);
            if (d.image) vkDestroyImage(device_, d.image, nullptr);
            if (d.mem) vkFreeMemory(device_, d.mem, nullptr);
        }
        if (session_params_) pvkDestroyVideoSessionParametersKHR(device_, session_params_, nullptr);
        if (session_) pvkDestroyVideoSessionKHR(device_, session_, nullptr);
        // 只能在这里释放：radv 把 session 内存当成 VCN 自己的 context buffer，
        // destroy session 之前那块内存一直被他引用（见 header 里的说明）。
        for (auto m : session_mems_) if (m) vkFreeMemory(device_, m, nullptr);
        session_mems_.clear();
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);

    instance_ = VK_NULL_HANDLE;
    physical_device_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    decode_queue_ = VK_NULL_HANDLE;
    transfer_queue_ = VK_NULL_HANDLE;
    session_ = VK_NULL_HANDLE;
    session_params_ = VK_NULL_HANDLE;
    decode_pool_ = VK_NULL_HANDLE;
    transfer_pool_ = VK_NULL_HANDLE;
    decode_cmd_ = VK_NULL_HANDLE;
    transfer_cmd_ = VK_NULL_HANDLE;
    decode_done_ = VK_NULL_HANDLE;
    copy_done_ = VK_NULL_HANDLE;
    copy_fence_ = VK_NULL_HANDLE;
    decode_query_ = VK_NULL_HANDLE;
    query_supported_ = false;
    device_lost_ = false;
    bitstream_buf_ = VK_NULL_HANDLE;
    bitstream_mem_ = VK_NULL_HANDLE;
    bitstream_buf_size_ = 0;

    outputs_.clear();
    dpb_images_.clear();
    dpb_layout_init_.clear();
    free_dpb_slots_.clear();
    exports_.clear();
    dpb_.clear();
    coded_width_ = 0;
    coded_height_ = 0;
    decode_order_ = 0;
    poc_ctx_ = {};
    profile_info_ = {};
    h264_profile_info_ = {};
}

// ===========================================================================
// initialize / ensure_session
// ===========================================================================

CUresult VulkanH264Decoder::initialize(const CUVIDDECODECREATEINFO &info) {
    reset();
    if (info.CodecType != cudaVideoCodec_H264 ||
        info.ChromaFormat != cudaVideoChromaFormat_420 ||
        info.bitDepthMinus8 != 0 ||
        info.OutputFormat != cudaVideoSurfaceFormat_NV12 ||
        info.DeinterlaceMode != cudaVideoDeinterlaceMode_Weave ||
        info.ulWidth == 0 || info.ulHeight == 0 ||
        info.ulWidth > 4096 || info.ulHeight > 4096 ||
        info.ulNumDecodeSurfaces == 0 || info.ulNumDecodeSurfaces > 32)
        return kNotSupported;
    if ((info.ulWidth & 1) || (info.ulHeight & 1)) return CUDA_ERROR_INVALID_VALUE;

    coded_width_ = static_cast<int>(info.ulWidth);
    coded_height_ = static_cast<int>(info.ulHeight);
    poc_ctx_.prev_poc_lsb = -1;   // matches P0: sentinel before the first picture

    // Pre-allocate output-image ring (Vulkan output images are tied to a session,
    // which needs SPS/PPS; sizes are known from CUVIDDECODECREATEINFO though, so we
    // can only finalize them in ensure_session). Here we just reserve bookkeeping.
    outputs_.resize(info.ulNumDecodeSurfaces);

    // build Vulkan instance/device now (SPS/PPS only needed for the video session,
    // not for instance/device/queue discovery).
    return ensure_session();
}

CUresult VulkanH264Decoder::ensure_session() {
    auto dbg = [](const char *s) { if (std::getenv("CUVID_SHIM_DEBUG_VK")) std::fprintf(stderr, "[vk] %s\n", s); };
    // SPS/PPS required before we can build the video session + parameters.
    std::vector<uint8_t> sps_nal, pps_nal;
    if (!VkSequenceCache::fetch(&sps_nal, &pps_nal)) {
        // Deferred: the CUVID host may call cuvidDecodePicture before the parser
        // has delivered SPS/PPS. Return success; decode() will re-trigger.
        return CUDA_SUCCESS;
    }

    // --- 1. instance (only if not yet created) ---
    if (instance_ == VK_NULL_HANDLE) {
        VkApplicationInfo app_info = {};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "cuda_shim_vk";
        app_info.apiVersion = VK_API_VERSION_1_3;

        // 视频解码扩展都是 device 扩展，不能放进 instance 扩展列表，否则
        // vkCreateInstance 直接报 VK_ERROR_EXTENSION_NOT_PRESENT。instance 只需要
        // 这两个（枚举物理设备时读 external-memory 能力、查设备属性）。
        const std::vector<const char *> inst_exts = {
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        };
        VkInstanceCreateInfo vici = {};
        vici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        vici.pApplicationInfo = &app_info;
        vici.enabledExtensionCount = static_cast<uint32_t>(inst_exts.size());
        vici.ppEnabledExtensionNames = inst_exts.data();
        dbg("create instance");
        if (vkCreateInstance(&vici, nullptr, &instance_) != VK_SUCCESS) {
            reset();
            return kNotSupported;
        }

        uint32_t pd_count = 0;
        vkEnumeratePhysicalDevices(instance_, &pd_count, nullptr);
        if (pd_count == 0) { dbg("FAIL: no physical device"); reset(); return kNotSupported; }
        std::vector<VkPhysicalDevice> pds(pd_count);
        vkEnumeratePhysicalDevices(instance_, &pd_count, pds.data());
        physical_device_ = pds[0];
    }

    // --- 2. queue families ---
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfps(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count, qfps.data());
    decode_qf_ = -1;
    transfer_qf_ = -1;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (decode_qf_ < 0 && (qfps[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)) decode_qf_ = i;
        if (transfer_qf_ < 0 && (qfps[i].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT)) &&
            static_cast<int>(i) != decode_qf_) transfer_qf_ = i;
    }
    if (decode_qf_ < 0) { dbg("FAIL: no video decode queue family"); reset(); return kNotSupported; }
    if (transfer_qf_ < 0) transfer_qf_ = 0;

    // --- 2b. does the decode queue family support video decode result queries? ---
    // 与 FFmpeg ff_vk_decode_init 的 nb_q 判定一致：只有 queryResultStatusSupport
    // 为真才建 query pool，否则 BeginQuery 是非法用法。
    {
        std::vector<VkQueueFamilyQueryResultStatusPropertiesKHR> qrsp(qf_count);
        std::vector<VkQueueFamilyVideoPropertiesKHR> qfpv(qf_count);
        std::vector<VkQueueFamilyProperties2> qfp2(qf_count);
        for (uint32_t i = 0; i < qf_count; i++) {
            qrsp[i] = {};
            qrsp[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR;
            qfpv[i] = {};
            qfpv[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
            qfpv[i].pNext = &qrsp[i];
            qfp2[i] = {};
            qfp2[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
            qfp2[i].pNext = &qfpv[i];
        }
        vkGetPhysicalDeviceQueueFamilyProperties2(physical_device_, &qf_count, qfp2.data());
        query_supported_ = qrsp[decode_qf_].queryResultStatusSupport != VK_FALSE;
        if (std::getenv("CUVID_SHIM_DEBUG_VK"))
            std::fprintf(stderr, "[vk] decode qf=%d queryResultStatusSupport=%d\n",
                         decode_qf_, static_cast<int>(query_supported_));
    }

    // --- 3. device ---
    if (device_ == VK_NULL_HANDLE) {
        const std::vector<const char *> dev_exts = {
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
            VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
            VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        };
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qcis[2] = {};
        qcis[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[0].queueFamilyIndex = static_cast<uint32_t>(decode_qf_);
        qcis[0].queueCount = 1;
        qcis[0].pQueuePriorities = &prio;
        qcis[1] = qcis[0];
        qcis[1].queueFamilyIndex = static_cast<uint32_t>(transfer_qf_);

        VkDeviceCreateInfo dci = {};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 2;
        dci.pQueueCreateInfos = qcis;
        dci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
        dci.ppEnabledExtensionNames = dev_exts.data();
        dbg("create device");
        if (vkCreateDevice(physical_device_, &dci, nullptr, &device_) != VK_SUCCESS) {
            dbg("FAIL: vkCreateDevice");
            reset();
            return kNotSupported;
        }
        if (!load_video_pfns(instance_, device_)) { dbg("FAIL: load_video_pfns"); reset(); return kNotSupported; }
        vkGetDeviceQueue(device_, decode_qf_, 0, &decode_queue_);
        vkGetDeviceQueue(device_, transfer_qf_, 0, &transfer_queue_);
    }

    // --- 4. parse SPS/PPS into Std (idempotent per rebuild) ---
    H264ParamSets ps;
    std::memset(&ps, 0, sizeof(ps));
    AVCodecContext avctx;
    std::memset(&avctx, 0, sizeof(avctx));
    avctx.codec_id = AV_CODEC_ID_H264;

    {
        H2645Packet pkt;
        std::memset(&pkt, 0, sizeof(pkt));
        if (ff_h2645_packet_split(&pkt, sps_nal.data(), static_cast<int>(sps_nal.size()),
                                  nullptr, 0, AV_CODEC_ID_H264, 0) < 0) {
            reset();
            return kNotSupported;
        }
        for (int i = 0; i < pkt.nb_nals; i++)
            if (pkt.nals[i].type == H264_NAL_SPS)
                ff_h264_decode_seq_parameter_set(&pkt.nals[i].gb, &avctx, &ps, 0);
        ff_h2645_packet_uninit(&pkt);
    }
    {
        H2645Packet pkt;
        std::memset(&pkt, 0, sizeof(pkt));
        if (ff_h2645_packet_split(&pkt, pps_nal.data(), static_cast<int>(pps_nal.size()),
                                  nullptr, 0, AV_CODEC_ID_H264, 0) < 0) {
            reset();
            return kNotSupported;
        }
        for (int i = 0; i < pkt.nb_nals; i++)
            if (pkt.nals[i].type == H264_NAL_PPS)
                ff_h264_decode_picture_parameter_set(&pkt.nals[i].gb, &avctx, &ps, pkt.nals[i].size_bits);
        ff_h2645_packet_uninit(&pkt);
    }

    const SPS *sps = nullptr;
    const PPS *pps = nullptr;
    for (int i = 0; i < MAX_SPS_COUNT && !sps; i++) if (ps.sps_list[i]) sps = ps.sps_list[i];
    for (int i = 0; i < MAX_PPS_COUNT && !pps; i++) if (ps.pps_list[i]) pps = ps.pps_list[i];
    if (!sps || !pps) { dbg("FAIL: no sps/pps"); ff_h264_ps_uninit(&ps); reset(); return kNotSupported; }

    // --- 5. profile + capabilities ---
    profile_info_ = {};
    profile_info_.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    profile_info_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    profile_info_.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profile_info_.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profile_info_.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    h264_profile_info_ = {};
    h264_profile_info_.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
    h264_profile_info_.stdProfileIdc = static_cast<StdVideoH264ProfileIdc>(sps->profile_idc);
    h264_profile_info_.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
    profile_info_.pNext = &h264_profile_info_;

    VkVideoCapabilitiesKHR caps = {};
    caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    VkVideoDecodeCapabilitiesKHR dec_caps = {};
    dec_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
    VkVideoDecodeH264CapabilitiesKHR h264_caps = {};
    h264_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
    dec_caps.pNext = &h264_caps;
    caps.pNext = &dec_caps;
    dbg("query video capabilities");
    if (pvkGetPhysicalDeviceVideoCapabilitiesKHR(physical_device_, &profile_info_, &caps) != VK_SUCCESS) {
        dbg("FAIL: video capabilities");
        ff_h264_ps_uninit(&ps); reset(); return kNotSupported;
    }
    max_dpb_slots_ = caps.maxDpbSlots ? caps.maxDpbSlots : 17;
    max_active_refs_ = caps.maxActiveReferencePictures;
    min_bitstream_align_ = caps.minBitstreamBufferSizeAlignment ? caps.minBitstreamBufferSizeAlignment : 128;

    VkPhysicalDeviceProperties pdp;
    vkGetPhysicalDeviceProperties(physical_device_, &pdp);
    pitch_align_ = pdp.limits.optimalBufferCopyRowPitchAlignment;
    if (pitch_align_ == 0) pitch_align_ = 256;

    // --- 5b. decode result-status query pool (2 slots, ping-pong by frame) ---
    if (query_supported_ && decode_query_ == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo qpci = {};
        qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.pNext = &profile_info_;   // required for video result-status queries
        qpci.queryType = VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR;
        qpci.queryCount = 2;
        if (vkCreateQueryPool(device_, &qpci, nullptr, &decode_query_) != VK_SUCCESS) {
            decode_query_ = VK_NULL_HANDLE;
            query_supported_ = false;
            dbg("create decode query pool failed (continuing without per-frame status)");
        } else {
            dbg("create decode query pool");
        }
    }

    // --- 6. session ---
    VkVideoSessionCreateInfoKHR session_ci = {};
    session_ci.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    session_ci.pNext = nullptr;
    session_ci.queueFamilyIndex = static_cast<uint32_t>(decode_qf_);
    session_ci.pVideoProfile = &profile_info_;
    session_ci.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    session_ci.maxCodedExtent = { static_cast<uint32_t>(coded_width_), static_cast<uint32_t>(coded_height_) };
    session_ci.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    session_ci.maxDpbSlots = max_dpb_slots_;
    session_ci.maxActiveReferencePictures = max_active_refs_;
    session_ci.pStdHeaderVersion = &caps.stdHeaderVersion;

    dbg("create video session");
    if (pvkCreateVideoSessionKHR(device_, &session_ci, nullptr, &session_) != VK_SUCCESS) {
        ff_h264_ps_uninit(&ps); reset(); return kNotSupported;
    }

    // --- 7. bind session memory ---
    uint32_t mem_req_count = 0;
    pvkGetVideoSessionMemoryRequirementsKHR(device_, session_, &mem_req_count, nullptr);
    std::vector<VkVideoSessionMemoryRequirementsKHR> mem_reqs(mem_req_count);
    for (auto &m : mem_reqs) m.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
    pvkGetVideoSessionMemoryRequirementsKHR(device_, session_, &mem_req_count, mem_reqs.data());

    std::vector<VkDeviceMemory> session_mems(mem_req_count, VK_NULL_HANDLE);
    std::vector<VkBindVideoSessionMemoryInfoKHR> binds(mem_req_count);
    for (uint32_t i = 0; i < mem_req_count; i++) {
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mem_reqs[i].memoryRequirements.size;
        mai.memoryTypeIndex = pick_memory_type(physical_device_, mem_reqs[i].memoryRequirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &mai, nullptr, &session_mems[i]) != VK_SUCCESS) {
            for (auto m : session_mems) if (m) vkFreeMemory(device_, m, nullptr);
            ff_h264_ps_uninit(&ps); reset(); return kNotSupported;
        }
        binds[i].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
        binds[i].memoryBindIndex = i;
        binds[i].memory = session_mems[i];
        binds[i].memoryOffset = 0;
        binds[i].memorySize = mem_reqs[i].memoryRequirements.size;
    }
    dbg("bind session memory");
    if (mem_req_count && pvkBindVideoSessionMemoryKHR(device_, session_, mem_req_count, binds.data()) != VK_SUCCESS) {
        for (auto m : session_mems) if (m) vkFreeMemory(device_, m, nullptr);
        ff_h264_ps_uninit(&ps); reset(); return kNotSupported;
    }
    // Ownership moves to the session: freed only in reset(), after the session is
    // destroyed (same lifetime rule as FFmpeg's ff_vk_video_session_mem).
    session_mems_ = std::move(session_mems);

    // --- 8. session parameters ---
    StdVideoH264ScalingLists sps_sl, pps_sl;
    StdVideoH264SequenceParameterSetVui sps_vui;
    StdVideoH264SequenceParameterSet std_sps;
    StdVideoH264PictureParameterSet std_pps;
    fill_sps_std(sps, &sps_sl, &sps_vui, &std_sps);
    fill_pps_std(pps, &pps_sl, &std_pps);
    sps_id_ = std_sps.seq_parameter_set_id;
    pps_id_ = std_pps.pic_parameter_set_id;
    if (std::getenv("CUVID_SHIM_DEBUG_VK")) {
        std::fprintf(stderr,
                     "[vk] std SPS id=%u profile=%d level=%d log2fn=%u poctype=%u log2poc=%u "
                     "maxrefs=%u frame_mbs_only=%u direct8x8=%u gaps=%u chroma=%u bitdepth=%u/%u\n",
                     std_sps.seq_parameter_set_id, (int)std_sps.profile_idc, (int)std_sps.level_idc,
                     std_sps.log2_max_frame_num_minus4, std_sps.pic_order_cnt_type,
                     std_sps.log2_max_pic_order_cnt_lsb_minus4, std_sps.max_num_ref_frames,
                     std_sps.flags.frame_mbs_only_flag, std_sps.flags.direct_8x8_inference_flag,
                     std_sps.flags.gaps_in_frame_num_value_allowed_flag, std_sps.chroma_format_idc,
                     std_sps.bit_depth_luma_minus8, std_sps.bit_depth_chroma_minus8);
        std::fprintf(stderr,
                     "[vk] std PPS id=%u sps=%u entropy=%u bfpo=%u l0def=%u l1def=%u wpred=%u wbipred=%u "
                     "initqp=%d chromaqp=%d second=%d deblock=%u cip=%u redun=%u t8x8=%u\n",
                     std_pps.pic_parameter_set_id, std_pps.seq_parameter_set_id,
                     std_pps.flags.entropy_coding_mode_flag,
                     std_pps.flags.bottom_field_pic_order_in_frame_present_flag,
                     std_pps.num_ref_idx_l0_default_active_minus1,
                     std_pps.num_ref_idx_l1_default_active_minus1,
                     std_pps.flags.weighted_pred_flag, std_pps.weighted_bipred_idc,
                     std_pps.pic_init_qp_minus26, std_pps.chroma_qp_index_offset,
                     std_pps.second_chroma_qp_index_offset,
                     std_pps.flags.deblocking_filter_control_present_flag,
                     std_pps.flags.constrained_intra_pred_flag, std_pps.flags.redundant_pic_cnt_present_flag,
                     std_pps.flags.transform_8x8_mode_flag);
    }

    VkVideoDecodeH264SessionParametersAddInfoKHR h264_add = {};
    h264_add.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
    h264_add.stdSPSCount = 1; h264_add.pStdSPSs = &std_sps;
    h264_add.stdPPSCount = 1; h264_add.pStdPPSs = &std_pps;

    VkVideoDecodeH264SessionParametersCreateInfoKHR h264_create = {};
    h264_create.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
    h264_create.maxStdSPSCount = 1;
    h264_create.maxStdPPSCount = 1;
    h264_create.pParametersAddInfo = &h264_add;

    VkVideoSessionParametersCreateInfoKHR sp_ci = {};
    sp_ci.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
    sp_ci.pNext = &h264_create;
    sp_ci.videoSession = session_;
    dbg("create session parameters");
    if (pvkCreateVideoSessionParametersKHR(device_, &sp_ci, nullptr, &session_params_) != VK_SUCCESS) {
        ff_h264_ps_uninit(&ps); reset(); return kNotSupported;
    }

    ff_h264_ps_uninit(&ps);

    // --- 9. images (output + DPB) ---
    const VkFormat NV12 = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    const uint32_t qf_indices[2] = { static_cast<uint32_t>(decode_qf_), static_cast<uint32_t>(transfer_qf_) };

    VkVideoProfileListInfoKHR prof_list = {};
    prof_list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    prof_list.profileCount = 1;
    prof_list.pProfiles = &profile_info_;

    auto create_image = [&](VkImageUsageFlags usage, ImageRes *r) -> bool {
        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.pNext = &prof_list;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = NV12;
        ici.extent = { static_cast<uint32_t>(coded_width_), static_cast<uint32_t>(coded_height_), 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ici.queueFamilyIndexCount = 2;
        ici.pQueueFamilyIndices = qf_indices;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ici, nullptr, &r->image) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(device_, r->image, &mr);
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = pick_memory_type(physical_device_, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &mai, nullptr, &r->mem) != VK_SUCCESS) return false;
        vkBindImageMemory(device_, r->image, r->mem, 0);
        VkImageViewCreateInfo vci = {};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = r->image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = NV12;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(device_, &vci, nullptr, &r->view) != VK_SUCCESS) return false;
        r->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    };

    for (auto &o : outputs_) {
        if (!create_image(VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &o)) {
            reset(); return kNotSupported;
        }
    }
    dpb_images_.assign(max_dpb_slots_, ImageRes{});
    for (uint32_t i = 0; i < max_dpb_slots_; i++) {
        // RADV VCN<5: DPB images must be DPB-only, not DST (radv_video.c:1184).
        // TRANSFER_SRC 只是可选的读写用途（不是 DST），用于把 DPB 图内容导出。
        if (!create_image(VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          &dpb_images_[i])) {
            reset(); return kNotSupported;
        }
    }
    dpb_layout_init_.assign(max_dpb_slots_, 0);
    free_dpb_slots_.clear();
    for (uint32_t i = 0; i < max_dpb_slots_; i++) free_dpb_slots_.push_back(static_cast<int>(i));

    // --- 10. export buffers (one per output surface) ---
    const uint32_t pitch = (coded_width_ + pitch_align_ - 1) / pitch_align_ * pitch_align_;
    const VkDeviceSize export_size = static_cast<VkDeviceSize>(pitch) * coded_height_ * 3 / 2;
    exports_.assign(outputs_.size(), ExportRes{});
    for (auto &e : exports_) {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = export_size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bci, nullptr, &e.buf) != VK_SUCCESS) { reset(); return kNotSupported; }
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(device_, e.buf, &mr);
        VkExportMemoryAllocateInfo export_info = {};
        export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext = &export_info;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = pick_memory_type(physical_device_, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &mai, nullptr, &e.mem) != VK_SUCCESS) { reset(); return kNotSupported; }
        vkBindBufferMemory(device_, e.buf, e.mem, 0);
        e.pitch = pitch;
        e.size = export_size;
    }

    // --- 11. bitstream buffer (single, reused) ---
    bitstream_buf_size_ = (coded_width_ * coded_height_ * 3 / 2) + 4 + min_bitstream_align_;  // generous upper bound
    {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bitstream_buf_size_;
        bci.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bci, nullptr, &bitstream_buf_) != VK_SUCCESS) { reset(); return kNotSupported; }
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(device_, bitstream_buf_, &mr);
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = pick_memory_type(physical_device_, mr.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device_, &mai, nullptr, &bitstream_mem_) != VK_SUCCESS) { reset(); return kNotSupported; }
        vkBindBufferMemory(device_, bitstream_buf_, bitstream_mem_, 0);
    }

    // --- 12. sync + command pools ---
    VkSemaphoreCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(device_, &sci, nullptr, &decode_done_);
    vkCreateSemaphore(device_, &sci, nullptr, &copy_done_);
    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(device_, &fci, nullptr, &copy_fence_);

    VkCommandPoolCreateInfo cpci = {};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = static_cast<uint32_t>(decode_qf_);
    vkCreateCommandPool(device_, &cpci, nullptr, &decode_pool_);
    cpci.queueFamilyIndex = static_cast<uint32_t>(transfer_qf_);
    vkCreateCommandPool(device_, &cpci, nullptr, &transfer_pool_);

    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = decode_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cbai, &decode_cmd_);
    cbai.commandPool = transfer_pool_;
    vkAllocateCommandBuffers(device_, &cbai, &transfer_cmd_);

    // --- 13. one-time VCN session init ---
    // radv 在 vkCreateVideoSession 时只建了 session 对象，真正给 VCN 固件发
    // SESSION_CREATE / 上下文初始化的动作发生在第一次
    // vkCmdControlVideoCodingKHR(flags=RESET)。FFmpeg 在解码器初始化末尾
    // ff_vk_decode_flush() 里必做这一步；缺了它 IDR 帧碰巧能出，P/B 帧一
    // 用 DPB 固件就挂（context lost）。这里在 session 建好后提交一次 RESET。
    if (!session_reset_done_) {
        dbg("session reset (control coding)");
        VkVideoBeginCodingInfoKHR r_begin = {};
        r_begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
        r_begin.videoSession = session_;
        r_begin.videoSessionParameters = session_params_;
        VkVideoCodingControlInfoKHR r_ctrl = {};
        r_ctrl.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
        r_ctrl.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
        VkVideoEndCodingInfoKHR r_end = {};
        r_end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;

        VkCommandBufferBeginInfo cbbi = {};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(decode_cmd_, &cbbi);
        pvkCmdBeginVideoCodingKHR(decode_cmd_, &r_begin);
        pvkCmdControlVideoCodingKHR(decode_cmd_, &r_ctrl);
        pvkCmdEndVideoCodingKHR(decode_cmd_, &r_end);
        vkEndCommandBuffer(decode_cmd_);

        VkCommandBufferSubmitInfo cb = {};
        cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cb.commandBuffer = decode_cmd_;
        VkSubmitInfo2 si2 = {};
        si2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si2.commandBufferInfoCount = 1;
        si2.pCommandBufferInfos = &cb;
        if (vkQueueSubmit2(decode_queue_, 1, &si2, VK_NULL_HANDLE) != VK_SUCCESS) {
            reset(); return CUDA_ERROR_UNKNOWN;
        }
        vkQueueWaitIdle(decode_queue_);
        session_reset_done_ = true;
    }

    return CUDA_SUCCESS;
}

// ===========================================================================
// decode
// ===========================================================================

CUresult VulkanH264Decoder::decode(const CUVIDPICPARAMS &picture) {
    auto dbg = [](const char *s) { if (std::getenv("CUVID_SHIM_DEBUG_VK")) std::fprintf(stderr, "[vk-decode] %s\n", s); };
    if (device_lost_) return CUDA_ERROR_UNKNOWN;   // 已经挂过：不要再往死掉的 device 提交
    // 诊断用：CUVID_SHIM_NOCOPY=1 只做解码（decode 队列 + fence），不做
    // transfer 队列的导出拷贝、不用跨队列 semaphore。用来区分"解码参数问题"
    // 和"多队列同步问题"。
    const bool nocopy = std::getenv("CUVID_SHIM_NOCOPY") != nullptr;
    if (device_ == VK_NULL_HANDLE || session_ == VK_NULL_HANDLE) {
        CUresult r = ensure_session();
        if (r != CUDA_SUCCESS) return r;
        if (session_ == VK_NULL_HANDLE) return CUDA_SUCCESS;   // still waiting on SPS/PPS
    }
    if (picture.CurrPicIdx < 0 || static_cast<std::size_t>(picture.CurrPicIdx) >= outputs_.size())
        return CUDA_ERROR_INVALID_VALUE;

    const uint32_t W = static_cast<uint32_t>(coded_width_);
    const uint32_t H = static_cast<uint32_t>(coded_height_);

    // --- 0. locate SPS/PPS (already cached/parsed once; re-parse cheaply here) ---
    // We need the parsed SPS/PPS to fill StdVideoDecodeH264PictureInfo + parse the
    // slice header. Re-fetch + parse from cache (cheap for single-stream).
    std::vector<uint8_t> sps_nal, pps_nal;
    if (!VkSequenceCache::fetch(&sps_nal, &pps_nal)) return kNotSupported;

    H264ParamSets ps;
    std::memset(&ps, 0, sizeof(ps));
    AVCodecContext avctx;
    std::memset(&avctx, 0, sizeof(avctx));
    avctx.codec_id = AV_CODEC_ID_H264;
    {
        H2645Packet pkt;
        std::memset(&pkt, 0, sizeof(pkt));
        if (ff_h2645_packet_split(&pkt, sps_nal.data(), static_cast<int>(sps_nal.size()),
                                  nullptr, 0, AV_CODEC_ID_H264, 0) < 0) return kNotSupported;
        for (int i = 0; i < pkt.nb_nals; i++)
            if (pkt.nals[i].type == H264_NAL_SPS)
                ff_h264_decode_seq_parameter_set(&pkt.nals[i].gb, &avctx, &ps, 0);
        ff_h2645_packet_uninit(&pkt);
    }
    {
        H2645Packet pkt;
        std::memset(&pkt, 0, sizeof(pkt));
        if (ff_h2645_packet_split(&pkt, pps_nal.data(), static_cast<int>(pps_nal.size()),
                                  nullptr, 0, AV_CODEC_ID_H264, 0) < 0) { ff_h264_ps_uninit(&ps); return kNotSupported; }
        for (int i = 0; i < pkt.nb_nals; i++)
            if (pkt.nals[i].type == H264_NAL_PPS)
                ff_h264_decode_picture_parameter_set(&pkt.nals[i].gb, &avctx, &ps, pkt.nals[i].size_bits);
        ff_h2645_packet_uninit(&pkt);
    }
    const SPS *sps = nullptr;
    const PPS *pps = nullptr;
    for (int i = 0; i < MAX_SPS_COUNT && !sps; i++) if (ps.sps_list[i]) sps = ps.sps_list[i];
    for (int i = 0; i < MAX_PPS_COUNT && !pps; i++) if (ps.pps_list[i]) pps = ps.pps_list[i];
    if (!sps || !pps) { ff_h264_ps_uninit(&ps); return kNotSupported; }

    // --- 1. parse the slice header (the CUVID host passes one slice NAL at a time) ---
    // picture.pBitstreamData 是 parser 传过来的**裸 NAL**（nal.raw_data，无 start
    // code），而 ff_h2645_packet_split 要求 Annex-B 起始码，否则报
    // "No start code is found."。这里临时补一个 00 00 00 01 再解析。
    std::vector<uint8_t> slice_buf;
    slice_buf.reserve(4 + picture.nBitstreamDataLen);
    const uint8_t sc[4] = { 0x00, 0x00, 0x00, 0x01 };
    slice_buf.insert(slice_buf.end(), sc, sc + 4);
    slice_buf.insert(slice_buf.end(), picture.pBitstreamData,
                     picture.pBitstreamData + picture.nBitstreamDataLen);
    H2645Packet slice_pkt;
    std::memset(&slice_pkt, 0, sizeof(slice_pkt));
    if (ff_h2645_packet_split(&slice_pkt, slice_buf.data(), static_cast<int>(slice_buf.size()),
                              nullptr, 0, AV_CODEC_ID_H264, 0) < 0 || slice_pkt.nb_nals == 0) {
        ff_h264_ps_uninit(&ps); return kNotSupported;
    }
    H2645NAL *slice_nal = nullptr;
    for (int i = 0; i < slice_pkt.nb_nals; i++) {
        if (slice_pkt.nals[i].type == H264_NAL_SLICE || slice_pkt.nals[i].type == H264_NAL_IDR_SLICE) {
            slice_nal = &slice_pkt.nals[i];
            break;
        }
    }
    if (!slice_nal) { dbg("no slice NAL found"); ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return kNotSupported; }

    SliceInfo si;
    if (!parse_slice_header(slice_nal, sps, pps, &si)) {
        dbg("slice header parse failed"); ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return kNotSupported;
    }

    // --- 2. POC computation (state persists across decode() calls) ---
    poc_ctx_.frame_num = si.frame_num;
    poc_ctx_.poc_lsb = si.poc_lsb;
    poc_ctx_.delta_poc_bottom = si.delta_poc_bottom;
    poc_ctx_.delta_poc[0] = si.delta_poc[0];
    poc_ctx_.delta_poc[1] = si.delta_poc[1];
    int field_poc[2] = {0, 0};
    int pic_poc = 0;
    if (ff_h264_init_poc(field_poc, &pic_poc, sps, &poc_ctx_, PICT_FRAME, si.nal_ref_idc) < 0) {
        dbg("ff_h264_init_poc failed"); ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return kNotSupported;
    }
    if (si.nal_ref_idc != 0) {
        poc_ctx_.prev_poc_msb = poc_ctx_.poc_msb;
        poc_ctx_.prev_poc_lsb = poc_ctx_.poc_lsb;
    }
    poc_ctx_.prev_frame_num = poc_ctx_.frame_num;
    poc_ctx_.prev_frame_num_offset = poc_ctx_.frame_num_offset;
    (void)pic_poc;

    // --- 3. allocate DPB slot for the current (setup) picture ---
    if (free_dpb_slots_.empty()) { dbg("no free DPB slots"); ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return CUDA_ERROR_OUT_OF_MEMORY; }
    // E2: 升序分配（0,1,2,...）。FFmpeg h264_vulkan 的 slotIndex 就是 h->DPB 下标
    // 0..15，而原来用 back() 得到 16,15,14,... 的 LIFO 顺序，slotIndex 挤在
    // [maxDpbSlots-1 ..] 一侧（radv 的 ref_frame_list[]/16 位掩码边界就在 16）。
    const auto it_min = std::min_element(free_dpb_slots_.begin(), free_dpb_slots_.end());
    const int setup_slot = *it_min;
    free_dpb_slots_.erase(it_min);

    // --- 4. build reference slots from current DPB ---
    std::vector<VkVideoReferenceSlotInfoKHR> ref_slots;
    std::vector<VkVideoPictureResourceInfoKHR> ref_res;
    std::vector<VkVideoDecodeH264DpbSlotInfoKHR> ref_dpb;
    std::vector<StdVideoDecodeH264ReferenceInfo> ref_std;
    ref_slots.reserve(dpb_images_.size());
    ref_res.reserve(dpb_images_.size());
    ref_dpb.reserve(dpb_images_.size());
    ref_std.reserve(dpb_images_.size());
    for (const auto &e : dpb_) {
        StdVideoDecodeH264ReferenceInfo rs = {};
        rs.FrameNum = static_cast<uint16_t>(e.frame_num);
        rs.PicOrderCnt[0] = e.field_poc[0];
        rs.PicOrderCnt[1] = e.field_poc[1];
        rs.flags.top_field_flag = 1;
        rs.flags.bottom_field_flag = 1;
        ref_std.push_back(rs);

        VkVideoDecodeH264DpbSlotInfoKHR rd = {};
        rd.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
        rd.pStdReferenceInfo = &ref_std.back();
        ref_dpb.push_back(rd);

        VkVideoPictureResourceInfoKHR rr = {};
        rr.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        rr.codedExtent = { W, H };
        rr.baseArrayLayer = 0;
        rr.imageViewBinding = dpb_images_[e.slot].view;
        ref_res.push_back(rr);

        VkVideoReferenceSlotInfoKHR sl = {};
        sl.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        sl.pNext = &ref_dpb.back();
        sl.slotIndex = e.slot;
        sl.pPictureResource = &ref_res.back();
        ref_slots.push_back(sl);
    }
    // --- 5. setup reference slot (current picture) ---
    StdVideoDecodeH264ReferenceInfo setup_std = {};
    setup_std.FrameNum = static_cast<uint16_t>(si.frame_num);
    setup_std.PicOrderCnt[0] = field_poc[0];
    setup_std.PicOrderCnt[1] = field_poc[1];
    setup_std.flags.top_field_flag = 1;
    setup_std.flags.bottom_field_flag = 1;
    if (std::getenv("CUVID_SHIM_DEBUG_VK")) {
        std::fprintf(stderr,
                     "[vk-decode] #%u type=%d poc=%d fn=%d ref=%d rc0=%d rc1=%d setup_slot=%d "
                     "refs=%zu dst_idx=%d dst_img=%llx setup_img=%llx free={",
                     decode_order_, si.slice_type_nos, field_poc[0], si.frame_num, si.nal_ref_idc,
                     si.ref_count[0], si.ref_count[1], setup_slot, ref_slots.size(),
                     picture.CurrPicIdx,
                     static_cast<unsigned long long>(
                         reinterpret_cast<uintptr_t>(outputs_[picture.CurrPicIdx].image)),
                     static_cast<unsigned long long>(
                         reinterpret_cast<uintptr_t>(dpb_images_[setup_slot].image)));
        for (std::size_t i = 0; i < free_dpb_slots_.size(); i++)
            std::fprintf(stderr, "%s%d", i ? "," : "", free_dpb_slots_[i]);
        std::fprintf(stderr, "} dpb:");
        for (const auto &e : dpb_)
            std::fprintf(stderr, " [slot%d:fn%d:poc%d:img%llx]", e.slot, e.frame_num, e.field_poc[0],
                         static_cast<unsigned long long>(
                             reinterpret_cast<uintptr_t>(dpb_images_[e.slot].image)));
        std::fprintf(stderr, "\n");
    }
    VkVideoDecodeH264DpbSlotInfoKHR setup_dpb = {};
    setup_dpb.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
    setup_dpb.pStdReferenceInfo = &setup_std;
    VkVideoPictureResourceInfoKHR setup_res = {};
    setup_res.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    setup_res.codedExtent = { W, H };
    setup_res.baseArrayLayer = 0;
    setup_res.imageViewBinding = dpb_images_[setup_slot].view;
    VkVideoReferenceSlotInfoKHR setup_slot_info = {};
    setup_slot_info.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    setup_slot_info.pNext = &setup_dpb;
    setup_slot_info.slotIndex = setup_slot;
    setup_slot_info.pPictureResource = &setup_res;

    // --- 6. write the Annex-B slice into the bitstream buffer ---
    {
        void *p = nullptr;
        vkMapMemory(device_, bitstream_mem_, 0, bitstream_buf_size_, 0, &p);
        std::memset(p, 0, bitstream_buf_size_);
        uint8_t *dst = static_cast<uint8_t *>(p);
        const uint8_t startcode[4] = { 0x00, 0x00, 0x00, 0x01 };
        std::memcpy(dst, startcode, 4);
        std::memcpy(dst + 4, slice_nal->raw_data, slice_nal->raw_size);
        vkUnmapMemory(device_, bitstream_mem_);
    }

    // --- 7. StdVideoDecodeH264PictureInfo ---
    StdVideoDecodeH264PictureInfo std_pic = {};
    std_pic.flags.is_intra = (si.slice_type_nos == AV_PICTURE_TYPE_I);
    std_pic.flags.IdrPicFlag = (si.nal_type == H264_NAL_IDR_SLICE);
    std_pic.flags.is_reference = (si.nal_ref_idc != 0);
    std_pic.seq_parameter_set_id = sps_id_;
    std_pic.pic_parameter_set_id = pps_id_;
    std_pic.frame_num = static_cast<uint16_t>(si.frame_num);
    std_pic.idr_pic_id = static_cast<uint16_t>(si.idr_pic_id);
    std_pic.PicOrderCnt[0] = field_poc[0];
    std_pic.PicOrderCnt[1] = field_poc[1];

    uint32_t slice_offsets[1] = { 0 };

    // --- 8. record decode command buffer ---
    ImageRes &out = outputs_[picture.CurrPicIdx];
    const uint32_t qidx = decode_order_ % 2;
    vkResetCommandPool(device_, decode_pool_, 0);
    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(decode_cmd_, &cbbi);
    // Query reset 必须在 video coding scope 之外（FFmpeg 在 ff_vk_exec_start 里做同一件事）。
    if (decode_query_ != VK_NULL_HANDLE) vkCmdResetQueryPool(decode_cmd_, decode_query_, qidx, 1);

    {   // output -> VIDEO_DECODE_DST
        VkImageMemoryBarrier2 b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = (out.layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        b.dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        b.oldLayout = out.layout;
        b.newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = out.image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo di = {};
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(decode_cmd_, &di);
        out.layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
    }
    {   // setup slot (+ all reference slots) -> VIDEO_DECODE_DPB
        // 以前只给 setup 槽做屏障，参考槽假设一直是 DPB 布局；dpbsrc 实验会把
        // DPB 图切到 TRANSFER_SRC，因此这里统一按记录的布局回到 DPB。
        auto to_dpb = [&](int slot) {
            ImageRes &im = dpb_images_[slot];
            const bool inited = im.layout != VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageMemoryBarrier2 b = {};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.srcAccessMask = inited ? (VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_TRANSFER_READ_BIT)
                                     : VK_ACCESS_2_NONE;
            b.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
            b.dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
            b.oldLayout = im.layout;
            b.newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = im.image;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkDependencyInfo di = {};
            di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(decode_cmd_, &di);
            im.layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
            dpb_layout_init_[slot] = 1;
        };
        for (const auto &e : dpb_) if (e.slot != setup_slot) to_dpb(e.slot);
        to_dpb(setup_slot);
    }

    VkVideoBeginCodingInfoKHR begin = {};
    begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    begin.videoSession = session_;
    begin.videoSessionParameters = session_params_;
    begin.referenceSlotCount = static_cast<uint32_t>(ref_slots.size());
    begin.pReferenceSlots = ref_slots.data();

    // 当前正在解码的画面必须作为"非激活参考"绑定进 BeginCoding 的参考槽列表，
    // slotIndex = -1 表示它只是被激活槽位，尚未产生可参考的数据。缺这一步
    // radv 在 P/B 帧（setup 槽位与参考槽位重叠）上会直接让 VCN 挂起 → context lost。
    // 参考 FFmpeg vulkan_decode.c:ff_vk_decode_frame 的同样处理。
    VkVideoReferenceSlotInfoKHR inactive_setup = setup_slot_info;
    inactive_setup.slotIndex = -1;
    std::vector<VkVideoReferenceSlotInfoKHR> begin_refs;
    begin_refs.reserve(ref_slots.size() + 1);
    begin_refs.insert(begin_refs.end(), ref_slots.begin(), ref_slots.end());
    begin_refs.push_back(inactive_setup);
    begin.referenceSlotCount = static_cast<uint32_t>(begin_refs.size());
    begin.pReferenceSlots = begin_refs.data();
    pvkCmdBeginVideoCodingKHR(decode_cmd_, &begin);
    // 解码结果状态 query 必须在 video coding scope 内 begin/end
    // （FFmpeg vulkan_decode.c:528-535 同样的位置）。
    if (decode_query_ != VK_NULL_HANDLE) vkCmdBeginQuery(decode_cmd_, decode_query_, qidx, 0);

    VkVideoDecodeH264PictureInfoKHR h264_pic_info = {};
    h264_pic_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR;
    h264_pic_info.pStdPictureInfo = &std_pic;
    h264_pic_info.sliceCount = 1;
    h264_pic_info.pSliceOffsets = slice_offsets;

    VkVideoDecodeInfoKHR decode_info = {};
    decode_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
    decode_info.pNext = &h264_pic_info;
    decode_info.srcBuffer = bitstream_buf_;
    decode_info.srcBufferOffset = 0;
    VkDeviceSize src_range = 4 + slice_nal->raw_size;
    src_range = (src_range + min_bitstream_align_ - 1) / min_bitstream_align_ * min_bitstream_align_;
    decode_info.srcBufferRange = src_range;
    decode_info.dstPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    decode_info.dstPictureResource.codedExtent = { W, H };
    decode_info.dstPictureResource.baseArrayLayer = 0;
    decode_info.dstPictureResource.imageViewBinding = out.view;
    decode_info.pSetupReferenceSlot = &setup_slot_info;
    decode_info.referenceSlotCount = static_cast<uint32_t>(ref_slots.size());
    decode_info.pReferenceSlots = ref_slots.data();
    pvkCmdDecodeVideoKHR(decode_cmd_, &decode_info);
    if (decode_query_ != VK_NULL_HANDLE) vkCmdEndQuery(decode_cmd_, decode_query_, qidx);

    VkVideoEndCodingInfoKHR end = {};
    end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
    pvkCmdEndVideoCodingKHR(decode_cmd_, &end);
    vkEndCommandBuffer(decode_cmd_);

    // --- 9. submit decode (wait copyDoneSem for output WAR, signal decodeDoneSem) ---
    {
        VkSemaphoreSubmitInfo wait_sem = {};
        wait_sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_sem.semaphore = copy_done_;
        wait_sem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSemaphoreSubmitInfo sig_sem = {};
        sig_sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        sig_sem.semaphore = decode_done_;
        sig_sem.stageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        VkCommandBufferSubmitInfo cb = {};
        cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cb.commandBuffer = decode_cmd_;
        VkSubmitInfo2 si2 = {};
        si2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si2.waitSemaphoreInfoCount = (decode_order_ == 0 || nocopy) ? 0 : 1;
        si2.pWaitSemaphoreInfos = &wait_sem;
        si2.commandBufferInfoCount = 1;
        si2.pCommandBufferInfos = &cb;
        si2.signalSemaphoreInfoCount = nocopy ? 0 : 1;
        si2.pSignalSemaphoreInfos = &sig_sem;
        const VkResult sr = vkQueueSubmit2(decode_queue_, 1, &si2, nocopy ? copy_fence_ : VK_NULL_HANDLE);
        if (sr != VK_SUCCESS) {
            std::fprintf(stderr, "[vk-decode] FATAL frame=%u decode queue submit=%s (idx=%d fn=%d poc=%d type=%d)\n",
                         decode_order_, vk_result_str(sr), picture.CurrPicIdx, si.frame_num,
                         field_poc[0], si.slice_type_nos);
            if (sr == VK_ERROR_DEVICE_LOST) device_lost_ = true;
            ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return CUDA_ERROR_UNKNOWN;
        }
    }

    // --- 10. record transfer command buffer (barrier + copy Y/UV) ---
    ExportRes &exp = exports_[picture.CurrPicIdx];
    ImageRes &copy_src = out;
    if (!nocopy) {
    vkResetCommandPool(device_, transfer_pool_, 0);
    vkBeginCommandBuffer(transfer_cmd_, &cbbi);
    {
        VkImageMemoryBarrier2 b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        b.srcAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = copy_src.layout;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = copy_src.image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo di = {};
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(transfer_cmd_, &di);
    }
    VkBufferImageCopy regions[2] = {};
    regions[0].bufferOffset = 0;
    regions[0].bufferRowLength = exp.pitch;
    regions[0].bufferImageHeight = H;
    regions[0].imageSubresource = { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 };
    regions[0].imageExtent = { W, H, 1 };
    // 多平面格式里 extent / bufferRowLength / bufferImageHeight 一律以**该平面自己
    // 的 texel 为单位**：NV12 的 plane 1 texel = 2 字节 (CbCr 对)，所以宽 / 行距都
    // 要除以 2。以前这里写的是 {W, H/2} 和整行 pitch（字节当 texel 用），radv 会按
    // 超出平面宽度的范围去搬 → 导出的色度是垃圾（表现为"UV 一片平"）。
    // 参考 FFmpeg hwcontext_vulkan.c:get_plane_buf / map_from（
    // region[i].bufferRowLength /= desc->comp[i].step）。
    regions[1].bufferOffset = static_cast<VkDeviceSize>(exp.pitch) * H;
    regions[1].bufferRowLength = exp.pitch / 2;
    regions[1].bufferImageHeight = H / 2;
    regions[1].imageSubresource = { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 };
    regions[1].imageExtent = { W / 2, H / 2, 1 };
    vkCmdCopyImageToBuffer(transfer_cmd_, copy_src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, exp.buf, 2, regions);
    copy_src.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkEndCommandBuffer(transfer_cmd_);

    {
        VkSemaphoreSubmitInfo wait_sem = {};
        wait_sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_sem.semaphore = decode_done_;
        wait_sem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSemaphoreSubmitInfo sig_sem = {};
        sig_sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        sig_sem.semaphore = copy_done_;
        sig_sem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cb = {};
        cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cb.commandBuffer = transfer_cmd_;
        VkSubmitInfo2 si2 = {};
        si2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si2.waitSemaphoreInfoCount = 1;
        si2.pWaitSemaphoreInfos = &wait_sem;
        si2.commandBufferInfoCount = 1;
        si2.pCommandBufferInfos = &cb;
        si2.signalSemaphoreInfoCount = 1;
        si2.pSignalSemaphoreInfos = &sig_sem;
        if (vkQueueSubmit2(transfer_queue_, 1, &si2, copy_fence_) != VK_SUCCESS) {
            dbg("transfer queue submit failed"); ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return CUDA_ERROR_UNKNOWN;
        }
    }
    }   // !nocopy
    // --- 10b. wait for the transfer queue (which waits on decode) + per-frame status ---
    // 以前这里用 UINT64_MAX 且不检查返回值：GPU 挂死时只会看到下一帧 submit 失败，
    // 无法判断到底是哪一帧出错。改成带超时 + 检查返回值 + 读解码 query 状态。
    {
        const uint64_t timeout_ns = 30ull * 1000 * 1000 * 1000;
        const VkResult wr = vkWaitForFences(device_, 1, &copy_fence_, VK_TRUE, timeout_ns);
        if (wr != VK_SUCCESS) {
            std::fprintf(stderr,
                         "[vk-decode] FATAL frame=%u vkWaitForFences=%s "
                         "(idx=%d fn=%d poc=%d type=%d setup=%d refs=%zu)\n",
                         decode_order_, vk_result_str(wr), picture.CurrPicIdx, si.frame_num,
                         field_poc[0], si.slice_type_nos, setup_slot, ref_slots.size());
            device_lost_ = true;
            ff_h2645_packet_uninit(&slice_pkt);
            ff_h264_ps_uninit(&ps);
            return CUDA_ERROR_UNKNOWN;
        }
        vkResetFences(device_, 1, &copy_fence_);
    }

    if (decode_query_ != VK_NULL_HANDLE) {
        uint32_t status = 0;
        const VkResult qr = vkGetQueryPoolResults(device_, decode_query_, qidx, 1,
                                                  sizeof(status), &status, sizeof(status),
                                                  VK_QUERY_RESULT_WAIT_BIT |
                                                      VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
        const bool complete = (qr == VK_SUCCESS) &&
                              (static_cast<int32_t>(status) == VK_QUERY_RESULT_STATUS_COMPLETE_KHR);
        if (!complete || std::getenv("CUVID_SHIM_DEBUG_VK")) {
            std::fprintf(stderr, "[vk-decode] frame=%u status=%s (%d) vkGetQueryPoolResults=%s\n",
                         decode_order_, (qr == VK_SUCCESS) ? vk_query_status_str(status) : "n/a",
                         static_cast<int>(static_cast<int32_t>(status)), vk_result_str(qr));
        }
        if (!complete) {
            ff_h2645_packet_uninit(&slice_pkt);
            ff_h264_ps_uninit(&ps);
            return CUDA_ERROR_UNKNOWN;
        }
    }

    // --- 11. export the fd for map_frame() to import ---
    if (exp.fd >= 0) close(exp.fd);
    exp.fd = -1;
    {
        VkMemoryGetFdInfoKHR fd_info = {};
        fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fd_info.memory = exp.mem;
        fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (pvkGetMemoryFdKHR(device_, &fd_info, &exp.fd) != VK_SUCCESS || exp.fd < 0) {
            dbg("get memory fd failed"); ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return CUDA_ERROR_UNKNOWN;
        }
    }

    // --- 12. DPB update (IDR clear / MMCO / sliding window / insert) ---
    if (si.nal_type == H264_NAL_IDR_SLICE) {
        for (auto &e : dpb_) free_dpb_slots_.push_back(e.slot);
        dpb_.clear();
        poc_ctx_.prev_frame_num = 0;
        poc_ctx_.prev_frame_num_offset = 0;
        poc_ctx_.prev_poc_msb = 0;
        poc_ctx_.prev_poc_lsb = -1;
    } else if (si.nal_ref_idc != 0) {
        for (int i = 0; i < si.nb_mmco; i++) {
            if (si.mmco[i].opcode == MMCO_SHORT2UNUSED) {
                const int fn = si.mmco[i].short_pic_num;
                for (auto it = dpb_.begin(); it != dpb_.end(); ++it) {
                    if (it->frame_num == fn) { free_dpb_slots_.push_back(it->slot); dpb_.erase(it); break; }
                }
            }
        }
        if (!si.explicit_ref_marking && static_cast<int>(dpb_.size()) >= sps->ref_frame_count && !dpb_.empty()) {
            free_dpb_slots_.push_back(dpb_.back().slot);
            dpb_.pop_back();
        }
    }

    if (si.nal_ref_idc != 0) {
        DpbEntry e;
        e.slot = setup_slot;
        e.frame_num = si.frame_num;
        e.field_poc[0] = field_poc[0];
        e.field_poc[1] = field_poc[1];
        dpb_.insert(dpb_.begin(), e);
    } else {
        free_dpb_slots_.push_back(setup_slot);   // non-reference (B) recycles its temp slot
    }

    decode_order_++;
    dbg("decode ok");
    ff_h2645_packet_uninit(&slice_pkt);
    ff_h264_ps_uninit(&ps);
    return CUDA_SUCCESS;
}

// ===========================================================================
// map_frame
// ===========================================================================

CUresult VulkanH264Decoder::map_frame(int picture_index, HipFrameMapping *mapping) {
    if (!mapping || picture_index < 0 || static_cast<std::size_t>(picture_index) >= exports_.size())
        return CUDA_ERROR_INVALID_VALUE;
    ExportRes &exp = exports_[picture_index];
    if (exp.fd < 0) return CUDA_ERROR_INVALID_VALUE;   // decode() hasn't produced this frame

    // Ownership: map_fd() takes the fd (it is closed on reset()). Hand off.
    const int fd = exp.fd;
    exp.fd = -1;
    return mapping->map_fd(fd, exp.size, exp.pitch, coded_height_);
}

} // namespace cuvid_shim
