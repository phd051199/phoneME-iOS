#include "phoneme/runtime/ParallelExecutor.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <string_view>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace phoneme::runtime {
namespace {

constexpr u32 kMaximumNativeWorkers = 8U;
thread_local ParallelExecutor* g_active_executor = nullptr;

[[nodiscard]] u32 configured_worker_limit() noexcept {
    const char* value = std::getenv("PHONEME_NATIVE_WORKERS");
    if (value != nullptr && *value != '\0') {
        char* end = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (errno == 0 && end != value && *end == '\0') {
            return static_cast<u32>(std::min<unsigned long>(
                parsed,
                static_cast<unsigned long>(kMaximumNativeWorkers)));
        }
    }

    const u32 hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads <= 2U) return 0U;

#if defined(PHONEME_WEB)
    // Synchronous helper dispatch is a net loss in browser Wasm for the frame
    // sized jobs this executor handles most often (240x320 conversion, fills,
    // blits). Waking an Emscripten pthread and then blocking the submitting VM
    // worker costs more than the scalar/SIMD loop and adds frame-time jitter.
    // Guest Java/lifecycle threads still use the Emscripten pthread pool; this
    // only keeps shared native compute jobs on their submitting worker.
    return 0U;
#else
    // Keep the persistent native compute pool deliberately small. The caller
    // participates, so two helpers already allow a frame job to use three CPU
    // lanes briefly. WorkCoordinator may admit fewer helpers dynamically.
    return std::min<u32>(hardware_threads - 2U, 2U);
#endif
}

} // namespace

ParallelExecutor::ParallelExecutor(u32 worker_limit) noexcept
    : worker_limit_(worker_limit == 0U
          ? configured_worker_limit()
          : std::min(worker_limit, kMaximumNativeWorkers)) {}

ParallelExecutor::~ParallelExecutor() {
    {
        std::scoped_lock lock(state_mutex_);
        stopping_ = true;
    }
    work_condition_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

u32 ParallelExecutor::active_worker_count() const noexcept {
    std::scoped_lock lock(state_mutex_);
    return static_cast<u32>(workers_.size());
}

void ParallelExecutor::ensure_workers(u32 required_worker_count) {
    if (worker_limit_ == 0U || required_worker_count == 0U) return;
    const u32 target = std::min(worker_limit_, required_worker_count);
    std::scoped_lock lock(state_mutex_);
    if (workers_.size() >= target) return;
    workers_.reserve(worker_limit_);
    const u64 observed_generation = generation_;
    while (workers_.size() < target) {
        workers_.emplace_back([this, observed_generation] {
            worker_loop(observed_generation);
        });
    }
}

void ParallelExecutor::parallel_for(usize item_count,
                                    usize minimum_parallel_items,
                                    usize chunk_items,
                                    const Task& task) {
    parallel_for(WorkClass::frame_critical,
                 item_count,
                 minimum_parallel_items,
                 chunk_items,
                 task);
}

void ParallelExecutor::parallel_for(WorkClass work_class,
                                    usize item_count,
                                    usize minimum_parallel_items,
                                    usize chunk_items,
                                    const Task& task) {
    parallel_for(work_class,
                 item_count,
                 item_count,
                 minimum_parallel_items,
                 chunk_items,
                 task);
}

void ParallelExecutor::parallel_for(WorkClass work_class,
                                    usize work_units,
                                    usize item_count,
                                    usize minimum_parallel_items,
                                    usize chunk_items,
                                    const Task& task) {
    if (!task || item_count == 0U) return;
    if (g_active_executor == this || worker_limit_ == 0U ||
        item_count < minimum_parallel_items) {
        task(0U, item_count);
        return;
    }

    WorkCoordinator& coordinator = shared_work_coordinator();
    const u32 helper_count = std::min(
        worker_limit_, coordinator.helper_limit(work_class, work_units));
    if (helper_count == 0U) {
        task(0U, item_count);
        return;
    }

    std::unique_lock submission_lock(submission_mutex_);
    ensure_workers(helper_count);

    u32 admitted_helpers = 0U;
    {
        std::scoped_lock lock(state_mutex_);
        admitted_helpers = std::min<u32>(
            helper_count, static_cast<u32>(workers_.size()));
        if (admitted_helpers != 0U) {
            current_task_ = &task;
            current_item_count_ = item_count;
            current_chunk_items_ = std::max<usize>(chunk_items, 1U);
            current_helper_count_ = admitted_helpers;
            helper_slots_remaining_ = admitted_helpers;
            next_item_.store(0U, std::memory_order_release);
            completed_workers_ = 0U;
            ++generation_;
        }
    }
    if (admitted_helpers == 0U) {
        task(0U, item_count);
        return;
    }

    const bool frame_work = work_class == WorkClass::frame_critical;
    if (frame_work) coordinator.begin_frame_work();
    if (admitted_helpers == 1U) {
        work_condition_.notify_one();
    } else {
        work_condition_.notify_all();
    }

    // The submitting VM/native thread is also a worker. This keeps latency low
    // and means an N-worker pool consumes N+1 cores only while useful work is
    // actually available.
    drain_current_job();

    {
        std::unique_lock lock(state_mutex_);
        completion_condition_.wait(lock, [this] {
            return completed_workers_ == current_helper_count_;
        });
        current_task_ = nullptr;
        current_item_count_ = 0U;
        current_helper_count_ = 0U;
        helper_slots_remaining_ = 0U;
    }
    if (frame_work) coordinator.end_frame_work();
}

void ParallelExecutor::worker_loop(u64 observed_generation) noexcept {
    for (;;) {
        bool participates = false;
        {
            std::unique_lock lock(state_mutex_);
            work_condition_.wait(lock, [this, observed_generation] {
                return stopping_ || generation_ != observed_generation;
            });
            if (stopping_) return;
            observed_generation = generation_;
            if (helper_slots_remaining_ != 0U) {
                --helper_slots_remaining_;
                participates = true;
            }
        }

        if (!participates) continue;
        drain_current_job();

        {
            std::scoped_lock lock(state_mutex_);
            ++completed_workers_;
            if (completed_workers_ == current_helper_count_) {
                completion_condition_.notify_one();
            }
        }
    }
}

void ParallelExecutor::drain_current_job() noexcept {
    ParallelExecutor* previous_executor = g_active_executor;
    g_active_executor = this;
    for (;;) {
        const usize begin = next_item_.fetch_add(
            current_chunk_items_, std::memory_order_relaxed);
        if (begin >= current_item_count_) break;
        const usize end = std::min(
            begin + current_chunk_items_, current_item_count_);
        (*current_task_)(begin, end);
    }
    g_active_executor = previous_executor;
}

ParallelExecutor& shared_compute_executor() noexcept {
    static ParallelExecutor executor;
    return executor;
}

} // namespace phoneme::runtime
