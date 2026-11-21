// Independent unit tests for the backend-neutral progressive H.264 DPB.
// No GPU, FFmpeg, ROCm or rocDecode runtime is required.

#include "h264_dpb.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using cuvid_shim::H264Dpb;
using cuvid_shim::H264DpbStatus;
using cuvid_shim::H264Mmco;
using cuvid_shim::H264Output;
using cuvid_shim::H264Picture;
using cuvid_shim::H264Reference;
using cuvid_shim::H264ReferenceType;
using cuvid_shim::H264RefListModification;
using cuvid_shim::H264Sequence;
using cuvid_shim::H264Slice;
using cuvid_shim::H264SliceType;

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void require_status(H264DpbStatus actual, H264DpbStatus expected,
                    const std::string &where) {
    if (actual != expected) {
        throw std::runtime_error(where + ": expected " +
                                 cuvid_shim::h264_dpb_status_name(expected) + ", got " +
                                 cuvid_shim::h264_dpb_status_name(actual));
    }
}

H264Sequence sequence(uint32_t poc_type = 0, uint32_t max_references = 4,
                      uint32_t max_dpb = 4) {
    H264Sequence result;
    result.poc_type = poc_type;
    result.max_num_ref_frames = max_references;
    result.max_dpb_frames = max_dpb;
    return result;
}

H264Slice slice(H264SliceType type, uint32_t frame_num, uint32_t poc_lsb,
                bool reference = true) {
    H264Slice result;
    result.type = type;
    result.frame_num = frame_num;
    result.poc_lsb = poc_lsb;
    result.is_reference = reference;
    return result;
}

H264Slice idr(bool long_term = false) {
    H264Slice result = slice(H264SliceType::kI, 0, 0, true);
    result.is_idr = true;
    result.long_term_reference_flag = long_term;
    return result;
}

H264Mmco mmco(uint32_t opcode) {
    H264Mmco result;
    result.opcode = opcode;
    return result;
}

H264Mmco short_to_long(uint32_t difference_minus1, uint32_t long_index) {
    H264Mmco result = mmco(3);
    result.difference_of_pic_nums_minus1 = difference_minus1;
    result.long_term_frame_idx = long_index;
    return result;
}

H264Mmco set_max_long(uint32_t plus1) {
    H264Mmco result = mmco(4);
    result.max_long_term_frame_idx_plus1 = plus1;
    return result;
}

H264Mmco current_to_long(uint32_t long_index) {
    H264Mmco result = mmco(6);
    result.long_term_frame_idx = long_index;
    return result;
}

H264RefListModification short_mod(uint32_t idc, uint32_t difference_minus1) {
    H264RefListModification result;
    result.idc = idc;
    result.abs_diff_pic_num_minus1 = difference_minus1;
    return result;
}

H264RefListModification long_mod(uint32_t long_pic_num) {
    H264RefListModification result;
    result.idc = 2;
    result.long_term_pic_num = long_pic_num;
    return result;
}

H264Picture begin(H264Dpb &dpb, const H264Slice &input, int index) {
    H264Picture picture;
    require_status(dpb.begin_picture(input, index, index * 100, &picture),
                   H264DpbStatus::kOk, "begin_picture");
    return picture;
}

H264Picture decode(H264Dpb &dpb, const H264Slice &input, int index) {
    H264Picture picture = begin(dpb, input, index);
    require_status(dpb.finish_picture(input, &picture), H264DpbStatus::kOk,
                   "finish_picture frame_num=" + std::to_string(input.frame_num));
    return picture;
}

std::vector<H264Reference> references(H264Dpb &dpb, H264Slice input,
                                      const H264Picture &current,
                                      unsigned int list, uint32_t count) {
    input.num_ref_idx_active[list] = count;
    std::vector<H264Reference> result;
    require_status(dpb.reference_list(input, current, list, &result),
                   H264DpbStatus::kOk, "reference_list");
    return result;
}

std::string indices_string(const std::vector<int> &indices) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < indices.size(); ++index) {
        if (index) output << ',';
        output << indices[index];
    }
    output << ']';
    return output.str();
}

void expect_indices(const std::vector<H264Reference> &references,
                    const std::vector<int> &expected, const std::string &where) {
    std::vector<int> actual;
    for (const H264Reference &reference : references) {
        actual.push_back(reference.picture_index);
    }
    require(actual == expected, where + ": expected " + indices_string(expected) +
                                    ", got " + indices_string(actual));
}

void expect_outputs(const std::vector<H264Output> &outputs,
                    const std::vector<int> &expected, const std::string &where) {
    std::vector<int> actual;
    for (const H264Output &output : outputs) actual.push_back(output.picture_index);
    require(actual == expected, where + ": expected " + indices_string(expected) +
                                    ", got " + indices_string(actual));
}

const H264Picture &stored(const H264Dpb &dpb, int picture_index) {
    for (const H264Picture &picture : dpb.pictures()) {
        if (picture.picture_index == picture_index) return picture;
    }
    throw std::runtime_error("picture not stored: " + std::to_string(picture_index));
}

void test_surface_exhaustion() {
    H264Dpb dpb;
    H264Sequence invalid = sequence();
    invalid.max_dpb_frames = 0;
    require_status(dpb.reset(invalid), H264DpbStatus::kInvalidArgument,
                   "reject zero DPB capacity");
    require_status(dpb.reset(sequence(0, 1, 1)), H264DpbStatus::kOk, "reset");
    decode(dpb, idr(), 0);

    int index = -1;
    require_status(dpb.choose_surface(1, {false}, &index),
                   H264DpbStatus::kNoFreeSurface, "DPB-held surface");
    require_status(dpb.drain(), H264DpbStatus::kOk, "drain");
    require_status(dpb.choose_surface(1, {false}, &index),
                   H264DpbStatus::kNoFreeSurface, "pending-output surface");
    expect_outputs(dpb.take_outputs(0, true), {0}, "drained output");
    require_status(dpb.choose_surface(1, {false}, &index), H264DpbStatus::kOk,
                   "released surface");
    require(index == 0, "expected surface zero");
    require_status(dpb.choose_surface(1, {true}, &index),
                   H264DpbStatus::kNoFreeSurface, "mapped surface");
    require_status(dpb.choose_surface(1, {}, &index), H264DpbStatus::kInvalidArgument,
                   "mapped vector too short");
}

void test_poc0_wrap_nonreference_and_mmco5() {
    H264Dpb dpb;
    require_status(dpb.reset(sequence()), H264DpbStatus::kOk, "POC0 reset");
    require(decode(dpb, idr(), 0).poc == 0, "IDR POC");
    require(decode(dpb, slice(H264SliceType::kP, 1, 6), 1).poc == 6,
            "initial POC0 reference");
    require(decode(dpb, slice(H264SliceType::kP, 2, 14), 2).poc == 14,
            "POC before wrap");
    require(decode(dpb, slice(H264SliceType::kB, 3, 0, false), 3).poc == 16,
            "non-reference wrap POC");

    H264Slice next = slice(H264SliceType::kP, 4, 2);
    H264Picture current = begin(dpb, next, 4);
    require(current.poc == 18, "non-reference picture must not update POC0 history");
    require_status(dpb.finish_picture(next, &current), H264DpbStatus::kOk,
                   "POC0 reference finish");

    H264Slice reset = slice(H264SliceType::kP, 5, 4);
    reset.adaptive_ref_pic_marking = true;
    reset.mmco = {mmco(5)};
    current = begin(dpb, reset, 5);
    require(current.poc == 20, "POC before MMCO5 adjustment");
    require_status(dpb.finish_picture(reset, &current), H264DpbStatus::kOk,
                   "MMCO5 finish");
    require(current.poc == 0 && current.frame_num == 0,
            "MMCO5 resets current POC and frame_num");

    require(decode(dpb, slice(H264SliceType::kP, 1, 2), 6).poc == 2,
            "POC history after MMCO5");
    require_status(dpb.drain(), H264DpbStatus::kOk, "POC0 drain");
    const std::vector<H264Output> outputs = dpb.take_outputs(0, true);
    require(outputs.size() == 7, "all POC0 pictures drained");
    for (std::size_t index = 1; index < outputs.size(); ++index) {
        require(outputs[index - 1].sequence_id < outputs[index].sequence_id ||
                    (outputs[index - 1].sequence_id == outputs[index].sequence_id &&
                     outputs[index - 1].poc <= outputs[index].poc),
                "output sequence/POC ordering");
    }
    require(outputs.front().picture_index == 0 && outputs.back().picture_index == 6,
            "MMCO5 must not reorder new sequence before old sequence");
}

void test_poc1_cycle_and_wrap() {
    H264Sequence config = sequence(1);
    config.offset_for_ref_frame = {2, -1, 3};
    config.offset_for_non_ref_pic = -2;
    config.offset_for_top_to_bottom_field = 1;
    H264Dpb dpb;
    require_status(dpb.reset(config), H264DpbStatus::kOk, "POC1 reset");
    require(decode(dpb, idr(), 0).poc == 0, "POC1 IDR");
    require(decode(dpb, slice(H264SliceType::kP, 1, 0), 1).poc == 2,
            "POC1 first cycle entry");
    require(decode(dpb, slice(H264SliceType::kB, 2, 0, false), 2).poc == 0,
            "POC1 non-reference adjustment");
    require(decode(dpb, slice(H264SliceType::kP, 3, 0), 3).poc == 4,
            "POC1 complete cycle");
    require(decode(dpb, slice(H264SliceType::kP, 4, 0), 4).poc == 6,
            "POC1 next cycle");

    H264Sequence wrapping = sequence(1, 2, 2);
    wrapping.log2_max_frame_num = 4;
    wrapping.offset_for_ref_frame = {2};
    require_status(dpb.reset(wrapping), H264DpbStatus::kOk, "POC1 wrap reset");
    decode(dpb, idr(), 0);
    require(decode(dpb, slice(H264SliceType::kP, 15, 0), 1).poc == 30,
            "POC1 before frame_num wrap");
    require(decode(dpb, slice(H264SliceType::kP, 0, 0), 2).poc == 32,
            "POC1 frame_num wrap");
}

void test_poc2_nonreference_and_wrap() {
    H264Sequence config = sequence(2, 2, 4);
    config.log2_max_frame_num = 4;
    H264Dpb dpb;
    require_status(dpb.reset(config), H264DpbStatus::kOk, "POC2 reset");
    require(decode(dpb, idr(), 0).poc == 0, "POC2 IDR");
    require(decode(dpb, slice(H264SliceType::kP, 1, 0), 1).poc == 2,
            "POC2 reference");
    require(decode(dpb, slice(H264SliceType::kB, 2, 0, false), 2).poc == 3,
            "POC2 non-reference odd POC");
    require(decode(dpb, slice(H264SliceType::kP, 3, 0), 3).poc == 6,
            "POC2 next reference");
    require(decode(dpb, slice(H264SliceType::kP, 15, 0), 4).poc == 30,
            "POC2 before frame_num wrap");
    require(decode(dpb, slice(H264SliceType::kP, 0, 0), 5).poc == 32,
            "POC2 wrap reference");
    require(decode(dpb, slice(H264SliceType::kB, 1, 0, false), 6).poc == 33,
            "POC2 wrap non-reference");
}

void test_sliding_window_preserves_long_term() {
    H264Dpb dpb;
    require_status(dpb.reset(sequence(0, 2, 4)), H264DpbStatus::kOk, "LTR reset");
    decode(dpb, idr(true), 0);
    decode(dpb, slice(H264SliceType::kP, 1, 2), 1);
    decode(dpb, slice(H264SliceType::kP, 2, 4), 2);
    require(stored(dpb, 0).reference == H264ReferenceType::kLongTerm,
            "sliding window must preserve LTR");
    require(stored(dpb, 1).reference == H264ReferenceType::kUnused,
            "sliding window evicts oldest short-term reference");
    require(stored(dpb, 2).reference == H264ReferenceType::kShortTerm,
            "current picture inserted as short-term");

    H264Slice next = slice(H264SliceType::kP, 3, 6);
    H264Picture current = begin(dpb, next, 3);
    expect_indices(references(dpb, next, current, 0, 2), {2, 0},
                   "P list orders short-term before long-term");
    require_status(dpb.finish_picture(next, &current), H264DpbStatus::kOk,
                   "LTR next finish");
}

void test_mmco_operations_and_rollback() {
    H264Dpb dpb;
    require_status(dpb.reset(sequence(0, 4, 12)), H264DpbStatus::kOk, "MMCO reset");
    decode(dpb, idr(), 0);
    decode(dpb, slice(H264SliceType::kP, 1, 2), 1);
    decode(dpb, slice(H264SliceType::kP, 2, 4), 2);

    H264Slice operation = slice(H264SliceType::kP, 3, 6);
    operation.adaptive_ref_pic_marking = true;
    operation.mmco = {set_max_long(3), short_to_long(1, 1)};
    decode(dpb, operation, 3);
    require(stored(dpb, 1).reference == H264ReferenceType::kLongTerm &&
                stored(dpb, 1).long_term_frame_idx == 1,
            "MMCO3 converts short-term to long-term");

    operation = slice(H264SliceType::kP, 4, 8);
    operation.adaptive_ref_pic_marking = true;
    operation.mmco = {mmco(2)};
    operation.mmco[0].long_term_pic_num = 1;
    decode(dpb, operation, 4);
    require(stored(dpb, 1).reference == H264ReferenceType::kUnused,
            "MMCO2 releases long-term reference");

    operation = slice(H264SliceType::kP, 5, 10);
    operation.adaptive_ref_pic_marking = true;
    operation.mmco = {set_max_long(1), mmco(1), current_to_long(0)};
    H264Picture current = decode(dpb, operation, 5);
    require(current.reference == H264ReferenceType::kLongTerm &&
                current.long_term_frame_idx == 0,
            "MMCO6 marks current picture long-term");

    operation = slice(H264SliceType::kP, 6, 12);
    operation.adaptive_ref_pic_marking = true;
    operation.mmco = {set_max_long(0)};
    decode(dpb, operation, 6);
    require(stored(dpb, 5).reference == H264ReferenceType::kUnused,
            "MMCO4 zero releases every long-term reference");

    operation = slice(H264SliceType::kP, 7, 14);
    operation.adaptive_ref_pic_marking = true;
    operation.mmco = {mmco(1)};
    decode(dpb, operation, 7);
    require(stored(dpb, 6).reference == H264ReferenceType::kUnused,
            "MMCO1 releases selected short-term reference");

    const std::size_t old_size = dpb.pictures().size();
    operation = slice(H264SliceType::kP, 8, 0);
    operation.adaptive_ref_pic_marking = true;
    operation.mmco = {mmco(2)};
    operation.mmco[0].long_term_pic_num = 15;
    current = begin(dpb, operation, 8);
    require_status(dpb.finish_picture(operation, &current),
                   H264DpbStatus::kReferenceNotFound, "missing MMCO2 reference");
    require(dpb.pictures().size() == old_size && !dpb.contains_picture_index(8),
            "failed finish_picture must roll back DPB state");

    operation.mmco = {mmco(1)};
    operation.mmco[0].difference_of_pic_nums_minus1 = 16;
    require_status(dpb.finish_picture(operation, &current),
                   H264DpbStatus::kInvalidArgument, "out-of-range MMCO1 difference");
    require(dpb.pictures().size() == old_size && !dpb.contains_picture_index(8),
            "invalid MMCO must also preserve DPB state");
}

void test_b_list_order_and_independent_modifications() {
    H264Dpb dpb;
    require_status(dpb.reset(sequence()), H264DpbStatus::kOk, "B list reset");
    decode(dpb, idr(), 0);
    decode(dpb, slice(H264SliceType::kP, 1, 6), 1);
    H264Slice convert = slice(H264SliceType::kP, 2, 8);
    convert.adaptive_ref_pic_marking = true;
    convert.mmco = {set_max_long(1), short_to_long(0, 0)};
    decode(dpb, convert, 2);

    H264Slice current_slice = slice(H264SliceType::kB, 3, 4, false);
    H264Picture current = begin(dpb, current_slice, 3);
    expect_indices(references(dpb, current_slice, current, 0, 3), {0, 2, 1},
                   "B list0 POC groups plus long-term");
    expect_indices(references(dpb, current_slice, current, 1, 3), {2, 0, 1},
                   "B list1 POC groups plus long-term");

    current_slice.modifications[0] = {short_mod(0, 0)};
    current_slice.modifications[1] = {long_mod(0)};
    expect_indices(references(dpb, current_slice, current, 0, 2), {2, 0},
                   "L0 uses only L0 modifications");
    expect_indices(references(dpb, current_slice, current, 1, 2), {1, 2},
                   "L1 uses only L1 modifications");
}

void test_short_modification_wrap_and_missing_reference() {
    H264Sequence config = sequence(0, 2, 2);
    config.log2_max_frame_num = 4;
    H264Dpb dpb;
    require_status(dpb.reset(config), H264DpbStatus::kOk, "mod wrap reset");
    decode(dpb, idr(), 0);
    decode(dpb, slice(H264SliceType::kP, 15, 6), 1);

    H264Slice wrapped = slice(H264SliceType::kP, 0, 0);
    wrapped.modifications[0] = {short_mod(0, 0)};
    H264Picture current = begin(dpb, wrapped, 2);
    expect_indices(references(dpb, wrapped, current, 0, 1), {1},
                   "short-term modification across frame_num wrap");

    wrapped.modifications[0] = {long_mod(0)};
    wrapped.num_ref_idx_active[0] = 1;
    std::vector<H264Reference> result;
    require_status(dpb.reference_list(wrapped, current, 0, &result),
                   H264DpbStatus::kReferenceNotFound, "missing modified reference");
}

void test_output_delay_drain_and_idr_discard() {
    H264Dpb dpb;
    require_status(dpb.reset(sequence(0, 1, 2)), H264DpbStatus::kOk, "output reset");
    decode(dpb, idr(), 0);
    decode(dpb, slice(H264SliceType::kP, 1, 2), 1);
    decode(dpb, slice(H264SliceType::kB, 2, 4, false), 2);
    expect_outputs(dpb.take_outputs(1), {}, "display delay retains one output");
    decode(dpb, slice(H264SliceType::kP, 3, 6), 3);
    expect_outputs(dpb.take_outputs(1), {0}, "display delay releases oldest output");
    require_status(dpb.drain(), H264DpbStatus::kOk, "EOS drain");
    expect_outputs(dpb.take_outputs(0, true), {1, 2, 3}, "EOS POC order");

    require_status(dpb.reset(sequence()), H264DpbStatus::kOk, "IDR discard reset");
    decode(dpb, idr(), 0);
    decode(dpb, slice(H264SliceType::kP, 1, 2), 1);
    H264Slice discard = idr();
    discard.no_output_of_prior_pics = true;
    decode(dpb, discard, 10);
    require_status(dpb.drain(), H264DpbStatus::kOk, "discard drain");
    expect_outputs(dpb.take_outputs(0, true), {10},
                   "no_output_of_prior_pics discards old output");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"surface_exhaustion", test_surface_exhaustion},
        {"poc0_wrap_nonreference_and_mmco5", test_poc0_wrap_nonreference_and_mmco5},
        {"poc1_cycle_and_wrap", test_poc1_cycle_and_wrap},
        {"poc2_nonreference_and_wrap", test_poc2_nonreference_and_wrap},
        {"sliding_window_preserves_long_term", test_sliding_window_preserves_long_term},
        {"mmco_operations_and_rollback", test_mmco_operations_and_rollback},
        {"b_list_order_and_independent_modifications",
         test_b_list_order_and_independent_modifications},
        {"short_modification_wrap_and_missing_reference",
         test_short_modification_wrap_and_missing_reference},
        {"output_delay_drain_and_idr_discard", test_output_delay_drain_and_idr_discard},
    };

    int failures = 0;
    for (const auto &test : tests) {
        try {
            test.second();
            std::cout << "PASS " << test.first << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
        }
    }
    std::cout << "H264 DPB: " << tests.size() - failures << '/' << tests.size()
              << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
