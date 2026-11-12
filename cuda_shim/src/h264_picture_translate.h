#ifndef CUDA_SHIM_H264_PICTURE_TRANSLATE_H
#define CUDA_SHIM_H264_PICTURE_TRANSLATE_H

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>
#include <va/va.h>

#include <vector>

namespace cuvid_shim {

struct VaH264Picture {
    VAPictureParameterBufferH264 parameters = {};
    VAIQMatrixBufferH264 inverse_quantizer = {};
    std::vector<VASliceParameterBufferH264> slices;
};

CUresult translate_h264_picture(const CUVIDPICPARAMS &input,
                                const std::vector<VASurfaceID> &surfaces,
                                VaH264Picture *output);

} // namespace cuvid_shim

#endif
