// GTE (COP2) tests: register quirks and every command family.
//
// Expected values are derived by hand from the psx-spx "Geometry Transformation Engine" formulas
// (the RTPS divide results were cross-checked with a short script implementing the psx-spx UNR
// algorithm; the FE3Fh/7F20h clamp case is the example psx-spx itself gives).

#include <psx/recomp.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                                  \
    do {                                                                                                \
        const uint32_t va_ = static_cast<uint32_t>(a), vb_ = static_cast<uint32_t>(b);                  \
        if (va_ != vb_) {                                                                               \
            std::fprintf(stderr, "%s:%d: %s == %s failed (0x%08X vs 0x%08X)\n", __FILE__, __LINE__, #a, \
                         #b, va_, vb_);                                                                 \
            std::exit(1);                                                                               \
        }                                                                                               \
    } while (0)

namespace {

PsxContext g_ctx;
PsxContext* const ctx = &g_ctx;

constexpr uint32_t kErr = 0x80000000u;

void reset() { std::memset(&g_ctx, 0, sizeof g_ctx); }
uint32_t rd(uint32_t r) { return psx_gte_read_data(ctx, r); }
void wd(uint32_t r, uint32_t v) { psx_gte_write_data(ctx, r, v); }
uint32_t rc(uint32_t r) { return psx_gte_read_ctrl(ctx, r); }
void wc(uint32_t r, uint32_t v) { psx_gte_write_ctrl(ctx, r, v); }
uint32_t flag() { return rc(31); }

uint32_t pair(int lo, int hi) { return (static_cast<uint32_t>(lo) & 0xFFFF) | (static_cast<uint32_t>(hi) << 16); }
uint32_t u(int v) { return static_cast<uint32_t>(v); }

/// COP2 command word: opcode | sf<<19 | mx<<17 | v<<15 | cv<<13 | lm<<10.
uint32_t cmd(uint32_t op, uint32_t sf = 1, uint32_t lm = 0, uint32_t mx = 0, uint32_t v = 0, uint32_t cv = 0) {
    return op | sf << 19 | mx << 17 | v << 15 | cv << 13 | lm << 10;
}
void run(uint32_t c) { psx_gte_command(ctx, c); }

/// Writes a 3x3 matrix at control base (RT=0, LLM=8, LCM=16).
void set_matrix(uint32_t b, const int m[3][3]) {
    wc(b + 0, pair(m[0][0], m[0][1]));
    wc(b + 1, pair(m[0][2], m[1][0]));
    wc(b + 2, pair(m[1][1], m[1][2]));
    wc(b + 3, pair(m[2][0], m[2][1]));
    wc(b + 4, u(m[2][2]));
}
constexpr int kIdentity[3][3] = {{0x1000, 0, 0}, {0, 0x1000, 0}, {0, 0, 0x1000}};
void set_ir(int a, int b, int c) { wd(9, u(a)); wd(10, u(b)); wd(11, u(c)); }
int32_t mac(int i) { return static_cast<int32_t>(rd(24 + static_cast<uint32_t>(i))); }
int32_t ir(int i) { return static_cast<int32_t>(rd(8 + static_cast<uint32_t>(i))); }

void test_registers() {
    reset();
    wd(9, 0xFFFF8000u);                 // IR1 reads sign-extended
    CHECK_EQ(rd(9), 0xFFFF8000u);
    wd(9, 0x00018000u);
    CHECK_EQ(rd(9), 0xFFFF8000u);
    wd(1, 0x0000FFFFu);                 // VZ0 sign-extended
    CHECK_EQ(rd(1), 0xFFFFFFFFu);
    wd(16, 0xABCD1234u);                // SZ0 unsigned 16-bit
    CHECK_EQ(rd(16), 0x1234u);
    wd(7, 0xFFFFFFFFu);                 // OTZ
    CHECK_EQ(rd(7), 0xFFFFu);

    wd(12, 1); wd(13, 2); wd(14, 3);    // SXYP write pushes the FIFO
    wd(15, 4);
    CHECK_EQ(rd(12), 2u); CHECK_EQ(rd(13), 3u); CHECK_EQ(rd(14), 4u); CHECK_EQ(rd(15), 4u);

    wd(28, 0x7FFFu & (31u | 10u << 5 | 1u << 10));  // IRGB expands to IR1-3
    CHECK_EQ(rd(9), 31u << 7); CHECK_EQ(rd(10), 10u << 7); CHECK_EQ(rd(11), 1u << 7);
    set_ir(-5, 0x7FFF, 0x100);          // ORGB saturates each IR/80h to 0..1Fh
    CHECK_EQ(rd(29), 0u | 31u << 5 | 2u << 10);

    wd(30, 0); CHECK_EQ(rd(31), 32u);    // LZCS/LZCR: leading zeros / ones
    wd(30, 0xFFFFFFFFu); CHECK_EQ(rd(31), 32u);
    wd(30, 0x00010000u); CHECK_EQ(rd(31), 15u);
    wd(30, 0xFFFF0000u); CHECK_EQ(rd(31), 16u);
    wd(31, 7); CHECK_EQ(rd(31), 16u);    // LZCR read-only

    wc(26, 0x8000);                     // H reads sign-extended (hardware bug) ...
    CHECK_EQ(rc(26), 0xFFFF8000u);
    wc(27, 0xFFFF);                     // DQA
    CHECK_EQ(rc(27), 0xFFFFFFFFu);
    wc(28, 0x12345678u);                // DQB 32-bit
    CHECK_EQ(rc(28), 0x12345678u);

    wc(31, 0xFFFFFFFFu);                // FLAG: bits 0-11 zero, bit 31 computed
    CHECK_EQ(flag(), 0xFFFFF000u);
    wc(31, 1u << 22);                   // IR3 flag is not part of the error summary
    CHECK_EQ(flag(), 1u << 22);
    wc(31, 1u << 13);
    CHECK_EQ(flag(), kErr | 1u << 13);
}

/// Common RTP setup: identity RT, zero TR, OFX=160.0, OFY=120.0, H=200, DQA=100h, DQB=100000h.
void rtp_setup() {
    reset();
    set_matrix(0, kIdentity);
    wc(24, 160u << 16);
    wc(25, 120u << 16);
    wc(26, 200);
    wc(27, 0x100);
    wc(28, 0x100000);
}

void test_rtps() {
    rtp_setup();
    wd(0, pair(100, 50));
    wd(1, 1000);
    wc(31, 0xFFFFFFFFu);  // commands reset FLAG
    run(cmd(0x01));
    // MAC/IR = V (identity, sf=1); SZ3 = 1000.
    CHECK_EQ(mac(1), 100); CHECK_EQ(mac(2), 50); CHECK_EQ(mac(3), 1000);
    CHECK_EQ(ir(1), 100); CHECK_EQ(ir(2), 50); CHECK_EQ(ir(3), 1000);
    CHECK_EQ(rd(19), 1000u);
    // UNR(200/1000) = 13107 (ideal 13107.2). SX = (13107*100 + 160<<16)>>16 = 179 (179.99..),
    // SY = (13107*50 + 120<<16)>>16 = 129.
    CHECK_EQ(rd(14), pair(179, 129));
    // MAC0 = DQB + DQA*n = 100000h + 256*13107; IR0 = MAC0>>12 = 1075.
    CHECK_EQ(mac(0), 0x100000 + 256 * 13107);
    CHECK_EQ(ir(0), 1075);
    CHECK_EQ(flag(), 0u);

    // Divide overflow: SZ3=50, H=200 >= 2*SZ3 -> n=1FFFFh, FLAG.17.
    // SX = (1FFFFh*1000 + 160<<16)>>16 = 2160 -> 3FFh (FLAG.14); SY = 219.
    // IR0 = (100000h + 256*1FFFFh)>>12 = 8448 -> 1000h (FLAG.12, not in the summary).
    rtp_setup();
    wd(0, pair(1000, 50));
    wd(1, 50);
    run(cmd(0x01));
    CHECK_EQ(rd(14), pair(0x3FF, 219));
    CHECK_EQ(ir(0), 0x1000);
    CHECK_EQ(mac(0), 0x100000 + 256 * 0x1FFFF);
    CHECK_EQ(flag(), kErr | 1u << 17 | 1u << 14 | 1u << 12);

    // psx-spx example: FE3Fh/7F20h gives UNR 20000h, clamped to 1FFFFh (no overflow flag).
    rtp_setup();
    wc(24, 0); wc(25, 0);
    wc(26, 0xFE3F);
    wd(0, pair(0x100, 0));
    wd(1, 0x7F20);
    run(cmd(0x01));
    CHECK_EQ(rd(14), pair(0x1FF, 0));
    CHECK_EQ(flag() & (1u << 17), 0u);

    // sf=0: MAC3 = raw, SZ3 = raw>>12. IR3 saturates from MAC3 but FLAG.22 follows MAC3>>12.
    rtp_setup();
    wd(0, pair(0, 0));
    wd(1, 1000);  // raw MAC3 = 1000*1000h = 3E8000h
    run(cmd(0x01, 0));
    CHECK_EQ(mac(3), 1000 * 0x1000);
    CHECK_EQ(ir(3), 0x7FFF);
    CHECK_EQ(rd(19), 1000u);
    CHECK_EQ(flag() & (1u << 22), 0u);

    // lm=1 clamps IR1/IR2 to 0 (with flags); SZ3 < 0 saturates to 0 (FLAG.18).
    rtp_setup();
    wd(0, pair(-100, -50));
    wd(1, u(-1000));
    run(cmd(0x01, 1, 1));
    CHECK_EQ(ir(1), 0); CHECK_EQ(ir(2), 0); CHECK_EQ(ir(3), 0);
    CHECK_EQ(mac(1), -100);
    CHECK_EQ(rd(19), 0u);
    // FLAG.22 is absent: for RTP it tracks only MAC3>>12 against -8000h..7FFFh (-1000 is in range).
    CHECK_EQ(flag(), kErr | 1u << 24 | 1u << 23 | 1u << 18 | 1u << 17 | 1u << 12);
}

void test_rtpt() {
    rtp_setup();
    wd(19, 777);            // old SZ3 becomes SZ0
    wd(0, pair(0, 0));      wd(1, 500);
    wd(2, pair(100, 50));   wd(3, 1000);
    wd(4, pair(-100, -50)); wd(5, 1000);
    run(cmd(0x30));
    CHECK_EQ(rd(16), 777u); CHECK_EQ(rd(17), 500u); CHECK_EQ(rd(18), 1000u); CHECK_EQ(rd(19), 1000u);
    CHECK_EQ(rd(12), pair(160, 120));
    CHECK_EQ(rd(13), pair(179, 129));
    CHECK_EQ(rd(14), pair(140, 110));  // (-1310700 + 160<<16)>>16 = 140, (-655350 + 120<<16)>>16 = 110
    CHECK_EQ(ir(0), 1075);             // depth cue from the last vertex only
    CHECK_EQ(ir(1), -100);
    CHECK_EQ(flag(), 0u);
}

void test_nclip() {
    reset();
    wd(12, pair(0, 0)); wd(13, pair(10, 0)); wd(14, pair(0, 10));
    run(cmd(0x06));
    CHECK_EQ(mac(0), 100);
    wd(12, pair(0, 0)); wd(13, pair(0, 10)); wd(14, pair(10, 0));
    run(cmd(0x06));
    CHECK_EQ(mac(0), -100);
    wd(12, pair(-5, -5)); wd(13, pair(5, -5)); wd(14, pair(-5, 5));
    run(cmd(0x06));
    CHECK_EQ(mac(0), 100);
    // Overflow: extreme coordinates exceed 31 bits -> FLAG.16.
    wd(12, pair(-0x8000, -0x8000)); wd(13, pair(0x7FFF, -0x8000)); wd(14, pair(-0x8000, 0x7FFF));
    run(cmd(0x06));
    CHECK_EQ(flag(), kErr | 1u << 16);
}

void test_avsz() {
    reset();
    wc(29, 0x555);
    wd(17, 100); wd(18, 200); wd(19, 300);
    run(cmd(0x2D));
    CHECK_EQ(mac(0), 600 * 0x555);
    CHECK_EQ(rd(7), (600 * 0x555) >> 12);  // 199
    CHECK_EQ(flag(), 0u);

    wc(30, 0x400);
    wd(16, 100); wd(17, 200); wd(18, 300); wd(19, 400);
    run(cmd(0x2E));
    CHECK_EQ(mac(0), 1000 * 0x400);
    CHECK_EQ(rd(7), 250u);

    // Saturation: 7FFFh*(3*FFFFh) overflows MAC0 (FLAG.16) and OTZ (FLAG.18).
    wc(29, 0x7FFF);
    wd(17, 0xFFFF); wd(18, 0xFFFF); wd(19, 0xFFFF);
    run(cmd(0x2D));
    CHECK_EQ(rd(7), 0xFFFFu);
    CHECK_EQ(flag(), kErr | 1u << 18 | 1u << 16);

    wc(29, u(-1));  // negative ZSF3 -> OTZ clamps to 0
    wd(17, 1); wd(18, 1); wd(19, 0x1000);
    run(cmd(0x2D));
    CHECK_EQ(rd(7), 0u);
    CHECK_EQ(mac(0), -0x1002);
    CHECK_EQ(flag(), kErr | 1u << 18);
}

void test_mvmva() {
    // RT * V1 + BK, sf=1: RT = diag(1.0, 0.5, -1.0), BK = (10,20,30), V1 = (100,200,300).
    reset();
    const int rt[3][3] = {{0x1000, 0, 0}, {0, 0x800, 0}, {0, 0, -0x1000}};
    set_matrix(0, rt);
    wc(13, 10); wc(14, 20); wc(15, 30);
    wd(2, pair(100, 200)); wd(3, 300);
    run(cmd(0x12, 1, 0, 0, 1, 1));
    CHECK_EQ(mac(1), 110); CHECK_EQ(mac(2), 120); CHECK_EQ(mac(3), -270);
    CHECK_EQ(ir(3), -270);
    CHECK_EQ(flag(), 0u);
    run(cmd(0x12, 1, 1, 0, 1, 1));  // lm=1: IR3 -> 0, FLAG.22 only (no summary bit)
    CHECK_EQ(ir(3), 0);
    CHECK_EQ(mac(3), -270);
    CHECK_EQ(flag(), 1u << 22);

    // LLM * IR, no translation, sf=0.
    reset();
    set_matrix(8, kIdentity);
    set_ir(1, 2, 3);
    wc(5, 999);  // TR ignored with cv=3
    run(cmd(0x12, 0, 0, 1, 3, 3));
    CHECK_EQ(mac(1), 0x1000); CHECK_EQ(mac(2), 0x2000); CHECK_EQ(mac(3), 0x3000);
    CHECK_EQ(ir(2), 0x2000);

    // LCM * V2 + TR, sf=1.
    reset();
    const int lcm[3][3] = {{0, 0x1000, 0}, {0, 0, 0x1000}, {0x1000, 0, 0}};
    set_matrix(16, lcm);
    wc(5, 1); wc(6, 2); wc(7, 3);
    wd(4, pair(7, 8)); wd(5, 9);
    run(cmd(0x12, 1, 0, 2, 2, 0));
    CHECK_EQ(mac(1), 9); CHECK_EQ(mac(2), 11); CHECK_EQ(mac(3), 10);

    // cv=2 (FC) bug: FC*1000h + M11*VX only sets flags; result = M12*VY + M13*VZ.
    reset();
    const int rt2[3][3] = {{0x1000, 0x10, 0x20}, {0, 0x1000, 0}, {0, 0, 0x1000}};
    set_matrix(0, rt2);
    wc(21, 0x10000); wc(22, 5); wc(23, 5);
    wd(0, pair(1, 2)); wd(1, 3);
    run(cmd(0x12, 0, 0, 0, 0, 2));
    CHECK_EQ(mac(1), 0x10 * 2 + 0x20 * 3);
    CHECK_EQ(ir(1), 0x80);
    CHECK_EQ(mac(2), 0x2000); CHECK_EQ(mac(3), 0x3000);
    CHECK_EQ(flag(), kErr | 1u << 24);  // from the discarded FC*1000h + M11*VX = 10001000h

    // mx=3 garbage matrix: [-R*16, R*16, IR0], [RT13 x3], [RT22 x3].
    reset();
    const int rt3[3][3] = {{0, 0, 5}, {0, 7, 0}, {0, 0, 0}};
    set_matrix(0, rt3);
    wd(6, 0x10);
    wd(8, 0x800);
    set_ir(1, 2, 3);
    run(cmd(0x12, 0, 0, 3, 3, 3));
    CHECK_EQ(mac(1), -0x100 + 0x200 + 0x800 * 3);
    CHECK_EQ(mac(2), 5 * 6);
    CHECK_EQ(mac(3), 7 * 6);

    // 44-bit MAC1 overflow: TRX*1000h = 7FFFFFFF000h, + 7FFFh*7FFFh exceeds 2^43-1 -> FLAG.30.
    reset();
    const int big[3][3] = {{0x7FFF, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    set_matrix(0, big);
    wc(5, 0x7FFFFFFF);
    wd(0, pair(0x7FFF, 0));
    run(cmd(0x12, 1, 0, 0, 0, 0));
    CHECK((flag() & (kErr | 1u << 30)) == (kErr | 1u << 30));
    // Negative: TRX = -80000000h, + (-8000h)*7FFFh -> FLAG.27.
    wc(5, 0x80000000u);
    const int neg[3][3] = {{-0x8000, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    set_matrix(0, neg);
    run(cmd(0x12, 1, 0, 0, 0, 0));
    CHECK((flag() & (kErr | 1u << 27)) == (kErr | 1u << 27));
}

void test_sqr_op() {
    reset();
    set_ir(0x100, -0x200, 0x7FFF);
    run(cmd(0x28, 0));
    CHECK_EQ(mac(1), 0x10000); CHECK_EQ(mac(2), 0x40000);
    CHECK_EQ(ir(1), 0x7FFF); CHECK_EQ(ir(2), 0x7FFF); CHECK_EQ(ir(3), 0x7FFF);
    CHECK_EQ(flag(), kErr | 1u << 24 | 1u << 23 | 1u << 22);

    set_ir(0x100, -0x200, 0x7FFF);
    run(cmd(0x28, 1));
    CHECK_EQ(mac(1), 0x10); CHECK_EQ(mac(2), 0x40); CHECK_EQ(mac(3), 0x3FFF0);
    CHECK_EQ(ir(3), 0x7FFF);
    CHECK_EQ(flag(), 1u << 22);  // IR3 saturation alone does not set bit 31

    // OP with D = RT diagonal = (1000h,1000h,1000h), IR = (100h,200h,300h), sf=1.
    reset();
    set_matrix(0, kIdentity);
    set_ir(0x100, 0x200, 0x300);
    run(cmd(0x0C, 1, 0));
    CHECK_EQ(mac(1), 0x100); CHECK_EQ(mac(2), -0x200); CHECK_EQ(mac(3), 0x100);
    CHECK_EQ(ir(2), -0x200);
    CHECK_EQ(flag(), 0u);
    set_ir(0x100, 0x200, 0x300);
    run(cmd(0x0C, 1, 1));  // lm=1 clamps IR2 to 0
    CHECK_EQ(ir(2), 0); CHECK_EQ(mac(2), -0x200);
    CHECK_EQ(flag(), kErr | 1u << 23);
}

/// Lighting setup: identity LLM and LCM, BK = 0.
void light_setup() {
    reset();
    set_matrix(8, kIdentity);
    set_matrix(16, kIdentity);
}

void test_color() {
    // CC: IR = LCM*IR = (1000h,800h,400h); MAC = RGB*IR SHL 4 SAR 12 = (800h,400h,200h).
    light_setup();
    wd(6, 0x2C808080u);
    wd(20, 0x11); wd(21, 0x22); wd(22, 0x33);
    set_ir(0x1000, 0x800, 0x400);
    run(cmd(0x1C, 1, 1));
    CHECK_EQ(mac(1), 0x800); CHECK_EQ(mac(2), 0x400); CHECK_EQ(mac(3), 0x200);
    CHECK_EQ(rd(20), 0x22u); CHECK_EQ(rd(21), 0x33u);
    CHECK_EQ(rd(22), 0x2C204080u);
    CHECK_EQ(flag(), 0u);
    // Colour saturation: 80h*7FFFh SHL 4 SAR 12 = 3FFFh -> /16 = 3FFh -> FFh (FLAG.21, no summary).
    set_ir(0x7FFF, 0, 0);
    run(cmd(0x1C, 1, 1));
    CHECK_EQ(rd(22), 0x2C0000FFu);
    CHECK_EQ(flag(), 1u << 21);

    // NCS: IR = LCM*(LLM*V0) = V0; colour = MAC/16.
    light_setup();
    wd(6, 0x30000000u);
    wd(0, pair(0x800, 0x400)); wd(1, 0x200);
    run(cmd(0x1E, 1, 1));
    CHECK_EQ(rd(22), 0x30204080u);

    // NCT: three normals -> three FIFO entries.
    wd(2, pair(0x100, 0x200)); wd(3, 0x300);
    wd(4, pair(0x10, 0x20)); wd(5, 0x30);
    run(cmd(0x20, 1, 1));
    CHECK_EQ(rd(20), 0x30204080u); CHECK_EQ(rd(21), 0x30302010u); CHECK_EQ(rd(22), 0x30030201u);

    // NCCS with BK=(10h,0,0): IR1 = 1010h, MAC1 = 80h*1010h/100h = 808h -> R=80h.
    light_setup();
    wc(13, 0x10);
    wd(6, 0x2C808080u);
    wd(0, pair(0x1000, 0x800)); wd(1, 0x400);
    run(cmd(0x1B, 1, 1));
    CHECK_EQ(mac(1), 0x808); CHECK_EQ(ir(1), 0x808);
    CHECK_EQ(rd(22), 0x2C204080u);
    run(cmd(0x3F, 1, 1));  // NCCT on V0 (V1=V2=0 here)
    CHECK_EQ(rd(20), 0x2C204080u); CHECK_EQ(rd(21), 0x2C000000u); CHECK_EQ(rd(22), 0x2C000000u);

    // NCDS: same lighting, FC=(FF0h,0,0), IR0=800h (half way).
    //   R: in=808000h, IR1=(FF0000h-808000h)>>12=7E8h, MAC1=(7E8h*800h+808000h)>>12=BFCh -> BFh
    //   G: in=400000h, IR2=-400h, MAC2=(-400h*800h+400000h)>>12=200h -> 20h
    //   B: in=200000h, IR3=-200h, MAC3=100h -> 10h
    light_setup();
    wc(13, 0x10);
    wc(21, 0xFF0);
    wd(6, 0x2C808080u);
    wd(8, 0x800);
    wd(0, pair(0x1000, 0x800)); wd(1, 0x400);
    run(cmd(0x13, 1, 1));
    CHECK_EQ(mac(1), 0xBFC); CHECK_EQ(mac(2), 0x200); CHECK_EQ(mac(3), 0x100);
    CHECK_EQ(ir(1), 0xBFC);
    CHECK_EQ(rd(22), 0x2C1020BFu);
    CHECK_EQ(flag(), 0u);
    run(cmd(0x16, 1, 1));  // NCDT: V0 again, then V1=V2=0 -> IR=BK -> only R from 10h*80h
    CHECK_EQ(rd(20), 0x2C1020BFu);

    // CDP: CC followed by depth cue; IR=(1000h,800h,400h), R=G=B=80h, FC=(FF0h,0,0), IR0=800h.
    //   R: in=800000h -> IR1=7F0h, MAC1=(7F0h*800h+800000h)>>12 = BF8h -> BFh; G=20h, B=10h.
    light_setup();
    wc(21, 0xFF0);
    wd(6, 0x2C808080u);
    wd(8, 0x800);
    set_ir(0x1000, 0x800, 0x400);
    run(cmd(0x14, 1, 1));
    CHECK_EQ(mac(1), 0xBF8);
    CHECK_EQ(rd(22), 0x2C1020BFu);

    // DCPL: same without the light matrix step.
    set_ir(0x1000, 0x800, 0x400);
    run(cmd(0x29, 1, 1));
    CHECK_EQ(mac(1), 0xBF8);
    CHECK_EQ(rd(22), 0x2C1020BFu);

    // DPCS: RGBC=(80h,40h,0), FC=(FF0h,FF0h,FF0h), IR0=800h -> (BFh,9Fh,7Fh).
    reset();
    wc(21, 0xFF0); wc(22, 0xFF0); wc(23, 0xFF0);
    wd(6, 0x07004080u);
    wd(8, 0x800);
    run(cmd(0x10, 1, 0));
    CHECK_EQ(mac(1), 0xBF8); CHECK_EQ(mac(2), 0x9F8); CHECK_EQ(mac(3), 0x7F8);
    CHECK_EQ(rd(22), 0x077F9FBFu);

    // DPCT with IR0=0 passes RGB0..2 through unchanged (CODE from RGBC).
    wd(8, 0);
    wd(20, 0xAA010203u); wd(21, 0xBB040506u); wd(22, 0xCC070809u);
    run(cmd(0x2A, 1, 0));
    CHECK_EQ(rd(20), 0x07010203u); CHECK_EQ(rd(21), 0x07040506u); CHECK_EQ(rd(22), 0x07070809u);

    // INTPL with IR0=1000h lands exactly on FC.
    reset();
    wc(21, 0x800); wc(22, 0x400); wc(23, 0x200);
    wd(8, 0x1000);
    set_ir(0x100, 0x200, 0x300);
    run(cmd(0x11, 1, 0));
    CHECK_EQ(mac(1), 0x800); CHECK_EQ(mac(2), 0x400); CHECK_EQ(mac(3), 0x200);
    CHECK_EQ(rd(22), 0x00204080u);

    // GPF: MAC = IR*IR0 SAR 12 -> (800h, 1000h, -80h); colours 80h, FFh (FLAG.20), 0 (FLAG.19).
    reset();
    wd(8, 0x800);
    set_ir(0x1000, 0x2000, -0x100);
    run(cmd(0x3D, 1, 0));
    CHECK_EQ(mac(1), 0x800); CHECK_EQ(mac(2), 0x1000); CHECK_EQ(mac(3), -0x80);
    CHECK_EQ(ir(3), -0x80);
    CHECK_EQ(rd(22), 0x0000FF80u);
    CHECK_EQ(flag(), 1u << 20 | 1u << 19);

    // GPL: MAC = ((MAC SHL 12) + IR*IR0) SAR 12.
    reset();
    wd(25, 0x100); wd(26, u(-0x10)); wd(27, 0);
    wd(8, 0x1000);
    set_ir(0x10, 0x10, 0x10);
    run(cmd(0x3E, 1, 0));
    CHECK_EQ(mac(1), 0x110); CHECK_EQ(mac(2), 0); CHECK_EQ(mac(3), 0x10);
}

void test_unknown() {
    reset();
    wc(31, 1u << 13);
    run(0x00);  // not a GTE command: ignored, no abort, FLAG untouched
    run(0x3C);
    CHECK_EQ(flag(), kErr | 1u << 13);
}

}  // namespace

int main() {
    test_registers();
    test_rtps();
    test_rtpt();
    test_nclip();
    test_avsz();
    test_mvmva();
    test_sqr_op();
    test_color();
    test_unknown();
    std::puts("gte.commands: ok");
    return 0;
}
