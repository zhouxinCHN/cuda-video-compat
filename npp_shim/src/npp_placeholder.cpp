// Placeholder shared object used for NPP sub-libraries that FFmpeg links but
// does not call in the P0 scale_npp path. The build assigns the required
// SONAME; no NPP symbols are claimed here.

extern "C" const char *npp_shim_placeholder_abi(void) {
    return "cuda-video-compat-npp-placeholder";
}
