// retry_exp3_v2.cpp
// 实验3（HIP→VAAPI 边界2）失败的第二轮定位。
//
// 上一轮（retry_exp3）发现两个问题：
//   1. radeonsi 用「tiled + DCC」surface（modifier 0x200000028a01f04，320x240 NV12 占 512KB）。
//   2. 我的 R1 代码把 separate-layers 当 composed-layers 解析，pitch/offset 被覆盖了，
//      且 R2 把 tiled modifier 硬套在线性 buffer 上，导入必然失败。
//
// 本轮只做两件干净的事：
//   R3  往返自检：导出 VAAPI 自己的 surface，用【一模一样的 desc】重新导入。
//        若 FAIL → 我的导入描述符构造有 bug；若 PASS → 代码没问题，问题只出在 HSA buffer 上。
//   R4  线性导入：从 HSA 内存池分配，导出【线性】dma-buf，用【线性 modifier + 正确对齐 pitch】
//        构造 composed-NV12 描述符，逐个 pitch/池 尝试导入。
//        若某组合 PASS → 边界2 可零拷贝（生产实现改从 HSA 池分配 + 正确 pitch）。
//        若全 FAIL → 边界2 是根本性限制（VCN 无法吃线性内存），回退拷贝。
//
// 运行：make retry2  （或 ./retry_exp3_v2）

#include <hip/hip_runtime_api.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
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
#include <vector>
#include <string>

#define LOG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define PASS(...)   do { printf("  [PASS] "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define FAIL(...)   do { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define INFO(...)   do { printf("  [info] "); printf(__VA_ARGS__); printf("\n"); } while (0)

static const int W = 320, H = 240;

// ---------------- VAAPI 打开 ----------------
struct VaCtx {
    int drm_fd;
    VADisplay dpy;
    bool ok;
};
static VaCtx va_open() {
    VaCtx c; c.ok = false;
    c.drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (c.drm_fd < 0) { FAIL("open renderD128: %s", strerror(errno)); return c; }
    c.dpy = vaGetDisplayDRM(c.drm_fd);
    int ma=0, mi=0;
    if (vaInitialize(c.dpy, &ma, &mi) != VA_STATUS_SUCCESS) {
        FAIL("vaInitialize"); close(c.drm_fd); return c;
    }
    c.ok = true;
    return c;
}
static void va_close(VaCtx& c) {
    if (c.ok) { vaTerminate(c.dpy); close(c.drm_fd); c.ok = false; }
}

// ---------------- R3：往返自检 ----------------
// 导出 VAAPI 自己的 surface，用【同一个 desc】重新导入，验证导入代码是否正确。
static void r3_roundtrip() {
    printf("\n========== R3：往返自检（导出后原样重新导入） ==========\n");
    VaCtx c = va_open();
    if (!c.ok) return;

    VASurfaceID surf = VA_INVALID_ID;
    VAStatus vs = vaCreateSurfaces(c.dpy, VA_RT_FORMAT_YUV420, W, H, &surf, 1, nullptr, 0);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaCreateSurfaces: %d", vs); va_close(c); return; }

    VADRMPRIMESurfaceDescriptor desc; memset(&desc, 0, sizeof(desc));
    vs = vaExportSurfaceHandle(c.dpy, surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    if (vs != VA_STATUS_SUCCESS) {
        vs = vaExportSurfaceHandle(c.dpy, surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &desc);
    }
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaExportSurfaceHandle: %d", vs); vaDestroySurfaces(c.dpy,&surf,1); va_close(c); return; }
    INFO("导出成功 num_objects=%u num_layers=%u modifier=0x%llx",
         desc.num_objects, desc.num_layers, (unsigned long long)desc.objects[0].drm_format_modifier);

    // 用同一个 desc 重新导入（保持 fd 打开）
    VASurfaceAttrib attribs[2];
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &desc;

    VASurfaceID surf2 = VA_INVALID_ID;
    vs = vaCreateSurfaces(c.dpy, VA_RT_FORMAT_YUV420, W, H, &surf2, 1, attribs, 2);
    if (vs != VA_STATUS_SUCCESS) {
        FAIL("往返重导入失败: status=%d (%s)  => 导入描述符构造有 bug，需先修代码", vs, vaErrorStr(vs));
    } else {
        PASS("往返重导入成功 (surface id=%u)  => 导入代码正确，问题只在 HSA buffer 上", surf2);
        vaDestroySurfaces(c.dpy, &surf2, 1);
    }

    for (uint32_t o = 0; o < desc.num_objects; o++) close(desc.objects[o].fd);
    vaDestroySurfaces(c.dpy, &surf, 1);
    va_close(c);
}

// ---------------- R4：线性导入 ----------------
struct PoolInfo {
    hsa_amd_memory_pool_t pool;
    hsa_amd_segment_t segment;
    uint32_t flags;
};
static std::vector<PoolInfo> g_pools;
static hsa_status_t collect_pool_cb(hsa_amd_memory_pool_t pool, void* data) {
    (void)data;
    PoolInfo p; p.pool = pool;
    hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &p.segment);
    uint32_t f=0; hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &f);
    p.flags = f;
    g_pools.push_back(p);
    return HSA_STATUS_SUCCESS;
}
static hsa_agent_t g_gpu; static bool g_gpu_found=false;
static hsa_status_t find_gpu_cb(hsa_agent_t a, void* d) {
    (void)d; hsa_device_type_t t; hsa_agent_get_info(a, HSA_AGENT_INFO_DEVICE, &t);
    if (t == HSA_DEVICE_TYPE_GPU) { g_gpu = a; g_gpu_found = true; return HSA_STATUS_INFO_BREAK; }
    return HSA_STATUS_SUCCESS;
}

// 用给定 pitch 构造 composed-NV12 导入描述符，尝试导入
static bool try_import_linear(VaCtx& c, int fd, size_t size, uint32_t pitch,
                              const char* tag) {
    VADRMPRIMESurfaceDescriptor desc; memset(&desc, 0, sizeof(desc));
    desc.fourcc = VA_FOURCC_NV12;
    desc.width = W; desc.height = H;
    desc.num_objects = 1;
    desc.objects[0].fd = fd;
    desc.objects[0].size = size;
    desc.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;  // 0 = 线性
    desc.num_layers = 1;
    desc.layers[0].drm_format = VA_FOURCC_NV12;
    desc.layers[0].num_planes = 2;
    desc.layers[0].object_index[0] = 0;
    desc.layers[0].offset[0] = 0;
    desc.layers[0].object_index[1] = 0;
    desc.layers[0].offset[1] = pitch * H;   // Y 之后是交错的 UV
    desc.layers[0].pitch[0] = pitch;
    desc.layers[0].pitch[1] = pitch;

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
    VAStatus vs = vaCreateSurfaces(c.dpy, VA_RT_FORMAT_YUV420, W, H, &surf, 1, attribs, 2);
    if (vs != VA_STATUS_SUCCESS) {
        FAIL("[%s] pitch=%u import FAIL: status=%d (%s)", tag, pitch, vs, vaErrorStr(vs));
        return false;
    }
    PASS("[%s] pitch=%u import 成功 (surface id=%u)", tag, pitch, surf);
    vaDestroySurfaces(c.dpy, &surf, 1);
    return true;
}

static void r4_linear_import() {
    printf("\n========== R4：线性导入（HSA 线性 buffer → VAAPI，正确 pitch） ==========\n");
    hsa_init();
    g_gpu_found = false;
    hsa_iterate_agents(find_gpu_cb, nullptr);
    if (!g_gpu_found) { FAIL("无 GPU agent"); return; }
    g_pools.clear();
    hsa_amd_agent_iterate_memory_pools(g_gpu, collect_pool_cb, nullptr);

    // pitch 对齐候选：320 向上对齐到 256/512
    const uint32_t pitches[] = { 512, 256, 1024, 320 };
    bool any_pass = false;

    VaCtx c = va_open();
    if (!c.ok) return;

    for (size_t pi = 0; pi < g_pools.size(); pi++) {
        PoolInfo& p = g_pools[pi];
        if (p.segment != HSA_AMD_SEGMENT_GLOBAL) continue;
        for (uint32_t pitch : pitches) {
            size_t size = (size_t)pitch * H * 3 / 2;  // NV12: Y + 半高 UV
            void* ptr = nullptr;
            hsa_status_t hs = hsa_amd_memory_pool_allocate(p.pool, size, 0, &ptr);
            if (hs != HSA_STATUS_SUCCESS || !ptr) { FAIL("池[%zu] alloc %zu: 0x%x", pi, size, hs); continue; }
            int fd = -1; uint64_t off = 0;
            hs = hsa_amd_portable_export_dmabuf(ptr, size, &fd, &off);
            if (hs != HSA_STATUS_SUCCESS) hs = hsa_amd_portable_export_dmabuf_v2(ptr, size, &fd, &off, 0);
            if (hs != HSA_STATUS_SUCCESS || fd < 0) {
                FAIL("池[%zu] pitch=%u 导出失败: 0x%x", pi, pitch, hs);
                hsa_amd_memory_pool_free(ptr); continue;
            }
            char tag[96];
            snprintf(tag, sizeof(tag), "池[%zu]_flags=0x%x_size=%zu", pi, p.flags, size);
            bool ok = try_import_linear(c, fd, size, pitch, tag);
            if (ok) any_pass = true;
            close(fd);
            hsa_amd_memory_pool_free(ptr);
        }
    }
    va_close(c);

    printf("\n  ==== R4 结论 ====\n");
    if (any_pass) {
        printf("  => 找到可导入的 pitch/池组合！边界2 可零拷贝，生产实现改用 HSA 池分配 + 该 pitch。\n");
    } else {
        printf("  => 所有线性导入都失败，边界2 是根本性限制（VCN 不吃线性内存），回退拷贝路径。\n");
    }
}

int main() {
    printf("===========================================================\n");
    printf(" 实验3 边界2 定位 v2：往返自检 + 线性导入\n");
    printf("===========================================================\n");
    r3_roundtrip();
    r4_linear_import();
    return 0;
}
