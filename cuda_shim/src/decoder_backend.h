#ifndef CUDA_SHIM_DECODER_BACKEND_H
#define CUDA_SHIM_DECODER_BACKEND_H

// Backend-agnostic H.264 decode interface.
//
// libnvcuvid.so.1 exposes the CUVID surface (cuvidCreateDecoder/DecodePicture/
// MapVideoFrame64/...). The actual decoder is swappable: today there are two
// implementations of this interface —
//   * VaapiH264Decoder   (VAAPI VLD, the primary/Tier-1 backend)
//   * VulkanH264Decoder  (VK_KHR_video_decode_h264, the Tier-2 alternative)
//
// Selection is by env CUVID_SHIM_BACKEND ("vaapi" | "vk"), resolved in
// make_backend(). See docs/architecture/Vulkan后端实现.md for the architecture and the SPS/PPS
// handoff between CuvidParser and VulkanH264Decoder.

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>

#include <memory>

namespace cuvid_shim {

class HipFrameMapping;

class DecoderBackend {
public:
    virtual ~DecoderBackend() = default;

    // Create decoder resources. Must be callable repeatedly (reset first).
    virtual CUresult initialize(const CUVIDDECODECREATEINFO &info) = 0;

    // Decode one picture. picture.CurrPicIdx indexes the output surface.
    virtual CUresult decode(const CUVIDPICPARAMS &picture) = 0;

    // Map a decoded picture (by index) into a HIP device pointer (zero-copy).
    // The caller keeps `mapping` alive for the duration of the map.
    virtual CUresult map_frame(int picture_index, HipFrameMapping *mapping) = 0;

    virtual int coded_width() const = 0;
    virtual int coded_height() const = 0;
    virtual void reset() = 0;
};

// Backend factory. Reads CUVID_SHIM_BACKEND; unknown/missing -> VAAPI.
std::unique_ptr<DecoderBackend> make_backend();

} // namespace cuvid_shim

#endif // CUDA_SHIM_DECODER_BACKEND_H
