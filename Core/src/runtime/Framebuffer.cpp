#include "phoneme/runtime/Framebuffer.hpp"

#include <algorithm>
#include <utility>

namespace phoneme::runtime
{
  namespace
  {

    [[nodiscard]] Result<usize> rgba_size(Dimensions dimensions)
    {
      if (!dimensions.valid() || dimensions.width > Framebuffer::kMaximumDimension ||
          dimensions.height > Framebuffer::kMaximumDimension)
      {
        return fail(ErrorCode::invalid_argument,
                    "framebuffer dimensions are outside the supported range");
      }
      auto width = checked_narrow<usize>(dimensions.width);
      auto height = checked_narrow<usize>(dimensions.height);
      if (!width || !height)
      {
        return fail(ErrorCode::overflow, "framebuffer dimension conversion failed");
      }
      auto pixels = checked_multiply(*width, *height);
      if (!pixels)
      {
        return std::unexpected(pixels.error());
      }
      return checked_multiply(*pixels, 4);
    }

    void copy_to_rgba(std::span<const u8> source,
                      FramePixelFormat format,
                      std::span<u8> destination) noexcept
    {
      if (destination.size() < source.size()) return;
      if (format == FramePixelFormat::rgba8)
      {
        std::copy(source.begin(), source.end(), destination.begin());
        return;
      }
      for (usize offset = 0U; offset + 3U < source.size(); offset += 4U)
      {
        destination[offset] = source[offset + 2U];
        destination[offset + 1U] = source[offset + 1U];
        destination[offset + 2U] = source[offset];
        destination[offset + 3U] = source[offset + 3U];
      }
    }

  } // namespace

  Status Framebuffer::resize(Dimensions dimensions)
  {
    auto required = rgba_size(dimensions);
    if (!required)
    {
      return std::unexpected(required.error());
    }

    std::vector<u8> replacement(*required, 0);
    for (usize offset = 3; offset < replacement.size(); offset += 4)
    {
      replacement[offset] = 0xFFU;
    }

    {
      std::scoped_lock lock(mutex_);
      dimensions_ = dimensions;
      rgba_ = std::move(replacement);
      pixel_format_ = FramePixelFormat::rgba8;
      damage_regions_.clear();
      damage_regions_.push_back(FrameDamageRegion {
          .x = 0,
          .y = 0,
          .width = dimensions.width,
          .height = dimensions.height,
      });
      ++generation_;
    }
    notify_changed();
    return {};
  }

  void Framebuffer::configure_change_sink(ChangeSink sink)
  {
    std::scoped_lock lock(mutex_);
    change_sink_ = std::move(sink);
  }

  void Framebuffer::notify_changed() noexcept
  {
    ChangeSink sink;
    {
      std::scoped_lock lock(mutex_);
      sink = change_sink_;
    }
    if (sink) sink();
  }

  Status Framebuffer::replace(Dimensions dimensions,
                              std::span<const u8> pixels,
                              FramePixelFormat format)
  {
    auto required = rgba_size(dimensions);
    if (!required)
    {
      return std::unexpected(required.error());
    }
    if (pixels.size() != *required)
    {
      return fail(ErrorCode::invalid_argument,
                  "frame byte size does not match its dimensions");
    }

    std::vector<u8> replacement(pixels.begin(), pixels.end());
    {
      std::scoped_lock lock(mutex_);
      dimensions_ = dimensions;
      rgba_ = std::move(replacement);
      pixel_format_ = format;
      damage_regions_.clear();
      damage_regions_.push_back(FrameDamageRegion {
          .x = 0,
          .y = 0,
          .width = dimensions.width,
          .height = dimensions.height,
      });
      ++generation_;
    }
    notify_changed();
    return {};
  }

  Status Framebuffer::replace_exchange(Dimensions dimensions,
                                       std::vector<u8>& pixels,
                                       FramePixelFormat format)
  {
    auto required = rgba_size(dimensions);
    if (!required)
    {
      return std::unexpected(required.error());
    }
    if (pixels.size() != *required)
    {
      return fail(ErrorCode::invalid_argument,
                  "frame byte size does not match its dimensions");
    }

    {
      std::scoped_lock lock(mutex_);
      dimensions_ = dimensions;
      rgba_.swap(pixels);
      pixel_format_ = format;
      damage_regions_.clear();
      damage_regions_.push_back(FrameDamageRegion {
          .x = 0,
          .y = 0,
          .width = dimensions.width,
          .height = dimensions.height,
      });
      ++generation_;
    }
    notify_changed();
    return {};
  }

  Status Framebuffer::update_region(Dimensions dimensions,
                                    i32 x,
                                    i32 y,
                                    i32 width,
                                    i32 height,
                                    std::span<const u8> pixels,
                                    FramePixelFormat format)
  {
    const FrameRegionUpdate update {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .rgba = pixels,
    };
    return update_regions(dimensions,
                          std::span<const FrameRegionUpdate>(&update, 1U),
                          format);
  }

  Status Framebuffer::update_regions(
      Dimensions dimensions,
      std::span<const FrameRegionUpdate> updates,
      FramePixelFormat format)
  {
    auto full_size = rgba_size(dimensions);
    if (!full_size)
    {
      return std::unexpected(full_size.error());
    }
    if (updates.empty()) return {};
    for (const FrameRegionUpdate& update : updates) {
      if (update.x < 0 || update.y < 0 || update.width <= 0 ||
          update.height <= 0 ||
          static_cast<i64>(update.x) + update.width > dimensions.width ||
          static_cast<i64>(update.y) + update.height > dimensions.height)
      {
        return fail(ErrorCode::invalid_argument,
                    "framebuffer update region is outside its dimensions");
      }
      auto region_pixels = checked_multiply(
          static_cast<usize>(update.width),
          static_cast<usize>(update.height));
      if (!region_pixels) return std::unexpected(region_pixels.error());
      auto region_bytes = checked_multiply(*region_pixels,
                                           static_cast<usize>(4));
      if (!region_bytes) return std::unexpected(region_bytes.error());
      if (update.rgba.size() != *region_bytes)
      {
        return fail(ErrorCode::invalid_argument,
                    "framebuffer update byte count does not match its region");
      }
    }

    {
      std::scoped_lock lock(mutex_);
      if (dimensions_.width != dimensions.width ||
          dimensions_.height != dimensions.height ||
          rgba_.size() != *full_size || pixel_format_ != format)
      {
        return fail(ErrorCode::invalid_state,
                    "framebuffer dimensions changed before partial update");
      }

      const usize destination_stride = static_cast<usize>(dimensions.width) * 4U;
      for (const FrameRegionUpdate& update : updates) {
        const usize source_stride = static_cast<usize>(update.width) * 4U;
        for (i32 row = 0; row < update.height; ++row)
        {
          const usize source_offset = static_cast<usize>(row) * source_stride;
          const usize destination_offset =
              static_cast<usize>(update.y + row) * destination_stride +
              static_cast<usize>(update.x) * 4U;
          std::copy_n(
              update.rgba.begin() +
                  static_cast<std::ptrdiff_t>(source_offset),
              source_stride,
              rgba_.begin() +
                  static_cast<std::ptrdiff_t>(destination_offset));
        }
      }
      damage_regions_.clear();
      if (damage_regions_.capacity() < updates.size()) {
        damage_regions_.reserve(updates.size());
      }
      for (const FrameRegionUpdate& update : updates) {
        damage_regions_.push_back(FrameDamageRegion {
            .x = update.x,
            .y = update.y,
            .width = update.width,
            .height = update.height,
        });
      }
      ++generation_;
    }
    notify_changed();
    return {};
  }

  FrameMetadata Framebuffer::metadata() const noexcept
  {
    std::scoped_lock lock(mutex_);
    return FrameMetadata{
        .dimensions = dimensions_,
        .generation = generation_,
        .byte_count = rgba_.size(),
        .pixel_format = pixel_format_,
    };
  }

  FrameMetadata Framebuffer::copy_rgba(
      std::span<u8> destination) const noexcept
  {
    std::scoped_lock lock(mutex_);
    const FrameMetadata result{
        .dimensions = dimensions_,
        .generation = generation_,
        .byte_count = rgba_.size(),
        .pixel_format = FramePixelFormat::rgba8,
    };
    if (!rgba_.empty() && destination.size() >= rgba_.size())
    {
      copy_to_rgba(rgba_, pixel_format_, destination);
    }
    return result;
  }

  std::optional<FrameMetadata> Framebuffer::copy_rgba_since(
      u64 previous_generation,
      std::span<u8> destination) const noexcept
  {
    std::scoped_lock lock(mutex_);
    if (generation_ == previous_generation)
    {
      return std::nullopt;
    }
    const FrameMetadata result{
        .dimensions = dimensions_,
        .generation = generation_,
        .byte_count = rgba_.size(),
        .pixel_format = FramePixelFormat::rgba8,
    };
    if (!rgba_.empty() && destination.size() >= rgba_.size())
    {
      copy_to_rgba(rgba_, pixel_format_, destination);
    }
    return result;
  }

  std::optional<Framebuffer::ReadLease> Framebuffer::acquire_rgba_since(
      u64 previous_generation) const noexcept
  {
    std::unique_lock lock(mutex_);
    if (generation_ == previous_generation || rgba_.empty())
    {
      return std::nullopt;
    }
    const FrameMetadata metadata{
        .dimensions = dimensions_,
        .generation = generation_,
        .byte_count = rgba_.size(),
        .pixel_format = FramePixelFormat::rgba8,
    };
    if (pixel_format_ == FramePixelFormat::bgra8)
    {
      std::vector<u8> converted(rgba_.size());
      copy_to_rgba(rgba_, pixel_format_, converted);
      return ReadLease(
          std::move(lock),
          metadata,
          {},
          std::span<const FrameDamageRegion>(
              damage_regions_.data(), damage_regions_.size()),
          std::move(converted));
    }
    return ReadLease(
        std::move(lock),
        metadata,
        std::span<const u8>(rgba_.data(), rgba_.size()),
        std::span<const FrameDamageRegion>(
            damage_regions_.data(), damage_regions_.size()));
  }

  std::optional<Framebuffer::ReadLease> Framebuffer::acquire_native_since(
      u64 previous_generation) const noexcept
  {
    std::unique_lock lock(mutex_);
    if (generation_ == previous_generation || rgba_.empty())
    {
      return std::nullopt;
    }
    const FrameMetadata metadata{
        .dimensions = dimensions_,
        .generation = generation_,
        .byte_count = rgba_.size(),
        .pixel_format = pixel_format_,
    };
    return ReadLease(
        std::move(lock),
        metadata,
        std::span<const u8>(rgba_.data(), rgba_.size()),
        std::span<const FrameDamageRegion>(
            damage_regions_.data(), damage_regions_.size()));
  }

  FrameSnapshot Framebuffer::snapshot() const
  {
    std::scoped_lock lock(mutex_);
    std::vector<u8> rgba(rgba_.size());
    copy_to_rgba(rgba_, pixel_format_, rgba);
    return FrameSnapshot{
        .dimensions = dimensions_,
        .generation = generation_,
        .rgba = std::move(rgba),
    };
  }

  void Framebuffer::clear() noexcept
  {
    {
      std::scoped_lock lock(mutex_);
      dimensions_ = {};
      rgba_.clear();
      pixel_format_ = FramePixelFormat::rgba8;
      damage_regions_.clear();
      ++generation_;
    }
    notify_changed();
  }

} // namespace phoneme::runtime
