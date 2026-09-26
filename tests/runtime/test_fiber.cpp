// Fiber switching: ping-pong between two fibers and the thread, deep stacks, exceptions on a
// fiber, and save-state snapshots (capture / restore on the same stack addresses).

#include <psx/fiber.hpp>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
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

void test_switching() {
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
}

// ---- Exceptions stay on their fiber (unwinding checks the stack bounds on Windows) ----------

void throw_deep(int n) {
    if (n == 0) throw std::runtime_error("boom");
    throw_deep(n - 1);
}

void run_thrower(void* arg) {
    auto* caught = static_cast<std::string*>(arg);
    try {
        throw_deep(50);
    } catch (const std::exception& e) {
        *caught = e.what();
    }
    main_fiber->resume();
    std::abort();
}

void test_exceptions() {
    std::string caught;
    auto f = psx::Fiber::create(run_thrower, &caught);
    f->resume();
    CHECK(caught == "boom");
    // And on the thread's own stack afterwards.
    try {
        throw_deep(5);
    } catch (const std::exception& e) {
        caught = std::string("thread ") + e.what();
    }
    CHECK(caught == "thread boom");
}

// ---- Snapshots -------------------------------------------------------------------------------

/// Counts on its own stack: each resume adds one and reports the running total through `out`.
void run_counter(void* arg) {
    auto* out = static_cast<int*>(arg);
    volatile int local = 0;  // lives on the fiber stack: restored with it
    for (;;) {
        local = local + 1;
        *out = local * 10 + deep(3);  // deep(3) == 3: frames pushed and popped on this stack
        main_fiber->resume();
    }
}

void test_snapshots() {
    if (!psx::Fiber::snapshots_supported()) {
        std::puts("runtime.fiber: snapshots not supported by this backend (skipped)");
        return;
    }
    int out = 0;
    auto f = psx::Fiber::create(run_counter, &out);
    f->resume();
    f->resume();
    CHECK(out == 23);
    const psx::Fiber::Image at2 = f->capture();  // suspended with local == 2
    CHECK(!at2.data.empty() && !at2.context.empty());
    f->resume();
    f->resume();
    CHECK(out == 43);

    // A running fiber and the thread cannot be captured.
    bool threw = false;
    try {
        (void)main_fiber->capture();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    // The stack is still owned by `f`: restoring over it is refused.
    threw = false;
    try {
        (void)psx::Fiber::restore(at2);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    // Destroyed, its stack goes to the free list; a new fiber of the same size reuses it, and a
    // restore then has to wait for that one to go too.
    const uint64_t base = at2.stack_base;
    f.reset();
    auto other = psx::Fiber::create(run_counter, &out);
    CHECK(other->capture().stack_base == base);
    threw = false;
    try {
        (void)psx::Fiber::restore(at2);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    other.reset();

    // Restored: continues after the second resume, on the same addresses, as many times as asked.
    for (int round = 0; round < 3; ++round) {
        auto g = psx::Fiber::restore(at2);
        CHECK(g->capture().stack_base == base);
        g->resume();
        CHECK(out == 33);
        g->resume();
        CHECK(out == 43);
    }

    // A fiber captured before it ever ran starts from its entry when restored.
    auto fresh = psx::Fiber::create(run_counter, &out);
    const psx::Fiber::Image unstarted = fresh->capture();
    fresh.reset();
    out = 0;
    auto h = psx::Fiber::restore(unstarted);
    h->resume();
    CHECK(out == 13);

    // Images from another process / backend are refused.
    psx::Fiber::Image bad = at2;
    bad.context.pop_back();
    threw = false;
    try {
        (void)psx::Fiber::restore(bad);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

}  // namespace

int main() {
    test_switching();
    test_exceptions();
    test_snapshots();
    std::puts("runtime.fiber: ok");
    return 0;
}
