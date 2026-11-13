// libnppicc.so.11 — Stage A NPP ABI layer for NV12 <-> YUV420P conversions.
// Device code is in nppicc_color_hip.cpp; this file must not include HIP headers.

#include "npp_common.h"

extern "C" NppStatus nppiYCbCr420_8u_P2P3R(
    const Npp8u *pSrcY, int nSrcYStep,
    const Npp8u *pSrcUV, int nSrcUVStep,
    Npp8u *pDst[3], int nDstStep[3], NppiSize oSizeROI) {
    if (!pSrcY || !pSrcUV || !pDst || !pDst[0] || !pDst[1] || !pDst[2] || !nDstStep)
        return NPP_NULL_POINTER_ERROR;
    if (!npp_shim::valid_size(oSizeROI) ||
        (oSizeROI.width & 1) || (oSizeROI.height & 1)) {
        return NPP_BAD_ARGUMENT_ERROR;
    }
    if (nSrcYStep < oSizeROI.width || nSrcUVStep < oSizeROI.width ||
        nDstStep[0] < oSizeROI.width ||
        nDstStep[1] < oSizeROI.width / 2 ||
        nDstStep[2] < oSizeROI.width / 2) {
        return NPP_BAD_ARGUMENT_ERROR;
    }

    int status = npp_gpu::launch_copy_y(
        pSrcY, nSrcYStep, pDst[0], nDstStep[0],
        oSizeROI.width, oSizeROI.height);
    if (status != npp_gpu::OK) return npp_shim::status_from_gpu(status);
    status = npp_gpu::launch_p2p3_uv(
        pSrcUV, nSrcUVStep,
        pDst[1], nDstStep[1], pDst[2], nDstStep[2],
        oSizeROI.width, oSizeROI.height);
    return npp_shim::status_from_gpu(status);
}

extern "C" NppStatus nppiYCbCr420_8u_P3P2R(
    const Npp8u *const pSrc[3], int nSrcStep[3],
    Npp8u *pDstY, int nDstYStep,
    Npp8u *pDstUV, int nDstUVStep, NppiSize oSizeROI) {
    if (!pSrc || !pSrc[0] || !pSrc[1] || !pSrc[2] || !nSrcStep ||
        !pDstY || !pDstUV) {
        return NPP_NULL_POINTER_ERROR;
    }
    if (!npp_shim::valid_size(oSizeROI) ||
        (oSizeROI.width & 1) || (oSizeROI.height & 1)) {
        return NPP_BAD_ARGUMENT_ERROR;
    }
    if (nSrcStep[0] < oSizeROI.width ||
        nSrcStep[1] < oSizeROI.width / 2 ||
        nSrcStep[2] < oSizeROI.width / 2 ||
        nDstYStep < oSizeROI.width || nDstUVStep < oSizeROI.width) {
        return NPP_BAD_ARGUMENT_ERROR;
    }

    int status = npp_gpu::launch_copy_y(
        pSrc[0], nSrcStep[0], pDstY, nDstYStep,
        oSizeROI.width, oSizeROI.height);
    if (status != npp_gpu::OK) return npp_shim::status_from_gpu(status);
    status = npp_gpu::launch_p3p2(
        pSrc[1], nSrcStep[1], pSrc[2], nSrcStep[2],
        pDstUV, nDstUVStep, oSizeROI.width, oSizeROI.height);
    return npp_shim::status_from_gpu(status);
}
