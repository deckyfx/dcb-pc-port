// Fiber switching: ping-pong between two fibers and the thread, and deep stacks.

#include <psx/fiber.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);   \
            std::exit(1);                                                                   \
        }                                                                                   \
    } while (0)

namespace {

std::string trace;
psx::Fiber* main_fiber = nullptr;
psx::Fiber* a = nullptr;
psx::Fiber* b = nullptr;

int deep(int n) {
    volatile char pad[256];
    pad[0] = static_cast<char>(n);
    return n == 0 ? pad[0] : deep(n - 1) + 1;
}

void run_a(void*) {
    trace += "a1 ";
    b->resume();
    trace += "a2 ";
    CHECK(deep(1000) == 1000);  // ~256 KB of frames on the fiber stack
    main_fiber->resume();
    std::abort();  // never resumed again
}

void run_b(void*) {
    trace += "b1 ";
    a->resume();
    std::abort();
}

}  // namespace

int main() {
    main_fiber = psx::Fiber::current();
    auto fa = psx::Fiber::create(run_a, nullptr);
    auto fb = psx::Fiber::create(run_b, nullptr);
    a = fa.get();
    b = fb.get();

    trace += "m1 ";
    a->resume();
    trace += "m2";
    CHECK(psx::Fiber::current() == main_fiber);
    CHECK(trace == "m1 a1 b1 a2 m2");
    std::puts("runtime.fiber: ok");
    return 0;
}
