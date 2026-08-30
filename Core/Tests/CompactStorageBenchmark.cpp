#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "phoneme/vm/Heap.hpp"
#include "phoneme/vm/SlotStorage.hpp"
#include "phoneme/vm/Value.hpp"

namespace {

using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

template <typename Fn>
std::uint64_t measure_nanoseconds(Fn&& fn) {
    const auto started = Clock::now();
    fn();
    const auto finished = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        finished - started).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0U;
}

} // namespace

int main(int argc, char** argv) {
    using phoneme::i32;
    using phoneme::u32;
    using phoneme::u64;
    using phoneme::usize;
    using phoneme::vm::Heap;
    using phoneme::vm::HeapLimits;
    using phoneme::vm::LocalVariables;
    using phoneme::vm::ObjectRef;
    using phoneme::vm::OperandStack;
    using phoneme::vm::Value;
    using phoneme::vm::ValueKind;

    require(argc == 2, "usage: CompactStorageBenchmark <output.json>");

    constexpr usize kSamples = 4'096U;
    constexpr usize kIterations = 2'000'000U;
    std::vector<i32> samples(kSamples);
    u32 state = 0x12345678U;
    for (usize index = 0U; index < samples.size(); ++index) {
        state = state * 1'664'525U + 1'013'904'223U;
        samples[index] = static_cast<i32>(state);
    }

    u64 checksum = 0U;
    std::array<Value, 16U> legacy_locals {};
    const u64 legacy_local_ns = measure_nanoseconds([&] {
        for (usize iteration = 0U; iteration < kIterations; ++iteration) {
            const i32 value = samples[iteration & (kSamples - 1U)];
            const usize slot = iteration & 15U;
            legacy_locals[slot] = Value::from_int(value);
            auto loaded = legacy_locals[slot].as_int();
            require(loaded.has_value(), "legacy local int read");
            checksum += static_cast<u64>(static_cast<u32>(*loaded));
        }
    });

    LocalVariables compact_locals(16U);
    const u64 compact_local_ns = measure_nanoseconds([&] {
        for (usize iteration = 0U; iteration < kIterations; ++iteration) {
            const i32 value = samples[iteration & (kSamples - 1U)];
            const usize slot = iteration & 15U;
            require(compact_locals.set_int(slot, value).has_value(),
                    "compact local int write");
            auto loaded = compact_locals.get_int(slot);
            require(loaded.has_value(), "compact local int read");
            checksum += static_cast<u64>(static_cast<u32>(*loaded));
        }
    });

    std::array<Value, 16U> legacy_stack {};
    usize legacy_depth = 0U;
    const u64 legacy_stack_ns = measure_nanoseconds([&] {
        for (usize iteration = 0U; iteration < kIterations; ++iteration) {
            const i32 value = samples[iteration & (kSamples - 1U)];
            legacy_stack[legacy_depth++] = Value::from_int(value);
            auto loaded = legacy_stack[--legacy_depth].as_int();
            require(loaded.has_value(), "legacy stack int pop");
            checksum += static_cast<u64>(static_cast<u32>(*loaded));
        }
    });

    OperandStack compact_stack(16U);
    const u64 compact_stack_ns = measure_nanoseconds([&] {
        for (usize iteration = 0U; iteration < kIterations; ++iteration) {
            const i32 value = samples[iteration & (kSamples - 1U)];
            require(compact_stack.push_int(value).has_value(),
                    "compact stack int push");
            auto loaded = compact_stack.pop_int();
            require(loaded.has_value(), "compact stack int pop");
            checksum += static_cast<u64>(static_cast<u32>(*loaded));
        }
    });

    std::vector<ObjectRef> references(kSamples);
    std::vector<u32> narrow_slots(kSamples);
    for (usize index = 0U; index < kSamples; ++index) {
        const u32 slot = static_cast<u32>(index + 1U);
        const u32 generation = static_cast<u32>((index * 17U) + 3U);
        references[index] = ObjectRef::make(slot, generation);
        narrow_slots[index] = slot;
    }
    const u64 object_ref_ns = measure_nanoseconds([&] {
        for (usize iteration = 0U; iteration < kIterations; ++iteration) {
            const ObjectRef reference = references[iteration & (kSamples - 1U)];
            checksum += static_cast<u64>(reference.slot());
            checksum += static_cast<u64>(reference.generation());
        }
    });
    const u64 narrow_ref_ns = measure_nanoseconds([&] {
        for (usize iteration = 0U; iteration < kIterations; ++iteration) {
            checksum += static_cast<u64>(
                narrow_slots[iteration & (kSamples - 1U)]);
        }
    });

    Heap heap(HeapLimits {
        .maximum_objects = 64U,
        .maximum_bytes = 8U * 1024U * 1024U,
    });
    const usize start_bytes = heap.stats().estimated_bytes;
    auto empty_object = heap.allocate_object("B", 0U);
    require(empty_object.has_value(), "allocate zero-field object");
    const usize after_empty_object = heap.stats().estimated_bytes;
    const usize empty_object_bytes = after_empty_object - start_bytes;
    // estimate_object_bytes adds class-name bytes including the terminator.
    const usize object_header_bytes = empty_object_bytes - 2U;

    std::array<Value, 8U> fields {
        Value::from_int(1), Value::from_int(2), Value::from_int(3),
        Value::from_int(4), Value::from_reference(ObjectRef::make(1U, 1U)),
        Value::from_int(6), Value::from_int(7), Value::from_int(8),
    };
    const usize before_fields = heap.stats().estimated_bytes;
    auto object8 = heap.allocate_object("C", fields);
    require(object8.has_value(), "allocate eight-field object");
    const usize object8_bytes =
        heap.stats().estimated_bytes - before_fields;

    const usize before_empty_array = heap.stats().estimated_bytes;
    auto empty_array = heap.allocate_array("[I", 0U, Value::from_int(0));
    require(empty_array.has_value(), "allocate empty int array");
    const usize empty_array_bytes =
        heap.stats().estimated_bytes - before_empty_array;
    const usize array_header_bytes = empty_array_bytes - 3U;

    const usize before_array64 = heap.stats().estimated_bytes;
    auto array64 = heap.allocate_array("[I", 64U, Value::from_int(0));
    require(array64.has_value(), "allocate 64-element int array");
    const usize array64_bytes = heap.stats().estimated_bytes - before_array64;

    const usize legacy_object8_estimate =
        object_header_bytes + 2U + 8U * sizeof(Value);
    const usize legacy_array64_estimate =
        array_header_bytes + 3U + 64U * sizeof(Value);

    const std::filesystem::path output_path(argv[1]);
    std::error_code directory_error;
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(
            output_path.parent_path(), directory_error);
    }
    require(!directory_error, "create compact benchmark output directory");
    std::ofstream output(output_path, std::ios::trunc);
    require(output.good(), "open compact benchmark output");

    const double object_ref_ratio = narrow_ref_ns == 0U
        ? 0.0
        : static_cast<double>(object_ref_ns) /
              static_cast<double>(narrow_ref_ns);
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"benchmark\": \"compact_storage\",\n"
           << "  \"iterations\": " << kIterations << ",\n"
           << "  \"checksum\": " << checksum << ",\n"
           << "  \"legacy_value_bytes\": " << sizeof(Value) << ",\n"
           << "  \"compact_slot_payload_bytes\": "
           << (sizeof(u64) + sizeof(ValueKind)) << ",\n"
           << "  \"operand_stack_object_bytes\": "
           << sizeof(OperandStack) << ",\n"
           << "  \"local_variables_object_bytes\": "
           << sizeof(LocalVariables) << ",\n"
           << "  \"legacy_operand_stack_reference_bytes\": 312,\n"
           << "  \"legacy_local_variables_reference_bytes\": 384,\n"
           << "  \"legacy_local_ns\": " << legacy_local_ns << ",\n"
           << "  \"compact_local_ns\": " << compact_local_ns << ",\n"
           << "  \"legacy_stack_ns\": " << legacy_stack_ns << ",\n"
           << "  \"compact_stack_ns\": " << compact_stack_ns << ",\n"
           << "  \"object_ref64_decode_ns\": " << object_ref_ns << ",\n"
           << "  \"slot32_read_ns\": " << narrow_ref_ns << ",\n"
           << "  \"object_ref64_to_slot32_ratio\": "
           << object_ref_ratio << ",\n"
           << "  \"object_ref64_bytes\": " << sizeof(ObjectRef) << ",\n"
           << "  \"slot32_bytes\": " << sizeof(u32) << ",\n"
           << "  \"object_header_bytes\": " << object_header_bytes << ",\n"
           << "  \"array_header_bytes\": " << array_header_bytes << ",\n"
           << "  \"object8_current_bytes\": " << object8_bytes << ",\n"
           << "  \"object8_legacy_value_estimate_bytes\": "
           << legacy_object8_estimate << ",\n"
           << "  \"int_array64_current_bytes\": " << array64_bytes << ",\n"
           << "  \"int_array64_legacy_value_estimate_bytes\": "
           << legacy_array64_estimate << "\n"
           << "}\n";
    require(output.good(), "write compact benchmark output");

    std::cout << "Compact storage benchmark passed\n";
    return 0;
}
