#include "phoneme/graphics/Image.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "phoneme/runtime/ParallelExecutor.hpp"

namespace phoneme::graphics {
namespace {

constexpr usize kMaximumPixels = 64U * 1024U * 1024U;
constexpr usize kParallelTransformPixels = 32U * 1024U;

[[nodiscard]] bool regions_touch_or_overlap(const ImageRegion& left,
                                            const ImageRegion& right) noexcept {
    const i64 left_right = static_cast<i64>(left.x) + left.width;
    const i64 left_bottom = static_cast<i64>(left.y) + left.height;
    const i64 right_right = static_cast<i64>(right.x) + right.width;
    const i64 right_bottom = static_cast<i64>(right.y) + right.height;
    return static_cast<i64>(left.x) <= right_right &&
           static_cast<i64>(right.x) <= left_right &&
           static_cast<i64>(left.y) <= right_bottom &&
           static_cast<i64>(right.y) <= left_bottom;
}

[[nodiscard]] ImageRegion union_region(const ImageRegion& left,
                                       const ImageRegion& right) noexcept {
    const i64 x = std::min<i64>(left.x, right.x);
    const i64 y = std::min<i64>(left.y, right.y);
    const i64 right_edge = std::max<i64>(
        static_cast<i64>(left.x) + left.width,
        static_cast<i64>(right.x) + right.width);
    const i64 bottom_edge = std::max<i64>(
        static_cast<i64>(left.y) + left.height,
        static_cast<i64>(right.y) + right.height);
    return ImageRegion {
        .x = static_cast<i32>(x),
        .y = static_cast<i32>(y),
        .width = static_cast<i32>(right_edge - x),
        .height = static_cast<i32>(bottom_edge - y),
    };
}

[[nodiscard]] ImageAlphaKind classify_alpha(
    std::span<const Pixel> pixels) noexcept {
    ImageAlphaKind kind = ImageAlphaKind::opaque;
    for (const Pixel pixel : pixels) {
        const u8 value = alpha(pixel);
        if (value == 255U) continue;
        if (value == 0U) {
            if (kind == ImageAlphaKind::opaque) {
                kind = ImageAlphaKind::binary;
            }
            continue;
        }
        return ImageAlphaKind::translucent;
    }
    return kind;
}

[[nodiscard]] std::pair<i32, i32> source_coordinate(
    i32 destination_x,
    i32 destination_y,
    i32 width,
    i32 height,
    Transform transform) noexcept {
    switch (transform) {
    case Transform::none:
        return {destination_x, destination_y};
    case Transform::mirror_rotate_180:
        return {destination_x, height - 1 - destination_y};
    case Transform::mirror:
        return {width - 1 - destination_x, destination_y};
    case Transform::rotate_180:
        return {width - 1 - destination_x,
                height - 1 - destination_y};
    case Transform::mirror_rotate_270:
        return {destination_y, destination_x};
    case Transform::rotate_90:
        return {destination_y, height - 1 - destination_x};
    case Transform::rotate_270:
        return {width - 1 - destination_y, destination_x};
    case Transform::mirror_rotate_90:
        return {width - 1 - destination_y,
                height - 1 - destination_x};
    }
    return {destination_x, destination_y};
}

} // namespace

Image::Image(i32 width,
             i32 height,
             bool mutable_image,
             std::vector<Pixel> pixels) noexcept
    : width_(width),
      height_(height),
      mutable_(mutable_image),
      pixels_(std::move(pixels)),
      alpha_kind_(mutable_image
          ? ImageAlphaKind::translucent
          : classify_alpha(pixels_)) {
    if (!mutable_ && alpha_kind_ != ImageAlphaKind::translucent) {
        device_pixels_.resize(pixels_.size());
        std::transform(
            pixels_.begin(), pixels_.end(), device_pixels_.begin(),
            [](Pixel pixel) noexcept { return rgb565_roundtrip(pixel); });
    }
}

Result<Size> validate_dimensions(i32 width, i32 height) {
    if (width <= 0 || height <= 0) {
        return fail(ErrorCode::invalid_argument,
                    "image dimensions must be positive");
    }
    return Size {.width = width, .height = height};
}

Result<usize> validated_pixel_count(i32 width, i32 height) {
    auto validated = validate_dimensions(width, height);
    if (!validated) {
        return std::unexpected(validated.error());
    }
    const auto unsigned_width = static_cast<usize>(width);
    const auto unsigned_height = static_cast<usize>(height);
    if (unsigned_height != 0U &&
        unsigned_width > std::numeric_limits<usize>::max() / unsigned_height) {
        return fail(ErrorCode::overflow, "image pixel count overflows size_t");
    }
    const usize count = unsigned_width * unsigned_height;
    if (count > kMaximumPixels) {
        return fail(ErrorCode::overflow,
                    "image exceeds the portable graphics pixel budget");
    }
    return count;
}

Result<Image> Image::create_mutable(i32 width, i32 height) {
    return create_mutable_argb(width, height, 0xFFFFFFFFU);
}

Result<Image> Image::create_mutable_argb(i32 width,
                                         i32 height,
                                         Pixel initial_pixel) {
    auto count = validated_pixel_count(width, height);
    if (!count) {
        return std::unexpected(count.error());
    }
    Image image(width,
                height,
                true,
                std::vector<Pixel>(*count, initial_pixel));
    image.mark_dirty_region(0, 0, width, height);
    return image;
}

Result<Image> Image::create_immutable(i32 width,
                                      i32 height,
                                      std::span<const Pixel> pixels) {
    auto count = validated_pixel_count(width, height);
    if (!count) {
        return std::unexpected(count.error());
    }
    if (pixels.size() != *count) {
        return fail(ErrorCode::invalid_argument,
                    "immutable image pixel count does not match dimensions");
    }
    return Image(width,
                 height,
                 false,
                 std::vector<Pixel>(pixels.begin(), pixels.end()));
}

Result<Image> Image::create_immutable_owned(i32 width,
                                            i32 height,
                                            std::vector<Pixel> pixels) {
    auto count = validated_pixel_count(width, height);
    if (!count) {
        return std::unexpected(count.error());
    }
    if (pixels.size() != *count) {
        return fail(ErrorCode::invalid_argument,
                    "immutable image pixel count does not match dimensions");
    }
    return Image(width, height, false, std::move(pixels));
}

Result<Image> Image::transformed_region(const Image& source,
                                        i32 x,
                                        i32 y,
                                        i32 width,
                                        i32 height,
                                        Transform transform) {
    if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
        x > source.width_ - width || y > source.height_ - height) {
        return fail(ErrorCode::out_of_range,
                    "image region is outside the source image");
    }
    const Size output_size = transformed_size(width, height, transform);
    auto count = validated_pixel_count(output_size.width,
                                       output_size.height);
    if (!count) {
        return std::unexpected(count.error());
    }
    std::vector<Pixel> output(*count, 0U);
    const usize output_width = static_cast<usize>(output_size.width);
    const usize output_height = static_cast<usize>(output_size.height);
    const auto transform_rows = [&](usize row_begin, usize row_end) {
        for (usize destination_y = row_begin;
             destination_y < row_end;
             ++destination_y) {
            for (usize destination_x = 0U;
                 destination_x < output_width;
                 ++destination_x) {
                const auto [source_x, source_y] = source_coordinate(
                    static_cast<i32>(destination_x),
                    static_cast<i32>(destination_y),
                    width,
                    height,
                    transform);
                const usize source_index =
                    static_cast<usize>(y + source_y) *
                        static_cast<usize>(source.width_) +
                    static_cast<usize>(x + source_x);
                const usize destination_index =
                    destination_y * output_width + destination_x;
                output[destination_index] = source.pixels_[source_index];
            }
        }
    };
    if (*count >= kParallelTransformPixels && output_height >= 4U) {
        const usize rows_per_chunk = std::max<usize>(
            1U, 4U * 1024U / std::max<usize>(output_width, 1U));
        runtime::shared_compute_executor().parallel_for(
            output_height, 4U, rows_per_chunk, transform_rows);
    } else {
        transform_rows(0U, output_height);
    }
    return Image(output_size.width,
                 output_size.height,
                 false,
                 std::move(output));
}

void Image::clear_dirty_region() noexcept {
    dirty_ = false;
    dirty_region_ = {};
    dirty_region_count_ = 0U;
}

void Image::mark_dirty_region(i32 x,
                              i32 y,
                              i32 width,
                              i32 height) noexcept {
    if (!mutable_ || width <= 0 || height <= 0) {
        return;
    }
    const i64 left = std::max<i64>(0, x);
    const i64 top = std::max<i64>(0, y);
    const i64 right = std::min<i64>(
        width_, static_cast<i64>(x) + static_cast<i64>(width));
    const i64 bottom = std::min<i64>(
        height_, static_cast<i64>(y) + static_cast<i64>(height));
    if (right <= left || bottom <= top) {
        return;
    }
    ImageRegion region {
        .x = static_cast<i32>(left),
        .y = static_cast<i32>(top),
        .width = static_cast<i32>(right - left),
        .height = static_cast<i32>(bottom - top),
    };
    if (!dirty_) {
        dirty_ = true;
        dirty_region_ = region;
        dirty_regions_[0] = region;
        dirty_region_count_ = 1U;
        return;
    }
    dirty_region_ = union_region(dirty_region_, region);

    // Preserve a small set of disjoint dirty islands so two distant sprites
    // do not force conversion of the large empty rectangle between them.
    // Touching/overlapping islands are merged transitively. If the fixed
    // budget is exceeded, collapse safely to the bounding rectangle.
    for (usize index = 0U; index < dirty_region_count_;) {
        if (!regions_touch_or_overlap(region, dirty_regions_[index])) {
            ++index;
            continue;
        }
        region = union_region(region, dirty_regions_[index]);
        --dirty_region_count_;
        if (index != dirty_region_count_) {
            dirty_regions_[index] = dirty_regions_[dirty_region_count_];
        }
        index = 0U;
    }
    if (dirty_region_count_ < dirty_regions_.size()) {
        dirty_regions_[dirty_region_count_++] = region;
        return;
    }
    dirty_regions_[0] = dirty_region_;
    dirty_region_count_ = 1U;
}

Result<Pixel> Image::pixel(i32 x, i32 y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return fail(ErrorCode::out_of_range,
                    "pixel coordinate is outside the image");
    }
    return pixels_[static_cast<usize>(y) * static_cast<usize>(width_) +
                   static_cast<usize>(x)];
}

Status Image::set_pixel(i32 x,
                        i32 y,
                        Pixel pixel_value,
                        bool blend) {
    if (!mutable_) {
        return fail(ErrorCode::invalid_state,
                    "cannot draw into an immutable image");
    }
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return {};
    }
    Pixel& destination =
        pixels_[static_cast<usize>(y) * static_cast<usize>(width_) +
                static_cast<usize>(x)];
    Pixel composited = destination;
    if (!blend) {
        composited = rgb565_roundtrip(pixel_value);
    } else {
        const u8 source_alpha = alpha(pixel_value);
        if (source_alpha == 0U) return {};
        composited = source_alpha == 255U
            ? rgb565_roundtrip(pixel_value)
            : rgb565_roundtrip(source_over(pixel_value, destination));
    }
    if (composited != destination) {
        destination = composited;
        mark_dirty_region(x, y, 1, 1);
    }
    return {};
}

Result<Transform> transform_from_int(i32 value) {
    if (value < static_cast<i32>(Transform::none) ||
        value > static_cast<i32>(Transform::mirror_rotate_90)) {
        return fail(ErrorCode::invalid_argument,
                    "invalid MIDP sprite transform");
    }
    return static_cast<Transform>(value);
}

Size transformed_size(i32 width,
                      i32 height,
                      Transform transform) noexcept {
    switch (transform) {
    case Transform::mirror_rotate_270:
    case Transform::rotate_90:
    case Transform::rotate_270:
    case Transform::mirror_rotate_90:
        return Size {.width = height, .height = width};
    default:
        return Size {.width = width, .height = height};
    }
}

} // namespace phoneme::graphics
