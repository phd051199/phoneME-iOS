#pragma once

#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "phoneme/vm/Value.hpp"

namespace phoneme::vm {

class Machine;

struct HeapLimits final {
    usize maximum_objects {1'000'000};
    usize maximum_bytes {64U * 1024U * 1024U};
};

struct HeapStats final {
    usize live_objects {0};
    usize live_slots {0};
    usize estimated_bytes {0};
    usize peak_estimated_bytes {0};
    usize maximum_objects {0};
    usize maximum_bytes {0};
    usize collections {0};
    usize failed_allocations {0};
};

enum class HeapArrayKind : u8 {
    boolean,
    byte,
    character,
    short_integer,
    integer,
    long_integer,
    float32,
    float64,
    reference,
};

struct HeapArrayInfo final {
    HeapArrayKind kind {HeapArrayKind::integer};
    usize length {0};
    // Borrowed from the Heap's interned array descriptor and therefore stable
    // for the Heap lifetime. Reference-array checks no longer allocate a
    // component std::string for every aaload/aastore/native query.
    std::string_view reference_component;
};

struct HeapArrayElementSnapshot final {
    HeapArrayKind kind {HeapArrayKind::integer};
    Value value;
};

struct HeapArrayRawElementSnapshot final {
    HeapArrayKind kind {HeapArrayKind::integer};
    u64 raw {0U};
};

struct HeapArrayPayloadLease final {
    void* first_payload {nullptr};
    usize length {0};
};

struct HeapAccessContext final {
    std::string_view owner;
    std::string_view method;
    std::string_view descriptor;
    usize bytecode_pc {0};
    const usize* live_bytecode_pc {nullptr};

    [[nodiscard]] usize current_bytecode_pc() const noexcept {
        return live_bytecode_pc != nullptr ? *live_bytecode_pc : bytecode_pc;
    }
};

[[nodiscard]] HeapAccessContext current_heap_access_context() noexcept;
void set_heap_access_context(HeapAccessContext context) noexcept;

class Heap final {
public:
    explicit Heap(usize maximum_objects = 1'000'000);
    explicit Heap(HeapLimits limits);

    [[nodiscard]] Result<ObjectRef> allocate_object(std::string class_name,
                                                    usize field_count);
    [[nodiscard]] Result<ObjectRef> allocate_object(
        std::string class_name,
        std::span<const Value> initial_fields);
    [[nodiscard]] Result<ObjectRef> allocate_text_object(
        std::string class_name,
        std::span<const Value> initial_fields,
        std::u16string text);
    [[nodiscard]] Result<ObjectRef> allocate_array(std::string class_name,
                                                   usize length,
                                                   Value initial_value = {});
    [[nodiscard]] Result<ObjectRef> clone_object(ObjectRef reference);

    [[nodiscard]] Result<Value> field(ObjectRef reference, usize index) const;
    [[nodiscard]] Status read_fields(ObjectRef reference,
                                     std::span<const usize> indices,
                                     std::span<Value> values) const;
    [[nodiscard]] Status write_fields(ObjectRef reference,
                                      std::span<const usize> indices,
                                      std::span<const Value> values);
    [[nodiscard]] Status set_field(ObjectRef reference, usize index, Value value);
    [[nodiscard]] Result<Value> element(ObjectRef reference, usize index) const;
    [[nodiscard]] Status set_element(ObjectRef reference, usize index, Value value);
    [[nodiscard]] Result<HeapArrayInfo> array_info(ObjectRef reference) const;
    [[nodiscard]] Result<HeapArrayElementSnapshot> array_element_snapshot(
        ObjectRef reference,
        usize index) const;
    [[nodiscard]] Status set_element_checked(ObjectRef reference,
                                             usize index,
                                             HeapArrayKind expected_kind,
                                             Value value);
    // Returns a stable pointer to the first homogeneous raw payload slot for a
    // fixed-size Java array. The array kind is stored once in Object metadata,
    // so each element does not need its own ValueKind tag. The caller must keep
    // `reference` rooted for the entire lease lifetime and must not retain the
    // pointer beyond such a rooted region.
    [[nodiscard]] Result<HeapArrayPayloadLease> array_payload_lease(
        ObjectRef reference,
        HeapArrayKind expected_kind);
    [[nodiscard]] Status fill_array_range(ObjectRef reference,
                                           usize index,
                                           usize length,
                                           Value value);
    [[nodiscard]] Status copy_array_range(ObjectRef source,
                                          usize source_index,
                                          ObjectRef destination,
                                          usize destination_index,
                                          usize length);
    // Fast path for System.arraycopy on primitive arrays. Returns true when
    // both arrays are primitive and the copy was completed, false when both
    // arrays are reference arrays (the caller must perform Java assignability
    // checks), and an error for mixed/mismatched primitive kinds or invalid
    // ranges. Keeping validation and the overlap-safe copy under one heap lock
    // avoids several metadata lookups per tiny DEFLATE/PNG copy operation.
    [[nodiscard]] Result<bool> try_copy_primitive_array_range(
        ObjectRef source,
        usize source_index,
        ObjectRef destination,
        usize destination_index,
        usize length);
    [[nodiscard]] Result<std::vector<u8>> read_byte_array(
        ObjectRef reference,
        usize offset,
        usize length) const;
    [[nodiscard]] Result<std::vector<u8>> read_byte_array(
        ObjectRef reference) const;
    [[nodiscard]] Result<std::vector<i32>> read_int_array(
        ObjectRef reference) const;
    [[nodiscard]] Result<std::vector<i16>> read_short_array(
        ObjectRef reference) const;
    [[nodiscard]] Result<std::vector<float>> read_float_array(
        ObjectRef reference) const;
    [[nodiscard]] Result<std::vector<u8>> read_boolean_array(
        ObjectRef reference) const;
    [[nodiscard]] Result<std::vector<ObjectRef>> read_reference_array(
        ObjectRef reference) const;
    [[nodiscard]] Status write_reference_array(
        ObjectRef reference,
        usize offset,
        std::span<const ObjectRef> values);
    [[nodiscard]] Status write_byte_array(ObjectRef reference,
                                          usize offset,
                                          std::span<const u8> bytes);
    [[nodiscard]] Status write_int_array(ObjectRef reference,
                                         usize offset,
                                         std::span<const i32> values);
    [[nodiscard]] Status write_float_array(ObjectRef reference,
                                           usize offset,
                                           std::span<const float> values);
    [[nodiscard]] Status write_boolean_array(ObjectRef reference,
                                             usize offset,
                                             std::span<const u8> values);
    [[nodiscard]] Status write_char_array(ObjectRef reference,
                                          usize offset,
                                          std::u16string_view characters);
    [[nodiscard]] Result<usize> array_length(ObjectRef reference) const;
    [[nodiscard]] Result<std::string> class_name(ObjectRef reference) const;
    [[nodiscard]] Status attach_string(ObjectRef reference,
                                       std::u16string value);
    [[nodiscard]] Status append_mutable_text(ObjectRef reference,
                                             usize capacity_field_index,
                                             std::u16string_view suffix);
    [[nodiscard]] Status insert_mutable_text(ObjectRef reference,
                                             usize capacity_field_index,
                                             usize offset,
                                             std::u16string_view inserted);
    [[nodiscard]] Result<std::u16string> string_value(
        ObjectRef reference) const;
    [[nodiscard]] Result<usize> string_length(ObjectRef reference) const;
    [[nodiscard]] Result<u16> string_character(ObjectRef reference,
                                               usize index) const;
    [[nodiscard]] Result<i32> string_index_of(ObjectRef reference,
                                              u16 character,
                                              usize start = 0U) const;

    [[nodiscard]] Status set_weak_referent(ObjectRef reference,
                                           ObjectRef referent);
    [[nodiscard]] Result<ObjectRef> weak_referent(ObjectRef reference) const;
    [[nodiscard]] Status clear_weak_referent(ObjectRef reference);

    [[nodiscard]] Status collect(std::span<const ObjectRef> roots);
    void clear() noexcept;
    [[nodiscard]] usize estimated_bytes() const noexcept;
    [[nodiscard]] bool automatic_collection_due() const noexcept;
    [[nodiscard]] HeapStats stats() const noexcept;

private:
    friend class Machine;

    // Fast bytecode-engine accessors. These intentionally skip Heap::mutex_
    // because Machine::execute() already serializes Java execution through the
    // per-VM execution gate. They must only be called from Machine while that
    // gate is held; public/native/host callers continue to use the locked API.
    [[nodiscard]] Result<Value> vm_field(ObjectRef reference,
                                         usize index) const;
    [[nodiscard]] Result<ObjectRef> vm_allocate_object(
        std::string_view class_name,
        std::span<const Value> initial_fields);
    [[nodiscard]] Result<ObjectRef> vm_allocate_array(
        std::string_view class_name,
        usize length,
        Value initial_value);
    [[nodiscard]] Result<Value> vm_field_typed(ObjectRef reference,
                                               usize index,
                                               ValueKind expected_kind) const;
    [[nodiscard]] Status vm_set_field(ObjectRef reference,
                                      usize index,
                                      Value value);
    [[nodiscard]] Status vm_set_field_typed(ObjectRef reference,
                                            usize index,
                                            ValueKind expected_kind,
                                            Value value);
    [[nodiscard]] Result<Value> vm_element(ObjectRef reference,
                                           usize index) const;
    [[nodiscard]] Status vm_set_element(ObjectRef reference,
                                        usize index,
                                        Value value);
    [[nodiscard]] Result<HeapArrayInfo> vm_array_info(
        ObjectRef reference) const;
    [[nodiscard]] Result<HeapArrayElementSnapshot> vm_array_element_snapshot(
        ObjectRef reference,
        usize index) const;
    [[nodiscard]] Result<HeapArrayRawElementSnapshot>
        vm_array_raw_element_snapshot(ObjectRef reference,
                                      usize index) const;
    [[nodiscard]] Status vm_set_array_raw_element_checked(
        ObjectRef reference,
        usize index,
        HeapArrayKind expected_kind,
        u64 raw);
    [[nodiscard]] Status vm_set_element_checked(ObjectRef reference,
                                                usize index,
                                                HeapArrayKind expected_kind,
                                                Value value);
    [[nodiscard]] Result<usize> vm_array_length(ObjectRef reference) const;
    // Borrowed class-name view for Machine hot paths. The view is valid only
    // while the VM execution gate keeps the referenced heap object alive; it
    // must not escape into host/native code or survive a point where the
    // reference may become unreachable.
    [[nodiscard]] Result<std::string_view> vm_class_name_view(
        ObjectRef reference) const;
    [[nodiscard]] Result<bool> vm_try_copy_primitive_array_range(
        ObjectRef source,
        usize source_index,
        ObjectRef destination,
        usize destination_index,
        usize length);
    [[nodiscard]] Result<usize> vm_string_length(ObjectRef reference) const;
    [[nodiscard]] Result<std::u16string> vm_string_value(
        ObjectRef reference) const;
    [[nodiscard]] Status vm_attach_string(ObjectRef reference,
                                          std::u16string value);
    [[nodiscard]] Status vm_append_mutable_text(
        ObjectRef reference,
        usize capacity_field_index,
        std::u16string_view suffix);
    [[nodiscard]] Result<u16> vm_string_character(ObjectRef reference,
                                                  usize index) const;
    [[nodiscard]] Result<i32> vm_string_index_of(ObjectRef reference,
                                                 u16 character,
                                                 usize start = 0U) const;
    [[nodiscard]] Result<bool> vm_string_starts_with(
        ObjectRef reference,
        ObjectRef prefix,
        usize offset = 0U) const;
    [[nodiscard]] Result<std::u16string> vm_string_slice(
        ObjectRef reference,
        usize begin,
        usize end) const;
    [[nodiscard]] Result<bool> vm_string_equals(ObjectRef left,
                                                ObjectRef right) const;
    [[nodiscard]] bool vm_automatic_collection_due() const noexcept;

    [[nodiscard]] Status attach_string_unlocked(ObjectRef reference,
                                                std::u16string value);
    [[nodiscard]] Status append_mutable_text_unlocked(
        ObjectRef reference,
        usize capacity_field_index,
        std::u16string_view suffix);

    class ArrayPayload final {
    public:
        ArrayPayload() noexcept = default;
        ArrayPayload(usize size, bool allow_inline);
        ArrayPayload(const ArrayPayload& other);
        ArrayPayload(ArrayPayload&& other) noexcept;
        ArrayPayload& operator=(const ArrayPayload& other);
        ArrayPayload& operator=(ArrayPayload&& other) noexcept;
        ~ArrayPayload();

        [[nodiscard]] usize size() const noexcept;
        [[nodiscard]] bool empty() const noexcept { return size() == 0U; }
        [[nodiscard]] bool inline_storage() const noexcept;
        [[nodiscard]] u8* data() noexcept;
        [[nodiscard]] const u8* data() const noexcept;
        [[nodiscard]] u8* begin() noexcept { return data(); }
        [[nodiscard]] const u8* begin() const noexcept { return data(); }
        [[nodiscard]] u8* end() noexcept { return data() + size(); }
        [[nodiscard]] const u8* end() const noexcept { return data() + size(); }
        [[nodiscard]] u8& operator[](usize index) noexcept { return data()[index]; }
        [[nodiscard]] const u8& operator[](usize index) const noexcept {
            return data()[index];
        }

    private:
        static constexpr usize kInlineCapacity = 16U;
        static constexpr usize kHeapFlag = usize{1}
            << (std::numeric_limits<usize>::digits - 1U);
        static constexpr usize kSizeMask = ~kHeapFlag;

        union Storage final {
            u8 inline_bytes[kInlineCapacity];
            u8* heap;
            constexpr Storage() noexcept : inline_bytes {} {}
        } storage_;
        usize encoded_size_ {0U};
    };
    static_assert(sizeof(ArrayPayload) == 16U + sizeof(usize),
                  "array payload must stay at one inline block plus size word");

    struct Object final {
        // Class descriptors are immutable VM metadata. Storing a full
        // std::string in every Java object caused an independent host heap
        // allocation for most real class names. Keep one interned copy per
        // Heap and let objects retain only a cheap view into that stable copy.
        std::string_view class_name;
        // Object fields use one compact word vector. The first N words are
        // the raw 64-bit payloads; trailing words pack ValueKind at 3 bits per
        // field. N is recovered from the total word count, so no per-object
        // field-count member or second allocation is required. This keeps the
        // Object header unchanged while moving field storage from 16 bytes per
        // Value toward ~8.4 bytes/field for ordinary multi-field objects.
        std::vector<u64> fields;
        // Java arrays are homogeneous. Keep the kind once and store one raw
        // byte payload at the Java element's natural width. This avoids both
        // per-element Value tags and multiple std::vector members in every
        // heap object. Generated ARM64 may directly lease this byte storage;
        // C++ accesses use memcpy so alignment/aliasing remain well-defined.
        ArrayPayload array_payload;
        usize array_length {0U};
        std::u16string string_payload;
        std::optional<ObjectRef> weak_referent;
        std::optional<HeapArrayKind> array_kind;
        bool is_array {false};
        bool is_string {false};
        bool marked {false};
    };

    struct Slot final {
        u32 generation {1};
        bool occupied {false};
        usize accounted_bytes {0};
        Object object;
    };

    [[nodiscard]] static bool uses_packed_byte_storage(
        HeapArrayKind kind) noexcept;
    [[nodiscard]] static bool uses_packed_half_storage(
        HeapArrayKind kind) noexcept;
    [[nodiscard]] static bool uses_packed_word_storage(
        HeapArrayKind kind) noexcept;
    [[nodiscard]] static usize array_element_width(
        HeapArrayKind kind) noexcept;
    static constexpr usize kFieldKindBits = 3U;
    static constexpr usize kFieldKindsPerWord =
        (sizeof(u64) * 8U) / kFieldKindBits;
    [[nodiscard]] static usize compact_field_word_count(
        usize field_count) noexcept;
    [[nodiscard]] static usize compact_field_count(
        const Object& object) noexcept;
    [[nodiscard]] static std::vector<u64> compact_fields(
        std::span<const Value> values);
    [[nodiscard]] static Result<Value> field_value_unlocked(
        const Object& object,
        usize index);
    [[nodiscard]] static Status set_field_value_unlocked(
        Object& object,
        usize index,
        Value value);
    [[nodiscard]] static usize array_length_unlocked(
        const Object& object) noexcept;
    [[nodiscard]] static Result<Value> array_value_unlocked(
        const Object& object,
        usize index);
    [[nodiscard]] static Status set_array_value_unlocked(
        Object& object,
        usize index,
        Value value);

    [[nodiscard]] Result<usize> resolve_slot_unlocked(ObjectRef reference) const;
    [[nodiscard]] Result<usize> resolve_slot_vm_fast(ObjectRef reference) const;
    void invalidate_vm_slot_cache_unlocked() const noexcept;
    [[nodiscard]] std::string_view intern_class_name(
        std::string_view class_name);
    [[nodiscard]] Result<bool> try_copy_primitive_array_range_unlocked(
        ObjectRef source,
        usize source_index,
        ObjectRef destination,
        usize destination_index,
        usize length);
    [[nodiscard]] Result<ObjectRef> allocate_unlocked(Object object,
                                                      usize accounted_bytes);
    [[nodiscard]] Result<ObjectRef> allocate_object_with_fields(
        std::string class_name,
        std::vector<Value> fields);
    [[nodiscard]] Status ensure_capacity_unlocked(usize object_bytes) noexcept;
    void update_automatic_collection_threshold_unlocked() noexcept;
    void mark_unlocked(ObjectRef root, std::vector<ObjectRef>& pending);
    [[nodiscard]] static Result<usize> estimate_object_bytes(
        std::string_view class_name,
        usize field_count,
        usize element_count,
        usize text_length) noexcept;

    HeapLimits limits_ {};
    mutable std::mutex mutex_;
    // Class names live for the Heap lifetime, including across clear(). This
    // keeps every Object::class_name view and the lock-free direct-mapped
    // allocation cache valid without generation bookkeeping.
    mutable std::mutex class_names_mutex_;
    std::unordered_set<std::string> class_names_;
    static constexpr usize kClassNameCacheSize = 64U;
    static_assert((kClassNameCacheSize & (kClassNameCacheSize - 1U)) == 0U);
    std::array<std::atomic<const std::string*>, kClassNameCacheSize>
        class_name_cache_ {};
    std::vector<Slot> slots_;
    std::vector<usize> free_slots_;
    struct VmSlotCacheEntry final {
        u64 reference_bits {0U};
        usize index {0U};
    };
    static constexpr usize kVmSlotCacheSize = 64U;
    mutable std::array<VmSlotCacheEntry, kVmSlotCacheSize> vm_slot_cache_ {};
    usize live_bytes_ {0};
    usize peak_bytes_ {0};
    usize automatic_collection_threshold_ {0};
    usize collections_ {0};
    usize failed_allocations_ {0};
};

} // namespace phoneme::vm
