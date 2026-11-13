#include "vaapi_h264_decoder.h"
#include "h264_picture_translate.h"

#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <drm/drm_fourcc.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace cuvid_shim {

namespace {
// FFmpeg 在 unsafe_output/多线程场景会通过 -extra_hw_frames 申请更多解码面。
// 原上限 32 会在 host 申请 33+ 时直接返回 NOT_SUPPORTED；这里允许到 64，
// 实际驱动仍须通过 vaCreateSurfaces/vaCreateContext 验证。
constexpr unsigned int kMaxDecodeSurfaces = 64;
}

VaapiH264Decoder::~VaapiH264Decoder() {
    reset();
}

VASurfaceID VaapiH264Decoder::surface(std::size_t index) const {
    return index < surfaces_.size() ? surfaces_[index] : VA_INVALID_SURFACE;
}

CUresult VaapiH264Decoder::decode(const CUVIDPICPARAMS &picture) {
    if (!display_ || context_ == VA_INVALID_ID) return static_cast<CUresult>(3);
    if (picture.CurrPicIdx < 0 ||
        static_cast<std::size_t>(picture.CurrPicIdx) >= surfaces_.size()) {
        return CUDA_ERROR_INVALID_VALUE;
    }
    if (picture.PicWidthInMbs * 16 != coded_width_ ||
        picture.FrameHeightInMbs * 16 != coded_height_) {
        return static_cast<CUresult>(801);
    }

    VaH264Picture translated;
    CUresult result = translate_h264_picture(picture, surfaces_, &translated);
    if (result != CUDA_SUCCESS) return result;

    // 临时调试：把目标 surface 固定到指定下标，验证"下标越界/驱动限制"假设
    int target = picture.CurrPicIdx;
    if (const char *force = std::getenv("CUVID_SHIM_FORCE_SURFACE")) target = std::atoi(force);
    if (const char *mod = std::getenv("CUVID_SHIM_SURFACE_MOD")) {
        const int m = std::atoi(mod);
        if (m > 0) target = picture.CurrPicIdx % m;
    }
    if (target < 0 || static_cast<std::size_t>(target) >= surfaces_.size()) target = picture.CurrPicIdx;

    VABufferID buffers[4] = {VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID};
    int buffer_count = 0;
    bool picture_started = false;
    VAStatus status = vaCreateBuffer(display_, context_, VAPictureParameterBufferType,
                                     sizeof(translated.parameters), 1,
                                     &translated.parameters, &buffers[buffer_count++]);
    if (status == VA_STATUS_SUCCESS) {
        status = vaCreateBuffer(display_, context_, VAIQMatrixBufferType,
                                sizeof(translated.inverse_quantizer), 1,
                                &translated.inverse_quantizer, &buffers[buffer_count++]);
    }
    if (status == VA_STATUS_SUCCESS) {
        status = vaCreateBuffer(display_, context_, VASliceParameterBufferType,
                                sizeof(translated.slices[0]), translated.slices.size(),
                                translated.slices.data(), &buffers[buffer_count++]);
    }
    if (status == VA_STATUS_SUCCESS) {
        status = vaCreateBuffer(display_, context_, VASliceDataBufferType,
                                picture.nBitstreamDataLen, 1,
                                const_cast<uint8_t *>(picture.pBitstreamData),
                                &buffers[buffer_count++]);
    }

    if (status == VA_STATUS_SUCCESS) {
        status = vaBeginPicture(display_, context_, surfaces_[target]);
        picture_started = status == VA_STATUS_SUCCESS;
    }
    for (int index = 0; index < buffer_count && status == VA_STATUS_SUCCESS; ++index) {
        status = vaRenderPicture(display_, context_, &buffers[index], 1);
    }
    if (picture_started) {
        VAStatus end_status = vaEndPicture(display_, context_);
        if (status == VA_STATUS_SUCCESS) status = end_status;
    }
    for (int index = 0; index < buffer_count; ++index) {
        if (buffers[index] != VA_INVALID_ID) (void)vaDestroyBuffer(display_, buffers[index]);
    }

    if (status == VA_STATUS_SUCCESS && std::getenv("CUVID_SHIM_DEBUG_VASUM")) {
        // 绕过 HIP 导入，直接读 VA 面，判断是"解码没写"还是"导入读错内存"。
        if (vaSyncSurface(display_, surfaces_[target]) == VA_STATUS_SUCCESS) {
            VAImage image;
            if (vaDeriveImage(display_, surfaces_[target], &image) == VA_STATUS_SUCCESS) {
                void *mem = nullptr;
                if (vaMapBuffer(display_, image.buf, &mem) == VA_STATUS_SUCCESS) {
                    const auto *base = static_cast<const uint8_t *>(mem);
                    unsigned long long sum = 0;
                    for (int row = 0; row < coded_height_; ++row) {
                        const auto *p = base + static_cast<std::size_t>(row) * image.pitches[0];
                        for (int x = 0; x < coded_width_; ++x) sum += p[x];
                    }
                    std::fprintf(stderr, "DBG va surface idx=%d -> target=%d %dx%d pitch=%u Ysum=%llu\n",
                                 picture.CurrPicIdx, target, coded_width_, coded_height_,
                                 image.pitches[0], sum);
                    if (const char *path = std::getenv("CUVID_SHIM_DUMP_VASUM")) {
                        static int dump_count = 0;
                        char name[512];
                        std::snprintf(name, sizeof(name), "%s/cnt%03d_idx%d.yuv", path,
                                      dump_count++, target);
                        if (FILE *f = std::fopen(name, "wb")) {
                            for (int row = 0; row < coded_height_; ++row)
                                std::fwrite(base + static_cast<std::size_t>(row) * image.pitches[0],
                                            1, static_cast<std::size_t>(coded_width_), f);
                            std::fclose(f);
                        }
                    }
                    (void)vaUnmapBuffer(display_, image.buf);
                }
                (void)vaDestroyImage(display_, image.image_id);
            }
        }
    }
    return status == VA_STATUS_SUCCESS ? CUDA_SUCCESS : CUDA_ERROR_UNKNOWN;
}

void VaapiH264Decoder::reset() {
    if (display_) {
        if (context_ != VA_INVALID_ID) (void)vaDestroyContext(display_, context_);
        if (!surfaces_.empty()) {
            for (VASurfaceID id : surfaces_) (void)vaSyncSurface(display_, id);
            (void)vaDestroySurfaces(display_, surfaces_.data(), surfaces_.size());
        }
        if (config_ != VA_INVALID_ID) (void)vaDestroyConfig(display_, config_);
        (void)vaTerminate(display_);
    }
    if (drm_fd_ >= 0) close(drm_fd_);
    drm_fd_ = -1;
    display_ = nullptr;
    config_ = VA_INVALID_ID;
    context_ = VA_INVALID_ID;
    coded_width_ = 0;
    coded_height_ = 0;
    surfaces_.clear();
}

CUresult VaapiH264Decoder::initialize(const CUVIDDECODECREATEINFO &info) {
    reset();
    if (info.CodecType != cudaVideoCodec_H264 ||
        info.ChromaFormat != cudaVideoChromaFormat_420 ||
        info.bitDepthMinus8 != 0 ||
        info.OutputFormat != cudaVideoSurfaceFormat_NV12 ||
        info.DeinterlaceMode != cudaVideoDeinterlaceMode_Weave ||
        info.ulWidth == 0 || info.ulHeight == 0 ||
        info.ulWidth > 4096 || info.ulHeight > 4096 ||
        info.ulNumDecodeSurfaces == 0 ||
        info.ulNumDecodeSurfaces > kMaxDecodeSurfaces) {
        return static_cast<CUresult>(801);
    }
    if ((info.ulWidth & 1) || (info.ulHeight & 1)) return CUDA_ERROR_INVALID_VALUE;

    const char *default_node = "/dev/dri/renderD128";
    const char *node = std::getenv("CUVID_SHIM_DRM_NODE");
    if (!node || !*node) node = default_node;
    drm_fd_ = open(node, O_RDWR | O_CLOEXEC);
    if (drm_fd_ < 0) return static_cast<CUresult>(100);

    display_ = vaGetDisplayDRM(drm_fd_);
    if (!display_) {
        reset();
        return static_cast<CUresult>(100);
    }
    int major = 0;
    int minor = 0;
    if (vaInitialize(display_, &major, &minor) != VA_STATUS_SUCCESS) {
        reset();
        return static_cast<CUresult>(100);
    }

    coded_width_ = static_cast<int>(info.ulWidth);
    coded_height_ = static_cast<int>(info.ulHeight);
    surfaces_.assign(info.ulNumDecodeSurfaces, VA_INVALID_SURFACE);
    if (std::getenv("CUVID_SHIM_TRACE_SURF")) {
        std::fprintf(stderr,
                     "SHIM VA INIT %ux%u surfaces=%u target=%ux%u\n",
                     info.ulWidth, info.ulHeight, info.ulNumDecodeSurfaces,
                     info.ulTargetWidth, info.ulTargetHeight);
    }

    uint64_t modifiers[] = {DRM_FORMAT_MOD_LINEAR};
    VADRMFormatModifierList modifier_list = {1, modifiers};
    VASurfaceAttrib attributes[2];
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type = VASurfaceAttribDRMFormatModifiers;
    attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = &modifier_list;

    // 调试：CUVID_SHIM_PLAIN_SURFACES 时建普通 VA 面（不带 DRM_PRIME_2/线性），
    // 用来区分"解码没写"是参数问题还是导出/修饰符问题。
    const bool plain = std::getenv("CUVID_SHIM_PLAIN_SURFACES") != nullptr;
    VAStatus status = vaCreateSurfaces(
        display_, VA_RT_FORMAT_YUV420, coded_width_, coded_height_,
        surfaces_.data(), surfaces_.size(), plain ? nullptr : attributes, plain ? 0 : 2);
    if (status != VA_STATUS_SUCCESS) {
        reset();
        return static_cast<CUresult>(801);
    }

    if (std::getenv("CUVID_SHIM_DEBUG_VASUM")) {
        std::fprintf(stderr, "DBG va surfaces:");
        for (std::size_t i = 0; i < surfaces_.size(); ++i)
            std::fprintf(stderr, " %zu:%u", i, surfaces_[i]);
        std::fprintf(stderr, "\n");
    }

    VAConfigAttrib attribute = {};
    attribute.type = VAConfigAttribRTFormat;
    status = vaGetConfigAttributes(display_, VAProfileH264High, VAEntrypointVLD,
                                   &attribute, 1);
    if (status != VA_STATUS_SUCCESS || attribute.value == VA_ATTRIB_NOT_SUPPORTED ||
        !(attribute.value & VA_RT_FORMAT_YUV420)) {
        reset();
        return static_cast<CUresult>(801);
    }
    status = vaCreateConfig(display_, VAProfileH264High, VAEntrypointVLD,
                            &attribute, 1, &config_);
    if (status != VA_STATUS_SUCCESS) {
        reset();
        return static_cast<CUresult>(801);
    }
    status = vaCreateContext(display_, config_, coded_width_, coded_height_,
                             VA_PROGRESSIVE, surfaces_.data(), surfaces_.size(), &context_);
    if (status != VA_STATUS_SUCCESS) {
        reset();
        return static_cast<CUresult>(801);
    }
    return CUDA_SUCCESS;
}

} // namespace cuvid_shim
