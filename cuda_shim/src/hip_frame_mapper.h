#ifndef CUDA_SHIM_HIP_FRAME_MAPPER_H
#define CUDA_SHIM_HIP_FRAME_MAPPER_H

#include <ffnvcodec/dynlink_cuda.h>
#include <hip/hip_runtime_api.h>
#include <va/va.h>

#include <vector>

namespace cuvid_shim {

class HipFrameMapping {
public:
    HipFrameMapping() = default;
    ~HipFrameMapping();

    HipFrameMapping(const HipFrameMapping &) = delete;
    HipFrameMapping &operator=(const HipFrameMapping &) = delete;
    HipFrameMapping(HipFrameMapping &&other) noexcept;
    HipFrameMapping &operator=(HipFrameMapping &&other) noexcept;

    CUresult map(VADisplay display, VASurfaceID surface,
                 int coded_width, int coded_height);
    // Import an already-exported dma-buf fd directly (Vulkan backend path:
    // vkGetMemoryFdKHR already produced the fd; no VAAPI descriptor involved).
    // `size` is the total NV12 byte size; Y/UV planes must be contiguous with
    // UV at offset pitch*height (the layout P0's vkCmdCopyImageToBuffer emits).
    CUresult map_fd(int fd, uint64_t size, uint32_t pitch, int coded_height);
    void reset();

    CUdeviceptr device_pointer() const;
    unsigned int pitch() const { return pitch_; }
    std::size_t uv_offset() const { return uv_offset_; }
    bool has_contiguous_nv12_layout() const { return contiguous_nv12_; }

private:
    struct ImportedObject {
        hipExternalMemory_t memory = nullptr;
        void *pointer = nullptr;
        int fd = -1;
    };

    std::vector<ImportedObject> objects_;
    CUdeviceptr device_pointer_ = 0;
    std::size_t uv_offset_ = 0;
    unsigned int pitch_ = 0;
    bool contiguous_nv12_ = false;
};

} // namespace cuvid_shim

#endif
