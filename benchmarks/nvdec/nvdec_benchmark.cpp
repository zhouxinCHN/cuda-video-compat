// nvdec_benchmark.cpp
// NVDEC (NVIDIA Video Decoder) 硬件解码直测程序
//
// 数据流：FFmpeg demux 输出 Annex-B 裸码流 → 本程序用 NVDEC parser API 解码并计时
//
// 编译:
//   g++ -o nvdec_benchmark nvdec_benchmark.cpp \
//       -I$HOME/nv-codec-headers/include -ldl
//
// 运行（先 demux 出裸码流）:
//   ffmpeg -i input.mp4 -c:v copy -bsf:v h264_mp4toannexb -f h264 out.h264
//   ./nvdec_benchmark --input out.h264 --codec h264 [--json out.json]
//
// 支持 codec: h264 / hevc / vp9 / av1 / mpeg2

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <vector>
#include <string>
#include <cuda.h>  // CUDA driver API（用于初始化 context）

#include "ffnvcodec/dynlink_loader.h"
#include "ffnvcodec/dynlink_cuviddec.h"
#include "ffnvcodec/dynlink_nvcuvid.h"

// ============================================================
// 简单 JSON
// ============================================================
struct Json {
    std::string s;
    bool first = true;
    void begin() { s = "{"; first = true; }
    void end()   { if (s.back()==',') s.pop_back(); s += "}"; }
    void key(const char* k) { if(!first) s+=","; first=false; s += std::string("\"")+k+"\":"; }
    void str(const char* v) { s += std::string("\"")+v+"\""; }
    void num(long long v)   { s += std::to_string(v); }
    void dbl(double v)      { char b[64]; snprintf(b,sizeof b,"%.6f",v); s += b; }
    void boolean(bool v)    { s += v?"true":"false"; }
};

// ============================================================
// 全局状态
// ============================================================
static CuvidFunctions* g_cuvid = nullptr;
static CUvideodecoder   g_decoder = nullptr;

static long long g_total_frames = 0;
static long long g_decode_error = 0;
static int g_width = 0, g_height = 0;

// ============================================================
// 回调
// ============================================================
// 序列回调：拿到格式信息后创建解码器
// 返回值为 ulNumDecodeSurfaces（parser 用它同步 surface 数量），0 表示失败
static int CUDAAPI handle_sequence(void* user, CUVIDEOFORMAT* fmt) {
    g_width = fmt->coded_width;
    g_height = fmt->coded_height;

    if (g_decoder) {
        g_cuvid->cuvidDestroyDecoder(g_decoder);
        g_decoder = nullptr;
    }

    unsigned num_surfaces = fmt->min_num_decode_surfaces + 3;  // FFmpeg 用 min+3

    CUVIDDECODECREATEINFO dci;
    memset(&dci, 0, sizeof(dci));
    dci.ulWidth = fmt->coded_width;
    dci.ulHeight = fmt->coded_height;
    dci.ulNumDecodeSurfaces = num_surfaces;
    dci.CodecType = fmt->codec;
    dci.ChromaFormat = fmt->chroma_format;
    dci.bitDepthMinus8 = fmt->bit_depth_luma_minus8;
    dci.ulMaxWidth = fmt->coded_width;
    dci.ulMaxHeight = fmt->coded_height;
    dci.display_area.left = 0;
    dci.display_area.top = 0;
    dci.display_area.right = (short)fmt->display_area.right;
    dci.display_area.bottom = (short)fmt->display_area.bottom;
    // 根据 chroma_format 和 bit_depth 动态选 OutputFormat（同 FFmpeg 逻辑）
    // 否则 4:4:4 / 10bit / 12bit 会因输出格式不匹配而解码失败
    bool high_bit_depth = (fmt->bit_depth_luma_minus8 > 0);
    if (fmt->chroma_format == cudaVideoChromaFormat_444) {
        dci.OutputFormat = high_bit_depth ? cudaVideoSurfaceFormat_YUV444_16Bit
                                          : cudaVideoSurfaceFormat_YUV444;
    } else {
        dci.OutputFormat = high_bit_depth ? cudaVideoSurfaceFormat_P016
                                          : cudaVideoSurfaceFormat_NV12;
    }
    // target width/height 必须是偶数
    unsigned tw = (fmt->coded_width + 1) & ~1u;
    unsigned th = (fmt->coded_height + 1) & ~1u;
    dci.ulTargetWidth = tw;
    dci.ulTargetHeight = th;
    // 关键：必须设置 target_rect，否则解码输出区域为 0
    dci.target_rect.left = 0;
    dci.target_rect.top = 0;
    dci.target_rect.right = (short)tw;
    dci.target_rect.bottom = (short)th;
    dci.ulNumOutputSurfaces = 8;
    dci.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;

    if (g_cuvid->cuvidCreateDecoder(&g_decoder, &dci) != CUDA_SUCCESS) {
        fprintf(stderr, "cuvidCreateDecoder 失败（格式可能不被 NVDEC 支持）\n");
        return 0;
    }
    // 关键：返回实际 surface 数（FFmpeg 这么做），让 parser 同步
    return (int)num_surfaces;
}

// 解码回调
static int CUDAAPI handle_picture_decode(void* user, CUVIDPICPARAMS* pic) {
    if (!g_decoder) return 0;
    if (g_cuvid->cuvidDecodePicture(g_decoder, pic) == CUDA_SUCCESS) {
        g_total_frames++;
        return 1;
    } else {
        g_decode_error++;
        return 0;
    }
}

// 显示回调（不映射内存，仅统计）
static int CUDAAPI handle_picture_display(void* user, CUVIDPARSERDISPINFO* disp) {
    return 1;
}

// ============================================================
// 主程序
// ============================================================
int main(int argc, char** argv) {
    const char* input = nullptr;
    const char* codec_name = "h264";
    const char* outfile = nullptr;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--input") && i+1<argc) input = argv[++i];
        else if (!strcmp(argv[i], "--codec") && i+1<argc) codec_name = argv[++i];
        else if (!strcmp(argv[i], "--json") && i+1<argc) outfile = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            printf("用法: %s --input <annexb.h264> --codec <h264|hevc|vp9|av1|mpeg2> [--json out.json]\n", argv[0]);
            printf("先用 FFmpeg 解封装出裸码流:\n");
            printf("  ffmpeg -i in.mp4 -c:v copy -bsf:v h264_mp4toannexb -f h264 out.h264\n");
            return 0;
        }
    }
    if (!input) { fprintf(stderr, "缺少 --input\n"); return 1; }

    cudaVideoCodec codec;
    if (!strcmp(codec_name, "h264")) codec = cudaVideoCodec_H264;
    else if (!strcmp(codec_name, "hevc")) codec = cudaVideoCodec_HEVC;
    else if (!strcmp(codec_name, "vp9")) codec = cudaVideoCodec_VP9;
    else if (!strcmp(codec_name, "av1")) codec = cudaVideoCodec_AV1;
    else if (!strcmp(codec_name, "mpeg2")) codec = cudaVideoCodec_MPEG2;
    else { fprintf(stderr, "未知 codec: %s\n", codec_name); return 1; }

    // 先初始化 CUDA context（NVDEC 必须有 CUDA context 才能用，否则 cuvidGetDecoderCaps/CreateDecoder 返回 201 NOT_SUPPORTED）
    CUresult cr = cuInit(0);
    if (cr != CUDA_SUCCESS) {
        fprintf(stderr, "cuInit 失败: %d\n", (int)cr);
        return 1;
    }
    CUdevice cu_dev;
    cuDeviceGet(&cu_dev, 0);
    CUcontext cu_ctx;
    if (cuCtxCreate(&cu_ctx, 0, cu_dev) != CUDA_SUCCESS) {
        fprintf(stderr, "cuCtxCreate 失败\n");
        return 1;
    }

    // 加载 NVDEC 动态库
    if (cuvid_load_functions(&g_cuvid, nullptr) != 0) {
        fprintf(stderr, "加载 libnvcuvid 失败（是否安装了 NVIDIA 驱动？）\n");
        return 1;
    }

    // 创建 parser
    CUVIDPARSERPARAMS pp;
    memset(&pp, 0, sizeof(pp));
    pp.CodecType = codec;
    pp.ulMaxNumDecodeSurfaces = 20;
    pp.ulClockRate = 10000000;
    pp.ulErrorThreshold = 100;
    pp.ulMaxDisplayDelay = 0;
    pp.pfnSequenceCallback = handle_sequence;
    pp.pfnDecodePicture = handle_picture_decode;
    pp.pfnDisplayPicture = handle_picture_display;

    CUvideoparser parser = nullptr;
    if (g_cuvid->cuvidCreateVideoParser(&parser, &pp) != CUDA_SUCCESS) {
        fprintf(stderr, "cuvidCreateVideoParser 失败\n");
        cuvid_free_functions(&g_cuvid);
        return 1;
    }

    // 读裸码流
    FILE* f = fopen(input, "rb");
    if (!f) { fprintf(stderr, "无法打开 %s\n", input); return 1; }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    std::vector<unsigned char> buf(1024*1024);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
        CUVIDSOURCEDATAPACKET pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.payload = buf.data();
        pkt.payload_size = n;
        g_cuvid->cuvidParseVideoData(parser, &pkt);
    }
    fclose(f);

    // EOS
    CUVIDSOURCEDATAPACKET eos;
    memset(&eos, 0, sizeof(eos));
    eos.flags = CUVID_PKT_ENDOFSTREAM;
    g_cuvid->cuvidParseVideoData(parser, &eos);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ms = (t1.tv_sec-t0.tv_sec)*1000.0 + (t1.tv_nsec-t0.tv_nsec)/1e6;

    if (g_decoder) g_cuvid->cuvidDestroyDecoder(g_decoder);
    g_cuvid->cuvidDestroyVideoParser(parser);
    cuvid_free_functions(&g_cuvid);

    double fps = g_total_frames * 1000.0 / (elapsed_ms > 0 ? elapsed_ms : 1);
    double per_frame_ms = elapsed_ms / (g_total_frames > 0 ? g_total_frames : 1);

    Json j;
    j.begin();
    j.key("framework"); j.str("NVDEC");
    j.key("gpu"); j.str("NVIDIA");
    j.key("codec"); j.str(codec_name);
    j.key("input"); j.str(input);
    j.key("width"); j.num(g_width);
    j.key("height"); j.num(g_height);
    j.key("total_frames"); j.num(g_total_frames);
    j.key("decode_errors"); j.num(g_decode_error);
    j.key("total_ms"); j.dbl(elapsed_ms);
    j.key("avg_ms_per_frame"); j.dbl(per_frame_ms);
    j.key("fps"); j.dbl(fps);
    j.key("ok"); j.boolean(g_total_frames > 0 && g_decode_error == 0);
    j.end();

    printf("%s\n", j.s.c_str());
    if (outfile) { FILE* o = fopen(outfile, "w"); if (o) { fputs(j.s.c_str(), o); fclose(o); } printf("JSON 已写入 %s\n", outfile); }

    return 0;
}
