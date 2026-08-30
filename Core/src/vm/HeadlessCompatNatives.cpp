#include "HeadlessCompatNatives.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/NativeRootScope.hpp"

namespace phoneme::vm {
namespace {

constexpr usize kArrayListDataField = 0U;
constexpr usize kArrayListSizeField = 1U;
constexpr usize kArrayListCapacityIncrementField = 2U;
constexpr usize kArrayListMutationModeField = 3U;
constexpr i32 kArrayListMutable = 0;
constexpr i32 kArrayListImmutable = 1;
constexpr i32 kArrayListFixedSize = 2;

constexpr usize kIteratorValuesField = 0U;
constexpr usize kIteratorIndexField = 1U;
constexpr usize kIteratorSizeField = 2U;
constexpr usize kIteratorOwnerField = 3U;
constexpr usize kIteratorLastReturnedField = 4U;
constexpr usize kIteratorRemoveKindField = 5U;
constexpr i32 kIteratorRemoveUnsupported = 0;
constexpr i32 kIteratorRemoveArrayList = 1;

constexpr usize kHashMapKeysField = 0U;
constexpr usize kHashMapValuesField = 1U;
constexpr usize kHashMapSizeField = 2U;
constexpr usize kHashMapHashesField = 3U;
constexpr usize kHashSetMapField = 0U;
constexpr usize kOptionalValueField = 0U;
constexpr usize kOptionalPresentField = 1U;

void add(NativeMethodRegistry& registry,
         std::string owner,
         std::string name,
         std::string descriptor,
         NativeMethod implementation) {
    auto registered = registry.register_method(std::move(owner),
                                               std::move(name),
                                               std::move(descriptor),
                                               std::move(implementation));
    if (!registered) std::terminate();
}

[[nodiscard]] Result<ObjectRef> receiver(std::span<const Value> arguments) {
    if (arguments.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "headless compatibility native is missing its receiver");
    }
    auto object = arguments.front().as_reference();
    if (!object || object->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "headless compatibility receiver is null");
    }
    return *object;
}

[[nodiscard]] Result<ObjectRef> reference_argument(
    std::span<const Value> arguments,
    usize index,
    bool nullable = false) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    "headless compatibility argument is missing");
    }
    auto object = arguments[index].as_reference();
    if (!object) return std::unexpected(object.error());
    if (!nullable && object->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "headless compatibility argument is null");
    }
    return *object;
}

[[nodiscard]] Result<i32> int_argument(std::span<const Value> arguments,
                                       usize index) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    "headless compatibility int argument is missing");
    }
    return arguments[index].as_int();
}

[[nodiscard]] Result<ObjectRef> reference_field(Machine& machine,
                                                 ObjectRef object,
                                                 usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
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
    auto read = machine.heap().read_fields(object, indices, values);
    if (!read) return std::unexpected(read.error());
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

struct OptionalState final {
    ObjectRef value {};
    bool present {false};
};

[[nodiscard]] Result<OptionalState> optional_state(Machine& machine,
                                                   ObjectRef object) {
    constexpr std::array<usize, 2> fields {
        kOptionalValueField,
        kOptionalPresentField,
    };
    auto state = field_values(machine, object, fields);
    if (!state) return std::unexpected(state.error());
    auto value = (*state)[0U].as_reference();
    auto present = (*state)[1U].as_int();
    if (!value) return std::unexpected(value.error());
    if (!present) return std::unexpected(present.error());
    return OptionalState {
        .value = *value,
        .present = *present != 0,
    };
}

[[nodiscard]] Status set_optional_state(Machine& machine,
                                        ObjectRef object,
                                        ObjectRef value,
                                        bool present) {
    constexpr std::array<usize, 2> fields {
        kOptionalValueField,
        kOptionalPresentField,
    };
    const std::array<Value, 2> state {
        Value::from_reference(value),
        Value::from_int(present ? 1 : 0),
    };
    return set_field_values(machine, object, fields, state);
}

[[nodiscard]] Status set_reference_field(Machine& machine,
                                         ObjectRef object,
                                         usize index,
                                         ObjectRef value) {
    return machine.heap().set_field(object, index,
                                    Value::from_reference(value));
}

[[nodiscard]] Status set_int_field(Machine& machine,
                                   ObjectRef object,
                                   usize index,
                                   i32 value) {
    return machine.heap().set_field(object, index, Value::from_int(value));
}

[[nodiscard]] Result<ObjectRef> allocate_object_array(Machine& machine,
                                                       usize length) {
    return machine.heap().allocate_array(
        "[Ljava/lang/Object;", length, Value::from_reference({}));
}

[[nodiscard]] Result<ObjectRef> allocate_byte_array(Machine& machine,
                                                     usize length) {
    return machine.heap().allocate_array("[B", length, Value::from_int(0));
}

[[nodiscard]] Result<ObjectRef> allocate_int_array(Machine& machine,
                                                    usize length) {
    return machine.heap().allocate_array("[I", length, Value::from_int(0));
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

[[nodiscard]] Result<std::string> utf8_text(Machine& machine,
                                            ObjectRef string) {
    if (string.is_null()) return std::string {};
    auto text = machine.heap().string_value(string);
    if (!text) return std::unexpected(text.error());
    std::string result;
    result.reserve(text->size());
    for (usize index = 0U; index < text->size(); ++index) {
        u32 code_point = static_cast<u16>((*text)[index]);
        if (code_point >= 0xD800U && code_point <= 0xDBFFU &&
            index + 1U < text->size()) {
            const u32 low = static_cast<u16>((*text)[index + 1U]);
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                code_point = 0x10000U +
                    ((code_point - 0xD800U) << 10U) +
                    (low - 0xDC00U);
                ++index;
            }
        }
        if (code_point <= 0x7FU) {
            result.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7FFU) {
            result.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else if (code_point <= 0xFFFFU) {
            result.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            result.push_back(static_cast<char>(0x80U |
                                               ((code_point >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else {
            result.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            result.push_back(static_cast<char>(0x80U |
                                               ((code_point >> 12U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U |
                                               ((code_point >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        }
    }
    return result;
}

[[nodiscard]] Result<std::optional<Value>> invoke_checked(
    Machine& machine,
    ObjectRef object,
    std::string_view declared_class,
    std::string_view name,
    std::string_view descriptor,
    std::span<const Value> arguments = {}) {
    auto invoked = machine.invoke_instance(object, declared_class, name,
                                           descriptor, arguments);
    if (!invoked) return std::unexpected(invoked.error());
    if (invoked->throwable.has_value()) {
        auto throwable_class = machine.heap().class_name(*invoked->throwable);
        if (!throwable_class) return std::unexpected(throwable_class.error());
        return fail_java(*throwable_class,
                         "headless compatibility callback threw from " +
                             std::string(declared_class) + "." +
                             std::string(name) + std::string(descriptor));
    }
    return invoked->return_value;
}

[[nodiscard]] Result<bool> values_equal(Machine& machine,
                                        ObjectRef left,
                                        ObjectRef right) {
    if (left == right) return true;
    if (left.is_null() || right.is_null()) return false;
    const Value argument = Value::from_reference(right);
    auto result = invoke_checked(machine, left, "java/lang/Object", "equals",
                                 "(Ljava/lang/Object;)Z",
                                 std::span<const Value>(&argument, 1U));
    if (!result) return std::unexpected(result.error());
    if (!result->has_value()) {
        return fail(ErrorCode::internal_error,
                    "Object.equals returned no value");
    }
    auto equal = result->value().as_int();
    if (!equal) return std::unexpected(equal.error());
    return *equal != 0;
}

[[nodiscard]] Result<bool> deep_values_equal(Machine& machine,
                                             ObjectRef left,
                                             ObjectRef right) {
    if (left == right) return true;
    if (left.is_null() || right.is_null()) return false;
    auto left_class = machine.heap().class_name(left);
    auto right_class = machine.heap().class_name(right);
    if (!left_class) return std::unexpected(left_class.error());
    if (!right_class) return std::unexpected(right_class.error());
    const bool left_array = !left_class->empty() && left_class->front() == '[';
    const bool right_array = !right_class->empty() && right_class->front() == '[';
    if (!left_array || !right_array) {
        return values_equal(machine, left, right);
    }

    const bool left_reference_array = left_class->starts_with("[L") ||
                                      left_class->starts_with("[[");
    const bool right_reference_array = right_class->starts_with("[L") ||
                                       right_class->starts_with("[[");
    if (left_reference_array != right_reference_array) return false;
    if (!left_reference_array && *left_class != *right_class) return false;

    auto left_length = machine.heap().array_length(left);
    auto right_length = machine.heap().array_length(right);
    if (!left_length) return std::unexpected(left_length.error());
    if (!right_length) return std::unexpected(right_length.error());
    if (*left_length != *right_length) return false;

    for (usize index = 0U; index < *left_length; ++index) {
        auto left_value = machine.heap().element(left, index);
        auto right_value = machine.heap().element(right, index);
        if (!left_value) return std::unexpected(left_value.error());
        if (!right_value) return std::unexpected(right_value.error());
        if (left_reference_array) {
            auto left_ref = left_value->as_reference();
            auto right_ref = right_value->as_reference();
            if (!left_ref) return std::unexpected(left_ref.error());
            if (!right_ref) return std::unexpected(right_ref.error());
            auto equal = deep_values_equal(machine, *left_ref, *right_ref);
            if (!equal) return std::unexpected(equal.error());
            if (!*equal) return false;
            continue;
        }
        if (left_value->kind() != right_value->kind() ||
            left_value->raw_bits_unchecked() != right_value->raw_bits_unchecked()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result<std::u16string> value_text(Machine& machine,
                                                ObjectRef value,
                                                ObjectRef container,
                                                std::u16string_view self_text) {
    if (value.is_null()) return std::u16string(u"null");
    if (value == container) return std::u16string(self_text);
    auto result = invoke_checked(machine, value, "java/lang/Object", "toString",
                                 "()Ljava/lang/String;");
    if (!result) return std::unexpected(result.error());
    if (!result->has_value()) {
        return fail(ErrorCode::internal_error,
                    "Object.toString returned no value");
    }
    auto string = result->value().as_reference();
    if (!string || string->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "Object.toString returned null");
    }
    return machine.heap().string_value(*string);
}

[[nodiscard]] Status initialize_array_list(Machine& machine,
                                           ObjectRef list,
                                           i32 requested_capacity) {
    if (requested_capacity < 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         "ArrayList capacity is negative");
    }
    const usize capacity = static_cast<usize>(requested_capacity);
    auto data = allocate_object_array(machine, capacity);
    if (!data) return std::unexpected(data.error());
    constexpr std::array<usize, 4> fields {
        kArrayListDataField,
        kArrayListSizeField,
        kArrayListCapacityIncrementField,
        kArrayListMutationModeField,
    };
    const std::array<Value, 4> state {
        Value::from_reference(*data),
        Value::from_int(0),
        Value::from_int(0),
        Value::from_int(kArrayListMutable),
    };
    return set_field_values(machine, list, fields, state);
}

[[nodiscard]] Result<ObjectRef> ensure_array_list_capacity_from_state(
    Machine& machine,
    ObjectRef list,
    ObjectRef data,
    i32 size,
    i32 minimum) {
    if (minimum < 0) {
        return fail_java("java/lang/OutOfMemoryError",
                         "ArrayList size overflow");
    }
    auto length = machine.heap().array_length(data);
    if (!length) return std::unexpected(length.error());
    if (static_cast<usize>(minimum) <= *length) return data;
    usize capacity = *length == 0U ? 10U : *length + (*length >> 1U);
    capacity = std::max(capacity, static_cast<usize>(minimum));
    auto replacement = allocate_object_array(machine, capacity);
    if (!replacement) return std::unexpected(replacement.error());
    if (size > 0) {
        auto copied = machine.heap().copy_array_range(
            data, 0U, *replacement, 0U, static_cast<usize>(size));
        if (!copied) return std::unexpected(copied.error());
    }
    auto stored = set_reference_field(machine, list, kArrayListDataField,
                                      *replacement);
    if (!stored) return std::unexpected(stored.error());
    return *replacement;
}

[[nodiscard]] Status array_list_append(Machine& machine,
                                       ObjectRef list,
                                       ObjectRef value) {
    constexpr std::array<usize, 2> fields {
        kArrayListDataField,
        kArrayListSizeField,
    };
    auto state = field_values(machine, list, fields);
    if (!state) return std::unexpected(state.error());
    auto data = (*state)[0U].as_reference();
    auto size = (*state)[1U].as_int();
    if (!data || data->is_null() || !size || *size < 0) {
        return fail(ErrorCode::invalid_state,
                    "ArrayList state is invalid");
    }
    auto writable_data = ensure_array_list_capacity_from_state(
        machine, list, *data, *size, *size + 1);
    if (!writable_data) return std::unexpected(writable_data.error());
    auto stored = machine.heap().set_element(
        *writable_data, static_cast<usize>(*size), Value::from_reference(value));
    if (!stored) return stored;
    return set_int_field(machine, list, kArrayListSizeField, *size + 1);
}

[[nodiscard]] Result<ObjectRef> create_array_iterator(
    Machine& machine,
    ObjectRef values,
    i32 size,
    ObjectRef owner = {},
    i32 remove_kind = kIteratorRemoveUnsupported) {
    if (size < 0) {
        return fail(ErrorCode::invalid_state,
                    "iterator size is negative");
    }
    auto iterator = machine.class_states().allocate_instance(
        machine.heap(), "java/util/ArrayIterator");
    if (!iterator) return std::unexpected(iterator.error());
    constexpr std::array<usize, 6> fields {
        kIteratorValuesField,
        kIteratorIndexField,
        kIteratorSizeField,
        kIteratorOwnerField,
        kIteratorLastReturnedField,
        kIteratorRemoveKindField,
    };
    const std::array<Value, 6> state {
        Value::from_reference(values),
        Value::from_int(0),
        Value::from_int(size),
        Value::from_reference(owner),
        Value::from_int(-1),
        Value::from_int(remove_kind),
    };
    auto stored = set_field_values(machine, *iterator, fields, state);
    if (!stored) return std::unexpected(stored.error());
    return *iterator;
}

[[nodiscard]] Result<std::vector<ObjectRef>> collection_elements(
    Machine& machine,
    ObjectRef collection) {
    auto iterator_value = invoke_checked(
        machine, collection, "java/util/Collection", "iterator",
        "()Ljava/util/Iterator;");
    if (!iterator_value) return std::unexpected(iterator_value.error());
    if (!iterator_value->has_value()) {
        return fail(ErrorCode::internal_error,
                    "Collection.iterator returned no value");
    }
    auto iterator = iterator_value->value().as_reference();
    if (!iterator || iterator->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "Collection.iterator returned null");
    }
    auto pinned = NativeRootScope::pin(machine.native_roots(), *iterator);
    if (!pinned) return std::unexpected(pinned.error());

    std::vector<ObjectRef> elements;
    for (;;) {
        auto has_next_value = invoke_checked(
            machine, *iterator, "java/util/Iterator", "hasNext", "()Z");
        if (!has_next_value) return std::unexpected(has_next_value.error());
        if (!has_next_value->has_value()) {
            return fail(ErrorCode::internal_error,
                        "Iterator.hasNext returned no value");
        }
        auto has_next = has_next_value->value().as_int();
        if (!has_next) return std::unexpected(has_next.error());
        if (*has_next == 0) break;
        auto next_value = invoke_checked(
            machine, *iterator, "java/util/Iterator", "next",
            "()Ljava/lang/Object;");
        if (!next_value) return std::unexpected(next_value.error());
        if (!next_value->has_value()) {
            return fail(ErrorCode::internal_error,
                        "Iterator.next returned no value");
        }
        auto next = next_value->value().as_reference();
        if (!next) return std::unexpected(next.error());
        elements.push_back(*next);
    }
    return elements;
}

[[nodiscard]] Result<i32> hash_map_key_hash(Machine& machine,
                                             ObjectRef key) {
    if (key.is_null()) return 0;

    auto class_name = machine.heap().class_name(key);
    if (!class_name) return std::unexpected(class_name.error());
    if (*class_name == "java/lang/String") {
        auto text = machine.heap().string_value(key);
        if (!text) return std::unexpected(text.error());
        u32 hash = 0U;
        for (const char16_t character : *text) {
            hash = hash * 31U + static_cast<u16>(character);
        }
        return static_cast<i32>(hash);
    }

    auto result = invoke_checked(machine, key, "java/lang/Object", "hashCode",
                                 "()I");
    if (!result) return std::unexpected(result.error());
    if (!result->has_value()) {
        return fail(ErrorCode::internal_error,
                    "Object.hashCode returned no value");
    }
    return result->value().as_int();
}

[[nodiscard]] Result<i32> map_find_key_with_hash(Machine& machine,
                                                  ObjectRef map,
                                                  ObjectRef key,
                                                  i32 target_hash) {
    constexpr std::array<usize, 3> fields {
        kHashMapSizeField,
        kHashMapKeysField,
        kHashMapHashesField,
    };
    auto state = field_values(machine, map, fields);
    if (!state) return std::unexpected(state.error());
    auto size = (*state)[0U].as_int();
    auto keys = (*state)[1U].as_reference();
    auto hashes = (*state)[2U].as_reference();
    if (!size || !keys || !hashes || keys->is_null() || hashes->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "HashMap state is invalid");
    }
    if (*size < 0) {
        return fail(ErrorCode::invalid_state,
                    "HashMap size is negative");
    }
    auto cached_hashes = machine.heap().read_int_array(*hashes);
    if (!cached_hashes) return std::unexpected(cached_hashes.error());
    if (static_cast<usize>(*size) > cached_hashes->size()) {
        return fail(ErrorCode::invalid_state,
                    "HashMap hash storage is shorter than its size");
    }
    for (i32 index = 0; index < *size; ++index) {
        if ((*cached_hashes)[static_cast<usize>(index)] != target_hash) continue;

        auto current_value = machine.heap().element(
            *keys, static_cast<usize>(index));
        if (!current_value) return std::unexpected(current_value.error());
        auto current = current_value->as_reference();
        if (!current) return std::unexpected(current.error());
        auto equal = values_equal(machine, *current, key);
        if (!equal) return std::unexpected(equal.error());
        if (*equal) return index;
    }
    return -1;
}

[[nodiscard]] Result<i32> map_find_key(Machine& machine,
                                       ObjectRef map,
                                       ObjectRef key) {
    auto hash = hash_map_key_hash(machine, key);
    if (!hash) return std::unexpected(hash.error());
    return map_find_key_with_hash(machine, map, key, *hash);
}

[[nodiscard]] Status initialize_hash_map(Machine& machine,
                                         ObjectRef map,
                                         i32 requested_capacity) {
    if (requested_capacity < 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         "HashMap capacity is negative");
    }
    const usize capacity = std::max<usize>(4U,
        static_cast<usize>(requested_capacity));
    auto keys = allocate_object_array(machine, capacity);
    if (!keys) return std::unexpected(keys.error());
    auto keys_root = NativeRootScope::pin(machine.native_roots(), *keys);
    if (!keys_root) return std::unexpected(keys_root.error());
    auto values = allocate_object_array(machine, capacity);
    if (!values) return std::unexpected(values.error());
    auto values_root = NativeRootScope::pin(machine.native_roots(), *values);
    if (!values_root) return std::unexpected(values_root.error());
    auto hashes = allocate_int_array(machine, capacity);
    if (!hashes) return std::unexpected(hashes.error());
    auto hashes_root = NativeRootScope::pin(machine.native_roots(), *hashes);
    if (!hashes_root) return std::unexpected(hashes_root.error());
    constexpr std::array<usize, 4> fields {
        kHashMapKeysField,
        kHashMapValuesField,
        kHashMapSizeField,
        kHashMapHashesField,
    };
    const std::array<Value, 4> values_to_store {
        Value::from_reference(*keys),
        Value::from_reference(*values),
        Value::from_int(0),
        Value::from_reference(*hashes),
    };
    return set_field_values(machine, map, fields, values_to_store);
}

[[nodiscard]] Status ensure_hash_map_capacity(Machine& machine,
                                              ObjectRef map,
                                              i32 minimum) {
    constexpr std::array<usize, 4> fields {
        kHashMapKeysField,
        kHashMapValuesField,
        kHashMapSizeField,
        kHashMapHashesField,
    };
    auto state = field_values(machine, map, fields);
    if (!state) return std::unexpected(state.error());
    auto keys = (*state)[0U].as_reference();
    auto values = (*state)[1U].as_reference();
    auto size = (*state)[2U].as_int();
    auto hashes = (*state)[3U].as_reference();
    if (!keys || !values || !hashes || keys->is_null() ||
        values->is_null() || hashes->is_null() || !size || *size < 0) {
        return fail(ErrorCode::invalid_state,
                    "HashMap storage is invalid");
    }
    auto capacity = machine.heap().array_length(*keys);
    if (!capacity) return std::unexpected(capacity.error());
    if (static_cast<usize>(minimum) <= *capacity) return {};
    const usize new_capacity = std::max(
        static_cast<usize>(minimum), *capacity * 2U);
    auto new_keys = allocate_object_array(machine, new_capacity);
    if (!new_keys) return std::unexpected(new_keys.error());
    auto new_keys_root = NativeRootScope::pin(machine.native_roots(), *new_keys);
    if (!new_keys_root) return std::unexpected(new_keys_root.error());
    auto new_values = allocate_object_array(machine, new_capacity);
    if (!new_values) return std::unexpected(new_values.error());
    auto new_values_root = NativeRootScope::pin(
        machine.native_roots(), *new_values);
    if (!new_values_root) return std::unexpected(new_values_root.error());
    auto new_hashes = allocate_int_array(machine, new_capacity);
    if (!new_hashes) return std::unexpected(new_hashes.error());
    auto new_hashes_root = NativeRootScope::pin(
        machine.native_roots(), *new_hashes);
    if (!new_hashes_root) return std::unexpected(new_hashes_root.error());
    if (*size > 0) {
        auto copied_keys = machine.heap().copy_array_range(
            *keys, 0U, *new_keys, 0U, static_cast<usize>(*size));
        auto copied_values = machine.heap().copy_array_range(
            *values, 0U, *new_values, 0U, static_cast<usize>(*size));
        auto copied_hashes = machine.heap().copy_array_range(
            *hashes, 0U, *new_hashes, 0U, static_cast<usize>(*size));
        if (!copied_keys) return copied_keys;
        if (!copied_values) return copied_values;
        if (!copied_hashes) return copied_hashes;
    }
    constexpr std::array<usize, 3> storage_fields {
        kHashMapKeysField,
        kHashMapValuesField,
        kHashMapHashesField,
    };
    const std::array<Value, 3> storage_values {
        Value::from_reference(*new_keys),
        Value::from_reference(*new_values),
        Value::from_reference(*new_hashes),
    };
    return set_field_values(machine, map, storage_fields, storage_values);
}

[[nodiscard]] Result<ObjectRef> hash_map_put(Machine& machine,
                                             ObjectRef map,
                                             ObjectRef key,
                                             ObjectRef value) {
    auto hash = hash_map_key_hash(machine, key);
    if (!hash) return std::unexpected(hash.error());
    auto index = map_find_key_with_hash(machine, map, key, *hash);
    if (!index) return std::unexpected(index.error());
    if (*index >= 0) {
        auto values = reference_field(machine, map, kHashMapValuesField);
        if (!values || values->is_null()) {
            return fail(ErrorCode::invalid_state,
                        "HashMap values are invalid");
        }
        auto previous_value = machine.heap().element(
            *values, static_cast<usize>(*index));
        if (!previous_value) return std::unexpected(previous_value.error());
        auto previous = previous_value->as_reference();
        if (!previous) return std::unexpected(previous.error());
        auto stored = machine.heap().set_element(
            *values, static_cast<usize>(*index),
            Value::from_reference(value));
        if (!stored) return std::unexpected(stored.error());
        return *previous;
    }
    auto size = int_field(machine, map, kHashMapSizeField);
    if (!size) return std::unexpected(size.error());
    auto ensured = ensure_hash_map_capacity(machine, map, *size + 1);
    if (!ensured) return std::unexpected(ensured.error());
    constexpr std::array<usize, 3> storage_fields {
        kHashMapKeysField,
        kHashMapValuesField,
        kHashMapHashesField,
    };
    auto storage = field_values(machine, map, storage_fields);
    if (!storage) return std::unexpected(storage.error());
    auto keys = (*storage)[0U].as_reference();
    auto values = (*storage)[1U].as_reference();
    auto hashes = (*storage)[2U].as_reference();
    if (!keys || !values || !hashes || keys->is_null() ||
        values->is_null() || hashes->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "HashMap storage is invalid after growth");
    }
    auto key_stored = machine.heap().set_element(
        *keys, static_cast<usize>(*size), Value::from_reference(key));
    auto value_stored = machine.heap().set_element(
        *values, static_cast<usize>(*size), Value::from_reference(value));
    auto hash_stored = machine.heap().set_element(
        *hashes, static_cast<usize>(*size), Value::from_int(*hash));
    auto size_stored = set_int_field(machine, map, kHashMapSizeField,
                                     *size + 1);
    if (!key_stored) return std::unexpected(key_stored.error());
    if (!value_stored) return std::unexpected(value_stored.error());
    if (!hash_stored) return std::unexpected(hash_stored.error());
    if (!size_stored) return std::unexpected(size_stored.error());
    return ObjectRef {};
}

[[nodiscard]] Result<ObjectRef> hash_map_remove(Machine& machine,
                                                ObjectRef map,
                                                ObjectRef key) {
    auto index = map_find_key(machine, map, key);
    if (!index) return std::unexpected(index.error());
    if (*index < 0) return ObjectRef {};
    constexpr std::array<usize, 4> fields {
        kHashMapKeysField,
        kHashMapValuesField,
        kHashMapSizeField,
        kHashMapHashesField,
    };
    auto state = field_values(machine, map, fields);
    if (!state) return std::unexpected(state.error());
    auto keys = (*state)[0U].as_reference();
    auto values = (*state)[1U].as_reference();
    auto size = (*state)[2U].as_int();
    auto hashes = (*state)[3U].as_reference();
    if (!keys || !values || !hashes || !size || keys->is_null() ||
        values->is_null() || hashes->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "HashMap state is invalid");
    }
    auto previous_value = machine.heap().element(
        *values, static_cast<usize>(*index));
    if (!previous_value) return std::unexpected(previous_value.error());
    auto previous = previous_value->as_reference();
    if (!previous) return std::unexpected(previous.error());
    const usize shifted = static_cast<usize>(*size - *index - 1);
    if (shifted != 0U) {
        const usize source = static_cast<usize>(*index + 1);
        const usize destination = static_cast<usize>(*index);
        auto shifted_keys = machine.heap().copy_array_range(
            *keys, source, *keys, destination, shifted);
        auto shifted_values = machine.heap().copy_array_range(
            *values, source, *values, destination, shifted);
        auto shifted_hashes = machine.heap().copy_array_range(
            *hashes, source, *hashes, destination, shifted);
        if (!shifted_keys) return std::unexpected(shifted_keys.error());
        if (!shifted_values) return std::unexpected(shifted_values.error());
        if (!shifted_hashes) return std::unexpected(shifted_hashes.error());
    }
    auto cleared_key = machine.heap().set_element(
        *keys, static_cast<usize>(*size - 1), Value::from_reference({}));
    auto cleared_value = machine.heap().set_element(
        *values, static_cast<usize>(*size - 1), Value::from_reference({}));
    auto cleared_hash = machine.heap().set_element(
        *hashes, static_cast<usize>(*size - 1), Value::from_int(0));
    auto size_stored = set_int_field(machine, map, kHashMapSizeField,
                                     *size - 1);
    if (!cleared_key) return std::unexpected(cleared_key.error());
    if (!cleared_value) return std::unexpected(cleared_value.error());
    if (!cleared_hash) return std::unexpected(cleared_hash.error());
    if (!size_stored) return std::unexpected(size_stored.error());
    return *previous;
}

[[nodiscard]] Status initialize_hash_set(Machine& machine,
                                         ObjectRef set,
                                         i32 capacity) {
    auto map = machine.class_states().allocate_instance(
        machine.heap(), "java/util/HashMap");
    if (!map) return std::unexpected(map.error());
    auto stored = set_reference_field(machine, set, kHashSetMapField, *map);
    if (!stored) return stored;
    return initialize_hash_map(machine, *map, capacity);
}

[[nodiscard]] Result<ObjectRef> hash_set_map(Machine& machine,
                                             ObjectRef set) {
    auto map = reference_field(machine, set, kHashSetMapField);
    if (!map) return std::unexpected(map.error());
    if (map->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "HashSet map is not initialized");
    }
    return *map;
}

[[nodiscard]] Result<std::vector<std::pair<ObjectRef, ObjectRef>>>
map_entries(Machine& machine, ObjectRef source) {
    auto key_set_value = invoke_checked(
        machine, source, "java/util/Map", "keySet", "()Ljava/util/Set;");
    if (!key_set_value) return std::unexpected(key_set_value.error());
    if (!key_set_value->has_value()) {
        return fail(ErrorCode::internal_error,
                    "Map.keySet returned no value");
    }
    auto key_set = key_set_value->value().as_reference();
    if (!key_set || key_set->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "Map.keySet returned null");
    }
    auto pinned = NativeRootScope::pin(machine.native_roots(), *key_set);
    if (!pinned) return std::unexpected(pinned.error());
    auto keys = collection_elements(machine, *key_set);
    if (!keys) return std::unexpected(keys.error());
    std::vector<std::pair<ObjectRef, ObjectRef>> entries;
    entries.reserve(keys->size());
    for (const ObjectRef key : *keys) {
        const Value argument = Value::from_reference(key);
        auto value_result = invoke_checked(
            machine, source, "java/util/Map", "get",
            "(Ljava/lang/Object;)Ljava/lang/Object;",
            std::span<const Value>(&argument, 1U));
        if (!value_result) return std::unexpected(value_result.error());
        if (!value_result->has_value()) {
            return fail(ErrorCode::internal_error,
                        "Map.get returned no value");
        }
        auto value = value_result->value().as_reference();
        if (!value) return std::unexpected(value.error());
        entries.emplace_back(key, *value);
    }
    return entries;
}

[[nodiscard]] Result<ObjectRef> create_array_list(Machine& machine,
                                                   i32 capacity) {
    auto list = machine.class_states().allocate_instance(
        machine.heap(), "java/util/ArrayList");
    if (!list) return std::unexpected(list.error());
    auto pinned = NativeRootScope::pin(machine.native_roots(), *list);
    if (!pinned) return std::unexpected(pinned.error());
    auto initialized = initialize_array_list(machine, *list, capacity);
    if (!initialized) return std::unexpected(initialized.error());
    return *list;
}

[[nodiscard]] Status array_list_copy_from_array(Machine& machine,
                                                ObjectRef list,
                                                ObjectRef source,
                                                i32 size) {
    if (size < 0) {
        return fail(ErrorCode::invalid_state,
                    "ArrayList snapshot size is negative");
    }
    auto data = reference_field(machine, list, kArrayListDataField);
    if (!data || data->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "ArrayList snapshot storage is invalid");
    }
    if (size > 0) {
        auto copied = machine.heap().copy_array_range(
            source, 0U, *data, 0U, static_cast<usize>(size));
        if (!copied) return std::unexpected(copied.error());
    }
    return set_int_field(machine, list, kArrayListSizeField, size);
}

[[nodiscard]] Result<ObjectRef> create_hash_set(Machine& machine,
                                                 i32 capacity) {
    auto set = machine.class_states().allocate_instance(
        machine.heap(), "java/util/HashSet");
    if (!set) return std::unexpected(set.error());
    auto pinned = NativeRootScope::pin(machine.native_roots(), *set);
    if (!pinned) return std::unexpected(pinned.error());
    auto initialized = initialize_hash_set(machine, *set, capacity);
    if (!initialized) return std::unexpected(initialized.error());
    return *set;
}

[[nodiscard]] Result<i32> list_size(Machine& machine, ObjectRef list) {
    auto result = invoke_checked(machine, list, "java/util/List", "size", "()I");
    if (!result) return std::unexpected(result.error());
    if (!result->has_value()) {
        return fail(ErrorCode::internal_error, "List.size returned no value");
    }
    return result->value().as_int();
}

[[nodiscard]] Result<ObjectRef> list_get(Machine& machine,
                                         ObjectRef list,
                                         i32 index) {
    const Value argument = Value::from_int(index);
    auto result = invoke_checked(machine, list, "java/util/List", "get",
                                 "(I)Ljava/lang/Object;",
                                 std::span<const Value>(&argument, 1U));
    if (!result) return std::unexpected(result.error());
    if (!result->has_value()) {
        return fail(ErrorCode::internal_error, "List.get returned no value");
    }
    return result->value().as_reference();
}

[[nodiscard]] Result<ObjectRef> list_set(Machine& machine,
                                         ObjectRef list,
                                         i32 index,
                                         ObjectRef value) {
    const std::array<Value, 2> arguments {
        Value::from_int(index), Value::from_reference(value),
    };
    auto result = invoke_checked(machine, list, "java/util/List", "set",
                                 "(ILjava/lang/Object;)Ljava/lang/Object;",
                                 arguments);
    if (!result) return std::unexpected(result.error());
    if (!result->has_value()) {
        return fail(ErrorCode::internal_error, "List.set returned no value");
    }
    return result->value().as_reference();
}

[[nodiscard]] Result<i32> compare_objects(Machine& machine,
                                          ObjectRef left,
                                          ObjectRef right) {
    if (left.is_null() || right.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Collections.sort does not accept null elements");
    }
    const Value argument = Value::from_reference(right);
    auto result = invoke_checked(machine, left, "java/lang/Comparable",
                                 "compareTo", "(Ljava/lang/Object;)I",
                                 std::span<const Value>(&argument, 1U));
    if (!result) return std::unexpected(result.error());
    if (!result->has_value()) {
        return fail(ErrorCode::internal_error,
                    "Comparable.compareTo returned no value");
    }
    return result->value().as_int();
}

[[nodiscard]] Result<std::vector<u8>> read_bytes(Machine& machine,
                                                  ObjectRef array) {
    if (array.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Base64 input is null");
    }
    auto length = machine.heap().array_length(array);
    if (!length) return std::unexpected(length.error());
    return machine.heap().read_byte_array(array, 0U, *length);
}

[[nodiscard]] Result<ObjectRef> byte_array_from_bytes(
    Machine& machine,
    std::span<const u8> bytes) {
    auto array = allocate_byte_array(machine, bytes.size());
    if (!array) return std::unexpected(array.error());
    auto written = machine.heap().write_byte_array(*array, 0U, bytes);
    if (!written) return std::unexpected(written.error());
    return *array;
}

[[nodiscard]] std::vector<u8> base64_encode(std::span<const u8> input) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<u8> output;
    output.reserve(((input.size() + 2U) / 3U) * 4U);
    for (usize index = 0U; index < input.size(); index += 3U) {
        const u32 first = input[index];
        const u32 second = index + 1U < input.size() ? input[index + 1U] : 0U;
        const u32 third = index + 2U < input.size() ? input[index + 2U] : 0U;
        const u32 combined = (first << 16U) | (second << 8U) | third;
        output.push_back(static_cast<u8>(alphabet[(combined >> 18U) & 0x3FU]));
        output.push_back(static_cast<u8>(alphabet[(combined >> 12U) & 0x3FU]));
        output.push_back(index + 1U < input.size()
            ? static_cast<u8>(alphabet[(combined >> 6U) & 0x3FU])
            : static_cast<u8>('='));
        output.push_back(index + 2U < input.size()
            ? static_cast<u8>(alphabet[combined & 0x3FU])
            : static_cast<u8>('='));
    }
    return output;
}

[[nodiscard]] i32 base64_value(u8 character) noexcept {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '+') return 62;
    if (character == '/') return 63;
    return -1;
}

[[nodiscard]] Result<std::vector<u8>> base64_decode(
    std::span<const u8> input) {
    if (input.empty()) return std::vector<u8> {};
    if ((input.size() % 4U) != 0U) {
        return fail_java("java/lang/IllegalArgumentException",
                         "invalid Base64 input length");
    }
    std::vector<u8> output;
    output.reserve((input.size() / 4U) * 3U);
    for (usize index = 0U; index < input.size(); index += 4U) {
        const bool padding2 = input[index + 2U] == '=';
        const bool padding3 = input[index + 3U] == '=';
        if (padding2 && !padding3) {
            return fail_java("java/lang/IllegalArgumentException",
                             "invalid Base64 padding");
        }
        if ((padding2 || padding3) && index + 4U != input.size()) {
            return fail_java("java/lang/IllegalArgumentException",
                             "Base64 padding must be final");
        }
        const i32 a = base64_value(input[index]);
        const i32 b = base64_value(input[index + 1U]);
        const i32 c = padding2 ? 0 : base64_value(input[index + 2U]);
        const i32 d = padding3 ? 0 : base64_value(input[index + 3U]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            return fail_java("java/lang/IllegalArgumentException",
                             "invalid Base64 character");
        }
        const u32 combined = (static_cast<u32>(a) << 18U) |
                             (static_cast<u32>(b) << 12U) |
                             (static_cast<u32>(c) << 6U) |
                             static_cast<u32>(d);
        output.push_back(static_cast<u8>((combined >> 16U) & 0xFFU));
        if (!padding2) {
            output.push_back(static_cast<u8>((combined >> 8U) & 0xFFU));
        }
        if (!padding3) {
            output.push_back(static_cast<u8>(combined & 0xFFU));
        }
    }
    return output;
}

void register_comparable_bridges(NativeMethodRegistry& registry) {
    const auto bridge = [&registry](const char* owner,
                                    const char* typed_descriptor) {
        add(registry, owner, "compareTo", "(Ljava/lang/Object;)I",
            [owner, typed_descriptor](Machine& machine,
                                      std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                auto other = reference_argument(arguments, 1U);
                if (!object) return std::unexpected(object.error());
                if (!other) return std::unexpected(other.error());
                auto other_class = machine.heap().class_name(*other);
                if (!other_class) return std::unexpected(other_class.error());
                if (*other_class != owner) {
                    return fail_java("java/lang/ClassCastException",
                                     "Comparable target has the wrong type");
                }
                const Value argument = Value::from_reference(*other);
                auto result = invoke_checked(
                    machine, *object, owner, "compareTo", typed_descriptor,
                    std::span<const Value>(&argument, 1U));
                if (!result) return std::unexpected(result.error());
                if (!result->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "typed compareTo returned no value");
                }
                auto comparison = result->value().as_int();
                if (!comparison) return std::unexpected(comparison.error());
                return std::optional<Value>(Value::from_int(*comparison));
            });
    };
    bridge("java/lang/String", "(Ljava/lang/String;)I");
    bridge("java/lang/Integer", "(Ljava/lang/Integer;)I");
    bridge("java/lang/Long", "(Ljava/lang/Long;)I");
}

void register_objects_extensions(NativeMethodRegistry& registry) {
    add(registry, "java/util/Objects", "requireNonNull",
        "(Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = reference_argument(arguments, 0U, true);
            auto message = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!message) return std::unexpected(message.error());
            if (object->is_null()) {
                auto text = utf8_text(machine, *message);
                if (!text) return std::unexpected(text.error());
                return fail_java("java/lang/NullPointerException",
                                 std::move(*text));
            }
            return std::optional<Value>(Value::from_reference(*object));
        });
    add(registry, "java/util/Objects", "requireNonNull",
        "(Ljava/lang/Object;Ljava/util/function/Supplier;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = reference_argument(arguments, 0U, true);
            auto supplier = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!supplier) return std::unexpected(supplier.error());
            if (!object->is_null()) {
                return std::optional<Value>(Value::from_reference(*object));
            }
            if (supplier->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "message supplier is null");
            }
            auto supplied = invoke_checked(
                machine, *supplier, "java/util/function/Supplier", "get",
                "()Ljava/lang/Object;");
            if (!supplied) return std::unexpected(supplied.error());
            if (!supplied->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Supplier.get returned no value");
            }
            auto message = supplied->value().as_reference();
            if (!message) return std::unexpected(message.error());
            if (message->is_null()) {
                return fail_java("java/lang/NullPointerException", "");
            }
            auto text = utf8_text(machine, *message);
            if (!text) return std::unexpected(text.error());
            return fail_java("java/lang/NullPointerException", std::move(*text));
        });
    add(registry, "java/util/Objects", "equals",
        "(Ljava/lang/Object;Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto left = reference_argument(arguments, 0U, true);
            auto right = reference_argument(arguments, 1U, true);
            if (!left) return std::unexpected(left.error());
            if (!right) return std::unexpected(right.error());
            auto equal = values_equal(machine, *left, *right);
            if (!equal) return std::unexpected(equal.error());
            return std::optional<Value>(Value::from_int(*equal ? 1 : 0));
        });
    add(registry, "java/util/Objects", "deepEquals",
        "(Ljava/lang/Object;Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto left = reference_argument(arguments, 0U, true);
            auto right = reference_argument(arguments, 1U, true);
            if (!left) return std::unexpected(left.error());
            if (!right) return std::unexpected(right.error());
            auto equal = deep_values_equal(machine, *left, *right);
            if (!equal) return std::unexpected(equal.error());
            return std::optional<Value>(Value::from_int(*equal ? 1 : 0));
        });
    add(registry, "java/util/Objects", "compare",
        "(Ljava/lang/Object;Ljava/lang/Object;Ljava/util/Comparator;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto left = reference_argument(arguments, 0U, true);
            auto right = reference_argument(arguments, 1U, true);
            if (!left) return std::unexpected(left.error());
            if (!right) return std::unexpected(right.error());
            if (*left == *right) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto comparator = reference_argument(arguments, 2U);
            if (!comparator) return std::unexpected(comparator.error());
            const std::array<Value, 2> callback_arguments {
                Value::from_reference(*left), Value::from_reference(*right),
            };
            auto result = invoke_checked(
                machine, *comparator, "java/util/Comparator", "compare",
                "(Ljava/lang/Object;Ljava/lang/Object;)I", callback_arguments);
            if (!result) return std::unexpected(result.error());
            if (!result->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Comparator.compare returned no value");
            }
            return result->value();
        });
    const auto register_null_predicate = [&registry](const char* name,
                                                      bool expected_null) {
        add(registry, "java/util/Objects", name, "(Ljava/lang/Object;)Z",
            [expected_null](Machine&, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = reference_argument(arguments, 0U, true);
                if (!object) return std::unexpected(object.error());
                const bool is_null = object->is_null();
                return std::optional<Value>(Value::from_int(
                    is_null == expected_null ? 1 : 0));
            });
    };
    register_null_predicate("isNull", true);
    register_null_predicate("nonNull", false);
    add(registry, "java/util/Objects", "hashCode",
        "(Ljava/lang/Object;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = reference_argument(arguments, 0U, true);
            if (!object) return std::unexpected(object.error());
            if (object->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto result = invoke_checked(machine, *object,
                "java/lang/Object", "hashCode", "()I");
            if (!result) return std::unexpected(result.error());
            if (!result->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Object.hashCode returned no value");
            }
            return result->value();
        });
    const auto register_to_string = [&registry](bool with_fallback) {
        add(registry, "java/util/Objects", "toString",
            with_fallback
                ? "(Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/String;"
                : "(Ljava/lang/Object;)Ljava/lang/String;",
            [with_fallback](Machine& machine,
                            std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = reference_argument(arguments, 0U, true);
                if (!object) return std::unexpected(object.error());
                if (object->is_null()) {
                    if (with_fallback) {
                        auto fallback = reference_argument(arguments, 1U, true);
                        if (!fallback) return std::unexpected(fallback.error());
                        return std::optional<Value>(
                            Value::from_reference(*fallback));
                    }
                    auto text = create_string(machine, std::u16string(u"null"));
                    if (!text) return std::unexpected(text.error());
                    return std::optional<Value>(Value::from_reference(*text));
                }
                auto result = invoke_checked(machine, *object,
                    "java/lang/Object", "toString", "()Ljava/lang/String;");
                if (!result) return std::unexpected(result.error());
                if (!result->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "Object.toString returned no value");
                }
                return result->value();
            });
    };
    register_to_string(false);
    register_to_string(true);
}

void register_array_iterator(NativeMethodRegistry& registry) {
    add(registry, "java/util/ArrayIterator", "hasNext", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 2> fields {
                kIteratorIndexField,
                kIteratorSizeField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto index = (*state)[0U].as_int();
            auto size = (*state)[1U].as_int();
            if (!index || !size) {
                return fail(ErrorCode::invalid_state,
                            "ArrayIterator state is invalid");
            }
            return std::optional<Value>(Value::from_int(
                *index < *size ? 1 : 0));
        });
    add(registry, "java/util/ArrayIterator", "next",
        "()Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 3> fields {
                kIteratorValuesField,
                kIteratorIndexField,
                kIteratorSizeField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto values = (*state)[0U].as_reference();
            auto index = (*state)[1U].as_int();
            auto size = (*state)[2U].as_int();
            if (!values || !index || !size || values->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "ArrayIterator state is invalid");
            }
            if (*index >= *size) {
                return fail_java("java/util/NoSuchElementException",
                                 "iterator has no more elements");
            }
            auto value = machine.heap().element(
                *values, static_cast<usize>(*index));
            if (!value) return std::unexpected(value.error());
            constexpr std::array<usize, 2> update_fields {
                kIteratorLastReturnedField,
                kIteratorIndexField,
            };
            const std::array<Value, 2> update_values {
                Value::from_int(*index),
                Value::from_int(*index + 1),
            };
            auto updated = set_field_values(
                machine, *object, update_fields, update_values);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value>(*value);
        });
    add(registry, "java/util/ArrayIterator", "remove", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 4> state_fields {
                kIteratorRemoveKindField,
                kIteratorLastReturnedField,
                kIteratorSizeField,
                kIteratorIndexField,
            };
            auto state = field_values(machine, *object, state_fields);
            if (!state) return std::unexpected(state.error());
            auto kind = (*state)[0U].as_int();
            auto last = (*state)[1U].as_int();
            auto size = (*state)[2U].as_int();
            auto index = (*state)[3U].as_int();
            if (!kind || !last || !size || !index) {
                return fail(ErrorCode::invalid_state,
                            "ArrayIterator remove state is invalid");
            }
            if (*kind == kIteratorRemoveUnsupported) {
                return fail_java("java/lang/UnsupportedOperationException",
                                 "iterator does not support remove");
            }
            if (*last < 0) {
                return fail_java("java/lang/IllegalStateException",
                                 "next() has not been called or element already removed");
            }
            if (*kind != kIteratorRemoveArrayList) {
                return fail_java("java/lang/UnsupportedOperationException",
                                 "iterator remove kind is unsupported");
            }
            constexpr std::array<usize, 2> reference_fields {
                kIteratorOwnerField,
                kIteratorValuesField,
            };
            auto references = field_values(
                machine, *object, reference_fields);
            if (!references) return std::unexpected(references.error());
            auto owner = (*references)[0U].as_reference();
            auto values = (*references)[1U].as_reference();
            if (!owner || owner->is_null() || !values || values->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "ArrayList iterator owner is invalid");
            }
            auto owner_size = int_field(machine, *owner,
                                        kArrayListSizeField);
            if (!owner_size || *owner_size != *size || *last >= *size) {
                return fail_java("java/util/ConcurrentModificationException",
                                 "ArrayList changed during iteration");
            }
            const usize shifted = static_cast<usize>(*size - *last - 1);
            if (shifted != 0U) {
                auto copied = machine.heap().copy_array_range(
                    *values, static_cast<usize>(*last + 1),
                    *values, static_cast<usize>(*last), shifted);
                if (!copied) return std::unexpected(copied.error());
            }
            auto cleared = machine.heap().set_element(
                *values, static_cast<usize>(*size - 1),
                Value::from_reference({}));
            auto owner_updated = set_int_field(machine, *owner,
                                               kArrayListSizeField,
                                               *size - 1);
            constexpr std::array<usize, 3> update_fields {
                kIteratorSizeField,
                kIteratorIndexField,
                kIteratorLastReturnedField,
            };
            const std::array<Value, 3> update_values {
                Value::from_int(*size - 1),
                Value::from_int(*last),
                Value::from_int(-1),
            };
            auto iterator_updated = set_field_values(
                machine, *object, update_fields, update_values);
            if (!cleared) return std::unexpected(cleared.error());
            if (!owner_updated) return std::unexpected(owner_updated.error());
            if (!iterator_updated) {
                return std::unexpected(iterator_updated.error());
            }
            return std::optional<Value> {};
        });
}

void register_array_list_extensions(NativeMethodRegistry& registry) {
    add(registry, "java/util/ArrayList", "<init>",
        "(Ljava/util/Collection;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto source = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!source) return std::unexpected(source.error());
            auto elements = collection_elements(machine, *source);
            if (!elements) return std::unexpected(elements.error());
            auto initialized = initialize_array_list(
                machine, *object, static_cast<i32>(elements->size()));
            if (!initialized) return std::unexpected(initialized.error());
            for (const ObjectRef element : *elements) {
                auto appended = array_list_append(machine, *object, element);
                if (!appended) return std::unexpected(appended.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "java/util/ArrayList", "iterator",
        "()Ljava/util/Iterator;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 3> fields {
                kArrayListDataField,
                kArrayListSizeField,
                kArrayListMutationModeField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto data = (*state)[0U].as_reference();
            auto size = (*state)[1U].as_int();
            auto mode = (*state)[2U].as_int();
            if (!data || data->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "ArrayList data is not initialized");
            }
            if (!size) return std::unexpected(size.error());
            if (!mode) return std::unexpected(mode.error());
            const i32 remove_kind = *mode == kArrayListMutable
                ? kIteratorRemoveArrayList
                : kIteratorRemoveUnsupported;
            auto iterator = create_array_iterator(
                machine, *data, *size, *object, remove_kind);
            if (!iterator) return std::unexpected(iterator.error());
            return std::optional<Value>(Value::from_reference(*iterator));
        });
    add(registry, "java/util/ArrayList", "toArray",
        "()[Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 2> fields {
                kArrayListDataField,
                kArrayListSizeField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto data = (*state)[0U].as_reference();
            auto size = (*state)[1U].as_int();
            if (!data || data->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "ArrayList data is not initialized");
            }
            if (!size) return std::unexpected(size.error());
            auto result = allocate_object_array(machine,
                                                static_cast<usize>(*size));
            if (!result) return std::unexpected(result.error());
            if (*size > 0) {
                auto copied = machine.heap().copy_array_range(
                    *data, 0U, *result, 0U, static_cast<usize>(*size));
                if (!copied) return std::unexpected(copied.error());
            }
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/util/ArrayList", "toArray",
        "([Ljava/lang/Object;)[Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto supplied = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!supplied) return std::unexpected(supplied.error());
            constexpr std::array<usize, 2> fields {
                kArrayListDataField,
                kArrayListSizeField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto data = (*state)[0U].as_reference();
            auto size = (*state)[1U].as_int();
            auto supplied_size = machine.heap().array_length(*supplied);
            auto supplied_class = machine.heap().class_name(*supplied);
            if (!data || data->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "ArrayList data is not initialized");
            }
            if (!size) return std::unexpected(size.error());
            if (!supplied_size) return std::unexpected(supplied_size.error());
            if (!supplied_class) return std::unexpected(supplied_class.error());
            ObjectRef result = *supplied;
            if (*supplied_size < static_cast<usize>(*size)) {
                auto allocated = machine.heap().allocate_array(
                    *supplied_class, static_cast<usize>(*size),
                    Value::from_reference({}));
                if (!allocated) return std::unexpected(allocated.error());
                result = *allocated;
            }
            if (*size > 0) {
                auto copied = machine.heap().copy_array_range(
                    *data, 0U, result, 0U, static_cast<usize>(*size));
                if (!copied) return std::unexpected(copied.error());
            }
            if (*supplied_size > static_cast<usize>(*size) &&
                result == *supplied) {
                auto terminated = machine.heap().set_element(
                    result, static_cast<usize>(*size),
                    Value::from_reference({}));
                if (!terminated) return std::unexpected(terminated.error());
            }
            return std::optional<Value>(Value::from_reference(result));
        });
    add(registry, "java/util/ArrayList", "addAll",
        "(Ljava/util/Collection;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto source = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!source) return std::unexpected(source.error());
            auto mode = int_field(machine, *object, kArrayListMutationModeField);
            if (!mode) return std::unexpected(mode.error());
            if (*mode != kArrayListMutable) {
                return fail_java("java/lang/UnsupportedOperationException",
                                 "list does not support structural modification");
            }
            auto elements = collection_elements(machine, *source);
            if (!elements) return std::unexpected(elements.error());
            for (const ObjectRef element : *elements) {
                auto appended = array_list_append(machine, *object, element);
                if (!appended) return std::unexpected(appended.error());
            }
            return std::optional<Value>(Value::from_int(
                elements->empty() ? 0 : 1));
        });

    const auto list_of = [&registry](const char* descriptor, usize count) {
        add(registry, "java/util/List", "of", descriptor,
            [count](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                if (arguments.size() != count) {
                    return fail(ErrorCode::invalid_argument,
                                "List.of argument count is invalid");
                }
                auto list = create_array_list(machine, static_cast<i32>(count));
                if (!list) return std::unexpected(list.error());
                auto root = NativeRootScope::pin(machine.native_roots(), *list);
                if (!root) return std::unexpected(root.error());
                for (usize index = 0U; index < count; ++index) {
                    auto value = arguments[index].as_reference();
                    if (!value) return std::unexpected(value.error());
                    if (value->is_null()) {
                        return fail_java("java/lang/NullPointerException",
                                         "List.of element is null");
                    }
                    auto appended = array_list_append(machine, *list, *value);
                    if (!appended) return std::unexpected(appended.error());
                }
                auto immutable = set_int_field(
                    machine, *list, kArrayListMutationModeField,
                    kArrayListImmutable);
                if (!immutable) return std::unexpected(immutable.error());
                return std::optional<Value>(Value::from_reference(*list));
            });
    };
    list_of("(Ljava/lang/Object;)Ljava/util/List;", 1U);
    list_of("(Ljava/lang/Object;Ljava/lang/Object;)Ljava/util/List;", 2U);
    list_of("(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Ljava/util/List;", 3U);

    add(registry, "java/util/List", "copyOf",
        "(Ljava/util/Collection;)Ljava/util/List;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto source = reference_argument(arguments, 0U);
            if (!source) return std::unexpected(source.error());
            auto elements = collection_elements(machine, *source);
            if (!elements) return std::unexpected(elements.error());
            auto list = create_array_list(
                machine, static_cast<i32>(elements->size()));
            if (!list) return std::unexpected(list.error());
            auto root = NativeRootScope::pin(machine.native_roots(), *list);
            if (!root) return std::unexpected(root.error());
            for (const ObjectRef element : *elements) {
                if (element.is_null()) {
                    return fail_java("java/lang/NullPointerException",
                                     "List.copyOf element is null");
                }
                auto appended = array_list_append(machine, *list, element);
                if (!appended) return std::unexpected(appended.error());
            }
            auto immutable = set_int_field(
                machine, *list, kArrayListMutationModeField,
                kArrayListImmutable);
            if (!immutable) return std::unexpected(immutable.error());
            return std::optional<Value>(Value::from_reference(*list));
        });
}

void register_hash_map(NativeMethodRegistry& registry) {
    const auto constructor = [&registry](const char* descriptor,
                                         i32 default_capacity) {
        add(registry, "java/util/HashMap", "<init>", descriptor,
            [descriptor, default_capacity](Machine& machine,
                                           std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                i32 capacity = default_capacity;
                if (std::string_view(descriptor) == "(I)V") {
                    auto requested = int_argument(arguments, 1U);
                    if (!requested) return std::unexpected(requested.error());
                    capacity = *requested;
                }
                auto initialized = initialize_hash_map(machine, *object,
                                                       capacity);
                if (!initialized) return std::unexpected(initialized.error());
                return std::optional<Value> {};
            });
    };
    constructor("()V", 16);
    constructor("(I)V", 0);

    add(registry, "java/util/HashMap", "<init>", "(Ljava/util/Map;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto source = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!source) return std::unexpected(source.error());
            auto entries = map_entries(machine, *source);
            if (!entries) return std::unexpected(entries.error());
            auto initialized = initialize_hash_map(
                machine, *object, static_cast<i32>(entries->size() * 2U));
            if (!initialized) return std::unexpected(initialized.error());
            for (const auto& [key, value] : *entries) {
                auto stored = hash_map_put(machine, *object, key, value);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "java/util/HashMap", "size", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto size = int_field(machine, *object, kHashMapSizeField);
            if (!size) return std::unexpected(size.error());
            return std::optional<Value>(Value::from_int(*size));
        });
    add(registry, "java/util/HashMap", "isEmpty", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto size = int_field(machine, *object, kHashMapSizeField);
            if (!size) return std::unexpected(size.error());
            return std::optional<Value>(Value::from_int(*size == 0 ? 1 : 0));
        });
    add(registry, "java/util/HashMap", "containsKey",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto key = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!key) return std::unexpected(key.error());
            auto index = map_find_key(machine, *object, *key);
            if (!index) return std::unexpected(index.error());
            return std::optional<Value>(Value::from_int(*index >= 0 ? 1 : 0));
        });
    add(registry, "java/util/HashMap", "containsValue",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto target = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!target) return std::unexpected(target.error());
            constexpr std::array<usize, 2> fields {
                kHashMapValuesField,
                kHashMapSizeField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto values = (*state)[0U].as_reference();
            auto size = (*state)[1U].as_int();
            if (!values || !size || values->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "HashMap state is invalid");
            }
            for (i32 index = 0; index < *size; ++index) {
                auto value = machine.heap().element(
                    *values, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto reference = value->as_reference();
                if (!reference) return std::unexpected(reference.error());
                auto equal = values_equal(machine, *reference, *target);
                if (!equal) return std::unexpected(equal.error());
                if (*equal) {
                    return std::optional<Value>(Value::from_int(1));
                }
            }
            return std::optional<Value>(Value::from_int(0));
        });
    add(registry, "java/util/HashMap", "get",
        "(Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto key = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!key) return std::unexpected(key.error());
            auto index = map_find_key(machine, *object, *key);
            if (!index) return std::unexpected(index.error());
            if (*index < 0) {
                return std::optional<Value>(Value::from_reference({}));
            }
            auto values = reference_field(machine, *object,
                                          kHashMapValuesField);
            if (!values || values->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "HashMap values are invalid");
            }
            auto value = machine.heap().element(
                *values, static_cast<usize>(*index));
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(*value);
        });
    add(registry, "java/util/HashMap", "getOrDefault",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto key = reference_argument(arguments, 1U, true);
            auto fallback = reference_argument(arguments, 2U, true);
            if (!object) return std::unexpected(object.error());
            if (!key) return std::unexpected(key.error());
            if (!fallback) return std::unexpected(fallback.error());
            auto index = map_find_key(machine, *object, *key);
            if (!index) return std::unexpected(index.error());
            if (*index < 0) {
                return std::optional<Value>(Value::from_reference(*fallback));
            }
            auto values = reference_field(machine, *object,
                                          kHashMapValuesField);
            if (!values || values->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "HashMap values are invalid");
            }
            auto value = machine.heap().element(
                *values, static_cast<usize>(*index));
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(*value);
        });
    add(registry, "java/util/HashMap", "putIfAbsent",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto key = reference_argument(arguments, 1U, true);
            auto value = reference_argument(arguments, 2U, true);
            if (!object) return std::unexpected(object.error());
            if (!key) return std::unexpected(key.error());
            if (!value) return std::unexpected(value.error());
            auto index = map_find_key(machine, *object, *key);
            if (!index) return std::unexpected(index.error());
            if (*index >= 0) {
                auto values = reference_field(machine, *object,
                                              kHashMapValuesField);
                if (!values || values->is_null()) {
                    return fail(ErrorCode::invalid_state,
                                "HashMap values are invalid");
                }
                auto current = machine.heap().element(
                    *values, static_cast<usize>(*index));
                if (!current) return std::unexpected(current.error());
                auto current_reference = current->as_reference();
                if (!current_reference) {
                    return std::unexpected(current_reference.error());
                }
                if (!current_reference->is_null()) {
                    return std::optional<Value>(*current);
                }
            }
            auto previous = hash_map_put(machine, *object, *key, *value);
            if (!previous) return std::unexpected(previous.error());
            return std::optional<Value>(Value::from_reference(*previous));
        });
    add(registry, "java/util/HashMap", "computeIfAbsent",
        "(Ljava/lang/Object;Ljava/util/function/Function;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto key = reference_argument(arguments, 1U, true);
            auto function = reference_argument(arguments, 2U);
            if (!object) return std::unexpected(object.error());
            if (!key) return std::unexpected(key.error());
            if (!function) return std::unexpected(function.error());
            auto index = map_find_key(machine, *object, *key);
            if (!index) return std::unexpected(index.error());
            if (*index >= 0) {
                auto values = reference_field(machine, *object,
                                              kHashMapValuesField);
                if (!values || values->is_null()) {
                    return fail(ErrorCode::invalid_state,
                                "HashMap values are invalid");
                }
                auto current = machine.heap().element(
                    *values, static_cast<usize>(*index));
                if (!current) return std::unexpected(current.error());
                auto current_reference = current->as_reference();
                if (!current_reference) {
                    return std::unexpected(current_reference.error());
                }
                if (!current_reference->is_null()) {
                    return std::optional<Value>(*current);
                }
            }
            const Value key_argument = Value::from_reference(*key);
            auto computed = invoke_checked(
                machine, *function, "java/util/function/Function", "apply",
                "(Ljava/lang/Object;)Ljava/lang/Object;",
                std::span<const Value>(&key_argument, 1U));
            if (!computed) return std::unexpected(computed.error());
            if (!computed->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Function.apply returned no value");
            }
            auto computed_reference = computed->value().as_reference();
            if (!computed_reference) {
                return std::unexpected(computed_reference.error());
            }
            if (!computed_reference->is_null()) {
                auto stored = hash_map_put(
                    machine, *object, *key, *computed_reference);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value>(
                Value::from_reference(*computed_reference));
        });
    add(registry, "java/util/HashMap", "merge",
        "(Ljava/lang/Object;Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto key = reference_argument(arguments, 1U, true);
            auto value = reference_argument(arguments, 2U);
            auto function = reference_argument(arguments, 3U);
            if (!object) return std::unexpected(object.error());
            if (!key) return std::unexpected(key.error());
            if (!value) return std::unexpected(value.error());
            if (!function) return std::unexpected(function.error());
            auto index = map_find_key(machine, *object, *key);
            if (!index) return std::unexpected(index.error());
            ObjectRef merged = *value;
            if (*index >= 0) {
                auto values = reference_field(machine, *object,
                                              kHashMapValuesField);
                if (!values || values->is_null()) {
                    return fail(ErrorCode::invalid_state,
                                "HashMap values are invalid");
                }
                auto current = machine.heap().element(
                    *values, static_cast<usize>(*index));
                if (!current) return std::unexpected(current.error());
                auto current_reference = current->as_reference();
                if (!current_reference) {
                    return std::unexpected(current_reference.error());
                }
                if (!current_reference->is_null()) {
                    const std::array<Value, 2> callback_arguments {
                        Value::from_reference(*current_reference),
                        Value::from_reference(*value),
                    };
                    auto combined = invoke_checked(
                        machine, *function, "java/util/function/BiFunction",
                        "apply",
                        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                        callback_arguments);
                    if (!combined) return std::unexpected(combined.error());
                    if (!combined->has_value()) {
                        return fail(ErrorCode::internal_error,
                                    "BiFunction.apply returned no value");
                    }
                    auto combined_reference =
                        combined->value().as_reference();
                    if (!combined_reference) {
                        return std::unexpected(combined_reference.error());
                    }
                    merged = *combined_reference;
                }
            }
            if (merged.is_null()) {
                auto removed = hash_map_remove(machine, *object, *key);
                if (!removed) return std::unexpected(removed.error());
            } else {
                auto stored = hash_map_put(machine, *object, *key, merged);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value>(Value::from_reference(merged));
        });
    add(registry, "java/util/HashMap", "put",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto key = reference_argument(arguments, 1U, true);
            auto value = reference_argument(arguments, 2U, true);
            if (!object) return std::unexpected(object.error());
            if (!key) return std::unexpected(key.error());
            if (!value) return std::unexpected(value.error());
            auto previous = hash_map_put(machine, *object, *key, *value);
            if (!previous) return std::unexpected(previous.error());
            return std::optional<Value>(Value::from_reference(*previous));
        });
    add(registry, "java/util/HashMap", "remove",
        "(Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto key = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!key) return std::unexpected(key.error());
            auto previous = hash_map_remove(machine, *object, *key);
            if (!previous) return std::unexpected(previous.error());
            return std::optional<Value>(Value::from_reference(*previous));
        });
    add(registry, "java/util/HashMap", "putAll", "(Ljava/util/Map;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto source = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!source) return std::unexpected(source.error());
            auto entries = map_entries(machine, *source);
            if (!entries) return std::unexpected(entries.error());
            for (const auto& [key, value] : *entries) {
                auto stored = hash_map_put(machine, *object, key, value);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "java/util/HashMap", "clear", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 4> fields {
                kHashMapKeysField,
                kHashMapValuesField,
                kHashMapSizeField,
                kHashMapHashesField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto keys = (*state)[0U].as_reference();
            auto values = (*state)[1U].as_reference();
            auto size = (*state)[2U].as_int();
            auto hashes = (*state)[3U].as_reference();
            if (!keys || !values || !hashes || !size || keys->is_null() ||
                values->is_null() || hashes->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "HashMap state is invalid");
            }
            if (*size > 0) {
                const usize live = static_cast<usize>(*size);
                auto key_cleared = machine.heap().fill_array_range(
                    *keys, 0U, live, Value::from_reference({}));
                auto value_cleared = machine.heap().fill_array_range(
                    *values, 0U, live, Value::from_reference({}));
                auto hash_cleared = machine.heap().fill_array_range(
                    *hashes, 0U, live, Value::from_int(0));
                if (!key_cleared) return std::unexpected(key_cleared.error());
                if (!value_cleared) return std::unexpected(value_cleared.error());
                if (!hash_cleared) return std::unexpected(hash_cleared.error());
            }
            auto reset = set_int_field(machine, *object, kHashMapSizeField, 0);
            if (!reset) return std::unexpected(reset.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/HashMap", "keySet", "()Ljava/util/Set;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 2> fields {
                kHashMapSizeField,
                kHashMapKeysField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto size = (*state)[0U].as_int();
            auto keys = (*state)[1U].as_reference();
            if (!size || !keys || keys->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "HashMap state is invalid");
            }
            if (*size < 0) {
                return fail(ErrorCode::invalid_state,
                            "HashMap size is negative");
            }
            auto key_values = machine.heap().read_reference_array(*keys);
            if (!key_values) return std::unexpected(key_values.error());
            if (static_cast<usize>(*size) > key_values->size()) {
                return fail(ErrorCode::invalid_state,
                            "HashMap key storage is shorter than its size");
            }
            auto set = create_hash_set(machine, *size * 2);
            if (!set) return std::unexpected(set.error());
            auto map = hash_set_map(machine, *set);
            if (!map) return std::unexpected(map.error());
            for (i32 index = 0; index < *size; ++index) {
                const ObjectRef key =
                    (*key_values)[static_cast<usize>(index)];
                auto stored = hash_map_put(machine, *map, key, key);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value>(Value::from_reference(*set));
        });
    add(registry, "java/util/HashMap", "values",
        "()Ljava/util/Collection;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 2> fields {
                kHashMapSizeField,
                kHashMapValuesField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto size = (*state)[0U].as_int();
            auto values = (*state)[1U].as_reference();
            if (!size || !values || values->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "HashMap state is invalid");
            }
            if (*size < 0) {
                return fail(ErrorCode::invalid_state,
                            "HashMap size is negative");
            }
            auto list = create_array_list(machine, *size);
            if (!list) return std::unexpected(list.error());
            auto copied = array_list_copy_from_array(
                machine, *list, *values, *size);
            if (!copied) return std::unexpected(copied.error());
            return std::optional<Value>(Value::from_reference(*list));
        });
    add(registry, "java/util/HashMap", "toString", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 3> fields {
                kHashMapSizeField,
                kHashMapKeysField,
                kHashMapValuesField,
            };
            auto state = field_values(machine, *object, fields);
            if (!state) return std::unexpected(state.error());
            auto size = (*state)[0U].as_int();
            auto keys = (*state)[1U].as_reference();
            auto values = (*state)[2U].as_reference();
            if (!size || !keys || !values || keys->is_null() || values->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "HashMap state is invalid");
            }
            std::u16string text(u"{");
            for (i32 index = 0; index < *size; ++index) {
                if (index != 0) text.append(u", ");
                auto key_value = machine.heap().element(
                    *keys, static_cast<usize>(index));
                auto mapped_value = machine.heap().element(
                    *values, static_cast<usize>(index));
                if (!key_value) return std::unexpected(key_value.error());
                if (!mapped_value) return std::unexpected(mapped_value.error());
                auto key = key_value->as_reference();
                auto value = mapped_value->as_reference();
                if (!key || !value) {
                    return fail(ErrorCode::invalid_state,
                                "HashMap contains a non-reference value");
                }
                auto key_text = value_text(machine, *key, *object,
                                           u"(this Map)");
                auto mapped_text = value_text(machine, *value, *object,
                                              u"(this Map)");
                if (!key_text) return std::unexpected(key_text.error());
                if (!mapped_text) return std::unexpected(mapped_text.error());
                text.append(*key_text);
                text.push_back(u'=');
                text.append(*mapped_text);
            }
            text.push_back(u'}');
            auto string = create_string(machine, std::move(text));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
}

void register_hash_set(NativeMethodRegistry& registry) {
    const auto constructor = [&registry](const char* descriptor,
                                         i32 default_capacity) {
        add(registry, "java/util/HashSet", "<init>", descriptor,
            [descriptor, default_capacity](
                Machine& machine,
                std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                i32 capacity = default_capacity;
                if (std::string_view(descriptor) == "(I)V") {
                    auto requested = int_argument(arguments, 1U);
                    if (!requested) return std::unexpected(requested.error());
                    capacity = *requested;
                }
                auto initialized = initialize_hash_set(machine, *object,
                                                       capacity);
                if (!initialized) return std::unexpected(initialized.error());
                return std::optional<Value> {};
            });
    };
    constructor("()V", 16);
    constructor("(I)V", 0);
    add(registry, "java/util/HashSet", "<init>",
        "(Ljava/util/Collection;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto source = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!source) return std::unexpected(source.error());
            auto elements = collection_elements(machine, *source);
            if (!elements) return std::unexpected(elements.error());
            auto initialized = initialize_hash_set(
                machine, *object, static_cast<i32>(elements->size() * 2U));
            if (!initialized) return std::unexpected(initialized.error());
            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            for (const ObjectRef element : *elements) {
                auto stored = hash_map_put(machine, *map, element, element);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
    const auto map_size = [&registry](const char* name, bool empty) {
        add(registry, "java/util/HashSet", name, empty ? "()Z" : "()I",
            [empty](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                auto map = hash_set_map(machine, *object);
                if (!map) return std::unexpected(map.error());
                auto size = int_field(machine, *map, kHashMapSizeField);
                if (!size) return std::unexpected(size.error());
                return std::optional<Value>(Value::from_int(
                    empty ? (*size == 0 ? 1 : 0) : *size));
            });
    };
    map_size("size", false);
    map_size("isEmpty", true);
    add(registry, "java/util/HashSet", "contains",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            auto index = map_find_key(machine, *map, *value);
            if (!index) return std::unexpected(index.error());
            return std::optional<Value>(Value::from_int(*index >= 0 ? 1 : 0));
        });
    add(registry, "java/util/HashSet", "add", "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            auto index = map_find_key(machine, *map, *value);
            if (!index) return std::unexpected(index.error());
            if (*index >= 0) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto stored = hash_map_put(machine, *map, *value, *value);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_int(1));
        });
    add(registry, "java/util/HashSet", "remove", "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            auto index = map_find_key(machine, *map, *value);
            if (!index) return std::unexpected(index.error());
            if (*index < 0) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto removed = hash_map_remove(machine, *map, *value);
            if (!removed) return std::unexpected(removed.error());
            return std::optional<Value>(Value::from_int(1));
        });
    add(registry, "java/util/HashSet", "clear", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            constexpr std::array<usize, 4> fields {
                kHashMapKeysField,
                kHashMapValuesField,
                kHashMapHashesField,
                kHashMapSizeField,
            };
            auto state = field_values(machine, *map, fields);
            if (!state) return std::unexpected(state.error());
            auto keys = (*state)[0U].as_reference();
            auto values = (*state)[1U].as_reference();
            auto hashes = (*state)[2U].as_reference();
            auto size = (*state)[3U].as_int();
            if (!keys || !values || !hashes || !size || keys->is_null() ||
                values->is_null() || hashes->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "HashSet state is invalid");
            }
            if (*size > 0) {
                const usize live = static_cast<usize>(*size);
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
            auto reset = set_int_field(machine, *map, kHashMapSizeField, 0);
            if (!reset) return std::unexpected(reset.error());
            return std::optional<Value> {};
        });
    const auto snapshot = [&registry](const char* name,
                                      const char* descriptor,
                                      bool iterator) {
        add(registry, "java/util/HashSet", name, descriptor,
            [iterator](Machine& machine,
                       std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                auto map = hash_set_map(machine, *object);
                if (!map) return std::unexpected(map.error());
                constexpr std::array<usize, 2> fields {
                    kHashMapSizeField,
                    kHashMapKeysField,
                };
                auto state = field_values(machine, *map, fields);
                if (!state) return std::unexpected(state.error());
                auto size = (*state)[0U].as_int();
                auto keys = (*state)[1U].as_reference();
                if (!size || !keys || keys->is_null()) {
                    return fail(ErrorCode::invalid_state,
                                "HashSet state is invalid");
                }
                auto values = allocate_object_array(
                    machine, static_cast<usize>(*size));
                if (!values) return std::unexpected(values.error());
                if (*size > 0) {
                    auto copied = machine.heap().copy_array_range(
                        *keys, 0U, *values, 0U, static_cast<usize>(*size));
                    if (!copied) return std::unexpected(copied.error());
                }
                if (!iterator) {
                    return std::optional<Value>(
                        Value::from_reference(*values));
                }
                auto result = create_array_iterator(machine, *values, *size);
                if (!result) return std::unexpected(result.error());
                return std::optional<Value>(Value::from_reference(*result));
            });
    };
    snapshot("iterator", "()Ljava/util/Iterator;", true);
    snapshot("toArray", "()[Ljava/lang/Object;", false);
    add(registry, "java/util/HashSet", "toArray",
        "([Ljava/lang/Object;)[Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto supplied = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!supplied) return std::unexpected(supplied.error());
            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            constexpr std::array<usize, 2> fields {
                kHashMapSizeField,
                kHashMapKeysField,
            };
            auto state = field_values(machine, *map, fields);
            if (!state) return std::unexpected(state.error());
            auto size = (*state)[0U].as_int();
            auto keys = (*state)[1U].as_reference();
            auto supplied_size = machine.heap().array_length(*supplied);
            auto supplied_class = machine.heap().class_name(*supplied);
            if (!size || !keys || keys->is_null() || !supplied_size ||
                !supplied_class) {
                return fail(ErrorCode::invalid_state,
                            "HashSet toArray state is invalid");
            }
            ObjectRef result = *supplied;
            if (*supplied_size < static_cast<usize>(*size)) {
                auto allocated = machine.heap().allocate_array(
                    *supplied_class, static_cast<usize>(*size),
                    Value::from_reference({}));
                if (!allocated) return std::unexpected(allocated.error());
                result = *allocated;
            }
            if (*size > 0) {
                auto copied = machine.heap().copy_array_range(
                    *keys, 0U, result, 0U, static_cast<usize>(*size));
                if (!copied) return std::unexpected(copied.error());
            }
            if (*supplied_size > static_cast<usize>(*size) &&
                result == *supplied) {
                auto terminated = machine.heap().set_element(
                    result, static_cast<usize>(*size),
                    Value::from_reference({}));
                if (!terminated) return std::unexpected(terminated.error());
            }
            return std::optional<Value>(Value::from_reference(result));
        });
    add(registry, "java/util/HashSet", "containsAll",
        "(Ljava/util/Collection;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto source = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!source) return std::unexpected(source.error());
            auto elements = collection_elements(machine, *source);
            if (!elements) return std::unexpected(elements.error());
            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            for (const ObjectRef element : *elements) {
                auto index = map_find_key(machine, *map, element);
                if (!index) return std::unexpected(index.error());
                if (*index < 0) {
                    return std::optional<Value>(Value::from_int(0));
                }
            }
            return std::optional<Value>(Value::from_int(1));
        });
    add(registry, "java/util/HashSet", "addAll",
        "(Ljava/util/Collection;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto source = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!source) return std::unexpected(source.error());
            auto elements = collection_elements(machine, *source);
            if (!elements) return std::unexpected(elements.error());
            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            bool changed = false;
            for (const ObjectRef element : *elements) {
                auto index = map_find_key(machine, *map, element);
                if (!index) return std::unexpected(index.error());
                if (*index >= 0) continue;
                auto stored = hash_map_put(machine, *map, element, element);
                if (!stored) return std::unexpected(stored.error());
                changed = true;
            }
            return std::optional<Value>(Value::from_int(changed ? 1 : 0));
        });
    add(registry, "java/util/HashSet", "removeAll",
        "(Ljava/util/Collection;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto source = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!source) return std::unexpected(source.error());
            auto elements = collection_elements(machine, *source);
            if (!elements) return std::unexpected(elements.error());
            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            bool changed = false;
            for (const ObjectRef element : *elements) {
                auto index = map_find_key(machine, *map, element);
                if (!index) return std::unexpected(index.error());
                if (*index < 0) continue;
                auto removed = hash_map_remove(machine, *map, element);
                if (!removed) return std::unexpected(removed.error());
                changed = true;
            }
            return std::optional<Value>(Value::from_int(changed ? 1 : 0));
        });
    add(registry, "java/util/HashSet", "retainAll",
        "(Ljava/util/Collection;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto source = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!source) return std::unexpected(source.error());
            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            auto size = int_field(machine, *map, kHashMapSizeField);
            auto keys = reference_field(machine, *map, kHashMapKeysField);
            if (!size || !keys || keys->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "HashSet state is invalid");
            }
            std::vector<ObjectRef> snapshot_values;
            snapshot_values.reserve(static_cast<usize>(*size));
            for (i32 index = 0; index < *size; ++index) {
                auto value = machine.heap().element(
                    *keys, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto reference = value->as_reference();
                if (!reference) return std::unexpected(reference.error());
                snapshot_values.push_back(*reference);
            }
            bool changed = false;
            for (const ObjectRef element : snapshot_values) {
                const Value argument = Value::from_reference(element);
                auto contained = invoke_checked(
                    machine, *source, "java/util/Collection", "contains",
                    "(Ljava/lang/Object;)Z",
                    std::span<const Value>(&argument, 1U));
                if (!contained) return std::unexpected(contained.error());
                if (!contained->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "Collection.contains returned no value");
                }
                auto keep = contained->value().as_int();
                if (!keep) return std::unexpected(keep.error());
                if (*keep != 0) continue;
                auto removed = hash_map_remove(machine, *map, element);
                if (!removed) return std::unexpected(removed.error());
                changed = true;
            }
            return std::optional<Value>(Value::from_int(changed ? 1 : 0));
        });
    add(registry, "java/util/HashSet", "equals",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "HashSet.equals argument is missing");
            }
            auto other = arguments[1].as_reference();
            if (!other) return std::unexpected(other.error());
            if (*object == *other) {
                return std::optional<Value>(Value::from_int(1));
            }
            if (other->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto is_set = machine.object_is_instance(*other, "java/util/Set");
            if (!is_set) return std::unexpected(is_set.error());
            if (!*is_set) {
                return std::optional<Value>(Value::from_int(0));
            }

            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            auto size = int_field(machine, *map, kHashMapSizeField);
            if (!size) return std::unexpected(size.error());

            auto other_size_value = invoke_checked(
                machine, *other, "java/util/Collection", "size", "()I");
            if (!other_size_value) {
                return std::unexpected(other_size_value.error());
            }
            if (!other_size_value->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Set.size returned no value");
            }
            auto other_size = other_size_value->value().as_int();
            if (!other_size) return std::unexpected(other_size.error());
            if (*size != *other_size) {
                return std::optional<Value>(Value::from_int(0));
            }

            auto elements = collection_elements(machine, *other);
            if (!elements) return std::unexpected(elements.error());
            for (const ObjectRef element : *elements) {
                auto index = map_find_key(machine, *map, element);
                if (!index) return std::unexpected(index.error());
                if (*index < 0) {
                    return std::optional<Value>(Value::from_int(0));
                }
            }
            return std::optional<Value>(Value::from_int(1));
        });
    add(registry, "java/util/HashSet", "hashCode", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            auto size = int_field(machine, *map, kHashMapSizeField);
            auto keys = reference_field(machine, *map, kHashMapKeysField);
            if (!size || !keys || keys->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "HashSet state is invalid");
            }
            u32 sum = 0U;
            for (i32 index = 0; index < *size; ++index) {
                auto value = machine.heap().element(
                    *keys, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto element = value->as_reference();
                if (!element) return std::unexpected(element.error());
                if (element->is_null()) continue;
                auto hash = invoke_checked(machine, *element,
                                           "java/lang/Object", "hashCode",
                                           "()I");
                if (!hash) return std::unexpected(hash.error());
                if (!hash->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "Object.hashCode returned no value");
                }
                auto code = hash->value().as_int();
                if (!code) return std::unexpected(code.error());
                sum += static_cast<u32>(*code);
            }
            return std::optional<Value>(
                Value::from_int(static_cast<i32>(sum)));
        });
    add(registry, "java/util/HashSet", "toString", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto map = hash_set_map(machine, *object);
            if (!map) return std::unexpected(map.error());
            auto size = int_field(machine, *map, kHashMapSizeField);
            auto keys = reference_field(machine, *map, kHashMapKeysField);
            if (!size || !keys || keys->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "HashSet state is invalid");
            }
            std::u16string text(u"[");
            for (i32 index = 0; index < *size; ++index) {
                if (index != 0) text.append(u", ");
                auto value = machine.heap().element(
                    *keys, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto reference = value->as_reference();
                if (!reference) return std::unexpected(reference.error());
                auto item_text = value_text(machine, *reference, *object,
                                            u"(this Collection)");
                if (!item_text) return std::unexpected(item_text.error());
                text.append(*item_text);
            }
            text.push_back(u']');
            auto string = create_string(machine, std::move(text));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
}

void register_arrays(NativeMethodRegistry& registry) {
    const auto equals = [&registry](const char* descriptor,
                                    int kind) {
        add(registry, "java/util/Arrays", "equals", descriptor,
            [kind](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto left = reference_argument(arguments, 0U, true);
                auto right = reference_argument(arguments, 1U, true);
                if (!left) return std::unexpected(left.error());
                if (!right) return std::unexpected(right.error());
                if (*left == *right) {
                    return std::optional<Value>(Value::from_int(1));
                }
                if (left->is_null() || right->is_null()) {
                    return std::optional<Value>(Value::from_int(0));
                }
                auto left_size = machine.heap().array_length(*left);
                auto right_size = machine.heap().array_length(*right);
                if (!left_size) return std::unexpected(left_size.error());
                if (!right_size) return std::unexpected(right_size.error());
                if (*left_size != *right_size) {
                    return std::optional<Value>(Value::from_int(0));
                }
                for (usize index = 0U; index < *left_size; ++index) {
                    auto left_value = machine.heap().element(*left, index);
                    auto right_value = machine.heap().element(*right, index);
                    if (!left_value) return std::unexpected(left_value.error());
                    if (!right_value) return std::unexpected(right_value.error());
                    bool equal = false;
                    if (kind == 0 || kind == 1) {
                        auto a = left_value->as_int();
                        auto b = right_value->as_int();
                        if (!a || !b) {
                            return fail(ErrorCode::invalid_state,
                                        "primitive array element is invalid");
                        }
                        equal = *a == *b;
                    } else {
                        auto a = left_value->as_reference();
                        auto b = right_value->as_reference();
                        if (!a || !b) {
                            return fail(ErrorCode::invalid_state,
                                        "object array element is invalid");
                        }
                        auto comparison = values_equal(machine, *a, *b);
                        if (!comparison) {
                            return std::unexpected(comparison.error());
                        }
                        equal = *comparison;
                    }
                    if (!equal) {
                        return std::optional<Value>(Value::from_int(0));
                    }
                }
                return std::optional<Value>(Value::from_int(1));
            });
    };
    equals("([B[B)Z", 0);
    equals("([I[I)Z", 1);
    equals("([Ljava/lang/Object;[Ljava/lang/Object;)Z", 2);

    const auto fill = [&registry](const char* descriptor,
                                  bool object,
                                  bool range) {
        add(registry, "java/util/Arrays", "fill", descriptor,
            [object, range](Machine& machine,
                            std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto array = reference_argument(arguments, 0U);
                if (!array) return std::unexpected(array.error());
                auto length = machine.heap().array_length(*array);
                if (!length) return std::unexpected(length.error());

                usize from = 0U;
                usize count = *length;
                const usize value_index = range ? 3U : 1U;
                if (range) {
                    auto from_index = int_argument(arguments, 1U);
                    auto to_index = int_argument(arguments, 2U);
                    if (!from_index) return std::unexpected(from_index.error());
                    if (!to_index) return std::unexpected(to_index.error());
                    if (*from_index > *to_index) {
                        return fail_java("java/lang/IllegalArgumentException",
                                         "Arrays.fill fromIndex exceeds toIndex");
                    }
                    if (*from_index < 0 || *to_index < 0 ||
                        static_cast<usize>(*to_index) > *length) {
                        return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                         "Arrays.fill range is outside array bounds");
                    }
                    from = static_cast<usize>(*from_index);
                    count = static_cast<usize>(*to_index - *from_index);
                }

                Value value;
                if (object) {
                    auto reference = reference_argument(
                        arguments, value_index, true);
                    if (!reference) return std::unexpected(reference.error());
                    value = Value::from_reference(*reference);
                } else {
                    auto integer = int_argument(arguments, value_index);
                    if (!integer) return std::unexpected(integer.error());
                    value = Value::from_int(*integer);
                }
                auto filled = machine.heap().fill_array_range(
                    *array, from, count, value);
                if (!filled) return std::unexpected(filled.error());
                return std::optional<Value> {};
            });
    };
    fill("([BB)V", false, false);
    fill("([BIIB)V", false, true);
    fill("([II)V", false, false);
    fill("([IIII)V", false, true);
    fill("([SS)V", false, false);
    fill("([SIIS)V", false, true);
    fill("([ZZ)V", false, false);
    fill("([ZIIZ)V", false, true);
    fill("([Ljava/lang/Object;Ljava/lang/Object;)V", true, false);
    fill("([Ljava/lang/Object;IILjava/lang/Object;)V", true, true);

    const auto copy_of = [&registry](const char* descriptor,
                                     std::string class_name,
                                     bool object) {
        add(registry, "java/util/Arrays", "copyOf", descriptor,
            [class_name = std::move(class_name), object](
                Machine& machine,
                std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto source = reference_argument(arguments, 0U);
                auto requested = int_argument(arguments, 1U);
                if (!source) return std::unexpected(source.error());
                if (!requested) return std::unexpected(requested.error());
                if (*requested < 0) {
                    return fail_java("java/lang/NegativeArraySizeException",
                                     "negative copy length");
                }
                std::string result_class = class_name;
                if (object) {
                    auto runtime = machine.heap().class_name(*source);
                    if (!runtime) return std::unexpected(runtime.error());
                    result_class = *runtime;
                }
                auto result = machine.heap().allocate_array(
                    result_class, static_cast<usize>(*requested),
                    object ? Value::from_reference({}) : Value::from_int(0));
                if (!result) return std::unexpected(result.error());
                auto source_size = machine.heap().array_length(*source);
                if (!source_size) return std::unexpected(source_size.error());
                const usize count = std::min(
                    *source_size, static_cast<usize>(*requested));
                if (count > 0U) {
                    auto copied = machine.heap().copy_array_range(
                        *source, 0U, *result, 0U, count);
                    if (!copied) return std::unexpected(copied.error());
                }
                return std::optional<Value>(Value::from_reference(*result));
            });
    };
    copy_of("([BI)[B", "[B", false);
    copy_of("([II)[I", "[I", false);
    copy_of("([SI)[S", "[S", false);
    copy_of("([ZI)[Z", "[Z", false);
    copy_of("([Ljava/lang/Object;I)[Ljava/lang/Object;",
            "[Ljava/lang/Object;", true);

    const auto copy_range = [&registry](const char* descriptor,
                                        std::string class_name) {
        add(registry, "java/util/Arrays", "copyOfRange", descriptor,
            [class_name = std::move(class_name)](
                Machine& machine,
                std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto source = reference_argument(arguments, 0U);
                auto from = int_argument(arguments, 1U);
                auto to = int_argument(arguments, 2U);
                if (!source) return std::unexpected(source.error());
                if (!from) return std::unexpected(from.error());
                if (!to) return std::unexpected(to.error());
                if (*from > *to) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "copy range start exceeds end");
                }
                if (*from < 0) {
                    return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                     "copy range start is negative");
                }
                auto source_size = machine.heap().array_length(*source);
                if (!source_size) return std::unexpected(source_size.error());
                if (static_cast<usize>(*from) > *source_size) {
                    return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                     "copy range start exceeds array length");
                }
                const usize result_size = static_cast<usize>(*to - *from);
                auto result = machine.heap().allocate_array(
                    class_name, result_size, Value::from_int(0));
                if (!result) return std::unexpected(result.error());
                const usize available = *source_size -
                    static_cast<usize>(*from);
                const usize count = std::min(available, result_size);
                if (count > 0U) {
                    auto copied = machine.heap().copy_array_range(
                        *source, static_cast<usize>(*from),
                        *result, 0U, count);
                    if (!copied) return std::unexpected(copied.error());
                }
                return std::optional<Value>(Value::from_reference(*result));
            });
    };
    copy_range("([BII)[B", "[B");
    copy_range("([III)[I", "[I");

    const auto sort = [&registry](const char* descriptor, bool bytes) {
        add(registry, "java/util/Arrays", "sort", descriptor,
            [bytes](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto array = reference_argument(arguments, 0U);
                if (!array) return std::unexpected(array.error());
                auto length = machine.heap().array_length(*array);
                if (!length) return std::unexpected(length.error());
                std::vector<i32> values;
                values.reserve(*length);
                for (usize index = 0U; index < *length; ++index) {
                    auto value = machine.heap().element(*array, index);
                    if (!value) return std::unexpected(value.error());
                    auto integer = value->as_int();
                    if (!integer) return std::unexpected(integer.error());
                    values.push_back(bytes
                        ? static_cast<i32>(static_cast<i8>(*integer))
                        : *integer);
                }
                std::sort(values.begin(), values.end());
                for (usize index = 0U; index < values.size(); ++index) {
                    auto stored = machine.heap().set_element(
                        *array, index, Value::from_int(values[index]));
                    if (!stored) return std::unexpected(stored.error());
                }
                return std::optional<Value> {};
            });
    };
    sort("([B)V", true);
    sort("([I)V", false);
    add(registry, "java/util/Arrays", "sort", "([III)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto array = reference_argument(arguments, 0U);
            auto from = int_argument(arguments, 1U);
            auto to = int_argument(arguments, 2U);
            if (!array) return std::unexpected(array.error());
            if (!from) return std::unexpected(from.error());
            if (!to) return std::unexpected(to.error());
            if (*from > *to) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "sort range start exceeds end");
            }
            auto length = machine.heap().array_length(*array);
            if (!length) return std::unexpected(length.error());
            if (*from < 0 || *to < 0 ||
                static_cast<usize>(*from) > *length ||
                static_cast<usize>(*to) > *length) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "sort range is outside array bounds");
            }
            std::vector<i32> values;
            values.reserve(static_cast<usize>(*to - *from));
            for (i32 index = *from; index < *to; ++index) {
                auto value = machine.heap().element(
                    *array, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto integer = value->as_int();
                if (!integer) return std::unexpected(integer.error());
                values.push_back(*integer);
            }
            std::sort(values.begin(), values.end());
            for (usize index = 0U; index < values.size(); ++index) {
                auto stored = machine.heap().set_element(
                    *array, static_cast<usize>(*from) + index,
                    Value::from_int(values[index]));
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });

    add(registry, "java/util/Arrays", "binarySearch", "([II)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto array = reference_argument(arguments, 0U);
            auto key = int_argument(arguments, 1U);
            if (!array) return std::unexpected(array.error());
            if (!key) return std::unexpected(key.error());
            auto length = machine.heap().array_length(*array);
            if (!length) return std::unexpected(length.error());
            i32 low = 0;
            i32 high = static_cast<i32>(*length) - 1;
            while (low <= high) {
                const i32 middle = low + ((high - low) >> 1);
                auto value = machine.heap().element(
                    *array, static_cast<usize>(middle));
                if (!value) return std::unexpected(value.error());
                auto integer = value->as_int();
                if (!integer) return std::unexpected(integer.error());
                if (*integer < *key) low = middle + 1;
                else if (*integer > *key) high = middle - 1;
                else return std::optional<Value>(Value::from_int(middle));
            }
            return std::optional<Value>(Value::from_int(-(low + 1)));
        });
    const auto primitive_to_string = [&registry](const char* descriptor,
                                                  int kind) {
        add(registry, "java/util/Arrays", "toString", descriptor,
            [kind](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto array = reference_argument(arguments, 0U, true);
                if (!array) return std::unexpected(array.error());
                if (array->is_null()) {
                    auto text = create_string(machine, u"null");
                    if (!text) return std::unexpected(text.error());
                    return std::optional<Value>(Value::from_reference(*text));
                }
                auto length = machine.heap().array_length(*array);
                if (!length) return std::unexpected(length.error());
                std::u16string text(u"[");
                for (usize index = 0U; index < *length; ++index) {
                    if (index != 0U) text.append(u", ");
                    auto value = machine.heap().element(*array, index);
                    if (!value) return std::unexpected(value.error());
                    auto integer = value->as_int();
                    if (!integer) return std::unexpected(integer.error());
                    i32 number = *integer;
                    if (kind == 0) number = static_cast<i8>(number);
                    if (kind == 2) number = static_cast<i16>(number);
                    const std::string rendered = std::to_string(number);
                    for (const char character : rendered) {
                        text.push_back(static_cast<char16_t>(
                            static_cast<unsigned char>(character)));
                    }
                }
                text.push_back(u']');
                auto result = create_string(machine, std::move(text));
                if (!result) return std::unexpected(result.error());
                return std::optional<Value>(Value::from_reference(*result));
            });
    };
    primitive_to_string("([B)Ljava/lang/String;", 0);
    primitive_to_string("([I)Ljava/lang/String;", 1);
    primitive_to_string("([S)Ljava/lang/String;", 2);

    add(registry, "java/util/Arrays", "toString",
        "([Ljava/lang/Object;)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto array = reference_argument(arguments, 0U, true);
            if (!array) return std::unexpected(array.error());
            if (array->is_null()) {
                auto text = create_string(machine, u"null");
                if (!text) return std::unexpected(text.error());
                return std::optional<Value>(Value::from_reference(*text));
            }
            auto length = machine.heap().array_length(*array);
            if (!length) return std::unexpected(length.error());
            std::u16string text(u"[");
            for (usize index = 0U; index < *length; ++index) {
                if (index != 0U) text.append(u", ");
                auto value = machine.heap().element(*array, index);
                if (!value) return std::unexpected(value.error());
                auto reference = value->as_reference();
                if (!reference) return std::unexpected(reference.error());
                auto rendered = value_text(
                    machine, *reference, *array, u"(this Array)");
                if (!rendered) return std::unexpected(rendered.error());
                text.append(*rendered);
            }
            text.push_back(u']');
            auto result = create_string(machine, std::move(text));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });

    add(registry, "java/util/Arrays", "asList",
        "([Ljava/lang/Object;)Ljava/util/List;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto array = reference_argument(arguments, 0U);
            if (!array) return std::unexpected(array.error());
            auto length = machine.heap().array_length(*array);
            if (!length) return std::unexpected(length.error());
            if (*length > static_cast<usize>(std::numeric_limits<i32>::max())) {
                return fail(ErrorCode::overflow,
                            "Arrays.asList input exceeds integer size");
            }
            auto list = create_array_list(machine, 0);
            if (!list) return std::unexpected(list.error());
            auto data_stored = set_reference_field(
                machine, *list, kArrayListDataField, *array);
            auto size_stored = set_int_field(
                machine, *list, kArrayListSizeField,
                static_cast<i32>(*length));
            auto mode_stored = set_int_field(
                machine, *list, kArrayListMutationModeField,
                kArrayListFixedSize);
            if (!data_stored) return std::unexpected(data_stored.error());
            if (!size_stored) return std::unexpected(size_stored.error());
            if (!mode_stored) return std::unexpected(mode_stored.error());
            return std::optional<Value>(Value::from_reference(*list));
        });
}

void register_collections(NativeMethodRegistry& registry) {
    add(registry, "java/util/Collections", "swap", "(Ljava/util/List;II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto list = reference_argument(arguments, 0U);
            auto first = int_argument(arguments, 1U);
            auto second = int_argument(arguments, 2U);
            if (!list) return std::unexpected(list.error());
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            auto first_value = list_get(machine, *list, *first);
            auto second_value = list_get(machine, *list, *second);
            if (!first_value) return std::unexpected(first_value.error());
            if (!second_value) return std::unexpected(second_value.error());
            auto ignored_first = list_set(machine, *list, *first,
                                          *second_value);
            auto ignored_second = list_set(machine, *list, *second,
                                           *first_value);
            if (!ignored_first) return std::unexpected(ignored_first.error());
            if (!ignored_second) return std::unexpected(ignored_second.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Collections", "reverse", "(Ljava/util/List;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto list = reference_argument(arguments, 0U);
            if (!list) return std::unexpected(list.error());
            auto size = list_size(machine, *list);
            if (!size) return std::unexpected(size.error());
            for (i32 left = 0, right = *size - 1; left < right;
                 ++left, --right) {
                auto first = list_get(machine, *list, left);
                auto second = list_get(machine, *list, right);
                if (!first) return std::unexpected(first.error());
                if (!second) return std::unexpected(second.error());
                auto ignored_first = list_set(machine, *list, left, *second);
                auto ignored_second = list_set(machine, *list, right, *first);
                if (!ignored_first) return std::unexpected(ignored_first.error());
                if (!ignored_second) return std::unexpected(ignored_second.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "java/util/Collections", "shuffle", "(Ljava/util/List;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto list = reference_argument(arguments, 0U);
            if (!list) return std::unexpected(list.error());
            auto size = list_size(machine, *list);
            if (!size) return std::unexpected(size.error());
            u64 state = static_cast<u64>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            for (i32 index = *size - 1; index > 0; --index) {
                state = state * 6364136223846793005ULL + 1ULL;
                const i32 other = static_cast<i32>(
                    (state >> 32U) % static_cast<u64>(index + 1));
                auto first = list_get(machine, *list, index);
                auto second = list_get(machine, *list, other);
                if (!first) return std::unexpected(first.error());
                if (!second) return std::unexpected(second.error());
                auto ignored_first = list_set(machine, *list, index, *second);
                auto ignored_second = list_set(machine, *list, other, *first);
                if (!ignored_first) return std::unexpected(ignored_first.error());
                if (!ignored_second) return std::unexpected(ignored_second.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "java/util/Collections", "fill",
        "(Ljava/util/List;Ljava/lang/Object;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto list = reference_argument(arguments, 0U);
            auto value = reference_argument(arguments, 1U, true);
            if (!list) return std::unexpected(list.error());
            if (!value) return std::unexpected(value.error());
            auto size = list_size(machine, *list);
            if (!size) return std::unexpected(size.error());
            for (i32 index = 0; index < *size; ++index) {
                auto ignored = list_set(machine, *list, index, *value);
                if (!ignored) return std::unexpected(ignored.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "java/util/Collections", "sort", "(Ljava/util/List;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto list = reference_argument(arguments, 0U);
            if (!list) return std::unexpected(list.error());
            auto size = list_size(machine, *list);
            if (!size) return std::unexpected(size.error());
            for (i32 index = 1; index < *size; ++index) {
                auto value = list_get(machine, *list, index);
                if (!value) return std::unexpected(value.error());
                i32 position = index;
                while (position > 0) {
                    auto previous = list_get(machine, *list, position - 1);
                    if (!previous) return std::unexpected(previous.error());
                    auto comparison = compare_objects(machine,
                                                      *previous, *value);
                    if (!comparison) return std::unexpected(comparison.error());
                    if (*comparison <= 0) break;
                    auto ignored = list_set(machine, *list, position,
                                            *previous);
                    if (!ignored) return std::unexpected(ignored.error());
                    --position;
                }
                auto ignored = list_set(machine, *list, position, *value);
                if (!ignored) return std::unexpected(ignored.error());
            }
            return std::optional<Value> {};
        });
    const auto identity_collection = [&registry](
        const char* name, const char* descriptor) {
        add(registry, "java/util/Collections", name, descriptor,
            [](Machine&, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto value = reference_argument(arguments, 0U);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_reference(*value));
            });
    };
    identity_collection("unmodifiableMap",
                        "(Ljava/util/Map;)Ljava/util/Map;");
    identity_collection("unmodifiableSet",
                        "(Ljava/util/Set;)Ljava/util/Set;");

    add(registry, "java/util/Collections", "emptyMap", "()Ljava/util/Map;",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            auto map = machine.class_states().allocate_instance(
                machine.heap(), "java/util/HashMap");
            if (!map) return std::unexpected(map.error());
            auto pinned = NativeRootScope::pin(machine.native_roots(), *map);
            if (!pinned) return std::unexpected(pinned.error());
            auto initialized = initialize_hash_map(machine, *map, 0);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value>(Value::from_reference(*map));
        });
    add(registry, "java/util/Collections", "emptySet", "()Ljava/util/Set;",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            auto set = create_hash_set(machine, 0);
            if (!set) return std::unexpected(set.error());
            return std::optional<Value>(Value::from_reference(*set));
        });
    add(registry, "java/util/Collections", "singleton",
        "(Ljava/lang/Object;)Ljava/util/Set;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = reference_argument(arguments, 0U, true);
            if (!value) return std::unexpected(value.error());
            auto set = create_hash_set(machine, 1);
            if (!set) return std::unexpected(set.error());
            const Value argument = Value::from_reference(*value);
            auto added = invoke_checked(
                machine, *set, "java/util/Set", "add", "(Ljava/lang/Object;)Z",
                std::span<const Value>(&argument, 1U));
            if (!added) return std::unexpected(added.error());
            return std::optional<Value>(Value::from_reference(*set));
        });
    add(registry, "java/util/Collections", "newSetFromMap",
        "(Ljava/util/Map;)Ljava/util/Set;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto source = reference_argument(arguments, 0U);
            if (!source) return std::unexpected(source.error());
            auto set = create_hash_set(machine, 0);
            if (!set) return std::unexpected(set.error());
            return std::optional<Value>(Value::from_reference(*set));
        });
    add(registry, "java/util/Collections", "addAll",
        "(Ljava/util/Collection;[Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto collection = reference_argument(arguments, 0U);
            auto values = reference_argument(arguments, 1U);
            if (!collection) return std::unexpected(collection.error());
            if (!values) return std::unexpected(values.error());
            auto length = machine.heap().array_length(*values);
            if (!length) return std::unexpected(length.error());
            bool changed = false;
            for (usize index = 0U; index < *length; ++index) {
                auto value = machine.heap().element(*values, index);
                if (!value) return std::unexpected(value.error());
                auto reference = value->as_reference();
                if (!reference) return std::unexpected(reference.error());
                const Value argument = Value::from_reference(*reference);
                auto added = invoke_checked(
                    machine, *collection, "java/util/Collection", "add",
                    "(Ljava/lang/Object;)Z",
                    std::span<const Value>(&argument, 1U));
                if (!added) return std::unexpected(added.error());
                if (!added->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "Collection.add returned no value");
                }
                auto result = added->value().as_int();
                if (!result) return std::unexpected(result.error());
                changed = changed || *result != 0;
            }
            return std::optional<Value>(Value::from_int(changed ? 1 : 0));
        });

    add(registry, "java/util/List", "of", "()Ljava/util/List;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "List.of() expects no arguments");
            }
            auto list = create_array_list(machine, 0);
            if (!list) return std::unexpected(list.error());
            auto immutable = set_int_field(
                machine, *list, kArrayListMutationModeField,
                kArrayListImmutable);
            if (!immutable) return std::unexpected(immutable.error());
            return std::optional<Value>(Value::from_reference(*list));
        });

    add(registry, "java/util/Collections", "unmodifiableList",
        "(Ljava/util/List;)Ljava/util/List;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto source = reference_argument(arguments, 0U);
            if (!source) return std::unexpected(source.error());
            auto size = list_size(machine, *source);
            if (!size) return std::unexpected(size.error());
            auto list = create_array_list(machine, *size);
            if (!list) return std::unexpected(list.error());
            for (i32 index = 0; index < *size; ++index) {
                auto value = list_get(machine, *source, index);
                if (!value) return std::unexpected(value.error());
                auto appended = array_list_append(machine, *list, *value);
                if (!appended) return std::unexpected(appended.error());
            }
            auto immutable = set_int_field(
                machine, *list, kArrayListMutationModeField,
                kArrayListImmutable);
            if (!immutable) return std::unexpected(immutable.error());
            return std::optional<Value>(Value::from_reference(*list));
        });
    const auto list_factory = [&registry](const char* name, bool singleton) {
        add(registry, "java/util/Collections", name,
            singleton ? "(Ljava/lang/Object;)Ljava/util/List;"
                      : "()Ljava/util/List;",
            [singleton](Machine& machine,
                        std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto list = create_array_list(machine, singleton ? 1 : 0);
                if (!list) return std::unexpected(list.error());
                if (singleton) {
                    auto value = reference_argument(arguments, 0U, true);
                    if (!value) return std::unexpected(value.error());
                    auto appended = array_list_append(machine, *list, *value);
                    if (!appended) return std::unexpected(appended.error());
                }
                auto immutable = set_int_field(
                    machine, *list, kArrayListMutationModeField,
                    kArrayListImmutable);
                if (!immutable) return std::unexpected(immutable.error());
                return std::optional<Value>(Value::from_reference(*list));
            });
    };
    list_factory("emptyList", false);
    list_factory("singletonList", true);
}

void register_optional(NativeMethodRegistry& registry) {
    const auto make_optional = [](Machine& machine, ObjectRef value, bool present)
        -> Result<ObjectRef> {
        auto optional = machine.class_states().allocate_instance(
            machine.heap(), "java/util/Optional");
        if (!optional) return std::unexpected(optional.error());
        auto pinned = NativeRootScope::pin(machine.native_roots(), *optional);
        if (!pinned) return std::unexpected(pinned.error());
        auto stored = set_optional_state(machine, *optional, value, present);
        if (!stored) return std::unexpected(stored.error());
        return *optional;
    };

    add(registry, "java/util/Optional", "<init>",
        "(Ljava/lang/Object;Z)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = reference_argument(arguments, 1U, true);
            auto present = int_argument(arguments, 2U);
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            if (!present) return std::unexpected(present.error());
            auto stored = set_optional_state(
                machine, *object, *value, *present != 0);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Optional", "empty", "()Ljava/util/Optional;",
        [make_optional](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            auto optional = make_optional(machine, {}, false);
            if (!optional) return std::unexpected(optional.error());
            return std::optional<Value>(Value::from_reference(*optional));
        });
    add(registry, "java/util/Optional", "of",
        "(Ljava/lang/Object;)Ljava/util/Optional;",
        [make_optional](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = reference_argument(arguments, 0U);
            if (!value) return std::unexpected(value.error());
            auto optional = make_optional(machine, *value, true);
            if (!optional) return std::unexpected(optional.error());
            return std::optional<Value>(Value::from_reference(*optional));
        });
    add(registry, "java/util/Optional", "ofNullable",
        "(Ljava/lang/Object;)Ljava/util/Optional;",
        [make_optional](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = reference_argument(arguments, 0U, true);
            if (!value) return std::unexpected(value.error());
            auto optional = make_optional(machine, *value, !value->is_null());
            if (!optional) return std::unexpected(optional.error());
            return std::optional<Value>(Value::from_reference(*optional));
        });
    add(registry, "java/util/Optional", "isPresent", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto present = int_field(machine, *object, kOptionalPresentField);
            if (!present) return std::unexpected(present.error());
            return std::optional<Value>(Value::from_int(*present != 0 ? 1 : 0));
        });
    add(registry, "java/util/Optional", "get", "()Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto state = optional_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (!state->present) {
                return fail_java("java/util/NoSuchElementException",
                                 "No value present");
            }
            return std::optional<Value>(Value::from_reference(state->value));
        });
    add(registry, "java/util/Optional", "ifPresent",
        "(Ljava/util/function/Consumer;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto consumer = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!consumer) return std::unexpected(consumer.error());
            auto state = optional_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (!state->present) return std::optional<Value> {};
            if (consumer->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Optional consumer is null");
            }
            const Value callback_argument = Value::from_reference(state->value);
            auto invoked = invoke_checked(
                machine, *consumer, "java/util/function/Consumer", "accept",
                "(Ljava/lang/Object;)V",
                std::span<const Value>(&callback_argument, 1U));
            if (!invoked) return std::unexpected(invoked.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Optional", "filter",
        "(Ljava/util/function/Predicate;)Ljava/util/Optional;",
        [make_optional](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto predicate = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!predicate) return std::unexpected(predicate.error());
            auto state = optional_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (!state->present) {
                return std::optional<Value>(Value::from_reference(*object));
            }
            const Value callback_argument = Value::from_reference(state->value);
            auto matched = invoke_checked(
                machine, *predicate, "java/util/function/Predicate", "test",
                "(Ljava/lang/Object;)Z",
                std::span<const Value>(&callback_argument, 1U));
            if (!matched) return std::unexpected(matched.error());
            if (!matched->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Predicate.test returned no value");
            }
            auto accepted = matched->value().as_int();
            if (!accepted) return std::unexpected(accepted.error());
            if (*accepted != 0) {
                return std::optional<Value>(Value::from_reference(*object));
            }
            auto empty = make_optional(machine, {}, false);
            if (!empty) return std::unexpected(empty.error());
            return std::optional<Value>(Value::from_reference(*empty));
        });
    add(registry, "java/util/Optional", "map",
        "(Ljava/util/function/Function;)Ljava/util/Optional;",
        [make_optional](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto mapper = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!mapper) return std::unexpected(mapper.error());
            auto state = optional_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (!state->present) {
                auto empty = make_optional(machine, {}, false);
                if (!empty) return std::unexpected(empty.error());
                return std::optional<Value>(Value::from_reference(*empty));
            }
            const Value callback_argument = Value::from_reference(state->value);
            auto mapped = invoke_checked(
                machine, *mapper, "java/util/function/Function", "apply",
                "(Ljava/lang/Object;)Ljava/lang/Object;",
                std::span<const Value>(&callback_argument, 1U));
            if (!mapped) return std::unexpected(mapped.error());
            if (!mapped->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Function.apply returned no value");
            }
            auto mapped_value = mapped->value().as_reference();
            if (!mapped_value) return std::unexpected(mapped_value.error());
            auto optional = make_optional(machine, *mapped_value,
                                          !mapped_value->is_null());
            if (!optional) return std::unexpected(optional.error());
            return std::optional<Value>(Value::from_reference(*optional));
        });
    add(registry, "java/util/Optional", "flatMap",
        "(Ljava/util/function/Function;)Ljava/util/Optional;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto mapper = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!mapper) return std::unexpected(mapper.error());
            auto state = optional_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (!state->present) {
                return std::optional<Value>(Value::from_reference(*object));
            }
            const Value callback_argument = Value::from_reference(state->value);
            auto mapped = invoke_checked(
                machine, *mapper, "java/util/function/Function", "apply",
                "(Ljava/lang/Object;)Ljava/lang/Object;",
                std::span<const Value>(&callback_argument, 1U));
            if (!mapped) return std::unexpected(mapped.error());
            if (!mapped->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Function.apply returned no value");
            }
            auto optional = mapped->value().as_reference();
            if (!optional) return std::unexpected(optional.error());
            if (optional->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Optional.flatMap mapper returned null");
            }
            auto class_name = machine.heap().class_name(*optional);
            if (!class_name) return std::unexpected(class_name.error());
            auto compatible = machine.classes().is_assignable(
                *class_name, "java/util/Optional");
            if (!compatible) return std::unexpected(compatible.error());
            if (!*compatible) {
                return fail_java("java/lang/ClassCastException",
                                 "Optional.flatMap mapper returned non-Optional");
            }
            return std::optional<Value>(Value::from_reference(*optional));
        });
    add(registry, "java/util/Optional", "orElse",
        "(Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto fallback = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!fallback) return std::unexpected(fallback.error());
            auto state = optional_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (!state->present) {
                return std::optional<Value>(Value::from_reference(*fallback));
            }
            return std::optional<Value>(Value::from_reference(state->value));
        });
    add(registry, "java/util/Optional", "orElseGet",
        "(Ljava/util/function/Supplier;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto supplier = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!supplier) return std::unexpected(supplier.error());
            auto state = optional_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (state->present) {
                return std::optional<Value>(Value::from_reference(state->value));
            }
            if (supplier->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Optional supplier is null");
            }
            auto supplied = invoke_checked(
                machine, *supplier, "java/util/function/Supplier", "get",
                "()Ljava/lang/Object;");
            if (!supplied) return std::unexpected(supplied.error());
            if (!supplied->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Supplier.get returned no value");
            }
            return supplied->value();
        });
    add(registry, "java/util/Optional", "orElseThrow",
        "(Ljava/util/function/Supplier;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto supplier = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!supplier) return std::unexpected(supplier.error());
            auto state = optional_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (state->present) {
                return std::optional<Value>(Value::from_reference(state->value));
            }
            if (supplier->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "exception supplier is null");
            }
            auto supplied = invoke_checked(
                machine, *supplier, "java/util/function/Supplier", "get",
                "()Ljava/lang/Object;");
            if (!supplied) return std::unexpected(supplied.error());
            if (!supplied->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Supplier.get returned no value");
            }
            auto throwable = supplied->value().as_reference();
            if (!throwable) return std::unexpected(throwable.error());
            if (throwable->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "exception supplier returned null");
            }
            auto throwable_class = machine.heap().class_name(*throwable);
            if (!throwable_class) return std::unexpected(throwable_class.error());
            auto compatible = machine.classes().is_assignable(
                *throwable_class, "java/lang/Throwable");
            if (!compatible) return std::unexpected(compatible.error());
            if (!*compatible) {
                return fail_java("java/lang/ClassCastException",
                                 "exception supplier returned non-Throwable");
            }
            auto message_value = invoke_checked(
                machine, *throwable, "java/lang/Throwable", "getMessage",
                "()Ljava/lang/String;");
            if (!message_value) return std::unexpected(message_value.error());
            std::string message;
            if (message_value->has_value()) {
                auto message_ref = message_value->value().as_reference();
                if (!message_ref) return std::unexpected(message_ref.error());
                if (!message_ref->is_null()) {
                    auto text = utf8_text(machine, *message_ref);
                    if (!text) return std::unexpected(text.error());
                    message = std::move(*text);
                }
            }
            return fail_java(*throwable_class, std::move(message));
        });
    add(registry, "java/util/Optional", "equals",
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
            if (*other_class != "java/util/Optional") {
                return std::optional<Value>(Value::from_int(0));
            }
            auto state = optional_state(machine, *object);
            auto other_state = optional_state(machine, *other);
            if (!state) return std::unexpected(state.error());
            if (!other_state) return std::unexpected(other_state.error());
            if (state->present != other_state->present) {
                return std::optional<Value>(Value::from_int(0));
            }
            if (!state->present) {
                return std::optional<Value>(Value::from_int(1));
            }
            auto equal = values_equal(machine, state->value, other_state->value);
            if (!equal) return std::unexpected(equal.error());
            return std::optional<Value>(Value::from_int(*equal ? 1 : 0));
        });
    add(registry, "java/util/Optional", "hashCode", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto state = optional_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (!state->present) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto hash = invoke_checked(machine, state->value, "java/lang/Object",
                                       "hashCode", "()I");
            if (!hash) return std::unexpected(hash.error());
            if (!hash->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Object.hashCode returned no value");
            }
            return hash->value();
        });
    add(registry, "java/util/Optional", "toString", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto state = optional_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            std::u16string text;
            if (!state->present) {
                text = u"Optional.empty";
            } else {
                auto rendered = value_text(machine, state->value, *object,
                                           u"(this Optional)");
                if (!rendered) return std::unexpected(rendered.error());
                text = u"Optional[";
                text.append(*rendered);
                text.push_back(u']');
            }
            auto string = create_string(machine, std::move(text));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
}

void register_base64(NativeMethodRegistry& registry) {
    const auto factory = [&registry](const char* name,
                                     const char* result_class) {
        std::string descriptor = "()L";
        descriptor.append(result_class);
        descriptor.push_back(';');
        add(registry, "java/util/Base64", name, descriptor,
            [result_class = std::string(result_class)](
                Machine& machine,
                std::span<const Value>)
                -> Result<std::optional<Value>> {
                auto object = machine.class_states().allocate_instance(
                    machine.heap(), result_class);
                if (!object) return std::unexpected(object.error());
                return std::optional<Value>(Value::from_reference(*object));
            });
    };
    factory("getEncoder", "java/util/Base64$Encoder");
    factory("getDecoder", "java/util/Base64$Decoder");

    add(registry, "java/util/Base64$Encoder", "encode", "([B)[B",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto input = reference_argument(arguments, 1U);
            if (!input) return std::unexpected(input.error());
            auto bytes = read_bytes(machine, *input);
            if (!bytes) return std::unexpected(bytes.error());
            const auto encoded = base64_encode(*bytes);
            auto result = byte_array_from_bytes(machine, encoded);
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/util/Base64$Encoder", "encodeToString",
        "([B)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto input = reference_argument(arguments, 1U);
            if (!input) return std::unexpected(input.error());
            auto bytes = read_bytes(machine, *input);
            if (!bytes) return std::unexpected(bytes.error());
            const auto encoded = base64_encode(*bytes);
            std::u16string text;
            text.reserve(encoded.size());
            for (const u8 byte : encoded) {
                text.push_back(static_cast<char16_t>(byte));
            }
            auto result = create_string(machine, std::move(text));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/util/Base64$Decoder", "decode", "([B)[B",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto input = reference_argument(arguments, 1U);
            if (!input) return std::unexpected(input.error());
            auto bytes = read_bytes(machine, *input);
            if (!bytes) return std::unexpected(bytes.error());
            auto decoded = base64_decode(*bytes);
            if (!decoded) return std::unexpected(decoded.error());
            auto result = byte_array_from_bytes(machine, *decoded);
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/util/Base64$Decoder", "decode",
        "(Ljava/lang/String;)[B",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto input = reference_argument(arguments, 1U);
            if (!input) return std::unexpected(input.error());
            auto text = machine.heap().string_value(*input);
            if (!text) return std::unexpected(text.error());
            std::vector<u8> bytes;
            bytes.reserve(text->size());
            for (const char16_t character : *text) {
                if (character > 0x7FU) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Base64 input is not ASCII");
                }
                bytes.push_back(static_cast<u8>(character));
            }
            auto decoded = base64_decode(bytes);
            if (!decoded) return std::unexpected(decoded.error());
            auto result = byte_array_from_bytes(machine, *decoded);
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
}

} // namespace

void register_headless_compat_natives(NativeMethodRegistry& registry) {
    register_comparable_bridges(registry);
    register_objects_extensions(registry);
    register_array_iterator(registry);
    register_array_list_extensions(registry);
    register_hash_map(registry);
    register_hash_set(registry);
    register_arrays(registry);
    register_collections(registry);
    register_optional(registry);
    register_base64(registry);
}

} // namespace phoneme::vm
