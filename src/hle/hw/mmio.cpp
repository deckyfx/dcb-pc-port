#include "hw/mmio.hpp"

#include "cdrom/load_log.hpp"
#include "system.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace hle {

namespace {

constexpr uint32_t kIStat = 0x1F801070, kIMask = 0x1F801074;
constexpr uint32_t kDmaBase = 0x1F801080, kDpcr = 0x1F8010F0, kDicr = 0x1F8010F4;
constexpr uint32_t kTimerBase = 0x1F801100;
constexpr uint32_t kGp0 = 0x1F801810, kGp1 = 0x1F801814;
constexpr uint32_t kMdecData = 0x1F801820, kMdecControl = 0x1F801824;

constexpr uint32_t kChcrBusy = 1u << 24;        // start/busy
constexpr uint32_t kChcrTrigger = 1u << 28;     // manual trigger (cleared when the transfer starts)

constexpr unsigned kLogLimit = 4;

}  // namespace

Mmio::Mmio()
    : cdrom_([this] { raise_irq(2); }, [this] { return system_ ? system_->cpu_cycles() : uint64_t{0}; }),
      sio_([this] { raise_irq(7); }, [this] { return system_ ? system_->cpu_cycles() : uint64_t{0}; }) {
    cdrom_.on_cd_audio([this](const int16_t* pcm, size_t frames) { spu_.push_cd_audio(pcm, frames); });
}

void Mmio::tick(uint64_t cycles) {
    cdrom_.tick(cycles);
    sio_.tick(cycles);
    // The SPU runs at 44100 Hz = one sample per 768 CPU cycles: produce what guest time owes.
    constexpr uint64_t kCyclesPerSample = 768;
    const uint64_t due = cycles / kCyclesPerSample;
    if (due <= spu_samples_) return;
    uint64_t owed = due - spu_samples_;
    if (owed > Spu::kSampleRate) owed = Spu::kSampleRate;  // a long native wait: don't build a backlog
    spu_samples_ = due;
    const size_t old = audio_.size();
    audio_.resize(old + static_cast<size_t>(owed) * 2);
    spu_.mix(audio_.data() + old, static_cast<size_t>(owed));
    if (spu_.take_irq()) raise_irq(9);
}

const std::vector<int16_t>& Mmio::take_audio() {
    audio_out_.swap(audio_);
    audio_.clear();
    return audio_out_;
}

uint64_t Mmio::timer_clock(unsigned index) const {
    if (!system_) return 0;
    const uint32_t source = (timer_[index].mode >> 8) & 3u;
    switch (index) {
        case 1: return (source & 1u) ? system_->hblanks() : system_->cpu_cycles();
        case 2: return (source & 2u) ? system_->cpu_cycles() / 8 : system_->cpu_cycles();
        default: return system_->cpu_cycles();  // dot clock approximated by the CPU clock
    }
}

uint32_t Mmio::timer_counter(unsigned index) const {
    const Timer& t = timer_[index];
    uint64_t ticks = timer_clock(index) - t.base;
    // Mode bit 3: counter resets after reaching the target; otherwise it wraps at 0xFFFF.
    const uint64_t period = (t.mode & 8u) && t.target ? t.target + 1ull : 0x10000ull;
    return static_cast<uint32_t>(ticks % period);
}

void Mmio::gp0(uint32_t word) {
    gpu_.gp0(word);
    if (gpu_.irq_pending()) raise_irq(1);  // GP0(1Fh); acknowledged through GP1(02h)
}

void Mmio::dma_write(unsigned channel, unsigned reg, uint32_t value) {
    dma_[channel][reg] = value;
    if (reg == 2 && (value & kChcrBusy)) {
        // Transfers run to completion immediately, then signal like the DMA controller does.
        dma_run(channel);
        dma_[channel][2] &= ~(kChcrBusy | kChcrTrigger);
        const bool master = dicr_ & (1u << 23);
        if (master && (dicr_ & (1u << (16 + channel)))) {
            dicr_ |= 1u << (24 + channel);
            raise_irq(3);
        }
    }
}

void Mmio::dma_gpu_linked_list(uint32_t addr) {
    // Each node: header (word count << 24 | next), then that many GP0 words. Bit 23 ends the list.
    for (uint32_t nodes = 0; nodes < (1u << 20); ++nodes) {
        const uint32_t header = psx_read32(ctx_, addr & 0x1FFFFCu);
        const uint32_t words = header >> 24;
        gpu_.set_packet_address(addr & 0x1FFFFCu);
        for (uint32_t k = 1; k <= words; ++k) gp0(psx_read32(ctx_, (addr & 0x1FFFFCu) + 4 * k));
        if (header & 0x800000u) {
            gpu_.set_packet_address(0);
            return;
        }
        addr = header & 0xFFFFFFu;
    }
    gpu_.set_packet_address(0);
    std::fprintf(stderr, "[dma] GPU linked list does not terminate (cycle?) - stopped\n");
}

void Mmio::dma_run(unsigned channel) {
    if (!ctx_) return;
    const uint32_t madr = dma_[channel][0] & 0x1FFFFCu;
    const uint32_t bcr = dma_[channel][1];
    const uint32_t chcr = dma_[channel][2];
    const bool from_ram = chcr & 1u;
    const int32_t step = (chcr & 2u) ? -4 : 4;
    const uint32_t sync = (chcr >> 9) & 3u;
    uint32_t words = sync == 0 ? ((bcr & 0xFFFFu) ? (bcr & 0xFFFFu) : 0x10000u)
                               : (bcr & 0xFFFFu) * (bcr >> 16);

    switch (channel) {
        case 0: {  // RAM -> MDEC (compressed macroblocks)
            std::vector<uint32_t> buf(words);
            for (uint32_t i = 0, a = madr; i < words; ++i, a += static_cast<uint32_t>(step)) buf[i] = psx_read32(ctx_, a);
            mdec_.dma_write(buf.data(), words);
            return;
        }
        case 1: {  // MDEC -> RAM (decoded pixels)
            std::vector<uint32_t> buf(words);
            mdec_.dma_read(buf.data(), words);
            if (mdec_transfers_++ == 0) LoadLog::instance().mdec(true);
            for (uint32_t i = 0, a = madr; i < words; ++i, a += static_cast<uint32_t>(step)) psx_write32(ctx_, a, buf[i]);
            return;
        }
        case 2: {  // GPU
            const auto t0 = std::chrono::steady_clock::now();
            struct GpuTimer {  // time spent rasterizing, for the overlay
                std::chrono::steady_clock::time_point start;
                uint64_t& total;
                ~GpuTimer() {
                    total += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                       std::chrono::steady_clock::now() - start).count());
                }
            } gpu_timer{t0, gpu_ns_};
            if (sync == 2) {
                dma_gpu_linked_list(madr);
            } else if (from_ram) {
                for (uint32_t i = 0, a = madr; i < words; ++i, a += static_cast<uint32_t>(step)) gp0(psx_read32(ctx_, a));
            } else {
                for (uint32_t i = 0, a = madr; i < words; ++i, a += static_cast<uint32_t>(step)) psx_write32(ctx_, a, gpu_.gpuread());
            }
            return;
        }
        case 3: {  // CD-ROM -> RAM
            if (std::getenv("DCB_TRACE_CD")) std::fprintf(stderr, "[cd] dma3 -> %08X (%u words, chcr %08X)\n", madr, words, chcr);
            std::vector<uint32_t> buf(words);
            cdrom_.dma_read(buf.data(), words);
            for (uint32_t i = 0; i < words; ++i) psx_write32(ctx_, madr + 4 * i, buf[i]);
            return;
        }
        case 4: {  // SPU
            std::vector<uint32_t> buf(words);
            if (from_ram) {
                for (uint32_t i = 0; i < words; ++i) buf[i] = psx_read32(ctx_, madr + 4 * i);
                // Guest RAM -> SPU RAM: sample-bank uploads. Coalescing would
                // hide the transfer size/address the analyst needs, so log each.
                LoadLog::instance().spu(spu_.transfer_addr(), words * 4);
                spu_.dma_write(buf.data(), words);
            } else {
                spu_.dma_read(buf.data(), words);
                for (uint32_t i = 0; i < words; ++i) psx_write32(ctx_, madr + 4 * i, buf[i]);
            }
            if (spu_.take_irq()) raise_irq(9);  // IRQ address hit by the transfer
            return;
        }
        case 6:  // OTC: build an empty ordering table backwards; the last entry ends the list
            for (uint32_t i = 0, a = madr; i < words; ++i, a -= 4) {
                psx_write32(ctx_, a, i + 1 == words ? 0x00FFFFFFu : ((a - 4) & 0x1FFFFFu));
            }
            return;
        default: {
            static bool warned[7] = {};
            if (!warned[channel]) {
                warned[channel] = true;
                std::fprintf(stderr, "[dma] channel %u transfer not implemented yet (chcr %08X)\n", channel, chcr);
            }
            return;
        }
    }
}

uint32_t Mmio::dicr() const {
    // Bit 31 is computed: force (15), or master enable (23) with an enabled channel flagged.
    const bool irq = (dicr_ & (1u << 15)) || ((dicr_ & (1u << 23)) && ((dicr_ >> 24) & (dicr_ >> 16) & 0x7Fu));
    return (dicr_ & 0x7FFFFFFFu) | (irq ? 0x80000000u : 0u);
}

uint32_t Mmio::dma_reg_read(uint32_t aligned) const {
    if (aligned == kDpcr) return dpcr_;
    if (aligned == kDicr) return dicr();
    if (aligned >= kDmaBase && aligned < kDpcr) return dma_[(aligned - kDmaBase) >> 4][((aligned & 0xF) >> 2) % 3];
    return 0;
}

void Mmio::dma_reg_write(uint32_t aligned, uint32_t value, uint32_t lanes) {
    // `lanes` masks the bytes actually written: libcd flips DICR channel enables with byte writes.
    if (aligned == kDicr) {
        const uint32_t plain = lanes & 0x00FF803Fu;               // enables, master, force, bits 0-5
        dicr_ = (dicr_ & ~plain) | (value & plain);
        dicr_ &= ~(value & lanes & 0x7F000000u);                  // flags: writing 1 acknowledges
        return;
    }
    const uint32_t merged = (dma_reg_read(aligned) & ~lanes) | (value & lanes);
    if (aligned == kDpcr) {
        dpcr_ = merged;
        return;
    }
    dma_write((aligned - kDmaBase) >> 4, ((aligned & 0xF) >> 2) % 3, merged);
}

uint32_t Mmio::read(uint32_t phys, unsigned width) {
    // Status registers are what games spin on: let time advance and interrupts arrive.
    if (system_ && (phys == kIStat || phys == kGp1 || (phys >= kTimerBase && phys < kTimerBase + 0x30)))
        system_->io_poll();
    switch (phys) {
        case kIStat: return i_stat_;
        case kIMask: return i_mask_;
        case kGp0: return gpu_.gpuread();
        case kGp1: return gpu_.gpustat();
        case kMdecData: return mdec_.read_data();
        case kMdecControl: return mdec_.status();
        default: break;
    }
    if (phys >= CdRom::kBase && phys < CdRom::kEnd) return cdrom_.read(phys);
    if (phys >= Sio0::kBase && phys < Sio0::kEnd) return sio_.read(phys, width);
    if (phys >= Spu::kBase && phys < Spu::kEnd) {
        const uint32_t lo = spu_.read16(phys & ~1u);
        return width == 4 ? lo | static_cast<uint32_t>(spu_.read16((phys & ~1u) + 2)) << 16 : lo;
    }
    if (phys >= kDmaBase && phys < kDicr + 4) {  // DMA registers: any width, any byte lane
        return dma_reg_read(phys & ~3u) >> (8 * (phys & 3u));
    }
    if (phys >= kTimerBase && phys < kTimerBase + 0x30) {
        const unsigned index = (phys - kTimerBase) >> 4;
        switch ((phys & 0xF) >> 2) {
            case 0: return timer_counter(index);
            case 1: return timer_[index].mode;
            default: return timer_[index].target;
        }
    }
    const auto it = misc_.find(phys);
    note(phys, false, 0, width);
    return it == misc_.end() ? 0 : it->second;
}

void Mmio::write(uint32_t phys, uint32_t value, unsigned width) {
    switch (phys) {
        case kIStat: i_stat_ &= value; return;  // writing 0 acknowledges
        case kIMask: i_mask_ = value & 0x7FFu; return;
        case kGp0: gp0(value); return;
        case kGp1:
            if ((value >> 24) == 0x05) ++display_flips_;  // display start: the game shows a new frame
            gpu_.gp1(value);
            return;
        case kMdecData: mdec_.write_command(value); return;
        case kMdecControl: mdec_.write_control(value); return;
        default: break;
    }
    if (phys >= CdRom::kBase && phys < CdRom::kEnd) {
        cdrom_.write(phys, static_cast<uint8_t>(value));
        return;
    }
    if (phys >= Sio0::kBase && phys < Sio0::kEnd) {
        sio_.write(phys, value, width);
        return;
    }
    if (phys >= Spu::kBase && phys < Spu::kEnd) {
        spu_.write16(phys & ~1u, static_cast<uint16_t>(value));
        if (width == 4) spu_.write16((phys & ~1u) + 2, static_cast<uint16_t>(value >> 16));
        return;
    }
    if (phys >= kDmaBase && phys < kDicr + 4) {
        const uint32_t shift = 8 * (phys & 3u);
        const uint32_t lanes = (width == 4 ? 0xFFFFFFFFu : width == 2 ? 0xFFFFu : 0xFFu) << shift;
        dma_reg_write(phys & ~3u, value << shift, lanes);
        return;
    }
    if (phys >= kTimerBase && phys < kTimerBase + 0x30) {
        const unsigned index = (phys - kTimerBase) >> 4;
        Timer& t = timer_[index];
        switch ((phys & 0xF) >> 2) {
            case 0:  // set counter value
                t.base = timer_clock(index) - (value & 0xFFFFu);
                break;
            case 1:  // mode write also resets the counter
                t.mode = value & 0x3FFu;
                t.base = timer_clock(index);
                break;
            default:
                t.target = value & 0xFFFFu;
                break;
        }
        return;
    }
    misc_[phys] = value;
    note(phys, true, value, width);
}

void Mmio::note(uint32_t phys, bool write, uint32_t value, unsigned width) {
    unsigned& n = logged_[{phys, write}];
    if (n >= kLogLimit) return;
    if (++n == kLogLimit) {
        std::fprintf(stderr, "[mmio] %s%u %08X ... (further accesses not logged)\n", write ? "write" : "read", width * 8, phys);
    } else if (write) {
        std::fprintf(stderr, "[mmio] write%u %08X = %08X\n", width * 8, phys, value);
    } else {
        std::fprintf(stderr, "[mmio] read%u  %08X\n", width * 8, phys);
    }
}

// ---------------------------------------------------------------------------------------------
// Save state

void Mmio::save_state(psx::StateWriter& w) const {
    w.begin(psx::state_tag("MMIO"), 1);
    w.u32(i_stat_);
    w.u32(i_mask_);
    w.pod(dma_);
    w.u32(dpcr_);
    w.u32(dicr_);
    for (const Timer& t : timer_) {
        w.u32(t.mode);
        w.u32(t.target);
        w.u64(t.base);
    }
    w.u64(spu_samples_);
    w.vec(audio_);
    w.u64(display_flips_);
    w.u64(mdec_transfers_);
    w.map(misc_);
    gpu_.save_state(w);
    spu_.save_state(w);
    cdrom_.save_state(w);
    sio_.save_state(w);
    mdec_.save_state(w);
    w.end();
}

void Mmio::load_state(psx::StateReader& r) {
    r.begin(psx::state_tag("MMIO"), 1);
    i_stat_ = r.u32();
    i_mask_ = r.u32();
    r.pod(dma_);
    dpcr_ = r.u32();
    dicr_ = r.u32();
    for (Timer& t : timer_) {
        t.mode = r.u32();
        t.target = r.u32();
        t.base = r.u64();
    }
    spu_samples_ = r.u64();
    r.vec(audio_, size_t{Spu::kSampleRate} * 2 * 4);
    display_flips_ = r.u64();
    mdec_transfers_ = r.u64();
    r.map(misc_, 1u << 16);
    gpu_.load_state(r);
    spu_.load_state(r);
    cdrom_.load_state(r);
    sio_.load_state(r);
    mdec_.load_state(r);
    r.end();
}

}  // namespace hle
