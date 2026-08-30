#include "phoneme/vm/PerformanceCounters.hpp"

#if PHONEME_ENABLE_VM_PROFILING

#include <algorithm>
#include <mutex>

#include "phoneme/vm/Heap.hpp"

namespace phoneme::vm {
namespace {

struct GlobalCounters final {
    std::mutex mutex;
    PerformanceCounterSnapshot snapshot;
};

GlobalCounters& global_counters() noexcept {
    static GlobalCounters counters;
    return counters;
}

thread_local PerformanceCounterSnapshot g_local_counters {};
thread_local bool g_local_dirty {false};

void mark_dirty() noexcept {
    g_local_dirty = true;
}

void merge_snapshot(PerformanceCounterSnapshot& destination,
                    const PerformanceCounterSnapshot& source) noexcept {
    destination.executed_bytecodes += source.executed_bytecodes;
    for (usize index = 0; index < destination.opcode_counts.size(); ++index) {
        destination.opcode_counts[index] += source.opcode_counts[index];
    }
    destination.method_invocations += source.method_invocations;
    destination.native_invocations += source.native_invocations;
    destination.maximum_java_call_depth = std::max(
        destination.maximum_java_call_depth,
        source.maximum_java_call_depth);
    destination.invocation_argument_overflows +=
        source.invocation_argument_overflows;
    destination.maximum_invocation_argument_values = std::max(
        destination.maximum_invocation_argument_values,
        source.maximum_invocation_argument_values);
    destination.oversized_execution_frames += source.oversized_execution_frames;
    destination.local_slot_storage_fallbacks +=
        source.local_slot_storage_fallbacks;
    destination.operand_slot_storage_fallbacks +=
        source.operand_slot_storage_fallbacks;
    destination.maximum_frame_local_slots = std::max(
        destination.maximum_frame_local_slots,
        source.maximum_frame_local_slots);
    destination.maximum_frame_operand_slots = std::max(
        destination.maximum_frame_operand_slots,
        source.maximum_frame_operand_slots);
    destination.execution_frame_stack_growths +=
        source.execution_frame_stack_growths;
    destination.maximum_execution_frame_stack_capacity = std::max(
        destination.maximum_execution_frame_stack_capacity,
        source.maximum_execution_frame_stack_capacity);
    destination.execution_frame_stack_pool_misses +=
        source.execution_frame_stack_pool_misses;
    destination.execution_root_publications +=
        source.execution_root_publications;
    destination.execution_root_copy_publications +=
        source.execution_root_copy_publications;
    destination.execution_root_exchange_publications +=
        source.execution_root_exchange_publications;
    destination.execution_roots_published +=
        source.execution_roots_published;
    destination.maximum_execution_roots_per_publication = std::max(
        destination.maximum_execution_roots_per_publication,
        source.maximum_execution_roots_per_publication);
    destination.verified_root_map_hits += source.verified_root_map_hits;
    destination.verified_root_map_partial_hits +=
        source.verified_root_map_partial_hits;
    destination.verified_root_map_fallbacks += source.verified_root_map_fallbacks;
    destination.verified_root_slots_visited += source.verified_root_slots_visited;
    destination.verified_root_slots_avoided += source.verified_root_slots_avoided;
    destination.fallback_root_slots_scanned += source.fallback_root_slots_scanned;
    destination.exception_dispatches += source.exception_dispatches;
    destination.class_initializations += source.class_initializations;
    destination.instruction_budget_exits += source.instruction_budget_exits;
    destination.scheduler_quanta += source.scheduler_quanta;
    for (usize index = 0U;
         index < destination.jit_runtime_operation_calls.size();
         ++index) {
        destination.jit_runtime_operation_calls[index] +=
            source.jit_runtime_operation_calls[index];
        destination.jit_runtime_safepoint_calls[index] +=
            source.jit_runtime_safepoint_calls[index];
    }
    destination.jit_call_operand_decodes += source.jit_call_operand_decodes;
    destination.jit_call_operand_overflows += source.jit_call_operand_overflows;
    destination.maximum_jit_call_operand_values = std::max(
        destination.maximum_jit_call_operand_values,
        source.maximum_jit_call_operand_values);
    destination.jit_root_stages += source.jit_root_stages;
    destination.jit_root_stage_commits += source.jit_root_stage_commits;
    destination.jit_staged_root_materializations +=
        source.jit_staged_root_materializations;
    destination.jit_staged_roots_materialized +=
        source.jit_staged_roots_materialized;
    destination.jit_staged_reference_slots_deferred +=
        source.jit_staged_reference_slots_deferred;
    destination.jit_staged_reference_slots_scanned +=
        source.jit_staged_reference_slots_scanned;

    destination.class_cache_hits += source.class_cache_hits;
    destination.class_cache_misses += source.class_cache_misses;
    destination.method_resolution_hits += source.method_resolution_hits;
    destination.method_resolution_misses += source.method_resolution_misses;
    destination.declared_method_resolution_hits +=
        source.declared_method_resolution_hits;
    destination.declared_method_resolution_misses +=
        source.declared_method_resolution_misses;
    destination.field_resolution_hits += source.field_resolution_hits;
    destination.field_resolution_misses += source.field_resolution_misses;
    destination.assignability_cache_hits += source.assignability_cache_hits;
    destination.assignability_cache_misses += source.assignability_cache_misses;
    destination.native_registry_lookups += source.native_registry_lookups;
    destination.metadata_key_constructions +=
        source.metadata_key_constructions;
    destination.virtual_inline_cache_hits += source.virtual_inline_cache_hits;
    destination.virtual_inline_cache_misses += source.virtual_inline_cache_misses;
    destination.direct_call_cache_hits += source.direct_call_cache_hits;
    destination.direct_call_cache_misses += source.direct_call_cache_misses;
    destination.operand_resolution_hits += source.operand_resolution_hits;
    destination.operand_resolution_misses += source.operand_resolution_misses;
    destination.operand_resolution_failures += source.operand_resolution_failures;
    destination.descriptor_cache_hits += source.descriptor_cache_hits;
    destination.descriptor_cache_misses += source.descriptor_cache_misses;
    destination.decoded_methods += source.decoded_methods;
    destination.decoded_instructions += source.decoded_instructions;
    destination.decoded_operands += source.decoded_operands;
    destination.decoded_switch_entries += source.decoded_switch_entries;
    destination.decoded_opcode_dispatches += source.decoded_opcode_dispatches;
    destination.decoded_operand_dispatches += source.decoded_operand_dispatches;

    for (usize index = 0; index < destination.allocations_by_kind.size(); ++index) {
        destination.allocations_by_kind[index] +=
            source.allocations_by_kind[index];
        destination.allocated_bytes_by_kind[index] +=
            source.allocated_bytes_by_kind[index];
    }
    destination.failed_allocations += source.failed_allocations;
    destination.public_locked_heap_operations +=
        source.public_locked_heap_operations;
    for (usize index = 0;
         index < destination.locked_heap_operations_by_kind.size();
         ++index) {
        destination.locked_heap_operations_by_kind[index] +=
            source.locked_heap_operations_by_kind[index];
    }
    for (usize index = 0;
         index < destination.locked_heap_operations_by_domain.size();
         ++index) {
        destination.locked_heap_operations_by_domain[index] +=
            source.locked_heap_operations_by_domain[index];
    }
    for (usize index = 0;
         index < destination.locked_heap_operations_by_domain_and_kind.size();
         ++index) {
        destination.locked_heap_operations_by_domain_and_kind[index] +=
            source.locked_heap_operations_by_domain_and_kind[index];
    }
    for (usize index = 0;
         index < destination.locked_heap_io_operations_by_owner.size();
         ++index) {
        destination.locked_heap_io_operations_by_owner[index] +=
            source.locked_heap_io_operations_by_owner[index];
    }
    for (usize index = 0;
         index < destination.locked_heap_graphics_3d_operations_by_owner.size();
         ++index) {
        destination.locked_heap_graphics_3d_operations_by_owner[index] +=
            source.locked_heap_graphics_3d_operations_by_owner[index];
    }
    destination.vm_fast_heap_operations += source.vm_fast_heap_operations;
    destination.vm_slot_cache_hits += source.vm_slot_cache_hits;
    destination.vm_slot_cache_misses += source.vm_slot_cache_misses;
    destination.gc_count += source.gc_count;
    destination.gc_total_nanoseconds += source.gc_total_nanoseconds;
    destination.gc_max_pause_nanoseconds = std::max(
        destination.gc_max_pause_nanoseconds,
        source.gc_max_pause_nanoseconds);
    destination.gc_roots_scanned += source.gc_roots_scanned;
    destination.gc_objects_scanned += source.gc_objects_scanned;
    destination.gc_objects_reclaimed += source.gc_objects_reclaimed;
    destination.gc_primitive_bytes_scanned += source.gc_primitive_bytes_scanned;

    destination.scheduler_state_transitions +=
        source.scheduler_state_transitions;
    destination.scheduler_queue_erase_scans +=
        source.scheduler_queue_erase_scans;
    destination.scheduler_yields += source.scheduler_yields;
    destination.scheduler_sleeps += source.scheduler_sleeps;
    destination.scheduler_event_wakeups += source.scheduler_event_wakeups;
    destination.scheduler_spurious_wakeups += source.scheduler_spurious_wakeups;
    destination.canvas_publications += source.canvas_publications;
    destination.canvas_unchanged_publications +=
        source.canvas_unchanged_publications;
    destination.canvas_dirty_pixels += source.canvas_dirty_pixels;
    destination.canvas_full_frame_pixels_avoided +=
        source.canvas_full_frame_pixels_avoided;
    destination.canvas_publication_nanoseconds +=
        source.canvas_publication_nanoseconds;
    destination.frame_backpressure_waits += source.frame_backpressure_waits;
    destination.frame_backpressure_nanoseconds +=
        source.frame_backpressure_nanoseconds;
    destination.maintenance_checks += source.maintenance_checks;
    destination.maintenance_background_checks +=
        source.maintenance_background_checks;
    destination.resource_array_cache_hits += source.resource_array_cache_hits;
    destination.resource_array_cache_misses += source.resource_array_cache_misses;
    destination.resource_array_cache_evictions +=
        source.resource_array_cache_evictions;
    destination.resource_array_cache_peak_bytes = std::max(
        destination.resource_array_cache_peak_bytes,
        source.resource_array_cache_peak_bytes);
    destination.core_text_cache_hits += source.core_text_cache_hits;
    destination.core_text_cache_misses += source.core_text_cache_misses;
    destination.core_text_cache_evictions += source.core_text_cache_evictions;
    destination.core_text_cache_peak_bytes = std::max(
        destination.core_text_cache_peak_bytes,
        source.core_text_cache_peak_bytes);
}

} // namespace

void PerformanceCounters::reset() noexcept {
    auto& global = global_counters();
    std::scoped_lock lock(global.mutex);
    global.snapshot = {};
    g_local_counters = {};
    g_local_dirty = false;
}

PerformanceCounterSnapshot PerformanceCounters::snapshot() noexcept {
    flush_thread_local();
    auto& global = global_counters();
    std::scoped_lock lock(global.mutex);
    return global.snapshot;
}

void PerformanceCounters::flush_thread_local() noexcept {
    if (!g_local_dirty) return;
    auto& global = global_counters();
    {
        std::scoped_lock lock(global.mutex);
        merge_snapshot(global.snapshot, g_local_counters);
    }
    g_local_counters = {};
    g_local_dirty = false;
}

void PerformanceCounters::record_opcode(u8 opcode) noexcept {
    ++g_local_counters.executed_bytecodes;
    ++g_local_counters.opcode_counts[opcode];
    mark_dirty();
}

void PerformanceCounters::record_method_invocation() noexcept {
    ++g_local_counters.method_invocations;
    mark_dirty();
}

void PerformanceCounters::record_native_invocation() noexcept {
    ++g_local_counters.native_invocations;
    mark_dirty();
}

void PerformanceCounters::observe_java_call_depth(usize depth) noexcept {
    g_local_counters.maximum_java_call_depth = std::max(
        g_local_counters.maximum_java_call_depth,
        static_cast<u64>(depth));
    mark_dirty();
}

void PerformanceCounters::observe_invocation_arguments(usize values,
                                                        bool overflow) noexcept {
    if (overflow) ++g_local_counters.invocation_argument_overflows;
    g_local_counters.maximum_invocation_argument_values = std::max(
        g_local_counters.maximum_invocation_argument_values,
        static_cast<u64>(values));
    mark_dirty();
}

void PerformanceCounters::observe_execution_frame_slots(usize locals,
                                                         usize operands,
                                                         bool local_fallback,
                                                         bool operand_fallback) noexcept {
    if (local_fallback || operand_fallback) {
        ++g_local_counters.oversized_execution_frames;
    }
    if (local_fallback) ++g_local_counters.local_slot_storage_fallbacks;
    if (operand_fallback) ++g_local_counters.operand_slot_storage_fallbacks;
    g_local_counters.maximum_frame_local_slots = std::max(
        g_local_counters.maximum_frame_local_slots,
        static_cast<u64>(locals));
    g_local_counters.maximum_frame_operand_slots = std::max(
        g_local_counters.maximum_frame_operand_slots,
        static_cast<u64>(operands));
    mark_dirty();
}

void PerformanceCounters::observe_execution_frame_stack(usize capacity,
                                                         bool grew) noexcept {
    if (grew) ++g_local_counters.execution_frame_stack_growths;
    g_local_counters.maximum_execution_frame_stack_capacity = std::max(
        g_local_counters.maximum_execution_frame_stack_capacity,
        static_cast<u64>(capacity));
    mark_dirty();
}

void PerformanceCounters::record_execution_frame_stack_pool_miss() noexcept {
    ++g_local_counters.execution_frame_stack_pool_misses;
    mark_dirty();
}

void PerformanceCounters::observe_execution_root_publication(
    usize roots,
    bool exchanged) noexcept {
    ++g_local_counters.execution_root_publications;
    if (exchanged) {
        ++g_local_counters.execution_root_exchange_publications;
    } else {
        ++g_local_counters.execution_root_copy_publications;
    }
    g_local_counters.execution_roots_published += static_cast<u64>(roots);
    g_local_counters.maximum_execution_roots_per_publication = std::max(
        g_local_counters.maximum_execution_roots_per_publication,
        static_cast<u64>(roots));
    mark_dirty();
}

void PerformanceCounters::record_verified_root_scan(
    bool full_hit,
    bool partial_hit,
    usize verified_slots_visited,
    usize verified_slots_avoided,
    usize fallback_slots_scanned) noexcept {
    if (full_hit) {
        ++g_local_counters.verified_root_map_hits;
    } else if (partial_hit) {
        ++g_local_counters.verified_root_map_partial_hits;
    } else {
        ++g_local_counters.verified_root_map_fallbacks;
    }
    g_local_counters.verified_root_slots_visited +=
        static_cast<u64>(verified_slots_visited);
    g_local_counters.verified_root_slots_avoided +=
        static_cast<u64>(verified_slots_avoided);
    g_local_counters.fallback_root_slots_scanned +=
        static_cast<u64>(fallback_slots_scanned);
    mark_dirty();
}

void PerformanceCounters::record_exception_dispatch() noexcept {
    ++g_local_counters.exception_dispatches;
    mark_dirty();
}

void PerformanceCounters::record_class_initialization() noexcept {
    ++g_local_counters.class_initializations;
    mark_dirty();
}

void PerformanceCounters::record_instruction_budget_exit() noexcept {
    ++g_local_counters.instruction_budget_exits;
    mark_dirty();
}

void PerformanceCounters::record_scheduler_quantum() noexcept {
    ++g_local_counters.scheduler_quanta;
    mark_dirty();
}

void PerformanceCounters::record_jit_runtime_operation(
    u32 operation,
    bool safepoint) noexcept {
    const usize index = static_cast<usize>(operation);
    if (index >= g_local_counters.jit_runtime_operation_calls.size()) return;
    ++g_local_counters.jit_runtime_operation_calls[index];
    if (safepoint) ++g_local_counters.jit_runtime_safepoint_calls[index];
    mark_dirty();
}

void PerformanceCounters::observe_jit_call_operands(
    usize values,
    bool overflow) noexcept {
    ++g_local_counters.jit_call_operand_decodes;
    if (overflow) ++g_local_counters.jit_call_operand_overflows;
    g_local_counters.maximum_jit_call_operand_values = std::max(
        g_local_counters.maximum_jit_call_operand_values,
        static_cast<u64>(values));
    mark_dirty();
}

void PerformanceCounters::record_jit_root_stage() noexcept {
    ++g_local_counters.jit_root_stages;
    mark_dirty();
}

void PerformanceCounters::record_jit_root_stage_commit() noexcept {
    ++g_local_counters.jit_root_stage_commits;
    mark_dirty();
}

void PerformanceCounters::observe_jit_staged_root_materialization(
    usize roots) noexcept {
    ++g_local_counters.jit_staged_root_materializations;
    g_local_counters.jit_staged_roots_materialized += static_cast<u64>(roots);
    mark_dirty();
}

void PerformanceCounters::observe_jit_staged_reference_slots(
    usize slots,
    bool scanned) noexcept {
    if (scanned) {
        g_local_counters.jit_staged_reference_slots_scanned +=
            static_cast<u64>(slots);
    } else {
        g_local_counters.jit_staged_reference_slots_deferred +=
            static_cast<u64>(slots);
    }
    mark_dirty();
}

void PerformanceCounters::record_class_cache(bool hit) noexcept {
    if (hit) ++g_local_counters.class_cache_hits;
    else ++g_local_counters.class_cache_misses;
    mark_dirty();
}

void PerformanceCounters::record_method_resolution(bool hit,
                                                      bool declared) noexcept {
    if (declared) {
        if (hit) ++g_local_counters.declared_method_resolution_hits;
        else ++g_local_counters.declared_method_resolution_misses;
    } else {
        if (hit) ++g_local_counters.method_resolution_hits;
        else ++g_local_counters.method_resolution_misses;
    }
    mark_dirty();
}

void PerformanceCounters::record_field_resolution(bool hit) noexcept {
    if (hit) ++g_local_counters.field_resolution_hits;
    else ++g_local_counters.field_resolution_misses;
    mark_dirty();
}

void PerformanceCounters::record_assignability_cache(bool hit) noexcept {
    if (hit) ++g_local_counters.assignability_cache_hits;
    else ++g_local_counters.assignability_cache_misses;
    mark_dirty();
}

void PerformanceCounters::record_native_registry_lookup() noexcept {
    ++g_local_counters.native_registry_lookups;
    mark_dirty();
}

void PerformanceCounters::record_metadata_key_construction() noexcept {
    ++g_local_counters.metadata_key_constructions;
    mark_dirty();
}

void PerformanceCounters::record_virtual_inline_cache(bool hit) noexcept {
    if (hit) ++g_local_counters.virtual_inline_cache_hits;
    else ++g_local_counters.virtual_inline_cache_misses;
    mark_dirty();
}

void PerformanceCounters::record_direct_call_cache(bool hit) noexcept {
    if (hit) ++g_local_counters.direct_call_cache_hits;
    else ++g_local_counters.direct_call_cache_misses;
    mark_dirty();
}

void PerformanceCounters::record_operand_resolution(bool hit) noexcept {
    if (hit) ++g_local_counters.operand_resolution_hits;
    else ++g_local_counters.operand_resolution_misses;
    mark_dirty();
}

void PerformanceCounters::record_operand_resolution_failure() noexcept {
    ++g_local_counters.operand_resolution_failures;
    mark_dirty();
}

void PerformanceCounters::record_descriptor_cache(bool hit) noexcept {
    if (hit) ++g_local_counters.descriptor_cache_hits;
    else ++g_local_counters.descriptor_cache_misses;
    mark_dirty();
}

void PerformanceCounters::record_decoded_method(
    usize instructions,
    usize operands,
    usize switch_entries) noexcept {
    ++g_local_counters.decoded_methods;
    g_local_counters.decoded_instructions += static_cast<u64>(instructions);
    g_local_counters.decoded_operands += static_cast<u64>(operands);
    g_local_counters.decoded_switch_entries +=
        static_cast<u64>(switch_entries);
    mark_dirty();
}

void PerformanceCounters::record_decoded_opcode_dispatch() noexcept {
    ++g_local_counters.decoded_opcode_dispatches;
    mark_dirty();
}

void PerformanceCounters::record_decoded_operand_dispatch() noexcept {
    ++g_local_counters.decoded_operand_dispatches;
    mark_dirty();
}

void PerformanceCounters::record_allocation(AllocationPayloadKind kind,
                                             usize bytes) noexcept {
    const usize index = static_cast<usize>(kind);
    if (index >= g_local_counters.allocations_by_kind.size()) return;
    ++g_local_counters.allocations_by_kind[index];
    g_local_counters.allocated_bytes_by_kind[index] += static_cast<u64>(bytes);
    mark_dirty();
}

void PerformanceCounters::record_failed_allocation() noexcept {
    ++g_local_counters.failed_allocations;
    mark_dirty();
}

void PerformanceCounters::record_locked_heap_operation(
    LockedHeapOperationKind kind) noexcept {
    ++g_local_counters.public_locked_heap_operations;
    const usize index = static_cast<usize>(kind);
    if (index < g_local_counters.locked_heap_operations_by_kind.size()) {
        ++g_local_counters.locked_heap_operations_by_kind[index];
    }
    const std::string_view owner = current_heap_access_context().owner;
    LockedHeapCallerDomain domain = LockedHeapCallerDomain::unknown;
    if (owner.starts_with("java/util/")) {
        domain = LockedHeapCallerDomain::java_util;
    } else if (owner.starts_with("java/lang/")) {
        domain = LockedHeapCallerDomain::java_lang;
    } else if (owner.starts_with("java/io/") ||
               owner.starts_with("javax/microedition/io/")) {
        domain = LockedHeapCallerDomain::io;
    } else if (owner.starts_with("javax/microedition/lcdui/")) {
        domain = LockedHeapCallerDomain::lcdui;
    } else if (owner.starts_with("javax/microedition/m3g/") ||
               owner.starts_with("com/mascotcapsule/")) {
        domain = LockedHeapCallerDomain::graphics_3d;
    } else if (owner.starts_with("javax/microedition/media/")) {
        domain = LockedHeapCallerDomain::media;
    } else if (!owner.empty()) {
        domain = LockedHeapCallerDomain::other_java;
    }
    const usize domain_index = static_cast<usize>(domain);
    if (domain_index < g_local_counters.locked_heap_operations_by_domain.size()) {
        ++g_local_counters.locked_heap_operations_by_domain[domain_index];
    }
    const usize kind_index = static_cast<usize>(kind);
    const usize kinds_per_domain =
        static_cast<usize>(LockedHeapOperationKind::count);
    const usize cross_index = domain_index * kinds_per_domain + kind_index;
    if (cross_index <
        g_local_counters.locked_heap_operations_by_domain_and_kind.size()) {
        ++g_local_counters.locked_heap_operations_by_domain_and_kind[cross_index];
    }
    if (domain == LockedHeapCallerDomain::io) {
        LockedHeapIoOwnerKind io_owner = LockedHeapIoOwnerKind::other;
        if (owner == "java/io/ByteArrayInputStream") {
            io_owner = LockedHeapIoOwnerKind::byte_array_input;
        } else if (owner == "java/io/ByteArrayOutputStream") {
            io_owner = LockedHeapIoOwnerKind::byte_array_output;
        } else if (owner == "java/io/DataInputStream") {
            io_owner = LockedHeapIoOwnerKind::data_input;
        } else if (owner == "java/io/DataOutputStream") {
            io_owner = LockedHeapIoOwnerKind::data_output;
        } else if (owner == "java/io/InputStreamReader") {
            io_owner = LockedHeapIoOwnerKind::input_stream_reader;
        } else if (owner == "java/io/OutputStreamWriter") {
            io_owner = LockedHeapIoOwnerKind::output_stream_writer;
        } else if (owner == "java/io/BufferedReader") {
            io_owner = LockedHeapIoOwnerKind::buffered_reader;
        } else if (owner == "java/io/BufferedOutputStream") {
            io_owner = LockedHeapIoOwnerKind::buffered_output;
        } else if (owner == "java/io/Reader") {
            io_owner = LockedHeapIoOwnerKind::reader;
        } else if (owner == "java/io/Writer") {
            io_owner = LockedHeapIoOwnerKind::writer;
        }
        const usize io_owner_index = static_cast<usize>(io_owner);
        if (io_owner_index <
            g_local_counters.locked_heap_io_operations_by_owner.size()) {
            ++g_local_counters.locked_heap_io_operations_by_owner[
                io_owner_index];
        }
    }
    if (domain == LockedHeapCallerDomain::graphics_3d) {
        LockedHeapGraphics3dOwnerKind graphics_owner =
            LockedHeapGraphics3dOwnerKind::m3g_other;
        if (owner.starts_with("javax/microedition/m3g/")) {
            if (owner == "javax/microedition/m3g/Graphics3D") {
                graphics_owner = LockedHeapGraphics3dOwnerKind::m3g_graphics;
            } else if (owner == "javax/microedition/m3g/Image2D" ||
                       owner == "javax/microedition/m3g/Texture2D" ||
                       owner == "javax/microedition/m3g/VertexArray" ||
                       owner == "javax/microedition/m3g/VertexBuffer" ||
                       owner == "javax/microedition/m3g/IndexBuffer" ||
                       owner == "javax/microedition/m3g/TriangleStripArray" ||
                       owner == "javax/microedition/m3g/Mesh" ||
                       owner == "javax/microedition/m3g/SkinnedMesh" ||
                       owner == "javax/microedition/m3g/MorphingMesh" ||
                       owner == "javax/microedition/m3g/Sprite3D") {
                graphics_owner = LockedHeapGraphics3dOwnerKind::m3g_geometry;
            }
        } else {
            graphics_owner = LockedHeapGraphics3dOwnerKind::micro3d_other;
            if (owner == "com/mascotcapsule/micro3d/v3/Graphics3D") {
                graphics_owner = LockedHeapGraphics3dOwnerKind::micro3d_graphics;
            } else if (owner == "com/mascotcapsule/micro3d/v3/Vector3D" ||
                       owner == "com/mascotcapsule/micro3d/v3/AffineTrans" ||
                       owner == "com/mascotcapsule/micro3d/v3/Util3D") {
                graphics_owner = LockedHeapGraphics3dOwnerKind::micro3d_math;
            } else if (owner == "com/mascotcapsule/micro3d/v3/Texture" ||
                       owner == "com/mascotcapsule/micro3d/v3/ActionTable" ||
                       owner == "com/mascotcapsule/micro3d/v3/Figure") {
                graphics_owner = LockedHeapGraphics3dOwnerKind::micro3d_resource;
            } else if (owner == "com/mascotcapsule/micro3d/v3/Light" ||
                       owner == "com/mascotcapsule/micro3d/v3/Effect3D" ||
                       owner == "com/mascotcapsule/micro3d/v3/FigureLayout") {
                graphics_owner = LockedHeapGraphics3dOwnerKind::micro3d_state;
            }
        }
        const usize graphics_owner_index = static_cast<usize>(graphics_owner);
        if (graphics_owner_index <
            g_local_counters.locked_heap_graphics_3d_operations_by_owner.size()) {
            ++g_local_counters.locked_heap_graphics_3d_operations_by_owner[
                graphics_owner_index];
        }
    }
    mark_dirty();
}

void PerformanceCounters::record_vm_fast_heap_operation() noexcept {
    ++g_local_counters.vm_fast_heap_operations;
    mark_dirty();
}

void PerformanceCounters::record_vm_slot_cache(bool hit) noexcept {
    if (hit) ++g_local_counters.vm_slot_cache_hits;
    else ++g_local_counters.vm_slot_cache_misses;
    mark_dirty();
}

void PerformanceCounters::record_gc(u64 pause_nanoseconds,
                                    usize roots_scanned,
                                    usize objects_scanned,
                                    usize objects_reclaimed,
                                    usize primitive_bytes_scanned) noexcept {
    ++g_local_counters.gc_count;
    g_local_counters.gc_total_nanoseconds += pause_nanoseconds;
    g_local_counters.gc_max_pause_nanoseconds = std::max(
        g_local_counters.gc_max_pause_nanoseconds,
        pause_nanoseconds);
    g_local_counters.gc_roots_scanned += static_cast<u64>(roots_scanned);
    g_local_counters.gc_objects_scanned += static_cast<u64>(objects_scanned);
    g_local_counters.gc_objects_reclaimed += static_cast<u64>(objects_reclaimed);
    g_local_counters.gc_primitive_bytes_scanned +=
        static_cast<u64>(primitive_bytes_scanned);
    mark_dirty();
}

void PerformanceCounters::record_scheduler_state_transition() noexcept {
    ++g_local_counters.scheduler_state_transitions;
    mark_dirty();
}

void PerformanceCounters::record_scheduler_queue_erase_scan(usize visited) noexcept {
    g_local_counters.scheduler_queue_erase_scans += static_cast<u64>(visited);
    mark_dirty();
}

void PerformanceCounters::record_scheduler_yield() noexcept {
    ++g_local_counters.scheduler_yields;
    mark_dirty();
}

void PerformanceCounters::record_scheduler_sleep() noexcept {
    ++g_local_counters.scheduler_sleeps;
    mark_dirty();
}

void PerformanceCounters::record_scheduler_event_wakeup() noexcept {
    ++g_local_counters.scheduler_event_wakeups;
    mark_dirty();
}

void PerformanceCounters::record_scheduler_spurious_wakeup() noexcept {
    ++g_local_counters.scheduler_spurious_wakeups;
    mark_dirty();
}

void PerformanceCounters::record_canvas_publication(
    usize dirty_pixels,
    usize full_frame_pixels,
    u64 elapsed_nanoseconds) noexcept {
    ++g_local_counters.canvas_publications;
    g_local_counters.canvas_dirty_pixels += static_cast<u64>(dirty_pixels);
    if (full_frame_pixels > dirty_pixels) {
        g_local_counters.canvas_full_frame_pixels_avoided +=
            static_cast<u64>(full_frame_pixels - dirty_pixels);
    }
    g_local_counters.canvas_publication_nanoseconds += elapsed_nanoseconds;
    mark_dirty();
}

void PerformanceCounters::record_canvas_unchanged_publication() noexcept {
    ++g_local_counters.canvas_unchanged_publications;
    mark_dirty();
}

void PerformanceCounters::record_frame_backpressure(u64 wait_nanoseconds) noexcept {
    ++g_local_counters.frame_backpressure_waits;
    g_local_counters.frame_backpressure_nanoseconds += wait_nanoseconds;
    mark_dirty();
}

void PerformanceCounters::record_maintenance_check(bool background) noexcept {
    ++g_local_counters.maintenance_checks;
    if (background) ++g_local_counters.maintenance_background_checks;
    mark_dirty();
}

void PerformanceCounters::record_resource_array_cache(bool hit) noexcept {
    if (hit) ++g_local_counters.resource_array_cache_hits;
    else ++g_local_counters.resource_array_cache_misses;
    mark_dirty();
}

void PerformanceCounters::record_resource_array_cache_eviction() noexcept {
    ++g_local_counters.resource_array_cache_evictions;
    mark_dirty();
}

void PerformanceCounters::observe_resource_array_cache_bytes(usize bytes) noexcept {
    g_local_counters.resource_array_cache_peak_bytes = std::max(
        g_local_counters.resource_array_cache_peak_bytes,
        static_cast<u64>(bytes));
    mark_dirty();
}

void PerformanceCounters::record_core_text_cache(bool hit) noexcept {
    if (hit) ++g_local_counters.core_text_cache_hits;
    else ++g_local_counters.core_text_cache_misses;
    mark_dirty();
}

void PerformanceCounters::record_core_text_cache_eviction() noexcept {
    ++g_local_counters.core_text_cache_evictions;
    mark_dirty();
}

void PerformanceCounters::observe_core_text_cache_bytes(usize bytes) noexcept {
    g_local_counters.core_text_cache_peak_bytes = std::max(
        g_local_counters.core_text_cache_peak_bytes,
        static_cast<u64>(bytes));
    mark_dirty();
}

} // namespace phoneme::vm

#endif
