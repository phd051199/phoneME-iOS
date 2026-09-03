#include "ClassNatives.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "phoneme/base/Types.hpp"
#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/NativeMethodRegistry.hpp"

namespace phoneme::vm {
namespace {

constexpr u16 kAccPublic = 0x0001U;
constexpr u16 kAccStatic = 0x0008U;
constexpr u16 kAccInterface = 0x0200U;
constexpr usize kByteInputBufferField = 0;
constexpr usize kByteInputPositionField = 1;
constexpr usize kByteInputMarkField = 2;
constexpr usize kByteInputCountField = 3;
constexpr usize kUrlOwnerField = 0;
constexpr usize kUrlNameField = 1;
constexpr usize kUriPathField = 0;
constexpr usize kReflectFieldDeclaringClassField = 0;
constexpr usize kReflectFieldNameField = 1;
constexpr usize kReflectFieldDescriptorField = 2;
constexpr usize kReflectFieldModifiersField = 3;
constexpr usize kReflectMethodDeclaringClassField = 0;
constexpr usize kReflectMethodNameField = 1;
constexpr usize kReflectMethodDescriptorField = 2;
constexpr usize kReflectMethodModifiersField = 3;

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

[[nodiscard]] Result<ObjectRef> receiver(
    std::span<const Value> arguments) {
    if (arguments.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "class native has no receiver");
    }
    auto reference = arguments.front().as_reference();
    if (!reference || reference->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "class native receiver is null");
    }
    return *reference;
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

[[nodiscard]] std::u16string ascii_text(std::string_view text) {
    std::u16string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(static_cast<char16_t>(
            static_cast<unsigned char>(character)));
    }
    return result;
}

[[nodiscard]] Result<std::string> utf8_text(Machine& machine,
                                            ObjectRef string) {
    if (string.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "String argument is null");
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

[[nodiscard]] Result<ObjectRef> attach_byte_input_stream(
    Machine& machine,
    ObjectRef buffer);

[[nodiscard]] Result<ObjectRef> create_byte_input_stream(
    Machine& machine,
    std::span<const phoneme::u8> bytes) {
    auto array = machine.heap().allocate_array(
        "[B", bytes.size(), Value::from_int(0));
    if (!array && array.error().code == ErrorCode::overflow) {
        auto collected = machine.collect_garbage();
        if (!collected) return std::unexpected(collected.error());
        array = machine.heap().allocate_array(
            "[B", bytes.size(), Value::from_int(0));
    }
    if (!array) return std::unexpected(array.error());
    auto array_root = machine.pin_native_root(*array);
    if (!array_root) return std::unexpected(array_root.error());

    auto bytes_stored = machine.heap().write_byte_array(*array, 0U, bytes);
    if (!bytes_stored) return std::unexpected(bytes_stored.error());
    return attach_byte_input_stream(machine, *array);
}

[[nodiscard]] Result<ObjectRef> attach_byte_input_stream(
    Machine& machine,
    ObjectRef buffer) {
    auto array_root = machine.pin_native_root(buffer);
    if (!array_root) return std::unexpected(array_root.error());
    auto length = machine.heap().array_length(buffer);
    if (!length) return std::unexpected(length.error());

    auto stream_root = machine.allocate_pinned_instance(
        "java/io/ByteArrayInputStream");
    if (!stream_root) return std::unexpected(stream_root.error());
    auto stream = stream_root->get();
    if (!stream) return std::unexpected(stream.error());
    auto buffer_stored = machine.heap().set_field(
        *stream, kByteInputBufferField, Value::from_reference(buffer));
    auto position_stored = machine.heap().set_field(
        *stream, kByteInputPositionField, Value::from_int(0));
    auto mark_stored = machine.heap().set_field(
        *stream, kByteInputMarkField, Value::from_int(0));
    auto count_stored = machine.heap().set_field(
        *stream, kByteInputCountField,
        Value::from_int(static_cast<i32>(*length)));
    if (!buffer_stored) return std::unexpected(buffer_stored.error());
    if (!position_stored) return std::unexpected(position_stored.error());
    if (!mark_stored) return std::unexpected(mark_stored.error());
    if (!count_stored) return std::unexpected(count_stored.error());
    return *stream;
}

[[nodiscard]] std::string normalized_class_name(std::string name) {
    std::replace(name.begin(), name.end(), '.', '/');
    return name;
}

[[nodiscard]] bool is_primitive_name(std::string_view name) noexcept {
    return name.size() == 1U &&
           std::string_view("ZBCSIJFDV").find(name.front()) !=
               std::string_view::npos;
}

[[nodiscard]] std::string display_class_name(std::string name) {
    if (name.size() == 1U) {
        switch (name.front()) {
        case 'Z': return "boolean";
        case 'B': return "byte";
        case 'C': return "char";
        case 'S': return "short";
        case 'I': return "int";
        case 'J': return "long";
        case 'F': return "float";
        case 'D': return "double";
        case 'V': return "void";
        default: break;
        }
    }
    std::replace(name.begin(), name.end(), '/', '.');
    return name;
}

[[nodiscard]] Result<ObjectRef> box_reflect_value(
    Machine& machine,
    Value value,
    std::string_view descriptor) {
    if (!descriptor.empty() &&
        (descriptor.front() == 'L' || descriptor.front() == '[')) {
        return value.as_reference();
    }
    std::string_view owner;
    std::string_view factory_descriptor;
    switch (descriptor.empty() ? '\0' : descriptor.front()) {
    case 'Z': owner = "java/lang/Boolean";
              factory_descriptor = "(Z)Ljava/lang/Boolean;"; break;
    case 'B': owner = "java/lang/Byte";
              factory_descriptor = "(B)Ljava/lang/Byte;"; break;
    case 'C': owner = "java/lang/Character";
              factory_descriptor = "(C)Ljava/lang/Character;"; break;
    case 'S': owner = "java/lang/Short";
              factory_descriptor = "(S)Ljava/lang/Short;"; break;
    case 'I': owner = "java/lang/Integer";
              factory_descriptor = "(I)Ljava/lang/Integer;"; break;
    case 'J': owner = "java/lang/Long";
              factory_descriptor = "(J)Ljava/lang/Long;"; break;
    case 'F': owner = "java/lang/Float";
              factory_descriptor = "(F)Ljava/lang/Float;"; break;
    case 'D': owner = "java/lang/Double";
              factory_descriptor = "(D)Ljava/lang/Double;"; break;
    default:
        return fail(ErrorCode::invalid_argument,
                    "reflection field has unsupported descriptor");
    }
    const std::array<Value, 1> arguments {value};
    auto boxed = machine.invoke_static(owner, "valueOf", factory_descriptor,
                                       arguments);
    if (!boxed) return std::unexpected(boxed.error());
    if (boxed->throwable.has_value()) {
        auto throwable = machine.heap().class_name(*boxed->throwable);
        if (!throwable) return std::unexpected(throwable.error());
        return fail_java(*throwable, "reflection value boxing failed");
    }
    if (!boxed->return_value.has_value()) {
        return fail(ErrorCode::internal_error,
                    "wrapper valueOf returned no value");
    }
    return boxed->return_value->as_reference();
}

[[nodiscard]] std::string descriptor_for_class_name(
    std::string_view class_name) {
    if (class_name.size() == 1U && is_primitive_name(class_name)) {
        return std::string(class_name);
    }
    if (!class_name.empty() && class_name.front() == '[') {
        return std::string(class_name);
    }
    std::string descriptor;
    descriptor.reserve(class_name.size() + 2U);
    descriptor.push_back('L');
    descriptor.append(class_name);
    descriptor.push_back(';');
    return descriptor;
}

[[nodiscard]] Result<std::vector<std::string>> method_parameter_descriptors(
    std::string_view descriptor) {
    if (descriptor.empty() || descriptor.front() != '(') {
        return fail(ErrorCode::malformed_class,
                    "reflection method descriptor is malformed");
    }
    std::vector<std::string> parameters;
    usize cursor = 1U;
    while (cursor < descriptor.size() && descriptor[cursor] != ')') {
        const usize start = cursor;
        while (cursor < descriptor.size() && descriptor[cursor] == '[') {
            ++cursor;
        }
        if (cursor >= descriptor.size()) {
            return fail(ErrorCode::malformed_class,
                        "reflection array descriptor is truncated");
        }
        if (descriptor[cursor] == 'L') {
            const usize end = descriptor.find(';', cursor + 1U);
            if (end == std::string_view::npos) {
                return fail(ErrorCode::malformed_class,
                            "reflection reference descriptor is truncated");
            }
            cursor = end + 1U;
        } else {
            if (std::string_view("ZBCSIJFD").find(descriptor[cursor]) ==
                std::string_view::npos) {
                return fail(ErrorCode::malformed_class,
                            "reflection parameter descriptor is invalid");
            }
            ++cursor;
        }
        parameters.emplace_back(descriptor.substr(start, cursor - start));
    }
    if (cursor >= descriptor.size() || descriptor[cursor] != ')') {
        return fail(ErrorCode::malformed_class,
                    "reflection method descriptor has no return type");
    }
    return parameters;
}

[[nodiscard]] Result<std::string_view> method_return_descriptor(
    std::string_view descriptor) {
    const usize closing = descriptor.find(')');
    if (closing == std::string_view::npos || closing + 1U >= descriptor.size()) {
        return fail(ErrorCode::malformed_class,
                    "reflection method descriptor has no return type");
    }
    return descriptor.substr(closing + 1U);
}

[[nodiscard]] Result<Value> unbox_reflect_argument(
    Machine& machine,
    ObjectRef value,
    std::string_view descriptor) {
    if (descriptor.empty()) {
        return fail(ErrorCode::malformed_class,
                    "reflection argument descriptor is empty");
    }
    if (descriptor.front() == 'L' || descriptor.front() == '[') {
        return Value::from_reference(value);
    }
    if (value.is_null()) {
        return fail_java("java/lang/IllegalArgumentException",
                         "null cannot be converted to a primitive argument");
    }
    std::string_view owner;
    std::string_view method_name;
    std::string_view method_descriptor;
    switch (descriptor.front()) {
    case 'Z': owner = "java/lang/Boolean"; method_name = "booleanValue";
              method_descriptor = "()Z"; break;
    case 'B': owner = "java/lang/Byte"; method_name = "byteValue";
              method_descriptor = "()B"; break;
    case 'C': owner = "java/lang/Character"; method_name = "charValue";
              method_descriptor = "()C"; break;
    case 'S': owner = "java/lang/Short"; method_name = "shortValue";
              method_descriptor = "()S"; break;
    case 'I': owner = "java/lang/Integer"; method_name = "intValue";
              method_descriptor = "()I"; break;
    case 'J': owner = "java/lang/Long"; method_name = "longValue";
              method_descriptor = "()J"; break;
    case 'F': owner = "java/lang/Float"; method_name = "floatValue";
              method_descriptor = "()F"; break;
    case 'D': owner = "java/lang/Double"; method_name = "doubleValue";
              method_descriptor = "()D"; break;
    default:
        return fail(ErrorCode::malformed_class,
                    "unsupported reflection primitive descriptor");
    }
    auto unboxed = machine.invoke_instance(
        value, owner, method_name, method_descriptor);
    if (!unboxed) return std::unexpected(unboxed.error());
    if (unboxed->throwable.has_value() || !unboxed->return_value.has_value()) {
        return fail_java("java/lang/IllegalArgumentException",
                         "reflection argument cannot be unboxed");
    }
    return *unboxed->return_value;
}

[[nodiscard]] Result<std::string> array_component_name(
    std::string_view array_name) {
    if (array_name.empty() || array_name.front() != '[' ||
        array_name.size() < 2U) {
        return fail(ErrorCode::invalid_argument,
                    "class is not an array descriptor");
    }
    const std::string_view component = array_name.substr(1U);
    if (component.front() == '[') return std::string(component);
    if (component.front() == 'L' && component.size() >= 3U &&
        component.back() == ';') {
        return std::string(component.substr(1U, component.size() - 2U));
    }
    if (component.size() == 1U && is_primitive_name(component)) {
        return std::string(component);
    }
    return fail(ErrorCode::malformed_class,
                "array Class contains an invalid component descriptor");
}

} // namespace

void register_class_natives(NativeMethodRegistry& registry) {
    add(registry, "java/lang/Object", "getClass",
        "()Ljava/lang/Class;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto class_name = machine.heap().class_name(*object);
            if (!class_name) return std::unexpected(class_name.error());
            auto mirror = machine.class_mirror(*class_name);
            if (!mirror) return std::unexpected(mirror.error());
            return std::optional<Value>(Value::from_reference(*mirror));
        });

    add(registry, "java/lang/Class", "getClassLoader",
        "()Ljava/lang/ClassLoader;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            if (!mirror) return std::unexpected(mirror.error());
            auto class_name = machine.mirrored_class_name(*mirror);
            if (!class_name) return std::unexpected(class_name.error());
            auto loader = machine.class_states().allocate_instance(
                machine.heap(), "java/lang/ClassLoader");
            if (!loader) return std::unexpected(loader.error());
            return std::optional<Value>(Value::from_reference(*loader));
        });

    add(registry, "java/lang/ClassLoader", "<init>", "()V",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto loader = receiver(arguments);
            if (!loader) return std::unexpected(loader.error());
            return std::optional<Value> {};
        });

    add(registry, "java/lang/ClassLoader", "getResourceAsStream",
        "(Ljava/lang/String;)Ljava/io/InputStream;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto loader = receiver(arguments);
            if (!loader) return std::unexpected(loader.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "ClassLoader resource name is missing");
            }
            auto name = arguments[1].as_reference();
            if (!name || name->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "ClassLoader resource name is null");
            }
            auto resource = utf8_text(machine, *name);
            if (!resource) return std::unexpected(resource.error());
            if (resource->empty()) {
                return std::optional<Value>(Value::from_reference({}));
            }
            std::string path = *resource;
            while (!path.empty() && path.front() == '/') {
                path.erase(path.begin());
            }
            if (path.empty()) {
                return std::optional<Value>(Value::from_reference({}));
            }
            auto bytes = machine.classes().read_resource(path);
            if (!bytes) {
                if (bytes.error().code == ErrorCode::class_not_found) {
                    return std::optional<Value>(Value::from_reference({}));
                }
                if (bytes.error().code == ErrorCode::invalid_argument) {
                    return fail_java("java/lang/SecurityException",
                                     bytes.error().message);
                }
                return std::unexpected(bytes.error());
            }
            auto stream = create_byte_input_stream(machine, *bytes);
            if (!stream) return std::unexpected(stream.error());
            return std::optional<Value>(Value::from_reference(*stream));
        });

    add(registry, "java/lang/Class", "getName", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            if (!mirror) return std::unexpected(mirror.error());
            auto class_name = machine.mirrored_class_name(*mirror);
            if (!class_name) return std::unexpected(class_name.error());
            auto string = create_string(
                machine, ascii_text(display_class_name(*class_name)));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
    add(registry, "java/lang/Class", "getSimpleName", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            if (!mirror) return std::unexpected(mirror.error());
            auto class_name = machine.mirrored_class_name(*mirror);
            if (!class_name) return std::unexpected(class_name.error());
            std::string simple = display_class_name(*class_name);
            const usize package = simple.find_last_of('.');
            if (package != std::string::npos) {
                simple.erase(0U, package + 1U);
            }
            const usize nested = simple.find_last_of('$');
            if (nested != std::string::npos) {
                simple.erase(0U, nested + 1U);
            }
            auto string = create_string(machine, ascii_text(simple));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
    add(registry, "java/lang/Class", "isArray", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            if (!mirror) return std::unexpected(mirror.error());
            auto class_name = machine.mirrored_class_name(*mirror);
            if (!class_name) return std::unexpected(class_name.error());
            return std::optional<Value>(Value::from_int(
                !class_name->empty() && class_name->front() == '[' ? 1 : 0));
        });
    add(registry, "java/lang/Class", "isInterface", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            if (!mirror) return std::unexpected(mirror.error());
            auto class_name = machine.mirrored_class_name(*mirror);
            if (!class_name) return std::unexpected(class_name.error());
            if ((!class_name->empty() && class_name->front() == '[') ||
                is_primitive_name(*class_name))
                return std::optional<Value>(Value::from_int(0));
            auto loaded = machine.classes().load(*class_name);
            if (!loaded) return std::unexpected(loaded.error());
            return std::optional<Value>(Value::from_int(
                ((*loaded)->access_flags() & kAccInterface) != 0U ? 1 : 0));
        });
    add(registry, "java/lang/Class", "isInstance",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            auto object = arguments[1].as_reference();
            if (!mirror) return std::unexpected(mirror.error());
            if (!object) return std::unexpected(object.error());
            auto class_name = machine.mirrored_class_name(*mirror);
            if (!class_name) return std::unexpected(class_name.error());
            if (is_primitive_name(*class_name))
                return std::optional<Value>(Value::from_int(0));
            auto instance = machine.object_is_instance(*object, *class_name);
            if (!instance) return std::unexpected(instance.error());
            return std::optional<Value>(Value::from_int(*instance ? 1 : 0));
        });
    add(registry, "java/lang/Class", "isAssignableFrom",
        "(Ljava/lang/Class;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto target_mirror = receiver(arguments);
            auto source_mirror = arguments[1].as_reference();
            if (!target_mirror) return std::unexpected(target_mirror.error());
            if (!source_mirror || source_mirror->is_null())
                return fail_java("java/lang/NullPointerException",
                                 "Class.isAssignableFrom source is null");
            auto target = machine.mirrored_class_name(*target_mirror);
            auto source = machine.mirrored_class_name(*source_mirror);
            if (!target) return std::unexpected(target.error());
            if (!source) return std::unexpected(source.error());
            if (is_primitive_name(*target) || is_primitive_name(*source)) {
                return std::optional<Value>(Value::from_int(
                    *target == *source ? 1 : 0));
            }
            auto assignable = machine.classes().is_assignable(*source, *target);
            if (!assignable) return std::unexpected(assignable.error());
            return std::optional<Value>(Value::from_int(*assignable ? 1 : 0));
        });
    add(registry, "java/lang/Class", "getSuperclass",
        "()Ljava/lang/Class;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            if (!mirror) return std::unexpected(mirror.error());
            auto class_name = machine.mirrored_class_name(*mirror);
            if (!class_name) return std::unexpected(class_name.error());
            if (is_primitive_name(*class_name))
                return std::optional<Value>(Value::from_reference({}));
            std::string super_name;
            if (!class_name->empty() && class_name->front() == '[') {
                super_name = "java/lang/Object";
            } else {
                auto loaded = machine.classes().load(*class_name);
                if (!loaded) return std::unexpected(loaded.error());
                if (((*loaded)->access_flags() & kAccInterface) != 0U ||
                    (*loaded)->super_name().empty()) {
                    return std::optional<Value>(Value::from_reference({}));
                }
                super_name = (*loaded)->super_name();
            }
            auto super_mirror = machine.class_mirror(super_name);
            if (!super_mirror) return std::unexpected(super_mirror.error());
            return std::optional<Value>(Value::from_reference(*super_mirror));
        });
    add(registry, "java/lang/Class", "getComponentType",
        "()Ljava/lang/Class;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            if (!mirror) return std::unexpected(mirror.error());
            auto class_name = machine.mirrored_class_name(*mirror);
            if (!class_name) return std::unexpected(class_name.error());
            if (class_name->empty() || class_name->front() != '[')
                return std::optional<Value>(Value::from_reference({}));
            auto component = array_component_name(*class_name);
            if (!component) return std::unexpected(component.error());
            auto component_mirror = machine.class_mirror(*component);
            if (!component_mirror)
                return std::unexpected(component_mirror.error());
            return std::optional<Value>(
                Value::from_reference(*component_mirror));
        });
    add(registry, "java/lang/Class", "desiredAssertionStatus", "()Z",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            if (!mirror) return std::unexpected(mirror.error());
            return std::optional<Value>(Value::from_int(0));
        });
    add(registry, "java/lang/Class", "toString",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            if (!mirror) return std::unexpected(mirror.error());
            auto class_name = machine.mirrored_class_name(*mirror);
            if (!class_name) return std::unexpected(class_name.error());
            std::string text;
            if (is_primitive_name(*class_name)) {
                text = display_class_name(*class_name);
            } else {
                bool is_interface = false;
                if (class_name->empty() || class_name->front() != '[') {
                    auto loaded = machine.classes().load(*class_name);
                    if (!loaded) return std::unexpected(loaded.error());
                    is_interface =
                        ((*loaded)->access_flags() & kAccInterface) != 0U;
                }
                text = is_interface ? "interface " : "class ";
                text.append(display_class_name(*class_name));
            }
            auto string = create_string(machine, ascii_text(text));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
    add(registry, "java/lang/Class", "forName",
        "(Ljava/lang/String;)Ljava/lang/Class;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto name = arguments[0].as_reference();
            if (!name || name->is_null())
                return fail_java("java/lang/NullPointerException",
                                 "Class.forName name is null");
            auto encoded = utf8_text(machine, *name);
            if (!encoded) return std::unexpected(encoded.error());
            const std::string normalized = normalized_class_name(*encoded);
            if (normalized.empty())
                return fail_java("java/lang/ClassNotFoundException",
                                 "Class.forName name is empty");
            if (normalized.front() != '[') {
                auto loaded = machine.classes().load(normalized);
                if (!loaded) {
                    if (loaded.error().code == ErrorCode::class_not_found)
                        return fail_java("java/lang/ClassNotFoundException",
                                         loaded.error().message);
                    return std::unexpected(loaded.error());
                }
            }
            auto mirror = machine.class_mirror(normalized);
            if (!mirror) return std::unexpected(mirror.error());
            return std::optional<Value>(Value::from_reference(*mirror));
        });
    add(registry, "java/lang/Class", "getResourceAsStream",
        "(Ljava/lang/String;)Ljava/io/InputStream;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            auto name = arguments[1].as_reference();
            if (!mirror) return std::unexpected(mirror.error());
            if (!name || name->is_null())
                return fail_java("java/lang/NullPointerException",
                                 "resource name is null");
            auto stream = machine.open_class_resource_stream(*mirror, *name);
            if (!stream) return std::unexpected(stream.error());
            return std::optional<Value>(Value::from_reference(*stream));
        });

    add(registry, "java/lang/Class", "getResource",
        "(Ljava/lang/String;)Ljava/net/URL;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            if (!mirror) return std::unexpected(mirror.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "Class.getResource name is missing");
            }
            auto name = arguments[1].as_reference();
            if (!name || name->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "resource name is null");
            }
            const Value name_argument = Value::from_reference(*name);
            auto stream = machine.invoke_instance(
                *mirror, "java/lang/Class", "getResourceAsStream",
                "(Ljava/lang/String;)Ljava/io/InputStream;",
                std::span<const Value>(&name_argument, 1U));
            if (!stream) return std::unexpected(stream.error());
            if (stream->throwable.has_value()) {
                auto throwable = machine.heap().class_name(*stream->throwable);
                if (!throwable) return std::unexpected(throwable.error());
                return fail_java(*throwable,
                                 "Class.getResource lookup failed");
            }
            if (!stream->return_value.has_value()) {
                return fail(ErrorCode::internal_error,
                            "Class.getResourceAsStream returned no value");
            }
            auto stream_ref = stream->return_value->as_reference();
            if (!stream_ref) return std::unexpected(stream_ref.error());
            if (stream_ref->is_null()) {
                return std::optional<Value>(Value::from_reference({}));
            }
            auto url = machine.class_states().allocate_instance(
                machine.heap(), "java/net/URL");
            if (!url) return std::unexpected(url.error());
            auto owner_stored = machine.heap().set_field(
                *url, kUrlOwnerField, Value::from_reference(*mirror));
            auto name_stored = machine.heap().set_field(
                *url, kUrlNameField, Value::from_reference(*name));
            if (!owner_stored) return std::unexpected(owner_stored.error());
            if (!name_stored) return std::unexpected(name_stored.error());
            return std::optional<Value>(Value::from_reference(*url));
        });

    add(registry, "java/net/URL", "<init>",
        "(Ljava/lang/Class;Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto url = receiver(arguments);
            if (!url) return std::unexpected(url.error());
            if (arguments.size() < 3U) {
                return fail(ErrorCode::invalid_argument,
                            "resource URL constructor arguments are missing");
            }
            auto owner = arguments[1].as_reference();
            auto name = arguments[2].as_reference();
            if (!owner || owner->is_null() || !name || name->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "resource URL argument is null");
            }
            auto owner_stored = machine.heap().set_field(
                *url, kUrlOwnerField, Value::from_reference(*owner));
            auto name_stored = machine.heap().set_field(
                *url, kUrlNameField, Value::from_reference(*name));
            if (!owner_stored) return std::unexpected(owner_stored.error());
            if (!name_stored) return std::unexpected(name_stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/net/URL", "openStream", "()Ljava/io/InputStream;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto url = receiver(arguments);
            if (!url) return std::unexpected(url.error());
            auto owner_value = machine.heap().field(*url, kUrlOwnerField);
            auto name_value = machine.heap().field(*url, kUrlNameField);
            if (!owner_value) return std::unexpected(owner_value.error());
            if (!name_value) return std::unexpected(name_value.error());
            auto owner = owner_value->as_reference();
            auto name = name_value->as_reference();
            if (!owner) return std::unexpected(owner.error());
            if (!name) return std::unexpected(name.error());
            const Value name_argument = Value::from_reference(*name);
            auto stream = machine.invoke_instance(
                *owner, "java/lang/Class", "getResourceAsStream",
                "(Ljava/lang/String;)Ljava/io/InputStream;",
                std::span<const Value>(&name_argument, 1U));
            if (!stream) return std::unexpected(stream.error());
            if (stream->throwable.has_value()) {
                auto throwable = machine.heap().class_name(*stream->throwable);
                if (!throwable) return std::unexpected(throwable.error());
                return fail_java(*throwable, "URL.openStream failed");
            }
            return stream->return_value;
        });
    add(registry, "java/net/URL", "toURI", "()Ljava/net/URI;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto url = receiver(arguments);
            if (!url) return std::unexpected(url.error());
            auto name = machine.heap().field(*url, kUrlNameField);
            if (!name) return std::unexpected(name.error());
            auto path = name->as_reference();
            if (!path) return std::unexpected(path.error());
            auto uri = machine.class_states().allocate_instance(
                machine.heap(), "java/net/URI");
            if (!uri) return std::unexpected(uri.error());
            auto stored = machine.heap().set_field(
                *uri, kUriPathField, Value::from_reference(*path));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*uri));
        });
    add(registry, "java/net/URL", "toString", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto url = receiver(arguments);
            if (!url) return std::unexpected(url.error());
            auto name = machine.heap().field(*url, kUrlNameField);
            if (!name) return std::unexpected(name.error());
            return std::optional<Value>(*name);
        });
    add(registry, "java/net/URI", "<init>", "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto uri = receiver(arguments);
            if (!uri) return std::unexpected(uri.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "URI constructor path is missing");
            }
            auto path = arguments[1].as_reference();
            if (!path || path->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "URI path is null");
            }
            auto stored = machine.heap().set_field(
                *uri, kUriPathField, Value::from_reference(*path));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    const auto uri_path = [](Machine& machine,
                             std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto uri = receiver(arguments);
        if (!uri) return std::unexpected(uri.error());
        auto path = machine.heap().field(*uri, kUriPathField);
        if (!path) return std::unexpected(path.error());
        return std::optional<Value>(*path);
    };
    add(registry, "java/net/URI", "getPath", "()Ljava/lang/String;", uri_path);
    add(registry, "java/net/URI", "toString", "()Ljava/lang/String;", uri_path);

    add(registry, "java/lang/Class", "getMethod",
        "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            if (!mirror) return std::unexpected(mirror.error());
            if (arguments.size() < 3U) {
                return fail(ErrorCode::invalid_argument,
                            "Class.getMethod arguments are missing");
            }
            auto requested_name = arguments[1].as_reference();
            auto parameter_types = arguments[2].as_reference();
            if (!requested_name || requested_name->is_null() ||
                !parameter_types || parameter_types->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Class.getMethod argument is null");
            }
            auto name = utf8_text(machine, *requested_name);
            auto class_name = machine.mirrored_class_name(*mirror);
            if (!name) return std::unexpected(name.error());
            if (!class_name) return std::unexpected(class_name.error());

            auto parameter_count = machine.heap().array_length(*parameter_types);
            if (!parameter_count) return std::unexpected(parameter_count.error());
            std::string parameter_prefix("(");
            for (usize index = 0U; index < *parameter_count; ++index) {
                auto value = machine.heap().element(*parameter_types, index);
                if (!value) return std::unexpected(value.error());
                auto parameter_mirror = value->as_reference();
                if (!parameter_mirror || parameter_mirror->is_null()) {
                    return fail_java("java/lang/NullPointerException",
                                     "Class.getMethod parameter type is null");
                }
                auto parameter_name =
                    machine.mirrored_class_name(*parameter_mirror);
                if (!parameter_name) {
                    return std::unexpected(parameter_name.error());
                }
                parameter_prefix.append(
                    descriptor_for_class_name(*parameter_name));
            }
            parameter_prefix.push_back(')');

            std::vector<std::string> pending {*class_name};
            std::unordered_set<std::string> visited;
            std::shared_ptr<const classfile::ClassFile> declaring_class;
            const classfile::Method* matched = nullptr;
            while (!pending.empty() && matched == nullptr) {
                std::string current = std::move(pending.back());
                pending.pop_back();
                if (!visited.insert(current).second) continue;
                auto loaded = machine.classes().load(current);
                if (!loaded) return std::unexpected(loaded.error());
                for (const auto& candidate : (*loaded)->methods()) {
                    if (candidate.name != *name ||
                        (candidate.access_flags & kAccPublic) == 0U ||
                        !candidate.descriptor.starts_with(parameter_prefix)) {
                        continue;
                    }
                    matched = &candidate;
                    declaring_class = *loaded;
                    break;
                }
                if (matched != nullptr) break;
                if (!(*loaded)->super_name().empty()) {
                    pending.push_back((*loaded)->super_name());
                }
                for (const auto& interface_name : (*loaded)->interfaces()) {
                    pending.push_back(interface_name);
                }
            }
            if (matched == nullptr || !declaring_class) {
                return fail_java("java/lang/NoSuchMethodException", *name);
            }

            auto declaring_mirror =
                machine.class_mirror(declaring_class->name());
            if (!declaring_mirror) {
                return std::unexpected(declaring_mirror.error());
            }
            auto descriptor_string = create_string(
                machine, ascii_text(matched->descriptor));
            if (!descriptor_string) {
                return std::unexpected(descriptor_string.error());
            }
            auto descriptor_root = machine.pin_native_root(*descriptor_string);
            if (!descriptor_root) {
                return std::unexpected(descriptor_root.error());
            }
            auto method = machine.class_states().allocate_instance(
                machine.heap(), "java/lang/reflect/Method");
            if (!method) return std::unexpected(method.error());
            auto declaring_stored = machine.heap().set_field(
                *method, kReflectMethodDeclaringClassField,
                Value::from_reference(*declaring_mirror));
            auto name_stored = machine.heap().set_field(
                *method, kReflectMethodNameField,
                Value::from_reference(*requested_name));
            auto descriptor_stored = machine.heap().set_field(
                *method, kReflectMethodDescriptorField,
                Value::from_reference(*descriptor_string));
            auto modifiers_stored = machine.heap().set_field(
                *method, kReflectMethodModifiersField,
                Value::from_int(static_cast<i32>(matched->access_flags)));
            if (!declaring_stored) {
                return std::unexpected(declaring_stored.error());
            }
            if (!name_stored) return std::unexpected(name_stored.error());
            if (!descriptor_stored) {
                return std::unexpected(descriptor_stored.error());
            }
            if (!modifiers_stored) {
                return std::unexpected(modifiers_stored.error());
            }
            return std::optional<Value>(Value::from_reference(*method));
        });

    add(registry, "java/lang/Class", "getDeclaredField",
        "(Ljava/lang/String;)Ljava/lang/reflect/Field;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto mirror = receiver(arguments);
            if (!mirror) return std::unexpected(mirror.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "Class.getDeclaredField name is missing");
            }
            auto requested_name = arguments[1].as_reference();
            if (!requested_name || requested_name->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "field name is null");
            }
            auto class_name = machine.mirrored_class_name(*mirror);
            auto name = utf8_text(machine, *requested_name);
            if (!class_name) return std::unexpected(class_name.error());
            if (!name) return std::unexpected(name.error());
            auto loaded = machine.classes().load(*class_name);
            if (!loaded) return std::unexpected(loaded.error());
            const classfile::Field* matched = nullptr;
            for (const auto& candidate : (*loaded)->fields()) {
                if (candidate.name == *name) {
                    matched = &candidate;
                    break;
                }
            }
            if (matched == nullptr) {
                return fail_java("java/lang/NoSuchFieldException", *name);
            }
            auto descriptor_string = create_string(
                machine, ascii_text(matched->descriptor));
            if (!descriptor_string) {
                return std::unexpected(descriptor_string.error());
            }
            auto descriptor_root = machine.pin_native_root(*descriptor_string);
            if (!descriptor_root) {
                return std::unexpected(descriptor_root.error());
            }
            auto field = machine.class_states().allocate_instance(
                machine.heap(), "java/lang/reflect/Field");
            if (!field) return std::unexpected(field.error());
            auto declaring_stored = machine.heap().set_field(
                *field, kReflectFieldDeclaringClassField,
                Value::from_reference(*mirror));
            auto name_stored = machine.heap().set_field(
                *field, kReflectFieldNameField,
                Value::from_reference(*requested_name));
            auto descriptor_stored = machine.heap().set_field(
                *field, kReflectFieldDescriptorField,
                Value::from_reference(*descriptor_string));
            auto modifiers_stored = machine.heap().set_field(
                *field, kReflectFieldModifiersField,
                Value::from_int(static_cast<i32>(matched->access_flags)));
            if (!declaring_stored) {
                return std::unexpected(declaring_stored.error());
            }
            if (!name_stored) return std::unexpected(name_stored.error());
            if (!descriptor_stored) {
                return std::unexpected(descriptor_stored.error());
            }
            if (!modifiers_stored) {
                return std::unexpected(modifiers_stored.error());
            }
            return std::optional<Value>(Value::from_reference(*field));
        });

    add(registry, "java/lang/reflect/Field", "<init>",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto field = receiver(arguments);
            if (!field) return std::unexpected(field.error());
            if (arguments.size() < 5U) {
                return fail(ErrorCode::invalid_argument,
                            "Field constructor arguments are missing");
            }
            for (usize index = 1U; index <= 3U; ++index) {
                auto reference = arguments[index].as_reference();
                if (!reference || reference->is_null()) {
                    return fail_java("java/lang/NullPointerException",
                                     "Field constructor argument is null");
                }
                auto stored = machine.heap().set_field(
                    *field, index - 1U, Value::from_reference(*reference));
                if (!stored) return std::unexpected(stored.error());
            }
            auto modifiers = arguments[4].as_int();
            if (!modifiers) return std::unexpected(modifiers.error());
            auto stored = machine.heap().set_field(
                *field, kReflectFieldModifiersField,
                Value::from_int(*modifiers));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/lang/reflect/Field", "getModifiers", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto field = receiver(arguments);
            if (!field) return std::unexpected(field.error());
            auto modifiers = machine.heap().field(
                *field, kReflectFieldModifiersField);
            if (!modifiers) return std::unexpected(modifiers.error());
            return std::optional<Value>(*modifiers);
        });
    add(registry, "java/lang/reflect/Field", "getType",
        "()Ljava/lang/Class;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto field = receiver(arguments);
            if (!field) return std::unexpected(field.error());
            auto descriptor_value = machine.heap().field(
                *field, kReflectFieldDescriptorField);
            if (!descriptor_value) {
                return std::unexpected(descriptor_value.error());
            }
            auto descriptor_ref = descriptor_value->as_reference();
            if (!descriptor_ref) {
                return std::unexpected(descriptor_ref.error());
            }
            auto descriptor = utf8_text(machine, *descriptor_ref);
            if (!descriptor) return std::unexpected(descriptor.error());
            std::string type_name;
            if (descriptor->size() >= 2U && descriptor->front() == 'L' &&
                descriptor->back() == ';') {
                type_name.assign(descriptor->begin() + 1,
                                 descriptor->end() - 1);
            } else {
                type_name = *descriptor;
            }
            auto mirror = machine.class_mirror(type_name);
            if (!mirror) return std::unexpected(mirror.error());
            return std::optional<Value>(Value::from_reference(*mirror));
        });
    add(registry, "java/lang/reflect/Field", "get",
        "(Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto field = receiver(arguments);
            if (!field) return std::unexpected(field.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "Field.get target is missing");
            }
            auto declaring_value = machine.heap().field(
                *field, kReflectFieldDeclaringClassField);
            auto name_value = machine.heap().field(
                *field, kReflectFieldNameField);
            auto descriptor_value = machine.heap().field(
                *field, kReflectFieldDescriptorField);
            auto modifiers_value = machine.heap().field(
                *field, kReflectFieldModifiersField);
            if (!declaring_value) {
                return std::unexpected(declaring_value.error());
            }
            if (!name_value) return std::unexpected(name_value.error());
            if (!descriptor_value) {
                return std::unexpected(descriptor_value.error());
            }
            if (!modifiers_value) {
                return std::unexpected(modifiers_value.error());
            }
            auto declaring = declaring_value->as_reference();
            auto name_ref = name_value->as_reference();
            auto descriptor_ref = descriptor_value->as_reference();
            auto modifiers = modifiers_value->as_int();
            if (!declaring) return std::unexpected(declaring.error());
            if (!name_ref) return std::unexpected(name_ref.error());
            if (!descriptor_ref) {
                return std::unexpected(descriptor_ref.error());
            }
            if (!modifiers) return std::unexpected(modifiers.error());
            auto class_name = machine.mirrored_class_name(*declaring);
            auto name = utf8_text(machine, *name_ref);
            auto descriptor = utf8_text(machine, *descriptor_ref);
            if (!class_name) return std::unexpected(class_name.error());
            if (!name) return std::unexpected(name.error());
            if (!descriptor) return std::unexpected(descriptor.error());
            const bool is_static = (*modifiers & 0x0008) != 0;
            auto location = machine.class_states().resolve_field(
                *class_name, *name, *descriptor, is_static);
            if (!location) return std::unexpected(location.error());
            Result<Value> value = is_static
                ? machine.class_states().static_field(*location)
                : Result<Value>(fail(ErrorCode::invalid_state,
                                     "uninitialized reflection value"));
            if (!is_static) {
                auto target = arguments[1].as_reference();
                if (!target || target->is_null()) {
                    return fail_java("java/lang/NullPointerException",
                                     "instance Field.get target is null");
                }
                auto compatible = machine.object_is_instance(
                    *target, *class_name);
                if (!compatible) return std::unexpected(compatible.error());
                if (!*compatible) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Field.get target has wrong class");
                }
                value = machine.heap().field(*target, location->index);
            }
            if (!value) return std::unexpected(value.error());
            auto boxed = box_reflect_value(machine, *value, *descriptor);
            if (!boxed) return std::unexpected(boxed.error());
            return std::optional<Value>(Value::from_reference(*boxed));
        });
    add(registry, "java/lang/reflect/Method", "getModifiers", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto method = receiver(arguments);
            if (!method) return std::unexpected(method.error());
            auto modifiers = machine.heap().field(
                *method, kReflectMethodModifiersField);
            if (!modifiers) return std::unexpected(modifiers.error());
            return std::optional<Value>(*modifiers);
        });
    add(registry, "java/lang/reflect/Method", "invoke",
        "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto method = receiver(arguments);
            if (!method) return std::unexpected(method.error());
            if (arguments.size() < 3U) {
                return fail(ErrorCode::invalid_argument,
                            "Method.invoke arguments are missing");
            }
            auto declaring_value = machine.heap().field(
                *method, kReflectMethodDeclaringClassField);
            auto name_value = machine.heap().field(
                *method, kReflectMethodNameField);
            auto descriptor_value = machine.heap().field(
                *method, kReflectMethodDescriptorField);
            auto modifiers_value = machine.heap().field(
                *method, kReflectMethodModifiersField);
            if (!declaring_value) {
                return std::unexpected(declaring_value.error());
            }
            if (!name_value) return std::unexpected(name_value.error());
            if (!descriptor_value) {
                return std::unexpected(descriptor_value.error());
            }
            if (!modifiers_value) {
                return std::unexpected(modifiers_value.error());
            }
            auto declaring = declaring_value->as_reference();
            auto name_ref = name_value->as_reference();
            auto descriptor_ref = descriptor_value->as_reference();
            auto modifiers = modifiers_value->as_int();
            if (!declaring) return std::unexpected(declaring.error());
            if (!name_ref) return std::unexpected(name_ref.error());
            if (!descriptor_ref) {
                return std::unexpected(descriptor_ref.error());
            }
            if (!modifiers) return std::unexpected(modifiers.error());
            auto class_name = machine.mirrored_class_name(*declaring);
            auto name = utf8_text(machine, *name_ref);
            auto descriptor = utf8_text(machine, *descriptor_ref);
            if (!class_name) return std::unexpected(class_name.error());
            if (!name) return std::unexpected(name.error());
            if (!descriptor) return std::unexpected(descriptor.error());

            auto parameters = method_parameter_descriptors(*descriptor);
            if (!parameters) return std::unexpected(parameters.error());
            auto supplied_arguments = arguments[2].as_reference();
            if (!supplied_arguments) {
                return std::unexpected(supplied_arguments.error());
            }
            usize supplied_count = 0U;
            if (!supplied_arguments->is_null()) {
                auto length = machine.heap().array_length(*supplied_arguments);
                if (!length) return std::unexpected(length.error());
                supplied_count = *length;
            }
            if (supplied_count != parameters->size()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Method.invoke argument count mismatch");
            }

            std::vector<Value> call_arguments;
            call_arguments.reserve(parameters->size());
            for (usize index = 0U; index < parameters->size(); ++index) {
                auto supplied = machine.heap().element(
                    *supplied_arguments, index);
                if (!supplied) return std::unexpected(supplied.error());
                auto reference = supplied->as_reference();
                if (!reference) return std::unexpected(reference.error());
                auto converted = unbox_reflect_argument(
                    machine, *reference, (*parameters)[index]);
                if (!converted) return std::unexpected(converted.error());
                call_arguments.push_back(*converted);
            }

            Result<ExecutionResult> invoked = fail(
                ErrorCode::invalid_state, "reflection invocation not started");
            if ((*modifiers & kAccStatic) != 0) {
                invoked = machine.invoke_static(
                    *class_name, *name, *descriptor, call_arguments);
            } else {
                auto target = arguments[1].as_reference();
                if (!target) return std::unexpected(target.error());
                if (target->is_null()) {
                    return fail_java("java/lang/NullPointerException",
                                     "Method.invoke target is null");
                }
                auto compatible = machine.object_is_instance(
                    *target, *class_name);
                if (!compatible) return std::unexpected(compatible.error());
                if (!*compatible) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Method.invoke target has wrong class");
                }
                invoked = machine.invoke_instance(
                    *target, *class_name, *name, *descriptor, call_arguments);
            }
            if (!invoked) return std::unexpected(invoked.error());
            if (invoked->throwable.has_value()) {
                auto throwable = machine.heap().class_name(*invoked->throwable);
                if (!throwable) return std::unexpected(throwable.error());
                return fail_java(*throwable,
                                 "reflected method threw an exception");
            }
            auto return_descriptor = method_return_descriptor(*descriptor);
            if (!return_descriptor) {
                return std::unexpected(return_descriptor.error());
            }
            if (*return_descriptor == "V") {
                return std::optional<Value>(Value::from_reference({}));
            }
            if (!invoked->return_value.has_value()) {
                return fail(ErrorCode::internal_error,
                            "reflected method returned no value");
            }
            auto boxed = box_reflect_value(
                machine, *invoked->return_value, *return_descriptor);
            if (!boxed) return std::unexpected(boxed.error());
            return std::optional<Value>(Value::from_reference(*boxed));
        });

    add(registry, "java/lang/reflect/Modifier", "isStatic", "(I)Z",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Modifier.isStatic argument is missing");
            }
            auto modifiers = arguments[0].as_int();
            if (!modifiers) return std::unexpected(modifiers.error());
            return std::optional<Value>(
                Value::from_int((*modifiers & 0x0008) != 0 ? 1 : 0));
        });
}

} // namespace phoneme::vm
