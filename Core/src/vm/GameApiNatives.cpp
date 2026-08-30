#include "GameApiNatives.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "GraphicsNativeSupport.hpp"
#include "phoneme/graphics/Graphics.hpp"
#include "phoneme/graphics/Image.hpp"
#include "phoneme/vm/Machine.hpp"

namespace phoneme::vm {
namespace {

using namespace graphics_native;

constexpr const char* kLayerOwner = "javax/microedition/lcdui/game/Layer";
constexpr const char* kSpriteOwner = "javax/microedition/lcdui/game/Sprite";
constexpr const char* kTiledLayerOwner = "javax/microedition/lcdui/game/TiledLayer";
constexpr const char* kLayerManagerOwner = "javax/microedition/lcdui/game/LayerManager";

constexpr usize kLayerX = 0;
constexpr usize kLayerY = 1;
constexpr usize kLayerWidth = 2;
constexpr usize kLayerHeight = 3;
constexpr usize kLayerVisible = 4;

constexpr usize kSpriteImage = 5;
constexpr usize kSpriteFrameWidth = 6;
constexpr usize kSpriteFrameHeight = 7;
constexpr usize kSpriteRawFrameCount = 8;
constexpr usize kSpriteFrameSequence = 9;
constexpr usize kSpriteSequenceIndex = 10;
constexpr usize kSpriteTransform = 11;
constexpr usize kSpriteReferenceX = 12;
constexpr usize kSpriteReferenceY = 13;
constexpr usize kSpriteCollisionX = 14;
constexpr usize kSpriteCollisionY = 15;
constexpr usize kSpriteCollisionWidth = 16;
constexpr usize kSpriteCollisionHeight = 17;

constexpr usize kTiledColumns = 5;
constexpr usize kTiledRows = 6;
constexpr usize kTiledImage = 7;
constexpr usize kTiledTileWidth = 8;
constexpr usize kTiledTileHeight = 9;
constexpr usize kTiledStaticCount = 10;
constexpr usize kTiledCells = 11;
constexpr usize kTiledAnimated = 12;

constexpr usize kManagerLayers = 0;
constexpr usize kManagerCount = 1;
constexpr usize kManagerViewX = 2;
constexpr usize kManagerViewY = 3;
constexpr usize kManagerViewWidth = 4;
constexpr usize kManagerViewHeight = 5;

constexpr usize kImageWidth = 0;
constexpr usize kImageHeight = 1;
constexpr usize kImageMutable = 2;

struct Point64 final { i64 x {0}; i64 y {0}; };
struct WorldRect final { i64 left {0}; i64 top {0}; i64 right {0}; i64 bottom {0}; };

struct SpriteState final {
    ObjectRef object;
    ObjectRef image;
    i32 x {0};
    i32 y {0};
    bool visible {true};
    i32 frame_width {0};
    i32 frame_height {0};
    i32 raw_frame_count {0};
    ObjectRef frame_sequence;
    i32 sequence_index {0};
    graphics::Transform transform {graphics::Transform::none};
    i32 reference_x {0};
    i32 reference_y {0};
    i32 collision_x {0};
    i32 collision_y {0};
    i32 collision_width {0};
    i32 collision_height {0};
};

struct TiledState final {
    ObjectRef object;
    ObjectRef image;
    i32 x {0};
    i32 y {0};
    bool visible {true};
    i32 columns {0};
    i32 rows {0};
    i32 tile_width {0};
    i32 tile_height {0};
    i32 static_count {0};
    ObjectRef cells;
    ObjectRef animated;
};

void add(NativeMethodRegistry& registry,
         std::string owner,
         std::string name,
         std::string descriptor,
         NativeMethod method) {
    auto registered = registry.register_method(std::move(owner), std::move(name),
                                               std::move(descriptor), std::move(method));
    if (!registered) std::terminate();
}

[[nodiscard]] i32 wrapping_add(i32 left, i32 right) noexcept {
    return static_cast<i32>(static_cast<u32>(left) + static_cast<u32>(right));
}

[[nodiscard]] i32 wrapping_subtract(i32 left, i32 right) noexcept {
    return static_cast<i32>(static_cast<u32>(left) - static_cast<u32>(right));
}

[[nodiscard]] Result<i32> int_field(Machine& machine, ObjectRef object, usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

[[nodiscard]] Result<ObjectRef> ref_field(Machine& machine, ObjectRef object, usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Status set_int_field(Machine& machine, ObjectRef object,
                                   usize index, i32 value) {
    return machine.heap().set_field(object, index, Value::from_int(value));
}

[[nodiscard]] Status set_ref_field(Machine& machine, ObjectRef object,
                                   usize index, ObjectRef value) {
    return machine.heap().set_field(object, index, Value::from_reference(value));
}

[[nodiscard]] Status initialize_layer(Machine& machine, ObjectRef layer,
                                      i32 width, i32 height) {
    if (width <= 0 || height <= 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         "Layer dimensions must be positive");
    }
    const std::array<std::pair<usize, i32>, 5> fields {{
        {kLayerX, 0}, {kLayerY, 0}, {kLayerWidth, width},
        {kLayerHeight, height}, {kLayerVisible, 1},
    }};
    for (const auto& [index, value] : fields) {
        auto stored = set_int_field(machine, layer, index, value);
        if (!stored) return stored;
    }
    return {};
}

[[nodiscard]] Result<graphics::Image*> image_payload(Machine& machine, ObjectRef image) {
    auto payload = machine.graphics().image(image.bits);
    if (!payload) return graphics_error(payload.error());
    return *payload;
}

[[nodiscard]] Result<ObjectRef> clone_image_object(Machine& machine,
                                                   ObjectRef source_ref) {
    auto source = image_payload(machine, source_ref);
    if (!source) return std::unexpected(source.error());
    auto copy = graphics::Image::create_immutable(
        (*source)->width(), (*source)->height(), (*source)->pixels());
    if (!copy) return graphics_error(copy.error());
    auto object = machine.class_states().allocate_instance(
        machine.heap(), "javax/microedition/lcdui/Image");
    if (!object) return std::unexpected(object.error());
    const std::array<std::pair<usize, i32>, 3> fields {{
        {kImageWidth, copy->width()},
        {kImageHeight, copy->height()},
        {kImageMutable, 0},
    }};
    for (const auto& [index, value] : fields) {
        auto stored = set_int_field(machine, *object, index, value);
        if (!stored) return std::unexpected(stored.error());
    }
    auto attached = machine.graphics().attach_image(object->bits,
                                                    std::move(*copy));
    if (!attached) return graphics_error(attached.error());
    return *object;
}

[[nodiscard]] Result<graphics::GraphicsContext*> graphics_context(
    Machine& machine, ObjectRef graphics_object) {
    auto context = machine.graphics().context(graphics_object.bits);
    if (!context) return graphics_error(context.error());
    return *context;
}

[[nodiscard]] Result<graphics::Image*> graphics_target(
    Machine& machine, const graphics::GraphicsContext& context) {
    auto target = machine.graphics().image(context.target_key);
    if (!target) return graphics_error(target.error());
    return *target;
}

[[nodiscard]] Result<ObjectRef> make_int_array(Machine& machine,
                                               std::span<const i32> values) {
    auto array = machine.heap().allocate_array("[I", values.size(), Value::from_int(0));
    if (!array) return std::unexpected(array.error());
    for (usize index = 0; index < values.size(); ++index) {
        auto stored = machine.heap().set_element(*array, index,
                                                 Value::from_int(values[index]));
        if (!stored) return std::unexpected(stored.error());
    }
    return *array;
}

[[nodiscard]] Result<std::vector<i32>> read_int_array(
    Machine& machine, ObjectRef array, std::string_view operation) {
    if (array.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         std::string(operation) + " array is null");
    }
    auto class_name = machine.heap().class_name(array);
    auto length = machine.heap().array_length(array);
    if (!class_name || !length || *class_name != "[I") {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " expects int[]");
    }
    std::vector<i32> values;
    values.reserve(*length);
    for (usize index = 0; index < *length; ++index) {
        auto value = machine.heap().element(array, index);
        if (!value) return std::unexpected(value.error());
        auto integer = value->as_int();
        if (!integer) return std::unexpected(integer.error());
        values.push_back(*integer);
    }
    return values;
}

[[nodiscard]] Result<ObjectRef> make_layer_array(
    Machine& machine, std::span<const ObjectRef> values) {
    auto array = machine.heap().allocate_array(
        "[Ljavax/microedition/lcdui/game/Layer;", values.size(),
        Value::from_reference({}));
    if (!array) return std::unexpected(array.error());
    for (usize index = 0; index < values.size(); ++index) {
        auto stored = machine.heap().set_element(*array, index,
                                                 Value::from_reference(values[index]));
        if (!stored) return std::unexpected(stored.error());
    }
    return *array;
}

struct ManagerStorage final {
    ObjectRef array {};
    usize count {0U};
};

[[nodiscard]] Result<ManagerStorage> manager_storage(
    Machine& machine, ObjectRef manager) {
    constexpr std::array<usize, 2> fields {
        kManagerCount,
        kManagerLayers,
    };
    std::array<Value, 2> values {};
    auto snapshot = machine.heap().read_fields(manager, fields, values);
    if (!snapshot) return std::unexpected(snapshot.error());
    auto count = values[0].as_int();
    auto array = values[1].as_reference();
    if (!count) return std::unexpected(count.error());
    if (!array) return std::unexpected(array.error());
    if (*count < 0 || array->is_null()) {
        return fail(ErrorCode::invalid_state, "LayerManager storage is corrupt");
    }
    auto length = machine.heap().array_length(*array);
    if (!length || static_cast<usize>(*count) > *length) {
        return fail(ErrorCode::invalid_state, "LayerManager count is invalid");
    }
    return ManagerStorage {
        .array = *array,
        .count = static_cast<usize>(*count),
    };
}

[[nodiscard]] Result<std::vector<ObjectRef>> manager_layers(
    Machine& machine, ObjectRef manager) {
    auto storage = manager_storage(machine, manager);
    if (!storage) return std::unexpected(storage.error());
    std::vector<ObjectRef> values;
    values.reserve(storage->count);
    for (usize index = 0U; index < storage->count; ++index) {
        auto value = machine.heap().element(storage->array, index);
        if (!value) return std::unexpected(value.error());
        auto reference = value->as_reference();
        if (!reference) return std::unexpected(reference.error());
        values.push_back(*reference);
    }
    return values;
}

[[nodiscard]] Status store_manager_layers(Machine& machine, ObjectRef manager,
                                          std::span<const ObjectRef> layers) {
    auto array = make_layer_array(machine, layers);
    if (!array) return std::unexpected(array.error());
    auto stored = set_ref_field(machine, manager, kManagerLayers, *array);
    if (!stored) return stored;
    return set_int_field(machine, manager, kManagerCount,
                         static_cast<i32>(layers.size()));
}

[[nodiscard]] Point64 transform_point(i64 x, i64 y, i32 width, i32 height,
                                      graphics::Transform transform) noexcept {
    const i64 w = width;
    const i64 h = height;
    switch (transform) {
    case graphics::Transform::none: return {x, y};
    case graphics::Transform::mirror_rotate_180: return {x, h - 1 - y};
    case graphics::Transform::mirror: return {w - 1 - x, y};
    case graphics::Transform::rotate_180: return {w - 1 - x, h - 1 - y};
    case graphics::Transform::mirror_rotate_270: return {y, x};
    case graphics::Transform::rotate_90: return {h - 1 - y, x};
    case graphics::Transform::rotate_270: return {y, w - 1 - x};
    case graphics::Transform::mirror_rotate_90: return {h - 1 - y, w - 1 - x};
    }
    return {x, y};
}

[[nodiscard]] Point64 source_point(i64 x, i64 y, i32 width, i32 height,
                                   graphics::Transform transform) noexcept {
    const i64 w = width;
    const i64 h = height;
    switch (transform) {
    case graphics::Transform::none: return {x, y};
    case graphics::Transform::mirror_rotate_180: return {x, h - 1 - y};
    case graphics::Transform::mirror: return {w - 1 - x, y};
    case graphics::Transform::rotate_180: return {w - 1 - x, h - 1 - y};
    case graphics::Transform::mirror_rotate_270: return {y, x};
    case graphics::Transform::rotate_90: return {y, h - 1 - x};
    case graphics::Transform::rotate_270: return {w - 1 - y, x};
    case graphics::Transform::mirror_rotate_90: return {w - 1 - y, h - 1 - x};
    }
    return {x, y};
}

[[nodiscard]] WorldRect transformed_rect(i32 layer_x, i32 layer_y,
                                         i32 x, i32 y, i32 width, i32 height,
                                         i32 frame_width, i32 frame_height,
                                         graphics::Transform transform) noexcept {
    if (width <= 0 || height <= 0) return {};
    const i64 right = static_cast<i64>(x) + width - 1;
    const i64 bottom = static_cast<i64>(y) + height - 1;
    const std::array<Point64, 4> points {{
        transform_point(x, y, frame_width, frame_height, transform),
        transform_point(right, y, frame_width, frame_height, transform),
        transform_point(x, bottom, frame_width, frame_height, transform),
        transform_point(right, bottom, frame_width, frame_height, transform),
    }};
    i64 min_x = points[0].x;
    i64 max_x = points[0].x;
    i64 min_y = points[0].y;
    i64 max_y = points[0].y;
    for (const Point64 point : points) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }
    return {static_cast<i64>(layer_x) + min_x,
            static_cast<i64>(layer_y) + min_y,
            static_cast<i64>(layer_x) + max_x + 1,
            static_cast<i64>(layer_y) + max_y + 1};
}

[[nodiscard]] WorldRect intersect_rect(WorldRect left, WorldRect right) noexcept {
    WorldRect result {std::max(left.left, right.left), std::max(left.top, right.top),
                      std::min(left.right, right.right),
                      std::min(left.bottom, right.bottom)};
    if (result.right <= result.left || result.bottom <= result.top) return {};
    return result;
}

[[nodiscard]] bool nonempty(WorldRect rectangle) noexcept {
    return rectangle.right > rectangle.left && rectangle.bottom > rectangle.top;
}

[[nodiscard]] Result<SpriteState> sprite_state(Machine& machine,
                                               ObjectRef sprite) {
    SpriteState state;
    state.object = sprite;
    constexpr std::array<usize, 16> fields {
        kSpriteImage,
        kLayerX,
        kLayerY,
        kLayerVisible,
        kSpriteFrameWidth,
        kSpriteFrameHeight,
        kSpriteRawFrameCount,
        kSpriteFrameSequence,
        kSpriteSequenceIndex,
        kSpriteTransform,
        kSpriteReferenceX,
        kSpriteReferenceY,
        kSpriteCollisionX,
        kSpriteCollisionY,
        kSpriteCollisionWidth,
        kSpriteCollisionHeight,
    };
    std::array<Value, 16> values {};
    auto snapshot = machine.heap().read_fields(sprite, fields, values);
    if (!snapshot) return std::unexpected(snapshot.error());

    auto image = values[0].as_reference();
    auto x = values[1].as_int();
    auto y = values[2].as_int();
    auto visible = values[3].as_int();
    auto frame_width = values[4].as_int();
    auto frame_height = values[5].as_int();
    auto raw_count = values[6].as_int();
    auto sequence = values[7].as_reference();
    auto sequence_index = values[8].as_int();
    auto transform_value = values[9].as_int();
    auto reference_x = values[10].as_int();
    auto reference_y = values[11].as_int();
    auto collision_x = values[12].as_int();
    auto collision_y = values[13].as_int();
    auto collision_width = values[14].as_int();
    auto collision_height = values[15].as_int();
    if (!image || !x || !y || !visible || !frame_width || !frame_height || !raw_count ||
        !sequence || !sequence_index || !transform_value || !reference_x ||
        !reference_y || !collision_x || !collision_y || !collision_width ||
        !collision_height) {
        return fail(ErrorCode::invalid_state, "Sprite fields are unavailable");
    }
    auto transform = graphics::transform_from_int(*transform_value);
    if (!transform) return graphics_error(transform.error());
    state.image = *image;
    state.x = *x;
    state.y = *y;
    state.visible = *visible != 0;
    state.frame_width = *frame_width;
    state.frame_height = *frame_height;
    state.raw_frame_count = *raw_count;
    state.frame_sequence = *sequence;
    state.sequence_index = *sequence_index;
    state.transform = *transform;
    state.reference_x = *reference_x;
    state.reference_y = *reference_y;
    state.collision_x = *collision_x;
    state.collision_y = *collision_y;
    state.collision_width = *collision_width;
    state.collision_height = *collision_height;
    return state;
}

[[nodiscard]] Result<i32> sequence_length(Machine& machine,
                                          const SpriteState& state) {
    if (state.frame_sequence.is_null()) return state.raw_frame_count;
    auto length = machine.heap().array_length(state.frame_sequence);
    if (!length || *length > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return fail(ErrorCode::invalid_state, "Sprite frame sequence is invalid");
    }
    return static_cast<i32>(*length);
}

[[nodiscard]] Result<i32> raw_frame(Machine& machine,
                                    const SpriteState& state) {
    auto length = sequence_length(machine, state);
    if (!length) return std::unexpected(length.error());
    if (*length <= 0 || state.sequence_index < 0 || state.sequence_index >= *length) {
        return fail(ErrorCode::invalid_state, "Sprite frame index is invalid");
    }
    if (state.frame_sequence.is_null()) return state.sequence_index;
    auto value = machine.heap().element(state.frame_sequence,
                                        static_cast<usize>(state.sequence_index));
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

[[nodiscard]] Result<TiledState> tiled_state(Machine& machine,
                                             ObjectRef tiled) {
    TiledState state;
    state.object = tiled;
    constexpr std::array<usize, 11> fields {
        kTiledImage,
        kLayerX,
        kLayerY,
        kLayerVisible,
        kTiledColumns,
        kTiledRows,
        kTiledTileWidth,
        kTiledTileHeight,
        kTiledStaticCount,
        kTiledCells,
        kTiledAnimated,
    };
    std::array<Value, 11> values {};
    auto snapshot = machine.heap().read_fields(tiled, fields, values);
    if (!snapshot) return std::unexpected(snapshot.error());

    auto image = values[0].as_reference();
    auto x = values[1].as_int();
    auto y = values[2].as_int();
    auto visible = values[3].as_int();
    auto columns = values[4].as_int();
    auto rows = values[5].as_int();
    auto tile_width = values[6].as_int();
    auto tile_height = values[7].as_int();
    auto static_count = values[8].as_int();
    auto cells = values[9].as_reference();
    auto animated = values[10].as_reference();
    if (!image || !x || !y || !visible || !columns || !rows || !tile_width ||
        !tile_height || !static_count || !cells || !animated) {
        return fail(ErrorCode::invalid_state, "TiledLayer fields are unavailable");
    }
    state.image = *image;
    state.x = *x;
    state.y = *y;
    state.visible = *visible != 0;
    state.columns = *columns;
    state.rows = *rows;
    state.tile_width = *tile_width;
    state.tile_height = *tile_height;
    state.static_count = *static_count;
    state.cells = *cells;
    state.animated = *animated;
    return state;
}

[[nodiscard]] Result<i32> resolve_tile(Machine& machine,
                                       const TiledState& state,
                                       i32 tile_index) {
    if (tile_index >= 0) return tile_index;
    const i64 slot = -static_cast<i64>(tile_index) - 1;
    auto length = machine.heap().array_length(state.animated);
    if (!length || slot < 0 || static_cast<u64>(slot) >= *length) {
        return fail_java("java/lang/IndexOutOfBoundsException",
                         "animated tile index does not exist");
    }
    auto value = machine.heap().element(state.animated, static_cast<usize>(slot));
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

[[nodiscard]] Result<i32> cell_value(Machine& machine,
                                     const TiledState& state,
                                     i32 column, i32 row) {
    if (column < 0 || row < 0 || column >= state.columns || row >= state.rows) {
        return fail_java("java/lang/IndexOutOfBoundsException",
                         "TiledLayer cell is outside the layer");
    }
    const usize index = static_cast<usize>(row) * static_cast<usize>(state.columns) +
                        static_cast<usize>(column);
    auto value = machine.heap().element(state.cells, index);
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

[[nodiscard]] Status validate_tile_index(Machine& machine,
                                         const TiledState& state,
                                         i32 tile_index) {
    if (tile_index >= 0) {
        if (tile_index > state.static_count) {
            return fail_java("java/lang/IndexOutOfBoundsException",
                             "static tile index does not exist");
        }
        return {};
    }
    auto resolved = resolve_tile(machine, state, tile_index);
    if (!resolved) return std::unexpected(resolved.error());
    return {};
}

[[nodiscard]] Result<bool> opaque_sprite_pixel(Machine& machine,
                                               const SpriteState& state,
                                               i64 world_x, i64 world_y) {
    const Point64 source = source_point(world_x - state.x, world_y - state.y,
                                        state.frame_width, state.frame_height,
                                        state.transform);
    if (source.x < 0 || source.y < 0 || source.x >= state.frame_width ||
        source.y >= state.frame_height) return false;
    auto frame = raw_frame(machine, state);
    auto image = image_payload(machine, state.image);
    if (!frame) return std::unexpected(frame.error());
    if (!image) return std::unexpected(image.error());
    const i32 columns = (*image)->width() / state.frame_width;
    const i32 frame_x = (*frame % columns) * state.frame_width;
    const i32 frame_y = (*frame / columns) * state.frame_height;
    auto pixel = (*image)->pixel(frame_x + static_cast<i32>(source.x),
                                 frame_y + static_cast<i32>(source.y));
    if (!pixel) return graphics_error(pixel.error());
    return ((*pixel >> 24U) & 0xFFU) != 0U;
}

[[nodiscard]] Result<bool> opaque_tile_pixel(Machine& machine,
                                             const TiledState& state,
                                             i64 world_x, i64 world_y) {
    const i64 local_x = world_x - state.x;
    const i64 local_y = world_y - state.y;
    if (local_x < 0 || local_y < 0 ||
        local_x >= static_cast<i64>(state.columns) * state.tile_width ||
        local_y >= static_cast<i64>(state.rows) * state.tile_height) return false;
    const i32 column = static_cast<i32>(local_x / state.tile_width);
    const i32 row = static_cast<i32>(local_y / state.tile_height);
    auto cell = cell_value(machine, state, column, row);
    if (!cell) return std::unexpected(cell.error());
    auto resolved = resolve_tile(machine, state, *cell);
    if (!resolved) return std::unexpected(resolved.error());
    if (*resolved == 0) return false;
    auto image = image_payload(machine, state.image);
    if (!image) return std::unexpected(image.error());
    const i32 columns = (*image)->width() / state.tile_width;
    const i32 tile_index = *resolved - 1;
    const i32 source_x = (tile_index % columns) * state.tile_width +
                         static_cast<i32>(local_x % state.tile_width);
    const i32 source_y = (tile_index / columns) * state.tile_height +
                         static_cast<i32>(local_y % state.tile_height);
    auto pixel = (*image)->pixel(source_x, source_y);
    if (!pixel) return graphics_error(pixel.error());
    return ((*pixel >> 24U) & 0xFFU) != 0U;
}

[[nodiscard]] Result<bool> collide_sprite_sprite(Machine& machine,
                                                 const SpriteState& left,
                                                 const SpriteState& right,
                                                 bool pixel_level) {
    if (!left.visible || !right.visible) return false;
    const auto left_rect = transformed_rect(left.x, left.y, left.collision_x,
        left.collision_y, left.collision_width, left.collision_height,
        left.frame_width, left.frame_height, left.transform);
    const auto right_rect = transformed_rect(right.x, right.y, right.collision_x,
        right.collision_y, right.collision_width, right.collision_height,
        right.frame_width, right.frame_height, right.transform);
    auto overlap = intersect_rect(left_rect, right_rect);
    if (!nonempty(overlap)) return false;
    if (!pixel_level) return true;
    const auto left_frame = transformed_rect(
        left.x, left.y, 0, 0, left.frame_width, left.frame_height,
        left.frame_width, left.frame_height, left.transform);
    const auto right_frame = transformed_rect(
        right.x, right.y, 0, 0, right.frame_width, right.frame_height,
        right.frame_width, right.frame_height, right.transform);
    overlap = intersect_rect(intersect_rect(overlap, left_frame), right_frame);
    if (!nonempty(overlap)) return false;
    for (i64 y = overlap.top; y < overlap.bottom; ++y) {
        for (i64 x = overlap.left; x < overlap.right; ++x) {
            auto a = opaque_sprite_pixel(machine, left, x, y);
            if (!a) return std::unexpected(a.error());
            if (!*a) continue;
            auto b = opaque_sprite_pixel(machine, right, x, y);
            if (!b) return std::unexpected(b.error());
            if (*b) return true;
        }
    }
    return false;
}

[[nodiscard]] Result<bool> collide_sprite_image(Machine& machine,
                                                const SpriteState& sprite,
                                                ObjectRef image_ref,
                                                i32 image_x, i32 image_y,
                                                bool pixel_level) {
    if (!sprite.visible) return false;
    auto image = image_payload(machine, image_ref);
    if (!image) return std::unexpected(image.error());
    const auto sprite_rect = transformed_rect(sprite.x, sprite.y,
        sprite.collision_x, sprite.collision_y, sprite.collision_width,
        sprite.collision_height, sprite.frame_width, sprite.frame_height,
        sprite.transform);
    const WorldRect image_rect {image_x, image_y,
        static_cast<i64>(image_x) + (*image)->width(),
        static_cast<i64>(image_y) + (*image)->height()};
    auto overlap = intersect_rect(sprite_rect, image_rect);
    if (!nonempty(overlap)) return false;
    if (!pixel_level) return true;
    const auto sprite_frame = transformed_rect(
        sprite.x, sprite.y, 0, 0, sprite.frame_width, sprite.frame_height,
        sprite.frame_width, sprite.frame_height, sprite.transform);
    overlap = intersect_rect(overlap, sprite_frame);
    if (!nonempty(overlap)) return false;
    for (i64 y = overlap.top; y < overlap.bottom; ++y) {
        for (i64 x = overlap.left; x < overlap.right; ++x) {
            auto a = opaque_sprite_pixel(machine, sprite, x, y);
            if (!a) return std::unexpected(a.error());
            if (!*a) continue;
            auto pixel = (*image)->pixel(static_cast<i32>(x - image_x),
                                         static_cast<i32>(y - image_y));
            if (!pixel) return graphics_error(pixel.error());
            if (((*pixel >> 24U) & 0xFFU) != 0U) return true;
        }
    }
    return false;
}

[[nodiscard]] Result<bool> collide_sprite_tiled(Machine& machine,
                                                const SpriteState& sprite,
                                                const TiledState& tiled,
                                                bool pixel_level) {
    if (!sprite.visible || !tiled.visible) return false;
    const auto sprite_rect = transformed_rect(sprite.x, sprite.y,
        sprite.collision_x, sprite.collision_y, sprite.collision_width,
        sprite.collision_height, sprite.frame_width, sprite.frame_height,
        sprite.transform);
    const WorldRect tiled_rect {tiled.x, tiled.y,
        static_cast<i64>(tiled.x) + static_cast<i64>(tiled.columns) * tiled.tile_width,
        static_cast<i64>(tiled.y) + static_cast<i64>(tiled.rows) * tiled.tile_height};
    auto overlap = intersect_rect(sprite_rect, tiled_rect);
    if (!nonempty(overlap)) return false;
    if (pixel_level) {
        const auto sprite_frame = transformed_rect(
            sprite.x, sprite.y, 0, 0, sprite.frame_width,
            sprite.frame_height, sprite.frame_width,
            sprite.frame_height, sprite.transform);
        overlap = intersect_rect(overlap, sprite_frame);
        if (!nonempty(overlap)) return false;
        for (i64 y = overlap.top; y < overlap.bottom; ++y) {
            for (i64 x = overlap.left; x < overlap.right; ++x) {
                auto a = opaque_sprite_pixel(machine, sprite, x, y);
                if (!a) return std::unexpected(a.error());
                if (!*a) continue;
                auto b = opaque_tile_pixel(machine, tiled, x, y);
                if (!b) return std::unexpected(b.error());
                if (*b) return true;
            }
        }
        return false;
    }
    const i64 first_column = std::max<i64>(0, (overlap.left - tiled.x) /
                                               tiled.tile_width);
    const i64 last_column = std::min<i64>(tiled.columns - 1,
        (overlap.right - 1 - tiled.x) / tiled.tile_width);
    const i64 first_row = std::max<i64>(0, (overlap.top - tiled.y) /
                                            tiled.tile_height);
    const i64 last_row = std::min<i64>(tiled.rows - 1,
        (overlap.bottom - 1 - tiled.y) / tiled.tile_height);
    for (i64 row = first_row; row <= last_row; ++row) {
        for (i64 column = first_column; column <= last_column; ++column) {
            auto value = cell_value(machine, tiled, static_cast<i32>(column),
                                    static_cast<i32>(row));
            if (!value) return std::unexpected(value.error());
            auto resolved = resolve_tile(machine, tiled, *value);
            if (!resolved) return std::unexpected(resolved.error());
            if (*resolved != 0) return true;
        }
    }
    return false;
}

[[nodiscard]] Result<std::optional<Value>> no_value() {
    return std::optional<Value> {};
}

} // namespace

void register_game_api_natives(NativeMethodRegistry& registry) {
    add(registry, kLayerOwner, "<init>", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto layer = receiver(arguments, "Layer.<init>");
            auto width = int_argument(arguments, 1U, "Layer.<init>");
            auto height = int_argument(arguments, 2U, "Layer.<init>");
            if (!layer) return std::unexpected(layer.error());
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            auto status = initialize_layer(machine, *layer, *width, *height);
            if (!status) return std::unexpected(status.error());
            return no_value();
        });

    const auto layer_getter = [&registry](const char* name, usize field) {
        add(registry, kLayerOwner, name, "()I",
            [field, name](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto layer = receiver(arguments, name);
                if (!layer) return std::unexpected(layer.error());
                auto value = int_field(machine, *layer, field);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_int(*value));
            });
    };
    layer_getter("getX", kLayerX);
    layer_getter("getY", kLayerY);
    layer_getter("getWidth", kLayerWidth);
    layer_getter("getHeight", kLayerHeight);

    add(registry, kLayerOwner, "isVisible", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto layer = receiver(arguments, "Layer.isVisible");
            if (!layer) return std::unexpected(layer.error());
            auto value = int_field(machine, *layer, kLayerVisible);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value != 0 ? 1 : 0));
        });
    add(registry, kLayerOwner, "setPosition", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto layer = receiver(arguments, "Layer.setPosition");
            auto x = int_argument(arguments, 1U, "Layer.setPosition");
            auto y = int_argument(arguments, 2U, "Layer.setPosition");
            if (!layer) return std::unexpected(layer.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            auto first = set_int_field(machine, *layer, kLayerX, *x);
            auto second = set_int_field(machine, *layer, kLayerY, *y);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            return no_value();
        });
    add(registry, kLayerOwner, "move", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto layer = receiver(arguments, "Layer.move");
            auto dx = int_argument(arguments, 1U, "Layer.move");
            auto dy = int_argument(arguments, 2U, "Layer.move");
            if (!layer) return std::unexpected(layer.error());
            if (!dx) return std::unexpected(dx.error());
            if (!dy) return std::unexpected(dy.error());
            auto x = int_field(machine, *layer, kLayerX);
            auto y = int_field(machine, *layer, kLayerY);
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            auto first = set_int_field(machine, *layer, kLayerX, wrapping_add(*x, *dx));
            auto second = set_int_field(machine, *layer, kLayerY, wrapping_add(*y, *dy));
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            return no_value();
        });
    add(registry, kLayerOwner, "setVisible", "(Z)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto layer = receiver(arguments, "Layer.setVisible");
            auto visible = int_argument(arguments, 1U, "Layer.setVisible");
            if (!layer) return std::unexpected(layer.error());
            if (!visible) return std::unexpected(visible.error());
            auto stored = set_int_field(machine, *layer, kLayerVisible,
                                        *visible != 0 ? 1 : 0);
            if (!stored) return std::unexpected(stored.error());
            return no_value();
        });

    const auto initialize_sprite = [](Machine& machine, ObjectRef sprite,
                                      ObjectRef image_ref, i32 frame_width,
                                      i32 frame_height) -> Status {
        auto image = image_payload(machine, image_ref);
        if (!image) return std::unexpected(image.error());
        if (frame_width <= 0 || frame_height <= 0 ||
            (*image)->width() % frame_width != 0 ||
            (*image)->height() % frame_height != 0) {
            return fail_java("java/lang/IllegalArgumentException",
                             "Sprite frame size must divide the image");
        }
        auto base = initialize_layer(machine, sprite, frame_width, frame_height);
        if (!base) return base;
        const i32 raw_count = ((*image)->width() / frame_width) *
                              ((*image)->height() / frame_height);
        auto image_stored = set_ref_field(machine, sprite, kSpriteImage, image_ref);
        auto sequence_stored = set_ref_field(machine, sprite, kSpriteFrameSequence, {});
        if (!image_stored) return image_stored;
        if (!sequence_stored) return sequence_stored;
        const std::array<std::pair<usize, i32>, 11> fields {{
            {kSpriteFrameWidth, frame_width}, {kSpriteFrameHeight, frame_height},
            {kSpriteRawFrameCount, raw_count}, {kSpriteSequenceIndex, 0},
            {kSpriteTransform, 0}, {kSpriteReferenceX, 0}, {kSpriteReferenceY, 0},
            {kSpriteCollisionX, 0}, {kSpriteCollisionY, 0},
            {kSpriteCollisionWidth, frame_width},
            {kSpriteCollisionHeight, frame_height},
        }};
        for (const auto& [index, value] : fields) {
            auto stored = set_int_field(machine, sprite, index, value);
            if (!stored) return stored;
        }
        return {};
    };

    add(registry, kSpriteOwner, "<init>",
        "(Ljavax/microedition/lcdui/Image;)V",
        [initialize_sprite](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.<init>");
            auto image_ref = reference_argument(arguments, 1U, "Sprite.<init>");
            if (!sprite) return std::unexpected(sprite.error());
            if (!image_ref) return std::unexpected(image_ref.error());
            auto image = image_payload(machine, *image_ref);
            if (!image) return std::unexpected(image.error());
            auto status = initialize_sprite(machine, *sprite, *image_ref,
                                            (*image)->width(), (*image)->height());
            if (!status) return std::unexpected(status.error());
            return no_value();
        });
    add(registry, kSpriteOwner, "<init>",
        "(Ljavax/microedition/lcdui/Image;II)V",
        [initialize_sprite](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.<init>");
            auto image_ref = reference_argument(arguments, 1U, "Sprite.<init>");
            auto width = int_argument(arguments, 2U, "Sprite.<init>");
            auto height = int_argument(arguments, 3U, "Sprite.<init>");
            if (!sprite) return std::unexpected(sprite.error());
            if (!image_ref) return std::unexpected(image_ref.error());
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            auto status = initialize_sprite(machine, *sprite, *image_ref, *width, *height);
            if (!status) return std::unexpected(status.error());
            return no_value();
        });
    add(registry, kSpriteOwner, "<init>",
        "(Ljavax/microedition/lcdui/game/Sprite;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.<init>");
            auto source = reference_argument(arguments, 1U, "Sprite.<init>");
            if (!sprite) return std::unexpected(sprite.error());
            if (!source) return std::unexpected(source.error());
            for (usize index = kLayerX; index <= kSpriteCollisionHeight; ++index) {
                if (index == kSpriteImage || index == kSpriteFrameSequence) continue;
                auto value = machine.heap().field(*source, index);
                if (!value) return std::unexpected(value.error());
                auto stored = machine.heap().set_field(*sprite, index, *value);
                if (!stored) return std::unexpected(stored.error());
            }
            auto source_image = ref_field(machine, *source, kSpriteImage);
            if (!source_image) return std::unexpected(source_image.error());
            auto copied_image = clone_image_object(machine, *source_image);
            if (!copied_image) return std::unexpected(copied_image.error());
            auto image_stored = set_ref_field(machine, *sprite,
                                              kSpriteImage, *copied_image);
            if (!image_stored) return std::unexpected(image_stored.error());
            auto sequence = ref_field(machine, *source, kSpriteFrameSequence);
            if (!sequence) return std::unexpected(sequence.error());
            ObjectRef copied {};
            if (!sequence->is_null()) {
                auto values = read_int_array(machine, *sequence, "Sprite.<init>");
                if (!values) return std::unexpected(values.error());
                auto array = make_int_array(machine, *values);
                if (!array) return std::unexpected(array.error());
                copied = *array;
            }
            auto stored = set_ref_field(machine, *sprite, kSpriteFrameSequence, copied);
            if (!stored) return std::unexpected(stored.error());
            return no_value();
        });

    add(registry, kSpriteOwner, "getFrame", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.getFrame");
            if (!sprite) return std::unexpected(sprite.error());
            auto value = int_field(machine, *sprite, kSpriteSequenceIndex);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    add(registry, kSpriteOwner, "getRawFrameCount", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.getRawFrameCount");
            if (!sprite) return std::unexpected(sprite.error());
            auto value = int_field(machine, *sprite, kSpriteRawFrameCount);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    add(registry, kSpriteOwner, "getFrameSequenceLength", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.getFrameSequenceLength");
            if (!sprite) return std::unexpected(sprite.error());
            auto state = sprite_state(machine, *sprite);
            if (!state) return std::unexpected(state.error());
            auto length = sequence_length(machine, *state);
            if (!length) return std::unexpected(length.error());
            return std::optional<Value>(Value::from_int(*length));
        });
    add(registry, kSpriteOwner, "setFrame", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.setFrame");
            auto frame = int_argument(arguments, 1U, "Sprite.setFrame");
            if (!sprite) return std::unexpected(sprite.error());
            if (!frame) return std::unexpected(frame.error());
            auto state = sprite_state(machine, *sprite);
            if (!state) return std::unexpected(state.error());
            auto length = sequence_length(machine, *state);
            if (!length) return std::unexpected(length.error());
            if (*frame < 0 || *frame >= *length) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "Sprite frame is outside the sequence");
            }
            auto stored = set_int_field(machine, *sprite, kSpriteSequenceIndex, *frame);
            if (!stored) return std::unexpected(stored.error());
            return no_value();
        });
    add(registry, kSpriteOwner, "nextFrame", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.nextFrame");
            if (!sprite) return std::unexpected(sprite.error());
            auto state = sprite_state(machine, *sprite);
            if (!state) return std::unexpected(state.error());
            auto length = sequence_length(machine, *state);
            if (!length) return std::unexpected(length.error());
            const i32 next = state->sequence_index + 1 == *length
                ? 0 : state->sequence_index + 1;
            auto stored = set_int_field(machine, *sprite, kSpriteSequenceIndex, next);
            if (!stored) return std::unexpected(stored.error());
            return no_value();
        });
    add(registry, kSpriteOwner, "prevFrame", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.prevFrame");
            if (!sprite) return std::unexpected(sprite.error());
            auto state = sprite_state(machine, *sprite);
            if (!state) return std::unexpected(state.error());
            auto length = sequence_length(machine, *state);
            if (!length) return std::unexpected(length.error());
            const i32 previous = state->sequence_index == 0
                ? *length - 1 : state->sequence_index - 1;
            auto stored = set_int_field(machine, *sprite, kSpriteSequenceIndex, previous);
            if (!stored) return std::unexpected(stored.error());
            return no_value();
        });
    add(registry, kSpriteOwner, "setFrameSequence", "([I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.setFrameSequence");
            auto sequence = reference_argument(arguments, 1U,
                                               "Sprite.setFrameSequence", true);
            if (!sprite) return std::unexpected(sprite.error());
            if (!sequence) return std::unexpected(sequence.error());
            ObjectRef copied {};
            if (!sequence->is_null()) {
                auto values = read_int_array(machine, *sequence,
                                             "Sprite.setFrameSequence");
                if (!values) return std::unexpected(values.error());
                if (values->empty()) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Sprite frame sequence cannot be empty");
                }
                auto count = int_field(machine, *sprite, kSpriteRawFrameCount);
                if (!count) return std::unexpected(count.error());
                for (const i32 value : *values) {
                    if (value < 0 || value >= *count) {
                        return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                         "Sprite frame sequence contains an invalid frame");
                    }
                }
                auto array = make_int_array(machine, *values);
                if (!array) return std::unexpected(array.error());
                copied = *array;
            }
            auto first = set_ref_field(machine, *sprite, kSpriteFrameSequence, copied);
            auto second = set_int_field(machine, *sprite, kSpriteSequenceIndex, 0);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            return no_value();
        });

    add(registry, kSpriteOwner, "defineReferencePixel", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.defineReferencePixel");
            auto x = int_argument(arguments, 1U, "Sprite.defineReferencePixel");
            auto y = int_argument(arguments, 2U, "Sprite.defineReferencePixel");
            if (!sprite) return std::unexpected(sprite.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            auto first = set_int_field(machine, *sprite, kSpriteReferenceX, *x);
            auto second = set_int_field(machine, *sprite, kSpriteReferenceY, *y);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            return no_value();
        });
    add(registry, kSpriteOwner, "setRefPixelPosition", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.setRefPixelPosition");
            auto x = int_argument(arguments, 1U, "Sprite.setRefPixelPosition");
            auto y = int_argument(arguments, 2U, "Sprite.setRefPixelPosition");
            if (!sprite) return std::unexpected(sprite.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            auto state = sprite_state(machine, *sprite);
            if (!state) return std::unexpected(state.error());
            const auto point = transform_point(state->reference_x, state->reference_y,
                                               state->frame_width, state->frame_height,
                                               state->transform);
            const i32 layer_x = static_cast<i32>(static_cast<u32>(*x) -
                                                  static_cast<u32>(point.x));
            const i32 layer_y = static_cast<i32>(static_cast<u32>(*y) -
                                                  static_cast<u32>(point.y));
            auto first = set_int_field(machine, *sprite, kLayerX, layer_x);
            auto second = set_int_field(machine, *sprite, kLayerY, layer_y);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            return no_value();
        });
    add(registry, kSpriteOwner, "getRefPixelX", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.getRefPixelX");
            if (!sprite) return std::unexpected(sprite.error());
            auto state = sprite_state(machine, *sprite);
            if (!state) return std::unexpected(state.error());
            const auto point = transform_point(state->reference_x, state->reference_y,
                                               state->frame_width, state->frame_height,
                                               state->transform);
            return std::optional<Value>(Value::from_int(
                wrapping_add(state->x, static_cast<i32>(point.x))));
        });
    add(registry, kSpriteOwner, "getRefPixelY", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.getRefPixelY");
            if (!sprite) return std::unexpected(sprite.error());
            auto state = sprite_state(machine, *sprite);
            if (!state) return std::unexpected(state.error());
            const auto point = transform_point(state->reference_x, state->reference_y,
                                               state->frame_width, state->frame_height,
                                               state->transform);
            return std::optional<Value>(Value::from_int(
                wrapping_add(state->y, static_cast<i32>(point.y))));
        });
    add(registry, kSpriteOwner, "defineCollisionRectangle", "(IIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.defineCollisionRectangle");
            if (!sprite) return std::unexpected(sprite.error());
            std::array<i32, 4> values {};
            for (usize index = 0; index < values.size(); ++index) {
                auto value = int_argument(arguments, index + 1U,
                                          "Sprite.defineCollisionRectangle");
                if (!value) return std::unexpected(value.error());
                values[index] = *value;
            }
            if (values[2] < 0 || values[3] < 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Sprite collision dimensions cannot be negative");
            }
            const std::array<usize, 4> fields {{kSpriteCollisionX,
                kSpriteCollisionY, kSpriteCollisionWidth, kSpriteCollisionHeight}};
            for (usize index = 0; index < fields.size(); ++index) {
                auto stored = set_int_field(machine, *sprite, fields[index], values[index]);
                if (!stored) return std::unexpected(stored.error());
            }
            return no_value();
        });

    add(registry, kSpriteOwner, "setTransform", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.setTransform");
            auto value = int_argument(arguments, 1U, "Sprite.setTransform");
            if (!sprite) return std::unexpected(sprite.error());
            if (!value) return std::unexpected(value.error());
            auto transform = graphics::transform_from_int(*value);
            if (!transform) return graphics_error(transform.error());
            auto state = sprite_state(machine, *sprite);
            if (!state) return std::unexpected(state.error());
            const auto old_point = transform_point(state->reference_x, state->reference_y,
                                                   state->frame_width,
                                                   state->frame_height,
                                                   state->transform);
            const i32 world_x = wrapping_add(state->x, static_cast<i32>(old_point.x));
            const i32 world_y = wrapping_add(state->y, static_cast<i32>(old_point.y));
            const auto new_point = transform_point(state->reference_x, state->reference_y,
                                                   state->frame_width,
                                                   state->frame_height, *transform);
            const auto size = graphics::transformed_size(state->frame_width,
                                                         state->frame_height,
                                                         *transform);
            const std::array<std::pair<usize, i32>, 5> fields {{
                {kSpriteTransform, *value}, {kLayerWidth, size.width},
                {kLayerHeight, size.height},
                {kLayerX, static_cast<i32>(static_cast<u32>(world_x) -
                                           static_cast<u32>(new_point.x))},
                {kLayerY, static_cast<i32>(static_cast<u32>(world_y) -
                                           static_cast<u32>(new_point.y))},
            }};
            for (const auto& [index, field_value] : fields) {
                auto stored = set_int_field(machine, *sprite, index, field_value);
                if (!stored) return std::unexpected(stored.error());
            }
            return no_value();
        });

    add(registry, kSpriteOwner, "setImage",
        "(Ljavax/microedition/lcdui/Image;II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.setImage");
            auto image_ref = reference_argument(arguments, 1U, "Sprite.setImage");
            auto width = int_argument(arguments, 2U, "Sprite.setImage");
            auto height = int_argument(arguments, 3U, "Sprite.setImage");
            if (!sprite) return std::unexpected(sprite.error());
            if (!image_ref) return std::unexpected(image_ref.error());
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            auto image = image_payload(machine, *image_ref);
            if (!image) return std::unexpected(image.error());
            if (*width <= 0 || *height <= 0 ||
                (*image)->width() % *width != 0 ||
                (*image)->height() % *height != 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Sprite frame size must divide the image");
            }
            auto state = sprite_state(machine, *sprite);
            if (!state) return std::unexpected(state.error());
            const i32 count = ((*image)->width() / *width) *
                              ((*image)->height() / *height);
            const bool fewer_frames = count < state->raw_frame_count;
            const ObjectRef sequence = fewer_frames
                ? ObjectRef {}
                : state->frame_sequence;
            const i32 next_index = fewer_frames ? 0 : state->sequence_index;
            const bool dimensions_changed = state->frame_width != *width ||
                                            state->frame_height != *height;
            i32 next_x = state->x;
            i32 next_y = state->y;
            if (dimensions_changed) {
                const auto old_point = transform_point(
                    state->reference_x, state->reference_y,
                    state->frame_width, state->frame_height,
                    state->transform);
                const i32 world_x = wrapping_add(
                    state->x, static_cast<i32>(old_point.x));
                const i32 world_y = wrapping_add(
                    state->y, static_cast<i32>(old_point.y));
                const auto new_point = transform_point(
                    state->reference_x, state->reference_y,
                    *width, *height, state->transform);
                next_x = wrapping_subtract(
                    world_x, static_cast<i32>(new_point.x));
                next_y = wrapping_subtract(
                    world_y, static_cast<i32>(new_point.y));
            }
            const auto size = graphics::transformed_size(
                *width, *height, state->transform);
            auto image_stored = set_ref_field(machine, *sprite,
                                              kSpriteImage, *image_ref);
            auto sequence_stored = set_ref_field(machine, *sprite,
                                                 kSpriteFrameSequence, sequence);
            if (!image_stored) return std::unexpected(image_stored.error());
            if (!sequence_stored) return std::unexpected(sequence_stored.error());
            const std::array<std::pair<usize, i32>, 8> fields {{
                {kSpriteFrameWidth, *width},
                {kSpriteFrameHeight, *height},
                {kSpriteRawFrameCount, count},
                {kSpriteSequenceIndex, next_index},
                {kLayerWidth, size.width},
                {kLayerHeight, size.height},
                {kLayerX, next_x},
                {kLayerY, next_y},
            }};
            for (const auto& [index, field_value] : fields) {
                auto stored = set_int_field(machine, *sprite, index, field_value);
                if (!stored) return std::unexpected(stored.error());
            }
            if (dimensions_changed) {
                const std::array<std::pair<usize, i32>, 4> reset {{
                    {kSpriteCollisionX, 0},
                    {kSpriteCollisionY, 0},
                    {kSpriteCollisionWidth, *width},
                    {kSpriteCollisionHeight, *height},
                }};
                for (const auto& [index, field_value] : reset) {
                    auto stored = set_int_field(machine, *sprite,
                                                index, field_value);
                    if (!stored) return std::unexpected(stored.error());
                }
            }
            return no_value();
        });

    add(registry, kSpriteOwner, "paint",
        "(Ljavax/microedition/lcdui/Graphics;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.paint");
            auto graphics_ref = reference_argument(arguments, 1U, "Sprite.paint");
            if (!sprite) return std::unexpected(sprite.error());
            if (!graphics_ref) return std::unexpected(graphics_ref.error());
            auto visible = int_field(machine, *sprite, kLayerVisible);
            if (!visible) return std::unexpected(visible.error());
            if (*visible == 0) return no_value();
            auto state = sprite_state(machine, *sprite);
            if (!state) return std::unexpected(state.error());
            auto frame = raw_frame(machine, *state);
            auto source = image_payload(machine, state->image);
            auto context = graphics_context(machine, *graphics_ref);
            if (!frame) return std::unexpected(frame.error());
            if (!source) return std::unexpected(source.error());
            if (!context) return std::unexpected(context.error());
            auto target = graphics_target(machine, **context);
            if (!target) return std::unexpected(target.error());
            const i32 columns = (*source)->width() / state->frame_width;
            const i32 source_x = (*frame % columns) * state->frame_width;
            const i32 source_y = (*frame / columns) * state->frame_height;
            auto painted = graphics::draw_region(
                **target, **context, **source, source_x, source_y,
                state->frame_width, state->frame_height, state->transform,
                state->x, state->y,
                graphics::anchor_left | graphics::anchor_top);
            if (!painted) return graphics_error(painted.error());
            return no_value();
        });

    add(registry, kSpriteOwner, "collidesWith",
        "(Ljavax/microedition/lcdui/game/Sprite;Z)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.collidesWith");
            auto other = reference_argument(arguments, 1U, "Sprite.collidesWith");
            auto pixel = int_argument(arguments, 2U, "Sprite.collidesWith");
            if (!sprite) return std::unexpected(sprite.error());
            if (!other) return std::unexpected(other.error());
            if (!pixel) return std::unexpected(pixel.error());
            auto left = sprite_state(machine, *sprite);
            auto right = sprite_state(machine, *other);
            if (!left) return std::unexpected(left.error());
            if (!right) return std::unexpected(right.error());
            auto collided = collide_sprite_sprite(machine, *left, *right, *pixel != 0);
            if (!collided) return std::unexpected(collided.error());
            return std::optional<Value>(Value::from_int(*collided ? 1 : 0));
        });
    add(registry, kSpriteOwner, "collidesWith",
        "(Ljavax/microedition/lcdui/game/TiledLayer;Z)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.collidesWith");
            auto tiled = reference_argument(arguments, 1U, "Sprite.collidesWith");
            auto pixel = int_argument(arguments, 2U, "Sprite.collidesWith");
            if (!sprite) return std::unexpected(sprite.error());
            if (!tiled) return std::unexpected(tiled.error());
            if (!pixel) return std::unexpected(pixel.error());
            auto sprite_value = sprite_state(machine, *sprite);
            auto tiled_value = tiled_state(machine, *tiled);
            if (!sprite_value) return std::unexpected(sprite_value.error());
            if (!tiled_value) return std::unexpected(tiled_value.error());
            auto collided = collide_sprite_tiled(machine, *sprite_value,
                                                  *tiled_value, *pixel != 0);
            if (!collided) return std::unexpected(collided.error());
            return std::optional<Value>(Value::from_int(*collided ? 1 : 0));
        });
    add(registry, kSpriteOwner, "collidesWith",
        "(Ljavax/microedition/lcdui/Image;IIZ)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto sprite = receiver(arguments, "Sprite.collidesWith");
            auto image = reference_argument(arguments, 1U, "Sprite.collidesWith");
            auto x = int_argument(arguments, 2U, "Sprite.collidesWith");
            auto y = int_argument(arguments, 3U, "Sprite.collidesWith");
            auto pixel = int_argument(arguments, 4U, "Sprite.collidesWith");
            if (!sprite) return std::unexpected(sprite.error());
            if (!image) return std::unexpected(image.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            if (!pixel) return std::unexpected(pixel.error());
            auto state = sprite_state(machine, *sprite);
            if (!state) return std::unexpected(state.error());
            auto collided = collide_sprite_image(machine, *state, *image,
                                                  *x, *y, *pixel != 0);
            if (!collided) return std::unexpected(collided.error());
            return std::optional<Value>(Value::from_int(*collided ? 1 : 0));
        });

    add(registry, kTiledLayerOwner, "<init>",
        "(IILjavax/microedition/lcdui/Image;II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.<init>");
            auto columns = int_argument(arguments, 1U, "TiledLayer.<init>");
            auto rows = int_argument(arguments, 2U, "TiledLayer.<init>");
            auto image_ref = reference_argument(arguments, 3U, "TiledLayer.<init>");
            auto tile_width = int_argument(arguments, 4U, "TiledLayer.<init>");
            auto tile_height = int_argument(arguments, 5U, "TiledLayer.<init>");
            if (!tiled) return std::unexpected(tiled.error());
            if (!columns) return std::unexpected(columns.error());
            if (!rows) return std::unexpected(rows.error());
            if (!image_ref) return std::unexpected(image_ref.error());
            if (!tile_width) return std::unexpected(tile_width.error());
            if (!tile_height) return std::unexpected(tile_height.error());
            auto image = image_payload(machine, *image_ref);
            if (!image) return std::unexpected(image.error());
            if (*columns <= 0 || *rows <= 0 || *tile_width <= 0 ||
                *tile_height <= 0 || (*image)->width() % *tile_width != 0 ||
                (*image)->height() % *tile_height != 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "invalid TiledLayer dimensions or tile set");
            }
            const i64 width64 = static_cast<i64>(*columns) * *tile_width;
            const i64 height64 = static_cast<i64>(*rows) * *tile_height;
            if (width64 > std::numeric_limits<i32>::max() ||
                height64 > std::numeric_limits<i32>::max()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TiledLayer dimensions overflow");
            }
            auto base = initialize_layer(machine, *tiled,
                                         static_cast<i32>(width64),
                                         static_cast<i32>(height64));
            if (!base) return std::unexpected(base.error());
            std::vector<i32> empty_cells(static_cast<usize>(*columns) *
                                         static_cast<usize>(*rows), 0);
            auto cells = make_int_array(machine, empty_cells);
            const std::span<const i32> empty_span {};
            auto animated = make_int_array(machine, empty_span);
            if (!cells) return std::unexpected(cells.error());
            if (!animated) return std::unexpected(animated.error());
            const i32 count = ((*image)->width() / *tile_width) *
                              ((*image)->height() / *tile_height);
            auto image_stored = set_ref_field(machine, *tiled, kTiledImage, *image_ref);
            auto cells_stored = set_ref_field(machine, *tiled, kTiledCells, *cells);
            auto animated_stored = set_ref_field(machine, *tiled,
                                                 kTiledAnimated, *animated);
            if (!image_stored) return std::unexpected(image_stored.error());
            if (!cells_stored) return std::unexpected(cells_stored.error());
            if (!animated_stored) return std::unexpected(animated_stored.error());
            const std::array<std::pair<usize, i32>, 5> fields {{
                {kTiledColumns, *columns}, {kTiledRows, *rows},
                {kTiledTileWidth, *tile_width}, {kTiledTileHeight, *tile_height},
                {kTiledStaticCount, count},
            }};
            for (const auto& [index, value] : fields) {
                auto stored = set_int_field(machine, *tiled, index, value);
                if (!stored) return std::unexpected(stored.error());
            }
            return no_value();
        });

    add(registry, kTiledLayerOwner, "getColumns", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.getColumns");
            if (!tiled) return std::unexpected(tiled.error());
            auto value = int_field(machine, *tiled, kTiledColumns);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    add(registry, kTiledLayerOwner, "getRows", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.getRows");
            if (!tiled) return std::unexpected(tiled.error());
            auto value = int_field(machine, *tiled, kTiledRows);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    add(registry, kTiledLayerOwner, "getCellWidth", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.getCellWidth");
            if (!tiled) return std::unexpected(tiled.error());
            auto value = int_field(machine, *tiled, kTiledTileWidth);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    add(registry, kTiledLayerOwner, "getCellHeight", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.getCellHeight");
            if (!tiled) return std::unexpected(tiled.error());
            auto value = int_field(machine, *tiled, kTiledTileHeight);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    add(registry, kTiledLayerOwner, "getCell", "(II)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.getCell");
            auto column = int_argument(arguments, 1U, "TiledLayer.getCell");
            auto row = int_argument(arguments, 2U, "TiledLayer.getCell");
            if (!tiled) return std::unexpected(tiled.error());
            if (!column) return std::unexpected(column.error());
            if (!row) return std::unexpected(row.error());
            auto state = tiled_state(machine, *tiled);
            if (!state) return std::unexpected(state.error());
            auto value = cell_value(machine, *state, *column, *row);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    add(registry, kTiledLayerOwner, "setCell", "(III)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.setCell");
            auto column = int_argument(arguments, 1U, "TiledLayer.setCell");
            auto row = int_argument(arguments, 2U, "TiledLayer.setCell");
            auto tile = int_argument(arguments, 3U, "TiledLayer.setCell");
            if (!tiled) return std::unexpected(tiled.error());
            if (!column) return std::unexpected(column.error());
            if (!row) return std::unexpected(row.error());
            if (!tile) return std::unexpected(tile.error());
            auto state = tiled_state(machine, *tiled);
            if (!state) return std::unexpected(state.error());
            auto valid = validate_tile_index(machine, *state, *tile);
            if (!valid) return std::unexpected(valid.error());
            auto checked = cell_value(machine, *state, *column, *row);
            if (!checked) return std::unexpected(checked.error());
            const usize index = static_cast<usize>(*row) *
                static_cast<usize>(state->columns) + static_cast<usize>(*column);
            auto stored = machine.heap().set_element(state->cells, index,
                                                      Value::from_int(*tile));
            if (!stored) return std::unexpected(stored.error());
            return no_value();
        });
    add(registry, kTiledLayerOwner, "fillCells", "(IIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.fillCells");
            if (!tiled) return std::unexpected(tiled.error());
            std::array<i32, 5> values {};
            for (usize index = 0; index < values.size(); ++index) {
                auto value = int_argument(arguments, index + 1U,
                                          "TiledLayer.fillCells");
                if (!value) return std::unexpected(value.error());
                values[index] = *value;
            }
            auto state = tiled_state(machine, *tiled);
            if (!state) return std::unexpected(state.error());
            if (values[2] < 0 || values[3] < 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TiledLayer fill dimensions cannot be negative");
            }
            if (values[0] < 0 || values[0] >= state->columns ||
                values[1] < 0 || values[1] >= state->rows ||
                values[2] > state->columns - values[0] ||
                values[3] > state->rows - values[1]) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "TiledLayer fill region is outside the layer");
            }
            auto valid = validate_tile_index(machine, *state, values[4]);
            if (!valid) return std::unexpected(valid.error());
            for (i32 row = 0; row < values[3]; ++row) {
                for (i32 column = 0; column < values[2]; ++column) {
                    const usize index = static_cast<usize>(values[1] + row) *
                        static_cast<usize>(state->columns) +
                        static_cast<usize>(values[0] + column);
                    auto stored = machine.heap().set_element(
                        state->cells, index, Value::from_int(values[4]));
                    if (!stored) return std::unexpected(stored.error());
                }
            }
            return no_value();
        });

    add(registry, kTiledLayerOwner, "createAnimatedTile", "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.createAnimatedTile");
            auto tile = int_argument(arguments, 1U, "TiledLayer.createAnimatedTile");
            if (!tiled) return std::unexpected(tiled.error());
            if (!tile) return std::unexpected(tile.error());
            auto state = tiled_state(machine, *tiled);
            if (!state) return std::unexpected(state.error());
            if (*tile < 0 || *tile > state->static_count) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "static tile index does not exist");
            }
            auto values = read_int_array(machine, state->animated,
                                         "TiledLayer.createAnimatedTile");
            if (!values) return std::unexpected(values.error());
            if (values->size() >= static_cast<usize>(std::numeric_limits<i32>::max())) {
                return fail_java("java/lang/IllegalStateException",
                                 "too many animated tiles");
            }
            values->push_back(*tile);
            auto array = make_int_array(machine, *values);
            if (!array) return std::unexpected(array.error());
            auto stored = set_ref_field(machine, *tiled, kTiledAnimated, *array);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_int(-static_cast<i32>(values->size())));
        });
    add(registry, kTiledLayerOwner, "getAnimatedTile", "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.getAnimatedTile");
            auto tile = int_argument(arguments, 1U, "TiledLayer.getAnimatedTile");
            if (!tiled) return std::unexpected(tiled.error());
            if (!tile) return std::unexpected(tile.error());
            if (*tile >= 0) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "animated tile index must be negative");
            }
            auto state = tiled_state(machine, *tiled);
            if (!state) return std::unexpected(state.error());
            auto value = resolve_tile(machine, *state, *tile);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    add(registry, kTiledLayerOwner, "setAnimatedTile", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.setAnimatedTile");
            auto animated = int_argument(arguments, 1U, "TiledLayer.setAnimatedTile");
            auto tile = int_argument(arguments, 2U, "TiledLayer.setAnimatedTile");
            if (!tiled) return std::unexpected(tiled.error());
            if (!animated) return std::unexpected(animated.error());
            if (!tile) return std::unexpected(tile.error());
            auto state = tiled_state(machine, *tiled);
            if (!state) return std::unexpected(state.error());
            if (*animated >= 0 || *tile < 0 || *tile > state->static_count) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "invalid animated or static tile index");
            }
            const i64 slot = -static_cast<i64>(*animated) - 1;
            auto length = machine.heap().array_length(state->animated);
            if (!length || slot < 0 || static_cast<u64>(slot) >= *length) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "animated tile index does not exist");
            }
            auto stored = machine.heap().set_element(state->animated,
                static_cast<usize>(slot), Value::from_int(*tile));
            if (!stored) return std::unexpected(stored.error());
            return no_value();
        });

    add(registry, kTiledLayerOwner, "setStaticTileSet",
        "(Ljavax/microedition/lcdui/Image;II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.setStaticTileSet");
            auto image_ref = reference_argument(arguments, 1U,
                                                "TiledLayer.setStaticTileSet");
            auto tile_width = int_argument(arguments, 2U,
                                           "TiledLayer.setStaticTileSet");
            auto tile_height = int_argument(arguments, 3U,
                                            "TiledLayer.setStaticTileSet");
            if (!tiled) return std::unexpected(tiled.error());
            if (!image_ref) return std::unexpected(image_ref.error());
            if (!tile_width) return std::unexpected(tile_width.error());
            if (!tile_height) return std::unexpected(tile_height.error());
            auto image = image_payload(machine, *image_ref);
            if (!image) return std::unexpected(image.error());
            if (*tile_width <= 0 || *tile_height <= 0 ||
                (*image)->width() % *tile_width != 0 ||
                (*image)->height() % *tile_height != 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "tile size must divide the image");
            }
            auto state = tiled_state(machine, *tiled);
            if (!state) return std::unexpected(state.error());
            const i32 new_count = ((*image)->width() / *tile_width) *
                                  ((*image)->height() / *tile_height);
            if (new_count < state->static_count) {
                std::vector<i32> cells(static_cast<usize>(state->columns) *
                                       static_cast<usize>(state->rows), 0);
                auto cell_array = make_int_array(machine, cells);
                const std::span<const i32> empty_span {};
                auto animated_array = make_int_array(machine, empty_span);
                if (!cell_array) return std::unexpected(cell_array.error());
                if (!animated_array) return std::unexpected(animated_array.error());
                auto first = set_ref_field(machine, *tiled, kTiledCells, *cell_array);
                auto second = set_ref_field(machine, *tiled, kTiledAnimated,
                                            *animated_array);
                if (!first) return std::unexpected(first.error());
                if (!second) return std::unexpected(second.error());
            }
            const i64 width64 = static_cast<i64>(state->columns) * *tile_width;
            const i64 height64 = static_cast<i64>(state->rows) * *tile_height;
            if (width64 > std::numeric_limits<i32>::max() ||
                height64 > std::numeric_limits<i32>::max()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TiledLayer dimensions overflow");
            }
            auto image_stored = set_ref_field(machine, *tiled, kTiledImage, *image_ref);
            if (!image_stored) return std::unexpected(image_stored.error());
            const std::array<std::pair<usize, i32>, 5> fields {{
                {kTiledTileWidth, *tile_width}, {kTiledTileHeight, *tile_height},
                {kTiledStaticCount, new_count},
                {kLayerWidth, static_cast<i32>(width64)},
                {kLayerHeight, static_cast<i32>(height64)},
            }};
            for (const auto& [index, value] : fields) {
                auto stored = set_int_field(machine, *tiled, index, value);
                if (!stored) return std::unexpected(stored.error());
            }
            return no_value();
        });

    add(registry, kTiledLayerOwner, "paint",
        "(Ljavax/microedition/lcdui/Graphics;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto tiled = receiver(arguments, "TiledLayer.paint");
            auto graphics_ref = reference_argument(arguments, 1U, "TiledLayer.paint");
            if (!tiled) return std::unexpected(tiled.error());
            if (!graphics_ref) return std::unexpected(graphics_ref.error());
            auto visible = int_field(machine, *tiled, kLayerVisible);
            if (!visible) return std::unexpected(visible.error());
            if (*visible == 0) return no_value();
            auto state = tiled_state(machine, *tiled);
            auto context = graphics_context(machine, *graphics_ref);
            if (!state) return std::unexpected(state.error());
            if (!context) return std::unexpected(context.error());
            auto target = graphics_target(machine, **context);
            auto source = image_payload(machine, state->image);
            if (!target) return std::unexpected(target.error());
            if (!source) return std::unexpected(source.error());
            const i64 layer_left = static_cast<i64>(state->x) +
                                   (*context)->translate_x;
            const i64 layer_top = static_cast<i64>(state->y) +
                                  (*context)->translate_y;
            const WorldRect layer_bounds {
                layer_left,
                layer_top,
                layer_left + static_cast<i64>(state->columns) *
                    state->tile_width,
                layer_top + static_cast<i64>(state->rows) *
                    state->tile_height,
            };
            const WorldRect clip_bounds {
                (*context)->clip.x,
                (*context)->clip.y,
                static_cast<i64>((*context)->clip.x) +
                    (*context)->clip.width,
                static_cast<i64>((*context)->clip.y) +
                    (*context)->clip.height,
            };
            const WorldRect visible_bounds = intersect_rect(
                layer_bounds, clip_bounds);
            if (!nonempty(visible_bounds)) return no_value();
            const i32 first_column = static_cast<i32>(
                (visible_bounds.left - layer_left) / state->tile_width);
            const i32 last_column = static_cast<i32>(
                (visible_bounds.right - layer_left + state->tile_width - 1) /
                state->tile_width);
            const i32 first_row = static_cast<i32>(
                (visible_bounds.top - layer_top) / state->tile_height);
            const i32 last_row = static_cast<i32>(
                (visible_bounds.bottom - layer_top + state->tile_height - 1) /
                state->tile_height);
            const i32 source_columns = (*source)->width() / state->tile_width;
            for (i32 row = first_row; row < last_row; ++row) {
                for (i32 column = first_column; column < last_column; ++column) {
                    auto tile = cell_value(machine, *state, column, row);
                    if (!tile) return std::unexpected(tile.error());
                    auto resolved = resolve_tile(machine, *state, *tile);
                    if (!resolved) return std::unexpected(resolved.error());
                    if (*resolved == 0) continue;
                    const i32 index = *resolved - 1;
                    const i32 source_x = (index % source_columns) * state->tile_width;
                    const i32 source_y = (index / source_columns) * state->tile_height;
                    auto painted = graphics::draw_region(
                        **target, **context, **source, source_x, source_y,
                        state->tile_width, state->tile_height,
                        graphics::Transform::none,
                        wrapping_add(state->x,
                                     column * state->tile_width),
                        wrapping_add(state->y,
                                     row * state->tile_height),
                        graphics::anchor_left | graphics::anchor_top);
                    if (!painted) return graphics_error(painted.error());
                }
            }
            return no_value();
        });

    add(registry, kLayerManagerOwner, "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto manager = receiver(arguments, "LayerManager.<init>");
            if (!manager) return std::unexpected(manager.error());
            const std::span<const ObjectRef> empty_span {};
            auto layers = make_layer_array(machine, empty_span);
            if (!layers) return std::unexpected(layers.error());
            auto stored = set_ref_field(machine, *manager, kManagerLayers, *layers);
            if (!stored) return std::unexpected(stored.error());
            const std::array<std::pair<usize, i32>, 5> fields {{
                {kManagerCount, 0}, {kManagerViewX, 0}, {kManagerViewY, 0},
                {kManagerViewWidth, std::numeric_limits<i32>::max()},
                {kManagerViewHeight, std::numeric_limits<i32>::max()},
            }};
            for (const auto& [index, value] : fields) {
                auto field_stored = set_int_field(machine, *manager, index, value);
                if (!field_stored) return std::unexpected(field_stored.error());
            }
            return no_value();
        });
    add(registry, kLayerManagerOwner, "getSize", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto manager = receiver(arguments, "LayerManager.getSize");
            if (!manager) return std::unexpected(manager.error());
            auto count = int_field(machine, *manager, kManagerCount);
            if (!count) return std::unexpected(count.error());
            return std::optional<Value>(Value::from_int(*count));
        });
    add(registry, kLayerManagerOwner, "getLayerAt",
        "(I)Ljavax/microedition/lcdui/game/Layer;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto manager = receiver(arguments, "LayerManager.getLayerAt");
            auto index = int_argument(arguments, 1U, "LayerManager.getLayerAt");
            if (!manager) return std::unexpected(manager.error());
            if (!index) return std::unexpected(index.error());
            auto layers = manager_layers(machine, *manager);
            if (!layers) return std::unexpected(layers.error());
            if (*index < 0 || static_cast<usize>(*index) >= layers->size()) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "LayerManager index is outside the list");
            }
            return std::optional<Value>(Value::from_reference(
                (*layers)[static_cast<usize>(*index)]));
        });
    add(registry, kLayerManagerOwner, "append",
        "(Ljavax/microedition/lcdui/game/Layer;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto manager = receiver(arguments, "LayerManager.append");
            auto layer = reference_argument(arguments, 1U, "LayerManager.append");
            if (!manager) return std::unexpected(manager.error());
            if (!layer) return std::unexpected(layer.error());
            auto layers = manager_layers(machine, *manager);
            if (!layers) return std::unexpected(layers.error());
            std::erase(*layers, *layer);
            layers->push_back(*layer);
            auto stored = store_manager_layers(machine, *manager, *layers);
            if (!stored) return std::unexpected(stored.error());
            return no_value();
        });
    add(registry, kLayerManagerOwner, "insert",
        "(Ljavax/microedition/lcdui/game/Layer;I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto manager = receiver(arguments, "LayerManager.insert");
            auto layer = reference_argument(arguments, 1U, "LayerManager.insert");
            auto index = int_argument(arguments, 2U, "LayerManager.insert");
            if (!manager) return std::unexpected(manager.error());
            if (!layer) return std::unexpected(layer.error());
            if (!index) return std::unexpected(index.error());
            auto layers = manager_layers(machine, *manager);
            if (!layers) return std::unexpected(layers.error());
            const auto existing = std::find(layers->begin(), layers->end(), *layer);
            const bool already_present = existing != layers->end();
            if (*index < 0 || static_cast<usize>(*index) > layers->size() ||
                (already_present && static_cast<usize>(*index) >= layers->size())) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "LayerManager insertion index is invalid");
            }
            if (already_present) layers->erase(existing);
            const usize target = static_cast<usize>(*index);
            layers->insert(layers->begin() + static_cast<isize>(target), *layer);
            auto stored = store_manager_layers(machine, *manager, *layers);
            if (!stored) return std::unexpected(stored.error());
            return no_value();
        });
    add(registry, kLayerManagerOwner, "remove",
        "(Ljavax/microedition/lcdui/game/Layer;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto manager = receiver(arguments, "LayerManager.remove");
            auto layer = reference_argument(arguments, 1U, "LayerManager.remove");
            if (!manager) return std::unexpected(manager.error());
            if (!layer) return std::unexpected(layer.error());
            auto layers = manager_layers(machine, *manager);
            if (!layers) return std::unexpected(layers.error());
            std::erase(*layers, *layer);
            auto stored = store_manager_layers(machine, *manager, *layers);
            if (!stored) return std::unexpected(stored.error());
            return no_value();
        });

    add(registry, kLayerManagerOwner, "setViewWindow", "(IIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto manager = receiver(arguments, "LayerManager.setViewWindow");
            if (!manager) return std::unexpected(manager.error());
            std::array<i32, 4> values {};
            for (usize index = 0; index < values.size(); ++index) {
                auto value = int_argument(arguments, index + 1U,
                                          "LayerManager.setViewWindow");
                if (!value) return std::unexpected(value.error());
                values[index] = *value;
            }
            if (values[2] < 0 || values[3] < 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "LayerManager view dimensions cannot be negative");
            }
            for (usize index = 0; index < values.size(); ++index) {
                auto stored = set_int_field(machine, *manager,
                                            kManagerViewX + index, values[index]);
                if (!stored) return std::unexpected(stored.error());
            }
            return no_value();
        });

    add(registry, kLayerManagerOwner, "paint",
        "(Ljavax/microedition/lcdui/Graphics;II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto manager = receiver(arguments, "LayerManager.paint");
            auto graphics_ref = reference_argument(arguments, 1U,
                                                   "LayerManager.paint");
            auto x = int_argument(arguments, 2U, "LayerManager.paint");
            auto y = int_argument(arguments, 3U, "LayerManager.paint");
            if (!manager) return std::unexpected(manager.error());
            if (!graphics_ref) return std::unexpected(graphics_ref.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            auto context = graphics_context(machine, *graphics_ref);
            if (!context) return std::unexpected(context.error());
            auto target = graphics_target(machine, **context);
            if (!target) return std::unexpected(target.error());
            auto view_x = int_field(machine, *manager, kManagerViewX);
            auto view_y = int_field(machine, *manager, kManagerViewY);
            auto view_width = int_field(machine, *manager, kManagerViewWidth);
            auto view_height = int_field(machine, *manager, kManagerViewHeight);
            if (!view_x || !view_y || !view_width || !view_height) {
                return fail(ErrorCode::invalid_state,
                            "LayerManager view fields are unavailable");
            }
            auto storage = manager_storage(machine, *manager);
            if (!storage) return std::unexpected(storage.error());
            const i32 saved_translate_x = (*context)->translate_x;
            const i32 saved_translate_y = (*context)->translate_y;
            const graphics::Rect saved_clip = (*context)->clip;
            const auto restore_view_state = [&]() noexcept {
                (*context)->translate_x = saved_translate_x;
                (*context)->translate_y = saved_translate_y;
                (*context)->clip = saved_clip;
            };
            graphics::translate(**context,
                                wrapping_subtract(*x, *view_x),
                                wrapping_subtract(*y, *view_y));
            auto clipped = graphics::clip_rect(**context, **target,
                                               *view_x, *view_y,
                                               *view_width, *view_height);
            if (!clipped) {
                restore_view_state();
                return graphics_error(clipped.error());
            }
            for (usize reverse = storage->count; reverse > 0U; --reverse) {
                auto layer_value = machine.heap().element(
                    storage->array, reverse - 1U);
                if (!layer_value) {
                    restore_view_state();
                    return std::unexpected(layer_value.error());
                }
                auto layer = layer_value->as_reference();
                if (!layer) {
                    restore_view_state();
                    return std::unexpected(layer.error());
                }
                auto visible = int_field(machine, *layer, kLayerVisible);
                if (!visible) {
                    restore_view_state();
                    return std::unexpected(visible.error());
                }
                if (*visible == 0) continue;
                const Value callback_arguments[] {
                    Value::from_reference(*graphics_ref),
                };
                auto result = machine.invoke_instance(
                    *layer, kLayerOwner, "paint",
                    "(Ljavax/microedition/lcdui/Graphics;)V",
                    callback_arguments);
                if (!result) {
                    restore_view_state();
                    return std::unexpected(result.error());
                }
                if (!result->completed_normally()) {
                    restore_view_state();
                    if (!result->throwable.has_value()) {
                        return fail(ErrorCode::internal_error,
                                    "Layer.paint failed without throwable");
                    }
                    auto throwable = machine.heap().class_name(*result->throwable);
                    if (!throwable) return std::unexpected(throwable.error());
                    return fail_java(*throwable,
                                     "LayerManager paint callback failed");
                }
            }
            restore_view_state();
            return no_value();
        });
}

} // namespace phoneme::vm
