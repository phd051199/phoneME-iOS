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
    using RootWalker =
        void (*)(void* context, std::vector<ObjectRef>& roots) noexcept;

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
    void set_root_walker(u32 invocation_depth,
                         void* context,
                         RootWalker walker,
                         bool clear_published_roots = false);
    void set_transient_root_walker(u32 invocation_depth,
                                   void* context,
                                   RootWalker walker,
                                   bool clear_published_roots = false);
    void clear_transient_root_walker(u32 invocation_depth,
                                     void* context) noexcept;
    void clear_published_roots(u32 invocation_depth) noexcept;
    void clear_roots(u32 invocation_depth) noexcept;
    void append_reference_roots(std::vector<ObjectRef>& roots) const;

    void set_pending_exception(std::optional<ObjectRef> throwable) noexcept;
    [[nodiscard]] std::optional<ObjectRef> pending_exception() const noexcept;

    void add_executed_instructions(u64 count) noexcept;
    [[nodiscard]] u64 executed_instructions() const noexcept;

private:
    struct DepthRoots final {
        std::vector<ObjectRef> published;
        void* walker_context {nullptr};
        RootWalker walker {nullptr};
        // JIT runtime dispatch temporarily overlays a precise native-frame
        // walker while the generated frame is guaranteed to be live. Keep it
        // separate from the interpreter's long-lived walker so nested JIT
        // calls do not replace or flatten interpreter roots.
        void* transient_walker_context {nullptr};
        RootWalker transient_walker {nullptr};
    };

    [[nodiscard]] DepthRoots& depth_roots(u32 invocation_depth);

    JavaThreadId thread_id_ {0};
    mutable std::mutex mutex_;
    // Invocation depth is dense and normally tiny. A flat vector avoids the
    // hash lookup/allocation that used to sit on every Java/JIT safepoint.
    // Each depth can also expose a live stack walker. Interpreter frames then
    // remain precise GC roots without flattening/copying them into a vector on
    // every cooperative yield; they are walked only when GC actually asks.
    std::vector<DepthRoots> roots_by_depth_;
    std::optional<ObjectRef> pending_exception_;
    u64 executed_instructions_ {0};
};

} // namespace phoneme::vm
