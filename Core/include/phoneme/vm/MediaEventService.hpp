#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "phoneme/media/MediaService.hpp"
#include "phoneme/vm/NativeRootScope.hpp"

namespace phoneme::vm {

class Machine;

struct MediaEventServiceDiagnostics final {
    usize registered_players {0U};
    usize pending_events {0U};
    bool worker_started {false};
    bool polling {false};
};

class MediaEventService final {
public:
    explicit MediaEventService(Machine& machine) noexcept;
    ~MediaEventService();

    MediaEventService(const MediaEventService&) = delete;
    MediaEventService& operator=(const MediaEventService&) = delete;

    [[nodiscard]] Status register_player(i32 player_id,
                                         ObjectRef player);
    void unregister_player(i32 player_id) noexcept;
    [[nodiscard]] Status enqueue(ObjectRef player,
                                 media::MediaEvent event);
    void wake() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] MediaEventServiceDiagnostics diagnostics() const noexcept;

    // Shared emulation event-loop hooks. MMAPI no longer owns a pthread; its
    // polling and listener delivery are multiplexed onto Machine's one event
    // worker together with Timer and LCDUI.
    [[nodiscard]] bool pump_due();
    [[nodiscard]] std::optional<std::chrono::milliseconds>
    time_until_next_event() const noexcept;

private:
    struct Registration final {
        ObjectRef player {};
    };

    struct PendingEvent final {
        NativeRootScope player_root;
        media::MediaEvent event;
    };

    [[nodiscard]] Status queue_event(ObjectRef player,
                                     media::MediaEvent event);
    [[nodiscard]] bool poll_players();
    [[nodiscard]] bool dispatch_one_pending();

    Machine& machine_;
    mutable std::mutex mutex_;
    std::unordered_map<i32, Registration> players_;
    std::deque<PendingEvent> pending_;
    std::chrono::steady_clock::time_point next_poll_deadline_ {};
    bool event_loop_registered_ {false};
    bool continuous_poll_ {false};
    bool poll_requested_ {false};
    bool shutting_down_ {false};
};

} // namespace phoneme::vm
