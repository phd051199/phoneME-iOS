#include "phoneme/vm/ClassRepository.hpp"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <utility>

#include "phoneme/filesystem/FileSystem.hpp"
#include "phoneme/vm/BuiltinClasses.hpp"
#include "phoneme/vm/Descriptor.hpp"
#include "phoneme/vm/PerformanceCounters.hpp"
#include "phoneme/vm/Verifier.hpp"

namespace phoneme::vm
{
  namespace
  {
    [[nodiscard]] usize combine_hash(usize seed, usize value) noexcept
    {
      return seed ^ (value + static_cast<usize>(0x9E3779B9U) +
                     (seed << 6U) + (seed >> 2U));
    }

    [[nodiscard]] u64 verified_class_stamp(
        const archive::ZipEntry &entry) noexcept
    {
      return (static_cast<u64>(entry.crc32) << 32U) |
             static_cast<u64>(entry.uncompressed_size);
    }

    // KnightOnline's bundled offline server publishes the character list and
    // only then starts loading the embedded world. On slower runtimes the user
    // can select a character while map.Map.entrys is still empty; Player.setup
    // then dereferences Map.get_map_by_id(1)[0], the login exception is swallowed
    // by the application, and the client remains on its spinner forever.
    //
    // This is intentionally an exact, signature-gated compatibility rewrite:
    // move EmbeddedOfflineEngine.start() before send_char_part() without changing
    // the method length, constant pool, branches, or stack-map layout. It only
    // applies to the known offline io/Session class carrying the distinctive
    // character-part marker and exact byte sequence below.
    [[nodiscard]] bool patch_knight_offline_boot_order(
        std::string_view internal_name,
        std::vector<u8>& bytes) noexcept
    {
      if (internal_name != "io/Session") return false;

      constexpr std::string_view marker =
          "[OFFLINE] Immediate character-part sync (0 remote parts required)";
      const auto marker_begin = std::search(
          bytes.begin(), bytes.end(), marker.begin(), marker.end());
      if (marker_begin == bytes.end()) return false;

      // getclientin4 tail before the rewrite:
      //   aload_0; invokevirtual send_char_part
      //   aload_0; iconst_1; putfield get_in4
      //   invokestatic EmbeddedOfflineEngine.getInstance
      //   invokevirtual EmbeddedOfflineEngine.start; return
      constexpr std::array<u8, 16> before {
          0x2a, 0xb6, 0x01, 0x87,
          0x2a, 0x04, 0xb5, 0x00, 0x53,
          0xb8, 0x00, 0xd8,
          0xb6, 0x01, 0x88,
          0xb1,
      };
      // Same instructions and byte count, safe order:
      //   EmbeddedOfflineEngine.getInstance().start();
      //   send_char_part();
      //   get_in4 = true;
      constexpr std::array<u8, 16> after {
          0xb8, 0x00, 0xd8,
          0xb6, 0x01, 0x88,
          0x2a, 0xb6, 0x01, 0x87,
          0x2a, 0x04, 0xb5, 0x00, 0x53,
          0xb1,
      };

      auto match = std::search(
          bytes.begin(), bytes.end(), before.begin(), before.end());
      if (match == bytes.end()) return false;
      const auto second = std::search(
          match + static_cast<std::ptrdiff_t>(before.size()), bytes.end(),
          before.begin(), before.end());
      if (second != bytes.end()) return false;

      std::copy(after.begin(), after.end(), match);
      return true;
    }
  } // namespace

  usize ClassRepository::MethodCacheKeyHash::operator()(
      const MethodCacheKey& key) const noexcept
  {
    return (*this)(MethodCacheKeyView {
        .owner = key.owner,
        .name = key.name,
        .descriptor = key.descriptor,
    });
  }

  usize ClassRepository::MethodCacheKeyHash::operator()(
      MethodCacheKeyView key) const noexcept
  {
    usize result = std::hash<std::string_view>{}(key.owner);
    result = combine_hash(result, std::hash<std::string_view>{}(key.name));
    return combine_hash(
        result, std::hash<std::string_view>{}(key.descriptor));
  }

  bool ClassRepository::MethodCacheKeyEqual::operator()(
      const MethodCacheKey& left,
      const MethodCacheKey& right) const noexcept
  {
    return left.owner == right.owner && left.name == right.name &&
           left.descriptor == right.descriptor;
  }

  bool ClassRepository::MethodCacheKeyEqual::operator()(
      const MethodCacheKey& left,
      MethodCacheKeyView right) const noexcept
  {
    return left.owner == right.owner && left.name == right.name &&
           left.descriptor == right.descriptor;
  }

  bool ClassRepository::MethodCacheKeyEqual::operator()(
      MethodCacheKeyView left,
      const MethodCacheKey& right) const noexcept
  {
    return (*this)(right, left);
  }

  usize ClassRepository::AssignabilityCacheKeyHash::operator()(
      const AssignabilityCacheKey& key) const noexcept
  {
    return (*this)(AssignabilityCacheKeyView {
        .source = key.source,
        .target = key.target,
    });
  }

  usize ClassRepository::AssignabilityCacheKeyHash::operator()(
      AssignabilityCacheKeyView key) const noexcept
  {
    const usize source_hash = std::hash<std::string_view>{}(key.source);
    return combine_hash(
        source_hash, std::hash<std::string_view>{}(key.target));
  }

  bool ClassRepository::AssignabilityCacheKeyEqual::operator()(
      const AssignabilityCacheKey& left,
      const AssignabilityCacheKey& right) const noexcept
  {
    return left.source == right.source && left.target == right.target;
  }

  bool ClassRepository::AssignabilityCacheKeyEqual::operator()(
      const AssignabilityCacheKey& left,
      AssignabilityCacheKeyView right) const noexcept
  {
    return left.source == right.source && left.target == right.target;
  }

  bool ClassRepository::AssignabilityCacheKeyEqual::operator()(
      AssignabilityCacheKeyView left,
      const AssignabilityCacheKey& right) const noexcept
  {
    return (*this)(right, left);
  }

  Status ClassRepository::add_archive(std::string archive_path)
  {
    return add_archive_impl(
        std::move(archive_path), nullptr, 0U, nullptr);
  }

  Status ClassRepository::add_archive(
      std::string archive_path,
      const std::array<u8, 32>& archive_sha256,
      u32 verification_cache_version,
      const std::unordered_map<std::string, u64>& verified_classes)
  {
    return add_archive_impl(
        std::move(archive_path),
        &archive_sha256,
        verification_cache_version,
        &verified_classes);
  }

  Status ClassRepository::add_archive_impl(
      std::string archive_path,
      const std::array<u8, 32>* archive_sha256,
      u32 verification_cache_version,
      const std::unordered_map<std::string, u64>* verified_classes)
  {
    if (archive_path.empty())
    {
      return fail(ErrorCode::invalid_argument,
                  "classpath archive path must not be empty");
    }
    auto archive = archive::ZipArchive::open(archive_path);
    if (!archive)
    {
      return std::unexpected(archive.error());
    }

    std::scoped_lock lock(mutex_);
    const auto existing = std::find_if(
        archives_.begin(), archives_.end(),
        [&archive_path](const ClasspathArchive &candidate)
        {
          return candidate.path == archive_path;
        });
    if (existing == archives_.end())
    {
      const bool cache_enabled =
          archive_sha256 != nullptr && verified_classes != nullptr &&
          verification_cache_version ==
              ClassRepository::verification_cache_version();
      archives_.push_back(ClasspathArchive{
          .path = std::move(archive_path),
          .archive = std::move(*archive),
          .archive_sha256 = archive_sha256 != nullptr
              ? *archive_sha256
              : std::array<u8, 32> {},
          .verification_cache_version = cache_enabled
              ? verification_cache_version
              : 0U,
          .verified_classes = cache_enabled
              ? *verified_classes
              : std::unordered_map<std::string, u64> {},
          .verification_cache_enabled = cache_enabled,
      });
      cache_.clear();
      method_cache_.clear();
      declared_method_cache_.clear();
      assignability_cache_.clear();
      metadata_.clear();
    }
    return {};
  }

  std::unordered_map<std::string, u64> ClassRepository::verified_classes(
      std::string_view archive_path) const
  {
    std::scoped_lock lock(mutex_);
    const auto found = std::find_if(
        archives_.begin(), archives_.end(),
        [archive_path](const ClasspathArchive &candidate)
        {
          return candidate.path == archive_path;
        });
    if (found == archives_.end() || !found->verification_cache_enabled)
    {
      return {};
    }
    return found->verified_classes;
  }

  Result<std::shared_ptr<const classfile::ClassFile>> ClassRepository::load(
      std::string_view binary_name)
  {
    if (binary_name.empty())
    {
      return fail(ErrorCode::invalid_argument,
                  "class name must not be empty");
    }

    std::string normalized_storage;
    std::string_view internal_name = binary_name;
    if (binary_name.find('.') != std::string_view::npos)
    {
      normalized_storage = normalize_name(binary_name);
      internal_name = normalized_storage;
    }

    std::scoped_lock lock(mutex_);
    if (const auto iterator = cache_.find(internal_name);
        iterator != cache_.end())
    {
      PerformanceCounters::record_class_cache(true);
      return iterator->second;
    }
    PerformanceCounters::record_class_cache(false);

    auto loaded = load_uncached(internal_name);
    if (!loaded)
    {
      return std::unexpected(loaded.error());
    }
    auto published = metadata_.publish_class(*loaded);
    if (!published)
    {
      return std::unexpected(published.error());
    }
    cache_.insert_or_assign(std::string(internal_name), *loaded);
    return *loaded;
  }

  Result<ResolvedMethod> ClassRepository::resolve_method(
      std::string_view binary_name,
      std::string_view method_name,
      std::string_view descriptor)
  {
    if (method_name.empty() || descriptor.empty())
    {
      return fail(ErrorCode::invalid_argument,
                  "method name and descriptor must not be empty");
    }

    std::string normalized_storage;
    std::string_view normalized_name = binary_name;
    if (binary_name.find('.') != std::string_view::npos)
    {
      normalized_storage = normalize_name(binary_name);
      normalized_name = normalized_storage;
    }
    const MethodCacheKeyView cache_key {
        .owner = normalized_name,
        .name = method_name,
        .descriptor = descriptor,
    };
    {
      std::scoped_lock lock(mutex_);
      if (const auto cached = method_cache_.find(cache_key);
          cached != method_cache_.end())
      {
        PerformanceCounters::record_method_resolution(true, false);
        return cached->second;
      }
    }
    PerformanceCounters::record_method_resolution(false, false);
    std::string current(normalized_name);
    const std::string cache_owner = current;
    const auto cache_method = [this,
                               &cache_owner,
                               method_name,
                               descriptor](ResolvedMethod resolved) {
      std::scoped_lock lock(mutex_);
      const auto [iterator, inserted] = method_cache_.emplace(
          MethodCacheKey {
              .owner = cache_owner,
              .name = std::string(method_name),
              .descriptor = std::string(descriptor),
          },
          std::move(resolved));
      if (inserted)
        PerformanceCounters::record_metadata_key_construction();
      (void)inserted;
      return iterator->second;
    };

    std::unordered_set<std::string> visited;
    visited.reserve(16);
    std::vector<std::string> pending_interfaces;
    pending_interfaces.reserve(8);

    while (!current.empty())
    {
      if (!visited.insert(current).second)
      {
        return fail(ErrorCode::malformed_class,
                    "class hierarchy contains a cycle");
      }
      if (visited.size() > 256)
      {
        return fail(ErrorCode::malformed_class,
                    "class hierarchy exceeds the supported depth");
      }

      auto loaded = load(current);
      if (!loaded)
      {
        return std::unexpected(loaded.error());
      }
      if (const classfile::Method *method =
              (*loaded)->find_method(method_name, descriptor);
          method != nullptr)
      {
        auto runtime = metadata_.publish_method(*loaded, *method);
        if (!runtime)
        {
          return std::unexpected(runtime.error());
        }
        return cache_method(ResolvedMethod{
            .owner = *loaded,
            .method = method,
            .runtime = std::move(*runtime),
        });
      }
      for (const std::string &interface_name : (*loaded)->interfaces())
      {
        pending_interfaces.push_back(interface_name);
      }
      current = (*loaded)->super_name();
    }

    while (!pending_interfaces.empty())
    {
      std::string interface_name = std::move(pending_interfaces.back());
      pending_interfaces.pop_back();
      if (!visited.insert(interface_name).second)
      {
        continue;
      }
      if (visited.size() > 1'024)
      {
        return fail(ErrorCode::malformed_class,
                    "method hierarchy exceeds the supported depth");
      }
      auto loaded = load(interface_name);
      if (!loaded)
      {
        return std::unexpected(loaded.error());
      }
      if (const classfile::Method *method =
              (*loaded)->find_method(method_name, descriptor);
          method != nullptr)
      {
        auto runtime = metadata_.publish_method(*loaded, *method);
        if (!runtime)
        {
          return std::unexpected(runtime.error());
        }
        return cache_method(ResolvedMethod{
            .owner = *loaded,
            .method = method,
            .runtime = std::move(*runtime),
        });
      }
      for (const std::string &parent_interface : (*loaded)->interfaces())
      {
        pending_interfaces.push_back(parent_interface);
      }
    }

    return fail(ErrorCode::method_not_found,
                "method was not found in the class hierarchy: " +
                    normalize_name(binary_name) + "." +
                    std::string(method_name) + std::string(descriptor));
  }

  Result<ResolvedMethod> ClassRepository::resolve_declared_method(
      std::string_view binary_name,
      std::string_view method_name,
      std::string_view descriptor)
  {
    if (method_name.empty() || descriptor.empty())
    {
      return fail(ErrorCode::invalid_argument,
                  "method name and descriptor must not be empty");
    }
    std::string normalized_storage;
    std::string_view normalized = binary_name;
    if (binary_name.find('.') != std::string_view::npos)
    {
      normalized_storage = normalize_name(binary_name);
      normalized = normalized_storage;
    }
    const MethodCacheKeyView cache_key {
        .owner = normalized,
        .name = method_name,
        .descriptor = descriptor,
    };
    {
      std::scoped_lock lock(mutex_);
      if (const auto cached = declared_method_cache_.find(cache_key);
          cached != declared_method_cache_.end())
      {
        PerformanceCounters::record_method_resolution(true, true);
        return cached->second;
      }
    }
    PerformanceCounters::record_method_resolution(false, true);
    auto loaded = load(normalized);
    if (!loaded)
    {
      return std::unexpected(loaded.error());
    }
    const classfile::Method *method =
        (*loaded)->find_method(method_name, descriptor);
    if (method == nullptr)
    {
      return fail(ErrorCode::method_not_found,
                  "method was not declared by the requested class: " +
                      normalize_name(binary_name) + "." +
                      std::string(method_name) + std::string(descriptor));
    }
    auto runtime = metadata_.publish_method(*loaded, *method);
    if (!runtime)
    {
      return std::unexpected(runtime.error());
    }
    ResolvedMethod resolved {
        .owner = *loaded,
        .method = method,
        .runtime = std::move(*runtime),
    };
    std::scoped_lock lock(mutex_);
    const auto [iterator, inserted] = declared_method_cache_.emplace(
        MethodCacheKey {
            .owner = std::string(normalized),
            .name = std::string(method_name),
            .descriptor = std::string(descriptor),
        },
        std::move(resolved));
    if (inserted)
      PerformanceCounters::record_metadata_key_construction();
    return iterator->second;
  }

  Result<bool> ClassRepository::is_assignable(
      std::string_view source_name,
      std::string_view target_name)
  {
    if (source_name.empty() || target_name.empty())
    {
      return fail(ErrorCode::invalid_argument,
                  "assignability requires non-empty class names");
    }

    std::string source_storage;
    std::string target_storage;
    std::string_view source_view = source_name;
    std::string_view target_view = target_name;
    if (source_name.find('.') != std::string_view::npos)
    {
      source_storage = normalize_name(source_name);
      source_view = source_storage;
    }
    if (target_name.find('.') != std::string_view::npos)
    {
      target_storage = normalize_name(target_name);
      target_view = target_storage;
    }
    const AssignabilityCacheKeyView cache_key {
        .source = source_view,
        .target = target_view,
    };
    {
      std::scoped_lock lock(mutex_);
      if (const auto cached = assignability_cache_.find(cache_key);
          cached != assignability_cache_.end())
      {
        PerformanceCounters::record_assignability_cache(true);
        return cached->second;
      }
    }
    PerformanceCounters::record_assignability_cache(false);
    const std::string source(source_view);
    const std::string target(target_view);
    const auto cache_result = [this, &source, &target](bool value) {
      std::scoped_lock lock(mutex_);
      const auto [iterator, inserted] = assignability_cache_.insert_or_assign(
          AssignabilityCacheKey {
              .source = source,
              .target = target,
          },
          value);
      if (inserted)
        PerformanceCounters::record_metadata_key_construction();
      (void)iterator;
      return value;
    };
    if (source == target)
    {
      return cache_result(true);
    }

    const auto component_name = [](std::string_view descriptor)
        -> Result<std::string>
    {
      if (descriptor.empty())
      {
        return fail(ErrorCode::malformed_class,
                    "array descriptor has no component type");
      }
      if (descriptor.front() == '[')
      {
        return std::string(descriptor);
      }
      if (descriptor.front() == 'L' && descriptor.size() >= 3 &&
          descriptor.back() == ';')
      {
        return std::string(descriptor.substr(1, descriptor.size() - 2));
      }
      if (descriptor.size() == 1 &&
          std::string_view("ZCBSIFJD").find(descriptor.front()) !=
              std::string_view::npos)
      {
        return std::string(descriptor);
      }
      return fail(ErrorCode::malformed_class,
                  "array descriptor contains an invalid component type");
    };

    if (source.front() == '[')
    {
      if (target == "java/lang/Object" ||
          target == "java/lang/Cloneable" ||
          target == "java/io/Serializable")
      {
        return cache_result(true);
      }
      if (target.front() != '[')
      {
        return cache_result(false);
      }
      auto source_component = component_name(std::string_view(source).substr(1));
      auto target_component = component_name(std::string_view(target).substr(1));
      if (!source_component)
      {
        return std::unexpected(source_component.error());
      }
      if (!target_component)
      {
        return std::unexpected(target_component.error());
      }
      const bool source_primitive = source_component->size() == 1;
      const bool target_primitive = target_component->size() == 1;
      if (source_primitive || target_primitive)
      {
        return cache_result(*source_component == *target_component);
      }
      auto component_assignable = is_assignable(
          *source_component, *target_component);
      if (!component_assignable)
      {
        return std::unexpected(component_assignable.error());
      }
      return cache_result(*component_assignable);
    }
    if (target.front() == '[')
    {
      return cache_result(false);
    }

    std::vector<std::string> pending{source};
    std::unordered_set<std::string> visited;
    visited.reserve(16);
    while (!pending.empty())
    {
      std::string current = std::move(pending.back());
      pending.pop_back();
      if (!visited.insert(current).second)
      {
        continue;
      }
      if (visited.size() > 1'024)
      {
        return fail(ErrorCode::malformed_class,
                    "class hierarchy exceeds the assignability limit");
      }
      if (current == target)
      {
        return cache_result(true);
      }
      auto loaded = load(current);
      if (!loaded)
      {
        return std::unexpected(loaded.error());
      }
      if (!(*loaded)->super_name().empty())
      {
        pending.push_back((*loaded)->super_name());
      }
      for (const std::string &interface_name : (*loaded)->interfaces())
      {
        pending.push_back(interface_name);
      }
    }
    return cache_result(false);
  }

  Result<std::vector<u8>> ClassRepository::read_resource(
      std::string_view resource_name) const
  {
    auto normalized = filesystem::normalize_resource_path(resource_name);
    if (!normalized)
    {
      return std::unexpected(normalized.error());
    }

    std::scoped_lock lock(mutex_);
    for (const ClasspathArchive &classpath_archive : archives_)
    {
      const archive::ZipEntry *entry =
          classpath_archive.archive.find(*normalized);
      if (entry == nullptr || entry->name.ends_with('/'))
      {
        continue;
      }
      return classpath_archive.archive.read(*entry);
    }
    return fail(ErrorCode::class_not_found,
                "resource is not present on the application classpath: " +
                    *normalized);
  }

  void ClassRepository::clear() noexcept
  {
    std::scoped_lock lock(mutex_);
    archives_.clear();
    cache_.clear();
    method_cache_.clear();
    declared_method_cache_.clear();
    assignability_cache_.clear();
    metadata_.clear();
  }

  Result<std::shared_ptr<const classfile::ClassFile>>
  ClassRepository::load_uncached(std::string_view internal_name)
  {
    if (!internal_name.empty() && internal_name.front() == '[')
    {
      auto descriptor = parse_field_descriptor(internal_name);
      if (!descriptor || descriptor->kind != JavaTypeKind::array)
      {
        return fail(ErrorCode::malformed_class,
                    "invalid array class descriptor: " +
                        std::string(internal_name));
      }
      constexpr u16 kArrayFlags = 0x0001U | 0x0010U | 0x0400U;
      return std::make_shared<const classfile::ClassFile>(
          classfile::ClassFile::builtin(
              std::string(internal_name),
              "java/lang/Object",
              kArrayFlags,
              {},
              {},
              {"java/lang/Cloneable", "java/io/Serializable"}));
    }
    // A few vendor APIs are commonly bundled as application-specific
    // compatibility wrappers. Prefer those JAR-local implementations and use
    // Core's vendor classes only as a fallback when the application omitted
    // them. Standard CLDC/MIDP classes remain protected from shadowing.
    const bool prefer_archive =
        internal_name.starts_with("com/sprintpcs/media/") ||
        internal_name == "com/samsung/util/AudioClip";
    if (!prefer_archive)
    {
      if (auto builtin = load_builtin_class(internal_name); builtin)
      {
        return builtin;
      }
    }

    const std::string entry_name = std::string(internal_name) + ".class";
    for (ClasspathArchive &classpath_archive : archives_)
    {
      const archive::ZipEntry *entry =
          classpath_archive.archive.find(entry_name);
      if (entry == nullptr)
      {
        continue;
      }
      auto bytes = classpath_archive.archive.read(*entry);
      if (!bytes)
      {
        return std::unexpected(bytes.error());
      }
      const bool compatibility_patched =
          patch_knight_offline_boot_order(internal_name, *bytes);
      auto parsed = classfile::ClassFile::parse(*bytes);
      if (!parsed)
      {
        return std::unexpected(parsed.error());
      }
      if (parsed->name() != internal_name)
      {
        return fail(ErrorCode::malformed_class,
                    "classpath entry name does not match class declaration");
      }
      const u64 verification_stamp = verified_class_stamp(*entry);
      bool already_verified = false;
      if (classpath_archive.verification_cache_enabled)
      {
        const auto cached = classpath_archive.verified_classes.find(
            std::string(internal_name));
        already_verified =
            cached != classpath_archive.verified_classes.end() &&
            cached->second == verification_stamp &&
            !compatibility_patched;
      }
      if (!already_verified)
      {
        auto verified = verify_class(*parsed);
        if (!verified)
        {
          return fail(verified.error().code,
                      std::string(internal_name) + "." +
                          verified.error().message);
        }
        if (classpath_archive.verification_cache_enabled)
        {
          classpath_archive.verified_classes.insert_or_assign(
              std::string(internal_name), verification_stamp);
        }
      }
      return std::make_shared<const classfile::ClassFile>(std::move(*parsed));
    }

    if (prefer_archive)
    {
      if (auto builtin = load_builtin_class(internal_name); builtin)
      {
        return builtin;
      }
    }

    return fail(ErrorCode::class_not_found,
                "class is neither built into Core nor present in the application JAR: " +
                    std::string(internal_name));
  }

  std::string ClassRepository::normalize_name(std::string_view binary_name)
  {
    std::string normalized(binary_name);
    std::replace(normalized.begin(), normalized.end(), '.', '/');
    while (!normalized.empty() && normalized.front() == '/')
    {
      normalized.erase(normalized.begin());
    }
    return normalized;
  }

} // namespace phoneme::vm
