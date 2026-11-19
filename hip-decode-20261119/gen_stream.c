// gen_stream.c — generate an H.264 Annex-B test stream with libx264.
//
// The demo in this directory validates Vulkan Video decode against a software
// reference, so it needs streams that exercise more of H.264 than a single
// hand-written fixture: profiles, chroma formats, slice counts, reference
// counts, B-pyramids and so on. Using libx264 directly (instead of ffmpeg)
// keeps the matrix reproducible even when the local ffmpeg build has no
// libx264 encoder.
//
// Build: gcc -O2 gen_stream.c -o gen_stream -lx264
// Usage: ./gen_stream out.h264 --width 320 --height 240 --frames 24 --profile high
//
// Exit code: 0 = written, 2 = unsupported request (e.g. 10-bit input with an
// 8-bit libx264), 1 = other error.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <x264.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s out.h264 [options]\n"
            "  --preset NAME (x264 preset, default medium)\n"
            "  --width N --height N --frames N\n"
            "  --profile baseline|main|high|high422|high444|high10\n"
            "  --chroma 420|422|444 --bitdepth 8|10\n"
            "  --bframes N --refs N --bpyramid N --slices N\n"
            "  --cabac 0|1 --weightp 0|1|2 --weightb 0|1 --8x8dct 0|1\n"
            "  --deblock 0|1 --intra-refresh 0|1 --aud 0|1 --open-gop 0|1\n"
            "  --keyint N --qp N\n",
            argv0);
}

static int arg_int(int argc, char **argv, int *i, int fallback) {
    if (*i + 1 >= argc) return fallback;
    return atoi(argv[++(*i)]);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *out_path = argv[1];

    int width = 320, height = 240, frames = 24;
    int chroma = 420, bitdepth = 8;
    int bframes = 3, refs = 4, bpyramid = 1, slices = 1;
    int cabac = 1, weightp = 1, weightb = 1, t8x8 = 1;
    int deblock = 1, intra_refresh = 0, aud = 0, open_gop = 0;
    int keyint = 12, qp = 20;
    const char *profile = NULL;
    const char *preset = "medium";

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--width")) width = arg_int(argc, argv, &i, width);
        else if (!strcmp(a, "--height")) height = arg_int(argc, argv, &i, height);
        else if (!strcmp(a, "--frames")) frames = arg_int(argc, argv, &i, frames);
        else if (!strcmp(a, "--profile")) profile = argv[++i];
        else if (!strcmp(a, "--chroma")) chroma = atoi(argv[++i]);
        else if (!strcmp(a, "--bitdepth")) bitdepth = arg_int(argc, argv, &i, bitdepth);
        else if (!strcmp(a, "--bframes")) bframes = arg_int(argc, argv, &i, bframes);
        else if (!strcmp(a, "--refs")) refs = arg_int(argc, argv, &i, refs);
        else if (!strcmp(a, "--bpyramid")) bpyramid = arg_int(argc, argv, &i, bpyramid);
        else if (!strcmp(a, "--slices")) slices = arg_int(argc, argv, &i, slices);
        else if (!strcmp(a, "--cabac")) cabac = arg_int(argc, argv, &i, cabac);
        else if (!strcmp(a, "--weightp")) weightp = arg_int(argc, argv, &i, weightp);
        else if (!strcmp(a, "--weightb")) weightb = arg_int(argc, argv, &i, weightb);
        else if (!strcmp(a, "--8x8dct")) t8x8 = arg_int(argc, argv, &i, t8x8);
        else if (!strcmp(a, "--deblock")) deblock = arg_int(argc, argv, &i, deblock);
        else if (!strcmp(a, "--intra-refresh")) intra_refresh = arg_int(argc, argv, &i, intra_refresh);
        else if (!strcmp(a, "--aud")) aud = arg_int(argc, argv, &i, aud);
        else if (!strcmp(a, "--open-gop")) open_gop = arg_int(argc, argv, &i, open_gop);
        else if (!strcmp(a, "--keyint")) keyint = arg_int(argc, argv, &i, keyint);
        else if (!strcmp(a, "--qp")) qp = arg_int(argc, argv, &i, qp);
        else if (!strcmp(a, "--preset")) preset = argv[++i];
        else { usage(argv[0]); return 1; }
    }
    if (!profile) profile = "high";
    if (width < 64 || height < 64 || frames <= 0) {
        fprintf(stderr, "gen_stream: width/height must be >= 64\n");
        return 2;
    }

    x264_param_t p;
    if (x264_param_default_preset(&p, preset, "psnr") < 0) return 1;
    p.i_csp = (chroma == 422 ? X264_CSP_I422 : chroma == 444 ? X264_CSP_I444 : X264_CSP_I420);
    if (bitdepth > 8) p.i_csp |= X264_CSP_HIGH_DEPTH;
    p.i_width = width;
    p.i_height = height;
    p.i_fps_num = 30;
    p.i_fps_den = 1;
    p.i_frame_total = frames;
    p.i_keyint_max = keyint;
    p.i_keyint_min = keyint;
    p.i_bframe = bframes;
    p.i_bframe_pyramid = bframes > 0 ? bpyramid : 0;
    p.i_frame_reference = refs;
    p.i_slice_count = slices;
    p.b_cabac = cabac;
    p.analyse.i_weighted_pred = weightp;
    p.analyse.b_weighted_bipred = weightb;
    p.analyse.b_transform_8x8 = t8x8;
    p.b_deblocking_filter = deblock;
    p.b_intra_refresh = intra_refresh;
    p.b_aud = aud;
    p.b_open_gop = open_gop;
    p.b_repeat_headers = 1;
    p.b_annexb = 1;
    p.rc.i_rc_method = X264_RC_CQP;
    p.rc.i_qp_constant = qp;
    p.i_log_level = X264_LOG_WARNING;

    if (x264_param_apply_profile(&p, profile) < 0) {
        fprintf(stderr, "gen_stream: profile '%s' not accepted\n", profile);
        return 2;
    }

    x264_t *enc = x264_encoder_open(&p);
    if (!enc) {
        fprintf(stderr, "gen_stream: x264_encoder_open failed (bit depth/chroma not built in)\n");
        return 2;
    }
    x264_param_t actual;
    x264_encoder_parameters(enc, &actual);
    if (bitdepth > 8 && actual.i_bitdepth <= 8) {
        fprintf(stderr, "gen_stream: 10-bit input needs a high-depth libx264 build "
                        "(this one produces %d-bit)\n", actual.i_bitdepth);
        x264_encoder_close(enc);
        return 2;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror(out_path); x264_encoder_close(enc); return 1; }

    x264_picture_t pic;
    x264_picture_t pic_out;
    if (x264_picture_alloc(&pic, p.i_csp, width, height) < 0) {
        fclose(out); x264_encoder_close(enc); return 1;
    }
    const int maxval = (1 << bitdepth) - 1;
    const int cw = (chroma == 444) ? width : width / 2;
    const int ch = (chroma == 420) ? height / 2 : height;

    int wrote = 0;
    x264_nal_t *nals = NULL;
    int nnal = 0;
    int ret = 0;

    for (int f = 0; f < frames && ret == 0; f++) {
        // Deterministic moving pattern: a gradient plus a bright square.
        // x264_picture_alloc uses 8-bit planes unless the HIGH_DEPTH flag was
        // set (and the library was built for it).
        for (int y = 0; y < height; y++) {
            void *row = pic.img.plane[0] + (size_t)y * pic.img.i_stride[0];
            for (int x = 0; x < width; x++) {
                int v = (x * 3 + y * 5 + f * 7) & maxval;
                const int sq = (f * 5) % (width - 16);
                if (x >= sq && x < sq + 16 && y >= height / 3 && y < height / 3 + 16)
                    v = maxval;
                if (bitdepth > 8) ((uint16_t *)row)[x] = (uint16_t)v;
                else ((uint8_t *)row)[x] = (uint8_t)v;
            }
        }
        for (int pl = 1; pl < 3; pl++) {
            for (int y = 0; y < ch; y++) {
                void *row = pic.img.plane[pl] + (size_t)y * pic.img.i_stride[pl];
                for (int x = 0; x < cw; x++) {
                    const int v = (x * 2 + y * 3 + f * 11 + pl * 40) & maxval;
                    if (bitdepth > 8) ((uint16_t *)row)[x] = (uint16_t)v;
                    else ((uint8_t *)row)[x] = (uint8_t)v;
                }
            }
        }
        pic.i_pts = f;
        pic.i_type = X264_TYPE_AUTO;
        if (x264_encoder_encode(enc, &nals, &nnal, &pic, &pic_out) < 0) {
            fprintf(stderr, "gen_stream: encode failed at frame %d\n", f);
            ret = 1;
            break;
        }
        for (int i = 0; i < nnal; i++) {
            if (fwrite(nals[i].p_payload, 1, (size_t)nals[i].i_payload, out) !=
                (size_t)nals[i].i_payload) {
                perror(out_path);
                ret = 1;
                break;
            }
        }
        wrote++;
    }

    // Drain the encoder: with lookahead/B-frames it holds frames back, and a
    // flush call can return 0 NALs without the delay being empty.
    while (ret == 0 && x264_encoder_delayed_frames(enc) > 0) {
        if (x264_encoder_encode(enc, &nals, &nnal, NULL, &pic_out) < 0) {
            fprintf(stderr, "gen_stream: flush failed\n");
            ret = 1;
            break;
        }
        for (int i = 0; i < nnal; i++) {
            if (fwrite(nals[i].p_payload, 1, (size_t)nals[i].i_payload, out) !=
                (size_t)nals[i].i_payload) {
                perror(out_path);
                ret = 1;
                break;
            }
        }
    }
    if (ret == 0 && x264_encoder_delayed_frames(enc) != 0) {
        fprintf(stderr, "gen_stream: encoder still holds %d frames\n",
                x264_encoder_delayed_frames(enc));
        ret = 1;
    }
    x264_picture_clean(&pic);
    x264_encoder_close(enc);
    fclose(out);
    if (ret == 0)
        fprintf(stderr, "gen_stream: %s %dx%d profile=%s %d-bit chroma=%d frames=%d "
                        "bframes=%d refs=%d slices=%d preset=%s\n",
                out_path, width, height, profile, bitdepth, chroma, frames, bframes, refs, slices,
                preset);
    return ret;
}
