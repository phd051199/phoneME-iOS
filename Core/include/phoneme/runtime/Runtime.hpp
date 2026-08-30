#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "phoneme/push/PushRegistry.hpp"
#include "phoneme/runtime/ConcurrentQueue.hpp"
#include "phoneme/runtime/Framebuffer.hpp"
#include "phoneme/runtime/SuiteStore.hpp"
#include "phoneme/security/PermissionPolicy.hpp"
#include "phoneme/translation/TranslationService.hpp"
#include "phoneme/vm/BaselineJit.hpp"
#include "phoneme/vm/NativeMethodRegistry.hpp"
#include "phoneme/vm/Scheduler.hpp"

namespace phoneme::runtime {

class ApplicationVM;

enum class AppState : i32 {
    none = 0,
    active = 1,
    paused = 2,
    destroyed = 3,
    error = 4,
};

enum class InputKind : u8 {
    key,
    pointer,
};

struct InputEvent final {
    InputKind kind {InputKind::key};
    AppId app_id;
    u64 app_generation {0};
    i32 first {0};
    i32 second {0};
    i32 third {0};
    u64 sequence {0};
};

struct UiEvent final {
    i32 kind {0};
    i32 component_id {0};
    i32 parent_id {0};
    i32 component_type {0};
    i32 index {0};
    std::array<i32, 4> arguments {};
    i64 value64 {0};
    u64 generation {0};
    std::string text;
    std::string detail;
};

struct App final {
    AppId id;
    SuiteId suite_id;
    std::string main_class;
    Dimensions dimensions;
    AppState state {AppState::none};
    u64 generation {0};
    u64 lifecycle_token {0};
    bool lifecycle_busy {false};
    std::shared_ptr<ApplicationVM> vm;
    std::string console_output;
    std::string error_message;
    std::vector<vm::NativeMethodInvocationCount> native_invocation_counts;
};

struct UiTranslationReplayState;

struct AppFramePacingConfig final {
    i32 frames_per_second {30};
    vm::FramePacingMode mode {vm::FramePacingMode::native};
};

struct AppHeapConfig final {
    usize maximum_bytes {256U * 1024U * 1024U};
};

struct FrameReadView final {
    const u8* pixels {nullptr};
    FrameMetadata metadata;
    std::span<const FrameDamageRegion> damage_regions;
};

class Runtime final {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] Status configure(std::string runtime_home,
                                   std::string optional_class_archive = {});
    [[nodiscard]] Status configure_keymap(std::array<i32, 7> keymap);
    [[nodiscard]] Status configure_app_frame_pacing(
        AppId app_id,
        i32 frames_per_second,
        vm::FramePacingMode mode);
    [[nodiscard]] Status configure_app_heap(AppId app_id,
                                            i32 heap_megabytes);
    [[nodiscard]] Status configure_jit(bool enabled);
    [[nodiscard]] vm::JitAvailability jit_availability() const noexcept;
    [[nodiscard]] Status configure_input_capabilities(
        bool pointer_events,
        bool pointer_motion,
        bool repeat_events);
    [[nodiscard]] Status configure_translation(
        bool enabled,
        translation::TranslationProvider provider =
            translation::TranslationProvider::automatic,
        std::string source_language = "auto",
        std::string target_language = "vi");
    [[nodiscard]] Status configure_app_translation(
        AppId app_id,
        bool enabled,
        translation::TranslationProvider provider =
            translation::TranslationProvider::automatic,
        std::string source_language = "auto",
        std::string target_language = "vi");
    [[nodiscard]] Status configure_permission_prompt(
        security::PermissionPromptCallback prompt);
    [[nodiscard]] Status set_suite_trust(
        SuiteId suite_id,
        security::SuiteTrust trust);
    [[nodiscard]] Result<SuiteId> install_jar(const std::string& jar_path);
    [[nodiscard]] Result<SuiteId> install_jar(
        const std::string& jar_path,
        std::string_view identity_scope);
    [[nodiscard]] std::optional<SuiteId> find_installed_suite(
        std::string_view vendor,
        std::string_view name,
        std::string_view version,
        std::string_view identity_scope = {}) const noexcept;
    [[nodiscard]] Status uninstall_suite(SuiteId suite_id,
                                         bool remove_data);
    [[nodiscard]] Status start_system();
    [[nodiscard]] Status start_midlet(SuiteId suite_id,
                                      std::string main_class,
                                      AppId app_id,
                                      Dimensions dimensions);
    [[nodiscard]] Status set_foreground(AppId app_id, Dimensions dimensions);
    [[nodiscard]] Status pause_midlet(AppId app_id);
    [[nodiscard]] Status resume_midlet(AppId app_id);
    [[nodiscard]] Status destroy_midlet(AppId app_id);

    [[nodiscard]] Status set_push_background_policy(
        SuiteId suite_id,
        push::BackgroundPolicy policy);
    [[nodiscard]] Status notify_push_connection_available(
        SuiteId suite_id,
        std::string connection,
        i64 received_at_millis);
    [[nodiscard]] Status notify_push_connection_available(
        SuiteId suite_id,
        std::string connection,
        std::string source_address,
        i64 received_at_millis);
    [[nodiscard]] Result<std::vector<push::LaunchRequest>>
    poll_push_launch_requests(SuiteId suite_id,
                              i64 now_millis,
                              bool background_execution_granted,
                              usize limit = 32U);
    [[nodiscard]] Status acknowledge_push_launch_request(
        SuiteId suite_id,
        u64 request_id);

    [[nodiscard]] AppState app_state(AppId app_id) noexcept;
    [[nodiscard]] AppId foreground_app_id() const noexcept;
    [[nodiscard]] i64 app_used_memory(AppId app_id) const noexcept;
    [[nodiscard]] std::string app_console_output(AppId app_id) const;
    [[nodiscard]] std::string app_error_message(AppId app_id) const;
    [[nodiscard]] std::vector<vm::NativeMethodInvocationCount>
    app_native_invocation_counts(AppId app_id) const;

    void record_error(const Error& error);
    void clear_error();
    [[nodiscard]] std::string last_error_message() const;

    void stop() noexcept;
    void suspend() noexcept;
    void resume() noexcept;
    // Persists deferred RMS writes for every live application VM. Call before
    // the host may kill the process (backgrounding, memory pressure).
    void flush_record_stores() noexcept;

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_suspended() const noexcept;
    [[nodiscard]] i32 last_exit_code() const noexcept;

    void send_key(i32 key_code, bool pressed);
    void send_pointer(i32 x, i32 y, i32 action);
    void pump_events();
    [[nodiscard]] bool try_pump_events();

    [[nodiscard]] FrameMetadata frame_metadata();
    [[nodiscard]] FrameMetadata copy_current_frame_rgba(
        std::span<u8> destination) const noexcept;
    [[nodiscard]] std::optional<FrameMetadata> copy_current_frame_rgba_since(
        u64 previous_generation,
        std::span<u8> destination) const noexcept;
    [[nodiscard]] std::optional<FrameReadView> acquire_current_frame_rgba_since(
        u64 previous_generation) noexcept;
    [[nodiscard]] std::optional<FrameReadView> acquire_current_frame_native_since(
        u64 previous_generation) noexcept;
    void release_current_frame_rgba() noexcept;
    [[nodiscard]] u64 storage_generation() noexcept;
    [[nodiscard]] FrameMetadata copy_lcdui_image_rgba(
        i32 component_id,
        std::span<u8> destination);
    [[nodiscard]] FrameSnapshot frame_snapshot();
    [[nodiscard]] FrameSnapshot frame_snapshot_unpumped() const;
    [[nodiscard]] std::optional<UiEvent> poll_ui_event();

    void ui_select_command(i32 command_id);
    void ui_select_list_item_command(i32 component_id,
                                     i32 element_index,
                                     i32 command_id);
    void ui_focus_item(i32 component_id);
    void ui_activate_item(i32 component_id);
    void ui_set_text(i32 component_id, std::string text, i32 caret_position);
    void ui_set_choice(i32 component_id, i32 element_index, bool selected);
    void ui_set_gauge(i32 component_id, i32 value);
    void ui_set_date(i32 component_id, i64 unix_seconds);
    void ui_set_scroll_position(i32 position);

private:
    [[nodiscard]] App* find_app_unlocked(AppId app_id) noexcept;
    [[nodiscard]] const App* find_app_unlocked(AppId app_id) const noexcept;
    [[nodiscard]] Status require_running_unlocked() const;
    [[nodiscard]] i32 mapped_key_code_unlocked(i32 host_key_code) const noexcept;
    void dispatch_input();
    void finalize_deferred_start(
        AppId app_id,
        const std::shared_ptr<ApplicationVM>& vm);
    void finalize_pending_destruction(
        AppId app_id,
        const std::shared_ptr<ApplicationVM>& vm) noexcept;
    void mark_canvas_failure_unlocked(App& app, const Error& error);
    void push_ui_action(i32 kind, i32 component_id, i32 first, i64 value64,
                        std::string text = {});

    // Protects Runtime state only. Java/native callbacks are serialized by the
    // owning ApplicationVM and must never execute while this mutex is held.
    mutable std::mutex mutex_;
    std::string runtime_home_;
    std::string optional_class_archive_;
    std::array<i32, 7> keymap_ {-1, -2, -3, -4, -5, -6, -7};
    AppFramePacingConfig default_frame_pacing_ {
        .frames_per_second = 30,
        .mode = vm::FramePacingMode::native,
    };
    std::unordered_map<i32, AppFramePacingConfig> app_frame_pacing_;
    std::unordered_map<i32, AppHeapConfig> app_heap_configs_;
    bool jit_enabled_by_default_ {true};
    std::shared_ptr<translation::TranslationService> translation_service_;
    bool translation_enabled_by_default_ {false};
    bool pointer_events_supported_ {true};
    bool pointer_motion_supported_ {true};
    bool repeat_events_supported_ {true};
    SuiteStore suite_store_;
    security::PermissionPromptCallback permission_prompt_;
    std::unordered_map<i32, security::SuiteTrust> suite_trust_;
    std::unordered_map<i32, security::SharedPermissionPolicy>
        permission_policies_;
    std::unordered_map<i32, App> apps_;
    bool configured_ {false};
    bool running_ {false};
    bool suspended_ {false};
    i32 last_exit_code_ {0};
    std::string last_error_message_;
    AppId foreground_app_id_ {};
    u64 sequence_ {0};
    Framebuffer framebuffer_;
    std::optional<Framebuffer::ReadLease> frame_read_lease_;
    ConcurrentQueue<InputEvent> input_queue_;
    ConcurrentQueue<UiEvent> ui_queue_;
    std::shared_ptr<UiTranslationReplayState> ui_translation_replay_;
};

} // namespace phoneme::runtime
