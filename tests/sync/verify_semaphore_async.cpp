// verify_semaphore_async.cpp
// 异步跨栈同步探测：HIP semaphore interop + DRM syncobj/sync_file
//
// 目的：确认"异步跨栈同步"在 ROCm/HIP + Mesa 上的真实可行路径。
//   (1) HIP 外部 semaphore interop 是否支持（OpaqueFd / TimelineSemaphoreFd）
//   (2) timelineSemaphoreInteropSupported 的真实值
//   (3) DRM syncobj + sync_file 跨驱动 fence 通路是否可用（Linux 标准做法）
//
// 背景（已调研确认）：
//   - hipImportExternalSemaphore 头文件注释 "currently not supported on Linux"
//   - 但 hipDeviceProp_t 有 timelineSemaphoreInteropSupported 字段
//   - DRM ioctl SYNCOBJ_HANDLE_TO_FD/FD_TO_HANDLE + IMPORT/EXPORT_SYNC_FILE 是
//     Linux 跨驱动同步的标准机制，Mesa radeonsi 有 si_fence.c
//
// 运行：make && ./verify_semaphore_async

#include <hip/hip_runtime_api.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <cstdio>
#include <cstring>
#include <cerrno>

#include <drm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>

#define PASS(...)   do { printf("  [PASS] "); printf(__VA_ARGS__); printf("\n"); g_pass++; } while (0)
#define FAIL(...)   do { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); g_fail++; } while (0)
#define INFO(...)   do { printf("  [info] "); printf(__VA_ARGS__); printf("\n"); } while (0)

static int g_pass = 0, g_fail = 0;

// ---------- (1) HIP 外部 semaphore interop ----------
static void test_hip_semaphore() {
    printf("\n========== (1) HIP 外部 semaphore interop ==========\n");

    if (hipInit(0) != hipSuccess) { FAIL("hipInit"); return; }
    int dev = 0;
    if (hipGetDevice(&dev) != hipSuccess) { FAIL("hipGetDevice"); return; }

    // 查 timelineSemaphoreInteropSupported
    hipDeviceProp_t prop;
    hipError_t he = hipGetDeviceProperties(&prop, dev);
    if (he != hipSuccess) {
        FAIL("hipGetDeviceProperties: %s", hipGetErrorString(he));
    } else {
        INFO("timelineSemaphoreInteropSupported = %d", prop.timelineSemaphoreInteropSupported);
        if (prop.timelineSemaphoreInteropSupported) {
            PASS("设备宣称支持 timeline semaphore interop");
        } else {
            FAIL("设备宣称 timeline semaphore interop 不支持");
        }
    }

    // 尝试 timeline semaphore fd import（用一个匿名 fd 测试路径）
    // 即使 fd 无效，若 API 本身在 Linux 不支持，会返回 hipErrorNotSupported 而非无效句柄
    int fd = -1;
    // 用 memfd 造一个假 fd（仅探测 API 是否被实现；timeline 需真实 semaphore fd）
    fd = syscall(SYS_memfd_create, "probe", 0);
    if (fd < 0) { FAIL("memfd_create"); return; }

    hipExternalSemaphoreHandleDesc desc = {};
    desc.type = hipExternalSemaphoreHandleTypeTimelineSemaphoreFd;
    desc.handle.fd = fd;

    hipExternalSemaphore_t extSem = nullptr;
    he = hipImportExternalSemaphore(&extSem, &desc);
    if (he == hipSuccess) {
        // 若竟然成功，则继续 signal/wait
        PASS("hipImportExternalSemaphore(TimelineSemaphoreFd) 成功");
        hipExternalSemaphoreSignalParams sp = {};
        sp.params.fence.value = 1;
        he = hipSignalExternalSemaphoresAsync(&extSem, &sp, 1, nullptr);
        if (he == hipSuccess) PASS("hipSignalExternalSemaphoresAsync 成功");
        else FAIL("hipSignalExternalSemaphoresAsync: %s (%d)", hipGetErrorString(he), (int)he);
        hipDestroyExternalSemaphore(extSem);
    } else {
        INFO("hipImportExternalSemaphore(TimelineSemaphoreFd) 失败: %s (%d)",
             hipGetErrorString(he), (int)he);
        // 区分：是"不支持"(801) 还是"无效句柄"
        if (he == hipErrorNotSupported) {
            FAIL("确认：HIP 外部 semaphore 在 Linux 不受支持 (hipErrorNotSupported=%d)", (int)he);
        } else if (he == hipErrorInvalidValue) {
            INFO("是无效值（可能仅因 fd 不是真 timeline semaphore），API 本身可能有实现");
            FAIL("无法据此判定 API 是否支持；需真实 timeline semaphore fd 再试");
        } else {
            FAIL("错误类型: %s (%d)", hipGetErrorString(he), (int)he);
        }
    }
    close(fd);
}

// ---------- (2) DRM syncobj + sync_file 跨驱动 fence 通路 ----------
static void test_drm_syncobj() {
    printf("\n========== (2) DRM syncobj / sync_file 通路 ==========\n");

    int drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (drm_fd < 0) { FAIL("open renderD128: %s", strerror(errno)); return; }

    // 检查 syncobj 能力
    uint64_t has_syncobj = 0, has_timeline = 0;
    drmGetCap(drm_fd, DRM_CAP_SYNCOBJ, &has_syncobj);
    drmGetCap(drm_fd, DRM_CAP_SYNCOBJ_TIMELINE, &has_timeline);
    INFO("DRM_CAP_SYNCOBJ = %llu, DRM_CAP_SYNCOBJ_TIMELINE = %llu",
         (unsigned long long)has_syncobj, (unsigned long long)has_timeline);
    if (has_syncobj) PASS("驱动支持 syncobj");
    else FAIL("驱动不支持 syncobj");

    // 创建 syncobj
    struct drm_syncobj_create create = { .flags = 0 };
    int ioctl_r = drmIoctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create);
    if (ioctl_r < 0) {
        FAIL("DRM_IOCTL_SYNCOBJ_CREATE: %s", strerror(errno));
        close(drm_fd); return;
    }
    uint32_t syncobj = create.handle;
    PASS("syncobj 创建成功 (handle=%u)", syncobj);

    // 导出为 sync_file fd
    struct drm_syncobj_handle to_fd;
    memset(&to_fd, 0, sizeof(to_fd));
    to_fd.handle = syncobj;
    to_fd.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    ioctl_r = drmIoctl(drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &to_fd);
    if (ioctl_r < 0) {
        FAIL("SYNCOBJ_HANDLE_TO_FD(EXPORT_SYNC_FILE): %s", strerror(errno));
    } else {
        int syncfile = to_fd.fd;
        PASS("syncobj → sync_file fd=%d 导出成功", syncfile);
        close(syncfile);
    }

    // 导入一个 sync_file 为 syncobj（用 memfd 不行，这里验证 ioctl 可达性）
    struct drm_syncobj_handle fd_to;
    memset(&fd_to, 0, sizeof(fd_to));
    fd_to.fd = -1;   // 无效 fd 会报错，但能验证路径存在
    fd_to.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    ioctl_r = drmIoctl(drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &fd_to);
    if (ioctl_r < 0) {
        // EBADF 说明路径被接受，只是 fd 无效；ENOTTY/EINVAL 才说明不支持
        if (errno == EBADF)
            INFO("SYNCOBJ_FD_TO_HANDLE 路径存在（无效 fd 报 EBADF，符合预期）");
        else if (errno == ENOTTY || errno == EINVAL)
            FAIL("SYNCOBJ_FD_TO_HANDLE(IMPORT_SYNC_FILE): %s", strerror(errno));
        else
            INFO("SYNCOBJ_FD_TO_HANDLE: %s", strerror(errno));
    } else {
        PASS("sync_file → syncobj 导入成功");
        drmSyncobjDestroy(drm_fd, fd_to.handle);
    }

    // 清理
    drmSyncobjDestroy(drm_fd, syncobj);
    close(drm_fd);
}

int main() {
    printf("===========================================================\n");
    printf(" 异步跨栈同步探测：HIP semaphore + DRM syncobj/sync_file\n");
    printf("===========================================================\n");

    test_hip_semaphore();
    test_drm_syncobj();

    printf("\n========== 汇总 ==========\n");
    printf("  PASS: %d   FAIL: %d\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("  >>> 异步同步通路确认可行\n");
    } else {
        printf("  >>> 存在不支持项：HIP semaphore interop 在 Linux 大概率不可用；\n");
        printf("      DRM syncobj/sync_file 是替代通路（需 Mesa 侧支持导出）。\n");
    }
    return g_fail == 0 ? 0 : 1;
}
