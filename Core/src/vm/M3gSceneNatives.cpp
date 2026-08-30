#include "M3gNativeModules.hpp"

#include <array>
#include <limits>
#include <unordered_set>
#include <vector>

#include "M3gNativeSupport.hpp"

namespace phoneme::vm {
namespace {

using namespace m3g;

constexpr usize kPickTextureUnitCount = 8U;

[[nodiscard]] Result<ObjectRef> animation_array(Machine& machine,
                                                 ObjectRef object) {
    auto array = reference_field(machine, object, kObject3D,
                                 "animationTracks",
                                 "[Ljavax/microedition/m3g/AnimationTrack;");
    if (!array) return std::unexpected(array.error());
    if (!array->is_null()) return *array;
    auto created = allocate_array(machine,
        "[Ljavax/microedition/m3g/AnimationTrack;", 4U,
        Value::from_reference({}));
    if (!created) return std::unexpected(created.error());
    auto stored = set_reference_field(machine, object, kObject3D,
        "animationTracks", "[Ljavax/microedition/m3g/AnimationTrack;",
        *created);
    if (!stored) return std::unexpected(stored.error());
    return *created;
}

[[nodiscard]] Status ensure_animation_capacity(Machine& machine,
                                                ObjectRef object,
                                                i32 minimum) {
    auto array = animation_array(machine, object);
    if (!array) return std::unexpected(array.error());
    auto capacity = machine.heap().array_length(*array);
    if (!capacity) return std::unexpected(capacity.error());
    if (minimum <= static_cast<i32>(*capacity)) return {};
    const usize new_capacity = std::max(static_cast<usize>(minimum),
        *capacity == 0U ? 4U : *capacity * 2U);
    auto replacement = allocate_array(machine,
        "[Ljavax/microedition/m3g/AnimationTrack;", new_capacity,
        Value::from_reference({}));
    if (!replacement) return std::unexpected(replacement.error());
    auto count = int_field(machine, object, kObject3D,
                           "animationTrackCount");
    if (!count) return std::unexpected(count.error());
    if (*count > 0) {
        auto copied = machine.heap().copy_array_range(
            *array, 0U, *replacement, 0U, static_cast<usize>(*count));
        if (!copied) return copied;
    }
    return set_reference_field(machine, object, kObject3D,
        "animationTracks", "[Ljavax/microedition/m3g/AnimationTrack;",
        *replacement);
}

[[nodiscard]] Result<ObjectRef> child_array(Machine& machine,
                                             ObjectRef object) {
    auto array = reference_field(machine, object, kGroup, "children",
                                 "[Ljavax/microedition/m3g/Node;");
    if (!array) return std::unexpected(array.error());
    if (!array->is_null()) return *array;
    auto initialized = initialize_group(machine, object);
    if (!initialized) return std::unexpected(initialized.error());
    return reference_field(machine, object, kGroup, "children",
                           "[Ljavax/microedition/m3g/Node;");
}

[[nodiscard]] Status ensure_child_capacity(Machine& machine,
                                            ObjectRef object,
                                            i32 minimum) {
    auto array = child_array(machine, object);
    if (!array) return std::unexpected(array.error());
    auto capacity = machine.heap().array_length(*array);
    if (!capacity) return std::unexpected(capacity.error());
    if (minimum <= static_cast<i32>(*capacity)) return {};
    const usize new_capacity = std::max(static_cast<usize>(minimum),
        *capacity == 0U ? 4U : *capacity * 2U);
    auto replacement = allocate_array(machine,
        "[Ljavax/microedition/m3g/Node;", new_capacity,
        Value::from_reference({}));
    if (!replacement) return std::unexpected(replacement.error());
    auto count = int_field(machine, object, kGroup, "childCount");
    if (!count) return std::unexpected(count.error());
    if (*count > 0) {
        auto copied = machine.heap().copy_array_range(
            *array, 0U, *replacement, 0U, static_cast<usize>(*count));
        if (!copied) return copied;
    }
    return set_reference_field(machine, object, kGroup, "children",
                               "[Ljavax/microedition/m3g/Node;", *replacement);
}

[[nodiscard]] Result<bool> is_descendant(Machine& machine,
                                         ObjectRef candidate,
                                         ObjectRef ancestor) {
    ObjectRef current = candidate;
    for (usize depth = 0; depth < 1'024U && !current.is_null(); ++depth) {
        if (current == ancestor) return true;
        auto parent = reference_field(machine, current, kNode, "parent",
                                      "Ljavax/microedition/m3g/Node;");
        if (!parent) return std::unexpected(parent.error());
        current = *parent;
    }
    return false;
}

[[nodiscard]] Result<Matrix> composite_matrix(Machine& machine,
                                              ObjectRef object) {
    std::vector<ObjectRef> chain;
    ObjectRef current = object;
    for (usize depth = 0; depth < 1'024U && !current.is_null(); ++depth) {
        chain.push_back(current);
        auto parent = reference_field(machine, current, kNode, "parent",
                                      "Ljavax/microedition/m3g/Node;");
        if (!parent) break;
        current = *parent;
    }
    std::reverse(chain.begin(), chain.end());
    Matrix result = identity_matrix();
    for (ObjectRef entry : chain) {
        auto local = local_transform(machine, entry);
        if (!local) return std::unexpected(local.error());
        auto matrix = transform_matrix(machine, *local);
        if (!matrix) return std::unexpected(matrix.error());
        result = multiply(result, *matrix);
    }
    return result;
}

using Vector3 = std::array<float, 3>;

[[nodiscard]] Vector3 subtract(Vector3 left, Vector3 right) noexcept {
    return {left[0U] - right[0U],
            left[1U] - right[1U],
            left[2U] - right[2U]};
}

[[nodiscard]] Vector3 multiply_vector(Vector3 value, float scale) noexcept {
    return {value[0U] * scale, value[1U] * scale, value[2U] * scale};
}

[[nodiscard]] float dot(Vector3 left, Vector3 right) noexcept {
    return left[0U] * right[0U] +
           left[1U] * right[1U] +
           left[2U] * right[2U];
}

[[nodiscard]] Vector3 cross(Vector3 left, Vector3 right) noexcept {
    return {
        left[1U] * right[2U] - left[2U] * right[1U],
        left[2U] * right[0U] - left[0U] * right[2U],
        left[0U] * right[1U] - left[1U] * right[0U],
    };
}

[[nodiscard]] Result<Vector3> normalize_vector(Vector3 value,
                                               std::string_view operation) {
    const float length = std::sqrt(dot(value, value));
    if (!std::isfinite(length) || length <= 1.0e-6F) {
        return fail_java("java/lang/IllegalStateException",
                         std::string(operation) + " direction is undefined");
    }
    return multiply_vector(value, 1.0F / length);
}

[[nodiscard]] Vector3 transform_vector3(const Matrix& matrix,
                                        Vector3 vector,
                                        float w) noexcept {
    return {
        matrix[0U] * vector[0U] + matrix[1U] * vector[1U] +
            matrix[2U] * vector[2U] + matrix[3U] * w,
        matrix[4U] * vector[0U] + matrix[5U] * vector[1U] +
            matrix[6U] * vector[2U] + matrix[7U] * w,
        matrix[8U] * vector[0U] + matrix[9U] * vector[1U] +
            matrix[10U] * vector[2U] + matrix[11U] * w,
    };
}

[[nodiscard]] Result<ObjectRef> root_node(Machine& machine,
                                          ObjectRef node) {
    ObjectRef current = node;
    for (usize depth = 0U; depth < 1'024U; ++depth) {
        auto parent = reference_field(machine, current, kNode, "parent",
                                      "Ljavax/microedition/m3g/Node;");
        if (!parent) return std::unexpected(parent.error());
        if (parent->is_null()) return current;
        current = *parent;
    }
    return fail(ErrorCode::invalid_state,
                "M3G scene graph parent chain is too deep");
}

[[nodiscard]] Result<Quaternion> quaternion_between(Vector3 source,
                                                    Vector3 target) {
    auto from = normalize_vector(source, "alignment source");
    auto to = normalize_vector(target, "alignment target");
    if (!from) return std::unexpected(from.error());
    if (!to) return std::unexpected(to.error());
    const float cosine = std::clamp(dot(*from, *to), -1.0F, 1.0F);
    if (cosine >= 1.0F - 1.0e-6F) return Quaternion {};
    if (cosine <= -1.0F + 1.0e-6F) {
        Vector3 axis = std::abs((*from)[0U]) < 0.8F
            ? cross(*from, Vector3 {1.0F, 0.0F, 0.0F})
            : cross(*from, Vector3 {0.0F, 1.0F, 0.0F});
        auto normalized_axis = normalize_vector(axis, "alignment opposite axis");
        if (!normalized_axis) return std::unexpected(normalized_axis.error());
        return Quaternion {
            .x = (*normalized_axis)[0U],
            .y = (*normalized_axis)[1U],
            .z = (*normalized_axis)[2U],
            .w = 0.0F,
        };
    }
    const Vector3 axis = cross(*from, *to);
    return normalized_quaternion(Quaternion {
        .x = axis[0U],
        .y = axis[1U],
        .z = axis[2U],
        .w = 1.0F + cosine,
    });
}

[[nodiscard]] Result<Quaternion> quaternion_from_basis(
    Vector3 x_axis,
    Vector3 y_axis,
    Vector3 z_axis) {
    Matrix matrix = identity_matrix();
    matrix[0U] = x_axis[0U];
    matrix[1U] = y_axis[0U];
    matrix[2U] = z_axis[0U];
    matrix[4U] = x_axis[1U];
    matrix[5U] = y_axis[1U];
    matrix[6U] = z_axis[1U];
    matrix[8U] = x_axis[2U];
    matrix[9U] = y_axis[2U];
    matrix[10U] = z_axis[2U];
    const float trace = matrix[0U] + matrix[5U] + matrix[10U];
    Quaternion quaternion;
    if (trace > 0.0F) {
        const float scale = std::sqrt(trace + 1.0F) * 2.0F;
        quaternion.w = 0.25F * scale;
        quaternion.x = (matrix[9U] - matrix[6U]) / scale;
        quaternion.y = (matrix[2U] - matrix[8U]) / scale;
        quaternion.z = (matrix[4U] - matrix[1U]) / scale;
    } else if (matrix[0U] > matrix[5U] && matrix[0U] > matrix[10U]) {
        const float scale = std::sqrt(
            1.0F + matrix[0U] - matrix[5U] - matrix[10U]) * 2.0F;
        quaternion.w = (matrix[9U] - matrix[6U]) / scale;
        quaternion.x = 0.25F * scale;
        quaternion.y = (matrix[1U] + matrix[4U]) / scale;
        quaternion.z = (matrix[2U] + matrix[8U]) / scale;
    } else if (matrix[5U] > matrix[10U]) {
        const float scale = std::sqrt(
            1.0F + matrix[5U] - matrix[0U] - matrix[10U]) * 2.0F;
        quaternion.w = (matrix[2U] - matrix[8U]) / scale;
        quaternion.x = (matrix[1U] + matrix[4U]) / scale;
        quaternion.y = 0.25F * scale;
        quaternion.z = (matrix[6U] + matrix[9U]) / scale;
    } else {
        const float scale = std::sqrt(
            1.0F + matrix[10U] - matrix[0U] - matrix[5U]) * 2.0F;
        quaternion.w = (matrix[4U] - matrix[1U]) / scale;
        quaternion.x = (matrix[2U] + matrix[8U]) / scale;
        quaternion.y = (matrix[6U] + matrix[9U]) / scale;
        quaternion.z = 0.25F * scale;
    }
    return normalized_quaternion(quaternion);
}

[[nodiscard]] Result<Vector3> alignment_target_vector(
    Machine& machine,
    ObjectRef node,
    ObjectRef target,
    i32 target_axis) {
    auto parent = reference_field(machine, node, kNode, "parent",
                                  "Ljavax/microedition/m3g/Node;");
    if (!parent) return std::unexpected(parent.error());
    Matrix parent_world = identity_matrix();
    if (!parent->is_null()) {
        auto loaded = composite_matrix(machine, *parent);
        if (!loaded) return std::unexpected(loaded.error());
        parent_world = *loaded;
    }
    auto inverse_parent = inverse_matrix(parent_world);
    if (!inverse_parent) return std::unexpected(inverse_parent.error());
    auto target_world = composite_matrix(machine, target);
    if (!target_world) return std::unexpected(target_world.error());
    if (target_axis == 145) {
        const Vector3 world_origin = transform_vector3(
            *target_world, Vector3 {0.0F, 0.0F, 0.0F}, 1.0F);
        const Vector3 parent_origin = transform_vector3(
            *inverse_parent, world_origin, 1.0F);
        auto tx = float_field(machine, node, kTransformable, "translationX");
        auto ty = float_field(machine, node, kTransformable, "translationY");
        auto tz = float_field(machine, node, kTransformable, "translationZ");
        if (!tx) return std::unexpected(tx.error());
        if (!ty) return std::unexpected(ty.error());
        if (!tz) return std::unexpected(tz.error());
        return normalize_vector(
            subtract(parent_origin, Vector3 {*tx, *ty, *tz}),
            "alignment origin");
    }
    Vector3 axis;
    switch (target_axis) {
    case 146: axis = {1.0F, 0.0F, 0.0F}; break;
    case 147: axis = {0.0F, 1.0F, 0.0F}; break;
    case 148: axis = {0.0F, 0.0F, 1.0F}; break;
    default:
        return fail_java("java/lang/IllegalArgumentException",
                         "alignment target is invalid");
    }
    const Vector3 world_axis = transform_vector3(*target_world, axis, 0.0F);
    return normalize_vector(
        transform_vector3(*inverse_parent, world_axis, 0.0F),
        "alignment axis");
}

struct PickVertexArray final {
    i32 vertex_count {0};
    i32 component_count {0};
    std::vector<float> values;
};

struct PickVertex final {
    Vector3 position {};
    std::array<float, kPickTextureUnitCount> texture_s {};
    std::array<float, kPickTextureUnitCount> texture_t {};
};

struct PickHit final {
    ObjectRef node {};
    float distance {std::numeric_limits<float>::infinity()};
    i32 submesh_index {0};
    std::array<float, kPickTextureUnitCount> texture_s {};
    std::array<float, kPickTextureUnitCount> texture_t {};
    Vector3 normal {0.0F, 0.0F, 1.0F};
};

[[nodiscard]] Result<std::vector<ObjectRef>> scene_reference_array(
    Machine& machine,
    ObjectRef array,
    std::string_view operation) {
    if (array.is_null()) return std::vector<ObjectRef> {};
    auto values = machine.heap().read_reference_array(array);
    if (!values && values.error().code == ErrorCode::invalid_argument) {
        return fail_java("java/lang/IllegalArgumentException",
                         std::string(operation) + " expects an object array");
    }
    if (!values) return std::unexpected(values.error());
    return values;
}

[[nodiscard]] Result<std::vector<i32>> pick_int_array(
    Machine& machine,
    ObjectRef array) {
    if (array.is_null()) return std::vector<i32> {};
    return machine.heap().read_int_array(array);
}

[[nodiscard]] Result<PickVertexArray> pick_vertex_array(
    Machine& machine,
    ObjectRef array) {
    if (array.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "VertexArray is null");
    }
    static constexpr std::array<ObjectFieldSpec, 4U> kFields {{
        {"vertexCount", "I"},
        {"componentCount", "I"},
        {"componentSize", "I"},
        {"data", "Ljava/lang/Object;"},
    }};
    auto fields = object_fields(machine, array, kVertexArray, kFields);
    if (!fields) return std::unexpected(fields.error());
    auto vertex_count = (*fields)[0U].as_int();
    auto component_count = (*fields)[1U].as_int();
    auto component_size = (*fields)[2U].as_int();
    auto data = (*fields)[3U].as_reference();
    if (!vertex_count) return std::unexpected(vertex_count.error());
    if (!component_count) return std::unexpected(component_count.error());
    if (!component_size) return std::unexpected(component_size.error());
    if (!data) return std::unexpected(data.error());
    if (*vertex_count < 0 || *component_count < 2 ||
        *component_count > 4 ||
        (*component_size != 1 && *component_size != 2) ||
        data->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "VertexArray storage is invalid");
    }
    const usize value_count = static_cast<usize>(*vertex_count) *
                              static_cast<usize>(*component_count);
    PickVertexArray result {
        .vertex_count = *vertex_count,
        .component_count = *component_count,
        .values = std::vector<float>(value_count),
    };
    if (*component_size == 1) {
        auto values = machine.heap().read_byte_array(*data);
        if (!values) return std::unexpected(values.error());
        if (values->size() < value_count) {
            return fail(ErrorCode::invalid_state,
                        "VertexArray storage is truncated");
        }
        for (usize index = 0U; index < value_count; ++index) {
            result.values[index] = static_cast<float>(
                static_cast<i8>((*values)[index]));
        }
    } else {
        auto values = machine.heap().read_short_array(*data);
        if (!values) return std::unexpected(values.error());
        if (values->size() < value_count) {
            return fail(ErrorCode::invalid_state,
                        "VertexArray storage is truncated");
        }
        for (usize index = 0U; index < value_count; ++index) {
            result.values[index] = static_cast<float>((*values)[index]);
        }
    }
    return result;
}

[[nodiscard]] Result<std::vector<PickVertex>> pick_vertices(
    Machine& machine,
    ObjectRef vertex_buffer) {
    static constexpr std::array<ObjectFieldSpec, 3U> kPositionFields {{
        {"positions", "Ljavax/microedition/m3g/VertexArray;"},
        {"positionScale", "F"},
        {"positionBias", "[F"},
    }};
    auto position_fields = object_fields(
        machine, vertex_buffer, kVertexBuffer, kPositionFields);
    if (!position_fields) return std::unexpected(position_fields.error());
    auto positions = (*position_fields)[0U].as_reference();
    auto scale = (*position_fields)[1U].as_float();
    auto bias = (*position_fields)[2U].as_reference();
    if (!positions) return std::unexpected(positions.error());
    if (!scale) return std::unexpected(scale.error());
    if (!bias) return std::unexpected(bias.error());
    if (positions->is_null()) return std::vector<PickVertex> {};
    auto source = pick_vertex_array(machine, *positions);
    if (!source) return std::unexpected(source.error());
    std::vector<float> offsets;
    if (!bias->is_null()) {
        auto loaded = read_float_array(machine, *bias,
                                       "VertexBuffer position bias");
        if (!loaded) return std::unexpected(loaded.error());
        offsets = std::move(*loaded);
    }
    std::vector<PickVertex> result(
        static_cast<usize>(source->vertex_count));
    for (i32 vertex = 0; vertex < source->vertex_count; ++vertex) {
        const usize base = static_cast<usize>(vertex) *
                           static_cast<usize>(source->component_count);
        const auto component = [&](usize index) {
            return index < static_cast<usize>(source->component_count)
                ? source->values[base + index] : 0.0F;
        };
        result[static_cast<usize>(vertex)].position = {
            component(0U) * *scale +
                (offsets.empty() ? 0.0F : offsets[0U]),
            component(1U) * *scale +
                (offsets.size() < 2U ? 0.0F : offsets[1U]),
            component(2U) * *scale +
                (offsets.size() < 3U ? 0.0F : offsets[2U]),
        };
    }

    static constexpr std::array<ObjectFieldSpec, 3U> kTextureFields {{
        {"texCoords", "[Ljavax/microedition/m3g/VertexArray;"},
        {"texScales", "[F"},
        {"texBiases", "[[F"},
    }};
    auto texture_fields = object_fields(
        machine, vertex_buffer, kVertexBuffer, kTextureFields);
    if (!texture_fields) return std::unexpected(texture_fields.error());
    auto texcoord_arrays = (*texture_fields)[0U].as_reference();
    auto texcoord_scales = (*texture_fields)[1U].as_reference();
    auto texcoord_biases = (*texture_fields)[2U].as_reference();
    if (!texcoord_arrays) return std::unexpected(texcoord_arrays.error());
    if (!texcoord_scales) return std::unexpected(texcoord_scales.error());
    if (!texcoord_biases) return std::unexpected(texcoord_biases.error());
    if (texcoord_arrays->is_null()) return result;
    auto texture_arrays = scene_reference_array(
        machine, *texcoord_arrays, "VertexBuffer texture coordinates");
    if (!texture_arrays) return std::unexpected(texture_arrays.error());
    std::vector<float> texture_scales;
    if (!texcoord_scales->is_null()) {
        auto loaded = read_float_array(
            machine, *texcoord_scales, "VertexBuffer texture scales");
        if (!loaded) return std::unexpected(loaded.error());
        texture_scales = std::move(*loaded);
    }
    std::vector<ObjectRef> texture_biases;
    if (!texcoord_biases->is_null()) {
        auto loaded = scene_reference_array(
            machine, *texcoord_biases, "VertexBuffer texture biases");
        if (!loaded) return std::unexpected(loaded.error());
        texture_biases = std::move(*loaded);
    }
    const usize active_units = std::min(
        texture_arrays->size(), kPickTextureUnitCount);
    for (usize unit = 0U; unit < active_units; ++unit) {
        const ObjectRef texcoord = (*texture_arrays)[unit];
        if (texcoord.is_null()) continue;
        auto texture_source = pick_vertex_array(machine, texcoord);
        if (!texture_source) return std::unexpected(texture_source.error());
        if (texture_source->vertex_count != source->vertex_count) {
            return fail(ErrorCode::invalid_state,
                        "texture coordinates do not match positions");
        }
        float texture_scale = 1.0F;
        if (unit < texture_scales.size()) {
            texture_scale = texture_scales[unit];
        }
        std::vector<float> texture_bias;
        if (unit < texture_biases.size()) {
            const ObjectRef bias_array = texture_biases[unit];
            if (!bias_array.is_null()) {
                auto loaded = read_float_array(machine, bias_array,
                                               "VertexBuffer texture bias");
                if (!loaded) return std::unexpected(loaded.error());
                texture_bias = std::move(*loaded);
            }
        }
        for (i32 vertex = 0; vertex < source->vertex_count; ++vertex) {
            const usize base = static_cast<usize>(vertex) *
                static_cast<usize>(texture_source->component_count);
            PickVertex& output = result[static_cast<usize>(vertex)];
            output.texture_s[unit] =
                texture_source->values[base] * texture_scale +
                (texture_bias.empty() ? 0.0F : texture_bias[0U]);
            output.texture_t[unit] =
                texture_source->values[base + 1U] * texture_scale +
                (texture_bias.size() < 2U ? 0.0F : texture_bias[1U]);
        }
    }
    return result;
}

[[nodiscard]] Result<bool> pick_has_vertex_attribute(
    Machine& machine,
    ObjectRef vertex_buffer,
    std::string_view field) {
    auto array = reference_field(machine, vertex_buffer, kVertexBuffer,
        field, "Ljavax/microedition/m3g/VertexArray;");
    if (!array) return std::unexpected(array.error());
    return !array->is_null();
}

[[nodiscard]] Result<std::vector<PickVertex>> morph_pick_vertices(
    Machine& machine,
    ObjectRef mesh,
    ObjectRef base_vertex_buffer) {
    auto base = pick_vertices(machine, base_vertex_buffer);
    if (!base) return std::unexpected(base.error());
    auto targets = reference_field(
        machine, mesh, "javax/microedition/m3g/MorphingMesh",
        "morphTargets", "[Ljavax/microedition/m3g/VertexBuffer;");
    auto weights = reference_field(
        machine, mesh, "javax/microedition/m3g/MorphingMesh",
        "weights", "[F");
    if (!targets) return std::unexpected(targets.error());
    if (!weights) return std::unexpected(weights.error());
    if (targets->is_null() || weights->is_null()) return *base;
    auto target_buffers = scene_reference_array(
        machine, *targets, "MorphingMesh pick targets");
    auto weight_values = read_float_array(machine, *weights,
                                          "MorphingMesh pick weights");
    if (!target_buffers) return std::unexpected(target_buffers.error());
    if (!weight_values) return std::unexpected(weight_values.error());
    const usize target_count = target_buffers->size();
    if (target_count != weight_values->size()) {
        return fail(ErrorCode::invalid_state,
                    "MorphingMesh target and weight counts differ");
    }
    usize position_target_count = 0U;
    for (usize index = 0U; index < target_count; ++index) {
        const ObjectRef target = (*target_buffers)[index];
        if (target.is_null()) {
            return fail_java("java/lang/IllegalStateException",
                             "MorphingMesh target is null");
        }
        auto present = pick_has_vertex_attribute(
            machine, target, "positions");
        if (!present) return std::unexpected(present.error());
        position_target_count += *present ? 1U : 0U;
    }
    if (position_target_count == 0U) return *base;
    if (position_target_count != target_count) {
        return fail_java(
            "java/lang/IllegalStateException",
            "MorphingMesh target positions are incompatible");
    }
    float weight_sum = 0.0F;
    for (const float weight : *weight_values) {
        if (!std::isfinite(weight)) {
            return fail_java("java/lang/IllegalStateException",
                             "MorphingMesh weight is not finite");
        }
        weight_sum += weight;
    }
    std::vector<PickVertex> result = *base;
    const float base_weight = 1.0F - weight_sum;
    for (usize vertex = 0U; vertex < result.size(); ++vertex) {
        result[vertex].position[0U] *= base_weight;
        result[vertex].position[1U] *= base_weight;
        result[vertex].position[2U] *= base_weight;
    }
    for (usize target_index = 0U; target_index < target_count;
         ++target_index) {
        auto target_vertices = pick_vertices(
            machine, (*target_buffers)[target_index]);
        if (!target_vertices) {
            return std::unexpected(target_vertices.error());
        }
        if (target_vertices->size() != result.size()) {
            return fail_java(
                "java/lang/IllegalStateException",
                "MorphingMesh target positions are incompatible");
        }
        const float weight = (*weight_values)[target_index];
        for (usize vertex = 0U; vertex < result.size(); ++vertex) {
            result[vertex].position[0U] +=
                (*target_vertices)[vertex].position[0U] * weight;
            result[vertex].position[1U] +=
                (*target_vertices)[vertex].position[1U] * weight;
            result[vertex].position[2U] +=
                (*target_vertices)[vertex].position[2U] * weight;
        }
    }
    return result;
}

[[nodiscard]] Result<std::vector<PickVertex>> skinned_pick_vertices(
    Machine& machine,
    ObjectRef mesh,
    ObjectRef base_vertex_buffer) {
    auto base = pick_vertices(machine, base_vertex_buffer);
    if (!base) return std::unexpected(base.error());
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
    if (bones->is_null()) return *base;
    auto bone_values = scene_reference_array(
        machine, *bones, "SkinnedMesh pick bones");
    auto rest_values = scene_reference_array(
        machine, *rest_transforms, "SkinnedMesh pick transforms");
    if (!bone_values) return std::unexpected(bone_values.error());
    if (!rest_values) return std::unexpected(rest_values.error());
    const usize bone_count = bone_values->size();
    if (bone_count == 0U) return *base;
    auto first_values = pick_int_array(machine, *first_vertices);
    auto count_values = pick_int_array(machine, *vertex_counts);
    auto weight_values = pick_int_array(machine, *weights);
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
    auto mesh_world = composite_matrix(machine, mesh);
    if (!mesh_world) return std::unexpected(mesh_world.error());
    auto inverse_mesh = inverse_matrix(*mesh_world);
    if (!inverse_mesh) return std::unexpected(inverse_mesh.error());
    std::vector<Vector3> accumulated(base->size());
    std::vector<float> accumulated_weights(base->size(), 0.0F);
    for (usize influence = 0U; influence < bone_count; ++influence) {
        const ObjectRef bone = (*bone_values)[influence];
        const ObjectRef rest_array = (*rest_values)[influence];
        if (bone.is_null() || rest_array.is_null()) continue;
        auto values = read_float_array(machine, rest_array,
                                       "SkinnedMesh rest transform");
        if (!values) return std::unexpected(values.error());
        if (values->size() != 16U) {
            return fail(ErrorCode::invalid_state,
                        "SkinnedMesh rest transform is truncated");
        }
        Matrix rest {};
        std::copy(values->begin(), values->end(), rest.begin());
        auto bone_world = composite_matrix(machine, bone);
        if (!bone_world) return std::unexpected(bone_world.error());
        const Matrix deformation = multiply(
            multiply(*inverse_mesh, *bone_world), rest);
        const i32 first = (*first_values)[influence];
        const i32 count = (*count_values)[influence];
        const i32 raw_weight = (*weight_values)[influence];
        if (first < 0 || count <= 0 || raw_weight <= 0 ||
            static_cast<i64>(first) + count >
                static_cast<i64>(base->size())) {
            return fail(ErrorCode::invalid_state,
                        "SkinnedMesh influence range is invalid");
        }
        const float weight = static_cast<float>(raw_weight);
        for (i32 index = first; index < first + count; ++index) {
            const Vector3 transformed = transform_vector3(
                deformation, (*base)[static_cast<usize>(index)].position,
                1.0F);
            accumulated[static_cast<usize>(index)][0U] +=
                transformed[0U] * weight;
            accumulated[static_cast<usize>(index)][1U] +=
                transformed[1U] * weight;
            accumulated[static_cast<usize>(index)][2U] +=
                transformed[2U] * weight;
            accumulated_weights[static_cast<usize>(index)] += weight;
        }
    }
    std::vector<PickVertex> result = *base;
    for (usize index = 0U; index < result.size(); ++index) {
        if (accumulated_weights[index] <= 0.0F) continue;
        const float inverse = 1.0F / accumulated_weights[index];
        result[index].position = {
            accumulated[index][0U] * inverse,
            accumulated[index][1U] * inverse,
            accumulated[index][2U] * inverse,
        };
    }
    return result;
}

[[nodiscard]] bool ray_triangle_intersection(
    Vector3 origin,
    Vector3 direction,
    Vector3 first,
    Vector3 second,
    Vector3 third,
    float& distance,
    float& second_weight,
    float& third_weight) noexcept {
    const Vector3 first_edge = subtract(second, first);
    const Vector3 second_edge = subtract(third, first);
    const Vector3 p = cross(direction, second_edge);
    const float determinant = dot(first_edge, p);
    if (!std::isfinite(determinant) ||
        std::abs(determinant) <= 1.0e-7F) {
        return false;
    }
    const float inverse = 1.0F / determinant;
    const Vector3 translated = subtract(origin, first);
    second_weight = dot(translated, p) * inverse;
    if (second_weight < 0.0F || second_weight > 1.0F) return false;
    const Vector3 q = cross(translated, first_edge);
    third_weight = dot(direction, q) * inverse;
    if (third_weight < 0.0F ||
        second_weight + third_weight > 1.0F) {
        return false;
    }
    distance = dot(second_edge, q) * inverse;
    return std::isfinite(distance) && distance >= 0.0F;
}

[[nodiscard]] Status pick_mesh(
    Machine& machine,
    ObjectRef mesh,
    const Matrix& transform,
    Vector3 origin,
    Vector3 direction,
    PickHit& nearest) {
    auto vertex_buffer = reference_field(machine, mesh, kMesh,
        "vertexBuffer", "Ljavax/microedition/m3g/VertexBuffer;");
    auto index_buffers = reference_field(machine, mesh, kMesh,
        "indexBuffers", "[Ljavax/microedition/m3g/IndexBuffer;");
    if (!vertex_buffer) return std::unexpected(vertex_buffer.error());
    if (!index_buffers) return std::unexpected(index_buffers.error());
    if (vertex_buffer->is_null() || index_buffers->is_null()) return {};
    auto class_name = machine.heap().class_name(mesh);
    if (!class_name) return std::unexpected(class_name.error());
    Result<std::vector<PickVertex>> vertices =
        *class_name == "javax/microedition/m3g/MorphingMesh"
            ? morph_pick_vertices(machine, mesh, *vertex_buffer)
            : *class_name == "javax/microedition/m3g/SkinnedMesh"
                ? skinned_pick_vertices(machine, mesh, *vertex_buffer)
                : pick_vertices(machine, *vertex_buffer);
    if (!vertices) return std::unexpected(vertices.error());
    if (vertices->empty()) return {};
    std::vector<Vector3> transformed(vertices->size());
    for (usize index = 0U; index < vertices->size(); ++index) {
        transformed[index] = transform_vector3(
            transform, (*vertices)[index].position, 1.0F);
    }
    auto submeshes = scene_reference_array(
        machine, *index_buffers, "Mesh index buffers");
    if (!submeshes) return std::unexpected(submeshes.error());
    for (usize submesh = 0U; submesh < submeshes->size(); ++submesh) {
        const ObjectRef index_buffer = (*submeshes)[submesh];
        if (index_buffer.is_null()) continue;
        auto index_array = reference_field(machine, index_buffer,
                                           kIndexBuffer, "indices", "[I");
        auto strip_array = reference_field(machine, index_buffer,
                                           kIndexBuffer, "stripLengths", "[I");
        if (!index_array) return std::unexpected(index_array.error());
        if (!strip_array) return std::unexpected(strip_array.error());
        if (index_array->is_null()) continue;
        auto indices = pick_int_array(machine, *index_array);
        if (!indices) return std::unexpected(indices.error());
        std::vector<i32> strip_lengths;
        if (strip_array->is_null()) {
            strip_lengths.push_back(static_cast<i32>(indices->size()));
        } else {
            auto lengths = pick_int_array(machine, *strip_array);
            if (!lengths) return std::unexpected(lengths.error());
            strip_lengths = std::move(*lengths);
        }
        usize offset = 0U;
        for (const i32 strip_length : strip_lengths) {
            if (strip_length < 3 ||
                offset + static_cast<usize>(strip_length) > indices->size()) {
                return fail(ErrorCode::invalid_state,
                            "IndexBuffer strip data is invalid");
            }
            for (i32 triangle = 0; triangle < strip_length - 2; ++triangle) {
                i32 first_index =
                    (*indices)[offset + static_cast<usize>(triangle)];
                i32 second_index =
                    (*indices)[offset + static_cast<usize>(triangle + 1)];
                const i32 third_index =
                    (*indices)[offset + static_cast<usize>(triangle + 2)];
                if ((triangle & 1) != 0) {
                    std::swap(first_index, second_index);
                }
                if (first_index < 0 || second_index < 0 || third_index < 0 ||
                    static_cast<usize>(first_index) >= vertices->size() ||
                    static_cast<usize>(second_index) >= vertices->size() ||
                    static_cast<usize>(third_index) >= vertices->size()) {
                    return fail_java("java/lang/IndexOutOfBoundsException",
                                     "IndexBuffer references an invalid vertex");
                }
                float distance = 0.0F;
                float second_weight = 0.0F;
                float third_weight = 0.0F;
                const Vector3& first =
                    transformed[static_cast<usize>(first_index)];
                const Vector3& second =
                    transformed[static_cast<usize>(second_index)];
                const Vector3& third =
                    transformed[static_cast<usize>(third_index)];
                if (!ray_triangle_intersection(
                        origin, direction, first, second, third,
                        distance, second_weight, third_weight) ||
                    distance >= nearest.distance) {
                    continue;
                }
                const float first_weight =
                    1.0F - second_weight - third_weight;
                const PickVertex& first_vertex =
                    (*vertices)[static_cast<usize>(first_index)];
                const PickVertex& second_vertex =
                    (*vertices)[static_cast<usize>(second_index)];
                const PickVertex& third_vertex =
                    (*vertices)[static_cast<usize>(third_index)];
                auto normal = normalize_vector(
                    cross(subtract(second, first), subtract(third, first)),
                    "picked triangle normal");
                if (!normal) continue;
                std::array<float, kPickTextureUnitCount> texture_s {};
                std::array<float, kPickTextureUnitCount> texture_t {};
                for (usize unit = 0U; unit < kPickTextureUnitCount; ++unit) {
                    texture_s[unit] =
                        first_vertex.texture_s[unit] * first_weight +
                        second_vertex.texture_s[unit] * second_weight +
                        third_vertex.texture_s[unit] * third_weight;
                    texture_t[unit] =
                        first_vertex.texture_t[unit] * first_weight +
                        second_vertex.texture_t[unit] * second_weight +
                        third_vertex.texture_t[unit] * third_weight;
                }
                nearest = PickHit {
                    .node = mesh,
                    .distance = distance,
                    .submesh_index = static_cast<i32>(submesh),
                    .texture_s = texture_s,
                    .texture_t = texture_t,
                    .normal = *normal,
                };
            }
            offset += static_cast<usize>(strip_length);
        }
    }
    return {};
}

[[nodiscard]] Status pick_subtree(
    Machine& machine,
    ObjectRef node,
    const Matrix& parent_transform,
    i32 mask,
    Vector3 origin,
    Vector3 direction,
    bool path_pickable,
    PickHit& nearest) {
    auto picking = int_field(machine, node, kNode, "pickingEnabled", "Z");
    if (!picking) return std::unexpected(picking.error());
    path_pickable = path_pickable && *picking != 0;
    if (!path_pickable) return {};
    auto local = local_transform(machine, node);
    if (!local) return std::unexpected(local.error());
    auto local_matrix = transform_matrix(machine, *local);
    if (!local_matrix) return std::unexpected(local_matrix.error());
    const Matrix transform = multiply(parent_transform, *local_matrix);
    auto class_name = machine.heap().class_name(node);
    if (!class_name) return std::unexpected(class_name.error());
    const bool mesh = *class_name == kMesh ||
        *class_name == "javax/microedition/m3g/MorphingMesh" ||
        *class_name == "javax/microedition/m3g/SkinnedMesh";
    if (mesh) {
        auto scope = int_field(machine, node, kNode, "scope");
        if (!scope) return std::unexpected(scope.error());
        if ((*scope & mask) != 0) {
            auto picked = pick_mesh(machine, node, transform,
                                    origin, direction, nearest);
            if (!picked) return picked;
        }
    }
    if (*class_name == kGroup || *class_name == kWorld) {
        auto children = reference_field(machine, node, kGroup,
                                        "children",
                                        "[Ljavax/microedition/m3g/Node;");
        auto child_count = int_field(machine, node, kGroup, "childCount");
        if (!children) return std::unexpected(children.error());
        if (!child_count) return std::unexpected(child_count.error());
        if (children->is_null()) return {};
        auto child_values = scene_reference_array(
            machine, *children, "Group children");
        if (!child_values) return std::unexpected(child_values.error());
        const usize count = std::min(
            child_values->size(), static_cast<usize>(std::max(*child_count, 0)));
        for (usize index = 0U; index < count; ++index) {
            const ObjectRef child = (*child_values)[index];
            if (child.is_null()) continue;
            auto picked = pick_subtree(
                machine, child, transform, mask,
                origin, direction, path_pickable, nearest);
            if (!picked) return picked;
        }
    }
    return {};
}

[[nodiscard]] Result<Matrix> pick_camera_projection(
    Machine& machine,
    ObjectRef camera) {
    auto type = int_field(machine, camera, kCamera, "projectionType");
    auto values = reference_field(machine, camera, kCamera,
                                  "projection", "[F");
    if (!type) return std::unexpected(type.error());
    if (!values) return std::unexpected(values.error());
    if (values->is_null()) {
        return fail_java("java/lang/IllegalStateException",
                         "Camera projection is not configured");
    }
    auto projection = read_float_array(machine, *values,
                                       "Camera projection");
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
        const float projection_scale = 1.0F /
                                       std::tan(radians * 0.5F);
        result[0U] = projection_scale / aspect;
        result[5U] = projection_scale;
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

[[nodiscard]] std::array<float, 4> transform_pick_point(
    const Matrix& matrix,
    std::array<float, 4> point) noexcept {
    std::array<float, 4> result {};
    for (usize row = 0U; row < 4U; ++row) {
        for (usize column = 0U; column < 4U; ++column) {
            result[row] += matrix[row * 4U + column] * point[column];
        }
    }
    return result;
}

[[nodiscard]] Result<std::array<Vector3, 2>> camera_pick_ray(
    Machine& machine,
    ObjectRef group,
    ObjectRef camera,
    float x,
    float y) {
    if (!std::isfinite(x) || !std::isfinite(y) ||
        x < 0.0F || x > 1.0F || y < 0.0F || y > 1.0F) {
        return fail_java("java/lang/IllegalArgumentException",
                         "pick coordinates are outside 0..1");
    }
    auto group_root = root_node(machine, group);
    auto camera_root = root_node(machine, camera);
    if (!group_root) return std::unexpected(group_root.error());
    if (!camera_root) return std::unexpected(camera_root.error());
    if (*group_root != *camera_root) {
        return fail_java("java/lang/IllegalArgumentException",
                         "pick camera is in another scene graph");
    }
    auto projection = pick_camera_projection(machine, camera);
    if (!projection) return std::unexpected(projection.error());
    auto inverse_projection = inverse_matrix(*projection);
    if (!inverse_projection) return std::unexpected(inverse_projection.error());
    auto group_world = composite_matrix(machine, group);
    auto camera_world = composite_matrix(machine, camera);
    if (!group_world) return std::unexpected(group_world.error());
    if (!camera_world) return std::unexpected(camera_world.error());
    auto inverse_group = inverse_matrix(*group_world);
    if (!inverse_group) return std::unexpected(inverse_group.error());
    const Matrix camera_to_group = multiply(*inverse_group, *camera_world);
    const float ndc_x = x * 2.0F - 1.0F;
    const float ndc_y = 1.0F - y * 2.0F;
    const auto unproject = [&](float ndc_z) -> Result<Vector3> {
        auto camera_point = transform_pick_point(
            *inverse_projection, {ndc_x, ndc_y, ndc_z, 1.0F});
        if (!std::isfinite(camera_point[3U]) ||
            std::abs(camera_point[3U]) <= 1.0e-7F) {
            return fail_java("java/lang/IllegalStateException",
                             "Camera projection cannot be unprojected");
        }
        const float inverse_w = 1.0F / camera_point[3U];
        const Vector3 point {
            camera_point[0U] * inverse_w,
            camera_point[1U] * inverse_w,
            camera_point[2U] * inverse_w,
        };
        return transform_vector3(camera_to_group, point, 1.0F);
    };
    auto near_point = unproject(-1.0F);
    auto far_point = unproject(1.0F);
    if (!near_point) return std::unexpected(near_point.error());
    if (!far_point) return std::unexpected(far_point.error());
    auto direction = normalize_vector(
        subtract(*far_point, *near_point), "camera pick ray");
    if (!direction) return std::unexpected(direction.error());
    return std::array<Vector3, 2> {*near_point, *direction};
}

[[nodiscard]] Status write_pick_result(
    Machine& machine,
    ObjectRef intersection,
    const PickHit& hit,
    Vector3 origin,
    Vector3 direction) {
    if (intersection.is_null()) return {};
    auto texture_s = reference_field(machine, intersection,
                                     kRayIntersection, "textureS", "[F");
    auto texture_t = reference_field(machine, intersection,
                                     kRayIntersection, "textureT", "[F");
    auto normal = reference_field(machine, intersection,
                                  kRayIntersection, "normal", "[F");
    auto ray = reference_field(machine, intersection,
                               kRayIntersection, "ray", "[F");
    if (!texture_s) return std::unexpected(texture_s.error());
    if (!texture_t) return std::unexpected(texture_t.error());
    if (!normal) return std::unexpected(normal.error());
    if (!ray) return std::unexpected(ray.error());
    if (texture_s->is_null() || texture_t->is_null() ||
        normal->is_null() || ray->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "RayIntersection arrays are not initialized");
    }
    const std::array<float, kPickTextureUnitCount> s = hit.texture_s;
    const std::array<float, kPickTextureUnitCount> t = hit.texture_t;
    const std::array<float, 3> normal_values {
        hit.normal[0U], hit.normal[1U], hit.normal[2U]};
    const std::array<float, 6> ray_values {
        origin[0U], origin[1U], origin[2U],
        direction[0U], direction[1U], direction[2U]};
    auto s_written = write_float_array(machine, *texture_s, s);
    auto t_written = write_float_array(machine, *texture_t, t);
    auto normal_written = write_float_array(machine, *normal, normal_values);
    auto ray_written = write_float_array(machine, *ray, ray_values);
    if (!s_written) return s_written;
    if (!t_written) return t_written;
    if (!normal_written) return normal_written;
    if (!ray_written) return ray_written;
    const std::array<Status, 3> stored {
        set_reference_field(machine, intersection, kRayIntersection,
                            "intersected",
                            "Ljavax/microedition/m3g/Node;", hit.node),
        set_float_field(machine, intersection, kRayIntersection,
                        "distance", hit.distance),
        set_int_field(machine, intersection, kRayIntersection,
                      "submeshIndex", hit.submesh_index),
    };
    for (const Status& status : stored) {
        if (!status) return status;
    }
    return {};
}

[[nodiscard]] Result<bool> pick_group_ray(
    Machine& machine,
    ObjectRef group,
    i32 mask,
    Vector3 origin,
    Vector3 direction,
    ObjectRef intersection) {
    auto normalized_direction = normalize_vector(direction, "pick ray");
    if (!normalized_direction) {
        return std::unexpected(normalized_direction.error());
    }
    auto group_picking = int_field(machine, group, kNode,
                                   "pickingEnabled", "Z");
    if (!group_picking) return std::unexpected(group_picking.error());
    if (*group_picking == 0 || mask == 0) return false;
    auto children = reference_field(machine, group, kGroup,
                                    "children",
                                    "[Ljavax/microedition/m3g/Node;");
    auto child_count = int_field(machine, group, kGroup, "childCount");
    if (!children) return std::unexpected(children.error());
    if (!child_count) return std::unexpected(child_count.error());
    PickHit nearest;
    if (!children->is_null()) {
        auto child_values = scene_reference_array(
            machine, *children, "Group children");
        if (!child_values) return std::unexpected(child_values.error());
        const usize count = std::min(
            child_values->size(), static_cast<usize>(std::max(*child_count, 0)));
        for (usize index = 0U; index < count; ++index) {
            const ObjectRef child = (*child_values)[index];
            if (child.is_null()) continue;
            auto picked = pick_subtree(
                machine, child, identity_matrix(), mask,
                origin, *normalized_direction, true, nearest);
            if (!picked) return std::unexpected(picked.error());
        }
    }
    if (nearest.node.is_null()) return false;
    auto written = write_pick_result(
        machine, intersection, nearest, origin, *normalized_direction);
    if (!written) return std::unexpected(written.error());
    return true;
}

void append_unique_reference(std::vector<ObjectRef>& references,
                             ObjectRef reference) {
    if (reference.is_null()) return;
    if (std::find(references.begin(), references.end(), reference) ==
        references.end()) {
        references.push_back(reference);
    }
}

[[nodiscard]] Status append_reference_field(
    Machine& machine,
    ObjectRef object,
    std::string_view owner,
    std::string_view name,
    std::string_view descriptor,
    std::vector<ObjectRef>& references) {
    auto reference = reference_field(machine, object, owner, name, descriptor);
    if (!reference) return std::unexpected(reference.error());
    append_unique_reference(references, *reference);
    return {};
}

[[nodiscard]] Status append_reference_array(
    Machine& machine,
    ObjectRef object,
    std::string_view owner,
    std::string_view name,
    std::string_view descriptor,
    usize maximum,
    std::vector<ObjectRef>& references) {
    auto array = reference_field(machine, object, owner, name, descriptor);
    if (!array) return std::unexpected(array.error());
    if (array->is_null()) return {};
    auto values = scene_reference_array(machine, *array, name);
    if (!values) return std::unexpected(values.error());
    const usize count = std::min(values->size(), maximum);
    for (usize index = 0U; index < count; ++index) {
        append_unique_reference(references, (*values)[index]);
    }
    return {};
}

[[nodiscard]] Result<std::vector<ObjectRef>> object_references(
    Machine& machine,
    ObjectRef object) {
    auto class_name = machine.heap().class_name(object);
    if (!class_name) return std::unexpected(class_name.error());
    const std::string_view type = *class_name;
    std::vector<ObjectRef> references;
    references.reserve(12U);

    auto animation_count = int_field(machine, object, kObject3D,
                                     "animationTrackCount");
    if (!animation_count) return std::unexpected(animation_count.error());
    const usize animations = *animation_count <= 0
        ? 0U : static_cast<usize>(*animation_count);
    auto appended = append_reference_array(
        machine, object, kObject3D, "animationTracks",
        "[Ljavax/microedition/m3g/AnimationTrack;", animations,
        references);
    if (!appended) return std::unexpected(appended.error());

    const bool node = type == kCamera || type == kLight || type == kGroup ||
        type == kWorld || type == kMesh ||
        type == "javax/microedition/m3g/MorphingMesh" ||
        type == "javax/microedition/m3g/SkinnedMesh" || type == kSprite3D;
    if (node) {
        appended = append_reference_field(
            machine, object, kNode, "zTarget",
            "Ljavax/microedition/m3g/Node;", references);
        if (!appended) return std::unexpected(appended.error());
        appended = append_reference_field(
            machine, object, kNode, "yTarget",
            "Ljavax/microedition/m3g/Node;", references);
        if (!appended) return std::unexpected(appended.error());
    }

    if (type == kGroup || type == kWorld) {
        auto child_count = int_field(machine, object, kGroup, "childCount");
        if (!child_count) return std::unexpected(child_count.error());
        const usize children = *child_count <= 0
            ? 0U : static_cast<usize>(*child_count);
        appended = append_reference_array(
            machine, object, kGroup, "children",
            "[Ljavax/microedition/m3g/Node;", children, references);
        if (!appended) return std::unexpected(appended.error());
    }
    if (type == kWorld) {
        appended = append_reference_field(
            machine, object, kWorld, "activeCamera",
            "Ljavax/microedition/m3g/Camera;", references);
        if (!appended) return std::unexpected(appended.error());
        appended = append_reference_field(
            machine, object, kWorld, "background",
            "Ljavax/microedition/m3g/Background;", references);
        if (!appended) return std::unexpected(appended.error());
    } else if (type == kBackground) {
        appended = append_reference_field(
            machine, object, kBackground, "image",
            "Ljavax/microedition/m3g/Image2D;", references);
        if (!appended) return std::unexpected(appended.error());
    } else if (type == kAppearance) {
        for (const auto& [name, descriptor] :
             std::array<std::pair<const char*, const char*>, 4> {{
                 {"compositingMode",
                  "Ljavax/microedition/m3g/CompositingMode;"},
                 {"fog", "Ljavax/microedition/m3g/Fog;"},
                 {"polygonMode", "Ljavax/microedition/m3g/PolygonMode;"},
                 {"material", "Ljavax/microedition/m3g/Material;"},
             }}) {
            appended = append_reference_field(
                machine, object, kAppearance, name, descriptor, references);
            if (!appended) return std::unexpected(appended.error());
        }
        appended = append_reference_array(
            machine, object, kAppearance, "textures",
            "[Ljavax/microedition/m3g/Texture2D;",
            std::numeric_limits<usize>::max(), references);
        if (!appended) return std::unexpected(appended.error());
    } else if (type == kTexture2D) {
        appended = append_reference_field(
            machine, object, kTexture2D, "image",
            "Ljavax/microedition/m3g/Image2D;", references);
        if (!appended) return std::unexpected(appended.error());
    } else if (type == kAnimationTrack) {
        appended = append_reference_field(
            machine, object, kAnimationTrack, "sequence",
            "Ljavax/microedition/m3g/KeyframeSequence;", references);
        if (!appended) return std::unexpected(appended.error());
        appended = append_reference_field(
            machine, object, kAnimationTrack, "controller",
            "Ljavax/microedition/m3g/AnimationController;", references);
        if (!appended) return std::unexpected(appended.error());
    }

    const bool mesh = type == kMesh ||
        type == "javax/microedition/m3g/MorphingMesh" ||
        type == "javax/microedition/m3g/SkinnedMesh";
    if (mesh) {
        appended = append_reference_field(
            machine, object, kMesh, "vertexBuffer",
            "Ljavax/microedition/m3g/VertexBuffer;", references);
        if (!appended) return std::unexpected(appended.error());
        appended = append_reference_array(
            machine, object, kMesh, "indexBuffers",
            "[Ljavax/microedition/m3g/IndexBuffer;",
            std::numeric_limits<usize>::max(), references);
        if (!appended) return std::unexpected(appended.error());
        appended = append_reference_array(
            machine, object, kMesh, "appearances",
            "[Ljavax/microedition/m3g/Appearance;",
            std::numeric_limits<usize>::max(), references);
        if (!appended) return std::unexpected(appended.error());
    }
    if (type == "javax/microedition/m3g/MorphingMesh") {
        appended = append_reference_array(
            machine, object, type, "morphTargets",
            "[Ljavax/microedition/m3g/VertexBuffer;",
            std::numeric_limits<usize>::max(), references);
        if (!appended) return std::unexpected(appended.error());
    } else if (type == "javax/microedition/m3g/SkinnedMesh") {
        appended = append_reference_field(
            machine, object, type, "skeleton",
            "Ljavax/microedition/m3g/Group;", references);
        if (!appended) return std::unexpected(appended.error());
        appended = append_reference_array(
            machine, object, type, "bones",
            "[Ljavax/microedition/m3g/Node;",
            std::numeric_limits<usize>::max(), references);
        if (!appended) return std::unexpected(appended.error());
    } else if (type == kSprite3D) {
        appended = append_reference_field(
            machine, object, kSprite3D, "image",
            "Ljavax/microedition/m3g/Image2D;", references);
        if (!appended) return std::unexpected(appended.error());
        appended = append_reference_field(
            machine, object, kSprite3D, "appearance",
            "Ljavax/microedition/m3g/Appearance;", references);
        if (!appended) return std::unexpected(appended.error());
    } else if (type == kVertexBuffer) {
        for (const char* name : {"positions", "normals", "colors"}) {
            appended = append_reference_field(
                machine, object, kVertexBuffer, name,
                "Ljavax/microedition/m3g/VertexArray;", references);
            if (!appended) return std::unexpected(appended.error());
        }
        appended = append_reference_array(
            machine, object, kVertexBuffer, "texCoords",
            "[Ljavax/microedition/m3g/VertexArray;",
            std::numeric_limits<usize>::max(), references);
        if (!appended) return std::unexpected(appended.error());
    }
    return references;
}

[[nodiscard]] Result<ObjectRef> find_by_user_id(
    Machine& machine,
    ObjectRef object,
    i32 user_id,
    std::unordered_set<u64>& visited,
    usize depth = 0U) {
    if (depth > 4'096U) {
        return fail(ErrorCode::overflow, "M3G object graph is too deep");
    }
    if (!visited.insert(object.bits).second) return ObjectRef {};
    auto current_id = int_field(machine, object, kObject3D, "userID");
    if (!current_id) return std::unexpected(current_id.error());
    if (*current_id == user_id) return object;
    auto references = object_references(machine, object);
    if (!references) return std::unexpected(references.error());
    for (ObjectRef reference : *references) {
        auto found = find_by_user_id(
            machine, reference, user_id, visited, depth + 1U);
        if (!found) return std::unexpected(found.error());
        if (!found->is_null()) return *found;
    }
    return ObjectRef {};
}

[[nodiscard]] Result<ObjectRef> find_by_user_id(Machine& machine,
                                                 ObjectRef object,
                                                 i32 user_id) {
    std::unordered_set<u64> visited;
    visited.reserve(64U);
    return find_by_user_id(machine, object, user_id, visited);
}

[[nodiscard]] Result<float> animation_controller_position(
    Machine& machine,
    ObjectRef controller,
    i32 world_time) {
    auto speed = float_field(machine, controller, kAnimationController, "speed");
    auto sequence_time = float_field(
        machine, controller, kAnimationController, "refSequenceTime");
    auto reference_time = int_field(
        machine, controller, kAnimationController, "refWorldTime");
    if (!speed) return std::unexpected(speed.error());
    if (!sequence_time) return std::unexpected(sequence_time.error());
    if (!reference_time) return std::unexpected(reference_time.error());
    return *sequence_time + *speed *
        static_cast<float>(world_time - *reference_time);
}

void register_animation_controller(NativeMethodRegistry& registry) {
    add(registry, kAnimationController, "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "AnimationController.<init>");
            if (!object) return std::unexpected(object.error());
            auto initialized = initialize_object3d(machine, *object);
            if (!initialized) return std::unexpected(initialized.error());
            const std::array<Status, 6> stored {
                set_int_field(machine, *object, kAnimationController,
                              "activeStart", std::numeric_limits<i32>::min()),
                set_int_field(machine, *object, kAnimationController,
                              "activeEnd", std::numeric_limits<i32>::max()),
                set_float_field(machine, *object, kAnimationController,
                                "speed", 1.0F),
                set_float_field(machine, *object, kAnimationController,
                                "weight", 1.0F),
                set_float_field(machine, *object, kAnimationController,
                                "refSequenceTime", 0.0F),
                set_int_field(machine, *object, kAnimationController,
                              "refWorldTime", 0),
            };
            for (const Status& status : stored) {
                if (!status) return std::unexpected(status.error());
            }
            return std::optional<Value> {};
        });
    add(registry, kAnimationController, "setActiveInterval", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments,
                                   "AnimationController.setActiveInterval");
            auto start = int_argument(arguments, 1U,
                                      "AnimationController.setActiveInterval");
            auto end = int_argument(arguments, 2U,
                                    "AnimationController.setActiveInterval");
            if (!object) return std::unexpected(object.error());
            if (!start) return std::unexpected(start.error());
            if (!end) return std::unexpected(end.error());
            if (*start > *end) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "animation active interval is reversed");
            }
            auto first = set_int_field(machine, *object, kAnimationController,
                                       "activeStart", *start);
            auto second = set_int_field(machine, *object, kAnimationController,
                                        "activeEnd", *end);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            return std::optional<Value> {};
        });
    for (const auto& [name, field] :
         std::array<std::pair<const char*, const char*>, 3> {{
             {"getActiveIntervalStart", "activeStart"},
             {"getActiveIntervalEnd", "activeEnd"},
             {"getRefWorldTime", "refWorldTime"},
         }}) {
        add(registry, kAnimationController, name, "()I",
            [name, field](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, name);
                if (!object) return std::unexpected(object.error());
                auto value = int_field(machine, *object,
                                       kAnimationController, field);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_int(*value));
            });
    }
    add(registry, kAnimationController, "setSpeed", "(FI)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "AnimationController.setSpeed");
            auto speed = float_argument(arguments, 1U,
                                        "AnimationController.setSpeed");
            auto world_time = int_argument(arguments, 2U,
                                           "AnimationController.setSpeed");
            if (!object) return std::unexpected(object.error());
            if (!speed) return std::unexpected(speed.error());
            if (!world_time) return std::unexpected(world_time.error());
            if (!std::isfinite(*speed)) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "animation speed is not finite");
            }
            auto position = animation_controller_position(
                machine, *object, *world_time);
            if (!position) return std::unexpected(position.error());
            const std::array<Status, 3> stored {
                set_float_field(machine, *object, kAnimationController,
                                "speed", *speed),
                set_float_field(machine, *object, kAnimationController,
                                "refSequenceTime", *position),
                set_int_field(machine, *object, kAnimationController,
                              "refWorldTime", *world_time),
            };
            for (const Status& status : stored) {
                if (!status) return std::unexpected(status.error());
            }
            return std::optional<Value> {};
        });
    add(registry, kAnimationController, "getSpeed", "()F",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "AnimationController.getSpeed");
            if (!object) return std::unexpected(object.error());
            auto speed = float_field(machine, *object,
                                     kAnimationController, "speed");
            if (!speed) return std::unexpected(speed.error());
            return std::optional<Value>(Value::from_float(*speed));
        });
    add(registry, kAnimationController, "setPosition", "(FI)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments,
                                   "AnimationController.setPosition");
            auto position = float_argument(arguments, 1U,
                                           "AnimationController.setPosition");
            auto world_time = int_argument(arguments, 2U,
                                           "AnimationController.setPosition");
            if (!object) return std::unexpected(object.error());
            if (!position) return std::unexpected(position.error());
            if (!world_time) return std::unexpected(world_time.error());
            if (!std::isfinite(*position)) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "animation position is not finite");
            }
            auto first = set_float_field(machine, *object,
                kAnimationController, "refSequenceTime", *position);
            auto second = set_int_field(machine, *object,
                kAnimationController, "refWorldTime", *world_time);
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            return std::optional<Value> {};
        });
    add(registry, kAnimationController, "getPosition", "(I)F",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments,
                                   "AnimationController.getPosition");
            auto world_time = int_argument(arguments, 1U,
                                           "AnimationController.getPosition");
            if (!object) return std::unexpected(object.error());
            if (!world_time) return std::unexpected(world_time.error());
            auto position = animation_controller_position(
                machine, *object, *world_time);
            if (!position) return std::unexpected(position.error());
            return std::optional<Value>(Value::from_float(*position));
        });
    add(registry, kAnimationController, "setWeight", "(F)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "AnimationController.setWeight");
            auto weight = float_argument(arguments, 1U,
                                         "AnimationController.setWeight");
            if (!object) return std::unexpected(object.error());
            if (!weight) return std::unexpected(weight.error());
            if (!std::isfinite(*weight) || *weight < 0.0F || *weight > 1.0F) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "animation weight is outside 0..1");
            }
            auto stored = set_float_field(machine, *object,
                kAnimationController, "weight", *weight);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, kAnimationController, "getWeight", "()F",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "AnimationController.getWeight");
            if (!object) return std::unexpected(object.error());
            auto weight = float_field(machine, *object,
                                      kAnimationController, "weight");
            if (!weight) return std::unexpected(weight.error());
            return std::optional<Value>(Value::from_float(*weight));
        });
}

void register_keyframe_sequence(NativeMethodRegistry& registry) {
    add(registry, kKeyframeSequence, "<init>", "(III)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "KeyframeSequence.<init>");
            auto keyframes = int_argument(arguments, 1U,
                                          "KeyframeSequence.<init>");
            auto components = int_argument(arguments, 2U,
                                           "KeyframeSequence.<init>");
            auto interpolation = int_argument(arguments, 3U,
                                              "KeyframeSequence.<init>");
            if (!object) return std::unexpected(object.error());
            if (!keyframes) return std::unexpected(keyframes.error());
            if (!components) return std::unexpected(components.error());
            if (!interpolation) return std::unexpected(interpolation.error());
            if (*keyframes <= 0 || *components <= 0 ||
                *interpolation < 176 || *interpolation > 180) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "keyframe sequence dimensions or interpolation are invalid");
            }
            const u64 value_count = static_cast<u64>(*keyframes) *
                                    static_cast<u64>(*components);
            if (value_count > static_cast<u64>(
                    std::numeric_limits<usize>::max())) {
                return fail_java("java/lang/OutOfMemoryError",
                                 "keyframe sequence is too large");
            }
            auto initialized = initialize_object3d(machine, *object);
            if (!initialized) return std::unexpected(initialized.error());
            auto times = allocate_array(machine, "[I",
                static_cast<usize>(*keyframes), Value::from_int(0));
            auto values = allocate_array(machine, "[F",
                static_cast<usize>(value_count), Value::from_float(0.0F));
            if (!times) return std::unexpected(times.error());
            if (!values) return std::unexpected(values.error());
            const std::array<Status, 9> stored {
                set_int_field(machine, *object, kKeyframeSequence,
                              "keyframeCount", *keyframes),
                set_int_field(machine, *object, kKeyframeSequence,
                              "componentCount", *components),
                set_int_field(machine, *object, kKeyframeSequence,
                              "interpolationType", *interpolation),
                set_int_field(machine, *object, kKeyframeSequence,
                              "validFirst", 0),
                set_int_field(machine, *object, kKeyframeSequence,
                              "validLast", *keyframes - 1),
                set_int_field(machine, *object, kKeyframeSequence,
                              "duration", 0),
                set_int_field(machine, *object, kKeyframeSequence,
                              "repeatMode", 192),
                set_reference_field(machine, *object, kKeyframeSequence,
                                    "times", "[I", *times),
                set_reference_field(machine, *object, kKeyframeSequence,
                                    "values", "[F", *values),
            };
            for (const Status& status : stored) {
                if (!status) return std::unexpected(status.error());
            }
            return std::optional<Value> {};
        });
    add(registry, kKeyframeSequence, "setKeyframe", "(II[F)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "KeyframeSequence.setKeyframe");
            auto index = int_argument(arguments, 1U,
                                      "KeyframeSequence.setKeyframe");
            auto time = int_argument(arguments, 2U,
                                     "KeyframeSequence.setKeyframe");
            auto source = reference_argument(arguments, 3U,
                                             "KeyframeSequence.setKeyframe", false);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            if (!time) return std::unexpected(time.error());
            if (!source) return std::unexpected(source.error());
            auto keyframes = int_field(machine, *object,
                                       kKeyframeSequence, "keyframeCount");
            auto components = int_field(machine, *object,
                                        kKeyframeSequence, "componentCount");
            auto times = reference_field(machine, *object,
                                         kKeyframeSequence, "times", "[I");
            auto values = reference_field(machine, *object,
                                          kKeyframeSequence, "values", "[F");
            if (!keyframes) return std::unexpected(keyframes.error());
            if (!components) return std::unexpected(components.error());
            if (!times) return std::unexpected(times.error());
            if (!values) return std::unexpected(values.error());
            if (*index < 0 || *index >= *keyframes || *time < 0) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "keyframe index or time is invalid");
            }
            auto source_length = machine.heap().array_length(*source);
            if (!source_length) return std::unexpected(source_length.error());
            if (*source_length < static_cast<usize>(*components)) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "keyframe value array is too short");
            }
            auto time_stored = machine.heap().set_element(
                *times, static_cast<usize>(*index), Value::from_int(*time));
            if (!time_stored) return std::unexpected(time_stored.error());
            const usize base = static_cast<usize>(*index) *
                               static_cast<usize>(*components);
            auto copied = machine.heap().copy_array_range(
                *source, 0U, *values, base, static_cast<usize>(*components));
            if (!copied) return std::unexpected(copied.error());
            return std::optional<Value> {};
        });
    add(registry, kKeyframeSequence, "getKeyframe", "(I[F)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "KeyframeSequence.getKeyframe");
            auto index = int_argument(arguments, 1U,
                                      "KeyframeSequence.getKeyframe");
            auto destination = reference_argument(
                arguments, 2U, "KeyframeSequence.getKeyframe", true);
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            if (!destination) return std::unexpected(destination.error());
            auto keyframes = int_field(machine, *object,
                                       kKeyframeSequence, "keyframeCount");
            auto components = int_field(machine, *object,
                                        kKeyframeSequence, "componentCount");
            auto times = reference_field(machine, *object,
                                         kKeyframeSequence, "times", "[I");
            auto values = reference_field(machine, *object,
                                          kKeyframeSequence, "values", "[F");
            if (!keyframes) return std::unexpected(keyframes.error());
            if (!components) return std::unexpected(components.error());
            if (!times) return std::unexpected(times.error());
            if (!values) return std::unexpected(values.error());
            if (*index < 0 || *index >= *keyframes) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "keyframe index is out of range");
            }
            if (!destination->is_null()) {
                auto length = machine.heap().array_length(*destination);
                if (!length) return std::unexpected(length.error());
                if (*length < static_cast<usize>(*components)) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "keyframe destination is too short");
                }
                const usize base = static_cast<usize>(*index) *
                                   static_cast<usize>(*components);
                auto copied = machine.heap().copy_array_range(
                    *values, base, *destination, 0U,
                    static_cast<usize>(*components));
                if (!copied) return std::unexpected(copied.error());
            }
            auto time = machine.heap().element(
                *times, static_cast<usize>(*index));
            if (!time) return std::unexpected(time.error());
            return std::optional<Value>(*time);
        });
    for (const auto& [name, field] :
         std::array<std::pair<const char*, const char*>, 7> {{
             {"getKeyframeCount", "keyframeCount"},
             {"getComponentCount", "componentCount"},
             {"getInterpolationType", "interpolationType"},
             {"getValidRangeFirst", "validFirst"},
             {"getValidRangeLast", "validLast"},
             {"getDuration", "duration"},
             {"getRepeatMode", "repeatMode"},
         }}) {
        add(registry, kKeyframeSequence, name, "()I",
            [name, field](Machine& machine,
                          std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, name);
                if (!object) return std::unexpected(object.error());
                auto value = int_field(machine, *object,
                                       kKeyframeSequence, field);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_int(*value));
            });
    }
    add(registry, kKeyframeSequence, "setValidRange", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "KeyframeSequence.setValidRange");
            auto first = int_argument(arguments, 1U,
                                      "KeyframeSequence.setValidRange");
            auto last = int_argument(arguments, 2U,
                                     "KeyframeSequence.setValidRange");
            if (!object) return std::unexpected(object.error());
            if (!first) return std::unexpected(first.error());
            if (!last) return std::unexpected(last.error());
            auto count = int_field(machine, *object,
                                   kKeyframeSequence, "keyframeCount");
            if (!count) return std::unexpected(count.error());
            if (*first < 0 || *last < *first || *last >= *count) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "keyframe valid range is invalid");
            }
            auto first_stored = set_int_field(machine, *object,
                kKeyframeSequence, "validFirst", *first);
            auto last_stored = set_int_field(machine, *object,
                kKeyframeSequence, "validLast", *last);
            if (!first_stored) return std::unexpected(first_stored.error());
            if (!last_stored) return std::unexpected(last_stored.error());
            return std::optional<Value> {};
        });
    add(registry, kKeyframeSequence, "setDuration", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "KeyframeSequence.setDuration");
            auto duration = int_argument(arguments, 1U,
                                         "KeyframeSequence.setDuration");
            if (!object) return std::unexpected(object.error());
            if (!duration) return std::unexpected(duration.error());
            if (*duration <= 0) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "keyframe duration must be positive");
            }
            auto stored = set_int_field(machine, *object,
                kKeyframeSequence, "duration", *duration);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, kKeyframeSequence, "setRepeatMode", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "KeyframeSequence.setRepeatMode");
            auto mode = int_argument(arguments, 1U,
                                     "KeyframeSequence.setRepeatMode");
            if (!object) return std::unexpected(object.error());
            if (!mode) return std::unexpected(mode.error());
            if (*mode != 192 && *mode != 193) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "keyframe repeat mode is invalid");
            }
            auto stored = set_int_field(machine, *object,
                kKeyframeSequence, "repeatMode", *mode);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
}

void register_animation_track(NativeMethodRegistry& registry) {
    add(registry, kAnimationTrack, "<init>",
        "(Ljavax/microedition/m3g/KeyframeSequence;I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "AnimationTrack.<init>");
            auto sequence = reference_argument(arguments, 1U,
                                               "AnimationTrack.<init>", false);
            auto property = int_argument(arguments, 2U,
                                         "AnimationTrack.<init>");
            if (!object) return std::unexpected(object.error());
            if (!sequence) return std::unexpected(sequence.error());
            if (!property) return std::unexpected(property.error());
            if (*property < 256 || *property > 276) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "animation target property is invalid");
            }
            auto initialized = initialize_object3d(machine, *object);
            if (!initialized) return std::unexpected(initialized.error());
            auto sequence_stored = set_reference_field(machine, *object,
                kAnimationTrack, "sequence",
                "Ljavax/microedition/m3g/KeyframeSequence;", *sequence);
            auto property_stored = set_int_field(machine, *object,
                kAnimationTrack, "property", *property);
            if (!sequence_stored) {
                return std::unexpected(sequence_stored.error());
            }
            if (!property_stored) {
                return std::unexpected(property_stored.error());
            }
            return std::optional<Value> {};
        });
    register_reference_property(registry, kAnimationTrack, kAnimationTrack,
        "sequence", "Ljavax/microedition/m3g/KeyframeSequence;",
        "setSequenceInternal", "getKeyframeSequence");
    register_reference_property(registry, kAnimationTrack, kAnimationTrack,
        "controller", "Ljavax/microedition/m3g/AnimationController;",
        "setController", "getController");
    register_int_property(registry, kAnimationTrack, kAnimationTrack,
                          "property", "setPropertyInternal",
                          "getTargetProperty");
}

struct AnimationSample final {
    i32 property {0};
    float weight {0.0F};
    i32 validity {std::numeric_limits<i32>::max()};
    std::vector<float> values;
};

[[nodiscard]] bool is_transformable_class(std::string_view name) noexcept {
    return name == kCamera || name == kGroup || name == kLight ||
        name == kMesh || name == "javax/microedition/m3g/MorphingMesh" ||
        name == "javax/microedition/m3g/SkinnedMesh" ||
        name == kSprite3D || name == kTexture2D || name == kWorld;
}

[[nodiscard]] bool is_node_class(std::string_view name) noexcept {
    return name == kCamera || name == kGroup || name == kLight ||
        name == kMesh || name == "javax/microedition/m3g/MorphingMesh" ||
        name == "javax/microedition/m3g/SkinnedMesh" ||
        name == kSprite3D || name == kWorld;
}

[[nodiscard]] Result<std::optional<AnimationSample>> sample_animation_track(
    Machine& machine,
    ObjectRef track,
    i32 world_time) {
    constexpr std::array<ObjectFieldSpec, 3> track_fields {{
        {"sequence", "Ljavax/microedition/m3g/KeyframeSequence;"},
        {"controller", "Ljavax/microedition/m3g/AnimationController;"},
        {"property", "I"},
    }};
    auto track_state = object_fields(
        machine, track, kAnimationTrack, track_fields);
    if (!track_state) return std::unexpected(track_state.error());
    auto sequence = (*track_state)[0U].as_reference();
    auto controller = (*track_state)[1U].as_reference();
    auto property = (*track_state)[2U].as_int();
    if (!sequence) return std::unexpected(sequence.error());
    if (!controller) return std::unexpected(controller.error());
    if (!property) return std::unexpected(property.error());
    if (sequence->is_null() || controller->is_null()) {
        return std::optional<AnimationSample> {};
    }
    constexpr std::array<ObjectFieldSpec, 4> controller_fields {{
        {"activeStart", "I"},
        {"activeEnd", "I"},
        {"speed", "F"},
        {"weight", "F"},
    }};
    auto controller_state = object_fields(
        machine, *controller, kAnimationController, controller_fields);
    if (!controller_state) return std::unexpected(controller_state.error());
    auto active_start = (*controller_state)[0U].as_int();
    auto active_end = (*controller_state)[1U].as_int();
    auto speed = (*controller_state)[2U].as_float();
    auto weight = (*controller_state)[3U].as_float();
    if (!active_start) return std::unexpected(active_start.error());
    if (!active_end) return std::unexpected(active_end.error());
    if (!speed) return std::unexpected(speed.error());
    if (!weight) return std::unexpected(weight.error());
    if (world_time < *active_start || world_time > *active_end ||
        *weight <= 0.0F) {
        i32 validity = std::numeric_limits<i32>::max();
        if (world_time < *active_start) validity = *active_start - world_time;
        return std::optional<AnimationSample>(AnimationSample {
            .property = *property,
            .weight = 0.0F,
            .validity = validity,
            .values = {},
        });
    }

    auto position = animation_controller_position(
        machine, *controller, world_time);
    constexpr std::array<ObjectFieldSpec, 9> sequence_fields {{
        {"keyframeCount", "I"},
        {"componentCount", "I"},
        {"interpolationType", "I"},
        {"validFirst", "I"},
        {"validLast", "I"},
        {"duration", "I"},
        {"repeatMode", "I"},
        {"times", "[I"},
        {"values", "[F"},
    }};
    auto sequence_state = object_fields(
        machine, *sequence, kKeyframeSequence, sequence_fields);
    if (!sequence_state) return std::unexpected(sequence_state.error());
    auto keyframe_count = (*sequence_state)[0U].as_int();
    auto component_count = (*sequence_state)[1U].as_int();
    auto interpolation = (*sequence_state)[2U].as_int();
    auto valid_first = (*sequence_state)[3U].as_int();
    auto valid_last = (*sequence_state)[4U].as_int();
    auto duration = (*sequence_state)[5U].as_int();
    auto repeat_mode = (*sequence_state)[6U].as_int();
    auto times = (*sequence_state)[7U].as_reference();
    auto values = (*sequence_state)[8U].as_reference();
    if (!position) return std::unexpected(position.error());
    if (!keyframe_count) return std::unexpected(keyframe_count.error());
    if (!component_count) return std::unexpected(component_count.error());
    if (!interpolation) return std::unexpected(interpolation.error());
    if (!valid_first) return std::unexpected(valid_first.error());
    if (!valid_last) return std::unexpected(valid_last.error());
    if (!duration) return std::unexpected(duration.error());
    if (!repeat_mode) return std::unexpected(repeat_mode.error());
    if (!times) return std::unexpected(times.error());
    if (!values) return std::unexpected(values.error());
    if (*keyframe_count <= 0 || *component_count <= 0 ||
        *valid_first < 0 || *valid_last < *valid_first ||
        *valid_last >= *keyframe_count || times->is_null() ||
        values->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "animation sequence state is invalid");
    }
    auto key_times = machine.heap().read_int_array(*times);
    auto key_values = read_float_array(
        machine, *values, "KeyframeSequence values");
    if (!key_times) return std::unexpected(key_times.error());
    if (!key_values) return std::unexpected(key_values.error());
    const u64 required_values = static_cast<u64>(*keyframe_count) *
                                static_cast<u64>(*component_count);
    if (key_times->size() < static_cast<usize>(*keyframe_count) ||
        required_values > static_cast<u64>(key_values->size())) {
        return fail(ErrorCode::invalid_state,
                    "animation sequence arrays are truncated");
    }

    float sequence_time = *position;
    if (*duration > 0) {
        if (*repeat_mode == 193) {
            sequence_time = std::fmod(sequence_time,
                                      static_cast<float>(*duration));
            if (sequence_time < 0.0F) {
                sequence_time += static_cast<float>(*duration);
            }
        } else {
            sequence_time = std::clamp(sequence_time, 0.0F,
                                       static_cast<float>(*duration));
        }
    }

    const auto key_time = [&](i32 index) -> i32 {
        return (*key_times)[static_cast<usize>(index)];
    };
    i32 left = *valid_first;
    i32 right = *valid_first;
    const i32 first_time = key_time(*valid_first);
    const i32 last_time = key_time(*valid_last);
    if (sequence_time <= static_cast<float>(first_time)) {
        left = right = *valid_first;
    } else if (sequence_time >= static_cast<float>(last_time)) {
        left = right = *valid_last;
    } else {
        for (i32 index = *valid_first; index < *valid_last; ++index) {
            const i32 next_time = key_time(index + 1);
            if (sequence_time <= static_cast<float>(next_time)) {
                left = index;
                right = index + 1;
                break;
            }
        }
    }
    const i32 left_time = key_time(left);
    const i32 right_time = key_time(right);
    float fraction = 0.0F;
    if (left != right && *interpolation != 180 &&
        right_time > left_time) {
        fraction = std::clamp(
            (sequence_time - static_cast<float>(left_time)) /
                static_cast<float>(right_time - left_time),
            0.0F, 1.0F);
    }
    std::vector<float> sampled(static_cast<usize>(*component_count), 0.0F);
    const usize left_base = static_cast<usize>(left) *
                            static_cast<usize>(*component_count);
    const usize right_base = static_cast<usize>(right) *
                             static_cast<usize>(*component_count);
    for (i32 component = 0; component < *component_count; ++component) {
        const usize component_index = static_cast<usize>(component);
        const float first_value = (*key_values)[left_base + component_index];
        const float second_value = (*key_values)[right_base + component_index];
        sampled[static_cast<usize>(component)] =
            first_value + (second_value - first_value) * fraction;
    }
    if ((*interpolation == 177 || *interpolation == 179) &&
        sampled.size() == 4U) {
        float length = 0.0F;
        for (float value : sampled) length += value * value;
        length = std::sqrt(length);
        if (length > 1.0e-7F) {
            for (float& value : sampled) value /= length;
        }
    }
    i32 validity = 1;
    if (*interpolation == 180 && right != left &&
        std::abs(*speed) > 1.0e-7F) {
        const float sequence_delta =
            static_cast<float>(right_time) - sequence_time;
        validity = std::max(1, static_cast<i32>(std::ceil(
            std::abs(sequence_delta / *speed))));
    }
    if (world_time < *active_end) {
        validity = std::min(validity, *active_end - world_time + 1);
    }
    return std::optional<AnimationSample>(AnimationSample {
        .property = *property,
        .weight = *weight,
        .validity = validity,
        .values = std::move(sampled),
    });
}

[[nodiscard]] float weighted_value(float current,
                                   float target,
                                   float weight) noexcept {
    return current + (target - current) * std::clamp(weight, 0.0F, 1.0F);
}

[[nodiscard]] i32 animation_color(std::span<const float> values,
                                  bool alpha) noexcept {
    const auto channel = [&](usize index, float fallback) -> u32 {
        const float value = index < values.size() ? values[index] : fallback;
        return static_cast<u32>(std::lround(
            std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    const u32 red = channel(0U, 0.0F);
    const u32 green = channel(1U, red / 255.0F);
    const u32 blue = channel(2U, green / 255.0F);
    const u32 opacity = alpha ? channel(3U, 1.0F) : 255U;
    return static_cast<i32>((opacity << 24U) | (red << 16U) |
                            (green << 8U) | blue);
}

[[nodiscard]] Status apply_animation_sample(Machine& machine,
                                            ObjectRef object,
                                            const AnimationSample& sample) {
    if (sample.values.empty() || sample.weight <= 0.0F) return {};
    auto class_name = machine.heap().class_name(object);
    if (!class_name) return std::unexpected(class_name.error());
    const auto value = [&](usize index, float fallback = 0.0F) {
        return index < sample.values.size() ? sample.values[index] : fallback;
    };
    if (sample.property == 275 && is_transformable_class(*class_name)) {
        const std::array<const char*, 3> fields {
            "translationX", "translationY", "translationZ"};
        for (usize index = 0U; index < fields.size(); ++index) {
            auto current = float_field(machine, object,
                                       kTransformable, fields[index]);
            if (!current) return std::unexpected(current.error());
            auto stored = set_float_field(machine, object, kTransformable,
                fields[index], weighted_value(*current, value(index, *current),
                                              sample.weight));
            if (!stored) return stored;
        }
        return rebuild_transformable_matrix(machine, object);
    }
    if (sample.property == 270 && is_transformable_class(*class_name)) {
        const std::array<const char*, 3> fields {"scaleX", "scaleY", "scaleZ"};
        for (usize index = 0U; index < fields.size(); ++index) {
            auto current = float_field(machine, object,
                                       kTransformable, fields[index]);
            if (!current) return std::unexpected(current.error());
            auto stored = set_float_field(machine, object, kTransformable,
                fields[index], weighted_value(*current, value(index, *current),
                                              sample.weight));
            if (!stored) return stored;
        }
        return rebuild_transformable_matrix(machine, object);
    }
    if (sample.property == 268 && is_transformable_class(*class_name) &&
        sample.values.size() >= 4U) {
        Quaternion target {
            sample.values[0U], sample.values[1U],
            sample.values[2U], sample.values[3U]};
        auto normalized = normalized_quaternion(target);
        if (!normalized) return std::unexpected(normalized.error());
        auto current = transformable_quaternion(machine, object);
        if (!current) return std::unexpected(current.error());
        float dot = current->x * normalized->x +
                    current->y * normalized->y +
                    current->z * normalized->z +
                    current->w * normalized->w;
        if (dot < 0.0F) {
            normalized->x = -normalized->x;
            normalized->y = -normalized->y;
            normalized->z = -normalized->z;
            normalized->w = -normalized->w;
        }
        Quaternion blended {
            weighted_value(current->x, normalized->x, sample.weight),
            weighted_value(current->y, normalized->y, sample.weight),
            weighted_value(current->z, normalized->z, sample.weight),
            weighted_value(current->w, normalized->w, sample.weight),
        };
        auto blended_normalized = normalized_quaternion(blended);
        if (!blended_normalized) {
            return std::unexpected(blended_normalized.error());
        }
        auto stored = set_transformable_quaternion(
            machine, object, *blended_normalized);
        if (!stored) return stored;
        return rebuild_transformable_matrix(machine, object);
    }
    if (sample.property == 256 && is_node_class(*class_name)) {
        auto current = float_field(machine, object, kNode, "alphaFactor");
        if (!current) return std::unexpected(current.error());
        return set_float_field(machine, object, kNode, "alphaFactor",
            std::clamp(weighted_value(*current, value(0U), sample.weight),
                       0.0F, 1.0F));
    }
    if ((sample.property == 269 || sample.property == 276) &&
        is_node_class(*class_name)) {
        return set_int_field(machine, object, kNode,
            sample.property == 269 ? "pickingEnabled" : "renderingEnabled",
            value(0U) >= 0.5F ? 1 : 0, "Z");
    }
    if (sample.property == 258) {
        const i32 color = animation_color(sample.values, false);
        if (*class_name == kBackground) {
            return set_int_field(machine, object, kBackground, "color", color);
        }
        if (*class_name == kFog) {
            return set_int_field(machine, object, kFog, "color", color);
        }
        if (*class_name == kLight) {
            return set_int_field(machine, object, kLight, "color", color);
        }
    }
    if (*class_name == kMaterial) {
        if (sample.property == 257 || sample.property == 261 ||
            sample.property == 262 || sample.property == 272) {
            const char* field = sample.property == 257 ? "ambient"
                : sample.property == 261 ? "diffuse"
                : sample.property == 262 ? "emissive" : "specular";
            return set_int_field(machine, object, kMaterial, field,
                animation_color(sample.values, sample.property == 261));
        }
        if (sample.property == 271) {
            auto current = float_field(machine, object, kMaterial, "shininess");
            if (!current) return std::unexpected(current.error());
            return set_float_field(machine, object, kMaterial, "shininess",
                weighted_value(*current, value(0U), sample.weight));
        }
    }
    if (*class_name == kLight) {
        const char* field = sample.property == 265 ? "intensity"
            : sample.property == 273 ? "spotAngle"
            : sample.property == 274 ? "spotExponent" : nullptr;
        if (field != nullptr) {
            auto current = float_field(machine, object, kLight, field);
            if (!current) return std::unexpected(current.error());
            return set_float_field(machine, object, kLight, field,
                weighted_value(*current, value(0U), sample.weight));
        }
    }
    if (*class_name == kFog) {
        const char* field = sample.property == 260 ? "density"
            : sample.property == 267 ? "nearDistance"
            : sample.property == 263 ? "farDistance" : nullptr;
        if (field != nullptr) {
            auto current = float_field(machine, object, kFog, field);
            if (!current) return std::unexpected(current.error());
            return set_float_field(machine, object, kFog, field,
                weighted_value(*current, value(0U), sample.weight));
        }
    }
    if (*class_name == kCamera && sample.property == 264) {
        auto projection = reference_field(machine, object, kCamera,
                                           "projection", "[F");
        if (!projection) return std::unexpected(projection.error());
        if (projection->is_null()) return {};
        auto current = machine.heap().element(*projection, 0U);
        if (!current) return std::unexpected(current.error());
        auto current_float = current->as_float();
        if (!current_float) return std::unexpected(current_float.error());
        return machine.heap().set_element(*projection, 0U,
            Value::from_float(weighted_value(
                *current_float, value(0U), sample.weight)));
    }
    if (*class_name == kBackground && sample.property == 259 &&
        sample.values.size() >= 4U) {
        const std::array<const char*, 4> fields {
            "cropX", "cropY", "cropWidth", "cropHeight"};
        for (usize index = 0U; index < fields.size(); ++index) {
            auto stored = set_int_field(machine, object, kBackground,
                fields[index], static_cast<i32>(std::lround(value(index))));
            if (!stored) return stored;
        }
        return {};
    }
    if (*class_name == "javax/microedition/m3g/MorphingMesh" &&
        sample.property == 266) {
        auto weights = reference_field(machine, object,
            "javax/microedition/m3g/MorphingMesh", "weights", "[F");
        if (!weights) return std::unexpected(weights.error());
        if (weights->is_null()) return {};
        auto current_values = read_float_array(
            machine, *weights, "MorphingMesh weights");
        if (!current_values) return std::unexpected(current_values.error());
        const usize count = std::min(current_values->size(), sample.values.size());
        for (usize index = 0U; index < count; ++index) {
            (*current_values)[index] = weighted_value(
                (*current_values)[index], sample.values[index], sample.weight);
        }
        auto stored = machine.heap().write_float_array(
            *weights, 0U, std::span<const float>(current_values->data(), count));
        if (!stored) return stored;
    }
    return {};
}

[[nodiscard]] Result<std::vector<ObjectRef>> active_animation_tracks(
    Machine& machine,
    ObjectRef object,
    i32 count) {
    if (count <= 0) return std::vector<ObjectRef> {};
    auto tracks = animation_array(machine, object);
    if (!tracks) return std::unexpected(tracks.error());
    auto values = scene_reference_array(
        machine, *tracks, "Object3D animation tracks");
    if (!values) return std::unexpected(values.error());
    if (values->size() < static_cast<usize>(count)) {
        return fail(ErrorCode::invalid_state,
                    "Object3D animation track array is truncated");
    }
    values->resize(static_cast<usize>(count));
    return values;
}

[[nodiscard]] Result<i32> animate_object_recursive(
    Machine& machine,
    ObjectRef object,
    i32 world_time,
    std::unordered_set<u64>& visited,
    usize depth = 0U) {
    if (object.is_null() || !visited.insert(object.bits).second) {
        return std::numeric_limits<i32>::max();
    }
    if (depth >= 1'024U) {
        return fail(ErrorCode::overflow, "M3G animation graph is too deep");
    }
    i32 validity = std::numeric_limits<i32>::max();
    auto count = int_field(machine, object, kObject3D, "animationTrackCount");
    if (!count) return std::unexpected(count.error());
    if (*count > 0) {
        auto tracks = active_animation_tracks(machine, object, *count);
        if (!tracks) return std::unexpected(tracks.error());
        for (ObjectRef track : *tracks) {
            if (track.is_null()) continue;
            auto sample = sample_animation_track(machine, track, world_time);
            if (!sample) return std::unexpected(sample.error());
            if (!sample->has_value()) continue;
            validity = std::min(validity, sample->value().validity);
            auto applied = apply_animation_sample(
                machine, object, sample->value());
            if (!applied) return std::unexpected(applied.error());
        }
    }
    auto references = object_references(machine, object);
    if (!references) return std::unexpected(references.error());
    for (ObjectRef reference : *references) {
        auto child_validity = animate_object_recursive(
            machine, reference, world_time, visited, depth + 1U);
        if (!child_validity) return std::unexpected(child_validity.error());
        validity = std::min(validity, *child_validity);
    }
    return validity;
}

void register_object3d(NativeMethodRegistry& registry) {
    register_noop_constructor(registry, kObject3D, "()V", initialize_object3d);
    register_int_property(registry, kObject3D, kObject3D, "userID",
                          "setUserID", "getUserID");
    add(registry, kObject3D, "setUserObject", "(Ljava/lang/Object;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Object3D.setUserObject");
            auto value = reference_argument(arguments, 1U,
                                            "Object3D.setUserObject", true);
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto stored = set_reference_field(machine, *object, kObject3D,
                "userObject", "Ljava/lang/Object;", *value);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, kObject3D, "getUserObject", "()Ljava/lang/Object;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Object3D.getUserObject");
            if (!object) return std::unexpected(object.error());
            auto value = reference_field(machine, *object, kObject3D,
                "userObject", "Ljava/lang/Object;");
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_reference(*value));
        });
    add(registry, kObject3D, "animate", "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Object3D.animate");
            auto world_time = int_argument(arguments, 1U, "Object3D.animate");
            if (!object) return std::unexpected(object.error());
            if (!world_time) return std::unexpected(world_time.error());
            std::unordered_set<u64> visited;
            visited.reserve(64U);
            auto validity = animate_object_recursive(
                machine, *object, *world_time, visited);
            if (!validity) return std::unexpected(validity.error());
            return std::optional<Value>(Value::from_int(
                *validity == std::numeric_limits<i32>::max()
                    ? 0 : std::max(*validity, 0)));
        });
    add(registry, kObject3D, "duplicate",
        "()Ljavax/microedition/m3g/Object3D;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Object3D.duplicate");
            if (!object) return std::unexpected(object.error());
            auto clone = machine.heap().clone_object(*object);
            if (!clone) return std::unexpected(clone.error());
            auto clone_root = machine.pin_native_root(*clone);
            if (!clone_root) return std::unexpected(clone_root.error());
            auto class_name = machine.heap().class_name(*object);
            if (!class_name) return std::unexpected(class_name.error());
            auto is_node = machine.classes().is_assignable(*class_name, kNode);
            if (!is_node) return std::unexpected(is_node.error());
            if (*is_node) {
                // A duplicated Node is detached from its original graph. A
                // shallow heap clone must not retain the source parent or a
                // subsequent Group.addChild() incorrectly rejects the copy.
                auto detached = set_reference_field(
                    machine, *clone, kNode, "parent",
                    "Ljavax/microedition/m3g/Node;", {});
                if (!detached) return std::unexpected(detached.error());
            }
            return std::optional<Value>(Value::from_reference(*clone));
        });
    add(registry, kObject3D, "find",
        "(I)Ljavax/microedition/m3g/Object3D;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Object3D.find");
            auto user_id = int_argument(arguments, 1U, "Object3D.find");
            if (!object) return std::unexpected(object.error());
            if (!user_id) return std::unexpected(user_id.error());
            auto found = find_by_user_id(machine, *object, *user_id);
            if (!found) return std::unexpected(found.error());
            return std::optional<Value>(Value::from_reference(*found));
        });
    add(registry, kObject3D, "getReferences",
        "([Ljavax/microedition/m3g/Object3D;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Object3D.getReferences");
            auto destination = reference_argument(
                arguments, 1U, "Object3D.getReferences", true);
            if (!object) return std::unexpected(object.error());
            if (!destination) return std::unexpected(destination.error());
            auto references = object_references(machine, *object);
            if (!references) return std::unexpected(references.error());
            if (!destination->is_null()) {
                auto length = machine.heap().array_length(*destination);
                if (!length) return std::unexpected(length.error());
                if (*length < references->size()) {
                    return fail_java(
                        "java/lang/IllegalArgumentException",
                        "Object3D reference array is too short");
                }
                for (usize index = 0U; index < references->size(); ++index) {
                    auto stored = machine.heap().set_element(
                        *destination, index,
                        Value::from_reference((*references)[index]));
                    if (!stored) return std::unexpected(stored.error());
                }
            }
            return std::optional<Value>(Value::from_int(
                static_cast<i32>(references->size())));
        });
    add(registry, kObject3D, "addAnimationTrack",
        "(Ljavax/microedition/m3g/AnimationTrack;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Object3D.addAnimationTrack");
            auto track = reference_argument(arguments, 1U,
                                            "Object3D.addAnimationTrack");
            if (!object) return std::unexpected(object.error());
            if (!track) return std::unexpected(track.error());
            auto count = int_field(machine, *object, kObject3D,
                                   "animationTrackCount");
            if (!count) return std::unexpected(count.error());
            auto capacity = ensure_animation_capacity(machine, *object,
                                                       *count + 1);
            if (!capacity) return std::unexpected(capacity.error());
            auto tracks = animation_array(machine, *object);
            if (!tracks) return std::unexpected(tracks.error());
            auto stored = machine.heap().set_element(
                *tracks, static_cast<usize>(*count),
                Value::from_reference(*track));
            if (!stored) return std::unexpected(stored.error());
            auto updated = set_int_field(machine, *object, kObject3D,
                                         "animationTrackCount", *count + 1);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, kObject3D, "removeAnimationTrack",
        "(Ljavax/microedition/m3g/AnimationTrack;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Object3D.removeAnimationTrack");
            auto track = reference_argument(arguments, 1U,
                                            "Object3D.removeAnimationTrack");
            if (!object) return std::unexpected(object.error());
            if (!track) return std::unexpected(track.error());
            auto tracks = animation_array(machine, *object);
            auto count = int_field(machine, *object, kObject3D,
                                   "animationTrackCount");
            if (!tracks) return std::unexpected(tracks.error());
            if (!count) return std::unexpected(count.error());
            auto track_values = machine.heap().read_reference_array(*tracks);
            if (!track_values) return std::unexpected(track_values.error());
            const usize active_count = static_cast<usize>(std::max(*count, 0));
            const auto end = track_values->begin() +
                static_cast<std::ptrdiff_t>(
                    std::min(active_count, track_values->size()));
            const auto found = std::find(track_values->begin(), end, *track);
            if (found != end) {
                const usize index = static_cast<usize>(
                    std::distance(track_values->begin(), found));
                const usize move_count = active_count - index - 1U;
                if (move_count != 0U) {
                    auto shifted = machine.heap().copy_array_range(
                        *tracks, index + 1U, *tracks, index, move_count);
                    if (!shifted) return std::unexpected(shifted.error());
                }
                auto cleared = machine.heap().set_element(
                    *tracks, static_cast<usize>(*count - 1),
                    Value::from_reference({}));
                if (!cleared) return std::unexpected(cleared.error());
                auto updated = set_int_field(machine, *object, kObject3D,
                                             "animationTrackCount", *count - 1);
                if (!updated) return std::unexpected(updated.error());
            }
            return std::optional<Value> {};
        });
    add(registry, kObject3D, "getAnimationTrack",
        "(I)Ljavax/microedition/m3g/AnimationTrack;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Object3D.getAnimationTrack");
            auto index = int_argument(arguments, 1U,
                                      "Object3D.getAnimationTrack");
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            auto count = int_field(machine, *object, kObject3D,
                                   "animationTrackCount");
            auto tracks = animation_array(machine, *object);
            if (!count) return std::unexpected(count.error());
            if (!tracks) return std::unexpected(tracks.error());
            if (*index < 0 || *index >= *count) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "animation track index is out of range");
            }
            auto value = machine.heap().element(*tracks,
                                                static_cast<usize>(*index));
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(*value);
        });
    register_int_property(registry, kObject3D, kObject3D,
                          "animationTrackCount", "setAnimationTrackCountInternal",
                          "getAnimationTrackCount");
}

void register_node(NativeMethodRegistry& registry) {
    register_noop_constructor(registry, kNode, "()V", initialize_node);
    register_int_property(registry, kNode, kNode, "renderingEnabled",
                          "setRenderingEnable", "isRenderingEnabled", "Z");
    register_int_property(registry, kNode, kNode, "pickingEnabled",
                          "setPickingEnable", "isPickingEnabled", "Z");
    register_float_property(registry, kNode, kNode, "alphaFactor",
                            "setAlphaFactor", "getAlphaFactor");
    register_int_property(registry, kNode, kNode, "scope",
                          "setScope", "getScope");
    add(registry, kNode, "getParent", "()Ljavax/microedition/m3g/Node;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Node.getParent");
            if (!object) return std::unexpected(object.error());
            auto parent = reference_field(machine, *object, kNode, "parent",
                                           "Ljavax/microedition/m3g/Node;");
            if (!parent) return std::unexpected(parent.error());
            return std::optional<Value>(Value::from_reference(*parent));
        });
    add(registry, kNode, "setAlignment",
        "(Ljavax/microedition/m3g/Node;ILjavax/microedition/m3g/Node;I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Node.setAlignment");
            auto z_target = reference_argument(arguments, 1U,
                                               "Node.setAlignment", true);
            auto z_reference = int_argument(arguments, 2U, "Node.setAlignment");
            auto y_target = reference_argument(arguments, 3U,
                                               "Node.setAlignment", true);
            auto y_reference = int_argument(arguments, 4U, "Node.setAlignment");
            if (!object) return std::unexpected(object.error());
            if (!z_target || !z_reference || !y_target || !y_reference) {
                return fail(ErrorCode::invalid_argument,
                            "Node alignment arguments are invalid");
            }
            if (*z_reference < 144 || *z_reference > 148 ||
                *y_reference < 144 || *y_reference > 148) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Node alignment target is invalid");
            }
            if (*z_target == *object || *y_target == *object) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Node cannot align relative to itself");
            }
            if (*z_reference != 144 && *z_reference == *y_reference &&
                *z_target == *y_target) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Z and Y alignment constraints are identical");
            }
            const ObjectRef stored_z_target =
                *z_reference == 144 ? ObjectRef {} : *z_target;
            const ObjectRef stored_y_target =
                *y_reference == 144 ? ObjectRef {} : *y_target;
            const std::array<Status, 4> stored {
                set_reference_field(machine, *object, kNode, "zTarget",
                    "Ljavax/microedition/m3g/Node;", stored_z_target),
                set_int_field(machine, *object, kNode, "zReference", *z_reference),
                set_reference_field(machine, *object, kNode, "yTarget",
                    "Ljavax/microedition/m3g/Node;", stored_y_target),
                set_int_field(machine, *object, kNode, "yReference", *y_reference),
            };
            for (const Status& status : stored) {
                if (!status) return std::unexpected(status.error());
            }
            return std::optional<Value> {};
        });
    add(registry, kNode, "align", "(Ljavax/microedition/m3g/Node;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Node.align");
            auto fallback = reference_argument(arguments, 1U,
                                               "Node.align", true);
            if (!object) return std::unexpected(object.error());
            if (!fallback) return std::unexpected(fallback.error());

            auto object_root = root_node(machine, *object);
            if (!object_root) return std::unexpected(object_root.error());
            if (!fallback->is_null()) {
                auto fallback_root = root_node(machine, *fallback);
                if (!fallback_root) {
                    return std::unexpected(fallback_root.error());
                }
                if (*fallback_root != *object_root) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     "alignment reference is in another scene graph");
                }
            }

            auto z_reference = reference_field(machine, *object, kNode,
                "zTarget", "Ljavax/microedition/m3g/Node;");
            auto y_reference = reference_field(machine, *object, kNode,
                "yTarget", "Ljavax/microedition/m3g/Node;");
            auto z_target = int_field(machine, *object, kNode, "zReference");
            auto y_target = int_field(machine, *object, kNode, "yReference");
            if (!z_reference) return std::unexpected(z_reference.error());
            if (!y_reference) return std::unexpected(y_reference.error());
            if (!z_target) return std::unexpected(z_target.error());
            if (!y_target) return std::unexpected(y_target.error());
            if (*z_target == 144 && *y_target == 144) {
                return std::optional<Value> {};
            }

            const auto resolve_reference = [&](ObjectRef configured,
                                               i32 target,
                                               std::string_view axis)
                -> Result<ObjectRef> {
                if (target == 144) return ObjectRef {};
                const ObjectRef resolved = configured.is_null()
                    ? *fallback : configured;
                if (resolved.is_null() || resolved == *object) {
                    return fail_java("java/lang/IllegalStateException",
                                     std::string(axis) +
                                         " alignment reference is undefined");
                }
                auto resolved_root = root_node(machine, resolved);
                if (!resolved_root) {
                    return std::unexpected(resolved_root.error());
                }
                if (*resolved_root != *object_root) {
                    return fail_java("java/lang/IllegalStateException",
                                     std::string(axis) +
                                         " alignment reference left the scene graph");
                }
                auto descendant = is_descendant(machine, resolved, *object);
                if (!descendant) return std::unexpected(descendant.error());
                if (*descendant) {
                    return fail_java("java/lang/IllegalStateException",
                                     std::string(axis) +
                                         " alignment reference is a descendant");
                }
                return resolved;
            };

            auto resolved_z = resolve_reference(
                *z_reference, *z_target, "Z");
            auto resolved_y = resolve_reference(
                *y_reference, *y_target, "Y");
            if (!resolved_z) return std::unexpected(resolved_z.error());
            if (!resolved_y) return std::unexpected(resolved_y.error());

            std::optional<Vector3> z_direction;
            std::optional<Vector3> y_direction;
            if (*z_target != 144) {
                auto direction = alignment_target_vector(
                    machine, *object, *resolved_z, *z_target);
                if (!direction) return std::unexpected(direction.error());
                z_direction = *direction;
            }
            if (*y_target != 144) {
                auto direction = alignment_target_vector(
                    machine, *object, *resolved_y, *y_target);
                if (!direction) return std::unexpected(direction.error());
                y_direction = *direction;
            }

            Result<Quaternion> orientation = Quaternion {};
            if (z_direction.has_value() && y_direction.has_value()) {
                auto normalized_z = normalize_vector(
                    *z_direction, "Z alignment");
                if (!normalized_z) {
                    return std::unexpected(normalized_z.error());
                }
                Vector3 projected_y = subtract(
                    *y_direction,
                    multiply_vector(*normalized_z,
                                    dot(*y_direction, *normalized_z)));
                const float projected_length =
                    std::sqrt(dot(projected_y, projected_y));
                if (!std::isfinite(projected_length) ||
                    projected_length <= 1.0e-6F) {
                    orientation = quaternion_between(
                        Vector3 {0.0F, 0.0F, 1.0F}, *normalized_z);
                } else {
                    projected_y = multiply_vector(
                        projected_y, 1.0F / projected_length);
                    auto x_axis = normalize_vector(
                        cross(projected_y, *normalized_z),
                        "alignment X axis");
                    if (!x_axis) return std::unexpected(x_axis.error());
                    auto y_axis = normalize_vector(
                        cross(*normalized_z, *x_axis),
                        "alignment Y axis");
                    if (!y_axis) return std::unexpected(y_axis.error());
                    orientation = quaternion_from_basis(
                        *x_axis, *y_axis, *normalized_z);
                }
            } else if (z_direction.has_value()) {
                orientation = quaternion_between(
                    Vector3 {0.0F, 0.0F, 1.0F}, *z_direction);
            } else if (y_direction.has_value()) {
                orientation = quaternion_between(
                    Vector3 {0.0F, 1.0F, 0.0F}, *y_direction);
            }
            if (!orientation) return std::unexpected(orientation.error());
            auto stored = set_transformable_quaternion(
                machine, *object, *orientation);
            if (!stored) return std::unexpected(stored.error());
            stored = rebuild_transformable_matrix(machine, *object);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, kNode, "getAlignmentTarget", "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Node.getAlignmentTarget");
            auto axis = int_argument(arguments, 1U, "Node.getAlignmentTarget");
            if (!object) return std::unexpected(object.error());
            if (!axis) return std::unexpected(axis.error());
            if (*axis != 147 && *axis != 148) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "alignment axis must be Y_AXIS or Z_AXIS");
            }
            const char* name = *axis == 148 ? "zReference" : "yReference";
            auto value = int_field(machine, *object, kNode, name);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    add(registry, kNode, "getAlignmentReference",
        "(I)Ljavax/microedition/m3g/Node;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Node.getAlignmentReference");
            auto axis = int_argument(arguments, 1U, "Node.getAlignmentReference");
            if (!object) return std::unexpected(object.error());
            if (!axis) return std::unexpected(axis.error());
            if (*axis != 147 && *axis != 148) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "alignment axis must be Y_AXIS or Z_AXIS");
            }
            const char* name = *axis == 148 ? "zTarget" : "yTarget";
            auto target = reference_field(machine, *object, kNode, name,
                                           "Ljavax/microedition/m3g/Node;");
            if (!target) return std::unexpected(target.error());
            return std::optional<Value>(Value::from_reference(*target));
        });
    add(registry, kNode, "getTransformTo",
        "(Ljavax/microedition/m3g/Node;Ljavax/microedition/m3g/Transform;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Node.getTransformTo");
            auto target = reference_argument(arguments, 1U,
                                             "Node.getTransformTo");
            auto destination = reference_argument(arguments, 2U,
                                                  "Node.getTransformTo");
            if (!object || !target || !destination) {
                return fail(ErrorCode::invalid_argument,
                            "Node.getTransformTo arguments are invalid");
            }
            auto source_root = root_node(machine, *object);
            auto target_root = root_node(machine, *target);
            if (!source_root) return std::unexpected(source_root.error());
            if (!target_root) return std::unexpected(target_root.error());
            if (*source_root != *target_root) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto source_matrix = composite_matrix(machine, *object);
            auto target_matrix = composite_matrix(machine, *target);
            if (!source_matrix) return std::unexpected(source_matrix.error());
            if (!target_matrix) return std::unexpected(target_matrix.error());
            auto inverse = inverse_matrix(*target_matrix);
            if (!inverse) return std::optional<Value>(Value::from_int(0));
            auto stored = set_transform_matrix(
                machine, *destination, multiply(*inverse, *source_matrix));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_int(1));
        });
}

void register_ray_intersection(NativeMethodRegistry& registry) {
    add(registry, kRayIntersection, "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "RayIntersection.<init>");
            if (!object) return std::unexpected(object.error());
            const std::array<float, 8> texture_values {};
            const std::array<float, 3> normal_values {0.0F, 0.0F, 1.0F};
            const std::array<float, 6> ray_values {
                0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
            auto texture_s = float_array(machine, texture_values);
            auto texture_t = float_array(machine, texture_values);
            auto normal = float_array(machine, normal_values);
            auto ray = float_array(machine, ray_values);
            if (!texture_s) return std::unexpected(texture_s.error());
            if (!texture_t) return std::unexpected(texture_t.error());
            if (!normal) return std::unexpected(normal.error());
            if (!ray) return std::unexpected(ray.error());
            const std::array<Status, 4> stored {
                set_reference_field(machine, *object, kRayIntersection,
                                    "textureS", "[F", *texture_s),
                set_reference_field(machine, *object, kRayIntersection,
                                    "textureT", "[F", *texture_t),
                set_reference_field(machine, *object, kRayIntersection,
                                    "normal", "[F", *normal),
                set_reference_field(machine, *object, kRayIntersection,
                                    "ray", "[F", *ray),
            };
            for (const Status& status : stored) {
                if (!status) return std::unexpected(status.error());
            }
            return std::optional<Value> {};
        });
    add(registry, kRayIntersection, "getIntersected",
        "()Ljavax/microedition/m3g/Node;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "RayIntersection.getIntersected");
            if (!object) return std::unexpected(object.error());
            auto node = reference_field(machine, *object, kRayIntersection,
                "intersected", "Ljavax/microedition/m3g/Node;");
            if (!node) return std::unexpected(node.error());
            return std::optional<Value>(Value::from_reference(*node));
        });
    add(registry, kRayIntersection, "getDistance", "()F",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "RayIntersection.getDistance");
            if (!object) return std::unexpected(object.error());
            auto value = float_field(machine, *object, kRayIntersection,
                                     "distance");
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_float(*value));
        });
    add(registry, kRayIntersection, "getSubmeshIndex", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "RayIntersection.getSubmeshIndex");
            if (!object) return std::unexpected(object.error());
            auto value = int_field(machine, *object, kRayIntersection,
                                   "submeshIndex");
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    const auto texture_getter = [&registry](const char* method,
                                            const char* field) {
        add(registry, kRayIntersection, method, "(I)F",
            [method, field](Machine& machine,
                            std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, method);
                auto index = int_argument(arguments, 1U, method);
                if (!object) return std::unexpected(object.error());
                if (!index) return std::unexpected(index.error());
                auto array = reference_field(machine, *object,
                                              kRayIntersection, field, "[F");
                if (!array) return std::unexpected(array.error());
                auto length = machine.heap().array_length(*array);
                if (!length) return std::unexpected(length.error());
                if (*index < 0 || static_cast<usize>(*index) >= *length) {
                    return fail_java("java/lang/IndexOutOfBoundsException",
                                     "texture coordinate index is out of range");
                }
                auto value = machine.heap().element(
                    *array, static_cast<usize>(*index));
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(*value);
            });
    };
    texture_getter("getTextureS", "textureS");
    texture_getter("getTextureT", "textureT");
    const auto normal_getter = [&registry](const char* method, usize index) {
        add(registry, kRayIntersection, method, "()F",
            [method, index](Machine& machine,
                            std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, method);
                if (!object) return std::unexpected(object.error());
                auto array = reference_field(machine, *object,
                                              kRayIntersection, "normal", "[F");
                if (!array) return std::unexpected(array.error());
                auto value = machine.heap().element(*array, index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(*value);
            });
    };
    normal_getter("getNormalX", 0U);
    normal_getter("getNormalY", 1U);
    normal_getter("getNormalZ", 2U);
    add(registry, kRayIntersection, "getRay", "([F)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "RayIntersection.getRay");
            auto destination = reference_argument(arguments, 1U,
                                                  "RayIntersection.getRay", false);
            if (!object) return std::unexpected(object.error());
            if (!destination) return std::unexpected(destination.error());
            auto source = reference_field(machine, *object,
                                           kRayIntersection, "ray", "[F");
            if (!source) return std::unexpected(source.error());
            auto values = read_float_array(machine, *source,
                                           "RayIntersection ray");
            if (!values) return std::unexpected(values.error());
            if (values->size() < 6U) {
                return fail(ErrorCode::invalid_state,
                            "RayIntersection ray is truncated");
            }
            auto length = machine.heap().array_length(*destination);
            if (!length) return std::unexpected(length.error());
            if (*length < 6U) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "RayIntersection destination is too short");
            }
            auto written = write_float_array(
                machine, *destination,
                std::span<const float>(values->data(), 6U));
            if (!written) return std::unexpected(written.error());
            return std::optional<Value> {};
        });
}

void register_group(NativeMethodRegistry& registry) {
    register_noop_constructor(registry, kGroup, "()V", initialize_group);
    add(registry, kGroup, "addChild", "(Ljavax/microedition/m3g/Node;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Group.addChild");
            auto child = reference_argument(arguments, 1U, "Group.addChild");
            if (!object) return std::unexpected(object.error());
            if (!child) return std::unexpected(child.error());
            if (*child == *object) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Group cannot contain itself");
            }
            auto cycle = is_descendant(machine, *object, *child);
            if (!cycle) return std::unexpected(cycle.error());
            if (*cycle) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Group child would create a cycle");
            }
            auto existing_parent = reference_field(machine, *child, kNode,
                "parent", "Ljavax/microedition/m3g/Node;");
            if (!existing_parent) return std::unexpected(existing_parent.error());
            if (!existing_parent->is_null()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Node already has a parent");
            }
            auto count = int_field(machine, *object, kGroup, "childCount");
            if (!count) return std::unexpected(count.error());
            auto capacity = ensure_child_capacity(machine, *object, *count + 1);
            if (!capacity) return std::unexpected(capacity.error());
            auto children = child_array(machine, *object);
            if (!children) return std::unexpected(children.error());
            auto stored = machine.heap().set_element(
                *children, static_cast<usize>(*count),
                Value::from_reference(*child));
            if (!stored) return std::unexpected(stored.error());
            auto parent = set_reference_field(machine, *child, kNode, "parent",
                "Ljavax/microedition/m3g/Node;", *object);
            if (!parent) return std::unexpected(parent.error());
            auto updated = set_int_field(machine, *object, kGroup,
                                         "childCount", *count + 1);
            if (!updated) return std::unexpected(updated.error());
            return std::optional<Value> {};
        });
    add(registry, kGroup, "removeChild",
        "(Ljavax/microedition/m3g/Node;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Group.removeChild");
            auto child = reference_argument(arguments, 1U,
                                            "Group.removeChild", true);
            if (!object) return std::unexpected(object.error());
            if (!child) return std::unexpected(child.error());
            auto children = child_array(machine, *object);
            auto count = int_field(machine, *object, kGroup, "childCount");
            if (!children) return std::unexpected(children.error());
            if (!count) return std::unexpected(count.error());
            auto child_values = machine.heap().read_reference_array(*children);
            if (!child_values) return std::unexpected(child_values.error());
            const usize active_count = static_cast<usize>(std::max(*count, 0));
            const auto end = child_values->begin() +
                static_cast<std::ptrdiff_t>(
                    std::min(active_count, child_values->size()));
            const auto found = std::find(child_values->begin(), end, *child);
            if (found != end) {
                const usize index = static_cast<usize>(
                    std::distance(child_values->begin(), found));
                const usize move_count = active_count - index - 1U;
                if (move_count != 0U) {
                    auto shifted = machine.heap().copy_array_range(
                        *children, index + 1U, *children, index, move_count);
                    if (!shifted) return std::unexpected(shifted.error());
                }
                auto cleared = machine.heap().set_element(
                    *children, static_cast<usize>(*count - 1),
                    Value::from_reference({}));
                if (!cleared) return std::unexpected(cleared.error());
                if (!child->is_null()) {
                    auto parent = set_reference_field(machine, *child, kNode,
                        "parent", "Ljavax/microedition/m3g/Node;", {});
                    if (!parent) return std::unexpected(parent.error());
                }
                auto updated = set_int_field(machine, *object, kGroup,
                                             "childCount", *count - 1);
                if (!updated) return std::unexpected(updated.error());
            }
            return std::optional<Value> {};
        });
    add(registry, kGroup, "getChild", "(I)Ljavax/microedition/m3g/Node;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Group.getChild");
            auto index = int_argument(arguments, 1U, "Group.getChild");
            if (!object) return std::unexpected(object.error());
            if (!index) return std::unexpected(index.error());
            auto count = int_field(machine, *object, kGroup, "childCount");
            auto children = child_array(machine, *object);
            if (!count) return std::unexpected(count.error());
            if (!children) return std::unexpected(children.error());
            if (*index < 0 || *index >= *count) {
                return fail_java("java/lang/IndexOutOfBoundsException",
                                 "Group child index is out of range");
            }
            auto value = machine.heap().element(*children,
                                                static_cast<usize>(*index));
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(*value);
        });
    register_int_property(registry, kGroup, kGroup, "childCount",
                          "setChildCountInternal", "getChildCount");
    add(registry, kGroup, "pick",
        "(IFFFFFFLjavax/microedition/m3g/RayIntersection;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto group = receiver(arguments, "Group.pick");
            auto mask = int_argument(arguments, 1U, "Group.pick");
            if (!group) return std::unexpected(group.error());
            if (!mask) return std::unexpected(mask.error());
            std::array<float, 6> ray {};
            for (usize index = 0U; index < ray.size(); ++index) {
                auto value = float_argument(arguments, index + 2U,
                                            "Group.pick");
                if (!value) return std::unexpected(value.error());
                ray[index] = *value;
            }
            auto intersection = reference_argument(
                arguments, 8U, "Group.pick", true);
            if (!intersection) return std::unexpected(intersection.error());
            auto picked = pick_group_ray(
                machine, *group, *mask,
                Vector3 {ray[0U], ray[1U], ray[2U]},
                Vector3 {ray[3U], ray[4U], ray[5U]},
                *intersection);
            if (!picked) return std::unexpected(picked.error());
            return std::optional<Value>(Value::from_int(*picked ? 1 : 0));
        });

    const auto camera_pick = [&registry](const char* descriptor,
                                         usize camera_index,
                                         usize intersection_index) {
        add(registry, kGroup, "pick", descriptor,
            [camera_index, intersection_index](
                Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto group = receiver(arguments, "Group.pick");
                auto mask = int_argument(arguments, 1U, "Group.pick");
                auto x = float_argument(arguments, 2U, "Group.pick");
                auto y = float_argument(arguments, 3U, "Group.pick");
                auto camera = reference_argument(
                    arguments, camera_index, "Group.pick", true);
                auto intersection = reference_argument(
                    arguments, intersection_index, "Group.pick", true);
                if (!group) return std::unexpected(group.error());
                if (!mask) return std::unexpected(mask.error());
                if (!x) return std::unexpected(x.error());
                if (!y) return std::unexpected(y.error());
                if (!camera) return std::unexpected(camera.error());
                if (!intersection) {
                    return std::unexpected(intersection.error());
                }
                ObjectRef resolved_camera = *camera;
                if (resolved_camera.is_null()) {
                    auto root = root_node(machine, *group);
                    if (!root) return std::unexpected(root.error());
                    auto root_class = machine.heap().class_name(*root);
                    if (!root_class) {
                        return std::unexpected(root_class.error());
                    }
                    if (*root_class != kWorld) {
                        return fail_java(
                            "java/lang/IllegalArgumentException",
                            "null pick camera requires a World root");
                    }
                    auto active = reference_field(
                        machine, *root, kWorld, "activeCamera",
                        "Ljavax/microedition/m3g/Camera;");
                    if (!active) return std::unexpected(active.error());
                    if (active->is_null()) {
                        return fail_java(
                            "java/lang/IllegalStateException",
                            "World has no active Camera");
                    }
                    resolved_camera = *active;
                }
                auto ray = camera_pick_ray(
                    machine, *group, resolved_camera, *x, *y);
                if (!ray) return std::unexpected(ray.error());
                auto picked = pick_group_ray(
                    machine, *group, *mask,
                    (*ray)[0U], (*ray)[1U], *intersection);
                if (!picked) return std::unexpected(picked.error());
                return std::optional<Value>(
                    Value::from_int(*picked ? 1 : 0));
            });
    };
    camera_pick(
        "(IFFLjavax/microedition/m3g/Camera;"
        "Ljavax/microedition/m3g/RayIntersection;)Z",
        4U, 5U);
    camera_pick(
        "(IFFFFLjavax/microedition/m3g/Camera;"
        "Ljavax/microedition/m3g/RayIntersection;)Z",
        6U, 7U);
}

void register_world(NativeMethodRegistry& registry) {
    register_noop_constructor(registry, kWorld, "()V", initialize_group);
    register_reference_property(registry, kWorld, kWorld, "activeCamera",
        "Ljavax/microedition/m3g/Camera;", "setActiveCamera",
        "getActiveCamera");
    add(registry, kWorld, "setBackground",
        "(Ljavax/microedition/m3g/Background;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "World.setBackground");
            auto value = reference_argument(arguments, 1U,
                                            "World.setBackground", true);
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto stored = set_reference_field(machine, *object, kWorld,
                "background", "Ljavax/microedition/m3g/Background;", *value);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, kWorld, "getBackground",
        "()Ljavax/microedition/m3g/Background;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "World.getBackground");
            if (!object) return std::unexpected(object.error());
            auto value = reference_field(machine, *object, kWorld,
                "background", "Ljavax/microedition/m3g/Background;");
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_reference(*value));
        });
}

void register_camera(NativeMethodRegistry& registry) {
    register_noop_constructor(registry, "javax/microedition/m3g/Camera",
                              "()V", initialize_node);
    const auto projection = [&registry](const char* name, i32 type) {
        add(registry, "javax/microedition/m3g/Camera", name, "(FFFF)V",
            [type](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, "Camera projection");
                if (!object) return std::unexpected(object.error());
                std::array<float, 4> values {};
                for (usize index = 0; index < values.size(); ++index) {
                    auto value = float_argument(arguments, index + 1U,
                                                "Camera projection");
                    if (!value) return std::unexpected(value.error());
                    values[index] = *value;
                }
                auto array = float_array(machine, values);
                if (!array) return std::unexpected(array.error());
                auto type_stored = set_int_field(machine, *object,
                    "javax/microedition/m3g/Camera", "projectionType", type);
                auto values_stored = set_reference_field(machine, *object,
                    "javax/microedition/m3g/Camera", "projection", "[F", *array);
                if (!type_stored) return std::unexpected(type_stored.error());
                if (!values_stored) return std::unexpected(values_stored.error());
                return std::optional<Value> {};
            });
    };
    projection("setParallel", 48);
    projection("setPerspective", 50);
    add(registry, "javax/microedition/m3g/Camera", "setGeneric",
        "(Ljavax/microedition/m3g/Transform;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Camera.setGeneric");
            auto source = reference_argument(arguments, 1U, "Camera.setGeneric");
            if (!object) return std::unexpected(object.error());
            if (!source) return std::unexpected(source.error());
            auto matrix = transform_matrix(machine, *source);
            if (!matrix) return std::unexpected(matrix.error());
            auto array = float_array(machine, *matrix);
            if (!array) return std::unexpected(array.error());
            auto type_stored = set_int_field(machine, *object,
                "javax/microedition/m3g/Camera", "projectionType", 52);
            auto values_stored = set_reference_field(machine, *object,
                "javax/microedition/m3g/Camera", "projection", "[F", *array);
            if (!type_stored) return std::unexpected(type_stored.error());
            if (!values_stored) return std::unexpected(values_stored.error());
            return std::optional<Value> {};
        });
    add(registry, "javax/microedition/m3g/Camera", "getProjection",
        "([F)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Camera.getProjection");
            auto destination = reference_argument(arguments, 1U,
                                                  "Camera.getProjection", true);
            if (!object) return std::unexpected(object.error());
            if (!destination) return std::unexpected(destination.error());
            auto type = int_field(machine, *object,
                                  "javax/microedition/m3g/Camera",
                                  "projectionType");
            if (!type) return std::unexpected(type.error());
            if (!destination->is_null()) {
                auto array = reference_field(machine, *object,
                    "javax/microedition/m3g/Camera", "projection", "[F");
                if (!array) return std::unexpected(array.error());
                if (!array->is_null()) {
                    auto values = read_float_array(machine, *array,
                                                   "Camera.getProjection");
                    if (!values) return std::unexpected(values.error());
                    auto stored = write_float_array(machine, *destination,
                                                    *values);
                    if (!stored) return std::unexpected(stored.error());
                }
            }
            return std::optional<Value>(Value::from_int(*type));
        });
    add(registry, "javax/microedition/m3g/Camera", "getProjection",
        "(Ljavax/microedition/m3g/Transform;)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Camera.getProjection");
            auto destination = reference_argument(arguments, 1U,
                                                  "Camera.getProjection", true);
            if (!object) return std::unexpected(object.error());
            if (!destination) return std::unexpected(destination.error());
            auto type = int_field(machine, *object,
                                  "javax/microedition/m3g/Camera",
                                  "projectionType");
            if (!type) return std::unexpected(type.error());
            if (!destination->is_null()) {
                Matrix matrix = identity_matrix();
                auto array = reference_field(machine, *object,
                    "javax/microedition/m3g/Camera", "projection", "[F");
                if (!array) return std::unexpected(array.error());
                if (*type == 52 && !array->is_null()) {
                    auto values = read_float_array(machine, *array,
                                                   "Camera.getProjection");
                    if (!values) return std::unexpected(values.error());
                    if (values->size() >= 16U) {
                        std::copy_n(values->begin(), 16, matrix.begin());
                    }
                }
                auto stored = set_transform_matrix(machine, *destination, matrix);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value>(Value::from_int(*type));
        });
}

void register_light(NativeMethodRegistry& registry) {
    constexpr const char* light = "javax/microedition/m3g/Light";
    register_noop_constructor(registry, light, "()V",
        [](Machine& machine, ObjectRef object) -> Status {
            auto initialized = initialize_node(machine, object);
            if (!initialized) return initialized;
            const std::array<float, 3> attenuation_values {
                1.0F, 0.0F, 0.0F};
            auto attenuation = float_array(machine, attenuation_values);
            if (!attenuation) return std::unexpected(attenuation.error());
            const std::array<Status, 7> stored {
                set_int_field(machine, object, light, "mode", 129),
                set_float_field(machine, object, light, "intensity", 1.0F),
                set_int_field(machine, object, light, "color", 0x00FFFFFF),
                set_float_field(machine, object, light, "spotAngle", 45.0F),
                set_float_field(machine, object, light, "spotExponent", 0.0F),
                set_reference_field(machine, object, light,
                                    "attenuation", "[F", *attenuation),
                set_int_field(machine, object, kNode,
                              "renderingEnabled", 1, "Z"),
            };
            for (const Status& status : stored) {
                if (!status) return status;
            }
            return {};
        });
    add(registry, light, "setMode", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Light.setMode");
            auto value = int_argument(arguments, 1U, "Light.setMode");
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            if (*value < 128 || *value > 131) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Light mode is invalid");
            }
            auto stored = set_int_field(machine, *object, light,
                                        "mode", *value);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, light, "getMode", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Light.getMode");
            if (!object) return std::unexpected(object.error());
            auto value = int_field(machine, *object, light, "mode");
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    register_float_property(registry, "javax/microedition/m3g/Light",
                            "javax/microedition/m3g/Light", "intensity",
                            "setIntensity", "getIntensity");
    add(registry, light, "setColor", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Light.setColor");
            auto value = int_argument(arguments, 1U, "Light.setColor");
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto stored = set_int_field(
                machine, *object, light, "color",
                static_cast<i32>(static_cast<u32>(*value) & 0x00FFFFFFU));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, light, "getColor", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Light.getColor");
            if (!object) return std::unexpected(object.error());
            auto value = int_field(machine, *object, light, "color");
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });
    const auto bounded_float_property = [&registry](
        const char* setter,
        const char* getter,
        const char* field,
        float minimum,
        float maximum) {
        add(registry, light, setter, "(F)V",
            [setter, field, minimum, maximum](
                Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, setter);
                auto value = float_argument(arguments, 1U, setter);
                if (!object) return std::unexpected(object.error());
                if (!value) return std::unexpected(value.error());
                if (!std::isfinite(*value) || *value < minimum ||
                    *value > maximum) {
                    return fail_java("java/lang/IllegalArgumentException",
                                     std::string(setter) +
                                         " value is out of range");
                }
                auto stored = set_float_field(machine, *object, light,
                                              field, *value);
                if (!stored) return std::unexpected(stored.error());
                return std::optional<Value> {};
            });
        add(registry, light, getter, "()F",
            [getter, field](Machine& machine,
                            std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, getter);
                if (!object) return std::unexpected(object.error());
                auto value = float_field(machine, *object, light, field);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(Value::from_float(*value));
            });
    };
    bounded_float_property("setSpotAngle", "getSpotAngle",
                           "spotAngle", 0.0F, 90.0F);
    bounded_float_property("setSpotExponent", "getSpotExponent",
                           "spotExponent", 0.0F, 128.0F);
    add(registry, "javax/microedition/m3g/Light", "setAttenuation", "(FFF)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments, "Light.setAttenuation");
            if (!object) return std::unexpected(object.error());
            std::array<float, 3> values {};
            for (usize index = 0; index < values.size(); ++index) {
                auto value = float_argument(arguments, index + 1U,
                                            "Light.setAttenuation");
                if (!value) return std::unexpected(value.error());
                values[index] = *value;
            }
            auto array = float_array(machine, values);
            if (!array) return std::unexpected(array.error());
            auto stored = set_reference_field(machine, *object,
                "javax/microedition/m3g/Light", "attenuation", "[F", *array);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    const auto attenuation_getter = [&registry](const char* name, usize index) {
        add(registry, "javax/microedition/m3g/Light", name, "()F",
            [index](Machine& machine, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto object = receiver(arguments, "Light attenuation getter");
                if (!object) return std::unexpected(object.error());
                auto array = reference_field(machine, *object,
                    "javax/microedition/m3g/Light", "attenuation", "[F");
                if (!array) return std::unexpected(array.error());
                if (array->is_null()) {
                    return std::optional<Value>(Value::from_float(index == 0U
                        ? 1.0F : 0.0F));
                }
                auto value = machine.heap().element(*array, index);
                if (!value) return std::unexpected(value.error());
                return std::optional<Value>(*value);
            });
    };
    attenuation_getter("getConstantAttenuation", 0U);
    attenuation_getter("getLinearAttenuation", 1U);
    attenuation_getter("getQuadraticAttenuation", 2U);
}

} // namespace

void register_m3g_scene_natives(NativeMethodRegistry& registry) {
    register_animation_controller(registry);
    register_keyframe_sequence(registry);
    register_animation_track(registry);
    register_object3d(registry);
    register_node(registry);
    register_ray_intersection(registry);
    register_group(registry);
    register_world(registry);
    register_camera(registry);
    register_light(registry);
}

} // namespace phoneme::vm
