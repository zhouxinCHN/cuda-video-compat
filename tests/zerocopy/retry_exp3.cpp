// retry_exp3.cpp
// 针对 实验3（HIP → VAAPI，计算→编码方向）失败 的定位程序。
//
// 失败现象：hsa_amd_portable_export_dmabuf 成功导出 fd，但 vaCreateSurfaces 导入失败
//          status=2 (VA_STATUS_ERROR_ALLOCATION_FAILED)。
//
// 定位策略（"向 VAAPI 自己学习"）：
//   R0: 打印 HSA 内存池拓扑（segment/flags/size），搞清 hipMalloc 与 VRAM 池的区别。
//   R1: 用 VAAPI 自己创建一个 surface 并导出，打印它的真实 pitch/modifier/offset/size，
//       拿到 radeonsi 实际接受的参数（这是"标准答案"）。
//   R2: 从多个 HSA 内存池（VRAM coarse-grained / 系统 fine-grained / hipMalloc 对照）分配，
//       用 R1 学到的 pitch 导出 dma-buf，逐个尝试 VAAPI 导入，找出哪个池能成。
//
// 运行：make retry  （或 ./retry_exp3）
//
// 依赖同 verify_zerocopy：ROCm(HIP/HSA) + libva + libdrm。

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

// ---------------- 工具：HSA 内存池 ----------------
struct PoolInfo {
    hsa_amd_memory_pool_t pool;
    hsa_amd_segment_t segment;
    uint32_t flags;
    uint64_t size;
    uint64_t alloc_max;
    bool accessible_all;
};

static hsa_agent_t g_gpu_agent;
static bool g_gpu_found = false;

static hsa_status_t find_gpu_cb(hsa_agent_t agent, void* data) {
    (void)data;
    hsa_device_type_t type;
    hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type);
    if (type == HSA_DEVICE_TYPE_GPU) {
        g_gpu_agent = agent;
        g_gpu_found = true;
        return HSA_STATUS_INFO_BREAK;
    }
    return HSA_STATUS_SUCCESS;
}

static std::vector<PoolInfo> g_pools;
static hsa_status_t collect_pool_cb(hsa_amd_memory_pool_t pool, void* data) {
    (void)data;
    PoolInfo p;
    p.pool = pool;
    hsa_amd_segment_t seg;
    hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &seg);
    p.segment = seg;
    uint32_t flags = 0;
    hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &flags);
    p.flags = flags;
    uint64_t sz = 0;
    hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SIZE, &sz);
    p.size = sz;
    uint64_t am = 0;
    hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_ALLOC_MAX_SIZE, &am);
    p.alloc_max = am;
    uint32_t acc = 0;
    hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_ACCESSIBLE_BY_ALL, &acc);
    p.accessible_all = (acc != 0);
    g_pools.push_back(p);
    return HSA_STATUS_SUCCESS;
}

static const char* segName(hsa_amd_segment_t s) {
    switch (s) {
        case HSA_AMD_SEGMENT_GLOBAL: return "GLOBAL";
        case HSA_AMD_SEGMENT_READONLY: return "READONLY";
        case HSA_AMD_SEGMENT_PRIVATE: return "PRIVATE";
        case HSA_AMD_SEGMENT_GROUP: return "GROUP";
        default: return "?";
    }
}

// ---------------- R0：内存池拓扑 ----------------
static void r0_print_pools() {
    printf("\n========== R0：HSA 内存池拓扑 ==========\n");
    hsa_init();
    g_gpu_found = false;
    hsa_iterate_agents(find_gpu_cb, nullptr);
    if (!g_gpu_found) { FAIL("未找到 GPU agent"); return; }
    g_pools.clear();
    hsa_amd_agent_iterate_memory_pools(g_gpu_agent, collect_pool_cb, nullptr);
    INFO("GPU agent 下共 %zu 个内存池：", g_pools.size());
    for (size_t i = 0; i < g_pools.size(); i++) {
        PoolInfo& p = g_pools[i];
        INFO("池[%zu] seg=%s flags=0x%x size=%llu alloc_max=%llu accessible_all=%d",
             i, segName(p.segment), p.flags,
             (unsigned long long)p.size, (unsigned long long)p.alloc_max,
             p.accessible_all);
    }
}

// ---------------- R1：向 VAAPI 学习正确参数 ----------------
struct LearnResult {
    bool ok;
    uint64_t modifier;
    uint32_t pitch[2];
    uint32_t offset[2];
    uint32_t num_objects;
    uint32_t num_layers;
    uint64_t object_size;
    uint32_t fourcc;
};

static LearnResult r1_learn_vaapi() {
    printf("\n========== R1：向 VAAPI 学习正确 surface 参数 ==========\n");
    LearnResult lr; memset(&lr, 0, sizeof(lr)); lr.ok = false;

    int drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (drm_fd < 0) { FAIL("open renderD128: %s", strerror(errno)); return lr; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    int major=0, minor=0;
    if (vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS) {
        FAIL("vaInitialize"); close(drm_fd); return lr;
    }

    VASurfaceID surf = VA_INVALID_ID;
    VAStatus vs = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, &surf, 1, nullptr, 0);
    if (vs != VA_STATUS_SUCCESS) {
        FAIL("vaCreateSurfaces: %d (%s)", vs, vaErrorStr(vs));
        vaTerminate(dpy); close(drm_fd); return lr;
    }

    VADRMPRIMESurfaceDescriptor desc; memset(&desc, 0, sizeof(desc));
    vs = vaExportSurfaceHandle(dpy, surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    if (vs != VA_STATUS_SUCCESS) {
        vs = vaExportSurfaceHandle(dpy, surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &desc);
    }
    if (vs != VA_STATUS_SUCCESS) {
        FAIL("vaExportSurfaceHandle: %d (%s)", vs, vaErrorStr(vs));
        vaDestroySurfaces(dpy, &surf, 1); vaTerminate(dpy); close(drm_fd); return lr;
    }

    lr.ok = true;
    lr.fourcc = desc.fourcc;
    lr.modifier = desc.objects[0].drm_format_modifier;
    lr.num_objects = desc.num_objects;
    lr.num_layers = desc.num_layers;
    lr.object_size = desc.objects[0].size;
    PASS("VAAPI 自建 surface 导出成功，参数如下（这是 radeonsi 的标准答案）：");
    INFO("fourcc=%.4s num_objects=%u num_layers=%u",
         (char*)&desc.fourcc, desc.num_objects, desc.num_layers);
    for (uint32_t o = 0; o < desc.num_objects; o++) {
        INFO("  object[%u]: fd=%d size=%llu modifier=0x%llx",
             o, desc.objects[o].fd, (unsigned long long)desc.objects[o].size,
             (unsigned long long)desc.objects[o].drm_format_modifier);
    }
    for (uint32_t l = 0; l < desc.num_layers; l++) {
        INFO("  layer[%u]: format=%.4s num_planes=%u",
             l, (char*)&desc.layers[l].drm_format, desc.layers[l].num_planes);
        for (uint32_t pl = 0; pl < desc.layers[l].num_planes; pl++) {
            INFO("    plane[%u]: obj_idx=%u offset=%u pitch=%u",
                 pl, desc.layers[l].object_index[pl],
                 desc.layers[l].offset[pl], desc.layers[l].pitch[pl]);
            lr.pitch[pl] = desc.layers[l].pitch[pl];
            lr.offset[pl] = desc.layers[l].offset[pl];
        }
    }

    // 关闭导出 fd（本程序不再使用）
    for (uint32_t o = 0; o < desc.num_objects; o++) close(desc.objects[o].fd);
    vaDestroySurfaces(dpy, &surf, 1);
    vaTerminate(dpy);
    close(drm_fd);
    return lr;
}

// ---------------- R2：多池分配 + 导入 ----------------
// 用给定内存池分配 NV12 buffer，导出 dma-buf，用 R1 学到的参数尝试 VAAPI 导入。
static bool r2_try_import_from_pool(const char* tag, hsa_amd_memory_pool_t pool,
                                    size_t alloc_size, const LearnResult& lr) {
    // 1. HSA 池分配
    void* ptr = nullptr;
    hsa_status_t hs = hsa_amd_memory_pool_allocate(pool, alloc_size, 0, &ptr);
    if (hs != HSA_STATUS_SUCCESS || !ptr) {
        FAIL("[%s] hsa_amd_memory_pool_allocate: 0x%x", tag, hs);
        return false;
    }

    // 2. 导出 dma-buf
    int fd = -1; uint64_t off = 0;
    hs = hsa_amd_portable_export_dmabuf(ptr, alloc_size, &fd, &off);
    if (hs != HSA_STATUS_SUCCESS) {
        hs = hsa_amd_portable_export_dmabuf_v2(ptr, alloc_size, &fd, &off, 0);
    }
    if (hs != HSA_STATUS_SUCCESS || fd < 0) {
        FAIL("[%s] 导出 dma-buf 失败: 0x%x", tag, hs);
        hsa_amd_memory_pool_free(ptr);
        return false;
    }
    INFO("[%s] 分配 %zu 字节 + 导出 fd=%d 成功", tag, alloc_size, fd);

    // 3. 构造 VADRMPRIMESurfaceDescriptor（用 R1 学到的 pitch/offset/modifier）
    VADRMPRIMESurfaceDescriptor desc; memset(&desc, 0, sizeof(desc));
    desc.fourcc = lr.fourcc ? lr.fourcc : VA_FOURCC_NV12;
    desc.width = W;
    desc.height = H;
    desc.num_objects = 1;
    desc.objects[0].fd = fd;
    desc.objects[0].size = alloc_size;
    desc.objects[0].drm_format_modifier = lr.modifier;  // 用 VAAPI 自己的 modifier
    desc.num_layers = 1;
    desc.layers[0].drm_format = desc.fourcc;
    desc.layers[0].num_planes = 2;
    desc.layers[0].object_index[0] = 0;
    desc.layers[0].offset[0] = lr.offset[0];
    desc.layers[0].object_index[1] = 0;
    desc.layers[0].offset[1] = lr.offset[1];
    desc.layers[0].pitch[0] = lr.pitch[0];
    desc.layers[0].pitch[1] = lr.pitch[1];

    // 4. VAAPI 导入
    int drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (drm_fd < 0) { FAIL("[%s] open renderD128: %s", tag, strerror(errno)); close(fd); hsa_amd_memory_pool_free(ptr); return false; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    int major=0, minor=0;
    if (vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS) {
        FAIL("[%s] vaInitialize", tag); close(fd); close(drm_fd); hsa_amd_memory_pool_free(ptr); return false;
    }

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
    VAStatus vs = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, &surf, 1, attribs, 2);
    if (vs != VA_STATUS_SUCCESS) {
        FAIL("[%s] vaCreateSurfaces(import): status=%d (%s)  modifier=0x%llx pitchY=%u pitchUV=%u",
             tag, vs, vaErrorStr(vs),
             (unsigned long long)lr.modifier, lr.pitch[0], lr.pitch[1]);
        close(fd); vaTerminate(dpy); close(drm_fd); hsa_amd_memory_pool_free(ptr); return false;
    }
    PASS("[%s] VAAPI 导入成功！ surface id=%u", tag, surf);

    vaDestroySurfaces(dpy, &surf, 1);
    vaTerminate(dpy);
    close(fd);
    close(drm_fd);
    hsa_amd_memory_pool_free(ptr);
    return true;
}

// ---------------- 主流程 ----------------
int main() {
    printf("===========================================================\n");
    printf(" 实验3 失败定位：HIP(HSA) 内存池 → VAAPI 导入\n");
    printf("===========================================================\n");

    r0_print_pools();

    LearnResult lr = r1_learn_vaapi();
    if (!lr.ok) {
        printf("\nR1 失败，无法拿到标准答案，终止。\n");
        return 1;
    }
    // 计算标准答案下的最小 alloc 大小
    size_t min_size = 0;
    if (lr.offset[1] > lr.offset[0]) {
        min_size = lr.offset[1] + (size_t)lr.pitch[1] * (H / 2);
    } else {
        min_size = (size_t)lr.pitch[0] * H * 3 / 2;
    }
    INFO("由 R1 标准答案推算最小 buffer 大小 ≈ %zu 字节", min_size);
    // 留余量（对齐 4K）
    size_t alloc_size = (min_size + 4095) & ~(size_t)4095;

    printf("\n========== R2：逐个内存池尝试 VAAPI 导入 ==========\n");
    int success_count = 0;
    for (size_t i = 0; i < g_pools.size(); i++) {
        PoolInfo& p = g_pools[i];
        // 只对 GLOBAL 段且 size 足够的池尝试
        if (p.segment != HSA_AMD_SEGMENT_GLOBAL) continue;
        if (p.alloc_max != 0 && p.alloc_max < alloc_size) continue;
        char tag[64];
        snprintf(tag, sizeof(tag), "池[%zu]_seg=%s_flags=0x%x", i, segName(p.segment), p.flags);
        bool ok = r2_try_import_from_pool(tag, p.pool, alloc_size, lr);
        if (ok) success_count++;
    }

    printf("\n========== 定位结论 ==========\n");
    printf("  成功导入的内存池数：%d / %zu\n", success_count, g_pools.size());
    if (success_count > 0) {
        printf("  => 找到可用的内存池！请把对应 [PASS] 行的池信息回传，\n");
        printf("     生产实现应改用 hsa_amd_memory_pool_allocate 从该池分配，而非 hipMalloc。\n");
    } else {
        printf("  => 所有池都导入失败。可能原因：\n");
        printf("     1) 该 GPU 的视频引擎无法访问 HSA 可导出的内存（根本性限制）；\n");
        printf("     2) 需要额外的 modifier / tiling 信息（见 R1 打印的 modifier）。\n");
        printf("     此时边界2 需回退拷贝路径（实验3 已知，非致命）。\n");
    }
    return success_count > 0 ? 0 : 1;
}
