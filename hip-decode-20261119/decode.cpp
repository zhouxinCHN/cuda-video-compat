// decode.cpp — Vulkan Video H.264 decode into LINEAR images, read back by HIP.
//
// This demo proves that a decode output image with VK_IMAGE_TILING_LINEAR,
// exported as an OPAQUE_FD dma-buf, can be imported into HIP and read with a
// plain device pointer. It is also the compatibility layer's coverage probe:
// it parses the stream itself (FFmpeg's H.264 parameter set and slice-header
// parsers), drives I/P/B decoding with a real DPB, and compares every decoded
// picture with a software reference in display order.
//
// Coverage is data-driven: profile, chroma format, bit depth, resolution,
// cropping, slice count, reference count and B-pyramid all come from the
// stream. If the driver does not support a combination,
// vkGetPhysicalDeviceVideoCapabilitiesKHR fails and the program exits 77
// (SKIP) with the VkResult, which is a valid result for the matrix runner.
//
// Usage:
//   decode <stream.h264> <reference.raw> <ref-format>
//   decode --probe
//
// ref-format: nv12 | p010le | yuv420p | yuv422p | yuv444p
//             | yuv420p10le | yuv422p10le | yuv444p10le
//
// Build and run: ./run.sh (it also builds gen_stream and runs the matrix).

#include <vulkan/vulkan_core.h>
#include <vk_video/vulkan_video_codec_h264std.h>
#include <vk_video/vulkan_video_codec_h264std_decode.h>
#include <hip/hip_runtime.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

// FFmpeg parser reuse: compile-time headers, link-time 9 objects + libavutil
// (same set as ../../../cuda_shim/Makefile). No libavcodec.so dependency.
extern "C" {
#define HAVE_AV_CONFIG_H 1
#include "libavcodec/avcodec.h"
#include "libavcodec/h2645_parse.h"
#include "libavcodec/h264_ps.h"
#include "libavcodec/h264_parse.h"
#include "libavcodec/h264.h"
#include "libavcodec/golomb.h"
#include "libavcodec/mpegutils.h"
#include "libavcodec/mathops.h"
}

namespace {

constexpr int kExitSkip = 77;

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void check(VkResult result, const char *operation)
{
    require(result == VK_SUCCESS, std::string(operation) + " returned " + std::to_string(result));
}

void check(hipError_t result, const char *operation)
{
    require(result == hipSuccess, std::string(operation) + ": " + hipGetErrorString(result));
}

#define CHECK(call) check((call), #call)

std::vector<uint8_t> read_file(const char *path)
{
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), std::string("Cannot open ") + path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string vk_result_name(VkResult result)
{
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR: return "VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR: return "VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR: return "VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR: return "VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR: return "VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR";
    default: return "VK_ERROR_" + std::to_string(static_cast<int>(result));
    }
}

const char *profile_name(StdVideoH264ProfileIdc idc)
{
    // The Vulkan std header only enumerates four H.264 profiles; High 10 and
    // High 4:2:2 are passed as their profile_idc values.
    switch (static_cast<int>(idc)) {
    case 66: return "baseline";
    case 77: return "main";
    case 100: return "high";
    case 110: return "high10";
    case 122: return "high422";
    case 244: return "high444";
    default: return "other";
    }
}

int sub_width(int chroma) { return chroma == 3 ? 1 : 2; }   // 420/422: 2
int sub_height(int chroma) { return chroma == 1 ? 2 : 1; }  // 420: 2

const char *chroma_name(int chroma)
{
    switch (chroma) {
    case 1: return "4:2:0";
    case 2: return "4:2:2";
    default: return "4:4:4";
    }
}

VkFormat output_format(int chroma, int depth)
{
    if (depth == 8) {
        switch (chroma) {
        case 1: return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        case 2: return VK_FORMAT_G8_B8R8_2PLANE_422_UNORM;
        default: return VK_FORMAT_G8_B8R8_2PLANE_444_UNORM;
        }
    }
    switch (chroma) {
    case 1: return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    case 2: return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16;
    default: return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16;
    }
}

// --- H.264 parameter sets -> StdVideo structures (same conversion as the
// compatibility layer's Vulkan backend, including the 8x8 zigzag order) ---

StdVideoH264LevelIdc convert_to_vk_level_idc(int level_idc)
{
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

const int kScalingList8Order[6] = {0, 3, 1, 4, 2, 5};

void fill_sps_std(const SPS *sps, StdVideoH264ScalingLists *sl,
                  StdVideoH264SequenceParameterSetVui *vui,
                  StdVideoH264SequenceParameterSet *out)
{
    std::memset(sl, 0, sizeof(*sl));
    std::memset(vui, 0, sizeof(*vui));
    std::memset(out, 0, sizeof(*out));

    sl->scaling_list_present_mask = sps->scaling_matrix_present_mask;
    for (int i = 0; i < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_LISTS; i++)
        for (int j = 0; j < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_ELEMENTS; j++)
            sl->ScalingList4x4[i][j] = sps->scaling_matrix4[i][ff_zigzag_scan[j]];
    for (int i = 0; i < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_LISTS; i++)
        for (int j = 0; j < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_ELEMENTS; j++)
            sl->ScalingList8x8[i][j] = sps->scaling_matrix8[kScalingList8Order[i]][ff_zigzag_direct[j]];

    *vui = (StdVideoH264SequenceParameterSetVui){
        .flags = (StdVideoH264SpsVuiFlags){
            .aspect_ratio_info_present_flag = static_cast<uint32_t>(sps->vui.aspect_ratio_info_present_flag),
            .overscan_info_present_flag = static_cast<uint32_t>(sps->vui.overscan_info_present_flag),
            .overscan_appropriate_flag = static_cast<uint32_t>(sps->vui.overscan_appropriate_flag),
            .video_signal_type_present_flag = static_cast<uint32_t>(sps->vui.video_signal_type_present_flag),
            .video_full_range_flag = static_cast<uint32_t>(sps->vui.video_full_range_flag),
            .color_description_present_flag = static_cast<uint32_t>(sps->vui.colour_description_present_flag),
            .chroma_loc_info_present_flag = static_cast<uint32_t>(sps->vui.chroma_loc_info_present_flag),
            .timing_info_present_flag = static_cast<uint32_t>(sps->timing_info_present_flag),
            .fixed_frame_rate_flag = static_cast<uint32_t>(sps->fixed_frame_rate_flag),
            .bitstream_restriction_flag = static_cast<uint32_t>(sps->bitstream_restriction_flag),
            .nal_hrd_parameters_present_flag = static_cast<uint32_t>(sps->nal_hrd_parameters_present_flag),
            .vcl_hrd_parameters_present_flag = static_cast<uint32_t>(sps->vcl_hrd_parameters_present_flag),
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
            .constraint_set0_flag = static_cast<uint32_t>((sps->constraint_set_flags >> 0) & 0x1),
            .constraint_set1_flag = static_cast<uint32_t>((sps->constraint_set_flags >> 1) & 0x1),
            .constraint_set2_flag = static_cast<uint32_t>((sps->constraint_set_flags >> 2) & 0x1),
            .constraint_set3_flag = static_cast<uint32_t>((sps->constraint_set_flags >> 3) & 0x1),
            .constraint_set4_flag = static_cast<uint32_t>((sps->constraint_set_flags >> 4) & 0x1),
            .constraint_set5_flag = static_cast<uint32_t>((sps->constraint_set_flags >> 5) & 0x1),
            .direct_8x8_inference_flag = static_cast<uint32_t>(sps->direct_8x8_inference_flag),
            .mb_adaptive_frame_field_flag = static_cast<uint32_t>(sps->mb_aff),
            .frame_mbs_only_flag = static_cast<uint32_t>(sps->frame_mbs_only_flag),
            .delta_pic_order_always_zero_flag = static_cast<uint32_t>(sps->delta_pic_order_always_zero_flag),
            .separate_colour_plane_flag = static_cast<uint32_t>(sps->residual_color_transform_flag),
            .gaps_in_frame_num_value_allowed_flag = static_cast<uint32_t>(sps->gaps_in_frame_num_allowed_flag),
            .qpprime_y_zero_transform_bypass_flag = static_cast<uint32_t>(sps->transform_bypass),
            .frame_cropping_flag = static_cast<uint32_t>(sps->crop),
            .seq_scaling_matrix_present_flag = static_cast<uint32_t>(sps->scaling_matrix_present),
            .vui_parameters_present_flag = static_cast<uint32_t>(sps->vui_parameters_present_flag),
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
                  StdVideoH264PictureParameterSet *out)
{
    std::memset(sl, 0, sizeof(*sl));
    std::memset(out, 0, sizeof(*out));

    sl->scaling_list_present_mask = pps->pic_scaling_matrix_present_mask;
    for (int i = 0; i < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_LISTS; i++)
        for (int j = 0; j < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_ELEMENTS; j++)
            sl->ScalingList4x4[i][j] = pps->scaling_matrix4[i][ff_zigzag_scan[j]];
    for (int i = 0; i < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_LISTS; i++)
        for (int j = 0; j < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_ELEMENTS; j++)
            sl->ScalingList8x8[i][j] = pps->scaling_matrix8[kScalingList8Order[i]][ff_zigzag_direct[j]];

    *out = (StdVideoH264PictureParameterSet){
        .flags = (StdVideoH264PpsFlags){
            .transform_8x8_mode_flag = static_cast<uint32_t>(pps->transform_8x8_mode),
            .redundant_pic_cnt_present_flag = static_cast<uint32_t>(pps->redundant_pic_cnt_present),
            .constrained_intra_pred_flag = static_cast<uint32_t>(pps->constrained_intra_pred),
            .deblocking_filter_control_present_flag = static_cast<uint32_t>(pps->deblocking_filter_parameters_present),
            .weighted_pred_flag = static_cast<uint32_t>(pps->weighted_pred),
            .bottom_field_pic_order_in_frame_present_flag = static_cast<uint32_t>(pps->pic_order_present),
            .entropy_coding_mode_flag = static_cast<uint32_t>(pps->cabac),
            .pic_scaling_matrix_present_flag = static_cast<uint32_t>(pps->pic_scaling_matrix_present_flag),
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

// --- slice header ---

struct MMCOOp {
    int opcode = 0;
    int short_pic_num = 0;
    int long_arg = 0;
};

struct SliceInfo {
    int nal_type = 0;
    int nal_ref_idc = 0;
    int first_mb_in_slice = 0;
    int slice_type_nos = 0;   // AV_PICTURE_TYPE_I/P/B
    int pps_id = 0;
    int frame_num = 0;
    int idr_pic_id = 0;
    int poc_lsb = 0;
    int delta_poc_bottom = 0;
    int delta_poc[2] = {0, 0};
    int nb_mmco = 0;
    MMCOOp mmco[32];
    int explicit_ref_marking = 0;
    int ref_count[2] = {0, 0};
    int picture_poc[2] = {0, 0};   // filled by the caller after POC computation
};

bool parse_slice_header(const H2645NAL *nal, const SPS *sps, const PPS *pps, SliceInfo *si)
{
    std::memset(si, 0, sizeof(*si));
    GetBitContext gb = nal->gb;
    si->nal_type = nal->type;
    si->nal_ref_idc = nal->ref_idc;

    si->first_mb_in_slice = static_cast<int>(get_ue_golomb_long(&gb));
    unsigned st = get_ue_golomb_31(&gb);
    if (st > 9) return false;
    if (st > 4) st -= 5;
    static const int to_pict[5] = {AV_PICTURE_TYPE_P, AV_PICTURE_TYPE_B, AV_PICTURE_TYPE_I,
                                   AV_PICTURE_TYPE_SP, AV_PICTURE_TYPE_SI};
    si->slice_type_nos = to_pict[st] & 3;
    if (si->slice_type_nos == AV_PICTURE_TYPE_SP || si->slice_type_nos == AV_PICTURE_TYPE_SI)
        return false;   // SP/SI out of scope

    si->pps_id = static_cast<int>(get_ue_golomb(&gb));
    si->frame_num = get_bits(&gb, sps->log2_max_frame_num);
    if (!sps->frame_mbs_only_flag) return false;   // interlaced out of scope

    if (nal->type == H264_NAL_IDR_SLICE)
        si->idr_pic_id = static_cast<int>(get_ue_golomb_long(&gb));

    if (sps->poc_type == 0) {
        si->poc_lsb = get_bits(&gb, sps->log2_max_poc_lsb);
        if (pps->pic_order_present == 1)
            si->delta_poc_bottom = get_se_golomb(&gb);
    } else if (sps->poc_type == 1) {
        si->delta_poc[0] = get_se_golomb(&gb);
        if (!sps->delta_pic_order_always_zero_flag) {
            si->delta_poc[1] = get_se_golomb(&gb);
            if (pps->pic_order_present == 1 && !sps->frame_mbs_only_flag)
                (void)get_se_golomb(&gb);
        }
    }

    if (pps->redundant_pic_cnt_present) (void)get_ue_golomb(&gb);
    if (si->slice_type_nos == AV_PICTURE_TYPE_B) (void)get_bits1(&gb);   // direct_spatial_mv_pred

    if (si->slice_type_nos != AV_PICTURE_TYPE_I) {
        int list_count = 0;
        if (ff_h264_parse_ref_count(&list_count, si->ref_count, &gb, pps, si->slice_type_nos,
                                    PICT_FRAME, nullptr) < 0)
            return false;
        for (int list = 0; list < list_count; list++) {
            if (!get_bits1(&gb)) continue;
            for (int idx = 0;; idx++) {
                unsigned op = get_ue_golomb_31(&gb);
                if (op == 3) break;
                if (idx >= si->ref_count[list] || op > 2) return false;
                (void)get_ue_golomb_long(&gb);
            }
        }
        if ((pps->weighted_pred && si->slice_type_nos == AV_PICTURE_TYPE_P) ||
            (pps->weighted_bipred_idc == 1 && si->slice_type_nos == AV_PICTURE_TYPE_B)) {
            H264PredWeightTable pwt;
            if (ff_h264_pred_weight_table(&gb, sps, si->ref_count, si->slice_type_nos,
                                          &pwt, PICT_FRAME, nullptr) < 0)
                return false;
        }
    }

    if (nal->ref_idc) {
        if (nal->type == H264_NAL_IDR_SLICE) {
            (void)get_bits1(&gb);   // no_output_of_prior_pics_flag
            if (get_bits1(&gb)) {   // long_term_reference_flag
                si->mmco[0].opcode = MMCO_LONG;
                si->mmco[0].long_arg = 0;
                si->nb_mmco = 1;
            }
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
                            (si->frame_num - get_ue_golomb_long(&gb) - 1) &
                            ((1 << sps->log2_max_frame_num) - 1);
                    } else {
                        return false;   // only MMCO 1 is tracked (LTR is out of scope)
                    }
                    si->nb_mmco = i + 1;
                }
            }
        }
    }
    return true;
}

// --- reference frames ---

struct RefFormat {
    const char *name = nullptr;
    int chroma = 1;
    int depth = 8;
    bool planar = false;       // false: interleaved CbCr plane (nv12 / p010)
    bool msb_aligned = false;  // P010-style 10-bit in the high bits
};

bool parse_ref_format(const char *name, RefFormat *out)
{
    struct Entry { const char *name; int chroma; int depth; bool planar; bool msb; };
    static const Entry entries[] = {
        {"nv12", 1, 8, false, false},
        {"p010le", 1, 10, false, true},
        {"yuv420p", 1, 8, true, false},
        {"yuv422p", 2, 8, true, false},
        {"yuv444p", 3, 8, true, false},
        {"yuv420p10le", 1, 10, true, false},
        {"yuv422p10le", 2, 10, true, false},
        {"yuv444p10le", 3, 10, true, false},
    };
    for (const Entry &entry : entries) {
        if (!std::strcmp(name, entry.name)) {
            out->name = entry.name;
            out->chroma = entry.chroma;
            out->depth = entry.depth;
            out->planar = entry.planar;
            out->msb_aligned = entry.msb;
            return true;
        }
    }
    return false;
}

struct RefFrame {
    std::vector<uint16_t> y, u, v;
};

std::vector<RefFrame> load_reference(const char *path, const RefFormat &format,
                                     int width, int height, int count)
{
    const std::vector<uint8_t> data = read_file(path);
    const int cw = width / sub_width(format.chroma);
    const int ch = height / sub_height(format.chroma);
    const int bytes = format.depth > 8 ? 2 : 1;
    const size_t luma_samples = static_cast<size_t>(width) * height;
    const size_t chroma_samples = static_cast<size_t>(cw) * ch;
    const size_t frame_size = (luma_samples + 2 * chroma_samples) * bytes;
    require(data.size() == frame_size * count,
            "reference size " + std::to_string(data.size()) + " != " + std::to_string(frame_size * count) +
                " (" + std::to_string(count) + " frames)");

    auto sample = [&](const uint8_t *base, size_t offset) -> uint16_t {
        if (bytes == 1) return base[offset];
        uint16_t word = 0;
        std::memcpy(&word, base + offset * 2, sizeof(word));
        return format.msb_aligned ? static_cast<uint16_t>(word >> 6)
                                  : static_cast<uint16_t>(word & 0x3ff);
    };

    std::vector<RefFrame> frames(count);
    for (int f = 0; f < count; ++f) {
        const uint8_t *base = data.data() + f * frame_size;
        RefFrame &frame = frames[f];
        frame.y.resize(luma_samples);
        frame.u.resize(chroma_samples);
        frame.v.resize(chroma_samples);
        for (size_t i = 0; i < luma_samples; ++i)
            frame.y[i] = sample(base, i);
        if (format.planar) {
            for (size_t i = 0; i < chroma_samples; ++i) {
                frame.u[i] = sample(base, luma_samples + i);
                frame.v[i] = sample(base, luma_samples + chroma_samples + i);
            }
        } else {
            for (size_t i = 0; i < chroma_samples; ++i) {
                frame.u[i] = sample(base, luma_samples + i * 2);
                frame.v[i] = sample(base, luma_samples + i * 2 + 1);
            }
        }
    }
    return frames;
}

// --- HIP comparison ---

__device__ unsigned load_sample(const uint8_t *p, unsigned bytes, int depth)
{
    if (bytes == 1) return *p;
    uint16_t word = *reinterpret_cast<const uint16_t *>(p);
    return depth > 8 ? (word >> 6) : word;
}

__global__ void compare_planes(const uint8_t *y, size_t y_pitch, unsigned y_bytes,
                               const uint8_t *uv, size_t uv_pitch, unsigned uv_bytes,
                               const uint16_t *ref_y, const uint16_t *ref_u, const uint16_t *ref_v,
                               int width, int height, int chroma_w, int chroma_h, int depth,
                               int y0, int x0, int yc0, int xc0, unsigned *errors)
{
    const int luma = width * height;
    const int total = luma + chroma_w * chroma_h;
    for (int idx = blockIdx.x * blockDim.x + threadIdx.x; idx < total;
         idx += blockDim.x * gridDim.x) {
        if (idx < luma) {
            const int r = idx / width;
            const int c = idx - r * width;
            const unsigned got = load_sample(y + static_cast<size_t>(y0 + r) * y_pitch +
                                                 static_cast<size_t>(x0 + c) * y_bytes,
                                             y_bytes, depth);
            const unsigned want = ref_y[idx];
            const unsigned diff = got > want ? got - want : want - got;
            if (diff) atomicMax(&errors[0], diff);
        } else {
            const int rel = idx - luma;
            const int j = rel / chroma_w;
            const int i = rel - j * chroma_w;
            const uint8_t *base = uv + static_cast<size_t>(yc0 + j) * uv_pitch +
                                  static_cast<size_t>(xc0 + i) * 2 * uv_bytes;
            const unsigned cb = load_sample(base, uv_bytes, depth);
            const unsigned cr = load_sample(base + uv_bytes, uv_bytes, depth);
            const unsigned want_u = ref_u[rel];
            const unsigned want_v = ref_v[rel];
            unsigned diff = cb > want_u ? cb - want_u : want_u - cb;
            const unsigned diff_v = cr > want_v ? cr - want_v : want_v - cr;
            if (diff_v > diff) diff = diff_v;
            if (diff) atomicMax(&errors[1], diff);
        }
    }
}

// --- Vulkan object graph ---

struct PlaneLayout {
    VkDeviceSize offset = 0;
    VkDeviceSize row_pitch = 0;
};

struct OutputFrame {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    int fd = -1;
    hipExternalMemory_t hip_memory = nullptr;
    void *hip_pointer = nullptr;
    PlaneLayout planes[2];
    int poc = 0;
    int gop = 0;
};

struct DpbImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct DpbEntry {
    int slot = -1;
    int frame_num = 0;
    int field_poc[2] = {0, 0};
};

class Decoder {
public:
    Decoder() = default;
    ~Decoder() { reset(); }
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    void init();
    int set_up(const SPS *sps, const PPS *pps);   // 0 ok, kExitSkip unsupported
    void decode_access_unit(const std::vector<const H2645NAL *> &slices,
                            const std::vector<SliceInfo> &infos);
    void update_dpb(const SliceInfo &si, int setup_slot);
    void probe();
    void reset();

    int coded_width() const { return coded_width_; }
    int coded_height() const { return coded_height_; }
    int display_width() const { return coded_width_ - static_cast<int>(crop_left_ + crop_right_); }
    int display_height() const { return coded_height_ - static_cast<int>(crop_top_ + crop_bottom_); }
    int crop_left() const { return static_cast<int>(crop_left_); }
    int crop_top() const { return static_cast<int>(crop_top_); }
    int chroma() const { return chroma_; }
    int bit_depth() const { return bit_depth_; }
    int setup_slot() const { return setup_slot_; }
    const std::vector<OutputFrame> &frames() const { return outputs_; }

private:
    void create_image(bool shared, VkFormat format, VkImageUsageFlags usage, VkImageTiling tiling,
                      const void *pnext, VkImageCreateFlags flags, VkImage *image,
                      VkDeviceMemory *memory, VkImageView *view);
    uint32_t pick_memory_type(uint32_t bits, VkMemoryPropertyFlags wanted);
    int allocate_slot();
    void ensure_dpb_image(int slot);
    bool has_dst_format() const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkQueryPool query_ = VK_NULL_HANDLE;
    VkVideoSessionKHR session_ = VK_NULL_HANDLE;
    VkVideoSessionParametersKHR parameters_ = VK_NULL_HANDLE;
    VkBuffer bitstream_ = VK_NULL_HANDLE;
    VkDeviceMemory bitstream_memory_ = VK_NULL_HANDLE;
    VkDeviceSize bitstream_size_ = 0;
    VkDeviceSize bitstream_align_ = 128;
    std::vector<VkDeviceMemory> session_memory_;
    bool query_supported_ = false;

    VkVideoProfileInfoKHR profile_{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    VkVideoDecodeH264ProfileInfoKHR h264_profile_{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
    VkVideoProfileListInfoKHR profile_list_{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    int chroma_ = 1;
    int bit_depth_ = 8;
    int coded_width_ = 0;
    int coded_height_ = 0;
    unsigned crop_left_ = 0, crop_right_ = 0, crop_top_ = 0, crop_bottom_ = 0;
    const SPS *sps_ = nullptr;
    const PPS *pps_ = nullptr;

    uint32_t max_dpb_slots_ = 17;
    std::vector<DpbImage> dpb_images_;
    std::vector<int> free_slots_;
    std::vector<DpbEntry> dpb_;
    std::vector<OutputFrame> outputs_;
    int setup_slot_ = -1;
    int gop_ = 0;   // POC restarts at every IDR; display order sorts per GOP

    PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR GetVideoCapabilities_ = nullptr;
    PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR GetVideoFormats_ = nullptr;
    PFN_vkCreateVideoSessionKHR CreateVideoSession_ = nullptr;
    PFN_vkDestroyVideoSessionKHR DestroyVideoSession_ = nullptr;
    PFN_vkGetVideoSessionMemoryRequirementsKHR GetVideoSessionMemory_ = nullptr;
    PFN_vkBindVideoSessionMemoryKHR BindVideoSessionMemory_ = nullptr;
    PFN_vkCreateVideoSessionParametersKHR CreateVideoParameters_ = nullptr;
    PFN_vkDestroyVideoSessionParametersKHR DestroyVideoParameters_ = nullptr;
    PFN_vkCmdBeginVideoCodingKHR BeginVideo_ = nullptr;
    PFN_vkCmdControlVideoCodingKHR ControlVideo_ = nullptr;
    PFN_vkCmdDecodeVideoKHR DecodeVideo_ = nullptr;
    PFN_vkCmdEndVideoCodingKHR EndVideo_ = nullptr;
    PFN_vkGetMemoryFdKHR GetMemoryFd_ = nullptr;
};

void Decoder::init()
{
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Vulkan Video H.264 linear output to HIP";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &application;
    CHECK(vkCreateInstance(&create, nullptr, &instance_));

    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    CHECK(vkEnumeratePhysicalDevices(instance_, &count, devices.data()));
    for (auto candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.vendorID == 0x1002 && properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical_ = candidate;
            std::printf("vulkan_device=%s\n", properties.deviceName);
            break;
        }
    }
    require(physical_ != VK_NULL_HANDLE, "No discrete AMD Vulkan device");

    VkPhysicalDevicePCIBusInfoPropertiesEXT pci{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &pci;
    vkGetPhysicalDeviceProperties2(physical_, &properties);
    char bus[32]{};
    std::snprintf(bus, sizeof(bus), "%04x:%02x:%02x.%u", pci.pciDomain, pci.pciBus,
                  pci.pciDevice, pci.pciFunction);
    int hip_count = 0;
    CHECK(hipGetDeviceCount(&hip_count));
    int hip_device = -1;
    for (int i = 0; i < hip_count; ++i) {
        char hip_bus[32]{};
        CHECK(hipDeviceGetPCIBusId(hip_bus, sizeof(hip_bus), i));
        if (!std::strcmp(bus, hip_bus)) {
            hip_device = i;
            break;
        }
    }
    require(hip_device >= 0, "HIP device does not match Vulkan PCI address");
    CHECK(hipSetDevice(hip_device));
    std::printf("same_gpu=%s hip_device=%d\n", bus, hip_device);

    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, queues.data());
    for (family_ = 0; family_ < count; ++family_) {
        if (queues[family_].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)
            break;
    }
    require(family_ < count, "No H.264 decode queue");

    {
        std::vector<VkQueueFamilyQueryResultStatusPropertiesKHR> status(count);
        std::vector<VkQueueFamilyVideoPropertiesKHR> video(count);
        std::vector<VkQueueFamilyProperties2> props(count);
        for (uint32_t i = 0; i < count; ++i) {
            status[i] = {VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR};
            video[i] = {VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR};
            video[i].pNext = &status[i];
            props[i] = {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2};
            props[i].pNext = &video[i];
        }
        vkGetPhysicalDeviceQueueFamilyProperties2(physical_, &count, props.data());
        query_supported_ = status[family_].queryResultStatusSupport != VK_FALSE;
    }

    VkPhysicalDeviceVideoMaintenance1FeaturesKHR maintenance{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2Features sync{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    maintenance.pNext = &sync;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &maintenance;
    vkGetPhysicalDeviceFeatures2(physical_, &features);
    require(maintenance.videoMaintenance1, "videoMaintenance1 unavailable");
    require(sync.synchronization2, "synchronization2 unavailable");

    const char *extensions[]{VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
                             VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME, VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
                             VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME};
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &maintenance;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = std::size(extensions);
    device_info.ppEnabledExtensionNames = extensions;
    CHECK(vkCreateDevice(physical_, &device_info, nullptr, &device_));
    vkGetDeviceQueue(device_, family_, 0, &queue_);

#define LOAD(field, name, proc) \
    field = reinterpret_cast<PFN_vk##name>(proc); \
    require(field != nullptr, "vk" #name " unavailable")
    GetVideoCapabilities_ = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR>(
        vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceVideoCapabilitiesKHR"));
    require(GetVideoCapabilities_ != nullptr, "vkGetPhysicalDeviceVideoCapabilitiesKHR unavailable");
    GetVideoFormats_ = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR>(
        vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceVideoFormatPropertiesKHR"));
    require(GetVideoFormats_ != nullptr, "vkGetPhysicalDeviceVideoFormatPropertiesKHR unavailable");
    LOAD(CreateVideoSession_, CreateVideoSessionKHR, vkGetDeviceProcAddr(device_, "vkCreateVideoSessionKHR"));
    LOAD(DestroyVideoSession_, DestroyVideoSessionKHR, vkGetDeviceProcAddr(device_, "vkDestroyVideoSessionKHR"));
    LOAD(GetVideoSessionMemory_, GetVideoSessionMemoryRequirementsKHR,
         vkGetDeviceProcAddr(device_, "vkGetVideoSessionMemoryRequirementsKHR"));
    LOAD(BindVideoSessionMemory_, BindVideoSessionMemoryKHR,
         vkGetDeviceProcAddr(device_, "vkBindVideoSessionMemoryKHR"));
    LOAD(CreateVideoParameters_, CreateVideoSessionParametersKHR,
         vkGetDeviceProcAddr(device_, "vkCreateVideoSessionParametersKHR"));
    LOAD(DestroyVideoParameters_, DestroyVideoSessionParametersKHR,
         vkGetDeviceProcAddr(device_, "vkDestroyVideoSessionParametersKHR"));
    LOAD(BeginVideo_, CmdBeginVideoCodingKHR, vkGetDeviceProcAddr(device_, "vkCmdBeginVideoCodingKHR"));
    LOAD(ControlVideo_, CmdControlVideoCodingKHR, vkGetDeviceProcAddr(device_, "vkCmdControlVideoCodingKHR"));
    LOAD(DecodeVideo_, CmdDecodeVideoKHR, vkGetDeviceProcAddr(device_, "vkCmdDecodeVideoKHR"));
    LOAD(EndVideo_, CmdEndVideoCodingKHR, vkGetDeviceProcAddr(device_, "vkCmdEndVideoCodingKHR"));
    LOAD(GetMemoryFd_, GetMemoryFdKHR, vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
#undef LOAD

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family_;
    CHECK(vkCreateCommandPool(device_, &pool_info, nullptr, &pool_));
    VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_info.commandPool = pool_;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    CHECK(vkAllocateCommandBuffers(device_, &cmd_info, &cmd_));
}

uint32_t Decoder::pick_memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &properties);
    for (uint32_t type = 0; type < properties.memoryTypeCount; ++type) {
        if ((bits & (1u << type)) && (properties.memoryTypes[type].propertyFlags & wanted) == wanted)
            return type;
    }
    return 0xffffffffu;
}

void Decoder::create_image(bool shared, VkFormat format, VkImageUsageFlags usage, VkImageTiling tiling,
                           const void *pnext, VkImageCreateFlags flags, VkImage *image,
                           VkDeviceMemory *memory, VkImageView *view)
{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.pNext = pnext;
    info.flags = flags;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {static_cast<uint32_t>(coded_width_), static_cast<uint32_t>(coded_height_), 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = tiling;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (shared) {
        VkPhysicalDeviceExternalImageFormatInfo sharing{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        sharing.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkPhysicalDeviceImageFormatInfo2 format_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        format_info.pNext = &sharing;
        format_info.format = format;
        format_info.type = info.imageType;
        format_info.tiling = info.tiling;
        format_info.usage = info.usage;
        format_info.flags = info.flags;
        VkExternalImageFormatProperties external_props{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        props.pNext = &external_props;
        CHECK(vkGetPhysicalDeviceImageFormatProperties2(physical_, &format_info, &props));
        require(external_props.externalMemoryProperties.externalMemoryFeatures &
                    VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT,
                "Linear decode output cannot export OPAQUE_FD");
    }
    CHECK(vkCreateImage(device_, &info, nullptr, image));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, *image, &req);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = *image;
    VkExportMemoryAllocateInfo exporting{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exporting.pNext = shared ? static_cast<const void *>(&dedicated) : nullptr;
    exporting.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext = shared ? static_cast<const void *>(&exporting) : nullptr;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = pick_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    require(alloc.memoryTypeIndex != 0xffffffffu, "No device-local memory type");
    CHECK(vkAllocateMemory(device_, &alloc, nullptr, memory));
    CHECK(vkBindImageMemory(device_, *image, *memory, 0));

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = *image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CHECK(vkCreateImageView(device_, &view_info, nullptr, view));
}

int Decoder::set_up(const SPS *sps, const PPS *pps)
{
    sps_ = sps;
    pps_ = pps;
    chroma_ = sps->chroma_format_idc;
    bit_depth_ = sps->bit_depth_luma;
    if (chroma_ < 1 || chroma_ > 3) {
        std::printf("SKIP chroma_format_idc=%d\n", chroma_);
        return kExitSkip;
    }
    if (sps->residual_color_transform_flag) {
        std::printf("SKIP separate_colour_plane_flag\n");
        return kExitSkip;
    }
    if (sps->bit_depth_luma != sps->bit_depth_chroma) {
        std::printf("SKIP asymmetric bit depths luma=%d chroma=%d\n",
                    sps->bit_depth_luma, sps->bit_depth_chroma);
        return kExitSkip;
    }
    if (bit_depth_ > 10) {
        std::printf("SKIP bit depth %d (only 8/10 implemented)\n", bit_depth_);
        return kExitSkip;
    }
    coded_width_ = 16 * sps->mb_width;
    coded_height_ = 16 * sps->mb_height;
    crop_left_ = sps->crop_left;
    crop_right_ = sps->crop_right;
    crop_top_ = sps->crop_top;
    crop_bottom_ = sps->crop_bottom;
    format_ = output_format(chroma_, bit_depth_);

    profile_ = {};
    profile_.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    profile_.chromaSubsampling = chroma_ == 1 ? VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR
                                 : chroma_ == 2 ? VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR
                                                : VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;
    profile_.lumaBitDepth = bit_depth_ == 8 ? VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR
                                            : VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
    profile_.chromaBitDepth = profile_.lumaBitDepth;
    h264_profile_ = {};
    h264_profile_.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
    h264_profile_.stdProfileIdc = static_cast<StdVideoH264ProfileIdc>(sps->profile_idc);
    h264_profile_.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
    profile_.pNext = &h264_profile_;
    profile_list_.profileCount = 1;
    profile_list_.pProfiles = &profile_;

    VkVideoDecodeH264CapabilitiesKHR h264_caps{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
    VkVideoDecodeCapabilitiesKHR decode_caps{VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR};
    decode_caps.pNext = &h264_caps;
    VkVideoCapabilitiesKHR caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
    caps.pNext = &decode_caps;
    const VkResult caps_result = GetVideoCapabilities_(physical_, &profile_, &caps);
    if (caps_result != VK_SUCCESS) {
        std::printf("SKIP profile=%s chroma=%s depth=%d caps=%s (%d)\n",
                    profile_name(static_cast<StdVideoH264ProfileIdc>(sps->profile_idc)),
                    chroma_name(chroma_), bit_depth_, vk_result_name(caps_result).c_str(),
                    static_cast<int>(caps_result));
        return kExitSkip;
    }
    if (!has_dst_format()) {
        std::printf("SKIP profile=%s chroma=%s depth=%d dst-format=%d not listed by "
                    "vkGetPhysicalDeviceVideoFormatPropertiesKHR\n",
                    profile_name(static_cast<StdVideoH264ProfileIdc>(sps->profile_idc)),
                    chroma_name(chroma_), bit_depth_, static_cast<int>(format_));
        return kExitSkip;
    }
    max_dpb_slots_ = caps.maxDpbSlots ? caps.maxDpbSlots : 17;
    const uint32_t max_active_refs = caps.maxActiveReferencePictures ? caps.maxActiveReferencePictures : 1;
    bitstream_align_ = caps.minBitstreamBufferSizeAlignment ? caps.minBitstreamBufferSizeAlignment : 128;
    if (caps.minCodedExtent.width > static_cast<uint32_t>(coded_width_) ||
        caps.minCodedExtent.height > static_cast<uint32_t>(coded_height_) ||
        caps.maxCodedExtent.width < static_cast<uint32_t>(coded_width_) ||
        caps.maxCodedExtent.height < static_cast<uint32_t>(coded_height_)) {
        std::printf("SKIP coded=%dx%d outside driver extent %ux%u..%ux%u\n",
                    coded_width_, coded_height_, caps.minCodedExtent.width, caps.minCodedExtent.height,
                    caps.maxCodedExtent.width, caps.maxCodedExtent.height);
        return kExitSkip;
    }

    VkVideoSessionCreateInfoKHR session_info{VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    session_info.queueFamilyIndex = family_;
    session_info.pVideoProfile = &profile_;
    session_info.pictureFormat = format_;
    session_info.maxCodedExtent = {static_cast<uint32_t>(coded_width_), static_cast<uint32_t>(coded_height_)};
    session_info.referencePictureFormat = format_;
    session_info.maxDpbSlots = max_dpb_slots_;
    session_info.maxActiveReferencePictures = max_active_refs;
    session_info.pStdHeaderVersion = &caps.stdHeaderVersion;
    CHECK(CreateVideoSession_(device_, &session_info, nullptr, &session_));

    uint32_t mem_count = 0;
    CHECK(GetVideoSessionMemory_(device_, session_, &mem_count, nullptr));
    std::vector<VkVideoSessionMemoryRequirementsKHR> requirements(
        mem_count, {VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR});
    CHECK(GetVideoSessionMemory_(device_, session_, &mem_count, requirements.data()));
    std::vector<VkBindVideoSessionMemoryInfoKHR> binds(
        mem_count, {VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR});
    session_memory_.resize(mem_count);
    for (uint32_t i = 0; i < mem_count; ++i) {
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = requirements[i].memoryRequirements.size;
        alloc.memoryTypeIndex = pick_memory_type(requirements[i].memoryRequirements.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        require(alloc.memoryTypeIndex != 0xffffffffu, "No session memory type");
        CHECK(vkAllocateMemory(device_, &alloc, nullptr, &session_memory_[i]));
        binds[i].memoryBindIndex = requirements[i].memoryBindIndex;
        binds[i].memory = session_memory_[i];
        binds[i].memorySize = requirements[i].memoryRequirements.size;
    }
    CHECK(BindVideoSessionMemory_(device_, session_, mem_count, binds.data()));

    StdVideoH264ScalingLists sps_sl{}, pps_sl{};
    StdVideoH264SequenceParameterSetVui vui{};
    StdVideoH264SequenceParameterSet std_sps{};
    StdVideoH264PictureParameterSet std_pps{};
    fill_sps_std(sps, &sps_sl, &vui, &std_sps);
    fill_pps_std(pps, &pps_sl, &std_pps);

    VkVideoDecodeH264SessionParametersAddInfoKHR add{
        VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
    add.stdSPSCount = 1;
    add.pStdSPSs = &std_sps;
    add.stdPPSCount = 1;
    add.pStdPPSs = &std_pps;
    VkVideoDecodeH264SessionParametersCreateInfoKHR h264_params{
        VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
    h264_params.maxStdSPSCount = 1;
    h264_params.maxStdPPSCount = 1;
    h264_params.pParametersAddInfo = &add;
    VkVideoSessionParametersCreateInfoKHR params{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
    params.pNext = &h264_params;
    params.videoSession = session_;
    CHECK(CreateVideoParameters_(device_, &params, nullptr, &parameters_));

    bitstream_size_ = static_cast<VkDeviceSize>(coded_width_) * coded_height_ * 3 / 2 + 4 + bitstream_align_;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.pNext = &profile_list_;
    buffer_info.size = bitstream_size_;
    buffer_info.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
    CHECK(vkCreateBuffer(device_, &buffer_info, nullptr, &bitstream_));
    VkMemoryRequirements buffer_req{};
    vkGetBufferMemoryRequirements(device_, bitstream_, &buffer_req);
    VkMemoryAllocateInfo buffer_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    buffer_alloc.allocationSize = buffer_req.size;
    buffer_alloc.memoryTypeIndex = pick_memory_type(
        buffer_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    require(buffer_alloc.memoryTypeIndex != 0xffffffffu, "No host-visible bitstream memory");
    CHECK(vkAllocateMemory(device_, &buffer_alloc, nullptr, &bitstream_memory_));
    CHECK(vkBindBufferMemory(device_, bitstream_, bitstream_memory_, 0));

    if (query_supported_) {
        VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query_info.pNext = &profile_;
        query_info.queryType = VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR;
        query_info.queryCount = 1;
        CHECK(vkCreateQueryPool(device_, &query_info, nullptr, &query_));
    }

    VkVideoBeginCodingInfoKHR begin{VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin.videoSession = session_;
    begin.videoSessionParameters = parameters_;
    VkVideoCodingControlInfoKHR control{VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
    control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    VkVideoEndCodingInfoKHR end{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkResetCommandBuffer(cmd_, 0);
    CHECK(vkBeginCommandBuffer(cmd_, &begin_info));
    BeginVideo_(cmd_, &begin);
    ControlVideo_(cmd_, &control);
    EndVideo_(cmd_, &end);
    CHECK(vkEndCommandBuffer(cmd_));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    CHECK(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue_));

    outputs_.clear();
    dpb_.clear();
    dpb_images_.assign(max_dpb_slots_, DpbImage{});
    free_slots_.clear();
    for (uint32_t i = 0; i < max_dpb_slots_; ++i)
        free_slots_.push_back(static_cast<int>(i));
    std::printf("setup profile=%s chroma=%s depth=%d coded=%dx%d display=%dx%d format=%d\n",
                profile_name(static_cast<StdVideoH264ProfileIdc>(sps->profile_idc)), chroma_name(chroma_),
                bit_depth_, coded_width_, coded_height_, display_width(), display_height(),
                static_cast<int>(format_));
    return 0;
}

int Decoder::allocate_slot()
{
    if (free_slots_.empty()) return -1;
    auto it = std::min_element(free_slots_.begin(), free_slots_.end());
    const int slot = *it;
    free_slots_.erase(it);
    return slot;
}

void Decoder::ensure_dpb_image(int slot)
{
    DpbImage &image = dpb_images_[slot];
    if (image.image != VK_NULL_HANDLE) return;
    create_image(false, format_, VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR, VK_IMAGE_TILING_OPTIMAL,
                 &profile_list_, 0, &image.image, &image.memory, &image.view);
}

void Decoder::decode_access_unit(const std::vector<const H2645NAL *> &slices,
                                 const std::vector<SliceInfo> &infos)
{
    require(!slices.empty() && slices.size() == infos.size(), "empty access unit");

    std::vector<uint8_t> bitstream;
    std::vector<uint32_t> offsets;
    for (const H2645NAL *nal : slices) {
        const uint8_t *raw = nal->raw_data ? nal->raw_data : nal->data;
        const size_t size = nal->raw_data ? static_cast<size_t>(nal->raw_size)
                                          : static_cast<size_t>(nal->size);
        offsets.push_back(static_cast<uint32_t>(bitstream.size()));
        bitstream.insert(bitstream.end(), {0, 0, 0, 1});
        bitstream.insert(bitstream.end(), raw, raw + size);
    }
    require(bitstream.size() <= bitstream_size_, "access unit exceeds the bitstream buffer");
    void *mapped = nullptr;
    CHECK(vkMapMemory(device_, bitstream_memory_, 0, bitstream_size_, 0, &mapped));
    std::memset(mapped, 0, bitstream_size_);
    std::memcpy(mapped, bitstream.data(), bitstream.size());
    vkUnmapMemory(device_, bitstream_memory_);

    setup_slot_ = allocate_slot();
    require(setup_slot_ >= 0, "no free DPB slot");
    ensure_dpb_image(setup_slot_);

    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    OutputFrame frame;
    create_image(true, format_, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR, VK_IMAGE_TILING_LINEAR,
                 &external, VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR,
                 &frame.image, &frame.memory, &frame.view);
    outputs_.reserve(outputs_.size() + 1);
    outputs_.push_back(frame);
    OutputFrame &record = outputs_.back();

    CHECK(vkResetCommandBuffer(cmd_, 0));
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(cmd_, &begin_info));
    if (query_) vkCmdResetQueryPool(cmd_, query_, 0, 1);

    auto barrier = [&](VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                       VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                       VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        VkImageMemoryBarrier2 transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        transition.srcStageMask = src_stage;
        transition.srcAccessMask = src_access;
        transition.dstStageMask = dst_stage;
        transition.dstAccessMask = dst_access;
        transition.oldLayout = old_layout;
        transition.newLayout = new_layout;
        transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        transition.image = image;
        transition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &transition;
        vkCmdPipelineBarrier2(cmd_, &dependency);
    };

    barrier(record.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR, 0, 0,
            VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);

    std::vector<VkVideoReferenceSlotInfoKHR> ref_slots;
    std::vector<VkVideoPictureResourceInfoKHR> ref_resources;
    std::vector<VkVideoDecodeH264DpbSlotInfoKHR> ref_dpb;
    std::vector<StdVideoDecodeH264ReferenceInfo> ref_std;
    ref_slots.reserve(dpb_.size() + 1);
    ref_resources.reserve(dpb_.size() + 1);
    ref_dpb.reserve(dpb_.size() + 1);
    ref_std.reserve(dpb_.size() + 1);
    for (const DpbEntry &entry : dpb_) {
        ensure_dpb_image(entry.slot);
        DpbImage &image = dpb_images_[entry.slot];
        if (image.layout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR) {
            barrier(image.image, image.layout, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, 0, 0,
                    VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                    VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
            image.layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        }
        StdVideoDecodeH264ReferenceInfo std_ref{};
        std_ref.FrameNum = static_cast<uint16_t>(entry.frame_num);
        std_ref.PicOrderCnt[0] = entry.field_poc[0];
        std_ref.PicOrderCnt[1] = entry.field_poc[1];
        // Progressive frame picture: both field flags clear.
        ref_std.push_back(std_ref);
        VkVideoDecodeH264DpbSlotInfoKHR slot_info{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR};
        slot_info.pStdReferenceInfo = &ref_std.back();
        ref_dpb.push_back(slot_info);
        VkVideoPictureResourceInfoKHR resource{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
        resource.codedExtent = {static_cast<uint32_t>(coded_width_), static_cast<uint32_t>(coded_height_)};
        resource.imageViewBinding = image.view;
        ref_resources.push_back(resource);
        VkVideoReferenceSlotInfoKHR slot{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
        slot.pNext = &ref_dpb.back();
        slot.slotIndex = entry.slot;
        slot.pPictureResource = &ref_resources.back();
        ref_slots.push_back(slot);
    }

    DpbImage &setup_image = dpb_images_[setup_slot_];
    if (setup_image.layout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR) {
        barrier(setup_image.image, setup_image.layout, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, 0, 0,
                VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
        setup_image.layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
    }

    const SliceInfo &si = infos.front();
    if (si.nal_type == H264_NAL_IDR_SLICE && !outputs_.empty()) gop_++;
    StdVideoDecodeH264ReferenceInfo setup_std{};
    setup_std.FrameNum = static_cast<uint16_t>(si.frame_num);
    setup_std.PicOrderCnt[0] = si.picture_poc[0];
    setup_std.PicOrderCnt[1] = si.picture_poc[1];
    // Progressive frame picture: both field flags clear.
    VkVideoDecodeH264DpbSlotInfoKHR setup_dpb{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR};
    setup_dpb.pStdReferenceInfo = &setup_std;
    VkVideoPictureResourceInfoKHR setup_resource{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    setup_resource.codedExtent = {static_cast<uint32_t>(coded_width_), static_cast<uint32_t>(coded_height_)};
    setup_resource.imageViewBinding = setup_image.view;
    VkVideoReferenceSlotInfoKHR setup_slot{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
    setup_slot.pNext = &setup_dpb;
    setup_slot.slotIndex = setup_slot_;
    setup_slot.pPictureResource = &setup_resource;

    VkVideoReferenceSlotInfoKHR inactive_setup = setup_slot;
    inactive_setup.slotIndex = -1;
    std::vector<VkVideoReferenceSlotInfoKHR> begin_refs = ref_slots;
    begin_refs.push_back(inactive_setup);
    VkVideoBeginCodingInfoKHR begin{VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin.videoSession = session_;
    begin.videoSessionParameters = parameters_;
    begin.referenceSlotCount = static_cast<uint32_t>(begin_refs.size());
    begin.pReferenceSlots = begin_refs.data();
    BeginVideo_(cmd_, &begin);
    if (query_) vkCmdBeginQuery(cmd_, query_, 0, 0);

    StdVideoDecodeH264PictureInfo picture{};
    picture.flags.is_intra = (si.slice_type_nos == AV_PICTURE_TYPE_I);
    picture.flags.IdrPicFlag = (si.nal_type == H264_NAL_IDR_SLICE);
    picture.flags.is_reference = (si.nal_ref_idc != 0);
    picture.seq_parameter_set_id = static_cast<uint8_t>(sps_->sps_id);
    picture.pic_parameter_set_id = static_cast<uint8_t>(si.pps_id);
    picture.frame_num = static_cast<uint16_t>(si.frame_num);
    picture.idr_pic_id = static_cast<uint16_t>(si.idr_pic_id);
    picture.PicOrderCnt[0] = si.picture_poc[0];
    picture.PicOrderCnt[1] = si.picture_poc[1];

    VkVideoDecodeH264PictureInfoKHR h264_picture{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR};
    h264_picture.pStdPictureInfo = &picture;
    h264_picture.sliceCount = static_cast<uint32_t>(offsets.size());
    h264_picture.pSliceOffsets = offsets.data();

    VkDeviceSize range = (bitstream.size() + bitstream_align_ - 1) / bitstream_align_ * bitstream_align_;
    VkVideoDecodeInfoKHR decode{VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
    decode.pNext = &h264_picture;
    decode.srcBuffer = bitstream_;
    decode.srcBufferRange = range;
    decode.dstPictureResource = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    decode.dstPictureResource.codedExtent = {static_cast<uint32_t>(coded_width_),
                                            static_cast<uint32_t>(coded_height_)};
    decode.dstPictureResource.imageViewBinding = record.view;
    decode.pSetupReferenceSlot = &setup_slot;
    decode.referenceSlotCount = static_cast<uint32_t>(ref_slots.size());
    decode.pReferenceSlots = ref_slots.data();
    DecodeVideo_(cmd_, &decode);
    if (query_) vkCmdEndQuery(cmd_, query_, 0);

    VkVideoEndCodingInfoKHR end{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    EndVideo_(cmd_, &end);
    barrier(record.image, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    CHECK(vkEndCommandBuffer(cmd_));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    CHECK(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue_));

    if (query_) {
        uint32_t status = 0;
        const VkResult query_result = vkGetQueryPoolResults(
            device_, query_, 0, 1, sizeof(status), &status, sizeof(status),
            VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
        require(query_result == VK_SUCCESS &&
                    static_cast<int32_t>(status) == VK_QUERY_RESULT_STATUS_COMPLETE_KHR,
                "decode status " + std::to_string(static_cast<int32_t>(status)) + " result " +
                    std::to_string(static_cast<int>(query_result)));
    }

    record.poc = si.picture_poc[0];
    record.gop = gop_;
    record.layout = VK_IMAGE_LAYOUT_GENERAL;

    VkMemoryGetFdInfoKHR fd_info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    fd_info.memory = record.memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    CHECK(GetMemoryFd_(device_, &fd_info, &record.fd));
    for (unsigned plane = 0; plane < 2; ++plane) {
        VkImageSubresource subresource{plane ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT,
                                       0, 0};
        VkSubresourceLayout layout{};
        vkGetImageSubresourceLayout(device_, record.image, &subresource, &layout);
        record.planes[plane].offset = layout.offset;
        record.planes[plane].row_pitch = layout.rowPitch;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, record.image, &req);
    hipExternalMemoryHandleDesc handle{};
    handle.type = hipExternalMemoryHandleTypeOpaqueFd;
    handle.handle.fd = record.fd;
    handle.size = req.size;
    CHECK(hipImportExternalMemory(&record.hip_memory, &handle));
    hipExternalMemoryBufferDesc buffer{};
    buffer.size = req.size;
    CHECK(hipExternalMemoryGetMappedBuffer(&record.hip_pointer, record.hip_memory, &buffer));
    close(record.fd);
    record.fd = -1;
}

void Decoder::update_dpb(const SliceInfo &si, int setup_slot)
{
    if (si.nal_type == H264_NAL_IDR_SLICE) {
        for (const DpbEntry &entry : dpb_)
            free_slots_.push_back(entry.slot);
        dpb_.clear();
    } else if (si.nal_ref_idc != 0) {
        for (int i = 0; i < si.nb_mmco; ++i) {
            if (si.mmco[i].opcode == MMCO_SHORT2UNUSED) {
                const int frame_num = si.mmco[i].short_pic_num;
                for (auto it = dpb_.begin(); it != dpb_.end(); ++it) {
                    if (it->frame_num == frame_num) {
                        free_slots_.push_back(it->slot);
                        dpb_.erase(it);
                        break;
                    }
                }
            }
        }
        if (!si.explicit_ref_marking && sps_ &&
            static_cast<int>(dpb_.size()) >= sps_->ref_frame_count && !dpb_.empty()) {
            free_slots_.push_back(dpb_.back().slot);
            dpb_.pop_back();
        }
    }

    if (si.nal_ref_idc != 0) {
        DpbEntry entry;
        entry.slot = setup_slot;
        entry.frame_num = si.frame_num;
        entry.field_poc[0] = si.picture_poc[0];
        entry.field_poc[1] = si.picture_poc[1];
        dpb_.insert(dpb_.begin(), entry);
    } else {
        free_slots_.push_back(setup_slot);
    }
}

bool Decoder::has_dst_format() const
{
    VkVideoProfileListInfoKHR list{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    list.profileCount = 1;
    list.pProfiles = const_cast<VkVideoProfileInfoKHR *>(&profile_);
    VkPhysicalDeviceVideoFormatInfoKHR query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR};
    query.pNext = &list;
    query.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
    uint32_t count = 0;
    if (GetVideoFormats_(physical_, &query, &count, nullptr) != VK_SUCCESS || !count) return false;
    std::vector<VkVideoFormatPropertiesKHR> formats(count,
                                                    {VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
    if (GetVideoFormats_(physical_, &query, &count, formats.data()) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (formats[i].format == format_) return true;
    return false;
}

void Decoder::probe()
{
    static const StdVideoH264ProfileIdc profiles[] = {
        STD_VIDEO_H264_PROFILE_IDC_BASELINE,
        STD_VIDEO_H264_PROFILE_IDC_MAIN,
        STD_VIDEO_H264_PROFILE_IDC_HIGH,
        static_cast<StdVideoH264ProfileIdc>(110),   // High 10
        static_cast<StdVideoH264ProfileIdc>(122),   // High 4:2:2
        STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE,
    };
    static const VkVideoChromaSubsamplingFlagBitsKHR chromas[] = {
        VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
        VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR,
        VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR,
    };
    static const char *chroma_names[] = {"4:2:0", "4:2:2", "4:4:4"};
    std::printf("probe: profile chroma depth result maxDpb activeRefs extent format\n");
    for (StdVideoH264ProfileIdc idc : profiles) {
        for (int c = 0; c < 3; ++c) {
            for (int depth : {8, 10}) {
                VkVideoDecodeH264ProfileInfoKHR h264{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
                h264.stdProfileIdc = idc;
                h264.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
                VkVideoProfileInfoKHR profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
                profile.pNext = &h264;
                profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
                profile.chromaSubsampling = chromas[c];
                profile.lumaBitDepth =
                    depth == 8 ? VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR : VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
                profile.chromaBitDepth = profile.lumaBitDepth;
                VkVideoDecodeH264CapabilitiesKHR h264_caps{
                    VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
                VkVideoDecodeCapabilitiesKHR decode_caps{VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR};
                decode_caps.pNext = &h264_caps;
                VkVideoCapabilitiesKHR caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
                caps.pNext = &decode_caps;
                const VkResult result = GetVideoCapabilities_(physical_, &profile, &caps);
                if (result != VK_SUCCESS) {
                    std::printf("probe: %-9s %s %2d-bit %s (%d)\n", profile_name(idc),
                                chroma_names[c], depth, vk_result_name(result).c_str(),
                                static_cast<int>(result));
                    continue;
                }
                const VkFormat want = output_format(c == 0 ? 1 : c == 1 ? 2 : 3, depth);
                VkVideoProfileListInfoKHR list{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
                list.profileCount = 1;
                list.pProfiles = &profile;
                VkPhysicalDeviceVideoFormatInfoKHR query{
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR};
                query.pNext = &list;
                query.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
                uint32_t count = 0;
                std::vector<VkVideoFormatPropertiesKHR> formats;
                bool match = false;
                if (GetVideoFormats_(physical_, &query, &count, nullptr) == VK_SUCCESS && count) {
                    formats.assign(count, {VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
                    if (GetVideoFormats_(physical_, &query, &count, formats.data()) == VK_SUCCESS)
                        for (uint32_t i = 0; i < count; ++i) match |= formats[i].format == want;
                }
                std::printf("probe: %-9s %s %2d-bit %s dpb=%u refs=%u extent=%ux%u..%ux%u dstFormats=",
                            profile_name(idc), chroma_names[c], depth,
                            match ? "OK" : "NO-DST-FORMAT", caps.maxDpbSlots,
                            caps.maxActiveReferencePictures, caps.minCodedExtent.width,
                            caps.minCodedExtent.height, caps.maxCodedExtent.width,
                            caps.maxCodedExtent.height);
                for (size_t i = 0; i < formats.size() && i < 4; ++i)
                    std::printf("%d(t%d) ", static_cast<int>(formats[i].format),
                                static_cast<int>(formats[i].imageTiling));
                std::printf("\n");
            }
        }
    }
}

void Decoder::reset()
{
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (OutputFrame &frame : outputs_) {
            if (frame.hip_memory) (void)hipDestroyExternalMemory(frame.hip_memory);
            if (frame.fd >= 0) close(frame.fd);
            if (frame.view) vkDestroyImageView(device_, frame.view, nullptr);
            if (frame.image) vkDestroyImage(device_, frame.image, nullptr);
            if (frame.memory) vkFreeMemory(device_, frame.memory, nullptr);
        }
        outputs_.clear();
        for (DpbImage &image : dpb_images_) {
            if (image.view) vkDestroyImageView(device_, image.view, nullptr);
            if (image.image) vkDestroyImage(device_, image.image, nullptr);
            if (image.memory) vkFreeMemory(device_, image.memory, nullptr);
        }
        dpb_images_.clear();
        if (query_) vkDestroyQueryPool(device_, query_, nullptr);
        if (bitstream_) vkDestroyBuffer(device_, bitstream_, nullptr);
        if (bitstream_memory_) vkFreeMemory(device_, bitstream_memory_, nullptr);
        if (parameters_) DestroyVideoParameters_(device_, parameters_, nullptr);
        if (session_) DestroyVideoSession_(device_, session_, nullptr);
        for (VkDeviceMemory memory : session_memory_)
            if (memory) vkFreeMemory(device_, memory, nullptr);
        session_memory_.clear();
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    cmd_ = VK_NULL_HANDLE;
    query_ = VK_NULL_HANDLE;
    session_ = VK_NULL_HANDLE;
    parameters_ = VK_NULL_HANDLE;
    bitstream_ = VK_NULL_HANDLE;
    bitstream_memory_ = VK_NULL_HANDLE;
    outputs_.clear();
    dpb_.clear();
    dpb_images_.clear();
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        Decoder decoder;
        if (argc == 2 && !std::strcmp(argv[1], "--probe")) {
            decoder.init();
            decoder.probe();
            return 0;
        }
        require(argc == 4, "usage: decode <stream.h264> <reference.raw> <ref-format> | decode --probe");

        const std::vector<uint8_t> stream = read_file(argv[1]);
        RefFormat ref_format;
        require(parse_ref_format(argv[3], &ref_format), std::string("Unknown reference format ") + argv[3]);

        decoder.init();

        H264ParamSets param_sets;
        std::memset(&param_sets, 0, sizeof(param_sets));
        AVCodecContext avctx;
        std::memset(&avctx, 0, sizeof(avctx));
        avctx.codec_id = AV_CODEC_ID_H264;

        H2645Packet packet;
        std::memset(&packet, 0, sizeof(packet));
        if (ff_h2645_packet_split(&packet, stream.data(), static_cast<int>(stream.size()),
                                  nullptr, 0, AV_CODEC_ID_H264, 0) < 0)
            throw std::runtime_error("Cannot split the Annex-B stream");

        std::vector<const H2645NAL *> au_slices;
        std::vector<SliceInfo> au_infos;
        int decoded = 0;
        bool session_ready = false;
        const SPS *sps = nullptr;
        const PPS *pps = nullptr;
        H264POCContext poc_ctx;
        std::memset(&poc_ctx, 0, sizeof(poc_ctx));
        poc_ctx.prev_poc_lsb = -1;
        const int fetched = [&]() {
            auto flush = [&]() -> int {
                if (au_slices.empty()) return 0;
                SliceInfo &first = au_infos.front();
                // Picture order count: the driver needs it for DPB management,
                // and the demo needs it to compare in display order. An IDR
                // picture resets the POC state and has PicOrderCnt 0 (8.2.1),
                // so clear the context before computing its POC.
                if (first.nal_type == H264_NAL_IDR_SLICE) {
                    poc_ctx.prev_frame_num = 0;
                    poc_ctx.prev_frame_num_offset = 0;
                    poc_ctx.prev_poc_msb = 0;
                    poc_ctx.prev_poc_lsb = -1;
                }
                poc_ctx.frame_num = first.frame_num;
                poc_ctx.poc_lsb = first.poc_lsb;
                poc_ctx.delta_poc_bottom = first.delta_poc_bottom;
                poc_ctx.delta_poc[0] = first.delta_poc[0];
                poc_ctx.delta_poc[1] = first.delta_poc[1];
                int field_poc[2] = {0, 0};
                int pic_poc = 0;
                if (ff_h264_init_poc(field_poc, &pic_poc, sps, &poc_ctx, PICT_FRAME,
                                     first.nal_ref_idc) < 0)
                    throw std::runtime_error("POC computation failed");
                first.picture_poc[0] = field_poc[0];
                first.picture_poc[1] = field_poc[1];
                if (first.nal_ref_idc) {
                    poc_ctx.prev_poc_msb = poc_ctx.poc_msb;
                    poc_ctx.prev_poc_lsb = poc_ctx.poc_lsb;
                }
                poc_ctx.prev_frame_num = poc_ctx.frame_num;
                poc_ctx.prev_frame_num_offset = poc_ctx.frame_num_offset;
                if (first.nal_type == H264_NAL_IDR_SLICE) {
                    poc_ctx.prev_poc_msb = 0;
                    poc_ctx.prev_poc_lsb = -1;
                    poc_ctx.prev_frame_num = 0;
                    poc_ctx.prev_frame_num_offset = 0;
                }
                if (std::getenv("DEMO_DEBUG"))
                    std::fprintf(stderr, "[au] idx=%d type=%d fn=%d lsb=%d poc0=%d poc1=%d ref=%d refs=%zu\n",
                                 decoded, first.slice_type_nos, first.frame_num, first.poc_lsb,
                                 field_poc[0], field_poc[1], first.nal_ref_idc, au_infos.size());
                decoder.decode_access_unit(au_slices, au_infos);
                decoder.update_dpb(first, decoder.setup_slot());
                decoded++;
                au_slices.clear();
                au_infos.clear();
                return 0;
            };

            for (int i = 0; i < packet.nb_nals; ++i) {
                H2645NAL *nal = &packet.nals[i];
                if (nal->type == H264_NAL_SPS) {
                    if (int rc = flush()) return rc;
                    if (ff_h264_decode_seq_parameter_set(&nal->gb, &avctx, &param_sets, 0) < 0)
                        throw std::runtime_error("SPS parse failed");
                    continue;
                }
                if (nal->type == H264_NAL_PPS) {
                    if (int rc = flush()) return rc;
                    if (ff_h264_decode_picture_parameter_set(&nal->gb, &avctx, &param_sets,
                                                             nal->size_bits) < 0)
                        throw std::runtime_error("PPS parse failed");
                    continue;
                }
                if (nal->type != H264_NAL_SLICE && nal->type != H264_NAL_IDR_SLICE) {
                    // SEI/AUD and anything else cannot appear inside an access
                    // unit after its slices; flush if one is open.
                    if (int rc = flush()) return rc;
                    continue;
                }

                // Peek pps_id to find the parameter sets this slice refers to.
                GetBitContext peek = nal->gb;
                (void)get_ue_golomb_long(&peek);
                (void)get_ue_golomb_31(&peek);
                const int pps_id = static_cast<int>(get_ue_golomb(&peek));
                if (pps_id < 0 || pps_id >= MAX_PPS_COUNT || !param_sets.pps_list[pps_id])
                    throw std::runtime_error("slice refers to an unknown PPS " + std::to_string(pps_id));
                const PPS *slice_pps = param_sets.pps_list[pps_id];
                if (slice_pps->sps_id < 0 || slice_pps->sps_id >= MAX_SPS_COUNT ||
                    !param_sets.sps_list[slice_pps->sps_id])
                    throw std::runtime_error("PPS refers to an unknown SPS");
                const SPS *slice_sps = param_sets.sps_list[slice_pps->sps_id];

                SliceInfo info;
                if (!parse_slice_header(nal, slice_sps, slice_pps, &info))
                    throw std::runtime_error("unsupported slice header (SP/SI, interlaced or MMCO)");

                if (!au_slices.empty() && info.first_mb_in_slice == 0) {
                    if (int rc = flush()) return rc;
                }
                if (au_slices.empty()) {
                    sps = slice_sps;
                    pps = slice_pps;
                    if (!session_ready) {
                        const int rc = decoder.set_up(sps, pps);
                        if (rc != 0) return rc;
                        session_ready = true;
                    } else if (sps->profile_idc != slice_sps->profile_idc ||
                               sps->mb_width != slice_sps->mb_width ||
                               sps->mb_height != slice_sps->mb_height ||
                               sps->chroma_format_idc != slice_sps->chroma_format_idc ||
                               sps->bit_depth_luma != slice_sps->bit_depth_luma) {
                        throw std::runtime_error("mid-stream SPS change is not supported by this probe");
                    }
                }
                au_slices.push_back(nal);
                au_infos.push_back(info);
            }
            return flush();
        }();
        if (fetched != 0) return fetched;

        if (decoded == 0) throw std::runtime_error("No pictures decoded");

        const int width = decoder.display_width();
        const int height = decoder.display_height();
        require(ref_format.chroma == decoder.chroma() && ref_format.depth == decoder.bit_depth(),
                "reference format does not match the stream (stream is " +
                    std::string(chroma_name(decoder.chroma())) + " " +
                    std::to_string(decoder.bit_depth()) + "-bit)");
        std::vector<RefFrame> refs = load_reference(argv[2], ref_format, width, height, decoded);

        std::vector<size_t> order(decoder.frames().size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const OutputFrame &fa = decoder.frames()[a];
            const OutputFrame &fb = decoder.frames()[b];
            if (fa.gop != fb.gop) return fa.gop < fb.gop;
            return fa.poc < fb.poc;
        });

        const int cw = width / sub_width(decoder.chroma());
        const int ch = height / sub_height(decoder.chroma());
        const unsigned bytes = decoder.bit_depth() > 8 ? 2 : 1;
        const size_t luma_samples = static_cast<size_t>(width) * height;
        const size_t chroma_samples = static_cast<size_t>(cw) * ch;

        uint16_t *dev_y = nullptr;
        uint16_t *dev_u = nullptr;
        uint16_t *dev_v = nullptr;
        unsigned *dev_errors = nullptr;
        CHECK(hipMalloc(&dev_y, luma_samples * sizeof(uint16_t)));
        CHECK(hipMalloc(&dev_u, chroma_samples * sizeof(uint16_t)));
        CHECK(hipMalloc(&dev_v, chroma_samples * sizeof(uint16_t)));
        CHECK(hipMalloc(&dev_errors, sizeof(unsigned) * 2));

        unsigned max_y = 0;
        unsigned max_c = 0;
        bool passed = true;
        for (size_t display = 0; display < order.size(); ++display) {
            const OutputFrame &frame = decoder.frames()[order[display]];
            const RefFrame &ref = refs[display];
            CHECK(hipMemcpy(dev_y, ref.y.data(), luma_samples * sizeof(uint16_t), hipMemcpyHostToDevice));
            CHECK(hipMemcpy(dev_u, ref.u.data(), chroma_samples * sizeof(uint16_t), hipMemcpyHostToDevice));
            CHECK(hipMemcpy(dev_v, ref.v.data(), chroma_samples * sizeof(uint16_t), hipMemcpyHostToDevice));
            unsigned zeros[2] = {0, 0};
            CHECK(hipMemset(dev_errors, 0, sizeof(zeros)));
            const int total = static_cast<int>(luma_samples + chroma_samples);
            const int threads = 256;
            compare_planes<<<(total + threads - 1) / threads, threads>>>(
                static_cast<const uint8_t *>(frame.hip_pointer) + frame.planes[0].offset,
                frame.planes[0].row_pitch, bytes,
                static_cast<const uint8_t *>(frame.hip_pointer) + frame.planes[1].offset,
                frame.planes[1].row_pitch, bytes, dev_y, dev_u, dev_v, width, height, cw, ch,
                decoder.bit_depth(), decoder.crop_top(), decoder.crop_left(),
                decoder.crop_top() / sub_height(decoder.chroma()),
                decoder.crop_left() / sub_width(decoder.chroma()), dev_errors);
            CHECK(hipGetLastError());
            unsigned errors[2] = {0, 0};
            CHECK(hipMemcpy(errors, dev_errors, sizeof(errors), hipMemcpyDeviceToHost));
            max_y = std::max(max_y, errors[0]);
            max_c = std::max(max_c, errors[1]);
            if (errors[0] || errors[1]) {
                passed = false;
                std::printf("frame=%zu display=%zu poc=%d max_y_error=%u max_c_error=%u\n",
                            order[display], display, frame.poc, errors[0], errors[1]);
            }
        }
        CHECK(hipFree(dev_y));
        CHECK(hipFree(dev_u));
        CHECK(hipFree(dev_v));
        CHECK(hipFree(dev_errors));

        require(passed, "HIP readback differs from software decode");
        std::printf("PASS Vulkan linear decode -> HIP frames=%d %dx%d %s %d-bit "
                    "max_y_error=%u max_c_error=%u\n",
                    decoded, width, height, chroma_name(decoder.chroma()), decoder.bit_depth(),
                    max_y, max_c);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
