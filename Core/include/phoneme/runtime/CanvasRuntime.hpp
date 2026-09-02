#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "phoneme/base/Error.hpp"
#include "phoneme/base/Types.hpp"
#include "phoneme/vm/CanvasBridge.hpp"
#include "phoneme/vm/NativeRootScope.hpp"

namespace phoneme::vm {
class Machine;
}

namespace phoneme::runtime {

struct CanvasRenderHooks final {
    using AcquirePaintGraphics = std::function<Result<vm::NativeRootScope>(
        vm::Machine&, vm::ObjectRef, Dimensions, vm::CanvasRect)>;
    using CommitPaint = std::function<Status(
        vm::Machine&, vm::ObjectRef, vm::ObjectRef, vm::CanvasRect)>;
    using AcquireGameGraphics = std::function<Result<vm::NativeRootScope>(
        vm::Machine&, vm::ObjectRef, Dimensions)>;
    using FlushGameGraphics = std::function<Status(
        vm::Machine&, vm::ObjectRef, vm::ObjectRef, vm::CanvasRect)>;
    using RequestHostWake = std::function<void()>;

    AcquirePaintGraphics acquire_paint_graphics;
    CommitPaint commit_paint;
    AcquireGameGraphics acquire_game_graphics;
    FlushGameGraphics flush_game_graphics;
    RequestHostWake request_host_wake;
};

class CanvasRuntime final : public vm::CanvasBridge {
public:
    static constexpr i32 kPointerPressed = 1;
    static constexpr i32 kPointerReleased = 2;
    static constexpr i32 kPointerDragged = 3;

    CanvasRuntime(vm::Machine& machine,
                  Dimensions dimensions,
                  std::array<i32, 7> keymap);

    CanvasRuntime(const CanvasRuntime&) = delete;
    CanvasRuntime& operator=(const CanvasRuntime&) = delete;

    void configure_render_hooks(CanvasRenderHooks hooks);
    void set_keymap(std::array<i32, 7> keymap) noexcept;
    void set_input_capabilities(bool pointer_events,
                                bool pointer_motion,
                                bool repeat_events) noexcept;
    void invalidate_translation_rendering() noexcept;
    void set_repeat_clock_for_testing(
        std::function<std::chrono::steady_clock::time_point()> clock);
    [[nodiscard]] Status set_dimensions(Dimensions dimensions);
    [[nodiscard]] Status set_host_foreground(bool foreground);
    [[nodiscard]] Status set_host_rendering_enabled(bool enabled);
    void enqueue_key(i32 key_code, bool pressed, u64 sequence);
    void enqueue_host_key(i32 key_code, bool pressed, u64 sequence);
    void enqueue_pointer(i32 x, i32 y, i32 action, u64 sequence);
    [[nodiscard]] Status pump();
    [[nodiscard]] Status try_pump();
    [[nodiscard]] usize estimated_bytes() const noexcept;

    [[nodiscard]] Status register_canvas(vm::ObjectRef canvas,
                                         bool game_canvas,
                                         bool suppress_key_events) override;
    [[nodiscard]] Status set_display_visible(vm::ObjectRef displayable,
                                             bool visible) override;
    [[nodiscard]] Status flush_visibility_callbacks() override;
    [[nodiscard]] Status request_repaint(vm::ObjectRef canvas,
                                         vm::CanvasRect region) override;
    [[nodiscard]] Status request_service_repaints(vm::ObjectRef canvas) override;
    [[nodiscard]] Status pump_blocking_wait_work() override;
    [[nodiscard]] Status set_fullscreen(vm::ObjectRef canvas,
                                        bool fullscreen) override;
    [[nodiscard]] Result<Dimensions> canvas_dimensions(
        vm::ObjectRef canvas) const override;
    [[nodiscard]] Dimensions display_dimensions() const noexcept override {
        return dimensions_;
    }
    [[nodiscard]] bool pointer_events_supported() const noexcept override {
        return pointer_events_supported_;
    }
    [[nodiscard]] bool pointer_motion_supported() const noexcept override {
        return pointer_motion_supported_;
    }
    [[nodiscard]] bool repeat_events_supported() const noexcept override {
        return repeat_events_supported_;
    }
    [[nodiscard]] i32 game_action_for_key(i32 key_code) const noexcept override;
    [[nodiscard]] Result<i32> key_code_for_action(i32 game_action) const override;
    [[nodiscard]] std::string key_name(i32 key_code) const override;
    [[nodiscard]] i32 game_key_states(vm::ObjectRef canvas) const noexcept override;
    [[nodiscard]] Result<vm::ObjectRef> game_graphics(vm::ObjectRef canvas) override;
    [[nodiscard]] Status request_game_flush(vm::ObjectRef canvas,
                                            vm::CanvasRect region) override;
    void append_reference_roots(
        std::vector<vm::ObjectRef>& roots) const override;

private:
    struct CanvasState final {
        vm::ObjectRef object;
        bool game_canvas {false};
        bool suppress_key_events {false};
        bool display_visible {false};
        bool effectively_visible {false};
        bool fullscreen {false};
        bool size_change_pending {false};
        bool service_requested {false};
        bool initial_paint_completed {false};
        bool initial_automatic_paint_pending {false};
        std::chrono::steady_clock::time_point
            initial_automatic_paint_deadline {};
        i32 key_states {0};
        std::unordered_set<i32> pressed_keys;
        std::unordered_map<i32, std::chrono::steady_clock::time_point>
            next_key_repeat;
        std::optional<vm::CanvasRect> repaint_region;
        std::optional<vm::CanvasRect> flush_region;
        vm::ObjectRef game_graphics;
    };

    enum class InputKind : u8 { key, host_key, pointer };
    enum class CallbackExceptionPolicy : u8 {
        fail_midlet,
        report_and_continue,
    };

    struct PendingInput final {
        InputKind kind {InputKind::key};
        i32 first {0};
        i32 second {0};
        i32 third {0};
        u64 sequence {0};
    };

    struct VisibilityChange final {
        vm::ObjectRef object;
        bool visible {false};
    };

    [[nodiscard]] CanvasState* find_state(vm::ObjectRef canvas) noexcept;
    [[nodiscard]] const CanvasState* find_state(vm::ObjectRef canvas) const noexcept;
    [[nodiscard]] Result<CanvasState*> require_state(vm::ObjectRef canvas);
    [[nodiscard]] Result<const CanvasState*> require_state(vm::ObjectRef canvas) const;
    [[nodiscard]] vm::CanvasRect full_region() const noexcept;
    [[nodiscard]] std::optional<vm::CanvasRect> clipped_region(
        vm::CanvasRect region) const noexcept;
    static void merge_region(std::optional<vm::CanvasRect>& destination,
                             vm::CanvasRect region) noexcept;
    [[nodiscard]] Status pump_under_execution();
    [[nodiscard]] Status process_visibility_changes();
    [[nodiscard]] Status process_size_changes();
    [[nodiscard]] Status process_inputs();
    [[nodiscard]] Status process_key_repeats();
    [[nodiscard]] Status process_flushes();
    [[nodiscard]] Status process_repaints();
    [[nodiscard]] CanvasState* active_visible_state() noexcept;
    [[nodiscard]] const CanvasState* active_visible_state() const noexcept;
    [[nodiscard]] Result<bool> invoke_paint(
        vm::ObjectRef receiver,
        vm::ObjectRef graphics,
        bool initial_paint);
    [[nodiscard]] Status invoke_void(
        vm::ObjectRef receiver,
        std::string_view declared_class,
        std::string_view method_name,
        std::string_view descriptor,
        std::span<const vm::Value> arguments = {},
        CallbackExceptionPolicy exception_policy =
            CallbackExceptionPolicy::fail_midlet);
    [[nodiscard]] Status invoke_key_callback(CanvasState& state,
                                             std::string_view method,
                                             i32 key_code);
    [[nodiscard]] Status invoke_pointer_callback(CanvasState& state,
                                                 std::string_view method,
                                                 i32 x,
                                                 i32 y);
    [[nodiscard]] Status update_effective_visibility(CanvasState& state);
    [[nodiscard]] bool host_rendering_active() const noexcept {
        return host_foreground_ && host_rendering_enabled_;
    }
    void set_game_rendering_enabled(CanvasState& state,
                                    bool enabled) noexcept;
    [[nodiscard]] bool suppresses_key_callback(
        const CanvasState& state,
        i32 key_code) const noexcept;
    [[nodiscard]] i32 key_state_mask(i32 key_code) const noexcept;

    vm::Machine& machine_;
    Dimensions dimensions_;
    std::array<i32, 7> keymap_;
    CanvasRenderHooks render_hooks_;
    std::function<std::chrono::steady_clock::time_point()> repeat_clock_ {
        [] { return std::chrono::steady_clock::now(); }};
    std::unordered_map<u64, CanvasState> canvases_;
    std::vector<u64> canvas_order_;
    std::optional<u64> active_canvas_;
    mutable std::mutex inputs_mutex_;
    std::deque<PendingInput> inputs_;
    std::atomic<usize> pending_input_count_ {0U};
    std::deque<VisibilityChange> visibility_changes_;
    bool host_foreground_ {false};
    bool host_rendering_enabled_ {true};
    bool pointer_events_supported_ {true};
    bool pointer_motion_supported_ {true};
    bool repeat_events_supported_ {true};
    u64 observed_translation_generation_ {0U};
    bool pumping_ {false};
};

} // namespace phoneme::runtime
