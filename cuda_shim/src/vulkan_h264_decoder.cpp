#include "vulkan_h264_decoder.h"

#include "hip_frame_mapper.h"
#include "vk_sequence_cache.h"

#include <vulkan/vulkan.h>
#include <vk_video/vulkan_video_codec_h264std.h>
#include <vk_video/vulkan_video_codec_h264std_decode.h>

#include <hip/hip_runtime_api.h>

#include <cstring>
#include <cstdlib>
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
DECL_PFN(vkCmdDecodeVideoKHR);
DECL_PFN(vkGetMemoryFdKHR);
#undef DECL_PFN

bool load_video_pfns(VkDevice device) {
#define LOAD(nm) p##nm = reinterpret_cast<PFN_##nm>(vkGetDeviceProcAddr(device, #nm)); \
    if (!p##nm) return false;
    LOAD(vkGetPhysicalDeviceVideoCapabilitiesKHR);
    LOAD(vkGetPhysicalDeviceVideoFormatPropertiesKHR);
    LOAD(vkCreateVideoSessionKHR);
    LOAD(vkDestroyVideoSessionKHR);
    LOAD(vkGetVideoSessionMemoryRequirementsKHR);
    LOAD(vkBindVideoSessionMemoryKHR);
    LOAD(vkCreateVideoSessionParametersKHR);
    LOAD(vkDestroyVideoSessionParametersKHR);
    LOAD(vkCmdBeginVideoCodingKHR);
    LOAD(vkCmdEndVideoCodingKHR);
    LOAD(vkCmdDecodeVideoKHR);
    LOAD(vkGetMemoryFdKHR);
#undef LOAD
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
};

bool parse_slice_header(const H2645NAL *nal, const SPS *sps, const PPS *pps, SliceInfo *si) {
    std::memset(si, 0, sizeof(*si));
    GetBitContext gb = nal->gb;
    si->nal_type = nal->type;
    si->nal_ref_idc = nal->ref_idc;

    get_ue_golomb_long(&gb);
    unsigned st = get_ue_golomb_31(&gb);
    if (st > 9) return false;
    if (st > 4) st -= 5;
    static const int to_pict[5] = { AV_PICTURE_TYPE_P, AV_PICTURE_TYPE_B, AV_PICTURE_TYPE_I,
                                    AV_PICTURE_TYPE_SP, AV_PICTURE_TYPE_SI };
    si->slice_type_nos = to_pict[st] & 3;
    if (si->slice_type_nos == AV_PICTURE_TYPE_SP || si->slice_type_nos == AV_PICTURE_TYPE_SI)
        return false;   // SP/SI out of scope

    get_ue_golomb(&gb);   // pic_parameter_set_id
    si->frame_num = get_bits(&gb, sps->log2_max_frame_num);
    if (!sps->frame_mbs_only_flag) return false;   // interlaced unsupported

    if (nal->type == H264_NAL_IDR_SLICE)
        si->idr_pic_id = get_ue_golomb_long(&gb);

    if (sps->poc_type == 0) {
        si->poc_lsb = get_bits(&gb, sps->log2_max_poc_lsb);
        if (pps->pic_order_present == 1)
            si->delta_poc_bottom = get_se_golomb(&gb);
    } else if (sps->poc_type != 2) {
        return false;
    }

    if (pps->redundant_pic_cnt_present) get_ue_golomb(&gb);
    if (si->slice_type_nos == AV_PICTURE_TYPE_B) get_bits1(&gb);   // direct_spatial_mv_pred

    if (si->slice_type_nos != AV_PICTURE_TYPE_I) {
        int list_count = 0, ref_count[2] = {0, 0};
        if (ff_h264_parse_ref_count(&list_count, ref_count, &gb, pps, si->slice_type_nos,
                                    PICT_FRAME, nullptr) < 0)
            return false;
        for (int list = 0; list < list_count; list++) {
            if (!get_bits1(&gb)) continue;
            for (int idx = 0; ; idx++) {
                unsigned op = get_ue_golomb_31(&gb);
                if (op == 3) break;
                if (idx >= ref_count[list] || op > 2) return false;
                get_ue_golomb_long(&gb);
            }
        }
        if ((pps->weighted_pred && si->slice_type_nos == AV_PICTURE_TYPE_P) ||
            (pps->weighted_bipred_idc == 1 && si->slice_type_nos == AV_PICTURE_TYPE_B))
            return false;   // weighted prediction unsupported
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
                        return false;   // only MMCO_SHORT2UNUSED
                    }
                    si->nb_mmco = i + 1;
                }
            }
        }
    }
    return true;
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

        const std::vector<const char *> inst_exts = {
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
            VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
            VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
            VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        };
        VkInstanceCreateInfo vici = {};
        vici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        vici.pApplicationInfo = &app_info;
        vici.enabledExtensionCount = static_cast<uint32_t>(inst_exts.size());
        vici.ppEnabledExtensionNames = inst_exts.data();
        if (vkCreateInstance(&vici, nullptr, &instance_) != VK_SUCCESS) {
            reset();
            return kNotSupported;
        }

        uint32_t pd_count = 0;
        vkEnumeratePhysicalDevices(instance_, &pd_count, nullptr);
        if (pd_count == 0) { reset(); return kNotSupported; }
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
    if (decode_qf_ < 0) { reset(); return kNotSupported; }
    if (transfer_qf_ < 0) transfer_qf_ = 0;

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
        if (vkCreateDevice(physical_device_, &dci, nullptr, &device_) != VK_SUCCESS) {
            reset();
            return kNotSupported;
        }
        if (!load_video_pfns(device_)) { reset(); return kNotSupported; }
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
    if (!sps || !pps) { ff_h264_ps_uninit(&ps); reset(); return kNotSupported; }

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
    caps.pNext = &dec_caps;
    if (pvkGetPhysicalDeviceVideoCapabilitiesKHR(physical_device_, &profile_info_, &caps) != VK_SUCCESS) {
        ff_h264_ps_uninit(&ps); reset(); return kNotSupported;
    }
    max_dpb_slots_ = caps.maxDpbSlots ? caps.maxDpbSlots : 17;
    max_active_refs_ = caps.maxActiveReferencePictures;
    min_bitstream_align_ = caps.minBitstreamBufferSizeAlignment ? caps.minBitstreamBufferSizeAlignment : 128;

    VkPhysicalDeviceProperties pdp;
    vkGetPhysicalDeviceProperties(physical_device_, &pdp);
    pitch_align_ = pdp.limits.optimalBufferCopyRowPitchAlignment;
    if (pitch_align_ == 0) pitch_align_ = 256;

    // --- 6. session ---
    VkVideoSessionCreateInfoKHR session_ci = {};
    session_ci.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    session_ci.pNext = &profile_info_;
    session_ci.queueFamilyIndex = static_cast<uint32_t>(decode_qf_);
    session_ci.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    session_ci.maxCodedExtent = { static_cast<uint32_t>(coded_width_), static_cast<uint32_t>(coded_height_) };
    session_ci.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    session_ci.maxDpbSlots = max_dpb_slots_;
    session_ci.maxActiveReferencePictures = max_active_refs_;
    session_ci.pStdHeaderVersion = nullptr;

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
    if (mem_req_count && pvkBindVideoSessionMemoryKHR(device_, session_, mem_req_count, binds.data()) != VK_SUCCESS) {
        for (auto m : session_mems) if (m) vkFreeMemory(device_, m, nullptr);
        ff_h264_ps_uninit(&ps); reset(); return kNotSupported;
    }
    for (auto m : session_mems) vkFreeMemory(device_, m, nullptr);

    // --- 8. session parameters ---
    StdVideoH264ScalingLists sps_sl, pps_sl;
    StdVideoH264SequenceParameterSetVui sps_vui;
    StdVideoH264SequenceParameterSet std_sps;
    StdVideoH264PictureParameterSet std_pps;
    fill_sps_std(sps, &sps_sl, &sps_vui, &std_sps);
    fill_pps_std(pps, &pps_sl, &std_pps);
    sps_id_ = std_sps.seq_parameter_set_id;
    pps_id_ = std_pps.pic_parameter_set_id;

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
        if (!create_image(VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR, &dpb_images_[i])) {
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

    return CUDA_SUCCESS;
}

// ===========================================================================
// decode
// ===========================================================================

CUresult VulkanH264Decoder::decode(const CUVIDPICPARAMS &picture) {
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
    // picture.pBitstreamData points at the raw slice NAL (Annex-B with start code);
    // picture.nBitstreamDataLen is its length. We wrap it for FFmpeg's parser.
    H2645Packet slice_pkt;
    std::memset(&slice_pkt, 0, sizeof(slice_pkt));
    if (ff_h2645_packet_split(&slice_pkt, picture.pBitstreamData, static_cast<int>(picture.nBitstreamDataLen),
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
    if (!slice_nal) { ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return kNotSupported; }

    SliceInfo si;
    if (!parse_slice_header(slice_nal, sps, pps, &si)) {
        ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return kNotSupported;
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
        ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return kNotSupported;
    }
    if (si.nal_ref_idc != 0) {
        poc_ctx_.prev_poc_msb = poc_ctx_.poc_msb;
        poc_ctx_.prev_poc_lsb = poc_ctx_.poc_lsb;
    }
    poc_ctx_.prev_frame_num = poc_ctx_.frame_num;
    poc_ctx_.prev_frame_num_offset = poc_ctx_.frame_num_offset;
    (void)pic_poc;

    // --- 3. allocate DPB slot for the current (setup) picture ---
    if (free_dpb_slots_.empty()) { ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return CUDA_ERROR_OUT_OF_MEMORY; }
    const int setup_slot = free_dpb_slots_.back();
    free_dpb_slots_.pop_back();

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
    vkResetCommandPool(device_, decode_pool_, 0);
    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(decode_cmd_, &cbbi);

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
    {   // setup slot -> VIDEO_DECODE_DPB
        const bool inited = dpb_layout_init_[setup_slot] != 0;
        VkImageMemoryBarrier2 b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        b.srcAccessMask = inited ? VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR : VK_ACCESS_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        b.dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        b.oldLayout = inited ? VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = dpb_images_[setup_slot].image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo di = {};
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(decode_cmd_, &di);
        dpb_layout_init_[setup_slot] = 1;
    }

    VkVideoBeginCodingInfoKHR begin = {};
    begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    begin.videoSession = session_;
    begin.videoSessionParameters = session_params_;
    begin.referenceSlotCount = static_cast<uint32_t>(ref_slots.size());
    begin.pReferenceSlots = ref_slots.data();
    pvkCmdBeginVideoCodingKHR(decode_cmd_, &begin);

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
        si2.waitSemaphoreInfoCount = (decode_order_ == 0) ? 0 : 1;
        si2.pWaitSemaphoreInfos = &wait_sem;
        si2.commandBufferInfoCount = 1;
        si2.pCommandBufferInfos = &cb;
        si2.signalSemaphoreInfoCount = 1;
        si2.pSignalSemaphoreInfos = &sig_sem;
        if (vkQueueSubmit2(decode_queue_, 1, &si2, VK_NULL_HANDLE) != VK_SUCCESS) {
            ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return CUDA_ERROR_UNKNOWN;
        }
    }

    // --- 10. record transfer command buffer (barrier + copy Y/UV) ---
    ExportRes &exp = exports_[picture.CurrPicIdx];
    vkResetCommandPool(device_, transfer_pool_, 0);
    vkBeginCommandBuffer(transfer_cmd_, &cbbi);
    {
        VkImageMemoryBarrier2 b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        b.srcAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = out.image;
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
    regions[1].bufferOffset = static_cast<VkDeviceSize>(exp.pitch) * H;
    regions[1].bufferRowLength = exp.pitch;
    regions[1].bufferImageHeight = H / 2;
    regions[1].imageSubresource = { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 };
    regions[1].imageExtent = { W, H / 2, 1 };
    vkCmdCopyImageToBuffer(transfer_cmd_, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, exp.buf, 2, regions);
    out.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
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
            ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return CUDA_ERROR_UNKNOWN;
        }
    }
    vkWaitForFences(device_, 1, &copy_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &copy_fence_);

    // --- 11. export the fd for map_frame() to import ---
    if (exp.fd >= 0) close(exp.fd);
    exp.fd = -1;
    {
        VkMemoryGetFdInfoKHR fd_info = {};
        fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fd_info.memory = exp.mem;
        fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (pvkGetMemoryFdKHR(device_, &fd_info, &exp.fd) != VK_SUCCESS || exp.fd < 0) {
            ff_h2645_packet_uninit(&slice_pkt); ff_h264_ps_uninit(&ps); return CUDA_ERROR_UNKNOWN;
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
