#include "Jdk8CompatNativesParts.hpp"

#include <array>
#include <string_view>

#include "Jdk8CompatNativeSupport.hpp"

namespace phoneme::vm {
namespace {

using namespace jdk8compat;

constexpr usize kSetMapField = 0U;
constexpr usize kEnumSetTypeField = 1U;
constexpr usize kTreeValuesField = 0U;
constexpr usize kTreeComparatorField = 1U;

void alias_hash_set(NativeMethodRegistry& registry,
                    std::string_view target,
                    bool constructors,
                    bool add_method) {
    if (constructors) {
        alias(registry, "java/util/HashSet", "<init>", "()V",
              std::string(target));
        alias(registry, "java/util/HashSet", "<init>", "(I)V",
              std::string(target));
        alias(registry, "java/util/HashSet", "<init>",
              "(Ljava/util/Collection;)V", std::string(target));
    }
    if (add_method) {
        alias(registry, "java/util/HashSet", "add",
              "(Ljava/lang/Object;)Z", std::string(target));
    }
    const std::array<std::pair<std::string_view, std::string_view>, 15> methods {{
        {"size", "()I"}, {"isEmpty", "()Z"},
        {"contains", "(Ljava/lang/Object;)Z"},
        {"remove", "(Ljava/lang/Object;)Z"}, {"clear", "()V"},
        {"iterator", "()Ljava/util/Iterator;"},
        {"toArray", "()[Ljava/lang/Object;"},
        {"toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;"},
        {"containsAll", "(Ljava/util/Collection;)Z"},
        {"addAll", "(Ljava/util/Collection;)Z"},
        {"removeAll", "(Ljava/util/Collection;)Z"},
        {"retainAll", "(Ljava/util/Collection;)Z"},
        {"equals", "(Ljava/lang/Object;)Z"}, {"hashCode", "()I"},
        {"toString", "()Ljava/lang/String;"},
    }};
    for (const auto& [name, descriptor] : methods) {
        alias(registry, "java/util/HashSet", name, descriptor,
              std::string(target));
    }
}

void alias_array_list(NativeMethodRegistry& registry,
                      std::string_view target) {
    alias(registry, "java/util/ArrayList", "<init>", "()V",
          std::string(target));
    alias(registry, "java/util/ArrayList", "<init>",
          "(Ljava/util/Collection;)V", std::string(target));
    const std::array<std::pair<std::string_view, std::string_view>, 18> methods {{
        {"size", "()I"}, {"isEmpty", "()Z"},
        {"contains", "(Ljava/lang/Object;)Z"},
        {"get", "(I)Ljava/lang/Object;"},
        {"set", "(ILjava/lang/Object;)Ljava/lang/Object;"},
        {"add", "(Ljava/lang/Object;)Z"},
        {"add", "(ILjava/lang/Object;)V"},
        {"remove", "(I)Ljava/lang/Object;"},
        {"remove", "(Ljava/lang/Object;)Z"},
        {"clear", "()V"}, {"indexOf", "(Ljava/lang/Object;)I"},
        {"lastIndexOf", "(Ljava/lang/Object;)I"},
        {"iterator", "()Ljava/util/Iterator;"},
        {"toArray", "()[Ljava/lang/Object;"},
        {"toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;"},
        {"addAll", "(Ljava/util/Collection;)Z"},
        {"equals", "(Ljava/lang/Object;)Z"},
        {"toString", "()Ljava/lang/String;"},
    }};
    for (const auto& [name, descriptor] : methods) {
        alias(registry, "java/util/ArrayList", name, descriptor,
              std::string(target));
    }
}

void register_linked_list(NativeMethodRegistry& registry) {
    alias_array_list(registry, "java/util/LinkedList");
    add(registry, "java/util/LinkedList", "addFirst",
        "(Ljava/lang/Object;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto list = receiver(arguments);
            auto value = reference_argument(arguments, 1U, true);
            if (!list) return std::unexpected(list.error());
            if (!value) return std::unexpected(value.error());
            const std::array<Value, 3> forwarded {
                Value::from_reference(*list), Value::from_int(0),
                Value::from_reference(*value),
            };
            auto result = invoke_native(machine, "java/util/ArrayList", "add",
                                        "(ILjava/lang/Object;)V", forwarded);
            if (!result) return std::unexpected(result.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/LinkedList", "addLast",
        "(Ljava/lang/Object;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto list = receiver(arguments);
            auto value = reference_argument(arguments, 1U, true);
            if (!list) return std::unexpected(list.error());
            if (!value) return std::unexpected(value.error());
            const std::array<Value, 2> forwarded {
                Value::from_reference(*list), Value::from_reference(*value),
            };
            auto result = invoke_native(machine, "java/util/ArrayList", "add",
                                        "(Ljava/lang/Object;)Z", forwarded);
            if (!result) return std::unexpected(result.error());
            return std::optional<Value> {};
        });
    const auto remove_end = [&registry](const char* name, bool first) {
        add(registry, "java/util/LinkedList", name,
            "()Ljava/lang/Object;",
            [first](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto list = receiver(arguments);
                if (!list) return std::unexpected(list.error());
                const Value receiver_value = Value::from_reference(*list);
                auto size_value = invoke_native(
                    machine, "java/util/ArrayList", "size", "()I",
                    std::span<const Value>(&receiver_value, 1U));
                if (!size_value) return std::unexpected(size_value.error());
                if (!size_value->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "LinkedList size returned no value");
                }
                auto size = size_value->value().as_int();
                if (!size) return std::unexpected(size.error());
                if (*size == 0) {
                    return fail_java("java/util/NoSuchElementException",
                                     "LinkedList is empty");
                }
                const std::array<Value, 2> remove_arguments {
                    Value::from_reference(*list),
                    Value::from_int(first ? 0 : *size - 1),
                };
                return invoke_native(machine, "java/util/ArrayList", "remove",
                                     "(I)Ljava/lang/Object;",
                                     remove_arguments);
            });
    };
    remove_end("removeFirst", true);
    remove_end("removeLast", false);

    const auto read_end = [&registry](const char* name, bool first,
                                      bool remove, bool throw_if_empty) {
        add(registry, "java/util/LinkedList", name,
            "()Ljava/lang/Object;",
            [first, remove, throw_if_empty](
                Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto list = receiver(arguments);
                if (!list) return std::unexpected(list.error());
                const Value receiver_value = Value::from_reference(*list);
                auto size_value = invoke_native(
                    machine, "java/util/ArrayList", "size", "()I",
                    std::span<const Value>(&receiver_value, 1U));
                if (!size_value) return std::unexpected(size_value.error());
                if (!size_value->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "LinkedList size returned no value");
                }
                auto size = size_value->value().as_int();
                if (!size) return std::unexpected(size.error());
                if (*size == 0) {
                    if (throw_if_empty) {
                        return fail_java("java/util/NoSuchElementException",
                                         "LinkedList is empty");
                    }
                    return std::optional<Value>(Value::from_reference({}));
                }
                const std::array<Value, 2> forwarded {
                    Value::from_reference(*list),
                    Value::from_int(first ? 0 : *size - 1),
                };
                return invoke_native(
                    machine, "java/util/ArrayList",
                    remove ? "remove" : "get",
                    "(I)Ljava/lang/Object;", forwarded);
            });
    };
    read_end("getFirst", true, false, true);
    read_end("getLast", false, false, true);
    read_end("peekFirst", true, false, false);
    read_end("peekLast", false, false, false);
    read_end("pollFirst", true, true, false);
    read_end("pollLast", false, true, false);

    const auto offer_end = [&registry](const char* name, bool first) {
        add(registry, "java/util/LinkedList", name,
            "(Ljava/lang/Object;)Z",
            [first](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto list = receiver(arguments);
                auto value = reference_argument(arguments, 1U, true);
                if (!list) return std::unexpected(list.error());
                if (!value) return std::unexpected(value.error());
                if (first) {
                    const std::array<Value, 3> forwarded {
                        Value::from_reference(*list), Value::from_int(0),
                        Value::from_reference(*value),
                    };
                    auto result = invoke_native(
                        machine, "java/util/ArrayList", "add",
                        "(ILjava/lang/Object;)V", forwarded);
                    if (!result) return std::unexpected(result.error());
                } else {
                    const std::array<Value, 2> forwarded {
                        Value::from_reference(*list),
                        Value::from_reference(*value),
                    };
                    auto result = invoke_native(
                        machine, "java/util/ArrayList", "add",
                        "(Ljava/lang/Object;)Z", forwarded);
                    if (!result) return std::unexpected(result.error());
                }
                return std::optional<Value>(Value::from_int(1));
            });
    };
    offer_end("offerFirst", true);
    offer_end("offerLast", false);
    offer_end("offer", false);

    add(registry, "java/util/LinkedList", "removeFirstOccurrence",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            return invoke_native(machine, "java/util/ArrayList", "remove",
                                 "(Ljava/lang/Object;)Z", arguments);
        });
    add(registry, "java/util/LinkedList", "removeLastOccurrence",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto list = receiver(arguments);
            auto value = reference_argument(arguments, 1U, true);
            if (!list) return std::unexpected(list.error());
            if (!value) return std::unexpected(value.error());
            const std::array<Value, 2> search_arguments {
                Value::from_reference(*list), Value::from_reference(*value),
            };
            auto index_value = invoke_native(
                machine, "java/util/ArrayList", "lastIndexOf",
                "(Ljava/lang/Object;)I", search_arguments);
            if (!index_value) return std::unexpected(index_value.error());
            if (!index_value->has_value()) {
                return fail(ErrorCode::internal_error,
                            "LinkedList lastIndexOf returned no value");
            }
            auto index = index_value->value().as_int();
            if (!index) return std::unexpected(index.error());
            if (*index < 0) {
                return std::optional<Value>(Value::from_int(0));
            }
            const std::array<Value, 2> remove_arguments {
                Value::from_reference(*list), Value::from_int(*index),
            };
            auto removed = invoke_native(
                machine, "java/util/ArrayList", "remove",
                "(I)Ljava/lang/Object;", remove_arguments);
            if (!removed) return std::unexpected(removed.error());
            return std::optional<Value>(Value::from_int(1));
        });

    const auto alias_noarg = [&registry](const char* target,
                                         const char* source) {
        add(registry, "java/util/LinkedList", target,
            "()Ljava/lang/Object;",
            [source](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                return invoke_native(machine, "java/util/LinkedList", source,
                                     "()Ljava/lang/Object;", arguments);
            });
    };
    alias_noarg("remove", "removeFirst");
    alias_noarg("poll", "pollFirst");
    alias_noarg("element", "getFirst");
    alias_noarg("peek", "peekFirst");
    alias_noarg("pop", "removeFirst");
    add(registry, "java/util/LinkedList", "push",
        "(Ljava/lang/Object;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto result = invoke_native(machine, "java/util/LinkedList",
                                        "addFirst", "(Ljava/lang/Object;)V",
                                        arguments);
            if (!result) return std::unexpected(result.error());
            return std::optional<Value> {};
        });
}

void register_enum_set(NativeMethodRegistry& registry) {
    alias_hash_set(registry, "java/util/EnumSet", false, false);
    add(registry, "java/util/EnumSet", "noneOf",
        "(Ljava/lang/Class;)Ljava/util/EnumSet;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto type = reference_argument(arguments, 0U);
            if (!type) return std::unexpected(type.error());
            auto set = new_instance(machine, "java/util/EnumSet");
            if (!set) return std::unexpected(set.error());
            auto root = machine.pin_native_root(*set);
            if (!root) return std::unexpected(root.error());
            auto map = new_hash_map(machine);
            if (!map) return std::unexpected(map.error());
            auto map_stored = set_reference_field(machine, *set,
                                                  kSetMapField, *map);
            auto type_stored = set_reference_field(machine, *set,
                                                   kEnumSetTypeField, *type);
            if (!map_stored) return std::unexpected(map_stored.error());
            if (!type_stored) return std::unexpected(type_stored.error());
            return std::optional<Value>(Value::from_reference(*set));
        });
    add(registry, "java/util/EnumSet", "add", "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto set = receiver(arguments);
            auto value = reference_argument(arguments, 1U);
            if (!set) return std::unexpected(set.error());
            if (!value) return std::unexpected(value.error());
            auto type = reference_field(machine, *set, kEnumSetTypeField);
            if (!type) return std::unexpected(type.error());
            auto type_name = machine.mirrored_class_name(*type);
            if (!type_name) return std::unexpected(type_name.error());
            auto valid = machine.object_is_instance(*value, *type_name);
            if (!valid) return std::unexpected(valid.error());
            if (!*valid) {
                return fail_java("java/lang/ClassCastException",
                                 "EnumSet value has wrong enum type");
            }
            const std::array<Value, 2> forwarded {
                Value::from_reference(*set), Value::from_reference(*value),
            };
            return invoke_native(machine, "java/util/HashSet", "add",
                                 "(Ljava/lang/Object;)Z", forwarded);
        });
    auto add_bridge = registry.register_alias(
        "java/util/EnumSet", "add", "(Ljava/lang/Object;)Z",
        "java/util/EnumSet", "add", "(Ljava/lang/Enum;)Z");
    if (!add_bridge) std::terminate();
    const auto copy_of = [&registry](const char* descriptor) {
        add(registry, "java/util/EnumSet", "copyOf", descriptor,
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto source = reference_argument(arguments, 0U);
                if (!source) return std::unexpected(source.error());
                auto source_is_enum_set = machine.object_is_instance(
                    *source, "java/util/EnumSet");
                if (!source_is_enum_set) {
                    return std::unexpected(source_is_enum_set.error());
                }
                ObjectRef type;
                if (*source_is_enum_set) {
                    auto stored = reference_field(machine, *source,
                                                  kEnumSetTypeField);
                    if (!stored) return std::unexpected(stored.error());
                    type = *stored;
                } else {
                    auto iterator = invoke_checked(
                        machine, *source, "java/util/Collection", "iterator",
                        "()Ljava/util/Iterator;");
                    if (!iterator) return std::unexpected(iterator.error());
                    if (!iterator->has_value()) {
                        return fail(ErrorCode::internal_error,
                                    "Collection.iterator returned no value");
                    }
                    auto cursor = iterator->value().as_reference();
                    if (!cursor) return std::unexpected(cursor.error());
                    auto has_next = invoke_checked(
                        machine, *cursor, "java/util/Iterator", "hasNext",
                        "()Z");
                    if (!has_next) return std::unexpected(has_next.error());
                    if (!has_next->has_value()) {
                        return fail(ErrorCode::internal_error,
                                    "Iterator.hasNext returned no value");
                    }
                    auto present = has_next->value().as_int();
                    if (!present) return std::unexpected(present.error());
                    if (*present == 0) {
                        return fail_java("java/lang/IllegalArgumentException",
                                         "Cannot infer enum type from empty collection");
                    }
                    auto first = invoke_checked(
                        machine, *cursor, "java/util/Iterator", "next",
                        "()Ljava/lang/Object;");
                    if (!first) return std::unexpected(first.error());
                    if (!first->has_value()) {
                        return fail(ErrorCode::internal_error,
                                    "Iterator.next returned no value");
                    }
                    auto first_ref = first->value().as_reference();
                    if (!first_ref) return std::unexpected(first_ref.error());
                    auto class_name = machine.heap().class_name(*first_ref);
                    if (!class_name) return std::unexpected(class_name.error());
                    auto mirror = machine.class_mirror(*class_name);
                    if (!mirror) return std::unexpected(mirror.error());
                    type = *mirror;
                }
                const Value type_argument = Value::from_reference(type);
                auto created = invoke_native(
                    machine, "java/util/EnumSet", "noneOf",
                    "(Ljava/lang/Class;)Ljava/util/EnumSet;",
                    std::span<const Value>(&type_argument, 1U));
                if (!created) return std::unexpected(created.error());
                if (!created->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "EnumSet.noneOf returned no value");
                }
                auto target = created->value().as_reference();
                if (!target) return std::unexpected(target.error());
                const Value source_argument = Value::from_reference(*source);
                auto added = invoke_checked(
                    machine, *target, "java/util/Collection", "addAll",
                    "(Ljava/util/Collection;)Z",
                    std::span<const Value>(&source_argument, 1U));
                if (!added) return std::unexpected(added.error());
                return created;
            });
    };
    copy_of("(Ljava/util/EnumSet;)Ljava/util/EnumSet;");
    copy_of("(Ljava/util/Collection;)Ljava/util/EnumSet;");
}

[[nodiscard]] Result<ObjectRef> tree_values(Machine& machine,
                                             ObjectRef tree) {
    auto values = reference_field(machine, tree, kTreeValuesField);
    if (!values) return std::unexpected(values.error());
    if (values->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "TreeSet backing list is not initialized");
    }
    return *values;
}

[[nodiscard]] Result<i32> tree_size(Machine& machine, ObjectRef tree) {
    auto values = tree_values(machine, tree);
    if (!values) return std::unexpected(values.error());
    auto result = invoke_checked(machine, *values, "java/util/List", "size",
                                 "()I");
    if (!result) return std::unexpected(result.error());
    if (!result->has_value()) {
        return fail(ErrorCode::internal_error,
                    "TreeSet backing List.size returned no value");
    }
    return result->value().as_int();
}

[[nodiscard]] Result<ObjectRef> tree_get(Machine& machine,
                                         ObjectRef tree,
                                         i32 index) {
    auto values = tree_values(machine, tree);
    if (!values) return std::unexpected(values.error());
    const Value argument = Value::from_int(index);
    auto result = invoke_checked(machine, *values, "java/util/List", "get",
                                 "(I)Ljava/lang/Object;",
                                 std::span<const Value>(&argument, 1U));
    if (!result) return std::unexpected(result.error());
    if (!result->has_value()) {
        return fail(ErrorCode::internal_error,
                    "TreeSet backing List.get returned no value");
    }
    return result->value().as_reference();
}

[[nodiscard]] Result<i32> tree_compare(Machine& machine,
                                       ObjectRef tree,
                                       ObjectRef left,
                                       ObjectRef right) {
    auto comparator = reference_field(machine, tree, kTreeComparatorField);
    if (!comparator) return std::unexpected(comparator.error());
    return comparator->is_null()
        ? natural_compare(machine, left, right)
        : comparator_compare(machine, *comparator, left, right);
}

[[nodiscard]] Result<i32> tree_position(Machine& machine,
                                        ObjectRef tree,
                                        ObjectRef value,
                                        bool& equal) {
    auto size = tree_size(machine, tree);
    if (!size) return std::unexpected(size.error());
    i32 low = 0;
    i32 high = *size;
    while (low < high) {
        const i32 middle = low + (high - low) / 2;
        auto current = tree_get(machine, tree, middle);
        if (!current) return std::unexpected(current.error());
        auto comparison = tree_compare(machine, tree, *current, value);
        if (!comparison) return std::unexpected(comparison.error());
        if (*comparison < 0) low = middle + 1;
        else high = middle;
    }
    equal = false;
    if (low < *size) {
        auto current = tree_get(machine, tree, low);
        if (!current) return std::unexpected(current.error());
        auto comparison = tree_compare(machine, tree, *current, value);
        if (!comparison) return std::unexpected(comparison.error());
        equal = *comparison == 0;
    }
    return low;
}

void register_tree_set(NativeMethodRegistry& registry) {
    const auto constructor = [&registry](const char* descriptor,
                                         bool collection,
                                         bool comparator) {
        add(registry, "java/util/TreeSet", "<init>", descriptor,
            [collection, comparator](
                Machine& machine,
                std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto tree = receiver(arguments);
                if (!tree) return std::unexpected(tree.error());
                ObjectRef configured_comparator;
                if (comparator) {
                    auto value = reference_argument(arguments, 1U, true);
                    if (!value) return std::unexpected(value.error());
                    configured_comparator = *value;
                }
                auto values = new_array_list(machine);
                if (!values) return std::unexpected(values.error());
                auto values_stored = set_reference_field(
                    machine, *tree, kTreeValuesField, *values);
                auto comparator_stored = set_reference_field(
                    machine, *tree, kTreeComparatorField,
                    configured_comparator);
                if (!values_stored) {
                    return std::unexpected(values_stored.error());
                }
                if (!comparator_stored) {
                    return std::unexpected(comparator_stored.error());
                }
                if (collection) {
                    auto source = reference_argument(arguments, 1U);
                    if (!source) return std::unexpected(source.error());
                    const Value source_argument =
                        Value::from_reference(*source);
                    auto added = invoke_checked(
                        machine, *tree, "java/util/TreeSet", "addAll",
                        "(Ljava/util/Collection;)Z",
                        std::span<const Value>(&source_argument, 1U));
                    if (!added) return std::unexpected(added.error());
                }
                return std::optional<Value> {};
            });
    };
    constructor("()V", false, false);
    constructor("(Ljava/util/Collection;)V", true, false);
    constructor("(Ljava/util/Comparator;)V", false, true);
    add(registry, "java/util/TreeSet", "size", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tree = receiver(arguments);
            if (!tree) return std::unexpected(tree.error());
            auto size = tree_size(machine, *tree);
            if (!size) return std::unexpected(size.error());
            return std::optional<Value>(Value::from_int(*size));
        });
    add(registry, "java/util/TreeSet", "isEmpty", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tree = receiver(arguments);
            if (!tree) return std::unexpected(tree.error());
            auto size = tree_size(machine, *tree);
            if (!size) return std::unexpected(size.error());
            return std::optional<Value>(Value::from_int(*size == 0 ? 1 : 0));
        });
    add(registry, "java/util/TreeSet", "add", "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tree = receiver(arguments);
            auto value = reference_argument(arguments, 1U);
            if (!tree) return std::unexpected(tree.error());
            if (!value) return std::unexpected(value.error());
            bool equal = false;
            auto position = tree_position(machine, *tree, *value, equal);
            if (!position) return std::unexpected(position.error());
            if (equal) return std::optional<Value>(Value::from_int(0));
            auto values = tree_values(machine, *tree);
            if (!values) return std::unexpected(values.error());
            const std::array<Value, 2> forwarded {
                Value::from_int(*position), Value::from_reference(*value),
            };
            auto added = invoke_checked(machine, *values, "java/util/List",
                                        "add", "(ILjava/lang/Object;)V",
                                        forwarded);
            if (!added) return std::unexpected(added.error());
            return std::optional<Value>(Value::from_int(1));
        });
    const auto lookup = [&registry](const char* name, bool remove) {
        add(registry, "java/util/TreeSet", name, "(Ljava/lang/Object;)Z",
            [remove](Machine& machine,
                     std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto tree = receiver(arguments);
                auto value = reference_argument(arguments, 1U);
                if (!tree) return std::unexpected(tree.error());
                if (!value) return std::unexpected(value.error());
                bool equal = false;
                auto position = tree_position(machine, *tree, *value, equal);
                if (!position) return std::unexpected(position.error());
                if (!equal || !remove) {
                    return std::optional<Value>(Value::from_int(equal ? 1 : 0));
                }
                auto values = tree_values(machine, *tree);
                if (!values) return std::unexpected(values.error());
                const Value index = Value::from_int(*position);
                auto removed = invoke_checked(
                    machine, *values, "java/util/List", "remove",
                    "(I)Ljava/lang/Object;",
                    std::span<const Value>(&index, 1U));
                if (!removed) return std::unexpected(removed.error());
                return std::optional<Value>(Value::from_int(1));
            });
    };
    lookup("contains", false);
    lookup("remove", true);
    const auto forward = [&registry](const char* name,
                                     const char* descriptor) {
        add(registry, "java/util/TreeSet", name, descriptor,
            [name, descriptor](Machine& machine,
                               std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto tree = receiver(arguments);
                if (!tree) return std::unexpected(tree.error());
                auto values = tree_values(machine, *tree);
                if (!values) return std::unexpected(values.error());
                return invoke_checked(machine, *values, "java/util/List",
                                      name, descriptor,
                                      arguments.subspan(1U));
            });
    };
    forward("iterator", "()Ljava/util/Iterator;");
    forward("toArray", "()[Ljava/lang/Object;");
    forward("toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;");
    forward("clear", "()V");
    forward("toString", "()Ljava/lang/String;");
    add(registry, "java/util/TreeSet", "addAll",
        "(Ljava/util/Collection;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tree = receiver(arguments);
            auto source = reference_argument(arguments, 1U);
            if (!tree) return std::unexpected(tree.error());
            if (!source) return std::unexpected(source.error());
            auto iterator = invoke_checked(
                machine, *source, "java/util/Collection", "iterator",
                "()Ljava/util/Iterator;");
            if (!iterator) return std::unexpected(iterator.error());
            if (!iterator->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Collection.iterator returned no value");
            }
            auto cursor = iterator->value().as_reference();
            if (!cursor) return std::unexpected(cursor.error());
            bool changed = false;
            while (true) {
                auto has_next = invoke_checked(
                    machine, *cursor, "java/util/Iterator", "hasNext", "()Z");
                if (!has_next) return std::unexpected(has_next.error());
                if (!has_next->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "Iterator.hasNext returned no value");
                }
                auto present = has_next->value().as_int();
                if (!present) return std::unexpected(present.error());
                if (*present == 0) break;
                auto next = invoke_checked(machine, *cursor,
                                           "java/util/Iterator", "next",
                                           "()Ljava/lang/Object;");
                if (!next) return std::unexpected(next.error());
                if (!next->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "Iterator.next returned no value");
                }
                auto value = next->value().as_reference();
                if (!value) return std::unexpected(value.error());
                const Value argument = Value::from_reference(*value);
                auto added = invoke_checked(
                    machine, *tree, "java/util/TreeSet", "add",
                    "(Ljava/lang/Object;)Z",
                    std::span<const Value>(&argument, 1U));
                if (!added) return std::unexpected(added.error());
                if (!added->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "TreeSet.add returned no value");
                }
                auto did_add = added->value().as_int();
                if (!did_add) return std::unexpected(did_add.error());
                changed = changed || *did_add != 0;
            }
            return std::optional<Value>(Value::from_int(changed ? 1 : 0));
        });
}

void register_remove_if(NativeMethodRegistry& registry) {
    add(registry, "java/util/Collection", "removeIf",
        "(Ljava/util/function/Predicate;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto collection = receiver(arguments);
            auto predicate = reference_argument(arguments, 1U);
            if (!collection) return std::unexpected(collection.error());
            if (!predicate) return std::unexpected(predicate.error());
            auto iterator = invoke_checked(
                machine, *collection, "java/util/Collection", "iterator",
                "()Ljava/util/Iterator;");
            if (!iterator) return std::unexpected(iterator.error());
            if (!iterator->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Collection.iterator returned no value");
            }
            auto cursor = iterator->value().as_reference();
            if (!cursor) return std::unexpected(cursor.error());
            bool changed = false;
            while (true) {
                auto has_next = invoke_checked(
                    machine, *cursor, "java/util/Iterator", "hasNext", "()Z");
                if (!has_next) return std::unexpected(has_next.error());
                if (!has_next->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "Iterator.hasNext returned no value");
                }
                auto present = has_next->value().as_int();
                if (!present) return std::unexpected(present.error());
                if (*present == 0) break;
                auto next = invoke_checked(machine, *cursor,
                                           "java/util/Iterator", "next",
                                           "()Ljava/lang/Object;");
                if (!next) return std::unexpected(next.error());
                if (!next->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "Iterator.next returned no value");
                }
                auto value = next->value().as_reference();
                if (!value) return std::unexpected(value.error());
                const Value argument = Value::from_reference(*value);
                auto selected = invoke_checked(
                    machine, *predicate, "java/util/function/Predicate",
                    "test", "(Ljava/lang/Object;)Z",
                    std::span<const Value>(&argument, 1U));
                if (!selected) return std::unexpected(selected.error());
                if (!selected->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "Predicate.test returned no value");
                }
                auto remove = selected->value().as_int();
                if (!remove) return std::unexpected(remove.error());
                if (*remove == 0) continue;
                auto removed = invoke_checked(machine, *cursor,
                                              "java/util/Iterator", "remove",
                                              "()V");
                if (!removed) return std::unexpected(removed.error());
                changed = true;
            }
            return std::optional<Value>(Value::from_int(changed ? 1 : 0));
        });
}

} // namespace

void register_jdk8_set_list_natives(NativeMethodRegistry& registry) {
    alias_hash_set(registry, "java/util/LinkedHashSet", true, true);
    register_linked_list(registry);
    alias_array_list(registry, "java/util/concurrent/CopyOnWriteArrayList");
    register_enum_set(registry);
    register_tree_set(registry);
    register_remove_if(registry);
}

} // namespace phoneme::vm
