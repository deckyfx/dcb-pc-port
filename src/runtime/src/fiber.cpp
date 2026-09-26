#include <psx/fiber.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

// Backend selection (see fiber.hpp).
#if defined(_WIN32) && defined(__x86_64__) && defined(__GNUC__)
#define DCB_FIBER_WIN64_ASM 1
#elif defined(_WIN32)
#define DCB_FIBER_WIN32_FIBERS 1
#else
#define DCB_FIBER_UCONTEXT 1
#endif

namespace psx {

namespace {

Fiber* g_current = nullptr;
std::unique_ptr<Fiber> g_thread_fiber;  // the adopted thread

#if !DCB_FIBER_WIN32_FIBERS

/// A stack mapping: [base, base + bytes), guard page at `base`.
struct Stack {
    uintptr_t base = 0;
    size_t bytes = 0;
};

size_t page_size() {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

/// Released stacks, still mapped: fibers restored from a save state need their old addresses.
/// Never destroyed (fibers may be released during static destruction).
std::vector<Stack>& free_stacks() {
    static auto* stacks = new std::vector<Stack>;
    return *stacks;
}

Stack map_stack(size_t bytes) {
#ifdef _WIN32
    // Committed up front: stack probes and SEH only need the pages to exist.
    void* mem = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        std::fprintf(stderr, "[fiber] cannot allocate a %zu-byte stack\n", bytes);
        std::abort();
    }
    DWORD old = 0;
    VirtualProtect(mem, page_size(), PAGE_NOACCESS, &old);  // guard: overflow faults
#else
    void* mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
        std::fprintf(stderr, "[fiber] cannot map a %zu-byte stack\n", bytes);
        std::abort();
    }
    mprotect(mem, page_size(), PROT_NONE);  // stack overflow faults instead of corrupting memory
#endif
    return {reinterpret_cast<uintptr_t>(mem), bytes};
}

/// A stack of `bytes` usable bytes (plus the guard page): the most recently released one that
/// fits (its pages are the likeliest to be warm), else a new mapping.
Stack take_stack(size_t bytes) {
    const size_t page = page_size();
    bytes = (bytes + page - 1) / page * page + page;
    auto& pool = free_stacks();
    const auto it = std::find_if(pool.rbegin(), pool.rend(), [&](const Stack& s) { return s.bytes == bytes; });
    if (it != pool.rend()) {
        const Stack s = *it;
        pool.erase(std::next(it).base());
        return s;
    }
    return map_stack(bytes);
}

void release_stack(const Stack& s) {
    if (s.base) free_stacks().push_back(s);
}

/// Take the released stack at exactly `base`/`bytes` out of the pool.
bool claim_stack(uintptr_t base, size_t bytes) {
    auto& pool = free_stacks();
    const auto it = std::find_if(pool.begin(), pool.end(), [&](const Stack& s) { return s.base == base && s.bytes == bytes; });
    if (it == pool.end()) return false;
    pool.erase(it);
    return true;
}

/// Checks shared by the capturing backends: the saved stack pointer lies inside the stack.
Fiber::Image capture_stack(const Stack& stack, uintptr_t sp, size_t red_zone) {
    const uintptr_t lo = stack.base + page_size(), top = stack.base + stack.bytes;
    if (!stack.base) throw std::runtime_error("fiber capture: the thread's own fiber has no stack of its own");
    if (sp < lo || sp > top) throw std::runtime_error("fiber capture: saved stack pointer outside the stack");
    const uintptr_t from = std::max(lo, sp - red_zone);
    Fiber::Image image;
    image.stack_base = stack.base;
    image.stack_bytes = stack.bytes;
    image.data_base = from;
    image.data.assign(reinterpret_cast<const uint8_t*>(from), reinterpret_cast<const uint8_t*>(top));
    return image;
}

/// Claim the image's stack and copy its live bytes back. Returns the stack.
Stack restore_stack(const Fiber::Image& image) {
    const uintptr_t base = static_cast<uintptr_t>(image.stack_base);
    const size_t bytes = static_cast<size_t>(image.stack_bytes);
    const uintptr_t top = base + bytes;
    if (image.data_base < base + page_size() || image.data_base + image.data.size() != top)
        throw std::runtime_error("fiber restore: inconsistent stack image");
    if (!claim_stack(base, bytes))
        throw std::runtime_error("fiber restore: the stack is not free (its fiber still exists, or it is from another process)");
    std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(image.data_base)), image.data.data(), image.data.size());
    return {base, bytes};
}

#endif  // !DCB_FIBER_WIN32_FIBERS

}  // namespace

#if DCB_FIBER_WIN64_ASM
// ---------------------------------------------------------------------------------------------
// Windows x64 (GCC/Clang): our own switch. Saves the Win64 callee-saved state (rbx rbp rdi rsi
// r12-r15, xmm6-xmm15, MXCSR, x87 control word) and the thread's stack bounds from the TIB
// (StackBase gs:[08h], StackLimit gs:[10h], DeallocationStack gs:[1478h]) on the stack being
// left, then loads the other stack's. Everything is on the fiber's own stack, so a save state
// only needs the stack bytes and the stack pointer.
//
// Frame at a suspended fiber's sp (low to high): xmm6-15 (A0h), MXCSR (+A0h), FPU CW (+A4h),
// pad, DeallocationStack (+B0h), StackLimit (+B8h), StackBase (+C0h), r15 r14 r13 r12 rsi rdi
// rbx rbp (+C8h..+100h), return address (+108h).

extern "C" void dcb_fiber_switch(void** save_sp, void* load_sp);
extern "C" void dcb_fiber_start();

asm(R"(
    .text
    .p2align 4
    .globl dcb_fiber_switch
    .def dcb_fiber_switch; .scl 2; .type 32; .endef
dcb_fiber_switch:
    pushq %rbp
    pushq %rbx
    pushq %rdi
    pushq %rsi
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %gs:0x08
    pushq %gs:0x10
    pushq %gs:0x1478
    subq $0xB0, %rsp
    movaps %xmm6, 0x00(%rsp)
    movaps %xmm7, 0x10(%rsp)
    movaps %xmm8, 0x20(%rsp)
    movaps %xmm9, 0x30(%rsp)
    movaps %xmm10, 0x40(%rsp)
    movaps %xmm11, 0x50(%rsp)
    movaps %xmm12, 0x60(%rsp)
    movaps %xmm13, 0x70(%rsp)
    movaps %xmm14, 0x80(%rsp)
    movaps %xmm15, 0x90(%rsp)
    stmxcsr 0xA0(%rsp)
    fnstcw 0xA4(%rsp)
    movq %rsp, (%rcx)
    movq %rdx, %rsp
    movaps 0x00(%rsp), %xmm6
    movaps 0x10(%rsp), %xmm7
    movaps 0x20(%rsp), %xmm8
    movaps 0x30(%rsp), %xmm9
    movaps 0x40(%rsp), %xmm10
    movaps 0x50(%rsp), %xmm11
    movaps 0x60(%rsp), %xmm12
    movaps 0x70(%rsp), %xmm13
    movaps 0x80(%rsp), %xmm14
    movaps 0x90(%rsp), %xmm15
    ldmxcsr 0xA0(%rsp)
    fldcw 0xA4(%rsp)
    addq $0xB0, %rsp
    popq %gs:0x1478
    popq %gs:0x10
    popq %gs:0x08
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rsi
    popq %rdi
    popq %rbx
    popq %rbp
    ret

    .p2align 4
    .globl dcb_fiber_start
    .def dcb_fiber_start; .scl 2; .type 32; .endef
dcb_fiber_start:
    movq %r12, %rcx
    movq %r13, %rdx
    andq $-16, %rsp
    subq $32, %rsp
    call dcb_fiber_entry
    ud2
)");

struct Fiber::Impl {
    void* sp = nullptr;  ///< saved stack pointer while suspended
    Stack stack{};       ///< base 0: the adopted thread
    Entry entry = nullptr;
    void* arg = nullptr;
};

/// First code on a new fiber: entry and argument come from its initial frame (r12, r13), so a
/// fiber restored before it ever ran needs nothing but its stack.
extern "C" void dcb_fiber_entry(Fiber::Entry entry, void* arg) {
    entry(arg);
    std::fprintf(stderr, "[fiber] entry returned\n");
    std::abort();
}

Fiber::Fiber(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Fiber::~Fiber() { release_stack(impl_->stack); }

Fiber* Fiber::current() {
    if (!g_current) {
        g_thread_fiber.reset(new Fiber(std::make_unique<Impl>()));
        g_current = g_thread_fiber.get();
    }
    return g_current;
}

std::unique_ptr<Fiber> Fiber::create(Entry entry, void* arg, size_t stack_bytes) {
    current();
    auto impl = std::make_unique<Impl>();
    impl->stack = take_stack(stack_bytes);
    impl->entry = entry;
    impl->arg = arg;
    // The frame dcb_fiber_switch pops (layout above): zeroed registers, default MXCSR/FPU CW,
    // this stack's TIB bounds, r12 = entry, r13 = arg, return into dcb_fiber_start.
    const uintptr_t top = impl->stack.base + impl->stack.bytes;
    auto* frame = reinterpret_cast<uint64_t*>(top - 0x120);
    std::memset(frame, 0, 0x120);
    auto* bytes = reinterpret_cast<uint8_t*>(frame);
    const uint32_t mxcsr = 0x1F80;
    const uint16_t fpucw = 0x037F;
    std::memcpy(bytes + 0xA0, &mxcsr, sizeof mxcsr);
    std::memcpy(bytes + 0xA4, &fpucw, sizeof fpucw);
    frame[0xB0 / 8] = impl->stack.base;                // DeallocationStack
    frame[0xB8 / 8] = impl->stack.base + page_size();  // StackLimit (above the guard page)
    frame[0xC0 / 8] = top;                             // StackBase
    frame[0xE0 / 8] = reinterpret_cast<uint64_t>(entry);  // r12
    frame[0xD8 / 8] = reinterpret_cast<uint64_t>(arg);    // r13
    frame[0x108 / 8] = reinterpret_cast<uint64_t>(&dcb_fiber_start);
    impl->sp = frame;
    return std::unique_ptr<Fiber>(new Fiber(std::move(impl)));
}

void Fiber::resume() {
    Fiber* from = current();
    if (from == this) return;
    g_current = this;
    dcb_fiber_switch(&from->impl_->sp, impl_->sp);
}

bool Fiber::snapshots_supported() { return true; }

Fiber::Image Fiber::capture() const {
    if (this == g_current) throw std::runtime_error("fiber capture: the fiber is running");
    Image image = capture_stack(impl_->stack, reinterpret_cast<uintptr_t>(impl_->sp), 0);
    image.entry = reinterpret_cast<uint64_t>(impl_->entry);
    image.arg = reinterpret_cast<uint64_t>(impl_->arg);
    image.context.resize(sizeof(uint64_t));
    const uint64_t sp = reinterpret_cast<uint64_t>(impl_->sp);
    std::memcpy(image.context.data(), &sp, sizeof sp);
    return image;
}

std::unique_ptr<Fiber> Fiber::restore(const Image& image) {
    if (image.context.size() != sizeof(uint64_t)) throw std::runtime_error("fiber restore: context from another backend");
    uint64_t sp = 0;
    std::memcpy(&sp, image.context.data(), sizeof sp);
    if (sp < image.data_base || sp >= image.stack_base + image.stack_bytes)
        throw std::runtime_error("fiber restore: stack pointer outside the stack image");
    current();
    auto impl = std::make_unique<Impl>();
    impl->stack = restore_stack(image);
    impl->entry = reinterpret_cast<Entry>(static_cast<uintptr_t>(image.entry));  // a fiber not started yet
    impl->arg = reinterpret_cast<void*>(static_cast<uintptr_t>(image.arg));
    impl->sp = reinterpret_cast<void*>(static_cast<uintptr_t>(sp));
    return std::unique_ptr<Fiber>(new Fiber(std::move(impl)));
}

#elif DCB_FIBER_WIN32_FIBERS
// ---------------------------------------------------------------------------------------------
// Windows Fibers API (MSVC builds). The context lives in OS memory: no save states.

struct Fiber::Impl {
    LPVOID handle = nullptr;
    bool owned = false;
    Entry entry = nullptr;
    void* arg = nullptr;
};

Fiber::Fiber(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Fiber::~Fiber() {
    if (impl_->owned && impl_->handle) DeleteFiber(impl_->handle);
}

Fiber* Fiber::current() {
    if (!g_current) {
        auto impl = std::make_unique<Impl>();
        impl->handle = ConvertThreadToFiber(nullptr);
        if (!impl->handle) {
            std::fprintf(stderr, "[fiber] ConvertThreadToFiber failed\n");
            std::abort();
        }
        g_thread_fiber.reset(new Fiber(std::move(impl)));
        g_current = g_thread_fiber.get();
    }
    return g_current;
}

std::unique_ptr<Fiber> Fiber::create(Entry entry, void* arg, size_t stack_bytes) {
    current();  // make sure the thread is a fiber before switching
    auto impl = std::make_unique<Impl>();
    impl->owned = true;
    impl->entry = entry;
    impl->arg = arg;
    Impl* raw = impl.get();
    // Reserve the whole stack but commit little: it grows on demand, like the Linux mmap stacks.
    impl->handle = CreateFiberEx(64 * 1024, stack_bytes, FIBER_FLAG_FLOAT_SWITCH, [](LPVOID p) {
        auto* self = static_cast<Impl*>(p);
        self->entry(self->arg);
        std::fprintf(stderr, "[fiber] entry returned\n");
        std::abort();
    }, raw);
    if (!impl->handle) {
        std::fprintf(stderr, "[fiber] CreateFiber failed\n");
        std::abort();
    }
    return std::unique_ptr<Fiber>(new Fiber(std::move(impl)));
}

void Fiber::resume() {
    Fiber* from = current();
    if (from == this) return;
    g_current = this;
    SwitchToFiber(impl_->handle);
    (void)from;
}

bool Fiber::snapshots_supported() { return false; }

Fiber::Image Fiber::capture() const { throw std::runtime_error("save states are not supported by Win32 fibers"); }

std::unique_ptr<Fiber> Fiber::restore(const Image&) {
    throw std::runtime_error("save states are not supported by Win32 fibers");
}

#else
// ---------------------------------------------------------------------------------------------
// ucontext.

struct Fiber::Impl {
    ucontext_t context{};
    Stack stack{};  ///< base 0: the adopted thread
    Entry entry = nullptr;
    void* arg = nullptr;
};

namespace {
Fiber::Impl* g_starting = nullptr;  // hands the new fiber its Impl (makecontext takes int args)

void trampoline() {
    Fiber::Impl* self = g_starting;
    self->entry(self->arg);
    std::fprintf(stderr, "[fiber] entry returned\n");
    std::abort();
}

// The saved stack pointer, and bytes below it the ABI lets code use without moving it.
#if defined(__GLIBC__) && defined(__x86_64__)
constexpr bool kSnapshots = true;
constexpr size_t kRedZone = 128;
uintptr_t context_sp(const ucontext_t& c) { return static_cast<uintptr_t>(c.uc_mcontext.gregs[REG_RSP]); }
/// glibc keeps a pointer to the x87/SSE save area inside the ucontext itself.
void relocate_context(ucontext_t& c) { c.uc_mcontext.fpregs = &c.__fpregs_mem; }
#elif defined(__GLIBC__) && defined(__aarch64__)
constexpr bool kSnapshots = true;
constexpr size_t kRedZone = 0;
uintptr_t context_sp(const ucontext_t& c) { return static_cast<uintptr_t>(c.uc_mcontext.sp); }
void relocate_context(ucontext_t&) {}
#else  // macOS and other libcs: the context layout is not known to be position-independent
constexpr bool kSnapshots = false;
constexpr size_t kRedZone = 0;
uintptr_t context_sp(const ucontext_t&) { return 0; }
void relocate_context(ucontext_t&) {}
#endif
}  // namespace

Fiber::Fiber(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Fiber::~Fiber() { release_stack(impl_->stack); }

Fiber* Fiber::current() {
    if (!g_current) {
        g_thread_fiber.reset(new Fiber(std::make_unique<Impl>()));
        g_current = g_thread_fiber.get();
    }
    return g_current;
}

std::unique_ptr<Fiber> Fiber::create(Entry entry, void* arg, size_t stack_bytes) {
    current();
    auto impl = std::make_unique<Impl>();
    impl->stack = take_stack(stack_bytes);
    impl->entry = entry;
    impl->arg = arg;
    const size_t page = page_size();
    getcontext(&impl->context);
    impl->context.uc_stack.ss_sp = reinterpret_cast<void*>(impl->stack.base + page);
    impl->context.uc_stack.ss_size = impl->stack.bytes - page;
    impl->context.uc_link = nullptr;
    makecontext(&impl->context, trampoline, 0);
    return std::unique_ptr<Fiber>(new Fiber(std::move(impl)));
}

void Fiber::resume() {
    Fiber* from = current();
    if (from == this) return;
    g_current = this;
    g_starting = impl_.get();  // read by trampoline() on a fiber's first run only
    swapcontext(&from->impl_->context, &impl_->context);
}

bool Fiber::snapshots_supported() { return kSnapshots; }

Fiber::Image Fiber::capture() const {
    if (!kSnapshots) throw std::runtime_error("save states are not supported by this platform's fibers");
    if (this == g_current) throw std::runtime_error("fiber capture: the fiber is running");
    Image image = capture_stack(impl_->stack, context_sp(impl_->context), kRedZone);
    image.entry = reinterpret_cast<uint64_t>(impl_->entry);
    image.arg = reinterpret_cast<uint64_t>(impl_->arg);
    image.context.resize(sizeof(ucontext_t));
    std::memcpy(image.context.data(), &impl_->context, sizeof(ucontext_t));
    return image;
}

std::unique_ptr<Fiber> Fiber::restore(const Image& image) {
    if (!kSnapshots) throw std::runtime_error("save states are not supported by this platform's fibers");
    if (image.context.size() != sizeof(ucontext_t)) throw std::runtime_error("fiber restore: context from another backend");
    current();
    auto impl = std::make_unique<Impl>();
    std::memcpy(&impl->context, image.context.data(), sizeof(ucontext_t));
    const uintptr_t sp = context_sp(impl->context);
    if (sp < image.data_base || sp >= image.stack_base + image.stack_bytes)
        throw std::runtime_error("fiber restore: stack pointer outside the stack image");
    relocate_context(impl->context);
    impl->stack = restore_stack(image);
    impl->entry = reinterpret_cast<Entry>(static_cast<uintptr_t>(image.entry));  // a fiber not started yet
    impl->arg = reinterpret_cast<void*>(static_cast<uintptr_t>(image.arg));
    return std::unique_ptr<Fiber>(new Fiber(std::move(impl)));
}

#endif

}  // namespace psx
