#include "ConnectionNatives.hpp"

#include "FileNatives.hpp"
#include "SensorNatives.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "phoneme/network/ConnectionRegistry.hpp"
#include "phoneme/security/PermissionPolicy.hpp"
#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/ModifiedUtf8.hpp"
#include "phoneme/vm/NativeMethodRegistry.hpp"

namespace phoneme::vm {
namespace {

constexpr usize kHandleIdField = 0;
constexpr usize kHandleGenerationField = 1;
constexpr usize kStreamClosedField = 2;
constexpr usize kStreamOwnsConnectionField = 3;
constexpr usize kStreamBufferField = 4;
constexpr usize kStreamBufferPositionField = 5;
constexpr usize kStreamBufferCountField = 6;

constexpr usize kDatagramDataField = 0;
constexpr usize kDatagramOffsetField = 1;
constexpr usize kDatagramLengthField = 2;
constexpr usize kDatagramPositionField = 3;
constexpr usize kDatagramAddressField = 4;

constexpr std::string_view kJavaNetSocketClass = "java/net/Socket";
constexpr std::string_view kInputStreamClass =
    "javax/microedition/io/NativeInputStream";
constexpr std::string_view kOutputStreamClass =
    "javax/microedition/io/NativeOutputStream";
constexpr std::string_view kSocketClass =
    "javax/microedition/io/NativeSocketConnection";
constexpr std::string_view kServerClass =
    "javax/microedition/io/NativeServerSocketConnection";
constexpr std::string_view kDatagramConnectionClass =
    "javax/microedition/io/NativeDatagramConnection";
constexpr std::string_view kHttpClass =
    "javax/microedition/io/NativeHttpConnection";
constexpr std::string_view kHttpsClass =
    "javax/microedition/io/NativeHttpsConnection";
constexpr std::string_view kDatagramClass =
    "javax/microedition/io/NativeDatagram";
constexpr std::string_view kSecurityInfoClass =
    "javax/microedition/io/NativeSecurityInfo";
constexpr std::string_view kCertificateClass =
    "javax/microedition/pki/NativeCertificate";
constexpr std::string_view kCertificateExceptionClass =
    "javax/microedition/pki/CertificateException";
constexpr std::string_view kWirelessConnectionClass =
    "javax/wireless/messaging/NativeMessageConnection";
constexpr std::string_view kWirelessTextMessageClass =
    "javax/wireless/messaging/NativeTextMessage";
constexpr std::string_view kWirelessBinaryMessageClass =
    "javax/wireless/messaging/NativeBinaryMessage";
constexpr std::string_view kWirelessMultipartMessageClass =
    "javax/wireless/messaging/NativeMultipartMessage";
constexpr std::string_view kWirelessMessagePartClass =
    "javax/wireless/messaging/MessagePart";

constexpr usize kWirelessConnectionAddressField = 0;
constexpr usize kWirelessConnectionModeField = 1;
constexpr usize kWirelessConnectionClosedField = 2;
constexpr usize kWirelessConnectionListenerField = 3;
constexpr usize kWirelessConnectionProtocolField = 4;

constexpr usize kWirelessMessageAddressField = 0;
constexpr usize kWirelessMessageTimestampField = 1;
constexpr usize kWirelessMultipartSubjectField = 2;
constexpr usize kWirelessMultipartStartContentIdField = 3;
constexpr usize kWirelessMultipartPartsField = 4;
constexpr usize kWirelessMultipartPartCountField = 5;
constexpr usize kWirelessPartContentField = 0;
constexpr usize kWirelessPartContentTypeField = 1;
constexpr usize kWirelessPartContentIdField = 2;
constexpr usize kWirelessPartContentLocationField = 3;
constexpr usize kWirelessPartEncodingField = 4;
constexpr usize kWirelessMessagePayloadField = 2;

constexpr usize kSecurityCertificateField = 0;
constexpr usize kSecurityProtocolNameField = 1;
constexpr usize kSecurityProtocolVersionField = 2;
constexpr usize kSecurityCipherSuiteField = 3;

constexpr usize kCertificateSubjectField = 0;
constexpr usize kCertificateIssuerField = 1;
constexpr usize kCertificateSerialField = 2;
constexpr usize kCertificateNotBeforeField = 3;
constexpr usize kCertificateNotAfterField = 4;

constexpr usize kThrowableMessageField = 0;
constexpr usize kThrowableCauseField = 1;
constexpr usize kThrowableCauseInitializedField = 2;
constexpr usize kCertificateExceptionCertificateField = 3;
constexpr usize kCertificateExceptionReasonField = 4;

[[nodiscard]] std::string certificate_exception_message(i32 reason) {
    switch (reason) {
    case 1:
        return "Certificate has unrecognized critical extensions";
    case 2:
        return "Server certificate chain exceeds the length allowed by an "
               "issuer's policy";
    case 3:
        return "Certificate is expired";
    case 4:
        return "Intermediate certificate in the chain does not have the "
               "authority to be an intermediate CA";
    case 5:
        return "Certificate object does not contain a signature";
    case 6:
        return "Certificate is not yet valid";
    case 7:
        return "Certificate does not contain the correct site name";
    case 8:
        return "Certificate was issued by an unrecognized entity";
    case 9:
        return "Certificate was signed using an unsupported algorithm";
    case 10:
        return "Certificate's public key has been used in a way deemed "
               "inappropriate by the issuer";
    case 11:
        return "Certificate in a chain was not issued by the next authority "
               "in the chain";
    case 12:
        return "Root CA's public key is expired";
    case 13:
        return "Certificate has a public key that is not a supported type";
    case 14:
        return "Certificate failed verification";
    default:
        return "Unknown reason (" + std::to_string(reason) + ")";
    }
}

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

[[nodiscard]] std::unexpected<Error> as_java_network_error(
    const Error& error) {
    if (error.code == ErrorCode::java_exception) {
        return std::unexpected(error);
    }
    switch (error.code) {
    case ErrorCode::unsupported_feature:
    case ErrorCode::class_not_found:
        return fail_java("javax/microedition/io/ConnectionNotFoundException",
                         error.message);
    case ErrorCode::invalid_argument:
    case ErrorCode::out_of_range:
        return fail_java("java/lang/IllegalArgumentException", error.message);
    case ErrorCode::overflow:
        return fail_java("java/lang/OutOfMemoryError", error.message);
    default:
        return fail_java("java/io/IOException", error.message);
    }
}

template <typename T>
[[nodiscard]] Result<T> java_network_result(Result<T> result) {
    if (!result) return as_java_network_error(result.error());
    return std::move(*result);
}

[[nodiscard]] Status java_network_status(Status status) {
    if (!status) return as_java_network_error(status.error());
    return {};
}

[[nodiscard]] Result<ObjectRef> receiver(
    std::span<const Value> arguments,
    std::string_view operation) {
    if (arguments.empty()) {
        return fail(ErrorCode::invalid_argument,
                    std::string(operation) + " has no receiver");
    }
    auto object = arguments.front().as_reference();
    if (!object || object->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         std::string(operation) + " receiver is null");
    }
    return *object;
}

[[nodiscard]] Result<ObjectRef> reference_argument(
    std::span<const Value> arguments,
    usize index,
    std::string_view label,
    bool nullable = false) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    std::string(label) + " is missing");
    }
    auto reference = arguments[index].as_reference();
    if (!reference) return std::unexpected(reference.error());
    if (!nullable && reference->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         std::string(label) + " is null");
    }
    return *reference;
}

[[nodiscard]] Result<i32> int_argument(
    std::span<const Value> arguments,
    usize index,
    std::string_view label) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    std::string(label) + " is missing");
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

[[nodiscard]] Result<i64> long_field(Machine& machine,
                                     ObjectRef object,
                                     usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_long();
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

[[nodiscard]] Status set_long_field(Machine& machine,
                                    ObjectRef object,
                                    usize index,
                                    i64 value) {
    return machine.heap().set_field(object, index, Value::from_long(value));
}

[[nodiscard]] Result<std::string> utf8_text(Machine& machine,
                                            ObjectRef string) {
    if (string.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "network string is null");
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
            result.push_back(static_cast<char>(0x80U |
                                               (code_point & 0x3FU)));
        } else if (code_point <= 0xFFFFU) {
            result.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            result.push_back(static_cast<char>(0x80U |
                                               ((code_point >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U |
                                               (code_point & 0x3FU)));
        } else {
            result.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            result.push_back(static_cast<char>(0x80U |
                                               ((code_point >> 12U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U |
                                               ((code_point >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U |
                                               (code_point & 0x3FU)));
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
                        "network text contains invalid UTF-8");
        }
        if (continuation_count > input.size() - index) {
            return fail(ErrorCode::invalid_argument,
                        "network text contains truncated UTF-8");
        }
        for (usize continuation = 0;
             continuation < continuation_count;
             ++continuation) {
            const u8 byte = static_cast<u8>(input[index++]);
            if ((byte & 0xC0U) != 0x80U) {
                return fail(ErrorCode::invalid_argument,
                            "network text contains invalid UTF-8 continuation");
            }
            code_point = (code_point << 6U) | (byte & 0x3FU);
        }
        const u32 minimum = continuation_count == 0U ? 0U
            : (continuation_count == 1U ? 0x80U
            : (continuation_count == 2U ? 0x800U : 0x10000U));
        if (code_point < minimum || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return fail(ErrorCode::invalid_argument,
                        "network text contains invalid UTF-8 code point");
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

[[nodiscard]] Result<ObjectRef> create_string(Machine& machine,
                                              std::u16string text) {
    auto object = machine.class_states().allocate_instance(
        machine.heap(), "java/lang/String");
    if (!object) return std::unexpected(object.error());
    auto attached = machine.heap().attach_string(*object, std::move(text));
    if (!attached) return std::unexpected(attached.error());
    return *object;
}

[[nodiscard]] Result<ObjectRef> create_string(Machine& machine,
                                              std::string_view utf8) {
    auto text = decode_utf8(utf8);
    if (!text) return std::unexpected(text.error());
    return create_string(machine, std::move(*text));
}

[[nodiscard]] Status set_static_int(Machine& machine,
                                    std::string_view owner,
                                    std::string_view name,
                                    i32 value) {
    auto field = machine.class_states().resolve_field(
        owner, name, "I", true);
    if (!field) return std::unexpected(field.error());
    return machine.class_states().set_static_field(
        *field, Value::from_int(value));
}

[[nodiscard]] Status set_static_byte(Machine& machine,
                                     std::string_view owner,
                                     std::string_view name,
                                     i32 value) {
    auto field = machine.class_states().resolve_field(
        owner, name, "B", true);
    if (!field) return std::unexpected(field.error());
    return machine.class_states().set_static_field(
        *field, Value::from_int(value));
}

[[nodiscard]] Status set_static_string(Machine& machine,
                                       std::string_view owner,
                                       std::string_view name,
                                       std::string_view value) {
    auto string = create_string(machine, value);
    if (!string) return std::unexpected(string.error());
    auto field = machine.class_states().resolve_field(
        owner, name, "Ljava/lang/String;", true);
    if (!field) return std::unexpected(field.error());
    return machine.class_states().set_static_field(
        *field, Value::from_reference(*string));
}

[[nodiscard]] Result<std::optional<Value>> optional_string(
    Machine& machine,
    const std::optional<std::string>& text) {
    if (!text.has_value()) {
        return std::optional<Value>(Value::from_reference(ObjectRef {}));
    }
    auto string = create_string(machine, *text);
    if (!string) return std::unexpected(string.error());
    return std::optional<Value>(Value::from_reference(*string));
}

[[nodiscard]] std::optional<unsigned> http_month(
    std::string_view name) noexcept {
    if (name.size() != 3U) return std::nullopt;
    std::array<char, 3> normalized {};
    for (usize index = 0; index < normalized.size(); ++index) {
        normalized[index] = static_cast<char>(std::tolower(
            static_cast<unsigned char>(name[index])));
    }
    constexpr std::array<std::string_view, 12> months {
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec",
    };
    const std::string_view value(normalized.data(), normalized.size());
    for (usize index = 0; index < months.size(); ++index) {
        if (months[index] == value) {
            return static_cast<unsigned>(index + 1U);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<i64> parse_http_date(
    std::string_view value) noexcept {
    const std::string text(value);
    char weekday[16] {};
    char month_name[4] {};
    char zone[8] {};
    i32 day = 0;
    i32 year = 0;
    i32 hour = 0;
    i32 minute = 0;
    i32 second = 0;

    i32 matched = std::sscanf(text.c_str(),
        "%15[^,], %d %3s %d %d:%d:%d %7s",
        weekday, &day, month_name, &year,
        &hour, &minute, &second, zone);
    if (matched != 8) {
        matched = std::sscanf(text.c_str(),
            "%15[^,], %d-%3s-%d %d:%d:%d %7s",
            weekday, &day, month_name, &year,
            &hour, &minute, &second, zone);
        if (matched == 8 && year >= 0 && year <= 99) {
            year += year >= 70 ? 1900 : 2000;
        }
    }
    if (matched != 8) {
        matched = std::sscanf(text.c_str(),
            "%15s %3s %d %d:%d:%d %d",
            weekday, month_name, &day,
            &hour, &minute, &second, &year);
        if (matched != 7) return std::nullopt;
    }

    auto month_number = http_month(month_name);
    if (!month_number.has_value() || year < 1601 || day < 1 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date {
        std::chrono::year {year},
        std::chrono::month {*month_number},
        std::chrono::day {static_cast<unsigned>(day)},
    };
    if (!date.ok()) return std::nullopt;
    const auto point = std::chrono::sys_days {date} +
        std::chrono::hours {hour} +
        std::chrono::minutes {minute} +
        std::chrono::seconds {std::min(second, 59)};
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        point.time_since_epoch()).count();
}

[[nodiscard]] Result<ObjectRef> create_security_info(
    Machine& machine,
    const network::SecurityMetadata& metadata) {
    auto certificate = machine.class_states().allocate_instance(
        machine.heap(), kCertificateClass);
    if (!certificate) return std::unexpected(certificate.error());
    auto subject = create_string(machine, metadata.certificate_subject);
    auto issuer = create_string(machine, metadata.certificate_issuer);
    auto serial = create_string(machine, metadata.certificate_serial);
    if (!subject) return std::unexpected(subject.error());
    if (!issuer) return std::unexpected(issuer.error());
    if (!serial) return std::unexpected(serial.error());
    auto subject_stored = set_reference_field(
        machine, *certificate, kCertificateSubjectField, *subject);
    auto issuer_stored = set_reference_field(
        machine, *certificate, kCertificateIssuerField, *issuer);
    auto serial_stored = set_reference_field(
        machine, *certificate, kCertificateSerialField, *serial);
    auto not_before_stored = set_long_field(
        machine, *certificate, kCertificateNotBeforeField,
        metadata.certificate_not_before);
    auto not_after_stored = set_long_field(
        machine, *certificate, kCertificateNotAfterField,
        metadata.certificate_not_after);
    if (!subject_stored) return std::unexpected(subject_stored.error());
    if (!issuer_stored) return std::unexpected(issuer_stored.error());
    if (!serial_stored) return std::unexpected(serial_stored.error());
    if (!not_before_stored)
        return std::unexpected(not_before_stored.error());
    if (!not_after_stored) return std::unexpected(not_after_stored.error());

    auto security = machine.class_states().allocate_instance(
        machine.heap(), kSecurityInfoClass);
    if (!security) return std::unexpected(security.error());
    auto protocol_name = create_string(machine, metadata.protocol_name);
    auto protocol_version = create_string(machine, metadata.protocol_version);
    auto cipher_suite = create_string(machine, metadata.cipher_suite);
    if (!protocol_name) return std::unexpected(protocol_name.error());
    if (!protocol_version) return std::unexpected(protocol_version.error());
    if (!cipher_suite) return std::unexpected(cipher_suite.error());
    auto certificate_stored = set_reference_field(
        machine, *security, kSecurityCertificateField, *certificate);
    auto protocol_name_stored = set_reference_field(
        machine, *security, kSecurityProtocolNameField, *protocol_name);
    auto protocol_version_stored = set_reference_field(
        machine, *security, kSecurityProtocolVersionField, *protocol_version);
    auto cipher_stored = set_reference_field(
        machine, *security, kSecurityCipherSuiteField, *cipher_suite);
    if (!certificate_stored)
        return std::unexpected(certificate_stored.error());
    if (!protocol_name_stored)
        return std::unexpected(protocol_name_stored.error());
    if (!protocol_version_stored)
        return std::unexpected(protocol_version_stored.error());
    if (!cipher_stored) return std::unexpected(cipher_stored.error());
    return *security;
}

[[nodiscard]] Result<network::ConnectionToken> token_from_object(
    Machine& machine,
    ObjectRef object) {
    auto id = int_field(machine, object, kHandleIdField);
    auto generation = int_field(machine, object, kHandleGenerationField);
    if (!id || !generation) {
        return fail(ErrorCode::invalid_state,
                    "network handle fields are invalid");
    }
    const network::ConnectionToken token {
        .id = *id,
        .generation = *generation,
    };
    if (!token.valid()) {
        return fail_java("java/io/IOException",
                         "network connection is closed");
    }
    return token;
}

[[nodiscard]] Status store_token(Machine& machine,
                                 ObjectRef object,
                                 network::ConnectionToken token) {
    auto id = set_int_field(machine, object, kHandleIdField, token.id);
    if (!id) return id;
    return set_int_field(machine, object, kHandleGenerationField,
                         token.generation);
}

[[nodiscard]] std::string concrete_connection_class(
    network::ConnectionKind kind) {
    switch (kind) {
    case network::ConnectionKind::stream: return std::string(kSocketClass);
    case network::ConnectionKind::server: return std::string(kServerClass);
    case network::ConnectionKind::datagram:
        return std::string(kDatagramConnectionClass);
    case network::ConnectionKind::http: return std::string(kHttpClass);
    case network::ConnectionKind::https: return std::string(kHttpsClass);
    }
    return std::string(kSocketClass);
}

[[nodiscard]] Result<ObjectRef> create_connection_object(
    Machine& machine,
    const network::OpenedConnection& opened) {
    auto object = machine.class_states().allocate_instance(
        machine.heap(), concrete_connection_class(opened.kind));
    if (!object) {
        (void)machine.connections().close(opened.token);
        return std::unexpected(object.error());
    }
    auto stored = store_token(machine, *object, opened.token);
    if (!stored) {
        (void)machine.connections().close(opened.token);
        return std::unexpected(stored.error());
    }
    return *object;
}

[[nodiscard]] Result<std::optional<ObjectRef>>
try_open_wireless_connection(Machine& machine,
                             ObjectRef url,
                             std::string_view text,
                             i32 mode) {
    std::string_view protocol;
    std::string_view send_permission;
    std::string_view receive_permission;
    if (text.starts_with("sms://")) {
        protocol = "sms";
        send_permission = security::permissions::wireless_sms_send;
        receive_permission = security::permissions::wireless_sms_receive;
    } else if (text.starts_with("mms://")) {
        protocol = "mms";
        send_permission = security::permissions::wireless_mms_send;
        receive_permission = security::permissions::wireless_mms_receive;
    } else if (text.starts_with("cbs://")) {
        protocol = "cbs";
        receive_permission = security::permissions::wireless_cbs_receive;
    } else {
        return std::optional<ObjectRef> {};
    }

    if (mode < 1 || mode > 3) {
        return fail_java("java/lang/IllegalArgumentException",
                         "wireless connection mode must be READ, WRITE or "
                         "READ_WRITE");
    }
    if ((mode & 2) != 0 && send_permission.empty()) {
        return fail_java("java/lang/IllegalArgumentException",
                         "CBS connections are receive-only");
    }
    if ((mode & 1) != 0 && !receive_permission.empty()) {
        auto permitted = machine.permission_policy().require(
            receive_permission, std::string(text));
        if (!permitted) return std::unexpected(permitted.error());
    }
    if ((mode & 2) != 0 && !send_permission.empty()) {
        auto permitted = machine.permission_policy().require(
            send_permission, std::string(text));
        if (!permitted) return std::unexpected(permitted.error());
    }

    auto connection = machine.class_states().allocate_instance(
        machine.heap(), kWirelessConnectionClass);
    if (!connection) return std::unexpected(connection.error());
    auto protocol_string = create_string(machine, protocol);
    if (!protocol_string) return std::unexpected(protocol_string.error());
    auto address_stored = set_reference_field(
        machine, *connection, kWirelessConnectionAddressField, url);
    auto mode_stored = set_int_field(
        machine, *connection, kWirelessConnectionModeField, mode);
    auto closed_stored = set_int_field(
        machine, *connection, kWirelessConnectionClosedField, 0);
    auto protocol_stored = set_reference_field(
        machine, *connection, kWirelessConnectionProtocolField,
        *protocol_string);
    if (!address_stored) return std::unexpected(address_stored.error());
    if (!mode_stored) return std::unexpected(mode_stored.error());
    if (!closed_stored) return std::unexpected(closed_stored.error());
    if (!protocol_stored) return std::unexpected(protocol_stored.error());
    return std::optional<ObjectRef>(*connection);
}

[[nodiscard]] Status require_network_permission(
    Machine& machine,
    std::string_view url) {
    auto parsed = network::Url::parse(url);
    if (!parsed) return {};

    std::string_view permission;
    switch (parsed->scheme) {
    case network::Scheme::socket:
        permission = parsed->server_endpoint
            ? security::permissions::connector_server_socket
            : security::permissions::connector_socket;
        break;
    case network::Scheme::server_socket:
        permission = security::permissions::connector_server_socket;
        break;
    case network::Scheme::datagram:
        permission = parsed->server_endpoint
            ? security::permissions::connector_datagram_receiver
            : security::permissions::connector_datagram;
        break;
    case network::Scheme::http:
        permission = security::permissions::connector_http;
        break;
    case network::Scheme::https:
        permission = security::permissions::connector_https;
        break;
    }
    return machine.permission_policy().require(permission, std::string(url));
}

[[nodiscard]] Result<ObjectRef> open_connection(
    Machine& machine,
    ObjectRef url,
    i32 mode,
    bool timeouts) {
    auto file_connection = try_open_file_connection(machine, url, mode);
    if (!file_connection) return std::unexpected(file_connection.error());
    if (file_connection->has_value()) return **file_connection;

    auto text = utf8_text(machine, url);
    if (!text) return std::unexpected(text.error());
    if (text->starts_with("sensor:")) {
        return open_sensor_connection(machine, url, *text, mode);
    }
    auto wireless = try_open_wireless_connection(
        machine, url, *text, mode);
    if (!wireless) return std::unexpected(wireless.error());
    if (wireless->has_value()) return **wireless;
    auto permitted = require_network_permission(machine, *text);
    if (!permitted) return std::unexpected(permitted.error());
    auto opened = java_network_result(machine.connections().open(
        *text, static_cast<network::ConnectionMode>(mode), timeouts));
    if (!opened) return std::unexpected(opened.error());
    return create_connection_object(machine, *opened);
}

[[nodiscard]] Result<ObjectRef> create_native_stream(
    Machine& machine,
    network::ConnectionToken token,
    bool input,
    bool owns_connection) {
    auto opened = input
        ? java_network_status(machine.connections().open_input(token))
        : java_network_status(machine.connections().open_output(token));
    if (!opened) return std::unexpected(opened.error());

    auto stream = machine.class_states().allocate_instance(
        machine.heap(), input ? kInputStreamClass : kOutputStreamClass);
    if (!stream) {
        if (input) (void)machine.connections().close_input(token);
        else (void)machine.connections().close_output(token);
        return std::unexpected(stream.error());
    }
    auto stored = store_token(machine, *stream, token);
    if (!stored) return std::unexpected(stored.error());
    auto closed = set_int_field(machine, *stream, kStreamClosedField, 0);
    if (!closed) return std::unexpected(closed.error());
    auto owns = set_int_field(machine, *stream, kStreamOwnsConnectionField,
                              owns_connection ? 1 : 0);
    if (!owns) return std::unexpected(owns.error());
    if (input) {
        auto buffer = set_reference_field(machine, *stream,
                                          kStreamBufferField, {});
        auto position = set_int_field(machine, *stream,
                                      kStreamBufferPositionField, 0);
        auto count = set_int_field(machine, *stream,
                                   kStreamBufferCountField, 0);
        if (!buffer) return std::unexpected(buffer.error());
        if (!position) return std::unexpected(position.error());
        if (!count) return std::unexpected(count.error());
    }
    return *stream;
}

[[nodiscard]] Result<ObjectRef> wrap_data_stream(Machine& machine,
                                                 ObjectRef native_stream,
                                                 bool input) {
    const std::string_view class_name = input
        ? std::string_view("java/io/DataInputStream")
        : std::string_view("java/io/DataOutputStream");
    auto wrapper = machine.class_states().allocate_instance(
        machine.heap(), class_name);
    if (!wrapper) return std::unexpected(wrapper.error());
    auto linked = set_reference_field(machine, *wrapper, 0, native_stream);
    if (!linked) return std::unexpected(linked.error());
    if (!input) {
        auto written = set_int_field(machine, *wrapper, 1, 0);
        if (!written) return std::unexpected(written.error());
    }
    return *wrapper;
}

[[nodiscard]] Result<ObjectRef> open_stream_from_connection(
    Machine& machine,
    ObjectRef connection,
    bool input,
    bool data,
    bool owns_connection) {
    auto file_stream = try_open_file_connection_stream(
        machine, connection, input, data);
    if (!file_stream) return std::unexpected(file_stream.error());
    if (file_stream->has_value()) return **file_stream;

    auto token = token_from_object(machine, connection);
    if (!token) return std::unexpected(token.error());
    auto stream = create_native_stream(machine, *token, input,
                                       owns_connection);
    if (!stream) return std::unexpected(stream.error());
    if (!data) return *stream;
    return wrap_data_stream(machine, *stream, input);
}

[[nodiscard]] Result<bool> stream_closed(Machine& machine,
                                         ObjectRef stream) {
    auto closed = int_field(machine, stream, kStreamClosedField);
    if (!closed) return std::unexpected(closed.error());
    return *closed != 0;
}

[[nodiscard]] Status require_open_stream(Machine& machine,
                                         ObjectRef stream) {
    auto closed = stream_closed(machine, stream);
    if (!closed) return std::unexpected(closed.error());
    if (*closed) {
        return fail_java("java/io/IOException", "network stream is closed");
    }
    return {};
}

[[nodiscard]] Status close_native_stream(Machine& machine,
                                         ObjectRef stream,
                                         bool input) {
    auto closed = stream_closed(machine, stream);
    if (!closed) return std::unexpected(closed.error());
    if (*closed) return {};
    auto token = token_from_object(machine, stream);
    if (!token) return std::unexpected(token.error());
    auto owns = int_field(machine, stream, kStreamOwnsConnectionField);
    if (!owns) return std::unexpected(owns.error());
    auto marked = set_int_field(machine, stream, kStreamClosedField, 1);
    if (!marked) return marked;
    auto released = input
        ? java_network_status(machine.connections().close_input(*token))
        : java_network_status(machine.connections().close_output(*token));
    if (!released) return released;
    if (*owns != 0) {
        return java_network_status(machine.connections().close(*token));
    }
    return {};
}

[[nodiscard]] Result<usize> byte_array_length(Machine& machine,
                                              ObjectRef array) {
    if (array.is_null()) {
        return fail_java("java/lang/NullPointerException", "byte[] is null");
    }
    auto class_name = machine.heap().class_name(array);
    if (!class_name || *class_name != "[B") {
        return fail_java("java/lang/IllegalArgumentException",
                         "value is not byte[]");
    }
    return machine.heap().array_length(array);
}

[[nodiscard]] Status validate_range(usize array_length,
                                    i32 offset,
                                    i32 length) {
    if (offset < 0 || length < 0 ||
        static_cast<usize>(offset) > array_length ||
        static_cast<usize>(length) >
            array_length - static_cast<usize>(offset)) {
        return fail_java("java/lang/IndexOutOfBoundsException",
                         "byte[] range is invalid");
    }
    return {};
}

[[nodiscard]] Result<u8> byte_at(Machine& machine,
                                 ObjectRef array,
                                 usize index) {
    auto value = machine.heap().element(array, index);
    if (!value) return std::unexpected(value.error());
    auto integer = value->as_int();
    if (!integer) return std::unexpected(integer.error());
    return static_cast<u8>(static_cast<i8>(*integer));
}

[[nodiscard]] Status set_byte(Machine& machine,
                              ObjectRef array,
                              usize index,
                              u8 byte) {
    return machine.heap().set_element(
        array, index,
        Value::from_int(static_cast<i32>(static_cast<i8>(byte))));
}

[[nodiscard]] Result<std::vector<u8>> byte_slice(Machine& machine,
                                                 ObjectRef array,
                                                 i32 offset,
                                                 i32 length) {
    auto array_length = byte_array_length(machine, array);
    if (!array_length) return std::unexpected(array_length.error());
    auto range = validate_range(*array_length, offset, length);
    if (!range) return std::unexpected(range.error());
    std::vector<u8> bytes;
    bytes.reserve(static_cast<usize>(length));
    for (i32 index = 0; index < length; ++index) {
        auto byte = byte_at(machine, array,
                            static_cast<usize>(offset + index));
        if (!byte) return std::unexpected(byte.error());
        bytes.push_back(*byte);
    }
    return bytes;
}

[[nodiscard]] Result<std::optional<i32>> native_read_one(
    Machine& machine,
    ObjectRef stream) {
    auto is_native = machine.object_is_instance(stream, kInputStreamClass);
    if (!is_native) return std::unexpected(is_native.error());
    if (!*is_native) return std::optional<i32> {};
    auto open = require_open_stream(machine, stream);
    if (!open) return std::unexpected(open.error());

    // phoneME leaves socket read-ahead disabled unless the internal
    // com.sun.midp.io.j2me.socket.buffersize property explicitly enables it.
    // Pass the exact Java-requested length to the native adapter by default.
    auto token = token_from_object(machine, stream);
    if (!token) return std::unexpected(token.error());
    auto bytes = java_network_result(machine.connections().read(*token, 1U));
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->empty()) return std::optional<i32>(-1);
    return std::optional<i32>(static_cast<i32>(bytes->front()));
}

[[nodiscard]] Result<i32> native_read_range(Machine& machine,
                                            ObjectRef stream,
                                            ObjectRef destination,
                                            i32 offset,
                                            i32 length) {
    auto destination_length = byte_array_length(machine, destination);
    if (!destination_length) return std::unexpected(destination_length.error());
    auto range = validate_range(*destination_length, offset, length);
    if (!range) return std::unexpected(range.error());
    if (length == 0) return 0;
    auto open = require_open_stream(machine, stream);
    if (!open) return std::unexpected(open.error());

    auto token = token_from_object(machine, stream);
    if (!token) return std::unexpected(token.error());
    auto bytes = java_network_result(machine.connections().read(
        *token, static_cast<usize>(length)));
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->empty()) return -1;

    auto stored = machine.heap().write_byte_array(
        destination, static_cast<usize>(offset), *bytes);
    if (!stored) return std::unexpected(stored.error());
    return static_cast<i32>(bytes->size());
}

[[nodiscard]] Status native_write_range(Machine& machine,
                                        ObjectRef stream,
                                        ObjectRef source,
                                        i32 offset,
                                        i32 length) {
    auto open = require_open_stream(machine, stream);
    if (!open) return open;
    auto bytes = byte_slice(machine, source, offset, length);
    if (!bytes) return std::unexpected(bytes.error());
    auto token = token_from_object(machine, stream);
    if (!token) return std::unexpected(token.error());
    return java_network_status(machine.connections().write(*token, *bytes));
}

[[nodiscard]] Result<std::optional<Value>> close_connection(
    Machine& machine,
    std::span<const Value> arguments) {
    auto connection = receiver(arguments, "Connection.close");
    if (!connection) return std::unexpected(connection.error());
    auto file_closed = try_close_file_connection(machine, *connection);
    if (!file_closed) return std::unexpected(file_closed.error());
    if (file_closed->has_value()) return std::optional<Value> {};

    auto token = token_from_object(machine, *connection);
    if (!token) return std::optional<Value> {};
    auto closed = java_network_status(machine.connections().close(*token));
    if (!closed) return std::unexpected(closed.error());
    (void)set_int_field(machine, *connection, kHandleIdField, 0);
    (void)set_int_field(machine, *connection, kHandleGenerationField, 0);
    return std::optional<Value> {};
}

void register_java_net_socket(NativeMethodRegistry& registry) {
    add(registry, std::string(kJavaNetSocketClass), "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto socket = receiver(arguments, "Socket.<init>");
            if (!socket) return std::unexpected(socket.error());
            auto id = set_int_field(machine, *socket, kHandleIdField, 0);
            if (!id) return std::unexpected(id.error());
            auto generation = set_int_field(
                machine, *socket, kHandleGenerationField, 0);
            if (!generation) return std::unexpected(generation.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kJavaNetSocketClass), "<init>",
        "(Ljava/lang/String;I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto socket = receiver(arguments, "Socket.<init>");
            auto host_object = reference_argument(arguments, 1, "socket host");
            auto port = int_argument(arguments, 2, "socket port");
            if (!socket) return std::unexpected(socket.error());
            if (!host_object) return std::unexpected(host_object.error());
            if (!port) return std::unexpected(port.error());
            if (*port <= 0 || *port > 65'535) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "socket port is outside 1..65535");
            }
            auto host = utf8_text(machine, *host_object);
            if (!host) return std::unexpected(host.error());
            if (host->empty()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "socket host is empty");
            }
            std::string endpoint;
            endpoint.reserve(host->size() + 24U);
            endpoint.append("socket://");
            const bool ipv6_literal = host->find(':') != std::string::npos &&
                !(host->starts_with('[') && host->ends_with(']'));
            if (ipv6_literal) endpoint.push_back('[');
            endpoint.append(*host);
            if (ipv6_literal) endpoint.push_back(']');
            endpoint.push_back(':');
            endpoint.append(std::to_string(*port));

            auto permitted = require_network_permission(machine, endpoint);
            if (!permitted) return std::unexpected(permitted.error());
            auto opened = java_network_result(machine.connections().open(
                endpoint, network::ConnectionMode::read_write, true));
            if (!opened) return std::unexpected(opened.error());
            if (opened->kind != network::ConnectionKind::stream) {
                (void)machine.connections().close(opened->token);
                return fail_java("java/io/IOException",
                                 "java.net.Socket requires a stream connection");
            }
            auto stored = store_token(machine, *socket, opened->token);
            if (!stored) {
                (void)machine.connections().close(opened->token);
                return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
    add(registry, std::string(kJavaNetSocketClass), "getInputStream",
        "()Ljava/io/InputStream;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto socket = receiver(arguments, "Socket.getInputStream");
            if (!socket) return std::unexpected(socket.error());
            auto stream = open_stream_from_connection(
                machine, *socket, true, false, false);
            if (!stream) return std::unexpected(stream.error());
            return std::optional<Value>(Value::from_reference(*stream));
        });
    add(registry, std::string(kJavaNetSocketClass), "getOutputStream",
        "()Ljava/io/OutputStream;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto socket = receiver(arguments, "Socket.getOutputStream");
            if (!socket) return std::unexpected(socket.error());
            auto stream = open_stream_from_connection(
                machine, *socket, false, false, false);
            if (!stream) return std::unexpected(stream.error());
            return std::optional<Value>(Value::from_reference(*stream));
        });
    add(registry, std::string(kJavaNetSocketClass), "close", "()V",
        close_connection);
    add(registry, std::string(kJavaNetSocketClass), "isClosed", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto socket = receiver(arguments, "Socket.isClosed");
            if (!socket) return std::unexpected(socket.error());
            auto id = int_field(machine, *socket, kHandleIdField);
            auto generation = int_field(
                machine, *socket, kHandleGenerationField);
            if (!id) return std::unexpected(id.error());
            if (!generation) return std::unexpected(generation.error());
            return std::optional<Value>(Value::from_int(
                *id <= 0 || *generation <= 0 ? 1 : 0));
        });
}

void register_connector(NativeMethodRegistry& registry) {
    add(registry, "javax/microedition/io/Connector", "open",
        "(Ljava/lang/String;)Ljavax/microedition/io/Connection;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto url = reference_argument(arguments, 0, "Connector URL");
            if (!url) return std::unexpected(url.error());
            auto connection = open_connection(machine, *url, 3, false);
            if (!connection) return std::unexpected(connection.error());
            return std::optional<Value>(Value::from_reference(*connection));
        });
    add(registry, "javax/microedition/io/Connector", "open",
        "(Ljava/lang/String;I)Ljavax/microedition/io/Connection;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto url = reference_argument(arguments, 0, "Connector URL");
            auto mode = int_argument(arguments, 1, "Connector mode");
            if (!url) return std::unexpected(url.error());
            if (!mode) return std::unexpected(mode.error());
            auto connection = open_connection(machine, *url, *mode, false);
            if (!connection) return std::unexpected(connection.error());
            return std::optional<Value>(Value::from_reference(*connection));
        });
    add(registry, "javax/microedition/io/Connector", "open",
        "(Ljava/lang/String;IZ)Ljavax/microedition/io/Connection;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto url = reference_argument(arguments, 0, "Connector URL");
            auto mode = int_argument(arguments, 1, "Connector mode");
            auto timeouts = int_argument(arguments, 2, "timeout flag");
            if (!url) return std::unexpected(url.error());
            if (!mode) return std::unexpected(mode.error());
            if (!timeouts) return std::unexpected(timeouts.error());
            auto connection = open_connection(machine, *url, *mode,
                                               *timeouts != 0);
            if (!connection) return std::unexpected(connection.error());
            return std::optional<Value>(Value::from_reference(*connection));
        });

    const auto convenience = [&registry](std::string method,
                                         std::string descriptor,
                                         bool input,
                                         bool data) {
        add(registry, "javax/microedition/io/Connector", std::move(method),
            std::move(descriptor),
            [input, data](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto url = reference_argument(arguments, 0, "Connector URL");
                if (!url) return std::unexpected(url.error());
                auto connection = open_connection(machine, *url,
                                                   input ? 1 : 2, false);
                if (!connection) return std::unexpected(connection.error());
                auto stream = open_stream_from_connection(
                    machine, *connection, input, data, true);
                if (!stream) return std::unexpected(stream.error());
                auto file_closed = try_close_file_connection(
                    machine, *connection);
                if (!file_closed) return std::unexpected(file_closed.error());
                if (!file_closed->has_value()) {
                    auto token = token_from_object(machine, *connection);
                    if (token) (void)machine.connections().close(*token);
                }
                return std::optional<Value>(Value::from_reference(*stream));
            });
    };
    convenience("openInputStream",
                "(Ljava/lang/String;)Ljava/io/InputStream;", true, false);
    convenience("openDataInputStream",
                "(Ljava/lang/String;)Ljava/io/DataInputStream;", true, true);
    convenience("openOutputStream",
                "(Ljava/lang/String;)Ljava/io/OutputStream;", false, false);
    convenience("openDataOutputStream",
                "(Ljava/lang/String;)Ljava/io/DataOutputStream;", false, true);
}

void register_stream_connections(NativeMethodRegistry& registry) {
    const std::array<std::string_view, 3> owners {
        kSocketClass, kHttpClass, kHttpsClass,
    };
    for (const auto owner : owners) {
        add(registry, std::string(owner), "close", "()V", close_connection);
        const auto open_stream = [&registry, owner](std::string method,
                                                    std::string descriptor,
                                                    bool input,
                                                    bool data) {
            add(registry, std::string(owner), std::move(method),
                std::move(descriptor),
                [input, data](Machine& machine,
                              std::span<const Value> arguments)
                    -> Result<std::optional<Value>> {
                    auto connection = receiver(arguments, "open stream");
                    if (!connection) return std::unexpected(connection.error());
                    auto stream = open_stream_from_connection(
                        machine, *connection, input, data, false);
                    if (!stream) return std::unexpected(stream.error());
                    return std::optional<Value>(Value::from_reference(*stream));
                });
        };
        open_stream("openInputStream", "()Ljava/io/InputStream;", true, false);
        open_stream("openDataInputStream", "()Ljava/io/DataInputStream;",
                    true, true);
        open_stream("openOutputStream", "()Ljava/io/OutputStream;",
                    false, false);
        open_stream("openDataOutputStream", "()Ljava/io/DataOutputStream;",
                    false, true);
    }
}

void register_native_streams(NativeMethodRegistry& registry) {
    add(registry, std::string(kInputStreamClass), "read", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto stream = receiver(arguments, "InputStream.read");
            if (!stream) return std::unexpected(stream.error());
            auto value = native_read_one(machine, *stream);
            if (!value) return std::unexpected(value.error());
            if (!value->has_value()) {
                return fail(ErrorCode::invalid_state,
                            "receiver is not a network input stream");
            }
            return std::optional<Value>(Value::from_int(**value));
        });
    const auto read_array = [&registry](bool with_range) {
        add(registry, std::string(kInputStreamClass), "read",
            with_range ? "([BII)I" : "([B)I",
            [with_range](Machine& machine,
                         std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto stream = receiver(arguments, "InputStream.read");
                auto destination = reference_argument(arguments, 1,
                                                      "read destination");
                if (!stream) return std::unexpected(stream.error());
                if (!destination) return std::unexpected(destination.error());
                auto array_length = byte_array_length(machine, *destination);
                if (!array_length) return std::unexpected(array_length.error());
                if (*array_length > static_cast<usize>(
                                        std::numeric_limits<i32>::max())) {
                    return fail_java("java/lang/OutOfMemoryError",
                                     "byte[] exceeds Java int length");
                }
                i32 offset = 0;
                i32 length = static_cast<i32>(*array_length);
                if (with_range) {
                    auto parsed_offset = int_argument(arguments, 2, "offset");
                    auto parsed_length = int_argument(arguments, 3, "length");
                    if (!parsed_offset)
                        return std::unexpected(parsed_offset.error());
                    if (!parsed_length)
                        return std::unexpected(parsed_length.error());
                    offset = *parsed_offset;
                    length = *parsed_length;
                }
                auto count = native_read_range(machine, *stream,
                                               *destination, offset, length);
                if (!count) return std::unexpected(count.error());
                return std::optional<Value>(Value::from_int(*count));
            });
    };
    read_array(false);
    read_array(true);
    add(registry, std::string(kInputStreamClass), "skip", "(J)J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto stream = receiver(arguments, "InputStream.skip");
            auto requested = arguments[1].as_long();
            if (!stream) return std::unexpected(stream.error());
            if (!requested) return std::unexpected(requested.error());
            if (*requested <= 0) {
                return std::optional<Value>(Value::from_long(0));
            }
            auto open = require_open_stream(machine, *stream);
            if (!open) return std::unexpected(open.error());
            auto position = int_field(machine, *stream,
                                      kStreamBufferPositionField);
            auto count = int_field(machine, *stream,
                                   kStreamBufferCountField);
            if (!position) return std::unexpected(position.error());
            if (!count) return std::unexpected(count.error());
            if (*position < 0 || *count < *position) {
                return fail(ErrorCode::invalid_state,
                            "network input buffer state is invalid");
            }
            i64 skipped = std::min<i64>(*requested,
                                        *count - *position);
            if (skipped > 0) {
                auto advanced = set_int_field(
                    machine, *stream, kStreamBufferPositionField,
                    *position + static_cast<i32>(skipped));
                if (!advanced) return std::unexpected(advanced.error());
            }
            if (skipped >= *requested) {
                return std::optional<Value>(Value::from_long(skipped));
            }
            auto token = token_from_object(machine, *stream);
            if (!token) return std::unexpected(token.error());
            while (skipped < *requested) {
                const usize amount = static_cast<usize>(std::min<i64>(
                    *requested - skipped, 4'096));
                auto bytes = java_network_result(
                    machine.connections().read(*token, amount));
                if (!bytes) return std::unexpected(bytes.error());
                if (bytes->empty()) break;
                skipped += static_cast<i64>(bytes->size());
            }
            return std::optional<Value>(Value::from_long(skipped));
        });
    add(registry, std::string(kInputStreamClass), "available", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto stream = receiver(arguments, "InputStream.available");
            if (!stream) return std::unexpected(stream.error());
            auto available = connection_stream_available(machine, *stream);
            if (!available) return std::unexpected(available.error());
            if (!available->has_value()) {
                return fail(ErrorCode::invalid_state,
                            "receiver is not a network input stream");
            }
            return std::optional<Value>(Value::from_int(static_cast<i32>(
                std::min(**available,
                         static_cast<usize>(std::numeric_limits<i32>::max())))));
        });
    add(registry, std::string(kInputStreamClass), "close", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto stream = receiver(arguments, "InputStream.close");
            if (!stream) return std::unexpected(stream.error());
            auto closed = close_native_stream(machine, *stream, true);
            if (!closed) return std::unexpected(closed.error());
            return std::optional<Value> {};
        });

    add(registry, std::string(kOutputStreamClass), "write", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto stream = receiver(arguments, "OutputStream.write");
            auto value = int_argument(arguments, 1, "byte");
            if (!stream) return std::unexpected(stream.error());
            if (!value) return std::unexpected(value.error());
            const std::array<u8, 1> byte {
                static_cast<u8>(*value & 0xFF),
            };
            auto token = token_from_object(machine, *stream);
            if (!token) return std::unexpected(token.error());
            auto written = java_network_status(
                machine.connections().write(*token, byte));
            if (!written) return std::unexpected(written.error());
            return std::optional<Value> {};
        });
    const auto write_array = [&registry](bool with_range) {
        add(registry, std::string(kOutputStreamClass), "write",
            with_range ? "([BII)V" : "([B)V",
            [with_range](Machine& machine,
                         std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto stream = receiver(arguments, "OutputStream.write");
                auto source = reference_argument(arguments, 1, "write source");
                if (!stream) return std::unexpected(stream.error());
                if (!source) return std::unexpected(source.error());
                auto size = byte_array_length(machine, *source);
                if (!size) return std::unexpected(size.error());
                if (*size > static_cast<usize>(
                                std::numeric_limits<i32>::max())) {
                    return fail_java("java/lang/OutOfMemoryError",
                                     "byte[] exceeds Java int length");
                }
                i32 offset = 0;
                i32 length = static_cast<i32>(*size);
                if (with_range) {
                    auto parsed_offset = int_argument(arguments, 2, "offset");
                    auto parsed_length = int_argument(arguments, 3, "length");
                    if (!parsed_offset)
                        return std::unexpected(parsed_offset.error());
                    if (!parsed_length)
                        return std::unexpected(parsed_length.error());
                    offset = *parsed_offset;
                    length = *parsed_length;
                }
                auto written = native_write_range(machine, *stream,
                                                  *source, offset, length);
                if (!written) return std::unexpected(written.error());
                return std::optional<Value> {};
            });
    };
    write_array(false);
    write_array(true);
    add(registry, std::string(kOutputStreamClass), "flush", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto stream = receiver(arguments, "OutputStream.flush");
            if (!stream) return std::unexpected(stream.error());
            auto token = token_from_object(machine, *stream);
            if (!token) return std::unexpected(token.error());
            auto flushed = java_network_status(
                machine.connections().flush(*token));
            if (!flushed) return std::unexpected(flushed.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kOutputStreamClass), "close", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto stream = receiver(arguments, "OutputStream.close");
            if (!stream) return std::unexpected(stream.error());
            auto closed = close_native_stream(machine, *stream, false);
            if (!closed) return std::unexpected(closed.error());
            return std::optional<Value> {};
        });
}

[[nodiscard]] Result<network::SocketOption> parse_socket_option(i32 option) {
    switch (option) {
    case 0: return network::SocketOption::delay;
    case 1: return network::SocketOption::linger;
    case 2: return network::SocketOption::keep_alive;
    case 3: return network::SocketOption::receive_buffer;
    case 4: return network::SocketOption::send_buffer;
    default:
        return fail_java("java/lang/IllegalArgumentException",
                         "unknown SocketConnection option");
    }
}

void register_socket_connection(NativeMethodRegistry& registry) {
    const auto endpoint_getter = [&registry](std::string method,
                                             bool local,
                                             bool address) {
        add(registry, std::string(kSocketClass), std::move(method),
            address ? "()Ljava/lang/String;" : "()I",
            [local, address](Machine& machine,
                             std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "socket endpoint");
                if (!connection) return std::unexpected(connection.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto endpoint = local
                    ? java_network_result(
                          machine.connections().local_endpoint(*token))
                    : java_network_result(
                          machine.connections().remote_endpoint(*token));
                if (!endpoint) return std::unexpected(endpoint.error());
                if (!address) {
                    return std::optional<Value>(Value::from_int(
                        static_cast<i32>(endpoint->port)));
                }
                auto text = create_string(machine, endpoint->host);
                if (!text) return std::unexpected(text.error());
                return std::optional<Value>(Value::from_reference(*text));
            });
    };
    endpoint_getter("getAddress", false, true);
    endpoint_getter("getPort", false, false);
    endpoint_getter("getLocalAddress", true, true);
    endpoint_getter("getLocalPort", true, false);

    add(registry, std::string(kSocketClass), "setSocketOption", "(BI)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(arguments, "setSocketOption");
            auto option_value = int_argument(arguments, 1, "socket option");
            auto value = int_argument(arguments, 2, "socket option value");
            if (!connection) return std::unexpected(connection.error());
            if (!option_value) return std::unexpected(option_value.error());
            if (!value) return std::unexpected(value.error());
            auto option = parse_socket_option(*option_value);
            if (!option) return std::unexpected(option.error());
            auto token = token_from_object(machine, *connection);
            if (!token) return std::unexpected(token.error());
            auto changed = java_network_status(
                machine.connections().set_socket_option(*token, *option,
                                                        *value));
            if (!changed) return std::unexpected(changed.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kSocketClass), "getSocketOption", "(B)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(arguments, "getSocketOption");
            auto option_value = int_argument(arguments, 1, "socket option");
            if (!connection) return std::unexpected(connection.error());
            if (!option_value) return std::unexpected(option_value.error());
            auto option = parse_socket_option(*option_value);
            if (!option) return std::unexpected(option.error());
            auto token = token_from_object(machine, *connection);
            if (!token) return std::unexpected(token.error());
            auto value = java_network_result(
                machine.connections().socket_option(*token, *option));
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
}

void register_server_connection(NativeMethodRegistry& registry) {
    add(registry, std::string(kServerClass), "close", "()V",
        close_connection);
    add(registry, std::string(kServerClass), "acceptAndOpen",
        "()Ljavax/microedition/io/StreamConnection;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto server = receiver(arguments, "acceptAndOpen");
            if (!server) return std::unexpected(server.error());
            auto token = token_from_object(machine, *server);
            if (!token) return std::unexpected(token.error());
            auto accepted = java_network_result(
                machine.connections().accept(*token));
            if (!accepted) return std::unexpected(accepted.error());
            auto object = create_connection_object(machine, *accepted);
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });
    add(registry, std::string(kServerClass), "getLocalAddress",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto server = receiver(arguments, "getLocalAddress");
            if (!server) return std::unexpected(server.error());
            auto token = token_from_object(machine, *server);
            if (!token) return std::unexpected(token.error());
            auto endpoint = java_network_result(
                machine.connections().local_endpoint(*token));
            if (!endpoint) return std::unexpected(endpoint.error());
            auto text = create_string(machine, endpoint->host);
            if (!text) return std::unexpected(text.error());
            return std::optional<Value>(Value::from_reference(*text));
        });
    add(registry, std::string(kServerClass), "getLocalPort", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto server = receiver(arguments, "getLocalPort");
            if (!server) return std::unexpected(server.error());
            auto token = token_from_object(machine, *server);
            if (!token) return std::unexpected(token.error());
            auto endpoint = java_network_result(
                machine.connections().local_endpoint(*token));
            if (!endpoint) return std::unexpected(endpoint.error());
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(endpoint->port)));
        });
}

[[nodiscard]] Result<ObjectRef> allocate_byte_array(Machine& machine,
                                                     usize length) {
    return machine.heap().allocate_array("[B", length, Value::from_int(0));
}

[[nodiscard]] Result<ObjectRef> create_datagram(Machine& machine,
                                                ObjectRef data,
                                                i32 offset,
                                                i32 length) {
    auto size = byte_array_length(machine, data);
    if (!size) return std::unexpected(size.error());
    auto range = validate_range(*size, offset, length);
    if (!range) return std::unexpected(range.error());
    auto datagram = machine.class_states().allocate_instance(
        machine.heap(), kDatagramClass);
    if (!datagram) return std::unexpected(datagram.error());
    auto stored_data = set_reference_field(machine, *datagram,
                                           kDatagramDataField, data);
    auto stored_offset = set_int_field(machine, *datagram,
                                       kDatagramOffsetField, offset);
    auto stored_length = set_int_field(machine, *datagram,
                                       kDatagramLengthField, length);
    auto stored_position = set_int_field(machine, *datagram,
                                         kDatagramPositionField, 0);
    auto stored_address = set_reference_field(machine, *datagram,
                                              kDatagramAddressField,
                                              ObjectRef {});
    if (!stored_data) return std::unexpected(stored_data.error());
    if (!stored_offset) return std::unexpected(stored_offset.error());
    if (!stored_length) return std::unexpected(stored_length.error());
    if (!stored_position) return std::unexpected(stored_position.error());
    if (!stored_address) return std::unexpected(stored_address.error());
    return *datagram;
}

[[nodiscard]] Result<network::DatagramPacket> datagram_packet(
    Machine& machine,
    ObjectRef datagram) {
    auto data = reference_field(machine, datagram, kDatagramDataField);
    auto offset = int_field(machine, datagram, kDatagramOffsetField);
    auto length = int_field(machine, datagram, kDatagramLengthField);
    auto address = reference_field(machine, datagram, kDatagramAddressField);
    if (!data) return std::unexpected(data.error());
    if (!offset) return std::unexpected(offset.error());
    if (!length) return std::unexpected(length.error());
    if (!address) return std::unexpected(address.error());
    auto bytes = byte_slice(machine, *data, *offset, *length);
    if (!bytes) return std::unexpected(bytes.error());
    network::DatagramPacket packet;
    packet.bytes = std::move(*bytes);
    if (!address->is_null()) {
        auto text = utf8_text(machine, *address);
        if (!text) return std::unexpected(text.error());
        auto parsed = network::Url::parse(*text);
        if (!parsed) return as_java_network_error(parsed.error());
        if (parsed->scheme != network::Scheme::datagram ||
            parsed->host.empty()) {
            return fail_java("java/lang/IllegalArgumentException",
                             "datagram address has no remote host");
        }
        packet.peer = network::Endpoint {
            .host = parsed->host,
            .port = parsed->effective_port(),
        };
    }
    return packet;
}

[[nodiscard]] std::string datagram_url(
    const network::Endpoint& endpoint) {
    std::string result = "datagram://";
    const bool ipv6 = endpoint.host.find(':') != std::string::npos;
    if (ipv6) result.push_back('[');
    result.append(endpoint.host);
    if (ipv6) result.push_back(']');
    result.push_back(':');
    result.append(std::to_string(endpoint.port));
    return result;
}

[[nodiscard]] Result<std::vector<u8>> datagram_read_bytes(
    Machine& machine,
    ObjectRef datagram,
    usize count) {
    auto data = reference_field(machine, datagram, kDatagramDataField);
    auto offset = int_field(machine, datagram, kDatagramOffsetField);
    auto length = int_field(machine, datagram, kDatagramLengthField);
    auto position = int_field(machine, datagram, kDatagramPositionField);
    if (!data) return std::unexpected(data.error());
    if (!offset) return std::unexpected(offset.error());
    if (!length) return std::unexpected(length.error());
    if (!position) return std::unexpected(position.error());
    if (*position < 0 || *length < 0 || *position > *length ||
        count > static_cast<usize>(*length - *position)) {
        return fail_java("java/io/EOFException",
                         "datagram input reached end of data");
    }
    auto bytes = byte_slice(machine, *data,
                            *offset + *position,
                            static_cast<i32>(count));
    if (!bytes) return std::unexpected(bytes.error());
    auto advanced = set_int_field(machine, datagram,
                                  kDatagramPositionField,
                                  *position + static_cast<i32>(count));
    if (!advanced) return std::unexpected(advanced.error());
    return bytes;
}

[[nodiscard]] Status datagram_write_bytes(Machine& machine,
                                          ObjectRef datagram,
                                          std::span<const u8> bytes) {
    auto data = reference_field(machine, datagram, kDatagramDataField);
    auto offset = int_field(machine, datagram, kDatagramOffsetField);
    auto length = int_field(machine, datagram, kDatagramLengthField);
    auto position = int_field(machine, datagram, kDatagramPositionField);
    if (!data) return std::unexpected(data.error());
    if (!offset) return std::unexpected(offset.error());
    if (!length) return std::unexpected(length.error());
    if (!position) return std::unexpected(position.error());
    auto capacity = byte_array_length(machine, *data);
    if (!capacity) return std::unexpected(capacity.error());
    if (*offset < 0 || *position < 0 ||
        static_cast<usize>(*offset) > *capacity ||
        static_cast<usize>(*position) >
            *capacity - static_cast<usize>(*offset) ||
        bytes.size() > *capacity - static_cast<usize>(*offset) -
                           static_cast<usize>(*position)) {
        return fail_java("java/io/IOException", "datagram buffer is full");
    }
    for (usize index = 0; index < bytes.size(); ++index) {
        auto stored = set_byte(
            machine, *data,
            static_cast<usize>(*offset + *position) + index,
            bytes[index]);
        if (!stored) return stored;
    }
    const i32 new_position = *position + static_cast<i32>(bytes.size());
    auto advanced = set_int_field(machine, datagram,
                                  kDatagramPositionField, new_position);
    if (!advanced) return advanced;
    if (new_position > *length) {
        return set_int_field(machine, datagram,
                             kDatagramLengthField, new_position);
    }
    return {};
}

void register_datagram_connection(NativeMethodRegistry& registry) {
    add(registry, std::string(kDatagramConnectionClass), "close", "()V",
        close_connection);
    add(registry, std::string(kDatagramConnectionClass), "getLocalAddress",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(arguments,
                                       "UDPDatagramConnection.getLocalAddress");
            if (!connection) return std::unexpected(connection.error());
            auto token = token_from_object(machine, *connection);
            if (!token) return std::unexpected(token.error());
            auto endpoint = java_network_result(
                machine.connections().local_endpoint(*token));
            if (!endpoint) return std::unexpected(endpoint.error());
            auto text = create_string(machine, endpoint->host);
            if (!text) return std::unexpected(text.error());
            return std::optional<Value>(Value::from_reference(*text));
        });
    add(registry, std::string(kDatagramConnectionClass), "getLocalPort", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(arguments,
                                       "UDPDatagramConnection.getLocalPort");
            if (!connection) return std::unexpected(connection.error());
            auto token = token_from_object(machine, *connection);
            if (!token) return std::unexpected(token.error());
            auto endpoint = java_network_result(
                machine.connections().local_endpoint(*token));
            if (!endpoint) return std::unexpected(endpoint.error());
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(endpoint->port)));
        });
    add(registry, std::string(kDatagramConnectionClass), "newDatagram",
        "(I)Ljavax/microedition/io/Datagram;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto size = int_argument(arguments, 1, "datagram size");
            if (!size) return std::unexpected(size.error());
            if (*size < 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "datagram size is negative");
            }
            auto data = allocate_byte_array(machine,
                                             static_cast<usize>(*size));
            if (!data) return std::unexpected(data.error());
            auto datagram = create_datagram(machine, *data, 0, 0);
            if (!datagram) return std::unexpected(datagram.error());
            return std::optional<Value>(Value::from_reference(*datagram));
        });
    add(registry, std::string(kDatagramConnectionClass), "newDatagram",
        "(ILjava/lang/String;)Ljavax/microedition/io/Datagram;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto size = int_argument(arguments, 1, "datagram size");
            auto address = reference_argument(arguments, 2,
                                              "datagram address");
            if (!size) return std::unexpected(size.error());
            if (!address) return std::unexpected(address.error());
            if (*size < 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "datagram size is negative");
            }
            auto data = allocate_byte_array(machine,
                                             static_cast<usize>(*size));
            if (!data) return std::unexpected(data.error());
            auto datagram = create_datagram(machine, *data, 0, 0);
            if (!datagram) return std::unexpected(datagram.error());
            auto stored = set_reference_field(machine, *datagram,
                                              kDatagramAddressField,
                                              *address);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*datagram));
        });
    add(registry, std::string(kDatagramConnectionClass), "newDatagram",
        "([BI)Ljavax/microedition/io/Datagram;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto data = reference_argument(arguments, 1, "datagram data");
            auto size = int_argument(arguments, 2, "datagram size");
            if (!data) return std::unexpected(data.error());
            if (!size) return std::unexpected(size.error());
            auto datagram = create_datagram(machine, *data, 0, *size);
            if (!datagram) return std::unexpected(datagram.error());
            return std::optional<Value>(Value::from_reference(*datagram));
        });
    add(registry, std::string(kDatagramConnectionClass), "newDatagram",
        "([BILjava/lang/String;)Ljavax/microedition/io/Datagram;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto data = reference_argument(arguments, 1, "datagram data");
            auto size = int_argument(arguments, 2, "datagram size");
            auto address = reference_argument(arguments, 3,
                                              "datagram address");
            if (!data) return std::unexpected(data.error());
            if (!size) return std::unexpected(size.error());
            if (!address) return std::unexpected(address.error());
            auto datagram = create_datagram(machine, *data, 0, *size);
            if (!datagram) return std::unexpected(datagram.error());
            auto stored = set_reference_field(machine, *datagram,
                                              kDatagramAddressField,
                                              *address);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*datagram));
        });
    add(registry, std::string(kDatagramConnectionClass), "newDatagram",
        "([BII)Ljavax/microedition/io/Datagram;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto data = reference_argument(arguments, 1, "datagram data");
            auto offset = int_argument(arguments, 2, "datagram offset");
            auto length = int_argument(arguments, 3, "datagram length");
            if (!data) return std::unexpected(data.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            auto datagram = create_datagram(machine, *data, *offset, *length);
            if (!datagram) return std::unexpected(datagram.error());
            return std::optional<Value>(Value::from_reference(*datagram));
        });
    add(registry, std::string(kDatagramConnectionClass), "send",
        "(Ljavax/microedition/io/Datagram;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(arguments, "DatagramConnection.send");
            auto datagram = reference_argument(arguments, 1, "datagram");
            if (!connection) return std::unexpected(connection.error());
            if (!datagram) return std::unexpected(datagram.error());
            auto token = token_from_object(machine, *connection);
            if (!token) return std::unexpected(token.error());
            auto packet = datagram_packet(machine, *datagram);
            if (!packet) return std::unexpected(packet.error());
            auto sent = java_network_status(
                machine.connections().send_datagram(*token,
                                                    std::move(*packet)));
            if (!sent) return std::unexpected(sent.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kDatagramConnectionClass), "receive",
        "(Ljavax/microedition/io/Datagram;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(arguments,
                                       "DatagramConnection.receive");
            auto datagram = reference_argument(arguments, 1, "datagram");
            if (!connection) return std::unexpected(connection.error());
            if (!datagram) return std::unexpected(datagram.error());
            auto token = token_from_object(machine, *connection);
            if (!token) return std::unexpected(token.error());
            auto data = reference_field(machine, *datagram,
                                        kDatagramDataField);
            auto offset = int_field(machine, *datagram,
                                    kDatagramOffsetField);
            if (!data) return std::unexpected(data.error());
            if (!offset) return std::unexpected(offset.error());
            auto capacity = byte_array_length(machine, *data);
            if (!capacity) return std::unexpected(capacity.error());
            if (*offset < 0 || static_cast<usize>(*offset) > *capacity) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "datagram offset is invalid");
            }
            auto packet = java_network_result(
                machine.connections().receive_datagram(
                    *token, *capacity - static_cast<usize>(*offset)));
            if (!packet) return std::unexpected(packet.error());
            for (usize index = 0; index < packet->bytes.size(); ++index) {
                auto stored = set_byte(machine, *data,
                    static_cast<usize>(*offset) + index,
                    packet->bytes[index]);
                if (!stored) return std::unexpected(stored.error());
            }
            auto stored_length = set_int_field(
                machine, *datagram, kDatagramLengthField,
                static_cast<i32>(packet->bytes.size()));
            auto stored_position = set_int_field(
                machine, *datagram, kDatagramPositionField, 0);
            auto address = create_string(machine, datagram_url(packet->peer));
            if (!stored_length)
                return std::unexpected(stored_length.error());
            if (!stored_position)
                return std::unexpected(stored_position.error());
            if (!address) return std::unexpected(address.error());
            auto stored_address = set_reference_field(
                machine, *datagram, kDatagramAddressField, *address);
            if (!stored_address)
                return std::unexpected(stored_address.error());
            return std::optional<Value> {};
        });
    const auto length_getter = [&registry](std::string name) {
        add(registry, std::string(kDatagramConnectionClass),
            std::move(name), "()I",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "datagram length");
                if (!connection) return std::unexpected(connection.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto length = java_network_result(
                    machine.connections().maximum_datagram_length(*token));
                if (!length) return std::unexpected(length.error());
                return std::optional<Value>(Value::from_int(
                    static_cast<i32>(*length)));
            });
    };
    length_getter("getMaximumLength");
    length_getter("getNominalLength");
}

void register_datagram_object(NativeMethodRegistry& registry) {
    add(registry, std::string(kDatagramClass), "getAddress",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.getAddress");
            if (!datagram) return std::unexpected(datagram.error());
            auto address = reference_field(machine, *datagram,
                                           kDatagramAddressField);
            if (!address) return std::unexpected(address.error());
            return std::optional<Value>(Value::from_reference(*address));
        });
    add(registry, std::string(kDatagramClass), "setAddress",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.setAddress");
            auto address = reference_argument(arguments, 1,
                                              "datagram address");
            if (!datagram) return std::unexpected(datagram.error());
            if (!address) return std::unexpected(address.error());
            auto text = utf8_text(machine, *address);
            if (!text) return std::unexpected(text.error());
            auto parsed = network::Url::parse(*text);
            if (!parsed || parsed->scheme != network::Scheme::datagram ||
                parsed->host.empty()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "datagram address is invalid");
            }
            auto stored = set_reference_field(machine, *datagram,
                                              kDatagramAddressField,
                                              *address);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kDatagramClass), "setAddress",
        "(Ljavax/microedition/io/Datagram;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.setAddress");
            auto source = reference_argument(arguments, 1,
                                             "source datagram");
            if (!datagram) return std::unexpected(datagram.error());
            if (!source) return std::unexpected(source.error());
            auto address = reference_field(machine, *source,
                                           kDatagramAddressField);
            if (!address) return std::unexpected(address.error());
            auto stored = set_reference_field(machine, *datagram,
                                              kDatagramAddressField,
                                              *address);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kDatagramClass), "getData", "()[B",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.getData");
            if (!datagram) return std::unexpected(datagram.error());
            auto data = reference_field(machine, *datagram,
                                        kDatagramDataField);
            if (!data) return std::unexpected(data.error());
            return std::optional<Value>(Value::from_reference(*data));
        });
    const auto int_getter = [&registry](std::string method, usize field) {
        add(registry, std::string(kDatagramClass), std::move(method), "()I",
            [field](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto datagram = receiver(arguments, "Datagram getter");
                if (!datagram) return std::unexpected(datagram.error());
                auto value = int_field(machine, *datagram, field);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_int(*value));
            });
    };
    int_getter("getOffset", kDatagramOffsetField);
    int_getter("getLength", kDatagramLengthField);
    add(registry, std::string(kDatagramClass), "setData", "([BII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.setData");
            auto data = reference_argument(arguments, 1, "datagram data");
            auto offset = int_argument(arguments, 2, "datagram offset");
            auto length = int_argument(arguments, 3, "datagram length");
            if (!datagram) return std::unexpected(datagram.error());
            if (!data) return std::unexpected(data.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            auto size = byte_array_length(machine, *data);
            if (!size) return std::unexpected(size.error());
            auto range = validate_range(*size, *offset, *length);
            if (!range) return std::unexpected(range.error());
            auto stored_data = set_reference_field(machine, *datagram,
                                                   kDatagramDataField,
                                                   *data);
            auto stored_offset = set_int_field(machine, *datagram,
                                               kDatagramOffsetField,
                                               *offset);
            auto stored_length = set_int_field(machine, *datagram,
                                               kDatagramLengthField,
                                               *length);
            auto stored_position = set_int_field(machine, *datagram,
                                                 kDatagramPositionField, 0);
            if (!stored_data) return std::unexpected(stored_data.error());
            if (!stored_offset) return std::unexpected(stored_offset.error());
            if (!stored_length) return std::unexpected(stored_length.error());
            if (!stored_position)
                return std::unexpected(stored_position.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kDatagramClass), "setLength", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.setLength");
            auto length = int_argument(arguments, 1, "datagram length");
            if (!datagram) return std::unexpected(datagram.error());
            if (!length) return std::unexpected(length.error());
            auto data = reference_field(machine, *datagram,
                                        kDatagramDataField);
            auto offset = int_field(machine, *datagram,
                                    kDatagramOffsetField);
            if (!data) return std::unexpected(data.error());
            if (!offset) return std::unexpected(offset.error());
            auto size = byte_array_length(machine, *data);
            if (!size) return std::unexpected(size.error());
            if (*length < 0 || *offset < 0 ||
                static_cast<usize>(*offset) > *size ||
                static_cast<usize>(*length) >
                    *size - static_cast<usize>(*offset)) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "datagram length is invalid");
            }
            auto stored = set_int_field(machine, *datagram,
                                        kDatagramLengthField, *length);
            if (!stored) return std::unexpected(stored.error());
            auto position = int_field(machine, *datagram,
                                      kDatagramPositionField);
            if (position && *position > *length) {
                (void)set_int_field(machine, *datagram,
                                    kDatagramPositionField, *length);
            }
            return std::optional<Value> {};
        });
    add(registry, std::string(kDatagramClass), "reset", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.reset");
            if (!datagram) return std::unexpected(datagram.error());
            auto reset = set_int_field(machine, *datagram,
                                       kDatagramPositionField, 0);
            if (!reset) return std::unexpected(reset.error());
            return std::optional<Value> {};
        });

    add(registry, std::string(kDatagramClass), "readUnsignedByte", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.readUnsignedByte");
            if (!datagram) return std::unexpected(datagram.error());
            auto bytes = datagram_read_bytes(machine, *datagram, 1U);
            if (!bytes) return std::unexpected(bytes.error());
            return std::optional<Value>(Value::from_int((*bytes)[0]));
        });
    add(registry, std::string(kDatagramClass), "readByte", "()B",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.readByte");
            if (!datagram) return std::unexpected(datagram.error());
            auto bytes = datagram_read_bytes(machine, *datagram, 1U);
            if (!bytes) return std::unexpected(bytes.error());
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(static_cast<i8>((*bytes)[0]))));
        });
    add(registry, std::string(kDatagramClass), "readBoolean", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.readBoolean");
            if (!datagram) return std::unexpected(datagram.error());
            auto bytes = datagram_read_bytes(machine, *datagram, 1U);
            if (!bytes) return std::unexpected(bytes.error());
            return std::optional<Value>(Value::from_int(
                (*bytes)[0] == 0U ? 0 : 1));
        });
    const auto read_u16 = [&registry](std::string method,
                                      std::string descriptor,
                                      bool signed_value) {
        add(registry, std::string(kDatagramClass), std::move(method),
            std::move(descriptor),
            [signed_value](Machine& machine,
                           std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto datagram = receiver(arguments, "Datagram read16");
                if (!datagram) return std::unexpected(datagram.error());
                auto bytes = datagram_read_bytes(machine, *datagram, 2U);
                if (!bytes) return std::unexpected(bytes.error());
                const u16 bits = static_cast<u16>(
                    (static_cast<u16>((*bytes)[0]) << 8U) | (*bytes)[1]);
                const i32 value = signed_value
                    ? static_cast<i32>(static_cast<i16>(bits))
                    : static_cast<i32>(bits);
                return std::optional<Value>(Value::from_int(value));
            });
    };
    read_u16("readShort", "()S", true);
    read_u16("readUnsignedShort", "()I", false);
    read_u16("readChar", "()C", false);
    add(registry, std::string(kDatagramClass), "readInt", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.readInt");
            if (!datagram) return std::unexpected(datagram.error());
            auto bytes = datagram_read_bytes(machine, *datagram, 4U);
            if (!bytes) return std::unexpected(bytes.error());
            const u32 bits = (static_cast<u32>((*bytes)[0]) << 24U) |
                             (static_cast<u32>((*bytes)[1]) << 16U) |
                             (static_cast<u32>((*bytes)[2]) << 8U) |
                             static_cast<u32>((*bytes)[3]);
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(bits)));
        });
    add(registry, std::string(kDatagramClass), "readLong", "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.readLong");
            if (!datagram) return std::unexpected(datagram.error());
            auto bytes = datagram_read_bytes(machine, *datagram, 8U);
            if (!bytes) return std::unexpected(bytes.error());
            u64 bits = 0;
            for (const u8 byte : *bytes) bits = (bits << 8U) | byte;
            return std::optional<Value>(Value::from_long(
                static_cast<i64>(bits)));
        });
    add(registry, std::string(kDatagramClass), "readFloat", "()F",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.readFloat");
            if (!datagram) return std::unexpected(datagram.error());
            auto bytes = datagram_read_bytes(machine, *datagram, 4U);
            if (!bytes) return std::unexpected(bytes.error());
            const u32 bits = (static_cast<u32>((*bytes)[0]) << 24U) |
                             (static_cast<u32>((*bytes)[1]) << 16U) |
                             (static_cast<u32>((*bytes)[2]) << 8U) |
                             static_cast<u32>((*bytes)[3]);
            return std::optional<Value>(Value::from_float(
                std::bit_cast<float>(bits)));
        });
    add(registry, std::string(kDatagramClass), "readDouble", "()D",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.readDouble");
            if (!datagram) return std::unexpected(datagram.error());
            auto bytes = datagram_read_bytes(machine, *datagram, 8U);
            if (!bytes) return std::unexpected(bytes.error());
            u64 bits = 0;
            for (const u8 byte : *bytes) bits = (bits << 8U) | byte;
            return std::optional<Value>(Value::from_double(
                std::bit_cast<double>(bits)));
        });

    const auto read_fully = [&registry](bool with_range) {
        add(registry, std::string(kDatagramClass), "readFully",
            with_range ? "([BII)V" : "([B)V",
            [with_range](Machine& machine,
                         std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto datagram = receiver(arguments, "Datagram.readFully");
                auto destination = reference_argument(arguments, 1,
                                                      "read destination");
                if (!datagram) return std::unexpected(datagram.error());
                if (!destination) return std::unexpected(destination.error());
                auto size = byte_array_length(machine, *destination);
                if (!size) return std::unexpected(size.error());
                if (*size > static_cast<usize>(
                                std::numeric_limits<i32>::max())) {
                    return fail_java("java/lang/OutOfMemoryError",
                                     "byte[] exceeds Java int length");
                }
                i32 offset = 0;
                i32 length = static_cast<i32>(*size);
                if (with_range) {
                    auto parsed_offset = int_argument(arguments, 2, "offset");
                    auto parsed_length = int_argument(arguments, 3, "length");
                    if (!parsed_offset)
                        return std::unexpected(parsed_offset.error());
                    if (!parsed_length)
                        return std::unexpected(parsed_length.error());
                    offset = *parsed_offset;
                    length = *parsed_length;
                }
                auto range = validate_range(*size, offset, length);
                if (!range) return std::unexpected(range.error());
                auto bytes = datagram_read_bytes(
                    machine, *datagram, static_cast<usize>(length));
                if (!bytes) return std::unexpected(bytes.error());
                for (usize index = 0; index < bytes->size(); ++index) {
                    auto stored = set_byte(
                        machine, *destination,
                        static_cast<usize>(offset) + index,
                        (*bytes)[index]);
                    if (!stored) return std::unexpected(stored.error());
                }
                return std::optional<Value> {};
            });
    };
    read_fully(false);
    read_fully(true);
    add(registry, std::string(kDatagramClass), "skipBytes", "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.skipBytes");
            auto requested = int_argument(arguments, 1, "skip count");
            if (!datagram) return std::unexpected(datagram.error());
            if (!requested) return std::unexpected(requested.error());
            auto position = int_field(machine, *datagram,
                                      kDatagramPositionField);
            auto length = int_field(machine, *datagram,
                                    kDatagramLengthField);
            if (!position) return std::unexpected(position.error());
            if (!length) return std::unexpected(length.error());
            const i32 skipped = std::max(
                0, std::min(*requested, *length - *position));
            auto advanced = set_int_field(machine, *datagram,
                                          kDatagramPositionField,
                                          *position + skipped);
            if (!advanced) return std::unexpected(advanced.error());
            return std::optional<Value>(Value::from_int(skipped));
        });
    add(registry, std::string(kDatagramClass), "readLine",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.readLine");
            if (!datagram) return std::unexpected(datagram.error());
            auto position = int_field(machine, *datagram,
                                      kDatagramPositionField);
            auto length = int_field(machine, *datagram,
                                    kDatagramLengthField);
            if (!position) return std::unexpected(position.error());
            if (!length) return std::unexpected(length.error());
            if (*position >= *length) {
                return std::optional<Value>(
                    Value::from_reference(ObjectRef {}));
            }
            std::u16string line;
            while (true) {
                auto bytes = datagram_read_bytes(machine, *datagram, 1U);
                if (!bytes) return std::unexpected(bytes.error());
                const u8 byte = (*bytes)[0];
                if (byte == static_cast<u8>('\n')) break;
                if (byte == static_cast<u8>('\r')) {
                    auto next_position = int_field(
                        machine, *datagram, kDatagramPositionField);
                    if (!next_position)
                        return std::unexpected(next_position.error());
                    if (*next_position < *length) {
                        auto data = reference_field(
                            machine, *datagram, kDatagramDataField);
                        auto offset = int_field(
                            machine, *datagram, kDatagramOffsetField);
                        if (!data) return std::unexpected(data.error());
                        if (!offset) return std::unexpected(offset.error());
                        auto next = byte_at(
                            machine, *data,
                            static_cast<usize>(*offset + *next_position));
                        if (!next) return std::unexpected(next.error());
                        if (*next == static_cast<u8>('\n')) {
                            auto consumed = set_int_field(
                                machine, *datagram,
                                kDatagramPositionField,
                                *next_position + 1);
                            if (!consumed)
                                return std::unexpected(consumed.error());
                        }
                    }
                    break;
                }
                line.push_back(static_cast<char16_t>(byte));
                auto current = int_field(machine, *datagram,
                                         kDatagramPositionField);
                if (!current) return std::unexpected(current.error());
                if (*current >= *length) break;
            }
            auto string = create_string(machine, std::move(line));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });
    add(registry, std::string(kDatagramClass), "readUTF",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.readUTF");
            if (!datagram) return std::unexpected(datagram.error());
            auto prefix = datagram_read_bytes(machine, *datagram, 2U);
            if (!prefix) return std::unexpected(prefix.error());
            const usize length =
                (static_cast<usize>((*prefix)[0]) << 8U) | (*prefix)[1];
            auto bytes = datagram_read_bytes(machine, *datagram, length);
            if (!bytes) return std::unexpected(bytes.error());
            const std::string encoded(bytes->begin(), bytes->end());
            auto decoded = decode_modified_utf8(
                encoded, ModifiedUtf8Mode::allow_raw_nul);
            if (!decoded) {
                return fail_java("java/io/UTFDataFormatException",
                                 decoded.error().message);
            }
            auto string = machine.class_states().allocate_instance(
                machine.heap(), "java/lang/String");
            if (!string) return std::unexpected(string.error());
            auto attached = machine.heap().attach_string(*string,
                                                         std::move(*decoded));
            if (!attached) return std::unexpected(attached.error());
            return std::optional<Value>(Value::from_reference(*string));
        });

    add(registry, std::string(kDatagramClass), "write", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.write");
            auto value = int_argument(arguments, 1, "byte");
            if (!datagram) return std::unexpected(datagram.error());
            if (!value) return std::unexpected(value.error());
            const std::array<u8, 1> bytes {
                static_cast<u8>(*value & 0xFF),
            };
            auto written = datagram_write_bytes(machine, *datagram, bytes);
            if (!written) return std::unexpected(written.error());
            return std::optional<Value> {};
        });
    const auto write_array = [&registry](bool with_range) {
        add(registry, std::string(kDatagramClass), "write",
            with_range ? "([BII)V" : "([B)V",
            [with_range](Machine& machine,
                         std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto datagram = receiver(arguments, "Datagram.write");
                auto source = reference_argument(arguments, 1, "write source");
                if (!datagram) return std::unexpected(datagram.error());
                if (!source) return std::unexpected(source.error());
                auto size = byte_array_length(machine, *source);
                if (!size) return std::unexpected(size.error());
                if (*size > static_cast<usize>(
                                std::numeric_limits<i32>::max())) {
                    return fail_java("java/lang/OutOfMemoryError",
                                     "byte[] exceeds Java int length");
                }
                i32 offset = 0;
                i32 length = static_cast<i32>(*size);
                if (with_range) {
                    auto parsed_offset = int_argument(arguments, 2, "offset");
                    auto parsed_length = int_argument(arguments, 3, "length");
                    if (!parsed_offset)
                        return std::unexpected(parsed_offset.error());
                    if (!parsed_length)
                        return std::unexpected(parsed_length.error());
                    offset = *parsed_offset;
                    length = *parsed_length;
                }
                auto bytes = byte_slice(machine, *source, offset, length);
                if (!bytes) return std::unexpected(bytes.error());
                auto written = datagram_write_bytes(machine, *datagram, *bytes);
                if (!written) return std::unexpected(written.error());
                return std::optional<Value> {};
            });
    };
    write_array(false);
    write_array(true);
    const auto write_int = [&registry](std::string method,
                                       std::string descriptor,
                                       usize byte_count) {
        add(registry, std::string(kDatagramClass), std::move(method),
            std::move(descriptor),
            [byte_count](Machine& machine,
                         std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto datagram = receiver(arguments, "Datagram write integer");
                auto value = int_argument(arguments, 1, "integer");
                if (!datagram) return std::unexpected(datagram.error());
                if (!value) return std::unexpected(value.error());
                const u32 bits = static_cast<u32>(*value);
                std::array<u8, 4> encoded {};
                for (usize index = 0; index < byte_count; ++index) {
                    encoded[index] = static_cast<u8>(
                        bits >> ((byte_count - index - 1U) * 8U));
                }
                auto written = datagram_write_bytes(
                    machine, *datagram,
                    std::span<const u8>(encoded.data(), byte_count));
                if (!written) return std::unexpected(written.error());
                return std::optional<Value> {};
            });
    };
    write_int("writeByte", "(I)V", 1U);
    write_int("writeShort", "(I)V", 2U);
    write_int("writeChar", "(I)V", 2U);
    write_int("writeInt", "(I)V", 4U);
    add(registry, std::string(kDatagramClass), "writeBoolean", "(Z)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.writeBoolean");
            auto value = int_argument(arguments, 1, "boolean");
            if (!datagram) return std::unexpected(datagram.error());
            if (!value) return std::unexpected(value.error());
            const std::array<u8, 1> encoded {
                static_cast<u8>(*value == 0 ? 0 : 1),
            };
            auto written = datagram_write_bytes(machine, *datagram, encoded);
            if (!written) return std::unexpected(written.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kDatagramClass), "writeLong", "(J)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.writeLong");
            auto value = arguments[1].as_long();
            if (!datagram) return std::unexpected(datagram.error());
            if (!value) return std::unexpected(value.error());
            const u64 bits = static_cast<u64>(*value);
            std::array<u8, 8> encoded {};
            for (usize index = 0; index < encoded.size(); ++index) {
                encoded[index] = static_cast<u8>(
                    bits >> ((encoded.size() - index - 1U) * 8U));
            }
            auto written = datagram_write_bytes(machine, *datagram, encoded);
            if (!written) return std::unexpected(written.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kDatagramClass), "writeFloat", "(F)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.writeFloat");
            auto value = arguments[1].as_float();
            if (!datagram) return std::unexpected(datagram.error());
            if (!value) return std::unexpected(value.error());
            const u32 bits = std::bit_cast<u32>(*value);
            const std::array<u8, 4> encoded {
                static_cast<u8>(bits >> 24U),
                static_cast<u8>(bits >> 16U),
                static_cast<u8>(bits >> 8U),
                static_cast<u8>(bits),
            };
            auto written = datagram_write_bytes(machine, *datagram, encoded);
            if (!written) return std::unexpected(written.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kDatagramClass), "writeDouble", "(D)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.writeDouble");
            auto value = arguments[1].as_double();
            if (!datagram) return std::unexpected(datagram.error());
            if (!value) return std::unexpected(value.error());
            const u64 bits = std::bit_cast<u64>(*value);
            std::array<u8, 8> encoded {};
            for (usize index = 0; index < encoded.size(); ++index) {
                encoded[index] = static_cast<u8>(
                    bits >> ((encoded.size() - index - 1U) * 8U));
            }
            auto written = datagram_write_bytes(machine, *datagram, encoded);
            if (!written) return std::unexpected(written.error());
            return std::optional<Value> {};
        });

    const auto write_string = [&registry](std::string method, bool characters) {
        add(registry, std::string(kDatagramClass), std::move(method),
            "(Ljava/lang/String;)V",
            [characters](Machine& machine,
                         std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto datagram = receiver(arguments, "Datagram write string");
                auto string = reference_argument(arguments, 1, "string");
                if (!datagram) return std::unexpected(datagram.error());
                if (!string) return std::unexpected(string.error());
                auto text = machine.heap().string_value(*string);
                if (!text) return std::unexpected(text.error());
                std::vector<u8> encoded;
                encoded.reserve(text->size() * (characters ? 2U : 1U));
                for (const char16_t character : *text) {
                    if (characters) {
                        encoded.push_back(static_cast<u8>(
                            static_cast<u16>(character) >> 8U));
                    }
                    encoded.push_back(static_cast<u8>(character));
                }
                auto written = datagram_write_bytes(machine, *datagram,
                                                    encoded);
                if (!written) return std::unexpected(written.error());
                return std::optional<Value> {};
            });
    };
    write_string("writeBytes", false);
    write_string("writeChars", true);
    add(registry, std::string(kDatagramClass), "writeUTF",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto datagram = receiver(arguments, "Datagram.writeUTF");
            auto string = reference_argument(arguments, 1, "UTF string");
            if (!datagram) return std::unexpected(datagram.error());
            if (!string) return std::unexpected(string.error());
            auto text = machine.heap().string_value(*string);
            if (!text) return std::unexpected(text.error());
            auto encoded = encode_modified_utf8(*text);
            if (!encoded) return std::unexpected(encoded.error());
            if (encoded->size() > std::numeric_limits<u16>::max()) {
                return fail_java("java/io/UTFDataFormatException",
                                 "encoded UTF string is too long");
            }
            const u16 length = static_cast<u16>(encoded->size());
            std::vector<u8> bytes {
                static_cast<u8>(length >> 8U),
                static_cast<u8>(length),
            };
            bytes.insert(bytes.end(), encoded->begin(), encoded->end());
            auto written = datagram_write_bytes(machine, *datagram, bytes);
            if (!written) return std::unexpected(written.error());
            return std::optional<Value> {};
        });
}

void register_http_connection(NativeMethodRegistry& registry) {
    const std::array<std::string_view, 2> owners {kHttpClass, kHttpsClass};
    for (const auto owner : owners) {
        add(registry, std::string(owner), "setRequestMethod",
            "(Ljava/lang/String;)V",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "setRequestMethod");
                auto method = reference_argument(arguments, 1,
                                                 "request method");
                if (!connection) return std::unexpected(connection.error());
                if (!method) return std::unexpected(method.error());
                auto text = utf8_text(machine, *method);
                if (!text) return std::unexpected(text.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto changed = java_network_status(
                    machine.connections().set_http_method(*token,
                                                          std::move(*text)));
                if (!changed) return std::unexpected(changed.error());
                return std::optional<Value> {};
            });
        add(registry, std::string(owner), "getRequestMethod",
            "()Ljava/lang/String;",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "getRequestMethod");
                if (!connection) return std::unexpected(connection.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto method = java_network_result(
                    machine.connections().http_method(*token));
                if (!method) return std::unexpected(method.error());
                auto text = create_string(machine, *method);
                if (!text) return std::unexpected(text.error());
                return std::optional<Value>(Value::from_reference(*text));
            });
        add(registry, std::string(owner), "setRequestProperty",
            "(Ljava/lang/String;Ljava/lang/String;)V",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "setRequestProperty");
                auto name = reference_argument(arguments, 1, "header name");
                auto value = reference_argument(arguments, 2, "header value");
                if (!connection) return std::unexpected(connection.error());
                if (!name) return std::unexpected(name.error());
                if (!value) return std::unexpected(value.error());
                auto name_text = utf8_text(machine, *name);
                auto value_text = utf8_text(machine, *value);
                if (!name_text) return std::unexpected(name_text.error());
                if (!value_text) return std::unexpected(value_text.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto changed = java_network_status(
                    machine.connections().set_http_header(
                        *token, std::move(*name_text),
                        std::move(*value_text)));
                if (!changed) return std::unexpected(changed.error());
                return std::optional<Value> {};
            });
        add(registry, std::string(owner), "getRequestProperty",
            "(Ljava/lang/String;)Ljava/lang/String;",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "getRequestProperty");
                auto name = reference_argument(arguments, 1, "header name");
                if (!connection) return std::unexpected(connection.error());
                if (!name) return std::unexpected(name.error());
                auto name_text = utf8_text(machine, *name);
                if (!name_text) return std::unexpected(name_text.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto value = java_network_result(
                    machine.connections().http_request_header(*token,
                                                              *name_text));
                if (!value) return std::unexpected(value.error());
                return optional_string(machine, *value);
            });
        add(registry, std::string(owner), "getResponseCode", "()I",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "getResponseCode");
                if (!connection) return std::unexpected(connection.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto code = java_network_result(
                    machine.connections().http_response_code(*token));
                if (!code) return std::unexpected(code.error());
                return std::optional<Value>(Value::from_int(*code));
            });
        add(registry, std::string(owner), "getResponseMessage",
            "()Ljava/lang/String;",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "getResponseMessage");
                if (!connection) return std::unexpected(connection.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto message = java_network_result(
                    machine.connections().http_response_message(*token));
                if (!message) return std::unexpected(message.error());
                auto text = create_string(machine, *message);
                if (!text) return std::unexpected(text.error());
                return std::optional<Value>(Value::from_reference(*text));
            });
        add(registry, std::string(owner), "getHeaderField",
            "(Ljava/lang/String;)Ljava/lang/String;",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "getHeaderField");
                auto name = reference_argument(arguments, 1, "header name");
                if (!connection) return std::unexpected(connection.error());
                if (!name) return std::unexpected(name.error());
                auto name_text = utf8_text(machine, *name);
                if (!name_text) return std::unexpected(name_text.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto value = java_network_result(
                    machine.connections().http_response_header(*token,
                                                               *name_text));
                if (!value) return std::unexpected(value.error());
                return optional_string(machine, *value);
            });
        const auto indexed_header = [&registry, owner](std::string method,
                                                       bool key) {
            add(registry, std::string(owner), std::move(method),
                "(I)Ljava/lang/String;",
                [key](Machine& machine, std::span<const Value> arguments)
                    -> Result<std::optional<Value>> {
                    auto connection = receiver(arguments, "indexed header");
                    auto index = int_argument(arguments, 1, "header index");
                    if (!connection)
                        return std::unexpected(connection.error());
                    if (!index) return std::unexpected(index.error());
                    if (*index < 0) {
                        return std::optional<Value>(
                            Value::from_reference(ObjectRef {}));
                    }
                    auto token = token_from_object(machine, *connection);
                    if (!token) return std::unexpected(token.error());
                    if (*index == 0) {
                        if (key) {
                            return std::optional<Value>(
                                Value::from_reference(ObjectRef {}));
                        }
                        auto code = java_network_result(
                            machine.connections().http_response_code(*token));
                        auto message = java_network_result(
                            machine.connections().http_response_message(*token));
                        if (!code) return std::unexpected(code.error());
                        if (!message) return std::unexpected(message.error());
                        auto status_line = create_string(
                            machine, "HTTP/1.1 " + std::to_string(*code) +
                                         " " + *message);
                        if (!status_line)
                            return std::unexpected(status_line.error());
                        return std::optional<Value>(
                            Value::from_reference(*status_line));
                    }
                    auto header = java_network_result(
                        machine.connections().http_response_header(
                            *token, static_cast<usize>(*index - 1)));
                    if (!header) return std::unexpected(header.error());
                    if (!header->has_value()) {
                        return std::optional<Value>(
                            Value::from_reference(ObjectRef {}));
                    }
                    auto text = create_string(
                        machine, key ? (**header).first : (**header).second);
                    if (!text) return std::unexpected(text.error());
                    return std::optional<Value>(Value::from_reference(*text));
                });
        };
        indexed_header("getHeaderField", false);
        indexed_header("getHeaderFieldKey", true);
        add(registry, std::string(owner), "getHeaderFieldInt",
            "(Ljava/lang/String;I)I",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "getHeaderFieldInt");
                auto name = reference_argument(arguments, 1, "header name");
                auto fallback = int_argument(arguments, 2, "fallback");
                if (!connection) return std::unexpected(connection.error());
                if (!name) return std::unexpected(name.error());
                if (!fallback) return std::unexpected(fallback.error());
                auto name_text = utf8_text(machine, *name);
                if (!name_text) return std::unexpected(name_text.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto value = java_network_result(
                    machine.connections().http_response_header(*token,
                                                               *name_text));
                if (!value) return std::unexpected(value.error());
                if (!value->has_value()) {
                    return std::optional<Value>(Value::from_int(*fallback));
                }
                i32 parsed = *fallback;
                const auto converted = std::from_chars(
                    (**value).data(), (**value).data() + (**value).size(),
                    parsed);
                if (converted.ec != std::errc {} ||
                    converted.ptr != (**value).data() + (**value).size()) {
                    parsed = *fallback;
                }
                return std::optional<Value>(Value::from_int(parsed));
            });
        add(registry, std::string(owner), "getHeaderFieldDate",
            "(Ljava/lang/String;J)J",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "getHeaderFieldDate");
                auto name = reference_argument(arguments, 1, "header name");
                auto fallback = arguments[2].as_long();
                if (!connection) return std::unexpected(connection.error());
                if (!name) return std::unexpected(name.error());
                if (!fallback) return std::unexpected(fallback.error());
                auto name_text = utf8_text(machine, *name);
                if (!name_text) return std::unexpected(name_text.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto value = java_network_result(
                    machine.connections().http_response_header(*token,
                                                               *name_text));
                if (!value) return std::unexpected(value.error());
                if (!value->has_value()) {
                    return std::optional<Value>(Value::from_long(*fallback));
                }
                const auto parsed = parse_http_date(**value);
                return std::optional<Value>(Value::from_long(
                    parsed.value_or(*fallback)));
            });

        const auto url_string = [&registry, owner](std::string method,
                                                   auto selector) {
            add(registry, std::string(owner), std::move(method),
                "()Ljava/lang/String;",
                [selector](Machine& machine,
                           std::span<const Value> arguments)
                    -> Result<std::optional<Value>> {
                    auto connection = receiver(arguments, "HTTP URL getter");
                    if (!connection)
                        return std::unexpected(connection.error());
                    auto token = token_from_object(machine, *connection);
                    if (!token) return std::unexpected(token.error());
                    auto url = java_network_result(
                        machine.connections().url(*token));
                    if (!url) return std::unexpected(url.error());
                    auto text = create_string(machine, selector(*url));
                    if (!text) return std::unexpected(text.error());
                    return std::optional<Value>(Value::from_reference(*text));
                });
        };
        url_string("getURL", [](const network::Url& url) {
            return url.to_string();
        });
        url_string("getProtocol", [](const network::Url& url) {
            return url.scheme_name;
        });
        url_string("getHost", [](const network::Url& url) {
            return url.host;
        });
        url_string("getFile", [](const network::Url& url) {
            return url.request_target();
        });
        url_string("getRef", [](const network::Url& url) {
            return url.fragment;
        });
        url_string("getQuery", [](const network::Url& url) {
            return url.query;
        });
        add(registry, std::string(owner), "getPort", "()I",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "getPort");
                if (!connection) return std::unexpected(connection.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto url = java_network_result(
                    machine.connections().url(*token));
                if (!url) return std::unexpected(url.error());
                return std::optional<Value>(Value::from_int(
                    url->port.has_value()
                        ? static_cast<i32>(*url->port)
                        : -1));
            });
        add(registry, std::string(owner), "getLength", "()J",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto connection = receiver(arguments, "getLength");
                if (!connection) return std::unexpected(connection.error());
                auto token = token_from_object(machine, *connection);
                if (!token) return std::unexpected(token.error());
                auto length = java_network_result(
                    machine.connections().http_content_length(*token));
                if (!length) return std::unexpected(length.error());
                return std::optional<Value>(Value::from_long(*length));
            });
        const auto content_header = [&registry, owner](std::string method,
                                                       std::string header) {
            add(registry, std::string(owner), std::move(method),
                "()Ljava/lang/String;",
                [header = std::move(header)](
                    Machine& machine,
                    std::span<const Value> arguments)
                    -> Result<std::optional<Value>> {
                    auto connection = receiver(arguments, "content header");
                    if (!connection)
                        return std::unexpected(connection.error());
                    auto token = token_from_object(machine, *connection);
                    if (!token) return std::unexpected(token.error());
                    auto value = java_network_result(
                        machine.connections().http_response_header(
                            *token, header));
                    if (!value) return std::unexpected(value.error());
                    return optional_string(machine, *value);
                });
        };
        content_header("getType", "Content-Type");
        content_header("getEncoding", "Content-Encoding");
        const auto date_header = [&registry, owner](std::string method,
                                                    std::string header) {
            add(registry, std::string(owner), std::move(method), "()J",
                [header = std::move(header)](
                    Machine& machine,
                    std::span<const Value> arguments)
                    -> Result<std::optional<Value>> {
                    auto connection = receiver(arguments, "HTTP date getter");
                    if (!connection)
                        return std::unexpected(connection.error());
                    auto token = token_from_object(machine, *connection);
                    if (!token) return std::unexpected(token.error());
                    auto value = java_network_result(
                        machine.connections().http_response_header(
                            *token, header));
                    if (!value) return std::unexpected(value.error());
                    if (!value->has_value()) {
                        return std::optional<Value>(Value::from_long(0));
                    }
                    return std::optional<Value>(Value::from_long(
                        parse_http_date(**value).value_or(0)));
                });
        };
        date_header("getExpiration", "Expires");
        date_header("getDate", "Date");
        date_header("getLastModified", "Last-Modified");
    }

    add(registry, std::string(kHttpsClass), "getSecurityInfo",
        "()Ljavax/microedition/io/SecurityInfo;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(arguments, "getSecurityInfo");
            if (!connection) return std::unexpected(connection.error());
            auto token = token_from_object(machine, *connection);
            if (!token) return std::unexpected(token.error());
            auto metadata = java_network_result(
                machine.connections().http_security_info(*token));
            if (!metadata) return std::unexpected(metadata.error());
            if (!metadata->has_value()) {
                return fail_java("java/io/IOException",
                                 "HTTPS security metadata is unavailable");
            }
            auto security = create_security_info(machine, **metadata);
            if (!security) return std::unexpected(security.error());
            return std::optional<Value>(Value::from_reference(*security));
        });
}

void register_connection_constants(NativeMethodRegistry& registry) {
    add(registry, "javax/microedition/io/Connector",
        "<clinit>", "()V",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            static constexpr std::array<std::pair<std::string_view, i32>, 3>
                constants {{
                    {"READ", 1},
                    {"WRITE", 2},
                    {"READ_WRITE", 3},
                }};
            for (const auto& [name, value] : constants) {
                auto stored = set_static_int(
                    machine, "javax/microedition/io/Connector",
                    name, value);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });

    add(registry, "javax/microedition/io/SocketConnection",
        "<clinit>", "()V",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            static constexpr std::array<std::pair<std::string_view, i32>, 5>
                constants {{
                    {"DELAY", 0},
                    {"LINGER", 1},
                    {"KEEPALIVE", 2},
                    {"RCVBUF", 3},
                    {"SNDBUF", 4},
                }};
            for (const auto& [name, value] : constants) {
                auto stored = set_static_byte(
                    machine, "javax/microedition/io/SocketConnection",
                    name, value);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });

    add(registry, std::string(kCertificateExceptionClass),
        "<clinit>", "()V",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            static constexpr std::array<std::pair<std::string_view, i32>, 14>
                constants {{
                    {"BAD_EXTENSIONS", 1},
                    {"CERTIFICATE_CHAIN_TOO_LONG", 2},
                    {"EXPIRED", 3},
                    {"UNAUTHORIZED_INTERMEDIATE_CA", 4},
                    {"MISSING_SIGNATURE", 5},
                    {"NOT_YET_VALID", 6},
                    {"SITENAME_MISMATCH", 7},
                    {"UNRECOGNIZED_ISSUER", 8},
                    {"UNSUPPORTED_SIGALG", 9},
                    {"INAPPROPRIATE_KEY_USAGE", 10},
                    {"BROKEN_CHAIN", 11},
                    {"ROOT_CA_EXPIRED", 12},
                    {"UNSUPPORTED_PUBLIC_KEY_TYPE", 13},
                    {"VERIFICATION_FAILED", 14},
                }};
            for (const auto& [name, value] : constants) {
                auto stored = set_static_byte(
                    machine, kCertificateExceptionClass, name, value);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });

    add(registry, "javax/microedition/io/HttpConnection",
        "<clinit>", "()V",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            static constexpr std::array<std::pair<std::string_view,
                                                  std::string_view>, 3>
                methods {{
                    {"HEAD", "HEAD"},
                    {"GET", "GET"},
                    {"POST", "POST"},
                }};
            for (const auto& [name, value] : methods) {
                auto stored = set_static_string(
                    machine, "javax/microedition/io/HttpConnection",
                    name, value);
                if (!stored) return std::unexpected(stored.error());
            }
            static constexpr std::array<std::pair<std::string_view, i32>, 38>
                statuses {{
                    {"HTTP_OK", 200},
                    {"HTTP_CREATED", 201},
                    {"HTTP_ACCEPTED", 202},
                    {"HTTP_NOT_AUTHORITATIVE", 203},
                    {"HTTP_NO_CONTENT", 204},
                    {"HTTP_RESET", 205},
                    {"HTTP_PARTIAL", 206},
                    {"HTTP_MULT_CHOICE", 300},
                    {"HTTP_MOVED_PERM", 301},
                    {"HTTP_MOVED_TEMP", 302},
                    {"HTTP_SEE_OTHER", 303},
                    {"HTTP_NOT_MODIFIED", 304},
                    {"HTTP_USE_PROXY", 305},
                    {"HTTP_TEMP_REDIRECT", 307},
                    {"HTTP_BAD_REQUEST", 400},
                    {"HTTP_UNAUTHORIZED", 401},
                    {"HTTP_PAYMENT_REQUIRED", 402},
                    {"HTTP_FORBIDDEN", 403},
                    {"HTTP_NOT_FOUND", 404},
                    {"HTTP_BAD_METHOD", 405},
                    {"HTTP_NOT_ACCEPTABLE", 406},
                    {"HTTP_PROXY_AUTH", 407},
                    {"HTTP_CLIENT_TIMEOUT", 408},
                    {"HTTP_CONFLICT", 409},
                    {"HTTP_GONE", 410},
                    {"HTTP_LENGTH_REQUIRED", 411},
                    {"HTTP_PRECON_FAILED", 412},
                    {"HTTP_ENTITY_TOO_LARGE", 413},
                    {"HTTP_REQ_TOO_LONG", 414},
                    {"HTTP_UNSUPPORTED_TYPE", 415},
                    {"HTTP_UNSUPPORTED_RANGE", 416},
                    {"HTTP_EXPECT_FAILED", 417},
                    {"HTTP_INTERNAL_ERROR", 500},
                    {"HTTP_NOT_IMPLEMENTED", 501},
                    {"HTTP_BAD_GATEWAY", 502},
                    {"HTTP_UNAVAILABLE", 503},
                    {"HTTP_GATEWAY_TIMEOUT", 504},
                    {"HTTP_VERSION", 505},
                }};
            for (const auto& [name, value] : statuses) {
                auto stored = set_static_int(
                    machine, "javax/microedition/io/HttpConnection",
                    name, value);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
}

void register_security_objects(NativeMethodRegistry& registry) {
    const auto reference_getter = [&registry](std::string owner,
                                              std::string method,
                                              std::string descriptor,
                                              usize field) {
        add(registry, std::move(owner), std::move(method),
            std::move(descriptor),
            [field](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, "security getter");
                if (!object) return std::unexpected(object.error());
                auto value = reference_field(machine, *object, field);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_reference(*value));
            });
    };
    reference_getter(std::string(kSecurityInfoClass),
                     "getServerCertificate",
                     "()Ljavax/microedition/pki/Certificate;",
                     kSecurityCertificateField);
    reference_getter(std::string(kSecurityInfoClass),
                     "getProtocolName", "()Ljava/lang/String;",
                     kSecurityProtocolNameField);
    reference_getter(std::string(kSecurityInfoClass),
                     "getProtocolVersion", "()Ljava/lang/String;",
                     kSecurityProtocolVersionField);
    reference_getter(std::string(kSecurityInfoClass),
                     "getCipherSuite", "()Ljava/lang/String;",
                     kSecurityCipherSuiteField);
    reference_getter(std::string(kCertificateClass),
                     "getSubject", "()Ljava/lang/String;",
                     kCertificateSubjectField);
    reference_getter(std::string(kCertificateClass),
                     "getIssuer", "()Ljava/lang/String;",
                     kCertificateIssuerField);
    reference_getter(std::string(kCertificateClass),
                     "getSerialNumber", "()Ljava/lang/String;",
                     kCertificateSerialField);

    const auto constant_string = [&registry](std::string method,
                                             std::string value) {
        add(registry, std::string(kCertificateClass), std::move(method),
            "()Ljava/lang/String;",
            [value = std::move(value)](
                Machine& machine, std::span<const Value>)
                -> Result<std::optional<Value>> {
                auto string = create_string(machine, value);
                if (!string) return std::unexpected(string.error());
                return std::optional<Value>(Value::from_reference(*string));
            });
    };
    constant_string("getType", "X.509");
    constant_string("getVersion", "3");
    constant_string("getSigAlgName", "");

    const auto long_getter = [&registry](std::string method, usize field) {
        add(registry, std::string(kCertificateClass), std::move(method),
            "()J",
            [field](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, "certificate date getter");
                if (!object) return std::unexpected(object.error());
                auto value = machine.heap().field(*object, field);
                if (!value) return std::unexpected(value.error());
                auto date = value->as_long();
                if (!date) return std::unexpected(date.error());
                return std::optional<Value>(Value::from_long(*date));
            });
    };
    long_getter("getNotBefore", kCertificateNotBeforeField);
    long_getter("getNotAfter", kCertificateNotAfterField);

    const auto certificate_exception_constructor =
        [&registry](std::string descriptor,
                    std::optional<usize> message_argument,
                    usize certificate_argument,
                    usize reason_argument) {
            add(registry, std::string(kCertificateExceptionClass), "<init>",
                std::move(descriptor),
                [message_argument, certificate_argument, reason_argument](
                    Machine& machine, std::span<const Value> arguments)
                    -> Result<std::optional<Value>> {
                    auto object = receiver(
                        arguments, "CertificateException constructor");
                    if (!object) return std::unexpected(object.error());
                    auto certificate = reference_argument(
                        arguments, certificate_argument,
                        "CertificateException certificate", true);
                    auto reason = int_argument(
                        arguments, reason_argument,
                        "CertificateException reason");
                    if (!certificate) {
                        return std::unexpected(certificate.error());
                    }
                    if (!reason) return std::unexpected(reason.error());

                    ObjectRef message;
                    if (message_argument.has_value()) {
                        auto supplied = reference_argument(
                            arguments, *message_argument,
                            "CertificateException message", true);
                        if (!supplied) {
                            return std::unexpected(supplied.error());
                        }
                        message = *supplied;
                    } else {
                        auto generated = create_string(
                            machine, certificate_exception_message(*reason));
                        if (!generated) {
                            return std::unexpected(generated.error());
                        }
                        message = *generated;
                    }

                    auto stored_message = machine.heap().set_field(
                        *object, kThrowableMessageField,
                        Value::from_reference(message));
                    auto stored_cause = machine.heap().set_field(
                        *object, kThrowableCauseField,
                        Value::from_reference(ObjectRef {}));
                    auto stored_cause_initialized = machine.heap().set_field(
                        *object, kThrowableCauseInitializedField,
                        Value::from_int(0));
                    auto stored_certificate = machine.heap().set_field(
                        *object, kCertificateExceptionCertificateField,
                        Value::from_reference(*certificate));
                    auto stored_reason = machine.heap().set_field(
                        *object, kCertificateExceptionReasonField,
                        Value::from_int(*reason));
                    if (!stored_message) {
                        return std::unexpected(stored_message.error());
                    }
                    if (!stored_cause) {
                        return std::unexpected(stored_cause.error());
                    }
                    if (!stored_cause_initialized) {
                        return std::unexpected(
                            stored_cause_initialized.error());
                    }
                    if (!stored_certificate) {
                        return std::unexpected(stored_certificate.error());
                    }
                    if (!stored_reason) {
                        return std::unexpected(stored_reason.error());
                    }
                    return std::optional<Value> {};
                });
        };
    certificate_exception_constructor(
        "(Ljavax/microedition/pki/Certificate;B)V", std::nullopt, 1U, 2U);
    certificate_exception_constructor(
        "(Ljava/lang/String;Ljavax/microedition/pki/Certificate;B)V",
        1U, 2U, 3U);

    reference_getter(std::string(kCertificateExceptionClass),
                     "getCertificate",
                     "()Ljavax/microedition/pki/Certificate;",
                     kCertificateExceptionCertificateField);
    add(registry, std::string(kCertificateExceptionClass), "getReason", "()B",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "CertificateException.getReason");
            if (!object) return std::unexpected(object.error());
            auto value = machine.heap().field(
                *object, kCertificateExceptionReasonField);
            if (!value) return std::unexpected(value.error());
            auto reason = value->as_int();
            if (!reason) return std::unexpected(reason.error());
            return std::optional<Value>(Value::from_int(*reason));
        });
}

[[nodiscard]] Status require_wireless_connection_open(
    Machine& machine,
    ObjectRef connection,
    i32 required_mode) {
    auto closed = int_field(
        machine, connection, kWirelessConnectionClosedField);
    if (!closed) return std::unexpected(closed.error());
    if (*closed != 0) {
        return fail_java("java/io/IOException",
                         "wireless message connection is closed");
    }
    auto mode = int_field(machine, connection, kWirelessConnectionModeField);
    if (!mode) return std::unexpected(mode.error());
    if ((*mode & required_mode) == 0) {
        return fail_java("java/io/IOException",
                         required_mode == 1
                             ? "wireless connection is not open for reading"
                             : "wireless connection is not open for writing");
    }
    return {};
}

[[nodiscard]] Result<ObjectRef> create_wireless_message(
    Machine& machine,
    ObjectRef type,
    ObjectRef address) {
    auto type_text = utf8_text(machine, type);
    if (!type_text) return std::unexpected(type_text.error());
    std::string_view class_name;
    if (*type_text == "text") {
        class_name = kWirelessTextMessageClass;
    } else if (*type_text == "binary") {
        class_name = kWirelessBinaryMessageClass;
    } else if (*type_text == "multipart") {
        class_name = kWirelessMultipartMessageClass;
    } else {
        return fail_java("java/lang/IllegalArgumentException",
                         "unknown wireless message type: " + *type_text);
    }

    auto message = machine.class_states().allocate_instance(
        machine.heap(), class_name);
    if (!message) return std::unexpected(message.error());
    auto address_stored = set_reference_field(
        machine, *message, kWirelessMessageAddressField, address);
    auto timestamp_stored = set_long_field(
        machine, *message, kWirelessMessageTimestampField, 0);
    if (!address_stored) return std::unexpected(address_stored.error());
    if (!timestamp_stored) return std::unexpected(timestamp_stored.error());
    if (class_name == kWirelessMultipartMessageClass) {
        auto parts = machine.heap().allocate_array(
            "[Ljavax/wireless/messaging/MessagePart;", 0U,
            Value::from_reference({}));
        if (!parts) return std::unexpected(parts.error());
        auto parts_stored = set_reference_field(
            machine, *message, kWirelessMultipartPartsField, *parts);
        auto count_stored = set_int_field(
            machine, *message, kWirelessMultipartPartCountField, 0);
        if (!parts_stored) return std::unexpected(parts_stored.error());
        if (!count_stored) return std::unexpected(count_stored.error());
    }
    return *message;
}

[[nodiscard]] Result<std::optional<Value>> wireless_message_timestamp(
    Machine& machine,
    std::span<const Value> arguments) {
    auto message = receiver(arguments, "Message.getTimestamp");
    if (!message) return std::unexpected(message.error());
    auto timestamp = long_field(
        machine, *message, kWirelessMessageTimestampField);
    if (!timestamp) return std::unexpected(timestamp.error());
    if (*timestamp <= 0) {
        return std::optional<Value>(Value::from_reference({}));
    }
    auto date = machine.class_states().allocate_instance(
        machine.heap(), "java/util/Date");
    if (!date) return std::unexpected(date.error());
    auto stored = set_long_field(machine, *date, 0, *timestamp);
    if (!stored) return std::unexpected(stored.error());
    return std::optional<Value>(Value::from_reference(*date));
}

void register_wireless_messaging(NativeMethodRegistry& registry) {
    add(registry, "javax/wireless/messaging/MessageConnection",
        "<clinit>", "()V",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            auto text = set_static_string(
                machine, "javax/wireless/messaging/MessageConnection",
                "TEXT_MESSAGE", "text");
            if (!text) return std::unexpected(text.error());
            auto binary = set_static_string(
                machine, "javax/wireless/messaging/MessageConnection",
                "BINARY_MESSAGE", "binary");
            if (!binary) return std::unexpected(binary.error());
            auto multipart = set_static_string(
                machine, "javax/wireless/messaging/MessageConnection",
                "MULTIPART_MESSAGE", "multipart");
            if (!multipart) return std::unexpected(multipart.error());
            return std::optional<Value> {};
        });

    add(registry, std::string(kWirelessConnectionClass), "close", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(arguments, "MessageConnection.close");
            if (!connection) return std::unexpected(connection.error());
            auto stored = set_int_field(
                machine, *connection, kWirelessConnectionClosedField, 1);
            if (!stored) return std::unexpected(stored.error());
            auto listener_cleared = set_reference_field(
                machine, *connection, kWirelessConnectionListenerField, {});
            if (!listener_cleared) {
                return std::unexpected(listener_cleared.error());
            }
            return std::optional<Value> {};
        });

    add(registry, std::string(kWirelessConnectionClass), "newMessage",
        "(Ljava/lang/String;)Ljavax/wireless/messaging/Message;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(arguments, "MessageConnection.newMessage");
            auto type = reference_argument(arguments, 1, "message type");
            if (!connection) return std::unexpected(connection.error());
            if (!type) return std::unexpected(type.error());
            auto open = require_wireless_connection_open(
                machine, *connection, 2);
            if (!open) return std::unexpected(open.error());
            auto address = reference_field(
                machine, *connection, kWirelessConnectionAddressField);
            if (!address) return std::unexpected(address.error());
            auto message = create_wireless_message(
                machine, *type, *address);
            if (!message) return std::unexpected(message.error());
            return std::optional<Value>(Value::from_reference(*message));
        });

    add(registry, std::string(kWirelessConnectionClass), "newMessage",
        "(Ljava/lang/String;Ljava/lang/String;)Ljavax/wireless/messaging/Message;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(arguments, "MessageConnection.newMessage");
            auto type = reference_argument(arguments, 1, "message type");
            auto address = reference_argument(
                arguments, 2, "message address", true);
            if (!connection) return std::unexpected(connection.error());
            if (!type) return std::unexpected(type.error());
            if (!address) return std::unexpected(address.error());
            auto open = require_wireless_connection_open(
                machine, *connection, 2);
            if (!open) return std::unexpected(open.error());
            auto message = create_wireless_message(
                machine, *type, *address);
            if (!message) return std::unexpected(message.error());
            return std::optional<Value>(Value::from_reference(*message));
        });

    add(registry, std::string(kWirelessConnectionClass), "send",
        "(Ljavax/wireless/messaging/Message;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(arguments, "MessageConnection.send");
            auto message = reference_argument(arguments, 1, "message");
            if (!connection) return std::unexpected(connection.error());
            if (!message) return std::unexpected(message.error());
            auto open = require_wireless_connection_open(
                machine, *connection, 2);
            if (!open) return std::unexpected(open.error());
            auto address = reference_field(
                machine, *message, kWirelessMessageAddressField);
            if (!address) return std::unexpected(address.error());
            if (address->is_null()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "wireless message has no destination address");
            }
            return fail_java(
                "java/io/IOException",
                "wireless send requires an explicit iOS host transport");
        });

    add(registry, std::string(kWirelessConnectionClass), "receive",
        "()Ljavax/wireless/messaging/Message;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(arguments, "MessageConnection.receive");
            if (!connection) return std::unexpected(connection.error());
            auto open = require_wireless_connection_open(
                machine, *connection, 1);
            if (!open) return std::unexpected(open.error());
            return fail_java(
                "java/io/InterruptedIOException",
                "wireless receive requires an iOS host delivery bridge");
        });

    add(registry, std::string(kWirelessConnectionClass), "numberOfSegments",
        "(Ljavax/wireless/messaging/Message;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(
                arguments, "MessageConnection.numberOfSegments");
            auto message = reference_argument(arguments, 1, "message");
            if (!connection) return std::unexpected(connection.error());
            if (!message) return std::unexpected(message.error());
            auto open = require_wireless_connection_open(
                machine, *connection, 2);
            if (!open) return std::unexpected(open.error());

            auto is_text = machine.object_is_instance(
                *message, "javax/wireless/messaging/TextMessage");
            if (!is_text) return std::unexpected(is_text.error());
            if (*is_text) {
                auto payload = reference_field(
                    machine, *message, kWirelessMessagePayloadField);
                if (!payload) return std::unexpected(payload.error());
                if (payload->is_null()) {
                    return std::optional<Value>(Value::from_int(0));
                }
                auto text = machine.heap().string_value(*payload);
                if (!text) return std::unexpected(text.error());
                bool gsm_compatible = true;
                for (char16_t character : *text) {
                    if (static_cast<u16>(character) > 0x7FU) {
                        gsm_compatible = false;
                        break;
                    }
                }
                const usize first_capacity = gsm_compatible ? 160U : 70U;
                const usize chained_capacity = gsm_compatible ? 153U : 67U;
                const usize length = text->size();
                const usize segments = length <= first_capacity
                    ? 1U
                    : 1U + ((length - first_capacity +
                              chained_capacity - 1U) /
                             chained_capacity);
                if (segments > static_cast<usize>(
                        std::numeric_limits<i32>::max())) {
                    return fail_java(
                        "javax/wireless/messaging/SizeExceededException",
                        "wireless text message is too large");
                }
                return std::optional<Value>(
                    Value::from_int(static_cast<i32>(segments)));
            }

            auto is_binary = machine.object_is_instance(
                *message, "javax/wireless/messaging/BinaryMessage");
            if (!is_binary) return std::unexpected(is_binary.error());
            if (!*is_binary) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "object is not a wireless message");
            }
            auto payload = reference_field(
                machine, *message, kWirelessMessagePayloadField);
            if (!payload) return std::unexpected(payload.error());
            if (payload->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto length = byte_array_length(machine, *payload);
            if (!length) return std::unexpected(length.error());
            const usize segments = *length <= 140U
                ? 1U
                : 1U + ((*length - 140U + 133U) / 134U);
            if (segments > static_cast<usize>(
                    std::numeric_limits<i32>::max())) {
                return fail_java(
                    "javax/wireless/messaging/SizeExceededException",
                    "wireless binary message is too large");
            }
            return std::optional<Value>(
                Value::from_int(static_cast<i32>(segments)));
        });

    add(registry, std::string(kWirelessConnectionClass),
        "setMessageListener",
        "(Ljavax/wireless/messaging/MessageListener;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto connection = receiver(
                arguments, "MessageConnection.setMessageListener");
            auto listener = reference_argument(
                arguments, 1, "message listener", true);
            if (!connection) return std::unexpected(connection.error());
            if (!listener) return std::unexpected(listener.error());
            auto open = require_wireless_connection_open(
                machine, *connection, 1);
            if (!open) return std::unexpected(open.error());
            auto stored = set_reference_field(
                machine, *connection, kWirelessConnectionListenerField,
                *listener);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    const auto register_common_message = [&registry](
        std::string_view owner) {
        add(registry, std::string(owner), "getAddress",
            "()Ljava/lang/String;",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto message = receiver(arguments, "Message.getAddress");
                if (!message) return std::unexpected(message.error());
                auto address = reference_field(
                    machine, *message, kWirelessMessageAddressField);
                if (!address) return std::unexpected(address.error());
                return std::optional<Value>(Value::from_reference(*address));
            });
        add(registry, std::string(owner), "setAddress",
            "(Ljava/lang/String;)V",
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto message = receiver(arguments, "Message.setAddress");
                auto address = reference_argument(
                    arguments, 1, "message address", true);
                if (!message) return std::unexpected(message.error());
                if (!address) return std::unexpected(address.error());
                auto stored = set_reference_field(
                    machine, *message, kWirelessMessageAddressField,
                    *address);
                if (!stored) return std::unexpected(stored.error());
                return std::optional<Value> {};
            });
        add(registry, std::string(owner), "getTimestamp",
            "()Ljava/util/Date;", wireless_message_timestamp);
    };
    register_common_message(kWirelessTextMessageClass);
    register_common_message(kWirelessBinaryMessageClass);
    register_common_message(kWirelessMultipartMessageClass);

    add(registry, std::string(kWirelessTextMessageClass),
        "getPayloadText", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto message = receiver(arguments, "TextMessage.getPayloadText");
            if (!message) return std::unexpected(message.error());
            auto payload = reference_field(
                machine, *message, kWirelessMessagePayloadField);
            if (!payload) return std::unexpected(payload.error());
            return std::optional<Value>(Value::from_reference(*payload));
        });
    add(registry, std::string(kWirelessTextMessageClass),
        "setPayloadText", "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto message = receiver(arguments, "TextMessage.setPayloadText");
            auto payload = reference_argument(
                arguments, 1, "text payload", true);
            if (!message) return std::unexpected(message.error());
            if (!payload) return std::unexpected(payload.error());
            auto stored = set_reference_field(
                machine, *message, kWirelessMessagePayloadField, *payload);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    add(registry, std::string(kWirelessBinaryMessageClass),
        "getPayloadData", "()[B",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto message = receiver(
                arguments, "BinaryMessage.getPayloadData");
            if (!message) return std::unexpected(message.error());
            auto payload = reference_field(
                machine, *message, kWirelessMessagePayloadField);
            if (!payload) return std::unexpected(payload.error());
            return std::optional<Value>(Value::from_reference(*payload));
        });
    add(registry, std::string(kWirelessBinaryMessageClass),
        "setPayloadData", "([B)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto message = receiver(
                arguments, "BinaryMessage.setPayloadData");
            auto payload = reference_argument(
                arguments, 1, "binary payload", true);
            if (!message) return std::unexpected(message.error());
            if (!payload) return std::unexpected(payload.error());
            auto stored = set_reference_field(
                machine, *message, kWirelessMessagePayloadField, *payload);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    add(registry, std::string(kWirelessMessagePartClass), "<init>",
        "([BIILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto part = receiver(arguments, "MessagePart.<init>");
            auto source = reference_argument(arguments, 1, "content", false);
            if (!part) return std::unexpected(part.error());
            if (!source) return std::unexpected(source.error());
            if (arguments.size() < 8U) {
                return fail(ErrorCode::invalid_argument,
                            "MessagePart constructor arguments are missing");
            }
            auto offset = arguments[2].as_int();
            auto length = arguments[3].as_int();
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            auto source_length = byte_array_length(machine, *source);
            if (!source_length) return std::unexpected(source_length.error());
            if (*offset < 0 || *length < 0 ||
                static_cast<usize>(*offset) > *source_length ||
                static_cast<usize>(*length) >
                    *source_length - static_cast<usize>(*offset)) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "MessagePart content range is invalid");
            }
            auto bytes = machine.heap().read_byte_array(
                *source, static_cast<usize>(*offset),
                static_cast<usize>(*length));
            if (!bytes) return std::unexpected(bytes.error());
            auto content = machine.heap().allocate_array(
                "[B", bytes->size(), Value::from_int(0));
            if (!content) return std::unexpected(content.error());
            auto written = machine.heap().write_byte_array(*content, 0U, *bytes);
            if (!written) return std::unexpected(written.error());
            auto content_root = machine.pin_native_root(*content);
            if (!content_root) return std::unexpected(content_root.error());
            const std::array<usize, 4> argument_indices {{4U, 5U, 6U, 7U}};
            const std::array<usize, 4> field_indices {{
                kWirelessPartContentTypeField, kWirelessPartContentIdField,
                kWirelessPartContentLocationField, kWirelessPartEncodingField,
            }};
            auto content_stored = set_reference_field(
                machine, *part, kWirelessPartContentField, *content);
            if (!content_stored) return std::unexpected(content_stored.error());
            for (usize index = 0; index < argument_indices.size(); ++index) {
                auto value = arguments[argument_indices[index]].as_reference();
                if (!value) return std::unexpected(value.error());
                auto stored = set_reference_field(machine, *part,
                                                  field_indices[index], *value);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
    add(registry, std::string(kWirelessMessagePartClass), "<init>",
        "(Ljava/io/InputStream;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
        [](Machine&, std::span<const Value>)
            -> Result<std::optional<Value>> {
            return fail_java("java/io/IOException",
                             "stream-backed MessagePart is not available");
        });
    const std::array<std::pair<std::string_view, usize>, 5> part_getters {{
        {"getContent", kWirelessPartContentField},
        {"getContentType", kWirelessPartContentTypeField},
        {"getContentID", kWirelessPartContentIdField},
        {"getContentLocation", kWirelessPartContentLocationField},
        {"getEncoding", kWirelessPartEncodingField},
    }};
    for (const auto& [name, field_index] : part_getters) {
        const std::string descriptor = name == "getContent"
            ? "()[B" : "()Ljava/lang/String;";
        add(registry, std::string(kWirelessMessagePartClass),
            std::string(name), descriptor,
            [field_index](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto part = receiver(arguments, "MessagePart getter");
                if (!part) return std::unexpected(part.error());
                auto value = reference_field(machine, *part, field_index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_reference(*value));
            });
    }
    add(registry, std::string(kWirelessMessagePartClass), "getLength", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto part = receiver(arguments, "MessagePart.getLength");
            if (!part) return std::unexpected(part.error());
            auto content = reference_field(machine, *part,
                                           kWirelessPartContentField);
            if (!content) return std::unexpected(content.error());
            if (content->is_null()) return std::optional<Value>(Value::from_int(0));
            auto length = byte_array_length(machine, *content);
            if (!length) return std::unexpected(length.error());
            if (*length > static_cast<usize>(std::numeric_limits<i32>::max())) {
                return fail_java("javax/wireless/messaging/SizeExceededException",
                                 "MessagePart is too large");
            }
            return std::optional<Value>(
                Value::from_int(static_cast<i32>(*length)));
        });
    add(registry, std::string(kWirelessMessagePartClass),
        "getContentAsStream", "()Ljava/io/InputStream;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto part = receiver(arguments, "MessagePart.getContentAsStream");
            if (!part) return std::unexpected(part.error());
            auto content = reference_field(machine, *part,
                                           kWirelessPartContentField);
            if (!content) return std::unexpected(content.error());
            if (content->is_null()) {
                return std::optional<Value>(Value::from_reference({}));
            }
            auto stream = machine.class_states().allocate_instance(
                machine.heap(), "java/io/ByteArrayInputStream");
            if (!stream) return std::unexpected(stream.error());
            auto stream_root = machine.pin_native_root(*stream);
            if (!stream_root) return std::unexpected(stream_root.error());
            const Value content_argument = Value::from_reference(*content);
            auto initialized = machine.invoke_instance(
                *stream, "java/io/ByteArrayInputStream", "<init>", "([B)V",
                std::span<const Value>(&content_argument, 1U));
            if (!initialized) return std::unexpected(initialized.error());
            if (initialized->throwable.has_value()) {
                return fail_java("java/io/IOException",
                                 "MessagePart stream initialization failed");
            }
            return std::optional<Value>(Value::from_reference(*stream));
        });

    for (const auto& [method, field_index] : {
             std::pair<std::string_view, usize>{"getSubject",
                                                kWirelessMultipartSubjectField},
             {"getStartContentId", kWirelessMultipartStartContentIdField}}) {
        add(registry, std::string(kWirelessMultipartMessageClass),
            std::string(method), "()Ljava/lang/String;",
            [field_index](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto message = receiver(arguments, "MultipartMessage getter");
                if (!message) return std::unexpected(message.error());
                auto value = reference_field(machine, *message, field_index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_reference(*value));
            });
    }
    for (const auto& [method, field_index] : {
             std::pair<std::string_view, usize>{"setSubject",
                                                kWirelessMultipartSubjectField},
             {"setStartContentId", kWirelessMultipartStartContentIdField}}) {
        add(registry, std::string(kWirelessMultipartMessageClass),
            std::string(method), "(Ljava/lang/String;)V",
            [field_index](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto message = receiver(arguments, "MultipartMessage setter");
                auto value = reference_argument(arguments, 1, "value", true);
                if (!message) return std::unexpected(message.error());
                if (!value) return std::unexpected(value.error());
                auto stored = set_reference_field(machine, *message,
                                                  field_index, *value);
                if (!stored) return std::unexpected(stored.error());
                return std::optional<Value> {};
            });
    }
    add(registry, std::string(kWirelessMultipartMessageClass),
        "addMessagePart", "(Ljavax/wireless/messaging/MessagePart;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto message = receiver(arguments, "MultipartMessage.addMessagePart");
            auto part = reference_argument(arguments, 1, "message part", false);
            if (!message) return std::unexpected(message.error());
            if (!part) return std::unexpected(part.error());
            auto count = int_field(machine, *message,
                                   kWirelessMultipartPartCountField);
            auto old_parts = reference_field(machine, *message,
                                             kWirelessMultipartPartsField);
            if (!count) return std::unexpected(count.error());
            if (!old_parts) return std::unexpected(old_parts.error());
            if (*count < 0 || *count >= 64) {
                return fail_java("javax/wireless/messaging/SizeExceededException",
                                 "too many multipart message parts");
            }
            auto new_parts = machine.heap().allocate_array(
                "[Ljavax/wireless/messaging/MessagePart;",
                static_cast<usize>(*count + 1), Value::from_reference({}));
            if (!new_parts) return std::unexpected(new_parts.error());
            auto new_root = machine.pin_native_root(*new_parts);
            if (!new_root) return std::unexpected(new_root.error());
            for (i32 index = 0; index < *count; ++index) {
                auto value = machine.heap().element(
                    *old_parts, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto stored = machine.heap().set_element(
                    *new_parts, static_cast<usize>(index), *value);
                if (!stored) return std::unexpected(stored.error());
            }
            auto appended = machine.heap().set_element(
                *new_parts, static_cast<usize>(*count),
                Value::from_reference(*part));
            if (!appended) return std::unexpected(appended.error());
            auto parts_stored = set_reference_field(
                machine, *message, kWirelessMultipartPartsField, *new_parts);
            auto count_stored = set_int_field(
                machine, *message, kWirelessMultipartPartCountField,
                *count + 1);
            if (!parts_stored) return std::unexpected(parts_stored.error());
            if (!count_stored) return std::unexpected(count_stored.error());
            return std::optional<Value> {};
        });
    add(registry, std::string(kWirelessMultipartMessageClass),
        "getMessageParts", "()[Ljavax/wireless/messaging/MessagePart;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto message = receiver(arguments, "MultipartMessage.getMessageParts");
            if (!message) return std::unexpected(message.error());
            auto parts = reference_field(machine, *message,
                                         kWirelessMultipartPartsField);
            if (!parts) return std::unexpected(parts.error());
            return std::optional<Value>(Value::from_reference(*parts));
        });
    add(registry, std::string(kWirelessMultipartMessageClass), "addAddress",
        "(Ljava/lang/String;Ljava/lang/String;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto message = receiver(arguments, "MultipartMessage.addAddress");
            auto address = reference_argument(arguments, 2, "address", false);
            if (!message) return std::unexpected(message.error());
            if (!address) return std::unexpected(address.error());
            auto stored = set_reference_field(
                machine, *message, kWirelessMessageAddressField, *address);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_int(1));
        });
    add(registry, std::string(kWirelessMultipartMessageClass), "getAddresses",
        "(Ljava/lang/String;)[Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto message = receiver(arguments, "MultipartMessage.getAddresses");
            if (!message) return std::unexpected(message.error());
            auto address = reference_field(machine, *message,
                                           kWirelessMessageAddressField);
            if (!address) return std::unexpected(address.error());
            const usize length = address->is_null() ? 0U : 1U;
            auto result = machine.heap().allocate_array(
                "[Ljava/lang/String;", length, Value::from_reference({}));
            if (!result) return std::unexpected(result.error());
            if (length != 0U) {
                auto stored = machine.heap().set_element(
                    *result, 0U, Value::from_reference(*address));
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value>(Value::from_reference(*result));
        });
    for (const auto& [method, descriptor] : {
             std::pair<std::string_view, std::string_view>{
                 "getHeader", "(Ljava/lang/String;)Ljava/lang/String;"},
             {"getMessagePart", "(Ljava/lang/String;)Ljavax/wireless/messaging/MessagePart;"}}) {
        add(registry, std::string(kWirelessMultipartMessageClass),
            std::string(method), std::string(descriptor),
            [](Machine&, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto message = receiver(arguments, "MultipartMessage lookup");
                if (!message) return std::unexpected(message.error());
                return std::optional<Value>(Value::from_reference({}));
            });
    }
    for (const auto& [method, descriptor] : {
             std::pair<std::string_view, std::string_view>{
                 "removeAddress", "(Ljava/lang/String;Ljava/lang/String;)Z"},
             {"removeMessagePart", "(Ljavax/wireless/messaging/MessagePart;)Z"},
             {"removeMessagePartId", "(Ljava/lang/String;)Z"},
             {"removeMessagePartLocation", "(Ljava/lang/String;)Z"}}) {
        add(registry, std::string(kWirelessMultipartMessageClass),
            std::string(method), std::string(descriptor),
            [](Machine&, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto message = receiver(arguments, "MultipartMessage remove");
                if (!message) return std::unexpected(message.error());
                return std::optional<Value>(Value::from_int(0));
            });
    }
    for (const auto& [method, descriptor] : {
             std::pair<std::string_view, std::string_view>{
                 "removeAddresses", "(Ljava/lang/String;)V"},
             {"setHeader", "(Ljava/lang/String;Ljava/lang/String;)V"}}) {
        add(registry, std::string(kWirelessMultipartMessageClass),
            std::string(method), std::string(descriptor),
            [](Machine&, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto message = receiver(arguments, "MultipartMessage operation");
                if (!message) return std::unexpected(message.error());
                return std::optional<Value> {};
            });
    }
}

} // namespace

Result<std::optional<i32>> connection_stream_read_one(
    Machine& machine,
    ObjectRef stream) {
    return native_read_one(machine, stream);
}

Result<std::optional<i32>> connection_stream_read_range(
    Machine& machine,
    ObjectRef stream,
    ObjectRef destination,
    i32 offset,
    i32 length) {
    auto is_native = machine.object_is_instance(stream, kInputStreamClass);
    if (!is_native) return std::unexpected(is_native.error());
    if (!*is_native) return std::optional<i32> {};
    auto value = native_read_range(machine,
                                   stream,
                                   destination,
                                   offset,
                                   length);
    if (!value) return std::unexpected(value.error());
    return std::optional<i32>(*value);
}

Result<std::optional<usize>> connection_stream_available(
    Machine& machine,
    ObjectRef stream) {
    auto is_native = machine.object_is_instance(stream, kInputStreamClass);
    if (!is_native) return std::unexpected(is_native.error());
    if (!*is_native) return std::optional<usize> {};
    auto open = require_open_stream(machine, stream);
    if (!open) return std::unexpected(open.error());
    auto position = int_field(machine, stream, kStreamBufferPositionField);
    auto count = int_field(machine, stream, kStreamBufferCountField);
    if (!position) return std::unexpected(position.error());
    if (!count) return std::unexpected(count.error());
    if (*position < 0 || *count < *position) {
        return fail(ErrorCode::invalid_state,
                    "network input buffer state is invalid");
    }
    const usize buffered = static_cast<usize>(*count - *position);
    auto token = token_from_object(machine, stream);
    if (!token) return std::unexpected(token.error());
    auto available = java_network_result(
        machine.connections().available(*token));
    if (!available) return std::unexpected(available.error());
    if (*available > std::numeric_limits<usize>::max() - buffered) {
        return std::optional<usize>(std::numeric_limits<usize>::max());
    }
    return std::optional<usize>(buffered + *available);
}

Result<std::optional<bool>> connection_stream_write_bytes(
    Machine& machine,
    ObjectRef stream,
    std::span<const u8> bytes) {
    auto is_native = machine.object_is_instance(stream, kOutputStreamClass);
    if (!is_native) return std::unexpected(is_native.error());
    if (!*is_native) return std::optional<bool> {};
    auto open = require_open_stream(machine, stream);
    if (!open) return std::unexpected(open.error());
    auto token = token_from_object(machine, stream);
    if (!token) return std::unexpected(token.error());
    auto written = java_network_status(
        machine.connections().write(*token, bytes));
    if (!written) return std::unexpected(written.error());
    return std::optional<bool>(true);
}

Result<std::optional<bool>> connection_stream_write_one(
    Machine& machine,
    ObjectRef stream,
    u8 byte) {
    const std::array<u8, 1> bytes {byte};
    return connection_stream_write_bytes(machine, stream, bytes);
}

Result<std::optional<bool>> connection_stream_flush(
    Machine& machine,
    ObjectRef stream) {
    auto is_native = machine.object_is_instance(stream, kOutputStreamClass);
    if (!is_native) return std::unexpected(is_native.error());
    if (!*is_native) return std::optional<bool> {};
    auto token = token_from_object(machine, stream);
    if (!token) return std::unexpected(token.error());
    auto flushed = java_network_status(machine.connections().flush(*token));
    if (!flushed) return std::unexpected(flushed.error());
    return std::optional<bool>(true);
}

Result<std::optional<bool>> connection_stream_close_input(
    Machine& machine,
    ObjectRef stream) {
    auto is_native = machine.object_is_instance(stream, kInputStreamClass);
    if (!is_native) return std::unexpected(is_native.error());
    if (!*is_native) return std::optional<bool> {};
    auto closed = close_native_stream(machine, stream, true);
    if (!closed) return std::unexpected(closed.error());
    return std::optional<bool>(true);
}

Result<std::optional<bool>> connection_stream_close_output(
    Machine& machine,
    ObjectRef stream) {
    auto is_native = machine.object_is_instance(stream, kOutputStreamClass);
    if (!is_native) return std::unexpected(is_native.error());
    if (!*is_native) return std::optional<bool> {};
    auto closed = close_native_stream(machine, stream, false);
    if (!closed) return std::unexpected(closed.error());
    return std::optional<bool>(true);
}

void register_connection_natives(NativeMethodRegistry& registry) {
    register_connection_constants(registry);
    register_java_net_socket(registry);
    register_connector(registry);
    register_stream_connections(registry);
    register_native_streams(registry);
    register_socket_connection(registry);
    register_server_connection(registry);
    register_datagram_connection(registry);
    register_datagram_object(registry);
    register_http_connection(registry);
    register_security_objects(registry);
    register_wireless_messaging(registry);
}

} // namespace phoneme::vm
