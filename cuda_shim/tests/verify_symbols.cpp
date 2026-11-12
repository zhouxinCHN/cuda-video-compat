// cuda_shim/tests/verify_symbols.cpp
// 验证 libcuda.so.1 是否导出了 dynlink_loader.h 里 cuda_load_functions()
// 所需的全部 LOAD_SYMBOL（必需）符号。
//
// 用法：./verify_symbols <libcuda.so.1 路径>
// 退出码 0 = 全部必需符号存在；非 0 = 有缺失（打印缺失清单）。

#include <cstdio>
#include <cstring>
#include <dlfcn.h>

// 与 nv-codec-headers/include/ffnvcodec/dynlink_loader.h 的 cuda_load_functions()
// 保持完全一致的 LOAD_SYMBOL 字符串清单（82 个必需符号）。
static const char *REQUIRED_SYMBOLS[] = {
    "cuInit", "cuDriverGetVersion",
    "cuDeviceGetCount", "cuDeviceGet", "cuDeviceGetAttribute", "cuDeviceGetName",
    "cuDeviceComputeCapability",
    "cuCtxCreate_v2", "cuCtxSynchronize", "cuCtxGetStreamPriorityRange",
    "cuCtxGetCurrent", "cuCtxSetLimit", "cuCtxPushCurrent_v2", "cuCtxPopCurrent_v2",
    "cuCtxDestroy_v2",
    "cuMemAlloc_v2", "cuMemAllocPitch_v2", "cuMemAllocManaged",
    "cuMemsetD8Async", "cuMemsetD16Async", "cuMemsetD32Async",
    "cuMemsetD2D8Async", "cuMemsetD2D16Async", "cuMemsetD2D32Async",
    "cuMemFree_v2", "cuMemHostAlloc", "cuMemFreeHost", "cuMemAllocAsync", "cuMemFreeAsync",
    "cuMemcpy", "cuMemcpyAsync", "cuMemcpy2D_v2", "cuMemcpy2DAsync_v2",
    "cuMemcpyHtoD_v2", "cuMemcpyHtoDAsync_v2", "cuMemcpyDtoH_v2", "cuMemcpyDtoHAsync_v2",
    "cuMemcpyDtoD_v2", "cuMemcpyDtoDAsync_v2",
    "cuGetErrorName", "cuGetErrorString", "cuCtxGetDevice",
    "cuDevicePrimaryCtxRetain", "cuDevicePrimaryCtxRelease", "cuDevicePrimaryCtxSetFlags",
    "cuDevicePrimaryCtxGetState", "cuDevicePrimaryCtxReset",
    "cuStreamCreate", "cuStreamCreateWithPriority", "cuStreamQuery", "cuStreamSynchronize",
    "cuStreamDestroy_v2", "cuStreamAddCallback", "cuStreamWaitEvent",
    "cuEventCreate", "cuEventDestroy_v2", "cuEventSynchronize", "cuEventQuery", "cuEventRecord",
    "cuLaunchKernel", "cuLaunchHostFunc",
    "cuLinkCreate", "cuLinkAddData", "cuLinkComplete", "cuLinkDestroy",
    "cuModuleLoadData", "cuModuleUnload", "cuModuleGetFunction", "cuModuleGetGlobal",
    "cuTexObjectCreate", "cuTexObjectDestroy",
    "cuGLGetDevices_v2", "cuGraphicsGLRegisterImage", "cuGraphicsUnregisterResource",
    "cuGraphicsMapResources", "cuGraphicsUnmapResources", "cuGraphicsSubResourceGetMappedArray",
    "cuGraphicsResourceGetMappedPointer_v2",
    "cuArrayCreate_v2", "cuArray3DCreate_v2", "cuArrayDestroy", "cuArray3DGetDescriptor_v2",
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <libcuda.so.1 路径>\n", argv[0]);
        return 2;
    }
    void *lib = dlopen(argv[1], RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "无法加载 %s: %s\n", argv[1], dlerror());
        return 2;
    }

    int total = sizeof(REQUIRED_SYMBOLS) / sizeof(REQUIRED_SYMBOLS[0]);
    int missing = 0;
    printf("检查 %d 个必需符号...\n", total);
    for (int i = 0; i < total; i++) {
        void *sym = dlsym(lib, REQUIRED_SYMBOLS[i]);
        if (!sym) {
            printf("  ✗ 缺失: %s\n", REQUIRED_SYMBOLS[i]);
            missing++;
        }
    }
    if (missing == 0) {
        printf("✓ 全部 %d 个必需符号导出正常\n", total);
    } else {
        printf("✗ %d 个符号缺失（共 %d 个）\n", missing, total);
    }
    dlclose(lib);
    return missing == 0 ? 0 : 1;
}
