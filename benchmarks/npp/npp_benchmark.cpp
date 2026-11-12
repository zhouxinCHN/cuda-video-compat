// npp_benchmark.cpp
// NPP (NVIDIA Performance Primitives) 内存直测程序
//
// 用 cudaMalloc 生成测试图像（灰度渐变 + RGB 彩条 + YUV/NV12 平面），
// 直接调用 NPP API 处理，用 CUDA event 计时，输出 JSON 结果。
//
// 覆盖能力维度（26 项）：
//   几何变换（缩放/旋转/镜像/仿射/透视）
//   滤波（锐化/中值/高斯/Sobel/Canny 边缘）
//   色彩转换（YUV420/NV12/YUV422 ↔ RGB）
//   位深转换（8u→16u / 8u→32f）
//   算术/融合（帧差 AbsDiff / 加权叠加 AddWeighted）
//   形态学（膨胀/腐蚀）
//   统计（均值/标准差/直方图）
//   阈值（Threshold）
//
// 编译:
//   nvcc -o npp_benchmark npp_benchmark.cpp \
//        -lnppc -lnppig -lnppi -lnppif -lnppicc -lcudart
//
// 运行: ./npp_benchmark [--iter N] [--size WxH] [--json out.json]
//
// 说明：单项失败（NPP 返回非成功状态）只标记 ok:false，不中断整个程序。

#include <npp.h>
#include <nppi.h>
#include <nppdefs.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <cmath>

// ============================================================
// 简单 JSON 输出（不依赖第三方库）
// ============================================================
struct JsonBuilder {
    std::string s;
    bool first = true;

    void begin_obj() { s += "{"; first = true; }
    void end_obj()   { if (s.back() == ',') s.pop_back(); s += "}"; }
    void begin_arr() { s += "["; first = true; }
    void end_arr()   { if (s.back() == ',') s.pop_back(); s += "]"; }

    void key(const char* k) {
        if (!first) s += ",";
        first = false;
        s += std::string("\"") + k + "\":";
    }
    void val(const char* v)  { s += std::string("\"") + v + "\""; }
    void val(long long v)    { s += std::to_string(v); }
    void val(double v)       { char b[64]; snprintf(b, sizeof b, "%.6f", v); s += b; }
    void val(bool v)         { s += v ? "true" : "false"; }

    void obj(const char* k)  { key(k); begin_obj(); }
    void arr(const char* k)  { key(k); begin_arr(); }
    void raw_obj() {
        if (!first) s += ",";
        first = false;
        begin_obj();
    }

    std::string str() const  { return s; }
};

// ============================================================
// 工具函数
// ============================================================
#define CUDA_CHECK(call) do { \
    cudaError_t e = (call); \
    if (e != cudaSuccess) { \
        fprintf(stderr, "CUDA error %s at %s:%d\n", cudaGetErrorString(e), __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

// 生成灰度渐变测试图（0-255 斜向渐变）
void fill_gradient_8u(Npp8u* d, int w, int h) {
    std::vector<Npp8u> host((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            host[(size_t)y * w + x] = (Npp8u)((x * 255 / w + y * 255 / h) / 2);
    CUDA_CHECK(cudaMemcpy(d, host.data(), (size_t)w * h, cudaMemcpyHostToDevice));
}

// 生成 RGB 彩条测试图
void fill_colorbars_rgb(Npp8u* d, int w, int h) {
    std::vector<Npp8u> host((size_t)w * h * 3);
    const Npp8u colors[8][3] = {
        {255,255,255},{255,255,0},{0,255,255},{0,255,0},
        {255,0,255},{255,0,0},{0,0,255},{0,0,0}
    };
    int bar = w / 8;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int idx = x / bar; if (idx > 7) idx = 7;
            size_t o = ((size_t)y * w + x) * 3;
            host[o+0] = colors[idx][0];
            host[o+1] = colors[idx][1];
            host[o+2] = colors[idx][2];
        }
    CUDA_CHECK(cudaMemcpy(d, host.data(), (size_t)w * h * 3, cudaMemcpyHostToDevice));
}

// ============================================================
// 计时器（CUDA event）
// ============================================================
struct Timer {
    cudaEvent_t s, e;
    Timer() { cudaEventCreate(&s); cudaEventCreate(&e); }
    ~Timer() { cudaEventDestroy(s); cudaEventDestroy(e); }
    void start() { cudaEventRecord(s); }
    double stop() { cudaEventRecord(e); cudaEventSynchronize(e); float ms=0; cudaEventElapsedTime(&ms, s, e); return ms; }
};

// ============================================================
// 安全测一次操作：fn 返回 NppStatus，非 NPP_SUCCESS 即失败
// warmup 3 次 + 迭代取平均耗时。单项失败只返回 false，不中断。
// ============================================================
template <typename F>
bool bench(F&& fn, int iters, double* out_ms) {
    *out_ms = 0;
    for (int i = 0; i < 3; i++) {          // warmup
        if (fn() != NPP_SUCCESS) return false;
    }
    cudaDeviceSynchronize();
    Timer t;
    t.start();
    for (int i = 0; i < iters; i++) {
        if (fn() != NPP_SUCCESS) return false;
    }
    cudaDeviceSynchronize();
    *out_ms = t.stop() / iters;
    return true;
}

// ============================================================
// 主程序
// ============================================================
int main(int argc, char** argv) {
    int iters = 50;
    int W = 1920, H = 1080;
    const char* outfile = nullptr;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iter") && i+1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i+1 < argc) { sscanf(argv[++i], "%dx%d", &W, &H); }
        else if (!strcmp(argv[i], "--json") && i+1 < argc) outfile = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            printf("用法: %s [--iter N] [--size WxH] [--json out.json]\n", argv[0]);
            return 0;
        }
    }

    JsonBuilder j;
    j.begin_obj();
    j.key("framework"); j.val("NPP");
    j.key("gpu"); j.val("NVIDIA");
    j.key("size"); { char b[64]; snprintf(b, sizeof b, "%dx%d", W, H); j.val(b); }
    j.key("iterations"); j.val((long long)iters);

    {
        int dev = 0; cudaGetDevice(&dev);
        cudaDeviceProp prop; cudaGetDeviceProperties(&prop, dev);
        j.key("device"); j.val(prop.name);
        j.key("npp_version"); {
            char b[64];
            snprintf(b, sizeof b, "%d.%d.%d", NPP_VERSION_MAJOR, NPP_VERSION_MINOR, NPP_VERSION_BUILD);
            j.val(b);
        }
    }

    j.arr("results");

    const int srcW = W, srcH = H;
    const int dstW = W/2, dstH = H/2;

    // ============================================================
    // 分配设备内存
    // ============================================================
    Npp8u *d_gray, *d_gray2, *d_gray_dst, *d_rgb, *d_rgb_dst, *d_rgb_out;
    Npp8u *d_uv_interleaved;                 // NV12 的 UV 交错平面
    Npp8u *d_yuv[3];                          // YUV 平面（满尺寸简化分配）
    Npp8u *d_yuv420_out[3];                   // RGB→YUV420 输出
    Npp16u *d_16u;                            // 8u→16u 输出
    Npp32f *d_32f, *d_32f_blend;              // 8u→32f 及加权叠加输出
    Npp8u *d_morph, *d_edge, *d_thresh, *d_affine, *d_persp;

    CUDA_CHECK(cudaMalloc(&d_gray, (size_t)srcW * srcH));
    CUDA_CHECK(cudaMalloc(&d_gray2, (size_t)srcW * srcH));
    CUDA_CHECK(cudaMalloc(&d_gray_dst, (size_t)dstW * dstH));
    CUDA_CHECK(cudaMalloc(&d_rgb, (size_t)srcW * srcH * 3));
    CUDA_CHECK(cudaMalloc(&d_rgb_dst, (size_t)dstW * dstH * 3));
    CUDA_CHECK(cudaMalloc(&d_rgb_out, (size_t)srcW * srcH * 3));
    CUDA_CHECK(cudaMalloc(&d_uv_interleaved, (size_t)srcW * srcH / 2));
    for (int p = 0; p < 3; p++)
        CUDA_CHECK(cudaMalloc(&d_yuv[p], (size_t)srcW * srcH));
    CUDA_CHECK(cudaMalloc(&d_yuv420_out[0], (size_t)srcW * srcH));
    CUDA_CHECK(cudaMalloc(&d_yuv420_out[1], (size_t)srcW * srcH / 4));
    CUDA_CHECK(cudaMalloc(&d_yuv420_out[2], (size_t)srcW * srcH / 4));
    CUDA_CHECK(cudaMalloc(&d_16u, (size_t)srcW * srcH * 2));
    CUDA_CHECK(cudaMalloc(&d_32f, (size_t)srcW * srcH * 4));
    CUDA_CHECK(cudaMalloc(&d_32f_blend, (size_t)srcW * srcH * 4));
    CUDA_CHECK(cudaMalloc(&d_morph, (size_t)srcW * srcH));
    CUDA_CHECK(cudaMalloc(&d_edge, (size_t)srcW * srcH));
    CUDA_CHECK(cudaMalloc(&d_thresh, (size_t)srcW * srcH));
    CUDA_CHECK(cudaMalloc(&d_affine, (size_t)dstW * dstH));
    CUDA_CHECK(cudaMalloc(&d_persp, (size_t)dstW * dstH));

    // 填充测试数据
    fill_gradient_8u(d_gray, srcW, srcH);
    fill_colorbars_rgb(d_rgb, srcW, srcH);
    // 第二个灰度图（轻微偏移，用于帧差）
    fill_gradient_8u(d_gray2, srcW, srcH);
    // NV12 的 UV 交错平面填中性 128（无彩色）
    CUDA_CHECK(cudaMemset(d_uv_interleaved, 128, (size_t)srcW * srcH / 2));
    // YUV 平面的 U/V 填中性 128
    for (int p = 1; p < 3; p++)
        CUDA_CHECK(cudaMemset(d_yuv[p], 128, (size_t)srcW * srcH));

    NppiSize srcSize = {srcW, srcH};
    NppiSize dstSize = {dstW, dstH};
    NppiRect srcROI = {0, 0, srcW, srcH};
    NppiRect dstROI = {0, 0, dstW, dstH};

    double ms = 0;

    // ============================================================
    // 一、几何变换
    // ============================================================

    // 1. 缩放（灰度，线性插值）
    {
        auto fn = [&]() { return nppiResizeSqrPixel_8u_C1R(
            d_gray, srcSize, srcW, srcROI,
            d_gray_dst, dstW, dstROI,
            (double)dstW/srcW, (double)dstH/srcH, 0, 0, NPPI_INTER_LINEAR); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("resize"); j.key("desc"); j.val("缩放 (灰度, 线性)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 2. 缩放（RGB，立方插值）
    {
        auto fn = [&]() { return nppiResizeSqrPixel_8u_C3R(
            d_rgb, srcSize, srcW*3, srcROI,
            d_rgb_dst, dstW*3, dstROI,
            (double)dstW/srcW, (double)dstH/srcH, 0, 0, NPPI_INTER_CUBIC); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("resize_rgb"); j.key("desc"); j.val("缩放 (RGB, 立方)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 3. 镜像翻转（水平）
    {
        auto fn = [&]() { return nppiMirror_8u_C1R(d_gray, srcW, d_gray_dst, dstW, dstSize, NPP_HORIZONTAL_AXIS); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("mirror_h"); j.key("desc"); j.val("水平镜像翻转");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 4. 镜像翻转（垂直）
    {
        auto fn = [&]() { return nppiMirror_8u_C1R(d_gray, srcW, d_gray_dst, dstW, dstSize, NPP_VERTICAL_AXIS); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("mirror_v"); j.key("desc"); j.val("垂直镜像翻转");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 5. 旋转 45°
    {
        double bbox[2][2];
        nppiGetRotateBound(srcROI, bbox, 45.0, 0, 0);
        int rotW = (int)ceil(bbox[1][0] - bbox[0][0]);
        int rotH = (int)ceil(bbox[1][1] - bbox[0][1]);
        Npp8u* d_rot;
        CUDA_CHECK(cudaMalloc(&d_rot, (size_t)rotW * rotH));
        NppiRect rotROI = {0, 0, rotW, rotH};

        auto fn = [&]() { return nppiRotate_8u_C1R(
            d_gray, srcSize, srcW, srcROI,
            d_rot, rotW, rotROI, 45.0, 0, 0, NPPI_INTER_LINEAR); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("rotate_45"); j.key("desc"); j.val("旋转 45° (线性)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
        cudaFree(d_rot);
    }

    // 6. 仿射变换（缩放+平移，NPP 独占任意角度）
    {
        double affine[2][3] = { {0.5, 0.0, 10.0}, {0.0, 0.5, 10.0} };
        auto fn = [&]() { return nppiWarpAffine_8u_C1R(
            d_gray, srcSize, srcW, srcROI,
            d_affine, dstW, dstROI, affine, NPPI_INTER_LINEAR); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("warp_affine"); j.key("desc"); j.val("仿射变换 (缩放+平移)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 7. 透视变换（NPP 独占）
    {
        double persp[3][3] = { {0.5, 0.0, 10.0}, {0.0, 0.5, 10.0}, {0.0, 0.0, 1.0} };
        auto fn = [&]() { return nppiWarpPerspective_8u_C1R(
            d_gray, srcSize, srcW, srcROI,
            d_persp, dstW, dstROI, persp, NPPI_INTER_LINEAR); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("warp_perspective"); j.key("desc"); j.val("透视变换");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // ============================================================
    // 二、滤波
    // ============================================================

    // 8. 锐化（3x3 锐化核）
    {
        NppiSize roi = {srcW, srcH};
        Npp32s sharpen_kernel[9] = {0,-1,0, -1,5,-1, 0,-1,0};
        NppiSize ksize = {3, 3};
        NppiPoint anchor = {1, 1};
        auto fn = [&]() { return nppiFilterBorder_8u_C1R(
            d_gray, srcW, srcSize, {0,0},
            d_edge, srcW, roi, sharpen_kernel, ksize, anchor, 1,
            NPP_BORDER_REPLICATE); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("sharpen"); j.key("desc"); j.val("锐化 (3x3 核)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 9. 中值滤波 3x3
    {
        NppiSize roi = {srcW, srcH};
        NppiSize msize = {3, 3};
        NppiPoint anchor = {1, 1};
        Npp32u bufSize = 0;
        nppiFilterMedianGetBufferSize_8u_C1R(roi, msize, &bufSize);
        Npp8u* d_buf;
        CUDA_CHECK(cudaMalloc(&d_buf, bufSize));
        auto fn = [&]() { return nppiFilterMedian_8u_C1R(
            d_gray, srcW, d_edge, srcW, roi, msize, anchor, d_buf); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("median_3x3"); j.key("desc"); j.val("中值滤波 3x3");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
        cudaFree(d_buf);
    }

    // 10. 高斯滤波 3x3
    {
        NppiSize roi = {srcW, srcH};
        auto fn = [&]() { return nppiFilterGauss_8u_C1R(
            d_gray, srcW, d_edge, srcW, roi, NPP_MASK_SIZE_3_X_3); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("gauss_3x3"); j.key("desc"); j.val("高斯滤波 3x3");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 11. Sobel 水平边缘检测
    {
        auto fn = [&]() { return nppiFilterSobelHoriz_8u_C1R(
            d_gray, srcW, d_edge, srcW, srcSize); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("sobel_h"); j.key("desc"); j.val("Sobel 水平边缘");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 12. Canny 边缘检测
    {
        NppiSize roi = {srcW, srcH};
        int bufSize = 0;
        nppiFilterCannyBorderGetBufferSize(roi, &bufSize);
        Npp8u* d_canny_buf;
        CUDA_CHECK(cudaMalloc(&d_canny_buf, bufSize));
        auto fn = [&]() { return nppiFilterCannyBorder_8u_C1R(
            d_gray, srcW, srcSize, {0,0},
            d_edge, srcW, roi, NPP_FILTER_SOBEL, NPP_MASK_SIZE_3_X_3,
            20, 60, nppiNormL1, NPP_BORDER_REPLICATE, d_canny_buf); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("canny"); j.key("desc"); j.val("Canny 边缘检测");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
        cudaFree(d_canny_buf);
    }

    // ============================================================
    // 三、色彩转换
    // ============================================================

    // 13. YUV420 → RGB（平面 3 输入）
    {
        const Npp8u* srcp[3] = { d_yuv[0], d_yuv[1], d_yuv[2] };
        int srcStep[3] = { srcW, srcW/2, srcW/2 };
        auto fn = [&]() { return nppiYUV420ToRGB_8u_P3C3R(srcp, srcStep, d_rgb_out, srcW*3, srcSize); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("yuv420_to_rgb"); j.key("desc"); j.val("YUV420→RGB (平面)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 14. NV12 → RGB（半平面，解码器实际输出格式）
    {
        const Npp8u* srcp[2] = { d_gray, d_uv_interleaved };
        auto fn = [&]() { return nppiNV12ToRGB_8u_P2C3R(srcp, srcW, d_rgb_out, srcW*3, srcSize); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("nv12_to_rgb"); j.key("desc"); j.val("NV12→RGB (半平面)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 15. NV12 → BGR
    {
        const Npp8u* srcp[2] = { d_gray, d_uv_interleaved };
        auto fn = [&]() { return nppiNV12ToBGR_8u_P2C3R(srcp, srcW, d_rgb_out, srcW*3, srcSize); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("nv12_to_bgr"); j.key("desc"); j.val("NV12→BGR (半平面)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 16. YUV422 → RGB（4:2:2 专业格式）
    {
        const Npp8u* srcp[3] = { d_yuv[0], d_yuv[1], d_yuv[2] };
        int srcStep[3] = { srcW, srcW/2, srcW/2 };
        auto fn = [&]() { return nppiYUV422ToRGB_8u_P3C3R(srcp, srcStep, d_rgb_out, srcW*3, srcSize); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("yuv422_to_rgb"); j.key("desc"); j.val("YUV422→RGB (4:2:2)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 17. RGB → YUV420（反向转换，编码前处理）
    {
        Npp8u* dstp[3] = { d_yuv420_out[0], d_yuv420_out[1], d_yuv420_out[2] };
        int dstStep[3] = { srcW, srcW/2, srcW/2 };
        auto fn = [&]() { return nppiRGBToYUV420_8u_C3P3R(d_rgb, srcW*3, dstp, dstStep, srcSize); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("rgb_to_yuv420"); j.key("desc"); j.val("RGB→YUV420 (平面)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // ============================================================
    // 四、位深转换
    // ============================================================

    // 18. 8u → 16u（10/12bit 高色深基础）
    {
        auto fn = [&]() { return nppiConvert_8u16u_C1R(d_gray, srcW, d_16u, srcW*2, srcSize); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("convert_8u16u"); j.key("desc"); j.val("位深转换 8u→16u");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 19. 8u → 32f
    {
        auto fn = [&]() { return nppiConvert_8u32f_C1R(d_gray, srcW, d_32f, srcW*4, srcSize); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("convert_8u32f"); j.key("desc"); j.val("位深转换 8u→32f");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // ============================================================
    // 五、算术 / 融合
    // ============================================================

    // 20. 帧差（运动检测基础）
    {
        auto fn = [&]() { return nppiAbsDiff_8u_C1R(
            d_gray, srcW, d_gray2, srcW, d_edge, srcW, srcSize); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("absdiff"); j.key("desc"); j.val("帧差 AbsDiff (运动检测)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 21. 加权叠加（8u → 32f in-place，alpha 混合）
    {
        // 先初始化 32f 目标图
        CUDA_CHECK(cudaMemset(d_32f_blend, 0, (size_t)srcW * srcH * 4));
        auto fn = [&]() { return nppiAddWeighted_8u32f_C1IR(
            d_gray, srcW, d_32f_blend, srcW*4, srcSize, 0.5f); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("addweighted"); j.key("desc"); j.val("加权叠加 AddWeighted (8u→32f)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // ============================================================
    // 六、形态学
    // ============================================================

    // 22. 膨胀（3x3 全 1 结构元素）
    {
        Npp8u mask[9] = {1,1,1, 1,1,1, 1,1,1};
        NppiSize msize = {3, 3};
        NppiPoint anchor = {1, 1};
        auto fn = [&]() { return nppiDilate_8u_C1R(
            d_gray, srcW, d_morph, srcW, srcSize, mask, msize, anchor); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("dilate"); j.key("desc"); j.val("膨胀 (3x3)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // 23. 腐蚀（3x3 全 1 结构元素）
    {
        Npp8u mask[9] = {1,1,1, 1,1,1, 1,1,1};
        NppiSize msize = {3, 3};
        NppiPoint anchor = {1, 1};
        auto fn = [&]() { return nppiErode_8u_C1R(
            d_gray, srcW, d_morph, srcW, srcSize, mask, msize, anchor); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("erode"); j.key("desc"); j.val("腐蚀 (3x3)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    // ============================================================
    // 七、统计 / 阈值
    // ============================================================

    // 24. 均值 / 标准差
    {
        int bufSize = 0;
        nppiMeanStdDevGetBufferHostSize_8u_C1R(srcSize, &bufSize);
        Npp8u* d_buf;
        CUDA_CHECK(cudaMalloc(&d_buf, bufSize));
        Npp64f mean = 0, stddev = 0;   // host 结果指针
        auto fn = [&]() { return nppiMean_StdDev_8u_C1R(
            d_gray, srcW, srcSize, d_buf, &mean, &stddev); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("mean_stddev"); j.key("desc"); j.val("均值/标准差");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
        cudaFree(d_buf);
    }

    // 25. 直方图（256 级）
    {
        int bufSize = 0;
        nppiHistogramEvenGetBufferSize_8u_C1R(srcSize, 256, &bufSize);
        Npp8u* d_buf;
        CUDA_CHECK(cudaMalloc(&d_buf, bufSize));
        Npp32s* h_hist = (Npp32s*)malloc(256 * sizeof(Npp32s));  // host 结果
        auto fn = [&]() { return nppiHistogramEven_8u_C1R(
            d_gray, srcW, srcSize, h_hist, 256, 0, 255, d_buf); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("histogram"); j.key("desc"); j.val("直方图 (256级)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
        free(h_hist); cudaFree(d_buf);
    }

    // 26. 阈值（> 128 → 255）
    {
        auto fn = [&]() { return nppiThreshold_Val_8u_C1R(
            d_gray, srcW, d_thresh, srcW, srcSize, 128, 255, NPP_CMP_GREATER); };
        bool ok = bench(fn, iters, &ms);
        j.raw_obj(); j.key("op"); j.val("threshold"); j.key("desc"); j.val("阈值 (>128→255)");
        j.key("ok"); j.val(ok); j.key("avg_ms"); j.val(ms); j.end_obj();
    }

    j.end_arr();

    j.key("note"); j.val("avg_ms 为单次操作平均耗时（含 warmup 后多次迭代）");
    j.end_obj();

    // 输出
    std::string out = j.str();
    if (outfile) {
        FILE* f = fopen(outfile, "w");
        if (f) { fputs(out.c_str(), f); fclose(f); }
        printf("JSON 已写入 %s\n", outfile);
    }
    printf("%s\n", out.c_str());

    // 清理
    cudaFree(d_gray); cudaFree(d_gray2); cudaFree(d_gray_dst); cudaFree(d_rgb);
    cudaFree(d_rgb_dst); cudaFree(d_rgb_out); cudaFree(d_uv_interleaved);
    for (int p = 0; p < 3; p++) cudaFree(d_yuv[p]);
    for (int p = 0; p < 3; p++) cudaFree(d_yuv420_out[p]);
    cudaFree(d_16u); cudaFree(d_32f); cudaFree(d_32f_blend);
    cudaFree(d_morph); cudaFree(d_edge); cudaFree(d_thresh);
    cudaFree(d_affine); cudaFree(d_persp);

    return 0;
}
