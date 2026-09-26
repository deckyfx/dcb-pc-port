// Input record / replay: change-only encoding, file round trip, frame lookup, truncated logs
// (crash before close) and rejection of foreign or malformed files.

#include "input_log.hpp"
#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

namespace {

namespace fs = std::filesystem;
using namespace platform;

constexpr const char* kGame = "SLPS-03101";

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

fs::path temp_file(const char* name) { return fs::temp_directory_path() / (std::string("dcb_test_input_") + name); }

/// Parse or fail the test; returns the recording.
InputRecording must_parse(const std::string& text) {
    std::string error;
    auto rec = InputRecording::parse(text, kGame, &error);
    if (!rec) std::fprintf(stderr, "unexpected parse error: %s\n", error.c_str());
    CHECK(rec.has_value());
    return *rec;
}

/// Expect a parse failure whose message contains `needle`.
void must_reject(const std::string& text, const std::string& needle) {
    std::string error;
    CHECK(!InputRecording::parse(text, kGame, &error));
    if (error.find(needle) == std::string::npos) std::fprintf(stderr, "error '%s' lacks '%s'\n", error.c_str(), needle.c_str());
    CHECK(error.find(needle) != std::string::npos);
}

uint16_t pressed(uint16_t buttons) { return static_cast<uint16_t>(0xFFFF & ~buttons); }

void test_button_names() {
    CHECK(pad_button_names(0xFFFF).empty());
    CHECK(pad_button_names(pressed(Start)) == "Start");
    CHECK(pad_button_names(pressed(Up | Cross)) == "Up+Cross");
}

void test_recorder_writes_changes_only() {
    const fs::path path = temp_file("changes.txt");
    {
        InputRecorder rec(path, kGame);
        for (uint64_t f = 0; f < 100; ++f) {
            InputState s;
            if (f >= 10 && f < 16) s.pad = pressed(Start);
            if (f == 40) s.any_press = true;               // pulse, pad unchanged
            if (f >= 50 && f < 52) s.pad = pressed(Left | Circle);
            rec.record(f, s);
        }
        CHECK(rec.events_written() == 6);  // 0, 10, 16, 40, 50, 52
    }
    const std::string text = read_file(path);
    CHECK(text.rfind("DCB-INPUT 1 SLPS-03101\n", 0) == 0);
    CHECK(text.find("10 FFF7 # Start\n") != std::string::npos);
    CHECK(text.find("40 FFFF A\n") != std::string::npos);
    CHECK(text.find("end 100\n") != std::string::npos);

    const InputRecording rec = must_parse(text);
    CHECK(rec.events().size() == 6);
    CHECK(rec.end_frame() == 100);
    // Frame lookup reproduces exactly what was recorded, frame by frame.
    for (uint64_t f = 0; f < 100; ++f) {
        InputState want;
        if (f >= 10 && f < 16) want.pad = pressed(Start);
        if (f == 40) want.any_press = true;
        if (f >= 50 && f < 52) want.pad = pressed(Left | Circle);
        CHECK(rec.at(f) == want);
    }
    CHECK(!rec.at(100));   // past the end: input returns to the host
    CHECK(!rec.at(5000));
    fs::remove(path);
}

void test_round_trip() {
    const InputRecording a = must_parse("# a comment\nDCB-INPUT 1 SLPS-03101\n0 FFFF\n7 BFFF A # Cross\n9 FFFF\nend 20\n");
    const InputRecording b = must_parse(a.to_string());
    CHECK(a.events() == b.events());
    CHECK(a.end_frame() == b.end_frame());
    CHECK(b.game_id() == kGame);
    CHECK(b.at(7) == (InputState{pressed(Cross), true}));
    CHECK(b.at(8) == (InputState{pressed(Cross), false}));
    CHECK(b.at(19) == InputState{});
    // CRLF line endings and frames before the first event (nothing pressed).
    const InputRecording c = must_parse("DCB-INPUT 1 SLPS-03101\r\n5 FFEF\r\nend 8\r\n");
    CHECK(c.at(0) == InputState{});
    CHECK(c.at(5)->pad == pressed(Up));
    CHECK(!c.at(8));
}

void test_truncated_log() {
    // A crash leaves no 'end': the log ends at its last checkpoint or just after its last event.
    CHECK(must_parse("DCB-INPUT 1 SLPS-03101\n0 FFFF\n30 FFF7\n").end_frame() == 31);
    CHECK(must_parse("DCB-INPUT 1 SLPS-03101\n0 FFFF\n30 FFF7\nt 700\n").end_frame() == 700);
    CHECK(must_parse("DCB-INPUT 1 SLPS-03101\n").end_frame() == 0);

    // The recorder flushes periodically: the file is usable before close/destruction.
    const fs::path path = temp_file("flush.txt");
    {
        InputRecorder rec(path, kGame);
        for (uint64_t f = 0; f <= InputRecorder::kCheckpointFrames + InputRecorder::kFlushFrames; ++f)
            rec.record(f, InputState{f >= 5 ? pressed(Square) : static_cast<uint16_t>(0xFFFF), false});
        const InputRecording mid = must_parse(read_file(path));   // still open, as after an abort
        CHECK(mid.events().size() == 2);
        CHECK(mid.end_frame() > InputRecorder::kCheckpointFrames);  // a checkpoint was written
        CHECK(mid.at(InputRecorder::kCheckpointFrames)->pad == pressed(Square));
    }
    fs::remove(path);
}

void test_rejects_bad_files() {
    must_reject("", "not a DCB input recording");
    must_reject("P6\n320 240\n255\n", "not a DCB input recording");
    must_reject("DCB-INPUT 2 SLPS-03101\n", "format version 2 is not supported");
    must_reject("DCB-INPUT 1 SLUS-01328\n0 FFFF\n", "recorded for SLUS-01328");
    must_reject("DCB-INPUT 1\n", "malformed header");
    must_reject("DCB-INPUT 1 SLPS-03101\n0 FFFF\nabc FFFF\n", "line 3");
    must_reject("DCB-INPUT 1 SLPS-03101\n0 FFFFF\n", "line 2");
    must_reject("DCB-INPUT 1 SLPS-03101\n0 FFFF B\n", "unknown flag");
    must_reject("DCB-INPUT 1 SLPS-03101\n5 FFFF\n5 FFF7\n", "frames must increase");
    must_reject("DCB-INPUT 1 SLPS-03101\n5 FFFF\nend 3\n", "precedes the last event");
    must_reject("DCB-INPUT 1 SLPS-03101\nend 3\n4 FFFF\n", "data after 'end'");

    std::string error;
    CHECK(!InputRecording::load(temp_file("does_not_exist.txt"), kGame, &error));
    CHECK(error.find("cannot open") != std::string::npos);
}

void test_session_replay_then_host() {
    const InputRecording rec = must_parse("DCB-INPUT 1 SLPS-03101\n0 FFFF\n2 FFF7 A\nend 4\n");
    const fs::path path = temp_file("rerecord.txt");
    {
        InputLog log(rec, std::make_unique<InputRecorder>(path, kGame), false);
        CHECK(log.replaying() && log.recording());
        for (uint64_t f = 0; f < 6; ++f) {
            uint16_t pad = pressed(Triangle);  // host input: ignored while replaying
            bool any = false;
            log.apply(f, pad, any);
            if (f < 2) CHECK(pad == 0xFFFF && !any);
            if (f == 2) CHECK(pad == pressed(Start) && any);
            if (f == 3) CHECK(pad == pressed(Start) && !any);
            if (f >= 4) CHECK(pad == pressed(Triangle));  // replay over: host input again
        }
        CHECK(!log.replaying());
    }
    // Recording while replaying captures what the game was actually given.
    const InputRecording again = must_parse(read_file(path));
    CHECK(again.events().size() == 3);
    CHECK(again.at(2) == (InputState{pressed(Start), true}));
    CHECK(again.at(5)->pad == pressed(Triangle));
    CHECK(again.end_frame() == 6);
    fs::remove(path);
}

}  // namespace

int main() {
    test_button_names();
    test_recorder_writes_changes_only();
    test_round_trip();
    test_truncated_log();
    test_rejects_bad_files();
    test_session_replay_then_host();
    std::printf("input_log: all tests passed\n");
    return 0;
}
