#pragma once

#include <iostream>
#include <fstream>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <bit>

class BitWriter {
public:
    explicit BitWriter(std::ostream& os) : out(&os), buffer(0), count(8) {}

    // Write a single bit at the available right-most position of the `buffer`.
    void writeBit(bool bit) {
        if (bit) {
            // 1. mask = 1 << (count - 1)
            // Shift binary representation of 1 to the left by (count - 1) positions
            // (create a mask with a single bit set at position (count - 1)).
            //
            // 2. buffer |= mask
            // Apply bitwise OR assignment operator.
            buffer |= (1 << (count - 1));
        }
        count--;

        // If `buffer` is filled, write it out and reinitialize.
        if (count == 0) {
            write_buf();
            buffer = 0;
            count = 8;
        }
    }

    // Write the `nbits` right-most bits of `u64` to the `buffer` in left-to-right order.
    //
    // E.g., given:
    // * `u64`   = ...0001010101010_000111
    // * `nbits` = 6,
    // it will write `000111` to the buffer.
    void writeBits(uint64_t u64, int nbits) {
        // Left-shit `u64` leaving only `nbits` of meaningful bits (on leading positions).
        // Was:    ...0001010101010_000111
        // Becase: 000111...00000000000000
        u64 <<= (64 - nbits);
        while (nbits >= 8) {
            auto byte = static_cast<uint8_t>(u64 >> 56);
            writeByte(byte);
            u64 <<= 8;
            nbits -= 8;
        }

        while (nbits > 0) {
            bool bit = (u64 >> 63) == 1;
            writeBit(bit);

            u64 <<= 1;
            nbits--;
        }
    }

    // Write a single byte to the stream, regardless of alignment.
    void writeByte(uint8_t byte) {
        //            writing ->
        // buffer:         [xxx*****]
        //  x -- non-empty (3)
        //  * -- empty     (5 = count)
        // 1. Shift `byte` on the number of already taken positions of `buffer`
        //    (3 in this example).
        // Was:    11001100
        // Became: 00011001
        // 2. Write the mask to the `buffer`.
        // 3. Write out the `buffer`.
        // 4. Write the remaining (right-remaining) part of the `byte` to the `buffer`
        //    (00000100 in this example)
        buffer |= (byte >> (8 - count));
        write_buf();
        buffer = byte << count;
    }

    // Empty the currently in-process `buffer` by filling it with 'bit'
    // (all unused right-most bits will be filled with `bit`).
    void flush(bool bit) {
        // `count` will become 8 after `buffer` is written out?
        while (count != 8) {
            writeBit(bit);
        }
    }

private:
    void write_buf() {
        out->write(reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    }

    std::ostream* out;
    uint8_t buffer;
    // How many right-most bits are available for writing in the current byte (the last byte of the buffer).
    uint8_t count;
};

constexpr int32_t FIRST_DELTA_BITS = 14;

uint8_t leading_zeros(uint64_t v) {
    uint64_t mask = 0x8000000000000000;
    uint8_t ret = 0;
    while (ret < 64 && (v & mask) == 0) {
        mask >>= 1;
        ret++;
    }
    return ret;
}

uint8_t trailing_zeros(uint64_t v) {
    uint64_t mask = 0x0000000000000001;
    uint8_t ret = 0;
    while (ret < 64 && (v & mask) == 0) {
        mask <<= 1;
        ret++;
    }
    return ret;
}

class Compressor {
public:
    Compressor(std::ostream& os, uint64_t header) : bw(os), header_(header), leading_zeros_(UINT8_MAX) {
        bw.writeBits(header_, 64);
    }

    void compress(uint64_t t, uint64_t v) {
        if (t_ == 0) {
            int64_t delta = static_cast<int64_t>(t) - static_cast<int64_t>(header_);
            t_ = t;
            t_delta_ = delta;
            value_ = v;

            bw.writeBits(delta, FIRST_DELTA_BITS);
            bw.writeBits(value_, 64);
        } else {
            compressTimestamp(t);
            compressValue(v);
        }
    }

    void finish() {
        if (t_ == 0) {
            bw.writeBits(1<< (FIRST_DELTA_BITS - 1), FIRST_DELTA_BITS);
            bw.writeBits(0, 64);
            bw.flush(false);
            return;
        }

        // 0x0F           = 00001111 -> 1111 (cutted).
        bw.writeBits(0x0F, 4);
        // 0xFFFFFFFF     = 11111111 11111111 11111111 11111111
        bw.writeBits(0xFFFFFFFF, 32);
        bw.writeBit(false);
        bw.flush(false);
    }

private:
    void compressTimestamp(uint64_t t) {
        auto delta = static_cast<int64_t>(t) - static_cast<int64_t>(t_);
        int64_t dod = static_cast<int64_t>(delta) - static_cast<int64_t>(t_delta_);

        t_ = t;
        t_delta_ = delta;

        if (dod == 0) {
            bw.writeBit(false);
        } else if (-63 <= dod && dod <= 64) {
            bw.writeBits(0x02, 2);
            writeInt64Bits(dod, 7);
        } else if (-255 <= dod && dod <= 256) {
            bw.writeBits(0x06, 3);
            writeInt64Bits(dod, 9);
        } else if (-2047 <= dod && dod <= 2048) {
            bw.writeBits(0x0E, 4);
            writeInt64Bits(dod, 12);
        } else {
            bw.writeBits(0x0F, 4);
            writeInt64Bits(dod, 64);
        }
    }

    void writeInt64Bits(int64_t i, int nbits) {
        uint64_t u;
        if (i >= 0 || nbits >= 64) {
            u = static_cast<uint64_t>(i);
        } else {
            u = static_cast<uint64_t>(1 << (nbits + i));
        }
        bw.writeBits(u, int(nbits));
    }

    void compressValue(uint64_t v) {
        uint64_t xor_val = value_ ^ v;
        value_ = v;

        if (xor_val == 0) {
            bw.writeBit(false);
            return;
        }

        uint8_t leading_zeros_val = leading_zeros(xor_val);
        uint8_t trailing_zeros_val = trailing_zeros(xor_val);

        bw.writeBit(true);

        if (leading_zeros_val <= leading_zeros_ && trailing_zeros_val <= trailing_zeros_) {
            bw.writeBit(false);
            int significant_bits = 64 - leading_zeros_ - trailing_zeros_;
            bw.writeBits(xor_val >> trailing_zeros_, significant_bits);
            return;
        }

        leading_zeros_ = leading_zeros_val;
        trailing_zeros_ = trailing_zeros_val;

        bw.writeBit(true);
        bw.writeBits(leading_zeros_val, 5);
        int significant_bits = 64 - leading_zeros_ - trailing_zeros_;
        bw.writeBits(static_cast<uint64_t>(significant_bits), 6);
        bw.writeBits(xor_val >> trailing_zeros_val, significant_bits);
    }

    // Util to apply bit operations.
    BitWriter bw;
    // Header bits.
    uint64_t header_;
    // Last time passed for compression.
    uint64_t t_ = 0;
    // 1.) In case first (time, value) pair passed after header, find delta with header time.
    // 2.) Otherwise, last time delta with new passed time and `t_`.
    int64_t t_delta_ = 0;
    uint8_t leading_zeros_ = 0;
    uint8_t trailing_zeros_ = 0;
    // Last value passed for compression.
    uint64_t value_ = 0;
};