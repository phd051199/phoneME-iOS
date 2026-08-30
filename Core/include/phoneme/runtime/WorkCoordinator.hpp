#pragma once

#include <atomic>

#include "phoneme/base/Types.hpp"

namespace phoneme::runtime {

enum class WorkClass : u8 {
    frame_critical,
    interactive,
    background,
};

enum class ThermalPressure : u8 {
    nominal,
    fair,
    serious,
    critical,
};

enum class FramePressure : u8 {
    low,
    normal,
    high,
    overloaded,
};

// Central CPU-budget policy shared by rendering and background native work.
// The VM remains single-execution-gate; only host work that does not mutate
// Java heap state is admitted here. Keeping one coordinator prevents the JIT
// and graphics pools from independently consuming every available core.
class WorkCoordinator final {
public:
    [[nodiscard]] u32 helper_limit(WorkClass work_class,
                                   usize item_count) const noexcept;

    void begin_frame_work() noexcept;
    void end_frame_work() noexcept;
    [[nodiscard]] bool background_work_allowed() const noexcept;
    [[nodiscard]] bool try_begin_background_work() noexcept;
    void end_background_work() noexcept;

    void note_frame_interval(u64 elapsed_nanoseconds,
                             u64 target_nanoseconds) noexcept;
    void set_thermal_pressure(ThermalPressure pressure) noexcept;

    [[nodiscard]] ThermalPressure thermal_pressure() const noexcept;
    [[nodiscard]] FramePressure frame_pressure() const noexcept;
    [[nodiscard]] bool frame_boost_active() const noexcept;
    [[nodiscard]] u32 active_frame_jobs() const noexcept;
    [[nodiscard]] u32 active_background_jobs() const noexcept;

private:
    std::atomic<u8> thermal_pressure_ {
        static_cast<u8>(ThermalPressure::nominal)};
    std::atomic<u8> frame_pressure_ {
        static_cast<u8>(FramePressure::normal)};
    std::atomic<u8> frame_boost_frames_ {0U};
    std::atomic<u32> active_frame_jobs_ {0U};
    std::atomic<u32> active_background_jobs_ {0U};
};

[[nodiscard]] WorkCoordinator& shared_work_coordinator() noexcept;

} // namespace phoneme::runtime
