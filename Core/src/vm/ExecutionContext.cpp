#include "phoneme/vm/ExecutionContext.hpp"

namespace phoneme::vm {

ExecutionContext::DepthRoots& ExecutionContext::depth_roots(
    u32 invocation_depth) {
    const usize required = static_cast<usize>(invocation_depth) + 1U;
    if (roots_by_depth_.size() < required) {
        roots_by_depth_.resize(required);
    }
    return roots_by_depth_[invocation_depth];
}

void ExecutionContext::publish_roots(u32 invocation_depth,
                                     std::span<const ObjectRef> roots) {
    std::scoped_lock lock(mutex_);
    auto& published = depth_roots(invocation_depth).published;
    published.assign(roots.begin(), roots.end());
}

std::span<const ObjectRef> ExecutionContext::exchange_roots(
    u32 invocation_depth,
    std::vector<ObjectRef>& roots) {
    std::scoped_lock lock(mutex_);
    auto& published = depth_roots(invocation_depth).published;
    published.swap(roots);
    return std::span<const ObjectRef>(published.data(), published.size());
}

void ExecutionContext::set_root_walker(u32 invocation_depth,
                                       void* context,
                                       RootWalker walker,
                                       bool clear_published_roots) {
    std::scoped_lock lock(mutex_);
    DepthRoots& roots = depth_roots(invocation_depth);
    roots.walker_context = context;
    roots.walker = walker;
    if (clear_published_roots) {
        roots.published.clear();
    }
}

void ExecutionContext::set_transient_root_walker(
    u32 invocation_depth,
    void* context,
    RootWalker walker,
    bool clear_published_roots) {
    std::scoped_lock lock(mutex_);
    DepthRoots& roots = depth_roots(invocation_depth);
    roots.transient_walker_context = context;
    roots.transient_walker = walker;
    if (clear_published_roots) {
        roots.published.clear();
    }
}

void ExecutionContext::clear_transient_root_walker(
    u32 invocation_depth,
    void* context) noexcept {
    std::scoped_lock lock(mutex_);
    if (invocation_depth >= roots_by_depth_.size()) {
        return;
    }
    DepthRoots& roots = roots_by_depth_[invocation_depth];
    // A nested dispatch at the same logical depth should never occur, but
    // matching the owner makes cleanup robust against an older scope trying
    // to clear a newer overlay.
    if (roots.transient_walker_context != context) {
        return;
    }
    roots.transient_walker_context = nullptr;
    roots.transient_walker = nullptr;
}

void ExecutionContext::clear_published_roots(u32 invocation_depth) noexcept {
    std::scoped_lock lock(mutex_);
    if (invocation_depth >= roots_by_depth_.size()) {
        return;
    }
    roots_by_depth_[invocation_depth].published.clear();
}

void ExecutionContext::clear_roots(u32 invocation_depth) noexcept {
    std::scoped_lock lock(mutex_);
    if (invocation_depth >= roots_by_depth_.size()) {
        return;
    }
    DepthRoots& roots = roots_by_depth_[invocation_depth];
    roots.published.clear();
    roots.walker_context = nullptr;
    roots.walker = nullptr;
    roots.transient_walker_context = nullptr;
    roots.transient_walker = nullptr;
    // Trim only trailing empty depths. This preserves allocations for the
    // common recursive call depth while keeping completed deep recursion from
    // pinning a large outer vector forever.
    while (!roots_by_depth_.empty()) {
        const DepthRoots& tail = roots_by_depth_.back();
        if (!tail.published.empty() || tail.walker != nullptr ||
            tail.transient_walker != nullptr) {
            break;
        }
        roots_by_depth_.pop_back();
    }
}

void ExecutionContext::append_reference_roots(
    std::vector<ObjectRef>& roots) const {
    std::scoped_lock lock(mutex_);
    for (const DepthRoots& depth : roots_by_depth_) {
        if (depth.walker != nullptr && depth.walker_context != nullptr) {
            depth.walker(depth.walker_context, roots);
        }
        if (depth.transient_walker != nullptr &&
            depth.transient_walker_context != nullptr) {
            depth.transient_walker(depth.transient_walker_context, roots);
        }
        for (const ObjectRef reference : depth.published) {
            if (!reference.is_null()) {
                roots.push_back(reference);
            }
        }
    }
    if (pending_exception_.has_value() &&
        !pending_exception_->is_null()) {
        roots.push_back(*pending_exception_);
    }
}

void ExecutionContext::set_pending_exception(
    std::optional<ObjectRef> throwable) noexcept {
    std::scoped_lock lock(mutex_);
    pending_exception_ = throwable;
}

std::optional<ObjectRef> ExecutionContext::pending_exception() const noexcept {
    std::scoped_lock lock(mutex_);
    return pending_exception_;
}

void ExecutionContext::add_executed_instructions(u64 count) noexcept {
    std::scoped_lock lock(mutex_);
    executed_instructions_ += count;
}

u64 ExecutionContext::executed_instructions() const noexcept {
    std::scoped_lock lock(mutex_);
    return executed_instructions_;
}

} // namespace phoneme::vm
