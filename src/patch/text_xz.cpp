// A small .xz / LZMA2 decoder: enough for xdelta3's LZMA secondary sections (tools/text/vcdiff.py
// uses Python's lzma module there). Written after the LZMA specification and the reference
// decoder in the LZMA SDK (public domain, Igor Pavlov); the .xz container after the XZ file
// format spec. Decodes into one flat buffer (the sections are small), so the "dictionary" is the
// output itself. Supported: one or more blocks with the LZMA2 filter alone, any check type
// (checks and the index are not verified: the VCDIFF window sizes are).

#include "text_internal.hpp"

#include <array>

namespace patch::text {

namespace {

[[noreturn]] void corrupt(const char* what) { throw std::runtime_error(std::string("xz: ") + what); }

// ---------------------------------------------------------------- range decoder

class RangeDecoder {
public:
    RangeDecoder() = default;
    RangeDecoder(View in, size_t pos) : in_(in), pos_(pos) {
        if (next() != 0) corrupt("bad range coder start");
        for (int i = 0; i < 4; ++i) code_ = (code_ << 8) | next();
        range_ = 0xFFFFFFFFu;
        if (code_ == range_) corrupt("bad range coder start");
    }
    size_t pos() const { return pos_; }

    uint32_t bit(uint16_t& prob) {
        const uint32_t bound = (range_ >> 11) * prob;
        uint32_t symbol;
        if (code_ < bound) {
            prob = static_cast<uint16_t>(prob + (((1u << 11) - prob) >> 5));
            range_ = bound;
            symbol = 0;
        } else {
            prob = static_cast<uint16_t>(prob - (prob >> 5));
            code_ -= bound;
            range_ -= bound;
            symbol = 1;
        }
        normalize();
        return symbol;
    }
    uint32_t direct(unsigned bits) {
        uint32_t res = 0;
        do {
            range_ >>= 1;
            code_ -= range_;
            const uint32_t t = 0u - (code_ >> 31);
            code_ += range_ & t;
            if (code_ == range_) corrupt("corrupted direct bits");
            normalize();
            res = (res << 1) + (t + 1);
        } while (--bits);
        return res;
    }

private:
    uint8_t next() {
        if (pos_ >= in_.size()) corrupt("truncated LZMA data");
        return in_[pos_++];
    }
    void normalize() {
        if (range_ < (1u << 24)) {
            range_ <<= 8;
            code_ = (code_ << 8) | next();
        }
    }

    View in_;
    size_t pos_ = 0;
    uint32_t range_ = 0, code_ = 0;
};

constexpr uint16_t kProbInit = 1 << 10;

template <unsigned Bits>
struct BitTree {
    std::array<uint16_t, 1u << Bits> probs;
    void init() { probs.fill(kProbInit); }
    uint32_t decode(RangeDecoder& rc) {
        uint32_t m = 1;
        for (unsigned i = 0; i < Bits; ++i) m = (m << 1) + rc.bit(probs[m]);
        return m - (1u << Bits);
    }
    uint32_t reverse(RangeDecoder& rc) { return reverse_decode(probs.data(), Bits, rc); }
    static uint32_t reverse_decode(uint16_t* probs, unsigned bits, RangeDecoder& rc) {
        uint32_t m = 1, symbol = 0;
        for (unsigned i = 0; i < bits; ++i) {
            const uint32_t bit = rc.bit(probs[m]);
            m = (m << 1) + bit;
            symbol |= bit << i;
        }
        return symbol;
    }
};

struct LenDecoder {
    uint16_t choice = kProbInit, choice2 = kProbInit;
    std::array<BitTree<3>, 16> low, mid;
    BitTree<8> high;
    void init() {
        choice = choice2 = kProbInit;
        for (auto& t : low) t.init();
        for (auto& t : mid) t.init();
        high.init();
    }
    uint32_t decode(RangeDecoder& rc, uint32_t pos_state) {
        if (!rc.bit(choice)) return low[pos_state].decode(rc);
        if (!rc.bit(choice2)) return 8 + mid[pos_state].decode(rc);
        return 16 + high.decode(rc);
    }
};

// ---------------------------------------------------------------- LZMA (inside LZMA2 chunks)

class Lzma {
public:
    void set_props(uint8_t d) {
        if (d >= 9 * 5 * 5) corrupt("bad LZMA properties");
        lc_ = d % 9;
        d /= 9;
        lp_ = d % 5;
        pb_ = d / 5;
        if (lc_ + lp_ > 4) corrupt("bad LZMA2 properties");
        literal_.assign(0x300u << (lc_ + lp_), kProbInit);
    }
    void reset_state() {
        std::fill(literal_.begin(), literal_.end(), kProbInit);
        for (auto& t : pos_slot_) t.init();
        align_.init();
        pos_decoders_.fill(kProbInit);
        is_match_.fill(kProbInit);
        is_rep_.fill(kProbInit);
        is_rep_g0_.fill(kProbInit);
        is_rep_g1_.fill(kProbInit);
        is_rep_g2_.fill(kProbInit);
        is_rep0_long_.fill(kProbInit);
        len_.init();
        rep_len_.init();
        state_ = 0;
        rep0_ = rep1_ = rep2_ = rep3_ = 0;
    }

    /// Decode `unpacked` bytes onto `out`; matches may reach back to `dict_start`.
    void decode(RangeDecoder& rc, Bytes& out, size_t dict_start, size_t unpacked) {
        const size_t end = out.size() + unpacked;
        const uint32_t pb_mask = (1u << pb_) - 1;
        while (out.size() < end) {
            const uint32_t pos_state = static_cast<uint32_t>(out.size() - dict_start) & pb_mask;
            if (!rc.bit(is_match_[(state_ << 4) + pos_state])) {
                literal(rc, out, dict_start);
                state_ = state_ < 4 ? 0 : state_ < 10 ? state_ - 3 : state_ - 6;
                continue;
            }
            uint32_t len;
            if (rc.bit(is_rep_[state_])) {
                if (out.size() == dict_start) corrupt("rep match on an empty dictionary");
                if (!rc.bit(is_rep_g0_[state_])) {
                    if (!rc.bit(is_rep0_long_[(state_ << 4) + pos_state])) {
                        state_ = state_ < 7 ? 9 : 11;
                        out.push_back(out[out.size() - rep0_ - 1]);
                        continue;
                    }
                } else {
                    uint32_t dist;
                    if (!rc.bit(is_rep_g1_[state_])) {
                        dist = rep1_;
                    } else {
                        if (!rc.bit(is_rep_g2_[state_])) {
                            dist = rep2_;
                        } else {
                            dist = rep3_;
                            rep3_ = rep2_;
                        }
                        rep2_ = rep1_;
                    }
                    rep1_ = rep0_;
                    rep0_ = dist;
                }
                len = rep_len_.decode(rc, pos_state);
                state_ = state_ < 7 ? 8 : 11;
            } else {
                rep3_ = rep2_;
                rep2_ = rep1_;
                rep1_ = rep0_;
                len = len_.decode(rc, pos_state);
                state_ = state_ < 7 ? 7 : 10;
                rep0_ = distance(rc, len);
                if (rep0_ == 0xFFFFFFFFu) corrupt("end marker inside an LZMA2 chunk");
            }
            len += 2;
            if (rep0_ >= out.size() - dict_start) corrupt("match distance past the dictionary");
            if (len > end - out.size()) corrupt("match past the chunk");
            for (uint32_t k = 0; k < len; ++k) out.push_back(out[out.size() - rep0_ - 1]);
        }
    }

private:
    void literal(RangeDecoder& rc, Bytes& out, size_t dict_start) {
        const uint32_t prev = out.size() > dict_start ? out.back() : 0;
        const uint32_t pos = static_cast<uint32_t>(out.size() - dict_start);  // positions count from the reset
        const uint32_t lit_state = ((pos & ((1u << lp_) - 1)) << lc_) + (prev >> (8 - lc_));
        uint16_t* probs = &literal_[0x300u * lit_state];
        uint32_t symbol = 1;
        if (state_ >= 7) {
            if (rep0_ >= out.size() - dict_start) corrupt("match byte past the dictionary");
            uint32_t match_byte = out[out.size() - rep0_ - 1];
            do {
                const uint32_t match_bit = (match_byte >> 7) & 1;
                match_byte <<= 1;
                const uint32_t bit = rc.bit(probs[((1 + match_bit) << 8) + symbol]);
                symbol = (symbol << 1) | bit;
                if (match_bit != bit) break;
            } while (symbol < 0x100);
        }
        while (symbol < 0x100) symbol = (symbol << 1) | rc.bit(probs[symbol]);
        out.push_back(static_cast<uint8_t>(symbol - 0x100));
    }

    uint32_t distance(RangeDecoder& rc, uint32_t len) {
        const uint32_t len_state = len < 3 ? len : 3;
        const uint32_t slot = pos_slot_[len_state].decode(rc);
        if (slot < 4) return slot;
        const unsigned direct_bits = (slot >> 1) - 1;
        uint32_t dist = (2 | (slot & 1)) << direct_bits;
        if (slot < 14) {
            dist += BitTree<0>::reverse_decode(pos_decoders_.data() + dist - slot, direct_bits, rc);
        } else {
            dist += rc.direct(direct_bits - 4) << 4;
            dist += align_.reverse(rc);
        }
        return dist;
    }

    uint32_t lc_ = 0, lp_ = 0, pb_ = 0;
    std::vector<uint16_t> literal_;
    std::array<BitTree<6>, 4> pos_slot_;
    BitTree<4> align_;
    std::array<uint16_t, 1 + 128 - 14> pos_decoders_;
    std::array<uint16_t, 12 << 4> is_match_;
    std::array<uint16_t, 12> is_rep_, is_rep_g0_, is_rep_g1_, is_rep_g2_;
    std::array<uint16_t, 12 << 4> is_rep0_long_;
    LenDecoder len_, rep_len_;
    uint32_t state_ = 0, rep0_ = 0, rep1_ = 0, rep2_ = 0, rep3_ = 0;
};

/// One LZMA2 stream at `in[pos...]`, appended to `out`; returns the position after its end byte.
size_t lzma2_decode(View in, size_t pos, Bytes& out, size_t limit) {
    Lzma lzma;
    bool have_props = false, need_dict_reset = true;
    size_t dict_start = out.size();
    const auto byte = [&]() -> uint32_t {
        if (pos >= in.size()) corrupt("truncated LZMA2 data");
        return in[pos++];
    };
    for (;;) {
        const uint32_t control = byte();
        if (control == 0) return pos;  // end of the LZMA2 data
        if (control == 1 || control == 2) {  // uncompressed chunk (1: with a dictionary reset)
            if (control == 1) {
                dict_start = out.size();
                need_dict_reset = false;
            } else if (need_dict_reset) {
                corrupt("LZMA2 chunk without a dictionary reset");
            }
            const size_t size = ((byte() << 8) | byte()) + 1;
            if (pos + size > in.size()) corrupt("truncated LZMA2 data");
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(pos),
                       in.begin() + static_cast<std::ptrdiff_t>(pos + size));
            pos += size;
        } else if (control >= 0x80) {
            size_t unpacked = (control & 0x1F) << 16;
            unpacked += (byte() << 8) | byte();
            unpacked += 1;
            const size_t packed = ((byte() << 8) | byte()) + 1;
            const uint32_t reset = (control >> 5) & 3;  // 0 none, 1 state, 2 +props, 3 +dictionary
            if (reset == 3) {
                dict_start = out.size();
                need_dict_reset = false;
            } else if (need_dict_reset) {
                corrupt("LZMA2 chunk without a dictionary reset");
            }
            if (reset >= 2) {
                lzma.set_props(static_cast<uint8_t>(byte()));
                have_props = true;
            } else if (!have_props) {
                corrupt("LZMA2 chunk without properties");
            }
            if (reset >= 1) lzma.reset_state();
            if (pos + packed > in.size()) corrupt("truncated LZMA2 data");
            RangeDecoder rc(in.first(pos + packed), pos);
            lzma.decode(rc, out, dict_start, unpacked);
            pos += packed;
        } else {
            corrupt("bad LZMA2 control byte");
        }
        if (out.size() > limit) corrupt("more data than expected");
    }
}

/// An .xz multibyte integer (7 bits per byte, least significant first).
uint64_t xz_varint(View in, size_t& pos) {
    uint64_t v = 0;
    for (unsigned shift = 0; shift < 63; shift += 7) {
        if (pos >= in.size()) corrupt("truncated header");
        const uint8_t c = in[pos++];
        v |= uint64_t{c & 0x7Fu} << shift;
        if (!(c & 0x80)) return v;
    }
    corrupt("bad integer");
}

}  // namespace

Bytes xz_decode(View s, size_t limit) {
    static constexpr uint8_t kMagic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};
    if (s.size() < 12 || !std::equal(kMagic, kMagic + 6, s.begin())) corrupt("not an .xz stream");
    const uint8_t check = s[7] & 0x0F;
    static constexpr uint8_t kCheckSize[16] = {0, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, 32, 32, 64, 64, 64};
    size_t pos = 12;
    Bytes out;
    while (out.size() < limit) {
        if (pos >= s.size()) corrupt("truncated stream");
        const size_t header_size = (size_t{s[pos]} + 1) * 4;
        if (s[pos] == 0) break;  // the index: no more blocks
        if (pos + header_size > s.size()) corrupt("truncated block header");
        const View header = s.subspan(pos, header_size - 4);  // without its CRC32
        size_t h = 1;
        const uint8_t flags = header[h++];
        if (flags & 0x3C) corrupt("unsupported block flags");
        if (flags & 0x40) xz_varint(header, h);  // compressed size
        if (flags & 0x80) xz_varint(header, h);  // uncompressed size
        if ((flags & 3) != 0) corrupt("only the LZMA2 filter alone is supported");
        if (xz_varint(header, h) != 0x21) corrupt("only the LZMA2 filter is supported");
        const uint64_t props = xz_varint(header, h);
        if (props != 1 || h >= header.size()) corrupt("bad LZMA2 filter properties");
        pos += header_size;
        const size_t start = pos;
        pos = lzma2_decode(s, pos, out, limit);
        pos += (4 - (pos - start) % 4) % 4;  // block padding
        pos += kCheckSize[check];
    }
    if (out.size() > limit) out.resize(limit);
    return out;
}

}  // namespace patch::text
