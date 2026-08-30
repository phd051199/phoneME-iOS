#include "phoneme/network/AsyncNetworkAdapter.hpp"
#include "phoneme/network/HttpParser.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(PHONEME_WEB)
#include <emscripten/fetch.h>
#endif

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__)
extern "C" {
using PhoneMEHTTPSCompletion = void (*)(int32_t, void*);
__attribute__((weak)) int32_t phoneme_ios_https_execute_async(
    const char*, const char*, const char*, const uint8_t*, int32_t, int32_t,
    int32_t, int64_t, PhoneMEHTTPSCompletion, void*) {
    return -1;
}
__attribute__((weak)) int32_t phoneme_ios_https_get_status_code(int32_t) {
    return -1;
}
__attribute__((weak)) int32_t phoneme_ios_https_copy_string(
    int32_t, int32_t, char*, int32_t) {
    return -1;
}
__attribute__((weak)) int32_t phoneme_ios_https_copy_body(
    int32_t, uint8_t*, int32_t) {
    return -1;
}
__attribute__((weak)) int64_t phoneme_ios_https_get_long(int32_t, int32_t) {
    return 0;
}
__attribute__((weak)) void phoneme_ios_https_cancel(int32_t) {}
__attribute__((weak)) void phoneme_ios_https_close(int32_t) {}
__attribute__((weak)) void phoneme_ios_https_clear_session(int64_t) {}
}
#endif

namespace phoneme::network {
namespace {

constexpr usize kMaximumHttpResponseBytes = 64U * 1024U * 1024U;
constexpr usize kMaximumHttpHeaderBytes = 64U * 1024U;
constexpr usize kMaximumInterimResponses = 8U;
constexpr usize kIoChunkSize = 16U * 1024U;
constexpr i32 kCancellationPollMilliseconds = 50;
// Socket reads and accepts may legitimately block for the lifetime of an
// online MIDlet. Web runs exactly one foreground MIDlet, so keep its idle
// pthread footprint small and grow only when concurrent socket work requires
// it. Native hosts retain the larger pool used by multi-app/background modes.
#if defined(__EMSCRIPTEN__)
constexpr usize kInitialNetworkWorkerCount = 2U;
constexpr usize kMaximumNetworkWorkerCount = 32U;
constexpr usize kMaximumReservedNetworkWorkers = 4U;
#else
// Native iOS games commonly spend long stretches entirely offline. The pool
// already grows synchronously when the first task is queued, so pre-spawning
// idle workers only adds pthread stacks and scheduler bookkeeping. Start at
// zero and create exactly the workers demanded by real socket/DNS activity.
constexpr usize kInitialNetworkWorkerCount = 0U;
constexpr usize kMaximumNetworkWorkerCount = 32U;
constexpr usize kMaximumReservedNetworkWorkers = 4U;
#endif

enum class NetworkTaskClass : u8 {
    blocking_input,
    latency_sensitive,
};

std::atomic<i32> g_address_resolution_delay_for_tests {0};
#if defined(__APPLE__)
std::atomic<u64> g_next_apple_http_session_id {1U};

[[nodiscard]] i64 allocate_apple_http_session_id() noexcept {
    const u64 value = g_next_apple_http_session_id.fetch_add(
        1U, std::memory_order_relaxed);
    constexpr u64 kMaximumSessionId =
        static_cast<u64>(std::numeric_limits<i64>::max());
    if (value == 0U || value > kMaximumSessionId) return 1;
    return static_cast<i64>(value);
}
#endif

struct CancellationCheck final {
    std::function<bool()> requested;
    int wake_descriptor {-1};

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(requested);
    }

    [[nodiscard]] bool operator()() const {
        return requested && requested();
    }
};

struct ScopedDescriptor final {
    int value {-1};
    bool owned {true};

    ScopedDescriptor() = default;
    explicit ScopedDescriptor(int descriptor, bool owns_descriptor = true) noexcept
        : value(descriptor), owned(owns_descriptor) {}
    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;
    ScopedDescriptor(ScopedDescriptor&& other) noexcept
        : value(std::exchange(other.value, -1)), owned(other.owned) {
        other.owned = true;
    }
    ScopedDescriptor& operator=(ScopedDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            value = std::exchange(other.value, -1);
            owned = other.owned;
            other.owned = true;
        }
        return *this;
    }
    ~ScopedDescriptor() { reset(); }

    void reset(int descriptor = -1) noexcept {
        if (owned && value >= 0) ::close(value);
        value = descriptor;
        owned = true;
    }

    [[nodiscard]] int release() noexcept {
        owned = true;
        return std::exchange(value, -1);
    }
};

struct HandleState final {
    explicit HandleState(int socket_descriptor) noexcept
        : descriptor(socket_descriptor) {}

    int descriptor {-1};
    // Browser WebSockets do not expose TCP socket options. Keep the MIDP
    // options virtualized per connection while SOCKFS/websockify owns the
    // actual transport.
    std::array<i32, 5> socket_options {1, 0, 0, 16'384, 16'384};
    std::timed_mutex input_gate;
    std::timed_mutex output_gate;
};

struct DuplicatedHandle final {
    ScopedDescriptor descriptor;
    std::shared_ptr<HandleState> state;
};

[[nodiscard]] bool cancellation_requested(
    const CancellationCheck& cancelled) {
    return cancelled && cancelled();
}

[[nodiscard]] std::unexpected<Error> cancelled_operation() {
    return fail(ErrorCode::io_error, "network operation was cancelled");
}

class OperationDeadline final {
public:
    explicit OperationDeadline(i32 timeout_ms) {
        if (timeout_ms > 0) {
            deadline_ = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        }
    }

    [[nodiscard]] Result<i32> next_poll_timeout() const {
        if (!deadline_.has_value()) return kCancellationPollMilliseconds;
        const auto now = std::chrono::steady_clock::now();
        if (now >= *deadline_) {
            return fail(ErrorCode::io_error,
                        "network operation timed out");
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            *deadline_ - now);
        if (remaining.count() <= 0) {
            remaining = std::chrono::milliseconds(1);
        }
        return static_cast<i32>(std::min<i64>(
            remaining.count(), kCancellationPollMilliseconds));
    }

    [[nodiscard]] Result<i32> next_fd_poll_timeout() const {
        if (!deadline_.has_value()) return -1;
        const auto now = std::chrono::steady_clock::now();
        if (now >= *deadline_) {
            return fail(ErrorCode::io_error,
                        "network operation timed out");
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            *deadline_ - now);
        if (remaining.count() <= 0) {
            remaining = std::chrono::milliseconds(1);
        }
        return static_cast<i32>(std::min<i64>(
            remaining.count(), static_cast<i64>(INT32_MAX)));
    }

private:
    std::optional<std::chrono::steady_clock::time_point> deadline_;
};

[[nodiscard]] Status check_operation_active(
    const OperationDeadline& deadline,
    const CancellationCheck& cancelled) {
    if (cancellation_requested(cancelled)) return cancelled_operation();
    auto wait_interval = deadline.next_poll_timeout();
    if (!wait_interval) return std::unexpected(wait_interval.error());
    return {};
}

[[nodiscard]] Result<std::unique_lock<std::timed_mutex>> acquire_operation_gate(
    std::timed_mutex& gate,
    const OperationDeadline& deadline,
    const CancellationCheck& cancelled) {
    std::unique_lock<std::timed_mutex> lock(gate, std::defer_lock);
    while (true) {
        if (cancellation_requested(cancelled)) return cancelled_operation();
        auto wait_interval = deadline.next_poll_timeout();
        if (!wait_interval) return std::unexpected(wait_interval.error());
        if (lock.try_lock_for(std::chrono::milliseconds(*wait_interval))) {
            break;
        }
    }
    if (cancellation_requested(cancelled)) return cancelled_operation();
    return lock;
}

[[nodiscard]] std::unexpected<Error> io_failure(std::string message) {
    if (errno != 0) {
        message.append(": ");
        message.append(std::strerror(errno));
    }
    return fail(ErrorCode::io_error, std::move(message));
}

[[nodiscard]] std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       if (character >= 'A' && character <= 'Z') {
                           return static_cast<char>(character - 'A' + 'a');
                       }
                       return static_cast<char>(character);
                   });
    return result;
}

[[nodiscard]] std::string trim(std::string_view value) {
    usize first = 0;
    while (first < value.size() &&
           (value[first] == ' ' || value[first] == '\t')) {
        ++first;
    }
    usize last = value.size();
    while (last > first &&
           (value[last - 1U] == ' ' || value[last - 1U] == '\t')) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

[[nodiscard]] Status wait_fd(
    int descriptor,
    short events,
    const OperationDeadline& deadline,
    const CancellationCheck& cancelled = {}) {
    while (true) {
        if (cancellation_requested(cancelled)) return cancelled_operation();
        auto poll_timeout = cancelled.wake_descriptor >= 0
            ? deadline.next_fd_poll_timeout()
            : deadline.next_poll_timeout();
        if (!poll_timeout) return std::unexpected(poll_timeout.error());

        std::array<pollfd, 2> poll_descriptors {{
            pollfd {.fd = descriptor, .events = events, .revents = 0},
            pollfd {
                .fd = cancelled.wake_descriptor,
                .events = POLLIN,
                .revents = 0,
            },
        }};
        const nfds_t count = cancelled.wake_descriptor >= 0 ? 2U : 1U;
        const int result = ::poll(poll_descriptors.data(), count, *poll_timeout);
        if (result > 0) {
            if (count == 2U &&
                (poll_descriptors[1].revents &
                 static_cast<short>(POLLIN | POLLERR | POLLHUP | POLLNVAL)) !=
                    0) {
                return cancelled_operation();
            }
            if ((poll_descriptors[0].revents &
                 static_cast<short>(POLLERR | POLLHUP | POLLNVAL)) != 0 &&
                (poll_descriptors[0].revents & events) == 0) {
                return fail(ErrorCode::io_error,
                            "network descriptor reported an error");
            }
            if (cancellation_requested(cancelled)) {
                return cancelled_operation();
            }
            return {};
        }
        if (result == 0) continue;
        if (errno != EINTR) return io_failure("poll failed");
    }
}

[[nodiscard]] Status set_nonblocking(int descriptor, bool enabled) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0) return io_failure("fcntl(F_GETFL) failed");
    const int updated = enabled ? (flags | O_NONBLOCK)
                                : (flags & ~O_NONBLOCK);
    if (::fcntl(descriptor, F_SETFL, updated) < 0) {
        return io_failure("fcntl(F_SETFL) failed");
    }
    return {};
}

void configure_descriptor(int descriptor) noexcept {
#if defined(__APPLE__)
    int enabled = 1;
    (void)::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE,
                       &enabled, static_cast<socklen_t>(sizeof(enabled)));
#else
    (void)descriptor;
#endif
}

void configure_stream_descriptor(int descriptor) noexcept {
    configure_descriptor(descriptor);

    // SocketConnection.DELAY defaults to zero, meaning small-write delay is
    // disabled. Many J2ME protocols emit packet headers via repeated
    // DataOutputStream.writeByte calls, so enabling TCP_NODELAY by default
    // avoids Nagle/delayed-ACK stalls during login and proxy handshakes.
    int no_delay = 1;
    (void)::setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY,
                       &no_delay, static_cast<socklen_t>(sizeof(no_delay)));
}

[[nodiscard]] bool network_trace_enabled() noexcept {
    const char* value = std::getenv("PHONEME_NETWORK_TRACE");
    return value != nullptr && std::string_view(value) != "0";
}

void trace_network_wait(const char* event,
                        int descriptor,
                        usize requested,
                        usize queued = 0U) noexcept {
    if (!network_trace_enabled()) return;
    std::fprintf(stderr,
                 "[phoneMENetwork] %s fd=%d requested=%zu queued=%zu\n",
                 event,
                 descriptor,
                 requested,
                 queued);
}

void trace_network_bytes(const char* direction,
                         int descriptor,
                         std::span<const u8> bytes) noexcept {
    if (!network_trace_enabled()) return;
    std::fprintf(stderr, "[phoneMENetwork] %s fd=%d bytes=%zu hex=",
                 direction, descriptor, bytes.size());
    const bool show_full_packet =
        std::getenv("PHONEME_NETWORK_TRACE_FULL") != nullptr;
    const usize shown = show_full_packet
        ? bytes.size()
        : std::min<usize>(bytes.size(), 64U);
    for (usize index = 0; index < shown; ++index) {
        std::fprintf(stderr, "%02x", static_cast<unsigned>(bytes[index]));
    }
    if (shown < bytes.size()) std::fprintf(stderr, "...");
    std::fprintf(stderr, "\n");
}

[[nodiscard]] Result<Endpoint> endpoint_from_sockaddr(
    const sockaddr* address,
    socklen_t address_length) {
    std::array<char, NI_MAXHOST> host {};
    std::array<char, NI_MAXSERV> service {};
    const int result = ::getnameinfo(address, address_length,
                                     host.data(),
                                     static_cast<socklen_t>(host.size()),
                                     service.data(),
                                     static_cast<socklen_t>(service.size()),
                                     NI_NUMERICHOST | NI_NUMERICSERV);
    if (result != 0) {
        return fail(ErrorCode::io_error,
                    std::string("getnameinfo failed: ") +
                        ::gai_strerror(result));
    }
    u32 port = 0;
    const std::string_view service_view(service.data());
    const auto converted = std::from_chars(service_view.data(),
                                           service_view.data() +
                                               service_view.size(),
                                           port);
    if (converted.ec != std::errc {} ||
        port > std::numeric_limits<u16>::max()) {
        return fail(ErrorCode::io_error,
                    "resolved endpoint has invalid port");
    }
    return Endpoint {
        .host = host.data(),
        .port = static_cast<u16>(port),
    };
}

[[nodiscard]] Result<Endpoint> socket_endpoint(int descriptor,
                                               bool peer) {
    sockaddr_storage address {};
    socklen_t length = static_cast<socklen_t>(sizeof(address));
    const int result = peer
        ? ::getpeername(descriptor,
                        reinterpret_cast<sockaddr*>(&address), &length)
        : ::getsockname(descriptor,
                        reinterpret_cast<sockaddr*>(&address), &length);
    if (result != 0) {
        return io_failure(peer ? "getpeername failed" : "getsockname failed");
    }
    return endpoint_from_sockaddr(
        reinterpret_cast<const sockaddr*>(&address), length);
}

struct AddressList final {
    addrinfo* value {nullptr};

    AddressList() = default;
    AddressList(const AddressList&) = delete;
    AddressList& operator=(const AddressList&) = delete;
    AddressList(AddressList&& other) noexcept : value(other.value) {
        other.value = nullptr;
    }
    AddressList& operator=(AddressList&& other) noexcept {
        if (this != &other) {
            if (value != nullptr) ::freeaddrinfo(value);
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    ~AddressList() {
        if (value != nullptr) ::freeaddrinfo(value);
    }
};

#if defined(__APPLE__)
struct AddressResolutionState final {
    std::mutex mutex;
    std::condition_variable condition;
    std::string host;
    std::string service;
    addrinfo hints {};
    addrinfo* value {nullptr};
    int result {EAI_AGAIN};
    bool completed {false};

    ~AddressResolutionState() {
        if (value != nullptr) ::freeaddrinfo(value);
    }
};

void resolve_addresses_on_dispatch_queue(void* opaque) {
    std::unique_ptr<std::shared_ptr<AddressResolutionState>> owner(
        static_cast<std::shared_ptr<AddressResolutionState>*>(opaque));
    if (!owner || !*owner) return;
    const auto state = *owner;
    const i32 delay = g_address_resolution_delay_for_tests.load(
        std::memory_order_acquire);
    if (delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    addrinfo* value = nullptr;
    const char* host_pointer = state->host.empty()
        ? nullptr : state->host.c_str();
    const int result = ::getaddrinfo(
        host_pointer, state->service.c_str(), &state->hints, &value);
    {
        std::scoped_lock lock(state->mutex);
        state->result = result;
        state->value = value;
        state->completed = true;
    }
    state->condition.notify_all();
}
#endif

[[nodiscard]] Result<AddressList> resolve_addresses(
    std::string_view host,
    u16 port,
    int socket_type,
    bool passive,
    const OperationDeadline& deadline,
    const CancellationCheck& cancelled = {}) {
    auto active = check_operation_active(deadline, cancelled);
    if (!active) return std::unexpected(active.error());

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socket_type;
    hints.ai_protocol = socket_type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    const std::string service = std::to_string(port);
    std::string host_text(host);
    if (port == 1080U) {
        const char* redirect =
            std::getenv("PHONEME_NETWORK_REDIRECT_1080_HOST");
        if (redirect != nullptr && redirect[0] != '\0') {
            host_text.assign(redirect);
        }
    }

#if defined(__APPLE__)
    auto state = std::make_shared<AddressResolutionState>();
    state->host = host_text;
    state->service = service;
    state->hints = hints;
    auto* context =
        new std::shared_ptr<AddressResolutionState>(state);
    dispatch_async_f(
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
        context,
        &resolve_addresses_on_dispatch_queue);

    std::unique_lock lock(state->mutex);
    while (!state->completed) {
        if (cancellation_requested(cancelled)) {
            return cancelled_operation();
        }
        auto wait_interval = deadline.next_poll_timeout();
        if (!wait_interval) return std::unexpected(wait_interval.error());
        state->condition.wait_for(
            lock, std::chrono::milliseconds(*wait_interval),
            [&state] { return state->completed; });
    }
    const int result = state->result;
    AddressList list;
    list.value = std::exchange(state->value, nullptr);
#else
    AddressList list;
    const char* host_pointer = host.empty() ? nullptr : host_text.c_str();
    const int result = ::getaddrinfo(host_pointer, service.c_str(),
                                     &hints, &list.value);
#endif
    active = check_operation_active(deadline, cancelled);
    if (!active) return std::unexpected(active.error());
    if (result != 0) {
        return fail(ErrorCode::io_error,
                    std::string("DNS resolution failed: ") +
                        ::gai_strerror(result));
    }
    return list;
}

[[nodiscard]] Result<int> connect_stream_descriptor(
    const Url& url,
    const OperationDeadline& deadline,
    const CancellationCheck& cancelled = {}) {
    if (cancellation_requested(cancelled)) return cancelled_operation();
    auto addresses = resolve_addresses(
        url.host, url.effective_port(), SOCK_STREAM, false,
        deadline, cancelled);
    if (!addresses) return std::unexpected(addresses.error());
    if (cancellation_requested(cancelled)) return cancelled_operation();

    Error last_error = Error::make(ErrorCode::io_error,
                                   "no resolved address could be connected");
    for (addrinfo* address = addresses->value;
         address != nullptr;
         address = address->ai_next) {
        auto active = check_operation_active(deadline, cancelled);
        if (!active) return std::unexpected(active.error());
        const int descriptor = ::socket(address->ai_family,
                                        address->ai_socktype,
                                        address->ai_protocol);
        if (descriptor < 0) {
            last_error = Error::make(ErrorCode::io_error,
                                     "socket creation failed");
            continue;
        }
        configure_stream_descriptor(descriptor);
        auto nonblocking = set_nonblocking(descriptor, true);
        if (!nonblocking) {
            last_error = nonblocking.error();
            ::close(descriptor);
            continue;
        }
        int result = ::connect(descriptor, address->ai_addr,
                               static_cast<socklen_t>(address->ai_addrlen));
        if (result != 0 && errno == EINPROGRESS) {
            auto ready = wait_fd(descriptor, POLLOUT, deadline, cancelled);
            if (!ready) {
                last_error = ready.error();
                ::close(descriptor);
                continue;
            }
            int socket_error_value = 0;
            socklen_t length = static_cast<socklen_t>(
                sizeof(socket_error_value));
            if (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR,
                             &socket_error_value, &length) != 0 ||
                socket_error_value != 0) {
                last_error = Error::make(
                    ErrorCode::io_error,
                    socket_error_value == 0
                        ? "connect failed"
                        : std::string("connect failed: ") +
                              std::strerror(socket_error_value));
                ::close(descriptor);
                continue;
            }
            result = 0;
        }
        if (result != 0) {
            last_error = Error::make(ErrorCode::io_error,
                                     std::string("connect failed: ") +
                                         std::strerror(errno));
            ::close(descriptor);
            continue;
        }
        if (cancellation_requested(cancelled)) {
            ::close(descriptor);
            return cancelled_operation();
        }
        return descriptor;
    }
    return std::unexpected(std::move(last_error));
}

[[nodiscard]] Result<int> create_server_descriptor(
    const Url& url,
    const OperationDeadline& deadline,
    const CancellationCheck& cancelled = {}) {
    auto addresses = resolve_addresses(
        url.host, url.effective_port(), SOCK_STREAM, true,
        deadline, cancelled);
    if (!addresses) return std::unexpected(addresses.error());

    Error last_error = Error::make(ErrorCode::io_error,
                                   "no local address could be bound");
    for (addrinfo* address = addresses->value;
         address != nullptr;
         address = address->ai_next) {
        auto active = check_operation_active(deadline, cancelled);
        if (!active) return std::unexpected(active.error());
        const int descriptor = ::socket(address->ai_family,
                                        address->ai_socktype,
                                        address->ai_protocol);
        if (descriptor < 0) continue;
        configure_descriptor(descriptor);
        int enabled = 1;
        (void)::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR,
                           &enabled, static_cast<socklen_t>(sizeof(enabled)));
        if (::bind(descriptor, address->ai_addr,
                   static_cast<socklen_t>(address->ai_addrlen)) == 0 &&
            ::listen(descriptor, 16) == 0) {
            auto nonblocking = set_nonblocking(descriptor, true);
            if (!nonblocking) {
                last_error = nonblocking.error();
                ::close(descriptor);
                continue;
            }
            return descriptor;
        }
        last_error = Error::make(ErrorCode::io_error,
                                 std::string("bind/listen failed: ") +
                                     std::strerror(errno));
        ::close(descriptor);
    }
    return std::unexpected(std::move(last_error));
}

[[nodiscard]] Result<int> create_datagram_descriptor(
    const Url& url,
    const OperationDeadline& deadline,
    const CancellationCheck& cancelled = {}) {
    const bool local = url.host.empty();
    auto addresses = resolve_addresses(
        url.host, url.effective_port(), SOCK_DGRAM, local,
        deadline, cancelled);
    if (!addresses) return std::unexpected(addresses.error());

    Error last_error = Error::make(ErrorCode::io_error,
                                   "no datagram address could be opened");
    for (addrinfo* address = addresses->value;
         address != nullptr;
         address = address->ai_next) {
        auto active = check_operation_active(deadline, cancelled);
        if (!active) return std::unexpected(active.error());
        const int descriptor = ::socket(address->ai_family,
                                        address->ai_socktype,
                                        address->ai_protocol);
        if (descriptor < 0) continue;
        configure_descriptor(descriptor);
        auto nonblocking = set_nonblocking(descriptor, true);
        if (!nonblocking) {
            last_error = nonblocking.error();
            ::close(descriptor);
            continue;
        }
        const int result = local
            ? ::bind(descriptor, address->ai_addr,
                     static_cast<socklen_t>(address->ai_addrlen))
            : ::connect(descriptor, address->ai_addr,
                        static_cast<socklen_t>(address->ai_addrlen));
        if (result == 0) return descriptor;
        last_error = Error::make(ErrorCode::io_error,
                                 std::string(local ? "UDP bind failed: "
                                                   : "UDP connect failed: ") +
                                     std::strerror(errno));
        ::close(descriptor);
    }
    return std::unexpected(std::move(last_error));
}

[[nodiscard]] Status send_all(
    int descriptor,
    std::span<const u8> bytes,
    const OperationDeadline& deadline,
    const CancellationCheck& cancelled = {}) {
    usize offset = 0;
    while (offset < bytes.size()) {
        auto ready = wait_fd(descriptor, POLLOUT, deadline, cancelled);
        if (!ready) return ready;
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const ssize_t written = ::send(
            descriptor,
            bytes.data() + offset,
            bytes.size() - offset,
            flags);
        if (written > 0) {
            offset += static_cast<usize>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return io_failure("socket write failed");
    }
    return {};
}

[[nodiscard]] Result<std::optional<usize>> complete_http_response_length(
    std::string_view request_method,
    std::span<const u8> bytes);

[[nodiscard]] Result<std::vector<u8>> receive_http_response(
    int descriptor,
    std::string_view request_method,
    const OperationDeadline& deadline,
    const CancellationCheck& cancelled = {}) {
    std::vector<u8> result;
    std::array<u8, kIoChunkSize> buffer {};
    while (true) {
        auto complete = complete_http_response_length(request_method, result);
        if (!complete) return std::unexpected(complete.error());
        if (complete->has_value()) {
            result.resize(**complete);
            return result;
        }

        auto ready = wait_fd(descriptor, POLLIN, deadline, cancelled);
        if (!ready) return std::unexpected(ready.error());
        const ssize_t count = ::recv(descriptor, buffer.data(),
                                     buffer.size(), 0);
        if (count == 0) return result;
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return io_failure("socket read failed");
        }
        const usize amount = static_cast<usize>(count);
        if (amount > kMaximumHttpResponseBytes - result.size()) {
            return fail(ErrorCode::overflow,
                        "HTTP response exceeds maximum size");
        }
        result.insert(result.end(), buffer.begin(),
                      buffer.begin() + static_cast<isize>(amount));
    }
}

[[nodiscard]] Result<std::vector<u8>> decode_chunked(
    std::span<const u8> encoded) {
    std::vector<u8> result;
    usize cursor = 0;
    while (true) {
        usize line_end = cursor;
        while (line_end + 1U < encoded.size() &&
               !(encoded[line_end] == '\r' &&
                 encoded[line_end + 1U] == '\n')) {
            ++line_end;
        }
        if (line_end + 1U >= encoded.size()) {
            return fail(ErrorCode::io_error,
                        "truncated chunked HTTP body");
        }
        std::string_view line(
            reinterpret_cast<const char*>(encoded.data() + cursor),
            line_end - cursor);
        const usize extension = line.find(';');
        if (extension != std::string_view::npos) {
            line = line.substr(0, extension);
        }
        usize chunk_size = 0;
        const auto converted = std::from_chars(line.data(),
                                               line.data() + line.size(),
                                               chunk_size, 16);
        if (converted.ec != std::errc {} ||
            converted.ptr != line.data() + line.size()) {
            return fail(ErrorCode::io_error,
                        "invalid HTTP chunk size");
        }
        cursor = line_end + 2U;
        if (chunk_size == 0U) {
            while (true) {
                usize trailer_end = cursor;
                while (trailer_end + 1U < encoded.size() &&
                       !(encoded[trailer_end] == '\r' &&
                         encoded[trailer_end + 1U] == '\n')) {
                    ++trailer_end;
                }
                if (trailer_end + 1U >= encoded.size()) {
                    return fail(ErrorCode::io_error,
                                "truncated HTTP chunk trailer");
                }
                if (trailer_end == cursor) {
                    cursor = trailer_end + 2U;
                    if (cursor != encoded.size()) {
                        return fail(ErrorCode::io_error,
                                    "unexpected bytes after chunked response");
                    }
                    return result;
                }
                const std::string_view trailer(
                    reinterpret_cast<const char*>(encoded.data() + cursor),
                    trailer_end - cursor);
                const usize separator = trailer.find(':');
                if (separator == std::string_view::npos || separator == 0U) {
                    return fail(ErrorCode::io_error,
                                "malformed HTTP chunk trailer");
                }
                cursor = trailer_end + 2U;
            }
        }
        if (chunk_size > encoded.size() - cursor ||
            chunk_size + 2U > encoded.size() - cursor) {
            return fail(ErrorCode::io_error,
                        "HTTP chunk exceeds received body");
        }
        if (chunk_size > kMaximumHttpResponseBytes - result.size()) {
            return fail(ErrorCode::overflow,
                        "decoded HTTP body exceeds maximum size");
        }
        result.insert(result.end(),
                      encoded.begin() + static_cast<isize>(cursor),
                      encoded.begin() +
                          static_cast<isize>(cursor + chunk_size));
        cursor += chunk_size;
        if (encoded[cursor] != '\r' || encoded[cursor + 1U] != '\n') {
            return fail(ErrorCode::io_error,
                        "HTTP chunk is missing terminator");
        }
        cursor += 2U;
    }
}

[[nodiscard]] std::optional<std::string> header_value(
    const std::vector<Header>& headers,
    std::string_view name) {
    const std::string key = lowercase(name);
    for (const auto& [header_name, value] : headers) {
        if (lowercase(header_name) == key) return value;
    }
    return std::nullopt;
}

enum class TransferFraming : u8 {
    none,
    chunked,
    connection_close,
};

[[nodiscard]] Result<TransferFraming> transfer_framing(
    const std::vector<Header>& headers) {
    std::vector<std::string> codings;
    for (const auto& [name, value] : headers) {
        if (lowercase(name) != "transfer-encoding") continue;
        usize cursor = 0U;
        while (cursor <= value.size()) {
            const usize separator = value.find(',', cursor);
            const usize end = separator == std::string::npos
                ? value.size() : separator;
            std::string coding = lowercase(trim(
                std::string_view(value).substr(cursor, end - cursor)));
            if (coding.empty()) {
                return fail(ErrorCode::io_error,
                            "HTTP response has an empty transfer coding");
            }
            const usize parameter = coding.find(';');
            if (parameter != std::string::npos) {
                if (coding.substr(0U, parameter) == "chunked") {
                    return fail(ErrorCode::io_error,
                                "HTTP chunked transfer coding has parameters");
                }
                coding.resize(parameter);
                coding = trim(coding);
                if (coding.empty()) {
                    return fail(ErrorCode::io_error,
                                "HTTP response transfer coding is invalid");
                }
            }
            codings.push_back(std::move(coding));
            if (separator == std::string::npos) break;
            cursor = separator + 1U;
        }
    }
    if (codings.empty()) return TransferFraming::none;

    bool saw_chunked = false;
    for (usize index = 0U; index < codings.size(); ++index) {
        if (codings[index] != "chunked") continue;
        if (saw_chunked || index + 1U != codings.size()) {
            return fail(ErrorCode::io_error,
                        "HTTP chunked transfer coding must appear once and last");
        }
        saw_chunked = true;
    }
    return saw_chunked ? TransferFraming::chunked
                       : TransferFraming::connection_close;
}

[[nodiscard]] bool valid_response_header_name(std::string_view name) {
    if (name.empty()) return false;
    for (const char value : name) {
        const auto character = static_cast<unsigned char>(value);
        if (character <= 0x20U || character >= 0x7FU || character == ':') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result<std::optional<usize>> content_length(
    const std::vector<Header>& headers) {
    std::optional<usize> parsed_length;
    for (const auto& [name, value] : headers) {
        if (lowercase(name) != "content-length") continue;
        usize length = 0;
        const auto converted = std::from_chars(
            value.data(), value.data() + value.size(), length);
        if (converted.ec != std::errc {} ||
            converted.ptr != value.data() + value.size()) {
            return fail(ErrorCode::io_error,
                        "HTTP response Content-Length is invalid");
        }
        if (parsed_length.has_value() && *parsed_length != length) {
            return fail(ErrorCode::io_error,
                        "HTTP response has conflicting Content-Length headers");
        }
        parsed_length = length;
    }
    return parsed_length;
}

[[nodiscard]] Result<std::optional<usize>> complete_chunked_encoded_length(
    std::span<const u8> encoded) {
    usize cursor = 0U;
    while (true) {
        usize line_end = cursor;
        while (line_end + 1U < encoded.size() &&
               !(encoded[line_end] == '\r' &&
                 encoded[line_end + 1U] == '\n')) {
            ++line_end;
        }
        if (line_end + 1U >= encoded.size()) {
            if (encoded.size() - cursor > kMaximumHttpHeaderBytes) {
                return fail(ErrorCode::io_error,
                            "HTTP chunk size line exceeds maximum size");
            }
            return std::optional<usize> {};
        }
        std::string_view line(
            reinterpret_cast<const char*>(encoded.data() + cursor),
            line_end - cursor);
        const usize extension = line.find(';');
        if (extension != std::string_view::npos) {
            line = line.substr(0U, extension);
        }
        if (line.empty()) {
            return fail(ErrorCode::io_error,
                        "HTTP chunk size is empty");
        }
        usize chunk_size = 0U;
        const auto converted = std::from_chars(
            line.data(), line.data() + line.size(), chunk_size, 16);
        if (converted.ec != std::errc {} ||
            converted.ptr != line.data() + line.size()) {
            return fail(ErrorCode::io_error,
                        "invalid HTTP chunk size");
        }
        if (chunk_size > kMaximumHttpResponseBytes) {
            return fail(ErrorCode::overflow,
                        "HTTP chunk exceeds maximum response size");
        }
        cursor = line_end + 2U;

        if (chunk_size == 0U) {
            while (true) {
                usize trailer_end = cursor;
                while (trailer_end + 1U < encoded.size() &&
                       !(encoded[trailer_end] == '\r' &&
                         encoded[trailer_end + 1U] == '\n')) {
                    ++trailer_end;
                }
                if (trailer_end + 1U >= encoded.size()) {
                    if (encoded.size() - cursor > kMaximumHttpHeaderBytes) {
                        return fail(ErrorCode::io_error,
                                    "HTTP chunk trailer exceeds maximum size");
                    }
                    return std::optional<usize> {};
                }
                if (trailer_end == cursor) {
                    return std::optional<usize>(trailer_end + 2U);
                }
                const std::string_view trailer(
                    reinterpret_cast<const char*>(encoded.data() + cursor),
                    trailer_end - cursor);
                const usize separator = trailer.find(':');
                if (separator == std::string_view::npos || separator == 0U ||
                    !valid_response_header_name(
                        trailer.substr(0U, separator))) {
                    return fail(ErrorCode::io_error,
                                "malformed HTTP chunk trailer");
                }
                cursor = trailer_end + 2U;
            }
        }

        const usize remaining = encoded.size() - cursor;
        if (chunk_size > remaining || remaining - chunk_size < 2U) {
            return std::optional<usize> {};
        }
        cursor += chunk_size;
        if (encoded[cursor] != '\r' || encoded[cursor + 1U] != '\n') {
            return fail(ErrorCode::io_error,
                        "HTTP chunk is missing terminator");
        }
        cursor += 2U;
    }
}

[[nodiscard]] Result<std::optional<usize>> complete_http_response_length(
    std::string_view request_method,
    std::span<const u8> bytes) {
    usize offset = 0U;
    for (usize response_index = 0U;
         response_index <= kMaximumInterimResponses;
         ++response_index) {
        const std::span<const u8> remaining = bytes.subspan(offset);
        usize header_end = 0U;
        bool found = false;
        const usize search_limit = std::min(
            remaining.size(), kMaximumHttpHeaderBytes + 4U);
        while (header_end + 3U < search_limit) {
            if (remaining[header_end] == '\r' &&
                remaining[header_end + 1U] == '\n' &&
                remaining[header_end + 2U] == '\r' &&
                remaining[header_end + 3U] == '\n') {
                found = true;
                break;
            }
            ++header_end;
        }
        if (!found) {
            if (remaining.size() > kMaximumHttpHeaderBytes) {
                return fail(ErrorCode::io_error,
                            "HTTP response header exceeds maximum size");
            }
            return std::optional<usize> {};
        }

        const std::string_view header_text(
            reinterpret_cast<const char*>(remaining.data()), header_end);
        const usize status_separator = header_text.find("\r\n");
        const usize status_end = status_separator == std::string_view::npos
            ? header_text.size() : status_separator;
        const std::string_view status_line = header_text.substr(0U, status_end);
        const usize first_space = status_line.find(' ');
        const usize second_space = first_space == std::string_view::npos
            ? std::string_view::npos
            : status_line.find(' ', first_space + 1U);
        if (!status_line.starts_with("HTTP/") ||
            first_space == std::string_view::npos) {
            return fail(ErrorCode::io_error,
                        "HTTP response status line is invalid");
        }
        const std::string_view code_text =
            second_space == std::string_view::npos
                ? status_line.substr(first_space + 1U)
                : status_line.substr(first_space + 1U,
                                     second_space - first_space - 1U);
        i32 status_code = -1;
        const auto code_converted = std::from_chars(
            code_text.data(), code_text.data() + code_text.size(),
            status_code);
        if (code_converted.ec != std::errc {} ||
            code_converted.ptr != code_text.data() + code_text.size() ||
            status_code < 100 || status_code > 599) {
            return fail(ErrorCode::io_error,
                        "HTTP response status code is invalid");
        }

        std::vector<Header> headers;
        usize cursor = status_separator == std::string_view::npos
            ? header_text.size() : status_end + 2U;
        while (cursor < header_text.size()) {
            const usize line_end = header_text.find("\r\n", cursor);
            const usize end = line_end == std::string_view::npos
                ? header_text.size() : line_end;
            const std::string_view line =
                header_text.substr(cursor, end - cursor);
            const usize separator = line.find(':');
            if (separator == std::string_view::npos || separator == 0U ||
                !valid_response_header_name(line.substr(0U, separator))) {
                return fail(ErrorCode::io_error,
                            "HTTP response contains malformed header");
            }
            headers.emplace_back(
                std::string(line.substr(0U, separator)),
                trim(line.substr(separator + 1U)));
            if (line_end == std::string_view::npos) break;
            cursor = line_end + 2U;
        }

        const usize body_offset = offset + header_end + 4U;
        if (status_code >= 100 && status_code < 200 &&
            status_code != 101) {
            if (response_index == kMaximumInterimResponses) {
                return fail(ErrorCode::io_error,
                            "HTTP response has too many interim responses");
            }
            offset = body_offset;
            continue;
        }

        const bool body_forbidden =
            lowercase(request_method) == "head" ||
            (status_code >= 100 && status_code < 200) ||
            status_code == 204 || status_code == 304;
        if (body_forbidden) return std::optional<usize>(body_offset);

        auto framing = transfer_framing(headers);
        if (!framing) return std::unexpected(framing.error());
        if (*framing == TransferFraming::chunked) {
            auto encoded_length = complete_chunked_encoded_length(
                bytes.subspan(body_offset));
            if (!encoded_length) {
                return std::unexpected(encoded_length.error());
            }
            if (!encoded_length->has_value()) return std::optional<usize> {};
            return std::optional<usize>(body_offset + **encoded_length);
        }
        if (*framing == TransferFraming::connection_close) {
            return std::optional<usize> {};
        }

        auto declared_length = content_length(headers);
        if (!declared_length) return std::unexpected(declared_length.error());
        if (!declared_length->has_value()) return std::optional<usize> {};
        if (**declared_length > bytes.size() - body_offset) {
            return std::optional<usize> {};
        }
        return std::optional<usize>(body_offset + **declared_length);
    }
    return fail(ErrorCode::io_error,
                "HTTP response has too many interim responses");
}

[[nodiscard]] Result<HttpResponse> parse_http_response(
    const Url& url,
    std::string_view request_method,
    std::span<const u8> bytes) {
    std::span<const u8> remaining = bytes;
    for (usize response_index = 0;
         response_index <= kMaximumInterimResponses;
         ++response_index) {
        usize header_end = 0;
        bool found = false;
        const usize search_limit = std::min(
            remaining.size(), kMaximumHttpHeaderBytes + 4U);
        while (header_end + 3U < search_limit) {
            if (remaining[header_end] == '\r' &&
                remaining[header_end + 1U] == '\n' &&
                remaining[header_end + 2U] == '\r' &&
                remaining[header_end + 3U] == '\n') {
                found = true;
                break;
            }
            ++header_end;
        }
        if (!found) {
            return fail(
                ErrorCode::io_error,
                remaining.size() > kMaximumHttpHeaderBytes
                    ? "HTTP response header exceeds maximum size"
                    : "HTTP response has no complete header block");
        }

        const std::string_view header_text(
            reinterpret_cast<const char*>(remaining.data()), header_end);
        const usize status_separator = header_text.find("\r\n");
        const usize status_end = status_separator == std::string_view::npos
            ? header_text.size() : status_separator;
        const std::string_view status_line = header_text.substr(0, status_end);
        const usize first_space = status_line.find(' ');
        const usize second_space = first_space == std::string_view::npos
            ? std::string_view::npos
            : status_line.find(' ', first_space + 1U);
        if (!status_line.starts_with("HTTP/") ||
            first_space == std::string_view::npos) {
            return fail(ErrorCode::io_error,
                        "HTTP response status line is invalid");
        }
        const std::string_view code_text =
            second_space == std::string_view::npos
                ? status_line.substr(first_space + 1U)
                : status_line.substr(first_space + 1U,
                                     second_space - first_space - 1U);
        i32 status_code = -1;
        const auto code_converted = std::from_chars(
            code_text.data(), code_text.data() + code_text.size(),
            status_code);
        if (code_converted.ec != std::errc {} ||
            code_converted.ptr != code_text.data() + code_text.size() ||
            status_code < 100 || status_code > 599) {
            return fail(ErrorCode::io_error,
                        "HTTP response status code is invalid");
        }

        HttpResponse response;
        response.final_url = url;
        response.status_code = status_code;
        if (second_space != std::string_view::npos) {
            response.reason =
                std::string(status_line.substr(second_space + 1U));
        }

        usize cursor = status_separator == std::string_view::npos
            ? header_text.size() : status_end + 2U;
        while (cursor < header_text.size()) {
            const usize line_end = header_text.find("\r\n", cursor);
            const usize end = line_end == std::string_view::npos
                ? header_text.size() : line_end;
            const std::string_view line =
                header_text.substr(cursor, end - cursor);
            const usize separator = line.find(':');
            if (separator == std::string_view::npos || separator == 0U ||
                !valid_response_header_name(line.substr(0, separator))) {
                return fail(ErrorCode::io_error,
                            "HTTP response contains malformed header");
            }
            response.headers.emplace_back(
                std::string(line.substr(0, separator)),
                trim(line.substr(separator + 1U)));
            if (line_end == std::string_view::npos) break;
            cursor = line_end + 2U;
        }

        const usize body_offset = header_end + 4U;
        if (status_code >= 100 && status_code < 200 &&
            status_code != 101) {
            if (response_index == kMaximumInterimResponses) {
                return fail(ErrorCode::io_error,
                            "HTTP response has too many interim responses");
            }
            remaining = remaining.subspan(body_offset);
            continue;
        }

        const bool body_forbidden =
            lowercase(request_method) == "head" ||
            (status_code >= 100 && status_code < 200) ||
            status_code == 204 || status_code == 304;
        if (body_forbidden) return response;

        std::span<const u8> body = remaining.subspan(body_offset);
        auto framing = transfer_framing(response.headers);
        if (!framing) return std::unexpected(framing.error());
        if (*framing == TransferFraming::chunked) {
            auto decoded = decode_chunked(body);
            if (!decoded) return std::unexpected(decoded.error());
            response.body = std::move(*decoded);
            return response;
        }
        if (*framing == TransferFraming::connection_close) {
            response.body.assign(body.begin(), body.end());
            return response;
        }

        auto declared_length = content_length(response.headers);
        if (!declared_length) {
            return std::unexpected(declared_length.error());
        }
        if (declared_length->has_value()) {
            if (**declared_length > body.size()) {
                return fail(ErrorCode::io_error,
                            "HTTP response body is shorter than Content-Length");
            }
            body = body.first(**declared_length);
        }
        response.body.assign(body.begin(), body.end());
        return response;
    }
    return fail(ErrorCode::io_error,
                "HTTP response has too many interim responses");
}

#if defined(PHONEME_WEB)
struct BrowserFetchHandle final {
    emscripten_fetch_t* value {nullptr};

    BrowserFetchHandle() = default;
    explicit BrowserFetchHandle(emscripten_fetch_t* fetch) : value(fetch) {}
    BrowserFetchHandle(const BrowserFetchHandle&) = delete;
    BrowserFetchHandle& operator=(const BrowserFetchHandle&) = delete;
    ~BrowserFetchHandle() {
        if (value != nullptr) emscripten_fetch_close(value);
    }
};

void append_browser_u32(std::vector<u8>& output, u32 value) {
    output.push_back(static_cast<u8>(value & 0xFFU));
    output.push_back(static_cast<u8>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<u8>((value >> 16U) & 0xFFU));
    output.push_back(static_cast<u8>((value >> 24U) & 0xFFU));
}

[[nodiscard]] u32 read_browser_u32(std::span<const u8> bytes,
                                   usize offset) noexcept {
    if (offset + 4U > bytes.size()) return 0U;
    return static_cast<u32>(bytes[offset]) |
           (static_cast<u32>(bytes[offset + 1U]) << 8U) |
           (static_cast<u32>(bytes[offset + 2U]) << 16U) |
           (static_cast<u32>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] std::string serialize_browser_proxy_headers(
    const std::vector<Header>& headers) {
    std::string serialized;
    for (const auto& [name, value] : headers) {
        if (name.find_first_of("\r\n") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos) {
            continue;
        }
        if (serialized.size() + name.size() + value.size() + 4U >
            kMaximumHttpHeaderBytes) {
            break;
        }
        serialized.append(name);
        serialized.append(": ");
        serialized.append(value);
        serialized.append("\r\n");
    }
    return serialized;
}

[[nodiscard]] std::vector<Header> parse_browser_response_headers(
    std::string_view serialized) {
    std::vector<Header> headers;
    usize cursor = 0;
    while (cursor < serialized.size()) {
        usize end = serialized.find('\n', cursor);
        if (end == std::string_view::npos) end = serialized.size();
        std::string_view line = serialized.substr(cursor, end - cursor);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        const usize separator = line.find(':');
        if (separator != std::string_view::npos && separator != 0U) {
            headers.emplace_back(std::string(line.substr(0U, separator)),
                                 trim(line.substr(separator + 1U)));
        }
        cursor = end == serialized.size() ? end : end + 1U;
    }
    return headers;
}

[[nodiscard]] Result<HttpResponse> perform_browser_http(HttpRequest request) {
    if (request.method.empty() || request.method.size() >= 32U) {
        return fail(ErrorCode::invalid_argument,
                    "browser HTTP method is invalid");
    }
    if (request.body.size() > kMaximumHttpResponseBytes) {
        return fail(ErrorCode::overflow,
                    "browser HTTP request body exceeds safety limit");
    }

    const std::string url = request.url.to_string();
    const std::string headers = serialize_browser_proxy_headers(request.headers);
    if (request.method.size() > std::numeric_limits<u32>::max() ||
        url.size() > std::numeric_limits<u32>::max() ||
        headers.size() > std::numeric_limits<u32>::max() ||
        request.body.size() > std::numeric_limits<u32>::max()) {
        return fail(ErrorCode::overflow,
                    "browser HTTP proxy request exceeds envelope limit");
    }

    std::vector<u8> envelope;
    envelope.reserve(20U + request.method.size() + url.size() +
                     headers.size() + request.body.size());
    envelope.insert(envelope.end(), {'P', 'M', 'H', '1'});
    append_browser_u32(envelope, static_cast<u32>(request.method.size()));
    append_browser_u32(envelope, static_cast<u32>(url.size()));
    append_browser_u32(envelope, static_cast<u32>(headers.size()));
    append_browser_u32(envelope, static_cast<u32>(request.body.size()));
    envelope.insert(envelope.end(), request.method.begin(), request.method.end());
    envelope.insert(envelope.end(), url.begin(), url.end());
    envelope.insert(envelope.end(), headers.begin(), headers.end());
    envelope.insert(envelope.end(), request.body.begin(), request.body.end());

    emscripten_fetch_attr_t attributes;
    emscripten_fetch_attr_init(&attributes);
    std::memcpy(attributes.requestMethod, "POST", 5U);
    attributes.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY |
                            EMSCRIPTEN_FETCH_SYNCHRONOUS |
                            EMSCRIPTEN_FETCH_REPLACE;
    attributes.timeoutMSecs = request.timeout_ms > 0
        ? static_cast<uint32_t>(request.timeout_ms)
        : 0U;
    attributes.requestData = reinterpret_cast<const char*>(envelope.data());
    attributes.requestDataSize = envelope.size();
    const char* proxy_headers[] = {
        "Content-Type", "application/octet-stream",
        "X-PhoneME-HTTP-Bridge", "1",
        nullptr
    };
    attributes.requestHeaders = proxy_headers;

    BrowserFetchHandle fetch(emscripten_fetch(&attributes, "/api/http"));
    if (fetch.value == nullptr) {
        return fail(ErrorCode::io_error,
                    "browser HTTP bridge could not be started");
    }
    if (fetch.value->status == 0U) {
        const std::string detail = fetch.value->statusText[0] != '\0'
            ? std::string(fetch.value->statusText)
            : std::string("HTTP bridge unavailable");
        return fail(ErrorCode::io_error,
                    "browser HTTP bridge failed: " + detail);
    }
    if (fetch.value->status < 200U || fetch.value->status >= 300U) {
        return fail(ErrorCode::io_error,
                    "browser HTTP bridge returned status " +
                    std::to_string(fetch.value->status));
    }
    if (fetch.value->data == nullptr || fetch.value->numBytes < 24U ||
        fetch.value->numBytes > kMaximumHttpResponseBytes +
                                  kMaximumHttpHeaderBytes + 256U * 1024U) {
        return fail(ErrorCode::io_error,
                    "browser HTTP bridge returned an invalid response");
    }

    const auto* raw = reinterpret_cast<const u8*>(fetch.value->data);
    const std::span<const u8> bytes(raw,
        static_cast<usize>(fetch.value->numBytes));
    if (std::memcmp(bytes.data(), "PMR1", 4U) != 0) {
        return fail(ErrorCode::io_error,
                    "browser HTTP bridge response has invalid magic");
    }
    const i32 status = static_cast<i32>(read_browser_u32(bytes, 4U));
    const usize final_url_length = read_browser_u32(bytes, 8U);
    const usize reason_length = read_browser_u32(bytes, 12U);
    const usize header_length = read_browser_u32(bytes, 16U);
    const usize body_length = read_browser_u32(bytes, 20U);
    if (header_length > kMaximumHttpHeaderBytes ||
        body_length > kMaximumHttpResponseBytes) {
        return fail(ErrorCode::overflow,
                    "browser HTTP bridge response exceeds safety limit");
    }
    const usize metadata_length = final_url_length + reason_length +
                                  header_length;
    if (metadata_length > bytes.size() - 24U ||
        body_length > bytes.size() - 24U - metadata_length) {
        return fail(ErrorCode::io_error,
                    "browser HTTP bridge response is truncated");
    }

    usize cursor = 24U;
    const std::string final_url(
        reinterpret_cast<const char*>(bytes.data() + cursor),
        final_url_length);
    cursor += final_url_length;
    const std::string reason(
        reinterpret_cast<const char*>(bytes.data() + cursor),
        reason_length);
    cursor += reason_length;
    const std::string serialized_headers(
        reinterpret_cast<const char*>(bytes.data() + cursor),
        header_length);
    cursor += header_length;

    HttpResponse response;
    response.final_url = request.url;
    if (!final_url.empty()) {
        auto parsed_url = Url::parse(final_url);
        if (parsed_url) response.final_url = std::move(*parsed_url);
    }
    response.status_code = status;
    response.reason = reason;
    response.headers = parse_browser_response_headers(serialized_headers);
    if (body_length > 0U) {
        response.body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                             bytes.begin() + static_cast<std::ptrdiff_t>(cursor + body_length));
    }
    if (response.final_url.scheme == Scheme::https) {
        response.security = SecurityMetadata {
            .protocol_name = "TLS",
            .protocol_version = "browser-proxy",
            .cipher_suite = "browser-managed",
        };
    }
    return response;
}
#endif

#if defined(__APPLE__)
struct AppleHttpsHandle final {
    i32 value {0};

    AppleHttpsHandle() = default;
    explicit AppleHttpsHandle(i32 handle) : value(handle) {}
    AppleHttpsHandle(const AppleHttpsHandle&) = delete;
    AppleHttpsHandle& operator=(const AppleHttpsHandle&) = delete;
    ~AppleHttpsHandle() {
        if (value > 0) phoneme_ios_https_close(value);
    }
};

[[nodiscard]] Result<std::optional<std::string>> copy_apple_https_string(
    i32 handle,
    i32 field) {
    const i32 length = phoneme_ios_https_copy_string(handle, field,
                                                     nullptr, 0);
    if (length < 0) return std::optional<std::string> {};
    if (length == std::numeric_limits<i32>::max()) {
        return fail(ErrorCode::overflow,
                    "HTTPS bridge string is too large");
    }
    std::vector<char> buffer(static_cast<usize>(length) + 1U, '\0');
    const i32 copied = phoneme_ios_https_copy_string(
        handle, field, buffer.data(), static_cast<i32>(buffer.size()));
    if (copied != length) {
        return fail(ErrorCode::io_error,
                    "HTTPS bridge could not copy response string");
    }
    return std::optional<std::string>(
        std::string(buffer.data(), static_cast<usize>(length)));
}

[[nodiscard]] std::vector<Header> parse_apple_https_headers(
    std::string_view serialized) {
    std::vector<Header> headers;
    usize cursor = 0;
    while (cursor < serialized.size()) {
        usize end = serialized.find('\n', cursor);
        if (end == std::string_view::npos) end = serialized.size();
        std::string_view line = serialized.substr(cursor, end - cursor);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        const usize separator = line.find(':');
        if (separator != std::string_view::npos && separator != 0U) {
            headers.emplace_back(std::string(line.substr(0, separator)),
                                 trim(line.substr(separator + 1U)));
        }
        cursor = end == serialized.size() ? end : end + 1U;
    }
    return headers;
}

[[nodiscard]] std::string serialize_apple_http_headers(
    const HttpRequest& request) {
    std::string serialized;
    for (const auto& [name, value] : request.headers) {
        serialized.append(name);
        serialized.append(": ");
        serialized.append(value);
        serialized.append("\r\n");
    }
    return serialized;
}

[[nodiscard]] Result<HttpResponse> collect_apple_http_response(
    i32 raw_handle,
    const HttpRequest& request) {
    AppleHttpsHandle handle(raw_handle);
    if (handle.value <= 0) {
        return fail(ErrorCode::io_error,
                    "Apple HTTP bridge returned an invalid result handle");
    }

    auto error = copy_apple_https_string(handle.value, 10);
    if (!error) return std::unexpected(error.error());
    if (error->has_value() && !(**error).empty()) {
        const i64 platform_error = phoneme_ios_https_get_long(handle.value, 3);
        constexpr i64 kCertificateBadDate = -1201;
        constexpr i64 kClientCertificateRequired = -1206;
        if (platform_error <= kCertificateBadDate &&
            platform_error >= kClientCertificateRequired) {
            return fail_java("java/lang/SecurityException", **error);
        }
        return fail(ErrorCode::io_error, **error);
    }

    HttpResponse response;
    response.status_code = phoneme_ios_https_get_status_code(handle.value);
    if (response.status_code < 100) {
        return fail(ErrorCode::io_error,
                    "HTTPS bridge returned an invalid status code");
    }
    auto reason = copy_apple_https_string(handle.value, 1);
    auto headers = copy_apple_https_string(handle.value, 2);
    auto final_url = copy_apple_https_string(handle.value, 3);
    if (!reason) return std::unexpected(reason.error());
    if (!headers) return std::unexpected(headers.error());
    if (!final_url) return std::unexpected(final_url.error());
    response.reason = reason->value_or(std::string {});
    response.headers = parse_apple_https_headers(
        headers->value_or(std::string {}));
    response.final_url = request.url;
    if (final_url->has_value() && !(**final_url).empty()) {
        auto parsed = Url::parse(**final_url);
        if (parsed) response.final_url = std::move(*parsed);
    }

    const i32 body_length = phoneme_ios_https_copy_body(handle.value,
                                                        nullptr, 0);
    if (body_length < 0) {
        return fail(ErrorCode::io_error,
                    "HTTPS bridge could not report response body length");
    }
    response.body.resize(static_cast<usize>(body_length));
    if (body_length > 0) {
        const i32 copied = phoneme_ios_https_copy_body(
            handle.value, response.body.data(), body_length);
        if (copied != body_length) {
            return fail(ErrorCode::io_error,
                        "HTTPS bridge could not copy response body");
        }
    }

    auto protocol_name = copy_apple_https_string(handle.value, 4);
    auto protocol_version = copy_apple_https_string(handle.value, 5);
    auto cipher_suite = copy_apple_https_string(handle.value, 6);
    auto certificate_subject = copy_apple_https_string(handle.value, 7);
    auto certificate_issuer = copy_apple_https_string(handle.value, 8);
    auto certificate_serial = copy_apple_https_string(handle.value, 9);
    if (!protocol_name) return std::unexpected(protocol_name.error());
    if (!protocol_version) return std::unexpected(protocol_version.error());
    if (!cipher_suite) return std::unexpected(cipher_suite.error());
    if (!certificate_subject)
        return std::unexpected(certificate_subject.error());
    if (!certificate_issuer)
        return std::unexpected(certificate_issuer.error());
    if (!certificate_serial)
        return std::unexpected(certificate_serial.error());
    if (protocol_name->has_value() || protocol_version->has_value() ||
        cipher_suite->has_value() || certificate_subject->has_value() ||
        certificate_issuer->has_value() || certificate_serial->has_value()) {
        response.security = SecurityMetadata {
            .protocol_name = protocol_name->value_or(std::string {}),
            .protocol_version = protocol_version->value_or(std::string {}),
            .cipher_suite = cipher_suite->value_or(std::string {}),
            .certificate_subject = certificate_subject->value_or(
                std::string {}),
            .certificate_issuer = certificate_issuer->value_or(
                std::string {}),
            .certificate_serial = certificate_serial->value_or(
                std::string {}),
            .certificate_not_before = phoneme_ios_https_get_long(
                handle.value, 1),
            .certificate_not_after = phoneme_ios_https_get_long(
                handle.value, 2),
        };
    }
    return response;
}

struct AppleHttpOperationState final {
    std::atomic_bool cancelled {false};
    std::atomic<i32> bridge_handle {0};
};

struct AppleHttpOperationRegistry final {
    std::mutex mutex;
    std::unordered_map<u64, std::shared_ptr<AppleHttpOperationState>> operations;
};

struct AppleHttpCallbackContext final {
    std::weak_ptr<AppleHttpOperationRegistry> registry;
    std::shared_ptr<AppleHttpOperationState> state;
    OperationId operation;
    HttpRequest request;
    Completion<HttpResponse> completion;
};

extern "C" void phoneme_apple_http_completed(i32 handle, void* opaque) {
    std::unique_ptr<AppleHttpCallbackContext> context(
        static_cast<AppleHttpCallbackContext*>(opaque));
    if (!context) {
        phoneme_ios_https_close(handle);
        return;
    }

    if (context->state->cancelled.load(std::memory_order_acquire)) {
        phoneme_ios_https_close(handle);
        return;
    }
    auto response = collect_apple_http_response(handle, context->request);

    bool deliver = false;
    if (auto registry = context->registry.lock()) {
        std::scoped_lock lock(registry->mutex);
        const auto found = registry->operations.find(context->operation.value);
        if (found != registry->operations.end() &&
            found->second == context->state &&
            !context->state->cancelled.load(std::memory_order_acquire)) {
            registry->operations.erase(found);
            deliver = true;
        }
    }
    if (deliver && context->completion) {
        context->completion(std::move(response));
    }
}
#endif

[[nodiscard]] Result<HttpResponse> perform_plain_http_with_deadline(
    HttpRequest request,
    const CancellationCheck& cancelled,
    const OperationDeadline& deadline) {
    if (request.url.scheme == Scheme::https) {
        return fail(ErrorCode::unsupported_feature,
                    "HTTPS requires an asynchronous platform TLS adapter");
    }
    if (request.url.scheme != Scheme::http) {
        return fail(ErrorCode::invalid_argument,
                    "HTTP adapter received a non-HTTP URL");
    }

    auto descriptor = connect_stream_descriptor(
        request.url, deadline, cancelled);
    if (!descriptor) return std::unexpected(descriptor.error());
    const int socket = *descriptor;

    std::string request_text = request.method + " " +
                               request.url.request_target() +
                               " HTTP/1.1\r\n";
    bool content_length_requested = false;
    for (const auto& [name, value] : request.headers) {
        const std::string normalized = lowercase(name);
        if (normalized == "content-length") {
            content_length_requested = true;
            continue;
        }
        if (normalized == "host" || normalized == "connection" ||
            normalized == "transfer-encoding") {
            continue;
        }
        request_text.append(name);
        request_text.append(": ");
        request_text.append(value);
        request_text.append("\r\n");
    }
    request_text.append("Host: ");
    request_text.append(request.url.authority());
    request_text.append("\r\n");
    request_text.append("Connection: close\r\n");
    if (!request.body.empty() || request.method == "POST" ||
        content_length_requested) {
        request_text.append("Content-Length: ");
        request_text.append(std::to_string(request.body.size()));
        request_text.append("\r\n");
    }
    request_text.append("\r\n");

    std::vector<u8> wire;
    wire.reserve(request_text.size() + request.body.size());
    wire.insert(wire.end(), request_text.begin(), request_text.end());
    wire.insert(wire.end(), request.body.begin(), request.body.end());
    auto sent = send_all(socket, wire, deadline, cancelled);
    if (!sent) {
        ::close(socket);
        return std::unexpected(sent.error());
    }
    auto received = receive_http_response(
        socket, request.method, deadline, cancelled);
    ::close(socket);
    if (!received) return std::unexpected(received.error());

    auto response = parse_http_response(
        request.url, request.method, *received);
    if (!response) return std::unexpected(response.error());
    const bool redirect = response->status_code == 301 ||
                          response->status_code == 302 ||
                          response->status_code == 303 ||
                          response->status_code == 307 ||
                          response->status_code == 308;
    if (!redirect || request.redirect_limit == 0U) return response;
    auto location = header_value(response->headers, "Location");
    if (!location.has_value()) return response;
    auto redirected = Url::resolve(request.url, *location);
    if (!redirected) return std::unexpected(redirected.error());
    const bool authority_changed =
        redirected->scheme != request.url.scheme ||
        lowercase(redirected->host) != lowercase(request.url.host) ||
        redirected->effective_port() != request.url.effective_port();
    if (authority_changed) {
        std::erase_if(request.headers, [](const Header& header) {
            const std::string name = lowercase(header.first);
            return name == "authorization" ||
                   name == "proxy-authorization" ||
                   name == "cookie";
        });
    }
    request.url = std::move(*redirected);
    --request.redirect_limit;
    if (response->status_code == 303 ||
        ((response->status_code == 301 || response->status_code == 302) &&
         request.method == "POST")) {
        request.method = "GET";
        request.body.clear();
    }
    return perform_plain_http_with_deadline(
        std::move(request), cancelled, deadline);
}

[[nodiscard]] Result<HttpResponse> perform_plain_http(
    HttpRequest request,
    const CancellationCheck& cancelled = {}) {
    OperationDeadline deadline(request.timeout_ms);
    return perform_plain_http_with_deadline(
        std::move(request), cancelled, deadline);
}

class PosixNetworkAdapter final : public AsyncNetworkAdapter {
public:
    PosixNetworkAdapter() {
        workers_.reserve(kMaximumNetworkWorkerCount);
        for (usize index = 0; index < kInitialNetworkWorkerCount; ++index) {
            spawn_worker_unlocked();
        }
    }

    ~PosixNetworkAdapter() override {
#if defined(__APPLE__)
        std::vector<i32> apple_handles;
        {
            std::scoped_lock lock(apple_http_operations_->mutex);
            apple_handles.reserve(apple_http_operations_->operations.size());
            for (const auto& [id, state] :
                 apple_http_operations_->operations) {
                (void)id;
                state->cancelled.store(true, std::memory_order_release);
                const i32 handle = state->bridge_handle.load(
                    std::memory_order_acquire);
                if (handle > 0) apple_handles.push_back(handle);
            }
            apple_http_operations_->operations.clear();
        }
        for (i32 handle : apple_handles) phoneme_ios_https_cancel(handle);
        phoneme_ios_https_clear_session(apple_http_session_id_);
#endif
        shutdown_workers();
        std::unordered_map<u64, std::shared_ptr<HandleState>> handles;
        {
            std::scoped_lock lock(mutex_);
            handles.swap(handles_);
        }
        for (const auto& [unused, state] : handles) {
            (void)unused;
            if (state != nullptr && state->descriptor >= 0) {
                ::close(state->descriptor);
                state->descriptor = -1;
            }
        }
    }

    [[nodiscard]] usize worker_count_for_tests() const noexcept override {
        return diagnostics().workers;
    }

    [[nodiscard]] NetworkAdapterDiagnostics diagnostics() const noexcept override {
        std::scoped_lock lock(operation_mutex_);
        return NetworkAdapterDiagnostics {
            .workers = workers_.size(),
            .active_workers = active_workers_,
            .active_blocking_workers = active_blocking_workers_,
            .queued_tasks = latency_sensitive_queue_.size() +
                            blocking_input_queue_.size(),
            .operations = operations_.size(),
        };
    }

    Result<OperationId> open_stream(
        const Url& url,
        i32 timeout_ms,
        Completion<NativeConnection> completion) override {
        return start_async_with_cleanup<NativeConnection>(
            NetworkTaskClass::latency_sensitive,
            std::move(completion),
            [this, url, timeout_ms](const CancellationCheck& cancelled)
                -> Result<NativeConnection> {
                OperationDeadline deadline(timeout_ms);
                auto descriptor = connect_stream_descriptor(
                    url, deadline, cancelled);
                if (!descriptor) return std::unexpected(descriptor.error());
                ScopedDescriptor owned(*descriptor);
                auto connection = store_connection(owned.value, true);
                if (!connection) return std::unexpected(connection.error());
                if (std::getenv("PHONEME_NETWORK_REDIRECT_1080_HOST") !=
                    nullptr) {
                    connection->remote = Endpoint {
                        .host = url.host,
                        .port = url.effective_port(),
                    };
                }
                (void)owned.release();
                return std::move(*connection);
            },
            [this](Result<NativeConnection>& result) {
                if (result) (void)close(result->handle);
            });
    }

    Result<OperationId> open_server(
        const Url& url,
        i32 timeout_ms,
        Completion<NativeConnection> completion) override {
        return start_async_with_cleanup<NativeConnection>(
            NetworkTaskClass::latency_sensitive,
            std::move(completion),
            [this, url, timeout_ms](const CancellationCheck& cancelled)
                -> Result<NativeConnection> {
                OperationDeadline deadline(timeout_ms);
                auto descriptor = create_server_descriptor(
                    url, deadline, cancelled);
                if (!descriptor) return std::unexpected(descriptor.error());
                ScopedDescriptor owned(*descriptor);
                if (cancellation_requested(cancelled)) {
                    return cancelled_operation();
                }
                auto connection = store_connection(owned.value, false);
                if (!connection) return std::unexpected(connection.error());
                (void)owned.release();
                return std::move(*connection);
            },
            [this](Result<NativeConnection>& result) {
                if (result) (void)close(result->handle);
            });
    }

    Result<OperationId> accept(
        NativeHandle server,
        i32 timeout_ms,
        Completion<NativeConnection> completion) override {
        return start_async_with_cleanup<NativeConnection>(
            NetworkTaskClass::blocking_input,
            std::move(completion),
            [this, server, timeout_ms](const CancellationCheck& cancelled)
                -> Result<NativeConnection> {
                OperationDeadline deadline(timeout_ms);
                auto descriptor = duplicate_handle(server);
                if (!descriptor) return std::unexpected(descriptor.error());
                auto gate = acquire_operation_gate(
                    descriptor->state->input_gate, deadline, cancelled);
                if (!gate) return std::unexpected(gate.error());
                while (true) {
                    auto ready = wait_fd(descriptor->descriptor.value, POLLIN,
                                         deadline, cancelled);
                    if (!ready) return std::unexpected(ready.error());
                    sockaddr_storage address {};
                    socklen_t length = static_cast<socklen_t>(sizeof(address));
                    const int accepted = ::accept(
                        descriptor->descriptor.value,
                        reinterpret_cast<sockaddr*>(&address), &length);
                    if (accepted < 0) {
                        if (errno == EINTR || errno == EAGAIN ||
                            errno == EWOULDBLOCK) {
                            continue;
                        }
                        return io_failure("accept failed");
                    }
                    ScopedDescriptor owned(accepted);
                    configure_stream_descriptor(owned.value);
                    auto nonblocking = set_nonblocking(owned.value, true);
                    if (!nonblocking) {
                        return std::unexpected(nonblocking.error());
                    }
                    if (cancellation_requested(cancelled)) {
                        return cancelled_operation();
                    }
                    auto connection = store_connection(owned.value, true);
                    if (!connection) {
                        return std::unexpected(connection.error());
                    }
                    (void)owned.release();
                    return std::move(*connection);
                }
            },
            [this](Result<NativeConnection>& result) {
                if (result) (void)close(result->handle);
            });
    }

    Result<OperationId> open_datagram(
        const Url& url,
        i32 timeout_ms,
        Completion<NativeConnection> completion) override {
        return start_async_with_cleanup<NativeConnection>(
            NetworkTaskClass::latency_sensitive,
            std::move(completion),
            [this, url, timeout_ms](const CancellationCheck& cancelled)
                -> Result<NativeConnection> {
                OperationDeadline deadline(timeout_ms);
                auto descriptor = create_datagram_descriptor(
                    url, deadline, cancelled);
                if (!descriptor) return std::unexpected(descriptor.error());
                ScopedDescriptor owned(*descriptor);
                if (cancellation_requested(cancelled)) {
                    return cancelled_operation();
                }
                auto connection = store_connection(
                    owned.value, !url.host.empty());
                if (!connection) return std::unexpected(connection.error());
                (void)owned.release();
                return std::move(*connection);
            },
            [this](Result<NativeConnection>& result) {
                if (result) (void)close(result->handle);
            });
    }

    Result<OperationId> read(
        NativeHandle handle,
        usize maximum_bytes,
        i32 timeout_ms,
        Completion<std::vector<u8>> completion) override {
        return start_async<std::vector<u8>>(
            NetworkTaskClass::blocking_input,
            std::move(completion),
            [this, handle, maximum_bytes, timeout_ms](
                const CancellationCheck& cancelled)
                -> Result<std::vector<u8>> {
                if (maximum_bytes == 0U) return std::vector<u8> {};
                OperationDeadline deadline(timeout_ms);
                auto descriptor = duplicate_handle(handle);
                if (!descriptor) return std::unexpected(descriptor.error());
                auto gate = acquire_operation_gate(
                    descriptor->state->input_gate, deadline, cancelled);
                if (!gate) return std::unexpected(gate.error());
                std::vector<u8> bytes;
                while (true) {
                    trace_network_wait("read-wait",
                                       descriptor->descriptor.value,
                                       maximum_bytes);
                    auto ready = wait_fd(descriptor->descriptor.value, POLLIN,
                                         deadline, cancelled);
                    if (!ready) return std::unexpected(ready.error());

                    int queued = 0;
                    if (::ioctl(descriptor->descriptor.value,
                                FIONREAD,
                                &queued) != 0) {
                        return io_failure("FIONREAD failed before socket read");
                    }

                    // Match phoneME's BSD PCSL implementation: pass the full
                    // Java-requested length to recv instead of imposing an
                    // artificial 16 KiB transport ceiling. FIONREAD keeps the
                    // temporary allocation bounded by bytes already queued in
                    // the kernel, while a one-byte fallback still detects EOF.
                    trace_network_wait("read-ready",
                                       descriptor->descriptor.value,
                                       maximum_bytes,
                                       static_cast<usize>(std::max(queued, 0)));
                    const usize initial_capacity = queued > 0
                        ? std::min(maximum_bytes,
                                   static_cast<usize>(queued))
                        : 1U;
                    bytes.resize(initial_capacity);
                    const ssize_t count = ::recv(
                        descriptor->descriptor.value, bytes.data(),
                        bytes.size(), 0);
                    if (count > 0) {
                        bytes.resize(static_cast<usize>(count));

                        // Drain only bytes that are already readable. This
                        // preserves InputStream's partial-read contract while
                        // avoiding one worker dispatch and one Java/native copy
                        // per small TCP segment during large payload bursts.
                        while (bytes.size() < maximum_bytes) {
                            int additional = 0;
                            if (::ioctl(descriptor->descriptor.value,
                                        FIONREAD,
                                        &additional) != 0) {
                                return io_failure(
                                    "FIONREAD failed while draining socket read");
                            }
                            if (additional <= 0) break;

                            const usize previous_size = bytes.size();
                            const usize amount = std::min(
                                maximum_bytes - previous_size,
                                static_cast<usize>(additional));
                            bytes.resize(previous_size + amount);
                            const ssize_t drained = ::recv(
                                descriptor->descriptor.value,
                                bytes.data() + previous_size,
                                amount,
                                0);
                            if (drained > 0) {
                                bytes.resize(previous_size +
                                             static_cast<usize>(drained));
                                continue;
                            }
                            bytes.resize(previous_size);
                            if (drained == 0 || errno == EAGAIN ||
                                errno == EWOULDBLOCK) {
                                break;
                            }
                            if (errno == EINTR) continue;
                            return io_failure("socket read drain failed");
                        }

                        trace_network_bytes("recv",
                                            descriptor->descriptor.value,
                                            bytes);
                        return bytes;
                    }
                    if (count == 0) {
                        bytes.clear();
                        trace_network_bytes("recv",
                                            descriptor->descriptor.value,
                                            bytes);
                        return bytes;
                    }
                    if (errno == EINTR || errno == EAGAIN ||
                        errno == EWOULDBLOCK) {
                        continue;
                    }
                    return io_failure("socket read failed");
                }
            });
    }

    Result<OperationId> write(
        NativeHandle handle,
        std::vector<u8> bytes,
        i32 timeout_ms,
        Completion<usize> completion) override {
        return start_async<usize>(
            NetworkTaskClass::latency_sensitive,
            std::move(completion),
            [this, handle, bytes = std::move(bytes), timeout_ms](
                const CancellationCheck& cancelled) -> Result<usize> {
                OperationDeadline deadline(timeout_ms);
                auto descriptor = duplicate_handle(handle);
                if (!descriptor) return std::unexpected(descriptor.error());
                auto gate = acquire_operation_gate(
                    descriptor->state->output_gate, deadline, cancelled);
                if (!gate) return std::unexpected(gate.error());
                trace_network_bytes("send", descriptor->descriptor.value,
                                    bytes);
                auto written = send_all(descriptor->descriptor.value, bytes,
                                        deadline, cancelled);
                if (!written) return std::unexpected(written.error());
                return bytes.size();
            });
    }

    Result<OperationId> available(
        NativeHandle handle,
        Completion<usize> completion) override {
        return start_async<usize>(
            NetworkTaskClass::latency_sensitive,
            std::move(completion),
            [this, handle](const CancellationCheck& cancelled)
                -> Result<usize> {
                if (cancellation_requested(cancelled)) {
                    return cancelled_operation();
                }
                auto descriptor = duplicate_handle(handle);
                if (!descriptor) return std::unexpected(descriptor.error());
                int count = 0;
                if (::ioctl(descriptor->descriptor.value, FIONREAD, &count) != 0) {
                    return io_failure("FIONREAD failed");
                }
                return static_cast<usize>(std::max(count, 0));
            });
    }

    Result<OperationId> send_datagram(
        NativeHandle handle,
        DatagramPacket packet,
        i32 timeout_ms,
        Completion<usize> completion) override {
        return start_async<usize>(
            NetworkTaskClass::latency_sensitive,
            std::move(completion),
            [this, handle, packet = std::move(packet), timeout_ms](
                const CancellationCheck& cancelled) mutable -> Result<usize> {
                OperationDeadline deadline(timeout_ms);
                auto descriptor = duplicate_handle(handle);
                if (!descriptor) return std::unexpected(descriptor.error());
                auto gate = acquire_operation_gate(
                    descriptor->state->output_gate, deadline, cancelled);
                if (!gate) return std::unexpected(gate.error());
                std::optional<AddressList> addresses;
                if (!packet.peer.host.empty()) {
                    auto resolved = resolve_addresses(
                        packet.peer.host, packet.peer.port, SOCK_DGRAM, false,
                        deadline, cancelled);
                    if (!resolved) return std::unexpected(resolved.error());
                    addresses.emplace(std::move(*resolved));
                }
                while (true) {
                    auto ready = wait_fd(descriptor->descriptor.value,
                                         POLLOUT, deadline, cancelled);
                    if (!ready) return std::unexpected(ready.error());
                    ssize_t sent = -1;
                    if (!addresses.has_value()) {
                        sent = ::send(descriptor->descriptor.value,
                                      packet.bytes.data(),
                                      packet.bytes.size(), 0);
                    } else {
                        for (addrinfo* address = addresses->value;
                             address != nullptr;
                             address = address->ai_next) {
                            auto active = check_operation_active(
                                deadline, cancelled);
                            if (!active) {
                                return std::unexpected(active.error());
                            }
                            sent = ::sendto(
                                descriptor->descriptor.value,
                                packet.bytes.data(), packet.bytes.size(), 0,
                                address->ai_addr,
                                static_cast<socklen_t>(address->ai_addrlen));
                            if (sent >= 0 ||
                                (errno != EAGAIN && errno != EWOULDBLOCK &&
                                 errno != EINTR)) {
                                break;
                            }
                        }
                    }
                    if (sent >= 0) return static_cast<usize>(sent);
                    if (errno == EINTR || errno == EAGAIN ||
                        errno == EWOULDBLOCK) {
                        continue;
                    }
                    return io_failure("UDP send failed");
                }
            });
    }

    Result<OperationId> receive_datagram(
        NativeHandle handle,
        usize maximum_bytes,
        i32 timeout_ms,
        Completion<DatagramPacket> completion) override {
        return start_async<DatagramPacket>(
            NetworkTaskClass::blocking_input,
            std::move(completion),
            [this, handle, maximum_bytes, timeout_ms](
                const CancellationCheck& cancelled) -> Result<DatagramPacket> {
                OperationDeadline deadline(timeout_ms);
                auto descriptor = duplicate_handle(handle);
                if (!descriptor) return std::unexpected(descriptor.error());
                auto gate = acquire_operation_gate(
                    descriptor->state->input_gate, deadline, cancelled);
                if (!gate) return std::unexpected(gate.error());
                while (true) {
                    auto ready = wait_fd(descriptor->descriptor.value,
                                         POLLIN, deadline, cancelled);
                    if (!ready) return std::unexpected(ready.error());
                    DatagramPacket packet;
                    packet.bytes.resize(std::min(
                        maximum_bytes, static_cast<usize>(65'507)));
                    sockaddr_storage peer {};
                    socklen_t peer_length =
                        static_cast<socklen_t>(sizeof(peer));
                    const ssize_t count = ::recvfrom(
                        descriptor->descriptor.value, packet.bytes.data(),
                        packet.bytes.size(), 0,
                        reinterpret_cast<sockaddr*>(&peer), &peer_length);
                    if (count < 0) {
                        if (errno == EINTR || errno == EAGAIN ||
                            errno == EWOULDBLOCK) {
                            continue;
                        }
                        return io_failure("UDP receive failed");
                    }
                    packet.bytes.resize(static_cast<usize>(count));
                    auto endpoint = endpoint_from_sockaddr(
                        reinterpret_cast<const sockaddr*>(&peer), peer_length);
                    if (!endpoint) return std::unexpected(endpoint.error());
                    packet.peer = std::move(*endpoint);
                    return packet;
                }
            });
    }

    Result<OperationId> perform_http(
        HttpRequest request,
        Completion<HttpResponse> completion) override {
#if defined(PHONEME_WEB)
        const OperationId operation {next_operation_.fetch_add(1U)};
        completion(perform_browser_http(std::move(request)));
        return operation;
#elif defined(__APPLE__)
        if (request.url.scheme == Scheme::http ||
            request.url.scheme == Scheme::https) {
            if (request.body.size() >
                static_cast<usize>(std::numeric_limits<i32>::max())) {
                return fail(ErrorCode::overflow,
                            "HTTP request body exceeds Apple bridge limit");
            }
            const OperationId operation {next_operation_.fetch_add(1U)};
            auto state = std::make_shared<AppleHttpOperationState>();
            {
                std::scoped_lock lock(apple_http_operations_->mutex);
                apple_http_operations_->operations.insert_or_assign(
                    operation.value, state);
            }
            auto* context = new AppleHttpCallbackContext {
                .registry = apple_http_operations_,
                .state = state,
                .operation = operation,
                .request = request,
                .completion = std::move(completion),
            };
            const std::string url = request.url.to_string();
            const std::string headers = serialize_apple_http_headers(request);
            const i32 handle = phoneme_ios_https_execute_async(
                url.c_str(), request.method.c_str(), headers.c_str(),
                request.body.empty() ? nullptr : request.body.data(),
                static_cast<i32>(request.body.size()), request.timeout_ms,
                static_cast<i32>(request.redirect_limit),
                apple_http_session_id_, &phoneme_apple_http_completed,
                context);
            if (handle <= 0) {
                {
                    std::scoped_lock lock(apple_http_operations_->mutex);
                    apple_http_operations_->operations.erase(operation.value);
                }
                delete context;
                return fail(ErrorCode::io_error,
                            "Apple HTTP bridge failed to start request");
            }
            state->bridge_handle.store(handle, std::memory_order_release);
            if (state->cancelled.load(std::memory_order_acquire)) {
                phoneme_ios_https_cancel(handle);
            }
            return operation;
        }
#endif
        return start_async<HttpResponse>(
            NetworkTaskClass::latency_sensitive,
            std::move(completion),
            [request = std::move(request)](
                const CancellationCheck& cancelled) mutable
                -> Result<HttpResponse> {
                return perform_plain_http(std::move(request), cancelled);
            });
    }

    Result<OperationId> set_socket_option(
        NativeHandle handle,
        SocketOption option,
        i32 value,
        Completion<bool> completion) override {
        if ((option == SocketOption::receive_buffer ||
             option == SocketOption::send_buffer) && value <= 0) {
            return fail(ErrorCode::invalid_argument,
                        "socket buffer size must be positive");
        }
#if defined(PHONEME_WEB)
        // Browser WebSocket/SOCKFS does not expose TCP_NODELAY, SO_LINGER,
        // SO_KEEPALIVE or native buffer sizing. MIDP applications commonly set
        // these options during startup, so preserve their requested values and
        // report success instead of failing with ENOPROTOOPT.
        const OperationId operation {next_operation_.fetch_add(1U)};
        {
            std::scoped_lock lock(mutex_);
            const auto found = handles_.find(handle.value);
            if (found == handles_.end() || found->second == nullptr ||
                found->second->descriptor < 0) {
                return fail(ErrorCode::invalid_state,
                            "native network handle is closed");
            }
            i32 stored = value;
            if (option == SocketOption::delay ||
                option == SocketOption::keep_alive) {
                stored = value == 0 ? 0 : 1;
            } else if (option == SocketOption::linger) {
                stored = std::max(value, 0);
            }
            found->second->socket_options[static_cast<usize>(option)] = stored;
        }
        completion(true);
        return operation;
#else
        return start_async<bool>(
            NetworkTaskClass::latency_sensitive,
            std::move(completion),
            [this, handle, option, value](
                const CancellationCheck& cancelled) -> Result<bool> {
                if (cancellation_requested(cancelled)) {
                    return cancelled_operation();
                }
                auto descriptor = duplicate_handle(handle);
                if (!descriptor) return std::unexpected(descriptor.error());
                int level = SOL_SOCKET;
                int name = 0;
                int integer = value;
                linger linger_value {};
                const void* pointer = &integer;
                socklen_t length = static_cast<socklen_t>(sizeof(integer));
                switch (option) {
                case SocketOption::delay:
                    level = IPPROTO_TCP;
                    name = TCP_NODELAY;
                    // Java ME exposes DELAY, while POSIX exposes its inverse.
                    integer = value == 0 ? 1 : 0;
                    break;
                case SocketOption::linger:
                    name = SO_LINGER;
                    linger_value.l_onoff = value > 0 ? 1 : 0;
                    linger_value.l_linger = std::max(value, 0);
                    pointer = &linger_value;
                    length = static_cast<socklen_t>(sizeof(linger_value));
                    break;
                case SocketOption::keep_alive:
                    name = SO_KEEPALIVE;
                    integer = value == 0 ? 0 : 1;
                    break;
                case SocketOption::receive_buffer:
                    name = SO_RCVBUF;
                    break;
                case SocketOption::send_buffer:
                    name = SO_SNDBUF;
                    break;
                }
                if (::setsockopt(descriptor->descriptor.value, level, name,
                                 pointer, length) != 0) {
                    return io_failure("setsockopt failed");
                }
                return true;
            });
#endif
    }

    Result<OperationId> get_socket_option(
        NativeHandle handle,
        SocketOption option,
        Completion<i32> completion) override {
#if defined(PHONEME_WEB)
        const OperationId operation {next_operation_.fetch_add(1U)};
        i32 value = 0;
        {
            std::scoped_lock lock(mutex_);
            const auto found = handles_.find(handle.value);
            if (found == handles_.end() || found->second == nullptr ||
                found->second->descriptor < 0) {
                return fail(ErrorCode::invalid_state,
                            "native network handle is closed");
            }
            value = found->second->socket_options[static_cast<usize>(option)];
        }
        completion(value);
        return operation;
#else
        return start_async<i32>(
            NetworkTaskClass::latency_sensitive,
            std::move(completion),
            [this, handle, option](const CancellationCheck& cancelled)
                -> Result<i32> {
                if (cancellation_requested(cancelled)) {
                    return cancelled_operation();
                }
                auto descriptor = duplicate_handle(handle);
                if (!descriptor) return std::unexpected(descriptor.error());
                int level = SOL_SOCKET;
                int name = 0;
                int integer = 0;
                linger linger_value {};
                void* pointer = &integer;
                socklen_t length = static_cast<socklen_t>(sizeof(integer));
                switch (option) {
                case SocketOption::delay:
                    level = IPPROTO_TCP;
                    name = TCP_NODELAY;
                    break;
                case SocketOption::linger:
                    name = SO_LINGER;
                    pointer = &linger_value;
                    length = static_cast<socklen_t>(sizeof(linger_value));
                    break;
                case SocketOption::keep_alive:
                    name = SO_KEEPALIVE;
                    break;
                case SocketOption::receive_buffer:
                    name = SO_RCVBUF;
                    break;
                case SocketOption::send_buffer:
                    name = SO_SNDBUF;
                    break;
                }
                if (::getsockopt(descriptor->descriptor.value, level, name,
                                 pointer, &length) != 0) {
                    return io_failure("getsockopt failed");
                }
                if (option == SocketOption::linger) {
                    integer = linger_value.l_onoff == 0
                        ? 0 : linger_value.l_linger;
                } else if (option == SocketOption::delay) {
                    // Convert TCP_NODELAY back to Java ME DELAY semantics.
                    integer = integer == 0 ? 1 : 0;
                }
                return static_cast<i32>(integer);
            });
#endif
    }

    Status shutdown_output(NativeHandle handle) override {
        if (!handle.valid()) {
            return fail(ErrorCode::invalid_argument,
                        "native network handle is invalid");
        }
        std::scoped_lock lock(mutex_);
        const auto found = handles_.find(handle.value);
        if (found == handles_.end()) {
            return fail(ErrorCode::invalid_state,
                        "native network handle is closed");
        }
        // phoneME treats output shutdown as best-effort: it must send FIN when
        // possible, but a peer that already closed must not make stream close
        // fail.
        (void)::shutdown(found->second->descriptor, SHUT_WR);
        return {};
    }

    Status close(NativeHandle handle) override {
        std::shared_ptr<HandleState> state;
        {
            std::scoped_lock lock(mutex_);
            const auto found = handles_.find(handle.value);
            if (found == handles_.end()) return {};
            state = std::move(found->second);
            handles_.erase(found);
        }
        if (state != nullptr && state->descriptor >= 0) {
            (void)::shutdown(state->descriptor, SHUT_RDWR);
            ::close(state->descriptor);
            state->descriptor = -1;
        }
        return {};
    }

    Status cancel(OperationId operation) override {
        {
            std::scoped_lock lock(operation_mutex_);
            const auto found = operations_.find(operation.value);
            if (found != operations_.end()) {
                found->second->signal_cancel();
            }
        }
        operation_condition_.notify_all();
#if defined(__APPLE__)
        std::shared_ptr<AppleHttpOperationState> state;
        {
            std::scoped_lock lock(apple_http_operations_->mutex);
            const auto found =
                apple_http_operations_->operations.find(operation.value);
            if (found != apple_http_operations_->operations.end()) {
                state = found->second;
                apple_http_operations_->operations.erase(found);
            }
        }
        if (state != nullptr) {
            state->cancelled.store(true, std::memory_order_release);
            const i32 handle = state->bridge_handle.load(
                std::memory_order_acquire);
            if (handle > 0) phoneme_ios_https_cancel(handle);
        }
#endif
        return {};
    }

private:
    struct WorkerWake final {
        int read_descriptor {-1};
        int write_descriptor {-1};

        WorkerWake() noexcept {
#if !defined(PHONEME_WEB)
            int descriptors[2] {-1, -1};
            if (::pipe(descriptors) == 0) {
                read_descriptor = descriptors[0];
                write_descriptor = descriptors[1];
                const int read_flags = ::fcntl(read_descriptor, F_GETFL, 0);
                const int write_flags = ::fcntl(write_descriptor, F_GETFL, 0);
                if (read_flags >= 0) {
                    (void)::fcntl(read_descriptor, F_SETFL,
                                  read_flags | O_NONBLOCK);
                }
                if (write_flags >= 0) {
                    (void)::fcntl(write_descriptor, F_SETFL,
                                  write_flags | O_NONBLOCK);
                }
            }
#endif
        }

        ~WorkerWake() {
            if (read_descriptor >= 0) ::close(read_descriptor);
            if (write_descriptor >= 0) ::close(write_descriptor);
        }

        WorkerWake(const WorkerWake&) = delete;
        WorkerWake& operator=(const WorkerWake&) = delete;

        void drain() noexcept {
            if (read_descriptor < 0) return;
            std::array<u8, 64> bytes {};
            while (::read(read_descriptor, bytes.data(), bytes.size()) > 0) {
            }
        }

        void signal() noexcept {
            if (write_descriptor < 0) return;
            const u8 byte = 1U;
            const ssize_t ignored = ::write(write_descriptor, &byte, 1U);
            (void)ignored;
        }
    };

    struct OperationState final {
        std::atomic_bool cancelled {false};
        std::mutex worker_wake_mutex;
        std::weak_ptr<WorkerWake> worker_wake;

        void attach_worker(const std::shared_ptr<WorkerWake>& wake) noexcept {
            std::scoped_lock lock(worker_wake_mutex);
            worker_wake = wake;
        }

        void detach_worker() noexcept {
            std::scoped_lock lock(worker_wake_mutex);
            worker_wake.reset();
        }

        void signal_cancel() noexcept {
            const bool was_cancelled =
                cancelled.exchange(true, std::memory_order_acq_rel);
            if (was_cancelled) return;
            std::shared_ptr<WorkerWake> wake;
            {
                std::scoped_lock lock(worker_wake_mutex);
                wake = worker_wake.lock();
            }
            if (wake) wake->signal();
        }
    };

    using AsyncTask =
        std::function<void(const std::shared_ptr<WorkerWake>&)>;

    template <typename T, typename Work, typename Cleanup>
    [[nodiscard]] Result<OperationId> start_async_with_cleanup(
        NetworkTaskClass task_class,
        Completion<T> completion,
        Work work,
        Cleanup cleanup) {
        const OperationId operation {next_operation_.fetch_add(1U)};
        if (!operation.valid()) {
            return fail(ErrorCode::overflow,
                        "network operation identifier space was exhausted");
        }
        auto state = std::make_shared<OperationState>();
        AsyncTask task = [this,
                          operation,
                          state,
                          task_class,
                          completion = std::move(completion),
                          work = std::move(work),
                          cleanup = std::move(cleanup)](
                             const std::shared_ptr<WorkerWake>& worker_wake) mutable {
            if (worker_wake) worker_wake->drain();
            state->attach_worker(worker_wake);
            const CancellationCheck cancelled {
                .requested = [state] {
                    return state->cancelled.load(std::memory_order_acquire);
                },
                .wake_descriptor = worker_wake
                    ? worker_wake->read_descriptor
                    : -1,
            };
            Result<T> result = work(cancelled);
            state->detach_worker();
            bool deliver = false;
            {
                std::scoped_lock lock(operation_mutex_);
                const auto found = operations_.find(operation.value);
                if (found != operations_.end() && found->second == state) {
                    operations_.erase(found);
                }
                deliver = !stopping_ &&
                          !state->cancelled.load(std::memory_order_acquire);

                // The worker has finished its blocking/native work and is now
                // only delivering the callback. Java often submits the next
                // readByte() from that callback before this task returns. Mark
                // this worker as immediately reusable so the capacity check
                // does not mistake the handoff window for pool starvation and
                // spawn one permanent thread per byte read.
                ++completing_workers_;
                if (task_class == NetworkTaskClass::blocking_input) {
                    ++completing_blocking_workers_;
                }
            }
            if (!deliver) {
                cleanup(result);
                return;
            }
            if (completion) completion(std::move(result));
        };

        {
            std::scoped_lock lock(operation_mutex_);
            if (stopping_) {
                return fail(ErrorCode::invalid_state,
                            "network adapter is shutting down");
            }
            operations_.insert_or_assign(operation.value, state);
            auto& queue = task_class == NetworkTaskClass::blocking_input
                ? blocking_input_queue_ : latency_sensitive_queue_;
            queue.push_back(std::move(task));
            ensure_worker_capacity_unlocked();
        }
        operation_condition_.notify_all();
        return operation;
    }

    template <typename T, typename Work>
    [[nodiscard]] Result<OperationId> start_async(
        NetworkTaskClass task_class,
        Completion<T> completion,
        Work work) {
        return start_async_with_cleanup<T>(
            task_class, std::move(completion), std::move(work),
            [](Result<T>&) {});
    }

    void spawn_worker_unlocked() {
        auto wake = std::make_shared<WorkerWake>();
        workers_.emplace_back(
            [this, wake = std::move(wake)](std::stop_token stop_token) {
                worker_loop(stop_token, wake);
            });
    }

    [[nodiscard]] usize reserved_worker_count_unlocked() const noexcept {
        if (workers_.size() <= 1U) return 0U;
        return std::min(
            kMaximumReservedNetworkWorkers,
            std::max<usize>(1U, workers_.size() / 4U));
    }

    [[nodiscard]] bool can_run_blocking_input_unlocked() const noexcept {
        if (blocking_input_queue_.empty() || workers_.empty()) return false;
        const usize reserved = reserved_worker_count_unlocked();
        const usize capacity = workers_.size() > reserved
            ? workers_.size() - reserved : 1U;
        return active_blocking_workers_ < capacity;
    }

    [[nodiscard]] bool has_runnable_task_unlocked() const noexcept {
        return !latency_sensitive_queue_.empty() ||
               can_run_blocking_input_unlocked();
    }

    void ensure_worker_capacity_unlocked() {
        if (stopping_ || workers_.size() >= kMaximumNetworkWorkerCount) return;
        const usize effective_active_workers =
            active_workers_ > completing_workers_
                ? active_workers_ - completing_workers_ : 0U;
        const usize idle_workers = workers_.size() > effective_active_workers
            ? workers_.size() - effective_active_workers : 0U;
        const usize latency_runnable = std::min(
            latency_sensitive_queue_.size(), idle_workers);
        const usize idle_after_latency = idle_workers - latency_runnable;
        const usize reserved = reserved_worker_count_unlocked();
        const usize blocking_capacity = workers_.size() > reserved
            ? workers_.size() - reserved : 1U;
        const usize effective_active_blocking_workers =
            active_blocking_workers_ > completing_blocking_workers_
                ? active_blocking_workers_ - completing_blocking_workers_ : 0U;
        const usize blocking_slots =
            blocking_capacity > effective_active_blocking_workers
                ? blocking_capacity - effective_active_blocking_workers : 0U;
        const usize blocking_runnable = std::min(
            blocking_input_queue_.size(),
            std::min(idle_after_latency, blocking_slots));
        const usize queued_tasks = latency_sensitive_queue_.size() +
                                   blocking_input_queue_.size();
        const usize runnable_tasks = latency_runnable + blocking_runnable;
        if (queued_tasks <= runnable_tasks) return;
        const usize missing_workers = queued_tasks - runnable_tasks;
        const usize capacity = kMaximumNetworkWorkerCount - workers_.size();
        const usize workers_to_add = std::min(missing_workers, capacity);
        for (usize index = 0; index < workers_to_add; ++index) {
            spawn_worker_unlocked();
        }
    }

    void worker_loop(
        std::stop_token stop_token,
        const std::shared_ptr<WorkerWake>& worker_wake) noexcept {
        while (true) {
            AsyncTask task;
            bool blocking_input = false;
            {
                std::unique_lock lock(operation_mutex_);
                operation_condition_.wait(lock, [this, &stop_token] {
                    return stopping_ || stop_token.stop_requested() ||
                           has_runnable_task_unlocked();
                });
                const bool queues_empty = latency_sensitive_queue_.empty() &&
                                          blocking_input_queue_.empty();
                if ((stopping_ || stop_token.stop_requested()) &&
                    queues_empty) {
                    return;
                }
                if (!latency_sensitive_queue_.empty()) {
                    task = std::move(latency_sensitive_queue_.front());
                    latency_sensitive_queue_.pop_front();
                } else if (can_run_blocking_input_unlocked()) {
                    task = std::move(blocking_input_queue_.front());
                    blocking_input_queue_.pop_front();
                    blocking_input = true;
                    ++active_blocking_workers_;
                } else {
                    continue;
                }
                ++active_workers_;
            }
            if (task) task(worker_wake);
            {
                std::scoped_lock lock(operation_mutex_);
                if (active_workers_ != 0U) --active_workers_;
                if (completing_workers_ != 0U) --completing_workers_;
                if (blocking_input && active_blocking_workers_ != 0U) {
                    --active_blocking_workers_;
                }
                if (blocking_input && completing_blocking_workers_ != 0U) {
                    --completing_blocking_workers_;
                }
            }
            operation_condition_.notify_all();
        }
    }

    void shutdown_workers() noexcept {
        {
            std::scoped_lock lock(operation_mutex_);
            if (stopping_) return;
            stopping_ = true;
            for (auto& [unused, state] : operations_) {
                (void)unused;
                state->signal_cancel();
            }
            latency_sensitive_queue_.clear();
            blocking_input_queue_.clear();
        }
        for (auto& worker : workers_) worker.request_stop();
        operation_condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable() &&
                worker.get_id() != std::this_thread::get_id()) {
                worker.join();
            }
        }
        workers_.clear();
        std::scoped_lock lock(operation_mutex_);
        operations_.clear();
    }

    [[nodiscard]] Result<DuplicatedHandle> duplicate_handle(
        NativeHandle handle) const {
        if (!handle.valid()) {
            return fail(ErrorCode::invalid_argument,
                        "native network handle is invalid");
        }
        int descriptor = -1;
        std::shared_ptr<HandleState> state;
        {
            std::scoped_lock lock(mutex_);
            const auto found = handles_.find(handle.value);
            if (found == handles_.end() || found->second == nullptr ||
                found->second->descriptor < 0) {
                return fail(ErrorCode::invalid_state,
                            "native network handle is closed");
            }
            state = found->second;
#if defined(PHONEME_WEB)
            // SOCKFS readiness notifications are attached to the original fd.
            // A dup() can write through the shared socket but its poll wait is
            // not notified when a WebSocket message arrives, so reads can hang.
            descriptor = state->descriptor;
#else
            descriptor = ::dup(state->descriptor);
#endif
        }
#if defined(PHONEME_WEB)
        return DuplicatedHandle {
            .descriptor = ScopedDescriptor {descriptor, false},
            .state = std::move(state),
        };
#else
        if (descriptor < 0) return io_failure("dup network handle failed");
        configure_descriptor(descriptor);
        auto nonblocking = set_nonblocking(descriptor, true);
        if (!nonblocking) {
            ::close(descriptor);
            return std::unexpected(nonblocking.error());
        }
        return DuplicatedHandle {
            .descriptor = ScopedDescriptor {descriptor},
            .state = std::move(state),
        };
#endif
    }

    [[nodiscard]] Result<NativeConnection> store_connection(
        int descriptor,
        bool has_peer) {
        auto local = socket_endpoint(descriptor, false);
        if (!local) return std::unexpected(local.error());
        Endpoint remote;
        if (has_peer) {
            auto peer = socket_endpoint(descriptor, true);
            if (!peer) return std::unexpected(peer.error());
            remote = std::move(*peer);
        }
        const NativeHandle handle {next_handle_.fetch_add(1U)};
        auto state = std::make_shared<HandleState>(descriptor);
        {
            std::scoped_lock lock(mutex_);
            handles_.insert_or_assign(handle.value, std::move(state));
        }
        return NativeConnection {
            .handle = handle,
            .local = std::move(*local),
            .remote = std::move(remote),
        };
    }

#if defined(__APPLE__)
    const i64 apple_http_session_id_ {allocate_apple_http_session_id()};
    std::shared_ptr<AppleHttpOperationRegistry> apple_http_operations_ {
        std::make_shared<AppleHttpOperationRegistry>()};
#endif
    mutable std::mutex operation_mutex_;
    std::condition_variable operation_condition_;
    std::deque<AsyncTask> latency_sensitive_queue_;
    std::deque<AsyncTask> blocking_input_queue_;
    std::unordered_map<u64, std::shared_ptr<OperationState>> operations_;
    std::vector<std::jthread> workers_;
    usize active_workers_ {0U};
    usize active_blocking_workers_ {0U};
    usize completing_workers_ {0U};
    usize completing_blocking_workers_ {0U};
    bool stopping_ {false};

    mutable std::mutex mutex_;
    std::unordered_map<u64, std::shared_ptr<HandleState>> handles_;
    std::atomic<u64> next_handle_ {1U};
    std::atomic<u64> next_operation_ {1U};
};

} // namespace

namespace detail {

usize posix_network_worker_count_for_tests(
    const std::shared_ptr<AsyncNetworkAdapter>& adapter) noexcept {
    return adapter == nullptr ? 0U : adapter->worker_count_for_tests();
}

Result<HttpResponse> parse_http_response_bytes(
    const Url& url,
    std::string_view request_method,
    std::span<const u8> bytes) {
    return parse_http_response(url, request_method, bytes);
}

Result<HttpResponse> perform_plain_http_request(HttpRequest request) {
    return perform_plain_http(std::move(request));
}

void set_address_resolution_delay_for_tests(i32 milliseconds) noexcept {
    g_address_resolution_delay_for_tests.store(
        std::max(milliseconds, 0), std::memory_order_release);
}

} // namespace detail

std::shared_ptr<AsyncNetworkAdapter> make_posix_network_adapter() {
    return std::make_shared<PosixNetworkAdapter>();
}

} // namespace phoneme::network
