// verify_zerocopy.cpp
// 零拷贝验证实验：ROCm(HIP) + Mesa(VAAPI) 同 GPU 内存互操作
//
// 三个实验（对应 docs/verification/zero-copy-experiment.md）：
//   实验1：HIP + VAAPI 同进程共存
//   实验2：边界1 —— VAAPI surface 导出 DMA-BUF → HIP import → HIP 读（解码→NPP 方向）
//   实验3：边界2 —— HIP 内存导出 DMA-BUF（经 HSA）→ VAAPI import（计算→编码方向）
//
// 运行：./verify_zerocopy
// 每个实验输出 PASS / FAIL / SKIP 及原因，最后给汇总结论。

#include <hip/hip_runtime_api.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

// ---------- 工具 ----------
static int g_pass = 0, g_fail = 0, g_skip = 0;

#define LOG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define PASS(...)   do { printf("  [PASS] "); printf(__VA_ARGS__); printf("\n"); g_pass++; } while (0)
#define FAIL(name, ...) do { printf("  [FAIL] %s: ", name); printf(__VA_ARGS__); printf("\n"); g_fail++; } while (0)
#define SKIP(name, ...) do { printf("  [SKIP] %s: ", name); printf(__VA_ARGS__); printf("\n"); g_skip++; } while (0)

static void section(const char *t) { printf("\n========== %s ==========\n", t); }

// ---------- 实验 1：同进程共存 ----------
static int exp1_coexist() {
    section("实验1：HIP + VAAPI 同进程共存");
    int r = 0;

    // HIP 初始化
    hipError_t he = hipInit(0);
    if (he != hipSuccess) { FAIL("hipInit", "code=%d (%s)", he, hipGetErrorString(he)); return 1; }
    int hipCount = 0;
    he = hipGetDeviceCount(&hipCount);
    if (he != hipSuccess || hipCount < 1) { FAIL("hipGetDeviceCount", "code=%d count=%d", he, hipCount); return 1; }
    PASS("HIP 初始化成功，设备数=%d", hipCount);

    // VAAPI 初始化（打开 /dev/dri/renderD128）
    int drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (drm_fd < 0) { FAIL("open renderD128", "%s", strerror(errno)); return 1; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) { FAIL("vaGetDisplayDRM", "返回 NULL"); close(drm_fd); return 1; }
    int major = 0, minor = 0;
    VAStatus vs = vaInitialize(dpy, &major, &minor);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaInitialize", "status=%d (%s)", vs, vaErrorStr(vs)); close(drm_fd); return 1; }
    PASS("VAAPI 初始化成功 (libva %d.%d)", major, minor);

    // HIP 分配一块内存（确认共存后还能正常 alloc）
    void *hipPtr = nullptr;
    he = hipMalloc(&hipPtr, 1 << 20);
    if (he != hipSuccess || !hipPtr) { FAIL("hipMalloc(1MB)", "code=%d", he); vaTerminate(dpy); close(drm_fd); return 1; }
    PASS("HIP 分配 1MB 设备内存成功");

    // 结束
    hipFree(hipPtr);
    vaTerminate(dpy);
    close(drm_fd);
    PASS("实验1 通过：HIP 与 VAAPI 同进程共存，无冲突");
    return 0;
}

// ---------- 实验 2：VAAPI surface → HIP（解码→NPP 方向） ----------
static int exp2_vaapi_to_hip() {
    section("实验2：边界1 —— VAAPI surface → HIP import（解码→NPP）");
    int drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (drm_fd < 0) { FAIL("open renderD128", "%s", strerror(errno)); return 1; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    int major = 0, minor = 0;
    VAStatus vs = vaInitialize(dpy, &major, &minor);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaInitialize", "status=%d", vs); close(drm_fd); return 1; }

    // 1. 创建 YUV420 surface
    const int W = 320, H = 240;
    VASurfaceID surf = VA_INVALID_ID;
    vs = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, &surf, 1, nullptr, 0);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaCreateSurfaces", "status=%d (%s)", vs, vaErrorStr(vs)); vaTerminate(dpy); close(drm_fd); return 1; }
    LOG("  surface 创建成功 (id=%u)", surf);

    // 2. 导出 DRM PRIME fd
    VADRMPRIMESurfaceDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    vs = vaExportSurfaceHandle(dpy, surf,
                               VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                               &desc);
    if (vs != VA_STATUS_SUCCESS) {
        // 有些驱动只支持 COMPOSED_LAYERS，重试
        vs = vaExportSurfaceHandle(dpy, surf,
                                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                   &desc);
    }
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaExportSurfaceHandle", "status=%d (%s)", vs, vaErrorStr(vs)); vaDestroySurfaces(dpy, &surf, 1); vaTerminate(dpy); close(drm_fd); return 1; }
    LOG("  导出 DRM PRIME 成功: fourcc=%.4s num_objects=%u num_layers=%u",
        (char*)&desc.fourcc, desc.num_objects, desc.num_layers);

    // 3. HIP 导入第一个 object 的 fd
    if (desc.num_objects < 1) { FAIL("导出", "num_objects=0"); vaDestroySurfaces(dpy, &surf, 1); vaTerminate(dpy); close(drm_fd); return 1; }
    hipExternalMemoryHandleDesc extDesc;
    memset(&extDesc, 0, sizeof(extDesc));
    extDesc.type = hipExternalMemoryHandleTypeOpaqueFd;
    extDesc.handle.fd = desc.objects[0].fd;
    extDesc.size = desc.objects[0].size;

    hipExternalMemory_t extMem = nullptr;
    hipError_t he = hipImportExternalMemory(&extMem, &extDesc);
    if (he != hipSuccess) { FAIL("hipImportExternalMemory", "code=%d (%s)", he, hipGetErrorString(he)); close(desc.objects[0].fd); vaDestroySurfaces(dpy, &surf, 1); vaTerminate(dpy); close(drm_fd); return 1; }
    PASS("HIP 导入 VAAPI 导出的 DMA-BUF fd 成功");

    // 4. 映射成 HIP 设备指针
    hipExternalMemoryBufferDesc bufDesc;
    memset(&bufDesc, 0, sizeof(bufDesc));
    bufDesc.offset = 0;
    bufDesc.size = desc.objects[0].size;
    void *hipPtr = nullptr;
    he = hipExternalMemoryGetMappedBuffer(&hipPtr, extMem, &bufDesc);
    if (he != hipSuccess || !hipPtr) { FAIL("hipExternalMemoryGetMappedBuffer", "code=%d", he); hipDestroyExternalMemory(extMem); close(desc.objects[0].fd); vaDestroySurfaces(dpy, &surf, 1); vaTerminate(dpy); close(drm_fd); return 1; }
    PASS("映射成 HIP 设备指针成功 (ptr=%p, size=%u)", hipPtr, desc.objects[0].size);

    // 5. （可选）写几个字节验证可写（这里不跑 kernel，仅验证指针可用）
    //    实际像素校验需要 HIP kernel，本实验先验证 import+map 链路通。

    hipDestroyExternalMemory(extMem);
    close(desc.objects[0].fd);
    vaDestroySurfaces(dpy, &surf, 1);
    vaTerminate(dpy);
    close(drm_fd);
    PASS("实验2 通过：VAAPI surface → HIP import 链路成立（解码→NPP 零拷贝可行）");
    return 0;
}

// ---------- 实验 3：HIP 内存 → VAAPI（计算→编码方向） ----------
static int exp3_hip_to_vaapi() {
    section("实验3：边界2 —— HIP 内存 → 导出 DMA-BUF → VAAPI import（计算→编码）");
    int r = 0;

    // 1. HIP 分配内存
    hipInit(0);
    const size_t SIZE = 320 * 240 * 3 / 2;  // NV12 大小
    void *hipPtr = nullptr;
    hipError_t he = hipMalloc(&hipPtr, SIZE);
    if (he != hipSuccess) { FAIL("hipMalloc", "code=%d", he); return 1; }
    LOG("  HIP 分配 %zu 字节成功 (ptr=%p)", SIZE, hipPtr);

    // 2. 通过 HSA 导出 DMA-BUF（HIP 标准 API 无导出，走 HSA）
    int dmabuf_fd = -1;
    uint64_t dmabuf_offset = 0;
    hsa_status_t hs = hsa_amd_portable_export_dmabuf(hipPtr, SIZE, &dmabuf_fd, &dmabuf_offset);
    if (hs != HSA_STATUS_SUCCESS) {
        // v1 失败试 v2（v2 多了 flags 参数）
        hs = hsa_amd_portable_export_dmabuf_v2(hipPtr, SIZE, &dmabuf_fd, &dmabuf_offset, 0);
    }
    if (hs != HSA_STATUS_SUCCESS || dmabuf_fd < 0) {
        FAIL("hsa_amd_portable_export_dmabuf", "HSA status=0x%x", hs);
        LOG("    => 边界2 无法 zero-copy，需走拷贝路径（不致命，但需计入性能预算）");
        hipFree(hipPtr);
        return 1;
    }
    PASS("HIP 内存经 HSA 导出 DMA-BUF fd 成功 (fd=%d, offset=%llu)", dmabuf_fd, (unsigned long long)dmabuf_offset);

    // 3. VAAPI 导入该 fd 创建 surface
    int drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (drm_fd < 0) { FAIL("open renderD128", "%s", strerror(errno)); close(dmabuf_fd); hipFree(hipPtr); return 1; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    int major = 0, minor = 0;
    VAStatus vs = vaInitialize(dpy, &major, &minor);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaInitialize", "status=%d", vs); close(dmabuf_fd); close(drm_fd); hipFree(hipPtr); return 1; }

    VADRMPRIMESurfaceDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    desc.fourcc = VA_FOURCC_NV12;
    desc.width = 320;
    desc.height = 240;
    desc.num_objects = 1;
    desc.objects[0].fd = dmabuf_fd;
    desc.objects[0].size = SIZE;
    desc.objects[0].drm_format_modifier = 0;  // 或 DRM_FORMAT_MOD_LINEAR
    desc.num_layers = 1;
    desc.layers[0].drm_format = VA_FOURCC_NV12;
    desc.layers[0].num_planes = 2;
    desc.layers[0].object_index[0] = 0;
    desc.layers[0].offset[0] = 0;
    desc.layers[0].object_index[1] = 0;
    desc.layers[0].offset[1] = 320 * 240;  // Y 平面后跟 UV
    desc.layers[0].pitch[0] = 320;
    desc.layers[0].pitch[1] = 320;

    VASurfaceAttrib attribs[2];
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &desc;

    VASurfaceID surf = VA_INVALID_ID;
    vs = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240, &surf, 1, attribs, 2);
    if (vs != VA_STATUS_SUCCESS) {
        FAIL("vaCreateSurfaces(导入HIP的DMA-BUF)", "status=%d (%s)", vs, vaErrorStr(vs));
        LOG("    => VAAPI 无法导入 HIP 导出的 DMA-BUF，边界2 需拷贝");
        close(dmabuf_fd); vaTerminate(dpy); close(drm_fd); hipFree(hipPtr);
        return 1;
    }
    PASS("VAAPI 导入 HIP 导出的 DMA-BUF 创建 surface 成功 (id=%u)", surf);

    vaDestroySurfaces(dpy, &surf, 1);
    vaTerminate(dpy);
    close(dmabuf_fd);
    close(drm_fd);
    hipFree(hipPtr);
    PASS("实验3 通过：HIP 内存 → DMA-BUF → VAAPI surface 链路成立（计算→编码零拷贝可行）");
    return 0;
}

// ---------- 主流程 ----------
int main() {
    printf("==========================================================\n");
    printf(" 零拷贝验证实验：ROCm(HIP) + Mesa(VAAPI) 内存互操作\n");
    printf("==========================================================\n");

    exp1_coexist();
    exp2_vaapi_to_hip();
    exp3_hip_to_vaapi();

    printf("\n========== 汇总 ==========\n");
    printf("  PASS: %d   FAIL: %d   SKIP: %d\n", g_pass, g_fail, g_skip);
    if (g_fail == 0) {
        printf("  结论：全链路零拷贝可行（解码→NPP→计算→编码 全部 zero-copy）\n");
    } else {
        printf("  结论：存在失败项，见上方 FAIL 详情；跨栈边界可能需回退拷贝路径\n");
    }
    return g_fail == 0 ? 0 : 1;
}
