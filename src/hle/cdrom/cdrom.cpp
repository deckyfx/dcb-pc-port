#include "cdrom/cdrom.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hle {

namespace {

constexpr uint64_t kCpuHz = 33868800;
constexpr uint64_t kAckDelay = 25000;       // first response (INT3), ~0.7 ms
constexpr uint64_t kCompleteDelay = 150000;  // second response (INT2) of slow commands, ~4.4 ms
constexpr uint64_t kSeekDelay = 250000;      // before the first sector of a read, ~7 ms

constexpr uint8_t kStatMotor = 0x02, kStatRead = 0x20, kStatSeek = 0x40, kStatPlay = 0x80;
constexpr uint8_t kModeDoubleSpeed = 0x80, kModeXaAdpcm = 0x40, kModeWholeSector = 0x20,
                  kModeXaFilter = 0x08;

uint8_t bcd(uint32_t v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); }
uint32_t from_bcd(uint8_t v) { return (v >> 4) * 10u + (v & 0xFu); }

}  // namespace

CdRom::CdRom(std::function<void()> raise_irq2)
    : raise_irq2_(std::move(raise_irq2)), trace_(std::getenv("DCB_TRACE_CD") != nullptr) {}

uint64_t CdRom::sector_period() const { return kCpuHz / ((mode_ & kModeDoubleSpeed) ? 150 : 75); }

void CdRom::push(uint8_t irq, std::vector<uint8_t> bytes, uint64_t delay) {
    queue_.push_back({irq, std::move(bytes), now_ + delay, false});
}

uint8_t CdRom::status_register() const {
    uint8_t s = index_ & 3u;
    if (params_.empty()) s |= 1u << 3;          // parameter FIFO empty
    if (params_.size() < 16) s |= 1u << 4;      // parameter FIFO not full
    if (!response_.empty()) s |= 1u << 5;       // response FIFO not empty
    if (data_pos_ < data_.size()) s |= 1u << 6; // data FIFO not empty
    return s;                                   // never busy: commands are accepted at once
}

void CdRom::deliver_due() {
    // One interrupt at a time: the next response waits until the game acknowledges the flags.
    if ((irq_flags_ & 7u) || queue_.empty() || queue_.front().due > now_) return;
    Response r = std::move(queue_.front());
    queue_.pop_front();
    if (r.sector) {
        ready_ = sector_;
        data_loaded_ = false;  // the next BFRD loads the new sector
    }
    response_.assign(r.bytes.begin(), r.bytes.end());
    irq_flags_ = static_cast<uint8_t>((irq_flags_ & ~7u) | r.irq);
    if (irq_flags_ & irq_enable_ & 7u) raise_irq2_();
}

void CdRom::read_sector() {
    if (!disc_ || !disc_->read(read_lba_, sector_.data())) {
        reading_ = false;
        stat_ &= static_cast<uint8_t>(~kStatRead);
        push(5, {static_cast<uint8_t>(stat_ | 0x01), 0x04}, 0);  // error: seek failed / end of disc
        return;
    }
    if (trace_) std::fprintf(stderr, "[cd] sector %u submode %02X\n", read_lba_, sector_[18]);
    ++read_lba_;
    // XA audio sectors (Form 2, audio submode) go to the audio path when XA playback is enabled,
    // filtered by file/channel; they never raise a data interrupt.
    const uint8_t file = sector_[16], channel = sector_[17], submode = sector_[18];
    const bool xa_audio = (submode & 0x04) && (submode & 0x20);
    if ((mode_ & kModeXaAdpcm) && xa_audio) {
        const bool match = !(mode_ & kModeXaFilter) || (file == filter_file_ && channel == filter_channel_);
        (void)match;  // XA-ADPCM playback lands with the audio layer
        return;
    }
    queue_.push_back({1, {stat_}, now_, true});
}

void CdRom::tick(uint64_t now) {
    now_ = now;
    if (reading_ && now_ >= next_sector_) {
        // Deliver sectors on schedule; if the game has not acknowledged the previous one, the
        // drive keeps its place and retries (a slow reader loses nothing).
        if (queue_.empty() || !queue_.back().sector) read_sector();
        next_sector_ = now_ + sector_period();
    }
    deliver_due();
}

void CdRom::command(uint8_t cmd) {
    auto param = [&](size_t i) { return i < params_.size() ? params_[i] : uint8_t{0}; };
    if (trace_) {
        std::fprintf(stderr, "[cd] cmd %02X", cmd);
        for (uint8_t p : params_) std::fprintf(stderr, " %02X", p);
        std::fprintf(stderr, "  (mode %02X, lba %u)\n", mode_, read_lba_);
    }
    switch (cmd) {
        case 0x01:  // Getstat
            push(3, {stat_}, kAckDelay);
            break;
        case 0x02:  // Setloc amm ass asect (BCD)
            setloc_lba_ = (from_bcd(param(0)) * 60 + from_bcd(param(1))) * 75 + from_bcd(param(2)) - 150;
            setloc_pending_ = true;
            push(3, {stat_}, kAckDelay);
            break;
        case 0x06: case 0x1B:  // ReadN / ReadS
            if (setloc_pending_) {
                read_lba_ = setloc_lba_;
                setloc_pending_ = false;
            }
            stat_ = static_cast<uint8_t>((stat_ | kStatMotor | kStatRead) & ~kStatPlay);
            reading_ = true;
            next_sector_ = now_ + kSeekDelay;
            push(3, {stat_}, kAckDelay);
            break;
        case 0x03:  // Play (CD-DA: this disc has no audio tracks)
            stat_ = static_cast<uint8_t>(stat_ | kStatMotor | kStatPlay);
            push(3, {stat_}, kAckDelay);
            break;
        case 0x07:  // MotorOn
            stat_ |= kStatMotor;
            push(3, {stat_}, kAckDelay);
            push(2, {stat_}, kCompleteDelay);
            break;
        case 0x08:  // Stop
            reading_ = false;
            push(3, {stat_}, kAckDelay);
            stat_ = 0;
            push(2, {stat_}, kCompleteDelay);
            break;
        case 0x09:  // Pause
            push(3, {stat_}, kAckDelay);
            reading_ = false;
            stat_ = static_cast<uint8_t>(stat_ & ~(kStatRead | kStatPlay | kStatSeek));
            // Sectors already announced stay deliverable; drop not-yet-delivered ones.
            std::erase_if(queue_, [](const Response& r) { return r.sector; });
            push(2, {stat_}, kCompleteDelay);
            break;
        case 0x0A:  // Init
            mode_ = 0;
            reading_ = false;
            stat_ = kStatMotor;
            queue_.clear();
            push(3, {stat_}, kAckDelay);
            push(2, {stat_}, kCompleteDelay);
            break;
        case 0x0B: case 0x0C:  // Mute / Demute
            push(3, {stat_}, kAckDelay);
            break;
        case 0x0D:  // Setfilter file, channel
            filter_file_ = param(0);
            filter_channel_ = param(1);
            push(3, {stat_}, kAckDelay);
            break;
        case 0x0E:  // Setmode
            mode_ = param(0);
            push(3, {stat_}, kAckDelay);
            break;
        case 0x0F:  // Getparam
            push(3, {stat_, mode_, 0x00, filter_file_, filter_channel_}, kAckDelay);
            break;
        case 0x10:  // GetlocL: header + subheader of the current sector
            push(3, {ready_[12], ready_[13], ready_[14], ready_[15], ready_[16], ready_[17], ready_[18], ready_[19]},
                 kAckDelay);
            break;
        case 0x11: {  // GetlocP: track, index, relative MSF, absolute MSF
            const uint32_t lba = read_lba_ ? read_lba_ - 1 : 0;
            const uint32_t abs = lba + 150;
            push(3, {0x01, 0x01, bcd(lba / 4500), bcd(lba / 75 % 60), bcd(lba % 75), bcd(abs / 4500),
                     bcd(abs / 75 % 60), bcd(abs % 75)},
                 kAckDelay);
            break;
        }
        case 0x13:  // GetTN: first and last track
            push(3, {stat_, 0x01, 0x01}, kAckDelay);
            break;
        case 0x14: {  // GetTD track: start (track 0 = end of disc)
            const uint32_t abs = from_bcd(param(0)) == 0 && disc_ ? disc_->sector_count() + 150 : 150;
            push(3, {stat_, bcd(abs / 4500), bcd(abs / 75 % 60)}, kAckDelay);
            break;
        }
        case 0x15: case 0x16:  // SeekL / SeekP
            if (setloc_pending_) {
                read_lba_ = setloc_lba_;
                setloc_pending_ = false;
            }
            reading_ = false;
            push(3, {stat_}, kAckDelay);
            push(2, {stat_}, kSeekDelay);
            break;
        case 0x19:  // Test
            if (param(0) == 0x20) push(3, {0x94, 0x09, 0x19, 0xC0}, kAckDelay);  // controller BIOS date/version
            else push(3, {stat_}, kAckDelay);
            break;
        case 0x1A:  // GetID: licensed Japanese Mode 2 disc ("SCEI")
            push(3, {stat_}, kAckDelay);
            push(2, {stat_, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'I'}, kCompleteDelay);
            break;
        case 0x1E:  // ReadTOC
            push(3, {stat_}, kAckDelay);
            push(2, {stat_}, kCompleteDelay * 4);
            break;
        default:
            std::fprintf(stderr, "[cdrom] unsupported command %02X\n", cmd);
            push(5, {static_cast<uint8_t>(stat_ | 0x01), 0x40}, kAckDelay);  // invalid command
            break;
    }
    params_.clear();
}

uint8_t CdRom::read(uint32_t phys) {
    switch (phys - kBase) {
        case 0: return status_register();
        case 1: {  // response FIFO
            if (response_.empty()) return 0;
            const uint8_t v = response_.front();
            response_.pop_front();
            return v;
        }
        case 2:  // data FIFO (byte reads)
            return data_pos_ < data_.size() ? data_[data_pos_++] : 0;
        default:  // 3: interrupt enable (index 0/2) or flags (index 1/3)
            return (index_ & 1u) ? static_cast<uint8_t>(irq_flags_ | 0xE0u) : static_cast<uint8_t>(irq_enable_ | 0xE0u);
    }
}

void CdRom::write(uint32_t phys, uint8_t value) {
    const uint32_t reg = phys - kBase;
    if (reg == 0) {
        index_ = value & 3u;
        return;
    }
    switch (reg << 2 | index_) {
        case (1 << 2 | 0): command(value); break;              // command
        case (2 << 2 | 0): params_.push_back(value); break;    // parameter FIFO
        case (2 << 2 | 1): irq_enable_ = value & 0x1Fu; break;
        case (3 << 2 | 0):                                     // request: BFRD
            // 1 loads the announced sector into the data FIFO; repeating it while loaded does not
            // rewind (libcd requests again before each partial transfer). 0 empties the FIFO.
            if ((value & 0x80u) && !data_loaded_) {
                const bool whole = mode_ & kModeWholeSector;
                const size_t offset = whole ? 12 : 24, size = whole ? 2340 : 2048;
                data_.assign(ready_.begin() + static_cast<std::ptrdiff_t>(offset),
                             ready_.begin() + static_cast<std::ptrdiff_t>(offset + size));
                data_pos_ = 0;
                data_loaded_ = true;
            } else if (!(value & 0x80u)) {
                data_.clear();
                data_pos_ = 0;
                data_loaded_ = false;
            }
            break;
        case (3 << 2 | 1):                                     // acknowledge interrupt flags
            irq_flags_ = static_cast<uint8_t>(irq_flags_ & ~(value & 0x1Fu));
            if (value & 0x40u) params_.clear();
            if (!(irq_flags_ & 7u)) response_.clear();
            deliver_due();
            break;
        default:  // audio volume / sound map registers: accepted, audio comes later
            break;
    }
}

void CdRom::dma_read(uint32_t* words, uint32_t count) {
    if (trace_) std::fprintf(stderr, "[cd] dma %u words (fifo %zu/%zu)\n", count, data_pos_, data_.size());
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t w = 0;
        for (int b = 0; b < 4; ++b) {
            const uint8_t v = data_pos_ < data_.size() ? data_[data_pos_++] : 0;
            w |= static_cast<uint32_t>(v) << (8 * b);
        }
        words[i] = w;
    }
}

}  // namespace hle
