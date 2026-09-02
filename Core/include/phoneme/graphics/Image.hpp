#pragma once

#include <array>
#include <span>
#include <vector>

#include "phoneme/base/Error.hpp"
#include "phoneme/graphics/Color.hpp"

namespace phoneme::graphics {

enum class Transform : i32 {
    none = 0,
    mirror_rotate_180 = 1,
    mirror = 2,
    rotate_180 = 3,
    mirror_rotate_270 = 4,
    rotate_90 = 5,
    rotate_270 = 6,
    mirror_rotate_90 = 7,
};

// Immutable decoded images are classified once so hot sprite blits can pick a
// compositing kernel before entering the pixel loop. Mutable images use the
// general path because their alpha distribution can change after creation.
enum class ImageAlphaKind : u8 {
    opaque,
    binary,
    translucent,
};

struct Size final {
    i32 width {0};
    i32 height {0};
};

struct ImageRegion final {
    i32 x {0};
    i32 y {0};
    i32 width {0};
    i32 height {0};
};

class Image final {
public:
    static constexpr usize kMaximumDirtyRegions = 8U;
    [[nodiscard]] static Result<Image> create_mutable(i32 width,
                                                       i32 height);
    [[nodiscard]] static Result<Image> create_mutable_argb(i32 width,
                                                            i32 height,
                                                            Pixel initial_pixel = 0U);
    [[nodiscard]] static Result<Image> create_immutable(
        i32 width,
        i32 height,
        std::span<const Pixel> pixels);
    [[nodiscard]] static Result<Image> create_immutable_owned(
        i32 width,
        i32 height,
        std::vector<Pixel> pixels);
    [[nodiscard]] static Result<Image> transformed_region(
        const Image& source,
        i32 x,
        i32 y,
        i32 width,
        i32 height,
        Transform transform);

    [[nodiscard]] i32 width() const noexcept { return width_; }
    [[nodiscard]] i32 height() const noexcept { return height_; }
    [[nodiscard]] bool is_mutable() const noexcept { return mutable_; }
    [[nodiscard]] ImageAlphaKind alpha_kind() const noexcept {
        return alpha_kind_;
    }
    [[nodiscard]] std::span<const Pixel> pixels() const noexcept {
        return pixels_;
    }
    // Immutable opaque/binary assets keep a pre-quantized RGB565 view for
    // sprite blits. Java-facing getRGB()/transforms continue to read pixels(),
    // so exact decoded ARGB data is preserved while the hot display path avoids
    // re-running rgb565_roundtrip() for every sprite pixel on every frame.
    [[nodiscard]] std::span<const Pixel> device_pixels() const noexcept {
        return device_pixels_.empty()
            ? std::span<const Pixel>(pixels_)
            : std::span<const Pixel>(device_pixels_);
    }
    [[nodiscard]] usize storage_bytes() const noexcept {
        return (pixels_.size() + device_pixels_.size()) * sizeof(Pixel);
    }
    [[nodiscard]] std::span<Pixel> mutable_pixels() noexcept {
        return pixels_;
    }
    [[nodiscard]] bool has_dirty_region() const noexcept { return dirty_; }
    [[nodiscard]] ImageRegion dirty_region() const noexcept {
        return dirty_region_;
    }
    [[nodiscard]] std::span<const ImageRegion> dirty_regions() const noexcept {
        return std::span<const ImageRegion>(dirty_regions_.data(),
                                            dirty_region_count_);
    }
    void clear_dirty_region() noexcept;
    void mark_dirty_region(i32 x, i32 y, i32 width, i32 height) noexcept;

    [[nodiscard]] Result<Pixel> pixel(i32 x, i32 y) const;
    [[nodiscard]] Status set_pixel(i32 x,
                                   i32 y,
                                   Pixel pixel,
                                   bool blend);
    // Internal rasterizers that already batch dirty tracking can bypass the
    // per-pixel dirty-island merge and publish one aggregate region afterward.
    [[nodiscard]] Status set_pixel_untracked(i32 x,
                                             i32 y,
                                             Pixel pixel,
                                             bool blend);

private:
    [[nodiscard]] Status store_pixel(i32 x,
                                     i32 y,
                                     Pixel pixel,
                                     bool blend,
                                     bool track_dirty);
    Image(i32 width,
          i32 height,
          bool mutable_image,
          std::vector<Pixel> pixels) noexcept;

    i32 width_ {0};
    i32 height_ {0};
    bool mutable_ {false};
    std::vector<Pixel> pixels_;
    std::vector<Pixel> device_pixels_;
    ImageAlphaKind alpha_kind_ {ImageAlphaKind::translucent};
    bool dirty_ {false};
    ImageRegion dirty_region_ {};
    std::array<ImageRegion, kMaximumDirtyRegions> dirty_regions_ {};
    usize dirty_region_count_ {0U};
};

[[nodiscard]] Result<Transform> transform_from_int(i32 value);
[[nodiscard]] Size transformed_size(i32 width,
                                    i32 height,
                                    Transform transform) noexcept;
[[nodiscard]] Result<Size> validate_dimensions(i32 width, i32 height);
[[nodiscard]] Result<usize> validated_pixel_count(i32 width, i32 height);

} // namespace phoneme::graphics
