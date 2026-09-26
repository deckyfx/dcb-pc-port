#pragma once
// CD-ROM controller (0x1F801800-0x1F801803) and DMA channel 3. libcd drives the drive itself
// (commands, responses, interrupt 2 per response and per sector), so this models the controller
// at its register interface, reading the original disc image. Responses and sectors arrive after
// realistic delays in guest cycles, so libcd's waits and callbacks see a working drive.
// Reference: psx-spx "CDROM Controller I/O Ports", "CDROM Controller Command Summary",
// "CDROM - Response/Data Queueing".

#include "cdrom/disc.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace hle {

class CdRom {
public:
    static constexpr uint32_t kBase = 0x1F801800, kEnd = 0x1F801804;

    /// `raise_irq2` requests the CD-ROM interrupt (I_STAT bit 2).
    explicit CdRom(std::function<void()> raise_irq2);

    void insert(std::unique_ptr<Disc> disc) { disc_ = std::move(disc); }

    uint8_t read(uint32_t phys);
    uint8_t read_reg(uint32_t phys);
    void write(uint32_t phys, uint8_t value);

    /// Advance to guest time `now` (cycles): deliver responses and sectors that are due.
    void tick(uint64_t now);

    /// DMA channel 3: take `count` words from the data FIFO.
    void dma_read(uint32_t* words, uint32_t count);

private:
    struct Response {
        uint8_t irq = 0;                  // INT1 data ready, INT2 complete, INT3 ack, INT4 end, INT5 error
        std::vector<uint8_t> bytes;
        uint64_t due = 0;                 // guest cycle when it may be delivered
        bool sector = false;              // INT1 of a read: loads the sector buffer when delivered
    };

    std::function<void()> raise_irq2_;
    std::unique_ptr<Disc> disc_;
    bool trace_ = false;  ///< DCB_TRACE_CD=1
    unsigned trace_regs_ = 0;
    uint64_t now_ = 0;

    // Registers
    uint8_t index_ = 0;
    // Power-on state as the BIOS leaves it after booting a disc: CD interrupts enabled. libcd's
    // first command (CdlNop in CD_init) is sent before it programs the enable register itself.
    uint8_t irq_enable_ = 0x1F, irq_flags_ = 0;
    std::deque<uint8_t> params_, response_;
    std::vector<uint8_t> data_;           // data FIFO (current sector, after a BFRD request)
    size_t data_pos_ = 0;
    bool data_loaded_ = false;

    // Drive state
    uint8_t mode_ = 0;
    uint8_t stat_ = 0x02;                 // motor on
    uint32_t setloc_lba_ = 0, read_lba_ = 0;
    bool setloc_pending_ = false;
    bool reading_ = false;
    uint8_t filter_file_ = 0, filter_channel_ = 0;
    std::array<uint8_t, Disc::kRawSector> sector_{};    // last sector read from disc
    std::array<uint8_t, Disc::kRawSector> ready_{};     // sector announced by the last INT1
    uint64_t next_sector_ = 0;

    std::deque<Response> queue_;          // responses waiting for delivery (and for IRQ ack)

    void command(uint8_t cmd);
    void push(uint8_t irq, std::vector<uint8_t> bytes, uint64_t delay);
    void deliver_due();
    void read_sector();
    uint64_t sector_period() const;
    uint8_t status_register() const;
};

}  // namespace hle
