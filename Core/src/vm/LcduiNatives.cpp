#include "LcduiNatives.hpp"
#include "ChoiceNatives.hpp"
#include "phoneme/vm/LcduiBridge.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "phoneme/vm/Machine.hpp"

namespace phoneme::vm {
namespace {

constexpr i32 kEventScreenCreated = 2;
constexpr i32 kEventScreenUpdated = 3;
constexpr i32 kEventScreenShown = 4;
constexpr i32 kEventScreenHidden = 5;
constexpr i32 kEventItemCreated = 7;
constexpr i32 kEventItemUpdated = 8;
constexpr i32 kEventItemShown = 9;
constexpr i32 kEventItemHidden = 10;
constexpr i32 kEventItemDeleted = 11;
constexpr i32 kEventCommandsReset = 14;
constexpr i32 kEventCommand = 15;

constexpr i32 kCommandTypeItem = 8;

constexpr i32 kTypeExclusiveChoice = 1;
constexpr i32 kTypeMultipleChoice = 3;
constexpr i32 kTypeCustomItem = 4;
constexpr i32 kTypeDateField = 5;
constexpr i32 kTypeProgressGauge = 6;
constexpr i32 kTypeInteractiveGauge = 7;
constexpr i32 kTypePlainImage = 8;
constexpr i32 kTypeHyperlinkImage = 9;
constexpr i32 kTypeButtonImage = 10;
constexpr i32 kTypeSpacer = 11;
constexpr i32 kTypePlainString = 12;
constexpr i32 kTypeHyperlinkString = 13;
constexpr i32 kTypeButtonString = 14;
constexpr i32 kTypeTextField = 15;
constexpr i32 kTypeNullAlert = 16;
constexpr i32 kTypeInfoAlert = 17;
constexpr i32 kTypeWarningAlert = 18;
constexpr i32 kTypeErrorAlert = 19;
constexpr i32 kTypeAlarmAlert = 20;
constexpr i32 kTypeConfirmationAlert = 21;
constexpr i32 kTypeForm = 23;

constexpr i32 kTextFieldMetadata = -1001;
constexpr i32 kGaugeMetadata = -1002;
constexpr i32 kDateFieldMetadata = -1003;
constexpr i32 kImageMetadata = -1004;
constexpr i32 kItemStyleMetadata = -1005;
constexpr i32 kScreenKindMetadata = -1006;
constexpr i32 kTickerMetadata = -1008;
constexpr i32 kAlertMetadata = -1009;
constexpr i32 kScreenKindList = 1;
constexpr i32 kScreenKindTextBox = 2;
constexpr i32 kScreenKindAlert = 3;

constexpr i32 kConstraintMask = 0xFFFF;
constexpr i32 kConstraintAny = 0;
constexpr i32 kConstraintEmail = 1;
constexpr i32 kConstraintNumeric = 2;
constexpr i32 kConstraintPhone = 3;
constexpr i32 kConstraintUrl = 4;
constexpr i32 kConstraintDecimal = 5;
constexpr i32 kConstraintUneditable = 0x20000;

constexpr usize kCommandIdField = 0;
constexpr usize kCommandLabelField = 1;
constexpr usize kCommandLongLabelField = 2;
constexpr usize kCommandTypeField = 3;
constexpr usize kCommandPriorityField = 4;
constexpr usize kCommandOwnerItemField = 5;

constexpr usize kDisplayableIdField = 0;
constexpr usize kDisplayableTypeField = 1;
constexpr usize kDisplayableTitleField = 2;
constexpr usize kDisplayableListenerField = 3;
constexpr usize kDisplayableCommandsField = 4;
constexpr usize kDisplayableCommandCountField = 5;
constexpr usize kDisplayableShownField = 6;
constexpr usize kDisplayableTickerField = 7;
constexpr usize kDisplayableScrollField = 8;

constexpr usize kDisplayCurrentField = 0;

constexpr usize kItemIdField = 0;
constexpr usize kItemTypeField = 1;
constexpr usize kItemLabelField = 2;
constexpr usize kItemParentField = 3;
constexpr usize kItemLayoutField = 4;
constexpr usize kItemListenerField = 5;
constexpr usize kItemCommandsField = 6;
constexpr usize kItemCommandCountField = 7;
constexpr usize kItemDefaultCommandField = 8;
constexpr usize kItemPreferredWidthField = 9;
constexpr usize kItemPreferredHeightField = 10;

constexpr usize kFormItemsField = 9;
constexpr usize kFormItemCountField = 10;
constexpr usize kFormItemStateListenerField = 11;
constexpr usize kChoiceGroupFitPolicyField = 16;

constexpr usize kStringItemTextField = 11;
constexpr usize kStringItemAppearanceField = 12;
constexpr usize kStringItemFontField = 13;

constexpr usize kTextFieldTextField = 11;
constexpr usize kTextFieldMaxSizeField = 12;
constexpr usize kTextFieldConstraintsField = 13;
constexpr usize kTextFieldCaretField = 14;
constexpr usize kTextFieldInputModeField = 15;

constexpr usize kGaugeInteractiveField = 11;
constexpr usize kGaugeMaxValueField = 12;
constexpr usize kGaugeValueField = 13;

constexpr usize kDateFieldDateField = 11;
constexpr usize kDateFieldInputModeField = 12;
constexpr usize kDateFieldTimeZoneField = 13;

constexpr usize kSpacerWidthField = 11;
constexpr usize kSpacerHeightField = 12;

constexpr usize kImageItemImageField = 11;
constexpr usize kImageItemAltTextField = 12;
constexpr usize kImageItemAppearanceField = 13;
constexpr usize kImageItemGenerationField = 14;

constexpr usize kCustomItemPaintImageField = 11;
constexpr usize kCustomItemPaintGenerationField = 12;

constexpr usize kListPeerIdField = 9;
constexpr usize kListChoiceTypeField = 10;
constexpr usize kTextBoxPeerIdField = 9;
constexpr usize kTextBoxTextField = 10;
constexpr usize kTextBoxMaxSizeField = 11;
constexpr usize kTextBoxConstraintsField = 12;
constexpr usize kTextBoxCaretField = 13;
constexpr usize kTextBoxInputModeField = 14;

constexpr usize kDateTimeField = 0;
constexpr usize kTimeZoneIdField = 0;
constexpr usize kTimeZoneRawOffsetField = 1;
constexpr i32 kDateModeDate = 1;
constexpr i32 kDateModeTime = 2;
constexpr i64 kMillisecondsPerMinute = 60'000LL;
constexpr i64 kMillisecondsPerDay = 86'400'000LL;

constexpr usize kAlertTextField = 9;
constexpr usize kAlertImageField = 10;
constexpr usize kAlertTypeField = 11;
constexpr usize kAlertTimeoutField = 12;
constexpr usize kAlertNextField = 13;
constexpr usize kAlertImageGenerationField = 14;
constexpr usize kAlertIndicatorField = 15;
constexpr usize kAlertTypeKindField = 0;
constexpr usize kTickerTextField = 0;
constexpr usize kTickerOwnerField = 1;
constexpr usize kImageWidthField = 0;
constexpr usize kImageHeightField = 1;
constexpr i32 kAlertForever = -2;
constexpr i32 kDefaultAlertTimeout = 2'000;

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
                    "LCDUI native method is missing its receiver");
    }
    auto object = arguments.front().as_reference();
    if (!object || object->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "LCDUI native receiver is null");
    }
    return *object;
}

[[nodiscard]] Result<ObjectRef> reference_argument(
    std::span<const Value> arguments,
    usize index,
    bool allow_null = true) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    "LCDUI native reference argument is missing");
    }
    auto reference = arguments[index].as_reference();
    if (!reference) return std::unexpected(reference.error());
    if (!allow_null && reference->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "LCDUI reference argument is null");
    }
    return *reference;
}

[[nodiscard]] Result<i32> integer_argument(std::span<const Value> arguments,
                                           usize index) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    "LCDUI native integer argument is missing");
    }
    return arguments[index].as_int();
}

[[nodiscard]] Result<Value> field_value(Machine& machine,
                                        ObjectRef object,
                                        usize index) {
    return machine.heap().field(object, index);
}

template <usize N>
[[nodiscard]] Result<std::array<Value, N>> field_values(
    Machine& machine,
    ObjectRef object,
    const std::array<usize, N>& indices) {
    std::array<Value, N> values {};
    auto loaded = machine.heap().read_fields(object, indices, values);
    if (!loaded) return std::unexpected(loaded.error());
    return values;
}

template <usize N>
[[nodiscard]] Status set_field_values(
    Machine& machine,
    ObjectRef object,
    const std::array<usize, N>& indices,
    const std::array<Value, N>& values) {
    return machine.heap().write_fields(object, indices, values);
}

[[nodiscard]] Result<i32> int_field(Machine& machine,
                                    ObjectRef object,
                                    usize index) {
    auto value = field_value(machine, object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

[[nodiscard]] Result<ObjectRef> reference_field(Machine& machine,
                                                ObjectRef object,
                                                usize index) {
    auto value = field_value(machine, object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Result<i64> long_field(Machine& machine,
                                     ObjectRef object,
                                     usize index) {
    auto value = field_value(machine, object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_long();
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

[[nodiscard]] i64 floor_multiple(i64 value, i64 unit) noexcept {
    const i64 remainder = value % unit;
    return remainder < 0 ? value - remainder - unit : value - remainder;
}

[[nodiscard]] i64 floor_mod(i64 value, i64 unit) noexcept {
    const i64 remainder = value % unit;
    return remainder < 0 ? remainder + unit : remainder;
}

[[nodiscard]] Result<i64> checked_offset(i64 value,
                                         i32 offset,
                                         bool add) {
    const i64 signed_offset = add
        ? static_cast<i64>(offset)
        : -static_cast<i64>(offset);
    if ((signed_offset > 0 &&
         value > std::numeric_limits<i64>::max() - signed_offset) ||
        (signed_offset < 0 &&
         value < std::numeric_limits<i64>::min() - signed_offset)) {
        return fail(ErrorCode::overflow,
                    "DateField timezone adjustment overflows long");
    }
    return value + signed_offset;
}

[[nodiscard]] Result<ObjectRef> copy_date(Machine& machine,
                                          i64 milliseconds) {
    auto date = machine.class_states().allocate_instance(
        machine.heap(), "java/util/Date");
    if (!date) return std::unexpected(date.error());
    auto stored = set_long_field(machine, *date, kDateTimeField,
                                 milliseconds);
    if (!stored) return std::unexpected(stored.error());
    return *date;
}

[[nodiscard]] Result<ObjectRef> normalize_date_field_value(
    Machine& machine,
    ObjectRef item,
    ObjectRef date,
    i32 mode,
    bool mode_change) {
    if (date.is_null()) return ObjectRef {};
    auto milliseconds = long_field(machine, date, kDateTimeField);
    auto zone = reference_field(machine, item, kDateFieldTimeZoneField);
    if (!milliseconds) return std::unexpected(milliseconds.error());
    if (!zone) return std::unexpected(zone.error());
    if (zone->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "DateField timezone is unavailable");
    }
    auto offset = int_field(machine, *zone, kTimeZoneRawOffsetField);
    if (!offset) return std::unexpected(offset.error());
    auto local = checked_offset(*milliseconds, *offset, true);
    if (!local) return std::unexpected(local.error());

    i64 normalized_local = *local;
    if (mode == kDateModeDate) {
        normalized_local = floor_multiple(*local, kMillisecondsPerDay);
    } else if (mode == kDateModeTime) {
        if (!mode_change && (*local < 0 || *local >= kMillisecondsPerDay)) {
            // MIDP TIME fields only accept times on the local zero-epoch day.
            return ObjectRef {};
        }
        normalized_local = floor_mod(*local, kMillisecondsPerDay);
        normalized_local = floor_multiple(normalized_local,
                                          kMillisecondsPerMinute);
    } else {
        normalized_local = floor_multiple(*local, kMillisecondsPerMinute);
    }

    auto normalized_utc = checked_offset(normalized_local, *offset, false);
    if (!normalized_utc) return std::unexpected(normalized_utc.error());
    return copy_date(machine, *normalized_utc);
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

[[nodiscard]] Result<ObjectRef> empty_string(Machine& machine) {
    return create_string(machine, {});
}

[[nodiscard]] Result<ObjectRef> create_default_font(Machine& machine) {
    auto font = machine.class_states().allocate_instance(
        machine.heap(), "javax/microedition/lcdui/Font");
    if (!font) return std::unexpected(font.error());
    auto face = set_int_field(machine, *font, 0U, 0);
    auto style = set_int_field(machine, *font, 1U, 0);
    auto size = set_int_field(machine, *font, 2U, 0);
    if (!face) return std::unexpected(face.error());
    if (!style) return std::unexpected(style.error());
    if (!size) return std::unexpected(size.error());
    return *font;
}

[[nodiscard]] Result<std::u16string> string_text(Machine& machine,
                                                 ObjectRef string,
                                                 bool null_as_empty = true) {
    if (string.is_null()) {
        if (null_as_empty) return std::u16string {};
        return fail_java("java/lang/NullPointerException",
                         "LCDUI String argument is null");
    }
    return machine.heap().string_value(string);
}

void append_utf8(std::string& output, u32 code_point) {
    if (code_point <= 0x7FU) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(
            0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(
            0x80U | ((code_point >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(
            0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

[[nodiscard]] std::string utf8_text(std::u16string_view text) {
    std::string output;
    output.reserve(text.size() * 2U);
    for (usize index = 0; index < text.size(); ++index) {
        u32 code_point = static_cast<u16>(text[index]);
        if (code_point >= 0xD800U && code_point <= 0xDBFFU &&
            index + 1U < text.size()) {
            const u32 low = static_cast<u16>(text[index + 1U]);
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                code_point = 0x10000U + ((code_point - 0xD800U) << 10U) +
                             (low - 0xDC00U);
                ++index;
            } else {
                code_point = 0xFFFDU;
            }
        } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
            code_point = 0xFFFDU;
        }
        append_utf8(output, code_point);
    }
    return output;
}

[[nodiscard]] Result<std::string> utf8_string(Machine& machine,
                                              ObjectRef string,
                                              bool null_as_empty = true) {
    auto text = string_text(machine, string, null_as_empty);
    if (!text) return std::unexpected(text.error());
    return utf8_text(*text);
}

[[nodiscard]] Result<i32> ensure_native_id(Machine& machine,
                                           ObjectRef object,
                                           usize field_index) {
    auto current = int_field(machine, object, field_index);
    if (!current) return std::unexpected(current.error());
    if (*current != 0) {
        auto registered = machine.register_ui_component(*current, object);
        if (!registered) return std::unexpected(registered.error());
        return *current;
    }
    const i32 allocated = machine.allocate_ui_component_id();
    if (allocated == 0) {
        return fail_java("java/lang/OutOfMemoryError",
                         "LCDUI component ID space is exhausted");
    }
    auto stored = set_int_field(machine, object, field_index, allocated);
    if (!stored) return std::unexpected(stored.error());
    auto registered = machine.register_ui_component(allocated, object);
    if (!registered) return std::unexpected(registered.error());
    return allocated;
}

[[nodiscard]] Result<ObjectRef> ensure_reference_array(
    Machine& machine,
    ObjectRef owner,
    usize field_index,
    std::string class_name,
    usize minimum_capacity) {
    auto current = reference_field(machine, owner, field_index);
    if (!current) return std::unexpected(current.error());
    usize current_capacity = 0;
    if (!current->is_null()) {
        auto length = machine.heap().array_length(*current);
        if (!length) return std::unexpected(length.error());
        current_capacity = *length;
        if (current_capacity >= minimum_capacity) return *current;
    }

    usize capacity = std::max<usize>(4U, current_capacity);
    while (capacity < minimum_capacity) {
        if (capacity > std::numeric_limits<usize>::max() / 2U) {
            return fail_java("java/lang/OutOfMemoryError",
                             "LCDUI array capacity overflow");
        }
        capacity *= 2U;
    }
    auto replacement = machine.heap().allocate_array(
        std::move(class_name), capacity, Value::from_reference({}));
    if (!replacement) {
        if (replacement.error().code == ErrorCode::overflow) {
            return fail_java("java/lang/OutOfMemoryError",
                             "LCDUI array allocation failed");
        }
        return std::unexpected(replacement.error());
    }
    if (!current->is_null()) {
        auto copied = machine.heap().copy_array_range(
            *current, 0U, *replacement, 0U, current_capacity);
        if (!copied) return std::unexpected(copied.error());
    }
    auto assigned = set_reference_field(machine, owner, field_index,
                                        *replacement);
    if (!assigned) return std::unexpected(assigned.error());
    return *replacement;
}

[[nodiscard]] Result<bool> is_shown(Machine& machine,
                                    ObjectRef displayable) {
    auto shown = int_field(machine, displayable, kDisplayableShownField);
    if (!shown) return std::unexpected(shown.error());
    return *shown != 0;
}

template <usize N>
[[nodiscard]] Result<std::array<bool, N>> instance_matches(
    Machine& machine,
    ObjectRef object,
    const std::array<std::string_view, N>& targets) {
    auto source = machine.heap().class_name(object);
    if (!source) return std::unexpected(source.error());
    std::array<bool, N> result {};
    for (usize index = 0U; index < N; ++index) {
        auto assignable = machine.classes().is_assignable(*source, targets[index]);
        if (!assignable) return std::unexpected(assignable.error());
        result[index] = *assignable;
    }
    return result;
}

[[nodiscard]] Status initialize_displayable(Machine& machine,
                                            ObjectRef object,
                                            i32 type,
                                            ObjectRef title) {
    auto id = ensure_native_id(machine, object, kDisplayableIdField);
    if (!id) return std::unexpected(id.error());
    if (title.is_null()) {
        auto empty = empty_string(machine);
        if (!empty) return std::unexpected(empty.error());
        title = *empty;
    }
    constexpr std::array<usize, 7> fields {
        kDisplayableTypeField,
        kDisplayableTitleField,
        kDisplayableListenerField,
        kDisplayableCommandCountField,
        kDisplayableShownField,
        kDisplayableTickerField,
        kDisplayableScrollField,
    };
    const std::array<Value, 7> values {
        Value::from_int(type),
        Value::from_reference(title),
        Value::from_reference({}),
        Value::from_int(0),
        Value::from_int(0),
        Value::from_reference({}),
        Value::from_int(0),
    };
    auto initialized = set_field_values(machine, object, fields, values);
    if (!initialized) return initialized;
    auto commands = ensure_reference_array(
        machine, object, kDisplayableCommandsField,
        "[Ljavax/microedition/lcdui/Command;", 4U);
    if (!commands) return std::unexpected(commands.error());
    return {};
}

[[nodiscard]] Status initialize_item(Machine& machine,
                                     ObjectRef item,
                                     i32 type,
                                     ObjectRef label) {
    auto id = ensure_native_id(machine, item, kItemIdField);
    if (!id) return std::unexpected(id.error());
    if (label.is_null()) {
        auto empty = empty_string(machine);
        if (!empty) return std::unexpected(empty.error());
        label = *empty;
    }
    constexpr std::array<usize, 9> fields {
        kItemTypeField,
        kItemLabelField,
        kItemParentField,
        kItemLayoutField,
        kItemListenerField,
        kItemCommandCountField,
        kItemDefaultCommandField,
        kItemPreferredWidthField,
        kItemPreferredHeightField,
    };
    const std::array<Value, 9> values {
        Value::from_int(type),
        Value::from_reference(label),
        Value::from_int(0),
        Value::from_int(0),
        Value::from_reference({}),
        Value::from_int(0),
        Value::from_reference({}),
        Value::from_int(-1),
        Value::from_int(-1),
    };
    auto initialized = set_field_values(machine, item, fields, values);
    if (!initialized) return initialized;
    auto commands = ensure_reference_array(
        machine, item, kItemCommandsField,
        "[Ljavax/microedition/lcdui/Command;", 4U);
    if (!commands) return std::unexpected(commands.error());
    return {};
}

[[nodiscard]] Result<UiBridgeEvent> screen_event(Machine& machine,
                                                 ObjectRef displayable,
                                                 i32 kind) {
    auto id = ensure_native_id(machine, displayable, kDisplayableIdField);
    if (!id) return std::unexpected(id.error());
    constexpr std::array<usize, 2> screen_fields {
        kDisplayableTypeField, kDisplayableTitleField,
    };
    auto screen_state = field_values(machine, displayable, screen_fields);
    if (!screen_state) return std::unexpected(screen_state.error());
    auto type = (*screen_state)[0U].as_int();
    auto title = (*screen_state)[1U].as_reference();
    if (!type) return std::unexpected(type.error());
    if (!title) return std::unexpected(title.error());
    auto title_text = utf8_string(machine, *title);
    if (!title_text) return std::unexpected(title_text.error());
    UiBridgeEvent event {
        .kind = kind,
        .component_id = *id,
        .component_type = *type,
        .text = std::move(*title_text),
    };
    if (*type >= kTypeNullAlert && *type <= kTypeConfirmationAlert) {
        constexpr std::array<usize, 5> alert_fields {
            kAlertTextField,
            kAlertTimeoutField,
            kAlertNextField,
            kAlertImageField,
            kAlertImageGenerationField,
        };
        auto alert_state = field_values(machine, displayable, alert_fields);
        if (!alert_state) return std::unexpected(alert_state.error());
        auto alert_text = (*alert_state)[0U].as_reference();
        auto timeout = (*alert_state)[1U].as_int();
        auto next = (*alert_state)[2U].as_reference();
        auto image = (*alert_state)[3U].as_reference();
        auto image_generation = (*alert_state)[4U].as_int();
        if (!alert_text) return std::unexpected(alert_text.error());
        if (!timeout) return std::unexpected(timeout.error());
        if (!next) return std::unexpected(next.error());
        if (!image) return std::unexpected(image.error());
        if (!image_generation)
            return std::unexpected(image_generation.error());
        auto encoded = utf8_string(machine, *alert_text);
        if (!encoded) return std::unexpected(encoded.error());
        i32 next_id = 0;
        if (!next->is_null()) {
            auto resolved = ensure_native_id(machine, *next,
                                             kDisplayableIdField);
            if (!resolved) return std::unexpected(resolved.error());
            next_id = *resolved;
        }
        i32 image_width = 0;
        i32 image_height = 0;
        if (!image->is_null()) {
            constexpr std::array<usize, 2> image_fields {
                kImageWidthField, kImageHeightField,
            };
            auto image_state = field_values(machine, *image, image_fields);
            if (!image_state) return std::unexpected(image_state.error());
            auto width = (*image_state)[0U].as_int();
            auto height = (*image_state)[1U].as_int();
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            image_width = std::max(*width, 0);
            image_height = std::max(*height, 0);
        }
        event.index = *image_generation;
        event.arguments = {*timeout, next_id, kScreenKindAlert,
                           kAlertMetadata};
        event.value64 =
            (static_cast<i64>(static_cast<u32>(image_width)) << 32U) |
            static_cast<i64>(static_cast<u32>(image_height));
        event.detail = std::move(*encoded);
    } else {
        auto ticker = reference_field(machine, displayable,
                                      kDisplayableTickerField);
        if (!ticker) return std::unexpected(ticker.error());
        if (!ticker->is_null()) {
            auto ticker_text = reference_field(machine, *ticker,
                                               kTickerTextField);
            if (!ticker_text) return std::unexpected(ticker_text.error());
            auto encoded = utf8_string(machine, *ticker_text);
            if (!encoded) return std::unexpected(encoded.error());
            event.arguments[3] = kTickerMetadata;
            event.detail = std::move(*encoded);
        }
    }
    return event;
}

[[nodiscard]] Result<UiBridgeEvent> item_event(Machine& machine,
                                               ObjectRef item,
                                               i32 kind) {
    auto id = ensure_native_id(machine, item, kItemIdField);
    if (!id) return std::unexpected(id.error());
    constexpr std::array<usize, 3> item_fields {
        kItemTypeField, kItemLabelField, kItemParentField,
    };
    auto item_state = field_values(machine, item, item_fields);
    if (!item_state) return std::unexpected(item_state.error());
    auto type = (*item_state)[0U].as_int();
    auto label = (*item_state)[1U].as_reference();
    auto parent = (*item_state)[2U].as_int();
    if (!type) return std::unexpected(type.error());
    if (!label) return std::unexpected(label.error());
    if (!parent) return std::unexpected(parent.error());
    auto label_text = utf8_string(machine, *label);
    if (!label_text) return std::unexpected(label_text.error());

    UiBridgeEvent event {
        .kind = kind,
        .component_id = *id,
        .parent_id = *parent,
        .component_type = *type,
        .index = -1,
        .text = std::move(*label_text),
    };
    auto is_custom = machine.object_is_instance(
        item, "javax/microedition/lcdui/CustomItem");
    if (!is_custom) return std::unexpected(is_custom.error());
    if (*is_custom) {
        constexpr std::array<usize, 3> custom_fields {
            kItemPreferredWidthField,
            kItemPreferredHeightField,
            kCustomItemPaintGenerationField,
        };
        auto custom_state = field_values(machine, item, custom_fields);
        if (!custom_state) return std::unexpected(custom_state.error());
        auto width = (*custom_state)[0U].as_int();
        auto height = (*custom_state)[1U].as_int();
        auto generation = (*custom_state)[2U].as_int();
        if (!width) return std::unexpected(width.error());
        if (!height) return std::unexpected(height.error());
        if (!generation) return std::unexpected(generation.error());
        event.arguments = {std::max(*width, 0), std::max(*height, 0),
                           *generation, kImageMetadata};
    } else if (*type >= kTypePlainString && *type <= kTypeButtonString) {
        auto text = reference_field(machine, item, kStringItemTextField);
        if (!text) return std::unexpected(text.error());
        auto encoded = utf8_string(machine, *text);
        if (!encoded) return std::unexpected(encoded.error());
        event.detail = std::move(*encoded);
    } else if (*type == kTypeTextField) {
        constexpr std::array<usize, 4> text_fields {
            kTextFieldTextField,
            kTextFieldMaxSizeField,
            kTextFieldConstraintsField,
            kTextFieldCaretField,
        };
        auto text_state = field_values(machine, item, text_fields);
        if (!text_state) return std::unexpected(text_state.error());
        auto text = (*text_state)[0U].as_reference();
        auto maximum = (*text_state)[1U].as_int();
        auto constraints = (*text_state)[2U].as_int();
        auto caret = (*text_state)[3U].as_int();
        if (!text) return std::unexpected(text.error());
        if (!maximum) return std::unexpected(maximum.error());
        if (!constraints) return std::unexpected(constraints.error());
        if (!caret) return std::unexpected(caret.error());
        auto encoded = utf8_string(machine, *text);
        if (!encoded) return std::unexpected(encoded.error());
        event.arguments = {*maximum, *constraints, *caret,
                           kTextFieldMetadata};
        event.detail = std::move(*encoded);
    } else if (*type == kTypeProgressGauge ||
               *type == kTypeInteractiveGauge) {
        constexpr std::array<usize, 3> gauge_fields {
            kGaugeInteractiveField, kGaugeMaxValueField, kGaugeValueField,
        };
        auto gauge_state = field_values(machine, item, gauge_fields);
        if (!gauge_state) return std::unexpected(gauge_state.error());
        auto interactive = (*gauge_state)[0U].as_int();
        auto maximum = (*gauge_state)[1U].as_int();
        auto value = (*gauge_state)[2U].as_int();
        if (!interactive) return std::unexpected(interactive.error());
        if (!maximum) return std::unexpected(maximum.error());
        if (!value) return std::unexpected(value.error());
        event.arguments = {*value, *maximum, *interactive,
                           kGaugeMetadata};
    } else if (*type == kTypeDateField) {
        constexpr std::array<usize, 3> date_fields {
            kDateFieldDateField,
            kDateFieldInputModeField,
            kDateFieldTimeZoneField,
        };
        auto date_state = field_values(machine, item, date_fields);
        if (!date_state) return std::unexpected(date_state.error());
        auto date = (*date_state)[0U].as_reference();
        auto mode = (*date_state)[1U].as_int();
        auto zone = (*date_state)[2U].as_reference();
        if (!date) return std::unexpected(date.error());
        if (!mode) return std::unexpected(mode.error());
        if (!zone) return std::unexpected(zone.error());
        const i32 bridge_mode = *mode == 2 ? 1 : (*mode == 1 ? 2 : 3);
        event.arguments = {0, bridge_mode, 0, kDateFieldMetadata};
        if (!date->is_null()) {
            auto milliseconds = long_field(machine, *date, kDateTimeField);
            if (!milliseconds) return std::unexpected(milliseconds.error());
            event.value64 = *milliseconds / 1'000;
        }
        if (!zone->is_null()) {
            auto zone_id = reference_field(machine, *zone, kTimeZoneIdField);
            if (!zone_id) return std::unexpected(zone_id.error());
            auto encoded = utf8_string(machine, *zone_id);
            if (!encoded) return std::unexpected(encoded.error());
            event.detail = std::move(*encoded);
        }
    } else if (*type == kTypeSpacer) {
        constexpr std::array<usize, 2> spacer_fields {
            kSpacerWidthField, kSpacerHeightField,
        };
        auto spacer_state = field_values(machine, item, spacer_fields);
        if (!spacer_state) return std::unexpected(spacer_state.error());
        auto width = (*spacer_state)[0U].as_int();
        auto height = (*spacer_state)[1U].as_int();
        if (!width) return std::unexpected(width.error());
        if (!height) return std::unexpected(height.error());
        event.arguments = {0, 0, *width, *height};
    } else if (*type >= kTypePlainImage && *type <= kTypeButtonImage) {
        constexpr std::array<usize, 3> image_item_fields {
            kImageItemImageField,
            kImageItemAltTextField,
            kImageItemGenerationField,
        };
        auto image_item_state = field_values(machine, item, image_item_fields);
        if (!image_item_state) {
            return std::unexpected(image_item_state.error());
        }
        auto image = (*image_item_state)[0U].as_reference();
        auto alt = (*image_item_state)[1U].as_reference();
        auto generation = (*image_item_state)[2U].as_int();
        if (!image) return std::unexpected(image.error());
        if (!alt) return std::unexpected(alt.error());
        if (!generation) return std::unexpected(generation.error());
        auto alt_text = utf8_string(machine, *alt);
        if (!alt_text) return std::unexpected(alt_text.error());
        event.detail = std::move(*alt_text);
        i32 width = 0;
        i32 height = 0;
        if (!image->is_null()) {
            constexpr std::array<usize, 2> image_fields {
                kImageWidthField, kImageHeightField,
            };
            auto image_state = field_values(machine, *image, image_fields);
            if (!image_state) return std::unexpected(image_state.error());
            auto image_width = (*image_state)[0U].as_int();
            auto image_height = (*image_state)[1U].as_int();
            if (!image_width) return std::unexpected(image_width.error());
            if (!image_height) return std::unexpected(image_height.error());
            width = *image_width;
            height = *image_height;
        }
        event.arguments = {width, height, *generation, kImageMetadata};
    }
    return event;
}

[[nodiscard]] Result<UiBridgeEvent> item_style_event(Machine& machine,
                                                     ObjectRef item,
                                                     i32 kind) {
    auto id = ensure_native_id(machine, item, kItemIdField);
    if (!id) return std::unexpected(id.error());
    constexpr std::array<usize, 4> style_fields {
        kItemTypeField,
        kItemLabelField,
        kItemParentField,
        kItemLayoutField,
    };
    auto style_state = field_values(machine, item, style_fields);
    if (!style_state) return std::unexpected(style_state.error());
    auto type = (*style_state)[0U].as_int();
    auto label = (*style_state)[1U].as_reference();
    auto parent = (*style_state)[2U].as_int();
    auto layout = (*style_state)[3U].as_int();
    if (!type) return std::unexpected(type.error());
    if (!label) return std::unexpected(label.error());
    if (!parent) return std::unexpected(parent.error());
    if (!layout) return std::unexpected(layout.error());
    auto label_text = utf8_string(machine, *label);
    if (!label_text) return std::unexpected(label_text.error());

    i32 appearance = 0;
    i32 fit_policy = 0;
    if (*type >= kTypePlainString && *type <= kTypeButtonString) {
        auto value = int_field(machine, item, kStringItemAppearanceField);
        if (!value) return std::unexpected(value.error());
        appearance = *value;
    } else if (*type >= kTypePlainImage && *type <= kTypeButtonImage) {
        auto value = int_field(machine, item, kImageItemAppearanceField);
        if (!value) return std::unexpected(value.error());
        appearance = *value;
    } else if (*type >= kTypeExclusiveChoice &&
               *type <= kTypeMultipleChoice) {
        auto value = int_field(machine, item, kChoiceGroupFitPolicyField);
        if (!value) return std::unexpected(value.error());
        fit_policy = *value;
    }
    return UiBridgeEvent {
        .kind = kind,
        .component_id = *id,
        .parent_id = *parent,
        .component_type = *type,
        .index = -1,
        .arguments = {*layout, appearance, fit_policy,
                      kItemStyleMetadata},
        .text = std::move(*label_text),
    };
}

[[nodiscard]] Result<UiBridgeEvent> text_box_event(Machine& machine,
                                                   ObjectRef text_box,
                                                   i32 kind) {
    auto peer_id = ensure_native_id(machine, text_box, kTextBoxPeerIdField);
    auto screen_id = ensure_native_id(machine, text_box,
                                      kDisplayableIdField);
    if (!peer_id) return std::unexpected(peer_id.error());
    if (!screen_id) return std::unexpected(screen_id.error());
    constexpr std::array<usize, 4> text_box_fields {
        kTextBoxTextField,
        kTextBoxMaxSizeField,
        kTextBoxConstraintsField,
        kTextBoxCaretField,
    };
    auto text_box_state = field_values(machine, text_box, text_box_fields);
    if (!text_box_state) return std::unexpected(text_box_state.error());
    auto text = (*text_box_state)[0U].as_reference();
    auto maximum = (*text_box_state)[1U].as_int();
    auto constraints = (*text_box_state)[2U].as_int();
    auto caret = (*text_box_state)[3U].as_int();
    if (!text) return std::unexpected(text.error());
    if (!maximum) return std::unexpected(maximum.error());
    if (!constraints) return std::unexpected(constraints.error());
    if (!caret) return std::unexpected(caret.error());
    auto encoded = utf8_string(machine, *text);
    if (!encoded) return std::unexpected(encoded.error());
    return UiBridgeEvent {
        .kind = kind,
        .component_id = *peer_id,
        .parent_id = *screen_id,
        .component_type = kTypeTextField,
        .arguments = {*maximum, *constraints, *caret,
                      kTextFieldMetadata},
        .detail = std::move(*encoded),
    };
}

[[nodiscard]] Status emit_text_box(Machine& machine,
                                   ObjectRef text_box,
                                   i32 kind) {
    auto event = text_box_event(machine, text_box, kind);
    if (!event) return std::unexpected(event.error());
    machine.emit_ui_event(std::move(*event));
    return {};
}

[[nodiscard]] Status emit_item(Machine& machine,
                               ObjectRef item,
                               i32 kind,
                               i32 form_index = -1) {
    auto event = item_event(machine, item, kind);
    if (!event) return std::unexpected(event.error());
    event->index = form_index;
    machine.emit_ui_event(std::move(*event));
    auto style = item_style_event(machine, item, kind);
    if (!style) return std::unexpected(style.error());
    style->index = form_index;
    machine.emit_ui_event(std::move(*style));
    return {};
}

[[nodiscard]] i32 command_weight(i32 type) noexcept {
    constexpr std::array<i32, 9> weights {
        127, 2, 5, 7, 3, 4, 8, 6, 1,
    };
    if (type < 1 || type >= static_cast<i32>(weights.size())) return 127;
    return weights[static_cast<usize>(type)];
}

[[nodiscard]] Result<UiBridgeEvent> command_event(Machine& machine,
                                                  ObjectRef command,
                                                  i32 order) {
    auto id = ensure_native_id(machine, command, kCommandIdField);
    if (!id) return std::unexpected(id.error());
    constexpr std::array<usize, 5> command_fields {
        kCommandLabelField,
        kCommandLongLabelField,
        kCommandTypeField,
        kCommandPriorityField,
        kCommandOwnerItemField,
    };
    auto command_state = field_values(machine, command, command_fields);
    if (!command_state) return std::unexpected(command_state.error());
    auto label = (*command_state)[0U].as_reference();
    auto long_label = (*command_state)[1U].as_reference();
    auto type = (*command_state)[2U].as_int();
    auto priority = (*command_state)[3U].as_int();
    auto owner = (*command_state)[4U].as_int();
    if (!label) return std::unexpected(label.error());
    if (!long_label) return std::unexpected(long_label.error());
    if (!type) return std::unexpected(type.error());
    if (!priority) return std::unexpected(priority.error());
    if (!owner) return std::unexpected(owner.error());
    auto label_text = utf8_string(machine, *label);
    auto long_text = utf8_string(machine, *long_label);
    if (!label_text) return std::unexpected(label_text.error());
    if (!long_text) return std::unexpected(long_text.error());
    return UiBridgeEvent {
        .kind = kEventCommand,
        .component_id = *id,
        .index = order,
        .arguments = {*type, *priority, *owner == 0 ? 0 : 1, *owner},
        .text = std::move(*label_text),
        .detail = std::move(*long_text),
    };
}

[[nodiscard]] Result<bool> reference_array_contains(
    Machine& machine,
    ObjectRef array,
    i32 count,
    ObjectRef target) {
    if (array.is_null()) return false;
    for (i32 index = 0; index < count; ++index) {
        auto value = machine.heap().element(array,
                                            static_cast<usize>(index));
        if (!value) return std::unexpected(value.error());
        auto reference = value->as_reference();
        if (!reference) return std::unexpected(reference.error());
        if (*reference == target) return true;
    }
    return false;
}

[[nodiscard]] Status emit_command_array(Machine& machine,
                                        ObjectRef commands,
                                        i32 count,
                                        std::vector<ObjectRef>& output) {
    if (commands.is_null()) return {};
    auto values = machine.heap().read_reference_array(commands);
    if (!values) return std::unexpected(values.error());
    if (count < 0 || static_cast<usize>(count) > values->size()) {
        return fail(ErrorCode::invalid_state,
                    "LCDUI command array is shorter than its count");
    }
    for (i32 index = 0; index < count; ++index) {
        const ObjectRef command = (*values)[static_cast<usize>(index)];
        if (!command.is_null()) output.push_back(command);
    }
    return {};
}

[[nodiscard]] Status emit_commands(Machine& machine,
                                   ObjectRef displayable,
                                   ObjectRef focused_item = {}) {
    machine.emit_ui_event(UiBridgeEvent {.kind = kEventCommandsReset});
    constexpr std::array<usize, 2> displayable_fields {
        kDisplayableCommandsField,
        kDisplayableCommandCountField,
    };
    auto displayable_state = field_values(
        machine, displayable, displayable_fields);
    if (!displayable_state) return std::unexpected(displayable_state.error());
    auto commands = (*displayable_state)[0U].as_reference();
    auto count = (*displayable_state)[1U].as_int();
    if (!commands || !count) {
        return fail(ErrorCode::invalid_state,
                    "LCDUI Displayable command state is invalid");
    }

    std::vector<ObjectRef> ordered;
    ordered.reserve(static_cast<usize>(std::max(*count, 0)) + 4U);
    auto screen_added = emit_command_array(machine, *commands, *count, ordered);
    if (!screen_added) return screen_added;
    if (!focused_item.is_null()) {
        constexpr std::array<usize, 2> item_fields {
            kItemCommandsField,
            kItemCommandCountField,
        };
        auto item_state = field_values(machine, focused_item, item_fields);
        if (!item_state) return std::unexpected(item_state.error());
        auto item_commands = (*item_state)[0U].as_reference();
        auto item_count = (*item_state)[1U].as_int();
        if (!item_commands || !item_count) {
            return fail(ErrorCode::invalid_state,
                        "LCDUI Item command state is invalid");
        }
        auto item_added = emit_command_array(machine, *item_commands,
                                             *item_count, ordered);
        if (!item_added) return item_added;
    }
    struct CommandSortEntry final {
        ObjectRef command {};
        i32 type {0};
        i32 priority {0};
    };
    std::vector<CommandSortEntry> sortable;
    sortable.reserve(ordered.size());
    constexpr std::array<usize, 2> command_fields {
        kCommandTypeField,
        kCommandPriorityField,
    };
    for (const ObjectRef command : ordered) {
        auto state = field_values(machine, command, command_fields);
        if (!state) return std::unexpected(state.error());
        auto type = (*state)[0U].as_int();
        auto priority = (*state)[1U].as_int();
        if (!type || !priority) {
            return fail(ErrorCode::invalid_state,
                        "LCDUI Command sort state is invalid");
        }
        sortable.push_back(CommandSortEntry {
            .command = command,
            .type = *type,
            .priority = *priority,
        });
    }
    std::stable_sort(sortable.begin(), sortable.end(),
        [](const CommandSortEntry& left, const CommandSortEntry& right) {
            const i32 left_weight = command_weight(left.type);
            const i32 right_weight = command_weight(right.type);
            if (left_weight != right_weight) return left_weight < right_weight;
            if (left.priority != right.priority) {
                return left.priority < right.priority;
            }
            return left.command.bits < right.command.bits;
        });
    for (usize index = 0; index < sortable.size(); ++index) {
        auto event = command_event(machine, sortable[index].command,
                                   static_cast<i32>(index));
        if (!event) return std::unexpected(event.error());
        machine.emit_ui_event(std::move(*event));
    }
    return {};
}

[[nodiscard]] Status layout_custom_item(Machine& machine,
                                        ObjectRef item,
                                        bool notify_show);
[[nodiscard]] Status notify_custom_items(Machine& machine,
                                         ObjectRef displayable,
                                         bool visible);

[[nodiscard]] Status attach_item(Machine& machine,
                                 ObjectRef form,
                                 ObjectRef item,
                                 i32 index,
                                 bool emit_created) {
    auto form_id = ensure_native_id(machine, form, kDisplayableIdField);
    auto parent = int_field(machine, item, kItemParentField);
    if (!form_id) return std::unexpected(form_id.error());
    if (!parent) return std::unexpected(parent.error());
    if (*parent != 0 && *parent != *form_id) {
        return fail_java("java/lang/IllegalStateException",
                         "LCDUI Item already belongs to another Form");
    }
    auto parent_stored = set_int_field(machine, item, kItemParentField,
                                       *form_id);
    if (!parent_stored) return parent_stored;
    if (emit_created) {
        auto created = emit_item(machine, item, kEventItemCreated, index);
        if (!created) return created;
    }
    auto shown = emit_item(machine, item, kEventItemShown, index);
    if (!shown) return shown;
    constexpr std::array<std::string_view, 2> item_types {
        "javax/microedition/lcdui/ChoiceGroup",
        "javax/microedition/lcdui/CustomItem",
    };
    auto item_matches = instance_matches(machine, item, item_types);
    if (!item_matches) return std::unexpected(item_matches.error());
    if ((*item_matches)[0U]) {
        auto choices = emit_choice_elements(machine, item);
        if (!choices) return choices;
    }
    if ((*item_matches)[1U]) {
        auto form_shown = is_shown(machine, form);
        if (!form_shown) return std::unexpected(form_shown.error());
        auto lifecycle = layout_custom_item(machine, item, *form_shown);
        if (!lifecycle) return lifecycle;
        auto updated = emit_item(machine, item, kEventItemUpdated, index);
        if (!updated) return updated;
    }
    return {};
}

[[nodiscard]] Status validate_item(Machine& machine, ObjectRef item) {
    if (item.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "LCDUI Item is null");
    }
    auto valid = machine.object_is_instance(
        item, "javax/microedition/lcdui/Item");
    if (!valid) return std::unexpected(valid.error());
    if (!*valid) {
        return fail_java("java/lang/IllegalArgumentException",
                         "object is not an LCDUI Item");
    }
    return {};
}

[[nodiscard]] Result<i32> append_item(Machine& machine,
                                      ObjectRef form,
                                      ObjectRef item) {
    auto valid = validate_item(machine, item);
    if (!valid) return std::unexpected(valid.error());
    auto count = int_field(machine, form, kFormItemCountField);
    if (!count) return std::unexpected(count.error());
    auto items = ensure_reference_array(
        machine, form, kFormItemsField,
        "[Ljavax/microedition/lcdui/Item;",
        static_cast<usize>(*count) + 1U);
    if (!items) return std::unexpected(items.error());
    auto stored = machine.heap().set_element(
        *items, static_cast<usize>(*count), Value::from_reference(item));
    if (!stored) return std::unexpected(stored.error());
    auto count_stored = set_int_field(machine, form, kFormItemCountField,
                                      *count + 1);
    if (!count_stored) return std::unexpected(count_stored.error());
    auto attached = attach_item(machine, form, item, *count, true);
    if (!attached) return std::unexpected(attached.error());
    return *count;
}

[[nodiscard]] Result<ObjectRef> form_item(Machine& machine,
                                          ObjectRef form,
                                          i32 index) {
    auto count = int_field(machine, form, kFormItemCountField);
    if (!count) return std::unexpected(count.error());
    if (index < 0 || index >= *count) {
        return fail_java("java/lang/IndexOutOfBoundsException",
                         "Form item index is outside bounds");
    }
    auto items = reference_field(machine, form, kFormItemsField);
    if (!items) return std::unexpected(items.error());
    auto value = machine.heap().element(*items, static_cast<usize>(index));
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Status emit_form_item_order(Machine& machine,
                                          ObjectRef form,
                                          i32 start_index) {
    auto count = int_field(machine, form, kFormItemCountField);
    auto items = reference_field(machine, form, kFormItemsField);
    if (!count) return std::unexpected(count.error());
    if (!items) return std::unexpected(items.error());
    if (items->is_null()) return {};

    const i32 first = std::clamp(start_index, 0, *count);
    for (i32 index = first; index < *count; ++index) {
        auto value = machine.heap().element(
            *items, static_cast<usize>(index));
        if (!value) return std::unexpected(value.error());
        auto item = value->as_reference();
        if (!item) return std::unexpected(item.error());
        if (item->is_null()) continue;
        auto event = item_style_event(machine, *item, kEventItemUpdated);
        if (!event) return std::unexpected(event.error());
        event->index = index;
        machine.emit_ui_event(std::move(*event));
    }
    return {};
}

[[nodiscard]] Status update_item_if_attached(Machine& machine,
                                             ObjectRef item) {
    auto parent = int_field(machine, item, kItemParentField);
    if (!parent) return std::unexpected(parent.error());
    if (*parent == 0) return {};
    return emit_item(machine, item, kEventItemUpdated);
}

[[nodiscard]] Status add_item_command(Machine& machine,
                                      ObjectRef item,
                                      ObjectRef command) {
    auto count = int_field(machine, item, kItemCommandCountField);
    if (!count) return std::unexpected(count.error());
    auto commands = ensure_reference_array(
        machine, item, kItemCommandsField,
        "[Ljavax/microedition/lcdui/Command;",
        static_cast<usize>(*count) + 1U);
    if (!commands) return std::unexpected(commands.error());
    for (i32 index = 0; index < *count; ++index) {
        auto value = machine.heap().element(*commands,
                                            static_cast<usize>(index));
        if (!value) return std::unexpected(value.error());
        auto existing = value->as_reference();
        if (!existing) return std::unexpected(existing.error());
        if (*existing == command) return {};
    }
    auto item_id = ensure_native_id(machine, item, kItemIdField);
    if (!item_id) return std::unexpected(item_id.error());
    auto owner = set_int_field(machine, command,
                               kCommandOwnerItemField, *item_id);
    if (!owner) return owner;
    auto stored = machine.heap().set_element(
        *commands, static_cast<usize>(*count), Value::from_reference(command));
    if (!stored) return stored;
    return set_int_field(machine, item, kItemCommandCountField, *count + 1);
}

[[nodiscard]] Status remove_item_command(Machine& machine,
                                         ObjectRef item,
                                         ObjectRef command) {
    auto count = int_field(machine, item, kItemCommandCountField);
    auto commands = reference_field(machine, item, kItemCommandsField);
    if (!count) return std::unexpected(count.error());
    if (!commands) return std::unexpected(commands.error());
    if (commands->is_null()) return {};
    i32 found = -1;
    for (i32 index = 0; index < *count; ++index) {
        auto value = machine.heap().element(*commands,
                                            static_cast<usize>(index));
        if (!value) return std::unexpected(value.error());
        auto existing = value->as_reference();
        if (!existing) return std::unexpected(existing.error());
        if (*existing == command) {
            found = index;
            break;
        }
    }
    if (found < 0) return {};
    const usize moved_count = static_cast<usize>(*count - found - 1);
    if (moved_count != 0U) {
        auto moved = machine.heap().copy_array_range(
            *commands,
            static_cast<usize>(found + 1),
            *commands,
            static_cast<usize>(found),
            moved_count);
        if (!moved) return moved;
    }
    auto cleared = machine.heap().set_element(
        *commands, static_cast<usize>(*count - 1),
        Value::from_reference({}));
    if (!cleared) return cleared;
    auto owner = set_int_field(machine, command, kCommandOwnerItemField, 0);
    if (!owner) return owner;
    auto default_command = reference_field(machine, item,
                                           kItemDefaultCommandField);
    if (!default_command) return std::unexpected(default_command.error());
    if (*default_command == command) {
        auto cleared_default = set_reference_field(
            machine, item, kItemDefaultCommandField, {});
        if (!cleared_default) return cleared_default;
    }
    return set_int_field(machine, item, kItemCommandCountField, *count - 1);
}

[[nodiscard]] Result<ObjectRef> normalized_string(Machine& machine,
                                                  ObjectRef value) {
    if (!value.is_null()) return value;
    return empty_string(machine);
}

[[nodiscard]] Result<i32> text_length(Machine& machine,
                                      ObjectRef string) {
    auto text = machine.heap().string_value(string);
    if (!text) return std::unexpected(text.error());
    if (text->size() > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return fail(ErrorCode::overflow,
                    "LCDUI String length exceeds Java int range");
    }
    return static_cast<i32>(text->size());
}

[[nodiscard]] Status validate_text_constraints(
    std::u16string_view text,
    i32 constraints) {
    const i32 base = constraints & kConstraintMask;
    if (base < kConstraintAny || base > kConstraintDecimal) {
        return fail_java("java/lang/IllegalArgumentException",
                         "LCDUI text constraints are invalid");
    }
    if (text.empty() || base == kConstraintAny ||
        base == kConstraintEmail || base == kConstraintUrl) {
        return {};
    }
    if (base == kConstraintNumeric) {
        usize offset = 0;
        if (text.front() == u'-') {
            if (text.size() == 1U) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "NUMERIC text cannot be only a minus sign");
            }
            offset = 1U;
        }
        for (; offset < text.size(); ++offset) {
            if (text[offset] < u'0' || text[offset] > u'9') {
                return fail_java("java/lang/IllegalArgumentException",
                                 "NUMERIC text contains a non-digit");
            }
        }
        return {};
    }
    if (base == kConstraintPhone) {
        for (const char16_t character : text) {
            const bool digit = character >= u'0' && character <= u'9';
            if (!digit && character != u'#' && character != u'*' &&
                character != u'+') {
                return fail_java("java/lang/IllegalArgumentException",
                                 "PHONENUMBER text contains an invalid character");
            }
        }
        return {};
    }
    if (base == kConstraintDecimal) {
        if (text == u"-" || text == u"." || text == u"-.") {
            return fail_java("java/lang/IllegalArgumentException",
                             "DECIMAL text is incomplete");
        }
        bool separator = false;
        for (usize index = 0; index < text.size(); ++index) {
            const char16_t character = text[index];
            if (character == u'.') {
                if (separator) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "DECIMAL text has multiple separators");
                }
                separator = true;
                continue;
            }
            const bool digit = character >= u'0' && character <= u'9';
            if (!digit && !(character == u'-' && index == 0U)) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "DECIMAL text contains an invalid character");
            }
        }
    }
    return {};
}

[[nodiscard]] Status validate_text_reference(Machine& machine,
                                             ObjectRef text,
                                             i32 constraints) {
    auto value = machine.heap().string_value(text);
    if (!value) return std::unexpected(value.error());
    return validate_text_constraints(*value, constraints);
}

[[nodiscard]] Result<std::u16string> char_array_slice(
    Machine& machine,
    ObjectRef array,
    i32 offset,
    i32 length) {
    auto class_name = machine.heap().class_name(array);
    if (!class_name) return std::unexpected(class_name.error());
    if (*class_name != "[C") {
        return fail_java("java/lang/IllegalArgumentException",
                         "LCDUI character buffer is not char[]");
    }
    auto array_length = machine.heap().array_length(array);
    if (!array_length) return std::unexpected(array_length.error());
    if (offset < 0 || length < 0 ||
        static_cast<usize>(offset) > *array_length ||
        static_cast<usize>(length) > *array_length -
            static_cast<usize>(offset)) {
        return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                         "LCDUI character range is outside buffer");
    }
    std::u16string value;
    value.reserve(static_cast<usize>(length));
    for (i32 index = 0; index < length; ++index) {
        auto element = machine.heap().element(
            array, static_cast<usize>(offset + index));
        if (!element) return std::unexpected(element.error());
        auto character = element->as_int();
        if (!character) return std::unexpected(character.error());
        value.push_back(static_cast<char16_t>(
            static_cast<u32>(*character) & 0xFFFFU));
    }
    return value;
}

[[nodiscard]] Status set_text_box_value(Machine& machine,
                                        ObjectRef text_box,
                                        std::u16string value,
                                        i32 caret) {
    auto maximum = int_field(machine, text_box, kTextBoxMaxSizeField);
    auto constraints = int_field(machine, text_box,
                                 kTextBoxConstraintsField);
    if (!maximum) return std::unexpected(maximum.error());
    if (!constraints) return std::unexpected(constraints.error());
    if (value.size() > static_cast<usize>(*maximum)) {
        return fail_java("java/lang/IllegalArgumentException",
                         "TextBox text exceeds maxSize");
    }
    auto valid = validate_text_constraints(value, *constraints);
    if (!valid) return valid;
    auto string = create_string(machine, std::move(value));
    if (!string) return std::unexpected(string.error());
    auto length = text_length(machine, *string);
    if (!length) return std::unexpected(length.error());
    auto first = set_reference_field(machine, text_box,
                                     kTextBoxTextField, *string);
    auto second = set_int_field(machine, text_box, kTextBoxCaretField,
                                std::clamp(caret, 0, *length));
    if (!first) return first;
    if (!second) return second;
    return emit_text_box(machine, text_box, kEventItemUpdated);
}

[[nodiscard]] Status set_text_field_value(Machine& machine,
                                          ObjectRef text_field,
                                          std::u16string value,
                                          i32 caret) {
    auto maximum = int_field(machine, text_field, kTextFieldMaxSizeField);
    auto constraints = int_field(machine, text_field,
                                 kTextFieldConstraintsField);
    if (!maximum) return std::unexpected(maximum.error());
    if (!constraints) return std::unexpected(constraints.error());
    if (value.size() > static_cast<usize>(*maximum)) {
        return fail_java("java/lang/IllegalArgumentException",
                         "TextField text exceeds maxSize");
    }
    auto valid = validate_text_constraints(value, *constraints);
    if (!valid) return valid;
    auto string = create_string(machine, std::move(value));
    if (!string) return std::unexpected(string.error());
    auto length = text_length(machine, *string);
    if (!length) return std::unexpected(length.error());
    auto first = set_reference_field(machine, text_field,
                                     kTextFieldTextField, *string);
    auto second = set_int_field(machine, text_field, kTextFieldCaretField,
                                std::clamp(caret, 0, *length));
    if (!first) return first;
    if (!second) return second;
    return emit_item(machine, text_field, kEventItemUpdated);
}

[[nodiscard]] Result<std::u16string> decode_ui_utf8(
    std::string_view encoded) {
    std::u16string output;
    output.reserve(encoded.size());
    usize index = 0;
    while (index < encoded.size()) {
        const u8 first = static_cast<u8>(encoded[index]);
        if (first <= 0x7FU) {
            output.push_back(static_cast<char16_t>(first));
            ++index;
            continue;
        }
        u32 code_point = 0;
        usize count = 0;
        u32 minimum = 0;
        if ((first & 0xE0U) == 0xC0U) {
            code_point = first & 0x1FU;
            count = 2U;
            minimum = 0x80U;
        } else if ((first & 0xF0U) == 0xE0U) {
            code_point = first & 0x0FU;
            count = 3U;
            minimum = 0x800U;
        } else if ((first & 0xF8U) == 0xF0U) {
            code_point = first & 0x07U;
            count = 4U;
            minimum = 0x10000U;
        } else {
            return fail(ErrorCode::invalid_argument,
                        "LCDUI text contains invalid UTF-8");
        }
        if (count > encoded.size() - index) {
            return fail(ErrorCode::invalid_argument,
                        "LCDUI text contains truncated UTF-8");
        }
        for (usize offset = 1U; offset < count; ++offset) {
            const u8 next = static_cast<u8>(encoded[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                return fail(ErrorCode::invalid_argument,
                            "LCDUI text contains invalid UTF-8 continuation");
            }
            code_point = (code_point << 6U) | (next & 0x3FU);
        }
        if (code_point < minimum || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return fail(ErrorCode::invalid_argument,
                        "LCDUI text contains invalid UTF-8 scalar");
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
        index += count;
    }
    return output;
}

[[nodiscard]] Result<ObjectRef> current_displayable(Machine& machine) {
    auto singleton_field = machine.class_states().resolve_field(
        "javax/microedition/lcdui/Display", "singleton",
        "Ljavax/microedition/lcdui/Display;", true);
    if (!singleton_field) return std::unexpected(singleton_field.error());
    auto singleton = machine.class_states().static_field(*singleton_field);
    if (!singleton) return std::unexpected(singleton.error());
    auto display = singleton->as_reference();
    if (!display) return std::unexpected(display.error());
    if (display->is_null()) return ObjectRef {};
    return reference_field(machine, *display, kDisplayCurrentField);
}

[[nodiscard]] Status refresh_alert_timeout(Machine& machine,
                                           ObjectRef displayable) {
    auto is_alert = machine.object_is_instance(
        displayable, "javax/microedition/lcdui/Alert");
    if (!is_alert) return std::unexpected(is_alert.error());
    if (!*is_alert) return {};

    constexpr std::array<usize, 3> fields {
        kDisplayableShownField,
        kAlertTimeoutField,
        kDisplayableCommandCountField,
    };
    auto state = field_values(machine, displayable, fields);
    if (!state) return std::unexpected(state.error());
    auto shown = (*state)[0U].as_int();
    auto timeout = (*state)[1U].as_int();
    auto command_count = (*state)[2U].as_int();
    if (!shown || !timeout || !command_count) {
        return fail(ErrorCode::invalid_state,
                    "LCDUI Alert timeout state is invalid");
    }

    if (*shown == 0 || *timeout == kAlertForever || *command_count != 1) {
        machine.cancel_lcdui_alert_timeout(displayable);
        return {};
    }
    return machine.schedule_lcdui_alert_timeout(displayable, *timeout);
}

[[nodiscard]] Status set_alert_indicator_visible(Machine& machine,
                                                  ObjectRef displayable,
                                                  bool visible) {
    auto is_alert = machine.object_is_instance(
        displayable, "javax/microedition/lcdui/Alert");
    if (!is_alert) return std::unexpected(is_alert.error());
    if (!*is_alert) return {};
    auto indicator = reference_field(machine, displayable,
                                     kAlertIndicatorField);
    if (!indicator) return std::unexpected(indicator.error());
    if (indicator->is_null()) return {};
    return emit_item(machine, *indicator,
                     visible ? kEventItemShown : kEventItemHidden);
}

[[nodiscard]] Status set_current_displayable(Machine& machine,
                                             ObjectRef display,
                                             ObjectRef next) {
    if (!next.is_null()) {
        auto valid = machine.object_is_instance(
            next, "javax/microedition/lcdui/Displayable");
        if (!valid) return std::unexpected(valid.error());
        if (!*valid) {
            return fail_java("java/lang/IllegalArgumentException",
                             "Display.setCurrent target is not Displayable");
        }
    }
    auto current = reference_field(machine, display, kDisplayCurrentField);
    if (!current) return std::unexpected(current.error());
    if (*current == next) return {};
    if (!current->is_null()) {
        machine.cancel_lcdui_alert_timeout(*current);
        auto indicator_hidden = set_alert_indicator_visible(
            machine, *current, false);
        if (!indicator_hidden) return indicator_hidden;
        auto custom_hidden = notify_custom_items(machine, *current, false);
        if (!custom_hidden) return custom_hidden;
        auto hidden = screen_event(machine, *current, kEventScreenHidden);
        if (!hidden) return std::unexpected(hidden.error());
        machine.emit_ui_event(std::move(*hidden));
        auto stored = set_int_field(machine, *current,
                                    kDisplayableShownField, 0);
        if (!stored) return std::unexpected(stored.error());
        if (auto* canvas = machine.canvas_bridge(); canvas != nullptr) {
            auto visibility = canvas->set_display_visible(*current, false);
            if (!visibility) return std::unexpected(visibility.error());
        }
    }
    auto assigned = set_reference_field(machine, display,
                                        kDisplayCurrentField, next);
    if (!assigned) return std::unexpected(assigned.error());
    if (next.is_null()) {
        machine.emit_ui_event(UiBridgeEvent {.kind = kEventCommandsReset});
        return {};
    }
    auto shown_stored = set_int_field(machine, next,
                                      kDisplayableShownField, 1);
    if (!shown_stored) return std::unexpected(shown_stored.error());
    if (auto* canvas = machine.canvas_bridge(); canvas != nullptr) {
        auto visibility = canvas->set_display_visible(next, true);
        if (!visibility) return std::unexpected(visibility.error());
        // phoneME schedules Canvas show/hide callbacks on the LCDUI event
        // thread. Running them synchronously from setCurrent() can observe a
        // subclass before its constructor or assigning expression completes.
        // CanvasRuntime::pump() drains the queued visibility edge before input
        // and repaint work, preserving order without constructor re-entrancy.
    }
    auto custom_shown = notify_custom_items(machine, next, true);
    if (!custom_shown) return custom_shown;
    auto indicator_shown = set_alert_indicator_visible(machine, next, true);
    if (!indicator_shown) return indicator_shown;
    auto shown = screen_event(machine, next, kEventScreenShown);
    if (!shown) return std::unexpected(shown.error());
    machine.emit_ui_event(std::move(*shown));

    // A List keeps its Choice elements alive while another Displayable is on
    // top. When Back reveals the same List again, replay the element metadata
    // so the host can request the real per-row images instead of depending on
    // a stale image cache from the previous presentation.
    auto is_list = machine.object_is_instance(
        next, "javax/microedition/lcdui/List");
    if (!is_list) return std::unexpected(is_list.error());
    if (*is_list) {
        auto choices = emit_choice_elements(machine, next);
        if (!choices) return choices;
    }

    auto alert_timeout = refresh_alert_timeout(machine, next);
    if (!alert_timeout) return alert_timeout;
    return emit_commands(machine, next);
}

[[nodiscard]] Status dispatch_command_listener(Machine& machine,
                                               ObjectRef command,
                                               ObjectRef displayable) {
    auto listener = reference_field(machine, displayable,
                                    kDisplayableListenerField);
    if (!listener) return std::unexpected(listener.error());
    if (listener->is_null()) return {};

    const Value callback_arguments[] {
        Value::from_reference(command),
        Value::from_reference(displayable),
    };
    auto result = machine.invoke_instance(
        *listener,
        "javax/microedition/lcdui/CommandListener",
        "commandAction",
        "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V",
        callback_arguments);
    if (!result) return std::unexpected(result.error());
    if (result->completed_normally()) return {};
    if (!result->throwable.has_value()) {
        return fail(ErrorCode::internal_error,
                    "LCDUI command callback failed without throwable");
    }
    auto throwable = machine.heap().class_name(*result->throwable);
    if (!throwable) return std::unexpected(throwable.error());
    return fail(ErrorCode::java_exception,
                "LCDUI command callback threw " + *throwable);
}

[[nodiscard]] Status callback_status(Machine& machine,
                                     const ExecutionResult& result,
                                     std::string_view callback_name) {
    if (result.completed_normally()) return {};
    if (!result.throwable.has_value()) {
        return fail(ErrorCode::internal_error,
                    std::string(callback_name) +
                        " failed without throwable");
    }
    auto throwable = machine.heap().class_name(*result.throwable);
    if (!throwable) return std::unexpected(throwable.error());
    return fail(ErrorCode::java_exception,
                std::string(callback_name) + " threw " + *throwable);
}

[[nodiscard]] Status dispatch_item_command_listener(Machine& machine,
                                                    ObjectRef command,
                                                    ObjectRef item) {
    auto listener = reference_field(machine, item, kItemListenerField);
    if (!listener) return std::unexpected(listener.error());
    if (listener->is_null()) return {};
    const Value callback_arguments[] {
        Value::from_reference(command),
        Value::from_reference(item),
    };
    auto result = machine.invoke_instance(
        *listener,
        "javax/microedition/lcdui/ItemCommandListener",
        "commandAction",
        "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Item;)V",
        callback_arguments);
    if (!result) return std::unexpected(result.error());
    return callback_status(machine, *result, "LCDUI item command callback");
}

[[nodiscard]] Status dispatch_item_state_listener(Machine& machine,
                                                  ObjectRef item) {
    auto parent_id = int_field(machine, item, kItemParentField);
    if (!parent_id) return std::unexpected(parent_id.error());
    if (*parent_id == 0) return {};
    auto form = machine.ui_component(*parent_id);
    if (!form) return std::unexpected(form.error());
    auto valid = machine.object_is_instance(
        *form, "javax/microedition/lcdui/Form");
    if (!valid) return std::unexpected(valid.error());
    if (!*valid) return {};
    auto listener = reference_field(machine, *form,
                                    kFormItemStateListenerField);
    if (!listener) return std::unexpected(listener.error());
    if (listener->is_null()) return {};
    const Value callback_arguments[] {Value::from_reference(item)};
    auto result = machine.invoke_instance(
        *listener,
        "javax/microedition/lcdui/ItemStateListener",
        "itemStateChanged",
        "(Ljavax/microedition/lcdui/Item;)V",
        callback_arguments);
    if (!result) return std::unexpected(result.error());
    return callback_status(machine, *result, "LCDUI item state callback");
}

[[nodiscard]] Result<ObjectRef> display_singleton(Machine& machine) {
    auto field = machine.class_states().resolve_field(
        "javax/microedition/lcdui/Display", "singleton",
        "Ljavax/microedition/lcdui/Display;", true);
    if (!field) return std::unexpected(field.error());
    auto value = machine.class_states().static_field(*field);
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Result<ObjectRef> alert_dismiss_command(Machine& machine) {
    auto field = machine.class_states().resolve_field(
        "javax/microedition/lcdui/Alert", "DISMISS_COMMAND",
        "Ljavax/microedition/lcdui/Command;", true);
    if (!field) return std::unexpected(field.error());
    auto value = machine.class_states().static_field(*field);
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Result<ObjectRef> sole_alert_command(Machine& machine,
                                                   ObjectRef alert) {
    constexpr std::array<usize, 2> fields {
        kDisplayableCommandsField,
        kDisplayableCommandCountField,
    };
    auto state = field_values(machine, alert, fields);
    if (!state) return std::unexpected(state.error());
    auto commands = (*state)[0U].as_reference();
    auto count = (*state)[1U].as_int();
    if (!commands || !count) {
        return fail(ErrorCode::invalid_state,
                    "LCDUI Alert command state is invalid");
    }
    if (*count != 1 || commands->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "timed Alert does not have exactly one command");
    }
    auto value = machine.heap().element(*commands, 0U);
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Status dispatch_alert_command(Machine& machine,
                                            ObjectRef alert,
                                            ObjectRef command) {
    auto listener = reference_field(machine, alert,
                                    kDisplayableListenerField);
    if (!listener) return std::unexpected(listener.error());
    if (!listener->is_null()) {
        return dispatch_command_listener(machine, command, alert);
    }

    auto next = reference_field(machine, alert, kAlertNextField);
    if (!next) return std::unexpected(next.error());
    auto display = display_singleton(machine);
    if (!display) return std::unexpected(display.error());
    if (display->is_null()) return {};
    return set_current_displayable(machine, *display, *next);
}

[[nodiscard]] Result<std::optional<Value>> invoke_custom(
    Machine& machine,
    ObjectRef item,
    std::string_view name,
    std::string_view descriptor,
    std::span<const Value> arguments = {}) {
    auto result = machine.invoke_instance(
        item, "javax/microedition/lcdui/CustomItem",
        name, descriptor, arguments);
    if (!result) return std::unexpected(result.error());
    auto status = callback_status(machine, *result,
                                  "LCDUI CustomItem callback");
    if (!status) return std::unexpected(status.error());
    return result->return_value;
}

[[nodiscard]] Result<i32> invoke_custom_int(
    Machine& machine,
    ObjectRef item,
    std::string_view name,
    std::string_view descriptor,
    std::span<const Value> arguments = {}) {
    auto value = invoke_custom(machine, item, name, descriptor, arguments);
    if (!value) return std::unexpected(value.error());
    if (!value->has_value()) {
        return fail(ErrorCode::invalid_state,
                    "CustomItem callback returned no int value");
    }
    return (*value)->as_int();
}

[[nodiscard]] Status paint_custom_item(Machine& machine,
                                       ObjectRef item,
                                       i32 width,
                                       i32 height) {
    const Value image_arguments[] {
        Value::from_int(std::max(width, 1)),
        Value::from_int(std::max(height, 1)),
    };
    auto image_result = machine.invoke_static(
        "javax/microedition/lcdui/Image", "createImage",
        "(II)Ljavax/microedition/lcdui/Image;", image_arguments);
    if (!image_result) return std::unexpected(image_result.error());
    auto image_status = callback_status(machine, *image_result,
                                        "CustomItem image allocation");
    if (!image_status) return image_status;
    if (!image_result->return_value.has_value()) {
        return fail(ErrorCode::invalid_state,
                    "CustomItem image allocation returned no image");
    }
    auto image = image_result->return_value->as_reference();
    if (!image) return std::unexpected(image.error());
    auto graphics_result = machine.invoke_instance(
        *image, "javax/microedition/lcdui/Image", "getGraphics",
        "()Ljavax/microedition/lcdui/Graphics;");
    if (!graphics_result) return std::unexpected(graphics_result.error());
    auto graphics_status = callback_status(machine, *graphics_result,
                                           "CustomItem graphics allocation");
    if (!graphics_status) return graphics_status;
    if (!graphics_result->return_value.has_value()) {
        return fail(ErrorCode::invalid_state,
                    "CustomItem graphics allocation returned no Graphics");
    }
    auto graphics = graphics_result->return_value->as_reference();
    if (!graphics) return std::unexpected(graphics.error());
    const Value paint_arguments[] {
        Value::from_reference(*graphics),
        Value::from_int(width),
        Value::from_int(height),
    };
    auto painted = invoke_custom(
        machine, item, "paint",
        "(Ljavax/microedition/lcdui/Graphics;II)V", paint_arguments);
    if (!painted) return std::unexpected(painted.error());
    auto generation = int_field(machine, item,
                                kCustomItemPaintGenerationField);
    if (!generation) return std::unexpected(generation.error());
    const i32 next_generation = *generation >= 2'000'000'000
        ? 1 : *generation + 1;
    auto image_stored = set_reference_field(
        machine, item, kCustomItemPaintImageField, *image);
    auto generation_stored = set_int_field(
        machine, item, kCustomItemPaintGenerationField, next_generation);
    if (!image_stored) return image_stored;
    if (!generation_stored) return generation_stored;
    return {};
}

[[nodiscard]] Status layout_custom_item(Machine& machine,
                                        ObjectRef item,
                                        bool notify_show) {
    if (notify_show) {
        auto shown = invoke_custom(machine, item, "showNotify", "()V");
        if (!shown) return std::unexpected(shown.error());
    }
    auto min_width = invoke_custom_int(machine, item,
                                       "getMinContentWidth", "()I");
    auto min_height = invoke_custom_int(machine, item,
                                        "getMinContentHeight", "()I");
    if (!min_width) return std::unexpected(min_width.error());
    if (!min_height) return std::unexpected(min_height.error());
    const Value pref_width_arguments[] {Value::from_int(-1)};
    auto pref_width = invoke_custom_int(machine, item,
                                        "getPrefContentWidth", "(I)I",
                                        pref_width_arguments);
    if (!pref_width) return std::unexpected(pref_width.error());
    const i32 width = std::max({*min_width, *pref_width, 1});
    const Value pref_height_arguments[] {Value::from_int(width)};
    auto pref_height = invoke_custom_int(machine, item,
                                         "getPrefContentHeight", "(I)I",
                                         pref_height_arguments);
    if (!pref_height) return std::unexpected(pref_height.error());
    const i32 height = std::max({*min_height, *pref_height, 1});
    auto width_stored = set_int_field(machine, item,
                                      kItemPreferredWidthField, width);
    auto height_stored = set_int_field(machine, item,
                                       kItemPreferredHeightField, height);
    if (!width_stored) return width_stored;
    if (!height_stored) return height_stored;
    const Value size_arguments[] {
        Value::from_int(width), Value::from_int(height),
    };
    auto sized = invoke_custom(machine, item, "sizeChanged", "(II)V",
                               size_arguments);
    if (!sized) return std::unexpected(sized.error());
    return paint_custom_item(machine, item, width, height);
}

[[nodiscard]] Status notify_custom_items(Machine& machine,
                                         ObjectRef displayable,
                                         bool visible) {
    auto is_form = machine.object_is_instance(
        displayable, "javax/microedition/lcdui/Form");
    if (!is_form) return std::unexpected(is_form.error());
    if (!*is_form) return {};
    auto count = int_field(machine, displayable, kFormItemCountField);
    auto items = reference_field(machine, displayable, kFormItemsField);
    if (!count) return std::unexpected(count.error());
    if (!items) return std::unexpected(items.error());
    if (items->is_null()) return {};
    for (i32 index = 0; index < *count; ++index) {
        auto value = machine.heap().element(*items,
                                            static_cast<usize>(index));
        if (!value) return std::unexpected(value.error());
        auto item = value->as_reference();
        if (!item) return std::unexpected(item.error());
        if (item->is_null()) continue;
        auto is_custom = machine.object_is_instance(
            *item, "javax/microedition/lcdui/CustomItem");
        if (!is_custom) return std::unexpected(is_custom.error());
        if (!*is_custom) continue;
        if (visible) {
            auto shown = layout_custom_item(machine, *item, true);
            if (!shown) return shown;
            auto updated = emit_item(machine, *item, kEventItemUpdated);
            if (!updated) return updated;
        } else {
            auto hidden = invoke_custom(machine, *item,
                                        "hideNotify", "()V");
            if (!hidden) return std::unexpected(hidden.error());
        }
    }
    return {};
}

[[nodiscard]] Result<i32> alert_component_type(Machine& machine,
                                                ObjectRef alert_type) {
    if (alert_type.is_null()) return kTypeNullAlert;
    auto valid = machine.object_is_instance(
        alert_type, "javax/microedition/lcdui/AlertType");
    if (!valid) return std::unexpected(valid.error());
    if (!*valid) {
        return fail_java("java/lang/IllegalArgumentException",
                         "Alert type is invalid");
    }
    auto kind = int_field(machine, alert_type, kAlertTypeKindField);
    if (!kind) return std::unexpected(kind.error());
    switch (*kind) {
    case 1: return kTypeInfoAlert;
    case 2: return kTypeWarningAlert;
    case 3: return kTypeErrorAlert;
    case 4: return kTypeAlarmAlert;
    case 5: return kTypeConfirmationAlert;
    default:
        return fail(ErrorCode::invalid_state,
                    "AlertType singleton has an invalid native kind");
    }
}

[[nodiscard]] Status initialize_alert(Machine& machine,
                                      ObjectRef alert,
                                      ObjectRef title,
                                      ObjectRef text,
                                      ObjectRef image,
                                      ObjectRef type) {
    auto component_type = alert_component_type(machine, type);
    if (!component_type) return std::unexpected(component_type.error());
    auto normalized_text = normalized_string(machine, text);
    if (!normalized_text) return std::unexpected(normalized_text.error());
    if (!image.is_null()) {
        auto valid_image = machine.object_is_instance(
            image, "javax/microedition/lcdui/Image");
        if (!valid_image) return std::unexpected(valid_image.error());
        if (!*valid_image) {
            return fail_java("java/lang/IllegalArgumentException",
                             "Alert image is invalid");
        }
    }
    auto initialized = initialize_displayable(machine, alert,
                                               *component_type, title);
    if (!initialized) return initialized;
    constexpr std::array<usize, 7> alert_fields {
        kAlertTextField,
        kAlertImageField,
        kAlertTypeField,
        kAlertTimeoutField,
        kAlertNextField,
        kAlertImageGenerationField,
        kAlertIndicatorField,
    };
    const std::array<Value, 7> alert_values {
        Value::from_reference(*normalized_text),
        Value::from_reference(image),
        Value::from_reference(type),
        Value::from_int(kDefaultAlertTimeout),
        Value::from_reference({}),
        Value::from_int(1),
        Value::from_reference({}),
    };
    auto alert_initialized = set_field_values(
        machine, alert, alert_fields, alert_values);
    if (!alert_initialized) return alert_initialized;

    auto dismiss_field = machine.class_states().resolve_field(
        "javax/microedition/lcdui/Alert", "DISMISS_COMMAND",
        "Ljavax/microedition/lcdui/Command;", true);
    if (!dismiss_field) return std::unexpected(dismiss_field.error());
    auto dismiss_value = machine.class_states().static_field(*dismiss_field);
    if (!dismiss_value) return std::unexpected(dismiss_value.error());
    auto dismiss = dismiss_value->as_reference();
    if (!dismiss) return std::unexpected(dismiss.error());
    if (!dismiss->is_null()) {
        auto commands = ensure_reference_array(
            machine, alert, kDisplayableCommandsField,
            "[Ljavax/microedition/lcdui/Command;", 1U);
        if (!commands) return std::unexpected(commands.error());
        auto stored = machine.heap().set_element(
            *commands, 0U, Value::from_reference(*dismiss));
        if (!stored) return std::unexpected(stored.error());
        auto count = set_int_field(machine, alert,
                                   kDisplayableCommandCountField, 1);
        if (!count) return count;
    }
    auto created = screen_event(machine, alert, kEventScreenCreated);
    if (!created) return std::unexpected(created.error());
    machine.emit_ui_event(std::move(*created));
    return {};
}

[[nodiscard]] Status emit_screen_update(Machine& machine,
                                        ObjectRef displayable) {
    auto event = screen_event(machine, displayable, kEventScreenUpdated);
    if (!event) return std::unexpected(event.error());
    machine.emit_ui_event(std::move(*event));
    return {};
}

[[nodiscard]] Result<i32> image_component_type(i32 appearance) {
    switch (appearance) {
    case 0: return kTypePlainImage;
    case 1: return kTypeHyperlinkImage;
    case 2: return kTypeButtonImage;
    default:
        return fail_java("java/lang/IllegalArgumentException",
                         "ImageItem appearance mode is invalid");
    }
}

[[nodiscard]] Result<i32> normalized_gauge_value(bool interactive,
                                                  i32 maximum,
                                                  i32 value) {
    if (maximum == -1) {
        if (interactive || value < 0 || value > 3) {
            return fail_java("java/lang/IllegalArgumentException",
                             "invalid indefinite Gauge state");
        }
        return value;
    }
    if (maximum <= 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         "Gauge maximum must be positive");
    }
    return std::clamp(value, 0, maximum);
}

[[nodiscard]] Status initialize_command_state(Machine& machine,
                                              ObjectRef command,
                                              ObjectRef label,
                                              ObjectRef long_label,
                                              i32 type,
                                              i32 priority) {
    constexpr std::array<usize, 5> fields {
        kCommandLabelField,
        kCommandLongLabelField,
        kCommandTypeField,
        kCommandPriorityField,
        kCommandOwnerItemField,
    };
    const std::array<Value, 5> values {
        Value::from_reference(label),
        Value::from_reference(long_label),
        Value::from_int(type),
        Value::from_int(priority),
        Value::from_int(0),
    };
    return set_field_values(machine, command, fields, values);
}

} // namespace

void register_lcdui_natives(NativeMethodRegistry& registry) {
    add(registry, "javax/microedition/lcdui/Command", "<init>",
        "(Ljava/lang/String;II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto command = receiver(arguments);
            auto label = reference_argument(arguments, 1U, false);
            auto type = integer_argument(arguments, 2U);
            auto priority = integer_argument(arguments, 3U);
            if (!command) return std::unexpected(command.error());
            if (!label) return std::unexpected(label.error());
            if (!type) return std::unexpected(type.error());
            if (!priority) return std::unexpected(priority.error());
            if (*type < 1 || *type > 8) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "invalid MIDP Command type");
            }
            auto id = ensure_native_id(machine, *command, kCommandIdField);
            if (!id) return std::unexpected(id.error());
            auto initialized = initialize_command_state(
                machine, *command, *label, *label, *type, *priority);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Command", "<init>",
        "(Ljava/lang/String;Ljava/lang/String;II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto command = receiver(arguments);
            auto label = reference_argument(arguments, 1U, false);
            auto long_label = reference_argument(arguments, 2U, false);
            auto type = integer_argument(arguments, 3U);
            auto priority = integer_argument(arguments, 4U);
            if (!command) return std::unexpected(command.error());
            if (!label) return std::unexpected(label.error());
            if (!long_label) return std::unexpected(long_label.error());
            if (!type) return std::unexpected(type.error());
            if (!priority) return std::unexpected(priority.error());
            if (*type < 1 || *type > 8) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "invalid MIDP Command type");
            }
            auto id = ensure_native_id(machine, *command, kCommandIdField);
            if (!id) return std::unexpected(id.error());
            auto initialized = initialize_command_state(
                machine, *command, *label, *long_label, *type, *priority);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });

    const auto command_reference_getter = [&registry](
        const char* name,
        usize field_index) {
        add(registry, "javax/microedition/lcdui/Command", name,
            "()Ljava/lang/String;",
            [field_index](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto command = receiver(arguments);
                if (!command) return std::unexpected(command.error());
                auto value = reference_field(machine, *command, field_index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_reference(*value));
            });
    };
    command_reference_getter("getLabel", kCommandLabelField);
    command_reference_getter("getLongLabel", kCommandLongLabelField);
    const auto command_int_getter = [&registry](const char* name,
                                                usize field_index) {
        add(registry, "javax/microedition/lcdui/Command", name, "()I",
            [field_index](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto command = receiver(arguments);
                if (!command) return std::unexpected(command.error());
                auto value = int_field(machine, *command, field_index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_int(*value));
            });
    };
    command_int_getter("getCommandType", kCommandTypeField);
    command_int_getter("getPriority", kCommandPriorityField);

    add(registry, "javax/microedition/lcdui/Displayable", "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto initialized = initialize_displayable(
                machine, *object, kTypeForm, {});
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Screen", "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto initialized = initialize_displayable(
                machine, *object, kTypeForm, {});
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Displayable", "getTitle",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto title = reference_field(machine, *object,
                                         kDisplayableTitleField);
            if (!title) return std::unexpected(title.error());
            return std::optional<Value>(Value::from_reference(*title));
        });
    add(registry, "javax/microedition/lcdui/Displayable", "setTitle",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto title = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!title) return std::unexpected(title.error());
            auto normalized = normalized_string(machine, *title);
            if (!normalized) return std::unexpected(normalized.error());
            auto stored = set_reference_field(machine, *object,
                                              kDisplayableTitleField,
                                              *normalized);
            if (!stored) return std::unexpected(stored.error());
            auto shown = is_shown(machine, *object);
            if (!shown) return std::unexpected(shown.error());
            if (*shown) {
                auto event = screen_event(machine, *object,
                                          kEventScreenUpdated);
                if (!event) return std::unexpected(event.error());
                machine.emit_ui_event(std::move(*event));
            }
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Displayable", "isShown", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto shown = is_shown(machine, *object);
            if (!shown) return std::unexpected(shown.error());
            return std::optional<Value>(Value::from_int(*shown ? 1 : 0));
        });
    add(registry, "javax/microedition/lcdui/Displayable", "getTicker",
        "()Ljavax/microedition/lcdui/Ticker;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto ticker = reference_field(machine, *object,
                                          kDisplayableTickerField);
            if (!ticker) return std::unexpected(ticker.error());
            return std::optional<Value>(Value::from_reference(*ticker));
        });
    const auto add_displayable_dimension = [&registry](
        const char* name,
        bool width) {
        add(registry, "javax/microedition/lcdui/Displayable", name, "()I",
            [width](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments);
                if (!object) return std::unexpected(object.error());
                auto* bridge = machine.canvas_bridge();
                if (bridge == nullptr) {
                    return fail(ErrorCode::not_configured,
                                "Displayable dimensions require a display bridge");
                }
                const Dimensions dimensions = bridge->display_dimensions();
                if (!dimensions.valid()) {
                    return fail(ErrorCode::invalid_state,
                                "Displayable dimensions are invalid");
                }
                return std::optional<Value>(Value::from_int(
                    width ? dimensions.width : dimensions.height));
            });
    };
    add_displayable_dimension("getWidth", true);
    add_displayable_dimension("getHeight", false);
    add(registry, "javax/microedition/lcdui/Displayable", "sizeChanged",
        "(II)V",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto width = integer_argument(arguments, 1U);
            auto height = integer_argument(arguments, 2U);
            if (!object) return std::unexpected(object.error());
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Displayable", "setTicker",
        "(Ljavax/microedition/lcdui/Ticker;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto ticker = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!ticker) return std::unexpected(ticker.error());
            auto display_id = ensure_native_id(machine, *object,
                                               kDisplayableIdField);
            if (!display_id) return std::unexpected(display_id.error());
            auto previous = reference_field(machine, *object,
                                            kDisplayableTickerField);
            if (!previous) return std::unexpected(previous.error());
            if (!previous->is_null()) {
                auto owner = int_field(machine, *previous, kTickerOwnerField);
                if (!owner) return std::unexpected(owner.error());
                if (*owner == *display_id) {
                    auto cleared = set_int_field(machine, *previous,
                                                 kTickerOwnerField, 0);
                    if (!cleared) return std::unexpected(cleared.error());
                }
            }
            if (!ticker->is_null()) {
                auto valid = machine.object_is_instance(
                    *ticker, "javax/microedition/lcdui/Ticker");
                if (!valid) return std::unexpected(valid.error());
                if (!*valid) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Displayable ticker is invalid");
                }
                auto owner = set_int_field(machine, *ticker,
                                           kTickerOwnerField, *display_id);
                if (!owner) return std::unexpected(owner.error());
            }
            auto stored = set_reference_field(machine, *object,
                                              kDisplayableTickerField,
                                              *ticker);
            if (!stored) return std::unexpected(stored.error());
            auto shown = is_shown(machine, *object);
            if (!shown) return std::unexpected(shown.error());
            if (*shown) {
                auto updated = emit_screen_update(machine, *object);
                if (!updated) return std::unexpected(updated.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Displayable",
        "setCommandListener",
        "(Ljavax/microedition/lcdui/CommandListener;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto listener = reference_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!listener) return std::unexpected(listener.error());
            auto stored = set_reference_field(machine, *object,
                                              kDisplayableListenerField,
                                              *listener);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Displayable", "addCommand",
        "(Ljavax/microedition/lcdui/Command;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto displayable = receiver(arguments);
            auto command = reference_argument(arguments, 1U, false);
            if (!displayable) return std::unexpected(displayable.error());
            if (!command) return std::unexpected(command.error());
            auto count = int_field(machine, *displayable,
                                   kDisplayableCommandCountField);
            if (!count) return std::unexpected(count.error());
            auto commands = ensure_reference_array(
                machine, *displayable, kDisplayableCommandsField,
                "[Ljavax/microedition/lcdui/Command;",
                static_cast<usize>(*count) + 1U);
            if (!commands) return std::unexpected(commands.error());
            for (i32 index = 0; index < *count; ++index) {
                auto value = machine.heap().element(
                    *commands, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto existing = value->as_reference();
                if (!existing) return std::unexpected(existing.error());
                if (*existing == *command) return std::optional<Value> {};
            }
            auto stored = machine.heap().set_element(
                *commands, static_cast<usize>(*count),
                Value::from_reference(*command));
            if (!stored) return std::unexpected(stored.error());
            auto updated = set_int_field(machine, *displayable,
                                         kDisplayableCommandCountField,
                                         *count + 1);
            if (!updated) return std::unexpected(updated.error());
            auto shown = is_shown(machine, *displayable);
            if (!shown) return std::unexpected(shown.error());
            if (*shown) {
                auto emitted = emit_commands(machine, *displayable);
                if (!emitted) return std::unexpected(emitted.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Displayable", "removeCommand",
        "(Ljavax/microedition/lcdui/Command;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto displayable = receiver(arguments);
            auto command = reference_argument(arguments, 1U);
            if (!displayable) return std::unexpected(displayable.error());
            if (!command) return std::unexpected(command.error());
            if (command->is_null()) return std::optional<Value> {};
            auto commands = reference_field(machine, *displayable,
                                            kDisplayableCommandsField);
            auto count = int_field(machine, *displayable,
                                   kDisplayableCommandCountField);
            if (!commands) return std::unexpected(commands.error());
            if (!count) return std::unexpected(count.error());
            i32 found = -1;
            for (i32 index = 0; index < *count; ++index) {
                auto value = machine.heap().element(
                    *commands, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto existing = value->as_reference();
                if (!existing) return std::unexpected(existing.error());
                if (*existing == *command) {
                    found = index;
                    break;
                }
            }
            if (found < 0) return std::optional<Value> {};
            const usize moved_count = static_cast<usize>(*count - found - 1);
            if (moved_count != 0U) {
                auto moved = machine.heap().copy_array_range(
                    *commands,
                    static_cast<usize>(found + 1),
                    *commands,
                    static_cast<usize>(found),
                    moved_count);
                if (!moved) return std::unexpected(moved.error());
            }
            auto cleared = machine.heap().set_element(
                *commands, static_cast<usize>(*count - 1),
                Value::from_reference({}));
            auto updated = set_int_field(machine, *displayable,
                                         kDisplayableCommandCountField,
                                         *count - 1);
            if (!cleared) return std::unexpected(cleared.error());
            if (!updated) return std::unexpected(updated.error());
            auto shown = is_shown(machine, *displayable);
            if (!shown) return std::unexpected(shown.error());
            if (*shown) {
                auto emitted = emit_commands(machine, *displayable);
                if (!emitted) return std::unexpected(emitted.error());
            }
            return std::optional<Value> {};
        });

    add(registry, "javax/microedition/lcdui/Display", "<clinit>", "()V",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            struct DisplayConstant final {
                const char* name;
                i32 value;
            };
            constexpr std::array<DisplayConstant, 10> constants {{
                {"LIST_ELEMENT", 1},
                {"CHOICE_GROUP_ELEMENT", 2},
                {"ALERT", 3},
                {"TAB", 4},
                {"COLOR_BACKGROUND", 0},
                {"COLOR_FOREGROUND", 1},
                {"COLOR_HIGHLIGHTED_BACKGROUND", 2},
                {"COLOR_HIGHLIGHTED_FOREGROUND", 3},
                {"COLOR_BORDER", 4},
                {"COLOR_HIGHLIGHTED_BORDER", 5},
            }};
            for (const DisplayConstant& constant : constants) {
                auto field = machine.class_states().resolve_field(
                    "javax/microedition/lcdui/Display", constant.name,
                    "I", true);
                if (!field) return std::unexpected(field.error());
                auto stored = machine.class_states().set_static_field(
                    *field, Value::from_int(constant.value));
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Display", "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto display = receiver(arguments);
            if (!display) return std::unexpected(display.error());
            auto stored = set_reference_field(machine, *display,
                                              kDisplayCurrentField, {});
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Display", "getDisplay",
        "(Ljavax/microedition/midlet/MIDlet;)Ljavax/microedition/lcdui/Display;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto midlet = reference_argument(arguments, 0U, false);
            if (!midlet) return std::unexpected(midlet.error());
            auto singleton_field = machine.class_states().resolve_field(
                "javax/microedition/lcdui/Display", "singleton",
                "Ljavax/microedition/lcdui/Display;", true);
            auto owner_field = machine.class_states().resolve_field(
                "javax/microedition/lcdui/Display", "ownerMidlet",
                "Ljavax/microedition/midlet/MIDlet;", true);
            if (!singleton_field)
                return std::unexpected(singleton_field.error());
            if (!owner_field) return std::unexpected(owner_field.error());
            auto owner_stored = machine.class_states().set_static_field(
                *owner_field, Value::from_reference(*midlet));
            if (!owner_stored) return std::unexpected(owner_stored.error());
            auto singleton = machine.class_states().static_field(
                *singleton_field);
            if (!singleton) return std::unexpected(singleton.error());
            auto display = singleton->as_reference();
            if (!display) return std::unexpected(display.error());
            if (display->is_null()) {
                auto allocated = machine.class_states().allocate_instance(
                    machine.heap(), "javax/microedition/lcdui/Display");
                if (!allocated) return std::unexpected(allocated.error());
                auto current_stored = set_reference_field(
                    machine, *allocated, kDisplayCurrentField, {});
                if (!current_stored)
                    return std::unexpected(current_stored.error());
                auto singleton_stored =
                    machine.class_states().set_static_field(
                        *singleton_field, Value::from_reference(*allocated));
                if (!singleton_stored)
                    return std::unexpected(singleton_stored.error());
                display = *allocated;
            }
            return std::optional<Value>(Value::from_reference(*display));
        });
    add(registry, "javax/microedition/lcdui/Display", "getCurrent",
        "()Ljavax/microedition/lcdui/Displayable;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto display = receiver(arguments);
            if (!display) return std::unexpected(display.error());
            auto current = reference_field(machine, *display,
                                           kDisplayCurrentField);
            if (!current) return std::unexpected(current.error());
            return std::optional<Value>(Value::from_reference(*current));
        });
    add(registry, "javax/microedition/lcdui/Display", "setCurrent",
        "(Ljavax/microedition/lcdui/Displayable;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto display = receiver(arguments);
            auto next = reference_argument(arguments, 1U);
            if (!display) return std::unexpected(display.error());
            if (!next) return std::unexpected(next.error());
            // MIDP treats setCurrent(null) as a background request; it does
            // not clear or hide the current Displayable.
            if (next->is_null()) return std::optional<Value> {};

            auto next_is_alert = machine.object_is_instance(
                *next, "javax/microedition/lcdui/Alert");
            if (!next_is_alert) return std::unexpected(next_is_alert.error());
            if (*next_is_alert) {
                auto current = reference_field(machine, *display,
                                               kDisplayCurrentField);
                if (!current) return std::unexpected(current.error());
                if (*current == *next) return std::optional<Value> {};
                if (!current->is_null()) {
                    auto current_is_alert = machine.object_is_instance(
                        *current, "javax/microedition/lcdui/Alert");
                    if (!current_is_alert)
                        return std::unexpected(current_is_alert.error());
                    if (*current_is_alert) {
                        return fail_java(
                            "java/lang/IllegalArgumentException",
                            "MIDP does not allow stacking Alerts");
                    }
                }
                auto stored = set_reference_field(machine, *next,
                                                  kAlertNextField, *current);
                if (!stored) return std::unexpected(stored.error());
            }
            auto switched = set_current_displayable(machine, *display, *next);
            if (!switched) return std::unexpected(switched.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Display", "setCurrent",
        "(Ljavax/microedition/lcdui/Alert;"
        "Ljavax/microedition/lcdui/Displayable;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto display = receiver(arguments);
            auto alert = reference_argument(arguments, 1U, false);
            auto next = reference_argument(arguments, 2U, false);
            if (!display) return std::unexpected(display.error());
            if (!alert) return std::unexpected(alert.error());
            if (!next) return std::unexpected(next.error());
            auto valid_alert = machine.object_is_instance(
                *alert, "javax/microedition/lcdui/Alert");
            if (!valid_alert) return std::unexpected(valid_alert.error());
            if (!*valid_alert) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Display.setCurrent alert is invalid");
            }
            constexpr std::array<std::string_view, 2> next_types {
                "javax/microedition/lcdui/Displayable",
                "javax/microedition/lcdui/Alert",
            };
            auto next_matches = instance_matches(machine, *next, next_types);
            if (!next_matches) return std::unexpected(next_matches.error());
            if (!(*next_matches)[0U] || (*next_matches)[1U]) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Alert return screen must be a non-Alert Displayable");
            }
            auto stored = set_reference_field(machine, *alert,
                                              kAlertNextField, *next);
            if (!stored) return std::unexpected(stored.error());
            auto switched = set_current_displayable(machine, *display, *alert);
            if (!switched) return std::unexpected(switched.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Display", "setCurrentItem",
        "(Ljavax/microedition/lcdui/Item;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto display = receiver(arguments);
            auto item = reference_argument(arguments, 1U, false);
            if (!display) return std::unexpected(display.error());
            if (!item) return std::unexpected(item.error());
            auto valid_item = machine.object_is_instance(
                *item, "javax/microedition/lcdui/Item");
            if (!valid_item) return std::unexpected(valid_item.error());
            if (!*valid_item) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Display.setCurrentItem target is invalid");
            }
            auto parent_id = int_field(machine, *item, kItemParentField);
            if (!parent_id) return std::unexpected(parent_id.error());
            if (*parent_id == 0) {
                return fail_java("java/lang/IllegalStateException",
                                 "Display.setCurrentItem target has no Form");
            }
            auto form = machine.ui_component(*parent_id);
            if (!form) return std::unexpected(form.error());
            auto valid_form = machine.object_is_instance(
                *form, "javax/microedition/lcdui/Form");
            if (!valid_form) return std::unexpected(valid_form.error());
            if (!*valid_form) {
                return fail_java("java/lang/IllegalStateException",
                                 "Display.setCurrentItem parent is not a Form");
            }
            auto items = reference_field(machine, *form, kFormItemsField);
            auto count = int_field(machine, *form, kFormItemCountField);
            if (!items) return std::unexpected(items.error());
            if (!count) return std::unexpected(count.error());
            i32 item_index = -1;
            for (i32 index = 0; index < *count; ++index) {
                auto value = machine.heap().element(
                    *items, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto candidate = value->as_reference();
                if (!candidate) return std::unexpected(candidate.error());
                if (*candidate == *item) {
                    item_index = index;
                    break;
                }
            }
            if (item_index < 0) {
                return fail_java("java/lang/IllegalStateException",
                                 "Display.setCurrentItem target is detached");
            }
            auto scroll = set_int_field(machine, *form,
                                        kDisplayableScrollField, item_index);
            if (!scroll) return std::unexpected(scroll.error());
            auto switched = set_current_displayable(machine, *display, *form);
            if (!switched) return std::unexpected(switched.error());
            auto event = screen_event(machine, *form, kEventScreenUpdated);
            if (!event) return std::unexpected(event.error());
            event->arguments = {item_index, 0, 0, kScreenKindMetadata};
            machine.emit_ui_event(std::move(*event));
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Display", "getColor", "(I)I",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto display = receiver(arguments);
            auto color_type = integer_argument(arguments, 1U);
            if (!display) return std::unexpected(display.error());
            if (!color_type) return std::unexpected(color_type.error());
            constexpr std::array<i32, 6> colors {{
                0xFFFFFF, 0x000000, 0x0A84FF,
                0xFFFFFF, 0x8E8E93, 0x0A84FF,
            }};
            if (*color_type < 0 ||
                static_cast<usize>(*color_type) >= colors.size()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Display color specifier is invalid");
            }
            return std::optional<Value>(Value::from_int(
                colors[static_cast<usize>(*color_type)]));
        });
    add(registry, "javax/microedition/lcdui/Display", "getBorderStyle",
        "(Z)I",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto display = receiver(arguments);
            auto highlighted = integer_argument(arguments, 1U);
            if (!display) return std::unexpected(display.error());
            if (!highlighted) return std::unexpected(highlighted.error());
            (void)highlighted;
            return std::optional<Value>(Value::from_int(0));
        });
    add(registry, "javax/microedition/lcdui/Display", "isColor", "()Z",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto display = receiver(arguments);
            if (!display) return std::unexpected(display.error());
            return std::optional<Value>(Value::from_int(1));
        });
    add(registry, "javax/microedition/lcdui/Display", "numColors", "()I",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto display = receiver(arguments);
            if (!display) return std::unexpected(display.error());
            return std::optional<Value>(Value::from_int(16'777'216));
        });
    add(registry, "javax/microedition/lcdui/Display", "numAlphaLevels",
        "()I",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto display = receiver(arguments);
            if (!display) return std::unexpected(display.error());
            return std::optional<Value>(Value::from_int(256));
        });
    add(registry, "javax/microedition/lcdui/Display", "callSerially",
        "(Ljava/lang/Runnable;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto display = receiver(arguments);
            auto runnable = reference_argument(arguments, 1U, false);
            if (!display) return std::unexpected(display.error());
            if (!runnable) return std::unexpected(runnable.error());
            auto valid = machine.object_is_instance(*runnable,
                                                    "java/lang/Runnable");
            if (!valid) return std::unexpected(valid.error());
            if (!*valid) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Display.callSerially target is not Runnable");
            }
            auto queued = machine.enqueue_serial_callback(*runnable);
            if (!queued) return std::unexpected(queued.error());
            // Foreground callSerially work must be driven by the LCDUI queue
            // itself rather than waiting for a later Canvas/frame pump. The
            // worker gate intentionally stays closed while the host is hidden.
            auto dispatched = machine.pump_serial_callbacks();
            if (!dispatched) return std::unexpected(dispatched.error());
            return std::optional<Value> {};
        });
    const auto display_timed_capability = [&registry](const char* name) {
        add(registry, "javax/microedition/lcdui/Display", name, "(I)Z",
            [](Machine&, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto display = receiver(arguments);
                auto duration = integer_argument(arguments, 1U);
                if (!display) return std::unexpected(display.error());
                if (!duration) return std::unexpected(duration.error());
                if (*duration < 0) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Display duration must not be negative");
                }
                return std::optional<Value>(Value::from_int(0));
            });
    };
    display_timed_capability("flashBacklight");
    display_timed_capability("vibrate");
    const auto display_best_image_size = [&registry](const char* name) {
        add(registry, "javax/microedition/lcdui/Display", name, "(I)I",
            [name](Machine&, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto display = receiver(arguments);
                auto image_type = integer_argument(arguments, 1U);
                if (!display) return std::unexpected(display.error());
                if (!image_type) return std::unexpected(image_type.error());
                if (*image_type < 1 || *image_type > 4) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Display image type is invalid");
                }
                const bool alert = *image_type == 3;
                const i32 dimension = alert ? 64 : 32;
                (void)name;
                return std::optional<Value>(Value::from_int(dimension));
            });
    };
    display_best_image_size("getBestImageWidth");
    display_best_image_size("getBestImageHeight");

    add(registry, "javax/microedition/lcdui/Item", "<init>",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto label = reference_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!label) return std::unexpected(label.error());
            auto initialized = initialize_item(machine, *item,
                                               kTypePlainString, *label);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Item", "getLabel",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto label = reference_field(machine, *item, kItemLabelField);
            if (!label) return std::unexpected(label.error());
            return std::optional<Value>(Value::from_reference(*label));
        });
    add(registry, "javax/microedition/lcdui/Item", "setLabel",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto label = reference_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!label) return std::unexpected(label.error());
            auto normalized = normalized_string(machine, *label);
            if (!normalized) return std::unexpected(normalized.error());
            auto stored = set_reference_field(machine, *item,
                                              kItemLabelField, *normalized);
            if (!stored) return std::unexpected(stored.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Item", "getLayout", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto layout = int_field(machine, *item, kItemLayoutField);
            if (!layout) return std::unexpected(layout.error());
            return std::optional<Value>(Value::from_int(*layout));
        });
    add(registry, "javax/microedition/lcdui/Item", "setLayout", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto layout = integer_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!layout) return std::unexpected(layout.error());
            auto stored = set_int_field(machine, *item,
                                        kItemLayoutField, *layout);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    const auto item_command_mutation = [&registry](const char* name,
                                                   bool adding) {
        add(registry, "javax/microedition/lcdui/Item", name,
            "(Ljavax/microedition/lcdui/Command;)V",
            [adding](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto item = receiver(arguments);
                auto command = reference_argument(arguments, 1U, false);
                if (!item) return std::unexpected(item.error());
                if (!command) return std::unexpected(command.error());
                auto valid = machine.object_is_instance(
                    *command, "javax/microedition/lcdui/Command");
                if (!valid) return std::unexpected(valid.error());
                if (!*valid) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Item command is invalid");
                }
                auto changed = adding
                    ? add_item_command(machine, *item, *command)
                    : remove_item_command(machine, *item, *command);
                if (!changed) return std::unexpected(changed.error());
                return std::optional<Value> {};
            });
    };
    item_command_mutation("addCommand", true);
    item_command_mutation("removeCommand", false);
    add(registry, "javax/microedition/lcdui/Item", "setDefaultCommand",
        "(Ljavax/microedition/lcdui/Command;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto command = reference_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!command) return std::unexpected(command.error());
            if (!command->is_null()) {
                auto added = add_item_command(machine, *item, *command);
                if (!added) return std::unexpected(added.error());
            }
            auto stored = set_reference_field(machine, *item,
                                              kItemDefaultCommandField,
                                              *command);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Item", "setPreferredSize",
        "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto width = integer_argument(arguments, 1U);
            auto height = integer_argument(arguments, 2U);
            if (!item) return std::unexpected(item.error());
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            if (*width < -1 || *height < -1) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Item preferred size is invalid");
            }
            auto first = set_int_field(machine, *item,
                                       kItemPreferredWidthField, *width);
            auto second = set_int_field(machine, *item,
                                        kItemPreferredHeightField, *height);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    const auto item_size_getter = [&registry](const char* name,
                                              usize field_index) {
        add(registry, "javax/microedition/lcdui/Item", name, "()I",
            [field_index](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto item = receiver(arguments);
                if (!item) return std::unexpected(item.error());
                auto value = int_field(machine, *item, field_index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_int(
                    std::max(*value, 0)));
            });
    };
    item_size_getter("getPreferredWidth", kItemPreferredWidthField);
    item_size_getter("getPreferredHeight", kItemPreferredHeightField);
    item_size_getter("getMinimumWidth", kItemPreferredWidthField);
    item_size_getter("getMinimumHeight", kItemPreferredHeightField);
    add(registry, "javax/microedition/lcdui/Item", "notifyStateChanged",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto callback = dispatch_item_state_listener(machine, *item);
            if (!callback) return std::unexpected(callback.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Item",
        "setItemCommandListener",
        "(Ljavax/microedition/lcdui/ItemCommandListener;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto listener = reference_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!listener) return std::unexpected(listener.error());
            auto stored = set_reference_field(machine, *item,
                                              kItemListenerField, *listener);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    const auto form_constructor = [&registry](const char* descriptor,
                                               bool has_items) {
        add(registry, "javax/microedition/lcdui/Form", "<init>", descriptor,
            [has_items](Machine& machine,
                        std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto form = receiver(arguments);
                auto title = reference_argument(arguments, 1U);
                if (!form) return std::unexpected(form.error());
                if (!title) return std::unexpected(title.error());
                auto initialized = initialize_displayable(
                    machine, *form, kTypeForm, *title);
                if (!initialized) return std::unexpected(initialized.error());
                auto count_stored = set_int_field(machine, *form,
                                                  kFormItemCountField, 0);
                auto listener_stored = set_reference_field(
                    machine, *form, kFormItemStateListenerField, {});
                if (!count_stored)
                    return std::unexpected(count_stored.error());
                if (!listener_stored)
                    return std::unexpected(listener_stored.error());
                auto items = ensure_reference_array(
                    machine, *form, kFormItemsField,
                    "[Ljavax/microedition/lcdui/Item;", 4U);
                if (!items) return std::unexpected(items.error());
                auto created = screen_event(machine, *form,
                                            kEventScreenCreated);
                if (!created) return std::unexpected(created.error());
                machine.emit_ui_event(std::move(*created));
                if (has_items) {
                    auto source = reference_argument(arguments, 2U, false);
                    if (!source) return std::unexpected(source.error());
                    auto length = machine.heap().array_length(*source);
                    if (!length) return std::unexpected(length.error());
                    if (*length > static_cast<usize>(
                            std::numeric_limits<i32>::max())) {
                        return fail_java("java/lang/OutOfMemoryError",
                                         "Form item count exceeds MIDP limits");
                    }

                    // Validate the complete source before publishing it into
                    // the new Form. Attachment/lifecycle callbacks still run
                    // in source order below, matching repeated append().
                    for (usize index = 0; index < *length; ++index) {
                        auto value = machine.heap().element(*source, index);
                        if (!value) return std::unexpected(value.error());
                        auto item = value->as_reference();
                        if (!item) return std::unexpected(item.error());
                        auto valid = validate_item(machine, *item);
                        if (!valid) return std::unexpected(valid.error());
                    }

                    if (*length != 0U) {
                        auto destination = ensure_reference_array(
                            machine, *form, kFormItemsField,
                            "[Ljavax/microedition/lcdui/Item;", *length);
                        if (!destination) {
                            return std::unexpected(destination.error());
                        }
                        auto copied = machine.heap().copy_array_range(
                            *source, 0U, *destination, 0U, *length);
                        if (!copied) return std::unexpected(copied.error());
                        auto count_updated = set_int_field(
                            machine, *form, kFormItemCountField,
                            static_cast<i32>(*length));
                        if (!count_updated) {
                            return std::unexpected(count_updated.error());
                        }

                        for (usize index = 0; index < *length; ++index) {
                            auto value = machine.heap().element(*source, index);
                            if (!value) return std::unexpected(value.error());
                            auto item = value->as_reference();
                            if (!item) return std::unexpected(item.error());
                            auto attached = attach_item(
                                machine, *form, *item,
                                static_cast<i32>(index), true);
                            if (!attached) {
                                return std::unexpected(attached.error());
                            }
                        }
                    }
                }
                return std::optional<Value> {};
            });
    };
    form_constructor("(Ljava/lang/String;)V", false);
    form_constructor("(Ljava/lang/String;[Ljavax/microedition/lcdui/Item;)V",
                     true);
    add(registry, "javax/microedition/lcdui/Form", "append",
        "(Ljavax/microedition/lcdui/Item;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto form = receiver(arguments);
            auto item = reference_argument(arguments, 1U, false);
            if (!form) return std::unexpected(form.error());
            if (!item) return std::unexpected(item.error());
            auto index = append_item(machine, *form, *item);
            if (!index) return std::unexpected(index.error());
            return std::optional<Value>(Value::from_int(*index));
        });
    add(registry, "javax/microedition/lcdui/Form", "append",
        "(Ljava/lang/String;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto form = receiver(arguments);
            auto text = reference_argument(arguments, 1U, false);
            if (!form) return std::unexpected(form.error());
            if (!text) return std::unexpected(text.error());
            auto item = machine.class_states().allocate_instance(
                machine.heap(), "javax/microedition/lcdui/StringItem");
            if (!item) return std::unexpected(item.error());
            auto empty = empty_string(machine);
            if (!empty) return std::unexpected(empty.error());
            auto initialized = initialize_item(machine, *item,
                                               kTypePlainString, *empty);
            if (!initialized) return std::unexpected(initialized.error());
            auto text_stored = set_reference_field(machine, *item,
                                                   kStringItemTextField,
                                                   *text);
            auto appearance_stored = set_int_field(machine, *item,
                                                   kStringItemAppearanceField,
                                                   0);
            if (!text_stored) return std::unexpected(text_stored.error());
            if (!appearance_stored)
                return std::unexpected(appearance_stored.error());
            auto index = append_item(machine, *form, *item);
            if (!index) return std::unexpected(index.error());
            return std::optional<Value>(Value::from_int(*index));
        });
    add(registry, "javax/microedition/lcdui/Form", "append",
        "(Ljavax/microedition/lcdui/Image;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto form = receiver(arguments);
            auto image = reference_argument(arguments, 1U, false);
            if (!form) return std::unexpected(form.error());
            if (!image) return std::unexpected(image.error());
            auto valid = machine.object_is_instance(
                *image, "javax/microedition/lcdui/Image");
            if (!valid) return std::unexpected(valid.error());
            if (!*valid) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Form image is invalid");
            }
            auto item = machine.class_states().allocate_instance(
                machine.heap(), "javax/microedition/lcdui/ImageItem");
            if (!item) return std::unexpected(item.error());
            auto empty = empty_string(machine);
            if (!empty) return std::unexpected(empty.error());
            auto initialized = initialize_item(machine, *item,
                                               kTypePlainImage, *empty);
            if (!initialized) return std::unexpected(initialized.error());
            auto image_stored = set_reference_field(
                machine, *item, kImageItemImageField, *image);
            auto alt_stored = set_reference_field(
                machine, *item, kImageItemAltTextField, *empty);
            auto appearance_stored = set_int_field(
                machine, *item, kImageItemAppearanceField, 0);
            auto generation_stored = set_int_field(
                machine, *item, kImageItemGenerationField, 1);
            if (!image_stored) return std::unexpected(image_stored.error());
            if (!alt_stored) return std::unexpected(alt_stored.error());
            if (!appearance_stored)
                return std::unexpected(appearance_stored.error());
            if (!generation_stored)
                return std::unexpected(generation_stored.error());
            auto index = append_item(machine, *form, *item);
            if (!index) return std::unexpected(index.error());
            return std::optional<Value>(Value::from_int(*index));
        });
    add(registry, "javax/microedition/lcdui/Form", "setItemStateListener",
        "(Ljavax/microedition/lcdui/ItemStateListener;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto form = receiver(arguments);
            auto listener = reference_argument(arguments, 1U);
            if (!form) return std::unexpected(form.error());
            if (!listener) return std::unexpected(listener.error());
            auto stored = set_reference_field(machine, *form,
                                              kFormItemStateListenerField,
                                              *listener);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Form", "size", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto form = receiver(arguments);
            if (!form) return std::unexpected(form.error());
            auto count = int_field(machine, *form, kFormItemCountField);
            if (!count) return std::unexpected(count.error());
            return std::optional<Value>(Value::from_int(*count));
        });
    add(registry, "javax/microedition/lcdui/Form", "get",
        "(I)Ljavax/microedition/lcdui/Item;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto form = receiver(arguments);
            auto index = integer_argument(arguments, 1U);
            if (!form) return std::unexpected(form.error());
            if (!index) return std::unexpected(index.error());
            auto item = form_item(machine, *form, *index);
            if (!item) return std::unexpected(item.error());
            return std::optional<Value>(Value::from_reference(*item));
        });
    add(registry, "javax/microedition/lcdui/Form", "insert",
        "(ILjavax/microedition/lcdui/Item;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto form = receiver(arguments);
            auto index = integer_argument(arguments, 1U);
            auto item = reference_argument(arguments, 2U, false);
            if (!form) return std::unexpected(form.error());
            if (!index) return std::unexpected(index.error());
            if (!item) return std::unexpected(item.error());
            auto valid = validate_item(machine, *item);
            if (!valid) return std::unexpected(valid.error());
            auto count = int_field(machine, *form, kFormItemCountField);
            if (!count) return std::unexpected(count.error());
            if (*index < 0 || *index > *count) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "Form insertion index is outside bounds");
            }
            auto items = ensure_reference_array(
                machine, *form, kFormItemsField,
                "[Ljavax/microedition/lcdui/Item;",
                static_cast<usize>(*count) + 1U);
            if (!items) return std::unexpected(items.error());
            const usize moved_count = static_cast<usize>(*count - *index);
            if (moved_count != 0U) {
                auto moved = machine.heap().copy_array_range(
                    *items,
                    static_cast<usize>(*index),
                    *items,
                    static_cast<usize>(*index + 1),
                    moved_count);
                if (!moved) return std::unexpected(moved.error());
            }
            auto stored = machine.heap().set_element(
                *items, static_cast<usize>(*index),
                Value::from_reference(*item));
            if (!stored) return std::unexpected(stored.error());
            auto count_stored = set_int_field(machine, *form,
                                              kFormItemCountField,
                                              *count + 1);
            if (!count_stored)
                return std::unexpected(count_stored.error());
            auto attached = attach_item(machine, *form, *item, *index, true);
            if (!attached) return std::unexpected(attached.error());
            auto reordered = emit_form_item_order(machine, *form, *index);
            if (!reordered) return std::unexpected(reordered.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Form", "delete", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto form = receiver(arguments);
            auto index = integer_argument(arguments, 1U);
            if (!form) return std::unexpected(form.error());
            if (!index) return std::unexpected(index.error());
            auto removed = form_item(machine, *form, *index);
            if (!removed) return std::unexpected(removed.error());
            auto items = reference_field(machine, *form, kFormItemsField);
            auto count = int_field(machine, *form, kFormItemCountField);
            if (!items) return std::unexpected(items.error());
            if (!count) return std::unexpected(count.error());
            auto removed_id = ensure_native_id(machine, *removed, kItemIdField);
            if (!removed_id) return std::unexpected(removed_id.error());
            machine.emit_ui_event(UiBridgeEvent {
                .kind = kEventItemHidden,
                .component_id = *removed_id,
            });
            machine.emit_ui_event(UiBridgeEvent {
                .kind = kEventItemDeleted,
                .component_id = *removed_id,
            });
            auto parent_cleared = set_int_field(machine, *removed,
                                                kItemParentField, 0);
            if (!parent_cleared)
                return std::unexpected(parent_cleared.error());
            machine.unregister_ui_component(*removed_id);
            const usize moved_count = static_cast<usize>(*count - *index - 1);
            if (moved_count != 0U) {
                auto moved = machine.heap().copy_array_range(
                    *items,
                    static_cast<usize>(*index + 1),
                    *items,
                    static_cast<usize>(*index),
                    moved_count);
                if (!moved) return std::unexpected(moved.error());
            }
            auto cleared = machine.heap().set_element(
                *items, static_cast<usize>(*count - 1),
                Value::from_reference({}));
            auto count_stored = set_int_field(machine, *form,
                                              kFormItemCountField,
                                              *count - 1);
            if (!cleared) return std::unexpected(cleared.error());
            if (!count_stored)
                return std::unexpected(count_stored.error());
            auto reordered = emit_form_item_order(machine, *form, *index);
            if (!reordered) return std::unexpected(reordered.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Form", "deleteAll", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto form = receiver(arguments);
            if (!form) return std::unexpected(form.error());
            auto count = int_field(machine, *form, kFormItemCountField);
            if (!count) return std::unexpected(count.error());
            for (i32 index = *count - 1; index >= 0; --index) {
                auto item = form_item(machine, *form, index);
                if (!item) return std::unexpected(item.error());
                auto id = ensure_native_id(machine, *item, kItemIdField);
                if (!id) return std::unexpected(id.error());
                machine.emit_ui_event(UiBridgeEvent {
                    .kind = kEventItemDeleted,
                    .component_id = *id,
                });
                auto cleared = set_int_field(machine, *item,
                                             kItemParentField, 0);
                if (!cleared) return std::unexpected(cleared.error());
                machine.unregister_ui_component(*id);
            }
            auto items = ensure_reference_array(
                machine, *form, kFormItemsField,
                "[Ljavax/microedition/lcdui/Item;", 4U);
            if (!items) return std::unexpected(items.error());
            auto length = machine.heap().array_length(*items);
            if (!length) return std::unexpected(length.error());
            auto cleared = machine.heap().fill_array_range(
                *items, 0U, *length, Value::from_reference({}));
            if (!cleared) return std::unexpected(cleared.error());
            auto count_stored = set_int_field(machine, *form,
                                              kFormItemCountField, 0);
            if (!count_stored)
                return std::unexpected(count_stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Form", "set",
        "(ILjavax/microedition/lcdui/Item;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto form = receiver(arguments);
            auto index = integer_argument(arguments, 1U);
            auto replacement = reference_argument(arguments, 2U, false);
            if (!form) return std::unexpected(form.error());
            if (!index) return std::unexpected(index.error());
            if (!replacement) return std::unexpected(replacement.error());
            auto old = form_item(machine, *form, *index);
            if (!old) return std::unexpected(old.error());
            auto valid = validate_item(machine, *replacement);
            if (!valid) return std::unexpected(valid.error());
            auto items = reference_field(machine, *form, kFormItemsField);
            if (!items) return std::unexpected(items.error());
            auto old_id = ensure_native_id(machine, *old, kItemIdField);
            if (!old_id) return std::unexpected(old_id.error());
            machine.emit_ui_event(UiBridgeEvent {
                .kind = kEventItemDeleted,
                .component_id = *old_id,
            });
            auto old_parent = set_int_field(machine, *old, kItemParentField, 0);
            if (!old_parent) return std::unexpected(old_parent.error());
            machine.unregister_ui_component(*old_id);
            auto stored = machine.heap().set_element(
                *items, static_cast<usize>(*index),
                Value::from_reference(*replacement));
            if (!stored) return std::unexpected(stored.error());
            auto attached = attach_item(machine, *form, *replacement,
                                        *index, true);
            if (!attached) return std::unexpected(attached.error());
            return std::optional<Value> {};
        });

    const auto string_item_constructor = [&registry](const char* descriptor,
                                                      bool has_appearance) {
        add(registry, "javax/microedition/lcdui/StringItem", "<init>",
            descriptor,
            [has_appearance](Machine& machine,
                             std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto item = receiver(arguments);
                auto label = reference_argument(arguments, 1U);
                auto text = reference_argument(arguments, 2U);
                if (!item) return std::unexpected(item.error());
                if (!label) return std::unexpected(label.error());
                if (!text) return std::unexpected(text.error());
                i32 appearance = 0;
                if (has_appearance) {
                    auto parsed = integer_argument(arguments, 3U);
                    if (!parsed) return std::unexpected(parsed.error());
                    if (*parsed < 0 || *parsed > 2) {
                        return fail_java("java/lang/IllegalArgumentException",
                                         "invalid StringItem appearance mode");
                    }
                    appearance = *parsed;
                }
                const i32 type = appearance == 1
                    ? kTypeHyperlinkString
                    : (appearance == 2 ? kTypeButtonString
                                       : kTypePlainString);
                auto initialized = initialize_item(machine, *item, type, *label);
                if (!initialized) return std::unexpected(initialized.error());
                auto normalized = normalized_string(machine, *text);
                if (!normalized) return std::unexpected(normalized.error());
                auto text_stored = set_reference_field(machine, *item,
                                                       kStringItemTextField,
                                                       *normalized);
                auto appearance_stored = set_int_field(
                    machine, *item, kStringItemAppearanceField, appearance);
                auto font_stored = set_reference_field(
                    machine, *item, kStringItemFontField, {});
                if (!text_stored) return std::unexpected(text_stored.error());
                if (!appearance_stored)
                    return std::unexpected(appearance_stored.error());
                if (!font_stored) return std::unexpected(font_stored.error());
                return std::optional<Value> {};
            });
    };
    string_item_constructor("(Ljava/lang/String;Ljava/lang/String;)V", false);
    string_item_constructor("(Ljava/lang/String;Ljava/lang/String;I)V", true);
    add(registry, "javax/microedition/lcdui/StringItem", "getText",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto text = reference_field(machine, *item, kStringItemTextField);
            if (!text) return std::unexpected(text.error());
            return std::optional<Value>(Value::from_reference(*text));
        });
    add(registry, "javax/microedition/lcdui/StringItem", "setText",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto text = reference_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!text) return std::unexpected(text.error());
            auto normalized = normalized_string(machine, *text);
            if (!normalized) return std::unexpected(normalized.error());
            auto stored = set_reference_field(machine, *item,
                                              kStringItemTextField,
                                              *normalized);
            if (!stored) return std::unexpected(stored.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/StringItem",
        "getAppearanceMode", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto appearance = int_field(machine, *item,
                                        kStringItemAppearanceField);
            if (!appearance) return std::unexpected(appearance.error());
            return std::optional<Value>(Value::from_int(*appearance));
        });

    add(registry, "javax/microedition/lcdui/StringItem", "getFont",
        "()Ljavax/microedition/lcdui/Font;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto font = reference_field(machine, *item, kStringItemFontField);
            if (!font) return std::unexpected(font.error());
            if (font->is_null()) {
                auto fallback = create_default_font(machine);
                if (!fallback) return std::unexpected(fallback.error());
                font = *fallback;
            }
            return std::optional<Value>(Value::from_reference(*font));
        });
    add(registry, "javax/microedition/lcdui/StringItem", "setFont",
        "(Ljavax/microedition/lcdui/Font;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto font = reference_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!font) return std::unexpected(font.error());
            if (!font->is_null()) {
                auto valid = machine.object_is_instance(
                    *font, "javax/microedition/lcdui/Font");
                if (!valid) return std::unexpected(valid.error());
                if (!*valid) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "StringItem font is invalid");
                }
            }
            auto stored = set_reference_field(machine, *item,
                                              kStringItemFontField, *font);
            if (!stored) return std::unexpected(stored.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });

    add(registry, "javax/microedition/lcdui/TextField", "<init>",
        "(Ljava/lang/String;Ljava/lang/String;II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto label = reference_argument(arguments, 1U);
            auto text = reference_argument(arguments, 2U);
            auto maximum = integer_argument(arguments, 3U);
            auto constraints = integer_argument(arguments, 4U);
            if (!item) return std::unexpected(item.error());
            if (!label) return std::unexpected(label.error());
            if (!text) return std::unexpected(text.error());
            if (!maximum) return std::unexpected(maximum.error());
            if (!constraints) return std::unexpected(constraints.error());
            if (*maximum <= 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TextField maxSize must be positive");
            }
            auto normalized = normalized_string(machine, *text);
            if (!normalized) return std::unexpected(normalized.error());
            auto length = text_length(machine, *normalized);
            if (!length) return std::unexpected(length.error());
            if (*length > *maximum) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TextField initial text exceeds maxSize");
            }
            auto valid_text = validate_text_reference(
                machine, *normalized, *constraints);
            if (!valid_text) return std::unexpected(valid_text.error());
            auto initialized = initialize_item(machine, *item,
                                               kTypeTextField, *label);
            if (!initialized) return std::unexpected(initialized.error());
            constexpr std::array<usize, 5> fields {
                kTextFieldTextField,
                kTextFieldMaxSizeField,
                kTextFieldConstraintsField,
                kTextFieldCaretField,
                kTextFieldInputModeField,
            };
            const std::array<Value, 5> values {
                Value::from_reference(*normalized),
                Value::from_int(*maximum),
                Value::from_int(*constraints),
                Value::from_int(*length),
                Value::from_reference({}),
            };
            auto text_initialized = set_field_values(
                machine, *item, fields, values);
            if (!text_initialized)
                return std::unexpected(text_initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/TextField", "getString",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto text = reference_field(machine, *item, kTextFieldTextField);
            if (!text) return std::unexpected(text.error());
            return std::optional<Value>(Value::from_reference(*text));
        });
    add(registry, "javax/microedition/lcdui/TextField", "setString",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto text = reference_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!text) return std::unexpected(text.error());
            auto normalized = normalized_string(machine, *text);
            if (!normalized) return std::unexpected(normalized.error());
            auto maximum = int_field(machine, *item, kTextFieldMaxSizeField);
            auto constraints = int_field(machine, *item,
                                         kTextFieldConstraintsField);
            auto length = text_length(machine, *normalized);
            if (!maximum) return std::unexpected(maximum.error());
            if (!constraints) return std::unexpected(constraints.error());
            if (!length) return std::unexpected(length.error());
            if (*length > *maximum) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TextField text exceeds maxSize");
            }
            auto valid_text = validate_text_reference(
                machine, *normalized, *constraints);
            if (!valid_text) return std::unexpected(valid_text.error());
            auto first = set_reference_field(machine, *item,
                                             kTextFieldTextField,
                                             *normalized);
            auto second = set_int_field(machine, *item,
                                        kTextFieldCaretField, *length);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/TextField", "getChars",
        "([C)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto destination = reference_argument(arguments, 1U, false);
            if (!item) return std::unexpected(item.error());
            if (!destination) return std::unexpected(destination.error());
            auto text = reference_field(machine, *item, kTextFieldTextField);
            if (!text) return std::unexpected(text.error());
            auto value = machine.heap().string_value(*text);
            if (!value) return std::unexpected(value.error());
            auto length = machine.heap().array_length(*destination);
            if (!length) return std::unexpected(length.error());
            if (*length < value->size()) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "TextField destination char[] is too small");
            }
            auto stored = machine.heap().write_char_array(
                *destination, 0U, *value);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(value->size())));
        });
    add(registry, "javax/microedition/lcdui/TextField", "setChars",
        "([CII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto data = reference_argument(arguments, 1U, false);
            auto offset = integer_argument(arguments, 2U);
            auto length = integer_argument(arguments, 3U);
            if (!item) return std::unexpected(item.error());
            if (!data) return std::unexpected(data.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            auto value = char_array_slice(machine, *data, *offset, *length);
            if (!value) return std::unexpected(value.error());
            auto stored = set_text_field_value(machine, *item,
                                               std::move(*value), *length);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/TextField", "insert",
        "(Ljava/lang/String;I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto source = reference_argument(arguments, 1U, false);
            auto position = integer_argument(arguments, 2U);
            if (!item) return std::unexpected(item.error());
            if (!source) return std::unexpected(source.error());
            if (!position) return std::unexpected(position.error());
            auto current_ref = reference_field(machine, *item,
                                               kTextFieldTextField);
            if (!current_ref) return std::unexpected(current_ref.error());
            auto current = machine.heap().string_value(*current_ref);
            auto inserted = machine.heap().string_value(*source);
            if (!current) return std::unexpected(current.error());
            if (!inserted) return std::unexpected(inserted.error());
            if (*position < 0 ||
                static_cast<usize>(*position) > current->size()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TextField insertion position is invalid");
            }
            current->insert(static_cast<usize>(*position), *inserted);
            auto stored = set_text_field_value(
                machine, *item, std::move(*current),
                *position + static_cast<i32>(inserted->size()));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/TextField", "insert",
        "([CIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto data = reference_argument(arguments, 1U, false);
            auto offset = integer_argument(arguments, 2U);
            auto length = integer_argument(arguments, 3U);
            auto position = integer_argument(arguments, 4U);
            if (!item) return std::unexpected(item.error());
            if (!data) return std::unexpected(data.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            if (!position) return std::unexpected(position.error());
            auto inserted = char_array_slice(machine, *data,
                                             *offset, *length);
            if (!inserted) return std::unexpected(inserted.error());
            auto current_ref = reference_field(machine, *item,
                                               kTextFieldTextField);
            if (!current_ref) return std::unexpected(current_ref.error());
            auto current = machine.heap().string_value(*current_ref);
            if (!current) return std::unexpected(current.error());
            if (*position < 0 ||
                static_cast<usize>(*position) > current->size()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TextField insertion position is invalid");
            }
            current->insert(static_cast<usize>(*position), *inserted);
            auto stored = set_text_field_value(
                machine, *item, std::move(*current), *position + *length);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/TextField", "delete", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto offset = integer_argument(arguments, 1U);
            auto length = integer_argument(arguments, 2U);
            if (!item) return std::unexpected(item.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            auto current_ref = reference_field(machine, *item,
                                               kTextFieldTextField);
            if (!current_ref) return std::unexpected(current_ref.error());
            auto current = machine.heap().string_value(*current_ref);
            if (!current) return std::unexpected(current.error());
            if (*offset < 0 || *length < 0 ||
                static_cast<usize>(*offset) > current->size() ||
                static_cast<usize>(*length) > current->size() -
                    static_cast<usize>(*offset)) {
                return fail_java("java/lang/StringIndexOutOfBoundsException",
                                 "TextField deletion range is invalid");
            }
            current->erase(static_cast<usize>(*offset),
                           static_cast<usize>(*length));
            auto stored = set_text_field_value(machine, *item,
                                               std::move(*current), *offset);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    const auto text_field_int_getter = [&registry](const char* name,
                                                   usize field_index) {
        add(registry, "javax/microedition/lcdui/TextField", name, "()I",
            [field_index](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto item = receiver(arguments);
                if (!item) return std::unexpected(item.error());
                auto value = int_field(machine, *item, field_index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_int(*value));
            });
    };
    text_field_int_getter("getMaxSize", kTextFieldMaxSizeField);
    text_field_int_getter("getCaretPosition", kTextFieldCaretField);
    text_field_int_getter("getConstraints", kTextFieldConstraintsField);
    add(registry, "javax/microedition/lcdui/TextField", "size", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto text = reference_field(machine, *item, kTextFieldTextField);
            if (!text) return std::unexpected(text.error());
            auto length = text_length(machine, *text);
            if (!length) return std::unexpected(length.error());
            return std::optional<Value>(Value::from_int(*length));
        });
    add(registry, "javax/microedition/lcdui/TextField", "setMaxSize",
        "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto maximum = integer_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!maximum) return std::unexpected(maximum.error());
            if (*maximum <= 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TextField maxSize must be positive");
            }
            auto text = reference_field(machine, *item, kTextFieldTextField);
            if (!text) return std::unexpected(text.error());
            auto value = machine.heap().string_value(*text);
            if (!value) return std::unexpected(value.error());
            if (value->size() > static_cast<usize>(*maximum)) {
                value->resize(static_cast<usize>(*maximum));
                auto replacement = create_string(machine, std::move(*value));
                if (!replacement) return std::unexpected(replacement.error());
                auto text_stored = set_reference_field(machine, *item,
                                                       kTextFieldTextField,
                                                       *replacement);
                if (!text_stored)
                    return std::unexpected(text_stored.error());
                auto caret_stored = set_int_field(machine, *item,
                                                  kTextFieldCaretField,
                                                  *maximum);
                if (!caret_stored)
                    return std::unexpected(caret_stored.error());
            }
            auto stored = set_int_field(machine, *item,
                                        kTextFieldMaxSizeField, *maximum);
            if (!stored) return std::unexpected(stored.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value>(Value::from_int(*maximum));
        });
    add(registry, "javax/microedition/lcdui/TextField", "setConstraints",
        "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto constraints = integer_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!constraints) return std::unexpected(constraints.error());
            auto constraint_valid = validate_text_constraints({}, *constraints);
            if (!constraint_valid)
                return std::unexpected(constraint_valid.error());
            auto text = reference_field(machine, *item, kTextFieldTextField);
            if (!text) return std::unexpected(text.error());
            auto value = machine.heap().string_value(*text);
            if (!value) return std::unexpected(value.error());
            auto content_valid = validate_text_constraints(*value, *constraints);
            if (!content_valid && !value->empty()) {
                auto empty = empty_string(machine);
                if (!empty) return std::unexpected(empty.error());
                auto cleared = set_reference_field(machine, *item,
                                                   kTextFieldTextField, *empty);
                auto caret = set_int_field(machine, *item,
                                           kTextFieldCaretField, 0);
                if (!cleared) return std::unexpected(cleared.error());
                if (!caret) return std::unexpected(caret.error());
            }
            auto stored = set_int_field(machine, *item,
                                        kTextFieldConstraintsField,
                                        *constraints);
            if (!stored) return std::unexpected(stored.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/TextField",
        "setInitialInputMode", "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto mode = reference_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!mode) return std::unexpected(mode.error());
            auto stored = set_reference_field(machine, *item,
                                              kTextFieldInputModeField,
                                              *mode);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    add(registry, "javax/microedition/lcdui/Gauge", "<init>",
        "(Ljava/lang/String;ZII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto label = reference_argument(arguments, 1U);
            auto interactive = integer_argument(arguments, 2U);
            auto maximum = integer_argument(arguments, 3U);
            auto value = integer_argument(arguments, 4U);
            if (!item) return std::unexpected(item.error());
            if (!label) return std::unexpected(label.error());
            if (!interactive) return std::unexpected(interactive.error());
            if (!maximum) return std::unexpected(maximum.error());
            if (!value) return std::unexpected(value.error());
            auto normalized = normalized_gauge_value(
                *interactive != 0, *maximum, *value);
            if (!normalized) return std::unexpected(normalized.error());
            auto initialized = initialize_item(
                machine, *item,
                *interactive != 0 ? kTypeInteractiveGauge
                                  : kTypeProgressGauge,
                *label);
            if (!initialized) return std::unexpected(initialized.error());
            constexpr std::array<usize, 3> fields {
                kGaugeInteractiveField,
                kGaugeMaxValueField,
                kGaugeValueField,
            };
            const std::array<Value, 3> values {
                Value::from_int(*interactive != 0 ? 1 : 0),
                Value::from_int(*maximum),
                Value::from_int(*normalized),
            };
            auto gauge_initialized = set_field_values(
                machine, *item, fields, values);
            if (!gauge_initialized)
                return std::unexpected(gauge_initialized.error());
            return std::optional<Value> {};
        });
    const auto gauge_getter = [&registry](const char* name,
                                          usize field_index) {
        add(registry, "javax/microedition/lcdui/Gauge", name, "()I",
            [field_index](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto item = receiver(arguments);
                if (!item) return std::unexpected(item.error());
                auto value = int_field(machine, *item, field_index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_int(*value));
            });
    };
    gauge_getter("getMaxValue", kGaugeMaxValueField);
    gauge_getter("getValue", kGaugeValueField);
    add(registry, "javax/microedition/lcdui/Gauge", "isInteractive", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto value = int_field(machine, *item, kGaugeInteractiveField);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value != 0 ? 1 : 0));
        });
    add(registry, "javax/microedition/lcdui/Gauge", "setValue", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto value = integer_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!value) return std::unexpected(value.error());
            auto interactive = int_field(machine, *item,
                                         kGaugeInteractiveField);
            auto maximum = int_field(machine, *item, kGaugeMaxValueField);
            if (!interactive) return std::unexpected(interactive.error());
            if (!maximum) return std::unexpected(maximum.error());
            auto normalized = normalized_gauge_value(
                *interactive != 0, *maximum, *value);
            if (!normalized) return std::unexpected(normalized.error());
            auto stored = set_int_field(machine, *item,
                                        kGaugeValueField, *normalized);
            if (!stored) return std::unexpected(stored.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Gauge", "setMaxValue", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto maximum = integer_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!maximum) return std::unexpected(maximum.error());
            auto interactive = int_field(machine, *item,
                                         kGaugeInteractiveField);
            auto value = int_field(machine, *item, kGaugeValueField);
            if (!interactive) return std::unexpected(interactive.error());
            if (!value) return std::unexpected(value.error());
            auto normalized = normalized_gauge_value(
                *interactive != 0, *maximum, *value);
            if (!normalized) return std::unexpected(normalized.error());
            auto first = set_int_field(machine, *item,
                                       kGaugeMaxValueField, *maximum);
            auto second = set_int_field(machine, *item,
                                        kGaugeValueField, *normalized);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });

    const auto image_item_constructor = [&registry](const char* descriptor,
                                                     bool has_appearance) {
        add(registry, "javax/microedition/lcdui/ImageItem", "<init>",
            descriptor,
            [has_appearance](Machine& machine,
                             std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto item = receiver(arguments);
                auto label = reference_argument(arguments, 1U);
                auto image = reference_argument(arguments, 2U);
                auto layout = integer_argument(arguments, 3U);
                auto alt = reference_argument(arguments, 4U);
                if (!item) return std::unexpected(item.error());
                if (!label) return std::unexpected(label.error());
                if (!image) return std::unexpected(image.error());
                if (!layout) return std::unexpected(layout.error());
                if (!alt) return std::unexpected(alt.error());
                i32 appearance = 0;
                if (has_appearance) {
                    auto parsed = integer_argument(arguments, 5U);
                    if (!parsed) return std::unexpected(parsed.error());
                    appearance = *parsed;
                }
                auto component_type = image_component_type(appearance);
                if (!component_type)
                    return std::unexpected(component_type.error());
                if (!image->is_null()) {
                    auto valid = machine.object_is_instance(
                        *image, "javax/microedition/lcdui/Image");
                    if (!valid) return std::unexpected(valid.error());
                    if (!*valid) {
                        return fail_java("java/lang/IllegalArgumentException",
                                         "ImageItem image is invalid");
                    }
                }
                auto normalized_alt = normalized_string(machine, *alt);
                if (!normalized_alt)
                    return std::unexpected(normalized_alt.error());
                auto initialized = initialize_item(machine, *item,
                                                   *component_type, *label);
                if (!initialized)
                    return std::unexpected(initialized.error());
                auto layout_stored = set_int_field(machine, *item,
                                                   kItemLayoutField, *layout);
                auto image_stored = set_reference_field(
                    machine, *item, kImageItemImageField, *image);
                auto alt_stored = set_reference_field(
                    machine, *item, kImageItemAltTextField, *normalized_alt);
                auto appearance_stored = set_int_field(
                    machine, *item, kImageItemAppearanceField, appearance);
                auto generation_stored = set_int_field(
                    machine, *item, kImageItemGenerationField, 1);
                if (!layout_stored)
                    return std::unexpected(layout_stored.error());
                if (!image_stored)
                    return std::unexpected(image_stored.error());
                if (!alt_stored) return std::unexpected(alt_stored.error());
                if (!appearance_stored)
                    return std::unexpected(appearance_stored.error());
                if (!generation_stored)
                    return std::unexpected(generation_stored.error());
                return std::optional<Value> {};
            });
    };
    image_item_constructor(
        "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;"
        "ILjava/lang/String;)V", false);
    image_item_constructor(
        "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;"
        "ILjava/lang/String;I)V", true);
    add(registry, "javax/microedition/lcdui/ImageItem", "getImage",
        "()Ljavax/microedition/lcdui/Image;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto image = reference_field(machine, *item,
                                         kImageItemImageField);
            if (!image) return std::unexpected(image.error());
            return std::optional<Value>(Value::from_reference(*image));
        });
    add(registry, "javax/microedition/lcdui/ImageItem", "setImage",
        "(Ljavax/microedition/lcdui/Image;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto image = reference_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!image) return std::unexpected(image.error());
            if (!image->is_null()) {
                auto valid = machine.object_is_instance(
                    *image, "javax/microedition/lcdui/Image");
                if (!valid) return std::unexpected(valid.error());
                if (!*valid) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "ImageItem image is invalid");
                }
            }
            auto generation = int_field(machine, *item,
                                        kImageItemGenerationField);
            if (!generation) return std::unexpected(generation.error());
            const i32 next_generation = *generation >= 2'000'000'000
                ? 1 : *generation + 1;
            auto stored = set_reference_field(machine, *item,
                                              kImageItemImageField, *image);
            auto generation_stored = set_int_field(
                machine, *item, kImageItemGenerationField, next_generation);
            if (!stored) return std::unexpected(stored.error());
            if (!generation_stored)
                return std::unexpected(generation_stored.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/ImageItem", "getAltText",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto alt = reference_field(machine, *item,
                                       kImageItemAltTextField);
            if (!alt) return std::unexpected(alt.error());
            return std::optional<Value>(Value::from_reference(*alt));
        });
    add(registry, "javax/microedition/lcdui/ImageItem", "setAltText",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto alt = reference_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!alt) return std::unexpected(alt.error());
            auto normalized = normalized_string(machine, *alt);
            if (!normalized) return std::unexpected(normalized.error());
            auto stored = set_reference_field(machine, *item,
                                              kImageItemAltTextField,
                                              *normalized);
            if (!stored) return std::unexpected(stored.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/ImageItem", "getAppearanceMode",
        "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto appearance = int_field(machine, *item,
                                        kImageItemAppearanceField);
            if (!appearance) return std::unexpected(appearance.error());
            return std::optional<Value>(Value::from_int(*appearance));
        });

    add(registry, "javax/microedition/lcdui/Ticker", "<init>",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto ticker = receiver(arguments);
            auto text = reference_argument(arguments, 1U, false);
            if (!ticker) return std::unexpected(ticker.error());
            if (!text) return std::unexpected(text.error());
            auto first = set_reference_field(machine, *ticker,
                                             kTickerTextField, *text);
            auto second = set_int_field(machine, *ticker,
                                        kTickerOwnerField, 0);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Ticker", "getString",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto ticker = receiver(arguments);
            if (!ticker) return std::unexpected(ticker.error());
            auto text = reference_field(machine, *ticker, kTickerTextField);
            if (!text) return std::unexpected(text.error());
            return std::optional<Value>(Value::from_reference(*text));
        });
    add(registry, "javax/microedition/lcdui/Ticker", "setString",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto ticker = receiver(arguments);
            auto text = reference_argument(arguments, 1U, false);
            if (!ticker) return std::unexpected(ticker.error());
            if (!text) return std::unexpected(text.error());
            auto stored = set_reference_field(machine, *ticker,
                                              kTickerTextField, *text);
            if (!stored) return std::unexpected(stored.error());
            auto owner = int_field(machine, *ticker, kTickerOwnerField);
            if (!owner) return std::unexpected(owner.error());
            if (*owner != 0) {
                auto displayable = machine.ui_component(*owner);
                if (!displayable)
                    return std::unexpected(displayable.error());
                auto shown = is_shown(machine, *displayable);
                if (!shown) return std::unexpected(shown.error());
                if (*shown) {
                    auto updated = emit_screen_update(machine, *displayable);
                    if (!updated) return std::unexpected(updated.error());
                }
            }
            return std::optional<Value> {};
        });

    add(registry, "javax/microedition/lcdui/CustomItem", "<init>",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto label = reference_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!label) return std::unexpected(label.error());
            auto initialized = initialize_item(machine, *item,
                                               kTypeCustomItem, *label);
            if (!initialized) return std::unexpected(initialized.error());
            auto image_stored = set_reference_field(
                machine, *item, kCustomItemPaintImageField, {});
            auto generation_stored = set_int_field(
                machine, *item, kCustomItemPaintGenerationField, 0);
            if (!image_stored) return std::unexpected(image_stored.error());
            if (!generation_stored)
                return std::unexpected(generation_stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/CustomItem", "getGameAction",
        "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto key = integer_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!key) return std::unexpected(key.error());
            auto* bridge = machine.canvas_bridge();
            return std::optional<Value>(Value::from_int(
                bridge == nullptr ? 0 : bridge->game_action_for_key(*key)));
        });
    add(registry, "javax/microedition/lcdui/CustomItem",
        "getInteractionModes", "()I",
        [](Machine&, std::span<const Value>)
            -> Result<std::optional<Value>> {
            return std::optional<Value>(Value::from_int(0xFF));
        });
    const auto custom_noop = [&registry](const char* name,
                                         const char* descriptor) {
        add(registry, "javax/microedition/lcdui/CustomItem", name,
            descriptor,
            [](Machine&, std::span<const Value>)
                -> Result<std::optional<Value>> {
                return std::optional<Value> {};
            });
    };
    custom_noop("sizeChanged", "(II)V");
    custom_noop("traverseOut", "()V");
    custom_noop("keyPressed", "(I)V");
    custom_noop("keyReleased", "(I)V");
    custom_noop("keyRepeated", "(I)V");
    custom_noop("pointerPressed", "(II)V");
    custom_noop("pointerReleased", "(II)V");
    custom_noop("pointerDragged", "(II)V");
    custom_noop("showNotify", "()V");
    custom_noop("hideNotify", "()V");
    add(registry, "javax/microedition/lcdui/CustomItem", "traverse",
        "(III[I)Z",
        [](Machine&, std::span<const Value>)
            -> Result<std::optional<Value>> {
            return std::optional<Value>(Value::from_int(0));
        });
    const auto custom_repaint = [&registry](const char* descriptor) {
        add(registry, "javax/microedition/lcdui/CustomItem", "repaint",
            descriptor,
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto item = receiver(arguments);
                if (!item) return std::unexpected(item.error());
                auto layout = layout_custom_item(machine, *item, false);
                if (!layout) return std::unexpected(layout.error());
                auto updated = update_item_if_attached(machine, *item);
                if (!updated) return std::unexpected(updated.error());
                return std::optional<Value> {};
            });
    };
    custom_repaint("()V");
    custom_repaint("(IIII)V");
    add(registry, "javax/microedition/lcdui/CustomItem", "invalidate",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto layout = layout_custom_item(machine, *item, false);
            if (!layout) return std::unexpected(layout.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });

    add(registry, "javax/microedition/lcdui/AlertType", "<clinit>",
        "()V",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            struct Entry final {
                const char* field_name;
                i32 kind;
            };
            constexpr std::array<Entry, 5> entries {{
                {"INFO", 1},
                {"WARNING", 2},
                {"ERROR", 3},
                {"ALARM", 4},
                {"CONFIRMATION", 5},
            }};
            for (const Entry& entry : entries) {
                auto object = machine.class_states().allocate_instance(
                    machine.heap(), "javax/microedition/lcdui/AlertType");
                if (!object) return std::unexpected(object.error());
                auto kind_stored = set_int_field(machine, *object,
                                                 kAlertTypeKindField,
                                                 entry.kind);
                if (!kind_stored)
                    return std::unexpected(kind_stored.error());
                auto field = machine.class_states().resolve_field(
                    "javax/microedition/lcdui/AlertType",
                    entry.field_name,
                    "Ljavax/microedition/lcdui/AlertType;", true);
                if (!field) return std::unexpected(field.error());
                auto stored = machine.class_states().set_static_field(
                    *field, Value::from_reference(*object));
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/AlertType", "<init>",
        "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto type = receiver(arguments);
            if (!type) return std::unexpected(type.error());
            auto stored = set_int_field(machine, *type,
                                        kAlertTypeKindField, 0);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/AlertType", "playSound",
        "(Ljavax/microedition/lcdui/Display;)Z",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto type = receiver(arguments);
            auto display = reference_argument(arguments, 1U, false);
            if (!type) return std::unexpected(type.error());
            if (!display) return std::unexpected(display.error());
            // The iOS bridge intentionally does not synthesize MIDP alert
            // beeps. Games may provide their own MMAPI sound instead.
            return std::optional<Value>(Value::from_int(0));
        });

    add(registry, "javax/microedition/lcdui/Alert", "<clinit>", "()V",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            auto command = machine.class_states().allocate_instance(
                machine.heap(), "javax/microedition/lcdui/Command");
            if (!command) return std::unexpected(command.error());
            auto label = create_string(machine, {});
            if (!label) return std::unexpected(label.error());
            auto id = ensure_native_id(machine, *command, kCommandIdField);
            if (!id) return std::unexpected(id.error());
            auto initialized = initialize_command_state(
                machine, *command, *label, *label, 4, 0);
            if (!initialized) return std::unexpected(initialized.error());
            auto field = machine.class_states().resolve_field(
                "javax/microedition/lcdui/Alert", "DISMISS_COMMAND",
                "Ljavax/microedition/lcdui/Command;", true);
            if (!field) return std::unexpected(field.error());
            auto stored = machine.class_states().set_static_field(
                *field, Value::from_reference(*command));
            if (!stored) return std::unexpected(stored.error());
            auto forever_field = machine.class_states().resolve_field(
                "javax/microedition/lcdui/Alert", "FOREVER", "I", true);
            if (!forever_field) return std::unexpected(forever_field.error());
            auto forever_stored = machine.class_states().set_static_field(
                *forever_field, Value::from_int(kAlertForever));
            if (!forever_stored)
                return std::unexpected(forever_stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Alert", "<init>",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto alert = receiver(arguments);
            auto title = reference_argument(arguments, 1U);
            if (!alert) return std::unexpected(alert.error());
            if (!title) return std::unexpected(title.error());
            auto initialized = initialize_alert(machine, *alert, *title,
                                                {}, {}, {});
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Alert", "<init>",
        "(Ljava/lang/String;Ljava/lang/String;"
        "Ljavax/microedition/lcdui/Image;"
        "Ljavax/microedition/lcdui/AlertType;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto alert = receiver(arguments);
            auto title = reference_argument(arguments, 1U);
            auto text = reference_argument(arguments, 2U);
            auto image = reference_argument(arguments, 3U);
            auto type = reference_argument(arguments, 4U);
            if (!alert) return std::unexpected(alert.error());
            if (!title) return std::unexpected(title.error());
            if (!text) return std::unexpected(text.error());
            if (!image) return std::unexpected(image.error());
            if (!type) return std::unexpected(type.error());
            auto initialized = initialize_alert(machine, *alert, *title,
                                                *text, *image, *type);
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        });
    const auto alert_reference_getter = [&registry](const char* name,
                                                     const char* descriptor,
                                                     usize field_index) {
        add(registry, "javax/microedition/lcdui/Alert", name, descriptor,
            [field_index](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto alert = receiver(arguments);
                if (!alert) return std::unexpected(alert.error());
                auto value = reference_field(machine, *alert, field_index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_reference(*value));
            });
    };
    alert_reference_getter("getString", "()Ljava/lang/String;",
                           kAlertTextField);
    alert_reference_getter("getImage",
                           "()Ljavax/microedition/lcdui/Image;",
                           kAlertImageField);
    alert_reference_getter("getType",
                           "()Ljavax/microedition/lcdui/AlertType;",
                           kAlertTypeField);
    add(registry, "javax/microedition/lcdui/Alert", "setString",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto alert = receiver(arguments);
            auto text = reference_argument(arguments, 1U);
            if (!alert) return std::unexpected(alert.error());
            if (!text) return std::unexpected(text.error());
            auto normalized = normalized_string(machine, *text);
            if (!normalized) return std::unexpected(normalized.error());
            auto stored = set_reference_field(machine, *alert,
                                              kAlertTextField, *normalized);
            if (!stored) return std::unexpected(stored.error());
            auto updated = emit_screen_update(machine, *alert);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Alert", "setImage",
        "(Ljavax/microedition/lcdui/Image;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto alert = receiver(arguments);
            auto image = reference_argument(arguments, 1U);
            if (!alert) return std::unexpected(alert.error());
            if (!image) return std::unexpected(image.error());
            if (!image->is_null()) {
                auto valid = machine.object_is_instance(
                    *image, "javax/microedition/lcdui/Image");
                if (!valid) return std::unexpected(valid.error());
                if (!*valid) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Alert image is invalid");
                }
            }
            auto generation = int_field(machine, *alert,
                                        kAlertImageGenerationField);
            if (!generation) return std::unexpected(generation.error());
            const i32 next_generation = *generation >= 2'000'000'000
                ? 1 : *generation + 1;
            auto stored = set_reference_field(machine, *alert,
                                              kAlertImageField, *image);
            auto generation_stored = set_int_field(
                machine, *alert, kAlertImageGenerationField,
                next_generation);
            if (!stored) return std::unexpected(stored.error());
            if (!generation_stored)
                return std::unexpected(generation_stored.error());
            auto updated = emit_screen_update(machine, *alert);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Alert", "setType",
        "(Ljavax/microedition/lcdui/AlertType;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto alert = receiver(arguments);
            auto type = reference_argument(arguments, 1U);
            if (!alert) return std::unexpected(alert.error());
            if (!type) return std::unexpected(type.error());
            auto component_type = alert_component_type(machine, *type);
            if (!component_type)
                return std::unexpected(component_type.error());
            auto first = set_reference_field(machine, *alert,
                                             kAlertTypeField, *type);
            auto second = set_int_field(machine, *alert,
                                        kDisplayableTypeField,
                                        *component_type);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            auto updated = emit_screen_update(machine, *alert);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Alert", "getTimeout", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto alert = receiver(arguments);
            if (!alert) return std::unexpected(alert.error());
            auto timeout = int_field(machine, *alert, kAlertTimeoutField);
            auto command_count = int_field(machine, *alert,
                                           kDisplayableCommandCountField);
            if (!timeout) return std::unexpected(timeout.error());
            if (!command_count)
                return std::unexpected(command_count.error());
            // Two or more commands make an Alert modal regardless of the
            // timeout value requested by the MIDlet.
            const i32 effective = *command_count >= 2
                ? kAlertForever : *timeout;
            return std::optional<Value>(Value::from_int(effective));
        });
    add(registry, "javax/microedition/lcdui/Alert", "setTimeout", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto alert = receiver(arguments);
            auto timeout = integer_argument(arguments, 1U);
            if (!alert) return std::unexpected(alert.error());
            if (!timeout) return std::unexpected(timeout.error());
            if (*timeout != kAlertForever && *timeout <= 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Alert timeout must be positive or FOREVER");
            }
            auto stored = set_int_field(machine, *alert,
                                        kAlertTimeoutField, *timeout);
            if (!stored) return std::unexpected(stored.error());
            auto updated = emit_screen_update(machine, *alert);
            if (!updated) return std::unexpected(updated.error());
            auto scheduled = refresh_alert_timeout(machine, *alert);
            if (!scheduled) return std::unexpected(scheduled.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Alert", "getDefaultTimeout",
        "()I",
        [](Machine&, std::span<const Value>)
            -> Result<std::optional<Value>> {
            return std::optional<Value>(
                Value::from_int(kDefaultAlertTimeout));
        });

    add(registry, "javax/microedition/lcdui/Alert", "getIndicator",
        "()Ljavax/microedition/lcdui/Gauge;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto alert = receiver(arguments);
            if (!alert) return std::unexpected(alert.error());
            auto indicator = reference_field(machine, *alert,
                                             kAlertIndicatorField);
            if (!indicator) return std::unexpected(indicator.error());
            return std::optional<Value>(Value::from_reference(*indicator));
        });
    add(registry, "javax/microedition/lcdui/Alert", "setIndicator",
        "(Ljavax/microedition/lcdui/Gauge;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto alert = receiver(arguments);
            auto indicator = reference_argument(arguments, 1U);
            if (!alert) return std::unexpected(alert.error());
            if (!indicator) return std::unexpected(indicator.error());
            auto current = reference_field(machine, *alert,
                                           kAlertIndicatorField);
            if (!current) return std::unexpected(current.error());
            if (*current == *indicator) return std::optional<Value> {};
            if (!indicator->is_null()) {
                auto valid = machine.object_is_instance(
                    *indicator, "javax/microedition/lcdui/Gauge");
                if (!valid) return std::unexpected(valid.error());
                if (!*valid) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Alert indicator is not a Gauge");
                }
                auto interactive = int_field(machine, *indicator,
                                             kGaugeInteractiveField);
                auto owner = int_field(machine, *indicator, kItemParentField);
                if (!interactive) return std::unexpected(interactive.error());
                if (!owner) return std::unexpected(owner.error());
                if (*interactive != 0) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Alert indicator must be non-interactive");
                }
                if (*owner != 0) {
                    return fail_java("java/lang/IllegalStateException",
                                     "Alert indicator already has an owner");
                }
            }
            auto shown = is_shown(machine, *alert);
            if (!shown) return std::unexpected(shown.error());
            if (*shown && !current->is_null()) {
                auto hidden = set_alert_indicator_visible(
                    machine, *alert, false);
                if (!hidden) return std::unexpected(hidden.error());
            }
            if (!current->is_null()) {
                auto detached = set_int_field(machine, *current,
                                              kItemParentField, 0);
                if (!detached) return std::unexpected(detached.error());
            }
            if (!indicator->is_null()) {
                auto alert_id = ensure_native_id(machine, *alert,
                                                 kDisplayableIdField);
                if (!alert_id) return std::unexpected(alert_id.error());
                auto attached = set_int_field(machine, *indicator,
                                              kItemParentField, *alert_id);
                if (!attached) return std::unexpected(attached.error());
            }
            auto stored = set_reference_field(machine, *alert,
                                              kAlertIndicatorField,
                                              *indicator);
            if (!stored) return std::unexpected(stored.error());
            if (*shown && !indicator->is_null()) {
                auto displayed = set_alert_indicator_visible(
                    machine, *alert, true);
                if (!displayed)
                    return std::unexpected(displayed.error());
            }
            auto updated = emit_screen_update(machine, *alert);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });

    add(registry, "javax/microedition/lcdui/TextBox", "<init>",
        "(Ljava/lang/String;Ljava/lang/String;II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto text_box = receiver(arguments);
            auto title = reference_argument(arguments, 1U);
            auto text = reference_argument(arguments, 2U);
            auto maximum = integer_argument(arguments, 3U);
            auto constraints = integer_argument(arguments, 4U);
            if (!text_box) return std::unexpected(text_box.error());
            if (!title) return std::unexpected(title.error());
            if (!text) return std::unexpected(text.error());
            if (!maximum) return std::unexpected(maximum.error());
            if (!constraints) return std::unexpected(constraints.error());
            if (*maximum <= 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TextBox maxSize must be positive");
            }
            auto normalized = normalized_string(machine, *text);
            if (!normalized) return std::unexpected(normalized.error());
            auto length = text_length(machine, *normalized);
            if (!length) return std::unexpected(length.error());
            if (*length > *maximum) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TextBox initial text exceeds maxSize");
            }
            auto valid_text = validate_text_reference(
                machine, *normalized, *constraints);
            if (!valid_text) return std::unexpected(valid_text.error());
            auto initialized = initialize_displayable(
                machine, *text_box, kTypeForm, *title);
            if (!initialized) return std::unexpected(initialized.error());
            auto peer = ensure_native_id(machine, *text_box,
                                         kTextBoxPeerIdField);
            if (!peer) return std::unexpected(peer.error());
            constexpr std::array<usize, 5> fields {
                kTextBoxTextField,
                kTextBoxMaxSizeField,
                kTextBoxConstraintsField,
                kTextBoxCaretField,
                kTextBoxInputModeField,
            };
            const std::array<Value, 5> values {
                Value::from_reference(*normalized),
                Value::from_int(*maximum),
                Value::from_int(*constraints),
                Value::from_int(*length),
                Value::from_reference({}),
            };
            auto text_initialized = set_field_values(
                machine, *text_box, fields, values);
            if (!text_initialized)
                return std::unexpected(text_initialized.error());

            auto created = screen_event(machine, *text_box,
                                        kEventScreenCreated);
            if (!created) return std::unexpected(created.error());
            machine.emit_ui_event(std::move(*created));
            constexpr std::array<usize, 2> screen_fields {
                kDisplayableIdField,
                kDisplayableTitleField,
            };
            auto screen_state = field_values(
                machine, *text_box, screen_fields);
            if (!screen_state) return std::unexpected(screen_state.error());
            auto screen_id = (*screen_state)[0U].as_int();
            auto screen_title = (*screen_state)[1U].as_reference();
            if (!screen_id) return std::unexpected(screen_id.error());
            if (!screen_title) return std::unexpected(screen_title.error());
            auto encoded_title = utf8_string(machine, *screen_title);
            if (!encoded_title)
                return std::unexpected(encoded_title.error());
            machine.emit_ui_event(UiBridgeEvent {
                .kind = kEventScreenUpdated,
                .component_id = *screen_id,
                .component_type = kTypeForm,
                .arguments = {kScreenKindTextBox, 0, 0,
                              kScreenKindMetadata},
                .text = std::move(*encoded_title),
            });
            auto item_created = emit_text_box(machine, *text_box,
                                              kEventItemCreated);
            if (!item_created) return std::unexpected(item_created.error());
            auto item_shown = emit_text_box(machine, *text_box,
                                            kEventItemShown);
            if (!item_shown) return std::unexpected(item_shown.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/TextBox", "getString",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto text_box = receiver(arguments);
            if (!text_box) return std::unexpected(text_box.error());
            auto text = reference_field(machine, *text_box,
                                        kTextBoxTextField);
            if (!text) return std::unexpected(text.error());
            return std::optional<Value>(Value::from_reference(*text));
        });
    add(registry, "javax/microedition/lcdui/TextBox", "setString",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto text_box = receiver(arguments);
            auto text = reference_argument(arguments, 1U);
            if (!text_box) return std::unexpected(text_box.error());
            if (!text) return std::unexpected(text.error());
            auto normalized = normalized_string(machine, *text);
            if (!normalized) return std::unexpected(normalized.error());
            auto maximum = int_field(machine, *text_box,
                                     kTextBoxMaxSizeField);
            auto constraints = int_field(machine, *text_box,
                                         kTextBoxConstraintsField);
            auto length = text_length(machine, *normalized);
            if (!maximum) return std::unexpected(maximum.error());
            if (!constraints) return std::unexpected(constraints.error());
            if (!length) return std::unexpected(length.error());
            if (*length > *maximum) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TextBox text exceeds maxSize");
            }
            auto valid_text = validate_text_reference(
                machine, *normalized, *constraints);
            if (!valid_text) return std::unexpected(valid_text.error());
            auto first = set_reference_field(machine, *text_box,
                                             kTextBoxTextField, *normalized);
            auto second = set_int_field(machine, *text_box,
                                        kTextBoxCaretField, *length);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            auto updated = emit_text_box(machine, *text_box,
                                         kEventItemUpdated);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/TextBox", "getChars",
        "([C)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto text_box = receiver(arguments);
            auto destination = reference_argument(arguments, 1U, false);
            if (!text_box) return std::unexpected(text_box.error());
            if (!destination) return std::unexpected(destination.error());
            auto text = reference_field(machine, *text_box,
                                        kTextBoxTextField);
            if (!text) return std::unexpected(text.error());
            auto value = machine.heap().string_value(*text);
            if (!value) return std::unexpected(value.error());
            auto length = machine.heap().array_length(*destination);
            if (!length) return std::unexpected(length.error());
            if (*length < value->size()) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "TextBox destination char[] is too small");
            }
            auto stored = machine.heap().write_char_array(
                *destination, 0U, *value);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(value->size())));
        });
    add(registry, "javax/microedition/lcdui/TextBox", "setChars",
        "([CII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto text_box = receiver(arguments);
            auto data = reference_argument(arguments, 1U, false);
            auto offset = integer_argument(arguments, 2U);
            auto length = integer_argument(arguments, 3U);
            if (!text_box) return std::unexpected(text_box.error());
            if (!data) return std::unexpected(data.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            auto value = char_array_slice(machine, *data, *offset, *length);
            if (!value) return std::unexpected(value.error());
            auto stored = set_text_box_value(machine, *text_box,
                                             std::move(*value), *length);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/TextBox", "insert",
        "(Ljava/lang/String;I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto text_box = receiver(arguments);
            auto source = reference_argument(arguments, 1U, false);
            auto position = integer_argument(arguments, 2U);
            if (!text_box) return std::unexpected(text_box.error());
            if (!source) return std::unexpected(source.error());
            if (!position) return std::unexpected(position.error());
            auto current_ref = reference_field(machine, *text_box,
                                               kTextBoxTextField);
            if (!current_ref) return std::unexpected(current_ref.error());
            auto current = machine.heap().string_value(*current_ref);
            auto inserted = machine.heap().string_value(*source);
            if (!current) return std::unexpected(current.error());
            if (!inserted) return std::unexpected(inserted.error());
            if (*position < 0 ||
                static_cast<usize>(*position) > current->size()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TextBox insertion position is invalid");
            }
            current->insert(static_cast<usize>(*position), *inserted);
            auto stored = set_text_box_value(
                machine, *text_box, std::move(*current),
                *position + static_cast<i32>(inserted->size()));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/TextBox", "insert",
        "([CIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto text_box = receiver(arguments);
            auto data = reference_argument(arguments, 1U, false);
            auto offset = integer_argument(arguments, 2U);
            auto length = integer_argument(arguments, 3U);
            auto position = integer_argument(arguments, 4U);
            if (!text_box) return std::unexpected(text_box.error());
            if (!data) return std::unexpected(data.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            if (!position) return std::unexpected(position.error());
            auto inserted = char_array_slice(machine, *data,
                                             *offset, *length);
            if (!inserted) return std::unexpected(inserted.error());
            auto current_ref = reference_field(machine, *text_box,
                                               kTextBoxTextField);
            if (!current_ref) return std::unexpected(current_ref.error());
            auto current = machine.heap().string_value(*current_ref);
            if (!current) return std::unexpected(current.error());
            if (*position < 0 ||
                static_cast<usize>(*position) > current->size()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TextBox insertion position is invalid");
            }
            current->insert(static_cast<usize>(*position), *inserted);
            auto stored = set_text_box_value(
                machine, *text_box, std::move(*current),
                *position + *length);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/TextBox", "delete",
        "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto text_box = receiver(arguments);
            auto offset = integer_argument(arguments, 1U);
            auto length = integer_argument(arguments, 2U);
            if (!text_box) return std::unexpected(text_box.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            auto current_ref = reference_field(machine, *text_box,
                                               kTextBoxTextField);
            if (!current_ref) return std::unexpected(current_ref.error());
            auto current = machine.heap().string_value(*current_ref);
            if (!current) return std::unexpected(current.error());
            if (*offset < 0 || *length < 0 ||
                static_cast<usize>(*offset) > current->size() ||
                static_cast<usize>(*length) > current->size() -
                    static_cast<usize>(*offset)) {
                return fail_java("java/lang/StringIndexOutOfBoundsException",
                                 "TextBox deletion range is invalid");
            }
            current->erase(static_cast<usize>(*offset),
                           static_cast<usize>(*length));
            auto stored = set_text_box_value(machine, *text_box,
                                             std::move(*current), *offset);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    const auto text_box_int_getter = [&registry](const char* name,
                                                 usize field_index) {
        add(registry, "javax/microedition/lcdui/TextBox", name, "()I",
            [field_index](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto text_box = receiver(arguments);
                if (!text_box) return std::unexpected(text_box.error());
                auto value = int_field(machine, *text_box, field_index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_int(*value));
            });
    };
    text_box_int_getter("getMaxSize", kTextBoxMaxSizeField);
    text_box_int_getter("getCaretPosition", kTextBoxCaretField);
    text_box_int_getter("getConstraints", kTextBoxConstraintsField);
    add(registry, "javax/microedition/lcdui/TextBox", "size", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto text_box = receiver(arguments);
            if (!text_box) return std::unexpected(text_box.error());
            auto text = reference_field(machine, *text_box,
                                        kTextBoxTextField);
            if (!text) return std::unexpected(text.error());
            auto length = text_length(machine, *text);
            if (!length) return std::unexpected(length.error());
            return std::optional<Value>(Value::from_int(*length));
        });
    add(registry, "javax/microedition/lcdui/TextBox", "setMaxSize",
        "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto text_box = receiver(arguments);
            auto maximum = integer_argument(arguments, 1U);
            if (!text_box) return std::unexpected(text_box.error());
            if (!maximum) return std::unexpected(maximum.error());
            if (*maximum <= 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TextBox maxSize must be positive");
            }
            auto text = reference_field(machine, *text_box,
                                        kTextBoxTextField);
            if (!text) return std::unexpected(text.error());
            auto value = machine.heap().string_value(*text);
            if (!value) return std::unexpected(value.error());
            if (value->size() > static_cast<usize>(*maximum)) {
                value->resize(static_cast<usize>(*maximum));
                auto replacement = create_string(machine, std::move(*value));
                if (!replacement)
                    return std::unexpected(replacement.error());
                auto text_stored = set_reference_field(
                    machine, *text_box, kTextBoxTextField, *replacement);
                auto caret_stored = set_int_field(
                    machine, *text_box, kTextBoxCaretField, *maximum);
                if (!text_stored)
                    return std::unexpected(text_stored.error());
                if (!caret_stored)
                    return std::unexpected(caret_stored.error());
            }
            auto stored = set_int_field(machine, *text_box,
                                        kTextBoxMaxSizeField, *maximum);
            if (!stored) return std::unexpected(stored.error());
            auto updated = emit_text_box(machine, *text_box,
                                         kEventItemUpdated);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value>(Value::from_int(*maximum));
        });
    add(registry, "javax/microedition/lcdui/TextBox", "setConstraints",
        "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto text_box = receiver(arguments);
            auto constraints = integer_argument(arguments, 1U);
            if (!text_box) return std::unexpected(text_box.error());
            if (!constraints) return std::unexpected(constraints.error());
            auto constraint_valid = validate_text_constraints({}, *constraints);
            if (!constraint_valid)
                return std::unexpected(constraint_valid.error());
            auto text = reference_field(machine, *text_box,
                                        kTextBoxTextField);
            if (!text) return std::unexpected(text.error());
            auto value = machine.heap().string_value(*text);
            if (!value) return std::unexpected(value.error());
            auto content_valid = validate_text_constraints(*value, *constraints);
            if (!content_valid && !value->empty()) {
                auto empty = empty_string(machine);
                if (!empty) return std::unexpected(empty.error());
                auto cleared = set_reference_field(
                    machine, *text_box, kTextBoxTextField, *empty);
                auto caret = set_int_field(machine, *text_box,
                                           kTextBoxCaretField, 0);
                if (!cleared) return std::unexpected(cleared.error());
                if (!caret) return std::unexpected(caret.error());
            }
            auto stored = set_int_field(machine, *text_box,
                                        kTextBoxConstraintsField,
                                        *constraints);
            if (!stored) return std::unexpected(stored.error());
            auto updated = emit_text_box(machine, *text_box,
                                         kEventItemUpdated);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/TextBox",
        "setInitialInputMode", "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto text_box = receiver(arguments);
            auto mode = reference_argument(arguments, 1U);
            if (!text_box) return std::unexpected(text_box.error());
            if (!mode) return std::unexpected(mode.error());
            auto stored = set_reference_field(machine, *text_box,
                                              kTextBoxInputModeField, *mode);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    const auto date_field_constructor = [&registry](const char* descriptor,
                                                     bool has_zone) {
        add(registry, "javax/microedition/lcdui/DateField", "<init>",
            descriptor,
            [has_zone](Machine& machine,
                       std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto item = receiver(arguments);
                auto label = reference_argument(arguments, 1U);
                auto mode = integer_argument(arguments, 2U);
                if (!item) return std::unexpected(item.error());
                if (!label) return std::unexpected(label.error());
                if (!mode) return std::unexpected(mode.error());
                if (*mode < 1 || *mode > 3) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "DateField input mode is invalid");
                }
                ObjectRef zone;
                if (has_zone) {
                    auto parsed = reference_argument(arguments, 3U, false);
                    if (!parsed) return std::unexpected(parsed.error());
                    auto valid = machine.object_is_instance(
                        *parsed, "java/util/TimeZone");
                    if (!valid) return std::unexpected(valid.error());
                    if (!*valid) {
                        return fail_java("java/lang/IllegalArgumentException",
                                         "DateField timezone is invalid");
                    }
                    zone = *parsed;
                } else {
                    auto default_zone = machine.invoke_static(
                        "java/util/TimeZone", "getDefault",
                        "()Ljava/util/TimeZone;");
                    if (!default_zone)
                        return std::unexpected(default_zone.error());
                    auto status = callback_status(
                        machine, *default_zone,
                        "DateField default timezone lookup");
                    if (!status) return std::unexpected(status.error());
                    if (!default_zone->return_value.has_value()) {
                        return fail(ErrorCode::invalid_state,
                                    "TimeZone.getDefault returned no value");
                    }
                    auto parsed = default_zone->return_value->as_reference();
                    if (!parsed) return std::unexpected(parsed.error());
                    if (parsed->is_null()) {
                        return fail(ErrorCode::invalid_state,
                                    "TimeZone.getDefault returned null");
                    }
                    zone = *parsed;
                }
                auto initialized = initialize_item(machine, *item,
                                                   kTypeDateField, *label);
                if (!initialized)
                    return std::unexpected(initialized.error());
                auto date_stored = set_reference_field(
                    machine, *item, kDateFieldDateField, {});
                auto mode_stored = set_int_field(
                    machine, *item, kDateFieldInputModeField, *mode);
                auto zone_stored = set_reference_field(
                    machine, *item, kDateFieldTimeZoneField, zone);
                if (!date_stored)
                    return std::unexpected(date_stored.error());
                if (!mode_stored)
                    return std::unexpected(mode_stored.error());
                if (!zone_stored)
                    return std::unexpected(zone_stored.error());
                return std::optional<Value> {};
            });
    };
    date_field_constructor("(Ljava/lang/String;I)V", false);
    date_field_constructor(
        "(Ljava/lang/String;ILjava/util/TimeZone;)V", true);
    add(registry, "javax/microedition/lcdui/DateField", "getDate",
        "()Ljava/util/Date;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto date = reference_field(machine, *item, kDateFieldDateField);
            if (!date) return std::unexpected(date.error());
            if (date->is_null()) {
                return std::optional<Value>(Value::from_reference({}));
            }
            auto milliseconds = long_field(machine, *date, kDateTimeField);
            if (!milliseconds) return std::unexpected(milliseconds.error());
            auto copy = copy_date(machine, *milliseconds);
            if (!copy) return std::unexpected(copy.error());
            return std::optional<Value>(Value::from_reference(*copy));
        });
    add(registry, "javax/microedition/lcdui/DateField", "setDate",
        "(Ljava/util/Date;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto date = reference_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!date) return std::unexpected(date.error());
            if (!date->is_null()) {
                auto valid = machine.object_is_instance(*date,
                                                        "java/util/Date");
                if (!valid) return std::unexpected(valid.error());
                if (!*valid) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "DateField value is not a Date");
                }
            }
            auto mode = int_field(machine, *item,
                                  kDateFieldInputModeField);
            if (!mode) return std::unexpected(mode.error());
            auto normalized = normalize_date_field_value(
                machine, *item, *date, *mode, false);
            if (!normalized) return std::unexpected(normalized.error());
            auto stored = set_reference_field(machine, *item,
                                              kDateFieldDateField,
                                              *normalized);
            if (!stored) return std::unexpected(stored.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/DateField", "getInputMode",
        "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            if (!item) return std::unexpected(item.error());
            auto mode = int_field(machine, *item, kDateFieldInputModeField);
            if (!mode) return std::unexpected(mode.error());
            return std::optional<Value>(Value::from_int(*mode));
        });
    add(registry, "javax/microedition/lcdui/DateField", "setInputMode",
        "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto mode = integer_argument(arguments, 1U);
            if (!item) return std::unexpected(item.error());
            if (!mode) return std::unexpected(mode.error());
            if (*mode < 1 || *mode > 3) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "DateField input mode is invalid");
            }
            auto previous_mode = int_field(machine, *item,
                                           kDateFieldInputModeField);
            auto date = reference_field(machine, *item,
                                        kDateFieldDateField);
            if (!previous_mode) return std::unexpected(previous_mode.error());
            if (!date) return std::unexpected(date.error());
            if (*previous_mode != *mode && !date->is_null()) {
                auto normalized = normalize_date_field_value(
                    machine, *item, *date, *mode, true);
                if (!normalized) return std::unexpected(normalized.error());
                auto date_stored = set_reference_field(
                    machine, *item, kDateFieldDateField, *normalized);
                if (!date_stored) return std::unexpected(date_stored.error());
            }
            auto stored = set_int_field(machine, *item,
                                        kDateFieldInputModeField, *mode);
            if (!stored) return std::unexpected(stored.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });

    add(registry, "javax/microedition/lcdui/Spacer", "<init>", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto width = integer_argument(arguments, 1U);
            auto height = integer_argument(arguments, 2U);
            if (!item) return std::unexpected(item.error());
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            if (*width < 0 || *height < 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Spacer size must not be negative");
            }
            auto initialized = initialize_item(machine, *item,
                                               kTypeSpacer, {});
            if (!initialized)
                return std::unexpected(initialized.error());
            auto first = set_int_field(machine, *item,
                                       kSpacerWidthField, *width);
            auto second = set_int_field(machine, *item,
                                        kSpacerHeightField, *height);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Spacer", "setMinimumSize",
        "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto item = receiver(arguments);
            auto width = integer_argument(arguments, 1U);
            auto height = integer_argument(arguments, 2U);
            if (!item) return std::unexpected(item.error());
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            if (*width < 0 || *height < 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Spacer size must not be negative");
            }
            auto first = set_int_field(machine, *item,
                                       kSpacerWidthField, *width);
            auto second = set_int_field(machine, *item,
                                        kSpacerHeightField, *height);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            auto updated = update_item_if_attached(machine, *item);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    const auto spacer_getter = [&registry](const char* name,
                                           usize field_index) {
        add(registry, "javax/microedition/lcdui/Spacer", name, "()I",
            [field_index](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto item = receiver(arguments);
                if (!item) return std::unexpected(item.error());
                auto value = int_field(machine, *item, field_index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_int(*value));
            });
    };
    spacer_getter("getMinimumWidth", kSpacerWidthField);
    spacer_getter("getMinimumHeight", kSpacerHeightField);

    const auto alias_native = [&registry](std::string owner,
                                          std::string base_owner,
                                          std::string name,
                                          std::string descriptor) {
        const std::string alias_name = name;
        const std::string alias_descriptor = descriptor;
        add(registry, std::move(owner), alias_name, alias_descriptor,
            [&registry,
             base_owner = std::move(base_owner),
             name = std::move(name),
             descriptor = std::move(descriptor)](
                Machine& machine,
                std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                return registry.invoke(machine, base_owner, name,
                                       descriptor, arguments);
            });
    };
    const auto displayable_alias = [&alias_native](const char* owner,
                                                   const char* name,
                                                   const char* descriptor) {
        alias_native(owner, "javax/microedition/lcdui/Displayable",
                     name, descriptor);
    };
    const auto item_alias = [&alias_native](const char* owner,
                                            const char* name,
                                            const char* descriptor) {
        alias_native(owner, "javax/microedition/lcdui/Item",
                     name, descriptor);
    };

    add(registry, "javax/microedition/lcdui/Alert", "addCommand",
        "(Ljavax/microedition/lcdui/Command;)V",
        [&registry](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto alert = receiver(arguments);
            auto command = reference_argument(arguments, 1U, false);
            if (!alert) return std::unexpected(alert.error());
            if (!command) return std::unexpected(command.error());
            auto dismiss = alert_dismiss_command(machine);
            if (!dismiss) return std::unexpected(dismiss.error());
            if (*command == *dismiss) return std::optional<Value> {};

            auto commands = reference_field(machine, *alert,
                                            kDisplayableCommandsField);
            auto count = int_field(machine, *alert,
                                   kDisplayableCommandCountField);
            if (!commands) return std::unexpected(commands.error());
            if (!count) return std::unexpected(count.error());
            if (*count == 1 && !commands->is_null()) {
                auto first = machine.heap().element(*commands, 0U);
                if (!first) return std::unexpected(first.error());
                auto existing = first->as_reference();
                if (!existing) return std::unexpected(existing.error());
                if (*existing == *dismiss) {
                    const Value remove_arguments[] {
                        Value::from_reference(*alert),
                        Value::from_reference(*dismiss),
                    };
                    auto removed = registry.invoke(
                        machine, "javax/microedition/lcdui/Displayable",
                        "removeCommand",
                        "(Ljavax/microedition/lcdui/Command;)V",
                        remove_arguments);
                    if (!removed) return std::unexpected(removed.error());
                }
            }

            auto added = registry.invoke(
                machine, "javax/microedition/lcdui/Displayable",
                "addCommand",
                "(Ljavax/microedition/lcdui/Command;)V", arguments);
            if (!added) return std::unexpected(added.error());
            auto scheduled = refresh_alert_timeout(machine, *alert);
            if (!scheduled) return std::unexpected(scheduled.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/lcdui/Alert", "removeCommand",
        "(Ljavax/microedition/lcdui/Command;)V",
        [&registry](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto alert = receiver(arguments);
            auto command = reference_argument(arguments, 1U);
            if (!alert) return std::unexpected(alert.error());
            if (!command) return std::unexpected(command.error());
            if (command->is_null()) return std::optional<Value> {};
            auto dismiss = alert_dismiss_command(machine);
            if (!dismiss) return std::unexpected(dismiss.error());
            if (*command == *dismiss) return std::optional<Value> {};

            auto removed = registry.invoke(
                machine, "javax/microedition/lcdui/Displayable",
                "removeCommand",
                "(Ljavax/microedition/lcdui/Command;)V", arguments);
            if (!removed) return std::unexpected(removed.error());
            auto count = int_field(machine, *alert,
                                   kDisplayableCommandCountField);
            if (!count) return std::unexpected(count.error());
            if (*count == 0) {
                const Value add_arguments[] {
                    Value::from_reference(*alert),
                    Value::from_reference(*dismiss),
                };
                auto restored = registry.invoke(
                    machine, "javax/microedition/lcdui/Displayable",
                    "addCommand",
                    "(Ljavax/microedition/lcdui/Command;)V",
                    add_arguments);
                if (!restored) return std::unexpected(restored.error());
            }
            auto scheduled = refresh_alert_timeout(machine, *alert);
            if (!scheduled) return std::unexpected(scheduled.error());
            return std::optional<Value> {};
        });
    displayable_alias("javax/microedition/lcdui/Alert",
                      "setCommandListener",
                      "(Ljavax/microedition/lcdui/CommandListener;)V");
    displayable_alias("javax/microedition/lcdui/Form", "getWidth", "()I");
    displayable_alias("javax/microedition/lcdui/Form", "getHeight", "()I");
    displayable_alias("javax/microedition/lcdui/List", "removeCommand",
                      "(Ljavax/microedition/lcdui/Command;)V");

    for (const char* name : {"addCommand", "setDefaultCommand"}) {
        item_alias("javax/microedition/lcdui/Gauge", name,
                   "(Ljavax/microedition/lcdui/Command;)V");
    }
    item_alias("javax/microedition/lcdui/Gauge",
               "setItemCommandListener",
               "(Ljavax/microedition/lcdui/ItemCommandListener;)V");
    item_alias("javax/microedition/lcdui/Gauge", "setLabel",
               "(Ljava/lang/String;)V");
    item_alias("javax/microedition/lcdui/Gauge", "setLayout", "(I)V");
    item_alias("javax/microedition/lcdui/Gauge", "setPreferredSize", "(II)V");

    for (const char* name : {"addCommand", "setDefaultCommand"}) {
        item_alias("javax/microedition/lcdui/Spacer", name,
                   "(Ljavax/microedition/lcdui/Command;)V");
    }
    item_alias("javax/microedition/lcdui/Spacer", "setLabel",
               "(Ljava/lang/String;)V");
    item_alias("javax/microedition/lcdui/ImageItem", "getLayout", "()I");
    item_alias("javax/microedition/lcdui/ImageItem", "setLayout", "(I)V");
    item_alias("javax/microedition/lcdui/StringItem", "setPreferredSize",
               "(II)V");
}

Status replay_current_lcdui(Machine& machine) {
    auto current = current_displayable(machine);
    if (!current) return std::unexpected(current.error());
    if (current->is_null()) {
        machine.emit_ui_event(UiBridgeEvent {.kind = kEventCommandsReset});
        return {};
    }

    // Re-assigning an already-running MIDlet to the host foreground does not
    // call Display.setCurrent() again, so no native bridge events are normally
    // emitted. Re-publish the live Displayable from VM fields instead of
    // relying on the iOS host to retain a process-global Form/List snapshot.
    // Do not emit RESET: keeping unrelated cached screens preserves native
    // focus/scroll state, while SCREEN_SHOWN deterministically selects this
    // MIDlet's current screen.
    auto shown = screen_event(machine, *current, kEventScreenShown);
    if (!shown) return std::unexpected(shown.error());
    machine.emit_ui_event(std::move(*shown));

    constexpr std::array<std::string_view, 4> screen_types {
        "javax/microedition/lcdui/TextBox",
        "javax/microedition/lcdui/List",
        "javax/microedition/lcdui/Form",
        "javax/microedition/lcdui/Alert",
    };
    auto screen_matches = instance_matches(machine, *current, screen_types);
    if (!screen_matches) return std::unexpected(screen_matches.error());
    const bool is_text_box = (*screen_matches)[0U];
    const bool is_list = (*screen_matches)[1U];
    const bool is_form = (*screen_matches)[2U];
    const bool is_alert = (*screen_matches)[3U];

    if (is_text_box) {
        auto metadata = screen_event(machine, *current, kEventScreenUpdated);
        if (!metadata) return std::unexpected(metadata.error());
        metadata->arguments = {kScreenKindTextBox, 0, 0,
                               kScreenKindMetadata};
        machine.emit_ui_event(std::move(*metadata));
        auto emitted = emit_text_box(machine, *current, kEventItemShown);
        if (!emitted) return emitted;
    } else if (is_list) {
        auto metadata = screen_event(machine, *current, kEventScreenUpdated);
        if (!metadata) return std::unexpected(metadata.error());
        metadata->arguments = {kScreenKindList, 0, 0,
                               kScreenKindMetadata};
        machine.emit_ui_event(std::move(*metadata));

        auto screen_id = int_field(machine, *current, kDisplayableIdField);
        auto peer_id = int_field(machine, *current, kListPeerIdField);
        auto choice_type = int_field(machine, *current,
                                     kListChoiceTypeField);
        if (!screen_id) return std::unexpected(screen_id.error());
        if (!peer_id) return std::unexpected(peer_id.error());
        if (!choice_type) return std::unexpected(choice_type.error());
        if (*choice_type < 1 || *choice_type > 4) {
            return fail(ErrorCode::invalid_state,
                        "LCDUI List has an invalid choice type");
        }
        machine.emit_ui_event(UiBridgeEvent {
            .kind = kEventItemShown,
            .component_id = *peer_id,
            .parent_id = *screen_id,
            .component_type = *choice_type - 1,
        });
        auto choices = emit_choice_elements(machine, *current);
        if (!choices) return choices;
    } else if (is_form) {
        auto count = int_field(machine, *current, kFormItemCountField);
        auto items = reference_field(machine, *current, kFormItemsField);
        if (!count) return std::unexpected(count.error());
        if (!items) return std::unexpected(items.error());
        if (!items->is_null()) {
            for (i32 index = 0; index < *count; ++index) {
                auto value = machine.heap().element(
                    *items, static_cast<usize>(index));
                if (!value) return std::unexpected(value.error());
                auto item = value->as_reference();
                if (!item) return std::unexpected(item.error());
                if (item->is_null()) continue;
                auto emitted = emit_item(machine, *item,
                                         kEventItemShown, index);
                if (!emitted) return emitted;
                auto is_choice_group = machine.object_is_instance(
                    *item, "javax/microedition/lcdui/ChoiceGroup");
                if (!is_choice_group) {
                    return std::unexpected(is_choice_group.error());
                }
                if (*is_choice_group) {
                    auto choices = emit_choice_elements(machine, *item);
                    if (!choices) return choices;
                }
            }
        }
    }

    if (is_alert) {
        auto indicator = reference_field(machine, *current,
                                         kAlertIndicatorField);
        if (!indicator) return std::unexpected(indicator.error());
        if (!indicator->is_null()) {
            auto emitted = emit_item(machine, *indicator,
                                     kEventItemShown);
            if (!emitted) return emitted;
        }
    }

    return emit_commands(machine, *current);
}

Status pump_lcdui_alert_timeouts(Machine& machine) {
    auto due = machine.take_due_lcdui_alert();
    if (!due) return std::unexpected(due.error());
    if (!due->has_value()) return {};

    const ObjectRef alert = **due;
    auto current = current_displayable(machine);
    if (!current) return std::unexpected(current.error());
    if (*current != alert) return {};

    constexpr std::array<usize, 3> fields {
        kDisplayableShownField,
        kAlertTimeoutField,
        kDisplayableCommandCountField,
    };
    auto state = field_values(machine, alert, fields);
    if (!state) return std::unexpected(state.error());
    auto shown = (*state)[0U].as_int();
    auto timeout = (*state)[1U].as_int();
    auto command_count = (*state)[2U].as_int();
    if (!shown || !timeout || !command_count) {
        return fail(ErrorCode::invalid_state,
                    "LCDUI Alert timeout state is invalid");
    }
    if (*shown == 0 || *timeout == kAlertForever || *command_count != 1) {
        return {};
    }

    auto command = sole_alert_command(machine, alert);
    if (!command) return std::unexpected(command.error());
    return dispatch_alert_command(machine, alert, *command);
}

Status handle_lcdui_action(Machine& machine,
                           i32 kind,
                           i32 component_id,
                           i32 first,
                           i64 value64,
                           std::string text) {
    if (kind == 107) {
        auto current = current_displayable(machine);
        if (!current) return std::unexpected(current.error());
        if (current->is_null()) return {};
        const i32 position = std::max(first, 0);
        auto stored = set_int_field(machine, *current,
                                    kDisplayableScrollField, position);
        if (!stored) return stored;
        auto event = screen_event(machine, *current, kEventScreenUpdated);
        if (!event) return std::unexpected(event.error());
        event->arguments = {0, 0, position, 0};
        machine.emit_ui_event(std::move(*event));
        return {};
    }
    auto component = machine.ui_component(component_id);
    if (!component) return std::unexpected(component.error());

    if (kind == static_cast<i32>(
            LcduiActionKind::select_list_item_command)) {
        if (value64 < std::numeric_limits<i32>::min() ||
            value64 > std::numeric_limits<i32>::max()) {
            return fail(ErrorCode::overflow,
                        "LCDUI list item command ID is out of range");
        }
        auto is_list = machine.object_is_instance(
            *component, "javax/microedition/lcdui/List");
        if (!is_list) return std::unexpected(is_list.error());
        if (!*is_list) {
            return fail(ErrorCode::invalid_argument,
                        "LCDUI list item command target is not a List");
        }
        auto current = current_displayable(machine);
        if (!current) return std::unexpected(current.error());
        if (current->is_null() || *current != *component) return {};

        const i32 command_id = static_cast<i32>(value64);
        auto command = machine.ui_component(command_id);
        if (!command) return std::unexpected(command.error());
        auto is_command = machine.object_is_instance(
            *command, "javax/microedition/lcdui/Command");
        if (!is_command) return std::unexpected(is_command.error());
        if (!*is_command) {
            return fail(ErrorCode::invalid_argument,
                        "LCDUI list item action target is not a Command");
        }
        auto type = int_field(machine, *command, kCommandTypeField);
        auto owner = int_field(machine, *command, kCommandOwnerItemField);
        if (!type) return std::unexpected(type.error());
        if (!owner) return std::unexpected(owner.error());
        if (*type != kCommandTypeItem || *owner != 0) {
            return fail(ErrorCode::invalid_argument,
                        "LCDUI list context action requires Command.ITEM");
        }
        auto commands = reference_field(machine, *component,
                                        kDisplayableCommandsField);
        auto count = int_field(machine, *component,
                               kDisplayableCommandCountField);
        if (!commands) return std::unexpected(commands.error());
        if (!count) return std::unexpected(count.error());
        auto attached = reference_array_contains(
            machine, *commands, *count, *command);
        if (!attached) return std::unexpected(attached.error());
        if (!*attached) {
            return fail(ErrorCode::out_of_range,
                        "LCDUI list item command action is stale");
        }

        auto selected = handle_choice_action(
            machine, *component, first, true);
        if (!selected) return selected;
        return dispatch_command_listener(machine, *command, *component);
    }

    if (kind == static_cast<i32>(LcduiActionKind::custom_item_key)) {
        auto valid = machine.object_is_instance(
            *component, "javax/microedition/lcdui/CustomItem");
        if (!valid) return std::unexpected(valid.error());
        if (!*valid) {
            return fail(ErrorCode::invalid_argument,
                        "LCDUI custom key target is not a CustomItem");
        }
        std::string_view callback;
        switch (static_cast<CustomItemKeyPhase>(value64)) {
        case CustomItemKeyPhase::pressed: callback = "keyPressed"; break;
        case CustomItemKeyPhase::released: callback = "keyReleased"; break;
        case CustomItemKeyPhase::repeated: callback = "keyRepeated"; break;
        default:
            return fail(ErrorCode::invalid_argument,
                        "LCDUI CustomItem key phase is invalid");
        }
        const Value key_arguments[] {Value::from_int(first)};
        auto invoked = invoke_custom(machine, *component, callback,
                                     "(I)V", key_arguments);
        if (!invoked) return std::unexpected(invoked.error());
        return {};
    }

    if (kind == 101) {
        auto valid = validate_item(machine, *component);
        if (!valid) return valid;
        auto is_custom = machine.object_is_instance(
            *component, "javax/microedition/lcdui/CustomItem");
        if (!is_custom) return std::unexpected(is_custom.error());
        if (*is_custom) {
            auto width = int_field(machine, *component,
                                   kItemPreferredWidthField);
            auto height = int_field(machine, *component,
                                    kItemPreferredHeightField);
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            auto visible = machine.heap().allocate_array(
                "[I", 4U, Value::from_int(0));
            if (!visible) return std::unexpected(visible.error());
            const Value traverse_arguments[] {
                Value::from_int(0),
                Value::from_int(std::max(*width, 0)),
                Value::from_int(std::max(*height, 0)),
                Value::from_reference(*visible),
            };
            auto traversed = invoke_custom(
                machine, *component, "traverse", "(III[I)Z",
                traverse_arguments);
            if (!traversed) return std::unexpected(traversed.error());
        }
        machine.emit_ui_event(UiBridgeEvent {
            .kind = 16,
            .component_id = component_id,
        });
        auto current = current_displayable(machine);
        if (!current) return std::unexpected(current.error());
        if (current->is_null()) return {};
        return emit_commands(machine, *current, *component);
    }

    if (kind == 102) {
        auto valid = validate_item(machine, *component);
        if (!valid) return valid;
        auto is_custom = machine.object_is_instance(
            *component, "javax/microedition/lcdui/CustomItem");
        if (!is_custom) return std::unexpected(is_custom.error());
        if (*is_custom) {
            const Value pointer_arguments[] {
                Value::from_int(0), Value::from_int(0),
            };
            auto pressed = invoke_custom(
                machine, *component, "pointerPressed", "(II)V",
                pointer_arguments);
            if (!pressed) return std::unexpected(pressed.error());
            auto released = invoke_custom(
                machine, *component, "pointerReleased", "(II)V",
                pointer_arguments);
            if (!released) return std::unexpected(released.error());
        }
        auto command = reference_field(machine, *component,
                                       kItemDefaultCommandField);
        if (!command) return std::unexpected(command.error());
        if (command->is_null()) return {};
        return dispatch_item_command_listener(machine, *command, *component);
    }

    if (kind == 103) {
        constexpr std::array<std::string_view, 2> text_types {
            "javax/microedition/lcdui/TextField",
            "javax/microedition/lcdui/TextBox",
        };
        auto text_matches = instance_matches(machine, *component, text_types);
        if (!text_matches) return std::unexpected(text_matches.error());
        const bool is_text_field = (*text_matches)[0U];
        const bool is_text_box = (*text_matches)[1U];
        if (!is_text_field && !is_text_box) {
            return fail(ErrorCode::invalid_argument,
                        "LCDUI text action target is not a text control");
        }
        const usize text_field = is_text_box
            ? kTextBoxTextField : kTextFieldTextField;
        const usize maximum_field = is_text_box
            ? kTextBoxMaxSizeField : kTextFieldMaxSizeField;
        const usize caret_field = is_text_box
            ? kTextBoxCaretField : kTextFieldCaretField;
        const usize constraints_field = is_text_box
            ? kTextBoxConstraintsField : kTextFieldConstraintsField;
        auto decoded = decode_ui_utf8(text);
        if (!decoded) return std::unexpected(decoded.error());
        auto maximum = int_field(machine, *component, maximum_field);
        auto constraints = int_field(machine, *component, constraints_field);
        if (!maximum) return std::unexpected(maximum.error());
        if (!constraints) return std::unexpected(constraints.error());
        if ((*constraints & kConstraintUneditable) != 0) return {};
        if (decoded->size() > static_cast<usize>(*maximum)) {
            decoded->resize(static_cast<usize>(*maximum));
        }
        auto valid_text = validate_text_constraints(*decoded, *constraints);
        if (!valid_text) return valid_text;
        auto string = create_string(machine, std::move(*decoded));
        if (!string) return std::unexpected(string.error());
        auto stored = set_reference_field(machine, *component,
                                          text_field, *string);
        if (!stored) return stored;
        auto length = text_length(machine, *string);
        if (!length) return std::unexpected(length.error());
        const i32 caret = std::clamp(first, 0, *length);
        auto caret_stored = set_int_field(machine, *component,
                                          caret_field, caret);
        if (!caret_stored) return caret_stored;
        auto emitted = is_text_box
            ? emit_text_box(machine, *component, kEventItemUpdated)
            : emit_item(machine, *component, kEventItemUpdated);
        if (!emitted) return emitted;
        if (is_text_field) {
            return dispatch_item_state_listener(machine, *component);
        }
        return {};
    }

    if (kind == 104) {
        const bool selected = value64 != 0;
        auto changed = handle_choice_action(machine, *component,
                                            first, selected);
        if (!changed) return changed;
        auto is_group = machine.object_is_instance(
            *component, "javax/microedition/lcdui/ChoiceGroup");
        if (!is_group) return std::unexpected(is_group.error());
        if (*is_group) {
            auto callback = dispatch_item_state_listener(machine, *component);
            if (!callback) return callback;
        }
        if (!selected) return {};

        auto command = implicit_choice_command(machine, *component);
        if (!command) return std::unexpected(command.error());
        if (!command->has_value()) return {};
        auto current = current_displayable(machine);
        if (!current) return std::unexpected(current.error());
        if (current->is_null() || *current != *component) return {};
        return dispatch_command_listener(machine, **command, *current);
    }

    if (kind == 105) {
        auto is_gauge = machine.object_is_instance(
            *component, "javax/microedition/lcdui/Gauge");
        if (!is_gauge) return std::unexpected(is_gauge.error());
        if (!*is_gauge) {
            return fail(ErrorCode::invalid_argument,
                        "LCDUI gauge action target is not a Gauge");
        }
        auto interactive = int_field(machine, *component,
                                     kGaugeInteractiveField);
        auto maximum = int_field(machine, *component, kGaugeMaxValueField);
        if (!interactive) return std::unexpected(interactive.error());
        if (!maximum) return std::unexpected(maximum.error());
        auto normalized = normalized_gauge_value(
            *interactive != 0, *maximum, first);
        if (!normalized) return std::unexpected(normalized.error());
        auto stored = set_int_field(machine, *component,
                                    kGaugeValueField, *normalized);
        if (!stored) return stored;
        auto emitted = emit_item(machine, *component, kEventItemUpdated);
        if (!emitted) return emitted;
        return dispatch_item_state_listener(machine, *component);
    }

    if (kind == 106) {
        auto is_date_field = machine.object_is_instance(
            *component, "javax/microedition/lcdui/DateField");
        if (!is_date_field) return std::unexpected(is_date_field.error());
        if (!*is_date_field) {
            return fail(ErrorCode::invalid_argument,
                        "LCDUI date action target is not a DateField");
        }
        if (value64 > std::numeric_limits<i64>::max() / 1'000 ||
            value64 < std::numeric_limits<i64>::min() / 1'000) {
            return fail(ErrorCode::overflow,
                        "LCDUI date value overflows milliseconds");
        }
        auto date = machine.class_states().allocate_instance(
            machine.heap(), "java/util/Date");
        if (!date) return std::unexpected(date.error());
        auto time_stored = set_long_field(machine, *date, kDateTimeField,
                                          value64 * 1'000);
        if (!time_stored) return std::unexpected(time_stored.error());
        auto date_stored = set_reference_field(machine, *component,
                                               kDateFieldDateField, *date);
        if (!date_stored) return std::unexpected(date_stored.error());
        auto emitted = emit_item(machine, *component, kEventItemUpdated);
        if (!emitted) return emitted;
        return dispatch_item_state_listener(machine, *component);
    }

    if (kind == 100) {
        auto is_command = machine.object_is_instance(
            *component, "javax/microedition/lcdui/Command");
        if (!is_command) return std::unexpected(is_command.error());
        if (!*is_command) {
            return fail(ErrorCode::invalid_argument,
                        "LCDUI command action target is not a Command");
        }
        auto owner_id = int_field(machine, *component,
                                  kCommandOwnerItemField);
        if (!owner_id) return std::unexpected(owner_id.error());
        if (*owner_id != 0) {
            auto item = machine.ui_component(*owner_id);
            if (!item) return std::unexpected(item.error());
            auto commands = reference_field(machine, *item,
                                            kItemCommandsField);
            auto count = int_field(machine, *item,
                                   kItemCommandCountField);
            if (!commands) return std::unexpected(commands.error());
            if (!count) return std::unexpected(count.error());
            auto attached = reference_array_contains(
                machine, *commands, *count, *component);
            if (!attached) return std::unexpected(attached.error());
            if (!*attached) {
                return fail(ErrorCode::out_of_range,
                            "LCDUI item command action is stale");
            }
            return dispatch_item_command_listener(machine, *component, *item);
        }
        auto current = current_displayable(machine);
        if (!current) return std::unexpected(current.error());
        if (current->is_null()) return {};
        auto commands = reference_field(machine, *current,
                                        kDisplayableCommandsField);
        auto count = int_field(machine, *current,
                               kDisplayableCommandCountField);
        if (!commands) return std::unexpected(commands.error());
        if (!count) return std::unexpected(count.error());
        auto attached = reference_array_contains(
            machine, *commands, *count, *component);
        if (!attached) return std::unexpected(attached.error());
        if (!*attached) {
            return fail(ErrorCode::out_of_range,
                        "LCDUI screen command action is stale");
        }

        auto is_alert = machine.object_is_instance(
            *current, "javax/microedition/lcdui/Alert");
        if (!is_alert) return std::unexpected(is_alert.error());
        if (*is_alert) {
            return dispatch_alert_command(machine, *current, *component);
        }
        return dispatch_command_listener(machine, *component, *current);
    }

    return fail(ErrorCode::unsupported_feature,
                "LCDUI action kind is not implemented");
}

} // namespace phoneme::vm
