#ifndef CUDA_SHIM_H264_DECODE_CONTEXT_H
#define CUDA_SHIM_H264_DECODE_CONTEXT_H

// Synchronous CUVID callback-scoped DPB context. CUVIDPICPARAMS has no fields
// for RefPicList0/1, so CuvidParser exposes the shared model only while the host
// is inside pfnDecodePicture. The pointer never escapes the callback and is
// restored for nested scopes. No global decoder or cross-thread ownership.

#include "h264_dpb.h"

namespace cuvid_shim {

struct H264DecodeContext {
    const H264Dpb *dpb = nullptr;
    const H264Slice *slice = nullptr;
    const H264Picture *picture = nullptr;
    const void *owner = nullptr; // CuvidParser instance used for surface leases.
};

class ScopedH264DecodeContext {
public:
    explicit ScopedH264DecodeContext(const H264DecodeContext &context)
        : previous_(current_) {
        current_ = &context;
    }

    ~ScopedH264DecodeContext() { current_ = previous_; }

    ScopedH264DecodeContext(const ScopedH264DecodeContext &) = delete;
    ScopedH264DecodeContext &operator=(const ScopedH264DecodeContext &) = delete;

    static const H264DecodeContext *current() { return current_; }

private:
    static inline thread_local const H264DecodeContext *current_ = nullptr;
    const H264DecodeContext *previous_;
};

} // namespace cuvid_shim

#endif // CUDA_SHIM_H264_DECODE_CONTEXT_H
