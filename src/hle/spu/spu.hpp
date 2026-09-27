#pragma once
// SPU: register file, 512 KB sound RAM and the audio pipeline (24 ADPCM voices with pitch,
// 4-point interpolation, ADSR, volume sweep, noise and pitch modulation; reverb; CD audio input;
// main volume). The SPU runs at 44100 Hz, so mix() produces output samples directly and advances
// the device by the same amount of time. Not thread-safe: call everything from the game thread.
// Reference: psx-spx "Sound Processing Unit (SPU)".

#include "spu/envelope.hpp"

#include <psx/state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hle {

class Spu {
public:
    static constexpr uint32_t kBase = 0x1F801C00, kEnd = 0x1F802000;
    static constexpr uint32_t kRamSize = 512 * 1024;
    static constexpr uint32_t kSampleRate = 44100;
    static constexpr int kVoices = 24;
    /// CD audio queue capacity in stereo frames (~0.74 s); older frames are dropped on overflow.
    static constexpr size_t kCdQueueFrames = 32768;

    Spu();

    uint16_t read16(uint32_t phys) const;
    void write16(uint32_t phys, uint16_t value);

    /// DMA channel 4, RAM -> SPU: append words at the current transfer address.
    void dma_write(const uint32_t* words, uint32_t count);
    /// DMA channel 4, SPU -> RAM.
    void dma_read(uint32_t* words, uint32_t count);

    /// Produce `frames` stereo s16 frames at 44100 Hz into `out` (interleaved L/R) and advance the
    /// SPU by that much time (key on/off, ENDX, envelopes, IRQ address hits). Allocation-free.
    void mix(int16_t* out, size_t frames);

    /// CD audio input (decoded XA or CD-DA), stereo s16 at 44100 Hz, already scaled by the CD-ROM
    /// controller's own volume matrix; mixed by mix() with the SPU's CD volume (AVOLL/AVOLR).
    void push_cd_audio(const int16_t* stereo, size_t frames);
    /// Stereo frames waiting in the CD audio queue (lets the producer pace itself).
    size_t cd_audio_queued() const { return cd_count_; }
    /// Drop queued CD audio (e.g. on CD Stop/Pause or a seek).
    void clear_cd_audio() { cd_head_ = cd_count_ = 0; }

    /// True when the SPU IRQ address was hit since the last call (SPUCNT bit 6 enabled): the
    /// integrator raises interrupt 9 for it. SPUSTAT bit 6 stays set until SPUCNT bit 6 is cleared.
    bool take_irq();

    /// Sound RAM, for tests and debugging.
    const uint8_t* ram() const { return ram_.data(); }

    /// Current DMA transfer address in sound RAM (for the load log).
    uint32_t transfer_addr() const { return transfer_addr_; }

    /// Save state: registers, sound RAM, voices (ADPCM decoder, ADSR, sweeps), noise, capture,
    /// the CD audio queue and reverb state (chunk "SPU ").
    void save_state(psx::StateWriter& w) const;
    void load_state(psx::StateReader& r);

private:
    enum class Phase : uint8_t { Off, Attack, Decay, Sustain, Release };

    struct Voice {
        uint32_t addr = 0;     ///< current byte address of the ADPCM block being played
        uint32_t counter = 0;  ///< pitch counter: bits 12+ sample in block, bits 4-11 interpolation
        /// [0..2] = last three samples of the previous block (interpolation history), [3..30] = block.
        std::array<int16_t, 31> samples{};
        uint8_t flags = 0;     ///< ADPCM flags of the current block
        bool has_block = false;
        bool ignore_loop = false;  ///< software wrote the repeat address: ignore Loop Start flags
        Phase phase = Phase::Off;
        int16_t adsr = 0;          ///< ENVX
        int32_t adsr_target = 0;
        spu::Envelope adsr_env;
        spu::Sweep vol_l, vol_r;
        int16_t out = 0;           ///< VxOUTX: sample after ADSR (pitch modulation source, capture)
    };

    std::array<uint16_t, (kEnd - kBase) / 2> regs_{};
    std::vector<uint8_t> ram_;
    uint32_t transfer_addr_ = 0;  ///< byte address in sound RAM

    std::array<Voice, kVoices> voices_{};
    uint32_t pending_kon_ = 0, pending_koff_ = 0;
    uint32_t endx_ = 0;
    spu::Sweep main_l_, main_r_;
    bool irq_flag_ = false, irq_pending_ = false;

    // Noise generator (psx-spx "SPU Noise Generator").
    int32_t noise_timer_ = 0;
    uint16_t noise_level_ = 1;

    // Capture buffers (CD L/R, voice 1/3) write position, 0..1FFh halfwords.
    uint32_t capture_pos_ = 0;

    // CD audio ring buffer (interleaved stereo).
    std::vector<int16_t> cd_buf_;
    size_t cd_head_ = 0, cd_count_ = 0;

    // Reverb (psx-spx "SPU Reverb Formula"): runs at 22050 Hz behind 39-tap resampling filters.
    uint32_t reverb_cur_ = 0;  ///< current buffer address, in halfwords
    std::array<std::array<int16_t, 64>, 2> reverb_in_{}, reverb_out_{};
    uint32_t reverb_pos_ = 0;
    bool reverb_phase_ = false;

    uint16_t& reg(uint32_t phys) { return regs_[(phys - kBase) / 2]; }
    uint16_t reg(uint32_t phys) const { return regs_[(phys - kBase) / 2]; }
    uint16_t voice_reg(int v, uint32_t off) const { return regs_[(static_cast<uint32_t>(v) * 0x10 + off) / 2]; }

    bool irq_enabled() const;
    void check_irq(uint32_t byte_addr);
    void key_on(int v);
    void key_off(int v);
    void update_adsr(Voice& vc, int v);
    void decode_block(Voice& vc, int v);
    int32_t sample_voice(int v);
    void tick_noise();
    void process_reverb(int32_t in_l, int32_t in_r, int32_t& out_l, int32_t& out_r);
    int16_t reverb_read(int32_t rel);
    void reverb_write(int32_t rel, int32_t value);
    uint32_t reverb_addr(int32_t rel) const;
};

}  // namespace hle
