#include "phoneme/runtime/WorkCoordinator.hpp"

#include <algorithm>
#include <thread>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace phoneme::runtime {
namespace {

[[nodiscard]] u32 available_native_helpers() noexcept {
    static const u32 available = []() noexcept {
        const u32 hardware_threads = std::thread::hardware_concurrency();
        if (hardware_threads <= 2U) return 0U;

#if defined(PHONEME_WEB)
        // Emscripten currently provisions a small pthread pool. One helper
        // keeps the VM/browser worker responsive and avoids delayed creation.
        return 1U;
#else
        // iOS benefits most from a small race-to-idle pool. More helpers raise
        // package power quickly and compete with the VM, audio and system UI.
        return std::min<u32>(hardware_threads - 2U, 2U);
#endif
    }();
    return available;
}

} // namespace

u32 WorkCoordinator::helper_limit(WorkClass work_class,
                                  usize item_count) const noexcept {
    const u32 available = available_native_helpers();
    if (available == 0U) return 0U;

    const ThermalPressure thermal = thermal_pressure();
    if (thermal == ThermalPressure::critical) return 0U;

    if (work_class == WorkClass::background) {
        return background_work_allowed() ? 1U : 0U;
    }

    if (thermal == ThermalPressure::serious) {
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
        return 0U;
#else
        return 1U;
#endif
    }

#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
    // Adaptive iPhone policy: stay single-lane while the game is meeting its
    // frame deadline, then temporarily borrow CPU only after real missed
    // frames. The short boost hold avoids 0/1/0/1 helper oscillation on games
    // sitting close to the deadline.
    if (work_class == WorkClass::frame_critical) {
        const FramePressure pressure = frame_pressure();
        if (pressure == FramePressure::overloaded) {
            return std::min<u32>(available, 2U);
        }
        if (pressure == FramePressure::high || frame_boost_active()) {
            return 1U;
        }
        return 0U;
    }
    if (work_class == WorkClass::interactive) {
        return frame_boost_active() ? 1U : 0U;
    }
#endif

    // Medium jobs use one helper. Reserve two helpers for genuinely large
    // frame work where finishing sooner creates an actual idle window.
    constexpr usize kLargeFrameItems = 96U * 1024U;
    if (work_class == WorkClass::frame_critical &&
        item_count >= kLargeFrameItems) {
        return available;
    }
    return 1U;
}

void WorkCoordinator::begin_frame_work() noexcept {
    active_frame_jobs_.fetch_add(1U, std::memory_order_acq_rel);
}

void WorkCoordinator::end_frame_work() noexcept {
    u32 current = active_frame_jobs_.load(std::memory_order_acquire);
    while (current != 0U &&
           !active_frame_jobs_.compare_exchange_weak(
               current, current - 1U,
               std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }
}

bool WorkCoordinator::background_work_allowed() const noexcept {
    if (active_frame_jobs_.load(std::memory_order_acquire) != 0U) return false;
    if (thermal_pressure() >= ThermalPressure::serious) return false;
    return frame_pressure() < FramePressure::high;
}

bool WorkCoordinator::try_begin_background_work() noexcept {
    if (!background_work_allowed()) return false;
    u32 expected = 0U;
    if (!active_background_jobs_.compare_exchange_strong(
            expected, 1U,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    // Frame work wins races against background admission. If a frame started
    // between the permission check and CAS, immediately release the token.
    if (!background_work_allowed()) {
        active_background_jobs_.store(0U, std::memory_order_release);
        return false;
    }
    return true;
}

void WorkCoordinator::end_background_work() noexcept {
    active_background_jobs_.store(0U, std::memory_order_release);
}

void WorkCoordinator::note_frame_interval(u64 elapsed_nanoseconds,
                                          u64 target_nanoseconds) noexcept {
    if (target_nanoseconds == 0U || elapsed_nanoseconds == 0U) return;

    FramePressure pressure = FramePressure::normal;
    // This sample is publication-to-publication cadence, not pure CPU time.
    // A healthy paced game therefore lands around 1.0x target and must remain
    // NORMAL. Only missed deadlines suppress background JIT; fast frames mean
    // spare throughput (or overproduction handled separately by backpressure).
    if (elapsed_nanoseconds * 10U <= target_nanoseconds * 8U) {
        pressure = FramePressure::low;
    } else if (elapsed_nanoseconds * 10U <= target_nanoseconds * 11U) {
        pressure = FramePressure::normal;
    } else if (elapsed_nanoseconds * 10U <= target_nanoseconds * 14U) {
        pressure = FramePressure::high;
    } else {
        pressure = FramePressure::overloaded;
    }
    frame_pressure_.store(static_cast<u8>(pressure), std::memory_order_release);

    // Hold a short performance burst after a miss. This is intentionally
    // frame-count based rather than time based: a 60 Hz game recovers quickly,
    // while a 30 Hz game gets a slightly longer wall-clock recovery window.
    u8 boost = frame_boost_frames_.load(std::memory_order_acquire);
    if (pressure == FramePressure::overloaded) {
        frame_boost_frames_.store(12U, std::memory_order_release);
    } else if (pressure == FramePressure::high) {
        frame_boost_frames_.store(std::max<u8>(boost, 6U),
                                  std::memory_order_release);
    } else if (boost != 0U) {
        frame_boost_frames_.store(static_cast<u8>(boost - 1U),
                                  std::memory_order_release);
    }
}

void WorkCoordinator::set_thermal_pressure(ThermalPressure pressure) noexcept {
    thermal_pressure_.store(static_cast<u8>(pressure), std::memory_order_release);
}

ThermalPressure WorkCoordinator::thermal_pressure() const noexcept {
    return static_cast<ThermalPressure>(
        thermal_pressure_.load(std::memory_order_acquire));
}

FramePressure WorkCoordinator::frame_pressure() const noexcept {
    return static_cast<FramePressure>(
        frame_pressure_.load(std::memory_order_acquire));
}

bool WorkCoordinator::frame_boost_active() const noexcept {
    return frame_boost_frames_.load(std::memory_order_acquire) != 0U;
}

u32 WorkCoordinator::active_frame_jobs() const noexcept {
    return active_frame_jobs_.load(std::memory_order_acquire);
}

u32 WorkCoordinator::active_background_jobs() const noexcept {
    return active_background_jobs_.load(std::memory_order_acquire);
}

WorkCoordinator& shared_work_coordinator() noexcept {
    static WorkCoordinator coordinator;
    return coordinator;
}

} // namespace phoneme::runtime
