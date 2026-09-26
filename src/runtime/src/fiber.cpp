#include <psx/fiber.hpp>

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace psx {

namespace {
Fiber* g_current = nullptr;
std::unique_ptr<Fiber> g_thread_fiber;  // the adopted thread
}  // namespace

#ifdef _WIN32

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
    impl->handle = CreateFiber(stack_bytes, [](LPVOID p) {
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

#else  // ucontext

struct Fiber::Impl {
    ucontext_t context{};
    void* stack = nullptr;
    size_t stack_bytes = 0;
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
}  // namespace

Fiber::Fiber(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Fiber::~Fiber() {
    if (impl_->stack) munmap(impl_->stack, impl_->stack_bytes);
}

Fiber* Fiber::current() {
    if (!g_current) {
        g_thread_fiber.reset(new Fiber(std::make_unique<Impl>()));
        g_current = g_thread_fiber.get();
    }
    return g_current;
}

std::unique_ptr<Fiber> Fiber::create(Entry entry, void* arg, size_t stack_bytes) {
    current();
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    stack_bytes = (stack_bytes + page - 1) / page * page + page;  // + guard page at the bottom
    void* mem = mmap(nullptr, stack_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
        std::fprintf(stderr, "[fiber] cannot map a %zu-byte stack\n", stack_bytes);
        std::abort();
    }
    mprotect(mem, page, PROT_NONE);  // stack overflow faults instead of corrupting memory

    auto impl = std::make_unique<Impl>();
    impl->stack = mem;
    impl->stack_bytes = stack_bytes;
    impl->entry = entry;
    impl->arg = arg;
    getcontext(&impl->context);
    impl->context.uc_stack.ss_sp = static_cast<char*>(mem) + page;
    impl->context.uc_stack.ss_size = stack_bytes - page;
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

#endif

}  // namespace psx
