#include "save_state.hpp"

#include "bios/bios.hpp"
#include "hw/mmio.hpp"
#include "system.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace hle {

namespace {

constexpr uint32_t kFormatVersion = 1;

void apply(const Guest& g, std::span<const uint8_t> state, const std::function<void(psx::StateReader&)>& host) {
    psx::StateReader r(state);
    r.begin(psx::state_tag("DCBS"), kFormatVersion);
    if (r.u64() != state_session()) r.fail("the state was made by another run of the game");
    r.end();
    g.machine.load_state(r);
    g.mmio.load_state(r);
    g.bios.load_state(r);
    g.system.load_state(r);  // last: replaces the fibers
    if (host) host(r);
    if (!r.at_end()) r.fail("unexpected data after the last chunk");
}

}  // namespace

uint64_t state_session() {
    static const uint64_t session = [] {
        std::random_device rd;
        const uint64_t t = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return (static_cast<uint64_t>(rd()) << 32 | rd()) ^ t;
    }();
    return session;
}

std::vector<uint8_t> save_guest(const Guest& g, const std::function<void(psx::StateWriter&)>& host) {
    std::string why;
    if (!g.system.can_save_state(&why)) throw psx::StateError(why);
    psx::StateWriter w;
    w.begin(psx::state_tag("DCBS"), kFormatVersion);
    w.u64(state_session());
    w.end();
    g.machine.save_state(w);
    g.mmio.save_state(w);
    g.bios.save_state(w);
    g.system.save_state(w);
    if (host) host(w);
    return w.take();
}

void load_guest(const Guest& g, std::span<const uint8_t> state, const std::function<void(psx::StateReader&)>& host) {
    std::string why;
    if (!g.system.can_save_state(&why)) throw psx::StateError(why);
    // Loading is not atomic (devices are overwritten one by one): keep the current machine to
    // put back if the state turns out to be bad halfway through.
    const std::vector<uint8_t> backup = save_guest(g, {});
    try {
        apply(g, state, host);
    } catch (const std::exception& e) {
        try {
            apply(g, backup, {});
        } catch (const std::exception& e2) {
            std::fprintf(stderr, "[state] cannot restore the machine after a failed load (%s): %s\n", e.what(), e2.what());
            std::abort();
        }
        g.system.resync_pacing();
        throw psx::StateError(e.what());
    }
    g.system.resync_pacing();
}

}  // namespace hle
