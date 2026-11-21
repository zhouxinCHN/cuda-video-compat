#ifndef CUDA_SHIM_H264_DPB_H
#define CUDA_SHIM_H264_DPB_H

// Backend-independent progressive H.264 DPB state machine.
//
// Algorithm reference: rocDecode AvcVideoParser from rocm-systems, commit
// 9a279cd640c9f52eebdff273b76d494fdd8e0d28 (MIT; see licenses/rocDecode-MIT.txt).
// This component does not parse bitstreams and has no VAAPI/Vulkan/HIP dependency.
// The caller performs hardware decode between reference_list() and finish_picture().
//
// Scope: progressive frame pictures, POC types 0/1/2, MMCO 1-6, sliding window,
// short/long-term references, reference-list modification and POC-ordered output.
// Field/MBAFF, frame_num gaps and non-existing concealment pictures are out of scope.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cuvid_shim {

constexpr std::size_t kH264MaxReferenceFrames = 16;
constexpr std::size_t kH264MaxReferenceListSize = 32;

enum class H264DpbStatus {
    kOk,
    kInvalidArgument,
    kUnsupported,
    kReferenceNotFound,
    kDpbOverflow,
    kNoFreeSurface,
};

const char *h264_dpb_status_name(H264DpbStatus status);

enum class H264SliceType { kI, kP, kB };
enum class H264ReferenceType { kUnused, kShortTerm, kLongTerm };

struct H264Sequence {
    // Actual bit widths, not the corresponding *_minus4 syntax elements.
    uint32_t log2_max_frame_num = 4;
    uint32_t log2_max_poc_lsb = 4;
    uint32_t poc_type = 0;

    // POC type 1. Vector length is num_ref_frames_in_pic_order_cnt_cycle.
    std::vector<int32_t> offset_for_ref_frame;
    int32_t offset_for_non_ref_pic = 0;
    int32_t offset_for_top_to_bottom_field = 0;

    uint32_t max_num_ref_frames = 1;
    // Level-derived DPB capacity, including pictures waiting for output.
    uint32_t max_dpb_frames = 1;
};

struct H264Mmco {
    uint32_t opcode = 0; // Opcode 0 is the terminator and is not stored here.
    uint32_t difference_of_pic_nums_minus1 = 0;
    uint32_t long_term_pic_num = 0;
    uint32_t long_term_frame_idx = 0;
    uint32_t max_long_term_frame_idx_plus1 = 0;
};

struct H264RefListModification {
    uint32_t idc = 3; // 0/1: short-term, 2: long-term, 3: terminator.
    uint32_t abs_diff_pic_num_minus1 = 0;
    uint32_t long_term_pic_num = 0;
};

struct H264Slice {
    H264SliceType type = H264SliceType::kI;
    bool is_idr = false;
    bool is_reference = false;
    uint32_t frame_num = 0;

    uint32_t poc_lsb = 0;
    int32_t delta_poc_bottom = 0;
    int32_t delta_poc[2] = {0, 0};

    bool output_flag = true;
    bool no_output_of_prior_pics = false;
    bool long_term_reference_flag = false; // IDR only.
    bool adaptive_ref_pic_marking = false;
    std::vector<H264Mmco> mmco; // Excludes the opcode-0 terminator.

    uint32_t num_ref_idx_active[2] = {0, 0};
    std::vector<H264RefListModification> modifications[2];
};

struct H264Picture {
    int picture_index = -1;
    int64_t timestamp = 0;
    uint64_t sequence_id = 0; // Increments at IDR and MMCO 5 POC resets.
    int frame_num = 0;
    int poc = 0;
    int top_field_order_cnt = 0;
    int bottom_field_order_cnt = 0;
    H264ReferenceType reference = H264ReferenceType::kUnused;
    uint32_t long_term_frame_idx = 0;
    bool needs_output = false;
};

struct H264Reference {
    int picture_index = -1;
    int frame_idx = 0; // frame_num for short-term; long_term_frame_idx for long-term.
    int poc = 0;
    int pic_num = 0; // PicNum for short-term; LongTermPicNum for long-term.
    bool is_long_term = false;
};

struct H264Output {
    int picture_index = -1;
    int64_t timestamp = 0;
    uint64_t sequence_id = 0;
    int poc = 0;
};

class H264Dpb {
public:
    H264DpbStatus reset(const H264Sequence &sequence);

    // Selects a surface not held by the DPB, pending output, or the caller's
    // mapped set. Unlike a round-robin fallback, exhaustion is always an error.
    H264DpbStatus choose_surface(unsigned int surface_count,
                                 const std::vector<bool> &mapped,
                                 int *picture_index) const;

    // Calculates POC. An IDR first drains the preceding sequence into pending
    // output, or discards prior output for no_output_of_prior_pics.
    H264DpbStatus begin_picture(const H264Slice &slice, int picture_index,
                                int64_t timestamp, H264Picture *picture);

    // Builds and modifies RefPicList0/1 from pre-current-picture references.
    // The result size is exactly num_ref_idx_active[list_index].
    H264DpbStatus reference_list(const H264Slice &slice,
                                 const H264Picture &current,
                                 unsigned int list_index,
                                 std::vector<H264Reference> *references) const;

    // Executes reference marking, inserts the current picture and bumps the DPB.
    // `current` is updated for IDR long-term marking and MMCO 5/6. On failure
    // the object is rolled back to its state before this call.
    H264DpbStatus finish_picture(const H264Slice &slice, H264Picture *current);

    // Queues every remaining output in POC order and releases all DPB entries.
    H264DpbStatus drain();

    // Returns pending.size - max_display_delay entries, or all when flush=true.
    // Once returned, the caller owns output lifetime through map/unmap tracking.
    std::vector<H264Output> take_outputs(std::size_t max_display_delay,
                                         bool flush = false);

    const H264Sequence &sequence() const { return sequence_; }
    const std::vector<H264Picture> &pictures() const { return pictures_; }
    std::size_t pending_output_count() const { return pending_outputs_.size(); }
    bool contains_picture_index(int picture_index) const;

private:
    H264DpbStatus calculate_poc(const H264Slice &slice, H264Picture *picture,
                                int *poc_msb, int *frame_num_offset) const;
    H264DpbStatus execute_mmco(const H264Slice &slice, H264Picture *current,
                               bool *has_mmco5);
    H264DpbStatus sliding_window(const H264Picture &current);
    H264DpbStatus assign_long_term(H264Picture *picture,
                                   uint32_t long_term_frame_idx);
    H264DpbStatus validate_reference_count(const H264Picture &current) const;
    H264DpbStatus make_initial_list(const H264Slice &slice,
                                    const H264Picture &current,
                                    unsigned int list_index,
                                    std::vector<H264Reference> *references) const;
    H264DpbStatus modify_list(const H264Picture &current,
                              const std::vector<H264RefListModification> &modifications,
                              std::size_t active_count,
                              std::vector<H264Reference> *references) const;
    H264DpbStatus bump_one();
    H264DpbStatus queue_smallest_output();
    void flush_sequence(bool discard_outputs);
    void queue_output(const H264Picture &picture);
    void erase_unreferenced();
    int32_t short_pic_num(const H264Picture &picture,
                          int current_frame_num) const;
    std::size_t reference_count() const;

    H264Sequence sequence_ = {};
    std::vector<H264Picture> pictures_;
    std::vector<H264Output> pending_outputs_;

    int prev_poc_msb_ = 0;
    int prev_poc_lsb_ = 0;
    int prev_top_field_order_cnt_ = 0;
    int prev_frame_num_ = 0;
    int prev_frame_num_offset_ = 0;
    int max_long_term_frame_idx_ = -1;
    bool prev_has_mmco5_ = false;

    bool picture_active_ = false;
    uint64_t sequence_id_ = 0;
    int active_poc_msb_ = 0;
    int active_frame_num_offset_ = 0;
};

} // namespace cuvid_shim

#endif // CUDA_SHIM_H264_DPB_H
