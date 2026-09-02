#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "phoneme/network/AsyncNetworkAdapter.hpp"

namespace phoneme::network {

enum class ConnectionKind : u8 {
    stream,
    server,
    datagram,
    http,
    https,
};

struct ConnectionToken final {
    i32 id {0};
    i32 generation {0};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return id > 0 && generation > 0;
    }
    friend constexpr bool operator==(ConnectionToken,
                                     ConnectionToken) noexcept = default;
};

struct NetworkBlockingHooks final {
    std::function<u32()> before_block;
    std::function<void(u32)> after_block;
    std::function<std::optional<Error>()> poll_cancellation;
    std::function<bool(std::chrono::milliseconds)> cooperative_wait;
    std::function<void()> wake_waiters;
};

struct OpenedConnection final {
    ConnectionToken token;
    ConnectionKind kind {ConnectionKind::stream};
};

struct ConnectionRegistryDiagnostics final {
    usize connections {0U};
    usize pending_operations {0U};
    NetworkAdapterDiagnostics adapter;
};

class ConnectionRegistry final {
public:
    ConnectionRegistry();
    explicit ConnectionRegistry(std::shared_ptr<AsyncNetworkAdapter> adapter);
    ~ConnectionRegistry();

    ConnectionRegistry(const ConnectionRegistry&) = delete;
    ConnectionRegistry& operator=(const ConnectionRegistry&) = delete;

    [[nodiscard]] Status set_adapter(
        std::shared_ptr<AsyncNetworkAdapter> adapter);
    void set_blocking_hooks(NetworkBlockingHooks hooks);
    void set_owner(i32 owner) noexcept;
    [[nodiscard]] i32 owner() const noexcept;
    [[nodiscard]] ConnectionRegistryDiagnostics diagnostics() const noexcept;

    [[nodiscard]] Result<OpenedConnection> open(
        std::string_view url,
        ConnectionMode mode,
        bool timeouts);
    [[nodiscard]] Result<OpenedConnection> accept(ConnectionToken server);
    [[nodiscard]] Status reconnect(ConnectionToken token);
    [[nodiscard]] Status close(ConnectionToken token);
    void close_all() noexcept;

    [[nodiscard]] Result<ConnectionKind> kind(ConnectionToken token) const;
    [[nodiscard]] Result<Url> url(ConnectionToken token) const;
    [[nodiscard]] Result<Endpoint> local_endpoint(ConnectionToken token) const;
    [[nodiscard]] Result<Endpoint> remote_endpoint(ConnectionToken token) const;

    [[nodiscard]] Status open_input(ConnectionToken token);
    [[nodiscard]] Status open_output(ConnectionToken token);
    [[nodiscard]] Status close_input(ConnectionToken token);
    [[nodiscard]] Status close_output(ConnectionToken token);
    [[nodiscard]] Result<std::vector<u8>> read(ConnectionToken token,
                                               usize maximum_bytes);
    [[nodiscard]] Status write(ConnectionToken token,
                               std::span<const u8> bytes);
    [[nodiscard]] Result<usize> available(ConnectionToken token);
    [[nodiscard]] Status flush(ConnectionToken token);

    [[nodiscard]] Status set_socket_option(ConnectionToken token,
                                           SocketOption option,
                                           i32 value);
    [[nodiscard]] Result<i32> socket_option(ConnectionToken token,
                                            SocketOption option);

    [[nodiscard]] Status send_datagram(ConnectionToken token,
                                       DatagramPacket packet);
    [[nodiscard]] Result<DatagramPacket> receive_datagram(
        ConnectionToken token,
        usize maximum_bytes);
    [[nodiscard]] Result<usize> maximum_datagram_length(
        ConnectionToken token) const;

    [[nodiscard]] Status set_http_method(ConnectionToken token,
                                         std::string method);
    [[nodiscard]] Result<std::string> http_method(
        ConnectionToken token) const;
    [[nodiscard]] Status set_http_header(ConnectionToken token,
                                         std::string name,
                                         std::string value);
    [[nodiscard]] Result<std::optional<std::string>> http_request_header(
        ConnectionToken token,
        std::string_view name) const;
    [[nodiscard]] Result<i32> http_response_code(ConnectionToken token);
    [[nodiscard]] Result<std::string> http_response_message(
        ConnectionToken token);
    [[nodiscard]] Result<std::optional<std::string>> http_response_header(
        ConnectionToken token,
        std::string_view name);
    [[nodiscard]] Result<std::optional<Header>> http_response_header(
        ConnectionToken token,
        usize index);
    [[nodiscard]] Result<i64> http_content_length(ConnectionToken token);
    [[nodiscard]] Result<std::optional<SecurityMetadata>> http_security_info(
        ConnectionToken token);

private:
    enum class PendingOperationKind : u8 {
        lifecycle,
        input,
        output,
        control,
    };

    struct PendingCancellation final {
        PendingOperationKind kind {PendingOperationKind::control};
        std::function<void()> callback;
    };

    struct Entry final {
        ConnectionToken token;
        ConnectionKind kind {ConnectionKind::stream};
        Url parsed_url;
        ConnectionMode mode {ConnectionMode::read_write};
        bool timeouts {false};
        i32 timeout_ms {30'000};
        bool connection_open {true};
        bool input_stream_issued {false};
        bool output_stream_issued {false};
        usize input_streams {0};
        usize output_streams {0};
        std::optional<NativeConnection> native;
        std::string http_method {"GET"};
        std::vector<Header> request_headers;
        std::vector<u8> request_body;
        std::optional<HttpResponse> response;
        usize response_cursor {0};
        bool request_started {false};
        std::unordered_map<u64, PendingCancellation> pending_cancellations;
    };

    template <typename T, typename Starter>
    [[nodiscard]] Result<T> await_operation(ConnectionToken token,
                                            PendingOperationKind kind,
                                            i32 timeout_ms,
                                            Starter&& starter);
    [[nodiscard]] Result<u64> register_pending_operation(
        ConnectionToken token,
        PendingOperationKind kind,
        std::function<void()> cancellation);
    void unregister_pending_operation(ConnectionToken token,
                                      u64 operation) noexcept;

    [[nodiscard]] Result<Entry*> mutable_entry(ConnectionToken token);
    [[nodiscard]] Result<const Entry*> entry(ConnectionToken token) const;
    [[nodiscard]] Result<NativeHandle> native_handle(ConnectionToken token) const;
    [[nodiscard]] Result<NativeConnection> open_native(const Entry& entry);
    [[nodiscard]] Status ensure_http_response(ConnectionToken token);
    [[nodiscard]] Status release_if_unused(ConnectionToken token);
    [[nodiscard]] ConnectionToken allocate_token_unlocked();

    mutable std::mutex mutex_;
    std::shared_ptr<AsyncNetworkAdapter> adapter_;
    NetworkBlockingHooks blocking_hooks_;
    std::unordered_map<i32, Entry> entries_;
    i32 owner_ {0};
    i32 next_id_ {1};
    i32 next_generation_ {1};
    u64 next_pending_operation_ {1};
};

} // namespace phoneme::network
