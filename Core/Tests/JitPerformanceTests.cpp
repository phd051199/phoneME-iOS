#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>

#include "phoneme/vm/ClassRepository.hpp"
#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/PerformanceCounters.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

struct TimedRun final {
    std::uint64_t elapsed_nanoseconds {0};
    std::int64_t checksum {0};
};

TimedRun run_sum_loop(phoneme::vm::Machine& machine,
                      int repetitions,
                      phoneme::i32 limit) {
    const phoneme::vm::Value argument = phoneme::vm::Value::from_int(limit);
    std::int64_t checksum = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < repetitions; ++iteration) {
        auto result = machine.invoke_static(
            "corefixture/JitOps",
            "sumLoop",
            "(I)I",
            std::span<const phoneme::vm::Value>(&argument, 1U));
        require(result.has_value() && result->completed_normally() &&
                    result->return_value.has_value(),
                "execute sumLoop benchmark invocation");
        auto value = result->return_value->as_int();
        require(value.has_value(), "sumLoop benchmark result is int");
        checksum += *value;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    return TimedRun {
        .elapsed_nanoseconds = static_cast<std::uint64_t>(
            elapsed > 0 ? elapsed : 0),
        .checksum = checksum,
    };
}

TimedRun run_array_loop(phoneme::vm::Machine& machine,
                        phoneme::vm::ObjectRef array,
                        int repetitions) {
    const phoneme::vm::Value argument =
        phoneme::vm::Value::from_reference(array);
    std::int64_t checksum = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < repetitions; ++iteration) {
        auto result = machine.invoke_static(
            "corefixture/JitOps",
            "sumIntArray",
            "([I)I",
            std::span<const phoneme::vm::Value>(&argument, 1U));
        require(result.has_value() && result->completed_normally() &&
                    result->return_value.has_value(),
                "execute sumIntArray benchmark invocation");
        auto value = result->return_value->as_int();
        require(value.has_value(), "sumIntArray benchmark result is int");
        checksum += *value;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    return TimedRun {
        .elapsed_nanoseconds = static_cast<std::uint64_t>(
            elapsed > 0 ? elapsed : 0),
        .checksum = checksum,
    };
}

TimedRun run_array_store_loop(phoneme::vm::Machine& machine,
                              phoneme::vm::ObjectRef array,
                              int repetitions) {
    const phoneme::vm::Value argument =
        phoneme::vm::Value::from_reference(array);
    std::int64_t checksum = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < repetitions; ++iteration) {
        auto result = machine.invoke_static(
            "corefixture/JitOps",
            "fillIntArray",
            "([I)I",
            std::span<const phoneme::vm::Value>(&argument, 1U));
        require(result.has_value() && result->completed_normally() &&
                    result->return_value.has_value(),
                "execute fillIntArray benchmark invocation");
        auto value = result->return_value->as_int();
        require(value.has_value(), "fillIntArray benchmark result is int");
        checksum += *value;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    return TimedRun {
        .elapsed_nanoseconds = static_cast<std::uint64_t>(
            elapsed > 0 ? elapsed : 0),
        .checksum = checksum,
    };
}

} // namespace

int main(int argc, char** argv) {
    require(argc == 3,
            "usage: JitPerformanceTests <fixture.jar> <output.json>");
    (void)::setenv("PHONEME_JIT_HOT_THRESHOLD", "1", 1);
    (void)::setenv("PHONEME_JIT_BACKGROUND_COMPILE", "0", 1);

    constexpr phoneme::i32 kLimit = 2'000;
    constexpr int kWarmupIterations = 8;
    constexpr int kTimedIterations = 250;
    constexpr phoneme::usize kArrayLength = 512U;
    constexpr int kArrayTimedIterations = 100;
    constexpr phoneme::i64 kExpected =
        static_cast<phoneme::i64>(kLimit) *
        static_cast<phoneme::i64>(kLimit + 1) / 2;

    phoneme::vm::ClassRepository interpreter_classes;
    require(interpreter_classes.add_archive(argv[1]).has_value(),
            "load benchmark fixture for interpreter");
    phoneme::vm::Machine interpreter(interpreter_classes);
    interpreter.configure_jit(false);

    phoneme::vm::ClassRepository jit_classes;
    require(jit_classes.add_archive(argv[1]).has_value(),
            "load benchmark fixture for JIT");
    phoneme::vm::Machine jit(jit_classes);
    jit.configure_jit(true);
    jit.configure_jit_startup(false);

    const phoneme::vm::Value cold_argument =
        phoneme::vm::Value::from_int(kLimit);
    const auto cold_started = std::chrono::steady_clock::now();
    auto cold_result = jit.invoke_static(
        "corefixture/JitOps",
        "sumLoop",
        "(I)I",
        std::span<const phoneme::vm::Value>(&cold_argument, 1U));
    const auto cold_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - cold_started).count();
    require(cold_result.has_value() && cold_result->completed_normally() &&
                cold_result->return_value.has_value() &&
                cold_result->return_value->as_int().value_or(0) == kExpected,
            "cold JIT benchmark result is correct");

    const TimedRun interpreter_warmup = run_sum_loop(
        interpreter, kWarmupIterations, kLimit);
    const TimedRun jit_warmup = run_sum_loop(jit, kWarmupIterations, kLimit);
    require(interpreter_warmup.checksum == jit_warmup.checksum,
            "JIT and interpreter warmup checksums match");

    const TimedRun interpreter_run = run_sum_loop(
        interpreter, kTimedIterations, kLimit);
    const TimedRun jit_run = run_sum_loop(jit, kTimedIterations, kLimit);
    const phoneme::i64 expected_checksum =
        kExpected * static_cast<phoneme::i64>(kTimedIterations);
    require(interpreter_run.checksum == expected_checksum &&
                jit_run.checksum == expected_checksum,
            "JIT and interpreter benchmark checksums match");

    auto interpreter_array = interpreter.heap().allocate_array(
        "[I", kArrayLength, phoneme::vm::Value::from_int(1));
    auto jit_array = jit.heap().allocate_array(
        "[I", kArrayLength, phoneme::vm::Value::from_int(1));
    require(interpreter_array.has_value() && jit_array.has_value(),
            "allocate A/B array benchmark inputs");
    const TimedRun interpreter_array_warmup = run_array_loop(
        interpreter, *interpreter_array, kWarmupIterations);
    const TimedRun jit_array_warmup = run_array_loop(
        jit, *jit_array, kWarmupIterations);
    require(interpreter_array_warmup.checksum == jit_array_warmup.checksum,
            "JIT and interpreter array warmup checksums match");

    phoneme::vm::PerformanceCounters::reset();
    const TimedRun interpreter_array_run = run_array_loop(
        interpreter, *interpreter_array, kArrayTimedIterations);
    const auto interpreter_array_counters =
        phoneme::vm::PerformanceCounters::snapshot();
    phoneme::vm::PerformanceCounters::reset();
    const TimedRun jit_array_run = run_array_loop(
        jit, *jit_array, kArrayTimedIterations);
    const auto jit_array_counters =
        phoneme::vm::PerformanceCounters::snapshot();
    const phoneme::i64 expected_array_checksum =
        static_cast<phoneme::i64>(kArrayLength) * kArrayTimedIterations;
    require(interpreter_array_run.checksum == expected_array_checksum &&
                jit_array_run.checksum == expected_array_checksum,
            "JIT and interpreter array benchmark checksums match");
    std::cout << "array_locked_heap_ops interpreter="
              << interpreter_array_counters.public_locked_heap_operations
              << " jit=" << jit_array_counters.public_locked_heap_operations
              << '\n';

    (void)run_array_store_loop(interpreter, *interpreter_array, kWarmupIterations);
    (void)run_array_store_loop(jit, *jit_array, kWarmupIterations);
    phoneme::vm::PerformanceCounters::reset();
    const TimedRun interpreter_store_run = run_array_store_loop(
        interpreter, *interpreter_array, kArrayTimedIterations);
    const auto interpreter_store_counters =
        phoneme::vm::PerformanceCounters::snapshot();
    phoneme::vm::PerformanceCounters::reset();
    const TimedRun jit_store_run = run_array_store_loop(
        jit, *jit_array, kArrayTimedIterations);
    const auto jit_store_counters =
        phoneme::vm::PerformanceCounters::snapshot();
    const phoneme::i64 expected_store_checksum =
        static_cast<phoneme::i64>(kArrayLength) * kArrayTimedIterations;
    require(interpreter_store_run.checksum == expected_store_checksum &&
                jit_store_run.checksum == expected_store_checksum,
            "JIT and interpreter array-store checksums match");
    if (phoneme::vm::PerformanceCounters::enabled()) {
        // The interpreter's own stores moved to lock-free vm_fast accessors,
        // so its locked-op count is 0 and a jit<interp comparison can never
        // hold. The absolute bound is what actually proves the store lease is
        // keeping hot element stores off the locked heap path.
        require(jit_store_counters.public_locked_heap_operations <=
                    static_cast<phoneme::u64>(kArrayTimedIterations * 2 + 8),
                "JIT primitive store lease keeps hot element stores lock-free");
    }
    std::cout << "store_locked_heap_ops interpreter="
              << interpreter_store_counters.public_locked_heap_operations
              << " jit=" << jit_store_counters.public_locked_heap_operations
              << '\n';

    const auto statistics = jit.jit_statistics();
    require(statistics.array_runtime_calls_eliminated > 0U &&
                statistics.array_bounds_checks_eliminated > 0U,
            "JIT benchmark exercises proven array-loop lease optimization");
    if (phoneme::vm::PerformanceCounters::enabled()) {
        // Interpreter array loads also use lock-free accessors now; the lease
        // bound proves the JIT keeps hot loops off the locked heap path.
        require(jit_array_counters.public_locked_heap_operations <=
                    static_cast<phoneme::u64>(kArrayTimedIterations * 2 + 8),
                "JIT array loop lease reduces public heap lock operations");
    }
    require(statistics.compiled_methods > 0U &&
                statistics.executed_methods > 0U,
            "JIT benchmark executes compiled native code");
    require(statistics.register_cached_methods > 0U &&
                statistics.stack_cached_methods > 0U,
            "JIT benchmark exercises ARM64 register caches");

    // An effectively unbounded scheduler budget must not directly admit a
    // loop method into the ARM64 JIT.  The scheduler remains interpreted while
    // finite helpers below it can still JIT.  This is the safety regression
    // test for iOS game loops: the sumLoop body is already hot/compiled from the
    // benchmark above, so an unbounded invocation must complete without
    // executing or deoptimizing that compiled entry.
    {
        constexpr phoneme::i32 kGuardedLimit = 1'000;
        const phoneme::i32 kGuardedExpected =
            (kGuardedLimit * (kGuardedLimit + 1)) / 2;
        const phoneme::vm::Value guarded_argument =
            phoneme::vm::Value::from_int(kGuardedLimit);
        const auto before = jit.jit_statistics();
        auto guarded_result = jit.invoke_static(
            "corefixture/JitOps",
            "sumLoop",
            "(I)I",
            std::span<const phoneme::vm::Value>(&guarded_argument, 1U),
            std::numeric_limits<phoneme::u64>::max());
        const auto after = jit.jit_statistics();
        require(guarded_result.has_value() &&
                    guarded_result->completed_normally() &&
                    guarded_result->return_value.has_value() &&
                    guarded_result->return_value->as_int().value_or(0) ==
                        kGuardedExpected,
                "unbounded loop invocation completes in the interpreter");
        require(after.executed_methods == before.executed_methods &&
                    after.deoptimized_executions == before.deoptimized_executions,
                "unbounded loop invocation does not enter compiled code");
    }

    const double speedup = jit_run.elapsed_nanoseconds == 0U
        ? 0.0
        : static_cast<double>(interpreter_run.elapsed_nanoseconds) /
              static_cast<double>(jit_run.elapsed_nanoseconds);
    const double array_speedup = jit_array_run.elapsed_nanoseconds == 0U
        ? 0.0
        : static_cast<double>(interpreter_array_run.elapsed_nanoseconds) /
              static_cast<double>(jit_array_run.elapsed_nanoseconds);
    const double store_speedup = jit_store_run.elapsed_nanoseconds == 0U
        ? 0.0
        : static_cast<double>(interpreter_store_run.elapsed_nanoseconds) /
              static_cast<double>(jit_store_run.elapsed_nanoseconds);

    const std::filesystem::path output_path(argv[2]);
    std::error_code directory_error;
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(
            output_path.parent_path(), directory_error);
    }
    require(!directory_error, "create JIT benchmark output directory");
    std::ofstream output(output_path, std::ios::trunc);
    require(output.good(), "open JIT benchmark output");
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"benchmark\": \"jit_sum_loop_ab\",\n"
           << "  \"loop_limit\": " << kLimit << ",\n"
           << "  \"timed_iterations\": " << kTimedIterations << ",\n"
           << "  \"cold_jit_nanoseconds\": "
           << (cold_elapsed > 0 ? cold_elapsed : 0) << ",\n"
           << "  \"interpreter_nanoseconds\": "
           << interpreter_run.elapsed_nanoseconds << ",\n"
           << "  \"jit_nanoseconds\": " << jit_run.elapsed_nanoseconds << ",\n"
           << "  \"steady_state_speedup\": " << speedup << ",\n"
           << "  \"checksum\": " << jit_run.checksum << ",\n"
           << "  \"array_length\": " << kArrayLength << ",\n"
           << "  \"array_timed_iterations\": "
           << kArrayTimedIterations << ",\n"
           << "  \"array_interpreter_nanoseconds\": "
           << interpreter_array_run.elapsed_nanoseconds << ",\n"
           << "  \"array_jit_nanoseconds\": "
           << jit_array_run.elapsed_nanoseconds << ",\n"
           << "  \"array_steady_state_speedup\": "
           << array_speedup << ",\n"
           << "  \"array_interpreter_locked_heap_operations\": "
           << interpreter_array_counters.public_locked_heap_operations << ",\n"
           << "  \"array_jit_locked_heap_operations\": "
           << jit_array_counters.public_locked_heap_operations << ",\n"
           << "  \"store_interpreter_nanoseconds\": "
           << interpreter_store_run.elapsed_nanoseconds << ",\n"
           << "  \"store_jit_nanoseconds\": "
           << jit_store_run.elapsed_nanoseconds << ",\n"
           << "  \"store_steady_state_speedup\": "
           << store_speedup << ",\n"
           << "  \"store_interpreter_locked_heap_operations\": "
           << interpreter_store_counters.public_locked_heap_operations << ",\n"
           << "  \"store_jit_locked_heap_operations\": "
           << jit_store_counters.public_locked_heap_operations << ",\n"
           << "  \"compiled_methods\": " << statistics.compiled_methods << ",\n"
           << "  \"quick_compiled_methods\": "
           << statistics.quick_compiled_methods << ",\n"
           << "  \"executed_methods\": " << statistics.executed_methods << ",\n"
           << "  \"compile_time_nanoseconds\": "
           << statistics.compile_time_nanoseconds << ",\n"
           << "  \"execution_time_nanoseconds\": "
           << statistics.execution_time_nanoseconds << ",\n"
           << "  \"code_cache_bytes\": " << statistics.code_cache_bytes << ",\n"
           << "  \"code_cache_limit_bytes\": "
           << statistics.code_cache_limit_bytes << ",\n"
           << "  \"background_compile_workers\": "
           << statistics.background_compile_worker_count << ",\n"
           << "  \"background_compile_queue_peak\": "
           << statistics.background_compile_queue_peak << ",\n"
           << "  \"background_compile_time_nanoseconds\": "
           << statistics.background_compile_time_nanoseconds << ",\n"
           << "  \"background_render_cooldown_waits\": "
           << statistics.background_compile_render_cooldown_waits << ",\n"
           << "  \"background_render_cooldown_nanoseconds\": "
           << statistics.background_compile_render_cooldown_nanoseconds << ",\n"
           << "  \"foreground_compile_deferred\": "
           << statistics.foreground_compile_deferred << ",\n"
           << "  \"executable_mapped_bytes\": "
           << statistics.executable_mapped_bytes << ",\n"
           << "  \"executable_slab_count\": "
           << statistics.executable_slab_count << ",\n"
           << "  \"register_cached_methods\": "
           << statistics.register_cached_methods << ",\n"
           << "  \"register_cached_loads\": "
           << statistics.register_cached_loads << ",\n"
           << "  \"stack_cached_methods\": "
           << statistics.stack_cached_methods << ",\n"
           << "  \"stack_cached_pops\": "
           << statistics.stack_cached_pops << ",\n"
           << "  \"stack_cached_stores_elided\": "
           << statistics.stack_cached_stores_elided << "\n"
           << "}\n";
    require(output.good(), "write JIT benchmark output");

    std::cout << "JIT performance benchmark passed speedup="
              << speedup << "x array_speedup=" << array_speedup
              << "x store_speedup=" << store_speedup << "x\n";
    return 0;
}
