// libnppc.so.11 — minimal NPP core symbols for Stage A.
//
// FFmpeg's configure probes nppGetLibVersion from the NPP link set. The target
// compatibility ABI is NPP 11.5.1.107; the version is virtualized accordingly
// even when the build host has newer CUDA headers installed.

#include <nppcore.h>

extern "C" const NppLibraryVersion *nppGetLibVersion(void) {
    static const NppLibraryVersion version = {11, 5, 107};
    return &version;
}
