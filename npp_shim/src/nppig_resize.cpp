// libnppig.so.11 — Stage A NPP ABI layer for
// nppiResizeSqrPixel_8u_C1R. Device code is in nppig_resize_hip.cpp; this
// file includes only the authorized NPP/CUDA headers and must not include HIP
// headers (their CUDA vector types conflict).

#include "npp_common.h"

extern "C" NppStatus nppiResizeSqrPixel_8u_C1R(
    const Npp8u *pSrc, NppiSize oSrcSize, int nSrcStep, NppiRect oSrcROI,
    Npp8u *pDst, int nDstStep, NppiRect oDstROI,
    double nXFactor, double nYFactor, double nXShift, double nYShift,
    int eInterpolation) {
    using namespace npp_shim;

    if (!pSrc || !pDst) return NPP_NULL_POINTER_ERROR;
    if (!valid_size(oSrcSize) || !valid_rect(oSrcROI) || !valid_rect(oDstROI) ||
        !positive_finite(nXFactor) || !positive_finite(nYFactor) ||
        !std::isfinite(nXShift) || !std::isfinite(nYShift)) {
        return NPP_BAD_ARGUMENT_ERROR;
    }
    if (nSrcStep < oSrcSize.width ||
        nDstStep < oDstROI.x + oDstROI.width) {
        return NPP_BAD_ARGUMENT_ERROR;
    }
    if (oSrcROI.x + oSrcROI.width > oSrcSize.width ||
        oSrcROI.y + oSrcROI.height > oSrcSize.height) {
        return NPP_WRONG_INTERSECTION_ROI_ERROR;
    }

    npp_gpu::ResizeJob job{
        pSrc, nSrcStep,
        oSrcROI.x, oSrcROI.y, oSrcROI.width, oSrcROI.height,
        pDst, nDstStep,
        oDstROI.x, oDstROI.y, oDstROI.width, oDstROI.height,
        nXFactor, nYFactor, nXShift, nYShift, eInterpolation
    };
    return status_from_gpu(npp_gpu::launch_resize(job));
}
