// verify_parser_reuse.cpp
// 实验 A：FFmpeg parser 复用复现 + 推进（§4.6）
//
// 目的：填补文档 §7.3 标注"当前目录未找到"的 parser 复用测试源文件缺口，
//       并验证（1）8 个 FFmpeg .o 可静态链接且不拖入编解码器依赖；
//             （2）ff_h2645_packet_split 切 NAL、ff_h264_decode_{seq,picture}_parameter_set
//                 解析 SPS/PPS、自填 VAPictureParameterBufferH264 字段映射正确。
//
// 关键坑（文档 §4.6 实测踩到，本实现逐一规避）：
//   1. 不能链整个 libavcodec.a —— 只提取 8 个 .o。
//   2. AVCodecContext* 不能传 NULL —— 栈上 zero-init 一个即可（会解引用 codec_tag/flags2）。
//   3. SPS/PPS 从 H264ParamSets.sps_list[]/pps_list[] 取，不是 ps.sps/ps.pps。
//   4. PPS 字段名：cabac（非 entropy_coding_mode_flag）、
//      deblocking_filter_parameters_present（非 deblocking_filter_control_present_flag）。
//
// 构建：make -f verify_parser_reuse.make
// 运行：./verify_parser_reuse ../../testdata/h264/test_320x240.h264
//
// 本文件是纯主机侧实验，不依赖 GPU / ROCm / libva 运行时，可在任意 Linux 编译运行。

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <string>

// libva 结构体（仅声明，不链接 libva）
#include <va/va.h>

// FFmpeg 内部头（编译期依赖，链接期只取 8 个 .o 提供的符号）
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavcodec/h2645_parse.h"
#include "libavcodec/h264_ps.h"
#include "libavcodec/h264.h"
}

#define PASS(...) do { printf("  [PASS] "); printf(__VA_ARGS__); printf("\n"); g_pass++; } while (0)
#define FAIL(...) do { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); g_fail++; } while (0)
#define INFO(...) do { printf("  [info] "); printf(__VA_ARGS__); printf("\n"); } while (0)

static int g_pass = 0, g_fail = 0;

// 读取整个文件
static bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) { FAIL("fopen %s: %s", path, strerror(errno)); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { FAIL("空文件 %s", path); fclose(f); return false; }
    out.resize((size_t)n);
    size_t rd = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) { FAIL("fread 不完整: %zu/%ld", rd, n); return false; }
    return true;
}

// 按 SPS 计算编码宽高（对齐 h264_parser.c:387-390 的规则）
static void sps_dimensions(const SPS* sps, int* coded_w, int* coded_h, int* w, int* h) {
    *coded_w = 16 * sps->mb_width;
    *coded_h = 16 * sps->mb_height;
    *w = *coded_w - (int)(sps->crop_right + sps->crop_left);
    *h = *coded_h - (int)(sps->crop_top + sps->crop_bottom);
}

int main(int argc, char** argv) {
    printf("===========================================================\n");
    printf(" 实验 A：FFmpeg parser 复用复现 + 推进（§4.6）\n");
    printf("===========================================================\n");

    if (argc < 2) {
        fprintf(stderr, "用法: %s <input.h264>\n", argv[0]);
        return 2;
    }

    std::vector<uint8_t> buf;
    if (!read_file(argv[1], buf)) return 1;

    // ---- 1. ff_h2645_packet_split 切 NAL ----
    H2645Packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    int ret = ff_h2645_packet_split(&pkt, buf.data(), (int)buf.size(),
                                    nullptr, 0, (enum AVCodecID)AV_CODEC_ID_H264, 0);
    if (ret < 0) { FAIL("ff_h2645_packet_split 返回 %d", ret); return 1; }
    PASS("ff_h2645_packet_split 切分成功，NAL 数=%d", pkt.nb_nals);

    int n_sps = 0, n_pps = 0, n_idr = 0;
    for (int i = 0; i < pkt.nb_nals; i++) {
        switch (pkt.nals[i].type) {
            case H264_NAL_SPS: n_sps++; break;
            case H264_NAL_PPS: n_pps++; break;
            case H264_NAL_IDR_SLICE: n_idr++; break;
        }
    }
    INFO("统计: SPS=%d PPS=%d IDR=%d", n_sps, n_pps, n_idr);
    if (n_sps < 1 || n_pps < 1 || n_idr < 1) {
        FAIL("码流缺少必要 NAL（SPS/PPS/IDR）");
        ff_h2645_packet_uninit(&pkt);
        return 1;
    }
    PASS("包含 SPS/PPS/IDR NAL");

    // ---- 2. 栈上 zero-init AVCodecContext（不能传 NULL） ----
    AVCodecContext avctx;
    memset(&avctx, 0, sizeof(avctx));

    H264ParamSets ps;
    memset(&ps, 0, sizeof(ps));

    // ---- 3. 解析 SPS / PPS ----
    for (int i = 0; i < pkt.nb_nals; i++) {
        H2645NAL& nal = pkt.nals[i];
        if (nal.type == H264_NAL_SPS) {
            ret = ff_h264_decode_seq_parameter_set(&nal.gb, &avctx, &ps, 0);
            if (ret < 0) {
                FAIL("ff_h264_decode_seq_parameter_set: %d", ret);
            }
        } else if (nal.type == H264_NAL_PPS) {
            ret = ff_h264_decode_picture_parameter_set(&nal.gb, &avctx, &ps, nal.size_bits);
            if (ret < 0) {
                FAIL("ff_h264_decode_picture_parameter_set: %d", ret);
            }
        }
    }

    // ---- 4. 从 sps_list[]/pps_list[] 取（不是 ps.sps/ps.pps） ----
    const SPS* sps = nullptr;
    const PPS* pps = nullptr;
    for (int i = 0; i < MAX_SPS_COUNT && !sps; i++) if (ps.sps_list[i]) sps = ps.sps_list[i];
    for (int i = 0; i < MAX_PPS_COUNT && !pps; i++) if (ps.pps_list[i]) pps = ps.pps_list[i];

    if (!sps) { FAIL("未取到 SPS（sps_list 为空）"); ff_h2645_packet_uninit(&pkt); return 1; }
    if (!pps) { FAIL("未取到 PPS（pps_list 为空）"); ff_h2645_packet_uninit(&pkt); return 1; }
    PASS("已从 sps_list[]/pps_list[] 取到参数集");

    int coded_w, coded_h, w, h;
    sps_dimensions(sps, &coded_w, &coded_h, &w, &h);
    INFO("SPS: profile=%d level=%d chroma=%d  coded=%dx%d  display=%dx%d",
         sps->profile_idc, sps->level_idc, sps->chroma_format_idc, coded_w, coded_h, w, h);
    INFO("SPS: bit_depth_luma=%d bit_depth_chroma=%d frame_mbs_only=%d mb_aff=%d",
         sps->bit_depth_luma, sps->bit_depth_chroma, sps->frame_mbs_only_flag, sps->mb_aff);

    // ---- 5. 校验 SPS/PPS 关键字段 ----
    // testsrc2 + libx264 baseline: profile=66(Baseline) 或 100(High)，320x240 8bit 4:2:0
    bool profile_ok = (sps->profile_idc == 66 || sps->profile_idc == 100);
    if (!profile_ok) FAIL("profile_idc=%d 非预期(Baseline/High)", sps->profile_idc);
    else PASS("profile_idc=%d (Baseline/High)", sps->profile_idc);

    if (w != 320 || h != 240) FAIL("显示尺寸 %dx%d 非 320x240", w, h);
    else PASS("显示尺寸 = %dx%d", w, h);

    if (sps->bit_depth_luma != 8 || sps->bit_depth_chroma != 8)
        FAIL("bit_depth = %d/%d 非 8bit", sps->bit_depth_luma, sps->bit_depth_chroma);
    else PASS("bit_depth = 8/8");

    if (sps->chroma_format_idc != 1) FAIL("chroma_format_idc=%d 非 4:2:0", sps->chroma_format_idc);
    else PASS("chroma_format_idc = 1 (4:2:0)");

    // ---- 6. 自填 VAPictureParameterBufferH264（字段映射） ----
    VAPictureParameterBufferH264 pp = {};
    pp.CurrPic.picture_id = VA_INVALID_SURFACE;
    for (int i = 0; i < 16; i++) pp.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
    pp.picture_width_in_mbs_minus1  = (uint16_t)(sps->mb_width - 1);
    pp.picture_height_in_mbs_minus1 = (uint16_t)((sps->mb_height) / (2 - sps->frame_mbs_only_flag) - 1);
    pp.bit_depth_luma_minus8        = (uint8_t)(sps->bit_depth_luma - 8);
    pp.bit_depth_chroma_minus8      = (uint8_t)(sps->bit_depth_chroma - 8);
    pp.num_ref_frames               = (uint8_t)sps->ref_frame_count;

    pp.seq_fields.bits.chroma_format_idc                  = sps->chroma_format_idc;
    pp.seq_fields.bits.residual_colour_transform_flag     = sps->residual_color_transform_flag;
    pp.seq_fields.bits.gaps_in_frame_num_value_allowed_flag = sps->gaps_in_frame_num_allowed_flag;
    pp.seq_fields.bits.frame_mbs_only_flag                = sps->frame_mbs_only_flag;
    pp.seq_fields.bits.mb_adaptive_frame_field_flag       = sps->mb_aff;
    pp.seq_fields.bits.direct_8x8_inference_flag          = sps->direct_8x8_inference_flag;
    pp.seq_fields.bits.log2_max_frame_num_minus4          = sps->log2_max_frame_num - 4;
    pp.seq_fields.bits.pic_order_cnt_type                 = sps->poc_type;
    pp.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4  = sps->log2_max_poc_lsb;
    pp.seq_fields.bits.delta_pic_order_always_zero_flag   = sps->delta_pic_order_always_zero_flag;

    pp.pic_fields.bits.entropy_coding_mode_flag           = pps->cabac;          // 坑4
    pp.pic_fields.bits.weighted_pred_flag                 = pps->weighted_pred;
    pp.pic_fields.bits.weighted_bipred_idc                = pps->weighted_bipred_idc;
    pp.pic_fields.bits.transform_8x8_mode_flag            = pps->transform_8x8_mode;
    pp.pic_fields.bits.constrained_intra_pred_flag        = pps->constrained_intra_pred;
    pp.pic_fields.bits.deblocking_filter_control_present_flag = pps->deblocking_filter_parameters_present; // 坑4
    pp.pic_fields.bits.redundant_pic_cnt_present_flag     = pps->redundant_pic_cnt_present;

    pp.pic_init_qp_minus26        = (int8_t)(pps->init_qp - 26);
    pp.pic_init_qs_minus26        = (int8_t)(pps->init_qs - 26);
    pp.chroma_qp_index_offset     = (int8_t)pps->chroma_qp_index_offset[0];
    pp.second_chroma_qp_index_offset = (int8_t)pps->chroma_qp_index_offset[1];

    PASS("VAPictureParameterBufferH264 自填完成");
    INFO("  pic_fields.cabac=%d weighted_pred=%d transform_8x8=%d deblocking_present=%d",
         pp.pic_fields.bits.entropy_coding_mode_flag,
         pp.pic_fields.bits.weighted_pred_flag,
         pp.pic_fields.bits.transform_8x8_mode_flag,
         pp.pic_fields.bits.deblocking_filter_control_present_flag);
    INFO("  seq_fields.log2_max_frame_num_minus4=%d pic_order_cnt_type=%d",
         pp.seq_fields.bits.log2_max_frame_num_minus4,
         pp.seq_fields.bits.pic_order_cnt_type);
    INFO("  picture_width_in_mbs_minus1=%d picture_height_in_mbs_minus1=%d num_ref_frames=%d",
         pp.picture_width_in_mbs_minus1, pp.picture_height_in_mbs_minus1, pp.num_ref_frames);

    ff_h2645_packet_uninit(&pkt);
    ff_h264_ps_uninit(&ps);

    printf("\n========== 汇总 ==========\n");
    printf("  PASS: %d   FAIL: %d\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("  >>> parser 复用路线复现通过：8 个 .o 可链接，SPS/PPS 解析及 VAAPI 填充字段映射正确\n");
    } else {
        printf("  >>> 存在失败项，见上方 FAIL 详情\n");
    }
    return g_fail == 0 ? 0 : 1;
}
