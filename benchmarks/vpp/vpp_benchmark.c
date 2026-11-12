// vpp_benchmark.c
// VAAPI VPP (Video Processing Pipeline) 内存直测程序
//
// 直接调用 libva 的 VAEntrypointVideoProc 测试 VPP 处理能力：
//   缩放、旋转、锐化、降噪、色彩调整（亮度/对比度/饱和度）、去隔行、HDR色调映射
//
// 用 VA 表面（GPU 内存）输入输出，CPU 计时，输出 JSON。
//
// 编译:
//   gcc -o vpp_benchmark vpp_benchmark.c $(pkg-config --cflags --libs libva libva-drm) -lm
//
// 运行（需在 AMD GPU 机器上，且用户有 /dev/dri/renderD128 权限）:
//   ./vpp_benchmark [--size WxH] [--iter N] [--json out.json]
//
// 前置: 加入 video 组: sudo usermod -aG video $USER

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_vpp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdint.h>

// ============================================================
// 简单 JSON 输出
// ============================================================
typedef struct { char s[65536]; int first; } Json;
static void j_begin(Json* j)   { j->s[0]=0; strcat(j->s,"{"); j->first=1; }
static void j_end(Json* j)     { if(j->s[strlen(j->s)-1]==',') j->s[strlen(j->s)-1]=0; strcat(j->s,"}"); }
static void j_arr(Json* j)     { if(j->s[strlen(j->s)-1]==',') j->s[strlen(j->s)-1]=0; strcat(j->s,"]"); }
static void j_key(Json* j, const char* k) {
    if(!j->first) strcat(j->s,","); j->first=0;
    char b[128]; snprintf(b,sizeof b,"\"%s\":",k); strcat(j->s,b);
}
static void j_str(Json* j, const char* v) { char b[256]; snprintf(b,sizeof b,"\"%s\"",v); strcat(j->s,b); }
static void j_int(Json* j, long long v)   { char b[32]; snprintf(b,sizeof b,"%lld",v); strcat(j->s,b); }
static void j_dbl(Json* j, double v)      { char b[48]; snprintf(b,sizeof b,"%.6f",v); strcat(j->s,b); }
static void j_bool(Json* j, int v)        { strcat(j->s, v?"true":"false"); }
static void j_rawobj(Json* j) {
    if(!j->first) strcat(j->s,","); j->first=0; strcat(j->s,"{"); j->first=1;
}
static void j_objend(Json* j) { if(j->s[strlen(j->s)-1]==',') j->s[strlen(j->s)-1]=0; strcat(j->s,"}"); }

// ============================================================
// 工具
// ============================================================
#define VA_CHECK(call) do { \
    VAStatus st = (call); \
    if (st != VA_STATUS_SUCCESS) { \
        fprintf(stderr, "VA error %s (0x%x) at %s:%d\n", vaErrorStr(st), st, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec*1000.0 + ts.tv_nsec/1e6;
}

// 查询并打印 VPP 能力
static void query_caps(VADisplay dpy, Json* j) {
    VAConfigID cfg;
    VAProcPipelineCaps caps;
    memset(&caps, 0, sizeof(caps));
    VA_CHECK(vaCreateConfig(dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &cfg));
    VA_CHECK(vaQueryVideoProcPipelineCaps(dpy, cfg, NULL, 0, &caps));

    // 支持的滤镜
    VAProcFilterType filters[VAProcFilterCount];
    unsigned int nf = VAProcFilterCount;
    vaQueryVideoProcFilters(dpy, cfg, filters, &nf);

    j_key(j, "vpp_filters");
    char buf[512] = "[";
    for (unsigned int i = 0; i < nf; i++) {
        const char* name = "unknown";
        switch (filters[i]) {
            case VAProcFilterNoiseReduction: name="denoise"; break;
            case VAProcFilterDeinterlacing: name="deinterlace"; break;
            case VAProcFilterSharpening: name="sharpen"; break;
            case VAProcFilterColorBalance: name="color_balance"; break;
            case VAProcFilterHVSNoiseReduction: name="hvs_denoise"; break;
            case VAProcFilterHighDynamicRangeToneMapping: name="hdr_tonemap"; break;
            case VAProcFilter3DLUT: name="3dlut"; break;
            default: name="other"; break;
        }
        if (i) strcat(buf, ",");
        strcat(buf, name);
    }
    strcat(buf, "]");
    j_str(j, buf);

    // 旋转支持
    j_key(j, "rotation_flags"); j_int(j, caps.rotation_flags);
    // 镜像支持
    j_key(j, "mirror_flags"); j_int(j, caps.mirror_flags);
    // 输入输出像素格式数量
    j_key(j, "num_input_pixel_formats"); j_int(j, caps.num_input_pixel_formats);
    j_key(j, "num_output_pixel_formats"); j_int(j, caps.num_output_pixel_formats);

    vaDestroyConfig(dpy, cfg);
}

// ============================================================
// VPP 管线执行
// ============================================================
typedef struct {
    VADisplay dpy;
    VAConfigID cfg;
    VAContextID ctx;
    int srcW, srcH, dstW, dstH;
    VASurfaceID src, dst;
} VppCtx;

static void vpp_init(VppCtx* c, VADisplay dpy, int srcW, int srcH, int dstW, int dstH) {
    memset(c, 0, sizeof *c);
    c->dpy = dpy; c->srcW = srcW; c->srcH = srcH; c->dstW = dstW; c->dstH = dstH;
    VA_CHECK(vaCreateConfig(dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &c->cfg));
    VA_CHECK(vaCreateContext(dpy, c->cfg, dstW, dstH, VA_PROGRESSIVE, NULL, 0, &c->ctx));
    VA_CHECK(vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, srcW, srcH, &c->src, 1, NULL, 0));
    VA_CHECK(vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, dstW, dstH, &c->dst, 1, NULL, 0));
}

static void vpp_deinit(VppCtx* c) {
    vaDestroySurfaces(c->dpy, &c->src, 1);
    vaDestroySurfaces(c->dpy, &c->dst, 1);
    vaDestroyContext(c->dpy, c->ctx);
    vaDestroyConfig(c->dpy, c->cfg);
}

// 执行一次 VPP 管线（带可选滤镜）
// filters: 已创建的滤镜参数 buffer 数组；n_filters: 数量
// rotation: 旋转角度（0/90/180/270，用 VA_ROTATION_*）
static VAStatus vpp_run(VppCtx* c, VABufferID* filters, unsigned n_filters, unsigned rotation) {
    VAProcPipelineParameterBuffer pipe;
    memset(&pipe, 0, sizeof pipe);
    pipe.surface = c->src;
    pipe.surface_region = NULL;
    pipe.output_region = NULL;
    pipe.output_background_color = 0xFF000000;
    pipe.filter_flags = 0;
    pipe.filters = filters;
    pipe.num_filters = n_filters;
    pipe.rotation_state = rotation;

    VABufferID pipe_buf;
    VAStatus st = vaCreateBuffer(c->dpy, c->ctx, VAProcPipelineParameterBufferType,
                                 sizeof(pipe), 1, &pipe, &pipe_buf);
    if (st != VA_STATUS_SUCCESS) return st;

    st = vaBeginPicture(c->dpy, c->ctx, c->dst);
    if (st == VA_STATUS_SUCCESS) st = vaRenderPicture(c->dpy, c->ctx, &pipe_buf, 1);
    if (st == VA_STATUS_SUCCESS) st = vaEndPicture(c->dpy, c->ctx);

    vaDestroyBuffer(c->dpy, pipe_buf);
    return st;
}

// 创建滤镜参数 buffer，返回 VABufferID
static VABufferID make_filter(VppCtx* c, VAProcFilterType type, float value,
                              int attrib, int algorithm) {
    // 根据滤镜类型构造参数
    if (type == VAProcFilterDeinterlacing) {
        VAProcFilterParameterBufferDeinterlacing p;
        memset(&p, 0, sizeof p);
        p.type = type; p.algorithm = algorithm;
        VABufferID b; VA_CHECK(vaCreateBuffer(c->dpy, c->ctx, VAProcFilterParameterBufferType, sizeof(p), 1, &p, &b));
        return b;
    } else if (type == VAProcFilterColorBalance) {
        VAProcFilterParameterBufferColorBalance p;
        memset(&p, 0, sizeof p);
        p.type = type; p.attrib = attrib; p.value = value;
        VABufferID b; VA_CHECK(vaCreateBuffer(c->dpy, c->ctx, VAProcFilterParameterBufferType, sizeof(p), 1, &p, &b));
        return b;
    } else {
        // 通用：锐化、降噪 用 VAProcFilterParameterBuffer {type, value}
        VAProcFilterParameterBuffer p;
        memset(&p, 0, sizeof p);
        p.type = type; p.value = value;
        VABufferID b; VA_CHECK(vaCreateBuffer(c->dpy, c->ctx, VAProcFilterParameterBufferType, sizeof(p), 1, &p, &b));
        return b;
    }
}

// ============================================================
// 主程序
// ============================================================
int main(int argc, char** argv) {
    int W = 1920, H = 1080;
    int iters = 50;
    const char* outfile = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i+1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--iter") && i+1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--json") && i+1 < argc) outfile = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            printf("用法: %s [--size WxH] [--iter N] [--json out.json]\n", argv[0]);
            printf("注意: 需在 AMD GPU 机器运行，且用户有 /dev/dri/renderD128 权限\n");
            return 0;
        }
    }

    // 打开 DRM 设备
    const char* drm_dev = "/dev/dri/renderD128";
    // 若 renderD128 不存在，尝试 renderD129
    if (access(drm_dev, R_OK|W_OK) != 0) {
        drm_dev = "/dev/dri/renderD129";
    }
    int fd = open(drm_dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "无法打开 %s\n", drm_dev);
        fprintf(stderr, "可能原因:\n");
        fprintf(stderr, "  1. 当前机器无 AMD GPU（本程序需在 AMD 机器运行）\n");
        fprintf(stderr, "  2. 无设备权限，尝试: sudo usermod -aG render,video $USER 后重新登录\n");
        fprintf(stderr, "  3. 设备节点不是 renderD128/renderD129，可用 ls /dev/dri/ 查看\n");
        return 1;
    }

    VADisplay dpy = vaGetDisplayDRM(fd);
    if (!dpy) { fprintf(stderr, "vaGetDisplayDRM 失败\n"); return 1; }

    int maj, min;
    VAStatus st = vaInitialize(dpy, &maj, &min);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaInitialize 失败: %s\n", vaErrorStr(st));
        return 1;
    }

    Json j; j_begin(&j);
    j_key(&j, "framework"); j_str(&j, "VAAPI-VPP");
    j_key(&j, "gpu"); j_str(&j, "AMD");
    char sz[32]; snprintf(sz, sizeof sz, "%dx%d", W, H); j_key(&j, "size"); j_str(&j, sz);
    j_key(&j, "iterations"); j_int(&j, iters);
    j_key(&j, "va_version"); { char b[32]; snprintf(b, sizeof b, "%d.%d", maj, min); j_str(&j, b); }

    // 查询能力
    query_caps(dpy, &j);

    j_key(&j, "results"); strcat(j.s, "["); j.first = 1;

    // 分配 VPP 上下文（dst 用 1/2 尺寸测缩放）
    VppCtx c;
    vpp_init(&c, dpy, W, H, W/2, H/2);

    // 通用测试 lambda（用宏模拟）
    #define RUN_TEST(op, desc, setup_fn, run_fn) do { \
        VABufferID* fl = NULL; unsigned nfl = 0; unsigned rot = 0; \
        setup_fn; \
        /* warmup */ \
        for (int w=0; w<3; w++) { VAStatus s = run_fn; if (s != VA_STATUS_SUCCESS) { break; } } \
        double t0 = now_ms(); \
        int ok = 1; \
        for (int i=0; i<iters; i++) { VAStatus s = run_fn; if (s != VA_STATUS_SUCCESS) { ok=0; break; } } \
        double t1 = now_ms(); \
        double avg = ok ? (t1-t0)/iters : 0; \
        j_rawobj(&j); j_key(&j, "op"); j_str(&j, op); j_key(&j, "desc"); j_str(&j, desc); \
        j_key(&j, "ok"); j_bool(&j, ok); j_key(&j, "avg_ms"); j_dbl(&j, avg); j_objend(&j); \
        if (fl) free(fl); \
    } while(0)

    // 1. 缩放（无滤镜，纯缩放）
    {
        RUN_TEST("scale", "缩放 (1920x1080 → 960x540)",
                 { fl=NULL; nfl=0; rot=0; },
                 vpp_run(&c, fl, nfl, rot));
    }

    // 2. 锐化
    {
        RUN_TEST("sharpen", "锐化 (value=50)",
                 { VABufferID fb = make_filter(&c, VAProcFilterSharpening, 50.0f, 0, 0);
                   fl = malloc(sizeof(VABufferID)); fl[0]=fb; nfl=1; },
                 vpp_run(&c, fl, nfl, rot));
    }

    // 3. 降噪
    {
        RUN_TEST("denoise", "降噪 (value=32)",
                 { VABufferID fb = make_filter(&c, VAProcFilterNoiseReduction, 32.0f, 0, 0);
                   fl = malloc(sizeof(VABufferID)); fl[0]=fb; nfl=1; },
                 vpp_run(&c, fl, nfl, rot));
    }

    // 4. 亮度调整
    {
        RUN_TEST("brightness", "亮度调整 (+20)",
                 { VABufferID fb = make_filter(&c, VAProcFilterColorBalance, 20.0f, VAProcColorBalanceBrightness, 0);
                   fl = malloc(sizeof(VABufferID)); fl[0]=fb; nfl=1; },
                 vpp_run(&c, fl, nfl, rot));
    }

    // 5. 对比度调整
    {
        RUN_TEST("contrast", "对比度调整 (1.5)",
                 { VABufferID fb = make_filter(&c, VAProcFilterColorBalance, 1.5f, VAProcColorBalanceContrast, 0);
                   fl = malloc(sizeof(VABufferID)); fl[0]=fb; nfl=1; },
                 vpp_run(&c, fl, nfl, rot));
    }

    // 6. 饱和度调整
    {
        RUN_TEST("saturation", "饱和度调整 (1.5)",
                 { VABufferID fb = make_filter(&c, VAProcFilterColorBalance, 1.5f, VAProcColorBalanceSaturation, 0);
                   fl = malloc(sizeof(VABufferID)); fl[0]=fb; nfl=1; },
                 vpp_run(&c, fl, nfl, rot));
    }

    // 7. 去隔行（bob 模式）
    {
        RUN_TEST("deinterlace_bob", "去隔行 (bob)",
                 { VABufferID fb = make_filter(&c, VAProcFilterDeinterlacing, 0, 0, VAProcDeinterlacingBob);
                   fl = malloc(sizeof(VABufferID)); fl[0]=fb; nfl=1; },
                 vpp_run(&c, fl, nfl, rot));
    }

    // 8. 去隔行（motion_adaptive 模式）
    {
        RUN_TEST("deinterlace_motion", "去隔行 (motion_adaptive)",
                 { VABufferID fb = make_filter(&c, VAProcFilterDeinterlacing, 0, 0, VAProcDeinterlacingMotionAdaptive);
                   fl = malloc(sizeof(VABufferID)); fl[0]=fb; nfl=1; },
                 vpp_run(&c, fl, nfl, rot));
    }

    // 9. 旋转 90°
    {
        RUN_TEST("rotate_90", "旋转 90°",
                 { fl=NULL; nfl=0; rot=VA_ROTATION_90; },
                 vpp_run(&c, fl, nfl, rot));
    }

    // 10. 旋转 180°
    {
        RUN_TEST("rotate_180", "旋转 180°",
                 { fl=NULL; nfl=0; rot=VA_ROTATION_180; },
                 vpp_run(&c, fl, nfl, rot));
    }

    #undef RUN_TEST

    // 结束 results 数组
    if (j.s[strlen(j.s)-1]==',') j.s[strlen(j.s)-1]=0;
    strcat(j.s, "]"); j.first = 0;

    j_key(&j, "note"); j_str(&j, "avg_ms 为单次 VPP 管线平均耗时");

    j_end(&j);

    // 输出
    if (outfile) { FILE* f = fopen(outfile, "w"); if (f) { fputs(j.s, f); fclose(f); } printf("JSON 已写入 %s\n", outfile); }
    printf("%s\n", j.s);

    vpp_deinit(&c);
    vaTerminate(dpy);
    close(fd);
    return 0;
}
