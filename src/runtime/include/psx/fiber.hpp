#pragma once
// Minimal cooperative fibers: one native stack per guest task. Games that switch tasks by
// swapping stacks (saving registers and jumping into another task's code) map onto switching
// fibers, because every guest task owns a chain of native C frames.
//
// Backends:
//   Linux (glibc x86-64 / AArch64), macOS: ucontext (makecontext/swapcontext).
//   Windows x64 with GCC/Clang (MinGW): a small context switch of our own, which also swaps the
//     stack bounds in the thread information block (SEH unwinding and stack probes check them).
//   Other Windows builds (MSVC): the Fibers API (CreateFiber/SwitchToFiber).
// Stacks are reserved address space with a guard page at the bottom, committed as they grow
// (the MinGW backend commits them up front). Single-threaded use only.
//
// Save states (capture()/restore()) need the backend's saved context to be plain data at a known
// place: supported by the first two backends, not by Win32 Fibers (snapshots_supported()).
// A restored fiber must run on the very same stack addresses (the stack holds pointers into
// itself), so stacks are never unmapped: a destroyed fiber's stack goes to a free list, is reused
// by the next fiber of the same size, and restore() claims it back by address.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace psx {

class Fiber {
public:
    using Entry = void (*)(void* arg);

    /// The fiber currently running (the thread itself is adopted as the first fiber on first use).
    static Fiber* current();

    /// A new suspended fiber that runs `entry(arg)` when first resumed. `entry` must never return:
    /// a finished fiber switches away for the last time and is destroyed by someone else.
    /// `stack_bytes` is reserved address space; memory is committed as the stack grows.
    static std::unique_ptr<Fiber> create(Entry entry, void* arg, size_t stack_bytes = 1u << 20);

    /// Suspend the current fiber and run this one. Returns when something resumes the caller.
    void resume();

    ~Fiber();
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;

    // ---- Save states ---------------------------------------------------------------------------

    /// A suspended fiber: where its stack is, the live part of that stack, and its saved
    /// registers. Only meaningful in the process that captured it.
    struct Image {
        uint64_t stack_base = 0;     ///< lowest address of the stack mapping (guard page included)
        uint64_t stack_bytes = 0;    ///< size of the mapping
        uint64_t data_base = 0;      ///< `data` holds [data_base, stack_base + stack_bytes)
        std::vector<uint8_t> data;
        std::vector<uint8_t> context;  ///< backend register context (opaque bytes)
        uint64_t entry = 0, arg = 0;   ///< create() arguments, for a fiber that has not started yet
    };

    /// Whether this build's backend can capture and restore fibers.
    static bool snapshots_supported();
    /// Capture this fiber. It must be suspended (not current()) and own its stack (not the thread).
    Image capture() const;
    /// A fiber that continues exactly where the captured one was suspended, on the same stack
    /// addresses. The stack must be free (its previous owner destroyed): throws std::runtime_error
    /// otherwise, or when the image does not match this backend.
    static std::unique_ptr<Fiber> restore(const Image& image);

    struct Impl;

private:
    explicit Fiber(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace psx
