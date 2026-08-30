#include "phoneme/vm/ExecutionContext.hpp"

namespace phoneme::vm {

void ExecutionContext::publish_roots(u32 invocation_depth,
                                     std::span<const ObjectRef> roots) {
    std::scoped_lock lock(mutex_);
    auto& published = roots_by_depth_[invocation_depth];
    published.assign(roots.begin(), roots.end());
}

std::span<const ObjectRef> ExecutionContext::exchange_roots(
    u32 invocation_depth,
    std::vector<ObjectRef>& roots) {
    std::scoped_lock lock(mutex_);
    auto& published = roots_by_depth_[invocation_depth];
    published.swap(roots);
    return std::span<const ObjectRef>(published.data(), published.size());
}

void ExecutionContext::clear_roots(u32 invocation_depth) noexcept {
    std::scoped_lock lock(mutex_);
    roots_by_depth_.erase(invocation_depth);
}

void ExecutionContext::append_reference_roots(
    std::vector<ObjectRef>& roots) const {
    std::scoped_lock lock(mutex_);
    for (const auto& [depth, published] : roots_by_depth_) {
        (void)depth;
        for (const ObjectRef reference : published) {
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
