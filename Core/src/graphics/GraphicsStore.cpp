#include "phoneme/graphics/GraphicsStore.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace phoneme::graphics {
namespace {

constexpr usize kGraphicsGcAllocationInterval = 16U * 1024U * 1024U;
#if defined(__EMSCRIPTEN__)
constexpr usize kGraphicsStorageLimit = 160U * 1024U * 1024U;
#else
constexpr usize kGraphicsStorageLimit = std::numeric_limits<usize>::max();
#endif

[[nodiscard]] usize image_storage_bytes(const Image& image) noexcept {
    return image.storage_bytes();
}

[[nodiscard]] usize saturated_add(usize left, usize right) noexcept {
    if (right > std::numeric_limits<usize>::max() - left) {
        return std::numeric_limits<usize>::max();
    }
    return left + right;
}

} // namespace

void GraphicsStore::invalidate_lookup_caches() const noexcept {
    for (auto& entry : image_lookup_cache_) entry = {};
    for (auto& entry : context_lookup_cache_) entry = {};
}

Status GraphicsStore::attach_image(u64 object_key, Image image_value) {
    if (object_key == 0U) {
        return fail(ErrorCode::invalid_argument,
                    "cannot attach an image to a null object key");
    }
    const usize next_image_bytes = image_storage_bytes(image_value);
    usize previous_image_bytes = 0U;
    if (const auto previous = images_.find(object_key); previous != images_.end()) {
        previous_image_bytes = image_storage_bytes(previous->second);
    }
    const usize retained_bytes = image_storage_bytes_ >= previous_image_bytes
        ? image_storage_bytes_ - previous_image_bytes
        : 0U;
    if (retained_bytes > kGraphicsStorageLimit ||
        next_image_bytes > kGraphicsStorageLimit - retained_bytes) {
        return fail(ErrorCode::overflow,
                    "native graphics image storage limit exceeded");
    }
    image_storage_bytes_ = retained_bytes + next_image_bytes;
    image_allocation_bytes_since_prune_ = saturated_add(
        image_allocation_bytes_since_prune_, next_image_bytes);
    images_.insert_or_assign(object_key, std::move(image_value));
    invalidate_lookup_caches();
    return {};
}

Status GraphicsStore::attach_context(u64 object_key,
                                     u64 target_image_key,
                                     bool display_target) {
    if (object_key == 0U || target_image_key == 0U) {
        return fail(ErrorCode::invalid_argument,
                    "graphics context keys must be non-zero");
    }
    auto target = image(target_image_key);
    if (!target) {
        return std::unexpected(target.error());
    }
    if (!(*target)->is_mutable()) {
        return fail(ErrorCode::invalid_state,
                    "graphics contexts require a mutable target image");
    }
    GraphicsContext context;
    context.target_key = target_image_key;
    context.display_target = display_target;
    context.clip = target_bounds(**target);
    contexts_.insert_or_assign(object_key, context);
    invalidate_lookup_caches();
    return {};
}

Result<Image*> GraphicsStore::image(u64 object_key) {
    auto& cached = image_lookup_cache_[lookup_cache_index(object_key)];
    if (cached.key == object_key && cached.value != nullptr) return cached.value;
    auto iterator = images_.find(object_key);
    if (iterator == images_.end()) {
        return fail(ErrorCode::invalid_state,
                    "Java Image has no native graphics payload");
    }
    cached = ImageLookupCacheEntry {.key = object_key, .value = &iterator->second};
    return cached.value;
}

Result<const Image*> GraphicsStore::image(u64 object_key) const {
    auto& cached = image_lookup_cache_[lookup_cache_index(object_key)];
    if (cached.key == object_key && cached.value != nullptr) return cached.value;
    auto iterator = images_.find(object_key);
    if (iterator == images_.end()) {
        return fail(ErrorCode::invalid_state,
                    "Java Image has no native graphics payload");
    }
    cached = ImageLookupCacheEntry {
        .key = object_key,
        .value = const_cast<Image*>(&iterator->second),
    };
    return cached.value;
}

Result<GraphicsContext*> GraphicsStore::context(u64 object_key) {
    auto& cached = context_lookup_cache_[lookup_cache_index(object_key)];
    if (cached.key == object_key && cached.value != nullptr) return cached.value;
    auto iterator = contexts_.find(object_key);
    if (iterator == contexts_.end()) {
        return fail(ErrorCode::invalid_state,
                    "Java Graphics has no native graphics context");
    }
    cached = ContextLookupCacheEntry {
        .key = object_key,
        .value = &iterator->second,
    };
    return cached.value;
}

Result<const GraphicsContext*> GraphicsStore::context(u64 object_key) const {
    auto& cached = context_lookup_cache_[lookup_cache_index(object_key)];
    if (cached.key == object_key && cached.value != nullptr) return cached.value;
    auto iterator = contexts_.find(object_key);
    if (iterator == contexts_.end()) {
        return fail(ErrorCode::invalid_state,
                    "Java Graphics has no native graphics context");
    }
    cached = ContextLookupCacheEntry {
        .key = object_key,
        .value = const_cast<GraphicsContext*>(&iterator->second),
    };
    return cached.value;
}

Result<std::optional<DirtyImageUpdate>> GraphicsStore::consume_dirty_update(
    u64 image_object_key) {
    auto payload = image(image_object_key);
    if (!payload) return std::unexpected(payload.error());
    Image& source = **payload;
    if (!source.has_dirty_region()) {
        return std::optional<DirtyImageUpdate> {};
    }

    const ImageRegion region = source.dirty_region();
    if (region.width <= 0 || region.height <= 0 || region.x < 0 ||
        region.y < 0 ||
        static_cast<i64>(region.x) + region.width > source.width() ||
        static_cast<i64>(region.y) + region.height > source.height()) {
        return fail(ErrorCode::internal_error,
                    "image dirty region is outside its pixel storage");
    }
    auto count = validated_pixel_count(region.width, region.height);
    if (!count) return std::unexpected(count.error());

    DirtyImageUpdate update;
    update.image_width = source.width();
    update.image_height = source.height();
    update.region = region;
    update.pixels.resize(*count);
    for (i32 row = 0; row < region.height; ++row) {
        const usize source_offset =
            static_cast<usize>(region.y + row) *
                static_cast<usize>(source.width()) +
            static_cast<usize>(region.x);
        const usize destination_offset =
            static_cast<usize>(row) * static_cast<usize>(region.width);
        std::copy_n(source.pixels().begin() +
                        static_cast<std::ptrdiff_t>(source_offset),
                    static_cast<usize>(region.width),
                    update.pixels.begin() +
                        static_cast<std::ptrdiff_t>(destination_offset));
    }
    source.clear_dirty_region();
    return std::optional<DirtyImageUpdate>(std::move(update));
}

void GraphicsStore::erase_image(u64 object_key) noexcept {
    if (const auto found = images_.find(object_key); found != images_.end()) {
        const usize bytes = image_storage_bytes(found->second);
        image_storage_bytes_ = image_storage_bytes_ >= bytes
            ? image_storage_bytes_ - bytes
            : 0U;
        images_.erase(found);
    }
    invalidate_lookup_caches();
    for (auto iterator = contexts_.begin(); iterator != contexts_.end();) {
        if (iterator->second.target_key == object_key) {
            iterator = contexts_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void GraphicsStore::erase_context(u64 object_key) noexcept {
    contexts_.erase(object_key);
    invalidate_lookup_caches();
}

bool GraphicsStore::automatic_collection_due() const noexcept {
    return image_allocation_bytes_since_prune_ >= kGraphicsGcAllocationInterval;
}

usize GraphicsStore::estimated_bytes() const noexcept {
    usize total = saturated_add(sizeof(*this), image_storage_bytes_);
    total = saturated_add(total, contexts_.size() * sizeof(GraphicsContext));
    return total;
}

void GraphicsStore::prune(const std::function<bool(u64)>& is_live) {
    invalidate_lookup_caches();
    for (auto iterator = images_.begin(); iterator != images_.end();) {
        if (!is_live(iterator->first)) {
            const usize bytes = image_storage_bytes(iterator->second);
            image_storage_bytes_ = image_storage_bytes_ >= bytes
                ? image_storage_bytes_ - bytes
                : 0U;
            iterator = images_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = contexts_.begin(); iterator != contexts_.end();) {
        if (!is_live(iterator->first) ||
            images_.find(iterator->second.target_key) == images_.end()) {
            iterator = contexts_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    image_allocation_bytes_since_prune_ = 0U;
}

void GraphicsStore::clear() noexcept {
    invalidate_lookup_caches();
    contexts_.clear();
    images_.clear();
    image_storage_bytes_ = 0U;
    image_allocation_bytes_since_prune_ = 0U;
}

} // namespace phoneme::graphics
