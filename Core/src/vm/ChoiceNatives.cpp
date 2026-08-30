#include "ChoiceNatives.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "phoneme/vm/Machine.hpp"

namespace phoneme::vm {
namespace {

constexpr i32 kEventScreenCreated = 2;
constexpr i32 kEventScreenUpdated = 3;
constexpr i32 kEventItemCreated = 7;
constexpr i32 kEventItemShown = 9;
constexpr i32 kEventChoiceElement = 12;
constexpr i32 kEventChoiceDeleted = 13;
constexpr i32 kScreenMetadata = -1006;

constexpr i32 kChoiceExclusive = 1;
constexpr i32 kChoiceMultiple = 2;
constexpr i32 kChoiceImplicit = 3;
constexpr i32 kChoicePopup = 4;

constexpr i32 kTypeExclusive = 0;
constexpr i32 kTypeMultiple = 1;
constexpr i32 kTypeImplicit = 2;
constexpr i32 kTypePopup = 3;
constexpr i32 kTypeForm = 23;

constexpr usize kDisplayableIdField = 0;
constexpr usize kDisplayableTypeField = 1;
constexpr usize kDisplayableTitleField = 2;
constexpr usize kDisplayableListenerField = 3;
constexpr usize kDisplayableCommandsField = 4;
constexpr usize kDisplayableCommandCountField = 5;
constexpr usize kDisplayableShownField = 6;

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

constexpr usize kCommandIdField = 0;
constexpr usize kCommandLabelField = 1;
constexpr usize kCommandLongLabelField = 2;
constexpr usize kCommandTypeField = 3;
constexpr usize kCommandPriorityField = 4;
constexpr usize kCommandOwnerItemField = 5;

struct ChoiceLayout final {
    bool list {false};
    usize native_id {0};
    usize type {0};
    usize strings {0};
    usize images {0};
    usize fonts {0};
    usize selected {0};
    usize count {0};
    usize fit_policy {0};
    usize select_command {0};
};

constexpr ChoiceLayout kGroupLayout {
    .list = false,
    .native_id = 0,
    .type = 11,
    .strings = 12,
    .images = 13,
    .fonts = 17,
    .selected = 14,
    .count = 15,
    .fit_policy = 16,
};

constexpr ChoiceLayout kListLayout {
    .list = true,
    .native_id = 9,
    .type = 10,
    .strings = 11,
    .images = 12,
    .fonts = 17,
    .selected = 13,
    .count = 14,
    .fit_policy = 15,
    .select_command = 16,
};

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
                    "Choice native method has no receiver");
    }
    auto object = arguments.front().as_reference();
    if (!object || object->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Choice receiver is null");
    }
    return *object;
}

[[nodiscard]] Result<ObjectRef> reference_argument(
    std::span<const Value> arguments,
    usize index,
    bool allow_null = true) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    "Choice reference argument is missing");
    }
    auto value = arguments[index].as_reference();
    if (!value) return std::unexpected(value.error());
    if (!allow_null && value->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Choice reference argument is null");
    }
    return *value;
}

[[nodiscard]] Result<i32> int_argument(std::span<const Value> arguments,
                                       usize index) {
    if (index >= arguments.size()) {
        return fail(ErrorCode::invalid_argument,
                    "Choice integer argument is missing");
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

[[nodiscard]] Result<std::u16string> string_value(Machine& machine,
                                                  ObjectRef string) {
    if (string.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Choice text is null");
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

[[nodiscard]] std::string utf8(std::u16string_view text) {
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

[[nodiscard]] Result<std::string> string_utf8(Machine& machine,
                                              ObjectRef string) {
    auto value = string_value(machine, string);
    if (!value) return std::unexpected(value.error());
    return utf8(*value);
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

[[nodiscard]] Result<ObjectRef> create_string(Machine& machine,
                                              std::u16string text) {
    auto string = machine.class_states().allocate_instance(
        machine.heap(), "java/lang/String");
    if (!string) return std::unexpected(string.error());
    auto attached = machine.heap().attach_string(*string, std::move(text));
    if (!attached) return std::unexpected(attached.error());
    return *string;
}

[[nodiscard]] Result<ChoiceLayout> layout_for(Machine& machine,
                                              ObjectRef object) {
    auto is_list = machine.object_is_instance(
        object, "javax/microedition/lcdui/List");
    if (!is_list) return std::unexpected(is_list.error());
    if (*is_list) return kListLayout;

    auto is_group = machine.object_is_instance(
        object, "javax/microedition/lcdui/ChoiceGroup");
    if (!is_group) return std::unexpected(is_group.error());
    if (*is_group) return kGroupLayout;

    return fail(ErrorCode::invalid_argument,
                "object is not an LCDUI Choice implementation");
}

[[nodiscard]] i32 component_type(i32 choice_type) noexcept {
    switch (choice_type) {
    case kChoiceExclusive: return kTypeExclusive;
    case kChoiceMultiple: return kTypeMultiple;
    case kChoiceImplicit: return kTypeImplicit;
    case kChoicePopup: return kTypePopup;
    default: return kTypeExclusive;
    }
}

[[nodiscard]] Status validate_choice_type(bool list, i32 choice_type) {
    const bool valid = list
        ? (choice_type == kChoiceExclusive ||
           choice_type == kChoiceMultiple ||
           choice_type == kChoiceImplicit)
        : (choice_type == kChoiceExclusive ||
           choice_type == kChoiceMultiple ||
           choice_type == kChoicePopup);
    if (!valid) {
        return fail_java("java/lang/IllegalArgumentException",
                         "invalid MIDP Choice type");
    }
    return {};
}

[[nodiscard]] Result<i32> ensure_component_id(Machine& machine,
                                              ObjectRef object,
                                              const ChoiceLayout& layout) {
    auto current = int_field(machine, object, layout.native_id);
    if (!current) return std::unexpected(current.error());
    if (*current != 0) {
        auto registered = machine.register_ui_component(*current, object);
        if (!registered) return std::unexpected(registered.error());
        return *current;
    }
    const i32 allocated = machine.allocate_ui_component_id();
    if (allocated == 0) {
        return fail_java("java/lang/OutOfMemoryError",
                         "Choice component ID space is exhausted");
    }
    auto stored = set_int_field(machine, object, layout.native_id, allocated);
    if (!stored) return std::unexpected(stored.error());
    auto registered = machine.register_ui_component(allocated, object);
    if (!registered) return std::unexpected(registered.error());
    return allocated;
}

[[nodiscard]] Result<ObjectRef> ensure_array(Machine& machine,
                                             ObjectRef owner,
                                             usize field_index,
                                             std::string class_name,
                                             usize minimum,
                                             Value initial) {
    auto current = reference_field(machine, owner, field_index);
    if (!current) return std::unexpected(current.error());
    usize capacity = 0;
    if (!current->is_null()) {
        auto length = machine.heap().array_length(*current);
        if (!length) return std::unexpected(length.error());
        capacity = *length;
        if (capacity >= minimum) return *current;
    }
    usize replacement_capacity = std::max<usize>(4U, capacity);
    while (replacement_capacity < minimum) {
        if (replacement_capacity > std::numeric_limits<usize>::max() / 2U) {
            return fail_java("java/lang/OutOfMemoryError",
                             "Choice storage capacity overflow");
        }
        replacement_capacity *= 2U;
    }
    auto replacement = machine.heap().allocate_array(
        std::move(class_name), replacement_capacity, initial);
    if (!replacement) {
        if (replacement.error().code == ErrorCode::overflow) {
            return fail_java("java/lang/OutOfMemoryError",
                             "Choice storage allocation failed");
        }
        return std::unexpected(replacement.error());
    }
    if (!current->is_null()) {
        auto copied = machine.heap().copy_array_range(
            *current, 0U, *replacement, 0U, capacity);
        if (!copied) return std::unexpected(copied.error());
    }
    auto assigned = set_reference_field(machine, owner, field_index,
                                        *replacement);
    if (!assigned) return std::unexpected(assigned.error());
    return *replacement;
}

[[nodiscard]] Status initialize_choice_storage(Machine& machine,
                                               ObjectRef object,
                                               const ChoiceLayout& layout,
                                               i32 choice_type,
                                               usize capacity) {
    auto type_stored = set_int_field(machine, object, layout.type, choice_type);
    auto count_stored = set_int_field(machine, object, layout.count, 0);
    auto fit_stored = set_int_field(machine, object, layout.fit_policy, 0);
    if (!type_stored) return type_stored;
    if (!count_stored) return count_stored;
    if (!fit_stored) return fit_stored;
    auto strings = ensure_array(machine, object, layout.strings,
                                "[Ljava/lang/String;", capacity,
                                Value::from_reference({}));
    auto images = ensure_array(machine, object, layout.images,
                               "[Ljavax/microedition/lcdui/Image;", capacity,
                               Value::from_reference({}));
    auto fonts = ensure_array(machine, object, layout.fonts,
                              "[Ljavax/microedition/lcdui/Font;", capacity,
                              Value::from_reference({}));
    auto selected = ensure_array(machine, object, layout.selected,
                                 "[Z", capacity, Value::from_int(0));
    if (!strings) return std::unexpected(strings.error());
    if (!images) return std::unexpected(images.error());
    if (!fonts) return std::unexpected(fonts.error());
    if (!selected) return std::unexpected(selected.error());
    return {};
}

[[nodiscard]] Result<i32> choice_count(Machine& machine,
                                       ObjectRef object,
                                       const ChoiceLayout& layout) {
    return int_field(machine, object, layout.count);
}

[[nodiscard]] Status check_index(Machine& machine,
                                 ObjectRef object,
                                 const ChoiceLayout& layout,
                                 i32 index,
                                 bool allow_end = false) {
    auto count = choice_count(machine, object, layout);
    if (!count) return std::unexpected(count.error());
    const bool valid = allow_end ? index >= 0 && index <= *count
                                 : index >= 0 && index < *count;
    if (!valid) {
        return fail_java("java/lang/IndexOutOfBoundsException",
                         "Choice element index is outside bounds");
    }
    return {};
}

[[nodiscard]] Result<ObjectRef> choice_string(Machine& machine,
                                              ObjectRef object,
                                              const ChoiceLayout& layout,
                                              i32 index) {
    auto checked = check_index(machine, object, layout, index);
    if (!checked) return std::unexpected(checked.error());
    auto strings = reference_field(machine, object, layout.strings);
    if (!strings) return std::unexpected(strings.error());
    auto value = machine.heap().element(*strings, static_cast<usize>(index));
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Result<ObjectRef> choice_image(Machine& machine,
                                             ObjectRef object,
                                             const ChoiceLayout& layout,
                                             i32 index) {
    auto checked = check_index(machine, object, layout, index);
    if (!checked) return std::unexpected(checked.error());
    auto images = reference_field(machine, object, layout.images);
    if (!images) return std::unexpected(images.error());
    auto value = machine.heap().element(*images, static_cast<usize>(index));
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Result<bool> selected_at(Machine& machine,
                                       ObjectRef object,
                                       const ChoiceLayout& layout,
                                       i32 index) {
    auto checked = check_index(machine, object, layout, index);
    if (!checked) return std::unexpected(checked.error());
    auto selected = reference_field(machine, object, layout.selected);
    if (!selected) return std::unexpected(selected.error());
    auto value = machine.heap().element(*selected, static_cast<usize>(index));
    if (!value) return std::unexpected(value.error());
    auto integer = value->as_int();
    if (!integer) return std::unexpected(integer.error());
    return *integer != 0;
}

[[nodiscard]] Status set_selected_raw(Machine& machine,
                                      ObjectRef object,
                                      const ChoiceLayout& layout,
                                      i32 index,
                                      bool selected) {
    auto array = reference_field(machine, object, layout.selected);
    if (!array) return std::unexpected(array.error());
    return machine.heap().set_element(*array, static_cast<usize>(index),
                                      Value::from_int(selected ? 1 : 0));
}

[[nodiscard]] Result<i32> selected_index(Machine& machine,
                                         ObjectRef object,
                                         const ChoiceLayout& layout) {
    const std::array<usize, 3> fields {
        layout.type,
        layout.count,
        layout.selected,
    };
    auto values = field_values(machine, object, fields);
    if (!values) return std::unexpected(values.error());
    auto type = (*values)[0U].as_int();
    auto count = (*values)[1U].as_int();
    auto selected = (*values)[2U].as_reference();
    if (!type || !count || !selected) {
        return fail(ErrorCode::invalid_state,
                    "Choice selection state is invalid");
    }
    // MIDP Choice.getSelectedIndex() is undefined for MULTIPLE choices and
    // the reference implementation returns -1 even when flags are selected.
    if (*type == kChoiceMultiple) return -1;
    auto flags = machine.heap().read_boolean_array(*selected);
    if (!flags) return std::unexpected(flags.error());
    if (*count < 0 || static_cast<usize>(*count) > flags->size()) {
        return fail(ErrorCode::invalid_state,
                    "Choice selection storage is truncated");
    }
    for (i32 index = 0; index < *count; ++index) {
        if ((*flags)[static_cast<usize>(index)] != 0U) return index;
    }
    return -1;
}

[[nodiscard]] Result<UiBridgeEvent> choice_event(Machine& machine,
                                                 ObjectRef object,
                                                 const ChoiceLayout& layout,
                                                 i32 index) {
    auto checked = check_index(machine, object, layout, index);
    if (!checked) return std::unexpected(checked.error());
    auto component_id = ensure_component_id(machine, object, layout);
    if (!component_id) return std::unexpected(component_id.error());

    const std::array<usize, 5> fields {
        layout.strings,
        layout.images,
        layout.selected,
        layout.fit_policy,
        layout.type,
    };
    auto values = field_values(machine, object, fields);
    if (!values) return std::unexpected(values.error());
    auto strings = (*values)[0U].as_reference();
    auto images = (*values)[1U].as_reference();
    auto selections = (*values)[2U].as_reference();
    auto fit = (*values)[3U].as_int();
    auto type = (*values)[4U].as_int();
    if (!strings || !images || !selections || !fit || !type) {
        return fail(ErrorCode::invalid_state,
                    "Choice event state is invalid");
    }

    const usize element_index = static_cast<usize>(index);
    auto text_value = machine.heap().element(*strings, element_index);
    auto image_value = machine.heap().element(*images, element_index);
    auto selected_value = machine.heap().element(*selections, element_index);
    if (!text_value) return std::unexpected(text_value.error());
    if (!image_value) return std::unexpected(image_value.error());
    if (!selected_value) return std::unexpected(selected_value.error());
    auto text = text_value->as_reference();
    auto image = image_value->as_reference();
    auto selected = selected_value->as_int();
    if (!text || !image || !selected) {
        return fail(ErrorCode::invalid_state,
                    "Choice event element state is invalid");
    }
    auto encoded = string_utf8(machine, *text);
    if (!encoded) return std::unexpected(encoded.error());

    i32 image_key = 0;
    // Eight index bits keep the synthetic key inside signed Int32 for all
    // 64 app namespaces supported by the iOS host.
    if (!image->is_null() && index >= 0 && index < 256) {
        const i64 packed =
            (static_cast<i64>(*component_id) << 8U) |
            static_cast<i64>(index);
        if (packed > 0 &&
            packed <= static_cast<i64>(std::numeric_limits<i32>::max())) {
            image_key = -static_cast<i32>(packed);
        }
    }
    return UiBridgeEvent {
        .kind = kEventChoiceElement,
        .component_id = *component_id,
        .component_type = component_type(*type),
        .index = index,
        .arguments = {*selected != 0 ? 1 : 0, 0, *fit, image_key},
        .text = std::move(*encoded),
    };
}

[[nodiscard]] Status emit_choice_element(Machine& machine,
                                         ObjectRef object,
                                         const ChoiceLayout& layout,
                                         i32 index) {
    auto event = choice_event(machine, object, layout, index);
    if (!event) return std::unexpected(event.error());
    machine.emit_ui_event(std::move(*event));
    return {};
}

[[nodiscard]] Status initialize_group(Machine& machine,
                                      ObjectRef group,
                                      ObjectRef label,
                                      i32 choice_type) {
    auto valid = validate_choice_type(false, choice_type);
    if (!valid) return valid;
    auto component_id = ensure_component_id(machine, group, kGroupLayout);
    if (!component_id) return std::unexpected(component_id.error());
    if (label.is_null()) {
        auto empty = create_string(machine, {});
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
        Value::from_int(component_type(choice_type)),
        Value::from_reference(label),
        Value::from_int(0),
        Value::from_int(0),
        Value::from_reference({}),
        Value::from_int(0),
        Value::from_reference({}),
        Value::from_int(-1),
        Value::from_int(-1),
    };
    auto initialized = set_field_values(machine, group, fields, values);
    if (!initialized) return initialized;
    auto commands = ensure_array(machine, group, kItemCommandsField,
                                 "[Ljavax/microedition/lcdui/Command;", 4U,
                                 Value::from_reference({}));
    if (!commands) return std::unexpected(commands.error());
    return initialize_choice_storage(machine, group, kGroupLayout,
                                     choice_type, 4U);
}

[[nodiscard]] Result<ObjectRef> list_select_command(Machine& machine) {
    auto field = machine.class_states().resolve_field(
        "javax/microedition/lcdui/List", "SELECT_COMMAND",
        "Ljavax/microedition/lcdui/Command;", true);
    if (!field) return std::unexpected(field.error());
    auto current = machine.class_states().static_field(*field);
    if (!current) return std::unexpected(current.error());
    auto command = current->as_reference();
    if (!command) return std::unexpected(command.error());
    if (!command->is_null()) return *command;

    auto label = create_string(machine, std::u16string(u"Select"));
    if (!label) return std::unexpected(label.error());
    auto allocated = machine.class_states().allocate_instance(
        machine.heap(), "javax/microedition/lcdui/Command");
    if (!allocated) return std::unexpected(allocated.error());
    const i32 id = machine.allocate_ui_component_id();
    if (id == 0) {
        return fail_java("java/lang/OutOfMemoryError",
                         "List select command ID space is exhausted");
    }
    constexpr std::array<usize, 6> fields {
        kCommandIdField,
        kCommandLabelField,
        kCommandLongLabelField,
        kCommandTypeField,
        kCommandPriorityField,
        kCommandOwnerItemField,
    };
    const std::array<Value, 6> values {
        Value::from_int(id),
        Value::from_reference(*label),
        Value::from_reference(*label),
        Value::from_int(1),
        Value::from_int(0),
        Value::from_int(0),
    };
    auto initialized = set_field_values(
        machine, *allocated, fields, values);
    if (!initialized) return std::unexpected(initialized.error());
    auto registered = machine.register_ui_component(id, *allocated);
    if (!registered) return std::unexpected(registered.error());
    auto stored = machine.class_states().set_static_field(
        *field, Value::from_reference(*allocated));
    if (!stored) return std::unexpected(stored.error());
    return *allocated;
}

[[nodiscard]] Status initialize_list(Machine& machine,
                                     ObjectRef list,
                                     ObjectRef title,
                                     i32 choice_type) {
    auto valid = validate_choice_type(true, choice_type);
    if (!valid) return valid;
    if (title.is_null()) {
        auto empty = create_string(machine, {});
        if (!empty) return std::unexpected(empty.error());
        title = *empty;
    }
    const i32 screen_id = machine.allocate_ui_component_id();
    if (screen_id == 0) {
        return fail_java("java/lang/OutOfMemoryError",
                         "List screen ID space is exhausted");
    }
    auto screen_registered = machine.register_ui_component(screen_id, list);
    if (!screen_registered) return screen_registered;
    constexpr std::array<usize, 8> fields {
        kDisplayableIdField,
        kDisplayableTypeField,
        kDisplayableTitleField,
        kDisplayableListenerField,
        kDisplayableCommandCountField,
        kDisplayableShownField,
        7U,
        8U,
    };
    const std::array<Value, 8> values {
        Value::from_int(screen_id),
        Value::from_int(kTypeForm),
        Value::from_reference(title),
        Value::from_reference({}),
        Value::from_int(0),
        Value::from_int(0),
        Value::from_reference({}),
        Value::from_int(0),
    };
    auto initialized = set_field_values(machine, list, fields, values);
    if (!initialized) return initialized;
    auto commands = ensure_array(machine, list, kDisplayableCommandsField,
                                 "[Ljavax/microedition/lcdui/Command;", 4U,
                                 Value::from_reference({}));
    if (!commands) return std::unexpected(commands.error());
    auto storage = initialize_choice_storage(machine, list, kListLayout,
                                             choice_type, 4U);
    if (!storage) return storage;
    auto select_command = list_select_command(machine);
    if (!select_command) return std::unexpected(select_command.error());
    auto select_stored = set_reference_field(machine, list,
                                             kListLayout.select_command,
                                             *select_command);
    if (!select_stored) return select_stored;
    auto component_id = ensure_component_id(machine, list, kListLayout);
    if (!component_id) return std::unexpected(component_id.error());

    auto title_text = string_utf8(machine, title);
    if (!title_text) return std::unexpected(title_text.error());
    machine.emit_ui_event(UiBridgeEvent {
        .kind = kEventScreenCreated,
        .component_id = screen_id,
        .component_type = kTypeForm,
        .text = *title_text,
    });
    machine.emit_ui_event(UiBridgeEvent {
        .kind = kEventScreenUpdated,
        .component_id = screen_id,
        .component_type = kTypeForm,
        .arguments = {1, 0, 0, kScreenMetadata},
        .text = *title_text,
    });
    machine.emit_ui_event(UiBridgeEvent {
        .kind = kEventItemCreated,
        .component_id = *component_id,
        .parent_id = screen_id,
        .component_type = component_type(choice_type),
    });
    machine.emit_ui_event(UiBridgeEvent {
        .kind = kEventItemShown,
        .component_id = *component_id,
        .parent_id = screen_id,
        .component_type = component_type(choice_type),
    });
    return {};
}

[[nodiscard]] Result<i32> append_choice(Machine& machine,
                                        ObjectRef object,
                                        const ChoiceLayout& layout,
                                        ObjectRef text,
                                        ObjectRef image) {
    if (text.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Choice text is null");
    }
    auto count = choice_count(machine, object, layout);
    if (!count) return std::unexpected(count.error());
    const usize needed = static_cast<usize>(*count) + 1U;
    auto strings = ensure_array(machine, object, layout.strings,
                                "[Ljava/lang/String;", needed,
                                Value::from_reference({}));
    auto images = ensure_array(machine, object, layout.images,
                               "[Ljavax/microedition/lcdui/Image;", needed,
                               Value::from_reference({}));
    auto fonts = ensure_array(machine, object, layout.fonts,
                              "[Ljavax/microedition/lcdui/Font;", needed,
                              Value::from_reference({}));
    auto selected = ensure_array(machine, object, layout.selected,
                                 "[Z", needed, Value::from_int(0));
    if (!strings) return std::unexpected(strings.error());
    if (!images) return std::unexpected(images.error());
    if (!fonts) return std::unexpected(fonts.error());
    if (!selected) return std::unexpected(selected.error());
    auto first = machine.heap().set_element(
        *strings, static_cast<usize>(*count), Value::from_reference(text));
    auto second = machine.heap().set_element(
        *images, static_cast<usize>(*count), Value::from_reference(image));
    auto font_stored = machine.heap().set_element(
        *fonts, static_cast<usize>(*count), Value::from_reference({}));
    if (!first) return std::unexpected(first.error());
    if (!second) return std::unexpected(second.error());
    if (!font_stored) return std::unexpected(font_stored.error());
    auto choice_type = int_field(machine, object, layout.type);
    if (!choice_type) return std::unexpected(choice_type.error());
    const bool initially_selected =
        *count == 0 && *choice_type != kChoiceMultiple;
    auto third = machine.heap().set_element(
        *selected, static_cast<usize>(*count),
        Value::from_int(initially_selected ? 1 : 0));
    auto fourth = set_int_field(machine, object, layout.count, *count + 1);
    if (!third) return std::unexpected(third.error());
    if (!fourth) return std::unexpected(fourth.error());
    auto emitted = emit_choice_element(machine, object, layout, *count);
    if (!emitted) return std::unexpected(emitted.error());
    return *count;
}

[[nodiscard]] Status insert_choice(Machine& machine,
                                   ObjectRef object,
                                   const ChoiceLayout& layout,
                                   i32 index,
                                   ObjectRef text,
                                   ObjectRef image) {
    auto checked = check_index(machine, object, layout, index, true);
    if (!checked) return checked;
    if (text.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Choice text is null");
    }
    auto count = choice_count(machine, object, layout);
    if (!count) return std::unexpected(count.error());
    const usize needed = static_cast<usize>(*count) + 1U;
    auto strings = ensure_array(machine, object, layout.strings,
                                "[Ljava/lang/String;", needed,
                                Value::from_reference({}));
    auto images = ensure_array(machine, object, layout.images,
                               "[Ljavax/microedition/lcdui/Image;", needed,
                               Value::from_reference({}));
    auto fonts = ensure_array(machine, object, layout.fonts,
                              "[Ljavax/microedition/lcdui/Font;", needed,
                              Value::from_reference({}));
    auto selected = ensure_array(machine, object, layout.selected,
                                 "[Z", needed, Value::from_int(0));
    if (!strings) return std::unexpected(strings.error());
    if (!images) return std::unexpected(images.error());
    if (!fonts) return std::unexpected(fonts.error());
    if (!selected) return std::unexpected(selected.error());
    const usize shifted = static_cast<usize>(*count - index);
    if (shifted != 0U) {
        for (const ObjectRef array : {*strings, *images, *fonts, *selected}) {
            auto moved = machine.heap().copy_array_range(
                array,
                static_cast<usize>(index),
                array,
                static_cast<usize>(index + 1),
                shifted);
            if (!moved) return moved;
        }
    }
    auto first = machine.heap().set_element(
        *strings, static_cast<usize>(index), Value::from_reference(text));
    auto second = machine.heap().set_element(
        *images, static_cast<usize>(index), Value::from_reference(image));
    auto font_stored = machine.heap().set_element(
        *fonts, static_cast<usize>(index), Value::from_reference({}));
    auto third = machine.heap().set_element(
        *selected, static_cast<usize>(index), Value::from_int(0));
    auto fourth = set_int_field(machine, object, layout.count, *count + 1);
    if (!first) return first;
    if (!second) return second;
    if (!font_stored) return font_stored;
    if (!third) return third;
    if (!fourth) return fourth;
    if (*count == 0) {
        auto type = int_field(machine, object, layout.type);
        if (!type) return std::unexpected(type.error());
        if (*type != kChoiceMultiple) {
            auto selected_stored = set_selected_raw(machine, object, layout,
                                                    index, true);
            if (!selected_stored) return selected_stored;
        }
    }
    return emit_choice_elements(machine, object);
}

[[nodiscard]] Status delete_choice(Machine& machine,
                                   ObjectRef object,
                                   const ChoiceLayout& layout,
                                   i32 index) {
    auto checked = check_index(machine, object, layout, index);
    if (!checked) return checked;
    auto count = choice_count(machine, object, layout);
    if (!count) return std::unexpected(count.error());
    auto strings = reference_field(machine, object, layout.strings);
    auto images = reference_field(machine, object, layout.images);
    auto fonts = reference_field(machine, object, layout.fonts);
    auto selected = reference_field(machine, object, layout.selected);
    if (!strings) return std::unexpected(strings.error());
    if (!images) return std::unexpected(images.error());
    if (!fonts) return std::unexpected(fonts.error());
    if (!selected) return std::unexpected(selected.error());
    const usize shifted = static_cast<usize>(*count - index - 1);
    if (shifted != 0U) {
        for (const ObjectRef array : {*strings, *images, *fonts, *selected}) {
            auto moved = machine.heap().copy_array_range(
                array,
                static_cast<usize>(index + 1),
                array,
                static_cast<usize>(index),
                shifted);
            if (!moved) return moved;
        }
    }
    auto first = machine.heap().set_element(
        *strings, static_cast<usize>(*count - 1), Value::from_reference({}));
    auto second = machine.heap().set_element(
        *images, static_cast<usize>(*count - 1), Value::from_reference({}));
    auto font_cleared = machine.heap().set_element(
        *fonts, static_cast<usize>(*count - 1), Value::from_reference({}));
    auto third = machine.heap().set_element(
        *selected, static_cast<usize>(*count - 1), Value::from_int(0));
    auto fourth = set_int_field(machine, object, layout.count, *count - 1);
    if (!first) return first;
    if (!second) return second;
    if (!font_cleared) return font_cleared;
    if (!third) return third;
    if (!fourth) return fourth;
    auto type = int_field(machine, object, layout.type);
    if (!type) return std::unexpected(type.error());
    if (*count - 1 > 0 && *type != kChoiceMultiple) {
        auto current = selected_index(machine, object, layout);
        if (!current) return std::unexpected(current.error());
        if (*current < 0) {
            const i32 replacement = std::min(index, *count - 2);
            auto selected_stored = set_selected_raw(
                machine, object, layout, replacement, true);
            if (!selected_stored) return selected_stored;
        }
    }
    auto component_id = ensure_component_id(machine, object, layout);
    if (!component_id) return std::unexpected(component_id.error());
    machine.emit_ui_event(UiBridgeEvent {
        .kind = kEventChoiceDeleted,
        .component_id = *component_id,
        .index = index,
    });
    return emit_choice_elements(machine, object);
}

[[nodiscard]] Status set_selected(Machine& machine,
                                  ObjectRef object,
                                  const ChoiceLayout& layout,
                                  i32 index,
                                  bool selected) {
    auto checked = check_index(machine, object, layout, index);
    if (!checked) return checked;
    const std::array<usize, 3> fields {
        layout.type,
        layout.count,
        layout.selected,
    };
    auto values = field_values(machine, object, fields);
    if (!values) return std::unexpected(values.error());
    auto type = (*values)[0U].as_int();
    auto count = (*values)[1U].as_int();
    auto array = (*values)[2U].as_reference();
    if (!type || !count || !array) {
        return fail(ErrorCode::invalid_state,
                    "Choice selection state is invalid");
    }
    if (*type == kChoiceMultiple) {
        auto stored = machine.heap().set_element(
            *array, static_cast<usize>(index),
            Value::from_int(selected ? 1 : 0));
        if (!stored) return stored;
    } else if (selected) {
        auto cleared = machine.heap().fill_array_range(
            *array, 0U, static_cast<usize>(*count), Value::from_int(0));
        if (!cleared) return cleared;
        auto stored = machine.heap().set_element(
            *array, static_cast<usize>(index), Value::from_int(1));
        if (!stored) return stored;
    }
    return emit_choice_elements(machine, object);
}

[[nodiscard]] Status initialize_from_arrays(Machine& machine,
                                            ObjectRef object,
                                            const ChoiceLayout& layout,
                                            ObjectRef strings,
                                            ObjectRef images) {
    if (strings.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Choice String array is null");
    }
    auto count = machine.heap().array_length(strings);
    if (!count) return std::unexpected(count.error());
    if (!images.is_null()) {
        auto image_count = machine.heap().array_length(images);
        if (!image_count) return std::unexpected(image_count.error());
        if (*image_count != *count) {
            return fail_java("java/lang/IllegalArgumentException",
                             "Choice image array length does not match text array");
        }
    }

    // Preserve the constructor's NullPointerException semantics before
    // publishing any part of the source arrays into the Choice storage.
    for (usize index = 0; index < *count; ++index) {
        auto text_value = machine.heap().element(strings, index);
        if (!text_value) return std::unexpected(text_value.error());
        auto text = text_value->as_reference();
        if (!text) return std::unexpected(text.error());
        if (text->is_null()) {
            return fail_java("java/lang/NullPointerException",
                             "Choice text is null");
        }
    }

    if (*count == 0U) return {};

    auto destination_strings = ensure_array(
        machine, object, layout.strings, "[Ljava/lang/String;", *count,
        Value::from_reference({}));
    auto destination_images = ensure_array(
        machine, object, layout.images, "[Ljavax/microedition/lcdui/Image;",
        *count, Value::from_reference({}));
    auto destination_fonts = ensure_array(
        machine, object, layout.fonts, "[Ljavax/microedition/lcdui/Font;",
        *count, Value::from_reference({}));
    auto destination_selected = ensure_array(
        machine, object, layout.selected, "[Z", *count, Value::from_int(0));
    if (!destination_strings) {
        return std::unexpected(destination_strings.error());
    }
    if (!destination_images) {
        return std::unexpected(destination_images.error());
    }
    if (!destination_fonts) {
        return std::unexpected(destination_fonts.error());
    }
    if (!destination_selected) {
        return std::unexpected(destination_selected.error());
    }

    auto strings_copied = machine.heap().copy_array_range(
        strings, 0U, *destination_strings, 0U, *count);
    if (!strings_copied) return strings_copied;
    if (!images.is_null()) {
        auto images_copied = machine.heap().copy_array_range(
            images, 0U, *destination_images, 0U, *count);
        if (!images_copied) return images_copied;
    }

    auto type = int_field(machine, object, layout.type);
    if (!type) return std::unexpected(type.error());
    if (*type != kChoiceMultiple) {
        auto selected = machine.heap().set_element(
            *destination_selected, 0U, Value::from_int(1));
        if (!selected) return selected;
    }
    auto count_stored = set_int_field(
        machine, object, layout.count, static_cast<i32>(*count));
    if (!count_stored) return count_stored;

    for (usize index = 0; index < *count; ++index) {
        auto emitted = emit_choice_element(
            machine, object, layout, static_cast<i32>(index));
        if (!emitted) return emitted;
    }
    return {};
}

void register_choice_methods(NativeMethodRegistry& registry,
                             std::string owner) {
    const bool list = owner == "javax/microedition/lcdui/List";
    const ChoiceLayout layout = list ? kListLayout : kGroupLayout;
    add(registry, owner, "size", "()I",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto count = choice_count(machine, *object, layout);
            if (!count) return std::unexpected(count.error());
            return std::optional<Value>(Value::from_int(*count));
        });
    add(registry, owner, "getString", "(I)Ljava/lang/String;",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            auto text = choice_string(machine, *object, layout, *index);
            if (!text) return std::unexpected(text.error());
            return std::optional<Value>(Value::from_reference(*text));
        });
    add(registry, owner, "getImage",
        "(I)Ljavax/microedition/lcdui/Image;",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            auto image = choice_image(machine, *object, layout, *index);
            if (!image) return std::unexpected(image.error());
            return std::optional<Value>(Value::from_reference(*image));
        });
    add(registry, owner, "getFont",
        "(I)Ljavax/microedition/lcdui/Font;",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            auto checked = check_index(machine, *object, layout, *index);
            if (!checked) return std::unexpected(checked.error());
            auto fonts = reference_field(machine, *object, layout.fonts);
            if (!fonts) return std::unexpected(fonts.error());
            auto value = machine.heap().element(
                *fonts, static_cast<usize>(*index));
            if (!value) return std::unexpected(value.error());
            auto font = value->as_reference();
            if (!font) return std::unexpected(font.error());
            if (font->is_null()) {
                auto fallback = create_default_font(machine);
                if (!fallback) return std::unexpected(fallback.error());
                font = *fallback;
            }
            return std::optional<Value>(Value::from_reference(*font));
        });
    add(registry, owner, "setFont",
        "(ILjavax/microedition/lcdui/Font;)V",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            auto font = reference_argument(arguments, 2U);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            if (!font) return std::unexpected(font.error());
            auto checked = check_index(machine, *object, layout, *index);
            if (!checked) return std::unexpected(checked.error());
            if (!font->is_null()) {
                auto valid = machine.object_is_instance(
                    *font, "javax/microedition/lcdui/Font");
                if (!valid) return std::unexpected(valid.error());
                if (!*valid) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Choice font is invalid");
                }
            }
            auto fonts = reference_field(machine, *object, layout.fonts);
            if (!fonts) return std::unexpected(fonts.error());
            auto stored = machine.heap().set_element(
                *fonts, static_cast<usize>(*index),
                Value::from_reference(*font));
            if (!stored) return std::unexpected(stored.error());
            auto emitted = emit_choice_element(machine, *object,
                                               layout, *index);
            if (!emitted) return std::unexpected(emitted.error());
            return std::optional<Value> {};
        });
    add(registry, owner, "append",
        "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto text = reference_argument(arguments, 1U, false);
            auto image = reference_argument(arguments, 2U);
            if (!object) return std::unexpected(object.error());
            if (!text) return std::unexpected(text.error());
            if (!image) return std::unexpected(image.error());
            auto index = append_choice(machine, *object, layout, *text, *image);
            if (!index) return std::unexpected(index.error());
            return std::optional<Value>(Value::from_int(*index));
        });
    add(registry, owner, "insert",
        "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            auto text = reference_argument(arguments, 2U, false);
            auto image = reference_argument(arguments, 3U);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            if (!text) return std::unexpected(text.error());
            if (!image) return std::unexpected(image.error());
            auto inserted = insert_choice(machine, *object, layout,
                                          *index, *text, *image);
            if (!inserted) return std::unexpected(inserted.error());
            return std::optional<Value> {};
        });
    add(registry, owner, "delete", "(I)V",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            auto deleted = delete_choice(machine, *object, layout, *index);
            if (!deleted) return std::unexpected(deleted.error());
            return std::optional<Value> {};
        });
    add(registry, owner, "deleteAll", "()V",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto count = choice_count(machine, *object, layout);
            if (!count) return std::unexpected(count.error());
            for (i32 index = *count - 1; index >= 0; --index) {
                auto deleted = delete_choice(machine, *object, layout, index);
                if (!deleted) return std::unexpected(deleted.error());
            }
            return std::optional<Value> {};
        });
    add(registry, owner, "set",
        "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            auto text = reference_argument(arguments, 2U, false);
            auto image = reference_argument(arguments, 3U);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            if (!text) return std::unexpected(text.error());
            if (!image) return std::unexpected(image.error());
            auto checked = check_index(machine, *object, layout, *index);
            if (!checked) return std::unexpected(checked.error());
            auto strings = reference_field(machine, *object, layout.strings);
            auto images = reference_field(machine, *object, layout.images);
            if (!strings) return std::unexpected(strings.error());
            if (!images) return std::unexpected(images.error());
            auto first = machine.heap().set_element(
                *strings, static_cast<usize>(*index),
                Value::from_reference(*text));
            auto second = machine.heap().set_element(
                *images, static_cast<usize>(*index),
                Value::from_reference(*image));
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            auto emitted = emit_choice_element(machine, *object,
                                               layout, *index);
            if (!emitted) return std::unexpected(emitted.error());
            return std::optional<Value> {};
        });
    add(registry, owner, "isSelected", "(I)Z",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            auto selected = selected_at(machine, *object, layout, *index);
            if (!selected) return std::unexpected(selected.error());
            return std::optional<Value>(Value::from_int(*selected ? 1 : 0));
        });
    add(registry, owner, "getSelectedIndex", "()I",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto index = selected_index(machine, *object, layout);
            if (!index) return std::unexpected(index.error());
            return std::optional<Value>(Value::from_int(*index));
        });
    add(registry, owner, "getSelectedFlags", "([Z)I",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto destination = reference_argument(arguments, 1U, false);
            if (!object) return std::unexpected(object.error());
            if (!destination) return std::unexpected(destination.error());
            const std::array<usize, 2> fields {
                layout.count,
                layout.selected,
            };
            auto values = field_values(machine, *object, fields);
            if (!values) return std::unexpected(values.error());
            auto count = (*values)[0U].as_int();
            auto selected_array = (*values)[1U].as_reference();
            if (!count || !selected_array) {
                return fail(ErrorCode::invalid_state,
                            "Choice selection state is invalid");
            }
            auto length = machine.heap().array_length(*destination);
            if (!length) return std::unexpected(length.error());
            if (*length < static_cast<usize>(*count)) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "selection flags array is too short");
            }
            auto flags = machine.heap().read_boolean_array(*selected_array);
            if (!flags) return std::unexpected(flags.error());
            if (*count < 0 || static_cast<usize>(*count) > flags->size()) {
                return fail(ErrorCode::invalid_state,
                            "Choice selection storage is truncated");
            }
            i32 selected_count = 0;
            for (i32 index = 0; index < *count; ++index) {
                if ((*flags)[static_cast<usize>(index)] != 0U) {
                    ++selected_count;
                }
            }
            auto stored = machine.heap().write_boolean_array(
                *destination, 0U,
                std::span<const u8>(flags->data(), static_cast<usize>(*count)));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_int(selected_count));
        });
    add(registry, owner, "setSelectedIndex", "(IZ)V",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto index = int_argument(arguments, 1U);
            auto selected = int_argument(arguments, 2U);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            if (!selected) return std::unexpected(selected.error());
            auto stored = set_selected(machine, *object, layout,
                                       *index, *selected != 0);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, owner, "setSelectedFlags", "([Z)V",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto source = reference_argument(arguments, 1U, false);
            if (!object) return std::unexpected(object.error());
            if (!source) return std::unexpected(source.error());
            const std::array<usize, 3> fields {
                layout.count,
                layout.type,
                layout.selected,
            };
            auto values = field_values(machine, *object, fields);
            if (!values) return std::unexpected(values.error());
            auto count = (*values)[0U].as_int();
            auto type = (*values)[1U].as_int();
            auto selected_array = (*values)[2U].as_reference();
            if (!count || !type || !selected_array) {
                return fail(ErrorCode::invalid_state,
                            "Choice selection state is invalid");
            }
            auto source_flags = machine.heap().read_boolean_array(*source);
            if (!source_flags) return std::unexpected(source_flags.error());
            if (*count < 0 || source_flags->size() < static_cast<usize>(*count)) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "selection flags array is too short");
            }
            i32 first_selected = -1;
            for (i32 index = 0; index < *count; ++index) {
                if ((*source_flags)[static_cast<usize>(index)] != 0U &&
                    first_selected < 0) {
                    first_selected = index;
                }
            }
            std::vector<u8> normalized(static_cast<usize>(*count), 0U);
            if (*type == kChoiceMultiple) {
                std::copy_n(source_flags->begin(), static_cast<usize>(*count),
                            normalized.begin());
            }
            if (*type != kChoiceMultiple && *count > 0) {
                if (first_selected < 0) first_selected = 0;
                normalized[static_cast<usize>(first_selected)] = 1U;
            }
            auto stored = machine.heap().write_boolean_array(
                *selected_array, 0U, normalized);
            if (!stored) return std::unexpected(stored.error());
            auto emitted = emit_choice_elements(machine, *object);
            if (!emitted) return std::unexpected(emitted.error());
            return std::optional<Value> {};
        });
    add(registry, owner, "setFitPolicy", "(I)V",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto policy = int_argument(arguments, 1U);
            if (!object) return std::unexpected(object.error());
            if (!policy) return std::unexpected(policy.error());
            if (*policy < 0 || *policy > 2) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "invalid Choice fit policy");
            }
            auto stored = set_int_field(machine, *object,
                                        layout.fit_policy, *policy);
            if (!stored) return std::unexpected(stored.error());
            auto emitted = emit_choice_elements(machine, *object);
            if (!emitted) return std::unexpected(emitted.error());
            return std::optional<Value> {};
        });
    add(registry, owner, "getFitPolicy", "()I",
        [layout](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto policy = int_field(machine, *object, layout.fit_policy);
            if (!policy) return std::unexpected(policy.error());
            return std::optional<Value>(Value::from_int(*policy));
        });
}

} // namespace

Status emit_choice_elements(Machine& machine, ObjectRef choice) {
    auto layout = layout_for(machine, choice);
    if (!layout) return std::unexpected(layout.error());
    auto component_id = ensure_component_id(machine, choice, *layout);
    if (!component_id) return std::unexpected(component_id.error());

    const std::array<usize, 6> fields {
        layout->strings,
        layout->images,
        layout->selected,
        layout->fit_policy,
        layout->type,
        layout->count,
    };
    auto values = field_values(machine, choice, fields);
    if (!values) return std::unexpected(values.error());
    auto strings = (*values)[0U].as_reference();
    auto images = (*values)[1U].as_reference();
    auto selections = (*values)[2U].as_reference();
    auto fit = (*values)[3U].as_int();
    auto type = (*values)[4U].as_int();
    auto count = (*values)[5U].as_int();
    if (!strings || !images || !selections || !fit || !type || !count ||
        strings->is_null() || images->is_null() || selections->is_null() ||
        *count < 0) {
        return fail(ErrorCode::invalid_state,
                    "Choice element storage is invalid");
    }

    auto string_values = machine.heap().read_reference_array(*strings);
    auto image_values = machine.heap().read_reference_array(*images);
    auto selected_values = machine.heap().read_boolean_array(*selections);
    if (!string_values) return std::unexpected(string_values.error());
    if (!image_values) return std::unexpected(image_values.error());
    if (!selected_values) return std::unexpected(selected_values.error());
    const usize element_count = static_cast<usize>(*count);
    if (element_count > string_values->size() ||
        element_count > image_values->size() ||
        element_count > selected_values->size()) {
        return fail(ErrorCode::invalid_state,
                    "Choice element storage is truncated");
    }

    for (usize element_index = 0U; element_index < element_count;
         ++element_index) {
        const ObjectRef text = (*string_values)[element_index];
        const ObjectRef image = (*image_values)[element_index];
        auto encoded = string_utf8(machine, text);
        if (!encoded) return std::unexpected(encoded.error());

        i32 image_key = 0;
        if (!image.is_null() && element_index < 256U) {
            const i64 packed =
                (static_cast<i64>(*component_id) << 8U) |
                static_cast<i64>(element_index);
            if (packed > 0 &&
                packed <= static_cast<i64>(std::numeric_limits<i32>::max())) {
                image_key = -static_cast<i32>(packed);
            }
        }
        machine.emit_ui_event(UiBridgeEvent {
            .kind = kEventChoiceElement,
            .component_id = *component_id,
            .component_type = component_type(*type),
            .index = static_cast<i32>(element_index),
            .arguments = {
                (*selected_values)[element_index] != 0U ? 1 : 0,
                0,
                *fit,
                image_key,
            },
            .text = std::move(*encoded),
        });
    }
    return {};
}

Status handle_choice_action(Machine& machine,
                            ObjectRef choice,
                            i32 element_index,
                            bool selected) {
    auto layout = layout_for(machine, choice);
    if (!layout) return std::unexpected(layout.error());
    return set_selected(machine, choice, *layout,
                        element_index, selected);
}

Result<std::optional<ObjectRef>> implicit_choice_command(
    Machine& machine,
    ObjectRef choice) {
    auto layout = layout_for(machine, choice);
    if (!layout) return std::unexpected(layout.error());
    if (!layout->list) return std::optional<ObjectRef> {};

    auto type = int_field(machine, choice, layout->type);
    if (!type) return std::unexpected(type.error());
    if (*type != kChoiceImplicit) return std::optional<ObjectRef> {};

    auto command = reference_field(machine, choice, layout->select_command);
    if (!command) return std::unexpected(command.error());
    if (command->is_null()) return std::optional<ObjectRef> {};
    return std::optional<ObjectRef>(*command);
}

void register_choice_natives(NativeMethodRegistry& registry) {
    add(registry, "javax/microedition/lcdui/List", "<clinit>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "List class initializer expects no arguments");
            }
            auto command = list_select_command(machine);
            if (!command) return std::unexpected(command.error());
            return std::optional<Value> {};
        });

    const auto group_constructor = [&registry](const char* descriptor,
                                                bool has_arrays) {
        add(registry, "javax/microedition/lcdui/ChoiceGroup", "<init>",
            descriptor,
            [has_arrays](Machine& machine,
                         std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto group = receiver(arguments);
                auto label = reference_argument(arguments, 1U);
                auto type = int_argument(arguments, 2U);
                if (!group) return std::unexpected(group.error());
                if (!label) return std::unexpected(label.error());
                if (!type) return std::unexpected(type.error());
                auto initialized = initialize_group(machine, *group,
                                                    *label, *type);
                if (!initialized) return std::unexpected(initialized.error());
                if (has_arrays) {
                    auto strings = reference_argument(arguments, 3U, false);
                    auto images = reference_argument(arguments, 4U);
                    if (!strings) return std::unexpected(strings.error());
                    if (!images) return std::unexpected(images.error());
                    auto copied = initialize_from_arrays(
                        machine, *group, kGroupLayout, *strings, *images);
                    if (!copied) return std::unexpected(copied.error());
                }
                return std::optional<Value> {};
            });
    };
    group_constructor("(Ljava/lang/String;I)V", false);
    group_constructor(
        "(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V",
        true);

    const auto list_constructor = [&registry](const char* descriptor,
                                               bool has_arrays) {
        add(registry, "javax/microedition/lcdui/List", "<init>", descriptor,
            [has_arrays](Machine& machine,
                         std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto list = receiver(arguments);
                auto title = reference_argument(arguments, 1U);
                auto type = int_argument(arguments, 2U);
                if (!list) return std::unexpected(list.error());
                if (!title) return std::unexpected(title.error());
                if (!type) return std::unexpected(type.error());
                auto initialized = initialize_list(machine, *list,
                                                   *title, *type);
                if (!initialized) return std::unexpected(initialized.error());
                if (has_arrays) {
                    auto strings = reference_argument(arguments, 3U, false);
                    auto images = reference_argument(arguments, 4U);
                    if (!strings) return std::unexpected(strings.error());
                    if (!images) return std::unexpected(images.error());
                    auto copied = initialize_from_arrays(
                        machine, *list, kListLayout, *strings, *images);
                    if (!copied) return std::unexpected(copied.error());
                }
                return std::optional<Value> {};
            });
    };
    list_constructor("(Ljava/lang/String;I)V", false);
    list_constructor(
        "(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V",
        true);

    register_choice_methods(registry, "javax/microedition/lcdui/ChoiceGroup");
    register_choice_methods(registry, "javax/microedition/lcdui/List");

    add(registry, "javax/microedition/lcdui/List", "setSelectCommand",
        "(Ljavax/microedition/lcdui/Command;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto list = receiver(arguments);
            auto command = reference_argument(arguments, 1U);
            if (!list) return std::unexpected(list.error());
            if (!command) return std::unexpected(command.error());
            auto stored = set_reference_field(machine, *list,
                                              kListLayout.select_command,
                                              *command);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

}

} // namespace phoneme::vm
