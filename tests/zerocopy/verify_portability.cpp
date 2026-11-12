// verify_portability.cpp
// 实验 E：目标平台可移植性探测（§4.9 / §1.7 第0步硬门槛）
//
// 目的：用最低成本在目标厂商平台证伪/确认 5 项关键能力，作为"是否可复用 AMD 实测结论"
//       的止损阀。任一项缺失 → 对应零拷贝/同步链路降级，触发范围复审。
//
// 探测项：
//   P1  HIP 与 VAAPI 同进程共存
//   P2  HSA 私有导出 API（hsa_amd_portable_export_dmabuf）存在性
//   P3  线性 NV12 surface 主动创建（modifier=LINEAR）
//   P4  DMA-BUF 双向互导（VAAPI export → HIP import；HIP export → VAAPI import）
//   P5  DRM syncobj / sync_file 能力
//
// 本程序只做"能力探测"，不做像素校验；像素校验见实验 C/D。
// 运行（目标平台或 AMD 机器）：
//   make -f verify_portability.make
//   ./verify_portability

#include <hip/hip_runtime_api.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <drm.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#define PASS(...) do { printf("  [PASS] "); printf(__VA_ARGS__); printf("\n"); g_pass++; } while (0)
#define FAIL(...) do { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); g_fail++; } while (0)
#define INFO(...) do { printf("  [info] "); printf(__VA_ARGS__); printf("\n"); } while (0)

static int g_pass = 0, g_fail = 0;
static const int W = 320, H = 240;

static hsa_agent_t g_gpu; static bool g_gpu_found = false;
static hsa_status_t find_gpu_cb(hsa_agent_t a, void* d) {
    (void)d; hsa_device_type_t t; hsa_agent_get_info(a, HSA_AGENT_INFO_DEVICE, &t);
    if (t == HSA_DEVICE_TYPE_GPU) { g_gpu = a; g_gpu_found = true; return HSA_STATUS_INFO_BREAK; }
    return HSA_STATUS_SUCCESS;
}
static hsa_amd_memory_pool_t g_pool; static bool g_pool_found = false;
static hsa_status_t collect_pool_cb(hsa_amd_memory_pool_t pool, void* d) {
    (void)d; hsa_amd_segment_t seg;
    hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &seg);
    if (seg == HSA_AMD_SEGMENT_GLOBAL) { g_pool = pool; g_pool_found = true; return HSA_STATUS_INFO_BREAK; }
    return HSA_STATUS_SUCCESS;
}

static int open_va(VADisplay* dpy, int* drm_fd) {
    *drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (*drm_fd < 0) { FAIL("open renderD128: %s", strerror(errno)); return 0; }
    *dpy = vaGetDisplayDRM(*drm_fd);
    int ma=0, mi=0;
    if (vaInitialize(*dpy, &ma, &mi) != VA_STATUS_SUCCESS) { FAIL("vaInitialize"); close(*drm_fd); return 0; }
    return 1;
}

int main() {
    printf("===========================================================\n");
    printf(" 实验 E：目标平台可移植性探测（5 项能力止损阀）\n");
    printf("===========================================================\n");

    // ---- P1：HIP + VAAPI 同进程共存 ----
    printf("\n[P1] HIP 与 VAAPI 同进程共存\n");
    hipError_t he = hipInit(0);
    if (he != hipSuccess) { FAIL("hipInit: %s", hipGetErrorString(he)); }
    else {
        int n = 0; hipGetDeviceCount(&n);
        INFO("HIP 初始化成功，设备数=%d", n);
    }
    VADisplay dpy; int drm_fd;
    if (open_va(&dpy, &drm_fd)) {
        PASS("P1 通过：HIP 与 VAAPI 同进程共存");
    }

    // ---- P2：HSA 私有导出 API 存在性 ----
    printf("\n[P2] HSA 私有导出 API（hsa_amd_portable_export_dmabuf）\n");
    hsa_init();
    g_gpu_found = false;
    hsa_iterate_agents(find_gpu_cb, nullptr);
    if (!g_gpu_found) { FAIL("P2 无 GPU agent"); }
    else {
        g_pool_found = false;
        hsa_amd_agent_iterate_memory_pools(g_gpu, collect_pool_cb, nullptr);
        if (!g_pool_found) { FAIL("P2 无 GLOBAL 内存池"); }
        else {
            size_t sz = 4096;
            void* p = nullptr;
            if (hsa_amd_memory_pool_allocate(g_pool, sz, 0, &p) != HSA_STATUS_SUCCESS) {
                FAIL("P2 池分配失败");
            } else {
                int fd = -1; uint64_t off = 0;
                hsa_status_t hs = hsa_amd_portable_export_dmabuf(p, sz, &fd, &off);
                if (hs != HSA_STATUS_SUCCESS)
                    hs = hsa_amd_portable_export_dmabuf_v2(p, sz, &fd, &off, 0);
                if (hs == HSA_STATUS_SUCCESS && fd >= 0) {
                    PASS("P2 通过：hsa_amd_portable_export_dmabuf 可用 (fd=%d)", fd);
                    close(fd);
                } else {
                    FAIL("P2 失败：HSA 私有导出不可用 (status=0x%x)，HIP→VAAPI 零拷贝需回退", hs);
                }
                hsa_amd_memory_pool_free(p);
            }
        }
    }

    // ---- P3：线性 NV12 surface 主动创建 ----
    printf("\n[P3] 线性 NV12 surface（modifier=LINEAR）\n");
    {
        uint64_t mods[] = { DRM_FORMAT_MOD_LINEAR };
        VADRMFormatModifierList ml = { 1, mods };
        VASurfaceAttrib a[2];
        a[0].type = VASurfaceAttribMemoryType; a[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        a[0].value.type = VAGenericValueTypeInteger; a[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
        a[1].type = VASurfaceAttribDRMFormatModifiers; a[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        a[1].value.type = VAGenericValueTypePointer; a[1].value.value.p = &ml;
        VASurfaceID s = VA_INVALID_ID;
        VAStatus vs = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, &s, 1, a, 2);
        if (vs != VA_STATUS_SUCCESS) { FAIL("P3 线性 NV12 创建失败: %d (%s)", vs, vaErrorStr(vs)); }
        else {
            VADRMPRIMESurfaceDescriptor d; memset(&d, 0, sizeof(d));
            vs = vaExportSurfaceHandle(dpy, s, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                       VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &d);
            if (vs != VA_STATUS_SUCCESS)
                vs = vaExportSurfaceHandle(dpy, s, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                           VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d);
            if (vs == VA_STATUS_SUCCESS && d.objects[0].drm_format_modifier == DRM_FORMAT_MOD_LINEAR) {
                PASS("P3 通过：线性 NV12 modifier=0x0, pitch=%u", d.layers[0].pitch[0]);
                for (uint32_t o = 0; o < d.num_objects; o++) close(d.objects[o].fd);
            } else {
                FAIL("P3 失败：未得到线性 modifier (0x%llx)", (unsigned long long)(vs==VA_STATUS_SUCCESS ? d.objects[0].drm_format_modifier : 0));
            }
            vaDestroySurfaces(dpy, &s, 1);
        }
    }

    // ---- P4：DMA-BUF 双向互导 ----
    printf("\n[P4] DMA-BUF 双向互导\n");
    {
        // P4a: VAAPI export → HIP import
        bool p4a = false, p4b = false;
        VASurfaceID s = VA_INVALID_ID;
        VAStatus vs = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, &s, 1, nullptr, 0);
        if (vs != VA_STATUS_SUCCESS) { FAIL("P4a VAAPI surface 创建失败"); }
        else {
            VADRMPRIMESurfaceDescriptor d; memset(&d, 0, sizeof(d));
            vs = vaExportSurfaceHandle(dpy, s, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                       VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &d);
            if (vs != VA_STATUS_SUCCESS)
                vs = vaExportSurfaceHandle(dpy, s, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                           VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d);
            if (vs != VA_STATUS_SUCCESS) { FAIL("P4a VAAPI export 失败"); }
            else {
                hipExternalMemoryHandleDesc hd = {};
                hd.type = hipExternalMemoryHandleTypeOpaqueFd;
                hd.handle.fd = d.objects[0].fd; hd.size = d.objects[0].size;
                hipExternalMemory_t em = nullptr;
                if (hipImportExternalMemory(&em, &hd) == hipSuccess) {
                    hipExternalMemoryBufferDesc bd = {}; bd.size = d.objects[0].size;
                    void* dp = nullptr;
                    if (hipExternalMemoryGetMappedBuffer(&dp, em, &bd) == hipSuccess) {
                        p4a = true; PASS("P4a VAAPI export → HIP import 成功");
                    }
                    hipDestroyExternalMemory(em);
                }
                for (uint32_t o = 0; o < d.num_objects; o++) close(d.objects[o].fd);
            }
            vaDestroySurfaces(dpy, &s, 1);
        }
        if (!p4a) FAIL("P4a VAAPI→HIP 失败");

        // P4b: HIP export → VAAPI import
        if (g_pool_found) {
            size_t sz = (size_t)512 * H * 3 / 2;
            void* p = nullptr;
            if (hsa_amd_memory_pool_allocate(g_pool, sz, 0, &p) == HSA_STATUS_SUCCESS) {
                int fd = -1; uint64_t off = 0;
                hsa_status_t hs = hsa_amd_portable_export_dmabuf(p, sz, &fd, &off);
                if (hs != HSA_STATUS_SUCCESS) hs = hsa_amd_portable_export_dmabuf_v2(p, sz, &fd, &off, 0);
                if (hs == HSA_STATUS_SUCCESS && fd >= 0) {
                    VADRMPRIMESurfaceDescriptor id; memset(&id, 0, sizeof(id));
                    id.fourcc = VA_FOURCC_NV12; id.width = W; id.height = H;
                    id.num_objects = 1; id.objects[0].fd = fd; id.objects[0].size = sz;
                    id.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
                    id.num_layers = 1; id.layers[0].drm_format = VA_FOURCC_NV12; id.layers[0].num_planes = 2;
                    id.layers[0].object_index[0] = 0; id.layers[0].offset[0] = 0;
                    id.layers[0].object_index[1] = 0; id.layers[0].offset[1] = 512 * H;
                    id.layers[0].pitch[0] = 512; id.layers[0].pitch[1] = 512;
                    VASurfaceAttrib ia[2];
                    ia[0].type = VASurfaceAttribMemoryType; ia[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
                    ia[0].value.type = VAGenericValueTypeInteger; ia[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
                    ia[1].type = VASurfaceAttribExternalBufferDescriptor; ia[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
                    ia[1].value.type = VAGenericValueTypePointer; ia[1].value.value.p = &id;
                    VASurfaceID is = VA_INVALID_ID;
                    VAStatus vv = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, &is, 1, ia, 2);
                    if (vv == VA_STATUS_SUCCESS) { p4b = true; PASS("P4b HIP export → VAAPI import 成功"); vaDestroySurfaces(dpy, &is, 1); }
                    else FAIL("P4b VAAPI 导入 HIP fd 失败: %d (%s)", vv, vaErrorStr(vv));
                    close(fd);
                }
                hsa_amd_memory_pool_free(p);
            }
        }
        if (!p4b) FAIL("P4b HIP→VAAPI 失败");
    }

    // ---- P5：DRM syncobj / sync_file ----
    printf("\n[P5] DRM syncobj / sync_file\n");
    {
        uint64_t has_syncobj = 0, has_timeline = 0;
        drmGetCap(drm_fd, DRM_CAP_SYNCOBJ, &has_syncobj);
        drmGetCap(drm_fd, DRM_CAP_SYNCOBJ_TIMELINE, &has_timeline);
        INFO("DRM_CAP_SYNCOBJ=%llu TIMELINE=%llu", (unsigned long long)has_syncobj, (unsigned long long)has_timeline);
        if (has_syncobj && has_timeline) PASS("P5 通过：syncobj + timeline 可用");
        else FAIL("P5 失败：syncobj/timeline 能力不足，异步 fence 需厂商协同");
    }

    vaTerminate(dpy);
    close(drm_fd);
    hsa_shut_down();

    printf("\n========== 汇总 ==========\n");
    printf("  PASS: %d   FAIL: %d\n", g_pass, g_fail);
    printf("\n  判定：");
    if (g_fail == 0) printf("5 项能力全部满足，可复用 AMD 实测结论进入目标平台复测\n");
    else printf("存在 %d 项能力缺口：对应零拷贝/同步链路须降级，触发 §1.7 第0步范围复审\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
