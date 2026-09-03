#include "phoneme/vm/ClassLayout.hpp"

#include <algorithm>
#include <bit>
#include <unordered_set>
#include <utility>

#include "phoneme/vm/PerformanceCounters.hpp"

namespace phoneme::vm
{
  namespace
  {

    constexpr u16 kAccStatic = 0x0008U;
    constexpr u16 kAccInterface = 0x0200U;
    constexpr u16 kAccAbstract = 0x0400U;

    [[nodiscard]] std::string normalize_name(std::string_view name)
    {
      std::string normalized(name);
      std::replace(normalized.begin(), normalized.end(), '.', '/');
      return normalized;
    }

    [[nodiscard]] Result<ValueKind> field_value_kind(
        std::string_view descriptor)
    {
      auto parsed = parse_field_descriptor(descriptor);
      if (!parsed)
      {
        return std::unexpected(parsed.error());
      }
      switch (parsed->kind)
      {
      case JavaTypeKind::boolean:
      case JavaTypeKind::byte:
      case JavaTypeKind::character:
      case JavaTypeKind::short_integer:
      case JavaTypeKind::integer:
        return ValueKind::int32;
      case JavaTypeKind::float32:
        return ValueKind::float32;
      case JavaTypeKind::long_integer:
        return ValueKind::int64;
      case JavaTypeKind::float64:
        return ValueKind::float64;
      case JavaTypeKind::reference:
      case JavaTypeKind::array:
        return ValueKind::reference;
      case JavaTypeKind::void_type:
        return fail(ErrorCode::malformed_class,
                    "field descriptor cannot use void");
      }
      return fail(ErrorCode::internal_error,
                  "field descriptor has an unknown value kind");
    }

    [[nodiscard]] usize combine_hash(usize seed, usize value) noexcept
    {
      return seed ^ (value + static_cast<usize>(0x9E3779B9U) +
                     (seed << 6U) + (seed >> 2U));
    }

  } // namespace

  usize ClassStateRegistry::FieldResolutionKeyHash::operator()(
      const FieldResolutionKey& key) const noexcept
  {
    return (*this)(FieldResolutionKeyView {
        .owner = key.owner,
        .name = key.name,
        .descriptor = key.descriptor,
        .require_static = key.require_static,
    });
  }

  usize ClassStateRegistry::FieldResolutionKeyHash::operator()(
      FieldResolutionKeyView key) const noexcept
  {
    usize result = std::hash<std::string_view>{}(key.owner);
    result = combine_hash(result, std::hash<std::string_view>{}(key.name));
    result = combine_hash(
        result, std::hash<std::string_view>{}(key.descriptor));
    return combine_hash(result, key.require_static ? 1U : 0U);
  }

  bool ClassStateRegistry::FieldResolutionKeyEqual::operator()(
      const FieldResolutionKey& left,
      const FieldResolutionKey& right) const noexcept
  {
    return left.owner == right.owner && left.name == right.name &&
           left.descriptor == right.descriptor &&
           left.require_static == right.require_static;
  }

  bool ClassStateRegistry::FieldResolutionKeyEqual::operator()(
      const FieldResolutionKey& left,
      FieldResolutionKeyView right) const noexcept
  {
    return left.owner == right.owner && left.name == right.name &&
           left.descriptor == right.descriptor &&
           left.require_static == right.require_static;
  }

  bool ClassStateRegistry::FieldResolutionKeyEqual::operator()(
      FieldResolutionKeyView left,
      const FieldResolutionKey& right) const noexcept
  {
    return (*this)(right, left);
  }

  Result<std::shared_ptr<const ClassLayout>> ClassStateRegistry::layout(
      std::string_view class_name)
  {
    if (class_name.empty())
    {
      return fail(ErrorCode::invalid_argument,
                  "class layout name must not be empty");
    }

    std::string normalized_storage;
    std::string_view normalized = class_name;
    if (class_name.find('.') != std::string_view::npos)
    {
      normalized_storage = normalize_name(class_name);
      normalized = normalized_storage;
    }

    {
      std::scoped_lock lock(mutex_);
      if (const auto iterator = layouts_.find(normalized);
          iterator != layouts_.end())
      {
        return iterator->second;
      }
    }

    auto built = build_layout(std::string(normalized));
    if (!built)
    {
      return std::unexpected(built.error());
    }

    std::scoped_lock lock(mutex_);
    const auto [iterator, inserted] = layouts_.emplace(
        std::string(normalized), *built);
    (void)inserted;
    return iterator->second;
  }

  Result<FieldLocation> ClassStateRegistry::resolve_field(
      std::string_view owner,
      std::string_view name,
      std::string_view descriptor,
      bool require_static)
  {
    if (owner.empty() || name.empty() || descriptor.empty())
    {
      return fail(ErrorCode::invalid_argument,
                  "field reference is incomplete");
    }

    std::string normalized_storage;
    std::string_view normalized_owner = owner;
    if (owner.find('.') != std::string_view::npos)
    {
      normalized_storage = normalize_name(owner);
      normalized_owner = normalized_storage;
    }
    const FieldResolutionKeyView cache_key {
        .owner = normalized_owner,
        .name = name,
        .descriptor = descriptor,
        .require_static = require_static,
    };
    {
      std::scoped_lock lock(mutex_);
      if (const auto cached = resolved_fields_.find(cache_key);
          cached != resolved_fields_.end())
      {
        PerformanceCounters::record_field_resolution(true);
        return cached->second;
      }
    }
    PerformanceCounters::record_field_resolution(false);
    std::string current(normalized_owner);
    const std::string cache_owner = current;
    const auto cache_location = [this,
                                 &cache_owner,
                                 name,
                                 descriptor,
                                 require_static](FieldLocation location)
        -> Result<FieldLocation>
    {
      std::scoped_lock lock(mutex_);
      const FieldResolutionKeyView lookup {
          .owner = cache_owner,
          .name = name,
          .descriptor = descriptor,
          .require_static = require_static,
      };
      if (const auto cached = resolved_fields_.find(lookup);
          cached != resolved_fields_.end())
      {
        return cached->second;
      }
      if (const auto canonical = field_ids_.find(location.storage_key);
          canonical != field_ids_.end())
      {
        location.id = canonical->second;
      }
      else
      {
        if (next_field_id_ == 0U)
        {
          return fail(ErrorCode::overflow,
                      "runtime field ID space is exhausted");
        }
        location.id = FieldId {next_field_id_++};
        field_ids_.emplace(location.storage_key, location.id);
      }
      const auto [iterator, inserted] = resolved_fields_.emplace(
          FieldResolutionKey {
              .owner = cache_owner,
              .name = std::string(name),
              .descriptor = std::string(descriptor),
              .require_static = require_static,
          },
          std::move(location));
      if (inserted)
        PerformanceCounters::record_metadata_key_construction();
      return iterator->second;
    };

    // JVMS 5.4.3.2 field resolution checks the declaring class first, then
    // its direct superinterfaces recursively, and only then its superclass.
    // Many MIDP codebases expose constants through interfaces while emitting a
    // Fieldref whose symbolic owner is the implementing class.
    std::unordered_set<std::string> visited;
    visited.reserve(16U);
    const auto resolve_in_hierarchy =
        [&](auto&& self,
            const std::string& candidate,
            usize depth) -> Result<std::optional<FieldLocation>>
    {
      if (candidate.empty())
        return std::optional<FieldLocation>{};
      if (depth > 256U)
      {
        return fail(ErrorCode::malformed_class,
                    "field hierarchy exceeds the supported depth");
      }
      if (!visited.insert(candidate).second)
        return std::optional<FieldLocation>{};

      auto loaded = classes_.load(candidate);
      if (!loaded)
        return std::unexpected(loaded.error());

      for (const classfile::Field &field : (*loaded)->fields())
      {
        if (field.name != name || field.descriptor != descriptor)
          continue;

        const bool is_static = (field.access_flags & kAccStatic) != 0;
        if (is_static != require_static)
        {
          return fail(ErrorCode::invalid_state,
                      "field staticness does not match the bytecode opcode");
        }

        auto value_kind = field_value_kind(descriptor);
        if (!value_kind)
          return std::unexpected(value_kind.error());
        const auto runtime_class = classes_.metadata().find_class(candidate);
        if (runtime_class == nullptr)
        {
          return fail(ErrorCode::internal_error,
                      "loaded field owner has no runtime class metadata");
        }

        if (is_static)
        {
          auto location = cache_location(FieldLocation{
              .declaring_class_id = runtime_class->id,
              .declaring_class = candidate,
              .name = std::string(name),
              .descriptor = std::string(descriptor),
              .index = 0,
              .value_kind = *value_kind,
              .is_static = true,
              .constant_value_index = field.constant_value_index,
              .storage_key = field_key(candidate, name, descriptor),
          });
          if (!location)
            return std::unexpected(location.error());
          return std::optional<FieldLocation>(std::move(*location));
        }

        auto declaring_layout = layout(candidate);
        if (!declaring_layout)
          return std::unexpected(declaring_layout.error());
        const std::string key = field_key(candidate, name, descriptor);
        const auto offset = (*declaring_layout)->instance_fields.find(key);
        if (offset == (*declaring_layout)->instance_fields.end())
        {
          return fail(ErrorCode::internal_error,
                      "instance field is absent from its class layout");
        }
        auto location = cache_location(FieldLocation{
            .declaring_class_id = runtime_class->id,
            .declaring_class = candidate,
            .name = std::string(name),
            .descriptor = std::string(descriptor),
            .index = offset->second,
            .value_kind = *value_kind,
            .is_static = false,
            .constant_value_index = std::nullopt,
            .storage_key = key,
        });
        if (!location)
          return std::unexpected(location.error());
        return std::optional<FieldLocation>(std::move(*location));
      }

      for (const std::string& interface_name : (*loaded)->interfaces())
      {
        auto resolved = self(self, interface_name, depth + 1U);
        if (!resolved)
          return std::unexpected(resolved.error());
        if (resolved->has_value())
          return resolved;
      }

      return self(self, (*loaded)->super_name(), depth + 1U);
    };

    auto resolved = resolve_in_hierarchy(
        resolve_in_hierarchy, current, 0U);
    if (!resolved)
      return std::unexpected(resolved.error());
    if (resolved->has_value())
      return std::move(**resolved);

    return fail(ErrorCode::class_not_found,
                "field was not found in the class hierarchy: " +
                    normalize_name(owner) + "." + std::string(name) +
                    std::string(descriptor));
  }

  Result<ObjectRef> ClassStateRegistry::allocate_instance(
      Heap &heap,
      std::string_view class_name)
  {
    auto class_layout = layout(class_name);
    if (!class_layout)
    {
      return std::unexpected(class_layout.error());
    }
    if (!(*class_layout)->instantiable)
    {
      return fail(ErrorCode::invalid_argument,
                  "cannot allocate an interface or abstract class");
    }
    return heap.allocate_object(
        (*class_layout)->class_name,
        std::span<const Value>((*class_layout)->instance_defaults));
  }

  Result<ObjectRef> ClassStateRegistry::allocate_text_instance(
      Heap &heap,
      std::string_view class_name,
      std::u16string text)
  {
    auto class_layout = layout(class_name);
    if (!class_layout)
      return std::unexpected(class_layout.error());
    if (!(*class_layout)->instantiable)
    {
      return fail(ErrorCode::invalid_argument,
                  "cannot allocate an interface or abstract class");
    }
    return heap.allocate_text_object(
        (*class_layout)->class_name,
        (*class_layout)->instance_defaults,
        std::move(text));
  }

  Result<Value> ClassStateRegistry::static_field(const FieldLocation &field)
  {
    if (!field.is_static)
    {
      return fail(ErrorCode::invalid_argument,
                  "requested field is not static");
    }
    if (!field.id.valid())
    {
      return fail(ErrorCode::invalid_argument,
                  "static field has no runtime field ID");
    }

    {
      std::scoped_lock lock(mutex_);
      const usize slot = static_cast<usize>(field.id.value);
      if (slot < static_fields_.size() && static_fields_[slot].has_value())
        return *static_fields_[slot];
    }

    auto initial = default_value(field.descriptor);
    if (!initial)
    {
      return std::unexpected(initial.error());
    }
    if (field.constant_value_index.has_value())
    {
      auto owner = classes_.load(field.declaring_class);
      if (!owner)
        return std::unexpected(owner.error());
      auto constant = (*owner)->constant(*field.constant_value_index);
      if (!constant)
        return std::unexpected(constant.error());
      switch ((*constant)->kind)
      {
      case classfile::ConstantKind::integer:
        initial = Value::from_int(static_cast<i32>(
            static_cast<u32>((*constant)->bits)));
        break;
      case classfile::ConstantKind::float32:
        initial = Value::from_float(std::bit_cast<float>(
            static_cast<u32>((*constant)->bits)));
        break;
      case classfile::ConstantKind::long64:
        initial = Value::from_long(static_cast<i64>((*constant)->bits));
        break;
      case classfile::ConstantKind::float64:
        initial = Value::from_double(
            std::bit_cast<double>((*constant)->bits));
        break;
      case classfile::ConstantKind::string_ref:
        break;
      default:
        return fail(ErrorCode::malformed_class,
                    "field ConstantValue has an unsupported constant kind");
      }
    }

    std::scoped_lock lock(mutex_);
    const usize slot = static_cast<usize>(field.id.value);
    if (static_fields_.size() <= slot)
      static_fields_.resize(slot + 1U);
    if (!static_fields_[slot].has_value())
      static_fields_[slot] = *initial;
    return *static_fields_[slot];
  }

  Result<Value> ClassStateRegistry::static_field(FieldId field_id) const
  {
    if (!field_id.valid())
    {
      return fail(ErrorCode::invalid_argument,
                  "static field has no runtime field ID");
    }
    std::scoped_lock lock(mutex_);
    const usize slot = static_cast<usize>(field_id.value);
    if (slot >= static_fields_.size() || !static_fields_[slot].has_value())
    {
      return fail(ErrorCode::invalid_state,
                  "quick static field was not prepared before access");
    }
    return *static_fields_[slot];
  }

  Result<Value> ClassStateRegistry::vm_static_field(FieldId field_id) const
  {
    if (!field_id.valid())
    {
      return fail(ErrorCode::invalid_argument,
                  "static field has no runtime field ID");
    }
    const usize slot = static_cast<usize>(field_id.value);
    if (slot >= static_fields_.size() || !static_fields_[slot].has_value())
    {
      return fail(ErrorCode::invalid_state,
                  "quick static field was not prepared before access");
    }
    return *static_fields_[slot];
  }

  Status ClassStateRegistry::set_static_field(const FieldLocation &field,
                                              Value value)
  {
    if (!field.is_static)
    {
      return fail(ErrorCode::invalid_argument,
                  "requested field is not static");
    }
    if (!field.id.valid())
    {
      return fail(ErrorCode::invalid_argument,
                  "static field has no runtime field ID");
    }
    if (value.kind() != field.value_kind)
    {
      return fail(ErrorCode::invalid_argument,
                  "static field value does not match its descriptor");
    }

    std::scoped_lock lock(mutex_);
    const usize slot = static_cast<usize>(field.id.value);
    if (static_fields_.size() <= slot)
      static_fields_.resize(slot + 1U);
    static_fields_[slot] = value;
    return {};
  }

  Status ClassStateRegistry::set_static_field(FieldId field_id,
                                              ValueKind expected_kind,
                                              Value value)
  {
    if (!field_id.valid())
    {
      return fail(ErrorCode::invalid_argument,
                  "static field has no runtime field ID");
    }
    if (value.kind() != expected_kind)
    {
      return fail(ErrorCode::invalid_argument,
                  "static field value does not match its descriptor");
    }
    std::scoped_lock lock(mutex_);
    const usize slot = static_cast<usize>(field_id.value);
    if (static_fields_.size() <= slot)
      static_fields_.resize(slot + 1U);
    static_fields_[slot] = value;
    return {};
  }

  Status ClassStateRegistry::vm_set_static_field(FieldId field_id,
                                                 ValueKind expected_kind,
                                                 Value value)
  {
    if (!field_id.valid())
    {
      return fail(ErrorCode::invalid_argument,
                  "static field has no runtime field ID");
    }
    if (value.kind() != expected_kind)
    {
      return fail(ErrorCode::invalid_argument,
                  "static field value does not match its descriptor");
    }
    const usize slot = static_cast<usize>(field_id.value);
    if (slot >= static_fields_.size() || !static_fields_[slot].has_value())
    {
      return fail(ErrorCode::invalid_state,
                  "quick static field was not prepared before store");
    }
    static_fields_[slot] = value;
    return {};
  }

  void ClassStateRegistry::append_reference_roots(
      std::vector<ObjectRef> &roots) const
  {
    std::scoped_lock lock(mutex_);
    for (const auto& slot : static_fields_)
    {
      if (!slot.has_value() || slot->kind() != ValueKind::reference)
        continue;
      auto reference = slot->as_reference();
      if (reference && !reference->is_null())
      {
        roots.push_back(*reference);
      }
    }
  }

  void ClassStateRegistry::clear() noexcept
  {
    std::scoped_lock lock(mutex_);
    layouts_.clear();
    resolved_fields_.clear();
    field_ids_.clear();
    static_fields_.clear();
    next_field_id_ = 1U;
  }

  Result<std::shared_ptr<const ClassLayout>> ClassStateRegistry::build_layout(
      std::string class_name)
  {
    auto loaded = classes_.load(class_name);
    if (!loaded)
    {
      return std::unexpected(loaded.error());
    }

    ClassLayout result{
        .class_name = class_name,
        .super_name = (*loaded)->super_name(),
        .instantiable =
            ((*loaded)->access_flags() & (kAccInterface | kAccAbstract)) == 0U,
        .instance_field_slots = 0,
        .instance_fields = {},
        .instance_defaults = {},
    };

    if (!result.super_name.empty())
    {
      auto parent = layout(result.super_name);
      if (!parent)
      {
        return std::unexpected(parent.error());
      }
      result.instance_field_slots = (*parent)->instance_field_slots;
      result.instance_fields = (*parent)->instance_fields;
      result.instance_defaults = (*parent)->instance_defaults;
    }

    for (const classfile::Field &field : (*loaded)->fields())
    {
      if ((field.access_flags & kAccStatic) != 0)
      {
        continue;
      }
      auto descriptor = parse_field_descriptor(field.descriptor);
      if (!descriptor)
      {
        return std::unexpected(descriptor.error());
      }
      if (descriptor->kind == JavaTypeKind::void_type)
      {
        return fail(ErrorCode::malformed_class,
                    "instance field cannot use the void descriptor");
      }

      const std::string key = field_key(class_name,
                                        field.name,
                                        field.descriptor);
      auto initial = default_value(field.descriptor);
      if (!initial)
      {
        return std::unexpected(initial.error());
      }
      result.instance_fields.insert_or_assign(key,
                                              result.instance_field_slots);
      result.instance_defaults.push_back(*initial);
      ++result.instance_field_slots;
    }

    return std::make_shared<const ClassLayout>(std::move(result));
  }

  Result<Value> ClassStateRegistry::default_value(
      std::string_view descriptor)
  {
    auto parsed = parse_field_descriptor(descriptor);
    if (!parsed)
    {
      return std::unexpected(parsed.error());
    }

    switch (parsed->kind)
    {
    case JavaTypeKind::boolean:
    case JavaTypeKind::byte:
    case JavaTypeKind::character:
    case JavaTypeKind::short_integer:
    case JavaTypeKind::integer:
      return Value::from_int(0);
    case JavaTypeKind::float32:
      return Value::from_float(0.0F);
    case JavaTypeKind::long_integer:
      return Value::from_long(0);
    case JavaTypeKind::float64:
      return Value::from_double(0.0);
    case JavaTypeKind::reference:
    case JavaTypeKind::array:
      return Value::from_reference({});
    case JavaTypeKind::void_type:
      return fail(ErrorCode::malformed_class,
                  "void is not a valid field descriptor");
    }
    return fail(ErrorCode::internal_error,
                "unknown field descriptor kind");
  }

  std::string ClassStateRegistry::field_key(
      std::string_view declaring_class,
      std::string_view name,
      std::string_view descriptor)
  {
    std::string key;
    key.reserve(declaring_class.size() + name.size() + descriptor.size() + 2);
    key.append(declaring_class);
    key.push_back('#');
    key.append(name);
    key.push_back(':');
    key.append(descriptor);
    return key;
  }

} // namespace phoneme::vm
