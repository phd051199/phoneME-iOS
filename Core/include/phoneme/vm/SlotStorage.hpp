#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <utility>
#include <vector>

#include "phoneme/vm/Value.hpp"

namespace phoneme::vm {

// Most J2ME methods are tiny wrappers/getters whose max_stack/max_locals are
// well below 16 slots. Keeping those slots inside the execution frame avoids
// several malloc/free pairs for every interpreted Java call. Large/obfuscated
// methods transparently fall back to the original vector-backed storage.
inline constexpr usize kInlineFrameSlotCapacity = 16U;
inline constexpr usize kInlineInvocationArgumentCapacity = 8U;

static_assert(sizeof(ValueKind) == 1U,
              "compact slot storage requires a byte-sized ValueKind");

[[nodiscard]] constexpr bool compact_category_two(ValueKind kind) noexcept {
    return kind == ValueKind::int64 || kind == ValueKind::float64;
}

struct CompactStackValue final {
    ValueKind kind {ValueKind::empty};
    u64 bits {0U};

    [[nodiscard]] constexpr bool category_two() const noexcept {
        return compact_category_two(kind);
    }
};

// Invocation arguments are part of the Java call hot path, so keep the same
// split representation as locals/operand stacks instead of carrying 16-byte
// tagged Value objects between frames. Native/external compatibility code can
// materialize Values explicitly at its boundary; normal Java/JIT execution
// reads the byte-sized kind and raw payload directly.
class InvocationArguments final {
public:
    InvocationArguments() = default;

    explicit InvocationArguments(usize size)
        : size_(size), inline_mode_(size <= kInlineInvocationArgumentCapacity) {
        if (!inline_mode_) {
            overflow_bits_.resize(size);
            overflow_kinds_.resize(size);
        }
    }

    explicit InvocationArguments(std::span<const Value> values)
        : InvocationArguments(values.size()) {
        for (usize index = 0U; index < values.size(); ++index) {
            set(index, values[index]);
        }
    }

    InvocationArguments(const InvocationArguments& other)
        : size_(other.size_), inline_mode_(other.inline_mode_) {
        if (inline_mode_) {
            std::copy_n(other.inline_bits_.begin(), size_, inline_bits_.begin());
            std::copy_n(other.inline_kinds_.begin(), size_, inline_kinds_.begin());
        } else {
            overflow_bits_ = other.overflow_bits_;
            overflow_kinds_ = other.overflow_kinds_;
        }
    }

    InvocationArguments& operator=(const InvocationArguments& other) {
        if (this == &other) return *this;
        size_ = other.size_;
        inline_mode_ = other.inline_mode_;
        if (inline_mode_) {
            overflow_bits_.clear();
            overflow_kinds_.clear();
            std::copy_n(other.inline_bits_.begin(), size_, inline_bits_.begin());
            std::copy_n(other.inline_kinds_.begin(), size_, inline_kinds_.begin());
        } else {
            overflow_bits_ = other.overflow_bits_;
            overflow_kinds_ = other.overflow_kinds_;
        }
        return *this;
    }

    InvocationArguments(InvocationArguments&& other) noexcept
        : size_(other.size_), inline_mode_(other.inline_mode_) {
        if (inline_mode_) {
            std::copy_n(other.inline_bits_.begin(), size_, inline_bits_.begin());
            std::copy_n(other.inline_kinds_.begin(), size_, inline_kinds_.begin());
        } else {
            overflow_bits_ = std::move(other.overflow_bits_);
            overflow_kinds_ = std::move(other.overflow_kinds_);
        }
        other.size_ = 0U;
        other.inline_mode_ = true;
    }

    InvocationArguments& operator=(InvocationArguments&& other) noexcept {
        if (this == &other) return *this;
        size_ = other.size_;
        inline_mode_ = other.inline_mode_;
        if (inline_mode_) {
            overflow_bits_.clear();
            overflow_kinds_.clear();
            std::copy_n(other.inline_bits_.begin(), size_, inline_bits_.begin());
            std::copy_n(other.inline_kinds_.begin(), size_, inline_kinds_.begin());
        } else {
            overflow_bits_ = std::move(other.overflow_bits_);
            overflow_kinds_ = std::move(other.overflow_kinds_);
        }
        other.size_ = 0U;
        other.inline_mode_ = true;
        return *this;
    }

    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
    [[nodiscard]] bool inline_mode() const noexcept { return inline_mode_; }

    void set(usize index, Value value) noexcept {
        bits_data()[index] = value.raw_bits_unchecked();
        kinds_data()[index] = value.kind();
    }

    void set_compact(usize index, ValueKind kind, u64 bits) noexcept {
        bits_data()[index] = bits;
        kinds_data()[index] = kind;
    }

    [[nodiscard]] Value value(usize index) const noexcept {
        return Value::from_raw_unchecked(kind(index), raw_bits(index));
    }

    // Return a const prvalue deliberately: callers may inspect/materialize an
    // argument, but `arguments[i] = value` must fail to compile instead of
    // silently assigning to a temporary. Mutations go through set().
    [[nodiscard]] const Value operator[](usize index) const noexcept {
        return value(index);
    }
    [[nodiscard]] const Value front() const noexcept { return value(0U); }

    [[nodiscard]] ValueKind kind(usize index) const noexcept {
        return kinds_data()[index];
    }

    [[nodiscard]] u64 raw_bits(usize index) const noexcept {
        return bits_data()[index];
    }

    [[nodiscard]] std::span<const u64> raw_bits_span() const noexcept {
        return std::span<const u64>(bits_data(), size_);
    }

    [[nodiscard]] ObjectRef reference_unchecked(usize index) const noexcept {
        return ObjectRef {raw_bits(index)};
    }

    void materialize(std::span<Value> output) const noexcept {
        const usize count = std::min(size_, output.size());
        for (usize index = 0U; index < count; ++index) {
            output[index] = value(index);
        }
    }

    void materialize_from(usize source_offset,
                          std::span<Value> output) const noexcept {
        if (source_offset >= size_) return;
        const usize count = std::min(size_ - source_offset, output.size());
        for (usize index = 0U; index < count; ++index) {
            output[index] = value(source_offset + index);
        }
    }

    void append_reference_roots(std::vector<ObjectRef>& roots) const {
        for (usize index = 0U; index < size_; ++index) {
            if (kind(index) != ValueKind::reference) continue;
            const ObjectRef reference {raw_bits(index)};
            if (!reference.is_null()) roots.push_back(reference);
        }
    }

private:
    [[nodiscard]] u64* bits_data() noexcept {
        return inline_mode_ ? inline_bits_.data() : overflow_bits_.data();
    }
    [[nodiscard]] const u64* bits_data() const noexcept {
        return inline_mode_ ? inline_bits_.data() : overflow_bits_.data();
    }
    [[nodiscard]] ValueKind* kinds_data() noexcept {
        return inline_mode_ ? inline_kinds_.data() : overflow_kinds_.data();
    }
    [[nodiscard]] const ValueKind* kinds_data() const noexcept {
        return inline_mode_ ? inline_kinds_.data() : overflow_kinds_.data();
    }

    usize size_ {0U};
    bool inline_mode_ {true};
    std::array<u64, kInlineInvocationArgumentCapacity> inline_bits_;
    std::array<ValueKind, kInlineInvocationArgumentCapacity> inline_kinds_;
    std::vector<u64> overflow_bits_;
    std::vector<ValueKind> overflow_kinds_;
};

class OperandStack final {
public:
    explicit OperandStack(usize maximum_slots)
        : maximum_slots_(maximum_slots),
          inline_mode_(maximum_slots <= kInlineFrameSlotCapacity) {
        if (!inline_mode_) {
            bits_.reserve(maximum_slots);
            kinds_.reserve(maximum_slots);
        }
    }

    OperandStack(const OperandStack&) = delete;
    OperandStack& operator=(const OperandStack&) = delete;

    OperandStack(OperandStack&& other) noexcept
        : maximum_slots_(other.maximum_slots_),
          used_slots_(other.used_slots_),
          value_count_(other.value_count_),
          inline_mode_(other.inline_mode_) {
        if (inline_mode_) {
            std::copy_n(other.inline_bits_.begin(),
                        value_count_,
                        inline_bits_.begin());
            std::copy_n(other.inline_kinds_.begin(),
                        value_count_,
                        inline_kinds_.begin());
        } else {
            bits_ = std::move(other.bits_);
            kinds_ = std::move(other.kinds_);
        }
    }

    OperandStack& operator=(OperandStack&& other) noexcept {
        if (this == &other) return *this;
        maximum_slots_ = other.maximum_slots_;
        used_slots_ = other.used_slots_;
        value_count_ = other.value_count_;
        inline_mode_ = other.inline_mode_;
        if (inline_mode_) {
            bits_.clear();
            kinds_.clear();
            std::copy_n(other.inline_bits_.begin(),
                        value_count_,
                        inline_bits_.begin());
            std::copy_n(other.inline_kinds_.begin(),
                        value_count_,
                        inline_kinds_.begin());
        } else {
            bits_ = std::move(other.bits_);
            kinds_ = std::move(other.kinds_);
        }
        return *this;
    }

    [[nodiscard]] usize used_slots() const noexcept { return used_slots_; }
    [[nodiscard]] bool empty() const noexcept { return value_count() == 0U; }

    [[nodiscard]] Status push(Value value) {
        if (value.empty() || value.continuation_slot()) {
            return fail(ErrorCode::invalid_argument,
                        "cannot push an empty or continuation value");
        }
        return push_raw(value.kind(), value.raw_bits_unchecked());
    }

    [[nodiscard]] Status push_compact(CompactStackValue value) {
        return push_raw(value.kind, value.bits);
    }

    [[nodiscard]] Status push_int(i32 value) {
        return push_raw(ValueKind::int32,
                        static_cast<u64>(std::bit_cast<u32>(value)));
    }

    [[nodiscard]] Status push_long(i64 value) {
        return push_raw(ValueKind::int64, std::bit_cast<u64>(value));
    }

    [[nodiscard]] Status push_float(float value) {
        return push_raw(ValueKind::float32,
                        static_cast<u64>(std::bit_cast<u32>(value)));
    }

    [[nodiscard]] Status push_double(double value) {
        return push_raw(ValueKind::float64, std::bit_cast<u64>(value));
    }

    [[nodiscard]] Status push_reference(ObjectRef value) {
        return push_raw(ValueKind::reference, value.bits);
    }

    [[nodiscard]] Result<Value> pop() {
        auto raw = pop_compact();
        if (!raw) return std::unexpected(raw.error());
        return Value::from_raw_unchecked(raw->kind, raw->bits);
    }

    [[nodiscard]] Result<CompactStackValue> pop_compact() {
        if (empty()) {
            return fail(ErrorCode::malformed_class,
                        "operand stack underflow");
        }
        CompactStackValue raw;
        if (inline_mode_) {
            --value_count_;
            raw.kind = inline_kinds_[value_count_];
            raw.bits = inline_bits_[value_count_];
        } else {
            raw.kind = kinds_.back();
            raw.bits = bits_.back();
            kinds_.pop_back();
            bits_.pop_back();
        }
        used_slots_ -= raw.category_two() ? 2U : 1U;
        return raw;
    }

    [[nodiscard]] Result<i32> pop_int() {
        auto raw = pop_compact();
        if (!raw) return std::unexpected(raw.error());
        if (raw->kind != ValueKind::int32) {
            return fail(ErrorCode::invalid_state, "value is not a Java int");
        }
        return std::bit_cast<i32>(static_cast<u32>(raw->bits));
    }

    [[nodiscard]] Result<i64> pop_long() {
        auto raw = pop_compact();
        if (!raw) return std::unexpected(raw.error());
        if (raw->kind != ValueKind::int64) {
            return fail(ErrorCode::invalid_state, "value is not a Java long");
        }
        return std::bit_cast<i64>(raw->bits);
    }

    [[nodiscard]] Result<float> pop_float() {
        auto raw = pop_compact();
        if (!raw) return std::unexpected(raw.error());
        if (raw->kind != ValueKind::float32) {
            return fail(ErrorCode::invalid_state, "value is not a Java float");
        }
        return std::bit_cast<float>(static_cast<u32>(raw->bits));
    }

    [[nodiscard]] Result<double> pop_double() {
        auto raw = pop_compact();
        if (!raw) return std::unexpected(raw.error());
        if (raw->kind != ValueKind::float64) {
            return fail(ErrorCode::invalid_state, "value is not a Java double");
        }
        return std::bit_cast<double>(raw->bits);
    }

    [[nodiscard]] Result<ObjectRef> pop_reference() {
        auto raw = pop_compact();
        if (!raw) return std::unexpected(raw.error());
        if (raw->kind != ValueKind::reference) {
            return fail(ErrorCode::invalid_state,
                        "value is not a Java object reference");
        }
        return ObjectRef {raw->bits};
    }

    [[nodiscard]] Result<const Value*> top() const {
        if (empty()) {
            return fail(ErrorCode::malformed_class,
                        "operand stack is empty");
        }
        const usize index = value_count() - 1U;
        top_cache_ = Value::from_raw_unchecked(kind_at(index), bits_at(index));
        return &top_cache_;
    }

    void clear() noexcept {
        if (inline_mode_) {
            value_count_ = 0U;
        } else {
            bits_.clear();
            kinds_.clear();
        }
        used_slots_ = 0U;
    }

    void append_reference_roots(std::vector<ObjectRef>& roots) const {
        const usize count = value_count();
        for (usize index = 0U; index < count; ++index) {
            if (kind_at(index) != ValueKind::reference) continue;
            const ObjectRef reference {bits_at(index)};
            if (!reference.is_null()) roots.push_back(reference);
        }
    }

    [[nodiscard]] bool append_reference_roots_at_physical_slots(
        std::span<const usize> frame_slots,
        usize frame_slot_base,
        std::vector<ObjectRef>& roots) const {
        if (frame_slots.empty()) return true;
        usize target_index = 0U;
        usize physical_slot = 0U;
        const usize count = value_count();
        for (usize value_index = 0U;
             value_index < count && target_index < frame_slots.size();
             ++value_index) {
            const usize target_frame_slot = frame_slots[target_index];
            if (target_frame_slot < frame_slot_base) return false;
            const usize target_slot = target_frame_slot - frame_slot_base;
            if (target_slot < physical_slot) return false;
            if (target_slot == physical_slot) {
                if (kind_at(value_index) != ValueKind::reference) return false;
                const ObjectRef reference {bits_at(value_index)};
                if (!reference.is_null()) roots.push_back(reference);
                ++target_index;
            }
            physical_slot += compact_category_two(kind_at(value_index)) ? 2U : 1U;
        }
        return target_index == frame_slots.size();
    }

    [[nodiscard]] Result<usize> write_jit_physical_bits(
        std::span<u64> output) const {
        if (output.size() < used_slots_) {
            return fail(ErrorCode::out_of_range,
                        "JIT operand-frame output is too small");
        }
        usize cursor = 0U;
        const usize count = value_count();
        for (usize index = 0U; index < count; ++index) {
            const ValueKind kind = kind_at(index);
            output[cursor++] = bits_at(index);
            if (compact_category_two(kind)) output[cursor++] = 0U;
        }
        return cursor;
    }

private:
    [[nodiscard]] Status push_raw(ValueKind kind, u64 bits) {
        const usize required = compact_category_two(kind) ? 2U : 1U;
        if (required > maximum_slots_ - used_slots_) {
            return fail(ErrorCode::malformed_class,
                        "operand stack exceeds max_stack slots");
        }
        if (inline_mode_) {
            inline_bits_[value_count_] = bits;
            inline_kinds_[value_count_] = kind;
            ++value_count_;
        } else {
            bits_.push_back(bits);
            kinds_.push_back(kind);
        }
        used_slots_ += required;
        return {};
    }

    [[nodiscard]] usize value_count() const noexcept {
        return inline_mode_ ? value_count_ : bits_.size();
    }

    [[nodiscard]] u64 bits_at(usize index) const noexcept {
        return inline_mode_ ? inline_bits_[index] : bits_[index];
    }

    [[nodiscard]] ValueKind kind_at(usize index) const noexcept {
        return inline_mode_ ? inline_kinds_[index] : kinds_[index];
    }

    usize maximum_slots_ {0U};
    usize used_slots_ {0U};
    usize value_count_ {0U};
    bool inline_mode_ {true};
    // Operand entries are written before they are observed; value_count_
    // defines the initialized prefix, so zeroing both 16-entry banks on every
    // Java call is wasted work.
    std::array<u64, kInlineFrameSlotCapacity> inline_bits_;
    std::array<ValueKind, kInlineFrameSlotCapacity> inline_kinds_;
    std::vector<u64> bits_;
    std::vector<ValueKind> kinds_;
    mutable Value top_cache_ {};
};

class LocalVariables final {
public:
    explicit LocalVariables(usize slot_count)
        : slot_count_(slot_count),
          inline_mode_(slot_count <= kInlineFrameSlotCapacity) {
        if (inline_mode_) {
            // Only kind state needs initialization. Payload bits
            // remain undefined until a non-empty kind is stored, and only the
            // active max_locals prefix can ever be addressed.
            std::fill_n(inline_kinds_.begin(), slot_count_, ValueKind::empty);
        } else {
            bits_.resize(slot_count);
            kinds_.resize(slot_count, ValueKind::empty);
        }
    }

    LocalVariables(const LocalVariables&) = delete;
    LocalVariables& operator=(const LocalVariables&) = delete;

    LocalVariables(LocalVariables&& other) noexcept
        : slot_count_(other.slot_count_),
          inline_mode_(other.inline_mode_) {
        if (inline_mode_) {
            std::copy_n(other.inline_kinds_.begin(),
                        slot_count_,
                        inline_kinds_.begin());
            for (usize index = 0U; index < slot_count_; ++index) {
                if (inline_kinds_[index] != ValueKind::empty) {
                    inline_bits_[index] = other.inline_bits_[index];
                }
            }
        } else {
            bits_ = std::move(other.bits_);
            kinds_ = std::move(other.kinds_);
        }
    }

    LocalVariables& operator=(LocalVariables&& other) noexcept {
        if (this == &other) return *this;
        slot_count_ = other.slot_count_;
        inline_mode_ = other.inline_mode_;
        if (inline_mode_) {
            bits_.clear();
            kinds_.clear();
            std::copy_n(other.inline_kinds_.begin(),
                        slot_count_,
                        inline_kinds_.begin());
            for (usize index = 0U; index < slot_count_; ++index) {
                if (inline_kinds_[index] != ValueKind::empty) {
                    inline_bits_[index] = other.inline_bits_[index];
                }
            }
        } else {
            bits_ = std::move(other.bits_);
            kinds_ = std::move(other.kinds_);
        }
        return *this;
    }

    [[nodiscard]] usize slot_count() const noexcept { return slot_count_; }

    [[nodiscard]] Status set(usize index, Value value) {
        if (value.empty() || value.continuation_slot()) {
            return fail(ErrorCode::invalid_argument,
                        "cannot store an empty or continuation value");
        }
        return set_raw(index, value.kind(), value.raw_bits_unchecked());
    }

    [[nodiscard]] Status set_compact(usize index,
                                     ValueKind kind,
                                     u64 bits) {
        if (kind == ValueKind::empty || kind == ValueKind::continuation) {
            return fail(ErrorCode::invalid_argument,
                        "cannot store an empty or continuation value");
        }
        return set_raw(index, kind, bits);
    }

    [[nodiscard]] Status set_int(usize index, i32 value) {
        return set_raw(index,
                       ValueKind::int32,
                       static_cast<u64>(std::bit_cast<u32>(value)));
    }

    [[nodiscard]] Status set_long(usize index, i64 value) {
        return set_raw(index, ValueKind::int64, std::bit_cast<u64>(value));
    }

    [[nodiscard]] Status set_float(usize index, float value) {
        return set_raw(index,
                       ValueKind::float32,
                       static_cast<u64>(std::bit_cast<u32>(value)));
    }

    [[nodiscard]] Status set_double(usize index, double value) {
        return set_raw(index, ValueKind::float64, std::bit_cast<u64>(value));
    }

    [[nodiscard]] Status set_reference(usize index, ObjectRef value) {
        return set_raw(index, ValueKind::reference, value.bits);
    }

    [[nodiscard]] Result<i32> get_int(usize index) const {
        auto raw = get_raw(index);
        if (!raw) return std::unexpected(raw.error());
        if (raw->kind != ValueKind::int32) {
            return fail(ErrorCode::invalid_state, "value is not a Java int");
        }
        return std::bit_cast<i32>(static_cast<u32>(raw->bits));
    }

    [[nodiscard]] Result<i64> get_long(usize index) const {
        auto raw = get_raw(index);
        if (!raw) return std::unexpected(raw.error());
        if (raw->kind != ValueKind::int64) {
            return fail(ErrorCode::invalid_state, "value is not a Java long");
        }
        return std::bit_cast<i64>(raw->bits);
    }

    [[nodiscard]] Result<float> get_float(usize index) const {
        auto raw = get_raw(index);
        if (!raw) return std::unexpected(raw.error());
        if (raw->kind != ValueKind::float32) {
            return fail(ErrorCode::invalid_state, "value is not a Java float");
        }
        return std::bit_cast<float>(static_cast<u32>(raw->bits));
    }

    [[nodiscard]] Result<double> get_double(usize index) const {
        auto raw = get_raw(index);
        if (!raw) return std::unexpected(raw.error());
        if (raw->kind != ValueKind::float64) {
            return fail(ErrorCode::invalid_state, "value is not a Java double");
        }
        return std::bit_cast<double>(raw->bits);
    }

    [[nodiscard]] Result<ObjectRef> get_reference(usize index) const {
        auto raw = get_raw(index);
        if (!raw) return std::unexpected(raw.error());
        if (raw->kind != ValueKind::reference) {
            return fail(ErrorCode::invalid_state,
                        "value is not a Java object reference");
        }
        return ObjectRef {raw->bits};
    }

    [[nodiscard]] Result<Value> get(usize index) const {
        auto raw = get_raw(index);
        if (!raw) return std::unexpected(raw.error());
        return Value::from_raw_unchecked(raw->kind, raw->bits);
    }

    void clear() noexcept {
        if (inline_mode_) {
            std::fill_n(inline_kinds_.begin(), slot_count_, ValueKind::empty);
        } else {
            std::fill(kinds_.begin(), kinds_.end(), ValueKind::empty);
        }
    }

    void append_reference_roots(std::vector<ObjectRef>& roots) const {
        // Safepoints are intentionally coarse (thousands of bytecodes apart),
        // while local stores can execute every few bytecodes. Scanning the
        // compact kind array here is cheaper overall than maintaining a second
        // reference-index/position structure on every astore/overwrite.
        for (usize index = 0U; index < slot_count_; ++index) {
            if (kind_at(index) != ValueKind::reference) continue;
            const ObjectRef reference {bits_at(index)};
            if (!reference.is_null()) roots.push_back(reference);
        }
    }

    [[nodiscard]] bool append_reference_roots_at_slots(
        std::span<const usize> slots,
        std::vector<ObjectRef>& roots) const {
        for (const usize index : slots) {
            if (index >= slot_count_ || kind_at(index) != ValueKind::reference) {
                return false;
            }
            const ObjectRef reference {bits_at(index)};
            if (!reference.is_null()) roots.push_back(reference);
        }
        return true;
    }

    [[nodiscard]] Result<usize> write_jit_physical_bits(
        std::span<u64> output) const {
        if (output.size() < slot_count_) {
            return fail(ErrorCode::out_of_range,
                        "JIT local-frame output is too small");
        }
        for (usize index = 0U; index < slot_count_; ++index) {
            const ValueKind kind = kind_at(index);
            output[index] = kind == ValueKind::empty ||
                                    kind == ValueKind::continuation
                                ? 0U
                                : bits_at(index);
        }
        return slot_count_;
    }

private:
    struct RawValue final {
        ValueKind kind {ValueKind::empty};
        u64 bits {0U};
    };

    [[nodiscard]] Status set_raw(usize index, ValueKind kind, u64 bits) {
        const usize required = compact_category_two(kind) ? 2U : 1U;
        if (index >= slot_count_ || required > slot_count_ - index) {
            return fail(ErrorCode::malformed_class,
                        "local-variable write exceeds max_locals slots");
        }

        clear_value_covering(index);
        if (required == 2U) {
            clear_value_covering(index + 1U);
        }
        set_slot(index, kind, bits);
        if (required == 2U) {
            set_slot(index + 1U, ValueKind::continuation, 0U);
        }
        return {};
    }

    [[nodiscard]] Result<RawValue> get_raw(usize index) const {
        if (index >= slot_count_) {
            return fail(ErrorCode::malformed_class,
                        "local-variable index exceeds max_locals slots");
        }
        const ValueKind kind = kind_at(index);
        if (kind == ValueKind::empty) {
            return fail(ErrorCode::malformed_class,
                        "read from an uninitialized local variable");
        }
        if (kind == ValueKind::continuation) {
            return fail(ErrorCode::malformed_class,
                        "read from the second slot of a category-2 local");
        }
        if (compact_category_two(kind) &&
            (index + 1U >= slot_count_ ||
             kind_at(index + 1U) != ValueKind::continuation)) {
            return fail(ErrorCode::malformed_class,
                        "corrupt category-2 local variable layout");
        }
        return RawValue {.kind = kind, .bits = bits_at(index)};
    }

    [[nodiscard]] u64 bits_at(usize index) const noexcept {
        return inline_mode_ ? inline_bits_[index] : bits_[index];
    }

    [[nodiscard]] ValueKind kind_at(usize index) const noexcept {
        return inline_mode_ ? inline_kinds_[index] : kinds_[index];
    }

    void set_slot(usize index, ValueKind kind, u64 bits) noexcept {
        if (inline_mode_) {
            inline_kinds_[index] = kind;
            inline_bits_[index] = bits;
        } else {
            kinds_[index] = kind;
            bits_[index] = bits;
        }
    }

    void clear_slot(usize index) noexcept {
        set_slot(index, ValueKind::empty, 0U);
    }

    void clear_value_covering(usize index) noexcept {
        if (index >= slot_count_) {
            return;
        }
        const ValueKind kind = kind_at(index);
        if (kind == ValueKind::continuation) {
            if (index > 0U) {
                clear_slot(index - 1U);
            }
            clear_slot(index);
            return;
        }
        if (compact_category_two(kind) && index + 1U < slot_count_) {
            clear_slot(index + 1U);
        }
        clear_slot(index);
    }

    usize slot_count_ {0U};
    bool inline_mode_ {true};
    std::array<u64, kInlineFrameSlotCapacity> inline_bits_;
    std::array<ValueKind, kInlineFrameSlotCapacity> inline_kinds_;
    std::vector<u64> bits_;
    std::vector<ValueKind> kinds_;
};

} // namespace phoneme::vm
