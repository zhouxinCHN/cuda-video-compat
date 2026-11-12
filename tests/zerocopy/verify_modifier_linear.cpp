// verify_modifier_linear.cpp
// 验证：用 VASurfaceAttribDRMFormatModifiers 主动限定 DRM_FORMAT_MOD_LINEAR
//       创建 VAAPI surface，导出后是否真是线性布局。
//
// 这是 shim 生产路径的关键验证：
//   AMD_DEBUG=notiling 是全局 debug 开关，不能作为生产依赖。
//   生产应在 vaCreateSurfaces 时主动声明 modifier，得到线性 surface。
//
// 验证三步：
//   1. 用 DRMFormatModifiers=[LINEAR] 属性创建 NV12 surface
//   2. vaExportSurfaceHandle 导出，检查 modifier == DRM_FORMAT_MOD_LINEAR(0)
//   3. 用该 surface 创建 context，确认可作解码输出用（对象可建）
//
// 运行：make modifier-linear && ./verify_modifier_linear

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
    printf(" 生产路径验证：modifier 限定 LINEAR 创建 VAAPI surface\n");
    printf("===========================================================\n");

    int drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (drm_fd < 0) { FAIL("open renderD128: %s", strerror(errno)); return 1; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    int major = 0, minor = 0;
    if (vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS) {
        FAIL("vaInitialize"); close(drm_fd); return 1;
    }
    INFO("VAAPI %d.%d", major, minor);

    // 1. 构造 modifier 列表：只含 LINEAR
    uint64_t modifiers[] = { DRM_FORMAT_MOD_LINEAR };
    VADRMFormatModifierList mod_list = {};
    mod_list.num_modifiers = 1;
    mod_list.modifiers = modifiers;

    // 2. 属性：限定 DRM_PRIME_2 内存类型 + modifier 列表
    //    （Mesa 实现：VASurfaceAttribDRMFormatModifiers 走 vlVaHandleSurfaceAllocate）
    VASurfaceAttrib attribs[2];
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attribs[1].type = VASurfaceAttribDRMFormatModifiers;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &mod_list;

    // 3. 创建 surface
    VASurfaceID surf = VA_INVALID_ID;
    VAStatus vs = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, &surf, 1, attribs, 2);
    if (vs != VA_STATUS_SUCCESS) {
        FAIL("vaCreateSurfaces(modifier=LINEAR): %d (%s)", vs, vaErrorStr(vs));
        INFO("  => 该驱动可能不支持 VASurfaceAttribDRMFormatModifiers");
        INFO("  => 或该格式/RT 不接受 modifier 限定");
        vaTerminate(dpy); close(drm_fd);
        printf("\n===== 结果：PASS=%d FAIL=%d =====\n", g_pass, g_fail);
        return 1;
    }
    PASS("vaCreateSurfaces(modifier=[LINEAR]) 创建成功 (surface=%u)", surf);

    // 4. 导出检查 modifier
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
        vaDestroySurfaces(dpy, &surf, 1);
        vaTerminate(dpy); close(drm_fd);
        return 1;
    }

    INFO("导出: num_objects=%u num_layers=%u", desc.num_objects, desc.num_layers);
    for (uint32_t o = 0; o < desc.num_objects; o++) {
        INFO("  object[%u]: size=%llu modifier=0x%llx",
             o, (unsigned long long)desc.objects[o].size,
             (unsigned long long)desc.objects[o].drm_format_modifier);
    }
    for (uint32_t l = 0; l < desc.num_layers; l++) {
        INFO("  layer[%u]: format=%.4s planes=%u", l,
             (char*)&desc.layers[l].drm_format, desc.layers[l].num_planes);
        for (uint32_t p = 0; p < desc.layers[l].num_planes; p++) {
            INFO("    plane[%u]: offset=%u pitch=%u obj=%u",
                 p, desc.layers[l].offset[p], desc.layers[l].pitch[p],
                 desc.layers[l].object_index[p]);
        }
    }

    // 检查 modifier 是否线性
    uint64_t mod = desc.num_objects ? desc.objects[0].drm_format_modifier : ~0ull;
    if (mod == DRM_FORMAT_MOD_LINEAR) {
        PASS("导出的 modifier = DRM_FORMAT_MOD_LINEAR(0x0) —— 生产路径可得到线性 surface");
    } else {
        FAIL("导出的 modifier = 0x%llx，不是线性！", (unsigned long long)mod);
        INFO("  => 主动声明 LINEAR 未生效，需要进一步排查");
    }

    // 关闭导出 fd
    for (uint32_t o = 0; o < desc.num_objects; o++) close(desc.objects[o].fd);

    // 5. 用该 surface 创建解码 context（验证可作解码输出/DPB 面）
    VAConfigID config = VA_INVALID_ID;
    VASurfaceID all_surfs[1] = { surf };
    VAContextID ctx = VA_INVALID_ID;

    VAConfigAttrib cfg_attr;
    cfg_attr.type = VAConfigAttribRTFormat;
    vs = vaGetConfigAttributes(dpy, VAProfileH264Main, VAEntrypointVLD, &cfg_attr, 1);
    if (vs != VA_STATUS_SUCCESS) {
        FAIL("vaGetConfigAttributes: %d", vs);
    } else {
        vs = vaCreateConfig(dpy, VAProfileH264Main, VAEntrypointVLD, &cfg_attr, 1, &config);
        if (vs != VA_STATUS_SUCCESS) {
            FAIL("vaCreateConfig: %d", vs);
        } else {
            vs = vaCreateContext(dpy, config, W, H, VA_PROGRESSIVE, all_surfs, 1, &ctx);
            if (vs != VA_STATUS_SUCCESS) {
                FAIL("vaCreateContext(用 modifier-linear surface): %d (%s)", vs, vaErrorStr(vs));
            } else {
                PASS("vaCreateContext 用该线性 surface 创建成功 —— 可作解码输出/DPB 面");
                vaDestroyContext(dpy, ctx);
            }
            vaDestroyConfig(dpy, config);
        }
    }

    vaDestroySurfaces(dpy, &surf, 1);
    vaTerminate(dpy);
    close(drm_fd);

    printf("\n===== 结果：PASS=%d FAIL=%d =====\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf(">>> 生产路径确认：主动用 modifier=LINEAR 可得到线性 surface，可用于解码输出\n");
    } else {
        printf(">>> 存在失败项，需排查\n");
    }
    return g_fail == 0 ? 0 : 1;
}