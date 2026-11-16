// libnppc core API probe.
//
// The same source builds against NVIDIA NPP and the AMD shim. Unlike the Stage
// A image probe it is not byte-compared across hosts: device properties are
// machine-specific. The probe instead checks the semantics every NPP client
// relies on: version reporting, non-zero device limits that agree between the
// individual queries and nppGetStreamContext(), and a consistent NPP stream
// state.
//
// Usage:
//   ./nppc_core_probe [--expect-lib-version 11.5.1]
//
// Exit code 0 = all checks passed.

#include <nppcore.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

} // namespace

int main(int argc, char **argv) {
    std::string expect_version;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--expect-lib-version") == 0 && i + 1 < argc) {
            expect_version = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    const NppLibraryVersion *lib = nppGetLibVersion();
    check(lib != nullptr, "nppGetLibVersion returns a pointer");
    if (!lib) return 1;

    char version[64];
    std::snprintf(version, sizeof(version), "%d.%d.%d",
                  lib->major, lib->minor, lib->build);
    std::printf("INFO: nppGetLibVersion = %s\n", version);
    if (!expect_version.empty()) {
        check(expect_version == version, "library version matches expectation");
    }

    const int sms = nppGetGpuNumSMs();
    const int max_threads_block = nppGetMaxThreadsPerBlock();
    const int max_threads_sm = nppGetMaxThreadsPerSM();
    std::printf("INFO: SMs=%d maxThreadsPerBlock=%d maxThreadsPerSM=%d\n",
                sms, max_threads_block, max_threads_sm);
    check(sms > 0, "nppGetGpuNumSMs > 0");
    check(max_threads_block > 0, "nppGetMaxThreadsPerBlock > 0");
    check(max_threads_sm > 0, "nppGetMaxThreadsPerSM > 0");

    int prop_threads_sm = -1;
    int prop_threads_block = -1;
    int prop_sms = -1;
    check(nppGetGpuDeviceProperties(&prop_threads_sm, &prop_threads_block,
                                    &prop_sms) == 0,
          "nppGetGpuDeviceProperties succeeds");
    check(prop_threads_sm == max_threads_sm,
          "nppGetGpuDeviceProperties threads/SM agrees");
    check(prop_threads_block == max_threads_block,
          "nppGetGpuDeviceProperties threads/block agrees");
    check(prop_sms == sms, "nppGetGpuDeviceProperties SM count agrees");

    const char *gpu_name = nppGetGpuName();
    std::printf("INFO: nppGetGpuName = %s\n", gpu_name ? gpu_name : "(null)");
    check(gpu_name != nullptr && gpu_name[0] != '\0', "nppGetGpuName is non-empty");

    check(nppGetStream() == nullptr, "default NPP stream is the NULL stream");
    check(nppSetStream(nullptr) == NPP_SUCCESS, "nppSetStream(NULL) succeeds");
    check(nppGetStream() == nullptr, "nppGetStream reflects NULL stream");

    NppStreamContext ctx{};
    check(nppGetStreamContext(&ctx) == NPP_SUCCESS,
          "nppGetStreamContext succeeds");
    check(ctx.hStream == nppGetStream(), "context stream equals nppGetStream");
    check(ctx.nCudaDeviceId >= 0, "context device id is valid");
    check(ctx.nMultiProcessorCount == sms, "context SM count agrees");
    check(ctx.nMaxThreadsPerMultiProcessor == max_threads_sm,
          "context threads/SM agrees");
    check(ctx.nMaxThreadsPerBlock == max_threads_block,
          "context threads/block agrees");
    check(ctx.nSharedMemPerBlock > 0, "context shared memory is non-zero");
    std::printf("INFO: device=%d cc=%d.%d smem/block=%zu\n",
                ctx.nCudaDeviceId,
                ctx.nCudaDevAttrComputeCapabilityMajor,
                ctx.nCudaDevAttrComputeCapabilityMinor,
                ctx.nSharedMemPerBlock);
    check(ctx.nCudaDevAttrComputeCapabilityMajor >= 0 &&
              ctx.nCudaDevAttrComputeCapabilityMinor >= 0,
          "compute capability is reported");

    check(nppGetStreamNumSMs() == static_cast<unsigned int>(sms),
          "nppGetStreamNumSMs agrees");
    check(nppGetStreamMaxThreadsPerSM() == static_cast<unsigned int>(max_threads_sm),
          "nppGetStreamMaxThreadsPerSM agrees");

    std::printf("RESULT: %s (%d failure%s)\n",
                g_failures == 0 ? "PASS" : "FAIL",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
