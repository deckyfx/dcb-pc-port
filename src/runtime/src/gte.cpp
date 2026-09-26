// GTE (COP2) register file. Commands are not implemented yet: the first one the game issues
// stops with its opcode so it can be implemented next (reference: psx-spx "GTE").

#include <psx/runtime.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int32_t sext16(uint32_t v) { return static_cast<int16_t>(v & 0xFFFF); }

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
            uint32_t v = (value & 0x80000000u) ? ~value : value;
            uint32_t n = 0;
            while (n < 32 && !(v & 0x80000000u)) { v <<= 1; ++n; }
            ctx->gte_data[31] = n;
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

void psx_gte_write_ctrl(PsxContext* ctx, uint32_t reg, uint32_t value) { ctx->gte_ctrl[reg & 31] = value; }

void psx_gte_command(PsxContext* ctx, uint32_t command) {
    std::fprintf(stderr, "[gte] command 0x%02X (0x%07X) not implemented (ra=%08X)\n", command & 0x3F, command,
                 ctx->r[31]);
    std::abort();
}

}  // extern "C"
