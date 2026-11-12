#ifndef CUDA_SHIM_BACKEND_DISPATCH_H
#define CUDA_SHIM_BACKEND_DISPATCH_H

// Backend factory: resolves CUVID_SHIM_BACKEND to a DecoderBackend instance.
//   "vk"     -> VulkanH264Decoder (Tier-2)
//   "vaapi"  -> VaapiH264Decoder (Tier-1, default)
//   missing/unknown -> VaapiH264Decoder
//
// Both backends share the DecoderBackend interface, so libnvcuvid.cpp is
// backend-agnostic: it only calls initialize/decode/map_frame/reset/coded_*.
// VaapiH264Decoder's surface()/display() are VAAPI-specific and therefore not
// part of the interface (see decoder_backend.h for the reasoning).

#include "decoder_backend.h"

namespace cuvid_shim {

std::unique_ptr<DecoderBackend> make_backend();

} // namespace cuvid_shim

#endif // CUDA_SHIM_BACKEND_DISPATCH_H
