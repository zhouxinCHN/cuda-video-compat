// Verify the FFmpeg nv-codec-headers CUVID loading surface.
// Passing this test proves symbol availability only, not decoder functionality.

#include <dlfcn.h>

#include <cstdio>

static const char *REQUIRED_SYMBOLS[] = {
    "cuvidCreateDecoder",
    "cuvidDestroyDecoder",
    "cuvidDecodePicture",
    "cuvidGetDecodeStatus",
    "cuvidReconfigureDecoder",
    "cuvidMapVideoFrame64",
    "cuvidUnmapVideoFrame64",
    "cuvidCtxLockCreate",
    "cuvidCtxLockDestroy",
    "cuvidCtxLock",
    "cuvidCtxUnlock",
    "cuvidCreateVideoSource",
    "cuvidCreateVideoSourceW",
    "cuvidDestroyVideoSource",
    "cuvidSetVideoSourceState",
    "cuvidGetVideoSourceState",
    "cuvidGetSourceVideoFormat",
    "cuvidGetSourceAudioFormat",
    "cuvidCreateVideoParser",
    "cuvidParseVideoData",
    "cuvidDestroyVideoParser",
};

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <libnvcuvid.so.1>\n", argv[0]);
        return 2;
    }

    void *library = dlopen(argv[1], RTLD_LAZY);
    if (!library) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 2;
    }

    int missing = 0;
    for (const char *name : REQUIRED_SYMBOLS) {
        if (!dlsym(library, name)) {
            std::fprintf(stderr, "MISSING: %s\n", name);
            ++missing;
        }
    }
    if (!dlsym(library, "cuvidGetDecoderCaps")) {
        std::fprintf(stderr, "MISSING optional capability API: cuvidGetDecoderCaps\n");
        ++missing;
    }

    std::printf("CUVID required symbols: %zu, optional capability symbols: 1, missing: %d\n",
                sizeof(REQUIRED_SYMBOLS) / sizeof(REQUIRED_SYMBOLS[0]), missing);
    dlclose(library);
    return missing == 0 ? 0 : 1;
}
