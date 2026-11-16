#ifndef CUDA_SHIM_VK_SEQUENCE_CACHE_H
#define CUDA_SHIM_VK_SEQUENCE_CACHE_H

// Per-thread stash of the most recently parsed H.264 SPS/PPS NAL bytes.
//
// Why this exists: the CUVID host (FFmpeg h264_cuvid / h264_nvdec) feeds the
// decoder only *per-slice* NALs through cuvidDecodePicture. SPS/PPS are parsed
// by the shim's own CuvidParser (cuvid_parser.cpp), which has no direct handle
// to the decoder (the two are coupled through the host's pfnSequenceCallback).
//
// This cache is the seam between them: CuvidParser::store()s the SPS/PPS NAL
// bytes when it sees them; VulkanH264Decoder::fetch()es them to lazily build its
// Vulkan session + session parameters.
//
// The slot is thread-local, not process-global: FFmpeg runs one decoder in its
// own thread, so concurrent streams no longer overwrite each other's parameter
// sets. A single thread that interleaves several decoders still has one slot
// and must serialize per-stream decode; that limitation is documented in
// docs/architecture/Vulkan后端实现.md.

#include <cstdint>
#include <cstddef>
#include <vector>

namespace cuvid_shim {

class VkSequenceCache {
public:
    static void store(const uint8_t *sps, size_t sps_n,
                      const uint8_t *pps, size_t pps_n) {
        sps_.clear();
        pps_.clear();
        if (sps && sps_n) {
            static const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
            sps_.insert(sps_.end(), kStartCode, kStartCode + 4);
            sps_.insert(sps_.end(), sps, sps + sps_n);
        }
        if (pps && pps_n) {
            static const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
            pps_.insert(pps_.end(), kStartCode, kStartCode + 4);
            pps_.insert(pps_.end(), pps, pps + pps_n);
        }
    }

    static bool fetch(std::vector<uint8_t> *sps, std::vector<uint8_t> *pps) {
        if (sps_.empty() || pps_.empty()) return false;
        *sps = sps_;
        *pps = pps_;
        return true;
    }

private:
    static inline thread_local std::vector<uint8_t> sps_;
    static inline thread_local std::vector<uint8_t> pps_;
};

} // namespace cuvid_shim

#endif // CUDA_SHIM_VK_SEQUENCE_CACHE_H
