#include "spu/spu.hpp"

#include "spu/adpcm.hpp"

#include <algorithm>

namespace hle {

namespace {

// Voice registers (offsets inside each 16-byte voice block), psx-spx "SPU I/O Port Summary".
constexpr uint32_t kVolL = 0x0, kVolR = 0x2, kPitch = 0x4, kStart = 0x6, kAdsr1 = 0x8, kAdsr2 = 0xA,
                   kEnvx = 0xC, kRepeat = 0xE;

constexpr uint32_t kVoiceEnd = 0x1F801D80;
constexpr uint32_t kMainVolL = 0x1F801D80, kMainVolR = 0x1F801D82;
constexpr uint32_t kReverbVolL = 0x1F801D84, kReverbVolR = 0x1F801D86;
constexpr uint32_t kKon0 = 0x1F801D88, kKon1 = 0x1F801D8A, kKoff0 = 0x1F801D8C, kKoff1 = 0x1F801D8E;
constexpr uint32_t kPmon0 = 0x1F801D90, kNon0 = 0x1F801D94, kEon0 = 0x1F801D98;
constexpr uint32_t kEndx0 = 0x1F801D9C, kEndx1 = 0x1F801D9E;
constexpr uint32_t kReverbBase = 0x1F801DA2;  // ESA, in 8-byte units
constexpr uint32_t kIrqAddr = 0x1F801DA4;     // in 8-byte units
constexpr uint32_t kTransferAddr = 0x1F801DA6;  // in 8-byte units
constexpr uint32_t kTransferFifo = 0x1F801DA8;
constexpr uint32_t kSpuCnt = 0x1F801DAA;
constexpr uint32_t kRamCtrl = 0x1F801DAC;
constexpr uint32_t kSpuStat = 0x1F801DAE;
constexpr uint32_t kCdVolL = 0x1F801DB0, kCdVolR = 0x1F801DB2;
constexpr uint32_t kMainVolXL = 0x1F801DB8, kMainVolXR = 0x1F801DBA;
constexpr uint32_t kVoiceVolX = 0x1F801E00, kVoiceVolXEnd = 0x1F801E60;

// SPUCNT bits (psx-spx "SPU Control and Status Register").
constexpr uint16_t kCntEnable = 0x8000, kCntUnmute = 0x4000, kCntReverb = 0x0080, kCntIrq = 0x0040,
                   kCntCdReverb = 0x0004, kCntCdEnable = 0x0001;

constexpr uint32_t kRamMask = Spu::kRamSize - 1;

int32_t clamp16(int32_t v) { return std::clamp(v, -0x8000, 0x7FFF); }

}  // namespace

Spu::Spu() : ram_(kRamSize, 0), cd_buf_(kCdQueueFrames * 2, 0) {}

uint16_t Spu::read16(uint32_t phys) const {
    if (phys < kVoiceEnd && (phys & 0xF) == kEnvx) {
        return static_cast<uint16_t>(voices_[(phys - kBase) >> 4].adsr);
    }
    if (phys >= kVoiceVolX && phys < kVoiceVolXEnd) {
        const Voice& vc = voices_[(phys - kVoiceVolX) >> 2];
        return static_cast<uint16_t>((phys & 2) ? vc.vol_r.level : vc.vol_l.level);
    }
    switch (phys) {
        case kSpuStat: {
            // Bits 0-5 mirror SPUCNT (libspu waits for this), bit 7 mirrors the DMA request mode,
            // bit 6 is the IRQ9 flag, bit 11 the capture buffer half. Transfers finish instantly,
            // so the busy flag (bit 10) is never set.
            const uint16_t cnt = reg(kSpuCnt);
            return static_cast<uint16_t>((cnt & 0x3Fu) | ((cnt & 0x20u) << 2) | (irq_flag_ ? 0x40u : 0u) |
                                         (capture_pos_ >= 0x100 ? 0x800u : 0u));
        }
        case kEndx0: return static_cast<uint16_t>(endx_);
        case kEndx1: return static_cast<uint16_t>(endx_ >> 16);
        case kMainVolXL: return static_cast<uint16_t>(main_l_.level);
        case kMainVolXR: return static_cast<uint16_t>(main_r_.level);
        default: return reg(phys);
    }
}

void Spu::write16(uint32_t phys, uint16_t value) {
    if (phys == kSpuStat || phys == kEndx0 || phys == kEndx1) return;  // read-only
    reg(phys) = value;

    if (phys < kVoiceEnd) {
        const int v = static_cast<int>((phys - kBase) >> 4);
        Voice& vc = voices_[static_cast<size_t>(v)];
        switch (phys & 0xF) {
            case kVolL: vc.vol_l.set(value); break;
            case kVolR: vc.vol_r.set(value); break;
            case kAdsr1: case kAdsr2:
                if (vc.phase != Phase::Off) update_adsr(vc, v);
                break;
            case kEnvx: vc.adsr = static_cast<int16_t>(value); break;
            case kRepeat:
                if (vc.phase != Phase::Off) vc.ignore_loop = true;
                break;
            default: break;
        }
        return;
    }

    switch (phys) {
        case kMainVolL: main_l_.set(value); break;
        case kMainVolR: main_r_.set(value); break;
        // Key on/off take effect at the next sample; ENDX clears right away so a poll straight
        // after KON doesn't see the previous run's end flag.
        case kKon0: pending_kon_ |= value; endx_ &= ~static_cast<uint32_t>(value); break;
        case kKon1:
            pending_kon_ |= static_cast<uint32_t>(value & 0xFF) << 16;
            endx_ &= ~(static_cast<uint32_t>(value & 0xFF) << 16);
            break;
        case kKoff0: pending_koff_ |= value; break;
        case kKoff1: pending_koff_ |= static_cast<uint32_t>(value & 0xFF) << 16; break;
        case kReverbBase: reverb_cur_ = static_cast<uint32_t>(value) * 4; break;
        case kTransferAddr: transfer_addr_ = (static_cast<uint32_t>(value) * 8u) % kRamSize; break;
        case kTransferFifo:  // manual (non-DMA) upload
            check_irq(transfer_addr_);
            ram_[transfer_addr_] = static_cast<uint8_t>(value);
            ram_[(transfer_addr_ + 1) % kRamSize] = static_cast<uint8_t>(value >> 8);
            transfer_addr_ = (transfer_addr_ + 2) % kRamSize;
            break;
        case kSpuCnt:
            if (!(value & kCntIrq) || !(value & kCntEnable)) irq_flag_ = false;  // acknowledge
            break;
        default: break;
    }
}

void Spu::dma_write(const uint32_t* words, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        check_irq(transfer_addr_);
        for (int b = 0; b < 4; ++b) {
            ram_[transfer_addr_] = static_cast<uint8_t>(words[i] >> (8 * b));
            transfer_addr_ = (transfer_addr_ + 1) % kRamSize;
        }
    }
}

void Spu::dma_read(uint32_t* words, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        check_irq(transfer_addr_);
        uint32_t w = 0;
        for (int b = 0; b < 4; ++b) {
            w |= static_cast<uint32_t>(ram_[transfer_addr_]) << (8 * b);
            transfer_addr_ = (transfer_addr_ + 1) % kRamSize;
        }
        words[i] = w;
    }
}

bool Spu::take_irq() {
    const bool hit = irq_pending_;
    irq_pending_ = false;
    return hit;
}

bool Spu::irq_enabled() const {
    const uint16_t cnt = reg(kSpuCnt);
    return (cnt & kCntEnable) && (cnt & kCntIrq);
}

// psx-spx "SPU Interrupt": a voice fetch, transfer, capture or reverb access at IRQA raises IRQ9.
// Addresses are compared at 8-byte granularity (the register's unit).
void Spu::check_irq(uint32_t byte_addr) {
    if (irq_flag_ || !irq_enabled()) return;
    if ((byte_addr & kRamMask & ~7u) == static_cast<uint32_t>(reg(kIrqAddr)) * 8) {
        irq_flag_ = true;
        irq_pending_ = true;
    }
}

void Spu::push_cd_audio(const int16_t* stereo, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        if (cd_count_ == kCdQueueFrames) {  // full: drop the oldest frame
            cd_head_ = (cd_head_ + 1) % kCdQueueFrames;
            --cd_count_;
        }
        const size_t tail = (cd_head_ + cd_count_) % kCdQueueFrames;
        cd_buf_[tail * 2] = stereo[i * 2];
        cd_buf_[tail * 2 + 1] = stereo[i * 2 + 1];
        ++cd_count_;
    }
}

// --- Voices ------------------------------------------------------------------------------------

// psx-spx "SPU Voice Flags" (KON): restart at the start address, ADSR from zero in Attack.
void Spu::key_on(int v) {
    Voice& vc = voices_[static_cast<size_t>(v)];
    vc.addr = (static_cast<uint32_t>(voice_reg(v, kStart)) * 8) & kRamMask;
    vc.counter = 0;
    vc.samples.fill(0);
    vc.has_block = false;
    vc.ignore_loop = false;
    vc.adsr = 0;
    vc.phase = Phase::Attack;
    update_adsr(vc, v);
    endx_ &= ~(1u << v);
}

void Spu::key_off(int v) {
    Voice& vc = voices_[static_cast<size_t>(v)];
    if (vc.phase == Phase::Off || vc.phase == Phase::Release) return;
    vc.phase = Phase::Release;
    update_adsr(vc, v);
}

// psx-spx "ADSR1/ADSR2": configure the envelope for the voice's current phase.
void Spu::update_adsr(Voice& vc, int v) {
    const uint16_t a1 = voice_reg(v, kAdsr1), a2 = voice_reg(v, kAdsr2);
    switch (vc.phase) {
        case Phase::Attack:
            vc.adsr_env.reset(static_cast<uint8_t>((a1 >> 8) & 0x7F), 0x7F, false, a1 & 0x8000, false);
            vc.adsr_target = 0x7FFF;
            break;
        case Phase::Decay:
            vc.adsr_env.reset(static_cast<uint8_t>(((a1 >> 4) & 0x0F) << 2), 0x7C, true, true, false);
            vc.adsr_target = std::min((static_cast<int32_t>(a1 & 0x0F) + 1) * 0x800, 0x7FFF);
            break;
        case Phase::Sustain:
            vc.adsr_env.reset(static_cast<uint8_t>((a2 >> 6) & 0x7F), 0x7F, a2 & 0x4000, a2 & 0x8000, false);
            vc.adsr_target = 0;
            break;
        case Phase::Release:
            vc.adsr_env.reset(static_cast<uint8_t>((a2 & 0x1F) << 2), 0x7C, true, a2 & 0x20, false);
            vc.adsr_target = 0;
            break;
        case Phase::Off: break;
    }
}

// Fetch and decode the block at the voice's current address (psx-spx "SPU ADPCM Samples").
void Spu::decode_block(Voice& vc, int v) {
    std::array<uint8_t, adpcm::kBlockBytes> block;
    for (uint32_t i = 0; i < adpcm::kBlockBytes; ++i) block[i] = ram_[(vc.addr + i) & kRamMask];
    check_irq(vc.addr);
    check_irq(vc.addr + 8);
    int16_t old = vc.samples[2], older = vc.samples[1];
    adpcm::decode_spu_block(block.data(), &vc.samples[3], old, older);
    vc.flags = block[1];
    if ((vc.flags & adpcm::kFlagLoopStart) && !vc.ignore_loop) {
        reg(kBase + static_cast<uint32_t>(v) * 0x10 + kRepeat) = static_cast<uint16_t>(vc.addr / 8);
    }
    vc.has_block = true;
}

// One 44.1 kHz step of voice `v`: returns VxOUTX (the sample after ADSR, before L/R volume).
// psx-spx "SPU ADPCM Pitch" (pitch counter, PMON) and "4-Point Gaussian Interpolation".
int32_t Spu::sample_voice(int v) {
    Voice& vc = voices_[static_cast<size_t>(v)];
    if (!vc.has_block) decode_block(vc, v);

    int32_t sample;
    const uint32_t bit = 1u << v;
    const uint32_t noise_mask = reg(kNon0) | static_cast<uint32_t>(reg(kNon0 + 2)) << 16;
    const bool noise = noise_mask & bit;
    if (noise) {
        sample = static_cast<int16_t>(noise_level_);
    } else {
        const uint32_t s = vc.counter >> 12;  // 0..27
        sample = adpcm::interpolate(vc.samples[s], vc.samples[s + 1], vc.samples[s + 2], vc.samples[s + 3],
                                    (vc.counter >> 4) & 0xFF);
    }
    const int32_t out = (sample * vc.adsr) >> 15;
    vc.out = static_cast<int16_t>(out);

    // Envelope.
    if (vc.phase != Phase::Off) {
        vc.adsr = vc.adsr_env.tick(vc.adsr);
        bool reached = false;
        switch (vc.phase) {
            case Phase::Attack: reached = vc.adsr >= vc.adsr_target; break;
            case Phase::Decay: reached = vc.adsr <= vc.adsr_target; break;
            case Phase::Release: reached = vc.adsr <= 0; break;
            default: break;
        }
        if (reached) {
            vc.phase = vc.phase == Phase::Attack  ? Phase::Decay
                       : vc.phase == Phase::Decay ? Phase::Sustain
                                                  : Phase::Off;
            if (vc.phase == Phase::Off) vc.adsr = 0;
            else update_adsr(vc, v);
        }
    }

    // Pitch counter.
    uint32_t step = voice_reg(v, kPitch);
    const uint32_t pmon = reg(kPmon0) | static_cast<uint32_t>(reg(kPmon0 + 2)) << 16;
    if (v > 0 && (pmon & bit)) {
        const int32_t factor = voices_[static_cast<size_t>(v) - 1].out + 0x8000;
        const auto signed_step = static_cast<int32_t>(static_cast<int16_t>(step));  // hardware glitch >7FFFh
        step = static_cast<uint32_t>((signed_step * factor) >> 15) & 0xFFFF;
    }
    if (step > 0x3FFF) step = 0x4000;
    vc.counter += step;

    if ((vc.counter >> 12) >= adpcm::kBlockSamples) {
        vc.counter -= adpcm::kBlockSamples << 12;
        // Keep the last three samples as interpolation (and filter) history.
        vc.samples[0] = vc.samples[28];
        vc.samples[1] = vc.samples[29];
        vc.samples[2] = vc.samples[30];
        vc.has_block = false;
        if (vc.flags & adpcm::kFlagLoopEnd) {
            endx_ |= bit;
            vc.addr = (static_cast<uint32_t>(voice_reg(v, kRepeat)) * 8) & kRamMask;
            if (!(vc.flags & adpcm::kFlagLoopRepeat) && !noise) {  // End+Mute
                vc.phase = Phase::Off;
                vc.adsr = 0;
            }
        } else {
            vc.addr = (vc.addr + adpcm::kBlockBytes) & kRamMask;
        }
    }
    return out;
}

// psx-spx "SPU Noise Generator".
void Spu::tick_noise() {
    const uint16_t cnt = reg(kSpuCnt);
    const int32_t shift = (cnt >> 10) & 0x0F;
    const int32_t step = ((cnt >> 8) & 3) + 4;
    const uint32_t lvl = noise_level_;
    const uint32_t parity = ((lvl >> 15) ^ (lvl >> 12) ^ (lvl >> 11) ^ (lvl >> 10) ^ 1u) & 1u;
    noise_timer_ -= step;
    if (noise_timer_ < 0) {
        noise_level_ = static_cast<uint16_t>(lvl * 2 + parity);
        noise_timer_ += 0x20000 >> shift;
        if (noise_timer_ < 0) noise_timer_ += 0x20000 >> shift;
    }
}

// --- Mixer -------------------------------------------------------------------------------------

void Spu::mix(int16_t* out, size_t frames) {
    for (size_t f = 0; f < frames; ++f) {
        // Key off then key on, so an OFF+ON pair within one interval restarts the voice.
        if (pending_koff_ | pending_kon_) {
            for (int v = 0; v < kVoices; ++v) {
                if (pending_koff_ & (1u << v)) key_off(v);
                if (pending_kon_ & (1u << v)) key_on(v);
            }
            pending_koff_ = pending_kon_ = 0;
        }

        const uint16_t cnt = reg(kSpuCnt);
        const bool irq_on = irq_enabled();
        const uint32_t eon = reg(kEon0) | static_cast<uint32_t>(reg(kEon0 + 2)) << 16;
        int32_t dry_l = 0, dry_r = 0, rev_l = 0, rev_r = 0;
        for (int v = 0; v < kVoices; ++v) {
            Voice& vc = voices_[static_cast<size_t>(v)];
            // Silent voices keep fetching ADPCM on hardware; that only matters for IRQ hits.
            if (vc.phase == Phase::Off && !irq_on) {
                vc.out = 0;
                continue;
            }
            const int32_t s = sample_voice(v);
            vc.vol_l.tick();
            vc.vol_r.tick();
            const int32_t l = (s * vc.vol_l.level) >> 15;
            const int32_t r = (s * vc.vol_r.level) >> 15;
            dry_l += l;
            dry_r += r;
            if (eon & (1u << v)) {
                rev_l += l;
                rev_r += r;
            }
        }
        tick_noise();
        if (!(cnt & kCntUnmute)) dry_l = dry_r = 0;  // mute applies to voices, not CD audio

        // CD audio input (psx-spx "AVOLL/AVOLR"), and the capture buffers ("SPU Memory layout").
        int16_t cd_l = 0, cd_r = 0;
        if (cd_count_ > 0) {
            cd_l = cd_buf_[cd_head_ * 2];
            cd_r = cd_buf_[cd_head_ * 2 + 1];
            cd_head_ = (cd_head_ + 1) % kCdQueueFrames;
            --cd_count_;
        }
        const auto capture = [&](uint32_t base, int16_t value) {
            const uint32_t a = base + capture_pos_ * 2;
            ram_[a] = static_cast<uint8_t>(value);
            ram_[a + 1] = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8);
            if (reg(kRamCtrl) & 0x0C) check_irq(a);
        };
        capture(0x000, cd_l);
        capture(0x400, cd_r);
        capture(0x800, voices_[1].out);
        capture(0xC00, voices_[3].out);
        capture_pos_ = (capture_pos_ + 1) & 0x1FF;
        if (cnt & kCntCdEnable) {
            const int32_t l = (cd_l * static_cast<int16_t>(reg(kCdVolL))) >> 15;
            const int32_t r = (cd_r * static_cast<int16_t>(reg(kCdVolR))) >> 15;
            dry_l += l;
            dry_r += r;
            if (cnt & kCntCdReverb) {
                rev_l += l;
                rev_r += r;
            }
        }

        int32_t wet_l = 0, wet_r = 0;
        process_reverb(clamp16(rev_l), clamp16(rev_r), wet_l, wet_r);

        main_l_.tick();
        main_r_.tick();
        const int32_t l = clamp16(dry_l + wet_l);
        const int32_t r = clamp16(dry_r + wet_r);
        out[f * 2] = static_cast<int16_t>((l * main_l_.level) >> 15);
        out[f * 2 + 1] = static_cast<int16_t>((r * main_r_.level) >> 15);
    }
}

// --- Reverb ------------------------------------------------------------------------------------

namespace {

// psx-spx "Reverb Buffer Resampling": 39-tap half-band FIR (odd taps other than the centre are 0).
constexpr std::array<int32_t, 39> kReverbFir{
    -0x0001, 0, 0x0002, 0, -0x000A, 0, 0x0023, 0, -0x0067, 0, 0x010A, 0, -0x0268, 0,
    0x0534,  0, -0x0B90, 0, 0x2806, 0x4000, 0x2806, 0, -0x0B90, 0, 0x0534, 0, -0x0268, 0,
    0x010A,  0, -0x0067, 0, 0x0023, 0, -0x000A, 0, 0x0002, 0, -0x0001,
};

int32_t fir(const std::array<int16_t, 64>& ring, uint32_t pos) {
    int32_t sum = 0;
    for (uint32_t k = 0; k < kReverbFir.size(); ++k) sum += kReverbFir[k] * ring[(pos - k) & 63];
    return sum;
}

int32_t vmul(int32_t a, int32_t b) { return clamp16((a * b) >> 15); }

}  // namespace

uint32_t Spu::reverb_addr(int32_t rel) const {
    // Relative to the current buffer address, wrapped within ESA..7FFFEh (halfword units).
    const int32_t base = reg(kReverbBase) * 4;
    const int32_t size = static_cast<int32_t>(kRamSize / 2) - base;
    int32_t off = (static_cast<int32_t>(reverb_cur_) - base + rel) % size;
    if (off < 0) off += size;
    return static_cast<uint32_t>(base + off);
}

int16_t Spu::reverb_read(int32_t rel) {
    const uint32_t a = reverb_addr(rel) * 2;
    return static_cast<int16_t>(ram_[a] | ram_[a + 1] << 8);
}

void Spu::reverb_write(int32_t rel, int32_t value) {
    if (!(reg(kSpuCnt) & kCntReverb)) return;  // master enable gates buffer writes only
    const uint32_t a = reverb_addr(rel) * 2;
    check_irq(a);
    const auto v = static_cast<uint16_t>(clamp16(value));
    ram_[a] = static_cast<uint8_t>(v);
    ram_[a + 1] = static_cast<uint8_t>(v >> 8);
}

// psx-spx "SPU Reverb Formula", evaluated once per 22050 Hz tick with saturating intermediates.
// Left and right are computed in the same tick (hardware alternates them per 44.1 kHz cycle).
void Spu::process_reverb(int32_t in_l, int32_t in_r, int32_t& out_l, int32_t& out_r) {
    reverb_in_[0][reverb_pos_] = static_cast<int16_t>(in_l);
    reverb_in_[1][reverb_pos_] = static_cast<int16_t>(in_r);

    // Fast path: with buffer writes disabled and no output volume the unit is inaudible.
    const bool active = (reg(kSpuCnt) & kCntReverb) || reg(kReverbVolL) || reg(kReverbVolR);
    if (reverb_phase_ && active) {
        const int32_t lin = clamp16(fir(reverb_in_[0], reverb_pos_) >> 15);
        const int32_t rin = clamp16(fir(reverb_in_[1], reverb_pos_) >> 15);

        const auto r = [this](uint32_t index) { return static_cast<int16_t>(regs_[(0x1F801DC0 - kBase) / 2 + index]); };
        const auto a = [this](uint32_t index) { return static_cast<int32_t>(regs_[(0x1F801DC0 - kBase) / 2 + index]) * 4; };
        const int32_t dapf1 = a(0), dapf2 = a(1);
        const int32_t viir = r(2), vcomb1 = r(3), vcomb2 = r(4), vcomb3 = r(5), vcomb4 = r(6), vwall = r(7);
        const int32_t vapf1 = r(8), vapf2 = r(9);
        const int32_t mlsame = a(10), mrsame = a(11), mlcomb1 = a(12), mrcomb1 = a(13), mlcomb2 = a(14),
                      mrcomb2 = a(15), dlsame = a(16), drsame = a(17), mldiff = a(18), mrdiff = a(19),
                      mlcomb3 = a(20), mrcomb3 = a(21), mlcomb4 = a(22), mrcomb4 = a(23), dldiff = a(24),
                      drdiff = a(25), mlapf1 = a(26), mrapf1 = a(27), mlapf2 = a(28), mrapf2 = a(29);
        const int32_t vlin = r(30), vrin = r(31);

        const int32_t l_in = vmul(lin, vlin), r_in = vmul(rin, vrin);

        // Same side and different side reflection.
        const auto reflect = [&](int32_t in, int32_t src, int32_t dst) {
            const int32_t prev = reverb_read(dst - 1);
            const int32_t x = clamp16(in + vmul(reverb_read(src), vwall) - prev);
            reverb_write(dst, vmul(x, viir) + prev);
        };
        reflect(l_in, dlsame, mlsame);
        reflect(r_in, drsame, mrsame);
        reflect(l_in, drdiff, mldiff);
        reflect(r_in, dldiff, mrdiff);

        // Early echo (comb).
        int32_t lout = clamp16(vmul(vcomb1, reverb_read(mlcomb1)) + vmul(vcomb2, reverb_read(mlcomb2)) +
                               vmul(vcomb3, reverb_read(mlcomb3)) + vmul(vcomb4, reverb_read(mlcomb4)));
        int32_t rout = clamp16(vmul(vcomb1, reverb_read(mrcomb1)) + vmul(vcomb2, reverb_read(mrcomb2)) +
                               vmul(vcomb3, reverb_read(mrcomb3)) + vmul(vcomb4, reverb_read(mrcomb4)));

        // Late reverb: two all-pass filters.
        const auto apf = [&](int32_t x, int32_t m, int32_t d, int32_t vol) {
            const int32_t delayed = reverb_read(m - d);
            x = clamp16(x - vmul(vol, delayed));
            reverb_write(m, x);
            return clamp16(vmul(x, vol) + delayed);
        };
        lout = apf(lout, mlapf1, dapf1, vapf1);
        rout = apf(rout, mrapf1, dapf1, vapf1);
        lout = apf(lout, mlapf2, dapf2, vapf2);
        rout = apf(rout, mrapf2, dapf2, vapf2);

        reverb_out_[0][reverb_pos_] = static_cast<int16_t>(vmul(lout, static_cast<int16_t>(reg(kReverbVolL))));
        reverb_out_[1][reverb_pos_] = static_cast<int16_t>(vmul(rout, static_cast<int16_t>(reg(kReverbVolR))));

        const uint32_t base = reg(kReverbBase) * 4u;
        reverb_cur_ = reverb_cur_ + 1 >= kRamSize / 2 ? base : std::max(base, reverb_cur_ + 1);
    } else {
        reverb_out_[0][reverb_pos_] = 0;  // zero-stuffed 22050 -> 44100 upsampling
        reverb_out_[1][reverb_pos_] = 0;
    }
    out_l = clamp16(fir(reverb_out_[0], reverb_pos_) >> 14);
    out_r = clamp16(fir(reverb_out_[1], reverb_pos_) >> 14);

    reverb_pos_ = (reverb_pos_ + 1) & 63;
    reverb_phase_ = !reverb_phase_;
}

}  // namespace hle
