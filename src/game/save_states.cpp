#include "save_states.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace dcb {

namespace {

/// "N[,N...]" from the environment (unparsable items are skipped).
std::vector<uint64_t> env_frames(const char* name) {
    std::vector<uint64_t> out;
    const char* s = std::getenv(name);
    while (s && *s) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(s, &end, 10);
        if (end != s) out.push_back(v);
        s = (end && end != s) ? end : s + 1;
        while (*s == ',' || *s == ' ') ++s;
    }
    return out;
}

bool take(std::vector<uint64_t>& frames, uint64_t now) {
    const auto it = std::find(frames.begin(), frames.end(), now);
    if (it == frames.end()) return false;
    frames.erase(it);
    return true;
}

}  // namespace

void HostFrameState::save_state(psx::StateWriter& w) const {
    w.begin(psx::state_tag("HOST"), 1);
    w.u64(pad_frame);
    w.u64(mdec_seen);
    w.u64(movie_until);
    w.u64(skip_until);
    w.end();
}

void HostFrameState::load_state(psx::StateReader& r) {
    r.begin(psx::state_tag("HOST"), 1);
    pad_frame = r.u64();
    mdec_seen = r.u64();
    movie_until = r.u64();
    skip_until = r.u64();
    r.end();
}

SaveStates::SaveStates(const hle::Guest& guest, HostFrameState& frame_state, platform::InputLog& input_log,
                       platform::Platform& host)
    : guest_(guest), frame_state_(frame_state), input_log_(input_log), host_(host),
      save_at_(env_frames("DCB_STATE_SAVE_AT")), load_at_(env_frames("DCB_STATE_LOAD_AT")),
      dump_at_(env_frames("DCB_STATE_DUMP_AT")) {
    const std::vector<uint64_t> exit_at = env_frames("DCB_EXIT_AT");
    if (!exit_at.empty()) exit_at_ = exit_at.front();
    const std::vector<uint64_t> stress = env_frames("DCB_STATE_STRESS");
    if (!stress.empty() && stress.front() > 0) stress_every_ = stress.front();
}

void SaveStates::notify(const std::string& text) {
    std::fprintf(stderr, "[state] frame %llu: %s\n", static_cast<unsigned long long>(frames_), text.c_str());
    host_.show_message(text);
}

std::string SaveStates::save_to(std::vector<uint8_t>& out) {
    try {
        out = hle::save_guest(guest_, [this](psx::StateWriter& w) { frame_state_.save_state(w); });
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

std::string SaveStates::load_from(const std::vector<uint8_t>& state) {
    HostFrameState loaded;
    try {
        hle::load_guest(guest_, state, [&](psx::StateReader& r) { loaded.load_state(r); });
    } catch (const std::exception& e) {
        return e.what();
    }
    frame_state_ = loaded;
    input_log_.rewind(loaded.pad_frame);
    host_.clear_audio();  // queued sound belongs to the replaced timeline
    return {};
}

bool SaveStates::occupied(int slot) const {
    return slot >= 0 && slot < kSlots && !slots_[static_cast<size_t>(slot)].empty();
}

const SaveStates::Thumbnail& SaveStates::thumbnail(int slot) const {
    static const Thumbnail empty;
    if (slot < 0 || slot >= kSlots) return empty;
    return thumbs_[static_cast<size_t>(slot)];
}

void SaveStates::set_thumbnail(int slot, Thumbnail thumb) {
    if (slot < 0 || slot >= kSlots) return;
    thumbs_[static_cast<size_t>(slot)] = std::move(thumb);
}

void SaveStates::select_slot(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    slot_ = slot;
    notify("Slot " + std::to_string(slot_ + 1));
}

bool SaveStates::save(int slot) {
    const std::string name = "State " + std::to_string(slot + 1);
    const auto t0 = std::chrono::steady_clock::now();
    if (const std::string error = save_to(slots_[static_cast<size_t>(slot)]); !error.empty()) {
        notify(name + ": " + error);
        return false;
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "[state] %s: %zu KB in %lld us, %s\n", name.c_str(),
                 slots_[static_cast<size_t>(slot)].size() / 1024, static_cast<long long>(us),
                 guest_.system.describe_tasks().c_str());
    notify(name + " saved");
    return true;
}

bool SaveStates::load(int slot) {
    const std::vector<uint8_t>& state = slots_[static_cast<size_t>(slot)];
    const std::string name = "State " + std::to_string(slot + 1);
    if (state.empty()) {
        notify("No state in slot " + std::to_string(slot + 1));
        return false;
    }
    const std::string before = guest_.system.describe_tasks();
    const auto t0 = std::chrono::steady_clock::now();
    if (const std::string error = load_from(state); !error.empty()) {
        notify(name + ": " + error);
        return false;
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "[state] %s: loaded in %lld us; before: %s; now: %s\n", name.c_str(), static_cast<long long>(us),
                 before.c_str(), guest_.system.describe_tasks().c_str());
    notify(name + " loaded");
    return true;
}

uint64_t SaveStates::digest() const {
    // CPU registers, guest time, RAM and scratchpad (the "CPU " chunk has no padding bytes), and
    // VRAM: what the game computed and what it shows. FNV-1a, 64-bit.
    psx::StateWriter w;
    guest_.machine.save_state(w);
    uint64_t h = 0xCBF29CE484222325ull;
    const auto mix = [&h](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    };
    mix(w.data().data(), w.data().size());
    mix(reinterpret_cast<const uint8_t*>(guest_.mmio.gpu().vram()), size_t{1024} * 512 * sizeof(uint16_t));
    return h;
}

bool SaveStates::stress() {
    // Save at a boundary, run `stress_every_` frames and fingerprint the machine; load, run the
    // same frames again and require the same fingerprint; then start over from there.
    if (stress_state_.empty() || frames_ == stress_mark_) {
        if (!stress_state_.empty()) {
            const uint64_t d = digest();
            if (!stress_replaying_) {
                stress_digest_ = d;
                if (const std::string error = load_from(stress_state_); !error.empty()) {
                    std::fprintf(stderr, "[state] stress: load failed at frame %llu: %s\n",
                                 static_cast<unsigned long long>(frames_), error.c_str());
                    std::abort();
                }
                stress_replaying_ = true;
                stress_mark_ = frames_ + stress_every_;
                return true;
            }
            if (d != stress_digest_) {
                std::fprintf(stderr, "[state] stress: MISMATCH at frame %llu (pad frame %llu) after replaying %llu frames\n",
                             static_cast<unsigned long long>(frames_), static_cast<unsigned long long>(frame_state_.pad_frame),
                             static_cast<unsigned long long>(stress_every_));
                std::abort();
            }
            ++stress_checks_;
            if (stress_checks_ % 500 == 0)
                std::fprintf(stderr, "[state] stress: %llu save/load checks passed (pad frame %llu), %s\n",
                             static_cast<unsigned long long>(stress_checks_),
                             static_cast<unsigned long long>(frame_state_.pad_frame), guest_.system.describe_tasks().c_str());
        }
        if (const std::string error = save_to(stress_state_); !error.empty()) {
            std::fprintf(stderr, "[state] stress: save failed at frame %llu: %s\n", static_cast<unsigned long long>(frames_),
                         error.c_str());
            std::abort();
        }
        stress_replaying_ = false;
        stress_mark_ = frames_ + stress_every_;
    }
    return false;
}

bool SaveStates::handle(uint32_t commands) {
    bool loaded = false;
    if (commands & platform::kNextStateSlot) {
        slot_ = (slot_ + 1) % kSlots;
        notify("Slot " + std::to_string(slot_ + 1));
    }
    if (commands & platform::kSaveState) save(slot_);
    if (commands & platform::kLoadState) loaded |= load(slot_);
    // Debug triggers: a save and a load at the same count happen in that order.
    if (take(save_at_, frames_)) save(slot_);
    if (take(load_at_, frames_)) loaded |= load(slot_);
    // Offline analysis: dump the slot bytes for tools/re/scan_state_ptrs.py.
    if (take(dump_at_, frames_)) {
        if (const char* path = std::getenv("DCB_STATE_DUMP_PATH")) {
            if (FILE* f = std::fopen(path, "wb")) {
                const auto& bytes = slots_[static_cast<size_t>(slot_)];
                std::fwrite(bytes.data(), 1, bytes.size(), f);
                std::fclose(f);
                std::fprintf(stderr, "[state] frame %llu: dumped slot %d (%zu KB) to %s\n",
                             static_cast<unsigned long long>(frames_), slot_ + 1, bytes.size() / 1024, path);
            }
        }
    }
    if (stress_every_) loaded |= stress();
    return loaded;
}

SaveStates::~SaveStates() {
    if (stress_every_)
        std::fprintf(stderr, "[state] stress: %llu save/load checks passed, every %llu frame(s)\n",
                     static_cast<unsigned long long>(stress_checks_), static_cast<unsigned long long>(stress_every_));
}

}  // namespace dcb
