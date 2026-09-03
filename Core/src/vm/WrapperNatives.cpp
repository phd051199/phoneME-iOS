#include "WrapperNatives.hpp"

#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/NativeMethodRegistry.hpp"

namespace phoneme::vm {
namespace {

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

[[nodiscard]] std::u16string ascii_text(std::string_view text) {
    std::u16string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(static_cast<char16_t>(
            static_cast<unsigned char>(character)));
    }
    return result;
}

[[nodiscard]] Result<ObjectRef> create_string(Machine& machine,
                                              std::u16string text) {
    auto object = machine.class_states().allocate_instance(
        machine.heap(), "java/lang/String");
    if (!object) {
        return std::unexpected(object.error());
    }
    auto attached = machine.heap().attach_string(*object, std::move(text));
    if (!attached) {
        return std::unexpected(attached.error());
    }
    return *object;
}

[[nodiscard]] Result<std::string> string_ascii(Machine& machine,
                                                ObjectRef reference) {
    if (reference.is_null()) {
        return fail_java("java/lang/NumberFormatException",
                         "numeric string is null");
    }
    auto text = machine.heap().string_value(reference);
    if (!text) {
        return fail_java("java/lang/NumberFormatException",
                         "numeric value is not a String");
    }
    std::string result;
    result.reserve(text->size());
    for (const char16_t character : *text) {
        if (character > 0x7FU) {
            return fail_java("java/lang/NumberFormatException",
                             "numeric string contains a non-ASCII character");
        }
        result.push_back(static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] Result<i64> parse_signed(std::string_view text,
                                       i32 radix,
                                       i64 minimum,
                                       i64 maximum) {
    if (radix < 2 || radix > 36 || text.empty()) {
        return fail_java("java/lang/NumberFormatException",
                         "invalid integer radix or empty input");
    }
    usize index = 0;
    bool negative = false;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        index = 1;
        if (index == text.size()) {
            return fail_java("java/lang/NumberFormatException",
                             "integer input contains only a sign");
        }
    }

    const u64 negative_limit =
        static_cast<u64>(-(minimum + 1)) + 1U;
    const u64 limit = negative ? negative_limit
                               : static_cast<u64>(maximum);
    u64 magnitude = 0;
    for (; index < text.size(); ++index) {
        const char character = text[index];
        u32 digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<u32>(character - '0');
        } else if (character >= 'a' && character <= 'z') {
            digit = 10U + static_cast<u32>(character - 'a');
        } else if (character >= 'A' && character <= 'Z') {
            digit = 10U + static_cast<u32>(character - 'A');
        } else {
            return fail_java("java/lang/NumberFormatException",
                             "integer input contains an invalid digit");
        }
        if (digit >= static_cast<u32>(radix) ||
            magnitude > (limit - digit) / static_cast<u32>(radix)) {
            return fail_java("java/lang/NumberFormatException",
                             "integer input exceeds its target range");
        }
        magnitude = magnitude * static_cast<u32>(radix) + digit;
    }

    if (!negative) {
        return static_cast<i64>(magnitude);
    }
    if (magnitude == negative_limit) {
        return minimum;
    }
    return -static_cast<i64>(magnitude);
}

[[nodiscard]] Result<std::u16string> signed_text(i64 value, i32 radix) {
    if (radix < 2 || radix > 36) {
        radix = 10;
    }
    std::array<char, 96> buffer {};
    auto converted = std::to_chars(buffer.data(),
                                   buffer.data() + buffer.size(),
                                   value,
                                   radix);
    if (converted.ec != std::errc {}) {
        return fail(ErrorCode::internal_error,
                    "failed to format signed wrapper value");
    }
    return ascii_text(std::string_view(
        buffer.data(),
        static_cast<usize>(converted.ptr - buffer.data())));
}

[[nodiscard]] Result<std::u16string> unsigned_text(u64 value, i32 radix) {
    std::array<char, 96> buffer {};
    auto converted = std::to_chars(buffer.data(),
                                   buffer.data() + buffer.size(),
                                   value,
                                   radix);
    if (converted.ec != std::errc {}) {
        return fail(ErrorCode::internal_error,
                    "failed to format unsigned wrapper value");
    }
    return ascii_text(std::string_view(
        buffer.data(),
        static_cast<usize>(converted.ptr - buffer.data())));
}

template <typename Number>
[[nodiscard]] Result<std::u16string> floating_text(Number value) {
    if (std::isnan(value)) {
        return std::u16string(u"NaN");
    }
    if (std::isinf(value)) {
        return std::signbit(value) ? std::u16string(u"-Infinity")
                                   : std::u16string(u"Infinity");
    }
    if (value == static_cast<Number>(0)) {
        return std::signbit(value) ? std::u16string(u"-0.0")
                                   : std::u16string(u"0.0");
    }
    std::array<char, 128> buffer {};
    const int length = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%.*g",
        std::numeric_limits<Number>::max_digits10,
        static_cast<double>(value));
    if (length < 0 || static_cast<usize>(length) >= buffer.size()) {
        return fail(ErrorCode::internal_error,
                    "failed to format floating wrapper value");
    }
    std::string text(buffer.data(), static_cast<usize>(length));
    if (text.find_first_of(".eE") == std::string::npos) {
        text.append(".0");
    }
    return ascii_text(text);
}

template <typename Number>
[[nodiscard]] Result<Number> parse_floating(std::string_view text) {
    if (text == "NaN") {
        return std::numeric_limits<Number>::quiet_NaN();
    }
    if (text == "Infinity" || text == "+Infinity") {
        return std::numeric_limits<Number>::infinity();
    }
    if (text == "-Infinity") {
        return -std::numeric_limits<Number>::infinity();
    }
    std::string input(text);
    char* end = nullptr;
    errno = 0;
    Number result {};
    if constexpr (std::is_same_v<Number, float>) {
        result = std::strtof(input.c_str(), &end);
    } else {
        result = std::strtod(input.c_str(), &end);
    }
    if (end == input.c_str() || end != input.c_str() + input.size() ||
        errno == ERANGE) {
        return fail_java("java/lang/NumberFormatException",
                         "invalid floating-point input");
    }
    return result;
}

[[nodiscard]] Result<ObjectRef> allocate_box(Machine& machine,
                                             std::string_view class_name,
                                             Value value) {
    auto object = machine.class_states().allocate_instance(
        machine.heap(), class_name);
    if (!object) {
        return std::unexpected(object.error());
    }
    auto stored = machine.heap().set_field(*object, 0, value);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    return *object;
}

[[nodiscard]] Result<ObjectRef> receiver(
    std::span<const Value> arguments) {
    if (arguments.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "wrapper method has no receiver");
    }
    auto reference = arguments.front().as_reference();
    if (!reference || reference->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "wrapper receiver is null");
    }
    return *reference;
}

[[nodiscard]] Result<Value> boxed_value(Machine& machine,
                                        ObjectRef object) {
    return machine.heap().field(object, 0);
}

struct IntegralSpec final {
    std::string class_name;
    std::string primitive_descriptor;
    std::string parse_name;
    i64 minimum {0};
    i64 maximum {0};
};

[[nodiscard]] i32 normalize_int(i64 value,
                                std::string_view descriptor) noexcept {
    if (descriptor == "B") {
        return static_cast<i32>(static_cast<i8>(value));
    }
    if (descriptor == "S") {
        return static_cast<i32>(static_cast<i16>(value));
    }
    return static_cast<i32>(value);
}

void register_integral_wrapper(NativeMethodRegistry& registry,
                               IntegralSpec spec) {
    const std::string object_descriptor = "L" + spec.class_name + ";";
    const std::string constructor_descriptor =
        "(" + spec.primitive_descriptor + ")V";
    const std::string value_of_descriptor =
        "(" + spec.primitive_descriptor + ")" + object_descriptor;
    const std::string static_to_string_descriptor =
        "(" + spec.primitive_descriptor + ")Ljava/lang/String;";

    add(registry, spec.class_name, "<init>", constructor_descriptor,
        [spec](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = arguments[1].as_int();
            if (!object) {
                return std::unexpected(object.error());
            }
            if (!value) {
                return std::unexpected(value.error());
            }
            auto stored = machine.heap().set_field(
                *object, 0,
                Value::from_int(normalize_int(*value,
                                              spec.primitive_descriptor)));
            if (!stored) {
                return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
    add(registry, spec.class_name, "valueOf", value_of_descriptor,
        [spec](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_int();
            if (!value) {
                return std::unexpected(value.error());
            }
            auto object = allocate_box(
                machine, spec.class_name,
                Value::from_int(normalize_int(*value,
                                              spec.primitive_descriptor)));
            if (!object) {
                return std::unexpected(object.error());
            }
            return std::optional<Value>(Value::from_reference(*object));
        });

    const auto add_int_conversion = [&](const char* name,
                                        const char* descriptor,
                                        auto convert) {
        add(registry, spec.class_name, name, descriptor,
            [convert](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) {
                    return std::unexpected(object.error());
                }
                auto value = boxed_value(machine, *object);
                if (!value) {
                    return std::unexpected(value.error());
                }
                auto integer = value->as_int();
                if (!integer) {
                    return std::unexpected(integer.error());
                }
                return std::optional<Value>(convert(*integer));
            });
    };
    add_int_conversion("byteValue", "()B", [](i32 value) {
        return Value::from_int(static_cast<i32>(static_cast<i8>(value)));
    });
    add_int_conversion("shortValue", "()S", [](i32 value) {
        return Value::from_int(static_cast<i32>(static_cast<i16>(value)));
    });
    add_int_conversion("intValue", "()I", [](i32 value) {
        return Value::from_int(value);
    });
    add_int_conversion("longValue", "()J", [](i32 value) {
        return Value::from_long(static_cast<i64>(value));
    });
    add_int_conversion("floatValue", "()F", [](i32 value) {
        return Value::from_float(static_cast<float>(value));
    });
    add_int_conversion("doubleValue", "()D", [](i32 value) {
        return Value::from_double(static_cast<double>(value));
    });

    add(registry, spec.class_name, "equals", "(Ljava/lang/Object;)Z",
        [spec](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto other = arguments[1].as_reference();
            if (!object) {
                return std::unexpected(object.error());
            }
            if (!other || other->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto other_class = machine.heap().class_name(*other);
            if (!other_class || *other_class != spec.class_name) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto left = boxed_value(machine, *object);
            auto right = boxed_value(machine, *other);
            if (!left || !right) {
                return fail(ErrorCode::invalid_state,
                            "wrapper object has no primitive payload");
            }
            auto left_value = left->as_int();
            auto right_value = right->as_int();
            if (!left_value || !right_value) {
                return fail(ErrorCode::invalid_state,
                            "wrapper payload is not an int slot");
            }
            return std::optional<Value>(Value::from_int(
                *left_value == *right_value ? 1 : 0));
        });
    add(registry, spec.class_name, "hashCode", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) {
                return std::unexpected(object.error());
            }
            auto value = boxed_value(machine, *object);
            if (!value) {
                return std::unexpected(value.error());
            }
            return std::optional<Value>(*value);
        });
    add(registry, spec.class_name, "toString", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) {
                return std::unexpected(object.error());
            }
            auto value = boxed_value(machine, *object);
            if (!value) {
                return std::unexpected(value.error());
            }
            auto integer = value->as_int();
            if (!integer) {
                return std::unexpected(integer.error());
            }
            auto text = signed_text(*integer, 10);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto string = create_string(machine, std::move(*text));
            if (!string) {
                return std::unexpected(string.error());
            }
            return std::optional<Value>(Value::from_reference(*string));
        });
    add(registry, spec.class_name, "toString", static_to_string_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_int();
            if (!value) {
                return std::unexpected(value.error());
            }
            auto text = signed_text(*value, 10);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto string = create_string(machine, std::move(*text));
            if (!string) {
                return std::unexpected(string.error());
            }
            return std::optional<Value>(Value::from_reference(*string));
        });

    const std::string parse_descriptor =
        "(Ljava/lang/String;)" + spec.primitive_descriptor;
    const std::string parse_radix_descriptor =
        "(Ljava/lang/String;I)" + spec.primitive_descriptor;
    add(registry, spec.class_name, spec.parse_name, parse_descriptor,
        [spec](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto string = arguments[0].as_reference();
            if (!string) {
                return std::unexpected(string.error());
            }
            auto text = string_ascii(machine, *string);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto parsed = parse_signed(*text, 10,
                                       spec.minimum, spec.maximum);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            return std::optional<Value>(Value::from_int(
                normalize_int(*parsed, spec.primitive_descriptor)));
        });
    add(registry, spec.class_name, spec.parse_name, parse_radix_descriptor,
        [spec](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto string = arguments[0].as_reference();
            auto radix = arguments[1].as_int();
            if (!string || !radix) {
                return fail(ErrorCode::invalid_argument,
                            "integral parse arguments are invalid");
            }
            auto text = string_ascii(machine, *string);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto parsed = parse_signed(*text, *radix,
                                       spec.minimum, spec.maximum);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            return std::optional<Value>(Value::from_int(
                normalize_int(*parsed, spec.primitive_descriptor)));
        });

    if (spec.class_name == "java/lang/Integer") {
        add(registry, spec.class_name, "valueOf",
            "(Ljava/lang/String;)Ljava/lang/Integer;",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                if (arguments.empty()) {
                    return fail(ErrorCode::invalid_argument,
                                "Integer.valueOf expects a String");
                }
                auto string = arguments[0].as_reference();
                if (!string) return std::unexpected(string.error());
                auto text = string_ascii(machine, *string);
                if (!text) return std::unexpected(text.error());
                auto parsed = parse_signed(
                    *text, 10,
                    std::numeric_limits<i32>::min(),
                    std::numeric_limits<i32>::max());
                if (!parsed) return std::unexpected(parsed.error());
                auto object = allocate_box(
                    machine, "java/lang/Integer",
                    Value::from_int(static_cast<i32>(*parsed)));
                if (!object) return std::unexpected(object.error());
                return std::optional<Value>(
                    Value::from_reference(*object));
            });
        add(registry, spec.class_name, "valueOf",
            "(Ljava/lang/String;I)Ljava/lang/Integer;",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                if (arguments.size() != 2U) {
                    return fail(ErrorCode::invalid_argument,
                                "Integer.valueOf expects String and radix");
                }
                auto string = arguments[0].as_reference();
                auto radix = arguments[1].as_int();
                if (!string || !radix) {
                    return fail(ErrorCode::invalid_argument,
                                "Integer.valueOf arguments are invalid");
                }
                auto text = string_ascii(machine, *string);
                if (!text) return std::unexpected(text.error());
                auto parsed = parse_signed(
                    *text, *radix,
                    std::numeric_limits<i32>::min(),
                    std::numeric_limits<i32>::max());
                if (!parsed) return std::unexpected(parsed.error());
                auto object = allocate_box(
                    machine, "java/lang/Integer",
                    Value::from_int(static_cast<i32>(*parsed)));
                if (!object) return std::unexpected(object.error());
                return std::optional<Value>(
                    Value::from_reference(*object));
            });
        add(registry, spec.class_name, "compareTo",
            "(Ljava/lang/Integer;)I",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                auto other = arguments[1].as_reference();
                if (!object) {
                    return std::unexpected(object.error());
                }
                if (!other || other->is_null()) {
                    return fail_java("java/lang/NullPointerException",
                                     "Integer.compareTo argument is null");
                }
                auto left = boxed_value(machine, *object);
                auto right = boxed_value(machine, *other);
                if (!left || !right) {
                    return fail(ErrorCode::invalid_state,
                                "Integer payload is missing");
                }
                auto left_value = left->as_int();
                auto right_value = right->as_int();
                if (!left_value || !right_value) {
                    return fail(ErrorCode::invalid_state,
                                "Integer payload is invalid");
                }
                const i32 comparison = *left_value < *right_value ? -1
                    : (*left_value > *right_value ? 1 : 0);
                return std::optional<Value>(Value::from_int(comparison));
            });
        add(registry, spec.class_name, "toString",
            "(II)Ljava/lang/String;",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto value = arguments[0].as_int();
                auto radix = arguments[1].as_int();
                if (!value || !radix) {
                    return fail(ErrorCode::invalid_argument,
                                "Integer.toString arguments are invalid");
                }
                auto text = signed_text(*value, *radix);
                if (!text) {
                    return std::unexpected(text.error());
                }
                auto string = create_string(machine, std::move(*text));
                if (!string) {
                    return std::unexpected(string.error());
                }
                return std::optional<Value>(Value::from_reference(*string));
            });
        const auto add_unsigned = [&registry](const char* name, i32 radix) {
            add(registry, "java/lang/Integer", name,
                "(I)Ljava/lang/String;",
                [radix](Machine& machine, std::span<const Value> arguments)
                    -> Result<std::optional<Value>> {
                    auto value = arguments[0].as_int();
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    auto text = unsigned_text(
                        static_cast<u32>(*value), radix);
                    if (!text) {
                        return std::unexpected(text.error());
                    }
                    auto string = create_string(machine, std::move(*text));
                    if (!string) {
                        return std::unexpected(string.error());
                    }
                    return std::optional<Value>(
                        Value::from_reference(*string));
                });
        };
        add_unsigned("toHexString", 16);
        add_unsigned("toOctalString", 8);
        add_unsigned("toBinaryString", 2);
    }
}

void register_long_wrapper(NativeMethodRegistry& registry) {
    constexpr std::string_view class_name = "java/lang/Long";
    add(registry, std::string(class_name), "<init>", "(J)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = arguments[1].as_long();
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto stored = machine.heap().set_field(
                *object, 0, Value::from_long(*value));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(class_name), "valueOf",
        "(J)Ljava/lang/Long;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_long();
            if (!value) return std::unexpected(value.error());
            auto object = allocate_box(machine, "java/lang/Long",
                                       Value::from_long(*value));
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });
    add(registry, std::string(class_name), "valueOf",
        "(Ljava/lang/String;)Ljava/lang/Long;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto string = arguments[0].as_reference();
            if (!string) return std::unexpected(string.error());
            auto text = string_ascii(machine, *string);
            if (!text) return std::unexpected(text.error());
            auto value = parse_signed(*text, 10,
                                      std::numeric_limits<i64>::min(),
                                      std::numeric_limits<i64>::max());
            if (!value) return std::unexpected(value.error());
            auto object = allocate_box(machine, "java/lang/Long",
                                       Value::from_long(*value));
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });
    const auto conversion = [&registry](const char* name,
                                        const char* descriptor,
                                        auto convert) {
        add(registry, "java/lang/Long", name, descriptor,
            [convert](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                auto value = boxed_value(machine, *object);
                if (!value) return std::unexpected(value.error());
                auto number = value->as_long();
                if (!number) return std::unexpected(number.error());
                return std::optional<Value>(convert(*number));
            });
    };
    conversion("byteValue", "()B", [](i64 value) {
        return Value::from_int(static_cast<i32>(static_cast<i8>(value)));
    });
    conversion("shortValue", "()S", [](i64 value) {
        return Value::from_int(static_cast<i32>(static_cast<i16>(value)));
    });
    conversion("intValue", "()I", [](i64 value) {
        return Value::from_int(static_cast<i32>(static_cast<u32>(value)));
    });
    conversion("longValue", "()J", [](i64 value) {
        return Value::from_long(value);
    });
    conversion("floatValue", "()F", [](i64 value) {
        return Value::from_float(static_cast<float>(value));
    });
    conversion("doubleValue", "()D", [](i64 value) {
        return Value::from_double(static_cast<double>(value));
    });
    add(registry, "java/lang/Long", "equals", "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto other = arguments[1].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!other || other->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto other_class = machine.heap().class_name(*other);
            if (!other_class || *other_class != "java/lang/Long") {
                return std::optional<Value>(Value::from_int(0));
            }
            auto left = boxed_value(machine, *object);
            auto right = boxed_value(machine, *other);
            if (!left || !right) {
                return fail(ErrorCode::invalid_state,
                            "Long payload is missing");
            }
            auto left_value = left->as_long();
            auto right_value = right->as_long();
            if (!left_value || !right_value) {
                return fail(ErrorCode::invalid_state,
                            "Long payload is invalid");
            }
            return std::optional<Value>(Value::from_int(
                *left_value == *right_value ? 1 : 0));
        });
    add(registry, "java/lang/Long", "hashCode", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = boxed_value(machine, *object);
            if (!value) return std::unexpected(value.error());
            auto number = value->as_long();
            if (!number) return std::unexpected(number.error());
            const u64 bits = static_cast<u64>(*number);
            return std::optional<Value>(Value::from_int(static_cast<i32>(
                static_cast<u32>(bits ^ (bits >> 32U)))));
        });
    const auto long_to_string = [](Machine& machine,
                                   i64 value,
                                   i32 radix)
        -> Result<std::optional<Value>> {
        auto text = signed_text(value, radix);
        if (!text) return std::unexpected(text.error());
        auto string = create_string(machine, std::move(*text));
        if (!string) return std::unexpected(string.error());
        return std::optional<Value>(Value::from_reference(*string));
    };
    add(registry, "java/lang/Long", "toString", "()Ljava/lang/String;",
        [long_to_string](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = boxed_value(machine, *object);
            if (!value) return std::unexpected(value.error());
            auto number = value->as_long();
            if (!number) return std::unexpected(number.error());
            return long_to_string(machine, *number, 10);
        });
    add(registry, "java/lang/Long", "toString",
        "(J)Ljava/lang/String;",
        [long_to_string](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_long();
            if (!value) return std::unexpected(value.error());
            return long_to_string(machine, *value, 10);
        });
    add(registry, "java/lang/Long", "toString",
        "(JI)Ljava/lang/String;",
        [long_to_string](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_long();
            auto radix = arguments[1].as_int();
            if (!value || !radix) {
                return fail(ErrorCode::invalid_argument,
                            "Long.toString arguments are invalid");
            }
            return long_to_string(machine, *value, *radix);
        });
    const auto parse_long = [&registry](bool with_radix) {
        add(registry, "java/lang/Long", "parseLong",
            with_radix ? "(Ljava/lang/String;I)J"
                       : "(Ljava/lang/String;)J",
            [with_radix](Machine& machine,
                         std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto string = arguments[0].as_reference();
                if (!string) return std::unexpected(string.error());
                i32 radix = 10;
                if (with_radix) {
                    auto parsed_radix = arguments[1].as_int();
                    if (!parsed_radix) {
                        return std::unexpected(parsed_radix.error());
                    }
                    radix = *parsed_radix;
                }
                auto text = string_ascii(machine, *string);
                if (!text) return std::unexpected(text.error());
                auto value = parse_signed(
                    *text, radix,
                    std::numeric_limits<i64>::min(),
                    std::numeric_limits<i64>::max());
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_long(*value));
            });
    };
    parse_long(false);
    parse_long(true);
    add(registry, "java/lang/Long", "compareTo",
        "(Ljava/lang/Long;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto other = arguments[1].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!other || other->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Long.compareTo argument is null");
            }
            auto left = boxed_value(machine, *object);
            auto right = boxed_value(machine, *other);
            if (!left || !right) {
                return fail(ErrorCode::invalid_state,
                            "Long payload is missing");
            }
            auto left_value = left->as_long();
            auto right_value = right->as_long();
            if (!left_value || !right_value) {
                return fail(ErrorCode::invalid_state,
                            "Long payload is invalid");
            }
            const i32 comparison = *left_value < *right_value ? -1
                : (*left_value > *right_value ? 1 : 0);
            return std::optional<Value>(Value::from_int(comparison));
        });
    const auto add_unsigned = [&registry](const char* name, i32 radix) {
        add(registry, "java/lang/Long", name,
            "(J)Ljava/lang/String;",
            [radix](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto value = arguments[0].as_long();
                if (!value) return std::unexpected(value.error());
                auto text = unsigned_text(static_cast<u64>(*value), radix);
                if (!text) return std::unexpected(text.error());
                auto string = create_string(machine, std::move(*text));
                if (!string) return std::unexpected(string.error());
                return std::optional<Value>(Value::from_reference(*string));
            });
    };
    add_unsigned("toHexString", 16);
    add_unsigned("toOctalString", 8);
    add_unsigned("toBinaryString", 2);
}

void register_boolean_wrapper(NativeMethodRegistry& registry) {
    add(registry, "java/lang/Boolean", "<init>", "(Z)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = arguments[1].as_int();
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto stored = machine.heap().set_field(
                *object, 0, Value::from_int(*value == 0 ? 0 : 1));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/lang/Boolean", "valueOf",
        "(Z)Ljava/lang/Boolean;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_int();
            if (!value) return std::unexpected(value.error());
            auto field = machine.class_states().resolve_field(
                "java/lang/Boolean",
                *value == 0 ? "FALSE" : "TRUE",
                "Ljava/lang/Boolean;",
                true);
            if (!field) return std::unexpected(field.error());
            auto object = machine.class_states().static_field(*field);
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(*object);
        });
    add(registry, "java/lang/Boolean", "valueOf",
        "(Ljava/lang/String;)Ljava/lang/Boolean;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto string = arguments[0].as_reference();
            bool equal = false;
            if (string && !string->is_null()) {
                auto text = machine.heap().string_value(*string);
                if (text) {
                    static constexpr std::u16string_view expected = u"true";
                    equal = text->size() == expected.size();
                    for (usize index = 0; equal && index < text->size(); ++index) {
                        char16_t value = (*text)[index];
                        if (value >= u'A' && value <= u'Z') {
                            value = static_cast<char16_t>(
                                value + (u'a' - u'A'));
                        }
                        equal = value == expected[index];
                    }
                }
            }
            auto field = machine.class_states().resolve_field(
                "java/lang/Boolean",
                equal ? "TRUE" : "FALSE",
                "Ljava/lang/Boolean;",
                true);
            if (!field) return std::unexpected(field.error());
            auto object = machine.class_states().static_field(*field);
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(*object);
        });
    add(registry, "java/lang/Boolean", "booleanValue", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = boxed_value(machine, *object);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(*value);
        });
    add(registry, "java/lang/Boolean", "hashCode", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = boxed_value(machine, *object);
            if (!value) return std::unexpected(value.error());
            auto boolean = value->as_int();
            if (!boolean) return std::unexpected(boolean.error());
            return std::optional<Value>(Value::from_int(
                *boolean == 0 ? 1237 : 1231));
        });
    add(registry, "java/lang/Boolean", "equals", "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto other = arguments[1].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!other || other->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto other_class = machine.heap().class_name(*other);
            if (!other_class || *other_class != "java/lang/Boolean") {
                return std::optional<Value>(Value::from_int(0));
            }
            auto left = boxed_value(machine, *object);
            auto right = boxed_value(machine, *other);
            if (!left || !right) {
                return fail(ErrorCode::invalid_state,
                            "Boolean payload is missing");
            }
            auto left_value = left->as_int();
            auto right_value = right->as_int();
            if (!left_value || !right_value) {
                return fail(ErrorCode::invalid_state,
                            "Boolean payload is invalid");
            }
            return std::optional<Value>(Value::from_int(
                (*left_value != 0) == (*right_value != 0) ? 1 : 0));
        });
    const auto boolean_string = [](Machine& machine, bool value)
        -> Result<std::optional<Value>> {
        auto string = create_string(machine,
                                    value ? std::u16string(u"true")
                                          : std::u16string(u"false"));
        if (!string) return std::unexpected(string.error());
        return std::optional<Value>(Value::from_reference(*string));
    };
    add(registry, "java/lang/Boolean", "toString", "()Ljava/lang/String;",
        [boolean_string](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = boxed_value(machine, *object);
            if (!value) return std::unexpected(value.error());
            auto boolean = value->as_int();
            if (!boolean) return std::unexpected(boolean.error());
            return boolean_string(machine, *boolean != 0);
        });
    add(registry, "java/lang/Boolean", "toString",
        "(Z)Ljava/lang/String;",
        [boolean_string](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_int();
            if (!value) return std::unexpected(value.error());
            return boolean_string(machine, *value != 0);
        });
    add(registry, "java/lang/Boolean", "getBoolean",
        "(Ljava/lang/String;)Z",
        [](Machine&, std::span<const Value>)
            -> Result<std::optional<Value>> {
            return std::optional<Value>(Value::from_int(0));
        });
    add(registry, "java/lang/Boolean", "parseBoolean",
        "(Ljava/lang/String;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto string = arguments[0].as_reference();
            if (!string || string->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto text = machine.heap().string_value(*string);
            if (!text) return std::optional<Value>(Value::from_int(0));
            const std::u16string expected(u"true");
            bool equal = text->size() == expected.size();
            for (usize index = 0; equal && index < text->size(); ++index) {
                char16_t value = (*text)[index];
                if (value >= u'A' && value <= u'Z') {
                    value = static_cast<char16_t>(value + (u'a' - u'A'));
                }
                equal = value == expected[index];
            }
            return std::optional<Value>(Value::from_int(equal ? 1 : 0));
        });
}

void register_character_wrapper(NativeMethodRegistry& registry) {
    add(registry, "java/lang/Character", "<init>", "(C)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = arguments[1].as_int();
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto stored = machine.heap().set_field(
                *object, 0, Value::from_int(static_cast<u16>(*value)));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/lang/Character", "valueOf",
        "(C)Ljava/lang/Character;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_int();
            if (!value) return std::unexpected(value.error());
            auto object = allocate_box(machine, "java/lang/Character",
                Value::from_int(static_cast<u16>(*value)));
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });
    add(registry, "java/lang/Character", "charValue", "()C",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = boxed_value(machine, *object);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(*value);
        });
    add(registry, "java/lang/Character", "hashCode", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = boxed_value(machine, *object);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(*value);
        });
    add(registry, "java/lang/Character", "equals", "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto other = arguments[1].as_reference();
            if (!object) return std::unexpected(object.error());
            if (!other || other->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto other_class = machine.heap().class_name(*other);
            if (!other_class || *other_class != "java/lang/Character") {
                return std::optional<Value>(Value::from_int(0));
            }
            auto left = boxed_value(machine, *object);
            auto right = boxed_value(machine, *other);
            if (!left || !right) {
                return fail(ErrorCode::invalid_state,
                            "Character payload is missing");
            }
            auto left_value = left->as_int();
            auto right_value = right->as_int();
            if (!left_value || !right_value) {
                return fail(ErrorCode::invalid_state,
                            "Character payload is invalid");
            }
            return std::optional<Value>(Value::from_int(
                *left_value == *right_value ? 1 : 0));
        });
    add(registry, "java/lang/Character", "toString",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = boxed_value(machine, *object);
            if (!value) return std::unexpected(value.error());
            auto character = value->as_int();
            if (!character) return std::unexpected(character.error());
            auto string = create_string(machine, std::u16string(
                1, static_cast<char16_t>(static_cast<u16>(*character))));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
    const auto predicate = [&registry](const char* name, auto test) {
        add(registry, "java/lang/Character", name, "(C)Z",
            [test](Machine&, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto value = arguments[0].as_int();
                if (!value) return std::unexpected(value.error());
                const char16_t character = static_cast<char16_t>(
                    static_cast<u16>(*value));
                return std::optional<Value>(Value::from_int(
                    test(character) ? 1 : 0));
            });
    };
    predicate("isDigit", [](char16_t value) {
        return value >= u'0' && value <= u'9';
    });
    predicate("isLetter", [](char16_t value) {
        return (value >= u'A' && value <= u'Z') ||
               (value >= u'a' && value <= u'z');
    });
    predicate("isLetterOrDigit", [](char16_t value) {
        return (value >= u'0' && value <= u'9') ||
               (value >= u'A' && value <= u'Z') ||
               (value >= u'a' && value <= u'z');
    });
    predicate("isWhitespace", [](char16_t value) {
        return value == u' ' || value == u'\t' || value == u'\n' ||
               value == u'\r' || value == u'\f';
    });
    predicate("isLowerCase", [](char16_t value) {
        return value >= u'a' && value <= u'z';
    });
    predicate("isUpperCase", [](char16_t value) {
        return value >= u'A' && value <= u'Z';
    });
    add(registry, "java/lang/Character", "digit", "(CI)I",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "Character.digit expects character and radix");
            }
            auto raw_character = arguments[0].as_int();
            auto radix = arguments[1].as_int();
            if (!raw_character) return std::unexpected(raw_character.error());
            if (!radix) return std::unexpected(radix.error());
            if (*radix < 2 || *radix > 36) {
                return std::optional<Value>(Value::from_int(-1));
            }
            const char16_t character = static_cast<char16_t>(
                static_cast<u16>(*raw_character));
            i32 digit = -1;
            if (character >= u'0' && character <= u'9') {
                digit = static_cast<i32>(character - u'0');
            } else if (character >= u'A' && character <= u'Z') {
                digit = 10 + static_cast<i32>(character - u'A');
            } else if (character >= u'a' && character <= u'z') {
                digit = 10 + static_cast<i32>(character - u'a');
            }
            if (digit >= *radix) digit = -1;
            return std::optional<Value>(Value::from_int(digit));
        });
    const auto case_method = [&registry](const char* name, bool upper) {
        add(registry, "java/lang/Character", name, "(C)C",
            [upper](Machine&, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto value = arguments[0].as_int();
                if (!value) return std::unexpected(value.error());
                char16_t character = static_cast<char16_t>(
                    static_cast<u16>(*value));
                if (upper && character >= u'a' && character <= u'z') {
                    character = static_cast<char16_t>(
                        character - (u'a' - u'A'));
                } else if (!upper && character >= u'A' && character <= u'Z') {
                    character = static_cast<char16_t>(
                        character + (u'a' - u'A'));
                }
                return std::optional<Value>(Value::from_int(
                    static_cast<i32>(static_cast<u16>(character))));
            });
    };
    case_method("toLowerCase", false);
    case_method("toUpperCase", true);
}

template <typename Number>
[[nodiscard]] i32 floating_to_int(Number value) noexcept {
    if (std::isnan(value)) return 0;
    if (value >= static_cast<Number>(std::numeric_limits<i32>::max()))
        return std::numeric_limits<i32>::max();
    if (value <= static_cast<Number>(std::numeric_limits<i32>::min()))
        return std::numeric_limits<i32>::min();
    return static_cast<i32>(value);
}

template <typename Number>
[[nodiscard]] i64 floating_to_long(Number value) noexcept {
    if (std::isnan(value)) return 0;
    if (value >= static_cast<Number>(std::numeric_limits<i64>::max()))
        return std::numeric_limits<i64>::max();
    if (value <= static_cast<Number>(std::numeric_limits<i64>::min()))
        return std::numeric_limits<i64>::min();
    return static_cast<i64>(value);
}

template <typename Number>
void register_floating_wrapper(NativeMethodRegistry& registry,
                               std::string class_name,
                               std::string primitive_descriptor) {
    const bool is_float = primitive_descriptor == "F";
    const std::string object_descriptor = "L" + class_name + ";";
    add(registry, class_name, "<init>",
        "(" + primitive_descriptor + ")V",
        [is_float](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            Value value;
            if (is_float) {
                auto parsed = arguments[1].as_float();
                if (!parsed) return std::unexpected(parsed.error());
                value = Value::from_float(*parsed);
            } else {
                auto parsed = arguments[1].as_double();
                if (!parsed) return std::unexpected(parsed.error());
                value = Value::from_double(*parsed);
            }
            auto stored = machine.heap().set_field(*object, 0, value);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    if (is_float) {
        add(registry, class_name, "<init>", "(D)V",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                auto parsed = arguments[1].as_double();
                if (!object) return std::unexpected(object.error());
                if (!parsed) return std::unexpected(parsed.error());
                auto stored = machine.heap().set_field(
                    *object, 0, Value::from_float(static_cast<float>(*parsed)));
                if (!stored) return std::unexpected(stored.error());
                return std::optional<Value> {};
            });
    }
    add(registry, class_name, "valueOf",
        "(" + primitive_descriptor + ")" + object_descriptor,
        [class_name, is_float](Machine& machine,
                               std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            Value value;
            if (is_float) {
                auto parsed = arguments[0].as_float();
                if (!parsed) return std::unexpected(parsed.error());
                value = Value::from_float(*parsed);
            } else {
                auto parsed = arguments[0].as_double();
                if (!parsed) return std::unexpected(parsed.error());
                value = Value::from_double(*parsed);
            }
            auto object = allocate_box(machine, class_name, value);
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });
    const auto read_number = [is_float](Machine& machine,
                                        std::span<const Value> arguments)
        -> Result<Number> {
        auto object = receiver(arguments);
        if (!object) return std::unexpected(object.error());
        auto value = boxed_value(machine, *object);
        if (!value) return std::unexpected(value.error());
        if (is_float) {
            auto parsed = value->as_float();
            if (!parsed) return std::unexpected(parsed.error());
            return static_cast<Number>(*parsed);
        }
        auto parsed = value->as_double();
        if (!parsed) return std::unexpected(parsed.error());
        return static_cast<Number>(*parsed);
    };
    add(registry, class_name, "byteValue", "()B",
        [read_number](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = read_number(machine, arguments);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(static_cast<i32>(
                static_cast<i8>(floating_to_int(*value)))));
        });
    add(registry, class_name, "shortValue", "()S",
        [read_number](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = read_number(machine, arguments);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(static_cast<i32>(
                static_cast<i16>(floating_to_int(*value)))));
        });
    add(registry, class_name, "intValue", "()I",
        [read_number](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = read_number(machine, arguments);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(
                floating_to_int(*value)));
        });
    add(registry, class_name, "longValue", "()J",
        [read_number](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = read_number(machine, arguments);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_long(
                floating_to_long(*value)));
        });
    add(registry, class_name, "floatValue", "()F",
        [read_number](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = read_number(machine, arguments);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_float(
                static_cast<float>(*value)));
        });
    add(registry, class_name, "doubleValue", "()D",
        [read_number](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = read_number(machine, arguments);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_double(
                static_cast<double>(*value)));
        });
    add(registry, class_name, "isNaN", "()Z",
        [read_number](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = read_number(machine, arguments);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(
                std::isnan(*value) ? 1 : 0));
        });
    add(registry, class_name, "isNaN",
        "(" + primitive_descriptor + ")Z",
        [is_float](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            const bool result = is_float
                ? arguments[0].as_float().transform([](float value) {
                      return std::isnan(value);
                  }).value_or(false)
                : arguments[0].as_double().transform([](double value) {
                      return std::isnan(value);
                  }).value_or(false);
            return std::optional<Value>(Value::from_int(result ? 1 : 0));
        });
    add(registry, class_name, "isInfinite", "()Z",
        [read_number](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = read_number(machine, arguments);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(
                std::isinf(*value) ? 1 : 0));
        });
    add(registry, class_name, "isInfinite",
        "(" + primitive_descriptor + ")Z",
        [is_float](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            const bool result = is_float
                ? arguments[0].as_float().transform([](float value) {
                      return std::isinf(value);
                  }).value_or(false)
                : arguments[0].as_double().transform([](double value) {
                      return std::isinf(value);
                  }).value_or(false);
            return std::optional<Value>(Value::from_int(result ? 1 : 0));
        });
    add(registry, class_name, "toString", "()Ljava/lang/String;",
        [read_number](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = read_number(machine, arguments);
            if (!value) return std::unexpected(value.error());
            auto text = floating_text(*value);
            if (!text) return std::unexpected(text.error());
            auto string = create_string(machine, std::move(*text));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
    add(registry, class_name, "toString",
        "(" + primitive_descriptor + ")Ljava/lang/String;",
        [is_float](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            Result<std::u16string> text = is_float
                ? arguments[0].as_float().and_then([](float value) {
                      return floating_text(value);
                  })
                : arguments[0].as_double().and_then([](double value) {
                      return floating_text(value);
                  });
            if (!text) return std::unexpected(text.error());
            auto string = create_string(machine, std::move(*text));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
    add(registry, class_name,
        is_float ? "parseFloat" : "parseDouble",
        is_float ? "(Ljava/lang/String;)F" : "(Ljava/lang/String;)D",
        [is_float](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto string = arguments[0].as_reference();
            if (!string) return std::unexpected(string.error());
            auto text = string_ascii(machine, *string);
            if (!text) return std::unexpected(text.error());
            if (is_float) {
                auto value = parse_floating<float>(*text);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_float(*value));
            }
            auto value = parse_floating<double>(*text);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_double(*value));
        });
    add(registry, class_name, "valueOf",
        "(Ljava/lang/String;)" + object_descriptor,
        [class_name, is_float](Machine& machine,
                               std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto string = arguments[0].as_reference();
            if (!string) return std::unexpected(string.error());
            auto text = string_ascii(machine, *string);
            if (!text) return std::unexpected(text.error());
            Value boxed;
            if (is_float) {
                auto value = parse_floating<float>(*text);
                if (!value) return std::unexpected(value.error());
                boxed = Value::from_float(*value);
            } else {
                auto value = parse_floating<double>(*text);
                if (!value) return std::unexpected(value.error());
                boxed = Value::from_double(*value);
            }
            auto object = allocate_box(machine, class_name, boxed);
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });
    add(registry, class_name, "hashCode", "()I",
        [read_number, is_float](Machine& machine,
                                std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = read_number(machine, arguments);
            if (!value) return std::unexpected(value.error());
            if (is_float) {
                float narrowed = static_cast<float>(*value);
                u32 bits = std::isnan(narrowed)
                    ? 0x7FC00000U
                    : std::bit_cast<u32>(narrowed);
                return std::optional<Value>(Value::from_int(
                    static_cast<i32>(bits)));
            }
            double widened = static_cast<double>(*value);
            u64 bits = std::isnan(widened)
                ? 0x7FF8000000000000ULL
                : std::bit_cast<u64>(widened);
            return std::optional<Value>(Value::from_int(static_cast<i32>(
                static_cast<u32>(bits ^ (bits >> 32U)))));
        });
    add(registry, class_name, "equals", "(Ljava/lang/Object;)Z",
        [class_name, read_number, is_float](
            Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto other = arguments[1].as_reference();
            if (!other || other->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto other_class = machine.heap().class_name(*other);
            if (!other_class || *other_class != class_name) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto left = read_number(machine, arguments);
            if (!left) return std::unexpected(left.error());
            auto other_value = boxed_value(machine, *other);
            if (!other_value) return std::unexpected(other_value.error());
            bool equal = false;
            if (is_float) {
                auto right = other_value->as_float();
                if (!right) return std::unexpected(right.error());
                const float left_float = static_cast<float>(*left);
                const u32 left_bits = std::isnan(left_float)
                    ? 0x7FC00000U : std::bit_cast<u32>(left_float);
                const u32 right_bits = std::isnan(*right)
                    ? 0x7FC00000U : std::bit_cast<u32>(*right);
                equal = left_bits == right_bits;
            } else {
                auto right = other_value->as_double();
                if (!right) return std::unexpected(right.error());
                const double left_double = static_cast<double>(*left);
                const u64 left_bits = std::isnan(left_double)
                    ? 0x7FF8000000000000ULL
                    : std::bit_cast<u64>(left_double);
                const u64 right_bits = std::isnan(*right)
                    ? 0x7FF8000000000000ULL
                    : std::bit_cast<u64>(*right);
                equal = left_bits == right_bits;
            }
            return std::optional<Value>(Value::from_int(equal ? 1 : 0));
        });
}

constexpr usize kThrowableMessageField = 0U;
constexpr usize kThrowableCauseField = 1U;
constexpr usize kThrowableCauseInitializedField = 2U;
constexpr usize kThrowableSuppressedField = 3U;
constexpr usize kThrowableSuppressedCountField = 4U;

[[nodiscard]] Status initialize_throwable(Machine& machine,
                                          ObjectRef throwable,
                                          ObjectRef message,
                                          ObjectRef cause,
                                          bool cause_initialized) {
    auto message_stored = machine.heap().set_field(
        throwable, kThrowableMessageField, Value::from_reference(message));
    auto cause_stored = machine.heap().set_field(
        throwable, kThrowableCauseField, Value::from_reference(cause));
    auto initialized_stored = machine.heap().set_field(
        throwable, kThrowableCauseInitializedField,
        Value::from_int(cause_initialized ? 1 : 0));
    auto suppressed_stored = machine.heap().set_field(
        throwable, kThrowableSuppressedField, Value::from_reference({}));
    auto suppressed_count_stored = machine.heap().set_field(
        throwable, kThrowableSuppressedCountField, Value::from_int(0));
    if (!message_stored) return message_stored;
    if (!cause_stored) return cause_stored;
    if (!initialized_stored) return initialized_stored;
    if (!suppressed_stored) return suppressed_stored;
    return suppressed_count_stored;
}

[[nodiscard]] Result<ObjectRef> throwable_reference_field(
    Machine& machine,
    ObjectRef throwable,
    usize index) {
    auto value = machine.heap().field(throwable, index);
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Result<std::u16string> throwable_text(Machine& machine,
                                                    ObjectRef throwable) {
    auto class_name = machine.heap().class_name(throwable);
    if (!class_name) return std::unexpected(class_name.error());
    std::u16string text;
    text.reserve(class_name->size() + 16U);
    for (const char character : *class_name) {
        text.push_back(character == '/'
            ? u'.'
            : static_cast<char16_t>(static_cast<unsigned char>(character)));
    }
    auto message = throwable_reference_field(machine, throwable,
                                              kThrowableMessageField);
    if (!message) return std::unexpected(message.error());
    if (!message->is_null()) {
        auto detail = machine.heap().string_value(*message);
        if (!detail) return std::unexpected(detail.error());
        if (!detail->empty()) {
            text.append(u": ");
            text.append(*detail);
        }
    }
    return text;
}

void register_throwable_natives(NativeMethodRegistry& registry) {
    static constexpr std::array<std::string_view, 49U> classes {{
        "java/lang/Throwable",
        "java/lang/Exception",
        "java/lang/RuntimeException",
        "java/lang/Error",
        "java/lang/VirtualMachineError",
        "java/lang/LinkageError",
        "java/lang/BootstrapMethodError",
        "java/lang/ExceptionInInitializerError",
        "java/lang/NoClassDefFoundError",
        "java/lang/ClassFormatError",
        "java/lang/VerifyError",
        "java/lang/IncompatibleClassChangeError",
        "java/lang/NoSuchMethodError",
        "java/lang/NoSuchFieldError",
        "java/lang/AbstractMethodError",
        "java/lang/InstantiationError",
        "java/lang/UnsatisfiedLinkError",
        "java/lang/NullPointerException",
        "java/lang/ArithmeticException",
        "java/lang/ArrayIndexOutOfBoundsException",
        "java/lang/NegativeArraySizeException",
        "java/lang/ArrayStoreException",
        "java/lang/IllegalMonitorStateException",
        "java/lang/ClassCastException",
        "java/lang/IllegalArgumentException",
        "java/lang/IllegalStateException",
        "java/lang/OutOfMemoryError",
        "java/lang/StackOverflowError",
        "java/lang/InterruptedException",
        "java/lang/NumberFormatException",
        "java/lang/ClassNotFoundException",
        "java/lang/InstantiationException",
        "java/lang/IllegalAccessException",
        "java/lang/CloneNotSupportedException",
        "java/lang/IndexOutOfBoundsException",
        "java/lang/StringIndexOutOfBoundsException",
        "java/lang/UnsupportedOperationException",
        "java/lang/SecurityException",
        "java/lang/IllegalThreadStateException",
        "java/io/IOException",
        "java/io/EOFException",
        "java/io/InterruptedIOException",
        "java/io/UTFDataFormatException",
        "java/io/UnsupportedEncodingException",
        "java/nio/BufferUnderflowException",
        "java/nio/BufferOverflowException",
        "java/security/NoSuchAlgorithmException",
        "javax/wireless/messaging/SizeExceededException",
        "java/util/NoSuchElementException",
    }};
    for (const std::string_view class_name : classes) {
        add(registry, std::string(class_name), "<init>", "()V",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                auto initialized = initialize_throwable(
                    machine, *object, {}, {}, false);
                if (!initialized) return std::unexpected(initialized.error());
                return std::optional<Value> {};
            });
        add(registry, std::string(class_name), "<init>",
            "(Ljava/lang/String;)V",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                auto message = arguments[1].as_reference();
                if (!object) return std::unexpected(object.error());
                if (!message) return std::unexpected(message.error());
                auto initialized = initialize_throwable(
                    machine, *object, *message, {}, false);
                if (!initialized) return std::unexpected(initialized.error());
                return std::optional<Value> {};
            });
        if (class_name != "java/lang/Throwable" &&
            class_name != "java/lang/ExceptionInInitializerError") {
            add(registry, std::string(class_name), "<init>",
                "(Ljava/lang/String;Ljava/lang/Throwable;)V",
                [](Machine& machine, std::span<const Value> arguments)
                    -> Result<std::optional<Value>> {
                    auto object = receiver(arguments);
                    auto message = arguments[1].as_reference();
                    auto cause = arguments[2].as_reference();
                    if (!object || !message || !cause) {
                        return fail(ErrorCode::invalid_argument,
                                    "Throwable constructor arguments are invalid");
                    }
                    auto initialized = initialize_throwable(
                        machine, *object, *message, *cause, true);
                    if (!initialized) {
                        return std::unexpected(initialized.error());
                    }
                    return std::optional<Value> {};
                });
            add(registry, std::string(class_name), "<init>",
                "(Ljava/lang/Throwable;)V",
                [](Machine& machine, std::span<const Value> arguments)
                    -> Result<std::optional<Value>> {
                    auto object = receiver(arguments);
                    auto cause = arguments[1].as_reference();
                    if (!object || !cause) {
                        return fail(ErrorCode::invalid_argument,
                                    "Throwable cause constructor is invalid");
                    }
                    ObjectRef message {};
                    if (!cause->is_null()) {
                        auto text = throwable_text(machine, *cause);
                        if (!text) return std::unexpected(text.error());
                        auto string = create_string(machine, std::move(*text));
                        if (!string) return std::unexpected(string.error());
                        message = *string;
                    }
                    auto initialized = initialize_throwable(
                        machine, *object, message, *cause, true);
                    if (!initialized) return std::unexpected(initialized.error());
                    return std::optional<Value> {};
                });
        }
    }

    for (const std::string_view class_name : {
             std::string_view("java/lang/ArrayIndexOutOfBoundsException"),
             std::string_view("java/lang/StringIndexOutOfBoundsException")}) {
        add(registry, std::string(class_name), "<init>", "(I)V",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                auto index = arguments[1].as_int();
                if (!object) return std::unexpected(object.error());
                if (!index) return std::unexpected(index.error());
                auto text = signed_text(*index, 10);
                if (!text) return std::unexpected(text.error());
                auto runtime_class = machine.heap().class_name(*object);
                if (!runtime_class) {
                    return std::unexpected(runtime_class.error());
                }
                std::u16string message_text =
                    *runtime_class ==
                            "java/lang/ArrayIndexOutOfBoundsException"
                        ? std::u16string(u"Array index out of range: ")
                        : std::u16string(u"String index out of range: ");
                message_text.append(*text);
                auto message = create_string(
                    machine, std::move(message_text));
                if (!message) return std::unexpected(message.error());
                auto initialized = initialize_throwable(
                    machine, *object, *message, {}, false);
                if (!initialized) return std::unexpected(initialized.error());
                return std::optional<Value> {};
            });
    }

    add(registry, "java/lang/ExceptionInInitializerError", "<init>",
        "(Ljava/lang/Throwable;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto cause = arguments[1].as_reference();
            if (!object || !cause) {
                return fail(ErrorCode::invalid_argument,
                            "ExceptionInInitializerError cause is invalid");
            }
            auto initialized = initialize_throwable(
                machine, *object, {}, *cause, true);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/lang/ExceptionInInitializerError", "getException",
        "()Ljava/lang/Throwable;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto cause = throwable_reference_field(
                machine, *object, kThrowableCauseField);
            if (!cause) return std::unexpected(cause.error());
            return std::optional<Value>(Value::from_reference(*cause));
        });

    add(registry, "java/lang/Throwable", "<init>",
        "(Ljava/lang/String;Ljava/lang/Throwable;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto message = arguments[1].as_reference();
            auto cause = arguments[2].as_reference();
            if (!object || !message || !cause) {
                return fail(ErrorCode::invalid_argument,
                            "Throwable constructor arguments are invalid");
            }
            auto initialized = initialize_throwable(
                machine, *object, *message, *cause, true);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/lang/Throwable", "<init>",
        "(Ljava/lang/Throwable;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto cause = arguments[1].as_reference();
            if (!object || !cause) {
                return fail(ErrorCode::invalid_argument,
                            "Throwable cause constructor is invalid");
            }
            ObjectRef message {};
            if (!cause->is_null()) {
                auto text = throwable_text(machine, *cause);
                if (!text) return std::unexpected(text.error());
                auto string = create_string(machine, std::move(*text));
                if (!string) return std::unexpected(string.error());
                message = *string;
            }
            auto initialized = initialize_throwable(
                machine, *object, message, *cause, true);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "java/lang/Throwable", "getMessage",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto message = throwable_reference_field(
                machine, *object, kThrowableMessageField);
            if (!message) return std::unexpected(message.error());
            return std::optional<Value>(Value::from_reference(*message));
        });
    add(registry, "java/lang/Throwable", "getLocalizedMessage",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto message = throwable_reference_field(
                machine, *object, kThrowableMessageField);
            if (!message) return std::unexpected(message.error());
            return std::optional<Value>(Value::from_reference(*message));
        });
    add(registry, "java/lang/Throwable", "getCause",
        "()Ljava/lang/Throwable;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto cause = throwable_reference_field(
                machine, *object, kThrowableCauseField);
            if (!cause) return std::unexpected(cause.error());
            return std::optional<Value>(Value::from_reference(*cause));
        });
    add(registry, "java/lang/Throwable", "initCause",
        "(Ljava/lang/Throwable;)Ljava/lang/Throwable;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto cause = arguments[1].as_reference();
            if (!object || !cause) {
                return fail(ErrorCode::invalid_argument,
                            "Throwable.initCause argument is invalid");
            }
            if (*cause == *object) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "self-causation is not permitted");
            }
            auto initialized_value = machine.heap().field(
                *object, kThrowableCauseInitializedField);
            if (!initialized_value) {
                return std::unexpected(initialized_value.error());
            }
            auto initialized = initialized_value->as_int();
            if (!initialized) return std::unexpected(initialized.error());
            if (*initialized != 0) {
                return fail_java("java/lang/IllegalStateException",
                                 "Throwable cause is already initialized");
            }
            auto cause_stored = machine.heap().set_field(
                *object, kThrowableCauseField,
                Value::from_reference(*cause));
            auto initialized_stored = machine.heap().set_field(
                *object, kThrowableCauseInitializedField,
                Value::from_int(1));
            if (!cause_stored) return std::unexpected(cause_stored.error());
            if (!initialized_stored) {
                return std::unexpected(initialized_stored.error());
            }
            return std::optional<Value>(Value::from_reference(*object));
        });
    add(registry, "java/lang/Throwable", "addSuppressed",
        "(Ljava/lang/Throwable;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "Throwable.addSuppressed argument is missing");
            }
            auto exception = arguments[1].as_reference();
            if (!exception || exception->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "suppressed exception is null");
            }
            if (*exception == *object) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "self-suppression is not permitted");
            }
            auto count_value = machine.heap().field(
                *object, kThrowableSuppressedCountField);
            auto array_value = machine.heap().field(
                *object, kThrowableSuppressedField);
            if (!count_value) return std::unexpected(count_value.error());
            if (!array_value) return std::unexpected(array_value.error());
            auto count = count_value->as_int();
            auto array = array_value->as_reference();
            if (!count) return std::unexpected(count.error());
            if (!array) return std::unexpected(array.error());
            if (*count < 0) {
                return fail(ErrorCode::invalid_state,
                            "Throwable suppressed count is negative");
            }
            usize capacity = 0U;
            if (!array->is_null()) {
                auto length = machine.heap().array_length(*array);
                if (!length) return std::unexpected(length.error());
                capacity = *length;
            }
            if (static_cast<usize>(*count) >= capacity) {
                const usize new_capacity = capacity == 0U ? 4U : capacity * 2U;
                auto replacement = machine.heap().allocate_array(
                    "[Ljava/lang/Throwable;", new_capacity,
                    Value::from_reference({}));
                if (!replacement) return std::unexpected(replacement.error());
                if (capacity != 0U) {
                    auto copied = machine.heap().copy_array_range(
                        *array, 0U, *replacement, 0U, capacity);
                    if (!copied) return std::unexpected(copied.error());
                }
                auto stored = machine.heap().set_field(
                    *object, kThrowableSuppressedField,
                    Value::from_reference(*replacement));
                if (!stored) return std::unexpected(stored.error());
                *array = *replacement;
            }
            auto appended = machine.heap().set_element(
                *array, static_cast<usize>(*count),
                Value::from_reference(*exception));
            if (!appended) return std::unexpected(appended.error());
            auto updated = machine.heap().set_field(
                *object, kThrowableSuppressedCountField,
                Value::from_int(*count + 1));
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "java/lang/Throwable", "getSuppressed",
        "()[Ljava/lang/Throwable;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto count_value = machine.heap().field(
                *object, kThrowableSuppressedCountField);
            auto array_value = machine.heap().field(
                *object, kThrowableSuppressedField);
            if (!count_value) return std::unexpected(count_value.error());
            if (!array_value) return std::unexpected(array_value.error());
            auto count = count_value->as_int();
            auto array = array_value->as_reference();
            if (!count) return std::unexpected(count.error());
            if (!array) return std::unexpected(array.error());
            if (*count < 0) {
                return fail(ErrorCode::invalid_state,
                            "Throwable suppressed count is negative");
            }
            auto result = machine.heap().allocate_array(
                "[Ljava/lang/Throwable;", static_cast<usize>(*count),
                Value::from_reference({}));
            if (!result) return std::unexpected(result.error());
            if (*count > 0) {
                if (array->is_null()) {
                    return fail(ErrorCode::invalid_state,
                                "Throwable suppressed storage is missing");
                }
                auto copied = machine.heap().copy_array_range(
                    *array, 0U, *result, 0U, static_cast<usize>(*count));
                if (!copied) return std::unexpected(copied.error());
            }
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/lang/Throwable", "toString",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto text = throwable_text(machine, *object);
            if (!text) return std::unexpected(text.error());
            auto string = create_string(machine, std::move(*text));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
    add(registry, "java/lang/Throwable", "printStackTrace", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto text = throwable_text(machine, *object);
            if (!text) return std::unexpected(text.error());
            text->push_back(u'\n');
            machine.append_console(*text);
            return std::optional<Value> {};
        });
}

} // namespace

void register_wrapper_natives(NativeMethodRegistry& registry) {
    register_throwable_natives(registry);
    register_boolean_wrapper(registry);
    register_integral_wrapper(registry, IntegralSpec{
        .class_name = "java/lang/Byte",
        .primitive_descriptor = "B",
        .parse_name = "parseByte",
        .minimum = std::numeric_limits<i8>::min(),
        .maximum = std::numeric_limits<i8>::max(),
    });
    register_integral_wrapper(registry, IntegralSpec{
        .class_name = "java/lang/Short",
        .primitive_descriptor = "S",
        .parse_name = "parseShort",
        .minimum = std::numeric_limits<i16>::min(),
        .maximum = std::numeric_limits<i16>::max(),
    });
    register_integral_wrapper(registry, IntegralSpec{
        .class_name = "java/lang/Integer",
        .primitive_descriptor = "I",
        .parse_name = "parseInt",
        .minimum = std::numeric_limits<i32>::min(),
        .maximum = std::numeric_limits<i32>::max(),
    });
    register_long_wrapper(registry);
    register_character_wrapper(registry);
    register_floating_wrapper<float>(registry,
                                     "java/lang/Float", "F");
    register_floating_wrapper<double>(registry,
                                      "java/lang/Double", "D");
}

} // namespace phoneme::vm
