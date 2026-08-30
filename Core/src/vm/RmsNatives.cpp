#include "RmsNatives.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "phoneme/runtime/RecordStoreRegistry.hpp"
#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/NativeMethodRegistry.hpp"

namespace phoneme::vm {
namespace {

constexpr usize kStoreStateField = 0;
constexpr usize kStoreOpenField = 1;
constexpr usize kStoreStateNameIndex = 0;
constexpr usize kStoreStateListenersIndex = 1;
constexpr usize kEnumerationRecordsField = 0;
constexpr usize kEnumerationIdsField = 1;
constexpr usize kEnumerationIndexField = 2;
constexpr usize kEnumerationDestroyedField = 3;
constexpr usize kEnumerationKeepUpdatedField = 4;
constexpr usize kEnumerationStoreNameField = 5;
constexpr usize kEnumerationFilterField = 6;
constexpr usize kEnumerationComparatorField = 7;
constexpr usize kEnumerationStoreField = 8;
constexpr usize kEnumerationVersionField = 9;

std::mutex g_open_handles_mutex;
std::unordered_map<Machine*, std::vector<ObjectRef>> g_open_handles;

void add(NativeMethodRegistry& registry,
         std::string owner,
         std::string name,
         std::string descriptor,
         NativeMethod method) {
    auto registered = registry.register_method(std::move(owner),
                                               std::move(name),
                                               std::move(descriptor),
                                               std::move(method));
    if (!registered) {
        std::terminate();
    }
}

[[nodiscard]] bool is_open_store_handle(Machine& machine,
                                        ObjectRef handle) {
    auto class_name = machine.heap().class_name(handle);
    if (!class_name || *class_name !=
            "javax/microedition/rms/RecordStore") {
        return false;
    }
    auto open_value = machine.heap().field(handle, kStoreOpenField);
    if (!open_value) return false;
    auto open = open_value->as_int();
    return open.has_value() && *open != 0;
}

void track_open_handle(Machine& machine, ObjectRef handle) {
    std::scoped_lock lock(g_open_handles_mutex);
    auto& handles = g_open_handles[&machine];
    std::erase_if(handles, [&machine](ObjectRef existing) {
        return !is_open_store_handle(machine, existing);
    });
    if (std::find(handles.begin(), handles.end(), handle) == handles.end()) {
        handles.push_back(handle);
    }
}

void untrack_open_handle(Machine& machine, ObjectRef handle) {
    std::scoped_lock lock(g_open_handles_mutex);
    const auto found = g_open_handles.find(&machine);
    if (found == g_open_handles.end()) return;
    auto& handles = found->second;
    std::erase(handles, handle);
    if (handles.empty()) g_open_handles.erase(found);
}

[[nodiscard]] std::vector<ObjectRef> open_handles_snapshot(Machine& machine) {
    std::scoped_lock lock(g_open_handles_mutex);
    const auto found = g_open_handles.find(&machine);
    if (found == g_open_handles.end()) return {};
    auto& handles = found->second;
    std::erase_if(handles, [&machine](ObjectRef handle) {
        return !is_open_store_handle(machine, handle);
    });
    if (handles.empty()) {
        g_open_handles.erase(found);
        return {};
    }
    return handles;
}

[[nodiscard]] std::unexpected<Error> rms_failure(const Error& error) {
    switch (error.code) {
    case ErrorCode::class_not_found:
        return fail_java(
            "javax/microedition/rms/RecordStoreNotFoundException",
            error.message);
    case ErrorCode::invalid_state:
        return fail_java(
            "javax/microedition/rms/RecordStoreNotOpenException",
            error.message);
    case ErrorCode::out_of_range:
        return fail_java(
            "javax/microedition/rms/InvalidRecordIDException",
            error.message);
    case ErrorCode::overflow:
        return fail_java(
            "javax/microedition/rms/RecordStoreFullException",
            error.message);
    default:
        return fail_java(
            "javax/microedition/rms/RecordStoreException",
            error.message);
    }
}

[[nodiscard]] Result<ObjectRef> receiver(
    std::span<const Value> arguments) {
    if (arguments.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "RMS method has no receiver");
    }
    auto object = arguments.front().as_reference();
    if (!object || object->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "RMS receiver is null");
    }
    return *object;
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
    if (string.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "RMS name is null");
    }
    auto text = machine.heap().string_value(string);
    if (!text) return std::unexpected(text.error());
    std::string result;
    result.reserve(text->size());
    for (usize index = 0; index < text->size(); ++index) {
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

[[nodiscard]] Result<std::u16string> decode_utf8(std::string_view input) {
    std::u16string output;
    output.reserve(input.size());
    usize index = 0;
    while (index < input.size()) {
        const u8 first = static_cast<u8>(input[index++]);
        u32 code_point = 0;
        usize continuation_count = 0;
        if ((first & 0x80U) == 0U) {
            code_point = first;
        } else if ((first & 0xE0U) == 0xC0U) {
            code_point = first & 0x1FU;
            continuation_count = 1;
        } else if ((first & 0xF0U) == 0xE0U) {
            code_point = first & 0x0FU;
            continuation_count = 2;
        } else if ((first & 0xF8U) == 0xF0U) {
            code_point = first & 0x07U;
            continuation_count = 3;
        } else {
            return fail(ErrorCode::invalid_argument,
                        "RMS name contains invalid UTF-8");
        }
        if (continuation_count > input.size() - index) {
            return fail(ErrorCode::invalid_argument,
                        "RMS name contains truncated UTF-8");
        }
        for (usize continuation = 0;
             continuation < continuation_count;
             ++continuation) {
            const u8 byte = static_cast<u8>(input[index++]);
            if ((byte & 0xC0U) != 0x80U) {
                return fail(ErrorCode::invalid_argument,
                            "RMS name contains invalid UTF-8 continuation");
            }
            code_point = (code_point << 6U) | (byte & 0x3FU);
        }
        const u32 minimum = continuation_count == 0 ? 0U
            : (continuation_count == 1 ? 0x80U
            : (continuation_count == 2 ? 0x800U : 0x10000U));
        if (code_point < minimum || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return fail(ErrorCode::invalid_argument,
                        "RMS name contains invalid UTF-8 code point");
        }
        if (code_point <= 0xFFFFU) {
            output.push_back(static_cast<char16_t>(code_point));
        } else {
            code_point -= 0x10000U;
            output.push_back(static_cast<char16_t>(
                0xD800U | (code_point >> 10U)));
            output.push_back(static_cast<char16_t>(
                0xDC00U | (code_point & 0x3FFU)));
        }
    }
    return output;
}

[[nodiscard]] Result<ObjectRef> ensure_store_state(Machine& machine,
                                                   ObjectRef handle) {
    auto state_value = machine.heap().field(handle, kStoreStateField);
    if (!state_value) return std::unexpected(state_value.error());
    auto state_reference = state_value->as_reference();
    if (!state_reference || state_reference->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "RecordStore handle state is invalid");
    }
    auto class_name = machine.heap().class_name(*state_reference);
    if (!class_name) return std::unexpected(class_name.error());
    if (*class_name == "[Ljava/lang/Object;") return *state_reference;
    if (*class_name != "java/lang/String") {
        return fail(ErrorCode::invalid_state,
                    "RecordStore handle state has an invalid type");
    }

    auto listeners = machine.heap().allocate_array(
        "[Ljava/lang/Object;", 0U, Value::from_reference({}));
    auto state = machine.heap().allocate_array(
        "[Ljava/lang/Object;", 2U, Value::from_reference({}));
    if (!listeners) return std::unexpected(listeners.error());
    if (!state) return std::unexpected(state.error());
    auto name_stored = machine.heap().set_element(
        *state, kStoreStateNameIndex,
        Value::from_reference(*state_reference));
    auto listeners_stored = machine.heap().set_element(
        *state, kStoreStateListenersIndex,
        Value::from_reference(*listeners));
    auto state_stored = machine.heap().set_field(
        handle, kStoreStateField, Value::from_reference(*state));
    if (!name_stored) return std::unexpected(name_stored.error());
    if (!listeners_stored) return std::unexpected(listeners_stored.error());
    if (!state_stored) return std::unexpected(state_stored.error());
    return *state;
}

[[nodiscard]] Result<ObjectRef> store_name_reference_from_handle(
    Machine& machine,
    ObjectRef handle,
    bool require_open) {
    auto open_value = machine.heap().field(handle, kStoreOpenField);
    if (!open_value) return std::unexpected(open_value.error());
    auto open = open_value->as_int();
    if (!open) return std::unexpected(open.error());
    if (require_open && *open == 0) {
        return fail_java(
            "javax/microedition/rms/RecordStoreNotOpenException",
            "RecordStore handle is closed");
    }
    auto state = ensure_store_state(machine, handle);
    if (!state) return std::unexpected(state.error());
    auto name_value = machine.heap().element(*state, kStoreStateNameIndex);
    if (!name_value) return std::unexpected(name_value.error());
    auto name_reference = name_value->as_reference();
    if (!name_reference || name_reference->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "RecordStore handle name is invalid");
    }
    return *name_reference;
}

[[nodiscard]] Result<std::string> store_name_from_handle(
    Machine& machine,
    ObjectRef handle,
    bool require_open) {
    auto name = store_name_reference_from_handle(
        machine, handle, require_open);
    if (!name) return std::unexpected(name.error());
    return utf8_text(machine, *name);
}

[[nodiscard]] Result<ObjectRef> store_listener_array(Machine& machine,
                                                     ObjectRef handle) {
    auto state = ensure_store_state(machine, handle);
    if (!state) return std::unexpected(state.error());
    auto listeners_value = machine.heap().element(
        *state, kStoreStateListenersIndex);
    if (!listeners_value) return std::unexpected(listeners_value.error());
    auto listeners = listeners_value->as_reference();
    if (!listeners || listeners->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "RecordStore listener state is invalid");
    }
    return *listeners;
}

[[nodiscard]] Result<std::vector<u8>> record_slice(
    Machine& machine,
    ObjectRef array,
    i32 offset,
    i32 length) {
    if (length < 0 || offset < 0) {
        return fail_java("java/lang/IndexOutOfBoundsException",
                         "RMS record slice is invalid");
    }
    if (array.is_null()) {
        if (length == 0 && offset == 0) return std::vector<u8> {};
        return fail_java("java/lang/NullPointerException",
                         "RMS record data is null");
    }
    auto class_name = machine.heap().class_name(array);
    auto array_length = machine.heap().array_length(array);
    if (!class_name || *class_name != "[B" || !array_length) {
        return fail_java("java/lang/IllegalArgumentException",
                         "RMS record data is not byte[]");
    }
    if (static_cast<usize>(offset) > *array_length ||
        static_cast<usize>(length) >
            *array_length - static_cast<usize>(offset)) {
        return fail_java("java/lang/IndexOutOfBoundsException",
                         "RMS record slice exceeds byte[]");
    }
    std::vector<u8> result;
    result.reserve(static_cast<usize>(length));
    for (i32 index = 0; index < length; ++index) {
        auto value = machine.heap().element(
            array, static_cast<usize>(offset + index));
        if (!value) return std::unexpected(value.error());
        auto integer = value->as_int();
        if (!integer) return std::unexpected(integer.error());
        result.push_back(static_cast<u8>(static_cast<i8>(*integer)));
    }
    return result;
}

[[nodiscard]] Result<ObjectRef> create_byte_array(
    Machine& machine,
    std::span<const u8> bytes) {
    auto array = machine.heap().allocate_array(
        "[B", bytes.size(), Value::from_int(0));
    if (!array) return std::unexpected(array.error());
    for (usize index = 0; index < bytes.size(); ++index) {
        auto stored = machine.heap().set_element(
            *array, index,
            Value::from_int(static_cast<i32>(static_cast<i8>(bytes[index]))));
        if (!stored) return std::unexpected(stored.error());
    }
    return *array;
}

[[nodiscard]] Result<ObjectRef> create_store_handle(
    Machine& machine,
    ObjectRef name) {
    auto store = machine.class_states().allocate_instance(
        machine.heap(), "javax/microedition/rms/RecordStore");
    auto listeners = machine.heap().allocate_array(
        "[Ljava/lang/Object;", 0U, Value::from_reference({}));
    auto state = machine.heap().allocate_array(
        "[Ljava/lang/Object;", 2U, Value::from_reference({}));
    if (!store) return std::unexpected(store.error());
    if (!listeners) return std::unexpected(listeners.error());
    if (!state) return std::unexpected(state.error());
    auto name_stored = machine.heap().set_element(
        *state, kStoreStateNameIndex, Value::from_reference(name));
    auto listeners_stored = machine.heap().set_element(
        *state, kStoreStateListenersIndex,
        Value::from_reference(*listeners));
    auto state_stored = machine.heap().set_field(
        *store, kStoreStateField, Value::from_reference(*state));
    auto open_stored = machine.heap().set_field(
        *store, kStoreOpenField, Value::from_int(1));
    if (!name_stored) return std::unexpected(name_stored.error());
    if (!listeners_stored) return std::unexpected(listeners_stored.error());
    if (!state_stored) return std::unexpected(state_stored.error());
    if (!open_stored) return std::unexpected(open_stored.error());
    track_open_handle(machine, *store);
    return *store;
}

[[nodiscard]] Result<std::optional<Value>> invoke_rms_callback(
    Machine& machine,
    ObjectRef callback,
    std::string_view declared_class,
    std::string_view method,
    std::string_view descriptor,
    std::span<const Value> arguments) {
    auto result = machine.invoke_instance(callback,
                                          declared_class,
                                          method,
                                          descriptor,
                                          arguments);
    if (!result) return std::unexpected(result.error());
    if (result->throwable.has_value()) {
        auto throwable_class = machine.heap().class_name(*result->throwable);
        if (!throwable_class) return std::unexpected(throwable_class.error());
        return fail_java(*throwable_class,
                         "RMS callback threw an exception");
    }
    return result->return_value;
}

[[nodiscard]] Result<std::vector<ObjectRef>> listener_snapshot(
    Machine& machine,
    ObjectRef handle) {
    auto listeners = store_listener_array(machine, handle);
    if (!listeners) return std::unexpected(listeners.error());
    auto length = machine.heap().array_length(*listeners);
    if (!length) return std::unexpected(length.error());
    std::vector<ObjectRef> result;
    result.reserve(*length);
    for (usize index = 0; index < *length; ++index) {
        auto value = machine.heap().element(*listeners, index);
        if (!value) return std::unexpected(value.error());
        auto listener = value->as_reference();
        if (!listener) return std::unexpected(listener.error());
        if (!listener->is_null()) result.push_back(*listener);
    }
    return result;
}

[[nodiscard]] Status replace_listener_array(
    Machine& machine,
    ObjectRef handle,
    std::span<const ObjectRef> listeners) {
    auto state = ensure_store_state(machine, handle);
    if (!state) return std::unexpected(state.error());
    auto array = machine.heap().allocate_array(
        "[Ljava/lang/Object;", listeners.size(),
        Value::from_reference({}));
    if (!array) return std::unexpected(array.error());
    for (usize index = 0; index < listeners.size(); ++index) {
        auto stored = machine.heap().set_element(
            *array, index, Value::from_reference(listeners[index]));
        if (!stored) return std::unexpected(stored.error());
    }
    return machine.heap().set_element(
        *state, kStoreStateListenersIndex,
        Value::from_reference(*array));
}

[[nodiscard]] Status notify_record_listeners(
    Machine& machine,
    std::string_view store_name,
    std::string_view method,
    i32 record_id) {
    const auto handles = open_handles_snapshot(machine);
    for (const ObjectRef handle : handles) {
        auto name = store_name_from_handle(machine, handle, true);
        if (!name) {
            untrack_open_handle(machine, handle);
            continue;
        }
        if (*name != store_name) continue;
        auto listeners = listener_snapshot(machine, handle);
        if (!listeners) return std::unexpected(listeners.error());
        const std::array<Value, 2> callback_arguments {
            Value::from_reference(handle),
            Value::from_int(record_id),
        };
        for (const ObjectRef listener : *listeners) {
            auto invoked = invoke_rms_callback(
                machine,
                listener,
                "javax/microedition/rms/RecordListener",
                method,
                "(Ljavax/microedition/rms/RecordStore;I)V",
                callback_arguments);
            if (!invoked) return std::unexpected(invoked.error());
        }
    }
    return {};
}

[[nodiscard]] Status require_enumeration_live(Machine& machine,
                                              ObjectRef enumeration) {
    auto destroyed = machine.heap().field(
        enumeration, kEnumerationDestroyedField);
    if (!destroyed) return std::unexpected(destroyed.error());
    auto flag = destroyed->as_int();
    if (!flag) return std::unexpected(flag.error());
    if (*flag != 0) {
        return fail_java("java/lang/IllegalStateException",
                         "RecordEnumeration is destroyed");
    }
    return {};
}

[[nodiscard]] Result<i32> enumeration_size(Machine& machine,
                                           ObjectRef enumeration) {
    auto records = machine.heap().field(enumeration,
                                        kEnumerationRecordsField);
    if (!records) return std::unexpected(records.error());
    auto array = records->as_reference();
    if (!array) return std::unexpected(array.error());
    auto size = machine.heap().array_length(*array);
    if (!size) return std::unexpected(size.error());
    if (*size > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return fail(ErrorCode::overflow,
                    "RecordEnumeration is too large");
    }
    return static_cast<i32>(*size);
}

[[nodiscard]] Result<i32> enumeration_index(Machine& machine,
                                            ObjectRef enumeration) {
    auto value = machine.heap().field(enumeration,
                                      kEnumerationIndexField);
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

[[nodiscard]] Status set_enumeration_index(Machine& machine,
                                           ObjectRef enumeration,
                                           i32 index) {
    return machine.heap().set_field(enumeration,
                                    kEnumerationIndexField,
                                    Value::from_int(index));
}

[[nodiscard]] Result<ObjectRef> clone_byte_array(Machine& machine,
                                                 ObjectRef source) {
    auto length = machine.heap().array_length(source);
    if (!length) return std::unexpected(length.error());
    auto clone = machine.heap().allocate_array(
        "[B", *length, Value::from_int(0));
    if (!clone) return std::unexpected(clone.error());
    for (usize index = 0; index < *length; ++index) {
        auto value = machine.heap().element(source, index);
        if (!value) return std::unexpected(value.error());
        auto stored = machine.heap().set_element(*clone, index, *value);
        if (!stored) return std::unexpected(stored.error());
    }
    return *clone;
}

struct EnumerationContext final {
    ObjectRef name;
    ObjectRef filter;
    ObjectRef comparator;
    ObjectRef store;
    i32 observed_version {0};
};

[[nodiscard]] Result<EnumerationContext> enumeration_context(
    Machine& machine,
    ObjectRef enumeration) {
    constexpr std::array<usize, 5> indices {
        kEnumerationStoreNameField,
        kEnumerationFilterField,
        kEnumerationComparatorField,
        kEnumerationStoreField,
        kEnumerationVersionField,
    };
    std::array<Value, indices.size()> values {};
    auto snapshot = machine.heap().read_fields(enumeration, indices, values);
    if (!snapshot) return std::unexpected(snapshot.error());
    const Value& name_value = values[0];
    const Value& filter_value = values[1];
    const Value& comparator_value = values[2];
    const Value& store_value = values[3];
    const Value& version_value = values[4];
    auto name = name_value.as_reference();
    auto filter = filter_value.as_reference();
    auto comparator = comparator_value.as_reference();
    auto store = store_value.as_reference();
    auto version = version_value.as_int();
    if (!name || !filter || !comparator || !store || !version ||
        name->is_null() || store->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "RecordEnumeration context fields are invalid");
    }
    return EnumerationContext {
        .name = *name,
        .filter = *filter,
        .comparator = *comparator,
        .store = *store,
        .observed_version = *version,
    };
}

[[nodiscard]] Status set_enumeration_version(Machine& machine,
                                             ObjectRef enumeration,
                                             i32 version) {
    return machine.heap().set_field(
        enumeration, kEnumerationVersionField, Value::from_int(version));
}

[[nodiscard]] Result<bool> filter_matches(
    Machine& machine,
    ObjectRef filter,
    std::span<const u8> bytes) {
    if (filter.is_null()) return true;
    auto candidate = create_byte_array(machine, bytes);
    if (!candidate) return std::unexpected(candidate.error());
    const std::array<Value, 1> arguments {
        Value::from_reference(*candidate),
    };
    auto result = invoke_rms_callback(
        machine,
        filter,
        "javax/microedition/rms/RecordFilter",
        "matches",
        "([B)Z",
        arguments);
    if (!result) return std::unexpected(result.error());
    if (!result->has_value()) {
        return fail(ErrorCode::invalid_state,
                    "RecordFilter.matches returned no value");
    }
    auto matched = (*result)->as_int();
    if (!matched) return std::unexpected(matched.error());
    return *matched != 0;
}

[[nodiscard]] Result<i32> compare_records(
    Machine& machine,
    ObjectRef comparator,
    const runtime::RecordSnapshot& first,
    const runtime::RecordSnapshot& second) {
    if (comparator.is_null()) return first.id < second.id ? -1
                                      : (first.id > second.id ? 1 : 0);
    auto first_array = create_byte_array(machine, first.bytes);
    auto second_array = create_byte_array(machine, second.bytes);
    if (!first_array) return std::unexpected(first_array.error());
    if (!second_array) return std::unexpected(second_array.error());
    const std::array<Value, 2> arguments {
        Value::from_reference(*first_array),
        Value::from_reference(*second_array),
    };
    auto result = invoke_rms_callback(
        machine,
        comparator,
        "javax/microedition/rms/RecordComparator",
        "compare",
        "([B[B)I",
        arguments);
    if (!result) return std::unexpected(result.error());
    if (!result->has_value()) {
        return fail(ErrorCode::invalid_state,
                    "RecordComparator.compare returned no value");
    }
    return (*result)->as_int();
}

[[nodiscard]] Result<std::vector<runtime::RecordSnapshot>>
filtered_and_sorted_snapshot(Machine& machine,
                             std::string_view store_name,
                             ObjectRef filter,
                             ObjectRef comparator,
                             i32& observed_version) {
    std::vector<runtime::RecordSnapshot> snapshot;
    for (usize attempt = 0; attempt < 3U; ++attempt) {
        auto before = machine.record_stores().info(store_name);
        if (!before) return rms_failure(before.error());
        auto records = machine.record_stores().snapshot(store_name);
        if (!records) return rms_failure(records.error());
        auto after = machine.record_stores().info(store_name);
        if (!after) return rms_failure(after.error());
        if (before->version == after->version) {
            snapshot = std::move(*records);
            observed_version = after->version;
            break;
        }
        if (attempt == 2U) {
            return fail_java(
                "javax/microedition/rms/RecordStoreException",
                "RecordStore changed continuously while enumerating");
        }
    }

    std::vector<runtime::RecordSnapshot> filtered;
    filtered.reserve(snapshot.size());
    for (auto& record : snapshot) {
        auto matched = filter_matches(machine, filter, record.bytes);
        if (!matched) return std::unexpected(matched.error());
        if (*matched) filtered.push_back(std::move(record));
    }
    if (!comparator.is_null()) {
        for (usize index = 1; index < filtered.size(); ++index) {
            auto current = std::move(filtered[index]);
            usize position = index;
            while (position > 0U) {
                auto comparison = compare_records(
                    machine, comparator, current, filtered[position - 1U]);
                if (!comparison) return std::unexpected(comparison.error());
                if (*comparison >= 0) break;
                filtered[position] = std::move(filtered[position - 1U]);
                --position;
            }
            filtered[position] = std::move(current);
        }
    }
    return filtered;
}

[[nodiscard]] Status populate_enumeration(
    Machine& machine,
    ObjectRef enumeration,
    bool reset_index) {
    auto context = enumeration_context(machine, enumeration);
    if (!context) return std::unexpected(context.error());
    auto store_name = store_name_from_handle(
        machine, context->store, true);
    if (!store_name) return std::unexpected(store_name.error());

    i32 old_index = 0;
    std::optional<i32> next_anchor_id;
    std::optional<i32> previous_anchor_id;
    if (!reset_index) {
        auto current_index = enumeration_index(machine, enumeration);
        if (!current_index) return std::unexpected(current_index.error());
        old_index = std::max(0, *current_index);
        auto ids_value = machine.heap().field(
            enumeration, kEnumerationIdsField);
        if (!ids_value) return std::unexpected(ids_value.error());
        auto ids = ids_value->as_reference();
        if (!ids) return std::unexpected(ids.error());
        if (!ids->is_null()) {
            auto length = machine.heap().array_length(*ids);
            if (!length) return std::unexpected(length.error());
            const auto read_anchor = [&](usize index)
                -> Result<i32> {
                auto value = machine.heap().element(*ids, index);
                if (!value) return std::unexpected(value.error());
                return value->as_int();
            };
            if (static_cast<usize>(old_index) < *length) {
                auto anchor = read_anchor(static_cast<usize>(old_index));
                if (!anchor) return std::unexpected(anchor.error());
                next_anchor_id = *anchor;
            }
            if (old_index > 0 &&
                static_cast<usize>(old_index - 1) < *length) {
                auto anchor = read_anchor(
                    static_cast<usize>(old_index - 1));
                if (!anchor) return std::unexpected(anchor.error());
                previous_anchor_id = *anchor;
            }
        }
    }

    i32 observed_version = 0;
    auto snapshot = filtered_and_sorted_snapshot(
        machine,
        *store_name,
        context->filter,
        context->comparator,
        observed_version);
    if (!snapshot) return std::unexpected(snapshot.error());
    auto records = machine.heap().allocate_array(
        "[[B", snapshot->size(), Value::from_reference({}));
    auto ids = machine.heap().allocate_array(
        "[I", snapshot->size(), Value::from_int(0));
    if (!records) return std::unexpected(records.error());
    if (!ids) return std::unexpected(ids.error());
    for (usize index = 0; index < snapshot->size(); ++index) {
        auto record = create_byte_array(machine, (*snapshot)[index].bytes);
        if (!record) return std::unexpected(record.error());
        auto record_stored = machine.heap().set_element(
            *records, index, Value::from_reference(*record));
        auto id_stored = machine.heap().set_element(
            *ids, index, Value::from_int((*snapshot)[index].id));
        if (!record_stored) return record_stored;
        if (!id_stored) return id_stored;
    }

    i32 new_index = 0;
    if (!reset_index) {
        new_index = std::min<i32>(
            old_index, static_cast<i32>(snapshot->size()));
        const auto find_record = [&](i32 record_id) {
            return std::find_if(
                snapshot->begin(), snapshot->end(),
                [record_id](const runtime::RecordSnapshot& record) {
                    return record.id == record_id;
                });
        };
        if (next_anchor_id.has_value()) {
            const auto found = find_record(*next_anchor_id);
            if (found != snapshot->end()) {
                new_index = static_cast<i32>(
                    std::distance(snapshot->begin(), found));
            } else if (previous_anchor_id.has_value()) {
                const auto previous = find_record(*previous_anchor_id);
                if (previous != snapshot->end()) {
                    new_index = static_cast<i32>(
                        std::distance(snapshot->begin(), previous) + 1);
                }
            }
        } else if (previous_anchor_id.has_value()) {
            const auto previous = find_record(*previous_anchor_id);
            if (previous != snapshot->end()) {
                new_index = static_cast<i32>(
                    std::distance(snapshot->begin(), previous) + 1);
            }
        }
    }
    auto records_stored = machine.heap().set_field(
        enumeration, kEnumerationRecordsField,
        Value::from_reference(*records));
    auto ids_stored = machine.heap().set_field(
        enumeration, kEnumerationIdsField,
        Value::from_reference(*ids));
    auto index_stored = set_enumeration_index(
        machine, enumeration, new_index);
    auto version_stored = set_enumeration_version(
        machine, enumeration, observed_version);
    if (!records_stored) return records_stored;
    if (!ids_stored) return ids_stored;
    if (!index_stored) return index_stored;
    return version_stored;
}

[[nodiscard]] Status refresh_enumeration_if_needed(
    Machine& machine,
    ObjectRef enumeration) {
    auto context = enumeration_context(machine, enumeration);
    if (!context) return std::unexpected(context.error());
    auto store_name = store_name_from_handle(
        machine, context->store, true);
    if (!store_name) return std::unexpected(store_name.error());
    auto keep_value = machine.heap().field(
        enumeration, kEnumerationKeepUpdatedField);
    if (!keep_value) return std::unexpected(keep_value.error());
    auto keep = keep_value->as_int();
    if (!keep) return std::unexpected(keep.error());
    if (*keep == 0) return {};
    auto info = machine.record_stores().info(*store_name);
    if (!info) return rms_failure(info.error());
    if (info->version == context->observed_version) return {};
    return populate_enumeration(machine, enumeration, false);
}

[[nodiscard]] Result<ObjectRef> create_enumeration(
    Machine& machine,
    ObjectRef store,
    ObjectRef filter,
    ObjectRef comparator,
    bool keep_updated) {
    auto store_name = store_name_reference_from_handle(
        machine, store, true);
    if (!store_name) return std::unexpected(store_name.error());
    auto enumeration = machine.class_states().allocate_instance(
        machine.heap(),
        "javax/microedition/rms/SnapshotRecordEnumeration");
    if (!enumeration) return std::unexpected(enumeration.error());
    constexpr std::array<usize, 5> context_indices {
        kEnumerationStoreNameField,
        kEnumerationFilterField,
        kEnumerationComparatorField,
        kEnumerationStoreField,
        kEnumerationVersionField,
    };
    const std::array<Value, context_indices.size()> context_values {
        Value::from_reference(*store_name),
        Value::from_reference(filter),
        Value::from_reference(comparator),
        Value::from_reference(store),
        Value::from_int(0),
    };
    auto context_stored = machine.heap().write_fields(
        *enumeration, context_indices, context_values);
    if (!context_stored) return std::unexpected(context_stored.error());
    auto destroyed_stored = machine.heap().set_field(
        *enumeration, kEnumerationDestroyedField, Value::from_int(0));
    auto keep_stored = machine.heap().set_field(
        *enumeration, kEnumerationKeepUpdatedField,
        Value::from_int(keep_updated ? 1 : 0));
    if (!destroyed_stored) return std::unexpected(destroyed_stored.error());
    if (!keep_stored) return std::unexpected(keep_stored.error());
    auto populated = populate_enumeration(machine, *enumeration, true);
    if (!populated) return std::unexpected(populated.error());
    return *enumeration;
}

[[nodiscard]] Result<std::optional<Value>> enumeration_record(
    Machine& machine,
    std::span<const Value> arguments,
    bool previous,
    bool return_id) {
    auto enumeration = receiver(arguments);
    if (!enumeration) return std::unexpected(enumeration.error());
    auto live = require_enumeration_live(machine, *enumeration);
    if (!live) return std::unexpected(live.error());
    auto refreshed = refresh_enumeration_if_needed(machine, *enumeration);
    if (!refreshed) return std::unexpected(refreshed.error());
    auto size = enumeration_size(machine, *enumeration);
    auto index = enumeration_index(machine, *enumeration);
    if (!size || !index)
        return fail(ErrorCode::invalid_state,
                    "RecordEnumeration state is invalid");
    i32 selected = *index;
    if (previous) {
        if (selected <= 0) {
            return fail_java(
                "javax/microedition/rms/InvalidRecordIDException",
                "RecordEnumeration has no previous record");
        }
        --selected;
    } else if (selected >= *size) {
        return fail_java(
            "javax/microedition/rms/InvalidRecordIDException",
            "RecordEnumeration has no next record");
    }
    auto updated = set_enumeration_index(
        machine, *enumeration, previous ? selected : selected + 1);
    if (!updated) return std::unexpected(updated.error());

    const usize field = return_id ? kEnumerationIdsField
                                  : kEnumerationRecordsField;
    auto array_value = machine.heap().field(*enumeration, field);
    if (!array_value) return std::unexpected(array_value.error());
    auto array = array_value->as_reference();
    if (!array) return std::unexpected(array.error());
    auto value = machine.heap().element(*array,
                                        static_cast<usize>(selected));
    if (!value) return std::unexpected(value.error());
    if (return_id) return std::optional<Value>(*value);
    auto record = value->as_reference();
    if (!record) return std::unexpected(record.error());
    auto clone = clone_byte_array(machine, *record);
    if (!clone) return std::unexpected(clone.error());
    return std::optional<Value>(Value::from_reference(*clone));
}

} // namespace

void register_rms_natives(NativeMethodRegistry& registry) {
    const auto open_store = [&registry](std::string descriptor,
                                        bool extended) {
        add(registry,
            "javax/microedition/rms/RecordStore",
            "openRecordStore",
            std::move(descriptor),
            [extended](Machine& machine,
                       std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto name_reference = arguments[0].as_reference();
                auto create = arguments[1].as_int();
                if (!name_reference || name_reference->is_null())
                    return fail_java("java/lang/NullPointerException",
                                     "RecordStore name is null");
                if (!create) return std::unexpected(create.error());
                if (extended) {
                    auto auth_mode = arguments[2].as_int();
                    auto writable = arguments[3].as_int();
                    if (!auth_mode || !writable)
                        return fail(ErrorCode::invalid_argument,
                                    "RecordStore access arguments are invalid");
                    if (*auth_mode != 0 && *auth_mode != 1)
                        return fail_java("java/lang/IllegalArgumentException",
                                         "RecordStore auth mode is invalid");
                }
                auto name = utf8_text(machine, *name_reference);
                if (!name) return std::unexpected(name.error());
                auto opened = machine.record_stores().open(
                    *name, *create != 0);
                if (!opened) return rms_failure(opened.error());
                auto handle = create_store_handle(machine, *name_reference);
                if (!handle) {
                    (void)machine.record_stores().close(*name);
                    return std::unexpected(handle.error());
                }
                return std::optional<Value>(Value::from_reference(*handle));
            });
    };
    open_store("(Ljava/lang/String;Z)Ljavax/microedition/rms/RecordStore;",
               false);
    open_store("(Ljava/lang/String;ZIZ)Ljavax/microedition/rms/RecordStore;",
               true);

    add(registry,
        "javax/microedition/rms/RecordStore",
        "openRecordStore",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)"
        "Ljavax/microedition/rms/RecordStore;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 3U) {
                return fail(ErrorCode::invalid_argument,
                            "cross-suite openRecordStore expects three strings");
            }
            auto name_reference = arguments[0].as_reference();
            auto vendor_reference = arguments[1].as_reference();
            auto suite_reference = arguments[2].as_reference();
            if (!name_reference || !vendor_reference || !suite_reference) {
                return fail(ErrorCode::invalid_argument,
                            "cross-suite RecordStore arguments are invalid");
            }
            if (vendor_reference->is_null() || suite_reference->is_null()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "vendorName and suiteName must be non null");
            }
            if (name_reference->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "RecordStore name is null");
            }
            auto name = utf8_text(machine, *name_reference);
            auto vendor = utf8_text(machine, *vendor_reference);
            auto suite = utf8_text(machine, *suite_reference);
            if (!name) return std::unexpected(name.error());
            if (!vendor) return std::unexpected(vendor.error());
            if (!suite) return std::unexpected(suite.error());
            auto opened = machine.record_stores().open(*name, false);
            if (!opened) return rms_failure(opened.error());
            auto handle = create_store_handle(machine, *name_reference);
            if (!handle) return std::unexpected(handle.error());
            track_open_handle(machine, *handle);
            return std::optional<Value>(Value::from_reference(*handle));
        });

    add(registry,
        "javax/microedition/rms/RecordStore",
        "deleteRecordStore",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto name_reference = arguments[0].as_reference();
            if (!name_reference || name_reference->is_null())
                return fail_java("java/lang/NullPointerException",
                                 "RecordStore name is null");
            auto name = utf8_text(machine, *name_reference);
            if (!name) return std::unexpected(name.error());
            auto deleted = machine.record_stores().delete_store(*name);
            if (!deleted) return rms_failure(deleted.error());
            return std::optional<Value> {};
        });
    add(registry,
        "javax/microedition/rms/RecordStore",
        "listRecordStores",
        "()[Ljava/lang/String;",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            auto names = machine.record_stores().list_store_names();
            if (!names) return rms_failure(names.error());
            if (names->empty())
                return std::optional<Value>(Value::from_reference({}));
            auto array = machine.heap().allocate_array(
                "[Ljava/lang/String;", names->size(),
                Value::from_reference({}));
            if (!array) return std::unexpected(array.error());
            for (usize index = 0; index < names->size(); ++index) {
                auto decoded = decode_utf8((*names)[index]);
                if (!decoded) return std::unexpected(decoded.error());
                auto string = create_string(machine, std::move(*decoded));
                if (!string) return std::unexpected(string.error());
                auto stored = machine.heap().set_element(
                    *array, index, Value::from_reference(*string));
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value>(Value::from_reference(*array));
        });

    add(registry,
        "javax/microedition/rms/RecordStore",
        "closeRecordStore",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto handle = receiver(arguments);
            if (!handle) return std::unexpected(handle.error());
            auto name = store_name_from_handle(machine, *handle, true);
            if (!name) return std::unexpected(name.error());
            auto closed = machine.record_stores().close(*name);
            if (!closed) return rms_failure(closed.error());
            auto open_stored = machine.heap().set_field(
                *handle, kStoreOpenField, Value::from_int(0));
            if (!open_stored) return std::unexpected(open_stored.error());
            untrack_open_handle(machine, *handle);
            return std::optional<Value> {};
        });
    add(registry,
        "javax/microedition/rms/RecordStore",
        "setMode",
        "(IZ)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto handle = receiver(arguments);
            if (!handle) return std::unexpected(handle.error());
            if (arguments.size() != 3U) {
                return fail(ErrorCode::invalid_argument,
                            "RecordStore.setMode expects authmode and writable");
            }
            auto authmode = arguments[1].as_int();
            auto writable = arguments[2].as_int();
            if (!authmode) return std::unexpected(authmode.error());
            if (!writable) return std::unexpected(writable.error());
            auto name = store_name_from_handle(machine, *handle, true);
            if (!name) return std::unexpected(name.error());
            if (*authmode != 0 && *authmode != 1) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "RecordStore authmode is invalid");
            }
            // The storage root already enforces owner-suite isolation. ACL
            // metadata for cross-suite sharing is intentionally deferred until
            // Runtime exposes per-suite vendor/name roots.
            return std::optional<Value> {};
        });
    add(registry,
        "javax/microedition/rms/RecordStore",
        "getName",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto handle = receiver(arguments);
            if (!handle) return std::unexpected(handle.error());
            auto name = store_name_reference_from_handle(
                machine, *handle, true);
            if (!name) return std::unexpected(name.error());
            return std::optional<Value>(Value::from_reference(*name));
        });

    const auto add_info_int = [&registry](const char* name,
                                         auto select) {
        add(registry,
            "javax/microedition/rms/RecordStore",
            name,
            "()I",
            [select](Machine& machine,
                     std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto handle = receiver(arguments);
                if (!handle) return std::unexpected(handle.error());
                auto store_name = store_name_from_handle(
                    machine, *handle, true);
                if (!store_name) return std::unexpected(store_name.error());
                auto info = machine.record_stores().info(*store_name);
                if (!info) return rms_failure(info.error());
                const i64 selected = select(*info);
                const i64 clamped = std::clamp<i64>(
                    selected,
                    std::numeric_limits<i32>::min(),
                    std::numeric_limits<i32>::max());
                return std::optional<Value>(
                    Value::from_int(static_cast<i32>(clamped)));
            });
    };
    add_info_int("getVersion", [](const runtime::RecordStoreInfo& info) {
        return static_cast<i64>(info.version);
    });
    add_info_int("getNumRecords", [](const runtime::RecordStoreInfo& info) {
        return static_cast<i64>(info.record_count);
    });
    add_info_int("getSize", [](const runtime::RecordStoreInfo& info) {
        return static_cast<i64>(info.total_record_bytes);
    });
    add_info_int("getNextRecordID", [](const runtime::RecordStoreInfo& info) {
        return static_cast<i64>(info.next_record_id);
    });
    add(registry,
        "javax/microedition/rms/RecordStore",
        "getSizeAvailable",
        "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto handle = receiver(arguments);
            if (!handle) return std::unexpected(handle.error());
            auto name = store_name_from_handle(machine, *handle, true);
            if (!name) return std::unexpected(name.error());
            auto available = machine.record_stores().available_bytes(*name);
            if (!available) return rms_failure(available.error());
            return std::optional<Value>(Value::from_int(static_cast<i32>(
                std::min(*available,
                         static_cast<usize>(
                             std::numeric_limits<i32>::max())))));
        });
    add(registry,
        "javax/microedition/rms/RecordStore",
        "getLastModified",
        "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto handle = receiver(arguments);
            if (!handle) return std::unexpected(handle.error());
            auto name = store_name_from_handle(machine, *handle, true);
            if (!name) return std::unexpected(name.error());
            auto info = machine.record_stores().info(*name);
            if (!info) return rms_failure(info.error());
            return std::optional<Value>(
                Value::from_long(info->last_modified_ms));
        });

    add(registry,
        "javax/microedition/rms/RecordStore",
        "addRecord",
        "([BII)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto handle = receiver(arguments);
            auto data = arguments[1].as_reference();
            auto offset = arguments[2].as_int();
            auto length = arguments[3].as_int();
            if (!handle || !data || !offset || !length)
                return fail(ErrorCode::invalid_argument,
                            "RecordStore.addRecord arguments are invalid");
            auto name = store_name_from_handle(machine, *handle, true);
            if (!name) return std::unexpected(name.error());
            auto bytes = record_slice(machine, *data, *offset, *length);
            if (!bytes) return std::unexpected(bytes.error());
            auto id = machine.record_stores().add_record(*name, *bytes);
            if (!id) return rms_failure(id.error());
            auto notified = notify_record_listeners(
                machine, *name, "recordAdded", *id);
            if (!notified) return std::unexpected(notified.error());
            return std::optional<Value>(Value::from_int(*id));
        });
    add(registry,
        "javax/microedition/rms/RecordStore",
        "setRecord",
        "(I[BII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto handle = receiver(arguments);
            auto id = arguments[1].as_int();
            auto data = arguments[2].as_reference();
            auto offset = arguments[3].as_int();
            auto length = arguments[4].as_int();
            if (!handle || !id || !data || !offset || !length)
                return fail(ErrorCode::invalid_argument,
                            "RecordStore.setRecord arguments are invalid");
            auto name = store_name_from_handle(machine, *handle, true);
            if (!name) return std::unexpected(name.error());
            auto bytes = record_slice(machine, *data, *offset, *length);
            if (!bytes) return std::unexpected(bytes.error());
            auto updated = machine.record_stores().set_record(
                *name, *id, *bytes);
            if (!updated) return rms_failure(updated.error());
            auto notified = notify_record_listeners(
                machine, *name, "recordChanged", *id);
            if (!notified) return std::unexpected(notified.error());
            return std::optional<Value> {};
        });
    add(registry,
        "javax/microedition/rms/RecordStore",
        "deleteRecord",
        "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto handle = receiver(arguments);
            auto id = arguments[1].as_int();
            if (!handle) return std::unexpected(handle.error());
            if (!id) return std::unexpected(id.error());
            auto name = store_name_from_handle(machine, *handle, true);
            if (!name) return std::unexpected(name.error());
            auto deleted = machine.record_stores().delete_record(*name, *id);
            if (!deleted) return rms_failure(deleted.error());
            auto notified = notify_record_listeners(
                machine, *name, "recordDeleted", *id);
            if (!notified) return std::unexpected(notified.error());
            return std::optional<Value> {};
        });

    const auto get_record = [&registry](bool into_buffer) {
        add(registry,
            "javax/microedition/rms/RecordStore",
            "getRecord",
            into_buffer ? "(I[BI)I" : "(I)[B",
            [into_buffer](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto handle = receiver(arguments);
                auto id = arguments[1].as_int();
                if (!handle) return std::unexpected(handle.error());
                if (!id) return std::unexpected(id.error());
                auto name = store_name_from_handle(machine, *handle, true);
                if (!name) return std::unexpected(name.error());
                auto bytes = machine.record_stores().record(*name, *id);
                if (!bytes) return rms_failure(bytes.error());
                if (!into_buffer) {
                    auto array = create_byte_array(machine, *bytes);
                    if (!array) return std::unexpected(array.error());
                    return std::optional<Value>(Value::from_reference(*array));
                }
                auto destination = arguments[2].as_reference();
                auto offset = arguments[3].as_int();
                if (!destination || destination->is_null())
                    return fail_java("java/lang/NullPointerException",
                                     "RecordStore destination is null");
                if (!offset) return std::unexpected(offset.error());
                auto class_name = machine.heap().class_name(*destination);
                auto length = machine.heap().array_length(*destination);
                if (!class_name || *class_name != "[B" || !length)
                    return fail_java("java/lang/IllegalArgumentException",
                                     "RecordStore destination is not byte[]");
                if (*offset < 0 ||
                    static_cast<usize>(*offset) > *length ||
                    bytes->size() > *length - static_cast<usize>(*offset))
                    return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                     "RecordStore destination is too small");
                for (usize index = 0; index < bytes->size(); ++index) {
                    auto stored = machine.heap().set_element(
                        *destination,
                        static_cast<usize>(*offset) + index,
                        Value::from_int(static_cast<i32>(
                            static_cast<i8>((*bytes)[index]))));
                    if (!stored) return std::unexpected(stored.error());
                }
                return std::optional<Value>(Value::from_int(
                    static_cast<i32>(bytes->size())));
            });
    };
    get_record(false);
    get_record(true);
    add(registry,
        "javax/microedition/rms/RecordStore",
        "getRecordSize",
        "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto handle = receiver(arguments);
            auto id = arguments[1].as_int();
            if (!handle) return std::unexpected(handle.error());
            if (!id) return std::unexpected(id.error());
            auto name = store_name_from_handle(machine, *handle, true);
            if (!name) return std::unexpected(name.error());
            auto bytes = machine.record_stores().record(*name, *id);
            if (!bytes) return rms_failure(bytes.error());
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(bytes->size())));
        });

    add(registry,
        "javax/microedition/rms/RecordStore",
        "enumerateRecords",
        "(Ljavax/microedition/rms/RecordFilter;Ljavax/microedition/rms/RecordComparator;Z)Ljavax/microedition/rms/RecordEnumeration;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto handle = receiver(arguments);
            auto filter = arguments[1].as_reference();
            auto comparator = arguments[2].as_reference();
            auto keep_updated = arguments[3].as_int();
            if (!handle || !filter || !comparator || !keep_updated)
                return fail(ErrorCode::invalid_argument,
                            "RecordStore.enumerateRecords arguments are invalid");
            auto name = store_name_from_handle(machine, *handle, true);
            if (!name) return std::unexpected(name.error());
            if (!filter->is_null()) {
                auto compatible = machine.object_is_instance(
                    *filter, "javax/microedition/rms/RecordFilter");
                if (!compatible) return std::unexpected(compatible.error());
                if (!*compatible) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "RMS filter does not implement RecordFilter");
                }
            }
            if (!comparator->is_null()) {
                auto compatible = machine.object_is_instance(
                    *comparator,
                    "javax/microedition/rms/RecordComparator");
                if (!compatible) return std::unexpected(compatible.error());
                if (!*compatible) {
                    return fail_java(
                        "java/lang/IllegalArgumentException",
                        "RMS comparator does not implement RecordComparator");
                }
            }
            auto enumeration = create_enumeration(
                machine, *handle, *filter, *comparator,
                *keep_updated != 0);
            if (!enumeration) return std::unexpected(enumeration.error());
            return std::optional<Value>(Value::from_reference(*enumeration));
        });

    const auto register_listener_method = [&registry](const char* name,
                                                      bool add_listener) {
        add(registry,
            "javax/microedition/rms/RecordStore",
            name,
            "(Ljavax/microedition/rms/RecordListener;)V",
            [add_listener](Machine& machine,
                           std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto handle = receiver(arguments);
                auto listener = arguments[1].as_reference();
                if (!handle) return std::unexpected(handle.error());
                if (!listener) return std::unexpected(listener.error());
                auto store_name = store_name_from_handle(
                    machine, *handle, true);
                if (!store_name) return std::unexpected(store_name.error());
                (void)store_name;
                if (listener->is_null()) {
                    if (!add_listener) return std::optional<Value> {};
                    return fail_java("java/lang/NullPointerException",
                                     "RecordListener is null");
                }
                auto compatible = machine.object_is_instance(
                    *listener, "javax/microedition/rms/RecordListener");
                if (!compatible) return std::unexpected(compatible.error());
                if (!*compatible) {
                    return fail_java(
                        "java/lang/IllegalArgumentException",
                        "listener does not implement RecordListener");
                }
                auto listeners = listener_snapshot(machine, *handle);
                if (!listeners) return std::unexpected(listeners.error());
                const auto found = std::find(
                    listeners->begin(), listeners->end(), *listener);
                if (add_listener) {
                    if (found == listeners->end()) {
                        listeners->push_back(*listener);
                    }
                } else if (found != listeners->end()) {
                    listeners->erase(found);
                }
                auto replaced = replace_listener_array(
                    machine, *handle, *listeners);
                if (!replaced) return std::unexpected(replaced.error());
                return std::optional<Value> {};
            });
    };
    register_listener_method("addRecordListener", true);
    register_listener_method("removeRecordListener", false);

    add(registry,
        "javax/microedition/rms/SnapshotRecordEnumeration",
        "numRecords",
        "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto enumeration = receiver(arguments);
            if (!enumeration) return std::unexpected(enumeration.error());
            auto live = require_enumeration_live(machine, *enumeration);
            if (!live) return std::unexpected(live.error());
            auto refreshed = refresh_enumeration_if_needed(
                machine, *enumeration);
            if (!refreshed) return std::unexpected(refreshed.error());
            auto size = enumeration_size(machine, *enumeration);
            if (!size) return std::unexpected(size.error());
            return std::optional<Value>(Value::from_int(*size));
        });
    const auto has_element = [&registry](const char* name, bool previous) {
        add(registry,
            "javax/microedition/rms/SnapshotRecordEnumeration",
            name,
            "()Z",
            [previous](Machine& machine,
                       std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto enumeration = receiver(arguments);
                if (!enumeration) return std::unexpected(enumeration.error());
                auto live = require_enumeration_live(machine, *enumeration);
                if (!live) return std::unexpected(live.error());
                auto refreshed = refresh_enumeration_if_needed(
                    machine, *enumeration);
                if (!refreshed) return std::unexpected(refreshed.error());
                auto size = enumeration_size(machine, *enumeration);
                auto index = enumeration_index(machine, *enumeration);
                if (!size || !index)
                    return fail(ErrorCode::invalid_state,
                                "RecordEnumeration state is invalid");
                const bool available = previous ? *index > 0
                                                : *index < *size;
                return std::optional<Value>(
                    Value::from_int(available ? 1 : 0));
            });
    };
    has_element("hasNextElement", false);
    has_element("hasPreviousElement", true);

    const auto add_enumeration_record = [&registry](const char* name,
                                                    bool previous,
                                                    bool return_id) {
        add(registry,
            "javax/microedition/rms/SnapshotRecordEnumeration",
            name,
            return_id ? "()I" : "()[B",
            [previous, return_id](Machine& machine,
                                  std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                return enumeration_record(machine, arguments,
                                          previous, return_id);
            });
    };
    add_enumeration_record("nextRecordId", false, true);
    add_enumeration_record("nextRecord", false, false);
    add_enumeration_record("previousRecordId", true, true);
    add_enumeration_record("previousRecord", true, false);

    add(registry,
        "javax/microedition/rms/SnapshotRecordEnumeration",
        "reset",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto enumeration = receiver(arguments);
            if (!enumeration) return std::unexpected(enumeration.error());
            auto live = require_enumeration_live(machine, *enumeration);
            if (!live) return std::unexpected(live.error());
            auto refreshed = refresh_enumeration_if_needed(
                machine, *enumeration);
            if (!refreshed) return std::unexpected(refreshed.error());
            auto reset = set_enumeration_index(machine, *enumeration, 0);
            if (!reset) return std::unexpected(reset.error());
            return std::optional<Value> {};
        });
    add(registry,
        "javax/microedition/rms/SnapshotRecordEnumeration",
        "rebuild",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto enumeration = receiver(arguments);
            if (!enumeration) return std::unexpected(enumeration.error());
            auto live = require_enumeration_live(machine, *enumeration);
            if (!live) return std::unexpected(live.error());
            auto rebuilt = populate_enumeration(
                machine, *enumeration, true);
            if (!rebuilt) return std::unexpected(rebuilt.error());
            return std::optional<Value> {};
        });
    add(registry,
        "javax/microedition/rms/SnapshotRecordEnumeration",
        "keepUpdated",
        "(Z)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto enumeration = receiver(arguments);
            auto keep = arguments[1].as_int();
            if (!enumeration) return std::unexpected(enumeration.error());
            if (!keep) return std::unexpected(keep.error());
            auto live = require_enumeration_live(machine, *enumeration);
            if (!live) return std::unexpected(live.error());
            auto stored = machine.heap().set_field(
                *enumeration, kEnumerationKeepUpdatedField,
                Value::from_int(*keep == 0 ? 0 : 1));
            if (!stored) return std::unexpected(stored.error());
            if (*keep != 0) {
                auto rebuilt = populate_enumeration(
                    machine, *enumeration, false);
                if (!rebuilt) return std::unexpected(rebuilt.error());
            }
            return std::optional<Value> {};
        });
    add(registry,
        "javax/microedition/rms/SnapshotRecordEnumeration",
        "isKeptUpdated",
        "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto enumeration = receiver(arguments);
            if (!enumeration) return std::unexpected(enumeration.error());
            auto live = require_enumeration_live(machine, *enumeration);
            if (!live) return std::unexpected(live.error());
            auto value = machine.heap().field(
                *enumeration, kEnumerationKeepUpdatedField);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(*value);
        });
    add(registry,
        "javax/microedition/rms/SnapshotRecordEnumeration",
        "destroy",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto enumeration = receiver(arguments);
            if (!enumeration) return std::unexpected(enumeration.error());
            auto destroyed = machine.heap().set_field(
                *enumeration, kEnumerationDestroyedField,
                Value::from_int(1));
            auto records = machine.heap().set_field(
                *enumeration, kEnumerationRecordsField,
                Value::from_reference({}));
            auto ids = machine.heap().set_field(
                *enumeration, kEnumerationIdsField,
                Value::from_reference({}));
            constexpr std::array<usize, 4> context_indices {
                kEnumerationStoreNameField,
                kEnumerationFilterField,
                kEnumerationComparatorField,
                kEnumerationStoreField,
            };
            const std::array<Value, context_indices.size()> null_context {
                Value::from_reference({}),
                Value::from_reference({}),
                Value::from_reference({}),
                Value::from_reference({}),
            };
            auto state = machine.heap().write_fields(
                *enumeration, context_indices, null_context);
            if (!destroyed) return std::unexpected(destroyed.error());
            if (!records) return std::unexpected(records.error());
            if (!ids) return std::unexpected(ids.error());
            if (!state) return std::unexpected(state.error());
            return std::optional<Value> {};
        });
}

} // namespace phoneme::vm
