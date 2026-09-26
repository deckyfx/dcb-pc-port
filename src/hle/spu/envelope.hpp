#pragma once
// Volume envelope stepping shared by the ADSR generator and volume sweep, run once per 44.1 kHz
// sample. Reference: psx-spx "SPU Volume and ADSR Generator" ("Envelope Operation depending on
// Shift/Step/Mode/Direction").

#include <algorithm>
#include <cstdint>

namespace hle::spu {

struct Envelope {
    int32_t counter = 0;
    int32_t increment = 0x8000;  ///< added to `counter` per sample; a step happens on bit 15
    int32_t step = 0;            ///< level delta per step (before exponential scaling)
    uint8_t rate = 0;            ///< shift << 2 | step
    bool decreasing = false, exponential = false, invert = false;

    /// Start a phase. `rate_mask` marks the bits of `rate` that are programmable: a rate with all
    /// of them set never steps (0x7F for attack/sustain/sweep, 0x7C for decay/release).
    void reset(uint8_t rate_, uint8_t rate_mask, bool decreasing_, bool exponential_, bool invert_) {
        rate = rate_;
        decreasing = decreasing_;
        exponential = exponential_;
        invert = invert_;
        counter = 0;
        increment = 0x8000;
        const int32_t shift = rate >> 2;
        const int32_t base = 7 - (rate & 3);  // +7,+6,+5,+4
        // Exponential decrease scales by the (signed) level, so it is always a negative step.
        step = ((decreasing != invert) || (decreasing && exponential)) ? ~base : base;
        if (shift < 11) step *= 1 << (11 - shift);
        else increment >>= std::min(shift - 11, 16);
        if ((rate & rate_mask) != rate_mask) increment = std::max(increment, 1);
    }

    /// Advance one sample and return the new level.
    int16_t tick(int16_t level) {
        int32_t this_step = step;
        int32_t this_increment = increment;
        if (exponential) {
            if (decreasing) {
                this_step = (this_step * level) >> 15;
            } else if (level > 0x6000) {  // "exponential" increase: slower linear above 6000h
                if (rate < 40) {
                    this_step >>= 2;
                } else if (rate >= 44) {
                    this_increment >>= 2;
                } else {
                    this_step >>= 1;
                    this_increment >>= 1;
                }
            }
        }
        counter += this_increment;
        if (!(counter & 0x8000)) return level;
        counter = 0;
        int32_t next = level + this_step;
        if (!decreasing) next = std::clamp(next, -0x8000, 0x7FFF);
        else if (invert) next = std::clamp(next, -0x8000, 0);
        else next = std::max(next, 0);
        return static_cast<int16_t>(next);
    }
};

/// A voice/main volume register pair: direct level or sweep (psx-spx "VOLL/VOLR", "MVOLL/MVOLR").
struct Sweep {
    Envelope env;
    bool sweeping = false;
    int16_t level = 0;  ///< current volume (-8000h..7FFFh), readable as VOLX/MVOLX

    void set(uint16_t reg) {
        if (reg & 0x8000) {
            sweeping = true;
            env.reset(static_cast<uint8_t>(reg & 0x7F), 0x7F, reg & 0x2000, reg & 0x4000, reg & 0x1000);
        } else {
            sweeping = false;
            level = static_cast<int16_t>(static_cast<uint16_t>(reg << 1));  // 15-bit volume/2
        }
    }
    void tick() {
        if (sweeping) level = env.tick(level);
    }
};

}  // namespace hle::spu
