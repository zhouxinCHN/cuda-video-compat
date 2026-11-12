#include "hip_frame_mapper.h"

#include <va/va_drmcommon.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unistd.h>

namespace cuvid_shim {
namespace {

CUresult hip_error(hipError_t error) {
    switch (error) {
    case hipSuccess: return CUDA_SUCCESS;
    case hipErrorInvalidValue: return CUDA_ERROR_INVALID_VALUE;
    case hipErrorOutOfMemory: return static_cast<CUresult>(2);
    case hipErrorNotInitialized: return static_cast<CUresult>(3);
    case hipErrorInvalidContext: return static_cast<CUresult>(201);
    case hipErrorNotSupported: return static_cast<CUresult>(801);
    default: return CUDA_ERROR_UNKNOWN;
    }
}

bool range_valid(std::size_t offset, std::size_t pitch,
                 std::size_t rows, std::size_t row_bytes,
                 std::size_t object_size) {
    if (rows == 0 || row_bytes == 0 || row_bytes > pitch) return false;
    if (offset > object_size) return false;
    const std::size_t last_row = rows - 1;
    if (last_row != 0 &&
        pitch > (std::numeric_limits<std::size_t>::max() - offset) / last_row) {
        return false;
    }
    const std::size_t last_row_offset = offset + pitch * last_row;
    return last_row_offset <= object_size && row_bytes <= object_size - last_row_offset;
}

} // namespace

HipFrameMapping::~HipFrameMapping() {
    reset();
}

HipFrameMapping::HipFrameMapping(HipFrameMapping &&other) noexcept {
    *this = std::move(other);
}

HipFrameMapping &HipFrameMapping::operator=(HipFrameMapping &&other) noexcept {
    if (this == &other) return *this;
    reset();
    objects_ = std::move(other.objects_);
    device_pointer_ = other.device_pointer_;
    uv_offset_ = other.uv_offset_;
    pitch_ = other.pitch_;
    contiguous_nv12_ = other.contiguous_nv12_;
    other.device_pointer_ = 0;
    other.uv_offset_ = 0;
    other.pitch_ = 0;
    other.contiguous_nv12_ = false;
    other.objects_.clear();
    return *this;
}

void HipFrameMapping::reset() {
    for (auto &object : objects_) {
        if (object.memory) (void)hipDestroyExternalMemory(object.memory);
        if (object.fd >= 0) close(object.fd);
    }
    objects_.clear();
    device_pointer_ = 0;
    uv_offset_ = 0;
    pitch_ = 0;
    contiguous_nv12_ = false;
}

CUdeviceptr HipFrameMapping::device_pointer() const {
    return device_pointer_;
}

CUresult HipFrameMapping::map_fd(int fd, uint64_t size, uint32_t pitch, int coded_height) {
    reset();
    if (fd < 0 || size == 0 || pitch == 0 || coded_height <= 0) {
        if (fd >= 0) close(fd);
        return CUDA_ERROR_INVALID_VALUE;
    }

    objects_.resize(1);
    auto &object = objects_[0];
    object.fd = fd;
    hipExternalMemoryHandleDesc handle = {};
    handle.type = hipExternalMemoryHandleTypeOpaqueFd;
    handle.handle.fd = fd;
    handle.size = size;
    hipError_t error = hipImportExternalMemory(&object.memory, &handle);
    if (error != hipSuccess) {
        reset();
        return hip_error(error);
    }
    hipExternalMemoryBufferDesc buffer = {};
    buffer.size = size;
    error = hipExternalMemoryGetMappedBuffer(&object.pointer, object.memory, &buffer);
    if (error != hipSuccess) {
        reset();
        return hip_error(error);
    }

    const std::size_t height = static_cast<std::size_t>(coded_height);
    if (!range_valid(0, pitch, height + height / 2, 1, static_cast<std::size_t>(size))) {
        reset();
        return CUDA_ERROR_INVALID_VALUE;
    }

    pitch_ = pitch;
    device_pointer_ = static_cast<CUdeviceptr>(reinterpret_cast<uintptr_t>(object.pointer));
    uv_offset_ = static_cast<std::size_t>(pitch) * height;
    contiguous_nv12_ = true;
    return CUDA_SUCCESS;
}

CUresult HipFrameMapping::map(VADisplay display, VASurfaceID surface,
                              int coded_width, int coded_height) {
    reset();
    if (!display || coded_width <= 0 || coded_height <= 0 ||
        (coded_width & 1) || (coded_height & 1)) {
        return CUDA_ERROR_INVALID_VALUE;
    }

    VAStatus status = vaSyncSurface(display, surface);
    if (status != VA_STATUS_SUCCESS) return CUDA_ERROR_UNKNOWN;

    VADRMPRIMESurfaceDescriptor descriptor;
    std::memset(&descriptor, 0, sizeof(descriptor));
    status = vaExportSurfaceHandle(
        display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &descriptor);
    if (status != VA_STATUS_SUCCESS) {
        status = vaExportSurfaceHandle(
            display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
            &descriptor);
    }
    if (status != VA_STATUS_SUCCESS) return static_cast<CUresult>(801);
    if (descriptor.fourcc != VA_FOURCC_NV12 || descriptor.num_objects == 0 ||
        descriptor.num_layers == 0) {
        for (uint32_t i = 0; i < descriptor.num_objects; ++i) close(descriptor.objects[i].fd);
        return static_cast<CUresult>(801);
    }

    objects_.resize(descriptor.num_objects);
    CUresult result = CUDA_SUCCESS;
    for (uint32_t i = 0; i < descriptor.num_objects; ++i) {
        auto &output = objects_[i];
        output.fd = descriptor.objects[i].fd;
        hipExternalMemoryHandleDesc handle = {};
        handle.type = hipExternalMemoryHandleTypeOpaqueFd;
        handle.handle.fd = output.fd;
        handle.size = descriptor.objects[i].size;
        hipError_t error = hipImportExternalMemory(&output.memory, &handle);
        if (error != hipSuccess) {
            result = hip_error(error);
            break;
        }
        hipExternalMemoryBufferDesc buffer = {};
        buffer.size = descriptor.objects[i].size;
        error = hipExternalMemoryGetMappedBuffer(&output.pointer, output.memory, &buffer);
        if (error != hipSuccess) {
            result = hip_error(error);
            break;
        }
    }
    if (result != CUDA_SUCCESS) {
        reset();
        return result;
    }

    uint32_t y_layer = 0;
    uint32_t y_plane = 0;
    uint32_t uv_layer = 0;
    uint32_t uv_plane = 0;
    if (descriptor.num_layers >= 2) {
        uv_layer = 1;
    } else {
        if (descriptor.layers[0].num_planes < 2) {
            reset();
            return static_cast<CUresult>(801);
        }
        uv_plane = 1;
    }

    const auto &y = descriptor.layers[y_layer];
    const auto &uv = descriptor.layers[uv_layer];
    const uint32_t y_object = y.object_index[y_plane];
    const uint32_t uv_object = uv.object_index[uv_plane];
    if (y_object >= descriptor.num_objects || uv_object >= descriptor.num_objects ||
        y.pitch[y_plane] != uv.pitch[uv_plane]) {
        reset();
        return static_cast<CUresult>(801);
    }

    const std::size_t width = static_cast<std::size_t>(coded_width);
    const std::size_t height = static_cast<std::size_t>(coded_height);
    if (!range_valid(y.offset[y_plane], y.pitch[y_plane], height, width,
                     descriptor.objects[y_object].size) ||
        !range_valid(uv.offset[uv_plane], uv.pitch[uv_plane], height / 2, width,
                     descriptor.objects[uv_object].size)) {
        reset();
        return CUDA_ERROR_INVALID_VALUE;
    }

    auto *y_base = static_cast<std::uint8_t *>(objects_[y_object].pointer) + y.offset[y_plane];
    auto *uv_base = static_cast<std::uint8_t *>(objects_[uv_object].pointer) + uv.offset[uv_plane];
    pitch_ = y.pitch[y_plane];
    device_pointer_ = static_cast<CUdeviceptr>(reinterpret_cast<uintptr_t>(y_base));
    contiguous_nv12_ = y_object == uv_object && uv_base >= y_base;
    if (contiguous_nv12_) {
        uv_offset_ = static_cast<std::size_t>(uv_base - y_base);
        contiguous_nv12_ = uv_offset_ == static_cast<std::size_t>(pitch_) * height;
    }
    if (!contiguous_nv12_) uv_offset_ = 0;
    return CUDA_SUCCESS;
}

} // namespace cuvid_shim
