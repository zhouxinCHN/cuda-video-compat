// verify_decode_to_hip.cpp
// 实验 C：真实解码 → HIP 读，端到端像素校验（§1.7 第1步 / §4.2-A4 / §4.10）
//
// I/P 帧版：在 I-only 多帧版基础上，扩展支持 P 帧（§7.2 P1 的 DPB 核心）。
//
// 复用 FFmpeg 纯语法函数（不依赖完整解码器状态机）：
//   - ff_h264_init_poc          —— POC 计算（依赖 SPS + H264POCContext）
//   - ff_h264_parse_ref_count   —— 参考帧数量语法
// 手写（FFmpeg 对应实现耦合 H264Context，不可轻量复用）：
//   - 滑动窗口 DPB 状态机 + MMCO（SHORT2UNUSED）
//   - RefPicList0 默认构建（frame_num 降序）+ list0 重排
//   - P 帧 slice header 语法（ref_count → reorder → pred_weight 跳过 → MMCO → qp → deblocking）
//
// 范围：H.264 I/P 帧、NV12、8bit、4:2:0、frame_mbs_only=1、poc_type=0/2、
//       Baseline/Main（weighted_pred=0，无长期参考，无场，无 MBAFF）。
//       非目标：B 帧、长期参考、加权预测、场/MBAFF、crop、seek。
//
// 运行（AMD 机器）：
//   make -f verify_decode_to_hip.make
//   make -f verify_decode_to_hip.make run-p   # 生成带 P 帧的码流 + 跑
//
// 依赖：AMD GPU + ROCm/HIP + Mesa radeonsi(libva) + ffmpeg（生成软解参考）。

#include <hip/hip_runtime_api.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <drm/drm_fourcc.h>

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
// 必须定义 HAVE_AV_CONFIG_H，否则 libavutil/common.h 不 include internal.h，
// golomb.h 的 SUINT 未定义会编译失败（见 §4.6 补充第 5 坑）。
#define HAVE_AV_CONFIG_H 1
#include "libavcodec/avcodec.h"
#include "libavcodec/h2645_parse.h"
#include "libavcodec/h264_ps.h"
#include "libavcodec/h264_parse.h"   // ff_h264_init_poc / ff_h264_parse_ref_count
#include "libavcodec/h264.h"
#include "libavcodec/golomb.h"
#include "libavcodec/mpegutils.h"    // PICT_FRAME
}

// VAAPI slice_type 语义：0=P, 1=B, 2=I（ff_h264_get_slice_type）
enum { VA_SLICE_P = 0, VA_SLICE_B = 1, VA_SLICE_I = 2 };

#define PASS(...) do { printf("  [PASS] "); printf(__VA_ARGS__); printf("\n"); g_pass++; } while (0)
#define FAIL(...) do { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); g_fail++; } while (0)
#define INFO(...) do { printf("  [info] "); printf(__VA_ARGS__); printf("\n"); } while (0)

static int g_pass = 0, g_fail = 0;
static const int W = 320, H = 240;

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

// 坑1：VAProfileH264Baseline 已废弃，radeonsi 只暴露 ConstrainedBaseline。
static VAProfile pick_profile(const SPS* sps) {
    switch (sps->profile_idc) {
        case 66:  return VAProfileH264ConstrainedBaseline;
        case 77:  return VAProfileH264Main;
        case 100: return VAProfileH264High;
        default:  return VAProfileH264ConstrainedBaseline;
    }
}

// ---- slice header 解析结果 ----
struct MMCOOp {
    int opcode;         // MMCO_*（h264_parse.h）
    int short_pic_num;  // opcode==SHORT2UNUSED 时有效
    int long_arg;
};

struct SliceInfo {
    int nal_type;
    int nal_ref_idc;
    int slice_type_nos;   // FFmpeg AV_PICTURE_TYPE_I/P/B（1/2/3）
    int va_slice_type;    // VAAPI 0=P 1=B 2=I
    int frame_num;
    int pps_id;
    int field_pic_flag;
    int bottom_field_flag;
    int idr_pic_id;
    // POC 输入（供 ff_h264_init_poc）
    int poc_lsb;
    int delta_poc_bottom;
    int delta_poc[2];
    // B 帧 direct_spatial_mv_pred（在 ref_count 之前解析）
    int direct_spatial_mv_pred;
    // ref count
    int ref_count[2];
    int list_count;
    // ref list reordering
    int nb_ref_modifications[2];
    struct { int op; int val; } ref_modifications[2][32];
    // MMCO
    int nb_mmco;
    MMCOOp mmco[32];
    int explicit_ref_marking;
    // qp / deblocking
    int slice_qp_delta;
    uint8_t disable_deblocking_idc;
    int8_t alpha_div2, beta_div2;
    int cabac_init_idc;
    int bit_offset;
};

// 解析 slice header（I/P 帧，frame_mbs_only=1，poc_type=0/2，weighted_pred=0）
// 顺序严格对齐 FFmpeg h264_slice.c decode_slice_header（§7.3.3）。
static bool parse_slice_header(const H2645NAL* nal, const SPS* sps, const PPS* pps,
                               SliceInfo* si) {
    memset(si, 0, sizeof(*si));
    GetBitContext gb = nal->gb;
    si->nal_type = nal->type;
    si->nal_ref_idc = nal->ref_idc;

    get_ue_golomb_long(&gb);                       // first_mb_in_slice
    unsigned st = get_ue_golomb_31(&gb);           // slice_type
    if (st > 9) return false;
    if (st > 4) st -= 5;                            // slice_type_fixed
    // ff_h264_golomb_to_pict_type: P,B,I,SP,SI → AV_PICTURE_TYPE_P/B/I
    static const int to_pict[5] = { AV_PICTURE_TYPE_P, AV_PICTURE_TYPE_B, AV_PICTURE_TYPE_I,
                                    AV_PICTURE_TYPE_SP, AV_PICTURE_TYPE_SI };
    si->slice_type_nos = to_pict[st] & 3;
    if (si->slice_type_nos == AV_PICTURE_TYPE_I) si->va_slice_type = VA_SLICE_I;
    else if (si->slice_type_nos == AV_PICTURE_TYPE_P) si->va_slice_type = VA_SLICE_P;
    else if (si->slice_type_nos == AV_PICTURE_TYPE_B) si->va_slice_type = VA_SLICE_B;
    else { FAIL("不支持 slice_type_nos=%d（SP/SI）", si->slice_type_nos); return false; }

    si->pps_id = get_ue_golomb(&gb);               // pic_parameter_set_id
    si->frame_num = get_bits(&gb, sps->log2_max_frame_num);

    // frame_mbs_only_flag==1 → 无 field_pic_flag 分支
    if (!sps->frame_mbs_only_flag) { FAIL("不支持 frame_mbs_only_flag=0（场编码）"); return false; }

    if (nal->type == H264_NAL_IDR_SLICE)
        si->idr_pic_id = get_ue_golomb_long(&gb);

    // POC（支持 poc_type==0 和 poc_type==2，均复用 ff_h264_init_poc）
    //   poc_type==0：读 pic_order_cnt_lsb + 可选 delta_poc_bottom
    //   poc_type==2：无语法字段，POC 由 ff_h264_init_poc 按 frame_num 线性推导
    if (sps->poc_type == 0) {
        si->poc_lsb = get_bits(&gb, sps->log2_max_poc_lsb);
        if (pps->pic_order_present == 1)
            si->delta_poc_bottom = get_se_golomb(&gb);
    } else if (sps->poc_type != 2) {
        FAIL("仅支持 poc_type=0/2（当前 poc_type=%d）", sps->poc_type);
        return false;
    }

    if (pps->redundant_pic_cnt_present)
        get_ue_golomb(&gb);                        // redundant_pic_cnt（丢弃）

    // B 帧特有：direct_spatial_mv_pred（在 ref_count 之前解析）
    if (si->slice_type_nos == AV_PICTURE_TYPE_B)
        si->direct_spatial_mv_pred = get_bits1(&gb);

    // 非 I 帧：ref_count 语法（复用 FFmpeg）
    if (si->slice_type_nos != AV_PICTURE_TYPE_I) {
        int ret = ff_h264_parse_ref_count(&si->list_count, si->ref_count,
                                          &gb, pps, si->slice_type_nos, PICT_FRAME, nullptr);
        if (ret < 0) { FAIL("ff_h264_parse_ref_count 失败"); return false; }

        // ref_pic_list_reordering（手写，对齐 ff_h264_decode_ref_pic_list_reordering）
        for (int list = 0; list < si->list_count; list++) {
            if (!get_bits1(&gb)) continue;         // ref_pic_list_modification_flag_l0/1
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

        // pred_weight_table：weighted_pred / weighted_bipred_idc==1 时才解析
        // （本程序不支持加权预测，P 帧 weighted_pred=1、B 帧 weighted_bipred_idc=1 时报错）
        if ((pps->weighted_pred && si->slice_type_nos == AV_PICTURE_TYPE_P) ||
            (pps->weighted_bipred_idc == 1 && si->slice_type_nos == AV_PICTURE_TYPE_B)) {
            FAIL("不支持加权预测（weighted_pred=%d weighted_bipred_idc=%d）",
                 pps->weighted_pred, pps->weighted_bipred_idc);
            return false;
        }
    }

    // dec_ref_pic_marking（手写，对齐 ff_h264_decode_ref_pic_marking）
    if (nal->ref_idc) {
        if (nal->type == H264_NAL_IDR_SLICE) {
            skip_bits1(&gb);                        // no_output_of_prior_pics_flag
            if (get_bits1(&gb)) {                   // long_term_reference_flag
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
                            (si->frame_num - get_ue_golomb_long(&gb) - 1) & ((1 << sps->log2_max_frame_num) - 1);
                    } else {
                        FAIL("仅支持 MMCO_SHORT2UNUSED（遇到 opcode=%u）", op);
                        return false;
                    }
                    si->nb_mmco = i + 1;
                }
            }
        }
    }

    // cabac_init_idc（P 帧 cabac 时）
    if (si->slice_type_nos != AV_PICTURE_TYPE_I && pps->cabac) {
        unsigned tmp = get_ue_golomb_31(&gb);
        if (tmp > 2) return false;
        si->cabac_init_idc = tmp;
    }

    // slice_qp_delta
    si->slice_qp_delta = get_se_golomb(&gb);

    // deblocking
    if (pps->deblocking_filter_parameters_present) {
        unsigned idc = get_ue_golomb_31(&gb);
        if (idc > 2) return false;
        si->disable_deblocking_idc = (uint8_t)idc;
        if (idc != 1) {
            si->alpha_div2 = (int8_t)get_se_golomb(&gb);
            si->beta_div2  = (int8_t)get_se_golomb(&gb);
        }
    }

    si->bit_offset = get_bits_count(&gb);
    return true;
}

// ---- 最小 DPB 状态机 ----
struct DpbEntry {
    VASurfaceID surface;
    int frame_num;
    int poc;             // pic_poc（ff_h264_init_poc 输出）
    int field_poc[2];
};

// ---- 解码帧（HIP 读回 host 后缓存，供显示序重排比对）----
struct DecodedFrame {
    int poc;
    int frame_num;
    int slice_type;      // VA_SLICE_I/P/B
    std::vector<uint8_t> y, uv;
};

// fd 计数（参考 verify_sync.cpp S8）
static int count_fds() {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0; while (readdir(d)) n++;
    closedir(d);
    return n - 2;
}

int main(int argc, char** argv) {
    printf("===========================================================\n");
    printf(" 实验 C（I/P 帧）：真实解码 → HIP 读，端到端像素校验\n");
    printf("===========================================================\n");

    if (argc < 3) { fprintf(stderr, "用法: %s <input.h264> <ref.yuv>\n", argv[0]); return 2; }

    std::vector<uint8_t> buf;
    if (!read_file(argv[1], buf)) return 1;

    // ---- 1. 打开 VAAPI ----
    int drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (drm_fd < 0) { FAIL("open renderD128: %s", strerror(errno)); return 1; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    int ma=0, mi=0;
    if (vaInitialize(dpy, &ma, &mi) != VA_STATUS_SUCCESS) { FAIL("vaInitialize"); close(drm_fd); return 1; }
    PASS("VAAPI 初始化 (libva %d.%d)", ma, mi);

    // ---- 2. 复用 FFmpeg parser 切 NAL ----
    H2645Packet pkt; memset(&pkt, 0, sizeof(pkt));
    int ret = ff_h2645_packet_split(&pkt, buf.data(), (int)buf.size(),
                                    nullptr, 0, (enum AVCodecID)AV_CODEC_ID_H264, 0);
    if (ret < 0) { FAIL("ff_h2645_packet_split: %d", ret); vaTerminate(dpy); close(drm_fd); return 1; }

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
    if (!sps || !pps) { FAIL("未取到 SPS/PPS"); ff_h2645_packet_uninit(&pkt); vaTerminate(dpy); close(drm_fd); return 1; }
    PASS("解析 SPS/PPS 成功");

    int disp_w, disp_h;
    sps_dimensions(sps, &disp_w, &disp_h);
    INFO("SPS: profile=%d %dx%d bit_depth=%d chroma=%d cabac=%d poc_type=%d ref_frames=%d",
         sps->profile_idc, disp_w, disp_h, sps->bit_depth_luma, sps->chroma_format_idc,
         pps->cabac, sps->poc_type, sps->ref_frame_count);

    // ---- 3. 收集所有 slice NAL（按解码顺序）----
    std::vector<H2645NAL*> slices;
    for (int i = 0; i < pkt.nb_nals; i++)
        if (pkt.nals[i].type == H264_NAL_IDR_SLICE || pkt.nals[i].type == H264_NAL_SLICE)
            slices.push_back(&pkt.nals[i]);
    if (slices.empty()) { FAIL("码流无 slice"); ff_h2645_packet_uninit(&pkt); vaTerminate(dpy); close(drm_fd); return 1; }
    int n_frames = (int)slices.size();
    INFO("检测到 %d 个 slice（I/P）", n_frames);

    // ---- 4. 创建 LINEAR NV12 surface 池（尊重 DPB 复用约束）----
    uint64_t mods[] = { DRM_FORMAT_MOD_LINEAR };
    VADRMFormatModifierList mod_list = { 1, mods };
    VASurfaceAttrib sattribs[2];
    sattribs[0].type = VASurfaceAttribMemoryType;
    sattribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    sattribs[0].value.type = VAGenericValueTypeInteger;
    sattribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    sattribs[1].type = VASurfaceAttribDRMFormatModifiers;
    sattribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    sattribs[1].value.type = VAGenericValueTypePointer;
    sattribs[1].value.value.p = &mod_list;

    // surface 池大小 = max_num_ref_frames + 3（DPB + 当前 + 余量，参考 §4.1 附带收获②）
    int num_surfs = sps->ref_frame_count + 3;
    if (num_surfs < 4) num_surfs = 4;
    std::vector<VASurfaceID> surfaces(num_surfs, VA_INVALID_ID);
    VAStatus vs = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H,
                                   surfaces.data(), num_surfs, sattribs, 2);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaCreateSurfaces(LINEAR): %d (%s)", vs, vaErrorStr(vs)); return 1; }
    PASS("创建 %d 个 LINEAR NV12 surface（DPB 复用，max_ref=%d）", num_surfs, sps->ref_frame_count);

    // 空闲 surface 队列（先进先出，与 DPB 隔离）
    std::vector<VASurfaceID> free_surfs(surfaces.begin(), surfaces.end());

    // ---- 5. 创建 config / context ----
    VAConfigAttrib cfg_attr; cfg_attr.type = VAConfigAttribRTFormat;
    vaGetConfigAttributes(dpy, pick_profile(sps), VAEntrypointVLD, &cfg_attr, 1);
    VAConfigID config;
    if (vaCreateConfig(dpy, pick_profile(sps), VAEntrypointVLD, &cfg_attr, 1, &config) != VA_STATUS_SUCCESS) {
        FAIL("vaCreateConfig(profile=%d)", pick_profile(sps)); return 1;
    }
    VAContextID ctx;
    if (vaCreateContext(dpy, config, W, H, VA_PROGRESSIVE, surfaces.data(), num_surfs, &ctx) != VA_STATUS_SUCCESS) {
        FAIL("vaCreateContext"); return 1;
    }
    PASS("vaCreateConfig/Context 成功 (profile=%d)", pick_profile(sps));

    // ---- 6. 读软解参考（N 帧连续 NV12，按解码顺序）----
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

    int fd_before = count_fds();
    INFO("初始 fd 数 = %d", fd_before);

    // ---- 7. 逐帧解码 → HIP 读 → 收集（显示序重排后统一比对）----
    std::vector<DpbEntry> dpb;      // 按 frame_num 降序（新在前）
    H264POCContext poc_ctx; memset(&poc_ctx, 0, sizeof(poc_ctx));
    poc_ctx.prev_poc_lsb = -1; poc_ctx.prev_poc_msb = 0;
    std::vector<DecodedFrame> decoded;  // 解码序收集，最后按 POC 重排比对
    int all_ok = 1;

    for (int f = 0; f < n_frames; f++) {
        SliceInfo si;
        if (!parse_slice_header(slices[f], sps, pps, &si)) {
            FAIL("帧 %d slice header 解析失败", f); all_ok = 0; break;
        }
        H2645NAL* slice_nal = slices[f];

        // ---- 7a. 分配目标 surface（从空闲池取）----
        if (free_surfs.empty()) { FAIL("帧 %d 无空闲 surface（DPB 复用不足）", f); all_ok = 0; break; }
        VASurfaceID target = free_surfs.back();
        free_surfs.pop_back();

        // ---- 7b. POC 计算（复用 ff_h264_init_poc）----
        poc_ctx.frame_num = si.frame_num;
        poc_ctx.poc_lsb = si.poc_lsb;
        poc_ctx.delta_poc_bottom = si.delta_poc_bottom;
        poc_ctx.delta_poc[0] = si.delta_poc[0];
        poc_ctx.delta_poc[1] = si.delta_poc[1];
        int field_poc[2], pic_poc;
        if (ff_h264_init_poc(field_poc, &pic_poc, sps, &poc_ctx, PICT_FRAME, si.nal_ref_idc) < 0) {
            FAIL("帧 %d ff_h264_init_poc 失败", f); all_ok = 0; break;
        }
        // 更新 prev 字段（对齐 h264_slice.c h264_field_start 452-458）
        // 坑3：非参考帧（B 帧 nal_ref_idc==0，即 FFmpeg 的 droppable）不更新 prev_poc_*，
        //      否则 B 帧的 poc_lsb 会污染下一个参考帧的 poc_msb 回绕判断（POC 错跳 65536）。
        if (si.nal_ref_idc != 0) {
            poc_ctx.prev_poc_msb = poc_ctx.poc_msb;
            poc_ctx.prev_poc_lsb = poc_ctx.poc_lsb;
        }
        poc_ctx.prev_frame_num = poc_ctx.frame_num;
        // 关键：frame_num_offset 回绕由 ff_h264_init_poc 内部计算，这里同步 prev
        //（对齐 h264_slice.c:458 `prev_frame_num_offset = frame_num_offset`）
        poc_ctx.prev_frame_num_offset = poc_ctx.frame_num_offset;

        // ---- 7c. 构建 RefPicList0/1（默认 + reorder）----
        // P 帧：list0 = dpb 顺序（frame_num 降序）
        // B 帧：list0 = [POC<cur 降序] + [POC>cur 升序]；list1 反之（对齐 h264_initialise_ref_list）
        std::vector<int> list0, list1;
        if (si.slice_type_nos == AV_PICTURE_TYPE_B) {
            int cur_poc = pic_poc;
            // list0：先 POC < cur（降序，即从近到远），再 POC > cur（升序，从近到远）
            std::vector<const DpbEntry*> lt, gt;
            for (auto& e : dpb) { if (e.poc < cur_poc) lt.push_back(&e); else gt.push_back(&e); }
            std::sort(lt.begin(), lt.end(), [](const DpbEntry* a, const DpbEntry* b){ return a->poc > b->poc; });
            std::sort(gt.begin(), gt.end(), [](const DpbEntry* a, const DpbEntry* b){ return a->poc < b->poc; });
            for (auto* p : lt) list0.push_back(p->frame_num);
            for (auto* p : gt) list0.push_back(p->frame_num);
            // list1：先 POC > cur（升序），再 POC < cur（降序）
            for (auto* p : gt) list1.push_back(p->frame_num);
            for (auto* p : lt) list1.push_back(p->frame_num);
        } else {
            for (auto& e : dpb) list0.push_back(e.frame_num);
        }

        // reorder（对齐 build_ref_list 后半段，P/B 均按 frame_num 匹配）
        int max_pic_num = 1 << sps->log2_max_frame_num;
        for (int list = 0; list < si.list_count; list++) {
            std::vector<int>& L = (list == 0) ? list0 : list1;
            for (int idx = 0; idx < si.nb_ref_modifications[list]; idx++) {
                int op = si.ref_modifications[list][idx].op;
                int val = si.ref_modifications[list][idx].val;
                int pred = si.frame_num;
                if (op == 0) pred -= (val + 1);
                else if (op == 1) pred += (val + 1);
                pred &= (max_pic_num - 1);
                for (int k = idx; k < (int)L.size(); k++) {
                    if (L[k] == pred) {
                        int v = L[k];
                        L.erase(L.begin() + k);
                        L.insert(L.begin() + idx, v);
                        break;
                    }
                }
            }
        }

        // ---- 7d. 填 VAPictureParameterBufferH264（CurrPic + DPB ReferenceFrames）----
        VAPictureParameterBufferH264 pp = {};
        pp.CurrPic.picture_id = target;
        pp.CurrPic.frame_idx = si.frame_num;
        pp.CurrPic.flags = 0;
        pp.CurrPic.TopFieldOrderCnt = field_poc[0];
        pp.CurrPic.BottomFieldOrderCnt = field_poc[1];
        for (int i = 0; i < 16; i++) pp.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        int nb_dpb = std::min((int)dpb.size(), 16);
        for (int i = 0; i < nb_dpb; i++) {
            pp.ReferenceFrames[i].picture_id = dpb[i].surface;
            pp.ReferenceFrames[i].frame_idx = dpb[i].frame_num;
            pp.ReferenceFrames[i].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
            pp.ReferenceFrames[i].TopFieldOrderCnt = dpb[i].field_poc[0];
            pp.ReferenceFrames[i].BottomFieldOrderCnt = dpb[i].field_poc[1];
        }

        pp.picture_width_in_mbs_minus1  = (uint16_t)(sps->mb_width - 1);
        pp.picture_height_in_mbs_minus1 = (uint16_t)(sps->mb_height - 1);
        pp.bit_depth_luma_minus8   = (uint8_t)(sps->bit_depth_luma - 8);
        pp.bit_depth_chroma_minus8 = (uint8_t)(sps->bit_depth_chroma - 8);
        pp.num_ref_frames          = (uint8_t)sps->ref_frame_count;
        pp.seq_fields.bits.chroma_format_idc = sps->chroma_format_idc;
        pp.seq_fields.bits.frame_mbs_only_flag = sps->frame_mbs_only_flag;
        pp.seq_fields.bits.mb_adaptive_frame_field_flag = sps->mb_aff;
        pp.seq_fields.bits.direct_8x8_inference_flag = sps->direct_8x8_inference_flag;
        pp.seq_fields.bits.log2_max_frame_num_minus4 = sps->log2_max_frame_num - 4;
        pp.seq_fields.bits.pic_order_cnt_type = sps->poc_type;
        pp.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_poc_lsb - 4;
        pp.seq_fields.bits.delta_pic_order_always_zero_flag = sps->delta_pic_order_always_zero_flag;
        pp.pic_fields.bits.entropy_coding_mode_flag = pps->cabac;
        pp.pic_fields.bits.weighted_pred_flag = pps->weighted_pred;
        pp.pic_fields.bits.weighted_bipred_idc = pps->weighted_bipred_idc;
        pp.pic_fields.bits.transform_8x8_mode_flag = pps->transform_8x8_mode;
        pp.pic_fields.bits.constrained_intra_pred_flag = pps->constrained_intra_pred;
        pp.pic_fields.bits.pic_order_present_flag = pps->pic_order_present;
        pp.pic_fields.bits.deblocking_filter_control_present_flag = pps->deblocking_filter_parameters_present;
        pp.pic_fields.bits.redundant_pic_cnt_present_flag = pps->redundant_pic_cnt_present;
        pp.pic_fields.bits.reference_pic_flag = (si.nal_ref_idc != 0);
        pp.pic_init_qp_minus26 = (int8_t)(pps->init_qp - 26);
        pp.pic_init_qs_minus26 = (int8_t)(pps->init_qs - 26);
        pp.chroma_qp_index_offset = (int8_t)pps->chroma_qp_index_offset[0];
        pp.second_chroma_qp_index_offset = (int8_t)pps->chroma_qp_index_offset[1];
        pp.frame_num = si.frame_num;

        VAIQMatrixBufferH264 iq = {};
        memcpy(iq.ScalingList4x4, pps->scaling_matrix4, sizeof(iq.ScalingList4x4));
        memcpy(iq.ScalingList8x8[0], pps->scaling_matrix8[0], sizeof(iq.ScalingList8x8[0]));
        memcpy(iq.ScalingList8x8[1], pps->scaling_matrix8[3], sizeof(iq.ScalingList8x8[0]));

        VASliceParameterBufferH264 sp = {};
        sp.slice_data_size = slice_nal->raw_size;
        sp.slice_data_offset = 0;
        sp.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        sp.slice_data_bit_offset = (uint16_t)si.bit_offset;
        sp.first_mb_in_slice = 0;
        sp.slice_type = si.va_slice_type;
        sp.direct_spatial_mv_pred_flag = (si.slice_type_nos == AV_PICTURE_TYPE_B) ? si.direct_spatial_mv_pred : 0;
        sp.num_ref_idx_l0_active_minus1 = (si.list_count > 0) ? (si.ref_count[0] - 1) : 0;
        sp.num_ref_idx_l1_active_minus1 = (si.list_count > 1) ? (si.ref_count[1] - 1) : 0;
        sp.cabac_init_idc = si.cabac_init_idc;
        sp.slice_qp_delta = (int8_t)si.slice_qp_delta;
        sp.disable_deblocking_filter_idc = si.disable_deblocking_idc;
        sp.slice_alpha_c0_offset_div2 = si.alpha_div2;
        sp.slice_beta_offset_div2 = si.beta_div2;
        for (int i = 0; i < 32; i++) { sp.RefPicList0[i].picture_id = VA_INVALID_SURFACE; sp.RefPicList1[i].picture_id = VA_INVALID_SURFACE; }
        // 填 RefPicList0/1（frame_num 匹配 dpb，含 B 帧的 list1）
        auto fill_list = [&](VAPictureH264* dst, const std::vector<int>& L, int n) {
            int nb = std::min((int)L.size(), std::min(n, 32));
            for (int i = 0; i < nb; i++) {
                int fn = L[i];
                for (auto& e : dpb) {
                    if (e.frame_num == fn) {
                        dst[i].picture_id = e.surface;
                        dst[i].frame_idx = e.frame_num;
                        dst[i].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
                        dst[i].TopFieldOrderCnt = e.field_poc[0];
                        dst[i].BottomFieldOrderCnt = e.field_poc[1];
                        break;
                    }
                }
            }
        };
        fill_list(sp.RefPicList0, list0, si.ref_count[0]);
        fill_list(sp.RefPicList1, list1, si.ref_count[1]);

        VABufferID pp_buf, iq_buf, sp_buf, sd_buf;
        if (vaCreateBuffer(dpy, ctx, VAPictureParameterBufferType, sizeof(pp), 1, &pp, &pp_buf) != VA_STATUS_SUCCESS) { FAIL("帧 %d vaCreateBuffer(PP)", f); all_ok = 0; break; }
        if (vaCreateBuffer(dpy, ctx, VAIQMatrixBufferType, sizeof(iq), 1, &iq, &iq_buf) != VA_STATUS_SUCCESS) { FAIL("帧 %d vaCreateBuffer(IQ)", f); all_ok = 0; break; }
        if (vaCreateBuffer(dpy, ctx, VASliceParameterBufferType, sizeof(sp), 1, &sp, &sp_buf) != VA_STATUS_SUCCESS) { FAIL("帧 %d vaCreateBuffer(SP)", f); all_ok = 0; break; }
        if (vaCreateBuffer(dpy, ctx, VASliceDataBufferType, slice_nal->raw_size, 1, (void*)slice_nal->raw_data, &sd_buf) != VA_STATUS_SUCCESS) { FAIL("帧 %d vaCreateBuffer(SD)", f); all_ok = 0; break; }

        vs = vaBeginPicture(dpy, ctx, target);
        if (vs != VA_STATUS_SUCCESS) { FAIL("帧 %d vaBeginPicture: %d (%s)", f, vs, vaErrorStr(vs)); all_ok = 0; break; }
        VABufferID bufs[] = { pp_buf, iq_buf, sp_buf, sd_buf };
        for (VABufferID b : bufs) {
            vs = vaRenderPicture(dpy, ctx, &b, 1);
            if (vs != VA_STATUS_SUCCESS) { FAIL("帧 %d vaRenderPicture: %d (%s)", f, vs, vaErrorStr(vs)); all_ok = 0; break; }
        }
        if (vs != VA_STATUS_SUCCESS) { all_ok = 0; break; }
        vs = vaEndPicture(dpy, ctx);
        if (vs != VA_STATUS_SUCCESS) { FAIL("帧 %d vaEndPicture: %d (%s)", f, vs, vaErrorStr(vs)); all_ok = 0; break; }
        vs = vaSyncSurface(dpy, target);
        if (vs != VA_STATUS_SUCCESS) { FAIL("帧 %d vaSyncSurface: %d (%s)", f, vs, vaErrorStr(vs)); all_ok = 0; break; }

        // ---- 7e. 导出 → HIP 读 → 比对 ----
        VADRMPRIMESurfaceDescriptor desc; memset(&desc, 0, sizeof(desc));
        vs = vaExportSurfaceHandle(dpy, target, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
        if (vs != VA_STATUS_SUCCESS) {
            vs = vaExportSurfaceHandle(dpy, target, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                       VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &desc);
        }
        if (vs != VA_STATUS_SUCCESS) { FAIL("帧 %d vaExportSurfaceHandle: %d (%s)", f, vs, vaErrorStr(vs)); all_ok = 0; break; }

        uint32_t y_pitch, uv_pitch, y_off, uv_off;
        int y_obj = 0, uv_obj = 0;
        if (desc.num_layers >= 2) {
            y_pitch  = desc.layers[0].pitch[0]; y_off = desc.layers[0].offset[0];
            uv_pitch = desc.layers[1].pitch[0]; uv_off = desc.layers[1].offset[0];
            y_obj = desc.layers[0].object_index[0];
            uv_obj = desc.layers[1].object_index[0];
        } else {
            y_pitch  = desc.layers[0].pitch[0]; y_off = desc.layers[0].offset[0];
            uv_pitch = desc.layers[0].pitch[1]; uv_off = desc.layers[0].offset[1];
            y_obj = desc.layers[0].object_index[0];
            uv_obj = desc.layers[0].object_index[1];
        }
        if (f == 0)
            INFO("布局: layers=%u Y pitch=%u off=%u  UV pitch=%u off=%u",
                 desc.num_layers, y_pitch, y_off, uv_pitch, uv_off);

        hipInit(0);
        std::vector<hipExternalMemory_t> extMems(desc.num_objects, nullptr);
        std::vector<void*> devPtrs(desc.num_objects, nullptr);
        bool import_ok = true;
        for (uint32_t o = 0; o < desc.num_objects && import_ok; o++) {
            hipExternalMemoryHandleDesc hd = {};
            hd.type = hipExternalMemoryHandleTypeOpaqueFd;
            hd.handle.fd = desc.objects[o].fd;
            hd.size = desc.objects[o].size;
            if (hipImportExternalMemory(&extMems[o], &hd) != hipSuccess) { FAIL("帧 %d hipImportExternalMemory obj%u", f, o); import_ok = false; break; }
            hipExternalMemoryBufferDesc bd = {}; bd.size = desc.objects[o].size;
            if (hipExternalMemoryGetMappedBuffer(&devPtrs[o], extMems[o], &bd) != hipSuccess) { FAIL("帧 %d GetMappedBuffer obj%u", f, o); import_ok = false; break; }
        }
        if (!import_ok) { for (auto e : extMems) if (e) hipDestroyExternalMemory(e); all_ok = 0; break; }

        std::vector<uint8_t> y(W * H), uv(W * H / 2);
        char* ybase = (char*)devPtrs[y_obj] + y_off;
        char* uvbase = (char*)devPtrs[uv_obj] + uv_off;
        for (int r = 0; r < H; r++)
            hipMemcpy(y.data() + r * W, ybase + (size_t)r * y_pitch, W, hipMemcpyDeviceToHost);
        for (int r = 0; r < H / 2; r++)
            hipMemcpy(uv.data() + r * W, uvbase + (size_t)r * uv_pitch, W, hipMemcpyDeviceToHost);

        for (auto e : extMems) if (e) hipDestroyExternalMemory(e);
        for (uint32_t o = 0; o < desc.num_objects; o++) close(desc.objects[o].fd);
        vaDestroyBuffer(dpy, pp_buf); vaDestroyBuffer(dpy, iq_buf);
        vaDestroyBuffer(dpy, sp_buf); vaDestroyBuffer(dpy, sd_buf);

        // 收集到 decoded（显示序重排统一比对）
        DecodedFrame df;
        df.poc = pic_poc;
        df.frame_num = si.frame_num;
        df.slice_type = si.va_slice_type;
        df.y = std::move(y);
        df.uv = std::move(uv);
        decoded.push_back(std::move(df));

        // ---- 7f. DPB 更新（MMCO 执行 + 滑动窗口 + 加入当前帧）----
        // IDR：清空整个 DPB + 重置 POC prev（对齐 FFmpeg idr() / ff_h264_remove_all_refs）
        if (si.nal_type == H264_NAL_IDR_SLICE) {
            for (auto& e : dpb) free_surfs.push_back(e.surface);
            dpb.clear();
            poc_ctx.prev_frame_num = 0;
            poc_ctx.prev_frame_num_offset = 0;
            // 坑4：prev_poc_msb 必须回到 0，不能用 FFmpeg 的 1<<16 哨兵值。
            //      FFmpeg 在 idr() 里设 1<<16 是因为它先 reset 再算 IDR 自己的 POC，
            //      所有帧统一偏移 65536（相对排序不变）；本程序 reset 发生在 IDR 之后，
            //      若用 1<<16 会使 IDR=0 而后续帧=65536+，偏移不一致导致显示序错乱。
            poc_ctx.prev_poc_msb = 0;
            poc_ctx.prev_poc_lsb = -1;
        } else if (si.nal_ref_idc != 0) {
            // 参考帧才执行 DPB 标记（H.264 §8.2.5.1）；非参考帧（B 帧 nal_ref_idc==0）不参与，
            // 否则 B 帧解码会误删 DPB 里的参考帧，导致下一个 B 帧缺参考。
            // 1) 显式 MMCO：SHORT2UNUSED 移除指定 frame_num
            for (int i = 0; i < si.nb_mmco; i++) {
                if (si.mmco[i].opcode == MMCO_SHORT2UNUSED) {
                    int fn = si.mmco[i].short_pic_num;
                    for (auto it = dpb.begin(); it != dpb.end(); ++it) {
                        if (it->frame_num == fn) {
                            free_surfs.push_back(it->surface);
                            dpb.erase(it);
                            break;
                        }
                    }
                }
            }
            // 2) 滑动窗口：若无显式 MMCO 且 DPB 满，移除最旧（frame_num 最小的末尾项）
            if (!si.explicit_ref_marking && (int)dpb.size() >= sps->ref_frame_count && !dpb.empty()) {
                free_surfs.push_back(dpb.back().surface);
                dpb.pop_back();
            }
        }
        // 3) 当前帧若为参考帧（nal_ref_idc != 0），插入 dpb 前端
        if (si.nal_ref_idc != 0) {
            DpbEntry e;
            e.surface = target;
            e.frame_num = si.frame_num;
            e.poc = pic_poc;
            e.field_poc[0] = field_poc[0];
            e.field_poc[1] = field_poc[1];
            dpb.insert(dpb.begin(), e);
        } else {
            // 非参考帧：解码完直接放回空闲池
            free_surfs.push_back(target);
        }
    }

    // ---- 7g. 显示序重排：按 POC 排序后与显示序参考逐帧比对 ----
    if (all_ok && (int)decoded.size() == n_frames) {
        // 稳定排序：按 POC 升序（显示序），POC 相同时按解码序稳定
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
                PASS("显示序帧 %d/%d (poc=%d fn=%d type=%d) 逐字节一致（%zu 字节）",
                     i, n_frames, decoded[i].poc, decoded[i].frame_num, decoded[i].slice_type,
                     decoded[i].y.size() + decoded[i].uv.size());
            }
        }
        if (n_bad == 0) PASS("显示序重排比对通过：%d 帧全部逐字节一致", n_frames);
        else { all_ok = 0; }
    } else if (all_ok) {
        FAIL("解码帧数 %d != 期望 %d", (int)decoded.size(), n_frames);
        all_ok = 0;
    }

    // ---- 8. flush 幂等检查 ----
    if (all_ok) {
        int flush_fail = 0;
        for (int s = 0; s < num_surfs; s++) {
            vs = vaSyncSurface(dpy, surfaces[s]);
            if (vs != VA_STATUS_SUCCESS) { FAIL("flush vaSyncSurface[%d]: %d", s, vs); flush_fail = 1; }
        }
        if (!flush_fail) PASS("flush 幂等检查通过（%d 个 surface）", num_surfs);
    }

    // ---- 9. fd 泄漏检查 ----
    int fd_after = count_fds();
    INFO("最终 fd 数 = %d（初始 %d）", fd_after, fd_before);
    if (fd_after <= fd_before + 5) PASS("fd 无泄漏（初始=%d 最终=%d）", fd_before, fd_after);
    else FAIL("fd 泄漏（初始=%d 最终=%d）", fd_before, fd_after);

    ff_h2645_packet_uninit(&pkt);
    ff_h264_ps_uninit(&ps);
    vaDestroySurfaces(dpy, surfaces.data(), num_surfs);
    vaDestroyContext(dpy, ctx);
    vaDestroyConfig(dpy, config);
    vaTerminate(dpy);
    close(drm_fd);

    printf("\n========== 汇总 ==========\n");
    printf("  PASS: %d   FAIL: %d   （解码 %d 帧）\n", g_pass, g_fail, n_frames);
    if (g_fail == 0) printf("  >>> 实验 C（I/P 帧）通过：真实解码→HIP 读逐字节一致 + DPB 复用 + flush + 无泄漏\n");
    else printf("  >>> 存在失败项，见上方 FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
