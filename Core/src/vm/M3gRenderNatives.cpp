#include "M3gNativeModules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "M3gLoader.hpp"
#include "M3gNativeSupport.hpp"
#include "phoneme/runtime/ParallelExecutor.hpp"

namespace phoneme::vm {
namespace {

using namespace m3g;

constexpr const char* kVertexArray = "javax/microedition/m3g/VertexArray";
constexpr const char* kVertexBuffer = "javax/microedition/m3g/VertexBuffer";
constexpr const char* kIndexBuffer = "javax/microedition/m3g/IndexBuffer";
constexpr const char* kAppearance = "javax/microedition/m3g/Appearance";
constexpr const char* kTexture2D = "javax/microedition/m3g/Texture2D";
constexpr const char* kMaterial = "javax/microedition/m3g/Material";
constexpr const char* kFog = "javax/microedition/m3g/Fog";
constexpr const char* kLight = "javax/microedition/m3g/Light";
constexpr usize kTextureUnitCount = 8U;
constexpr usize kParallelM3gRasterPixels = 8U * 1024U;

struct BoundRenderTarget final {
    graphics::Image* image {nullptr};
    graphics::GraphicsContext context {};
};

struct RenderArea final {
    graphics::Rect viewport {};
    graphics::Rect clip {};
};

struct VertexArrayData final {
    i32 vertex_count {0};
    i32 component_count {0};
    std::vector<float> values;
};

struct Position final {
    float x {0.0F};
    float y {0.0F};
    float z {0.0F};
};

struct TexCoord final {
    float u {0.0F};
    float v {0.0F};
};

struct Vector3 final {
    float x {0.0F};
    float y {0.0F};
    float z {0.0F};
};

struct VertexColor final {
    float a {1.0F};
    float r {1.0F};
    float g {1.0F};
    float b {1.0F};
};

struct MaterialState final {
    bool enabled {false};
    bool vertex_color_tracking {false};
    VertexColor ambient {1.0F, 0.2F, 0.2F, 0.2F};
    VertexColor diffuse {1.0F, 0.8F, 0.8F, 0.8F};
    VertexColor emissive {1.0F, 0.0F, 0.0F, 0.0F};
    VertexColor specular {1.0F, 0.0F, 0.0F, 0.0F};
    float shininess {0.0F};
};

struct FogState final {
    bool enabled {false};
    i32 mode {81};
    VertexColor color {1.0F, 0.0F, 0.0F, 0.0F};
    float density {1.0F};
    float near_distance {0.0F};
    float far_distance {1.0F};
};

struct LightState final {
    i32 mode {129};
    i32 scope {-1};
    VertexColor color {1.0F, 1.0F, 1.0F, 1.0F};
    float intensity {1.0F};
    float constant_attenuation {1.0F};
    float linear_attenuation {0.0F};
    float quadratic_attenuation {0.0F};
    float spot_angle {45.0F};
    float spot_exponent {0.0F};
    Vector3 position {};
    Vector3 direction {0.0F, 0.0F, -1.0F};
};

struct SceneLight final {
    ObjectRef light {};
    Matrix transform {identity_matrix()};
};

struct TextureData final {
    i32 width {0};
    i32 height {0};
    i32 format {100};
    i32 wrap_s {240};
    i32 wrap_t {240};
    i32 blending {227};
    i32 image_filter {210};
    graphics::Pixel blend_color {0xFF000000U};
    Matrix transform {identity_matrix()};
    std::vector<graphics::Pixel> pixels;
};

using TextureUnits =
    std::array<std::optional<TextureData>, kTextureUnitCount>;

struct ScreenVertex final {
    float x {0.0F};
    float y {0.0F};
    float depth {1.0F};
    float inverse_w {0.0F};
    std::array<float, kTextureUnitCount> u_over_w {};
    std::array<float, kTextureUnitCount> v_over_w {};
    float alpha_over_w {0.0F};
    float red_over_w {0.0F};
    float green_over_w {0.0F};
    float blue_over_w {0.0F};
    float fog_distance_over_w {0.0F};
    bool visible {false};
};

struct DepthKey final {
    const Machine* machine {nullptr};
    u64 graphics3d {0U};

    [[nodiscard]] bool operator==(const DepthKey&) const noexcept = default;
};

struct DepthKeyHash final {
    [[nodiscard]] usize operator()(const DepthKey& key) const noexcept {
        const auto pointer = reinterpret_cast<std::uintptr_t>(key.machine);
        return std::hash<std::uintptr_t> {}(pointer) ^
               (std::hash<u64> {}(key.graphics3d) << 1U);
    }
};

struct DepthSurface final {
    i32 width {0};
    i32 height {0};
    std::vector<float> values;
};

struct RenderState final {
    i32 culling {160};
    i32 winding {168};
    i32 shading {165};
    i32 blending {68};
    float alpha_threshold {0.0F};
    bool alpha_write {true};
    bool color_write {true};
    bool depth_write {true};
    bool depth_test {true};
    bool two_sided_lighting {false};
    float depth_offset_factor {0.0F};
    float depth_offset_units {0.0F};
};

std::mutex g_depth_mutex;
std::unordered_map<DepthKey, DepthSurface, DepthKeyHash> g_depth_surfaces;

[[nodiscard]] Result<BoundRenderTarget> bound_render_target(
    Machine& machine,
    ObjectRef graphics3d) {
    auto target = reference_field(machine, graphics3d, kGraphics3D,
                                  "target", "Ljava/lang/Object;");
    if (!target) return std::unexpected(target.error());
    if (target->is_null()) {
        return fail_java("java/lang/IllegalStateException",
                         "Graphics3D has no bound target");
    }
    auto class_name = machine.heap().class_name(*target);
    if (!class_name) return std::unexpected(class_name.error());

    if (*class_name == "javax/microedition/lcdui/Graphics") {
        auto context = machine.graphics().context(target->bits);
        if (!context) return std::unexpected(context.error());
        auto image = machine.graphics().image((*context)->target_key);
        if (!image) return std::unexpected(image.error());
        return BoundRenderTarget {.image = *image, .context = **context};
    }

    ObjectRef image_object {};
    if (*class_name == kImage2D) {
        auto mutable_value = int_field(
            machine, *target, kImage2D, "mutable", "Z");
        if (!mutable_value) return std::unexpected(mutable_value.error());
        if (*mutable_value != 0) {
            auto image = machine.graphics().image(target->bits);
            if (!image) {
                auto width = int_field(machine, *target, kImage2D, "width");
                auto height = int_field(machine, *target, kImage2D, "height");
                if (!width) return std::unexpected(width.error());
                if (!height) return std::unexpected(height.error());
                auto created = graphics::Image::create_mutable(*width, *height);
                if (!created) return std::unexpected(created.error());
                auto attached = machine.graphics().attach_image(
                    target->bits, std::move(*created));
                if (!attached) return std::unexpected(attached.error());
                image = machine.graphics().image(target->bits);
                if (!image) return std::unexpected(image.error());
            }
            graphics::GraphicsContext context {};
            context.target_key = target->bits;
            context.clip = graphics::target_bounds(**image);
            return BoundRenderTarget {.image = *image, .context = context};
        }
        auto source = reference_field(machine, *target, kImage2D,
                                      "source", "Ljava/lang/Object;");
        if (!source) return std::unexpected(source.error());
        if (!source->is_null()) {
            auto source_class = machine.heap().class_name(*source);
            if (!source_class) return std::unexpected(source_class.error());
            if (*source_class == "javax/microedition/lcdui/Image") {
                image_object = *source;
            }
        }
    } else if (*class_name == "javax/microedition/lcdui/Image") {
        image_object = *target;
    }
    if (image_object.is_null()) {
        return fail_java("java/lang/IllegalArgumentException",
                         "Graphics3D target is not a renderable image");
    }
    auto image = machine.graphics().image(image_object.bits);
    if (!image) return std::unexpected(image.error());
    graphics::GraphicsContext context {};
    context.target_key = image_object.bits;
    context.clip = graphics::target_bounds(**image);
    return BoundRenderTarget {.image = *image, .context = context};
}

[[nodiscard]] Result<RenderArea> render_area(
    Machine& machine,
    ObjectRef graphics3d,
    const BoundRenderTarget& target) {
    static constexpr std::array<ObjectFieldSpec, 4U> kFields {{
        {"viewportX", "I"},
        {"viewportY", "I"},
        {"viewportWidth", "I"},
        {"viewportHeight", "I"},
    }};
    auto fields = object_fields(machine, graphics3d, kGraphics3D, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto x = (*fields)[0U].as_int();
    auto y = (*fields)[1U].as_int();
    auto width = (*fields)[2U].as_int();
    auto height = (*fields)[3U].as_int();
    if (!x) return std::unexpected(x.error());
    if (!y) return std::unexpected(y.error());
    if (!width) return std::unexpected(width.error());
    if (!height) return std::unexpected(height.error());
    graphics::Rect viewport {
        *x + target.context.translate_x,
        *y + target.context.translate_y,
        *width,
        *height,
    };
    const graphics::Rect clip = graphics::intersect(
        graphics::intersect(viewport, target.context.clip),
        graphics::target_bounds(*target.image));
    return RenderArea {.viewport = viewport, .clip = clip};
}

[[nodiscard]] DepthSurface& depth_surface(Machine& machine,
                                          ObjectRef graphics3d,
                                          i32 width,
                                          i32 height) {
    auto& surface = g_depth_surfaces[DepthKey {&machine, graphics3d.bits}];
    const usize count = static_cast<usize>(width) *
                        static_cast<usize>(height);
    if (surface.width != width || surface.height != height ||
        surface.values.size() != count) {
        surface.width = width;
        surface.height = height;
        surface.values.assign(count, std::numeric_limits<float>::infinity());
    }
    return surface;
}

void erase_depth_surface(Machine& machine, ObjectRef graphics3d) {
    std::scoped_lock lock(g_depth_mutex);
    g_depth_surfaces.erase(DepthKey {&machine, graphics3d.bits});
}

[[nodiscard]] Result<ObjectRef> snapshot_matrix(Machine& machine,
                                                const Matrix& matrix) {
    auto snapshot = allocate_instance(machine, kTransform);
    if (!snapshot) return std::unexpected(snapshot.error());
    auto root = machine.pin_native_root(*snapshot);
    if (!root) return std::unexpected(root.error());
    auto stored = set_transform_matrix(machine, *snapshot, matrix);
    if (!stored) return std::unexpected(stored.error());
    return *snapshot;
}

[[nodiscard]] Result<ObjectRef> snapshot_transform(Machine& machine,
                                                   ObjectRef source) {
    Matrix matrix = identity_matrix();
    if (!source.is_null()) {
        auto loaded = transform_matrix(machine, source);
        if (!loaded) return std::unexpected(loaded.error());
        matrix = *loaded;
    }
    return snapshot_matrix(machine, matrix);
}

[[nodiscard]] Result<std::vector<i32>> read_int_array(
    Machine& machine,
    ObjectRef array,
    std::string_view operation) {
    if (array.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         std::string(operation) + " array is null");
    }
    auto result = machine.heap().read_int_array(array);
    if (!result && result.error().code == ErrorCode::invalid_argument) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " expects int[]");
    }
    if (!result) return std::unexpected(result.error());
    return result;
}

[[nodiscard]] Result<std::vector<ObjectRef>> read_reference_array(
    Machine& machine,
    ObjectRef array,
    std::string_view operation) {
    if (array.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         std::string(operation) + " array is null");
    }
    auto result = machine.heap().read_reference_array(array);
    if (!result && result.error().code == ErrorCode::invalid_argument) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " expects an object array");
    }
    if (!result) return std::unexpected(result.error());
    return result;
}

[[nodiscard]] Result<std::vector<u8>> read_byte_array(
    Machine& machine,
    ObjectRef array,
    std::string_view operation) {
    if (array.is_null()) return std::vector<u8> {};
    auto result = machine.heap().read_byte_array(array);
    if (!result && result.error().code == ErrorCode::invalid_argument) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " expects byte[]");
    }
    if (!result) return std::unexpected(result.error());
    return result;
}

[[nodiscard]] Result<VertexArrayData> vertex_array_data(
    Machine& machine,
    ObjectRef array,
    std::string_view operation) {
    static constexpr std::array<ObjectFieldSpec, 4U> kFields {{
        {"vertexCount", "I"},
        {"componentCount", "I"},
        {"componentSize", "I"},
        {"data", "Ljava/lang/Object;"},
    }};
    auto fields = object_fields(machine, array, kVertexArray, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto count = (*fields)[0U].as_int();
    auto components = (*fields)[1U].as_int();
    auto component_size = (*fields)[2U].as_int();
    auto data = (*fields)[3U].as_reference();
    if (!count) return std::unexpected(count.error());
    if (!components) return std::unexpected(components.error());
    if (!component_size) return std::unexpected(component_size.error());
    if (!data) return std::unexpected(data.error());
    if (*count <= 0 || *components < 2 || *components > 4 ||
        (*component_size != 1 && *component_size != 2) || data->is_null()) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " has invalid vertex data");
    }
    const usize value_count = static_cast<usize>(*count) *
                              static_cast<usize>(*components);
    std::vector<float> values(value_count);
    if (*component_size == 1) {
        auto source = machine.heap().read_byte_array(*data);
        if (!source) return std::unexpected(source.error());
        if (source->size() < value_count) {
            return fail(ErrorCode::invalid_state,
                        std::string(operation) + " vertex data is truncated");
        }
        for (usize index = 0U; index < value_count; ++index) {
            values[index] = static_cast<float>(
                static_cast<std::int8_t>((*source)[index]));
        }
    } else {
        auto source = machine.heap().read_short_array(*data);
        if (!source) return std::unexpected(source.error());
        if (source->size() < value_count) {
            return fail(ErrorCode::invalid_state,
                        std::string(operation) + " vertex data is truncated");
        }
        for (usize index = 0U; index < value_count; ++index) {
            values[index] = static_cast<float>((*source)[index]);
        }
    }
    return VertexArrayData {
        .vertex_count = *count,
        .component_count = *components,
        .values = std::move(values),
    };
}

[[nodiscard]] Result<std::vector<float>> nullable_float_array(
    Machine& machine,
    ObjectRef array,
    std::string_view operation) {
    if (array.is_null()) return std::vector<float> {};
    return read_float_array(machine, array, operation);
}

[[nodiscard]] Result<std::vector<Position>> positions(
    Machine& machine,
    ObjectRef vertex_buffer) {
    static constexpr std::array<ObjectFieldSpec, 3U> kFields {{
        {"positions", "Ljavax/microedition/m3g/VertexArray;"},
        {"positionScale", "F"},
        {"positionBias", "[F"},
    }};
    auto fields = object_fields(machine, vertex_buffer, kVertexBuffer, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto array = (*fields)[0U].as_reference();
    auto scale = (*fields)[1U].as_float();
    auto bias = (*fields)[2U].as_reference();
    if (!array) return std::unexpected(array.error());
    if (!scale) return std::unexpected(scale.error());
    if (!bias) return std::unexpected(bias.error());
    if (array->is_null()) {
        return fail_java("java/lang/IllegalStateException",
                         "VertexBuffer has no positions");
    }
    auto source = vertex_array_data(machine, *array,
                                    "VertexBuffer positions");
    if (!source) return std::unexpected(source.error());
    auto offsets = nullable_float_array(machine, *bias,
                                        "VertexBuffer position bias");
    if (!offsets) return std::unexpected(offsets.error());
    std::vector<Position> result(static_cast<usize>(source->vertex_count));
    for (i32 vertex = 0; vertex < source->vertex_count; ++vertex) {
        const usize base = static_cast<usize>(vertex) *
                           static_cast<usize>(source->component_count);
        const auto component = [&](usize index) {
            const float value = index < static_cast<usize>(source->component_count)
                ? source->values[base + index] : 0.0F;
            const float offset = index < offsets->size()
                ? (*offsets)[index] : 0.0F;
            return value * *scale + offset;
        };
        result[static_cast<usize>(vertex)] = Position {
            component(0U), component(1U), component(2U)};
    }
    return result;
}

struct TextureCoordinateState final {
    std::vector<ObjectRef> arrays;
    std::vector<float> scales;
    std::vector<ObjectRef> biases;
};

[[nodiscard]] Result<TextureCoordinateState> texture_coordinate_state(
    Machine& machine,
    ObjectRef vertex_buffer) {
    static constexpr std::array<ObjectFieldSpec, 3U> kFields {{
        {"texCoords", "[Ljavax/microedition/m3g/VertexArray;"},
        {"texScales", "[F"},
        {"texBiases", "[[F"},
    }};
    auto fields = object_fields(machine, vertex_buffer, kVertexBuffer, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto arrays = (*fields)[0U].as_reference();
    auto scales = (*fields)[1U].as_reference();
    auto biases = (*fields)[2U].as_reference();
    if (!arrays) return std::unexpected(arrays.error());
    if (!scales) return std::unexpected(scales.error());
    if (!biases) return std::unexpected(biases.error());
    TextureCoordinateState state;
    if (arrays->is_null()) return state;
    auto array_values = read_reference_array(
        machine, *arrays, "VertexBuffer texture coordinates");
    auto scale_values = read_float_array(
        machine, *scales, "VertexBuffer texture scales");
    auto bias_values = read_reference_array(
        machine, *biases, "VertexBuffer texture biases");
    if (!array_values) return std::unexpected(array_values.error());
    if (!scale_values) return std::unexpected(scale_values.error());
    if (!bias_values) return std::unexpected(bias_values.error());
    state.arrays = std::move(*array_values);
    state.scales = std::move(*scale_values);
    state.biases = std::move(*bias_values);
    return state;
}

[[nodiscard]] Result<std::vector<TexCoord>> texture_coordinates_from_state(
    Machine& machine,
    const TextureCoordinateState& state,
    usize vertex_count,
    usize unit) {
    std::vector<TexCoord> result(vertex_count);
    if (unit >= kTextureUnitCount || unit >= state.arrays.size() ||
        unit >= state.scales.size() || unit >= state.biases.size()) {
        return result;
    }
    const ObjectRef array = state.arrays[unit];
    const float scale = state.scales[unit];
    const ObjectRef bias = state.biases[unit];
    if (array.is_null()) return result;
    auto source = vertex_array_data(machine, array,
                                    "VertexBuffer texture coordinates");
    if (!source) return std::unexpected(source.error());
    auto offsets = nullable_float_array(machine, bias,
                                        "VertexBuffer texture bias");
    if (!offsets) return std::unexpected(offsets.error());
    const usize count = std::min(vertex_count,
                                 static_cast<usize>(source->vertex_count));
    for (usize vertex = 0U; vertex < count; ++vertex) {
        const usize base = vertex *
                           static_cast<usize>(source->component_count);
        const float u = source->values[base] * scale +
                        (offsets->empty() ? 0.0F : (*offsets)[0U]);
        const float v = source->values[base + 1U] * scale +
                        (offsets->size() < 2U ? 0.0F : (*offsets)[1U]);
        result[vertex] = TexCoord {u, v};
    }
    return result;
}

using TextureCoordinateSets =
    std::array<std::vector<TexCoord>, kTextureUnitCount>;

[[nodiscard]] Result<TextureCoordinateSets> all_texture_coordinates(
    Machine& machine,
    ObjectRef vertex_buffer,
    usize vertex_count) {
    auto state = texture_coordinate_state(machine, vertex_buffer);
    if (!state) return std::unexpected(state.error());
    TextureCoordinateSets result;
    for (usize unit = 0U; unit < kTextureUnitCount; ++unit) {
        auto coordinates = texture_coordinates_from_state(
            machine, *state, vertex_count, unit);
        if (!coordinates) return std::unexpected(coordinates.error());
        result[unit] = std::move(*coordinates);
    }
    return result;
}

[[nodiscard]] std::array<float, 4> transform_point(
    const Matrix& matrix,
    float x,
    float y,
    float z,
    float w = 1.0F) noexcept;

[[nodiscard]] Vector3 transform_normal(
    const Matrix& inverse_model,
    const Vector3& normal) noexcept;

[[nodiscard]] float vector_length(const Vector3& value) noexcept {
    return std::sqrt(value.x * value.x + value.y * value.y +
                     value.z * value.z);
}

[[nodiscard]] Vector3 normalized(Vector3 value,
                                 Vector3 fallback = {0.0F, 0.0F, 1.0F}) noexcept {
    const float length = vector_length(value);
    if (!std::isfinite(length) || length <= 1.0e-8F) return fallback;
    const float inverse = 1.0F / length;
    value.x *= inverse;
    value.y *= inverse;
    value.z *= inverse;
    return value;
}

[[nodiscard]] float dot(const Vector3& left,
                        const Vector3& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] Vector3 subtract(const Vector3& left,
                               const Vector3& right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] Vector3 negate(const Vector3& value) noexcept {
    return {-value.x, -value.y, -value.z};
}

[[nodiscard]] VertexColor color_from_argb(i32 value) noexcept {
    const auto pixel = static_cast<graphics::Pixel>(static_cast<u32>(value));
    constexpr float scale = 1.0F / 255.0F;
    return {
        static_cast<float>(graphics::alpha(pixel)) * scale,
        static_cast<float>(graphics::red(pixel)) * scale,
        static_cast<float>(graphics::green(pixel)) * scale,
        static_cast<float>(graphics::blue(pixel)) * scale,
    };
}

[[nodiscard]] graphics::Pixel color_to_argb(VertexColor value) noexcept {
    const auto channel = [](float component) -> u8 {
        const float scaled = std::clamp(component, 0.0F, 1.0F) * 255.0F;
        return static_cast<u8>(std::lround(scaled));
    };
    return graphics::argb(channel(value.a), channel(value.r),
                          channel(value.g), channel(value.b));
}

[[nodiscard]] Result<std::vector<Vector3>> vertex_normals(
    Machine& machine,
    ObjectRef vertex_buffer,
    usize vertex_count) {
    auto array = reference_field(machine, vertex_buffer, kVertexBuffer,
        "normals", "Ljavax/microedition/m3g/VertexArray;");
    if (!array) return std::unexpected(array.error());
    if (array->is_null()) return std::vector<Vector3> {};
    auto source = vertex_array_data(machine, *array,
                                    "VertexBuffer normals");
    if (!source) return std::unexpected(source.error());
    if (source->component_count != 3 ||
        static_cast<usize>(source->vertex_count) != vertex_count) {
        return fail_java("java/lang/IllegalStateException",
                         "VertexBuffer normals are incompatible");
    }
    std::vector<Vector3> result(vertex_count);
    for (usize vertex = 0U; vertex < vertex_count; ++vertex) {
        const usize base = vertex * 3U;
        result[vertex] = normalized(Vector3 {
            source->values[base], source->values[base + 1U],
            source->values[base + 2U],
        });
    }
    return result;
}

[[nodiscard]] Result<std::vector<VertexColor>> vertex_colors(
    Machine& machine,
    ObjectRef vertex_buffer,
    usize vertex_count) {
    static constexpr std::array<ObjectFieldSpec, 2U> kBufferFields {{
        {"defaultColor", "I"},
        {"colors", "Ljavax/microedition/m3g/VertexArray;"},
    }};
    auto buffer_fields = object_fields(
        machine, vertex_buffer, kVertexBuffer, kBufferFields);
    if (!buffer_fields) return std::unexpected(buffer_fields.error());
    auto default_color = (*buffer_fields)[0U].as_int();
    auto array = (*buffer_fields)[1U].as_reference();
    if (!default_color) return std::unexpected(default_color.error());
    if (!array) return std::unexpected(array.error());
    std::vector<VertexColor> result(
        vertex_count, color_from_argb(*default_color));
    if (array->is_null()) return result;
    static constexpr std::array<ObjectFieldSpec, 4U> kArrayFields {{
        {"componentCount", "I"},
        {"componentSize", "I"},
        {"vertexCount", "I"},
        {"data", "Ljava/lang/Object;"},
    }};
    auto array_fields = object_fields(machine, *array, kVertexArray, kArrayFields);
    if (!array_fields) return std::unexpected(array_fields.error());
    auto components = (*array_fields)[0U].as_int();
    auto component_size = (*array_fields)[1U].as_int();
    auto count = (*array_fields)[2U].as_int();
    auto data = (*array_fields)[3U].as_reference();
    if (!components) return std::unexpected(components.error());
    if (!component_size) return std::unexpected(component_size.error());
    if (!count) return std::unexpected(count.error());
    if (!data) return std::unexpected(data.error());
    if ((*components != 3 && *components != 4) || *component_size != 1 ||
        *count < 0 || static_cast<usize>(*count) != vertex_count ||
        data->is_null()) {
        return fail_java("java/lang/IllegalStateException",
                         "VertexBuffer colors are incompatible");
    }
    auto color_values = machine.heap().read_byte_array(*data);
    if (!color_values) return std::unexpected(color_values.error());
    const usize required = vertex_count * static_cast<usize>(*components);
    if (color_values->size() < required) {
        return fail(ErrorCode::invalid_state,
                    "VertexBuffer color data is truncated");
    }
    constexpr float scale = 1.0F / 255.0F;
    for (usize vertex = 0U; vertex < vertex_count; ++vertex) {
        const usize base = vertex * static_cast<usize>(*components);
        std::array<float, 4> values {1.0F, 1.0F, 1.0F, 1.0F};
        for (usize component = 0U;
             component < static_cast<usize>(*components); ++component) {
            values[component] =
                static_cast<float>((*color_values)[base + component]) * scale;
        }
        result[vertex] = VertexColor {
            *components == 4 ? values[3U] : 1.0F,
            values[0U], values[1U], values[2U],
        };
    }
    return result;
}

struct DeformedGeometry final {
    std::vector<Position> positions;
    std::vector<Vector3> normals;
    std::vector<VertexColor> colors;
    TextureCoordinateSets texture_coordinates;
};

[[nodiscard]] Result<std::array<bool, kTextureUnitCount>>
texture_coordinate_presence(Machine& machine,
                            ObjectRef vertex_buffer) {
    std::array<bool, kTextureUnitCount> result {};
    auto arrays = reference_field(machine, vertex_buffer, kVertexBuffer,
        "texCoords", "[Ljavax/microedition/m3g/VertexArray;");
    if (!arrays) return std::unexpected(arrays.error());
    if (arrays->is_null()) return result;
    auto values = read_reference_array(
        machine, *arrays, "VertexBuffer texture coordinates");
    if (!values) return std::unexpected(values.error());
    const usize count = std::min(values->size(), kTextureUnitCount);
    for (usize unit = 0U; unit < count; ++unit) {
        result[unit] = !(*values)[unit].is_null();
    }
    return result;
}

[[nodiscard]] Result<DeformedGeometry> morphing_geometry(
    Machine& machine,
    ObjectRef mesh,
    ObjectRef base_vertex_buffer) {
    auto base_positions = positions(machine, base_vertex_buffer);
    if (!base_positions) return std::unexpected(base_positions.error());
    auto base_normals = vertex_normals(
        machine, base_vertex_buffer, base_positions->size());
    auto base_colors = vertex_colors(
        machine, base_vertex_buffer, base_positions->size());
    auto base_textures = all_texture_coordinates(
        machine, base_vertex_buffer, base_positions->size());
    if (!base_normals) return std::unexpected(base_normals.error());
    if (!base_colors) return std::unexpected(base_colors.error());
    if (!base_textures) return std::unexpected(base_textures.error());
    static constexpr std::array<ObjectFieldSpec, 2U> kBasePresenceFields {{
        {"normals", "Ljavax/microedition/m3g/VertexArray;"},
        {"colors", "Ljavax/microedition/m3g/VertexArray;"},
    }};
    auto base_presence = object_fields(
        machine, base_vertex_buffer, kVertexBuffer, kBasePresenceFields);
    if (!base_presence) return std::unexpected(base_presence.error());
    auto base_normals_ref = (*base_presence)[0U].as_reference();
    auto base_colors_ref = (*base_presence)[1U].as_reference();
    if (!base_normals_ref) return std::unexpected(base_normals_ref.error());
    if (!base_colors_ref) return std::unexpected(base_colors_ref.error());
    const bool base_has_normals = !base_normals_ref->is_null();
    const bool base_has_colors = !base_colors_ref->is_null();

    static constexpr std::array<ObjectFieldSpec, 2U> kMorphFields {{
        {"morphTargets", "[Ljavax/microedition/m3g/VertexBuffer;"},
        {"weights", "[F"},
    }};
    auto morph_fields = object_fields(
        machine, mesh, "javax/microedition/m3g/MorphingMesh", kMorphFields);
    if (!morph_fields) return std::unexpected(morph_fields.error());
    auto targets = (*morph_fields)[0U].as_reference();
    auto weights = (*morph_fields)[1U].as_reference();
    if (!targets) return std::unexpected(targets.error());
    if (!weights) return std::unexpected(weights.error());
    if (targets->is_null() || weights->is_null()) {
        return DeformedGeometry {
            .positions = std::move(*base_positions),
            .normals = std::move(*base_normals),
            .colors = std::move(*base_colors),
            .texture_coordinates = std::move(*base_textures),
        };
    }
    auto target_buffers = read_reference_array(
        machine, *targets, "MorphingMesh targets");
    auto weight_values = read_float_array(machine, *weights,
                                          "MorphingMesh weights");
    if (!target_buffers) return std::unexpected(target_buffers.error());
    if (!weight_values) return std::unexpected(weight_values.error());
    if (target_buffers->size() != weight_values->size()) {
        return fail(ErrorCode::invalid_state,
                    "MorphingMesh target and weight counts differ");
    }
    const usize target_count = target_buffers->size();
    float weight_sum = 0.0F;
    for (const float value : *weight_values) {
        if (!std::isfinite(value)) {
            return fail_java("java/lang/IllegalStateException",
                             "MorphingMesh weight is not finite");
        }
        weight_sum += value;
    }
    auto base_texture_presence = texture_coordinate_presence(
        machine, base_vertex_buffer);
    if (!base_texture_presence) {
        return std::unexpected(base_texture_presence.error());
    }
    const auto& base_has_texture = *base_texture_presence;
    usize position_target_count = 0U;
    usize normal_target_count = 0U;
    usize color_target_count = 0U;
    std::array<usize, kTextureUnitCount> texture_target_counts {};
    for (usize target_index = 0U; target_index < target_count;
         ++target_index) {
        const ObjectRef target = (*target_buffers)[target_index];
        if (target.is_null()) {
            return fail_java("java/lang/IllegalStateException",
                             "MorphingMesh target is null");
        }
        static constexpr std::array<ObjectFieldSpec, 3U> kPresenceFields {{
            {"positions", "Ljavax/microedition/m3g/VertexArray;"},
            {"normals", "Ljavax/microedition/m3g/VertexArray;"},
            {"colors", "Ljavax/microedition/m3g/VertexArray;"},
        }};
        auto presence_fields = object_fields(
            machine, target, kVertexBuffer, kPresenceFields);
        if (!presence_fields) return std::unexpected(presence_fields.error());
        auto positions_ref = (*presence_fields)[0U].as_reference();
        auto normals_ref = (*presence_fields)[1U].as_reference();
        auto colors_ref = (*presence_fields)[2U].as_reference();
        if (!positions_ref) return std::unexpected(positions_ref.error());
        if (!normals_ref) return std::unexpected(normals_ref.error());
        if (!colors_ref) return std::unexpected(colors_ref.error());
        position_target_count += positions_ref->is_null() ? 0U : 1U;
        normal_target_count += normals_ref->is_null() ? 0U : 1U;
        color_target_count += colors_ref->is_null() ? 0U : 1U;
        auto texture_presence = texture_coordinate_presence(machine, target);
        if (!texture_presence) return std::unexpected(texture_presence.error());
        for (usize unit = 0U; unit < kTextureUnitCount; ++unit) {
            texture_target_counts[unit] += (*texture_presence)[unit] ? 1U : 0U;
        }
    }
    const auto validate_target_attribute = [target_count](
        bool base_present,
        usize present_count,
        const char* name) -> Result<bool> {
        if ((!base_present && present_count != 0U) ||
            (base_present && present_count != 0U &&
             present_count != target_count)) {
            return fail_java(
                "java/lang/IllegalStateException",
                std::string("MorphingMesh ") + name +
                    " arrays are incompatible");
        }
        return base_present && present_count == target_count;
    };
    auto morph_positions = validate_target_attribute(
        true, position_target_count, "position");
    auto morph_normals = validate_target_attribute(
        base_has_normals, normal_target_count, "normal");
    auto morph_colors = validate_target_attribute(
        base_has_colors, color_target_count, "color");
    if (!morph_positions) return std::unexpected(morph_positions.error());
    if (!morph_normals) return std::unexpected(morph_normals.error());
    if (!morph_colors) return std::unexpected(morph_colors.error());
    std::array<bool, kTextureUnitCount> morph_textures {};
    for (usize unit = 0U; unit < kTextureUnitCount; ++unit) {
        auto morph = validate_target_attribute(
            base_has_texture[unit], texture_target_counts[unit],
            "texture coordinate");
        if (!morph) return std::unexpected(morph.error());
        morph_textures[unit] = *morph;
    }
    DeformedGeometry result {
        .positions = *morph_positions
            ? std::vector<Position>(base_positions->size())
            : *base_positions,
        .normals = *morph_normals
            ? std::vector<Vector3>(base_positions->size())
            : *base_normals,
        .colors = *base_colors,
        .texture_coordinates = {},
    };
    const float base_weight = 1.0F - weight_sum;
    for (usize index = 0U; index < base_positions->size(); ++index) {
        if (*morph_positions) {
            result.positions[index] = Position {
                (*base_positions)[index].x * base_weight,
                (*base_positions)[index].y * base_weight,
                (*base_positions)[index].z * base_weight,
            };
        }
        if (*morph_normals) {
            result.normals[index] = Vector3 {
                (*base_normals)[index].x * base_weight,
                (*base_normals)[index].y * base_weight,
                (*base_normals)[index].z * base_weight,
            };
        }
        if (*morph_colors) {
            result.colors[index] = VertexColor {
                (*base_colors)[index].a * base_weight,
                (*base_colors)[index].r * base_weight,
                (*base_colors)[index].g * base_weight,
                (*base_colors)[index].b * base_weight,
            };
        }
    }
    for (usize unit = 0U; unit < kTextureUnitCount; ++unit) {
        result.texture_coordinates[unit] = (*base_textures)[unit];
        if (!morph_textures[unit]) continue;
        for (usize vertex = 0U; vertex < base_positions->size(); ++vertex) {
            result.texture_coordinates[unit][vertex] = TexCoord {
                (*base_textures)[unit][vertex].u * base_weight,
                (*base_textures)[unit][vertex].v * base_weight,
            };
        }
    }
    for (usize target_index = 0U; target_index < target_count;
         ++target_index) {
        const ObjectRef target = (*target_buffers)[target_index];
        const float weight = (*weight_values)[target_index];
        if (*morph_positions) {
            auto target_positions = positions(machine, target);
            if (!target_positions) {
                return std::unexpected(target_positions.error());
            }
            if (target_positions->size() != result.positions.size()) {
                return fail_java(
                    "java/lang/IllegalStateException",
                    "MorphingMesh target positions are incompatible");
            }
            for (usize vertex = 0U; vertex < result.positions.size(); ++vertex) {
                result.positions[vertex].x +=
                    (*target_positions)[vertex].x * weight;
                result.positions[vertex].y +=
                    (*target_positions)[vertex].y * weight;
                result.positions[vertex].z +=
                    (*target_positions)[vertex].z * weight;
            }
        }
        if (*morph_normals) {
            auto target_normals = vertex_normals(
                machine, target, result.positions.size());
            if (!target_normals) {
                return std::unexpected(target_normals.error());
            }
            for (usize vertex = 0U; vertex < result.normals.size(); ++vertex) {
                result.normals[vertex].x +=
                    (*target_normals)[vertex].x * weight;
                result.normals[vertex].y +=
                    (*target_normals)[vertex].y * weight;
                result.normals[vertex].z +=
                    (*target_normals)[vertex].z * weight;
            }
        }
        if (*morph_colors) {
            auto target_colors = vertex_colors(
                machine, target, result.positions.size());
            if (!target_colors) {
                return std::unexpected(target_colors.error());
            }
            for (usize vertex = 0U; vertex < result.colors.size(); ++vertex) {
                result.colors[vertex].a +=
                    (*target_colors)[vertex].a * weight;
                result.colors[vertex].r +=
                    (*target_colors)[vertex].r * weight;
                result.colors[vertex].g +=
                    (*target_colors)[vertex].g * weight;
                result.colors[vertex].b +=
                    (*target_colors)[vertex].b * weight;
            }
        }
        if (std::any_of(morph_textures.begin(), morph_textures.end(),
                        [](bool value) { return value; })) {
            auto target_textures = all_texture_coordinates(
                machine, target, result.positions.size());
            if (!target_textures) {
                return std::unexpected(target_textures.error());
            }
            for (usize unit = 0U; unit < kTextureUnitCount; ++unit) {
                if (!morph_textures[unit]) continue;
                const auto& target_texture = (*target_textures)[unit];
                if (target_texture.size() !=
                    result.texture_coordinates[unit].size()) {
                    return fail_java(
                        "java/lang/IllegalStateException",
                        "MorphingMesh texture coordinates are incompatible");
                }
                for (usize vertex = 0U;
                     vertex < result.texture_coordinates[unit].size(); ++vertex) {
                    result.texture_coordinates[unit][vertex].u +=
                        target_texture[vertex].u * weight;
                    result.texture_coordinates[unit][vertex].v +=
                        target_texture[vertex].v * weight;
                }
            }
        }
    }
    for (Vector3& normal : result.normals) {
        normal = normalized(normal);
    }
    for (VertexColor& color : result.colors) {
        color.a = std::clamp(color.a, 0.0F, 1.0F);
        color.r = std::clamp(color.r, 0.0F, 1.0F);
        color.g = std::clamp(color.g, 0.0F, 1.0F);
        color.b = std::clamp(color.b, 0.0F, 1.0F);
    }
    return result;
}

[[nodiscard]] Result<DeformedGeometry> skinned_geometry(
    Machine& machine,
    ObjectRef mesh,
    ObjectRef base_vertex_buffer) {
    auto base_positions = positions(machine, base_vertex_buffer);
    if (!base_positions) return std::unexpected(base_positions.error());
    auto base_normals = vertex_normals(
        machine, base_vertex_buffer, base_positions->size());
    auto base_colors = vertex_colors(
        machine, base_vertex_buffer, base_positions->size());
    auto base_textures = all_texture_coordinates(
        machine, base_vertex_buffer, base_positions->size());
    if (!base_normals) return std::unexpected(base_normals.error());
    if (!base_colors) return std::unexpected(base_colors.error());
    if (!base_textures) return std::unexpected(base_textures.error());
    DeformedGeometry result {
        .positions = *base_positions,
        .normals = *base_normals,
        .colors = *base_colors,
        .texture_coordinates = std::move(*base_textures),
    };
    auto bones = reference_field(
        machine, mesh, "javax/microedition/m3g/SkinnedMesh",
        "bones", "[Ljavax/microedition/m3g/Node;");
    auto first_vertices = reference_field(
        machine, mesh, "javax/microedition/m3g/SkinnedMesh",
        "boneFirstVertices", "[I");
    auto vertex_counts = reference_field(
        machine, mesh, "javax/microedition/m3g/SkinnedMesh",
        "boneVertexCounts", "[I");
    auto weights = reference_field(
        machine, mesh, "javax/microedition/m3g/SkinnedMesh",
        "boneWeights", "[I");
    auto rest_transforms = reference_field(
        machine, mesh, "javax/microedition/m3g/SkinnedMesh",
        "boneTransforms", "[[F");
    if (!bones) return std::unexpected(bones.error());
    if (!first_vertices) return std::unexpected(first_vertices.error());
    if (!vertex_counts) return std::unexpected(vertex_counts.error());
    if (!weights) return std::unexpected(weights.error());
    if (!rest_transforms) return std::unexpected(rest_transforms.error());
    if (bones->is_null()) return result;
    auto bone_values = read_reference_array(machine, *bones,
                                            "SkinnedMesh bones");
    auto rest_values = read_reference_array(machine, *rest_transforms,
                                            "SkinnedMesh transforms");
    if (!bone_values) return std::unexpected(bone_values.error());
    if (!rest_values) return std::unexpected(rest_values.error());
    const usize bone_count = bone_values->size();
    if (bone_count == 0U) return result;
    auto first_values = read_int_array(machine, *first_vertices,
                                       "SkinnedMesh first vertices");
    auto count_values = read_int_array(machine, *vertex_counts,
                                       "SkinnedMesh vertex counts");
    auto weight_values = read_int_array(machine, *weights,
                                        "SkinnedMesh weights");
    if (!first_values) return std::unexpected(first_values.error());
    if (!count_values) return std::unexpected(count_values.error());
    if (!weight_values) return std::unexpected(weight_values.error());
    if (first_values->size() != bone_count ||
        count_values->size() != bone_count ||
        weight_values->size() != bone_count ||
        rest_values->size() != bone_count) {
        return fail(ErrorCode::invalid_state,
                    "SkinnedMesh influence arrays differ in length");
    }
    auto mesh_world = node_composite_matrix(machine, mesh);
    if (!mesh_world) return std::unexpected(mesh_world.error());
    auto inverse_mesh = inverse_matrix(*mesh_world);
    if (!inverse_mesh) return std::unexpected(inverse_mesh.error());
    std::vector<Position> accumulated(result.positions.size());
    std::vector<Vector3> accumulated_normals(result.positions.size());
    std::vector<float> accumulated_weights(result.positions.size(), 0.0F);
    for (usize influence = 0U; influence < bone_count; ++influence) {
        const ObjectRef bone = (*bone_values)[influence];
        const ObjectRef rest_array = (*rest_values)[influence];
        if (bone.is_null() || rest_array.is_null()) continue;
        auto rest_matrix_values = read_float_array(machine, rest_array,
                                            "SkinnedMesh rest transform");
        if (!rest_matrix_values) {
            return std::unexpected(rest_matrix_values.error());
        }
        if (rest_matrix_values->size() != 16U) {
            return fail(ErrorCode::invalid_state,
                        "SkinnedMesh rest transform is truncated");
        }
        Matrix rest {};
        std::copy(rest_matrix_values->begin(), rest_matrix_values->end(),
                  rest.begin());
        auto bone_world = node_composite_matrix(machine, bone);
        if (!bone_world) return std::unexpected(bone_world.error());
        const Matrix current_bone_to_mesh = multiply(*inverse_mesh, *bone_world);
        const Matrix deformation = multiply(current_bone_to_mesh, rest);
        auto inverse_deformation = inverse_matrix(deformation);
        if (!inverse_deformation && !base_normals->empty()) {
            return fail_java("java/lang/ArithmeticException",
                             "SkinnedMesh bone transform is singular");
        }
        const i32 first = (*first_values)[influence];
        const i32 count = (*count_values)[influence];
        const i32 raw_weight = (*weight_values)[influence];
        if (first < 0 || count <= 0 || raw_weight <= 0 ||
            static_cast<i64>(first) + count >
                static_cast<i64>(result.positions.size())) {
            return fail(ErrorCode::invalid_state,
                        "SkinnedMesh influence range is invalid");
        }
        const float weight = static_cast<float>(raw_weight);
        for (i32 index = first; index < first + count; ++index) {
            const Position& source = (*base_positions)[static_cast<usize>(index)];
            const auto transformed = transform_point(
                deformation, source.x, source.y, source.z);
            accumulated[static_cast<usize>(index)].x += transformed[0U] * weight;
            accumulated[static_cast<usize>(index)].y += transformed[1U] * weight;
            accumulated[static_cast<usize>(index)].z += transformed[2U] * weight;
            if (!base_normals->empty()) {
                const Vector3 transformed_normal = transform_normal(
                    *inverse_deformation,
                    (*base_normals)[static_cast<usize>(index)]);
                accumulated_normals[static_cast<usize>(index)].x +=
                    transformed_normal.x * weight;
                accumulated_normals[static_cast<usize>(index)].y +=
                    transformed_normal.y * weight;
                accumulated_normals[static_cast<usize>(index)].z +=
                    transformed_normal.z * weight;
            }
            accumulated_weights[static_cast<usize>(index)] += weight;
        }
    }
    for (usize index = 0U; index < result.positions.size(); ++index) {
        if (accumulated_weights[index] <= 0.0F) continue;
        const float inverse = 1.0F / accumulated_weights[index];
        result.positions[index] = Position {
            accumulated[index].x * inverse,
            accumulated[index].y * inverse,
            accumulated[index].z * inverse,
        };
        if (!result.normals.empty()) {
            result.normals[index] = normalized(Vector3 {
                accumulated_normals[index].x * inverse,
                accumulated_normals[index].y * inverse,
                accumulated_normals[index].z * inverse,
            });
        }
    }
    return result;
}

[[nodiscard]] u8 image2d_luminance(graphics::Pixel pixel) noexcept {
    const u32 weighted = 77U * graphics::red(pixel) +
                         150U * graphics::green(pixel) +
                         29U * graphics::blue(pixel);
    return static_cast<u8>((weighted + 128U) >> 8U);
}

[[nodiscard]] Result<std::vector<graphics::Pixel>> convert_image2d_pixels(
    std::span<const graphics::Pixel> source,
    i32 format,
    bool source_has_alpha) {
    if (format < 96 || format > 100) {
        return fail_java("java/lang/IllegalArgumentException",
                         "unsupported Image2D format");
    }
    std::vector<graphics::Pixel> result;
    result.reserve(source.size());
    for (const graphics::Pixel pixel : source) {
        const u8 luminance = image2d_luminance(pixel);
        switch (format) {
        case 96: {
            const u8 alpha = source_has_alpha
                ? graphics::alpha(pixel) : luminance;
            result.push_back(graphics::argb(
                alpha, 255U, 255U, 255U));
            break;
        }
        case 97:
            result.push_back(graphics::argb(
                255U, luminance, luminance, luminance));
            break;
        case 98:
            result.push_back(graphics::argb(
                source_has_alpha ? graphics::alpha(pixel) : 255U,
                luminance, luminance, luminance));
            break;
        case 99:
            result.push_back(graphics::argb(
                255U, graphics::red(pixel), graphics::green(pixel),
                graphics::blue(pixel)));
            break;
        case 100:
            result.push_back(graphics::argb(
                source_has_alpha ? graphics::alpha(pixel) : 255U,
                graphics::red(pixel), graphics::green(pixel),
                graphics::blue(pixel)));
            break;
        default:
            break;
        }
    }
    return result;
}

[[nodiscard]] Result<std::vector<graphics::Pixel>> image2d_pixels(
    Machine& machine,
    ObjectRef image2d,
    i32 width,
    i32 height,
    i32 format) {
    auto render_surface = machine.graphics().image(image2d.bits);
    if (render_surface) {
        const bool source_has_alpha = format == 96 || format == 98 ||
                                      format == 100;
        return convert_image2d_pixels(
            (*render_surface)->pixels(), format, source_has_alpha);
    }
    static constexpr std::array<ObjectFieldSpec, 2U> kFields {{
        {"source", "Ljava/lang/Object;"},
        {"palette", "[B"},
    }};
    auto fields = object_fields(machine, image2d, kImage2D, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto source = (*fields)[0U].as_reference();
    auto palette = (*fields)[1U].as_reference();
    if (!source) return std::unexpected(source.error());
    if (!palette) return std::unexpected(palette.error());
    if (source->is_null()) return std::vector<graphics::Pixel> {};
    auto source_class = machine.heap().class_name(*source);
    if (!source_class) return std::unexpected(source_class.error());
    if (*source_class == "javax/microedition/lcdui/Image") {
        auto image = machine.graphics().image(source->bits);
        if (!image) return std::unexpected(image.error());
        return convert_image2d_pixels(
            (*image)->pixels(), format, !(*image)->is_mutable());
    }
    if (*source_class != "[B") return std::vector<graphics::Pixel> {};
    auto bytes = read_byte_array(machine, *source, "Image2D pixels");
    auto palette_bytes = read_byte_array(machine, *palette, "Image2D palette");
    if (!bytes) return std::unexpected(bytes.error());
    if (!palette_bytes) return std::unexpected(palette_bytes.error());
    const usize pixel_count = static_cast<usize>(width) *
                              static_cast<usize>(height);
    const usize components = format == 96 || format == 97 ? 1U
        : format == 98 ? 2U : format == 99 ? 3U : format == 100 ? 4U : 0U;
    if (components == 0U) {
        return fail_java("java/lang/IllegalArgumentException",
                         "unsupported Image2D format");
    }
    const bool indexed = !palette_bytes->empty();
    const usize required = indexed ? pixel_count : pixel_count * components;
    if (bytes->size() < required) {
        return fail(ErrorCode::invalid_state,
                    "Image2D pixel data is truncated");
    }
    std::vector<graphics::Pixel> result(pixel_count);
    for (usize index = 0U; index < pixel_count; ++index) {
        const usize base = indexed
            ? static_cast<usize>((*bytes)[index]) * components
            : index * components;
        const auto& data = indexed ? *palette_bytes : *bytes;
        if (base + components > data.size()) {
            return fail(ErrorCode::invalid_state,
                        "Image2D palette index is out of range");
        }
        u8 red = 255U;
        u8 green = 255U;
        u8 blue = 255U;
        u8 alpha = 255U;
        if (format == 96) {
            alpha = data[base];
        } else if (format == 97) {
            red = green = blue = data[base];
        } else if (format == 98) {
            red = green = blue = data[base];
            alpha = data[base + 1U];
        } else {
            red = data[base];
            green = data[base + 1U];
            blue = data[base + 2U];
            if (format == 100) alpha = data[base + 3U];
        }
        result[index] = graphics::argb(alpha, red, green, blue);
    }
    return result;
}

[[nodiscard]] Result<TextureData> image2d_texture_data(
    Machine& machine,
    ObjectRef image,
    i32 wrap_s = 241,
    i32 wrap_t = 241) {
    if (image.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Image2D is null");
    }
    static constexpr std::array<ObjectFieldSpec, 3U> kFields {{
        {"width", "I"},
        {"height", "I"},
        {"format", "I"},
    }};
    auto fields = object_fields(machine, image, kImage2D, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto width = (*fields)[0U].as_int();
    auto height = (*fields)[1U].as_int();
    auto format = (*fields)[2U].as_int();
    if (!width) return std::unexpected(width.error());
    if (!height) return std::unexpected(height.error());
    if (!format) return std::unexpected(format.error());
    auto pixels = image2d_pixels(machine, image, *width, *height, *format);
    if (!pixels) return std::unexpected(pixels.error());
    return TextureData {
        .width = *width,
        .height = *height,
        .format = *format,
        .wrap_s = wrap_s,
        .wrap_t = wrap_t,
        .pixels = std::move(*pixels),
    };
}

[[nodiscard]] Result<std::optional<TextureData>> texture_data_from_texture(
    Machine& machine,
    ObjectRef texture) {
    if (texture.is_null()) return std::optional<TextureData> {};
    static constexpr std::array<ObjectFieldSpec, 6U> kTextureFields {{
        {"image", "Ljavax/microedition/m3g/Image2D;"},
        {"wrapS", "I"},
        {"wrapT", "I"},
        {"blending", "I"},
        {"blendColor", "I"},
        {"imageFilter", "I"},
    }};
    auto texture_fields = object_fields(
        machine, texture, kTexture2D, kTextureFields);
    if (!texture_fields) return std::unexpected(texture_fields.error());
    auto image = (*texture_fields)[0U].as_reference();
    auto wrap_s = (*texture_fields)[1U].as_int();
    auto wrap_t = (*texture_fields)[2U].as_int();
    auto blending = (*texture_fields)[3U].as_int();
    auto blend_color = (*texture_fields)[4U].as_int();
    auto image_filter = (*texture_fields)[5U].as_int();
    auto local = local_transform(machine, texture);
    if (!image) return std::unexpected(image.error());
    if (!wrap_s) return std::unexpected(wrap_s.error());
    if (!wrap_t) return std::unexpected(wrap_t.error());
    if (!blending) return std::unexpected(blending.error());
    if (!blend_color) return std::unexpected(blend_color.error());
    if (!image_filter) return std::unexpected(image_filter.error());
    if (!local) return std::unexpected(local.error());
    if (image->is_null()) return std::optional<TextureData> {};
    static constexpr std::array<ObjectFieldSpec, 3U> kImageFields {{
        {"width", "I"},
        {"height", "I"},
        {"format", "I"},
    }};
    auto image_fields = object_fields(machine, *image, kImage2D, kImageFields);
    if (!image_fields) return std::unexpected(image_fields.error());
    auto width = (*image_fields)[0U].as_int();
    auto height = (*image_fields)[1U].as_int();
    auto format = (*image_fields)[2U].as_int();
    if (!width) return std::unexpected(width.error());
    if (!height) return std::unexpected(height.error());
    if (!format) return std::unexpected(format.error());
    auto pixels = image2d_pixels(machine, *image, *width, *height, *format);
    auto matrix = transform_matrix(machine, *local);
    if (!pixels) return std::unexpected(pixels.error());
    if (!matrix) return std::unexpected(matrix.error());
    if (pixels->empty()) return std::optional<TextureData> {};
    return std::optional<TextureData>(TextureData {
        .width = *width,
        .height = *height,
        .format = *format,
        .wrap_s = *wrap_s,
        .wrap_t = *wrap_t,
        .blending = *blending,
        .image_filter = *image_filter,
        .blend_color = static_cast<graphics::Pixel>(
            static_cast<u32>(*blend_color)) | 0xFF000000U,
        .transform = *matrix,
        .pixels = std::move(*pixels),
    });
}

[[nodiscard]] Result<TextureUnits> all_texture_data(
    Machine& machine,
    ObjectRef textures) {
    TextureUnits result;
    if (textures.is_null()) return result;
    auto texture_values = read_reference_array(
        machine, textures, "Appearance textures");
    if (!texture_values) return std::unexpected(texture_values.error());
    const usize count = std::min(texture_values->size(), kTextureUnitCount);
    for (usize unit = 0U; unit < count; ++unit) {
        auto texture = texture_data_from_texture(
            machine, (*texture_values)[unit]);
        if (!texture) return std::unexpected(texture.error());
        result[unit] = std::move(*texture);
    }
    return result;
}

[[nodiscard]] Result<Matrix> camera_projection(Machine& machine,
                                               ObjectRef camera) {
    static constexpr std::array<ObjectFieldSpec, 2U> kFields {{
        {"projectionType", "I"},
        {"projection", "[F"},
    }};
    auto fields = object_fields(machine, camera, kCamera, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto type = (*fields)[0U].as_int();
    auto values = (*fields)[1U].as_reference();
    if (!type) return std::unexpected(type.error());
    if (!values) return std::unexpected(values.error());
    if (values->is_null()) {
        return fail_java("java/lang/IllegalStateException",
                         "Camera projection is not configured");
    }
    auto projection = read_float_array(machine, *values, "Camera projection");
    if (!projection) return std::unexpected(projection.error());
    if (*type == 52) {
        if (projection->size() < 16U) {
            return fail(ErrorCode::invalid_state,
                        "generic Camera projection is truncated");
        }
        Matrix result {};
        std::copy_n(projection->begin(), 16U, result.begin());
        return result;
    }
    if (projection->size() < 4U) {
        return fail(ErrorCode::invalid_state,
                    "Camera projection parameters are truncated");
    }
    const float height_or_fov = (*projection)[0U];
    const float aspect = (*projection)[1U];
    const float near_plane = (*projection)[2U];
    const float far_plane = (*projection)[3U];
    const bool depth_range_valid = *type == 48
        ? far_plane > near_plane
        : near_plane > 0.0F && far_plane > near_plane;
    if (!std::isfinite(height_or_fov) || !std::isfinite(aspect) ||
        !std::isfinite(near_plane) || !std::isfinite(far_plane) ||
        !(height_or_fov > 0.0F) || !(aspect > 0.0F) ||
        !depth_range_valid) {
        return fail_java("java/lang/IllegalStateException",
                         "Camera projection parameters are invalid");
    }
    Matrix result {};
    if (*type == 50) {
        const float radians = height_or_fov *
            (std::numbers::pi_v<float> / 180.0F);
        const float scale = 1.0F / std::tan(radians * 0.5F);
        result[0U] = scale / aspect;
        result[5U] = scale;
        result[10U] = (far_plane + near_plane) /
                      (near_plane - far_plane);
        result[11U] = (2.0F * far_plane * near_plane) /
                      (near_plane - far_plane);
        result[14U] = -1.0F;
    } else if (*type == 48) {
        result = identity_matrix();
        result[0U] = 2.0F / (height_or_fov * aspect);
        result[5U] = 2.0F / height_or_fov;
        result[10U] = -2.0F / (far_plane - near_plane);
        result[11U] = -(far_plane + near_plane) /
                      (far_plane - near_plane);
    } else {
        return fail_java("java/lang/IllegalStateException",
                         "Camera projection type is unsupported");
    }
    return result;
}

[[nodiscard]] std::array<float, 4> transform_point(
    const Matrix& matrix,
    float x,
    float y,
    float z,
    float w) noexcept {
    const std::array<float, 4> input {x, y, z, w};
    std::array<float, 4> output {};
    for (usize row = 0U; row < 4U; ++row) {
        for (usize column = 0U; column < 4U; ++column) {
            output[row] += matrix[row * 4U + column] * input[column];
        }
    }
    return output;
}

[[nodiscard]] graphics::Pixel opaque_m3g_color(i32 color) noexcept {
    return static_cast<graphics::Pixel>(static_cast<u32>(color)) |
           0xFF000000U;
}

[[nodiscard]] float wrapped_coordinate(float value, i32 mode) noexcept {
    if (mode == 241) {
        value -= std::floor(value);
        return value < 0.0F ? value + 1.0F : value;
    }
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] graphics::Pixel interpolate_pixel(
    graphics::Pixel first,
    graphics::Pixel second,
    float amount) noexcept {
    amount = std::clamp(amount, 0.0F, 1.0F);
    const auto channel = [amount](u8 left, u8 right) -> u8 {
        return static_cast<u8>(std::lround(
            static_cast<float>(left) +
            (static_cast<float>(right) - static_cast<float>(left)) * amount));
    };
    return graphics::argb(
        channel(graphics::alpha(first), graphics::alpha(second)),
        channel(graphics::red(first), graphics::red(second)),
        channel(graphics::green(first), graphics::green(second)),
        channel(graphics::blue(first), graphics::blue(second)));
}

[[nodiscard]] graphics::Pixel sample_texture(const TextureData& texture,
                                              float u,
                                              float v) noexcept {
    const auto transformed = transform_point(texture.transform, u, v, 0.0F);
    if (std::isfinite(transformed[3U]) &&
        std::abs(transformed[3U]) > 1.0e-8F) {
        u = transformed[0U] / transformed[3U];
        v = transformed[1U] / transformed[3U];
    } else {
        u = transformed[0U];
        v = transformed[1U];
    }
    u = wrapped_coordinate(u, texture.wrap_s);
    v = wrapped_coordinate(v, texture.wrap_t);
    const float pixel_x = u * static_cast<float>(texture.width) - 0.5F;
    const float pixel_y = v * static_cast<float>(texture.height) - 0.5F;
    const auto texel = [&](i32 x, i32 y) {
        if (texture.wrap_s == 241) {
            x %= texture.width;
            if (x < 0) x += texture.width;
        } else {
            x = std::clamp(x, 0, texture.width - 1);
        }
        if (texture.wrap_t == 241) {
            y %= texture.height;
            if (y < 0) y += texture.height;
        } else {
            y = std::clamp(y, 0, texture.height - 1);
        }
        return texture.pixels[static_cast<usize>(y) *
                              static_cast<usize>(texture.width) +
                              static_cast<usize>(x)];
    };
    if (texture.image_filter != 209) {
        return texel(static_cast<i32>(std::floor(pixel_x + 0.5F)),
                     static_cast<i32>(std::floor(pixel_y + 0.5F)));
    }
    const i32 left = static_cast<i32>(std::floor(pixel_x));
    const i32 top = static_cast<i32>(std::floor(pixel_y));
    const float horizontal = pixel_x - static_cast<float>(left);
    const float vertical = pixel_y - static_cast<float>(top);
    const auto upper = interpolate_pixel(
        texel(left, top), texel(left + 1, top), horizontal);
    const auto lower = interpolate_pixel(
        texel(left, top + 1), texel(left + 1, top + 1), horizontal);
    return interpolate_pixel(upper, lower, vertical);
}

[[nodiscard]] graphics::Pixel apply_texture_function(
    graphics::Pixel fragment,
    graphics::Pixel texel,
    const TextureData& texture) noexcept {
    const VertexColor base = color_from_argb(static_cast<i32>(fragment));
    const VertexColor sample = color_from_argb(static_cast<i32>(texel));
    const VertexColor blend = color_from_argb(
        static_cast<i32>(texture.blend_color));
    const bool has_color = texture.format != 96;
    const bool has_alpha = texture.format == 96 || texture.format == 98 ||
                           texture.format == 100;
    VertexColor result = base;
    switch (texture.blending) {
    case 224: // FUNC_ADD
        if (has_color) {
            result.r = std::min(1.0F, base.r + sample.r);
            result.g = std::min(1.0F, base.g + sample.g);
            result.b = std::min(1.0F, base.b + sample.b);
        }
        result.a = has_alpha ? base.a * sample.a : base.a;
        break;
    case 225: // FUNC_BLEND
        if (has_color) {
            result.r = base.r * (1.0F - sample.r) + blend.r * sample.r;
            result.g = base.g * (1.0F - sample.g) + blend.g * sample.g;
            result.b = base.b * (1.0F - sample.b) + blend.b * sample.b;
        }
        result.a = has_alpha ? base.a * sample.a : base.a;
        break;
    case 226: // FUNC_DECAL
        if (texture.format == 99) {
            result.r = sample.r;
            result.g = sample.g;
            result.b = sample.b;
        } else if (texture.format == 100) {
            result.r = base.r * (1.0F - sample.a) + sample.r * sample.a;
            result.g = base.g * (1.0F - sample.a) + sample.g * sample.a;
            result.b = base.b * (1.0F - sample.a) + sample.b * sample.a;
        }
        result.a = base.a;
        break;
    case 228: // FUNC_REPLACE
        if (has_color) {
            result.r = sample.r;
            result.g = sample.g;
            result.b = sample.b;
        }
        result.a = has_alpha ? sample.a : base.a;
        break;
    case 227: // FUNC_MODULATE
    default:
        if (has_color) {
            result.r = base.r * sample.r;
            result.g = base.g * sample.g;
            result.b = base.b * sample.b;
        }
        result.a = has_alpha ? base.a * sample.a : base.a;
        break;
    }
    return color_to_argb(result);
}

[[nodiscard]] float edge(float ax, float ay,
                         float bx, float by,
                         float px, float py) noexcept {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

[[nodiscard]] Result<MaterialState> appearance_material(
    Machine& machine,
    ObjectRef material) {
    MaterialState state;
    if (material.is_null()) return state;
    static constexpr std::array<ObjectFieldSpec, 6U> kFields {{
        {"ambient", "I"},
        {"diffuse", "I"},
        {"emissive", "I"},
        {"specular", "I"},
        {"shininess", "F"},
        {"vertexColorTracking", "Z"},
    }};
    auto fields = object_fields(machine, material, kMaterial, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto ambient = (*fields)[0U].as_int();
    auto diffuse = (*fields)[1U].as_int();
    auto emissive = (*fields)[2U].as_int();
    auto specular = (*fields)[3U].as_int();
    auto shininess = (*fields)[4U].as_float();
    auto tracking = (*fields)[5U].as_int();
    if (!ambient) return std::unexpected(ambient.error());
    if (!diffuse) return std::unexpected(diffuse.error());
    if (!emissive) return std::unexpected(emissive.error());
    if (!specular) return std::unexpected(specular.error());
    if (!shininess) return std::unexpected(shininess.error());
    if (!tracking) return std::unexpected(tracking.error());
    state.enabled = true;
    state.vertex_color_tracking = *tracking != 0;
    state.ambient = color_from_argb(*ambient);
    state.ambient.a = 1.0F;
    state.diffuse = color_from_argb(*diffuse);
    state.emissive = color_from_argb(*emissive);
    state.emissive.a = 1.0F;
    state.specular = color_from_argb(*specular);
    state.specular.a = 1.0F;
    state.shininess = std::clamp(*shininess, 0.0F, 128.0F);
    return state;
}

[[nodiscard]] Result<FogState> appearance_fog(Machine& machine,
                                               ObjectRef fog) {
    FogState state;
    if (fog.is_null()) return state;
    static constexpr std::array<ObjectFieldSpec, 5U> kFields {{
        {"mode", "I"},
        {"color", "I"},
        {"density", "F"},
        {"nearDistance", "F"},
        {"farDistance", "F"},
    }};
    auto fields = object_fields(machine, fog, kFog, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto mode = (*fields)[0U].as_int();
    auto color = (*fields)[1U].as_int();
    auto density = (*fields)[2U].as_float();
    auto near_distance = (*fields)[3U].as_float();
    auto far_distance = (*fields)[4U].as_float();
    if (!mode) return std::unexpected(mode.error());
    if (!color) return std::unexpected(color.error());
    if (!density) return std::unexpected(density.error());
    if (!near_distance) return std::unexpected(near_distance.error());
    if (!far_distance) return std::unexpected(far_distance.error());
    state.enabled = true;
    state.mode = *mode;
    state.color = color_from_argb(*color);
    state.color.a = 1.0F;
    state.density = std::max(*density, 0.0F);
    state.near_distance = *near_distance;
    state.far_distance = *far_distance;
    return state;
}

[[nodiscard]] Vector3 matrix_position(const Matrix& matrix) noexcept {
    return {matrix[3U], matrix[7U], matrix[11U]};
}

[[nodiscard]] Vector3 matrix_z_axis(const Matrix& matrix) noexcept {
    return normalized(Vector3 {matrix[2U], matrix[6U], matrix[10U]});
}

[[nodiscard]] Vector3 transform_normal(const Matrix& inverse_model,
                                       const Vector3& normal) noexcept {
    return normalized(Vector3 {
        inverse_model[0U] * normal.x + inverse_model[4U] * normal.y +
            inverse_model[8U] * normal.z,
        inverse_model[1U] * normal.x + inverse_model[5U] * normal.y +
            inverse_model[9U] * normal.z,
        inverse_model[2U] * normal.x + inverse_model[6U] * normal.y +
            inverse_model[10U] * normal.z,
    });
}

[[nodiscard]] Result<LightState> load_light(Machine& machine,
                                            ObjectRef light,
                                            const Matrix& transform) {
    LightState state;
    auto scope = int_field(machine, light, kNode, "scope");
    static constexpr std::array<ObjectFieldSpec, 6U> kFields {{
        {"mode", "I"},
        {"color", "I"},
        {"intensity", "F"},
        {"spotAngle", "F"},
        {"spotExponent", "F"},
        {"attenuation", "[F"},
    }};
    auto fields = object_fields(machine, light, kLight, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto mode = (*fields)[0U].as_int();
    auto color = (*fields)[1U].as_int();
    auto intensity = (*fields)[2U].as_float();
    auto angle = (*fields)[3U].as_float();
    auto exponent = (*fields)[4U].as_float();
    auto attenuation = (*fields)[5U].as_reference();
    if (!mode) return std::unexpected(mode.error());
    if (!scope) return std::unexpected(scope.error());
    if (!color) return std::unexpected(color.error());
    if (!intensity) return std::unexpected(intensity.error());
    if (!angle) return std::unexpected(angle.error());
    if (!exponent) return std::unexpected(exponent.error());
    if (!attenuation) return std::unexpected(attenuation.error());
    state.mode = *mode;
    state.scope = *scope;
    state.color = color_from_argb(*color);
    state.color.a = 1.0F;
    state.intensity = *intensity;
    state.spot_angle = *angle;
    state.spot_exponent = *exponent;
    state.position = matrix_position(transform);
    state.direction = negate(matrix_z_axis(transform));
    if (!attenuation->is_null()) {
        auto values = read_float_array(machine, *attenuation,
                                       "Light attenuation");
        if (!values) return std::unexpected(values.error());
        if (values->size() < 3U) {
            return fail(ErrorCode::invalid_state,
                        "Light attenuation is truncated");
        }
        state.constant_attenuation = (*values)[0U];
        state.linear_attenuation = (*values)[1U];
        state.quadratic_attenuation = (*values)[2U];
    }
    return state;
}

[[nodiscard]] Result<std::vector<LightState>> active_lights(
    Machine& machine,
    ObjectRef graphics3d,
    i32 object_scope,
    const std::vector<SceneLight>* scene_lights) {
    std::vector<LightState> result;
    auto append_light = [&](ObjectRef light, const Matrix& transform)
        -> Status {
        if (light.is_null()) return {};
        auto loaded = load_light(machine, light, transform);
        if (!loaded) return std::unexpected(loaded.error());
        if ((static_cast<u32>(loaded->scope) &
             static_cast<u32>(object_scope)) != 0U) {
            result.push_back(*loaded);
        }
        return {};
    };
    static constexpr std::array<ObjectFieldSpec, 3U> kFields {{
        {"lightCount", "I"},
        {"lights", "[Ljavax/microedition/m3g/Light;"},
        {"lightTransforms", "[Ljavax/microedition/m3g/Transform;"},
    }};
    auto fields = object_fields(machine, graphics3d, kGraphics3D, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto count = (*fields)[0U].as_int();
    auto lights = (*fields)[1U].as_reference();
    auto transforms = (*fields)[2U].as_reference();
    if (!count) return std::unexpected(count.error());
    if (!lights) return std::unexpected(lights.error());
    if (!transforms) return std::unexpected(transforms.error());
    if (*count > 0) {
        if (!lights->is_null() && !transforms->is_null()) {
            auto light_values = read_reference_array(
                machine, *lights, "Graphics3D lights");
            auto transform_values = read_reference_array(
                machine, *transforms, "Graphics3D light transforms");
            if (!light_values) return std::unexpected(light_values.error());
            if (!transform_values) {
                return std::unexpected(transform_values.error());
            }
            const usize required = static_cast<usize>(*count);
            if (light_values->size() < required ||
                transform_values->size() < required) {
                return fail(ErrorCode::invalid_state,
                            "Graphics3D light arrays are truncated");
            }
            for (i32 index = 0; index < *count; ++index) {
                const usize position = static_cast<usize>(index);
                const ObjectRef light = (*light_values)[position];
                const ObjectRef transform = (*transform_values)[position];
                Matrix matrix = identity_matrix();
                if (!transform.is_null()) {
                    auto loaded = transform_matrix(machine, transform);
                    if (!loaded) return std::unexpected(loaded.error());
                    matrix = *loaded;
                }
                auto appended = append_light(light, matrix);
                if (!appended) return std::unexpected(appended.error());
            }
        }
    }
    if (scene_lights != nullptr) {
        for (const SceneLight& scene_light : *scene_lights) {
            auto appended = append_light(scene_light.light,
                                         scene_light.transform);
            if (!appended) return std::unexpected(appended.error());
        }
    }
    return result;
}

[[nodiscard]] VertexColor shade_vertex(
    const VertexColor& vertex_color,
    const MaterialState& material,
    const std::vector<LightState>& lights,
    const Vector3& world_position,
    Vector3 world_normal,
    const Vector3& camera_position,
    bool two_sided_lighting,
    float alpha_factor) noexcept {
    if (!material.enabled) {
        VertexColor result = vertex_color;
        result.a *= alpha_factor;
        return result;
    }
    VertexColor ambient = material.vertex_color_tracking
        ? vertex_color : material.ambient;
    VertexColor diffuse = material.vertex_color_tracking
        ? vertex_color : material.diffuse;
    VertexColor result = material.emissive;
    result.a = diffuse.a * alpha_factor;
    const Vector3 view = normalized(subtract(camera_position, world_position),
                                    {0.0F, 0.0F, 1.0F});
    world_normal = normalized(world_normal);
    for (const LightState& light : lights) {
        const float light_red = light.color.r * light.intensity;
        const float light_green = light.color.g * light.intensity;
        const float light_blue = light.color.b * light.intensity;
        if (light.mode == 128) {
            result.r += ambient.r * light_red;
            result.g += ambient.g * light_green;
            result.b += ambient.b * light_blue;
            continue;
        }
        Vector3 direction {};
        float attenuation = 1.0F;
            if (light.mode == 129) {
            direction = negate(light.direction);
        } else {
            const Vector3 offset = subtract(light.position, world_position);
            const float distance = vector_length(offset);
            direction = normalized(offset);
            const float divisor = light.constant_attenuation +
                light.linear_attenuation * distance +
                light.quadratic_attenuation * distance * distance;
            attenuation = divisor > 1.0e-8F ? 1.0F / divisor : 0.0F;
            if (light.mode == 131) {
                const Vector3 from_light = normalized(
                    subtract(world_position, light.position));
                const float spot_cosine = dot(from_light, light.direction);
                const float cutoff = std::cos(
                    light.spot_angle * std::numbers::pi_v<float> / 180.0F);
                if (spot_cosine < cutoff) {
                    attenuation = 0.0F;
                } else {
                    attenuation *= std::pow(
                        std::max(spot_cosine, 0.0F),
                        light.spot_exponent);
                }
            }
        }
        float normal_dot_light = dot(world_normal, direction);
        if (two_sided_lighting) normal_dot_light = std::abs(normal_dot_light);
        normal_dot_light = std::max(normal_dot_light, 0.0F);
        if (normal_dot_light <= 0.0F || attenuation <= 0.0F) continue;
        const float diffuse_factor = normal_dot_light * attenuation;
        result.r += diffuse.r * light_red * diffuse_factor;
        result.g += diffuse.g * light_green * diffuse_factor;
        result.b += diffuse.b * light_blue * diffuse_factor;
        if (material.specular.r > 0.0F || material.specular.g > 0.0F ||
            material.specular.b > 0.0F) {
            const Vector3 half_vector = normalized(Vector3 {
                direction.x + view.x,
                direction.y + view.y,
                direction.z + view.z,
            });
            float normal_dot_half = dot(world_normal, half_vector);
            if (two_sided_lighting) normal_dot_half = std::abs(normal_dot_half);
            const float specular_factor = std::pow(
                std::max(normal_dot_half, 0.0F), material.shininess) *
                attenuation;
            result.r += material.specular.r * light_red * specular_factor;
            result.g += material.specular.g * light_green * specular_factor;
            result.b += material.specular.b * light_blue * specular_factor;
        }
    }
    result.r = std::clamp(result.r, 0.0F, 1.0F);
    result.g = std::clamp(result.g, 0.0F, 1.0F);
    result.b = std::clamp(result.b, 0.0F, 1.0F);
    result.a = std::clamp(result.a, 0.0F, 1.0F);
    return result;
}

[[nodiscard]] VertexColor apply_fog(VertexColor color,
                                    const FogState& fog,
                                    float distance) noexcept {
    if (!fog.enabled || !std::isfinite(distance)) return color;
    float factor = 1.0F;
    if (fog.mode == 80) {
        factor = std::exp(-fog.density * std::max(distance, 0.0F));
    } else {
        const float span = fog.far_distance - fog.near_distance;
        factor = std::abs(span) <= 1.0e-8F
            ? (distance <= fog.near_distance ? 1.0F : 0.0F)
            : (fog.far_distance - distance) / span;
    }
    factor = std::clamp(factor, 0.0F, 1.0F);
    color.r = color.r * factor + fog.color.r * (1.0F - factor);
    color.g = color.g * factor + fog.color.g * (1.0F - factor);
    color.b = color.b * factor + fog.color.b * (1.0F - factor);
    return color;
}

[[nodiscard]] Result<RenderState> appearance_render_state(
    Machine& machine,
    ObjectRef polygon,
    ObjectRef compositing) {
    RenderState state;
    if (!polygon.is_null()) {
        static constexpr std::array<ObjectFieldSpec, 4U> kPolygonFields {{
            {"culling", "I"},
            {"winding", "I"},
            {"shading", "I"},
            {"twoSided", "Z"},
        }};
        auto polygon_fields = object_fields(
            machine, polygon, kPolygonMode, kPolygonFields);
        if (!polygon_fields) return std::unexpected(polygon_fields.error());
        auto culling = (*polygon_fields)[0U].as_int();
        auto winding = (*polygon_fields)[1U].as_int();
        auto shading = (*polygon_fields)[2U].as_int();
        auto two_sided = (*polygon_fields)[3U].as_int();
        if (!culling) return std::unexpected(culling.error());
        if (!winding) return std::unexpected(winding.error());
        if (!shading) return std::unexpected(shading.error());
        if (!two_sided) return std::unexpected(two_sided.error());
        state.culling = *culling;
        state.winding = *winding;
        state.shading = *shading;
        state.two_sided_lighting = *two_sided != 0;
    }
    if (!compositing.is_null()) {
        static constexpr std::array<ObjectFieldSpec, 8U> kCompositingFields {{
            {"blending", "I"},
            {"alphaThreshold", "F"},
            {"depthOffsetFactor", "F"},
            {"depthOffsetUnits", "F"},
            {"alphaWrite", "Z"},
            {"colorWrite", "Z"},
            {"depthWrite", "Z"},
            {"depthTest", "Z"},
        }};
        auto compositing_fields = object_fields(
            machine, compositing, kCompositingMode, kCompositingFields);
        if (!compositing_fields) {
            return std::unexpected(compositing_fields.error());
        }
        auto blending = (*compositing_fields)[0U].as_int();
        auto threshold = (*compositing_fields)[1U].as_float();
        auto offset_factor = (*compositing_fields)[2U].as_float();
        auto offset_units = (*compositing_fields)[3U].as_float();
        auto alpha_write = (*compositing_fields)[4U].as_int();
        auto color_write = (*compositing_fields)[5U].as_int();
        auto depth_write = (*compositing_fields)[6U].as_int();
        auto depth_test = (*compositing_fields)[7U].as_int();
        if (!blending) return std::unexpected(blending.error());
        if (!threshold) return std::unexpected(threshold.error());
        if (!offset_factor) return std::unexpected(offset_factor.error());
        if (!offset_units) return std::unexpected(offset_units.error());
        if (!alpha_write) return std::unexpected(alpha_write.error());
        if (!color_write) return std::unexpected(color_write.error());
        if (!depth_write) return std::unexpected(depth_write.error());
        if (!depth_test) return std::unexpected(depth_test.error());
        state.blending = *blending;
        state.alpha_threshold = std::clamp(*threshold, 0.0F, 1.0F);
        state.depth_offset_factor = *offset_factor;
        state.depth_offset_units = *offset_units;
        state.alpha_write = *alpha_write != 0;
        state.color_write = *color_write != 0;
        state.depth_write = *depth_write != 0;
        state.depth_test = *depth_test != 0;
    }
    return state;
}

[[nodiscard]] u8 blend_channel(u8 source,
                               u8 destination,
                               i32 mode,
                               u8 source_alpha) noexcept {
    const u32 src = source;
    const u32 dst = destination;
    const u32 alpha = source_alpha;
    switch (mode) {
    case 64:
        return static_cast<u8>((src * alpha + dst * (255U - alpha) + 127U) /
                               255U);
    case 65:
        return static_cast<u8>(std::min<u32>(
            255U, (src * alpha + 127U) / 255U + dst));
    case 66:
        return static_cast<u8>((src * dst + 127U) / 255U);
    case 67:
        return static_cast<u8>(std::min<u32>(
            255U, (2U * src * dst + 127U) / 255U));
    case 68:
    default:
        return source;
    }
}

[[nodiscard]] graphics::Pixel composite_pixel(
    graphics::Pixel source,
    graphics::Pixel destination,
    const RenderState& state) noexcept {
    const u8 source_alpha = graphics::alpha(source);
    const u8 red = state.color_write
        ? blend_channel(graphics::red(source), graphics::red(destination),
                        state.blending, source_alpha)
        : graphics::red(destination);
    const u8 green = state.color_write
        ? blend_channel(graphics::green(source), graphics::green(destination),
                        state.blending, source_alpha)
        : graphics::green(destination);
    const u8 blue = state.color_write
        ? blend_channel(graphics::blue(source), graphics::blue(destination),
                        state.blending, source_alpha)
        : graphics::blue(destination);
    const u8 alpha = state.alpha_write
        ? blend_channel(source_alpha, graphics::alpha(destination),
                        state.blending, source_alpha)
        : graphics::alpha(destination);
    return graphics::argb(alpha, red, green, blue);
}

[[nodiscard]] Status rasterize_triangle(
    graphics::Image& image,
    const graphics::Rect& clip,
    DepthSurface& depth,
    const ScreenVertex& first,
    const ScreenVertex& second,
    const ScreenVertex& third,
    const TextureUnits& textures,
    const FogState& fog,
    const RenderState& state) {
    if (!first.visible || !second.visible || !third.visible) return {};
    const float area = edge(first.x, first.y, second.x, second.y,
                            third.x, third.y);
    if (!std::isfinite(area) || std::abs(area) < 1.0e-6F) return {};
    const bool front_facing = state.winding == 169 ? area < 0.0F
                                                    : area > 0.0F;
    if ((state.culling == 160 && !front_facing) ||
        (state.culling == 161 && front_facing)) {
        return {};
    }
    const float plane_denominator =
        (second.x - first.x) * (third.y - first.y) -
        (third.x - first.x) * (second.y - first.y);
    float depth_slope = 0.0F;
    if (std::abs(plane_denominator) > 1.0e-8F) {
        const float depth_dx =
            ((second.depth - first.depth) * (third.y - first.y) -
             (third.depth - first.depth) * (second.y - first.y)) /
            plane_denominator;
        const float depth_dy =
            ((second.x - first.x) * (third.depth - first.depth) -
             (third.x - first.x) * (second.depth - first.depth)) /
            plane_denominator;
        depth_slope = std::max(std::abs(depth_dx), std::abs(depth_dy));
    }
    constexpr float depth_resolution = 1.0F / 16'777'216.0F;
    const float depth_offset =
        state.depth_offset_factor * depth_slope +
        state.depth_offset_units * depth_resolution;
    const i32 minimum_x = std::max(
        clip.x, static_cast<i32>(std::floor(
            std::min({first.x, second.x, third.x}))));
    const i32 maximum_x = std::min(
        clip.x + clip.width - 1, static_cast<i32>(std::ceil(
            std::max({first.x, second.x, third.x}))));
    const i32 minimum_y = std::max(
        clip.y, static_cast<i32>(std::floor(
            std::min({first.y, second.y, third.y}))));
    const i32 maximum_y = std::min(
        clip.y + clip.height - 1, static_cast<i32>(std::ceil(
            std::max({first.y, second.y, third.y}))));
    if (minimum_x > maximum_x || minimum_y > maximum_y) return {};
    auto pixels = image.mutable_pixels();
    const usize raster_width = static_cast<usize>(maximum_x - minimum_x + 1);
    const usize raster_height = static_cast<usize>(maximum_y - minimum_y + 1);
    const auto raster_rows = [&](usize row_begin, usize row_end) {
      for (usize row = row_begin; row < row_end; ++row) {
        const i32 y = minimum_y + static_cast<i32>(row);
        for (i32 x = minimum_x; x <= maximum_x; ++x) {
            const float sample_x = static_cast<float>(x) + 0.5F;
            const float sample_y = static_cast<float>(y) + 0.5F;
            const float first_edge = edge(second.x, second.y,
                                          third.x, third.y,
                                          sample_x, sample_y);
            const float second_edge = edge(third.x, third.y,
                                           first.x, first.y,
                                           sample_x, sample_y);
            const float third_edge = edge(first.x, first.y,
                                          second.x, second.y,
                                          sample_x, sample_y);
            const bool inside = area > 0.0F
                ? first_edge >= 0.0F && second_edge >= 0.0F &&
                  third_edge >= 0.0F
                : first_edge <= 0.0F && second_edge <= 0.0F &&
                  third_edge <= 0.0F;
            if (!inside) continue;
            const float first_weight = first_edge / area;
            const float second_weight = second_edge / area;
            const float third_weight = third_edge / area;
            const float pixel_depth =
                first_weight * first.depth +
                second_weight * second.depth +
                third_weight * third.depth + depth_offset;
            if (pixel_depth < 0.0F || pixel_depth > 1.0F) continue;
            const usize pixel_index = static_cast<usize>(y) *
                                      static_cast<usize>(image.width()) +
                                      static_cast<usize>(x);
            if (pixel_index >= depth.values.size()) continue;
            if (state.depth_test &&
                pixel_depth > depth.values[pixel_index]) {
                continue;
            }
            const float denominator =
                first_weight * first.inverse_w +
                second_weight * second.inverse_w +
                third_weight * third.inverse_w;
            if (std::abs(denominator) < 1.0e-8F) continue;
            VertexColor interpolated;
            float fog_distance = 0.0F;
            if (state.shading == 164) {
                const float inverse = third.inverse_w > 1.0e-8F
                    ? 1.0F / third.inverse_w : 0.0F;
                interpolated = VertexColor {
                    third.alpha_over_w * inverse,
                    third.red_over_w * inverse,
                    third.green_over_w * inverse,
                    third.blue_over_w * inverse,
                };
                fog_distance = third.fog_distance_over_w * inverse;
            } else {
                interpolated = VertexColor {
                    (first_weight * first.alpha_over_w +
                     second_weight * second.alpha_over_w +
                     third_weight * third.alpha_over_w) / denominator,
                    (first_weight * first.red_over_w +
                     second_weight * second.red_over_w +
                     third_weight * third.red_over_w) / denominator,
                    (first_weight * first.green_over_w +
                     second_weight * second.green_over_w +
                     third_weight * third.green_over_w) / denominator,
                    (first_weight * first.blue_over_w +
                     second_weight * second.blue_over_w +
                     third_weight * third.blue_over_w) / denominator,
                };
                fog_distance =
                    (first_weight * first.fog_distance_over_w +
                     second_weight * second.fog_distance_over_w +
                     third_weight * third.fog_distance_over_w) / denominator;
            }
            graphics::Pixel source = color_to_argb(interpolated);
            for (usize unit = 0U; unit < kTextureUnitCount; ++unit) {
                if (!textures[unit].has_value()) continue;
                const float u =
                    (first_weight * first.u_over_w[unit] +
                     second_weight * second.u_over_w[unit] +
                     third_weight * third.u_over_w[unit]) / denominator;
                const float v =
                    (first_weight * first.v_over_w[unit] +
                     second_weight * second.v_over_w[unit] +
                     third_weight * third.v_over_w[unit]) / denominator;
                source = apply_texture_function(
                    source, sample_texture(*textures[unit], u, v),
                    *textures[unit]);
            }
            source = color_to_argb(apply_fog(
                color_from_argb(static_cast<i32>(source)),
                fog, fog_distance));
            if (static_cast<float>(graphics::alpha(source)) / 255.0F <
                state.alpha_threshold) {
                continue;
            }
            pixels[pixel_index] = composite_pixel(
                source, pixels[pixel_index], state);
            if (state.depth_write) depth.values[pixel_index] = pixel_depth;
        }
      }
    };
    if (raster_width * raster_height >= kParallelM3gRasterPixels &&
        raster_height >= 4U) {
        const usize rows_per_chunk = std::max<usize>(
            1U, 2U * 1024U / std::max<usize>(raster_width, 1U));
        runtime::shared_compute_executor().parallel_for(
            raster_height, 4U, rows_per_chunk, raster_rows);
    } else {
        raster_rows(0U, raster_height);
    }
    return {};
}

[[nodiscard]] Status render_geometry(
    Machine& machine,
    ObjectRef graphics3d,
    ObjectRef vertex_buffer,
    ObjectRef index_buffer,
    ObjectRef appearance,
    ObjectRef model_transform,
    const std::vector<Position>* position_override = nullptr,
    const TextureCoordinateSets* texture_override = nullptr,
    const std::vector<Vector3>* normal_override = nullptr,
    const std::vector<VertexColor>* color_override = nullptr,
    i32 object_scope = -1,
    float alpha_factor = 1.0F,
    const std::vector<SceneLight>* scene_lights = nullptr) {
    auto target = bound_render_target(machine, graphics3d);
    if (!target) return std::unexpected(target.error());
    if (!target->context.rendering_enabled) return {};
    auto area = render_area(machine, graphics3d, *target);
    if (!area) return std::unexpected(area.error());
    if (graphics::empty(area->clip)) return {};
    std::vector<Position> vertices;
    if (position_override != nullptr) {
        vertices = *position_override;
    } else {
        auto loaded = positions(machine, vertex_buffer);
        if (!loaded) return std::unexpected(loaded.error());
        vertices = std::move(*loaded);
    }
    TextureCoordinateSets tex_coords;
    if (texture_override != nullptr) {
        tex_coords = *texture_override;
    } else {
        auto loaded = all_texture_coordinates(
            machine, vertex_buffer, vertices.size());
        if (!loaded) return std::unexpected(loaded.error());
        tex_coords = std::move(*loaded);
    }
    for (auto& coordinates : tex_coords) {
        if (coordinates.size() < vertices.size()) {
            coordinates.resize(vertices.size());
        }
    }
    std::vector<VertexColor> colors;
    if (color_override != nullptr) {
        colors = *color_override;
    } else {
        auto loaded = vertex_colors(machine, vertex_buffer, vertices.size());
        if (!loaded) return std::unexpected(loaded.error());
        colors = std::move(*loaded);
    }
    std::vector<Vector3> normals;
    if (normal_override != nullptr) {
        normals = *normal_override;
    } else {
        auto loaded = vertex_normals(machine, vertex_buffer, vertices.size());
        if (!loaded) return std::unexpected(loaded.error());
        normals = std::move(*loaded);
    }
    if (colors.size() < vertices.size()) {
        return fail(ErrorCode::invalid_state,
                    "deformed vertex colors are truncated");
    }
    if (!normals.empty() && normals.size() < vertices.size()) {
        return fail(ErrorCode::invalid_state,
                    "deformed vertex normals are truncated");
    }
    static constexpr std::array<ObjectFieldSpec, 2U> kIndexFields {{
        {"indices", "[I"},
        {"stripLengths", "[I"},
    }};
    auto index_fields = object_fields(
        machine, index_buffer, kIndexBuffer, kIndexFields);
    if (!index_fields) return std::unexpected(index_fields.error());
    auto index_array = (*index_fields)[0U].as_reference();
    auto strip_array = (*index_fields)[1U].as_reference();
    if (!index_array) return std::unexpected(index_array.error());
    if (!strip_array) return std::unexpected(strip_array.error());
    if (index_array->is_null()) return {};
    auto indices = read_int_array(machine, *index_array,
                                  "IndexBuffer indices");
    if (!indices) return std::unexpected(indices.error());
    std::vector<i32> strip_lengths;
    if (strip_array->is_null()) {
        strip_lengths.push_back(static_cast<i32>(indices->size()));
    } else {
        auto lengths = read_int_array(machine, *strip_array,
                                      "IndexBuffer strip lengths");
        if (!lengths) return std::unexpected(lengths.error());
        strip_lengths = std::move(*lengths);
    }
    static constexpr std::array<ObjectFieldSpec, 5U> kGraphicsFields {{
        {"camera", "Ljavax/microedition/m3g/Camera;"},
        {"cameraTransform", "Ljavax/microedition/m3g/Transform;"},
        {"depthNear", "F"},
        {"depthFar", "F"},
        {"depthBuffer", "Z"},
    }};
    auto graphics_fields = object_fields(
        machine, graphics3d, kGraphics3D, kGraphicsFields);
    if (!graphics_fields) return std::unexpected(graphics_fields.error());
    auto camera = (*graphics_fields)[0U].as_reference();
    auto camera_transform = (*graphics_fields)[1U].as_reference();
    auto near_range = (*graphics_fields)[2U].as_float();
    auto far_range = (*graphics_fields)[3U].as_float();
    auto depth_enabled = (*graphics_fields)[4U].as_int();
    if (!camera) return std::unexpected(camera.error());
    if (!camera_transform) return std::unexpected(camera_transform.error());
    if (!near_range) return std::unexpected(near_range.error());
    if (!far_range) return std::unexpected(far_range.error());
    if (!depth_enabled) return std::unexpected(depth_enabled.error());
    if (camera->is_null()) {
        return fail_java("java/lang/IllegalStateException",
                         "Graphics3D has no active camera");
    }
    auto projection = camera_projection(machine, *camera);
    if (!projection) return std::unexpected(projection.error());
    Matrix camera_matrix = identity_matrix();
    if (!camera_transform->is_null()) {
        auto loaded = transform_matrix(machine, *camera_transform);
        if (!loaded) return std::unexpected(loaded.error());
        camera_matrix = *loaded;
    }
    auto view = inverse_matrix(camera_matrix);
    if (!view) return std::unexpected(view.error());
    Matrix model = identity_matrix();
    if (!model_transform.is_null()) {
        auto loaded = transform_matrix(machine, model_transform);
        if (!loaded) return std::unexpected(loaded.error());
        model = *loaded;
    }
    ObjectRef textures_ref {};
    ObjectRef material_ref {};
    ObjectRef fog_ref {};
    ObjectRef polygon_ref {};
    ObjectRef compositing_ref {};
    if (!appearance.is_null()) {
        static constexpr std::array<ObjectFieldSpec, 5U> kAppearanceFields {{
            {"textures", "[Ljavax/microedition/m3g/Texture2D;"},
            {"material", "Ljavax/microedition/m3g/Material;"},
            {"fog", "Ljavax/microedition/m3g/Fog;"},
            {"polygonMode", "Ljavax/microedition/m3g/PolygonMode;"},
            {"compositingMode", "Ljavax/microedition/m3g/CompositingMode;"},
        }};
        auto appearance_fields = object_fields(
            machine, appearance, kAppearance, kAppearanceFields);
        if (!appearance_fields) {
            return std::unexpected(appearance_fields.error());
        }
        auto textures_value = (*appearance_fields)[0U].as_reference();
        auto material_value = (*appearance_fields)[1U].as_reference();
        auto fog_value = (*appearance_fields)[2U].as_reference();
        auto polygon_value = (*appearance_fields)[3U].as_reference();
        auto compositing_value = (*appearance_fields)[4U].as_reference();
        if (!textures_value) return std::unexpected(textures_value.error());
        if (!material_value) return std::unexpected(material_value.error());
        if (!fog_value) return std::unexpected(fog_value.error());
        if (!polygon_value) return std::unexpected(polygon_value.error());
        if (!compositing_value) {
            return std::unexpected(compositing_value.error());
        }
        textures_ref = *textures_value;
        material_ref = *material_value;
        fog_ref = *fog_value;
        polygon_ref = *polygon_value;
        compositing_ref = *compositing_value;
    }
    auto loaded_textures = all_texture_data(machine, textures_ref);
    if (!loaded_textures) return std::unexpected(loaded_textures.error());
    TextureUnits textures = std::move(*loaded_textures);
    auto material = appearance_material(machine, material_ref);
    auto fog = appearance_fog(machine, fog_ref);
    auto state = appearance_render_state(machine, polygon_ref, compositing_ref);
    auto lights = active_lights(machine, graphics3d, object_scope, scene_lights);
    if (!material) return std::unexpected(material.error());
    if (!fog) return std::unexpected(fog.error());
    if (!state) return std::unexpected(state.error());
    if (!lights) return std::unexpected(lights.error());
    if (*depth_enabled == 0) {
        state->depth_test = false;
        state->depth_write = false;
    }
    auto inverse_model = inverse_matrix(model);
    if (!inverse_model && material->enabled) {
        return fail_java("java/lang/ArithmeticException",
                         "Mesh model transform is singular");
    }
    const Vector3 camera_position = matrix_position(camera_matrix);

    std::vector<ScreenVertex> transformed(vertices.size());
    for (usize index = 0U; index < vertices.size(); ++index) {
        const Position& vertex = vertices[index];
        const auto world_position_value = transform_point(
            model, vertex.x, vertex.y, vertex.z);
        const auto view_position_value = transform_point(
            *view, world_position_value[0U], world_position_value[1U],
            world_position_value[2U], world_position_value[3U]);
        const auto clip_position = transform_point(
            *projection, view_position_value[0U], view_position_value[1U],
            view_position_value[2U], view_position_value[3U]);
        const float w = clip_position[3U];
        if (!std::isfinite(w) || w <= 1.0e-6F) continue;
        const float inverse_w = 1.0F / w;
        const float ndc_x = clip_position[0U] * inverse_w;
        const float ndc_y = clip_position[1U] * inverse_w;
        const float ndc_z = clip_position[2U] * inverse_w;
        if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y) ||
            !std::isfinite(ndc_z)) {
            continue;
        }
        std::array<float, kTextureUnitCount> u_over_w {};
        std::array<float, kTextureUnitCount> v_over_w {};
        for (usize unit = 0U; unit < kTextureUnitCount; ++unit) {
            const TexCoord coordinate = index < tex_coords[unit].size()
                ? tex_coords[unit][index] : TexCoord {};
            u_over_w[unit] = coordinate.u * inverse_w;
            v_over_w[unit] = coordinate.v * inverse_w;
        }
        const Vector3 local_normal = normals.empty()
            ? Vector3 {0.0F, 0.0F, 1.0F} : normals[index];
        const Vector3 world_normal = inverse_model
            ? transform_normal(*inverse_model, local_normal)
            : local_normal;
        const VertexColor shaded = shade_vertex(
            colors[index], *material, *lights,
            Vector3 {world_position_value[0U], world_position_value[1U],
                     world_position_value[2U]},
            world_normal, camera_position, state->two_sided_lighting,
            std::clamp(alpha_factor, 0.0F, 1.0F));
        transformed[index] = ScreenVertex {
            .x = static_cast<float>(area->viewport.x) +
                 (ndc_x * 0.5F + 0.5F) *
                 static_cast<float>(area->viewport.width),
            .y = static_cast<float>(area->viewport.y) +
                 (0.5F - ndc_y * 0.5F) *
                 static_cast<float>(area->viewport.height),
            .depth = *near_range + (ndc_z * 0.5F + 0.5F) *
                     (*far_range - *near_range),
            .inverse_w = inverse_w,
            .u_over_w = u_over_w,
            .v_over_w = v_over_w,
            .alpha_over_w = shaded.a * inverse_w,
            .red_over_w = shaded.r * inverse_w,
            .green_over_w = shaded.g * inverse_w,
            .blue_over_w = shaded.b * inverse_w,
            .fog_distance_over_w = std::abs(view_position_value[2U]) *
                                   inverse_w,
            .visible = true,
        };
    }

    std::scoped_lock lock(g_depth_mutex);
    auto& depth = depth_surface(machine, graphics3d,
                                target->image->width(),
                                target->image->height());
    usize offset = 0U;
    for (const i32 strip_length : strip_lengths) {
        if (strip_length < 3 ||
            offset + static_cast<usize>(strip_length) > indices->size()) {
            return fail(ErrorCode::invalid_state,
                        "IndexBuffer strip data is invalid");
        }
        for (i32 triangle = 0; triangle < strip_length - 2; ++triangle) {
            i32 first = (*indices)[offset + static_cast<usize>(triangle)];
            i32 second = (*indices)[offset + static_cast<usize>(triangle + 1)];
            const i32 third =
                (*indices)[offset + static_cast<usize>(triangle + 2)];
            if ((triangle & 1) != 0) std::swap(first, second);
            if (first < 0 || second < 0 || third < 0 ||
                static_cast<usize>(first) >= transformed.size() ||
                static_cast<usize>(second) >= transformed.size() ||
                static_cast<usize>(third) >= transformed.size()) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "IndexBuffer references an invalid vertex");
            }
            auto rasterized = rasterize_triangle(
                *target->image, area->clip, depth,
                transformed[static_cast<usize>(first)],
                transformed[static_cast<usize>(second)],
                transformed[static_cast<usize>(third)],
                textures, *fog, *state);
            if (!rasterized) return rasterized;
        }
        offset += static_cast<usize>(strip_length);
    }
    target->image->mark_dirty_region(area->clip.x, area->clip.y,
                                     area->clip.width, area->clip.height);
    return {};
}

[[nodiscard]] std::optional<i32> background_coordinate(
    i64 value,
    i32 size,
    i32 mode) noexcept {
    if (size <= 0) return std::nullopt;
    if (mode == 33) {
        i64 wrapped = value % static_cast<i64>(size);
        if (wrapped < 0) wrapped += size;
        return static_cast<i32>(wrapped);
    }
    if (value < 0 || value >= size) return std::nullopt;
    return static_cast<i32>(value);
}

[[nodiscard]] Status draw_background_image(
    Machine& machine,
    ObjectRef background,
    BoundRenderTarget& target,
    const RenderArea& area) {
    static constexpr std::array<ObjectFieldSpec, 7U> kFields {{
        {"image", "Ljavax/microedition/m3g/Image2D;"},
        {"cropX", "I"},
        {"cropY", "I"},
        {"cropWidth", "I"},
        {"cropHeight", "I"},
        {"imageModeX", "I"},
        {"imageModeY", "I"},
    }};
    auto fields = object_fields(machine, background, kBackground, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto image = (*fields)[0U].as_reference();
    auto crop_x = (*fields)[1U].as_int();
    auto crop_y = (*fields)[2U].as_int();
    auto crop_width = (*fields)[3U].as_int();
    auto crop_height = (*fields)[4U].as_int();
    auto mode_x = (*fields)[5U].as_int();
    auto mode_y = (*fields)[6U].as_int();
    if (!image) return std::unexpected(image.error());
    if (image->is_null()) return {};
    if (!crop_x) return std::unexpected(crop_x.error());
    if (!crop_y) return std::unexpected(crop_y.error());
    if (!crop_width) return std::unexpected(crop_width.error());
    if (!crop_height) return std::unexpected(crop_height.error());
    if (!mode_x) return std::unexpected(mode_x.error());
    if (!mode_y) return std::unexpected(mode_y.error());
    if (*crop_width == 0 || *crop_height == 0 ||
        area.viewport.width <= 0 || area.viewport.height <= 0) {
        return {};
    }
    auto texture = image2d_texture_data(machine, *image, *mode_x, *mode_y);
    if (!texture) return std::unexpected(texture.error());
    if (texture->pixels.empty()) return {};
    auto pixels = target.image->mutable_pixels();
    const usize background_width = static_cast<usize>(area.clip.width);
    const usize background_height = static_cast<usize>(area.clip.height);
    const auto draw_rows = [&](usize row_begin, usize row_end) {
      for (usize row = row_begin; row < row_end; ++row) {
        const i32 y = area.clip.y + static_cast<i32>(row);
        const double normalized_y =
            (static_cast<double>(y - area.viewport.y) + 0.5) /
            static_cast<double>(area.viewport.height);
        const i64 source_y = static_cast<i64>(std::floor(
            static_cast<double>(*crop_y) +
            normalized_y * static_cast<double>(*crop_height)));
        const auto sampled_y = background_coordinate(
            source_y, texture->height, *mode_y);
        if (!sampled_y.has_value()) continue;
        for (i32 x = area.clip.x; x < area.clip.x + area.clip.width; ++x) {
            const double normalized_x =
                (static_cast<double>(x - area.viewport.x) + 0.5) /
                static_cast<double>(area.viewport.width);
            const i64 source_x = static_cast<i64>(std::floor(
                static_cast<double>(*crop_x) +
                normalized_x * static_cast<double>(*crop_width)));
            const auto sampled_x = background_coordinate(
                source_x, texture->width, *mode_x);
            if (!sampled_x.has_value()) continue;
            const usize source_index =
                static_cast<usize>(*sampled_y) *
                    static_cast<usize>(texture->width) +
                static_cast<usize>(*sampled_x);
            const usize destination_index =
                static_cast<usize>(y) *
                    static_cast<usize>(target.image->width()) +
                static_cast<usize>(x);
            const graphics::Pixel source = texture->pixels[source_index];
            pixels[destination_index] = graphics::alpha(source) == 255U
                ? source
                : graphics::source_over(source, pixels[destination_index]);
        }
      }
    };
    if (background_width * background_height >= kParallelM3gRasterPixels &&
        background_height >= 4U) {
        const usize rows_per_chunk = std::max<usize>(
            1U, 2U * 1024U / std::max<usize>(background_width, 1U));
        runtime::shared_compute_executor().parallel_for(
            background_height, 4U, rows_per_chunk, draw_rows);
    } else {
        draw_rows(0U, background_height);
    }
    target.image->mark_dirty_region(area.clip.x, area.clip.y,
                                    area.clip.width, area.clip.height);
    return {};
}

[[nodiscard]] Status clear_background(Machine& machine,
                                      ObjectRef graphics3d,
                                      ObjectRef background) {
    auto target = bound_render_target(machine, graphics3d);
    if (!target) return std::unexpected(target.error());
    auto area = render_area(machine, graphics3d, *target);
    if (!area) return std::unexpected(area.error());
    if (graphics::empty(area->clip)) return {};

    bool clear_color = true;
    bool clear_depth = true;
    graphics::Pixel color = 0xFF000000U;
    if (!background.is_null()) {
        static constexpr std::array<ObjectFieldSpec, 3U> kFields {{
            {"color", "I"},
            {"colorClear", "Z"},
            {"depthClear", "Z"},
        }};
        auto fields = object_fields(machine, background, kBackground, kFields);
        if (!fields) return std::unexpected(fields.error());
        auto color_value = (*fields)[0U].as_int();
        auto color_clear = (*fields)[1U].as_int();
        auto depth_clear = (*fields)[2U].as_int();
        if (!color_value) return std::unexpected(color_value.error());
        if (!color_clear) return std::unexpected(color_clear.error());
        if (!depth_clear) return std::unexpected(depth_clear.error());
        color = opaque_m3g_color(*color_value);
        clear_color = *color_clear != 0;
        clear_depth = *depth_clear != 0;
    }
    if (clear_color && target->context.rendering_enabled) {
        auto pixels = target->image->mutable_pixels();
        for (i32 y = area->clip.y;
             y < area->clip.y + area->clip.height; ++y) {
            const usize offset = static_cast<usize>(y) *
                                 static_cast<usize>(target->image->width()) +
                                 static_cast<usize>(area->clip.x);
            std::fill_n(pixels.begin() + static_cast<std::ptrdiff_t>(offset),
                        static_cast<usize>(area->clip.width), color);
        }
        target->image->mark_dirty_region(
            area->clip.x, area->clip.y,
            area->clip.width, area->clip.height);
    }
    if (clear_color && !background.is_null() &&
        target->context.rendering_enabled) {
        auto drawn = draw_background_image(
            machine, background, *target, *area);
        if (!drawn) return drawn;
    }
    if (clear_depth) {
        std::scoped_lock lock(g_depth_mutex);
        auto& depth = depth_surface(machine, graphics3d,
                                    target->image->width(),
                                    target->image->height());
        for (i32 y = area->clip.y;
             y < area->clip.y + area->clip.height; ++y) {
            const usize offset = static_cast<usize>(y) *
                                 static_cast<usize>(depth.width) +
                                 static_cast<usize>(area->clip.x);
            std::fill_n(depth.values.begin() +
                            static_cast<std::ptrdiff_t>(offset),
                        static_cast<usize>(area->clip.width),
                        std::numeric_limits<float>::infinity());
        }
    }
    return {};
}

[[nodiscard]] Status render_sprite(
    Machine& machine,
    ObjectRef graphics3d,
    ObjectRef sprite,
    const Matrix& model,
    float node_alpha) {
    auto target = bound_render_target(machine, graphics3d);
    if (!target) return std::unexpected(target.error());
    if (!target->context.rendering_enabled) return {};
    auto area = render_area(machine, graphics3d, *target);
    if (!area) return std::unexpected(area.error());
    if (graphics::empty(area->clip)) return {};

    static constexpr std::array<ObjectFieldSpec, 9U> kSpriteFields {{
        {"image", "Ljavax/microedition/m3g/Image2D;"},
        {"appearance", "Ljavax/microedition/m3g/Appearance;"},
        {"scaled", "Z"},
        {"cropX", "I"},
        {"cropY", "I"},
        {"cropWidth", "I"},
        {"cropHeight", "I"},
        {"flipX", "Z"},
        {"flipY", "Z"},
    }};
    auto sprite_fields = object_fields(
        machine, sprite, kSprite3D, kSpriteFields);
    if (!sprite_fields) return std::unexpected(sprite_fields.error());
    auto image = (*sprite_fields)[0U].as_reference();
    auto appearance = (*sprite_fields)[1U].as_reference();
    auto scaled = (*sprite_fields)[2U].as_int();
    auto crop_x = (*sprite_fields)[3U].as_int();
    auto crop_y = (*sprite_fields)[4U].as_int();
    auto crop_width = (*sprite_fields)[5U].as_int();
    auto crop_height = (*sprite_fields)[6U].as_int();
    auto flip_x = (*sprite_fields)[7U].as_int();
    auto flip_y = (*sprite_fields)[8U].as_int();
    if (!image) return std::unexpected(image.error());
    if (!appearance) return std::unexpected(appearance.error());
    if (!scaled) return std::unexpected(scaled.error());
    if (!crop_x) return std::unexpected(crop_x.error());
    if (!crop_y) return std::unexpected(crop_y.error());
    if (!crop_width) return std::unexpected(crop_width.error());
    if (!crop_height) return std::unexpected(crop_height.error());
    if (!flip_x) return std::unexpected(flip_x.error());
    if (!flip_y) return std::unexpected(flip_y.error());
    if (image->is_null() || *crop_width <= 0 || *crop_height <= 0) {
        return {};
    }
    auto texture = image2d_texture_data(machine, *image);
    if (!texture) return std::unexpected(texture.error());
    if (texture->width <= 0 || texture->height <= 0 ||
        texture->pixels.empty()) {
        return {};
    }
    const i64 crop_right = static_cast<i64>(*crop_x) + *crop_width;
    const i64 crop_bottom = static_cast<i64>(*crop_y) + *crop_height;
    const i32 source_left = std::max(*crop_x, 0);
    const i32 source_top = std::max(*crop_y, 0);
    const i32 source_right = static_cast<i32>(std::min<i64>(
        crop_right, texture->width));
    const i32 source_bottom = static_cast<i32>(std::min<i64>(
        crop_bottom, texture->height));
    const i32 source_width = source_right - source_left;
    const i32 source_height = source_bottom - source_top;
    if (source_width <= 0 || source_height <= 0) return {};

    static constexpr std::array<ObjectFieldSpec, 4U> kGraphicsFields {{
        {"camera", "Ljavax/microedition/m3g/Camera;"},
        {"cameraTransform", "Ljavax/microedition/m3g/Transform;"},
        {"depthNear", "F"},
        {"depthFar", "F"},
    }};
    auto graphics_fields = object_fields(
        machine, graphics3d, kGraphics3D, kGraphicsFields);
    if (!graphics_fields) return std::unexpected(graphics_fields.error());
    auto camera = (*graphics_fields)[0U].as_reference();
    auto camera_transform = (*graphics_fields)[1U].as_reference();
    auto near_range = (*graphics_fields)[2U].as_float();
    auto far_range = (*graphics_fields)[3U].as_float();
    if (!camera) return std::unexpected(camera.error());
    if (!camera_transform) return std::unexpected(camera_transform.error());
    if (!near_range) return std::unexpected(near_range.error());
    if (!far_range) return std::unexpected(far_range.error());
    if (camera->is_null()) {
        return fail_java("java/lang/IllegalStateException",
                         "Graphics3D has no active camera");
    }
    auto projection = camera_projection(machine, *camera);
    if (!projection) return std::unexpected(projection.error());
    Matrix camera_matrix = identity_matrix();
    if (!camera_transform->is_null()) {
        auto loaded = transform_matrix(machine, *camera_transform);
        if (!loaded) return std::unexpected(loaded.error());
        camera_matrix = *loaded;
    }
    auto view = inverse_matrix(camera_matrix);
    if (!view) return std::unexpected(view.error());
    const Matrix mvp = multiply(*projection, multiply(*view, model));
    const auto project = [&](float x, float y, float z)
        -> std::optional<ScreenVertex> {
        const auto clip = transform_point(mvp, x, y, z);
        if (!std::isfinite(clip[3U]) || clip[3U] <= 1.0e-6F) {
            return std::nullopt;
        }
        const float inverse_w = 1.0F / clip[3U];
        const float ndc_x = clip[0U] * inverse_w;
        const float ndc_y = clip[1U] * inverse_w;
        const float ndc_z = clip[2U] * inverse_w;
        if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y) ||
            !std::isfinite(ndc_z)) {
            return std::nullopt;
        }
        return ScreenVertex {
            .x = static_cast<float>(area->viewport.x) +
                 (ndc_x * 0.5F + 0.5F) *
                     static_cast<float>(area->viewport.width),
            .y = static_cast<float>(area->viewport.y) +
                 (0.5F - ndc_y * 0.5F) *
                     static_cast<float>(area->viewport.height),
            .depth = *near_range + (ndc_z * 0.5F + 0.5F) *
                     (*far_range - *near_range),
            .inverse_w = inverse_w,
            .visible = true,
        };
    };
    const auto center = project(0.0F, 0.0F, 0.0F);
    if (!center.has_value() || center->depth < 0.0F || center->depth > 1.0F) {
        return {};
    }

    float full_width = static_cast<float>(*crop_width);
    float full_height = static_cast<float>(*crop_height);
    if (*scaled != 0) {
        const auto half_x = project(0.5F, 0.0F, 0.0F);
        const auto half_y = project(0.0F, 0.5F, 0.0F);
        if (!half_x.has_value() || !half_y.has_value()) return {};
        full_width = 2.0F * std::hypot(
            half_x->x - center->x, half_x->y - center->y);
        full_height = 2.0F * std::hypot(
            half_y->x - center->x, half_y->y - center->y);
    }
    if (!(full_width > 0.0F) || !(full_height > 0.0F) ||
        !std::isfinite(full_width) || !std::isfinite(full_height)) {
        return {};
    }
    const float pixels_per_source_x =
        full_width / static_cast<float>(*crop_width);
    const float pixels_per_source_y =
        full_height / static_cast<float>(*crop_height);
    const float crop_center_x =
        static_cast<float>(*crop_x) + static_cast<float>(*crop_width) * 0.5F;
    const float crop_center_y =
        static_cast<float>(*crop_y) + static_cast<float>(*crop_height) * 0.5F;
    const float source_center_x =
        static_cast<float>(source_left + source_right) * 0.5F;
    const float source_center_y =
        static_cast<float>(source_top + source_bottom) * 0.5F;
    const float draw_center_x = center->x +
        (source_center_x - crop_center_x) * pixels_per_source_x;
    const float draw_center_y = center->y +
        (source_center_y - crop_center_y) * pixels_per_source_y;
    const float draw_width = static_cast<float>(source_width) *
                             pixels_per_source_x;
    const float draw_height = static_cast<float>(source_height) *
                              pixels_per_source_y;
    const i32 minimum_x = std::max(
        area->clip.x,
        static_cast<i32>(std::floor(draw_center_x - draw_width * 0.5F)));
    const i32 maximum_x = std::min(
        area->clip.x + area->clip.width - 1,
        static_cast<i32>(std::ceil(draw_center_x + draw_width * 0.5F)) - 1);
    const i32 minimum_y = std::max(
        area->clip.y,
        static_cast<i32>(std::floor(draw_center_y - draw_height * 0.5F)));
    const i32 maximum_y = std::min(
        area->clip.y + area->clip.height - 1,
        static_cast<i32>(std::ceil(draw_center_y + draw_height * 0.5F)) - 1);
    if (minimum_x > maximum_x || minimum_y > maximum_y) return {};

    ObjectRef sprite_fog {};
    ObjectRef sprite_polygon {};
    ObjectRef sprite_compositing {};
    if (!appearance->is_null()) {
        static constexpr std::array<ObjectFieldSpec, 3U> kAppearanceFields {{
            {"fog", "Ljavax/microedition/m3g/Fog;"},
            {"polygonMode", "Ljavax/microedition/m3g/PolygonMode;"},
            {"compositingMode", "Ljavax/microedition/m3g/CompositingMode;"},
        }};
        auto appearance_fields = object_fields(
            machine, *appearance, kAppearance, kAppearanceFields);
        if (!appearance_fields) {
            return std::unexpected(appearance_fields.error());
        }
        auto fog_value = (*appearance_fields)[0U].as_reference();
        auto polygon_value = (*appearance_fields)[1U].as_reference();
        auto compositing_value = (*appearance_fields)[2U].as_reference();
        if (!fog_value) return std::unexpected(fog_value.error());
        if (!polygon_value) return std::unexpected(polygon_value.error());
        if (!compositing_value) {
            return std::unexpected(compositing_value.error());
        }
        sprite_fog = *fog_value;
        sprite_polygon = *polygon_value;
        sprite_compositing = *compositing_value;
    }
    auto state = appearance_render_state(
        machine, sprite_polygon, sprite_compositing);
    auto fog = appearance_fog(machine, sprite_fog);
    if (!state) return std::unexpected(state.error());
    if (!fog) return std::unexpected(fog.error());
    auto depth_enabled = int_field(machine, graphics3d, kGraphics3D,
                                   "depthBuffer", "Z");
    if (!depth_enabled) return std::unexpected(depth_enabled.error());
    if (*depth_enabled == 0) {
        state->depth_test = false;
        state->depth_write = false;
    }
    const auto world_center = transform_point(model, 0.0F, 0.0F, 0.0F);
    const auto view_center = transform_point(
        *view, world_center[0U], world_center[1U], world_center[2U],
        world_center[3U]);
    const float fog_distance = std::abs(view_center[2U]);
    const float sprite_depth = center->depth +
        state->depth_offset_units * (1.0F / 16'777'216.0F);

    std::scoped_lock lock(g_depth_mutex);
    auto& depth = depth_surface(machine, graphics3d,
                                target->image->width(),
                                target->image->height());
    auto pixels = target->image->mutable_pixels();
    const float left = draw_center_x - draw_width * 0.5F;
    const float top = draw_center_y - draw_height * 0.5F;
    node_alpha = std::clamp(node_alpha, 0.0F, 1.0F);
    const usize sprite_width = static_cast<usize>(maximum_x - minimum_x + 1);
    const usize sprite_height = static_cast<usize>(maximum_y - minimum_y + 1);
    const auto draw_rows = [&](usize row_begin, usize row_end) {
      for (usize row = row_begin; row < row_end; ++row) {
        const i32 y = minimum_y + static_cast<i32>(row);
        const float normalized_y = std::clamp(
            (static_cast<float>(y) + 0.5F - top) / draw_height,
            0.0F, std::nextafter(1.0F, 0.0F));
        i32 source_y = static_cast<i32>(
            normalized_y * static_cast<float>(source_height));
        if (*flip_y != 0) source_y = source_height - 1 - source_y;
        source_y += source_top;
        for (i32 x = minimum_x; x <= maximum_x; ++x) {
            const usize destination_index =
                static_cast<usize>(y) *
                    static_cast<usize>(target->image->width()) +
                static_cast<usize>(x);
            if (state->depth_test &&
                sprite_depth >= depth.values[destination_index]) {
                continue;
            }
            const float normalized_x = std::clamp(
                (static_cast<float>(x) + 0.5F - left) / draw_width,
                0.0F, std::nextafter(1.0F, 0.0F));
            i32 source_x = static_cast<i32>(
                normalized_x * static_cast<float>(source_width));
            if (*flip_x != 0) source_x = source_width - 1 - source_x;
            source_x += source_left;
            const usize source_index =
                static_cast<usize>(source_y) *
                    static_cast<usize>(texture->width) +
                static_cast<usize>(source_x);
            graphics::Pixel source = texture->pixels[source_index];
            const u8 adjusted_alpha = static_cast<u8>(std::clamp(
                std::lround(static_cast<double>(graphics::alpha(source)) *
                            static_cast<double>(node_alpha)),
                0L, 255L));
            source = graphics::argb(
                adjusted_alpha, graphics::red(source),
                graphics::green(source), graphics::blue(source));
            source = color_to_argb(apply_fog(
                color_from_argb(static_cast<i32>(source)),
                *fog, fog_distance));
            if (static_cast<float>(graphics::alpha(source)) / 255.0F <
                state->alpha_threshold) {
                continue;
            }
            pixels[destination_index] = composite_pixel(
                source, pixels[destination_index], *state);
            if (state->depth_write) {
                depth.values[destination_index] = sprite_depth;
            }
        }
      }
    };
    if (sprite_width * sprite_height >= kParallelM3gRasterPixels &&
        sprite_height >= 4U) {
        const usize rows_per_chunk = std::max<usize>(
            1U, 2U * 1024U / std::max<usize>(sprite_width, 1U));
        runtime::shared_compute_executor().parallel_for(
            sprite_height, 4U, rows_per_chunk, draw_rows);
    } else {
        draw_rows(0U, sprite_height);
    }
    target->image->mark_dirty_region(minimum_x, minimum_y,
                                     maximum_x - minimum_x + 1,
                                     maximum_y - minimum_y + 1);
    return {};
}

[[nodiscard]] Result<Matrix> node_local_matrix(Machine& machine,
                                               ObjectRef node) {
    auto transform = local_transform(machine, node);
    if (!transform) return std::unexpected(transform.error());
    return transform_matrix(machine, *transform);
}

[[nodiscard]] Status collect_scene_lights(
    Machine& machine,
    ObjectRef node,
    const Matrix& parent_transform,
    std::vector<SceneLight>& lights,
    usize depth = 0U) {
    if (node.is_null()) return {};
    if (depth >= 1'024U) {
        return fail(ErrorCode::overflow, "M3G scene graph is too deep");
    }
    auto class_name = machine.heap().class_name(node);
    if (!class_name) return std::unexpected(class_name.error());
    auto rendering = int_field(machine, node, kNode,
                               "renderingEnabled", "Z");
    if (!rendering) return std::unexpected(rendering.error());
    if (*rendering == 0) return {};
    auto local = node_local_matrix(machine, node);
    if (!local) return std::unexpected(local.error());
    const Matrix model = multiply(parent_transform, *local);
    if (*class_name == kLight) {
        lights.push_back(SceneLight {.light = node, .transform = model});
    }
    if (*class_name == kGroup || *class_name == kWorld) {
        static constexpr std::array<ObjectFieldSpec, 2U> kGroupFields {{
            {"children", "[Ljavax/microedition/m3g/Node;"},
            {"childCount", "I"},
        }};
        auto group_fields = object_fields(
            machine, node, kGroup, kGroupFields);
        if (!group_fields) return std::unexpected(group_fields.error());
        auto children = (*group_fields)[0U].as_reference();
        auto count = (*group_fields)[1U].as_int();
        if (!children) return std::unexpected(children.error());
        if (!count) return std::unexpected(count.error());
        if (children->is_null()) return {};
        auto child_values = read_reference_array(
            machine, *children, "Group children");
        if (!child_values) return std::unexpected(child_values.error());
        const usize child_count = std::min(
            child_values->size(), static_cast<usize>(std::max(*count, 0)));
        for (usize index = 0U; index < child_count; ++index) {
            auto collected = collect_scene_lights(
                machine, (*child_values)[index], model, lights, depth + 1U);
            if (!collected) return collected;
        }
    }
    return {};
}

[[nodiscard]] Status render_scene_node(Machine& machine,
                                       ObjectRef graphics3d,
                                       ObjectRef node,
                                       const Matrix& parent_transform,
                                       i32 render_scope,
                                       float inherited_alpha = 1.0F,
                                       const std::vector<SceneLight>* scene_lights = nullptr,
                                       usize depth = 0U) {
    if (node.is_null()) return {};
    if (depth >= 1'024U) {
        return fail(ErrorCode::overflow, "M3G scene graph is too deep");
    }
    auto class_name = machine.heap().class_name(node);
    if (!class_name) return std::unexpected(class_name.error());
    static constexpr std::array<ObjectFieldSpec, 3U> kNodeFields {{
        {"renderingEnabled", "Z"},
        {"scope", "I"},
        {"alphaFactor", "F"},
    }};
    auto node_fields = object_fields(machine, node, kNode, kNodeFields);
    if (!node_fields) return std::unexpected(node_fields.error());
    auto rendering = (*node_fields)[0U].as_int();
    auto scope = (*node_fields)[1U].as_int();
    auto alpha = (*node_fields)[2U].as_float();
    if (!rendering) return std::unexpected(rendering.error());
    if (!scope) return std::unexpected(scope.error());
    if (!alpha) return std::unexpected(alpha.error());
    if (*rendering == 0 || (render_scope != -1 &&
        (static_cast<u32>(*scope) & static_cast<u32>(render_scope)) == 0U)) {
        return {};
    }
    auto local = node_local_matrix(machine, node);
    if (!local) return std::unexpected(local.error());
    const Matrix model = multiply(parent_transform, *local);
    const float combined_alpha = std::clamp(
        inherited_alpha * *alpha, 0.0F, 1.0F);

    if (*class_name == kSprite3D) {
        auto rendered = render_sprite(
            machine, graphics3d, node, model, combined_alpha);
        if (!rendered) return rendered;
    }

    const bool mesh = *class_name == kMesh ||
        *class_name == "javax/microedition/m3g/MorphingMesh" ||
        *class_name == "javax/microedition/m3g/SkinnedMesh";
    if (mesh) {
        static constexpr std::array<ObjectFieldSpec, 3U> kMeshFields {{
            {"vertexBuffer", "Ljavax/microedition/m3g/VertexBuffer;"},
            {"indexBuffers", "[Ljavax/microedition/m3g/IndexBuffer;"},
            {"appearances", "[Ljavax/microedition/m3g/Appearance;"},
        }};
        auto mesh_fields = object_fields(machine, node, kMesh, kMeshFields);
        if (!mesh_fields) return std::unexpected(mesh_fields.error());
        auto vertex_buffer = (*mesh_fields)[0U].as_reference();
        auto index_buffers = (*mesh_fields)[1U].as_reference();
        auto appearances = (*mesh_fields)[2U].as_reference();
        if (!vertex_buffer) return std::unexpected(vertex_buffer.error());
        if (!index_buffers) return std::unexpected(index_buffers.error());
        if (!appearances) return std::unexpected(appearances.error());
        if (vertex_buffer->is_null() || index_buffers->is_null()) return {};
        std::optional<DeformedGeometry> deformed;
        if (*class_name == "javax/microedition/m3g/MorphingMesh") {
            auto loaded = morphing_geometry(machine, node, *vertex_buffer);
            if (!loaded) return std::unexpected(loaded.error());
            deformed = std::move(*loaded);
        } else if (*class_name == "javax/microedition/m3g/SkinnedMesh") {
            auto loaded = skinned_geometry(machine, node, *vertex_buffer);
            if (!loaded) return std::unexpected(loaded.error());
            deformed = std::move(*loaded);
        }
        auto index_values = read_reference_array(
            machine, *index_buffers, "Mesh index buffers");
        if (!index_values) return std::unexpected(index_values.error());
        std::vector<ObjectRef> appearance_values;
        if (!appearances->is_null()) {
            auto loaded = read_reference_array(
                machine, *appearances, "Mesh appearances");
            if (!loaded) return std::unexpected(loaded.error());
            appearance_values = std::move(*loaded);
        }
        auto transform = snapshot_matrix(machine, model);
        if (!transform) return std::unexpected(transform.error());
        auto transform_root = machine.pin_native_root(*transform);
        if (!transform_root) return std::unexpected(transform_root.error());
        for (usize index = 0U; index < index_values->size(); ++index) {
            const ObjectRef index_buffer = (*index_values)[index];
            if (index_buffer.is_null()) continue;
            ObjectRef appearance {};
            if (index < appearance_values.size()) {
                appearance = appearance_values[index];
            }
            const std::vector<Position>* deformed_positions =
                deformed.has_value() ? &deformed->positions : nullptr;
            const TextureCoordinateSets* deformed_texture =
                deformed.has_value() ? &deformed->texture_coordinates : nullptr;
            const std::vector<Vector3>* deformed_normals =
                deformed.has_value() ? &deformed->normals : nullptr;
            const std::vector<VertexColor>* deformed_colors =
                deformed.has_value() ? &deformed->colors : nullptr;
            auto rendered = render_geometry(
                machine, graphics3d, *vertex_buffer, index_buffer,
                appearance, *transform,
                deformed_positions, deformed_texture,
                deformed_normals, deformed_colors,
                *scope, combined_alpha, scene_lights);
            if (!rendered) return rendered;
        }
    }

    if (*class_name == kGroup || *class_name == kWorld) {
        static constexpr std::array<ObjectFieldSpec, 2U> kGroupFields {{
            {"children", "[Ljavax/microedition/m3g/Node;"},
            {"childCount", "I"},
        }};
        auto group_fields = object_fields(
            machine, node, kGroup, kGroupFields);
        if (!group_fields) return std::unexpected(group_fields.error());
        auto children = (*group_fields)[0U].as_reference();
        auto count = (*group_fields)[1U].as_int();
        if (!children) return std::unexpected(children.error());
        if (!count) return std::unexpected(count.error());
        if (children->is_null()) return {};
        auto child_values = read_reference_array(
            machine, *children, "Group children");
        if (!child_values) return std::unexpected(child_values.error());
        const usize child_count = std::min(
            child_values->size(), static_cast<usize>(std::max(*count, 0)));
        for (usize index = 0U; index < child_count; ++index) {
            auto rendered = render_scene_node(
                machine, graphics3d, (*child_values)[index], model, render_scope,
                combined_alpha, scene_lights, depth + 1U);
            if (!rendered) return rendered;
        }
    }
    return {};
}

[[nodiscard]] Status configure_world_camera(Machine& machine,
                                            ObjectRef graphics3d,
                                            ObjectRef world) {
    auto camera = reference_field(machine, world, kWorld,
        "activeCamera", "Ljavax/microedition/m3g/Camera;");
    if (!camera) return std::unexpected(camera.error());
    if (camera->is_null()) {
        return fail_java("java/lang/IllegalStateException",
                         "World has no active camera");
    }
    auto composite = node_composite_matrix(machine, *camera);
    if (!composite) return std::unexpected(composite.error());
    auto transform = snapshot_matrix(machine, *composite);
    if (!transform) return std::unexpected(transform.error());
    auto camera_stored = set_reference_field(machine, graphics3d, kGraphics3D,
        "camera", "Ljavax/microedition/m3g/Camera;", *camera);
    auto transform_stored = set_reference_field(machine, graphics3d, kGraphics3D,
        "cameraTransform", "Ljavax/microedition/m3g/Transform;", *transform);
    if (!camera_stored) return camera_stored;
    return transform_stored;
}

[[nodiscard]] Result<ObjectRef> graphics3d_reference_array(
    Machine& machine,
    ObjectRef graphics3d,
    std::string_view field,
    std::string_view descriptor) {
    auto array = reference_field(machine, graphics3d, kGraphics3D,
                                 field, descriptor);
    if (!array) return std::unexpected(array.error());
    if (!array->is_null()) return *array;
    auto created = allocate_array(machine, std::string(descriptor), 4U,
                                  Value::from_reference({}));
    if (!created) return std::unexpected(created.error());
    auto stored = set_reference_field(machine, graphics3d, kGraphics3D,
                                      field, descriptor, *created);
    if (!stored) return std::unexpected(stored.error());
    return *created;
}

[[nodiscard]] Status ensure_graphics3d_reference_capacity(
    Machine& machine,
    ObjectRef graphics3d,
    std::string_view field,
    std::string_view descriptor,
    usize minimum) {
    auto array = graphics3d_reference_array(
        machine, graphics3d, field, descriptor);
    if (!array) return std::unexpected(array.error());
    auto length = machine.heap().array_length(*array);
    if (!length) return std::unexpected(length.error());
    if (*length >= minimum) return {};
    const usize capacity = std::max(
        minimum, *length == 0U ? 4U : *length * 2U);
    auto replacement = allocate_array(
        machine, std::string(descriptor), capacity,
                                      Value::from_reference({}));
    if (!replacement) return std::unexpected(replacement.error());
    auto replacement_root = machine.pin_native_root(*replacement);
    if (!replacement_root) return std::unexpected(replacement_root.error());
    if (*length != 0U) {
        auto copied = machine.heap().copy_array_range(
            *array, 0U, *replacement, 0U, *length);
        if (!copied) return copied;
    }
    return set_reference_field(machine, graphics3d, kGraphics3D,
                               field, descriptor, *replacement);
}

[[nodiscard]] Status ensure_light_capacity(Machine& machine,
                                           ObjectRef graphics3d,
                                           usize minimum) {
    auto lights = ensure_graphics3d_reference_capacity(
        machine, graphics3d, "lights",
        "[Ljavax/microedition/m3g/Light;", minimum);
    if (!lights) return lights;
    return ensure_graphics3d_reference_capacity(
        machine, graphics3d, "lightTransforms",
        "[Ljavax/microedition/m3g/Transform;", minimum);
}

[[nodiscard]] Result<ObjectRef> execution_reference(
    Machine& machine,
    const ExecutionResult& result,
    std::string_view operation);

[[nodiscard]] Result<ObjectRef> create_ascii_string(
    Machine& machine,
    std::string_view text);

[[nodiscard]] Result<ObjectRef> box_graphics3d_boolean(
    Machine& machine,
    bool value) {
    const Value argument = Value::from_int(value ? 1 : 0);
    auto boxed = machine.invoke_static(
        "java/lang/Boolean", "valueOf", "(Z)Ljava/lang/Boolean;",
        std::span<const Value>(&argument, 1U));
    if (!boxed) return std::unexpected(boxed.error());
    return execution_reference(machine, *boxed,
                               "Graphics3D Boolean property boxing");
}

[[nodiscard]] Result<ObjectRef> box_graphics3d_integer(
    Machine& machine,
    i32 value) {
    const Value argument = Value::from_int(value);
    auto boxed = machine.invoke_static(
        "java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;",
        std::span<const Value>(&argument, 1U));
    if (!boxed) return std::unexpected(boxed.error());
    return execution_reference(machine, *boxed,
                               "Graphics3D Integer property boxing");
}

[[nodiscard]] Status put_graphics3d_property(
    Machine& machine,
    ObjectRef table,
    std::string_view key_text,
    ObjectRef value) {
    auto key = create_ascii_string(machine, key_text);
    if (!key) return std::unexpected(key.error());
    auto key_root = machine.pin_native_root(*key);
    auto value_root = machine.pin_native_root(value);
    if (!key_root) return std::unexpected(key_root.error());
    if (!value_root) return std::unexpected(value_root.error());
    const std::array<Value, 2> arguments {
        Value::from_reference(*key), Value::from_reference(value),
    };
    auto inserted = machine.invoke_instance(
        table, "java/util/Hashtable", "put",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
        arguments);
    if (!inserted) return std::unexpected(inserted.error());
    if (!inserted->completed_normally()) {
        return fail(ErrorCode::java_exception,
                    "Graphics3D property insertion failed");
    }
    return {};
}

[[nodiscard]] Result<ObjectRef> graphics3d_properties(Machine& machine) {
    auto table = allocate_instance(machine, "java/util/Hashtable");
    if (!table) return std::unexpected(table.error());
    auto table_root = machine.pin_native_root(*table);
    if (!table_root) return std::unexpected(table_root.error());
    auto initialized = machine.invoke_instance(
        *table, "java/util/Hashtable", "<init>", "()V");
    if (!initialized) return std::unexpected(initialized.error());
    if (!initialized->completed_normally()) {
        return fail(ErrorCode::java_exception,
                    "Graphics3D properties table initialization failed");
    }
    const std::array<std::pair<std::string_view, bool>, 6> booleans {{
        {"supportAntialiasing", false},
        {"supportTrueColor", true},
        {"supportDithering", false},
        {"supportMipmapping", false},
        {"supportPerspectiveCorrection", true},
        {"supportLocalCameraLighting", false},
    }};
    for (const auto& [key, value] : booleans) {
        auto boxed = box_graphics3d_boolean(machine, value);
        if (!boxed) return std::unexpected(boxed.error());
        auto stored = put_graphics3d_property(machine, *table, key, *boxed);
        if (!stored) return std::unexpected(stored.error());
    }
    const std::array<std::pair<std::string_view, i32>, 8> integers {{
        {"maxLights", 8}, {"maxViewportWidth", 4096},
        {"maxViewportHeight", 4096}, {"maxViewportDimension", 4096},
        {"maxTextureDimension", 4096},
        {"maxSpriteCropDimension", 4096},
        {"numTextureUnits", 8}, {"maxTransformsPerVertex", 4},
    }};
    for (const auto& [key, value] : integers) {
        auto boxed = box_graphics3d_integer(machine, value);
        if (!boxed) return std::unexpected(boxed.error());
        auto stored = put_graphics3d_property(machine, *table, key, *boxed);
        if (!stored) return std::unexpected(stored.error());
    }
    auto release = create_ascii_string(machine, "phoneME-iOS");
    if (!release) return std::unexpected(release.error());
    auto stored = put_graphics3d_property(
        machine, *table, "m3gRelease", *release);
    if (!stored) return std::unexpected(stored.error());
    return *table;
}

[[nodiscard]] Status require_bound_target(Machine& machine,
                                          ObjectRef graphics3d) {
    auto target = reference_field(machine, graphics3d, kGraphics3D,
                                  "target", "Ljava/lang/Object;");
    if (!target) return std::unexpected(target.error());
    if (target->is_null()) {
        return fail_java("java/lang/IllegalStateException",
                         "Graphics3D has no bound target");
    }
    return {};
}

void register_graphics3d(NativeMethodRegistry& registry) {
    register_noop_constructor(registry, kGraphics3D, "()V");
    add(registry, kGraphics3D, "getInstance",
        "()Ljavax/microedition/m3g/Graphics3D;",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            auto location = field_location(machine, kGraphics3D, "INSTANCE",
                                           "Ljavax/microedition/m3g/Graphics3D;",
                                           true);
            if (!location) return std::unexpected(location.error());
            auto current = machine.class_states().static_field(*location);
            if (!current) return std::unexpected(current.error());
            auto instance = current->as_reference();
            if (!instance) return std::unexpected(instance.error());
            if (instance->is_null()) {
                auto allocated = allocate_instance(machine, kGraphics3D);
                if (!allocated) return std::unexpected(allocated.error());
                const Dimensions dimensions = machine.canvas_bridge() != nullptr
                    ? machine.canvas_bridge()->display_dimensions()
                    : Dimensions {320, 240};
                auto width = set_int_field(machine, *allocated, kGraphics3D,
                                           "viewportWidth", dimensions.width);
                auto height = set_int_field(machine, *allocated, kGraphics3D,
                                            "viewportHeight", dimensions.height);
                auto near_value = set_float_field(machine, *allocated, kGraphics3D,
                                                  "depthNear", 0.0F);
                auto far_value = set_float_field(machine, *allocated, kGraphics3D,
                                                 "depthFar", 1.0F);
                if (!width) return std::unexpected(width.error());
                if (!height) return std::unexpected(height.error());
                if (!near_value) return std::unexpected(near_value.error());
                if (!far_value) return std::unexpected(far_value.error());
                auto stored = machine.class_states().set_static_field(
                    *location, Value::from_reference(*allocated));
                if (!stored) return std::unexpected(stored.error());
                instance = *allocated;
            }
            return std::optional<Value>(Value::from_reference(*instance));
        });
    add(registry, kGraphics3D, "getProperties", "()Ljava/util/Hashtable;",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            auto table = graphics3d_properties(machine);
            if (!table) return std::unexpected(table.error());
            return std::optional<Value>(Value::from_reference(*table));
        });

    const auto register_bind = [&registry](const char* descriptor) {
        add(registry, kGraphics3D, "bindTarget", descriptor,
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, "Graphics3D.bindTarget");
                auto target = reference_argument(arguments, 1U,
                                                "Graphics3D.bindTarget", false);
                if (!object) return std::unexpected(object.error());
                if (!target) return std::unexpected(target.error());
                auto current = reference_field(machine, *object, kGraphics3D,
                                               "target", "Ljava/lang/Object;");
                if (!current) return std::unexpected(current.error());
                if (!current->is_null()) {
                    return fail_java("java/lang/IllegalStateException",
                                     "Graphics3D target is already bound");
                }
                i32 depth = 1;
                i32 hints = 0;
                if (arguments.size() >= 4U) {
                    auto parsed_depth = int_argument(
                        arguments, 2U, "Graphics3D.bindTarget");
                    auto parsed_hints = int_argument(
                        arguments, 3U, "Graphics3D.bindTarget");
                    if (!parsed_depth) {
                        return std::unexpected(parsed_depth.error());
                    }
                    if (!parsed_hints) {
                        return std::unexpected(parsed_hints.error());
                    }
                    depth = *parsed_depth != 0 ? 1 : 0;
                    hints = *parsed_hints;
                }
                if ((hints & ~0x1E) != 0) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Graphics3D hints contain invalid bits");
                }
                const std::array<Status, 3> stored {
                    set_reference_field(machine, *object, kGraphics3D,
                        "target", "Ljava/lang/Object;", *target),
                    set_int_field(machine, *object, kGraphics3D,
                                  "depthBuffer", depth, "Z"),
                    set_int_field(machine, *object, kGraphics3D,
                                  "hints", hints),
                };
                for (const Status& status : stored) {
                    if (!status) return std::unexpected(status.error());
                }
                return std::optional<Value> {};
            });
    };
    register_bind("(Ljava/lang/Object;)V");
    register_bind("(Ljava/lang/Object;ZI)V");
    add(registry, kGraphics3D, "releaseTarget", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.releaseTarget");
            if (!object) return std::unexpected(object.error());
            erase_depth_surface(machine, *object);
            auto stored = set_reference_field(machine, *object, kGraphics3D,
                                              "target", "Ljava/lang/Object;", {});
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, kGraphics3D, "getTarget", "()Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.getTarget");
            if (!object) return std::unexpected(object.error());
            auto target = reference_field(machine, *object, kGraphics3D,
                                          "target", "Ljava/lang/Object;");
            if (!target) return std::unexpected(target.error());
            return std::optional<Value>(Value::from_reference(*target));
        });
    add(registry, kGraphics3D, "getHints", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.getHints");
            if (!object) return std::unexpected(object.error());
            auto hints = int_field(machine, *object, kGraphics3D, "hints");
            if (!hints) return std::unexpected(hints.error());
            return std::optional<Value>(Value::from_int(*hints));
        });
    add(registry, kGraphics3D, "isDepthBufferEnabled", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(
                arguments, "Graphics3D.isDepthBufferEnabled");
            if (!object) return std::unexpected(object.error());
            auto enabled = int_field(machine, *object, kGraphics3D,
                                     "depthBuffer", "Z");
            if (!enabled) return std::unexpected(enabled.error());
            return std::optional<Value>(Value::from_int(*enabled != 0));
        });
    add(registry, kGraphics3D, "clear",
        "(Ljavax/microedition/m3g/Background;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.clear");
            auto background = reference_argument(
                arguments, 1U, "Graphics3D.clear", true);
            if (!object) return std::unexpected(object.error());
            if (!background) return std::unexpected(background.error());
            auto cleared = clear_background(machine, *object, *background);
            if (!cleared) return std::unexpected(cleared.error());
            return std::optional<Value> {};
        });

    add(registry, kGraphics3D, "render",
        "(Ljavax/microedition/m3g/World;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.render(World)");
            auto world = reference_argument(
                arguments, 1U, "Graphics3D.render(World)", false);
            if (!object) return std::unexpected(object.error());
            if (!world) return std::unexpected(world.error());
            auto bound = require_bound_target(machine, *object);
            if (!bound) return std::unexpected(bound.error());
            auto background = reference_field(machine, *world, kWorld,
                "background", "Ljavax/microedition/m3g/Background;");
            if (!background) return std::unexpected(background.error());
            auto cleared = clear_background(machine, *object, *background);
            if (!cleared) return std::unexpected(cleared.error());
            auto camera = configure_world_camera(machine, *object, *world);
            if (!camera) return std::unexpected(camera.error());
            std::vector<SceneLight> scene_lights;
            auto collected = collect_scene_lights(
                machine, *world, identity_matrix(), scene_lights);
            if (!collected) return std::unexpected(collected.error());
            auto rendered = render_scene_node(
                machine, *object, *world, identity_matrix(), -1,
                1.0F, &scene_lights);
            if (!rendered) return std::unexpected(rendered.error());
            return std::optional<Value> {};
        });
    add(registry, kGraphics3D, "render",
        "(Ljavax/microedition/m3g/Node;"
        "Ljavax/microedition/m3g/Transform;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.render(Node)");
            auto node = reference_argument(
                arguments, 1U, "Graphics3D.render(Node)", false);
            auto transform = reference_argument(
                arguments, 2U, "Graphics3D.render(Node)", true);
            if (!object) return std::unexpected(object.error());
            if (!node) return std::unexpected(node.error());
            if (!transform) return std::unexpected(transform.error());
            auto bound = require_bound_target(machine, *object);
            if (!bound) return std::unexpected(bound.error());
            Matrix base = identity_matrix();
            if (!transform->is_null()) {
                auto loaded = transform_matrix(machine, *transform);
                if (!loaded) return std::unexpected(loaded.error());
                base = *loaded;
            }
            auto rendered = render_scene_node(
                machine, *object, *node, base, -1);
            if (!rendered) return std::unexpected(rendered.error());
            return std::optional<Value> {};
        });

    const auto register_geometry_render = [&registry](const char* descriptor) {
        add(registry, kGraphics3D, "render", descriptor,
            [](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, "Graphics3D.render");
                auto vertices = reference_argument(
                    arguments, 1U, "Graphics3D.render", false);
                auto indices = reference_argument(
                    arguments, 2U, "Graphics3D.render", false);
                auto appearance = reference_argument(
                    arguments, 3U, "Graphics3D.render", true);
                auto transform = reference_argument(
                    arguments, 4U, "Graphics3D.render", true);
                if (!object) return std::unexpected(object.error());
                if (!vertices) return std::unexpected(vertices.error());
                if (!indices) return std::unexpected(indices.error());
                if (!appearance) return std::unexpected(appearance.error());
                if (!transform) return std::unexpected(transform.error());
                i32 scope = -1;
                if (arguments.size() >= 6U) {
                    auto parsed_scope = int_argument(
                        arguments, 5U, "Graphics3D.render");
                    if (!parsed_scope) {
                        return std::unexpected(parsed_scope.error());
                    }
                    scope = *parsed_scope;
                }
                auto rendered = render_geometry(
                    machine, *object, *vertices, *indices,
                    *appearance, *transform,
                    nullptr, nullptr, nullptr, nullptr, scope);
                if (!rendered) return std::unexpected(rendered.error());
                return std::optional<Value> {};
            });
    };
    register_geometry_render(
        "(Ljavax/microedition/m3g/VertexBuffer;"
        "Ljavax/microedition/m3g/IndexBuffer;"
        "Ljavax/microedition/m3g/Appearance;"
        "Ljavax/microedition/m3g/Transform;)V");
    register_geometry_render(
        "(Ljavax/microedition/m3g/VertexBuffer;"
        "Ljavax/microedition/m3g/IndexBuffer;"
        "Ljavax/microedition/m3g/Appearance;"
        "Ljavax/microedition/m3g/Transform;I)V");

    add(registry, kGraphics3D, "setCamera",
        "(Ljavax/microedition/m3g/Camera;"
        "Ljavax/microedition/m3g/Transform;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.setCamera");
            auto camera = reference_argument(arguments, 1U,
                                             "Graphics3D.setCamera", true);
            auto transform = reference_argument(arguments, 2U,
                                                "Graphics3D.setCamera", true);
            if (!object) return std::unexpected(object.error());
            if (!camera) return std::unexpected(camera.error());
            if (!transform) return std::unexpected(transform.error());
            auto transform_snapshot = snapshot_transform(machine, *transform);
            if (!transform_snapshot) {
                return std::unexpected(transform_snapshot.error());
            }
            auto camera_stored = set_reference_field(machine, *object,
                kGraphics3D, "camera",
                "Ljavax/microedition/m3g/Camera;", *camera);
            auto transform_stored = set_reference_field(machine, *object,
                kGraphics3D, "cameraTransform",
                "Ljavax/microedition/m3g/Transform;", *transform_snapshot);
            if (!camera_stored) return std::unexpected(camera_stored.error());
            if (!transform_stored) {
                return std::unexpected(transform_stored.error());
            }
            return std::optional<Value> {};
        });
    add(registry, kGraphics3D, "getCamera",
        "(Ljavax/microedition/m3g/Transform;)"
        "Ljavax/microedition/m3g/Camera;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.getCamera");
            auto destination = reference_argument(arguments, 1U,
                                                  "Graphics3D.getCamera", true);
            if (!object) return std::unexpected(object.error());
            if (!destination) return std::unexpected(destination.error());
            auto camera = reference_field(machine, *object, kGraphics3D,
                "camera", "Ljavax/microedition/m3g/Camera;");
            auto transform = reference_field(machine, *object, kGraphics3D,
                "cameraTransform", "Ljavax/microedition/m3g/Transform;");
            if (!camera) return std::unexpected(camera.error());
            if (!transform) return std::unexpected(transform.error());
            if (!destination->is_null() && !transform->is_null()) {
                auto matrix = transform_matrix(machine, *transform);
                if (!matrix) return std::unexpected(matrix.error());
                auto stored = set_transform_matrix(machine, *destination, *matrix);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value>(Value::from_reference(*camera));
        });

    add(registry, kGraphics3D, "setViewport", "(IIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.setViewport");
            if (!object) return std::unexpected(object.error());
            const std::array<const char*, 4> fields {
                "viewportX", "viewportY", "viewportWidth", "viewportHeight",
            };
            for (usize index = 0; index < fields.size(); ++index) {
                auto value = int_argument(arguments, index + 1U,
                                          "Graphics3D.setViewport");
                if (!value) return std::unexpected(value.error());
                if (index >= 2U && *value <= 0) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "Graphics3D viewport dimensions must be positive");
                }
                auto stored = set_int_field(machine, *object, kGraphics3D,
                                            fields[index], *value);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });
    const std::array<std::pair<const char*, const char*>, 4> viewport_getters {{
        {"viewportX", "getViewportX"},
        {"viewportY", "getViewportY"},
        {"viewportWidth", "getViewportWidth"},
        {"viewportHeight", "getViewportHeight"},
    }};
    for (const auto& [field, getter] : viewport_getters) {
        add(registry, kGraphics3D, getter, "()I",
            [field, getter](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, getter);
                if (!object) return std::unexpected(object.error());
                auto value = int_field(machine, *object, kGraphics3D, field);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_int(*value));
            });
    }
    add(registry, kGraphics3D, "setDepthRange", "(FF)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.setDepthRange");
            auto near_value = float_argument(arguments, 1U,
                                             "Graphics3D.setDepthRange");
            auto far_value = float_argument(arguments, 2U,
                                            "Graphics3D.setDepthRange");
            if (!object) return std::unexpected(object.error());
            if (!near_value) return std::unexpected(near_value.error());
            if (!far_value) return std::unexpected(far_value.error());
            if (*near_value < 0.0F || *far_value > 1.0F ||
                *near_value > *far_value) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Graphics3D depth range is invalid");
            }
            auto first = set_float_field(machine, *object, kGraphics3D,
                                         "depthNear", *near_value);
            auto second = set_float_field(machine, *object, kGraphics3D,
                                          "depthFar", *far_value);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            return std::optional<Value> {};
        });
    for (const auto& [field, getter] :
         std::array<std::pair<const char*, const char*>, 2> {{
             {"depthNear", "getDepthRangeNear"},
             {"depthFar", "getDepthRangeFar"},
         }}) {
        add(registry, kGraphics3D, getter, "()F",
            [field, getter](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, getter);
                if (!object) return std::unexpected(object.error());
                auto value = float_field(machine, *object, kGraphics3D, field);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_float(*value));
            });
    }

    add(registry, kGraphics3D, "getLightCount", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.getLightCount");
            if (!object) return std::unexpected(object.error());
            auto count = int_field(machine, *object, kGraphics3D,
                                   "lightCount");
            if (!count) return std::unexpected(count.error());
            return std::optional<Value>(Value::from_int(*count));
        });
    add(registry, kGraphics3D, "resetLights", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.resetLights");
            if (!object) return std::unexpected(object.error());
            auto count = int_field(machine, *object, kGraphics3D, "lightCount");
            if (!count) return std::unexpected(count.error());
            if (*count > 0) {
                auto lights = graphics3d_reference_array(
                    machine, *object, "lights",
                    "[Ljavax/microedition/m3g/Light;");
                auto transforms = graphics3d_reference_array(
                    machine, *object, "lightTransforms",
                    "[Ljavax/microedition/m3g/Transform;");
                if (!lights) return std::unexpected(lights.error());
                if (!transforms) return std::unexpected(transforms.error());
                for (i32 index = 0; index < *count; ++index) {
                    auto cleared = machine.heap().set_element(
                        *lights, static_cast<usize>(index),
                        Value::from_reference({}));
                    if (!cleared) return std::unexpected(cleared.error());
                    cleared = machine.heap().set_element(
                        *transforms, static_cast<usize>(index),
                        Value::from_reference({}));
                    if (!cleared) return std::unexpected(cleared.error());
                }
            }
            auto stored = set_int_field(machine, *object, kGraphics3D,
                                        "lightCount", 0);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, kGraphics3D, "addLight",
        "(Ljavax/microedition/m3g/Light;"
        "Ljavax/microedition/m3g/Transform;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.addLight");
            auto light = reference_argument(arguments, 1U,
                                            "Graphics3D.addLight", false);
            auto transform = reference_argument(arguments, 2U,
                                                "Graphics3D.addLight", true);
            if (!object) return std::unexpected(object.error());
            if (!light) return std::unexpected(light.error());
            if (!transform) return std::unexpected(transform.error());
            auto count = int_field(machine, *object, kGraphics3D, "lightCount");
            if (!count) return std::unexpected(count.error());
            if (*count < 0 || *count == std::numeric_limits<i32>::max()) {
                return fail_java("java/lang/IllegalStateException",
                                 "Graphics3D light count is invalid");
            }
            auto capacity = ensure_light_capacity(
                machine, *object, static_cast<usize>(*count + 1));
            if (!capacity) return std::unexpected(capacity.error());
            auto lights = graphics3d_reference_array(
                machine, *object, "lights",
                "[Ljavax/microedition/m3g/Light;");
            auto transforms = graphics3d_reference_array(
                machine, *object, "lightTransforms",
                "[Ljavax/microedition/m3g/Transform;");
            if (!lights) return std::unexpected(lights.error());
            if (!transforms) return std::unexpected(transforms.error());
            auto stored = machine.heap().set_element(
                *lights, static_cast<usize>(*count),
                Value::from_reference(*light));
            if (!stored) return std::unexpected(stored.error());
            auto transform_snapshot = snapshot_transform(machine, *transform);
            if (!transform_snapshot) {
                return std::unexpected(transform_snapshot.error());
            }
            stored = machine.heap().set_element(
                *transforms, static_cast<usize>(*count),
                Value::from_reference(*transform_snapshot));
            if (!stored) return std::unexpected(stored.error());
            stored = set_int_field(machine, *object, kGraphics3D,
                                   "lightCount", *count + 1);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_int(*count));
        });
    add(registry, kGraphics3D, "setLight",
        "(ILjavax/microedition/m3g/Light;"
        "Ljavax/microedition/m3g/Transform;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.setLight");
            auto index = int_argument(arguments, 1U, "Graphics3D.setLight");
            auto light = reference_argument(arguments, 2U,
                                            "Graphics3D.setLight", true);
            auto transform = reference_argument(arguments, 3U,
                                                "Graphics3D.setLight", true);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            if (!light) return std::unexpected(light.error());
            if (!transform) return std::unexpected(transform.error());
            auto count = int_field(machine, *object, kGraphics3D, "lightCount");
            if (!count) return std::unexpected(count.error());
            if (*index < 0 || *index >= *count) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "Graphics3D light index is out of range");
            }
            auto lights = graphics3d_reference_array(
                machine, *object, "lights",
                "[Ljavax/microedition/m3g/Light;");
            auto transforms = graphics3d_reference_array(
                machine, *object, "lightTransforms",
                "[Ljavax/microedition/m3g/Transform;");
            if (!lights) return std::unexpected(lights.error());
            if (!transforms) return std::unexpected(transforms.error());
            auto stored = machine.heap().set_element(
                *lights, static_cast<usize>(*index),
                Value::from_reference(*light));
            if (!stored) return std::unexpected(stored.error());
            auto transform_snapshot = snapshot_transform(machine, *transform);
            if (!transform_snapshot) {
                return std::unexpected(transform_snapshot.error());
            }
            stored = machine.heap().set_element(
                *transforms, static_cast<usize>(*index),
                Value::from_reference(*transform_snapshot));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, kGraphics3D, "getLight",
        "(ILjavax/microedition/m3g/Transform;)"
        "Ljavax/microedition/m3g/Light;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Graphics3D.getLight");
            auto index = int_argument(arguments, 1U, "Graphics3D.getLight");
            auto destination = reference_argument(
                arguments, 2U, "Graphics3D.getLight", true);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            if (!destination) return std::unexpected(destination.error());
            auto count = int_field(machine, *object, kGraphics3D, "lightCount");
            if (!count) return std::unexpected(count.error());
            if (*index < 0 || *index >= *count) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "Graphics3D light index is out of range");
            }
            auto lights = graphics3d_reference_array(
                machine, *object, "lights",
                "[Ljavax/microedition/m3g/Light;");
            auto transforms = graphics3d_reference_array(
                machine, *object, "lightTransforms",
                "[Ljavax/microedition/m3g/Transform;");
            if (!lights) return std::unexpected(lights.error());
            if (!transforms) return std::unexpected(transforms.error());
            auto light_value = machine.heap().element(
                *lights, static_cast<usize>(*index));
            auto transform_value = machine.heap().element(
                *transforms, static_cast<usize>(*index));
            if (!light_value) return std::unexpected(light_value.error());
            if (!transform_value) {
                return std::unexpected(transform_value.error());
            }
            auto light = light_value->as_reference();
            auto transform = transform_value->as_reference();
            if (!light) return std::unexpected(light.error());
            if (!transform) return std::unexpected(transform.error());
            if (!destination->is_null()) {
                Matrix matrix = identity_matrix();
                if (!transform->is_null()) {
                    auto loaded = transform_matrix(machine, *transform);
                    if (!loaded) return std::unexpected(loaded.error());
                    matrix = *loaded;
                }
                auto stored = set_transform_matrix(
                    machine, *destination, matrix);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value>(Value::from_reference(*light));
        });
}

[[nodiscard]] Result<std::string> loader_resource_name(
    Machine& machine,
    ObjectRef string) {
    if (string.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Loader resource name is null");
    }
    auto value = machine.heap().string_value(string);
    if (!value) return std::unexpected(value.error());
    std::string result;
    result.reserve(value->size());
    for (usize index = 0U; index < value->size(); ++index) {
        char32_t codepoint = static_cast<char32_t>((*value)[index]);
        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
            if (index + 1U >= value->size()) {
                return fail_java("java/io/IOException",
                                 "M3G resource name has an invalid surrogate");
            }
            const char32_t low = static_cast<char32_t>((*value)[++index]);
            if (low < 0xDC00U || low > 0xDFFFU) {
                return fail_java("java/io/IOException",
                                 "M3G resource name has an invalid surrogate");
            }
            codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) +
                        (low - 0xDC00U);
        } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
            return fail_java("java/io/IOException",
                             "M3G resource name has an invalid surrogate");
        }
        if (codepoint <= 0x7FU) {
            result.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            result.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            result.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            result.push_back(static_cast<char>(
                0x80U | ((codepoint >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            result.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            result.push_back(static_cast<char>(
                0x80U | ((codepoint >> 12U) & 0x3FU)));
            result.push_back(static_cast<char>(
                0x80U | ((codepoint >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }
    if (result.find(':') == std::string::npos) {
        while (!result.empty() && result.front() == '/') {
            result.erase(result.begin());
        }
    }
    if (result.empty()) {
        return fail_java("java/io/IOException",
                         "M3G resource name is empty");
    }
    return result;
}

[[nodiscard]] Result<std::string> external_resource_name(
    std::string_view owner_resource,
    std::string_view external_uri) {
    if (external_uri.empty()) {
        return fail_java("java/io/IOException",
                         "M3G external reference URI is empty");
    }
    if (external_uri.find('\\') != std::string_view::npos ||
        external_uri.find('\0') != std::string_view::npos) {
        return fail_java("java/io/IOException",
                         "M3G external reference URI is invalid");
    }
    if (external_uri.find(':') != std::string_view::npos) {
        return std::string(external_uri);
    }
    const usize scheme_separator = owner_resource.find("://");
    const bool network_owner = scheme_separator != std::string_view::npos;
    if (network_owner && external_uri.starts_with("//")) {
        return std::string(owner_resource.substr(0U, scheme_separator)) +
               ":" + std::string(external_uri);
    }

    std::string prefix;
    std::string_view owner_path = owner_resource;
    if (network_owner) {
        const usize authority_end = owner_resource.find(
            '/', scheme_separator + 3U);
        if (authority_end == std::string_view::npos) {
            prefix = std::string(owner_resource);
            owner_path = {};
        } else {
            prefix = std::string(owner_resource.substr(0U, authority_end));
            owner_path = owner_resource.substr(authority_end + 1U);
        }
    }

    std::vector<std::string> components;
    const bool absolute = external_uri.front() == '/';
    if (!absolute) {
        const usize slash = owner_path.rfind('/');
        const std::string_view base = slash == std::string_view::npos
            ? std::string_view {} : owner_path.substr(0U, slash);
        usize start = 0U;
        while (start < base.size()) {
            const usize end = base.find('/', start);
            const auto component = base.substr(
                start, end == std::string_view::npos
                    ? base.size() - start : end - start);
            if (!component.empty()) components.emplace_back(component);
            if (end == std::string_view::npos) break;
            start = end + 1U;
        }
    }

    usize start = absolute ? 1U : 0U;
    while (start <= external_uri.size()) {
        const usize end = external_uri.find('/', start);
        const auto component = external_uri.substr(
            start, end == std::string_view::npos
                ? external_uri.size() - start : end - start);
        if (component.empty() || component == ".") {
            // Ignore duplicate separators and current-directory segments.
        } else if (component == "..") {
            if (components.empty()) {
                if (!network_owner) {
                    return fail_java(
                        "java/io/IOException",
                        "M3G external reference escapes the JAR root");
                }
            } else {
                components.pop_back();
            }
        } else {
            components.emplace_back(component);
        }
        if (end == std::string_view::npos) break;
        start = end + 1U;
    }
    if (components.empty() && prefix.empty()) {
        return fail_java("java/io/IOException",
                         "M3G external reference has no resource name");
    }

    std::string result = std::move(prefix);
    if (network_owner) result.push_back('/');
    for (usize index = 0U; index < components.size(); ++index) {
        if (index != 0U) result.push_back('/');
        result += components[index];
    }
    return result;
}

[[nodiscard]] Result<ObjectRef> execution_reference(
    Machine& machine,
    const ExecutionResult& result,
    std::string_view operation) {
    if (!result.completed_normally()) {
        if (!result.throwable.has_value()) {
            return fail(ErrorCode::internal_error,
                        std::string(operation) +
                            " failed without a Java throwable");
        }
        auto class_name = machine.heap().class_name(*result.throwable);
        if (!class_name) return std::unexpected(class_name.error());
        return fail_java(*class_name, std::string(operation) + " failed");
    }
    if (!result.return_value.has_value()) {
        return fail(ErrorCode::invalid_state,
                    std::string(operation) + " returned no object");
    }
    auto reference = result.return_value->as_reference();
    if (!reference) return std::unexpected(reference.error());
    if (reference->is_null()) {
        return fail_java("java/io/IOException",
                         std::string(operation) + " returned null");
    }
    return *reference;
}

[[nodiscard]] Result<ObjectRef> create_ascii_string(
    Machine& machine,
    std::string_view text) {
    auto string = allocate_instance(machine, "java/lang/String");
    if (!string) return std::unexpected(string.error());
    std::u16string utf16;
    utf16.reserve(text.size());
    for (const char character : text) {
        utf16.push_back(static_cast<char16_t>(
            static_cast<unsigned char>(character)));
    }
    auto attached = machine.heap().attach_string(*string, std::move(utf16));
    if (!attached) return std::unexpected(attached.error());
    return *string;
}

enum class LoaderResourceKind : u8 {
    m3g,
    png,
    jpeg,
    unknown,
};

constexpr std::array<u8, 12> kLoaderM3gSignature {
    0xABU, 0x4AU, 0x53U, 0x52U, 0x31U, 0x38U,
    0x34U, 0xBBU, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
};
constexpr std::array<u8, 8> kLoaderPngSignature {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
};
constexpr usize kMaximumLoaderBytes = 64U * 1024U * 1024U;

[[nodiscard]] LoaderResourceKind loader_resource_kind(
    std::span<const u8> bytes) noexcept {
    if (bytes.size() >= kLoaderM3gSignature.size() &&
        std::equal(kLoaderM3gSignature.begin(), kLoaderM3gSignature.end(),
                   bytes.begin())) {
        return LoaderResourceKind::m3g;
    }
    if (bytes.size() >= kLoaderPngSignature.size() &&
        std::equal(kLoaderPngSignature.begin(), kLoaderPngSignature.end(),
                   bytes.begin())) {
        return LoaderResourceKind::png;
    }
    if (bytes.size() >= 2U && bytes[0U] == 0xFFU && bytes[1U] == 0xD8U) {
        return LoaderResourceKind::jpeg;
    }
    return LoaderResourceKind::unknown;
}

[[nodiscard]] i32 png_image2d_format(std::span<const u8> bytes) noexcept {
    if (bytes.size() < 26U) return 99;
    const u8 color_type = bytes[25U];
    bool transparent = color_type == 4U || color_type == 6U;
    usize offset = 8U;
    while (offset + 12U <= bytes.size()) {
        const u32 length =
            (static_cast<u32>(bytes[offset]) << 24U) |
            (static_cast<u32>(bytes[offset + 1U]) << 16U) |
            (static_cast<u32>(bytes[offset + 2U]) << 8U) |
            static_cast<u32>(bytes[offset + 3U]);
        if (length > bytes.size() - offset - 12U) break;
        const std::array<u8, 4> type {
            bytes[offset + 4U], bytes[offset + 5U],
            bytes[offset + 6U], bytes[offset + 7U],
        };
        if (type == std::array<u8, 4> {'t', 'R', 'N', 'S'}) {
            transparent = true;
        }
        if (type == std::array<u8, 4> {'I', 'D', 'A', 'T'}) break;
        offset += 12U + static_cast<usize>(length);
    }
    if (color_type == 0U) return transparent ? 98 : 96;
    if (color_type == 4U) return 98;
    return transparent ? 100 : 99;
}

[[nodiscard]] i32 jpeg_image2d_format(std::span<const u8> bytes) noexcept {
    usize offset = 2U;
    while (offset + 3U < bytes.size()) {
        while (offset < bytes.size() && bytes[offset] != 0xFFU) ++offset;
        while (offset < bytes.size() && bytes[offset] == 0xFFU) ++offset;
        if (offset >= bytes.size()) break;
        const u8 marker = bytes[offset++];
        if (marker == 0xD9U || marker == 0xDAU) break;
        if (marker == 0x01U || (marker >= 0xD0U && marker <= 0xD7U)) continue;
        if (offset + 2U > bytes.size()) break;
        const usize length =
            (static_cast<usize>(bytes[offset]) << 8U) |
            static_cast<usize>(bytes[offset + 1U]);
        if (length < 2U || length > bytes.size() - offset) break;
        const bool start_of_frame =
            (marker >= 0xC0U && marker <= 0xC3U) ||
            (marker >= 0xC5U && marker <= 0xC7U) ||
            (marker >= 0xC9U && marker <= 0xCBU) ||
            (marker >= 0xCDU && marker <= 0xCFU);
        if (start_of_frame && length >= 8U) {
            return bytes[offset + 7U] == 1U ? 96 : 99;
        }
        offset += length;
    }
    return 99;
}

[[nodiscard]] Result<ObjectRef> wrap_lcdui_image2d(
    Machine& machine,
    ObjectRef image,
    i32 image_format) {
    auto image_root = machine.pin_native_root(image);
    if (!image_root) return std::unexpected(image_root.error());
    auto width = int_field(machine, image,
                           "javax/microedition/lcdui/Image", "width");
    auto height = int_field(machine, image,
                            "javax/microedition/lcdui/Image", "height");
    if (!width) return std::unexpected(width.error());
    if (!height) return std::unexpected(height.error());
    auto image2d = allocate_instance(machine, kImage2D);
    if (!image2d) return std::unexpected(image2d.error());
    auto image2d_root = machine.pin_native_root(*image2d);
    if (!image2d_root) return std::unexpected(image2d_root.error());
    auto initialized = initialize_object3d(machine, *image2d);
    if (!initialized) return std::unexpected(initialized.error());
    const std::array<Status, 5> stored {
        set_int_field(machine, *image2d, kImage2D, "format", image_format),
        set_int_field(machine, *image2d, kImage2D, "width", *width),
        set_int_field(machine, *image2d, kImage2D, "height", *height),
        set_int_field(machine, *image2d, kImage2D, "mutable", 0, "Z"),
        set_reference_field(machine, *image2d, kImage2D,
                            "source", "Ljava/lang/Object;", image),
    };
    for (const Status& status : stored) {
        if (!status) return std::unexpected(status.error());
    }
    return *image2d;
}

[[nodiscard]] Result<ObjectRef> load_image2d_bytes(
    Machine& machine,
    std::span<const u8> bytes,
    LoaderResourceKind kind) {
    if (bytes.empty() || bytes.size() > kMaximumLoaderBytes) {
        return fail_java("java/io/IOException",
                         "M3G image resource size is invalid");
    }
    auto data = allocate_array(machine, "[B", bytes.size(),
                               Value::from_int(0));
    if (!data) return std::unexpected(data.error());
    auto data_root = machine.pin_native_root(*data);
    if (!data_root) return std::unexpected(data_root.error());
    auto written = machine.heap().write_byte_array(*data, 0U, bytes);
    if (!written) return std::unexpected(written.error());
    const std::array<Value, 3> arguments {
        Value::from_reference(*data), Value::from_int(0),
        Value::from_int(static_cast<i32>(bytes.size())),
    };
    auto created = machine.invoke_static(
        "javax/microedition/lcdui/Image", "createImage",
        "([BII)Ljavax/microedition/lcdui/Image;", arguments);
    if (!created) return std::unexpected(created.error());
    auto image = execution_reference(machine, *created,
                                     "M3G image decode");
    if (!image) return std::unexpected(image.error());
    const i32 format = kind == LoaderResourceKind::png
        ? png_image2d_format(bytes)
        : jpeg_image2d_format(bytes);
    return wrap_lcdui_image2d(machine, *image, format);
}

[[nodiscard]] Result<ObjectRef> single_object3d_array(
    Machine& machine,
    ObjectRef object) {
    auto result = allocate_array(
        machine, "[Ljavax/microedition/m3g/Object3D;", 1U,
        Value::from_reference({}));
    if (!result) return std::unexpected(result.error());
    auto result_root = machine.pin_native_root(*result);
    if (!result_root) return std::unexpected(result_root.error());
    auto stored = machine.heap().set_element(
        *result, 0U, Value::from_reference(object));
    if (!stored) return std::unexpected(stored.error());
    return *result;
}

struct ExternalLoadState final :
    std::enable_shared_from_this<ExternalLoadState> {
    explicit ExternalLoadState(Machine& target) noexcept : machine(target) {}

    [[nodiscard]] ExternalReferenceResolver resolver(std::string owner) {
        auto self = shared_from_this();
        return [self = std::move(self), owner = std::move(owner)](
            std::string_view uri) -> Result<ObjectRef> {
            auto resource = external_resource_name(owner, uri);
            if (!resource) return std::unexpected(resource.error());
            return self->load(*resource);
        };
    }

    [[nodiscard]] Result<ObjectRef> load(const std::string& resource) {
        const auto found = cache.find(resource);
        if (found != cache.end()) return found->second;
        if (loading.contains(resource)) {
            return fail_java("java/io/IOException",
                             "M3G external reference cycle detected");
        }
        loading.insert(resource);
        struct LoadingGuard final {
            std::unordered_set<std::string>& resources;
            const std::string& resource;
            ~LoadingGuard() { resources.erase(resource); }
        } guard {loading, resource};

        auto bytes = machine.classes().read_resource(resource);
        if (!bytes) {
            return fail_java("java/io/IOException", bytes.error().message);
        }
        ObjectRef object {};
        switch (loader_resource_kind(*bytes)) {
        case LoaderResourceKind::png: {
            auto loaded = load_image2d_bytes(
                machine, *bytes, LoaderResourceKind::png);
            if (!loaded) return std::unexpected(loaded.error());
            object = *loaded;
            break;
        }
        case LoaderResourceKind::m3g: {
            auto objects = load_m3g(
                machine, *bytes, resolver(resource));
            if (!objects) return std::unexpected(objects.error());
            auto objects_root = machine.pin_native_root(*objects);
            if (!objects_root) return std::unexpected(objects_root.error());
            auto count = machine.heap().array_length(*objects);
            if (!count) return std::unexpected(count.error());
            if (*count == 0U) {
                return fail_java(
                    "java/io/IOException",
                    "external M3G file has no root-level object");
            }
            auto first = machine.heap().element(*objects, 0U);
            if (!first) return std::unexpected(first.error());
            auto reference = first->as_reference();
            if (!reference) return std::unexpected(reference.error());
            if (reference->is_null()) {
                return fail_java(
                    "java/io/IOException",
                    "external M3G root-level object is null");
            }
            object = *reference;
            break;
        }
        case LoaderResourceKind::jpeg:
            return fail_java(
                "java/io/IOException",
                "JPEG is not valid for an M3G external reference");
        case LoaderResourceKind::unknown:
            return fail_java(
                "java/io/IOException",
                "unsupported M3G external reference type");
        }
        cache.emplace(resource, object);
        return object;
    }

    Machine& machine;
    std::unordered_map<std::string, ObjectRef> cache;
    std::unordered_set<std::string> loading;
};

[[nodiscard]] ExternalReferenceResolver external_resolver(
    Machine& machine,
    std::string owner_resource) {
    auto state = std::make_shared<ExternalLoadState>(machine);
    if (!owner_resource.empty()) {
        state->loading.insert(owner_resource);
    }
    return state->resolver(std::move(owner_resource));
}

[[nodiscard]] Result<ObjectRef> load_loader_bytes(
    Machine& machine,
    std::span<const u8> bytes,
    std::string owner_resource) {
    switch (const LoaderResourceKind kind = loader_resource_kind(bytes)) {
    case LoaderResourceKind::m3g:
        return load_m3g(
            machine, bytes,
            external_resolver(machine, std::move(owner_resource)));
    case LoaderResourceKind::png:
    case LoaderResourceKind::jpeg: {
        auto image = load_image2d_bytes(machine, bytes, kind);
        if (!image) return std::unexpected(image.error());
        return single_object3d_array(machine, *image);
    }
    case LoaderResourceKind::unknown:
        return fail_java("java/io/IOException",
                         "unsupported M3G Loader resource type");
    }
    return fail(ErrorCode::internal_error,
                "unreachable M3G Loader resource type");
}

[[nodiscard]] Result<std::vector<u8>> loader_byte_array(
    Machine& machine,
    ObjectRef array,
    i32 offset) {
    if (array.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Loader byte array is null");
    }
    auto class_name = machine.heap().class_name(array);
    auto length = machine.heap().array_length(array);
    if (!class_name) return std::unexpected(class_name.error());
    if (!length) return std::unexpected(length.error());
    if (*class_name != "[B" || offset < 0 ||
        static_cast<usize>(offset) > *length) {
        return fail_java("java/lang/IndexOutOfBoundsException",
                         "Loader byte array offset is invalid");
    }
    std::vector<u8> result;
    result.reserve(*length - static_cast<usize>(offset));
    for (usize index = static_cast<usize>(offset); index < *length; ++index) {
        auto value = machine.heap().element(array, index);
        if (!value) return std::unexpected(value.error());
        auto number = value->as_int();
        if (!number) return std::unexpected(number.error());
        result.push_back(static_cast<u8>(*number & 0xFF));
    }
    return result;
}

void register_loader(NativeMethodRegistry& registry) {
    add(registry, "javax/microedition/m3g/Loader", "load",
        "(Ljava/lang/String;)[Ljavax/microedition/m3g/Object3D;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto name = reference_argument(arguments, 0U,
                                            "Loader.load", false);
            if (!name) return std::unexpected(name.error());
            auto resource = loader_resource_name(machine, *name);
            if (!resource) return std::unexpected(resource.error());
            auto bytes = machine.classes().read_resource(*resource);
            if (!bytes) {
                return fail_java("java/io/IOException",
                                 bytes.error().message);
            }
            auto objects = load_loader_bytes(
                machine, *bytes, std::move(*resource));
            if (!objects) return std::unexpected(objects.error());
            return std::optional<Value>(Value::from_reference(*objects));
        });
    add(registry, "javax/microedition/m3g/Loader", "load",
        "([BI)[Ljavax/microedition/m3g/Object3D;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto data = reference_argument(arguments, 0U,
                                            "Loader.load", false);
            auto offset = int_argument(arguments, 1U, "Loader.load");
            if (!data) return std::unexpected(data.error());
            if (!offset) return std::unexpected(offset.error());
            auto bytes = loader_byte_array(machine, *data, *offset);
            if (!bytes) return std::unexpected(bytes.error());
            auto objects = load_loader_bytes(machine, *bytes, {});
            if (!objects) return std::unexpected(objects.error());
            return std::optional<Value>(Value::from_reference(*objects));
        });
}

} // namespace

void register_m3g_render_natives(NativeMethodRegistry& registry) {
    register_graphics3d(registry);
    register_loader(registry);
}

} // namespace phoneme::vm
