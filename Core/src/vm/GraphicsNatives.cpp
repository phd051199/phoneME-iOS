#include "GraphicsNatives.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "GraphicsNativeSupport.hpp"
#include "phoneme/vm/NativeMethodRegistry.hpp"

#if defined(__APPLE__)
extern "C" __attribute__((weak)) int phoneme_ios_device_start_vibrate(
    int, std::int64_t) {
    return 0;
}
extern "C" __attribute__((weak)) int phoneme_ios_device_stop_vibrate(void) {
    return 0;
}
extern "C" __attribute__((weak)) int phoneme_ios_device_flash_lights(
    std::int64_t) {
    return 0;
}
#endif

namespace phoneme::vm {
namespace {

using namespace graphics_native;

constexpr usize kFontFaceField = 0;
constexpr usize kFontStyleField = 1;
constexpr usize kFontSizeField = 2;
constexpr usize kDirectGraphicsTargetField = 0;
constexpr i32 kNokiaTypeByte1Gray = 1;
constexpr i32 kNokiaTypeByte1GrayVertical = -1;
constexpr i32 kNokiaTypeByte2Gray = 2;
constexpr i32 kNokiaTypeByte4Gray = 4;
constexpr i32 kNokiaTypeByte8Gray = 8;
constexpr i32 kNokiaTypeByte332Rgb = 332;
constexpr i32 kNokiaTypeUShort4444Argb = 4444;
constexpr i32 kNokiaTypeUShort444Rgb = 444;
constexpr i32 kNokiaTypeUShort555Rgb = 555;
constexpr i32 kNokiaTypeUShort1555Argb = 1555;
constexpr i32 kNokiaTypeUShort565Rgb = 565;
constexpr i32 kNokiaTypeInt888Rgb = 888;
constexpr i32 kNokiaTypeInt8888Argb = 8888;

struct BoundGraphics final {
    u64 graphics_id {0U};
    graphics::GraphicsContext* context {nullptr};
    graphics::Image* target {nullptr};
};

[[nodiscard]] std::span<const char32_t> translated_characters(
    Machine& machine,
    std::span<const char32_t> original,
    std::shared_ptr<const std::vector<char32_t>>& translated) {
    const auto service = machine.translation_service();
    if (!service || !service->enabled()) return original;
    translated = service->lookup_or_request(original);
    if (!translated) return original;
    return *translated;
}

[[nodiscard]] i32 clamped_coordinate(i64 value) noexcept {
    return static_cast<i32>(std::clamp<i64>(
        value,
        std::numeric_limits<i32>::min(),
        std::numeric_limits<i32>::max()));
}

[[nodiscard]] bool wrapping_space(char32_t character) noexcept {
    return character == U' ' || character == U'\t' ||
           character == U'\r' || character == U'\u3000';
}

void trim_wrapping_spaces(std::vector<char32_t>& line) {
    while (!line.empty() && wrapping_space(line.front())) {
        line.erase(line.begin());
    }
    while (!line.empty() && wrapping_space(line.back())) {
        line.pop_back();
    }
}

[[nodiscard]] std::vector<std::vector<char32_t>> wrap_translated_lines(
    const graphics::Font& font,
    std::span<const char32_t> text,
    i32 maximum_width) {
    std::vector<std::vector<char32_t>> lines;
    std::vector<char32_t> current;
    auto flush = [&]() {
        trim_wrapping_spaces(current);
        if (!current.empty()) lines.push_back(std::move(current));
        current.clear();
    };

    for (const char32_t character : text) {
        if (character == U'\n') {
            flush();
            continue;
        }
        current.push_back(character);
        if (font.chars_width(current) <= maximum_width) continue;

        usize break_index = current.size();
        while (break_index > 1U &&
               !wrapping_space(current[break_index - 1U])) {
            --break_index;
        }
        if (break_index <= 1U) {
            const char32_t overflow = current.back();
            current.pop_back();
            flush();
            current.push_back(overflow);
        } else {
            std::vector<char32_t> remainder(
                current.begin() + static_cast<std::ptrdiff_t>(break_index),
                current.end());
            current.erase(
                current.begin() + static_cast<std::ptrdiff_t>(break_index),
                current.end());
            flush();
            current = std::move(remainder);
            trim_wrapping_spaces(current);
        }
    }
    flush();
    if (lines.empty()) lines.emplace_back();
    return lines;
}

struct TranslatedTextLayout final {
    std::vector<std::vector<char32_t>> lines;
    i32 line_advance {1};
    i64 total_height {1};
};

[[nodiscard]] TranslatedTextLayout layout_translated_text(
    const graphics::Font& font,
    std::span<const char32_t> text,
    i32 maximum_width) {
    auto lines = wrap_translated_lines(
        font, text, std::max(maximum_width, 1));
    const i32 line_advance = std::max(font.height(), 1) + 1;
    const i64 total_height = lines.empty()
        ? font.height()
        : static_cast<i64>(lines.size()) * line_advance - 1;
    return TranslatedTextLayout {
        .lines = std::move(lines),
        .line_advance = line_advance,
        .total_height = std::max<i64>(1, total_height),
    };
}

[[nodiscard]] constexpr graphics::Pixel translated_outline_color(
    graphics::Pixel text_color) noexcept {
    const u32 luminance =
        299U * graphics::red(text_color) +
        587U * graphics::green(text_color) +
        114U * graphics::blue(text_color);
    const u8 component = luminance < 128'000U ? 0xFFU : 0x00U;
    return graphics::argb(
        graphics::alpha(text_color), component, component, component);
}

static_assert(translated_outline_color(0xFF000000U) == 0xFFFFFFFFU);
static_assert(translated_outline_color(0xFFFFFFFFU) == 0xFF000000U);

[[nodiscard]] Status draw_outlined_translated_line(
    graphics::Image& target,
    const graphics::GraphicsContext& context,
    std::span<const char32_t> text,
    i32 x,
    i32 y,
    i32 anchor) {
    graphics::GraphicsContext outline_context = context;
    outline_context.color = translated_outline_color(context.color);
    constexpr std::array<std::pair<i32, i32>, 8> offsets {{
        {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
        {1, 0}, {-1, 1}, {0, 1}, {1, 1},
    }};
    for (const auto [offset_x, offset_y] : offsets) {
        auto outlined = graphics::draw_text(
            target,
            outline_context,
            text,
            x + offset_x,
            y + offset_y,
            anchor);
        if (!outlined) return outlined;
    }
    return graphics::draw_text(target, context, text, x, y, anchor);
}

[[nodiscard]] Status draw_translated_text(
    graphics::Image& target,
    const graphics::GraphicsContext& context,
    std::span<const char32_t> text,
    bool translated,
    i32 x,
    i32 y,
    i32 anchor) {
    if (!translated) {
        return graphics::draw_text(target, context, text, x, y, anchor);
    }
    if (context.clip.width <= 0 || context.clip.height <= 0) {
        return {};
    }

    const i32 resolved_anchor = anchor == 0
        ? graphics::anchor_left | graphics::anchor_top
        : anchor;
    const i32 horizontal = resolved_anchor &
        (graphics::anchor_left | graphics::anchor_right |
         graphics::anchor_hcenter);
    const i32 vertical = resolved_anchor &
        (graphics::anchor_top | graphics::anchor_bottom |
         graphics::anchor_baseline);
    const i64 absolute_x = static_cast<i64>(x) + context.translate_x;
    const i64 absolute_y = static_cast<i64>(y) + context.translate_y;
    const i64 clip_left = context.clip.x;
    const i64 clip_right = clip_left + context.clip.width;

    i64 available_width = context.clip.width;
    if (horizontal == graphics::anchor_left) {
        available_width = clip_right - std::max(absolute_x, clip_left);
    } else if (horizontal == graphics::anchor_right) {
        available_width = std::min(absolute_x, clip_right) - clip_left;
    } else if (horizontal == graphics::anchor_hcenter) {
        available_width = 2 * std::min(
            std::max<i64>(0, absolute_x - clip_left),
            std::max<i64>(0, clip_right - absolute_x));
    }
    const i32 maximum_width = std::max<i32>(
        1, clamped_coordinate(available_width));
    const TranslatedTextLayout layout = layout_translated_text(
        context.font, text, maximum_width);

    i64 top = absolute_y;
    if (vertical == graphics::anchor_bottom) {
        top -= layout.total_height;
    } else if (vertical == graphics::anchor_baseline) {
        top -= context.font.baseline();
    }
    const i64 clip_top = context.clip.y;
    const i64 clip_bottom = clip_top + context.clip.height;
    if (layout.total_height <= context.clip.height) {
        top = std::clamp(
            top,
            clip_top,
            std::max(clip_top, clip_bottom - layout.total_height));
    } else {
        top = std::max(top, clip_top);
    }
    const i32 local_top = clamped_coordinate(top - context.translate_y);
    const i32 line_anchor = horizontal | graphics::anchor_top;

    for (usize index = 0U; index < layout.lines.size(); ++index) {
        const i32 line_y = clamped_coordinate(
            static_cast<i64>(local_top) +
            static_cast<i64>(index) * layout.line_advance);
        auto drawn = draw_outlined_translated_line(
            target, context, layout.lines[index], x, line_y, line_anchor);
        if (!drawn) return drawn;
    }
    return {};
}

[[nodiscard]] Status draw_runtime_translated_text(
    Machine& machine,
    const BoundGraphics& bound,
    std::span<const char32_t> text,
    bool translated,
    i32 x,
    i32 y,
    i32 anchor) {
    if (!translated) {
        return draw_translated_text(
            *bound.target, *bound.context, text, false, x, y, anchor);
    }
    const auto planned = machine.plan_translated_text(
        bound.graphics_id,
        text,
        x,
        y,
        anchor,
        bound.context->font.face(),
        bound.context->font.style(),
        bound.context->font.size(),
        bound.context->clip.x,
        bound.context->clip.y,
        bound.context->clip.width,
        bound.context->clip.height,
        bound.context->translate_x,
        bound.context->translate_y);
    if (planned.suppress) return {};
    return draw_translated_text(
        *bound.target,
        *bound.context,
        text,
        true,
        x,
        planned.y,
        anchor);
}

void add(NativeMethodRegistry& registry,
         std::string owner,
         std::string name,
         std::string descriptor,
         NativeMethod method,
         NativeJitPolicy jit_policy = NativeJitPolicy::conservative,
         CompactNativeMethod compact_method = {}) {
    // MIDP Graphics operations are synchronous, in-memory drawing/state
    // updates. They never wait on external I/O, so compiled Java renderers can
    // invoke them exactly once without pre-deoptimizing back to the
    // interpreter at every draw call.
    if (jit_policy == NativeJitPolicy::conservative &&
        owner == "javax/microedition/lcdui/Graphics") {
        jit_policy = NativeJitPolicy::synchronous_bounded;
    }
    auto registered = registry.register_method(std::move(owner),
                                               std::move(name),
                                               std::move(descriptor),
                                               std::move(method),
                                               jit_policy,
                                               std::move(compact_method));
    if (!registered) {
        std::terminate();
    }
}

[[nodiscard]] Result<graphics::Image*> image_payload(Machine& machine,
                                                     ObjectRef image) {
    auto payload = machine.graphics().image(image.bits);
    if (!payload) return graphics_error(payload.error());
    return *payload;
}

template <typename Arguments>
[[nodiscard]] Result<BoundGraphics> bound_graphics(
    Machine& machine,
    const Arguments& arguments,
    std::string_view operation,
    bool preserve_character_run = false) {
    auto graphics_object = receiver(arguments, operation);
    if (!graphics_object) return std::unexpected(graphics_object.error());
    if (!preserve_character_run) machine.break_character_translation_run();
    auto context = machine.graphics().context(graphics_object->bits);
    if (!context) return graphics_error(context.error());
    auto target = machine.graphics().image((*context)->target_key);
    if (!target) return graphics_error(target.error());
    return BoundGraphics {
        .graphics_id = graphics_object->bits,
        .context = *context,
        .target = *target,
    };
}

[[nodiscard]] Result<ObjectRef> direct_graphics_target(
    Machine& machine,
    std::span<const Value> arguments,
    std::string_view operation) {
    auto object = receiver(arguments, operation);
    if (!object) return std::unexpected(object.error());
    auto class_name = machine.heap().class_name(*object);
    if (!class_name) return std::unexpected(class_name.error());
    if (*class_name == "javax/microedition/lcdui/Graphics") {
        return *object;
    }
    auto target = machine.heap().field(*object, kDirectGraphicsTargetField);
    if (!target) return std::unexpected(target.error());
    auto graphics = target->as_reference();
    if (!graphics || graphics->is_null()) {
        return fail_java("java/lang/IllegalStateException",
                         std::string(operation) +
                             " has no backing Graphics object");
    }
    return *graphics;
}

[[nodiscard]] Result<BoundGraphics> bound_direct_graphics(
    Machine& machine,
    std::span<const Value> arguments,
    std::string_view operation) {
    auto graphics = direct_graphics_target(machine, arguments, operation);
    if (!graphics) return std::unexpected(graphics.error());
    const std::array<Value, 1> forwarded {
        Value::from_reference(*graphics),
    };
    return bound_graphics(machine, forwarded, operation);
}

[[nodiscard]] Result<ObjectRef> invocation_reference(
    Machine& machine,
    const ExecutionResult& result,
    std::string_view operation) {
    if (!result.completed_normally()) {
        if (!result.throwable.has_value()) {
            return fail(ErrorCode::internal_error,
                        std::string(operation) +
                            " failed without a Java throwable");
        }
        auto throwable = machine.heap().class_name(*result.throwable);
        if (!throwable) return std::unexpected(throwable.error());
        return fail_java(*throwable,
                         std::string(operation) + " failed");
    }
    if (!result.return_value.has_value()) {
        return fail(ErrorCode::invalid_state,
                    std::string(operation) + " returned no object");
    }
    return result.return_value->as_reference();
}

[[nodiscard]] Result<graphics::Transform> nokia_transform(i32 manipulation) {
    constexpr i32 kFlipHorizontal = 0x2000;
    constexpr i32 kFlipVertical = 0x4000;
    const i32 flips = manipulation & (kFlipHorizontal | kFlipVertical);
    const i32 rotation = manipulation & ~(kFlipHorizontal | kFlipVertical);
    if (rotation != 0 && rotation != 90 &&
        rotation != 180 && rotation != 270) {
        return fail_java("java/lang/IllegalArgumentException",
                         "DirectGraphics manipulation is invalid");
    }
    if (flips == (kFlipHorizontal | kFlipVertical)) {
        switch (rotation) {
        case 0: return graphics::Transform::rotate_180;
        case 90: return graphics::Transform::rotate_270;
        case 180: return graphics::Transform::none;
        case 270: return graphics::Transform::rotate_90;
        default: break;
        }
    }
    if (flips == kFlipHorizontal) {
        switch (rotation) {
        case 0: return graphics::Transform::mirror;
        case 90: return graphics::Transform::mirror_rotate_90;
        case 180: return graphics::Transform::mirror_rotate_180;
        case 270: return graphics::Transform::mirror_rotate_270;
        default: break;
        }
    }
    if (flips == kFlipVertical) {
        switch (rotation) {
        case 0: return graphics::Transform::mirror_rotate_180;
        case 90: return graphics::Transform::mirror_rotate_270;
        case 180: return graphics::Transform::mirror;
        case 270: return graphics::Transform::mirror_rotate_90;
        default: break;
        }
    }
    switch (rotation) {
    case 0: return graphics::Transform::none;
    case 90: return graphics::Transform::rotate_90;
    case 180: return graphics::Transform::rotate_180;
    case 270: return graphics::Transform::rotate_270;
    default:
        return fail_java("java/lang/IllegalArgumentException",
                         "DirectGraphics manipulation is invalid");
    }
}

[[nodiscard]] bool nokia_short_format(i32 format) noexcept {
    return format == kNokiaTypeUShort4444Argb ||
           format == kNokiaTypeUShort444Rgb ||
           format == kNokiaTypeUShort555Rgb ||
           format == kNokiaTypeUShort1555Argb ||
           format == kNokiaTypeUShort565Rgb;
}

[[nodiscard]] u8 expand4(u32 value) noexcept {
    return static_cast<u8>(value * 17U);
}

[[nodiscard]] u8 expand5(u32 value) noexcept {
    return static_cast<u8>((value << 3U) | (value >> 2U));
}

[[nodiscard]] u8 expand6(u32 value) noexcept {
    return static_cast<u8>((value << 2U) | (value >> 4U));
}

[[nodiscard]] graphics::Pixel nokia_short_to_argb(
    u16 value,
    bool transparency,
    i32 format) noexcept {
    u8 alpha = 255U;
    u8 red = 0U;
    u8 green = 0U;
    u8 blue = 0U;
    if (format == kNokiaTypeUShort4444Argb) {
        alpha = transparency ? expand4((value >> 12U) & 0xFU) : 255U;
        red = expand4((value >> 8U) & 0xFU);
        green = expand4((value >> 4U) & 0xFU);
        blue = expand4(value & 0xFU);
    } else if (format == kNokiaTypeUShort444Rgb) {
        red = expand4((value >> 8U) & 0xFU);
        green = expand4((value >> 4U) & 0xFU);
        blue = expand4(value & 0xFU);
    } else if (format == kNokiaTypeUShort1555Argb) {
        alpha = transparency && (value & 0x8000U) == 0U ? 0U : 255U;
        red = expand5((value >> 10U) & 0x1FU);
        green = expand5((value >> 5U) & 0x1FU);
        blue = expand5(value & 0x1FU);
    } else if (format == kNokiaTypeUShort555Rgb) {
        red = expand5((value >> 10U) & 0x1FU);
        green = expand5((value >> 5U) & 0x1FU);
        blue = expand5(value & 0x1FU);
    } else {
        red = expand5((value >> 11U) & 0x1FU);
        green = expand6((value >> 5U) & 0x3FU);
        blue = expand5(value & 0x1FU);
    }
    return graphics::argb(alpha, red, green, blue);
}

[[nodiscard]] u16 nokia_argb_to_short(graphics::Pixel pixel,
                                      i32 format) noexcept {
    const u32 alpha = graphics::alpha(pixel);
    const u32 red = graphics::red(pixel);
    const u32 green = graphics::green(pixel);
    const u32 blue = graphics::blue(pixel);
    if (format == kNokiaTypeUShort4444Argb) {
        return static_cast<u16>(((alpha >> 4U) << 12U) |
                                ((red >> 4U) << 8U) |
                                ((green >> 4U) << 4U) |
                                (blue >> 4U));
    }
    if (format == kNokiaTypeUShort444Rgb) {
        return static_cast<u16>(((red >> 4U) << 8U) |
                                ((green >> 4U) << 4U) |
                                (blue >> 4U));
    }
    if (format == kNokiaTypeUShort1555Argb) {
        return static_cast<u16>((alpha == 0U ? 0U : 0x8000U) |
                                ((red >> 3U) << 10U) |
                                ((green >> 3U) << 5U) |
                                (blue >> 3U));
    }
    if (format == kNokiaTypeUShort555Rgb) {
        return static_cast<u16>(((red >> 3U) << 10U) |
                                ((green >> 3U) << 5U) |
                                (blue >> 3U));
    }
    return static_cast<u16>(((red >> 3U) << 11U) |
                            ((green >> 2U) << 5U) |
                            (blue >> 3U));
}

[[nodiscard]] Result<usize> checked_array_index(
    i32 offset,
    i32 scan_length,
    i32 row,
    i32 column,
    usize array_length,
    std::string_view operation) {
    const i64 index = static_cast<i64>(offset) +
                      static_cast<i64>(row) * scan_length + column;
    if (index < 0 || static_cast<u64>(index) >= array_length) {
        return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                         std::string(operation) +
                             " pixel slice is outside the array");
    }
    return static_cast<usize>(index);
}

[[nodiscard]] bool nokia_byte_format(i32 format) noexcept {
    return format == kNokiaTypeByte1Gray ||
           format == kNokiaTypeByte1GrayVertical ||
           format == kNokiaTypeByte2Gray ||
           format == kNokiaTypeByte4Gray ||
           format == kNokiaTypeByte8Gray ||
           format == kNokiaTypeByte332Rgb;
}

[[nodiscard]] Result<u8> byte_array_value(
    Machine& machine,
    ObjectRef array,
    i64 index,
    std::string_view operation) {
    auto length = machine.heap().array_length(array);
    if (!length || index < 0 || static_cast<u64>(index) >= *length) {
        return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                         std::string(operation) +
                             " packed pixel index is outside the array");
    }
    auto value = machine.heap().element(array, static_cast<usize>(index));
    if (!value) return std::unexpected(value.error());
    auto integer = value->as_int();
    if (!integer) return std::unexpected(integer.error());
    return static_cast<u8>(static_cast<u32>(*integer) & 0xFFU);
}

[[nodiscard]] Status set_byte_array_value(
    Machine& machine,
    ObjectRef array,
    i64 index,
    u8 value,
    std::string_view operation) {
    auto length = machine.heap().array_length(array);
    if (!length || index < 0 || static_cast<u64>(index) >= *length) {
        return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                         std::string(operation) +
                             " packed pixel index is outside the array");
    }
    return machine.heap().set_element(
        array, static_cast<usize>(index),
        Value::from_int(static_cast<i32>(static_cast<i8>(value))));
}

[[nodiscard]] Result<graphics::Pixel> nokia_byte_to_argb(
    Machine& machine,
    ObjectRef pixels,
    i32 position,
    i32 scan_length,
    i32 format,
    std::string_view operation) {
    if (format == kNokiaTypeByte8Gray ||
        format == kNokiaTypeByte332Rgb) {
        auto source = byte_array_value(machine, pixels, position, operation);
        if (!source) return std::unexpected(source.error());
        if (format == kNokiaTypeByte8Gray) {
            const u32 gray = *source;
            return 0xFF000000U | (gray << 16U) | (gray << 8U) | gray;
        }
        const u32 red = ((static_cast<u32>(*source) >> 5U) & 7U) * 255U / 7U;
        const u32 green = ((static_cast<u32>(*source) >> 2U) & 7U) * 255U / 7U;
        const u32 blue = (static_cast<u32>(*source) & 3U) * 255U / 3U;
        return 0xFF000000U | (red << 16U) | (green << 8U) | blue;
    }

    i64 byte_index = 0;
    i32 shift = 0;
    i32 mask = 0;
    if (format == kNokiaTypeByte1GrayVertical) {
        if (scan_length <= 0) {
            return fail_java("java/lang/IllegalArgumentException",
                             std::string(operation) +
                                 " requires a positive vertical scan length");
        }
        const i32 row = position / scan_length;
        const i32 column = position % scan_length;
        byte_index = static_cast<i64>(row / 8) * scan_length + column;
        shift = row & 7;
        mask = 1;
    } else {
        const i32 bits = format;
        const i64 bit_position = static_cast<i64>(position) * bits;
        byte_index = bit_position >> 3;
        shift = 8 - bits - static_cast<i32>(bit_position & 7);
        mask = (1 << bits) - 1;
        if (shift < 0) {
            return fail_java("java/lang/IllegalArgumentException",
                             std::string(operation) +
                                 " packed scan alignment is invalid");
        }
    }
    auto source = byte_array_value(machine, pixels, byte_index, operation);
    if (!source) return std::unexpected(source.error());
    const u32 level =
        ((static_cast<u32>(*source) >> static_cast<u32>(shift)) &
         static_cast<u32>(mask)) * 255U / static_cast<u32>(mask);
    return 0xFF000000U | (level << 16U) | (level << 8U) | level;
}

[[nodiscard]] Result<bool> packed_alpha_bit(
    Machine& machine,
    ObjectRef mask,
    i32 position,
    std::string_view operation) {
    const i64 index = static_cast<i64>(position) >> 3;
    auto value = byte_array_value(machine, mask, index, operation);
    if (!value) return std::unexpected(value.error());
    const u32 shift = static_cast<u32>(7 - (position & 7));
    return ((static_cast<u32>(*value) >> shift) & 1U) != 0U;
}

[[nodiscard]] Status set_packed_alpha_bit(
    Machine& machine,
    ObjectRef mask,
    i32 position,
    bool enabled,
    std::string_view operation) {
    const i64 index = static_cast<i64>(position) >> 3;
    auto current = byte_array_value(machine, mask, index, operation);
    if (!current) return std::unexpected(current.error());
    const u8 bit = static_cast<u8>(1U <<
        static_cast<u32>(7 - (position & 7)));
    const u8 updated = enabled
        ? static_cast<u8>(*current | bit)
        : static_cast<u8>(*current & static_cast<u8>(~bit));
    return set_byte_array_value(machine, mask, index, updated, operation);
}

[[nodiscard]] u8 nokia_gray(graphics::Pixel pixel) noexcept {
    const u32 red = (pixel >> 16U) & 0xFFU;
    const u32 green = (pixel >> 8U) & 0xFFU;
    const u32 blue = pixel & 0xFFU;
    return static_cast<u8>((red * 30U + green * 59U + blue * 11U) / 100U);
}

[[nodiscard]] Status set_nokia_byte_pixel(
    Machine& machine,
    ObjectRef pixels,
    i32 position,
    i32 scan_length,
    i32 format,
    graphics::Pixel pixel,
    std::string_view operation) {
    const u32 red = (pixel >> 16U) & 0xFFU;
    const u32 green = (pixel >> 8U) & 0xFFU;
    const u32 blue = pixel & 0xFFU;
    if (format == kNokiaTypeByte8Gray) {
        return set_byte_array_value(machine, pixels, position,
                                    nokia_gray(pixel), operation);
    }
    if (format == kNokiaTypeByte332Rgb) {
        const u8 packed = static_cast<u8>(
            ((((red * 7U) + 127U) / 255U) << 5U) |
            ((((green * 7U) + 127U) / 255U) << 2U) |
            (((blue * 3U) + 127U) / 255U));
        return set_byte_array_value(machine, pixels, position,
                                    packed, operation);
    }

    i64 byte_index = 0;
    i32 shift = 0;
    i32 mask = 0;
    if (format == kNokiaTypeByte1GrayVertical) {
        if (scan_length <= 0) {
            return fail_java("java/lang/IllegalArgumentException",
                             std::string(operation) +
                                 " requires a positive vertical scan length");
        }
        const i32 row = position / scan_length;
        const i32 column = position % scan_length;
        byte_index = static_cast<i64>(row / 8) * scan_length + column;
        shift = row & 7;
        mask = 1;
    } else {
        const i32 bits = format;
        const i64 bit_position = static_cast<i64>(position) * bits;
        byte_index = bit_position >> 3;
        shift = 8 - bits - static_cast<i32>(bit_position & 7);
        mask = (1 << bits) - 1;
        if (shift < 0) {
            return fail_java("java/lang/IllegalArgumentException",
                             std::string(operation) +
                                 " packed scan alignment is invalid");
        }
    }
    auto current = byte_array_value(machine, pixels, byte_index, operation);
    if (!current) return std::unexpected(current.error());
    const u8 gray = nokia_gray(pixel);
    const i32 packed =
        (static_cast<i32>(gray) * mask + 127) / 255;
    const u32 shifted_mask = static_cast<u32>(mask) <<
                             static_cast<u32>(shift);
    const u8 updated = static_cast<u8>(
        (static_cast<u32>(*current) & ~shifted_mask) |
        (static_cast<u32>(packed) << static_cast<u32>(shift)));
    return set_byte_array_value(machine, pixels, byte_index,
                                updated, operation);
}

[[nodiscard]] Result<std::vector<graphics::Pixel>> nokia_source_pixels(
    Machine& machine,
    ObjectRef array,
    bool short_pixels,
    bool transparency,
    i32 offset,
    i32 scan_length,
    i32 width,
    i32 height,
    i32 format,
    std::string_view operation) {
    if (width < 0 || height < 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) +
                             " dimensions are negative");
    }
    if (short_pixels ? !nokia_short_format(format)
                     : (format != kNokiaTypeInt888Rgb &&
                        format != kNokiaTypeInt8888Argb)) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) +
                             " pixel format is unsupported");
    }
    auto class_name = machine.heap().class_name(array);
    auto length = machine.heap().array_length(array);
    if (!class_name || !length) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " expects a pixel array");
    }
    const std::string_view expected = short_pixels ? "[S" : "[I";
    if (*class_name != expected) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) +
                             " pixel array has the wrong type");
    }
    if (width == 0 || height == 0) return std::vector<graphics::Pixel> {};
    auto count = graphics::validated_pixel_count(width, height);
    if (!count) return graphics_error(count.error());
    std::vector<graphics::Pixel> result(*count);
    for (i32 row = 0; row < height; ++row) {
        for (i32 column = 0; column < width; ++column) {
            auto source_index = checked_array_index(offset, scan_length,
                                                    row, column, *length,
                                                    operation);
            if (!source_index) return std::unexpected(source_index.error());
            auto value = machine.heap().element(array, *source_index);
            if (!value) return std::unexpected(value.error());
            auto integer = value->as_int();
            if (!integer) return std::unexpected(integer.error());
            graphics::Pixel pixel;
            if (short_pixels) {
                pixel = nokia_short_to_argb(
                    static_cast<u16>(*integer), transparency, format);
            } else {
                pixel = static_cast<graphics::Pixel>(
                    static_cast<u32>(*integer));
                if (format == kNokiaTypeInt888Rgb || !transparency) {
                    pixel |= 0xFF000000U;
                }
            }
            result[static_cast<usize>(row) *
                   static_cast<usize>(width) +
                   static_cast<usize>(column)] = pixel;
        }
    }
    return result;
}

[[nodiscard]] Status draw_nokia_pixels(
    Machine& machine,
    std::span<const Value> arguments,
    bool short_pixels,
    std::string_view operation) {
    auto bound = bound_direct_graphics(machine, arguments, operation);
    auto array = reference_argument(arguments, 1U, operation);
    auto transparency = int_argument(arguments, 2U, operation);
    auto offset = int_argument(arguments, 3U, operation);
    auto scan_length = int_argument(arguments, 4U, operation);
    auto x = int_argument(arguments, 5U, operation);
    auto y = int_argument(arguments, 6U, operation);
    auto width = int_argument(arguments, 7U, operation);
    auto height = int_argument(arguments, 8U, operation);
    auto manipulation = int_argument(arguments, 9U, operation);
    auto format = int_argument(arguments, 10U, operation);
    if (!bound) return std::unexpected(bound.error());
    if (!array) return std::unexpected(array.error());
    if (!transparency) return std::unexpected(transparency.error());
    if (!offset) return std::unexpected(offset.error());
    if (!scan_length) return std::unexpected(scan_length.error());
    if (!x) return std::unexpected(x.error());
    if (!y) return std::unexpected(y.error());
    if (!width) return std::unexpected(width.error());
    if (!height) return std::unexpected(height.error());
    if (!manipulation) return std::unexpected(manipulation.error());
    if (!format) return std::unexpected(format.error());
    auto pixels = nokia_source_pixels(
        machine, *array, short_pixels, *transparency != 0,
        *offset, *scan_length, *width, *height, *format, operation);
    if (!pixels) return std::unexpected(pixels.error());
    if (*width == 0 || *height == 0) return {};
    auto source = graphics::Image::create_immutable(*width, *height, *pixels);
    if (!source) return graphics_error(source.error());
    auto transform = nokia_transform(*manipulation);
    if (!transform) return std::unexpected(transform.error());
    return graphics::draw_region(
        *bound->target, *bound->context, *source,
        0, 0, *width, *height, *transform, *x, *y,
        graphics::anchor_top | graphics::anchor_left);
}

[[nodiscard]] Status validate_byte_array(
    Machine& machine,
    ObjectRef array,
    std::string_view operation) {
    auto class_name = machine.heap().class_name(array);
    if (!class_name || *class_name != "[B") {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " expects byte[]");
    }
    return {};
}

[[nodiscard]] Status draw_nokia_byte_pixels(
    Machine& machine,
    std::span<const Value> arguments,
    std::string_view operation) {
    auto bound = bound_direct_graphics(machine, arguments, operation);
    auto pixels = reference_argument(arguments, 1U, operation);
    auto alpha_mask = reference_argument(arguments, 2U, operation, true);
    auto offset = int_argument(arguments, 3U, operation);
    auto scan_length = int_argument(arguments, 4U, operation);
    auto x = int_argument(arguments, 5U, operation);
    auto y = int_argument(arguments, 6U, operation);
    auto width = int_argument(arguments, 7U, operation);
    auto height = int_argument(arguments, 8U, operation);
    auto manipulation = int_argument(arguments, 9U, operation);
    auto format = int_argument(arguments, 10U, operation);
    if (!bound) return std::unexpected(bound.error());
    if (!pixels) return std::unexpected(pixels.error());
    if (!alpha_mask) return std::unexpected(alpha_mask.error());
    if (!offset) return std::unexpected(offset.error());
    if (!scan_length) return std::unexpected(scan_length.error());
    if (!x) return std::unexpected(x.error());
    if (!y) return std::unexpected(y.error());
    if (!width) return std::unexpected(width.error());
    if (!height) return std::unexpected(height.error());
    if (!manipulation) return std::unexpected(manipulation.error());
    if (!format) return std::unexpected(format.error());
    if (*width < 0 || *height < 0 || !nokia_byte_format(*format)) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) +
                             " dimensions or byte format are invalid");
    }
    auto valid_pixels = validate_byte_array(machine, *pixels, operation);
    if (!valid_pixels) return valid_pixels;
    if (!alpha_mask->is_null()) {
        auto valid_mask = validate_byte_array(machine, *alpha_mask, operation);
        if (!valid_mask) return valid_mask;
    }
    if (*width == 0 || *height == 0) return {};
    auto count = graphics::validated_pixel_count(*width, *height);
    if (!count) return graphics_error(count.error());
    std::vector<graphics::Pixel> converted(*count);
    for (i32 row = 0; row < *height; ++row) {
        for (i32 column = 0; column < *width; ++column) {
            const i64 raw_position = static_cast<i64>(*offset) +
                                     static_cast<i64>(row) * *scan_length +
                                     column;
            if (raw_position < 0 ||
                raw_position > std::numeric_limits<i32>::max()) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 std::string(operation) +
                                     " pixel position is outside bounds");
            }
            const i32 position = static_cast<i32>(raw_position);
            auto pixel = nokia_byte_to_argb(
                machine, *pixels, position, *scan_length, *format, operation);
            if (!pixel) return std::unexpected(pixel.error());
            if (!alpha_mask->is_null()) {
                auto opaque = packed_alpha_bit(
                    machine, *alpha_mask, position, operation);
                if (!opaque) return std::unexpected(opaque.error());
                if (!*opaque) *pixel &= 0x00FFFFFFU;
            }
            converted[static_cast<usize>(row) *
                      static_cast<usize>(*width) +
                      static_cast<usize>(column)] = *pixel;
        }
    }
    auto source = graphics::Image::create_immutable(
        *width, *height, converted);
    if (!source) return graphics_error(source.error());
    auto transform = nokia_transform(*manipulation);
    if (!transform) return std::unexpected(transform.error());
    return graphics::draw_region(
        *bound->target, *bound->context, *source,
        0, 0, *width, *height, *transform, *x, *y,
        graphics::anchor_top | graphics::anchor_left);
}

[[nodiscard]] Status get_nokia_byte_pixels(
    Machine& machine,
    std::span<const Value> arguments,
    std::string_view operation) {
    auto bound = bound_direct_graphics(machine, arguments, operation);
    auto pixels = reference_argument(arguments, 1U, operation);
    auto alpha_mask = reference_argument(arguments, 2U, operation, true);
    auto offset = int_argument(arguments, 3U, operation);
    auto scan_length = int_argument(arguments, 4U, operation);
    auto x = int_argument(arguments, 5U, operation);
    auto y = int_argument(arguments, 6U, operation);
    auto width = int_argument(arguments, 7U, operation);
    auto height = int_argument(arguments, 8U, operation);
    auto format = int_argument(arguments, 9U, operation);
    if (!bound) return std::unexpected(bound.error());
    if (!pixels) return std::unexpected(pixels.error());
    if (!alpha_mask) return std::unexpected(alpha_mask.error());
    if (!offset) return std::unexpected(offset.error());
    if (!scan_length) return std::unexpected(scan_length.error());
    if (!x) return std::unexpected(x.error());
    if (!y) return std::unexpected(y.error());
    if (!width) return std::unexpected(width.error());
    if (!height) return std::unexpected(height.error());
    if (!format) return std::unexpected(format.error());
    if (*width < 0 || *height < 0 || !nokia_byte_format(*format)) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) +
                             " dimensions or byte format are invalid");
    }
    auto valid_pixels = validate_byte_array(machine, *pixels, operation);
    if (!valid_pixels) return valid_pixels;
    if (!alpha_mask->is_null()) {
        auto valid_mask = validate_byte_array(machine, *alpha_mask, operation);
        if (!valid_mask) return valid_mask;
    }
    if (*width == 0 || *height == 0) return {};
    const i64 source_x = static_cast<i64>(*x) +
                         bound->context->translate_x;
    const i64 source_y = static_cast<i64>(*y) +
                         bound->context->translate_y;
    if (source_x < 0 || source_y < 0 ||
        source_x + *width > bound->target->width() ||
        source_y + *height > bound->target->height()) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) +
                             " source rectangle is outside the surface");
    }
    for (i32 row = 0; row < *height; ++row) {
        for (i32 column = 0; column < *width; ++column) {
            const i64 raw_position = static_cast<i64>(*offset) +
                                     static_cast<i64>(row) * *scan_length +
                                     column;
            if (raw_position < 0 ||
                raw_position > std::numeric_limits<i32>::max()) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 std::string(operation) +
                                     " pixel position is outside bounds");
            }
            const i32 position = static_cast<i32>(raw_position);
            auto pixel = bound->target->pixel(
                static_cast<i32>(source_x) + column,
                static_cast<i32>(source_y) + row);
            if (!pixel) return graphics_error(pixel.error());
            auto stored = set_nokia_byte_pixel(
                machine, *pixels, position, *scan_length,
                *format, *pixel, operation);
            if (!stored) return stored;
            if (!alpha_mask->is_null()) {
                auto alpha = set_packed_alpha_bit(
                    machine, *alpha_mask, position,
                    ((*pixel >> 24U) & 0xFFU) != 0U, operation);
                if (!alpha) return alpha;
            }
        }
    }
    return {};
}

[[nodiscard]] Result<std::vector<i32>> nokia_polygon_points(
    Machine& machine,
    ObjectRef array,
    i32 offset,
    i32 count,
    std::string_view operation) {
    auto class_name = machine.heap().class_name(array);
    auto length = machine.heap().array_length(array);
    if (!class_name || !length || *class_name != "[I") {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " expects int[]");
    }
    if (offset < 0 || count < 0 ||
        static_cast<usize>(offset) > *length ||
        static_cast<usize>(count) >
            *length - static_cast<usize>(offset)) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) +
                             " point range is invalid");
    }
    std::vector<i32> points;
    points.reserve(static_cast<usize>(count));
    for (i32 index = 0; index < count; ++index) {
        auto value = machine.heap().element(
            array, static_cast<usize>(offset + index));
        if (!value) return std::unexpected(value.error());
        auto integer = value->as_int();
        if (!integer) return std::unexpected(integer.error());
        points.push_back(*integer);
    }
    return points;
}

[[nodiscard]] Status get_nokia_pixels(
    Machine& machine,
    std::span<const Value> arguments,
    bool short_pixels,
    std::string_view operation) {
    auto bound = bound_direct_graphics(machine, arguments, operation);
    auto array = reference_argument(arguments, 1U, operation);
    auto offset = int_argument(arguments, 2U, operation);
    auto scan_length = int_argument(arguments, 3U, operation);
    auto x = int_argument(arguments, 4U, operation);
    auto y = int_argument(arguments, 5U, operation);
    auto width = int_argument(arguments, 6U, operation);
    auto height = int_argument(arguments, 7U, operation);
    auto format = int_argument(arguments, 8U, operation);
    if (!bound) return std::unexpected(bound.error());
    if (!array) return std::unexpected(array.error());
    if (!offset) return std::unexpected(offset.error());
    if (!scan_length) return std::unexpected(scan_length.error());
    if (!x) return std::unexpected(x.error());
    if (!y) return std::unexpected(y.error());
    if (!width) return std::unexpected(width.error());
    if (!height) return std::unexpected(height.error());
    if (!format) return std::unexpected(format.error());
    if (*width < 0 || *height < 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) +
                             " dimensions are negative");
    }
    if (short_pixels ? !nokia_short_format(*format)
                     : (*format != kNokiaTypeInt888Rgb &&
                        *format != kNokiaTypeInt8888Argb)) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) +
                             " pixel format is unsupported");
    }
    auto class_name = machine.heap().class_name(*array);
    auto array_length = machine.heap().array_length(*array);
    const std::string_view expected = short_pixels ? "[S" : "[I";
    if (!class_name || !array_length || *class_name != expected) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) +
                             " pixel array has the wrong type");
    }
    if (*width == 0 || *height == 0) return {};
    const i64 source_x = static_cast<i64>(*x) +
                         bound->context->translate_x;
    const i64 source_y = static_cast<i64>(*y) +
                         bound->context->translate_y;
    if (source_x < 0 || source_y < 0 ||
        source_x + *width > bound->target->width() ||
        source_y + *height > bound->target->height()) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) +
                             " source rectangle is outside the surface");
    }
    for (i32 row = 0; row < *height; ++row) {
        for (i32 column = 0; column < *width; ++column) {
            auto destination_index = checked_array_index(
                *offset, *scan_length, row, column, *array_length, operation);
            if (!destination_index) {
                return std::unexpected(destination_index.error());
            }
            auto pixel = bound->target->pixel(
                static_cast<i32>(source_x) + column,
                static_cast<i32>(source_y) + row);
            if (!pixel) return graphics_error(pixel.error());
            i32 stored_value;
            if (short_pixels) {
                stored_value = static_cast<i32>(static_cast<i16>(
                    nokia_argb_to_short(*pixel, *format)));
            } else {
                const u32 value = *format == kNokiaTypeInt888Rgb
                    ? (*pixel & 0x00FFFFFFU) : *pixel;
                stored_value = static_cast<i32>(value);
            }
            auto stored = machine.heap().set_element(
                *array, *destination_index, Value::from_int(stored_value));
            if (!stored) return std::unexpected(stored.error());
        }
    }
    return {};
}

[[nodiscard]] Result<i32> object_int_field(Machine& machine,
                                           ObjectRef object,
                                           usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

[[nodiscard]] Result<graphics::Font> font_payload(Machine& machine,
                                                  ObjectRef font) {
    if (font.is_null()) return graphics::Font::default_font();
    auto class_name = machine.heap().class_name(font);
    if (!class_name || *class_name != "javax/microedition/lcdui/Font") {
        return fail_java("java/lang/IllegalArgumentException",
                         "object is not a javax.microedition.lcdui.Font");
    }
    auto face = object_int_field(machine, font, kFontFaceField);
    auto style = object_int_field(machine, font, kFontStyleField);
    auto size = object_int_field(machine, font, kFontSizeField);
    if (!face) return std::unexpected(face.error());
    if (!style) return std::unexpected(style.error());
    if (!size) return std::unexpected(size.error());
    auto result = graphics::Font::create(*face, *style, *size);
    if (!result) return graphics_error(result.error());
    return *result;
}

[[nodiscard]] Result<ObjectRef> create_font_object(Machine& machine,
                                                   const graphics::Font& font) {
    auto object = machine.class_states().allocate_instance(
        machine.heap(), "javax/microedition/lcdui/Font");
    if (!object) return std::unexpected(object.error());
    auto face = machine.heap().set_field(
        *object, kFontFaceField, Value::from_int(font.face()));
    auto style = machine.heap().set_field(
        *object, kFontStyleField, Value::from_int(font.style()));
    auto size = machine.heap().set_field(
        *object, kFontSizeField, Value::from_int(font.size()));
    if (!face) return std::unexpected(face.error());
    if (!style) return std::unexpected(style.error());
    if (!size) return std::unexpected(size.error());
    return *object;
}

[[nodiscard]] Result<std::vector<char32_t>> string_characters(
    Machine& machine,
    ObjectRef string,
    std::string_view operation) {
    auto text = string_text(machine, string, operation);
    if (!text) return std::unexpected(text.error());
    return utf32_text(*text);
}

[[nodiscard]] Result<std::vector<char32_t>> substring_characters(
    Machine& machine,
    ObjectRef string,
    i32 offset,
    i32 length,
    std::string_view operation) {
    auto text = string_text(machine, string, operation);
    if (!text) return std::unexpected(text.error());
    if (offset < 0 || length < 0 ||
        static_cast<usize>(offset) > text->size() ||
        static_cast<usize>(length) >
            text->size() - static_cast<usize>(offset)) {
        return fail_java("java/lang/StringIndexOutOfBoundsException",
                         std::string(operation) + " substring is outside the String");
    }
    return utf32_text(std::u16string_view(*text).substr(
        static_cast<usize>(offset), static_cast<usize>(length)));
}

[[nodiscard]] Result<std::vector<char32_t>> char_array_characters(
    Machine& machine,
    ObjectRef array,
    i32 offset,
    i32 length,
    std::string_view operation) {
    if (array.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         std::string(operation) + " char[] is null");
    }
    if (offset < 0 || length < 0) {
        return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                         std::string(operation) + " char[] slice is negative");
    }
    auto class_name = machine.heap().class_name(array);
    auto array_length = machine.heap().array_length(array);
    if (!class_name || !array_length || *class_name != "[C") {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " expects char[]");
    }
    if (static_cast<usize>(offset) > *array_length ||
        static_cast<usize>(length) >
            *array_length - static_cast<usize>(offset)) {
        return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                         std::string(operation) + " char[] slice is outside the array");
    }
    std::u16string text;
    text.reserve(static_cast<usize>(length));
    for (i32 index = 0; index < length; ++index) {
        auto value = machine.heap().element(
            array, static_cast<usize>(offset + index));
        if (!value) return std::unexpected(value.error());
        auto character = value->as_int();
        if (!character) return std::unexpected(character.error());
        text.push_back(static_cast<char16_t>(static_cast<u16>(*character)));
    }
    return utf32_text(text);
}

[[nodiscard]] Result<std::optional<Value>> status_result(Status status) {
    if (!status) return graphics_error(status.error());
    return std::optional<Value> {};
}

[[nodiscard]] Result<std::optional<Value>> font_boolean(bool value) {
    return std::optional<Value>(Value::from_int(value ? 1 : 0));
}

[[nodiscard]] graphics::Pixel phone_me_opaque_color(u8 red,
                                                    u8 green,
                                                    u8 blue) noexcept {
    // Preserve the logical Graphics color in RGB888. Lower color-depth
    // backends may quantize during final scanout, but doing it here corrupts
    // getColor(), component getters and every mutable off-screen Image.
    return graphics::argb(255U, red, green, blue);
}

[[nodiscard]] graphics::Pixel phone_me_opaque_color(u32 rgb) noexcept {
    return phone_me_opaque_color(
        static_cast<u8>((rgb >> 16U) & 0xFFU),
        static_cast<u8>((rgb >> 8U) & 0xFFU),
        static_cast<u8>(rgb & 0xFFU));
}

template <typename Arguments>
[[nodiscard]] Result<std::optional<Value>> draw_image_native(
    Machine& machine,
    const Arguments& arguments) {
    auto bound = bound_graphics(machine, arguments, "Graphics.drawImage");
    auto source_reference = reference_argument(
        arguments, 1U, "Graphics.drawImage");
    auto x = int_argument(arguments, 2U, "Graphics.drawImage");
    auto y = int_argument(arguments, 3U, "Graphics.drawImage");
    auto anchor = int_argument(arguments, 4U, "Graphics.drawImage");
    if (!bound) return std::unexpected(bound.error());
    if (!source_reference) return std::unexpected(source_reference.error());
    if (!x) return std::unexpected(x.error());
    if (!y) return std::unexpected(y.error());
    if (!anchor) return std::unexpected(anchor.error());
    auto source = image_payload(machine, *source_reference);
    if (!source) return std::unexpected(source.error());
    machine.intercept_image_draw(ImageDrawEvent {
        .kind = ImageDrawKind::image,
        .graphics_id = bound->graphics_id,
        .target_image_id = bound->context->target_key,
        .source_image_id = source_reference->bits,
        .source_width = (*source)->width(),
        .source_height = (*source)->height(),
        .transform = static_cast<i32>(graphics::Transform::none),
        .destination_x = *x,
        .destination_y = *y,
        .anchor = *anchor,
        .translate_x = bound->context->translate_x,
        .translate_y = bound->context->translate_y,
        .clip_x = bound->context->clip.x,
        .clip_y = bound->context->clip.y,
        .clip_width = bound->context->clip.width,
        .clip_height = bound->context->clip.height,
        .display_target = bound->context->display_target,
    });
    return status_result(graphics::draw_image(
        *bound->target, *bound->context, **source, *x, *y, *anchor));
}

template <typename Arguments>
[[nodiscard]] Result<std::optional<Value>> draw_region_native(
    Machine& machine,
    const Arguments& arguments) {
    auto bound = bound_graphics(machine, arguments, "Graphics.drawRegion");
    auto source_reference = reference_argument(
        arguments, 1U, "Graphics.drawRegion");
    std::array<Result<i32>, 8> values {
        int_argument(arguments, 2U, "Graphics.drawRegion"),
        int_argument(arguments, 3U, "Graphics.drawRegion"),
        int_argument(arguments, 4U, "Graphics.drawRegion"),
        int_argument(arguments, 5U, "Graphics.drawRegion"),
        int_argument(arguments, 6U, "Graphics.drawRegion"),
        int_argument(arguments, 7U, "Graphics.drawRegion"),
        int_argument(arguments, 8U, "Graphics.drawRegion"),
        int_argument(arguments, 9U, "Graphics.drawRegion"),
    };
    if (!bound) return std::unexpected(bound.error());
    if (!source_reference) return std::unexpected(source_reference.error());
    for (const auto& value : values) {
        if (!value) return std::unexpected(value.error());
    }
    auto transform = graphics::transform_from_int(*values[4]);
    if (!transform) return graphics_error(transform.error());
    auto anchor_validation = graphics::anchored_rect(
        0, 0, 0, 0, *values[7], false);
    if (!anchor_validation) return graphics_error(anchor_validation.error());
    if (source_reference->bits == bound->context->target_key) {
        return fail_java("java/lang/IllegalArgumentException",
                         "drawRegion source equals destination");
    }
    auto source = image_payload(machine, *source_reference);
    if (!source) return std::unexpected(source.error());
    machine.intercept_image_draw(ImageDrawEvent {
        .kind = ImageDrawKind::region,
        .graphics_id = bound->graphics_id,
        .target_image_id = bound->context->target_key,
        .source_image_id = source_reference->bits,
        .source_x = *values[0],
        .source_y = *values[1],
        .source_width = *values[2],
        .source_height = *values[3],
        .transform = *values[4],
        .destination_x = *values[5],
        .destination_y = *values[6],
        .anchor = *values[7],
        .translate_x = bound->context->translate_x,
        .translate_y = bound->context->translate_y,
        .clip_x = bound->context->clip.x,
        .clip_y = bound->context->clip.y,
        .clip_width = bound->context->clip.width,
        .clip_height = bound->context->clip.height,
        .display_target = bound->context->display_target,
    });
    return status_result(graphics::draw_region(
        *bound->target, *bound->context, **source,
        *values[0], *values[1], *values[2], *values[3],
        *transform, *values[5], *values[6], *values[7]));
}

} // namespace

void register_graphics_natives(NativeMethodRegistry& registry) {
    constexpr const char* graphics_owner =
        "javax/microedition/lcdui/Graphics";
    constexpr const char* font_owner = "javax/microedition/lcdui/Font";

    add(registry, graphics_owner, "setColor", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.setColor");
            auto rgb = int_argument(arguments, 1U, "Graphics.setColor");
            if (!bound) return std::unexpected(bound.error());
            if (!rgb) return std::unexpected(rgb.error());
            bound->context->color = phone_me_opaque_color(
                static_cast<u32>(*rgb));
            return std::optional<Value> {};
        },
        NativeJitPolicy::conservative,
        [](Machine& machine, const InvocationArguments& arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.setColor");
            auto rgb = int_argument(arguments, 1U, "Graphics.setColor");
            if (!bound) return std::unexpected(bound.error());
            if (!rgb) return std::unexpected(rgb.error());
            bound->context->color = phone_me_opaque_color(
                static_cast<u32>(*rgb));
            return std::optional<Value> {};
        });

    add(registry, graphics_owner, "setColor", "(III)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.setColor");
            auto red = int_argument(arguments, 1U, "Graphics.setColor");
            auto green = int_argument(arguments, 2U, "Graphics.setColor");
            auto blue = int_argument(arguments, 3U, "Graphics.setColor");
            if (!bound) return std::unexpected(bound.error());
            if (!red) return std::unexpected(red.error());
            if (!green) return std::unexpected(green.error());
            if (!blue) return std::unexpected(blue.error());
            if (*red < 0 || *red > 255 || *green < 0 || *green > 255 ||
                *blue < 0 || *blue > 255) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Graphics color components must be 0..255");
            }
            bound->context->color = phone_me_opaque_color(
                static_cast<u8>(*red),
                static_cast<u8>(*green),
                static_cast<u8>(*blue));
            return std::optional<Value> {};
        });

    add(registry, graphics_owner, "getColor", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.getColor");
            if (!bound) return std::unexpected(bound.error());
            return std::optional<Value>(Value::from_int(static_cast<i32>(
                bound->context->color & 0x00FFFFFFU)));
        });

    add(registry, graphics_owner, "getDisplayColor", "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.getDisplayColor");
            auto requested = int_argument(
                arguments, 1U, "Graphics.getDisplayColor");
            if (!bound) return std::unexpected(bound.error());
            if (!requested) return std::unexpected(requested.error());
            const graphics::Pixel displayed = graphics::rgb565_roundtrip(
                phone_me_opaque_color(static_cast<u32>(*requested)));
            return std::optional<Value>(Value::from_int(static_cast<i32>(
                displayed & 0x00FFFFFFU)));
        });

    const auto component_getter = [&registry](const char* name,
                                              auto getter) {
        add(registry, graphics_owner, name, "()I",
            [getter, name](Machine& machine,
                           std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto bound = bound_graphics(machine, arguments, name);
                if (!bound) return std::unexpected(bound.error());
                return std::optional<Value>(Value::from_int(
                    getter(bound->context->color)));
            });
    };
    component_getter("getRedComponent", graphics::red);
    component_getter("getGreenComponent", graphics::green);
    component_getter("getBlueComponent", graphics::blue);

    add(registry, graphics_owner, "setGrayScale", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.setGrayScale");
            auto value = int_argument(arguments, 1U,
                                      "Graphics.setGrayScale");
            if (!bound) return std::unexpected(bound.error());
            if (!value) return std::unexpected(value.error());
            if (*value < 0 || *value > 255) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "gray scale must be 0..255");
            }
            const u8 component = static_cast<u8>(*value);
            bound->context->color = phone_me_opaque_color(
                component, component, component);
            return std::optional<Value> {};
        });

    add(registry, graphics_owner, "getGrayScale", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.getGrayScale");
            if (!bound) return std::unexpected(bound.error());
            const u32 gray = (static_cast<u32>(graphics::red(
                                  bound->context->color)) +
                              graphics::green(bound->context->color) +
                              graphics::blue(bound->context->color)) /
                             3U;
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(gray)));
        });

    add(registry, graphics_owner, "setStrokeStyle", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.setStrokeStyle");
            auto style = int_argument(arguments, 1U,
                                      "Graphics.setStrokeStyle");
            if (!bound) return std::unexpected(bound.error());
            if (!style) return std::unexpected(style.error());
            if (*style != graphics::stroke_solid &&
                *style != graphics::stroke_dotted) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "invalid Graphics stroke style");
            }
            bound->context->stroke_style = *style;
            return std::optional<Value> {};
        });

    add(registry, graphics_owner, "getStrokeStyle", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.getStrokeStyle");
            if (!bound) return std::unexpected(bound.error());
            return std::optional<Value>(Value::from_int(
                bound->context->stroke_style));
        });

    add(registry, graphics_owner, "translate", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.translate");
            auto x = int_argument(arguments, 1U, "Graphics.translate");
            auto y = int_argument(arguments, 2U, "Graphics.translate");
            if (!bound) return std::unexpected(bound.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            graphics::translate(*bound->context, *x, *y);
            return std::optional<Value> {};
        },
        NativeJitPolicy::conservative,
        [](Machine& machine, const InvocationArguments& arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.translate");
            auto x = int_argument(arguments, 1U, "Graphics.translate");
            auto y = int_argument(arguments, 2U, "Graphics.translate");
            if (!bound) return std::unexpected(bound.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            graphics::translate(*bound->context, *x, *y);
            return std::optional<Value> {};
        });

    add(registry, graphics_owner, "getTranslateX", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.getTranslateX");
            if (!bound) return std::unexpected(bound.error());
            return std::optional<Value>(Value::from_int(
                bound->context->translate_x));
        },
        NativeJitPolicy::conservative,
        [](Machine& machine, const InvocationArguments& arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.getTranslateX");
            if (!bound) return std::unexpected(bound.error());
            return std::optional<Value>(Value::from_int(
                bound->context->translate_x));
        });
    add(registry, graphics_owner, "getTranslateY", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.getTranslateY");
            if (!bound) return std::unexpected(bound.error());
            return std::optional<Value>(Value::from_int(
                bound->context->translate_y));
        },
        NativeJitPolicy::conservative,
        [](Machine& machine, const InvocationArguments& arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.getTranslateY");
            if (!bound) return std::unexpected(bound.error());
            return std::optional<Value>(Value::from_int(
                bound->context->translate_y));
        });

    const auto clip_operation = [&registry](const char* name, bool replace) {
        add(registry, graphics_owner, name, "(IIII)V",
            [replace, name](Machine& machine,
                            std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto bound = bound_graphics(machine, arguments, name);
                auto x = int_argument(arguments, 1U, name);
                auto y = int_argument(arguments, 2U, name);
                auto width = int_argument(arguments, 3U, name);
                auto height = int_argument(arguments, 4U, name);
                if (!bound) return std::unexpected(bound.error());
                if (!x) return std::unexpected(x.error());
                if (!y) return std::unexpected(y.error());
                if (!width) return std::unexpected(width.error());
                if (!height) return std::unexpected(height.error());
                return status_result(replace
                    ? graphics::set_clip(*bound->context,
                                         *bound->target,
                                         *x, *y, *width, *height)
                    : graphics::clip_rect(*bound->context,
                                          *bound->target,
                                          *x, *y, *width, *height));
            },
            NativeJitPolicy::conservative,
            [replace, name](Machine& machine,
                            const InvocationArguments& arguments)
                -> Result<std::optional<Value>> {
                auto bound = bound_graphics(machine, arguments, name);
                auto x = int_argument(arguments, 1U, name);
                auto y = int_argument(arguments, 2U, name);
                auto width = int_argument(arguments, 3U, name);
                auto height = int_argument(arguments, 4U, name);
                if (!bound) return std::unexpected(bound.error());
                if (!x) return std::unexpected(x.error());
                if (!y) return std::unexpected(y.error());
                if (!width) return std::unexpected(width.error());
                if (!height) return std::unexpected(height.error());
                return status_result(replace
                    ? graphics::set_clip(*bound->context,
                                         *bound->target,
                                         *x, *y, *width, *height)
                    : graphics::clip_rect(*bound->context,
                                          *bound->target,
                                          *x, *y, *width, *height));
            });
    };
    clip_operation("setClip", true);
    clip_operation("clipRect", false);

    const auto clip_getter = [&registry](const char* method_name,
                                         auto getter) {
        add(registry, graphics_owner, method_name, "()I",
            [getter, method_name](Machine& machine,
                                  std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto bound = bound_graphics(machine, arguments, method_name);
                if (!bound) return std::unexpected(bound.error());
                return std::optional<Value>(Value::from_int(
                    getter(*bound->context)));
            });
    };
    clip_getter("getClipX", [](const graphics::GraphicsContext& value) {
        return value.clip.x - value.translate_x;
    });
    clip_getter("getClipY", [](const graphics::GraphicsContext& value) {
        return value.clip.y - value.translate_y;
    });
    clip_getter("getClipWidth", [](const graphics::GraphicsContext& value) {
        return value.clip.width;
    });
    clip_getter("getClipHeight", [](const graphics::GraphicsContext& value) {
        return value.clip.height;
    });

    add(registry, graphics_owner, "setFont",
        "(Ljavax/microedition/lcdui/Font;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.setFont");
            auto font_reference = reference_argument(
                arguments, 1U, "Graphics.setFont", true);
            if (!bound) return std::unexpected(bound.error());
            if (!font_reference) return std::unexpected(font_reference.error());
            auto font = font_payload(machine, *font_reference);
            if (!font) return std::unexpected(font.error());
            bound->context->font = *font;
            return std::optional<Value> {};
        });

    add(registry, graphics_owner, "getFont",
        "()Ljavax/microedition/lcdui/Font;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.getFont");
            if (!bound) return std::unexpected(bound.error());
            auto font = create_font_object(machine, bound->context->font);
            if (!font) return std::unexpected(font.error());
            return std::optional<Value>(Value::from_reference(*font));
        });

    add(registry, graphics_owner, "drawLine", "(IIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.drawLine");
            auto x1 = int_argument(arguments, 1U, "Graphics.drawLine");
            auto y1 = int_argument(arguments, 2U, "Graphics.drawLine");
            auto x2 = int_argument(arguments, 3U, "Graphics.drawLine");
            auto y2 = int_argument(arguments, 4U, "Graphics.drawLine");
            if (!bound) return std::unexpected(bound.error());
            if (!x1) return std::unexpected(x1.error());
            if (!y1) return std::unexpected(y1.error());
            if (!x2) return std::unexpected(x2.error());
            if (!y2) return std::unexpected(y2.error());
            return status_result(graphics::draw_line(
                *bound->target, *bound->context, *x1, *y1, *x2, *y2));
        });

    const auto rectangle_operation = [&registry](const char* name,
                                                  bool fill) {
        add(registry, graphics_owner, name, "(IIII)V",
            [fill, name](Machine& machine,
                         std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto bound = bound_graphics(machine, arguments, name);
                auto x = int_argument(arguments, 1U, name);
                auto y = int_argument(arguments, 2U, name);
                auto width = int_argument(arguments, 3U, name);
                auto height = int_argument(arguments, 4U, name);
                if (!bound) return std::unexpected(bound.error());
                if (!x) return std::unexpected(x.error());
                if (!y) return std::unexpected(y.error());
                if (!width) return std::unexpected(width.error());
                if (!height) return std::unexpected(height.error());
                return status_result(fill
                    ? graphics::fill_rect(*bound->target,
                                          *bound->context,
                                          *x, *y, *width, *height)
                    : graphics::draw_rect(*bound->target,
                                          *bound->context,
                                          *x, *y, *width, *height));
            },
            NativeJitPolicy::conservative,
            [fill, name](Machine& machine,
                         const InvocationArguments& arguments)
                -> Result<std::optional<Value>> {
                auto bound = bound_graphics(machine, arguments, name);
                auto x = int_argument(arguments, 1U, name);
                auto y = int_argument(arguments, 2U, name);
                auto width = int_argument(arguments, 3U, name);
                auto height = int_argument(arguments, 4U, name);
                if (!bound) return std::unexpected(bound.error());
                if (!x) return std::unexpected(x.error());
                if (!y) return std::unexpected(y.error());
                if (!width) return std::unexpected(width.error());
                if (!height) return std::unexpected(height.error());
                return status_result(fill
                    ? graphics::fill_rect(*bound->target,
                                          *bound->context,
                                          *x, *y, *width, *height)
                    : graphics::draw_rect(*bound->target,
                                          *bound->context,
                                          *x, *y, *width, *height));
            });
    };
    rectangle_operation("fillRect", true);
    rectangle_operation("drawRect", false);
    add(registry, graphics_owner, "clearARGBRect", "(IIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.clearARGBRect");
            auto x = int_argument(arguments, 1U, "Graphics.clearARGBRect");
            auto y = int_argument(arguments, 2U, "Graphics.clearARGBRect");
            auto width = int_argument(arguments, 3U,
                                      "Graphics.clearARGBRect");
            auto height = int_argument(arguments, 4U,
                                       "Graphics.clearARGBRect");
            if (!bound) return std::unexpected(bound.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            return status_result(graphics::clear_rect(
                *bound->target, *bound->context, *x, *y, *width, *height));
        });

    const auto round_rectangle_operation = [&registry](const char* name,
                                                        bool fill) {
        add(registry, graphics_owner, name, "(IIIIII)V",
            [fill, name](Machine& machine,
                         std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto bound = bound_graphics(machine, arguments, name);
                std::array<Result<i32>, 6> values {
                    int_argument(arguments, 1U, name),
                    int_argument(arguments, 2U, name),
                    int_argument(arguments, 3U, name),
                    int_argument(arguments, 4U, name),
                    int_argument(arguments, 5U, name),
                    int_argument(arguments, 6U, name),
                };
                if (!bound) return std::unexpected(bound.error());
                for (const auto& value : values) {
                    if (!value) return std::unexpected(value.error());
                }
                return status_result(graphics::draw_round_rect(
                    *bound->target, *bound->context,
                    *values[0], *values[1], *values[2], *values[3],
                    *values[4], *values[5], fill));
            });
    };
    round_rectangle_operation("drawRoundRect", false);
    round_rectangle_operation("fillRoundRect", true);

    const auto arc_operation = [&registry](const char* name, bool fill) {
        add(registry, graphics_owner, name, "(IIIIII)V",
            [fill, name](Machine& machine,
                         std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto bound = bound_graphics(machine, arguments, name);
                std::array<Result<i32>, 6> values {
                    int_argument(arguments, 1U, name),
                    int_argument(arguments, 2U, name),
                    int_argument(arguments, 3U, name),
                    int_argument(arguments, 4U, name),
                    int_argument(arguments, 5U, name),
                    int_argument(arguments, 6U, name),
                };
                if (!bound) return std::unexpected(bound.error());
                for (const auto& value : values) {
                    if (!value) return std::unexpected(value.error());
                }
                return status_result(graphics::draw_arc(
                    *bound->target, *bound->context,
                    *values[0], *values[1], *values[2], *values[3],
                    *values[4], *values[5], fill));
            });
    };
    arc_operation("drawArc", false);
    arc_operation("fillArc", true);

    add(registry, graphics_owner, "fillTriangle", "(IIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.fillTriangle");
            std::array<Result<i32>, 6> values {
                int_argument(arguments, 1U, "Graphics.fillTriangle"),
                int_argument(arguments, 2U, "Graphics.fillTriangle"),
                int_argument(arguments, 3U, "Graphics.fillTriangle"),
                int_argument(arguments, 4U, "Graphics.fillTriangle"),
                int_argument(arguments, 5U, "Graphics.fillTriangle"),
                int_argument(arguments, 6U, "Graphics.fillTriangle"),
            };
            if (!bound) return std::unexpected(bound.error());
            for (const auto& value : values) {
                if (!value) return std::unexpected(value.error());
            }
            return status_result(graphics::fill_triangle(
                *bound->target, *bound->context,
                *values[0], *values[1], *values[2], *values[3],
                *values[4], *values[5]));
        });

    add(registry, graphics_owner, "drawImage",
        "(Ljavax/microedition/lcdui/Image;III)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            return draw_image_native(machine, arguments);
        },
        NativeJitPolicy::conservative,
        [](Machine& machine, const InvocationArguments& arguments)
            -> Result<std::optional<Value>> {
            return draw_image_native(machine, arguments);
        });

    add(registry, graphics_owner, "drawRegion",
        "(Ljavax/microedition/lcdui/Image;IIIIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            return draw_region_native(machine, arguments);
        },
        NativeJitPolicy::conservative,
        [](Machine& machine, const InvocationArguments& arguments)
            -> Result<std::optional<Value>> {
            return draw_region_native(machine, arguments);
        });

    add(registry, graphics_owner, "copyArea", "(IIIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.copyArea");
            std::array<Result<i32>, 7> values {
                int_argument(arguments, 1U, "Graphics.copyArea"),
                int_argument(arguments, 2U, "Graphics.copyArea"),
                int_argument(arguments, 3U, "Graphics.copyArea"),
                int_argument(arguments, 4U, "Graphics.copyArea"),
                int_argument(arguments, 5U, "Graphics.copyArea"),
                int_argument(arguments, 6U, "Graphics.copyArea"),
                int_argument(arguments, 7U, "Graphics.copyArea"),
            };
            if (!bound) return std::unexpected(bound.error());
            for (const auto& value : values) {
                if (!value) return std::unexpected(value.error());
            }
            if (bound->context->display_target) {
                return fail_java("java/lang/IllegalStateException",
                                 "copyArea is unavailable on display Graphics");
            }
            return status_result(graphics::copy_area(
                *bound->target, *bound->context,
                *values[0], *values[1], *values[2], *values[3],
                *values[4], *values[5], *values[6]));
        });

    add(registry, graphics_owner, "drawRGB", "([IIIIIIIZ)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.drawRGB");
            auto rgb_reference = reference_argument(
                arguments, 1U, "Graphics.drawRGB");
            std::array<Result<i32>, 7> values {
                int_argument(arguments, 2U, "Graphics.drawRGB"),
                int_argument(arguments, 3U, "Graphics.drawRGB"),
                int_argument(arguments, 4U, "Graphics.drawRGB"),
                int_argument(arguments, 5U, "Graphics.drawRGB"),
                int_argument(arguments, 6U, "Graphics.drawRGB"),
                int_argument(arguments, 7U, "Graphics.drawRGB"),
                int_argument(arguments, 8U, "Graphics.drawRGB"),
            };
            if (!bound) return std::unexpected(bound.error());
            if (!rgb_reference) return std::unexpected(rgb_reference.error());
            for (const auto& value : values) {
                if (!value) return std::unexpected(value.error());
            }
            auto pixels = int_array(machine, *rgb_reference,
                                    "Graphics.drawRGB");
            if (!pixels) return std::unexpected(pixels.error());
            auto drawn = graphics::draw_rgb(
                *bound->target, *bound->context, *pixels,
                *values[0], *values[1], *values[2], *values[3],
                *values[4], *values[5], *values[6] != 0);
            if (!drawn) {
                if (drawn.error().code == ErrorCode::out_of_range) {
                    return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                     drawn.error().message);
                }
                return graphics_error(drawn.error());
            }
            return std::optional<Value> {};
        });

    add(registry, graphics_owner, "drawString",
        "(Ljava/lang/String;III)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.drawString");
            auto string = reference_argument(arguments, 1U,
                                             "Graphics.drawString");
            auto x = int_argument(arguments, 2U, "Graphics.drawString");
            auto y = int_argument(arguments, 3U, "Graphics.drawString");
            auto anchor = int_argument(arguments, 4U,
                                       "Graphics.drawString");
            if (!bound) return std::unexpected(bound.error());
            if (!string) return std::unexpected(string.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            if (!anchor) return std::unexpected(anchor.error());
            auto text = string_characters(machine, *string,
                                          "Graphics.drawString");
            if (!text) return std::unexpected(text.error());
            std::shared_ptr<const std::vector<char32_t>> translated;
            const auto rendered = translated_characters(
                machine, *text, translated);
            return status_result(draw_runtime_translated_text(
                machine, *bound, rendered,
                translated != nullptr, *x, *y, *anchor));
        });

    add(registry, graphics_owner, "drawSubstring",
        "(Ljava/lang/String;IIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.drawSubstring");
            auto string = reference_argument(arguments, 1U,
                                             "Graphics.drawSubstring");
            std::array<Result<i32>, 5> values {
                int_argument(arguments, 2U, "Graphics.drawSubstring"),
                int_argument(arguments, 3U, "Graphics.drawSubstring"),
                int_argument(arguments, 4U, "Graphics.drawSubstring"),
                int_argument(arguments, 5U, "Graphics.drawSubstring"),
                int_argument(arguments, 6U, "Graphics.drawSubstring"),
            };
            if (!bound) return std::unexpected(bound.error());
            if (!string) return std::unexpected(string.error());
            for (const auto& value : values) {
                if (!value) return std::unexpected(value.error());
            }
            auto text = substring_characters(
                machine, *string, *values[0], *values[1],
                "Graphics.drawSubstring");
            if (!text) return std::unexpected(text.error());
            std::shared_ptr<const std::vector<char32_t>> translated;
            const auto rendered = translated_characters(
                machine, *text, translated);
            return status_result(draw_runtime_translated_text(
                machine, *bound, rendered,
                translated != nullptr,
                *values[2], *values[3], *values[4]));
        });

    add(registry, graphics_owner, "drawChar", "(CIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.drawChar", true);
            auto character = int_argument(arguments, 1U,
                                          "Graphics.drawChar");
            auto x = int_argument(arguments, 2U, "Graphics.drawChar");
            auto y = int_argument(arguments, 3U, "Graphics.drawChar");
            auto anchor = int_argument(arguments, 4U,
                                       "Graphics.drawChar");
            if (!bound) return std::unexpected(bound.error());
            if (!character) return std::unexpected(character.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            if (!anchor) return std::unexpected(anchor.error());
            const char32_t text[] {
                static_cast<char32_t>(static_cast<u16>(*character)),
            };
            const auto decision = machine.translate_draw_character(
                bound->graphics_id,
                text[0],
                *x,
                *y,
                *anchor,
                bound->context->font.face(),
                bound->context->font.style(),
                bound->context->font.size());
            if (decision.action ==
                Machine::CharacterTranslationDecision::Action::suppress) {
                return std::optional<Value> {};
            }
            if (decision.action ==
                    Machine::CharacterTranslationDecision::Action::draw_translation &&
                decision.translated) {
                return status_result(draw_runtime_translated_text(
                    machine,
                    *bound,
                    *decision.translated,
                    true,
                    decision.x,
                    decision.y,
                    decision.anchor));
            }
            return status_result(graphics::draw_text(
                *bound->target, *bound->context, text,
                *x, *y, *anchor));
        });

    add(registry, graphics_owner, "drawChars", "([CIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_graphics(machine, arguments,
                                        "Graphics.drawChars");
            auto data = reference_argument(arguments, 1U,
                                           "Graphics.drawChars");
            std::array<Result<i32>, 5> values {
                int_argument(arguments, 2U, "Graphics.drawChars"),
                int_argument(arguments, 3U, "Graphics.drawChars"),
                int_argument(arguments, 4U, "Graphics.drawChars"),
                int_argument(arguments, 5U, "Graphics.drawChars"),
                int_argument(arguments, 6U, "Graphics.drawChars"),
            };
            if (!bound) return std::unexpected(bound.error());
            if (!data) return std::unexpected(data.error());
            for (const auto& value : values) {
                if (!value) return std::unexpected(value.error());
            }
            auto text = char_array_characters(
                machine, *data, *values[0], *values[1],
                "Graphics.drawChars");
            if (!text) return std::unexpected(text.error());
            std::shared_ptr<const std::vector<char32_t>> translated;
            const auto rendered = translated_characters(
                machine, *text, translated);
            return status_result(draw_runtime_translated_text(
                machine, *bound, rendered,
                translated != nullptr,
                *values[2], *values[3], *values[4]));
        });

    add(registry, font_owner, "getDefaultFont",
        "()Ljavax/microedition/lcdui/Font;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Font.getDefaultFont expects no arguments");
            }
            auto object = create_font_object(
                machine, graphics::Font::default_font());
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        }, NativeJitPolicy::synchronous_bounded);

    add(registry, font_owner, "getFont",
        "(I)Ljavax/microedition/lcdui/Font;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto specifier = int_argument(arguments, 0U, "Font.getFont");
            if (!specifier) return std::unexpected(specifier.error());
            if (*specifier != 0 && *specifier != 1) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Font specifier is invalid");
            }
            auto object = create_font_object(
                machine, graphics::Font::default_font());
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        }, NativeJitPolicy::synchronous_bounded);

    add(registry, font_owner, "getFont",
        "(III)Ljavax/microedition/lcdui/Font;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto face = int_argument(arguments, 0U, "Font.getFont");
            auto style = int_argument(arguments, 1U, "Font.getFont");
            auto size = int_argument(arguments, 2U, "Font.getFont");
            if (!face) return std::unexpected(face.error());
            if (!style) return std::unexpected(style.error());
            if (!size) return std::unexpected(size.error());
            auto font = graphics::Font::create(*face, *style, *size);
            if (!font) return graphics_error(font.error());
            auto object = create_font_object(machine, *font);
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        }, NativeJitPolicy::synchronous_bounded);

    const auto font_int_getter = [&registry](const char* name, auto getter) {
        add(registry, font_owner, name, "()I",
            [getter, name](Machine& machine,
                           std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, name);
                if (!object) return std::unexpected(object.error());
                auto font = font_payload(machine, *object);
                if (!font) return std::unexpected(font.error());
                return std::optional<Value>(Value::from_int(getter(*font)));
            }, NativeJitPolicy::synchronous_bounded);
    };
    font_int_getter("getFace", [](const graphics::Font& font) {
        return font.face();
    });
    font_int_getter("getStyle", [](const graphics::Font& font) {
        return font.style();
    });
    font_int_getter("getSize", [](const graphics::Font& font) {
        return font.size();
    });
    font_int_getter("getHeight", [](const graphics::Font& font) {
        return font.height();
    });
    font_int_getter("getBaselinePosition", [](const graphics::Font& font) {
        return font.baseline();
    });

    const auto font_flag_getter = [&registry](const char* name, auto getter) {
        add(registry, font_owner, name, "()Z",
            [getter, name](Machine& machine,
                           std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, name);
                if (!object) return std::unexpected(object.error());
                auto font = font_payload(machine, *object);
                if (!font) return std::unexpected(font.error());
                return font_boolean(getter(*font));
            }, NativeJitPolicy::synchronous_bounded);
    };
    font_flag_getter("isPlain", [](const graphics::Font& font) {
        return font.is_plain();
    });
    font_flag_getter("isBold", [](const graphics::Font& font) {
        return font.is_bold();
    });
    font_flag_getter("isItalic", [](const graphics::Font& font) {
        return font.is_italic();
    });
    font_flag_getter("isUnderlined", [](const graphics::Font& font) {
        return font.is_underlined();
    });

    add(registry, font_owner, "charWidth", "(C)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Font.charWidth");
            auto character = int_argument(arguments, 1U, "Font.charWidth");
            if (!object) return std::unexpected(object.error());
            if (!character) return std::unexpected(character.error());
            auto font = font_payload(machine, *object);
            if (!font) return std::unexpected(font.error());
            return std::optional<Value>(Value::from_int(font->char_width(
                static_cast<char32_t>(static_cast<u16>(*character)))));
        });

    add(registry, font_owner, "charsWidth", "([CII)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Font.charsWidth");
            auto data = reference_argument(arguments, 1U, "Font.charsWidth");
            auto offset = int_argument(arguments, 2U, "Font.charsWidth");
            auto length = int_argument(arguments, 3U, "Font.charsWidth");
            if (!object) return std::unexpected(object.error());
            if (!data) return std::unexpected(data.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            auto font = font_payload(machine, *object);
            if (!font) return std::unexpected(font.error());
            auto text = char_array_characters(machine, *data, *offset, *length,
                                              "Font.charsWidth");
            if (!text) return std::unexpected(text.error());
            // MIDP Font metrics describe the Java characters supplied by the
            // MIDlet. Auto-translation is presentation-only and must never
            // change values used by game layout, clipping, or indexing.
            return std::optional<Value>(Value::from_int(
                font->chars_width(*text)));
        });

    add(registry, font_owner, "stringWidth", "(Ljava/lang/String;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Font.stringWidth");
            auto string = reference_argument(arguments, 1U,
                                             "Font.stringWidth");
            if (!object) return std::unexpected(object.error());
            if (!string) return std::unexpected(string.error());
            auto font = font_payload(machine, *object);
            if (!font) return std::unexpected(font.error());
            auto text = string_characters(machine, *string,
                                          "Font.stringWidth");
            if (!text) return std::unexpected(text.error());
            // Keep Java-visible metrics stable when an asynchronous
            // translation enters the cache. Only Graphics draw calls replace
            // rendered text.
            return std::optional<Value>(Value::from_int(
                font->chars_width(*text)));
        });

    add(registry, font_owner, "substringWidth",
        "(Ljava/lang/String;II)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Font.substringWidth");
            auto string = reference_argument(arguments, 1U,
                                             "Font.substringWidth");
            auto offset = int_argument(arguments, 2U,
                                       "Font.substringWidth");
            auto length = int_argument(arguments, 3U,
                                       "Font.substringWidth");
            if (!object) return std::unexpected(object.error());
            if (!string) return std::unexpected(string.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            auto font = font_payload(machine, *object);
            if (!font) return std::unexpected(font.error());
            auto text = substring_characters(machine, *string, *offset, *length,
                                             "Font.substringWidth");
            if (!text) return std::unexpected(text.error());
            // Preserve consistency with charWidth/charsWidth for the original
            // substring; translated metrics can invalidate layout invariants
            // in the middle of a frame.
            return std::optional<Value>(Value::from_int(
                font->chars_width(*text)));
        });

    constexpr const char* direct_impl =
        "com/nokia/mid/ui/DirectGraphicsImpl";
    constexpr const char* direct_owner =
        "javax/microedition/lcdui/Graphics";
    constexpr const char* direct_utils = "com/nokia/mid/ui/DirectUtils";
    constexpr const char* device_control = "com/nokia/mid/ui/DeviceControl";

    add(registry, direct_impl, "<init>",
        "(Ljavax/microedition/lcdui/Graphics;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "DirectGraphics.<init>");
            auto graphics = reference_argument(
                arguments, 1U, "DirectGraphics.<init>");
            if (!object) return std::unexpected(object.error());
            if (!graphics) return std::unexpected(graphics.error());
            auto valid = machine.object_is_instance(
                *graphics, "javax/microedition/lcdui/Graphics");
            if (!valid) return std::unexpected(valid.error());
            if (!*valid) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "DirectGraphics requires Graphics");
            }
            auto stored = machine.heap().set_field(
                *object, kDirectGraphicsTargetField,
                Value::from_reference(*graphics));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    add(registry, direct_utils, "getDirectGraphics",
        "(Ljavax/microedition/lcdui/Graphics;)"
        "Lcom/nokia/mid/ui/DirectGraphics;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto graphics = reference_argument(
                arguments, 0U, "DirectUtils.getDirectGraphics");
            if (!graphics) return std::unexpected(graphics.error());
            auto valid = machine.object_is_instance(
                *graphics, "javax/microedition/lcdui/Graphics");
            if (!valid) return std::unexpected(valid.error());
            if (!*valid) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "DirectUtils argument is not Graphics");
            }
            return std::optional<Value>(Value::from_reference(*graphics));
        });

    add(registry, direct_owner, "setARGBColor", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_direct_graphics(
                machine, arguments, "DirectGraphics.setARGBColor");
            auto color = int_argument(
                arguments, 1U, "DirectGraphics.setARGBColor");
            if (!bound) return std::unexpected(bound.error());
            if (!color) return std::unexpected(color.error());
            bound->context->color = static_cast<graphics::Pixel>(
                static_cast<u32>(*color));
            return std::optional<Value> {};
        });

    add(registry, direct_owner, "getAlphaComponent", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_direct_graphics(
                machine, arguments, "DirectGraphics.getAlphaComponent");
            if (!bound) return std::unexpected(bound.error());
            return std::optional<Value>(Value::from_int(
                static_cast<i32>((bound->context->color >> 24U) & 0xFFU)));
        });

    add(registry, direct_owner, "getNativePixelFormat", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto graphics = direct_graphics_target(
                machine, arguments, "DirectGraphics.getNativePixelFormat");
            if (!graphics) return std::unexpected(graphics.error());
            return std::optional<Value>(
                Value::from_int(kNokiaTypeUShort565Rgb));
        });

    add(registry, direct_owner, "drawImage",
        "(Ljavax/microedition/lcdui/Image;IIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_direct_graphics(
                machine, arguments, "DirectGraphics.drawImage");
            auto source_object = reference_argument(
                arguments, 1U, "DirectGraphics.drawImage");
            auto x = int_argument(arguments, 2U, "DirectGraphics.drawImage");
            auto y = int_argument(arguments, 3U, "DirectGraphics.drawImage");
            auto anchor = int_argument(
                arguments, 4U, "DirectGraphics.drawImage");
            auto manipulation = int_argument(
                arguments, 5U, "DirectGraphics.drawImage");
            if (!bound) return std::unexpected(bound.error());
            if (!source_object) return std::unexpected(source_object.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            if (!anchor) return std::unexpected(anchor.error());
            if (!manipulation) return std::unexpected(manipulation.error());
            auto source = image_payload(machine, *source_object);
            if (!source) return std::unexpected(source.error());
            auto transform = nokia_transform(*manipulation);
            if (!transform) return std::unexpected(transform.error());
            return status_result(graphics::draw_region(
                *bound->target, *bound->context, **source,
                0, 0, (*source)->width(), (*source)->height(),
                *transform, *x, *y, *anchor));
        });

    add(registry, direct_owner, "drawPixels", "([B[BIIIIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto drawn = draw_nokia_byte_pixels(
                machine, arguments, "DirectGraphics.drawPixels(byte[])");
            if (!drawn) return std::unexpected(drawn.error());
            return std::optional<Value> {};
        });
    add(registry, direct_owner, "drawPixels", "([SZIIIIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto drawn = draw_nokia_pixels(
                machine, arguments, true, "DirectGraphics.drawPixels(short[])");
            if (!drawn) return std::unexpected(drawn.error());
            return std::optional<Value> {};
        });
    add(registry, direct_owner, "drawPixels", "([IZIIIIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto drawn = draw_nokia_pixels(
                machine, arguments, false, "DirectGraphics.drawPixels(int[])");
            if (!drawn) return std::unexpected(drawn.error());
            return std::optional<Value> {};
        });
    add(registry, direct_owner, "getPixels", "([B[BIIIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto read = get_nokia_byte_pixels(
                machine, arguments, "DirectGraphics.getPixels(byte[])");
            if (!read) return std::unexpected(read.error());
            return std::optional<Value> {};
        });
    add(registry, direct_owner, "getPixels", "([SIIIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto read = get_nokia_pixels(
                machine, arguments, true, "DirectGraphics.getPixels(short[])");
            if (!read) return std::unexpected(read.error());
            return std::optional<Value> {};
        });
    add(registry, direct_owner, "getPixels", "([IIIIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto read = get_nokia_pixels(
                machine, arguments, false, "DirectGraphics.getPixels(int[])");
            if (!read) return std::unexpected(read.error());
            return std::optional<Value> {};
        });

    add(registry, direct_owner, "drawTriangle", "(IIIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_direct_graphics(
                machine, arguments, "DirectGraphics.drawTriangle");
            std::array<Result<i32>, 7> values {
                int_argument(arguments, 1U, "DirectGraphics.drawTriangle"),
                int_argument(arguments, 2U, "DirectGraphics.drawTriangle"),
                int_argument(arguments, 3U, "DirectGraphics.drawTriangle"),
                int_argument(arguments, 4U, "DirectGraphics.drawTriangle"),
                int_argument(arguments, 5U, "DirectGraphics.drawTriangle"),
                int_argument(arguments, 6U, "DirectGraphics.drawTriangle"),
                int_argument(arguments, 7U, "DirectGraphics.drawTriangle"),
            };
            if (!bound) return std::unexpected(bound.error());
            for (const auto& value : values) {
                if (!value) return std::unexpected(value.error());
            }
            if ((static_cast<u32>(*values[6]) >> 24U) == 0U) {
                return std::optional<Value> {};
            }
            graphics::GraphicsContext context = *bound->context;
            context.color = static_cast<graphics::Pixel>(
                static_cast<u32>(*values[6]));
            auto first = graphics::draw_line(
                *bound->target, context,
                *values[0], *values[1], *values[2], *values[3]);
            if (!first) return graphics_error(first.error());
            auto second = graphics::draw_line(
                *bound->target, context,
                *values[2], *values[3], *values[4], *values[5]);
            if (!second) return graphics_error(second.error());
            return status_result(graphics::draw_line(
                *bound->target, context,
                *values[4], *values[5], *values[0], *values[1]));
        });

    add(registry, direct_owner, "fillTriangle", "(IIIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bound = bound_direct_graphics(
                machine, arguments, "DirectGraphics.fillTriangle");
            std::array<Result<i32>, 7> values {
                int_argument(arguments, 1U, "DirectGraphics.fillTriangle"),
                int_argument(arguments, 2U, "DirectGraphics.fillTriangle"),
                int_argument(arguments, 3U, "DirectGraphics.fillTriangle"),
                int_argument(arguments, 4U, "DirectGraphics.fillTriangle"),
                int_argument(arguments, 5U, "DirectGraphics.fillTriangle"),
                int_argument(arguments, 6U, "DirectGraphics.fillTriangle"),
                int_argument(arguments, 7U, "DirectGraphics.fillTriangle"),
            };
            if (!bound) return std::unexpected(bound.error());
            for (const auto& value : values) {
                if (!value) return std::unexpected(value.error());
            }
            graphics::GraphicsContext context = *bound->context;
            context.color = static_cast<graphics::Pixel>(
                static_cast<u32>(*values[6]));
            return status_result(graphics::fill_triangle(
                *bound->target, context,
                *values[0], *values[1], *values[2], *values[3],
                *values[4], *values[5]));
        });

    const auto register_polygon = [&registry](
        const char* name,
        bool fill) {
        add(registry, direct_owner, name, "([II[IIII)V",
            [fill](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                const std::string operation = fill
                    ? "DirectGraphics.fillPolygon"
                    : "DirectGraphics.drawPolygon";
                auto bound = bound_direct_graphics(machine, arguments,
                                                   operation);
                auto x_array = reference_argument(arguments, 1U, operation);
                auto x_offset = int_argument(arguments, 2U, operation);
                auto y_array = reference_argument(arguments, 3U, operation);
                auto y_offset = int_argument(arguments, 4U, operation);
                auto count = int_argument(arguments, 5U, operation);
                auto color = int_argument(arguments, 6U, operation);
                if (!bound) return std::unexpected(bound.error());
                if (!x_array) return std::unexpected(x_array.error());
                if (!x_offset) return std::unexpected(x_offset.error());
                if (!y_array) return std::unexpected(y_array.error());
                if (!y_offset) return std::unexpected(y_offset.error());
                if (!count) return std::unexpected(count.error());
                if (!color) return std::unexpected(color.error());
                if (*count < 3) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     operation + " requires three points");
                }
                auto xs = nokia_polygon_points(machine, *x_array,
                                               *x_offset, *count, operation);
                auto ys = nokia_polygon_points(machine, *y_array,
                                               *y_offset, *count, operation);
                if (!xs) return std::unexpected(xs.error());
                if (!ys) return std::unexpected(ys.error());
                if ((static_cast<u32>(*color) >> 24U) == 0U) {
                    return std::optional<Value> {};
                }
                graphics::GraphicsContext context = *bound->context;
                context.color = static_cast<graphics::Pixel>(
                    static_cast<u32>(*color));
                if (!fill) {
                    for (i32 index = 0; index < *count; ++index) {
                        const i32 next = index + 1 == *count ? 0 : index + 1;
                        auto drawn = graphics::draw_line(
                            *bound->target, context,
                            (*xs)[static_cast<usize>(index)],
                            (*ys)[static_cast<usize>(index)],
                            (*xs)[static_cast<usize>(next)],
                            (*ys)[static_cast<usize>(next)]);
                        if (!drawn) return graphics_error(drawn.error());
                    }
                    return std::optional<Value> {};
                }
                if (*count == 3) {
                    return status_result(graphics::fill_triangle(
                        *bound->target, context,
                        (*xs)[0], (*ys)[0], (*xs)[1], (*ys)[1],
                        (*xs)[2], (*ys)[2]));
                }
                const auto [minimum_y, maximum_y] =
                    std::minmax_element(ys->begin(), ys->end());
                std::vector<i32> intersections;
                intersections.reserve(static_cast<usize>(*count));
                for (i32 scan_y = *minimum_y;
                     scan_y <= *maximum_y;
                     ++scan_y) {
                    intersections.clear();
                    for (i32 index = 0; index < *count; ++index) {
                        const i32 next = index + 1 == *count ? 0 : index + 1;
                        const i32 x1 = (*xs)[static_cast<usize>(index)];
                        const i32 y1 = (*ys)[static_cast<usize>(index)];
                        const i32 x2 = (*xs)[static_cast<usize>(next)];
                        const i32 y2 = (*ys)[static_cast<usize>(next)];
                        if ((y1 <= scan_y && y2 > scan_y) ||
                            (y2 <= scan_y && y1 > scan_y)) {
                            const i64 numerator =
                                static_cast<i64>(scan_y - y1) *
                                static_cast<i64>(x2 - x1);
                            intersections.push_back(x1 + static_cast<i32>(
                                numerator / static_cast<i64>(y2 - y1)));
                        }
                    }
                    std::sort(intersections.begin(), intersections.end());
                    for (usize index = 0;
                         index + 1U < intersections.size();
                         index += 2U) {
                        auto drawn = graphics::draw_line(
                            *bound->target, context,
                            intersections[index], scan_y,
                            intersections[index + 1U], scan_y);
                        if (!drawn) return graphics_error(drawn.error());
                    }
                    if (scan_y == std::numeric_limits<i32>::max()) break;
                }
                return std::optional<Value> {};
            });
    };
    register_polygon("drawPolygon", false);
    register_polygon("fillPolygon", true);

    add(registry, direct_utils, "createImage",
        "(III)Ljavax/microedition/lcdui/Image;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto width = int_argument(
                arguments, 0U, "DirectUtils.createImage");
            auto height = int_argument(
                arguments, 1U, "DirectUtils.createImage");
            auto color = int_argument(
                arguments, 2U, "DirectUtils.createImage");
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            if (!color) return std::unexpected(color.error());
            const std::array<Value, 2> image_arguments {
                Value::from_int(*width), Value::from_int(*height),
            };
            auto created = machine.invoke_static(
                "javax/microedition/lcdui/Image", "createImage",
                "(II)Ljavax/microedition/lcdui/Image;", image_arguments);
            if (!created) return std::unexpected(created.error());
            auto image_object = invocation_reference(
                machine, *created, "DirectUtils.createImage");
            if (!image_object) return std::unexpected(image_object.error());
            auto image = image_payload(machine, *image_object);
            if (!image) return std::unexpected(image.error());
            const auto device_color = graphics::rgb565_roundtrip(
                static_cast<graphics::Pixel>(static_cast<u32>(*color)));
            std::fill((*image)->mutable_pixels().begin(),
                      (*image)->mutable_pixels().end(),
                      device_color);
            (*image)->mark_dirty_region(0, 0, *width, *height);
            return std::optional<Value>(
                Value::from_reference(*image_object));
        });

    add(registry, direct_utils, "createImage",
        "([BII)Ljavax/microedition/lcdui/Image;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto bytes = reference_argument(
                arguments, 0U, "DirectUtils.createImage");
            auto offset = int_argument(
                arguments, 1U, "DirectUtils.createImage");
            auto length = int_argument(
                arguments, 2U, "DirectUtils.createImage");
            if (!bytes) return std::unexpected(bytes.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            const std::array<Value, 3> image_arguments {
                Value::from_reference(*bytes),
                Value::from_int(*offset),
                Value::from_int(*length),
            };
            auto created = machine.invoke_static(
                "javax/microedition/lcdui/Image", "createImage",
                "([BII)Ljavax/microedition/lcdui/Image;", image_arguments);
            if (!created) return std::unexpected(created.error());
            auto image_object = invocation_reference(
                machine, *created, "DirectUtils.createImage");
            if (!image_object) return std::unexpected(image_object.error());
            return std::optional<Value>(
                Value::from_reference(*image_object));
        });

    add(registry, device_control, "setLights", "(II)V",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto light = int_argument(arguments, 0U, "DeviceControl.setLights");
            auto level = int_argument(arguments, 1U, "DeviceControl.setLights");
            if (!light) return std::unexpected(light.error());
            if (!level) return std::unexpected(level.error());
            if (*level < 0 || *level > 100) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "DeviceControl light level is outside 0..100");
            }
            return std::optional<Value> {};
        });

    add(registry, device_control, "flashLights", "(J)V",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1U) {
                return fail(ErrorCode::invalid_argument,
                            "DeviceControl.flashLights expects a duration");
            }
            auto duration = arguments[0].as_long();
            if (!duration) return std::unexpected(duration.error());
            if (*duration < 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "DeviceControl light duration is negative");
            }
#if defined(__APPLE__)
            if (*duration != 0 && phoneme_ios_device_flash_lights != nullptr) {
                (void)phoneme_ios_device_flash_lights(*duration);
            }
#endif
            return std::optional<Value> {};
        });

    add(registry, device_control, "startVibra", "(IJ)V",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "DeviceControl.startVibra arguments are missing");
            }
            auto frequency = arguments[0].as_int();
            auto duration = arguments[1].as_long();
            if (!frequency) return std::unexpected(frequency.error());
            if (!duration) return std::unexpected(duration.error());
            if (*frequency < 0 || *frequency > 100 || *duration < 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "DeviceControl vibration arguments are invalid");
            }
#if defined(__APPLE__)
            if ((*frequency == 0 || *duration == 0) &&
                phoneme_ios_device_stop_vibrate != nullptr) {
                (void)phoneme_ios_device_stop_vibrate();
            } else if (phoneme_ios_device_start_vibrate != nullptr) {
                (void)phoneme_ios_device_start_vibrate(*frequency, *duration);
            }
#endif
            return std::optional<Value> {};
        });

    add(registry, device_control, "stopVibra", "()V",
        [](Machine&, std::span<const Value>)
            -> Result<std::optional<Value>> {
#if defined(__APPLE__)
            if (phoneme_ios_device_stop_vibrate != nullptr) {
                (void)phoneme_ios_device_stop_vibrate();
            }
#endif
            return std::optional<Value> {};
        });
}

} // namespace phoneme::vm
