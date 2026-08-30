#include "phoneme/vm/Scheduler.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <thread>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <os/log.h>
#endif

#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/MonitorTable.hpp"
#include "phoneme/vm/PerformanceCounters.hpp"
#include "phoneme/vm/VmTrace.hpp"
#include "phoneme/runtime/WorkCoordinator.hpp"

namespace phoneme::vm {

thread_local Scheduler* Scheduler::tls_scheduler_ = nullptr;
thread_local JavaThreadId Scheduler::tls_thread_id_ = 0;
thread_local u32 Scheduler::tls_unblocked_quantum_count_ = 0U;
thread_local u64 Scheduler::tls_unblocked_active_microseconds_ = 0U;
thread_local u64 Scheduler::tls_host_foreground_generation_ = 0U;
thread_local std::chrono::steady_clock::time_point
    Scheduler::tls_quantum_resume_time_ {};
thread_local bool Scheduler::tls_quantum_timing_valid_ = false;
thread_local u32 Scheduler::tls_unpaced_execution_depth_ = 0U;
thread_local bool Scheduler::tls_frame_boundary_pending_ = false;
thread_local u32 Scheduler::tls_frame_pacing_streak_ = 0U;
thread_local i64 Scheduler::tls_frame_interval_sample_nanoseconds_ = 0;
thread_local bool Scheduler::tls_frame_loop_wake_valid_ = false;
thread_local bool Scheduler::tls_frame_cap_deadline_valid_ = false;
thread_local bool Scheduler::tls_native_frame_boundary_valid_ = false;
thread_local bool Scheduler::tls_native_backpressure_active_ = false;
thread_local bool Scheduler::tls_pressure_frame_boundary_valid_ = false;
thread_local u32 Scheduler::tls_native_fast_frame_streak_ = 0U;
thread_local u64 Scheduler::tls_frame_pacing_generation_ = 0U;
thread_local std::chrono::steady_clock::time_point
    Scheduler::tls_frame_loop_wake_time_ {};
thread_local std::chrono::steady_clock::time_point
    Scheduler::tls_frame_cap_deadline_ {};
thread_local std::chrono::steady_clock::time_point
    Scheduler::tls_frame_boundary_time_ {};
thread_local std::chrono::steady_clock::time_point
    Scheduler::tls_native_frame_boundary_time_ {};
thread_local std::chrono::steady_clock::time_point
    Scheduler::tls_pressure_frame_boundary_time_ {};

namespace {

constexpr auto kExplicitYieldBackoff = std::chrono::milliseconds(1);
// Foreground VM quanta only need enough pause to hand the execution gate to
// framebuffer/input workers. The previous 0.5-10 ms adaptive sleeps consumed
// roughly one third of foreground wall time and could pull otherwise healthy
// game loops below their intended frame rate. Keep a light duty-cycle guard for
// pathological spin loops without turning scheduler fairness into frame pacing.
constexpr auto kInitialQuantumBackoff = std::chrono::microseconds(50);
constexpr auto kWarmMinimumBackoff = std::chrono::microseconds(100);
constexpr auto kWarmMaximumBackoff = std::chrono::microseconds(500);
constexpr auto kSustainedMinimumBackoff = std::chrono::microseconds(150);
constexpr auto kSustainedMaximumBackoff = std::chrono::microseconds(1'500);
constexpr auto kBusyMinimumBackoff = std::chrono::microseconds(350);
constexpr auto kBusyMaximumBackoff = std::chrono::milliseconds(4);
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
// A Java worker that keeps consuming CPU without blocking or publishing a
// frame is almost always a polling/game-logic loop. Harrier naturally limits
// these loops through its coarser interpreter. phoneME's faster interpreter
// otherwise lets them pin a performance core even when presentation is capped
// at 30 FPS. Start conservatively at nominal temperature and tighten the duty
// cycle only after iOS reports real thermal pressure.
constexpr auto kNominalNonRenderActivation = std::chrono::milliseconds(50);
constexpr auto kFairNonRenderActivation = std::chrono::milliseconds(25);
constexpr auto kSeriousNonRenderActivation = std::chrono::milliseconds(12);
constexpr auto kCriticalNonRenderActivation = std::chrono::milliseconds(6);
constexpr auto kMinimumRecentFrameGrace = std::chrono::milliseconds(100);

[[nodiscard]] std::chrono::microseconds non_render_activation(
    runtime::ThermalPressure pressure) noexcept {
    switch (pressure) {
    case runtime::ThermalPressure::critical:
        return std::chrono::duration_cast<std::chrono::microseconds>(
            kCriticalNonRenderActivation);
    case runtime::ThermalPressure::serious:
        return std::chrono::duration_cast<std::chrono::microseconds>(
            kSeriousNonRenderActivation);
    case runtime::ThermalPressure::fair:
        return std::chrono::duration_cast<std::chrono::microseconds>(
            kFairNonRenderActivation);
    case runtime::ThermalPressure::nominal:
        return std::chrono::duration_cast<std::chrono::microseconds>(
            kNominalNonRenderActivation);
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(
        kNominalNonRenderActivation);
}

[[nodiscard]] std::chrono::microseconds non_render_backoff(
    runtime::ThermalPressure pressure,
    std::chrono::microseconds active_cpu_time) noexcept {
    switch (pressure) {
    case runtime::ThermalPressure::critical:
        return std::clamp(
            active_cpu_time * 4,
            std::chrono::microseconds(1'000),
            std::chrono::microseconds(5'000));
    case runtime::ThermalPressure::serious:
        return std::clamp(
            active_cpu_time * 2,
            std::chrono::microseconds(500),
            std::chrono::microseconds(3'000));
    case runtime::ThermalPressure::fair:
        return std::clamp(
            active_cpu_time,
            std::chrono::microseconds(250),
            std::chrono::microseconds(1500));
    case runtime::ThermalPressure::nominal:
        return std::clamp(
            active_cpu_time / 2,
            std::chrono::microseconds(100),
            std::chrono::microseconds(750));
    }
    return std::chrono::microseconds(100);
}
#endif
// Hidden MIDlets share one execution gate per VM. This caps aggregate CPU even
// when a game has several Java threads that would otherwise take turns while
// each individual thread is sleeping. A blocked socket/timer callback still
// runs immediately until it reaches the next bounded bytecode scheduler quantum.
constexpr auto kBackgroundMinimumInterval = std::chrono::milliseconds(250);
constexpr auto kBackgroundMaximumInterval = std::chrono::seconds(2);
// Only a short, stable sleep immediately following an actual framebuffer
// publication may be treated as game-loop pacing. Override mode deliberately
// requires several matching frames before it may shorten a Java sleep.
constexpr auto kMaximumFrameBoundaryToSleepDelay =
    std::chrono::milliseconds(20);
constexpr auto kMaximumFramePacingRequest =
    std::chrono::milliseconds(100);
constexpr auto kMinimumFramePacingRequest =
    std::chrono::milliseconds(1);
constexpr auto kMinimumSleepStabilityTolerance =
    std::chrono::milliseconds(2);
constexpr u32 kOverrideActivationStreak = 6U;
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
// Avoid oscillating right at the frame deadline. Only sustained, clearly
// over-fast rendering is capped on device; near-target heavy games are left
// alone so the pacing gate cannot manufacture stutter.
constexpr u32 kNativeBackpressureActivationStreak = 3U;
constexpr i64 kNativeBackpressureFastFrameNumerator = 4;
constexpr i64 kNativeBackpressureFastFrameDenominator = 5;
#else
constexpr u32 kNativeBackpressureActivationStreak = 4U;
constexpr i64 kNativeBackpressureFastFrameNumerator = 4;
constexpr i64 kNativeBackpressureFastFrameDenominator = 5;
#endif

[[nodiscard]] const char* thread_state_name(JavaThreadState state) noexcept {
    switch (state) {
    case JavaThreadState::new_thread: return "new";
    case JavaThreadState::runnable: return "runnable";
    case JavaThreadState::running: return "running";
    case JavaThreadState::blocked_monitor: return "blocked-monitor";
    case JavaThreadState::blocked_io: return "blocked-io";
    case JavaThreadState::waiting: return "waiting";
    case JavaThreadState::sleeping: return "sleeping";
    case JavaThreadState::joining: return "joining";
    case JavaThreadState::terminated: return "terminated";
    }
    return "unknown";
}

[[nodiscard]] bool is_traceworthy_thread_state(
    JavaThreadState state) noexcept {
    return state == JavaThreadState::blocked_monitor ||
           state == JavaThreadState::blocked_io ||
           state == JavaThreadState::waiting ||
           state == JavaThreadState::joining ||
           state == JavaThreadState::terminated;
}

[[nodiscard]] std::chrono::steady_clock::duration background_interval(
    std::chrono::microseconds active_cpu_time) noexcept {
    // Reserve roughly one execution slot for every twenty-five units of interpreter
    // time. This targets <5% aggregate VM duty with margin for scheduler/JIT
    // overhead while adapting across Debug, Release and device generations.
    const auto target = active_cpu_time * 25;
    return std::clamp(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(target),
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            kBackgroundMinimumInterval),
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            kBackgroundMaximumInterval));
}

[[nodiscard]] std::chrono::microseconds clamp_backoff(
    std::chrono::microseconds value,
    std::chrono::microseconds minimum,
    std::chrono::microseconds maximum) noexcept {
    return std::clamp(value, minimum, maximum);
}

[[nodiscard]] std::chrono::microseconds quantum_backoff(
    u32 uninterrupted_quantums,
    std::chrono::microseconds active_cpu_time) noexcept {
    if (uninterrupted_quantums < 4U) {
        return kInitialQuantumBackoff;
    }
    if (uninterrupted_quantums < 16U) {
        return clamp_backoff(
            active_cpu_time / 8,
            kWarmMinimumBackoff,
            kWarmMaximumBackoff);
    }
    if (uninterrupted_quantums < 64U) {
        return clamp_backoff(
            active_cpu_time / 3,
            kSustainedMinimumBackoff,
            kSustainedMaximumBackoff);
    }
    return clamp_backoff(
        active_cpu_time * 2 / 3,
        kBusyMinimumBackoff,
        std::chrono::duration_cast<std::chrono::microseconds>(
            kBusyMaximumBackoff));
}

void report_thread_failure(
    Machine& machine,
    JavaThreadId thread_id,
    const std::optional<ObjectRef>& throwable,
    const std::optional<Error>& failure,
    std::string_view exception_context) noexcept {
    if (failure.has_value()) {
        char message[1024] {};
        std::snprintf(
            message,
            sizeof(message),
            "phoneME Java thread %u failed: %s%s%s",
            static_cast<unsigned>(thread_id),
            failure->java_exception_class.empty()
                ? ""
                : failure->java_exception_class.c_str(),
            failure->java_exception_class.empty() ? "" : ": ",
            failure->message.c_str());
        std::fprintf(stderr, "%s\n", message);
#if defined(__APPLE__)
        os_log_error(OS_LOG_DEFAULT, "%{public}s", message);
#endif
    }
    if (throwable.has_value() && !throwable->is_null()) {
        auto class_name = machine.heap().class_name(*throwable);
        std::string detail;
        auto detail_value = machine.heap().field(*throwable, 0U);
        if (detail_value) {
            auto detail_reference = detail_value->as_reference();
            if (detail_reference && !detail_reference->is_null()) {
                auto text = machine.heap().string_value(*detail_reference);
                if (text) {
                    detail.reserve(text->size());
                    for (char16_t character : *text) {
                        detail.push_back(character <= 0x7fU
                            ? static_cast<char>(character)
                            : '?');
                    }
                }
            }
        }
        char message[1024] {};
        std::snprintf(
            message,
            sizeof(message),
            "phoneME Java thread %u terminated with uncaught %s%s%s%s%s",
            static_cast<unsigned>(thread_id),
            class_name.has_value() ? class_name->c_str() : "Throwable",
            detail.empty() ? "" : ": ",
            detail.c_str(),
            exception_context.empty() ? "" : " at ",
            exception_context.data());
        std::fprintf(stderr, "%s\n", message);
#if defined(__APPLE__)
        os_log_error(OS_LOG_DEFAULT, "%{public}s", message);
#endif
    }
}

} // namespace

Scheduler::Scheduler() {
    auto main_thread = std::make_shared<JavaThread>(
        1U, ObjectRef {}, ObjectRef {});
    main_thread->started_ = true;
    main_thread->state_ = JavaThreadState::running;
    by_id_.emplace(1U, main_thread);
    tls_scheduler_ = this;
    tls_thread_id_ = 1U;
}

Scheduler::~Scheduler() {
    shutdown();
    if (tls_scheduler_ == this) {
        tls_scheduler_ = nullptr;
        tls_thread_id_ = 0;
    }
}

JavaThreadId Scheduler::current_thread_id() const noexcept {
    if (tls_scheduler_ == this && tls_thread_id_ != 0U) {
        return tls_thread_id_;
    }
    return 1U;
}

std::shared_ptr<JavaThread> Scheduler::find_thread_locked(
    ObjectRef thread_object) const {
    if (thread_object.is_null()) {
        return {};
    }
    const auto iterator = by_object_.find(thread_object.bits);
    return iterator == by_object_.end() ? nullptr : iterator->second;
}

std::shared_ptr<JavaThread> Scheduler::current_thread_record() const {
    const JavaThreadId id = current_thread_id();
    std::scoped_lock lock(mutex_);
    const auto iterator = by_id_.find(id);
    return iterator == by_id_.end() ? nullptr : iterator->second;
}

Result<ObjectRef> Scheduler::current_thread_object() const {
    auto thread = current_thread_record();
    if (!thread) {
        return fail(ErrorCode::invalid_state,
                    "current Java thread is not registered");
    }
    std::scoped_lock lock(thread->mutex_);
    if (thread->object_.is_null()) {
        return fail(ErrorCode::invalid_state,
                    "current Java thread has no java.lang.Thread object");
    }
    return thread->object_;
}

Status Scheduler::bind_current_thread_object(ObjectRef object) {
    if (object.is_null()) {
        return fail(ErrorCode::invalid_argument,
                    "cannot bind a null java.lang.Thread object");
    }
    auto thread = current_thread_record();
    if (!thread) {
        return fail(ErrorCode::invalid_state,
                    "current Java thread is not registered");
    }
    std::scoped_lock scheduler_lock(mutex_);
    const auto existing = by_object_.find(object.bits);
    if (existing != by_object_.end() && existing->second != thread) {
        return fail(ErrorCode::invalid_state,
                    "java.lang.Thread object is already bound");
    }
    {
        std::scoped_lock thread_lock(thread->mutex_);
        if (!thread->object_.is_null() && thread->object_ != object) {
            by_object_.erase(thread->object_.bits);
        }
        thread->object_ = object;
    }
    by_object_[object.bits] = thread;
    return {};
}

Status Scheduler::register_thread(ObjectRef thread_object,
                                  ObjectRef runnable_target) {
    if (thread_object.is_null()) {
        return fail(ErrorCode::invalid_argument,
                    "cannot register a null java.lang.Thread object");
    }
    std::scoped_lock lock(mutex_);
    if (shutting_down_) {
        return fail(ErrorCode::invalid_state,
                    "scheduler is shutting down");
    }
    if (auto existing = find_thread_locked(thread_object)) {
        std::scoped_lock thread_lock(existing->mutex_);
        if (existing->started_) {
            return fail_java("java/lang/IllegalThreadStateException",
                             "Thread constructor called after start");
        }
        existing->target_ = runnable_target;
        return {};
    }
    if (next_thread_id_ == 0U) {
        return fail(ErrorCode::overflow,
                    "Java thread identifier space was exhausted");
    }
    const JavaThreadId id = next_thread_id_++;
    auto thread = std::make_shared<JavaThread>(id,
                                               thread_object,
                                               runnable_target);
    by_object_.emplace(thread_object.bits, thread);
    by_id_.emplace(id, std::move(thread));
    return {};
}

Status Scheduler::start_thread(Machine& machine, ObjectRef thread_object) {
    std::shared_ptr<JavaThread> thread;
    {
        std::scoped_lock lock(mutex_);
        if (shutting_down_) {
            return fail(ErrorCode::invalid_state,
                        "scheduler is shutting down");
        }
        thread = find_thread_locked(thread_object);
        if (!thread) {
            if (next_thread_id_ == 0U) {
                return fail(ErrorCode::overflow,
                            "Java thread identifier space was exhausted");
            }
            const JavaThreadId id = next_thread_id_++;
            thread = std::make_shared<JavaThread>(
                id, thread_object, ObjectRef {});
            by_object_.emplace(thread_object.bits, thread);
            by_id_.emplace(id, thread);
        }
        {
            std::scoped_lock thread_lock(thread->mutex_);
            if (thread->started_) {
                return fail_java("java/lang/IllegalThreadStateException",
                                 "Thread.start called more than once");
            }
            thread->started_ = true;
            thread->state_ = JavaThreadState::runnable;
        }
        update_queue_membership_locked(thread->id_,
                                       JavaThreadState::runnable);
    }

    vm_trace("thread",
             "start java=%u native=0 object=%llu",
             static_cast<unsigned>(thread->id_),
             static_cast<unsigned long long>(thread_object.bits));
    auto worker_started = thread->worker_.start(
        [this, &machine, thread](std::stop_token stop_token) {
            tls_scheduler_ = this;
            tls_thread_id_ = thread->id_;

            std::optional<ObjectRef> throwable;
            std::optional<Error> failure;
            std::string exception_context;
            if (!stop_token.stop_requested()) {
                auto result = machine.run_java_thread_entry(thread->object_);
                if (!result) {
                    failure = result.error();
                } else if (result->throwable.has_value()) {
                    throwable = result->throwable;
                    exception_context = result->exception_context;
                }
            }
            // Forced MIDlet teardown cancels blocked workers internally. That
            // cancellation is not an uncaught application exception and must
            // not poison lifecycle state or emit a misleading Java failure.
            if (stop_token.stop_requested()) {
                throwable.reset();
                failure.reset();
            }

            report_thread_failure(
                machine, thread->id_, throwable, failure, exception_context);
            machine.monitors().release_all(thread->id_);
            finish_thread(thread, throwable, failure);
            tls_scheduler_ = nullptr;
            tls_thread_id_ = 0;
        });
    if (!worker_started) {
        {
            std::scoped_lock thread_lock(thread->mutex_);
            thread->started_ = false;
            thread->state_ = JavaThreadState::new_thread;
        }
        std::scoped_lock lock(mutex_);
        update_queue_membership_locked(thread->id_,
                                       JavaThreadState::new_thread);
        return std::unexpected(worker_started.error());
    }
    return {};
}

Status Scheduler::start_native_thread(
    Machine& machine,
    ObjectRef thread_object,
    SchedulerNativeTask task) {
    if (!task) {
        return fail(ErrorCode::invalid_argument,
                    "native Java thread requires a task");
    }
    prune_terminated_native_threads();

    std::shared_ptr<JavaThread> thread;
    {
        std::scoped_lock lock(mutex_);
        if (shutting_down_) {
            return fail(ErrorCode::invalid_state,
                        "scheduler is shutting down");
        }
        thread = find_thread_locked(thread_object);
        if (!thread) {
            if (next_thread_id_ == 0U) {
                return fail(ErrorCode::overflow,
                            "Java thread identifier space was exhausted");
            }
            const JavaThreadId id = next_thread_id_++;
            thread = std::make_shared<JavaThread>(
                id, thread_object, ObjectRef {});
            by_object_.emplace(thread_object.bits, thread);
            by_id_.emplace(id, thread);
        }
        {
            std::scoped_lock thread_lock(thread->mutex_);
            if (thread->started_) {
                return fail_java("java/lang/IllegalThreadStateException",
                                 "native Java thread was started twice");
            }
            thread->started_ = true;
            thread->native_task_ = true;
            thread->state_ = JavaThreadState::runnable;
        }
        update_queue_membership_locked(thread->id_,
                                       JavaThreadState::runnable);
    }

    vm_trace("thread",
             "start java=%u native=1 object=%llu",
             static_cast<unsigned>(thread->id_),
             static_cast<unsigned long long>(thread_object.bits));
    auto worker_started = thread->worker_.start(
        [this, &machine, thread, task = std::move(task)](
            std::stop_token stop_token) mutable {
            tls_scheduler_ = this;
            tls_thread_id_ = thread->id_;
            set_current_state(JavaThreadState::running);

            std::optional<ObjectRef> throwable;
            std::optional<Error> failure;
            if (!stop_token.stop_requested()) {
                auto result = task(stop_token);
                if (!result) {
                    failure = result.error();
                } else {
                    throwable = *result;
                }
            }
            if (stop_token.stop_requested()) {
                throwable.reset();
                failure.reset();
            }

            report_thread_failure(
                machine, thread->id_, throwable, failure, {});
            machine.monitors().release_all(thread->id_);
            finish_thread(thread, throwable, failure);
            tls_scheduler_ = nullptr;
            tls_thread_id_ = 0;
        });
    if (!worker_started) {
        {
            std::scoped_lock thread_lock(thread->mutex_);
            thread->started_ = false;
            thread->native_task_ = false;
            thread->state_ = JavaThreadState::new_thread;
        }
        std::scoped_lock lock(mutex_);
        update_queue_membership_locked(thread->id_,
                                       JavaThreadState::new_thread);
        return std::unexpected(worker_started.error());
    }
    return {};
}

Result<ObjectRef> Scheduler::runnable_target(ObjectRef thread_object) const {
    std::shared_ptr<JavaThread> thread;
    {
        std::scoped_lock lock(mutex_);
        thread = find_thread_locked(thread_object);
    }
    if (!thread) {
        return fail(ErrorCode::invalid_state,
                    "java.lang.Thread object is not registered");
    }
    std::scoped_lock lock(thread->mutex_);
    return thread->target_;
}

void Scheduler::begin_execution_slice() noexcept {
    // Host-driven main-thread invocations do not own Scheduler TLS, but they
    // execute the same interpreter loop and require identical CPU pacing.
    tls_unblocked_quantum_count_ = 0U;
    tls_unblocked_active_microseconds_ = 0U;
    // Re-synchronize foreground ownership at the first maintenance boundary
    // of every top-level execution. This also makes an invocation that starts
    // while already backgrounded enter the shared CPU gate immediately.
    tls_host_foreground_generation_ = 0U;
    tls_quantum_resume_time_ = std::chrono::steady_clock::now();
    tls_quantum_timing_valid_ = true;
}

bool Scheduler::consume_current_background_transition() noexcept {
    const u64 generation =
        host_foreground_generation_.load(std::memory_order_acquire);
    if (tls_host_foreground_generation_ == generation) return false;
    tls_host_foreground_generation_ = generation;
    return !host_foreground_.load(std::memory_order_acquire);
}

void Scheduler::begin_unpaced_execution() noexcept {
    if (tls_unpaced_execution_depth_ != std::numeric_limits<u32>::max()) {
        ++tls_unpaced_execution_depth_;
    }
    tls_unblocked_quantum_count_ = 0U;
    tls_unblocked_active_microseconds_ = 0U;
    tls_quantum_resume_time_ = std::chrono::steady_clock::now();
    tls_quantum_timing_valid_ = true;
}

void Scheduler::end_unpaced_execution() noexcept {
    if (tls_unpaced_execution_depth_ != 0U) {
        --tls_unpaced_execution_depth_;
    }
    tls_unblocked_quantum_count_ = 0U;
    tls_unblocked_active_microseconds_ = 0U;
    tls_quantum_resume_time_ = std::chrono::steady_clock::now();
    tls_quantum_timing_valid_ = true;
}

void Scheduler::set_host_foreground(bool foreground) noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (host_foreground_.load(std::memory_order_acquire) == foreground) {
            return;
        }
        host_foreground_.store(foreground, std::memory_order_release);
        background_resume_deadline_ = {};
        host_foreground_generation_.fetch_add(1U, std::memory_order_acq_rel);
        frame_pacing_generation_.fetch_add(1U, std::memory_order_acq_rel);
    }
    // Foregrounding must release every Java thread waiting on the shared
    // background gate immediately instead of waiting for its reserved slot.
    background_condition_.notify_all();
}

void Scheduler::configure_frame_pacing(
    i32 frames_per_second,
    FramePacingMode mode) noexcept {
    const i64 normalized = std::clamp<i64>(frames_per_second, 1, 240);
    const i64 interval =
        (1'000'000'000LL + normalized / 2LL) / normalized;
    frame_interval_nanoseconds_.store(
        std::max<i64>(interval, 1),
        std::memory_order_release);
    frame_pacing_mode_.store(
        static_cast<i32>(mode),
        std::memory_order_release);
    frame_pacing_generation_.fetch_add(1U, std::memory_order_acq_rel);
    background_condition_.notify_all();
}

void Scheduler::reset_current_frame_pacing_state() noexcept {
    tls_frame_boundary_pending_ = false;
    tls_frame_pacing_streak_ = 0U;
    tls_frame_interval_sample_nanoseconds_ = 0;
    tls_frame_loop_wake_valid_ = false;
    tls_frame_cap_deadline_valid_ = false;
    tls_native_frame_boundary_valid_ = false;
    tls_native_backpressure_active_ = false;
    tls_pressure_frame_boundary_valid_ = false;
    tls_native_fast_frame_streak_ = 0U;
}

void Scheduler::synchronize_current_frame_pacing_state() noexcept {
    const u64 generation =
        frame_pacing_generation_.load(std::memory_order_acquire);
    if (tls_frame_pacing_generation_ == generation) return;
    reset_current_frame_pacing_state();
    tls_frame_pacing_generation_ = generation;
}

void Scheduler::pace_current_frame_publication(Machine& machine) {
    if (tls_scheduler_ != this || tls_thread_id_ == 0U) return;
    synchronize_current_frame_pacing_state();

    const auto mode = static_cast<FramePacingMode>(
        frame_pacing_mode_.load(std::memory_order_acquire));
    const bool host_foreground =
        host_foreground_.load(std::memory_order_acquire);
    if (!host_foreground) {
        tls_frame_cap_deadline_valid_ = false;
        tls_native_frame_boundary_valid_ = false;
        tls_native_backpressure_active_ = false;
        tls_native_fast_frame_streak_ = 0U;
        return;
    }

    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (mode == FramePacingMode::cap) {
        if (!tls_frame_cap_deadline_valid_) return;
        deadline = tls_frame_cap_deadline_;
    } else if (mode == FramePacingMode::native) {
        // Native mode preserves a MIDlet's own pacing. Activate only after a
        // sustained run of frames produced faster than the configured display
        // interval. Once active, keep backpressure engaged while the game
        // reaches the next publication gate early. A game that begins pacing
        // itself (sleep/work >= the fast threshold) disables host backpressure
        // immediately, so we never stack a host delay on top of a real game
        // delay.
        if (!tls_native_frame_boundary_valid_) return;
        const i64 target = frame_interval_nanoseconds_.load(
            std::memory_order_acquire);
        const i64 fast_threshold =
            (target * kNativeBackpressureFastFrameNumerator) /
            kNativeBackpressureFastFrameDenominator;
        const auto native_now = std::chrono::steady_clock::now();
        const i64 since_boundary =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                native_now - tls_native_frame_boundary_time_).count();
        if (tls_native_backpressure_active_) {
            if (since_boundary >= fast_threshold) {
                tls_native_backpressure_active_ = false;
                tls_native_fast_frame_streak_ = 0U;
                return;
            }
        } else if (tls_native_fast_frame_streak_ >=
                   kNativeBackpressureActivationStreak) {
            tls_native_backpressure_active_ = true;
        } else {
            return;
        }
        deadline = tls_native_frame_boundary_time_ +
            std::chrono::nanoseconds(target);
    } else {
        tls_frame_cap_deadline_valid_ = false;
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (!deadline.has_value() || now >= *deadline) return;

    // This is a presentation gate, not Java Thread.sleep. Keep the interrupt
    // flag untouched and release the VM execution gate while waiting so input,
    // networking and other Java workers continue to make progress. A pacing
    // generation change wakes the gate immediately when FPS, mode or foreground
    // ownership changes.
    const u64 pacing_generation = tls_frame_pacing_generation_;
    PerformanceCounters::record_scheduler_sleep();
    set_current_state(JavaThreadState::runnable);
    const u32 depth = machine.suspend_execution_for_blocking();
    {
        std::unique_lock lock(mutex_);
        background_condition_.wait_until(
            lock,
            *deadline,
            [this, pacing_generation] {
                return shutting_down_.load(std::memory_order_acquire) ||
                       !host_foreground_.load(std::memory_order_acquire) ||
                       frame_pacing_generation_.load(
                           std::memory_order_acquire) != pacing_generation;
            });
    }
    machine.resume_execution_after_blocking(depth);
    const auto resumed = std::chrono::steady_clock::now();
    if (resumed > now) {
        PerformanceCounters::record_frame_backpressure(static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                resumed - now).count()));
    }
    tls_quantum_resume_time_ = resumed;
    tls_quantum_timing_valid_ = true;
    set_current_state(JavaThreadState::running);
}

void Scheduler::note_current_frame_boundary() noexcept {
    if (tls_scheduler_ != this || tls_thread_id_ == 0U) return;
    synchronize_current_frame_pacing_state();

    const auto mode = static_cast<FramePacingMode>(
        frame_pacing_mode_.load(std::memory_order_acquire));
    const bool host_foreground =
        host_foreground_.load(std::memory_order_acquire);
    if (!host_foreground) {
        reset_current_frame_pacing_state();
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    // Feed the *actual* publication-to-publication cadence into the shared
    // native CPU budget at the frame boundary itself. Doing this from
    // pace_current_frame_publication() missed ordinary native-paced games,
    // because that function intentionally returns early until sustained
    // overproduction activates backpressure. A game that is already missing
    // frame deadlines must suppress speculative JIT/native background work
    // even when host pacing is otherwise inactive.
    if (tls_pressure_frame_boundary_valid_ &&
        now >= tls_pressure_frame_boundary_time_) {
        const i64 elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - tls_pressure_frame_boundary_time_).count();
        const i64 target = frame_interval_nanoseconds_.load(
            std::memory_order_acquire);
        if (elapsed > 0 && target > 0) {
            runtime::shared_work_coordinator().note_frame_interval(
                static_cast<u64>(elapsed), static_cast<u64>(target));
        }
    }
    tls_pressure_frame_boundary_time_ = now;
    tls_pressure_frame_boundary_valid_ = true;

    // A published framebuffer is a real cooperative progress boundary. Do not
    // carry the sustained-busy quantum streak across frames; otherwise a
    // healthy foreground game gradually receives the same heavy backoff as a
    // non-rendering spin loop after it has run for a while.
    tls_unblocked_quantum_count_ = 0U;
    tls_unblocked_active_microseconds_ = 0U;
    tls_quantum_resume_time_ = now;
    tls_quantum_timing_valid_ = true;

    if (mode == FramePacingMode::native) {
        const i64 target = frame_interval_nanoseconds_.load(
            std::memory_order_acquire);
        if (!tls_native_backpressure_active_ &&
            tls_native_frame_boundary_valid_ &&
            now >= tls_native_frame_boundary_time_) {
            const i64 elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - tls_native_frame_boundary_time_).count();
            const i64 fast_threshold =
                (target * kNativeBackpressureFastFrameNumerator) /
                kNativeBackpressureFastFrameDenominator;
            if (elapsed > 0 && elapsed < fast_threshold) {
                if (tls_native_fast_frame_streak_ !=
                    std::numeric_limits<u32>::max()) {
                    ++tls_native_fast_frame_streak_;
                }
            } else {
                tls_native_fast_frame_streak_ = 0U;
            }
        }
        tls_native_frame_boundary_time_ = now;
        tls_native_frame_boundary_valid_ = true;
        tls_frame_boundary_pending_ = false;
        tls_frame_pacing_streak_ = 0U;
        tls_frame_interval_sample_nanoseconds_ = 0;
        tls_frame_loop_wake_valid_ = false;
        tls_frame_cap_deadline_valid_ = false;
        return;
    }

    tls_native_frame_boundary_valid_ = false;
    tls_native_backpressure_active_ = false;
    tls_native_fast_frame_streak_ = 0U;

    if (mode == FramePacingMode::cap) {
        tls_frame_boundary_pending_ = false;
        tls_frame_pacing_streak_ = 0U;
        tls_frame_interval_sample_nanoseconds_ = 0;
        tls_frame_loop_wake_valid_ = false;
        tls_frame_cap_deadline_ = now + std::chrono::nanoseconds(
            frame_interval_nanoseconds_.load(std::memory_order_acquire));
        tls_frame_cap_deadline_valid_ = true;
        return;
    }

    tls_frame_cap_deadline_valid_ = false;
    tls_frame_boundary_pending_ = true;
    tls_frame_boundary_time_ = now;
}

void Scheduler::note_current_frame_request() noexcept {
    if (tls_scheduler_ != this || tls_thread_id_ == 0U) return;
    synchronize_current_frame_pacing_state();

    const auto mode = static_cast<FramePacingMode>(
        frame_pacing_mode_.load(std::memory_order_acquire));
    const bool host_foreground =
        host_foreground_.load(std::memory_order_acquire);
    if (!host_foreground || mode != FramePacingMode::override_game_loop) {
        return;
    }

    // Canvas.repaint() is asynchronous in MIDP: the game loop thread requests
    // a frame, while the runtime may paint/publish it on another Java thread.
    // Keep the override detector attached to the requesting thread so a stable
    // repaint(); sleep(...) loop can be retargeted without changing unrelated
    // timers. Actual publication still owns cap pacing and FPS accounting.
    tls_frame_boundary_pending_ = true;
    tls_frame_boundary_time_ = std::chrono::steady_clock::now();
}

void Scheduler::break_current_frame_pacing_sequence() noexcept {
    if (tls_scheduler_ != this || tls_thread_id_ == 0U) return;
    synchronize_current_frame_pacing_state();
    reset_current_frame_pacing_state();
}

void Scheduler::cooperative_quantum(Machine& machine) {
    auto current = current_thread_record();
    if (!current) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto active_cpu_time = tls_quantum_timing_valid_
        ? std::max(
              std::chrono::duration_cast<std::chrono::microseconds>(
                  now - tls_quantum_resume_time_),
              std::chrono::microseconds(1))
        : kInitialQuantumBackoff;
    const bool unpaced_execution = tls_unpaced_execution_depth_ != 0U;
    if (!unpaced_execution && tls_unblocked_quantum_count_ !=
        std::numeric_limits<u32>::max()) {
        ++tls_unblocked_quantum_count_;
    }
    if (!unpaced_execution) {
        const u64 active_microseconds = static_cast<u64>(
            std::max<i64>(active_cpu_time.count(), 0));
        if (tls_unblocked_active_microseconds_ >
            std::numeric_limits<u64>::max() - active_microseconds) {
            tls_unblocked_active_microseconds_ =
                std::numeric_limits<u64>::max();
        } else {
            tls_unblocked_active_microseconds_ += active_microseconds;
        }
    }
    bool deterministic_mode = false;
    bool foreground_peer_runnable = false;
    std::optional<std::chrono::steady_clock::time_point> background_deadline;
    {
        std::scoped_lock lock(mutex_);
        deterministic_mode = deterministic_;
        foreground_peer_runnable =
            !deterministic_mode &&
            host_foreground_.load(std::memory_order_acquire) &&
            !runnable_queue_.empty();
        if (!deterministic_mode &&
            !host_foreground_.load(std::memory_order_acquire)) {
            const auto reservation_time = std::chrono::steady_clock::now();
            if (background_resume_deadline_ < reservation_time) {
                background_resume_deadline_ = reservation_time;
            }
            background_resume_deadline_ +=
                background_interval(active_cpu_time);
            background_deadline = background_resume_deadline_;
        }
    }
    const auto backoff = deterministic_mode
        ? std::chrono::duration_cast<std::chrono::microseconds>(
              kBusyMinimumBackoff)
        : quantum_backoff(
              tls_unblocked_quantum_count_, active_cpu_time);

    const bool periodic_foreground_handoff =
        !unpaced_execution && !deterministic_mode &&
        !background_deadline.has_value() &&
        ((tls_unblocked_quantum_count_ & 7U) == 0U);
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
    // A thread may publish an intro frame once and then enter a permanent
    // polling loop, so "has ever rendered" is not enough to exempt it from
    // thermal pacing. Treat only a *recent* frame as productive render work.
    // Blocking/sleep/yield and real frame publication reset the accumulated
    // active CPU streak immediately.
    const runtime::ThermalPressure thermal_pressure =
        runtime::shared_work_coordinator().thermal_pressure();
    const auto target_frame_interval = std::chrono::nanoseconds(
        std::max<i64>(
            frame_interval_nanoseconds_.load(std::memory_order_acquire),
            1));
    const auto recent_frame_grace = std::max(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            kMinimumRecentFrameGrace),
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            target_frame_interval * 4));
    const bool recent_frame =
        tls_pressure_frame_boundary_valid_ &&
        now >= tls_pressure_frame_boundary_time_ &&
        now - tls_pressure_frame_boundary_time_ <= recent_frame_grace;
    const auto spin_activation = non_render_activation(thermal_pressure);
    const bool nominal_cadence_slot =
        thermal_pressure != runtime::ThermalPressure::nominal ||
        ((tls_unblocked_quantum_count_ & 1U) == 0U);
    const bool thermal_non_render_spin_backoff =
        !unpaced_execution && !deterministic_mode &&
        !background_deadline.has_value() &&
        !recent_frame && nominal_cadence_slot &&
        tls_unblocked_quantum_count_ >= 8U &&
        tls_unblocked_active_microseconds_ >=
            static_cast<u64>(spin_activation.count());
    const auto thermal_spin_backoff = thermal_non_render_spin_backoff
        ? non_render_backoff(thermal_pressure, active_cpu_time)
        : std::chrono::microseconds::zero();
#else
    const bool thermal_non_render_spin_backoff = false;
    const auto thermal_spin_backoff = std::chrono::microseconds::zero();
#endif
    const bool needs_execution_handoff =
        background_deadline.has_value() || unpaced_execution ||
        deterministic_mode || foreground_peer_runnable ||
        periodic_foreground_handoff || thermal_non_render_spin_backoff;

    if (!needs_execution_handoff) {
        // Most healthy foreground quanta have no Java peer waiting and are not
        // at the periodic fairness boundary. Previously we still transitioned
        // running -> runnable and released/reacquired execution_mutex_ here,
        // even though the branch below performed no yield or sleep. That lock
        // churn sits directly in the interpreter hot path. Keep the periodic
        // one-in-eight handoff for newly-runnable peers while making the other
        // seven quanta a true no-op.
        tls_quantum_resume_time_ = now;
        tls_quantum_timing_valid_ = true;
        return;
    }

    set_current_state(JavaThreadState::runnable);
    const u32 depth = machine.suspend_execution_for_blocking();
    if (background_deadline.has_value()) {
        std::unique_lock lock(mutex_);
        background_condition_.wait_until(lock, *background_deadline, [this] {
            return host_foreground_.load(std::memory_order_acquire) ||
                   shutting_down_.load(std::memory_order_acquire);
        });
    } else if (unpaced_execution) {
        // MIDlet construction and class initialization are finite bootstrap
        // work. Release the VM gate so Java workers may run, but do not stretch
        // large obfuscated <clinit> blocks into minute-long launches.
        std::this_thread::yield();
    } else if (deterministic_mode) {
        // Deterministic harnesses deliberately retain a small, repeatable
        // pause so timing-sensitive scheduler tests do not become host-load
        // dependent.
        std::this_thread::sleep_for(backoff);
    } else if (thermal_non_render_spin_backoff) {
        PerformanceCounters::record_scheduler_sleep();
        std::this_thread::sleep_for(thermal_spin_backoff);
    } else if (foreground_peer_runnable || periodic_foreground_handoff) {
        // Foreground scheduler fairness must not become a CPU duty-cycle cap.
        // Sustained work here may be class loading, decompression, AI or a
        // non-render worker; sleeping it reduced real throughput by roughly
        // half on the host regression. Render overproduction is handled at the
        // presentation gate where we can prove the extra frames are redundant.
        // A host yield is enough to hand execution to a runnable peer without
        // slowing single-threaded Java work.
        std::this_thread::yield();
    }
    machine.resume_execution_after_blocking(depth);
    if (unpaced_execution) {
        tls_unblocked_quantum_count_ = 0U;
        tls_unblocked_active_microseconds_ = 0U;
    }
    tls_quantum_resume_time_ = std::chrono::steady_clock::now();
    tls_quantum_timing_valid_ = true;
    set_current_state(JavaThreadState::running);
}

void Scheduler::cooperative_yield(Machine& machine) {
    PerformanceCounters::record_scheduler_yield();
    auto current = current_thread_record();
    if (!current) {
        return;
    }

    // Thread.yield() and Thread.sleep(0) are commonly used as frame pacing in
    // older MIDlets. Giving them a real scheduler-sized pause avoids a tight
    // currentTimeMillis/yield loop monopolizing a core on iOS.
    tls_unblocked_quantum_count_ = 0U;
    tls_unblocked_active_microseconds_ = 0U;
    std::optional<std::chrono::steady_clock::time_point> background_deadline;
    {
        std::scoped_lock lock(mutex_);
        if (!deterministic_ &&
            !host_foreground_.load(std::memory_order_acquire)) {
            const auto reservation_time = std::chrono::steady_clock::now();
            if (background_resume_deadline_ < reservation_time) {
                background_resume_deadline_ = reservation_time;
            }
            background_resume_deadline_ += kBackgroundMinimumInterval;
            background_deadline = background_resume_deadline_;
        }
    }
    set_current_state(JavaThreadState::runnable);
    const u32 depth = machine.suspend_execution_for_blocking();
    if (background_deadline.has_value()) {
        std::unique_lock lock(mutex_);
        background_condition_.wait_until(lock, *background_deadline, [this] {
            return host_foreground_.load(std::memory_order_acquire) ||
                   shutting_down_.load(std::memory_order_acquire);
        });
    } else {
        std::this_thread::sleep_for(kExplicitYieldBackoff);
    }
    machine.resume_execution_after_blocking(depth);
    tls_quantum_resume_time_ = std::chrono::steady_clock::now();
    tls_quantum_timing_valid_ = true;
    set_current_state(JavaThreadState::running);
}

Result<SchedulerWaitResult> Scheduler::sleep_current(
    Machine& machine,
    std::chrono::nanoseconds duration) {
    auto current = current_thread_record();
    if (!current) {
        return fail(ErrorCode::invalid_state,
                    "current Java thread is not registered");
    }

    synchronize_current_frame_pacing_state();
    const auto now = std::chrono::steady_clock::now();
    auto effective_duration = duration;
    bool frame_loop_sleep = false;
    const i64 frame_interval_nanoseconds =
        frame_interval_nanoseconds_.load(std::memory_order_acquire);
    const auto pacing_mode = static_cast<FramePacingMode>(
        frame_pacing_mode_.load(std::memory_order_acquire));
    const bool host_foreground =
        host_foreground_.load(std::memory_order_acquire);

    if (tls_frame_boundary_pending_) {
        tls_frame_boundary_pending_ = false;
        const auto boundary_to_sleep = now - tls_frame_boundary_time_;
        const bool eligible =
            pacing_mode == FramePacingMode::override_game_loop &&
            host_foreground &&
            duration >= kMinimumFramePacingRequest &&
            duration <= kMaximumFramePacingRequest &&
            boundary_to_sleep >= std::chrono::steady_clock::duration::zero() &&
            boundary_to_sleep <= kMaximumFrameBoundaryToSleepDelay;
        if (eligible) {
            frame_loop_sleep = true;
            const i64 requested = duration.count();
            std::chrono::nanoseconds active_time {};
            if (tls_frame_loop_wake_valid_ &&
                now >= tls_frame_loop_wake_time_) {
                active_time =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now - tls_frame_loop_wake_time_);
            }

            // Fixed-step games commonly request "target frame time minus work
            // already spent". Compare the reconstructed total frame interval,
            // not the remaining sleep alone, so variable render cost does not
            // prevent a valid loop from reaching the override confidence gate.
            const i64 observed_interval = requested + active_time.count();
            const i64 previous = tls_frame_interval_sample_nanoseconds_;
            bool stable_interval = previous > 0;
            if (stable_interval) {
                const i64 difference = observed_interval >= previous
                    ? observed_interval - previous
                    : previous - observed_interval;
                const i64 tolerance = std::max<i64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        kMinimumSleepStabilityTolerance).count(),
                    std::max(observed_interval, previous) / 5);
                stable_interval = difference <= tolerance;
            }
            tls_frame_interval_sample_nanoseconds_ = observed_interval;
            if (stable_interval) {
                if (tls_frame_pacing_streak_ !=
                    std::numeric_limits<u32>::max()) {
                    ++tls_frame_pacing_streak_;
                }
            } else {
                tls_frame_pacing_streak_ = 1U;
            }

            if (tls_frame_loop_wake_valid_) {
                const auto target_interval = std::chrono::nanoseconds(
                    frame_interval_nanoseconds);
                const auto target_sleep = active_time < target_interval
                    ? target_interval - active_time
                    : std::chrono::nanoseconds::zero();
                if (tls_frame_pacing_streak_ >= kOverrideActivationStreak) {
                    // Loop override may accelerate a slower self-paced game,
                    // but it must never lengthen the MIDlet's own sleep. A
                    // loader that flushes progress then sleeps for 1-2 ms is
                    // not asking the emulator to cap that publication at the
                    // configured FPS; stretching it here serializes asset
                    // loading and was the root cause of per-game pacing hacks.
                    effective_duration = std::min(duration, target_sleep);
                }
            }
        } else {
            tls_frame_pacing_streak_ = 0U;
            tls_frame_interval_sample_nanoseconds_ = 0;
            tls_frame_loop_wake_valid_ = false;
        }
    } else {
        // Any unrelated sleep breaks the render-loop pattern. Its original
        // timeout remains untouched for timers, loading and I/O code.
        tls_frame_pacing_streak_ = 0U;
        tls_frame_interval_sample_nanoseconds_ = 0;
        tls_frame_loop_wake_valid_ = false;
    }

    {
        std::scoped_lock lock(current->mutex_);
        if (current->interrupted_) {
            current->interrupted_ = false;
            tls_frame_pacing_streak_ = 0U;
            tls_frame_interval_sample_nanoseconds_ = 0;
            tls_frame_loop_wake_valid_ = false;
            return SchedulerWaitResult::interrupted;
        }
    }

    if (effective_duration <= std::chrono::nanoseconds::zero()) {
        cooperative_yield(machine);
        if (frame_loop_sleep) {
            tls_frame_loop_wake_time_ = std::chrono::steady_clock::now();
            tls_frame_loop_wake_valid_ = true;
        }
        return SchedulerWaitResult::completed;
    }

    PerformanceCounters::record_scheduler_sleep();
    set_current_state(JavaThreadState::sleeping);
    const u32 depth = machine.suspend_execution_for_blocking();
    bool interrupted = false;
    {
        std::unique_lock lock(current->mutex_);
        current->condition_.wait_for(lock, effective_duration, [&current] {
            return current->interrupted_ || current->stop_requested_;
        });
        interrupted = current->interrupted_;
        if (interrupted) {
            current->interrupted_ = false;
        }
    }
    machine.resume_execution_after_blocking(depth);
    set_current_state(JavaThreadState::running);

    if (frame_loop_sleep && !interrupted) {
        tls_frame_loop_wake_time_ = std::chrono::steady_clock::now();
        tls_frame_loop_wake_valid_ = true;
    } else if (interrupted) {
        tls_frame_pacing_streak_ = 0U;
        tls_frame_interval_sample_nanoseconds_ = 0;
        tls_frame_loop_wake_valid_ = false;
    }
    return interrupted ? SchedulerWaitResult::interrupted
                       : SchedulerWaitResult::completed;
}

Result<SchedulerWaitResult> Scheduler::join_current(
    Machine& machine,
    ObjectRef target,
    std::optional<std::chrono::milliseconds> timeout) {
    std::shared_ptr<JavaThread> target_thread;
    {
        std::scoped_lock lock(mutex_);
        target_thread = find_thread_locked(target);
    }
    if (!target_thread) {
        return SchedulerWaitResult::completed;
    }
    if (target_thread->id_ == current_thread_id()) {
        if (timeout.has_value() && timeout->count() == 0) {
            return SchedulerWaitResult::timed_out;
        }
        return fail(ErrorCode::invalid_argument,
                    "a Java thread cannot join itself indefinitely");
    }

    auto current = current_thread_record();
    if (!current) {
        return fail(ErrorCode::invalid_state,
                    "current Java thread is not registered");
    }
    {
        std::scoped_lock lock(current->mutex_);
        if (current->interrupted_) {
            current->interrupted_ = false;
            return SchedulerWaitResult::interrupted;
        }
    }

    {
        std::scoped_lock lock(target_thread->mutex_);
        if (!target_thread->started_ ||
            target_thread->state_ == JavaThreadState::terminated) {
            return SchedulerWaitResult::completed;
        }
    }

    set_current_state(JavaThreadState::joining);
    const u32 depth = machine.suspend_execution_for_blocking();
    bool completed = false;
    bool interrupted = false;
    {
        std::unique_lock lock(target_thread->mutex_);
        const auto predicate = [&] {
            std::scoped_lock current_lock(current->mutex_);
            interrupted = current->interrupted_;
            return target_thread->state_ == JavaThreadState::terminated ||
                   interrupted || current->stop_requested_;
        };
        if (timeout.has_value()) {
            completed = target_thread->condition_.wait_for(
                lock, *timeout, predicate);
        } else {
            target_thread->condition_.wait(lock, predicate);
            completed = true;
        }
        if (target_thread->state_ == JavaThreadState::terminated) {
            completed = true;
        }
    }
    if (interrupted) {
        std::scoped_lock lock(current->mutex_);
        current->interrupted_ = false;
    }
    machine.resume_execution_after_blocking(depth);
    set_current_state(JavaThreadState::running);

    if (interrupted) {
        return SchedulerWaitResult::interrupted;
    }
    return completed ? SchedulerWaitResult::completed
                     : SchedulerWaitResult::timed_out;
}

Status Scheduler::interrupt(ObjectRef thread_object) {
    std::shared_ptr<JavaThread> thread;
    std::vector<std::shared_ptr<JavaThread>> wake_targets;
    {
        std::scoped_lock lock(mutex_);
        thread = find_thread_locked(thread_object);
        wake_targets.reserve(by_id_.size());
        for (const auto& [id, candidate] : by_id_) {
            (void)id;
            wake_targets.push_back(candidate);
        }
    }
    if (!thread) {
        return {};
    }
    {
        std::scoped_lock lock(thread->mutex_);
        thread->interrupted_ = true;
    }
    // A joining thread waits on the target's condition variable, not its own.
    // Wake every scheduler condition so interruption is observed immediately
    // regardless of the current blocking reason.
    for (const auto& candidate : wake_targets) {
        candidate->condition_.notify_all();
        PerformanceCounters::record_scheduler_event_wakeup();
    }
    return {};
}

bool Scheduler::current_is_interrupted() const noexcept {
    auto current = current_thread_record();
    if (!current) {
        return false;
    }
    std::scoped_lock lock(current->mutex_);
    return current->interrupted_;
}

bool Scheduler::current_stop_requested() const noexcept {
    // Forced shutdown stops MIDlet-owned workers, while the scheduler's main
    // execution thread remains available for destroyApp(true) and final RMS
    // cleanup. Keep this check lock-free because it runs once per bytecode.
    return shutting_down_.load(std::memory_order_acquire) &&
           tls_scheduler_ == this && tls_thread_id_ > 1U;
}

bool Scheduler::consume_current_interrupt() noexcept {
    auto current = current_thread_record();
    if (!current) {
        return false;
    }
    std::scoped_lock lock(current->mutex_);
    const bool interrupted = current->interrupted_;
    current->interrupted_ = false;
    return interrupted;
}

Result<bool> Scheduler::is_interrupted(ObjectRef thread_object) const {
    std::shared_ptr<JavaThread> thread;
    {
        std::scoped_lock lock(mutex_);
        thread = find_thread_locked(thread_object);
    }
    if (!thread) {
        return false;
    }
    std::scoped_lock lock(thread->mutex_);
    return thread->interrupted_;
}

Result<bool> Scheduler::is_alive(ObjectRef thread_object) const {
    std::shared_ptr<JavaThread> thread;
    {
        std::scoped_lock lock(mutex_);
        thread = find_thread_locked(thread_object);
    }
    if (!thread) {
        return false;
    }
    std::scoped_lock lock(thread->mutex_);
    return thread->started_ &&
           thread->state_ != JavaThreadState::terminated;
}

Status Scheduler::set_priority(ObjectRef thread_object, i32 priority) {
    if (priority < 1 || priority > 10) {
        return fail_java("java/lang/IllegalArgumentException",
                         "Thread priority must be in the range 1..10");
    }
    std::shared_ptr<JavaThread> thread;
    {
        std::scoped_lock lock(mutex_);
        thread = find_thread_locked(thread_object);
    }
    if (!thread) {
        return fail(ErrorCode::invalid_state,
                    "java.lang.Thread object is not registered");
    }
    std::scoped_lock lock(thread->mutex_);
    thread->priority_ = priority;
    return {};
}

Result<i32> Scheduler::priority(ObjectRef thread_object) const {
    std::shared_ptr<JavaThread> thread;
    {
        std::scoped_lock lock(mutex_);
        thread = find_thread_locked(thread_object);
    }
    if (!thread) {
        return 5;
    }
    std::scoped_lock lock(thread->mutex_);
    return thread->priority_;
}

void Scheduler::erase_id(std::deque<JavaThreadId>& queue,
                         JavaThreadId id) noexcept {
    PerformanceCounters::record_scheduler_queue_erase_scan(queue.size());
    queue.erase(std::remove(queue.begin(), queue.end(), id), queue.end());
}

void Scheduler::update_queue_membership_locked(JavaThreadId id,
                                                JavaThreadState state) {
    PerformanceCounters::record_scheduler_state_transition();
    erase_id(runnable_queue_, id);
    erase_id(blocked_queue_, id);
    erase_id(sleeping_queue_, id);
    switch (state) {
    case JavaThreadState::runnable:
    case JavaThreadState::running:
        runnable_queue_.push_back(id);
        break;
    case JavaThreadState::blocked_monitor:
    case JavaThreadState::blocked_io:
    case JavaThreadState::waiting:
    case JavaThreadState::joining:
        blocked_queue_.push_back(id);
        break;
    case JavaThreadState::sleeping:
        sleeping_queue_.push_back(id);
        break;
    case JavaThreadState::new_thread:
    case JavaThreadState::terminated:
        break;
    }
}

void Scheduler::set_current_state(JavaThreadState state) noexcept {
    if (state != JavaThreadState::runnable &&
        state != JavaThreadState::running) {
        tls_unblocked_quantum_count_ = 0U;
        tls_unblocked_active_microseconds_ = 0U;
        tls_quantum_timing_valid_ = false;
    } else if (state == JavaThreadState::running &&
               !tls_quantum_timing_valid_) {
        tls_quantum_resume_time_ = std::chrono::steady_clock::now();
        tls_quantum_timing_valid_ = true;
    }
    auto current = current_thread_record();
    if (!current) {
        return;
    }
    JavaThreadState previous = JavaThreadState::new_thread;
    {
        std::scoped_lock lock(current->mutex_);
        previous = current->state_;
        current->state_ = state;
    }
    {
        std::scoped_lock lock(mutex_);
        update_queue_membership_locked(current->id_, state);
    }
    if (previous != state &&
        (is_traceworthy_thread_state(previous) ||
         is_traceworthy_thread_state(state))) {
        vm_trace("thread",
                 "state java=%u %s->%s",
                 static_cast<unsigned>(current->id_),
                 thread_state_name(previous),
                 thread_state_name(state));
    }
}

void Scheduler::publish_current_roots(
    u32 invocation_depth,
    std::span<const ObjectRef> roots) {
    auto current = current_thread_record();
    if (current) {
        current->context_->publish_roots(invocation_depth, roots);
    }
}

std::span<const ObjectRef> Scheduler::exchange_current_roots(
    u32 invocation_depth,
    std::vector<ObjectRef>& roots) {
    auto current = current_thread_record();
    if (current) {
        return current->context_->exchange_roots(invocation_depth, roots);
    }
    return {};
}

void Scheduler::clear_current_roots(u32 invocation_depth) noexcept {
    auto current = current_thread_record();
    if (current) {
        current->context_->clear_roots(invocation_depth);
    }
}

void Scheduler::set_current_pending_exception(
    std::optional<ObjectRef> throwable) noexcept {
    auto current = current_thread_record();
    if (current) {
        current->context_->set_pending_exception(throwable);
    }
}

void Scheduler::add_current_executed_instructions(u64 count) noexcept {
    auto current = current_thread_record();
    if (current) {
        current->context_->add_executed_instructions(count);
    }
}

void Scheduler::append_reference_roots(std::vector<ObjectRef>& roots) const {
    std::vector<std::shared_ptr<JavaThread>> threads;
    {
        std::scoped_lock lock(mutex_);
        threads.reserve(by_id_.size());
        for (const auto& [id, thread] : by_id_) {
            (void)id;
            threads.push_back(thread);
        }
    }
    for (const auto& thread : threads) {
        std::scoped_lock lock(thread->mutex_);
        if (thread->state_ == JavaThreadState::terminated) {
            // A scheduler record may remain so Java references can still query
            // isAlive()/join(), but it must not make a completed Thread and its
            // Runnable graph immortal. Native callback records are pruned on
            // the next native task creation.
            continue;
        }
        if (!thread->object_.is_null()) {
            roots.push_back(thread->object_);
        }
        if (!thread->target_.is_null()) {
            roots.push_back(thread->target_);
        }
        if (thread->uncaught_throwable_.has_value() &&
            !thread->uncaught_throwable_->is_null()) {
            roots.push_back(*thread->uncaught_throwable_);
        }
        thread->context_->append_reference_roots(roots);
    }
}

SchedulerSnapshot Scheduler::snapshot() const {
    SchedulerSnapshot result;
    std::scoped_lock lock(mutex_);
    result.runnable.assign(runnable_queue_.begin(), runnable_queue_.end());
    result.blocked.assign(blocked_queue_.begin(), blocked_queue_.end());
    result.sleeping.assign(sleeping_queue_.begin(), sleeping_queue_.end());
    result.threads.reserve(by_id_.size());
    for (const auto& [id, thread] : by_id_) {
        (void)id;
        result.threads.push_back(thread->snapshot());
    }
    std::sort(result.threads.begin(), result.threads.end(),
              [](const JavaThreadSnapshot& left,
                 const JavaThreadSnapshot& right) {
                  return left.id < right.id;
              });
    return result;
}

void Scheduler::set_deterministic(bool enabled) noexcept {
    std::scoped_lock lock(mutex_);
    deterministic_ = enabled;
}

bool Scheduler::deterministic() const noexcept {
    std::scoped_lock lock(mutex_);
    return deterministic_;
}

void Scheduler::wake_thread(JavaThreadId thread_id) noexcept {
    std::shared_ptr<JavaThread> thread;
    {
        std::scoped_lock lock(mutex_);
        const auto iterator = by_id_.find(thread_id);
        if (iterator != by_id_.end()) {
            thread = iterator->second;
        }
    }
    if (thread) {
        thread->condition_.notify_all();
    }
}

void Scheduler::finish_thread(const std::shared_ptr<JavaThread>& thread,
                              std::optional<ObjectRef> throwable,
                              std::optional<Error> failure) noexcept {
    {
        std::scoped_lock lock(thread->mutex_);
        thread->target_ = {};
        thread->uncaught_throwable_ = throwable;
        thread->native_failure_ = std::move(failure);
        thread->state_ = JavaThreadState::terminated;
    }
    thread->context_->set_pending_exception(throwable);
    thread->condition_.notify_all();
    {
        std::scoped_lock lock(mutex_);
        update_queue_membership_locked(thread->id_,
                                       JavaThreadState::terminated);
    }
    vm_trace("thread",
             "finish java=%u throwable=%d failure=%d",
             static_cast<unsigned>(thread->id_),
             throwable.has_value() ? 1 : 0,
             thread->native_failure_.has_value() ? 1 : 0);
}

void Scheduler::prune_terminated_native_threads() {
    std::vector<std::shared_ptr<JavaThread>> candidates;
    {
        std::scoped_lock lock(mutex_);
        candidates.reserve(by_id_.size());
        for (const auto& [id, thread] : by_id_) {
            if (id != 1U && id != tls_thread_id_) {
                candidates.push_back(thread);
            }
        }
    }

    std::vector<std::shared_ptr<JavaThread>> retired;
    for (const auto& thread : candidates) {
        bool eligible = false;
        {
            std::scoped_lock thread_lock(thread->mutex_);
            eligible = thread->native_task_ &&
                       thread->state_ == JavaThreadState::terminated;
        }
        if (!eligible) continue;

        std::scoped_lock lock(mutex_);
        const auto found = by_id_.find(thread->id_);
        if (found == by_id_.end() || found->second != thread) continue;
        by_object_.erase(thread->object_.bits);
        by_id_.erase(found);
        retired.push_back(thread);
    }
    // Destroy/join completed workers only after releasing scheduler locks.
    retired.clear();
}

void Scheduler::shutdown(MonitorTable* monitors) noexcept {
    std::vector<std::shared_ptr<JavaThread>> threads;
    const JavaThreadId caller_thread_id = current_thread_id();
    {
        std::scoped_lock lock(mutex_);
        if (shutting_down_) {
            return;
        }
        shutting_down_ = true;
        for (const auto& [id, thread] : by_id_) {
            // Preserve whichever execution context initiated shutdown. Runtime
            // may force-destroy from a lifecycle/native host thread rather than
            // the bootstrap ID 1 thread, and that caller still has to execute
            // destroyApp(true) after all MIDlet-owned workers have stopped.
            if (id != caller_thread_id) {
                threads.push_back(thread);
            }
        }
    }
    background_condition_.notify_all();
    for (const auto& thread : threads) {
        {
            std::scoped_lock lock(thread->mutex_);
            thread->stop_requested_ = true;
            thread->interrupted_ = true;
        }
        thread->worker_.request_stop();
        thread->condition_.notify_all();
    }
    if (monitors != nullptr) {
        monitors->clear();
    }
    for (const auto& thread : threads) {
        if (thread->worker_.joinable() &&
            !thread->worker_.is_current_thread()) {
            thread->worker_.join();
        }
    }
}

} // namespace phoneme::vm
