#ifndef CUDA_SHIM_H264_DIRECT_PARAMS_H
#define CUDA_SHIM_H264_DIRECT_PARAMS_H

// Reconstruct a minimal Annex-B SPS/PPS pair from CUVIDH264PICPARAMS.
//
// Why this is needed:
//   FFmpeg's h264_nvdec path parses the bitstream itself and calls only
//   cuvidDecodePicture(). It never calls cuvidParseVideoData(), so the shim's
//   CuvidParser never sees SPS/PPS. The CUVID ABI also carries no raw SPS/PPS
//   bytes, only the parsed H264 picture fields. The Vulkan Video backend still
//   needs a parameter-set pair to create its video session.
//
// This helper writes a legal, minimal SPS/PPS whose fields are available in
// CUVIDH264PICPARAMS. Fields absent from the ABI (profile constraint flags,
// exact level, VUI, cropping, SPS/PPS IDs, POC type 1 cycle) are inferred or
// defaulted. It is therefore a compatibility fallback, not a byte-exact copy
// of the original parameter sets. Unsupported combinations fail explicitly.

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cuvid_shim {

bool synthesize_h264_parameter_sets(const CUVIDPICPARAMS &picture,
                                    std::vector<uint8_t> *sps_annex_b,
                                    std::vector<uint8_t> *pps_annex_b);

} // namespace cuvid_shim

#endif // CUDA_SHIM_H264_DIRECT_PARAMS_H
