// GTE (COP2): register file and every documented command.
//
// Reference: psx-spx "Geometry Transformation Engine (GTE)". Section names are cited next to
// the code that implements them. Arithmetic follows the documented hardware data path:
// MAC1-3 are 44-bit accumulators whose overflow is checked (and wrapped) after every addition,
// MAC0 is checked against 32 bits, IR/SZ/SXY/colour writes saturate and set FLAG bits.

#include <psx/runtime.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>

namespace {

int32_t sext16(uint32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); }
int32_t hi16(uint32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v >> 16)); }

// ---- FLAG (cop2r63) bits, psx-spx "GTE Saturation / FLAG register" ----
constexpr uint32_t kFlagMacPos[4] = {1u << 16, 1u << 30, 1u << 29, 1u << 28};  ///< MAC0..3 too large
constexpr uint32_t kFlagMacNeg[4] = {1u << 15, 1u << 27, 1u << 26, 1u << 25};  ///< MAC0..3 too small
constexpr uint32_t kFlagIr[4] = {1u << 12, 1u << 24, 1u << 23, 1u << 22};      ///< IR0..3 saturated
constexpr uint32_t kFlagColor[3] = {1u << 21, 1u << 20, 1u << 19};             ///< colour FIFO R,G,B
constexpr uint32_t kFlagSz = 1u << 18;                                          ///< SZ3/OTZ saturated
constexpr uint32_t kFlagDivide = 1u << 17;                                      ///< RTP divide overflow
constexpr uint32_t kFlagSx = 1u << 14;                                          ///< SX2 saturated
constexpr uint32_t kFlagSy = 1u << 13;                                          ///< SY2 saturated
/// Bit 31 is the OR of bits 30-23 and 18-13 (not 22-19).
constexpr uint32_t kFlagErrorMask = 0x7F87E000u;
constexpr uint32_t kFlagWritable = 0x7FFFF000u;

constexpr int64_t kMac44Max = (int64_t{1} << 43) - 1;
constexpr int64_t kMac44Min = -(int64_t{1} << 43);

/// Folds the error summary into FLAG bit 31.
constexpr uint32_t finish_flag(uint32_t f) { return (f & kFlagErrorMask) ? (f | 0x80000000u) : f; }

/// psx-spx "GTE Division Inaccuracy": unr_table[i] = max(0, (40000h/(i+100h)+1)/2 - 101h).
constexpr std::array<uint8_t, 257> make_unr_table() {
    std::array<uint8_t, 257> t{};
    for (int i = 0; i < 257; ++i) {
        const int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        t[static_cast<size_t>(i)] = static_cast<uint8_t>(v < 0 ? 0 : v);
    }
    return t;
}
constexpr std::array<uint8_t, 257> kUnrTable = make_unr_table();

/// RTPS/RTPT perspective divide (H*20000h/SZ3 + 1)/2 via the hardware's Newton-Raphson UNR
/// reciprocal (psx-spx "GTE Division Inaccuracy"). Caller guarantees h < sz3*2.
constexpr uint32_t unr_divide(uint32_t h, uint32_t sz3) {
    const int z = std::countl_zero(static_cast<uint16_t>(sz3));  // 0..15
    const uint64_t n = static_cast<uint64_t>(h) << z;             // 0..7FFF8000h
    const int64_t d0 = static_cast<int64_t>(sz3) << z;            // 8000h..FFFFh
    const int64_t u = kUnrTable[static_cast<size_t>((d0 - 0x7FC0) >> 7)] + 0x101;  // 101h..200h
    const int64_t d1 = (0x2000080 - d0 * u) >> 8;                 // 10000h..FF01h
    const int64_t d2 = (0x0000080 + d1 * u) >> 8;                 // 20000h..10000h
    const uint64_t q = (n * static_cast<uint64_t>(d2) + 0x8000) >> 16;
    return q > 0x1FFFF ? 0x1FFFFu : static_cast<uint32_t>(q);
}

using Vec3 = std::array<int32_t, 3>;
using Mat3 = std::array<Vec3, 3>;

/// One command's view of the register file; accumulates FLAG locally and stores it on exit.
class Gte {
public:
    explicit Gte(PsxContext* ctx) : d_(ctx->gte_data), c_(ctx->gte_ctrl) {}
    ~Gte() { c_[31] = finish_flag(flag_); }
    Gte(const Gte&) = delete;
    Gte& operator=(const Gte&) = delete;

    // ---- commands (psx-spx "GTE Coordinate Calculation Commands" etc.) ----

    /// RTPS/RTPT single vertex: perspective transformation (psx-spx "GTE Coordinate Calculation Commands").
    void rtp(int v, int sh, bool lm, bool last) {
        const Vec3 vec = vertex(v);
        const Mat3 rt = matrix(0);
        int64_t raw[3];
        for (int i = 0; i < 3; ++i) raw[i] = dot(i + 1, rt[static_cast<size_t>(i)], vec, ctrl32(5 + i));
        for (int i = 0; i < 3; ++i) d_[25 + i] = static_cast<uint32_t>(raw[i] >> sh);
        set_ir(1, mac(1), lm);
        set_ir(2, mac(2), lm);
        // IR3 is saturated from MAC3, but with sf=0 its FLAG bit follows "MAC3 SAR 12".
        const int64_t z12 = raw[2] >> 12;
        if (z12 < -0x8000 || z12 > 0x7FFF) flag_ |= kFlagIr[3];
        const int32_t lo = lm ? 0 : -0x8000;
        const int32_t m3 = mac(3);
        d_[11] = static_cast<uint32_t>(m3 < lo ? lo : m3 > 0x7FFF ? 0x7FFF : m3);
        push_sz(z12);  // SZ3 = MAC3 SAR ((1-sf)*12)

        const uint32_t h = c_[26] & 0xFFFF;
        const uint32_t sz3 = d_[19] & 0xFFFF;
        uint32_t n;
        if (h < sz3 * 2) {
            n = unr_divide(h, sz3);
        } else {
            n = 0x1FFFF;
            flag_ |= kFlagDivide;
        }
        const int64_t sx = static_cast<int64_t>(n) * ir(1) + ctrl32(24);
        const int64_t sy = static_cast<int64_t>(n) * ir(2) + ctrl32(25);
        check_mac0(sx);
        check_mac0(sy);
        push_sxy(sx >> 16, sy >> 16);
        if (last) {  // depth cueing: MAC0 = DQB + DQA*n, IR0 = MAC0/1000h
            const int64_t dq = static_cast<int64_t>(sext16(c_[27])) * n + ctrl32(28);
            check_mac0(dq);
            d_[24] = static_cast<uint32_t>(dq);
            set_ir0(dq >> 12);
        }
    }

    /// NCLIP: MAC0 = SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1.
    void nclip() {
        const int64_t x0 = sext16(d_[12]), y0 = hi16(d_[12]);
        const int64_t x1 = sext16(d_[13]), y1 = hi16(d_[13]);
        const int64_t x2 = sext16(d_[14]), y2 = hi16(d_[14]);
        const int64_t r = x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1;
        check_mac0(r);
        d_[24] = static_cast<uint32_t>(r);
    }

    /// OP: outer product of [IR1,IR2,IR3] with the RT diagonal [D1,D2,D3].
    void op(int sh, bool lm) {
        const int64_t d1 = sext16(c_[0]), d2 = sext16(c_[2]), d3 = sext16(c_[4]);
        const int64_t i1 = ir(1), i2 = ir(2), i3 = ir(3);
        set_mac_ir(1, i3 * d2 - i2 * d3, sh, lm);
        set_mac_ir(2, i1 * d3 - i3 * d1, sh, lm);
        set_mac_ir(3, i2 * d1 - i1 * d2, sh, lm);
    }

    /// SQR: [MAC,IR] = IR*IR SAR sf.
    void sqr(int sh, bool lm) {
        for (int i = 1; i <= 3; ++i) set_mac_ir(i, static_cast<int64_t>(ir(i)) * ir(i), sh, lm);
    }

    /// AVSZ3 / AVSZ4: MAC0 = ZSF*(sum SZ), OTZ = MAC0/1000h.
    void avsz(bool four) {
        int64_t sum = sz(1) + sz(2) + sz(3);
        int64_t zsf = sext16(c_[29]);
        if (four) {
            sum += sz(0);
            zsf = sext16(c_[30]);
        }
        const int64_t r = zsf * sum;
        check_mac0(r);
        d_[24] = static_cast<uint32_t>(r);
        d_[7] = static_cast<uint32_t>(sat_flag(r >> 12, 0, 0xFFFF, kFlagSz));
    }

    /// MVMVA: [MAC,IR] = (Tx*1000h + Mx*Vx) SAR sf, including the mx=3 and cv=2 hardware bugs.
    void mvmva(int mx, int vsel, int cv, int sh, bool lm) {
        Mat3 m;
        if (mx == 3) {  // "garbage" matrix: [-R*16, R*16, IR0], [RT13 x3], [RT22 x3]
            const int32_t r = static_cast<int32_t>(d_[6] & 0xFF) << 4;
            const int32_t rt13 = sext16(c_[1]), rt22 = sext16(c_[2]);
            m = {{{-r, r, ir(0)}, {rt13, rt13, rt13}, {rt22, rt22, rt22}}};
        } else {
            m = matrix(mx * 8);
        }
        const Vec3 v = vsel == 3 ? Vec3{ir(1), ir(2), ir(3)} : vertex(vsel);
        if (cv == 2) {
            // FC is bugged: the first product (FC*1000h + Mi1*VX) only sets flags (IR with
            // lm=0), and the result is just (Mi2*VY + Mi3*VZ) SAR sf.
            for (int i = 0; i < 3; ++i) {
                const auto& row = m[static_cast<size_t>(i)];
                const int64_t first =
                    acc(i + 1, (static_cast<int64_t>(ctrl32(21 + i)) << 12) + static_cast<int64_t>(row[0]) * v[0]);
                sat_flag(static_cast<int32_t>(first >> sh), -0x8000, 0x7FFF, kFlagIr[i + 1]);
                const int64_t r = acc(i + 1, acc(i + 1, static_cast<int64_t>(row[1]) * v[1]) +
                                                 static_cast<int64_t>(row[2]) * v[2]);
                set_mac_ir(i + 1, r, sh, lm);
            }
            return;
        }
        const Vec3 t = cv == 3 ? Vec3{0, 0, 0} : translation(cv == 0 ? 5 : 13);
        mat_vec(m, v, t, sh, lm);
    }

    /// NCS/NCT (psx-spx "GTE General Purpose Calculation Commands" / lighting): light + colour.
    void ncs(int v, int sh, bool lm) {
        light(v, sh, lm);
        push_color();
    }

    /// NCCS/NCCT: light, then multiply by RGBC.
    void nccs(int v, int sh, bool lm) {
        light(v, sh, lm);
        color_mul(sh, lm);
    }

    /// NCDS/NCDT: light, multiply by RGBC, depth-cue towards FC.
    void ncds(int v, int sh, bool lm) {
        light(v, sh, lm);
        color_depth(sh, lm);
    }

    /// CC: background + LCM*IR, then multiply by RGBC.
    void cc(int sh, bool lm) {
        mat_vec(matrix(16), ir_vec(), translation(13), sh, lm);
        color_mul(sh, lm);
    }

    /// CDP: background + LCM*IR, multiply by RGBC, depth-cue towards FC.
    void cdp(int sh, bool lm) {
        mat_vec(matrix(16), ir_vec(), translation(13), sh, lm);
        color_depth(sh, lm);
    }

    /// DPCS (RGBC) / DPCT (RGB0, three times): depth-cue a colour towards FC.
    void dpcs(uint32_t rgb, int sh, bool lm) {
        const int64_t in[3] = {static_cast<int64_t>(rgb & 0xFF) << 16, static_cast<int64_t>((rgb >> 8) & 0xFF) << 16,
                               static_cast<int64_t>((rgb >> 16) & 0xFF) << 16};
        interpolate(in, sh, lm);
        push_color();
    }
    void dpct(int sh, bool lm) {
        for (int k = 0; k < 3; ++k) dpcs(d_[20], sh, lm);
    }

    /// DCPL: [R*IR1, G*IR2, B*IR3] SHL 4, depth-cue towards FC.
    void dcpl(int sh, bool lm) { color_depth(sh, lm); }

    /// INTPL: [IR1..3] SHL 12, depth-cue towards FC.
    void intpl(int sh, bool lm) {
        const int64_t in[3] = {static_cast<int64_t>(ir(1)) << 12, static_cast<int64_t>(ir(2)) << 12,
                               static_cast<int64_t>(ir(3)) << 12};
        interpolate(in, sh, lm);
        push_color();
    }

    /// GPF: [MAC,IR] = (IR*IR0) SAR sf.
    void gpf(int sh, bool lm) {
        const int64_t i0 = ir(0);
        for (int i = 1; i <= 3; ++i) set_mac_ir(i, i0 * ir(i), sh, lm);
        push_color();
    }

    /// GPL: [MAC,IR] = ((MAC SHL sf) + IR*IR0) SAR sf.
    void gpl(int sh, bool lm) {
        const int64_t i0 = ir(0);
        for (int i = 1; i <= 3; ++i)
            set_mac_ir(i, (static_cast<int64_t>(mac(i)) << sh) + i0 * ir(i), sh, lm);
        push_color();
    }

private:
    // ---- register accessors ----
    int32_t ir(int i) const { return sext16(d_[8 + i]); }
    int32_t mac(int i) const { return static_cast<int32_t>(d_[24 + i]); }
    int64_t sz(int i) const { return d_[16 + i] & 0xFFFF; }
    int32_t ctrl32(int r) const { return static_cast<int32_t>(c_[r]); }
    Vec3 ir_vec() const { return {ir(1), ir(2), ir(3)}; }
    Vec3 vertex(int v) const {
        const uint32_t xy = d_[v * 2], z = d_[v * 2 + 1];
        return {sext16(xy), hi16(xy), sext16(z)};
    }
    Vec3 translation(int r) const { return {ctrl32(r), ctrl32(r + 1), ctrl32(r + 2)}; }
    /// 3x3 matrix packed as five 16-bit pairs starting at control register `b` (RT=0, LLM=8, LCM=16).
    Mat3 matrix(int b) const {
        const uint32_t* p = c_ + b;
        return {{{sext16(p[0]), hi16(p[0]), sext16(p[1])},
                 {hi16(p[1]), sext16(p[2]), hi16(p[2])},
                 {sext16(p[3]), hi16(p[3]), sext16(p[4])}}};
    }

    // ---- saturation helpers (psx-spx "GTE Saturation") ----
    int64_t sat_flag(int64_t v, int64_t lo, int64_t hi, uint32_t bit) {
        if (v < lo) { flag_ |= bit; return lo; }
        if (v > hi) { flag_ |= bit; return hi; }
        return v;
    }
    /// 44-bit accumulator step for MAC1-3: flags overflow, wraps to 44 bits.
    int64_t acc(int i, int64_t v) {
        if (v > kMac44Max) flag_ |= kFlagMacPos[i];
        else if (v < kMac44Min) flag_ |= kFlagMacNeg[i];
        return static_cast<int64_t>(static_cast<uint64_t>(v) << 20) >> 20;
    }
    void check_mac0(int64_t v) {
        if (v > INT32_MAX) flag_ |= kFlagMacPos[0];
        else if (v < INT32_MIN) flag_ |= kFlagMacNeg[0];
    }
    void set_ir(int i, int32_t v, bool lm) {
        d_[8 + i] = static_cast<uint32_t>(sat_flag(v, lm ? 0 : -0x8000, 0x7FFF, kFlagIr[i]));
    }
    void set_ir0(int64_t v) { d_[8] = static_cast<uint32_t>(sat_flag(v, 0, 0x1000, kFlagIr[0])); }
    /// MACi = v SAR sh (after the 44-bit check), IRi = saturate(MACi).
    void set_mac_ir(int i, int64_t v, int sh, bool lm) {
        const int32_t m = static_cast<int32_t>(acc(i, v) >> sh);
        d_[24 + i] = static_cast<uint32_t>(m);
        set_ir(i, m, lm);
    }
    /// T*1000h + row.v with a 44-bit check after every addition.
    int64_t dot(int i, const Vec3& row, const Vec3& v, int32_t t) {
        int64_t a = acc(i, (static_cast<int64_t>(t) << 12) + static_cast<int64_t>(row[0]) * v[0]);
        a = acc(i, a + static_cast<int64_t>(row[1]) * v[1]);
        return acc(i, a + static_cast<int64_t>(row[2]) * v[2]);
    }
    void mat_vec(const Mat3& m, Vec3 v, const Vec3& t, int sh, bool lm) {
        for (int i = 0; i < 3; ++i) {
            const auto k = static_cast<size_t>(i);
            set_mac_ir(i + 1, dot(i + 1, m[k], v, t[k]), sh, lm);
        }
    }

    // ---- FIFOs ----
    void push_sz(int64_t z) {
        d_[16] = d_[17];
        d_[17] = d_[18];
        d_[18] = d_[19];
        d_[19] = static_cast<uint32_t>(sat_flag(z, 0, 0xFFFF, kFlagSz));
    }
    void push_sxy(int64_t x, int64_t y) {
        const auto sx = static_cast<uint32_t>(sat_flag(x, -0x400, 0x3FF, kFlagSx));
        const auto sy = static_cast<uint32_t>(sat_flag(y, -0x400, 0x3FF, kFlagSy));
        d_[12] = d_[13];
        d_[13] = d_[14];
        d_[14] = (sx & 0xFFFF) | (sy << 16);
    }
    /// Colour FIFO <- [MAC1/16, MAC2/16, MAC3/16, CODE] saturated to 0..FFh.
    void push_color() {
        uint32_t rgb = d_[6] & 0xFF000000u;
        for (int i = 0; i < 3; ++i)
            rgb |= static_cast<uint32_t>(sat_flag(mac(i + 1) >> 4, 0, 0xFF, kFlagColor[i])) << (8 * i);
        d_[20] = d_[21];
        d_[21] = d_[22];
        d_[22] = rgb;
    }

    // ---- lighting / colour building blocks (psx-spx "GTE Color Calculation Commands") ----
    /// IR = (LLM*V) SAR sf; IR = (BK*1000h + LCM*IR) SAR sf.
    void light(int v, int sh, bool lm) {
        mat_vec(matrix(8), vertex(v), Vec3{0, 0, 0}, sh, lm);
        mat_vec(matrix(16), ir_vec(), translation(13), sh, lm);
    }
    int64_t rgbc_mul(int i) const {  // (R|G|B)*IRi SHL 4
        return (static_cast<int64_t>((d_[6] >> (8 * i)) & 0xFF) * ir(i + 1)) << 4;
    }
    /// [MAC,IR] = ([R,G,B]*IR SHL 4) SAR sf; push colour.
    void color_mul(int sh, bool lm) {
        const int64_t in[3] = {rgbc_mul(0), rgbc_mul(1), rgbc_mul(2)};
        for (int i = 0; i < 3; ++i) set_mac_ir(i + 1, in[i], sh, lm);
        push_color();
    }
    /// MAC = [R,G,B]*IR SHL 4, then MAC + (FC-MAC)*IR0; push colour.
    void color_depth(int sh, bool lm) {
        const int64_t in[3] = {rgbc_mul(0), rgbc_mul(1), rgbc_mul(2)};
        interpolate(in, sh, lm);
        push_color();
    }
    /// psx-spx "Details on MAC+(FC-MAC)*IR0":
    ///   IR = ((FC SHL 12) - MAC) SAR sf   (saturated with lm=0)
    ///   MAC = (IR*IR0 + MAC) SAR sf,  IR = MAC (lm)
    void interpolate(const int64_t (&in)[3], int sh, bool lm) {
        for (int i = 0; i < 3; ++i) set_mac_ir(i + 1, (static_cast<int64_t>(ctrl32(21 + i)) << 12) - in[i], sh, false);
        const int64_t i0 = ir(0);
        for (int i = 0; i < 3; ++i) set_mac_ir(i + 1, static_cast<int64_t>(ir(i + 1)) * i0 + in[i], sh, lm);
    }

    uint32_t* d_;
    uint32_t* c_;
    uint32_t flag_ = 0;
};

/// Unknown opcodes are logged once each and ignored.
void unknown_command(PsxContext* ctx, uint32_t command) {
    static uint64_t seen = 0;
    const uint64_t bit = uint64_t{1} << (command & 0x3F);
    if (seen & bit) return;
    seen |= bit;
    std::fprintf(stderr, "[gte] unknown command 0x%02X (0x%07X), ignored (ra=%08X)\n", command & 0x3F, command,
                 ctx->r[31]);
}

}  // namespace

extern "C" {

uint32_t psx_gte_read_data(PsxContext* ctx, uint32_t reg) {
    switch (reg) {
        // Registers that read back sign-extended 16-bit values.
        case 1: case 3: case 5: case 8: case 9: case 10: case 11:
            return static_cast<uint32_t>(sext16(ctx->gte_data[reg]));
        case 7: case 16: case 17: case 18: case 19:  // OTZ, SZ0-3: unsigned 16-bit
            return ctx->gte_data[reg] & 0xFFFF;
        case 15:  // SXYP mirrors SXY2
            return ctx->gte_data[14];
        case 28: case 29: {  // IRGB/ORGB: IR1-3 packed to 5:5:5
            auto clamp5 = [](uint32_t ir) {
                const int32_t v = sext16(ir) >> 7;
                return static_cast<uint32_t>(v < 0 ? 0 : v > 0x1F ? 0x1F : v);
            };
            return clamp5(ctx->gte_data[9]) | clamp5(ctx->gte_data[10]) << 5 | clamp5(ctx->gte_data[11]) << 10;
        }
        default:
            return ctx->gte_data[reg & 31];
    }
}

void psx_gte_write_data(PsxContext* ctx, uint32_t reg, uint32_t value) {
    switch (reg) {
        case 15:  // SXYP: push the screen-XY FIFO
            ctx->gte_data[12] = ctx->gte_data[13];
            ctx->gte_data[13] = ctx->gte_data[14];
            ctx->gte_data[14] = value;
            return;
        case 28:  // IRGB: expand 5:5:5 into IR1-3
            ctx->gte_data[9] = (value & 0x1F) << 7;
            ctx->gte_data[10] = ((value >> 5) & 0x1F) << 7;
            ctx->gte_data[11] = ((value >> 10) & 0x1F) << 7;
            return;
        case 29: case 31:  // ORGB, LZCR are read-only
            return;
        case 30: {  // LZCS: also computes leading-zero/one count into LZCR
            ctx->gte_data[30] = value;
            const uint32_t v = (value & 0x80000000u) ? ~value : value;
            ctx->gte_data[31] = static_cast<uint32_t>(std::countl_zero(v));
            return;
        }
        default:
            ctx->gte_data[reg & 31] = value;
            return;
    }
}

uint32_t psx_gte_read_ctrl(PsxContext* ctx, uint32_t reg) {
    switch (reg) {
        case 4: case 12: case 20: case 26: case 27: case 29: case 30:  // 16-bit, sign-extended
            return static_cast<uint32_t>(sext16(ctx->gte_ctrl[reg]));
        default:
            return ctx->gte_ctrl[reg & 31];
    }
}

void psx_gte_write_ctrl(PsxContext* ctx, uint32_t reg, uint32_t value) {
    if ((reg & 31) == 31) {  // FLAG: bits 0-11 read as zero, bit 31 is the computed error summary
        ctx->gte_ctrl[31] = finish_flag(value & kFlagWritable);
        return;
    }
    ctx->gte_ctrl[reg & 31] = value;
}

/// Executes one COP2 command (instruction bits 0-24). Field layout (psx-spx "GTE Command
/// Encoding"): bit19 sf, bits17-18 mx, bits15-16 v, bits13-14 cv, bit10 lm, bits0-5 opcode.
void psx_gte_command(PsxContext* ctx, uint32_t command) {
    const int sh = (command & (1u << 19)) ? 12 : 0;
    const bool lm = (command & (1u << 10)) != 0;
    switch (command & 0x3F) {
        case 0x01: { Gte g(ctx); g.rtp(0, sh, lm, true); return; }                        // RTPS
        case 0x06: { Gte g(ctx); g.nclip(); return; }                                     // NCLIP
        case 0x0C: { Gte g(ctx); g.op(sh, lm); return; }                                  // OP
        case 0x10: { Gte g(ctx); g.dpcs(ctx->gte_data[6], sh, lm); return; }              // DPCS
        case 0x11: { Gte g(ctx); g.intpl(sh, lm); return; }                               // INTPL
        case 0x12: {                                                                      // MVMVA
            Gte g(ctx);
            g.mvmva(static_cast<int>((command >> 17) & 3), static_cast<int>((command >> 15) & 3),
                    static_cast<int>((command >> 13) & 3), sh, lm);
            return;
        }
        case 0x13: { Gte g(ctx); g.ncds(0, sh, lm); return; }                             // NCDS
        case 0x14: { Gte g(ctx); g.cdp(sh, lm); return; }                                 // CDP
        case 0x16: { Gte g(ctx); for (int v = 0; v < 3; ++v) g.ncds(v, sh, lm); return; } // NCDT
        case 0x1B: { Gte g(ctx); g.nccs(0, sh, lm); return; }                             // NCCS
        case 0x1C: { Gte g(ctx); g.cc(sh, lm); return; }                                  // CC
        case 0x1E: { Gte g(ctx); g.ncs(0, sh, lm); return; }                              // NCS
        case 0x20: { Gte g(ctx); for (int v = 0; v < 3; ++v) g.ncs(v, sh, lm); return; }  // NCT
        case 0x28: { Gte g(ctx); g.sqr(sh, lm); return; }                                 // SQR
        case 0x29: { Gte g(ctx); g.dcpl(sh, lm); return; }                                // DCPL
        case 0x2A: { Gte g(ctx); g.dpct(sh, lm); return; }                                // DPCT
        case 0x2D: { Gte g(ctx); g.avsz(false); return; }                                 // AVSZ3
        case 0x2E: { Gte g(ctx); g.avsz(true); return; }                                  // AVSZ4
        case 0x30: {                                                                      // RTPT
            Gte g(ctx);
            for (int v = 0; v < 3; ++v) g.rtp(v, sh, lm, v == 2);
            return;
        }
        case 0x3D: { Gte g(ctx); g.gpf(sh, lm); return; }                                 // GPF
        case 0x3E: { Gte g(ctx); g.gpl(sh, lm); return; }                                 // GPL
        case 0x3F: { Gte g(ctx); for (int v = 0; v < 3; ++v) g.nccs(v, sh, lm); return; } // NCCT
        default: unknown_command(ctx, command); return;
    }
}

}  // extern "C"
