#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "phoneme/vm/JavaThread.hpp"
#include "phoneme/vm/Heap.hpp"

namespace phoneme::vm {

class Machine;
class MonitorTable;

enum class SchedulerWaitResult : u8 {
    completed,
    timed_out,
    interrupted,
};

enum class FramePacingMode : i32 {
    native = 0,
    cap = 1,
    override_game_loop = 2,
};

struct SchedulerSnapshot final {
    std::vector<JavaThreadSnapshot> threads;
    std::vector<JavaThreadId> runnable;
    std::vector<JavaThreadId> blocked;
    std::vector<JavaThreadId> sleeping;
};

using SchedulerNativeTask =
    std::function<Result<std::optional<ObjectRef>>(std::stop_token)>;

class Scheduler final {
public:
    Scheduler();
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    [[nodiscard]] JavaThreadId current_thread_id() const noexcept;
    [[nodiscard]] Result<ObjectRef> current_thread_object() const;
    [[nodiscard]] Status bind_current_thread_object(ObjectRef object);

    [[nodiscard]] Status register_thread(ObjectRef thread_object,
                                         ObjectRef runnable_target);
    [[nodiscard]] Status start_thread(Machine& machine,
                                      ObjectRef thread_object);
    [[nodiscard]] Status start_native_thread(
        Machine& machine,
        ObjectRef thread_object,
        SchedulerNativeTask task);
    [[nodiscard]] Result<ObjectRef> runnable_target(
        ObjectRef thread_object) const;

    void begin_execution_slice() noexcept;
    [[nodiscard]] bool consume_current_background_transition() noexcept;
    void begin_unpaced_execution() noexcept;
    void end_unpaced_execution() noexcept;
    void set_host_foreground(bool foreground) noexcept;
    [[nodiscard]] bool host_foreground() const noexcept {
        return host_foreground_.load(std::memory_order_acquire);
    }
    void configure_frame_pacing(i32 frames_per_second,
                                FramePacingMode mode) noexcept;
    // Harrier/JarlyME-style emulation-turn wakeup. Host input, repaint and
    // other latency-sensitive events call this to cancel any frame remainder
    // sleep immediately instead of waiting for the next display deadline.
    void signal_emulation_event() noexcept;
    void pace_current_frame_publication(Machine& machine);
    void note_current_frame_boundary() noexcept;
    void note_current_frame_request() noexcept;
    void break_current_frame_pacing_sequence() noexcept;
    void cooperative_quantum(Machine& machine);
    void cooperative_yield(Machine& machine);
    [[nodiscard]] Result<SchedulerWaitResult> sleep_current(
        Machine& machine,
        std::chrono::nanoseconds duration);
    [[nodiscard]] Result<SchedulerWaitResult> join_current(
        Machine& machine,
        ObjectRef target,
        std::optional<std::chrono::milliseconds> timeout);

    [[nodiscard]] Status interrupt(ObjectRef thread_object);
    [[nodiscard]] bool current_is_interrupted() const noexcept;
    [[nodiscard]] bool current_stop_requested() const noexcept;
    [[nodiscard]] bool consume_current_interrupt() noexcept;
    [[nodiscard]] Result<bool> is_interrupted(ObjectRef thread_object) const;
    [[nodiscard]] Result<bool> is_alive(ObjectRef thread_object) const;
    [[nodiscard]] Status set_priority(ObjectRef thread_object, i32 priority);
    [[nodiscard]] Result<i32> priority(ObjectRef thread_object) const;

    void set_current_state(JavaThreadState state) noexcept;
    void publish_current_roots(u32 invocation_depth,
                               std::span<const ObjectRef> roots);
    std::span<const ObjectRef> exchange_current_roots(
        u32 invocation_depth,
        std::vector<ObjectRef>& roots);
    void set_current_root_walker(
        u32 invocation_depth,
        void* context,
        ExecutionContext::RootWalker walker,
        bool clear_published_roots = false);
    void set_current_transient_root_walker(
        u32 invocation_depth,
        void* context,
        ExecutionContext::RootWalker walker,
        bool clear_published_roots = false);
    void clear_current_transient_root_walker(
        u32 invocation_depth,
        void* context) noexcept;
    void clear_current_published_roots(u32 invocation_depth) noexcept;
    void clear_current_roots(u32 invocation_depth) noexcept;
    void set_current_pending_exception(
        std::optional<ObjectRef> throwable) noexcept;
    void add_current_executed_instructions(u64 count) noexcept;
    void append_reference_roots(std::vector<ObjectRef>& roots) const;

    [[nodiscard]] SchedulerSnapshot snapshot() const;
    void set_deterministic(bool enabled) noexcept;
    [[nodiscard]] bool deterministic() const noexcept;

    void wake_thread(JavaThreadId thread_id) noexcept;
    [[nodiscard]] bool current_is_fiber() const noexcept;
    void park_current_fiber(
        JavaThreadState state,
        std::optional<std::chrono::steady_clock::time_point> deadline =
            std::nullopt);
    void yield_current_fiber();
    void wake_fibers(JavaThreadState state) noexcept;
    void shutdown(MonitorTable* monitors = nullptr) noexcept;

private:
    [[nodiscard]] std::shared_ptr<JavaThread> find_thread_locked(
        ObjectRef thread_object) const;
    [[nodiscard]] std::shared_ptr<JavaThread> current_thread_record() const;
    void finish_thread(const std::shared_ptr<JavaThread>& thread,
                       std::optional<ObjectRef> throwable,
                       std::optional<Error> failure) noexcept;
    void prune_terminated_native_threads();
    [[nodiscard]] bool ensure_fiber_carrier(Machine& machine);
    void fiber_carrier_loop(Machine& machine,
                            std::stop_token stop_token) noexcept;
    void enqueue_fiber(JavaThreadId thread_id) noexcept;
    void wake_fiber(JavaThreadId thread_id) noexcept;
    void wake_all_fibers_for_shutdown() noexcept;
    [[nodiscard]] bool has_live_fibers() const noexcept;
    void reset_current_frame_pacing_state() noexcept;
    void synchronize_current_frame_pacing_state() noexcept;
    void update_queue_membership_locked(JavaThreadId id,
                                        JavaThreadState state);
    static void erase_id(std::deque<JavaThreadId>& queue,
                         JavaThreadId id) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable background_condition_;
    std::unordered_map<u64, std::shared_ptr<JavaThread>> by_object_;
    std::unordered_map<JavaThreadId, std::shared_ptr<JavaThread>> by_id_;
    std::deque<JavaThreadId> runnable_queue_;
    std::deque<JavaThreadId> blocked_queue_;
    std::deque<JavaThreadId> sleeping_queue_;
    JavaThreadId next_thread_id_ {2};
    bool deterministic_ {false};
    std::atomic_bool host_foreground_ {true};
    std::chrono::steady_clock::time_point background_resume_deadline_ {};
    std::atomic_bool shutting_down_ {false};
    std::atomic<i64> frame_interval_nanoseconds_ {33'333'333};
    std::atomic<i32> frame_pacing_mode_ {
        static_cast<i32>(FramePacingMode::native)
    };
    std::atomic<u64> frame_pacing_generation_ {1U};
    std::atomic<u64> host_foreground_generation_ {1U};
    // Physical-iOS global emulation turn gate. Unlike the per-thread pacing
    // state below, this deadline applies to every guest worker sharing the VM,
    // matching Harrier's single emulation clock: once a frame finishes early,
    // all guest execution idles for the unused frame budget unless an event
    // explicitly wakes the emulation loop.
    std::atomic<i64> emulation_frame_deadline_nanoseconds_ {0};
    std::atomic<i64> emulation_last_frame_nanoseconds_ {0};
    std::atomic<u64> emulation_event_generation_ {1U};

    enum class FiberRunState : u8 {
        runnable,
        running,
        parked,
        finished,
    };

    struct FiberControl final {
        FiberRunState state {FiberRunState::runnable};
        JavaThreadState park_state {JavaThreadState::runnable};
        std::optional<std::chrono::steady_clock::time_point> deadline;
        bool queued {false};
    };

    struct FiberTlsState final {
        u32 unblocked_quantum_count {0U};
        u64 unblocked_active_microseconds {0U};
        u64 host_foreground_generation {0U};
        std::chrono::steady_clock::time_point quantum_resume_time {};
        bool quantum_timing_valid {false};
        u32 unpaced_execution_depth {0U};
        bool frame_boundary_pending {false};
        u32 frame_pacing_streak {0U};
        i64 frame_interval_sample_nanoseconds {0};
        bool frame_loop_wake_valid {false};
        bool frame_cap_deadline_valid {false};
        bool native_frame_boundary_valid {false};
        bool native_backpressure_active {false};
        bool pressure_frame_boundary_valid {false};
        u32 native_fast_frame_streak {0U};
        u64 frame_pacing_generation {0U};
        std::chrono::steady_clock::time_point frame_loop_wake_time {};
        std::chrono::steady_clock::time_point frame_cap_deadline {};
        std::chrono::steady_clock::time_point frame_boundary_time {};
        std::chrono::steady_clock::time_point native_frame_boundary_time {};
        std::chrono::steady_clock::time_point pressure_frame_boundary_time {};
        HeapAccessContext heap_access_context {};
    };

    void save_fiber_tls_state(JavaThreadId thread_id) noexcept;
    void restore_fiber_tls_state(JavaThreadId thread_id) noexcept;

    mutable std::mutex fiber_mutex_;
    std::condition_variable fiber_condition_;
    std::unordered_map<JavaThreadId, FiberControl> fiber_controls_;
    std::unordered_map<JavaThreadId, FiberTlsState> fiber_tls_states_;
    std::deque<JavaThreadId> fiber_runnable_queue_;
    JavaWorkerThread fiber_carrier_;
    bool fiber_carrier_started_ {false};

    static thread_local Scheduler* tls_scheduler_;
    static thread_local JavaThreadId tls_thread_id_;
    static thread_local u32 tls_unblocked_quantum_count_;
    static thread_local u64 tls_unblocked_active_microseconds_;
    static thread_local u64 tls_host_foreground_generation_;
    static thread_local std::chrono::steady_clock::time_point
        tls_quantum_resume_time_;
    static thread_local bool tls_quantum_timing_valid_;
    static thread_local u32 tls_unpaced_execution_depth_;
    static thread_local bool tls_frame_boundary_pending_;
    static thread_local u32 tls_frame_pacing_streak_;
    static thread_local i64 tls_frame_interval_sample_nanoseconds_;
    static thread_local bool tls_frame_loop_wake_valid_;
    static thread_local bool tls_frame_cap_deadline_valid_;
    static thread_local bool tls_native_frame_boundary_valid_;
    static thread_local bool tls_native_backpressure_active_;
    static thread_local bool tls_pressure_frame_boundary_valid_;
    static thread_local u32 tls_native_fast_frame_streak_;
    static thread_local u64 tls_frame_pacing_generation_;
    static thread_local std::chrono::steady_clock::time_point
        tls_frame_loop_wake_time_;
    static thread_local std::chrono::steady_clock::time_point
        tls_frame_cap_deadline_;
    static thread_local std::chrono::steady_clock::time_point
        tls_frame_boundary_time_;
    static thread_local std::chrono::steady_clock::time_point
        tls_native_frame_boundary_time_;
    static thread_local std::chrono::steady_clock::time_point
        tls_pressure_frame_boundary_time_;
};

} // namespace phoneme::vm
