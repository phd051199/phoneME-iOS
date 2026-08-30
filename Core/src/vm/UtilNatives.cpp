#include "UtilNatives.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/NativeMethodRegistry.hpp"

namespace phoneme::vm {
namespace {

constexpr usize kVectorDataField = 0;
constexpr usize kVectorCountField = 1;
constexpr usize kVectorIncrementField = 2;
constexpr usize kArrayListMutationModeField = 3;
constexpr i32 kArrayListImmutable = 1;
constexpr i32 kArrayListFixedSize = 2;
constexpr usize kHashtableKeysField = 0;
constexpr usize kHashtableValuesField = 1;
constexpr usize kHashtableCountField = 2;
constexpr usize kHashtableHashesField = 3;
constexpr usize kHashtableBucketsField = 4;
constexpr usize kHashtableIterationOrderField = 5;
constexpr usize kHashtableTableCapacityField = 6;
constexpr usize kHashtableThresholdField = 7;
constexpr usize kHashtableLoadFactorField = 8;
constexpr usize kEnumerationArrayField = 0;
constexpr usize kEnumerationIndexField = 1;
constexpr usize kEnumerationSizeField = 2;
constexpr usize kRandomSeedField = 0;
constexpr usize kDateMillisField = 0;
constexpr usize kTokenizerStringField = 0;
constexpr usize kTokenizerDelimitersField = 1;
constexpr usize kTokenizerPositionField = 2;
constexpr usize kTokenizerReturnDelimitersField = 3;
constexpr u64 kRandomMultiplier = 0x5DEECE66DULL;
constexpr u64 kRandomAddend = 0xBULL;
constexpr u64 kRandomMask = (1ULL << 48U) - 1ULL;

void add(NativeMethodRegistry& registry,
         std::string owner,
         std::string name,
         std::string descriptor,
         NativeMethod method,
         NativeJitPolicy jit_policy = NativeJitPolicy::conservative) {
    auto registered = registry.register_method(std::move(owner),
                                               std::move(name),
                                               std::move(descriptor),
                                               std::move(method),
                                               jit_policy);
    if (!registered) {
        std::terminate();
    }
}

[[nodiscard]] Result<ObjectRef> receiver(
    std::span<const Value> arguments) {
    if (arguments.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "java.util method has no receiver");
    }
    auto reference = arguments.front().as_reference();
    if (!reference || reference->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "java.util receiver is null");
    }
    return *reference;
}

[[nodiscard]] Result<ObjectRef> reference_argument(
    std::span<const Value> arguments,
    usize index,
    bool nullable = false) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    "java.util reference argument is missing");
    }
    auto reference = arguments[index].as_reference();
    if (!reference) return std::unexpected(reference.error());
    if (!nullable && reference->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "java.util reference argument is null");
    }
    return *reference;
}

[[nodiscard]] Result<i32> int_argument(
    std::span<const Value> arguments,
    usize index) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    "java.util int argument is missing");
    }
    return arguments[index].as_int();
}

[[nodiscard]] Result<i64> long_argument(
    std::span<const Value> arguments,
    usize index) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    "java.util long argument is missing");
    }
    return arguments[index].as_long();
}

[[nodiscard]] i64 current_time_millis() noexcept {
    return static_cast<i64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

[[nodiscard]] Result<i64> delayed_time(i64 delay) {
    if (delay < 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         "Timer delay cannot be negative");
    }
    const i64 now = current_time_millis();
    if (delay > std::numeric_limits<i64>::max() - now) {
        return fail_java("java/lang/IllegalArgumentException",
                         "Timer delay overflows absolute time");
    }
    return now + delay;
}

[[nodiscard]] Result<i32> int_field(Machine& machine,
                                    ObjectRef object,
                                    usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

template <usize N>
[[nodiscard]] Result<std::array<Value, N>> field_values(
    Machine& machine,
    ObjectRef object,
    const std::array<usize, N>& indices) {
    std::array<Value, N> values {};
    auto loaded = machine.heap().read_fields(object, indices, values);
    if (!loaded) return std::unexpected(loaded.error());
    return values;
}

template <usize N>
[[nodiscard]] Status set_field_values(
    Machine& machine,
    ObjectRef object,
    const std::array<usize, N>& indices,
    const std::array<Value, N>& values) {
    return machine.heap().write_fields(object, indices, values);
}

[[nodiscard]] Result<i64> long_field(Machine& machine,
                                     ObjectRef object,
                                     usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_long();
}

[[nodiscard]] Result<ObjectRef> reference_field(Machine& machine,
                                                 ObjectRef object,
                                                 usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Status set_int_field(Machine& machine,
                                   ObjectRef object,
                                   usize index,
                                   i32 value) {
    return machine.heap().set_field(object, index, Value::from_int(value));
}

[[nodiscard]] Status set_long_field(Machine& machine,
                                    ObjectRef object,
                                    usize index,
                                    i64 value) {
    return machine.heap().set_field(object, index, Value::from_long(value));
}

[[nodiscard]] Status set_reference_field(Machine& machine,
                                         ObjectRef object,
                                         usize index,
                                         ObjectRef value) {
    return machine.heap().set_field(object, index,
                                    Value::from_reference(value));
}

[[nodiscard]] Status require_array_list_mutation(
    Machine& machine,
    ObjectRef list,
    bool structural) {
    auto mode = int_field(machine, list, kArrayListMutationModeField);
    if (!mode) return std::unexpected(mode.error());
    if (*mode == kArrayListImmutable ||
        (structural && *mode == kArrayListFixedSize)) {
        return fail_java("java/lang/UnsupportedOperationException",
                         structural
                             ? "list does not support structural modification"
                             : "list is immutable");
    }
    return {};
}

[[nodiscard]] Result<ObjectRef> create_string(Machine& machine,
                                              std::u16string text) {
    auto object = machine.class_states().allocate_instance(
        machine.heap(), "java/lang/String");
    if (!object) return std::unexpected(object.error());
    auto attached = machine.heap().attach_string(*object, std::move(text));
    if (!attached) return std::unexpected(attached.error());
    return *object;
}

[[nodiscard]] Result<std::u16string> collection_value_text(
    Machine& machine,
    ObjectRef value,
    ObjectRef owner,
    std::u16string_view self_text) {
    if (value.is_null()) return std::u16string(u"null");
    if (value == owner) return std::u16string(self_text);
    auto class_name = machine.heap().class_name(value);
    if (!class_name) return std::unexpected(class_name.error());
    auto invoked = machine.invoke_instance(
        value, *class_name, "toString", "()Ljava/lang/String;");
    if (!invoked) return std::unexpected(invoked.error());
    if (invoked->throwable.has_value()) {
        auto thrown = machine.heap().class_name(*invoked->throwable);
        if (!thrown) return std::unexpected(thrown.error());
        return fail_java(*thrown, "collection element toString threw");
    }
    if (!invoked->return_value.has_value()) {
        return fail(ErrorCode::internal_error,
                    "collection element toString returned no value");
    }
    auto string = invoked->return_value->as_reference();
    if (!string) return std::unexpected(string.error());
    if (string->is_null()) return std::u16string(u"null");
    return machine.heap().string_value(*string);
}

[[nodiscard]] constexpr bool is_high_surrogate(char16_t value) noexcept {
    return value >= static_cast<char16_t>(0xD800U) &&
           value <= static_cast<char16_t>(0xDBFFU);
}

[[nodiscard]] constexpr bool is_low_surrogate(char16_t value) noexcept {
    return value >= static_cast<char16_t>(0xDC00U) &&
           value <= static_cast<char16_t>(0xDFFFU);
}

struct Utf16CodePoint final {
    u32 value {0};
    usize units {1};
};

[[nodiscard]] Utf16CodePoint code_point_at(std::u16string_view text,
                                           usize index) noexcept {
    const auto first = text[index];
    if (is_high_surrogate(first) && index + 1U < text.size()) {
        const auto second = text[index + 1U];
        if (is_low_surrogate(second)) {
            const u32 high = static_cast<u32>(first) - 0xD800U;
            const u32 low = static_cast<u32>(second) - 0xDC00U;
            return Utf16CodePoint {
                .value = 0x10000U + (high << 10U) + low,
                .units = 2U,
            };
        }
    }
    return Utf16CodePoint {
        .value = static_cast<u32>(first),
        .units = 1U,
    };
}

[[nodiscard]] bool delimiter_contains(std::u16string_view delimiters,
                                      u32 code_point) noexcept {
    for (usize index = 0; index < delimiters.size();) {
        const auto delimiter = code_point_at(delimiters, index);
        if (delimiter.value == code_point) return true;
        index += delimiter.units;
    }
    return false;
}

[[nodiscard]] bool is_delimiter_at(std::u16string_view text,
                                   usize index,
                                   std::u16string_view delimiters,
                                   usize* units = nullptr) noexcept {
    const auto candidate = code_point_at(text, index);
    if (units != nullptr) *units = candidate.units;
    return delimiter_contains(delimiters, candidate.value);
}

struct TokenizerState final {
    ObjectRef object {};
    ObjectRef string_object {};
    ObjectRef delimiter_object {};
    std::u16string string;
    std::u16string delimiters;
    usize position {0};
    bool return_delimiters {false};
};

[[nodiscard]] Result<TokenizerState> tokenizer_state(
    Machine& machine,
    std::span<const Value> arguments) {
    auto object = receiver(arguments);
    if (!object) return std::unexpected(object.error());
    constexpr std::array<usize, 4> fields {
        kTokenizerStringField,
        kTokenizerDelimitersField,
        kTokenizerPositionField,
        kTokenizerReturnDelimitersField,
    };
    auto values = field_values(machine, *object, fields);
    if (!values) return std::unexpected(values.error());
    auto string_object = (*values)[0U].as_reference();
    auto delimiter_object = (*values)[1U].as_reference();
    auto position = (*values)[2U].as_int();
    auto return_delimiters = (*values)[3U].as_int();
    if (!string_object || !delimiter_object || !position || !return_delimiters) {
        return fail(ErrorCode::invalid_state,
                    "StringTokenizer state is invalid");
    }
    if (string_object->is_null() || delimiter_object->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "StringTokenizer string or delimiters are null");
    }
    auto string = machine.heap().string_value(*string_object);
    auto delimiters = machine.heap().string_value(*delimiter_object);
    if (!string || !delimiters) {
        return fail(ErrorCode::invalid_state,
                    "StringTokenizer text payload is unavailable");
    }
    if (*position < 0 || static_cast<usize>(*position) > string->size()) {
        return fail(ErrorCode::invalid_state,
                    "StringTokenizer position is outside the string");
    }
    return TokenizerState {
        .object = *object,
        .string_object = *string_object,
        .delimiter_object = *delimiter_object,
        .string = std::move(*string),
        .delimiters = std::move(*delimiters),
        .position = static_cast<usize>(*position),
        .return_delimiters = *return_delimiters != 0,
    };
}

[[nodiscard]] usize skip_delimiters(const TokenizerState& state,
                                    usize position) noexcept {
    if (state.return_delimiters) return position;
    while (position < state.string.size()) {
        usize units = 1U;
        if (!is_delimiter_at(state.string, position,
                             state.delimiters, &units)) {
            break;
        }
        position += units;
    }
    return position;
}

[[nodiscard]] usize scan_token_end(const TokenizerState& state,
                                   usize position) noexcept {
    while (position < state.string.size()) {
        usize units = 1U;
        if (is_delimiter_at(state.string, position,
                            state.delimiters, &units)) {
            break;
        }
        position += units;
    }
    return position;
}

[[nodiscard]] Result<std::optional<Value>> tokenizer_next(
    Machine& machine,
    TokenizerState state) {
    const usize start = skip_delimiters(state, state.position);
    if (start >= state.string.size()) {
        return fail_java("java/util/NoSuchElementException",
                         "StringTokenizer has no more tokens");
    }

    usize end = start;
    usize delimiter_units = 1U;
    if (state.return_delimiters &&
        is_delimiter_at(state.string, start, state.delimiters,
                        &delimiter_units)) {
        end += delimiter_units;
    } else {
        end = scan_token_end(state, start);
    }

    if (end > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return fail(ErrorCode::overflow,
                    "StringTokenizer position exceeds Java int range");
    }
    auto stored = set_int_field(machine, state.object,
                                kTokenizerPositionField,
                                static_cast<i32>(end));
    if (!stored) return std::unexpected(stored.error());
    auto token = create_string(machine,
        state.string.substr(start, end - start));
    if (!token) return std::unexpected(token.error());
    return std::optional<Value>(Value::from_reference(*token));
}

[[nodiscard]] Result<i32> object_hash(Machine& machine,
                                      ObjectRef object) {
    if (object.is_null()) return 0;
    auto class_name = machine.heap().class_name(object);
    if (!class_name) return std::unexpected(class_name.error());
    auto hash = machine.invoke_instance(
        object, *class_name, "hashCode", "()I", {});
    if (!hash) return std::unexpected(hash.error());
    if (hash->throwable.has_value()) {
        auto throwable_class = machine.heap().class_name(*hash->throwable);
        if (!throwable_class) return std::unexpected(throwable_class.error());
        return fail_java(*throwable_class,
                         "Hashtable key hashCode() threw an exception");
    }
    if (!hash->return_value.has_value()) {
        return fail(ErrorCode::internal_error,
                    "Object.hashCode returned without an int value");
    }
    auto value = hash->return_value->as_int();
    if (!value) return std::unexpected(value.error());
    return *value;
}

[[nodiscard]] Result<i32> comparator_compare(Machine& machine,
                                              ObjectRef comparator,
                                              ObjectRef left,
                                              ObjectRef right) {
    if (comparator.is_null()) {
        if (left.is_null() || right.is_null()) {
            if (left == right) return 0;
            return left.is_null() ? -1 : 1;
        }
        auto left_class = machine.heap().class_name(left);
        if (!left_class) return std::unexpected(left_class.error());
        const Value argument = Value::from_reference(right);
        auto result = machine.invoke_instance(
            left, *left_class, "compareTo", "(Ljava/lang/Object;)I",
            std::span<const Value>(&argument, 1U));
        if (!result) return std::unexpected(result.error());
        if (result->throwable.has_value()) {
            auto throwable = machine.heap().class_name(*result->throwable);
            if (!throwable) return std::unexpected(throwable.error());
            return fail_java(*throwable, "Comparable.compareTo threw");
        }
        if (!result->return_value.has_value()) {
            return fail(ErrorCode::internal_error,
                        "Comparable.compareTo returned no value");
        }
        return result->return_value->as_int();
    }
    auto comparator_class = machine.heap().class_name(comparator);
    if (!comparator_class) return std::unexpected(comparator_class.error());
    const std::array<Value, 2> arguments {
        Value::from_reference(left), Value::from_reference(right),
    };
    auto result = machine.invoke_instance(
        comparator, *comparator_class, "compare",
        "(Ljava/lang/Object;Ljava/lang/Object;)I", arguments);
    if (!result) return std::unexpected(result.error());
    if (result->throwable.has_value()) {
        auto throwable = machine.heap().class_name(*result->throwable);
        if (!throwable) return std::unexpected(throwable.error());
        return fail_java(*throwable, "Comparator.compare threw");
    }
    if (!result->return_value.has_value()) {
        return fail(ErrorCode::internal_error,
                    "Comparator.compare returned no value");
    }
    return result->return_value->as_int();
}

[[nodiscard]] Result<bool> values_equal(Machine& machine,
                                        ObjectRef left,
                                        ObjectRef right) {
    if (left == right) return true;
    if (left.is_null() || right.is_null()) return false;
    auto left_class = machine.heap().class_name(left);
    auto right_class = machine.heap().class_name(right);
    if (!left_class || !right_class) {
        return fail(ErrorCode::invalid_argument,
                    "collection contains a stale reference");
    }
    if (*left_class == *right_class &&
        (*left_class == "java/lang/String" ||
         *left_class == "java/lang/StringBuilder" ||
         *left_class == "java/lang/StringBuffer")) {
        auto left_text = machine.heap().string_value(left);
        auto right_text = machine.heap().string_value(right);
        if (!left_text || !right_text) {
            return fail(ErrorCode::invalid_state,
                        "text object has no payload");
        }
        return *left_text == *right_text;
    }
    if (*left_class == *right_class &&
        (*left_class == "java/lang/Boolean" ||
         *left_class == "java/lang/Byte" ||
         *left_class == "java/lang/Short" ||
         *left_class == "java/lang/Integer" ||
         *left_class == "java/lang/Long" ||
         *left_class == "java/lang/Character" ||
         *left_class == "java/lang/Float" ||
         *left_class == "java/lang/Double")) {
        auto left_value = machine.heap().field(left, 0);
        auto right_value = machine.heap().field(right, 0);
        if (!left_value || !right_value ||
            left_value->kind() != right_value->kind()) {
            return false;
        }
        switch (left_value->kind()) {
        case ValueKind::int32: {
            auto left_number = left_value->as_int();
            auto right_number = right_value->as_int();
            return left_number && right_number &&
                   *left_number == *right_number;
        }
        case ValueKind::int64: {
            auto left_number = left_value->as_long();
            auto right_number = right_value->as_long();
            return left_number && right_number &&
                   *left_number == *right_number;
        }
        case ValueKind::float32: {
            auto left_number = left_value->as_float();
            auto right_number = right_value->as_float();
            return left_number && right_number &&
                   *left_number == *right_number;
        }
        case ValueKind::float64: {
            auto left_number = left_value->as_double();
            auto right_number = right_value->as_double();
            return left_number && right_number &&
                   *left_number == *right_number;
        }
        default:
            return false;
        }
    }
    const Value argument = Value::from_reference(right);
    auto equality = machine.invoke_instance(
        left,
        *left_class,
        "equals",
        "(Ljava/lang/Object;)Z",
        std::span<const Value>(&argument, 1U));
    if (!equality) return std::unexpected(equality.error());
    if (equality->throwable.has_value()) {
        auto throwable_class = machine.heap().class_name(*equality->throwable);
        if (!throwable_class) return std::unexpected(throwable_class.error());
        return fail_java(*throwable_class,
                         "collection element equals() threw an exception");
    }
    if (!equality->return_value.has_value()) {
        return fail(ErrorCode::internal_error,
                    "Object.equals returned without a boolean value");
    }
    auto equal = equality->return_value->as_int();
    if (!equal) return std::unexpected(equal.error());
    return *equal != 0;
}

[[nodiscard]] Result<ObjectRef> allocate_object_array(Machine& machine,
                                                       usize length) {
    return machine.heap().allocate_array(
        "[Ljava/lang/Object;", length, Value::from_reference({}));
}

[[nodiscard]] Result<ObjectRef> finish_enumeration(Machine& machine,
                                                   ObjectRef array,
                                                   i32 size) {
    if (size < 0) {
        return fail(ErrorCode::invalid_state,
                    "Enumeration size is negative");
    }
    auto array_root = machine.pin_native_root(array);
    if (!array_root) return std::unexpected(array_root.error());
    auto enumeration = machine.class_states().allocate_instance(
        machine.heap(), "java/util/ArrayEnumeration");
    if (!enumeration) return std::unexpected(enumeration.error());
    constexpr std::array<usize, 3> fields {
        kEnumerationArrayField,
        kEnumerationIndexField,
        kEnumerationSizeField,
    };
    const std::array<Value, 3> initial_values {
        Value::from_reference(array),
        Value::from_int(0),
        Value::from_int(size),
    };
    auto initialized = set_field_values(
        machine, *enumeration, fields, initial_values);
    if (!initialized) return std::unexpected(initialized.error());
    return *enumeration;
}

[[nodiscard]] Result<ObjectRef> make_enumeration(
    Machine& machine,
    std::span<const ObjectRef> values) {
    auto array = allocate_object_array(machine, values.size());
    if (!array) return std::unexpected(array.error());
    auto stored = machine.heap().write_reference_array(*array, 0U, values);
    if (!stored) return std::unexpected(stored.error());
    return finish_enumeration(
        machine, *array, static_cast<i32>(values.size()));
}

[[nodiscard]] Result<ObjectRef> make_enumeration_from_array(
    Machine& machine,
    ObjectRef source,
    i32 size) {
    if (size < 0) {
        return fail(ErrorCode::invalid_state,
                    "Enumeration source size is negative");
    }
    auto array = allocate_object_array(machine, static_cast<usize>(size));
    if (!array) return std::unexpected(array.error());
    if (size != 0) {
        auto copied = machine.heap().copy_array_range(
            source, 0U, *array, 0U, static_cast<usize>(size));
        if (!copied) return std::unexpected(copied.error());
    }
    return finish_enumeration(machine, *array, size);
}

[[nodiscard]] Status initialize_vector(Machine& machine,
                                       ObjectRef object,
                                       i32 capacity,
                                       i32 increment) {
    if (capacity < 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         "Vector capacity is negative");
    }
    auto data = allocate_object_array(machine,
                                      static_cast<usize>(capacity));
    if (!data) return std::unexpected(data.error());
    constexpr std::array<usize, 3> fields {
        kVectorDataField,
        kVectorCountField,
        kVectorIncrementField,
    };
    const std::array<Value, 3> values {
        Value::from_reference(*data),
        Value::from_int(0),
        Value::from_int(increment),
    };
    return set_field_values(machine, object, fields, values);
}

[[nodiscard]] Result<ObjectRef> vector_data(Machine& machine,
                                            ObjectRef vector) {
    auto data = reference_field(machine, vector, kVectorDataField);
    if (!data) return std::unexpected(data.error());
    if (data->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "Vector has not been initialized");
    }
    return *data;
}

struct VectorStorageState final {
    ObjectRef data {};
    i32 count {0};
};

struct VectorCapacityState final {
    ObjectRef data {};
    i32 count {0};
    i32 increment {0};
};

struct ArrayListState final {
    ObjectRef data {};
    i32 count {0};
    i32 increment {0};
    i32 mutation_mode {0};
};

[[nodiscard]] Result<VectorStorageState> vector_storage_state(
    Machine& machine,
    ObjectRef vector) {
    constexpr std::array<usize, 2> fields {
        kVectorDataField,
        kVectorCountField,
    };
    auto values = field_values(machine, vector, fields);
    if (!values) return std::unexpected(values.error());
    auto data = (*values)[0U].as_reference();
    auto count = (*values)[1U].as_int();
    if (!data || !count || data->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "Vector storage state is invalid");
    }
    return VectorStorageState {
        .data = *data,
        .count = *count,
    };
}

[[nodiscard]] Result<VectorCapacityState> vector_capacity_state(
    Machine& machine,
    ObjectRef vector) {
    constexpr std::array<usize, 3> fields {
        kVectorDataField,
        kVectorCountField,
        kVectorIncrementField,
    };
    auto values = field_values(machine, vector, fields);
    if (!values) return std::unexpected(values.error());
    auto data = (*values)[0U].as_reference();
    auto count = (*values)[1U].as_int();
    auto increment = (*values)[2U].as_int();
    if (!data || !count || !increment || data->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "Vector capacity state is invalid");
    }
    return VectorCapacityState {
        .data = *data,
        .count = *count,
        .increment = *increment,
    };
}

[[nodiscard]] Result<ArrayListState> array_list_state(
    Machine& machine,
    ObjectRef list,
    bool structural) {
    constexpr std::array<usize, 4> fields {
        kVectorDataField,
        kVectorCountField,
        kVectorIncrementField,
        kArrayListMutationModeField,
    };
    auto values = field_values(machine, list, fields);
    if (!values) return std::unexpected(values.error());
    auto data = (*values)[0U].as_reference();
    auto count = (*values)[1U].as_int();
    auto increment = (*values)[2U].as_int();
    auto mode = (*values)[3U].as_int();
    if (!data || !count || !increment || !mode || data->is_null() ||
        *count < 0) {
        return fail(ErrorCode::invalid_state,
                    "ArrayList state is invalid");
    }
    if (*mode == kArrayListImmutable ||
        (structural && *mode == kArrayListFixedSize)) {
        return fail_java("java/lang/UnsupportedOperationException",
                         structural
                             ? "list does not support structural modification"
                             : "list is immutable");
    }
    return ArrayListState {
        .data = *data,
        .count = *count,
        .increment = *increment,
        .mutation_mode = *mode,
    };
}

[[nodiscard]] Result<ObjectRef> ensure_vector_capacity_from_state(
    Machine& machine,
    ObjectRef vector,
    const VectorCapacityState& state,
    i32 minimum) {
    auto capacity = machine.heap().array_length(state.data);
    if (!capacity) return std::unexpected(capacity.error());
    if (minimum <= static_cast<i32>(*capacity)) return state.data;

    usize new_capacity = state.increment > 0
        ? *capacity + static_cast<usize>(state.increment)
        : (*capacity == 0U ? 1U : *capacity * 2U);
    if (new_capacity < static_cast<usize>(minimum)) {
        new_capacity = static_cast<usize>(minimum);
    }
    auto replacement = allocate_object_array(machine, new_capacity);
    if (!replacement) return std::unexpected(replacement.error());
    if (state.count != 0) {
        auto copied = machine.heap().copy_array_range(
            state.data, 0U, *replacement, 0U,
            static_cast<usize>(state.count));
        if (!copied) return std::unexpected(copied.error());
    }
    auto stored = set_reference_field(
        machine, vector, kVectorDataField, *replacement);
    if (!stored) return std::unexpected(stored.error());
    return *replacement;
}

[[nodiscard]] Status ensure_vector_capacity(Machine& machine,
                                            ObjectRef vector,
                                            i32 minimum) {
    auto state = vector_capacity_state(machine, vector);
    if (!state) return std::unexpected(state.error());
    auto data = ensure_vector_capacity_from_state(
        machine, vector, *state, minimum);
    if (!data) return std::unexpected(data.error());
    return {};
}

[[nodiscard]] Result<i32> vector_index_of(Machine& machine,
                                          ObjectRef vector,
                                          ObjectRef target,
                                          i32 start,
                                          bool reverse) {
    auto state = vector_storage_state(machine, vector);
    if (!state) return std::unexpected(state.error());
    if (!reverse) {
        i32 index = start < 0 ? 0 : start;
        for (; index < state->count; ++index) {
            auto value = machine.heap().element(state->data,
                                                static_cast<usize>(index));
            if (!value) return std::unexpected(value.error());
            auto reference = value->as_reference();
            if (!reference) return std::unexpected(reference.error());
            auto equal = values_equal(machine, *reference, target);
            if (!equal) return std::unexpected(equal.error());
            if (*equal) return index;
        }
        return -1;
    }
    i32 index = std::min(start, state->count - 1);
    for (; index >= 0; --index) {
        auto value = machine.heap().element(state->data,
                                            static_cast<usize>(index));
        if (!value) return std::unexpected(value.error());
        auto reference = value->as_reference();
        if (!reference) return std::unexpected(reference.error());
        auto equal = values_equal(machine, *reference, target);
        if (!equal) return std::unexpected(equal.error());
        if (*equal) return index;
    }
    return -1;
}

[[nodiscard]] Status remove_vector_index_from_state(
    Machine& machine,
    ObjectRef vector,
    const VectorStorageState& state,
    i32 index) {
    if (index < 0 || index >= state.count) {
        return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                         "Vector index is out of range");
    }
    const usize shifted = static_cast<usize>(state.count - index - 1);
    if (shifted != 0U) {
        auto copied = machine.heap().copy_array_range(
            state.data, static_cast<usize>(index + 1),
            state.data, static_cast<usize>(index), shifted);
        if (!copied) return copied;
    }
    auto cleared = machine.heap().set_element(
        state.data, static_cast<usize>(state.count - 1),
        Value::from_reference({}));
    if (!cleared) return cleared;
    return set_int_field(machine, vector, kVectorCountField, state.count - 1);
}

[[nodiscard]] Status remove_vector_index(Machine& machine,
                                         ObjectRef vector,
                                         i32 index) {
    auto state = vector_storage_state(machine, vector);
    if (!state) return std::unexpected(state.error());
    return remove_vector_index_from_state(machine, vector, *state, index);
}

[[nodiscard]] Status initialize_tokenizer(Machine& machine,
                                          ObjectRef object,
                                          ObjectRef string_object,
                                          ObjectRef delimiter_object,
                                          bool return_delimiters) {
    if (string_object.is_null() || delimiter_object.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "StringTokenizer string or delimiters are null");
    }
    auto string = machine.heap().string_value(string_object);
    auto delimiters = machine.heap().string_value(delimiter_object);
    if (!string || !delimiters) {
        return fail_java("java/lang/IllegalArgumentException",
                         "StringTokenizer requires String arguments");
    }
    constexpr std::array<usize, 4> fields {
        kTokenizerStringField,
        kTokenizerDelimitersField,
        kTokenizerPositionField,
        kTokenizerReturnDelimitersField,
    };
    const std::array<Value, 4> values {
        Value::from_reference(string_object),
        Value::from_reference(delimiter_object),
        Value::from_int(0),
        Value::from_int(return_delimiters ? 1 : 0),
    };
    return set_field_values(machine, object, fields, values);
}

[[nodiscard]] Result<std::optional<Value>> tokenizer_has_more(
    Machine& machine,
    std::span<const Value> arguments) {
    auto state = tokenizer_state(machine, arguments);
    if (!state) return std::unexpected(state.error());
    const usize position = skip_delimiters(*state, state->position);
    return std::optional<Value>(Value::from_int(
        position < state->string.size() ? 1 : 0));
}

[[nodiscard]] Result<std::optional<Value>> tokenizer_count_tokens(
    Machine& machine,
    std::span<const Value> arguments) {
    auto state = tokenizer_state(machine, arguments);
    if (!state) return std::unexpected(state.error());
    usize position = state->position;
    usize count = 0U;
    while (position < state->string.size()) {
        position = skip_delimiters(*state, position);
        if (position >= state->string.size()) break;
        usize delimiter_units = 1U;
        if (state->return_delimiters &&
            is_delimiter_at(state->string, position, state->delimiters,
                            &delimiter_units)) {
            position += delimiter_units;
        } else {
            position = scan_token_end(*state, position);
        }
        ++count;
        if (count > static_cast<usize>(std::numeric_limits<i32>::max())) {
            return fail(ErrorCode::overflow,
                        "StringTokenizer token count exceeds Java int range");
        }
    }
    return std::optional<Value>(
        Value::from_int(static_cast<i32>(count)));
}

void register_string_tokenizer(NativeMethodRegistry& registry) {
    add(registry, "java/util/StringTokenizer", "<init>",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto string_object = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!string_object) return std::unexpected(string_object.error());
            auto delimiters = create_string(machine, u" \t\n\r\f");
            if (!delimiters) return std::unexpected(delimiters.error());
            auto initialized = initialize_tokenizer(
                machine, *object, *string_object, *delimiters, false);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/StringTokenizer", "<init>",
        "(Ljava/lang/String;Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto string_object = reference_argument(arguments, 1U);
            auto delimiters = reference_argument(arguments, 2U);
            if (!object) return std::unexpected(object.error());
            if (!string_object) return std::unexpected(string_object.error());
            if (!delimiters) return std::unexpected(delimiters.error());
            auto initialized = initialize_tokenizer(
                machine, *object, *string_object, *delimiters, false);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/StringTokenizer", "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Z)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto string_object = reference_argument(arguments, 1U);
            auto delimiters = reference_argument(arguments, 2U);
            auto return_delimiters = int_argument(arguments, 3U);
            if (!object) return std::unexpected(object.error());
            if (!string_object) return std::unexpected(string_object.error());
            if (!delimiters) return std::unexpected(delimiters.error());
            if (!return_delimiters) {
                return std::unexpected(return_delimiters.error());
            }
            auto initialized = initialize_tokenizer(
                machine, *object, *string_object, *delimiters,
                *return_delimiters != 0);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/StringTokenizer", "hasMoreTokens", "()Z",
        tokenizer_has_more);
    add(registry, "java/util/StringTokenizer", "hasMoreElements", "()Z",
        tokenizer_has_more);
    add(registry, "java/util/StringTokenizer", "nextToken",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = tokenizer_state(machine, arguments);
            if (!state) return std::unexpected(state.error());
            return tokenizer_next(machine, std::move(*state));
        });
    add(registry, "java/util/StringTokenizer", "nextElement",
        "()Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = tokenizer_state(machine, arguments);
            if (!state) return std::unexpected(state.error());
            return tokenizer_next(machine, std::move(*state));
        });
    add(registry, "java/util/StringTokenizer", "nextToken",
        "(Ljava/lang/String;)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto delimiters = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!delimiters) return std::unexpected(delimiters.error());
            auto delimiter_text = machine.heap().string_value(*delimiters);
            if (!delimiter_text) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "StringTokenizer delimiter is not a String");
            }
            auto stored = set_reference_field(
                machine, *object, kTokenizerDelimitersField, *delimiters);
            if (!stored) return std::unexpected(stored.error());
            auto state = tokenizer_state(machine, arguments.first(1U));
            if (!state) return std::unexpected(state.error());
            return tokenizer_next(machine, std::move(*state));
        });
    add(registry, "java/util/StringTokenizer", "countTokens", "()I",
        tokenizer_count_tokens);
}

void register_enumeration(NativeMethodRegistry& registry) {
    add(registry, "java/util/ArrayEnumeration", "hasMoreElements", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 2> fields {
                kEnumerationIndexField,
                kEnumerationSizeField,
            };
            auto values = field_values(machine, *object, fields);
            if (!values) return std::unexpected(values.error());
            auto index = (*values)[0U].as_int();
            auto size = (*values)[1U].as_int();
            if (!index || !size) {
                return fail(ErrorCode::invalid_state,
                            "Enumeration state is invalid");
            }
            return std::optional<Value>(Value::from_int(
                *index < *size ? 1 : 0));
        });
    add(registry, "java/util/ArrayEnumeration", "nextElement",
        "()Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 3> fields {
                kEnumerationIndexField,
                kEnumerationSizeField,
                kEnumerationArrayField,
            };
            auto values = field_values(machine, *object, fields);
            if (!values) return std::unexpected(values.error());
            auto index = (*values)[0U].as_int();
            auto size = (*values)[1U].as_int();
            auto array = (*values)[2U].as_reference();
            if (!index || !size || !array || array->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "Enumeration state is invalid");
            }
            if (*index >= *size) {
                return fail_java("java/util/NoSuchElementException",
                                 "Enumeration has no more elements");
            }
            auto value = machine.heap().element(
                *array, static_cast<usize>(*index));
            if (!value) return std::unexpected(value.error());
            auto updated = set_int_field(machine, *object,
                                         kEnumerationIndexField,
                                         *index + 1);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value>(*value);
        });
}

void register_array_list(NativeMethodRegistry& registry) {
    add(registry, "java/util/ArrayList", "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto initialized = initialize_vector(machine, *object, 10, 0);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/ArrayList", "<init>", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto capacity = int_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!capacity) return std::unexpected(capacity.error());
            auto initialized = initialize_vector(machine, *object, *capacity, 0);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/ArrayList", "size", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto count = int_field(machine, *object, kVectorCountField);
            if (!count) return std::unexpected(count.error());
            return std::optional<Value>(Value::from_int(*count));
        });
    add(registry, "java/util/ArrayList", "isEmpty", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto count = int_field(machine, *object, kVectorCountField);
            if (!count) return std::unexpected(count.error());
            return std::optional<Value>(Value::from_int(*count == 0 ? 1 : 0));
        });

    const auto add_search = [&registry](const char* name, bool reverse) {
        add(registry, "java/util/ArrayList", name,
            "(Ljava/lang/Object;)I",
            [reverse](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                auto target = reference_argument(arguments, 1U, true);
                if (!object) return std::unexpected(object.error());
                if (!target) return std::unexpected(target.error());
                const i32 start = reverse
                    ? std::numeric_limits<i32>::max() : 0;
                auto index = vector_index_of(machine, *object, *target,
                                             start, reverse);
                if (!index) return std::unexpected(index.error());
                return std::optional<Value>(Value::from_int(*index));
            });
    };
    add_search("indexOf", false);
    add_search("lastIndexOf", true);
    add(registry, "java/util/ArrayList", "contains",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto target = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!target) return std::unexpected(target.error());
            auto index = vector_index_of(machine, *object, *target, 0, false);
            if (!index) return std::unexpected(index.error());
            return std::optional<Value>(Value::from_int(*index >= 0 ? 1 : 0));
        });
    add(registry, "java/util/ArrayList", "get",
        "(I)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            auto state = vector_storage_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (*index < 0 || *index >= state->count) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "ArrayList index is out of range");
            }
            auto value = machine.heap().element(
                state->data, static_cast<usize>(*index));
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(*value);
        });
    add(registry, "java/util/ArrayList", "set",
        "(ILjava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            auto replacement = reference_argument(arguments, 2U, true);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            if (!replacement) return std::unexpected(replacement.error());
            auto state = array_list_state(machine, *object, false);
            if (!state) return std::unexpected(state.error());
            if (*index < 0 || *index >= state->count) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "ArrayList index is out of range");
            }
            auto previous = machine.heap().element(
                state->data, static_cast<usize>(*index));
            if (!previous) return std::unexpected(previous.error());
            auto stored = machine.heap().set_element(
                state->data, static_cast<usize>(*index),
                Value::from_reference(*replacement));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(*previous);
        });
    add(registry, "java/util/ArrayList", "add",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto state = array_list_state(machine, *object, true);
            if (!state) return std::unexpected(state.error());
            const VectorCapacityState capacity_state {
                .data = state->data,
                .count = state->count,
                .increment = state->increment,
            };
            auto data = ensure_vector_capacity_from_state(
                machine, *object, capacity_state, state->count + 1);
            if (!data) return std::unexpected(data.error());
            auto stored = machine.heap().set_element(
                *data, static_cast<usize>(state->count),
                Value::from_reference(*value));
            if (!stored) return std::unexpected(stored.error());
            auto updated = set_int_field(machine, *object,
                                         kVectorCountField, state->count + 1);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value>(Value::from_int(1));
        });
    add(registry, "java/util/ArrayList", "add",
        "(ILjava/lang/Object;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            auto value = reference_argument(arguments, 2U, true);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            if (!value) return std::unexpected(value.error());
            auto state = array_list_state(machine, *object, true);
            if (!state) return std::unexpected(state.error());
            if (*index < 0 || *index > state->count) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "ArrayList insertion index is out of range");
            }
            const VectorCapacityState capacity_state {
                .data = state->data,
                .count = state->count,
                .increment = state->increment,
            };
            auto data = ensure_vector_capacity_from_state(
                machine, *object, capacity_state, state->count + 1);
            if (!data) return std::unexpected(data.error());
            const usize shifted = static_cast<usize>(state->count - *index);
            if (shifted != 0U) {
                auto copied = machine.heap().copy_array_range(
                    *data, static_cast<usize>(*index),
                    *data, static_cast<usize>(*index + 1), shifted);
                if (!copied) return std::unexpected(copied.error());
            }
            auto stored = machine.heap().set_element(
                *data, static_cast<usize>(*index),
                Value::from_reference(*value));
            if (!stored) return std::unexpected(stored.error());
            auto updated = set_int_field(machine, *object,
                                         kVectorCountField, state->count + 1);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/ArrayList", "remove",
        "(I)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            auto state = array_list_state(machine, *object, true);
            if (!state) return std::unexpected(state.error());
            if (*index < 0 || *index >= state->count) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "ArrayList index is out of range");
            }
            auto previous = machine.heap().element(
                state->data, static_cast<usize>(*index));
            if (!previous) return std::unexpected(previous.error());
            const VectorStorageState storage_state {
                .data = state->data,
                .count = state->count,
            };
            auto removed = remove_vector_index_from_state(
                machine, *object, storage_state, *index);
            if (!removed) return std::unexpected(removed.error());
            return std::optional<Value>(*previous);
        });
    add(registry, "java/util/ArrayList", "remove",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto target = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!target) return std::unexpected(target.error());
            auto mutable_list = require_array_list_mutation(
                machine, *object, true);
            if (!mutable_list) return std::unexpected(mutable_list.error());
            auto index = vector_index_of(machine, *object, *target, 0, false);
            if (!index) return std::unexpected(index.error());
            if (*index < 0) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto removed = remove_vector_index(machine, *object, *index);
            if (!removed) return std::unexpected(removed.error());
            return std::optional<Value>(Value::from_int(1));
        });
    add(registry, "java/util/ArrayList", "clear", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto state = array_list_state(machine, *object, true);
            if (!state) return std::unexpected(state.error());
            if (state->count != 0) {
                auto cleared = machine.heap().fill_array_range(
                    state->data, 0U, static_cast<usize>(state->count),
                    Value::from_reference({}));
                if (!cleared) return std::unexpected(cleared.error());
            }
            auto updated = set_int_field(machine, *object,
                                         kVectorCountField, 0);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/ArrayList", "sort",
        "(Ljava/util/Comparator;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto comparator = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!comparator) return std::unexpected(comparator.error());
            auto state = array_list_state(machine, *object, false);
            if (!state) return std::unexpected(state.error());
            for (i32 index = 1; index < state->count; ++index) {
                auto current_value = machine.heap().element(
                    state->data, static_cast<usize>(index));
                if (!current_value) {
                    return std::unexpected(current_value.error());
                }
                auto current = current_value->as_reference();
                if (!current) return std::unexpected(current.error());
                i32 position = index;
                while (position > 0) {
                    auto previous_value = machine.heap().element(
                        state->data, static_cast<usize>(position - 1));
                    if (!previous_value) {
                        return std::unexpected(previous_value.error());
                    }
                    auto previous = previous_value->as_reference();
                    if (!previous) return std::unexpected(previous.error());
                    auto comparison = comparator_compare(
                        machine, *comparator, *previous, *current);
                    if (!comparison) {
                        return std::unexpected(comparison.error());
                    }
                    if (*comparison <= 0) break;
                    auto shifted = machine.heap().set_element(
                        state->data, static_cast<usize>(position),
                        Value::from_reference(*previous));
                    if (!shifted) return std::unexpected(shifted.error());
                    --position;
                }
                auto stored = machine.heap().set_element(
                    state->data, static_cast<usize>(position),
                    Value::from_reference(*current));
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "java/util/ArrayList", "equals",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto other = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!other) return std::unexpected(other.error());
            if (*object == *other) {
                return std::optional<Value>(Value::from_int(1));
            }
            if (other->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }

            auto other_class = machine.heap().class_name(*other);
            if (!other_class) return std::unexpected(other_class.error());
            auto other_size = machine.invoke_instance(
                *other, *other_class, "size", "()I", {});
            if (!other_size || other_size->throwable.has_value() ||
                !other_size->return_value.has_value()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto other_count = other_size->return_value->as_int();
            auto state = vector_storage_state(machine, *object);
            if (!other_count || !state) {
                return std::optional<Value>(Value::from_int(0));
            }
            if (*other_count != state->count) {
                return std::optional<Value>(Value::from_int(0));
            }

            for (i32 index = 0; index < state->count; ++index) {
                auto left = machine.heap().element(
                    state->data, static_cast<usize>(index));
                if (!left) return std::unexpected(left.error());
                const Value index_value = Value::from_int(index);
                auto right_result = machine.invoke_instance(
                    *other, *other_class, "get", "(I)Ljava/lang/Object;",
                    std::span<const Value>(&index_value, 1U));
                if (!right_result || right_result->throwable.has_value() ||
                    !right_result->return_value.has_value()) {
                    return std::optional<Value>(Value::from_int(0));
                }
                auto left_ref = left->as_reference();
                auto right_ref = right_result->return_value->as_reference();
                if (!left_ref || !right_ref) {
                    return std::optional<Value>(Value::from_int(0));
                }
                auto equal = values_equal(machine, *left_ref, *right_ref);
                if (!equal) return std::unexpected(equal.error());
                if (!*equal) {
                    return std::optional<Value>(Value::from_int(0));
                }
            }
            return std::optional<Value>(Value::from_int(1));
        });
    add(registry, "java/util/ArrayList", "toString",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto state = vector_storage_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            std::u16string text(u"[");
            for (i32 index = 0; index < state->count; ++index) {
                if (index != 0) text.append(u", ");
                auto value = machine.heap().element(
                    state->data, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto reference = value->as_reference();
                if (!reference) return std::unexpected(reference.error());
                auto value_text = collection_value_text(
                    machine, *reference, *object, u"(this Collection)");
                if (!value_text) return std::unexpected(value_text.error());
                text.append(*value_text);
            }
            text.push_back(u']');
            auto string = create_string(machine, std::move(text));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
}

void register_vector(NativeMethodRegistry& registry) {
    add(registry, "java/util/Vector", "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto initialized = initialize_vector(machine, *object, 10, 0);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Vector", "<init>", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto capacity = arguments[1].as_int();
            if (!object) return std::unexpected(object.error());
            if (!capacity) return std::unexpected(capacity.error());
            auto initialized = initialize_vector(machine, *object,
                                                 *capacity, 0);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Vector", "<init>", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto capacity = arguments[1].as_int();
            auto increment = arguments[2].as_int();
            if (!object) return std::unexpected(object.error());
            if (!capacity || !increment)
                return fail(ErrorCode::invalid_argument,
                            "Vector constructor arguments are invalid");
            auto initialized = initialize_vector(machine, *object,
                                                 *capacity, *increment);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Stack", "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto initialized = initialize_vector(machine, *object, 10, 0);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });

    add(registry, "java/util/Vector", "size", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto count = int_field(machine, *object, kVectorCountField);
            if (!count) return std::unexpected(count.error());
            return std::optional<Value>(Value::from_int(*count));
        }, NativeJitPolicy::synchronous_bounded);
    add(registry, "java/util/Vector", "capacity", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto data = vector_data(machine, *object);
            if (!data) return std::unexpected(data.error());
            auto length = machine.heap().array_length(*data);
            if (!length) return std::unexpected(length.error());
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(*length)));
        }, NativeJitPolicy::synchronous_bounded);
    add(registry, "java/util/Vector", "ensureCapacity", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto minimum = int_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!minimum) return std::unexpected(minimum.error());
            if (*minimum > 0) {
                auto ensured = ensure_vector_capacity(
                    machine, *object, *minimum);
                if (!ensured) return std::unexpected(ensured.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "java/util/Vector", "trimToSize", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto state = vector_storage_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            auto capacity = machine.heap().array_length(state->data);
            if (!capacity) return std::unexpected(capacity.error());
            if (*capacity == static_cast<usize>(state->count)) {
                return std::optional<Value> {};
            }
            auto replacement = allocate_object_array(
                machine, static_cast<usize>(state->count));
            if (!replacement) return std::unexpected(replacement.error());
            if (state->count != 0) {
                auto copied = machine.heap().copy_array_range(
                    state->data, 0U, *replacement, 0U,
                    static_cast<usize>(state->count));
                if (!copied) return std::unexpected(copied.error());
            }
            auto stored = set_reference_field(
                machine, *object, kVectorDataField, *replacement);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Vector", "setSize", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto requested = int_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!requested) return std::unexpected(requested.error());
            if (*requested < 0) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "Vector size is negative");
            }
            auto state = vector_capacity_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (*requested > state->count) {
                auto data = ensure_vector_capacity_from_state(
                    machine, *object, *state, *requested);
                if (!data) return std::unexpected(data.error());
            } else if (*requested < state->count) {
                auto cleared = machine.heap().fill_array_range(
                    state->data, static_cast<usize>(*requested),
                    static_cast<usize>(state->count - *requested),
                    Value::from_reference({}));
                if (!cleared) return std::unexpected(cleared.error());
            }
            auto updated = set_int_field(
                machine, *object, kVectorCountField, *requested);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Vector", "isEmpty", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto count = int_field(machine, *object, kVectorCountField);
            if (!count) return std::unexpected(count.error());
            return std::optional<Value>(Value::from_int(*count == 0 ? 1 : 0));
        }, NativeJitPolicy::synchronous_bounded);
    add(registry, "java/util/Vector", "copyInto",
        "([Ljava/lang/Object;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "Vector.copyInto destination is missing");
            }
            auto destination = arguments[1].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!destination || destination->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Vector.copyInto destination is null");
            }
            auto destination_class = machine.heap().class_name(*destination);
            auto destination_length = machine.heap().array_length(*destination);
            auto state = vector_storage_state(machine, *object);
            if (!destination_class || !destination_length || !state) {
                return fail(ErrorCode::invalid_state,
                            "Vector.copyInto state is invalid");
            }
            if (!destination_class->starts_with("[L") &&
                !destination_class->starts_with("[[")) {
                return fail_java("java/lang/ArrayStoreException",
                                 "Vector.copyInto requires a reference array");
            }
            if (*destination_length < static_cast<usize>(state->count)) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "Vector.copyInto destination is too small");
            }
            if (state->count != 0) {
                auto copied = machine.heap().copy_array_range(
                    state->data, 0U, *destination, 0U,
                    static_cast<usize>(state->count));
                if (!copied) return std::unexpected(copied.error());
            }
            return std::optional<Value> {};
        });

    const auto add_index_search = [&registry](const char* name,
                                              bool reverse,
                                              bool with_start) {
        add(registry, "java/util/Vector", name,
            with_start ? "(Ljava/lang/Object;I)I"
                       : "(Ljava/lang/Object;)I",
            [reverse, with_start](Machine& machine,
                                  std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                auto target = arguments[1].as_reference();
                if (!object) return std::unexpected(object.error());
                if (!target) return std::unexpected(target.error());
                i32 start = reverse ? std::numeric_limits<i32>::max() : 0;
                if (with_start) {
                    auto parsed = arguments[2].as_int();
                    if (!parsed) return std::unexpected(parsed.error());
                    start = *parsed;
                }
                auto index = vector_index_of(machine, *object, *target,
                                             start, reverse);
                if (!index) return std::unexpected(index.error());
                return std::optional<Value>(Value::from_int(*index));
            });
    };
    add_index_search("indexOf", false, false);
    add_index_search("indexOf", false, true);
    add_index_search("lastIndexOf", true, false);
    add_index_search("lastIndexOf", true, true);
    add(registry, "java/util/Vector", "contains",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto target = arguments[1].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!target) return std::unexpected(target.error());
            auto index = vector_index_of(machine, *object, *target, 0, false);
            if (!index) return std::unexpected(index.error());
            return std::optional<Value>(Value::from_int(*index >= 0 ? 1 : 0));
        });

    add(registry, "java/util/Vector", "elementAt",
        "(I)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = arguments[1].as_int();
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            auto state = vector_storage_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (*index < 0 || *index >= state->count) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "Vector index is out of range");
            }
            auto value = machine.heap().element(
                state->data, static_cast<usize>(*index));
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(*value);
        }, NativeJitPolicy::synchronous_bounded);
    const auto add_edge = [&registry](const char* name, bool last) {
        add(registry, "java/util/Vector", name, "()Ljava/lang/Object;",
            [last](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                auto state = vector_storage_state(machine, *object);
                if (!state) return std::unexpected(state.error());
                if (state->count == 0) {
                    return fail_java("java/util/NoSuchElementException",
                                     "Vector is empty");
                }
                const usize index = last
                    ? static_cast<usize>(state->count - 1) : 0U;
                auto value = machine.heap().element(state->data, index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(*value);
            });
    };
    add_edge("firstElement", false);
    add_edge("lastElement", true);

    add(registry, "java/util/Vector", "setElementAt",
        "(Ljava/lang/Object;I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = arguments[1].as_reference();
            auto index = arguments[2].as_int();
            if (!object || !value || !index)
                return fail(ErrorCode::invalid_argument,
                            "Vector.setElementAt arguments are invalid");
            auto state = vector_storage_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (*index < 0 || *index >= state->count) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "Vector index is out of range");
            }
            auto stored = machine.heap().set_element(
                state->data, static_cast<usize>(*index),
                Value::from_reference(*value));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Vector", "removeElementAt", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = arguments[1].as_int();
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            auto removed = remove_vector_index(machine, *object, *index);
            if (!removed) return std::unexpected(removed.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Vector", "insertElementAt",
        "(Ljava/lang/Object;I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = arguments[1].as_reference();
            auto index = arguments[2].as_int();
            if (!object || !value || !index)
                return fail(ErrorCode::invalid_argument,
                            "Vector.insertElementAt arguments are invalid");
            auto state = vector_capacity_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (*index < 0 || *index > state->count) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "Vector insertion index is out of range");
            }
            auto data = ensure_vector_capacity_from_state(
                machine, *object, *state, state->count + 1);
            if (!data) return std::unexpected(data.error());
            const usize shifted = static_cast<usize>(state->count - *index);
            if (shifted != 0U) {
                auto copied = machine.heap().copy_array_range(
                    *data, static_cast<usize>(*index),
                    *data, static_cast<usize>(*index + 1), shifted);
                if (!copied) return std::unexpected(copied.error());
            }
            auto stored = machine.heap().set_element(
                *data, static_cast<usize>(*index),
                Value::from_reference(*value));
            if (!stored) return std::unexpected(stored.error());
            auto updated = set_int_field(machine, *object,
                                         kVectorCountField, state->count + 1);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Vector", "addElement",
        "(Ljava/lang/Object;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = arguments[1].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto state = vector_capacity_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            auto data = ensure_vector_capacity_from_state(
                machine, *object, *state, state->count + 1);
            if (!data) return std::unexpected(data.error());
            auto stored = machine.heap().set_element(
                *data, static_cast<usize>(state->count),
                Value::from_reference(*value));
            if (!stored) return std::unexpected(stored.error());
            auto updated = set_int_field(machine, *object,
                                         kVectorCountField, state->count + 1);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Vector", "removeElement",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto target = arguments[1].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!target) return std::unexpected(target.error());
            auto index = vector_index_of(machine, *object, *target, 0, false);
            if (!index) return std::unexpected(index.error());
            if (*index < 0) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto removed = remove_vector_index(machine, *object, *index);
            if (!removed) return std::unexpected(removed.error());
            return std::optional<Value>(Value::from_int(1));
        });
    add(registry, "java/util/Vector", "removeAllElements", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto state = vector_storage_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (state->count != 0) {
                auto cleared = machine.heap().fill_array_range(
                    state->data, 0U, static_cast<usize>(state->count),
                    Value::from_reference({}));
                if (!cleared) return std::unexpected(cleared.error());
            }
            auto updated = set_int_field(machine, *object,
                                         kVectorCountField, 0);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Vector", "elements",
        "()Ljava/util/Enumeration;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto state = vector_storage_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            auto enumeration = make_enumeration_from_array(
                machine, state->data, state->count);
            if (!enumeration) return std::unexpected(enumeration.error());
            return std::optional<Value>(Value::from_reference(*enumeration));
        });
    add(registry, "java/util/Vector", "toString", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto state = vector_storage_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            std::u16string text(u"[");
            for (i32 index = 0; index < state->count; ++index) {
                if (index != 0) text.append(u", ");
                auto value = machine.heap().element(
                    state->data, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto reference = value->as_reference();
                if (!reference) return std::unexpected(reference.error());
                auto value_text = collection_value_text(
                    machine, *reference, *object, u"(this Collection)");
                if (!value_text) return std::unexpected(value_text.error());
                text.append(*value_text);
            }
            text.push_back(u']');
            auto string = create_string(machine, std::move(text));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });

    add(registry, "java/util/Stack", "push",
        "(Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = arguments[1].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto state = vector_capacity_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            auto data = ensure_vector_capacity_from_state(
                machine, *object, *state, state->count + 1);
            if (!data) return std::unexpected(data.error());
            auto stored = machine.heap().set_element(
                *data, static_cast<usize>(state->count),
                Value::from_reference(*value));
            if (!stored) return std::unexpected(stored.error());
            auto updated = set_int_field(machine, *object,
                                         kVectorCountField, state->count + 1);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value>(Value::from_reference(*value));
        });
    const auto stack_edge = [&registry](const char* name, bool remove) {
        add(registry, "java/util/Stack", name, "()Ljava/lang/Object;",
            [remove](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                auto state = vector_storage_state(machine, *object);
                if (!state) return std::unexpected(state.error());
                if (state->count == 0) {
                    return fail_java("java/util/EmptyStackException",
                                     "Stack is empty");
                }
                const usize index = static_cast<usize>(state->count - 1);
                auto value = machine.heap().element(state->data, index);
                if (!value) return std::unexpected(value.error());
                if (remove) {
                    auto cleared = machine.heap().set_element(
                        state->data, index, Value::from_reference({}));
                    auto updated = set_int_field(machine, *object,
                                                 kVectorCountField,
                                                 state->count - 1);
                    if (!cleared) return std::unexpected(cleared.error());
                    if (!updated) return std::unexpected(updated.error());
                }
                return std::optional<Value>(*value);
            });
    };
    stack_edge("pop", true);
    stack_edge("peek", false);
    add(registry, "java/util/Stack", "empty", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto count = int_field(machine, *object, kVectorCountField);
            if (!count) return std::unexpected(count.error());
            return std::optional<Value>(Value::from_int(*count == 0 ? 1 : 0));
        });
    add(registry, "java/util/Stack", "search",
        "(Ljava/lang/Object;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto target = arguments[1].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!target) return std::unexpected(target.error());
            auto index = vector_index_of(machine, *object, *target,
                                         std::numeric_limits<i32>::max(),
                                         true);
            if (!index) return std::unexpected(index.error());
            if (*index < 0) return std::optional<Value>(Value::from_int(-1));
            auto count = int_field(machine, *object, kVectorCountField);
            if (!count) return std::unexpected(count.error());
            return std::optional<Value>(Value::from_int(*count - *index));
        });
}

[[nodiscard]] Result<usize> hashtable_bucket_capacity(
    usize entry_capacity) {
    if (entry_capacity >
        (std::numeric_limits<usize>::max() - 1U) / 2U) {
        return fail(ErrorCode::overflow,
                    "Hashtable bucket capacity overflowed");
    }
    return std::max<usize>(3U, entry_capacity * 2U + 1U);
}

[[nodiscard]] Result<ObjectRef> allocate_int_array(Machine& machine,
                                                   usize length) {
    return machine.heap().allocate_array(
        "[I", length, Value::from_int(0));
}

[[nodiscard]] i32 hashtable_threshold(i32 capacity,
                                      float load_factor) noexcept {
    const double product = static_cast<double>(capacity) *
                           static_cast<double>(load_factor);
    if (product >= static_cast<double>(std::numeric_limits<i32>::max())) {
        return std::numeric_limits<i32>::max();
    }
    return static_cast<i32>(product);
}

[[nodiscard]] Result<std::vector<i32>> read_hashtable_iteration_order(
    Machine& machine,
    ObjectRef order,
    i32 count) {
    if (order.is_null() || count < 0) {
        return fail(ErrorCode::invalid_state,
                    "Hashtable iteration order is invalid");
    }
    auto result = machine.heap().read_int_array(order);
    if (!result) return std::unexpected(result.error());
    if (static_cast<usize>(count) > result->size()) {
        return fail(ErrorCode::invalid_state,
                    "Hashtable iteration order is shorter than its count");
    }
    result->resize(static_cast<usize>(count));
    return result;
}

[[nodiscard]] Status write_hashtable_iteration_order(
    Machine& machine,
    ObjectRef array,
    std::span<const i32> order) {
    if (array.is_null()) {
        return fail(ErrorCode::invalid_state,
                    "Hashtable iteration order storage is invalid");
    }
    auto length = machine.heap().array_length(array);
    if (!length) return std::unexpected(length.error());
    if (order.size() > *length) {
        return fail(ErrorCode::overflow,
                    "Hashtable iteration order exceeds capacity");
    }
    std::vector<i32> values(*length, 0);
    std::copy(order.begin(), order.end(), values.begin());
    return machine.heap().write_int_array(array, 0U, values);
}

[[nodiscard]] Status store_hashtable_iteration_order(
    Machine& machine,
    ObjectRef table,
    std::span<const i32> order) {
    auto array = reference_field(machine, table,
                                 kHashtableIterationOrderField);
    if (!array) return std::unexpected(array.error());
    return write_hashtable_iteration_order(machine, *array, order);
}

[[nodiscard]] usize java_hashtable_bucket_unchecked(i32 hash,
                                                     i32 capacity) noexcept {
    return static_cast<usize>(
        (static_cast<u32>(hash) & 0x7fffffffU) %
        static_cast<u32>(capacity));
}

[[nodiscard]] Result<std::vector<i32>> rehashed_iteration_order(
    Machine& machine,
    ObjectRef table,
    i32 new_capacity) {
    constexpr std::array<usize, 3> fields {
        kHashtableCountField,
        kHashtableIterationOrderField,
        kHashtableHashesField,
    };
    auto state = field_values(machine, table, fields);
    if (!state) return std::unexpected(state.error());
    auto count = (*state)[0U].as_int();
    auto order_array = (*state)[1U].as_reference();
    auto hashes = (*state)[2U].as_reference();
    if (!count || !order_array || !hashes || hashes->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "Hashtable hashes are invalid");
    }
    auto order = read_hashtable_iteration_order(
        machine, *order_array, *count);
    if (!order) return std::unexpected(order.error());
    auto hash_values = machine.heap().read_int_array(*hashes);
    if (!hash_values) return std::unexpected(hash_values.error());
    std::vector<std::vector<i32>> buckets(
        static_cast<usize>(new_capacity));
    for (const i32 entry : *order) {
        if (entry < 0 || static_cast<usize>(entry) >= hash_values->size()) {
            return fail(ErrorCode::invalid_state,
                        "Hashtable iteration order references an invalid hash");
        }
        const i32 hash = (*hash_values)[static_cast<usize>(entry)];
        auto& bucket = buckets[java_hashtable_bucket_unchecked(
            hash, new_capacity)];
        bucket.insert(bucket.begin(), entry);
    }
    std::vector<i32> result;
    result.reserve(order->size());
    for (usize bucket = buckets.size(); bucket > 0U; --bucket) {
        result.insert(result.end(), buckets[bucket - 1U].begin(),
                      buckets[bucket - 1U].end());
    }
    return result;
}

[[nodiscard]] Result<std::vector<i32>> iteration_order_with_insert(
    Machine& machine,
    ObjectRef table,
    i32 entry_index,
    i32 hash) {
    constexpr std::array<usize, 4> fields {
        kHashtableCountField,
        kHashtableIterationOrderField,
        kHashtableHashesField,
        kHashtableTableCapacityField,
    };
    auto state = field_values(machine, table, fields);
    if (!state) return std::unexpected(state.error());
    auto count = (*state)[0U].as_int();
    auto order_array = (*state)[1U].as_reference();
    auto hashes = (*state)[2U].as_reference();
    auto capacity = (*state)[3U].as_int();
    if (!count || !order_array || !hashes || hashes->is_null() ||
        !capacity || *capacity <= 0) {
        return fail(ErrorCode::invalid_state,
                    "Hashtable iteration state is invalid");
    }
    auto order = read_hashtable_iteration_order(
        machine, *order_array, *count);
    if (!order) return std::unexpected(order.error());
    auto hash_values = machine.heap().read_int_array(*hashes);
    if (!hash_values) return std::unexpected(hash_values.error());
    const usize target_bucket = java_hashtable_bucket_unchecked(
        hash, *capacity);
    auto position = order->end();
    for (auto iterator = order->begin(); iterator != order->end(); ++iterator) {
        if (*iterator < 0 ||
            static_cast<usize>(*iterator) >= hash_values->size()) {
            return fail(ErrorCode::invalid_state,
                        "Hashtable iteration order references an invalid hash");
        }
        const i32 existing_hash =
            (*hash_values)[static_cast<usize>(*iterator)];
        const usize existing_bucket = java_hashtable_bucket_unchecked(
            existing_hash, *capacity);
        if (existing_bucket <= target_bucket) {
            position = iterator;
            break;
        }
    }
    order->insert(position, entry_index);
    return *order;
}

[[nodiscard]] Status initialize_hashtable(Machine& machine,
                                          ObjectRef object,
                                          i32 capacity,
                                          float load_factor) {
    if (capacity < 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         "Hashtable capacity is negative");
    }
    const usize actual = capacity == 0 ? 1U : static_cast<usize>(capacity);
    auto bucket_capacity = hashtable_bucket_capacity(actual);
    if (!bucket_capacity) return std::unexpected(bucket_capacity.error());
    // Keep the Java-visible sizing semantics, but do not allocate five backing
    // arrays for an empty table. Legacy games create large numbers of tiny,
    // short-lived Hashtables as scratch objects; eager storage was responsible
    // for tens of thousands of int[]/Object[] allocations per few seconds.
    // Storage is materialized on the first mutation instead.
    (void)*bucket_capacity;
    const i32 actual_capacity = static_cast<i32>(actual);
    constexpr std::array<usize, 9> fields {
        kHashtableKeysField,
        kHashtableValuesField,
        kHashtableHashesField,
        kHashtableBucketsField,
        kHashtableIterationOrderField,
        kHashtableTableCapacityField,
        kHashtableThresholdField,
        kHashtableLoadFactorField,
        kHashtableCountField,
    };
    const std::array<Value, 9> state {
        Value::from_reference({}),
        Value::from_reference({}),
        Value::from_reference({}),
        Value::from_reference({}),
        Value::from_reference({}),
        Value::from_int(actual_capacity),
        Value::from_int(hashtable_threshold(actual_capacity, load_factor)),
        Value::from_float(load_factor),
        Value::from_int(0),
    };
    return set_field_values(machine, object, fields, state);
}

[[nodiscard]] Status materialize_hashtable_storage(Machine& machine,
                                                   ObjectRef object) {
    constexpr std::array<usize, 2> fields {
        kHashtableKeysField,
        kHashtableTableCapacityField,
    };
    auto state = field_values(machine, object, fields);
    if (!state) return std::unexpected(state.error());
    auto keys = (*state)[0U].as_reference();
    auto capacity = (*state)[1U].as_int();
    if (!keys || !capacity || *capacity <= 0) {
        return fail(ErrorCode::invalid_state,
                    "Hashtable sizing state is invalid");
    }
    if (!keys->is_null()) return {};

    const usize actual = static_cast<usize>(*capacity);
    auto bucket_capacity = hashtable_bucket_capacity(actual);
    if (!bucket_capacity) return std::unexpected(bucket_capacity.error());
    auto new_keys = allocate_object_array(machine, actual);
    auto values = allocate_object_array(machine, actual);
    auto hashes = allocate_int_array(machine, actual);
    auto buckets = allocate_int_array(machine, *bucket_capacity);
    auto order = allocate_int_array(machine, actual);
    if (!new_keys) return std::unexpected(new_keys.error());
    if (!values) return std::unexpected(values.error());
    if (!hashes) return std::unexpected(hashes.error());
    if (!buckets) return std::unexpected(buckets.error());
    if (!order) return std::unexpected(order.error());

    constexpr std::array<usize, 5> storage_fields {
        kHashtableKeysField,
        kHashtableValuesField,
        kHashtableHashesField,
        kHashtableBucketsField,
        kHashtableIterationOrderField,
    };
    const std::array<Value, 5> storage {
        Value::from_reference(*new_keys),
        Value::from_reference(*values),
        Value::from_reference(*hashes),
        Value::from_reference(*buckets),
        Value::from_reference(*order),
    };
    return set_field_values(machine, object, storage_fields, storage);
}

[[nodiscard]] Status hashtable_insert_bucket(Machine& machine,
                                                ObjectRef buckets,
                                                i32 hash,
                                                i32 entry_index) {
    auto bucket_count = machine.heap().array_length(buckets);
    if (!bucket_count) return std::unexpected(bucket_count.error());
    if (*bucket_count == 0U) {
        return fail(ErrorCode::invalid_state,
                    "Hashtable has no hash buckets");
    }
    usize bucket = static_cast<usize>(
        static_cast<u32>(hash) & 0x7fffffffU) % *bucket_count;
    for (usize probe = 0; probe < *bucket_count; ++probe) {
        auto marker = machine.heap().element(buckets, bucket);
        if (!marker) return std::unexpected(marker.error());
        auto stored_index = marker->as_int();
        if (!stored_index) return std::unexpected(stored_index.error());
        if (*stored_index == 0) {
            return machine.heap().set_element(
                buckets, bucket, Value::from_int(entry_index + 1));
        }
        bucket = (bucket + 1U) % *bucket_count;
    }
    return fail(ErrorCode::overflow,
                "Hashtable hash buckets are full");
}

[[nodiscard]] Status rebuild_hashtable_buckets(Machine& machine,
                                                ObjectRef table) {
    constexpr std::array<usize, 3> fields {
        kHashtableKeysField,
        kHashtableHashesField,
        kHashtableCountField,
    };
    auto state = field_values(machine, table, fields);
    if (!state) return std::unexpected(state.error());
    auto keys = (*state)[0U].as_reference();
    auto hashes = (*state)[1U].as_reference();
    auto count = (*state)[2U].as_int();
    if (!keys || !hashes || !count || keys->is_null() || hashes->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "Hashtable state is invalid");
    }
    auto entry_capacity = machine.heap().array_length(*keys);
    if (!entry_capacity) return std::unexpected(entry_capacity.error());
    auto bucket_capacity = hashtable_bucket_capacity(*entry_capacity);
    if (!bucket_capacity) return std::unexpected(bucket_capacity.error());
    auto buckets = allocate_int_array(machine, *bucket_capacity);
    if (!buckets) return std::unexpected(buckets.error());
    auto hash_values = machine.heap().read_int_array(*hashes);
    if (!hash_values) return std::unexpected(hash_values.error());
    if (*count < 0 || static_cast<usize>(*count) > hash_values->size()) {
        return fail(ErrorCode::invalid_state,
                    "Hashtable hash storage is shorter than its count");
    }
    std::vector<i32> bucket_values(*bucket_capacity, 0);
    for (i32 index = 0; index < *count; ++index) {
        const i32 hash = (*hash_values)[static_cast<usize>(index)];
        usize bucket = static_cast<usize>(
            static_cast<u32>(hash) & 0x7fffffffU) % *bucket_capacity;
        bool inserted = false;
        for (usize probe = 0; probe < *bucket_capacity; ++probe) {
            if (bucket_values[bucket] == 0) {
                bucket_values[bucket] = index + 1;
                inserted = true;
                break;
            }
            bucket = (bucket + 1U) % *bucket_capacity;
        }
        if (!inserted) {
            return fail(ErrorCode::overflow,
                        "Hashtable hash buckets are full");
        }
    }
    auto written = machine.heap().write_int_array(*buckets, 0U, bucket_values);
    if (!written) return written;
    return set_reference_field(machine, table,
                               kHashtableBucketsField, *buckets);
}

[[nodiscard]] Result<i32> hashtable_find(
    Machine& machine,
    ObjectRef table,
    ObjectRef key,
    bool search_values,
    std::optional<i32> known_hash = std::nullopt) {
    if (search_values) {
        constexpr std::array<usize, 2> fields {
            kHashtableCountField,
            kHashtableValuesField,
        };
        auto state = field_values(machine, table, fields);
        if (!state) return std::unexpected(state.error());
        auto count = (*state)[0U].as_int();
        auto values = (*state)[1U].as_reference();
        if (!count || !values) {
            return fail(ErrorCode::invalid_state,
                        "Hashtable values state is invalid");
        }
        if (*count == 0) return -1;
        if (values->is_null()) {
            return fail(ErrorCode::invalid_state,
                        "Hashtable values storage is missing");
        }
        for (i32 index = 0; index < *count; ++index) {
            auto value = machine.heap().element(
                *values, static_cast<usize>(index));
            if (!value) return std::unexpected(value.error());
            auto reference = value->as_reference();
            if (!reference) return std::unexpected(reference.error());
            auto equal = values_equal(machine, *reference, key);
            if (!equal) return std::unexpected(equal.error());
            if (*equal) return index;
        }
        return -1;
    }

    constexpr std::array<usize, 4> fields {
        kHashtableCountField,
        kHashtableKeysField,
        kHashtableHashesField,
        kHashtableBucketsField,
    };
    auto state = field_values(machine, table, fields);
    if (!state) return std::unexpected(state.error());
    auto count = (*state)[0U].as_int();
    auto keys = (*state)[1U].as_reference();
    auto hashes = (*state)[2U].as_reference();
    auto buckets = (*state)[3U].as_reference();
    if (!count || !keys || !hashes || !buckets) {
        return fail(ErrorCode::invalid_state,
                    "Hashtable key index is missing");
    }
    auto hash = known_hash.has_value()
        ? Result<i32>(*known_hash)
        : object_hash(machine, key);
    if (!hash) return std::unexpected(hash.error());
    if (*count == 0) return -1;
    if (keys->is_null() || hashes->is_null() || buckets->is_null() ||
        *count < 0) {
        return fail(ErrorCode::invalid_state,
                    "Hashtable key index state is invalid");
    }
    auto hash_values = machine.heap().read_int_array(*hashes);
    auto bucket_values = machine.heap().read_int_array(*buckets);
    if (!hash_values) return std::unexpected(hash_values.error());
    if (!bucket_values) return std::unexpected(bucket_values.error());
    if (static_cast<usize>(*count) > hash_values->size()) {
        return fail(ErrorCode::invalid_state,
                    "Hashtable hash storage is shorter than its count");
    }
    if (bucket_values->empty()) return -1;

    usize bucket = static_cast<usize>(
        static_cast<u32>(*hash) & 0x7fffffffU) % bucket_values->size();
    for (usize probe = 0; probe < bucket_values->size(); ++probe) {
        const i32 marker = (*bucket_values)[bucket];
        if (marker == 0) return -1;
        const i32 index = marker - 1;
        if (index < 0 || index >= *count) {
            return fail(ErrorCode::invalid_state,
                        "Hashtable bucket references an invalid entry");
        }
        if ((*hash_values)[static_cast<usize>(index)] == *hash) {
            auto value = machine.heap().element(
                *keys, static_cast<usize>(index));
            if (!value) return std::unexpected(value.error());
            auto reference = value->as_reference();
            if (!reference) return std::unexpected(reference.error());
            auto equal = values_equal(machine, *reference, key);
            if (!equal) return std::unexpected(equal.error());
            if (*equal) return index;
        }
        bucket = (bucket + 1U) % bucket_values->size();
    }
    return -1;
}

[[nodiscard]] Status ensure_hashtable_capacity(Machine& machine,
                                               ObjectRef table,
                                               i32 minimum) {
    auto materialized = materialize_hashtable_storage(machine, table);
    if (!materialized) return materialized;
    constexpr std::array<usize, 5> state_fields {
        kHashtableKeysField,
        kHashtableValuesField,
        kHashtableHashesField,
        kHashtableCountField,
        kHashtableLoadFactorField,
    };
    auto state = field_values(machine, table, state_fields);
    if (!state) return std::unexpected(state.error());
    auto keys = (*state)[0U].as_reference();
    auto values = (*state)[1U].as_reference();
    auto hashes = (*state)[2U].as_reference();
    auto count = (*state)[3U].as_int();
    auto factor = (*state)[4U].as_float();
    if (!keys || !values || !hashes || !count || !factor)
        return fail(ErrorCode::invalid_state,
                    "Hashtable arrays are missing");
    auto capacity = machine.heap().array_length(*keys);
    if (!capacity) return std::unexpected(capacity.error());
    if (minimum <= static_cast<i32>(*capacity)) return {};
    if (*capacity > (std::numeric_limits<usize>::max() - 1U) / 2U) {
        return fail(ErrorCode::overflow,
                    "Hashtable entry capacity overflowed");
    }
    usize new_capacity = *capacity * 2U + 1U;
    if (new_capacity < static_cast<usize>(minimum))
        new_capacity = static_cast<usize>(minimum);
    auto new_order_values = rehashed_iteration_order(
        machine, table, static_cast<i32>(new_capacity));
    if (!new_order_values) return std::unexpected(new_order_values.error());
    auto new_keys = allocate_object_array(machine, new_capacity);
    auto new_values = allocate_object_array(machine, new_capacity);
    auto new_hashes = allocate_int_array(machine, new_capacity);
    auto new_order = allocate_int_array(machine, new_capacity);
    if (!new_keys) return std::unexpected(new_keys.error());
    if (!new_values) return std::unexpected(new_values.error());
    if (!new_hashes) return std::unexpected(new_hashes.error());
    if (!new_order) return std::unexpected(new_order.error());
    if (*count != 0) {
        const usize live = static_cast<usize>(*count);
        auto keys_copied = machine.heap().copy_array_range(
            *keys, 0U, *new_keys, 0U, live);
        auto values_copied = machine.heap().copy_array_range(
            *values, 0U, *new_values, 0U, live);
        if (!keys_copied) return keys_copied;
        if (!values_copied) return values_copied;
        auto hash_values = machine.heap().read_int_array(*hashes);
        if (!hash_values) return std::unexpected(hash_values.error());
        if (live > hash_values->size()) {
            return fail(ErrorCode::invalid_state,
                        "Hashtable hashes are shorter than its count");
        }
        auto hashes_written = machine.heap().write_int_array(
            *new_hashes, 0U,
            std::span<const i32>(hash_values->data(), live));
        if (!hashes_written) return hashes_written;
    }
    if (!new_order_values->empty()) {
        auto order_written = machine.heap().write_int_array(
            *new_order, 0U, *new_order_values);
        if (!order_written) return order_written;
    }
    constexpr std::array<usize, 6> update_fields {
        kHashtableKeysField,
        kHashtableValuesField,
        kHashtableHashesField,
        kHashtableIterationOrderField,
        kHashtableTableCapacityField,
        kHashtableThresholdField,
    };
    const std::array<Value, 6> update_values {
        Value::from_reference(*new_keys),
        Value::from_reference(*new_values),
        Value::from_reference(*new_hashes),
        Value::from_reference(*new_order),
        Value::from_int(static_cast<i32>(new_capacity)),
        Value::from_int(hashtable_threshold(
            static_cast<i32>(new_capacity), *factor)),
    };
    auto updated = set_field_values(
        machine, table, update_fields, update_values);
    if (!updated) return updated;
    return rebuild_hashtable_buckets(machine, table);
}

void register_hashtable(NativeMethodRegistry& registry) {
    add(registry, "java/util/Hashtable", "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto initialized = initialize_hashtable(
                machine, *object, 11, 0.75F);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Hashtable", "<init>", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto capacity = arguments[1].as_int();
            if (!object) return std::unexpected(object.error());
            if (!capacity) return std::unexpected(capacity.error());
            auto initialized = initialize_hashtable(
                machine, *object, *capacity, 0.75F);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Hashtable", "<init>", "(IF)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto capacity = arguments[1].as_int();
            auto load_factor = arguments[2].as_float();
            if (!object) return std::unexpected(object.error());
            if (!capacity || !load_factor)
                return fail(ErrorCode::invalid_argument,
                            "Hashtable constructor arguments are invalid");
            if (*load_factor <= 0.0F || std::isnan(*load_factor)) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Hashtable load factor is invalid");
            }
            auto initialized = initialize_hashtable(
                machine, *object, *capacity, *load_factor);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Hashtable", "size", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto count = int_field(machine, *object, kHashtableCountField);
            if (!count) return std::unexpected(count.error());
            return std::optional<Value>(Value::from_int(*count));
        });
    add(registry, "java/util/Hashtable", "isEmpty", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto count = int_field(machine, *object, kHashtableCountField);
            if (!count) return std::unexpected(count.error());
            return std::optional<Value>(Value::from_int(*count == 0 ? 1 : 0));
        });
    const auto contains = [&registry](const char* name, bool values) {
        add(registry, "java/util/Hashtable", name,
            "(Ljava/lang/Object;)Z",
            [values](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                auto target = arguments[1].as_reference();
                if (!object) return std::unexpected(object.error());
                if (!target || target->is_null()) {
                    return fail_java("java/lang/NullPointerException",
                                     "Hashtable lookup key/value is null");
                }
                auto index = hashtable_find(machine, *object, *target, values);
                if (!index) return std::unexpected(index.error());
                return std::optional<Value>(Value::from_int(*index >= 0 ? 1 : 0));
            });
    };
    contains("containsKey", false);
    contains("contains", true);
    add(registry, "java/util/Hashtable", "get",
        "(Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto key = arguments[1].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!key || key->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Hashtable key is null");
            }
            auto index = hashtable_find(machine, *object, *key, false);
            if (!index) return std::unexpected(index.error());
            if (*index < 0)
                return std::optional<Value>(Value::from_reference({}));
            auto values = reference_field(machine, *object,
                                          kHashtableValuesField);
            if (!values) return std::unexpected(values.error());
            auto value = machine.heap().element(
                *values, static_cast<usize>(*index));
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(*value);
        });
    add(registry, "java/util/Hashtable", "put",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto key = arguments[1].as_reference();
            auto value = arguments[2].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!key || key->is_null() || !value || value->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Hashtable does not accept null keys or values");
            }
            auto hash = object_hash(machine, *key);
            if (!hash) return std::unexpected(hash.error());
            auto existing = hashtable_find(
                machine, *object, *key, false, *hash);
            if (!existing) return std::unexpected(existing.error());
            if (*existing >= 0) {
                auto values = reference_field(machine, *object,
                                              kHashtableValuesField);
                if (!values) return std::unexpected(values.error());
                auto old = machine.heap().element(
                    *values, static_cast<usize>(*existing));
                if (!old) return std::unexpected(old.error());
                auto stored = machine.heap().set_element(
                    *values, static_cast<usize>(*existing),
                    Value::from_reference(*value));
                if (!stored) return std::unexpected(stored.error());
                return std::optional<Value>(*old);
            }
            constexpr std::array<usize, 3> sizing_fields {
                kHashtableCountField,
                kHashtableThresholdField,
                kHashtableTableCapacityField,
            };
            auto sizing = field_values(machine, *object, sizing_fields);
            if (!sizing) return std::unexpected(sizing.error());
            auto count = (*sizing)[0U].as_int();
            auto threshold = (*sizing)[1U].as_int();
            auto table_capacity = (*sizing)[2U].as_int();
            if (!count || !threshold || !table_capacity) {
                return fail(ErrorCode::invalid_state,
                            "Hashtable sizing state is invalid");
            }
            i32 required_capacity = *count + 1;
            if (*count >= *threshold) {
                if (*table_capacity >
                    (std::numeric_limits<i32>::max() - 1) / 2) {
                    return fail_java("java/lang/OutOfMemoryError",
                                     "Hashtable capacity overflowed");
                }
                required_capacity = *table_capacity * 2 + 1;
            }
            auto capacity = ensure_hashtable_capacity(
                machine, *object, required_capacity);
            if (!capacity) return std::unexpected(capacity.error());
            constexpr std::array<usize, 4> storage_fields {
                kHashtableKeysField,
                kHashtableValuesField,
                kHashtableHashesField,
                kHashtableBucketsField,
            };
            auto storage = field_values(machine, *object, storage_fields);
            if (!storage) return std::unexpected(storage.error());
            auto keys = (*storage)[0U].as_reference();
            auto values = (*storage)[1U].as_reference();
            auto hashes = (*storage)[2U].as_reference();
            auto buckets = (*storage)[3U].as_reference();
            if (!keys || !values || !hashes || !buckets)
                return fail(ErrorCode::invalid_state,
                            "Hashtable arrays are missing");
            auto key_stored = machine.heap().set_element(
                *keys, static_cast<usize>(*count),
                Value::from_reference(*key));
            auto value_stored = machine.heap().set_element(
                *values, static_cast<usize>(*count),
                Value::from_reference(*value));
            auto hash_stored = machine.heap().set_element(
                *hashes, static_cast<usize>(*count),
                Value::from_int(*hash));
            if (!key_stored) return std::unexpected(key_stored.error());
            if (!value_stored) return std::unexpected(value_stored.error());
            if (!hash_stored) return std::unexpected(hash_stored.error());
            auto order = iteration_order_with_insert(
                machine, *object, *count, *hash);
            if (!order) return std::unexpected(order.error());
            auto order_stored = store_hashtable_iteration_order(
                machine, *object, *order);
            if (!order_stored) return std::unexpected(order_stored.error());
            auto bucket_stored = hashtable_insert_bucket(
                machine, *buckets, *hash, *count);
            if (!bucket_stored) return std::unexpected(bucket_stored.error());
            auto updated = set_int_field(machine, *object,
                                         kHashtableCountField, *count + 1);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value>(Value::from_reference({}));
        });
    add(registry, "java/util/Hashtable", "remove",
        "(Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto key = arguments[1].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!key || key->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Hashtable key is null");
            }
            auto index = hashtable_find(machine, *object, *key, false);
            if (!index) return std::unexpected(index.error());
            if (*index < 0)
                return std::optional<Value>(Value::from_reference({}));
            constexpr std::array<usize, 5> fields {
                kHashtableKeysField,
                kHashtableValuesField,
                kHashtableHashesField,
                kHashtableCountField,
                kHashtableIterationOrderField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto keys = (*state)[0U].as_reference();
            auto values = (*state)[1U].as_reference();
            auto hashes = (*state)[2U].as_reference();
            auto count = (*state)[3U].as_int();
            auto order_array = (*state)[4U].as_reference();
            if (!keys || !values || !hashes || !count || !order_array ||
                keys->is_null() || values->is_null() || hashes->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "Hashtable state is invalid");
            }
            auto old = machine.heap().element(
                *values, static_cast<usize>(*index));
            if (!old) return std::unexpected(old.error());
            auto order = read_hashtable_iteration_order(
                machine, *order_array, *count);
            if (!order) return std::unexpected(order.error());
            order->erase(std::remove(order->begin(), order->end(), *index),
                         order->end());
            for (i32& entry : *order) {
                if (entry > *index) --entry;
            }
            const usize shifted = static_cast<usize>(*count - *index - 1);
            if (shifted != 0U) {
                const usize source = static_cast<usize>(*index + 1);
                const usize destination = static_cast<usize>(*index);
                auto keys_shifted = machine.heap().copy_array_range(
                    *keys, source, *keys, destination, shifted);
                auto values_shifted = machine.heap().copy_array_range(
                    *values, source, *values, destination, shifted);
                auto hashes_shifted = machine.heap().copy_array_range(
                    *hashes, source, *hashes, destination, shifted);
                if (!keys_shifted) return std::unexpected(keys_shifted.error());
                if (!values_shifted) {
                    return std::unexpected(values_shifted.error());
                }
                if (!hashes_shifted) {
                    return std::unexpected(hashes_shifted.error());
                }
            }
            auto key_cleared = machine.heap().set_element(
                *keys, static_cast<usize>(*count - 1),
                Value::from_reference({}));
            auto value_cleared = machine.heap().set_element(
                *values, static_cast<usize>(*count - 1),
                Value::from_reference({}));
            auto hash_cleared = machine.heap().set_element(
                *hashes, static_cast<usize>(*count - 1),
                Value::from_int(0));
            auto updated = set_int_field(machine, *object,
                                         kHashtableCountField, *count - 1);
            if (!key_cleared) return std::unexpected(key_cleared.error());
            if (!value_cleared) return std::unexpected(value_cleared.error());
            if (!hash_cleared) return std::unexpected(hash_cleared.error());
            if (!updated) return std::unexpected(updated.error());
            auto order_stored = write_hashtable_iteration_order(
                machine, *order_array, *order);
            if (!order_stored) return std::unexpected(order_stored.error());
            auto rebuilt = rebuild_hashtable_buckets(machine, *object);
            if (!rebuilt) return std::unexpected(rebuilt.error());
            return std::optional<Value>(*old);
        });
    add(registry, "java/util/Hashtable", "clear", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 4> fields {
                kHashtableKeysField,
                kHashtableValuesField,
                kHashtableHashesField,
                kHashtableCountField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto keys = (*state)[0U].as_reference();
            auto values = (*state)[1U].as_reference();
            auto hashes = (*state)[2U].as_reference();
            auto count = (*state)[3U].as_int();
            if (!keys || !values || !hashes || !count) {
                return fail(ErrorCode::invalid_state,
                            "Hashtable state is invalid");
            }
            if (*count == 0) {
                return std::optional<Value> {};
            }
            if (
                keys->is_null() || values->is_null() || hashes->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "Hashtable state is invalid");
            }
            if (*count > 0) {
                const usize live = static_cast<usize>(*count);
                auto keys_cleared = machine.heap().fill_array_range(
                    *keys, 0U, live, Value::from_reference({}));
                auto values_cleared = machine.heap().fill_array_range(
                    *values, 0U, live, Value::from_reference({}));
                auto hashes_cleared = machine.heap().fill_array_range(
                    *hashes, 0U, live, Value::from_int(0));
                if (!keys_cleared) return std::unexpected(keys_cleared.error());
                if (!values_cleared) {
                    return std::unexpected(values_cleared.error());
                }
                if (!hashes_cleared) {
                    return std::unexpected(hashes_cleared.error());
                }
            }
            auto updated = set_int_field(machine, *object,
                                         kHashtableCountField, 0);
            if (!updated) return std::unexpected(updated.error());
            const std::span<const i32> empty_order;
            auto order_stored = store_hashtable_iteration_order(
                machine, *object, empty_order);
            if (!order_stored) return std::unexpected(order_stored.error());
            auto rebuilt = rebuild_hashtable_buckets(machine, *object);
            if (!rebuilt) return std::unexpected(rebuilt.error());
            return std::optional<Value> {};
        });
    const auto enumeration = [&registry](const char* name, bool values) {
        add(registry, "java/util/Hashtable", name,
            "()Ljava/util/Enumeration;",
            [values](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                const std::array<usize, 3> fields {
                    kHashtableCountField,
                    values ? kHashtableValuesField : kHashtableKeysField,
                    kHashtableIterationOrderField,
                };
                auto state = field_values(machine, *object, fields);
                if (!state) return std::unexpected(state.error());
                auto count = (*state)[0U].as_int();
                auto array = (*state)[1U].as_reference();
                auto order_array = (*state)[2U].as_reference();
                if (!count || !array || !order_array)
                    return fail(ErrorCode::invalid_state,
                                "Hashtable state is invalid");
                if (*count == 0) {
                    const std::span<const ObjectRef> empty;
                    auto result = make_enumeration(machine, empty);
                    if (!result) return std::unexpected(result.error());
                    return std::optional<Value>(Value::from_reference(*result));
                }
                if (array->is_null() || order_array->is_null()) {
                    return fail(ErrorCode::invalid_state,
                                "Hashtable iteration storage is missing");
                }
                auto order = read_hashtable_iteration_order(
                    machine, *order_array, *count);
                if (!order) return std::unexpected(order.error());
                auto array_values = machine.heap().read_reference_array(*array);
                if (!array_values) return std::unexpected(array_values.error());
                std::vector<ObjectRef> snapshot;
                snapshot.reserve(static_cast<usize>(*count));
                for (const i32 entry : *order) {
                    if (entry < 0 ||
                        static_cast<usize>(entry) >= array_values->size()) {
                        return fail(ErrorCode::invalid_state,
                                    "Hashtable iteration order is out of range");
                    }
                    snapshot.push_back(
                        (*array_values)[static_cast<usize>(entry)]);
                }
                auto result = make_enumeration(machine, snapshot);
                if (!result) return std::unexpected(result.error());
                return std::optional<Value>(Value::from_reference(*result));
            });
    };
    enumeration("keys", false);
    enumeration("elements", true);
    add(registry, "java/util/Hashtable", "rehash", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto materialized = materialize_hashtable_storage(machine, *object);
            if (!materialized) return std::unexpected(materialized.error());
            auto keys = reference_field(machine, *object, kHashtableKeysField);
            if (!keys) return std::unexpected(keys.error());
            auto capacity = machine.heap().array_length(*keys);
            if (!capacity) return std::unexpected(capacity.error());
            if (*capacity > static_cast<usize>(
                    (std::numeric_limits<i32>::max() - 1) / 2)) {
                return fail_java("java/lang/OutOfMemoryError",
                                 "Hashtable capacity overflowed");
            }
            auto grown = ensure_hashtable_capacity(
                machine, *object, static_cast<i32>(*capacity * 2U + 1U));
            if (!grown) return std::unexpected(grown.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Hashtable", "toString",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 4> fields {
                kHashtableCountField,
                kHashtableKeysField,
                kHashtableValuesField,
                kHashtableIterationOrderField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto count = (*state)[0U].as_int();
            auto keys = (*state)[1U].as_reference();
            auto values = (*state)[2U].as_reference();
            auto order_array = (*state)[3U].as_reference();
            if (!count || !keys || !values || !order_array) {
                return fail(ErrorCode::invalid_state,
                            "Hashtable state is invalid");
            }
            if (*count == 0) {
                auto string = create_string(machine, std::u16string(u"{}"));
                if (!string) return std::unexpected(string.error());
                return std::optional<Value>(Value::from_reference(*string));
            }
            if (keys->is_null() || values->is_null() ||
                order_array->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "Hashtable string storage is missing");
            }
            auto order = read_hashtable_iteration_order(
                machine, *order_array, *count);
            if (!order) return std::unexpected(order.error());
            std::u16string text(u"{");
            for (usize position = 0; position < order->size(); ++position) {
                if (position != 0U) text.append(u", ");
                const i32 entry = (*order)[position];
                auto key_value = machine.heap().element(
                    *keys, static_cast<usize>(entry));
                auto mapped_value = machine.heap().element(
                    *values, static_cast<usize>(entry));
                if (!key_value || !mapped_value) {
                    return fail(ErrorCode::invalid_state,
                                "Hashtable entry is invalid");
                }
                auto key = key_value->as_reference();
                auto value = mapped_value->as_reference();
                if (!key || !value) {
                    return fail(ErrorCode::invalid_state,
                                "Hashtable entry is not a reference");
                }
                auto key_text = collection_value_text(
                    machine, *key, *object, u"(this Map)");
                auto value_text = collection_value_text(
                    machine, *value, *object, u"(this Map)");
                if (!key_text) return std::unexpected(key_text.error());
                if (!value_text) return std::unexpected(value_text.error());
                text.append(*key_text);
                text.push_back(u'=');
                text.append(*value_text);
            }
            text.push_back(u'}');
            auto string = create_string(machine, std::move(text));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
}

[[nodiscard]] Result<i32> random_next(Machine& machine,
                                      ObjectRef random,
                                      i32 bits) {
    if (bits < 1 || bits > 32) {
        return fail_java("java/lang/IllegalArgumentException",
                         "Random.next bit count is out of range");
    }
    auto seed = long_field(machine, random, kRandomSeedField);
    if (!seed) return std::unexpected(seed.error());
    const u64 updated = (static_cast<u64>(*seed) * kRandomMultiplier +
                         kRandomAddend) & kRandomMask;
    auto stored = set_long_field(machine, random,
                                 kRandomSeedField,
                                 static_cast<i64>(updated));
    if (!stored) return std::unexpected(stored.error());
    return static_cast<i32>(static_cast<u32>(updated >> (48 - bits)));
}

[[nodiscard]] Status seed_random_object(Machine& machine,
                                        ObjectRef random,
                                        i64 seed) {
    const u64 scrambled = (static_cast<u64>(seed) ^
                           kRandomMultiplier) & kRandomMask;
    return set_long_field(machine, random, kRandomSeedField,
                          static_cast<i64>(scrambled));
}

[[nodiscard]] Result<u64> random_u64(Machine& machine,
                                     ObjectRef random) {
    auto high = random_next(machine, random, 32);
    auto low = random_next(machine, random, 32);
    if (!high || !low) {
        return fail(ErrorCode::invalid_state,
                    "Random state update failed");
    }
    return (static_cast<u64>(static_cast<u32>(*high)) << 32U) |
           static_cast<u32>(*low);
}

[[nodiscard]] Result<i64> random_bounded_long(Machine& machine,
                                              ObjectRef random,
                                              i64 bound) {
    if (bound <= 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         "ThreadLocalRandom bound is not positive");
    }
    const u64 unsigned_bound = static_cast<u64>(bound);
    const u64 range = 1ULL << 63U;
    const u64 limit = range - (range % unsigned_bound);
    while (true) {
        auto bits = random_u64(machine, random);
        if (!bits) return std::unexpected(bits.error());
        const u64 candidate = *bits >> 1U;
        if (candidate < limit) {
            return static_cast<i64>(candidate % unsigned_bound);
        }
    }
}

[[nodiscard]] Result<double> random_unit_double(Machine& machine,
                                                ObjectRef random) {
    auto high = random_next(machine, random, 26);
    auto low = random_next(machine, random, 27);
    if (!high || !low) {
        return fail(ErrorCode::invalid_state,
                    "Random state update failed");
    }
    const u64 combined = (static_cast<u64>(static_cast<u32>(*high)) << 27U) +
                         static_cast<u32>(*low);
    return static_cast<double>(combined) / 9007199254740992.0;
}

void register_timer(NativeMethodRegistry& registry) {
    add(registry, "java/util/TimerTask", "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto task = receiver(arguments);
            if (!task) return std::unexpected(task.error());
            auto initialized = machine.timers().initialize_task(*task);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/TimerTask", "cancel", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto task = receiver(arguments);
            if (!task) return std::unexpected(task.error());
            auto cancelled = machine.timers().cancel_task(*task);
            if (!cancelled) return std::unexpected(cancelled.error());
            return std::optional<Value>(Value::from_int(*cancelled ? 1 : 0));
        });
    add(registry, "java/util/TimerTask", "scheduledExecutionTime", "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto task = receiver(arguments);
            if (!task) return std::unexpected(task.error());
            auto scheduled = machine.timers().scheduled_execution_time(*task);
            if (!scheduled) return std::unexpected(scheduled.error());
            return std::optional<Value>(Value::from_long(*scheduled));
        });

    add(registry, "java/util/Timer", "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto timer = receiver(arguments);
            if (!timer) return std::unexpected(timer.error());
            auto initialized = machine.timers().initialize_timer(*timer, false);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Timer", "<init>", "(Z)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto timer = receiver(arguments);
            auto daemon = int_argument(arguments, 1U);
            if (!timer) return std::unexpected(timer.error());
            if (!daemon) return std::unexpected(daemon.error());
            auto initialized =
                machine.timers().initialize_timer(*timer, *daemon != 0);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });

    const auto schedule_delay = [](bool repeating, bool fixed_rate) {
        return [repeating, fixed_rate](
            Machine& machine,
            std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto timer = receiver(arguments);
            auto task = reference_argument(arguments, 1U);
            auto delay = long_argument(arguments, 2U);
            if (!timer) return std::unexpected(timer.error());
            if (!task) return std::unexpected(task.error());
            if (!delay) return std::unexpected(delay.error());
            auto first = delayed_time(*delay);
            if (!first) return std::unexpected(first.error());
            i64 period = 0;
            if (repeating) {
                auto value = long_argument(arguments, 3U);
                if (!value) return std::unexpected(value.error());
                if (*value <= 0) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Timer period must be positive");
                }
                period = *value;
            }
            auto scheduled = machine.timers().schedule(
                *timer, *task, *first, period, fixed_rate);
            if (!scheduled) return std::unexpected(scheduled.error());
            return std::optional<Value> {};
        };
    };
    const auto schedule_date = [](bool repeating, bool fixed_rate) {
        return [repeating, fixed_rate](
            Machine& machine,
            std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto timer = receiver(arguments);
            auto task = reference_argument(arguments, 1U);
            auto date = reference_argument(arguments, 2U);
            if (!timer) return std::unexpected(timer.error());
            if (!task) return std::unexpected(task.error());
            if (!date) return std::unexpected(date.error());
            auto time = long_field(machine, *date, kDateMillisField);
            if (!time) return std::unexpected(time.error());
            if (*time < 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Timer date cannot precede the epoch");
            }
            i64 period = 0;
            if (repeating) {
                auto value = long_argument(arguments, 3U);
                if (!value) return std::unexpected(value.error());
                if (*value <= 0) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Timer period must be positive");
                }
                period = *value;
            }
            const i64 first = std::max(*time, current_time_millis());
            auto scheduled = machine.timers().schedule(
                *timer, *task, first, period, fixed_rate);
            if (!scheduled) return std::unexpected(scheduled.error());
            return std::optional<Value> {};
        };
    };

    add(registry, "java/util/Timer", "schedule",
        "(Ljava/util/TimerTask;J)V", schedule_delay(false, false));
    add(registry, "java/util/Timer", "schedule",
        "(Ljava/util/TimerTask;JJ)V", schedule_delay(true, false));
    add(registry, "java/util/Timer", "schedule",
        "(Ljava/util/TimerTask;Ljava/util/Date;)V",
        schedule_date(false, false));
    add(registry, "java/util/Timer", "schedule",
        "(Ljava/util/TimerTask;Ljava/util/Date;J)V",
        schedule_date(true, false));
    add(registry, "java/util/Timer", "scheduleAtFixedRate",
        "(Ljava/util/TimerTask;JJ)V", schedule_delay(true, true));
    add(registry, "java/util/Timer", "scheduleAtFixedRate",
        "(Ljava/util/TimerTask;Ljava/util/Date;J)V",
        schedule_date(true, true));
    add(registry, "java/util/Timer", "cancel", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto timer = receiver(arguments);
            if (!timer) return std::unexpected(timer.error());
            auto cancelled = machine.timers().cancel_timer(*timer);
            if (!cancelled) return std::unexpected(cancelled.error());
            return std::optional<Value> {};
        });
}

void register_random(NativeMethodRegistry& registry) {
    const auto set_seed = [](Machine& machine,
                             ObjectRef object,
                             i64 seed) -> Status {
        const u64 scrambled = (static_cast<u64>(seed) ^
                               kRandomMultiplier) & kRandomMask;
        return set_long_field(machine, object, kRandomSeedField,
                              static_cast<i64>(scrambled));
    };
    add(registry, "java/util/Random", "<init>", "()V",
        [set_seed](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            const auto now = std::chrono::high_resolution_clock::now()
                                 .time_since_epoch().count();
            auto stored = set_seed(machine, *object,
                static_cast<i64>(now) ^ static_cast<i64>(object->bits));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Random", "<init>", "(J)V",
        [set_seed](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto seed = arguments[1].as_long();
            if (!object) return std::unexpected(object.error());
            if (!seed) return std::unexpected(seed.error());
            auto stored = set_seed(machine, *object, *seed);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Random", "setSeed", "(J)V",
        [set_seed](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto seed = arguments[1].as_long();
            if (!object) return std::unexpected(object.error());
            if (!seed) return std::unexpected(seed.error());
            auto stored = set_seed(machine, *object, *seed);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Random", "next", "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto bits = arguments[1].as_int();
            if (!object) return std::unexpected(object.error());
            if (!bits) return std::unexpected(bits.error());
            auto value = random_next(machine, *object, *bits);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    add(registry, "java/util/Random", "nextInt", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = random_next(machine, *object, 32);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    add(registry, "java/util/Random", "nextInt", "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto bound = arguments[1].as_int();
            if (!object) return std::unexpected(object.error());
            if (!bound) return std::unexpected(bound.error());
            if (*bound <= 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Random.nextInt bound is not positive");
            }
            if ((*bound & -*bound) == *bound) {
                auto bits = random_next(machine, *object, 31);
                if (!bits) return std::unexpected(bits.error());
                const i64 result = (static_cast<i64>(*bound) *
                                    static_cast<i64>(*bits)) >> 31;
                return std::optional<Value>(
                    Value::from_int(static_cast<i32>(result)));
            }
            i32 bits = 0;
            i32 value = 0;
            do {
                auto generated = random_next(machine, *object, 31);
                if (!generated) return std::unexpected(generated.error());
                bits = *generated;
                value = bits % *bound;
            } while (bits - value + (*bound - 1) < 0);
            return std::optional<Value>(Value::from_int(value));
        });
    add(registry, "java/util/Random", "nextLong", "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto high = random_next(machine, *object, 32);
            auto low = random_next(machine, *object, 32);
            if (!high || !low)
                return fail(ErrorCode::invalid_state,
                            "Random state update failed");
            const u64 bits = (static_cast<u64>(static_cast<u32>(*high))
                              << 32U) |
                             static_cast<u32>(*low);
            return std::optional<Value>(
                Value::from_long(static_cast<i64>(bits)));
        });
    add(registry, "java/util/Random", "nextBoolean", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = random_next(machine, *object, 1);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    add(registry, "java/util/Random", "nextFloat", "()F",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = random_next(machine, *object, 24);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_float(
                static_cast<float>(*value) / 16777216.0F));
        });
    add(registry, "java/util/Random", "nextDouble", "()D",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto high = random_next(machine, *object, 26);
            auto low = random_next(machine, *object, 27);
            if (!high || !low)
                return fail(ErrorCode::invalid_state,
                            "Random state update failed");
            const u64 combined = (static_cast<u64>(static_cast<u32>(*high))
                                  << 27U) +
                                 static_cast<u32>(*low);
            return std::optional<Value>(Value::from_double(
                static_cast<double>(combined) /
                9007199254740992.0));
        });
}

void register_thread_local_random(NativeMethodRegistry& registry) {
    constexpr std::string_view kOwner =
        "java/util/concurrent/ThreadLocalRandom";
    constexpr std::string_view kDescriptor =
        "Ljava/util/concurrent/ThreadLocalRandom;";

    const auto singleton_field = [](Machine& machine)
        -> Result<FieldLocation> {
        return machine.class_states().resolve_field(
            "java/util/concurrent/ThreadLocalRandom",
            "INSTANCE",
            "Ljava/util/concurrent/ThreadLocalRandom;",
            true);
    };
    const auto allocate_singleton = [](Machine& machine)
        -> Result<ObjectRef> {
        auto object = machine.class_states().allocate_instance(
            machine.heap(), "java/util/concurrent/ThreadLocalRandom");
        if (!object) return std::unexpected(object.error());
        const auto now = std::chrono::high_resolution_clock::now()
                             .time_since_epoch().count();
        auto seeded = seed_random_object(
            machine, *object,
            static_cast<i64>(now) ^ static_cast<i64>(object->bits));
        if (!seeded) return std::unexpected(seeded.error());
        return *object;
    };

    add(registry, std::string(kOwner), "<clinit>", "()V",
        [singleton_field, allocate_singleton](
            Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "ThreadLocalRandom.<clinit> expects no arguments");
            }
            auto field = singleton_field(machine);
            if (!field) return std::unexpected(field.error());
            auto object = allocate_singleton(machine);
            if (!object) return std::unexpected(object.error());
            auto stored = machine.class_states().set_static_field(
                *field, Value::from_reference(*object));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    add(registry, std::string(kOwner), "current", "()" +
            std::string(kDescriptor),
        [singleton_field, allocate_singleton](
            Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "ThreadLocalRandom.current expects no arguments");
            }
            auto field = singleton_field(machine);
            if (!field) return std::unexpected(field.error());
            auto value = machine.class_states().static_field(*field);
            if (!value) return std::unexpected(value.error());
            auto object = value->as_reference();
            if (!object) return std::unexpected(object.error());
            if (object->is_null()) {
                auto allocated = allocate_singleton(machine);
                if (!allocated) return std::unexpected(allocated.error());
                auto stored = machine.class_states().set_static_field(
                    *field, Value::from_reference(*allocated));
                if (!stored) return std::unexpected(stored.error());
                return std::optional<Value>(
                    Value::from_reference(*allocated));
            }
            return std::optional<Value>(Value::from_reference(*object));
        });

    add(registry, std::string(kOwner), "nextInt", "(II)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto origin = int_argument(arguments, 1);
            auto bound = int_argument(arguments, 2);
            if (!object) return std::unexpected(object.error());
            if (!origin) return std::unexpected(origin.error());
            if (!bound) return std::unexpected(bound.error());
            if (*origin >= *bound) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "ThreadLocalRandom origin is not below bound");
            }
            const i64 range = static_cast<i64>(*bound) -
                              static_cast<i64>(*origin);
            auto offset = random_bounded_long(machine, *object, range);
            if (!offset) return std::unexpected(offset.error());
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(static_cast<i64>(*origin) + *offset)));
        });

    add(registry, std::string(kOwner), "nextLong", "(J)J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto bound = long_argument(arguments, 1);
            if (!object) return std::unexpected(object.error());
            if (!bound) return std::unexpected(bound.error());
            auto value = random_bounded_long(machine, *object, *bound);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_long(*value));
        });

    add(registry, std::string(kOwner), "nextLong", "(JJ)J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto origin = long_argument(arguments, 1);
            auto bound = long_argument(arguments, 2);
            if (!object) return std::unexpected(object.error());
            if (!origin) return std::unexpected(origin.error());
            if (!bound) return std::unexpected(bound.error());
            if (*origin >= *bound) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "ThreadLocalRandom origin is not below bound");
            }
            const u64 width = static_cast<u64>(*bound) -
                              static_cast<u64>(*origin);
            if (width <= static_cast<u64>(std::numeric_limits<i64>::max())) {
                auto offset = random_bounded_long(
                    machine, *object, static_cast<i64>(width));
                if (!offset) return std::unexpected(offset.error());
                return std::optional<Value>(Value::from_long(
                    static_cast<i64>(static_cast<u64>(*origin) +
                                     static_cast<u64>(*offset))));
            }
            while (true) {
                auto bits = random_u64(machine, *object);
                if (!bits) return std::unexpected(bits.error());
                const i64 candidate = static_cast<i64>(*bits);
                if (candidate >= *origin && candidate < *bound) {
                    return std::optional<Value>(
                        Value::from_long(candidate));
                }
            }
        });

    add(registry, std::string(kOwner), "nextDouble", "(D)D",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto bound = arguments[1].as_double();
            if (!object) return std::unexpected(object.error());
            if (!bound) return std::unexpected(bound.error());
            if (!(*bound > 0.0) || !std::isfinite(*bound)) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "ThreadLocalRandom bound is not finite and positive");
            }
            auto unit = random_unit_double(machine, *object);
            if (!unit) return std::unexpected(unit.error());
            double result = *unit * *bound;
            if (result >= *bound) {
                result = std::nextafter(*bound, 0.0);
            }
            return std::optional<Value>(Value::from_double(result));
        });

    add(registry, std::string(kOwner), "nextDouble", "(DD)D",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto origin = arguments[1].as_double();
            auto bound = arguments[2].as_double();
            if (!object) return std::unexpected(object.error());
            if (!origin) return std::unexpected(origin.error());
            if (!bound) return std::unexpected(bound.error());
            if (!(*origin < *bound) || !std::isfinite(*origin) ||
                !std::isfinite(*bound)) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "ThreadLocalRandom double range is invalid");
            }
            auto unit = random_unit_double(machine, *object);
            if (!unit) return std::unexpected(unit.error());
            double result = *origin + (*bound - *origin) * *unit;
            if (result >= *bound) {
                result = std::nextafter(*bound, *origin);
            }
            return std::optional<Value>(Value::from_double(result));
        });
}

void register_locale(NativeMethodRegistry& registry) {
    add(registry, "java/util/Locale", "<clinit>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Locale.<clinit> expects no arguments");
            }
            auto root = machine.class_states().allocate_instance(
                machine.heap(), "java/util/Locale");
            if (!root) return std::unexpected(root.error());
            auto field = machine.class_states().resolve_field(
                "java/util/Locale", "ROOT", "Ljava/util/Locale;", true);
            if (!field) return std::unexpected(field.error());
            auto stored = machine.class_states().set_static_field(
                *field, Value::from_reference(*root));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
}

} // namespace

void register_util_natives(NativeMethodRegistry& registry) {
    register_locale(registry);
    register_string_tokenizer(registry);
    register_enumeration(registry);
    register_array_list(registry);
    register_vector(registry);
    register_hashtable(registry);
    register_timer(registry);
    register_random(registry);
    register_thread_local_random(registry);
}

} // namespace phoneme::vm
