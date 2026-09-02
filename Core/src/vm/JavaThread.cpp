#include "phoneme/vm/JavaThread.hpp"
#include "phoneme/vm/JavaFiber.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <new>
#include <system_error>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <pthread/qos.h>
#endif

namespace phoneme::vm {

#if defined(__APPLE__)
struct JavaWorkerThread::StartContext final {
    std::function<void(std::stop_token)> task;
    std::stop_token stop_token;
};
#endif

JavaWorkerThread::~JavaWorkerThread() {
    request_stop();
    if (!joinable()) return;
    if (is_current_thread()) {
        detach();
    } else {
        join();
    }
}

Status JavaWorkerThread::start(
    std::function<void(std::stop_token)> task) {
    if (!task) {
        return fail(ErrorCode::invalid_argument,
                    "Java worker thread requires a task");
    }
    if (joinable()) {
        return fail(ErrorCode::already_running,
                    "Java worker thread is already running");
    }

#if defined(__APPLE__)
    stop_source_ = std::stop_source {};
    auto context = std::unique_ptr<StartContext>(
        new (std::nothrow) StartContext {
            .task = std::move(task),
            .stop_token = stop_source_.get_token(),
        });
    if (!context) {
        return fail(ErrorCode::internal_error,
                    "could not allocate Java worker thread context");
    }

    pthread_attr_t attributes;
    int status = pthread_attr_init(&attributes);
    if (status != 0) {
        return fail(ErrorCode::internal_error,
                    "pthread_attr_init failed: " +
                        std::string(std::strerror(status)));
    }
    // Java frames live in Machine/ExecutionContext rather than recursively on
    // the pthread C stack. 8 MiB per guest Thread was therefore mostly dead
    // address-space/stack commitment and made thread-heavy MIDlets needlessly
    // expensive on iOS. Keep a generous native stack for JNI/HLE recursion
    // while staying much closer to a mobile JVM worker footprint.
    constexpr usize kJavaWorkerStackBytes = 2U * 1024U * 1024U;
    status = pthread_attr_setstacksize(
        &attributes,
        std::max<usize>(kJavaWorkerStackBytes,
                        static_cast<usize>(PTHREAD_STACK_MIN)));
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
    if (status == 0) {
        // Do not inherit a user-interactive/user-initiated QoS from the launch
        // path, but also do not demote the foreground game to Utility. Utility
        // is aggressively deprioritized under load/thermal pressure and makes
        // the serialized VM execution gate visible as input/render latency.
        // QOS_CLASS_DEFAULT lets iOS choose P/E cores while frame pacing and
        // cooperative backoff control package power explicitly.
        const int qos_status = pthread_attr_set_qos_class_np(
            &attributes, QOS_CLASS_DEFAULT, 0);
        // QoS is a thermal hint, not a correctness requirement. Older runtimes
        // may reject the non-portable attribute, in which case keep creating
        // the worker with the normal scheduler defaults.
        (void)qos_status;
    }
#endif
    if (status == 0) {
        status = pthread_create(
            &thread_, &attributes, &JavaWorkerThread::thread_entry,
            context.get());
    }
    pthread_attr_destroy(&attributes);
    if (status != 0) {
        return fail(ErrorCode::internal_error,
                    "could not start Java worker thread: " +
                        std::string(std::strerror(status)));
    }
    context.release();
    joinable_ = true;
    return {};
#else
#if defined(__cpp_exceptions)
    try {
        thread_ = std::jthread(
            [task = std::move(task)](std::stop_token stop_token) mutable {
                task(stop_token);
            });
    } catch (const std::system_error& error) {
        return fail(ErrorCode::internal_error,
                    "could not start Java worker thread: " +
                        std::string(error.what()));
    }
#else
    thread_ = std::jthread(
        [task = std::move(task)](std::stop_token stop_token) mutable {
            task(stop_token);
        });
#endif
    return {};
#endif
}

void JavaWorkerThread::request_stop() noexcept {
#if defined(__APPLE__)
    stop_source_.request_stop();
#else
    thread_.request_stop();
#endif
}

bool JavaWorkerThread::joinable() const noexcept {
#if defined(__APPLE__)
    return joinable_;
#else
    return thread_.joinable();
#endif
}

bool JavaWorkerThread::is_current_thread() const noexcept {
    if (!joinable()) return false;
#if defined(__APPLE__)
    return pthread_equal(thread_, pthread_self()) != 0;
#else
    return thread_.get_id() == std::this_thread::get_id();
#endif
}

void JavaWorkerThread::join() {
    if (!joinable()) return;
#if defined(__APPLE__)
    const int status = pthread_join(thread_, nullptr);
    if (status == 0) joinable_ = false;
#else
    thread_.join();
#endif
}

void JavaWorkerThread::detach() {
    if (!joinable()) return;
#if defined(__APPLE__)
    const int status = pthread_detach(thread_);
    if (status == 0) joinable_ = false;
#else
    thread_.detach();
#endif
}

#if defined(__APPLE__)
void* JavaWorkerThread::thread_entry(void* raw_context) noexcept {
    std::unique_ptr<StartContext> context(
        static_cast<StartContext*>(raw_context));
    context->task(context->stop_token);
    return nullptr;
}
#endif

JavaThread::JavaThread(JavaThreadId thread_id,
                       ObjectRef thread_object,
                       ObjectRef runnable_target)
    : id_(thread_id),
      object_(thread_object),
      target_(runnable_target),
      context_(std::make_shared<ExecutionContext>(thread_id)) {}

JavaThread::~JavaThread() {
    // A very short native task can publish its terminated state before its
    // worker lambda has released the final shared_ptr capture. If another host
    // thread retires the scheduler record at exactly that point, the final
    // JavaThread reference may be released on the worker itself. std::jthread's
    // default destructor would then try to join the current thread and abort
    // with EDEADLK. The worker has finished all JavaThread access by destructor
    // entry, so detaching this already-exiting self handle is the only safe
    // ownership transition; all non-self destruction keeps jthread's normal
    // stop-and-join behavior.
    if (worker_.joinable() && worker_.is_current_thread()) {
        worker_.detach();
    }
}

JavaThreadSnapshot JavaThread::snapshot() const noexcept {
    std::scoped_lock lock(mutex_);
    return JavaThreadSnapshot {
        .id = id_,
        .object = object_,
        .target = target_,
        .state = state_,
        .priority = priority_,
        .interrupted = interrupted_,
        .alive = started_ && state_ != JavaThreadState::terminated,
    };
}

} // namespace phoneme::vm
