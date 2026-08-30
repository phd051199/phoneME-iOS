#pragma once

#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "phoneme/base/Checked.hpp"

namespace phoneme::runtime {

enum class FramePixelFormat : u8 {
    rgba8 = 0,
    bgra8 = 1,
};

struct FrameMetadata final {
    Dimensions dimensions;
    u64 generation {0};
    usize byte_count {0};
    FramePixelFormat pixel_format {FramePixelFormat::rgba8};
};

struct FrameSnapshot final {
    Dimensions dimensions;
    u64 generation {0};
    std::vector<u8> rgba;
};

struct FrameRegionUpdate final {
    i32 x {0};
    i32 y {0};
    i32 width {0};
    i32 height {0};
    std::span<const u8> rgba;
};

struct FrameDamageRegion final {
    i32 x {0};
    i32 y {0};
    i32 width {0};
    i32 height {0};
};

class Framebuffer final {
public:
    static constexpr i32 kMaximumDimension = 4'096;

    class ReadLease final {
    public:
        ReadLease(ReadLease&&) noexcept = default;
        ReadLease& operator=(ReadLease&&) noexcept = default;
        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;

        [[nodiscard]] const FrameMetadata& metadata() const noexcept {
            return metadata_;
        }
        [[nodiscard]] std::span<const u8> pixels() const noexcept {
            return pixels_;
        }
        [[nodiscard]] std::span<const FrameDamageRegion> damage_regions()
            const noexcept {
            return damage_regions_;
        }

    private:
        friend class Framebuffer;
        ReadLease(std::unique_lock<std::mutex> lock,
                  FrameMetadata metadata,
                  std::span<const u8> pixels,
                  std::span<const FrameDamageRegion> damage_regions,
                  std::vector<u8> owned_pixels = {}) noexcept
            : lock_(std::move(lock)),
              metadata_(metadata),
              owned_pixels_(std::move(owned_pixels)),
              pixels_(pixels),
              damage_regions_(damage_regions) {
            if (!owned_pixels_.empty()) {
                pixels_ = std::span<const u8>(owned_pixels_.data(),
                                              owned_pixels_.size());
            }
        }

        std::unique_lock<std::mutex> lock_;
        FrameMetadata metadata_;
        std::vector<u8> owned_pixels_;
        std::span<const u8> pixels_;
        std::span<const FrameDamageRegion> damage_regions_;
    };

    [[nodiscard]] Status resize(Dimensions dimensions);
    [[nodiscard]] Status replace(Dimensions dimensions,
                                 std::span<const u8> pixels,
                                 FramePixelFormat format = FramePixelFormat::rgba8);
    [[nodiscard]] Status replace_exchange(Dimensions dimensions,
                                          std::vector<u8>& pixels,
                                          FramePixelFormat format = FramePixelFormat::rgba8);
    [[nodiscard]] Status update_region(Dimensions dimensions,
                                       i32 x,
                                       i32 y,
                                       i32 width,
                                       i32 height,
                                       std::span<const u8> pixels,
                                       FramePixelFormat format = FramePixelFormat::rgba8);
    [[nodiscard]] Status update_regions(
        Dimensions dimensions,
        std::span<const FrameRegionUpdate> updates,
        FramePixelFormat format = FramePixelFormat::rgba8);
    [[nodiscard]] FrameMetadata metadata() const noexcept;
    [[nodiscard]] FrameMetadata copy_rgba(std::span<u8> destination) const noexcept;
    [[nodiscard]] std::optional<FrameMetadata> copy_rgba_since(
        u64 previous_generation,
        std::span<u8> destination) const noexcept;
    [[nodiscard]] std::optional<ReadLease> acquire_rgba_since(
        u64 previous_generation) const noexcept;
    [[nodiscard]] std::optional<ReadLease> acquire_native_since(
        u64 previous_generation) const noexcept;
    [[nodiscard]] FrameSnapshot snapshot() const;
    void clear() noexcept;

private:
    mutable std::mutex mutex_;
    Dimensions dimensions_;
    u64 generation_ {0};
    std::vector<u8> rgba_;
    FramePixelFormat pixel_format_ {FramePixelFormat::rgba8};
    std::vector<FrameDamageRegion> damage_regions_;
};

} // namespace phoneme::runtime
