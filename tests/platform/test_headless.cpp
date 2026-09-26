// Headless backend defaults: always running, nothing pressed, present/audio are no-ops.

#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

int main() {
    auto p = platform::make_headless();
    CHECK(p != nullptr);
    CHECK(p->pump_events());
    CHECK(p->pad_buttons(0) == 0xFFFF);
    CHECK(p->pad_buttons(1) == 0xFFFF);

    std::vector<uint16_t> vram(static_cast<size_t>(platform::kVramWidth) * platform::kVramHeight, 0x7FFF);
    p->present(vram.data(), platform::DisplayArea{});
    const int16_t samples[4] = {1, -1, 2, -2};
    p->queue_audio(samples, 2);
    CHECK(p->pump_events());

    // Button bits match the PS1 digital pad layout.
    CHECK(platform::Select == 0x0001 && platform::Start == 0x0008 && platform::Up == 0x0010);
    CHECK(platform::Left == 0x0080 && platform::L2 == 0x0100 && platform::R1 == 0x0800);
    CHECK(platform::Triangle == 0x1000 && platform::Cross == 0x4000 && platform::Square == 0x8000);

    std::puts("platform.headless: ok");
    return 0;
}
