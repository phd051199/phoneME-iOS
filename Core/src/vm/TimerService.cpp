#include "phoneme/vm/TimerService.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

#include "phoneme/vm/Machine.hpp"

namespace phoneme::vm {

TimerService::TimerService(Machine& machine) noexcept : machine_(machine) {}

TimerService::~TimerService() { shutdown(); }

i64 TimerService::current_time_millis() noexcept {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (value > std::numeric_limits<i64>::max()) {
        return std::numeric_limits<i64>::max();
    }
    if (value < std::numeric_limits<i64>::min()) {
        return std::numeric_limits<i64>::min();
    }
    return static_cast<i64>(value);
}

void TimerService::sort_entries(std::vector<ScheduledEntry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const ScheduledEntry& left,
                 const ScheduledEntry& right) {
                  if (left.next_time_millis != right.next_time_millis) {
                      return left.next_time_millis < right.next_time_millis;
                  }
                  return left.sequence < right.sequence;
              });
}

Status TimerService::initialize_timer(ObjectRef timer, bool daemon) {
    if (timer.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Timer receiver is null");
    }

    auto state = std::make_shared<TimerState>();
    state->timer = timer;
    state->daemon = daemon;
    {
        std::scoped_lock lock(mutex_);
        if (shutting_down_) {
            return fail(ErrorCode::invalid_state,
                        "Timer service is shutting down");
        }
        if (timers_.contains(timer.bits)) {
            return fail_java("java/lang/IllegalStateException",
                             "Timer was initialized twice");
        }
        timers_.emplace(timer.bits, state);
    }

    auto started = machine_.ensure_emulation_event_worker();
    if (!started) {
        std::scoped_lock lock(mutex_);
        timers_.erase(timer.bits);
        return started;
    }
    return {};
}

Status TimerService::initialize_task(ObjectRef task) {
    if (task.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "TimerTask receiver is null");
    }
    std::scoped_lock lock(mutex_);
    if (shutting_down_) {
        return fail(ErrorCode::invalid_state,
                    "Timer service is shutting down");
    }
    auto& state = tasks_[task.bits];
    if (state.initialized) {
        return fail_java("java/lang/IllegalStateException",
                         "TimerTask was initialized twice");
    }
    state.initialized = true;
    return {};
}

Status TimerService::schedule(ObjectRef timer,
                              ObjectRef task,
                              i64 first_time_millis,
                              i64 period_millis,
                              bool fixed_rate) {
    if (timer.is_null() || task.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Timer and TimerTask must be non-null");
    }
    if (period_millis < 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         "Timer period cannot be negative");
    }

    std::scoped_lock lock(mutex_);
    const auto timer_found = timers_.find(timer.bits);
    if (timer_found == timers_.end() || timer_found->second->cancelled ||
        shutting_down_) {
        return fail_java("java/lang/IllegalStateException",
                         "Timer is cancelled or unavailable");
    }
    auto& task_state = tasks_[task.bits];
    if (!task_state.initialized) task_state.initialized = true;
    if (task_state.scheduled || task_state.cancelled || task_state.completed) {
        return fail_java("java/lang/IllegalStateException",
                         "TimerTask is already scheduled or cancelled");
    }
    if (next_sequence_ == 0U) {
        return fail(ErrorCode::overflow,
                    "Timer schedule sequence is exhausted");
    }

    task_state.scheduled = true;
    task_state.owner_timer_bits = timer.bits;
    timer_found->second->entries.push_back(ScheduledEntry {
        .task = task,
        .next_time_millis = first_time_millis,
        .period_millis = period_millis,
        .fixed_rate = fixed_rate,
        .sequence = next_sequence_++,
    });
    sort_entries(timer_found->second->entries);
    machine_.wake_emulation_event_worker();
    return {};
}

Status TimerService::cancel_timer(ObjectRef timer) {
    if (timer.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Timer receiver is null");
    }
    std::scoped_lock lock(mutex_);
    const auto found = timers_.find(timer.bits);
    if (found == timers_.end()) return {};
    found->second->cancelled = true;
    found->second->entries.clear();
    machine_.wake_emulation_event_worker();
    return {};
}

Result<bool> TimerService::cancel_task(ObjectRef task) {
    if (task.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "TimerTask receiver is null");
    }
    std::scoped_lock lock(mutex_);
    auto& state = tasks_[task.bits];
    if (!state.initialized) state.initialized = true;
    const bool prevented = state.scheduled && !state.cancelled &&
                           !state.completed;
    state.cancelled = true;
    if (state.owner_timer_bits != 0U) {
        const auto owner = timers_.find(state.owner_timer_bits);
        if (owner != timers_.end()) {
            std::erase_if(owner->second->entries,
                          [task](const ScheduledEntry& entry) {
                              return entry.task == task;
                          });
        }
    }
    machine_.wake_emulation_event_worker();
    return prevented;
}

Result<i64> TimerService::scheduled_execution_time(ObjectRef task) const {
    if (task.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "TimerTask receiver is null");
    }
    std::scoped_lock lock(mutex_);
    const auto found = tasks_.find(task.bits);
    return found == tasks_.end() ? 0 : found->second.scheduled_execution_time;
}

TimerServiceDiagnostics TimerService::diagnostics() const noexcept {
    std::scoped_lock lock(mutex_);
    TimerServiceDiagnostics result {
        .timers = timers_.size(),
        .task_records = tasks_.size(),
    };
    for (const auto& [id, timer] : timers_) {
        (void)id;
        if (timer != nullptr && !timer->cancelled) {
            result.scheduled_tasks += timer->entries.size();
        }
    }
    return result;
}

Result<bool> TimerService::dispatch_one_due() {
    ScheduledEntry entry;
    std::shared_ptr<TimerState> timer;
    {
        std::scoped_lock lock(mutex_);
        if (shutting_down_) return false;

        const i64 now = current_time_millis();
        i64 earliest_due = std::numeric_limits<i64>::max();
        for (auto& [bits, candidate] : timers_) {
            (void)bits;
            if (candidate == nullptr || candidate->cancelled) continue;
            std::erase_if(
                candidate->entries,
                [this](const ScheduledEntry& scheduled) {
                    const auto task = tasks_.find(scheduled.task.bits);
                    return task == tasks_.end() || task->second.cancelled ||
                           task->second.completed;
                });
            sort_entries(candidate->entries);
            if (!candidate->entries.empty() &&
                candidate->entries.front().next_time_millis < earliest_due) {
                earliest_due = candidate->entries.front().next_time_millis;
                timer = candidate;
            }
        }
        if (!timer || earliest_due > now) return false;

        entry = timer->entries.front();
        timer->entries.erase(timer->entries.begin());
        auto task = tasks_.find(entry.task.bits);
        if (task == tasks_.end() || task->second.cancelled) return true;
        task->second.scheduled_execution_time = entry.next_time_millis;
        if (entry.period_millis == 0) {
            task->second.completed = true;
        } else if (entry.fixed_rate) {
            if (entry.next_time_millis >
                std::numeric_limits<i64>::max() - entry.period_millis) {
                task->second.completed = true;
            } else {
                entry.next_time_millis += entry.period_millis;
                timer->entries.push_back(entry);
                sort_entries(timer->entries);
            }
        }
    }

    auto invoked = machine_.invoke_instance(
        entry.task, "java/util/TimerTask", "run", "()V");
    if (!invoked || invoked->throwable.has_value()) {
        std::scoped_lock lock(mutex_);
        timer->cancelled = true;
        timer->entries.clear();
        return true;
    }

    if (entry.period_millis > 0 && !entry.fixed_rate) {
        std::scoped_lock lock(mutex_);
        const auto task = tasks_.find(entry.task.bits);
        if (!timer->cancelled && task != tasks_.end() &&
            !task->second.cancelled && !task->second.completed &&
            !shutting_down_) {
            const i64 now = current_time_millis();
            if (now <= std::numeric_limits<i64>::max() - entry.period_millis) {
                entry.next_time_millis = now + entry.period_millis;
                timer->entries.push_back(entry);
                sort_entries(timer->entries);
            } else {
                task->second.completed = true;
            }
        }
    }
    return true;
}

std::optional<std::chrono::milliseconds>
TimerService::time_until_next_event() const noexcept {
    std::scoped_lock lock(mutex_);
    if (shutting_down_) return std::nullopt;

    i64 earliest_due = std::numeric_limits<i64>::max();
    for (const auto& [bits, timer] : timers_) {
        (void)bits;
        if (timer == nullptr || timer->cancelled) continue;
        for (const ScheduledEntry& entry : timer->entries) {
            const auto task = tasks_.find(entry.task.bits);
            if (task == tasks_.end() || task->second.cancelled ||
                task->second.completed) {
                continue;
            }
            earliest_due = std::min(earliest_due, entry.next_time_millis);
        }
    }
    if (earliest_due == std::numeric_limits<i64>::max()) {
        return std::nullopt;
    }
    const i64 now = current_time_millis();
    if (earliest_due <= now) return std::chrono::milliseconds::zero();
    const i64 delta = earliest_due - now;
    return std::chrono::milliseconds(delta);
}

void TimerService::append_reference_roots(
    std::vector<ObjectRef>& roots) const {
    std::scoped_lock lock(mutex_);
    roots.reserve(roots.size() + timers_.size() * 2U + tasks_.size());
    for (const auto& [bits, timer] : timers_) {
        (void)bits;
        if (!timer->timer.is_null()) roots.push_back(timer->timer);
        for (const ScheduledEntry& entry : timer->entries) {
            if (!entry.task.is_null()) roots.push_back(entry.task);
        }
    }
    for (const auto& [bits, state] : tasks_) {
        if (state.initialized && !state.completed) {
            roots.push_back(ObjectRef {bits});
        }
    }
}

void TimerService::shutdown() noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (shutting_down_) return;
        shutting_down_ = true;
        for (auto& [bits, timer] : timers_) {
            (void)bits;
            timer->cancelled = true;
            timer->entries.clear();
        }
    }
    machine_.wake_emulation_event_worker();
}

} // namespace phoneme::vm
