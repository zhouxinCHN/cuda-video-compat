// libnppc.so.11 — NPP core API for Stage A.
//
// FFmpeg's configure probes nppGetLibVersion from the NPP link set. The target
// compatibility ABI is NPP 11.5.1.107; the version is virtualized accordingly
// even when the build host has newer CUDA headers installed.
//
// Device and stream queries are forwarded to the HIP runtime through the plain
// bridge in npp_gpu_bridge.h. The NPP stream handle is only recorded: the
// Stage A kernels synchronize the device around every operation, so ordering
// with respect to any user stream is preserved even though the kernels run on
// the default stream.

#include <nppcore.h>

#include "npp_gpu_bridge.h"

#include <atomic>
#include <cstring>

namespace {

std::atomic<void *> g_npp_stream{nullptr};

bool query_active(npp_gpu::DeviceInfo *info) {
    return npp_gpu::query_device(-1, info) == npp_gpu::OK;
}

} // namespace

extern "C" const NppLibraryVersion *nppGetLibVersion(void) {
    static const NppLibraryVersion version = {11, 5, 107};
    return &version;
}

extern "C" int nppGetGpuNumSMs(void) {
    npp_gpu::DeviceInfo info{};
    return query_active(&info) ? info.multi_processor_count : 0;
}

extern "C" int nppGetMaxThreadsPerBlock(void) {
    npp_gpu::DeviceInfo info{};
    return query_active(&info) ? info.max_threads_per_block : 0;
}

extern "C" int nppGetMaxThreadsPerSM(void) {
    npp_gpu::DeviceInfo info{};
    return query_active(&info) ? info.max_threads_per_multi_processor : 0;
}

extern "C" int nppGetGpuDeviceProperties(int *pMaxThreadsPerSM,
                                         int *pMaxThreadsPerBlock,
                                         int *pNumberOfSMs) {
    if (!pMaxThreadsPerSM || !pMaxThreadsPerBlock || !pNumberOfSMs) return -1;

    npp_gpu::DeviceInfo info{};
    if (!query_active(&info)) return -1;

    *pMaxThreadsPerSM = info.max_threads_per_multi_processor;
    *pMaxThreadsPerBlock = info.max_threads_per_block;
    *pNumberOfSMs = info.multi_processor_count;
    return 0;
}

extern "C" const char *nppGetGpuName(void) {
    static thread_local char name[sizeof(npp_gpu::DeviceInfo::name)];
    npp_gpu::DeviceInfo info{};
    if (!query_active(&info)) return "";

    std::memcpy(name, info.name, sizeof(name));
    name[sizeof(name) - 1] = '\0';
    return name;
}

extern "C" cudaStream_t nppGetStream(void) {
    return static_cast<cudaStream_t>(g_npp_stream.load(std::memory_order_relaxed));
}

extern "C" NppStatus nppSetStream(cudaStream_t hStream) {
    void *requested = static_cast<void *>(hStream);
    if (g_npp_stream.load(std::memory_order_relaxed) == requested) {
        return NPP_SUCCESS;
    }
    // Match NPP's documented flush-on-switch behaviour. Stage A synchronizes
    // the whole device rather than one stream, which is a superset.
    if (npp_gpu::device_synchronize() != npp_gpu::OK) {
        return NPP_CUDA_KERNEL_EXECUTION_ERROR;
    }
    g_npp_stream.store(requested, std::memory_order_relaxed);
    return NPP_SUCCESS;
}

extern "C" NppStatus nppGetStreamContext(NppStreamContext *pNppStreamContext) {
    if (!pNppStreamContext) return NPP_NULL_POINTER_ERROR;

    npp_gpu::DeviceInfo info{};
    if (!query_active(&info)) return NPP_CUDA_KERNEL_EXECUTION_ERROR;

    pNppStreamContext->hStream = nppGetStream();
    pNppStreamContext->nCudaDeviceId = info.device_id;
    pNppStreamContext->nMultiProcessorCount = info.multi_processor_count;
    pNppStreamContext->nMaxThreadsPerMultiProcessor =
        info.max_threads_per_multi_processor;
    pNppStreamContext->nMaxThreadsPerBlock = info.max_threads_per_block;
    pNppStreamContext->nSharedMemPerBlock =
        static_cast<size_t>(info.shared_mem_per_block);
    pNppStreamContext->nCudaDevAttrComputeCapabilityMajor =
        info.compute_capability_major;
    pNppStreamContext->nCudaDevAttrComputeCapabilityMinor =
        info.compute_capability_minor;
    pNppStreamContext->nStreamFlags = 0;
    pNppStreamContext->nReserved0 = 0;
    return NPP_SUCCESS;
}

extern "C" unsigned int nppGetStreamNumSMs(void) {
    npp_gpu::DeviceInfo info{};
    return query_active(&info) ? static_cast<unsigned int>(info.multi_processor_count) : 0;
}

extern "C" unsigned int nppGetStreamMaxThreadsPerSM(void) {
    npp_gpu::DeviceInfo info{};
    return query_active(&info)
               ? static_cast<unsigned int>(info.max_threads_per_multi_processor)
               : 0;
}
