#include "Jdk8CompatNativesParts.hpp"

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "Jdk8CompatNativeSupport.hpp"

namespace phoneme::vm {
namespace {

using namespace jdk8compat;

constexpr usize kMapKeysField = 0U;
constexpr usize kMapValuesField = 1U;
constexpr usize kMapSizeField = 2U;
constexpr usize kEnumerationArrayField = 0U;
constexpr usize kEnumerationIndexField = 1U;
constexpr usize kEnumerationSizeField = 2U;

[[nodiscard]] int hex_digit(char16_t value) {
    if (value >= u'0' && value <= u'9') return value - u'0';
    if (value >= u'a' && value <= u'f') return value - u'a' + 10;
    if (value >= u'A' && value <= u'F') return value - u'A' + 10;
    return -1;
}

[[nodiscard]] std::u16string_view trim_view(std::u16string_view value) {
    usize start = 0U;
    usize end = value.size();
    while (start < end && (value[start] == u' ' || value[start] == u'\t' ||
                           value[start] == u'\f')) {
        ++start;
    }
    while (end > start && (value[end - 1U] == u' ' ||
                           value[end - 1U] == u'\t' ||
                           value[end - 1U] == u'\f')) {
        --end;
    }
    return value.substr(start, end - start);
}

[[nodiscard]] std::u16string unescape(std::u16string_view value) {
    std::u16string output;
    output.reserve(value.size());
    for (usize index = 0U; index < value.size(); ++index) {
        const char16_t current = value[index];
        if (current != u'\\' || index + 1U >= value.size()) {
            output.push_back(current);
            continue;
        }
        const char16_t next = value[++index];
        if (next == u'n') output.push_back(u'\n');
        else if (next == u'r') output.push_back(u'\r');
        else if (next == u't') output.push_back(u'\t');
        else if (next == u'f') output.push_back(u'\f');
        else if (next == u'u' && index + 4U < value.size()) {
            int decoded = 0;
            bool valid = true;
            for (usize digit = 0U; digit < 4U; ++digit) {
                const int part = hex_digit(value[index + 1U + digit]);
                if (part < 0) {
                    valid = false;
                    break;
                }
                decoded = (decoded << 4) | part;
            }
            if (valid) {
                output.push_back(static_cast<char16_t>(decoded));
                index += 4U;
            } else {
                output.push_back(next);
            }
        } else {
            output.push_back(next);
        }
    }
    return output;
}

[[nodiscard]] usize separator(std::u16string_view line) {
    bool escaped = false;
    for (usize index = 0U; index < line.size(); ++index) {
        const char16_t value = line[index];
        if (!escaped && (value == u'=' || value == u':' || value == u' ' ||
                         value == u'\t' || value == u'\f')) {
            return index;
        }
        if (value == u'\\') escaped = !escaped;
        else escaped = false;
    }
    return std::u16string_view::npos;
}

[[nodiscard]] bool odd_trailing_slashes(std::u16string_view line) {
    usize count = 0U;
    for (usize index = line.size(); index > 0U && line[index - 1U] == u'\\';
         --index) {
        ++count;
    }
    return (count & 1U) != 0U;
}

[[nodiscard]] Status put_property(Machine& machine,
                                  ObjectRef properties,
                                  std::u16string key,
                                  std::u16string value) {
    auto key_string = create_string(machine, std::move(key));
    if (!key_string) return std::unexpected(key_string.error());
    auto key_root = machine.pin_native_root(*key_string);
    if (!key_root) return std::unexpected(key_root.error());
    auto value_string = create_string(machine, std::move(value));
    if (!value_string) return std::unexpected(value_string.error());
    const std::array<Value, 3> arguments {
        Value::from_reference(properties), Value::from_reference(*key_string),
        Value::from_reference(*value_string),
    };
    auto stored = invoke_native(
        machine, "java/util/HashMap", "put",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
        arguments);
    if (!stored) return std::unexpected(stored.error());
    return {};
}

[[nodiscard]] Status parse_properties(Machine& machine,
                                      ObjectRef properties,
                                      std::u16string_view content) {
    usize cursor = 0U;
    std::u16string logical;
    while (cursor <= content.size()) {
        usize end = cursor;
        while (end < content.size() && content[end] != u'\n' &&
               content[end] != u'\r') {
            ++end;
        }
        std::u16string_view physical = content.substr(cursor, end - cursor);
        const bool continued = odd_trailing_slashes(physical);
        if (continued) physical.remove_suffix(1U);
        if (logical.empty()) logical.assign(physical);
        else logical.append(trim_view(physical));
        while (end < content.size() &&
               (content[end] == u'\n' || content[end] == u'\r')) {
            ++end;
        }
        cursor = end;
        if (continued && cursor < content.size()) continue;

        auto line = trim_view(logical);
        if (!line.empty() && line.front() != u'#' && line.front() != u'!') {
            const usize split = separator(line);
            const auto key_part = split == std::u16string_view::npos
                ? line : trim_view(line.substr(0U, split));
            usize value_start = split == std::u16string_view::npos
                ? line.size() : split + 1U;
            while (value_start < line.size() &&
                   (line[value_start] == u' ' || line[value_start] == u'\t' ||
                    line[value_start] == u'\f' || line[value_start] == u'=' ||
                    line[value_start] == u':')) {
                ++value_start;
            }
            auto stored = put_property(machine, properties,
                                       unescape(key_part),
                                       unescape(line.substr(value_start)));
            if (!stored) return stored;
        }
        logical.clear();
        if (cursor >= content.size()) break;
    }
    return {};
}

[[nodiscard]] Result<std::u16string> read_stream(Machine& machine,
                                                 ObjectRef stream,
                                                 std::string_view owner) {
    std::u16string content;
    while (true) {
        auto read = invoke_checked(machine, stream, owner, "read", "()I");
        if (!read) return std::unexpected(read.error());
        if (!read->has_value()) {
            return fail(ErrorCode::internal_error,
                        "Properties stream read returned no value");
        }
        auto value = read->value().as_int();
        if (!value) return std::unexpected(value.error());
        if (*value < 0) break;
        content.push_back(static_cast<char16_t>(static_cast<u16>(*value)));
    }
    return content;
}

[[nodiscard]] std::u16string escape_property(std::u16string_view value,
                                             bool key) {
    static constexpr char16_t hex[] = u"0123456789ABCDEF";
    std::u16string output;
    for (usize index = 0U; index < value.size(); ++index) {
        const char16_t current = value[index];
        if (current == u'\\') output.append(u"\\\\");
        else if (current == u'\t') output.append(u"\\t");
        else if (current == u'\n') output.append(u"\\n");
        else if (current == u'\r') output.append(u"\\r");
        else if (current == u'\f') output.append(u"\\f");
        else if ((key && (current == u'=' || current == u':' ||
                          current == u' ')) ||
                 (!key && index == 0U && current == u' ')) {
            output.push_back(u'\\');
            output.push_back(current);
        } else if (current < 0x20U || current > 0xFFU) {
            output.append(u"\\u");
            output.push_back(hex[(current >> 12U) & 0xFU]);
            output.push_back(hex[(current >> 8U) & 0xFU]);
            output.push_back(hex[(current >> 4U) & 0xFU]);
            output.push_back(hex[current & 0xFU]);
        } else {
            output.push_back(current);
        }
    }
    return output;
}

[[nodiscard]] Status write_latin1(Machine& machine,
                                  ObjectRef output,
                                  std::u16string_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return fail(ErrorCode::overflow,
                    "Properties output exceeds Java array limits");
    }
    std::vector<u8> bytes(text.size());
    for (usize index = 0U; index < text.size(); ++index) {
        bytes[index] = static_cast<u8>(text[index] & 0xFFU);
    }
    auto array = machine.heap().allocate_array(
        "[B", bytes.size(), Value::from_int(0));
    if (!array) return std::unexpected(array.error());
    auto root = machine.pin_native_root(*array);
    if (!root) return std::unexpected(root.error());
    auto copied = machine.heap().write_byte_array(*array, 0U, bytes);
    if (!copied) return copied;
    const std::array<Value, 3> arguments {
        Value::from_reference(*array),
        Value::from_int(0),
        Value::from_int(static_cast<i32>(bytes.size())),
    };
    auto written = invoke_checked(machine, output, "java/io/OutputStream",
                                  "write", "([BII)V", arguments);
    if (!written) return std::unexpected(written.error());
    return {};
}

[[nodiscard]] Result<bool> map_equals(Machine& machine,
                                      ObjectRef map,
                                      ObjectRef other) {
    if (map == other) return true;
    if (other.is_null()) return false;
    auto is_map = machine.object_is_instance(other, "java/util/Map");
    if (!is_map) return std::unexpected(is_map.error());
    if (!*is_map) return false;
    constexpr std::array<usize, 3> fields {
        kMapSizeField,
        kMapKeysField,
        kMapValuesField,
    };
    std::array<Value, fields.size()> state {};
    auto loaded = machine.heap().read_fields(map, fields, state);
    if (!loaded) return std::unexpected(loaded.error());
    auto own_size = state[0U].as_int();
    auto keys = state[1U].as_reference();
    auto values = state[2U].as_reference();
    if (!own_size || !keys || !values || *own_size < 0 || keys->is_null() ||
        values->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "Properties storage is invalid");
    }
    auto other_size = invoke_checked(machine, other, "java/util/Map",
                                     "size", "()I");
    if (!other_size) return std::unexpected(other_size.error());
    if (!other_size->has_value()) {
        return fail(ErrorCode::internal_error,
                    "Map.size returned no value");
    }
    auto size = other_size->value().as_int();
    if (!size) return std::unexpected(size.error());
    if (*size != *own_size) return false;
    auto key_values = machine.heap().read_reference_array(*keys);
    auto own_values = machine.heap().read_reference_array(*values);
    if (!key_values) return std::unexpected(key_values.error());
    if (!own_values) return std::unexpected(own_values.error());
    if (static_cast<usize>(*own_size) > key_values->size() ||
        static_cast<usize>(*own_size) > own_values->size()) {
        return fail(ErrorCode::invalid_state,
                    "Properties storage is shorter than its size");
    }
    for (i32 index = 0; index < *own_size; ++index) {
        const ObjectRef key = (*key_values)[static_cast<usize>(index)];
        const ObjectRef value = (*own_values)[static_cast<usize>(index)];
        const Value argument = Value::from_reference(key);
        auto mapped = invoke_checked(
            machine, other, "java/util/Map", "get",
            "(Ljava/lang/Object;)Ljava/lang/Object;",
            std::span<const Value>(&argument, 1U));
        if (!mapped) return std::unexpected(mapped.error());
        if (!mapped->has_value()) {
            return fail(ErrorCode::internal_error,
                        "Map.get returned no value");
        }
        auto mapped_ref = mapped->value().as_reference();
        if (!mapped_ref) return std::unexpected(mapped_ref.error());
        auto equal = object_equals(machine, value, *mapped_ref);
        if (!equal) return std::unexpected(equal.error());
        if (!*equal) return false;
        if (value.is_null()) {
            auto contains = invoke_checked(
                machine, other, "java/util/Map", "containsKey",
                "(Ljava/lang/Object;)Z",
                std::span<const Value>(&argument, 1U));
            if (!contains) return std::unexpected(contains.error());
            if (!contains->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Map.containsKey returned no value");
            }
            auto present = contains->value().as_int();
            if (!present) return std::unexpected(present.error());
            if (*present == 0) return false;
        }
    }
    return true;
}

void register_properties_methods(NativeMethodRegistry& registry) {
    alias(registry, "java/util/HashMap", "<init>", "()V",
          "java/util/Properties");
    add(registry, "java/util/Properties", "setProperty",
        "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto properties = receiver(arguments);
            auto key = reference_argument(arguments, 1U);
            auto value = reference_argument(arguments, 2U);
            if (!properties) return std::unexpected(properties.error());
            if (!key) return std::unexpected(key.error());
            if (!value) return std::unexpected(value.error());
            const std::array<Value, 3> forwarded {
                Value::from_reference(*properties), Value::from_reference(*key),
                Value::from_reference(*value),
            };
            return invoke_native(
                machine, "java/util/HashMap", "put",
                "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                forwarded);
        });
    const auto get_property = [&registry](const char* descriptor,
                                          bool fallback) {
        add(registry, "java/util/Properties", "getProperty", descriptor,
            [fallback](Machine& machine,
                       std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto properties = receiver(arguments);
                auto key = reference_argument(arguments, 1U);
                if (!properties) return std::unexpected(properties.error());
                if (!key) return std::unexpected(key.error());
                const std::array<Value, 2> forwarded {
                    Value::from_reference(*properties),
                    Value::from_reference(*key),
                };
                auto value = invoke_native(
                    machine, "java/util/HashMap", "get",
                    "(Ljava/lang/Object;)Ljava/lang/Object;", forwarded);
                if (!value) return std::unexpected(value.error());
                if (!value->has_value()) {
                    return fail(ErrorCode::internal_error,
                                "Properties get returned no value");
                }
                auto reference = value->value().as_reference();
                if (!reference) return std::unexpected(reference.error());
                if (!reference->is_null()) {
                    auto is_string = machine.object_is_instance(
                        *reference, "java/lang/String");
                    if (!is_string) return std::unexpected(is_string.error());
                    if (*is_string) return value;
                }
                if (!fallback) {
                    return std::optional<Value>(Value::from_reference({}));
                }
                auto default_value = reference_argument(arguments, 2U, true);
                if (!default_value) {
                    return std::unexpected(default_value.error());
                }
                return std::optional<Value>(
                    Value::from_reference(*default_value));
            });
    };
    get_property("(Ljava/lang/String;)Ljava/lang/String;", false);
    get_property("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", true);
    const auto load = [&registry](const char* descriptor,
                                  const char* owner) {
        add(registry, "java/util/Properties", "load", descriptor,
            [owner](Machine& machine,
                    std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto properties = receiver(arguments);
                auto input = reference_argument(arguments, 1U);
                if (!properties) return std::unexpected(properties.error());
                if (!input) return std::unexpected(input.error());
                auto content = read_stream(machine, *input, owner);
                if (!content) return std::unexpected(content.error());
                auto parsed = parse_properties(machine, *properties, *content);
                if (!parsed) return std::unexpected(parsed.error());
                return std::optional<Value> {};
            });
    };
    load("(Ljava/io/Reader;)V", "java/io/Reader");
    load("(Ljava/io/InputStream;)V", "java/io/InputStream");
    add(registry, "java/util/Properties", "stringPropertyNames",
        "()Ljava/util/Set;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto properties = receiver(arguments);
            if (!properties) return std::unexpected(properties.error());
            auto set = new_hash_set(machine);
            if (!set) return std::unexpected(set.error());
            auto root = machine.pin_native_root(*set);
            if (!root) return std::unexpected(root.error());
            constexpr std::array<usize, 3> fields {
                kMapSizeField,
                kMapKeysField,
                kMapValuesField,
            };
            std::array<Value, fields.size()> state {};
            auto loaded = machine.heap().read_fields(
                *properties, fields, state);
            if (!loaded) return std::unexpected(loaded.error());
            auto size = state[0U].as_int();
            auto keys = state[1U].as_reference();
            auto values = state[2U].as_reference();
            if (!size || !keys || !values || *size < 0 || keys->is_null() ||
                values->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "Properties storage is invalid");
            }
            auto key_values = machine.heap().read_reference_array(*keys);
            auto property_values = machine.heap().read_reference_array(*values);
            if (!key_values) return std::unexpected(key_values.error());
            if (!property_values) {
                return std::unexpected(property_values.error());
            }
            if (static_cast<usize>(*size) > key_values->size() ||
                static_cast<usize>(*size) > property_values->size()) {
                return fail(ErrorCode::invalid_state,
                            "Properties storage is shorter than its size");
            }
            for (i32 index = 0; index < *size; ++index) {
                const ObjectRef key = (*key_values)[static_cast<usize>(index)];
                const ObjectRef value =
                    (*property_values)[static_cast<usize>(index)];
                auto key_string = machine.object_is_instance(key,
                                                             "java/lang/String");
                auto value_string = machine.object_is_instance(
                    value, "java/lang/String");
                if (!key_string) return std::unexpected(key_string.error());
                if (!value_string) {
                    return std::unexpected(value_string.error());
                }
                if (!*key_string || !*value_string) continue;
                const Value argument = Value::from_reference(key);
                auto added = invoke_checked(
                    machine, *set, "java/util/Set", "add",
                    "(Ljava/lang/Object;)Z",
                    std::span<const Value>(&argument, 1U));
                if (!added) return std::unexpected(added.error());
            }
            return std::optional<Value>(Value::from_reference(*set));
        });
    add(registry, "java/util/Properties", "propertyNames",
        "()Ljava/util/Enumeration;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto properties = receiver(arguments);
            if (!properties) return std::unexpected(properties.error());
            constexpr std::array<usize, 2> fields {
                kMapSizeField,
                kMapKeysField,
            };
            std::array<Value, fields.size()> state {};
            auto loaded = machine.heap().read_fields(
                *properties, fields, state);
            if (!loaded) return std::unexpected(loaded.error());
            auto size = state[0U].as_int();
            auto keys = state[1U].as_reference();
            if (!size || !keys || keys->is_null() || *size < 0) {
                return fail(ErrorCode::invalid_state,
                            "Properties key storage is invalid");
            }
            auto values = allocate_object_array(
                machine, static_cast<usize>(*size));
            if (!values) return std::unexpected(values.error());
            auto values_root = machine.pin_native_root(*values);
            if (!values_root) return std::unexpected(values_root.error());
            if (*size > 0) {
                auto copied = machine.heap().copy_array_range(
                    *keys, 0U, *values, 0U, static_cast<usize>(*size));
                if (!copied) return std::unexpected(copied.error());
            }
            auto enumeration = new_instance(machine, "java/util/ArrayEnumeration");
            if (!enumeration) return std::unexpected(enumeration.error());
            constexpr std::array<usize, 3> enumeration_fields {
                kEnumerationArrayField,
                kEnumerationIndexField,
                kEnumerationSizeField,
            };
            const std::array<Value, enumeration_fields.size()> enumeration_state {
                Value::from_reference(*values),
                Value::from_int(0),
                Value::from_int(*size),
            };
            auto stored = machine.heap().write_fields(
                *enumeration, enumeration_fields, enumeration_state);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*enumeration));
        });
    add(registry, "java/util/Properties", "equals",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto properties = receiver(arguments);
            auto other = reference_argument(arguments, 1U, true);
            if (!properties) return std::unexpected(properties.error());
            if (!other) return std::unexpected(other.error());
            auto equal = map_equals(machine, *properties, *other);
            if (!equal) return std::unexpected(equal.error());
            return std::optional<Value>(Value::from_int(*equal ? 1 : 0));
        });
    add(registry, "java/util/Properties", "store",
        "(Ljava/io/OutputStream;Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto properties = receiver(arguments);
            auto output = reference_argument(arguments, 1U);
            auto comments = reference_argument(arguments, 2U, true);
            if (!properties) return std::unexpected(properties.error());
            if (!output) return std::unexpected(output.error());
            if (!comments) return std::unexpected(comments.error());
            std::u16string serialized;
            if (!comments->is_null()) {
                auto text = string_value(machine, *comments);
                if (!text) return std::unexpected(text.error());
                serialized.push_back(u'#');
                serialized.append(*text);
                serialized.push_back(u'\n');
            }
            constexpr std::array<usize, 3> fields {
                kMapSizeField,
                kMapKeysField,
                kMapValuesField,
            };
            std::array<Value, fields.size()> state {};
            auto loaded = machine.heap().read_fields(
                *properties, fields, state);
            if (!loaded) return std::unexpected(loaded.error());
            auto size = state[0U].as_int();
            auto keys = state[1U].as_reference();
            auto values = state[2U].as_reference();
            if (!size || !keys || !values || *size < 0 || keys->is_null() ||
                values->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "Properties storage is invalid");
            }
            auto key_values = machine.heap().read_reference_array(*keys);
            auto property_values = machine.heap().read_reference_array(*values);
            if (!key_values) return std::unexpected(key_values.error());
            if (!property_values) {
                return std::unexpected(property_values.error());
            }
            if (static_cast<usize>(*size) > key_values->size() ||
                static_cast<usize>(*size) > property_values->size()) {
                return fail(ErrorCode::invalid_state,
                            "Properties storage is shorter than its size");
            }
            for (i32 index = 0; index < *size; ++index) {
                const ObjectRef key = (*key_values)[static_cast<usize>(index)];
                const ObjectRef value =
                    (*property_values)[static_cast<usize>(index)];
                auto key_string = machine.object_is_instance(key,
                                                             "java/lang/String");
                auto value_string = machine.object_is_instance(
                    value, "java/lang/String");
                if (!key_string || !value_string) {
                    return fail_java("java/lang/ClassCastException",
                                     "Properties keys and values must be strings");
                }
                if (!*key_string || !*value_string) {
                    return fail_java("java/lang/ClassCastException",
                                     "Properties keys and values must be strings");
                }
                auto key_text = string_value(machine, key);
                auto value_text = string_value(machine, value);
                if (!key_text) return std::unexpected(key_text.error());
                if (!value_text) return std::unexpected(value_text.error());
                serialized.append(escape_property(*key_text, true));
                serialized.push_back(u'=');
                serialized.append(escape_property(*value_text, false));
                serialized.push_back(u'\n');
            }
            auto written = write_latin1(machine, *output, serialized);
            if (!written) return std::unexpected(written.error());
            return std::optional<Value> {};
        });
}

} // namespace

void register_jdk8_properties_natives(NativeMethodRegistry& registry) {
    register_properties_methods(registry);
}

} // namespace phoneme::vm
