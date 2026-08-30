#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "phoneme/base/Error.hpp"
#include "phoneme/classfile/ClassFile.hpp"
#include "phoneme/vm/MetadataId.hpp"
#include "phoneme/vm/RuntimeMetadata.hpp"
#include "phoneme/vm/SlotStorage.hpp"
#include "phoneme/vm/Value.hpp"

namespace phoneme::vm {

enum class JitAvailability : i32 {
    unavailable = 0,
    ready = 1,
};

enum class JitRejectReason : u8 {
    missing_code,
    unsupported_signature,
    exception_handlers,
    frame_too_large,
    decode_failure,
    unsupported_opcode,
    invalid_stack_shape,
    executable_memory,
    code_cache_limit,
    argument_kind,
    instruction_budget,
    unsafe_side_effect,
    count,
};

inline constexpr usize kJitRejectReasonCount =
    static_cast<usize>(JitRejectReason::count);

[[nodiscard]] constexpr std::string_view jit_reject_reason_name(
    JitRejectReason reason) noexcept {
    switch (reason) {
    case JitRejectReason::missing_code: return "missing-code";
    case JitRejectReason::unsupported_signature: return "unsupported-signature";
    case JitRejectReason::exception_handlers: return "exception-handlers";
    case JitRejectReason::frame_too_large: return "frame-too-large";
    case JitRejectReason::decode_failure: return "decode-failure";
    case JitRejectReason::unsupported_opcode: return "unsupported-opcode";
    case JitRejectReason::invalid_stack_shape: return "invalid-stack-shape";
    case JitRejectReason::executable_memory: return "executable-memory";
    case JitRejectReason::code_cache_limit: return "code-cache-limit";
    case JitRejectReason::argument_kind: return "argument-kind";
    case JitRejectReason::instruction_budget: return "instruction-budget";
    case JitRejectReason::unsafe_side_effect: return "unsafe-side-effect";
    case JitRejectReason::count: break;
    }
    return "unknown";
}

enum class JitRuntimeOperation : u32 {
    get_field = 1,
    array_load = 2,
    array_length = 3,
    float_remainder = 4,
    double_remainder = 5,
    float_compare_less = 6,
    float_compare_greater = 7,
    double_compare_less = 8,
    double_compare_greater = 9,
    int_to_float = 10,
    int_to_double = 11,
    long_to_float = 12,
    long_to_double = 13,
    float_to_int = 14,
    float_to_long = 15,
    float_to_double = 16,
    double_to_int = 17,
    double_to_long = 18,
    double_to_float = 19,
    get_static = 20,
    check_cast = 21,
    instance_of = 22,
    put_field = 23,
    put_static = 24,
    array_store = 25,
    invoke_virtual = 26,
    invoke_special = 27,
    invoke_static = 28,
    invoke_interface = 29,
    new_primitive_array = 30,
    new_reference_array = 31,
    new_object = 32,
    new_multi_array = 33,
    load_constant = 34,
    throw_object = 35,
    match_exception_handler = 36,
    monitor_enter = 37,
    monitor_exit = 38,
    arithmetic_exception = 39,
    invoke_dynamic = 40,
    array_payload_lease = 41,
    budget_safepoint = 42,
};

enum class JitRuntimeStatus : u32 {
    success = 0,
    deoptimize = 1,
    null_pointer = 2,
    array_index_out_of_bounds = 3,
    array_store = 4,
    class_cast = 5,
    java_throwable = 6,
    budget_exhausted = 7,
    unsupported_call = 8,
    fatal_runtime_error = 9,
    retryable_call = 10,
};

enum class JitExceptionKind : u8 {
    null_pointer,
    array_index_out_of_bounds,
    array_store,
    class_cast,
    runtime_throwable,
};

inline constexpr u32 kJitRuntimeNoSafepointOperandFlag = 0x8000'0000U;

inline constexpr usize kJitRuntimeBudgetInitialByteOffset = 32U;
inline constexpr usize kJitRuntimeBudgetRemainingByteOffset = 36U;
inline constexpr usize kJitRuntimeConsumedByteOffset = 48U;
inline constexpr usize kJitRuntimeLocalSlotsByteOffset = 52U;
inline constexpr usize kJitRuntimeStackDepthByteOffset = 56U;
inline constexpr usize kJitRuntimeFrameHeaderBytes = 64U;

using JitRuntimeDispatch = u32 (*)(void* context,
                                   JitRuntimeOperation operation,
                                   u64 packed_operand,
                                   u64 first,
                                   u64 second,
                                   u64 third,
                                   const u64* frame_base,
                                   u64* result_bits);
using JitRootPublisher = void (*)(void* context,
                                  const u64* roots,
                                  usize root_count,
                                  const u64* frame_base,
                                  const u32* root_offsets,
                                  usize root_offset_count,
                                  bool defer_scheduler_publication);

struct JitRuntimeHooks final {
    void* context {nullptr};
    JitRuntimeDispatch dispatch {nullptr};
    JitRootPublisher publish_roots {nullptr};
};

struct JitInlineTarget final {
    std::shared_ptr<const classfile::ClassFile> owner;
    const classfile::Method* method {nullptr};
};

using JitInlineResolver = std::optional<JitInlineTarget> (*)(
    void* context,
    std::string_view owner,
    std::string_view name,
    std::string_view descriptor);

struct JitInlineResolverHooks final {
    void* context {nullptr};
    JitInlineResolver resolve {nullptr};
};

struct JitPhysicalFrameView final {
    u32 bytecode_pc {0U};
    u32 local_slots {0U};
    u32 stack_slots {0U};
    std::span<const u64> physical_slots;

    [[nodiscard]] usize expected_slot_count() const noexcept {
        return static_cast<usize>(local_slots) +
               static_cast<usize>(stack_slots);
    }

    [[nodiscard]] bool valid() const noexcept {
        return physical_slots.size() == expected_slot_count();
    }
};

struct JitDeoptState final {
    u32 bytecode_pc {0U};
    u32 local_slots {0U};
    u32 stack_slots {0U};
    // Physical JVM slots: locals first, followed by the operand stack.
    // Slot kinds come from RuntimeMethod::verified_frames when the interpreter
    // restores the frame, so a precise deopt copies only raw 64-bit payloads
    // instead of materializing Value/optional<Value> vectors.
    std::vector<u64> physical_slots;

    [[nodiscard]] JitPhysicalFrameView view() const noexcept {
        return JitPhysicalFrameView {
            .bytecode_pc = bytecode_pc,
            .local_slots = local_slots,
            .stack_slots = stack_slots,
            .physical_slots = physical_slots,
        };
    }
};

struct JitExecutionResult final {
    std::optional<Value> return_value;
    std::optional<JitExceptionKind> exception;
    u32 exception_bci {0};
    u32 bytecode_instructions {0};
    std::optional<JitDeoptState> deopt_state;
};

struct JitStatistics final {
    u64 compile_attempts {0};
    u64 compiled_methods {0};
    u64 executed_methods {0};
    u64 rejected_methods {0};
    u64 deoptimized_executions {0};
    u64 evicted_methods {0};
    u64 optimized_methods {0};
    u64 loop_optimized_methods {0};
    u64 propagated_constants {0};
    u64 folded_operations {0};
    u64 folded_branches {0};
    u64 strength_reductions {0};
    u64 dead_local_writes_eliminated {0};
    u64 common_subexpressions_eliminated {0};
    u64 cross_block_common_subexpressions_eliminated {0};
    u64 loop_invariants_hoisted {0};
    u64 inlined_calls {0};
    u64 inlined_bytecodes {0};
    u64 devirtualized_calls {0};
    u64 array_bounds_checks_eliminated {0};
    u64 array_runtime_calls_eliminated {0};
    u64 scalar_replaced_allocations {0};
    u64 scalar_replaced_array_accesses {0};
    u64 unrolled_int_array_loops {0};
    u64 budget_checks_elided {0};
    u64 code_cache_bytes {0};
    u64 code_cache_limit_bytes {0};
    u64 executable_mapped_bytes {0};
    u64 executable_slab_count {0};
    u64 warmup_fallbacks {0};
    u64 unsupported_fallbacks {0};
    u64 platform_fallbacks {0};
    u64 argument_fallbacks {0};
    u64 budget_fallbacks {0};
    u64 compile_time_nanoseconds {0};
    u64 execution_time_nanoseconds {0};
    u64 hot_loop_candidates {0};
    u64 osr_attempts {0};
    u64 osr_compiled_entries {0};
    u64 osr_executions {0};
    u64 osr_fallbacks {0};
    u64 osr_sync_promotions {0};
    u64 fast_chain_attempts {0};
    u64 fast_chain_hits {0};
    u64 fast_chain_runtime_hits {0};
    u64 fast_chain_deopt_resumes {0};
    u64 fast_chain_fallbacks {0};
    u64 register_cached_methods {0};
    u64 register_cached_loads {0};
    u64 register_cached_stores_elided {0};
    u64 stack_cached_methods {0};
    u64 stack_cached_pops {0};
    u64 stack_cached_stores_elided {0};
    u64 background_compile_queued {0};
    u64 background_compile_completed {0};
    u64 background_compile_published {0};
    u64 background_compile_discarded {0};
    u64 background_compile_time_nanoseconds {0};
    u64 background_compile_worker_count {0};
    u64 background_compile_queue_peak {0};
    u64 background_compile_render_cooldown_waits {0};
    u64 background_compile_render_cooldown_nanoseconds {0};
    u64 startup_compile_attempts {0};
    u64 startup_compiled_methods {0};
    u64 startup_compile_deferred {0};
    u64 startup_loop_analysis_deferred {0};
    u64 profile_loaded_methods {0};
    u64 profile_prewarm_hits {0};
    u64 profile_saved_methods {0};
    std::array<u64, kJitRejectReasonCount> reject_reasons {};
    std::array<u64, 256U> unsupported_opcodes {};
};

// ARM64 tiered optimizing JIT for verified Java bytecode. It supports scalar,
// reference, heap, allocation and call bytecodes with exact GC maps, native
// exception routing, precise deoptimization, OSR, background compilation,
// register/stack caching and guarded runtime dispatch. Every generated path
// remains constrained by the VM instruction budget.
class BaselineJit final {
public:
    explicit BaselineJit(bool enabled = true);
    ~BaselineJit();

    BaselineJit(const BaselineJit&) = delete;
    BaselineJit& operator=(const BaselineJit&) = delete;

    void set_enabled(bool enabled) noexcept;
    void set_startup_mode(bool enabled) noexcept;
    void note_frame_published() noexcept;
    void set_inline_resolver(JitInlineResolverHooks hooks) noexcept;
    void refresh_availability() noexcept;
    void clear_cache() noexcept;
    [[nodiscard]] Status configure_profile(std::string path);
    [[nodiscard]] Status flush_profile();
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool startup_mode() const noexcept;
    [[nodiscard]] JitAvailability availability() const noexcept;
    [[nodiscard]] JitStatistics statistics() const noexcept;

    // Native results pack the executed-bytecode count into 30 bits. Machine
    // uses this limit to admit only statically bounded calls when a Java thread
    // itself has a larger (for example effectively-unbounded) VM budget.
    [[nodiscard]] static constexpr u64 maximum_instruction_budget() noexcept {
        return 0x3FFF'FFFFU;
    }

    [[nodiscard]] Result<std::optional<JitExecutionResult>> try_execute(
        MethodId method_id,
        const classfile::ClassFile& owner,
        const classfile::Method& method,
        const CachedMethodDescriptor& descriptor,
        std::span<const Value> arguments,
        bool has_receiver,
        u64 instruction_budget,
        JitRuntimeHooks runtime_hooks = {},
        std::shared_ptr<const classfile::ClassFile> owner_lifetime = {},
        std::shared_ptr<const VerifiedMethodReferenceMaps> verified_frames = {},
        std::shared_ptr<const CachedMethodDescriptor> descriptor_lifetime = {});

    [[nodiscard]] Result<std::optional<JitExecutionResult>> try_execute(
        MethodId method_id,
        const classfile::ClassFile& owner,
        const classfile::Method& method,
        const CachedMethodDescriptor& descriptor,
        const InvocationArguments& arguments,
        bool has_receiver,
        u64 instruction_budget,
        JitRuntimeHooks runtime_hooks = {},
        std::shared_ptr<const classfile::ClassFile> owner_lifetime = {},
        std::shared_ptr<const VerifiedMethodReferenceMaps> verified_frames = {},
        std::shared_ptr<const CachedMethodDescriptor> descriptor_lifetime = {});

    [[nodiscard]] Result<std::optional<JitExecutionResult>> try_execute_osr(
        MethodId method_id,
        const classfile::ClassFile& owner,
        const classfile::Method& method,
        const CachedMethodDescriptor& descriptor,
        bool has_receiver,
        JitPhysicalFrameView frame,
        u64 instruction_budget,
        JitRuntimeHooks runtime_hooks = {},
        std::shared_ptr<const classfile::ClassFile> owner_lifetime = {},
        std::shared_ptr<const VerifiedMethodReferenceMaps> verified_frames = {},
        std::shared_ptr<const CachedMethodDescriptor> descriptor_lifetime = {});

    // Executes an already-compiled method directly without triggering
    // compilation. Runtime-dispatching entries are admitted only when hooks are
    // supplied, allowing Machine to preserve GC roots and resume a callee's
    // precise deopt frame without replaying the caller.
    [[nodiscard]] Result<std::optional<JitExecutionResult>> try_execute_cached(
        MethodId method_id,
        const classfile::ClassFile& owner,
        const classfile::Method& method,
        const CachedMethodDescriptor& descriptor,
        std::span<const Value> arguments,
        bool has_receiver,
        u64 instruction_budget,
        JitRuntimeHooks runtime_hooks = {});

    [[nodiscard]] Result<std::optional<JitExecutionResult>> try_execute_cached(
        MethodId method_id,
        const classfile::ClassFile& owner,
        const classfile::Method& method,
        const CachedMethodDescriptor& descriptor,
        const InvocationArguments& arguments,
        bool has_receiver,
        u64 instruction_budget,
        JitRuntimeHooks runtime_hooks = {});

    [[nodiscard]] static JitAvailability probe_platform() noexcept;
    [[nodiscard]] static bool conservative_device_mode() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace phoneme::vm
