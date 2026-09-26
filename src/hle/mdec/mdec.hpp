#pragma once
// MDEC (macroblock decoder): run-level decoding, dequantisation, IDCT and YUV->RGB for STR video.
// libpress feeds compressed macroblocks through DMA0 (0x1F801820 writes) and drains decoded pixels
// through DMA1 (0x1F801820 reads). Every command completes as soon as its last parameter word
// arrives, so the device is never "busy" between commands and output is ready immediately.
// Reference: psx-spx "Macroblock Decoder (MDEC)".

#include <psx/state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hle {

class Mdec {
public:
    static constexpr uint32_t kDataAddr = 0x1F801820;     ///< MDEC0: command/parameter write, data read
    static constexpr uint32_t kControlAddr = 0x1F801824;  ///< MDEC1: control write, status read
    static constexpr uint32_t kResetStatus = 0x80040000;  ///< psx-spx "MDEC Control/Reset Register"

    Mdec() { reset(); }

    /// 0x1F801820 write: a command word, or the next parameter word of the pending command.
    void write_command(uint32_t word);
    /// 0x1F801820 read: next decoded output word (0 when the output FIFO is empty).
    uint32_t read_data();
    /// 0x1F801824 write: bit31 reset, bit30 enable DMA0 (data-in), bit29 enable DMA1 (data-out).
    void write_control(uint32_t word);
    /// 0x1F801824 read, laid out as psx-spx "MDEC Status Register".
    uint32_t status() const;
    /// Words read_data() can return right now.
    size_t output_words_available() const { return out_.size() - out_pos_; }

    /// DMA channel 0 (RAM -> MDEC): same as calling write_command() for each word.
    void dma_write(const uint32_t* words, uint32_t count);
    /// DMA channel 1 (MDEC -> RAM): same as calling read_data() for each word.
    void dma_read(uint32_t* words, uint32_t count);

    bool dma_in_enabled() const { return dma_in_enabled_; }
    bool dma_out_enabled() const { return dma_out_enabled_; }

    /// Abort any command and clear the FIFOs (control bit 31). Tables survive, as on hardware.
    void reset();

    const std::array<uint8_t, 64>& luma_quant() const { return quant_y_; }
    const std::array<uint8_t, 64>& chroma_quant() const { return quant_uv_; }
    const std::array<int16_t, 64>& scale_table() const { return scale_; }

    /// Save state: command state, tables, the block being decoded and the output FIFO ("MDEC").
    void save_state(psx::StateWriter& w) const;
    void load_state(psx::StateReader& r);

private:
    enum class State : uint8_t { Idle, Decode, QuantTable, ScaleTable };
    using Block = std::array<int16_t, 64>;

    // Mode bits of the current MDEC(1) command word (psx-spx "MDEC(1) - Decode Macroblock(s)").
    uint32_t depth() const { return (command_ >> 27) & 3u; }  ///< 0=4bit 1=8bit 2=24bit 3=15bit
    bool is_signed() const { return (command_ >> 26) & 1u; }
    bool set_bit15() const { return (command_ >> 25) & 1u; }
    bool colour() const { return depth() >= 2; }

    void start_command(uint32_t word);
    void parameter(uint32_t word);
    void decode_halfword(uint16_t n);
    void finish_block();
    void yuv_to_rgb(unsigned xx, unsigned yy);
    void emit_colour();
    void emit_mono();

    State state_ = State::Idle;
    uint32_t command_ = 0;       ///< last command word (mode bits feed the status register)
    uint32_t remaining_ = 0;     ///< parameter words still expected
    uint16_t param_field_ = 0;   ///< status bits 15-0
    uint32_t table_pos_ = 0;     ///< words received by MDEC(2)/MDEC(3)
    bool dma_in_enabled_ = false, dma_out_enabled_ = false;

    std::array<uint8_t, 64> quant_y_{}, quant_uv_{};
    std::array<int16_t, 64> scale_{};

    // Run-level decoder state: one block at a time, fed halfword by halfword.
    Block blk_{};
    unsigned coef_ = 64;     ///< 64 = waiting for the DC halfword of the next block
    uint32_t q_scale_ = 0;
    unsigned block_ = 0;     ///< colour: 0=Cr 1=Cb 2..5=Y1..Y4 (decode order)
    Block cr_{}, cb_{};
    std::array<uint32_t, 256> rgb_{};  ///< 16x16 macroblock, R | G<<8 | B<<16 (sign already applied)

    std::vector<uint32_t> out_;
    size_t out_pos_ = 0;
};

}  // namespace hle
