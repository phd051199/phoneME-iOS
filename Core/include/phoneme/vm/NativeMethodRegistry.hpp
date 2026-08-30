#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "phoneme/vm/MetadataId.hpp"
#include "phoneme/vm/Value.hpp"

namespace phoneme::vm {

class Machine;

using NativeMethod = std::function<Result<std::optional<Value>>(
    Machine& machine,
    std::span<const Value> arguments)>;

enum class NativeJitPolicy : u8 {
    conservative,
    synchronous_bounded,
};

struct NativeMethodSignature final {
    NativeMethodId id;
    std::string owner;
    std::string name;
    std::string descriptor;
};

struct NativeMethodInvocationCount final {
    NativeMethodSignature signature;
    std::size_t count {0};
};

struct NativeMethodBinding final {
    NativeMethodId id;
    u64 generation {0U};
};

class NativeMethodRegistry final {
public:
    NativeMethodRegistry();
    ~NativeMethodRegistry();

    // Core registers hundreds of native/HLE entries during bootstrap. Batch
    // mode defers rebuilding the immutable ID dispatch snapshot until the
    // outermost batch ends, avoiding O(N^2) startup copies while keeping
    // ordinary late registration immediately visible to lock-free readers.
    void begin_registration_batch() noexcept;
    void end_registration_batch() noexcept;

    [[nodiscard]] Status register_method(
        std::string owner,
        std::string name,
        std::string descriptor,
        NativeMethod implementation,
        NativeJitPolicy jit_policy = NativeJitPolicy::conservative);
    [[nodiscard]] Status register_alias(std::string_view source_owner,
                                        std::string_view source_name,
                                        std::string_view source_descriptor,
                                        std::string target_owner,
                                        std::string target_name,
                                        std::string target_descriptor);
    [[nodiscard]] NativeMethodId resolve(std::string_view owner,
                                         std::string_view name,
                                         std::string_view descriptor) const noexcept;
    [[nodiscard]] NativeMethodBinding resolve_binding(
        std::string_view owner,
        std::string_view name,
        std::string_view descriptor) const noexcept;
    [[nodiscard]] bool contains(std::string_view owner,
                                std::string_view name,
                                std::string_view descriptor) const noexcept;
    [[nodiscard]] NativeJitPolicy jit_policy(
        NativeMethodId method_id) const noexcept;
    [[nodiscard]] Result<std::optional<Value>> invoke(
        Machine& machine,
        NativeMethodId method_id,
        std::span<const Value> arguments) const;
    [[nodiscard]] Result<std::optional<Value>> invoke(
        Machine& machine,
        std::string_view owner,
        std::string_view name,
        std::string_view descriptor,
        std::span<const Value> arguments) const;
    [[nodiscard]] std::vector<NativeMethodSignature>
    registered_methods() const;
    [[nodiscard]] std::vector<NativeMethodInvocationCount>
    invocation_counts() const;
    void reset_invocation_counts() noexcept;
    [[nodiscard]] u64 generation() const noexcept;
    void clear() noexcept;

private:
    struct NativeLookupKey final {
        std::string_view owner;
        std::string_view name;
        std::string_view descriptor;
    };

    struct NativeLookupKeyHash final {
        [[nodiscard]] usize operator()(NativeLookupKey key) const noexcept;
    };

    struct NativeLookupKeyEqual final {
        [[nodiscard]] bool operator()(NativeLookupKey left,
                                      NativeLookupKey right) const noexcept;
    };

    struct Entry final {
        NativeMethodSignature signature;
        NativeMethod implementation;
        NativeJitPolicy jit_policy {NativeJitPolicy::conservative};
        std::atomic<std::size_t> invocation_count {0};
    };

    using DispatchTable = std::vector<std::shared_ptr<Entry>>;

    void publish_dispatch_table_locked();
    void advance_generation_locked() noexcept;

    mutable std::mutex mutex_;
    // Non-owning keys point into Entry::signature. Entry objects are stable
    // until clear/destruction, so registration and lookup avoid allocating a
    // concatenated owner/name/descriptor key on every operation.
    std::unordered_map<NativeLookupKey,
                       NativeMethodId,
                       NativeLookupKeyHash,
                       NativeLookupKeyEqual> ids_by_key_;
    std::unordered_set<std::string_view> owners_;
    DispatchTable entries_;
    // Readers only need a stable immutable table pointer. Published snapshots
    // are retained for the registry lifetime so invoke(NativeMethodId) avoids
    // the atomic shared_ptr reference-count traffic that would otherwise occur
    // on every native call. Registration/clear are cold writer paths.
    std::vector<std::unique_ptr<const DispatchTable>> dispatch_snapshots_;
    std::atomic<const DispatchTable*> dispatch_table_ {nullptr};
    u32 registration_batch_depth_ {0U};
    std::atomic<u64> generation_ {1U};
};

void register_core_natives(NativeMethodRegistry& registry);

} // namespace phoneme::vm
