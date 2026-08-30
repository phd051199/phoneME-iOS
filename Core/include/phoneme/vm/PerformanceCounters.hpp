#pragma once

#include <array>
#include <cstdint>

#include "phoneme/base/Types.hpp"

#ifndef PHONEME_ENABLE_VM_PROFILING
#define PHONEME_ENABLE_VM_PROFILING 0
#endif

namespace phoneme::vm {

inline constexpr usize kJitRuntimeOperationCounterCount = 43U;

enum class AllocationPayloadKind : u8 {
    object,
    array,
    clone,
    string_payload,
    count,
};

enum class LockedHeapOperationKind : u8 {
    allocation,
    field,
    array_element_read,
    array_element_write,
    array_element_snapshot,
    array_element_checked,
    array_metadata,
    array_bulk,
    class_name,
    string,
    reference,
    garbage_collection,
    count,
};

enum class LockedHeapCallerDomain : u8 {
    java_util,
    java_lang,
    io,
    lcdui,
    graphics_3d,
    media,
    other_java,
    unknown,
    count,
};

enum class LockedHeapIoOwnerKind : u8 {
    byte_array_input,
    byte_array_output,
    data_input,
    data_output,
    input_stream_reader,
    output_stream_writer,
    buffered_reader,
    buffered_output,
    reader,
    writer,
    other,
    count,
};

enum class LockedHeapGraphics3dOwnerKind : u8 {
    m3g_geometry,
    m3g_graphics,
    m3g_other,
    micro3d_math,
    micro3d_graphics,
    micro3d_resource,
    micro3d_state,
    micro3d_other,
    count,
};

struct PerformanceCounterSnapshot final {
    u64 executed_bytecodes {0};
    std::array<u64, 256> opcode_counts {};
    u64 method_invocations {0};
    u64 native_invocations {0};
    u64 maximum_java_call_depth {0};
    u64 invocation_argument_overflows {0};
    u64 maximum_invocation_argument_values {0};
    u64 oversized_execution_frames {0};
    u64 local_slot_storage_fallbacks {0};
    u64 operand_slot_storage_fallbacks {0};
    u64 maximum_frame_local_slots {0};
    u64 maximum_frame_operand_slots {0};
    u64 execution_frame_stack_growths {0};
    u64 maximum_execution_frame_stack_capacity {0};
    u64 execution_frame_stack_pool_misses {0};
    u64 execution_root_publications {0};
    u64 execution_root_copy_publications {0};
    u64 execution_root_exchange_publications {0};
    u64 execution_roots_published {0};
    u64 maximum_execution_roots_per_publication {0};
    u64 verified_root_map_hits {0};
    u64 verified_root_map_partial_hits {0};
    u64 verified_root_map_fallbacks {0};
    u64 verified_root_slots_visited {0};
    u64 verified_root_slots_avoided {0};
    u64 fallback_root_slots_scanned {0};
    u64 exception_dispatches {0};
    u64 class_initializations {0};
    u64 instruction_budget_exits {0};
    u64 scheduler_quanta {0};
    std::array<u64, kJitRuntimeOperationCounterCount>
        jit_runtime_operation_calls {};
    std::array<u64, kJitRuntimeOperationCounterCount>
        jit_runtime_safepoint_calls {};
    u64 jit_call_operand_decodes {0};
    u64 jit_call_operand_overflows {0};
    u64 maximum_jit_call_operand_values {0};
    u64 jit_root_stages {0};
    u64 jit_root_stage_commits {0};
    u64 jit_staged_root_materializations {0};
    u64 jit_staged_roots_materialized {0};
    u64 jit_staged_reference_slots_deferred {0};
    u64 jit_staged_reference_slots_scanned {0};

    u64 class_cache_hits {0};
    u64 class_cache_misses {0};
    u64 method_resolution_hits {0};
    u64 method_resolution_misses {0};
    u64 declared_method_resolution_hits {0};
    u64 declared_method_resolution_misses {0};
    u64 field_resolution_hits {0};
    u64 field_resolution_misses {0};
    u64 assignability_cache_hits {0};
    u64 assignability_cache_misses {0};
    u64 native_registry_lookups {0};
    u64 metadata_key_constructions {0};
    u64 virtual_inline_cache_hits {0};
    u64 virtual_inline_cache_misses {0};
    u64 direct_call_cache_hits {0};
    u64 direct_call_cache_misses {0};
    u64 operand_resolution_hits {0};
    u64 operand_resolution_misses {0};
    u64 operand_resolution_failures {0};
    u64 descriptor_cache_hits {0};
    u64 descriptor_cache_misses {0};
    u64 decoded_methods {0};
    u64 decoded_instructions {0};
    u64 decoded_operands {0};
    u64 decoded_switch_entries {0};
    u64 decoded_opcode_dispatches {0};
    u64 decoded_operand_dispatches {0};

    std::array<u64, static_cast<usize>(AllocationPayloadKind::count)>
        allocations_by_kind {};
    std::array<u64, static_cast<usize>(AllocationPayloadKind::count)>
        allocated_bytes_by_kind {};
    u64 failed_allocations {0};
    u64 public_locked_heap_operations {0};
    std::array<u64, static_cast<usize>(LockedHeapOperationKind::count)>
        locked_heap_operations_by_kind {};
    std::array<u64, static_cast<usize>(LockedHeapCallerDomain::count)>
        locked_heap_operations_by_domain {};
    std::array<u64,
               static_cast<usize>(LockedHeapCallerDomain::count) *
                   static_cast<usize>(LockedHeapOperationKind::count)>
        locked_heap_operations_by_domain_and_kind {};
    std::array<u64, static_cast<usize>(LockedHeapIoOwnerKind::count)>
        locked_heap_io_operations_by_owner {};
    std::array<u64, static_cast<usize>(LockedHeapGraphics3dOwnerKind::count)>
        locked_heap_graphics_3d_operations_by_owner {};
    u64 vm_fast_heap_operations {0};
    u64 vm_slot_cache_hits {0};
    u64 vm_slot_cache_misses {0};
    u64 gc_count {0};
    u64 gc_total_nanoseconds {0};
    u64 gc_max_pause_nanoseconds {0};
    u64 gc_roots_scanned {0};
    u64 gc_objects_scanned {0};
    u64 gc_objects_reclaimed {0};
    u64 gc_primitive_bytes_scanned {0};

    u64 scheduler_state_transitions {0};
    u64 scheduler_queue_erase_scans {0};
    u64 scheduler_yields {0};
    u64 scheduler_sleeps {0};
    u64 scheduler_event_wakeups {0};
    u64 scheduler_spurious_wakeups {0};

    u64 canvas_publications {0};
    u64 canvas_unchanged_publications {0};
    u64 canvas_dirty_pixels {0};
    u64 canvas_full_frame_pixels_avoided {0};
    u64 canvas_publication_nanoseconds {0};
    u64 frame_backpressure_waits {0};
    u64 frame_backpressure_nanoseconds {0};
    u64 maintenance_checks {0};
    u64 maintenance_background_checks {0};
    u64 resource_array_cache_hits {0};
    u64 resource_array_cache_misses {0};
    u64 resource_array_cache_evictions {0};
    u64 resource_array_cache_peak_bytes {0};
    u64 core_text_cache_hits {0};
    u64 core_text_cache_misses {0};
    u64 core_text_cache_evictions {0};
    u64 core_text_cache_peak_bytes {0};
};

class PerformanceCounters final {
public:
    [[nodiscard]] static constexpr bool enabled() noexcept {
        return PHONEME_ENABLE_VM_PROFILING != 0;
    }

#if PHONEME_ENABLE_VM_PROFILING
    static void reset() noexcept;
    [[nodiscard]] static PerformanceCounterSnapshot snapshot() noexcept;
    static void flush_thread_local() noexcept;

    static void record_opcode(u8 opcode) noexcept;
    static void record_method_invocation() noexcept;
    static void record_native_invocation() noexcept;
    static void observe_java_call_depth(usize depth) noexcept;
    static void observe_invocation_arguments(usize values,
                                             bool overflow) noexcept;
    static void observe_execution_frame_slots(usize locals,
                                              usize operands,
                                              bool local_fallback,
                                              bool operand_fallback) noexcept;
    static void observe_execution_frame_stack(usize capacity,
                                              bool grew) noexcept;
    static void record_execution_frame_stack_pool_miss() noexcept;
    static void observe_execution_root_publication(usize roots,
                                                   bool exchanged = false) noexcept;
    static void record_verified_root_scan(bool full_hit,
                                          bool partial_hit,
                                          usize verified_slots_visited,
                                          usize verified_slots_avoided,
                                          usize fallback_slots_scanned) noexcept;
    static void record_exception_dispatch() noexcept;
    static void record_class_initialization() noexcept;
    static void record_instruction_budget_exit() noexcept;
    static void record_scheduler_quantum() noexcept;
    static void record_jit_runtime_operation(u32 operation,
                                             bool safepoint) noexcept;
    static void observe_jit_call_operands(usize values,
                                          bool overflow) noexcept;
    static void record_jit_root_stage() noexcept;
    static void record_jit_root_stage_commit() noexcept;
    static void observe_jit_staged_root_materialization(usize roots) noexcept;
    static void observe_jit_staged_reference_slots(usize slots,
                                                   bool scanned) noexcept;

    static void record_class_cache(bool hit) noexcept;
    static void record_method_resolution(bool hit, bool declared) noexcept;
    static void record_field_resolution(bool hit) noexcept;
    static void record_assignability_cache(bool hit) noexcept;
    static void record_native_registry_lookup() noexcept;
    static void record_metadata_key_construction() noexcept;
    static void record_virtual_inline_cache(bool hit) noexcept;
    static void record_direct_call_cache(bool hit) noexcept;
    static void record_operand_resolution(bool hit) noexcept;
    static void record_operand_resolution_failure() noexcept;
    static void record_descriptor_cache(bool hit) noexcept;
    static void record_decoded_method(usize instructions,
                                      usize operands,
                                      usize switch_entries) noexcept;
    static void record_decoded_opcode_dispatch() noexcept;
    static void record_decoded_operand_dispatch() noexcept;

    static void record_allocation(AllocationPayloadKind kind, usize bytes) noexcept;
    static void record_failed_allocation() noexcept;
    static void record_locked_heap_operation(LockedHeapOperationKind kind) noexcept;
    static void record_vm_fast_heap_operation() noexcept;
    static void record_vm_slot_cache(bool hit) noexcept;
    static void record_gc(u64 pause_nanoseconds,
                          usize roots_scanned,
                          usize objects_scanned,
                          usize objects_reclaimed,
                          usize primitive_bytes_scanned) noexcept;

    static void record_scheduler_state_transition() noexcept;
    static void record_scheduler_queue_erase_scan(usize visited) noexcept;
    static void record_scheduler_yield() noexcept;
    static void record_scheduler_sleep() noexcept;
    static void record_scheduler_event_wakeup() noexcept;
    static void record_scheduler_spurious_wakeup() noexcept;
    static void record_canvas_publication(usize dirty_pixels,
                                          usize full_frame_pixels,
                                          u64 elapsed_nanoseconds) noexcept;
    static void record_canvas_unchanged_publication() noexcept;
    static void record_frame_backpressure(u64 wait_nanoseconds) noexcept;
    static void record_maintenance_check(bool background) noexcept;
    static void record_resource_array_cache(bool hit) noexcept;
    static void record_resource_array_cache_eviction() noexcept;
    static void observe_resource_array_cache_bytes(usize bytes) noexcept;
    static void record_core_text_cache(bool hit) noexcept;
    static void record_core_text_cache_eviction() noexcept;
    static void observe_core_text_cache_bytes(usize bytes) noexcept;
#else
    static constexpr void reset() noexcept {}
    [[nodiscard]] static constexpr PerformanceCounterSnapshot snapshot() noexcept {
        return {};
    }
    static constexpr void flush_thread_local() noexcept {}

    static constexpr void record_opcode(u8) noexcept {}
    static constexpr void record_method_invocation() noexcept {}
    static constexpr void record_native_invocation() noexcept {}
    static constexpr void observe_java_call_depth(usize) noexcept {}
    static constexpr void observe_invocation_arguments(usize, bool) noexcept {}
    static constexpr void observe_execution_frame_slots(
        usize, usize, bool, bool) noexcept {}
    static constexpr void observe_execution_frame_stack(usize, bool) noexcept {}
    static constexpr void record_execution_frame_stack_pool_miss() noexcept {}
    static constexpr void observe_execution_root_publication(
        usize, bool = false) noexcept {}
    static constexpr void record_verified_root_scan(
        bool, bool, usize, usize, usize) noexcept {}
    static constexpr void record_exception_dispatch() noexcept {}
    static constexpr void record_class_initialization() noexcept {}
    static constexpr void record_instruction_budget_exit() noexcept {}
    static constexpr void record_scheduler_quantum() noexcept {}
    static constexpr void record_jit_runtime_operation(u32, bool) noexcept {}
    static constexpr void observe_jit_call_operands(usize, bool) noexcept {}
    static constexpr void record_jit_root_stage() noexcept {}
    static constexpr void record_jit_root_stage_commit() noexcept {}
    static constexpr void observe_jit_staged_root_materialization(usize) noexcept {}
    static constexpr void observe_jit_staged_reference_slots(
        usize, bool) noexcept {}

    static constexpr void record_class_cache(bool) noexcept {}
    static constexpr void record_method_resolution(bool, bool) noexcept {}
    static constexpr void record_field_resolution(bool) noexcept {}
    static constexpr void record_assignability_cache(bool) noexcept {}
    static constexpr void record_native_registry_lookup() noexcept {}
    static constexpr void record_metadata_key_construction() noexcept {}
    static constexpr void record_virtual_inline_cache(bool) noexcept {}
    static constexpr void record_direct_call_cache(bool) noexcept {}
    static constexpr void record_operand_resolution(bool) noexcept {}
    static constexpr void record_operand_resolution_failure() noexcept {}
    static constexpr void record_descriptor_cache(bool) noexcept {}
    static constexpr void record_decoded_method(usize, usize, usize) noexcept {}
    static constexpr void record_decoded_opcode_dispatch() noexcept {}
    static constexpr void record_decoded_operand_dispatch() noexcept {}

    static constexpr void record_allocation(AllocationPayloadKind, usize) noexcept {}
    static constexpr void record_failed_allocation() noexcept {}
    static constexpr void record_locked_heap_operation(
        LockedHeapOperationKind) noexcept {}
    static constexpr void record_vm_fast_heap_operation() noexcept {}
    static constexpr void record_vm_slot_cache(bool) noexcept {}
    static constexpr void record_gc(u64, usize, usize, usize, usize) noexcept {}

    static constexpr void record_scheduler_state_transition() noexcept {}
    static constexpr void record_scheduler_queue_erase_scan(usize) noexcept {}
    static constexpr void record_scheduler_yield() noexcept {}
    static constexpr void record_scheduler_sleep() noexcept {}
    static constexpr void record_scheduler_event_wakeup() noexcept {}
    static constexpr void record_scheduler_spurious_wakeup() noexcept {}
    static constexpr void record_canvas_publication(usize, usize, u64) noexcept {}
    static constexpr void record_canvas_unchanged_publication() noexcept {}
    static constexpr void record_frame_backpressure(u64) noexcept {}
    static constexpr void record_maintenance_check(bool) noexcept {}
    static constexpr void record_resource_array_cache(bool) noexcept {}
    static constexpr void record_resource_array_cache_eviction() noexcept {}
    static constexpr void observe_resource_array_cache_bytes(usize) noexcept {}
    static constexpr void record_core_text_cache(bool) noexcept {}
    static constexpr void record_core_text_cache_eviction() noexcept {}
    static constexpr void observe_core_text_cache_bytes(usize) noexcept {}
#endif
};

} // namespace phoneme::vm
