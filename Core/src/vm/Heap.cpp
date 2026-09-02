#include "phoneme/vm/Heap.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "phoneme/base/Checked.hpp"
#include "phoneme/runtime/ParallelExecutor.hpp"
#include "phoneme/vm/PerformanceCounters.hpp"
#include "phoneme/vm/VmTrace.hpp"

namespace phoneme::vm {
namespace {

thread_local HeapAccessContext g_heap_access_context {};
constexpr usize kParallelGcSlotThreshold = 4U * 1024U;
constexpr usize kParallelGcSlotChunk = 512U;

#if PHONEME_ENABLE_VM_PROFILING
struct ArrayAllocationSiteStats final {
    u64 count {0U};
    u64 bytes {0U};
    u64 elements {0U};
};

class ArrayAllocationProfiler final {
public:
    ArrayAllocationProfiler() {
        const char* value = std::getenv("PHONEME_TRACE_ARRAY_ALLOC");
        enabled_ = value != nullptr && *value != '\0' &&
                   std::string_view(value) != "0";
    }

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    void record(std::string_view array_class,
                usize length,
                usize bytes) {
        if (!enabled_) return;
        const HeapAccessContext context = current_heap_access_context();
        std::string key;
        key.reserve(array_class.size() + context.owner.size() +
                    context.method.size() + context.descriptor.size() + 32U);
        key.append(array_class);
        key.push_back('\t');
        if (context.owner.empty()) {
            key.append("<host>");
        } else {
            key.append(context.owner);
            key.push_back('.');
            key.append(context.method);
            key.append(context.descriptor);
            key.push_back('@');
            key.append(std::to_string(context.current_bytecode_pc()));
        }
        std::scoped_lock lock(mutex_);
        auto& stats = sites_[std::move(key)];
        ++stats.count;
        stats.bytes += static_cast<u64>(bytes);
        stats.elements += static_cast<u64>(length);
    }

    void record_inline_payload(usize bytes) {
        if (!enabled_) return;
        std::scoped_lock lock(mutex_);
        ++inline_payloads_;
        inline_payload_bytes_ += static_cast<u64>(bytes);
    }


    void dump() noexcept {
        if (!enabled_) return;
        std::vector<std::pair<std::string, ArrayAllocationSiteStats>> entries;
        {
            std::scoped_lock lock(mutex_);
            entries.reserve(sites_.size());
            for (const auto& entry : sites_) entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(), [](const auto& left,
                                                     const auto& right) {
            if (left.second.bytes != right.second.bytes)
                return left.second.bytes > right.second.bytes;
            return left.second.count > right.second.count;
        });
        const usize shown = std::min<usize>(entries.size(), 40U);
        for (usize index = 0U; index < shown; ++index) {
            const auto& [key, stats] = entries[index];
            std::fprintf(stderr,
                         "[phoneME-array-alloc] count=%llu kb=%.1f elements=%llu %s\n",
                         static_cast<unsigned long long>(stats.count),
                         static_cast<double>(stats.bytes) / 1024.0,
                         static_cast<unsigned long long>(stats.elements),
                         key.c_str());
        }
        std::fprintf(stderr,
                     "[phoneME-array-inline] count=%llu payload_kb=%.1f\n",
                     static_cast<unsigned long long>(inline_payloads_),
                     static_cast<double>(inline_payload_bytes_) / 1024.0);
    }

private:
    bool enabled_ {false};
    std::mutex mutex_;
    std::unordered_map<std::string, ArrayAllocationSiteStats> sites_;
    u64 inline_payloads_ {0U};
    u64 inline_payload_bytes_ {0U};
};

ArrayAllocationProfiler& array_allocation_profiler() {
    static ArrayAllocationProfiler* profiler = [] {
        auto* instance = new ArrayAllocationProfiler();
        if (instance->enabled()) {
            std::atexit([] { array_allocation_profiler().dump(); });
        }
        return instance;
    }();
    return *profiler;
}

class ObjectAllocationProfiler final {
public:
    ObjectAllocationProfiler() {
        const char* value = std::getenv("PHONEME_TRACE_OBJECT_ALLOC");
        enabled_ = value != nullptr && *value != '\0' &&
                   std::string_view(value) != "0";
    }

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    void record(std::string_view class_name, usize bytes) {
        if (!enabled_) return;
        const HeapAccessContext context = current_heap_access_context();
        std::string key;
        key.reserve(class_name.size() + context.owner.size() +
                    context.method.size() + context.descriptor.size() + 32U);
        key.append(class_name);
        key.push_back('\t');
        if (context.owner.empty()) {
            key.append("<host>");
        } else {
            key.append(context.owner);
            key.push_back('.');
            key.append(context.method);
            key.append(context.descriptor);
            key.push_back('@');
            key.append(std::to_string(context.current_bytecode_pc()));
        }
        std::scoped_lock lock(mutex_);
        auto& stats = sites_[std::move(key)];
        ++stats.count;
        stats.bytes += static_cast<u64>(bytes);
    }

    void dump() noexcept {
        if (!enabled_) return;
        std::vector<std::pair<std::string, ArrayAllocationSiteStats>> entries;
        {
            std::scoped_lock lock(mutex_);
            entries.reserve(sites_.size());
            for (const auto& entry : sites_) entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(), [](const auto& left,
                                                     const auto& right) {
            if (left.second.bytes != right.second.bytes)
                return left.second.bytes > right.second.bytes;
            return left.second.count > right.second.count;
        });
        const usize shown = std::min<usize>(entries.size(), 40U);
        for (usize index = 0U; index < shown; ++index) {
            const auto& [key, stats] = entries[index];
            std::fprintf(stderr,
                         "[phoneME-object-alloc] count=%llu kb=%.1f %s\n",
                         static_cast<unsigned long long>(stats.count),
                         static_cast<double>(stats.bytes) / 1024.0,
                         key.c_str());
        }
    }

private:
    bool enabled_ {false};
    std::mutex mutex_;
    std::unordered_map<std::string, ArrayAllocationSiteStats> sites_;
};

ObjectAllocationProfiler& object_allocation_profiler() {
    static ObjectAllocationProfiler* profiler = [] {
        auto* instance = new ObjectAllocationProfiler();
        if (instance->enabled()) {
            std::atexit([] { object_allocation_profiler().dump(); });
        }
        return instance;
    }();
    return *profiler;
}
#endif

[[nodiscard]] bool supports_text_payload(std::string_view class_name) noexcept {
    return class_name == "java/lang/String" ||
           class_name == "java/lang/StringBuilder" ||
           class_name == "java/lang/StringBuffer";
}

[[nodiscard]] usize saturated_add(usize left, usize right) noexcept {
    if (right > std::numeric_limits<usize>::max() - left) {
        return std::numeric_limits<usize>::max();
    }
    return left + right;
}

[[nodiscard]] std::optional<HeapArrayKind> heap_array_kind(
    std::string_view class_name) noexcept {
    if (class_name == "[Z") return HeapArrayKind::boolean;
    if (class_name == "[B") return HeapArrayKind::byte;
    if (class_name == "[C") return HeapArrayKind::character;
    if (class_name == "[S") return HeapArrayKind::short_integer;
    if (class_name == "[I") return HeapArrayKind::integer;
    if (class_name == "[J") return HeapArrayKind::long_integer;
    if (class_name == "[F") return HeapArrayKind::float32;
    if (class_name == "[D") return HeapArrayKind::float64;
    if (class_name.starts_with("[L") || class_name.starts_with("[[")) {
        return HeapArrayKind::reference;
    }
    return std::nullopt;
}

[[nodiscard]] Value array_value_from_raw(HeapArrayKind kind,
                                         u64 raw) noexcept {
    switch (kind) {
    case HeapArrayKind::boolean:
        return Value::from_int(raw == 0U ? 0 : 1);
    case HeapArrayKind::byte:
        return Value::from_int(static_cast<i32>(
            static_cast<i8>(static_cast<u8>(raw))));
    case HeapArrayKind::character:
        return Value::from_int(static_cast<i32>(static_cast<u16>(raw)));
    case HeapArrayKind::short_integer:
        return Value::from_int(static_cast<i32>(
            static_cast<i16>(static_cast<u16>(raw))));
    case HeapArrayKind::integer:
        return Value::from_int(static_cast<i32>(static_cast<u32>(raw)));
    case HeapArrayKind::long_integer:
        return Value::from_raw_unchecked(ValueKind::int64, raw);
    case HeapArrayKind::float32:
        return Value::from_raw_unchecked(
            ValueKind::float32, static_cast<u64>(static_cast<u32>(raw)));
    case HeapArrayKind::float64:
        return Value::from_raw_unchecked(ValueKind::float64, raw);
    case HeapArrayKind::reference:
        return Value::from_reference(ObjectRef {raw});
    }
    return {};
}

[[nodiscard]] Result<u64> array_raw_from_value(HeapArrayKind kind,
                                               Value value) {
    // Newly allocated Java arrays are zero initialized. Callers historically
    // used an empty Value to request that default for every array kind.
    if (value.empty()) return 0U;

    switch (kind) {
    case HeapArrayKind::boolean: {
        auto integer = value.as_int();
        if (!integer) return std::unexpected(integer.error());
        return *integer == 0 ? 0U : 1U;
    }
    case HeapArrayKind::byte: {
        auto integer = value.as_int();
        if (!integer) return std::unexpected(integer.error());
        const i32 normalized = static_cast<i32>(static_cast<i8>(*integer));
        return static_cast<u64>(static_cast<u32>(normalized));
    }
    case HeapArrayKind::character: {
        auto integer = value.as_int();
        if (!integer) return std::unexpected(integer.error());
        return static_cast<u64>(static_cast<u16>(*integer));
    }
    case HeapArrayKind::short_integer: {
        auto integer = value.as_int();
        if (!integer) return std::unexpected(integer.error());
        const i32 normalized = static_cast<i32>(static_cast<i16>(*integer));
        return static_cast<u64>(static_cast<u32>(normalized));
    }
    case HeapArrayKind::integer: {
        auto integer = value.as_int();
        if (!integer) return std::unexpected(integer.error());
        return static_cast<u64>(static_cast<u32>(*integer));
    }
    case HeapArrayKind::long_integer: {
        auto integer = value.as_long();
        if (!integer) return std::unexpected(integer.error());
        return static_cast<u64>(*integer);
    }
    case HeapArrayKind::float32: {
        auto number = value.as_float();
        if (!number) return std::unexpected(number.error());
        return static_cast<u64>(static_cast<u32>(value.raw_bits_unchecked()));
    }
    case HeapArrayKind::float64: {
        auto number = value.as_double();
        if (!number) return std::unexpected(number.error());
        return value.raw_bits_unchecked();
    }
    case HeapArrayKind::reference: {
        auto reference = value.as_reference();
        if (!reference) return std::unexpected(reference.error());
        return reference->bits;
    }
    }
    return fail(ErrorCode::invalid_state, "unknown Java array kind");
}

[[nodiscard]] std::string_view reference_array_component(
    std::string_view class_name) {
    if (class_name.starts_with("[L") && class_name.ends_with(';') &&
        class_name.size() >= 4U) {
        return class_name.substr(2U, class_name.size() - 3U);
    }
    if (class_name.starts_with("[[")) {
        return class_name.substr(1U);
    }
    return {};
}

[[nodiscard]] std::unexpected<Error> heap_access_error(
    Error error,
    std::string_view operation,
    ObjectRef reference) {
    const std::string detail = std::move(error.message);
    error.message = std::string(operation) + " failed for object slot " +
                    std::to_string(reference.slot()) + " generation " +
                    std::to_string(reference.generation());
    if (!g_heap_access_context.owner.empty()) {
        error.message += " while executing " +
                         std::string(g_heap_access_context.owner) + "." +
                         std::string(g_heap_access_context.method) +
                         std::string(g_heap_access_context.descriptor) +
                         " at bytecode " +
                         std::to_string(
                             g_heap_access_context.current_bytecode_pc());
    }
    error.message += ": " + detail;
    return std::unexpected(std::move(error));
}

} // namespace

HeapAccessContext current_heap_access_context() noexcept {
    return g_heap_access_context;
}

void set_heap_access_context(HeapAccessContext context) noexcept {
    g_heap_access_context = context;
}

Heap::ArrayPayload::ArrayPayload(usize size, bool allow_inline) {
    if (allow_inline && size <= kInlineCapacity) {
        encoded_size_ = size;
        std::memset(storage_.inline_bytes, 0, kInlineCapacity);
        return;
    }
    storage_.heap = size == 0U ? nullptr : new u8[size]();
    encoded_size_ = size | kHeapFlag;
}

Heap::ArrayPayload::ArrayPayload(const ArrayPayload& other) {
    const usize bytes = other.size();
    if (other.inline_storage()) {
        encoded_size_ = bytes;
        std::memcpy(storage_.inline_bytes,
                    other.storage_.inline_bytes,
                    kInlineCapacity);
        return;
    }
    storage_.heap = bytes == 0U ? nullptr : new u8[bytes];
    encoded_size_ = bytes | kHeapFlag;
    if (bytes != 0U) std::memcpy(storage_.heap, other.storage_.heap, bytes);
}

Heap::ArrayPayload::ArrayPayload(ArrayPayload&& other) noexcept {
    if (other.inline_storage()) {
        encoded_size_ = other.encoded_size_;
        std::memcpy(storage_.inline_bytes,
                    other.storage_.inline_bytes,
                    kInlineCapacity);
    } else {
        encoded_size_ = other.encoded_size_;
        storage_.heap = other.storage_.heap;
        other.storage_.heap = nullptr;
    }
    other.encoded_size_ = 0U;
}

Heap::ArrayPayload& Heap::ArrayPayload::operator=(const ArrayPayload& other) {
    if (this == &other) return *this;
    ArrayPayload copy(other);
    return *this = std::move(copy);
}

Heap::ArrayPayload& Heap::ArrayPayload::operator=(ArrayPayload&& other) noexcept {
    if (this == &other) return *this;
    if (!inline_storage()) delete[] storage_.heap;
    if (other.inline_storage()) {
        encoded_size_ = other.encoded_size_;
        std::memcpy(storage_.inline_bytes,
                    other.storage_.inline_bytes,
                    kInlineCapacity);
    } else {
        encoded_size_ = other.encoded_size_;
        storage_.heap = other.storage_.heap;
        other.storage_.heap = nullptr;
    }
    other.encoded_size_ = 0U;
    return *this;
}

Heap::ArrayPayload::~ArrayPayload() {
    if (!inline_storage()) delete[] storage_.heap;
}

usize Heap::ArrayPayload::size() const noexcept {
    return encoded_size_ & kSizeMask;
}

bool Heap::ArrayPayload::inline_storage() const noexcept {
    return (encoded_size_ & kHeapFlag) == 0U;
}

u8* Heap::ArrayPayload::data() noexcept {
    return inline_storage() ? storage_.inline_bytes : storage_.heap;
}

const u8* Heap::ArrayPayload::data() const noexcept {
    return inline_storage() ? storage_.inline_bytes : storage_.heap;
}

bool Heap::uses_packed_byte_storage(HeapArrayKind kind) noexcept {
    return kind == HeapArrayKind::byte || kind == HeapArrayKind::boolean;
}

bool Heap::uses_packed_half_storage(HeapArrayKind kind) noexcept {
    return kind == HeapArrayKind::character ||
           kind == HeapArrayKind::short_integer;
}

bool Heap::uses_packed_word_storage(HeapArrayKind kind) noexcept {
    return kind == HeapArrayKind::integer || kind == HeapArrayKind::float32;
}

usize Heap::array_element_width(HeapArrayKind kind) noexcept {
    if (uses_packed_byte_storage(kind)) return sizeof(u8);
    if (uses_packed_half_storage(kind)) return sizeof(u16);
    if (uses_packed_word_storage(kind)) return sizeof(u32);
    return sizeof(u64);
}

usize Heap::compact_field_word_count(usize field_count) noexcept {
    if (field_count == 0U) return 0U;
    const usize kind_words =
        (field_count + kFieldKindsPerWord - 1U) / kFieldKindsPerWord;
    return field_count + kind_words;
}

usize Heap::compact_field_count(const Object& object) noexcept {
    const usize words = object.fields.size();
    if (words == 0U) return 0U;
    // For a valid compact layout:
    //   words = fields + ceil(fields / kFieldKindsPerWord)
    // Therefore the number of kind words is ceil(words / (K + 1)).
    const usize kind_words =
        (words + kFieldKindsPerWord) / (kFieldKindsPerWord + 1U);
    return words - kind_words;
}

std::vector<u64> Heap::compact_fields(std::span<const Value> values) {
    const usize field_count = values.size();
    std::vector<u64> storage(compact_field_word_count(field_count), 0U);
    if (field_count == 0U) return storage;
    for (usize index = 0U; index < field_count; ++index) {
        storage[index] = values[index].raw_bits_unchecked();
        const usize kind_word = field_count + index / kFieldKindsPerWord;
        const usize shift = (index % kFieldKindsPerWord) * kFieldKindBits;
        storage[kind_word] |=
            static_cast<u64>(values[index].kind()) << shift;
    }
    return storage;
}

Result<Value> Heap::field_value_unlocked(const Object& object, usize index) {
    if (object.is_array) {
        return fail(ErrorCode::invalid_state,
                    "field access requires a non-array object");
    }
    const usize field_count = compact_field_count(object);
    if (index >= field_count) {
        return fail(ErrorCode::out_of_range, "object field index is out of range");
    }
    const usize kind_word = field_count + index / kFieldKindsPerWord;
    const usize shift = (index % kFieldKindsPerWord) * kFieldKindBits;
    const u64 kind_bits =
        (object.fields[kind_word] >> shift) & ((1ULL << kFieldKindBits) - 1ULL);
    if (kind_bits > static_cast<u64>(ValueKind::return_address)) {
        return fail(ErrorCode::invalid_state, "object field kind is invalid");
    }
    return Value::from_raw_unchecked(
        static_cast<ValueKind>(kind_bits), object.fields[index]);
}

Status Heap::set_field_value_unlocked(Object& object,
                                      usize index,
                                      Value value) {
    if (object.is_array) {
        return fail(ErrorCode::invalid_state,
                    "field store requires a non-array object");
    }
    const usize field_count = compact_field_count(object);
    if (index >= field_count) {
        return fail(ErrorCode::out_of_range, "object field index is out of range");
    }
    object.fields[index] = value.raw_bits_unchecked();
    const usize kind_word = field_count + index / kFieldKindsPerWord;
    const usize shift = (index % kFieldKindsPerWord) * kFieldKindBits;
    const u64 mask = ((1ULL << kFieldKindBits) - 1ULL) << shift;
    object.fields[kind_word] =
        (object.fields[kind_word] & ~mask) |
        (static_cast<u64>(value.kind()) << shift);
    return {};
}

usize Heap::array_length_unlocked(const Object& object) noexcept {
    return object.is_array ? object.array_length : 0U;
}

Result<Value> Heap::array_value_unlocked(const Object& object, usize index) {
    if (!object.is_array || !object.array_kind.has_value()) {
        return fail(ErrorCode::invalid_state,
                    "array access requires a valid array object");
    }
    if (index >= object.array_length) {
        return fail(ErrorCode::out_of_range, "array index is out of range");
    }
    const HeapArrayKind kind = *object.array_kind;
    const usize width = array_element_width(kind);
    if (width == 0U || index > std::numeric_limits<usize>::max() / width) {
        return fail(ErrorCode::overflow, "array payload offset overflow");
    }
    const usize offset = index * width;
    if (offset > object.array_payload.size() ||
        width > object.array_payload.size() - offset) {
        return fail(ErrorCode::invalid_state,
                    "array payload length is inconsistent");
    }
    u64 raw = 0U;
    std::memcpy(&raw, object.array_payload.data() + offset, width);
    return array_value_from_raw(kind, raw);
}

Status Heap::set_array_value_unlocked(Object& object,
                                      usize index,
                                      Value value) {
    if (!object.is_array || !object.array_kind.has_value()) {
        return fail(ErrorCode::invalid_state,
                    "array store requires a valid array object");
    }
    if (index >= object.array_length) {
        return fail(ErrorCode::out_of_range, "array index is out of range");
    }
    const HeapArrayKind kind = *object.array_kind;
    auto raw = array_raw_from_value(kind, value);
    if (!raw) return std::unexpected(raw.error());
    const usize width = array_element_width(kind);
    if (width == 0U || index > std::numeric_limits<usize>::max() / width) {
        return fail(ErrorCode::overflow, "array payload offset overflow");
    }
    const usize offset = index * width;
    if (offset > object.array_payload.size() ||
        width > object.array_payload.size() - offset) {
        return fail(ErrorCode::invalid_state,
                    "array payload length is inconsistent");
    }
    std::memcpy(object.array_payload.data() + offset, &*raw, width);
    return {};
}

Heap::Heap(usize maximum_objects)
    : Heap(HeapLimits {.maximum_objects = maximum_objects}) {}

Heap::Heap(HeapLimits limits) : limits_(limits) {
    const usize hard_limit =
        static_cast<usize>(std::numeric_limits<u32>::max()) - 1U;
    limits_.maximum_objects = std::min(limits_.maximum_objects, hard_limit);
    update_automatic_collection_threshold_unlocked();
}

std::string_view Heap::intern_class_name(std::string_view class_name) {
    const usize cache_index =
        std::hash<std::string_view>{}(class_name) & (kClassNameCacheSize - 1U);
    const std::string* cached =
        class_name_cache_[cache_index].load(std::memory_order_acquire);
    if (cached != nullptr && *cached == class_name) {
        return *cached;
    }

    std::scoped_lock lock(class_names_mutex_);
    const auto [iterator, inserted] = class_names_.emplace(class_name);
    (void)inserted;
    const std::string* interned = &*iterator;
    class_name_cache_[cache_index].store(interned, std::memory_order_release);
    return *interned;
}

Result<ObjectRef> Heap::allocate_object(std::string class_name,
                                        usize field_count) {
    return allocate_object_with_fields(
        std::move(class_name), std::vector<Value>(field_count));
}

Result<ObjectRef> Heap::allocate_object(
    std::string class_name,
    std::span<const Value> initial_fields) {
    return allocate_object_with_fields(
        std::move(class_name),
        std::vector<Value>(initial_fields.begin(), initial_fields.end()));
}

Result<ObjectRef> Heap::allocate_object_with_fields(
    std::string class_name,
    std::vector<Value> fields) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::allocation);
    if (class_name.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "object class name must not be empty");
    }
    std::scoped_lock lock(mutex_);
    auto object_bytes = estimate_object_bytes(
        class_name, fields.size(), 0U, 0U);
    if (!object_bytes) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        return std::unexpected(object_bytes.error());
    }
    auto capacity = ensure_capacity_unlocked(*object_bytes);
    if (!capacity) {
        return std::unexpected(capacity.error());
    }

#if PHONEME_ENABLE_VM_PROFILING
    object_allocation_profiler().record(class_name, *object_bytes);
#endif

    Object object {
        .class_name = intern_class_name(class_name),
        .fields = compact_fields(fields),
        .array_payload = {},
        .string_payload = {},
        .weak_referent = std::nullopt,
        .array_kind = std::nullopt,
        .is_array = false,
        .is_string = false,
        .marked = false,
    };
    auto allocated = allocate_unlocked(std::move(object), *object_bytes);
    if (allocated) {
        PerformanceCounters::record_allocation(
            AllocationPayloadKind::object, *object_bytes);
    }
    return allocated;
}

Result<ObjectRef> Heap::allocate_text_object(
    std::string class_name,
    std::span<const Value> initial_fields,
    std::u16string text) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::allocation);
    if (class_name.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "text object class name must not be empty");
    }
    if (!supports_text_payload(class_name)) {
        return fail(ErrorCode::invalid_argument,
                    "text object class does not support a text payload");
    }
    std::scoped_lock lock(mutex_);
    auto base_bytes = estimate_object_bytes(
        class_name, initial_fields.size(), 0U, 0U);
    auto object_bytes = estimate_object_bytes(
        class_name, initial_fields.size(), 0U, text.size());
    if (!base_bytes || !object_bytes) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        return !base_bytes ? std::unexpected(base_bytes.error())
                           : std::unexpected(object_bytes.error());
    }
    auto capacity = ensure_capacity_unlocked(*object_bytes);
    if (!capacity) return std::unexpected(capacity.error());

#if PHONEME_ENABLE_VM_PROFILING
    object_allocation_profiler().record(class_name, *base_bytes);
#endif

    Object object {
        .class_name = intern_class_name(class_name),
        .fields = compact_fields(initial_fields),
        .array_payload = {},
        .string_payload = std::move(text),
        .weak_referent = std::nullopt,
        .array_kind = std::nullopt,
        .is_array = false,
        .is_string = true,
        .marked = false,
    };
    auto allocated = allocate_unlocked(std::move(object), *object_bytes);
    if (allocated) {
        PerformanceCounters::record_allocation(
            AllocationPayloadKind::object, *base_bytes);
        if (*object_bytes > *base_bytes) {
            PerformanceCounters::record_allocation(
                AllocationPayloadKind::string_payload,
                *object_bytes - *base_bytes);
        }
    }
    return allocated;
}

Result<ObjectRef> Heap::allocate_array(std::string class_name,
                                       usize length,
                                       Value initial_value) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::allocation);
    if (class_name.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "array class name must not be empty");
    }
    std::scoped_lock lock(mutex_);
    const auto array_kind = heap_array_kind(class_name);
    if (!array_kind.has_value()) {
        return fail(ErrorCode::invalid_argument,
                    "array class name has an invalid descriptor");
    }
    auto initial_raw = array_raw_from_value(*array_kind, initial_value);
    if (!initial_raw) return std::unexpected(initial_raw.error());
    auto object_bytes = estimate_object_bytes(class_name, 0U, length, 0U);
    if (!object_bytes) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        return std::unexpected(object_bytes.error());
    }
    auto capacity = ensure_capacity_unlocked(*object_bytes);
    if (!capacity) {
        return std::unexpected(capacity.error());
    }

#if PHONEME_ENABLE_VM_PROFILING
    array_allocation_profiler().record(class_name, length, *object_bytes);
#endif

    const usize element_width = array_element_width(*array_kind);
    auto payload_bytes = checked_multiply(length, element_width);
    if (!payload_bytes) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        return std::unexpected(payload_bytes.error());
    }
    ArrayPayload payload(
        *payload_bytes, uses_packed_byte_storage(*array_kind));
    if (*initial_raw != 0U && element_width != 0U) {
        for (usize index = 0U; index < length; ++index) {
            std::memcpy(payload.data() + index * element_width,
                        &*initial_raw,
                        element_width);
        }
    }
    Object object {
        .class_name = intern_class_name(class_name),
        .fields = {},
        .array_payload = std::move(payload),
        .array_length = length,
        .string_payload = {},
        .weak_referent = std::nullopt,
        .array_kind = array_kind,
        .is_array = true,
        .is_string = false,
        .marked = false,
    };
    auto allocated = allocate_unlocked(std::move(object), *object_bytes);
    if (allocated) {
        PerformanceCounters::record_allocation(
            AllocationPayloadKind::array, *object_bytes);
    }
    return allocated;
}

Result<ObjectRef> Heap::clone_object(ObjectRef reference) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::allocation);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.clone_object", reference);
    }

    const Object& source = slots_[*slot].object;
    auto capacity = ensure_capacity_unlocked(slots_[*slot].accounted_bytes);
    if (!capacity) {
        return std::unexpected(capacity.error());
    }

    Object clone = source;
    clone.marked = false;
    const usize clone_bytes = slots_[*slot].accounted_bytes;
    auto allocated = allocate_unlocked(std::move(clone), clone_bytes);
    if (allocated) {
        PerformanceCounters::record_allocation(
            AllocationPayloadKind::clone, clone_bytes);
    }
    return allocated;
}

Result<Value> Heap::field(ObjectRef reference, usize index) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::field);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.field", reference);
    }
    return field_value_unlocked(slots_[*slot].object, index);
}

Status Heap::read_fields(ObjectRef reference,
                         std::span<const usize> indices,
                         std::span<Value> values) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::field);
    if (indices.size() != values.size()) {
        return fail(ErrorCode::invalid_argument,
                    "field snapshot index/value counts differ");
    }
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.read_fields", reference);
    }
    const Object& object = slots_[*slot].object;
    if (object.is_array) {
        return fail(ErrorCode::invalid_state,
                    "field snapshot requires a non-array object");
    }
    const usize field_count = compact_field_count(object);
    for (const usize index : indices) {
        if (index >= field_count) {
            return fail(ErrorCode::out_of_range,
                        "object field index is out of range");
        }
    }
    for (usize index = 0U; index < indices.size(); ++index) {
        auto value = field_value_unlocked(object, indices[index]);
        if (!value) return std::unexpected(value.error());
        values[index] = *value;
    }
    return {};
}

Status Heap::write_fields(ObjectRef reference,
                          std::span<const usize> indices,
                          std::span<const Value> values) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::field);
    if (indices.size() != values.size()) {
        return fail(ErrorCode::invalid_argument,
                    "field update index/value counts differ");
    }
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.write_fields", reference);
    }
    Object& object = slots_[*slot].object;
    if (object.is_array) {
        return fail(ErrorCode::invalid_state,
                    "field update requires a non-array object");
    }
    const usize field_count = compact_field_count(object);
    for (const usize index : indices) {
        if (index >= field_count) {
            return fail(ErrorCode::out_of_range,
                        "object field index is out of range");
        }
    }
    for (usize index = 0U; index < indices.size(); ++index) {
        auto stored = set_field_value_unlocked(
            object, indices[index], values[index]);
        if (!stored) return stored;
    }
    return {};
}

Status Heap::set_field(ObjectRef reference, usize index, Value value) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::field);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.set_field", reference);
    }
    return set_field_value_unlocked(slots_[*slot].object, index, value);
}

Result<ObjectRef> Heap::vm_allocate_object(
    std::string_view class_name,
    std::span<const Value> initial_fields) {
    PerformanceCounters::record_vm_fast_heap_operation();
    if (class_name.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "object class name must not be empty");
    }
    auto object_bytes = estimate_object_bytes(
        class_name, initial_fields.size(), 0U, 0U);
    if (!object_bytes) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        return std::unexpected(object_bytes.error());
    }
    auto capacity = ensure_capacity_unlocked(*object_bytes);
    if (!capacity) return std::unexpected(capacity.error());

#if PHONEME_ENABLE_VM_PROFILING
    object_allocation_profiler().record(class_name, *object_bytes);
#endif

    Object object {
        .class_name = intern_class_name(class_name),
        .fields = compact_fields(initial_fields),
        .array_payload = {},
        .array_length = 0U,
        .string_payload = {},
        .weak_referent = std::nullopt,
        .array_kind = std::nullopt,
        .is_array = false,
        .is_string = false,
        .marked = false,
    };
    auto allocated = allocate_unlocked(std::move(object), *object_bytes);
    if (allocated) {
        PerformanceCounters::record_allocation(
            AllocationPayloadKind::object, *object_bytes);
    }
    return allocated;
}

Result<ObjectRef> Heap::vm_allocate_array(std::string_view class_name,
                                          usize length,
                                          Value initial_value) {
    PerformanceCounters::record_vm_fast_heap_operation();
    if (class_name.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "array class name must not be empty");
    }
    const auto array_kind = heap_array_kind(class_name);
    if (!array_kind.has_value()) {
        return fail(ErrorCode::invalid_argument,
                    "array class name has an invalid descriptor");
    }
    auto initial_raw = array_raw_from_value(*array_kind, initial_value);
    if (!initial_raw) return std::unexpected(initial_raw.error());
    auto object_bytes = estimate_object_bytes(class_name, 0U, length, 0U);
    if (!object_bytes) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        return std::unexpected(object_bytes.error());
    }
    auto capacity = ensure_capacity_unlocked(*object_bytes);
    if (!capacity) return std::unexpected(capacity.error());

#if PHONEME_ENABLE_VM_PROFILING
    array_allocation_profiler().record(class_name, length, *object_bytes);
#endif

    const usize element_width = array_element_width(*array_kind);
    auto payload_bytes = checked_multiply(length, element_width);
    if (!payload_bytes) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        return std::unexpected(payload_bytes.error());
    }
    ArrayPayload payload(
        *payload_bytes, uses_packed_byte_storage(*array_kind));
    if (*initial_raw != 0U && element_width != 0U) {
        for (usize index = 0U; index < length; ++index) {
            std::memcpy(payload.data() + index * element_width,
                        &*initial_raw,
                        element_width);
        }
    }
    Object object {
        .class_name = intern_class_name(class_name),
        .fields = {},
        .array_payload = std::move(payload),
        .array_length = length,
        .string_payload = {},
        .weak_referent = std::nullopt,
        .array_kind = array_kind,
        .is_array = true,
        .is_string = false,
        .marked = false,
    };
    auto allocated = allocate_unlocked(std::move(object), *object_bytes);
    if (allocated) {
        PerformanceCounters::record_allocation(
            AllocationPayloadKind::array, *object_bytes);
    }
    return allocated;
}

Result<Value> Heap::vm_field(ObjectRef reference, usize index) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.vm_field", reference);
    }
    return field_value_unlocked(slots_[*slot].object, index);
}

Result<Value> Heap::vm_field_typed(ObjectRef reference,
                                   usize index,
                                   ValueKind expected_kind) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.vm_field_typed", reference);
    }
    const Object& object = slots_[*slot].object;
    if (object.is_array || index >= compact_field_count(object)) {
        return fail(ErrorCode::out_of_range, "object field index is out of range");
    }
    return Value::from_raw_unchecked(expected_kind, object.fields[index]);
}

Status Heap::vm_set_field(ObjectRef reference, usize index, Value value) {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.vm_set_field", reference);
    }
    return set_field_value_unlocked(slots_[*slot].object, index, value);
}

Status Heap::vm_set_field_typed(ObjectRef reference,
                                usize index,
                                ValueKind expected_kind,
                                Value value) {
    PerformanceCounters::record_vm_fast_heap_operation();
    if (value.kind() != expected_kind) {
        return fail(ErrorCode::invalid_state,
                    "typed field store value kind does not match field kind");
    }
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(
            slot.error(), "Heap.vm_set_field_typed", reference);
    }
    Object& object = slots_[*slot].object;
    if (object.is_array || index >= compact_field_count(object)) {
        return fail(ErrorCode::out_of_range, "object field index is out of range");
    }
    object.fields[index] = value.raw_bits_unchecked();
    return {};
}

Result<Value> Heap::vm_element(ObjectRef reference, usize index) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.vm_element", reference);
    }
    return array_value_unlocked(slots_[*slot].object, index);
}

Status Heap::vm_set_element(ObjectRef reference, usize index, Value value) {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.vm_set_element", reference);
    }
    return set_array_value_unlocked(slots_[*slot].object, index, value);
}

Result<Value> Heap::element(ObjectRef reference, usize index) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_element_read);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.element", reference);
    }
    return array_value_unlocked(slots_[*slot].object, index);
}

Status Heap::set_element(ObjectRef reference, usize index, Value value) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_element_write);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.set_element", reference);
    }
    return set_array_value_unlocked(slots_[*slot].object, index, value);
}

Result<HeapArrayInfo> Heap::array_info(ObjectRef reference) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_metadata);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.array_info", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array) {
        return fail(ErrorCode::invalid_state, "object is not an array");
    }
    const auto kind = object.array_kind;
    if (!kind.has_value()) {
        return fail(ErrorCode::invalid_state, "array has an invalid class descriptor");
    }
    HeapArrayInfo info {
        .kind = *kind,
        .length = object.array_length,
    };
    if (*kind == HeapArrayKind::reference) {
        info.reference_component = reference_array_component(object.class_name);
        if (info.reference_component.empty()) {
            return fail(ErrorCode::invalid_state,
                        "reference array has an invalid component descriptor");
        }
    }
    return info;
}

Result<HeapArrayInfo> Heap::vm_array_info(ObjectRef reference) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.vm_array_info", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array) {
        return fail(ErrorCode::invalid_state, "object is not an array");
    }
    const auto kind = object.array_kind;
    if (!kind.has_value()) {
        return fail(ErrorCode::invalid_state, "array has an invalid class descriptor");
    }
    HeapArrayInfo info {
        .kind = *kind,
        .length = object.array_length,
    };
    if (*kind == HeapArrayKind::reference) {
        info.reference_component = reference_array_component(object.class_name);
        if (info.reference_component.empty()) {
            return fail(ErrorCode::invalid_state,
                        "reference array has an invalid component descriptor");
        }
    }
    return info;
}

Result<HeapArrayElementSnapshot> Heap::array_element_snapshot(
    ObjectRef reference,
    usize index) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_element_snapshot);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(
            slot.error(), "Heap.array_element_snapshot", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array) {
        return fail(ErrorCode::invalid_state, "object is not an array");
    }
    if (index >= object.array_length) {
        return fail(ErrorCode::out_of_range, "array index is out of range");
    }
    const auto kind = object.array_kind;
    if (!kind.has_value()) {
        return fail(ErrorCode::invalid_state, "array has an invalid class descriptor");
    }
    auto value = array_value_unlocked(object, index);
    if (!value) return std::unexpected(value.error());
    return HeapArrayElementSnapshot {
        .kind = *kind,
        .value = *value,
    };
}

Result<HeapArrayElementSnapshot> Heap::vm_array_element_snapshot(
    ObjectRef reference,
    usize index) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(
            slot.error(), "Heap.vm_array_element_snapshot", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array) {
        return fail(ErrorCode::invalid_state, "object is not an array");
    }
    if (index >= object.array_length) {
        return fail(ErrorCode::out_of_range, "array index is out of range");
    }
    const auto kind = object.array_kind;
    if (!kind.has_value()) {
        return fail(ErrorCode::invalid_state, "array has an invalid class descriptor");
    }
    auto value = array_value_unlocked(object, index);
    if (!value) return std::unexpected(value.error());
    return HeapArrayElementSnapshot {
        .kind = *kind,
        .value = *value,
    };
}

Result<HeapArrayRawElementSnapshot> Heap::vm_array_raw_element_snapshot(
    ObjectRef reference,
    usize index) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(
            slot.error(), "Heap.vm_array_raw_element_snapshot", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array || !object.array_kind.has_value()) {
        return fail(ErrorCode::invalid_state, "object is not a valid array");
    }
    if (index >= object.array_length) {
        return fail(ErrorCode::out_of_range, "array index is out of range");
    }
    const HeapArrayKind kind = *object.array_kind;
    const usize width = array_element_width(kind);
    if (width == 0U || index > std::numeric_limits<usize>::max() / width) {
        return fail(ErrorCode::overflow, "array payload offset overflow");
    }
    const usize offset = index * width;
    if (offset > object.array_payload.size() ||
        width > object.array_payload.size() - offset) {
        return fail(ErrorCode::invalid_state,
                    "array payload length is inconsistent");
    }
    u64 raw = 0U;
    std::memcpy(&raw, object.array_payload.data() + offset, width);
    return HeapArrayRawElementSnapshot {.kind = kind, .raw = raw};
}

Status Heap::vm_set_array_raw_element_checked(ObjectRef reference,
                                              usize index,
                                              HeapArrayKind expected_kind,
                                              u64 raw) {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(
            slot.error(), "Heap.vm_set_array_raw_element_checked", reference);
    }
    Object& object = slots_[*slot].object;
    if (!object.is_array || !object.array_kind.has_value()) {
        return fail(ErrorCode::invalid_state, "object is not a valid array");
    }
    if (index >= object.array_length) {
        return fail(ErrorCode::out_of_range, "array index is out of range");
    }
    if (*object.array_kind != expected_kind) {
        return fail(ErrorCode::invalid_state,
                    "array element kind does not match requested store");
    }
    const usize width = array_element_width(expected_kind);
    if (width == 0U || index > std::numeric_limits<usize>::max() / width) {
        return fail(ErrorCode::overflow, "array payload offset overflow");
    }
    const usize offset = index * width;
    if (offset > object.array_payload.size() ||
        width > object.array_payload.size() - offset) {
        return fail(ErrorCode::invalid_state,
                    "array payload length is inconsistent");
    }
    std::memcpy(object.array_payload.data() + offset, &raw, width);
    return {};
}

Status Heap::set_element_checked(ObjectRef reference,
                                 usize index,
                                 HeapArrayKind expected_kind,
                                 Value value) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_element_checked);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(
            slot.error(), "Heap.set_element_checked", reference);
    }
    Object& object = slots_[*slot].object;
    if (!object.is_array) {
        return fail(ErrorCode::invalid_state, "object is not an array");
    }
    if (index >= object.array_length) {
        return fail(ErrorCode::out_of_range, "array index is out of range");
    }
    const auto actual_kind = object.array_kind;
    if (!actual_kind.has_value() || *actual_kind != expected_kind) {
        return fail(ErrorCode::invalid_state,
                    "array element kind does not match requested store");
    }
    return set_array_value_unlocked(object, index, value);
}

Status Heap::vm_set_element_checked(ObjectRef reference,
                                    usize index,
                                    HeapArrayKind expected_kind,
                                    Value value) {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(
            slot.error(), "Heap.vm_set_element_checked", reference);
    }
    Object& object = slots_[*slot].object;
    if (!object.is_array) {
        return fail(ErrorCode::invalid_state, "object is not an array");
    }
    if (index >= object.array_length) {
        return fail(ErrorCode::out_of_range, "array index is out of range");
    }
    const auto actual_kind = object.array_kind;
    if (!actual_kind.has_value() || *actual_kind != expected_kind) {
        return fail(ErrorCode::invalid_state,
                    "array element kind does not match requested store");
    }
    return set_array_value_unlocked(object, index, value);
}

Result<HeapArrayPayloadLease> Heap::array_payload_lease(
    ObjectRef reference,
    HeapArrayKind expected_kind) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_metadata);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(
            slot.error(), "Heap.array_payload_lease", reference);
    }
    Object& object = slots_[*slot].object;
    if (!object.is_array) {
        return fail(ErrorCode::invalid_state, "object is not an array");
    }
    if (!object.array_kind.has_value() ||
        *object.array_kind != expected_kind) {
        return fail(ErrorCode::invalid_state,
                    "array payload kind does not match requested lease");
    }
    if (uses_packed_byte_storage(expected_kind)) {
        return fail(ErrorCode::unsupported_feature,
                    "packed byte/boolean arrays do not expose u64 JIT leases");
    }
    return HeapArrayPayloadLease {
        .first_payload = object.array_payload.empty()
            ? nullptr
            : static_cast<void*>(object.array_payload.data()),
        .length = object.array_length,
    };
}

Status Heap::fill_array_range(ObjectRef reference,
                              usize index,
                              usize length,
                              Value value) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.fill_array_range", reference);
    }
    Object& object = slots_[*slot].object;
    if (!object.is_array) {
        return fail(ErrorCode::invalid_state,
                    "array range fill requires an array object");
    }
    if (index > object.array_length ||
        length > object.array_length - index) {
        return fail(ErrorCode::out_of_range,
                    "array range fill is outside array bounds");
    }
    if (!object.array_kind.has_value()) {
        return fail(ErrorCode::invalid_state,
                    "array has an invalid class descriptor");
    }
    auto raw = array_raw_from_value(*object.array_kind, value);
    if (!raw) return std::unexpected(raw.error());
    const usize width = array_element_width(*object.array_kind);
    if (*raw == 0U) {
        std::memset(object.array_payload.data() + index * width,
                    0,
                    length * width);
    } else {
        for (usize element_index = 0U; element_index < length;
             ++element_index) {
            std::memcpy(object.array_payload.data() +
                            (index + element_index) * width,
                        &*raw,
                        width);
        }
    }
    return {};
}

Status Heap::copy_array_range(ObjectRef source,
                              usize source_index,
                              ObjectRef destination,
                              usize destination_index,
                              usize length) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto source_slot = resolve_slot_unlocked(source);
    if (!source_slot) {
        return heap_access_error(source_slot.error(),
                                 "Heap.copy_array_range source", source);
    }
    auto destination_slot = resolve_slot_unlocked(destination);
    if (!destination_slot) {
        return heap_access_error(destination_slot.error(),
                                 "Heap.copy_array_range destination",
                                 destination);
    }

    Object& source_object = slots_[*source_slot].object;
    Object& destination_object = slots_[*destination_slot].object;
    if (!source_object.is_array || !destination_object.is_array) {
        return fail(ErrorCode::invalid_state,
                    "array range copy requires array objects");
    }
    if (source_index > source_object.array_length ||
        length > source_object.array_length - source_index ||
        destination_index > destination_object.array_length ||
        length > destination_object.array_length - destination_index) {
        return fail(ErrorCode::out_of_range,
                    "array range copy is outside array bounds");
    }
    if (length == 0U) return {};

    if (!source_object.array_kind.has_value() ||
        !destination_object.array_kind.has_value()) {
        return fail(ErrorCode::invalid_state,
                    "array range copy requires valid array kinds");
    }
    const HeapArrayKind source_kind = *source_object.array_kind;
    const HeapArrayKind destination_kind = *destination_object.array_kind;
    if (source_kind != destination_kind &&
        (source_kind != HeapArrayKind::reference ||
         destination_kind != HeapArrayKind::reference)) {
        return fail(ErrorCode::invalid_argument,
                    "array range copy storage kinds do not match");
    }
    const usize width = array_element_width(source_kind);
    std::memmove(destination_object.array_payload.data() +
                     destination_index * width,
                 source_object.array_payload.data() + source_index * width,
                 length * width);
    return {};
}

Result<bool> Heap::try_copy_primitive_array_range(
    ObjectRef source,
    usize source_index,
    ObjectRef destination,
    usize destination_index,
    usize length) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    return try_copy_primitive_array_range_unlocked(
        source, source_index, destination, destination_index, length);
}

Result<bool> Heap::vm_try_copy_primitive_array_range(
    ObjectRef source,
    usize source_index,
    ObjectRef destination,
    usize destination_index,
    usize length) {
    PerformanceCounters::record_vm_fast_heap_operation();
    return try_copy_primitive_array_range_unlocked(
        source, source_index, destination, destination_index, length);
}

Result<bool> Heap::try_copy_primitive_array_range_unlocked(
    ObjectRef source,
    usize source_index,
    ObjectRef destination,
    usize destination_index,
    usize length) {
    auto source_slot = resolve_slot_unlocked(source);
    if (!source_slot) {
        return heap_access_error(source_slot.error(),
                                 "Heap.try_copy_primitive_array_range source",
                                 source);
    }
    auto destination_slot = resolve_slot_unlocked(destination);
    if (!destination_slot) {
        return heap_access_error(
            destination_slot.error(),
            "Heap.try_copy_primitive_array_range destination",
            destination);
    }

    Object& source_object = slots_[*source_slot].object;
    Object& destination_object = slots_[*destination_slot].object;
    if (!source_object.is_array || !destination_object.is_array ||
        !source_object.array_kind.has_value() ||
        !destination_object.array_kind.has_value()) {
        return fail(ErrorCode::invalid_state,
                    "primitive array copy requires valid array objects");
    }

    const HeapArrayKind source_kind = *source_object.array_kind;
    const HeapArrayKind destination_kind = *destination_object.array_kind;
    const bool source_reference = source_kind == HeapArrayKind::reference;
    const bool destination_reference =
        destination_kind == HeapArrayKind::reference;
    if (source_reference || destination_reference) {
        if (source_reference && destination_reference) return false;
        return fail(ErrorCode::invalid_argument,
                    "primitive and reference array types do not match");
    }
    if (source_kind != destination_kind) {
        return fail(ErrorCode::invalid_argument,
                    "primitive array types do not match");
    }
    if (source_index > source_object.array_length ||
        length > source_object.array_length - source_index ||
        destination_index > destination_object.array_length ||
        length > destination_object.array_length - destination_index) {
        return fail(ErrorCode::out_of_range,
                    "primitive array copy range is outside array bounds");
    }
    if (length == 0U) return true;

    const usize width = array_element_width(source_kind);
    std::memmove(destination_object.array_payload.data() +
                     destination_index * width,
                 source_object.array_payload.data() + source_index * width,
                 length * width);
    return true;
}

Result<std::vector<u8>> Heap::read_byte_array(ObjectRef reference,
                                              usize offset,
                                              usize length) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.read_byte_array", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array || object.class_name != "[B") {
        return fail(ErrorCode::invalid_argument,
                    "byte array read requires byte[]");
    }
    if (offset > object.array_length ||
        length > object.array_length - offset) {
        return fail(ErrorCode::out_of_range,
                    "byte array read is outside array bounds");
    }

    std::vector<u8> bytes(length);
    std::copy_n(object.array_payload.begin() +
                    static_cast<std::ptrdiff_t>(offset),
                length,
                bytes.begin());
    return bytes;
}

Result<std::vector<u8>> Heap::read_byte_array(ObjectRef reference) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.read_byte_array", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array || object.class_name != "[B") {
        return fail(ErrorCode::invalid_argument,
                    "byte array read requires byte[]");
    }
    return std::vector<u8>(object.array_payload.begin(),
                           object.array_payload.end());
}

Result<std::vector<i32>> Heap::read_int_array(ObjectRef reference) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.read_int_array", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array || object.class_name != "[I") {
        return fail(ErrorCode::invalid_argument,
                    "int array read requires int[]");
    }
    std::vector<i32> values(object.array_length);
    for (usize index = 0U; index < object.array_length; ++index) {
        u32 raw = 0U;
        std::memcpy(&raw,
                    object.array_payload.data() + index * sizeof(u32),
                    sizeof(raw));
        values[index] = static_cast<i32>(raw);
    }
    return values;
}

Result<std::vector<i16>> Heap::read_short_array(ObjectRef reference) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.read_short_array", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array || object.class_name != "[S") {
        return fail(ErrorCode::invalid_argument,
                    "short array read requires short[]");
    }
    std::vector<i16> values(object.array_length);
    for (usize index = 0U; index < object.array_length; ++index) {
        u16 raw = 0U;
        std::memcpy(&raw,
                    object.array_payload.data() + index * sizeof(u16),
                    sizeof(raw));
        values[index] = static_cast<i16>(raw);
    }
    return values;
}

Result<std::vector<float>> Heap::read_float_array(ObjectRef reference) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.read_float_array", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array || object.class_name != "[F") {
        return fail(ErrorCode::invalid_argument,
                    "float array read requires float[]");
    }
    std::vector<float> values(object.array_length);
    for (usize index = 0U; index < object.array_length; ++index) {
        u32 raw = 0U;
        std::memcpy(&raw,
                    object.array_payload.data() + index * sizeof(u32),
                    sizeof(raw));
        values[index] = std::bit_cast<float>(raw);
    }
    return values;
}

Result<std::vector<u8>> Heap::read_boolean_array(ObjectRef reference) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.read_boolean_array", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array || object.class_name != "[Z") {
        return fail(ErrorCode::invalid_argument,
                    "boolean array read requires boolean[]");
    }
    std::vector<u8> values(object.array_payload.size());
    std::transform(object.array_payload.begin(),
                   object.array_payload.end(),
                   values.begin(),
                   [](u8 element) { return element == 0U ? 0U : 1U; });
    return values;
}

Result<std::vector<ObjectRef>> Heap::read_reference_array(
    ObjectRef reference) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.read_reference_array", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array || !object.array_kind.has_value() ||
        *object.array_kind != HeapArrayKind::reference) {
        return fail(ErrorCode::invalid_argument,
                    "reference array read requires a reference array");
    }
    std::vector<ObjectRef> values(object.array_length);
    for (usize index = 0U; index < object.array_length; ++index) {
        u64 raw = 0U;
        std::memcpy(&raw,
                    object.array_payload.data() + index * sizeof(u64),
                    sizeof(raw));
        values[index] = ObjectRef {raw};
    }
    return values;
}

Status Heap::write_reference_array(ObjectRef reference,
                                   usize offset,
                                   std::span<const ObjectRef> values) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.write_reference_array", reference);
    }
    Object& object = slots_[*slot].object;
    if (!object.is_array || !object.array_kind.has_value() ||
        *object.array_kind != HeapArrayKind::reference) {
        return fail(ErrorCode::invalid_argument,
                    "reference array write requires a reference array");
    }
    if (offset > object.array_length ||
        values.size() > object.array_length - offset) {
        return fail(ErrorCode::out_of_range,
                    "reference array write is outside array bounds");
    }
    for (usize index = 0; index < values.size(); ++index) {
        const u64 raw = values[index].bits;
        std::memcpy(object.array_payload.data() +
                        (offset + index) * sizeof(u64),
                    &raw,
                    sizeof(raw));
    }
    return {};
}

Status Heap::write_byte_array(ObjectRef reference,
                              usize offset,
                              std::span<const u8> bytes) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.write_byte_array", reference);
    }
    Object& object = slots_[*slot].object;
    if (!object.is_array || object.class_name != "[B") {
        return fail(ErrorCode::invalid_argument,
                    "byte array write requires byte[]");
    }
    if (offset > object.array_length ||
        bytes.size() > object.array_length - offset) {
        return fail(ErrorCode::out_of_range,
                    "byte array write is outside array bounds");
    }

    std::copy(bytes.begin(), bytes.end(),
              object.array_payload.begin() + static_cast<std::ptrdiff_t>(offset));
    return {};
}

Status Heap::write_int_array(ObjectRef reference,
                             usize offset,
                             std::span<const i32> values) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.write_int_array", reference);
    }
    Object& object = slots_[*slot].object;
    if (!object.is_array || object.class_name != "[I") {
        return fail(ErrorCode::invalid_argument,
                    "int array write requires int[]");
    }
    if (offset > object.array_length ||
        values.size() > object.array_length - offset) {
        return fail(ErrorCode::out_of_range,
                    "int array write is outside array bounds");
    }
    for (usize index = 0; index < values.size(); ++index) {
        const u32 raw = static_cast<u32>(values[index]);
        std::memcpy(object.array_payload.data() +
                        (offset + index) * sizeof(u32),
                    &raw,
                    sizeof(raw));
    }
    return {};
}

Status Heap::write_float_array(ObjectRef reference,
                               usize offset,
                               std::span<const float> values) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.write_float_array", reference);
    }
    Object& object = slots_[*slot].object;
    if (!object.is_array || object.class_name != "[F") {
        return fail(ErrorCode::invalid_argument,
                    "float array write requires float[]");
    }
    if (offset > object.array_length ||
        values.size() > object.array_length - offset) {
        return fail(ErrorCode::out_of_range,
                    "float array write is outside array bounds");
    }
    for (usize index = 0; index < values.size(); ++index) {
        const u32 raw = std::bit_cast<u32>(values[index]);
        std::memcpy(object.array_payload.data() +
                        (offset + index) * sizeof(u32),
                    &raw,
                    sizeof(raw));
    }
    return {};
}

Status Heap::write_boolean_array(ObjectRef reference,
                                 usize offset,
                                 std::span<const u8> values) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.write_boolean_array", reference);
    }
    Object& object = slots_[*slot].object;
    if (!object.is_array || object.class_name != "[Z") {
        return fail(ErrorCode::invalid_argument,
                    "boolean array write requires boolean[]");
    }
    if (offset > object.array_length ||
        values.size() > object.array_length - offset) {
        return fail(ErrorCode::out_of_range,
                    "boolean array write is outside array bounds");
    }
    for (usize index = 0; index < values.size(); ++index) {
        object.array_payload[offset + index] = values[index] == 0U ? 0U : 1U;
    }
    return {};
}

Status Heap::write_char_array(ObjectRef reference,
                              usize offset,
                              std::u16string_view characters) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_bulk);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.write_char_array", reference);
    }
    Object& object = slots_[*slot].object;
    if (!object.is_array || object.class_name != "[C") {
        return fail(ErrorCode::invalid_argument,
                    "char array write requires char[]");
    }
    if (offset > object.array_length ||
        characters.size() > object.array_length - offset) {
        return fail(ErrorCode::out_of_range,
                    "char array write is outside array bounds");
    }
    for (usize index = 0; index < characters.size(); ++index) {
        const u16 raw = static_cast<u16>(characters[index]);
        std::memcpy(object.array_payload.data() +
                        (offset + index) * sizeof(u16),
                    &raw,
                    sizeof(raw));
    }
    return {};
}

Result<usize> Heap::array_length(ObjectRef reference) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::array_metadata);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.array_length", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array) {
        return fail(ErrorCode::invalid_state, "object is not an array");
    }
    return object.array_length;
}

Result<usize> Heap::vm_array_length(ObjectRef reference) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.vm_array_length", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_array) {
        return fail(ErrorCode::invalid_state, "object is not an array");
    }
    return object.array_length;
}

Result<std::string> Heap::class_name(ObjectRef reference) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::class_name);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.class_name", reference);
    }
    return std::string(slots_[*slot].object.class_name);
}

Result<std::string_view> Heap::vm_class_name_view(ObjectRef reference) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(
            slot.error(), "Heap.vm_class_name_view", reference);
    }
    return slots_[*slot].object.class_name;
}

Status Heap::attach_string(ObjectRef reference, std::u16string value) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::string);
    std::scoped_lock lock(mutex_);
    return attach_string_unlocked(reference, std::move(value));
}

Status Heap::attach_string_unlocked(ObjectRef reference, std::u16string value) {
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.attach_string", reference);
    }
    Slot& target = slots_[*slot];
    Object& object = target.object;
    if (object.is_array || !supports_text_payload(object.class_name)) {
        return fail(ErrorCode::invalid_argument,
                    "text payload can only be attached to String or a string builder");
    }

    auto updated_bytes = estimate_object_bytes(object.class_name,
                                               compact_field_count(object),
                                               0U,
                                               value.size());
    if (!updated_bytes) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        return std::unexpected(updated_bytes.error());
    }
    if (*updated_bytes > target.accounted_bytes) {
        const usize growth = *updated_bytes - target.accounted_bytes;
        if (growth > limits_.maximum_bytes - live_bytes_) {
            ++failed_allocations_;
            PerformanceCounters::record_failed_allocation();
            return fail(ErrorCode::overflow, "Java heap byte limit reached");
        }
        live_bytes_ += growth;
        peak_bytes_ = std::max(peak_bytes_, live_bytes_);
    } else {
        live_bytes_ -= target.accounted_bytes - *updated_bytes;
    }

    const usize previous_bytes = target.accounted_bytes;
    object.string_payload = std::move(value);
    object.is_string = true;
    target.accounted_bytes = *updated_bytes;
    if (*updated_bytes > previous_bytes) {
        PerformanceCounters::record_allocation(
            AllocationPayloadKind::string_payload,
            *updated_bytes - previous_bytes);
    }
    return {};
}

Status Heap::vm_attach_string(ObjectRef reference, std::u16string value) {
    PerformanceCounters::record_vm_fast_heap_operation();
    return attach_string_unlocked(reference, std::move(value));
}

Status Heap::append_mutable_text(ObjectRef reference,
                                 usize capacity_field_index,
                                 std::u16string_view suffix) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::string);
    std::scoped_lock lock(mutex_);
    return append_mutable_text_unlocked(
        reference, capacity_field_index, suffix);
}

Status Heap::append_mutable_text_unlocked(ObjectRef reference,
                                          usize capacity_field_index,
                                          std::u16string_view suffix) {
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(
            slot.error(), "Heap.append_mutable_text", reference);
    }
    Slot& target = slots_[*slot];
    Object& object = target.object;
    if (!object.is_string ||
        (object.class_name != "java/lang/StringBuilder" &&
         object.class_name != "java/lang/StringBuffer")) {
        return fail(ErrorCode::invalid_argument,
                    "mutable text append requires a StringBuilder or StringBuffer");
    }
    if (capacity_field_index >= compact_field_count(object)) {
        return fail(ErrorCode::out_of_range,
                    "mutable text capacity field is out of range");
    }
    auto capacity_value = field_value_unlocked(object, capacity_field_index);
    if (!capacity_value) return std::unexpected(capacity_value.error());
    auto capacity = capacity_value->as_int();
    if (!capacity) return std::unexpected(capacity.error());
    if (suffix.size() > object.string_payload.max_size() -
                            object.string_payload.size()) {
        return fail(ErrorCode::overflow,
                    "mutable text append exceeds host string capacity");
    }
    const usize required = object.string_payload.size() + suffix.size();
    if (required > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return fail(ErrorCode::overflow,
                    "mutable text append exceeds Java capacity range");
    }

    usize grown = static_cast<usize>(std::max(*capacity, 0));
    if (required > grown) {
        if (grown <=
            (static_cast<usize>(std::numeric_limits<i32>::max()) - 2U) / 2U) {
            grown = grown * 2U + 2U;
        } else {
            grown = static_cast<usize>(std::numeric_limits<i32>::max());
        }
        grown = std::max(grown, required);
    }

    auto updated_bytes = estimate_object_bytes(
        object.class_name, compact_field_count(object), 0U, required);
    if (!updated_bytes) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        return std::unexpected(updated_bytes.error());
    }
    if (*updated_bytes > target.accounted_bytes) {
        const usize growth = *updated_bytes - target.accounted_bytes;
        if (live_bytes_ > limits_.maximum_bytes ||
            growth > limits_.maximum_bytes - live_bytes_) {
            ++failed_allocations_;
            PerformanceCounters::record_failed_allocation();
            return fail(ErrorCode::overflow, "Java heap byte limit reached");
        }
    }

    const usize previous_bytes = target.accounted_bytes;
    object.string_payload.append(suffix);
    auto capacity_stored = set_field_value_unlocked(
        object, capacity_field_index, Value::from_int(static_cast<i32>(grown)));
    if (!capacity_stored) return capacity_stored;
    if (*updated_bytes > previous_bytes) {
        live_bytes_ += *updated_bytes - previous_bytes;
        peak_bytes_ = std::max(peak_bytes_, live_bytes_);
        PerformanceCounters::record_allocation(
            AllocationPayloadKind::string_payload,
            *updated_bytes - previous_bytes);
    }
    target.accounted_bytes = *updated_bytes;
    return {};
}

Status Heap::vm_append_mutable_text(ObjectRef reference,
                                    usize capacity_field_index,
                                    std::u16string_view suffix) {
    PerformanceCounters::record_vm_fast_heap_operation();
    return append_mutable_text_unlocked(
        reference, capacity_field_index, suffix);
}

Status Heap::insert_mutable_text(ObjectRef reference,
                                 usize capacity_field_index,
                                 usize offset,
                                 std::u16string_view inserted) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::string);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(
            slot.error(), "Heap.insert_mutable_text", reference);
    }
    Slot& target = slots_[*slot];
    Object& object = target.object;
    if (!object.is_string ||
        (object.class_name != "java/lang/StringBuilder" &&
         object.class_name != "java/lang/StringBuffer")) {
        return fail(ErrorCode::invalid_argument,
                    "mutable text insert requires a StringBuilder or StringBuffer");
    }
    if (capacity_field_index >= compact_field_count(object)) {
        return fail(ErrorCode::out_of_range,
                    "mutable text capacity field is out of range");
    }
    if (offset > object.string_payload.size()) {
        return fail(ErrorCode::out_of_range,
                    "mutable text insert offset is out of range");
    }
    auto capacity_value = field_value_unlocked(object, capacity_field_index);
    if (!capacity_value) return std::unexpected(capacity_value.error());
    auto capacity = capacity_value->as_int();
    if (!capacity) return std::unexpected(capacity.error());
    if (inserted.size() > object.string_payload.max_size() -
                              object.string_payload.size()) {
        return fail(ErrorCode::overflow,
                    "mutable text insert exceeds host string capacity");
    }
    const usize required = object.string_payload.size() + inserted.size();
    if (required > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return fail(ErrorCode::overflow,
                    "mutable text insert exceeds Java capacity range");
    }

    usize grown = static_cast<usize>(std::max(*capacity, 0));
    if (required > grown) {
        if (grown <=
            (static_cast<usize>(std::numeric_limits<i32>::max()) - 2U) / 2U) {
            grown = grown * 2U + 2U;
        } else {
            grown = static_cast<usize>(std::numeric_limits<i32>::max());
        }
        grown = std::max(grown, required);
    }

    auto updated_bytes = estimate_object_bytes(
        object.class_name, compact_field_count(object), 0U, required);
    if (!updated_bytes) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        return std::unexpected(updated_bytes.error());
    }
    if (*updated_bytes > target.accounted_bytes) {
        const usize growth = *updated_bytes - target.accounted_bytes;
        if (live_bytes_ > limits_.maximum_bytes ||
            growth > limits_.maximum_bytes - live_bytes_) {
            ++failed_allocations_;
            PerformanceCounters::record_failed_allocation();
            return fail(ErrorCode::overflow, "Java heap byte limit reached");
        }
    }

    const usize previous_bytes = target.accounted_bytes;
    object.string_payload.insert(offset, inserted);
    auto capacity_stored = set_field_value_unlocked(
        object, capacity_field_index, Value::from_int(static_cast<i32>(grown)));
    if (!capacity_stored) return capacity_stored;
    if (*updated_bytes > previous_bytes) {
        live_bytes_ += *updated_bytes - previous_bytes;
        peak_bytes_ = std::max(peak_bytes_, live_bytes_);
        PerformanceCounters::record_allocation(
            AllocationPayloadKind::string_payload,
            *updated_bytes - previous_bytes);
    }
    target.accounted_bytes = *updated_bytes;
    return {};
}

Result<std::u16string> Heap::string_value(ObjectRef reference) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::string);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.string_value", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_string || !supports_text_payload(object.class_name)) {
        return fail(ErrorCode::invalid_state,
                    "object does not contain a Java text payload");
    }
    return object.string_payload;
}

Result<usize> Heap::string_length(ObjectRef reference) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::string);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.string_length", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_string || !supports_text_payload(object.class_name)) {
        return fail(ErrorCode::invalid_state,
                    "object does not contain a Java text payload");
    }
    return object.string_payload.size();
}

Result<u16> Heap::string_character(ObjectRef reference, usize index) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::string);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.string_character", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_string || !supports_text_payload(object.class_name)) {
        return fail(ErrorCode::invalid_state,
                    "object does not contain a Java text payload");
    }
    if (index >= object.string_payload.size()) {
        return fail(ErrorCode::out_of_range,
                    "Java text character index is out of range");
    }
    return static_cast<u16>(object.string_payload[index]);
}

Result<i32> Heap::string_index_of(ObjectRef reference,
                                  u16 character,
                                  usize start) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::string);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.string_index_of", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_string || !supports_text_payload(object.class_name)) {
        return fail(ErrorCode::invalid_state,
                    "object does not contain a Java text payload");
    }
    if (start > object.string_payload.size()) return -1;
    const usize position = object.string_payload.find(
        static_cast<char16_t>(character), start);
    if (position == std::u16string::npos) return -1;
    if (position > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return fail(ErrorCode::overflow,
                    "Java text character position exceeds int range");
    }
    return static_cast<i32>(position);
}

Result<usize> Heap::vm_string_length(ObjectRef reference) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.vm_string_length", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_string || !supports_text_payload(object.class_name)) {
        return fail(ErrorCode::invalid_state,
                    "object does not contain a Java text payload");
    }
    return object.string_payload.size();
}

Result<std::u16string> Heap::vm_string_value(ObjectRef reference) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.vm_string_value", reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_string || !supports_text_payload(object.class_name)) {
        return fail(ErrorCode::invalid_state,
                    "object does not contain a Java text payload");
    }
    return object.string_payload;
}

Result<u16> Heap::vm_string_character(ObjectRef reference, usize index) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.vm_string_character",
                                 reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_string || !supports_text_payload(object.class_name)) {
        return fail(ErrorCode::invalid_state,
                    "object does not contain a Java text payload");
    }
    if (index >= object.string_payload.size()) {
        return fail(ErrorCode::out_of_range,
                    "Java text character index is out of range");
    }
    return static_cast<u16>(object.string_payload[index]);
}

Result<i32> Heap::vm_string_index_of(ObjectRef reference,
                                     u16 character,
                                     usize start) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.vm_string_index_of",
                                 reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_string || !supports_text_payload(object.class_name)) {
        return fail(ErrorCode::invalid_state,
                    "object does not contain a Java text payload");
    }
    if (start > object.string_payload.size()) return -1;
    const usize position = object.string_payload.find(
        static_cast<char16_t>(character), start);
    if (position == std::u16string::npos) return -1;
    if (position > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return fail(ErrorCode::overflow,
                    "Java text character position exceeds int range");
    }
    return static_cast<i32>(position);
}

Result<bool> Heap::vm_string_starts_with(ObjectRef reference,
                                         ObjectRef prefix,
                                         usize offset) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto text_slot = resolve_slot_vm_fast(reference);
    if (!text_slot) {
        return heap_access_error(text_slot.error(),
                                 "Heap.vm_string_starts_with",
                                 reference);
    }
    const Object& text = slots_[*text_slot].object;
    if (!text.is_string || text.class_name != "java/lang/String") {
        return fail(ErrorCode::invalid_state,
                    "String.startsWith receiver is not a Java String");
    }

    auto prefix_slot = resolve_slot_vm_fast(prefix);
    if (!prefix_slot) {
        return heap_access_error(prefix_slot.error(),
                                 "Heap.vm_string_starts_with",
                                 prefix);
    }
    const Object& prefix_object = slots_[*prefix_slot].object;
    if (!prefix_object.is_string ||
        prefix_object.class_name != "java/lang/String") {
        return fail(ErrorCode::invalid_state,
                    "String.startsWith prefix is not a Java String");
    }
    if (offset > text.string_payload.size() ||
        prefix_object.string_payload.size() >
            text.string_payload.size() - offset) {
        return false;
    }
    return std::equal(prefix_object.string_payload.begin(),
                      prefix_object.string_payload.end(),
                      text.string_payload.begin() +
                          static_cast<std::ptrdiff_t>(offset));
}

Result<std::u16string> Heap::vm_string_slice(ObjectRef reference,
                                             usize begin,
                                             usize end) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    auto slot = resolve_slot_vm_fast(reference);
    if (!slot) {
        return heap_access_error(slot.error(),
                                 "Heap.vm_string_slice",
                                 reference);
    }
    const Object& object = slots_[*slot].object;
    if (!object.is_string || object.class_name != "java/lang/String") {
        return fail(ErrorCode::invalid_state,
                    "String.substring receiver is not a Java String");
    }
    if (begin > end || end > object.string_payload.size()) {
        return fail(ErrorCode::out_of_range,
                    "String substring range is outside the Java text payload");
    }
    return object.string_payload.substr(begin, end - begin);
}

Result<bool> Heap::vm_string_equals(ObjectRef left, ObjectRef right) const {
    PerformanceCounters::record_vm_fast_heap_operation();
    if (right.is_null()) return false;
    if (left == right) return true;

    auto left_slot = resolve_slot_vm_fast(left);
    if (!left_slot) {
        return heap_access_error(left_slot.error(),
                                 "Heap.vm_string_equals",
                                 left);
    }
    const Object& left_object = slots_[*left_slot].object;
    if (!left_object.is_string || left_object.class_name != "java/lang/String") {
        return fail(ErrorCode::invalid_state,
                    "String.equals receiver is not a Java String");
    }

    auto right_slot = resolve_slot_vm_fast(right);
    if (!right_slot) {
        return heap_access_error(right_slot.error(),
                                 "Heap.vm_string_equals",
                                 right);
    }
    const Object& right_object = slots_[*right_slot].object;
    if (!right_object.is_string ||
        right_object.class_name != "java/lang/String") {
        return false;
    }
    return left_object.string_payload == right_object.string_payload;
}

bool Heap::vm_automatic_collection_due() const noexcept {
    PerformanceCounters::record_vm_fast_heap_operation();
    return automatic_collection_threshold_ != 0U &&
           live_bytes_ >= automatic_collection_threshold_;
}

Status Heap::set_weak_referent(ObjectRef reference, ObjectRef referent) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::reference);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.set_weak_referent", reference);
    }
    if (!referent.is_null()) {
        auto referent_slot = resolve_slot_unlocked(referent);
        if (!referent_slot) {
            return heap_access_error(referent_slot.error(),
                                     "Heap.set_weak_referent referent",
                                     referent);
        }
    }
    slots_[*slot].object.weak_referent = referent;
    return {};
}

Result<ObjectRef> Heap::weak_referent(ObjectRef reference) const {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::reference);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.weak_referent", reference);
    }
    const auto referent = slots_[*slot].object.weak_referent;
    if (!referent.has_value() || referent->is_null()) {
        return ObjectRef {};
    }
    auto referent_slot = resolve_slot_unlocked(*referent);
    if (!referent_slot) {
        return ObjectRef {};
    }
    return *referent;
}

Status Heap::clear_weak_referent(ObjectRef reference) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::reference);
    std::scoped_lock lock(mutex_);
    auto slot = resolve_slot_unlocked(reference);
    if (!slot) {
        return heap_access_error(slot.error(), "Heap.clear_weak_referent", reference);
    }
    slots_[*slot].object.weak_referent.reset();
    return {};
}

Status Heap::collect(std::span<const ObjectRef> roots) {
    PerformanceCounters::record_locked_heap_operation(
        LockedHeapOperationKind::garbage_collection);
    const auto collection_started = std::chrono::steady_clock::now();
    std::scoped_lock lock(mutex_);
    // VM-fast resolver entries deliberately skip generation checks on cache
    // hits. A collection can reclaim/reuse slots, so invalidate before any
    // sweep mutation makes an old ObjectRef cache entry stale.
    invalidate_vm_slot_cache_unlocked();
    const usize bytes_before = live_bytes_;
    std::atomic<usize> scanned_objects {0U};
    std::atomic<usize> scanned_primitive_bytes {0U};
    const auto reset_marks = [&](usize begin, usize end) {
        usize local_objects = 0U;
        usize local_primitive_bytes = 0U;
        for (usize index = begin; index < end; ++index) {
            Slot& slot = slots_[index];
            if (slot.occupied) {
                ++local_objects;
                if (slot.object.is_array && slot.object.class_name.size() >= 2U &&
                    slot.object.class_name.front() == '[' &&
                    slot.object.class_name[1U] != 'L' &&
                    slot.object.class_name[1U] != '[') {
                    const usize payload_bytes = slot.object.array_payload.size();
                    local_primitive_bytes = saturated_add(
                        local_primitive_bytes, payload_bytes);
                }
                slot.object.marked = false;
            }
        }
        scanned_objects.fetch_add(local_objects, std::memory_order_relaxed);
        scanned_primitive_bytes.fetch_add(
            local_primitive_bytes, std::memory_order_relaxed);
    };
    runtime::shared_compute_executor().parallel_for(
        slots_.size(),
        kParallelGcSlotThreshold,
        kParallelGcSlotChunk,
        reset_marks);
    const usize objects_scanned =
        scanned_objects.load(std::memory_order_relaxed);
    const usize primitive_bytes_scanned =
        scanned_primitive_bytes.load(std::memory_order_relaxed);
    vm_trace("gc",
             "begin roots=%zu live=%zu bytes=%zu capacity=%zu",
             roots.size(),
             objects_scanned,
             bytes_before,
             slots_.size());

    std::vector<ObjectRef> pending;
    pending.reserve(roots.size());
    for (ObjectRef root : roots) {
        if (!root.is_null()) {
            pending.push_back(root);
        }
    }

    while (!pending.empty()) {
        const ObjectRef reference = pending.back();
        pending.pop_back();
        mark_unlocked(reference, pending);
    }

    for (Slot& slot : slots_) {
        if (!slot.occupied || !slot.object.weak_referent.has_value()) {
            continue;
        }
        const ObjectRef referent = *slot.object.weak_referent;
        if (referent.is_null() || referent.slot() == 0U) {
            slot.object.weak_referent.reset();
            continue;
        }
        const usize referent_index = static_cast<usize>(referent.slot() - 1U);
        if (referent_index >= slots_.size()) {
            slot.object.weak_referent.reset();
            continue;
        }
        const Slot& target = slots_[referent_index];
        if (!target.occupied || target.generation != referent.generation() ||
            !target.object.marked) {
            slot.object.weak_referent.reset();
        }
    }

    std::atomic<usize> reclaimed_objects {0U};
    std::atomic<usize> reclaimed_bytes {0U};
    std::mutex reclaimed_indices_mutex;
    std::vector<usize> reclaimed_indices;
    const auto sweep_slots = [&](usize begin, usize end) {
        usize local_objects = 0U;
        usize local_bytes = 0U;
        std::vector<usize> local_indices;
        for (usize index = begin; index < end; ++index) {
            Slot& slot = slots_[index];
            if (!slot.occupied || slot.object.marked) {
                continue;
            }
            ++local_objects;
            local_bytes = saturated_add(local_bytes, slot.accounted_bytes);
            slot.object = {};
            slot.accounted_bytes = 0U;
            slot.occupied = false;
            ++slot.generation;
            if (slot.generation == 0U) {
                slot.generation = 1U;
            }
            local_indices.push_back(index);
        }
        reclaimed_objects.fetch_add(local_objects, std::memory_order_relaxed);
        reclaimed_bytes.fetch_add(local_bytes, std::memory_order_relaxed);
        if (!local_indices.empty()) {
            std::scoped_lock indices_lock(reclaimed_indices_mutex);
            reclaimed_indices.insert(
                reclaimed_indices.end(),
                local_indices.begin(),
                local_indices.end());
        }
    };
    runtime::shared_compute_executor().parallel_for(
        slots_.size(),
        kParallelGcSlotThreshold,
        kParallelGcSlotChunk,
        sweep_slots);
    std::sort(reclaimed_indices.begin(), reclaimed_indices.end());
    free_slots_.insert(
        free_slots_.end(), reclaimed_indices.begin(), reclaimed_indices.end());
    const usize objects_reclaimed =
        reclaimed_objects.load(std::memory_order_relaxed);
    const usize bytes_reclaimed =
        reclaimed_bytes.load(std::memory_order_relaxed);
    live_bytes_ -= std::min(live_bytes_, bytes_reclaimed);
    ++collections_;
    update_automatic_collection_threshold_unlocked();
    const auto collection_finished = std::chrono::steady_clock::now();
    const auto pause = std::chrono::duration_cast<std::chrono::nanoseconds>(
        collection_finished - collection_started).count();
    PerformanceCounters::record_gc(
        pause > 0 ? static_cast<u64>(pause) : 0U,
        roots.size(),
        objects_scanned,
        objects_reclaimed,
        primitive_bytes_scanned);
    vm_trace("gc",
             "end roots=%zu scanned=%zu reclaimed=%zu live=%zu->%zu "
             "bytes=%zu->%zu pause_us=%lld collections=%llu",
             roots.size(),
             objects_scanned,
             objects_reclaimed,
             objects_scanned,
             objects_scanned - objects_reclaimed,
             bytes_before,
             live_bytes_,
             static_cast<long long>(pause / 1'000),
             static_cast<unsigned long long>(collections_));
    return {};
}

void Heap::clear() noexcept {
    std::scoped_lock lock(mutex_);
    invalidate_vm_slot_cache_unlocked();
    slots_.clear();
    free_slots_.clear();
    live_bytes_ = 0U;
    peak_bytes_ = 0U;
    update_automatic_collection_threshold_unlocked();
    collections_ = 0U;
    failed_allocations_ = 0U;
}

usize Heap::estimated_bytes() const noexcept {
    std::scoped_lock lock(mutex_);
    return live_bytes_;
}

bool Heap::automatic_collection_due() const noexcept {
    std::scoped_lock lock(mutex_);
    return automatic_collection_threshold_ != 0U &&
           live_bytes_ >= automatic_collection_threshold_;
}

HeapStats Heap::stats() const noexcept {
    std::scoped_lock lock(mutex_);
    HeapStats result {
        .estimated_bytes = live_bytes_,
        .peak_estimated_bytes = peak_bytes_,
        .maximum_objects = limits_.maximum_objects,
        .maximum_bytes = limits_.maximum_bytes,
        .collections = collections_,
        .failed_allocations = failed_allocations_,
    };
    for (const Slot& slot : slots_) {
        if (!slot.occupied) {
            continue;
        }
        ++result.live_objects;
        result.live_slots = saturated_add(
            result.live_slots,
            saturated_add(compact_field_count(slot.object),
                          slot.object.is_array
                              ? slot.object.array_length
                              : 0U));
    }
    return result;
}

Result<usize> Heap::resolve_slot_unlocked(ObjectRef reference) const {
    if (reference.is_null() || reference.slot() == 0U) {
        return fail(ErrorCode::invalid_argument, "null object reference");
    }
    const usize index = static_cast<usize>(reference.slot() - 1U);
    if (index >= slots_.size()) {
        return fail(ErrorCode::invalid_argument, "object reference slot is invalid");
    }
    const Slot& slot = slots_[index];
    if (!slot.occupied || slot.generation != reference.generation()) {
        return fail(ErrorCode::invalid_argument, "stale object reference");
    }
    return index;
}

Result<usize> Heap::resolve_slot_vm_fast(ObjectRef reference) const {
    if (reference.is_null() || reference.slot() == 0U) {
        return fail(ErrorCode::invalid_argument, "null object reference");
    }
    const usize cache_index =
        static_cast<usize>(reference.slot()) & (kVmSlotCacheSize - 1U);
    const VmSlotCacheEntry& cached = vm_slot_cache_[cache_index];
    if (cached.reference_bits == reference.bits) {
        PerformanceCounters::record_vm_slot_cache(true);
        return cached.index;
    }
    PerformanceCounters::record_vm_slot_cache(false);
    auto resolved = resolve_slot_unlocked(reference);
    if (!resolved) return std::unexpected(resolved.error());
    vm_slot_cache_[cache_index] = VmSlotCacheEntry {
        .reference_bits = reference.bits,
        .index = *resolved,
    };
    return *resolved;
}

void Heap::invalidate_vm_slot_cache_unlocked() const noexcept {
    for (VmSlotCacheEntry& entry : vm_slot_cache_) entry = {};
}

Result<ObjectRef> Heap::allocate_unlocked(Object object,
                                          usize accounted_bytes) {
    auto capacity = ensure_capacity_unlocked(accounted_bytes);
    if (!capacity) {
        return std::unexpected(capacity.error());
    }

    usize index = 0U;
    if (!free_slots_.empty()) {
        index = free_slots_.back();
        free_slots_.pop_back();
    } else {
        index = slots_.size();
        slots_.push_back({});
    }

    const usize encoded_slot = index + 1U;
    auto slot32 = checked_narrow<u32>(encoded_slot);
    if (!slot32) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        if (index + 1U == slots_.size()) {
            slots_.pop_back();
        } else {
            free_slots_.push_back(index);
        }
        return std::unexpected(slot32.error());
    }

    Slot& slot = slots_[index];
    slot.occupied = true;
    slot.accounted_bytes = accounted_bytes;
    slot.object = std::move(object);
    live_bytes_ += accounted_bytes;
    peak_bytes_ = std::max(peak_bytes_, live_bytes_);
    return ObjectRef::make(*slot32, slot.generation);
}

Status Heap::ensure_capacity_unlocked(usize object_bytes) noexcept {
    if (free_slots_.empty() && slots_.size() >= limits_.maximum_objects) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        return fail(ErrorCode::overflow, "Java heap object limit reached");
    }
    if (live_bytes_ > limits_.maximum_bytes ||
        object_bytes > limits_.maximum_bytes - live_bytes_) {
        ++failed_allocations_;
        PerformanceCounters::record_failed_allocation();
        return fail(ErrorCode::overflow, "Java heap byte limit reached");
    }
    return {};
}

void Heap::update_automatic_collection_threshold_unlocked() noexcept {
    const usize maximum = limits_.maximum_bytes;
    if (maximum == 0U) {
        automatic_collection_threshold_ = 0U;
        return;
    }
    if (live_bytes_ >= maximum) {
        automatic_collection_threshold_ = maximum;
        return;
    }

    // Start collecting at half the configured heap. After each collection,
    // retain half of the remaining free space as headroom. This adapts upward
    // for games with a genuinely large live set while keeping enough reserve
    // that small native allocations (notably String payload attachment) cannot
    // hit the hard limit between interpreter safepoints.
    const usize initial = std::max<usize>(maximum / 2U, 1U);
    const usize remaining = maximum - live_bytes_;
    const usize growth = std::max<usize>(remaining / 2U, 1U);
    const usize adaptive = live_bytes_ + growth;
    automatic_collection_threshold_ = std::max(initial, adaptive);
    automatic_collection_threshold_ =
        std::min(automatic_collection_threshold_, maximum);
}

void Heap::mark_unlocked(ObjectRef root, std::vector<ObjectRef>& pending) {
    if (root.is_null() || root.slot() == 0U) {
        return;
    }
    const usize index = static_cast<usize>(root.slot() - 1U);
    if (index >= slots_.size()) {
        return;
    }
    Slot& slot = slots_[index];
    if (!slot.occupied || slot.generation != root.generation() ||
        slot.object.marked) {
        return;
    }
    slot.object.marked = true;

    const usize field_count = compact_field_count(slot.object);
    for (usize field_index = 0U; field_index < field_count; ++field_index) {
        const usize kind_word =
            field_count + field_index / kFieldKindsPerWord;
        const usize shift =
            (field_index % kFieldKindsPerWord) * kFieldKindBits;
        const u64 kind_bits =
            (slot.object.fields[kind_word] >> shift) &
            ((1ULL << kFieldKindBits) - 1ULL);
        if (kind_bits != static_cast<u64>(ValueKind::reference)) continue;
        const ObjectRef reference {slot.object.fields[field_index]};
        if (!reference.is_null()) pending.push_back(reference);
    }

    // Array payloads are homogeneous raw words. Primitive arrays contain no
    // references at all; reference arrays can be traced without reconstructing
    // a Value/ValueKind for every element.
    if (slot.object.is_array && slot.object.array_kind.has_value() &&
        *slot.object.array_kind == HeapArrayKind::reference) {
        for (usize element_index = 0U;
             element_index < slot.object.array_length;
             ++element_index) {
            u64 raw = 0U;
            std::memcpy(&raw,
                        slot.object.array_payload.data() +
                            element_index * sizeof(u64),
                        sizeof(raw));
            const ObjectRef reference {raw};
            if (!reference.is_null()) pending.push_back(reference);
        }
    }
}

Result<usize> Heap::estimate_object_bytes(std::string_view class_name,
                                          usize field_count,
                                          usize element_count,
                                          usize text_length) noexcept {
    auto field_bytes = checked_multiply(
        compact_field_word_count(field_count), sizeof(u64));
    if (!field_bytes) {
        return std::unexpected(field_bytes.error());
    }
    usize element_width = sizeof(u64);
    if (class_name == "[B" || class_name == "[Z") element_width = sizeof(u8);
    if (class_name == "[C" || class_name == "[S") element_width = sizeof(u16);
    if (class_name == "[I" || class_name == "[F") element_width = sizeof(u32);
    auto element_bytes = checked_multiply(element_count, element_width);
    if (!element_bytes) {
        return std::unexpected(element_bytes.error());
    }
    auto text_bytes = checked_multiply(text_length, sizeof(char16_t));
    if (!text_bytes) {
        return std::unexpected(text_bytes.error());
    }

    // Class-name bytes are shared interned metadata, not per-object payload.
    auto total = checked_add(sizeof(Object), *field_bytes);
    if (!total) {
        return std::unexpected(total.error());
    }
    total = checked_add(*total, *element_bytes);
    if (!total) {
        return std::unexpected(total.error());
    }
    return checked_add(*total, *text_bytes);
}

} // namespace phoneme::vm
