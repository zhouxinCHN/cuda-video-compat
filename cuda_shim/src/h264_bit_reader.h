#ifndef CUDA_SHIM_H264_BIT_READER_H
#define CUDA_SHIM_H264_BIT_READER_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cuvid_shim {

// Minimal H.264 Exp-Golomb / fixed-length bit reader.
// Operates on raw RBSP bytes (start code already stripped).
class BitReader {
public:
    BitReader(const uint8_t *data, std::size_t size)
        : data_(data), size_(size) {}

    bool read_bit(unsigned int *value) {
        if (bit_position_ >= size_ * 8) return false;
        *value = (data_[bit_position_ / 8] >> (7 - bit_position_ % 8)) & 1U;
        ++bit_position_;
        return true;
    }

    bool read_bits(unsigned int count, uint32_t *value) {
        if (count > 32 || bit_position_ + count > size_ * 8) return false;
        *value = 0;
        for (unsigned int i = 0; i < count; ++i) {
            unsigned int bit = 0;
            if (!read_bit(&bit)) return false;
            *value = (*value << 1) | bit;
        }
        return true;
    }

    // Unsigned Exp-Golomb (ue(v)).
    bool read_ue(uint32_t *value) {
        unsigned int zeros = 0;
        unsigned int bit = 0;
        while (read_bit(&bit) && bit == 0) {
            if (++zeros > 31) return false;
        }
        if (bit != 1) return false; // hit end of buffer
        if (zeros == 0) {
            *value = 0;
            return true;
        }
        uint32_t suffix = 0;
        if (!read_bits(zeros, &suffix)) return false;
        *value = ((1U << zeros) - 1U) + suffix;
        return true;
    }

    // Signed Exp-Golomb (se(v)).
    bool read_se(int32_t *value) {
        uint32_t code = 0;
        if (!read_ue(&code)) return false;
        if (code & 1U) {
            *value = static_cast<int32_t>((code + 1U) / 2U);
        } else {
            *value = -static_cast<int32_t>(code / 2U);
        }
        return true;
    }

    // Remaining bits that are not necessarily byte-aligned.
    std::size_t bit_position() const { return bit_position_; }

    // FFmpeg's skip_bits1() style: consume one bit, no bounds reporting.
    bool skip_bits(unsigned int count) {
        if (bit_position_ + count > size_ * 8) return false;
        bit_position_ += count;
        return true;
    }

private:
    const uint8_t *data_;
    std::size_t size_;
    std::size_t bit_position_ = 0;
};

} // namespace cuvid_shim

#endif // CUDA_SHIM_H264_BIT_READER_H
