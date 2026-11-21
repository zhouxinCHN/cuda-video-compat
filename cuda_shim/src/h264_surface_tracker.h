#ifndef CUDA_SHIM_H264_SURFACE_TRACKER_H
#define CUDA_SHIM_H264_SURFACE_TRACKER_H

// Host-visible surface leases. A display callback makes a surface outstanding;
// only cuvidUnmapVideoFrame64 (or decoder destruction) releases it. This closes
// the display->map window where the DPB alone would otherwise allow reuse.

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cuvid_shim {

class H264SurfaceTracker {
public:
    static void mark(const void *owner, int picture_index, bool outstanding) {
        if (!owner || picture_index < 0) return;
        std::lock_guard<std::mutex> guard(mutex_);
        auto &entries = entries_[owner];
        const std::size_t index = static_cast<std::size_t>(picture_index);
        if (entries.size() <= index) entries.resize(index + 1, false);
        entries[index] = outstanding;
    }

    static std::vector<bool> snapshot(const void *owner, std::size_t count) {
        std::lock_guard<std::mutex> guard(mutex_);
        std::vector<bool> result(count, false);
        if (!owner) return result;
        const auto found = entries_.find(owner);
        if (found == entries_.end()) return result;
        const std::size_t copied = std::min(count, found->second.size());
        std::copy_n(found->second.begin(), copied, result.begin());
        return result;
    }

    static void clear(const void *owner) {
        if (!owner) return;
        std::lock_guard<std::mutex> guard(mutex_);
        entries_.erase(owner);
    }

private:
    static inline std::mutex mutex_;
    static inline std::unordered_map<const void *, std::vector<bool>> entries_;
};

} // namespace cuvid_shim

#endif // CUDA_SHIM_H264_SURFACE_TRACKER_H
