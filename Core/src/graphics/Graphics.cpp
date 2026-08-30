#include "phoneme/graphics/Graphics.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cctype>
#include <limits>
#include <optional>
#include <vector>

#include "phoneme/graphics/TextRasterizer.hpp"
#include "phoneme/runtime/ParallelExecutor.hpp"

namespace phoneme::graphics {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr usize kParallelBlitPixels = 64U * 1024U;
constexpr usize kParallelDrawRgbPixels = 128U * 1024U;

[[nodiscard]] i32 saturated_add(i32 left, i32 right) noexcept {
    const i64 value = static_cast<i64>(left) + static_cast<i64>(right);
    return static_cast<i32>(std::clamp<i64>(
        value,
        std::numeric_limits<i32>::min(),
        std::numeric_limits<i32>::max()));
}

[[nodiscard]] i32 rounded_i32(double value) noexcept {
    if (!std::isfinite(value)) {
        return 0;
    }
    const double minimum = static_cast<double>(
        std::numeric_limits<i32>::min());
    const double maximum = static_cast<double>(
        std::numeric_limits<i32>::max());
    return static_cast<i32>(std::llround(std::clamp(value,
                                                    minimum,
                                                    maximum)));
}

[[nodiscard]] bool contains(Rect rectangle, i32 x, i32 y) noexcept {
    if (rectangle.width <= 0 || rectangle.height <= 0) {
        return false;
    }
    const i64 right = static_cast<i64>(rectangle.x) + rectangle.width;
    const i64 bottom = static_cast<i64>(rectangle.y) + rectangle.height;
    return static_cast<i64>(x) >= rectangle.x &&
           static_cast<i64>(y) >= rectangle.y &&
           static_cast<i64>(x) < right &&
           static_cast<i64>(y) < bottom;
}

[[nodiscard]] i32 half_magnitude(i32 value, i32 maximum) noexcept {
    const i64 magnitude = value < 0 ? -static_cast<i64>(value)
                                    : static_cast<i64>(value);
    return static_cast<i32>(std::min<i64>(magnitude / 2, maximum));
}

[[nodiscard]] bool rectangles_overlap(Rect left, Rect right) noexcept {
    return !empty(intersect(left, right));
}

struct TransformSteps final {
    i32 base_x {0};
    i32 base_y {0};
    i32 column_x {1};
    i32 column_y {0};
    i32 row_x {0};
    i32 row_y {1};
};

[[nodiscard]] constexpr TransformSteps transform_steps(
    i32 width,
    i32 height,
    Transform transform) noexcept {
    switch (transform) {
    case Transform::none:
        return {};
    case Transform::mirror_rotate_180:
        return {.base_y = height - 1, .row_y = -1};
    case Transform::mirror:
        return {.base_x = width - 1, .column_x = -1};
    case Transform::rotate_180:
        return {.base_x = width - 1, .base_y = height - 1,
                .column_x = -1, .row_y = -1};
    case Transform::mirror_rotate_270:
        return {.column_x = 0, .column_y = 1,
                .row_x = 1, .row_y = 0};
    case Transform::rotate_90:
        return {.base_y = height - 1,
                .column_x = 0, .column_y = -1,
                .row_x = 1, .row_y = 0};
    case Transform::rotate_270:
        return {.base_x = width - 1,
                .column_x = 0, .column_y = 1,
                .row_x = -1, .row_y = 0};
    case Transform::mirror_rotate_90:
        return {.base_x = width - 1, .base_y = height - 1,
                .column_x = 0, .column_y = -1,
                .row_x = -1, .row_y = 0};
    }
    return {};
}

[[nodiscard]] inline bool composite_device_pixel(
    Pixel source,
    Pixel& destination) noexcept {
    const u8 source_alpha = alpha(source);
    if (source_alpha == 0U) return false;
    const Pixel composited = source_alpha == 255U
        ? rgb565_roundtrip(source)
        : rgb565_roundtrip(source_over(source, destination));
    if (composited == destination) return false;
    destination = composited;
    return true;
}

template <ImageAlphaKind AlphaKind>
[[nodiscard]] inline bool composite_classified_device_pixel(
    Pixel source,
    Pixel& destination) noexcept {
    if constexpr (AlphaKind == ImageAlphaKind::opaque) {
        const Pixel composited = source;
        if (composited == destination) return false;
        destination = composited;
        return true;
    } else if constexpr (AlphaKind == ImageAlphaKind::binary) {
        if ((source & 0xFF000000U) == 0U) return false;
        const Pixel composited = source;
        if (composited == destination) return false;
        destination = composited;
        return true;
    } else {
        return composite_device_pixel(source, destination);
    }
}

template <ImageAlphaKind AlphaKind>
[[nodiscard]] bool blit_linear_pixels(
    std::span<Pixel> target_pixels,
    usize target_stride,
    std::span<const Pixel> source_pixels,
    usize source_stride,
    usize destination_x,
    usize destination_y,
    usize source_x,
    usize source_y,
    usize width,
    usize height) noexcept {
    std::atomic<bool> changed {false};
    const auto blit_rows = [&](usize row_begin, usize row_end) {
        bool local_changed = false;
        for (usize row = row_begin; row < row_end; ++row) {
            const usize destination_offset =
                (destination_y + row) * target_stride + destination_x;
            const usize source_offset =
                (source_y + row) * source_stride + source_x;
            for (usize column = 0; column < width; ++column) {
                local_changed = composite_classified_device_pixel<AlphaKind>(
                    source_pixels[source_offset + column],
                    target_pixels[destination_offset + column]) ||
                    local_changed;
            }
        }
        if (local_changed) changed.store(true, std::memory_order_relaxed);
    };
    if (width * height >= kParallelBlitPixels && height >= 4U) {
        const usize rows_per_chunk = std::max<usize>(
            1U, 4U * 1024U / std::max<usize>(width, 1U));
        runtime::shared_compute_executor().parallel_for(
            runtime::WorkClass::frame_critical,
            width * height,
            height, 4U, rows_per_chunk, blit_rows);
    } else {
        blit_rows(0U, height);
    }
    return changed.load(std::memory_order_relaxed);
}

template <ImageAlphaKind AlphaKind>
[[nodiscard]] bool blit_strided_pixels(
    std::span<Pixel> target_pixels,
    usize target_stride,
    const Pixel* source_base,
    i64 first_source_index,
    i64 column_step,
    i64 row_step,
    usize destination_x,
    usize destination_y,
    usize width,
    usize height) noexcept {
    std::atomic<bool> changed {false};
    const auto blit_rows = [&](usize row_begin, usize row_end) {
        bool local_changed = false;
        for (usize row = row_begin; row < row_end; ++row) {
            const usize destination_offset =
                (destination_y + row) * target_stride + destination_x;
            i64 source_index = first_source_index +
                static_cast<i64>(row) * row_step;
            for (usize column = 0; column < width; ++column) {
                local_changed = composite_classified_device_pixel<AlphaKind>(
                    source_base[static_cast<usize>(source_index)],
                    target_pixels[destination_offset + column]) ||
                    local_changed;
                source_index += column_step;
            }
        }
        if (local_changed) changed.store(true, std::memory_order_relaxed);
    };
    if (width * height >= kParallelBlitPixels && height >= 4U) {
        const usize rows_per_chunk = std::max<usize>(
            1U, 4U * 1024U / std::max<usize>(width, 1U));
        runtime::shared_compute_executor().parallel_for(
            runtime::WorkClass::frame_critical,
            width * height,
            height, 4U, rows_per_chunk, blit_rows);
    } else {
        blit_rows(0U, height);
    }
    return changed.load(std::memory_order_relaxed);
}

[[nodiscard]] Status require_mutable(const Image& image) {
    if (!image.is_mutable()) {
        return fail(ErrorCode::invalid_state,
                    "graphics target image is immutable");
    }
    return {};
}

[[nodiscard]] Status put_pixel(Image& target,
                               const GraphicsContext& context,
                               i32 x,
                               i32 y,
                               Pixel pixel,
                               bool blend = true) {
    if (!contains(context.clip, x, y)) {
        return {};
    }
    return target.set_pixel(x, y, pixel, blend);
}

[[nodiscard]] Status draw_line_absolute(Image& target,
                                        const GraphicsContext& context,
                                        i32 x1,
                                        i32 y1,
                                        i32 x2,
                                        i32 y2) {
    if (empty(context.clip)) return {};

    const i64 start_x = x1;
    const i64 start_y = y1;
    const u64 delta_x = static_cast<u64>(std::llabs(
        static_cast<i64>(x2) - start_x));
    const u64 delta_y = static_cast<u64>(std::llabs(
        static_cast<i64>(y2) - start_y));
    const i64 step_x = x1 <= x2 ? 1 : -1;
    const i64 step_y = y1 <= y2 ? 1 : -1;
    const i64 clip_left = context.clip.x;
    const i64 clip_top = context.clip.y;
    const i64 clip_right = static_cast<i64>(context.clip.x) +
                           context.clip.width - 1;
    const i64 clip_bottom = static_cast<i64>(context.clip.y) +
                            context.clip.height - 1;

    const auto clipped_major_range = [](i64 start,
                                        i64 step,
                                        u64 length,
                                        i64 lower,
                                        i64 upper)
        -> std::optional<std::pair<u64, u64>> {
        i64 first = 0;
        i64 last = static_cast<i64>(length);
        if (step > 0) {
            first = std::max<i64>(first, lower - start);
            last = std::min<i64>(last, upper - start);
        } else {
            first = std::max<i64>(first, start - upper);
            last = std::min<i64>(last, start - lower);
        }
        if (first > last || last < 0) return std::nullopt;
        first = std::max<i64>(first, 0);
        return std::pair<u64, u64> {
            static_cast<u64>(first), static_cast<u64>(last)};
    };

    const auto rounded_minor = [](u64 major_step,
                                  u64 minor_delta,
                                  u64 major_delta) noexcept -> u64 {
        if (major_delta == 0U || minor_delta == 0U) return 0U;
        const u64 product = major_step * minor_delta;
        const u64 quotient = product / major_delta;
        const u64 remainder = product % major_delta;
        return quotient + (remainder * 2U >= major_delta ? 1U : 0U);
    };

    const bool x_major = delta_x >= delta_y;
    const u64 major_delta = x_major ? delta_x : delta_y;
    const u64 minor_delta = x_major ? delta_y : delta_x;
    auto range = x_major
        ? clipped_major_range(start_x, step_x, major_delta,
                              clip_left, clip_right)
        : clipped_major_range(start_y, step_y, major_delta,
                              clip_top, clip_bottom);
    if (!range.has_value()) return {};

    for (u64 sample = range->first; sample <= range->second; ++sample) {
        const u64 minor = rounded_minor(sample, minor_delta, major_delta);
        const i64 current_x = x_major
            ? start_x + step_x * static_cast<i64>(sample)
            : start_x + step_x * static_cast<i64>(minor);
        const i64 current_y = x_major
            ? start_y + step_y * static_cast<i64>(minor)
            : start_y + step_y * static_cast<i64>(sample);
        if (current_x >= clip_left && current_x <= clip_right &&
            current_y >= clip_top && current_y <= clip_bottom &&
            (context.stroke_style == stroke_solid ||
             (sample & 1U) == 0U)) {
            auto stored = target.set_pixel(
                static_cast<i32>(current_x),
                static_cast<i32>(current_y),
                context.color,
                true);
            if (!stored) return stored;
        }
        if (sample == range->second) break;
    }
    return {};
}

[[nodiscard]] bool rounded_contains(i32 local_x,
                                    i32 local_y,
                                    i32 width,
                                    i32 height,
                                    i32 radius_x,
                                    i32 radius_y) noexcept {
    if (local_x < 0 || local_y < 0 || local_x >= width || local_y >= height) {
        return false;
    }
    if (radius_x <= 0 || radius_y <= 0 ||
        (local_x >= radius_x && local_x < width - radius_x) ||
        (local_y >= radius_y && local_y < height - radius_y)) {
        return true;
    }
    const double center_x = local_x < radius_x
        ? static_cast<double>(radius_x) - 0.5
        : static_cast<double>(width - radius_x) - 0.5;
    const double center_y = local_y < radius_y
        ? static_cast<double>(radius_y) - 0.5
        : static_cast<double>(height - radius_y) - 0.5;
    const double normalized_x =
        (static_cast<double>(local_x) + 0.5 - center_x) /
        static_cast<double>(radius_x);
    const double normalized_y =
        (static_cast<double>(local_y) + 0.5 - center_y) /
        static_cast<double>(radius_y);
    return normalized_x * normalized_x + normalized_y * normalized_y <= 1.0;
}

struct Glyph final {
    char character;
    std::array<u8, 5> columns;
};

constexpr std::array<Glyph, 36> kGlyphs {{
    {'0', {0x3EU, 0x51U, 0x49U, 0x45U, 0x3EU}},
    {'1', {0x00U, 0x42U, 0x7FU, 0x40U, 0x00U}},
    {'2', {0x42U, 0x61U, 0x51U, 0x49U, 0x46U}},
    {'3', {0x21U, 0x41U, 0x45U, 0x4BU, 0x31U}},
    {'4', {0x18U, 0x14U, 0x12U, 0x7FU, 0x10U}},
    {'5', {0x27U, 0x45U, 0x45U, 0x45U, 0x39U}},
    {'6', {0x3CU, 0x4AU, 0x49U, 0x49U, 0x30U}},
    {'7', {0x01U, 0x71U, 0x09U, 0x05U, 0x03U}},
    {'8', {0x36U, 0x49U, 0x49U, 0x49U, 0x36U}},
    {'9', {0x06U, 0x49U, 0x49U, 0x29U, 0x1EU}},
    {'A', {0x7EU, 0x11U, 0x11U, 0x11U, 0x7EU}},
    {'B', {0x7FU, 0x49U, 0x49U, 0x49U, 0x36U}},
    {'C', {0x3EU, 0x41U, 0x41U, 0x41U, 0x22U}},
    {'D', {0x7FU, 0x41U, 0x41U, 0x22U, 0x1CU}},
    {'E', {0x7FU, 0x49U, 0x49U, 0x49U, 0x41U}},
    {'F', {0x7FU, 0x09U, 0x09U, 0x09U, 0x01U}},
    {'G', {0x3EU, 0x41U, 0x49U, 0x49U, 0x7AU}},
    {'H', {0x7FU, 0x08U, 0x08U, 0x08U, 0x7FU}},
    {'I', {0x00U, 0x41U, 0x7FU, 0x41U, 0x00U}},
    {'J', {0x20U, 0x40U, 0x41U, 0x3FU, 0x01U}},
    {'K', {0x7FU, 0x08U, 0x14U, 0x22U, 0x41U}},
    {'L', {0x7FU, 0x40U, 0x40U, 0x40U, 0x40U}},
    {'M', {0x7FU, 0x02U, 0x0CU, 0x02U, 0x7FU}},
    {'N', {0x7FU, 0x04U, 0x08U, 0x10U, 0x7FU}},
    {'O', {0x3EU, 0x41U, 0x41U, 0x41U, 0x3EU}},
    {'P', {0x7FU, 0x09U, 0x09U, 0x09U, 0x06U}},
    {'Q', {0x3EU, 0x41U, 0x51U, 0x21U, 0x5EU}},
    {'R', {0x7FU, 0x09U, 0x19U, 0x29U, 0x46U}},
    {'S', {0x46U, 0x49U, 0x49U, 0x49U, 0x31U}},
    {'T', {0x01U, 0x01U, 0x7FU, 0x01U, 0x01U}},
    {'U', {0x3FU, 0x40U, 0x40U, 0x40U, 0x3FU}},
    {'V', {0x1FU, 0x20U, 0x40U, 0x20U, 0x1FU}},
    {'W', {0x3FU, 0x40U, 0x38U, 0x40U, 0x3FU}},
    {'X', {0x63U, 0x14U, 0x08U, 0x14U, 0x63U}},
    {'Y', {0x07U, 0x08U, 0x70U, 0x08U, 0x07U}},
    {'Z', {0x61U, 0x51U, 0x49U, 0x45U, 0x43U}},
}};

[[nodiscard]] std::array<u8, 5> glyph_columns(char32_t character) noexcept {
    if (character >= U'a' && character <= U'z') {
        character -= U'a' - U'A';
    }
    if (character <= 0x7FU) {
        const char ascii = static_cast<char>(character);
        for (const Glyph& glyph : kGlyphs) {
            if (glyph.character == ascii) {
                return glyph.columns;
            }
        }
        switch (ascii) {
        case ' ':
            return {};
        case '.':
            return {0x00U, 0x60U, 0x60U, 0x00U, 0x00U};
        case ',':
            return {0x00U, 0x80U, 0x60U, 0x00U, 0x00U};
        case ':':
            return {0x00U, 0x36U, 0x36U, 0x00U, 0x00U};
        case '-':
            return {0x08U, 0x08U, 0x08U, 0x08U, 0x08U};
        case '_':
            return {0x40U, 0x40U, 0x40U, 0x40U, 0x40U};
        case '/':
            return {0x20U, 0x10U, 0x08U, 0x04U, 0x02U};
        case '\\':
            return {0x02U, 0x04U, 0x08U, 0x10U, 0x20U};
        case '!':
            return {0x00U, 0x00U, 0x5FU, 0x00U, 0x00U};
        case '?':
            return {0x02U, 0x01U, 0x51U, 0x09U, 0x06U};
        case '(':
            return {0x00U, 0x1CU, 0x22U, 0x41U, 0x00U};
        case ')':
            return {0x00U, 0x41U, 0x22U, 0x1CU, 0x00U};
        default:
            break;
        }
    }
    const u8 seed = static_cast<u8>((character ^ (character >> 8U)) & 0x7FU);
    return {0x7FU,
            static_cast<u8>(0x41U | (seed & 0x1CU)),
            static_cast<u8>(0x41U |
                            ((static_cast<u32>(seed) << 1U) & 0x1CU)),
            static_cast<u8>(0x41U | ((seed >> 1U) & 0x1CU)),
            0x7FU};
}

[[nodiscard]] Status draw_glyph(Image& target,
                                const GraphicsContext& context,
                                char32_t character,
                                i32 cell_x,
                                i32 top) {
    if (character == U' ' || character == U'\t') {
        return {};
    }
    const auto columns = glyph_columns(character);
    const i32 scale = context.font.height() >= 20 ? 2 : 1;
    const i32 glyph_width = 5 * scale;
    const i32 glyph_height = 7 * scale;
    const i32 cell_width = context.font.char_width(character);
    const i32 origin_x = cell_x + std::max(0, (cell_width - glyph_width) / 2);
    const i32 origin_y = top + context.font.baseline() - glyph_height;

    for (i32 column = 0; column < 5; ++column) {
        for (i32 row = 0; row < 7; ++row) {
            if ((columns[static_cast<usize>(column)] & (1U << row)) == 0U) {
                continue;
            }
            const i32 italic_shift = context.font.is_italic()
                ? (6 - row) / 3
                : 0;
            for (i32 scale_y = 0; scale_y < scale; ++scale_y) {
                for (i32 scale_x = 0; scale_x < scale; ++scale_x) {
                    const i32 pixel_x = origin_x + column * scale + scale_x +
                                        italic_shift;
                    const i32 pixel_y = origin_y + row * scale + scale_y;
                    auto stored = put_pixel(target,
                                            context,
                                            pixel_x,
                                            pixel_y,
                                            context.color);
                    if (!stored) {
                        return stored;
                    }
                    if (context.font.is_bold()) {
                        stored = put_pixel(target,
                                           context,
                                           pixel_x + 1,
                                           pixel_y,
                                           context.color);
                        if (!stored) {
                            return stored;
                        }
                    }
                }
            }
        }
    }
    return {};
}

} // namespace

Rect intersect(Rect left_rectangle, Rect right_rectangle) noexcept {
    const i64 left = std::max<i64>(left_rectangle.x, right_rectangle.x);
    const i64 top = std::max<i64>(left_rectangle.y, right_rectangle.y);
    const i64 right = std::min<i64>(
        static_cast<i64>(left_rectangle.x) +
            std::max(0, left_rectangle.width),
        static_cast<i64>(right_rectangle.x) +
            std::max(0, right_rectangle.width));
    const i64 bottom = std::min<i64>(
        static_cast<i64>(left_rectangle.y) +
            std::max(0, left_rectangle.height),
        static_cast<i64>(right_rectangle.y) +
            std::max(0, right_rectangle.height));
    if (right <= left || bottom <= top) {
        return Rect {.x = static_cast<i32>(std::clamp<i64>(
                         left,
                         std::numeric_limits<i32>::min(),
                         std::numeric_limits<i32>::max())),
                     .y = static_cast<i32>(std::clamp<i64>(
                         top,
                         std::numeric_limits<i32>::min(),
                         std::numeric_limits<i32>::max())),
                     .width = 0,
                     .height = 0};
    }
    return Rect {
        .x = static_cast<i32>(left),
        .y = static_cast<i32>(top),
        .width = static_cast<i32>(std::min<i64>(
            right - left, std::numeric_limits<i32>::max())),
        .height = static_cast<i32>(std::min<i64>(
            bottom - top, std::numeric_limits<i32>::max())),
    };
}

bool empty(Rect rectangle) noexcept {
    return rectangle.width <= 0 || rectangle.height <= 0;
}

Rect target_bounds(const Image& image) noexcept {
    return Rect {.x = 0,
                 .y = 0,
                 .width = image.width(),
                 .height = image.height()};
}

Status set_clip(GraphicsContext& context,
                const Image& target,
                i32 x,
                i32 y,
                i32 width,
                i32 height) {
    context.clip = intersect(
        Rect {.x = saturated_add(x, context.translate_x),
              .y = saturated_add(y, context.translate_y),
              .width = std::max(0, width),
              .height = std::max(0, height)},
        target_bounds(target));
    return {};
}

Status clip_rect(GraphicsContext& context,
                 const Image& target,
                 i32 x,
                 i32 y,
                 i32 width,
                 i32 height) {
    context.clip = intersect(
        context.clip,
        intersect(Rect {.x = saturated_add(x, context.translate_x),
                        .y = saturated_add(y, context.translate_y),
                        .width = std::max(0, width),
                        .height = std::max(0, height)},
                  target_bounds(target)));
    return {};
}

void translate(GraphicsContext& context, i32 x, i32 y) noexcept {
    context.translate_x = saturated_add(context.translate_x, x);
    context.translate_y = saturated_add(context.translate_y, y);
}

Result<Rect> anchored_rect(i32 x,
                           i32 y,
                           i32 width,
                           i32 height,
                           i32 anchor,
                           bool text,
                           i32 baseline) {
    if (width < 0 || height < 0) {
        return fail(ErrorCode::invalid_argument,
                    "anchored dimensions cannot be negative");
    }
    if (anchor == 0) {
        anchor = anchor_left | anchor_top;
    }
    const i32 horizontal = anchor &
        (anchor_left | anchor_right | anchor_hcenter);
    const i32 vertical_mask = text
        ? (anchor_top | anchor_bottom | anchor_baseline)
        : (anchor_top | anchor_bottom | anchor_vcenter);
    const i32 vertical = anchor & vertical_mask;
    const i32 allowed = (anchor_left | anchor_right | anchor_hcenter) |
                        vertical_mask;
    const auto one_bit = [](i32 value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    };
    if (!one_bit(horizontal) || !one_bit(vertical) ||
        (anchor & ~allowed) != 0) {
        return fail(ErrorCode::invalid_argument,
                    "invalid or conflicting Graphics anchor");
    }

    i32 left = x;
    if (horizontal == anchor_right) {
        left = saturated_add(x, -width);
    } else if (horizontal == anchor_hcenter) {
        left = saturated_add(x, -(width / 2));
    }

    i32 top = y;
    if (vertical == anchor_bottom) {
        top = saturated_add(y, -height);
    } else if (vertical == anchor_vcenter) {
        top = saturated_add(y, -(height / 2));
    } else if (vertical == anchor_baseline) {
        top = saturated_add(y, -baseline);
    }
    return Rect {.x = left, .y = top, .width = width, .height = height};
}

Status draw_line(Image& target,
                 const GraphicsContext& context,
                 i32 x1,
                 i32 y1,
                 i32 x2,
                 i32 y2) {
    auto mutable_target = require_mutable(target);
    if (!mutable_target) {
        return mutable_target;
    }
    if (!context.rendering_enabled) return {};
    return draw_line_absolute(target,
                              context,
                              saturated_add(x1, context.translate_x),
                              saturated_add(y1, context.translate_y),
                              saturated_add(x2, context.translate_x),
                              saturated_add(y2, context.translate_y));
}

Status fill_rect(Image& target,
                 const GraphicsContext& context,
                 i32 x,
                 i32 y,
                 i32 width,
                 i32 height) {
    auto mutable_target = require_mutable(target);
    if (!mutable_target) {
        return mutable_target;
    }
    if (width <= 0 || height <= 0) {
        return {};
    }
    if (!context.rendering_enabled) {
        return {};
    }
    const Rect fill = intersect(
        Rect {.x = saturated_add(x, context.translate_x),
              .y = saturated_add(y, context.translate_y),
              .width = width,
              .height = height},
        context.clip);
    if (empty(fill)) {
        return {};
    }
    if (alpha(context.color) == 255U) {
        const Pixel device_color = rgb565_roundtrip(context.color);
        auto pixels = target.mutable_pixels();
        const usize stride = static_cast<usize>(target.width());
        if (fill.x == 0 && fill.width == target.width()) {
            const usize offset = static_cast<usize>(fill.y) * stride;
            const usize count = static_cast<usize>(fill.height) * stride;
            std::fill_n(
                pixels.begin() + static_cast<std::ptrdiff_t>(offset),
                count,
                device_color);
        } else {
            for (i32 row = 0; row < fill.height; ++row) {
                const usize offset =
                    static_cast<usize>(fill.y + row) * stride +
                    static_cast<usize>(fill.x);
                std::fill_n(
                    pixels.begin() + static_cast<std::ptrdiff_t>(offset),
                    fill.width,
                    device_color);
            }
        }
        target.mark_dirty_region(fill.x, fill.y, fill.width, fill.height);
        return {};
    }
    auto pixels = target.mutable_pixels();
    const usize target_stride = static_cast<usize>(target.width());
    const usize fill_width = static_cast<usize>(fill.width);
    const usize fill_height = static_cast<usize>(fill.height);
    std::atomic<bool> changed {false};
    const auto fill_rows = [&](usize row_begin, usize row_end) {
        bool local_changed = false;
        for (usize row = row_begin; row < row_end; ++row) {
            const usize offset =
                (static_cast<usize>(fill.y) + row) * target_stride +
                static_cast<usize>(fill.x);
            for (usize column = 0; column < fill_width; ++column) {
                local_changed = composite_device_pixel(
                    context.color, pixels[offset + column]) || local_changed;
            }
        }
        if (local_changed) changed.store(true, std::memory_order_relaxed);
    };
    if (fill_width * fill_height >= kParallelBlitPixels && fill_height >= 4U) {
        const usize rows_per_chunk = std::max<usize>(
            1U, 4U * 1024U / std::max<usize>(fill_width, 1U));
        runtime::shared_compute_executor().parallel_for(
            fill_height, 4U, rows_per_chunk, fill_rows);
    } else {
        fill_rows(0U, fill_height);
    }
    if (changed.load(std::memory_order_relaxed)) {
        target.mark_dirty_region(fill.x, fill.y, fill.width, fill.height);
    }
    return {};
}

Status clear_rect(Image& target,
                  const GraphicsContext& context,
                  i32 x,
                  i32 y,
                  i32 width,
                  i32 height) {
    auto mutable_target = require_mutable(target);
    if (!mutable_target) {
        return mutable_target;
    }
    if (width <= 0 || height <= 0 || !context.rendering_enabled) {
        return {};
    }
    const Rect clear = intersect(
        Rect {.x = saturated_add(x, context.translate_x),
              .y = saturated_add(y, context.translate_y),
              .width = width,
              .height = height},
        context.clip);
    if (empty(clear)) {
        return {};
    }
    auto pixels = target.mutable_pixels();
    const usize stride = static_cast<usize>(target.width());
    if (clear.x == 0 && clear.width == target.width()) {
        const usize offset = static_cast<usize>(clear.y) * stride;
        const usize count = static_cast<usize>(clear.height) * stride;
        std::fill_n(pixels.begin() + static_cast<std::ptrdiff_t>(offset),
                    count,
                    0U);
    } else {
        for (i32 row = 0; row < clear.height; ++row) {
            const usize offset =
                static_cast<usize>(clear.y + row) * stride +
                static_cast<usize>(clear.x);
            std::fill_n(
                pixels.begin() + static_cast<std::ptrdiff_t>(offset),
                clear.width,
                0U);
        }
    }
    target.mark_dirty_region(clear.x, clear.y, clear.width, clear.height);
    return {};
}

Status draw_rect(Image& target,
                 const GraphicsContext& context,
                 i32 x,
                 i32 y,
                 i32 width,
                 i32 height) {
    if (width < 0 || height < 0) {
        return {};
    }
    const i32 right = saturated_add(x, width);
    const i32 bottom = saturated_add(y, height);
    auto status = draw_line(target, context, x, y, right, y);
    if (!status) return status;
    status = draw_line(target, context, x, y, x, bottom);
    if (!status) return status;
    status = draw_line(target, context, right, y, right, bottom);
    if (!status) return status;
    return draw_line(target, context, x, bottom, right, bottom);
}

Status draw_round_rect(Image& target,
                       const GraphicsContext& context,
                       i32 x,
                       i32 y,
                       i32 width,
                       i32 height,
                       i32 arc_width,
                       i32 arc_height,
                       bool fill) {
    auto mutable_target = require_mutable(target);
    if (!mutable_target) return mutable_target;
    if (width <= 0 || height <= 0) return {};
    if (!context.rendering_enabled) return {};
    const i32 radius_x = half_magnitude(arc_width, width / 2);
    const i32 radius_y = half_magnitude(arc_height, height / 2);
    const i32 absolute_x = saturated_add(x, context.translate_x);
    const i32 absolute_y = saturated_add(y, context.translate_y);
    const Rect visible = intersect(
        Rect {.x = absolute_x, .y = absolute_y,
              .width = width, .height = height},
        context.clip);
    for (i32 destination_y = visible.y;
         destination_y < visible.y + visible.height;
         ++destination_y) {
        const i32 local_y = static_cast<i32>(
            static_cast<i64>(destination_y) - absolute_y);
        for (i32 destination_x = visible.x;
             destination_x < visible.x + visible.width;
             ++destination_x) {
            const i32 local_x = static_cast<i32>(
                static_cast<i64>(destination_x) - absolute_x);
            if (!rounded_contains(local_x, local_y, width, height,
                                  radius_x, radius_y)) {
                continue;
            }
            if (!fill) {
                const bool inner = width > 2 && height > 2 &&
                    rounded_contains(local_x - 1,
                                     local_y - 1,
                                     width - 2,
                                     height - 2,
                                     std::max(0, radius_x - 1),
                                     std::max(0, radius_y - 1));
                if (inner) continue;
            }
            auto stored = target.set_pixel(destination_x,
                                           destination_y,
                                           context.color,
                                           true);
            if (!stored) return stored;
        }
    }
    return {};
}

Status draw_arc(Image& target,
                const GraphicsContext& context,
                i32 x,
                i32 y,
                i32 width,
                i32 height,
                i32 start_angle,
                i32 arc_angle,
                bool fill) {
    auto mutable_target = require_mutable(target);
    if (!mutable_target) return mutable_target;
    if (width <= 0 || height <= 0 || arc_angle == 0) return {};
    if (!context.rendering_enabled) return {};
    const i32 absolute_x = saturated_add(x, context.translate_x);
    const i32 absolute_y = saturated_add(y, context.translate_y);
    const double center_x = static_cast<double>(absolute_x) +
                            static_cast<double>(width - 1) / 2.0;
    const double center_y = static_cast<double>(absolute_y) +
                            static_cast<double>(height - 1) / 2.0;
    const double radius_x = static_cast<double>(width - 1) / 2.0;
    const double radius_y = static_cast<double>(height - 1) / 2.0;
    const i32 normalized_start = start_angle % 360;
    const i32 clamped_arc = std::clamp(arc_angle, -360, 360);
    const double desired_steps = std::ceil(
        std::abs(static_cast<double>(clamped_arc)) *
        static_cast<double>(std::max(width, height)) / 90.0);
    const i64 target_step_budget = std::clamp<i64>(
        8LL * (static_cast<i64>(target.width()) + target.height()),
        64,
        1'000'000);
    const i32 steps = static_cast<i32>(std::clamp<double>(
        desired_steps,
        1.0,
        static_cast<double>(target_step_budget)));
    i32 previous_x = rounded_i32(
        center_x + radius_x * std::cos(normalized_start * kPi / 180.0));
    i32 previous_y = rounded_i32(
        center_y - radius_y * std::sin(normalized_start * kPi / 180.0));
    for (i32 step = 0; step <= steps; ++step) {
        const double progress = static_cast<double>(step) / steps;
        const double angle_value =
            (static_cast<double>(normalized_start) +
             clamped_arc * progress) * kPi / 180.0;
        const i32 current_x = rounded_i32(
            center_x + radius_x * std::cos(angle_value));
        const i32 current_y = rounded_i32(
            center_y - radius_y * std::sin(angle_value));
        auto status = draw_line_absolute(target,
                                         context,
                                         previous_x,
                                         previous_y,
                                         current_x,
                                         current_y);
        if (!status) return status;
        if (fill) {
            status = draw_line_absolute(target,
                                        context,
                                        rounded_i32(center_x),
                                        rounded_i32(center_y),
                                        current_x,
                                        current_y);
            if (!status) return status;
        }
        previous_x = current_x;
        previous_y = current_y;
    }
    return {};
}

Status fill_triangle(Image& target,
                     const GraphicsContext& context,
                     i32 x1,
                     i32 y1,
                     i32 x2,
                     i32 y2,
                     i32 x3,
                     i32 y3) {
    auto mutable_target = require_mutable(target);
    if (!mutable_target) return mutable_target;
    if (!context.rendering_enabled) return {};
    x1 = saturated_add(x1, context.translate_x);
    y1 = saturated_add(y1, context.translate_y);
    x2 = saturated_add(x2, context.translate_x);
    y2 = saturated_add(y2, context.translate_y);
    x3 = saturated_add(x3, context.translate_x);
    y3 = saturated_add(y3, context.translate_y);
    const i64 minimum_x = std::min<i64>({x1, x2, x3});
    const i64 maximum_x = std::max<i64>({x1, x2, x3});
    const i64 minimum_y = std::min<i64>({y1, y2, y3});
    const i64 maximum_y = std::max<i64>({y1, y2, y3});
    const i64 clip_right = static_cast<i64>(context.clip.x) +
                           context.clip.width - 1;
    const i64 clip_bottom = static_cast<i64>(context.clip.y) +
                            context.clip.height - 1;
    const i64 visible_left = std::max<i64>(minimum_x, context.clip.x);
    const i64 visible_right = std::min(maximum_x, clip_right);
    const i64 visible_top = std::max<i64>(minimum_y, context.clip.y);
    const i64 visible_bottom = std::min(maximum_y, clip_bottom);
    const auto edge = [](i32 ax, i32 ay, i32 bx, i32 by,
                         i32 px, i32 py) noexcept -> long double {
        return (static_cast<long double>(bx) - ax) *
                   (static_cast<long double>(py) - ay) -
               (static_cast<long double>(by) - ay) *
                   (static_cast<long double>(px) - ax);
    };
    long double area = edge(x1, y1, x2, y2, x3, y3);
    if (area == 0.0L) {
        auto status = draw_line_absolute(target, context, x1, y1, x2, y2);
        if (!status) return status;
        status = draw_line_absolute(target, context, x2, y2, x3, y3);
        if (!status) return status;
        return draw_line_absolute(target, context, x3, y3, x1, y1);
    }
    if (area < 0.0L) {
        std::swap(x2, x3);
        std::swap(y2, y3);
        area = -area;
    }
    const auto top_left = [](i32 ax, i32 ay, i32 bx, i32 by) noexcept {
        const i64 delta_x = static_cast<i64>(bx) - ax;
        const i64 delta_y = static_cast<i64>(by) - ay;
        return delta_y < 0 || (delta_y == 0 && delta_x > 0);
    };
    const bool first_top_left = top_left(x1, y1, x2, y2);
    const bool second_top_left = top_left(x2, y2, x3, y3);
    const bool third_top_left = top_left(x3, y3, x1, y1);
    if (empty(context.clip) || visible_left > visible_right ||
        visible_top > visible_bottom) {
        return {};
    }
    for (i64 y_cursor = visible_top;
         y_cursor <= visible_bottom;
         ++y_cursor) {
        const i32 y_value = static_cast<i32>(y_cursor);
        for (i64 x_cursor = visible_left;
             x_cursor <= visible_right;
             ++x_cursor) {
            const i32 x_value = static_cast<i32>(x_cursor);
            const long double first = edge(x1, y1, x2, y2,
                                           x_value, y_value);
            const long double second = edge(x2, y2, x3, y3,
                                            x_value, y_value);
            const long double third = edge(x3, y3, x1, y1,
                                           x_value, y_value);
            const bool inside_first = first > 0.0L ||
                (first == 0.0L && first_top_left);
            const bool inside_second = second > 0.0L ||
                (second == 0.0L && second_top_left);
            const bool inside_third = third > 0.0L ||
                (third == 0.0L && third_top_left);
            if (!inside_first || !inside_second || !inside_third) continue;
            auto stored = target.set_pixel(x_value,
                                           y_value,
                                           context.color,
                                           true);
            if (!stored) return stored;
        }
    }
    return {};
}

Status draw_image(Image& target,
                  const GraphicsContext& context,
                  const Image& source,
                  i32 x,
                  i32 y,
                  i32 anchor) {
    auto placement = anchored_rect(saturated_add(x, context.translate_x),
                                   saturated_add(y, context.translate_y),
                                   source.width(),
                                   source.height(),
                                   anchor,
                                   false);
    if (!placement) return std::unexpected(placement.error());
    auto mutable_target = require_mutable(target);
    if (!mutable_target) return mutable_target;
    if (!context.rendering_enabled) return {};
    const Rect visible = intersect(
        intersect(*placement, context.clip),
        target_bounds(target));
    if (empty(visible)) {
        return {};
    }
    const i32 first_source_x = static_cast<i32>(
        static_cast<i64>(visible.x) - placement->x);
    const i32 first_source_y = static_cast<i32>(
        static_cast<i64>(visible.y) - placement->y);
    const Rect sampled_source {
        .x = first_source_x,
        .y = first_source_y,
        .width = visible.width,
        .height = visible.height,
    };
    const bool needs_snapshot = &target == &source &&
        rectangles_overlap(sampled_source, visible);
    std::vector<Pixel> snapshot;
    if (needs_snapshot) {
        auto snapshot_count = validated_pixel_count(visible.width,
                                                    visible.height);
        if (!snapshot_count) {
            return std::unexpected(snapshot_count.error());
        }
        snapshot.resize(*snapshot_count);
        for (i32 row = 0; row < visible.height; ++row) {
            const usize source_offset =
                static_cast<usize>(first_source_y + row) *
                    static_cast<usize>(source.width()) +
                static_cast<usize>(first_source_x);
            const usize destination_offset =
                static_cast<usize>(row) *
                static_cast<usize>(visible.width);
            std::copy_n(source.pixels().begin() +
                            static_cast<std::ptrdiff_t>(source_offset),
                        visible.width,
                        snapshot.begin() +
                            static_cast<std::ptrdiff_t>(destination_offset));
        }
    }
    auto target_pixels = target.mutable_pixels();
    const auto source_pixels = source.pixels();
    const auto source_device_pixels = source.device_pixels();
    const usize target_stride = static_cast<usize>(target.width());
    const usize source_stride = static_cast<usize>(source.width());
    const usize visible_width = static_cast<usize>(visible.width);
    const ImageAlphaKind alpha_kind = needs_snapshot
        ? ImageAlphaKind::translucent
        : source.alpha_kind();
    const std::span<const Pixel> blit_source = needs_snapshot
        ? std::span<const Pixel>(snapshot)
        : (alpha_kind == ImageAlphaKind::translucent
            ? source_pixels
            : source_device_pixels);
    const usize blit_source_stride = needs_snapshot
        ? visible_width
        : source_stride;
    const usize blit_source_x = needs_snapshot
        ? 0U
        : static_cast<usize>(first_source_x);
    const usize blit_source_y = needs_snapshot
        ? 0U
        : static_cast<usize>(first_source_y);
    const bool changed = [&]() noexcept {
        switch (alpha_kind) {
        case ImageAlphaKind::opaque:
            return blit_linear_pixels<ImageAlphaKind::opaque>(
                target_pixels, target_stride,
                blit_source, blit_source_stride,
                static_cast<usize>(visible.x),
                static_cast<usize>(visible.y),
                blit_source_x, blit_source_y,
                visible_width, static_cast<usize>(visible.height));
        case ImageAlphaKind::binary:
            return blit_linear_pixels<ImageAlphaKind::binary>(
                target_pixels, target_stride,
                blit_source, blit_source_stride,
                static_cast<usize>(visible.x),
                static_cast<usize>(visible.y),
                blit_source_x, blit_source_y,
                visible_width, static_cast<usize>(visible.height));
        case ImageAlphaKind::translucent:
            return blit_linear_pixels<ImageAlphaKind::translucent>(
                target_pixels, target_stride,
                blit_source, blit_source_stride,
                static_cast<usize>(visible.x),
                static_cast<usize>(visible.y),
                blit_source_x, blit_source_y,
                visible_width, static_cast<usize>(visible.height));
        }
        return false;
    }();
    if (changed) {
        target.mark_dirty_region(
            visible.x, visible.y, visible.width, visible.height);
    }
    return {};
}

Status draw_region(Image& target,
                   const GraphicsContext& context,
                   const Image& source,
                   i32 source_x,
                   i32 source_y,
                   i32 width,
                   i32 height,
                   Transform transform_value,
                   i32 x,
                   i32 y,
                   i32 anchor) {
    if (source_x < 0 || source_y < 0 || width < 0 || height < 0 ||
        static_cast<i64>(source_x) + width > source.width() ||
        static_cast<i64>(source_y) + height > source.height()) {
        return fail(ErrorCode::out_of_range,
                    "image region is outside the source image");
    }
    auto mutable_target = require_mutable(target);
    if (!mutable_target) return mutable_target;

    // phoneME normalizes the destination anchor against the untransformed
    // source-region dimensions before applying the Sprite transform. This is
    // observable for non-square regions rotated by 90/270 degrees.
    auto anchor_placement = anchored_rect(
        saturated_add(x, context.translate_x),
        saturated_add(y, context.translate_y),
        width,
        height,
        anchor,
        false);
    if (!anchor_placement) {
        return std::unexpected(anchor_placement.error());
    }
    if (!context.rendering_enabled) return {};

    const Size output_size = transformed_size(width,
                                              height,
                                              transform_value);
    const Rect placement {
        .x = anchor_placement->x,
        .y = anchor_placement->y,
        .width = output_size.width,
        .height = output_size.height,
    };
    const Rect visible = intersect(
        intersect(placement, context.clip),
        target_bounds(target));
    if (empty(visible)) {
        return {};
    }
    const Rect source_bounds {
        .x = source_x,
        .y = source_y,
        .width = width,
        .height = height,
    };
    const bool needs_snapshot = &target == &source &&
        rectangles_overlap(source_bounds, visible);
    std::vector<Pixel> snapshot;
    if (needs_snapshot) {
        auto snapshot_count = validated_pixel_count(width, height);
        if (!snapshot_count) {
            return std::unexpected(snapshot_count.error());
        }
        snapshot.resize(*snapshot_count);
        for (i32 row = 0; row < height; ++row) {
            const usize source_offset =
                static_cast<usize>(source_y + row) *
                    static_cast<usize>(source.width()) +
                static_cast<usize>(source_x);
            const usize destination_offset =
                static_cast<usize>(row) * static_cast<usize>(width);
            std::copy_n(source.pixels().begin() +
                            static_cast<std::ptrdiff_t>(source_offset),
                        width,
                        snapshot.begin() +
                            static_cast<std::ptrdiff_t>(destination_offset));
        }
    }
    auto target_pixels = target.mutable_pixels();
    const auto source_pixels = source.pixels();
    const auto source_device_pixels = source.device_pixels();
    const usize target_stride = static_cast<usize>(target.width());
    const i64 source_stride = needs_snapshot
        ? static_cast<i64>(width)
        : static_cast<i64>(source.width());
    const ImageAlphaKind alpha_kind = needs_snapshot
        ? ImageAlphaKind::translucent
        : source.alpha_kind();
    const auto strided_source = alpha_kind == ImageAlphaKind::translucent
        ? source_pixels
        : source_device_pixels;
    const Pixel* source_base = needs_snapshot
        ? snapshot.data()
        : strided_source.data() +
            static_cast<usize>(source_y) * static_cast<usize>(source.width()) +
            static_cast<usize>(source_x);

    // Transform is affine with coefficients in {-1, 0, 1}. Resolve it once per
    // blit and advance a signed source index instead of switching and
    // recomputing x/y for every destination pixel.
    const TransformSteps steps = transform_steps(width, height, transform_value);
    const i64 first_local_x = static_cast<i64>(visible.x) - placement.x;
    const i64 first_local_y = static_cast<i64>(visible.y) - placement.y;
    const i64 first_source_x = steps.base_x +
        static_cast<i64>(steps.column_x) * first_local_x +
        static_cast<i64>(steps.row_x) * first_local_y;
    const i64 first_source_y = steps.base_y +
        static_cast<i64>(steps.column_y) * first_local_x +
        static_cast<i64>(steps.row_y) * first_local_y;
    const i64 column_step =
        static_cast<i64>(steps.column_y) * source_stride + steps.column_x;
    const i64 row_step =
        static_cast<i64>(steps.row_y) * source_stride + steps.row_x;
    const i64 first_source_index =
        first_source_y * source_stride + first_source_x;
    const bool changed = [&]() noexcept {
        switch (alpha_kind) {
        case ImageAlphaKind::opaque:
            return blit_strided_pixels<ImageAlphaKind::opaque>(
                target_pixels, target_stride, source_base,
                first_source_index, column_step, row_step,
                static_cast<usize>(visible.x),
                static_cast<usize>(visible.y),
                static_cast<usize>(visible.width),
                static_cast<usize>(visible.height));
        case ImageAlphaKind::binary:
            return blit_strided_pixels<ImageAlphaKind::binary>(
                target_pixels, target_stride, source_base,
                first_source_index, column_step, row_step,
                static_cast<usize>(visible.x),
                static_cast<usize>(visible.y),
                static_cast<usize>(visible.width),
                static_cast<usize>(visible.height));
        case ImageAlphaKind::translucent:
            return blit_strided_pixels<ImageAlphaKind::translucent>(
                target_pixels, target_stride, source_base,
                first_source_index, column_step, row_step,
                static_cast<usize>(visible.x),
                static_cast<usize>(visible.y),
                static_cast<usize>(visible.width),
                static_cast<usize>(visible.height));
        }
        return false;
    }();
    if (changed) {
        target.mark_dirty_region(
            visible.x, visible.y, visible.width, visible.height);
    }
    return {};
}

Status copy_area(Image& target,
                 const GraphicsContext& context,
                 i32 source_x,
                 i32 source_y,
                 i32 width,
                 i32 height,
                 i32 x,
                 i32 y,
                 i32 anchor) {
    const i32 absolute_source_x = saturated_add(source_x, context.translate_x);
    const i32 absolute_source_y = saturated_add(source_y, context.translate_y);
    return draw_region(target,
                       context,
                       target,
                       absolute_source_x,
                       absolute_source_y,
                       width,
                       height,
                       Transform::none,
                       x,
                       y,
                       anchor);
}

Status draw_rgb(Image& target,
                const GraphicsContext& context,
                std::span<const Pixel> pixels,
                i32 offset,
                i32 scan_length,
                i32 x,
                i32 y,
                i32 width,
                i32 height,
                bool process_alpha) {
    auto mutable_target = require_mutable(target);
    if (!mutable_target) return mutable_target;

    // Keep the exact validation order and index formula used by phoneME's
    // javax_microedition_lcdui_Graphics_drawRGB KNI implementation. In
    // particular, zero dimensions and zero scanlength are validated against
    // the source array first, then accepted as no-ops; negative dimensions are
    // also left to the low-level renderer, which treats an empty rectangle as
    // a no-op rather than throwing IllegalArgumentException.
    const i64 row_delta = height == 0
        ? 0
        : (static_cast<i64>(height) - 1) * scan_length;
    const i64 maximum = static_cast<i64>(offset) +
        (width == 0 ? 0 : static_cast<i64>(width) - 1) +
        (scan_length < 0 ? 0 : row_delta);
    const i64 minimum = static_cast<i64>(offset) +
        (scan_length < 0 ? row_delta : 0);
    const i64 pixel_count = static_cast<i64>(pixels.size());
    if (maximum >= pixel_count || minimum < 0 ||
        maximum < 0 || minimum >= pixel_count) {
        return fail(ErrorCode::out_of_range,
                    "drawRGB source slice exceeds int[]");
    }
    if (scan_length == 0 || width == 0 || height == 0 ||
        width < 0 || height < 0) {
        return {};
    }
    if (!context.rendering_enabled) return {};
    const i32 absolute_x = saturated_add(x, context.translate_x);
    const i32 absolute_y = saturated_add(y, context.translate_y);
    const Rect visible = intersect(
        intersect(Rect {.x = absolute_x, .y = absolute_y,
                        .width = width, .height = height},
                  context.clip),
        target_bounds(target));
    if (empty(visible)) return {};

    auto target_pixels = target.mutable_pixels();
    const usize target_stride = static_cast<usize>(target.width());
    const i64 first_source_column = static_cast<i64>(visible.x) - absolute_x;
    const i64 first_source_row = static_cast<i64>(visible.y) - absolute_y;
    std::atomic<bool> changed {false};
    const usize visible_width = static_cast<usize>(visible.width);
    const usize visible_height = static_cast<usize>(visible.height);
    const auto draw_rows = [&](usize row_begin, usize row_end) {
        bool local_changed = false;
        for (usize row = row_begin; row < row_end; ++row) {
            const i64 source_index = static_cast<i64>(offset) +
                (first_source_row + static_cast<i64>(row)) * scan_length +
                first_source_column;
            const usize destination_offset =
                (static_cast<usize>(visible.y) + row) * target_stride +
                static_cast<usize>(visible.x);
            for (usize column = 0U; column < visible_width; ++column) {
                const Pixel pixel_value = pixels[static_cast<usize>(
                    source_index + static_cast<i64>(column))];
                Pixel& destination = target_pixels[destination_offset + column];
                if (process_alpha) {
                    local_changed =
                        composite_device_pixel(pixel_value, destination) ||
                        local_changed;
                } else {
                    const Pixel device_pixel =
                        rgb565_roundtrip(opaque(pixel_value));
                    if (device_pixel != destination) {
                        destination = device_pixel;
                        local_changed = true;
                    }
                }
            }
        }
        if (local_changed) changed.store(true, std::memory_order_relaxed);
    };
    if (visible_width * visible_height >= kParallelDrawRgbPixels &&
        visible_height >= 4U) {
        const usize rows_per_chunk = std::max<usize>(
            1U, 4U * 1024U / std::max<usize>(visible_width, 1U));
        runtime::shared_compute_executor().parallel_for(
            runtime::WorkClass::frame_critical,
            visible_width * visible_height,
            visible_height, 4U, rows_per_chunk, draw_rows);
    } else {
        draw_rows(0U, visible_height);
    }
    if (changed.load(std::memory_order_relaxed)) {
        target.mark_dirty_region(
            visible.x, visible.y, visible.width, visible.height);
    }
    return {};
}

Status draw_text(Image& target,
                 const GraphicsContext& context,
                 std::span<const char32_t> text,
                 i32 x,
                 i32 y,
                 i32 anchor) {
    auto mutable_target = require_mutable(target);
    if (!mutable_target) return mutable_target;
    const i32 width = context.font.chars_width(text);
    auto placement = anchored_rect(saturated_add(x, context.translate_x),
                                   saturated_add(y, context.translate_y),
                                   width,
                                   context.font.height(),
                                   anchor,
                                   true,
                                   context.font.baseline());
    if (!placement) return std::unexpected(placement.error());
    if (!context.rendering_enabled) return {};
    auto platform = draw_platform_text(target,
                                       context.font,
                                       text,
                                       placement->x,
                                       placement->y,
                                       context.color,
                                       context.clip);
    if (platform) {
        if (context.font.is_underlined() && width > 0) {
            return draw_line_absolute(
                target,
                context,
                placement->x,
                placement->y + context.font.baseline() + 1,
                placement->x + width - 1,
                placement->y + context.font.baseline() + 1);
        }
        return {};
    }
    i32 cursor = placement->x;
    for (char32_t character : text) {
        auto status = draw_glyph(target,
                                 context,
                                 character,
                                 cursor,
                                 placement->y);
        if (!status) return status;
        cursor = saturated_add(cursor, context.font.char_width(character));
    }
    if (context.font.is_underlined() && width > 0) {
        return draw_line_absolute(target,
                                  context,
                                  placement->x,
                                  placement->y + context.font.baseline() + 1,
                                  placement->x + width - 1,
                                  placement->y + context.font.baseline() + 1);
    }
    return {};
}

} // namespace phoneme::graphics
