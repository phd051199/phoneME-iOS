#include <cstdlib>
#include <iostream>

#include "phoneme/vm/NativeMethodRegistry.hpp"
#include "phoneme/vm/PerformanceCounters.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

} // namespace

int main() {
    using phoneme::vm::AllocationPayloadKind;
    using phoneme::vm::LockedHeapOperationKind;
    using phoneme::vm::PerformanceCounters;

    require(PerformanceCounters::enabled(),
            "performance counter test requires profiling enabled");
    PerformanceCounters::reset();

    PerformanceCounters::record_opcode(0x60U);
    PerformanceCounters::record_opcode(0x60U);
    PerformanceCounters::record_opcode(0xB6U);
    PerformanceCounters::record_method_invocation();
    PerformanceCounters::record_native_invocation();
    PerformanceCounters::observe_java_call_depth(3U);
    PerformanceCounters::observe_java_call_depth(7U);
    PerformanceCounters::record_exception_dispatch();
    PerformanceCounters::record_class_initialization();
    PerformanceCounters::record_instruction_budget_exit();
    PerformanceCounters::record_scheduler_quantum();
    PerformanceCounters::observe_execution_frame_slots(8U, 12U, false, false);
    PerformanceCounters::observe_execution_frame_slots(32U, 12U, true, false);
    PerformanceCounters::observe_execution_frame_slots(8U, 24U, false, true);
    PerformanceCounters::observe_execution_frame_slots(48U, 40U, true, true);

    PerformanceCounters::record_class_cache(true);
    PerformanceCounters::record_class_cache(false);
    PerformanceCounters::record_method_resolution(true, false);
    PerformanceCounters::record_method_resolution(false, false);
    PerformanceCounters::record_method_resolution(true, true);
    PerformanceCounters::record_method_resolution(false, true);
    PerformanceCounters::record_field_resolution(true);
    PerformanceCounters::record_field_resolution(false);
    PerformanceCounters::record_assignability_cache(true);
    PerformanceCounters::record_assignability_cache(false);
    PerformanceCounters::record_native_registry_lookup();
    PerformanceCounters::record_metadata_key_construction();
    PerformanceCounters::record_virtual_inline_cache(true);
    PerformanceCounters::record_virtual_inline_cache(false);
    PerformanceCounters::record_direct_call_cache(true);
    PerformanceCounters::record_direct_call_cache(false);
    PerformanceCounters::record_operand_resolution(true);
    PerformanceCounters::record_operand_resolution(false);
    PerformanceCounters::record_operand_resolution_failure();
    PerformanceCounters::record_descriptor_cache(true);
    PerformanceCounters::record_descriptor_cache(false);
    PerformanceCounters::record_decoded_method(17U, 9U, 3U);
    PerformanceCounters::record_decoded_opcode_dispatch();
    PerformanceCounters::record_decoded_operand_dispatch();

    PerformanceCounters::record_allocation(
        AllocationPayloadKind::object, 64U);
    PerformanceCounters::record_allocation(
        AllocationPayloadKind::array, 128U);
    PerformanceCounters::record_failed_allocation();
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::allocation);
    PerformanceCounters::record_vm_fast_heap_operation();
    PerformanceCounters::record_gc(1'250U, 8U, 13U, 5U, 512U);
    PerformanceCounters::observe_execution_root_publication(3U, false);
    PerformanceCounters::observe_execution_root_publication(7U, true);
    PerformanceCounters::record_verified_root_scan(true, false, 4U, 9U, 0U);
    PerformanceCounters::record_verified_root_scan(false, true, 2U, 5U, 3U);
    PerformanceCounters::record_verified_root_scan(false, false, 0U, 0U, 9U);
    PerformanceCounters::record_jit_runtime_operation(7U, false);
    PerformanceCounters::record_jit_runtime_operation(7U, true);
    PerformanceCounters::observe_jit_call_operands(3U, false);
    PerformanceCounters::observe_jit_call_operands(11U, true);
    PerformanceCounters::record_jit_root_stage();
    PerformanceCounters::record_jit_root_stage();
    PerformanceCounters::record_jit_root_stage_commit();
    PerformanceCounters::observe_jit_staged_root_materialization(5U);
    PerformanceCounters::observe_jit_staged_root_materialization(2U);
    PerformanceCounters::observe_jit_staged_reference_slots(9U, false);
    PerformanceCounters::observe_jit_staged_reference_slots(3U, true);

    PerformanceCounters::record_scheduler_state_transition();
    PerformanceCounters::record_scheduler_queue_erase_scan(9U);
    PerformanceCounters::record_scheduler_yield();
    PerformanceCounters::record_scheduler_sleep();
    PerformanceCounters::record_scheduler_event_wakeup();
    PerformanceCounters::record_scheduler_spurious_wakeup();

    const auto snapshot = PerformanceCounters::snapshot();
    require(snapshot.executed_bytecodes == 3U,
            "executed bytecode count");
    require(snapshot.opcode_counts[0x60U] == 2U,
            "opcode histogram count");
    require(snapshot.opcode_counts[0xB6U] == 1U,
            "invoke opcode histogram count");
    require(snapshot.method_invocations == 1U,
            "method invocation count");
    require(snapshot.native_invocations == 1U,
            "native invocation count");
    require(snapshot.maximum_java_call_depth == 7U,
            "maximum Java call depth");
    require(snapshot.oversized_execution_frames == 3U &&
                snapshot.local_slot_storage_fallbacks == 2U &&
                snapshot.operand_slot_storage_fallbacks == 2U &&
                snapshot.maximum_frame_local_slots == 48U &&
                snapshot.maximum_frame_operand_slots == 40U,
            "execution slot-storage fallback counters");
    require(snapshot.execution_root_publications == 2U &&
                snapshot.execution_root_copy_publications == 1U &&
                snapshot.execution_root_exchange_publications == 1U &&
                snapshot.execution_roots_published == 10U &&
                snapshot.maximum_execution_roots_per_publication == 7U,
            "execution root publication counters");
    require(snapshot.verified_root_map_hits == 1U &&
                snapshot.verified_root_map_partial_hits == 1U &&
                snapshot.verified_root_map_fallbacks == 1U &&
                snapshot.verified_root_slots_visited == 6U &&
                snapshot.verified_root_slots_avoided == 14U &&
                snapshot.fallback_root_slots_scanned == 12U,
            "verified execution root-map counters");
    require(snapshot.jit_runtime_operation_calls[7U] == 2U &&
                snapshot.jit_runtime_safepoint_calls[7U] == 1U,
            "JIT runtime operation histogram counters");
    require(snapshot.jit_call_operand_decodes == 2U &&
                snapshot.jit_call_operand_overflows == 1U &&
                snapshot.maximum_jit_call_operand_values == 11U,
            "JIT call operand storage counters");
    require(snapshot.jit_root_stages == 2U &&
                snapshot.jit_root_stage_commits == 1U,
            "JIT deferred root publication counters");
    require(snapshot.jit_staged_root_materializations == 2U &&
                snapshot.jit_staged_roots_materialized == 7U,
            "JIT staged root materialization counters");
    require(snapshot.jit_staged_reference_slots_deferred == 9U &&
                snapshot.jit_staged_reference_slots_scanned == 3U,
            "JIT staged reference-slot counters");
    require(snapshot.class_cache_hits == 1U &&
                snapshot.class_cache_misses == 1U,
            "class cache hit and miss counts");
    require(snapshot.metadata_key_constructions == 1U,
            "metadata key construction count");
    require(snapshot.virtual_inline_cache_hits == 1U &&
                snapshot.virtual_inline_cache_misses == 1U,
            "virtual inline cache hit and miss counts");
    require(snapshot.direct_call_cache_hits == 1U &&
                snapshot.direct_call_cache_misses == 1U,
            "direct call cache hit and miss counts");
    require(snapshot.operand_resolution_hits == 1U &&
                snapshot.operand_resolution_misses == 1U &&
                snapshot.operand_resolution_failures == 1U,
            "operand resolution hit miss and failure counts");
    require(snapshot.descriptor_cache_hits == 1U &&
                snapshot.descriptor_cache_misses == 1U,
            "descriptor cache hit and miss counts");
    require(snapshot.decoded_methods == 1U &&
                snapshot.decoded_instructions == 17U &&
                snapshot.decoded_operands == 9U &&
                snapshot.decoded_switch_entries == 3U &&
                snapshot.decoded_opcode_dispatches == 1U &&
                snapshot.decoded_operand_dispatches == 1U,
            "decoded publication and dispatch counters");
    require(snapshot.allocations_by_kind[
                static_cast<phoneme::usize>(AllocationPayloadKind::object)] == 1U,
            "object allocation count");
    require(snapshot.allocated_bytes_by_kind[
                static_cast<phoneme::usize>(AllocationPayloadKind::array)] == 128U,
            "array allocation byte count");
    require(snapshot.public_locked_heap_operations == 1U &&
                snapshot.locked_heap_operations_by_kind[
                    static_cast<phoneme::usize>(
                        LockedHeapOperationKind::allocation)] == 1U,
            "locked heap allocation counters");
    require(snapshot.gc_count == 1U &&
                snapshot.gc_total_nanoseconds == 1'250U &&
                snapshot.gc_max_pause_nanoseconds == 1'250U,
            "GC timing counters");
    require(snapshot.scheduler_queue_erase_scans == 9U,
            "scheduler queue scan count");

    PerformanceCounters::reset();
    require(PerformanceCounters::snapshot().executed_bytecodes == 0U,
            "counter reset");

    phoneme::vm::NativeMethodRegistry natives;
    const auto initial_generation = natives.generation();
    require(natives.register_method(
                "test/Native",
                "call",
                "()V",
                [](phoneme::vm::Machine&,
                   std::span<const phoneme::vm::Value>)
                    -> phoneme::Result<std::optional<phoneme::vm::Value>> {
                    return std::optional<phoneme::vm::Value>{};
                })
                .has_value(),
            "register native method with runtime ID");
    const auto registered_generation = natives.generation();
    require(registered_generation != initial_generation,
            "native registration advances registry generation");
    const auto native_binding = natives.resolve_binding(
        "test/Native", "call", "()V");
    require(native_binding.id.valid() &&
                native_binding.generation == registered_generation,
            "resolve registered native method with atomic generation");
    natives.clear();
    require(natives.generation() != registered_generation,
            "native clear advances registry generation");
    require(!natives.resolve("test/Native", "call", "()V").valid(),
            "native clear invalidates prior resolution");

    std::cout << "Performance counter tests passed\n";
    return 0;
}
