#pragma once
// Minimal cooperative fibers: one native stack per guest task. Games that switch tasks by
// swapping stacks (saving registers and jumping into another task's code) map onto switching
// fibers, because every guest task owns a chain of native C frames.
//
// Linux/macOS: ucontext (makecontext/swapcontext) on mmap'd, lazily committed stacks with a guard
// page. Windows: the Fibers API (CreateFiber/SwitchToFiber). Single-threaded use only.

#include <cstddef>
#include <memory>

namespace psx {

class Fiber {
public:
    using Entry = void (*)(void* arg);

    /// The fiber currently running (the thread itself is adopted as the first fiber on first use).
    static Fiber* current();

    /// A new suspended fiber that runs `entry(arg)` when first resumed. `entry` must never return:
    /// a finished fiber switches away for the last time and is destroyed by someone else.
    static std::unique_ptr<Fiber> create(Entry entry, void* arg, size_t stack_bytes = 1u << 20);

    /// Suspend the current fiber and run this one. Returns when something resumes the caller.
    void resume();

    ~Fiber();
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;

    struct Impl;

private:
    explicit Fiber(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace psx
