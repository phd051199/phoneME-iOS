#include "phoneme/vm/JavaFiber.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <utility>

#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace phoneme::vm {

#if defined(__APPLE__) && defined(__aarch64__)
namespace {

struct alignas(16) FiberContext final {
    // x19..x30 are the AAPCS64 callee-saved/general frame registers. x30 is
    // also the bootstrap PC for a never-before-run fiber.
    u64 x19_to_x30[12] {};
    u64 sp {0};
    u64 reserved {0};
    alignas(16) unsigned __int128 q8_to_q15[8] {};
};

static_assert(offsetof(FiberContext, x19_to_x30) == 0U);
static_assert(offsetof(FiberContext, sp) == 96U);
static_assert(offsetof(FiberContext, q8_to_q15) == 112U);
static_assert(sizeof(FiberContext) == 240U);

extern "C" __attribute__((naked, noinline))
void phoneme_fiber_swap(FiberContext*, const FiberContext*) noexcept {
    __asm__ volatile(
        "stp x19, x20, [x0, #0]\n"
        "stp x21, x22, [x0, #16]\n"
        "stp x23, x24, [x0, #32]\n"
        "stp x25, x26, [x0, #48]\n"
        "stp x27, x28, [x0, #64]\n"
        "stp x29, x30, [x0, #80]\n"
        "mov x9, sp\n"
        "str x9, [x0, #96]\n"
        "stp q8, q9, [x0, #112]\n"
        "stp q10, q11, [x0, #144]\n"
        "stp q12, q13, [x0, #176]\n"
        "stp q14, q15, [x0, #208]\n"
        "ldp x19, x20, [x1, #0]\n"
        "ldp x21, x22, [x1, #16]\n"
        "ldp x23, x24, [x1, #32]\n"
        "ldp x25, x26, [x1, #48]\n"
        "ldp x27, x28, [x1, #64]\n"
        "ldp x29, x30, [x1, #80]\n"
        "ldr x9, [x1, #96]\n"
        "mov sp, x9\n"
        "ldp q8, q9, [x1, #112]\n"
        "ldp q10, q11, [x1, #144]\n"
        "ldp q12, q13, [x1, #176]\n"
        "ldp q14, q15, [x1, #208]\n"
        "ret\n");
}

[[nodiscard]] usize system_page_size() noexcept {
    const long value = ::sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<usize>(value) : 16U * 1024U;
}

[[nodiscard]] usize round_up(usize value, usize alignment) noexcept {
    if (alignment == 0U || value >
            std::numeric_limits<usize>::max() - (alignment - 1U)) {
        return 0U;
    }
    return (value + alignment - 1U) & ~(alignment - 1U);
}

} // namespace

struct JavaFiber::Impl final {
    FiberContext fiber_context;
    FiberContext carrier_context;
    Task task;
    void* mapping {MAP_FAILED};
    usize mapping_bytes {0U};
    bool initialized {false};
    bool finished {false};

    ~Impl() {
        if (mapping != MAP_FAILED) {
            ::munmap(mapping, mapping_bytes);
        }
    }

    static void entry_thunk(void* raw) noexcept {
        auto* self = static_cast<Impl*>(raw);
        self->task();
        self->finished = true;
        phoneme_fiber_swap(&self->fiber_context, &self->carrier_context);
        std::abort();
    }
};

extern "C" __attribute__((used, retain, noinline))
void phoneme_fiber_entry_thunk(void* raw) noexcept {
    JavaFiber::Impl::entry_thunk(raw);
}

extern "C" __attribute__((naked, noinline))
void phoneme_fiber_bootstrap() noexcept {
    // The initial context stores Impl* in x19. Enter the normal C++ thunk with
    // a standard ABI argument; it never returns to this bootstrap frame.
    __asm__ volatile(
        "mov x0, x19\n"
        "bl _phoneme_fiber_entry_thunk\n"
        "brk #0\n");
}

#else

struct JavaFiber::Impl final {
    Task task;
    bool initialized {false};
    bool finished {false};
};

#endif

JavaFiber::JavaFiber() : impl_(std::make_unique<Impl>()) {}
JavaFiber::~JavaFiber() = default;

bool JavaFiber::supported() noexcept {
#if defined(__APPLE__) && defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

Status JavaFiber::initialize(Task task, usize stack_bytes) {
    if (!task) {
        return fail(ErrorCode::invalid_argument, "Java fiber requires a task");
    }
    if (impl_->initialized) {
        return fail(ErrorCode::already_running, "Java fiber is already initialized");
    }
#if defined(__APPLE__) && defined(__aarch64__)
    const usize page = system_page_size();
    const usize minimum_stack = 128U * 1024U;
    const usize usable = round_up(std::max(stack_bytes, minimum_stack), page);
    if (usable == 0U || usable >
            std::numeric_limits<usize>::max() - page * 2U) {
        return fail(ErrorCode::overflow, "Java fiber stack size overflowed");
    }
    const usize mapping_bytes = usable + page * 2U;
    void* mapping = ::mmap(nullptr,
                           mapping_bytes,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON,
                           -1,
                           0);
    if (mapping == MAP_FAILED) {
        return fail(ErrorCode::internal_error, "could not allocate Java fiber stack");
    }
    auto* bytes = static_cast<u8*>(mapping);
    if (::mprotect(bytes, page, PROT_NONE) != 0 ||
        ::mprotect(bytes + page + usable, page, PROT_NONE) != 0) {
        ::munmap(mapping, mapping_bytes);
        return fail(ErrorCode::internal_error, "could not guard Java fiber stack");
    }

    impl_->task = std::move(task);
    impl_->mapping = mapping;
    impl_->mapping_bytes = mapping_bytes;
    impl_->fiber_context = {};
    impl_->carrier_context = {};
    impl_->fiber_context.x19_to_x30[0] =
        static_cast<u64>(reinterpret_cast<uintptr_t>(impl_.get()));
    impl_->fiber_context.x19_to_x30[11] =
        static_cast<u64>(reinterpret_cast<uintptr_t>(&phoneme_fiber_bootstrap));
    const uintptr_t stack_top = reinterpret_cast<uintptr_t>(bytes + page + usable);
    impl_->fiber_context.sp = static_cast<u64>(stack_top & ~uintptr_t {0xFU});
    impl_->initialized = true;
    return {};
#else
    (void)stack_bytes;
    impl_->task = std::move(task);
    return fail(ErrorCode::unsupported_feature,
                "stackful Java fibers are unavailable on this platform");
#endif
}

bool JavaFiber::initialized() const noexcept { return impl_->initialized; }
bool JavaFiber::finished() const noexcept { return impl_->finished; }

void JavaFiber::resume() noexcept {
#if defined(__APPLE__) && defined(__aarch64__)
    if (!impl_->initialized || impl_->finished) return;
    phoneme_fiber_swap(&impl_->carrier_context, &impl_->fiber_context);
#endif
}

void JavaFiber::yield() noexcept {
#if defined(__APPLE__) && defined(__aarch64__)
    if (!impl_->initialized || impl_->finished) return;
    phoneme_fiber_swap(&impl_->fiber_context, &impl_->carrier_context);
#endif
}

} // namespace phoneme::vm
