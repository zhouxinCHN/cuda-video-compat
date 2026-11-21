// Algorithm reference: rocm-systems/projects/rocdecode AvcVideoParser,
// commit 9a279cd640c9f52eebdff273b76d494fdd8e0d28.
// rocDecode is MIT licensed; see licenses/rocDecode-MIT.txt.

#include "h264_dpb.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace cuvid_shim {
namespace {

bool fits_int32(int64_t value) {
    return value >= std::numeric_limits<int32_t>::min() &&
           value <= std::numeric_limits<int32_t>::max();
}

H264DpbStatus validate_sequence(const H264Sequence &sequence) {
    if (sequence.log2_max_frame_num < 4 || sequence.log2_max_frame_num > 16 ||
        sequence.log2_max_poc_lsb < 4 || sequence.log2_max_poc_lsb > 16 ||
        sequence.poc_type > 2 || sequence.offset_for_ref_frame.size() > 255 ||
        sequence.max_num_ref_frames > kH264MaxReferenceFrames ||
        sequence.max_dpb_frames == 0 ||
        sequence.max_dpb_frames > kH264MaxReferenceFrames ||
        sequence.max_num_ref_frames > sequence.max_dpb_frames) {
        return H264DpbStatus::kInvalidArgument;
    }
    return H264DpbStatus::kOk;
}

bool poc_less(const H264Picture &left, const H264Picture &right) {
    if (left.poc != right.poc) return left.poc < right.poc;
    return left.picture_index < right.picture_index;
}

bool poc_greater(const H264Picture &left, const H264Picture &right) {
    if (left.poc != right.poc) return left.poc > right.poc;
    return left.picture_index < right.picture_index;
}

} // namespace

const char *h264_dpb_status_name(H264DpbStatus status) {
    switch (status) {
    case H264DpbStatus::kOk: return "ok";
    case H264DpbStatus::kInvalidArgument: return "invalid_argument";
    case H264DpbStatus::kUnsupported: return "unsupported";
    case H264DpbStatus::kReferenceNotFound: return "reference_not_found";
    case H264DpbStatus::kDpbOverflow: return "dpb_overflow";
    case H264DpbStatus::kNoFreeSurface: return "no_free_surface";
    }
    return "unknown";
}

H264DpbStatus H264Dpb::reset(const H264Sequence &sequence) {
    const H264DpbStatus valid = validate_sequence(sequence);
    if (valid != H264DpbStatus::kOk) return valid;
    sequence_ = sequence;
    pictures_.clear();
    pending_outputs_.clear();
    prev_poc_msb_ = 0;
    prev_poc_lsb_ = 0;
    prev_top_field_order_cnt_ = 0;
    prev_frame_num_ = 0;
    prev_frame_num_offset_ = 0;
    max_long_term_frame_idx_ = -1;
    prev_has_mmco5_ = false;
    picture_active_ = false;
    sequence_id_ = 0;
    active_poc_msb_ = 0;
    active_frame_num_offset_ = 0;
    return H264DpbStatus::kOk;
}

bool H264Dpb::contains_picture_index(int picture_index) const {
    const auto matches = [picture_index](const auto &entry) {
        return entry.picture_index == picture_index;
    };
    return std::any_of(pictures_.begin(), pictures_.end(), matches) ||
           std::any_of(pending_outputs_.begin(), pending_outputs_.end(), matches);
}

H264DpbStatus H264Dpb::choose_surface(unsigned int surface_count,
                                      const std::vector<bool> &mapped,
                                      int *picture_index) const {
    if (!picture_index || surface_count == 0 || mapped.size() < surface_count) {
        return H264DpbStatus::kInvalidArgument;
    }
    *picture_index = -1;
    for (unsigned int index = 0; index < surface_count; ++index) {
        if (!mapped[index] && !contains_picture_index(static_cast<int>(index))) {
            *picture_index = static_cast<int>(index);
            return H264DpbStatus::kOk;
        }
    }
    return H264DpbStatus::kNoFreeSurface;
}

H264DpbStatus H264Dpb::begin_picture(const H264Slice &slice, int picture_index,
                                     int64_t timestamp, H264Picture *picture) {
    if (!picture || picture_index < 0 || picture_active_ ||
        contains_picture_index(picture_index)) {
        return H264DpbStatus::kInvalidArgument;
    }
    if (slice.frame_num >= (1U << sequence_.log2_max_frame_num) ||
        (slice.is_idr && slice.type != H264SliceType::kI) ||
        (!slice.is_idr && slice.long_term_reference_flag) ||
        (!slice.is_reference &&
         (slice.long_term_reference_flag || slice.adaptive_ref_pic_marking ||
          !slice.mmco.empty())) ||
        (!slice.adaptive_ref_pic_marking && !slice.mmco.empty())) {
        return H264DpbStatus::kInvalidArgument;
    }

    if (slice.is_idr) flush_sequence(slice.no_output_of_prior_pics);

    *picture = H264Picture{};
    picture->picture_index = picture_index;
    picture->timestamp = timestamp;
    picture->sequence_id = sequence_id_;
    picture->frame_num = static_cast<int>(slice.frame_num);
    picture->needs_output = slice.output_flag;

    const H264DpbStatus status = calculate_poc(slice, picture, &active_poc_msb_,
                                               &active_frame_num_offset_);
    if (status != H264DpbStatus::kOk) return status;
    picture_active_ = true;
    return H264DpbStatus::kOk;
}

H264DpbStatus H264Dpb::calculate_poc(const H264Slice &slice, H264Picture *picture,
                                     int *poc_msb, int *frame_num_offset) const {
    const int max_frame_num = 1 << sequence_.log2_max_frame_num;
    int64_t frame_offset = prev_frame_num_offset_;
    int previous_frame_num = prev_frame_num_;
    if (prev_has_mmco5_) {
        frame_offset = 0;
        previous_frame_num = 0;
    }
    if (slice.is_idr) {
        frame_offset = 0;
    } else if (previous_frame_num > static_cast<int>(slice.frame_num)) {
        frame_offset += max_frame_num;
    }
    if (!fits_int32(frame_offset)) return H264DpbStatus::kUnsupported;

    int64_t top = 0;
    int64_t bottom = 0;
    *poc_msb = 0;
    *frame_num_offset = static_cast<int>(frame_offset);

    if (sequence_.poc_type == 0) {
        const int max_poc_lsb = 1 << sequence_.log2_max_poc_lsb;
        if (slice.poc_lsb >= static_cast<uint32_t>(max_poc_lsb)) {
            return H264DpbStatus::kInvalidArgument;
        }
        int previous_msb = prev_poc_msb_;
        int previous_lsb = prev_poc_lsb_;
        if (prev_has_mmco5_) {
            previous_msb = 0;
            previous_lsb = prev_top_field_order_cnt_;
        }
        if (static_cast<int>(slice.poc_lsb) < previous_lsb &&
            previous_lsb - static_cast<int>(slice.poc_lsb) >= max_poc_lsb / 2) {
            *poc_msb = previous_msb + max_poc_lsb;
        } else if (static_cast<int>(slice.poc_lsb) > previous_lsb &&
                   static_cast<int>(slice.poc_lsb) - previous_lsb > max_poc_lsb / 2) {
            *poc_msb = previous_msb - max_poc_lsb;
        } else {
            *poc_msb = previous_msb;
        }
        top = static_cast<int64_t>(*poc_msb) + slice.poc_lsb;
        bottom = top + slice.delta_poc_bottom;
    } else if (sequence_.poc_type == 1) {
        const std::size_t cycle = sequence_.offset_for_ref_frame.size();
        int64_t abs_frame_num = cycle == 0 ? 0 : frame_offset + slice.frame_num;
        if (!slice.is_reference && abs_frame_num > 0) --abs_frame_num;

        int64_t expected_delta = 0;
        for (int32_t offset : sequence_.offset_for_ref_frame) expected_delta += offset;
        int64_t expected_poc = 0;
        if (abs_frame_num > 0) {
            const int64_t cycle_count = (abs_frame_num - 1) / static_cast<int64_t>(cycle);
            const int64_t in_cycle = (abs_frame_num - 1) % static_cast<int64_t>(cycle);
            expected_poc = cycle_count * expected_delta;
            for (int64_t index = 0; index <= in_cycle; ++index) {
                expected_poc += sequence_.offset_for_ref_frame[static_cast<std::size_t>(index)];
            }
        }
        if (!slice.is_reference) expected_poc += sequence_.offset_for_non_ref_pic;
        top = expected_poc + slice.delta_poc[0];
        bottom = top + sequence_.offset_for_top_to_bottom_field + slice.delta_poc[1];
    } else if (sequence_.poc_type == 2) {
        top = 2 * (frame_offset + slice.frame_num) - (slice.is_reference ? 0 : 1);
        bottom = top;
    } else {
        return H264DpbStatus::kUnsupported;
    }

    if (!fits_int32(top) || !fits_int32(bottom)) return H264DpbStatus::kUnsupported;
    picture->top_field_order_cnt = static_cast<int>(top);
    picture->bottom_field_order_cnt = static_cast<int>(bottom);
    picture->poc = static_cast<int>(std::min(top, bottom));
    return H264DpbStatus::kOk;
}

int32_t H264Dpb::short_pic_num(const H264Picture &picture,
                               int current_frame_num) const {
    const int max_frame_num = 1 << sequence_.log2_max_frame_num;
    return picture.frame_num > current_frame_num ? picture.frame_num - max_frame_num
                                                 : picture.frame_num;
}

std::size_t H264Dpb::reference_count() const {
    return static_cast<std::size_t>(std::count_if(
        pictures_.begin(), pictures_.end(), [](const H264Picture &picture) {
            return picture.reference != H264ReferenceType::kUnused;
        }));
}

H264DpbStatus H264Dpb::make_initial_list(
    const H264Slice &slice, const H264Picture &current, unsigned int list_index,
    std::vector<H264Reference> *references) const {
    references->clear();
    if (slice.type == H264SliceType::kI) return H264DpbStatus::kOk;

    std::vector<H264Picture> short_less;
    std::vector<H264Picture> short_greater;
    std::vector<H264Picture> long_term;
    for (const H264Picture &picture : pictures_) {
        if (picture.reference == H264ReferenceType::kShortTerm) {
            if (slice.type == H264SliceType::kB && picture.poc > current.poc) {
                short_greater.push_back(picture);
            } else {
                short_less.push_back(picture);
            }
        } else if (picture.reference == H264ReferenceType::kLongTerm) {
            long_term.push_back(picture);
        }
    }

    const auto short_num = [this, &current](const H264Picture &picture) {
        return short_pic_num(picture, current.frame_num);
    };
    const auto pic_num_desc = [&short_num](const H264Picture &left,
                                           const H264Picture &right) {
        const int left_num = short_num(left);
        const int right_num = short_num(right);
        if (left_num != right_num) return left_num > right_num;
        return left.picture_index < right.picture_index;
    };
    const auto long_num_asc = [](const H264Picture &left, const H264Picture &right) {
        if (left.long_term_frame_idx != right.long_term_frame_idx) {
            return left.long_term_frame_idx < right.long_term_frame_idx;
        }
        return left.picture_index < right.picture_index;
    };

    if (slice.type == H264SliceType::kP) {
        std::sort(short_less.begin(), short_less.end(), pic_num_desc);
    } else if (list_index == 0) {
        std::sort(short_less.begin(), short_less.end(), poc_greater);
        std::sort(short_greater.begin(), short_greater.end(), poc_less);
    } else {
        std::sort(short_greater.begin(), short_greater.end(), poc_less);
        std::sort(short_less.begin(), short_less.end(), poc_greater);
        short_less.swap(short_greater);
    }
    std::sort(long_term.begin(), long_term.end(), long_num_asc);

    const auto append = [this, &current, references](const H264Picture &picture) {
        H264Reference reference;
        reference.picture_index = picture.picture_index;
        reference.poc = picture.poc;
        reference.is_long_term = picture.reference == H264ReferenceType::kLongTerm;
        reference.frame_idx = reference.is_long_term
                                  ? static_cast<int>(picture.long_term_frame_idx)
                                  : picture.frame_num;
        reference.pic_num = reference.is_long_term
                                ? static_cast<int>(picture.long_term_frame_idx)
                                : short_pic_num(picture, current.frame_num);
        references->push_back(reference);
    };
    for (const H264Picture &picture : short_less) append(picture);
    if (slice.type == H264SliceType::kB) {
        for (const H264Picture &picture : short_greater) append(picture);
    }
    for (const H264Picture &picture : long_term) append(picture);
    return H264DpbStatus::kOk;
}

H264DpbStatus H264Dpb::modify_list(
    const H264Picture &current,
    const std::vector<H264RefListModification> &modifications,
    std::size_t active_count, std::vector<H264Reference> *references) const {
    if (active_count > kH264MaxReferenceListSize || modifications.size() > active_count) {
        return H264DpbStatus::kInvalidArgument;
    }
    const std::vector<H264Reference> initial = *references;
    if (initial.size() < active_count) return H264DpbStatus::kReferenceNotFound;

    std::vector<H264Reference> modified;
    std::vector<bool> selected(initial.size(), false);
    modified.reserve(active_count);
    const int max_frame_num = 1 << sequence_.log2_max_frame_num;
    int64_t pic_num_pred = current.frame_num;

    for (const H264RefListModification &modification : modifications) {
        if (modification.idc == 3) break;
        if (modification.idc > 2 || modified.size() >= active_count) {
            return H264DpbStatus::kInvalidArgument;
        }
        std::size_t found = initial.size();
        if (modification.idc < 2) {
            const int64_t difference = modification.abs_diff_pic_num_minus1 + 1LL;
            if (difference > max_frame_num) return H264DpbStatus::kInvalidArgument;
            int64_t no_wrap;
            if (modification.idc == 0) {
                no_wrap = pic_num_pred < difference ? pic_num_pred - difference + max_frame_num
                                                    : pic_num_pred - difference;
            } else {
                no_wrap = pic_num_pred + difference >= max_frame_num
                              ? pic_num_pred + difference - max_frame_num
                              : pic_num_pred + difference;
            }
            pic_num_pred = no_wrap;
            const int pic_num = static_cast<int>(
                no_wrap > current.frame_num ? no_wrap - max_frame_num : no_wrap);
            found = static_cast<std::size_t>(std::find_if(
                initial.begin(), initial.end(), [&pic_num](const H264Reference &reference) {
                    return !reference.is_long_term && reference.pic_num == pic_num;
                }) - initial.begin());
        } else {
            found = static_cast<std::size_t>(std::find_if(
                initial.begin(), initial.end(), [&modification](const H264Reference &reference) {
                    return reference.is_long_term &&
                           reference.pic_num == static_cast<int>(modification.long_term_pic_num);
                }) - initial.begin());
        }
        if (found == initial.size()) return H264DpbStatus::kReferenceNotFound;
        modified.push_back(initial[found]);
        selected[found] = true;
    }

    for (std::size_t index = 0; index < initial.size() && modified.size() < active_count;
         ++index) {
        if (!selected[index]) modified.push_back(initial[index]);
    }
    if (modified.size() != active_count) return H264DpbStatus::kReferenceNotFound;
    *references = std::move(modified);
    return H264DpbStatus::kOk;
}

H264DpbStatus H264Dpb::reference_list(
    const H264Slice &slice, const H264Picture &current, unsigned int list_index,
    std::vector<H264Reference> *references) const {
    if (!references || list_index > 1 || !picture_active_ ||
        current.picture_index < 0 ||
        slice.num_ref_idx_active[list_index] > kH264MaxReferenceListSize ||
        (slice.type != H264SliceType::kB && list_index == 1 &&
         slice.num_ref_idx_active[1] != 0)) {
        return H264DpbStatus::kInvalidArgument;
    }
    references->clear();
    H264DpbStatus status = make_initial_list(slice, current, list_index, references);
    if (status != H264DpbStatus::kOk) return status;
    return modify_list(current, slice.modifications[list_index],
                       slice.num_ref_idx_active[list_index], references);
}

H264DpbStatus H264Dpb::assign_long_term(H264Picture *picture,
                                        uint32_t long_term_frame_idx) {
    if (long_term_frame_idx >= kH264MaxReferenceFrames || max_long_term_frame_idx_ < 0 ||
        static_cast<int>(long_term_frame_idx) > max_long_term_frame_idx_) {
        return H264DpbStatus::kInvalidArgument;
    }
    for (H264Picture &entry : pictures_) {
        if (entry.reference == H264ReferenceType::kLongTerm &&
            entry.long_term_frame_idx == long_term_frame_idx) {
            entry.reference = H264ReferenceType::kUnused;
        }
    }
    picture->reference = H264ReferenceType::kLongTerm;
    picture->long_term_frame_idx = long_term_frame_idx;
    return H264DpbStatus::kOk;
}

H264DpbStatus H264Dpb::sliding_window(const H264Picture &current) {
    if (sequence_.max_num_ref_frames == 0) return H264DpbStatus::kInvalidArgument;
    if (reference_count() < sequence_.max_num_ref_frames) return H264DpbStatus::kOk;

    auto oldest = pictures_.end();
    int32_t oldest_pic_num = 0;
    for (auto iterator = pictures_.begin(); iterator != pictures_.end(); ++iterator) {
        if (iterator->reference != H264ReferenceType::kShortTerm) continue;
        const int32_t pic_num = short_pic_num(*iterator, current.frame_num);
        if (oldest == pictures_.end() || pic_num < oldest_pic_num) {
            oldest = iterator;
            oldest_pic_num = pic_num;
        }
    }
    if (oldest == pictures_.end()) return H264DpbStatus::kDpbOverflow;
    oldest->reference = H264ReferenceType::kUnused;
    return H264DpbStatus::kOk;
}

H264DpbStatus H264Dpb::execute_mmco(const H264Slice &slice, H264Picture *current,
                                    bool *has_mmco5) {
    *has_mmco5 = false;
    const int max_frame_num = 1 << sequence_.log2_max_frame_num;
    const auto short_target = [&slice, max_frame_num](const H264Mmco &mmco) {
        return (static_cast<int>(slice.frame_num) -
                static_cast<int>(mmco.difference_of_pic_nums_minus1) - 1) &
               (max_frame_num - 1);
    };
    const auto find_short = [this](int frame_num) {
        return std::find_if(pictures_.begin(), pictures_.end(),
                            [frame_num](const H264Picture &picture) {
                                return picture.reference == H264ReferenceType::kShortTerm &&
                                       picture.frame_num == frame_num;
                            });
    };

    for (const H264Mmco &mmco : slice.mmco) {
        switch (mmco.opcode) {
        case 1: {
            if (mmco.difference_of_pic_nums_minus1 >= static_cast<uint32_t>(max_frame_num)) {
                return H264DpbStatus::kInvalidArgument;
            }
            auto target = find_short(short_target(mmco));
            if (target == pictures_.end()) return H264DpbStatus::kReferenceNotFound;
            target->reference = H264ReferenceType::kUnused;
            break;
        }
        case 2: {
            if (mmco.long_term_pic_num >= kH264MaxReferenceFrames) {
                return H264DpbStatus::kInvalidArgument;
            }
            auto target = std::find_if(
                pictures_.begin(), pictures_.end(), [&mmco](const H264Picture &picture) {
                    return picture.reference == H264ReferenceType::kLongTerm &&
                           picture.long_term_frame_idx == mmco.long_term_pic_num;
                });
            if (target == pictures_.end()) return H264DpbStatus::kReferenceNotFound;
            target->reference = H264ReferenceType::kUnused;
            break;
        }
        case 3: {
            if (mmco.difference_of_pic_nums_minus1 >= static_cast<uint32_t>(max_frame_num)) {
                return H264DpbStatus::kInvalidArgument;
            }
            auto target = find_short(short_target(mmco));
            if (target == pictures_.end()) return H264DpbStatus::kReferenceNotFound;
            const H264DpbStatus status = assign_long_term(&*target, mmco.long_term_frame_idx);
            if (status != H264DpbStatus::kOk) return status;
            break;
        }
        case 4: {
            if (mmco.max_long_term_frame_idx_plus1 > kH264MaxReferenceFrames) {
                return H264DpbStatus::kInvalidArgument;
            }
            max_long_term_frame_idx_ =
                mmco.max_long_term_frame_idx_plus1 == 0
                    ? -1
                    : static_cast<int>(mmco.max_long_term_frame_idx_plus1) - 1;
            for (H264Picture &picture : pictures_) {
                if (picture.reference == H264ReferenceType::kLongTerm &&
                    static_cast<int>(picture.long_term_frame_idx) > max_long_term_frame_idx_) {
                    picture.reference = H264ReferenceType::kUnused;
                }
            }
            break;
        }
        case 5: {
            for (H264Picture &picture : pictures_) {
                picture.reference = H264ReferenceType::kUnused;
            }
            flush_sequence(false);
            max_long_term_frame_idx_ = -1;
            current->frame_num = 0;
            current->sequence_id = sequence_id_;
            current->top_field_order_cnt -= current->poc;
            current->bottom_field_order_cnt -= current->poc;
            current->poc = std::min(current->top_field_order_cnt,
                                    current->bottom_field_order_cnt);
            *has_mmco5 = true;
            picture_active_ = true; // flush_sequence clears the active flag.
            break;
        }
        case 6: {
            const H264DpbStatus status =
                assign_long_term(current, mmco.long_term_frame_idx);
            if (status != H264DpbStatus::kOk) return status;
            break;
        }
        default:
            return H264DpbStatus::kInvalidArgument;
        }
    }
    return H264DpbStatus::kOk;
}

H264DpbStatus H264Dpb::validate_reference_count(const H264Picture &current) const {
    const std::size_t count = reference_count() +
                              (current.reference == H264ReferenceType::kUnused ? 0 : 1);
    return count > sequence_.max_num_ref_frames ? H264DpbStatus::kDpbOverflow
                                                : H264DpbStatus::kOk;
}

void H264Dpb::queue_output(const H264Picture &picture) {
    pending_outputs_.push_back(H264Output{picture.picture_index, picture.timestamp,
                                          picture.sequence_id, picture.poc});
    std::stable_sort(pending_outputs_.begin(), pending_outputs_.end(),
                     [](const H264Output &left, const H264Output &right) {
                         if (left.sequence_id != right.sequence_id) {
                             return left.sequence_id < right.sequence_id;
                         }
                         return left.poc < right.poc;
                     });
}

void H264Dpb::erase_unreferenced() {
    pictures_.erase(std::remove_if(pictures_.begin(), pictures_.end(),
                                   [](const H264Picture &picture) {
                                       return picture.reference == H264ReferenceType::kUnused &&
                                              !picture.needs_output;
                                   }),
                    pictures_.end());
}

H264DpbStatus H264Dpb::queue_smallest_output() {
    auto smallest = pictures_.end();
    for (auto iterator = pictures_.begin(); iterator != pictures_.end(); ++iterator) {
        if (iterator->needs_output &&
            (smallest == pictures_.end() || iterator->poc < smallest->poc)) {
            smallest = iterator;
        }
    }
    if (smallest == pictures_.end()) return H264DpbStatus::kReferenceNotFound;
    queue_output(*smallest);
    smallest->needs_output = false;
    erase_unreferenced();
    return H264DpbStatus::kOk;
}

H264DpbStatus H264Dpb::bump_one() {
    auto target = pictures_.end();
    for (auto iterator = pictures_.begin(); iterator != pictures_.end(); ++iterator) {
        if (iterator->reference == H264ReferenceType::kUnused &&
            (target == pictures_.end() || iterator->poc < target->poc)) {
            target = iterator;
        }
    }
    if (target == pictures_.end()) return H264DpbStatus::kDpbOverflow;
    const int target_poc = target->poc;

    while (true) {
        auto smallest = pictures_.end();
        for (auto iterator = pictures_.begin(); iterator != pictures_.end(); ++iterator) {
            if (iterator->needs_output && iterator->poc <= target_poc &&
                (smallest == pictures_.end() || iterator->poc < smallest->poc)) {
                smallest = iterator;
            }
        }
        if (smallest == pictures_.end()) break;
        queue_output(*smallest);
        smallest->needs_output = false;
    }
    const std::size_t old_size = pictures_.size();
    erase_unreferenced();
    return pictures_.size() < old_size ? H264DpbStatus::kOk : H264DpbStatus::kDpbOverflow;
}

void H264Dpb::flush_sequence(bool discard_outputs) {
    if (discard_outputs) {
        pending_outputs_.clear();
    } else {
        while (queue_smallest_output() == H264DpbStatus::kOk) {
        }
    }
    pictures_.clear();
    prev_poc_msb_ = 0;
    prev_poc_lsb_ = 0;
    prev_top_field_order_cnt_ = 0;
    prev_frame_num_ = 0;
    prev_frame_num_offset_ = 0;
    max_long_term_frame_idx_ = -1;
    prev_has_mmco5_ = false;
    picture_active_ = false;
    ++sequence_id_;
}

H264DpbStatus H264Dpb::finish_picture(const H264Slice &slice, H264Picture *current) {
    if (!current || !picture_active_ || current->picture_index < 0) {
        return H264DpbStatus::kInvalidArgument;
    }
    H264Dpb rollback = *this;
    H264Picture original_current = *current;
    bool has_mmco5 = false;
    H264DpbStatus status = H264DpbStatus::kOk;

    if (slice.is_idr) {
        current->reference = H264ReferenceType::kUnused;
        max_long_term_frame_idx_ = -1;
        if (slice.is_reference && sequence_.max_num_ref_frames > 0) {
            if (slice.long_term_reference_flag) {
                max_long_term_frame_idx_ = 0;
                status = assign_long_term(current, 0);
            } else {
                current->reference = H264ReferenceType::kShortTerm;
            }
        }
    } else if (slice.is_reference) {
        if (sequence_.max_num_ref_frames == 0) {
            status = H264DpbStatus::kInvalidArgument;
        } else {
            current->reference = H264ReferenceType::kShortTerm;
            status = slice.adaptive_ref_pic_marking
                         ? execute_mmco(slice, current, &has_mmco5)
                         : sliding_window(*current);
        }
    } else {
        current->reference = H264ReferenceType::kUnused;
    }

    if (status == H264DpbStatus::kOk) status = validate_reference_count(*current);
    erase_unreferenced();
    while (status == H264DpbStatus::kOk && pictures_.size() >= sequence_.max_dpb_frames) {
        status = bump_one();
    }
    if (status == H264DpbStatus::kOk) pictures_.push_back(*current);

    if (status != H264DpbStatus::kOk) {
        *this = std::move(rollback);
        *current = std::move(original_current);
        return status;
    }

    if (slice.is_reference) {
        if (sequence_.poc_type == 0) {
            prev_poc_msb_ = active_poc_msb_;
            prev_poc_lsb_ = static_cast<int>(slice.poc_lsb);
            prev_top_field_order_cnt_ = current->top_field_order_cnt;
        }
        prev_has_mmco5_ = has_mmco5;
    } else {
        prev_has_mmco5_ = false;
    }
    prev_frame_num_ = current->frame_num;
    prev_frame_num_offset_ = active_frame_num_offset_;
    picture_active_ = false;
    return H264DpbStatus::kOk;
}

H264DpbStatus H264Dpb::drain() {
    if (picture_active_) return H264DpbStatus::kInvalidArgument;
    flush_sequence(false);
    return H264DpbStatus::kOk;
}

std::vector<H264Output> H264Dpb::take_outputs(std::size_t max_display_delay,
                                               bool flush) {
    std::size_t count = 0;
    if (flush) {
        count = pending_outputs_.size();
    } else if (pending_outputs_.size() > max_display_delay) {
        count = pending_outputs_.size() - max_display_delay;
    }
    std::vector<H264Output> outputs;
    outputs.reserve(count);
    std::move(pending_outputs_.begin(), pending_outputs_.begin() + count,
              std::back_inserter(outputs));
    pending_outputs_.erase(pending_outputs_.begin(), pending_outputs_.begin() + count);
    return outputs;
}

} // namespace cuvid_shim
