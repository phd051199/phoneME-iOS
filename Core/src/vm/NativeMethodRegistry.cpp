#include "phoneme/vm/NativeMethodRegistry.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/PerformanceCounters.hpp"

namespace phoneme::vm {
namespace {

class ProcessNativeCoverage final {
public:
    ProcessNativeCoverage() {
        const char* configured = std::getenv("PHONEME_NATIVE_COVERAGE");
        if (configured != nullptr && *configured != '\0') {
            output_path_ = configured;
        }
    }

    ProcessNativeCoverage(const ProcessNativeCoverage&) = delete;
    ProcessNativeCoverage& operator=(const ProcessNativeCoverage&) = delete;

    void flush() noexcept {
        if (output_path_.empty()) return;
        const std::filesystem::path path(output_path_);
        std::error_code directory_error;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(
                path.parent_path(), directory_error);
        }
        if (directory_error) {
            std::fprintf(stderr,
                         "cannot create native coverage directory: %s\n",
                         directory_error.message().c_str());
            return;
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::fprintf(stderr,
                         "cannot open native coverage file: %s\n",
                         output_path_.c_str());
            return;
        }
        output << "owner\tname\tdescriptor\tinvocations\n";
        std::scoped_lock lock(mutex_);
        for (const auto& [signature, count] : counts_) {
            const auto& [owner, name, descriptor] = signature;
            output << owner << '\t' << name << '\t' << descriptor
                   << '\t' << count << '\n';
        }
    }

    [[nodiscard]] bool enabled() const noexcept {
        return !output_path_.empty();
    }

    void merge(std::span<const NativeMethodInvocationCount> counts) {
        if (output_path_.empty()) return;
        std::scoped_lock lock(mutex_);
        for (const NativeMethodInvocationCount& invocation : counts) {
            if (invocation.count == 0U) continue;
            const NativeMethodSignature& signature = invocation.signature;
            auto& count = counts_[std::tuple {
                signature.owner, signature.name, signature.descriptor,
            }];
            if (invocation.count > std::numeric_limits<std::size_t>::max() - count) {
                count = std::numeric_limits<std::size_t>::max();
            } else {
                count += invocation.count;
            }
        }
    }

private:
    std::string output_path_;
    std::mutex mutex_;
    std::map<std::tuple<std::string, std::string, std::string>, std::size_t>
        counts_;
};

ProcessNativeCoverage& process_native_coverage() {
    // Intentionally process-lifetime storage: VM worker shutdown can still
    // record a final native call while ordinary function-static destructors are
    // running. Leaking this tiny optional telemetry object keeps its mutex and
    // map valid until the OS tears down the process.
    static ProcessNativeCoverage* coverage = [] {
        auto* instance = new ProcessNativeCoverage();
        std::atexit([] {
            process_native_coverage().flush();
        });
        return instance;
    }();
    return *coverage;
}

[[nodiscard]] bool native_invocation_counting_enabled() noexcept {
    // Per-native invocation counters are diagnostics, not execution state.
    // Keeping an atomic fetch_add in every HLE call is surprisingly visible in
    // sprite-heavy MIDP games where Graphics natives run tens of thousands of
    // times per second. Profiling/coverage builds still get exact counts, while
    // the shipping runtime avoids the atomic RMW unless explicitly requested.
    static const bool enabled = [] {
        if constexpr (PerformanceCounters::enabled()) return true;

        const char* coverage = std::getenv("PHONEME_NATIVE_COVERAGE");
        if (coverage != nullptr && *coverage != '\0') return true;

        const char* requested = std::getenv("PHONEME_NATIVE_COUNTS");
        if (requested == nullptr || *requested == '\0') return false;
        const std::string_view value(requested);
        return value != "0" && value != "false" && value != "off";
    }();
    return enabled;
}

} // namespace

NativeMethodRegistry::NativeMethodRegistry() {
    publish_dispatch_table_locked();
}

NativeMethodRegistry::~NativeMethodRegistry() {
    ProcessNativeCoverage& coverage = process_native_coverage();
    if (coverage.enabled()) coverage.merge(invocation_counts());
}

void NativeMethodRegistry::publish_dispatch_table_locked() {
    auto snapshot = std::make_unique<const DispatchTable>(entries_);
    const DispatchTable* published = snapshot.get();
    dispatch_snapshots_.push_back(std::move(snapshot));
    dispatch_table_.store(published, std::memory_order_release);
}

void NativeMethodRegistry::begin_registration_batch() noexcept {
    std::scoped_lock lock(mutex_);
    if (registration_batch_depth_ != std::numeric_limits<u32>::max()) {
        ++registration_batch_depth_;
    }
}

void NativeMethodRegistry::end_registration_batch() noexcept {
    std::scoped_lock lock(mutex_);
    if (registration_batch_depth_ == 0U) return;
    --registration_batch_depth_;
    if (registration_batch_depth_ == 0U) {
        publish_dispatch_table_locked();
    }
}

void NativeMethodRegistry::advance_generation_locked() noexcept {
    const u64 current = generation_.load(std::memory_order_relaxed);
    generation_.store(
        current == std::numeric_limits<u64>::max() ? 1U : current + 1U,
        std::memory_order_release);
}

usize NativeMethodRegistry::NativeLookupKeyHash::operator()(
    NativeLookupKey key) const noexcept {
    const usize owner_hash = std::hash<std::string_view>{}(key.owner);
    const usize name_hash = std::hash<std::string_view>{}(key.name);
    const usize descriptor_hash = std::hash<std::string_view>{}(key.descriptor);
    usize result = owner_hash;
    result ^= name_hash + static_cast<usize>(0x9E3779B9U) +
              (result << 6U) + (result >> 2U);
    result ^= descriptor_hash + static_cast<usize>(0x9E3779B9U) +
              (result << 6U) + (result >> 2U);
    return result;
}

bool NativeMethodRegistry::NativeLookupKeyEqual::operator()(
    NativeLookupKey left,
    NativeLookupKey right) const noexcept {
    return left.owner == right.owner &&
           left.name == right.name &&
           left.descriptor == right.descriptor;
}

Status NativeMethodRegistry::register_method(
    std::string owner,
    std::string name,
    std::string descriptor,
    NativeMethod implementation,
    NativeJitPolicy jit_policy,
    CompactNativeMethod compact_implementation) {
    if (owner.empty() || name.empty() || descriptor.empty() || !implementation) {
        return fail(ErrorCode::invalid_argument,
                    "native method registration is incomplete");
    }

    std::scoped_lock lock(mutex_);
    const NativeLookupKey lookup {
        .owner = owner,
        .name = name,
        .descriptor = descriptor,
    };
    if (ids_by_key_.contains(lookup)) {
        return fail(ErrorCode::invalid_state,
                    "native method is already registered");
    }
    if (entries_.size() >= static_cast<usize>(
            std::numeric_limits<u32>::max() - 1U)) {
        return fail(ErrorCode::overflow,
                    "native method ID space is exhausted");
    }
    const NativeMethodId method_id {
        static_cast<u32>(entries_.size() + 1U),
    };
    auto entry = std::make_shared<Entry>();
    entry->signature = NativeMethodSignature {
        .id = method_id,
        .owner = std::move(owner),
        .name = std::move(name),
        .descriptor = std::move(descriptor),
    };
    entry->implementation = std::move(implementation);
    entry->compact_implementation = std::move(compact_implementation);
    entry->jit_policy = jit_policy;
    Entry* stable_entry = entry.get();
    entries_.push_back(std::move(entry));
    owners_.insert(stable_entry->signature.owner);
    ids_by_key_.emplace(NativeLookupKey {
        .owner = stable_entry->signature.owner,
        .name = stable_entry->signature.name,
        .descriptor = stable_entry->signature.descriptor,
    }, method_id);
    if (registration_batch_depth_ == 0U) publish_dispatch_table_locked();
    advance_generation_locked();
    return {};
}

Status NativeMethodRegistry::register_alias(
    std::string_view source_owner,
    std::string_view source_name,
    std::string_view source_descriptor,
    std::string target_owner,
    std::string target_name,
    std::string target_descriptor) {
    if (source_owner.empty() || source_name.empty() ||
        source_descriptor.empty() || target_owner.empty() ||
        target_name.empty() || target_descriptor.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "native method alias registration is incomplete");
    }

    std::scoped_lock lock(mutex_);
    const auto source = ids_by_key_.find(NativeLookupKey {
        .owner = source_owner,
        .name = source_name,
        .descriptor = source_descriptor,
    });
    if (source == ids_by_key_.end()) {
        return fail(ErrorCode::class_not_found,
                    "native method alias source is not registered");
    }
    if (ids_by_key_.contains(NativeLookupKey {
            .owner = target_owner,
            .name = target_name,
            .descriptor = target_descriptor,
        })) {
        return fail(ErrorCode::invalid_state,
                    "native method alias target is already registered");
    }
    const usize source_index =
        static_cast<usize>(source->second.value - 1U);
    if (source_index >= entries_.size()) {
        return fail(ErrorCode::invalid_state,
                    "native method alias source ID is invalid");
    }
    if (entries_.size() >= static_cast<usize>(
            std::numeric_limits<u32>::max() - 1U)) {
        return fail(ErrorCode::overflow,
                    "native method ID space is exhausted");
    }
    const NativeMethodId method_id {
        static_cast<u32>(entries_.size() + 1U),
    };
    auto entry = std::make_shared<Entry>();
    entry->signature = NativeMethodSignature {
        .id = method_id,
        .owner = std::move(target_owner),
        .name = std::move(target_name),
        .descriptor = std::move(target_descriptor),
    };
    entry->implementation = entries_[source_index]->implementation;
    entry->compact_implementation =
        entries_[source_index]->compact_implementation;
    entry->jit_policy = entries_[source_index]->jit_policy;
    Entry* stable_entry = entry.get();
    entries_.push_back(std::move(entry));
    owners_.insert(stable_entry->signature.owner);
    ids_by_key_.emplace(NativeLookupKey {
        .owner = stable_entry->signature.owner,
        .name = stable_entry->signature.name,
        .descriptor = stable_entry->signature.descriptor,
    }, method_id);
    if (registration_batch_depth_ == 0U) publish_dispatch_table_locked();
    advance_generation_locked();
    return {};
}

NativeMethodId NativeMethodRegistry::resolve(
    std::string_view owner,
    std::string_view name,
    std::string_view descriptor) const noexcept {
    return resolve_binding(owner, name, descriptor).id;
}

NativeMethodBinding NativeMethodRegistry::resolve_binding(
    std::string_view owner,
    std::string_view name,
    std::string_view descriptor) const noexcept {
    PerformanceCounters::record_native_registry_lookup();
    std::scoped_lock lock(mutex_);
    if (!owners_.contains(owner)) {
        return NativeMethodBinding {
            .id = {},
            .generation = generation_.load(std::memory_order_acquire),
        };
    }
    const auto found = ids_by_key_.find(NativeLookupKey {
        .owner = owner,
        .name = name,
        .descriptor = descriptor,
    });
    return NativeMethodBinding {
        .id = found == ids_by_key_.end() ? NativeMethodId {} : found->second,
        .generation = generation_.load(std::memory_order_acquire),
    };
}

bool NativeMethodRegistry::contains(std::string_view owner,
                                    std::string_view name,
                                    std::string_view descriptor) const noexcept {
    return resolve(owner, name, descriptor).valid();
}

NativeJitPolicy NativeMethodRegistry::jit_policy(
    NativeMethodId method_id) const noexcept {
    if (!method_id.valid()) return NativeJitPolicy::conservative;
    const DispatchTable* table = dispatch_table_.load(std::memory_order_acquire);
    if (table == nullptr) return NativeJitPolicy::conservative;
    const usize index = static_cast<usize>(method_id.value - 1U);
    if (index >= table->size() || (*table)[index] == nullptr ||
        (*table)[index]->signature.id != method_id) {
        return NativeJitPolicy::conservative;
    }
    return (*table)[index]->jit_policy;
}

bool NativeMethodRegistry::has_compact_implementation(
    NativeMethodId method_id) const noexcept {
    if (!method_id.valid()) return false;
    const DispatchTable* table = dispatch_table_.load(std::memory_order_acquire);
    if (table == nullptr) return false;
    const usize index = static_cast<usize>(method_id.value - 1U);
    return index < table->size() && (*table)[index] != nullptr &&
           (*table)[index]->signature.id == method_id &&
           static_cast<bool>((*table)[index]->compact_implementation);
}

Result<std::optional<Value>> NativeMethodRegistry::invoke_compact(
    Machine& machine,
    NativeMethodId method_id,
    const InvocationArguments& arguments) const {
    if (!method_id.valid()) {
        return fail(ErrorCode::unsupported_feature,
                    "native method is not ported");
    }
    const DispatchTable* table = dispatch_table_.load(std::memory_order_acquire);
    const usize index = static_cast<usize>(method_id.value - 1U);
    if (table == nullptr || index >= table->size() ||
        (*table)[index] == nullptr ||
        (*table)[index]->signature.id != method_id ||
        !(*table)[index]->compact_implementation) {
        return fail(ErrorCode::unsupported_feature,
                    "native method has no compact implementation");
    }
    const std::shared_ptr<Entry>& entry = (*table)[index];
    if (native_invocation_counting_enabled()) {
        entry->invocation_count.fetch_add(1U, std::memory_order_relaxed);
    }
    PerformanceCounters::record_native_invocation();
    auto result = entry->compact_implementation(machine, arguments);
    if (!result) {
        Error error = result.error();
        if (error.java_exception_class.empty()) {
            error.message = entry->signature.owner + "." +
                            entry->signature.name + entry->signature.descriptor +
                            ": " + error.message;
        }
        return std::unexpected(std::move(error));
    }
    return result;
}

Result<std::optional<Value>> NativeMethodRegistry::invoke(
    Machine& machine,
    NativeMethodId method_id,
    std::span<const Value> arguments) const {
    if (!method_id.valid()) {
        return fail(ErrorCode::unsupported_feature,
                    "native method is not ported");
    }

    const DispatchTable* table = dispatch_table_.load(std::memory_order_acquire);
    const usize index = static_cast<usize>(method_id.value - 1U);
    if (table == nullptr || index >= table->size() ||
        (*table)[index] == nullptr ||
        (*table)[index]->signature.id != method_id) {
        return fail(ErrorCode::unsupported_feature,
                    "native method ID is stale or invalid");
    }
    const std::shared_ptr<Entry>& entry = (*table)[index];
    if (native_invocation_counting_enabled()) {
        entry->invocation_count.fetch_add(1U, std::memory_order_relaxed);
    }
    PerformanceCounters::record_native_invocation();
    auto result = entry->implementation(machine, arguments);
    if (!result) {
        Error error = result.error();
        // Java-visible exception messages are part of the language/API
        // contract. Keep them byte-for-byte intact; only internal native
        // failures receive the method context prefix used for diagnostics.
        if (error.java_exception_class.empty()) {
            error.message = entry->signature.owner + "." +
                            entry->signature.name + entry->signature.descriptor +
                            ": " + error.message;
        }
        return std::unexpected(std::move(error));
    }
    return result;
}

Result<std::optional<Value>> NativeMethodRegistry::invoke(
    Machine& machine,
    std::string_view owner,
    std::string_view name,
    std::string_view descriptor,
    std::span<const Value> arguments) const {
    const NativeMethodId method_id = resolve(owner, name, descriptor);
    if (!method_id.valid()) {
        return fail(ErrorCode::unsupported_feature,
                    "native method is not ported: " + std::string(owner) +
                        "." + std::string(name) + std::string(descriptor));
    }
    return invoke(machine, method_id, arguments);
}

std::vector<NativeMethodSignature>
NativeMethodRegistry::registered_methods() const {
    const DispatchTable* table = dispatch_table_.load(std::memory_order_acquire);
    std::vector<NativeMethodSignature> result;
    if (table == nullptr) return result;
    result.reserve(table->size());
    for (const auto& entry : *table) {
        if (entry != nullptr) result.push_back(entry->signature);
    }
    std::ranges::sort(result, {}, [](const NativeMethodSignature& signature) {
        return std::tie(signature.owner, signature.name, signature.descriptor);
    });
    return result;
}

std::vector<NativeMethodInvocationCount>
NativeMethodRegistry::invocation_counts() const {
    const DispatchTable* table = dispatch_table_.load(std::memory_order_acquire);
    std::vector<NativeMethodInvocationCount> result;
    if (table == nullptr) return result;
    result.reserve(table->size());
    for (const auto& entry : *table) {
        if (entry == nullptr) continue;
        result.push_back(NativeMethodInvocationCount {
            .signature = entry->signature,
            .count = entry->invocation_count.load(std::memory_order_relaxed),
        });
    }
    std::ranges::sort(result, {},
        [](const NativeMethodInvocationCount& entry) {
            return std::tie(entry.signature.owner,
                            entry.signature.name,
                            entry.signature.descriptor);
        });
    return result;
}

void NativeMethodRegistry::reset_invocation_counts() noexcept {
    const DispatchTable* table = dispatch_table_.load(std::memory_order_acquire);
    if (table == nullptr) return;
    for (const auto& entry : *table) {
        if (entry != nullptr) {
            entry->invocation_count.store(0U, std::memory_order_relaxed);
        }
    }
}

u64 NativeMethodRegistry::generation() const noexcept {
    return generation_.load(std::memory_order_acquire);
}

void NativeMethodRegistry::clear() noexcept {
    ProcessNativeCoverage& coverage = process_native_coverage();
    if (coverage.enabled()) coverage.merge(invocation_counts());
    std::scoped_lock lock(mutex_);
    ids_by_key_.clear();
    owners_.clear();
    entries_.clear();
    registration_batch_depth_ = 0U;
    publish_dispatch_table_locked();
    advance_generation_locked();
}

} // namespace phoneme::vm
