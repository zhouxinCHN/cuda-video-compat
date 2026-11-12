#ifndef CUDA_SHIM_VAAPI_H264_DECODER_H
#define CUDA_SHIM_VAAPI_H264_DECODER_H

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>
#include <va/va.h>

#include <cstddef>
#include <vector>

namespace cuvid_shim {

class VaapiH264Decoder {
public:
    VaapiH264Decoder() = default;
    ~VaapiH264Decoder();

    VaapiH264Decoder(const VaapiH264Decoder &) = delete;
    VaapiH264Decoder &operator=(const VaapiH264Decoder &) = delete;

    CUresult initialize(const CUVIDDECODECREATEINFO &info);
    CUresult decode(const CUVIDPICPARAMS &picture);
    void reset();

    VADisplay display() const { return display_; }
    VAContextID context() const { return context_; }
    int coded_width() const { return coded_width_; }
    int coded_height() const { return coded_height_; }
    std::size_t surface_count() const { return surfaces_.size(); }
    VASurfaceID surface(std::size_t index) const;

private:
    int drm_fd_ = -1;
    VADisplay display_ = nullptr;
    VAConfigID config_ = VA_INVALID_ID;
    VAContextID context_ = VA_INVALID_ID;
    int coded_width_ = 0;
    int coded_height_ = 0;
    std::vector<VASurfaceID> surfaces_;
};

} // namespace cuvid_shim

#endif
