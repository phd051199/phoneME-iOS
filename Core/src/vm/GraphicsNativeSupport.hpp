#pragma once

#include <span>
#include <string>
#include <vector>

#include "phoneme/graphics/Graphics.hpp"
#include "phoneme/vm/Machine.hpp"

namespace phoneme::vm::graphics_native {

[[nodiscard]] inline Result<ObjectRef> receiver(
    std::span<const Value> arguments,
    std::string_view operation) {
    if (arguments.empty()) {
        return fail(ErrorCode::invalid_argument,
                    std::string(operation) + " has no receiver");
    }
    auto reference = arguments.front().as_reference();
    if (!reference || reference->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         std::string(operation) + " receiver is null");
    }
    return *reference;
}

[[nodiscard]] inline Result<ObjectRef> reference_argument(
    std::span<const Value> arguments,
    usize index,
    std::string_view operation,
    bool allow_null = false) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    std::string(operation) + " is missing an argument");
    }
    auto reference = arguments[index].as_reference();
    if (!reference) return std::unexpected(reference.error());
    if (!allow_null && reference->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         std::string(operation) + " argument is null");
    }
    return *reference;
}

[[nodiscard]] inline Result<i32> int_argument(
    std::span<const Value> arguments,
    usize index,
    std::string_view operation) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    std::string(operation) + " is missing an argument");
    }
    return arguments[index].as_int();
}

[[nodiscard]] inline Result<std::u16string> string_text(
    Machine& machine,
    ObjectRef string,
    std::string_view operation) {
    if (string.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         std::string(operation) + " string is null");
    }
    auto class_name = machine.heap().class_name(string);
    if (!class_name || *class_name != "java/lang/String") {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " expects a String");
    }
    return machine.heap().string_value(string);
}

[[nodiscard]] inline std::vector<char32_t> utf32_text(
    std::u16string_view input) {
    std::vector<char32_t> output;
    output.reserve(input.size());
    for (usize index = 0; index < input.size(); ++index) {
        u32 value = static_cast<u16>(input[index]);
        if (value >= 0xD800U && value <= 0xDBFFU &&
            index + 1U < input.size()) {
            const u32 low = static_cast<u16>(input[index + 1U]);
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                value = 0x10000U + ((value - 0xD800U) << 10U) +
                        (low - 0xDC00U);
                ++index;
            }
        }
        output.push_back(static_cast<char32_t>(value));
    }
    return output;
}

[[nodiscard]] inline Result<std::string> utf8_text(
    Machine& machine,
    ObjectRef string,
    std::string_view operation) {
    auto text = string_text(machine, string, operation);
    if (!text) return std::unexpected(text.error());
    std::string output;
    output.reserve(text->size());
    const auto code_points = utf32_text(*text);
    for (char32_t character : code_points) {
        const u32 value = static_cast<u32>(character);
        if (value <= 0x7FU) {
            output.push_back(static_cast<char>(value));
        } else if (value <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (value >> 6U)));
            output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        } else if (value <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (value >> 12U)));
            output.push_back(static_cast<char>(0x80U |
                                               ((value >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (value >> 18U)));
            output.push_back(static_cast<char>(0x80U |
                                               ((value >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U |
                                               ((value >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        }
    }
    return output;
}

[[nodiscard]] inline Result<std::vector<u8>> byte_array_slice(
    Machine& machine,
    ObjectRef array,
    i32 offset,
    i32 length,
    std::string_view operation) {
    if (array.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         std::string(operation) + " byte[] is null");
    }
    if (offset < 0 || length < 0) {
        return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                         std::string(operation) + " byte[] slice is negative");
    }
    auto class_name = machine.heap().class_name(array);
    auto array_length = machine.heap().array_length(array);
    if (!class_name || !array_length || *class_name != "[B") {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " expects byte[]");
    }
    if (static_cast<usize>(offset) > *array_length ||
        static_cast<usize>(length) >
            *array_length - static_cast<usize>(offset)) {
        return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                         std::string(operation) + " byte[] slice is outside the array");
    }
    return machine.heap().read_byte_array(
        array,
        static_cast<usize>(offset),
        static_cast<usize>(length));
}

[[nodiscard]] inline Result<std::vector<graphics::Pixel>> int_array(
    Machine& machine,
    ObjectRef array,
    std::string_view operation) {
    if (array.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         std::string(operation) + " int[] is null");
    }
    auto class_name = machine.heap().class_name(array);
    auto length = machine.heap().array_length(array);
    if (!class_name || !length || *class_name != "[I") {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " expects int[]");
    }
    auto values = machine.heap().read_int_array(array);
    if (!values) return std::unexpected(values.error());
    std::vector<graphics::Pixel> result;
    result.reserve(values->size());
    for (const i32 integer : *values) {
        result.push_back(static_cast<graphics::Pixel>(
            static_cast<u32>(integer)));
    }
    return result;
}

[[nodiscard]] inline std::unexpected<Error> graphics_error(
    const Error& error,
    std::string_view java_class = "java/lang/IllegalArgumentException") {
    if (error.code == ErrorCode::java_exception) {
        return std::unexpected(error);
    }
    if (error.code == ErrorCode::invalid_state) {
        return fail_java("java/lang/IllegalStateException", error.message);
    }
    if (error.code == ErrorCode::out_of_range) {
        return fail_java(std::string(java_class), error.message);
    }
    if (error.code == ErrorCode::overflow) {
        return fail_java("java/lang/OutOfMemoryError", error.message);
    }
    return fail_java(std::string(java_class), error.message);
}

} // namespace phoneme::vm::graphics_native
