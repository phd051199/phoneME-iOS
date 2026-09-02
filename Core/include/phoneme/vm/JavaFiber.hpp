#pragma once

#include <functional>
#include <memory>

#include "phoneme/base/Error.hpp"

namespace phoneme::vm {

// Small stackful execution context used by the physical-iOS guest scheduler.
// Java bytecode execution remains ordinary C++/JIT code; switching a fiber
// therefore preserves native helper frames without serializing interpreter
// state. Unsupported targets simply report supported()==false and keep the
// existing pthread-per-thread fallback.
class JavaFiber final {
public:
    using Task = std::function<void()>;
    // Kept public only so the C ABI bootstrap thunk in JavaFiber.cpp can name
    // the opaque implementation type. The definition remains translation-unit
    // private and no caller can inspect it through this header.
    struct Impl;

    JavaFiber();
    ~JavaFiber();

    JavaFiber(const JavaFiber&) = delete;
    JavaFiber& operator=(const JavaFiber&) = delete;

    [[nodiscard]] static bool supported() noexcept;
    // Keep the logical-thread stack at the same safe ceiling as the old
    // pthread worker. Large generated builtin factories and deep JIT runtime
    // helpers can transiently use well over 512 KiB of native C++ stack even
    // though ordinary Java frames live in VM-managed storage. mmap-backed
    // fiber stacks remain demand-paged, so this reserves address space without
    // committing 2 MiB of physical memory per guest thread up front.
    [[nodiscard]] Status initialize(Task task,
                                    usize stack_bytes = 2U * 1024U * 1024U);
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool finished() const noexcept;

    void resume() noexcept;
    void yield() noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace phoneme::vm
