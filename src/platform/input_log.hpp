#pragma once
// Input record / replay: a compact, change-only log of the pad state the game was given each
// VBLANK frame, written while playing (DCB_RECORD) and fed back later (DCB_REPLAY). Guest time
// is virtual, so replaying a log reproduces the run frame for frame. Pure C++, no SDL.
//
// File format (text, '\n' lines, '#' starts a comment):
//
//     DCB-INPUT 1 SLPS-03101        header: magic, format version, game id
//     0 FFFF                        <frame> <pad, 4 hex digits, active low> [A]
//     2000 FFFF A                   'A' = the "any key" pulse (FMV skip) fired on this frame
//     2000 FFF7 # Start             trailing comments name the pressed buttons (informational)
//     t 2400                        checkpoint: frames below 2400 are recorded
//     end 4210                      recording closed: frames below 4210 are recorded
//
// Events are written only when the pad state changes or the pulse fires; the state holds
// until the next event. A log cut short by a crash has no `end` line and ends at its last
// checkpoint (or just after its last event).

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platform {

inline constexpr std::string_view kInputLogMagic = "DCB-INPUT";
inline constexpr int kInputLogVersion = 1;

/// What the game is given on one frame.
struct InputState {
    uint16_t pad = 0xFFFF;   ///< active-low pad bits (0xFFFF = nothing pressed)
    bool any_press = false;  ///< the one-frame "any key" pulse
    bool operator==(const InputState&) const = default;
};

/// One change in a log: from `frame` on the pad is `state.pad` (the pulse applies to `frame` only).
struct InputEvent {
    uint64_t frame = 0;
    InputState state;
    bool operator==(const InputEvent&) const = default;
};

/// A loaded log: frame lookup over its change events.
class InputRecording {
public:
    /// Parse a log. Rejects (nullopt, reason in `*error`) a missing/unknown header, another
    /// format version, another game id, malformed lines and non-increasing frames.
    static std::optional<InputRecording> parse(std::string_view text, std::string_view game_id, std::string* error);

    /// Read and parse `path` (see parse); I/O failures are reported the same way.
    static std::optional<InputRecording> load(const std::filesystem::path& path, std::string_view game_id,
                                              std::string* error);

    /// Serialise (header, events, `end` line): parse(to_string()) round-trips.
    std::string to_string() const;

    /// State for `frame`, or nullopt at/after the end of the recording (input returns to the host).
    /// Frames before the first event read as nothing pressed.
    std::optional<InputState> at(uint64_t frame) const;

    /// First frame not covered by the recording.
    uint64_t end_frame() const { return end_; }
    const std::vector<InputEvent>& events() const { return events_; }
    const std::string& game_id() const { return game_id_; }

private:
    std::string game_id_;
    std::vector<InputEvent> events_;  ///< strictly increasing frames
    uint64_t end_ = 0;
};

/// Writes a log as the game runs. Call record() once per frame with increasing frame numbers.
/// Buffered output reaches the file every kFlushFrames frames and on close/destruction, so a
/// crash or abort loses at most the last flush interval.
class InputRecorder {
public:
    static constexpr uint64_t kFlushFrames = 60;         ///< flush period (about one second)
    static constexpr uint64_t kCheckpointFrames = 600;   ///< idle time before a `t` checkpoint line

    /// Create/truncate `path` and write the header. Throws std::runtime_error on failure.
    InputRecorder(const std::filesystem::path& path, std::string_view game_id);
    ~InputRecorder();
    InputRecorder(const InputRecorder&) = delete;
    InputRecorder& operator=(const InputRecorder&) = delete;

    /// Log `state` for `frame` (written only if it differs from the previous frame's pad or
    /// carries the pulse). Frames must increase; out-of-order calls are ignored.
    void record(uint64_t frame, InputState state);

    /// Push buffered lines to the file (adds a checkpoint when the log has been idle long).
    void flush();

    /// Write the `end` line and close. Idempotent; the destructor calls it.
    void close();

    /// Number of event lines written so far.
    size_t events_written() const { return events_; }

private:
    void write_line(const std::string& line);

    std::FILE* file_ = nullptr;
    bool started_ = false;
    uint16_t last_pad_ = 0xFFFF;
    uint64_t next_frame_ = 0;       ///< one past the last recorded frame
    uint64_t last_flush_ = 0;
    uint64_t last_written_ = 0;     ///< frame covered by the last event/checkpoint line
    size_t events_ = 0;
};

/// The per-run glue main() uses: configured from the environment, applied once per frame to
/// the pad state that is about to be given to the game.
///   DCB_RECORD=<file>     record every frame's final state
///   DCB_REPLAY=<file>     replace host input (keyboard, gamepad, DCB_PAD_SCRIPT, FMV skip) with
///                         the log until it ends, then hand input back to the host
///   DCB_REPLAY_EXIT=1     quit (exit status 0) when the replay ends, for scripted regressions
class InputLog {
public:
    /// Read the variables above. Throws std::runtime_error (with the reason) if the replay
    /// file is unusable or the recording cannot be created.
    static InputLog from_env(std::string_view game_id);

    InputLog() = default;
    InputLog(std::optional<InputRecording> replay, std::unique_ptr<InputRecorder> recorder, bool exit_after_replay);

    /// Per frame, just before the state is handed to the game: while replaying, overwrite
    /// `pad`/`any_press` with the recorded state; then record the result.
    void apply(uint64_t frame, uint16_t& pad, bool& any_press);

    bool replaying() const { return replay_.has_value() && !replay_done_; }
    bool recording() const { return recorder_ != nullptr; }

private:
    std::optional<InputRecording> replay_;
    std::unique_ptr<InputRecorder> recorder_;
    bool exit_after_replay_ = false;
    bool replay_done_ = false;
};

/// Pressed button names of an active-low pad value, '+'-joined ("" if none), e.g. "Start+Cross".
std::string pad_button_names(uint16_t pad);

}  // namespace platform
