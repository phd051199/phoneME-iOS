#pragma once

#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "phoneme/vm/MonitorTable.hpp"

namespace phoneme::vm {

class ExecutionContext final {
public:
    explicit ExecutionContext(JavaThreadId thread_id) noexcept
        : thread_id_(thread_id) {}

    [[nodiscard]] JavaThreadId thread_id() const noexcept {
        return thread_id_;
    }

    void publish_roots(u32 invocation_depth,
                       std::span<const ObjectRef> roots);
    std::span<const ObjectRef> exchange_roots(
        u32 invocation_depth,
        std::vector<ObjectRef>& roots);
    void clear_roots(u32 invocation_depth) noexcept;
    void append_reference_roots(std::vector<ObjectRef>& roots) const;

    void set_pending_exception(std::optional<ObjectRef> throwable) noexcept;
    [[nodiscard]] std::optional<ObjectRef> pending_exception() const noexcept;

    void add_executed_instructions(u64 count) noexcept;
    [[nodiscard]] u64 executed_instructions() const noexcept;

private:
    JavaThreadId thread_id_ {0};
    mutable std::mutex mutex_;
    std::unordered_map<u32, std::vector<ObjectRef>> roots_by_depth_;
    std::optional<ObjectRef> pending_exception_;
    u64 executed_instructions_ {0};
};

} // namespace phoneme::vm
