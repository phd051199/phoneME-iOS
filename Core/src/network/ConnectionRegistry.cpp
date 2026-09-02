#include "phoneme/network/ConnectionRegistry.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace phoneme::network {
namespace {

constexpr i32 kTimeoutEnabledMilliseconds = 30'000;
constexpr i32 kStreamTimeoutEnabledMilliseconds = 120'000;
constexpr i32 kTimeoutDisabledMilliseconds = 0;
constexpr usize kMaximumBufferedHttpBody = 16U * 1024U * 1024U;
constexpr usize kMaximumDatagramLength = 65'507U;
constexpr i32 kBlockingCancellationPollMilliseconds = 25;

[[nodiscard]] bool readable(ConnectionMode mode) noexcept {
    return mode == ConnectionMode::read ||
           mode == ConnectionMode::read_write;
}

[[nodiscard]] bool writable(ConnectionMode mode) noexcept {
    return mode == ConnectionMode::write ||
           mode == ConnectionMode::read_write;
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

[[nodiscard]] bool valid_http_token(std::string_view value) noexcept {
    if (value.empty()) return false;
    for (const char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte <= 32U || byte >= 127U || character == ':' ||
            character == '\r' || character == '\n') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_header_value(std::string_view value) noexcept {
    return value.find('\r') == std::string_view::npos &&
           value.find('\n') == std::string_view::npos;
}

class NetworkBlockingScope final {
public:
    explicit NetworkBlockingScope(const NetworkBlockingHooks& hooks)
        : after_(hooks.after_block) {
        if (hooks.before_block) depth_ = hooks.before_block();
    }

    ~NetworkBlockingScope() {
        if (after_) after_(depth_);
    }

    NetworkBlockingScope(const NetworkBlockingScope&) = delete;
    NetworkBlockingScope& operator=(const NetworkBlockingScope&) = delete;

private:
    std::function<void(u32)> after_;
    u32 depth_ {0};
};

template <typename T>
class WaitState final {
public:
    explicit WaitState(std::function<void()> wake_waiters = {})
        : wake_waiters_(std::move(wake_waiters)) {}

    [[nodiscard]] std::optional<Result<T>> complete(Result<T> value) {
        {
            std::scoped_lock lock(mutex_);
            if (completed_) return std::move(value);
            result_.emplace(std::move(value));
            completed_ = true;
        }
        condition_.notify_all();
        if (wake_waiters_) wake_waiters_();
        return std::nullopt;
    }

    void attach_operation(
        OperationId operation,
        const std::shared_ptr<AsyncNetworkAdapter>& adapter) {
        bool cancel_now = false;
        {
            std::scoped_lock lock(mutex_);
            operation_ = operation;
            cancel_now = cancel_requested_;
        }
        if (cancel_now && adapter) {
            (void)adapter->cancel(operation);
        }
    }

    void cancel(const std::shared_ptr<AsyncNetworkAdapter>& adapter,
                std::string message) {
        std::optional<OperationId> operation;
        {
            std::scoped_lock lock(mutex_);
            if (completed_) return;
            cancel_requested_ = true;
            completed_ = true;
            result_.emplace(std::unexpected(
                Error::make(ErrorCode::io_error, std::move(message))));
            operation = operation_;
        }
        condition_.notify_all();
        if (wake_waiters_) wake_waiters_();
        if (operation.has_value() && adapter) {
            (void)adapter->cancel(*operation);
        }
    }

    [[nodiscard]] std::optional<Result<T>> consume_if_completed() {
        std::scoped_lock lock(mutex_);
        if (!completed_ || !result_.has_value()) return std::nullopt;
        auto value = std::move(*result_);
        result_.reset();
        return value;
    }

    [[nodiscard]] Result<T> wait(
        i32 timeout_ms,
        const std::shared_ptr<AsyncNetworkAdapter>& adapter,
        const NetworkBlockingHooks& hooks) {
        std::optional<OperationId> operation;
        const auto started = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::milliseconds(
            std::max(timeout_ms, 0));
        std::unique_lock lock(mutex_);
        while (!completed_) {
            lock.unlock();
            std::optional<Error> cancellation;
            if (hooks.poll_cancellation) {
                cancellation = hooks.poll_cancellation();
            }
            lock.lock();
            if (completed_) break;
            if (cancellation.has_value()) {
                cancel_requested_ = true;
                completed_ = true;
                result_.emplace(std::unexpected(std::move(*cancellation)));
                operation = operation_;
                break;
            }

            auto wait_slice = std::chrono::milliseconds(
                kBlockingCancellationPollMilliseconds);
            if (timeout_ms > 0) {
                const auto elapsed =
                    std::chrono::steady_clock::now() - started;
                if (elapsed >= timeout) {
                    cancel_requested_ = true;
                    completed_ = true;
                    result_.emplace(std::unexpected(Error::make(
                        ErrorCode::io_error,
                        "network adapter operation timed out")));
                    operation = operation_;
                    break;
                }
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        timeout - elapsed);
                wait_slice = std::min(wait_slice, remaining);
            }
            if (hooks.cooperative_wait) {
                lock.unlock();
                const bool parked = hooks.cooperative_wait(wait_slice);
                lock.lock();
                if (!parked && !completed_) {
                    condition_.wait_for(lock, wait_slice,
                                        [this] { return completed_; });
                }
            } else {
                condition_.wait_for(lock, wait_slice,
                                    [this] { return completed_; });
            }
        }
        if (!result_.has_value()) {
            return fail(ErrorCode::internal_error,
                        "network adapter completed without a result");
        }
        auto value = std::move(*result_);
        result_.reset();
        lock.unlock();
        if (operation.has_value() && adapter) {
            (void)adapter->cancel(*operation);
        }
        return value;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool completed_ {false};
    bool cancel_requested_ {false};
    std::optional<OperationId> operation_;
    std::optional<Result<T>> result_;
    std::function<void()> wake_waiters_;
};

[[nodiscard]] std::optional<std::string> find_header(
    const std::vector<Header>& headers,
    std::string_view name) {
    const std::string normalized = lowercase(name);
    for (auto iterator = headers.rbegin(); iterator != headers.rend();
         ++iterator) {
        if (lowercase(iterator->first) == normalized) {
            return iterator->second;
        }
    }
    return std::nullopt;
}

void set_header(std::vector<Header>& headers,
                std::string name,
                std::string value) {
    const std::string normalized = lowercase(name);
    for (auto& [existing_name, existing_value] : headers) {
        if (lowercase(existing_name) == normalized) {
            existing_name = std::move(name);
            existing_value = std::move(value);
            return;
        }
    }
    headers.emplace_back(std::move(name), std::move(value));
}

} // namespace

template <typename T, typename Starter>
Result<T> ConnectionRegistry::await_operation(ConnectionToken token,
                                              PendingOperationKind kind,
                                              i32 timeout_ms,
                                              Starter&& starter) {
    std::shared_ptr<AsyncNetworkAdapter> adapter;
    NetworkBlockingHooks hooks;
    {
        std::scoped_lock lock(mutex_);
        adapter = adapter_;
        hooks = blocking_hooks_;
    }
    if (!adapter) {
        return fail(ErrorCode::not_configured,
                    "network adapter is not configured");
    }

    auto state = std::make_shared<WaitState<T>>(hooks.wake_waiters);
    const std::weak_ptr<AsyncNetworkAdapter> weak_adapter(adapter);
    auto registered = register_pending_operation(
        token, kind,
        [state, weak_adapter] {
            state->cancel(
                weak_adapter.lock(),
                "network operation was cancelled because the connection closed");
        });
    if (!registered) return std::unexpected(registered.error());
    const u64 pending_operation = *registered;

    if (auto cancelled = state->consume_if_completed();
        cancelled.has_value()) {
        unregister_pending_operation(token, pending_operation);
        return std::move(*cancelled);
    }

    NetworkBlockingScope blocking_scope(hooks);
    auto started = starter(
        adapter,
        Completion<T> {[state, weak_adapter](Result<T> result) {
            auto dropped = state->complete(std::move(result));
            if constexpr (std::is_same_v<T, NativeConnection>) {
                if (dropped.has_value() && dropped->has_value()) {
                    if (auto live_adapter = weak_adapter.lock()) {
                        (void)live_adapter->close((**dropped).handle);
                    }
                }
            }
        }});
    if (!started) {
        auto cancelled = state->consume_if_completed();
        unregister_pending_operation(token, pending_operation);
        if (cancelled.has_value()) return std::move(*cancelled);
        return std::unexpected(started.error());
    }

    state->attach_operation(*started, adapter);
    auto result = state->wait(timeout_ms, adapter, hooks);
    unregister_pending_operation(token, pending_operation);
    return result;
}

Result<u64> ConnectionRegistry::register_pending_operation(
    ConnectionToken token,
    PendingOperationKind kind,
    std::function<void()> cancellation) {
    if (!cancellation) {
        return fail(ErrorCode::invalid_argument,
                    "network pending-operation cancellation is empty");
    }
    std::scoped_lock lock(mutex_);
    auto found = mutable_entry(token);
    if (!found) return std::unexpected(found.error());
    if (next_pending_operation_ == 0U ||
        next_pending_operation_ == std::numeric_limits<u64>::max()) {
        return fail(ErrorCode::overflow,
                    "network pending-operation identifier space was exhausted");
    }
    const u64 operation = next_pending_operation_++;
    (*found)->pending_cancellations.insert_or_assign(
        operation,
        PendingCancellation {
            .kind = kind,
            .callback = std::move(cancellation),
        });
    return operation;
}

void ConnectionRegistry::unregister_pending_operation(
    ConnectionToken token,
    u64 operation) noexcept {
    std::scoped_lock lock(mutex_);
    auto found = entries_.find(token.id);
    if (found == entries_.end() || found->second.token != token) return;
    found->second.pending_cancellations.erase(operation);
}

ConnectionRegistry::ConnectionRegistry()
    : ConnectionRegistry(make_posix_network_adapter()) {}

ConnectionRegistry::ConnectionRegistry(
    std::shared_ptr<AsyncNetworkAdapter> adapter)
    : adapter_(std::move(adapter)) {}

ConnectionRegistry::~ConnectionRegistry() { close_all(); }

Status ConnectionRegistry::set_adapter(
    std::shared_ptr<AsyncNetworkAdapter> adapter) {
    if (!adapter) {
        return fail(ErrorCode::invalid_argument,
                    "network adapter is null");
    }
    std::scoped_lock lock(mutex_);
    if (!entries_.empty()) {
        return fail(ErrorCode::invalid_state,
                    "network adapter cannot change with open connections");
    }
    adapter_ = std::move(adapter);
    return {};
}

void ConnectionRegistry::set_blocking_hooks(NetworkBlockingHooks hooks) {
    std::scoped_lock lock(mutex_);
    blocking_hooks_ = std::move(hooks);
}

void ConnectionRegistry::set_owner(i32 owner) noexcept {
    std::scoped_lock lock(mutex_);
    owner_ = std::max(owner, 0);
}

i32 ConnectionRegistry::owner() const noexcept {
    std::scoped_lock lock(mutex_);
    return owner_;
}

ConnectionRegistryDiagnostics ConnectionRegistry::diagnostics() const noexcept {
    std::scoped_lock lock(mutex_);
    ConnectionRegistryDiagnostics result {
        .connections = entries_.size(),
        .adapter = adapter_ != nullptr
            ? adapter_->diagnostics()
            : NetworkAdapterDiagnostics {},
    };
    for (const auto& [id, entry] : entries_) {
        (void)id;
        result.pending_operations += entry.pending_cancellations.size();
    }
    return result;
}

ConnectionToken ConnectionRegistry::allocate_token_unlocked() {
    if (next_id_ <= 0 || next_id_ == std::numeric_limits<i32>::max()) {
        next_id_ = 1;
    }
    if (next_generation_ <= 0 ||
        next_generation_ == std::numeric_limits<i32>::max()) {
        next_generation_ = 1;
    }
    while (entries_.contains(next_id_)) {
        ++next_id_;
        if (next_id_ <= 0 ||
            next_id_ == std::numeric_limits<i32>::max()) {
            next_id_ = 1;
        }
    }
    return ConnectionToken {
        .id = next_id_++,
        .generation = next_generation_++,
    };
}

Result<ConnectionRegistry::Entry*> ConnectionRegistry::mutable_entry(
    ConnectionToken token) {
    if (!token.valid()) {
        return fail(ErrorCode::invalid_argument,
                    "connection token is invalid");
    }
    const auto found = entries_.find(token.id);
    if (found == entries_.end() || found->second.token != token) {
        return fail(ErrorCode::invalid_state,
                    "connection is closed or belongs to another generation");
    }
    return &found->second;
}

Result<const ConnectionRegistry::Entry*> ConnectionRegistry::entry(
    ConnectionToken token) const {
    if (!token.valid()) {
        return fail(ErrorCode::invalid_argument,
                    "connection token is invalid");
    }
    const auto found = entries_.find(token.id);
    if (found == entries_.end() || found->second.token != token) {
        return fail(ErrorCode::invalid_state,
                    "connection is closed or belongs to another generation");
    }
    return &found->second;
}

Result<NativeConnection> ConnectionRegistry::open_native(
    const Entry& entry_value) {
    switch (entry_value.kind) {
    case ConnectionKind::stream:
        return await_operation<NativeConnection>(
            entry_value.token, PendingOperationKind::lifecycle,
            entry_value.timeout_ms,
            [&entry_value](
                const std::shared_ptr<AsyncNetworkAdapter>& adapter,
                Completion<NativeConnection> completion) {
                return adapter->open_stream(entry_value.parsed_url,
                                            entry_value.timeout_ms,
                                            std::move(completion));
            });
    case ConnectionKind::server:
        return await_operation<NativeConnection>(
            entry_value.token, PendingOperationKind::lifecycle,
            entry_value.timeout_ms,
            [&entry_value](
                const std::shared_ptr<AsyncNetworkAdapter>& adapter,
                Completion<NativeConnection> completion) {
                return adapter->open_server(entry_value.parsed_url,
                                            entry_value.timeout_ms,
                                            std::move(completion));
            });
    case ConnectionKind::datagram:
        return await_operation<NativeConnection>(
            entry_value.token, PendingOperationKind::lifecycle,
            entry_value.timeout_ms,
            [&entry_value](
                const std::shared_ptr<AsyncNetworkAdapter>& adapter,
                Completion<NativeConnection> completion) {
                return adapter->open_datagram(entry_value.parsed_url,
                                              entry_value.timeout_ms,
                                              std::move(completion));
            });
    case ConnectionKind::http:
    case ConnectionKind::https:
        return fail(ErrorCode::invalid_state,
                    "HTTP connections are opened lazily");
    }
    return fail(ErrorCode::internal_error,
                "unknown network connection kind");
}

Result<OpenedConnection> ConnectionRegistry::open(
    std::string_view url_text,
    ConnectionMode mode,
    bool timeouts) {
    if (mode != ConnectionMode::read &&
        mode != ConnectionMode::write &&
        mode != ConnectionMode::read_write) {
        return fail(ErrorCode::invalid_argument,
                    "GCF connection mode is invalid");
    }
    auto parsed = Url::parse(url_text);
    if (!parsed) return std::unexpected(parsed.error());

    Entry created;
    {
        std::scoped_lock lock(mutex_);
        created.token = allocate_token_unlocked();
        created.parsed_url = *parsed;
        created.mode = mode;
        created.timeouts = timeouts;
        const bool long_lived_stream =
            parsed->scheme == Scheme::socket ||
            parsed->scheme == Scheme::server_socket;
        created.timeout_ms = timeouts
            ? (long_lived_stream
                   ? kStreamTimeoutEnabledMilliseconds
                   : kTimeoutEnabledMilliseconds)
            : kTimeoutDisabledMilliseconds;
        switch (parsed->scheme) {
        case Scheme::socket:
            created.kind = parsed->server_endpoint
                ? ConnectionKind::server
                : ConnectionKind::stream;
            break;
        case Scheme::server_socket:
            created.kind = ConnectionKind::server;
            break;
        case Scheme::datagram:
            created.kind = ConnectionKind::datagram;
            break;
        case Scheme::http:
            created.kind = ConnectionKind::http;
            break;
        case Scheme::https:
            created.kind = ConnectionKind::https;
            break;
        }
        // Reserve the token before native I/O so concurrent opens cannot
        // allocate the same id/generation while this operation is pending.
        entries_.insert_or_assign(created.token.id, created);
    }

    if (created.kind != ConnectionKind::http &&
        created.kind != ConnectionKind::https) {
        auto native = open_native(created);
        if (!native) {
            std::scoped_lock lock(mutex_);
            const auto found = entries_.find(created.token.id);
            if (found != entries_.end() &&
                found->second.token == created.token) {
                entries_.erase(found);
            }
            return std::unexpected(native.error());
        }

        std::optional<Error> completion_error;
        {
            std::scoped_lock lock(mutex_);
            auto found = mutable_entry(created.token);
            if (!found) {
                completion_error = found.error();
            } else if (!(*found)->connection_open) {
                completion_error = Error::make(
                    ErrorCode::invalid_state,
                    "connection closed while opening");
            } else {
                (*found)->native = std::move(*native);
            }
        }
        if (completion_error.has_value()) {
            (void)adapter_->close(native->handle);
            return std::unexpected(std::move(*completion_error));
        }
    }

    return OpenedConnection {
        .token = created.token,
        .kind = created.kind,
    };
}

Result<OpenedConnection> ConnectionRegistry::accept(
    ConnectionToken server) {
    NativeHandle handle;
    i32 timeout_ms = 0;
    bool timeouts = false;
    {
        std::scoped_lock lock(mutex_);
        auto found = entry(server);
        if (!found) return std::unexpected(found.error());
        if ((*found)->kind != ConnectionKind::server ||
            !(*found)->native.has_value()) {
            return fail(ErrorCode::invalid_state,
                        "connection is not an open server socket");
        }
        handle = (*found)->native->handle;
        timeout_ms = (*found)->timeout_ms;
        timeouts = (*found)->timeouts;
    }
    auto accepted = await_operation<NativeConnection>(
        server, PendingOperationKind::lifecycle, timeout_ms,
        [handle, timeout_ms](
            const std::shared_ptr<AsyncNetworkAdapter>& adapter,
            Completion<NativeConnection> completion) {
            return adapter->accept(handle, timeout_ms,
                                   std::move(completion));
        });
    if (!accepted) return std::unexpected(accepted.error());

    Entry created;
    {
        std::scoped_lock lock(mutex_);
        auto still_open = entry(server);
        if (!still_open) {
            (void)adapter_->close(accepted->handle);
            return std::unexpected(still_open.error());
        }
        created.token = allocate_token_unlocked();
        created.kind = ConnectionKind::stream;
        created.mode = ConnectionMode::read_write;
        created.timeouts = timeouts;
        created.timeout_ms = timeout_ms;
        created.native = *accepted;
        std::string remote_host = accepted->remote.host;
        if (remote_host.find(':') != std::string::npos) {
            remote_host = "[" + remote_host + "]";
        }
        auto parsed = Url::parse(
            "socket://" + remote_host + ":" +
            std::to_string(accepted->remote.port));
        if (parsed) created.parsed_url = std::move(*parsed);
        entries_.insert_or_assign(created.token.id, created);
    }
    return OpenedConnection {
        .token = created.token,
        .kind = created.kind,
    };
}

Status ConnectionRegistry::reconnect(ConnectionToken token) {
    Entry snapshot;
    std::optional<NativeHandle> old_handle;
    std::vector<std::function<void()>> cancellations;
    std::shared_ptr<AsyncNetworkAdapter> adapter;
    {
        std::scoped_lock lock(mutex_);
        auto found = mutable_entry(token);
        if (!found) return std::unexpected(found.error());
        if (!(*found)->connection_open) {
            return fail(ErrorCode::invalid_state,
                        "closed connection cannot reconnect");
        }
        if (((*found)->kind == ConnectionKind::http ||
             (*found)->kind == ConnectionKind::https) &&
            (*found)->request_started && (*found)->http_method == "POST") {
            return fail(ErrorCode::invalid_state,
                        "reconnect cannot replay a committed HTTP POST");
        }
        snapshot = **found;
        snapshot.pending_cancellations.clear();
        cancellations.reserve((*found)->pending_cancellations.size());
        for (auto& [unused, pending] :
             (*found)->pending_cancellations) {
            (void)unused;
            cancellations.push_back(std::move(pending.callback));
        }
        (*found)->pending_cancellations.clear();
        if ((*found)->native.has_value()) {
            old_handle = (*found)->native->handle;
            (*found)->native.reset();
        }
        (*found)->response.reset();
        (*found)->response_cursor = 0;
        (*found)->request_started = false;
        adapter = adapter_;
    }
    for (auto& cancellation : cancellations) cancellation();
    if (old_handle.has_value() && adapter) {
        (void)adapter->close(*old_handle);
    }
    if (snapshot.kind == ConnectionKind::http ||
        snapshot.kind == ConnectionKind::https) {
        return {};
    }
    auto reopened = open_native(snapshot);
    if (!reopened) return std::unexpected(reopened.error());
    std::scoped_lock lock(mutex_);
    auto found = mutable_entry(token);
    if (!found) {
        if (adapter) (void)adapter->close(reopened->handle);
        return std::unexpected(found.error());
    }
    (*found)->native = std::move(*reopened);
    return {};
}

Status ConnectionRegistry::close(ConnectionToken token) {
    std::optional<NativeHandle> native;
    std::vector<std::function<void()>> cancellations;
    std::shared_ptr<AsyncNetworkAdapter> adapter;
    {
        std::scoped_lock lock(mutex_);
        auto found = mutable_entry(token);
        if (!found) return {};
        (*found)->connection_open = false;
        if ((*found)->input_streams != 0U ||
            (*found)->output_streams != 0U) {
            return {};
        }
        if ((*found)->native.has_value()) native = (*found)->native->handle;
        cancellations.reserve((*found)->pending_cancellations.size());
        for (auto& [unused, pending] :
             (*found)->pending_cancellations) {
            (void)unused;
            cancellations.push_back(std::move(pending.callback));
        }
        (*found)->pending_cancellations.clear();
        adapter = adapter_;
        entries_.erase(token.id);
    }
    for (auto& cancellation : cancellations) cancellation();
    if (native.has_value() && adapter) return adapter->close(*native);
    return {};
}

void ConnectionRegistry::close_all() noexcept {
    std::vector<NativeHandle> handles;
    std::vector<std::function<void()>> cancellations;
    std::shared_ptr<AsyncNetworkAdapter> adapter;
    {
        std::scoped_lock lock(mutex_);
        handles.reserve(entries_.size());
        for (auto& [unused, value] : entries_) {
            (void)unused;
            if (value.native.has_value()) {
                handles.push_back(value.native->handle);
            }
            for (auto& [pending_id, pending] :
                 value.pending_cancellations) {
                (void)pending_id;
                cancellations.push_back(std::move(pending.callback));
            }
            value.pending_cancellations.clear();
        }
        adapter = adapter_;
        entries_.clear();
    }
    for (auto& cancellation : cancellations) cancellation();
    if (adapter) {
        for (const NativeHandle handle : handles) {
            (void)adapter->close(handle);
        }
    }
}

Result<ConnectionKind> ConnectionRegistry::kind(
    ConnectionToken token) const {
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    return (*found)->kind;
}

Result<Url> ConnectionRegistry::url(ConnectionToken token) const {
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    if ((*found)->response.has_value()) return (*found)->response->final_url;
    return (*found)->parsed_url;
}

Result<Endpoint> ConnectionRegistry::local_endpoint(
    ConnectionToken token) const {
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    if (!(*found)->native.has_value()) {
        return fail(ErrorCode::invalid_state,
                    "connection has no native local endpoint");
    }
    return (*found)->native->local;
}

Result<Endpoint> ConnectionRegistry::remote_endpoint(
    ConnectionToken token) const {
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    if ((*found)->kind == ConnectionKind::http ||
        (*found)->kind == ConnectionKind::https) {
        return Endpoint {
            .host = (*found)->parsed_url.host,
            .port = (*found)->parsed_url.effective_port(),
        };
    }
    if (!(*found)->native.has_value()) {
        return fail(ErrorCode::invalid_state,
                    "connection has no native remote endpoint");
    }
    return (*found)->native->remote;
}

Status ConnectionRegistry::open_input(ConnectionToken token) {
    std::scoped_lock lock(mutex_);
    auto found = mutable_entry(token);
    if (!found) return std::unexpected(found.error());
    if (!(*found)->connection_open || !readable((*found)->mode)) {
        return fail(ErrorCode::invalid_state,
                    "connection is not open for input");
    }
    if ((*found)->input_stream_issued) {
        return fail(ErrorCode::io_error,
                    "no more input streams are available");
    }
    (*found)->input_stream_issued = true;
    (*found)->input_streams = 1U;
    return {};
}

Status ConnectionRegistry::open_output(ConnectionToken token) {
    std::scoped_lock lock(mutex_);
    auto found = mutable_entry(token);
    if (!found) return std::unexpected(found.error());
    if (!(*found)->connection_open || !writable((*found)->mode)) {
        return fail(ErrorCode::invalid_state,
                    "connection is not open for output");
    }
    if ((*found)->request_started) {
        return fail(ErrorCode::invalid_state,
                    "HTTP request output cannot open after request starts");
    }
    if ((*found)->output_stream_issued) {
        return fail(ErrorCode::io_error,
                    "no more output streams are available");
    }
    (*found)->output_stream_issued = true;
    (*found)->output_streams = 1U;
    return {};
}

Status ConnectionRegistry::release_if_unused(ConnectionToken token) {
    std::optional<NativeHandle> native;
    std::vector<std::function<void()>> cancellations;
    std::shared_ptr<AsyncNetworkAdapter> adapter;
    {
        std::scoped_lock lock(mutex_);
        auto found = mutable_entry(token);
        if (!found) return {};
        if ((*found)->connection_open || (*found)->input_streams != 0U ||
            (*found)->output_streams != 0U) {
            return {};
        }
        if ((*found)->native.has_value()) native = (*found)->native->handle;
        cancellations.reserve((*found)->pending_cancellations.size());
        for (auto& [unused, pending] :
             (*found)->pending_cancellations) {
            (void)unused;
            cancellations.push_back(std::move(pending.callback));
        }
        (*found)->pending_cancellations.clear();
        adapter = adapter_;
        entries_.erase(token.id);
    }
    for (auto& cancellation : cancellations) cancellation();
    if (native.has_value() && adapter) return adapter->close(*native);
    return {};
}

Status ConnectionRegistry::close_input(ConnectionToken token) {
    std::vector<std::function<void()>> cancellations;
    {
        std::scoped_lock lock(mutex_);
        auto found = mutable_entry(token);
        if (!found) return {};
        if ((*found)->input_streams != 0U) --(*found)->input_streams;
        if ((*found)->input_streams == 0U) {
            auto iterator = (*found)->pending_cancellations.begin();
            while (iterator != (*found)->pending_cancellations.end()) {
                if (iterator->second.kind == PendingOperationKind::input) {
                    cancellations.push_back(
                        std::move(iterator->second.callback));
                    iterator = (*found)->pending_cancellations.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        }
    }
    for (auto& cancellation : cancellations) cancellation();
    return release_if_unused(token);
}

Status ConnectionRegistry::close_output(ConnectionToken token) {
    std::vector<std::function<void()>> cancellations;
    std::optional<NativeHandle> shutdown_handle;
    std::shared_ptr<AsyncNetworkAdapter> adapter;
    {
        std::scoped_lock lock(mutex_);
        auto found = mutable_entry(token);
        if (!found) return {};
        const bool was_open = (*found)->output_streams != 0U;
        if (was_open) --(*found)->output_streams;
        if (was_open && (*found)->output_streams == 0U) {
            auto iterator = (*found)->pending_cancellations.begin();
            while (iterator != (*found)->pending_cancellations.end()) {
                if (iterator->second.kind == PendingOperationKind::output) {
                    cancellations.push_back(
                        std::move(iterator->second.callback));
                    iterator = (*found)->pending_cancellations.erase(iterator);
                } else {
                    ++iterator;
                }
            }
            if ((*found)->kind == ConnectionKind::stream &&
                (*found)->native.has_value()) {
                shutdown_handle = (*found)->native->handle;
                adapter = adapter_;
            }
        }
    }
    for (auto& cancellation : cancellations) cancellation();
    Status shutdown_status {};
    if (shutdown_handle.has_value() && adapter) {
        shutdown_status = adapter->shutdown_output(*shutdown_handle);
    }
    auto released = release_if_unused(token);
    if (!shutdown_status) return shutdown_status;
    return released;
}

Status ConnectionRegistry::ensure_http_response(ConnectionToken token) {
    HttpRequest request;
    {
        std::scoped_lock lock(mutex_);
        auto found = mutable_entry(token);
        if (!found) return std::unexpected(found.error());
        if ((*found)->kind != ConnectionKind::http &&
            (*found)->kind != ConnectionKind::https) {
            return fail(ErrorCode::invalid_state,
                        "connection is not HTTP(S)");
        }
        if ((*found)->response.has_value()) return {};
        if (!(*found)->connection_open && (*found)->input_streams == 0U) {
            return fail(ErrorCode::invalid_state,
                        "HTTP connection is closed");
        }
        if ((*found)->request_started) {
            return fail(ErrorCode::invalid_state,
                        "HTTP request is already in progress");
        }
        (*found)->request_started = true;
        request.url = (*found)->parsed_url;
        request.method = (*found)->http_method;
        request.headers = (*found)->request_headers;
        request.body = (*found)->request_body;
        request.timeout_ms = (*found)->timeout_ms;
    }

    auto response = await_operation<HttpResponse>(
        token, PendingOperationKind::input, request.timeout_ms,
        [request = std::move(request)](
            const std::shared_ptr<AsyncNetworkAdapter>& adapter,
            Completion<HttpResponse> completion) mutable {
            return adapter->perform_http(std::move(request),
                                         std::move(completion));
        });
    if (!response) {
        std::scoped_lock lock(mutex_);
        auto found = mutable_entry(token);
        if (found) (*found)->request_started = false;
        return std::unexpected(response.error());
    }

    std::scoped_lock lock(mutex_);
    auto found = mutable_entry(token);
    if (!found) return std::unexpected(found.error());
    (*found)->response = std::move(*response);
    (*found)->response_cursor = 0;
    return {};
}

Result<std::vector<u8>> ConnectionRegistry::read(
    ConnectionToken token,
    usize maximum_bytes) {
    if (maximum_bytes == 0U) return std::vector<u8> {};
    ConnectionKind connection_kind;
    NativeHandle handle;
    i32 timeout_ms = 0;
    {
        std::scoped_lock lock(mutex_);
        auto found = entry(token);
        if (!found) return std::unexpected(found.error());
        connection_kind = (*found)->kind;
        timeout_ms = (*found)->timeout_ms;
        if (connection_kind != ConnectionKind::http &&
            connection_kind != ConnectionKind::https) {
            if (!(*found)->native.has_value()) {
                return fail(ErrorCode::invalid_state,
                            "network connection has no native handle");
            }
            handle = (*found)->native->handle;
        }
    }
    if (connection_kind == ConnectionKind::http ||
        connection_kind == ConnectionKind::https) {
        auto prepared = ensure_http_response(token);
        if (!prepared) return std::unexpected(prepared.error());
        std::scoped_lock lock(mutex_);
        auto found = mutable_entry(token);
        if (!found) return std::unexpected(found.error());
        const auto& body = (*found)->response->body;
        if ((*found)->response_cursor >= body.size()) {
            return std::vector<u8> {};
        }
        const usize count = std::min(maximum_bytes,
                                     body.size() -
                                         (*found)->response_cursor);
        std::vector<u8> result(
            body.begin() + static_cast<isize>((*found)->response_cursor),
            body.begin() + static_cast<isize>(
                (*found)->response_cursor + count));
        (*found)->response_cursor += count;
        return result;
    }

    return await_operation<std::vector<u8>>(
        token, PendingOperationKind::input, timeout_ms,
        [handle, maximum_bytes, timeout_ms](
            const std::shared_ptr<AsyncNetworkAdapter>& adapter,
            Completion<std::vector<u8>> completion) {
            return adapter->read(handle, maximum_bytes, timeout_ms,
                                 std::move(completion));
        });
}

Status ConnectionRegistry::write(ConnectionToken token,
                                 std::span<const u8> bytes) {
    if (bytes.empty()) return {};
    ConnectionKind connection_kind;
    NativeHandle handle;
    i32 timeout_ms = 0;
    {
        std::scoped_lock lock(mutex_);
        auto found = mutable_entry(token);
        if (!found) return std::unexpected(found.error());
        connection_kind = (*found)->kind;
        timeout_ms = (*found)->timeout_ms;
        if (connection_kind == ConnectionKind::http ||
            connection_kind == ConnectionKind::https) {
            if ((*found)->request_started) {
                return fail(ErrorCode::invalid_state,
                            "HTTP request body is already committed");
            }
            if (bytes.size() > kMaximumBufferedHttpBody -
                                   (*found)->request_body.size()) {
                return fail(ErrorCode::overflow,
                            "HTTP request body exceeds maximum size");
            }
            (*found)->request_body.insert((*found)->request_body.end(),
                                          bytes.begin(), bytes.end());
            return {};
        }
        if (!(*found)->native.has_value()) {
            return fail(ErrorCode::invalid_state,
                        "network connection has no native handle");
        }
        handle = (*found)->native->handle;
    }

    std::vector<u8> owned(bytes.begin(), bytes.end());
    auto written = await_operation<usize>(
        token, PendingOperationKind::output, timeout_ms,
        [handle, timeout_ms, owned = std::move(owned)](
            const std::shared_ptr<AsyncNetworkAdapter>& adapter,
            Completion<usize> completion) mutable {
            return adapter->write(handle, std::move(owned), timeout_ms,
                                  std::move(completion));
        });
    if (!written) return std::unexpected(written.error());
    if (*written != bytes.size()) {
        return fail(ErrorCode::io_error,
                    "network adapter performed a partial write");
    }
    return {};
}

Result<usize> ConnectionRegistry::available(ConnectionToken token) {
    ConnectionKind connection_kind;
    NativeHandle handle;
    {
        std::scoped_lock lock(mutex_);
        auto found = entry(token);
        if (!found) return std::unexpected(found.error());
        connection_kind = (*found)->kind;
        if (connection_kind == ConnectionKind::http ||
            connection_kind == ConnectionKind::https) {
            if (!(*found)->response.has_value()) return 0U;
            return (*found)->response->body.size() -
                   (*found)->response_cursor;
        }
        if (!(*found)->native.has_value()) {
            return fail(ErrorCode::invalid_state,
                        "network connection has no native handle");
        }
        handle = (*found)->native->handle;
    }
    return await_operation<usize>(
        token, PendingOperationKind::input,
        kTimeoutEnabledMilliseconds,
        [handle](const std::shared_ptr<AsyncNetworkAdapter>& adapter,
                 Completion<usize> completion) {
            return adapter->available(handle, std::move(completion));
        });
}

Status ConnectionRegistry::flush(ConnectionToken token) {
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    return {};
}

Result<NativeHandle> ConnectionRegistry::native_handle(
    ConnectionToken token) const {
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    if (!(*found)->native.has_value()) {
        return fail(ErrorCode::invalid_state,
                    "connection has no native handle");
    }
    return (*found)->native->handle;
}

Status ConnectionRegistry::set_socket_option(ConnectionToken token,
                                             SocketOption option,
                                             i32 value) {
    auto handle = native_handle(token);
    if (!handle) return std::unexpected(handle.error());
    auto changed = await_operation<bool>(
        token, PendingOperationKind::control,
        kTimeoutEnabledMilliseconds,
        [handle = *handle, option, value](
            const std::shared_ptr<AsyncNetworkAdapter>& adapter,
            Completion<bool> completion) {
            return adapter->set_socket_option(handle, option, value,
                                              std::move(completion));
        });
    if (!changed) return std::unexpected(changed.error());
    return {};
}

Result<i32> ConnectionRegistry::socket_option(ConnectionToken token,
                                              SocketOption option) {
    auto handle = native_handle(token);
    if (!handle) return std::unexpected(handle.error());
    return await_operation<i32>(
        token, PendingOperationKind::control,
        kTimeoutEnabledMilliseconds,
        [handle = *handle, option](
            const std::shared_ptr<AsyncNetworkAdapter>& adapter,
            Completion<i32> completion) {
            return adapter->get_socket_option(handle, option,
                                              std::move(completion));
        });
}

Status ConnectionRegistry::send_datagram(ConnectionToken token,
                                         DatagramPacket packet) {
    NativeHandle handle;
    i32 timeout_ms = 0;
    {
        std::scoped_lock lock(mutex_);
        auto found = entry(token);
        if (!found) return std::unexpected(found.error());
        if ((*found)->kind != ConnectionKind::datagram ||
            !(*found)->native.has_value()) {
            return fail(ErrorCode::invalid_state,
                        "connection is not a datagram socket");
        }
        handle = (*found)->native->handle;
        timeout_ms = (*found)->timeout_ms;
        if (packet.peer.host.empty() &&
            !(*found)->native->remote.host.empty()) {
            packet.peer = (*found)->native->remote;
        }
    }
    if (packet.bytes.size() > kMaximumDatagramLength) {
        return fail(ErrorCode::out_of_range,
                    "UDP payload exceeds maximum datagram length");
    }
    auto sent = await_operation<usize>(
        token, PendingOperationKind::output, timeout_ms,
        [handle, packet = std::move(packet), timeout_ms](
            const std::shared_ptr<AsyncNetworkAdapter>& adapter,
            Completion<usize> completion) mutable {
            return adapter->send_datagram(handle, std::move(packet),
                                          timeout_ms,
                                          std::move(completion));
        });
    if (!sent) return std::unexpected(sent.error());
    return {};
}

Result<DatagramPacket> ConnectionRegistry::receive_datagram(
    ConnectionToken token,
    usize maximum_bytes) {
    NativeHandle handle;
    i32 timeout_ms = 0;
    {
        std::scoped_lock lock(mutex_);
        auto found = entry(token);
        if (!found) return std::unexpected(found.error());
        if ((*found)->kind != ConnectionKind::datagram ||
            !(*found)->native.has_value()) {
            return fail(ErrorCode::invalid_state,
                        "connection is not a datagram socket");
        }
        handle = (*found)->native->handle;
        timeout_ms = (*found)->timeout_ms;
    }
    return await_operation<DatagramPacket>(
        token, PendingOperationKind::input, timeout_ms,
        [handle, maximum_bytes, timeout_ms](
            const std::shared_ptr<AsyncNetworkAdapter>& adapter,
            Completion<DatagramPacket> completion) {
            return adapter->receive_datagram(
                handle, std::min(maximum_bytes, kMaximumDatagramLength),
                timeout_ms, std::move(completion));
        });
}

Result<usize> ConnectionRegistry::maximum_datagram_length(
    ConnectionToken token) const {
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    if ((*found)->kind != ConnectionKind::datagram) {
        return fail(ErrorCode::invalid_state,
                    "connection is not a datagram socket");
    }
    return kMaximumDatagramLength;
}

Status ConnectionRegistry::set_http_method(ConnectionToken token,
                                           std::string method) {
    if (!valid_http_token(method)) {
        return fail(ErrorCode::invalid_argument,
                    "HTTP request method is invalid");
    }
    std::transform(method.begin(), method.end(), method.begin(),
                   [](unsigned char character) {
                       if (character >= 'a' && character <= 'z') {
                           return static_cast<char>(character - 'a' + 'A');
                       }
                       return static_cast<char>(character);
                   });
    if (method != "GET" && method != "HEAD" && method != "POST") {
        return fail(ErrorCode::io_error,
                    "unsupported HTTP request method: " + method);
    }
    std::scoped_lock lock(mutex_);
    auto found = mutable_entry(token);
    if (!found) return std::unexpected(found.error());
    if ((*found)->kind != ConnectionKind::http &&
        (*found)->kind != ConnectionKind::https) {
        return fail(ErrorCode::invalid_state,
                    "connection is not HTTP(S)");
    }
    if ((*found)->request_started) {
        return fail(ErrorCode::invalid_state,
                    "HTTP request is already committed");
    }
    if ((*found)->output_stream_issued) {
        return {};
    }
    (*found)->http_method = std::move(method);
    return {};
}

Result<std::string> ConnectionRegistry::http_method(
    ConnectionToken token) const {
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    if ((*found)->kind != ConnectionKind::http &&
        (*found)->kind != ConnectionKind::https) {
        return fail(ErrorCode::invalid_state,
                    "connection is not HTTP(S)");
    }
    return (*found)->http_method;
}

Status ConnectionRegistry::set_http_header(ConnectionToken token,
                                           std::string name,
                                           std::string value) {
    if (!valid_http_token(name) || !valid_header_value(value)) {
        return fail(ErrorCode::invalid_argument,
                    "HTTP header is invalid");
    }
    std::scoped_lock lock(mutex_);
    auto found = mutable_entry(token);
    if (!found) return std::unexpected(found.error());
    if ((*found)->kind != ConnectionKind::http &&
        (*found)->kind != ConnectionKind::https) {
        return fail(ErrorCode::invalid_state,
                    "connection is not HTTP(S)");
    }
    if ((*found)->request_started) {
        return fail(ErrorCode::invalid_state,
                    "HTTP request is already committed");
    }
    if ((*found)->output_stream_issued) {
        return {};
    }
    set_header((*found)->request_headers, std::move(name), std::move(value));
    return {};
}

Result<std::optional<std::string>>
ConnectionRegistry::http_request_header(ConnectionToken token,
                                        std::string_view name) const {
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    return find_header((*found)->request_headers, name);
}

Result<i32> ConnectionRegistry::http_response_code(
    ConnectionToken token) {
    auto prepared = ensure_http_response(token);
    if (!prepared) return std::unexpected(prepared.error());
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    return (*found)->response->status_code;
}

Result<std::string> ConnectionRegistry::http_response_message(
    ConnectionToken token) {
    auto prepared = ensure_http_response(token);
    if (!prepared) return std::unexpected(prepared.error());
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    return (*found)->response->reason;
}

Result<std::optional<std::string>>
ConnectionRegistry::http_response_header(ConnectionToken token,
                                         std::string_view name) {
    auto prepared = ensure_http_response(token);
    if (!prepared) return std::unexpected(prepared.error());
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    return find_header((*found)->response->headers, name);
}

Result<std::optional<Header>> ConnectionRegistry::http_response_header(
    ConnectionToken token,
    usize index) {
    auto prepared = ensure_http_response(token);
    if (!prepared) return std::unexpected(prepared.error());
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    if (index >= (*found)->response->headers.size()) {
        return std::optional<Header> {};
    }
    return std::optional<Header>((*found)->response->headers[index]);
}

Result<i64> ConnectionRegistry::http_content_length(
    ConnectionToken token) {
    auto prepared = ensure_http_response(token);
    if (!prepared) return std::unexpected(prepared.error());
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    auto header = find_header((*found)->response->headers, "Content-Length");
    if (!header.has_value()) {
        const usize size = (*found)->response->body.size();
        if (size > static_cast<usize>(std::numeric_limits<i64>::max())) {
            return fail(ErrorCode::overflow,
                        "HTTP body length exceeds Java long");
        }
        return static_cast<i64>(size);
    }
    i64 length = -1;
    const auto converted = std::from_chars(
        header->data(), header->data() + header->size(), length);
    if (converted.ec != std::errc {} ||
        converted.ptr != header->data() + header->size() || length < 0) {
        return -1;
    }
    return length;
}

Result<std::optional<SecurityMetadata>>
ConnectionRegistry::http_security_info(ConnectionToken token) {
    auto prepared = ensure_http_response(token);
    if (!prepared) return std::unexpected(prepared.error());
    std::scoped_lock lock(mutex_);
    auto found = entry(token);
    if (!found) return std::unexpected(found.error());
    if ((*found)->kind != ConnectionKind::https) {
        return fail(ErrorCode::invalid_state,
                    "security information requires HTTPS");
    }
    return (*found)->response->security;
}

} // namespace phoneme::network
