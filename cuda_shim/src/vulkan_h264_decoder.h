#ifndef CUDA_SHIM_VULKAN_H264_DECODER_H
#define CUDA_SHIM_VULKAN_H264_DECODER_H

// Vulkan Video H.264 decode backend (Tier-2 alternative to VAAPI).
//
// Mirrors VaapiH264Decoder's role behind the shared DecoderBackend interface:
//   cuvidCreateDecoder -> VulkanH264Decoder::initialize
//   cuvidDecodePicture  -> VulkanH264Decoder::decode
//   cuvidMapVideoFrame64-> VulkanH264Decoder::map_frame (vkCmdCopyImageToBuffer
//                          -> vkGetMemoryFdKHR -> HIP import, same as P0 PoC)
//
// Structural difference from VAAPI: Vulkan needs SPS/PPS up-front to build the
// decode session + session parameters, but the CUVID host only hands this
// decoder per-slice NALs. SPS/PPS arrive via VkSequenceCache (filled by
// CuvidParser). ensure_session() lazily builds the Vulkan object graph the
// first time SPS/PPS are both cached. See docs/architecture/vulkan-backend.md.

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

// FFmpeg POC state type (h264_parse.h). Same extern-C + HAVE_AV_CONFIG_H pattern
// as cuvid_parser.h. Kept in the header so decode() can persist POC state across
// calls (POC type 0 MSB wrap-around needs prev_poc_msb/prev_poc_lsb of the last
// reference picture).
extern "C" {
#define HAVE_AV_CONFIG_H 1
#include "libavcodec/h264_parse.h"
}

namespace cuvid_shim {

class HipFrameMapping;

class VulkanH264Decoder {
public:
    VulkanH264Decoder() = default;
    ~VulkanH264Decoder();

    VulkanH264Decoder(const VulkanH264Decoder &) = delete;
    VulkanH264Decoder &operator=(const VulkanH264Decoder &) = delete;

    CUresult initialize(const CUVIDDECODECREATEINFO &info);
    CUresult decode(const CUVIDPICPARAMS &picture);
    CUresult map_frame(int picture_index, HipFrameMapping *mapping);
    void reset();

    int coded_width() const { return coded_width_; }
    int coded_height() const { return coded_height_; }

private:
    // Build session/params/images/buffers/sync once SPS/PPS are cached. Idempotent.
    // Returns CUDA_SUCCESS on build OR when the cache is still empty (deferred);
    // caller checks session_ != VK_NULL_HANDLE to distinguish ready vs deferred.
    CUresult ensure_session();

    // Core object graph (all destroyed in reset()).
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue decode_queue_ = VK_NULL_HANDLE;
    VkQueue transfer_queue_ = VK_NULL_HANDLE;
    VkVideoSessionKHR session_ = VK_NULL_HANDLE;
    VkVideoSessionParametersKHR session_params_ = VK_NULL_HANDLE;
    VkVideoProfileInfoKHR profile_info_ = {};           // built from SPS, reused for images
    VkVideoDecodeH264ProfileInfoKHR h264_profile_info_ = {};  // chained on profile_info_.pNext

    // Command + sync.
    VkCommandPool decode_pool_ = VK_NULL_HANDLE;
    VkCommandPool transfer_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer decode_cmd_ = VK_NULL_HANDLE;
    VkCommandBuffer transfer_cmd_ = VK_NULL_HANDLE;
    VkSemaphore decode_done_ = VK_NULL_HANDLE;
    VkSemaphore copy_done_ = VK_NULL_HANDLE;
    VkFence copy_fence_ = VK_NULL_HANDLE;

    // 解码结果状态 query（VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR，与 FFmpeg
    // ff_vk_decode_frame 同一套做法）。没有它，vkCmdDecodeVideoKHR 是 void，
    // 失败只能等内核 ring timeout 变成 context lost，日志里连"哪一帧出错"都
    // 判断不了。队列族不支持 queryResultStatus 时保持 VK_NULL_HANDLE（跳过）。
    VkQueryPool decode_query_ = VK_NULL_HANDLE;
    bool query_supported_ = false;

    // Output images (indexed by CUVID CurrPicIdx) + DPB images (indexed by slot).
    struct ImageRes {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    std::vector<ImageRes> outputs_;
    std::vector<ImageRes> dpb_images_;
    std::vector<uint8_t> dpb_layout_init_;
    std::vector<int> free_dpb_slots_;

    // One export buffer per output surface (decode copies NV12 into it, then
    // exports an fd; map_frame() imports that fd into HIP).
    struct ExportRes {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint32_t pitch = 0;
        VkDeviceSize size = 0;
        int fd = -1;   // exported by decode(), consumed by map_frame()
    };
    std::vector<ExportRes> exports_;

    // Annex-B bitstream buffer (single, reused per slice).
    VkBuffer bitstream_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory bitstream_mem_ = VK_NULL_HANDLE;
    VkDeviceSize bitstream_buf_size_ = 0;

    // Memory bound to the video session (VCN session/decoder context buffers).
    // MUST stay allocated until vkDestroyVideoSessionKHR(): radv keeps raw
    // radv_device_memory* pointers to it (radv_video.c:copy_bind) and programs
    // RDECODE_CMD_CONTEXT_BUFFER with its address on every decode. Freeing it
    // early = the firmware's own context buffer gets recycled by a later
    // allocation → VCN ring timeout.
    std::vector<VkDeviceMemory> session_mems_;

    // Codec/session state.
    int coded_width_ = 0;
    int coded_height_ = 0;
    int decode_qf_ = 0;
    int transfer_qf_ = 0;
    uint32_t max_dpb_slots_ = 17;
    uint32_t max_active_refs_ = 0;
    VkDeviceSize min_bitstream_align_ = 128;
    uint32_t pitch_align_ = 256;
    uint8_t sps_id_ = 0;
    uint8_t pps_id_ = 0;
    uint32_t decode_order_ = 0;   // decode-order cursor (output image reuse + first-submit WAR)
    bool session_reset_done_ = false;   // one-time VK_VIDEO_CODING_CONTROL_RESET after session create
    bool device_lost_ = false;          // set after a fence timeout / DEVICE_LOST; fail fast afterwards

    // POC state carried across decode() calls. Reset on IDR.
    H264POCContext poc_ctx_ = {};

    // Active reference picture set (slot + frame_num + POC). The driver orders the
    // actual ref lists internally; this only tracks which pictures are still valid
    // references, mirroring P0's DPB state machine (MMCO + sliding window + IDR).
    struct DpbEntry {
        int slot = -1;
        int frame_num = 0;
        int field_poc[2] = {0, 0};
    };
    std::vector<DpbEntry> dpb_;
};

} // namespace cuvid_shim

#endif // CUDA_SHIM_VULKAN_H264_DECODER_H
