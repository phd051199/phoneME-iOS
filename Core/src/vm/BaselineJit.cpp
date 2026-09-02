#include "phoneme/vm/BaselineJit.hpp"
#include "phoneme/vm/PerformanceCounters.hpp"
#include "phoneme/vm/Verifier.hpp"
#include "phoneme/runtime/WorkCoordinator.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#endif

#if defined(__arm64e__) && __has_include(<ptrauth.h>)
#include <ptrauth.h>
#endif

#if defined(__aarch64__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace phoneme::vm {
namespace {

constexpr u32 kDefaultHotThreshold = 32U;
constexpr u32 kMaximumHotThreshold = 10'000U;
constexpr u32 kDefaultStartupHotThreshold = 384U;
constexpr u32 kOsrBackgroundPromotionPolls = 3U;
constexpr u32 kDefaultStartupLoopBytecodeThreshold = 96U;
constexpr u32 kMinimumStartupLoopBytecodeThreshold = 16U;
constexpr u32 kMaximumStartupLoopBytecodeThreshold = 4'096U;
constexpr u32 kDefaultStartupCompileLimit = 3U;
constexpr u32 kMaximumStartupCompileLimit = 256U;
constexpr u64 kDefaultStartupCompileBudgetNanoseconds = 3'000'000U;
constexpr u32 kStartupDeoptRetireThreshold = 2U;
constexpr u32 kNormalDeoptRetireThreshold = 16U;
constexpr u32 kRetryableCallRetireThreshold = 64U;
constexpr u64 kMaximumStartupCompileBudgetMilliseconds = 1'000U;
constexpr u64 kDefaultCodeCacheBytes = 16U * 1024U * 1024U;
constexpr u64 kMinimumCodeCacheBytes = 1U * 1024U * 1024U;
constexpr u64 kMaximumCodeCacheBytes = 256U * 1024U * 1024U;
constexpr u32 kUnavailableProbeInterval = 256U;
constexpr usize kMaximumBackgroundCompileQueue = 32U;
constexpr u32 kMaximumBackgroundCompileWorkers = 1U;
constexpr usize kMaximumSwitchCases = 512U;
constexpr usize kMaximumNativeInstructions = 256U * 1024U;
constexpr u64 kDefaultDeviceForegroundCompileIntervalMilliseconds = 200U;
constexpr u64 kMaximumDeviceForegroundCompileIntervalMilliseconds = 10'000U;

[[nodiscard]] u32 configured_hot_threshold() noexcept {
    const char* value = std::getenv("PHONEME_JIT_HOT_THRESHOLD");
    if (value == nullptr || *value == '\0') {
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
        // JarlyME's guest execution is interpreter-led. On physical iOS avoid
        // spending energy compiling helpers that are merely warm; only methods
        // that remain genuinely hot reach the conservative foreground JIT.
        return 128U;
#else
        return kDefaultHotThreshold;
#endif
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0UL) {
        return kDefaultHotThreshold;
    }
    return static_cast<u32>(std::min<unsigned long>(
        parsed, static_cast<unsigned long>(kMaximumHotThreshold)));
}

[[nodiscard]] u32 configured_startup_hot_threshold() noexcept {
    const char* value = std::getenv("PHONEME_JIT_STARTUP_HOT_THRESHOLD");
    if (value == nullptr || *value == '\0') {
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
        return 1'024U;
#else
        return kDefaultStartupHotThreshold;
#endif
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0UL) {
        return kDefaultStartupHotThreshold;
    }
    return static_cast<u32>(std::min<unsigned long>(
        parsed, static_cast<unsigned long>(kMaximumHotThreshold)));
}

[[nodiscard]] u32 configured_startup_loop_bytecode_threshold() noexcept {
    const char* value = std::getenv("PHONEME_JIT_STARTUP_LOOP_BYTES");
    if (value == nullptr || *value == '\0') {
        return kDefaultStartupLoopBytecodeThreshold;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return kDefaultStartupLoopBytecodeThreshold;
    }
    return static_cast<u32>(std::clamp<unsigned long>(
        parsed,
        static_cast<unsigned long>(kMinimumStartupLoopBytecodeThreshold),
        static_cast<unsigned long>(kMaximumStartupLoopBytecodeThreshold)));
}

[[nodiscard]] u32 configured_startup_compile_limit() noexcept {
    const char* value = std::getenv("PHONEME_JIT_STARTUP_COMPILE_LIMIT");
    if (value == nullptr || *value == '\0') return kDefaultStartupCompileLimit;
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return kDefaultStartupCompileLimit;
    }
    return static_cast<u32>(std::min<unsigned long>(
        parsed, static_cast<unsigned long>(kMaximumStartupCompileLimit)));
}

[[nodiscard]] bool configured_background_compile_enabled() noexcept {
    const char* value = std::getenv("PHONEME_JIT_BACKGROUND_COMPILE");
    if (value == nullptr || *value == '\0') return true;
    return std::string_view(value) != "0" &&
           std::string_view(value) != "false" &&
           std::string_view(value) != "off";
}

[[nodiscard]] bool conservative_device_jit_mode() noexcept {
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR && \
    defined(__aarch64__)
    static const bool conservative = []() noexcept {
        const char* value = std::getenv("PHONEME_IOS_JIT_AGGRESSIVE");
        if (value != nullptr && *value != '\0') {
            const std::string_view mode(value);
            if (mode != "0" && mode != "false" && mode != "off") {
                return false;
            }
        }
        return true;
    }();
    return conservative;
#else
    return false;
#endif
}

[[nodiscard]] bool quick_compile_tier_enabled() noexcept {
    if (const char* value = std::getenv("PHONEME_JIT_QUICK_TIER");
        value != nullptr && *value != '\0') {
        const std::string_view mode(value);
        return mode != "0" && mode != "false" && mode != "off";
    }
    return conservative_device_jit_mode();
}

[[nodiscard]] u32 adaptive_device_hot_threshold(u32 configured) noexcept {
    if (!conservative_device_jit_mode()) return configured;
    const auto& coordinator = runtime::shared_work_coordinator();
    // Once iOS reports real thermal pressure, stop spending package power on
    // compiling merely warm methods. Existing compiled code stays usable; we
    // only make admission progressively harder until the device cools. This
    // mirrors Harrier's interpreter-led steady state without globally
    // disabling phoneME's JIT when it is actually helping a heavy game.
    switch (coordinator.thermal_pressure()) {
    case runtime::ThermalPressure::critical:
        return kMaximumHotThreshold;
    case runtime::ThermalPressure::serious:
        return std::max<u32>(configured, 4'096U);
    case runtime::ThermalPressure::fair:
        configured = std::max<u32>(configured, 256U);
        break;
    case runtime::ThermalPressure::nominal:
        break;
    }
    switch (coordinator.frame_pressure()) {
    case runtime::FramePressure::overloaded:
        return std::min<u32>(configured, 32U);
    case runtime::FramePressure::high:
        return std::min<u32>(configured, 64U);
    case runtime::FramePressure::low:
    case runtime::FramePressure::normal:
        return configured;
    }
    return configured;
}

[[nodiscard]] u64 configured_device_foreground_compile_interval_nanoseconds()
    noexcept {
    const char* value = std::getenv("PHONEME_IOS_JIT_COMPILE_INTERVAL_MS");
    if (value == nullptr || *value == '\0') {
        return kDefaultDeviceForegroundCompileIntervalMilliseconds * 1'000'000U;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return kDefaultDeviceForegroundCompileIntervalMilliseconds * 1'000'000U;
    }
    const u64 milliseconds = std::min<u64>(
        static_cast<u64>(parsed),
        kMaximumDeviceForegroundCompileIntervalMilliseconds);
    return milliseconds * 1'000'000U;
}

[[nodiscard]] u32 configured_background_compile_worker_count() noexcept {
    const char* value = std::getenv("PHONEME_JIT_BACKGROUND_WORKERS");
    if (value != nullptr && *value != '\0') {
        char* end = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (errno == 0 && end != value && *end == '\0' && parsed != 0UL) {
            return static_cast<u32>(std::min<unsigned long>(
                parsed,
                static_cast<unsigned long>(kMaximumBackgroundCompileWorkers)));
        }
    }

    // Compilation is independent of Java execution, but allowing several JIT
    // workers to race rendering raises package power much more than it improves
    // steady-state game performance. A single sleeping worker is enough to
    // warm hot methods without oversubscribing the device.
    return 1U;
}

[[nodiscard]] bool background_compile_platform_supported() noexcept {
#if defined(__APPLE__) && defined(MAP_JIT) && defined(__aarch64__)
    return !conservative_device_jit_mode();
#else
    return false;
#endif
}

[[nodiscard]] u64 configured_startup_compile_budget_nanoseconds() noexcept {
    const char* value = std::getenv("PHONEME_JIT_STARTUP_COMPILE_BUDGET_MS");
    if (value == nullptr || *value == '\0') {
        return kDefaultStartupCompileBudgetNanoseconds;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return kDefaultStartupCompileBudgetNanoseconds;
    }
    const u64 milliseconds = std::min<u64>(
        static_cast<u64>(parsed), kMaximumStartupCompileBudgetMilliseconds);
    return milliseconds * 1'000'000U;
}

[[nodiscard]] u64 configured_code_cache_bytes() noexcept {
    const char* value = std::getenv("PHONEME_JIT_CODE_CACHE_MB");
    if (value == nullptr || *value == '\0') return kDefaultCodeCacheBytes;
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0UL) {
        return kDefaultCodeCacheBytes;
    }
    const u64 bytes = static_cast<u64>(parsed) * 1024U * 1024U;
    return std::clamp(bytes, kMinimumCodeCacheBytes, kMaximumCodeCacheBytes);
}

[[nodiscard]] u64 configured_render_compile_cooldown_nanoseconds() noexcept {
    constexpr u64 kDefaultCooldownMicroseconds = 1'500U;
    constexpr u64 kMaximumCooldownMicroseconds = 8'000U;
    const char* value = std::getenv("PHONEME_JIT_RENDER_COOLDOWN_US");
    if (value == nullptr || *value == '\0') {
        return kDefaultCooldownMicroseconds * 1'000U;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return kDefaultCooldownMicroseconds * 1'000U;
    }
    return std::min<u64>(static_cast<u64>(parsed),
                         kMaximumCooldownMicroseconds) * 1'000U;
}

[[nodiscard]] bool integer_like(JavaTypeKind kind) noexcept {
    switch (kind) {
    case JavaTypeKind::boolean:
    case JavaTypeKind::byte:
    case JavaTypeKind::character:
    case JavaTypeKind::short_integer:
    case JavaTypeKind::integer:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool scalar_like(JavaTypeKind kind) noexcept {
    return integer_like(kind) || kind == JavaTypeKind::long_integer ||
           kind == JavaTypeKind::float32 || kind == JavaTypeKind::float64 ||
           kind == JavaTypeKind::reference || kind == JavaTypeKind::array;
}

[[nodiscard]] u32 jit_slot_count(JavaTypeKind kind) noexcept {
    return kind == JavaTypeKind::long_integer || kind == JavaTypeKind::float64
        ? 2U
        : 1U;
}

#if defined(__aarch64__)

using JitFunction = u64 (*)(void* runtime_context,
                            JitRuntimeDispatch runtime_dispatch,
                            const u64* arguments,
                            u32 instruction_budget,
                            u64* result_bits);

constexpr u32 kRuntimeContextOffset = 0U;
constexpr u32 kRuntimeDispatchOffset = 8U;
constexpr u32 kResultPointerOffset = 16U;
constexpr u32 kReturnAddressOffset = 24U;
constexpr u32 kBudgetInitialOffset =
    static_cast<u32>(kJitRuntimeBudgetInitialByteOffset);
constexpr u32 kBudgetRemainingOffset =
    static_cast<u32>(kJitRuntimeBudgetRemainingByteOffset);
constexpr u32 kRuntimeResultOffset = 40U;
constexpr u32 kJitFrameHeaderBytes =
    static_cast<u32>(kJitRuntimeFrameHeaderBytes);

constexpr u32 kScratchMetadata = 8U;
constexpr u32 kScratchLeft = 9U;
constexpr u32 kScratchRight = 10U;
constexpr u32 kBudgetRemaining = 11U;
constexpr u32 kBudgetInitial = 12U;
constexpr u32 kScratchThird = 13U;
constexpr u32 kScratchFourth = 14U;
constexpr u32 kResultPointer = 15U;
constexpr std::array<u32, 4U> kCachedLocalRegisters {19U, 20U, 21U, 22U};
constexpr std::array<u32, 4U> kCachedStackRegisters {23U, 24U, 25U, 26U};
constexpr std::array<u32, 2U> kGvnRegisters {27U, 28U};
constexpr u32 kArrayLeaseRegister = 29U;
constexpr u32 kStackPointer = 31U;
constexpr u64 kDeoptMarker = 1ULL << 63U;
constexpr u64 kBudgetExhaustedMarker = 1ULL << 62U;
constexpr u32 kMaximumPackedBudget = static_cast<u32>(
    BaselineJit::maximum_instruction_budget());

enum class Arm64Condition : u32 {
    equal = 0x0U,
    not_equal = 0x1U,
    unsigned_lower = 0x3U,
    greater_equal = 0xAU,
    less_than = 0xBU,
    greater_than = 0xCU,
    less_equal = 0xDU,
};

class ExecutableArena;

struct ExecutableBlock final {
    ExecutableArena* arena {nullptr};
    void* memory {nullptr};
    // For arena-backed code this is the aligned slice size, not the slab's
    // page-rounded mapping size. Code-cache accounting therefore reflects
    // useful compiled bytes instead of charging one VM page per method.
    usize mapped_size {0};
    JitFunction function {nullptr};

    ExecutableBlock() = default;
    ExecutableBlock(void* initial_memory,
                    usize initial_size,
                    JitFunction initial_function,
                    ExecutableArena* initial_arena = nullptr) noexcept
        : arena(initial_arena),
          memory(initial_memory),
          mapped_size(initial_size),
          function(initial_function) {}

    ExecutableBlock(const ExecutableBlock&) = delete;
    ExecutableBlock& operator=(const ExecutableBlock&) = delete;

    ExecutableBlock(ExecutableBlock&& other) noexcept
        : arena(std::exchange(other.arena, nullptr)),
          memory(std::exchange(other.memory, nullptr)),
          mapped_size(std::exchange(other.mapped_size, 0U)),
          function(std::exchange(other.function, nullptr)) {}

    ExecutableBlock& operator=(ExecutableBlock&& other) noexcept {
        if (this == &other) return *this;
        reset();
        arena = std::exchange(other.arena, nullptr);
        memory = std::exchange(other.memory, nullptr);
        mapped_size = std::exchange(other.mapped_size, 0U);
        function = std::exchange(other.function, nullptr);
        return *this;
    }

    ~ExecutableBlock();
    void reset() noexcept;
};

class Arm64Emitter final {
public:
    [[nodiscard]] usize position() const noexcept { return code_.size(); }
    [[nodiscard]] const std::vector<u32>& code() const noexcept { return code_; }

    void emit(u32 instruction) { code_.push_back(instruction); }

    void sub_sp(u32 bytes) {
        emit(0xD10003FFU | ((bytes & 0xFFFU) << 10U));
    }

    void add_sp(u32 bytes) {
        emit(0x910003FFU | ((bytes & 0xFFFU) << 10U));
    }

    void add_imm_x(u32 destination, u32 source, u32 immediate) {
        emit(0x91000000U | ((immediate & 0xFFFU) << 10U) |
             ((source & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void load_w(u32 target, u32 base, u32 byte_offset) {
        emit(0xB9400000U |
             (((byte_offset / 4U) & 0xFFFU) << 10U) |
             ((base & 0x1FU) << 5U) |
             (target & 0x1FU));
    }

    void store_w(u32 source, u32 base, u32 byte_offset) {
        emit(0xB9000000U |
             (((byte_offset / 4U) & 0xFFFU) << 10U) |
             ((base & 0x1FU) << 5U) |
             (source & 0x1FU));
    }

    void load_x(u32 target, u32 base, u32 byte_offset) {
        emit(0xF9400000U |
             (((byte_offset / 8U) & 0xFFFU) << 10U) |
             ((base & 0x1FU) << 5U) |
             (target & 0x1FU));
    }

    void store_x(u32 source, u32 base, u32 byte_offset) {
        emit(0xF9000000U |
             (((byte_offset / 8U) & 0xFFFU) << 10U) |
             ((base & 0x1FU) << 5U) |
             (source & 0x1FU));
    }

    void move_imm32(u32 target, u32 value) {
        emit(0x52800000U | ((value & 0xFFFFU) << 5U) |
             (target & 0x1FU));
        if ((value >> 16U) != 0U) {
            emit(0x72A00000U | (((value >> 16U) & 0xFFFFU) << 5U) |
                 (target & 0x1FU));
        }
    }

    void move_imm64(u32 target, u64 value) {
        emit(0xD2800000U |
             ((static_cast<u32>(value) & 0xFFFFU) << 5U) |
             (target & 0x1FU));
        for (u32 shift = 16U; shift < 64U; shift += 16U) {
            const u32 half = static_cast<u32>((value >> shift) & 0xFFFFU);
            if (half == 0U) continue;
            emit(0xF2800000U | ((shift / 16U) << 21U) |
                 (half << 5U) | (target & 0x1FU));
        }
    }

    void move_w(u32 destination, u32 source) {
        emit(0x2A0003E0U | ((source & 0x1FU) << 16U) |
             (destination & 0x1FU));
    }

    void move_x(u32 destination, u32 source) {
        emit(0xAA0003E0U | ((source & 0x1FU) << 16U) |
             (destination & 0x1FU));
    }

    void add_w(u32 destination, u32 left, u32 right) {
        emit(0x0B000000U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void sub_w(u32 destination, u32 left, u32 right) {
        emit(0x4B000000U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void add_x(u32 destination, u32 left, u32 right) {
        emit(0x8B000000U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void sub_x(u32 destination, u32 left, u32 right) {
        emit(0xCB000000U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void sub_imm_w(u32 destination, u32 source, u32 immediate) {
        emit(0x51000000U | ((immediate & 0xFFFU) << 10U) |
             ((source & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void multiply_w(u32 destination, u32 left, u32 right) {
        emit(0x1B007C00U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void multiply_x(u32 destination, u32 left, u32 right) {
        emit(0x9B007C00U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void divide_signed_w(u32 destination, u32 left, u32 right) {
        emit(0x1AC00C00U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void divide_signed_x(u32 destination, u32 left, u32 right) {
        emit(0x9AC00C00U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void multiply_subtract_w(u32 destination,
                             u32 multiplier,
                             u32 multiplicand,
                             u32 minuend) {
        emit(0x1B008000U | ((multiplicand & 0x1FU) << 16U) |
             ((minuend & 0x1FU) << 10U) |
             ((multiplier & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void multiply_subtract_x(u32 destination,
                             u32 multiplier,
                             u32 multiplicand,
                             u32 minuend) {
        emit(0x9B008000U | ((multiplicand & 0x1FU) << 16U) |
             ((minuend & 0x1FU) << 10U) |
             ((multiplier & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void and_w(u32 destination, u32 left, u32 right) {
        emit(0x0A000000U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void or_w(u32 destination, u32 left, u32 right) {
        emit(0x2A000000U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void xor_w(u32 destination, u32 left, u32 right) {
        emit(0x4A000000U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void and_x(u32 destination, u32 left, u32 right) {
        emit(0x8A000000U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void or_x(u32 destination, u32 left, u32 right) {
        emit(0xAA000000U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void xor_x(u32 destination, u32 left, u32 right) {
        emit(0xCA000000U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void negate_w(u32 destination, u32 source) {
        emit(0x4B000000U | ((source & 0x1FU) << 16U) |
             (31U << 5U) | (destination & 0x1FU));
    }

    void negate_x(u32 destination, u32 source) {
        emit(0xCB000000U | ((source & 0x1FU) << 16U) |
             (31U << 5U) | (destination & 0x1FU));
    }

    void shift_left_w(u32 destination, u32 value, u32 shift) {
        emit(0x1AC02000U | ((shift & 0x1FU) << 16U) |
             ((value & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void shift_right_logical_w(u32 destination, u32 value, u32 shift) {
        emit(0x1AC02400U | ((shift & 0x1FU) << 16U) |
             ((value & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void shift_right_arithmetic_w(u32 destination, u32 value, u32 shift) {
        emit(0x1AC02800U | ((shift & 0x1FU) << 16U) |
             ((value & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void shift_left_x(u32 destination, u32 value, u32 shift) {
        emit(0x9AC02000U | ((shift & 0x1FU) << 16U) |
             ((value & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void shift_right_logical_x(u32 destination, u32 value, u32 shift) {
        emit(0x9AC02400U | ((shift & 0x1FU) << 16U) |
             ((value & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void shift_right_arithmetic_x(u32 destination, u32 value, u32 shift) {
        emit(0x9AC02800U | ((shift & 0x1FU) << 16U) |
             ((value & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void sign_extend_byte_w(u32 destination, u32 source) {
        emit(0x13001C00U | ((source & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void sign_extend_half_w(u32 destination, u32 source) {
        emit(0x13003C00U | ((source & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void zero_extend_half_w(u32 destination, u32 source) {
        emit(0x53003C00U | ((source & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void compare_zero_w(u32 value) {
        emit(0x7100001FU | ((value & 0x1FU) << 5U));
    }

    void compare_imm_w(u32 value, u32 immediate) {
        emit(0x7100001FU | ((immediate & 0xFFFU) << 10U) |
             ((value & 0x1FU) << 5U));
    }

    void compare_w(u32 left, u32 right) {
        emit(0x6B00001FU | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U));
    }

    void compare_zero_x(u32 value) {
        emit(0xF100001FU | ((value & 0x1FU) << 5U));
    }

    void compare_x(u32 left, u32 right) {
        emit(0xEB00001FU | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U));
    }

    void set_condition_w(u32 destination, Arm64Condition condition) {
        const u32 inverse = static_cast<u32>(condition) ^ 1U;
        emit(0x1A9F07E0U | ((inverse & 0xFU) << 12U) |
             (destination & 0x1FU));
    }

    void move_w_to_float(u32 destination, u32 source) {
        emit(0x1E270000U | ((source & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void move_float_to_w(u32 destination, u32 source) {
        emit(0x1E260000U | ((source & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void move_x_to_double(u32 destination, u32 source) {
        emit(0x9E670000U | ((source & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void move_double_to_x(u32 destination, u32 source) {
        emit(0x9E660000U | ((source & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void add_float(u32 destination, u32 left, u32 right) {
        emit(0x1E202800U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void sub_float(u32 destination, u32 left, u32 right) {
        emit(0x1E203800U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void multiply_float(u32 destination, u32 left, u32 right) {
        emit(0x1E200800U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void divide_float(u32 destination, u32 left, u32 right) {
        emit(0x1E201800U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void negate_float(u32 destination, u32 source) {
        emit(0x1E214000U | ((source & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void add_double(u32 destination, u32 left, u32 right) {
        emit(0x1E602800U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void sub_double(u32 destination, u32 left, u32 right) {
        emit(0x1E603800U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void multiply_double(u32 destination, u32 left, u32 right) {
        emit(0x1E600800U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void divide_double(u32 destination, u32 left, u32 right) {
        emit(0x1E601800U | ((right & 0x1FU) << 16U) |
             ((left & 0x1FU) << 5U) | (destination & 0x1FU));
    }

    void negate_double(u32 destination, u32 source) {
        emit(0x1E614000U | ((source & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    [[nodiscard]] usize emit_branch_placeholder() {
        const usize index = position();
        emit(0x14000000U);
        return index;
    }

    [[nodiscard]] usize emit_conditional_branch_placeholder(
        Arm64Condition condition) {
        const usize index = position();
        emit(0x54000000U | static_cast<u32>(condition));
        return index;
    }

    [[nodiscard]] usize emit_cbz_w_placeholder(u32 value) {
        const usize index = position();
        emit(0x34000000U | (value & 0x1FU));
        return index;
    }

    [[nodiscard]] usize emit_cbz_x_placeholder(u32 value) {
        const usize index = position();
        emit(0xB4000000U | (value & 0x1FU));
        return index;
    }

    [[nodiscard]] bool patch_branch(usize index, usize target) {
        if (index >= code_.size()) return false;
        const i64 delta = static_cast<i64>(target) - static_cast<i64>(index);
        if (delta < -(1LL << 25U) || delta >= (1LL << 25U)) return false;
        code_[index] = 0x14000000U |
            (static_cast<u32>(delta) & 0x03FF'FFFFU);
        return true;
    }

    [[nodiscard]] bool patch_conditional_branch(usize index,
                                                usize target,
                                                Arm64Condition condition) {
        if (index >= code_.size()) return false;
        const i64 delta = static_cast<i64>(target) - static_cast<i64>(index);
        if (delta < -(1LL << 18U) || delta >= (1LL << 18U)) return false;
        code_[index] = 0x54000000U |
            ((static_cast<u32>(delta) & 0x7FFFFU) << 5U) |
            static_cast<u32>(condition);
        return true;
    }

    [[nodiscard]] bool patch_cbz_w(usize index, usize target, u32 value) {
        if (index >= code_.size()) return false;
        const i64 delta = static_cast<i64>(target) - static_cast<i64>(index);
        if (delta < -(1LL << 18U) || delta >= (1LL << 18U)) return false;
        code_[index] = 0x34000000U |
            ((static_cast<u32>(delta) & 0x7FFFFU) << 5U) |
            (value & 0x1FU);
        return true;
    }

    [[nodiscard]] bool patch_cbz_x(usize index, usize target, u32 value) {
        if (index >= code_.size()) return false;
        const i64 delta = static_cast<i64>(target) - static_cast<i64>(index);
        if (delta < -(1LL << 18U) || delta >= (1LL << 18U)) return false;
        code_[index] = 0xB4000000U |
            ((static_cast<u32>(delta) & 0x7FFFFU) << 5U) |
            (value & 0x1FU);
        return true;
    }

    void zero_extend_w_to_x(u32 destination, u32 source) {
        emit(0xD3407C00U | ((source & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void sign_extend_w_to_x(u32 destination, u32 source) {
        emit(0x93407C00U | ((source & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void or_x_shifted(u32 destination,
                      u32 left,
                      u32 right,
                      u32 shift) {
        emit(0xAA000000U | ((right & 0x1FU) << 16U) |
             ((shift & 0x3FU) << 10U) |
             ((left & 0x1FU) << 5U) |
             (destination & 0x1FU));
    }

    void branch_link_register(u32 target) {
        emit(0xD63F0000U | ((target & 0x1FU) << 5U));
    }

    void move_deopt_marker_x0() { emit(0xD2F00000U); }
    void move_budget_deopt_marker_x0() { emit(0xD2F80000U); }
    void ret() { emit(0xD65F03C0U); }

private:
    std::vector<u32> code_;
};

struct JitReferenceMap final {
    std::vector<u32> frame_offsets;
};

struct RuntimeDispatchContext final {
    static constexpr usize kInlineRootCapacity = 64U;
    static constexpr usize kInlineDeoptSlotCapacity = 128U;

    JitRuntimeHooks hooks;
    const std::vector<JitReferenceMap>* reference_maps {nullptr};
    std::array<u64, kInlineRootCapacity> inline_roots {};
    std::vector<u64> overflow_roots;
    u64* root_storage {inline_roots.data()};
    usize root_capacity {inline_roots.size()};

    std::array<u64, kInlineDeoptSlotCapacity> inline_deopt_slots {};
    std::vector<u64> overflow_deopt_slots;
    u64* deopt_storage {inline_deopt_slots.data()};
    usize deopt_capacity {inline_deopt_slots.size()};
    usize deopt_slot_count {0U};
    u32 deopt_local_slots {0U};
    u32 deopt_stack_slots {0U};
    bool deopt_captured {false};

    void prepare_roots(usize capacity) {
        if (capacity <= inline_roots.size()) return;
        overflow_roots.resize(capacity);
        root_storage = overflow_roots.data();
        root_capacity = overflow_roots.size();
    }

    void prepare_deopt(usize capacity) {
        if (capacity <= inline_deopt_slots.size()) return;
        overflow_deopt_slots.resize(capacity);
        deopt_storage = overflow_deopt_slots.data();
        deopt_capacity = overflow_deopt_slots.size();
    }

    void capture_deopt_frame(const u64* frame_base) noexcept {
        deopt_captured = false;
        deopt_slot_count = 0U;
        if (frame_base == nullptr) return;
        const auto* frame_bytes = reinterpret_cast<const u8*>(frame_base);
        u32 local_slots = 0U;
        u32 stack_slots = 0U;
        std::memcpy(&local_slots,
                    frame_bytes + kJitRuntimeLocalSlotsByteOffset,
                    sizeof(local_slots));
        std::memcpy(&stack_slots,
                    frame_bytes + kJitRuntimeStackDepthByteOffset,
                    sizeof(stack_slots));
        const usize total = static_cast<usize>(local_slots) +
                            static_cast<usize>(stack_slots);
        if (total > deopt_capacity) return;
        const auto* slots = reinterpret_cast<const u64*>(
            frame_bytes + kJitRuntimeFrameHeaderBytes);
        std::copy_n(slots, total, deopt_storage);
        deopt_local_slots = local_slots;
        deopt_stack_slots = stack_slots;
        deopt_slot_count = total;
        deopt_captured = true;
    }
};

[[nodiscard]] u32 dispatch_runtime_trampoline(
    void* context,
    JitRuntimeOperation operation,
    u64 packed_operand,
    u64 first,
    u64 second,
    u64 third,
    const u64* frame_base,
    u64* result_bits) noexcept {
    auto* execution = static_cast<RuntimeDispatchContext*>(context);
    if (execution == nullptr || execution->hooks.dispatch == nullptr ||
        result_bits == nullptr) {
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
    }

    const u32 encoded_operand = static_cast<u32>(packed_operand);
    const bool no_safepoint =
        (encoded_operand & kJitRuntimeNoSafepointOperandFlag) != 0U;
    const u32 operand =
        encoded_operand & ~kJitRuntimeNoSafepointOperandFlag;
    const u32 safepoint_id = static_cast<u32>(packed_operand >> 32U);
    PerformanceCounters::record_jit_runtime_operation(
        static_cast<u32>(operation),
        !no_safepoint && execution->hooks.publish_roots != nullptr);
    if (!no_safepoint && execution->hooks.publish_roots != nullptr) {
        const bool defer_scheduler_publication =
            !conservative_device_jit_mode() &&
            (operation == JitRuntimeOperation::invoke_static ||
             operation == JitRuntimeOperation::invoke_virtual);
        const JitReferenceMap* reference_map = nullptr;
        if (frame_base != nullptr && execution->reference_maps != nullptr &&
            safepoint_id < execution->reference_maps->size()) {
            reference_map = &(*execution->reference_maps)[safepoint_id];
        }
        if (defer_scheduler_publication) {
            execution->hooks.publish_roots(
                execution->hooks.context,
                nullptr,
                0U,
                frame_base,
                reference_map != nullptr && !reference_map->frame_offsets.empty()
                    ? reference_map->frame_offsets.data()
                    : nullptr,
                reference_map != nullptr ? reference_map->frame_offsets.size()
                                         : 0U,
                true);
        } else {
            usize root_count = 0U;
            if (reference_map != nullptr) {
                for (const u32 byte_offset : reference_map->frame_offsets) {
                    const auto* slot = reinterpret_cast<const u64*>(
                        reinterpret_cast<const u8*>(frame_base) + byte_offset);
                    const u64 bits = *slot;
                    if (bits != 0U && root_count < execution->root_capacity) {
                        execution->root_storage[root_count++] = bits;
                    }
                }
            }
            execution->hooks.publish_roots(execution->hooks.context,
                                           execution->root_storage,
                                           root_count,
                                           nullptr,
                                           nullptr,
                                           0U,
                                           false);
        }
    }
    const u64 dispatch_operand = static_cast<u64>(
        operand | (no_safepoint ? kJitRuntimeNoSafepointOperandFlag : 0U));
    const u32 status = execution->hooks.dispatch(execution->hooks.context,
                                                 operation,
                                                 dispatch_operand,
                                                 first,
                                                 second,
                                                 third,
                                                 frame_base,
                                                 result_bits);
    const auto runtime_status = static_cast<JitRuntimeStatus>(status);
    if (runtime_status == JitRuntimeStatus::deoptimize ||
        runtime_status == JitRuntimeStatus::unsupported_call ||
        runtime_status == JitRuntimeStatus::retryable_call) {
        execution->capture_deopt_frame(frame_base);
    }
    return status;
}

struct JitOsrEntry final {
    JitFunction function {nullptr};
    u32 local_slots {0U};
    u32 stack_slots {0U};
};

struct OsrEntryOffset final {
    u32 bytecode_pc {0U};
    usize native_offset {0U};
    u32 local_slots {0U};
    u32 stack_slots {0U};
};

struct JitDeoptFrameMap final {
    u32 local_slots {0U};
    u32 stack_slots {0U};
    std::vector<VerifiedSlotKind> slot_kinds;
};

struct CompiledMethod final {
    ExecutableBlock block;
    std::unordered_map<u32, JitOsrEntry> osr_entries;
    std::unordered_map<u32, JitDeoptFrameMap> deopt_frame_maps;
    std::vector<JitReferenceMap> reference_maps;
    usize maximum_reference_roots {0};
    JavaTypeKind return_kind {JavaTypeKind::void_type};
    bool returns_value {false};
    bool requires_runtime_dispatch {false};
    bool quick_tier {false};
    bool optimized {false};
    bool contains_loop {false};
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
    u64 register_cached_loads {0};
    u64 register_cached_stores_elided {0};
    u64 stack_cached_pops {0};
    u64 stack_cached_stores_elided {0};
    bool register_cached {false};
    bool stack_cached {false};
};

[[nodiscard]] std::optional<JitDeoptState> decode_deopt_state(
    const CompiledMethod& compiled,
    const RuntimeDispatchContext& dispatch,
    u32 bytecode_pc) {
    if (!dispatch.deopt_captured) return std::nullopt;
    const auto found = compiled.deopt_frame_maps.find(bytecode_pc);
    if (found == compiled.deopt_frame_maps.end()) return std::nullopt;
    const JitDeoptFrameMap& map = found->second;
    const usize expected_slots = static_cast<usize>(map.local_slots) +
                                 static_cast<usize>(map.stack_slots);
    if (dispatch.deopt_local_slots != map.local_slots ||
        dispatch.deopt_stack_slots != map.stack_slots ||
        dispatch.deopt_slot_count != expected_slots ||
        map.slot_kinds.size() != expected_slots) {
        return std::nullopt;
    }

    JitDeoptState state;
    state.bytecode_pc = bytecode_pc;
    state.local_slots = map.local_slots;
    state.stack_slots = map.stack_slots;
    state.physical_slots.assign(
        dispatch.deopt_storage,
        dispatch.deopt_storage + expected_slots);
    return state;
}

enum class CompileDisposition : u8 {
    success,
    unsupported,
    executable_memory_unavailable,
};

struct CompileAttempt final {
    CompileDisposition disposition {CompileDisposition::unsupported};
    std::optional<CompiledMethod> method;
    JitRejectReason reject_reason {JitRejectReason::decode_failure};
    std::optional<u8> unsupported_opcode;
};

struct SwitchTarget final {
    i32 key {0};
    usize target_pc {0};
};

struct DecodedInstruction final {
    usize pc {0};
    usize next_pc {0};
    u8 opcode {0};
    u32 local_index {0};
    i32 immediate {0};
    u64 wide_immediate {0};
    JavaTypeKind value_kind {JavaTypeKind::void_type};
    u32 argument_slots {0U};
    bool has_receiver {false};
    std::optional<usize> branch_target;
    std::optional<usize> default_target;
    std::vector<SwitchTarget> switch_targets;
};

struct BranchPatch final {
    enum class Kind : u8 { unconditional, conditional };
    Kind kind {Kind::unconditional};
    usize native_index {0};
    usize target_pc {0};
    Arm64Condition condition {Arm64Condition::equal};
};

struct ArithmeticTrapSite final {
    usize zero_branch {0U};
    u32 bytecode_pc {0U};
    u32 stack_depth {0U};
    bool wide {false};
};

struct RuntimeExceptionSite final {
    usize failure_branch {0};
    u32 bytecode_pc {0};
    u32 safepoint_id {0};
    std::vector<u16> handler_pcs;
};

[[nodiscard]] usize system_page_size() noexcept {
    const long page_size = ::sysconf(_SC_PAGESIZE);
    return page_size > 0L ? static_cast<usize>(page_size) : 4096U;
}

#if defined(__APPLE__)
void set_thread_jit_write_protection(bool enabled) noexcept {
    using SupportedFunction = int (*)(void);
    using ProtectFunction = void (*)(int);
    static const auto supported = reinterpret_cast<SupportedFunction>(
        ::dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_supported_np"));
    static const auto protect = reinterpret_cast<ProtectFunction>(
        ::dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_np"));
    if (supported != nullptr && protect != nullptr && supported() != 0) {
        protect(enabled ? 1 : 0);
    }
}
#else
void set_thread_jit_write_protection(bool) noexcept {}
#endif

void invalidate_instruction_cache(void* memory, usize size) noexcept {
#if defined(__APPLE__)
    ::sys_icache_invalidate(memory, size);
#else
    auto* begin = static_cast<char*>(memory);
    __builtin___clear_cache(begin, begin + size);
#endif
}

[[nodiscard]] JitFunction function_at(void* memory) noexcept {
    JitFunction function = nullptr;
#if defined(__arm64e__) && __has_include(<ptrauth.h>)
    void* signed_pointer = ptrauth_sign_unauthenticated(
        memory,
        ptrauth_key_function_pointer,
        0);
    static_assert(sizeof(function) == sizeof(signed_pointer));
    std::memcpy(&function, &signed_pointer, sizeof(function));
#else
    static_assert(sizeof(function) == sizeof(memory));
    std::memcpy(&function, &memory, sizeof(function));
#endif
    return function;
}

class ExecutableArena final {
public:
    static constexpr usize kDefaultSlabBytes = 1U * 1024U * 1024U;
    static constexpr usize kCodeAlignment = 16U;

    explicit ExecutableArena(bool map_jit_only = false) noexcept
        : map_jit_only_(map_jit_only) {}
    ExecutableArena(const ExecutableArena&) = delete;
    ExecutableArena& operator=(const ExecutableArena&) = delete;

    ~ExecutableArena() { clear(); }

    [[nodiscard]] std::optional<ExecutableBlock> finalize(
        const std::vector<u32>& code) noexcept {
        if (code.empty() || code.size() > kMaximumNativeInstructions) {
            return std::nullopt;
        }
        const usize byte_count = code.size() * sizeof(u32);
        if (byte_count > std::numeric_limits<usize>::max() -
                             (kCodeAlignment - 1U)) {
            return std::nullopt;
        }
        const usize allocation_size =
            (byte_count + kCodeAlignment - 1U) & ~(kCodeAlignment - 1U);
        std::scoped_lock lock(mutex_);
        auto allocation = allocate(allocation_size);
        if (!allocation.has_value()) return std::nullopt;

        Slab& slab = *slabs_[allocation->slab_index];
        auto* destination = static_cast<u8*>(slab.memory) + allocation->offset;
        if (!begin_write(slab)) {
            slab.usable = false;
            return std::nullopt;
        }
        std::memcpy(destination, code.data(), byte_count);
        if (allocation_size > byte_count) {
            std::memset(destination + byte_count,
                        0,
                        allocation_size - byte_count);
        }
        invalidate_instruction_cache(destination, byte_count);
        if (!end_write(slab)) {
            slab.usable = false;
            return std::nullopt;
        }
        return ExecutableBlock(destination,
                               allocation_size,
                               function_at(destination),
                               this);
    }

    void release(void* memory, usize size) noexcept {
        if (memory == nullptr || size == 0U) return;
        std::scoped_lock lock(mutex_);
        const auto address = reinterpret_cast<uintptr_t>(memory);
        for (const auto& slab_pointer : slabs_) {
            Slab& slab = *slab_pointer;
            const auto begin = reinterpret_cast<uintptr_t>(slab.memory);
            if (address < begin || address >= begin + slab.mapped_size) {
                continue;
            }
            const usize offset = static_cast<usize>(address - begin);
            if (offset > slab.mapped_size ||
                size > slab.mapped_size - offset) {
                return;
            }
            slab.free_ranges.push_back(Range {offset, size});
            merge_free_ranges(slab);
            return;
        }
    }

    void clear() noexcept {
        std::scoped_lock lock(mutex_);
        for (auto& slab : slabs_) {
            if (slab->memory != nullptr && slab->mapped_size != 0U) {
                (void)::munmap(slab->memory, slab->mapped_size);
            }
        }
        slabs_.clear();
        mapped_bytes_ = 0U;
    }

    [[nodiscard]] u64 mapped_bytes() const noexcept {
        std::scoped_lock lock(mutex_);
        return mapped_bytes_;
    }

    [[nodiscard]] u64 slab_count() const noexcept {
        std::scoped_lock lock(mutex_);
        return static_cast<u64>(slabs_.size());
    }

private:
    struct Range final {
        usize offset {0U};
        usize size {0U};
    };

    struct Slab final {
        void* memory {nullptr};
        usize mapped_size {0U};
        bool uses_map_jit {false};
        bool writable {true};
        bool usable {true};
        std::vector<Range> free_ranges;
    };

    struct Allocation final {
        usize slab_index {0U};
        usize offset {0U};
    };

    [[nodiscard]] static usize round_to_page(usize bytes) noexcept {
        const usize page_size = system_page_size();
        if (bytes > std::numeric_limits<usize>::max() - (page_size - 1U)) {
            return 0U;
        }
        return ((bytes + page_size - 1U) / page_size) * page_size;
    }

    [[nodiscard]] std::optional<usize> create_slab(
        usize minimum_bytes) noexcept {
        const usize requested = std::max(kDefaultSlabBytes, minimum_bytes);
        const usize mapped_size = round_to_page(requested);
        if (mapped_size == 0U) return std::nullopt;

        const int base_flags = MAP_PRIVATE | MAP_ANON;
        void* memory = MAP_FAILED;
        bool uses_map_jit = false;
        bool writable = true;

#if defined(__APPLE__) && defined(MAP_JIT)
        if (!conservative_device_jit_mode()) {
            memory = ::mmap(nullptr,
                            mapped_size,
                            PROT_READ | PROT_WRITE | PROT_EXEC,
                            base_flags | MAP_JIT,
                            -1,
                            0);
            if (memory != MAP_FAILED) {
                uses_map_jit = true;
            }
        }
#endif
        if (map_jit_only_ && memory == MAP_FAILED) {
            return std::nullopt;
        }
        if (memory == MAP_FAILED) {
            memory = ::mmap(nullptr,
                            mapped_size,
                            PROT_READ | PROT_WRITE,
                            base_flags,
                            -1,
                            0);
        }
        if (memory == MAP_FAILED) {
            memory = ::mmap(nullptr,
                            mapped_size,
                            PROT_READ | PROT_WRITE | PROT_EXEC,
                            base_flags,
                            -1,
                            0);
        }
        if (memory == MAP_FAILED) return std::nullopt;

        auto slab = std::make_unique<Slab>();
        slab->memory = memory;
        slab->mapped_size = mapped_size;
        slab->uses_map_jit = uses_map_jit;
        slab->writable = writable;
        slab->free_ranges.push_back(Range {0U, mapped_size});
        const usize index = slabs_.size();
        slabs_.push_back(std::move(slab));
        mapped_bytes_ += static_cast<u64>(mapped_size);
        return index;
    }

    [[nodiscard]] std::optional<Allocation> allocate(
        usize allocation_size) noexcept {
        const auto try_slab = [&](usize slab_index)
            -> std::optional<Allocation> {
            Slab& slab = *slabs_[slab_index];
            if (!slab.usable) return std::nullopt;
            for (usize index = 0U; index < slab.free_ranges.size(); ++index) {
                Range& range = slab.free_ranges[index];
                if (range.size < allocation_size) continue;
                const usize offset = range.offset;
                range.offset += allocation_size;
                range.size -= allocation_size;
                if (range.size == 0U) {
                    slab.free_ranges.erase(slab.free_ranges.begin() +
                                           static_cast<std::ptrdiff_t>(index));
                }
                return Allocation {slab_index, offset};
            }
            return std::nullopt;
        };

        for (usize index = 0U; index < slabs_.size(); ++index) {
            if (auto allocation = try_slab(index); allocation.has_value()) {
                return allocation;
            }
        }
        auto new_slab = create_slab(allocation_size);
        if (!new_slab.has_value()) return std::nullopt;
        return try_slab(*new_slab);
    }

    [[nodiscard]] static bool begin_write(Slab& slab) noexcept {
        if (slab.uses_map_jit) {
            set_thread_jit_write_protection(false);
            return true;
        }
        if (slab.writable) return true;
        if (::mprotect(slab.memory,
                       slab.mapped_size,
                       PROT_READ | PROT_WRITE) != 0) {
            return false;
        }
        slab.writable = true;
        return true;
    }

    [[nodiscard]] static bool end_write(Slab& slab) noexcept {
        if (slab.uses_map_jit) {
            set_thread_jit_write_protection(true);
            return true;
        }
        if (::mprotect(slab.memory,
                       slab.mapped_size,
                       PROT_READ | PROT_EXEC) != 0) {
            return false;
        }
        slab.writable = false;
        return true;
    }

    static void merge_free_ranges(Slab& slab) noexcept {
        std::sort(slab.free_ranges.begin(),
                  slab.free_ranges.end(),
                  [](const Range& left, const Range& right) {
                      return left.offset < right.offset;
                  });
        usize write = 0U;
        for (const Range range : slab.free_ranges) {
            if (write != 0U) {
                Range& previous = slab.free_ranges[write - 1U];
                if (previous.offset + previous.size == range.offset) {
                    previous.size += range.size;
                    continue;
                }
            }
            slab.free_ranges[write++] = range;
        }
        slab.free_ranges.resize(write);
    }

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Slab>> slabs_;
    u64 mapped_bytes_ {0U};
    bool map_jit_only_ {false};
};

ExecutableBlock::~ExecutableBlock() { reset(); }

void ExecutableBlock::reset() noexcept {
    if (memory != nullptr && mapped_size != 0U) {
        if (arena != nullptr) {
            arena->release(memory, mapped_size);
        } else {
            (void)::munmap(memory, mapped_size);
        }
    }
    arena = nullptr;
    memory = nullptr;
    mapped_size = 0U;
    function = nullptr;
}

[[nodiscard]] std::optional<ExecutableBlock> finalize_with_mapping(
    const std::vector<u32>& code,
    usize mapped_size,
    int flags,
    int initial_protection,
    bool uses_map_jit) noexcept {
    void* memory = ::mmap(nullptr,
                          mapped_size,
                          initial_protection,
                          flags,
                          -1,
                          0);
    if (memory == MAP_FAILED) return std::nullopt;

    const usize byte_count = code.size() * sizeof(u32);
    if (uses_map_jit) set_thread_jit_write_protection(false);
    std::memcpy(memory, code.data(), byte_count);
    invalidate_instruction_cache(memory, byte_count);
    if (uses_map_jit) set_thread_jit_write_protection(true);

    if ((initial_protection & PROT_EXEC) == 0) {
        if (::mprotect(memory, mapped_size, PROT_READ | PROT_EXEC) != 0) {
            (void)::munmap(memory, mapped_size);
            return std::nullopt;
        }
    } else {
        // Prefer W^X after finalization. Some debugger-enabled iOS versions
        // permit the initial RWX mapping but reject the later protection
        // transition; in that case the already executable mapping remains a
        // valid last-resort JIT path.
        (void)::mprotect(memory, mapped_size, PROT_READ | PROT_EXEC);
    }

    return ExecutableBlock(memory, mapped_size, function_at(memory));
}

[[nodiscard, maybe_unused]] std::optional<ExecutableBlock> finalize_code(
    const std::vector<u32>& code) noexcept {
    if (code.empty() || code.size() > kMaximumNativeInstructions) {
        return std::nullopt;
    }
    const usize byte_count = code.size() * sizeof(u32);
    const usize page_size = system_page_size();
    if (byte_count > std::numeric_limits<usize>::max() - (page_size - 1U)) {
        return std::nullopt;
    }
    const usize mapped_size =
        ((byte_count + page_size - 1U) / page_size) * page_size;
    const int base_flags = MAP_PRIVATE | MAP_ANON;

    if (auto block = finalize_with_mapping(
            code,
            mapped_size,
            base_flags,
            PROT_READ | PROT_WRITE,
            false)) {
        return block;
    }

#if defined(__APPLE__) && defined(MAP_JIT)
    if (auto block = finalize_with_mapping(
            code,
            mapped_size,
            base_flags | MAP_JIT,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            true)) {
        return block;
    }
    if (auto block = finalize_with_mapping(
            code,
            mapped_size,
            base_flags | MAP_JIT,
            PROT_READ | PROT_WRITE,
            true)) {
        return block;
    }
#endif

    return finalize_with_mapping(
        code,
        mapped_size,
        base_flags,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        false);
}

[[nodiscard]] bool read_u8(const std::vector<u8>& code,
                           usize& pc,
                           u8& value) noexcept {
    if (pc >= code.size()) return false;
    value = code[pc++];
    return true;
}

[[nodiscard]] bool read_u16(const std::vector<u8>& code,
                            usize& pc,
                            u16& value) noexcept {
    u8 high = 0U;
    u8 low = 0U;
    if (!read_u8(code, pc, high) || !read_u8(code, pc, low)) return false;
    value = static_cast<u16>((static_cast<u16>(high) << 8U) |
                             static_cast<u16>(low));
    return true;
}

[[nodiscard]] bool read_i8(const std::vector<u8>& code,
                           usize& pc,
                           i32& value) noexcept {
    u8 raw = 0U;
    if (!read_u8(code, pc, raw)) return false;
    value = static_cast<i32>(static_cast<i8>(raw));
    return true;
}

[[nodiscard]] bool read_i16(const std::vector<u8>& code,
                            usize& pc,
                            i32& value) noexcept {
    u16 raw = 0U;
    if (!read_u16(code, pc, raw)) return false;
    value = static_cast<i32>(static_cast<i16>(raw));
    return true;
}

[[nodiscard]] bool read_i32(const std::vector<u8>& code,
                            usize& pc,
                            i32& value) noexcept {
    u8 b0 = 0U;
    u8 b1 = 0U;
    u8 b2 = 0U;
    u8 b3 = 0U;
    if (!read_u8(code, pc, b0) || !read_u8(code, pc, b1) ||
        !read_u8(code, pc, b2) || !read_u8(code, pc, b3)) {
        return false;
    }
    const u32 raw = (static_cast<u32>(b0) << 24U) |
                    (static_cast<u32>(b1) << 16U) |
                    (static_cast<u32>(b2) << 8U) |
                    static_cast<u32>(b3);
    value = static_cast<i32>(raw);
    return true;
}

[[nodiscard]] std::optional<usize> branch_target(usize pc,
                                                 i32 offset,
                                                 usize code_size) noexcept {
    const i64 target = static_cast<i64>(pc) + static_cast<i64>(offset);
    if (target < 0 || target >= static_cast<i64>(code_size)) {
        return std::nullopt;
    }
    return static_cast<usize>(target);
}

[[nodiscard]] bool is_unary_branch(u8 opcode) noexcept {
    return opcode >= 0x99U && opcode <= 0x9EU;
}

[[nodiscard]] bool is_binary_branch(u8 opcode) noexcept {
    return opcode >= 0x9FU && opcode <= 0xA4U;
}

[[nodiscard]] bool is_reference_branch(u8 opcode) noexcept {
    return opcode == 0xA5U || opcode == 0xA6U ||
           opcode == 0xC6U || opcode == 0xC7U;
}

[[nodiscard]] bool is_conditional_branch(u8 opcode) noexcept {
    return is_unary_branch(opcode) || is_binary_branch(opcode) ||
           is_reference_branch(opcode);
}

[[nodiscard]] bool is_goto(u8 opcode) noexcept {
    return opcode == 0xA7U || opcode == 0xC8U;
}

[[nodiscard]] bool is_jsr(u8 opcode) noexcept {
    return opcode == 0xA8U || opcode == 0xC9U;
}

[[nodiscard]] bool is_ret(u8 opcode) noexcept {
    return opcode == 0xA9U;
}

[[nodiscard]] std::optional<Arm64Condition> branch_condition(
    u8 opcode) noexcept {
    switch (opcode) {
    case 0x99U:
    case 0x9FU:
    case 0xA5U:
    case 0xC6U:
        return Arm64Condition::equal;
    case 0x9AU:
    case 0xA0U:
    case 0xA6U:
    case 0xC7U:
        return Arm64Condition::not_equal;
    case 0x9BU:
    case 0xA1U:
        return Arm64Condition::less_than;
    case 0x9CU:
    case 0xA2U:
        return Arm64Condition::greater_equal;
    case 0x9DU:
    case 0xA3U:
        return Arm64Condition::greater_than;
    case 0x9EU:
    case 0xA4U:
        return Arm64Condition::less_equal;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::vector<DecodedInstruction>> decode_method(
    const classfile::ClassFile& owner,
    const classfile::Method& method,
    std::optional<u8>* unsupported_opcode = nullptr) {
    if (!method.code.has_value()) return std::nullopt;
    const std::vector<u8>& code = method.code->bytecode;
    if (code.empty()) return std::nullopt;

    std::vector<DecodedInstruction> instructions;
    usize pc = 0U;
    while (pc < code.size()) {
        DecodedInstruction instruction;
        instruction.pc = pc;
        if (!read_u8(code, pc, instruction.opcode)) return std::nullopt;

        switch (instruction.opcode) {
        case 0x00U:
        case 0x01U:
        case 0x02U:
        case 0x03U:
        case 0x04U:
        case 0x05U:
        case 0x06U:
        case 0x07U:
        case 0x08U:
        case 0x09U:
        case 0x0AU:
        case 0x0BU:
        case 0x0CU:
        case 0x0DU:
        case 0x0EU:
        case 0x0FU:
        case 0x1AU:
        case 0x1BU:
        case 0x1CU:
        case 0x1DU:
        case 0x1EU:
        case 0x1FU:
        case 0x20U:
        case 0x21U:
        case 0x22U:
        case 0x23U:
        case 0x24U:
        case 0x25U:
        case 0x26U:
        case 0x27U:
        case 0x28U:
        case 0x29U:
        case 0x2AU:
        case 0x2BU:
        case 0x2CU:
        case 0x2DU:
        case 0x2EU:
        case 0x2FU:
        case 0x30U:
        case 0x31U:
        case 0x32U:
        case 0x33U:
        case 0x34U:
        case 0x35U:
        case 0x3BU:
        case 0x3CU:
        case 0x3DU:
        case 0x3EU:
        case 0x3FU:
        case 0x40U:
        case 0x41U:
        case 0x42U:
        case 0x43U:
        case 0x44U:
        case 0x45U:
        case 0x46U:
        case 0x47U:
        case 0x48U:
        case 0x49U:
        case 0x4AU:
        case 0x4BU:
        case 0x4CU:
        case 0x4DU:
        case 0x4EU:
        case 0x4FU:
        case 0x50U:
        case 0x51U:
        case 0x52U:
        case 0x53U:
        case 0x54U:
        case 0x55U:
        case 0x56U:
        case 0x57U:
        case 0x58U:
        case 0x59U:
        case 0x5AU:
        case 0x5BU:
        case 0x5CU:
        case 0x5DU:
        case 0x5EU:
        case 0x5FU:
        case 0x60U:
        case 0x61U:
        case 0x62U:
        case 0x63U:
        case 0x64U:
        case 0x65U:
        case 0x66U:
        case 0x67U:
        case 0x68U:
        case 0x69U:
        case 0x6AU:
        case 0x6BU:
        case 0x6CU:
        case 0x6DU:
        case 0x6EU:
        case 0x6FU:
        case 0x70U:
        case 0x71U:
        case 0x72U:
        case 0x73U:
        case 0x74U:
        case 0x75U:
        case 0x76U:
        case 0x77U:
        case 0x78U:
        case 0x79U:
        case 0x7AU:
        case 0x7BU:
        case 0x7CU:
        case 0x7DU:
        case 0x7EU:
        case 0x7FU:
        case 0x80U:
        case 0x81U:
        case 0x82U:
        case 0x83U:
        case 0x85U:
        case 0x86U:
        case 0x87U:
        case 0x88U:
        case 0x89U:
        case 0x8AU:
        case 0x8BU:
        case 0x8CU:
        case 0x8DU:
        case 0x8EU:
        case 0x8FU:
        case 0x90U:
        case 0x91U:
        case 0x92U:
        case 0x93U:
        case 0x94U:
        case 0x95U:
        case 0x96U:
        case 0x97U:
        case 0x98U:
        case 0xACU:
        case 0xADU:
        case 0xAEU:
        case 0xAFU:
        case 0xB0U:
        case 0xB1U:
        case 0xBEU:
        case 0xBFU:
        case 0xC2U:
        case 0xC3U:
            break;

        case 0x10U:
            if (!read_i8(code, pc, instruction.immediate)) {
                return std::nullopt;
            }
            break;

        case 0x11U:
            if (!read_i16(code, pc, instruction.immediate)) {
                return std::nullopt;
            }
            break;

        case 0x12U: {
            u8 index = 0U;
            if (!read_u8(code, pc, index)) return std::nullopt;
            instruction.local_index = index;
            auto constant = owner.constant(static_cast<u16>(index));
            if (!constant) return std::nullopt;
            if ((*constant)->kind == classfile::ConstantKind::integer) {
                instruction.value_kind = JavaTypeKind::integer;
                instruction.immediate = static_cast<i32>(
                    static_cast<u32>((*constant)->bits));
            } else if ((*constant)->kind == classfile::ConstantKind::float32) {
                instruction.value_kind = JavaTypeKind::float32;
                instruction.wide_immediate = (*constant)->bits;
            } else if ((*constant)->kind ==
                           classfile::ConstantKind::string_ref ||
                       (*constant)->kind ==
                           classfile::ConstantKind::class_ref) {
                instruction.value_kind = JavaTypeKind::reference;
            } else {
                return std::nullopt;
            }
            break;
        }

        case 0x13U: {
            u16 index = 0U;
            if (!read_u16(code, pc, index)) return std::nullopt;
            instruction.local_index = index;
            auto constant = owner.constant(index);
            if (!constant) return std::nullopt;
            if ((*constant)->kind == classfile::ConstantKind::integer) {
                instruction.value_kind = JavaTypeKind::integer;
                instruction.immediate = static_cast<i32>(
                    static_cast<u32>((*constant)->bits));
            } else if ((*constant)->kind == classfile::ConstantKind::float32) {
                instruction.value_kind = JavaTypeKind::float32;
                instruction.wide_immediate = (*constant)->bits;
            } else if ((*constant)->kind ==
                           classfile::ConstantKind::string_ref ||
                       (*constant)->kind ==
                           classfile::ConstantKind::class_ref) {
                instruction.value_kind = JavaTypeKind::reference;
            } else {
                return std::nullopt;
            }
            break;
        }

        case 0x14U: {
            u16 index = 0U;
            if (!read_u16(code, pc, index)) return std::nullopt;
            instruction.local_index = index;
            auto constant = owner.constant(index);
            if (!constant) return std::nullopt;
            if ((*constant)->kind == classfile::ConstantKind::long64) {
                instruction.value_kind = JavaTypeKind::long_integer;
            } else if ((*constant)->kind == classfile::ConstantKind::float64) {
                instruction.value_kind = JavaTypeKind::float64;
            } else {
                return std::nullopt;
            }
            instruction.wide_immediate = (*constant)->bits;
            break;
        }

        case 0x15U:
        case 0x16U:
        case 0x17U:
        case 0x18U:
        case 0x19U:
        case 0x36U:
        case 0x37U:
        case 0x38U:
        case 0x39U:
        case 0x3AU: {
            u8 index = 0U;
            if (!read_u8(code, pc, index)) return std::nullopt;
            instruction.local_index = index;
            break;
        }

        case 0xB2U:
        case 0xB3U:
        case 0xB4U:
        case 0xB5U: {
            u16 index = 0U;
            if (!read_u16(code, pc, index)) return std::nullopt;
            auto reference = owner.member_reference(index);
            if (!reference ||
                reference->kind != classfile::ConstantKind::field_ref) {
                return std::nullopt;
            }
            auto field_type = parse_field_descriptor(reference->descriptor);
            if (!field_type || !scalar_like(field_type->kind)) {
                return std::nullopt;
            }
            instruction.local_index = index;
            instruction.value_kind = field_type->kind;
            break;
        }

        case 0xB6U:
        case 0xB7U:
        case 0xB8U:
        case 0xB9U: {
            u16 index = 0U;
            if (!read_u16(code, pc, index)) return std::nullopt;
            auto reference = owner.member_reference(index);
            if (!reference ||
                (reference->kind != classfile::ConstantKind::method_ref &&
                 reference->kind !=
                     classfile::ConstantKind::interface_method_ref)) {
                return std::nullopt;
            }
            if (instruction.opcode == 0xB9U) {
                u8 count = 0U;
                u8 zero = 0U;
                if (!read_u8(code, pc, count) ||
                    !read_u8(code, pc, zero) || zero != 0U ||
                    reference->kind !=
                        classfile::ConstantKind::interface_method_ref) {
                    return std::nullopt;
                }
                auto call = parse_method_descriptor(reference->descriptor);
                if (!call || call->parameter_slots(true) != count) {
                    return std::nullopt;
                }
                for (const TypeDescriptor& parameter : call->parameters) {
                    if (!scalar_like(parameter.kind)) return std::nullopt;
                }
                if (call->return_type.kind != JavaTypeKind::void_type &&
                    !scalar_like(call->return_type.kind)) {
                    return std::nullopt;
                }
                instruction.local_index = index;
                instruction.argument_slots = static_cast<u32>(
                    call->parameter_slots(false));
                instruction.has_receiver = true;
                instruction.value_kind = call->return_type.kind;
                break;
            }

            auto call = parse_method_descriptor(reference->descriptor);
            if (!call) return std::nullopt;
            for (const TypeDescriptor& parameter : call->parameters) {
                if (!scalar_like(parameter.kind)) return std::nullopt;
            }
            if (call->return_type.kind != JavaTypeKind::void_type &&
                !scalar_like(call->return_type.kind)) {
                return std::nullopt;
            }
            instruction.local_index = index;
            instruction.argument_slots = static_cast<u32>(
                call->parameter_slots(false));
            instruction.has_receiver = instruction.opcode != 0xB8U;
            instruction.value_kind = call->return_type.kind;
            break;
        }

        case 0xBAU: {
            u16 index = 0U;
            u8 zero_first = 0U;
            u8 zero_second = 0U;
            if (!read_u16(code, pc, index) ||
                !read_u8(code, pc, zero_first) ||
                !read_u8(code, pc, zero_second) ||
                zero_first != 0U || zero_second != 0U) {
                return std::nullopt;
            }
            auto dynamic = owner.invoke_dynamic_reference(index);
            if (!dynamic) return std::nullopt;
            auto call = parse_method_descriptor(dynamic->descriptor);
            if (!call) return std::nullopt;
            for (const TypeDescriptor& parameter : call->parameters) {
                if (!scalar_like(parameter.kind)) return std::nullopt;
            }
            if (call->return_type.kind != JavaTypeKind::reference &&
                call->return_type.kind != JavaTypeKind::array) {
                return std::nullopt;
            }
            instruction.local_index = index;
            instruction.argument_slots = static_cast<u32>(
                call->parameter_slots(false));
            instruction.has_receiver = false;
            instruction.value_kind = call->return_type.kind;
            break;
        }

        case 0xBBU: {
            u16 index = 0U;
            if (!read_u16(code, pc, index)) return std::nullopt;
            auto target = owner.class_name_constant(index);
            if (!target || target->starts_with('[')) return std::nullopt;
            instruction.local_index = index;
            instruction.value_kind = JavaTypeKind::reference;
            break;
        }

        case 0xBCU: {
            u8 atype = 0U;
            if (!read_u8(code, pc, atype) || atype < 4U || atype > 11U) {
                return std::nullopt;
            }
            instruction.local_index = atype;
            instruction.value_kind = JavaTypeKind::array;
            break;
        }

        case 0xBDU: {
            u16 index = 0U;
            if (!read_u16(code, pc, index)) return std::nullopt;
            auto target = owner.class_name_constant(index);
            if (!target) return std::nullopt;
            instruction.local_index = index;
            instruction.value_kind = JavaTypeKind::array;
            break;
        }

        case 0xC5U: {
            u16 index = 0U;
            u8 dimensions = 0U;
            if (!read_u16(code, pc, index) ||
                !read_u8(code, pc, dimensions) || dimensions == 0U) {
                return std::nullopt;
            }
            auto target = owner.class_name_constant(index);
            if (!target || !target->starts_with('[')) return std::nullopt;
            usize declared_dimensions = 0U;
            while (declared_dimensions < target->size() &&
                   (*target)[declared_dimensions] == '[') {
                ++declared_dimensions;
            }
            if (static_cast<usize>(dimensions) > declared_dimensions) {
                return std::nullopt;
            }
            instruction.local_index = index;
            instruction.argument_slots = dimensions;
            instruction.value_kind = JavaTypeKind::array;
            break;
        }

        case 0xC0U:
        case 0xC1U: {
            u16 index = 0U;
            if (!read_u16(code, pc, index)) return std::nullopt;
            auto target = owner.class_name_constant(index);
            if (!target) return std::nullopt;
            instruction.local_index = index;
            break;
        }

        case 0x84U: {
            u8 index = 0U;
            if (!read_u8(code, pc, index) ||
                !read_i8(code, pc, instruction.immediate)) {
                return std::nullopt;
            }
            instruction.local_index = index;
            break;
        }

        case 0x99U:
        case 0x9AU:
        case 0x9BU:
        case 0x9CU:
        case 0x9DU:
        case 0x9EU:
        case 0x9FU:
        case 0xA0U:
        case 0xA1U:
        case 0xA2U:
        case 0xA3U:
        case 0xA4U:
        case 0xA5U:
        case 0xA6U:
        case 0xA7U:
        case 0xA8U:
        case 0xC6U:
        case 0xC7U: {
            i32 offset = 0;
            if (!read_i16(code, pc, offset)) return std::nullopt;
            instruction.branch_target = branch_target(
                instruction.pc, offset, code.size());
            if (!instruction.branch_target.has_value()) return std::nullopt;
            break;
        }

        case 0xA9U: {
            u8 index = 0U;
            if (!read_u8(code, pc, index)) return std::nullopt;
            instruction.local_index = index;
            break;
        }

        case 0xAAU:
        case 0xABU: {
            while ((pc & 3U) != 0U) {
                u8 padding = 0U;
                if (!read_u8(code, pc, padding) || padding != 0U) {
                    return std::nullopt;
                }
            }
            i32 default_offset = 0;
            if (!read_i32(code, pc, default_offset)) return std::nullopt;
            instruction.default_target = branch_target(
                instruction.pc, default_offset, code.size());
            if (!instruction.default_target.has_value()) return std::nullopt;

            if (instruction.opcode == 0xAAU) {
                i32 low = 0;
                i32 high = 0;
                if (!read_i32(code, pc, low) || !read_i32(code, pc, high)) {
                    return std::nullopt;
                }
                const i64 count = static_cast<i64>(high) -
                                  static_cast<i64>(low) + 1;
                if (count <= 0 ||
                    count > static_cast<i64>(kMaximumSwitchCases)) {
                    return std::nullopt;
                }
                instruction.switch_targets.reserve(static_cast<usize>(count));
                for (i64 index = 0; index < count; ++index) {
                    i32 offset = 0;
                    if (!read_i32(code, pc, offset)) return std::nullopt;
                    auto target = branch_target(
                        instruction.pc, offset, code.size());
                    if (!target.has_value()) return std::nullopt;
                    instruction.switch_targets.push_back(SwitchTarget {
                        .key = static_cast<i32>(static_cast<i64>(low) + index),
                        .target_pc = *target,
                    });
                }
            } else {
                i32 pair_count = 0;
                if (!read_i32(code, pc, pair_count) || pair_count < 0 ||
                    pair_count > static_cast<i32>(kMaximumSwitchCases)) {
                    return std::nullopt;
                }
                instruction.switch_targets.reserve(
                    static_cast<usize>(pair_count));
                i32 previous_key = std::numeric_limits<i32>::min();
                for (i32 index = 0; index < pair_count; ++index) {
                    i32 key = 0;
                    i32 offset = 0;
                    if (!read_i32(code, pc, key) ||
                        !read_i32(code, pc, offset) ||
                        (index != 0 && key <= previous_key)) {
                        return std::nullopt;
                    }
                    auto target = branch_target(
                        instruction.pc, offset, code.size());
                    if (!target.has_value()) return std::nullopt;
                    instruction.switch_targets.push_back(SwitchTarget {
                        .key = key,
                        .target_pc = *target,
                    });
                    previous_key = key;
                }
            }
            break;
        }

        case 0xC4U: {
            u8 wide_opcode = 0U;
            u16 index = 0U;
            if (!read_u8(code, pc, wide_opcode) ||
                !read_u16(code, pc, index)) {
                return std::nullopt;
            }
            if (wide_opcode != 0x15U && wide_opcode != 0x16U &&
                wide_opcode != 0x17U && wide_opcode != 0x18U &&
                wide_opcode != 0x19U && wide_opcode != 0x36U &&
                wide_opcode != 0x37U && wide_opcode != 0x38U &&
                wide_opcode != 0x39U && wide_opcode != 0x3AU &&
                wide_opcode != 0x84U && wide_opcode != 0xA9U) {
                return std::nullopt;
            }
            instruction.opcode = wide_opcode;
            instruction.local_index = index;
            if (wide_opcode == 0x84U &&
                !read_i16(code, pc, instruction.immediate)) {
                return std::nullopt;
            }
            break;
        }

        case 0xC8U:
        case 0xC9U: {
            i32 offset = 0;
            if (!read_i32(code, pc, offset)) return std::nullopt;
            instruction.branch_target = branch_target(
                instruction.pc, offset, code.size());
            if (!instruction.branch_target.has_value()) return std::nullopt;
            break;
        }

        default:
            if (unsupported_opcode != nullptr) {
                *unsupported_opcode = instruction.opcode;
            }
            return std::nullopt;
        }

        instruction.next_pc = pc;
        instructions.push_back(std::move(instruction));
    }
    return instructions;
}

struct StackShape final {
    u32 pop {0};
    u32 push {0};
};

[[nodiscard]] std::optional<StackShape> stack_shape(
    const DecodedInstruction& instruction,
    JavaTypeKind return_kind) noexcept {
    switch (instruction.opcode) {
    case 0x00U:
    case 0x84U:
    case 0xA7U:
    case 0xA9U:
    case 0xC8U:
        return StackShape {};
    case 0x01U:
    case 0x02U:
    case 0x03U:
    case 0x04U:
    case 0x05U:
    case 0x06U:
    case 0x07U:
    case 0x08U:
    case 0x0BU:
    case 0x0CU:
    case 0x0DU:
    case 0x10U:
    case 0x11U:
    case 0x12U:
    case 0x13U:
    case 0x15U:
    case 0x17U:
    case 0x19U:
    case 0x1AU:
    case 0x1BU:
    case 0x1CU:
    case 0x1DU:
    case 0x22U:
    case 0x23U:
    case 0x24U:
    case 0x25U:
    case 0x2AU:
    case 0x2BU:
    case 0x2CU:
    case 0x2DU:
    case 0xA8U:
    case 0xC9U:
        return StackShape {.pop = 0U, .push = 1U};
    case 0x09U:
    case 0x0AU:
    case 0x0EU:
    case 0x0FU:
    case 0x14U:
    case 0x16U:
    case 0x18U:
    case 0x1EU:
    case 0x1FU:
    case 0x20U:
    case 0x21U:
    case 0x26U:
    case 0x27U:
    case 0x28U:
    case 0x29U:
        return StackShape {.pop = 0U, .push = 2U};
    case 0x36U:
    case 0x38U:
    case 0x3AU:
    case 0x3BU:
    case 0x3CU:
    case 0x3DU:
    case 0x3EU:
    case 0x43U:
    case 0x44U:
    case 0x45U:
    case 0x46U:
    case 0x4BU:
    case 0x4CU:
    case 0x4DU:
    case 0x4EU:
    case 0x57U:
        return StackShape {.pop = 1U, .push = 0U};
    case 0x37U:
    case 0x39U:
    case 0x3FU:
    case 0x40U:
    case 0x41U:
    case 0x42U:
    case 0x47U:
    case 0x48U:
    case 0x49U:
    case 0x4AU:
        return StackShape {.pop = 2U, .push = 0U};
    case 0x58U:
        return StackShape {.pop = 2U, .push = 0U};
    case 0x59U:
        return StackShape {.pop = 1U, .push = 2U};
    case 0x5AU:
        return StackShape {.pop = 2U, .push = 3U};
    case 0x5BU:
        return StackShape {.pop = 3U, .push = 4U};
    case 0x5CU:
        return StackShape {.pop = 2U, .push = 4U};
    case 0x5DU:
        return StackShape {.pop = 3U, .push = 5U};
    case 0x5EU:
        return StackShape {.pop = 4U, .push = 6U};
    case 0x5FU:
        return StackShape {.pop = 2U, .push = 2U};
    case 0x2EU:
    case 0x30U:
    case 0x32U:
    case 0x33U:
    case 0x34U:
    case 0x35U:
        return StackShape {.pop = 2U, .push = 1U};
    case 0x2FU:
    case 0x31U:
        return StackShape {.pop = 2U, .push = 2U};
    case 0x4FU:
    case 0x51U:
    case 0x53U:
    case 0x54U:
    case 0x55U:
    case 0x56U:
        return StackShape {.pop = 3U, .push = 0U};
    case 0x50U:
    case 0x52U:
        return StackShape {.pop = 4U, .push = 0U};
    case 0xB2U:
        if (!scalar_like(instruction.value_kind)) return std::nullopt;
        return StackShape {
            .pop = 0U,
            .push = jit_slot_count(instruction.value_kind),
        };
    case 0xB3U:
        if (!scalar_like(instruction.value_kind)) return std::nullopt;
        return StackShape {
            .pop = jit_slot_count(instruction.value_kind),
            .push = 0U,
        };
    case 0xB4U:
        if (!scalar_like(instruction.value_kind)) return std::nullopt;
        return StackShape {
            .pop = 1U,
            .push = jit_slot_count(instruction.value_kind),
        };
    case 0xB5U:
        if (!scalar_like(instruction.value_kind)) return std::nullopt;
        return StackShape {
            .pop = 1U + jit_slot_count(instruction.value_kind),
            .push = 0U,
        };
    case 0xB6U:
    case 0xB7U:
    case 0xB8U:
    case 0xB9U:
    case 0xBAU:
        if (instruction.value_kind != JavaTypeKind::void_type &&
            !scalar_like(instruction.value_kind)) {
            return std::nullopt;
        }
        return StackShape {
            .pop = instruction.argument_slots +
                   (instruction.has_receiver ? 1U : 0U),
            .push = instruction.value_kind == JavaTypeKind::void_type
                ? 0U
                : jit_slot_count(instruction.value_kind),
        };
    case 0xBBU:
        return StackShape {.pop = 0U, .push = 1U};
    case 0xC5U:
        return StackShape {
            .pop = instruction.argument_slots,
            .push = 1U,
        };
    case 0xBCU:
    case 0xBDU:
    case 0xC0U:
    case 0xC1U:
        return StackShape {.pop = 1U, .push = 1U};
    case 0xBEU:
        return StackShape {.pop = 1U, .push = 1U};
    case 0x60U:
    case 0x62U:
    case 0x64U:
    case 0x66U:
    case 0x68U:
    case 0x6AU:
    case 0x6CU:
    case 0x6EU:
    case 0x70U:
    case 0x72U:
    case 0x8EU:
    case 0x90U:
    case 0x95U:
    case 0x96U:
    case 0x78U:
    case 0x7AU:
    case 0x7CU:
    case 0x7EU:
    case 0x80U:
    case 0x82U:
        return StackShape {.pop = 2U, .push = 1U};
    case 0x61U:
    case 0x63U:
    case 0x65U:
    case 0x67U:
    case 0x69U:
    case 0x6BU:
    case 0x6DU:
    case 0x6FU:
    case 0x71U:
    case 0x73U:
    case 0x7FU:
    case 0x81U:
    case 0x83U:
        return StackShape {.pop = 4U, .push = 2U};
    case 0x79U:
    case 0x7BU:
    case 0x7DU:
        return StackShape {.pop = 3U, .push = 2U};
    case 0x75U:
    case 0x77U:
    case 0x8AU:
    case 0x8FU:
        return StackShape {.pop = 2U, .push = 2U};
    case 0x85U:
    case 0x87U:
    case 0x8CU:
    case 0x8DU:
        return StackShape {.pop = 1U, .push = 2U};
    case 0x88U:
    case 0x89U:
        return StackShape {.pop = 2U, .push = 1U};
    case 0x94U:
    case 0x97U:
    case 0x98U:
        return StackShape {.pop = 4U, .push = 1U};
    case 0x74U:
    case 0x76U:
    case 0x86U:
    case 0x8BU:
    case 0x91U:
    case 0x92U:
    case 0x93U:
        return StackShape {.pop = 1U, .push = 1U};
    case 0x99U:
    case 0x9AU:
    case 0x9BU:
    case 0x9CU:
    case 0x9DU:
    case 0x9EU:
    case 0xC6U:
    case 0xC7U:
    case 0xAAU:
    case 0xABU:
        return StackShape {.pop = 1U, .push = 0U};
    case 0x9FU:
    case 0xA0U:
    case 0xA1U:
    case 0xA2U:
    case 0xA3U:
    case 0xA4U:
    case 0xA5U:
    case 0xA6U:
        return StackShape {.pop = 2U, .push = 0U};
    case 0xACU:
        if (!integer_like(return_kind)) return std::nullopt;
        return StackShape {.pop = 1U, .push = 0U};
    case 0xADU:
        if (return_kind != JavaTypeKind::long_integer) return std::nullopt;
        return StackShape {.pop = 2U, .push = 0U};
    case 0xAEU:
        if (return_kind != JavaTypeKind::float32) return std::nullopt;
        return StackShape {.pop = 1U, .push = 0U};
    case 0xAFU:
        if (return_kind != JavaTypeKind::float64) return std::nullopt;
        return StackShape {.pop = 2U, .push = 0U};
    case 0xB0U:
        if (return_kind != JavaTypeKind::reference &&
            return_kind != JavaTypeKind::array) {
            return std::nullopt;
        }
        return StackShape {.pop = 1U, .push = 0U};
    case 0xB1U:
        if (return_kind != JavaTypeKind::void_type) return std::nullopt;
        return StackShape {};
    case 0xBFU:
    case 0xC2U:
    case 0xC3U:
        return StackShape {.pop = 1U, .push = 0U};
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool uses_local_index(u8 opcode) noexcept {
    return opcode == 0x15U || opcode == 0x16U || opcode == 0x17U ||
           opcode == 0x18U || opcode == 0x19U || opcode == 0x36U ||
           opcode == 0x37U || opcode == 0x38U || opcode == 0x39U ||
           opcode == 0x3AU || opcode == 0x84U || opcode == 0xA9U;
}

[[nodiscard]] u32 local_slot_width(u8 opcode) noexcept {
    return opcode == 0x16U || opcode == 0x18U ||
           opcode == 0x37U || opcode == 0x39U ? 2U : 1U;
}

[[nodiscard]] std::optional<std::vector<std::optional<u32>>>
compute_stack_depths(
    const std::vector<DecodedInstruction>& instructions,
    u32 local_slots,
    u32 stack_slots,
    JavaTypeKind return_kind,
    std::span<const classfile::ExceptionHandler> exception_handlers) {
    if (instructions.empty() || instructions.front().pc != 0U) {
        return std::nullopt;
    }

    std::unordered_map<usize, usize> index_by_pc;
    index_by_pc.reserve(instructions.size());
    for (usize index = 0; index < instructions.size(); ++index) {
        if (!index_by_pc.emplace(instructions[index].pc, index).second) {
            return std::nullopt;
        }
        if (uses_local_index(instructions[index].opcode) &&
            (instructions[index].local_index >= local_slots ||
             local_slot_width(instructions[index].opcode) >
                 local_slots - instructions[index].local_index)) {
            return std::nullopt;
        }
        if (instructions[index].opcode >= 0x1AU &&
            instructions[index].opcode <= 0x1DU &&
            static_cast<u32>(instructions[index].opcode - 0x1AU) >=
                local_slots) {
            return std::nullopt;
        }
        if (instructions[index].opcode >= 0x3BU &&
            instructions[index].opcode <= 0x3EU &&
            static_cast<u32>(instructions[index].opcode - 0x3BU) >=
                local_slots) {
            return std::nullopt;
        }
        if (instructions[index].opcode >= 0x1EU &&
            instructions[index].opcode <= 0x21U) {
            const u32 local =
                static_cast<u32>(instructions[index].opcode - 0x1EU);
            if (local >= local_slots || 2U > local_slots - local) {
                return std::nullopt;
            }
        }
        if (instructions[index].opcode >= 0x3FU &&
            instructions[index].opcode <= 0x42U) {
            const u32 local =
                static_cast<u32>(instructions[index].opcode - 0x3FU);
            if (local >= local_slots || 2U > local_slots - local) {
                return std::nullopt;
            }
        }
        if (instructions[index].opcode >= 0x22U &&
            instructions[index].opcode <= 0x25U &&
            static_cast<u32>(instructions[index].opcode - 0x22U) >=
                local_slots) {
            return std::nullopt;
        }
        if (instructions[index].opcode >= 0x26U &&
            instructions[index].opcode <= 0x29U) {
            const u32 local =
                static_cast<u32>(instructions[index].opcode - 0x26U);
            if (local >= local_slots || 2U > local_slots - local) {
                return std::nullopt;
            }
        }
        if (instructions[index].opcode >= 0x43U &&
            instructions[index].opcode <= 0x46U &&
            static_cast<u32>(instructions[index].opcode - 0x43U) >=
                local_slots) {
            return std::nullopt;
        }
        if (instructions[index].opcode >= 0x47U &&
            instructions[index].opcode <= 0x4AU) {
            const u32 local =
                static_cast<u32>(instructions[index].opcode - 0x47U);
            if (local >= local_slots || 2U > local_slots - local) {
                return std::nullopt;
            }
        }
        if (instructions[index].opcode >= 0x2AU &&
            instructions[index].opcode <= 0x2DU &&
            static_cast<u32>(instructions[index].opcode - 0x2AU) >=
                local_slots) {
            return std::nullopt;
        }
        if (instructions[index].opcode >= 0x4BU &&
            instructions[index].opcode <= 0x4EU &&
            static_cast<u32>(instructions[index].opcode - 0x4BU) >=
                local_slots) {
            return std::nullopt;
        }
    }

    const auto resolve = [&](usize target_pc) -> std::optional<usize> {
        const auto found = index_by_pc.find(target_pc);
        if (found == index_by_pc.end()) return std::nullopt;
        return found->second;
    };
    std::vector<usize> jsr_return_pcs;
    for (const DecodedInstruction& instruction : instructions) {
        if (is_jsr(instruction.opcode)) {
            if (!resolve(instruction.next_pc).has_value()) return std::nullopt;
            jsr_return_pcs.push_back(instruction.next_pc);
        }
    }
    std::sort(jsr_return_pcs.begin(), jsr_return_pcs.end());
    jsr_return_pcs.erase(
        std::unique(jsr_return_pcs.begin(), jsr_return_pcs.end()),
        jsr_return_pcs.end());

    std::vector<std::optional<u32>> depths(instructions.size());
    std::deque<usize> worklist;
    depths[0] = 0U;
    worklist.push_back(0U);
    for (const classfile::ExceptionHandler& handler : exception_handlers) {
        const auto handler_index = resolve(static_cast<usize>(handler.handler_pc));
        if (!handler_index.has_value()) return std::nullopt;
        if (!depths[*handler_index].has_value()) {
            depths[*handler_index] = 1U;
            worklist.push_back(*handler_index);
        } else if (*depths[*handler_index] != 1U) {
            return std::nullopt;
        }
    }

    const auto merge_depth = [&](usize successor,
                                 u32 depth,
                                 auto& queue,
                                 auto& values) -> bool {
        if (!values[successor].has_value()) {
            values[successor] = depth;
            queue.push_back(successor);
            return true;
        }
        return *values[successor] == depth;
    };

    while (!worklist.empty()) {
        const usize index = worklist.front();
        worklist.pop_front();
        const DecodedInstruction& instruction = instructions[index];
        const auto shape = stack_shape(instruction, return_kind);
        if (!shape.has_value() || *depths[index] < shape->pop) {
            return std::nullopt;
        }
        const u32 output_depth = *depths[index] - shape->pop + shape->push;
        if (output_depth > stack_slots) return std::nullopt;

        if (instruction.opcode == 0xACU || instruction.opcode == 0xADU ||
            instruction.opcode == 0xAEU || instruction.opcode == 0xAFU ||
            instruction.opcode == 0xB0U || instruction.opcode == 0xB1U ||
            instruction.opcode == 0xBFU) {
            if (output_depth != 0U) return std::nullopt;
            continue;
        }

        if (is_goto(instruction.opcode) || is_jsr(instruction.opcode)) {
            if (!instruction.branch_target.has_value()) return std::nullopt;
            const auto successor = resolve(*instruction.branch_target);
            if (!successor.has_value() ||
                !merge_depth(*successor, output_depth, worklist, depths)) {
                return std::nullopt;
            }
            continue;
        }

        if (is_ret(instruction.opcode)) {
            if (jsr_return_pcs.empty()) return std::nullopt;
            for (const usize return_pc : jsr_return_pcs) {
                const auto successor = resolve(return_pc);
                if (!successor.has_value() ||
                    !merge_depth(*successor,
                                 output_depth,
                                 worklist,
                                 depths)) {
                    return std::nullopt;
                }
            }
            continue;
        }

        if (is_conditional_branch(instruction.opcode)) {
            if (!instruction.branch_target.has_value()) return std::nullopt;
            const auto branch = resolve(*instruction.branch_target);
            const auto fallthrough = resolve(instruction.next_pc);
            if (!branch.has_value() || !fallthrough.has_value() ||
                !merge_depth(*branch, output_depth, worklist, depths) ||
                !merge_depth(*fallthrough, output_depth, worklist, depths)) {
                return std::nullopt;
            }
            continue;
        }

        if (instruction.opcode == 0xAAU || instruction.opcode == 0xABU) {
            if (!instruction.default_target.has_value()) return std::nullopt;
            const auto default_successor = resolve(*instruction.default_target);
            if (!default_successor.has_value() ||
                !merge_depth(*default_successor,
                             output_depth,
                             worklist,
                             depths)) {
                return std::nullopt;
            }
            for (const SwitchTarget& target : instruction.switch_targets) {
                const auto successor = resolve(target.target_pc);
                if (!successor.has_value() ||
                    !merge_depth(*successor,
                                 output_depth,
                                 worklist,
                                 depths)) {
                    return std::nullopt;
                }
            }
            continue;
        }

        const auto successor = resolve(instruction.next_pc);
        if (!successor.has_value() ||
            !merge_depth(*successor, output_depth, worklist, depths)) {
            return std::nullopt;
        }
    }

    return depths;
}

struct InstructionOptimization final {
    std::optional<i32> folded_result;
    std::optional<i32> right_constant;
    std::optional<bool> branch_taken;
    std::optional<usize> forced_target;
    bool elide_push {false};
    bool dead_local_write {false};
    bool gvn_elide_input_push {false};
    std::optional<u32> gvn_capture_register;
    std::optional<u32> gvn_reuse_register;
    bool licm_elide_input_push {false};
    std::optional<u32> licm_reuse_register;
    bool array_lease_elide_input_push {false};
    std::optional<u32> array_lease_register;
    std::optional<u32> array_lease_index_local;
    std::optional<u8> array_lease_opcode;
    std::optional<u32> array_lease_store_register;
    std::optional<u8> array_lease_store_opcode;
    std::optional<u32> scalar_array_new_local;
    std::optional<u32> scalar_array_load_local;
    std::optional<u32> scalar_array_store_local;
};

enum class InlineRecipeKind : u8 {
    identity_int,
    binary_int,
    unary_int,
    int_right_constant,
    rect_contains_int,
    static_byte_cursor_read,
    identity_long,
    binary_long,
    unary_long,
    identity_float,
    binary_float,
    unary_float,
    identity_double,
    binary_double,
    unary_double,
};

struct InlineRecipe final {
    InlineRecipeKind kind {InlineRecipeKind::identity_int};
    u8 opcode {0U};
    i32 constant {0};
    u32 first_operand {0U};
    u32 second_operand {0U};
    u32 nested_bytecode_cost {0U};
    u32 result_slots {1U};
    bool has_receiver {false};
};

struct LicmExpression final {
    usize loop_header_pc {0U};
    usize loop_end_pc {0U};
    u8 opcode {0U};
    u32 left_local {0U};
    u32 right_local {0U};
    u32 register_index {0U};
};

struct ArrayLoopLease final {
    usize loop_header_pc {0U};
    usize loop_end_pc {0U};
    u32 array_local {0U};
    u32 length_local {0U};
    u32 index_local {0U};
    u8 lease_opcode {0U};
    u32 pointer_register {29U};
};

struct UnrolledIntArrayReduction final {
    usize loop_header_pc {0U};
    usize loop_end_pc {0U};
    usize exit_pc {0U};
    u32 total_local {0U};
    u32 length_local {0U};
    u32 index_local {0U};
    u32 iteration_bytecodes {0U};
    u32 final_check_bytecodes {0U};
};

struct OptimizationPlan final {
    std::vector<InstructionOptimization> instructions;
    std::vector<u32> block_budget_costs;
    bool contains_loop {false};
    u64 propagated_constants {0};
    u64 folded_operations {0};
    u64 folded_branches {0};
    u64 dead_local_writes_eliminated {0};
    u64 common_subexpressions_eliminated {0};
    u64 cross_block_common_subexpressions_eliminated {0};
    u64 loop_invariants_hoisted {0};
    std::vector<LicmExpression> licm_expressions;
    std::vector<ArrayLoopLease> array_leases;
    u64 array_bounds_checks_eliminated {0};
    u64 array_runtime_calls_eliminated {0};
    u64 scalar_replaced_allocations {0};
    u64 scalar_replaced_array_accesses {0};
    std::optional<UnrolledIntArrayReduction> unrolled_int_array_reduction;
    u32 licm_register_count {0U};
    u32 gvn_register_count {0U};
    u64 budget_checks_elided {0};
};

[[nodiscard]] i32 wrapping_add(i32 left, i32 right) noexcept {
    return static_cast<i32>(static_cast<u32>(left) +
                            static_cast<u32>(right));
}

[[nodiscard]] i32 wrapping_subtract(i32 left, i32 right) noexcept {
    return static_cast<i32>(static_cast<u32>(left) -
                            static_cast<u32>(right));
}

[[nodiscard]] i32 wrapping_multiply(i32 left, i32 right) noexcept {
    return static_cast<i32>(static_cast<u32>(left) *
                            static_cast<u32>(right));
}

[[nodiscard]] i32 wrapping_negate(i32 value) noexcept {
    return static_cast<i32>(0U - static_cast<u32>(value));
}

[[nodiscard]] bool evaluate_integer_branch(u8 opcode,
                                           i32 left,
                                           i32 right = 0) noexcept {
    switch (opcode) {
    case 0x99U:
    case 0x9FU:
        return left == right;
    case 0x9AU:
    case 0xA0U:
        return left != right;
    case 0x9BU:
    case 0xA1U:
        return left < right;
    case 0x9CU:
    case 0xA2U:
        return left >= right;
    case 0x9DU:
    case 0xA3U:
        return left > right;
    case 0x9EU:
    case 0xA4U:
        return left <= right;
    default:
        return false;
    }
}

[[nodiscard]] std::optional<i32> pushed_constant(
    const DecodedInstruction& instruction) noexcept {
    if (instruction.opcode == 0x01U) return 0;
    if (instruction.opcode >= 0x02U && instruction.opcode <= 0x08U) {
        return static_cast<i32>(instruction.opcode) - 3;
    }
    if (instruction.opcode == 0x10U || instruction.opcode == 0x11U) {
        return instruction.immediate;
    }
    if ((instruction.opcode == 0x12U || instruction.opcode == 0x13U) &&
        instruction.value_kind == JavaTypeKind::integer) {
        return instruction.immediate;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<i32> fold_binary_integer(u8 opcode,
                                                     i32 left,
                                                     i32 right) noexcept {
    switch (opcode) {
    case 0x60U:
        return wrapping_add(left, right);
    case 0x64U:
        return wrapping_subtract(left, right);
    case 0x68U:
        return wrapping_multiply(left, right);
    case 0x6CU:
        if (right == 0) return std::nullopt;
        if (left == std::numeric_limits<i32>::min() && right == -1) {
            return std::numeric_limits<i32>::min();
        }
        return left / right;
    case 0x70U:
        if (right == 0) return std::nullopt;
        if (left == std::numeric_limits<i32>::min() && right == -1) {
            return 0;
        }
        return left % right;
    case 0x78U:
        return static_cast<i32>(static_cast<u32>(left) <<
                                (static_cast<u32>(right) & 31U));
    case 0x7AU:
        return left >> (static_cast<u32>(right) & 31U);
    case 0x7CU:
        return static_cast<i32>(static_cast<u32>(left) >>
                                (static_cast<u32>(right) & 31U));
    case 0x7EU:
        return left & right;
    case 0x80U:
        return left | right;
    case 0x82U:
        return left ^ right;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<i32> fold_unary_integer(u8 opcode,
                                                    i32 value) noexcept {
    switch (opcode) {
    case 0x74U:
        return wrapping_negate(value);
    case 0x91U:
        return static_cast<i32>(static_cast<i8>(value));
    case 0x92U:
        return static_cast<i32>(static_cast<u16>(value));
    case 0x93U:
        return static_cast<i32>(static_cast<i16>(value));
    default:
        return std::nullopt;
    }
}

struct ConstantFlowState final {
    std::vector<std::optional<i32>> locals;
    std::vector<std::optional<i32>> stack;
};

[[nodiscard]] std::vector<std::optional<ConstantFlowState>>
compute_constant_flow(
    const std::vector<DecodedInstruction>& instructions,
    const std::vector<std::optional<u32>>& depths,
    u32 local_slots,
    JavaTypeKind return_kind,
    std::span<const classfile::ExceptionHandler> exception_handlers) {
    std::vector<std::optional<ConstantFlowState>> states(instructions.size());
    if (instructions.empty()) return states;

    std::unordered_map<usize, usize> index_by_pc;
    index_by_pc.reserve(instructions.size());
    for (usize index = 0U; index < instructions.size(); ++index) {
        index_by_pc.emplace(instructions[index].pc, index);
    }

    std::vector<usize> worklist;
    std::vector<bool> queued(instructions.size(), false);
    const auto enqueue = [&](usize index) {
        if (!queued[index]) {
            queued[index] = true;
            worklist.push_back(index);
        }
    };
    const auto unknown_state = [&](usize index) {
        ConstantFlowState state;
        state.locals.resize(local_slots);
        state.stack.resize(depths[index].value_or(0U));
        return state;
    };
    const auto merge_state = [&](usize index,
                                 const ConstantFlowState& incoming) -> bool {
        if (index >= states.size() || !depths[index].has_value() ||
            incoming.locals.size() != local_slots ||
            incoming.stack.size() != *depths[index]) {
            return false;
        }
        if (!states[index].has_value()) {
            states[index] = incoming;
            enqueue(index);
            return true;
        }
        ConstantFlowState& current = *states[index];
        bool changed = false;
        for (usize slot = 0U; slot < current.locals.size(); ++slot) {
            if (current.locals[slot] != incoming.locals[slot] &&
                current.locals[slot].has_value()) {
                current.locals[slot] = std::nullopt;
                changed = true;
            }
        }
        for (usize slot = 0U; slot < current.stack.size(); ++slot) {
            if (current.stack[slot] != incoming.stack[slot] &&
                current.stack[slot].has_value()) {
                current.stack[slot] = std::nullopt;
                changed = true;
            }
        }
        if (changed) enqueue(index);
        return true;
    };

    if (depths[0].has_value()) {
        states[0] = unknown_state(0U);
        enqueue(0U);
    }
    // Exception edges deliberately start with unknown locals. This is more
    // conservative than the verifier state but guarantees optimization never
    // assumes a value across a potentially throwing instruction.
    for (const classfile::ExceptionHandler& handler : exception_handlers) {
        const auto found = index_by_pc.find(handler.handler_pc);
        if (found == index_by_pc.end() || !depths[found->second].has_value()) {
            continue;
        }
        ConstantFlowState handler_state = unknown_state(found->second);
        (void)merge_state(found->second, handler_state);
    }

    const auto pop_value = [](ConstantFlowState& state)
        -> std::optional<i32> {
        if (state.stack.empty()) return std::nullopt;
        const std::optional<i32> value = state.stack.back();
        state.stack.pop_back();
        return value;
    };
    const auto generic_transfer = [&](ConstantFlowState& state,
                                      const DecodedInstruction& instruction) {
        const auto shape = stack_shape(instruction, return_kind);
        if (!shape.has_value() || state.stack.size() < shape->pop) {
            state.stack.clear();
            return;
        }
        state.stack.resize(state.stack.size() - shape->pop);
        state.stack.insert(state.stack.end(), shape->push, std::nullopt);
    };

    while (!worklist.empty()) {
        const usize index = worklist.back();
        worklist.pop_back();
        queued[index] = false;
        if (!states[index].has_value()) continue;
        ConstantFlowState output = *states[index];
        const DecodedInstruction& instruction = instructions[index];

        if (const auto constant = pushed_constant(instruction);
            constant.has_value()) {
            output.stack.push_back(*constant);
        } else {
            switch (instruction.opcode) {
            case 0x00U:
                break;
            case 0x15U:
            case 0x19U: {
                const u32 local = instruction.local_index;
                output.stack.push_back(local < output.locals.size()
                    ? output.locals[local] : std::nullopt);
                break;
            }
            case 0x1AU: case 0x1BU: case 0x1CU: case 0x1DU: {
                const u32 local = static_cast<u32>(instruction.opcode - 0x1AU);
                output.stack.push_back(local < output.locals.size()
                    ? output.locals[local] : std::nullopt);
                break;
            }
            case 0x2AU: case 0x2BU: case 0x2CU: case 0x2DU: {
                const u32 local = static_cast<u32>(instruction.opcode - 0x2AU);
                output.stack.push_back(local < output.locals.size()
                    ? output.locals[local] : std::nullopt);
                break;
            }
            case 0x36U:
            case 0x3AU: {
                const std::optional<i32> value = pop_value(output);
                if (instruction.local_index < output.locals.size()) {
                    output.locals[instruction.local_index] = value;
                }
                break;
            }
            case 0x3BU: case 0x3CU: case 0x3DU: case 0x3EU: {
                const std::optional<i32> value = pop_value(output);
                const u32 local = static_cast<u32>(instruction.opcode - 0x3BU);
                if (local < output.locals.size()) output.locals[local] = value;
                break;
            }
            case 0x4BU: case 0x4CU: case 0x4DU: case 0x4EU: {
                const std::optional<i32> value = pop_value(output);
                const u32 local = static_cast<u32>(instruction.opcode - 0x4BU);
                if (local < output.locals.size()) output.locals[local] = value;
                break;
            }
            case 0x84U:
                if (instruction.local_index < output.locals.size()) {
                    auto& value = output.locals[instruction.local_index];
                    if (value.has_value()) {
                        value = wrapping_add(*value, instruction.immediate);
                    }
                }
                break;
            case 0x60U: case 0x64U: case 0x68U: case 0x6CU:
            case 0x70U: case 0x78U: case 0x7AU: case 0x7CU:
            case 0x7EU: case 0x80U: case 0x82U: {
                const auto right = pop_value(output);
                const auto left = pop_value(output);
                output.stack.push_back(left.has_value() && right.has_value()
                    ? fold_binary_integer(instruction.opcode, *left, *right)
                    : std::nullopt);
                break;
            }
            case 0x74U: case 0x91U: case 0x92U: case 0x93U: {
                const auto value = pop_value(output);
                output.stack.push_back(value.has_value()
                    ? fold_unary_integer(instruction.opcode, *value)
                    : std::nullopt);
                break;
            }
            case 0x59U: {
                if (output.stack.empty()) { output.stack.clear(); break; }
                output.stack.push_back(output.stack.back());
                break;
            }
            case 0x5FU: {
                if (output.stack.size() < 2U) { output.stack.clear(); break; }
                std::swap(output.stack[output.stack.size() - 1U],
                          output.stack[output.stack.size() - 2U]);
                break;
            }
            default:
                generic_transfer(output, instruction);
                // Any store of a non-int/non-reference value invalidates that
                // local's integer/null constant fact.
                if (instruction.opcode == 0x37U ||
                    instruction.opcode == 0x38U ||
                    instruction.opcode == 0x39U) {
                    if (instruction.local_index < output.locals.size()) {
                        output.locals[instruction.local_index] = std::nullopt;
                    }
                } else if (instruction.opcode >= 0x3FU &&
                           instruction.opcode <= 0x4AU) {
                    const u32 base = instruction.opcode <= 0x42U
                        ? 0x3FU : (instruction.opcode <= 0x46U ? 0x43U : 0x47U);
                    const u32 local = static_cast<u32>(instruction.opcode - base);
                    if (local < output.locals.size()) {
                        output.locals[local] = std::nullopt;
                    }
                }
                break;
            }
        }

        const auto merge_pc = [&](std::optional<usize> pc) {
            if (!pc.has_value()) return;
            const auto found = index_by_pc.find(*pc);
            if (found != index_by_pc.end()) {
                (void)merge_state(found->second, output);
            }
        };
        if (is_conditional_branch(instruction.opcode)) {
            merge_pc(instruction.branch_target);
            if (index + 1U < instructions.size()) {
                (void)merge_state(index + 1U, output);
            }
        } else if (is_goto(instruction.opcode) || is_jsr(instruction.opcode)) {
            merge_pc(instruction.branch_target);
        } else if (instruction.opcode == 0xAAU || instruction.opcode == 0xABU) {
            merge_pc(instruction.default_target);
            for (const SwitchTarget& target : instruction.switch_targets) {
                merge_pc(target.target_pc);
            }
        } else if (is_ret(instruction.opcode) ||
                   instruction.opcode == 0xACU ||
                   instruction.opcode == 0xADU ||
                   instruction.opcode == 0xAEU ||
                   instruction.opcode == 0xAFU ||
                   instruction.opcode == 0xB0U ||
                   instruction.opcode == 0xB1U ||
                   instruction.opcode == 0xBFU) {
            // No statically known fallthrough.
        } else if (index + 1U < instructions.size()) {
            (void)merge_state(index + 1U, output);
        }
    }
    return states;
}

[[nodiscard]] std::vector<std::vector<u8>> compute_local_liveness(
    const std::vector<DecodedInstruction>& instructions,
    u32 local_slots,
    std::span<const classfile::ExceptionHandler> exception_handlers) {
    if (!exception_handlers.empty() || local_slots == 0U ||
        local_slots > 256U || instructions.size() > 4'096U) {
        return {};
    }
    for (const DecodedInstruction& instruction : instructions) {
        if (is_jsr(instruction.opcode) || is_ret(instruction.opcode)) return {};
    }

    std::unordered_map<usize, usize> index_by_pc;
    index_by_pc.reserve(instructions.size());
    for (usize index = 0U; index < instructions.size(); ++index) {
        index_by_pc.emplace(instructions[index].pc, index);
    }
    std::vector<std::vector<usize>> successors(instructions.size());
    const auto add_successor = [&](usize source, std::optional<usize> pc) {
        if (!pc.has_value()) return;
        const auto found = index_by_pc.find(*pc);
        if (found != index_by_pc.end()) successors[source].push_back(found->second);
    };
    for (usize index = 0U; index < instructions.size(); ++index) {
        const DecodedInstruction& instruction = instructions[index];
        if (is_conditional_branch(instruction.opcode)) {
            add_successor(index, instruction.branch_target);
            if (index + 1U < instructions.size()) successors[index].push_back(index + 1U);
        } else if (is_goto(instruction.opcode)) {
            add_successor(index, instruction.branch_target);
        } else if (instruction.opcode == 0xAAU || instruction.opcode == 0xABU) {
            add_successor(index, instruction.default_target);
            for (const SwitchTarget& target : instruction.switch_targets) {
                add_successor(index, target.target_pc);
            }
        } else if (instruction.opcode == 0xACU ||
                   instruction.opcode == 0xADU ||
                   instruction.opcode == 0xAEU ||
                   instruction.opcode == 0xAFU ||
                   instruction.opcode == 0xB0U ||
                   instruction.opcode == 0xB1U ||
                   instruction.opcode == 0xBFU) {
            continue;
        } else if (index + 1U < instructions.size()) {
            successors[index].push_back(index + 1U);
        }
    }

    std::vector<std::vector<u8>> uses(
        instructions.size(), std::vector<u8>(local_slots, 0U));
    std::vector<std::vector<u8>> defs(
        instructions.size(), std::vector<u8>(local_slots, 0U));
    const auto mark = [local_slots](std::vector<u8>& slots,
                                    u32 local,
                                    bool wide) {
        if (local < local_slots) slots[local] = 1U;
        if (wide && local + 1U < local_slots) slots[local + 1U] = 1U;
    };
    for (usize index = 0U; index < instructions.size(); ++index) {
        const DecodedInstruction& instruction = instructions[index];
        switch (instruction.opcode) {
        case 0x15U: case 0x17U: case 0x19U:
            mark(uses[index], instruction.local_index, false); break;
        case 0x16U: case 0x18U:
            mark(uses[index], instruction.local_index, true); break;
        case 0x1AU: case 0x1BU: case 0x1CU: case 0x1DU:
            mark(uses[index], static_cast<u32>(instruction.opcode - 0x1AU), false); break;
        case 0x1EU: case 0x1FU: case 0x20U: case 0x21U:
            mark(uses[index], static_cast<u32>(instruction.opcode - 0x1EU), true); break;
        case 0x22U: case 0x23U: case 0x24U: case 0x25U:
            mark(uses[index], static_cast<u32>(instruction.opcode - 0x22U), false); break;
        case 0x26U: case 0x27U: case 0x28U: case 0x29U:
            mark(uses[index], static_cast<u32>(instruction.opcode - 0x26U), true); break;
        case 0x2AU: case 0x2BU: case 0x2CU: case 0x2DU:
            mark(uses[index], static_cast<u32>(instruction.opcode - 0x2AU), false); break;
        case 0x36U: case 0x38U: case 0x3AU:
            mark(defs[index], instruction.local_index, false); break;
        case 0x37U: case 0x39U:
            mark(defs[index], instruction.local_index, true); break;
        case 0x3BU: case 0x3CU: case 0x3DU: case 0x3EU:
            mark(defs[index], static_cast<u32>(instruction.opcode - 0x3BU), false); break;
        case 0x3FU: case 0x40U: case 0x41U: case 0x42U:
            mark(defs[index], static_cast<u32>(instruction.opcode - 0x3FU), true); break;
        case 0x43U: case 0x44U: case 0x45U: case 0x46U:
            mark(defs[index], static_cast<u32>(instruction.opcode - 0x43U), false); break;
        case 0x47U: case 0x48U: case 0x49U: case 0x4AU:
            mark(defs[index], static_cast<u32>(instruction.opcode - 0x47U), true); break;
        case 0x4BU: case 0x4CU: case 0x4DU: case 0x4EU:
            mark(defs[index], static_cast<u32>(instruction.opcode - 0x4BU), false); break;
        case 0x84U:
            mark(uses[index], instruction.local_index, false);
            mark(defs[index], instruction.local_index, false);
            break;
        default:
            break;
        }
    }

    std::vector<std::vector<u8>> live_in(
        instructions.size(), std::vector<u8>(local_slots, 0U));
    std::vector<std::vector<u8>> live_out(
        instructions.size(), std::vector<u8>(local_slots, 0U));
    bool changed = true;
    while (changed) {
        changed = false;
        for (usize reverse = instructions.size(); reverse > 0U; --reverse) {
            const usize index = reverse - 1U;
            std::vector<u8> next_out(local_slots, 0U);
            for (const usize successor : successors[index]) {
                for (u32 local = 0U; local < local_slots; ++local) {
                    next_out[local] = static_cast<u8>(
                        next_out[local] | live_in[successor][local]);
                }
            }
            std::vector<u8> next_in = next_out;
            for (u32 local = 0U; local < local_slots; ++local) {
                if (defs[index][local] != 0U) next_in[local] = 0U;
                if (uses[index][local] != 0U) next_in[local] = 1U;
            }
            if (next_out != live_out[index] || next_in != live_in[index]) {
                live_out[index] = std::move(next_out);
                live_in[index] = std::move(next_in);
                changed = true;
            }
        }
    }
    return live_out;
}

[[nodiscard]] OptimizationPlan build_optimization_plan(
    const std::vector<DecodedInstruction>& instructions,
    const std::vector<std::optional<u32>>& depths,
    u32 local_slots,
    JavaTypeKind return_kind,
    std::span<const classfile::ExceptionHandler> exception_handlers,
    bool enable_expensive_optimizations) {
    OptimizationPlan plan;
    plan.instructions.resize(instructions.size());
    if (instructions.empty()) return plan;

    std::unordered_map<usize, usize> index_by_pc;
    index_by_pc.reserve(instructions.size());
    for (usize index = 0; index < instructions.size(); ++index) {
        index_by_pc.emplace(instructions[index].pc, index);
    }

    std::vector<bool> block_boundary(instructions.size(), false);
    block_boundary[0] = true;
    const auto mark_target = [&](std::optional<usize> target) {
        if (!target.has_value()) return;
        const auto found = index_by_pc.find(*target);
        if (found != index_by_pc.end()) block_boundary[found->second] = true;
    };
    for (const classfile::ExceptionHandler& handler : exception_handlers) {
        mark_target(static_cast<usize>(handler.handler_pc));
    }

    for (usize index = 0; index < instructions.size(); ++index) {
        const DecodedInstruction& instruction = instructions[index];
        mark_target(instruction.branch_target);
        mark_target(instruction.default_target);
        for (const SwitchTarget& target : instruction.switch_targets) {
            mark_target(target.target_pc);
            if (target.target_pc <= instruction.pc) plan.contains_loop = true;
        }
        if (instruction.branch_target.has_value() &&
            *instruction.branch_target <= instruction.pc) {
            plan.contains_loop = true;
        }
        if (index + 1U < instructions.size() &&
            (is_conditional_branch(instruction.opcode) ||
             is_goto(instruction.opcode) || is_jsr(instruction.opcode) ||
             is_ret(instruction.opcode) ||
             instruction.opcode == 0xAAU || instruction.opcode == 0xABU ||
             instruction.opcode == 0xACU || instruction.opcode == 0xADU ||
             instruction.opcode == 0xAEU || instruction.opcode == 0xAFU ||
             instruction.opcode == 0xB0U || instruction.opcode == 0xB1U ||
             instruction.opcode == 0xBFU)) {
            block_boundary[index + 1U] = true;
        }
    }

    plan.block_budget_costs.assign(instructions.size(), 0U);
    u64 reachable_instructions = 0U;
    u64 emitted_budget_guards = 0U;
    for (const auto& depth : depths) {
        if (depth.has_value()) ++reachable_instructions;
    }
    for (usize start = 0; start < instructions.size(); ++start) {
        if (!block_boundary[start] || !depths[start].has_value()) continue;
        u32 cost = 0U;
        for (usize cursor = start;
             cursor < instructions.size() &&
             (cursor == start || !block_boundary[cursor]);
             ++cursor) {
            if (depths[cursor].has_value()) ++cost;
        }
        plan.block_budget_costs[start] = cost;
        emitted_budget_guards +=
            (static_cast<u64>(cost) + 4'094U) / 4'095U;
    }
    if (reachable_instructions > emitted_budget_guards) {
        plan.budget_checks_elided =
            reachable_instructions - emitted_budget_guards;
    }

    // Physical iPhone defaults to a thermal-first quick tier. The structural
    // information above is required by code generation and budget accounting,
    // but the analyses below (constant-flow/liveness, GVN/CSE, LICM, array
    // range proofs, scalar replacement, etc.) are compile-time work whose
    // payoff is often too small for short J2ME methods. Keep them for the
    // optimizing tier on desktop/simulator and the explicit aggressive iOS
    // mode while allowing the device tier to reach native code with one cheap
    // decode/CFG pass.
    if (!enable_expensive_optimizations) return plan;

    const auto constant_flow = compute_constant_flow(
        instructions,
        depths,
        local_slots,
        return_kind,
        exception_handlers);
    const auto local_live_out = compute_local_liveness(
        instructions, local_slots, exception_handlers);
    std::vector<std::optional<i32>> locals(local_slots);
    std::vector<std::optional<i32>> stack;
    for (usize index = 0; index < instructions.size(); ++index) {
        if (!depths[index].has_value()) continue;
        const u32 expected_depth = *depths[index];
        if (index < constant_flow.size() && constant_flow[index].has_value()) {
            locals = constant_flow[index]->locals;
            stack = constant_flow[index]->stack;
        } else {
            locals.assign(local_slots, std::nullopt);
            stack.assign(expected_depth, std::nullopt);
        }

        InstructionOptimization& optimization = plan.instructions[index];
        const DecodedInstruction& instruction = instructions[index];
        if (!local_live_out.empty()) {
            std::optional<u32> integer_definition;
            if (instruction.opcode == 0x36U || instruction.opcode == 0x84U) {
                integer_definition = instruction.local_index;
            } else if (instruction.opcode >= 0x3BU &&
                       instruction.opcode <= 0x3EU) {
                integer_definition = static_cast<u32>(instruction.opcode - 0x3BU);
            }
            if (integer_definition.has_value() &&
                *integer_definition < local_live_out[index].size() &&
                local_live_out[index][*integer_definition] == 0U) {
                optimization.dead_local_write = true;
                ++plan.dead_local_writes_eliminated;
            }
        }
        const auto pop_value = [&]() -> std::optional<i32> {
            if (stack.empty()) return std::nullopt;
            std::optional<i32> value = stack.back();
            stack.pop_back();
            return value;
        };
        const auto push_value = [&](std::optional<i32> value) {
            stack.push_back(value);
        };

        if (const auto constant = pushed_constant(instruction);
            constant.has_value()) {
            push_value(*constant);
            continue;
        }

        switch (instruction.opcode) {
        case 0x00U:
            break;
        case 0x0BU:
        case 0x0CU:
        case 0x0DU:
            push_value(std::nullopt);
            break;
        case 0x0EU:
        case 0x0FU:
        case 0x09U:
        case 0x0AU:
        case 0x14U:
            push_value(std::nullopt);
            push_value(std::nullopt);
            break;
        case 0x12U:
        case 0x13U:
            if (instruction.value_kind == JavaTypeKind::float32) {
                push_value(std::nullopt);
            }
            break;
        case 0x15U:
        case 0x1AU:
        case 0x1BU:
        case 0x1CU:
        case 0x1DU: {
            const u32 local = instruction.opcode == 0x15U
                ? instruction.local_index
                : static_cast<u32>(instruction.opcode - 0x1AU);
            const std::optional<i32> value = local < locals.size()
                ? locals[local]
                : std::nullopt;
            optimization.folded_result = value;
            if (value.has_value()) ++plan.propagated_constants;
            push_value(value);
            break;
        }
        case 0x17U:
        case 0x22U:
        case 0x23U:
        case 0x24U:
        case 0x25U:
            push_value(std::nullopt);
            break;
        case 0x16U:
        case 0x18U:
        case 0x1EU:
        case 0x1FU:
        case 0x20U:
        case 0x21U:
        case 0x26U:
        case 0x27U:
        case 0x28U:
        case 0x29U:
            push_value(std::nullopt);
            push_value(std::nullopt);
            break;
        case 0x19U:
        case 0x2AU:
        case 0x2BU:
        case 0x2CU:
        case 0x2DU: {
            const u32 local = instruction.opcode == 0x19U
                ? instruction.local_index
                : static_cast<u32>(instruction.opcode - 0x2AU);
            push_value(local < locals.size() ? locals[local] : std::nullopt);
            break;
        }
        case 0x2EU:
        case 0x30U:
        case 0x32U:
        case 0x33U:
        case 0x34U:
        case 0x35U:
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            break;
        case 0x2FU:
        case 0x31U:
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            push_value(std::nullopt);
            break;
        case 0x4FU:
        case 0x51U:
        case 0x53U:
        case 0x54U:
        case 0x55U:
        case 0x56U:
            (void)pop_value();
            (void)pop_value();
            (void)pop_value();
            break;
        case 0x50U:
        case 0x52U:
            (void)pop_value();
            (void)pop_value();
            (void)pop_value();
            (void)pop_value();
            break;
        case 0xB2U:
            push_value(std::nullopt);
            if (jit_slot_count(instruction.value_kind) == 2U) {
                push_value(std::nullopt);
            }
            break;
        case 0xB3U:
            (void)pop_value();
            if (jit_slot_count(instruction.value_kind) == 2U) {
                (void)pop_value();
            }
            break;
        case 0xB4U:
            (void)pop_value();
            push_value(std::nullopt);
            if (jit_slot_count(instruction.value_kind) == 2U) {
                push_value(std::nullopt);
            }
            break;
        case 0xB5U:
            (void)pop_value();
            if (jit_slot_count(instruction.value_kind) == 2U) {
                (void)pop_value();
            }
            (void)pop_value();
            break;
        case 0xB6U:
        case 0xB7U:
        case 0xB8U:
        case 0xB9U:
        case 0xBAU: {
            const u32 consumed_slots = instruction.argument_slots +
                (instruction.has_receiver ? 1U : 0U);
            for (u32 slot = 0U; slot < consumed_slots; ++slot) {
                (void)pop_value();
            }
            if (instruction.value_kind != JavaTypeKind::void_type) {
                push_value(std::nullopt);
                if (jit_slot_count(instruction.value_kind) == 2U) {
                    push_value(std::nullopt);
                }
            }
            break;
        }
        case 0xBBU:
            push_value(std::nullopt);
            break;
        case 0xC5U:
            for (u32 dimension = 0U;
                 dimension < instruction.argument_slots;
                 ++dimension) {
                (void)pop_value();
            }
            push_value(std::nullopt);
            break;
        case 0xBCU:
        case 0xBDU:
        case 0xC0U:
        case 0xC1U:
            (void)pop_value();
            push_value(std::nullopt);
            break;
        case 0xBEU:
            (void)pop_value();
            push_value(std::nullopt);
            break;
        case 0xC2U:
        case 0xC3U:
            (void)pop_value();
            break;
        case 0x38U:
        case 0x43U:
        case 0x44U:
        case 0x45U:
        case 0x46U:
            (void)pop_value();
            break;
        case 0x36U:
        case 0x3BU:
        case 0x3CU:
        case 0x3DU:
        case 0x3EU: {
            const u32 local = instruction.opcode == 0x36U
                ? instruction.local_index
                : static_cast<u32>(instruction.opcode - 0x3BU);
            const std::optional<i32> value = pop_value();
            if (local < locals.size()) locals[local] = value;
            break;
        }
        case 0x39U:
        case 0x47U:
        case 0x48U:
        case 0x49U:
        case 0x4AU:
            (void)pop_value();
            (void)pop_value();
            break;
        case 0x37U:
        case 0x3FU:
        case 0x40U:
        case 0x41U:
        case 0x42U: {
            (void)pop_value();
            (void)pop_value();
            const u32 local = instruction.opcode == 0x37U
                ? instruction.local_index
                : static_cast<u32>(instruction.opcode - 0x3FU);
            if (local < locals.size()) locals[local] = std::nullopt;
            if (local + 1U < locals.size()) locals[local + 1U] = std::nullopt;
            break;
        }
        case 0x3AU:
        case 0x4BU:
        case 0x4CU:
        case 0x4DU:
        case 0x4EU: {
            const u32 local = instruction.opcode == 0x3AU
                ? instruction.local_index
                : static_cast<u32>(instruction.opcode - 0x4BU);
            const std::optional<i32> value = pop_value();
            if (local < locals.size()) locals[local] = value;
            break;
        }
        case 0x57U:
            (void)pop_value();
            break;
        case 0x58U:
            (void)pop_value();
            (void)pop_value();
            break;
        case 0x59U: {
            const std::optional<i32> first = pop_value();
            push_value(first);
            push_value(first);
            break;
        }
        case 0x5AU: {
            const std::optional<i32> first = pop_value();
            const std::optional<i32> second = pop_value();
            push_value(first);
            push_value(second);
            push_value(first);
            break;
        }
        case 0x5BU: {
            const std::optional<i32> first = pop_value();
            const std::optional<i32> second = pop_value();
            const std::optional<i32> third = pop_value();
            push_value(first);
            push_value(third);
            push_value(second);
            push_value(first);
            break;
        }
        case 0x5CU: {
            const std::optional<i32> first = pop_value();
            const std::optional<i32> second = pop_value();
            push_value(second);
            push_value(first);
            push_value(second);
            push_value(first);
            break;
        }
        case 0x5DU: {
            const std::optional<i32> first = pop_value();
            const std::optional<i32> second = pop_value();
            const std::optional<i32> third = pop_value();
            push_value(second);
            push_value(first);
            push_value(third);
            push_value(second);
            push_value(first);
            break;
        }
        case 0x5EU: {
            const std::optional<i32> first = pop_value();
            const std::optional<i32> second = pop_value();
            const std::optional<i32> third = pop_value();
            const std::optional<i32> fourth = pop_value();
            push_value(second);
            push_value(first);
            push_value(fourth);
            push_value(third);
            push_value(second);
            push_value(first);
            break;
        }
        case 0x5FU: {
            const std::optional<i32> first = pop_value();
            const std::optional<i32> second = pop_value();
            push_value(first);
            push_value(second);
            break;
        }
        case 0x60U:
        case 0x64U:
        case 0x68U:
        case 0x6CU:
        case 0x70U:
        case 0x78U:
        case 0x7AU:
        case 0x7CU:
        case 0x7EU:
        case 0x80U:
        case 0x82U: {
            const std::optional<i32> right = pop_value();
            const std::optional<i32> left = pop_value();
            optimization.right_constant = right;
            if (left.has_value() && right.has_value()) {
                optimization.folded_result = fold_binary_integer(
                    instruction.opcode, *left, *right);
                if (optimization.folded_result.has_value()) {
                    ++plan.folded_operations;
                }
            }
            push_value(optimization.folded_result);
            break;
        }
        case 0x62U:
        case 0x66U:
        case 0x6AU:
        case 0x6EU:
        case 0x72U:
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            break;
        case 0x63U:
        case 0x67U:
        case 0x6BU:
        case 0x6FU:
        case 0x73U:
            (void)pop_value();
            (void)pop_value();
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            push_value(std::nullopt);
            break;
        case 0x61U:
        case 0x65U:
        case 0x69U:
        case 0x6DU:
        case 0x71U:
        case 0x7FU:
        case 0x81U:
        case 0x83U:
            (void)pop_value();
            (void)pop_value();
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            push_value(std::nullopt);
            break;
        case 0x79U:
        case 0x7BU:
        case 0x7DU:
            (void)pop_value();
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            push_value(std::nullopt);
            break;
        case 0x76U:
            (void)pop_value();
            push_value(std::nullopt);
            break;
        case 0x75U:
        case 0x77U:
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            push_value(std::nullopt);
            break;
        case 0x86U:
        case 0x8BU:
            (void)pop_value();
            push_value(std::nullopt);
            break;
        case 0x87U:
        case 0x8CU:
        case 0x8DU:
            (void)pop_value();
            push_value(std::nullopt);
            push_value(std::nullopt);
            break;
        case 0x89U:
        case 0x8EU:
        case 0x90U:
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            break;
        case 0x8AU:
        case 0x8FU:
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            push_value(std::nullopt);
            break;
        case 0x95U:
        case 0x96U:
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            break;
        case 0x97U:
        case 0x98U:
            (void)pop_value();
            (void)pop_value();
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            break;
        case 0x85U:
            (void)pop_value();
            push_value(std::nullopt);
            push_value(std::nullopt);
            break;
        case 0x88U:
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            break;
        case 0x94U:
            (void)pop_value();
            (void)pop_value();
            (void)pop_value();
            (void)pop_value();
            push_value(std::nullopt);
            break;
        case 0x74U:
        case 0x91U:
        case 0x92U:
        case 0x93U: {
            const std::optional<i32> value = pop_value();
            if (value.has_value()) {
                optimization.folded_result = fold_unary_integer(
                    instruction.opcode, *value);
                if (optimization.folded_result.has_value()) {
                    ++plan.folded_operations;
                }
            }
            push_value(optimization.folded_result);
            break;
        }
        case 0x84U:
            if (instruction.local_index < locals.size() &&
                locals[instruction.local_index].has_value()) {
                locals[instruction.local_index] = wrapping_add(
                    *locals[instruction.local_index], instruction.immediate);
                ++plan.propagated_constants;
            } else if (instruction.local_index < locals.size()) {
                locals[instruction.local_index] = std::nullopt;
            }
            break;
        case 0x99U:
        case 0x9AU:
        case 0x9BU:
        case 0x9CU:
        case 0x9DU:
        case 0x9EU: {
            const std::optional<i32> value = pop_value();
            if (value.has_value()) {
                optimization.branch_taken = evaluate_integer_branch(
                    instruction.opcode, *value);
                ++plan.folded_branches;
            }
            break;
        }
        case 0x9FU:
        case 0xA0U:
        case 0xA1U:
        case 0xA2U:
        case 0xA3U:
        case 0xA4U: {
            const std::optional<i32> right = pop_value();
            const std::optional<i32> left = pop_value();
            if (left.has_value() && right.has_value()) {
                optimization.branch_taken = evaluate_integer_branch(
                    instruction.opcode, *left, *right);
                ++plan.folded_branches;
            }
            break;
        }
        case 0xA5U:
        case 0xA6U: {
            const std::optional<i32> right = pop_value();
            const std::optional<i32> left = pop_value();
            if (left.has_value() && right.has_value()) {
                optimization.branch_taken = instruction.opcode == 0xA5U
                    ? *left == *right
                    : *left != *right;
                ++plan.folded_branches;
            }
            break;
        }
        case 0xC6U:
        case 0xC7U: {
            const std::optional<i32> value = pop_value();
            if (value.has_value()) {
                optimization.branch_taken = instruction.opcode == 0xC6U
                    ? *value == 0
                    : *value != 0;
                ++plan.folded_branches;
            }
            break;
        }
        case 0xAAU:
        case 0xABU: {
            const std::optional<i32> key = pop_value();
            if (key.has_value() && instruction.default_target.has_value()) {
                usize target_pc = *instruction.default_target;
                for (const SwitchTarget& target : instruction.switch_targets) {
                    if (target.key == *key) {
                        target_pc = target.target_pc;
                        break;
                    }
                }
                optimization.forced_target = target_pc;
                ++plan.folded_branches;
            }
            break;
        }
        case 0xA8U:
        case 0xC9U:
            push_value(std::nullopt);
            break;
        case 0xA7U:
        case 0xA9U:
        case 0xC8U:
        case 0xB1U:
            break;
        case 0xACU:
        case 0xAEU:
        case 0xB0U:
        case 0xBFU:
            (void)pop_value();
            break;
        case 0xADU:
        case 0xAFU:
            (void)pop_value();
            (void)pop_value();
            break;
        default:
            break;
        }
    }

    // A constant immediately consumed as the right operand never needs a
    // native stack store: the optimized consumer materializes it directly.
    for (usize index = 0; index + 1U < instructions.size(); ++index) {
        const auto constant = pushed_constant(instructions[index]);
        const InstructionOptimization& consumer = plan.instructions[index + 1U];
        if (constant.has_value() && consumer.right_constant.has_value() &&
            *constant == *consumer.right_constant &&
            !block_boundary[index + 1U]) {
            plan.instructions[index].elide_push = true;
        }
    }

    // Local GVN/CSE for the common Java bytecode shape
    //   iload a; iload b; <pure integer op>
    // within a basic block. Local versions make the value number invalid as
    // soon as either input is written. The first value is retained in x27/x28
    // (callee-saved); later occurrences skip both input loads and the ALU op.
    struct GvnKey final {
        u8 opcode{0U};
        u32 left_local{0U};
        u32 left_version{0U};
        u32 right_local{0U};
        u32 right_version{0U};
        bool operator==(const GvnKey&) const noexcept = default;
    };
    struct GvnKeyHash final {
        usize operator()(const GvnKey& key) const noexcept {
            usize value = std::hash<u32>{}(
                static_cast<u32>(key.opcode) |
                (key.left_local << 8U));
            value ^= std::hash<u32>{}(key.left_version + 0x9E37'79B9U) +
                     (value << 6U) + (value >> 2U);
            value ^= std::hash<u32>{}(key.right_local + 0x85EB'CA6BU) +
                     (value << 6U) + (value >> 2U);
            value ^= std::hash<u32>{}(key.right_version + 0xC2B2'AE35U) +
                     (value << 6U) + (value >> 2U);
            return value;
        }
    };
    struct GvnRecord final {
        usize first_operation{0U};
        std::optional<u32> register_index;
    };
    const auto int_load_local = [](const DecodedInstruction& instruction)
        -> std::optional<u32> {
        if (instruction.opcode == 0x15U) return instruction.local_index;
        if (instruction.opcode >= 0x1AU && instruction.opcode <= 0x1DU) {
            return static_cast<u32>(instruction.opcode - 0x1AU);
        }
        return std::nullopt;
    };
    const auto gvn_opcode = [](u8 opcode) noexcept {
        switch (opcode) {
        case 0x60U: // iadd
        case 0x64U: // isub
        case 0x68U: // imul
        case 0x78U: // ishl
        case 0x7AU: // ishr
        case 0x7CU: // iushr
        case 0x7EU: // iand
        case 0x80U: // ior
        case 0x82U: // ixor
            return true;
        default:
            return false;
        }
    };
    const auto commutative_gvn_opcode = [](u8 opcode) noexcept {
        return opcode == 0x60U || opcode == 0x68U || opcode == 0x7EU ||
               opcode == 0x80U || opcode == 0x82U;
    };

    // True loop-invariant code motion. For now we deliberately restrict this
    // to pure integer expressions fed directly by two locals and to methods
    // without exception handlers. A natural-loop backedge defines the loop;
    // if neither operand local is written anywhere in that loop, compute the
    // expression once at the loop header and keep it in a callee-saved value
    // register. Backedges branch past the hoisted preheader, while OSR enters
    // through it so the invariant register is always initialized.
    if (exception_handlers.empty() && plan.contains_loop &&
        !kGvnRegisters.empty()) {
        std::unordered_map<usize, usize> loop_index_by_pc;
        loop_index_by_pc.reserve(instructions.size());
        for (usize index = 0U; index < instructions.size(); ++index) {
            loop_index_by_pc.emplace(instructions[index].pc, index);
        }
        const auto mark_written_local = [local_slots](
            std::vector<u8>& written,
            const DecodedInstruction& instruction) {
            std::optional<u32> local;
            bool wide = false;
            switch (instruction.opcode) {
            case 0x36U: case 0x38U: case 0x3AU:
                local = instruction.local_index; break;
            case 0x37U: case 0x39U:
                local = instruction.local_index; wide = true; break;
            case 0x3BU: case 0x3CU: case 0x3DU: case 0x3EU:
                local = static_cast<u32>(instruction.opcode - 0x3BU); break;
            case 0x3FU: case 0x40U: case 0x41U: case 0x42U:
                local = static_cast<u32>(instruction.opcode - 0x3FU);
                wide = true; break;
            case 0x43U: case 0x44U: case 0x45U: case 0x46U:
                local = static_cast<u32>(instruction.opcode - 0x43U); break;
            case 0x47U: case 0x48U: case 0x49U: case 0x4AU:
                local = static_cast<u32>(instruction.opcode - 0x47U);
                wide = true; break;
            case 0x4BU: case 0x4CU: case 0x4DU: case 0x4EU:
                local = static_cast<u32>(instruction.opcode - 0x4BU); break;
            case 0x84U:
                local = instruction.local_index; break;
            default:
                break;
            }
            if (!local.has_value() || *local >= local_slots) return;
            written[*local] = 1U;
            if (wide && *local + 1U < local_slots) written[*local + 1U] = 1U;
        };

        bool selected = false;
        for (usize source = 0U; source < instructions.size() && !selected;
             ++source) {
            const auto consider_loop = [&](std::optional<usize> target_pc) {
                if (selected || !target_pc.has_value() ||
                    *target_pc > instructions[source].pc) {
                    return;
                }
                const auto target_found = loop_index_by_pc.find(*target_pc);
                if (target_found == loop_index_by_pc.end() ||
                    target_found->second >= source) {
                    return;
                }
                const usize target = target_found->second;
                std::vector<u8> written(local_slots, 0U);
                for (usize index = target; index <= source; ++index) {
                    mark_written_local(written, instructions[index]);
                }
                for (usize index = target + 2U; index <= source; ++index) {
                    if (!gvn_opcode(instructions[index].opcode) ||
                        block_boundary[index] || block_boundary[index - 1U] ||
                        plan.instructions[index].folded_result.has_value() ||
                        plan.instructions[index].right_constant.has_value()) {
                        continue;
                    }
                    const auto left = int_load_local(instructions[index - 2U]);
                    const auto right = int_load_local(instructions[index - 1U]);
                    if (!left.has_value() || !right.has_value() ||
                        *left >= local_slots || *right >= local_slots ||
                        written[*left] != 0U || written[*right] != 0U) {
                        continue;
                    }
                    const u32 reg = kGvnRegisters[0U];
                    plan.licm_expressions.push_back(LicmExpression {
                        .loop_header_pc = instructions[target].pc,
                        .loop_end_pc = instructions[source].pc,
                        .opcode = instructions[index].opcode,
                        .left_local = *left,
                        .right_local = *right,
                        .register_index = reg,
                    });
                    plan.instructions[index - 2U].licm_elide_input_push = true;
                    plan.instructions[index - 1U].licm_elide_input_push = true;
                    plan.instructions[index].licm_reuse_register = reg;
                    plan.licm_register_count = 1U;
                    plan.gvn_register_count = 1U;
                    ++plan.loop_invariants_hoisted;
                    selected = true;
                    return;
                }
            };
            consider_loop(instructions[source].branch_target);
            if (!selected) {
                consider_loop(instructions[source].default_target);
            }
            if (!selected) {
                for (const SwitchTarget& target :
                     instructions[source].switch_targets) {
                    consider_loop(target.target_pc);
                    if (selected) break;
                }
            }
        }
    }

    // Counted-loop range analysis for fixed-size Java arrays. The initial
    // implementation intentionally recognizes only the strongest canonical
    // proof: array.length is captured before the loop, index starts at zero,
    // the loop header exits on index >= length, and index advances only by
    // iinc +1. Under these conditions aload[index] is provably non-null,
    // in-range and of the verifier-known array kind for every loop iteration.
    if (exception_handlers.empty() && plan.contains_loop &&
        plan.array_leases.empty()) {
        const auto aload_local = [](const DecodedInstruction& instruction)
            -> std::optional<u32> {
            if (instruction.opcode == 0x19U) return instruction.local_index;
            if (instruction.opcode >= 0x2AU && instruction.opcode <= 0x2DU) {
                return static_cast<u32>(instruction.opcode - 0x2AU);
            }
            return std::nullopt;
        };
        const auto int_store_local = [](const DecodedInstruction& instruction)
            -> std::optional<u32> {
            if (instruction.opcode == 0x36U) return instruction.local_index;
            if (instruction.opcode >= 0x3BU && instruction.opcode <= 0x3EU) {
                return static_cast<u32>(instruction.opcode - 0x3BU);
            }
            return std::nullopt;
        };
        const auto ref_store_local = [](const DecodedInstruction& instruction)
            -> std::optional<u32> {
            if (instruction.opcode == 0x3AU) return instruction.local_index;
            if (instruction.opcode >= 0x4BU && instruction.opcode <= 0x4EU) {
                return static_cast<u32>(instruction.opcode - 0x4BU);
            }
            return std::nullopt;
        };
        const auto writes_local = [&](const DecodedInstruction& instruction,
                                      u32 local) noexcept {
            if (const auto stored = int_store_local(instruction);
                stored.has_value() && *stored == local) {
                return true;
            }
            if (const auto stored = ref_store_local(instruction);
                stored.has_value() && *stored == local) {
                return true;
            }
            if (instruction.opcode == 0x37U || instruction.opcode == 0x38U ||
                instruction.opcode == 0x39U) {
                return instruction.local_index == local;
            }
            if (instruction.opcode >= 0x3FU && instruction.opcode <= 0x4AU) {
                const u32 base = instruction.opcode <= 0x42U
                    ? 0x3FU
                    : (instruction.opcode <= 0x46U ? 0x43U : 0x47U);
                return static_cast<u32>(instruction.opcode - base) == local;
            }
            return instruction.opcode == 0x84U &&
                   instruction.local_index == local;
        };
        const auto is_array_load = [](u8 opcode) noexcept {
            // Narrow byte/boolean/char/short arrays stay on the runtime fast
            // path for now. int/float leases use a 4-byte stride while
            // long/double/reference leases retain the 8-byte stride.
            return opcode >= 0x2EU && opcode <= 0x32U;
        };
        const auto primitive_store_lease_opcode = [](u8 opcode)
            -> std::optional<u8> {
            switch (opcode) {
            case 0x4FU: return 0x2EU; // iastore -> iaload kind
            case 0x50U: return 0x2FU; // lastore -> laload kind
            case 0x51U: return 0x30U; // fastore -> faload kind
            case 0x52U: return 0x31U; // dastore -> daload kind
            default: return std::nullopt;
            }
        };

        std::unordered_map<usize, usize> array_index_by_pc;
        array_index_by_pc.reserve(instructions.size());
        for (usize index = 0U; index < instructions.size(); ++index) {
            array_index_by_pc.emplace(instructions[index].pc, index);
        }

        bool selected_array_loop = false;
        for (usize source = 0U;
             source < instructions.size() && !selected_array_loop;
             ++source) {
            const DecodedInstruction& backedge = instructions[source];
            if (!is_goto(backedge.opcode) || !backedge.branch_target.has_value() ||
                *backedge.branch_target >= backedge.pc) {
                continue;
            }
            const auto header_found = array_index_by_pc.find(
                *backedge.branch_target);
            if (header_found == array_index_by_pc.end()) continue;
            const usize header = header_found->second;
            if (header + 2U >= instructions.size() || header >= source) continue;

            const auto index_local = int_load_local(instructions[header]);
            const auto length_local = int_load_local(instructions[header + 1U]);
            if (!index_local.has_value() || !length_local.has_value() ||
                instructions[header + 2U].opcode != 0xA2U ||
                !instructions[header + 2U].branch_target.has_value() ||
                *instructions[header + 2U].branch_target <= backedge.pc) {
                continue;
            }

            // No control-flow edge from outside the loop may enter the header
            // or body directly; otherwise it could bypass the arraylength and
            // zero-index preconditions.
            bool external_entry = false;
            const usize header_pc = instructions[header].pc;
            const usize end_pc = backedge.pc;
            const auto target_enters_loop = [&](std::optional<usize> pc) {
                return pc.has_value() && *pc >= header_pc && *pc <= end_pc;
            };
            for (usize index = 0U; index < instructions.size(); ++index) {
                if (index >= header && index <= source) continue;
                const DecodedInstruction& candidate = instructions[index];
                if (target_enters_loop(candidate.branch_target) ||
                    target_enters_loop(candidate.default_target)) {
                    external_entry = true;
                    break;
                }
                for (const SwitchTarget& target : candidate.switch_targets) {
                    if (target.target_pc >= header_pc &&
                        target.target_pc <= end_pc) {
                        external_entry = true;
                        break;
                    }
                }
                if (external_entry) break;
            }
            if (external_entry) continue;

            // Find `aload array; arraylength; istore length` before the loop.
            std::optional<usize> length_setup;
            std::optional<u32> array_local;
            for (usize index = 2U; index < header; ++index) {
                if (instructions[index - 1U].opcode != 0xBEU) continue;
                const auto loaded_array = aload_local(instructions[index - 2U]);
                const auto stored_length = int_store_local(instructions[index]);
                if (loaded_array.has_value() && stored_length.has_value() &&
                    *stored_length == *length_local) {
                    length_setup = index;
                    array_local = *loaded_array;
                }
            }
            if (!length_setup.has_value() || !array_local.has_value()) continue;

            // Find the latest `iconst_0; istore index` after the length setup.
            std::optional<usize> index_setup;
            for (usize index = *length_setup + 1U; index < header; ++index) {
                const auto stored_index = int_store_local(instructions[index]);
                if (!stored_index.has_value() || *stored_index != *index_local ||
                    index == 0U) {
                    continue;
                }
                const auto constant = pushed_constant(instructions[index - 1U]);
                if (constant.has_value() && *constant == 0) {
                    index_setup = index;
                }
            }
            if (!index_setup.has_value()) continue;

            bool invalidated = false;
            for (usize index = *length_setup + 1U; index < header; ++index) {
                if (index != *index_setup &&
                    (writes_local(instructions[index], *array_local) ||
                     writes_local(instructions[index], *length_local) ||
                     writes_local(instructions[index], *index_local))) {
                    invalidated = true;
                    break;
                }
            }
            if (invalidated) continue;

            u32 index_increments = 0U;
            for (usize index = header; index <= source; ++index) {
                const DecodedInstruction& candidate = instructions[index];
                if (writes_local(candidate, *array_local) ||
                    writes_local(candidate, *length_local)) {
                    invalidated = true;
                    break;
                }
                if (candidate.opcode == 0x84U &&
                    candidate.local_index == *index_local) {
                    if (candidate.immediate != 1) {
                        invalidated = true;
                        break;
                    }
                    ++index_increments;
                } else if (writes_local(candidate, *index_local)) {
                    invalidated = true;
                    break;
                }
            }
            if (invalidated || index_increments != 1U) continue;

            std::optional<u8> lease_opcode;
            std::vector<usize> matching_loads;
            std::vector<usize> matching_stores;
            const auto accept_lease_opcode = [&](u8 candidate) {
                if (lease_opcode.has_value() && *lease_opcode != candidate) {
                    invalidated = true;
                    return false;
                }
                lease_opcode = candidate;
                return true;
            };
            for (usize index = header + 2U; index <= source; ++index) {
                if (is_array_load(instructions[index].opcode) && index >= 2U) {
                    const auto loaded_array = aload_local(instructions[index - 2U]);
                    const auto loaded_index = int_load_local(instructions[index - 1U]);
                    if (!loaded_array.has_value() || !loaded_index.has_value() ||
                        *loaded_array != *array_local ||
                        *loaded_index != *index_local) {
                        continue;
                    }
                    if (!accept_lease_opcode(instructions[index].opcode)) break;
                    matching_loads.push_back(index);
                    continue;
                }

                const auto store_lease_opcode =
                    primitive_store_lease_opcode(instructions[index].opcode);
                if (!store_lease_opcode.has_value() || index < 3U ||
                    block_boundary[index - 2U] || block_boundary[index - 1U] ||
                    block_boundary[index]) {
                    continue;
                }
                const auto stored_array = aload_local(instructions[index - 3U]);
                const auto stored_index = int_load_local(instructions[index - 2U]);
                const auto value_shape = stack_shape(
                    instructions[index - 1U], return_kind);
                const u32 expected_value_slots =
                    (instructions[index].opcode == 0x50U ||
                     instructions[index].opcode == 0x52U) ? 2U : 1U;
                if (!stored_array.has_value() || !stored_index.has_value() ||
                    *stored_array != *array_local ||
                    *stored_index != *index_local || !value_shape.has_value() ||
                    value_shape->pop != 0U ||
                    value_shape->push != expected_value_slots) {
                    continue;
                }
                if (!accept_lease_opcode(*store_lease_opcode)) break;
                matching_stores.push_back(index);
            }
            if (invalidated || !lease_opcode.has_value() ||
                (matching_loads.empty() && matching_stores.empty())) {
                continue;
            }

            plan.array_leases.push_back(ArrayLoopLease {
                .loop_header_pc = header_pc,
                .loop_end_pc = end_pc,
                .array_local = *array_local,
                .length_local = *length_local,
                .index_local = *index_local,
                .lease_opcode = *lease_opcode,
                .pointer_register = 29U,
            });
            for (const usize load : matching_loads) {
                plan.instructions[load - 2U].array_lease_elide_input_push = true;
                plan.instructions[load - 1U].array_lease_elide_input_push = true;
                plan.instructions[load].array_lease_register = 29U;
                plan.instructions[load].array_lease_index_local = *index_local;
                plan.instructions[load].array_lease_opcode = *lease_opcode;
                ++plan.array_bounds_checks_eliminated;
                ++plan.array_runtime_calls_eliminated;
            }
            for (const usize store : matching_stores) {
                plan.instructions[store].array_lease_store_register = 29U;
                plan.instructions[store].array_lease_store_opcode =
                    instructions[store].opcode;
                ++plan.array_bounds_checks_eliminated;
                ++plan.array_runtime_calls_eliminated;
            }

            // Canonical int-array reduction:
            //   for (i = 0; i < length; ++i) total += array[i];
            // It is safe to unroll only when the entire loop body matches this
            // shape exactly. The direct array lease already proves array/index
            // stability and bounds; the exact CFG shape keeps Java budget and
            // OSR semantics straightforward.
            if (!plan.unrolled_int_array_reduction.has_value() &&
                *lease_opcode == 0x2EU && matching_loads.size() == 1U &&
                matching_stores.empty() && header + 10U == source &&
                matching_loads[0] == header + 6U &&
                instructions[header + 2U].opcode == 0xA2U &&
                instructions[header + 2U].branch_target.has_value() &&
                instructions[header + 7U].opcode == 0x60U &&
                instructions[header + 9U].opcode == 0x84U &&
                instructions[header + 9U].local_index == *index_local &&
                instructions[header + 9U].immediate == 1 &&
                is_goto(instructions[header + 10U].opcode) &&
                instructions[header + 10U].branch_target ==
                    std::optional<usize>(header_pc)) {
                const auto total_loaded =
                    int_load_local(instructions[header + 3U]);
                std::optional<u32> total_stored;
                const DecodedInstruction& total_store =
                    instructions[header + 8U];
                if (total_store.opcode == 0x36U) {
                    total_stored = total_store.local_index;
                } else if (total_store.opcode >= 0x3BU &&
                           total_store.opcode <= 0x3EU) {
                    total_stored = static_cast<u32>(
                        total_store.opcode - 0x3BU);
                }
                if (total_loaded.has_value() && total_stored.has_value() &&
                    *total_loaded == *total_stored &&
                    *total_loaded != *index_local &&
                    *total_loaded != *length_local) {
                    plan.unrolled_int_array_reduction =
                        UnrolledIntArrayReduction {
                            .loop_header_pc = header_pc,
                            .loop_end_pc = end_pc,
                            .exit_pc = *instructions[header + 2U].branch_target,
                            .total_local = *total_loaded,
                            .length_local = *length_local,
                            .index_local = *index_local,
                            .iteration_bytecodes = 11U,
                            .final_check_bytecodes = 3U,
                        };
                }
            }
            selected_array_loop = true;
        }
    }

    // Escape analysis / scalar replacement for a non-escaping one-element
    // int array. The virtual array is represented by its original local frame
    // slot: newarray becomes a null placeholder, iastore writes the scalar
    // payload into that slot, and iaload reads it back. We admit this only when
    // every use is a proven [0] access and no later instruction can safepoint or
    // deopt into the interpreter with the virtual reference live.
    if (exception_handlers.empty()) {
        const auto scalar_aload_local = [](const DecodedInstruction& instruction)
            -> std::optional<u32> {
            if (instruction.opcode == 0x19U) return instruction.local_index;
            if (instruction.opcode >= 0x2AU && instruction.opcode <= 0x2DU) {
                return static_cast<u32>(instruction.opcode - 0x2AU);
            }
            return std::nullopt;
        };
        const auto scalar_astore_local = [](const DecodedInstruction& instruction)
            -> std::optional<u32> {
            if (instruction.opcode == 0x3AU) return instruction.local_index;
            if (instruction.opcode >= 0x4BU && instruction.opcode <= 0x4EU) {
                return static_cast<u32>(instruction.opcode - 0x4BU);
            }
            return std::nullopt;
        };
        const auto scalar_safe_after_allocation = [](u8 opcode) noexcept {
            if ((opcode >= 0x02U && opcode <= 0x08U) || opcode == 0x10U ||
                opcode == 0x11U || opcode == 0x15U || opcode == 0x19U ||
                (opcode >= 0x1AU && opcode <= 0x1DU) ||
                (opcode >= 0x2AU && opcode <= 0x2DU) ||
                opcode == 0x2EU || opcode == 0x36U || opcode == 0x3AU ||
                (opcode >= 0x3BU && opcode <= 0x3EU) ||
                (opcode >= 0x4BU && opcode <= 0x4EU) || opcode == 0x4FU ||
                opcode == 0x60U || opcode == 0x64U || opcode == 0x68U ||
                opcode == 0x74U || opcode == 0x78U || opcode == 0x7AU ||
                opcode == 0x7CU || opcode == 0x7EU || opcode == 0x80U ||
                opcode == 0x82U || opcode == 0x84U ||
                (opcode >= 0x91U && opcode <= 0x93U) ||
                (opcode >= 0x99U && opcode <= 0xA7U) ||
                opcode == 0xAAU || opcode == 0xABU || opcode == 0xACU ||
                opcode == 0xB1U || opcode == 0xC6U || opcode == 0xC7U) {
                return true;
            }
            return false;
        };
        const auto simple_scalar_value = [](const DecodedInstruction& instruction) {
            return instruction.opcode == 0x15U ||
                   (instruction.opcode >= 0x1AU && instruction.opcode <= 0x1DU) ||
                   pushed_constant(instruction).has_value();
        };

        bool selected_scalar_array = false;
        for (usize allocation = 1U;
             allocation + 1U < instructions.size() && !selected_scalar_array;
             ++allocation) {
            if (instructions[allocation].opcode != 0xBCU ||
                instructions[allocation].local_index != 10U ||
                pushed_constant(instructions[allocation - 1U]).value_or(-1) != 1) {
                continue;
            }
            const auto stored_local =
                scalar_astore_local(instructions[allocation + 1U]);
            if (!stored_local.has_value() || *stored_local >= local_slots ||
                block_boundary[allocation] ||
                block_boundary[allocation + 1U]) {
                continue;
            }

            const u32 virtual_local = *stored_local;
            std::vector<usize> scalar_loads;
            std::vector<usize> scalar_stores;
            bool valid = true;
            for (usize index = allocation + 2U;
                 index < instructions.size();
                 ++index) {
                const DecodedInstruction& candidate = instructions[index];
                if (!scalar_safe_after_allocation(candidate.opcode)) {
                    valid = false;
                    break;
                }
                if (const auto stored = scalar_astore_local(candidate);
                    stored.has_value() && *stored == virtual_local) {
                    valid = false;
                    break;
                }
                const auto loaded = scalar_aload_local(candidate);
                if (!loaded.has_value() || *loaded != virtual_local) continue;

                if (index + 2U < instructions.size() &&
                    pushed_constant(instructions[index + 1U]).value_or(-1) == 0 &&
                    instructions[index + 2U].opcode == 0x2EU &&
                    !block_boundary[index + 1U] &&
                    !block_boundary[index + 2U]) {
                    scalar_loads.push_back(index + 2U);
                    continue;
                }
                if (index + 3U < instructions.size() &&
                    pushed_constant(instructions[index + 1U]).value_or(-1) == 0 &&
                    simple_scalar_value(instructions[index + 2U]) &&
                    instructions[index + 3U].opcode == 0x4FU &&
                    !block_boundary[index + 1U] &&
                    !block_boundary[index + 2U] &&
                    !block_boundary[index + 3U]) {
                    scalar_stores.push_back(index + 3U);
                    continue;
                }
                valid = false;
                break;
            }
            if (!valid || (scalar_loads.empty() && scalar_stores.empty())) continue;

            plan.instructions[allocation].scalar_array_new_local = virtual_local;
            for (const usize load : scalar_loads) {
                plan.instructions[load].scalar_array_load_local = virtual_local;
                ++plan.scalar_replaced_array_accesses;
            }
            for (const usize store : scalar_stores) {
                plan.instructions[store].scalar_array_store_local = virtual_local;
                ++plan.scalar_replaced_array_accesses;
            }
            ++plan.scalar_replaced_allocations;
            selected_scalar_array = true;
        }
    }

    // Preserve value numbers across an extended basic-block edge when the
    // destination has exactly one predecessor and that predecessor is the
    // immediately preceding fallthrough instruction. This is a real CFG
    // proof: joins, loop headers, exception entries and arbitrary branch
    // targets still invalidate the table.
    std::vector<u32> gvn_predecessor_count(instructions.size(), 0U);
    std::vector<bool> gvn_extend_boundary(instructions.size(), false);
    if (exception_handlers.empty()) {
        const auto add_gvn_successor = [&](std::vector<usize>& successors,
                                           std::optional<usize> pc) {
            if (!pc.has_value()) return;
            const auto found = index_by_pc.find(*pc);
            if (found != index_by_pc.end()) successors.push_back(found->second);
        };
        const auto gvn_has_fallthrough = [](u8 opcode) noexcept {
            if (is_goto(opcode) || is_jsr(opcode) || is_ret(opcode) ||
                opcode == 0xAAU || opcode == 0xABU ||
                opcode == 0xACU || opcode == 0xADU || opcode == 0xAEU ||
                opcode == 0xAFU || opcode == 0xB0U || opcode == 0xB1U ||
                opcode == 0xBFU) {
                return false;
            }
            return true;
        };
        for (usize index = 0U; index < instructions.size(); ++index) {
            const DecodedInstruction& instruction = instructions[index];
            std::vector<usize> successors;
            successors.reserve(4U);
            add_gvn_successor(successors, instruction.branch_target);
            add_gvn_successor(successors, instruction.default_target);
            for (const SwitchTarget& target : instruction.switch_targets) {
                add_gvn_successor(successors, target.target_pc);
            }
            if (gvn_has_fallthrough(instruction.opcode) &&
                index + 1U < instructions.size()) {
                successors.push_back(index + 1U);
            }
            std::sort(successors.begin(), successors.end());
            successors.erase(std::unique(successors.begin(), successors.end()),
                             successors.end());
            for (const usize successor : successors) {
                if (successor < gvn_predecessor_count.size()) {
                    ++gvn_predecessor_count[successor];
                }
            }
        }
        for (usize index = 1U; index < instructions.size(); ++index) {
            if (!block_boundary[index] || gvn_predecessor_count[index] != 1U) {
                continue;
            }
            const DecodedInstruction& previous = instructions[index - 1U];
            if (previous.next_pc == instructions[index].pc &&
                gvn_has_fallthrough(previous.opcode)) {
                gvn_extend_boundary[index] = true;
            }
        }
    }

    std::vector<u32> local_versions(local_slots, 0U);
    std::unordered_map<GvnKey, GvnRecord, GvnKeyHash> gvn_values;
    u32 block_gvn_registers = 0U;
    const auto bump_version = [&](u32 local, bool wide) {
        if (local < local_versions.size()) ++local_versions[local];
        if (wide && local + 1U < local_versions.size()) {
            ++local_versions[local + 1U];
        }
    };
    for (usize index = 0U; index < instructions.size(); ++index) {
        if (block_boundary[index] && !gvn_extend_boundary[index]) {
            std::fill(local_versions.begin(), local_versions.end(), 0U);
            gvn_values.clear();
            block_gvn_registers = 0U;
        }
        const DecodedInstruction& instruction = instructions[index];
        if (index >= 2U && gvn_opcode(instruction.opcode) &&
            !block_boundary[index] && !block_boundary[index - 1U] &&
            !plan.instructions[index].folded_result.has_value() &&
            !plan.instructions[index].right_constant.has_value() &&
            !plan.instructions[index].licm_reuse_register.has_value()) {
            const auto left_local = int_load_local(instructions[index - 2U]);
            const auto right_local = int_load_local(instructions[index - 1U]);
            if (left_local.has_value() && right_local.has_value() &&
                *left_local < local_versions.size() &&
                *right_local < local_versions.size() &&
                !plan.instructions[index - 2U].folded_result.has_value() &&
                !plan.instructions[index - 1U].folded_result.has_value()) {
                GvnKey key {
                    .opcode = instruction.opcode,
                    .left_local = *left_local,
                    .left_version = local_versions[*left_local],
                    .right_local = *right_local,
                    .right_version = local_versions[*right_local],
                };
                if (commutative_gvn_opcode(instruction.opcode) &&
                    std::tie(key.right_local, key.right_version) <
                        std::tie(key.left_local, key.left_version)) {
                    std::swap(key.left_local, key.right_local);
                    std::swap(key.left_version, key.right_version);
                }
                auto [found, inserted] = gvn_values.try_emplace(
                    key, GvnRecord {.first_operation = index});
                if (!inserted) {
                    if (!found->second.register_index.has_value() &&
                        plan.licm_register_count + block_gvn_registers <
                            kGvnRegisters.size()) {
                        const u32 reg = kGvnRegisters[
                            plan.licm_register_count + block_gvn_registers++];
                        found->second.register_index = reg;
                        plan.instructions[found->second.first_operation]
                            .gvn_capture_register = reg;
                        plan.gvn_register_count = std::max(
                            plan.gvn_register_count,
                            plan.licm_register_count + block_gvn_registers);
                    }
                    if (found->second.register_index.has_value()) {
                        plan.instructions[index - 2U].gvn_elide_input_push = true;
                        plan.instructions[index - 1U].gvn_elide_input_push = true;
                        plan.instructions[index].gvn_reuse_register =
                            found->second.register_index;
                        ++plan.common_subexpressions_eliminated;
                        for (usize cursor = found->second.first_operation + 1U;
                             cursor <= index;
                             ++cursor) {
                            if (block_boundary[cursor]) {
                                ++plan.cross_block_common_subexpressions_eliminated;
                                break;
                            }
                        }
                    }
                }
            }
        }

        switch (instruction.opcode) {
        case 0x36U: case 0x38U: case 0x3AU:
            bump_version(instruction.local_index, false); break;
        case 0x37U: case 0x39U:
            bump_version(instruction.local_index, true); break;
        case 0x3BU: case 0x3CU: case 0x3DU: case 0x3EU:
            bump_version(static_cast<u32>(instruction.opcode - 0x3BU), false); break;
        case 0x3FU: case 0x40U: case 0x41U: case 0x42U:
            bump_version(static_cast<u32>(instruction.opcode - 0x3FU), true); break;
        case 0x43U: case 0x44U: case 0x45U: case 0x46U:
            bump_version(static_cast<u32>(instruction.opcode - 0x43U), false); break;
        case 0x47U: case 0x48U: case 0x49U: case 0x4AU:
            bump_version(static_cast<u32>(instruction.opcode - 0x47U), true); break;
        case 0x4BU: case 0x4CU: case 0x4DU: case 0x4EU:
            bump_version(static_cast<u32>(instruction.opcode - 0x4BU), false); break;
        case 0x84U:
            bump_version(instruction.local_index, false); break;
        default:
            break;
        }
    }

    return plan;
}

[[nodiscard]] std::optional<InlineRecipe> analyze_inline_recipe(
    const classfile::ClassFile& owner,
    const classfile::Method& method) {
    if (!method.code.has_value() || !method.code->exception_table.empty()) {
        return std::nullopt;
    }
    std::optional<u8> unsupported_opcode;
    auto decoded = decode_method(owner, method, &unsupported_opcode);
    if (!decoded.has_value()) return std::nullopt;
    const auto int_local = [](const DecodedInstruction& instruction)
        -> std::optional<u32> {
        if (instruction.opcode == 0x15U) return instruction.local_index;
        if (instruction.opcode >= 0x1AU && instruction.opcode <= 0x1DU) {
            return static_cast<u32>(instruction.opcode - 0x1AU);
        }
        return std::nullopt;
    };
    const auto long_local = [](const DecodedInstruction& instruction)
        -> std::optional<u32> {
        if (instruction.opcode == 0x16U) return instruction.local_index;
        if (instruction.opcode >= 0x1EU && instruction.opcode <= 0x21U) {
            return static_cast<u32>(instruction.opcode - 0x1EU);
        }
        return std::nullopt;
    };
    const auto float_local = [](const DecodedInstruction& instruction)
        -> std::optional<u32> {
        if (instruction.opcode == 0x17U) return instruction.local_index;
        if (instruction.opcode >= 0x22U && instruction.opcode <= 0x25U) {
            return static_cast<u32>(instruction.opcode - 0x22U);
        }
        return std::nullopt;
    };
    const auto double_local = [](const DecodedInstruction& instruction)
        -> std::optional<u32> {
        if (instruction.opcode == 0x18U) return instruction.local_index;
        if (instruction.opcode >= 0x26U && instruction.opcode <= 0x29U) {
            return static_cast<u32>(instruction.opcode - 0x26U);
        }
        return std::nullopt;
    };
    const bool has_receiver = (method.access_flags & 0x0008U) == 0U;
    const u32 argument_base = has_receiver ? 1U : 0U;
    const auto pure_binary = [](u8 opcode) noexcept {
        switch (opcode) {
        case 0x60U: case 0x64U: case 0x68U:
        case 0x78U: case 0x7AU: case 0x7CU:
        case 0x7EU: case 0x80U: case 0x82U:
            return true;
        default:
            return false;
        }
    };
    const auto pure_long_binary = [](u8 opcode) noexcept {
        switch (opcode) {
        case 0x61U: case 0x65U: case 0x69U:
        case 0x7FU: case 0x81U: case 0x83U:
            return true;
        default:
            return false;
        }
    };
    const auto pure_float_binary = [](u8 opcode) noexcept {
        return opcode == 0x62U || opcode == 0x66U ||
               opcode == 0x6AU || opcode == 0x6EU;
    };
    const auto pure_double_binary = [](u8 opcode) noexcept {
        return opcode == 0x63U || opcode == 0x67U ||
               opcode == 0x6BU || opcode == 0x6FU;
    };
    if (method.descriptor == "(I)I") {
        if (decoded->size() == 2U &&
            int_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
                argument_base &&
            (*decoded)[1].opcode == 0xACU) {
            return InlineRecipe {
                .kind = InlineRecipeKind::identity_int,
                .nested_bytecode_cost = 2U,
                .has_receiver = has_receiver,
            };
        }
        if (decoded->size() == 3U &&
            int_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
                argument_base &&
            ((*decoded)[1].opcode == 0x74U ||
             (*decoded)[1].opcode == 0x91U ||
             (*decoded)[1].opcode == 0x92U ||
             (*decoded)[1].opcode == 0x93U) &&
            (*decoded)[2].opcode == 0xACU) {
            return InlineRecipe {
                .kind = InlineRecipeKind::unary_int,
                .opcode = (*decoded)[1].opcode,
                .nested_bytecode_cost = 3U,
                .has_receiver = has_receiver,
            };
        }
        if (decoded->size() == 4U &&
            int_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
                argument_base &&
            pushed_constant((*decoded)[1]).has_value() &&
            pure_binary((*decoded)[2].opcode) &&
            (*decoded)[3].opcode == 0xACU) {
            return InlineRecipe {
                .kind = InlineRecipeKind::int_right_constant,
                .opcode = (*decoded)[2].opcode,
                .constant = *pushed_constant((*decoded)[1]),
                .nested_bytecode_cost = 4U,
                .has_receiver = has_receiver,
            };
        }
    }
    if (method.descriptor == "(II)I" && decoded->size() == 4U &&
        int_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
            argument_base &&
        int_local((*decoded)[1]).value_or(std::numeric_limits<u32>::max()) ==
            argument_base + 1U &&
        pure_binary((*decoded)[2].opcode) &&
        (*decoded)[3].opcode == 0xACU) {
        return InlineRecipe {
            .kind = InlineRecipeKind::binary_int,
            .opcode = (*decoded)[2].opcode,
            .nested_bytecode_cost = 4U,
            .has_receiver = has_receiver,
        };
    }
    // Common compact binary-reader helper:
    //
    //   static byte read() { return bytes[cursor++]; }
    //
    // Obfuscated CLDC games frequently put this behind a tiny method and call
    // it once for every byte while parsing PNGs, map packs and custom archives.
    // A separately compiled JIT leaf still pays the full nested invoke/root
    // publication cost per byte. Preserve the exact bytecode operations but
    // inline them into same-class callers so the call boundary disappears.
    if (!has_receiver && method.descriptor == "()B" &&
        decoded->size() == 8U &&
        (*decoded)[0].opcode == 0xB2U && // getstatic byte[]
        (*decoded)[1].opcode == 0xB2U && // getstatic cursor
        (*decoded)[2].opcode == 0x59U && // dup
        (*decoded)[3].opcode == 0x04U && // iconst_1
        (*decoded)[4].opcode == 0x60U && // iadd
        (*decoded)[5].opcode == 0xB3U && // putstatic cursor
        (*decoded)[6].opcode == 0x33U && // baload
        (*decoded)[7].opcode == 0xACU &&
        (*decoded)[1].local_index == (*decoded)[5].local_index) {
        auto bytes_reference = owner.member_reference(
            static_cast<u16>((*decoded)[0].local_index));
        auto cursor_reference = owner.member_reference(
            static_cast<u16>((*decoded)[1].local_index));
        if (bytes_reference && cursor_reference &&
            bytes_reference->kind == classfile::ConstantKind::field_ref &&
            cursor_reference->kind == classfile::ConstantKind::field_ref &&
            bytes_reference->descriptor == "[B" &&
            cursor_reference->descriptor == "I" &&
            bytes_reference->owner == owner.name() &&
            cursor_reference->owner == owner.name()) {
            return InlineRecipe {
                .kind = InlineRecipeKind::static_byte_cursor_read,
                .first_operand = (*decoded)[0].local_index,
                .second_operand = (*decoded)[1].local_index,
                .nested_bytecode_cost = 8U,
                .has_receiver = false,
            };
        }
    }
    // Common collision/UI predicate:
    //   x >= left && x < left + width && y >= top && y < top + height
    // Recognize the bytecode shape rather than a class/method name so callers
    // from any already-initialized class can eliminate millions of tiny
    // runtime-dispatch calls without changing Java overflow semantics.
    if (method.descriptor == "(IIIIII)Z" && decoded->size() == 20U) {
        const auto local_is = [&](usize index, u32 local) {
            return int_local((*decoded)[index]).value_or(
                std::numeric_limits<u32>::max()) == argument_base + local;
        };
        const usize false_pc = (*decoded)[18].pc;
        const usize return_pc = (*decoded)[19].pc;
        const auto branches_to_false = [&](usize index, u8 opcode) {
            return (*decoded)[index].opcode == opcode &&
                (*decoded)[index].branch_target.has_value() &&
                *(*decoded)[index].branch_target == false_pc;
        };
        const bool shape =
            local_is(0U, 0U) && local_is(1U, 2U) &&
            branches_to_false(2U, 0xA1U) && // if_icmplt
            local_is(3U, 0U) && local_is(4U, 2U) && local_is(5U, 4U) &&
            (*decoded)[6].opcode == 0x60U &&
            branches_to_false(7U, 0xA2U) && // if_icmpge
            local_is(8U, 1U) && local_is(9U, 3U) &&
            branches_to_false(10U, 0xA1U) &&
            local_is(11U, 1U) && local_is(12U, 3U) && local_is(13U, 5U) &&
            (*decoded)[14].opcode == 0x60U &&
            branches_to_false(15U, 0xA2U) &&
            pushed_constant((*decoded)[16]).value_or(-1) == 1 &&
            (*decoded)[17].opcode == 0xA7U &&
            (*decoded)[17].branch_target.has_value() &&
            *(*decoded)[17].branch_target == return_pc &&
            pushed_constant((*decoded)[18]).value_or(-1) == 0 &&
            (*decoded)[19].opcode == 0xACU;
        if (shape) {
            return InlineRecipe {
                .kind = InlineRecipeKind::rect_contains_int,
                .nested_bytecode_cost = 20U,
                .has_receiver = has_receiver,
            };
        }
    }
    if (method.descriptor == "(J)J") {
        if (decoded->size() == 2U &&
            long_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
                argument_base &&
            (*decoded)[1].opcode == 0xADU) {
            return InlineRecipe {
                .kind = InlineRecipeKind::identity_long,
                .nested_bytecode_cost = 2U,
                .result_slots = 2U,
                .has_receiver = has_receiver,
            };
        }
        if (decoded->size() == 3U &&
            long_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
                argument_base &&
            (*decoded)[1].opcode == 0x75U &&
            (*decoded)[2].opcode == 0xADU) {
            return InlineRecipe {
                .kind = InlineRecipeKind::unary_long,
                .opcode = 0x75U,
                .nested_bytecode_cost = 3U,
                .result_slots = 2U,
                .has_receiver = has_receiver,
            };
        }
    }
    if (method.descriptor == "(JJ)J" && decoded->size() == 4U &&
        long_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
            argument_base &&
        long_local((*decoded)[1]).value_or(std::numeric_limits<u32>::max()) ==
            argument_base + 2U &&
        pure_long_binary((*decoded)[2].opcode) &&
        (*decoded)[3].opcode == 0xADU) {
        return InlineRecipe {
            .kind = InlineRecipeKind::binary_long,
            .opcode = (*decoded)[2].opcode,
            .nested_bytecode_cost = 4U,
            .result_slots = 2U,
            .has_receiver = has_receiver,
        };
    }
    if (method.descriptor == "(F)F") {
        if (decoded->size() == 2U &&
            float_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
                argument_base &&
            (*decoded)[1].opcode == 0xAEU) {
            return InlineRecipe {
                .kind = InlineRecipeKind::identity_float,
                .nested_bytecode_cost = 2U,
                .has_receiver = has_receiver,
            };
        }
        if (decoded->size() == 3U &&
            float_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
                argument_base &&
            (*decoded)[1].opcode == 0x76U &&
            (*decoded)[2].opcode == 0xAEU) {
            return InlineRecipe {
                .kind = InlineRecipeKind::unary_float,
                .opcode = 0x76U,
                .nested_bytecode_cost = 3U,
                .has_receiver = has_receiver,
            };
        }
    }
    if (method.descriptor == "(FF)F" && decoded->size() == 4U &&
        float_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
            argument_base &&
        float_local((*decoded)[1]).value_or(std::numeric_limits<u32>::max()) ==
            argument_base + 1U &&
        pure_float_binary((*decoded)[2].opcode) &&
        (*decoded)[3].opcode == 0xAEU) {
        return InlineRecipe {
            .kind = InlineRecipeKind::binary_float,
            .opcode = (*decoded)[2].opcode,
            .nested_bytecode_cost = 4U,
            .has_receiver = has_receiver,
        };
    }
    if (method.descriptor == "(D)D") {
        if (decoded->size() == 2U &&
            double_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
                argument_base &&
            (*decoded)[1].opcode == 0xAFU) {
            return InlineRecipe {
                .kind = InlineRecipeKind::identity_double,
                .nested_bytecode_cost = 2U,
                .result_slots = 2U,
                .has_receiver = has_receiver,
            };
        }
        if (decoded->size() == 3U &&
            double_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
                argument_base &&
            (*decoded)[1].opcode == 0x77U &&
            (*decoded)[2].opcode == 0xAFU) {
            return InlineRecipe {
                .kind = InlineRecipeKind::unary_double,
                .opcode = 0x77U,
                .nested_bytecode_cost = 3U,
                .result_slots = 2U,
                .has_receiver = has_receiver,
            };
        }
    }
    if (method.descriptor == "(DD)D" && decoded->size() == 4U &&
        double_local((*decoded)[0]).value_or(std::numeric_limits<u32>::max()) ==
            argument_base &&
        double_local((*decoded)[1]).value_or(std::numeric_limits<u32>::max()) ==
            argument_base + 2U &&
        pure_double_binary((*decoded)[2].opcode) &&
        (*decoded)[3].opcode == 0xAFU) {
        return InlineRecipe {
            .kind = InlineRecipeKind::binary_double,
            .opcode = (*decoded)[2].opcode,
            .nested_bytecode_cost = 4U,
            .result_slots = 2U,
            .has_receiver = has_receiver,
        };
    }
    return std::nullopt;
}

[[nodiscard]] CompileAttempt reject_attempt(
    JitRejectReason reason,
    std::optional<u8> unsupported_opcode = std::nullopt) {
    return CompileAttempt {
        .disposition = CompileDisposition::unsupported,
        .method = std::nullopt,
        .reject_reason = reason,
        .unsupported_opcode = unsupported_opcode,
    };
}

[[nodiscard]] CompileAttempt compile_scalar_method(
    const classfile::ClassFile& owner,
    const classfile::Method& method,
    const CachedMethodDescriptor& descriptor,
    bool has_receiver,
    ExecutableArena& executable_arena,
    JitInlineResolverHooks inline_resolver,
    const VerifiedMethodReferenceMaps* preverified_frames = nullptr) {
    if (!method.code.has_value()) {
        return reject_attempt(JitRejectReason::missing_code);
    }
    if ((!scalar_like(descriptor.return_kind) &&
         descriptor.return_kind != JavaTypeKind::void_type)) {
        return reject_attempt(JitRejectReason::unsupported_signature);
    }
    for (const TypeDescriptor& parameter : descriptor.descriptor.parameters) {
        if (!scalar_like(parameter.kind)) {
            return reject_attempt(JitRejectReason::unsupported_signature);
        }
    }
    const u32 local_slots = method.code->max_locals;
    const u32 stack_slots = method.code->max_stack;
    if (descriptor.argument_slots(has_receiver) > local_slots ||
        local_slots > 1'000U || stack_slots > 1'000U ||
        local_slots > std::numeric_limits<u32>::max() - stack_slots) {
        return reject_attempt(JitRejectReason::frame_too_large);
    }

    const u32 total_slots = local_slots + stack_slots;
    const bool quick_tier = quick_compile_tier_enabled();

    std::optional<u8> unsupported_opcode;
    auto decoded = decode_method(owner, method, &unsupported_opcode);
    if (!decoded.has_value()) {
        return reject_attempt(
            unsupported_opcode.has_value()
                ? JitRejectReason::unsupported_opcode
                : JitRejectReason::decode_failure,
            unsupported_opcode);
    }
    // Monitor bytecodes release/reacquire host execution around contention and
    // Object.wait. The baseline JIT's current runtime bridge can resume with a
    // stale compiled monitor ownership assumption, which manifests as a tight
    // IllegalMonitorStateException loop in real MIDlets. Keep synchronized
    // regions on the interpreter until monitor ownership is represented in the
    // compiled deopt state. This is correctness-first and affects only methods
    // that explicitly contain monitorenter/monitorexit; ordinary hot methods
    // remain JIT eligible.
    if (std::any_of(decoded->begin(), decoded->end(),
                    [](const DecodedInstruction& instruction) {
                        return instruction.opcode == 0xC2U ||
                               instruction.opcode == 0xC3U;
                    })) {
        return reject_attempt(JitRejectReason::unsafe_side_effect);
    }
    auto depths = compute_stack_depths(
        *decoded,
        local_slots,
        stack_slots,
        descriptor.return_kind,
        method.code->exception_table);
    if (!depths.has_value()) {
        return reject_attempt(JitRejectReason::invalid_stack_shape);
    }
    std::optional<VerifiedMethodReferenceMaps> rebuilt_verified_frames;
    const VerifiedMethodReferenceMaps* verified_frames = preverified_frames;
    if (verified_frames == nullptr) {
        auto rebuilt = verified_reference_maps(owner, method);
        if (!rebuilt) {
            return reject_attempt(JitRejectReason::invalid_stack_shape);
        }
        rebuilt_verified_frames = std::move(*rebuilt);
        verified_frames = &*rebuilt_verified_frames;
    }
    std::vector<JitReferenceMap> reference_maps(1U);
    std::unordered_map<usize, u32> reference_map_id_by_pc;
    reference_map_id_by_pc.reserve(verified_frames->frames.size());
    std::unordered_map<u32, JitDeoptFrameMap> deopt_frame_maps;
    deopt_frame_maps.reserve(verified_frames->frames.size());
    usize maximum_reference_roots = 0U;
    for (const VerifiedReferenceMap& verified : verified_frames->frames) {
        if (verified.bytecode_pc >
                static_cast<usize>(std::numeric_limits<u32>::max()) ||
            verified.stack_slots >
                static_cast<usize>(std::numeric_limits<u32>::max())) {
            return reject_attempt(JitRejectReason::frame_too_large);
        }
        deopt_frame_maps.emplace(
            static_cast<u32>(verified.bytecode_pc),
            JitDeoptFrameMap {
                .local_slots = local_slots,
                .stack_slots = static_cast<u32>(verified.stack_slots),
                .slot_kinds = verified.slot_kinds,
            });
        if (verified.reference_slots.empty()) {
            reference_map_id_by_pc.emplace(verified.bytecode_pc, 0U);
            continue;
        }
        if (reference_maps.size() >=
            static_cast<usize>(std::numeric_limits<u32>::max())) {
            return reject_attempt(JitRejectReason::frame_too_large);
        }
        JitReferenceMap map;
        map.frame_offsets.reserve(verified.reference_slots.size());
        for (const usize slot : verified.reference_slots) {
            if (slot >= total_slots) {
                return reject_attempt(JitRejectReason::invalid_stack_shape);
            }
            map.frame_offsets.push_back(
                kJitFrameHeaderBytes + static_cast<u32>(slot) * 8U);
        }
        maximum_reference_roots = std::max(
            maximum_reference_roots, map.frame_offsets.size());
        const u32 map_id = static_cast<u32>(reference_maps.size());
        reference_maps.push_back(std::move(map));
        reference_map_id_by_pc.emplace(verified.bytecode_pc, map_id);
    }

    // Runtime-helper deopts now preserve a verifier-typed physical frame and
    // every admitted nested call either fails before execution (precise resume)
    // or completes/throws exactly once. Legacy jsr/ret remains the only OSR
    // exclusion because its subroutine return-address state spans multiple
    // control-flow entries.
    bool osr_safe = true;
    for (const DecodedInstruction& instruction : *decoded) {
        if (is_jsr(instruction.opcode) || is_ret(instruction.opcode)) {
            osr_safe = false;
        }
    }

    OptimizationPlan optimization_plan = build_optimization_plan(
        *decoded,
        *depths,
        local_slots,
        descriptor.return_kind,
        method.code->exception_table,
        !quick_tier);

    std::unordered_map<usize, InlineRecipe> inline_recipes;
    u64 inlined_bytecodes = 0U;
    u64 devirtualized_calls = 0U;
    const auto callsite_aload_local = [](const DecodedInstruction& instruction)
        -> std::optional<u32> {
        if (instruction.opcode == 0x19U) return instruction.local_index;
        if (instruction.opcode >= 0x2AU && instruction.opcode <= 0x2DU) {
            return static_cast<u32>(instruction.opcode - 0x2AU);
        }
        return std::nullopt;
    };
    const auto simple_int_argument = [](const DecodedInstruction& instruction) {
        if (instruction.opcode == 0x15U ||
            (instruction.opcode >= 0x1AU && instruction.opcode <= 0x1DU)) {
            return true;
        }
        return pushed_constant(instruction).has_value();
    };
    if (!quick_tier) {
        for (usize index = 0U; index < decoded->size(); ++index) {
            const DecodedInstruction& instruction = (*decoded)[index];
            if (!(*depths)[index].has_value() ||
                (instruction.opcode != 0xB8U && instruction.opcode != 0xB7U &&
                 instruction.opcode != 0xB6U)) {
                continue;
            }
            auto reference = owner.member_reference(
                static_cast<u16>(instruction.local_index));
            if (!reference) continue;
            std::shared_ptr<const classfile::ClassFile> cross_owner;
            const classfile::ClassFile* callee_owner = &owner;
            const classfile::Method* callee = nullptr;
            if (reference->owner == owner.name()) {
                callee = owner.find_method(reference->name, reference->descriptor);
            } else {
                // Cross-class inlining is restricted to invokestatic and delegated
                // to Machine's resolver. The resolver only exposes targets whose
                // declaring class has already completed initialization, preserving
                // the JVM's active-use class initialization semantics.
                if (instruction.opcode != 0xB8U ||
                    inline_resolver.resolve == nullptr) {
                    continue;
                }
                auto target = inline_resolver.resolve(
                    inline_resolver.context,
                    reference->owner,
                    reference->name,
                    reference->descriptor);
                if (!target.has_value() || target->owner == nullptr ||
                    target->method == nullptr) {
                    continue;
                }
                cross_owner = std::move(target->owner);
                callee_owner = cross_owner.get();
                callee = target->method;
            }
            if (callee == nullptr ||
                (callee_owner == &owner && callee == &method) ||
                (callee->access_flags & (0x0020U | 0x0100U | 0x0400U)) != 0U) {
                continue;
            }
            const bool callee_static = (callee->access_flags & 0x0008U) != 0U;
            if ((instruction.opcode == 0xB8U) != callee_static) continue;
            if (instruction.opcode == 0xB6U &&
                (callee->access_flags & 0x0010U) == 0U &&
                (owner.access_flags() & 0x0010U) == 0U) {
                continue;
            }
            auto recipe = analyze_inline_recipe(*callee_owner, *callee);
            if (!recipe.has_value()) continue;
            // Field constant-pool indices in this recipe belong to the callee.
            // Runtime field dispatch below uses the currently compiled owner's
            // constant pool, so only apply it when both are the same class.
            if (recipe->kind == InlineRecipeKind::static_byte_cursor_read &&
                callee_owner != &owner) {
                continue;
            }
            if (instruction.opcode == 0xB7U || instruction.opcode == 0xB6U) {
                // Only inline an instance leaf when the receiver is the caller's
                // local-0 `this`, which is JVM-proven non-null. This preserves the
                // implicit NPE semantics without adding a speculative null guard.
                if (!has_receiver || instruction.argument_slots > 2U ||
                    index < static_cast<usize>(instruction.argument_slots + 1U)) {
                    continue;
                }
                const usize receiver_index =
                    index - static_cast<usize>(instruction.argument_slots) - 1U;
                const auto receiver =
                    callsite_aload_local((*decoded)[receiver_index]);
                if (!receiver.has_value() || *receiver != 0U) continue;
                bool simple_arguments = true;
                for (usize argument_index = receiver_index + 1U;
                     argument_index < index;
                     ++argument_index) {
                    if (!simple_int_argument((*decoded)[argument_index])) {
                        simple_arguments = false;
                        break;
                    }
                }
                if (!simple_arguments) continue;
            }
            if (instruction.opcode == 0xB6U) ++devirtualized_calls;
            inlined_bytecodes += recipe->nested_bytecode_cost;
            inline_recipes.emplace(index, *recipe);
        }
    }

    enum class CachedLocalKind : u8 {
        int32,
        int64,
        float32,
        float64,
        reference,
        conflict,
    };
    struct CachedLocalCandidate final {
        u32 index {0U};
        CachedLocalKind kind {CachedLocalKind::int32};
        u32 loads {0U};
        u32 stores {0U};
    };
    struct CachedLocal final {
        u32 index {0U};
        CachedLocalKind kind {CachedLocalKind::int32};
        u32 register_index {0U};
        u32 loads {0U};
    };
    std::unordered_map<u32, CachedLocalCandidate> local_candidates;
    const auto observe_local = [&](u32 index,
                                   CachedLocalKind kind,
                                   bool load) {
        if (index >= local_slots) return;
        auto [iterator, inserted] = local_candidates.try_emplace(
            index,
            CachedLocalCandidate {
                .index = index,
                .kind = kind,
            });
        CachedLocalCandidate& candidate = iterator->second;
        if (!inserted && candidate.kind != kind) {
            candidate.kind = CachedLocalKind::conflict;
        }
        if (load) ++candidate.loads;
        else ++candidate.stores;
    };
    for (const DecodedInstruction& instruction : *decoded) {
        switch (instruction.opcode) {
        case 0x15U:
            observe_local(instruction.local_index,
                          CachedLocalKind::int32,
                          true);
            break;
        case 0x16U:
            observe_local(instruction.local_index,
                          CachedLocalKind::int64,
                          true);
            break;
        case 0x17U:
            observe_local(instruction.local_index,
                          CachedLocalKind::float32,
                          true);
            break;
        case 0x18U:
            observe_local(instruction.local_index,
                          CachedLocalKind::float64,
                          true);
            break;
        case 0x19U:
            observe_local(instruction.local_index,
                          CachedLocalKind::reference,
                          true);
            break;
        case 0x36U:
            observe_local(instruction.local_index,
                          CachedLocalKind::int32,
                          false);
            break;
        case 0x37U:
            observe_local(instruction.local_index,
                          CachedLocalKind::int64,
                          false);
            break;
        case 0x38U:
            observe_local(instruction.local_index,
                          CachedLocalKind::float32,
                          false);
            break;
        case 0x39U:
            observe_local(instruction.local_index,
                          CachedLocalKind::float64,
                          false);
            break;
        case 0x3AU:
            observe_local(instruction.local_index,
                          CachedLocalKind::reference,
                          false);
            break;
        case 0x84U:
            observe_local(instruction.local_index,
                          CachedLocalKind::int32,
                          true);
            break;
        default:
            if (instruction.opcode >= 0x1AU &&
                instruction.opcode <= 0x1DU) {
                observe_local(
                    static_cast<u32>(instruction.opcode - 0x1AU),
                    CachedLocalKind::int32,
                    true);
            } else if (instruction.opcode >= 0x1EU &&
                       instruction.opcode <= 0x21U) {
                observe_local(
                    static_cast<u32>(instruction.opcode - 0x1EU),
                    CachedLocalKind::int64,
                    true);
            } else if (instruction.opcode >= 0x22U &&
                       instruction.opcode <= 0x25U) {
                observe_local(
                    static_cast<u32>(instruction.opcode - 0x22U),
                    CachedLocalKind::float32,
                    true);
            } else if (instruction.opcode >= 0x26U &&
                       instruction.opcode <= 0x29U) {
                observe_local(
                    static_cast<u32>(instruction.opcode - 0x26U),
                    CachedLocalKind::float64,
                    true);
            } else if (instruction.opcode >= 0x2AU &&
                       instruction.opcode <= 0x2DU) {
                observe_local(
                    static_cast<u32>(instruction.opcode - 0x2AU),
                    CachedLocalKind::reference,
                    true);
            } else if (instruction.opcode >= 0x3BU &&
                       instruction.opcode <= 0x3EU) {
                observe_local(
                    static_cast<u32>(instruction.opcode - 0x3BU),
                    CachedLocalKind::int32,
                    false);
            } else if (instruction.opcode >= 0x3FU &&
                       instruction.opcode <= 0x42U) {
                observe_local(
                    static_cast<u32>(instruction.opcode - 0x3FU),
                    CachedLocalKind::int64,
                    false);
            } else if (instruction.opcode >= 0x43U &&
                       instruction.opcode <= 0x46U) {
                observe_local(
                    static_cast<u32>(instruction.opcode - 0x43U),
                    CachedLocalKind::float32,
                    false);
            } else if (instruction.opcode >= 0x47U &&
                       instruction.opcode <= 0x4AU) {
                observe_local(
                    static_cast<u32>(instruction.opcode - 0x47U),
                    CachedLocalKind::float64,
                    false);
            } else if (instruction.opcode >= 0x4BU &&
                       instruction.opcode <= 0x4EU) {
                observe_local(
                    static_cast<u32>(instruction.opcode - 0x4BU),
                    CachedLocalKind::reference,
                    false);
            }
            break;
        }
    }
    std::vector<CachedLocalCandidate> ranked_locals;
    ranked_locals.reserve(local_candidates.size());
    for (const auto& [index, candidate] : local_candidates) {
        (void)index;
        const u32 minimum_loads = optimization_plan.contains_loop ? 2U : 4U;
        if (candidate.kind == CachedLocalKind::conflict ||
            candidate.loads < minimum_loads) {
            continue;
        }
        ranked_locals.push_back(candidate);
    }
    std::sort(ranked_locals.begin(),
              ranked_locals.end(),
              [](const CachedLocalCandidate& left,
                 const CachedLocalCandidate& right) {
                  if (left.loads != right.loads) return left.loads > right.loads;
                  if (left.stores != right.stores) return left.stores > right.stores;
                  return left.index < right.index;
              });
    std::vector<CachedLocal> cached_locals;
    const usize cached_count = std::min(
        ranked_locals.size(), kCachedLocalRegisters.size());
    cached_locals.reserve(cached_count);
    u64 register_cached_loads = 0U;
    for (usize index = 0U; index < cached_count; ++index) {
        cached_locals.push_back(CachedLocal {
            .index = ranked_locals[index].index,
            .kind = ranked_locals[index].kind,
            .register_index = kCachedLocalRegisters[index],
            .loads = ranked_locals[index].loads,
        });
        register_cached_loads += ranked_locals[index].loads;
    }
    const auto cached_local_register = [&](u32 index,
                                           CachedLocalKind kind)
        -> std::optional<u32> {
        for (const CachedLocal& cached : cached_locals) {
            if (cached.index == index && cached.kind == kind) {
                return cached.register_index;
            }
        }
        return std::nullopt;
    };
    const auto cached_local_is_wide = [](CachedLocalKind kind) noexcept {
        return kind == CachedLocalKind::int64 ||
               kind == CachedLocalKind::float64 ||
               kind == CachedLocalKind::reference;
    };

    bool stack_cache_safe = optimization_plan.contains_loop && osr_safe &&
        method.code->exception_table.empty();
    if (stack_cache_safe) {
        for (const DecodedInstruction& instruction : *decoded) {
            if (instruction.opcode >= 0x59U && instruction.opcode <= 0x5FU) {
                stack_cache_safe = false;
                break;
            }
        }
    }
    const u32 stack_cache_count = stack_cache_safe
        ? static_cast<u32>(std::min<usize>(
              stack_slots, kCachedStackRegisters.size()))
        : 0U;
    const auto cached_stack_register = [stack_cache_count](u32 depth)
        -> std::optional<u32> {
        if (depth >= stack_cache_count) return std::nullopt;
        return kCachedStackRegisters[depth];
    };

    const bool has_array_lease = !optimization_plan.array_leases.empty();
    const u32 saved_register_bytes =
        (static_cast<u32>(cached_locals.size()) + stack_cache_count +
         optimization_plan.gvn_register_count +
         (has_array_lease ? 1U : 0U)) * 8U;
    const u32 raw_frame_size =
        kJitFrameHeaderBytes + total_slots * 8U + saved_register_bytes;
    const u32 aligned_frame_size = (raw_frame_size + 15U) & ~15U;
    const u32 frame_size = std::max(16U, aligned_frame_size);
    if (frame_size > 4'080U) {
        return reject_attempt(JitRejectReason::frame_too_large);
    }
    const auto saved_register_offset = [&](usize index) noexcept {
        return kJitFrameHeaderBytes + total_slots * 8U +
               static_cast<u32>(index) * 8U;
    };
    const auto saved_stack_register_offset = [&](u32 index) noexcept {
        return saved_register_offset(cached_locals.size()) + index * 8U;
    };
    const auto saved_gvn_register_offset = [&](u32 index) noexcept {
        return saved_stack_register_offset(stack_cache_count) + index * 8U;
    };
    const auto saved_array_lease_register_offset = [&]() noexcept {
        return saved_gvn_register_offset(optimization_plan.gvn_register_count);
    };

    u64 strength_reductions = 0U;
    bool requires_runtime_dispatch = false;

    Arm64Emitter emitter;
    emitter.sub_sp(frame_size);
    for (usize index = 0U; index < cached_locals.size(); ++index) {
        emitter.store_x(cached_locals[index].register_index,
                        kStackPointer,
                        saved_register_offset(index));
    }
    for (u32 index = 0U; index < stack_cache_count; ++index) {
        emitter.store_x(kCachedStackRegisters[index],
                        kStackPointer,
                        saved_stack_register_offset(index));
    }
    for (u32 index = 0U; index < optimization_plan.gvn_register_count; ++index) {
        emitter.store_x(kGvnRegisters[index],
                        kStackPointer,
                        saved_gvn_register_offset(index));
    }
    if (has_array_lease) {
        emitter.store_x(kArrayLeaseRegister,
                        kStackPointer,
                        saved_array_lease_register_offset());
    }
    emitter.store_x(0U, kStackPointer, kRuntimeContextOffset);
    emitter.store_x(1U, kStackPointer, kRuntimeDispatchOffset);
    emitter.store_x(4U, kStackPointer, kResultPointerOffset);
    emitter.store_x(30U, kStackPointer, kReturnAddressOffset);
    emitter.move_w(kBudgetInitial, 3U);
    emitter.move_w(kBudgetRemaining, 3U);
    emitter.move_x(kResultPointer, 4U);
    emitter.move_imm32(kScratchLeft, 0U);
    emitter.store_w(kScratchLeft,
                    kStackPointer,
                    static_cast<u32>(kJitRuntimeConsumedByteOffset));
    emitter.move_imm32(kScratchLeft, local_slots);
    emitter.store_w(kScratchLeft,
                    kStackPointer,
                    static_cast<u32>(kJitRuntimeLocalSlotsByteOffset));
    emitter.store_w(31U,
                    kStackPointer,
                    static_cast<u32>(kJitRuntimeStackDepthByteOffset));

    usize argument_index = 0U;
    u32 local_slot = 0U;
    if (has_receiver) {
        emitter.load_x(kScratchLeft, 2U, 0U);
        emitter.store_x(kScratchLeft,
                        kStackPointer,
                        kJitFrameHeaderBytes);
        ++argument_index;
        ++local_slot;
    }
    for (const TypeDescriptor& parameter : descriptor.descriptor.parameters) {
        const u32 argument_offset = static_cast<u32>(argument_index) * 8U;
        emitter.load_x(kScratchLeft, 2U, argument_offset);
        emitter.store_x(kScratchLeft,
                        kStackPointer,
                        kJitFrameHeaderBytes + local_slot * 8U);
        ++argument_index;
        local_slot += static_cast<u32>(parameter.slot_count());
    }

    const auto local_offset = [](u32 index) noexcept {
        return kJitFrameHeaderBytes + index * 8U;
    };
    const auto stack_offset = [local_slots](u32 depth) noexcept {
        return kJitFrameHeaderBytes + (local_slots + depth) * 8U;
    };
    for (const CachedLocal& cached : cached_locals) {
        if (cached_local_is_wide(cached.kind)) {
            emitter.load_x(cached.register_index,
                           kStackPointer,
                           local_offset(cached.index));
        } else {
            emitter.load_w(cached.register_index,
                           kStackPointer,
                           local_offset(cached.index));
        }
    }
    const auto spill_cached_locals = [&]() {
        for (const CachedLocal& cached : cached_locals) {
            if (cached_local_is_wide(cached.kind)) {
                emitter.store_x(cached.register_index,
                                kStackPointer,
                                local_offset(cached.index));
            } else {
                emitter.store_w(cached.register_index,
                                kStackPointer,
                                local_offset(cached.index));
            }
        }
    };
    const auto spill_cached_stack = [&](u32 live_depth) {
        const u32 cached_depth = std::min(stack_cache_count, live_depth);
        for (u32 depth = 0U; depth < cached_depth; ++depth) {
            emitter.store_x(kCachedStackRegisters[depth],
                            kStackPointer,
                            stack_offset(depth));
        }
    };
    const auto restore_cached_registers = [&]() {
        for (usize index = 0U; index < cached_locals.size(); ++index) {
            emitter.load_x(cached_locals[index].register_index,
                           kStackPointer,
                           saved_register_offset(index));
        }
        for (u32 index = 0U; index < stack_cache_count; ++index) {
            emitter.load_x(kCachedStackRegisters[index],
                           kStackPointer,
                           saved_stack_register_offset(index));
        }
        for (u32 index = 0U;
             index < optimization_plan.gvn_register_count;
             ++index) {
            emitter.load_x(kGvnRegisters[index],
                           kStackPointer,
                           saved_gvn_register_offset(index));
        }
        if (has_array_lease) {
            emitter.load_x(kArrayLeaseRegister,
                           kStackPointer,
                           saved_array_lease_register_offset());
        }
    };

    std::unordered_map<usize, usize> native_position_by_pc;
    native_position_by_pc.reserve(decoded->size());
    std::unordered_map<usize, usize> loop_body_position_by_pc;
    loop_body_position_by_pc.reserve(
        optimization_plan.licm_expressions.size() +
        optimization_plan.array_leases.size());
    std::vector<BranchPatch> branch_patches;
    std::vector<ArithmeticTrapSite> arithmetic_trap_sites;
    std::vector<usize> budget_patches;
    std::vector<usize> runtime_failure_patches;
    std::vector<RuntimeExceptionSite> runtime_exception_sites;
    std::vector<usize> terminal_deopt_patches;
    std::vector<usize> jsr_return_pcs;
    for (const DecodedInstruction& instruction : *decoded) {
        if (is_jsr(instruction.opcode)) {
            jsr_return_pcs.push_back(instruction.next_pc);
        }
    }
    std::sort(jsr_return_pcs.begin(), jsr_return_pcs.end());
    jsr_return_pcs.erase(
        std::unique(jsr_return_pcs.begin(), jsr_return_pcs.end()),
        jsr_return_pcs.end());
    u32 active_safepoint_depth = 0U;
    u64 stack_cached_pops = 0U;
    u64 stack_cached_stores_elided = 0U;
    u64 unrolled_int_array_loops = 0U;
    u64 register_cached_stores_elided = 0U;


    const auto emit_normal_return = [&](bool has_value,
                                        u32 result_register) {
        emitter.load_x(kResultPointer,
                       kStackPointer,
                       kResultPointerOffset);
        if (has_value) {
            emitter.store_x(result_register, kResultPointer, 0U);
        } else {
            emitter.move_imm32(kScratchLeft, 0U);
            emitter.store_x(kScratchLeft, kResultPointer, 0U);
        }
        emitter.sub_w(kScratchThird, kBudgetInitial, kBudgetRemaining);
        emitter.move_imm32(0U, 0U);
        emitter.or_x_shifted(0U, 0U, kScratchThird, 32U);
        emitter.load_x(30U, kStackPointer, kReturnAddressOffset);
        restore_cached_registers();
        emitter.add_sp(frame_size);
        emitter.ret();
    };

    const auto emit_runtime_call = [&](JitRuntimeOperation operation,
                                       u32 operand,
                                       u32 first_register,
                                       u32 second_register,
                                       u32 third_register,
                                       u32 bytecode_pc) {
        spill_cached_locals();
        spill_cached_stack(active_safepoint_depth);
        const auto map = reference_map_id_by_pc.find(bytecode_pc);
        const u32 safepoint_id = map == reference_map_id_by_pc.end()
            ? 0U
            : map->second;
        const auto operation_is_non_allocating = [](JitRuntimeOperation value) {
            switch (value) {
            case JitRuntimeOperation::get_field:
            case JitRuntimeOperation::put_field:
            case JitRuntimeOperation::get_static:
            case JitRuntimeOperation::put_static:
            case JitRuntimeOperation::array_load:
            case JitRuntimeOperation::array_store:
            case JitRuntimeOperation::array_length:
            case JitRuntimeOperation::array_payload_lease:
            case JitRuntimeOperation::check_cast:
            case JitRuntimeOperation::instance_of:
            case JitRuntimeOperation::float_remainder:
            case JitRuntimeOperation::double_remainder:
            case JitRuntimeOperation::float_compare_less:
            case JitRuntimeOperation::float_compare_greater:
            case JitRuntimeOperation::double_compare_less:
            case JitRuntimeOperation::double_compare_greater:
            case JitRuntimeOperation::int_to_float:
            case JitRuntimeOperation::int_to_double:
            case JitRuntimeOperation::long_to_float:
            case JitRuntimeOperation::long_to_double:
            case JitRuntimeOperation::float_to_int:
            case JitRuntimeOperation::float_to_long:
            case JitRuntimeOperation::float_to_double:
            case JitRuntimeOperation::double_to_int:
            case JitRuntimeOperation::double_to_long:
            case JitRuntimeOperation::double_to_float:
                return true;
            default:
                return false;
            }
        };
        bool covered_by_handler = false;
        for (const classfile::ExceptionHandler& handler :
             method.code->exception_table) {
            if (static_cast<usize>(bytecode_pc) >=
                    static_cast<usize>(handler.start_pc) &&
                static_cast<usize>(bytecode_pc) <
                    static_cast<usize>(handler.end_pc)) {
                covered_by_handler = true;
                break;
            }
        }
        const bool no_safepoint =
            operation_is_non_allocating(operation) && !covered_by_handler;
        const u32 encoded_operand = operand |
            (no_safepoint ? kJitRuntimeNoSafepointOperandFlag : 0U);
        const u64 packed_operand =
            (static_cast<u64>(safepoint_id) << 32U) |
            static_cast<u64>(encoded_operand);
        emitter.store_w(kBudgetInitial,
                        kStackPointer,
                        kBudgetInitialOffset);
        emitter.store_w(kBudgetRemaining,
                        kStackPointer,
                        kBudgetRemainingOffset);
        emitter.move_imm32(kScratchMetadata, 0U);
        emitter.store_w(kScratchMetadata,
                        kStackPointer,
                        static_cast<u32>(kJitRuntimeConsumedByteOffset));
        emitter.move_imm32(kScratchMetadata, active_safepoint_depth);
        emitter.store_w(kScratchMetadata,
                        kStackPointer,
                        static_cast<u32>(kJitRuntimeStackDepthByteOffset));
        emitter.load_x(0U, kStackPointer, kRuntimeContextOffset);
        emitter.move_imm32(1U, static_cast<u32>(operation));
        emitter.move_imm64(2U, packed_operand);
        emitter.move_x(3U, first_register);
        emitter.move_x(4U, second_register);
        emitter.move_x(5U, third_register);
        emitter.add_imm_x(6U, kStackPointer, 0U);
        emitter.add_imm_x(7U, kStackPointer, kRuntimeResultOffset);
        emitter.load_x(16U, kStackPointer, kRuntimeDispatchOffset);
        emitter.branch_link_register(16U);
        emitter.load_w(kBudgetInitial,
                       kStackPointer,
                       kBudgetInitialOffset);
        emitter.load_w(kBudgetRemaining,
                       kStackPointer,
                       kBudgetRemainingOffset);
        emitter.load_w(kScratchFourth,
                       kStackPointer,
                       static_cast<u32>(kJitRuntimeConsumedByteOffset));
        emitter.compare_w(kBudgetRemaining, kScratchFourth);
        budget_patches.push_back(
            emitter.emit_conditional_branch_placeholder(
                Arm64Condition::unsigned_lower));
        emitter.sub_w(kBudgetRemaining,
                      kBudgetRemaining,
                      kScratchFourth);
        emitter.load_x(kResultPointer,
                       kStackPointer,
                       kResultPointerOffset);
        emitter.store_w(0U, kResultPointer, 0U);
        emitter.move_imm32(kScratchThird, bytecode_pc);
        emitter.store_w(kScratchThird, kResultPointer, 4U);
        emitter.compare_zero_w(0U);
        const usize failure_branch =
            emitter.emit_conditional_branch_placeholder(
                Arm64Condition::not_equal);
        std::vector<u16> handler_pcs;
        for (const classfile::ExceptionHandler& handler :
             method.code->exception_table) {
            if (static_cast<usize>(bytecode_pc) >=
                    static_cast<usize>(handler.start_pc) &&
                static_cast<usize>(bytecode_pc) <
                    static_cast<usize>(handler.end_pc)) {
                handler_pcs.push_back(handler.handler_pc);
            }
        }
        if (handler_pcs.empty()) {
            runtime_failure_patches.push_back(failure_branch);
        } else {
            std::sort(handler_pcs.begin(), handler_pcs.end());
            handler_pcs.erase(
                std::unique(handler_pcs.begin(), handler_pcs.end()),
                handler_pcs.end());
            runtime_exception_sites.push_back(RuntimeExceptionSite {
                .failure_branch = failure_branch,
                .bytecode_pc = bytecode_pc,
                .safepoint_id = safepoint_id,
                .handler_pcs = std::move(handler_pcs),
            });
        }
        emitter.load_x(kScratchLeft,
                       kStackPointer,
                       kRuntimeResultOffset);
    };

    // Budget exhaustion routes through a runtime call so the dispatch
    // trampoline captures an exact deopt frame at this pc. The interpreter
    // then resumes the method at the boundary instead of unwinding, which is
    // what lets scheduler-owned entries with effectively unbounded budgets
    // still enter compiled code (see Machine::safe_jit_instruction_budget).
    const auto emit_budget_guard = [&](u32 cost, u32 bytecode_pc) -> bool {
        // The guard's runtime call needs the dispatch trampoline so the deopt
        // frame is captured at this pc. Without it a budget exhaustion would
        // fall into the no-state deopt path and re-execute side effects.
        requires_runtime_dispatch = true;
        const u32 original_cost = cost;
        std::vector<usize> exhausted_branches;
        while (cost != 0U) {
            const u32 chunk = std::min(cost, 4'095U);
            emitter.compare_imm_w(kBudgetRemaining, chunk);
            exhausted_branches.push_back(
                emitter.emit_conditional_branch_placeholder(
                    Arm64Condition::unsigned_lower));
            emitter.sub_imm_w(kBudgetRemaining,
                              kBudgetRemaining,
                              chunk);
            cost -= chunk;
        }
        if (exhausted_branches.empty()) return true;
        const usize continue_branch = emitter.emit_branch_placeholder();
        const usize slow_path = emitter.position();
        emit_runtime_call(JitRuntimeOperation::budget_safepoint,
                          0U,
                          31U,
                          31U,
                          31U,
                          bytecode_pc);
        // In total-budget mode the runtime returns a deopt status and the
        // failure edge above captures/resumes in the interpreter. A
        // progress-watchdog invocation may instead replenish its native JIT
        // window and return success; charge this instruction against that
        // fresh window before continuing compiled execution.
        u32 slow_cost = original_cost;
        while (slow_cost != 0U) {
            const u32 chunk = std::min(slow_cost, 4'095U);
            emitter.sub_imm_w(kBudgetRemaining,
                              kBudgetRemaining,
                              chunk);
            slow_cost -= chunk;
        }
        const usize resume_position = emitter.position();
        if (!emitter.patch_branch(continue_branch, resume_position))
            return false;
        for (const usize branch : exhausted_branches) {
            if (!emitter.patch_conditional_branch(
                    branch, slow_path, Arm64Condition::unsigned_lower))
                return false;
        }
        return true;
    };

    for (usize instruction_index = 0;
         instruction_index < decoded->size();
         ++instruction_index) {
        if (!(*depths)[instruction_index].has_value()) continue;
        const DecodedInstruction& instruction = (*decoded)[instruction_index];
        const InstructionOptimization& optimization =
            optimization_plan.instructions[instruction_index];
        active_safepoint_depth = *(*depths)[instruction_index];
        native_position_by_pc.insert_or_assign(
            instruction.pc, emitter.position());
        bool loop_preheader_emitted = false;
        for (const LicmExpression& invariant :
             optimization_plan.licm_expressions) {
            if (invariant.loop_header_pc != instruction.pc) continue;
            if (const auto cached = cached_local_register(
                    invariant.left_local, CachedLocalKind::int32);
                cached.has_value()) {
                emitter.move_w(kScratchLeft, *cached);
            } else {
                emitter.load_w(kScratchLeft,
                               kStackPointer,
                               local_offset(invariant.left_local));
            }
            if (const auto cached = cached_local_register(
                    invariant.right_local, CachedLocalKind::int32);
                cached.has_value()) {
                emitter.move_w(kScratchRight, *cached);
            } else {
                emitter.load_w(kScratchRight,
                               kStackPointer,
                               local_offset(invariant.right_local));
            }
            switch (invariant.opcode) {
            case 0x60U:
                emitter.add_w(kScratchLeft, kScratchLeft, kScratchRight);
                break;
            case 0x64U:
                emitter.sub_w(kScratchLeft, kScratchLeft, kScratchRight);
                break;
            case 0x68U:
                emitter.multiply_w(kScratchLeft, kScratchLeft, kScratchRight);
                break;
            case 0x78U:
                emitter.shift_left_w(kScratchLeft, kScratchLeft, kScratchRight);
                break;
            case 0x7AU:
                emitter.shift_right_arithmetic_w(
                    kScratchLeft, kScratchLeft, kScratchRight);
                break;
            case 0x7CU:
                emitter.shift_right_logical_w(
                    kScratchLeft, kScratchLeft, kScratchRight);
                break;
            case 0x7EU:
                emitter.and_w(kScratchLeft, kScratchLeft, kScratchRight);
                break;
            case 0x80U:
                emitter.or_w(kScratchLeft, kScratchLeft, kScratchRight);
                break;
            case 0x82U:
                emitter.xor_w(kScratchLeft, kScratchLeft, kScratchRight);
                break;
            default:
                return {};
            }
            emitter.move_w(invariant.register_index, kScratchLeft);
            loop_preheader_emitted = true;
        }
        for (const ArrayLoopLease& lease : optimization_plan.array_leases) {
            if (lease.loop_header_pc != instruction.pc) continue;
            if (const auto cached = cached_local_register(
                    lease.array_local, CachedLocalKind::reference);
                cached.has_value()) {
                emitter.move_x(kScratchLeft, *cached);
            } else {
                emitter.load_x(kScratchLeft,
                               kStackPointer,
                               local_offset(lease.array_local));
            }
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::array_payload_lease,
                              lease.lease_opcode,
                              kScratchLeft,
                              31U,
                              31U,
                              static_cast<u32>(instruction.pc));
            emitter.move_x(lease.pointer_register, kScratchLeft);
            loop_preheader_emitted = true;
        }
        if (loop_preheader_emitted) {
            loop_body_position_by_pc.insert_or_assign(
                instruction.pc, emitter.position());
        }

        // Four-way unroll for the exact canonical int-array reduction proven
        // by the optimizer. int[] now uses its natural 4-byte packed payload;
        // no per-element ValueKind tag or 64-bit padding is read.
        // total/index remain in cached locals; length may stay in the frame.
        if (optimization_plan.unrolled_int_array_reduction.has_value() &&
            optimization_plan.unrolled_int_array_reduction->loop_header_pc ==
                instruction.pc) {
            const UnrolledIntArrayReduction& reduction =
                *optimization_plan.unrolled_int_array_reduction;
            const auto total_register = cached_local_register(
                reduction.total_local, CachedLocalKind::int32);
            const auto index_register = cached_local_register(
                reduction.index_local, CachedLocalKind::int32);
            const auto length_register = cached_local_register(
                reduction.length_local, CachedLocalKind::int32);
            if (total_register.has_value() && index_register.has_value()) {
                const auto load_length = [&](u32 target) {
                    if (length_register.has_value()) {
                        emitter.move_w(target, *length_register);
                    } else {
                        emitter.load_w(target,
                                       kStackPointer,
                                       local_offset(reduction.length_local));
                    }
                };
                const auto emit_element_address = [&]() {
                    emitter.move_imm32(kScratchRight, 2U);
                    emitter.shift_left_x(kScratchLeft,
                                         *index_register,
                                         kScratchRight);
                    emitter.add_x(kScratchLeft,
                                  kArrayLeaseRegister,
                                  kScratchLeft);
                };

                const usize vector_loop = emitter.position();
                load_length(kScratchThird);
                emitter.compare_w(*index_register, kScratchThird);
                const usize already_done =
                    emitter.emit_conditional_branch_placeholder(
                        Arm64Condition::greater_equal);
                emitter.sub_w(kScratchFourth,
                              kScratchThird,
                              *index_register);
                emitter.compare_imm_w(kScratchFourth, 4U);
                const usize to_tail =
                    emitter.emit_conditional_branch_placeholder(
                        Arm64Condition::less_than);

                if (!emit_budget_guard(reduction.iteration_bytecodes * 4U,
                                        static_cast<u32>(instruction.pc)))
                    return {};
                emit_element_address();
                emitter.load_w(kScratchRight, kScratchLeft, 0U);
                emitter.add_w(*total_register,
                              *total_register,
                              kScratchRight);
                emitter.load_w(kScratchRight, kScratchLeft, 4U);
                emitter.add_w(*total_register,
                              *total_register,
                              kScratchRight);
                emitter.load_w(kScratchRight, kScratchLeft, 8U);
                emitter.add_w(*total_register,
                              *total_register,
                              kScratchRight);
                emitter.load_w(kScratchRight, kScratchLeft, 12U);
                emitter.add_w(*total_register,
                              *total_register,
                              kScratchRight);
                emitter.move_imm32(kScratchRight, 4U);
                emitter.add_w(*index_register,
                              *index_register,
                              kScratchRight);
                const usize vector_back = emitter.emit_branch_placeholder();
                if (!emitter.patch_branch(vector_back, vector_loop)) return {};

                const usize tail_loop = emitter.position();
                if (!emitter.patch_conditional_branch(
                        to_tail,
                        tail_loop,
                        Arm64Condition::less_than)) {
                    return {};
                }
                load_length(kScratchThird);
                emitter.compare_w(*index_register, kScratchThird);
                const usize tail_done =
                    emitter.emit_conditional_branch_placeholder(
                        Arm64Condition::greater_equal);
                if (!emit_budget_guard(reduction.iteration_bytecodes,
                                        static_cast<u32>(instruction.pc)))
                    return {};
                emit_element_address();
                emitter.load_w(kScratchRight, kScratchLeft, 0U);
                emitter.add_w(*total_register,
                              *total_register,
                              kScratchRight);
                emitter.move_imm32(kScratchRight, 1U);
                emitter.add_w(*index_register,
                              *index_register,
                              kScratchRight);
                const usize tail_back = emitter.emit_branch_placeholder();
                if (!emitter.patch_branch(tail_back, tail_loop)) return {};

                const usize final_check = emitter.position();
                if (!emitter.patch_conditional_branch(
                        already_done,
                        final_check,
                        Arm64Condition::greater_equal) ||
                    !emitter.patch_conditional_branch(
                        tail_done,
                        final_check,
                        Arm64Condition::greater_equal)) {
                    return {};
                }
                if (!emit_budget_guard(reduction.final_check_bytecodes,
                                        static_cast<u32>(instruction.pc)))
                    return {};
                branch_patches.push_back(BranchPatch {
                    .kind = BranchPatch::Kind::unconditional,
                    .native_index = emitter.emit_branch_placeholder(),
                    .target_pc = reduction.exit_pc,
                    .condition = Arm64Condition::equal,
                });
                ++unrolled_int_array_loops;
            }
        }

        if (!emit_budget_guard(
                optimization_plan.block_budget_costs[instruction_index],
                static_cast<u32>(instruction.pc)))
            return {};

        u32 depth = active_safepoint_depth;
        const auto push_register = [&](u32 source) -> bool {
            if (depth >= stack_slots) return false;
            if (const auto cached = cached_stack_register(depth);
                cached.has_value()) {
                emitter.move_w(*cached, source);
                ++stack_cached_stores_elided;
            } else {
                emitter.store_w(source, kStackPointer, stack_offset(depth));
            }
            ++depth;
            return true;
        };
        const auto pop_register = [&](u32 target) -> bool {
            if (depth == 0U) return false;
            --depth;
            if (const auto cached = cached_stack_register(depth);
                cached.has_value()) {
                emitter.move_w(target, *cached);
                ++stack_cached_pops;
            } else {
                emitter.load_w(target, kStackPointer, stack_offset(depth));
            }
            return true;
        };
        const auto push_reference_register = [&](u32 source) -> bool {
            if (depth >= stack_slots) return false;
            if (const auto cached = cached_stack_register(depth);
                cached.has_value()) {
                emitter.move_x(*cached, source);
                ++stack_cached_stores_elided;
            } else {
                emitter.store_x(source, kStackPointer, stack_offset(depth));
            }
            ++depth;
            return true;
        };
        const auto pop_reference_register = [&](u32 target) -> bool {
            if (depth == 0U) return false;
            --depth;
            if (const auto cached = cached_stack_register(depth);
                cached.has_value()) {
                emitter.move_x(target, *cached);
                ++stack_cached_pops;
            } else {
                emitter.load_x(target, kStackPointer, stack_offset(depth));
            }
            return true;
        };
        const auto push_long_register = [&](u32 source) -> bool {
            if (depth > stack_slots || 2U > stack_slots - depth) return false;
            if (const auto cached = cached_stack_register(depth);
                cached.has_value()) {
                emitter.move_x(*cached, source);
                ++stack_cached_stores_elided;
            } else {
                emitter.store_x(source, kStackPointer, stack_offset(depth));
            }
            depth += 2U;
            return true;
        };
        const auto pop_long_register = [&](u32 target) -> bool {
            if (depth < 2U) return false;
            depth -= 2U;
            if (const auto cached = cached_stack_register(depth);
                cached.has_value()) {
                emitter.move_x(target, *cached);
                ++stack_cached_pops;
            } else {
                emitter.load_x(target, kStackPointer, stack_offset(depth));
            }
            return true;
        };
        const auto pop_typed_register = [&](JavaTypeKind kind,
                                            u32 target) -> bool {
            if (kind == JavaTypeKind::long_integer ||
                kind == JavaTypeKind::float64) {
                return pop_long_register(target);
            }
            if (kind == JavaTypeKind::reference || kind == JavaTypeKind::array) {
                return pop_reference_register(target);
            }
            return pop_register(target);
        };
        const auto emit_binary = [&](auto operation) -> bool {
            if (!pop_register(kScratchRight) ||
                !pop_register(kScratchLeft)) {
                return false;
            }
            operation(kScratchLeft, kScratchLeft, kScratchRight);
            return push_register(kScratchLeft);
        };
        const auto emit_long_binary = [&](auto operation) -> bool {
            if (!pop_long_register(kScratchRight) ||
                !pop_long_register(kScratchLeft)) {
                return false;
            }
            operation(kScratchLeft, kScratchLeft, kScratchRight);
            return push_long_register(kScratchLeft);
        };
        const auto emit_float_binary = [&](auto operation) -> bool {
            if (!pop_register(kScratchRight) ||
                !pop_register(kScratchLeft)) {
                return false;
            }
            emitter.move_w_to_float(kScratchLeft, kScratchLeft);
            emitter.move_w_to_float(kScratchRight, kScratchRight);
            operation(kScratchLeft, kScratchLeft, kScratchRight);
            emitter.move_float_to_w(kScratchLeft, kScratchLeft);
            return push_register(kScratchLeft);
        };
        const auto emit_double_binary = [&](auto operation) -> bool {
            if (!pop_long_register(kScratchRight) ||
                !pop_long_register(kScratchLeft)) {
                return false;
            }
            emitter.move_x_to_double(kScratchLeft, kScratchLeft);
            emitter.move_x_to_double(kScratchRight, kScratchRight);
            operation(kScratchLeft, kScratchLeft, kScratchRight);
            emitter.move_double_to_x(kScratchLeft, kScratchLeft);
            return push_long_register(kScratchLeft);
        };
        const auto emit_runtime_unary_scalar = [&](JitRuntimeOperation operation,
                                                   bool input_wide,
                                                   bool output_wide) -> bool {
            const bool popped = input_wide
                ? pop_long_register(kScratchLeft)
                : pop_register(kScratchLeft);
            if (!popped) return false;
            requires_runtime_dispatch = true;
            emit_runtime_call(operation,
                              0U,
                              kScratchLeft,
                              31U,
                              31U,
                              static_cast<u32>(instruction.pc));
            return output_wide
                ? push_long_register(kScratchLeft)
                : push_register(kScratchLeft);
        };
        const auto emit_runtime_binary_scalar = [&](JitRuntimeOperation operation,
                                                    bool input_wide,
                                                    bool output_wide) -> bool {
            const bool right_popped = input_wide
                ? pop_long_register(kScratchRight)
                : pop_register(kScratchRight);
            const bool left_popped = input_wide
                ? pop_long_register(kScratchLeft)
                : pop_register(kScratchLeft);
            if (!right_popped || !left_popped) return false;
            requires_runtime_dispatch = true;
            emit_runtime_call(operation,
                              0U,
                              kScratchLeft,
                              kScratchRight,
                              31U,
                              static_cast<u32>(instruction.pc));
            return output_wide
                ? push_long_register(kScratchLeft)
                : push_register(kScratchLeft);
        };
        const auto replace_stack_with_constant = [&](u32 popped,
                                                      i32 value) -> bool {
            if (depth < popped) return false;
            depth -= popped;
            emitter.move_imm32(kScratchLeft, static_cast<u32>(value));
            return push_register(kScratchLeft);
        };
        const auto emit_right_constant_binary = [&](u8 opcode,
                                                    i32 constant) -> bool {
            if (depth < 2U ||
                ((opcode == 0x6CU || opcode == 0x70U) && constant == 0)) {
                return false;
            }
            depth -= 2U;
            if (const auto cached = cached_stack_register(depth);
                cached.has_value()) {
                emitter.move_w(kScratchLeft, *cached);
                ++stack_cached_pops;
            } else {
                emitter.load_w(kScratchLeft,
                               kStackPointer,
                               stack_offset(depth));
            }

            switch (opcode) {
            case 0x60U:
                if (constant != 0) {
                    emitter.move_imm32(kScratchRight,
                                       static_cast<u32>(constant));
                    emitter.add_w(kScratchLeft,
                                  kScratchLeft,
                                  kScratchRight);
                }
                break;
            case 0x64U:
                if (constant != 0) {
                    emitter.move_imm32(kScratchRight,
                                       static_cast<u32>(constant));
                    emitter.sub_w(kScratchLeft,
                                  kScratchLeft,
                                  kScratchRight);
                }
                break;
            case 0x68U: {
                const u32 magnitude = constant < 0
                    ? 0U - static_cast<u32>(constant)
                    : static_cast<u32>(constant);
                if (constant == 0) {
                    emitter.move_imm32(kScratchLeft, 0U);
                } else if (constant == 1) {
                    // Identity.
                } else if (constant == -1) {
                    emitter.negate_w(kScratchLeft, kScratchLeft);
                } else if (std::has_single_bit(magnitude)) {
                    emitter.move_imm32(
                        kScratchRight,
                        static_cast<u32>(std::countr_zero(magnitude)));
                    emitter.shift_left_w(kScratchLeft,
                                         kScratchLeft,
                                         kScratchRight);
                    if (constant < 0) {
                        emitter.negate_w(kScratchLeft, kScratchLeft);
                    }
                } else {
                    emitter.move_imm32(kScratchRight,
                                       static_cast<u32>(constant));
                    emitter.multiply_w(kScratchLeft,
                                       kScratchLeft,
                                       kScratchRight);
                }
                break;
            }
            case 0x6CU:
                if (constant == 1) {
                    // Identity.
                } else if (constant == -1) {
                    emitter.negate_w(kScratchLeft, kScratchLeft);
                } else {
                    emitter.move_imm32(kScratchRight,
                                       static_cast<u32>(constant));
                    emitter.divide_signed_w(kScratchLeft,
                                            kScratchLeft,
                                            kScratchRight);
                }
                break;
            case 0x70U:
                if (constant == 1 || constant == -1) {
                    emitter.move_imm32(kScratchLeft, 0U);
                } else {
                    emitter.move_imm32(kScratchRight,
                                       static_cast<u32>(constant));
                    emitter.divide_signed_w(kScratchThird,
                                            kScratchLeft,
                                            kScratchRight);
                    emitter.multiply_subtract_w(kScratchLeft,
                                                kScratchThird,
                                                kScratchRight,
                                                kScratchLeft);
                }
                break;
            case 0x78U:
            case 0x7AU:
            case 0x7CU:
                emitter.move_imm32(
                    kScratchRight, static_cast<u32>(constant) & 31U);
                if (opcode == 0x78U) {
                    emitter.shift_left_w(kScratchLeft,
                                         kScratchLeft,
                                         kScratchRight);
                } else if (opcode == 0x7AU) {
                    emitter.shift_right_arithmetic_w(kScratchLeft,
                                                     kScratchLeft,
                                                     kScratchRight);
                } else {
                    emitter.shift_right_logical_w(kScratchLeft,
                                                  kScratchLeft,
                                                  kScratchRight);
                }
                break;
            case 0x7EU:
                if (constant == 0) {
                    emitter.move_imm32(kScratchLeft, 0U);
                } else if (constant != -1) {
                    emitter.move_imm32(kScratchRight,
                                       static_cast<u32>(constant));
                    emitter.and_w(kScratchLeft,
                                  kScratchLeft,
                                  kScratchRight);
                }
                break;
            case 0x80U:
                if (constant == -1) {
                    emitter.move_imm32(kScratchLeft, 0xFFFF'FFFFU);
                } else if (constant != 0) {
                    emitter.move_imm32(kScratchRight,
                                       static_cast<u32>(constant));
                    emitter.or_w(kScratchLeft,
                                 kScratchLeft,
                                 kScratchRight);
                }
                break;
            case 0x82U:
                if (constant != 0) {
                    emitter.move_imm32(kScratchRight,
                                       static_cast<u32>(constant));
                    emitter.xor_w(kScratchLeft,
                                  kScratchLeft,
                                  kScratchRight);
                }
                break;
            default:
                return false;
            }
            ++strength_reductions;
            return push_register(kScratchLeft);
        };

        if (optimization.gvn_elide_input_push ||
            optimization.licm_elide_input_push ||
            optimization.array_lease_elide_input_push) {
            if (depth >= stack_slots) return {};
            ++depth;
        } else if (optimization.scalar_array_store_local.has_value()) {
            const u32 scalar_local = *optimization.scalar_array_store_local;
            if (!pop_register(kScratchThird) ||
                !pop_register(kScratchRight) ||
                !pop_reference_register(kScratchLeft)) {
                return {};
            }
            if (const auto cached = cached_local_register(
                    scalar_local, CachedLocalKind::reference);
                cached.has_value()) {
                emitter.move_w(*cached, kScratchThird);
                ++register_cached_stores_elided;
            } else {
                emitter.store_w(kScratchThird,
                                kStackPointer,
                                local_offset(scalar_local));
            }
        } else if (optimization.scalar_array_load_local.has_value()) {
            const u32 scalar_local = *optimization.scalar_array_load_local;
            if (!pop_register(kScratchRight) ||
                !pop_reference_register(kScratchLeft)) {
                return {};
            }
            if (const auto cached = cached_local_register(
                    scalar_local, CachedLocalKind::reference);
                cached.has_value()) {
                emitter.move_w(kScratchLeft, *cached);
            } else {
                emitter.load_w(kScratchLeft,
                               kStackPointer,
                               local_offset(scalar_local));
            }
            if (!push_register(kScratchLeft)) return {};
        } else if (optimization.array_lease_store_register.has_value()) {
            if (!optimization.array_lease_store_opcode.has_value()) return {};
            const u8 store_opcode = *optimization.array_lease_store_opcode;
            const bool wide_value = store_opcode == 0x50U || store_opcode == 0x52U;
            const bool value_popped = wide_value
                ? pop_long_register(kScratchThird)
                : pop_register(kScratchThird);
            if (!value_popped || !pop_register(kScratchRight) ||
                !pop_reference_register(kScratchLeft)) {
                return {};
            }
            const u32 element_shift =
                (store_opcode == 0x4FU || store_opcode == 0x51U) ? 2U : 3U;
            emitter.move_imm32(kScratchFourth, element_shift);
            emitter.shift_left_x(kScratchRight,
                                 kScratchRight,
                                 kScratchFourth);
            emitter.add_x(kScratchLeft,
                          *optimization.array_lease_store_register,
                          kScratchRight);
            if (element_shift == 2U) {
                emitter.store_w(kScratchThird, kScratchLeft, 0U);
            } else {
                emitter.store_x(kScratchThird, kScratchLeft, 0U);
            }
        } else if (optimization.array_lease_register.has_value()) {
            if (depth < 2U ||
                !optimization.array_lease_index_local.has_value() ||
                !optimization.array_lease_opcode.has_value()) {
                return {};
            }
            depth -= 2U;
            const u32 index_local = *optimization.array_lease_index_local;
            if (const auto cached = cached_local_register(
                    index_local, CachedLocalKind::int32);
                cached.has_value()) {
                emitter.move_w(kScratchRight, *cached);
            } else {
                emitter.load_w(kScratchRight,
                               kStackPointer,
                               local_offset(index_local));
            }
            const u8 array_opcode = *optimization.array_lease_opcode;
            const u32 element_shift =
                (array_opcode == 0x2EU || array_opcode == 0x30U) ? 2U : 3U;
            emitter.move_imm32(kScratchThird, element_shift);
            emitter.shift_left_x(kScratchRight,
                                 kScratchRight,
                                 kScratchThird);
            emitter.add_x(kScratchThird,
                          *optimization.array_lease_register,
                          kScratchRight);
            if (element_shift == 2U) {
                emitter.load_w(kScratchLeft, kScratchThird, 0U);
            } else {
                emitter.load_x(kScratchLeft, kScratchThird, 0U);
            }
            bool pushed = false;
            switch (array_opcode) {
            case 0x2EU: // iaload
            case 0x30U: // faload
                pushed = push_register(kScratchLeft);
                break;
            case 0x2FU: // laload
            case 0x31U: // daload
                pushed = push_long_register(kScratchLeft);
                break;
            case 0x32U: // aaload
                pushed = push_reference_register(kScratchLeft);
                break;
            default:
                return {};
            }
            if (!pushed) return {};
        } else if (optimization.licm_reuse_register.has_value() ||
                   optimization.gvn_reuse_register.has_value()) {
            if (depth < 2U) return {};
            depth -= 2U;
            const u32 reuse_register =
                optimization.licm_reuse_register.has_value()
                    ? *optimization.licm_reuse_register
                    : *optimization.gvn_reuse_register;
            emitter.move_w(kScratchLeft, reuse_register);
            if (!push_register(kScratchLeft)) return {};
        } else {
        switch (instruction.opcode) {
        case 0x00U:
            break;
        case 0x01U:
            emitter.move_imm32(kScratchLeft, 0U);
            if (!push_reference_register(kScratchLeft)) return {};
            break;
        case 0x02U:
        case 0x03U:
        case 0x04U:
        case 0x05U:
        case 0x06U:
        case 0x07U:
        case 0x08U: {
            const i32 value = static_cast<i32>(instruction.opcode) - 3;
            if (optimization.elide_push) {
                if (depth >= stack_slots) return {};
                ++depth;
            } else {
                emitter.move_imm32(kScratchLeft, static_cast<u32>(value));
                if (!push_register(kScratchLeft)) return {};
            }
            break;
        }
        case 0x09U:
        case 0x0AU:
            emitter.move_imm64(
                kScratchLeft,
                static_cast<u64>(instruction.opcode - 0x09U));
            if (!push_long_register(kScratchLeft)) return {};
            break;
        case 0x0BU:
        case 0x0CU:
        case 0x0DU:
            emitter.move_imm32(
                kScratchLeft,
                std::bit_cast<u32>(
                    static_cast<float>(instruction.opcode - 0x0BU)));
            if (!push_register(kScratchLeft)) return {};
            break;
        case 0x0EU:
        case 0x0FU:
            emitter.move_imm64(
                kScratchLeft,
                std::bit_cast<u64>(
                    static_cast<double>(instruction.opcode - 0x0EU)));
            if (!push_long_register(kScratchLeft)) return {};
            break;
        case 0x10U:
        case 0x11U:
            if (optimization.elide_push) {
                if (depth >= stack_slots) return {};
                ++depth;
            } else {
                emitter.move_imm32(
                    kScratchLeft, static_cast<u32>(instruction.immediate));
                if (!push_register(kScratchLeft)) return {};
            }
            break;
        case 0x12U:
        case 0x13U:
            if (instruction.value_kind == JavaTypeKind::reference) {
                requires_runtime_dispatch = true;
                emit_runtime_call(JitRuntimeOperation::load_constant,
                                  instruction.local_index,
                                  31U,
                                  31U,
                                  31U,
                                  static_cast<u32>(instruction.pc));
                if (!push_reference_register(kScratchLeft)) return {};
            } else if (instruction.value_kind == JavaTypeKind::float32) {
                emitter.move_imm32(
                    kScratchLeft,
                    static_cast<u32>(instruction.wide_immediate));
                if (!push_register(kScratchLeft)) return {};
            } else if (optimization.elide_push) {
                if (depth >= stack_slots) return {};
                ++depth;
            } else {
                emitter.move_imm32(
                    kScratchLeft, static_cast<u32>(instruction.immediate));
                if (!push_register(kScratchLeft)) return {};
            }
            break;
        case 0x14U:
            emitter.move_imm64(kScratchLeft, instruction.wide_immediate);
            if (!push_long_register(kScratchLeft)) return {};
            break;
        case 0x15U:
            if (optimization.folded_result.has_value()) {
                emitter.move_imm32(
                    kScratchLeft,
                    static_cast<u32>(*optimization.folded_result));
            } else if (const auto cached = cached_local_register(
                           instruction.local_index,
                           CachedLocalKind::int32);
                       cached.has_value()) {
                emitter.move_w(kScratchLeft, *cached);
            } else {
                emitter.load_w(kScratchLeft,
                               kStackPointer,
                               local_offset(instruction.local_index));
            }
            if (!push_register(kScratchLeft)) return {};
            break;
        case 0x16U:
            if (const auto cached = cached_local_register(
                    instruction.local_index,
                    CachedLocalKind::int64);
                cached.has_value()) {
                emitter.move_x(kScratchLeft, *cached);
            } else {
                emitter.load_x(kScratchLeft,
                               kStackPointer,
                               local_offset(instruction.local_index));
            }
            if (!push_long_register(kScratchLeft)) return {};
            break;
        case 0x18U:
            if (const auto cached = cached_local_register(
                    instruction.local_index,
                    CachedLocalKind::float64);
                cached.has_value()) {
                emitter.move_x(kScratchLeft, *cached);
            } else {
                emitter.load_x(kScratchLeft,
                               kStackPointer,
                               local_offset(instruction.local_index));
            }
            if (!push_long_register(kScratchLeft)) return {};
            break;
        case 0x17U:
            if (const auto cached = cached_local_register(
                    instruction.local_index,
                    CachedLocalKind::float32);
                cached.has_value()) {
                emitter.move_w(kScratchLeft, *cached);
            } else {
                emitter.load_w(kScratchLeft,
                               kStackPointer,
                               local_offset(instruction.local_index));
            }
            if (!push_register(kScratchLeft)) return {};
            break;
        case 0x1AU:
        case 0x1BU:
        case 0x1CU:
        case 0x1DU: {
            const u32 index = static_cast<u32>(instruction.opcode - 0x1AU);
            if (optimization.folded_result.has_value()) {
                emitter.move_imm32(
                    kScratchLeft,
                    static_cast<u32>(*optimization.folded_result));
            } else if (const auto cached = cached_local_register(
                           index, CachedLocalKind::int32);
                       cached.has_value()) {
                emitter.move_w(kScratchLeft, *cached);
            } else {
                emitter.load_w(kScratchLeft,
                               kStackPointer,
                               local_offset(index));
            }
            if (!push_register(kScratchLeft)) return {};
            break;
        }
        case 0x1EU:
        case 0x1FU:
        case 0x20U:
        case 0x21U: {
            const u32 index = static_cast<u32>(instruction.opcode - 0x1EU);
            if (const auto cached = cached_local_register(
                    index, CachedLocalKind::int64);
                cached.has_value()) {
                emitter.move_x(kScratchLeft, *cached);
            } else {
                emitter.load_x(kScratchLeft, kStackPointer, local_offset(index));
            }
            if (!push_long_register(kScratchLeft)) return {};
            break;
        }
        case 0x22U:
        case 0x23U:
        case 0x24U:
        case 0x25U: {
            const u32 index = static_cast<u32>(instruction.opcode - 0x22U);
            if (const auto cached = cached_local_register(
                    index, CachedLocalKind::float32);
                cached.has_value()) {
                emitter.move_w(kScratchLeft, *cached);
            } else {
                emitter.load_w(kScratchLeft, kStackPointer, local_offset(index));
            }
            if (!push_register(kScratchLeft)) return {};
            break;
        }
        case 0x26U:
        case 0x27U:
        case 0x28U:
        case 0x29U: {
            const u32 index = static_cast<u32>(instruction.opcode - 0x26U);
            if (const auto cached = cached_local_register(
                    index, CachedLocalKind::float64);
                cached.has_value()) {
                emitter.move_x(kScratchLeft, *cached);
            } else {
                emitter.load_x(kScratchLeft, kStackPointer, local_offset(index));
            }
            if (!push_long_register(kScratchLeft)) return {};
            break;
        }
        case 0x19U:
            if (const auto cached = cached_local_register(
                    instruction.local_index,
                    CachedLocalKind::reference);
                cached.has_value()) {
                emitter.move_x(kScratchLeft, *cached);
            } else {
                emitter.load_x(kScratchLeft,
                               kStackPointer,
                               local_offset(instruction.local_index));
            }
            if (!push_reference_register(kScratchLeft)) return {};
            break;
        case 0x2AU:
        case 0x2BU:
        case 0x2CU:
        case 0x2DU: {
            const u32 index = static_cast<u32>(instruction.opcode - 0x2AU);
            if (const auto cached = cached_local_register(
                    index, CachedLocalKind::reference);
                cached.has_value()) {
                emitter.move_x(kScratchLeft, *cached);
            } else {
                emitter.load_x(kScratchLeft, kStackPointer, local_offset(index));
            }
            if (!push_reference_register(kScratchLeft)) return {};
            break;
        }
        case 0x2EU:
        case 0x2FU:
        case 0x30U:
        case 0x31U:
        case 0x32U:
        case 0x33U:
        case 0x34U:
        case 0x35U:
            if (!pop_register(kScratchRight) ||
                !pop_reference_register(kScratchLeft)) {
                return {};
            }
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::array_load,
                              instruction.opcode,
                              kScratchLeft,
                              kScratchRight,
                              31U,
                              static_cast<u32>(instruction.pc));
            if (instruction.opcode == 0x2FU ||
                instruction.opcode == 0x31U) {
                if (!push_long_register(kScratchLeft)) return {};
            } else if (instruction.opcode == 0x32U) {
                if (!push_reference_register(kScratchLeft)) return {};
            } else if (!push_register(kScratchLeft)) {
                return {};
            }
            break;
        case 0x4FU:
        case 0x50U:
        case 0x51U:
        case 0x52U:
        case 0x53U:
        case 0x54U:
        case 0x55U:
        case 0x56U: {
            bool value_popped = false;
            if (instruction.opcode == 0x50U ||
                instruction.opcode == 0x52U) {
                value_popped = pop_long_register(kScratchThird);
            } else if (instruction.opcode == 0x53U) {
                value_popped = pop_reference_register(kScratchThird);
            } else {
                value_popped = pop_register(kScratchThird);
            }
            if (!value_popped || !pop_register(kScratchRight) ||
                !pop_reference_register(kScratchLeft)) {
                return {};
            }
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::array_store,
                              instruction.opcode,
                              kScratchLeft,
                              kScratchRight,
                              kScratchThird,
                              static_cast<u32>(instruction.pc));
            break;
        }
        case 0xB2U:
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::get_static,
                              instruction.local_index,
                              31U,
                              31U,
                              31U,
                              static_cast<u32>(instruction.pc));
            if (instruction.value_kind == JavaTypeKind::long_integer ||
                instruction.value_kind == JavaTypeKind::float64) {
                if (!push_long_register(kScratchLeft)) return {};
            } else if (instruction.value_kind == JavaTypeKind::reference ||
                       instruction.value_kind == JavaTypeKind::array) {
                if (!push_reference_register(kScratchLeft)) return {};
            } else if (!push_register(kScratchLeft)) {
                return {};
            }
            break;
        case 0xB3U:
            if (!pop_typed_register(instruction.value_kind, kScratchLeft)) {
                return {};
            }
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::put_static,
                              instruction.local_index,
                              kScratchLeft,
                              31U,
                              31U,
                              static_cast<u32>(instruction.pc));
            break;
        case 0xB4U:
            if (!pop_reference_register(kScratchLeft)) return {};
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::get_field,
                              instruction.local_index,
                              kScratchLeft,
                              31U,
                              31U,
                              static_cast<u32>(instruction.pc));
            if (instruction.value_kind == JavaTypeKind::long_integer ||
                instruction.value_kind == JavaTypeKind::float64) {
                if (!push_long_register(kScratchLeft)) return {};
            } else if (instruction.value_kind == JavaTypeKind::reference ||
                       instruction.value_kind == JavaTypeKind::array) {
                if (!push_reference_register(kScratchLeft)) return {};
            } else if (!push_register(kScratchLeft)) {
                return {};
            }
            break;
        case 0xB5U:
            if (!pop_typed_register(instruction.value_kind, kScratchRight) ||
                !pop_reference_register(kScratchLeft)) {
                return {};
            }
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::put_field,
                              instruction.local_index,
                              kScratchLeft,
                              kScratchRight,
                              31U,
                              static_cast<u32>(instruction.pc));
            break;
        case 0xB6U:
        case 0xB7U:
        case 0xB8U:
        case 0xB9U:
        case 0xBAU: {
            if (instruction.opcode == 0xB8U || instruction.opcode == 0xB7U ||
                instruction.opcode == 0xB6U) {
                const auto inline_found = inline_recipes.find(instruction_index);
                if (inline_found != inline_recipes.end()) {
                    const InlineRecipe& recipe = inline_found->second;
                    if (!emit_budget_guard(recipe.nested_bytecode_cost,
                                            static_cast<u32>(instruction.pc)))
                        return {};
                    const auto emit_inline_binary = [&](u8 opcode) -> bool {
                        switch (opcode) {
                        case 0x60U:
                            return emit_binary([&](u32 d, u32 l, u32 r) {
                                emitter.add_w(d, l, r);
                            });
                        case 0x64U:
                            return emit_binary([&](u32 d, u32 l, u32 r) {
                                emitter.sub_w(d, l, r);
                            });
                        case 0x68U:
                            return emit_binary([&](u32 d, u32 l, u32 r) {
                                emitter.multiply_w(d, l, r);
                            });
                        case 0x78U:
                            return emit_binary([&](u32 d, u32 l, u32 r) {
                                emitter.shift_left_w(d, l, r);
                            });
                        case 0x7AU:
                            return emit_binary([&](u32 d, u32 l, u32 r) {
                                emitter.shift_right_arithmetic_w(d, l, r);
                            });
                        case 0x7CU:
                            return emit_binary([&](u32 d, u32 l, u32 r) {
                                emitter.shift_right_logical_w(d, l, r);
                            });
                        case 0x7EU:
                            return emit_binary([&](u32 d, u32 l, u32 r) {
                                emitter.and_w(d, l, r);
                            });
                        case 0x80U:
                            return emit_binary([&](u32 d, u32 l, u32 r) {
                                emitter.or_w(d, l, r);
                            });
                        case 0x82U:
                            return emit_binary([&](u32 d, u32 l, u32 r) {
                                emitter.xor_w(d, l, r);
                            });
                        default:
                            return false;
                        }
                    };
                    const auto emit_inline_long_binary = [&](u8 opcode) -> bool {
                        switch (opcode) {
                        case 0x61U:
                            return emit_long_binary([&](u32 d, u32 l, u32 r) {
                                emitter.add_x(d, l, r);
                            });
                        case 0x65U:
                            return emit_long_binary([&](u32 d, u32 l, u32 r) {
                                emitter.sub_x(d, l, r);
                            });
                        case 0x69U:
                            return emit_long_binary([&](u32 d, u32 l, u32 r) {
                                emitter.multiply_x(d, l, r);
                            });
                        case 0x7FU:
                            return emit_long_binary([&](u32 d, u32 l, u32 r) {
                                emitter.and_x(d, l, r);
                            });
                        case 0x81U:
                            return emit_long_binary([&](u32 d, u32 l, u32 r) {
                                emitter.or_x(d, l, r);
                            });
                        case 0x83U:
                            return emit_long_binary([&](u32 d, u32 l, u32 r) {
                                emitter.xor_x(d, l, r);
                            });
                        default:
                            return false;
                        }
                    };
                    const auto emit_inline_float_binary = [&](u8 opcode) -> bool {
                        switch (opcode) {
                        case 0x62U:
                            return emit_float_binary([&](u32 d, u32 l, u32 r) {
                                emitter.add_float(d, l, r);
                            });
                        case 0x66U:
                            return emit_float_binary([&](u32 d, u32 l, u32 r) {
                                emitter.sub_float(d, l, r);
                            });
                        case 0x6AU:
                            return emit_float_binary([&](u32 d, u32 l, u32 r) {
                                emitter.multiply_float(d, l, r);
                            });
                        case 0x6EU:
                            return emit_float_binary([&](u32 d, u32 l, u32 r) {
                                emitter.divide_float(d, l, r);
                            });
                        default:
                            return false;
                        }
                    };
                    const auto emit_inline_double_binary = [&](u8 opcode) -> bool {
                        switch (opcode) {
                        case 0x63U:
                            return emit_double_binary([&](u32 d, u32 l, u32 r) {
                                emitter.add_double(d, l, r);
                            });
                        case 0x67U:
                            return emit_double_binary([&](u32 d, u32 l, u32 r) {
                                emitter.sub_double(d, l, r);
                            });
                        case 0x6BU:
                            return emit_double_binary([&](u32 d, u32 l, u32 r) {
                                emitter.multiply_double(d, l, r);
                            });
                        case 0x6FU:
                            return emit_double_binary([&](u32 d, u32 l, u32 r) {
                                emitter.divide_double(d, l, r);
                            });
                        default:
                            return false;
                        }
                    };
                    const auto emit_inline_right_constant = [&](u8 opcode,
                                                                i32 constant)
                        -> bool {
                        if (!pop_register(kScratchLeft)) return false;
                        emitter.move_imm32(kScratchRight,
                                           static_cast<u32>(constant));
                        switch (opcode) {
                        case 0x60U:
                            emitter.add_w(kScratchLeft,
                                          kScratchLeft,
                                          kScratchRight);
                            break;
                        case 0x64U:
                            emitter.sub_w(kScratchLeft,
                                          kScratchLeft,
                                          kScratchRight);
                            break;
                        case 0x68U:
                            emitter.multiply_w(kScratchLeft,
                                               kScratchLeft,
                                               kScratchRight);
                            break;
                        case 0x78U:
                            emitter.shift_left_w(kScratchLeft,
                                                 kScratchLeft,
                                                 kScratchRight);
                            break;
                        case 0x7AU:
                            emitter.shift_right_arithmetic_w(
                                kScratchLeft, kScratchLeft, kScratchRight);
                            break;
                        case 0x7CU:
                            emitter.shift_right_logical_w(
                                kScratchLeft, kScratchLeft, kScratchRight);
                            break;
                        case 0x7EU:
                            emitter.and_w(kScratchLeft,
                                          kScratchLeft,
                                          kScratchRight);
                            break;
                        case 0x80U:
                            emitter.or_w(kScratchLeft,
                                         kScratchLeft,
                                         kScratchRight);
                            break;
                        case 0x82U:
                            emitter.xor_w(kScratchLeft,
                                          kScratchLeft,
                                          kScratchRight);
                            break;
                        default:
                            return false;
                        }
                        return push_register(kScratchLeft);
                    };

                    bool emitted = true;
                    switch (recipe.kind) {
                    case InlineRecipeKind::identity_int:
                        if (depth < 1U) return {};
                        break;
                    case InlineRecipeKind::binary_int:
                        emitted = emit_inline_binary(recipe.opcode);
                        break;
                    case InlineRecipeKind::unary_int:
                        if (!pop_register(kScratchLeft)) return {};
                        if (recipe.opcode == 0x74U) {
                            emitter.negate_w(kScratchLeft, kScratchLeft);
                        } else if (recipe.opcode == 0x91U) {
                            emitter.sign_extend_byte_w(kScratchLeft, kScratchLeft);
                        } else if (recipe.opcode == 0x92U) {
                            emitter.zero_extend_half_w(kScratchLeft, kScratchLeft);
                        } else if (recipe.opcode == 0x93U) {
                            emitter.sign_extend_half_w(kScratchLeft, kScratchLeft);
                        } else {
                            emitted = false;
                        }
                        if (emitted) emitted = push_register(kScratchLeft);
                        break;
                    case InlineRecipeKind::int_right_constant:
                        emitted = emit_inline_right_constant(
                            recipe.opcode, recipe.constant);
                        break;
                    case InlineRecipeKind::rect_contains_int: {
                        if (depth < 6U ||
                            !pop_register(kScratchLeft) ||     // height
                            !pop_register(kScratchRight) ||    // width
                            !pop_register(kScratchThird) ||    // top
                            !pop_register(kScratchFourth) ||   // left
                            !pop_register(kScratchMetadata)) { // y
                            return {};
                        }
                        std::array<std::pair<usize, Arm64Condition>, 4U>
                            false_branches {};
                        emitter.compare_w(kScratchMetadata, kScratchThird);
                        false_branches[0U] = {
                            emitter.emit_conditional_branch_placeholder(
                                Arm64Condition::less_than),
                            Arm64Condition::less_than,
                        };
                        // Java int addition wraps naturally in a W register.
                        emitter.add_w(kScratchThird,
                                      kScratchThird,
                                      kScratchLeft);
                        emitter.compare_w(kScratchMetadata, kScratchThird);
                        false_branches[1U] = {
                            emitter.emit_conditional_branch_placeholder(
                                Arm64Condition::greater_equal),
                            Arm64Condition::greater_equal,
                        };

                        // height is dead now, so reuse kScratchLeft for x.
                        if (!pop_register(kScratchLeft)) return {};
                        emitter.compare_w(kScratchLeft, kScratchFourth);
                        false_branches[2U] = {
                            emitter.emit_conditional_branch_placeholder(
                                Arm64Condition::less_than),
                            Arm64Condition::less_than,
                        };
                        emitter.add_w(kScratchFourth,
                                      kScratchFourth,
                                      kScratchRight);
                        emitter.compare_w(kScratchLeft, kScratchFourth);
                        false_branches[3U] = {
                            emitter.emit_conditional_branch_placeholder(
                                Arm64Condition::greater_equal),
                            Arm64Condition::greater_equal,
                        };

                        emitter.move_imm32(kScratchMetadata, 1U);
                        const usize done_branch =
                            emitter.emit_branch_placeholder();
                        const usize false_position = emitter.position();
                        emitter.move_imm32(kScratchMetadata, 0U);
                        const usize done_position = emitter.position();
                        for (const auto& [patch, condition] : false_branches) {
                            if (!emitter.patch_conditional_branch(
                                    patch, false_position, condition)) {
                                return {};
                            }
                        }
                        if (!emitter.patch_branch(done_branch, done_position)) {
                            return {};
                        }
                        emitted = push_register(kScratchMetadata);
                        break;
                    }
                    case InlineRecipeKind::static_byte_cursor_read: {
                        // Inline the exact sequence
                        //   getstatic bytes; getstatic cursor; dup; iconst_1;
                        //   iadd; putstatic cursor; baload
                        // while keeping all field/array checks in the existing
                        // runtime helpers. This removes only the Java method
                        // call boundary; exceptions and byte sign-extension are
                        // still handled by the normal JIT runtime operations.
                        requires_runtime_dispatch = true;
                        emit_runtime_call(JitRuntimeOperation::get_static,
                                          recipe.first_operand,
                                          31U, 31U, 31U,
                                          static_cast<u32>(instruction.pc));
                        if (!push_reference_register(kScratchLeft)) return {};

                        emit_runtime_call(JitRuntimeOperation::get_static,
                                          recipe.second_operand,
                                          31U, 31U, 31U,
                                          static_cast<u32>(instruction.pc));
                        if (!push_register(kScratchLeft) ||
                            !pop_register(kScratchRight) ||
                            !push_register(kScratchRight) ||
                            !push_register(kScratchRight) ||
                            !pop_register(kScratchLeft)) {
                            return {};
                        }
                        emitter.move_imm32(kScratchThird, 1U);
                        emitter.add_w(kScratchLeft,
                                      kScratchLeft,
                                      kScratchThird);
                        emit_runtime_call(JitRuntimeOperation::put_static,
                                          recipe.second_operand,
                                          kScratchLeft,
                                          31U, 31U,
                                          static_cast<u32>(instruction.pc));

                        if (!pop_register(kScratchRight) ||
                            !pop_reference_register(kScratchLeft)) {
                            return {};
                        }
                        emit_runtime_call(JitRuntimeOperation::array_load,
                                          0x33U,
                                          kScratchLeft,
                                          kScratchRight,
                                          31U,
                                          static_cast<u32>(instruction.pc));
                        emitted = push_register(kScratchLeft);
                        break;
                    }
                    case InlineRecipeKind::identity_long:
                        if (depth < 2U) return {};
                        break;
                    case InlineRecipeKind::binary_long:
                        emitted = emit_inline_long_binary(recipe.opcode);
                        break;
                    case InlineRecipeKind::unary_long:
                        if (!pop_long_register(kScratchLeft)) return {};
                        if (recipe.opcode != 0x75U) {
                            emitted = false;
                        } else {
                            emitter.negate_x(kScratchLeft, kScratchLeft);
                        }
                        if (emitted) emitted = push_long_register(kScratchLeft);
                        break;
                    case InlineRecipeKind::identity_float:
                        if (depth < 1U) return {};
                        break;
                    case InlineRecipeKind::binary_float:
                        emitted = emit_inline_float_binary(recipe.opcode);
                        break;
                    case InlineRecipeKind::unary_float:
                        if (!pop_register(kScratchLeft)) return {};
                        if (recipe.opcode != 0x76U) {
                            emitted = false;
                        } else {
                            emitter.move_w_to_float(kScratchLeft, kScratchLeft);
                            emitter.negate_float(kScratchLeft, kScratchLeft);
                            emitter.move_float_to_w(kScratchLeft, kScratchLeft);
                        }
                        if (emitted) emitted = push_register(kScratchLeft);
                        break;
                    case InlineRecipeKind::identity_double:
                        if (depth < 2U) return {};
                        break;
                    case InlineRecipeKind::binary_double:
                        emitted = emit_inline_double_binary(recipe.opcode);
                        break;
                    case InlineRecipeKind::unary_double:
                        if (!pop_long_register(kScratchLeft)) return {};
                        if (recipe.opcode != 0x77U) {
                            emitted = false;
                        } else {
                            emitter.move_x_to_double(kScratchLeft, kScratchLeft);
                            emitter.negate_double(kScratchLeft, kScratchLeft);
                            emitter.move_double_to_x(kScratchLeft, kScratchLeft);
                        }
                        if (emitted) emitted = push_long_register(kScratchLeft);
                        break;
                    }
                    if (emitted && recipe.has_receiver) {
                        // Collapse the proven non-null local-0 receiver beneath
                        // the result while respecting category-2 JVM values.
                        if (recipe.result_slots == 2U) {
                            if (!pop_long_register(kScratchLeft) || depth == 0U) {
                                return {};
                            }
                            --depth;
                            emitted = push_long_register(kScratchLeft);
                        } else {
                            if (!pop_register(kScratchLeft) || depth == 0U) return {};
                            --depth;
                            emitted = push_register(kScratchLeft);
                        }
                    }
                    if (!emitted) return {};
                    break;
                }
            }
            const u32 consumed_slots = instruction.argument_slots +
                (instruction.has_receiver ? 1U : 0U);
            if (depth < consumed_slots) return {};
            depth -= consumed_slots;
            JitRuntimeOperation operation = JitRuntimeOperation::invoke_virtual;
            if (instruction.opcode == 0xB7U) {
                operation = JitRuntimeOperation::invoke_special;
            } else if (instruction.opcode == 0xB8U) {
                operation = JitRuntimeOperation::invoke_static;
            } else if (instruction.opcode == 0xB9U) {
                operation = JitRuntimeOperation::invoke_interface;
            } else if (instruction.opcode == 0xBAU) {
                operation = JitRuntimeOperation::invoke_dynamic;
            }
            requires_runtime_dispatch = true;
            emit_runtime_call(operation,
                              instruction.local_index,
                              instruction.opcode == 0xBAU
                                  ? 31U
                                  : kBudgetRemaining,
                              31U,
                              31U,
                              static_cast<u32>(instruction.pc));
            if (instruction.value_kind == JavaTypeKind::void_type) {
                break;
            }
            if (instruction.value_kind == JavaTypeKind::long_integer ||
                instruction.value_kind == JavaTypeKind::float64) {
                if (!push_long_register(kScratchLeft)) return {};
            } else if (instruction.value_kind == JavaTypeKind::reference ||
                       instruction.value_kind == JavaTypeKind::array) {
                if (!push_reference_register(kScratchLeft)) return {};
            } else if (!push_register(kScratchLeft)) {
                return {};
            }
            break;
        }
        case 0xBBU:
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::new_object,
                              instruction.local_index,
                              31U,
                              31U,
                              31U,
                              static_cast<u32>(instruction.pc));
            if (!push_reference_register(kScratchLeft)) return {};
            break;
        case 0xBCU:
            if (!pop_register(kScratchLeft)) return {};
            if (optimization.scalar_array_new_local.has_value()) {
                emitter.move_imm32(kScratchLeft, 0U);
                if (!push_reference_register(kScratchLeft)) return {};
                break;
            }
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::new_primitive_array,
                              instruction.local_index,
                              kScratchLeft,
                              31U,
                              31U,
                              static_cast<u32>(instruction.pc));
            if (!push_reference_register(kScratchLeft)) return {};
            break;
        case 0xBDU:
            if (!pop_register(kScratchLeft)) return {};
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::new_reference_array,
                              instruction.local_index,
                              kScratchLeft,
                              31U,
                              31U,
                              static_cast<u32>(instruction.pc));
            if (!push_reference_register(kScratchLeft)) return {};
            break;
        case 0xC5U:
            if (instruction.argument_slots == 0U ||
                depth < instruction.argument_slots) {
                return {};
            }
            depth -= instruction.argument_slots;
            emitter.move_imm32(kScratchLeft, instruction.argument_slots);
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::new_multi_array,
                              instruction.local_index,
                              kScratchLeft,
                              31U,
                              31U,
                              static_cast<u32>(instruction.pc));
            if (!push_reference_register(kScratchLeft)) return {};
            break;
        case 0xC0U:
        case 0xC1U:
            if (!pop_reference_register(kScratchLeft)) return {};
            requires_runtime_dispatch = true;
            emit_runtime_call(
                instruction.opcode == 0xC0U
                    ? JitRuntimeOperation::check_cast
                    : JitRuntimeOperation::instance_of,
                instruction.local_index,
                kScratchLeft,
                31U,
                31U,
                static_cast<u32>(instruction.pc));
            if (instruction.opcode == 0xC0U) {
                if (!push_reference_register(kScratchLeft)) return {};
            } else if (!push_register(kScratchLeft)) {
                return {};
            }
            break;
        case 0xBEU:
            if (!pop_reference_register(kScratchLeft)) return {};
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::array_length,
                              0U,
                              kScratchLeft,
                              31U,
                              31U,
                              static_cast<u32>(instruction.pc));
            if (!push_register(kScratchLeft)) return {};
            break;
        case 0xC2U:
        case 0xC3U:
            if (!pop_reference_register(kScratchLeft)) return {};
            requires_runtime_dispatch = true;
            emit_runtime_call(
                instruction.opcode == 0xC2U
                    ? JitRuntimeOperation::monitor_enter
                    : JitRuntimeOperation::monitor_exit,
                0U,
                kScratchLeft,
                31U,
                31U,
                static_cast<u32>(instruction.pc));
            break;
        case 0xBFU:
            if (!pop_reference_register(kScratchLeft)) return {};
            requires_runtime_dispatch = true;
            emit_runtime_call(JitRuntimeOperation::throw_object,
                              0U,
                              kScratchLeft,
                              31U,
                              31U,
                              static_cast<u32>(instruction.pc));
            terminal_deopt_patches.push_back(
                emitter.emit_branch_placeholder());
            break;
        case 0x36U:
            if (!pop_register(kScratchLeft)) return {};
            if (!optimization.dead_local_write) {
                if (const auto cached = cached_local_register(
                        instruction.local_index,
                        CachedLocalKind::int32);
                    cached.has_value()) {
                    emitter.move_w(*cached, kScratchLeft);
                    ++register_cached_stores_elided;
                } else {
                    emitter.store_w(kScratchLeft,
                                    kStackPointer,
                                    local_offset(instruction.local_index));
                }
            }
            break;
        case 0x38U:
            if (!pop_register(kScratchLeft)) return {};
            if (const auto cached = cached_local_register(
                    instruction.local_index,
                    CachedLocalKind::float32);
                cached.has_value()) {
                emitter.move_w(*cached, kScratchLeft);
                ++register_cached_stores_elided;
            } else {
                emitter.store_w(kScratchLeft,
                                kStackPointer,
                                local_offset(instruction.local_index));
            }
            break;
        case 0x37U:
            if (!pop_long_register(kScratchLeft)) return {};
            if (const auto cached = cached_local_register(
                    instruction.local_index,
                    CachedLocalKind::int64);
                cached.has_value()) {
                emitter.move_x(*cached, kScratchLeft);
                ++register_cached_stores_elided;
            } else {
                emitter.store_x(kScratchLeft,
                                kStackPointer,
                                local_offset(instruction.local_index));
            }
            break;
        case 0x39U:
            if (!pop_long_register(kScratchLeft)) return {};
            if (const auto cached = cached_local_register(
                    instruction.local_index,
                    CachedLocalKind::float64);
                cached.has_value()) {
                emitter.move_x(*cached, kScratchLeft);
                ++register_cached_stores_elided;
            } else {
                emitter.store_x(kScratchLeft,
                                kStackPointer,
                                local_offset(instruction.local_index));
            }
            break;
        case 0x3BU:
        case 0x3CU:
        case 0x3DU:
        case 0x3EU: {
            const u32 index = static_cast<u32>(instruction.opcode - 0x3BU);
            if (!pop_register(kScratchLeft)) return {};
            if (!optimization.dead_local_write) {
                if (const auto cached = cached_local_register(
                        index, CachedLocalKind::int32);
                    cached.has_value()) {
                    emitter.move_w(*cached, kScratchLeft);
                    ++register_cached_stores_elided;
                } else {
                    emitter.store_w(kScratchLeft,
                                    kStackPointer,
                                    local_offset(index));
                }
            }
            break;
        }
        case 0x43U:
        case 0x44U:
        case 0x45U:
        case 0x46U: {
            const u32 index = static_cast<u32>(instruction.opcode - 0x43U);
            if (!pop_register(kScratchLeft)) return {};
            if (const auto cached = cached_local_register(
                    index, CachedLocalKind::float32);
                cached.has_value()) {
                emitter.move_w(*cached, kScratchLeft);
                ++register_cached_stores_elided;
            } else {
                emitter.store_w(kScratchLeft, kStackPointer, local_offset(index));
            }
            break;
        }
        case 0x3FU:
        case 0x40U:
        case 0x41U:
        case 0x42U: {
            const u32 index = static_cast<u32>(instruction.opcode - 0x3FU);
            if (!pop_long_register(kScratchLeft)) return {};
            if (const auto cached = cached_local_register(
                    index, CachedLocalKind::int64);
                cached.has_value()) {
                emitter.move_x(*cached, kScratchLeft);
                ++register_cached_stores_elided;
            } else {
                emitter.store_x(kScratchLeft, kStackPointer, local_offset(index));
            }
            break;
        }
        case 0x47U:
        case 0x48U:
        case 0x49U:
        case 0x4AU: {
            const u32 index = static_cast<u32>(instruction.opcode - 0x47U);
            if (!pop_long_register(kScratchLeft)) return {};
            if (const auto cached = cached_local_register(
                    index, CachedLocalKind::float64);
                cached.has_value()) {
                emitter.move_x(*cached, kScratchLeft);
                ++register_cached_stores_elided;
            } else {
                emitter.store_x(kScratchLeft, kStackPointer, local_offset(index));
            }
            break;
        }
        case 0x3AU:
            if (!pop_reference_register(kScratchLeft)) return {};
            if (const auto cached = cached_local_register(
                    instruction.local_index,
                    CachedLocalKind::reference);
                cached.has_value()) {
                emitter.move_x(*cached, kScratchLeft);
                ++register_cached_stores_elided;
            } else {
                emitter.store_x(kScratchLeft,
                                kStackPointer,
                                local_offset(instruction.local_index));
            }
            break;
        case 0x4BU:
        case 0x4CU:
        case 0x4DU:
        case 0x4EU: {
            const u32 index = static_cast<u32>(instruction.opcode - 0x4BU);
            if (!pop_reference_register(kScratchLeft)) return {};
            if (const auto cached = cached_local_register(
                    index, CachedLocalKind::reference);
                cached.has_value()) {
                emitter.move_x(*cached, kScratchLeft);
                ++register_cached_stores_elided;
            } else {
                emitter.store_x(kScratchLeft, kStackPointer, local_offset(index));
            }
            break;
        }
        case 0x57U:
            if (depth == 0U) return {};
            --depth;
            break;
        case 0x58U:
            if (depth < 2U) return {};
            depth -= 2U;
            break;
        case 0x59U:
            if (depth == 0U || depth >= stack_slots) return {};
            emitter.load_x(kScratchLeft,
                           kStackPointer,
                           stack_offset(depth - 1U));
            if (!push_reference_register(kScratchLeft)) return {};
            break;
        case 0x5AU:
            if (depth < 2U || depth >= stack_slots) return {};
            emitter.load_x(kScratchLeft, kStackPointer, stack_offset(depth - 1U));
            emitter.load_x(kScratchRight, kStackPointer, stack_offset(depth - 2U));
            emitter.store_x(kScratchLeft, kStackPointer, stack_offset(depth - 2U));
            emitter.store_x(kScratchRight, kStackPointer, stack_offset(depth - 1U));
            emitter.store_x(kScratchLeft, kStackPointer, stack_offset(depth));
            ++depth;
            break;
        case 0x5BU:
            if (depth < 3U || depth >= stack_slots) return {};
            emitter.load_x(kScratchLeft, kStackPointer, stack_offset(depth - 1U));
            emitter.load_x(kScratchRight, kStackPointer, stack_offset(depth - 2U));
            emitter.load_x(kScratchThird, kStackPointer, stack_offset(depth - 3U));
            emitter.store_x(kScratchLeft, kStackPointer, stack_offset(depth - 3U));
            emitter.store_x(kScratchThird, kStackPointer, stack_offset(depth - 2U));
            emitter.store_x(kScratchRight, kStackPointer, stack_offset(depth - 1U));
            emitter.store_x(kScratchLeft, kStackPointer, stack_offset(depth));
            ++depth;
            break;
        case 0x5CU:
            if (depth < 2U || depth + 2U > stack_slots) return {};
            emitter.load_x(kScratchLeft, kStackPointer, stack_offset(depth - 2U));
            emitter.load_x(kScratchRight, kStackPointer, stack_offset(depth - 1U));
            emitter.store_x(kScratchLeft, kStackPointer, stack_offset(depth));
            emitter.store_x(kScratchRight, kStackPointer, stack_offset(depth + 1U));
            depth += 2U;
            break;
        case 0x5DU:
            if (depth < 3U || depth + 2U > stack_slots) return {};
            emitter.load_x(kScratchLeft, kStackPointer, stack_offset(depth - 1U));
            emitter.load_x(kScratchRight, kStackPointer, stack_offset(depth - 2U));
            emitter.load_x(kScratchThird, kStackPointer, stack_offset(depth - 3U));
            emitter.store_x(kScratchRight, kStackPointer, stack_offset(depth - 3U));
            emitter.store_x(kScratchLeft, kStackPointer, stack_offset(depth - 2U));
            emitter.store_x(kScratchThird, kStackPointer, stack_offset(depth - 1U));
            emitter.store_x(kScratchRight, kStackPointer, stack_offset(depth));
            emitter.store_x(kScratchLeft, kStackPointer, stack_offset(depth + 1U));
            depth += 2U;
            break;
        case 0x5EU:
            if (depth < 4U || depth + 2U > stack_slots) return {};
            emitter.load_x(kScratchLeft, kStackPointer, stack_offset(depth - 1U));
            emitter.load_x(kScratchRight, kStackPointer, stack_offset(depth - 2U));
            emitter.load_x(kScratchThird, kStackPointer, stack_offset(depth - 3U));
            emitter.load_x(kScratchFourth, kStackPointer, stack_offset(depth - 4U));
            emitter.store_x(kScratchRight, kStackPointer, stack_offset(depth - 4U));
            emitter.store_x(kScratchLeft, kStackPointer, stack_offset(depth - 3U));
            emitter.store_x(kScratchFourth, kStackPointer, stack_offset(depth - 2U));
            emitter.store_x(kScratchThird, kStackPointer, stack_offset(depth - 1U));
            emitter.store_x(kScratchRight, kStackPointer, stack_offset(depth));
            emitter.store_x(kScratchLeft, kStackPointer, stack_offset(depth + 1U));
            depth += 2U;
            break;
        case 0x5FU:
            if (depth < 2U) return {};
            emitter.load_x(kScratchLeft, kStackPointer, stack_offset(depth - 2U));
            emitter.load_x(kScratchRight, kStackPointer, stack_offset(depth - 1U));
            emitter.store_x(kScratchRight, kStackPointer, stack_offset(depth - 2U));
            emitter.store_x(kScratchLeft, kStackPointer, stack_offset(depth - 1U));
            break;
        case 0x60U:
            if (optimization.folded_result.has_value()) {
                if (!replace_stack_with_constant(
                        2U, *optimization.folded_result)) return {};
            } else if (optimization.right_constant.has_value()) {
                if (!emit_right_constant_binary(
                        instruction.opcode,
                        *optimization.right_constant)) return {};
            } else if (!emit_binary([&](u32 d, u32 l, u32 r) {
                           emitter.add_w(d, l, r);
                       })) {
                return {};
            }
            break;
        case 0x64U:
            if (optimization.folded_result.has_value()) {
                if (!replace_stack_with_constant(
                        2U, *optimization.folded_result)) return {};
            } else if (optimization.right_constant.has_value()) {
                if (!emit_right_constant_binary(
                        instruction.opcode,
                        *optimization.right_constant)) return {};
            } else if (!emit_binary([&](u32 d, u32 l, u32 r) {
                           emitter.sub_w(d, l, r);
                       })) {
                return {};
            }
            break;
        case 0x68U:
            if (optimization.folded_result.has_value()) {
                if (!replace_stack_with_constant(
                        2U, *optimization.folded_result)) return {};
            } else if (optimization.right_constant.has_value()) {
                if (!emit_right_constant_binary(
                        instruction.opcode,
                        *optimization.right_constant)) return {};
            } else if (!emit_binary([&](u32 d, u32 l, u32 r) {
                           emitter.multiply_w(d, l, r);
                       })) {
                return {};
            }
            break;
        case 0x6CU:
        case 0x70U:
            if (optimization.folded_result.has_value()) {
                if (!replace_stack_with_constant(
                        2U, *optimization.folded_result)) return {};
                break;
            }
            if (optimization.right_constant.has_value() &&
                *optimization.right_constant != 0) {
                if (!emit_right_constant_binary(
                        instruction.opcode,
                        *optimization.right_constant)) return {};
                break;
            }
            if (!pop_register(kScratchRight) ||
                !pop_register(kScratchLeft)) {
                return {};
            }
            arithmetic_trap_sites.push_back(ArithmeticTrapSite {
                .zero_branch = emitter.emit_cbz_w_placeholder(kScratchRight),
                .bytecode_pc = static_cast<u32>(instruction.pc),
                .stack_depth = active_safepoint_depth,
                .wide = false,
            });
            emitter.divide_signed_w(
                kScratchThird, kScratchLeft, kScratchRight);
            if (instruction.opcode == 0x6CU) {
                emitter.move_w(kScratchLeft, kScratchThird);
            } else {
                emitter.multiply_subtract_w(kScratchLeft,
                                            kScratchThird,
                                            kScratchRight,
                                            kScratchLeft);
            }
            if (!push_register(kScratchLeft)) return {};
            break;
        case 0x74U:
            if (optimization.folded_result.has_value()) {
                if (!replace_stack_with_constant(
                        1U, *optimization.folded_result)) return {};
            } else {
                if (!pop_register(kScratchLeft)) return {};
                emitter.negate_w(kScratchLeft, kScratchLeft);
                if (!push_register(kScratchLeft)) return {};
            }
            break;
        case 0x78U:
            if (optimization.folded_result.has_value()) {
                if (!replace_stack_with_constant(
                        2U, *optimization.folded_result)) return {};
            } else if (optimization.right_constant.has_value()) {
                if (!emit_right_constant_binary(
                        instruction.opcode,
                        *optimization.right_constant)) return {};
            } else if (!emit_binary([&](u32 d, u32 l, u32 r) {
                           emitter.shift_left_w(d, l, r);
                       })) {
                return {};
            }
            break;
        case 0x7AU:
            if (optimization.folded_result.has_value()) {
                if (!replace_stack_with_constant(
                        2U, *optimization.folded_result)) return {};
            } else if (optimization.right_constant.has_value()) {
                if (!emit_right_constant_binary(
                        instruction.opcode,
                        *optimization.right_constant)) return {};
            } else if (!emit_binary([&](u32 d, u32 l, u32 r) {
                           emitter.shift_right_arithmetic_w(d, l, r);
                       })) {
                return {};
            }
            break;
        case 0x7CU:
            if (optimization.folded_result.has_value()) {
                if (!replace_stack_with_constant(
                        2U, *optimization.folded_result)) return {};
            } else if (optimization.right_constant.has_value()) {
                if (!emit_right_constant_binary(
                        instruction.opcode,
                        *optimization.right_constant)) return {};
            } else if (!emit_binary([&](u32 d, u32 l, u32 r) {
                           emitter.shift_right_logical_w(d, l, r);
                       })) {
                return {};
            }
            break;
        case 0x7EU:
            if (optimization.folded_result.has_value()) {
                if (!replace_stack_with_constant(
                        2U, *optimization.folded_result)) return {};
            } else if (optimization.right_constant.has_value()) {
                if (!emit_right_constant_binary(
                        instruction.opcode,
                        *optimization.right_constant)) return {};
            } else if (!emit_binary([&](u32 d, u32 l, u32 r) {
                           emitter.and_w(d, l, r);
                       })) {
                return {};
            }
            break;
        case 0x80U:
            if (optimization.folded_result.has_value()) {
                if (!replace_stack_with_constant(
                        2U, *optimization.folded_result)) return {};
            } else if (optimization.right_constant.has_value()) {
                if (!emit_right_constant_binary(
                        instruction.opcode,
                        *optimization.right_constant)) return {};
            } else if (!emit_binary([&](u32 d, u32 l, u32 r) {
                           emitter.or_w(d, l, r);
                       })) {
                return {};
            }
            break;
        case 0x82U:
            if (optimization.folded_result.has_value()) {
                if (!replace_stack_with_constant(
                        2U, *optimization.folded_result)) return {};
            } else if (optimization.right_constant.has_value()) {
                if (!emit_right_constant_binary(
                        instruction.opcode,
                        *optimization.right_constant)) return {};
            } else if (!emit_binary([&](u32 d, u32 l, u32 r) {
                           emitter.xor_w(d, l, r);
                       })) {
                return {};
            }
            break;
        case 0x62U:
            if (!emit_float_binary([&](u32 d, u32 l, u32 r) {
                    emitter.add_float(d, l, r);
                })) return {};
            break;
        case 0x66U:
            if (!emit_float_binary([&](u32 d, u32 l, u32 r) {
                    emitter.sub_float(d, l, r);
                })) return {};
            break;
        case 0x6AU:
            if (!emit_float_binary([&](u32 d, u32 l, u32 r) {
                    emitter.multiply_float(d, l, r);
                })) return {};
            break;
        case 0x6EU:
            if (!emit_float_binary([&](u32 d, u32 l, u32 r) {
                    emitter.divide_float(d, l, r);
                })) return {};
            break;
        case 0x72U:
            if (!emit_runtime_binary_scalar(
                    JitRuntimeOperation::float_remainder,
                    false,
                    false)) return {};
            break;
        case 0x63U:
            if (!emit_double_binary([&](u32 d, u32 l, u32 r) {
                    emitter.add_double(d, l, r);
                })) return {};
            break;
        case 0x67U:
            if (!emit_double_binary([&](u32 d, u32 l, u32 r) {
                    emitter.sub_double(d, l, r);
                })) return {};
            break;
        case 0x6BU:
            if (!emit_double_binary([&](u32 d, u32 l, u32 r) {
                    emitter.multiply_double(d, l, r);
                })) return {};
            break;
        case 0x6FU:
            if (!emit_double_binary([&](u32 d, u32 l, u32 r) {
                    emitter.divide_double(d, l, r);
                })) return {};
            break;
        case 0x73U:
            if (!emit_runtime_binary_scalar(
                    JitRuntimeOperation::double_remainder,
                    true,
                    true)) return {};
            break;
        case 0x76U:
            if (!pop_register(kScratchLeft)) return {};
            emitter.move_w_to_float(kScratchLeft, kScratchLeft);
            emitter.negate_float(kScratchLeft, kScratchLeft);
            emitter.move_float_to_w(kScratchLeft, kScratchLeft);
            if (!push_register(kScratchLeft)) return {};
            break;
        case 0x77U:
            if (!pop_long_register(kScratchLeft)) return {};
            emitter.move_x_to_double(kScratchLeft, kScratchLeft);
            emitter.negate_double(kScratchLeft, kScratchLeft);
            emitter.move_double_to_x(kScratchLeft, kScratchLeft);
            if (!push_long_register(kScratchLeft)) return {};
            break;
        case 0x61U:
            if (!emit_long_binary([&](u32 d, u32 l, u32 r) {
                    emitter.add_x(d, l, r);
                })) return {};
            break;
        case 0x65U:
            if (!emit_long_binary([&](u32 d, u32 l, u32 r) {
                    emitter.sub_x(d, l, r);
                })) return {};
            break;
        case 0x69U:
            if (!emit_long_binary([&](u32 d, u32 l, u32 r) {
                    emitter.multiply_x(d, l, r);
                })) return {};
            break;
        case 0x6DU:
        case 0x71U:
            if (!pop_long_register(kScratchRight) ||
                !pop_long_register(kScratchLeft)) {
                return {};
            }
            arithmetic_trap_sites.push_back(ArithmeticTrapSite {
                .zero_branch = emitter.emit_cbz_x_placeholder(kScratchRight),
                .bytecode_pc = static_cast<u32>(instruction.pc),
                .stack_depth = active_safepoint_depth,
                .wide = true,
            });
            emitter.divide_signed_x(
                kScratchThird, kScratchLeft, kScratchRight);
            if (instruction.opcode == 0x6DU) {
                emitter.move_x(kScratchLeft, kScratchThird);
            } else {
                emitter.multiply_subtract_x(kScratchLeft,
                                            kScratchThird,
                                            kScratchRight,
                                            kScratchLeft);
            }
            if (!push_long_register(kScratchLeft)) return {};
            break;
        case 0x75U:
            if (!pop_long_register(kScratchLeft)) return {};
            emitter.negate_x(kScratchLeft, kScratchLeft);
            if (!push_long_register(kScratchLeft)) return {};
            break;
        case 0x79U:
        case 0x7BU:
        case 0x7DU:
            if (!pop_register(kScratchRight) ||
                !pop_long_register(kScratchLeft)) {
                return {};
            }
            if (instruction.opcode == 0x79U) {
                emitter.shift_left_x(
                    kScratchLeft, kScratchLeft, kScratchRight);
            } else if (instruction.opcode == 0x7BU) {
                emitter.shift_right_arithmetic_x(
                    kScratchLeft, kScratchLeft, kScratchRight);
            } else {
                emitter.shift_right_logical_x(
                    kScratchLeft, kScratchLeft, kScratchRight);
            }
            if (!push_long_register(kScratchLeft)) return {};
            break;
        case 0x7FU:
            if (!emit_long_binary([&](u32 d, u32 l, u32 r) {
                    emitter.and_x(d, l, r);
                })) return {};
            break;
        case 0x81U:
            if (!emit_long_binary([&](u32 d, u32 l, u32 r) {
                    emitter.or_x(d, l, r);
                })) return {};
            break;
        case 0x83U:
            if (!emit_long_binary([&](u32 d, u32 l, u32 r) {
                    emitter.xor_x(d, l, r);
                })) return {};
            break;
        case 0x85U:
            if (!pop_register(kScratchLeft)) return {};
            emitter.sign_extend_w_to_x(kScratchLeft, kScratchLeft);
            if (!push_long_register(kScratchLeft)) return {};
            break;
        case 0x86U:
            if (!emit_runtime_unary_scalar(
                    JitRuntimeOperation::int_to_float,
                    false,
                    false)) return {};
            break;
        case 0x87U:
            if (!emit_runtime_unary_scalar(
                    JitRuntimeOperation::int_to_double,
                    false,
                    true)) return {};
            break;
        case 0x88U:
            if (!pop_long_register(kScratchLeft)) return {};
            if (!push_register(kScratchLeft)) return {};
            break;
        case 0x89U:
            if (!emit_runtime_unary_scalar(
                    JitRuntimeOperation::long_to_float,
                    true,
                    false)) return {};
            break;
        case 0x8AU:
            if (!emit_runtime_unary_scalar(
                    JitRuntimeOperation::long_to_double,
                    true,
                    true)) return {};
            break;
        case 0x8BU:
            if (!emit_runtime_unary_scalar(
                    JitRuntimeOperation::float_to_int,
                    false,
                    false)) return {};
            break;
        case 0x8CU:
            if (!emit_runtime_unary_scalar(
                    JitRuntimeOperation::float_to_long,
                    false,
                    true)) return {};
            break;
        case 0x8DU:
            if (!emit_runtime_unary_scalar(
                    JitRuntimeOperation::float_to_double,
                    false,
                    true)) return {};
            break;
        case 0x8EU:
            if (!emit_runtime_unary_scalar(
                    JitRuntimeOperation::double_to_int,
                    true,
                    false)) return {};
            break;
        case 0x8FU:
            if (!emit_runtime_unary_scalar(
                    JitRuntimeOperation::double_to_long,
                    true,
                    true)) return {};
            break;
        case 0x90U:
            if (!emit_runtime_unary_scalar(
                    JitRuntimeOperation::double_to_float,
                    true,
                    false)) return {};
            break;
        case 0x94U:
            if (!pop_long_register(kScratchRight) ||
                !pop_long_register(kScratchLeft)) {
                return {};
            }
            emitter.compare_x(kScratchLeft, kScratchRight);
            emitter.set_condition_w(kScratchLeft, Arm64Condition::greater_than);
            emitter.set_condition_w(kScratchRight, Arm64Condition::less_than);
            emitter.sub_w(kScratchLeft, kScratchLeft, kScratchRight);
            if (!push_register(kScratchLeft)) return {};
            break;
        case 0x95U:
            if (!emit_runtime_binary_scalar(
                    JitRuntimeOperation::float_compare_less,
                    false,
                    false)) return {};
            break;
        case 0x96U:
            if (!emit_runtime_binary_scalar(
                    JitRuntimeOperation::float_compare_greater,
                    false,
                    false)) return {};
            break;
        case 0x97U:
            if (!emit_runtime_binary_scalar(
                    JitRuntimeOperation::double_compare_less,
                    true,
                    false)) return {};
            break;
        case 0x98U:
            if (!emit_runtime_binary_scalar(
                    JitRuntimeOperation::double_compare_greater,
                    true,
                    false)) return {};
            break;
        case 0x84U:
            if (optimization.dead_local_write) break;
            if (const auto cached = cached_local_register(
                    instruction.local_index,
                    CachedLocalKind::int32);
                cached.has_value()) {
                emitter.move_w(kScratchLeft, *cached);
                emitter.move_imm32(
                    kScratchRight, static_cast<u32>(instruction.immediate));
                emitter.add_w(*cached, kScratchLeft, kScratchRight);
                ++register_cached_stores_elided;
            } else {
                emitter.load_w(kScratchLeft,
                               kStackPointer,
                               local_offset(instruction.local_index));
                emitter.move_imm32(
                    kScratchRight, static_cast<u32>(instruction.immediate));
                emitter.add_w(kScratchLeft, kScratchLeft, kScratchRight);
                emitter.store_w(kScratchLeft,
                                kStackPointer,
                                local_offset(instruction.local_index));
            }
            break;
        case 0x91U:
        case 0x92U:
        case 0x93U:
            if (optimization.folded_result.has_value()) {
                if (!replace_stack_with_constant(
                        1U, *optimization.folded_result)) return {};
            } else {
                if (!pop_register(kScratchLeft)) return {};
                if (instruction.opcode == 0x91U) {
                    emitter.sign_extend_byte_w(kScratchLeft, kScratchLeft);
                } else if (instruction.opcode == 0x92U) {
                    emitter.zero_extend_half_w(kScratchLeft, kScratchLeft);
                } else {
                    emitter.sign_extend_half_w(kScratchLeft, kScratchLeft);
                }
                if (!push_register(kScratchLeft)) return {};
            }
            break;
        case 0x99U:
        case 0x9AU:
        case 0x9BU:
        case 0x9CU:
        case 0x9DU:
        case 0x9EU: {
            if (!instruction.branch_target.has_value()) return {};
            if (optimization.branch_taken.has_value()) {
                if (depth == 0U) return {};
                --depth;
                if (*optimization.branch_taken) {
                    branch_patches.push_back(BranchPatch {
                        .kind = BranchPatch::Kind::unconditional,
                        .native_index = emitter.emit_branch_placeholder(),
                        .target_pc = *instruction.branch_target,
                        .condition = Arm64Condition::equal,
                    });
                }
                break;
            }
            if (!pop_register(kScratchLeft)) return {};
            const auto condition = branch_condition(instruction.opcode);
            if (!condition.has_value()) return {};
            emitter.compare_zero_w(kScratchLeft);
            branch_patches.push_back(BranchPatch {
                .kind = BranchPatch::Kind::conditional,
                .native_index =
                    emitter.emit_conditional_branch_placeholder(*condition),
                .target_pc = *instruction.branch_target,
                .condition = *condition,
            });
            break;
        }
        case 0x9FU:
        case 0xA0U:
        case 0xA1U:
        case 0xA2U:
        case 0xA3U:
        case 0xA4U: {
            if (!instruction.branch_target.has_value()) return {};
            if (optimization.branch_taken.has_value()) {
                if (depth < 2U) return {};
                depth -= 2U;
                if (*optimization.branch_taken) {
                    branch_patches.push_back(BranchPatch {
                        .kind = BranchPatch::Kind::unconditional,
                        .native_index = emitter.emit_branch_placeholder(),
                        .target_pc = *instruction.branch_target,
                        .condition = Arm64Condition::equal,
                    });
                }
                break;
            }
            if (!pop_register(kScratchRight) ||
                !pop_register(kScratchLeft)) {
                return {};
            }
            const auto condition = branch_condition(instruction.opcode);
            if (!condition.has_value()) return {};
            emitter.compare_w(kScratchLeft, kScratchRight);
            branch_patches.push_back(BranchPatch {
                .kind = BranchPatch::Kind::conditional,
                .native_index =
                    emitter.emit_conditional_branch_placeholder(*condition),
                .target_pc = *instruction.branch_target,
                .condition = *condition,
            });
            break;
        }
        case 0xA5U:
        case 0xA6U: {
            if (!instruction.branch_target.has_value()) return {};
            if (optimization.branch_taken.has_value()) {
                if (depth < 2U) return {};
                depth -= 2U;
                if (*optimization.branch_taken) {
                    branch_patches.push_back(BranchPatch {
                        .kind = BranchPatch::Kind::unconditional,
                        .native_index = emitter.emit_branch_placeholder(),
                        .target_pc = *instruction.branch_target,
                        .condition = Arm64Condition::equal,
                    });
                }
                break;
            }
            if (!pop_reference_register(kScratchRight) ||
                !pop_reference_register(kScratchLeft)) {
                return {};
            }
            const auto condition = branch_condition(instruction.opcode);
            if (!condition.has_value()) return {};
            emitter.compare_x(kScratchLeft, kScratchRight);
            branch_patches.push_back(BranchPatch {
                .kind = BranchPatch::Kind::conditional,
                .native_index =
                    emitter.emit_conditional_branch_placeholder(*condition),
                .target_pc = *instruction.branch_target,
                .condition = *condition,
            });
            break;
        }
        case 0xC6U:
        case 0xC7U: {
            if (!instruction.branch_target.has_value()) return {};
            if (optimization.branch_taken.has_value()) {
                if (depth == 0U) return {};
                --depth;
                if (*optimization.branch_taken) {
                    branch_patches.push_back(BranchPatch {
                        .kind = BranchPatch::Kind::unconditional,
                        .native_index = emitter.emit_branch_placeholder(),
                        .target_pc = *instruction.branch_target,
                        .condition = Arm64Condition::equal,
                    });
                }
                break;
            }
            if (!pop_reference_register(kScratchLeft)) return {};
            const auto condition = branch_condition(instruction.opcode);
            if (!condition.has_value()) return {};
            emitter.compare_zero_x(kScratchLeft);
            branch_patches.push_back(BranchPatch {
                .kind = BranchPatch::Kind::conditional,
                .native_index =
                    emitter.emit_conditional_branch_placeholder(*condition),
                .target_pc = *instruction.branch_target,
                .condition = *condition,
            });
            break;
        }
        case 0xA8U:
        case 0xC9U:
            if (!instruction.branch_target.has_value()) return {};
            emitter.move_imm64(kScratchLeft,
                               static_cast<u64>(instruction.next_pc));
            if (!push_reference_register(kScratchLeft)) return {};
            branch_patches.push_back(BranchPatch {
                .kind = BranchPatch::Kind::unconditional,
                .native_index = emitter.emit_branch_placeholder(),
                .target_pc = *instruction.branch_target,
                .condition = Arm64Condition::equal,
            });
            break;
        case 0xA9U:
            if (jsr_return_pcs.empty()) return {};
            emitter.load_x(kScratchLeft,
                           kStackPointer,
                           local_offset(instruction.local_index));
            for (const usize return_pc : jsr_return_pcs) {
                emitter.move_imm64(kScratchRight,
                                   static_cast<u64>(return_pc));
                emitter.compare_x(kScratchLeft, kScratchRight);
                branch_patches.push_back(BranchPatch {
                    .kind = BranchPatch::Kind::conditional,
                    .native_index = emitter.emit_conditional_branch_placeholder(
                        Arm64Condition::equal),
                    .target_pc = return_pc,
                    .condition = Arm64Condition::equal,
                });
            }
            terminal_deopt_patches.push_back(
                emitter.emit_branch_placeholder());
            break;
        case 0xA7U:
        case 0xC8U:
            if (!instruction.branch_target.has_value()) return {};
            branch_patches.push_back(BranchPatch {
                .kind = BranchPatch::Kind::unconditional,
                .native_index = emitter.emit_branch_placeholder(),
                .target_pc = *instruction.branch_target,
                .condition = Arm64Condition::equal,
            });
            break;
        case 0xAAU:
        case 0xABU:
            if (!instruction.default_target.has_value()) return {};
            if (optimization.forced_target.has_value()) {
                if (depth == 0U) return {};
                --depth;
                branch_patches.push_back(BranchPatch {
                    .kind = BranchPatch::Kind::unconditional,
                    .native_index = emitter.emit_branch_placeholder(),
                    .target_pc = *optimization.forced_target,
                    .condition = Arm64Condition::equal,
                });
                break;
            }
            if (!pop_register(kScratchLeft)) return {};
            for (const SwitchTarget& target : instruction.switch_targets) {
                emitter.move_imm32(kScratchRight, static_cast<u32>(target.key));
                emitter.compare_w(kScratchLeft, kScratchRight);
                branch_patches.push_back(BranchPatch {
                    .kind = BranchPatch::Kind::conditional,
                    .native_index = emitter.emit_conditional_branch_placeholder(
                        Arm64Condition::equal),
                    .target_pc = target.target_pc,
                    .condition = Arm64Condition::equal,
                });
            }
            branch_patches.push_back(BranchPatch {
                .kind = BranchPatch::Kind::unconditional,
                .native_index = emitter.emit_branch_placeholder(),
                .target_pc = *instruction.default_target,
                .condition = Arm64Condition::equal,
            });
            break;
        case 0xACU:
            if (!pop_register(kScratchLeft) || depth != 0U) return {};
            emit_normal_return(true, kScratchLeft);
            break;
        case 0xADU:
            if (!pop_long_register(kScratchLeft) || depth != 0U) return {};
            emit_normal_return(true, kScratchLeft);
            break;
        case 0xAEU:
            if (!pop_register(kScratchLeft) || depth != 0U) return {};
            emit_normal_return(true, kScratchLeft);
            break;
        case 0xAFU:
            if (!pop_long_register(kScratchLeft) || depth != 0U) return {};
            emit_normal_return(true, kScratchLeft);
            break;
        case 0xB0U:
            if (!pop_reference_register(kScratchLeft) || depth != 0U) return {};
            emit_normal_return(true, kScratchLeft);
            break;
        case 0xB1U:
            if (depth != 0U) return {};
            emit_normal_return(false, kScratchLeft);
            break;
        default:
            return {};
        }
        }
        if (optimization.gvn_capture_register.has_value()) {
            emitter.move_w(*optimization.gvn_capture_register, kScratchLeft);
        }

        const auto shape = stack_shape(instruction, descriptor.return_kind);
        if (!shape.has_value()) return {};
        const u32 expected_depth = *(*depths)[instruction_index] -
                                   shape->pop + shape->push;
        if (depth != expected_depth) return {};
    }

    const usize main_code_end_position = emitter.position();
    std::vector<OsrEntryOffset> osr_entry_offsets;
    if (osr_safe && optimization_plan.contains_loop) {
        std::unordered_map<usize, usize> instruction_index_by_pc;
        instruction_index_by_pc.reserve(decoded->size());
        for (usize index = 0U; index < decoded->size(); ++index) {
            instruction_index_by_pc.emplace((*decoded)[index].pc, index);
        }

        std::vector<usize> osr_targets;
        const auto add_osr_target = [&](usize target_pc, usize source_pc) {
            if (target_pc > source_pc) return;
            if (instruction_index_by_pc.contains(target_pc)) {
                osr_targets.push_back(target_pc);
            }
        };
        for (const DecodedInstruction& instruction : *decoded) {
            if (instruction.branch_target.has_value()) {
                add_osr_target(*instruction.branch_target, instruction.pc);
            }
            for (const SwitchTarget& target : instruction.switch_targets) {
                add_osr_target(target.target_pc, instruction.pc);
            }
        }
        std::sort(osr_targets.begin(), osr_targets.end());
        osr_targets.erase(std::unique(osr_targets.begin(), osr_targets.end()),
                          osr_targets.end());

        for (const usize target_pc : osr_targets) {
            const auto index = instruction_index_by_pc.find(target_pc);
            if (index == instruction_index_by_pc.end() ||
                !(*depths)[index->second].has_value()) {
                continue;
            }
            const u32 target_depth = *(*depths)[index->second];
            const usize copy_slots =
                static_cast<usize>(local_slots) + target_depth;
            const usize entry_offset = emitter.position();

            // OSR uses the same native ABI as normal entry, but x2 points to
            // a physical JVM-slot snapshot (locals first, then operand stack).
            // Rebuild the generated frame and jump directly to the loop header.
            emitter.sub_sp(frame_size);
            for (usize cached_index = 0U;
                 cached_index < cached_locals.size();
                 ++cached_index) {
                emitter.store_x(cached_locals[cached_index].register_index,
                                kStackPointer,
                                saved_register_offset(cached_index));
            }
            for (u32 cached_index = 0U;
                 cached_index < stack_cache_count;
                 ++cached_index) {
                emitter.store_x(kCachedStackRegisters[cached_index],
                                kStackPointer,
                                saved_stack_register_offset(cached_index));
            }
            for (u32 gvn_index = 0U;
                 gvn_index < optimization_plan.gvn_register_count;
                 ++gvn_index) {
                emitter.store_x(kGvnRegisters[gvn_index],
                                kStackPointer,
                                saved_gvn_register_offset(gvn_index));
            }
            if (has_array_lease) {
                emitter.store_x(kArrayLeaseRegister,
                                kStackPointer,
                                saved_array_lease_register_offset());
            }
            emitter.store_x(0U, kStackPointer, kRuntimeContextOffset);
            emitter.store_x(1U, kStackPointer, kRuntimeDispatchOffset);
            emitter.store_x(4U, kStackPointer, kResultPointerOffset);
            emitter.store_x(30U, kStackPointer, kReturnAddressOffset);
            emitter.move_w(kBudgetInitial, 3U);
            emitter.move_w(kBudgetRemaining, 3U);
            emitter.move_x(kResultPointer, 4U);
            emitter.move_imm32(kScratchLeft, 0U);
            emitter.store_w(kScratchLeft,
                            kStackPointer,
                            static_cast<u32>(kJitRuntimeConsumedByteOffset));
            emitter.move_imm32(kScratchLeft, local_slots);
            emitter.store_w(kScratchLeft,
                            kStackPointer,
                            static_cast<u32>(kJitRuntimeLocalSlotsByteOffset));
            emitter.move_imm32(kScratchLeft, target_depth);
            emitter.store_w(kScratchLeft,
                            kStackPointer,
                            static_cast<u32>(kJitRuntimeStackDepthByteOffset));
            for (usize slot = 0U; slot < copy_slots; ++slot) {
                emitter.load_x(kScratchLeft,
                               2U,
                               static_cast<u32>(slot * 8U));
                emitter.store_x(
                    kScratchLeft,
                    kStackPointer,
                    kJitFrameHeaderBytes + static_cast<u32>(slot * 8U));
            }
            for (const CachedLocal& cached : cached_locals) {
                if (cached_local_is_wide(cached.kind)) {
                    emitter.load_x(cached.register_index,
                                   kStackPointer,
                                   local_offset(cached.index));
                } else {
                    emitter.load_w(cached.register_index,
                                   kStackPointer,
                                   local_offset(cached.index));
                }
            }
            for (u32 cached_depth = 0U;
                 cached_depth < std::min(stack_cache_count, target_depth);
                 ++cached_depth) {
                emitter.load_x(kCachedStackRegisters[cached_depth],
                               kStackPointer,
                               stack_offset(cached_depth));
            }
            branch_patches.push_back(BranchPatch {
                .kind = BranchPatch::Kind::unconditional,
                .native_index = emitter.emit_branch_placeholder(),
                .target_pc = target_pc,
                .condition = Arm64Condition::equal,
            });
            osr_entry_offsets.push_back(OsrEntryOffset {
                .bytecode_pc = static_cast<u32>(target_pc),
                .native_offset = entry_offset,
                .local_slots = local_slots,
                .stack_slots = target_depth,
            });
        }
    }

    // Integer/long divide-by-zero stays entirely on the native path. The hot
    // non-zero path executes the ARM64 divide directly; only the cold zero
    // branch enters the runtime to materialize ArithmeticException and route it
    // through the same native catch-handler machinery as other JIT exceptions.
    for (const ArithmeticTrapSite& site : arithmetic_trap_sites) {
        const usize stub_position = emitter.position();
        const bool patched = site.wide
            ? emitter.patch_cbz_x(site.zero_branch,
                                  stub_position,
                                  kScratchRight)
            : emitter.patch_cbz_w(site.zero_branch,
                                  stub_position,
                                  kScratchRight);
        if (!patched) return {};
        active_safepoint_depth = site.stack_depth;
        requires_runtime_dispatch = true;
        emit_runtime_call(JitRuntimeOperation::arithmetic_exception,
                          0U,
                          31U,
                          31U,
                          31U,
                          site.bytecode_pc);
        // arithmetic_exception never returns success; keep a defensive terminal
        // branch if a future runtime implementation changes that contract.
        terminal_deopt_patches.push_back(emitter.emit_branch_placeholder());
    }

    std::vector<usize> exception_stub_deopt_patches;
    for (const RuntimeExceptionSite& site : runtime_exception_sites) {
        const usize stub_position = emitter.position();
        if (!emitter.patch_conditional_branch(
                site.failure_branch,
                stub_position,
                Arm64Condition::not_equal)) {
            return {};
        }

        // Runtime helpers normalize all catchable implicit/runtime failures to
        // java_throwable. Non-throwable failures still deopt conservatively.
        emitter.compare_imm_w(
            0U, static_cast<u32>(JitRuntimeStatus::java_throwable));
        exception_stub_deopt_patches.push_back(
            emitter.emit_conditional_branch_placeholder(
                Arm64Condition::not_equal));

        // Preserve the throwable in the handler's canonical operand-stack
        // slot before asking the VM to select the first matching catch block.
        emitter.load_x(kScratchLeft,
                       kStackPointer,
                       kRuntimeResultOffset);
        emitter.store_x(kScratchLeft,
                        kStackPointer,
                        stack_offset(0U));
        emitter.store_w(kBudgetInitial,
                        kStackPointer,
                        kBudgetInitialOffset);
        emitter.store_w(kBudgetRemaining,
                        kStackPointer,
                        kBudgetRemainingOffset);
        emitter.move_imm32(kScratchMetadata, 0U);
        emitter.store_w(kScratchMetadata,
                        kStackPointer,
                        static_cast<u32>(kJitRuntimeConsumedByteOffset));
        emitter.move_imm32(kScratchMetadata, 1U);
        emitter.store_w(kScratchMetadata,
                        kStackPointer,
                        static_cast<u32>(kJitRuntimeStackDepthByteOffset));
        emitter.load_x(0U, kStackPointer, kRuntimeContextOffset);
        emitter.move_imm32(
            1U, static_cast<u32>(JitRuntimeOperation::match_exception_handler));
        const u64 packed_operand =
            (static_cast<u64>(site.safepoint_id) << 32U) |
            static_cast<u64>(site.bytecode_pc);
        emitter.move_imm64(2U, packed_operand);
        emitter.load_x(3U, kStackPointer, stack_offset(0U));
        emitter.move_imm32(4U, 0U);
        emitter.move_imm32(5U, 0U);
        emitter.add_imm_x(6U, kStackPointer, 0U);
        emitter.add_imm_x(7U, kStackPointer, kRuntimeResultOffset);
        emitter.load_x(16U, kStackPointer, kRuntimeDispatchOffset);
        emitter.branch_link_register(16U);
        emitter.load_w(kBudgetInitial,
                       kStackPointer,
                       kBudgetInitialOffset);
        emitter.load_w(kBudgetRemaining,
                       kStackPointer,
                       kBudgetRemainingOffset);
        emitter.load_x(kResultPointer,
                       kStackPointer,
                       kResultPointerOffset);
        emitter.compare_zero_w(0U);
        exception_stub_deopt_patches.push_back(
            emitter.emit_conditional_branch_placeholder(
                Arm64Condition::not_equal));

        emitter.load_w(kScratchLeft,
                       kStackPointer,
                       kRuntimeResultOffset);
        for (const u16 handler_pc : site.handler_pcs) {
            emitter.move_imm32(kScratchRight,
                               static_cast<u32>(handler_pc));
            emitter.compare_w(kScratchLeft, kScratchRight);
            branch_patches.push_back(BranchPatch {
                .kind = BranchPatch::Kind::conditional,
                .native_index =
                    emitter.emit_conditional_branch_placeholder(
                        Arm64Condition::equal),
                .target_pc = static_cast<usize>(handler_pc),
                .condition = Arm64Condition::equal,
            });
        }
        terminal_deopt_patches.push_back(
            emitter.emit_branch_placeholder());
    }

    const usize deopt_position = emitter.position();
    emitter.sub_w(kScratchThird, kBudgetInitial, kBudgetRemaining);
    emitter.move_deopt_marker_x0();
    emitter.or_x_shifted(0U, 0U, kScratchThird, 32U);
    emitter.load_x(30U, kStackPointer, kReturnAddressOffset);
    restore_cached_registers();
    emitter.add_sp(frame_size);
    emitter.ret();

    const usize budget_deopt_position = emitter.position();
    emitter.sub_w(kScratchThird, kBudgetInitial, kBudgetRemaining);
    emitter.move_budget_deopt_marker_x0();
    emitter.or_x_shifted(0U, 0U, kScratchThird, 32U);
    emitter.load_x(30U, kStackPointer, kReturnAddressOffset);
    restore_cached_registers();
    emitter.add_sp(frame_size);
    emitter.ret();

    for (const BranchPatch& patch : branch_patches) {
        const auto target = native_position_by_pc.find(patch.target_pc);
        if (target == native_position_by_pc.end()) return {};
        usize target_position = target->second;
        if (patch.native_index < main_code_end_position) {
            usize source_pc = 0U;
            usize source_position = 0U;
            bool source_found = false;
            for (const DecodedInstruction& source : *decoded) {
                const auto source_native = native_position_by_pc.find(source.pc);
                if (source_native == native_position_by_pc.end() ||
                    source_native->second > patch.native_index) {
                    continue;
                }
                if (!source_found || source_native->second >= source_position) {
                    source_found = true;
                    source_pc = source.pc;
                    source_position = source_native->second;
                }
            }
            if (source_found) {
                bool inside_preheader_loop = false;
                for (const LicmExpression& invariant :
                     optimization_plan.licm_expressions) {
                    if (invariant.loop_header_pc == patch.target_pc &&
                        source_pc >= invariant.loop_header_pc &&
                        source_pc <= invariant.loop_end_pc) {
                        inside_preheader_loop = true;
                        break;
                    }
                }
                if (!inside_preheader_loop) {
                    for (const ArrayLoopLease& lease :
                         optimization_plan.array_leases) {
                        if (lease.loop_header_pc == patch.target_pc &&
                            source_pc >= lease.loop_header_pc &&
                            source_pc <= lease.loop_end_pc) {
                            inside_preheader_loop = true;
                            break;
                        }
                    }
                }
                if (inside_preheader_loop) {
                    const auto body = loop_body_position_by_pc.find(
                        patch.target_pc);
                    if (body != loop_body_position_by_pc.end()) {
                        target_position = body->second;
                    }
                }
            }
        }
        const bool patched = patch.kind == BranchPatch::Kind::unconditional
            ? emitter.patch_branch(patch.native_index, target_position)
            : emitter.patch_conditional_branch(
                patch.native_index, target_position, patch.condition);
        if (!patched) return {};
    }
    for (const usize patch : budget_patches) {
        if (!emitter.patch_conditional_branch(
                patch,
                budget_deopt_position,
                Arm64Condition::unsigned_lower)) {
            return {};
        }
    }
    for (const usize patch : runtime_failure_patches) {
        if (!emitter.patch_conditional_branch(
                patch,
                deopt_position,
                Arm64Condition::not_equal)) {
            return {};
        }
    }
    for (const usize patch : exception_stub_deopt_patches) {
        if (!emitter.patch_conditional_branch(
                patch,
                deopt_position,
                Arm64Condition::not_equal)) {
            return {};
        }
    }
    for (const usize patch : terminal_deopt_patches) {
        if (!emitter.patch_branch(patch, deopt_position)) return {};
    }

    auto block = executable_arena.finalize(emitter.code());
    if (!block.has_value()) {
        return CompileAttempt {
            .disposition = CompileDisposition::executable_memory_unavailable,
            .method = std::nullopt,
            .reject_reason = JitRejectReason::executable_memory,
        };
    }
    std::unordered_map<u32, JitOsrEntry> osr_entries;
    osr_entries.reserve(osr_entry_offsets.size());
    for (const OsrEntryOffset& entry : osr_entry_offsets) {
        auto* address = static_cast<u8*>(block->memory) +
            entry.native_offset * sizeof(u32);
        osr_entries.emplace(entry.bytecode_pc, JitOsrEntry {
            .function = function_at(address),
            .local_slots = entry.local_slots,
            .stack_slots = entry.stack_slots,
        });
    }
    return CompileAttempt {
        .disposition = CompileDisposition::success,
        .method = CompiledMethod {
            .block = std::move(*block),
            .osr_entries = std::move(osr_entries),
            .deopt_frame_maps = std::move(deopt_frame_maps),
            .reference_maps = std::move(reference_maps),
            .maximum_reference_roots = maximum_reference_roots,
            .return_kind = descriptor.return_kind,
            .returns_value = descriptor.return_kind != JavaTypeKind::void_type,
            .requires_runtime_dispatch = requires_runtime_dispatch,
            .quick_tier = quick_tier,
            .optimized = optimization_plan.propagated_constants != 0U ||
                         optimization_plan.folded_operations != 0U ||
                         optimization_plan.folded_branches != 0U ||
                         optimization_plan.dead_local_writes_eliminated != 0U ||
                         optimization_plan.common_subexpressions_eliminated != 0U ||
                         optimization_plan.loop_invariants_hoisted != 0U ||
                         optimization_plan.scalar_replaced_allocations != 0U ||
                         !optimization_plan.array_leases.empty() ||
                         !inline_recipes.empty() ||
                         strength_reductions != 0U,
            .contains_loop = optimization_plan.contains_loop,
            .propagated_constants = optimization_plan.propagated_constants,
            .folded_operations = optimization_plan.folded_operations,
            .folded_branches = optimization_plan.folded_branches,
            .strength_reductions = strength_reductions,
            .dead_local_writes_eliminated =
                optimization_plan.dead_local_writes_eliminated,
            .common_subexpressions_eliminated =
                optimization_plan.common_subexpressions_eliminated,
            .cross_block_common_subexpressions_eliminated =
                optimization_plan.cross_block_common_subexpressions_eliminated,
            .loop_invariants_hoisted = optimization_plan.loop_invariants_hoisted,
            .inlined_calls = inline_recipes.size(),
            .inlined_bytecodes = inlined_bytecodes,
            .devirtualized_calls = devirtualized_calls,
            .array_bounds_checks_eliminated =
                optimization_plan.array_bounds_checks_eliminated,
            .array_runtime_calls_eliminated =
                optimization_plan.array_runtime_calls_eliminated,
            .scalar_replaced_allocations =
                optimization_plan.scalar_replaced_allocations,
            .scalar_replaced_array_accesses =
                optimization_plan.scalar_replaced_array_accesses,
            .unrolled_int_array_loops = unrolled_int_array_loops,
            .budget_checks_elided =
                optimization_plan.budget_checks_elided,
            .register_cached_loads = register_cached_loads,
            .register_cached_stores_elided = register_cached_stores_elided,
            .stack_cached_pops = stack_cached_pops,
            .stack_cached_stores_elided = stack_cached_stores_elided,
            .register_cached = !cached_locals.empty(),
            .stack_cached = stack_cache_count != 0U,
        },
    };
}

#endif

} // namespace

class BaselineJit::Impl final {
#if defined(__aarch64__)
    struct BackgroundCompileTask final {
        MethodId method_id;
        std::shared_ptr<const classfile::ClassFile> owner;
        std::shared_ptr<const CachedMethodDescriptor> descriptor;
        std::shared_ptr<const VerifiedMethodReferenceMaps> verified_frames;
        const classfile::ClassFile* source_owner {nullptr};
        const classfile::Method* source_method {nullptr};
        bool has_receiver {false};
        bool startup {false};
    };

    struct BackgroundCompileResult final {
        MethodId method_id;
        std::shared_ptr<const classfile::ClassFile> owner;
        const classfile::ClassFile* source_owner {nullptr};
        const classfile::Method* source_method {nullptr};
        bool startup {false};
        u64 compile_time_nanoseconds {0U};
        CompileAttempt attempt;
    };
#endif

public:
    explicit Impl(bool initial_enabled)
        : enabled_(initial_enabled),
          availability_(initial_enabled
              ? probe_platform()
              : JitAvailability::unavailable),
          hot_threshold_(configured_hot_threshold()),
          startup_hot_threshold_(std::max(
              hot_threshold_, configured_startup_hot_threshold())),
          startup_loop_bytecode_threshold_(
              configured_startup_loop_bytecode_threshold()),
          startup_compile_limit_(configured_startup_compile_limit()),
          startup_compile_budget_nanoseconds_(
              configured_startup_compile_budget_nanoseconds()),
          background_compile_enabled_(
              configured_background_compile_enabled() &&
              background_compile_platform_supported()),
          background_compile_worker_limit_(
              configured_background_compile_worker_count()),
          code_cache_limit_bytes_(configured_code_cache_bytes()) {
        stats_.code_cache_limit_bytes = code_cache_limit_bytes_;
    }

    ~Impl() {
#if defined(__aarch64__)
        stop_background_compiler();
#endif
        (void)flush_profile();
    }

    [[nodiscard]] Status configure_profile(std::string path) {
#if defined(__aarch64__)
        profile_path_ = std::move(path);
        hot_profile_.clear();
        stats_.profile_loaded_methods = 0U;
        for (auto& [method_id, entry] : entries_) {
            (void)method_id;
            entry.profile_checked = false;
            entry.profile_hot = false;
        }
        if (profile_path_.empty()) return {};

        std::ifstream input(profile_path_);
        if (!input.is_open()) {
            std::error_code exists_error;
            const bool exists = std::filesystem::exists(
                std::filesystem::path(profile_path_), exists_error);
            if (!exists && !exists_error) return {};
            return fail(ErrorCode::io_error,
                        "failed to open JIT hot profile: " + profile_path_);
        }
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            const usize first = line.find('\t');
            const usize second = first == std::string::npos
                ? std::string::npos
                : line.find('\t', first + 1U);
            if (first == std::string::npos || second == std::string::npos ||
                first == 0U || second == first + 1U ||
                second + 1U >= line.size()) {
                continue;
            }
            hot_profile_.insert(std::move(line));
        }
        if (input.bad()) {
            return fail(ErrorCode::io_error,
                        "failed while reading JIT hot profile: " + profile_path_);
        }
        stats_.profile_loaded_methods = hot_profile_.size();
#else
        (void)path;
#endif
        return {};
    }

    [[nodiscard]] Status flush_profile() {
#if defined(__aarch64__)
        if (profile_path_.empty()) return {};
        std::unordered_set<std::string> hot = hot_profile_;
        for (const auto& [method_id, entry] : entries_) {
            (void)method_id;
            if (entry.source_owner == nullptr || entry.source_method == nullptr) {
                continue;
            }
            if (!entry.compiled.has_value() && !entry.profile_hot &&
                entry.observed_calls < hot_threshold_) {
                continue;
            }
            hot.insert(profile_key(*entry.source_owner, *entry.source_method));
        }

        std::vector<std::string> sorted(hot.begin(), hot.end());
        std::sort(sorted.begin(), sorted.end());
        const std::filesystem::path target(profile_path_);
        if (target.has_parent_path()) {
            std::error_code directory_error;
            std::filesystem::create_directories(
                target.parent_path(), directory_error);
            if (directory_error) {
                return fail(ErrorCode::io_error,
                            "failed to create JIT profile directory: " +
                                target.parent_path().string());
            }
        }
        const std::filesystem::path temporary = target.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output.is_open()) {
                return fail(ErrorCode::io_error,
                            "failed to open temporary JIT profile: " +
                                temporary.string());
            }
            for (const std::string& key : sorted) output << key << '\n';
            output.flush();
            if (!output.good()) {
                return fail(ErrorCode::io_error,
                            "failed to write JIT hot profile: " +
                                temporary.string());
            }
        }
        if (std::rename(temporary.c_str(), target.c_str()) != 0) {
            std::error_code remove_error;
            std::filesystem::remove(temporary, remove_error);
            return fail(ErrorCode::io_error,
                        "failed to publish JIT hot profile: " + profile_path_);
        }
        hot_profile_ = std::move(hot);
        stats_.profile_saved_methods = hot_profile_.size();
#endif
        return {};
    }

    void set_enabled(bool value) noexcept {
        enabled_ = value;
        if (!enabled_) {
            clear_cache();
            availability_ = JitAvailability::unavailable;
            unavailable_probe_countdown_ = 0U;
            return;
        }
        refresh_availability();
    }

    void set_inline_resolver(JitInlineResolverHooks hooks) noexcept {
        inline_resolver_ = hooks;
    }

    void set_startup_mode(bool value) noexcept {
        const bool previous = startup_mode_.exchange(
            value, std::memory_order_acq_rel);
        if (value && !previous) {
            startup_compile_attempts_.store(0U, std::memory_order_release);
            startup_compile_time_nanoseconds_.store(
                0U, std::memory_order_release);
        }
    }

    void refresh_availability() noexcept {
        availability_ = probe_platform();
        unavailable_probe_countdown_ = availability_ == JitAvailability::ready
            ? 0U
            : kUnavailableProbeInterval;
    }

    void clear_cache() noexcept {
#if defined(__aarch64__)
        stop_background_compiler();
        entries_.clear();
        executable_arena_.clear();
        background_executable_arena_.clear();
        stats_.executable_mapped_bytes = 0U;
        stats_.executable_slab_count = 0U;
#endif
        code_cache_bytes_ = 0U;
        stats_.code_cache_bytes = 0U;
    }

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool startup_mode() const noexcept {
        return startup_mode_.load(std::memory_order_acquire);
    }
    [[nodiscard]] JitAvailability availability() const noexcept {
        return availability_;
    }
    [[nodiscard]] JitStatistics statistics() const noexcept {
        JitStatistics result = stats_;
        result.background_compile_queue_peak =
            background_compile_queue_peak_.load(std::memory_order_acquire);
        result.background_compile_worker_count =
            background_compile_worker_count_.load(std::memory_order_acquire);
        result.background_compile_render_cooldown_waits =
            background_compile_render_cooldown_waits_.load(
                std::memory_order_acquire);
        result.background_compile_render_cooldown_nanoseconds =
            background_compile_render_cooldown_nanoseconds_.load(
                std::memory_order_acquire);
        return result;
    }

    void note_frame_published() noexcept {
#if defined(__aarch64__)
        const u64 nanoseconds = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        last_frame_publication_nanoseconds_.store(
            nanoseconds, std::memory_order_release);
        background_condition_.notify_all();
#endif
    }

    [[nodiscard]] bool reserve_foreground_compile_slot() noexcept {
        if (!conservative_device_jit_mode()) return true;

        const auto& coordinator = runtime::shared_work_coordinator();
        const runtime::ThermalPressure thermal = coordinator.thermal_pressure();
        // Once the device is already hot, compiling more code is the wrong
        // trade: keep executing the interpreter / already-published native
        // entries and let compilation resume automatically after cooling.
        if (thermal == runtime::ThermalPressure::serious ||
            thermal == runtime::ThermalPressure::critical ||
            coordinator.active_frame_jobs() != 0U) {
            return false;
        }

        u64 interval = device_foreground_compile_interval_nanoseconds_;
        if (interval == 0U) return true;
        if (thermal == runtime::ThermalPressure::fair) {
            interval = std::max<u64>(interval, 1'000'000'000U);
        } else {
            switch (coordinator.frame_pressure()) {
            case runtime::FramePressure::high:
            case runtime::FramePressure::overloaded:
                interval = std::max<u64>(interval, 500'000'000U);
                break;
            case runtime::FramePressure::low:
            case runtime::FramePressure::normal:
                break;
            }
        }

        const u64 now = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        u64 next = next_foreground_compile_nanoseconds_.load(
            std::memory_order_acquire);
        for (;;) {
            if (now < next) return false;
            const u64 deadline = now > std::numeric_limits<u64>::max() - interval
                ? std::numeric_limits<u64>::max()
                : now + interval;
            if (next_foreground_compile_nanoseconds_.compare_exchange_weak(
                    next,
                    deadline,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    [[nodiscard]] Result<std::optional<JitExecutionResult>> try_execute(
        MethodId method_id,
        const classfile::ClassFile& owner,
        const classfile::Method& method,
        const CachedMethodDescriptor& descriptor,
        const InvocationArguments& arguments,
        bool has_receiver,
        u64 instruction_budget,
        JitRuntimeHooks runtime_hooks,
        std::shared_ptr<const classfile::ClassFile> owner_lifetime,
        std::shared_ptr<const VerifiedMethodReferenceMaps> verified_frames,
        std::shared_ptr<const CachedMethodDescriptor> descriptor_lifetime) {
        if (!enabled_ || !method_id.valid()) {
            return std::optional<JitExecutionResult>{};
        }
        if (availability_ != JitAvailability::ready) {
            if (unavailable_probe_countdown_ == 0U) {
                refresh_availability();
            } else {
                --unavailable_probe_countdown_;
            }
            if (availability_ != JitAvailability::ready) {
                ++stats_.platform_fallbacks;
                return std::optional<JitExecutionResult>{};
            }
        }
#if !defined(__aarch64__)
        (void)owner;
        (void)method;
        (void)descriptor;
        (void)arguments;
        (void)has_receiver;
        (void)instruction_budget;
        (void)runtime_hooks;
        (void)owner_lifetime;
        (void)verified_frames;
        ++stats_.platform_fallbacks;
        return std::optional<JitExecutionResult>{};
#else
        publish_background_results();
        if (instruction_budget >
            static_cast<u64>(kMaximumPackedBudget)) {
            // The compact native ABI stores the exact executed bytecode count
            // in 30 bits. Preserve scheduler semantics by using the interpreter
            // rather than truncating a larger long-lived budget.
            ++stats_.budget_fallbacks;
            ++stats_.reject_reasons[
                static_cast<usize>(JitRejectReason::instruction_budget)];
            return std::optional<JitExecutionResult>{};
        }

        const usize expected_arguments =
            descriptor.descriptor.parameters.size() + (has_receiver ? 1U : 0U);
        if (arguments.size() != expected_arguments) {
            ++stats_.argument_fallbacks;
            ++stats_.reject_reasons[
                static_cast<usize>(JitRejectReason::argument_kind)];
            return std::optional<JitExecutionResult>{};
        }
        for (usize index = 0; index < arguments.size(); ++index) {
            switch (arguments.kind(index)) {
            case ValueKind::int32:
            case ValueKind::int64:
            case ValueKind::float32:
            case ValueKind::float64:
            case ValueKind::reference:
                break;
            default:
                ++stats_.argument_fallbacks;
                ++stats_.reject_reasons[
                    static_cast<usize>(JitRejectReason::argument_kind)];
                return std::optional<JitExecutionResult>{};
            }
        }

        Entry& entry = entries_[method_id];
        if ((entry.source_method != nullptr && entry.source_method != &method) ||
            (entry.source_owner != nullptr && entry.source_owner != &owner)) {
            release_compiled_entry(entry);
            entry = Entry {};
        }
        entry.source_method = &method;
        entry.source_owner = &owner;
        if (!entry.profile_checked) {
            entry.profile_checked = true;
            if (!hot_profile_.empty() &&
                hot_profile_.contains(profile_key(owner, method))) {
                entry.profile_hot = true;
                if (!conservative_device_jit_mode()) {
                    entry.compilation_threshold = 1U;
                }
                ++stats_.profile_prewarm_hits;
            }
        }
        if (entry.rejected) {
            ++stats_.unsupported_fallbacks;
            return std::optional<JitExecutionResult>{};
        }

        if (!entry.compiled.has_value()) {
            const bool startup = startup_mode();
            const auto analyze_loop_candidate = [&]() {
                if (entry.loop_analyzed) return;
                entry.loop_analyzed = true;
                auto decoded = decode_method(owner, method);
                if (!decoded.has_value()) return;
                for (const auto& instruction : *decoded) {
                    bool backward = instruction.branch_target.has_value() &&
                        *instruction.branch_target <= instruction.pc;
                    if (!backward) {
                        for (const SwitchTarget& target :
                             instruction.switch_targets) {
                            if (target.target_pc <= instruction.pc) {
                                backward = true;
                                break;
                            }
                        }
                    }
                    if (!backward) continue;
                    entry.loop_candidate = true;
                    entry.compilation_threshold = conservative_device_jit_mode()
                        ? hot_threshold_
                        : 1U;
                    ++stats_.hot_loop_candidates;
                    break;
                }
            };

            if (entry.compilation_threshold == 0U) {
                entry.compilation_threshold = startup
                    ? startup_hot_threshold_
                    : hot_threshold_;
            }
            if (!entry.loop_analyzed) {
                const usize bytecode_size = method.code.has_value()
                    ? method.code->bytecode.size()
                    : 0U;
                if (startup &&
                    bytecode_size < startup_loop_bytecode_threshold_) {
                    if (!entry.startup_loop_analysis_deferred) {
                        entry.startup_loop_analysis_deferred = true;
                        ++stats_.startup_loop_analysis_deferred;
                    }
                } else {
                    analyze_loop_candidate();
                }
            }
            if (!startup) {
                entry.startup_compile_deferred = false;
                if (!entry.loop_analyzed) analyze_loop_candidate();
                entry.compilation_threshold = entry.loop_candidate
                    ? (conservative_device_jit_mode() ? hot_threshold_ : 1U)
                    : std::min(entry.compilation_threshold, hot_threshold_);
            }

            if (entry.observed_calls != std::numeric_limits<u32>::max()) {
                ++entry.observed_calls;
            }
            const u32 required_calls = adaptive_device_hot_threshold(
                entry.compilation_threshold);
            if (entry.observed_calls < required_calls) {
                ++stats_.warmup_fallbacks;
                return std::optional<JitExecutionResult>{};
            }
            if (entry.compile_pending) {
                ++stats_.warmup_fallbacks;
                return std::optional<JitExecutionResult>{};
            }
            if (startup && enqueue_background_compile(
                    method_id,
                    owner,
                    method,
                    descriptor,
                    has_receiver,
                    true,
                    owner_lifetime,
                    verified_frames,
                    descriptor_lifetime)) {
                entry.compile_pending = true;
                entry.startup_compile_deferred = false;
                ++stats_.warmup_fallbacks;
                return std::optional<JitExecutionResult>{};
            }
            if (startup && entry.startup_compile_deferred) {
                ++stats_.warmup_fallbacks;
                return std::optional<JitExecutionResult>{};
            }
            if (startup) {
                const u32 startup_attempts = startup_compile_attempts_.load(
                    std::memory_order_acquire);
                const u64 startup_compile_time =
                    startup_compile_time_nanoseconds_.load(
                        std::memory_order_acquire);
                if (startup_compile_limit_ == 0U ||
                    startup_compile_budget_nanoseconds_ == 0U ||
                    startup_attempts >= startup_compile_limit_ ||
                    startup_compile_time >=
                        startup_compile_budget_nanoseconds_) {
                    entry.startup_compile_deferred = true;
                    ++stats_.startup_compile_deferred;
                    ++stats_.warmup_fallbacks;
                    return std::optional<JitExecutionResult>{};
                }
                startup_compile_attempts_.fetch_add(
                    1U, std::memory_order_acq_rel);
                ++stats_.startup_compile_attempts;
            }

            if (!reserve_foreground_compile_slot()) {
                ++stats_.foreground_compile_deferred;
                ++stats_.warmup_fallbacks;
                return std::optional<JitExecutionResult>{};
            }

            const auto compile_started = std::chrono::steady_clock::now();
            ++stats_.compile_attempts;
            CompileAttempt attempt = compile_scalar_method(
                owner, method, descriptor, has_receiver, executable_arena_,
                inline_resolver_, verified_frames.get());
            const u64 compile_elapsed_nanoseconds = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - compile_started).count());
            stats_.compile_time_nanoseconds += compile_elapsed_nanoseconds;
            if (startup) {
                startup_compile_time_nanoseconds_.fetch_add(
                    compile_elapsed_nanoseconds, std::memory_order_acq_rel);
            }
            if (attempt.disposition ==
                    CompileDisposition::executable_memory_unavailable) {
                record_reject(attempt.reject_reason,
                              attempt.unsupported_opcode,
                              owner,
                              method);
                ++stats_.platform_fallbacks;
                refresh_availability();
                return std::optional<JitExecutionResult>{};
            }
            if (attempt.disposition != CompileDisposition::success ||
                !attempt.method.has_value()) {
                entry.rejected = true;
                ++stats_.rejected_methods;
                ++stats_.unsupported_fallbacks;
                record_reject(attempt.reject_reason,
                              attempt.unsupported_opcode,
                              owner,
                              method);
                return std::optional<JitExecutionResult>{};
            }

            const u64 needed = attempt.method->block.mapped_size;
            if (needed > code_cache_limit_bytes_ ||
                !evict_for(needed, method_id)) {
                entry.rejected = true;
                ++stats_.rejected_methods;
                ++stats_.unsupported_fallbacks;
                record_reject(JitRejectReason::code_cache_limit,
                              std::nullopt,
                              owner,
                              method);
                return std::optional<JitExecutionResult>{};
            }
            code_cache_bytes_ += needed;
            stats_.code_cache_bytes = code_cache_bytes_;
            update_executable_arena_statistics();
            entry.compiled = std::move(*attempt.method);
            entry.deopt_count = 0U;
            entry.retryable_call_count = 0U;
            entry.last_used_tick = ++use_tick_;
            ++stats_.compiled_methods;
            if (entry.compiled->quick_tier) ++stats_.quick_compiled_methods;
            stats_.osr_compiled_entries += entry.compiled->osr_entries.size();
            if (startup) ++stats_.startup_compiled_methods;
            if (entry.compiled->optimized) ++stats_.optimized_methods;
            if (entry.compiled->contains_loop) {
                ++stats_.loop_optimized_methods;
            }
            const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
            if (trace_value != nullptr && *trace_value != '\0' &&
                std::string_view(trace_value) != "0") {
                std::fprintf(stderr,
                             "[phoneMEJIT] compile %s.%s%s optimized=%d loop=%d "
                             "code_bytes=%llu\n",
                             owner.name().c_str(),
                             method.name.c_str(),
                             method.descriptor.c_str(),
                             entry.compiled->optimized ? 1 : 0,
                             entry.compiled->contains_loop ? 1 : 0,
                             static_cast<unsigned long long>(needed));
            }
            stats_.propagated_constants +=
                entry.compiled->propagated_constants;
            stats_.folded_operations += entry.compiled->folded_operations;
            stats_.folded_branches += entry.compiled->folded_branches;
            stats_.strength_reductions +=
                entry.compiled->strength_reductions;
            stats_.dead_local_writes_eliminated +=
                entry.compiled->dead_local_writes_eliminated;
            stats_.common_subexpressions_eliminated +=
                entry.compiled->common_subexpressions_eliminated;
            stats_.cross_block_common_subexpressions_eliminated +=
                entry.compiled->cross_block_common_subexpressions_eliminated;
            stats_.loop_invariants_hoisted +=
                entry.compiled->loop_invariants_hoisted;
            stats_.inlined_calls += entry.compiled->inlined_calls;
            stats_.inlined_bytecodes += entry.compiled->inlined_bytecodes;
            stats_.devirtualized_calls += entry.compiled->devirtualized_calls;
            stats_.array_bounds_checks_eliminated +=
                entry.compiled->array_bounds_checks_eliminated;
            stats_.array_runtime_calls_eliminated +=
                entry.compiled->array_runtime_calls_eliminated;
            stats_.scalar_replaced_allocations +=
                entry.compiled->scalar_replaced_allocations;
            stats_.scalar_replaced_array_accesses +=
                entry.compiled->scalar_replaced_array_accesses;
            stats_.unrolled_int_array_loops +=
                entry.compiled->unrolled_int_array_loops;
            stats_.budget_checks_elided +=
                entry.compiled->budget_checks_elided;
            if (entry.compiled->register_cached) {
                ++stats_.register_cached_methods;
            }
            stats_.register_cached_loads +=
                entry.compiled->register_cached_loads;
            stats_.register_cached_stores_elided +=
                entry.compiled->register_cached_stores_elided;
            if (entry.compiled->stack_cached) {
                ++stats_.stack_cached_methods;
            }
            stats_.stack_cached_pops += entry.compiled->stack_cached_pops;
            stats_.stack_cached_stores_elided +=
                entry.compiled->stack_cached_stores_elided;
        }

        if (entry.compiled->requires_runtime_dispatch &&
            runtime_hooks.dispatch == nullptr) {
            ++stats_.unsupported_fallbacks;
            return std::optional<JitExecutionResult>{};
        }

        const u32 native_budget = static_cast<u32>(instruction_budget);
        const std::span<const u64> raw_arguments = arguments.raw_bits_span();
        const u64* argument_pointer = arguments.empty()
            ? nullptr
            : raw_arguments.data();
        u64 result_bits = 0U;
        RuntimeDispatchContext dispatch_context {
            .hooks = runtime_hooks,
            .reference_maps = &entry.compiled->reference_maps,
        };
        if (entry.compiled->requires_runtime_dispatch) {
            dispatch_context.prepare_roots(
                entry.compiled->maximum_reference_roots);
            if (method.code.has_value()) {
                dispatch_context.prepare_deopt(
                    static_cast<usize>(method.code->max_locals) +
                    static_cast<usize>(method.code->max_stack));
            }
        }
        const auto execution_started = std::chrono::steady_clock::now();
        const u64 raw_result = entry.compiled->block.function(
            entry.compiled->requires_runtime_dispatch
                ? static_cast<void*>(&dispatch_context)
                : runtime_hooks.context,
            entry.compiled->requires_runtime_dispatch
                ? &dispatch_runtime_trampoline
                : runtime_hooks.dispatch,
            argument_pointer,
            native_budget,
            &result_bits);
        stats_.execution_time_nanoseconds += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - execution_started).count());
        entry.last_used_tick = ++use_tick_;

        const u32 executed = static_cast<u32>(
            (raw_result >> 32U) & static_cast<u64>(kMaximumPackedBudget));
        if ((raw_result & kDeoptMarker) != 0U) {
            ++stats_.deoptimized_executions;
            if ((raw_result & kBudgetExhaustedMarker) != 0U) {
                ++stats_.budget_fallbacks;
                return fail(
                    ErrorCode::invalid_state,
                    "JIT bytecode instruction budget was exhausted");
            }
            const auto runtime_status = static_cast<JitRuntimeStatus>(
                static_cast<u32>(result_bits));
            const u32 exception_bci = static_cast<u32>(result_bits >> 32U);
            std::optional<JitExceptionKind> exception;
            switch (runtime_status) {
            case JitRuntimeStatus::fatal_runtime_error:
                return fail(
                    ErrorCode::internal_error,
                    "JIT runtime helper failed after nested execution began");
            case JitRuntimeStatus::unsupported_call: {
                ++stats_.unsupported_fallbacks;
                auto deopt_state = decode_deopt_state(
                    *entry.compiled, dispatch_context, exception_bci);
                release_compiled_entry(entry);
                entry.rejected = true;
                ++stats_.rejected_methods;
                if (deopt_state.has_value()) {
                    return std::optional<JitExecutionResult>(JitExecutionResult {
                        .return_value = std::nullopt,
                        .exception = std::nullopt,
                        .exception_bci = 0U,
                        .bytecode_instructions = executed,
                        .deopt_state = std::move(deopt_state),
                    });
                }
                return std::optional<JitExecutionResult>{};
            }
            case JitRuntimeStatus::retryable_call: {
                ++stats_.unsupported_fallbacks;
                if (entry.retryable_call_count != std::numeric_limits<u32>::max()) {
                    ++entry.retryable_call_count;
                }
                auto deopt_state = decode_deopt_state(
                    *entry.compiled, dispatch_context, exception_bci);
                if (entry.retryable_call_count >= kRetryableCallRetireThreshold) {
                    if (const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
                        trace_value != nullptr && *trace_value != '\0' &&
                        std::string_view(trace_value) != "0") {
                        std::fprintf(stderr,
                                     "[phoneMEJIT] retire %s.%s%s after=%u retryable-calls\n",
                                     owner.name().c_str(),
                                     method.name.c_str(),
                                     method.descriptor.c_str(),
                                     static_cast<unsigned>(entry.retryable_call_count));
                    }
                    release_compiled_entry(entry);
                    entry.rejected = true;
                    ++stats_.rejected_methods;
                }
                if (deopt_state.has_value()) {
                    return std::optional<JitExecutionResult>(JitExecutionResult {
                        .return_value = std::nullopt,
                        .exception = std::nullopt,
                        .exception_bci = 0U,
                        .bytecode_instructions = executed,
                        .deopt_state = std::move(deopt_state),
                    });
                }
                return std::optional<JitExecutionResult>{};
            }
            case JitRuntimeStatus::success:
            case JitRuntimeStatus::deoptimize: {
                ++stats_.unsupported_fallbacks;
                ++entry.deopt_count;
                auto deopt_state = decode_deopt_state(
                    *entry.compiled, dispatch_context, exception_bci);
                if (const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
                    trace_value != nullptr && *trace_value != '\0' &&
                    std::string_view(trace_value) != "0") {
                    std::fprintf(stderr,
                                 "[phoneMEJIT] deopt %s.%s%s status=%u "
                                 "bytecodes=%u count=%u\n",
                                 owner.name().c_str(),
                                 method.name.c_str(),
                                 method.descriptor.c_str(),
                                 static_cast<unsigned>(runtime_status),
                                 executed,
                                 static_cast<unsigned>(entry.deopt_count));
                }
                const u32 deopt_retire_threshold = startup_mode()
                    ? kStartupDeoptRetireThreshold
                    : kNormalDeoptRetireThreshold;
                const bool retire_entry =
                    entry.deopt_count >= deopt_retire_threshold;
                if (retire_entry) {
                    const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
                    if (trace_value != nullptr && *trace_value != '\0' &&
                        std::string_view(trace_value) != "0") {
                        std::fprintf(stderr,
                                     "[phoneMEJIT] retire %s.%s%s after=%u deopts\n",
                                     owner.name().c_str(),
                                     method.name.c_str(),
                                     method.descriptor.c_str(),
                                     static_cast<unsigned>(entry.deopt_count));
                    }
                    release_compiled_entry(entry);
                    entry.rejected = true;
                    ++stats_.rejected_methods;
                }
                if (deopt_state.has_value()) {
                    return std::optional<JitExecutionResult>(JitExecutionResult {
                        .return_value = std::nullopt,
                        .exception = std::nullopt,
                        .exception_bci = 0U,
                        .bytecode_instructions = executed,
                        .deopt_state = std::move(deopt_state),
                    });
                }
                return std::optional<JitExecutionResult>{};
            }
            case JitRuntimeStatus::null_pointer:
                exception = JitExceptionKind::null_pointer;
                break;
            case JitRuntimeStatus::array_index_out_of_bounds:
                exception = JitExceptionKind::array_index_out_of_bounds;
                break;
            case JitRuntimeStatus::array_store:
                exception = JitExceptionKind::array_store;
                break;
            case JitRuntimeStatus::class_cast:
                exception = JitExceptionKind::class_cast;
                break;
            case JitRuntimeStatus::java_throwable:
                exception = JitExceptionKind::runtime_throwable;
                break;
            case JitRuntimeStatus::budget_exhausted:
                ++stats_.budget_fallbacks;
                return fail(
                    ErrorCode::invalid_state,
                    "JIT nested invocation exhausted instruction budget");
            }
            entry.deopt_count = 0U;
            entry.retryable_call_count = 0U;
            ++stats_.executed_methods;
            return std::optional<JitExecutionResult>(JitExecutionResult {
                .return_value = std::nullopt,
                .exception = exception,
                .exception_bci = exception_bci,
                .bytecode_instructions = executed,
            });
        }

        std::optional<Value> return_value;
        if (entry.compiled->returns_value) {
            if (integer_like(entry.compiled->return_kind)) {
                return_value = Value::from_int(
                    static_cast<i32>(static_cast<u32>(result_bits)));
            } else if (entry.compiled->return_kind == JavaTypeKind::long_integer) {
                return_value = Value::from_long(static_cast<i64>(result_bits));
            } else if (entry.compiled->return_kind == JavaTypeKind::float32) {
                return_value = Value::from_float(std::bit_cast<float>(
                    static_cast<u32>(result_bits)));
            } else if (entry.compiled->return_kind == JavaTypeKind::float64) {
                return_value = Value::from_double(std::bit_cast<double>(result_bits));
            } else if (entry.compiled->return_kind == JavaTypeKind::reference ||
                       entry.compiled->return_kind == JavaTypeKind::array) {
                return_value = Value::from_reference(ObjectRef {result_bits});
            } else {
                return fail(ErrorCode::invalid_state,
                            "JIT returned an unsupported value kind");
            }
        }
        entry.deopt_count = 0U;
        entry.retryable_call_count = 0U;
        ++stats_.executed_methods;
        const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
        if (trace_value != nullptr && *trace_value != '\0' &&
            std::string_view(trace_value) != "0") {
            std::fprintf(stderr,
                         "[phoneMEJIT] execute %s.%s%s bytecodes=%u\n",
                         owner.name().c_str(),
                         method.name.c_str(),
                         method.descriptor.c_str(),
                         executed);
        }
        return std::optional<JitExecutionResult>(JitExecutionResult {
            .return_value = return_value,
            .exception = std::nullopt,
            .exception_bci = 0U,
            .bytecode_instructions = executed,
        });
#endif
    }

    [[nodiscard]] Result<std::optional<JitExecutionResult>> try_execute_osr(
        MethodId method_id,
        const classfile::ClassFile& owner,
        const classfile::Method& method,
        const CachedMethodDescriptor& descriptor,
        bool has_receiver,
        JitPhysicalFrameView frame,
        u64 instruction_budget,
        JitRuntimeHooks runtime_hooks,
        std::shared_ptr<const classfile::ClassFile> owner_lifetime,
        std::shared_ptr<const VerifiedMethodReferenceMaps> verified_frames,
        std::shared_ptr<const CachedMethodDescriptor> descriptor_lifetime) {
        ++stats_.osr_attempts;
        if (!enabled_ || !method_id.valid()) {
            ++stats_.osr_fallbacks;
            return std::optional<JitExecutionResult>{};
        }
        if (availability_ != JitAvailability::ready) {
            if (unavailable_probe_countdown_ == 0U) {
                refresh_availability();
            } else {
                --unavailable_probe_countdown_;
            }
            if (availability_ != JitAvailability::ready) {
                ++stats_.platform_fallbacks;
                ++stats_.osr_fallbacks;
                return std::optional<JitExecutionResult>{};
            }
        }
#if !defined(__aarch64__)
        (void)owner;
        (void)method;
        (void)descriptor;
        (void)has_receiver;
        (void)frame;
        (void)instruction_budget;
        (void)runtime_hooks;
        (void)owner_lifetime;
        (void)verified_frames;
        ++stats_.platform_fallbacks;
        ++stats_.osr_fallbacks;
        return std::optional<JitExecutionResult>{};
#else
        publish_background_results();
        if (instruction_budget > static_cast<u64>(kMaximumPackedBudget)) {
            ++stats_.budget_fallbacks;
            ++stats_.osr_fallbacks;
            return std::optional<JitExecutionResult>{};
        }

        Entry& entry = entries_[method_id];
        if ((entry.source_method != nullptr && entry.source_method != &method) ||
            (entry.source_owner != nullptr && entry.source_owner != &owner)) {
            release_compiled_entry(entry);
            entry = Entry {};
        }
        entry.source_method = &method;
        entry.source_owner = &owner;
        if (!entry.profile_checked) {
            entry.profile_checked = true;
            if (!hot_profile_.empty() &&
                hot_profile_.contains(profile_key(owner, method))) {
                entry.profile_hot = true;
                if (!conservative_device_jit_mode()) {
                    entry.compilation_threshold = 1U;
                }
                ++stats_.profile_prewarm_hits;
            }
        }
        if (entry.rejected) {
            ++stats_.osr_fallbacks;
            return std::optional<JitExecutionResult>{};
        }

        if (!entry.compiled.has_value()) {
            bool force_synchronous_compile = false;
            if (entry.compile_pending) {
                if (entry.osr_pending_polls !=
                    std::numeric_limits<u32>::max()) {
                    ++entry.osr_pending_polls;
                }
                if (entry.osr_pending_polls <
                    kOsrBackgroundPromotionPolls) {
                    ++stats_.osr_fallbacks;
                    return std::optional<JitExecutionResult>{};
                }
                // A hot interpreter loop must not miss OSR just because its
                // background compile is delayed by unrelated host work. Detach
                // the pending result (it will be discarded when published) and
                // compile once on the VM thread. This never waits on the worker.
                entry.compile_pending = false;
                entry.osr_pending_polls = 0U;
                force_synchronous_compile = true;
                ++stats_.osr_sync_promotions;
            }
            if (!force_synchronous_compile &&
                enqueue_background_compile(method_id,
                                           owner,
                                           method,
                                           descriptor,
                                           has_receiver,
                                           startup_mode(),
                                           owner_lifetime,
                                           verified_frames,
                                           descriptor_lifetime)) {
                entry.compile_pending = true;
                entry.osr_pending_polls = 0U;
                ++stats_.osr_fallbacks;
                return std::optional<JitExecutionResult>{};
            }
            const auto compile_started = std::chrono::steady_clock::now();
            ++stats_.compile_attempts;
            CompileAttempt attempt = compile_scalar_method(
                owner, method, descriptor, has_receiver, executable_arena_,
                inline_resolver_, verified_frames.get());
            stats_.compile_time_nanoseconds += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - compile_started).count());
            if (attempt.disposition ==
                    CompileDisposition::executable_memory_unavailable) {
                record_reject(attempt.reject_reason,
                              attempt.unsupported_opcode,
                              owner,
                              method);
                ++stats_.platform_fallbacks;
                ++stats_.osr_fallbacks;
                refresh_availability();
                return std::optional<JitExecutionResult>{};
            }
            if (attempt.disposition != CompileDisposition::success ||
                !attempt.method.has_value()) {
                entry.rejected = true;
                ++stats_.rejected_methods;
                ++stats_.unsupported_fallbacks;
                ++stats_.osr_fallbacks;
                record_reject(attempt.reject_reason,
                              attempt.unsupported_opcode,
                              owner,
                              method);
                return std::optional<JitExecutionResult>{};
            }
            const u64 needed = attempt.method->block.mapped_size;
            if (needed > code_cache_limit_bytes_ ||
                !evict_for(needed, method_id)) {
                entry.rejected = true;
                ++stats_.rejected_methods;
                ++stats_.unsupported_fallbacks;
                ++stats_.osr_fallbacks;
                record_reject(JitRejectReason::code_cache_limit,
                              std::nullopt,
                              owner,
                              method);
                return std::optional<JitExecutionResult>{};
            }
            code_cache_bytes_ += needed;
            stats_.code_cache_bytes = code_cache_bytes_;
            update_executable_arena_statistics();
            entry.compiled = std::move(*attempt.method);
            entry.deopt_count = 0U;
            entry.retryable_call_count = 0U;
            entry.last_used_tick = ++use_tick_;
            ++stats_.compiled_methods;
            if (entry.compiled->quick_tier) ++stats_.quick_compiled_methods;
            stats_.osr_compiled_entries += entry.compiled->osr_entries.size();
            if (entry.compiled->optimized) ++stats_.optimized_methods;
            if (entry.compiled->contains_loop) ++stats_.loop_optimized_methods;
            stats_.propagated_constants += entry.compiled->propagated_constants;
            stats_.folded_operations += entry.compiled->folded_operations;
            stats_.folded_branches += entry.compiled->folded_branches;
            stats_.strength_reductions += entry.compiled->strength_reductions;
            stats_.dead_local_writes_eliminated +=
                entry.compiled->dead_local_writes_eliminated;
            stats_.common_subexpressions_eliminated +=
                entry.compiled->common_subexpressions_eliminated;
            stats_.cross_block_common_subexpressions_eliminated +=
                entry.compiled->cross_block_common_subexpressions_eliminated;
            stats_.loop_invariants_hoisted +=
                entry.compiled->loop_invariants_hoisted;
            stats_.inlined_calls += entry.compiled->inlined_calls;
            stats_.inlined_bytecodes += entry.compiled->inlined_bytecodes;
            stats_.devirtualized_calls += entry.compiled->devirtualized_calls;
            stats_.array_bounds_checks_eliminated +=
                entry.compiled->array_bounds_checks_eliminated;
            stats_.array_runtime_calls_eliminated +=
                entry.compiled->array_runtime_calls_eliminated;
            stats_.scalar_replaced_allocations +=
                entry.compiled->scalar_replaced_allocations;
            stats_.scalar_replaced_array_accesses +=
                entry.compiled->scalar_replaced_array_accesses;
            stats_.unrolled_int_array_loops +=
                entry.compiled->unrolled_int_array_loops;
            stats_.budget_checks_elided += entry.compiled->budget_checks_elided;
            if (entry.compiled->register_cached) {
                ++stats_.register_cached_methods;
            }
            stats_.register_cached_loads +=
                entry.compiled->register_cached_loads;
            stats_.register_cached_stores_elided +=
                entry.compiled->register_cached_stores_elided;
            if (entry.compiled->stack_cached) {
                ++stats_.stack_cached_methods;
            }
            stats_.stack_cached_pops += entry.compiled->stack_cached_pops;
            stats_.stack_cached_stores_elided +=
                entry.compiled->stack_cached_stores_elided;
        }

        if (!frame.valid()) {
            ++stats_.osr_fallbacks;
            return std::optional<JitExecutionResult>{};
        }
        const auto osr = entry.compiled->osr_entries.find(frame.bytecode_pc);
        if (osr == entry.compiled->osr_entries.end() ||
            osr->second.function == nullptr ||
            frame.local_slots != osr->second.local_slots ||
            frame.stack_slots != osr->second.stack_slots ||
            (entry.compiled->requires_runtime_dispatch &&
             runtime_hooks.dispatch == nullptr)) {
            ++stats_.osr_fallbacks;
            return std::optional<JitExecutionResult>{};
        }

        RuntimeDispatchContext dispatch_context {
            .hooks = runtime_hooks,
            .reference_maps = &entry.compiled->reference_maps,
        };
        if (entry.compiled->requires_runtime_dispatch) {
            dispatch_context.prepare_roots(
                entry.compiled->maximum_reference_roots);
            if (method.code.has_value()) {
                dispatch_context.prepare_deopt(
                    static_cast<usize>(method.code->max_locals) +
                    static_cast<usize>(method.code->max_stack));
            }
        }

        u64 result_bits = 0U;
        const auto execution_started = std::chrono::steady_clock::now();
        const u64 raw_result = osr->second.function(
            entry.compiled->requires_runtime_dispatch
                ? static_cast<void*>(&dispatch_context)
                : runtime_hooks.context,
            entry.compiled->requires_runtime_dispatch
                ? &dispatch_runtime_trampoline
                : runtime_hooks.dispatch,
            frame.physical_slots.empty() ? nullptr
                                         : frame.physical_slots.data(),
            static_cast<u32>(instruction_budget),
            &result_bits);
        stats_.execution_time_nanoseconds += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - execution_started).count());
        entry.last_used_tick = ++use_tick_;

        const u32 executed = static_cast<u32>(
            (raw_result >> 32U) & static_cast<u64>(kMaximumPackedBudget));
        if ((raw_result & kDeoptMarker) != 0U) {
            ++stats_.deoptimized_executions;
            if ((raw_result & kBudgetExhaustedMarker) != 0U) {
                ++stats_.budget_fallbacks;
                ++stats_.osr_fallbacks;
                return fail(ErrorCode::invalid_state,
                            "JIT OSR instruction budget was exhausted");
            }
            const auto runtime_status = static_cast<JitRuntimeStatus>(
                static_cast<u32>(result_bits));
            const u32 bytecode_pc = static_cast<u32>(result_bits >> 32U);
            if (runtime_status == JitRuntimeStatus::deoptimize ||
                runtime_status == JitRuntimeStatus::unsupported_call ||
                runtime_status == JitRuntimeStatus::retryable_call ||
                runtime_status == JitRuntimeStatus::success) {
                const bool retryable_call =
                    runtime_status == JitRuntimeStatus::retryable_call;
                if (retryable_call) {
                    if (entry.retryable_call_count !=
                        std::numeric_limits<u32>::max()) {
                        ++entry.retryable_call_count;
                    }
                } else {
                    ++entry.deopt_count;
                }
                auto deopt_state = decode_deopt_state(
                    *entry.compiled, dispatch_context, bytecode_pc);
                const u32 deopt_retire_threshold = startup_mode()
                    ? kStartupDeoptRetireThreshold
                    : kNormalDeoptRetireThreshold;
                const bool retire_entry = retryable_call
                    ? entry.retryable_call_count >= kRetryableCallRetireThreshold
                    : (runtime_status == JitRuntimeStatus::unsupported_call ||
                       entry.deopt_count >= deopt_retire_threshold);
                if (const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
                    trace_value != nullptr && *trace_value != '\0' &&
                    std::string_view(trace_value) != "0") {
                    std::fprintf(stderr,
                                 "[phoneMEJIT] osr-deopt %s.%s%s bci=%u "
                                 "status=%u bytecodes=%u count=%u\n",
                                 owner.name().c_str(),
                                 method.name.c_str(),
                                 method.descriptor.c_str(),
                                 static_cast<unsigned>(frame.bytecode_pc),
                                 static_cast<unsigned>(runtime_status),
                                 static_cast<unsigned>(executed),
                                 static_cast<unsigned>(retryable_call
                                     ? entry.retryable_call_count
                                     : entry.deopt_count));
                }
                if (retire_entry) {
                    if (const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
                        trace_value != nullptr && *trace_value != '\0' &&
                        std::string_view(trace_value) != "0") {
                        std::fprintf(stderr,
                                     "[phoneMEJIT] osr-retire %s.%s%s after=%u deopts\n",
                                     owner.name().c_str(),
                                     method.name.c_str(),
                                     method.descriptor.c_str(),
                                     static_cast<unsigned>(entry.deopt_count));
                    }
                    // Machine deliberately retries OSR quickly after a precise
                    // resume. Without retirement, a loop whose OSR entry always
                    // hits the same unsupported/unbounded call can bounce back
                    // into native code every few backedges for the lifetime of
                    // the loop. Long trajectory loops amplify that into an
                    // apparent hard freeze on iOS.
                    release_compiled_entry(entry);
                    entry.rejected = true;
                    ++stats_.rejected_methods;
                }
                ++stats_.osr_fallbacks;
                if (deopt_state.has_value()) {
                    return std::optional<JitExecutionResult>(JitExecutionResult {
                        .return_value = std::nullopt,
                        .exception = std::nullopt,
                        .exception_bci = 0U,
                        .bytecode_instructions = executed,
                        .deopt_state = std::move(deopt_state),
                    });
                }
                return std::optional<JitExecutionResult>{};
            }

            std::optional<JitExceptionKind> exception;
            switch (runtime_status) {
            case JitRuntimeStatus::fatal_runtime_error:
                return fail(
                    ErrorCode::internal_error,
                    "JIT OSR runtime helper failed after nested execution began");
            case JitRuntimeStatus::null_pointer:
                exception = JitExceptionKind::null_pointer;
                break;
            case JitRuntimeStatus::array_index_out_of_bounds:
                exception = JitExceptionKind::array_index_out_of_bounds;
                break;
            case JitRuntimeStatus::array_store:
                exception = JitExceptionKind::array_store;
                break;
            case JitRuntimeStatus::class_cast:
                exception = JitExceptionKind::class_cast;
                break;
            case JitRuntimeStatus::java_throwable:
                exception = JitExceptionKind::runtime_throwable;
                break;
            case JitRuntimeStatus::budget_exhausted:
                ++stats_.budget_fallbacks;
                return fail(ErrorCode::invalid_state,
                            "JIT OSR nested invocation exhausted budget");
            case JitRuntimeStatus::success:
            case JitRuntimeStatus::deoptimize:
            case JitRuntimeStatus::unsupported_call:
            case JitRuntimeStatus::retryable_call:
                break;
            }
            entry.deopt_count = 0U;
            entry.retryable_call_count = 0U;
            ++stats_.executed_methods;
            ++stats_.osr_executions;
            return std::optional<JitExecutionResult>(JitExecutionResult {
                .return_value = std::nullopt,
                .exception = exception,
                .exception_bci = bytecode_pc,
                .bytecode_instructions = executed,
            });
        }

        std::optional<Value> return_value;
        if (entry.compiled->returns_value) {
            if (integer_like(entry.compiled->return_kind)) {
                return_value = Value::from_int(
                    static_cast<i32>(static_cast<u32>(result_bits)));
            } else if (entry.compiled->return_kind == JavaTypeKind::long_integer) {
                return_value = Value::from_long(static_cast<i64>(result_bits));
            } else if (entry.compiled->return_kind == JavaTypeKind::float32) {
                return_value = Value::from_float(std::bit_cast<float>(
                    static_cast<u32>(result_bits)));
            } else if (entry.compiled->return_kind == JavaTypeKind::float64) {
                return_value = Value::from_double(std::bit_cast<double>(result_bits));
            } else if (entry.compiled->return_kind == JavaTypeKind::reference ||
                       entry.compiled->return_kind == JavaTypeKind::array) {
                return_value = Value::from_reference(ObjectRef {result_bits});
            } else {
                return fail(ErrorCode::invalid_state,
                            "JIT OSR returned an unsupported value kind");
            }
        }
        entry.deopt_count = 0U;
        entry.retryable_call_count = 0U;
        ++stats_.executed_methods;
        ++stats_.osr_executions;
        if (const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
            trace_value != nullptr && *trace_value != '\0' &&
            std::string_view(trace_value) != "0") {
            std::fprintf(stderr,
                         "[phoneMEJIT] osr %s.%s%s bci=%u bytecodes=%u\n",
                         owner.name().c_str(),
                         method.name.c_str(),
                         method.descriptor.c_str(),
                         static_cast<unsigned>(frame.bytecode_pc),
                         static_cast<unsigned>(executed));
        }
        return std::optional<JitExecutionResult>(JitExecutionResult {
            .return_value = return_value,
            .exception = std::nullopt,
            .exception_bci = 0U,
            .bytecode_instructions = executed,
        });
#endif
    }

    [[nodiscard]] Result<std::optional<JitExecutionResult>> try_execute_cached(
        MethodId method_id,
        const classfile::ClassFile& owner,
        const classfile::Method& method,
        const CachedMethodDescriptor& descriptor,
        const InvocationArguments& arguments,
        bool has_receiver,
        u64 instruction_budget,
        JitRuntimeHooks runtime_hooks) {
        ++stats_.fast_chain_attempts;
#if !defined(__aarch64__)
        (void)method_id;
        (void)owner;
        (void)method;
        (void)descriptor;
        (void)arguments;
        (void)has_receiver;
        (void)instruction_budget;
        (void)runtime_hooks;
        ++stats_.fast_chain_fallbacks;
        return std::optional<JitExecutionResult>{};
#else
        if (!enabled_ || availability_ != JitAvailability::ready ||
            !method_id.valid()) {
            ++stats_.fast_chain_fallbacks;
            return std::optional<JitExecutionResult>{};
        }
        const auto found = entries_.find(method_id);
        if (found == entries_.end() ||
            found->second.source_owner != &owner ||
            found->second.source_method != &method ||
            !found->second.compiled.has_value() ||
            (found->second.compiled->requires_runtime_dispatch &&
             runtime_hooks.dispatch == nullptr)) {
            ++stats_.fast_chain_fallbacks;
            return std::optional<JitExecutionResult>{};
        }
        const bool runtime_dispatch =
            found->second.compiled->requires_runtime_dispatch;
        auto result = try_execute(method_id,
                                  owner,
                                  method,
                                  descriptor,
                                  arguments,
                                  has_receiver,
                                  instruction_budget,
                                  runtime_hooks,
                                  {},
                                  {},
                                  {});
        if (!result) return std::unexpected(result.error());
        if (!result->has_value()) {
            ++stats_.fast_chain_fallbacks;
            return std::optional<JitExecutionResult>{};
        }
        ++stats_.fast_chain_hits;
        if (runtime_dispatch) ++stats_.fast_chain_runtime_hits;
        if ((*result)->deopt_state.has_value()) {
            ++stats_.fast_chain_deopt_resumes;
        }
        return result;
#endif
    }

    [[nodiscard]] static JitAvailability probe_platform() noexcept {
#if !defined(__aarch64__)
        return JitAvailability::unavailable;
#else
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
        // Never probe iOS JIT permission by executing a generated instruction.
        // On A12+ the kernel may terminate a non-debugged process at the first
        // unsigned instruction even when mmap/mprotect appeared to succeed.
        // The app bridge follows UTM and checks csflags/ptrace state instead.
        using PlatformStatusFunction = i32 (*)(void);
        static const auto platform_status =
            reinterpret_cast<PlatformStatusFunction>(
                ::dlsym(RTLD_DEFAULT, "phoneme_platform_jit_status"));
        return platform_status != nullptr && platform_status() != 0
            ? JitAvailability::ready
            : JitAvailability::unavailable;
#else
        static std::atomic<i32> cached {-1};
        const i32 known = cached.load(std::memory_order_acquire);
        if (known >= 0) return static_cast<JitAvailability>(known);

        const std::vector<u32> self_test_code {
            0x52800549U, // mov w9, #42
            0xF9000089U, // str x9, [x4]
            0x52800000U, // mov w0, #0
            0xD65F03C0U, // ret
        };
        auto self_test = finalize_code(self_test_code);
        u64 self_test_value = 0U;
        if (self_test.has_value() &&
            self_test->function != nullptr &&
            self_test->function(
                nullptr, nullptr, nullptr, 1U, &self_test_value) == 0U &&
            self_test_value == 42U) {
            cached.store(
                static_cast<i32>(JitAvailability::ready),
                std::memory_order_release);
            return JitAvailability::ready;
        }
        return JitAvailability::unavailable;
#endif
#endif
    }

private:
    void record_reject(JitRejectReason reason,
                       std::optional<u8> opcode,
                       const classfile::ClassFile& owner,
                       const classfile::Method& method) noexcept {
        const usize index = static_cast<usize>(reason);
        if (index < stats_.reject_reasons.size()) {
            ++stats_.reject_reasons[index];
        }
        if (opcode.has_value()) {
            ++stats_.unsupported_opcodes[*opcode];
        }
        const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
        if (trace_value != nullptr && *trace_value != '\0' &&
            std::string_view(trace_value) != "0") {
            if (opcode.has_value()) {
                std::fprintf(stderr,
                             "[phoneMEJIT] reject %s.%s%s reason=%.*s "
                             "opcode=0x%02x\n",
                             owner.name().c_str(),
                             method.name.c_str(),
                             method.descriptor.c_str(),
                             static_cast<int>(jit_reject_reason_name(reason).size()),
                             jit_reject_reason_name(reason).data(),
                             static_cast<unsigned>(*opcode));
            } else {
                std::fprintf(stderr,
                             "[phoneMEJIT] reject %s.%s%s reason=%.*s\n",
                             owner.name().c_str(),
                             method.name.c_str(),
                             method.descriptor.c_str(),
                             static_cast<int>(jit_reject_reason_name(reason).size()),
                             jit_reject_reason_name(reason).data());
            }
        }
    }

#if defined(__aarch64__)
    [[nodiscard]] static std::string profile_key(
        const classfile::ClassFile& owner,
        const classfile::Method& method) {
        std::string key;
        key.reserve(owner.name().size() + method.name.size() +
                    method.descriptor.size() + 2U);
        key.append(owner.name());
        key.push_back('\t');
        key.append(method.name);
        key.push_back('\t');
        key.append(method.descriptor);
        return key;
    }

    struct Entry final {
        const classfile::ClassFile* source_owner {nullptr};
        const classfile::Method* source_method {nullptr};
        u32 observed_calls {0};
        u32 compilation_threshold {0};
        u32 deopt_count {0};
        u32 retryable_call_count {0};
        u32 osr_pending_polls {0};
        bool rejected {false};
        bool loop_candidate {false};
        bool loop_analyzed {false};
        bool startup_loop_analysis_deferred {false};
        bool startup_compile_deferred {false};
        bool compile_pending {false};
        bool profile_checked {false};
        bool profile_hot {false};
        u64 last_used_tick {0};
        std::optional<CompiledMethod> compiled;
    };

    void release_compiled_entry(Entry& entry) noexcept {
        if (!entry.compiled.has_value()) return;
        const u64 released = entry.compiled->block.mapped_size;
        code_cache_bytes_ = released > code_cache_bytes_
            ? 0U
            : code_cache_bytes_ - released;
        stats_.code_cache_bytes = code_cache_bytes_;
        entry.compiled.reset();
    }

    [[nodiscard]] bool evict_for(u64 needed, MethodId protected_method) {
        while (code_cache_bytes_ + needed > code_cache_limit_bytes_) {
            auto candidate = entries_.end();
            for (auto current = entries_.begin(); current != entries_.end();
                 ++current) {
                if (current->first == protected_method ||
                    !current->second.compiled.has_value()) {
                    continue;
                }
                if (candidate == entries_.end() ||
                    current->second.last_used_tick <
                        candidate->second.last_used_tick) {
                    candidate = current;
                }
            }
            if (candidate == entries_.end()) return false;
            release_compiled_entry(candidate->second);
            candidate->second.observed_calls = 0U;
            ++stats_.evicted_methods;
        }
        return true;
    }

    void update_executable_arena_statistics() noexcept {
        stats_.executable_mapped_bytes =
            executable_arena_.mapped_bytes() +
            background_executable_arena_.mapped_bytes();
        stats_.executable_slab_count =
            executable_arena_.slab_count() +
            background_executable_arena_.slab_count();
    }

    [[nodiscard]] bool start_background_compiler() {
        if (!background_compile_enabled_ || !background_workers_.empty()) {
            return !background_workers_.empty();
        }
        {
            std::scoped_lock lock(background_mutex_);
            background_stop_ = false;
        }
        background_workers_.reserve(background_compile_worker_limit_);
        for (u32 index = 0U; index < background_compile_worker_limit_; ++index) {
            background_workers_.emplace_back([this, index] {
                background_compile_loop(index);
            });
        }
        background_compile_worker_count_.store(
            static_cast<u64>(background_workers_.size()),
            std::memory_order_release);
        return !background_workers_.empty();
    }

    void stop_background_compiler() noexcept {
        {
            std::scoped_lock lock(background_mutex_);
            background_stop_ = true;
            background_tasks_.clear();
        }
        background_condition_.notify_all();
        for (std::thread& worker : background_workers_) {
            if (worker.joinable()) worker.join();
        }
        background_workers_.clear();
        background_compile_worker_count_.store(0U, std::memory_order_release);
        std::scoped_lock lock(background_mutex_);
        background_tasks_.clear();
        background_results_.clear();
        background_stop_ = false;
    }

    void background_compile_loop(u32 worker_index) noexcept {
        (void)worker_index;
        runtime::WorkCoordinator& work_coordinator =
            runtime::shared_work_coordinator();
        for (;;) {
            BackgroundCompileTask task;
            {
                std::unique_lock lock(background_mutex_);
                background_condition_.wait(lock, [this] {
                    return background_stop_ || !background_tasks_.empty();
                });
                if (background_stop_ && background_tasks_.empty()) return;
                while (!background_stop_ && !background_tasks_.empty()) {
                    const u64 last_frame =
                        last_frame_publication_nanoseconds_.load(
                            std::memory_order_acquire);
                    if (last_frame == 0U ||
                        render_compile_cooldown_nanoseconds_ == 0U) {
                        break;
                    }
                    const u64 now = static_cast<u64>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
                    if (now >= last_frame + render_compile_cooldown_nanoseconds_) {
                        break;
                    }
                    const u64 remaining =
                        last_frame + render_compile_cooldown_nanoseconds_ - now;
                    background_compile_render_cooldown_waits_.fetch_add(
                        1U, std::memory_order_relaxed);
                    background_compile_render_cooldown_nanoseconds_.fetch_add(
                        remaining, std::memory_order_relaxed);
                    background_condition_.wait_for(
                        lock, std::chrono::nanoseconds(remaining));
                }
                if (background_stop_ && background_tasks_.empty()) return;
                if (background_tasks_.empty()) continue;

                // Rendering/gameplay has priority over speculative JIT work.
                // The coordinator also serializes native background compute so
                // a compiler burst cannot overlap another CPU-heavy subsystem.
                while (!background_stop_ && !background_tasks_.empty() &&
                       !work_coordinator.try_begin_background_work()) {
                    background_condition_.wait_for(
                        lock,
                        std::chrono::milliseconds(32),
                        [this, &work_coordinator] {
                            return background_stop_ ||
                                   background_tasks_.empty() ||
                                   (work_coordinator.background_work_allowed() &&
                                    work_coordinator.active_background_jobs() ==
                                        0U);
                        });
                }
                if (background_stop_ && background_tasks_.empty()) return;
                if (background_tasks_.empty()) continue;
                task = std::move(background_tasks_.front());
                background_tasks_.pop_front();
            }

            const auto started = std::chrono::steady_clock::now();
            CompileAttempt attempt;
            // ClassFile is immutable and task.owner pins the exact object that
            // owns source_method, so the method pointer remains stable for the
            // entire background compile. Avoid copying two strings into every
            // task and re-hashing them on the worker thread.
            const classfile::Method* snapshot_method =
                task.owner != nullptr && task.owner.get() == task.source_owner
                    ? task.source_method
                    : nullptr;
            if (snapshot_method == nullptr) {
                attempt = reject_attempt(JitRejectReason::decode_failure);
            } else {
                attempt = compile_scalar_method(*task.owner,
                                                *snapshot_method,
                                                *task.descriptor,
                                                task.has_receiver,
                                                background_executable_arena_,
                                                inline_resolver_,
                                                task.verified_frames.get());
            }
            const u64 elapsed = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started).count());
            work_coordinator.end_background_work();
            {
                std::scoped_lock lock(background_mutex_);
                if (background_stop_) continue;
                background_results_.push_back(BackgroundCompileResult {
                    .method_id = task.method_id,
                    .owner = std::move(task.owner),
                    .source_owner = task.source_owner,
                    .source_method = task.source_method,
                    .startup = task.startup,
                    .compile_time_nanoseconds = elapsed,
                    .attempt = std::move(attempt),
                });
            }
        }
    }

    [[nodiscard]] bool enqueue_background_compile(
        MethodId method_id,
        const classfile::ClassFile& owner,
        const classfile::Method& method,
        const CachedMethodDescriptor& descriptor,
        bool has_receiver,
        bool startup,
        const std::shared_ptr<const classfile::ClassFile>& owner_lifetime,
        std::shared_ptr<const VerifiedMethodReferenceMaps> verified_frames,
        std::shared_ptr<const CachedMethodDescriptor> descriptor_lifetime) {
        if (!background_compile_enabled_ || owner_lifetime == nullptr ||
            owner_lifetime.get() != &owner || !start_background_compiler()) {
            return false;
        }
        {
            std::scoped_lock lock(background_mutex_);
            if (background_tasks_.size() >= kMaximumBackgroundCompileQueue) {
                return false;
            }
            if (descriptor_lifetime == nullptr ||
                descriptor_lifetime.get() != &descriptor) {
                descriptor_lifetime =
                    std::make_shared<const CachedMethodDescriptor>(descriptor);
            }
            background_tasks_.push_back(BackgroundCompileTask {
                .method_id = method_id,
                .owner = owner_lifetime,
                .descriptor = std::move(descriptor_lifetime),
                .verified_frames = std::move(verified_frames),
                .source_owner = &owner,
                .source_method = &method,
                .has_receiver = has_receiver,
                .startup = startup,
            });
            const u64 depth = static_cast<u64>(background_tasks_.size());
            u64 previous = background_compile_queue_peak_.load(
                std::memory_order_relaxed);
            while (depth > previous &&
                   !background_compile_queue_peak_.compare_exchange_weak(
                       previous,
                       depth,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
        }
        background_condition_.notify_one();
        ++stats_.background_compile_queued;
        const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
        if (trace_value != nullptr && *trace_value != '\0' &&
            std::string_view(trace_value) != "0") {
            std::fprintf(stderr,
                         "[phoneMEJIT] queue %s.%s%s background=1\n",
                         owner.name().c_str(),
                         method.name.c_str(),
                         method.descriptor.c_str());
        }
        return true;
    }

    void publish_background_results() {
        std::deque<BackgroundCompileResult> ready;
        {
            std::scoped_lock lock(background_mutex_);
            ready.swap(background_results_);
        }
        for (BackgroundCompileResult& completed : ready) {
            ++stats_.background_compile_completed;
            stats_.background_compile_time_nanoseconds +=
                completed.compile_time_nanoseconds;
            stats_.compile_time_nanoseconds +=
                completed.compile_time_nanoseconds;

            auto entry_iterator = entries_.find(completed.method_id);
            if (entry_iterator == entries_.end() ||
                entry_iterator->second.source_owner != completed.source_owner ||
                entry_iterator->second.source_method != completed.source_method ||
                !entry_iterator->second.compile_pending) {
                ++stats_.background_compile_discarded;
                continue;
            }
            Entry& entry = entry_iterator->second;
            entry.compile_pending = false;
            const classfile::Method* snapshot_method =
                completed.owner != nullptr &&
                        completed.owner.get() == completed.source_owner
                    ? completed.source_method
                    : nullptr;
            if (snapshot_method == nullptr) {
                ++stats_.background_compile_discarded;
                continue;
            }

            if (completed.attempt.disposition ==
                    CompileDisposition::executable_memory_unavailable) {
                // MAP_JIT-only background allocation may fail even though the
                // foreground W^X path remains valid. Disable async compilation
                // without poisoning JIT availability or the method itself.
                background_compile_enabled_ = false;
                entry.startup_compile_deferred = false;
                ++stats_.background_compile_discarded;
                continue;
            }

            ++stats_.compile_attempts;
            if (completed.startup) ++stats_.startup_compile_attempts;
            if (completed.attempt.disposition != CompileDisposition::success ||
                !completed.attempt.method.has_value()) {
                entry.rejected = true;
                ++stats_.rejected_methods;
                ++stats_.unsupported_fallbacks;
                record_reject(completed.attempt.reject_reason,
                              completed.attempt.unsupported_opcode,
                              *completed.owner,
                              *snapshot_method);
                ++stats_.background_compile_published;
                continue;
            }

            const u64 needed = completed.attempt.method->block.mapped_size;
            if (needed > code_cache_limit_bytes_ ||
                !evict_for(needed, completed.method_id)) {
                entry.rejected = true;
                ++stats_.rejected_methods;
                ++stats_.unsupported_fallbacks;
                record_reject(JitRejectReason::code_cache_limit,
                              std::nullopt,
                              *completed.owner,
                              *snapshot_method);
                ++stats_.background_compile_published;
                continue;
            }

            code_cache_bytes_ += needed;
            stats_.code_cache_bytes = code_cache_bytes_;
            entry.compiled = std::move(*completed.attempt.method);
            entry.deopt_count = 0U;
            entry.retryable_call_count = 0U;
            entry.last_used_tick = ++use_tick_;
            ++stats_.compiled_methods;
            if (entry.compiled->quick_tier) ++stats_.quick_compiled_methods;
            ++stats_.background_compile_published;
            stats_.osr_compiled_entries += entry.compiled->osr_entries.size();
            if (completed.startup) ++stats_.startup_compiled_methods;
            if (entry.compiled->optimized) ++stats_.optimized_methods;
            if (entry.compiled->contains_loop) ++stats_.loop_optimized_methods;
            stats_.propagated_constants += entry.compiled->propagated_constants;
            stats_.folded_operations += entry.compiled->folded_operations;
            stats_.folded_branches += entry.compiled->folded_branches;
            stats_.strength_reductions += entry.compiled->strength_reductions;
            stats_.dead_local_writes_eliminated +=
                entry.compiled->dead_local_writes_eliminated;
            stats_.common_subexpressions_eliminated +=
                entry.compiled->common_subexpressions_eliminated;
            stats_.cross_block_common_subexpressions_eliminated +=
                entry.compiled->cross_block_common_subexpressions_eliminated;
            stats_.loop_invariants_hoisted +=
                entry.compiled->loop_invariants_hoisted;
            stats_.inlined_calls += entry.compiled->inlined_calls;
            stats_.inlined_bytecodes += entry.compiled->inlined_bytecodes;
            stats_.devirtualized_calls += entry.compiled->devirtualized_calls;
            stats_.array_bounds_checks_eliminated +=
                entry.compiled->array_bounds_checks_eliminated;
            stats_.array_runtime_calls_eliminated +=
                entry.compiled->array_runtime_calls_eliminated;
            stats_.scalar_replaced_allocations +=
                entry.compiled->scalar_replaced_allocations;
            stats_.scalar_replaced_array_accesses +=
                entry.compiled->scalar_replaced_array_accesses;
            stats_.unrolled_int_array_loops +=
                entry.compiled->unrolled_int_array_loops;
            stats_.budget_checks_elided += entry.compiled->budget_checks_elided;
            if (entry.compiled->register_cached) {
                ++stats_.register_cached_methods;
            }
            stats_.register_cached_loads +=
                entry.compiled->register_cached_loads;
            stats_.register_cached_stores_elided +=
                entry.compiled->register_cached_stores_elided;
            if (entry.compiled->stack_cached) {
                ++stats_.stack_cached_methods;
            }
            stats_.stack_cached_pops += entry.compiled->stack_cached_pops;
            stats_.stack_cached_stores_elided +=
                entry.compiled->stack_cached_stores_elided;
            update_executable_arena_statistics();

            const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
            if (trace_value != nullptr && *trace_value != '\0' &&
                std::string_view(trace_value) != "0") {
                std::fprintf(stderr,
                             "[phoneMEJIT] bg-compile %s.%s%s code_bytes=%llu\n",
                             completed.owner->name().c_str(),
                             snapshot_method->name.c_str(),
                             snapshot_method->descriptor.c_str(),
                             static_cast<unsigned long long>(needed));
            }
        }
    }

    ExecutableArena executable_arena_;
    ExecutableArena background_executable_arena_ {true};
    std::mutex background_mutex_;
    std::condition_variable background_condition_;
    std::deque<BackgroundCompileTask> background_tasks_;
    std::deque<BackgroundCompileResult> background_results_;
    std::vector<std::thread> background_workers_;
    bool background_stop_ {false};
    std::unordered_map<MethodId, Entry, MetadataIdHash<MethodId>> entries_;
    std::string profile_path_;
    std::unordered_set<std::string> hot_profile_;
#endif
    bool enabled_ {true};
    std::atomic<bool> startup_mode_ {false};
    std::atomic<u64> last_frame_publication_nanoseconds_ {0U};
    const u64 render_compile_cooldown_nanoseconds_ {
        configured_render_compile_cooldown_nanoseconds()};
    const u64 device_foreground_compile_interval_nanoseconds_ {
        configured_device_foreground_compile_interval_nanoseconds()};
    std::atomic<u64> next_foreground_compile_nanoseconds_ {0U};
    std::atomic<u64> background_compile_queue_peak_ {0U};
    std::atomic<u64> background_compile_worker_count_ {0U};
    std::atomic<u64> background_compile_render_cooldown_waits_ {0U};
    std::atomic<u64> background_compile_render_cooldown_nanoseconds_ {0U};
    JitAvailability availability_ {JitAvailability::unavailable};
    JitInlineResolverHooks inline_resolver_ {};
    u32 hot_threshold_ {kDefaultHotThreshold};
    u32 startup_hot_threshold_ {kDefaultStartupHotThreshold};
    u32 startup_loop_bytecode_threshold_ {
        kDefaultStartupLoopBytecodeThreshold};
    u32 startup_compile_limit_ {kDefaultStartupCompileLimit};
    u64 startup_compile_budget_nanoseconds_ {
        kDefaultStartupCompileBudgetNanoseconds};
    bool background_compile_enabled_ {false};
    const u32 background_compile_worker_limit_ {1U};
    std::atomic<u32> startup_compile_attempts_ {0U};
    std::atomic<u64> startup_compile_time_nanoseconds_ {0U};
    u32 unavailable_probe_countdown_ {0U};
    u64 code_cache_limit_bytes_ {kDefaultCodeCacheBytes};
    u64 code_cache_bytes_ {0U};
#if defined(__aarch64__)
    u64 use_tick_ {0U};
#endif
    JitStatistics stats_;
};

BaselineJit::BaselineJit(bool enabled)
    : impl_(std::make_unique<Impl>(enabled)) {}

BaselineJit::~BaselineJit() = default;

void BaselineJit::set_enabled(bool enabled) noexcept {
    impl_->set_enabled(enabled);
}

void BaselineJit::set_startup_mode(bool enabled) noexcept {
    impl_->set_startup_mode(enabled);
}

void BaselineJit::note_frame_published() noexcept {
    impl_->note_frame_published();
}

void BaselineJit::set_inline_resolver(JitInlineResolverHooks hooks) noexcept {
    impl_->set_inline_resolver(hooks);
}

void BaselineJit::refresh_availability() noexcept {
    impl_->refresh_availability();
}

void BaselineJit::clear_cache() noexcept { impl_->clear_cache(); }

Status BaselineJit::configure_profile(std::string path) {
    return impl_->configure_profile(std::move(path));
}

Status BaselineJit::flush_profile() { return impl_->flush_profile(); }

bool BaselineJit::enabled() const noexcept { return impl_->enabled(); }

bool BaselineJit::startup_mode() const noexcept {
    return impl_->startup_mode();
}

JitAvailability BaselineJit::availability() const noexcept {
    return impl_->availability();
}

JitStatistics BaselineJit::statistics() const noexcept {
    return impl_->statistics();
}

Result<std::optional<JitExecutionResult>> BaselineJit::try_execute(
    MethodId method_id,
    const classfile::ClassFile& owner,
    const classfile::Method& method,
    const CachedMethodDescriptor& descriptor,
    std::span<const Value> arguments,
    bool has_receiver,
    u64 instruction_budget,
    JitRuntimeHooks runtime_hooks,
    std::shared_ptr<const classfile::ClassFile> owner_lifetime,
    std::shared_ptr<const VerifiedMethodReferenceMaps> verified_frames,
    std::shared_ptr<const CachedMethodDescriptor> descriptor_lifetime) {
    InvocationArguments compact_arguments(arguments);
    return try_execute(method_id,
                       owner,
                       method,
                       descriptor,
                       compact_arguments,
                       has_receiver,
                       instruction_budget,
                       runtime_hooks,
                       std::move(owner_lifetime),
                       std::move(verified_frames),
                       std::move(descriptor_lifetime));
}

Result<std::optional<JitExecutionResult>> BaselineJit::try_execute(
    MethodId method_id,
    const classfile::ClassFile& owner,
    const classfile::Method& method,
    const CachedMethodDescriptor& descriptor,
    const InvocationArguments& arguments,
    bool has_receiver,
    u64 instruction_budget,
    JitRuntimeHooks runtime_hooks,
    std::shared_ptr<const classfile::ClassFile> owner_lifetime,
    std::shared_ptr<const VerifiedMethodReferenceMaps> verified_frames,
    std::shared_ptr<const CachedMethodDescriptor> descriptor_lifetime) {
    return impl_->try_execute(method_id,
                              owner,
                              method,
                              descriptor,
                              arguments,
                              has_receiver,
                              instruction_budget,
                              runtime_hooks,
                              std::move(owner_lifetime),
                              std::move(verified_frames),
                              std::move(descriptor_lifetime));
}

Result<std::optional<JitExecutionResult>> BaselineJit::try_execute_osr(
    MethodId method_id,
    const classfile::ClassFile& owner,
    const classfile::Method& method,
    const CachedMethodDescriptor& descriptor,
    bool has_receiver,
    JitPhysicalFrameView frame,
    u64 instruction_budget,
    JitRuntimeHooks runtime_hooks,
    std::shared_ptr<const classfile::ClassFile> owner_lifetime,
    std::shared_ptr<const VerifiedMethodReferenceMaps> verified_frames,
    std::shared_ptr<const CachedMethodDescriptor> descriptor_lifetime) {
    return impl_->try_execute_osr(method_id,
                                  owner,
                                  method,
                                  descriptor,
                                  has_receiver,
                                  frame,
                                  instruction_budget,
                                  runtime_hooks,
                                  std::move(owner_lifetime),
                                  std::move(verified_frames),
                                  std::move(descriptor_lifetime));
}

Result<std::optional<JitExecutionResult>> BaselineJit::try_execute_cached(
    MethodId method_id,
    const classfile::ClassFile& owner,
    const classfile::Method& method,
    const CachedMethodDescriptor& descriptor,
    std::span<const Value> arguments,
    bool has_receiver,
    u64 instruction_budget,
    JitRuntimeHooks runtime_hooks) {
    InvocationArguments compact_arguments(arguments);
    return try_execute_cached(method_id,
                              owner,
                              method,
                              descriptor,
                              compact_arguments,
                              has_receiver,
                              instruction_budget,
                              runtime_hooks);
}

Result<std::optional<JitExecutionResult>> BaselineJit::try_execute_cached(
    MethodId method_id,
    const classfile::ClassFile& owner,
    const classfile::Method& method,
    const CachedMethodDescriptor& descriptor,
    const InvocationArguments& arguments,
    bool has_receiver,
    u64 instruction_budget,
    JitRuntimeHooks runtime_hooks) {
    return impl_->try_execute_cached(method_id,
                                     owner,
                                     method,
                                     descriptor,
                                     arguments,
                                     has_receiver,
                                     instruction_budget,
                                     runtime_hooks);
}

JitAvailability BaselineJit::probe_platform() noexcept {
    return Impl::probe_platform();
}

bool BaselineJit::conservative_device_mode() noexcept {
    return conservative_device_jit_mode();
}

} // namespace phoneme::vm
