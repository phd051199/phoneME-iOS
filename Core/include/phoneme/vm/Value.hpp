#pragma once

#include <bit>
#include <limits>
#include <string>

#include "phoneme/base/Error.hpp"

namespace phoneme::vm {

struct ObjectRef final {
    u64 bits {0};

    [[nodiscard]] constexpr bool is_null() const noexcept { return bits == 0; }
    [[nodiscard]] constexpr u32 slot() const noexcept {
        return bits == 0 ? 0 : static_cast<u32>(bits & 0xFFFFFFFFULL);
    }
    [[nodiscard]] constexpr u32 generation() const noexcept {
        return static_cast<u32>(bits >> 32U);
    }

    [[nodiscard]] static constexpr ObjectRef make(u32 slot, u32 generation) noexcept {
        return ObjectRef {(static_cast<u64>(generation) << 32U) |
                          static_cast<u64>(slot)};
    }

    friend constexpr bool operator==(ObjectRef, ObjectRef) noexcept = default;
};

enum class ValueKind : u8 {
    empty,
    continuation,
    int32,
    int64,
    float32,
    float64,
    reference,
    return_address,
};

class Value final {
public:
    constexpr Value() noexcept = default;

    [[nodiscard]] static constexpr Value continuation() noexcept {
        return Value(ValueKind::continuation, 0);
    }

    [[nodiscard]] static constexpr Value from_int(i32 value) noexcept {
        return Value(ValueKind::int32, static_cast<u64>(static_cast<u32>(value)));
    }

    [[nodiscard]] static constexpr Value from_long(i64 value) noexcept {
        return Value(ValueKind::int64, static_cast<u64>(value));
    }

    [[nodiscard]] static Value from_float(float value) noexcept {
        return Value(ValueKind::float32,
                     static_cast<u64>(std::bit_cast<u32>(value)));
    }

    [[nodiscard]] static Value from_double(double value) noexcept {
        return Value(ValueKind::float64, std::bit_cast<u64>(value));
    }

    [[nodiscard]] static constexpr Value from_reference(ObjectRef value) noexcept {
        return Value(ValueKind::reference, value.bits);
    }

    [[nodiscard]] static constexpr Value return_address(usize value) noexcept {
        return Value(ValueKind::return_address, static_cast<u64>(value));
    }

    // VM-internal reconstruction path for compact slot storage. The verifier
    // and slot container own the type invariant; avoid repeating conversions
    // through the checked public accessors when materializing a Value at the
    // compatibility boundary.
    [[nodiscard]] static constexpr Value from_raw_unchecked(
        ValueKind kind,
        u64 bits) noexcept {
        return Value(kind, bits);
    }

    [[nodiscard]] constexpr ValueKind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr bool empty() const noexcept {
        return kind_ == ValueKind::empty;
    }
    [[nodiscard]] constexpr bool continuation_slot() const noexcept {
        return kind_ == ValueKind::continuation;
    }
    [[nodiscard]] constexpr bool category_two() const noexcept {
        return kind_ == ValueKind::int64 || kind_ == ValueKind::float64;
    }

    [[nodiscard]] Result<i32> as_int() const {
        if (kind_ != ValueKind::int32) {
            return fail(ErrorCode::invalid_state, "value is not a Java int");
        }
        return static_cast<i32>(static_cast<u32>(bits_));
    }

    [[nodiscard]] Result<i64> as_long() const {
        if (kind_ != ValueKind::int64) {
            return fail(ErrorCode::invalid_state, "value is not a Java long");
        }
        return static_cast<i64>(bits_);
    }

    [[nodiscard]] Result<float> as_float() const {
        if (kind_ != ValueKind::float32) {
            return fail(ErrorCode::invalid_state, "value is not a Java float");
        }
        return std::bit_cast<float>(static_cast<u32>(bits_));
    }

    [[nodiscard]] Result<double> as_double() const {
        if (kind_ != ValueKind::float64) {
            return fail(ErrorCode::invalid_state, "value is not a Java double");
        }
        return std::bit_cast<double>(bits_);
    }

    [[nodiscard]] constexpr u64 raw_bits_unchecked() const noexcept {
        return bits_;
    }

    // JIT-only payload access. Heap array payload leases use this address while
    // the owning array remains rooted. Value objects are fixed-size and Java
    // arrays never resize after allocation, so the payload address is stable
    // for the lifetime of the array object.
    [[nodiscard]] constexpr const u64* raw_bits_address_unchecked() const noexcept {
        return &bits_;
    }

    [[nodiscard]] constexpr u64* raw_bits_address_unchecked() noexcept {
        return &bits_;
    }

    [[nodiscard]] constexpr ObjectRef reference_unchecked() const noexcept {
        return ObjectRef {bits_};
    }

    [[nodiscard]] Result<ObjectRef> as_reference() const {
        if (kind_ != ValueKind::reference) {
            return fail(ErrorCode::invalid_state,
                        "value is not a Java object reference");
        }
        return reference_unchecked();
    }

    [[nodiscard]] Result<usize> as_return_address() const {
        if (kind_ != ValueKind::return_address) {
            return fail(ErrorCode::invalid_state,
                        "value is not a return address");
        }
        if (bits_ > static_cast<u64>(std::numeric_limits<usize>::max())) {
            return fail(ErrorCode::overflow, "return address exceeds size_t");
        }
        return static_cast<usize>(bits_);
    }

private:
    constexpr Value(ValueKind kind, u64 bits) noexcept : kind_(kind), bits_(bits) {}

    ValueKind kind_ {ValueKind::empty};
    u64 bits_ {0};
};

static_assert(sizeof(Value) == 16, "Java slot value must remain compact and aligned");

} // namespace phoneme::vm
