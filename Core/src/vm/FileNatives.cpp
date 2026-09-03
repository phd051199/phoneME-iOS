#include "FileNatives.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "phoneme/filesystem/FileSystem.hpp"
#include "phoneme/security/PermissionPolicy.hpp"
#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/NativeMethodRegistry.hpp"

namespace phoneme::vm {
namespace {

constexpr usize kJavaFilePathField = 0;
constexpr usize kFileStreamHandleField = 0;
constexpr usize kFileStreamClosedField = 1;
constexpr usize kConnectionPathField = 0;
constexpr usize kConnectionModeField = 1;
constexpr usize kConnectionOpenField = 2;
constexpr usize kConnectionInputField = 3;
constexpr usize kConnectionOutputField = 4;
constexpr usize kFilterStreamField = 0;
constexpr usize kArrayEnumerationValuesField = 0;
constexpr usize kArrayEnumerationIndexField = 1;
constexpr usize kArrayEnumerationSizeField = 2;
constexpr i32 kConnectorRead = 1;
constexpr i32 kConnectorWrite = 2;
constexpr i32 kConnectorReadWrite = 3;

[[nodiscard]] Status require_file_permissions(
    Machine& machine,
    bool read,
    bool write,
    std::string_view resource) {
    if (read) {
        auto allowed = machine.permission_policy().require(
            security::permissions::connector_file_read,
            std::string(resource));
        if (!allowed) return allowed;
    }
    if (write) {
        auto allowed = machine.permission_policy().require(
            security::permissions::connector_file_write,
            std::string(resource));
        if (!allowed) return allowed;
    }
    return {};
}

void add(NativeMethodRegistry& registry,
         std::string owner,
         std::string name,
         std::string descriptor,
         NativeMethod method) {
    auto registered = registry.register_method(std::move(owner),
                                               std::move(name),
                                               std::move(descriptor),
                                               std::move(method));
    if (!registered) std::terminate();
}

[[nodiscard]] Result<ObjectRef> receiver(std::span<const Value> arguments) {
    if (arguments.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "file native has no receiver");
    }
    auto object = arguments.front().as_reference();
    if (!object || object->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "file native receiver is null");
    }
    return *object;
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

[[nodiscard]] Result<std::u16string> utf16_text(std::string_view text) {
    std::u16string result;
    result.reserve(text.size());
    for (usize index = 0; index < text.size();) {
        const u8 first = static_cast<u8>(text[index]);
        u32 code_point = 0;
        usize count = 0;
        if (first <= 0x7FU) {
            code_point = first;
            count = 1;
        } else if ((first & 0xE0U) == 0xC0U) {
            code_point = first & 0x1FU;
            count = 2;
        } else if ((first & 0xF0U) == 0xE0U) {
            code_point = first & 0x0FU;
            count = 3;
        } else if ((first & 0xF8U) == 0xF0U) {
            code_point = first & 0x07U;
            count = 4;
        } else {
            return fail(ErrorCode::invalid_argument,
                        "filesystem returned invalid UTF-8");
        }
        if (index + count > text.size()) {
            return fail(ErrorCode::invalid_argument,
                        "filesystem returned truncated UTF-8");
        }
        for (usize continuation = 1; continuation < count; ++continuation) {
            const u8 byte = static_cast<u8>(text[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                return fail(ErrorCode::invalid_argument,
                            "filesystem returned invalid UTF-8 continuation");
            }
            code_point = (code_point << 6U) | (byte & 0x3FU);
        }
        if (code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return fail(ErrorCode::invalid_argument,
                        "filesystem returned invalid Unicode scalar");
        }
        if (code_point <= 0xFFFFU) {
            result.push_back(static_cast<char16_t>(code_point));
        } else {
            code_point -= 0x10000U;
            result.push_back(static_cast<char16_t>(
                0xD800U | (code_point >> 10U)));
            result.push_back(static_cast<char16_t>(
                0xDC00U | (code_point & 0x3FFU)));
        }
        index += count;
    }
    return result;
}

[[nodiscard]] Result<ObjectRef> create_string(Machine& machine,
                                              std::string_view text) {
    auto decoded = utf16_text(text);
    if (!decoded) return std::unexpected(decoded.error());
    auto object = machine.class_states().allocate_instance(
        machine.heap(), "java/lang/String");
    if (!object) return std::unexpected(object.error());
    auto attached = machine.heap().attach_string(*object, std::move(*decoded));
    if (!attached) return std::unexpected(attached.error());
    return *object;
}

[[nodiscard]] std::unexpected<Error> file_error(Error error,
                                                 bool opening = false) {
    if (error.code == ErrorCode::invalid_argument) {
        return fail_java("java/lang/SecurityException",
                         std::move(error.message));
    }
    return fail_java(opening ? "java/io/FileNotFoundException"
                             : "java/io/IOException",
                     std::move(error.message));
}

[[nodiscard]] Result<usize> byte_array_length(Machine& machine,
                                              ObjectRef array) {
    if (array.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "byte array is null");
    }
    auto class_name = machine.heap().class_name(array);
    if (!class_name || *class_name != "[B") {
        return fail_java("java/lang/IllegalArgumentException",
                         "value is not byte[]");
    }
    return machine.heap().array_length(array);
}

[[nodiscard]] Status validate_range(usize size, i32 offset, i32 length) {
    if (offset < 0 || length < 0 ||
        static_cast<usize>(offset) > size ||
        static_cast<usize>(length) > size - static_cast<usize>(offset)) {
        return fail_java("java/lang/IndexOutOfBoundsException",
                         "byte array range is invalid");
    }
    return {};
}

[[nodiscard]] Result<std::vector<u8>> read_byte_array(
    Machine& machine,
    ObjectRef array,
    i32 offset,
    i32 length) {
    auto size = byte_array_length(machine, array);
    if (!size) return std::unexpected(size.error());
    auto valid = validate_range(*size, offset, length);
    if (!valid) return std::unexpected(valid.error());
    std::vector<u8> bytes;
    bytes.reserve(static_cast<usize>(length));
    for (i32 index = 0; index < length; ++index) {
        auto value = machine.heap().element(
            array, static_cast<usize>(offset + index));
        if (!value) return std::unexpected(value.error());
        auto integer = value->as_int();
        if (!integer) return std::unexpected(integer.error());
        bytes.push_back(static_cast<u8>(static_cast<i8>(*integer)));
    }
    return bytes;
}

[[nodiscard]] Status write_byte_array(Machine& machine,
                                      ObjectRef array,
                                      i32 offset,
                                      std::span<const u8> bytes) {
    auto size = byte_array_length(machine, array);
    if (!size) return std::unexpected(size.error());
    if (bytes.size() > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return fail_java("java/lang/OutOfMemoryError",
                         "native file read exceeds Java array range");
    }
    auto valid = validate_range(*size, offset,
                                static_cast<i32>(bytes.size()));
    if (!valid) return valid;
    for (usize index = 0; index < bytes.size(); ++index) {
        auto stored = machine.heap().set_element(
            array, static_cast<usize>(offset) + index,
            Value::from_int(static_cast<i32>(
                static_cast<i8>(bytes[index]))));
        if (!stored) return stored;
    }
    return {};
}

[[nodiscard]] Result<i32> stream_handle(Machine& machine,
                                        ObjectRef stream) {
    auto closed = int_field(machine, stream, kFileStreamClosedField);
    if (!closed) return std::unexpected(closed.error());
    if (*closed != 0) {
        return fail_java("java/io/IOException", "file stream is closed");
    }
    return int_field(machine, stream, kFileStreamHandleField);
}

[[nodiscard]] Status initialize_stream(Machine& machine,
                                       ObjectRef stream,
                                       i32 handle) {
    auto stored_handle = set_int_field(machine, stream,
                                       kFileStreamHandleField, handle);
    if (!stored_handle) return stored_handle;
    return set_int_field(machine, stream, kFileStreamClosedField, 0);
}

[[nodiscard]] Result<bool> stream_is_open(Machine& machine,
                                          ObjectRef stream) {
    if (stream.is_null()) return false;
    auto closed = int_field(machine, stream, kFileStreamClosedField);
    if (!closed) return std::unexpected(closed.error());
    return *closed == 0;
}

[[nodiscard]] Status register_connection_stream(
    Machine& machine,
    ObjectRef connection,
    ObjectRef stream,
    bool input) {
    const usize field = input ? kConnectionInputField
                              : kConnectionOutputField;
    auto existing = reference_field(machine, connection, field);
    if (!existing) return std::unexpected(existing.error());
    auto open = stream_is_open(machine, *existing);
    if (!open) return std::unexpected(open.error());
    if (*open) {
        return fail_java("java/io/IOException",
                         input
                             ? "FileConnection already has an open input stream"
                             : "FileConnection already has an open output stream");
    }
    return set_reference_field(machine, connection, field, stream);
}

[[nodiscard]] Status close_connection_stream(
    Machine& machine,
    ObjectRef connection,
    bool input) {
    const usize field = input ? kConnectionInputField
                              : kConnectionOutputField;
    auto stream = reference_field(machine, connection, field);
    if (!stream) return std::unexpected(stream.error());
    if (stream->is_null()) return {};
    auto open = stream_is_open(machine, *stream);
    if (!open) return std::unexpected(open.error());
    if (*open) {
        auto closed = input ? file_input_close(machine, *stream)
                            : file_output_close(machine, *stream);
        if (!closed) return closed;
    }
    return set_reference_field(machine, connection, field, {});
}

[[nodiscard]] Status close_connection_streams(Machine& machine,
                                              ObjectRef connection) {
    auto input = close_connection_stream(machine, connection, true);
    auto output = close_connection_stream(machine, connection, false);
    if (!input) return input;
    return output;
}

[[nodiscard]] Status flush_connection_output(Machine& machine,
                                             ObjectRef connection) {
    auto stream = reference_field(machine, connection,
                                  kConnectionOutputField);
    if (!stream) return std::unexpected(stream.error());
    if (stream->is_null()) return {};
    auto open = stream_is_open(machine, *stream);
    if (!open) return std::unexpected(open.error());
    return *open ? file_output_flush(machine, *stream) : Status {};
}

[[nodiscard]] Result<std::string> normalize_path_argument(
    Machine& machine,
    ObjectRef path) {
    auto text = utf8_text(machine, path);
    if (!text) return std::unexpected(text.error());
    if (text->starts_with("file:")) {
        return filesystem::path_from_file_url(*text);
    }
    return filesystem::normalize_virtual_path(*text);
}

[[nodiscard]] Result<ObjectRef> create_file_input_stream(
    Machine& machine,
    std::string_view path,
    std::optional<ObjectRef> connection = std::nullopt) {
    auto handle = machine.filesystem().open(path,
                                            filesystem::OpenMode::read,
                                            false, false);
    if (!handle) return file_error(handle.error());
    auto stream = machine.class_states().allocate_instance(
        machine.heap(), "java/io/FileInputStream");
    if (!stream) {
        static_cast<void>(machine.filesystem().close(*handle));
        return std::unexpected(stream.error());
    }
    auto initialized = initialize_stream(machine, *stream, *handle);
    if (!initialized) {
        static_cast<void>(machine.filesystem().close(*handle));
        return std::unexpected(initialized.error());
    }
    if (connection.has_value()) {
        auto registered = register_connection_stream(
            machine, *connection, *stream, true);
        if (!registered) {
            static_cast<void>(file_input_close(machine, *stream));
            return std::unexpected(registered.error());
        }
    }
    return *stream;
}

[[nodiscard]] Result<ObjectRef> create_file_output_stream(
    Machine& machine,
    std::string_view path,
    std::optional<i64> offset = std::nullopt,
    std::optional<ObjectRef> connection = std::nullopt) {
    if (offset.has_value() && *offset < 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         "output stream offset is negative");
    }
    auto handle = machine.filesystem().open(
        path, filesystem::OpenMode::read_write, false, false);
    if (!handle) return file_error(handle.error());
    if (offset.has_value()) {
        auto size = machine.filesystem().size(*handle);
        if (!size) {
            static_cast<void>(machine.filesystem().close(*handle));
            return file_error(size.error());
        }
        const i64 position = std::min(*offset, *size);
        auto positioned = machine.filesystem().seek(
            *handle, position, filesystem::SeekOrigin::begin);
        if (!positioned) {
            static_cast<void>(machine.filesystem().close(*handle));
            return file_error(positioned.error());
        }
    } else {
        auto positioned = machine.filesystem().seek(
            *handle, 0, filesystem::SeekOrigin::begin);
        if (!positioned) {
            static_cast<void>(machine.filesystem().close(*handle));
            return file_error(positioned.error());
        }
    }
    auto stream = machine.class_states().allocate_instance(
        machine.heap(), "java/io/FileOutputStream");
    if (!stream) {
        static_cast<void>(machine.filesystem().close(*handle));
        return std::unexpected(stream.error());
    }
    auto initialized = initialize_stream(machine, *stream, *handle);
    if (!initialized) {
        static_cast<void>(machine.filesystem().close(*handle));
        return std::unexpected(initialized.error());
    }
    if (connection.has_value()) {
        auto registered = register_connection_stream(
            machine, *connection, *stream, false);
        if (!registered) {
            static_cast<void>(file_output_close(machine, *stream));
            return std::unexpected(registered.error());
        }
    }
    return *stream;
}

[[nodiscard]] Result<ObjectRef> wrap_data_input(Machine& machine,
                                                ObjectRef input) {
    auto stream = machine.class_states().allocate_instance(
        machine.heap(), "java/io/DataInputStream");
    if (!stream) return std::unexpected(stream.error());
    auto stored = set_reference_field(machine, *stream,
                                      kFilterStreamField, input);
    if (!stored) return std::unexpected(stored.error());
    return *stream;
}

[[nodiscard]] Result<ObjectRef> wrap_data_output(Machine& machine,
                                                 ObjectRef output) {
    auto stream = machine.class_states().allocate_instance(
        machine.heap(), "java/io/DataOutputStream");
    if (!stream) return std::unexpected(stream.error());
    auto stored = set_reference_field(machine, *stream,
                                      kFilterStreamField, output);
    if (!stored) return std::unexpected(stored.error());
    return *stream;
}

[[nodiscard]] std::string file_resource_uri(std::string_view path);

struct ConnectionState final {
    ObjectRef object;
    std::string path;
    i32 mode {0};
};

[[nodiscard]] Result<ConnectionState> connection_state(
    Machine& machine,
    std::span<const Value> arguments,
    bool require_read = false,
    bool require_write = false) {
    auto object = receiver(arguments);
    if (!object) return std::unexpected(object.error());
    auto open = int_field(machine, *object, kConnectionOpenField);
    auto mode = int_field(machine, *object, kConnectionModeField);
    auto path_object = reference_field(machine, *object, kConnectionPathField);
    if (!open || !mode || !path_object) {
        return fail(ErrorCode::invalid_state,
                    "FileConnection state is invalid");
    }
    if (*open == 0) {
        return fail_java(
            "javax/microedition/io/file/ConnectionClosedException",
            "FileConnection is closed");
    }
    if (require_read && *mode != kConnectorRead &&
        *mode != kConnectorReadWrite) {
        return fail_java(
            "javax/microedition/io/file/IllegalModeException",
            "FileConnection is not open for reading");
    }
    if (require_write && *mode != kConnectorWrite &&
        *mode != kConnectorReadWrite) {
        return fail_java(
            "javax/microedition/io/file/IllegalModeException",
            "FileConnection is not open for writing");
    }
    auto path = utf8_text(machine, *path_object);
    if (!path) return std::unexpected(path.error());
    auto permitted = require_file_permissions(
        machine, require_read, require_write, file_resource_uri(*path));
    if (!permitted) return std::unexpected(permitted.error());
    return ConnectionState {
        .object = *object,
        .path = std::move(*path),
        .mode = *mode,
    };
}

[[nodiscard]] Result<ObjectRef> create_connection(Machine& machine,
                                                  ObjectRef url,
                                                  i32 mode) {
    if (mode != kConnectorRead && mode != kConnectorWrite &&
        mode != kConnectorReadWrite) {
        return fail_java("java/lang/IllegalArgumentException",
                         "Connector mode must be READ, WRITE, or READ_WRITE");
    }
    auto text = utf8_text(machine, url);
    if (!text) return std::unexpected(text.error());
    auto path = filesystem::path_from_file_url(*text);
    if (!path) return file_error(path.error());
    auto permitted = require_file_permissions(
        machine,
        mode == kConnectorRead || mode == kConnectorReadWrite,
        mode == kConnectorWrite || mode == kConnectorReadWrite,
        file_resource_uri(*path));
    if (!permitted) return std::unexpected(permitted.error());
    auto path_string = create_string(machine, *path);
    if (!path_string) return std::unexpected(path_string.error());
    auto connection = machine.class_states().allocate_instance(
        machine.heap(), "javax/microedition/io/file/FileConnectionImpl");
    if (!connection) return std::unexpected(connection.error());
    auto stored_path = set_reference_field(machine, *connection,
                                           kConnectionPathField,
                                           *path_string);
    auto stored_mode = set_int_field(machine, *connection,
                                     kConnectionModeField, mode);
    auto stored_open = set_int_field(machine, *connection,
                                     kConnectionOpenField, 1);
    auto stored_input = set_reference_field(machine, *connection,
                                            kConnectionInputField, {});
    auto stored_output = set_reference_field(machine, *connection,
                                             kConnectionOutputField, {});
    if (!stored_path) return std::unexpected(stored_path.error());
    if (!stored_mode) return std::unexpected(stored_mode.error());
    if (!stored_open) return std::unexpected(stored_open.error());
    if (!stored_input) return std::unexpected(stored_input.error());
    if (!stored_output) return std::unexpected(stored_output.error());
    return *connection;
}

[[nodiscard]] bool wildcard_match(std::string_view pattern,
                                  std::string_view text) noexcept {
    usize pattern_index = 0;
    usize text_index = 0;
    usize star = std::string_view::npos;
    usize checkpoint = 0;
    while (text_index < text.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == '?' ||
             pattern[pattern_index] == text[text_index])) {
            ++pattern_index;
            ++text_index;
        } else if (pattern_index < pattern.size() &&
                   pattern[pattern_index] == '*') {
            star = pattern_index++;
            checkpoint = text_index;
        } else if (star != std::string_view::npos) {
            pattern_index = star + 1U;
            text_index = ++checkpoint;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

[[nodiscard]] Result<ObjectRef> create_enumeration(
    Machine& machine,
    const std::vector<std::string>& names) {
    auto values = machine.heap().allocate_array(
        "[Ljava/lang/Object;", names.size(), Value::from_reference({}));
    if (!values) return std::unexpected(values.error());
    for (usize index = 0; index < names.size(); ++index) {
        auto name = create_string(machine, names[index]);
        if (!name) return std::unexpected(name.error());
        auto stored = machine.heap().set_element(
            *values, index, Value::from_reference(*name));
        if (!stored) return std::unexpected(stored.error());
    }
    auto enumeration = machine.class_states().allocate_instance(
        machine.heap(), "java/util/ArrayEnumeration");
    if (!enumeration) return std::unexpected(enumeration.error());
    auto stored_values = set_reference_field(machine, *enumeration,
                                              kArrayEnumerationValuesField,
                                              *values);
    auto stored_index = set_int_field(machine, *enumeration,
                                      kArrayEnumerationIndexField, 0);
    auto stored_size = set_int_field(machine, *enumeration,
                                     kArrayEnumerationSizeField,
                                     static_cast<i32>(names.size()));
    if (!stored_values) return std::unexpected(stored_values.error());
    if (!stored_index) return std::unexpected(stored_index.error());
    if (!stored_size) return std::unexpected(stored_size.error());
    return *enumeration;
}

[[nodiscard]] std::string parent_path(std::string_view path) {
    const usize slash = path.rfind('/');
    return slash == std::string_view::npos
               ? std::string {}
               : std::string(path.substr(0, slash));
}

[[nodiscard]] std::string leaf_name(std::string_view path) {
    const usize slash = path.rfind('/');
    return std::string(path.substr(slash == std::string_view::npos
                                       ? 0U
                                       : slash + 1U));
}

[[nodiscard]] std::string file_resource_uri(std::string_view path) {
    std::string uri = "file:///";
    uri.append(path);
    return uri;
}

[[nodiscard]] i64 clamp_java_long_size(u64 value) noexcept {
    const auto maximum = static_cast<u64>(std::numeric_limits<i64>::max());
    return static_cast<i64>(std::min(value, maximum));
}

[[nodiscard]] Result<ObjectRef> java_file_path_object(
    Machine& machine,
    ObjectRef file) {
    auto path = reference_field(machine, file, kJavaFilePathField);
    if (!path) return std::unexpected(path.error());
    if (path->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "java.io.File path is not initialized");
    }
    return *path;
}

[[nodiscard]] Result<std::string> normalized_java_file_path(
    Machine& machine,
    ObjectRef file) {
    auto path = java_file_path_object(machine, file);
    if (!path) return std::unexpected(path.error());
    return normalize_path_argument(machine, *path);
}

[[nodiscard]] Status initialize_file_input_stream(
    Machine& machine,
    ObjectRef stream,
    ObjectRef path_object) {
    auto path = normalize_path_argument(machine, path_object);
    if (!path) return file_error(path.error());
    auto permitted = require_file_permissions(
        machine, true, false, file_resource_uri(*path));
    if (!permitted) return permitted;
    auto handle = machine.filesystem().open(
        *path, filesystem::OpenMode::read, false, false);
    if (!handle) return file_error(handle.error(), true);
    auto initialized = initialize_stream(machine, stream, *handle);
    if (!initialized) {
        static_cast<void>(machine.filesystem().close(*handle));
        return initialized;
    }
    return {};
}

[[nodiscard]] Status initialize_file_output_stream(
    Machine& machine,
    ObjectRef stream,
    ObjectRef path_object,
    bool append) {
    auto path = normalize_path_argument(machine, path_object);
    if (!path) return file_error(path.error());
    auto permitted = require_file_permissions(
        machine, false, true, file_resource_uri(*path));
    if (!permitted) return permitted;
    auto handle = machine.filesystem().open(
        *path,
        append ? filesystem::OpenMode::append
               : filesystem::OpenMode::write,
        true,
        !append);
    if (!handle) return file_error(handle.error(), true);
    auto initialized = initialize_stream(machine, stream, *handle);
    if (!initialized) {
        static_cast<void>(machine.filesystem().close(*handle));
        return initialized;
    }
    return {};
}

void register_java_file_natives(NativeMethodRegistry& registry) {
    add(registry, "java/io/File", "<init>",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "File constructor path is missing");
            }
            auto path = arguments[1].as_reference();
            if (!path || path->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "File path is null");
            }
            auto stored = set_reference_field(
                machine, *object, kJavaFilePathField, *path);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    const auto path_value = [](Machine& machine,
                               std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto object = receiver(arguments);
        if (!object) return std::unexpected(object.error());
        auto path = java_file_path_object(machine, *object);
        if (!path) return std::unexpected(path.error());
        return std::optional<Value>(Value::from_reference(*path));
    };
    add(registry, "java/io/File", "getPath", "()Ljava/lang/String;",
        path_value);
    add(registry, "java/io/File", "toString", "()Ljava/lang/String;",
        path_value);
    add(registry, "java/io/File", "getAbsolutePath", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto path = normalized_java_file_path(machine, *object);
            if (!path) return std::unexpected(path.error());

            // java.io.File resolves relative paths against the process working
            // directory. phoneME deliberately exposes a sandboxed virtual
            // filesystem instead, whose root is the Java process root. Return
            // a stable absolute-looking path rooted at that virtual root so
            // equality/canonicalisation code written for Java SE behaves as
            // expected without leaking the host filesystem path.
            std::string absolute = "/";
            absolute.append(*path);
            auto string = create_string(machine, absolute);
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });

    const auto path_component = [&registry](const char* name, bool parent) {
        add(registry, "java/io/File", name, "()Ljava/lang/String;",
            [parent](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                auto path = normalized_java_file_path(machine, *object);
                if (!path) return std::unexpected(path.error());
                const std::string value = parent
                    ? parent_path(*path)
                    : leaf_name(*path);
                if (parent && value.empty()) {
                    return std::optional<Value>(Value::from_reference({}));
                }
                auto string = create_string(machine, value);
                if (!string) return std::unexpected(string.error());
                return std::optional<Value>(Value::from_reference(*string));
            });
    };
    path_component("getName", false);
    path_component("getParent", true);

    add(registry, "java/io/File", "getParentFile", "()Ljava/io/File;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto path = normalized_java_file_path(machine, *object);
            if (!path) return std::unexpected(path.error());
            const std::string parent = parent_path(*path);
            if (parent.empty()) {
                return std::optional<Value>(Value::from_reference({}));
            }
            auto path_string = create_string(machine, parent);
            if (!path_string) return std::unexpected(path_string.error());
            auto path_root = machine.pin_native_root(*path_string);
            if (!path_root) return std::unexpected(path_root.error());
            auto file = machine.class_states().allocate_instance(
                machine.heap(), "java/io/File");
            if (!file) return std::unexpected(file.error());
            auto stored = set_reference_field(machine, *file,
                                               kJavaFilePathField,
                                               *path_string);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*file));
        });

    add(registry, "java/io/File", "isAbsolute", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto path_object = java_file_path_object(machine, *object);
            if (!path_object) return std::unexpected(path_object.error());
            auto path = utf8_text(machine, *path_object);
            if (!path) return std::unexpected(path.error());
            const bool absolute = path->starts_with('/') ||
                                  path->starts_with("file:/");
            return std::optional<Value>(
                Value::from_int(absolute ? 1 : 0));
        });

    const auto stat_boolean = [&registry](const char* name, int property) {
        add(registry, "java/io/File", name, "()Z",
            [property](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                auto path = normalized_java_file_path(machine, *object);
                if (!path) return std::unexpected(path.error());
                auto permitted = require_file_permissions(
                    machine, true, false, file_resource_uri(*path));
                if (!permitted) return std::unexpected(permitted.error());
                auto info = machine.filesystem().stat(*path);
                if (!info) return file_error(info.error());
                const bool value = property == 0
                    ? info->exists
                    : (property == 1
                        ? info->exists && info->directory
                        : info->exists && !info->directory);
                return std::optional<Value>(
                    Value::from_int(value ? 1 : 0));
            });
    };
    stat_boolean("exists", 0);
    stat_boolean("isDirectory", 1);
    stat_boolean("isFile", 2);

    add(registry, "java/io/File", "length", "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto path = normalized_java_file_path(machine, *object);
            if (!path) return std::unexpected(path.error());
            auto permitted = require_file_permissions(
                machine, true, false, file_resource_uri(*path));
            if (!permitted) return std::unexpected(permitted.error());
            auto info = machine.filesystem().stat(*path);
            if (!info) return file_error(info.error());
            const i64 size = info->exists && !info->directory
                ? clamp_java_long_size(info->size)
                : 0;
            return std::optional<Value>(Value::from_long(size));
        });
    add(registry, "java/io/File", "lastModified", "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto path = normalized_java_file_path(machine, *object);
            if (!path) return std::unexpected(path.error());
            auto permitted = require_file_permissions(
                machine, true, false, file_resource_uri(*path));
            if (!permitted) return std::unexpected(permitted.error());
            auto info = machine.filesystem().stat(*path);
            if (!info) return file_error(info.error());
            if (!info->exists) {
                return std::optional<Value>(Value::from_long(0));
            }
            const i64 seconds = info->modified_seconds;
            const i64 millis = seconds > std::numeric_limits<i64>::max() / 1000
                ? std::numeric_limits<i64>::max()
                : (seconds < std::numeric_limits<i64>::min() / 1000
                    ? std::numeric_limits<i64>::min()
                    : seconds * 1000);
            return std::optional<Value>(Value::from_long(millis));
        });

    const auto mutate = [&registry](const char* name, bool directory) {
        add(registry, "java/io/File", name, "()Z",
            [directory](Machine& machine,
                        std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                auto path = normalized_java_file_path(machine, *object);
                if (!path) return std::unexpected(path.error());
                auto permitted = require_file_permissions(
                    machine, false, true, file_resource_uri(*path));
                if (!permitted) return std::unexpected(permitted.error());
                Status changed = directory
                    ? machine.filesystem().create_directory(*path)
                    : machine.filesystem().remove(*path, false);
                return std::optional<Value>(
                    Value::from_int(changed ? 1 : 0));
            });
    };
    mutate("mkdir", true);
    mutate("delete", false);

    add(registry, "java/io/File", "mkdirs", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto path = normalized_java_file_path(machine, *object);
            if (!path) return std::unexpected(path.error());
            auto permitted = require_file_permissions(
                machine, false, true, file_resource_uri(*path));
            if (!permitted) return std::unexpected(permitted.error());
            auto target_info = machine.filesystem().stat(*path);
            if (!target_info) return file_error(target_info.error());
            if (target_info->exists) {
                return std::optional<Value>(Value::from_int(0));
            }

            bool created_any = false;
            std::string current;
            usize cursor = 0U;
            if (!path->empty() && path->front() == '/') {
                current = "/";
                cursor = 1U;
            }
            while (cursor <= path->size()) {
                const usize slash = path->find('/', cursor);
                const usize end = slash == std::string::npos
                    ? path->size() : slash;
                const std::string_view component(path->data() + cursor,
                                                 end - cursor);
                if (!component.empty()) {
                    if (!current.empty() && current.back() != '/') {
                        current.push_back('/');
                    }
                    current.append(component);
                    auto info = machine.filesystem().stat(current);
                    if (!info) return file_error(info.error());
                    if (info->exists) {
                        if (!info->directory) {
                            return std::optional<Value>(Value::from_int(0));
                        }
                    } else {
                        auto created = machine.filesystem().create_directory(current);
                        if (!created) {
                            return std::optional<Value>(Value::from_int(0));
                        }
                        created_any = true;
                    }
                }
                if (slash == std::string::npos) break;
                cursor = slash + 1U;
            }
            return std::optional<Value>(
                Value::from_int(created_any ? 1 : 0));
        });

    add(registry, "java/io/File", "listFiles", "()[Ljava/io/File;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto path = normalized_java_file_path(machine, *object);
            if (!path) return std::unexpected(path.error());
            auto permitted = require_file_permissions(
                machine, true, false, file_resource_uri(*path));
            if (!permitted) return std::unexpected(permitted.error());
            auto info = machine.filesystem().stat(*path);
            if (!info) return file_error(info.error());
            if (!info->exists || !info->directory) {
                return std::optional<Value>(Value::from_reference({}));
            }
            auto names = machine.filesystem().list(*path);
            if (!names) return file_error(names.error());
            auto files = machine.heap().allocate_array(
                "[Ljava/io/File;", names->size(), Value::from_reference({}));
            if (!files) return std::unexpected(files.error());
            auto files_root = machine.pin_native_root(*files);
            if (!files_root) return std::unexpected(files_root.error());
            for (usize index = 0U; index < names->size(); ++index) {
                std::string child = *path;
                if (!child.empty() && child.back() != '/') child.push_back('/');
                child.append((*names)[index]);
                auto path_string = create_string(machine, child);
                if (!path_string) return std::unexpected(path_string.error());
                auto path_root = machine.pin_native_root(*path_string);
                if (!path_root) return std::unexpected(path_root.error());
                auto file = machine.class_states().allocate_instance(
                    machine.heap(), "java/io/File");
                if (!file) return std::unexpected(file.error());
                auto stored_path = set_reference_field(
                    machine, *file, kJavaFilePathField, *path_string);
                if (!stored_path) return std::unexpected(stored_path.error());
                auto stored = machine.heap().set_element(
                    *files, index, Value::from_reference(*file));
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value>(Value::from_reference(*files));
        });

    add(registry, "java/io/File", "renameTo", "(Ljava/io/File;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto source = receiver(arguments);
            if (!source) return std::unexpected(source.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "File.renameTo target is missing");
            }
            auto target = arguments[1].as_reference();
            if (!target || target->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "File.renameTo target is null");
            }
            auto from = normalized_java_file_path(machine, *source);
            auto to = normalized_java_file_path(machine, *target);
            if (!from) return std::unexpected(from.error());
            if (!to) return std::unexpected(to.error());
            auto permitted_from = require_file_permissions(
                machine, false, true, file_resource_uri(*from));
            auto permitted_to = require_file_permissions(
                machine, false, true, file_resource_uri(*to));
            if (!permitted_from) return std::unexpected(permitted_from.error());
            if (!permitted_to) return std::unexpected(permitted_to.error());
            auto renamed = machine.filesystem().rename(*from, *to);
            return std::optional<Value>(
                Value::from_int(renamed ? 1 : 0));
        });
}

void register_file_stream_natives(NativeMethodRegistry& registry) {
    const auto input_constructor = [&registry](bool file_argument) {
        add(registry, "java/io/FileInputStream", "<init>",
            file_argument ? "(Ljava/io/File;)V"
                          : "(Ljava/lang/String;)V",
            [file_argument](Machine& machine,
                            std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                if (arguments.size() < 2U) {
                    return fail(ErrorCode::invalid_argument,
                                "file input path is missing");
                }
                auto argument = arguments[1].as_reference();
                if (!argument || argument->is_null()) {
                    return fail_java("java/lang/NullPointerException",
                                     "file input path is null");
                }
                Result<ObjectRef> path_object = file_argument
                    ? java_file_path_object(machine, *argument)
                    : Result<ObjectRef>(*argument);
                if (!path_object) {
                    return std::unexpected(path_object.error());
                }
                auto initialized = initialize_file_input_stream(
                    machine, *object, *path_object);
                if (!initialized) return std::unexpected(initialized.error());
                return std::optional<Value> {};
            });
    };
    input_constructor(false);
    input_constructor(true);

    const auto output_constructor = [&registry](bool file_argument,
                                                bool with_append) {
        const char* descriptor = nullptr;
        if (file_argument) {
            descriptor = with_append ? "(Ljava/io/File;Z)V"
                                     : "(Ljava/io/File;)V";
        } else {
            descriptor = with_append ? "(Ljava/lang/String;Z)V"
                                     : "(Ljava/lang/String;)V";
        }
        add(registry, "java/io/FileOutputStream", "<init>", descriptor,
            [file_argument, with_append](
                Machine& machine,
                std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                if (arguments.size() < 2U) {
                    return fail(ErrorCode::invalid_argument,
                                "file output path is missing");
                }
                auto argument = arguments[1].as_reference();
                if (!argument || argument->is_null()) {
                    return fail_java("java/lang/NullPointerException",
                                     "file output path is null");
                }
                bool append = false;
                if (with_append) {
                    if (arguments.size() < 3U) {
                        return fail(ErrorCode::invalid_argument,
                                    "file output append flag is missing");
                    }
                    auto value = arguments[2].as_int();
                    if (!value) return std::unexpected(value.error());
                    append = *value != 0;
                }
                Result<ObjectRef> path_object = file_argument
                    ? java_file_path_object(machine, *argument)
                    : Result<ObjectRef>(*argument);
                if (!path_object) {
                    return std::unexpected(path_object.error());
                }
                auto initialized = initialize_file_output_stream(
                    machine, *object, *path_object, append);
                if (!initialized) return std::unexpected(initialized.error());
                return std::optional<Value> {};
            });
    };
    output_constructor(false, false);
    output_constructor(false, true);
    output_constructor(true, false);
    output_constructor(true, true);

    add(registry, "java/io/FileInputStream", "read", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = file_input_read_one(machine, *object);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    const auto register_input_array = [&registry](bool ranged) {
        add(registry, "java/io/FileInputStream", "read",
            ranged ? "([BII)I" : "([B)I",
            [ranged](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                auto array = arguments[1].as_reference();
                if (!object) return std::unexpected(object.error());
                if (!array) return std::unexpected(array.error());
                auto size = byte_array_length(machine, *array);
                if (!size) return std::unexpected(size.error());
                i32 offset = 0;
                i32 length = static_cast<i32>(*size);
                if (ranged) {
                    auto parsed_offset = arguments[2].as_int();
                    auto parsed_length = arguments[3].as_int();
                    if (!parsed_offset || !parsed_length) {
                        return fail(ErrorCode::invalid_argument,
                                    "file read range is invalid");
                    }
                    offset = *parsed_offset;
                    length = *parsed_length;
                }
                auto valid = validate_range(*size, offset, length);
                if (!valid) return std::unexpected(valid.error());
                if (length == 0) {
                    return std::optional<Value>(Value::from_int(0));
                }
                auto handle = stream_handle(machine, *object);
                if (!handle) return std::unexpected(handle.error());
                std::vector<u8> bytes(static_cast<usize>(length));
                auto count = machine.filesystem().read(*handle, bytes);
                if (!count) return file_error(count.error());
                if (*count == 0U) {
                    return std::optional<Value>(Value::from_int(-1));
                }
                bytes.resize(*count);
                auto stored = write_byte_array(machine, *array, offset, bytes);
                if (!stored) return std::unexpected(stored.error());
                return std::optional<Value>(Value::from_int(
                    static_cast<i32>(*count)));
            });
    };
    register_input_array(false);
    register_input_array(true);
    add(registry, "java/io/FileInputStream", "skip", "(J)J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto requested = arguments[1].as_long();
            if (!object) return std::unexpected(object.error());
            if (!requested) return std::unexpected(requested.error());
            auto skipped = file_input_skip(machine, *object, *requested);
            if (!skipped) return std::unexpected(skipped.error());
            return std::optional<Value>(Value::from_long(*skipped));
        });
    add(registry, "java/io/FileInputStream", "available", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto available = file_input_available(machine, *object);
            if (!available) return std::unexpected(available.error());
            return std::optional<Value>(Value::from_int(*available));
        });
    add(registry, "java/io/FileInputStream", "close", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto closed = file_input_close(machine, *object);
            if (!closed) return std::unexpected(closed.error());
            return std::optional<Value> {};
        });

    add(registry, "java/io/FileOutputStream", "write", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = arguments[1].as_int();
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto written = file_output_write_one(
                machine, *object, static_cast<u8>(*value));
            if (!written) return std::unexpected(written.error());
            return std::optional<Value> {};
        });
    const auto register_output_array = [&registry](bool ranged) {
        add(registry, "java/io/FileOutputStream", "write",
            ranged ? "([BII)V" : "([B)V",
            [ranged](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                auto array = arguments[1].as_reference();
                if (!object) return std::unexpected(object.error());
                if (!array) return std::unexpected(array.error());
                auto size = byte_array_length(machine, *array);
                if (!size) return std::unexpected(size.error());
                i32 offset = 0;
                i32 length = static_cast<i32>(*size);
                if (ranged) {
                    auto parsed_offset = arguments[2].as_int();
                    auto parsed_length = arguments[3].as_int();
                    if (!parsed_offset || !parsed_length) {
                        return fail(ErrorCode::invalid_argument,
                                    "file write range is invalid");
                    }
                    offset = *parsed_offset;
                    length = *parsed_length;
                }
                auto bytes = read_byte_array(machine, *array, offset, length);
                if (!bytes) return std::unexpected(bytes.error());
                auto handle = stream_handle(machine, *object);
                if (!handle) return std::unexpected(handle.error());
                auto count = machine.filesystem().write(*handle, *bytes);
                if (!count) return file_error(count.error());
                return std::optional<Value> {};
            });
    };
    register_output_array(false);
    register_output_array(true);
    add(registry, "java/io/FileOutputStream", "flush", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto flushed = file_output_flush(machine, *object);
            if (!flushed) return std::unexpected(flushed.error());
            return std::optional<Value> {};
        });
    add(registry, "java/io/FileOutputStream", "close", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto closed = file_output_close(machine, *object);
            if (!closed) return std::unexpected(closed.error());
            return std::optional<Value> {};
        });
}

void register_file_connection_natives(NativeMethodRegistry& registry) {
    constexpr const char* owner =
        "javax/microedition/io/file/FileConnectionImpl";
    add(registry, owner, "isOpen", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto open = int_field(machine, *object, kConnectionOpenField);
            if (!open) return std::unexpected(open.error());
            return std::optional<Value>(Value::from_int(*open != 0 ? 1 : 0));
        });
    add(registry, owner, "close", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto open = int_field(machine, *object, kConnectionOpenField);
            if (!open) return std::unexpected(open.error());
            if (*open == 0) return std::optional<Value> {};
            auto streams_closed = close_connection_streams(machine, *object);
            auto closed = set_int_field(machine, *object,
                                        kConnectionOpenField, 0);
            if (!streams_closed) {
                return std::unexpected(streams_closed.error());
            }
            if (!closed) return std::unexpected(closed.error());
            return std::optional<Value> {};
        });

    const auto register_stat_boolean = [&registry](const char* name,
                                                   auto selector) {
        add(registry, owner, name, "()Z",
            [selector](Machine& machine,
                       std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto state = connection_state(machine, arguments, true);
                if (!state) return std::unexpected(state.error());
                auto info = machine.filesystem().stat(state->path);
                if (!info) return file_error(info.error());
                return std::optional<Value>(Value::from_int(
                    selector(*info) ? 1 : 0));
            });
    };
    register_stat_boolean("exists", [](const filesystem::FileInfo& info) {
        return info.exists;
    });
    register_stat_boolean("isDirectory", [](const filesystem::FileInfo& info) {
        return info.exists && info.directory;
    });
    register_stat_boolean("canRead", [](const filesystem::FileInfo& info) {
        return info.readable;
    });
    register_stat_boolean("canWrite", [](const filesystem::FileInfo& info) {
        return info.writable;
    });
    register_stat_boolean("isHidden", [](const filesystem::FileInfo& info) {
        return info.hidden;
    });

    add(registry, owner, "fileSize", "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, true);
            if (!state) return std::unexpected(state.error());
            auto info = machine.filesystem().stat(state->path);
            if (!info) return file_error(info.error());
            if (!info->exists) {
                return std::optional<Value>(Value::from_long(-1));
            }
            if (info->directory) {
                return fail_java("java/io/IOException",
                                 "FileConnection does not name a file");
            }
            return std::optional<Value>(Value::from_long(
                clamp_java_long_size(info->size)));
        });
    add(registry, owner, "directorySize", "(Z)J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, true);
            auto include_subdirectories = arguments[1].as_int();
            if (!state) return std::unexpected(state.error());
            if (!include_subdirectories) {
                return std::unexpected(include_subdirectories.error());
            }
            auto info = machine.filesystem().stat(state->path);
            if (!info) return file_error(info.error());
            if (!info->exists) {
                return std::optional<Value>(Value::from_long(-1));
            }
            if (!info->directory) {
                return fail_java("java/io/IOException",
                                 "FileConnection does not name a directory");
            }
            auto size = machine.filesystem().directory_size(
                state->path, *include_subdirectories != 0);
            if (!size) return file_error(size.error());
            return std::optional<Value>(Value::from_long(
                clamp_java_long_size(*size)));
        });

    const auto register_storage_size = [&registry](
        const char* name,
        auto selector) {
        add(registry, owner, name, "()J",
            [selector](Machine& machine,
                       std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto state = connection_state(machine, arguments, true);
                if (!state) return std::unexpected(state.error());
                auto info = machine.filesystem().storage_info();
                if (!info) return file_error(info.error());
                return std::optional<Value>(Value::from_long(
                    clamp_java_long_size(selector(*info))));
            });
    };
    register_storage_size("availableSize",
                          [](const filesystem::StorageInfo& info) {
                              return info.available;
                          });
    register_storage_size("totalSize",
                          [](const filesystem::StorageInfo& info) {
                              return info.total;
                          });
    register_storage_size("usedSize",
                          [](const filesystem::StorageInfo& info) {
                              return info.used;
                          });

    add(registry, owner, "lastModified", "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, true);
            if (!state) return std::unexpected(state.error());
            auto info = machine.filesystem().stat(state->path);
            if (!info) return file_error(info.error());
            return std::optional<Value>(Value::from_long(
                info->exists ? info->modified_seconds * 1000 : 0));
        });

    const auto register_string_property = [&registry](const char* name,
                                                       auto selector) {
        add(registry, owner, name, "()Ljava/lang/String;",
            [selector](Machine& machine,
                       std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto state = connection_state(machine, arguments);
                if (!state) return std::unexpected(state.error());
                auto text = create_string(machine, selector(state->path));
                if (!text) return std::unexpected(text.error());
                return std::optional<Value>(Value::from_reference(*text));
            });
    };
    add(registry, owner, "getName", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments);
            if (!state) return std::unexpected(state.error());
            auto info = machine.filesystem().stat(state->path);
            if (!info) return file_error(info.error());
            std::string name = leaf_name(state->path);
            if (info->exists && info->directory && !name.empty()) {
                name.push_back('/');
            }
            auto text = create_string(machine, name);
            if (!text) return std::unexpected(text.error());
            return std::optional<Value>(Value::from_reference(*text));
        });
    register_string_property("getPath", [](const std::string& path) {
        const std::string parent = parent_path(path);
        return parent.empty() ? std::string("/")
                              : "/" + parent + "/";
    });
    add(registry, owner, "getURL", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments);
            if (!state) return std::unexpected(state.error());
            auto info = machine.filesystem().stat(state->path);
            if (!info) return file_error(info.error());
            std::string url = file_resource_uri(state->path);
            if (info->exists && info->directory && !url.ends_with('/')) {
                url.push_back('/');
            }
            auto text = create_string(machine, url);
            if (!text) return std::unexpected(text.error());
            return std::optional<Value>(Value::from_reference(*text));
        });

    const auto register_permission_mutation = [&registry](
        const char* name,
        auto operation) {
        add(registry, owner, name, "(Z)V",
            [operation](Machine& machine,
                        std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto state = connection_state(machine, arguments,
                                              false, true);
                auto enabled = arguments[1].as_int();
                if (!state) return std::unexpected(state.error());
                if (!enabled) return std::unexpected(enabled.error());
                auto status = operation(machine.filesystem(), state->path,
                                        *enabled != 0);
                if (!status) return file_error(status.error());
                return std::optional<Value> {};
            });
    };
    register_permission_mutation(
        "setReadable",
        [](filesystem::FileSystem& files,
           const std::string& path,
           bool readable) {
            return files.set_readable(path, readable);
        });
    register_permission_mutation(
        "setWritable",
        [](filesystem::FileSystem& files,
           const std::string& path,
           bool writable) {
            return files.set_writable(path, writable);
        });

    add(registry, owner, "setHidden", "(Z)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, false, true);
            auto requested = arguments[1].as_int();
            if (!state) return std::unexpected(state.error());
            if (!requested) return std::unexpected(requested.error());
            std::string name = leaf_name(state->path);
            if (name.empty()) return std::optional<Value> {};
            const bool hidden = name.front() == '.';
            if (hidden == (*requested != 0)) {
                return std::optional<Value> {};
            }
            if (*requested != 0) {
                name.insert(name.begin(), '.');
            } else {
                name.erase(name.begin());
                if (name.empty()) {
                    return fail_java("java/io/IOException",
                                     "hidden file name cannot become empty");
                }
            }
            std::string destination = parent_path(state->path);
            if (!destination.empty()) destination.push_back('/');
            destination.append(name);
            auto closed = close_connection_streams(machine, state->object);
            if (!closed) return std::unexpected(closed.error());
            auto renamed = machine.filesystem().rename(state->path,
                                                       destination);
            if (!renamed) return file_error(renamed.error());
            auto string = create_string(machine, destination);
            if (!string) return std::unexpected(string.error());
            auto stored = set_reference_field(machine, state->object,
                                              kConnectionPathField, *string);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    const auto register_mutation = [&registry](const char* name,
                                               auto operation) {
        add(registry, owner, name, "()V",
            [operation](Machine& machine,
                        std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto state = connection_state(machine, arguments,
                                              false, true);
                if (!state) return std::unexpected(state.error());
                auto status = operation(machine.filesystem(), state->path);
                if (!status) return file_error(status.error());
                return std::optional<Value> {};
            });
    };
    register_mutation("create", [](filesystem::FileSystem& files,
                                    const std::string& path) {
        return files.create_file(path);
    });
    register_mutation("mkdir", [](filesystem::FileSystem& files,
                                   const std::string& path) {
        return files.create_directory(path);
    });

    add(registry, owner, "delete", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, false, true);
            if (!state) return std::unexpected(state.error());
            auto closed = close_connection_streams(machine, state->object);
            if (!closed) return std::unexpected(closed.error());
            auto removed = machine.filesystem().remove(state->path, false);
            if (!removed) return file_error(removed.error());
            return std::optional<Value> {};
        });

    add(registry, owner, "truncate", "(J)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, false, true);
            auto length = arguments[1].as_long();
            if (!state) return std::unexpected(state.error());
            if (!length) return std::unexpected(length.error());
            if (*length < 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "truncate length is negative");
            }
            auto flushed = flush_connection_output(machine, state->object);
            if (!flushed) return std::unexpected(flushed.error());
            auto info = machine.filesystem().stat(state->path);
            if (!info) return file_error(info.error());
            if (!info->exists || info->directory) {
                return fail_java("java/io/IOException",
                                 "truncate requires an existing file");
            }
            if (static_cast<u64>(*length) >= info->size) {
                return std::optional<Value> {};
            }
            auto status = machine.filesystem().truncate(
                state->path, static_cast<u64>(*length));
            if (!status) return file_error(status.error());
            return std::optional<Value> {};
        });
    add(registry, owner, "rename", "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, false, true);
            auto name_object = arguments[1].as_reference();
            if (!state) return std::unexpected(state.error());
            if (!name_object || name_object->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "new file name is null");
            }
            auto name = utf8_text(machine, *name_object);
            if (!name) return std::unexpected(name.error());
            if (name->empty() || name->find('/') != std::string::npos ||
                name->find('\\') != std::string::npos || *name == "." ||
                *name == "..") {
                return fail_java("java/lang/IllegalArgumentException",
                                 "rename requires one file name");
            }
            std::string destination = parent_path(state->path);
            if (!destination.empty()) destination.push_back('/');
            destination.append(*name);
            auto closed = close_connection_streams(machine, state->object);
            if (!closed) return std::unexpected(closed.error());
            auto renamed = machine.filesystem().rename(state->path,
                                                       destination);
            if (!renamed) return file_error(renamed.error());
            auto string = create_string(machine, destination);
            if (!string) return std::unexpected(string.error());
            auto stored = set_reference_field(machine, state->object,
                                              kConnectionPathField, *string);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    add(registry, owner, "setFileConnection", "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, true);
            if (!state) return std::unexpected(state.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "setFileConnection target is missing");
            }
            auto target_object = arguments[1].as_reference();
            if (!target_object || target_object->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "setFileConnection target is null");
            }
            auto target = utf8_text(machine, *target_object);
            if (!target) return std::unexpected(target.error());
            if (target->empty() || *target == "." ||
                (target->find('/') != std::string::npos) ||
                (target->find('\\') != std::string::npos)) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "setFileConnection requires one path component");
            }

            std::string destination;
            if (*target == "..") {
                destination = parent_path(state->path);
            } else {
                auto current = machine.filesystem().stat(state->path);
                if (!current) return file_error(current.error());
                if (!current->exists || !current->directory) {
                    return fail_java("java/io/IOException",
                                     "setFileConnection requires a directory");
                }
                destination = state->path;
                if (!destination.empty()) destination.push_back('/');
                destination.append(*target);
            }
            auto normalized = filesystem::normalize_virtual_path(
                destination, true);
            if (!normalized) return file_error(normalized.error());
            auto info = machine.filesystem().stat(*normalized);
            if (!info) return file_error(info.error());
            if (!info->exists) {
                return fail_java("java/io/IOException",
                                 "setFileConnection target does not exist");
            }
            auto closed = close_connection_streams(machine, state->object);
            if (!closed) return std::unexpected(closed.error());
            auto string = create_string(machine, *normalized);
            if (!string) return std::unexpected(string.error());
            auto stored = set_reference_field(machine, state->object,
                                              kConnectionPathField, *string);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    const auto register_list = [&registry](bool filtered) {
        add(registry, owner, "list",
            filtered ? "(Ljava/lang/String;Z)Ljava/util/Enumeration;"
                     : "()Ljava/util/Enumeration;",
            [filtered](Machine& machine,
                       std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto state = connection_state(machine, arguments, true);
                if (!state) return std::unexpected(state.error());
                std::string filter = "*";
                bool include_hidden = false;
                if (filtered) {
                    auto filter_object = arguments[1].as_reference();
                    auto hidden = arguments[2].as_int();
                    if (!filter_object || filter_object->is_null()) {
                        return fail_java("java/lang/NullPointerException",
                                         "file filter is null");
                    }
                    if (!hidden) return std::unexpected(hidden.error());
                    auto parsed = utf8_text(machine, *filter_object);
                    if (!parsed) return std::unexpected(parsed.error());
                    if (parsed->empty() ||
                        parsed->find('/') != std::string::npos ||
                        parsed->find('\\') != std::string::npos ||
                        *parsed == "." || *parsed == "..") {
                        return fail_java(
                            "java/lang/IllegalArgumentException",
                            "file listing filter contains a path component");
                    }
                    filter = std::move(*parsed);
                    include_hidden = *hidden != 0;
                }
                auto names = machine.filesystem().list(state->path);
                if (!names) return file_error(names.error());
                std::vector<std::string> selected;
                for (const std::string& name : *names) {
                    if (!include_hidden && !name.empty() &&
                        name.front() == '.') continue;
                    if (wildcard_match(filter, name)) selected.push_back(name);
                }
                auto enumeration = create_enumeration(machine, selected);
                if (!enumeration) return std::unexpected(enumeration.error());
                return std::optional<Value>(
                    Value::from_reference(*enumeration));
            });
    };
    register_list(false);
    register_list(true);

    add(registry, owner, "openInputStream", "()Ljava/io/InputStream;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, true);
            if (!state) return std::unexpected(state.error());
            auto stream = create_file_input_stream(
                machine, state->path, state->object);
            if (!stream) return std::unexpected(stream.error());
            return std::optional<Value>(Value::from_reference(*stream));
        });
    add(registry, owner, "openDataInputStream",
        "()Ljava/io/DataInputStream;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, true);
            if (!state) return std::unexpected(state.error());
            auto input = create_file_input_stream(
                machine, state->path, state->object);
            if (!input) return std::unexpected(input.error());
            auto stream = wrap_data_input(machine, *input);
            if (!stream) {
                static_cast<void>(file_input_close(machine, *input));
                return std::unexpected(stream.error());
            }
            return std::optional<Value>(Value::from_reference(*stream));
        });
    add(registry, owner, "openOutputStream", "()Ljava/io/OutputStream;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, false, true);
            if (!state) return std::unexpected(state.error());
            auto stream = create_file_output_stream(
                machine, state->path, std::nullopt, state->object);
            if (!stream) return std::unexpected(stream.error());
            return std::optional<Value>(Value::from_reference(*stream));
        });
    add(registry, owner, "openOutputStream", "(J)Ljava/io/OutputStream;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, false, true);
            auto offset = arguments[1].as_long();
            if (!state) return std::unexpected(state.error());
            if (!offset) return std::unexpected(offset.error());
            auto stream = create_file_output_stream(
                machine, state->path, *offset, state->object);
            if (!stream) return std::unexpected(stream.error());
            return std::optional<Value>(Value::from_reference(*stream));
        });
    add(registry, owner, "openDataOutputStream",
        "()Ljava/io/DataOutputStream;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto state = connection_state(machine, arguments, false, true);
            if (!state) return std::unexpected(state.error());
            auto output = create_file_output_stream(
                machine, state->path, std::nullopt, state->object);
            if (!output) return std::unexpected(output.error());
            auto stream = wrap_data_output(machine, *output);
            if (!stream) {
                static_cast<void>(file_output_close(machine, *output));
                return std::unexpected(stream.error());
            }
            return std::optional<Value>(Value::from_reference(*stream));
        });
}

} // namespace

Result<std::optional<ObjectRef>> try_open_file_connection(
    Machine& machine,
    ObjectRef url,
    i32 mode) {
    auto text = utf8_text(machine, url);
    if (!text) return std::unexpected(text.error());
    if (!text->starts_with("file:")) return std::optional<ObjectRef> {};
    auto connection = create_connection(machine, url, mode);
    if (!connection) return std::unexpected(connection.error());
    return std::optional<ObjectRef>(*connection);
}

Result<std::optional<ObjectRef>> try_open_file_connection_stream(
    Machine& machine,
    ObjectRef connection,
    bool input,
    bool data) {
    auto is_file = machine.object_is_instance(
        connection, "javax/microedition/io/file/FileConnection");
    if (!is_file) return std::unexpected(is_file.error());
    if (!*is_file) return std::optional<ObjectRef> {};

    const std::array<Value, 1> arguments {
        Value::from_reference(connection),
    };
    auto state = connection_state(machine, arguments, input, !input);
    if (!state) return std::unexpected(state.error());
    Result<ObjectRef> stream = input
        ? create_file_input_stream(machine, state->path, state->object)
        : create_file_output_stream(machine, state->path, std::nullopt,
                                    state->object);
    if (!stream) return std::unexpected(stream.error());
    if (data) {
        const ObjectRef raw_stream = *stream;
        stream = input ? wrap_data_input(machine, raw_stream)
                       : wrap_data_output(machine, raw_stream);
        if (!stream) {
            if (input) {
                static_cast<void>(file_input_close(machine, raw_stream));
            } else {
                static_cast<void>(file_output_close(machine, raw_stream));
            }
            return std::unexpected(stream.error());
        }
    }
    return std::optional<ObjectRef>(*stream);
}

Result<std::optional<bool>> try_close_file_connection(
    Machine& machine,
    ObjectRef connection) {
    auto is_file = machine.object_is_instance(
        connection, "javax/microedition/io/file/FileConnection");
    if (!is_file) return std::unexpected(is_file.error());
    if (!*is_file) return std::optional<bool> {};
    auto open = int_field(machine, connection, kConnectionOpenField);
    if (!open) return std::unexpected(open.error());
    if (*open == 0) return std::optional<bool>(true);
    auto streams_closed = close_connection_streams(machine, connection);
    auto closed = set_int_field(machine, connection, kConnectionOpenField, 0);
    if (!streams_closed) return std::unexpected(streams_closed.error());
    if (!closed) return std::unexpected(closed.error());
    return std::optional<bool>(true);
}

Result<i32> file_input_read_one(Machine& machine, ObjectRef stream) {
    auto handle = stream_handle(machine, stream);
    if (!handle) return std::unexpected(handle.error());
    std::array<u8, 1> byte {};
    auto count = machine.filesystem().read(*handle, byte);
    if (!count) return file_error(count.error());
    return *count == 0U ? -1 : static_cast<i32>(byte.front());
}

Result<i64> file_input_skip(Machine& machine,
                            ObjectRef stream,
                            i64 requested) {
    if (requested <= 0) return 0;
    auto handle = stream_handle(machine, stream);
    if (!handle) return std::unexpected(handle.error());
    auto available = machine.filesystem().available(*handle);
    if (!available) return file_error(available.error());
    const i64 amount = std::min(requested, *available);
    auto position = machine.filesystem().seek(
        *handle, amount, filesystem::SeekOrigin::current);
    if (!position) return file_error(position.error());
    return amount;
}

Result<i32> file_input_available(Machine& machine, ObjectRef stream) {
    auto handle = stream_handle(machine, stream);
    if (!handle) return std::unexpected(handle.error());
    auto available = machine.filesystem().available(*handle);
    if (!available) return file_error(available.error());
    return static_cast<i32>(std::min<i64>(
        *available, std::numeric_limits<i32>::max()));
}

Status file_input_close(Machine& machine, ObjectRef stream) {
    auto closed = int_field(machine, stream, kFileStreamClosedField);
    if (!closed) return std::unexpected(closed.error());
    if (*closed != 0) return {};
    auto handle = int_field(machine, stream, kFileStreamHandleField);
    if (!handle) return std::unexpected(handle.error());
    auto status = machine.filesystem().close(*handle);
    if (!status) return file_error(status.error());
    return set_int_field(machine, stream, kFileStreamClosedField, 1);
}

Status file_output_write_one(Machine& machine,
                             ObjectRef stream,
                             u8 byte) {
    auto handle = stream_handle(machine, stream);
    if (!handle) return std::unexpected(handle.error());
    const std::array<u8, 1> bytes {byte};
    auto count = machine.filesystem().write(*handle, bytes);
    if (!count) return file_error(count.error());
    return {};
}

Status file_output_flush(Machine& machine, ObjectRef stream) {
    auto handle = stream_handle(machine, stream);
    if (!handle) return std::unexpected(handle.error());
    auto status = machine.filesystem().flush(*handle);
    if (!status) return file_error(status.error());
    return {};
}

Status file_output_close(Machine& machine, ObjectRef stream) {
    auto closed = int_field(machine, stream, kFileStreamClosedField);
    if (!closed) return std::unexpected(closed.error());
    if (*closed != 0) return {};
    auto handle = int_field(machine, stream, kFileStreamHandleField);
    if (!handle) return std::unexpected(handle.error());
    auto flushed = machine.filesystem().flush(*handle);
    auto closed_status = machine.filesystem().close(*handle);
    auto marked = set_int_field(machine, stream, kFileStreamClosedField, 1);
    if (!flushed) return file_error(flushed.error());
    if (!closed_status) return file_error(closed_status.error());
    return marked;
}

void register_file_natives(NativeMethodRegistry& registry) {
    register_java_file_natives(registry);
    register_file_stream_natives(registry);
    register_file_connection_natives(registry);

    const auto file_system_listeners = [&registry](Machine& machine)
        -> Result<ObjectRef> {
        constexpr std::string_view owner =
            "javax/microedition/io/file/FileSystemRegistry";
        auto field = machine.class_states().resolve_field(
            owner, "listeners", "Ljava/util/Vector;", true);
        if (!field) return std::unexpected(field.error());
        auto current = machine.class_states().static_field(*field);
        if (!current) return std::unexpected(current.error());
        auto listeners = current->as_reference();
        if (!listeners) return std::unexpected(listeners.error());
        if (!listeners->is_null()) return *listeners;

        auto vector = machine.class_states().allocate_instance(
            machine.heap(), "java/util/Vector");
        if (!vector) return std::unexpected(vector.error());
        const std::array<Value, 1> constructor_arguments {
            Value::from_reference(*vector),
        };
        auto initialized = registry.invoke(
            machine, "java/util/Vector", "<init>", "()V",
            constructor_arguments);
        if (!initialized) return std::unexpected(initialized.error());
        auto stored = machine.class_states().set_static_field(
            *field, Value::from_reference(*vector));
        if (!stored) return std::unexpected(stored.error());
        return *vector;
    };

    add(registry,
        "javax/microedition/io/file/FileSystemRegistry",
        "listRoots",
        "()Ljava/util/Enumeration;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "FileSystemRegistry.listRoots expects no arguments");
            }
            const std::vector<std::string> roots {"root/"};
            auto enumeration = create_enumeration(machine, roots);
            if (!enumeration) return std::unexpected(enumeration.error());
            return std::optional<Value>(Value::from_reference(*enumeration));
        });

    add(registry,
        "javax/microedition/io/file/FileSystemRegistry",
        "addFileSystemListener",
        "(Ljavax/microedition/io/file/FileSystemListener;)Z",
        [&registry, file_system_listeners](
            Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1U) {
                return fail(ErrorCode::invalid_argument,
                            "addFileSystemListener expects one listener");
            }
            auto listener = arguments[0].as_reference();
            if (!listener) return std::unexpected(listener.error());
            if (listener->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "filesystem listener is null");
            }
            auto valid = machine.object_is_instance(
                *listener,
                "javax/microedition/io/file/FileSystemListener");
            if (!valid) return std::unexpected(valid.error());
            if (!*valid) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "object is not a FileSystemListener");
            }
            auto listeners = file_system_listeners(machine);
            if (!listeners) return std::unexpected(listeners.error());
            const std::array<Value, 2> vector_arguments {
                Value::from_reference(*listeners),
                Value::from_reference(*listener),
            };
            auto contained = registry.invoke(
                machine, "java/util/Vector", "contains",
                "(Ljava/lang/Object;)Z", vector_arguments);
            if (!contained) return std::unexpected(contained.error());
            if (!contained->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Vector.contains returned no value");
            }
            auto exists = contained->value().as_int();
            if (!exists) return std::unexpected(exists.error());
            if (*exists == 0) {
                auto added = registry.invoke(
                    machine, "java/util/Vector", "addElement",
                    "(Ljava/lang/Object;)V", vector_arguments);
                if (!added) return std::unexpected(added.error());
            }
            return std::optional<Value>(Value::from_int(1));
        });

    add(registry,
        "javax/microedition/io/file/FileSystemRegistry",
        "removeFileSystemListener",
        "(Ljavax/microedition/io/file/FileSystemListener;)Z",
        [&registry, file_system_listeners](
            Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1U) {
                return fail(ErrorCode::invalid_argument,
                            "removeFileSystemListener expects one listener");
            }
            auto listener = arguments[0].as_reference();
            if (!listener) return std::unexpected(listener.error());
            if (listener->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "filesystem listener is null");
            }
            auto listeners = file_system_listeners(machine);
            if (!listeners) return std::unexpected(listeners.error());
            const std::array<Value, 2> vector_arguments {
                Value::from_reference(*listeners),
                Value::from_reference(*listener),
            };
            auto removed = registry.invoke(
                machine, "java/util/Vector", "removeElement",
                "(Ljava/lang/Object;)Z", vector_arguments);
            if (!removed) return std::unexpected(removed.error());
            if (!removed->has_value()) {
                return fail(ErrorCode::internal_error,
                            "Vector.removeElement returned no value");
            }
            return removed;
        });
}

} // namespace phoneme::vm
