#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "phoneme/media/MediaService.hpp"
#include "phoneme/filesystem/FileSystem.hpp"
#include "phoneme/graphics/GraphicsStore.hpp"
#include "phoneme/network/ConnectionRegistry.hpp"
#include "phoneme/push/PushRegistry.hpp"
#include "phoneme/runtime/RecordStoreRegistry.hpp"
#include "phoneme/security/PermissionPolicy.hpp"
#include "phoneme/translation/TranslationService.hpp"
#include "phoneme/vm/BaselineJit.hpp"
#include "phoneme/vm/CanvasBridge.hpp"
#include "phoneme/vm/ClassLayout.hpp"
#include "phoneme/vm/Interpreter.hpp"
#include "phoneme/vm/MediaEventService.hpp"
#include "phoneme/vm/MonitorTable.hpp"
#include "phoneme/vm/NativeMethodRegistry.hpp"
#include "phoneme/vm/NativeRootScope.hpp"
#include "phoneme/vm/RootSet.hpp"
#include "phoneme/vm/Scheduler.hpp"
#include "phoneme/vm/SlotStorage.hpp"
#include "phoneme/vm/TimerService.hpp"

namespace phoneme::runtime
{
  class CanvasRuntime;
}

namespace phoneme::vm
{

  struct UiBridgeEvent final
  {
    i32 kind{0};
    i32 component_id{0};
    i32 parent_id{0};
    i32 component_type{0};
    i32 index{0};
    std::array<i32, 4> arguments{};
    i64 value64{0};
    u64 generation{0};
    std::string text;
    std::string detail;
  };

  using UiEventSink = std::function<void(UiBridgeEvent)>;

  enum class ImageDrawKind : u8
  {
    image,
    region,
  };

  struct ImageDrawEvent final
  {
    ImageDrawKind kind{ImageDrawKind::image};
    u64 graphics_id{0};
    u64 target_image_id{0};
    u64 source_image_id{0};
    i32 source_x{0};
    i32 source_y{0};
    i32 source_width{0};
    i32 source_height{0};
    i32 transform{0};
    i32 destination_x{0};
    i32 destination_y{0};
    i32 anchor{0};
    i32 translate_x{0};
    i32 translate_y{0};
    i32 clip_x{0};
    i32 clip_y{0};
    i32 clip_width{0};
    i32 clip_height{0};
    bool display_target{false};
  };

  using ImageDrawSink = std::function<void(const ImageDrawEvent&)>;

  enum class MidletSignal : u8
  {
    none,
    destroyed,
    paused,
    resume_requested,
  };

  enum class InstructionBudgetMode : u8
  {
    total,
    progress_watchdog,
  };

  class Machine final
  {
  public:
    struct CharacterTranslationDecision final
    {
      enum class Action : u8
      {
        draw_original,
        draw_translation,
        suppress,
      };

      Action action{Action::draw_original};
      std::shared_ptr<const std::vector<char32_t>> translated;
      i32 x{0};
      i32 y{0};
      i32 anchor{0};
    };

    struct TextTranslationLayoutDecision final
    {
      bool planned{false};
      bool suppress{false};
      i32 y{0};
      i32 height{0};
    };

    // Scheduler-owned Thread.run invocations are long-lived. They still yield
    // every VM quantum and observe cancellation, but must not inherit the
    // finite callback/lifecycle budget used to isolate buggy application code.
    static constexpr u64 kLongLivedThreadInstructionBudget =
        std::numeric_limits<u64>::max();

    explicit Machine(ClassRepository &classes,
                     usize maximum_heap_objects = 1'000'000);
    explicit Machine(ClassRepository &classes, HeapLimits heap_limits);
    ~Machine();
    void shutdown() noexcept;

  private:
    void dump_performance_summary_if_requested() noexcept;

  public:

    Machine(const Machine &) = delete;
    Machine &operator=(const Machine &) = delete;

    [[nodiscard]] Result<ExecutionResult> invoke_static(
        std::string_view class_name,
        std::string_view method_name,
        std::string_view descriptor,
        std::span<const Value> arguments = {},
        u64 instruction_budget = 10'000'000);

    [[nodiscard]] Result<ExecutionResult> invoke_instance(
        ObjectRef receiver,
        std::string_view declared_class,
        std::string_view method_name,
        std::string_view descriptor,
        std::span<const Value> arguments = {},
        u64 instruction_budget = 10'000'000,
        InstructionBudgetMode budget_mode = InstructionBudgetMode::total);

    [[nodiscard]] Heap &heap() noexcept { return heap_; }
    [[nodiscard]] const Heap &heap() const noexcept { return heap_; }
    // Native java.io fast path. Native methods execute while Machine owns the
    // VM execution gate, so exact DataInputStream/FilterInputStream chains that
    // terminate in ByteArrayInputStream can use Heap's lock-free VM accessors.
    // A null optional means "not this stream shape" and callers must use the
    // generic InputStream path; Java EOF/invalid-state errors are preserved.
    [[nodiscard]] Result<std::optional<u64>>
    try_read_byte_array_input_bits(ObjectRef input, usize byte_count);
    [[nodiscard]] ClassStateRegistry &class_states() noexcept { return states_; }
    [[nodiscard]] ClassRepository &classes() noexcept { return classes_; }
    [[nodiscard]] NativeMethodRegistry &natives() noexcept { return natives_; }
    [[nodiscard]] MonitorTable &monitors() noexcept { return monitors_; }
    [[nodiscard]] Scheduler& scheduler() noexcept { return scheduler_; }
    [[nodiscard]] const Scheduler& scheduler() const noexcept {
      return scheduler_;
    }
    [[nodiscard]] TimerService& timers() noexcept { return timers_; }
    [[nodiscard]] const TimerService& timers() const noexcept { return timers_; }
    [[nodiscard]] Result<ObjectRef> current_java_thread();
    [[nodiscard]] Status initialize_java_thread(ObjectRef thread,
                                                ObjectRef target);
    [[nodiscard]] Status start_java_thread(ObjectRef thread);
    [[nodiscard]] Result<ExecutionResult> run_java_thread_entry(
        ObjectRef thread);
    [[nodiscard]] Result<std::optional<Value>> run_java_thread_target(
        ObjectRef thread);
    void configure_frame_pacing(i32 frames_per_second,
                                FramePacingMode mode) noexcept;
    void configure_jit(bool enabled) noexcept;
    void configure_jit_startup(bool enabled) noexcept;
    [[nodiscard]] Status configure_jit_profile(std::string path);
    [[nodiscard]] Status flush_jit_profile();
    [[nodiscard]] bool jit_enabled() const noexcept;
    [[nodiscard]] bool jit_startup_mode() const noexcept;
    [[nodiscard]] JitAvailability jit_availability() const noexcept;
    [[nodiscard]] JitStatistics jit_statistics() const noexcept;
    void pace_frame_publication();
    void note_frame_pacing_boundary() noexcept;
    void note_frame_pacing_request() noexcept;
    void break_frame_pacing_sequence() noexcept;
    [[nodiscard]] Result<SchedulerWaitResult> sleep_current_thread(i64 millis);
    [[nodiscard]] Result<SchedulerWaitResult> join_java_thread(
        ObjectRef thread,
        std::optional<i64> millis);
    [[nodiscard]] Status interrupt_java_thread(ObjectRef thread);
    [[nodiscard]] Result<MonitorWaitResult> wait_on_object(
        ObjectRef object,
        std::optional<i64> millis);
    [[nodiscard]] Status notify_object(ObjectRef object, bool all);
    void cooperative_yield();
    [[nodiscard]] bool executing_on_current_thread() const noexcept;
    void request_garbage_collection() noexcept;
    [[nodiscard]] graphics::GraphicsStore& graphics() noexcept {
      return graphics_;
    }
    [[nodiscard]] const graphics::GraphicsStore& graphics() const noexcept {
      return graphics_;
    }
    void configure_translation_service(
        std::shared_ptr<translation::TranslationService> service) noexcept {
      std::scoped_lock lock(translation_service_mutex_);
      translation_service_ = std::move(service);
    }
    [[nodiscard]] std::shared_ptr<translation::TranslationService>
    translation_service() const noexcept {
      std::scoped_lock lock(translation_service_mutex_);
      return translation_service_;
    }
    void begin_character_translation_frame() noexcept;
    void end_character_translation_frame();
    void break_character_translation_run() noexcept;
    [[nodiscard]] CharacterTranslationDecision translate_draw_character(
        u64 graphics_id,
        char32_t character,
        i32 x,
        i32 y,
        i32 anchor,
        i32 font_face,
        i32 font_style,
        i32 font_size);
    [[nodiscard]] TextTranslationLayoutDecision plan_translated_text(
        u64 graphics_id,
        std::span<const char32_t> text,
        i32 x,
        i32 y,
        i32 anchor,
        i32 font_face,
        i32 font_style,
        i32 font_size,
        i32 clip_x,
        i32 clip_y,
        i32 clip_width,
        i32 clip_height,
        i32 translate_x,
        i32 translate_y);
    void configure_image_draw_interceptor(ImageDrawSink sink);
    void intercept_image_draw(const ImageDrawEvent& event) const;
    [[nodiscard]] double next_random_double() noexcept;
    void configure_ui_bridge(i32 app_namespace, UiEventSink sink);
    [[nodiscard]] i32 allocate_ui_component_id() noexcept;
    void emit_ui_event(UiBridgeEvent event);
    [[nodiscard]] Status enqueue_serial_callback(ObjectRef runnable);
    [[nodiscard]] Status pump_serial_callbacks(usize maximum_callbacks = 8U);
    [[nodiscard]] usize pending_serial_callbacks() const noexcept;
    void set_serial_callback_coalescing(bool enabled) noexcept;
    [[nodiscard]] Status schedule_lcdui_alert_timeout(ObjectRef alert,
                                                       i32 timeout_millis);
    void cancel_lcdui_alert_timeout(ObjectRef alert = {}) noexcept;
    [[nodiscard]] Result<std::optional<ObjectRef>> take_due_lcdui_alert();
    [[nodiscard]] Status register_ui_component(i32 component_id,
                                               ObjectRef object);
    void unregister_ui_component(i32 component_id) noexcept;
    [[nodiscard]] Result<ObjectRef> ui_component(i32 component_id) const;
    void configure_canvas_bridge(CanvasBridge* bridge) noexcept {
      canvas_bridge_ = bridge;
    }
    [[nodiscard]] CanvasBridge* canvas_bridge() noexcept {
      return canvas_bridge_;
    }
    [[nodiscard]] const CanvasBridge* canvas_bridge() const noexcept {
      return canvas_bridge_;
    }
    [[nodiscard]] Status collect_garbage();
    [[nodiscard]] RootSet& native_roots() noexcept { return native_roots_; }
    [[nodiscard]] const RootSet& native_roots() const noexcept {
      return native_roots_;
    }
    [[nodiscard]] Result<NativeRootScope> pin_native_root(
        ObjectRef reference = {}) {
      return NativeRootScope::pin(native_roots_, reference);
    }
    [[nodiscard]] Result<NativeRootScope> allocate_pinned_instance(
        std::string_view class_name);
    // Returns a cached, pinned byte[] holding the archive resource payload.
    // ByteArrayInputStream exposes no buffer mutation, so sharing the array
    // between calls is safe; each caller still gets a fresh stream object.
    [[nodiscard]] Result<ObjectRef> cached_resource_byte_array(
        std::string_view resource_name);
    void append_console(std::u16string_view text);
    [[nodiscard]] const std::u16string& console_output() const noexcept {
      return console_output_;
    }
    void signal_midlet(MidletSignal signal) noexcept;
    [[nodiscard]] MidletSignal consume_midlet_signal() noexcept;
    [[nodiscard]] bool consume_midlet_destroyed_signal() noexcept;
    [[nodiscard]] Result<ObjectRef> class_mirror(
        std::string_view class_name);
    [[nodiscard]] Result<std::string> mirrored_class_name(
        ObjectRef mirror) const;
    [[nodiscard]] Result<bool> object_is_instance(
        ObjectRef object,
        std::string_view target_class);
    [[nodiscard]] Status configure_record_store_root(std::string root);
    [[nodiscard]] runtime::RecordStoreRegistry& record_stores() noexcept {
      return record_stores_;
    }
    [[nodiscard]] Status configure_push_registry(std::string root,
                                                 SuiteId suite_id);
    [[nodiscard]] push::PushRegistry& push_registry() noexcept {
      return push_registry_;
    }
    void set_permission_policy(security::SharedPermissionPolicy policy);
    [[nodiscard]] security::PermissionPolicy& permission_policy() noexcept {
      return *permission_policy_;
    }
    [[nodiscard]] const security::PermissionPolicy& permission_policy() const noexcept {
      return *permission_policy_;
    }
    [[nodiscard]] Status configure_filesystem(std::string sandbox_root,
                                              std::string temporary_root);
    [[nodiscard]] filesystem::FileSystem& filesystem() noexcept {
      return filesystem_;
    }
    [[nodiscard]] const filesystem::FileSystem& filesystem() const noexcept {
      return filesystem_;
    }
    [[nodiscard]] media::MediaService& media() noexcept { return media_; }
    [[nodiscard]] const media::MediaService& media() const noexcept {
      return media_;
    }
    [[nodiscard]] MediaEventService& media_events() noexcept {
      return media_events_;
    }
    [[nodiscard]] const MediaEventService& media_events() const noexcept {
      return media_events_;
    }
    [[nodiscard]] network::ConnectionRegistry& connections() noexcept {
      return connections_;
    }
    [[nodiscard]] const network::ConnectionRegistry& connections() const noexcept {
      return connections_;
    }
    [[nodiscard]] Status configure_network_owner(i32 owner) noexcept;
    [[nodiscard]] Status configure_network_adapter(
        std::shared_ptr<network::AsyncNetworkAdapter> adapter);
    void close_connections() noexcept;
    void set_app_property(std::u16string key, std::u16string value);
    [[nodiscard]] Result<std::optional<std::u16string>> app_property(
        ObjectRef key) const;
    void set_system_property(std::u16string key, std::u16string value);
    [[nodiscard]] std::optional<std::u16string> configured_system_property(
        std::u16string_view key) const;

  private:
    friend class Scheduler;
    friend class runtime::CanvasRuntime;

    struct Invocation final
    {
      ResolvedMethod method;
      std::shared_ptr<const CachedMethodDescriptor> descriptor;
      NativeMethodId native_method;
      InvocationArguments arguments;
      bool has_receiver{false};
      std::optional<Value> return_override;
      bool return_override_boxes_result{false};
      bool discard_return_value{false};
      std::optional<std::string> return_reference_cast_target;
      std::optional<JavaTypeKind> return_unboxing_target;
      std::optional<JavaTypeKind> return_widening_source;
      std::optional<JavaTypeKind> return_widening_target;
      std::optional<JitDeoptState> resume_jit_deopt_state;
      u64 resume_jit_instructions{0U};
      u64 resume_jit_nested_instructions{0U};
    };

    struct TrivialGetterIntrinsic final
    {
      FieldLocation field;
      bool is_static{false};
    };

    struct StaticByteCursorReadIntrinsic final
    {
      FieldLocation bytes;
      FieldLocation cursor;
    };

    struct TiledAlphaCollisionIntrinsic final
    {
      FieldLocation tiles;
      FieldLocation tile_x;
      FieldLocation tile_y;
      FieldLocation tile_width;
      FieldLocation tile_height;
      FieldLocation tile_silk_collision;
      FieldLocation tile_pixels;
      std::string tile_class;
    };

    struct ProjectileCollisionIntrinsic final
    {
      FieldLocation players;
      FieldLocation team;
      FieldLocation hp;
      FieldLocation invisible;
      FieldLocation state;
      FieldLocation x;
      FieldLocation y;
      std::string player_class;
      std::string boss_class;
    };

    struct TransparentStringHash final
    {
      using is_transparent = void;
      [[nodiscard]] usize operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
      }
      [[nodiscard]] usize operator()(const std::string& value) const noexcept {
        return (*this)(std::string_view(value));
      }
    };

    struct TransparentStringEqual final
    {
      using is_transparent = void;
      [[nodiscard]] bool operator()(std::string_view left,
                                    std::string_view right) const noexcept {
        return left == right;
      }
    };

    struct CharacterDrawSample final
    {
      u64 graphics_id{0};
      char32_t character{0};
      i32 x{0};
      i32 y{0};
      i32 anchor{0};
      i32 font_face{0};
      i32 font_style{0};
      i32 font_size{0};
      u64 run_generation{0};
    };

    struct CharacterRunPlan final
    {
      usize first_sample{0};
      usize sample_count{0};
      i32 x{0};
      i32 y{0};
      i32 anchor{0};
      std::vector<char32_t> source;
    };

    struct PlannedCharacterSample final
    {
      CharacterDrawSample sample;
      usize run_index{std::numeric_limits<usize>::max()};
      bool first_in_run{false};
    };

    struct TranslatedTextDrawSample final
    {
      u64 graphics_id{0};
      std::vector<char32_t> text;
      i32 x{0};
      i32 y{0};
      i32 anchor{0};
      i32 font_face{0};
      i32 font_style{0};
      i32 font_size{0};
      i32 clip_x{0};
      i32 clip_y{0};
      i32 clip_width{0};
      i32 clip_height{0};
      i32 translate_x{0};
      i32 translate_y{0};
    };

    struct PlannedTranslatedTextSample final
    {
      TranslatedTextDrawSample sample;
      i32 y{0};
      i32 height{0};
      bool suppress{false};
    };

    struct DirectCallCache final
    {
      MethodId target_method;
      NativeMethodId native_method;
      u64 native_generation{0U};
      bool native_binding_cached{false};
      bool valid{false};
    };

    struct VirtualCallCacheEntry final
    {
      ClassId receiver_class;
      MethodId target_method;
      NativeMethodId native_method;
      u64 native_generation{0U};
      bool native_binding_cached{false};
      bool valid{false};
    };

    struct VirtualCallCache final
    {
      static constexpr usize kCapacity = 4U;
      std::array<VirtualCallCacheEntry, kCapacity> entries{};
      u8 next_replace{0U};

      [[nodiscard]] std::optional<MethodId> lookup(
          ClassId receiver_class) const noexcept
      {
        for (const VirtualCallCacheEntry& entry : entries)
        {
          if (entry.valid && entry.receiver_class == receiver_class)
            return entry.target_method;
        }
        return std::nullopt;
      }

      [[nodiscard]] VirtualCallCacheEntry* lookup_entry(
          ClassId receiver_class) noexcept
      {
        for (VirtualCallCacheEntry& entry : entries)
        {
          if (entry.valid && entry.receiver_class == receiver_class)
            return &entry;
        }
        return nullptr;
      }

      void invalidate(ClassId receiver_class) noexcept
      {
        for (VirtualCallCacheEntry& entry : entries)
        {
          if (entry.valid && entry.receiver_class == receiver_class)
          {
            entry = {};
            return;
          }
        }
      }

      void update(ClassId receiver_class,
                  MethodId target_method,
                  NativeMethodId native_method = {},
                  u64 native_generation = 0U,
                  bool native_binding_cached = false) noexcept
      {
        for (VirtualCallCacheEntry& entry : entries)
        {
          if (entry.valid && entry.receiver_class == receiver_class)
          {
            entry.target_method = target_method;
            entry.native_method = native_method;
            entry.native_generation = native_generation;
            entry.native_binding_cached = native_binding_cached;
            return;
          }
        }
        for (VirtualCallCacheEntry& entry : entries)
        {
          if (!entry.valid)
          {
            entry = VirtualCallCacheEntry {
                .receiver_class = receiver_class,
                .target_method = target_method,
                .native_method = native_method,
                .native_generation = native_generation,
                .native_binding_cached = native_binding_cached,
                .valid = true,
            };
            return;
          }
        }
        const usize slot = static_cast<usize>(next_replace) % kCapacity;
        entries[slot] = VirtualCallCacheEntry {
            .receiver_class = receiver_class,
            .target_method = target_method,
            .native_method = native_method,
            .native_generation = native_generation,
            .native_binding_cached = native_binding_cached,
            .valid = true,
        };
        next_replace = static_cast<u8>((slot + 1U) % kCapacity);
      }
    };

    struct InvokeSiteBinding final
    {
      classfile::MemberReference reference;
      std::shared_ptr<const CachedMethodDescriptor> descriptor;
    };

    struct QuickFieldBinding final
    {
      FieldId id;
      ClassId declaring_class;
      std::shared_ptr<const RuntimeClass> declaring_runtime_class;
      usize index{0U};
      ValueKind value_kind{ValueKind::int32};
      bool is_static{false};
      std::optional<u16> string_constant_value_index;
    };

    struct JitExecutionContext final
    {
      using AppendOuterRoots = void (*)(void*, std::vector<ObjectRef>&) noexcept;

      Machine* machine{nullptr};
      const classfile::ClassFile* owner{nullptr};
      const classfile::Method* method{nullptr};
      u32 invocation_depth{0U};
      std::span<const ObjectRef> base_roots;
      JitExecutionContext* parent_jit_context{nullptr};
      void* outer_roots_context{nullptr};
      AppendOuterRoots append_outer_roots{nullptr};
      std::span<const Value> extra_root_values;
      const InvocationArguments* compact_extra_root_values{nullptr};
      std::vector<ObjectRef> published_roots;
      std::span<const ObjectRef> published_root_view;
      std::span<const u64> frame_root_bits;
      const u64* staged_frame_base{nullptr};
      std::span<const u32> staged_root_offsets;
      bool roots_staged{false};
      std::optional<ObjectRef> pending_throwable;
      u64 nested_instructions{0U};
      bool progress_watchdog{false};
      u64 progress_total_budget{0U};
      u64 progress_reset_instructions{0U};
    };

    struct LambdaBinding final
    {
      std::string interface_name;
      std::string sam_name;
      std::string sam_descriptor;
      std::string instantiated_descriptor;
      u8 implementation_kind{0};
      classfile::MemberReference implementation;
      std::vector<std::string> marker_interfaces;
      std::vector<std::string> bridge_descriptors;
      usize captured_count{0};
    };

    [[nodiscard]] Status initialize_system_streams();
    [[nodiscard]] Result<ExecutionResult> execute(
        Invocation invocation,
        u64 instruction_budget,
        InstructionBudgetMode budget_mode = InstructionBudgetMode::total);
    [[nodiscard]] NativeMethodBinding resolve_native_binding(
        const classfile::ClassFile& owner,
        const classfile::Method& method,
        MethodId runtime_method_id = {});
    [[nodiscard]] Result<Invocation> prepare_invocation(
        ResolvedMethod method,
        std::span<const Value> arguments,
        bool has_receiver,
        std::optional<NativeMethodId> prebound_native_method = std::nullopt,
        bool arguments_verified = false);
    [[nodiscard]] Result<Invocation> prepare_invocation(
        ResolvedMethod method,
        InvocationArguments arguments,
        bool has_receiver,
        std::optional<NativeMethodId> prebound_native_method = std::nullopt,
        bool arguments_verified = false);
    void refresh_metadata_bindings_if_needed() noexcept;
    [[nodiscard]] std::shared_ptr<const RuntimeMethod> cached_runtime_method(
        MethodId method_id);
    [[nodiscard]] Result<std::shared_ptr<const CachedMethodDescriptor>>
    cached_method_descriptor(std::string_view descriptor);
    [[nodiscard]] Result<std::optional<QuickFieldBinding>*>
    field_binding_slot(const classfile::ClassFile& owner,
                       u16 constant_pool_index);
    [[nodiscard]] Result<QuickFieldBinding> resolve_quick_field_binding(
        const classfile::MemberReference& reference,
        bool require_static);
    [[nodiscard]] Result<InvokeSiteBinding*> invoke_site_binding(
        const classfile::ClassFile& owner,
        u16 constant_pool_index);
    [[nodiscard]] Result<DirectCallCache*> direct_call_binding_slot(
        const classfile::ClassFile& owner,
        u16 constant_pool_index);
    [[nodiscard]] Result<VirtualCallCache*> virtual_call_binding_slot(
        const classfile::ClassFile& owner,
        u16 constant_pool_index);
    [[nodiscard]] std::shared_ptr<const RuntimeClass> cached_runtime_class(
        std::string_view class_name);
    [[nodiscard]] Result<OperandResolutionEntry*> operand_resolution_entry(
        MethodId method_id,
        u32 operand_index,
        usize bytecode_pc);
    [[nodiscard]] Result<OperandResolutionEntry*> resolve_class_operand(
        MethodId method_id,
        u32 operand_index,
        usize bytecode_pc,
        const classfile::ClassFile& owner,
        u16 constant_pool_index,
        bool load_target_class,
        bool derive_reference_array_name);
    [[nodiscard]] Result<std::shared_ptr<const classfile::ClassFile>>
    load_linkage_class(std::string_view class_name);
    [[nodiscard]] Result<std::optional<ObjectRef>> ensure_initialized(
        std::string_view class_name,
        u64 instruction_budget);
    [[nodiscard]] Result<std::optional<ObjectRef>>
    ensure_default_interfaces_initialized(
        const classfile::ClassFile &owner,
        u64 instruction_budget);
    [[nodiscard]] Result<std::optional<Value>> invoke_native(
        const Invocation &invocation);
    [[nodiscard]] static u32 jit_runtime_dispatch_callback(
        void* context,
        JitRuntimeOperation operation,
        u64 operand,
        u64 first,
        u64 second,
        u64 third,
        const u64* frame_base,
        u64* result_bits) noexcept;
    static void jit_publish_roots_callback(void* context,
                                           const u64* roots,
                                           usize root_count,
                                           const u64* frame_base,
                                           const u32* root_offsets,
                                           usize root_offset_count,
                                           bool defer_scheduler_publication) noexcept;
    static void append_jit_context_roots(
        const JitExecutionContext* execution,
        std::vector<ObjectRef>& roots) noexcept;
    void commit_staged_jit_roots(JitExecutionContext* execution) noexcept;
    [[nodiscard]] u32 dispatch_jit_runtime(
        JitExecutionContext* parent_context,
        const classfile::ClassFile& owner,
        JitRuntimeOperation operation,
        u32 operand,
        u64 first,
        u64 second,
        u64 third,
        const u64* frame_base,
        u32* consumed_instructions,
        u64* result_bits) noexcept;
    [[nodiscard]] std::optional<u64> bounded_jit_invocation_cost(
        const ResolvedMethod& target,
        u32 recursion_depth);
    [[nodiscard]] std::optional<u64> safe_jit_instruction_budget(
        const ResolvedMethod& target,
        u64 requested_budget);
    [[nodiscard]] Result<ObjectRef> intern_string(
        std::string_view modified_utf8);
    [[nodiscard]] Result<ObjectRef> create_throwable(
        std::string_view class_name);
    [[nodiscard]] Result<ObjectRef> create_throwable(
        std::string_view class_name,
        std::string_view message);
    [[nodiscard]] Result<Value> load_constant(
        const classfile::ClassFile &owner,
        u16 index,
        bool category_two_only);
    [[nodiscard]] Result<std::optional<ObjectRef>> acquire_synchronized_monitor(
        const Invocation &invocation);
    [[nodiscard]] Result<std::optional<ObjectRef>> synchronized_monitor(
        const Invocation &invocation);
    [[nodiscard]] Status release_synchronized_monitor(
        std::optional<ObjectRef> monitor);
    [[nodiscard]] Status enter_monitor(ObjectRef monitor);
    [[nodiscard]] Status enter_external_execution();
    [[nodiscard]] bool try_enter_external_execution() noexcept;
    void leave_external_execution() noexcept;
    [[nodiscard]] u32 suspend_execution_for_blocking() noexcept;
    void resume_execution_after_blocking(u32 depth) noexcept;
    void publish_execution_roots(u32 invocation_depth,
                                 const std::vector<ObjectRef>& roots);
    std::span<const ObjectRef> exchange_execution_roots(
        u32 invocation_depth,
        std::vector<ObjectRef>& roots);
    void clear_execution_roots(u32 invocation_depth) noexcept;
    [[nodiscard]] Result<LambdaBinding> resolve_lambda_binding(
        const classfile::ClassFile &owner,
        u16 invoke_dynamic_index);
    [[nodiscard]] Result<Invocation> prepare_lambda_invocation(
        ObjectRef receiver,
        const LambdaBinding &binding,
        std::span<const Value> invocation_arguments,
        std::optional<ObjectRef> constructor_receiver = std::nullopt);
    [[nodiscard]] Result<Value> cast_lambda_reference(
        Value value,
        std::string_view target_class);
    [[nodiscard]] Result<Value> unbox_lambda_value(
        Value value,
        JavaTypeKind target_kind);
    [[nodiscard]] Result<std::optional<Value>>
    try_tiled_alpha_collision_intrinsic(const ResolvedMethod& method,
                                        i32 x,
                                        i32 y);
    [[nodiscard]] Result<std::optional<Value>>
    try_projectile_collision_intrinsic(const ResolvedMethod& method,
                                       i32 x,
                                       i32 y,
                                       ObjectRef shooter);
    void prune_lambda_bindings();

    ClassRepository &classes_;
    ClassStateRegistry states_;
    Heap heap_;
    NativeMethodRegistry natives_;
    MonitorTable monitors_;
    mutable std::recursive_mutex execution_mutex_;
    graphics::GraphicsStore graphics_;
    mutable std::mutex translation_service_mutex_;
    std::shared_ptr<translation::TranslationService> translation_service_;
    mutable std::mutex image_draw_sink_mutex_;
    ImageDrawSink image_draw_sink_;
    std::atomic_bool image_draw_interception_enabled_{false};
    std::vector<CharacterDrawSample> character_translation_capture_;
    std::vector<PlannedCharacterSample> character_translation_plan_samples_;
    std::vector<CharacterRunPlan> character_translation_runs_;
    usize character_translation_replay_index_{0};
    u64 character_translation_run_generation_{0};
    bool character_translation_frame_active_{false};
    bool character_translation_replay_valid_{true};
    std::vector<TranslatedTextDrawSample> translated_text_capture_;
    std::vector<PlannedTranslatedTextSample> translated_text_plan_;
    usize translated_text_replay_index_{0};
    bool translated_text_replay_valid_{true};
    runtime::RecordStoreRegistry record_stores_;
    security::SharedPermissionPolicy permission_policy_;
    push::PushRegistry push_registry_;
    filesystem::FileSystem filesystem_;
    media::MediaService media_;
    network::ConnectionRegistry connections_;
    std::unordered_map<std::u16string, ObjectRef> interned_strings_;

    // ponytail: byte[] still stores 16-byte Values, so the heap cost is 16x
    // the payload budget; track payload bytes when arrays get raw storage.
    static constexpr u64 kResourceArrayCachePayloadLimit = 8ULL * 1024ULL * 1024ULL;
    std::unordered_map<std::string, ObjectRef> resource_array_cache_;
    std::deque<std::string> resource_array_cache_order_;
    u64 resource_array_cache_payload_bytes_ {0};
    std::unordered_map<std::string, ObjectRef> class_mirrors_;
    std::unordered_map<i32, ObjectRef> ui_components_;
    std::unordered_map<u64, LambdaBinding> lambda_bindings_;
    u64 metadata_binding_generation_ {0U};
    mutable std::mutex native_bindings_mutex_;
    u64 native_binding_generation_ {0U};
    std::vector<std::optional<NativeMethodId>> native_bindings_by_method_;
    std::unordered_map<const classfile::Method*, NativeMethodId> native_bindings_;
    std::unordered_map<const classfile::Method*,
                       std::optional<TrivialGetterIntrinsic>>
        trivial_getter_intrinsics_;
    std::unordered_map<const classfile::Method*,
                       std::optional<StaticByteCursorReadIntrinsic>>
        static_byte_cursor_read_intrinsics_;
    std::unordered_map<const classfile::Method*,
                       std::optional<TiledAlphaCollisionIntrinsic>>
        tiled_alpha_collision_intrinsics_;
    std::unordered_map<const classfile::Method*,
                       std::optional<ProjectileCollisionIntrinsic>>
        projectile_collision_intrinsics_;
    std::unordered_map<
        const classfile::ClassFile*,
        std::vector<std::optional<QuickFieldBinding>>>
        field_bindings_;
    std::unordered_map<
        const classfile::ClassFile*,
        std::vector<std::optional<InvokeSiteBinding>>>
        invoke_site_bindings_;
    std::unordered_map<
        const classfile::ClassFile*,
        std::vector<DirectCallCache>>
        direct_call_bindings_;
    std::unordered_map<
        const classfile::ClassFile*,
        std::vector<std::unique_ptr<VirtualCallCache>>>
        virtual_call_bindings_;
    RootSet native_roots_;
    mutable std::mutex serial_callbacks_mutex_;
    std::deque<NativeRootScope> serial_callbacks_;
    bool serial_callback_coalescing_ {false};
    mutable std::mutex lcd_ui_alert_timeout_mutex_;
    std::optional<NativeRootScope> lcd_ui_alert_timeout_root_;
    std::chrono::steady_clock::time_point lcd_ui_alert_timeout_deadline_ {};
    ObjectRef emergency_out_of_memory_error_ {};
    mutable std::mutex class_initialization_mutex_;
    std::condition_variable class_initialization_condition_;
    std::unordered_set<std::string> initialized_classes_;
    std::unordered_map<std::string, JavaThreadId> initializing_class_owners_;
    std::unordered_set<std::string> erroneous_classes_;
    std::unordered_map<std::u16string, std::u16string> app_properties_;
    std::unordered_map<std::u16string, std::u16string> system_properties_;
    u64 random_state_{0x9E3779B97F4A7C15ULL};
    std::atomic<MidletSignal> midlet_signal_{MidletSignal::none};
    UiEventSink ui_event_sink_;
    CanvasBridge* canvas_bridge_{nullptr};
    i32 next_ui_component_id_{1};
    u64 ui_generation_{0};
    bool system_streams_initialized_{false};
    std::u16string console_output_;
    std::atomic_bool gc_requested_{false};
    std::atomic_bool shutdown_started_{false};
    TimerService timers_;
    Scheduler scheduler_;
    MediaEventService media_events_;
    // Appended to preserve the established layout of every pre-existing
    // Machine member for mixed-object compatibility validation builds.
    bool serial_callback_worker_running_ {false};
    std::optional<Error> serial_callback_failure_;
    MethodId operand_resolution_method_id_ {};
    std::shared_ptr<const RuntimeMethod> operand_resolution_method_;
    std::vector<std::shared_ptr<const RuntimeMethod>> runtime_method_bindings_;
    std::unordered_map<std::string,
                       std::shared_ptr<const CachedMethodDescriptor>,
                       TransparentStringHash,
                       TransparentStringEqual>
        descriptor_bindings_;
    std::unordered_map<std::string,
                       std::shared_ptr<const RuntimeClass>,
                       TransparentStringHash,
                       TransparentStringEqual>
        runtime_class_bindings_;
    BaselineJit jit_;
  };

} // namespace phoneme::vm
