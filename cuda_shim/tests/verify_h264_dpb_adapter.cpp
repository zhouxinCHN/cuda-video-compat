// FFmpeg/CUVID syntax-adapter tests and real-Annex-B POC differential.

#include "h264_dpb_adapter.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include "libavcodec/golomb.h"
#include "libavcodec/h2645_parse.h"
#include "libavcodec/h264_parse.h"
#include "libavcodec/mpegutils.h"
}

using cuvid_shim::H264Dpb;
using cuvid_shim::H264DpbStatus;
using cuvid_shim::H264Output;
using cuvid_shim::H264Picture;
using cuvid_shim::H264Sequence;
using cuvid_shim::H264Slice;
using cuvid_shim::H264SliceParseParams;
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

class BitWriter {
public:
    void bit(uint32_t value) { bits(1, value); }

    void bits(unsigned int count, uint32_t value) {
        for (unsigned int index = 0; index < count; ++index) {
            const unsigned int shift = count - index - 1;
            bit_position(static_cast<unsigned int>(bit_position_ / 8),
                         static_cast<unsigned int>(bit_position_ % 8), (value >> shift) & 1);
            ++bit_position_;
        }
    }

    void ue(uint32_t value) {
        const uint32_t code = value + 1;
        unsigned int width = 0;
        for (uint32_t remaining = code; remaining; remaining >>= 1) ++width;
        bits(width - 1, 0);
        bits(width, code);
    }

    void se(int32_t value) {
        ue(value <= 0 ? static_cast<uint32_t>(-static_cast<int64_t>(value) * 2)
                      : static_cast<uint32_t>(value) * 2 - 1);
    }

    const std::vector<uint8_t> &data() const { return data_; }

private:
    void bit_position(unsigned int byte, unsigned int bit, uint32_t value) {
        if (byte >= data_.size()) data_.push_back(0);
        if (value) data_[byte] |= static_cast<uint8_t>(1U << (7 - bit));
    }

    std::vector<uint8_t> data_;
    std::size_t bit_position_ = 0;
};

H264Slice parse_bits(const std::vector<uint8_t> &data, int nal_type,
                     int nal_ref_idc, const H264SliceParseParams &params) {
    std::vector<uint8_t> padded = data;
    padded.resize(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    GetBitContext gb;
    require(init_get_bits8(&gb, padded.data(), static_cast<int>(data.size())) >= 0,
            "init_get_bits8");
    H264Slice slice;
    require_status(cuvid_shim::parse_h264_slice_header(gb, nal_type, nal_ref_idc,
                                                        params, &slice),
                   H264DpbStatus::kOk, "parse_h264_slice_header");
    return slice;
}

void test_sequence_conversion_and_poc_override() {
    SPS sps = {};
    sps.log2_max_frame_num = 6;
    sps.log2_max_poc_lsb = 8;
    sps.poc_type = 1;
    sps.poc_cycle_length = 2;
    sps.offset_for_ref_frame[0] = 3;
    sps.offset_for_ref_frame[1] = -1;
    sps.offset_for_non_ref_pic = -2;
    sps.offset_for_top_to_bottom_field = 1;
    sps.ref_frame_count = 4;
    sps.max_dec_frame_buffering = 5;
    sps.frame_mbs_only_flag = 1;
    sps.chroma_format_idc = 1;
    PPS pps = {};
    pps.ref_count[0] = 2;
    pps.ref_count[1] = 3;
    pps.pic_order_present = 1;
    pps.weighted_bipred_idc = 1;

    H264Sequence sequence;
    require_status(cuvid_shim::h264_sequence_from_ffmpeg(sps, 5, &sequence),
                   H264DpbStatus::kOk, "FFmpeg sequence conversion");
    require(sequence.log2_max_frame_num == 6 && sequence.log2_max_poc_lsb == 8 &&
                sequence.poc_type == 1 && sequence.offset_for_ref_frame.size() == 2 &&
                sequence.offset_for_ref_frame[0] == 3 &&
                sequence.offset_for_ref_frame[1] == -1 &&
                sequence.max_num_ref_frames == 4 && sequence.max_dpb_frames == 5,
            "FFmpeg sequence fields");
    require(cuvid_shim::h264_dpb_capacity_from_ffmpeg(sps) == 5,
            "FFmpeg DPB capacity");

    H264SliceParseParams params;
    require_status(cuvid_shim::h264_slice_params_from_ffmpeg(sps, pps, &params),
                   H264DpbStatus::kOk, "FFmpeg slice params");
    require(params.default_ref_count[0] == 2 && params.default_ref_count[1] == 3 &&
                params.pic_order_present && !params.has_poc_override,
            "FFmpeg slice params fields");

    CUVIDH264PICPARAMS h264 = {};
    h264.log2_max_frame_num_minus4 = 1;
    h264.log2_max_pic_order_cnt_lsb_minus4 = 2;
    h264.pic_order_cnt_type = 1;
    h264.num_ref_frames = 3;
    h264.frame_mbs_only_flag = 1;
    h264.num_ref_idx_l0_active_minus1 = 1;
    h264.num_ref_idx_l1_active_minus1 = 2;
    h264.CurrFieldOrderCnt[0] = 10;
    h264.CurrFieldOrderCnt[1] = 12;
    require_status(cuvid_shim::h264_sequence_from_cuvid(h264, 6, &sequence),
                   H264DpbStatus::kOk, "CUVID sequence conversion");
    require(sequence.offset_for_ref_frame.empty() && sequence.max_dpb_frames == 6,
            "CUVID does not invent POC1 offsets");
    require_status(cuvid_shim::h264_slice_params_from_cuvid(h264, &params),
                   H264DpbStatus::kOk, "CUVID slice params");
    require(params.has_poc_override && params.poc_override[0] == 10 &&
                params.poc_override[1] == 12 && params.default_ref_count[0] == 2 &&
                params.default_ref_count[1] == 3,
            "CUVID POC override fields");

    H264Dpb dpb;
    require_status(dpb.reset(sequence), H264DpbStatus::kOk, "override DPB reset");
    H264Slice input;
    input.is_idr = true;
    input.is_reference = true;
    input.has_poc_override = true;
    input.poc_override[0] = 10;
    input.poc_override[1] = 12;
    H264Picture picture;
    require_status(dpb.begin_picture(input, 0, 0, &picture), H264DpbStatus::kOk,
                   "override begin");
    require(picture.poc == 10, "CUVID POC override is used");
}

void write_poc1_b_slice(BitWriter *writer) {
    writer->ue(0); // first_mb_in_slice
    writer->ue(6); // B slice (1 + 5)
    writer->ue(0); // pps_id
    writer->bits(4, 3); // frame_num
    writer->se(2); // delta_pic_order_cnt[0]
    writer->se(-1); // delta_pic_order_cnt[1]
    writer->bit(1); // direct_spatial_mv_pred_flag
    writer->bit(0); // num_ref_idx_active_override_flag (1/1 references)
    writer->bit(0); // ref_pic_list_modification_flag_l0
    writer->bit(0); // ref_pic_list_modification_flag_l1

    writer->ue(2); // luma_log2_weight_denom
    writer->ue(3); // chroma_log2_weight_denom
    writer->bit(1); // luma_weight_l0_flag
    writer->se(8);
    writer->se(-2);
    writer->bit(1); // chroma_weight_l0_flag (shared by Cb/Cr)
    writer->se(4);  // Cb weight
    writer->se(-1); // Cb offset
    writer->se(5);  // Cr weight
    writer->se(1);  // Cr offset
    writer->bit(0); // luma_weight_l1_flag
    writer->bit(0); // chroma_weight_l1_flag (shared by Cb/Cr)

    writer->bit(1); // adaptive_ref_pic_marking_mode_flag
    writer->ue(4);
    writer->ue(3); // MaxLongTermFrameIdx = 2
    writer->ue(3);
    writer->ue(1); // short-term difference
    writer->ue(1); // long-term index 1
    writer->ue(2);
    writer->ue(0); // long-term pic num 0
    writer->ue(1);
    writer->ue(0); // short-term difference
    writer->ue(6);
    writer->ue(1); // current long-term index 1
    writer->ue(5); // MMCO 5
    writer->ue(0); // terminator
}

void test_slice_syntax_poc1_mmco_and_weights() {
    H264SliceParseParams params;
    params.poc_type = 1;
    params.pic_order_present = true;
    params.weighted_bipred_idc = 1;
    BitWriter writer;
    write_poc1_b_slice(&writer);
    H264Slice slice = parse_bits(writer.data(), 1, 1, params);
    require(slice.type == H264SliceType::kB && slice.frame_num == 3 &&
                slice.delta_poc[0] == 2 && slice.delta_poc[1] == -1 &&
                slice.num_ref_idx_active[0] == 1 && slice.num_ref_idx_active[1] == 1,
            "POC1/B slice fields");
    const std::vector<uint32_t> expected = {4, 3, 2, 1, 6, 5};
    require(slice.mmco.size() == expected.size(), "MMCO operation count");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        require(slice.mmco[index].opcode == expected[index],
                "MMCO opcode at " + std::to_string(index));
    }
    require(slice.mmco[0].max_long_term_frame_idx_plus1 == 3 &&
                slice.mmco[1].difference_of_pic_nums_minus1 == 1 &&
                slice.mmco[1].long_term_frame_idx == 1 &&
                slice.mmco[2].long_term_pic_num == 0 &&
                slice.mmco[3].difference_of_pic_nums_minus1 == 0 &&
                slice.mmco[4].long_term_frame_idx == 1,
            "MMCO arguments");
}

void test_independent_ref_list_modifications() {
    H264SliceParseParams params;
    params.poc_type = 0;
    BitWriter writer;
    writer.ue(0);
    writer.ue(6); // B
    writer.ue(0);
    writer.bits(4, 7);
    writer.bits(4, 2); // POC lsb
    writer.bit(0); // direct spatial
    writer.bit(0); // default 1/1 references
    writer.bit(1); // L0 modification flag
    writer.ue(2);
    writer.ue(1); // long-term pic num 1
    writer.ue(3);
    writer.bit(1); // L1 modification flag
    writer.ue(0);
    writer.ue(0); // short-term difference 1
    writer.ue(3);
    writer.bit(0); // sliding window
    H264Slice slice = parse_bits(writer.data(), 1, 1, params);
    require(slice.modifications[0].size() == 1 && slice.modifications[1].size() == 1 &&
                slice.modifications[0][0].idc == 2 &&
                slice.modifications[0][0].long_term_pic_num == 1 &&
                slice.modifications[1][0].idc == 0 &&
                slice.modifications[1][0].abs_diff_pic_num_minus1 == 0,
            "L0/L1 modifications remain independent");

    BitWriter truncated;
    truncated.ue(0);
    truncated.ue(0); // P
    truncated.ue(0);
    truncated.bits(4, 1);
    // Missing POC lsb.
    std::vector<uint8_t> padded = truncated.data();
    padded.resize(truncated.data().size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    GetBitContext gb;
    require(init_get_bits8(&gb, padded.data(),
                           static_cast<int>(truncated.data().size())) >= 0,
            "truncated init");
    H264Slice rejected;
    require_status(cuvid_shim::parse_h264_slice_header(gb, 1, 1, params, &rejected),
                   H264DpbStatus::kInvalidArgument, "truncated slice rejection");
}

std::vector<uint8_t> read_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "open " + path.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

const SPS *first_sps(const H264ParamSets &ps) {
    for (const SPS *sps : ps.sps_list) {
        if (sps) return sps;
    }
    return nullptr;
}

const PPS *first_pps(const H264ParamSets &ps) {
    for (const PPS *pps : ps.pps_list) {
        if (pps) return pps;
    }
    return nullptr;
}

void append_outputs(std::vector<H264Output> *all, std::vector<H264Output> additions) {
    all->insert(all->end(), additions.begin(), additions.end());
}

std::size_t test_real_stream(const std::filesystem::path &path) {
    const std::vector<uint8_t> bytes = read_file(path);
    AVCodecContext avctx = {};
    avctx.codec_id = AV_CODEC_ID_H264;
    H264ParamSets ps = {};
    H2645Packet packet = {};
    int result = ff_h2645_packet_split(&packet, bytes.data(), static_cast<int>(bytes.size()),
                                       &avctx, 0, AV_CODEC_ID_H264, 0);
    require(result >= 0, "split " + path.string());

    H264Dpb dpb;
    H264Sequence current_sequence = {};
    bool have_sequence = false;
    std::vector<uint8_t> sps_signature;
    H264POCContext reference_poc = {};
    reference_poc.prev_poc_lsb = -1;
    int picture_index = 0;
    std::vector<H264Output> outputs;
    std::size_t parsed_pictures = 0;
    std::size_t long_term_pictures = 0;
    std::size_t mmco_resets = 0;
    std::size_t modification_commands = 0;

    for (int index = 0; index < packet.nb_nals; ++index) {
        const H2645NAL &nal = packet.nals[index];
        if (nal.type == 7) {
            GetBitContext gb = nal.gb;
            result = ff_h264_decode_seq_parameter_set(&gb, &avctx, &ps, 0);
            require(result >= 0, "SPS " + path.string());
            const SPS *sps = first_sps(ps);
            require(sps != nullptr, "active SPS");
            std::vector<uint8_t> signature(sps->data, sps->data + sps->data_size);
            if (signature != sps_signature) {
                if (have_sequence) {
                    require_status(dpb.drain(), H264DpbStatus::kOk, "sequence-change drain");
                    append_outputs(&outputs, dpb.take_outputs(0, true));
                }
                require_status(cuvid_shim::h264_sequence_from_ffmpeg(
                                   *sps, cuvid_shim::h264_dpb_capacity_from_ffmpeg(*sps),
                                   &current_sequence),
                               H264DpbStatus::kOk,
                               "stream sequence conversion " + path.filename().string());
                require_status(dpb.reset(current_sequence), H264DpbStatus::kOk,
                               "stream DPB reset");
                have_sequence = true;
                sps_signature = std::move(signature);
                reference_poc = {};
                reference_poc.prev_poc_lsb = -1;
            }
        } else if (nal.type == 8) {
            GetBitContext gb = nal.gb;
            result = ff_h264_decode_picture_parameter_set(&gb, &avctx, &ps, nal.size_bits);
            require(result >= 0, "PPS " + path.string());
        } else if ((nal.type == 1 || nal.type == 5) && have_sequence) {
            const SPS *sps = first_sps(ps);
            const PPS *pps = first_pps(ps);
            require(sps && pps, "slice parameter sets");
            GetBitContext first = nal.gb;
            const uint32_t first_mb = get_ue_golomb_long(&first);
            if (first_mb != 0) continue;

            H264SliceParseParams params;
            require_status(cuvid_shim::h264_slice_params_from_ffmpeg(*sps, *pps, &params),
                           H264DpbStatus::kOk, "stream slice params");
            H264Slice input;
            require_status(cuvid_shim::parse_h264_slice_header(nal.gb, nal.type,
                                                                nal.ref_idc, params, &input),
                           H264DpbStatus::kOk,
                           "stream slice parse " + path.filename().string() +
                               " picture " + std::to_string(parsed_pictures));

            reference_poc.frame_num = static_cast<int>(input.frame_num);
            reference_poc.poc_lsb = static_cast<int>(input.poc_lsb);
            reference_poc.delta_poc_bottom = input.delta_poc_bottom;
            reference_poc.delta_poc[0] = input.delta_poc[0];
            reference_poc.delta_poc[1] = input.delta_poc[1];
            if (input.is_idr) {
                reference_poc = {};
                reference_poc.prev_poc_lsb = -1;
                reference_poc.frame_num = static_cast<int>(input.frame_num);
                reference_poc.poc_lsb = static_cast<int>(input.poc_lsb);
                reference_poc.delta_poc_bottom = input.delta_poc_bottom;
                reference_poc.delta_poc[0] = input.delta_poc[0];
                reference_poc.delta_poc[1] = input.delta_poc[1];
            }
            int field_poc[2] = {0, 0};
            int expected_poc = 0;
            require(ff_h264_init_poc(field_poc, &expected_poc, sps, &reference_poc,
                                     PICT_FRAME, nal.ref_idc) >= 0,
                    "FFmpeg POC");

            H264Picture picture;
            require_status(dpb.begin_picture(input, picture_index, picture_index * 100,
                                              &picture),
                           H264DpbStatus::kOk, "stream begin");
            std::ostringstream context;
            context << path.filename().string() << " picture " << parsed_pictures;
            require(picture.poc == expected_poc,
                    context.str() + " POC expected " + std::to_string(expected_poc) +
                        ", got " + std::to_string(picture.poc));
            for (unsigned int list = 0; list < 2; ++list) {
                if (list == 1 && input.type != H264SliceType::kB) continue;
                std::vector<cuvid_shim::H264Reference> references;
                require_status(dpb.reference_list(input, picture, list, &references),
                               H264DpbStatus::kOk, context.str() + " reference list");
            }
            require_status(dpb.finish_picture(input, &picture), H264DpbStatus::kOk,
                           context.str() + " finish");
            ++parsed_pictures;
            if (picture.reference == cuvid_shim::H264ReferenceType::kLongTerm) {
                ++long_term_pictures;
            }
            modification_commands += input.modifications[0].size() + input.modifications[1].size();
            append_outputs(&outputs, dpb.take_outputs(4));

            reference_poc.prev_poc_msb = reference_poc.poc_msb;
            reference_poc.prev_poc_lsb = reference_poc.poc_lsb;
            reference_poc.prev_frame_num = reference_poc.frame_num;
            reference_poc.prev_frame_num_offset = reference_poc.frame_num_offset;
            bool has_mmco5 = std::any_of(input.mmco.begin(), input.mmco.end(),
                                         [](const cuvid_shim::H264Mmco &operation) {
                                             return operation.opcode == 5;
                                         });
            if (has_mmco5) {
                ++mmco_resets;
                reference_poc.prev_poc_msb = 0;
                reference_poc.prev_poc_lsb = 0;
                reference_poc.prev_frame_num = 0;
                reference_poc.prev_frame_num_offset = 0;
            }
            ++picture_index;
        }
    }
    if (have_sequence) {
        require_status(dpb.drain(), H264DpbStatus::kOk, "stream EOS drain");
        append_outputs(&outputs, dpb.take_outputs(0, true));
    }
    ff_h2645_packet_uninit(&packet);
    ff_h264_ps_uninit(&ps);
    require(outputs.size() == parsed_pictures,
            path.filename().string() + " output count expected " +
                std::to_string(parsed_pictures) + ", got " + std::to_string(outputs.size()));
    std::cout << "  " << path.filename().string() << ": " << parsed_pictures
              << " pictures, poc_type=" << current_sequence.poc_type
              << ", ltr=" << long_term_pictures << ", mmco5=" << mmco_resets
              << ", ref_mods=" << modification_commands << '\n';
    return parsed_pictures;
}

void test_repository_streams() {
    const std::filesystem::path root = std::filesystem::path(__FILE__).parent_path() /
                                       ".." / ".." / "testdata" / "h264";
    std::vector<std::filesystem::path> streams;
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() == ".h264") streams.push_back(entry.path());
    }
    std::sort(streams.begin(), streams.end());
    require(!streams.empty(), "repository H264 streams exist");
    std::size_t pictures = 0;
    const char *only = std::getenv("H264_STREAM");
    for (const auto &stream : streams) {
        if (only && stream.filename() != only) continue;
        const std::size_t count = test_real_stream(stream);
        pictures += count;
    }
    std::cout << "  real-stream differential: " << streams.size() << " streams, "
              << pictures << " pictures\n";
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"sequence_conversion_and_poc_override", test_sequence_conversion_and_poc_override},
        {"slice_syntax_poc1_mmco_and_weights", test_slice_syntax_poc1_mmco_and_weights},
        {"independent_ref_list_modifications", test_independent_ref_list_modifications},
        {"repository_streams", test_repository_streams},
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
    std::cout << "H264 DPB adapter: " << tests.size() - failures << '/' << tests.size()
              << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
