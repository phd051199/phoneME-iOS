#include "phoneme/runtime/CanvasRuntime.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <utility>

#include "phoneme/vm/LcduiBridge.hpp"
#include "phoneme/vm/Machine.hpp"

namespace phoneme::runtime {
namespace {

constexpr i32 kActionUp = 1;
constexpr i32 kActionLeft = 2;
constexpr i32 kActionRight = 5;
constexpr i32 kActionDown = 6;
constexpr i32 kActionFire = 8;
constexpr i32 kActionGameA = 9;
constexpr i32 kActionGameB = 10;
constexpr i32 kActionGameC = 11;
constexpr i32 kActionGameD = 12;
constexpr i32 kScreenUpdated = 3;
constexpr i32 kCanvasComponentType = 22;
constexpr i32 kScreenModeMetadata = -1007;
constexpr usize kDisplayableIdField = 0;
constexpr auto kKeyRepeatInitialDelay = std::chrono::milliseconds(400);
constexpr auto kKeyRepeatInterval = std::chrono::milliseconds(80);
// MIDP posts the initial expose asynchronously. Give a newly-visible Canvas a
// brief chance to finish worker-side setup; an explicit repaint cancels this
// grace period immediately, and Canvases that never repaint still receive the
// mandatory initial paint shortly afterward.
constexpr auto kInitialAutomaticPaintGrace =
    std::chrono::milliseconds(0);
constexpr auto kWorkerInitialAutomaticPaintGrace =
    std::chrono::milliseconds(1500);
constexpr usize kMaximumRepeatsPerPump = 8U;
constexpr u64 kCanvasCallbackInstructionBudget = 5'000'000U;
constexpr u64 kCanvasPaintWatchdogInstructionBudget = 5'000'000U;

class ScopedCharacterTranslationFrame final {
public:
    explicit ScopedCharacterTranslationFrame(vm::Machine& machine) noexcept
        : machine_(machine) {
        machine_.begin_character_translation_frame();
    }

    ~ScopedCharacterTranslationFrame() {
        machine_.end_character_translation_frame();
    }

    ScopedCharacterTranslationFrame(
        const ScopedCharacterTranslationFrame&) = delete;
    ScopedCharacterTranslationFrame& operator=(
        const ScopedCharacterTranslationFrame&) = delete;

private:
    vm::Machine& machine_;
};

class ScopedUnpacedCallback final {
public:
    explicit ScopedUnpacedCallback(vm::Scheduler& scheduler,
                                    bool enabled) noexcept
        : scheduler_(scheduler), enabled_(enabled) {
        if (enabled_) scheduler_.begin_unpaced_execution();
    }

    ~ScopedUnpacedCallback() {
        if (enabled_) scheduler_.end_unpaced_execution();
    }

    ScopedUnpacedCallback(const ScopedUnpacedCallback&) = delete;
    ScopedUnpacedCallback& operator=(const ScopedUnpacedCallback&) = delete;

private:
    vm::Scheduler& scheduler_;
    bool enabled_ {false};
};

[[nodiscard]] i32 saturating_i32(i64 value) noexcept {
    return static_cast<i32>(std::clamp<i64>(
        value,
        static_cast<i64>(std::numeric_limits<i32>::min()),
        static_cast<i64>(std::numeric_limits<i32>::max())));
}

void append_canvas_diagnostic(vm::Machine& machine,
                              std::string_view message) {
    std::u16string diagnostic = u"[Canvas] ";
    diagnostic.reserve(diagnostic.size() + message.size() + 1U);
    for (const char character : message) {
        diagnostic.push_back(static_cast<char16_t>(
            static_cast<unsigned char>(character)));
    }
    diagnostic.push_back(u'\n');
    machine.append_console(diagnostic);
}

} // namespace

CanvasRuntime::CanvasRuntime(vm::Machine& machine,
                             Dimensions dimensions,
                             std::array<i32, 7> keymap)
    : machine_(machine), dimensions_(dimensions), keymap_(keymap) {}

void CanvasRuntime::configure_render_hooks(CanvasRenderHooks hooks) {
    render_hooks_ = std::move(hooks);
}

void CanvasRuntime::set_keymap(std::array<i32, 7> keymap) noexcept {
    keymap_ = keymap;
}

void CanvasRuntime::set_input_capabilities(bool pointer_events,
                                           bool pointer_motion,
                                           bool repeat_events) noexcept {
    pointer_events_supported_ = pointer_events;
    pointer_motion_supported_ = pointer_events && pointer_motion;
    repeat_events_supported_ = repeat_events;
    if (!repeat_events_supported_) {
        for (auto& [key, state] : canvases_) {
            (void)key;
            state.next_key_repeat.clear();
        }
    }
}

void CanvasRuntime::invalidate_translation_rendering() noexcept {
    observed_translation_generation_ = 0U;
    if (CanvasState* state = active_visible_state(); state != nullptr) {
        merge_region(state->repaint_region, full_region());
    }
}

void CanvasRuntime::set_repeat_clock_for_testing(
    std::function<std::chrono::steady_clock::time_point()> clock) {
    repeat_clock_ = clock ? std::move(clock)
                          : [] { return std::chrono::steady_clock::now(); };
}

Status CanvasRuntime::set_dimensions(Dimensions dimensions) {
    if (!dimensions.valid()) {
        return fail(ErrorCode::invalid_argument,
                    "Canvas dimensions must be positive");
    }
    auto entered = machine_.enter_external_execution();
    if (!entered) return entered;
    const auto leave_execution = [](vm::Machine* machine) noexcept {
        machine->leave_external_execution();
    };
    std::unique_ptr<vm::Machine, decltype(leave_execution)> execution_scope(
        &machine_, leave_execution);

    if (dimensions.width == dimensions_.width &&
        dimensions.height == dimensions_.height) {
        return {};
    }
    dimensions_ = dimensions;
    for (u64 key : canvas_order_) {
        auto found = canvases_.find(key);
        if (found == canvases_.end()) continue;
        found->second.size_change_pending = true;
        found->second.game_graphics = {};
        if (host_rendering_active() && found->second.display_visible) {
            merge_region(found->second.repaint_region, full_region());
        } else {
            found->second.repaint_region.reset();
            found->second.flush_region.reset();
            found->second.service_requested = false;
        }
    }
    return {};
}

Status CanvasRuntime::set_host_foreground(bool foreground) {
    if (host_foreground_ == foreground) {
        return {};
    }
    auto entered = machine_.enter_external_execution();
    if (!entered) return entered;
    const auto leave_execution = [](vm::Machine* machine) noexcept {
        machine->leave_external_execution();
    };
    std::unique_ptr<vm::Machine, decltype(leave_execution)> execution_scope(
        &machine_, leave_execution);

    host_foreground_ = foreground;
    machine_.set_serial_callback_coalescing(!foreground);
    // Discard historical Displayable transitions. While detached, only the
    // final active screen matters; replaying every intermediate show/hide edge
    // on foregrounding causes repaint storms and stale UI callbacks.
    visibility_changes_.clear();
    for (u64 key : canvas_order_) {
        const auto found = canvases_.find(key);
        if (found == canvases_.end()) continue;
        set_game_rendering_enabled(
            found->second,
            foreground && host_rendering_enabled_ &&
                found->second.display_visible);
        // Render work is disposable while the host surface is detached. Never
        // retain dirty regions or serviceRepaints state to replay later; showing
        // the Canvas will schedule exactly one fresh full repaint instead.
        found->second.repaint_region.reset();
        found->second.flush_region.reset();
        found->second.service_requested = false;
        visibility_changes_.push_back(VisibilityChange {
            .object = found->second.object,
            .visible = found->second.display_visible,
        });
    }
    return {};
}

Status CanvasRuntime::set_host_rendering_enabled(bool enabled) {
    if (host_rendering_enabled_ == enabled) {
        return {};
    }
    auto entered = machine_.enter_external_execution();
    if (!entered) return entered;
    const auto leave_execution = [](vm::Machine* machine) noexcept {
        machine->leave_external_execution();
    };
    std::unique_ptr<vm::Machine, decltype(leave_execution)> execution_scope(
        &machine_, leave_execution);

    host_rendering_enabled_ = enabled;
    bool scheduled_repaint = false;
    std::vector<std::pair<vm::ObjectRef, std::vector<i32>>> release_callbacks;
    for (u64 key : canvas_order_) {
        const auto found = canvases_.find(key);
        if (found == canvases_.end()) continue;
        CanvasState& state = found->second;
        const bool renderable =
            host_rendering_active() && state.display_visible;
        set_game_rendering_enabled(state, renderable);
        state.repaint_region.reset();
        state.flush_region.reset();
        state.service_requested = false;
        if (!enabled) {
            // Presentation detachment must behave like releasing every host
            // input edge without exposing a Canvas hideNotify lifecycle event.
            // Some MIDlets maintain their own movement flags in keyPressed /
            // keyReleased rather than polling GameCanvas.getKeyStates(), so a
            // silent pressed_keys clear can leave gameplay permanently latched.
            if (!state.pressed_keys.empty()) {
                std::vector<i32> keys(
                    state.pressed_keys.begin(), state.pressed_keys.end());
                std::ranges::sort(keys);
                release_callbacks.emplace_back(state.object, std::move(keys));
            }
            state.key_states = 0;
            state.pressed_keys.clear();
            state.next_key_repeat.clear();
        }
        if (renderable && state.effectively_visible) {
            merge_region(state.repaint_region, full_region());
            scheduled_repaint = true;
        }
    }
    if (!enabled) {
        for (const auto& [object, keys] : release_callbacks) {
            for (const i32 key_code : keys) {
                CanvasState* current = find_state(object);
                if (current == nullptr) break;
                if (suppresses_key_callback(*current, key_code)) continue;
                auto released = invoke_key_callback(
                    *current, "keyReleased", key_code);
                if (!released) return released;
            }
        }
    }
    if (scheduled_repaint) {
        machine_.note_frame_pacing_request();
    }
    return {};
}

void CanvasRuntime::enqueue_key(i32 key_code, bool pressed, u64 sequence) {
    machine_.scheduler().signal_emulation_event();
    PendingInput input {
        .kind = InputKind::key,
        .first = key_code,
        .second = pressed ? 1 : 0,
        .sequence = sequence,
    };
    std::scoped_lock input_lock(inputs_mutex_);
    const auto position = std::upper_bound(
        inputs_.begin(), inputs_.end(), sequence,
        [](u64 value, const PendingInput& candidate) {
            return value < candidate.sequence;
        });
    inputs_.insert(position, input);
    pending_input_count_.store(inputs_.size(), std::memory_order_relaxed);
}

void CanvasRuntime::enqueue_host_key(i32 key_code,
                                     bool pressed,
                                     u64 sequence) {
    machine_.scheduler().signal_emulation_event();
    PendingInput input {
        .kind = InputKind::host_key,
        .first = key_code,
        .second = pressed ? 1 : 0,
        .sequence = sequence,
    };
    std::scoped_lock input_lock(inputs_mutex_);
    const auto position = std::upper_bound(
        inputs_.begin(), inputs_.end(), sequence,
        [](u64 value, const PendingInput& candidate) {
            return value < candidate.sequence;
        });
    inputs_.insert(position, input);
    pending_input_count_.store(inputs_.size(), std::memory_order_relaxed);
}

void CanvasRuntime::enqueue_pointer(i32 x,
                                    i32 y,
                                    i32 action,
                                    u64 sequence) {
    if (!pointer_events_supported_ ||
        (action == kPointerDragged && !pointer_motion_supported_)) {
        return;
    }
    machine_.scheduler().signal_emulation_event();
    PendingInput input {
        .kind = InputKind::pointer,
        .first = x,
        .second = y,
        .third = action,
        .sequence = sequence,
    };
    std::scoped_lock input_lock(inputs_mutex_);
    const auto position = std::upper_bound(
        inputs_.begin(), inputs_.end(), sequence,
        [](u64 value, const PendingInput& candidate) {
            return value < candidate.sequence;
        });
    inputs_.insert(position, input);
    pending_input_count_.store(inputs_.size(), std::memory_order_relaxed);
}

Status CanvasRuntime::pump() {
    // Canvas state is also mutated by LCDUI/GameCanvas natives running on Java
    // worker threads. Serialize the complete host pump with bytecode execution;
    // otherwise repaint traversal can race a Java-side repaint/flush and corrupt
    // the stored render-hook function objects.
    auto entered = machine_.enter_external_execution();
    if (!entered) return entered;
    const auto leave_execution = [](vm::Machine* machine) noexcept {
        machine->leave_external_execution();
    };
    std::unique_ptr<vm::Machine, decltype(leave_execution)> execution_scope(
        &machine_, leave_execution);
    return pump_under_execution();
}

Status CanvasRuntime::try_pump() {
    // Frame acquisition is observational and must never delay a Java game
    // thread. phoneME processes repaint events independently from the native
    // display scanout; if the VM is busy, publish the latest complete frame and
    // let the next host tick process asynchronous repaint work.
    if (!machine_.try_enter_external_execution()) {
        return {};
    }
    const auto leave_execution = [](vm::Machine* machine) noexcept {
        machine->leave_external_execution();
    };
    std::unique_ptr<vm::Machine, decltype(leave_execution)> execution_scope(
        &machine_, leave_execution);
    return pump_under_execution();
}

Status CanvasRuntime::pump_under_execution() {
    if (pumping_) {
        return {};
    }
    pumping_ = true;
    struct PumpGuard final {
        bool& flag;
        ~PumpGuard() { flag = false; }
    } guard {pumping_};

    const bool dispatched_visibility = !visibility_changes_.empty();
    auto visibility = process_visibility_changes();
    if (!visibility) return visibility;
    auto sizes = process_size_changes();
    if (!sizes) return sizes;
    auto inputs = process_inputs();
    if (!inputs) return inputs;
    auto repeats = process_key_repeats();
    if (!repeats) return repeats;
    auto serial_callbacks = machine_.pump_serial_callbacks();
    if (!serial_callbacks) return serial_callbacks;
    auto alert_timeouts = vm::pump_lcdui_alert_timeouts(machine_);
    if (!alert_timeouts) return alert_timeouts;
    const bool dispatched_visibility_after_input =
        !visibility_changes_.empty();
    auto visibility_after_input = process_visibility_changes();
    if (!visibility_after_input) return visibility_after_input;
    auto flushes = process_flushes();
    if (!flushes) return flushes;

    if (const auto& translation = machine_.translation_service(); translation) {
        const u64 generation = translation->generation();
        if (generation != observed_translation_generation_) {
            observed_translation_generation_ = generation;
            if (CanvasState* state = active_visible_state(); state != nullptr) {
                merge_region(state->repaint_region, full_region());
            }
        }
    }

    if (dispatched_visibility || dispatched_visibility_after_input) {
        // phoneME posts visibility and expose/paint as separate LCDUI events.
        // Deferring paint by one pump prevents a Canvas from observing worker-
        // initialized fields before the constructor/startApp turn completes.
        return {};
    }
    return process_repaints();
}

usize CanvasRuntime::estimated_bytes() const noexcept {
    usize total = sizeof(*this);
    total += canvases_.size() * sizeof(CanvasState);
    total += canvas_order_.capacity() * sizeof(u64);
    total += pending_input_count_.load(std::memory_order_relaxed) *
             sizeof(PendingInput);
    total += visibility_changes_.size() * sizeof(VisibilityChange);
    for (const auto& [key, state] : canvases_) {
        (void)key;
        total += state.pressed_keys.size() * sizeof(i32);
        total += state.next_key_repeat.size() *
                 sizeof(std::pair<const i32,
                                  std::chrono::steady_clock::time_point>);
    }
    return total;
}

Status CanvasRuntime::register_canvas(vm::ObjectRef canvas,
                                      bool game_canvas,
                                      bool suppress_key_events) {
    if (canvas.is_null()) {
        return fail(ErrorCode::invalid_argument,
                    "cannot register a null Canvas");
    }
    auto [iterator, inserted] = canvases_.try_emplace(
        canvas.bits,
        CanvasState {
            .object = canvas,
            .game_canvas = game_canvas,
            .suppress_key_events = suppress_key_events,
        });
    if (!inserted) {
        iterator->second.game_canvas = game_canvas;
        iterator->second.suppress_key_events = suppress_key_events;
    } else {
        canvas_order_.push_back(canvas.bits);
        // Registration does not itself change the Canvas viewport. phoneME
        // only calls sizeChanged after an actual host/fullscreen dimension
        // transition, not merely because a Canvas became visible.
    }
    return {};
}

Status CanvasRuntime::set_display_visible(vm::ObjectRef displayable,
                                          bool visible) {
    CanvasState* state = find_state(displayable);
    if (state == nullptr) {
        return {};
    }
    state->display_visible = visible;
    if (visible) {
        active_canvas_ = displayable.bits;
    } else if (active_canvas_.has_value() &&
               *active_canvas_ == displayable.bits) {
        active_canvas_.reset();
    }
    set_game_rendering_enabled(*state, host_rendering_active() && visible);
    if (!host_foreground_) {
        state->repaint_region.reset();
        state->flush_region.reset();
        state->service_requested = false;
        return {};
    }
    visibility_changes_.push_back(VisibilityChange {
        .object = displayable,
        .visible = visible,
    });
    if (render_hooks_.request_host_wake) {
        render_hooks_.request_host_wake();
    }
    return {};
}

Status CanvasRuntime::flush_visibility_callbacks() {
    return process_visibility_changes();
}

Status CanvasRuntime::request_repaint(vm::ObjectRef canvas,
                                      vm::CanvasRect region) {
    auto state = require_state(canvas);
    if (!state) return std::unexpected(state.error());
    if (!host_rendering_active() || !(*state)->display_visible) {
        (*state)->repaint_region.reset();
        (*state)->service_requested = false;
        return {};
    }
    auto clipped = clipped_region(region);
    if (clipped.has_value()) {
        (*state)->initial_automatic_paint_pending = false;
        merge_region((*state)->repaint_region, *clipped);
        machine_.note_frame_pacing_request();
        if (render_hooks_.request_host_wake) {
            render_hooks_.request_host_wake();
        }
    }
    return {};
}

Status CanvasRuntime::pump_blocking_wait_work() {
    // Some legacy MIDlets run a loading animation synchronously from
    // startApp(), using repaint() followed by a short Object.wait(timeout).
    // The host display loop cannot pump while startApp() is still on-stack, so
    // use that blocking interval as the MIDP event-dispatch opportunity.
    return pump();
}

Status CanvasRuntime::request_service_repaints(vm::ObjectRef canvas) {
    auto state = require_state(canvas);
    if (!state) return std::unexpected(state.error());
    if (!host_rendering_active() || !(*state)->display_visible) {
        (*state)->repaint_region.reset();
        (*state)->service_requested = false;
        return {};
    }
    (*state)->service_requested = true;
    if (pumping_ || !(*state)->repaint_region.has_value()) {
        return {};
    }

    // MIDP requires serviceRepaints() to block until the pending paint has
    // completed. Games such as Nicknso call repaint()/serviceRepaints() from
    // their own game thread on every tick. Deferring that paint to the host's
    // framebuffer polling timer coalesces intermediate positions and makes
    // movement visibly stutter. Machine execution is recursively serialized,
    // so pumping here is safe; pumping_ still prevents recursion when
    // serviceRepaints() is called from inside paint/show callbacks.
    auto pumped = pump();
    if (!pumped) return pumped;

    // A newly-visible Canvas deliberately receives showNotify and paint on
    // separate event turns. serviceRepaints() is the explicit synchronous
    // escape hatch, so complete the deferred paint before returning.
    state = require_state(canvas);
    if (!state) return std::unexpected(state.error());
    if ((*state)->repaint_region.has_value() &&
        host_rendering_active() && (*state)->display_visible) {
        return pump();
    }
    return {};
}

Status CanvasRuntime::set_fullscreen(vm::ObjectRef canvas, bool fullscreen) {
    auto state = require_state(canvas);
    if (!state) return std::unexpected(state.error());
    if ((*state)->fullscreen == fullscreen) {
        return {};
    }
    (*state)->fullscreen = fullscreen;
    if (host_rendering_active() && (*state)->display_visible) {
        merge_region((*state)->repaint_region, full_region());
    }

    auto id_value = machine_.heap().field(canvas, kDisplayableIdField);
    if (!id_value) return std::unexpected(id_value.error());
    auto id = id_value->as_int();
    if (!id) return std::unexpected(id.error());
    machine_.emit_ui_event(vm::UiBridgeEvent {
        .kind = kScreenUpdated,
        .component_id = *id,
        .component_type = kCanvasComponentType,
        .arguments = {fullscreen ? 1 : 0, 0, 0, kScreenModeMetadata},
    });
    return {};
}

Result<Dimensions> CanvasRuntime::canvas_dimensions(
    vm::ObjectRef canvas) const {
    auto state = require_state(canvas);
    if (!state) return std::unexpected(state.error());
    return dimensions_;
}

i32 CanvasRuntime::game_action_for_key(i32 key_code) const noexcept {
    if (key_code == keymap_[0] || key_code == '2') return kActionUp;
    if (key_code == keymap_[1] || key_code == '8') return kActionDown;
    if (key_code == keymap_[2] || key_code == '4') return kActionLeft;
    if (key_code == keymap_[3] || key_code == '6') return kActionRight;
    if (key_code == keymap_[4] || key_code == '5') return kActionFire;
    if (key_code == '1') return kActionGameA;
    if (key_code == '3') return kActionGameB;
    if (key_code == '7') return kActionGameC;
    if (key_code == '9') return kActionGameD;
    return 0;
}

Result<i32> CanvasRuntime::key_code_for_action(i32 game_action) const {
    switch (game_action) {
    case kActionUp: return keymap_[0];
    case kActionDown: return keymap_[1];
    case kActionLeft: return keymap_[2];
    case kActionRight: return keymap_[3];
    case kActionFire: return keymap_[4];
    case kActionGameA: return static_cast<i32>('1');
    case kActionGameB: return static_cast<i32>('3');
    case kActionGameC: return static_cast<i32>('7');
    case kActionGameD: return static_cast<i32>('9');
    default:
        return fail_java("java/lang/IllegalArgumentException",
                         "unsupported Canvas game action");
    }
}

std::string CanvasRuntime::key_name(i32 key_code) const {
    const i32 action = game_action_for_key(key_code);
    switch (action) {
    case kActionUp: return "UP";
    case kActionDown: return "DOWN";
    case kActionLeft: return "LEFT";
    case kActionRight: return "RIGHT";
    case kActionFire: return "FIRE";
    case kActionGameA: return "GAME_A";
    case kActionGameB: return "GAME_B";
    case kActionGameC: return "GAME_C";
    case kActionGameD: return "GAME_D";
    default: break;
    }
    if (key_code == keymap_[5]) return "SOFT1";
    if (key_code == keymap_[6]) return "SOFT2";
    if (key_code >= 32 && key_code <= 126) {
        return std::string(1, static_cast<char>(key_code));
    }
    return "KEY_" + std::to_string(key_code);
}

i32 CanvasRuntime::game_key_states(vm::ObjectRef canvas) const noexcept {
    const CanvasState* state = find_state(canvas);
    return state == nullptr ? 0 : state->key_states;
}

Result<vm::ObjectRef> CanvasRuntime::game_graphics(vm::ObjectRef canvas) {
    auto state = require_state(canvas);
    if (!state) return std::unexpected(state.error());
    if (!(*state)->game_canvas) {
        return fail_java("java/lang/IllegalStateException",
                         "Canvas is not a GameCanvas");
    }
    if (!(*state)->game_graphics.is_null()) {
        set_game_rendering_enabled(**state,
                                   host_rendering_active() &&
                                       (*state)->display_visible);
        return (*state)->game_graphics;
    }
    if (!render_hooks_.acquire_game_graphics) {
        return vm::ObjectRef {};
    }
    auto graphics_root = render_hooks_.acquire_game_graphics(
        machine_, canvas, dimensions_);
    if (!graphics_root) return std::unexpected(graphics_root.error());
    auto graphics = graphics_root->get();
    if (!graphics) return std::unexpected(graphics.error());
    (*state)->game_graphics = *graphics;
    set_game_rendering_enabled(**state,
                               host_rendering_active() &&
                                   (*state)->display_visible);
    machine_.begin_character_translation_frame();
    return *graphics;
}

Status CanvasRuntime::request_game_flush(vm::ObjectRef canvas,
                                         vm::CanvasRect region) {
    auto state = require_state(canvas);
    if (!state) return std::unexpected(state.error());
    if (!(*state)->game_canvas) {
        return fail_java("java/lang/IllegalStateException",
                         "Canvas is not a GameCanvas");
    }
    if (!host_rendering_active() || !(*state)->display_visible) {
        (*state)->flush_region.reset();
        return {};
    }
    auto clipped = clipped_region(region);
    if (clipped.has_value()) {
        merge_region((*state)->flush_region, *clipped);
    }
    // GameCanvas.flushGraphics is a synchronous display operation. Publishing
    // here preserves frame ordering and lets pacing observe the exact Java
    // thread that completed the frame. Calls made recursively from a host pump
    // remain deferred until that pump reaches process_flushes().
    if (!pumping_ && clipped.has_value()) {
        return process_flushes();
    }
    return {};
}

void CanvasRuntime::append_reference_roots(
    std::vector<vm::ObjectRef>& roots) const {
    for (u64 key : canvas_order_) {
        const auto found = canvases_.find(key);
        if (found == canvases_.end()) continue;
        if (!found->second.object.is_null()) {
            roots.push_back(found->second.object);
        }
        if (!found->second.game_graphics.is_null()) {
            roots.push_back(found->second.game_graphics);
        }
    }
}

CanvasRuntime::CanvasState* CanvasRuntime::find_state(
    vm::ObjectRef canvas) noexcept {
    const auto found = canvases_.find(canvas.bits);
    return found == canvases_.end() ? nullptr : &found->second;
}

const CanvasRuntime::CanvasState* CanvasRuntime::find_state(
    vm::ObjectRef canvas) const noexcept {
    const auto found = canvases_.find(canvas.bits);
    return found == canvases_.end() ? nullptr : &found->second;
}

Result<CanvasRuntime::CanvasState*> CanvasRuntime::require_state(
    vm::ObjectRef canvas) {
    CanvasState* state = find_state(canvas);
    if (state == nullptr) {
        return fail(ErrorCode::invalid_argument,
                    "Canvas object is not registered");
    }
    return state;
}

Result<const CanvasRuntime::CanvasState*> CanvasRuntime::require_state(
    vm::ObjectRef canvas) const {
    const CanvasState* state = find_state(canvas);
    if (state == nullptr) {
        return fail(ErrorCode::invalid_argument,
                    "Canvas object is not registered");
    }
    return state;
}

vm::CanvasRect CanvasRuntime::full_region() const noexcept {
    return vm::CanvasRect {
        .x = 0,
        .y = 0,
        .width = dimensions_.width,
        .height = dimensions_.height,
    };
}

std::optional<vm::CanvasRect> CanvasRuntime::clipped_region(
    vm::CanvasRect region) const noexcept {
    if (!dimensions_.valid() || region.width <= 0 || region.height <= 0) {
        return std::nullopt;
    }
    const i64 left = std::max<i64>(0, region.x);
    const i64 top = std::max<i64>(0, region.y);
    const i64 right = std::min<i64>(
        dimensions_.width,
        static_cast<i64>(region.x) + static_cast<i64>(region.width));
    const i64 bottom = std::min<i64>(
        dimensions_.height,
        static_cast<i64>(region.y) + static_cast<i64>(region.height));
    if (right <= left || bottom <= top) {
        return std::nullopt;
    }
    return vm::CanvasRect {
        .x = saturating_i32(left),
        .y = saturating_i32(top),
        .width = saturating_i32(right - left),
        .height = saturating_i32(bottom - top),
    };
}

void CanvasRuntime::merge_region(
    std::optional<vm::CanvasRect>& destination,
    vm::CanvasRect region) noexcept {
    if (!destination.has_value()) {
        destination = region;
        return;
    }
    const i64 left = std::min<i64>(destination->x, region.x);
    const i64 top = std::min<i64>(destination->y, region.y);
    const i64 right = std::max<i64>(
        static_cast<i64>(destination->x) + destination->width,
        static_cast<i64>(region.x) + region.width);
    const i64 bottom = std::max<i64>(
        static_cast<i64>(destination->y) + destination->height,
        static_cast<i64>(region.y) + region.height);
    *destination = vm::CanvasRect {
        .x = saturating_i32(left),
        .y = saturating_i32(top),
        .width = saturating_i32(right - left),
        .height = saturating_i32(bottom - top),
    };
}

Status CanvasRuntime::process_visibility_changes() {
    while (!visibility_changes_.empty()) {
        const VisibilityChange change = visibility_changes_.front();
        visibility_changes_.pop_front();
        CanvasState* state = find_state(change.object);
        if (state == nullptr) continue;
        state->display_visible = change.visible;
        auto updated = update_effective_visibility(*state);
        if (!updated) return updated;
    }
    return {};
}

Status CanvasRuntime::process_size_changes() {
    const std::array<vm::Value, 2> arguments {
        vm::Value::from_int(dimensions_.width),
        vm::Value::from_int(dimensions_.height),
    };
    for (u64 key : canvas_order_) {
        auto found = canvases_.find(key);
        if (found == canvases_.end() ||
            !found->second.effectively_visible ||
            !found->second.size_change_pending) {
            continue;
        }
        const vm::ObjectRef canvas = found->second.object;
        found->second.size_change_pending = false;
        auto invoked = invoke_void(canvas,
                                   "javax/microedition/lcdui/Canvas",
                                   "sizeChanged",
                                   "(II)V",
                                   arguments);
        if (!invoked) return invoked;
    }
    return {};
}

Status CanvasRuntime::process_inputs() {
    while (true) {
        PendingInput input;
        {
            std::scoped_lock input_lock(inputs_mutex_);
            if (inputs_.empty()) break;
            input = inputs_.front();
            inputs_.pop_front();
            pending_input_count_.store(inputs_.size(), std::memory_order_relaxed);
        }
        (void)input.sequence;

        CanvasState* state = active_visible_state();
        if (state == nullptr) continue;

        if (input.kind != InputKind::pointer) {
            const bool host_key = input.kind == InputKind::host_key;
            const i32 key_code = input.first;
            const bool pressed = input.second != 0;
            const bool was_pressed = state->pressed_keys.contains(key_code);
            const i32 mask = key_state_mask(key_code);
            const bool suppress_callback =
                suppresses_key_callback(*state, key_code);
            std::string_view callback;
            if (pressed) {
                if (was_pressed) {
                    if (!host_key) {
                        continue;
                    }
                    // Host input carries only press/release edges; scheduled
                    // keyRepeated callbacks are generated below from the repeat
                    // clock. If a release edge is lost by UIKit/SwiftUI, the
                    // next physical press reaches us while the key is still in
                    // pressed_keys. Converting that press to keyRepeated wedges
                    // MIDlets such as Nicknso that implement keyPressed and
                    // keyReleased but leave keyRepeated as the Canvas no-op.
                    // Deliver the new host edge as keyPressed so input recovers
                    // without requiring the Canvas to be recreated.
                    if (repeat_events_supported_ && !suppress_callback) {
                        state->next_key_repeat.insert_or_assign(
                            key_code,
                            repeat_clock_() + kKeyRepeatInitialDelay);
                    }
                    callback = "keyPressed";
                } else {
                    state->pressed_keys.insert(key_code);
                    state->key_states |= mask;
                    if (repeat_events_supported_ && !suppress_callback) {
                        state->next_key_repeat.insert_or_assign(
                            key_code,
                            repeat_clock_() + kKeyRepeatInitialDelay);
                    }
                    callback = "keyPressed";
                }
            } else {
                if (!was_pressed) continue;
                state->pressed_keys.erase(key_code);
                state->next_key_repeat.erase(key_code);
                state->key_states &= ~mask;
                callback = "keyReleased";
            }
            const vm::ObjectRef object = state->object;
            if (!suppress_callback) {
                auto invoked = invoke_key_callback(*state, callback, key_code);
                if (!invoked) return invoked;
            }
            state = find_state(object);
            if (state == nullptr) continue;
        } else {
            std::string_view callback;
            switch (input.third) {
            case kPointerPressed: callback = "pointerPressed"; break;
            case kPointerReleased: callback = "pointerReleased"; break;
            case kPointerDragged: callback = "pointerDragged"; break;
            default: continue;
            }
            if (!pointer_events_supported_ ||
                (input.third == kPointerDragged &&
                 !pointer_motion_supported_)) {
                continue;
            }
            const i32 maximum_x = std::max(0, dimensions_.width - 1);
            const i32 maximum_y = std::max(0, dimensions_.height - 1);
            const i32 clipped_x = std::clamp(input.first, 0, maximum_x);
            const i32 clipped_y = std::clamp(input.second, 0, maximum_y);
            auto invoked = invoke_pointer_callback(
                *state, callback, clipped_x, clipped_y);
            if (!invoked) return invoked;
        }
    }
    return {};
}

Status CanvasRuntime::process_key_repeats() {
    CanvasState* state = active_visible_state();
    if (!repeat_events_supported_ || state == nullptr ||
        state->next_key_repeat.empty()) {
        return {};
    }

    const auto now = repeat_clock_();
    std::vector<i32> due_keys;
    due_keys.reserve(state->next_key_repeat.size());
    for (const auto& [key_code, deadline] : state->next_key_repeat) {
        if (deadline <= now && state->pressed_keys.contains(key_code)) {
            due_keys.push_back(key_code);
        }
    }
    std::sort(due_keys.begin(), due_keys.end());

    const vm::ObjectRef object = state->object;
    for (i32 key_code : due_keys) {
        usize delivered = 0U;
        while (delivered < kMaximumRepeatsPerPump) {
            state = find_state(object);
            if (state == nullptr || !state->effectively_visible ||
                !state->pressed_keys.contains(key_code)) {
                break;
            }
            const auto found = state->next_key_repeat.find(key_code);
            if (found == state->next_key_repeat.end() || found->second > now) {
                break;
            }
            if (suppresses_key_callback(*state, key_code)) {
                state->next_key_repeat.erase(found);
                break;
            }
            found->second += kKeyRepeatInterval;
            auto invoked = invoke_key_callback(*state, "keyRepeated", key_code);
            if (!invoked) return invoked;
            ++delivered;
        }
        state = find_state(object);
        if (state == nullptr) break;
        const auto found = state->next_key_repeat.find(key_code);
        if (found != state->next_key_repeat.end() && found->second <= now) {
            found->second = now + kKeyRepeatInterval;
        }
    }
    return {};
}

Status CanvasRuntime::process_flushes() {
    if (!host_rendering_active()) {
        for (auto& [key, state] : canvases_) {
            (void)key;
            state.flush_region.reset();
        }
        return {};
    }

    std::vector<u64> pending;
    pending.reserve(canvas_order_.size());
    for (u64 key : canvas_order_) {
        const auto found = canvases_.find(key);
        if (found != canvases_.end() &&
            found->second.effectively_visible &&
            found->second.flush_region.has_value()) {
            pending.push_back(key);
        }
    }
    for (u64 key : pending) {
        auto found = canvases_.find(key);
        if (found == canvases_.end() ||
            !found->second.effectively_visible ||
            !found->second.flush_region.has_value()) {
            continue;
        }
        const vm::ObjectRef canvas = found->second.object;
        const vm::CanvasRect region = *found->second.flush_region;
        found->second.flush_region.reset();
        machine_.end_character_translation_frame();
        auto graphics = game_graphics(canvas);
        if (!graphics) return std::unexpected(graphics.error());
        if (render_hooks_.flush_game_graphics) {
            auto flushed = render_hooks_.flush_game_graphics(
                machine_, canvas, *graphics, region);
            if (!flushed) return flushed;
        } else {
            auto current = find_state(canvas);
            if (current != nullptr) {
                merge_region(current->repaint_region, region);
            }
        }
        machine_.begin_character_translation_frame();
    }
    return {};
}

Status CanvasRuntime::process_repaints() {
    if (!host_rendering_active()) {
        for (auto& [key, state] : canvases_) {
            (void)key;
            state.repaint_region.reset();
            state.service_requested = false;
        }
        return {};
    }

    std::vector<u64> pending;
    pending.reserve(canvas_order_.size());
    const auto now = std::chrono::steady_clock::now();
    for (u64 key : canvas_order_) {
        const auto found = canvases_.find(key);
        if (found == canvases_.end() ||
            !found->second.effectively_visible ||
            !found->second.repaint_region.has_value()) {
            continue;
        }
        if (!found->second.initial_paint_completed &&
            found->second.initial_automatic_paint_pending &&
            now < found->second.initial_automatic_paint_deadline) {
            continue;
        }
        pending.push_back(key);
    }
    for (u64 key : pending) {
        auto found = canvases_.find(key);
        if (found == canvases_.end() ||
            !found->second.effectively_visible ||
            !found->second.repaint_region.has_value()) {
            continue;
        }
        const vm::ObjectRef canvas = found->second.object;
        const vm::CanvasRect region = *found->second.repaint_region;
        const bool initial_paint = !found->second.initial_paint_completed;
        found->second.initial_automatic_paint_pending = false;
        found->second.repaint_region.reset();
        found->second.service_requested = false;

        vm::ObjectRef graphics;
        vm::NativeRootScope graphics_root;
        if (render_hooks_.acquire_paint_graphics) {
            auto acquired = render_hooks_.acquire_paint_graphics(
                machine_, canvas, dimensions_, region);
            if (!acquired) return std::unexpected(acquired.error());
            graphics_root = std::move(*acquired);
            auto pinned_graphics = graphics_root.get();
            if (!pinned_graphics) {
                return std::unexpected(pinned_graphics.error());
            }
            graphics = *pinned_graphics;
        }
        auto painted = invoke_paint(canvas, graphics, initial_paint);
        if (!painted) return std::unexpected(painted.error());
        if (render_hooks_.commit_paint) {
            auto committed = render_hooks_.commit_paint(
                machine_, canvas, graphics, region);
            if (!committed) return committed;
        }
        if (*painted) {
            auto current = find_state(canvas);
            if (current != nullptr) current->initial_paint_completed = true;
        }
    }
    return {};
}

CanvasRuntime::CanvasState* CanvasRuntime::active_visible_state() noexcept {
    if (active_canvas_.has_value()) {
        CanvasState* active = find_state(vm::ObjectRef {*active_canvas_});
        if (active != nullptr && active->effectively_visible) return active;
    }
    for (auto iterator = canvas_order_.rbegin();
         iterator != canvas_order_.rend(); ++iterator) {
        const auto found = canvases_.find(*iterator);
        if (found != canvases_.end() && found->second.effectively_visible) {
            active_canvas_ = *iterator;
            return &found->second;
        }
    }
    return nullptr;
}

const CanvasRuntime::CanvasState*
CanvasRuntime::active_visible_state() const noexcept {
    if (active_canvas_.has_value()) {
        const auto found = canvases_.find(*active_canvas_);
        if (found != canvases_.end() && found->second.effectively_visible) {
            return &found->second;
        }
    }
    for (auto iterator = canvas_order_.rbegin();
         iterator != canvas_order_.rend(); ++iterator) {
        const auto found = canvases_.find(*iterator);
        if (found != canvases_.end() && found->second.effectively_visible) {
            return &found->second;
        }
    }
    return nullptr;
}

Result<bool> CanvasRuntime::invoke_paint(
    vm::ObjectRef receiver,
    vm::ObjectRef graphics,
    bool initial_paint) {
    const std::array<vm::Value, 1> arguments {
        vm::Value::from_reference(graphics),
    };
    ScopedCharacterTranslationFrame translation_frame(machine_);
    ScopedUnpacedCallback unpaced(machine_.scheduler(), initial_paint);
    auto result = machine_.invoke_instance(
        receiver,
        "javax/microedition/lcdui/Canvas",
        "paint",
        "(Ljavax/microedition/lcdui/Graphics;)V",
        arguments,
        kCanvasPaintWatchdogInstructionBudget,
        vm::InstructionBudgetMode::progress_watchdog);
    if (!result) {
        const bool budget_exhausted =
            result.error().code == ErrorCode::invalid_state &&
            (result.error().message.starts_with(
                 "VM progress watchdog was exhausted") ||
             result.error().message.starts_with(
                 "VM instruction budget was exhausted"));
        if (budget_exhausted) {
            std::string diagnostic =
                "callback paint(Ljavax/microedition/lcdui/Graphics;)V "
                "exceeded its progress budget: ";
            diagnostic += result.error().message;
            append_canvas_diagnostic(machine_, diagnostic);
            return false;
        }
        return std::unexpected(result.error());
    }
    if (result->completed_normally()) return true;
    if (!result->throwable.has_value()) {
        return fail(ErrorCode::internal_error,
                    "Canvas paint failed without a Java throwable");
    }
    auto throwable = machine_.heap().class_name(*result->throwable);
    if (!throwable) return std::unexpected(throwable.error());
    std::string message = "Canvas callback threw " + *throwable;
    if (!result->exception_context.empty()) {
        message += " from " + result->exception_context;
    }
    append_canvas_diagnostic(machine_, message);

    // paint() is a framework callback, not a Java call made directly by the
    // MIDlet.  Propagating an application exception out through
    // serviceRepaints() kills the caller's game loop, while real MIDP
    // implementations isolate callback failures and continue dispatching
    // subsequent UI/network events.  Treat the frame as incomplete, keep the
    // diagnostic, and let the next repaint recover.  This is especially
    // important for legacy clients whose transient loading state can be
    // internally inconsistent for one frame while protocol messages are being
    // delivered.
    return false;
}

Status CanvasRuntime::invoke_void(
    vm::ObjectRef receiver,
    std::string_view declared_class,
    std::string_view method_name,
    std::string_view descriptor,
    std::span<const vm::Value> arguments,
    CallbackExceptionPolicy exception_policy) {
    auto result = machine_.invoke_instance(receiver,
                                           declared_class,
                                           method_name,
                                           descriptor,
                                           arguments,
                                           kCanvasCallbackInstructionBudget,
                                           vm::InstructionBudgetMode::progress_watchdog);
    if (!result) {
        const bool budget_exhausted =
            result.error().code == ErrorCode::invalid_state &&
            (result.error().message.starts_with(
                 "VM instruction budget was exhausted") ||
             result.error().message.starts_with(
                 "VM progress watchdog was exhausted"));
        if (budget_exhausted) {
            std::string diagnostic = "callback ";
            diagnostic.append(method_name);
            diagnostic.append(descriptor);
            diagnostic += " exceeded instruction budget";
            append_canvas_diagnostic(machine_, diagnostic);
            return {};
        }
        return std::unexpected(result.error());
    }
    if (result->completed_normally()) return {};
    if (!result->throwable.has_value()) {
        return fail(ErrorCode::internal_error,
                    "Canvas callback failed without a Java throwable");
    }
    auto throwable = machine_.heap().class_name(*result->throwable);
    if (!throwable) return std::unexpected(throwable.error());
    std::string message = "Canvas callback threw " + *throwable;
    if (!result->exception_context.empty()) {
        message += " from " + result->exception_context;
    }
    append_canvas_diagnostic(machine_, message);
    if (exception_policy == CallbackExceptionPolicy::report_and_continue) {
        return {};
    }

    return fail_java(*throwable, std::move(message));
}

Status CanvasRuntime::invoke_key_callback(CanvasState& state,
                                          std::string_view method,
                                          i32 key_code) {
    const std::array<vm::Value, 1> arguments {
        vm::Value::from_int(key_code),
    };
    return invoke_void(state.object,
                       "javax/microedition/lcdui/Canvas",
                       method,
                       "(I)V",
                       arguments,
                       CallbackExceptionPolicy::report_and_continue);
}

Status CanvasRuntime::invoke_pointer_callback(CanvasState& state,
                                              std::string_view method,
                                              i32 x,
                                              i32 y) {
    const std::array<vm::Value, 2> arguments {
        vm::Value::from_int(x),
        vm::Value::from_int(y),
    };
    return invoke_void(state.object,
                       "javax/microedition/lcdui/Canvas",
                       method,
                       "(II)V",
                       arguments,
                       CallbackExceptionPolicy::report_and_continue);
}

Status CanvasRuntime::update_effective_visibility(CanvasState& state) {
    const bool desired = host_foreground_ && state.display_visible;
    if (desired == state.effectively_visible) return {};
    state.effectively_visible = desired;
    set_game_rendering_enabled(
        state, desired && host_rendering_enabled_);
    state.repaint_region.reset();
    state.flush_region.reset();
    state.service_requested = false;
    const vm::ObjectRef canvas = state.object;
    if (desired) {
        active_canvas_ = canvas.bits;
        if (!state.initial_paint_completed) {
            const auto scheduler = machine_.scheduler().snapshot();
            const bool has_application_worker = std::any_of(
                scheduler.threads.begin(), scheduler.threads.end(),
                [](const vm::JavaThreadSnapshot& thread) {
                    return thread.id > 1U && thread.alive;
                });
            state.initial_automatic_paint_pending = true;
            state.initial_automatic_paint_deadline =
                std::chrono::steady_clock::now() +
                (has_application_worker
                    ? kWorkerInitialAutomaticPaintGrace
                    : kInitialAutomaticPaintGrace);
        }
        auto shown = invoke_void(
            canvas,
            "javax/microedition/lcdui/Canvas",
            "showNotify",
            "()V",
            {},
            CallbackExceptionPolicy::report_and_continue);
        if (!shown) return shown;
        CanvasState* current = find_state(canvas);
        if (current != nullptr && host_rendering_enabled_) {
            merge_region(current->repaint_region, full_region());
        }
        return {};
    }
    if (active_canvas_.has_value() && *active_canvas_ == canvas.bits) {
        active_canvas_.reset();
    }
    state.initial_automatic_paint_pending = false;
    state.key_states = 0;
    state.pressed_keys.clear();
    state.next_key_repeat.clear();
    return invoke_void(
        canvas,
        "javax/microedition/lcdui/Canvas",
        "hideNotify",
        "()V",
        {},
        CallbackExceptionPolicy::report_and_continue);
}

void CanvasRuntime::set_game_rendering_enabled(
    CanvasState& state,
    bool enabled) noexcept {
    if (!state.game_canvas || state.game_graphics.is_null()) return;
    auto context = machine_.graphics().context(state.game_graphics.bits);
    if (context) {
        (*context)->rendering_enabled = enabled;
    } else {
        state.game_graphics = {};
    }
}

bool CanvasRuntime::suppresses_key_callback(
    const CanvasState& state,
    i32 key_code) const noexcept {
    return state.game_canvas && state.suppress_key_events &&
           game_action_for_key(key_code) != 0;
}

i32 CanvasRuntime::key_state_mask(i32 key_code) const noexcept {
    const i32 action = game_action_for_key(key_code);
    if (action <= 0 || action >= 31) return 0;
    return static_cast<i32>(1U << static_cast<u32>(action));
}

} // namespace phoneme::runtime
