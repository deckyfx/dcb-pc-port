#pragma once
// Save-state slots for the host loop: the F5/F6/F7 hotkeys, the on-screen notices, and debug
// triggers for scripted runs. States live in memory for this run only (docs/HOST_MAIN_LOOP.md).
//
//   DCB_STATE_SAVE_AT=<frame>[,<frame>...]   save to the selected slot once that many frames ran
//   DCB_STATE_LOAD_AT=<frame>[,<frame>...]   load the selected slot at those frame counts
//   DCB_RESET_AT=<frame>[,<frame>...]        reset to power-on (the menu's Reset game) at those
//                                            frame counts
//   DCB_EXIT_AT=<frame>                      quit cleanly once that many frames ran
//   DCB_STATE_STRESS=<n>                     self-check: save, run n frames, fingerprint the
//                                            machine, load, run them again, compare; repeat
//                                            (aborts on a mismatch; every boundary with n=1)
//   DCB_STATE_DUMP_AT=<frame>                write the slot's bytes to DCB_STATE_DUMP_PATH
//   DCB_STATE_DUMP_PATH=<file>               (debug: offline analysis of state contents)
// Frame counts are host frames (every resume_guest() since start, never rewound by a load), the
// same numbering as DCB_SNAPSHOT's frame_NNNNN files: after a load at M of a state saved at N,
// snapshot M+k shows what snapshot N+k showed in a run without the load.

#include "bios/bios.hpp"
#include "hw/mmio.hpp"
#include "save_state.hpp"
#include "system.hpp"

#include "input_log.hpp"
#include "platform.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dcb {

/// Host-loop values that decide what the game is given each frame (they follow the guest):
/// the pad frame counter (DCB_PAD_SCRIPT and DCB_RECORD/DCB_REPLAY index it) and the FMV-skip
/// state. Saved with every state.
struct HostFrameState {
    uint64_t pad_frame = 0;
    uint64_t mdec_seen = 0;
    uint64_t movie_until = 0;
    uint64_t skip_until = 0;

    void save_state(psx::StateWriter& w) const;
    void load_state(psx::StateReader& r);
};

class SaveStates {
public:
    static constexpr int kSlots = 4;

    SaveStates(const hle::Guest& guest, HostFrameState& frame_state, platform::InputLog& input_log,
               platform::Platform& host);

    /// Between frames (the game suspended): act on the save-state hotkeys in `commands` and on
    /// the debug triggers. Returns true when a state was loaded (the caller re-reads the display).
    bool handle(uint32_t commands);
    /// Call after each frame the game ran.
    void frame_done() { ++frames_; }
    /// DCB_EXIT_AT reached.
    bool exit_requested() const { return exit_at_ != 0 && frames_ >= exit_at_; }

    /// Save into / load from slot `slot` (0-based). They report through the platform notice.
    bool save(int slot);
    bool load(int slot);
    /// Back to power-on: loads the state captured when this object was made (right after
    /// System::start, before the first frame). Save slots and the memory card are untouched; a
    /// movie playing is dropped.
    bool reset();

    /// Slot inspection for the native menu (thumbnails, timestamps).
    bool occupied(int slot) const;
    int selected_slot() const { return slot_; }
    void select_slot(int slot);

    /// Small display-area capture for the menu's States page (set by the host
    /// loop after a successful save; cleared with the slot on load failure).
    struct Thumbnail {
        int width = 0, height = 0;
        std::vector<uint8_t> rgb;  ///< width*height*3, 8-bit RGB
        std::string saved_at;      ///< local time string, empty when unset
    };
    const Thumbnail& thumbnail(int slot) const;
    void set_thumbnail(int slot, Thumbnail thumb);

    ~SaveStates();
    SaveStates(const SaveStates&) = delete;
    SaveStates& operator=(const SaveStates&) = delete;

private:
    hle::Guest guest_;
    HostFrameState& frame_state_;
    platform::InputLog& input_log_;
    platform::Platform& host_;
    std::array<std::vector<uint8_t>, kSlots> slots_;
    std::array<Thumbnail, kSlots> thumbs_;
    std::vector<uint8_t> power_on_;  ///< the state before the first frame (reset())
    std::string power_on_error_;     ///< why power_on_ could not be captured
    int slot_ = 0;
    uint64_t frames_ = 0;
    std::vector<uint64_t> save_at_, load_at_, dump_at_, reset_at_;
    uint64_t exit_at_ = 0;

    // DCB_STATE_STRESS
    uint64_t stress_every_ = 0;
    std::vector<uint8_t> stress_state_;
    uint64_t stress_mark_ = 0, stress_digest_ = 0, stress_checks_ = 0;
    bool stress_replaying_ = false;

    void notify(const std::string& text);
    /// Save / load without notices; the error message, or empty on success.
    std::string save_to(std::vector<uint8_t>& out);
    std::string load_from(const std::vector<uint8_t>& state);
    /// Fingerprint of the guest (CPU, RAM, VRAM) for the stress check.
    uint64_t digest() const;
    /// One step of the DCB_STATE_STRESS check; true when it loaded a state.
    bool stress();
};

}  // namespace dcb
