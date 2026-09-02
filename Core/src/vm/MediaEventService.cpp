#include "phoneme/vm/MediaEventService.hpp"

#include <chrono>
#include <utility>
#include <vector>

#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/MediaEventDispatch.hpp"

namespace phoneme::vm {
namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(25);

} // namespace

MediaEventService::MediaEventService(Machine& machine) noexcept
    : machine_(machine) {}

MediaEventService::~MediaEventService() {
    shutdown();
}

Status MediaEventService::register_player(i32 player_id,
                                          ObjectRef player) {
    if (player_id <= 0 || player.is_null()) {
        return fail(ErrorCode::invalid_argument,
                    "media event registration is invalid");
    }

    {
        std::scoped_lock lock(mutex_);
        if (shutting_down_) {
            return fail(ErrorCode::invalid_state,
                        "media event service is shutting down");
        }
        players_.insert_or_assign(player_id, Registration {.player = player});
        poll_requested_ = true;
    }
    auto started = machine_.ensure_emulation_event_worker();
    if (!started) {
        std::scoped_lock lock(mutex_);
        players_.erase(player_id);
        return started;
    }
    {
        std::scoped_lock lock(mutex_);
        event_loop_registered_ = true;
    }
    machine_.wake_emulation_event_worker();
    return {};
}

void MediaEventService::unregister_player(i32 player_id) noexcept {
    {
        std::scoped_lock lock(mutex_);
        players_.erase(player_id);
        poll_requested_ = true;
    }
    machine_.wake_emulation_event_worker();
}

void MediaEventService::wake() noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (shutting_down_) return;
        poll_requested_ = true;
    }
    machine_.wake_emulation_event_worker();
}

MediaEventServiceDiagnostics MediaEventService::diagnostics() const noexcept {
    std::scoped_lock lock(mutex_);
    return MediaEventServiceDiagnostics {
        .registered_players = players_.size(),
        .pending_events = pending_.size(),
        .worker_started = event_loop_registered_,
        .polling = poll_requested_ || continuous_poll_,
    };
}

void MediaEventService::shutdown() noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (shutting_down_) return;
        shutting_down_ = true;
        players_.clear();
        pending_.clear();
        poll_requested_ = true;
        continuous_poll_ = false;
    }
    machine_.wake_emulation_event_worker();
}

Status MediaEventService::enqueue(ObjectRef player,
                                  media::MediaEvent event) {
    return queue_event(player, std::move(event));
}

Status MediaEventService::queue_event(ObjectRef player,
                                      media::MediaEvent event) {
    auto root = machine_.pin_native_root(player);
    if (!root) return std::unexpected(root.error());

    std::scoped_lock lock(mutex_);
    if (shutting_down_) return {};
    pending_.push_back(PendingEvent {
        .player_root = std::move(*root),
        .event = std::move(event),
    });
    machine_.wake_emulation_event_worker();
    return {};
}

bool MediaEventService::pump_due() {
    bool should_poll = false;
    bool has_pending = false;
    const auto now = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock(mutex_);
        if (shutting_down_) return false;
        should_poll = poll_requested_ ||
            (continuous_poll_ && now >= next_poll_deadline_);
        if (should_poll) poll_requested_ = false;
        has_pending = !pending_.empty();
    }

    bool did_work = false;
    if (should_poll) {
        // A registered Player only needs periodic synchronization while the
        // platform backend is actually playing. Realized/prefetched/stopped
        // players leave the shared emulation event loop fully asleep.
        const bool continuous = poll_players();
        {
            std::scoped_lock lock(mutex_);
            continuous_poll_ = continuous;
            next_poll_deadline_ = std::chrono::steady_clock::now() +
                                  kPollInterval;
            has_pending = !pending_.empty();
        }
        did_work = true;
    }
    if (has_pending && dispatch_one_pending()) did_work = true;
    return did_work;
}

std::optional<std::chrono::milliseconds>
MediaEventService::time_until_next_event() const noexcept {
    std::scoped_lock lock(mutex_);
    if (shutting_down_) return std::nullopt;
    if (!pending_.empty() || poll_requested_) {
        return std::chrono::milliseconds::zero();
    }
    if (!continuous_poll_) return std::nullopt;
    const auto now = std::chrono::steady_clock::now();
    if (next_poll_deadline_ <= now) {
        return std::chrono::milliseconds::zero();
    }
    auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
        next_poll_deadline_ - now);
    if (delay <= std::chrono::milliseconds::zero()) {
        delay = std::chrono::milliseconds(1);
    }
    return delay;
}

bool MediaEventService::poll_players() {
    std::vector<std::pair<i32, ObjectRef>> players;
    {
        std::scoped_lock lock(mutex_);
        players.reserve(players_.size());
        for (const auto& [player_id, registration] : players_) {
            players.emplace_back(player_id, registration.player);
        }
    }

    bool needs_continuous_poll = false;
    for (const auto& [player_id, player] : players) {
        if (!machine_.heap().class_name(player)) {
            (void)machine_.media().close(player_id);
            unregister_player(player_id);
            continue;
        }

        auto event = machine_.media().synchronize(player_id);
        if (event && event->has_value()) {
            const bool closed =
                (**event).kind == media::MediaEventKind::closed;
            (void)queue_event(player, std::move(**event));
            if (closed) {
                unregister_player(player_id);
                continue;
            }
        }

        auto poll_needed = machine_.media().event_poll_needed(player_id);
        if (poll_needed && *poll_needed) {
            needs_continuous_poll = true;
        }
    }
    return needs_continuous_poll;
}

bool MediaEventService::dispatch_one_pending() {
    std::optional<PendingEvent> pending;
    {
        std::scoped_lock lock(mutex_);
        if (pending_.empty() || shutting_down_) return false;
        pending.emplace(std::move(pending_.front()));
        pending_.pop_front();
    }
    auto player = pending->player_root.get();
    if (!player) return true;
    // Listener failures are isolated to the listener invocation. A broken
    // listener must not terminate the shared MMAPI dispatcher and starve
    // every other Player in the suite.
    (void)dispatch_media_event(machine_, *player, pending->event);
    return true;
}

} // namespace phoneme::vm
