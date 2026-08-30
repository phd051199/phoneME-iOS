#include "phoneme/vm/NativeMethodRegistry.hpp"
#include "phoneme/vm/Machine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "ArrayDequeNatives.hpp"
#include "BluetoothNatives.hpp"
#include "CanvasNatives.hpp"
#include "ChoiceNatives.hpp"
#include "ClassNatives.hpp"
#include "ConnectionNatives.hpp"
#include "ConsoleNatives.hpp"
#include "FileNatives.hpp"
#include "GameCanvasNatives.hpp"
#include "GameApiNatives.hpp"
#include "GraphicsNatives.hpp"
#include "HeadlessCompatNatives.hpp"
#include "ImageNatives.hpp"
#include "IONatives.hpp"
#include "Jdk8CompatNatives.hpp"
#include "LcduiNatives.hpp"
#include "MathNatives.hpp"
#include "M3gNatives.hpp"
#include "Micro3dNatives.hpp"
#include "MediaNatives.hpp"
#include "PushNatives.hpp"
#include "ReferenceNatives.hpp"
#include "RegexEngine.hpp"
#include "RmsNatives.hpp"
#include "SecurityNatives.hpp"
#include "StringEncodingNatives.hpp"
#include "TimeNatives.hpp"
#include "UtilNatives.hpp"
#include "WrapperNatives.hpp"
#include "XmlNatives.hpp"
#include "VendorNatives.hpp"
#include "SensorNatives.hpp"
#include "PimNatives.hpp"
#include "AmmsNatives.hpp"

namespace phoneme::vm {
namespace {

[[nodiscard]] Result<ObjectRef> require_receiver(
    std::span<const Value> arguments) {
    if (arguments.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "native instance method is missing its receiver");
    }
    auto reference = arguments.front().as_reference();
    if (!reference || reference->is_null()) {
        return fail(ErrorCode::invalid_argument,
                    "native instance method receiver is null");
    }
    return *reference;
}

[[nodiscard]] i32 stable_identity_hash(ObjectRef reference) noexcept {
    const u64 mixed = reference.bits ^ (reference.bits >> 33U) ^
                      (reference.bits << 11U);
    return static_cast<i32>(static_cast<u32>(mixed ^ (mixed >> 32U)));
}

[[nodiscard]] std::u16string ascii_text(std::string_view text) {
    std::u16string result(text.size(), u'\0');
    for (usize index = 0U; index < text.size(); ++index) {
        result[index] = static_cast<char16_t>(
            static_cast<unsigned char>(text[index]));
    }
    return result;
}

template <typename Number>
[[nodiscard]] Result<std::u16string> integral_text(Number value) {
    // to_chars overwrites exactly the returned range. Avoid clearing a cache
    // line for every Integer/Long.toString and String.valueOf call.
    std::array<char, 64> buffer;
    const auto converted = std::to_chars(buffer.data(),
                                         buffer.data() + buffer.size(),
                                         value);
    if (converted.ec != std::errc {}) {
        return fail(ErrorCode::internal_error,
                    "failed to format integral Java value");
    }
    return ascii_text(std::string_view(buffer.data(),
                                       static_cast<usize>(converted.ptr -
                                                          buffer.data())));
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

    std::array<char, 128> buffer;
    const int length = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%.*g",
        std::numeric_limits<Number>::max_digits10,
        static_cast<double>(value));
    if (length < 0 || static_cast<usize>(length) >= buffer.size()) {
        return fail(ErrorCode::internal_error,
                    "failed to format floating Java value");
    }
    std::string text(buffer.data(), static_cast<usize>(length));
    if (text.find_first_of(".eE") == std::string::npos) {
        text.append(".0");
    }
    return ascii_text(text);
}

[[nodiscard]] Result<ObjectRef> create_java_string(Machine& machine,
                                                   std::u16string text) {
    return machine.class_states().allocate_text_instance(
        machine.heap(), "java/lang/String", std::move(text));
}

std::atomic<u64> thread_name_sequence {0};

[[nodiscard]] Result<FieldLocation> thread_name_field(Machine& machine) {
    return machine.class_states().resolve_field(
        "java/lang/Thread", "name", "Ljava/lang/String;", false);
}

[[nodiscard]] Result<ObjectRef> make_default_thread_name(Machine& machine) {
    const u64 sequence =
        thread_name_sequence.fetch_add(1U, std::memory_order_relaxed) + 1U;
    auto suffix = integral_text(sequence);
    if (!suffix) {
        return std::unexpected(suffix.error());
    }
    std::u16string name(u"Thread-");
    name.append(*suffix);
    return create_java_string(machine, std::move(name));
}

[[nodiscard]] Status set_thread_name(Machine& machine,
                                     ObjectRef thread,
                                     ObjectRef name) {
    auto field = thread_name_field(machine);
    if (!field) {
        return std::unexpected(field.error());
    }
    return machine.heap().set_field(
        thread, field->index, Value::from_reference(name));
}

[[nodiscard]] Result<ObjectRef> get_thread_name(Machine& machine,
                                                ObjectRef thread) {
    auto field = thread_name_field(machine);
    if (!field) {
        return std::unexpected(field.error());
    }
    auto value = machine.heap().field(thread, field->index);
    if (!value) {
        return std::unexpected(value.error());
    }
    auto name = value->as_reference();
    if (!name) {
        return std::unexpected(name.error());
    }
    if (!name->is_null()) {
        return *name;
    }
    auto generated = make_default_thread_name(machine);
    if (!generated) {
        return std::unexpected(generated.error());
    }
    auto stored = set_thread_name(machine, thread, *generated);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    return *generated;
}

[[nodiscard]] Result<std::string> array_component_name(
    std::string_view descriptor) {
    if (descriptor.size() < 2U || descriptor.front() != '[') {
        return fail(ErrorCode::invalid_argument,
                    "object is not a Java array");
    }
    const std::string_view component = descriptor.substr(1);
    if (component.front() == '[') {
        return std::string(component);
    }
    if (component.front() == 'L' && component.size() >= 3U &&
        component.back() == ';') {
        return std::string(component.substr(1, component.size() - 2U));
    }
    if (component.size() == 1U &&
        std::string_view("ZCBSIFJD").find(component.front()) !=
            std::string_view::npos) {
        return std::string(component);
    }
    return fail(ErrorCode::malformed_class,
                "array contains an invalid component descriptor");
}

[[nodiscard]] Result<std::u16string> identity_object_text(
    Machine& machine,
    ObjectRef reference) {
    if (reference.is_null()) {
        return std::u16string(u"null");
    }
    auto class_name = machine.heap().class_name(reference);
    if (!class_name) {
        return std::unexpected(class_name.error());
    }

    std::string printable = *class_name;
    std::replace(printable.begin(), printable.end(), '/', '.');
    printable.push_back('@');
    std::array<char, 16> hash_buffer {};
    const auto hash = static_cast<u32>(stable_identity_hash(reference));
    const auto converted = std::to_chars(hash_buffer.data(),
                                         hash_buffer.data() + hash_buffer.size(),
                                         hash,
                                         16);
    if (converted.ec != std::errc {}) {
        return fail(ErrorCode::internal_error,
                    "failed to format Java identity hash");
    }
    printable.append(hash_buffer.data(), converted.ptr);
    return ascii_text(printable);
}

[[nodiscard]] Result<std::u16string> object_text(Machine& machine,
                                                 ObjectRef reference) {
    if (reference.is_null()) {
        return std::u16string(u"null");
    }
    auto class_name = machine.heap().class_name(reference);
    if (!class_name) {
        return std::unexpected(class_name.error());
    }
    if (*class_name == "java/lang/String" ||
        *class_name == "java/lang/StringBuilder" ||
        *class_name == "java/lang/StringBuffer") {
        return machine.heap().string_value(reference);
    }
    // Arrays inherit Object.toString and have no loadable class body of their
    // own in CLDC. Their Java representation is therefore the identity form.
    if (class_name->starts_with('[')) {
        return identity_object_text(machine, reference);
    }

    auto resolved = machine.classes().resolve_method(
        *class_name, "toString", "()Ljava/lang/String;");
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    if (resolved->owner == nullptr ||
        resolved->owner->name() == "java/lang/Object") {
        return identity_object_text(machine, reference);
    }

    auto invoked = machine.invoke_instance(
        reference,
        *class_name,
        "toString",
        "()Ljava/lang/String;",
        {});
    if (!invoked) {
        return std::unexpected(invoked.error());
    }
    if (invoked->throwable.has_value()) {
        auto throwable_class = machine.heap().class_name(*invoked->throwable);
        if (!throwable_class) {
            return std::unexpected(throwable_class.error());
        }
        return fail_java(*throwable_class,
                         "Object.toString() threw an exception");
    }
    if (!invoked->return_value.has_value()) {
        return fail(ErrorCode::internal_error,
                    "Object.toString() returned without a value");
    }
    auto text_reference = invoked->return_value->as_reference();
    if (!text_reference) {
        return std::unexpected(text_reference.error());
    }
    if (text_reference->is_null()) {
        return std::u16string(u"null");
    }
    auto text_class = machine.heap().class_name(*text_reference);
    if (!text_class) {
        return std::unexpected(text_class.error());
    }
    if (*text_class != "java/lang/String") {
        return fail(ErrorCode::invalid_state,
                    "Object.toString() returned a non-String object");
    }
    return machine.heap().string_value(*text_reference);
}

void add(NativeMethodRegistry& registry,
         std::string owner,
         std::string name,
         std::string descriptor,
         NativeMethod method,
         NativeJitPolicy jit_policy = NativeJitPolicy::conservative) {
    const std::string diagnostic_owner = owner;
    const std::string diagnostic_name = name;
    const std::string diagnostic_descriptor = descriptor;
    const auto registered = registry.register_method(std::move(owner),
                                                     std::move(name),
                                                     std::move(descriptor),
                                                     std::move(method),
                                                     jit_policy);
    if (!registered) {
        std::fprintf(stderr,
                     "failed to register native %s.%s%s: %s\n",
                     diagnostic_owner.c_str(), diagnostic_name.c_str(),
                     diagnostic_descriptor.c_str(),
                     registered.error().message.c_str());
        std::terminate();
    }
}

constexpr usize kBuilderCapacityField = 0U;

[[nodiscard]] Result<i32> builder_capacity(Machine& machine,
                                           ObjectRef builder) {
    auto value = machine.heap().field(builder, kBuilderCapacityField);
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

[[nodiscard]] Status set_builder_capacity(Machine& machine,
                                          ObjectRef builder,
                                          i32 capacity) {
    return machine.heap().set_field(
        builder, kBuilderCapacityField, Value::from_int(capacity));
}

[[nodiscard]] Status ensure_builder_capacity(Machine& machine,
                                             ObjectRef builder,
                                             usize required) {
    if (required > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return fail_java("java/lang/OutOfMemoryError",
                         "string builder capacity exceeds int range");
    }
    auto current = builder_capacity(machine, builder);
    if (!current) return std::unexpected(current.error());
    const usize current_capacity = static_cast<usize>(std::max(*current, 0));
    if (required <= current_capacity) return {};
    usize grown = current_capacity;
    if (grown <= (static_cast<usize>(std::numeric_limits<i32>::max()) - 2U) / 2U) {
        grown = grown * 2U + 2U;
    } else {
        grown = static_cast<usize>(std::numeric_limits<i32>::max());
    }
    grown = std::max(grown, required);
    return set_builder_capacity(machine, builder, static_cast<i32>(grown));
}

[[nodiscard]] Result<std::optional<Value>> append_builder_text(
    Machine& machine,
    std::span<const Value> arguments,
    std::u16string suffix) {
    auto receiver = require_receiver(arguments);
    if (!receiver) {
        return std::unexpected(receiver.error());
    }
    auto appended = machine.heap().append_mutable_text(
        *receiver, kBuilderCapacityField, suffix);
    if (!appended) {
        if (appended.error().code == ErrorCode::overflow) {
            return fail_java("java/lang/OutOfMemoryError",
                             "string builder append exceeds capacity");
        }
        return std::unexpected(appended.error());
    }
    return std::optional<Value>(Value::from_reference(*receiver));
}

[[nodiscard]] Result<std::optional<Value>> insert_builder_text(
    Machine& machine,
    std::span<const Value> arguments,
    std::u16string inserted) {
    auto receiver = require_receiver(arguments);
    auto offset = arguments[1].as_int();
    if (!receiver) return std::unexpected(receiver.error());
    if (!offset) return std::unexpected(offset.error());
    if (*offset < 0) {
        return fail_java("java/lang/StringIndexOutOfBoundsException",
                         "string builder insert offset is out of range");
    }
    auto stored = machine.heap().insert_mutable_text(
        *receiver, kBuilderCapacityField, static_cast<usize>(*offset), inserted);
    if (!stored) {
        if (stored.error().code == ErrorCode::out_of_range) {
            return fail_java("java/lang/StringIndexOutOfBoundsException",
                             "string builder insert offset is out of range");
        }
        if (stored.error().code == ErrorCode::overflow) {
            return fail_java("java/lang/OutOfMemoryError",
                             "string builder insert exceeds capacity");
        }
        return std::unexpected(stored.error());
    }
    return std::optional<Value>(Value::from_reference(*receiver));
}

[[nodiscard]] Result<std::u16string> char_array_slice(
    Machine& machine,
    ObjectRef array,
    i32 offset,
    i32 count);

void register_text_builder(NativeMethodRegistry& registry,
                           std::string class_name) {
    const std::string return_descriptor = "L" + class_name + ";";

    add(registry, class_name, "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto attached = machine.heap().attach_string(*receiver, {});
            if (!attached) {
                return std::unexpected(attached.error());
            }
            auto capacity = set_builder_capacity(machine, *receiver, 16);
            if (!capacity) return std::unexpected(capacity.error());
            return std::optional<Value> {};
        });
    add(registry, class_name, "<init>", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder capacity constructor expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto capacity = arguments[1].as_int();
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (!capacity) {
                return std::unexpected(capacity.error());
            }
            if (*capacity < 0) {
                return fail_java("java/lang/NegativeArraySizeException",
                                 "string builder capacity is negative");
            }
            auto attached = machine.heap().attach_string(*receiver, {});
            if (!attached) {
                return std::unexpected(attached.error());
            }
            auto stored_capacity = set_builder_capacity(
                machine, *receiver, *capacity);
            if (!stored_capacity) {
                return std::unexpected(stored_capacity.error());
            }
            return std::optional<Value> {};
        });
    add(registry, class_name, "<init>", "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder String constructor expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto source = arguments[1].as_reference();
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (!source || source->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "string builder String constructor received null");
            }
            auto text = machine.heap().string_value(*source);
            if (!text) {
                return std::unexpected(text.error());
            }
            const usize initial_capacity = text->size() + 16U;
            if (initial_capacity > static_cast<usize>(
                    std::numeric_limits<i32>::max())) {
                return fail_java("java/lang/OutOfMemoryError",
                                 "string builder capacity exceeds int range");
            }
            auto attached = machine.heap().attach_string(*receiver,
                                                         std::move(*text));
            if (!attached) {
                return std::unexpected(attached.error());
            }
            auto capacity = set_builder_capacity(
                machine, *receiver, static_cast<i32>(initial_capacity));
            if (!capacity) return std::unexpected(capacity.error());
            return std::optional<Value> {};
        });

    add(registry, class_name, "append",
        "(Ljava/lang/String;)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder append(String) expects one argument");
            }
            auto source = arguments[1].as_reference();
            if (!source) {
                return std::unexpected(source.error());
            }
            auto text = object_text(machine, *source);
            if (!text) {
                return std::unexpected(text.error());
            }
            return append_builder_text(machine, arguments, std::move(*text));
        });
    add(registry, class_name, "append",
        "(Ljava/lang/Object;)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder append(Object) expects one argument");
            }
            auto source = arguments[1].as_reference();
            if (!source) {
                return std::unexpected(source.error());
            }
            auto text = object_text(machine, *source);
            if (!text) {
                return std::unexpected(text.error());
            }
            return append_builder_text(machine, arguments, std::move(*text));
        });
    add(registry, class_name, "append",
        "(Ljava/lang/CharSequence;)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder append(CharSequence) expects one argument");
            }
            auto source = arguments[1].as_reference();
            if (!source) return std::unexpected(source.error());
            auto text = object_text(machine, *source);
            if (!text) return std::unexpected(text.error());
            return append_builder_text(machine, arguments, std::move(*text));
        });
    add(registry, class_name, "append",
        "(Ljava/lang/CharSequence;II)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 4U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder append(CharSequence,int,int) arguments are invalid");
            }
            auto source = arguments[1].as_reference();
            auto start = arguments[2].as_int();
            auto end = arguments[3].as_int();
            if (!source) return std::unexpected(source.error());
            if (!start) return std::unexpected(start.error());
            if (!end) return std::unexpected(end.error());
            auto text = object_text(machine, *source);
            if (!text) return std::unexpected(text.error());
            if (*start < 0 || *end < *start ||
                static_cast<usize>(*end) > text->size()) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "CharSequence append range is out of bounds");
            }
            std::u16string slice = text->substr(
                static_cast<usize>(*start),
                static_cast<usize>(*end - *start));
            return append_builder_text(machine, arguments, std::move(slice));
        });
    add(registry, class_name, "append", "(Z)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder append(boolean) expects one argument");
            }
            auto value = arguments[1].as_int();
            if (!value) {
                return std::unexpected(value.error());
            }
            return append_builder_text(machine, arguments,
                                       *value == 0 ? std::u16string(u"false")
                                                   : std::u16string(u"true"));
        });
    add(registry, class_name, "append", "(C)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder append(char) expects one argument");
            }
            auto value = arguments[1].as_int();
            if (!value) {
                return std::unexpected(value.error());
            }
            return append_builder_text(
                machine, arguments,
                std::u16string(1, static_cast<char16_t>(
                    static_cast<u16>(*value))));
        });
    add(registry, class_name, "append", "(I)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder append(int) expects one argument");
            }
            auto value = arguments[1].as_int();
            if (!value) {
                return std::unexpected(value.error());
            }
            auto text = integral_text(*value);
            if (!text) {
                return std::unexpected(text.error());
            }
            return append_builder_text(machine, arguments, std::move(*text));
        });
    add(registry, class_name, "append", "(J)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder append(long) expects one argument");
            }
            auto value = arguments[1].as_long();
            if (!value) {
                return std::unexpected(value.error());
            }
            auto text = integral_text(*value);
            if (!text) {
                return std::unexpected(text.error());
            }
            return append_builder_text(machine, arguments, std::move(*text));
        });
    add(registry, class_name, "append", "(F)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder append(float) expects one argument");
            }
            auto value = arguments[1].as_float();
            if (!value) {
                return std::unexpected(value.error());
            }
            auto text = floating_text(*value);
            if (!text) {
                return std::unexpected(text.error());
            }
            return append_builder_text(machine, arguments, std::move(*text));
        });
    add(registry, class_name, "append", "(D)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder append(double) expects one argument");
            }
            auto value = arguments[1].as_double();
            if (!value) {
                return std::unexpected(value.error());
            }
            auto text = floating_text(*value);
            if (!text) {
                return std::unexpected(text.error());
            }
            return append_builder_text(machine, arguments, std::move(*text));
        });
    add(registry, class_name, "append", "([C)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder append(char[]) expects one argument");
            }
            auto array = arguments[1].as_reference();
            if (!array) return std::unexpected(array.error());
            auto length = machine.heap().array_length(*array);
            if (!length) return std::unexpected(length.error());
            if (*length > static_cast<usize>(std::numeric_limits<i32>::max())) {
                return fail(ErrorCode::overflow,
                            "string builder char[] exceeds int range");
            }
            auto text = char_array_slice(
                machine, *array, 0, static_cast<i32>(*length));
            if (!text) return std::unexpected(text.error());
            return append_builder_text(machine, arguments, std::move(*text));
        });
    add(registry, class_name, "append", "([CII)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 4U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder append(char[],int,int) arguments are invalid");
            }
            auto array = arguments[1].as_reference();
            auto offset = arguments[2].as_int();
            auto length = arguments[3].as_int();
            if (!array) return std::unexpected(array.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            auto text = char_array_slice(
                machine, *array, *offset, *length);
            if (!text) return std::unexpected(text.error());
            return append_builder_text(machine, arguments, std::move(*text));
        });

    add(registry, class_name, "length", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto text = machine.heap().string_value(*receiver);
            if (!text) {
                return std::unexpected(text.error());
            }
            if (text->size() > static_cast<usize>(
                                   std::numeric_limits<i32>::max())) {
                return fail(ErrorCode::overflow,
                            "string builder length exceeds int range");
            }
            return std::optional<Value>(
                Value::from_int(static_cast<i32>(text->size())));
        });
    add(registry, class_name, "capacity", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) return std::unexpected(receiver.error());
            auto capacity = builder_capacity(machine, *receiver);
            if (!capacity) return std::unexpected(capacity.error());
            return std::optional<Value>(Value::from_int(*capacity));
        });
    add(registry, class_name, "charAt", "(I)C",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder charAt expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto index = arguments[1].as_int();
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (!index) {
                return std::unexpected(index.error());
            }
            auto text = machine.heap().string_value(*receiver);
            if (!text) {
                return std::unexpected(text.error());
            }
            if (*index < 0 || static_cast<usize>(*index) >= text->size()) {
                return fail_java("java/lang/StringIndexOutOfBoundsException",
                                 "string builder charAt index is out of range");
            }
            return std::optional<Value>(Value::from_int(static_cast<i32>(
                static_cast<u16>((*text)[static_cast<usize>(*index)]))));
        });
    add(registry, class_name, "subSequence",
        "(II)Ljava/lang/CharSequence;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 3U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder subSequence arguments are invalid");
            }
            auto receiver = require_receiver(arguments);
            auto begin = arguments[1].as_int();
            auto end = arguments[2].as_int();
            if (!receiver) return std::unexpected(receiver.error());
            if (!begin) return std::unexpected(begin.error());
            if (!end) return std::unexpected(end.error());
            auto text = machine.heap().string_value(*receiver);
            if (!text) return std::unexpected(text.error());
            if (*begin < 0 || *end < *begin ||
                static_cast<usize>(*end) > text->size()) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "string builder subSequence range is invalid");
            }
            auto result = create_java_string(
                machine,
                text->substr(static_cast<usize>(*begin),
                             static_cast<usize>(*end - *begin)));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, class_name, "setCharAt", "(IC)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 3U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder setCharAt arguments are invalid");
            }
            auto receiver = require_receiver(arguments);
            auto index = arguments[1].as_int();
            auto character = arguments[2].as_int();
            if (!receiver) return std::unexpected(receiver.error());
            if (!index) return std::unexpected(index.error());
            if (!character) return std::unexpected(character.error());
            auto text = machine.heap().string_value(*receiver);
            if (!text) return std::unexpected(text.error());
            if (*index < 0 || static_cast<usize>(*index) >= text->size()) {
                return fail_java("java/lang/StringIndexOutOfBoundsException",
                                 "string builder setCharAt index is out of range");
            }
            (*text)[static_cast<usize>(*index)] = static_cast<char16_t>(
                static_cast<u16>(*character));
            auto stored = machine.heap().attach_string(*receiver,
                                                       std::move(*text));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, class_name, "getChars", "(II[CI)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 5U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder getChars arguments are invalid");
            }
            auto receiver = require_receiver(arguments);
            auto source_begin = arguments[1].as_int();
            auto source_end = arguments[2].as_int();
            auto destination = arguments[3].as_reference();
            auto destination_begin = arguments[4].as_int();
            if (!receiver) return std::unexpected(receiver.error());
            if (!source_begin) return std::unexpected(source_begin.error());
            if (!source_end) return std::unexpected(source_end.error());
            if (!destination || destination->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "string builder getChars destination is null");
            }
            if (!destination_begin) {
                return std::unexpected(destination_begin.error());
            }
            auto text = machine.heap().string_value(*receiver);
            auto destination_class = machine.heap().class_name(*destination);
            auto destination_length = machine.heap().array_length(*destination);
            if (!text) return std::unexpected(text.error());
            if (!destination_class || *destination_class != "[C" ||
                !destination_length) {
                return fail_java("java/lang/ArrayStoreException",
                                 "string builder getChars expects char[]");
            }
            if (*source_begin < 0 || *source_end < *source_begin ||
                static_cast<usize>(*source_end) > text->size()) {
                return fail_java("java/lang/StringIndexOutOfBoundsException",
                                 "string builder getChars source range is invalid");
            }
            const i32 count = *source_end - *source_begin;
            if (*destination_begin < 0 ||
                static_cast<usize>(*destination_begin) > *destination_length ||
                static_cast<usize>(count) >
                    *destination_length - static_cast<usize>(*destination_begin)) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "string builder getChars destination range is invalid");
            }
            for (i32 index = 0; index < count; ++index) {
                const char16_t character = (*text)[static_cast<usize>(
                    *source_begin + index)];
                auto stored = machine.heap().set_element(
                    *destination,
                    static_cast<usize>(*destination_begin + index),
                    Value::from_int(static_cast<i32>(
                        static_cast<u16>(character))));
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
    add(registry, class_name, "delete", "(II)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 3U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder delete arguments are invalid");
            }
            auto receiver = require_receiver(arguments);
            auto start = arguments[1].as_int();
            auto end = arguments[2].as_int();
            if (!receiver) return std::unexpected(receiver.error());
            if (!start) return std::unexpected(start.error());
            if (!end) return std::unexpected(end.error());
            auto text = machine.heap().string_value(*receiver);
            if (!text) return std::unexpected(text.error());
            if (*start < 0 || *start > *end ||
                static_cast<usize>(*start) > text->size()) {
                return fail_java("java/lang/StringIndexOutOfBoundsException",
                                 "string builder delete range is invalid");
            }
            const usize first = static_cast<usize>(*start);
            const usize last = std::min(
                text->size(), static_cast<usize>(std::max(*end, 0)));
            text->erase(first, last - first);
            auto stored = machine.heap().attach_string(*receiver,
                                                       std::move(*text));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*receiver));
        });
    add(registry, class_name, "deleteCharAt", "(I)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder deleteCharAt expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto index = arguments[1].as_int();
            if (!receiver) return std::unexpected(receiver.error());
            if (!index) return std::unexpected(index.error());
            auto text = machine.heap().string_value(*receiver);
            if (!text) return std::unexpected(text.error());
            if (*index < 0 || static_cast<usize>(*index) >= text->size()) {
                return fail_java("java/lang/StringIndexOutOfBoundsException",
                                 "string builder deleteCharAt index is out of range");
            }
            text->erase(static_cast<usize>(*index), 1U);
            auto stored = machine.heap().attach_string(*receiver,
                                                       std::move(*text));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*receiver));
        });
    add(registry, class_name, "insert",
        "(ILjava/lang/String;)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 3U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder insert(String) arguments are invalid");
            }
            auto source = arguments[2].as_reference();
            if (!source) return std::unexpected(source.error());
            auto inserted = object_text(machine, *source);
            if (!inserted) return std::unexpected(inserted.error());
            return insert_builder_text(machine, arguments,
                                       std::move(*inserted));
        });
    add(registry, class_name, "insert", "(IC)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 3U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder insert(char) arguments are invalid");
            }
            auto character = arguments[2].as_int();
            if (!character) return std::unexpected(character.error());
            return insert_builder_text(
                machine, arguments,
                std::u16string(1, static_cast<char16_t>(
                    static_cast<u16>(*character))));
        });
    add(registry, class_name, "insert", "(II)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 3U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder insert(int) arguments are invalid");
            }
            auto value = arguments[2].as_int();
            if (!value) return std::unexpected(value.error());
            auto inserted = integral_text(*value);
            if (!inserted) return std::unexpected(inserted.error());
            return insert_builder_text(machine, arguments,
                                       std::move(*inserted));
        });
    add(registry, class_name, "insert",
        "(ILjava/lang/Object;)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto source = arguments[2].as_reference();
            if (!source) return std::unexpected(source.error());
            auto inserted = object_text(machine, *source);
            if (!inserted) return std::unexpected(inserted.error());
            return insert_builder_text(machine, arguments,
                                       std::move(*inserted));
        });
    add(registry, class_name, "insert", "(IZ)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[2].as_int();
            if (!value) return std::unexpected(value.error());
            return insert_builder_text(
                machine, arguments,
                *value == 0 ? std::u16string(u"false")
                            : std::u16string(u"true"));
        });
    add(registry, class_name, "insert", "(IJ)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[2].as_long();
            if (!value) return std::unexpected(value.error());
            auto inserted = integral_text(*value);
            if (!inserted) return std::unexpected(inserted.error());
            return insert_builder_text(machine, arguments,
                                       std::move(*inserted));
        });
    add(registry, class_name, "insert", "(IF)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[2].as_float();
            if (!value) return std::unexpected(value.error());
            auto inserted = floating_text(*value);
            if (!inserted) return std::unexpected(inserted.error());
            return insert_builder_text(machine, arguments,
                                       std::move(*inserted));
        });
    add(registry, class_name, "insert", "(ID)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[2].as_double();
            if (!value) return std::unexpected(value.error());
            auto inserted = floating_text(*value);
            if (!inserted) return std::unexpected(inserted.error());
            return insert_builder_text(machine, arguments,
                                       std::move(*inserted));
        });
    add(registry, class_name, "insert", "(I[C)" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto array = arguments[2].as_reference();
            if (!array) return std::unexpected(array.error());
            auto length = machine.heap().array_length(*array);
            if (!length) return std::unexpected(length.error());
            if (*length > static_cast<usize>(std::numeric_limits<i32>::max())) {
                return fail(ErrorCode::overflow,
                            "string builder char[] exceeds int range");
            }
            auto inserted = char_array_slice(
                machine, *array, 0, static_cast<i32>(*length));
            if (!inserted) return std::unexpected(inserted.error());
            return insert_builder_text(machine, arguments,
                                       std::move(*inserted));
        });
    add(registry, class_name, "reverse", "()" + return_descriptor,
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) return std::unexpected(receiver.error());
            auto text = machine.heap().string_value(*receiver);
            if (!text) return std::unexpected(text.error());
            std::reverse(text->begin(), text->end());
            for (usize index = 0; index + 1U < text->size(); ++index) {
                const u16 first = static_cast<u16>((*text)[index]);
                const u16 second = static_cast<u16>((*text)[index + 1U]);
                if (first >= 0xDC00U && first <= 0xDFFFU &&
                    second >= 0xD800U && second <= 0xDBFFU) {
                    std::swap((*text)[index], (*text)[index + 1U]);
                    ++index;
                }
            }
            auto stored = machine.heap().attach_string(*receiver,
                                                       std::move(*text));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*receiver));
        });
    add(registry, class_name, "ensureCapacity", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) return std::unexpected(receiver.error());
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder ensureCapacity expects one argument");
            }
            auto minimum = arguments[1].as_int();
            if (!minimum) return std::unexpected(minimum.error());
            if (*minimum > 0) {
                auto capacity = ensure_builder_capacity(
                    machine, *receiver, static_cast<usize>(*minimum));
                if (!capacity) return std::unexpected(capacity.error());
            }
            return std::optional<Value> {};
        });
    add(registry, class_name, "setLength", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "string builder setLength expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto length = arguments[1].as_int();
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (!length) {
                return std::unexpected(length.error());
            }
            if (*length < 0) {
                return fail_java("java/lang/StringIndexOutOfBoundsException",
                                 "string builder length is negative");
            }
            auto text = machine.heap().string_value(*receiver);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto capacity = ensure_builder_capacity(
                machine, *receiver, static_cast<usize>(*length));
            if (!capacity) return std::unexpected(capacity.error());
            text->resize(static_cast<usize>(*length), u'\0');
            auto stored = machine.heap().attach_string(*receiver,
                                                       std::move(*text));
            if (!stored) {
                return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
    add(registry, class_name, "toString", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto text = machine.heap().string_value(*receiver);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto string = create_java_string(machine, std::move(*text));
            if (!string) {
                return std::unexpected(string.error());
            }
            return std::optional<Value>(Value::from_reference(*string));
        });
}

[[nodiscard]] Result<std::u16string> char_array_slice(
    Machine& machine,
    ObjectRef array,
    i32 offset,
    i32 count) {
    if (array.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "character array is null");
    }
    auto class_name = machine.heap().class_name(array);
    if (!class_name || *class_name != "[C") {
        return fail_java("java/lang/IllegalArgumentException",
                         "value is not a char array");
    }
    auto length = machine.heap().array_length(array);
    if (!length) {
        return std::unexpected(length.error());
    }
    if (offset < 0 || count < 0 ||
        static_cast<usize>(offset) > *length ||
        static_cast<usize>(count) > *length - static_cast<usize>(offset)) {
        return fail_java("java/lang/StringIndexOutOfBoundsException",
                         "character array slice is out of range");
    }
    std::u16string result;
    result.reserve(static_cast<usize>(count));
    for (i32 index = 0; index < count; ++index) {
        auto value = machine.heap().element(
            array, static_cast<usize>(offset + index));
        if (!value) {
            return std::unexpected(value.error());
        }
        auto character = value->as_int();
        if (!character) {
            return std::unexpected(character.error());
        }
        result.push_back(static_cast<char16_t>(
            static_cast<u16>(*character)));
    }
    return result;
}

[[nodiscard]] char16_t simple_case_fold(char16_t value) noexcept {
    if (value >= u'A' && value <= u'Z') {
        return static_cast<char16_t>(value + (u'a' - u'A'));
    }
    return value;
}

[[nodiscard]] char16_t simple_case_upper(char16_t value) noexcept {
    if (value >= u'a' && value <= u'z') {
        return static_cast<char16_t>(value - (u'a' - u'A'));
    }
    return value;
}

void register_string_extensions(NativeMethodRegistry& registry) {
    add(registry, "java/lang/String", "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto attached = machine.heap().attach_string(*receiver, {});
            if (!attached) {
                return std::unexpected(attached.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "java/lang/String", "<init>",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "String copy constructor expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto source = arguments[1].as_reference();
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (!source || source->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String copy source is null");
            }
            auto text = machine.heap().string_value(*source);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto attached = machine.heap().attach_string(*receiver,
                                                         std::move(*text));
            if (!attached) {
                return std::unexpected(attached.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "java/lang/String", "<init>",
        "(Ljava/lang/StringBuffer;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "String buffer constructor expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto source = arguments[1].as_reference();
            if (!receiver) return std::unexpected(receiver.error());
            if (!source || source->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String buffer source is null");
            }
            auto source_class = machine.heap().class_name(*source);
            if (!source_class) return std::unexpected(source_class.error());
            if (*source_class != "java/lang/StringBuffer") {
                return fail_java("java/lang/IllegalArgumentException",
                                 "String buffer source has the wrong class");
            }
            auto text = machine.heap().string_value(*source);
            if (!text) return std::unexpected(text.error());
            auto attached = machine.heap().attach_string(*receiver,
                                                         std::move(*text));
            if (!attached) return std::unexpected(attached.error());
            return std::optional<Value> {};
        });
    add(registry, "java/lang/String", "<init>", "([C)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "String char-array constructor expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto array = arguments[1].as_reference();
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (!array) {
                return std::unexpected(array.error());
            }
            auto length = machine.heap().array_length(*array);
            if (!length) {
                return std::unexpected(length.error());
            }
            if (*length > static_cast<usize>(std::numeric_limits<i32>::max())) {
                return fail_java("java/lang/OutOfMemoryError",
                                 "String char array is too large");
            }
            auto text = char_array_slice(machine, *array, 0,
                                         static_cast<i32>(*length));
            if (!text) {
                return std::unexpected(text.error());
            }
            auto attached = machine.heap().attach_string(*receiver,
                                                         std::move(*text));
            if (!attached) {
                return std::unexpected(attached.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "java/lang/String", "<init>", "([CII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 4U) {
                return fail(ErrorCode::invalid_argument,
                            "String char-array slice constructor expects three arguments");
            }
            auto receiver = require_receiver(arguments);
            auto array = arguments[1].as_reference();
            auto offset = arguments[2].as_int();
            auto count = arguments[3].as_int();
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (!array || !offset || !count) {
                return fail(ErrorCode::invalid_argument,
                            "String char-array slice arguments are invalid");
            }
            auto text = char_array_slice(machine, *array, *offset, *count);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto attached = machine.heap().attach_string(*receiver,
                                                         std::move(*text));
            if (!attached) {
                return std::unexpected(attached.error());
            }
            return std::optional<Value> {};
        });

    add(registry, "java/lang/String", "getChars", "(II[CI)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 5U) {
                return fail(ErrorCode::invalid_argument,
                            "String.getChars expects four arguments");
            }
            auto receiver = require_receiver(arguments);
            auto begin = arguments[1].as_int();
            auto end = arguments[2].as_int();
            auto destination = arguments[3].as_reference();
            auto destination_begin = arguments[4].as_int();
            if (!receiver || !begin || !end || !destination ||
                !destination_begin) {
                return fail(ErrorCode::invalid_argument,
                            "String.getChars arguments are invalid");
            }
            if (destination->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String.getChars destination is null");
            }
            auto text = machine.heap().string_value(*receiver);
            auto destination_class = machine.heap().class_name(*destination);
            auto destination_length = machine.heap().array_length(*destination);
            if (!text) {
                return std::unexpected(text.error());
            }
            if (!destination_class || *destination_class != "[C" ||
                !destination_length) {
                return fail_java("java/lang/ArrayStoreException",
                                 "String.getChars destination is not char[]");
            }
            if (*begin < 0 || *end < *begin ||
                static_cast<usize>(*end) > text->size() ||
                *destination_begin < 0) {
                return fail_java("java/lang/StringIndexOutOfBoundsException",
                                 "String.getChars source range is invalid");
            }
            const usize count = static_cast<usize>(*end - *begin);
            const usize destination_offset =
                static_cast<usize>(*destination_begin);
            if (destination_offset > *destination_length ||
                count > *destination_length - destination_offset) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "String.getChars destination range is invalid");
            }
            for (usize index = 0; index < count; ++index) {
                auto stored = machine.heap().set_element(
                    *destination,
                    destination_offset + index,
                    Value::from_int(static_cast<i32>(static_cast<u16>(
                        (*text)[static_cast<usize>(*begin) + index]))));
                if (!stored) {
                    return std::unexpected(stored.error());
                }
            }
            return std::optional<Value> {};
        });
    add(registry, "java/lang/String", "toCharArray", "()[C",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto text = machine.heap().string_value(*receiver);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto array = machine.heap().allocate_array(
                "[C", text->size(), Value::from_int(0));
            if (!array) {
                return std::unexpected(array.error());
            }
            for (usize index = 0; index < text->size(); ++index) {
                auto stored = machine.heap().set_element(
                    *array, index,
                    Value::from_int(static_cast<i32>(
                        static_cast<u16>((*text)[index]))));
                if (!stored) {
                    return std::unexpected(stored.error());
                }
            }
            return std::optional<Value>(Value::from_reference(*array));
        });

    add(registry, "java/lang/String", "equalsIgnoreCase",
        "(Ljava/lang/String;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "String.equalsIgnoreCase expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto other = arguments[1].as_reference();
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (!other || other->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto left = machine.heap().string_value(*receiver);
            auto right = machine.heap().string_value(*other);
            if (!left || !right) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "String.equalsIgnoreCase argument is not String");
            }
            bool equal = left->size() == right->size();
            for (usize index = 0; equal && index < left->size(); ++index) {
                equal = simple_case_fold((*left)[index]) ==
                        simple_case_fold((*right)[index]);
            }
            return std::optional<Value>(Value::from_int(equal ? 1 : 0));
        });
    add(registry, "java/lang/String", "regionMatches",
        "(ZILjava/lang/String;II)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 6U) {
                return fail(ErrorCode::invalid_argument,
                            "String.regionMatches arguments are invalid");
            }
            auto receiver = require_receiver(arguments);
            auto ignore_case = arguments[1].as_int();
            auto receiver_offset = arguments[2].as_int();
            auto other = arguments[3].as_reference();
            auto other_offset = arguments[4].as_int();
            auto length = arguments[5].as_int();
            if (!receiver) return std::unexpected(receiver.error());
            if (!ignore_case) return std::unexpected(ignore_case.error());
            if (!receiver_offset) {
                return std::unexpected(receiver_offset.error());
            }
            if (!other || other->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String.regionMatches argument is null");
            }
            if (!other_offset) return std::unexpected(other_offset.error());
            if (!length) return std::unexpected(length.error());
            auto left = machine.heap().string_value(*receiver);
            auto right = machine.heap().string_value(*other);
            if (!left || !right) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "String.regionMatches argument is not String");
            }
            if (*receiver_offset < 0 || *other_offset < 0 || *length < 0 ||
                static_cast<usize>(*receiver_offset) > left->size() ||
                static_cast<usize>(*other_offset) > right->size() ||
                static_cast<usize>(*length) >
                    left->size() - static_cast<usize>(*receiver_offset) ||
                static_cast<usize>(*length) >
                    right->size() - static_cast<usize>(*other_offset)) {
                return std::optional<Value>(Value::from_int(0));
            }
            bool equal = true;
            for (i32 index = 0; equal && index < *length; ++index) {
                char16_t left_character = (*left)[static_cast<usize>(
                    *receiver_offset + index)];
                char16_t right_character = (*right)[static_cast<usize>(
                    *other_offset + index)];
                if (*ignore_case != 0) {
                    left_character = simple_case_fold(left_character);
                    right_character = simple_case_fold(right_character);
                }
                equal = left_character == right_character;
            }
            return std::optional<Value>(Value::from_int(equal ? 1 : 0));
        });
    add(registry, "java/lang/String", "compareTo",
        "(Ljava/lang/String;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "String.compareTo expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto other = arguments[1].as_reference();
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (!other || other->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String.compareTo argument is null");
            }
            auto left = machine.heap().string_value(*receiver);
            auto right = machine.heap().string_value(*other);
            if (!left || !right) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "String.compareTo argument is not String");
            }
            const usize common = std::min(left->size(), right->size());
            for (usize index = 0; index < common; ++index) {
                if ((*left)[index] != (*right)[index]) {
                    return std::optional<Value>(Value::from_int(
                        static_cast<i32>(static_cast<u16>((*left)[index])) -
                        static_cast<i32>(static_cast<u16>((*right)[index]))));
                }
            }
            const auto difference = static_cast<i64>(left->size()) -
                                    static_cast<i64>(right->size());
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(difference)));
        });

    const auto register_prefix = [&registry](bool with_offset) {
        add(registry, "java/lang/String", "startsWith",
            with_offset ? "(Ljava/lang/String;I)Z"
                        : "(Ljava/lang/String;)Z",
            [with_offset](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                const usize expected = with_offset ? 3U : 2U;
                if (arguments.size() != expected) {
                    return fail(ErrorCode::invalid_argument,
                                "String.startsWith argument count is invalid");
                }
                auto receiver = require_receiver(arguments);
                auto prefix = arguments[1].as_reference();
                i32 offset = 0;
                if (with_offset) {
                    auto parsed_offset = arguments[2].as_int();
                    if (!parsed_offset) {
                        return std::unexpected(parsed_offset.error());
                    }
                    offset = *parsed_offset;
                }
                if (!receiver) {
                    return std::unexpected(receiver.error());
                }
                if (!prefix || prefix->is_null()) {
                    return fail_java("java/lang/NullPointerException",
                                     "String.startsWith prefix is null");
                }
                auto text = machine.heap().string_value(*receiver);
                auto prefix_text = machine.heap().string_value(*prefix);
                if (!text || !prefix_text) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "String.startsWith prefix is not String");
                }
                const bool matches = offset >= 0 &&
                    static_cast<usize>(offset) <= text->size() &&
                    prefix_text->size() <=
                        text->size() - static_cast<usize>(offset) &&
                    std::equal(prefix_text->begin(), prefix_text->end(),
                               text->begin() + offset);
                return std::optional<Value>(Value::from_int(matches ? 1 : 0));
            });
    };
    register_prefix(false);
    register_prefix(true);
    add(registry, "java/lang/String", "endsWith",
        "(Ljava/lang/String;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "String.endsWith expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto suffix = arguments[1].as_reference();
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (!suffix || suffix->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String.endsWith suffix is null");
            }
            auto text = machine.heap().string_value(*receiver);
            auto suffix_text = machine.heap().string_value(*suffix);
            if (!text || !suffix_text) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "String.endsWith suffix is not String");
            }
            const bool matches = suffix_text->size() <= text->size() &&
                std::equal(suffix_text->begin(), suffix_text->end(),
                           text->end() -
                               static_cast<std::ptrdiff_t>(suffix_text->size()));
            return std::optional<Value>(Value::from_int(matches ? 1 : 0));
        });

    const auto register_char_search = [&registry](bool reverse,
                                                  bool with_offset) {
        const std::string name = reverse ? "lastIndexOf" : "indexOf";
        add(registry, "java/lang/String", name,
            with_offset ? "(II)I" : "(I)I",
            [reverse, with_offset](Machine& machine,
                                   std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto receiver = require_receiver(arguments);
                auto character = arguments[1].as_int();
                if (!receiver) {
                    return std::unexpected(receiver.error());
                }
                if (!character) {
                    return std::unexpected(character.error());
                }
                // The forward, no-offset form is a hot bitmap-font primitive
                // in many MIDP games. Avoid copying the complete UTF-16 payload
                // for each glyph lookup.
                if (!reverse && !with_offset) {
                    auto position = machine.heap().string_index_of(
                        *receiver, static_cast<u16>(*character));
                    if (!position) return std::unexpected(position.error());
                    return std::optional<Value>(Value::from_int(*position));
                }
                auto text = machine.heap().string_value(*receiver);
                if (!text) return std::unexpected(text.error());
                i32 from = reverse
                    ? static_cast<i32>(std::min<usize>(
                          text->size(),
                          static_cast<usize>(
                              std::numeric_limits<i32>::max()))) - 1
                    : 0;
                if (with_offset) {
                    auto parsed = arguments[2].as_int();
                    if (!parsed) {
                        return std::unexpected(parsed.error());
                    }
                    from = *parsed;
                }
                const char16_t target = static_cast<char16_t>(
                    static_cast<u16>(*character));
                usize position = std::u16string::npos;
                if (reverse) {
                    if (from >= 0 && !text->empty()) {
                        const usize start = std::min(
                            static_cast<usize>(from), text->size() - 1U);
                        position = text->rfind(target, start);
                    }
                } else {
                    const usize start = from <= 0 ? 0U
                        : static_cast<usize>(from);
                    if (start <= text->size()) {
                        position = text->find(target, start);
                    }
                }
                return std::optional<Value>(Value::from_int(
                    position == std::u16string::npos
                        ? -1
                        : static_cast<i32>(position)));
            },
            !reverse && !with_offset
                ? NativeJitPolicy::synchronous_bounded
                : NativeJitPolicy::conservative);
    };
    register_char_search(false, false);
    register_char_search(false, true);
    register_char_search(true, false);
    register_char_search(true, true);

    const auto register_string_search = [&registry](bool reverse,
                                                    bool with_offset) {
        const std::string name = reverse ? "lastIndexOf" : "indexOf";
        add(registry, "java/lang/String", name,
            with_offset ? "(Ljava/lang/String;I)I"
                        : "(Ljava/lang/String;)I",
            [reverse, with_offset](Machine& machine,
                                   std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto receiver = require_receiver(arguments);
                auto needle = arguments[1].as_reference();
                if (!receiver) {
                    return std::unexpected(receiver.error());
                }
                if (!needle || needle->is_null()) {
                    return fail_java("java/lang/NullPointerException",
                                     "String search argument is null");
                }
                auto text = machine.heap().string_value(*receiver);
                auto needle_text = machine.heap().string_value(*needle);
                if (!text || !needle_text) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "String search argument is not String");
                }
                i32 from = reverse
                    ? static_cast<i32>(std::min<usize>(
                          text->size(),
                          static_cast<usize>(
                              std::numeric_limits<i32>::max())))
                    : 0;
                if (with_offset) {
                    auto parsed = arguments[2].as_int();
                    if (!parsed) {
                        return std::unexpected(parsed.error());
                    }
                    from = *parsed;
                }
                usize position = std::u16string::npos;
                if (reverse) {
                    if (from >= 0) {
                        position = text->rfind(
                            *needle_text,
                            std::min(static_cast<usize>(from), text->size()));
                    }
                } else {
                    const usize start = from <= 0 ? 0U
                        : static_cast<usize>(from);
                    if (start <= text->size()) {
                        position = text->find(*needle_text, start);
                    }
                }
                return std::optional<Value>(Value::from_int(
                    position == std::u16string::npos
                        ? -1
                        : static_cast<i32>(position)));
            });
    };
    register_string_search(false, false);
    register_string_search(false, true);
    register_string_search(true, false);
    register_string_search(true, true);

    const auto register_substring = [&registry](bool with_end) {
        add(registry, "java/lang/String", "substring",
            with_end ? "(II)Ljava/lang/String;"
                     : "(I)Ljava/lang/String;",
            [with_end](Machine& machine,
                       std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto receiver = require_receiver(arguments);
                auto begin = arguments[1].as_int();
                if (!receiver) {
                    return std::unexpected(receiver.error());
                }
                if (!begin) {
                    return std::unexpected(begin.error());
                }
                auto text = machine.heap().string_value(*receiver);
                if (!text) {
                    return std::unexpected(text.error());
                }
                i32 end = static_cast<i32>(text->size());
                if (with_end) {
                    auto parsed_end = arguments[2].as_int();
                    if (!parsed_end) {
                        return std::unexpected(parsed_end.error());
                    }
                    end = *parsed_end;
                }
                if (*begin < 0 || end < *begin ||
                    static_cast<usize>(end) > text->size()) {
                    return fail_java(
                        "java/lang/StringIndexOutOfBoundsException",
                        "String.substring range is invalid: begin=" +
                            std::to_string(*begin) + " end=" +
                            std::to_string(end) + " length=" +
                            std::to_string(text->size()));
                }
                auto result = create_java_string(
                    machine,
                    text->substr(static_cast<usize>(*begin),
                                 static_cast<usize>(end - *begin)));
                if (!result) {
                    return std::unexpected(result.error());
                }
                return std::optional<Value>(Value::from_reference(*result));
            });
    };
    register_substring(false);
    register_substring(true);
    add(registry, "java/lang/String", "subSequence",
        "(II)Ljava/lang/CharSequence;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            auto begin = arguments[1].as_int();
            auto end = arguments[2].as_int();
            if (!receiver) return std::unexpected(receiver.error());
            if (!begin) return std::unexpected(begin.error());
            if (!end) return std::unexpected(end.error());
            auto text = machine.heap().string_value(*receiver);
            if (!text) return std::unexpected(text.error());
            if (*begin < 0 || *end < *begin ||
                static_cast<usize>(*end) > text->size()) {
                return fail_java("java/lang/StringIndexOutOfBoundsException",
                                 "String.subSequence range is invalid");
            }
            auto result = create_java_string(
                machine,
                text->substr(static_cast<usize>(*begin),
                             static_cast<usize>(*end - *begin)));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });

    add(registry, "java/lang/String", "concat",
        "(Ljava/lang/String;)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            auto other = arguments[1].as_reference();
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (!other || other->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String.concat argument is null");
            }
            auto left = machine.heap().string_value(*receiver);
            auto right = machine.heap().string_value(*other);
            if (!left || !right) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "String.concat argument is not String");
            }
            left->append(*right);
            auto result = create_java_string(machine, std::move(*left));
            if (!result) {
                return std::unexpected(result.error());
            }
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/lang/String", "contains",
        "(Ljava/lang/CharSequence;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            auto sequence = arguments[1].as_reference();
            if (!receiver) return std::unexpected(receiver.error());
            if (!sequence || sequence->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String.contains argument is null");
            }
            auto text = machine.heap().string_value(*receiver);
            auto target = machine.heap().string_value(*sequence);
            if (!text || !target) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "String.contains requires String CharSequence");
            }
            return std::optional<Value>(Value::from_int(
                text->find(*target) != std::u16string::npos ? 1 : 0));
        });
    add(registry, "java/lang/String", "replace",
        "(CC)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            auto old_character = arguments[1].as_int();
            auto new_character = arguments[2].as_int();
            if (!receiver || !old_character || !new_character) {
                return fail(ErrorCode::invalid_argument,
                            "String.replace arguments are invalid");
            }
            auto text = machine.heap().string_value(*receiver);
            if (!text) {
                return std::unexpected(text.error());
            }
            std::replace(text->begin(), text->end(),
                         static_cast<char16_t>(static_cast<u16>(*old_character)),
                         static_cast<char16_t>(static_cast<u16>(*new_character)));
            auto result = create_java_string(machine, std::move(*text));
            if (!result) {
                return std::unexpected(result.error());
            }
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/lang/String", "replace",
        "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            auto target_reference = arguments[1].as_reference();
            auto replacement_reference = arguments[2].as_reference();
            if (!receiver) return std::unexpected(receiver.error());
            if (!target_reference || target_reference->is_null() ||
                !replacement_reference || replacement_reference->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String.replace sequence is null");
            }
            auto text = machine.heap().string_value(*receiver);
            auto target = machine.heap().string_value(*target_reference);
            auto replacement = machine.heap().string_value(*replacement_reference);
            if (!text || !target || !replacement) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "String.replace requires String sequences");
            }
            if (target->empty()) {
                std::u16string expanded;
                expanded.reserve(text->size() +
                                 (text->size() + 1U) * replacement->size());
                expanded.append(*replacement);
                for (const char16_t character : *text) {
                    expanded.push_back(character);
                    expanded.append(*replacement);
                }
                text = std::move(expanded);
            } else {
                usize cursor = 0U;
                while ((cursor = text->find(*target, cursor)) !=
                       std::u16string::npos) {
                    text->replace(cursor, target->size(), *replacement);
                    cursor += replacement->size();
                }
            }
            auto result = create_java_string(machine, std::move(*text));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/lang/String", "replaceAll",
        "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            auto regex_reference = arguments[1].as_reference();
            auto replacement_reference = arguments[2].as_reference();
            if (!receiver) return std::unexpected(receiver.error());
            if (!regex_reference || regex_reference->is_null() ||
                !replacement_reference || replacement_reference->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String.replaceAll argument is null");
            }
            auto text = machine.heap().string_value(*receiver);
            auto regex = machine.heap().string_value(*regex_reference);
            auto replacement = machine.heap().string_value(*replacement_reference);
            if (!text || !regex || !replacement) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "String.replaceAll arguments are invalid");
            }
            auto output = replace_all_java_regex(*regex, *text, *replacement);
            if (!output) return std::unexpected(output.error());
            auto result = create_java_string(machine, std::move(*output));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/lang/String", "trim", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto text = machine.heap().string_value(*receiver);
            if (!text) {
                return std::unexpected(text.error());
            }
            usize begin = 0;
            usize end = text->size();
            while (begin < end && (*text)[begin] <= u' ') {
                ++begin;
            }
            while (end > begin && (*text)[end - 1U] <= u' ') {
                --end;
            }
            auto result = create_java_string(
                machine, text->substr(begin, end - begin));
            if (!result) {
                return std::unexpected(result.error());
            }
            return std::optional<Value>(Value::from_reference(*result));
        });

    const auto register_string_case = [&registry](
        const char* name,
        char16_t (*convert)(char16_t) noexcept) {
        const auto implementation = [convert](
            Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) return std::unexpected(receiver.error());
            auto text = machine.heap().string_value(*receiver);
            if (!text) return std::unexpected(text.error());

            bool changed = false;
            for (char16_t& character : *text) {
                const char16_t converted = convert(character);
                changed = changed || converted != character;
                character = converted;
            }
            if (!changed) {
                return std::optional<Value>(
                    Value::from_reference(*receiver));
            }
            auto result = create_java_string(machine, std::move(*text));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        };
        add(registry, "java/lang/String", name,
            "()Ljava/lang/String;", implementation);
        add(registry, "java/lang/String", name,
            "(Ljava/util/Locale;)Ljava/lang/String;", implementation);
    };
    register_string_case("toLowerCase", simple_case_fold);
    register_string_case("toUpperCase", simple_case_upper);

    add(registry, "java/lang/String", "split",
        "(Ljava/lang/String;)[Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "String.split expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto regex = arguments[1].as_reference();
            if (!receiver) return std::unexpected(receiver.error());
            if (!regex || regex->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String.split pattern is null");
            }
            auto text = machine.heap().string_value(*receiver);
            auto regex_text = machine.heap().string_value(*regex);
            if (!text || !regex_text) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "String.split pattern is not String");
            }

            auto parts = split_java_regex(*regex_text, *text);
            if (!parts) return std::unexpected(parts.error());
            auto array = machine.heap().allocate_array(
                "[Ljava/lang/String;", parts->size(),
                Value::from_reference({}));
            if (!array) return std::unexpected(array.error());
            auto array_root = machine.pin_native_root(*array);
            if (!array_root) return std::unexpected(array_root.error());
            for (usize index = 0; index < parts->size(); ++index) {
                auto value = create_java_string(machine,
                                                std::move((*parts)[index]));
                if (!value) return std::unexpected(value.error());
                auto stored = machine.heap().set_element(
                    *array, index, Value::from_reference(*value));
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value>(Value::from_reference(*array));
        });

    add(registry, "java/lang/String", "split",
        "(Ljava/lang/String;I)[Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 3U) {
                return fail(ErrorCode::invalid_argument,
                            "String.split expects pattern and limit");
            }
            auto receiver = require_receiver(arguments);
            auto regex = arguments[1].as_reference();
            auto limit = arguments[2].as_int();
            if (!receiver) return std::unexpected(receiver.error());
            if (!limit) return std::unexpected(limit.error());
            if (!regex || regex->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String.split pattern is null");
            }
            auto text = machine.heap().string_value(*receiver);
            auto regex_text = machine.heap().string_value(*regex);
            if (!text || !regex_text) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "String.split pattern is not String");
            }

            auto parts = split_java_regex(*regex_text, *text, *limit);
            if (!parts) return std::unexpected(parts.error());
            auto array = machine.heap().allocate_array(
                "[Ljava/lang/String;", parts->size(),
                Value::from_reference({}));
            if (!array) return std::unexpected(array.error());
            auto array_root = machine.pin_native_root(*array);
            if (!array_root) return std::unexpected(array_root.error());
            for (usize index = 0; index < parts->size(); ++index) {
                auto value = create_java_string(machine,
                                                std::move((*parts)[index]));
                if (!value) return std::unexpected(value.error());
                auto stored = machine.heap().set_element(
                    *array, index, Value::from_reference(*value));
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value>(Value::from_reference(*array));
        });

    add(registry, "java/lang/String", "matches", "(Ljava/lang/String;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "String.matches expects one argument");
            }
            auto receiver = require_receiver(arguments);
            auto regex_reference = arguments[1].as_reference();
            if (!receiver) return std::unexpected(receiver.error());
            if (!regex_reference || regex_reference->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String.matches pattern is null");
            }
            auto text = machine.heap().string_value(*receiver);
            auto pattern_text = machine.heap().string_value(*regex_reference);
            if (!text || !pattern_text) {
                return fail(ErrorCode::invalid_argument,
                            "String.matches arguments are invalid");
            }
            auto match = match_java_regex(*pattern_text, *text);
            if (!match) return std::unexpected(match.error());
            return std::optional<Value>(Value::from_int(
                match->has_value() ? 1 : 0));
        });

    add(registry, "java/lang/String", "format",
        "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "String.format expects format and arguments");
            }
            auto format_reference = arguments[0].as_reference();
            auto values_reference = arguments[1].as_reference();
            if (!format_reference || format_reference->is_null() ||
                !values_reference || values_reference->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "String.format argument is null");
            }
            auto format = machine.heap().string_value(*format_reference);
            auto value_count = machine.heap().array_length(*values_reference);
            if (!format || !value_count) {
                return fail(ErrorCode::invalid_argument,
                            "String.format arguments are invalid");
            }

            std::u16string output;
            output.reserve(format->size() + 16U);
            usize argument_index = 0U;
            for (usize index = 0U; index < format->size(); ++index) {
                const char16_t current = (*format)[index];
                if (current != u'%') {
                    output.push_back(current);
                    continue;
                }
                if (index + 1U < format->size() &&
                    (*format)[index + 1U] == u'%') {
                    output.push_back(u'%');
                    ++index;
                    continue;
                }

                bool zero_pad = false;
                i32 width = 0;
                usize specifier_index = index + 1U;
                if (specifier_index < format->size() &&
                    (*format)[specifier_index] == u'0') {
                    zero_pad = true;
                    ++specifier_index;
                }
                while (specifier_index < format->size() &&
                       (*format)[specifier_index] >= u'0' &&
                       (*format)[specifier_index] <= u'9') {
                    width = width * 10 +
                            static_cast<i32>((*format)[specifier_index] - u'0');
                    ++specifier_index;
                }
                if (specifier_index >= format->size()) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "String.format has an incomplete conversion");
                }
                const char16_t conversion = (*format)[specifier_index];
                if (argument_index >= *value_count) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "String.format has too few arguments");
                }
                auto argument_value = machine.heap().element(
                    *values_reference, argument_index++);
                if (!argument_value) {
                    return std::unexpected(argument_value.error());
                }
                auto argument_reference = argument_value->as_reference();
                if (!argument_reference) {
                    return std::unexpected(argument_reference.error());
                }

                std::u16string inserted;
                if (conversion == u's' || conversion == u'S') {
                    auto text = object_text(machine, *argument_reference);
                    if (!text) return std::unexpected(text.error());
                    inserted = std::move(*text);
                    if (conversion == u'S') {
                        for (char16_t& character : inserted) {
                            character = simple_case_upper(character);
                        }
                    }
                } else if (conversion == u'd' || conversion == u'x' ||
                           conversion == u'X') {
                    if (argument_reference->is_null()) {
                        inserted = u"null";
                    } else {
                        auto class_name = machine.heap().class_name(
                            *argument_reference);
                        if (!class_name) {
                            return std::unexpected(class_name.error());
                        }
                        auto field = machine.heap().field(*argument_reference, 0U);
                        if (!field) return std::unexpected(field.error());
                        i64 number = 0;
                        if (*class_name == "java/lang/Long") {
                            auto value = field->as_long();
                            if (!value) return std::unexpected(value.error());
                            number = *value;
                        } else {
                            auto value = field->as_int();
                            if (!value) return std::unexpected(value.error());
                            number = *value;
                        }

                        std::array<char, 64> buffer {};
                        const int base = conversion == u'd' ? 10 : 16;
                        const auto converted = std::to_chars(
                            buffer.data(), buffer.data() + buffer.size(),
                            number, base);
                        if (converted.ec != std::errc {}) {
                            return fail(ErrorCode::internal_error,
                                        "String.format number conversion failed");
                        }
                        inserted = ascii_text(std::string_view(
                            buffer.data(), static_cast<usize>(
                                               converted.ptr - buffer.data())));
                        if (conversion == u'X') {
                            for (char16_t& character : inserted) {
                                if (character >= u'a' && character <= u'f') {
                                    character = static_cast<char16_t>(
                                        character - (u'a' - u'A'));
                                }
                            }
                        }
                    }
                } else {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "String.format conversion is unsupported");
                }

                if (width > 0 && inserted.size() < static_cast<usize>(width)) {
                    const usize padding = static_cast<usize>(width) -
                                          inserted.size();
                    const char16_t fill = zero_pad ? u'0' : u' ';
                    if (zero_pad && !inserted.empty() &&
                        inserted.front() == u'-') {
                        output.push_back(u'-');
                        output.append(padding, fill);
                        output.append(inserted.substr(1U));
                    } else {
                        output.append(padding, fill);
                        output.append(inserted);
                    }
                } else {
                    output.append(inserted);
                }
                index = specifier_index;
            }

            auto result = create_java_string(machine, std::move(output));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });

    add(registry, "java/lang/String", "valueOf",
        "(Ljava/lang/Object;)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto reference = arguments[0].as_reference();
            if (!reference) {
                return std::unexpected(reference.error());
            }
            auto text = object_text(machine, *reference);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto result = create_java_string(machine, std::move(*text));
            if (!result) {
                return std::unexpected(result.error());
            }
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/lang/String", "valueOf", "([C)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto array = arguments[0].as_reference();
            if (!array) {
                return std::unexpected(array.error());
            }
            auto length = machine.heap().array_length(*array);
            if (!length) {
                return std::unexpected(length.error());
            }
            auto text = char_array_slice(machine, *array, 0,
                                         static_cast<i32>(*length));
            if (!text) {
                return std::unexpected(text.error());
            }
            auto result = create_java_string(machine, std::move(*text));
            if (!result) {
                return std::unexpected(result.error());
            }
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/lang/String", "valueOf", "([CII)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto array = arguments[0].as_reference();
            auto offset = arguments[1].as_int();
            auto count = arguments[2].as_int();
            if (!array || !offset || !count) {
                return fail(ErrorCode::invalid_argument,
                            "String.valueOf char slice arguments are invalid");
            }
            auto text = char_array_slice(machine, *array, *offset, *count);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto result = create_java_string(machine, std::move(*text));
            if (!result) {
                return std::unexpected(result.error());
            }
            return std::optional<Value>(Value::from_reference(*result));
        });

    const auto register_value_of = [&registry](std::string descriptor,
                                               NativeMethod converter) {
        add(registry, "java/lang/String", "valueOf",
            std::move(descriptor), std::move(converter));
    };
    register_value_of("(Z)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_int();
            if (!value) return std::unexpected(value.error());
            auto result = create_java_string(
                machine, *value == 0 ? std::u16string(u"false")
                                     : std::u16string(u"true"));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    register_value_of("(C)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_int();
            if (!value) return std::unexpected(value.error());
            auto result = create_java_string(
                machine, std::u16string(1, static_cast<char16_t>(
                    static_cast<u16>(*value))));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    register_value_of("(I)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_int();
            if (!value) return std::unexpected(value.error());
            auto text = integral_text(*value);
            if (!text) return std::unexpected(text.error());
            auto result = create_java_string(machine, std::move(*text));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    register_value_of("(J)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_long();
            if (!value) return std::unexpected(value.error());
            auto text = integral_text(*value);
            if (!text) return std::unexpected(text.error());
            auto result = create_java_string(machine, std::move(*text));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    register_value_of("(F)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_float();
            if (!value) return std::unexpected(value.error());
            auto text = floating_text(*value);
            if (!text) return std::unexpected(text.error());
            auto result = create_java_string(machine, std::move(*text));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    register_value_of("(D)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto value = arguments[0].as_double();
            if (!value) return std::unexpected(value.error());
            auto text = floating_text(*value);
            if (!text) return std::unexpected(text.error());
            auto result = create_java_string(machine, std::move(*text));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
}

} // namespace

void register_core_natives(NativeMethodRegistry& registry) {
    registry.begin_registration_batch();
    register_array_deque_natives(registry);
    register_bluetooth_natives(registry);
    register_canvas_natives(registry);
    register_class_natives(registry);
    register_choice_natives(registry);
    register_connection_natives(registry);
    register_console_natives(registry);
    register_file_natives(registry);
    register_game_canvas_natives(registry);
    register_game_api_natives(registry);
    register_graphics_natives(registry);
    register_headless_compat_natives(registry);
    register_image_natives(registry);
    register_io_natives(registry);
    register_lcdui_natives(registry);
    register_math_natives(registry);
    register_m3g_natives(registry);
    register_micro3d_natives(registry);
    register_media_natives(registry);
    register_push_natives(registry);
    register_reference_natives(registry);
    register_rms_natives(registry);
    register_security_natives(registry);
    register_string_encoding_natives(registry);
    register_time_natives(registry);
    register_util_natives(registry);
    register_jdk8_compat_natives(registry);
    register_wrapper_natives(registry);
    register_xml_natives(registry);
    register_vendor_natives(registry);
    register_sensor_natives(registry);
    register_pim_natives(registry);
    register_amms_natives(registry);

    add(registry,
        "java/lang/Object",
        "<init>",
        "()V",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Object",
        "hashCode",
        "()I",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            return std::optional<Value>(
                Value::from_int(stable_identity_hash(*receiver)));
        });

    add(registry,
        "java/lang/Object",
        "equals",
        "(Ljava/lang/Object;)Z",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (arguments.size() != 2) {
                return fail(ErrorCode::invalid_argument,
                            "Object.equals expects one argument");
            }
            auto other = arguments[1].as_reference();
            if (!other) {
                return std::unexpected(other.error());
            }
            return std::optional<Value>(
                Value::from_int(*receiver == *other ? 1 : 0));
        });

    add(registry,
        "java/lang/Object",
        "toString",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto text = identity_object_text(machine, *receiver);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto string = create_java_string(machine, std::move(*text));
            if (!string) {
                return std::unexpected(string.error());
            }
            return std::optional<Value>(Value::from_reference(*string));
        });

    add(registry,
        "java/lang/Object",
        "wait",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto waited = machine.wait_on_object(*receiver, std::nullopt);
            if (!waited) {
                return std::unexpected(waited.error());
            }
            if (*waited == MonitorWaitResult::interrupted) {
                return fail_java("java/lang/InterruptedException",
                                 "Object.wait was interrupted");
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Object",
        "wait",
        "(J)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "Object.wait(long) expects one timeout");
            }
            auto receiver = require_receiver(arguments);
            auto timeout = arguments[1].as_long();
            if (!receiver || !timeout) {
                return fail(ErrorCode::invalid_argument,
                            "Object.wait(long) arguments are invalid");
            }
            auto waited = machine.wait_on_object(*receiver, *timeout);
            if (!waited) {
                return std::unexpected(waited.error());
            }
            if (*waited == MonitorWaitResult::interrupted) {
                return fail_java("java/lang/InterruptedException",
                                 "Object.wait was interrupted");
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Object",
        "wait",
        "(JI)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 3U) {
                return fail(ErrorCode::invalid_argument,
                            "Object.wait(long,int) expects timeout and nanos");
            }
            auto receiver = require_receiver(arguments);
            auto timeout = arguments[1].as_long();
            auto nanos = arguments[2].as_int();
            if (!receiver || !timeout || !nanos) {
                return fail(ErrorCode::invalid_argument,
                            "Object.wait(long,int) arguments are invalid");
            }
            if (*timeout < 0 || *nanos < 0 || *nanos > 999'999) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Object.wait timeout is out of range");
            }
            i64 rounded_timeout = *timeout;
            if (*nanos >= 500'000 || (*nanos != 0 && rounded_timeout == 0)) {
                if (rounded_timeout == std::numeric_limits<i64>::max()) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Object.wait timeout overflowed");
                }
                ++rounded_timeout;
            }
            auto waited = machine.wait_on_object(*receiver, rounded_timeout);
            if (!waited) {
                return std::unexpected(waited.error());
            }
            if (*waited == MonitorWaitResult::interrupted) {
                return fail_java("java/lang/InterruptedException",
                                 "Object.wait was interrupted");
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Object",
        "notify",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto notified = machine.notify_object(*receiver, false);
            if (!notified) {
                return std::unexpected(notified.error());
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Object",
        "notifyAll",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto notified = machine.notify_object(*receiver, true);
            if (!notified) {
                return std::unexpected(notified.error());
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/String",
        "length",
        "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto length = machine.heap().string_length(*receiver);
            if (!length) return std::unexpected(length.error());
            if (*length > static_cast<usize>(std::numeric_limits<i32>::max())) {
                return fail(ErrorCode::overflow,
                            "Java String length exceeds int range");
            }
            return std::optional<Value>(
                Value::from_int(static_cast<i32>(*length)));
        },
        NativeJitPolicy::synchronous_bounded);

    add(registry,
        "java/lang/String",
        "isEmpty",
        "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto text = machine.heap().string_value(*receiver);
            if (!text) {
                return std::unexpected(text.error());
            }
            return std::optional<Value>(Value::from_int(text->empty() ? 1 : 0));
        });

    add(registry,
        "java/lang/String",
        "isBlank",
        "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto text = machine.heap().string_value(*receiver);
            if (!text) {
                return std::unexpected(text.error());
            }
            for (const char16_t character : *text) {
                const bool whitespace =
                    character == u' ' || character == u'\t' ||
                    character == u'\n' || character == u'\v' ||
                    character == u'\f' || character == u'\r' ||
                    character == 0x001CU || character == 0x001DU ||
                    character == 0x001EU || character == 0x001FU ||
                    character == 0x1680U ||
                    (character >= 0x2000U && character <= 0x2006U) ||
                    (character >= 0x2008U && character <= 0x200AU) ||
                    character == 0x2028U || character == 0x2029U ||
                    character == 0x205FU || character == 0x3000U;
                if (!whitespace) {
                    return std::optional<Value>(Value::from_int(0));
                }
            }
            return std::optional<Value>(Value::from_int(1));
        });

    add(registry,
        "java/lang/String",
        "charAt",
        "(I)C",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2) {
                return fail(ErrorCode::invalid_argument,
                            "String.charAt expects receiver and index");
            }
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto index = arguments[1].as_int();
            if (!index) {
                return std::unexpected(index.error());
            }
            if (*index < 0) {
                return fail_java("java/lang/StringIndexOutOfBoundsException",
                                 "String.charAt index is out of range");
            }
            auto character = machine.heap().string_character(
                *receiver, static_cast<usize>(*index));
            if (!character) {
                if (character.error().code == ErrorCode::out_of_range) {
                    return fail_java("java/lang/StringIndexOutOfBoundsException",
                                     "String.charAt index is out of range");
                }
                return std::unexpected(character.error());
            }
            return std::optional<Value>(Value::from_int(static_cast<i32>(
                *character)));
        },
        NativeJitPolicy::synchronous_bounded);

    add(registry,
        "java/lang/String",
        "equals",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2) {
                return fail(ErrorCode::invalid_argument,
                            "String.equals expects receiver and argument");
            }
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto other = arguments[1].as_reference();
            if (!other) {
                return std::unexpected(other.error());
            }
            if (other->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto other_class = machine.heap().class_name(*other);
            if (!other_class || *other_class != "java/lang/String") {
                return std::optional<Value>(Value::from_int(0));
            }
            auto left = machine.heap().string_value(*receiver);
            auto right = machine.heap().string_value(*other);
            if (!left) {
                return std::unexpected(left.error());
            }
            if (!right) {
                return std::unexpected(right.error());
            }
            return std::optional<Value>(Value::from_int(*left == *right ? 1 : 0));
        });

    add(registry,
        "java/lang/String",
        "hashCode",
        "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto text = machine.heap().string_value(*receiver);
            if (!text) {
                return std::unexpected(text.error());
            }
            u32 hash = 0;
            for (const char16_t character : *text) {
                hash = hash * 31U + static_cast<u16>(character);
            }
            return std::optional<Value>(
                Value::from_int(static_cast<i32>(hash)));
        });

    add(registry,
        "java/lang/String",
        "intern",
        "()Ljava/lang/String;",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            return std::optional<Value>(Value::from_reference(*receiver));
        });

    add(registry,
        "java/lang/String",
        "toString",
        "()Ljava/lang/String;",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            return std::optional<Value>(Value::from_reference(*receiver));
        });

    register_string_extensions(registry);
    register_text_builder(registry, "java/lang/StringBuilder");
    register_text_builder(registry, "java/lang/StringBuffer");

    add(registry, "java/lang/System", "getenv",
        "(Ljava/lang/String;)Ljava/lang/String;",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1U) {
                return fail(ErrorCode::invalid_argument,
                            "System.getenv expects one argument");
            }
            auto name = arguments[0].as_reference();
            if (!name || name->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "System.getenv name is null");
            }
            return std::optional<Value>(Value::from_reference({}));
        });
    add(registry, "java/lang/System", "lineSeparator",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "System.lineSeparator expects no arguments");
            }
            auto result = create_java_string(machine, u"\n");
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });

    add(registry,
        "java/lang/System",
        "identityHashCode",
        "(Ljava/lang/Object;)I",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1) {
                return fail(ErrorCode::invalid_argument,
                            "identityHashCode expects one argument");
            }
            auto reference = arguments[0].as_reference();
            if (!reference) {
                return std::unexpected(reference.error());
            }
            return std::optional<Value>(Value::from_int(
                reference->is_null() ? 0 : stable_identity_hash(*reference)));
        });

    add(registry,
        "java/lang/System",
        "linkLegacyWtChain",
        "(Ljava/lang/Object;Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "System.linkLegacyWtChain expects two objects");
            }
            auto current = arguments[0].as_reference();
            auto value = arguments[1].as_reference();
            if (!current || !value) {
                return fail(ErrorCode::invalid_argument,
                            "System.linkLegacyWtChain arguments are invalid");
            }
            if (current->is_null() || value->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }

            ObjectRef node = *current;
            constexpr usize kMaximumLinks = 65'536U;
            for (usize index = 0; index < kMaximumLinks; ++index) {
                if (node == *value) {
                    return std::optional<Value>(Value::from_int(1));
                }
                auto class_name = machine.heap().class_name(node);
                if (!class_name) {
                    return std::unexpected(class_name.error());
                }
                auto field = machine.class_states().resolve_field(
                    *class_name, "c", "Lwt;", false);
                if (!field) {
                    return std::optional<Value>(Value::from_int(0));
                }
                auto next_value = machine.heap().field(node, field->index);
                if (!next_value) {
                    return std::unexpected(next_value.error());
                }
                auto next = next_value->as_reference();
                if (!next) {
                    return std::optional<Value>(Value::from_int(0));
                }
                if (next->is_null()) {
                    auto stored = machine.heap().set_field(
                        node, field->index, Value::from_reference(*value));
                    if (!stored) {
                        return std::unexpected(stored.error());
                    }
                    return std::optional<Value>(Value::from_int(1));
                }
                node = *next;
            }
            return std::optional<Value>(Value::from_int(0));
        });

    add(registry,
        "java/lang/System",
        "arraycopy",
        "(Ljava/lang/Object;ILjava/lang/Object;II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 5U) {
                return fail(ErrorCode::invalid_argument,
                            "System.arraycopy expects five arguments");
            }
            auto source = arguments[0].as_reference();
            auto source_position = arguments[1].as_int();
            auto destination = arguments[2].as_reference();
            auto destination_position = arguments[3].as_int();
            auto length = arguments[4].as_int();
            if (!source || !source_position || !destination ||
                !destination_position || !length) {
                return fail(ErrorCode::invalid_argument,
                            "System.arraycopy argument kinds are invalid");
            }
            if (source->is_null() || destination->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "System.arraycopy received a null array");
            }
            if (*source_position < 0 || *destination_position < 0 ||
                *length < 0) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "System.arraycopy range is negative");
            }
            const usize source_start = static_cast<usize>(*source_position);
            const usize destination_start =
                static_cast<usize>(*destination_position);
            const usize count = static_cast<usize>(*length);

            // Compression/image loaders frequently use tiny byte[]/int[]
            // arraycopy operations in their inner loops. Resolve both arrays,
            // validate their primitive kinds/ranges and perform the copy under
            // one Heap lock instead of taking separate class-name, length and
            // copy locks for every call. Reference arrays deliberately fall
            // through to the full Java assignability path below.
            auto primitive_copy = machine.heap().try_copy_primitive_array_range(
                *source, source_start,
                *destination, destination_start,
                count);
            if (!primitive_copy) {
                if (primitive_copy.error().code == ErrorCode::out_of_range) {
                    return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                     "System.arraycopy range exceeds array bounds");
                }
                if (primitive_copy.error().code == ErrorCode::invalid_state ||
                    primitive_copy.error().code == ErrorCode::invalid_argument) {
                    return fail_java("java/lang/ArrayStoreException",
                                     primitive_copy.error().message);
                }
                return std::unexpected(primitive_copy.error());
            }
            if (*primitive_copy) {
                return std::optional<Value> {};
            }

            auto source_class = machine.heap().class_name(*source);
            auto destination_class = machine.heap().class_name(*destination);
            if (!source_class || !destination_class) {
                return fail_java("java/lang/ArrayStoreException",
                                 "System.arraycopy requires array objects");
            }
            auto source_length = machine.heap().array_length(*source);
            auto destination_length = machine.heap().array_length(*destination);
            if (!source_length || !destination_length) {
                return fail_java("java/lang/ArrayStoreException",
                                 "System.arraycopy requires array objects");
            }
            if (source_start > *source_length ||
                count > *source_length - source_start ||
                destination_start > *destination_length ||
                count > *destination_length - destination_start) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "System.arraycopy range exceeds array bounds");
            }

            auto source_component = array_component_name(*source_class);
            auto destination_component = array_component_name(*destination_class);
            if (!source_component || !destination_component) {
                return fail_java("java/lang/ArrayStoreException",
                                 "System.arraycopy array descriptor is invalid");
            }
            const bool source_primitive = source_component->size() == 1U;
            const bool destination_primitive =
                destination_component->size() == 1U;
            if (source_primitive || destination_primitive) {
                // Primitive/mixed arrays are handled by the one-lock fast path
                // above. Reaching this branch means heap metadata changed
                // unexpectedly while System.arraycopy held the VM execution
                // gate, which is an internal consistency failure.
                return fail(ErrorCode::internal_error,
                            "System.arraycopy primitive fast path was bypassed");
            }

            std::vector<Value> copied;
            copied.reserve(count);
            for (usize index = 0; index < count; ++index) {
                auto value = machine.heap().element(*source,
                                                    source_start + index);
                if (!value) {
                    return std::unexpected(value.error());
                }
                copied.push_back(*value);
            }
            if (!destination_primitive) {
                for (const Value& value : copied) {
                    auto reference = value.as_reference();
                    if (!reference) {
                        return fail_java("java/lang/ArrayStoreException",
                                         "reference array contains a primitive value");
                    }
                    if (reference->is_null()) {
                        continue;
                    }
                    auto value_class = machine.heap().class_name(*reference);
                    if (!value_class) {
                        return std::unexpected(value_class.error());
                    }
                    auto assignable = machine.classes().is_assignable(
                        *value_class,
                        *destination_component);
                    if (!assignable) {
                        return std::unexpected(assignable.error());
                    }
                    if (!*assignable) {
                        return fail_java("java/lang/ArrayStoreException",
                                         "array element is not assignable to destination");
                    }
                }
            }
            for (usize index = 0; index < count; ++index) {
                auto stored = machine.heap().set_element(
                    *destination,
                    destination_start + index,
                    copied[index]);
                if (!stored) {
                    return std::unexpected(stored.error());
                }
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/System",
        "currentTimeMillis",
        "()J",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "currentTimeMillis expects no arguments");
            }
            const auto now = std::chrono::system_clock::now()
                                 .time_since_epoch();
            const auto milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(now)
                    .count();
            return std::optional<Value>(
                Value::from_long(static_cast<i64>(milliseconds)));
        });

    add(registry,
        "java/lang/Float",
        "floatToIntBits",
        "(F)I",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1) {
                return fail(ErrorCode::invalid_argument,
                            "floatToIntBits expects one argument");
            }
            auto value = arguments[0].as_float();
            if (!value) {
                return std::unexpected(value.error());
            }
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(std::bit_cast<u32>(*value))));
        });

    add(registry,
        "java/lang/Float",
        "intBitsToFloat",
        "(I)F",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1) {
                return fail(ErrorCode::invalid_argument,
                            "intBitsToFloat expects one argument");
            }
            auto value = arguments[0].as_int();
            if (!value) {
                return std::unexpected(value.error());
            }
            return std::optional<Value>(Value::from_float(
                std::bit_cast<float>(static_cast<u32>(*value))));
        });

    add(registry,
        "java/lang/Double",
        "doubleToLongBits",
        "(D)J",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1) {
                return fail(ErrorCode::invalid_argument,
                            "doubleToLongBits expects one argument");
            }
            auto value = arguments[0].as_double();
            if (!value) {
                return std::unexpected(value.error());
            }
            return std::optional<Value>(Value::from_long(
                static_cast<i64>(std::bit_cast<u64>(*value))));
        });

    add(registry,
        "java/lang/Double",
        "longBitsToDouble",
        "(J)D",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1) {
                return fail(ErrorCode::invalid_argument,
                            "longBitsToDouble expects one argument");
            }
            auto value = arguments[0].as_long();
            if (!value) {
                return std::unexpected(value.error());
            }
            return std::optional<Value>(Value::from_double(
                std::bit_cast<double>(static_cast<u64>(*value))));
        });

    add(registry,
        "java/lang/Runtime",
        "getRuntime",
        "()Ljava/lang/Runtime;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Runtime.getRuntime expects no arguments");
            }
            auto field = machine.class_states().resolve_field(
                "java/lang/Runtime",
                "currentRuntime",
                "Ljava/lang/Runtime;",
                true);
            if (!field) return std::unexpected(field.error());
            auto current = machine.class_states().static_field(*field);
            if (!current) return std::unexpected(current.error());
            auto reference = current->as_reference();
            if (!reference) return std::unexpected(reference.error());
            if (reference->is_null()) {
                auto allocated = machine.class_states().allocate_instance(
                    machine.heap(), "java/lang/Runtime");
                if (!allocated) return std::unexpected(allocated.error());
                auto stored = machine.class_states().set_static_field(
                    *field, Value::from_reference(*allocated));
                if (!stored) return std::unexpected(stored.error());
                reference = *allocated;
            }
            return std::optional<Value>(Value::from_reference(*reference));
        });

    add(registry,
        "java/lang/Runtime",
        "totalMemory",
        "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            const usize capacity = machine.heap().stats().maximum_bytes;
            return std::optional<Value>(
                Value::from_long(static_cast<i64>(capacity)));
        });

    add(registry,
        "java/lang/Runtime",
        "freeMemory",
        "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            const auto stats = machine.heap().stats();
            const usize capacity = stats.maximum_bytes;
            const usize used = stats.estimated_bytes;
            const usize free = used >= capacity ? 0 : capacity - used;
            return std::optional<Value>(
                Value::from_long(static_cast<i64>(free)));
        });

    add(registry,
        "java/lang/Runtime",
        "maxMemory",
        "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) return std::unexpected(receiver.error());
            return std::optional<Value>(Value::from_long(
                static_cast<i64>(machine.heap().stats().maximum_bytes)));
        });

    add(registry,
        "java/lang/Integer",
        "bitCount",
        "(I)I",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1U) {
                return fail(ErrorCode::invalid_argument,
                            "Integer.bitCount expects one argument");
            }
            auto value = arguments[0].as_int();
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(static_cast<i32>(
                std::popcount(static_cast<u32>(*value)))));
        });

    add(registry,
        "java/lang/Runtime",
        "gc",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto collected = machine.collect_garbage();
            if (!collected) {
                return std::unexpected(collected.error());
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/util/Objects",
        "requireNonNull",
        "(Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1U) {
                return fail(ErrorCode::invalid_argument,
                            "Objects.requireNonNull expects one argument");
            }
            auto reference = arguments.front().as_reference();
            if (!reference || reference->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Objects.requireNonNull received null");
            }
            return std::optional<Value>(arguments.front());
        });

    add(registry,
        "java/lang/Thread",
        "<init>",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto initialized = machine.initialize_java_thread(*receiver,
                                                              ObjectRef {});
            if (!initialized) {
                return std::unexpected(initialized.error());
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Thread",
        "<init>",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "Thread(String) expects one name");
            }
            auto receiver = require_receiver(arguments);
            auto name = arguments[1].as_reference();
            if (!receiver || !name) {
                return fail(ErrorCode::invalid_argument,
                            "Thread(String) arguments are invalid");
            }
            if (name->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Thread name is null");
            }
            auto initialized = machine.initialize_java_thread(*receiver,
                                                              ObjectRef {});
            if (!initialized) {
                return std::unexpected(initialized.error());
            }
            auto stored = set_thread_name(machine, *receiver, *name);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Thread",
        "<init>",
        "(Ljava/lang/Runnable;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "Thread(Runnable) expects one target");
            }
            auto receiver = require_receiver(arguments);
            auto target = arguments[1].as_reference();
            if (!receiver || !target) {
                return fail(ErrorCode::invalid_argument,
                            "Thread(Runnable) arguments are invalid");
            }
            auto initialized = machine.initialize_java_thread(*receiver,
                                                              *target);
            if (!initialized) {
                return std::unexpected(initialized.error());
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Thread",
        "<init>",
        "(Ljava/lang/Runnable;Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 3U) {
                return fail(ErrorCode::invalid_argument,
                            "Thread(Runnable,String) expects target and name");
            }
            auto receiver = require_receiver(arguments);
            auto target = arguments[1].as_reference();
            auto name = arguments[2].as_reference();
            if (!receiver || !target || !name) {
                return fail(ErrorCode::invalid_argument,
                            "Thread(Runnable,String) arguments are invalid");
            }
            if (name->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Thread name is null");
            }
            auto initialized = machine.initialize_java_thread(*receiver,
                                                              *target);
            if (!initialized) return std::unexpected(initialized.error());
            auto stored = set_thread_name(machine, *receiver, *name);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Thread",
        "start",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto started = machine.start_java_thread(*receiver);
            if (!started) {
                return std::unexpected(started.error());
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Thread",
        "run",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            return machine.run_java_thread_target(*receiver);
        });

    add(registry,
        "java/lang/Thread",
        "currentThread",
        "()Ljava/lang/Thread;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Thread.currentThread expects no arguments");
            }
            auto current = machine.current_java_thread();
            if (!current) {
                return std::unexpected(current.error());
            }
            return std::optional<Value>(Value::from_reference(*current));
        });

    add(registry,
        "java/lang/Thread",
        "activeCount",
        "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Thread.activeCount expects no arguments");
            }
            const auto snapshot = machine.scheduler().snapshot();
            const usize count = static_cast<usize>(std::count_if(
                snapshot.threads.begin(),
                snapshot.threads.end(),
                [](const JavaThreadSnapshot& thread) {
                    return thread.alive;
                }));
            return std::optional<Value>(Value::from_int(
                count > static_cast<usize>(std::numeric_limits<i32>::max())
                    ? std::numeric_limits<i32>::max()
                    : static_cast<i32>(count)));
        });

    add(registry,
        "java/lang/Thread",
        "yield",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Thread.yield expects no arguments");
            }
            machine.cooperative_yield();
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Thread",
        "sleep",
        "(J)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1U) {
                return fail(ErrorCode::invalid_argument,
                            "Thread.sleep expects one timeout");
            }
            auto timeout = arguments[0].as_long();
            if (!timeout) {
                return std::unexpected(timeout.error());
            }
            auto slept = machine.sleep_current_thread(*timeout);
            if (!slept) {
                return std::unexpected(slept.error());
            }
            if (*slept == SchedulerWaitResult::interrupted) {
                return fail_java("java/lang/InterruptedException",
                                 "Thread.sleep was interrupted");
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Thread",
        "join",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto joined = machine.join_java_thread(*receiver, std::nullopt);
            if (!joined) {
                return std::unexpected(joined.error());
            }
            if (*joined == SchedulerWaitResult::interrupted) {
                return fail_java("java/lang/InterruptedException",
                                 "Thread.join was interrupted");
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Thread",
        "join",
        "(J)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "Thread.join(long) expects one timeout");
            }
            auto receiver = require_receiver(arguments);
            auto timeout = arguments[1].as_long();
            if (!receiver || !timeout) {
                return fail(ErrorCode::invalid_argument,
                            "Thread.join(long) arguments are invalid");
            }
            auto joined = machine.join_java_thread(*receiver, *timeout);
            if (!joined) {
                return std::unexpected(joined.error());
            }
            if (*joined == SchedulerWaitResult::interrupted) {
                return fail_java("java/lang/InterruptedException",
                                 "Thread.join was interrupted");
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Thread",
        "isAlive",
        "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto alive = machine.scheduler().is_alive(*receiver);
            if (!alive) {
                return std::unexpected(alive.error());
            }
            return std::optional<Value>(Value::from_int(*alive ? 1 : 0));
        });

    add(registry,
        "java/lang/Thread",
        "interrupt",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto interrupted = machine.interrupt_java_thread(*receiver);
            if (!interrupted) {
                return std::unexpected(interrupted.error());
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Thread",
        "interrupted",
        "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Thread.interrupted expects no arguments");
            }
            return std::optional<Value>(Value::from_int(
                machine.scheduler().consume_current_interrupt() ? 1 : 0));
        });

    add(registry,
        "java/lang/Thread",
        "isInterrupted",
        "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto interrupted = machine.scheduler().is_interrupted(*receiver);
            if (!interrupted) {
                return std::unexpected(interrupted.error());
            }
            return std::optional<Value>(Value::from_int(
                *interrupted ? 1 : 0));
        });

    add(registry,
        "java/lang/Thread",
        "setPriority",
        "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "Thread.setPriority expects one value");
            }
            auto receiver = require_receiver(arguments);
            auto priority = arguments[1].as_int();
            if (!receiver || !priority) {
                return fail(ErrorCode::invalid_argument,
                            "Thread.setPriority arguments are invalid");
            }
            auto updated = machine.scheduler().set_priority(*receiver,
                                                            *priority);
            if (!updated) {
                return std::unexpected(updated.error());
            }
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Thread",
        "getPriority",
        "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto priority = machine.scheduler().priority(*receiver);
            if (!priority) {
                return std::unexpected(priority.error());
            }
            return std::optional<Value>(Value::from_int(*priority));
        });

    add(registry,
        "java/lang/Thread",
        "getName",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto name = get_thread_name(machine, *receiver);
            if (!name) {
                return std::unexpected(name.error());
            }
            return std::optional<Value>(Value::from_reference(*name));
        });

    add(registry,
        "java/lang/Thread",
        "checkAccess",
        "()V",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            // phoneME CLDC's AccessController.checkPermission body is empty;
            // preserve that behavior rather than introducing a new denial path.
            return std::optional<Value> {};
        });

    add(registry,
        "java/lang/Thread",
        "toString",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            auto name = get_thread_name(machine, *receiver);
            if (!name) {
                return std::unexpected(name.error());
            }
            auto name_text = machine.heap().string_value(*name);
            if (!name_text) {
                return std::unexpected(name_text.error());
            }
            auto priority = machine.scheduler().priority(*receiver);
            if (!priority) {
                return std::unexpected(priority.error());
            }
            auto priority_text = integral_text(*priority);
            if (!priority_text) {
                return std::unexpected(priority_text.error());
            }
            std::u16string text(u"Thread[");
            text.append(*name_text);
            text.push_back(u',');
            text.append(*priority_text);
            text.push_back(u']');
            auto string = create_java_string(machine, std::move(text));
            if (!string) {
                return std::unexpected(string.error());
            }
            return std::optional<Value>(Value::from_reference(*string));
        });

    const auto add_midlet_signal = [&registry](const char* name,
                                               MidletSignal signal) {
        add(registry,
            "javax/microedition/midlet/MIDlet",
            name,
            "()V",
            [signal](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto receiver = require_receiver(arguments);
                if (!receiver) {
                    return std::unexpected(receiver.error());
                }
                machine.signal_midlet(signal);
                return std::optional<Value> {};
            });
    };
    add_midlet_signal("notifyDestroyed", MidletSignal::destroyed);
    add_midlet_signal("notifyPaused", MidletSignal::paused);
    add_midlet_signal("resumeRequest", MidletSignal::resume_requested);

    add(registry,
        "javax/microedition/midlet/MIDlet",
        "getAppProperty",
        "(Ljava/lang/String;)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (arguments.size() != 2) {
                return fail(ErrorCode::invalid_argument,
                            "MIDlet.getAppProperty expects one key");
            }
            auto key = arguments[1].as_reference();
            if (!key) {
                return std::unexpected(key.error());
            }
            auto property = machine.app_property(*key);
            if (!property) {
                return std::unexpected(property.error());
            }
            if (!property->has_value()) {
                return std::optional<Value>(Value::from_reference({}));
            }
            auto string = create_java_string(machine,
                                             std::move(**property));
            if (!string) {
                return std::unexpected(string.error());
            }
            return std::optional<Value>(Value::from_reference(*string));
        });

    add(registry,
        "javax/microedition/midlet/MIDlet",
        "platformRequest",
        "(Ljava/lang/String;)Z",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto receiver = require_receiver(arguments);
            if (!receiver) {
                return std::unexpected(receiver.error());
            }
            if (arguments.size() != 2) {
                return fail(ErrorCode::invalid_argument,
                            "MIDlet.platformRequest expects one URL");
            }
            return std::optional<Value>(Value::from_int(0));
        });
    registry.end_registration_batch();
}

} // namespace phoneme::vm
