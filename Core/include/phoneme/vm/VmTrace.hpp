#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

namespace phoneme::vm {

inline bool vm_trace_enabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("PHONEME_TRACE_VM");
        return value != nullptr && value[0] != '\0' &&
               std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

inline bool jit_trace_enabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("PHONEME_JIT_TRACE");
        return value != nullptr && value[0] != '\0' &&
               std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

inline unsigned long long vm_trace_elapsed_microseconds() noexcept {
    static const auto origin = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::steady_clock::now() - origin;
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
}

inline unsigned long long vm_trace_host_thread() noexcept {
    return static_cast<unsigned long long>(
        std::hash<std::thread::id> {}(std::this_thread::get_id()));
}

inline std::mutex& vm_trace_output_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

template <typename... Arguments>
inline void vm_trace(const char* category,
                     const char* format,
                     Arguments... arguments) noexcept {
    if (!vm_trace_enabled()) return;
    std::scoped_lock lock(vm_trace_output_mutex());
    std::fprintf(stderr,
                 "[phoneMETrace][%s][%llu us][host=%llx] ",
                 category,
                 vm_trace_elapsed_microseconds(),
                 vm_trace_host_thread());
    std::fprintf(stderr, format, arguments...);
    std::fputc('\n', stderr);
}

} // namespace phoneme::vm
