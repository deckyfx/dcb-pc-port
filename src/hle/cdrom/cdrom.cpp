#include "cdrom/cdrom.hpp"
#include "cdrom/load_log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hle {

namespace {

constexpr uint64_t kCpuHz = 33868800;
constexpr uint64_t kAckDelay = 25000;       // first response (INT3), ~0.7 ms
constexpr uint64_t kCompleteDelay = 150000;  // second response (INT2) of slow commands, ~4.4 ms
constexpr uint64_t kSeekDelay = 250000;      // before the first sector of a read, ~7 ms
// After an acknowledge the next queued interrupt waits a little (psx-spx "CDROM - Response/Data
// Queueing": data requests shortly after the acknowledge still belong to the old INT1). libcd
// acknowledges INT1 before its callback requests the sector: without this gap, a sector already
// buffered behind it (the game was slow to take the last one) replaced it before the request.
constexpr uint64_t kRearmDelay = 2000;       // ~60 us

constexpr uint8_t kStatMotor = 0x02, kStatRead = 0x20, kStatSeek = 0x40, kStatPlay = 0x80;
constexpr uint8_t kModeDoubleSpeed = 0x80, kModeXaAdpcm = 0x40, kModeWholeSector = 0x20,
                  kModeXaFilter = 0x08;

uint8_t bcd(uint32_t v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); }
uint32_t from_bcd(uint8_t v) { return (v >> 4) * 10u + (v & 0xFu); }

}  // namespace

CdRom::CdRom(std::function<void()> raise_irq2, std::function<uint64_t()> clock)
    : raise_irq2_(std::move(raise_irq2)), clock_(std::move(clock)), trace_(std::getenv("DCB_TRACE_CD") != nullptr) {}

void CdRom::reset_xa() {
    xa_.reset();
    // A seek/stop/init ends any live XA stream (the decoder state is dropped).
    if (xa_active_) {
        xa_active_ = false;
        LoadLog::instance().xa(false, filter_file_, filter_channel_);
    }
}

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
    // One interrupt at a time: the next response waits until the game acknowledges the flags,
    // and then kRearmDelay more.
    if ((irq_flags_ & 7u) || now_ < hold_until_ || queue_.empty() || queue_.front().due > now_) return;
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
    LoadLog::instance().sector(read_lba_);
    ++read_lba_;
    // XA audio sectors (Form 2, audio submode) go to the audio path when XA playback is enabled,
    // filtered by file/channel; they never raise a data interrupt.
    const uint8_t file = sector_[16], channel = sector_[17], submode = sector_[18];
    const bool xa_audio = (submode & 0x04) && (submode & 0x20);
    if ((mode_ & kModeXaAdpcm) && xa_audio) {
        const bool match = !(mode_ & kModeXaFilter) || (file == filter_file_ && (channel & 0x1F) == filter_channel_);
        // Log stream edges only: start when a matching stream begins, stop when
        // it ends (non-matching sector, mode change, or seek — those reset
        // the decoder, see command()).
        if (match && !xa_active_) LoadLog::instance().xa(true, file, channel);
        if (!match && xa_active_) LoadLog::instance().xa(false, file, channel);
        xa_active_ = match;
        if (match && cd_audio_) {
            // Keep decoding while muted so the ADPCM filter history stays continuous.
            xa_pcm_.clear();
            if (xa_.decode(sector_.data(), xa_pcm_) && !xa_pcm_.empty()) {
                const size_t frames = xa_pcm_.size() / 2;
                apply_cd_volume(xa_pcm_.data(), frames, atv_[0], atv_[1], atv_[3], atv_[2], muted_ || xa_muted_);
                cd_audio_(xa_pcm_.data(), frames);
            }
        }
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
        std::fprintf(stderr, "[cd %7.3fs] cmd %02X", static_cast<double>(now_) / static_cast<double>(kCpuHz), cmd);
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
            reset_xa();
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
            reset_xa();
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
            muted_ = false;
            reset_xa();
            reading_ = false;
            stat_ = kStatMotor;
            queue_.clear();
            push(3, {stat_}, kAckDelay);
            push(2, {stat_}, kCompleteDelay);
            break;
        case 0x0B: case 0x0C:  // Mute / Demute (CD audio output)
            muted_ = cmd == 0x0B;
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
    const uint8_t v = read_reg(phys);
    if (trace_ && trace_regs_ < 60) {
        ++trace_regs_;
        std::fprintf(stderr, "[cd %7.3fs] rd %u.%u -> %02X\n", static_cast<double>(now_) / static_cast<double>(kCpuHz),
                     phys - kBase, index_, v);
    }
    return v;
}

uint8_t CdRom::read_reg(uint32_t phys) {
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
    if (trace_ && trace_regs_ < 60) {
        ++trace_regs_;
        std::fprintf(stderr, "[cd %7.3fs] wr %u.%u <- %02X\n", static_cast<double>(now_) / static_cast<double>(kCpuHz),
                     reg, index_, value);
    }
    if (reg == 0) {
        index_ = value & 3u;
        return;
    }
    switch (reg << 2 | index_) {
        case (1 << 2 | 0): command(value); break;              // command
        case (2 << 2 | 0): params_.push_back(value); break;    // parameter FIFO
        case (2 << 2 | 1):  // interrupt enable: a response already waiting interrupts now
            irq_enable_ = value & 0x1Fu;
            if (irq_flags_ & irq_enable_ & 7u) raise_irq2_();
            break;
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
            if (!(irq_flags_ & 7u)) {
                response_.clear();
                if (clock_) now_ = std::max(now_, clock_());  // the ack lands between ticks
                hold_until_ = now_ + kRearmDelay;  // the next response comes on a later tick
            }
            break;
        // CD audio volume matrix (psx-spx "CDROM Audio Volume"): latched, applied by ADPCTL bit 5.
        case (2 << 2 | 2): atv_pending_[0] = value; break;     // L -> L
        case (3 << 2 | 2): atv_pending_[1] = value; break;     // L -> R
        case (1 << 2 | 3): atv_pending_[2] = value; break;     // R -> R
        case (2 << 2 | 3): atv_pending_[3] = value; break;     // R -> L
        case (3 << 2 | 3):                                     // ADPCTL
            xa_muted_ = value & 0x01u;
            if (value & 0x20u) std::copy(std::begin(atv_pending_), std::end(atv_pending_), std::begin(atv_));
            break;
        default:  // sound map (XA from the CPU): unused by this game
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

// ---------------------------------------------------------------------------------------------
// Save state

namespace {
constexpr size_t kMaxQueue = 4096;    // responses waiting (a few in practice)
constexpr size_t kMaxFifo = 4096;     // parameter / response FIFO bytes
}  // namespace

void CdRom::save_state(psx::StateWriter& w) const {
    w.begin(psx::state_tag("CDRM"), 1);
    w.u64(now_);
    w.u8(index_);
    w.u8(irq_enable_);
    w.u8(irq_flags_);
    w.u64(hold_until_);
    w.deque(params_);
    w.deque(response_);
    w.vec(data_);
    w.size(data_pos_);
    w.boolean(data_loaded_);
    w.u8(mode_);
    w.u8(stat_);
    w.u32(setloc_lba_);
    w.u32(read_lba_);
    w.boolean(setloc_pending_);
    w.boolean(reading_);
    w.u8(filter_file_);
    w.u8(filter_channel_);
    w.pod(sector_);
    w.pod(ready_);
    w.u64(next_sector_);
    w.size(queue_.size());
    for (const Response& q : queue_) {
        w.u8(q.irq);
        w.vec(q.bytes);
        w.u64(q.due);
        w.boolean(q.sector);
    }
    xa_.save_state(w);
    w.pod(atv_pending_);
    w.pod(atv_);
    w.boolean(muted_);
    w.boolean(xa_muted_);
    w.end();
}

void CdRom::load_state(psx::StateReader& r) {
    r.begin(psx::state_tag("CDRM"), 1);
    now_ = r.u64();
    index_ = r.u8();
    irq_enable_ = r.u8();
    irq_flags_ = r.u8();
    hold_until_ = r.u64();
    r.deque(params_, kMaxFifo);
    r.deque(response_, kMaxFifo);
    r.vec(data_, Disc::kRawSector);
    data_pos_ = r.size(data_.size(), 0);
    data_loaded_ = r.boolean();
    mode_ = r.u8();
    stat_ = r.u8();
    setloc_lba_ = r.u32();
    read_lba_ = r.u32();
    setloc_pending_ = r.boolean();
    reading_ = r.boolean();
    filter_file_ = r.u8();
    filter_channel_ = r.u8();
    r.pod(sector_);
    r.pod(ready_);
    next_sector_ = r.u64();
    const size_t n = r.size(kMaxQueue);
    queue_.clear();
    for (size_t i = 0; i < n; ++i) {
        Response q;
        q.irq = r.u8();
        r.vec(q.bytes, kMaxFifo);
        q.due = r.u64();
        q.sector = r.boolean();
        queue_.push_back(std::move(q));
    }
    xa_.load_state(r);
    r.pod(atv_pending_);
    r.pod(atv_);
    muted_ = r.boolean();
    xa_muted_ = r.boolean();
    r.end();
    // Log-only edge tracker (not device state, not serialized): re-sniff from
    // the next sector so a load mid-stream cannot miss the start edge.
    xa_active_ = false;
}

}  // namespace hle
