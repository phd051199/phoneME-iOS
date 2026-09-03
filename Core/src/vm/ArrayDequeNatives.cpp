#include "ArrayDequeNatives.hpp"

#include <algorithm>
#include <array>
#include <array>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/NativeMethodRegistry.hpp"

namespace phoneme::vm {
namespace {

constexpr usize kElementsField = 0U;
constexpr usize kSizeField = 1U;
constexpr usize kIteratorValuesField = 0U;
constexpr usize kIteratorIndexField = 1U;
constexpr usize kIteratorSizeField = 2U;
constexpr usize kDefaultCapacity = 16U;

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

[[nodiscard]] Result<ObjectRef> receiver(std::span<const Value> arguments) {
    if (arguments.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "ArrayDeque method has no receiver");
    }
    auto reference = arguments.front().as_reference();
    if (!reference || reference->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "ArrayDeque receiver is null");
    }
    return *reference;
}

[[nodiscard]] Result<ObjectRef> reference_argument(
    std::span<const Value> arguments,
    usize index,
    bool nullable = false) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    "ArrayDeque reference argument is missing");
    }
    auto reference = arguments[index].as_reference();
    if (!reference) return std::unexpected(reference.error());
    if (!nullable && reference->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "ArrayDeque does not permit null elements");
    }
    return *reference;
}

[[nodiscard]] Result<i32> int_argument(std::span<const Value> arguments,
                                       usize index) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    "ArrayDeque int argument is missing");
    }
    return arguments[index].as_int();
}

[[nodiscard]] Result<i32> int_field(Machine& machine,
                                    ObjectRef object,
                                    usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_int();
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

[[nodiscard]] Status set_reference_field(Machine& machine,
                                         ObjectRef object,
                                         usize index,
                                         ObjectRef value) {
    return machine.heap().set_field(object, index,
                                    Value::from_reference(value));
}

[[nodiscard]] Result<ObjectRef> allocate_object_array(Machine& machine,
                                                       usize length) {
    return machine.heap().allocate_array(
        "[Ljava/lang/Object;", length, Value::from_reference({}));
}

struct DequeState final {
    ObjectRef elements {};
    i32 size {0};
};

[[nodiscard]] Result<DequeState> deque_state(Machine& machine,
                                             ObjectRef deque) {
    constexpr std::array<usize, 2> fields {
        kElementsField,
        kSizeField,
    };
    std::array<Value, fields.size()> values {};
    auto read = machine.heap().read_fields(deque, fields, values);
    if (!read) return std::unexpected(read.error());
    auto elements = values[0U].as_reference();
    auto size = values[1U].as_int();
    if (!elements || elements->is_null() || !size || *size < 0) {
        return fail(ErrorCode::invalid_state,
                    "ArrayDeque state is invalid");
    }
    return DequeState {.elements = *elements, .size = *size};
}

[[nodiscard]] Status initialize_deque(Machine& machine,
                                      ObjectRef deque,
                                      i32 requested_capacity) {
    if (requested_capacity < 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         "ArrayDeque capacity is negative");
    }
    const usize capacity = std::max<usize>(
        1U, requested_capacity == 0
                ? 1U
                : static_cast<usize>(requested_capacity));
    auto elements = allocate_object_array(machine, capacity);
    if (!elements) return std::unexpected(elements.error());
    constexpr std::array<usize, 2> fields {
        kElementsField,
        kSizeField,
    };
    const std::array<Value, fields.size()> values {
        Value::from_reference(*elements),
        Value::from_int(0),
    };
    return machine.heap().write_fields(deque, fields, values);
}

[[nodiscard]] Result<ObjectRef> deque_elements(Machine& machine,
                                                ObjectRef deque) {
    auto elements = reference_field(machine, deque, kElementsField);
    if (!elements) return std::unexpected(elements.error());
    if (elements->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "ArrayDeque has not been initialized");
    }
    return *elements;
}

[[nodiscard]] Result<i32> deque_size(Machine& machine, ObjectRef deque) {
    auto size = int_field(machine, deque, kSizeField);
    if (!size) return std::unexpected(size.error());
    if (*size < 0) {
        return fail(ErrorCode::invalid_state,
                    "ArrayDeque size is negative");
    }
    return *size;
}

[[nodiscard]] Status ensure_capacity(Machine& machine,
                                     ObjectRef deque,
                                     DequeState& state,
                                     i32 minimum) {
    auto capacity = machine.heap().array_length(state.elements);
    if (!capacity) return std::unexpected(capacity.error());
    if (minimum <= static_cast<i32>(*capacity)) return {};
    if (minimum < 0) {
        return fail_java("java/lang/OutOfMemoryError",
                         "ArrayDeque capacity overflow");
    }
    usize grown = *capacity < 2U ? 2U : *capacity * 2U;
    grown = std::max(grown, static_cast<usize>(minimum));
    if (grown > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return fail_java("java/lang/OutOfMemoryError",
                         "ArrayDeque capacity overflow");
    }
    auto replacement = allocate_object_array(machine, grown);
    if (!replacement) return std::unexpected(replacement.error());
    if (state.size > 0) {
        auto copied = machine.heap().copy_array_range(
            state.elements, 0U, *replacement, 0U,
            static_cast<usize>(state.size));
        if (!copied) return copied;
    }
    auto stored = set_reference_field(machine, deque, kElementsField, *replacement);
    if (!stored) return stored;
    state.elements = *replacement;
    return {};
}

[[nodiscard]] Result<ObjectRef> element_reference(Machine& machine,
                                                   ObjectRef elements,
                                                   i32 index) {
    auto value = machine.heap().element(elements, static_cast<usize>(index));
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Status append(Machine& machine,
                            ObjectRef deque,
                            ObjectRef value) {
    if (value.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "ArrayDeque does not permit null elements");
    }
    auto state = deque_state(machine, deque);
    if (!state) return std::unexpected(state.error());
    if (state->size == std::numeric_limits<i32>::max()) {
        return fail_java("java/lang/OutOfMemoryError",
                         "ArrayDeque size overflow");
    }
    auto capacity = ensure_capacity(machine, deque, *state, state->size + 1);
    if (!capacity) return capacity;
    auto stored = machine.heap().set_element(
        state->elements, static_cast<usize>(state->size),
        Value::from_reference(value));
    if (!stored) return stored;
    return set_int_field(machine, deque, kSizeField, state->size + 1);
}

[[nodiscard]] Status prepend(Machine& machine,
                             ObjectRef deque,
                             ObjectRef value) {
    if (value.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "ArrayDeque does not permit null elements");
    }
    auto state = deque_state(machine, deque);
    if (!state) return std::unexpected(state.error());
    if (state->size == std::numeric_limits<i32>::max()) {
        return fail_java("java/lang/OutOfMemoryError",
                         "ArrayDeque size overflow");
    }
    auto capacity = ensure_capacity(machine, deque, *state, state->size + 1);
    if (!capacity) return capacity;
    if (state->size > 0) {
        auto shifted = machine.heap().copy_array_range(
            state->elements, 0U, state->elements, 1U,
            static_cast<usize>(state->size));
        if (!shifted) return shifted;
    }
    auto stored = machine.heap().set_element(
        state->elements, 0U, Value::from_reference(value));
    if (!stored) return stored;
    return set_int_field(machine, deque, kSizeField, state->size + 1);
}

[[nodiscard]] Result<ObjectRef> remove_at(Machine& machine,
                                          ObjectRef deque,
                                          i32 index) {
    auto state = deque_state(machine, deque);
    if (!state) return std::unexpected(state.error());
    if (index < 0 || index >= state->size) {
        return fail_java("java/util/NoSuchElementException",
                         "ArrayDeque is empty");
    }
    auto removed = element_reference(machine, state->elements, index);
    if (!removed) return std::unexpected(removed.error());
    const i32 shifted_count = state->size - index - 1;
    if (shifted_count > 0) {
        auto shifted = machine.heap().copy_array_range(
            state->elements, static_cast<usize>(index + 1),
            state->elements, static_cast<usize>(index),
            static_cast<usize>(shifted_count));
        if (!shifted) return std::unexpected(shifted.error());
    }
    auto cleared = machine.heap().set_element(
        state->elements, static_cast<usize>(state->size - 1),
        Value::from_reference({}));
    if (!cleared) return std::unexpected(cleared.error());
    auto resized = set_int_field(machine, deque, kSizeField, state->size - 1);
    if (!resized) return std::unexpected(resized.error());
    return *removed;
}

[[nodiscard]] Result<ObjectRef> peek_at(Machine& machine,
                                        ObjectRef deque,
                                        bool first,
                                        bool require_value) {
    auto state = deque_state(machine, deque);
    if (!state) return std::unexpected(state.error());
    if (state->size == 0) {
        if (require_value) {
            return fail_java("java/util/NoSuchElementException",
                             "ArrayDeque is empty");
        }
        return ObjectRef {};
    }
    return element_reference(machine, state->elements,
                             first ? 0 : state->size - 1);
}

[[nodiscard]] Result<bool> values_equal(Machine& machine,
                                        ObjectRef left,
                                        ObjectRef right) {
    if (left == right) return true;
    if (left.is_null() || right.is_null()) return false;
    auto class_name = machine.heap().class_name(left);
    if (!class_name) return std::unexpected(class_name.error());
    const Value argument = Value::from_reference(right);
    auto invoked = machine.invoke_instance(
        left, *class_name, "equals", "(Ljava/lang/Object;)Z",
        std::span<const Value>(&argument, 1U));
    if (!invoked) return std::unexpected(invoked.error());
    if (invoked->throwable.has_value()) {
        auto throwable_class = machine.heap().class_name(*invoked->throwable);
        if (!throwable_class) return std::unexpected(throwable_class.error());
        return fail_java(*throwable_class,
                         "ArrayDeque element equals() threw");
    }
    if (!invoked->return_value.has_value()) {
        return fail(ErrorCode::internal_error,
                    "Object.equals returned no value");
    }
    auto equal = invoked->return_value->as_int();
    if (!equal) return std::unexpected(equal.error());
    return *equal != 0;
}

[[nodiscard]] Result<i32> find_index(Machine& machine,
                                     ObjectRef deque,
                                     ObjectRef target,
                                     bool reverse) {
    auto state = deque_state(machine, deque);
    if (!state) return std::unexpected(state.error());
    if (!reverse) {
        for (i32 index = 0; index < state->size; ++index) {
            auto value = element_reference(machine, state->elements, index);
            if (!value) return std::unexpected(value.error());
            auto equal = values_equal(machine, *value, target);
            if (!equal) return std::unexpected(equal.error());
            if (*equal) return index;
        }
    } else {
        for (i32 index = state->size - 1; index >= 0; --index) {
            auto value = element_reference(machine, state->elements, index);
            if (!value) return std::unexpected(value.error());
            auto equal = values_equal(machine, *value, target);
            if (!equal) return std::unexpected(equal.error());
            if (*equal) return index;
        }
    }
    return -1;
}

[[nodiscard]] Result<ObjectRef> snapshot(Machine& machine,
                                         ObjectRef deque,
                                         bool reverse) {
    auto state = deque_state(machine, deque);
    if (!state) return std::unexpected(state.error());
    auto result = allocate_object_array(machine, static_cast<usize>(state->size));
    if (!result) return std::unexpected(result.error());
    if (!reverse && state->size > 0) {
        auto copied = machine.heap().copy_array_range(
            state->elements, 0U, *result, 0U,
            static_cast<usize>(state->size));
        if (!copied) return std::unexpected(copied.error());
        return *result;
    }
    for (i32 index = 0; index < state->size; ++index) {
        const i32 source = reverse ? state->size - 1 - index : index;
        auto value = machine.heap().element(
            state->elements, static_cast<usize>(source));
        if (!value) return std::unexpected(value.error());
        auto stored = machine.heap().set_element(
            *result, static_cast<usize>(index), *value);
        if (!stored) return std::unexpected(stored.error());
    }
    return *result;
}

[[nodiscard]] Result<ObjectRef> make_iterator(Machine& machine,
                                              ObjectRef deque,
                                              bool reverse) {
    auto values = snapshot(machine, deque, reverse);
    if (!values) return std::unexpected(values.error());
    auto size = deque_size(machine, deque);
    if (!size) return std::unexpected(size.error());
    auto iterator = machine.class_states().allocate_instance(
        machine.heap(), "java/util/ArrayDequeIterator");
    if (!iterator) return std::unexpected(iterator.error());
    constexpr std::array<usize, 3> fields {
        kIteratorValuesField,
        kIteratorIndexField,
        kIteratorSizeField,
    };
    const std::array<Value, fields.size()> state {
        Value::from_reference(*values),
        Value::from_int(0),
        Value::from_int(*size),
    };
    auto stored = machine.heap().write_fields(*iterator, fields, state);
    if (!stored) return std::unexpected(stored.error());
    return *iterator;
}

[[nodiscard]] Result<ObjectRef> collection_to_array(Machine& machine,
                                                     ObjectRef collection) {
    auto class_name = machine.heap().class_name(collection);
    if (!class_name) return std::unexpected(class_name.error());
    auto invoked = machine.invoke_instance(
        collection, *class_name, "toArray", "()[Ljava/lang/Object;");
    if (!invoked) return std::unexpected(invoked.error());
    if (invoked->throwable.has_value()) {
        auto throwable_class = machine.heap().class_name(*invoked->throwable);
        if (!throwable_class) return std::unexpected(throwable_class.error());
        return fail_java(*throwable_class,
                         "Collection.toArray() threw while constructing ArrayDeque");
    }
    if (!invoked->return_value.has_value()) {
        return fail(ErrorCode::internal_error,
                    "Collection.toArray() returned no value");
    }
    auto array = invoked->return_value->as_reference();
    if (!array) return std::unexpected(array.error());
    if (array->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Collection.toArray() returned null");
    }
    return *array;
}

[[nodiscard]] Result<bool> append_collection(Machine& machine,
                                             ObjectRef deque,
                                             ObjectRef collection) {
    auto array = collection_to_array(machine, collection);
    if (!array) return std::unexpected(array.error());
    auto length = machine.heap().array_length(*array);
    if (!length) return std::unexpected(length.error());
    bool changed = false;
    for (usize index = 0; index < *length; ++index) {
        auto value = machine.heap().element(*array, index);
        if (!value) return std::unexpected(value.error());
        auto reference = value->as_reference();
        if (!reference) return std::unexpected(reference.error());
        auto stored = append(machine, deque, *reference);
        if (!stored) return std::unexpected(stored.error());
        changed = true;
    }
    return changed;
}

Result<std::optional<Value>> return_reference(Result<ObjectRef> result) {
    if (!result) return std::unexpected(result.error());
    return std::optional<Value>(Value::from_reference(*result));
}

void register_iterator_natives(NativeMethodRegistry& registry) {
    constexpr std::string_view kOwner = "java/util/ArrayDequeIterator";
    add(registry, std::string(kOwner), "hasNext", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 2> fields {
                kIteratorIndexField,
                kIteratorSizeField,
            };
            std::array<Value, fields.size()> state {};
            auto read = machine.heap().read_fields(*object, fields, state);
            if (!read) return std::unexpected(read.error());
            auto index = state[0U].as_int();
            auto size = state[1U].as_int();
            if (!index) return std::unexpected(index.error());
            if (!size) return std::unexpected(size.error());
            return std::optional<Value>(Value::from_int(*index < *size ? 1 : 0));
        });
    add(registry, std::string(kOwner), "next", "()Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            constexpr std::array<usize, 3> fields {
                kIteratorValuesField,
                kIteratorIndexField,
                kIteratorSizeField,
            };
            std::array<Value, fields.size()> state {};
            auto read = machine.heap().read_fields(*object, fields, state);
            if (!read) return std::unexpected(read.error());
            auto values = state[0U].as_reference();
            auto index = state[1U].as_int();
            auto size = state[2U].as_int();
            if (!index) return std::unexpected(index.error());
            if (!size) return std::unexpected(size.error());
            if (!values) return std::unexpected(values.error());
            if (*index >= *size) {
                return fail_java("java/util/NoSuchElementException",
                                 "ArrayDeque iterator is exhausted");
            }
            auto value = machine.heap().element(
                *values, static_cast<usize>(*index));
            if (!value) return std::unexpected(value.error());
            auto advanced = set_int_field(
                machine, *object, kIteratorIndexField, *index + 1);
            if (!advanced) return std::unexpected(advanced.error());
            return std::optional<Value>(*value);
        });
    add(registry, std::string(kOwner), "remove", "()V",
        [](Machine&, std::span<const Value>)
            -> Result<std::optional<Value>> {
            return fail_java("java/lang/UnsupportedOperationException",
                             "snapshot iterator does not support remove");
        });
}

} // namespace

void register_array_deque_natives(NativeMethodRegistry& registry) {
    constexpr std::string_view kOwner = "java/util/ArrayDeque";

    add(registry, std::string(kOwner), "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto initialized = initialize_deque(
                machine, *object, static_cast<i32>(kDefaultCapacity));
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kOwner), "<init>", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto capacity = int_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!capacity) return std::unexpected(capacity.error());
            auto initialized = initialize_deque(machine, *object, *capacity);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kOwner), "<init>", "(Ljava/util/Collection;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto collection = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!collection) return std::unexpected(collection.error());
            auto initialized = initialize_deque(machine, *object, 1);
            if (!initialized) return std::unexpected(initialized.error());
            auto appended = append_collection(machine, *object, *collection);
            if (!appended) return std::unexpected(appended.error());
            return std::optional<Value> {};
        });

    const auto add_first = [](Machine& machine, std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        auto value = reference_argument(arguments, 1U);
        if (!object) return std::unexpected(object.error());
        if (!value) return std::unexpected(value.error());
        auto stored = prepend(machine, *object, *value);
        if (!stored) return std::unexpected(stored.error());
        return std::optional<Value> {};
    };
    const auto add_last = [](Machine& machine, std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        auto value = reference_argument(arguments, 1U);
        if (!object) return std::unexpected(object.error());
        if (!value) return std::unexpected(value.error());
        auto stored = append(machine, *object, *value);
        if (!stored) return std::unexpected(stored.error());
        return std::optional<Value> {};
    };
    add(registry, std::string(kOwner), "addFirst", "(Ljava/lang/Object;)V", add_first);
    add(registry, std::string(kOwner), "push", "(Ljava/lang/Object;)V", add_first);
    add(registry, std::string(kOwner), "addLast", "(Ljava/lang/Object;)V", add_last);

    const auto offer_first = [](Machine& machine, std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        auto value = reference_argument(arguments, 1U);
        if (!object) return std::unexpected(object.error());
        if (!value) return std::unexpected(value.error());
        auto stored = prepend(machine, *object, *value);
        if (!stored) return std::unexpected(stored.error());
        return std::optional<Value>(Value::from_int(1));
    };
    const auto offer_last = [](Machine& machine, std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        auto value = reference_argument(arguments, 1U);
        if (!object) return std::unexpected(object.error());
        if (!value) return std::unexpected(value.error());
        auto stored = append(machine, *object, *value);
        if (!stored) return std::unexpected(stored.error());
        return std::optional<Value>(Value::from_int(1));
    };
    add(registry, std::string(kOwner), "offerFirst", "(Ljava/lang/Object;)Z", offer_first);
    add(registry, std::string(kOwner), "offerLast", "(Ljava/lang/Object;)Z", offer_last);
    add(registry, std::string(kOwner), "add", "(Ljava/lang/Object;)Z", offer_last);
    add(registry, std::string(kOwner), "offer", "(Ljava/lang/Object;)Z", offer_last);

    const auto remove_first = [](Machine& machine, std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        if (!object) return std::unexpected(object.error());
        return return_reference(remove_at(machine, *object, 0));
    };
    const auto remove_last = [](Machine& machine, std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        if (!object) return std::unexpected(object.error());
        auto size = deque_size(machine, *object);
        if (!size) return std::unexpected(size.error());
        return return_reference(remove_at(machine, *object, *size - 1));
    };
    add(registry, std::string(kOwner), "removeFirst", "()Ljava/lang/Object;", remove_first);
    add(registry, std::string(kOwner), "remove", "()Ljava/lang/Object;", remove_first);
    add(registry, std::string(kOwner), "pop", "()Ljava/lang/Object;", remove_first);
    add(registry, std::string(kOwner), "removeLast", "()Ljava/lang/Object;", remove_last);

    const auto poll_first = [](Machine& machine, std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        if (!object) return std::unexpected(object.error());
        auto size = deque_size(machine, *object);
        if (!size) return std::unexpected(size.error());
        if (*size == 0) {
            return std::optional<Value>(Value::from_reference({}));
        }
        return return_reference(remove_at(machine, *object, 0));
    };
    const auto poll_last = [](Machine& machine, std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        if (!object) return std::unexpected(object.error());
        auto size = deque_size(machine, *object);
        if (!size) return std::unexpected(size.error());
        if (*size == 0) {
            return std::optional<Value>(Value::from_reference({}));
        }
        return return_reference(remove_at(machine, *object, *size - 1));
    };
    add(registry, std::string(kOwner), "pollFirst", "()Ljava/lang/Object;", poll_first);
    add(registry, std::string(kOwner), "poll", "()Ljava/lang/Object;", poll_first);
    add(registry, std::string(kOwner), "pollLast", "()Ljava/lang/Object;", poll_last);

    const auto get_first = [](Machine& machine, std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        if (!object) return std::unexpected(object.error());
        return return_reference(peek_at(machine, *object, true, true));
    };
    const auto get_last = [](Machine& machine, std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        if (!object) return std::unexpected(object.error());
        return return_reference(peek_at(machine, *object, false, true));
    };
    const auto peek_first = [](Machine& machine, std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        if (!object) return std::unexpected(object.error());
        return return_reference(peek_at(machine, *object, true, false));
    };
    const auto peek_last = [](Machine& machine, std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        if (!object) return std::unexpected(object.error());
        return return_reference(peek_at(machine, *object, false, false));
    };
    add(registry, std::string(kOwner), "getFirst", "()Ljava/lang/Object;", get_first,
        NativeJitPolicy::synchronous_bounded);
    add(registry, std::string(kOwner), "element", "()Ljava/lang/Object;", get_first,
        NativeJitPolicy::synchronous_bounded);
    add(registry, std::string(kOwner), "getLast", "()Ljava/lang/Object;", get_last,
        NativeJitPolicy::synchronous_bounded);
    add(registry, std::string(kOwner), "peekFirst", "()Ljava/lang/Object;", peek_first,
        NativeJitPolicy::synchronous_bounded);
    add(registry, std::string(kOwner), "peek", "()Ljava/lang/Object;", peek_first,
        NativeJitPolicy::synchronous_bounded);
    add(registry, std::string(kOwner), "peekLast", "()Ljava/lang/Object;", peek_last,
        NativeJitPolicy::synchronous_bounded);

    const auto occurrence = [](bool reverse) {
        return [reverse](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto target = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!target) return std::unexpected(target.error());
            auto index = find_index(machine, *object, *target, reverse);
            if (!index) return std::unexpected(index.error());
            if (*index < 0) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto removed = remove_at(machine, *object, *index);
            if (!removed) return std::unexpected(removed.error());
            return std::optional<Value>(Value::from_int(1));
        };
    };
    add(registry, std::string(kOwner), "removeFirstOccurrence",
        "(Ljava/lang/Object;)Z", occurrence(false));
    add(registry, std::string(kOwner), "removeLastOccurrence",
        "(Ljava/lang/Object;)Z", occurrence(true));
    add(registry, std::string(kOwner), "remove",
        "(Ljava/lang/Object;)Z", occurrence(false));

    add(registry, std::string(kOwner), "size", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto size = deque_size(machine, *object);
            if (!size) return std::unexpected(size.error());
            return std::optional<Value>(Value::from_int(*size));
        });
    add(registry, std::string(kOwner), "isEmpty", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto size = deque_size(machine, *object);
            if (!size) return std::unexpected(size.error());
            return std::optional<Value>(Value::from_int(*size == 0 ? 1 : 0));
        });
    add(registry, std::string(kOwner), "contains", "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto target = reference_argument(arguments, 1U, true);
            if (!object) return std::unexpected(object.error());
            if (!target) return std::unexpected(target.error());
            auto index = find_index(machine, *object, *target, false);
            if (!index) return std::unexpected(index.error());
            return std::optional<Value>(Value::from_int(*index >= 0 ? 1 : 0));
        });
    add(registry, std::string(kOwner), "clear", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto state = deque_state(machine, *object);
            if (!state) return std::unexpected(state.error());
            if (state->size > 0) {
                auto cleared = machine.heap().fill_array_range(
                    state->elements, 0U, static_cast<usize>(state->size),
                    Value::from_reference({}));
                if (!cleared) return std::unexpected(cleared.error());
            }
            auto resized = set_int_field(machine, *object, kSizeField, 0);
            if (!resized) return std::unexpected(resized.error());
            return std::optional<Value> {};
        });

    add(registry, std::string(kOwner), "iterator", "()Ljava/util/Iterator;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            return return_reference(make_iterator(machine, *object, false));
        });
    add(registry, std::string(kOwner), "descendingIterator", "()Ljava/util/Iterator;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            return return_reference(make_iterator(machine, *object, true));
        });
    add(registry, std::string(kOwner), "toArray", "()[Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            return return_reference(snapshot(machine, *object, false));
        });
    add(registry, std::string(kOwner), "toArray",
        "([Ljava/lang/Object;)[Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto supplied = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!supplied) return std::unexpected(supplied.error());
            auto size = deque_size(machine, *object);
            auto elements = deque_elements(machine, *object);
            auto supplied_length = machine.heap().array_length(*supplied);
            if (!size) return std::unexpected(size.error());
            if (!elements) return std::unexpected(elements.error());
            if (!supplied_length) return std::unexpected(supplied_length.error());
            ObjectRef destination = *supplied;
            if (*supplied_length < static_cast<usize>(*size)) {
                auto array_class = machine.heap().class_name(*supplied);
                if (!array_class) return std::unexpected(array_class.error());
                auto allocated = machine.heap().allocate_array(
                    *array_class, static_cast<usize>(*size),
                    Value::from_reference({}));
                if (!allocated) return std::unexpected(allocated.error());
                destination = *allocated;
            }
            for (i32 index = 0; index < *size; ++index) {
                auto value = machine.heap().element(
                    *elements, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto stored = machine.heap().set_element(
                    destination, static_cast<usize>(index), *value);
                if (!stored) return std::unexpected(stored.error());
            }
            auto destination_length = machine.heap().array_length(destination);
            if (!destination_length) {
                return std::unexpected(destination_length.error());
            }
            if (*destination_length > static_cast<usize>(*size)) {
                auto terminated = machine.heap().set_element(
                    destination, static_cast<usize>(*size),
                    Value::from_reference({}));
                if (!terminated) return std::unexpected(terminated.error());
            }
            return std::optional<Value>(Value::from_reference(destination));
        });
    add(registry, std::string(kOwner), "addAll", "(Ljava/util/Collection;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto collection = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!collection) return std::unexpected(collection.error());
            auto changed = append_collection(machine, *object, *collection);
            if (!changed) return std::unexpected(changed.error());
            return std::optional<Value>(Value::from_int(*changed ? 1 : 0));
        });
    add(registry, std::string(kOwner), "clone", "()Ljava/util/ArrayDeque;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto size = deque_size(machine, *object);
            if (!size) return std::unexpected(size.error());
            auto clone = machine.class_states().allocate_instance(
                machine.heap(), "java/util/ArrayDeque");
            if (!clone) return std::unexpected(clone.error());
            auto initialized = initialize_deque(machine, *clone, std::max(1, *size));
            if (!initialized) return std::unexpected(initialized.error());
            auto values = snapshot(machine, *object, false);
            if (!values) return std::unexpected(values.error());
            auto length = machine.heap().array_length(*values);
            if (!length) return std::unexpected(length.error());
            for (usize index = 0; index < *length; ++index) {
                auto value = machine.heap().element(*values, index);
                if (!value) return std::unexpected(value.error());
                auto reference = value->as_reference();
                if (!reference) return std::unexpected(reference.error());
                auto stored = append(machine, *clone, *reference);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value>(Value::from_reference(*clone));
        });

    constexpr std::string_view kBlockingOwner =
        "java/util/concurrent/LinkedBlockingDeque";
    add(registry, std::string(kBlockingOwner), "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto initialized = initialize_deque(
                machine, *object, static_cast<i32>(kDefaultCapacity));
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kBlockingOwner), "put",
        "(Ljava/lang/Object;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto stored = append(machine, *object, *value);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kBlockingOwner), "take",
        "()Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            for (;;) {
                auto size = deque_size(machine, *object);
                if (!size) return std::unexpected(size.error());
                if (*size != 0) {
                    return return_reference(remove_at(machine, *object, 0));
                }
                auto slept = machine.sleep_current_thread(10);
                if (!slept) return std::unexpected(slept.error());
                if (*slept == SchedulerWaitResult::interrupted) {
                    return fail_java("java/lang/InterruptedException",
                                     "BlockingQueue.take was interrupted");
                }
            }
        });
    add(registry, std::string(kBlockingOwner), "remainingCapacity", "()I",
        [](Machine&, std::span<const Value>)
            -> Result<std::optional<Value>> {
            return std::optional<Value>(
                Value::from_int(std::numeric_limits<i32>::max()));
        }, NativeJitPolicy::synchronous_bounded);

    register_iterator_natives(registry);
}

} // namespace phoneme::vm
