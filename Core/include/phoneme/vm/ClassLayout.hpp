#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "phoneme/vm/ClassRepository.hpp"
#include "phoneme/vm/Descriptor.hpp"
#include "phoneme/vm/Heap.hpp"

namespace phoneme::vm {

struct FieldLocation final {
    FieldId id;
    ClassId declaring_class_id;
    std::string declaring_class;
    std::string name;
    std::string descriptor;
    usize index {0};
    ValueKind value_kind {ValueKind::int32};
    bool is_static {false};
    std::optional<u16> constant_value_index;
    std::string storage_key;
};

struct ClassLayout final {
    std::string class_name;
    std::string super_name;
    bool instantiable {true};
    usize instance_field_slots {0};
    std::unordered_map<std::string, usize> instance_fields;
    std::vector<Value> instance_defaults;
};

class ClassStateRegistry final {
public:
    explicit ClassStateRegistry(ClassRepository& classes) noexcept
        : classes_(classes) {}

    [[nodiscard]] Result<std::shared_ptr<const ClassLayout>> layout(
        std::string_view class_name);
    [[nodiscard]] Result<FieldLocation> resolve_field(
        std::string_view owner,
        std::string_view name,
        std::string_view descriptor,
        bool require_static);
    [[nodiscard]] Result<ObjectRef> allocate_instance(
        Heap& heap,
        std::string_view class_name);
    [[nodiscard]] Result<ObjectRef> allocate_text_instance(
        Heap& heap,
        std::string_view class_name,
        std::u16string text);
    [[nodiscard]] Result<Value> static_field(const FieldLocation& field);
    [[nodiscard]] Result<Value> static_field(FieldId field_id) const;
    [[nodiscard]] Status set_static_field(const FieldLocation& field, Value value);
    [[nodiscard]] Status set_static_field(FieldId field_id,
                                          ValueKind expected_kind,
                                          Value value);
    void append_reference_roots(std::vector<ObjectRef>& roots) const;
    void clear() noexcept;

private:
    friend class Machine;

    // Java execution is serialized by Machine::execution_mutex_. These hot
    // accessors mirror Heap's vm_* API and avoid taking ClassStateRegistry's
    // metadata mutex for already-linked static fields. They must never escape
    // a Machine execution-gate region.
    [[nodiscard]] Result<Value> vm_static_field(FieldId field_id) const;
    [[nodiscard]] Status vm_set_static_field(FieldId field_id,
                                              ValueKind expected_kind,
                                              Value value);

    struct StringHash final {
        using is_transparent = void;

        [[nodiscard]] usize operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
        [[nodiscard]] usize operator()(const std::string& value) const noexcept {
            return (*this)(std::string_view(value));
        }
    };

    struct StringEqual final {
        using is_transparent = void;

        [[nodiscard]] bool operator()(std::string_view left,
                                      std::string_view right) const noexcept {
            return left == right;
        }
    };

    struct FieldResolutionKey final {
        std::string owner;
        std::string name;
        std::string descriptor;
        bool require_static {false};
    };

    struct FieldResolutionKeyView final {
        std::string_view owner;
        std::string_view name;
        std::string_view descriptor;
        bool require_static {false};
    };

    struct FieldResolutionKeyHash final {
        using is_transparent = void;
        [[nodiscard]] usize operator()(
            const FieldResolutionKey& key) const noexcept;
        [[nodiscard]] usize operator()(
            FieldResolutionKeyView key) const noexcept;
    };

    struct FieldResolutionKeyEqual final {
        using is_transparent = void;
        [[nodiscard]] bool operator()(
            const FieldResolutionKey& left,
            const FieldResolutionKey& right) const noexcept;
        [[nodiscard]] bool operator()(
            const FieldResolutionKey& left,
            FieldResolutionKeyView right) const noexcept;
        [[nodiscard]] bool operator()(
            FieldResolutionKeyView left,
            const FieldResolutionKey& right) const noexcept;
    };

    [[nodiscard]] Result<std::shared_ptr<const ClassLayout>> build_layout(
        std::string class_name);
    [[nodiscard]] static Result<Value> default_value(
        std::string_view descriptor);
    [[nodiscard]] static std::string field_key(
        std::string_view declaring_class,
        std::string_view name,
        std::string_view descriptor);

    ClassRepository& classes_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string,
                       std::shared_ptr<const ClassLayout>,
                       StringHash,
                       StringEqual> layouts_;
    std::unordered_map<FieldResolutionKey,
                       FieldLocation,
                       FieldResolutionKeyHash,
                       FieldResolutionKeyEqual> resolved_fields_;
    std::unordered_map<std::string, FieldId> field_ids_;
    std::vector<std::optional<Value>> static_fields_;
    u32 next_field_id_ {1U};
};

} // namespace phoneme::vm
