// Input record / replay (see input_log.hpp for the file format).

#include "input_log.hpp"

#include "settings.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace platform {

namespace {

/// Whitespace-separated tokens of `line` up to an optional '#' comment.
std::vector<std::string_view> tokens(std::string_view line) {
    if (const size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    std::vector<std::string_view> out;
    size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '\r')) ++pos;
        size_t end = pos;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r') ++end;
        if (end > pos) out.push_back(line.substr(pos, end - pos));
        pos = end;
    }
    return out;
}

/// Whole-token unsigned parse in `base`.
template <typename T>
bool parse_number(std::string_view s, T* out, int base = 10) {
    if (s.empty()) return false;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), *out, base);
    return ec == std::errc() && ptr == s.data() + s.size();
}

std::string event_line(uint64_t frame, InputState s) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%llu %04X%s", static_cast<unsigned long long>(frame), s.pad, s.any_press ? " A" : "");
    std::string line = buf;
    const std::string names = pad_button_names(s.pad);
    if (!names.empty()) line += " # " + names;
    return line;
}

std::string header_line(std::string_view game_id) {
    return std::string(kInputLogMagic) + " " + std::to_string(kInputLogVersion) + " " + std::string(game_id);
}

}  // namespace

std::string pad_button_names(uint16_t pad) {
    std::string names;
    for (const PadButtonInfo& info : kPadButtons) {
        if (pad & info.bit) continue;
        if (!names.empty()) names += '+';
        names += info.name;
    }
    return names;
}

// ---------------------------------------------------------------------------------------------
// InputRecording
// ---------------------------------------------------------------------------------------------

std::optional<InputRecording> InputRecording::parse(std::string_view text, std::string_view game_id, std::string* error) {
    const auto fail = [&](size_t line_no, const std::string& why) -> std::optional<InputRecording> {
        if (error) *error = line_no ? "line " + std::to_string(line_no) + ": " + why : why;
        return std::nullopt;
    };
    InputRecording rec;
    bool have_header = false, have_end = false;
    uint64_t checkpoint = 0, end = 0;
    size_t line_no = 0, pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++line_no;
        const std::vector<std::string_view> t = tokens(line);
        if (t.empty()) continue;
        if (!have_header) {
            if (t[0] != kInputLogMagic) return fail(0, "not a DCB input recording (missing '" + std::string(kInputLogMagic) + "' header)");
            int version = 0;
            if (t.size() != 3 || !parse_number(t[1], &version)) return fail(line_no, "malformed header");
            if (version != kInputLogVersion)
                return fail(0, "format version " + std::string(t[1]) + " is not supported (expected " +
                                   std::to_string(kInputLogVersion) + ")");
            if (t[2] != game_id)
                return fail(0, "recorded for " + std::string(t[2]) + ", but this build runs " + std::string(game_id));
            rec.game_id_ = std::string(t[2]);
            have_header = true;
            continue;
        }
        if (have_end) return fail(line_no, "data after 'end'");
        if (t[0] == "t" || t[0] == "end") {
            uint64_t frame = 0;
            if (t.size() != 2 || !parse_number(t[1], &frame)) return fail(line_no, "malformed '" + std::string(t[0]) + "' line");
            if (t[0] == "t") {
                checkpoint = std::max(checkpoint, frame);
            } else {
                have_end = true;
                end = frame;
            }
            continue;
        }
        InputEvent ev;
        if (t.size() < 2 || t.size() > 3 || !parse_number(t[0], &ev.frame) || t[1].size() != 4 ||
            !parse_number(t[1], &ev.state.pad, 16))
            return fail(line_no, "expected '<frame> <pad hex> [A]'");
        if (t.size() == 3) {
            if (t[2] != "A") return fail(line_no, "unknown flag '" + std::string(t[2]) + "'");
            ev.state.any_press = true;
        }
        if (!rec.events_.empty() && ev.frame <= rec.events_.back().frame) return fail(line_no, "frames must increase");
        rec.events_.push_back(ev);
    }
    if (!have_header) return fail(0, "not a DCB input recording (empty file)");
    const uint64_t after_last = rec.events_.empty() ? 0 : rec.events_.back().frame + 1;
    if (have_end && end < after_last) return fail(0, "'end " + std::to_string(end) + "' precedes the last event");
    rec.end_ = have_end ? end : std::max(checkpoint, after_last);
    return rec;
}

std::optional<InputRecording> InputRecording::load(const std::filesystem::path& path, std::string_view game_id,
                                                   std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path.string();
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    std::string why;
    auto rec = parse(text.str(), game_id, &why);
    if (!rec && error) *error = path.string() + ": " + why;
    return rec;
}

std::string InputRecording::to_string() const {
    std::string out = header_line(game_id_) + "\n";
    for (const InputEvent& ev : events_) out += event_line(ev.frame, ev.state) + "\n";
    out += "end " + std::to_string(end_) + "\n";
    return out;
}

std::optional<InputState> InputRecording::at(uint64_t frame) const {
    if (frame >= end_) return std::nullopt;
    const auto it = std::upper_bound(events_.begin(), events_.end(), frame,
                                     [](uint64_t f, const InputEvent& ev) { return f < ev.frame; });
    if (it == events_.begin()) return InputState{};
    const InputEvent& ev = *(it - 1);
    return InputState{ev.state.pad, ev.state.any_press && ev.frame == frame};
}

// ---------------------------------------------------------------------------------------------
// InputRecorder
// ---------------------------------------------------------------------------------------------

InputRecorder::InputRecorder(const std::filesystem::path& path, std::string_view game_id)
    : path_(path), game_id_(game_id) {
    file_ = std::fopen(path.string().c_str(), "wb");
    if (!file_) throw std::runtime_error("cannot create input recording " + path.string());
    write_line(header_line(game_id));
    std::fflush(file_);
}

InputRecorder::~InputRecorder() { close(); }

void InputRecorder::write_line(const std::string& line) {
    std::fputs(line.c_str(), file_);
    std::fputc('\n', file_);
}

void InputRecorder::record(uint64_t frame, InputState state) {
    if (!file_ || (started_ && frame < next_frame_)) return;
    if (!started_ || state.pad != last_pad_ || state.any_press) {
        write_line(event_line(frame, state));
        log_.push_back({frame, state});
        ++events_;
        last_pad_ = state.pad;
        last_written_ = frame + 1;
    }
    if (!started_) last_flush_ = frame;
    started_ = true;
    next_frame_ = frame + 1;
    if (frame - last_flush_ >= kFlushFrames) {
        flush();
        last_flush_ = frame;
    }
}

void InputRecorder::flush() {
    if (!file_) return;
    if (next_frame_ >= last_written_ + kCheckpointFrames) {
        write_line("t " + std::to_string(next_frame_));
        last_written_ = next_frame_;
    }
    std::fflush(file_);
}

void InputRecorder::rewind(uint64_t frame) {
    if (!file_ || !started_ || frame >= next_frame_) return;
    while (!log_.empty() && log_.back().frame >= frame) log_.pop_back();
    // The log only grows forward: write it again, up to the new present.
    std::fclose(file_);
    file_ = std::fopen(path_.string().c_str(), "wb");
    if (!file_) {
        std::fprintf(stderr, "[input] cannot rewrite the input recording %s\n", path_.string().c_str());
        return;
    }
    write_line(header_line(game_id_));
    for (const InputEvent& ev : log_) write_line(event_line(ev.frame, ev.state));
    std::fflush(file_);
    started_ = !log_.empty();
    last_pad_ = log_.empty() ? 0xFFFF : log_.back().state.pad;
    last_written_ = log_.empty() ? 0 : log_.back().frame + 1;
    next_frame_ = frame;
    last_flush_ = frame;
    events_ = log_.size();
}

void InputRecorder::close() {
    if (!file_) return;
    write_line("end " + std::to_string(next_frame_));
    const bool failed = std::ferror(file_) != 0;
    if (std::fclose(file_) != 0 || failed) std::fprintf(stderr, "[input] error while writing the input recording\n");
    file_ = nullptr;
}

// ---------------------------------------------------------------------------------------------
// InputLog
// ---------------------------------------------------------------------------------------------

InputLog::InputLog(std::optional<InputRecording> replay, std::unique_ptr<InputRecorder> recorder, bool exit_after_replay)
    : replay_(std::move(replay)), recorder_(std::move(recorder)), exit_after_replay_(exit_after_replay) {}

InputLog InputLog::from_env(std::string_view game_id) {
    const auto env = [](const char* name) -> std::string {
        const char* v = std::getenv(name);
        return v ? v : "";
    };
    std::optional<InputRecording> replay;
    if (const std::string path = env("DCB_REPLAY"); !path.empty()) {
        std::string error;
        replay = InputRecording::load(path, game_id, &error);
        if (!replay) throw std::runtime_error("DCB_REPLAY: " + error);
        std::fprintf(stderr, "[input] replaying %s: %zu events, frames 0-%llu\n", path.c_str(), replay->events().size(),
                     static_cast<unsigned long long>(replay->end_frame()));
    }
    std::unique_ptr<InputRecorder> recorder;
    if (const std::string path = env("DCB_RECORD"); !path.empty()) {
        recorder = std::make_unique<InputRecorder>(path, game_id);
        std::fprintf(stderr, "[input] recording to %s\n", path.c_str());
    }
    return InputLog(std::move(replay), std::move(recorder), !env("DCB_REPLAY_EXIT").empty());
}

void InputLog::apply(uint64_t frame, uint16_t& pad, bool& any_press) {
    if (replay_ && !replay_done_) {
        if (const std::optional<InputState> s = replay_->at(frame)) {
            pad = s->pad;
            any_press = s->any_press;
        } else {
            replay_done_ = true;
            std::fprintf(stderr, "[input] replay finished at frame %llu; input returns to the host\n",
                         static_cast<unsigned long long>(frame));
            if (exit_after_replay_) std::exit(0);  // static destructors close an active recording
        }
    }
    if (recorder_) recorder_->record(frame, InputState{pad, any_press});
}

void InputLog::rewind(uint64_t frame) {
    if (replay_) replay_done_ = replay_done_ && frame >= replay_->end_frame();
    if (recorder_) recorder_->rewind(frame);
}

}  // namespace platform
