#include "backend_dispatch.h"

#include "hip_frame_mapper.h"
#include "vaapi_h264_decoder.h"
#include "vulkan_h264_decoder.h"

#include <cstdlib>
#include <cstring>

namespace cuvid_shim {

// Thin DecoderBackend adapter over VaapiH264Decoder. The VAAPI backend keeps its
// VA-specific surface/display accessors out of the shared interface; map_frame
// needs display + surface + coded size, which it can reach through the concrete
// type here.
class VaapiBackend final : public DecoderBackend {
public:
    CUresult initialize(const CUVIDDECODECREATEINFO &info) override {
        return impl_.initialize(info);
    }
    CUresult decode(const CUVIDPICPARAMS &picture) override {
        return impl_.decode(picture);
    }
    CUresult map_frame(int picture_index, HipFrameMapping *mapping) override {
        VASurfaceID surface = impl_.surface(static_cast<std::size_t>(picture_index));
        if (surface == VA_INVALID_SURFACE) return CUDA_ERROR_INVALID_VALUE;
        return mapping->map(impl_.display(), surface, impl_.coded_width(), impl_.coded_height());
    }
    int coded_width() const override { return impl_.coded_width(); }
    int coded_height() const override { return impl_.coded_height(); }
    void reset() override { impl_.reset(); }

private:
    VaapiH264Decoder impl_;
};

class VulkanBackend final : public DecoderBackend {
public:
    CUresult initialize(const CUVIDDECODECREATEINFO &info) override {
        return impl_.initialize(info);
    }
    CUresult decode(const CUVIDPICPARAMS &picture) override {
        return impl_.decode(picture);
    }
    CUresult map_frame(int picture_index, HipFrameMapping *mapping) override {
        return impl_.map_frame(picture_index, mapping);
    }
    int coded_width() const override { return impl_.coded_width(); }
    int coded_height() const override { return impl_.coded_height(); }
    void reset() override { impl_.reset(); }

private:
    VulkanH264Decoder impl_;
};

std::unique_ptr<DecoderBackend> make_backend() {
    const char *backend = std::getenv("CUVID_SHIM_BACKEND");
    if (backend && std::strcmp(backend, "vk") == 0)
        return std::make_unique<VulkanBackend>();
    return std::make_unique<VaapiBackend>();
}

} // namespace cuvid_shim
