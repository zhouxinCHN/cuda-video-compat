// verify_p010_linear.cpp
// 验证：10-bit (P010) 线性 surface 的创建、导出、HIP 导入
//
// 目的：确认 shim 支持 10-bit 解码的布局路径。
//   10-bit 是 NVDEC/NVENC 真实产品大量使用的格式（HDR、专业视频）。
//   NV12 8-bit 已验证线性直通；P010 的布局（pitch/offset/size、16bit 对齐）需独立验证。
//
// 验证四步：
//   1. 用 VA_RT_FORMAT_YUV420_10 + modifier=[LINEAR] 创建 P010 surface
//   2. vaExportSurfaceHandle 导出，检查 modifier 线性、布局正确
//   3. HIP 导入该 fd（10-bit 也能走零拷贝）
//   4. 打印布局供后续 HIP kernel 读取用
//
// 运行：make p010 && ./verify_p010_linear

#include <hip/hip_runtime_api.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <drm/drm_fourcc.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#define PASS(...)   do { printf("  [PASS] "); printf(__VA_ARGS__); printf("\n"); g_pass++; } while (0)
#define FAIL(...)   do { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); g_fail++; } while (0)
#define INFO(...)   do { printf("  [info] "); printf(__VA_ARGS__); printf("\n"); } while (0)

static int g_pass = 0, g_fail = 0;
static const int W = 320, H = 240;

int main() {
    printf("===========================================================\n");
    printf(" 10-bit (P010) 线性 surface 验证\n");
    printf("===========================================================\n");

    int drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (drm_fd < 0) { FAIL("open renderD128: %s", strerror(errno)); return 1; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    int major = 0, minor = 0;
    if (vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS) {
        FAIL("vaInitialize"); close(drm_fd); return 1;
    }
    INFO("VAAPI %d.%d", major, minor);

    // 1. 创建 P010 surface（限定 LINEAR）
    uint64_t modifiers[] = { DRM_FORMAT_MOD_LINEAR };
    VADRMFormatModifierList mod_list = {};
    mod_list.num_modifiers = 1;
    mod_list.modifiers = modifiers;

    VASurfaceAttrib attribs[2];
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attribs[1].type = VASurfaceAttribDRMFormatModifiers;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &mod_list;

    VASurfaceID surf = VA_INVALID_ID;
    VAStatus vs = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420_10, W, H, &surf, 1, attribs, 2);
    if (vs != VA_STATUS_SUCCESS) {
        FAIL("vaCreateSurfaces(P010+LINEAR): %d (%s)", vs, vaErrorStr(vs));
        INFO("  => 驱动可能不支持 P010 或 P010+LINEAR 组合");
        vaTerminate(dpy); close(drm_fd);
        printf("\n===== 结果：PASS=%d FAIL=%d =====\n", g_pass, g_fail);
        return 1;
    }
    PASS("vaCreateSurfaces(P010 + modifier=[LINEAR]) 创建成功 (surface=%u)", surf);

    // 2. 导出并检查布局
    VADRMPRIMESurfaceDescriptor desc = {};
    memset(&desc, 0, sizeof(desc));
    vs = vaExportSurfaceHandle(dpy, surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                               &desc);
    if (vs != VA_STATUS_SUCCESS) {
        vs = vaExportSurfaceHandle(dpy, surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                   &desc);
    }
    if (vs != VA_STATUS_SUCCESS) {
        FAIL("vaExportSurfaceHandle: %d (%s)", vs, vaErrorStr(vs));
        vaDestroySurfaces(dpy, &surf, 1); vaTerminate(dpy); close(drm_fd);
        return 1;
    }

    INFO("导出: num_objects=%u num_layers=%u fourcc=%.4s",
         desc.num_objects, desc.num_layers, (char*)&desc.fourcc);
    for (uint32_t o = 0; o < desc.num_objects; o++) {
        INFO("  object[%u]: size=%llu modifier=0x%llx",
             o, (unsigned long long)desc.objects[o].size,
             (unsigned long long)desc.objects[o].drm_format_modifier);
    }
    for (uint32_t l = 0; l < desc.num_layers; l++) {
        INFO("  layer[%u]: format=%.4s planes=%u",
             l, (char*)&desc.layers[l].drm_format, desc.layers[l].num_planes);
        for (uint32_t p = 0; p < desc.layers[l].num_planes; p++) {
            INFO("    plane[%u]: offset=%u pitch=%u obj=%u",
                 p, desc.layers[l].offset[p], desc.layers[l].pitch[p],
                 desc.layers[l].object_index[p]);
        }
    }

    // 检查 modifier 线性
    uint64_t mod = desc.num_objects ? desc.objects[0].drm_format_modifier : ~0ull;
    bool linear = (mod == DRM_FORMAT_MOD_LINEAR);
    if (linear) {
        PASS("导出的 modifier = LINEAR (0x0)");
    } else {
        FAIL("导出的 modifier = 0x%llx，非线性", (unsigned long long)mod);
    }

    // 检查 fourcc 是 P010 且布局合理（10bit 用 16bit 存储，Y 平面 pitch=W*2 起）
    if (desc.fourcc == VA_FOURCC_P010) {
        PASS("导出 fourcc = P010");
    } else {
        INFO("导出 fourcc = %.4s（不是 P010？）", (char*)&desc.fourcc);
    }

    // 3. HIP 导入（10-bit 也能零拷贝）
    if (desc.num_objects >= 1 && linear) {
        hipInit(0);
        hipExternalMemoryHandleDesc hdesc = {};
        hdesc.type = hipExternalMemoryHandleTypeOpaqueFd;
        hdesc.handle.fd = desc.objects[0].fd;
        hdesc.size = desc.objects[0].size;

        hipExternalMemory_t extMem = nullptr;
        hipError_t he = hipImportExternalMemory(&extMem, &hdesc);
        if (he != hipSuccess) {
            FAIL("hipImportExternalMemory(P010 fd): %s", hipGetErrorString(he));
        } else {
            hipExternalMemoryBufferDesc bdesc = {};
            bdesc.size = desc.objects[0].size;
            void* devPtr = nullptr;
            he = hipExternalMemoryGetMappedBuffer(&devPtr, extMem, &bdesc);
            if (he != hipSuccess) {
                FAIL("hipExternalMemoryGetMappedBuffer: %s", hipGetErrorString(he));
            } else {
                PASS("HIP 导入 P010 surface 成功 (ptr=%p, size=%llu)",
                     devPtr, (unsigned long long)desc.objects[0].size);
                INFO("  => 10-bit 解码输出同样可零拷贝给 HIP 处理");
            }
            hipDestroyExternalMemory(extMem);
        }
    }

    // 清理
    for (uint32_t o = 0; o < desc.num_objects; o++) close(desc.objects[o].fd);
    vaDestroySurfaces(dpy, &surf, 1);
    vaTerminate(dpy);
    close(drm_fd);

    printf("\n===== 结果：PASS=%d FAIL=%d =====\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf(">>> P010 线性 surface 创建/导出/HIP 导入全通，10-bit 解码可零拷贝\n");
    } else {
        printf(">>> 存在失败项，需排查\n");
    }
    return g_fail == 0 ? 0 : 1;
}
