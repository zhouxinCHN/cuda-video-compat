// verify_vk_decode_to_hip.cpp
// P0：Vulkan Video 解码 → 拷贝(可导出 buffer) → HIP 导入读回，逐字节校验
//
// 对标 tests/zerocopy/verify_decode_to_hip.cpp（VAAPI 版实验 C），把 VAAPI 段换成 Vulkan Video。
// 目标数据通路（TODO §2）：
//   Vulkan 解码输出 image（OPTIMAL tiling NV12）
//     → barrier 转 TRANSFER_SRC_OPTIMAL
//     → vkCmdCopyImageToBuffer 逐平面拷到可导出 buffer
//     → vkGetMemoryFdKHR 导出 dma-buf fd
//     → hipImportExternalMemory + hipExternalMemoryGetMappedBuffer
//     → HIP 读回逐字节 vs 软解参考
//
// 关键实现结论（已对照 RADV radv_video.c / FFmpeg h264_vulkan.c 核实）：
//   1. 双队列：decode 队列族(VIDEO_DECODE) 无 TRANSFER 能力，copy 必须在 transfer 队列族跑；
//      image/buffer 用 CONCURRENT 共享 + semaphore 桥接（sync2）。
//   2. DPB：驱动内部按 frame_num/POC 排 ref list，应用**不需要**手写 RefPicList0/1 重排；
//      但应用仍需维护 active 参考集合（MMCO + 滑动窗口 + IDR 清空），用来填 pReferenceSlots。
//   3. pSetupReferenceSlot 每帧都非 NULL（含 B 帧）——RADV get_h264_msg 对 H264 恒断言非空
//      （radv_video.c:2678）；非参考帧用临时 slot，解码后即回收。
//   4. 同步：decode(signal sem) → transfer(wait sem, signal fence) → host(wait fence) → 导出/HIP 读；
//      下一帧 decode wait copyDoneSem 解决 output 图像复用的 WAR。
//
// 范围：H.264 I/P/B、NV12、8bit、4:2:0、frame_mbs_only=1、poc_type=0/2、Baseline/Main/High。
//
// 运行（AMD 验证机 RX 7900 XTX）：
//   make -f verify_vk_decode_to_hip.make
//   make -f verify_vk_decode_to_hip.make run-b    # I/P/B 30 帧（P0 核心验收）

#include <vulkan/vulkan.h>

#include <hip/hip_runtime_api.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <algorithm>

// FFmpeg parser 复用（仅编译期头，链接期取 9 个 .o + libavutil.a）
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

#define PASS(...) do { printf("  [PASS] "); printf(__VA_ARGS__); printf("\n"); g_pass++; } while (0)
#define FAIL(...) do { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); g_fail++; } while (0)
#define INFO(...) do { printf("  [info] "); printf(__VA_ARGS__); printf("\n"); } while (0)

static int g_pass = 0, g_fail = 0;
static int W = 320, H = 240;   // 从 SPS 覆盖

// ==================== 工具函数（后端无关，同 VAAPI 版） ====================

static bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) { FAIL("fopen %s: %s", path, strerror(errno)); return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { FAIL("空文件 %s", path); fclose(f); return false; }
    out.resize((size_t)n);
    size_t rd = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return rd == (size_t)n;
}

static void sps_dimensions(const SPS* sps, int* w, int* h) {
    *w = 16 * sps->mb_width  - (int)(sps->crop_right + sps->crop_left);
    *h = 16 * sps->mb_height - (int)(sps->crop_top + sps->crop_bottom);
}

static int count_fds() {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0; while (readdir(d)) n++;
    closedir(d);
    return n - 2;
}

static const char* vkResultStr(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
        case VK_ERROR_TOO_MANY_OBJECTS: return "TOO_MANY_OBJECTS";
        case VK_ERROR_DEVICE_LOST: return "DEVICE_LOST";
        default: return "UNKNOWN";
    }
}

// ==================== slice header 解析（后端无关，同 VAAPI 版） ====================

struct MMCOOp {
    int opcode;
    int short_pic_num;
    int long_arg;
};

struct SliceInfo {
    int nal_type;
    int nal_ref_idc;
    int slice_type_nos;   // AV_PICTURE_TYPE_I/P/B
    int frame_num;
    int pps_id;
    int field_pic_flag;
    int bottom_field_flag;
    int idr_pic_id;
    int poc_lsb;
    int delta_poc_bottom;
    int delta_poc[2];
    int direct_spatial_mv_pred;
    int ref_count[2];
    int list_count;
    int nb_ref_modifications[2];
    struct { int op; int val; } ref_modifications[2][32];
    int nb_mmco;
    MMCOOp mmco[32];
    int explicit_ref_marking;
    int slice_qp_delta;
    uint8_t disable_deblocking_idc;
    int8_t alpha_div2, beta_div2;
    int cabac_init_idc;
    int bit_offset;
};

static bool parse_slice_header(const H2645NAL* nal, const SPS* sps, const PPS* pps,
                               SliceInfo* si) {
    memset(si, 0, sizeof(*si));
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
    if (si->slice_type_nos == AV_PICTURE_TYPE_SP || si->slice_type_nos == AV_PICTURE_TYPE_SI) {
        FAIL("不支持 SP/SI slice"); return false;
    }

    si->pps_id = get_ue_golomb(&gb);
    si->frame_num = get_bits(&gb, sps->log2_max_frame_num);

    if (!sps->frame_mbs_only_flag) { FAIL("不支持 frame_mbs_only_flag=0（场编码）"); return false; }

    if (nal->type == H264_NAL_IDR_SLICE)
        si->idr_pic_id = get_ue_golomb_long(&gb);

    if (sps->poc_type == 0) {
        si->poc_lsb = get_bits(&gb, sps->log2_max_poc_lsb);
        if (pps->pic_order_present == 1)
            si->delta_poc_bottom = get_se_golomb(&gb);
    } else if (sps->poc_type != 2) {
        FAIL("仅支持 poc_type=0/2（当前 poc_type=%d）", sps->poc_type);
        return false;
    }

    if (pps->redundant_pic_cnt_present)
        get_ue_golomb(&gb);

    if (si->slice_type_nos == AV_PICTURE_TYPE_B)
        si->direct_spatial_mv_pred = get_bits1(&gb);

    if (si->slice_type_nos != AV_PICTURE_TYPE_I) {
        int ret = ff_h264_parse_ref_count(&si->list_count, si->ref_count,
                                          &gb, pps, si->slice_type_nos, PICT_FRAME, nullptr);
        if (ret < 0) { FAIL("ff_h264_parse_ref_count 失败"); return false; }
        for (int list = 0; list < si->list_count; list++) {
            if (!get_bits1(&gb)) continue;
            for (int idx = 0; ; idx++) {
                unsigned op = get_ue_golomb_31(&gb);
                if (op == 3) break;
                if (idx >= si->ref_count[list]) return false;
                if (op > 2) return false;
                si->ref_modifications[list][idx].op = op;
                si->ref_modifications[list][idx].val = get_ue_golomb_long(&gb);
                si->nb_ref_modifications[list]++;
            }
        }
        if ((pps->weighted_pred && si->slice_type_nos == AV_PICTURE_TYPE_P) ||
            (pps->weighted_bipred_idc == 1 && si->slice_type_nos == AV_PICTURE_TYPE_B)) {
            FAIL("不支持加权预测"); return false;
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
                        FAIL("仅支持 MMCO_SHORT2UNUSED（opcode=%u）", op);
                        return false;
                    }
                    si->nb_mmco = i + 1;
                }
            }
        }
    }

    if (si->slice_type_nos != AV_PICTURE_TYPE_I && pps->cabac) {
        unsigned tmp = get_ue_golomb_31(&gb);
        if (tmp > 2) return false;
        si->cabac_init_idc = tmp;
    }

    si->slice_qp_delta = get_se_golomb(&gb);

    if (pps->deblocking_filter_parameters_present) {
        unsigned idc = get_ue_golomb_31(&gb);
        if (idc > 2) return false;
        si->disable_deblocking_idc = (uint8_t)idc;
        if (idc != 1) { si->alpha_div2 = (int8_t)get_se_golomb(&gb); si->beta_div2 = (int8_t)get_se_golomb(&gb); }
    }

    si->bit_offset = get_bits_count(&gb);
    return true;
}

// ==================== FFmpeg SPS/PPS → Std 转换（仿 FFmpeg h264_vulkan.c set_sps/set_pps） ====================

static StdVideoH264LevelIdc convert_to_vk_level_idc(int level_idc) {
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

// 8x8 列表顺序（FFmpeg h264_vulkan.c h264_scaling_list8_order）
static const int kScalingList8Order[6] = { 0, 3, 1, 4, 2, 5 };

static void fill_sps_std(const SPS* sps, StdVideoH264ScalingLists* sl,
                         StdVideoH264SequenceParameterSetVui* vui,
                         StdVideoH264SequenceParameterSet* out) {
    memset(sl, 0, sizeof(*sl));
    memset(vui, 0, sizeof(*vui));
    memset(out, 0, sizeof(*out));

    sl->scaling_list_present_mask = sps->scaling_matrix_present_mask;
    sl->use_default_scaling_matrix_mask = 0;
    for (int i = 0; i < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_LISTS; i++)
        for (int j = 0; j < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_ELEMENTS; j++)
            sl->ScalingList4x4[i][j] = sps->scaling_matrix4[i][ff_zigzag_scan[j]];
    for (int i = 0; i < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_LISTS; i++)
        for (int j = 0; j < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_ELEMENTS; j++)
            sl->ScalingList8x8[i][j] = sps->scaling_matrix8[kScalingList8Order[i]][ff_zigzag_direct[j]];

    *vui = (StdVideoH264SequenceParameterSetVui) {
        .flags = (StdVideoH264SpsVuiFlags) {
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
        .aspect_ratio_idc = (StdVideoH264AspectRatioIdc)sps->vui.aspect_ratio_idc,
        .sar_width  = (uint16_t)sps->vui.sar.num,
        .sar_height = (uint16_t)sps->vui.sar.den,
        .video_format = (uint8_t)sps->vui.video_format,
        .colour_primaries = (uint8_t)sps->vui.colour_primaries,
        .transfer_characteristics = (uint8_t)sps->vui.transfer_characteristics,
        .matrix_coefficients = (uint8_t)sps->vui.matrix_coeffs,
        .num_units_in_tick = sps->num_units_in_tick,
        .time_scale = sps->time_scale,
        .max_num_reorder_frames = (uint8_t)sps->num_reorder_frames,
        .max_dec_frame_buffering = (uint8_t)sps->max_dec_frame_buffering,
        .chroma_sample_loc_type_top_field = (uint8_t)sps->vui.chroma_sample_loc_type_top_field,
        .chroma_sample_loc_type_bottom_field = (uint8_t)sps->vui.chroma_sample_loc_type_bottom_field,
    };

    *out = (StdVideoH264SequenceParameterSet) {
        .flags = (StdVideoH264SpsFlags) {
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
        .profile_idc = (StdVideoH264ProfileIdc)sps->profile_idc,
        .level_idc = convert_to_vk_level_idc(sps->level_idc),
        .chroma_format_idc = (StdVideoH264ChromaFormatIdc)sps->chroma_format_idc,
        .seq_parameter_set_id = (uint8_t)sps->sps_id,
        .bit_depth_luma_minus8 = (uint8_t)(sps->bit_depth_luma - 8),
        .bit_depth_chroma_minus8 = (uint8_t)(sps->bit_depth_chroma - 8),
        .log2_max_frame_num_minus4 = (uint8_t)(sps->log2_max_frame_num - 4),
        .pic_order_cnt_type = (StdVideoH264PocType)sps->poc_type,
        .offset_for_non_ref_pic = sps->offset_for_non_ref_pic,
        .offset_for_top_to_bottom_field = sps->offset_for_top_to_bottom_field,
        .log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)(sps->poc_type ? 0 : sps->log2_max_poc_lsb - 4),
        .num_ref_frames_in_pic_order_cnt_cycle = (uint8_t)sps->poc_cycle_length,
        .max_num_ref_frames = (uint8_t)sps->ref_frame_count,
        .pic_width_in_mbs_minus1 = (uint32_t)(sps->mb_width - 1),
        .pic_height_in_map_units_minus1 = (uint32_t)(sps->mb_height / (2 - sps->frame_mbs_only_flag) - 1),
        .frame_crop_left_offset = sps->crop_left,
        .frame_crop_right_offset = sps->crop_right,
        .frame_crop_top_offset = sps->crop_top,
        .frame_crop_bottom_offset = sps->crop_bottom,
        .pOffsetForRefFrame = sps->offset_for_ref_frame,
        .pScalingLists = sl,
        .pSequenceParameterSetVui = vui,
    };
}

static void fill_pps_std(const PPS* pps, const SPS* sps,
                         StdVideoH264ScalingLists* sl, StdVideoH264PictureParameterSet* out) {
    (void)sps;
    memset(sl, 0, sizeof(*sl));
    memset(out, 0, sizeof(*out));

    sl->scaling_list_present_mask = pps->pic_scaling_matrix_present_mask;
    sl->use_default_scaling_matrix_mask = 0;
    for (int i = 0; i < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_LISTS; i++)
        for (int j = 0; j < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_ELEMENTS; j++)
            sl->ScalingList4x4[i][j] = pps->scaling_matrix4[i][ff_zigzag_scan[j]];
    for (int i = 0; i < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_LISTS; i++)
        for (int j = 0; j < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_ELEMENTS; j++)
            sl->ScalingList8x8[i][j] = pps->scaling_matrix8[kScalingList8Order[i]][ff_zigzag_direct[j]];

    *out = (StdVideoH264PictureParameterSet) {
        .flags = (StdVideoH264PpsFlags) {
            .transform_8x8_mode_flag = pps->transform_8x8_mode,
            .redundant_pic_cnt_present_flag = pps->redundant_pic_cnt_present,
            .constrained_intra_pred_flag = pps->constrained_intra_pred,
            .deblocking_filter_control_present_flag = pps->deblocking_filter_parameters_present,
            .weighted_pred_flag = pps->weighted_pred,
            .bottom_field_pic_order_in_frame_present_flag = pps->pic_order_present,
            .entropy_coding_mode_flag = pps->cabac,
            .pic_scaling_matrix_present_flag = pps->pic_scaling_matrix_present_flag,
        },
        .seq_parameter_set_id = (uint8_t)pps->sps_id,
        .pic_parameter_set_id = (uint8_t)pps->pps_id,
        .num_ref_idx_l0_default_active_minus1 = (uint8_t)(pps->ref_count[0] - 1),
        .num_ref_idx_l1_default_active_minus1 = (uint8_t)(pps->ref_count[1] - 1),
        .weighted_bipred_idc = (StdVideoH264WeightedBipredIdc)pps->weighted_bipred_idc,
        .pic_init_qp_minus26 = (int8_t)(pps->init_qp - 26),
        .pic_init_qs_minus26 = (int8_t)(pps->init_qs - 26),
        .chroma_qp_index_offset = (int8_t)pps->chroma_qp_index_offset[0],
        .second_chroma_qp_index_offset = (int8_t)pps->chroma_qp_index_offset[1],
        .pScalingLists = sl,
    };
}

// ==================== Vulkan 动态加载的命令 ====================

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

static bool load_video_pfns(VkDevice device) {
    #define LOAD(nm) p##nm = (PFN_##nm)vkGetDeviceProcAddr(device, #nm); \
        if (!p##nm) { FAIL("加载 " #nm " 失败"); return false; }
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

// ==================== 结构体：VK 资源 ====================

struct ImageRes {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct DpbEntry {
    int slot;
    int frame_num;
    int poc;
    int field_poc[2];
};

struct DecodedFrame {
    int poc;
    int frame_num;
    int slice_type;   // AV_PICTURE_TYPE_I/P/B
    std::vector<uint8_t> y, uv;
};

// ==================== main ====================

int main(int argc, char** argv) {
    printf("===========================================================\n");
    printf(" P0：Vulkan Video 解码 → 拷贝(可导出) → HIP 读，逐字节校验\n");
    printf("===========================================================\n");

    if (argc < 3) { fprintf(stderr, "用法: %s <input.h264> <ref.yuv>\n", argv[0]); return 2; }

    std::vector<uint8_t> buf;
    if (!read_file(argv[1], buf)) return 1;

    // ---- 1. 复用 FFmpeg parser 切 NAL + 解析 SPS/PPS ----
    H2645Packet pkt; memset(&pkt, 0, sizeof(pkt));
    int ret = ff_h2645_packet_split(&pkt, buf.data(), (int)buf.size(),
                                    nullptr, 0, (enum AVCodecID)AV_CODEC_ID_H264, 0);
    if (ret < 0) { FAIL("ff_h2645_packet_split: %d", ret); return 1; }

    AVCodecContext avctx; memset(&avctx, 0, sizeof(avctx));
    H264ParamSets ps; memset(&ps, 0, sizeof(ps));
    for (int i = 0; i < pkt.nb_nals; i++) {
        H2645NAL& nal = pkt.nals[i];
        if (nal.type == H264_NAL_SPS)
            ff_h264_decode_seq_parameter_set(&nal.gb, &avctx, &ps, 0);
        else if (nal.type == H264_NAL_PPS)
            ff_h264_decode_picture_parameter_set(&nal.gb, &avctx, &ps, nal.size_bits);
    }
    const SPS* sps = nullptr; const PPS* pps = nullptr;
    for (int i = 0; i < MAX_SPS_COUNT && !sps; i++) if (ps.sps_list[i]) sps = ps.sps_list[i];
    for (int i = 0; i < MAX_PPS_COUNT && !pps; i++) if (ps.pps_list[i]) pps = ps.pps_list[i];
    if (!sps || !pps) { FAIL("未取到 SPS/PPS"); ff_h2645_packet_uninit(&pkt); return 1; }
    PASS("解析 SPS/PPS 成功");

    int disp_w, disp_h;
    sps_dimensions(sps, &disp_w, &disp_h);
    W = disp_w; H = disp_h;
    INFO("SPS: profile=%d level=%d %dx%d cabac=%d poc_type=%d ref_frames=%d",
         sps->profile_idc, sps->level_idc, disp_w, disp_h,
         pps->cabac, sps->poc_type, sps->ref_frame_count);

    // ---- 2. 收集 slice NAL（解码顺序）----
    std::vector<H2645NAL*> slices;
    for (int i = 0; i < pkt.nb_nals; i++)
        if (pkt.nals[i].type == H264_NAL_IDR_SLICE || pkt.nals[i].type == H264_NAL_SLICE)
            slices.push_back(&pkt.nals[i]);
    if (slices.empty()) { FAIL("码流无 slice"); ff_h2645_packet_uninit(&pkt); return 1; }
    int n_frames = (int)slices.size();
    INFO("检测到 %d 个 slice", n_frames);

    // ---- 3. 读软解参考 ----
    std::vector<uint8_t> ref;
    {
        FILE* rf = fopen(argv[2], "rb");
        if (!rf) { FAIL("无法打开参考文件 %s", argv[2]); return 1; }
        fseek(rf, 0, SEEK_END); long rn = ftell(rf); fseek(rf, 0, SEEK_SET);
        ref.resize((size_t)rn);
        fread(ref.data(), 1, (size_t)rn, rf);
        fclose(rf);
    }
    size_t frame_bytes = (size_t)W * H * 3 / 2;
    if (ref.size() < frame_bytes * (size_t)n_frames) {
        FAIL("参考文件不足 %d 帧（%zu bytes，需 %zu）", n_frames, ref.size(), frame_bytes * (size_t)n_frames);
        return 1;
    }
    PASS("参考文件含 %zu 帧 NV12", ref.size() / frame_bytes);

    // ---- 4. Vulkan instance ----
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "verify_vk_decode_to_hip";
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> instExts = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
    };
    VkInstanceCreateInfo vici = {};
    vici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    vici.pApplicationInfo = &appInfo;
    vici.enabledExtensionCount = (uint32_t)instExts.size();
    vici.ppEnabledExtensionNames = instExts.data();

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&vici, nullptr, &inst) != VK_SUCCESS) {
        FAIL("vkCreateInstance 失败"); return 1;
    }

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(inst, &pdCount, nullptr);
    if (pdCount == 0) { FAIL("没有 Vulkan physical device"); vkDestroyInstance(inst, nullptr); return 1; }
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(inst, &pdCount, pds.data());
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceProperties pdp;
    vkGetPhysicalDeviceProperties(pd, &pdp);
    INFO("Vulkan 设备 = %s (Vulkan %d.%d.%d)", pdp.deviceName,
         VK_VERSION_MAJOR(pdp.apiVersion), VK_VERSION_MINOR(pdp.apiVersion), VK_VERSION_PATCH(pdp.apiVersion));
    INFO("optimalBufferCopyRowPitchAlignment = %llu",
         (unsigned long long)pdp.limits.optimalBufferCopyRowPitchAlignment);

    // 找 decode / transfer 队列族
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfps(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfps.data());
    int decode_qf = -1, transfer_qf = -1;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (decode_qf < 0 && (qfps[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)) decode_qf = i;
        if (transfer_qf < 0 && (qfps[i].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT)) &&
            (int)i != decode_qf) transfer_qf = i;
    }
    if (decode_qf < 0) { FAIL("无 VIDEO_DECODE 队列族"); vkDestroyInstance(inst, nullptr); return 1; }
    if (transfer_qf < 0) { transfer_qf = 0; }  // 兜底：用 family 0（通常 GRAPHICS|TRANSFER）
    INFO("decode_qf=%d transfer_qf=%d", decode_qf, transfer_qf);

    // device 扩展
    std::vector<const char*> devExts = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
    };

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qcis[2];
    qcis[0] = {}; qcis[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qcis[0].queueFamilyIndex = (uint32_t)decode_qf; qcis[0].queueCount = 1; qcis[0].pQueuePriorities = &qprio;
    qcis[1] = qcis[0];
    qcis[1].queueFamilyIndex = (uint32_t)transfer_qf;

    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 2;
    dci.pQueueCreateInfos = qcis;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(pd, &dci, nullptr, &device) != VK_SUCCESS) {
        FAIL("vkCreateDevice 失败"); vkDestroyInstance(inst, nullptr); return 1;
    }
    if (!load_video_pfns(device)) { vkDestroyDevice(device, nullptr); vkDestroyInstance(inst, nullptr); return 1; }
    PASS("Vulkan device + 视频命令加载成功");

    VkQueue decodeQueue, transferQueue;
    vkGetDeviceQueue(device, decode_qf, 0, &decodeQueue);
    vkGetDeviceQueue(device, transfer_qf, 0, &transferQueue);

    // ---- 5. 视频 profile（Baseline/Main/High 按 sps->profile_idc）----
    VkVideoProfileInfoKHR profileInfo = {};
    profileInfo.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    profileInfo.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    profileInfo.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profileInfo.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profileInfo.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    VkVideoDecodeH264ProfileInfoKHR h264Profile = {};
    h264Profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
    h264Profile.stdProfileIdc = (StdVideoH264ProfileIdc)sps->profile_idc;
    h264Profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
    profileInfo.pNext = &h264Profile;

    // 查能力（取 maxDpbSlots / maxActiveReferencePictures）
    VkVideoCapabilitiesKHR caps = {};
    caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    VkVideoDecodeCapabilitiesKHR decCaps = {};
    decCaps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
    caps.pNext = &decCaps;
    VkResult vr = pvkGetPhysicalDeviceVideoCapabilitiesKHR(pd, &profileInfo, &caps);
    if (vr != VK_SUCCESS) { FAIL("查询视频能力: %s", vkResultStr(vr)); return 1; }
    INFO("maxDpbSlots=%u maxActiveReferencePictures=%u flags=0x%x",
         caps.maxDpbSlots, caps.maxActiveReferencePictures, caps.flags);
    if (decCaps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR)
        INFO("DPB 与 output DISTINCT（分离）");
    uint32_t maxDpbSlots = caps.maxDpbSlots;
    if (maxDpbSlots == 0) maxDpbSlots = 17;
    VkDeviceSize minBitstreamSizeAlign = caps.minBitstreamBufferSizeAlignment;
    if (minBitstreamSizeAlign == 0) minBitstreamSizeAlign = 128;
    INFO("minBitstreamBufferSizeAlignment=%llu", (unsigned long long)minBitstreamSizeAlign);

    // ---- 6. session ----
    VkVideoSessionCreateInfoKHR sessionCreateInfo = {};
    sessionCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    sessionCreateInfo.pNext = &profileInfo;
    sessionCreateInfo.queueFamilyIndex = (uint32_t)decode_qf;
    sessionCreateInfo.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    sessionCreateInfo.maxCodedExtent = { (uint32_t)W, (uint32_t)H };
    sessionCreateInfo.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    sessionCreateInfo.maxDpbSlots = maxDpbSlots;
    sessionCreateInfo.maxActiveReferencePictures = caps.maxActiveReferencePictures;
    sessionCreateInfo.pStdHeaderVersion = nullptr;

    VkVideoSessionKHR session = VK_NULL_HANDLE;
    vr = pvkCreateVideoSessionKHR(device, &sessionCreateInfo, nullptr, &session);
    if (vr != VK_SUCCESS) { FAIL("vkCreateVideoSessionKHR: %s", vkResultStr(vr)); return 1; }
    PASS("视频会话创建成功");

    // 绑定 session 内存
    uint32_t memReqCount = 0;
    pvkGetVideoSessionMemoryRequirementsKHR(device, session, &memReqCount, nullptr);
    std::vector<VkVideoSessionMemoryRequirementsKHR> memReqs(memReqCount);
    for (auto& m : memReqs) m.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
    pvkGetVideoSessionMemoryRequirementsKHR(device, session, &memReqCount, memReqs.data());

    std::vector<VkDeviceMemory> sessionMems(memReqCount, VK_NULL_HANDLE);
    std::vector<VkBindVideoSessionMemoryInfoKHR> binds(memReqCount);
    for (uint32_t i = 0; i < memReqCount; i++) {
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReqs[i].memoryRequirements.size;
        // 选 memoryTypeBits 里第一个 device-local 的 type
        uint32_t typeIdx = 0;
        {
            VkPhysicalDeviceMemoryProperties mp;
            vkGetPhysicalDeviceMemoryProperties(pd, &mp);
            for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
                if ((memReqs[i].memoryRequirements.memoryTypeBits & (1u << t)) &&
                    (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    typeIdx = t; break;
                }
            }
        }
        mai.memoryTypeIndex = typeIdx;
        if (vkAllocateMemory(device, &mai, nullptr, &sessionMems[i]) != VK_SUCCESS) {
            FAIL("分配 session 内存 bind=%u 失败", i); return 1;
        }
        binds[i].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
        binds[i].memoryBindIndex = i;
        binds[i].memory = sessionMems[i];
        binds[i].memoryOffset = 0;
        binds[i].memorySize = memReqs[i].memoryRequirements.size;
    }
    if (memReqCount && pvkBindVideoSessionMemoryKHR(device, session, memReqCount, binds.data()) != VK_SUCCESS) {
        FAIL("vkBindVideoSessionMemoryKHR 失败"); return 1;
    }
    PASS("会话内存绑定完成（%u 个绑定）", memReqCount);

    // ---- 7. session parameters（SPS/PPS → Std）----
    StdVideoH264ScalingLists spsSl, ppsSl;
    StdVideoH264SequenceParameterSetVui spsVui;
    StdVideoH264SequenceParameterSet stdSps;
    StdVideoH264PictureParameterSet stdPps;
    fill_sps_std(sps, &spsSl, &spsVui, &stdSps);
    fill_pps_std(pps, sps, &ppsSl, &stdPps);

    VkVideoDecodeH264SessionParametersAddInfoKHR h264Add = {};
    h264Add.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
    h264Add.stdSPSCount = 1; h264Add.pStdSPSs = &stdSps;
    h264Add.stdPPSCount = 1; h264Add.pStdPPSs = &stdPps;

    VkVideoDecodeH264SessionParametersCreateInfoKHR h264Create = {};
    h264Create.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
    h264Create.maxStdSPSCount = 1;
    h264Create.maxStdPPSCount = 1;
    h264Create.pParametersAddInfo = &h264Add;

    VkVideoSessionParametersCreateInfoKHR spCreate = {};
    spCreate.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
    spCreate.pNext = &h264Create;
    spCreate.videoSession = session;

    VkVideoSessionParametersKHR videoSessionParams = VK_NULL_HANDLE;
    if (pvkCreateVideoSessionParametersKHR(device, &spCreate, nullptr, &videoSessionParams) != VK_SUCCESS) {
        FAIL("vkCreateVideoSessionParametersKHR 失败"); return 1;
    }
    PASS("session parameters 创建成功（SPS/PPS 已上传）");

    // ---- 8. 图像池（DPB + output），OPTIMAL + CONCURRENT 共享 ----
    const VkFormat NV12 = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    uint32_t qfIndices[2] = { (uint32_t)decode_qf, (uint32_t)transfer_qf };

    // profile list（挂到 VkImageCreateInfo.pNext）
    VkVideoProfileListInfoKHR profList = {};
    profList.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    profList.profileCount = 1;
    profList.pProfiles = &profileInfo;

    auto create_image = [&](VkImageUsageFlags usage, ImageRes* r) -> bool {
        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.pNext = &profList;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = NV12;
        ici.extent = { (uint32_t)W, (uint32_t)H, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ici.queueFamilyIndexCount = 2;
        ici.pQueueFamilyIndices = qfIndices;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &ici, nullptr, &r->image) != VK_SUCCESS) return false;
        r->layout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(device, r->image, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        uint32_t typeIdx = 0;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
            if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { typeIdx = t; break; }
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = typeIdx;
        if (vkAllocateMemory(device, &mai, nullptr, &r->mem) != VK_SUCCESS) return false;
        vkBindImageMemory(device, r->image, r->mem, 0);

        VkImageViewCreateInfo vci = {};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = r->image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = NV12;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(device, &vci, nullptr, &r->view) != VK_SUCCESS) return false;
        return true;
    };

    // output 图像池（4 个复用）
    const int N_OUTPUT = 4;
    std::vector<ImageRes> outputs(N_OUTPUT);
    for (int i = 0; i < N_OUTPUT; i++) {
        if (!create_image(VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &outputs[i])) {
            FAIL("创建 output 图像 %d 失败", i); return 1;
        }
    }
    std::vector<int> free_outputs;
    for (int i = 0; i < N_OUTPUT; i++) free_outputs.push_back(i);

    // DPB 图像池（每 slot 一个）
    // 注意：RADV 要求 VCN<5 时 DPB 与 DST 分离（radv_video.c:1184），DPB 图只能带 DPB usage，
    //       不能同时带 VIDEO_DECODE_DST（否则 VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR）。
    std::vector<ImageRes> dpbImages(maxDpbSlots);
    for (uint32_t i = 0; i < maxDpbSlots; i++) {
        if (!create_image(VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR, &dpbImages[i])) {
            FAIL("创建 DPB 图像 %u 失败", i); return 1;
        }
    }
    std::vector<int> free_dpb_slots;
    for (uint32_t i = 0; i < maxDpbSlots; i++) free_dpb_slots.push_back((int)i);
    // DPB 图 layout 初始化标记（首次作为 setup slot 写目标时，需 UNDEFINED → VIDEO_DECODE_DPB）
    std::vector<uint8_t> dpbLayoutInit(maxDpbSlots, 0);
    PASS("图像池创建：output×%d + DPB×%u", N_OUTPUT, maxDpbSlots);

    // ---- 9. 码流 buffer（HOST_VISIBLE）+ 可导出 buffer ----
    // 码流 buffer：按帧存放 Annex-B slice（含 start code）。每帧解码前写入当前 slice，
    // srcBufferOffset 指向 start code，pSliceOffsets[0]=0（RADV 直接送 srcBufferOffset+slice_offset）。
    // 一个复用 buffer 即可（解码是逐帧串行的，transfer 完成前 buffer 不会被改写）。
    VkBuffer bitstreamBuf = VK_NULL_HANDLE;
    VkDeviceMemory bitstreamMem = VK_NULL_HANDLE;
    size_t bsBufSize = 0;
    {
        // 上限：单个 slice NAL 的 raw_size + 4(start code) + 对齐余量
        size_t maxSlice = 0;
        for (auto* n : slices) maxSlice = std::max(maxSlice, (size_t)n->raw_size);
        bsBufSize = (maxSlice + 4 + minBitstreamSizeAlign - 1) / minBitstreamSizeAlign * minBitstreamSizeAlign;

        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bsBufSize;
        bci.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bci, nullptr, &bitstreamBuf) != VK_SUCCESS) { FAIL("创建码流 buffer 失败"); return 1; }
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(device, bitstreamBuf, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        uint32_t typeIdx = 0;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
            if ((mr.memoryTypeBits & (1u << t)) &&
                (mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) { typeIdx = t; break; }
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = typeIdx;
        vkAllocateMemory(device, &mai, nullptr, &bitstreamMem);
        vkBindBufferMemory(device, bitstreamBuf, bitstreamMem, 0);
        INFO("码流 buffer：%zu bytes（单 slice 复用）", bsBufSize);
    }

    // 导出 pitch（对齐到 optimalBufferCopyRowPitchAlignment）
    uint32_t pitchAlign = pdp.limits.optimalBufferCopyRowPitchAlignment;
    if (pitchAlign == 0) pitchAlign = 256;
    uint32_t pitch = (W + pitchAlign - 1) / pitchAlign * pitchAlign;
    VkDeviceSize exportSize = (VkDeviceSize)pitch * H * 3 / 2;

    VkBuffer exportBuf = VK_NULL_HANDLE;
    VkDeviceMemory exportMem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = exportSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bci, nullptr, &exportBuf) != VK_SUCCESS) { FAIL("创建导出 buffer 失败"); return 1; }
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(device, exportBuf, &mr);

        VkExportMemoryAllocateInfo exportInfo = {};
        exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        uint32_t typeIdx = 0;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
            if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { typeIdx = t; break; }

        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext = &exportInfo;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = typeIdx;
        if (vkAllocateMemory(device, &mai, nullptr, &exportMem) != VK_SUCCESS) { FAIL("分配可导出内存失败"); return 1; }
        vkBindBufferMemory(device, exportBuf, exportMem, 0);
    }
    INFO("导出布局: pitch=%u exportSize=%llu", pitch, (unsigned long long)exportSize);

    // ---- 10. 同步对象 + 命令池 ----
    VkSemaphoreCreateInfo sci = {}; sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore decodeDoneSem, copyDoneSem;
    vkCreateSemaphore(device, &sci, nullptr, &decodeDoneSem);
    vkCreateSemaphore(device, &sci, nullptr, &copyDoneSem);
    VkFenceCreateInfo fci = {}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence copyFence;
    vkCreateFence(device, &fci, nullptr, &copyFence);

    VkCommandPoolCreateInfo cpci = {};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = (uint32_t)decode_qf;
    VkCommandPool decodePool, transferPool;
    vkCreateCommandPool(device, &cpci, nullptr, &decodePool);
    cpci.queueFamilyIndex = (uint32_t)transfer_qf;
    vkCreateCommandPool(device, &cpci, nullptr, &transferPool);

    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = decodePool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer decodeCmd, transferCmd;
    vkAllocateCommandBuffers(device, &cbai, &decodeCmd);
    cbai.commandPool = transferPool;
    vkAllocateCommandBuffers(device, &cbai, &transferCmd);

    int fd_before = count_fds();
    INFO("初始 fd 数 = %d", fd_before);

    // ---- 11. 逐帧解码 → 拷贝 → 导出 → HIP 读 ----
    std::vector<DpbEntry> dpb;
    H264POCContext poc_ctx; memset(&poc_ctx, 0, sizeof(poc_ctx));
    poc_ctx.prev_poc_lsb = -1; poc_ctx.prev_poc_msb = 0;
    std::vector<DecodedFrame> decoded;
    int all_ok = 1;

    for (int f = 0; f < n_frames; f++) {
        SliceInfo si;
        if (!parse_slice_header(slices[f], sps, pps, &si)) { FAIL("帧 %d slice header 解析失败", f); all_ok = 0; break; }
        H2645NAL* slice_nal = slices[f];

        // ---- 11a. 分配 output 图像 ----
        if (free_outputs.empty()) { FAIL("帧 %d 无空闲 output", f); all_ok = 0; break; }
        int out_idx = free_outputs.back(); free_outputs.pop_back();
        ImageRes& out = outputs[out_idx];

        // ---- 11b. POC 计算 ----
        poc_ctx.frame_num = si.frame_num;
        poc_ctx.poc_lsb = si.poc_lsb;
        poc_ctx.delta_poc_bottom = si.delta_poc_bottom;
        poc_ctx.delta_poc[0] = si.delta_poc[0];
        poc_ctx.delta_poc[1] = si.delta_poc[1];
        int field_poc[2], pic_poc;
        if (ff_h264_init_poc(field_poc, &pic_poc, sps, &poc_ctx, PICT_FRAME, si.nal_ref_idc) < 0) {
            FAIL("帧 %d ff_h264_init_poc 失败", f); all_ok = 0; break;
        }
        if (si.nal_ref_idc != 0) { poc_ctx.prev_poc_msb = poc_ctx.poc_msb; poc_ctx.prev_poc_lsb = poc_ctx.poc_lsb; }
        poc_ctx.prev_frame_num = poc_ctx.frame_num;
        poc_ctx.prev_frame_num_offset = poc_ctx.frame_num_offset;

        // ---- 11c. 分配 setup slot（每帧都需要，含 B 帧）----
        if (free_dpb_slots.empty()) { FAIL("帧 %d 无空闲 DPB slot", f); all_ok = 0; break; }
        int setup_slot = free_dpb_slots.back(); free_dpb_slots.pop_back();

        // ---- 11d. reference slots（active 参考帧，任意顺序，驱动自己排）----
        std::vector<VkVideoReferenceSlotInfoKHR> refSlots(dpb.size());
        std::vector<VkVideoPictureResourceInfoKHR> refRes(dpb.size());
        std::vector<VkVideoDecodeH264DpbSlotInfoKHR> refDpb(dpb.size());
        std::vector<StdVideoDecodeH264ReferenceInfo> refStd(dpb.size());
        for (size_t i = 0; i < dpb.size(); i++) {
            refStd[i] = {};
            refStd[i].FrameNum = (uint16_t)dpb[i].frame_num;
            refStd[i].PicOrderCnt[0] = dpb[i].field_poc[0];
            refStd[i].PicOrderCnt[1] = dpb[i].field_poc[1];
            refStd[i].flags.top_field_flag = 1;
            refStd[i].flags.bottom_field_flag = 1;

            refDpb[i] = {};
            refDpb[i].sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
            refDpb[i].pStdReferenceInfo = &refStd[i];

            refRes[i] = {};
            refRes[i].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
            refRes[i].codedExtent = { (uint32_t)W, (uint32_t)H };
            refRes[i].baseArrayLayer = 0;
            refRes[i].imageViewBinding = dpbImages[dpb[i].slot].view;

            refSlots[i] = {};
            refSlots[i].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
            refSlots[i].pNext = &refDpb[i];
            refSlots[i].slotIndex = dpb[i].slot;
            refSlots[i].pPictureResource = &refRes[i];
        }

        // setup reference slot（当前图，挂 DpbSlotInfo 描述本图参考信息，与 FFmpeg vk_h264_fill_pict is_current=1 一致）
        VkVideoPictureResourceInfoKHR setupRes = {};
        setupRes.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        setupRes.codedExtent = { (uint32_t)W, (uint32_t)H };
        setupRes.baseArrayLayer = 0;
        setupRes.imageViewBinding = dpbImages[setup_slot].view;
        StdVideoDecodeH264ReferenceInfo setupStd = {};
        setupStd.FrameNum = (uint16_t)si.frame_num;
        setupStd.PicOrderCnt[0] = field_poc[0];
        setupStd.PicOrderCnt[1] = field_poc[1];
        setupStd.flags.top_field_flag = 1;
        setupStd.flags.bottom_field_flag = 1;
        VkVideoDecodeH264DpbSlotInfoKHR setupDpb = {};
        setupDpb.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
        setupDpb.pStdReferenceInfo = &setupStd;
        VkVideoReferenceSlotInfoKHR setupSlot = {};
        setupSlot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        setupSlot.pNext = &setupDpb;
        setupSlot.slotIndex = setup_slot;
        setupSlot.pPictureResource = &setupRes;

        // ---- 11e. 写 Annex-B slice 到码流 buffer ----
        // raw_data 指向「去 start code 的 slice 数据」（h2645_parse.c ff_h2645_extract_rbsp 的 src），
        // raw_size 为去掉 start code 后的长度；这里手动补回 3 字节 start code。
        {
            uint8_t startcode[4] = { 0x00, 0x00, 0x00, 0x01 };
            void* p = nullptr;
            vkMapMemory(device, bitstreamMem, 0, bsBufSize, 0, &p);
            memset(p, 0, bsBufSize);   // 清零 padding，避免固件读到脏数据
            uint8_t* dst = (uint8_t*)p;
            memcpy(dst, startcode, 4);
            memcpy(dst + 4, slice_nal->raw_data, slice_nal->raw_size);
            vkUnmapMemory(device, bitstreamMem);
        }

        // ---- 11f. StdVideoDecodeH264PictureInfo ----
        StdVideoDecodeH264PictureInfo stdPic = {};
        stdPic.flags.is_intra = (si.slice_type_nos == AV_PICTURE_TYPE_I);
        stdPic.flags.IdrPicFlag = (si.nal_type == H264_NAL_IDR_SLICE);
        stdPic.flags.is_reference = (si.nal_ref_idc != 0);
        stdPic.seq_parameter_set_id = (uint8_t)sps->sps_id;
        stdPic.pic_parameter_set_id = (uint8_t)pps->pps_id;
        stdPic.frame_num = (uint16_t)si.frame_num;
        stdPic.idr_pic_id = (uint16_t)si.idr_pic_id;
        stdPic.PicOrderCnt[0] = field_poc[0];
        stdPic.PicOrderCnt[1] = field_poc[1];

        // slice offset：RADV 用 pSliceOffsets[0] 相对 srcBufferOffset 定位（radv_video.c:1601）。
        // 因为每个 slice 独占一次 buffer 写入，srcBufferOffset=0，slice 从 buffer 起点开始。
        uint32_t sliceOffsets[1] = { 0 };

        // ---- 11f. 录制 decode 命令（decode 队列）----
        vkResetCommandPool(device, decodePool, 0);
        VkCommandBufferBeginInfo cbbi = {};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(decodeCmd, &cbbi);

        // output 图像 layout 转 VIDEO_DECODE_DST
        {
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
            vkCmdPipelineBarrier2(decodeCmd, &di);
            out.layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
        }

        // setup slot 图 layout 转 VIDEO_DECODE_DPB（首次 UNDEFINED→DPB，复用 DPB→DPB）
        {
            bool inited = dpbLayoutInit[setup_slot] != 0;
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
            b.image = dpbImages[setup_slot].image;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkDependencyInfo di = {};
            di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(decodeCmd, &di);
            dpbLayoutInit[setup_slot] = 1;
        }

        VkVideoBeginCodingInfoKHR begin = {};
        begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
        begin.videoSession = session;
        begin.videoSessionParameters = videoSessionParams;
        begin.referenceSlotCount = (uint32_t)refSlots.size();
        begin.pReferenceSlots = refSlots.data();
        pvkCmdBeginVideoCodingKHR(decodeCmd, &begin);

        VkVideoDecodeH264PictureInfoKHR h264PicInfo = {};
        h264PicInfo.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR;
        h264PicInfo.pStdPictureInfo = &stdPic;
        h264PicInfo.sliceCount = 1;
        h264PicInfo.pSliceOffsets = sliceOffsets;

        VkVideoDecodeInfoKHR decodeInfo = {};
        decodeInfo.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
        decodeInfo.pNext = &h264PicInfo;
        decodeInfo.srcBuffer = bitstreamBuf;
        decodeInfo.srcBufferOffset = 0;
        {
            VkDeviceSize r = 4 + (VkDeviceSize)slice_nal->raw_size;
            // 对齐到 minBitstreamBufferSizeAlignment（对齐 FFmpeg vulkan_decode.c:345）
            r = (r + minBitstreamSizeAlign - 1) / minBitstreamSizeAlign * minBitstreamSizeAlign;
            decodeInfo.srcBufferRange = r;
        }
        decodeInfo.dstPictureResource = {};
        decodeInfo.dstPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        decodeInfo.dstPictureResource.codedExtent = { (uint32_t)W, (uint32_t)H };
        decodeInfo.dstPictureResource.baseArrayLayer = 0;
        decodeInfo.dstPictureResource.imageViewBinding = out.view;
        decodeInfo.pSetupReferenceSlot = &setupSlot;
        decodeInfo.referenceSlotCount = (uint32_t)refSlots.size();
        decodeInfo.pReferenceSlots = refSlots.data();
        pvkCmdDecodeVideoKHR(decodeCmd, &decodeInfo);

        VkVideoEndCodingInfoKHR end = {};
        end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
        pvkCmdEndVideoCodingKHR(decodeCmd, &end);
        vkEndCommandBuffer(decodeCmd);

        // submit decode：signal decodeDoneSem（首帧不 wait copyDoneSem）
        {
            VkSemaphoreSubmitInfo waitSem = {};
            waitSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waitSem.semaphore = copyDoneSem;
            waitSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSemaphoreSubmitInfo sigSem = {};
            sigSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            sigSem.semaphore = decodeDoneSem;
            sigSem.stageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
            VkCommandBufferSubmitInfo cb = {};
            cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cb.commandBuffer = decodeCmd;
            VkSubmitInfo2 si2 = {};
            si2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            si2.waitSemaphoreInfoCount = (f == 0) ? 0 : 1;
            si2.pWaitSemaphoreInfos = &waitSem;
            si2.commandBufferInfoCount = 1;
            si2.pCommandBufferInfos = &cb;
            si2.signalSemaphoreInfoCount = 1;
            si2.pSignalSemaphoreInfos = &sigSem;
            if (vkQueueSubmit2(decodeQueue, 1, &si2, VK_NULL_HANDLE) != VK_SUCCESS) {
                FAIL("帧 %d decode submit 失败", f); all_ok = 0; break;
            }
        }

        // ---- 11g. 录制 transfer 命令（barrier → copy）----
        vkResetCommandPool(device, transferPool, 0);
        vkBeginCommandBuffer(transferCmd, &cbbi);

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
            vkCmdPipelineBarrier2(transferCmd, &di);
        }

        VkBufferImageCopy regions[2] = {};
        regions[0] = {};
        regions[0].bufferOffset = 0;
        regions[0].bufferRowLength = pitch;
        regions[0].bufferImageHeight = (uint32_t)H;
        regions[0].imageSubresource = { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 };
        regions[0].imageExtent = { (uint32_t)W, (uint32_t)H, 1 };
        regions[1] = {};
        regions[1].bufferOffset = (VkDeviceSize)pitch * H;
        regions[1].bufferRowLength = pitch;
        regions[1].bufferImageHeight = (uint32_t)H / 2;
        regions[1].imageSubresource = { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 };
        regions[1].imageExtent = { (uint32_t)W, (uint32_t)H / 2, 1 };
        vkCmdCopyImageToBuffer(transferCmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               exportBuf, 2, regions);
        out.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;   // copy 后 layout 更新，供下一帧 barrier 用
        vkEndCommandBuffer(transferCmd);

        {
            VkSemaphoreSubmitInfo waitSem = {};
            waitSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waitSem.semaphore = decodeDoneSem;
            waitSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSemaphoreSubmitInfo sigSem = {};
            sigSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            sigSem.semaphore = copyDoneSem;
            sigSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkCommandBufferSubmitInfo cb = {};
            cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cb.commandBuffer = transferCmd;
            VkSubmitInfo2 si2 = {};
            si2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            si2.waitSemaphoreInfoCount = 1;
            si2.pWaitSemaphoreInfos = &waitSem;
            si2.commandBufferInfoCount = 1;
            si2.pCommandBufferInfos = &cb;
            si2.signalSemaphoreInfoCount = 1;
            si2.pSignalSemaphoreInfos = &sigSem;
            if (vkQueueSubmit2(transferQueue, 1, &si2, copyFence) != VK_SUCCESS) {
                FAIL("帧 %d transfer submit 失败", f); all_ok = 0; break;
            }
        }

        vkWaitForFences(device, 1, &copyFence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &copyFence);

        // ---- 11h. 导出 fd → HIP 导入 → 读回 ----
        int fd = -1;
        {
            VkMemoryGetFdInfoKHR fdInfo = {};
            fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            fdInfo.memory = exportMem;
            fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            if (pvkGetMemoryFdKHR(device, &fdInfo, &fd) != VK_SUCCESS || fd < 0) {
                FAIL("帧 %d vkGetMemoryFdKHR 失败", f); all_ok = 0; break;
            }
        }

        hipInit(0);
        hipExternalMemory_t extMem = nullptr;
        void* devPtr = nullptr;
        {
            hipExternalMemoryHandleDesc hd = {};
            hd.type = hipExternalMemoryHandleTypeOpaqueFd;
            hd.handle.fd = fd;
            hd.size = exportSize;
            if (hipImportExternalMemory(&extMem, &hd) != hipSuccess) { FAIL("帧 %d hipImportExternalMemory", f); close(fd); all_ok = 0; break; }
            hipExternalMemoryBufferDesc bd = {};
            bd.size = exportSize;
            if (hipExternalMemoryGetMappedBuffer(&devPtr, extMem, &bd) != hipSuccess) { FAIL("帧 %d GetMappedBuffer", f); hipDestroyExternalMemory(extMem); close(fd); all_ok = 0; break; }
        }

        std::vector<uint8_t> y(W * H), uv(W * H / 2);
        char* ybase = (char*)devPtr;
        char* uvbase = (char*)devPtr + (size_t)pitch * H;
        for (int r = 0; r < H; r++)
            hipMemcpy(y.data() + r * W, ybase + (size_t)r * pitch, W, hipMemcpyDeviceToHost);
        for (int r = 0; r < H / 2; r++)
            hipMemcpy(uv.data() + r * W, uvbase + (size_t)r * pitch, W, hipMemcpyDeviceToHost);

        hipDestroyExternalMemory(extMem);
        close(fd);

        // 收集
        DecodedFrame df;
        df.poc = pic_poc;
        df.frame_num = si.frame_num;
        df.slice_type = si.slice_type_nos;
        df.y = std::move(y);
        df.uv = std::move(uv);
        decoded.push_back(std::move(df));

        // ---- 11i. DPB 更新（MMCO + 滑动窗口 + 插入）----
        if (si.nal_type == H264_NAL_IDR_SLICE) {
            for (auto& e : dpb) free_dpb_slots.push_back(e.slot);
            dpb.clear();
            poc_ctx.prev_frame_num = 0;
            poc_ctx.prev_frame_num_offset = 0;
            poc_ctx.prev_poc_msb = 0;
            poc_ctx.prev_poc_lsb = -1;
        } else if (si.nal_ref_idc != 0) {
            for (int i = 0; i < si.nb_mmco; i++) {
                if (si.mmco[i].opcode == MMCO_SHORT2UNUSED) {
                    int fn = si.mmco[i].short_pic_num;
                    for (auto it = dpb.begin(); it != dpb.end(); ++it) {
                        if (it->frame_num == fn) { free_dpb_slots.push_back(it->slot); dpb.erase(it); break; }
                    }
                }
            }
            if (!si.explicit_ref_marking && (int)dpb.size() >= sps->ref_frame_count && !dpb.empty()) {
                free_dpb_slots.push_back(dpb.back().slot);
                dpb.pop_back();
            }
        }

        if (si.nal_ref_idc != 0) {
            DpbEntry e;
            e.slot = setup_slot;
            e.frame_num = si.frame_num;
            e.poc = pic_poc;
            e.field_poc[0] = field_poc[0];
            e.field_poc[1] = field_poc[1];
            dpb.insert(dpb.begin(), e);
        } else {
            free_dpb_slots.push_back(setup_slot);   // B 帧临时 slot 回收
        }

        free_outputs.push_back(out_idx);   // output 图像回收

        if ((f + 1) % 10 == 0) INFO("帧 %d/%d 完成 (poc=%d fn=%d type=%d)", f + 1, n_frames, pic_poc, si.frame_num, si.slice_type_nos);
    }

    // ---- 12. 显示序重排 + 比对 ----
    if (all_ok && (int)decoded.size() == n_frames) {
        std::stable_sort(decoded.begin(), decoded.end(),
                         [](const DecodedFrame& a, const DecodedFrame& b){ return a.poc < b.poc; });
        int n_bad = 0;
        for (int i = 0; i < n_frames; i++) {
            const uint8_t* ref_y = ref.data() + (size_t)i * frame_bytes;
            const uint8_t* ref_uv = ref.data() + (size_t)i * frame_bytes + W * H;
            size_t diff = 0;
            for (size_t k = 0; k < decoded[i].y.size(); k++) if (decoded[i].y[k] != ref_y[k]) diff++;
            for (size_t k = 0; k < decoded[i].uv.size(); k++) if (decoded[i].uv[k] != ref_uv[k]) diff++;
            if (diff != 0) {
                FAIL("显示序帧 %d (poc=%d fn=%d type=%d) 像素不一致，差异=%zu/%zu",
                     i, decoded[i].poc, decoded[i].frame_num, decoded[i].slice_type,
                     diff, decoded[i].y.size() + decoded[i].uv.size());
                n_bad++;
            } else if (i < 3 || i == n_frames - 1) {
                PASS("显示序帧 %d/%d (poc=%d fn=%d type=%d) 逐字节一致",
                     i, n_frames, decoded[i].poc, decoded[i].frame_num, decoded[i].slice_type);
            }
        }
        if (n_bad == 0) PASS("显示序重排比对通过：%d 帧全部逐字节一致", n_frames);
        else all_ok = 0;
    } else if (all_ok) {
        FAIL("解码帧数 %d != 期望 %d", (int)decoded.size(), n_frames);
        all_ok = 0;
    }

    // ---- 13. fd 泄漏检查 ----
    int fd_after = count_fds();
    INFO("最终 fd 数 = %d（初始 %d）", fd_after, fd_before);
    if (fd_after <= fd_before + 5) PASS("fd 无泄漏（初始=%d 最终=%d）", fd_before, fd_after);
    else FAIL("fd 泄漏（初始=%d 最终=%d）", fd_before, fd_after);

    // ---- 14. 清理 ----
    vkDeviceWaitIdle(device);
    vkDestroyFence(device, copyFence, nullptr);
    vkDestroySemaphore(device, decodeDoneSem, nullptr);
    vkDestroySemaphore(device, copyDoneSem, nullptr);
    vkDestroyCommandPool(device, decodePool, nullptr);
    vkDestroyCommandPool(device, transferPool, nullptr);
    vkDestroyBuffer(device, exportBuf, nullptr);
    vkFreeMemory(device, exportMem, nullptr);
    vkDestroyBuffer(device, bitstreamBuf, nullptr);
    vkFreeMemory(device, bitstreamMem, nullptr);
    for (auto& o : outputs) { vkDestroyImageView(device, o.view, nullptr); vkDestroyImage(device, o.image, nullptr); vkFreeMemory(device, o.mem, nullptr); }
    for (auto& d : dpbImages) { vkDestroyImageView(device, d.view, nullptr); vkDestroyImage(device, d.image, nullptr); vkFreeMemory(device, d.mem, nullptr); }
    pvkDestroyVideoSessionParametersKHR(device, videoSessionParams, nullptr);
    pvkDestroyVideoSessionKHR(device, session, nullptr);
    for (auto m : sessionMems) vkFreeMemory(device, m, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(inst, nullptr);

    ff_h2645_packet_uninit(&pkt);
    ff_h264_ps_uninit(&ps);

    printf("\n========== 汇总 ==========\n");
    printf("  PASS: %d   FAIL: %d   （解码 %d 帧）\n", g_pass, g_fail, n_frames);
    if (g_fail == 0) printf("  >>> P0 通过：Vulkan Video 解码→HIP 读逐字节一致 + 无泄漏\n");
    else printf("  >>> 存在失败项，见上方 FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
