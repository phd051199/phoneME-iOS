#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#if defined(__APPLE__)
#include <pthread.h>
#endif

#include "phoneme/base/Error.hpp"
#include "phoneme/vm/ExecutionContext.hpp"

namespace phoneme::vm {

class JavaFiber;

enum class JavaThreadState : u8 {
    new_thread,
    runnable,
    running,
    blocked_monitor,
    blocked_io,
    waiting,
    sleeping,
    joining,
    terminated,
};

class JavaWorkerThread final {
public:
    JavaWorkerThread() = default;
    ~JavaWorkerThread();

    JavaWorkerThread(const JavaWorkerThread&) = delete;
    JavaWorkerThread& operator=(const JavaWorkerThread&) = delete;

    [[nodiscard]] Status start(
        std::function<void(std::stop_token)> task);
    void request_stop() noexcept;
    [[nodiscard]] bool joinable() const noexcept;
    [[nodiscard]] bool is_current_thread() const noexcept;
    void join();
    void detach();

private:
#if defined(__APPLE__)
    struct StartContext;
    static void* thread_entry(void* context) noexcept;

    pthread_t thread_ {};
    std::stop_source stop_source_;
    bool joinable_ {false};
#else
    std::jthread thread_;
#endif
};

struct JavaThreadSnapshot final {
    JavaThreadId id {0};
    ObjectRef object {};
    ObjectRef target {};
    JavaThreadState state {JavaThreadState::new_thread};
    i32 priority {5};
    bool interrupted {false};
    bool alive {false};
};

class JavaThread final {
public:
    JavaThread(JavaThreadId thread_id,
               ObjectRef thread_object,
               ObjectRef runnable_target);
    ~JavaThread();

    JavaThread(const JavaThread&) = delete;
    JavaThread& operator=(const JavaThread&) = delete;

    [[nodiscard]] JavaThreadSnapshot snapshot() const noexcept;

private:
    friend class Scheduler;

    JavaThreadId id_ {0};
    ObjectRef object_ {};
    ObjectRef target_ {};
    std::shared_ptr<ExecutionContext> context_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    JavaWorkerThread worker_;
    std::unique_ptr<JavaFiber> fiber_;
    JavaThreadState state_ {JavaThreadState::new_thread};
    i32 priority_ {5};
    bool interrupted_ {false};
    bool started_ {false};
    bool stop_requested_ {false};
    bool native_task_ {false};
    bool fiber_backed_ {false};
    std::optional<ObjectRef> uncaught_throwable_;
    std::optional<Error> native_failure_;
};

} // namespace phoneme::vm
