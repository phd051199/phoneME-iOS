#include "phoneme/runtime/SuiteStore.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace phoneme::runtime {
namespace {

[[nodiscard]] std::string class_entry_name(std::string_view binary_name) {
    std::string entry(binary_name);
    std::replace(entry.begin(), entry.end(), '.', '/');
    entry.append(".class");
    return entry;
}

[[nodiscard]] std::filesystem::path suites_directory(
    const std::string& root_path) {
    return std::filesystem::path(root_path) / "suites";
}

[[nodiscard]] std::filesystem::path suite_directory(
    const std::string& root_path,
    SuiteId id) {
    return suites_directory(root_path) / std::to_string(id.value);
}

[[nodiscard]] std::filesystem::path stage_directory(
    const std::string& root_path,
    SuiteId id) {
    return suites_directory(root_path) /
           (".stage-" + std::to_string(id.value));
}

[[nodiscard]] std::filesystem::path backup_directory(
    const std::string& root_path,
    SuiteId id) {
    return suites_directory(root_path) /
           (".backup-" + std::to_string(id.value));
}

[[nodiscard]] std::filesystem::path delete_directory(
    const std::string& root_path,
    SuiteId id) {
    return suites_directory(root_path) /
           (".delete-" + std::to_string(id.value));
}

[[nodiscard]] Status filesystem_error(std::string message,
                                      const std::error_code& error) {
    if (error) {
        message.append(": ");
        message.append(error.message());
    }
    return fail(ErrorCode::io_error, std::move(message));
}

[[nodiscard]] Status ensure_directory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        return filesystem_error("unable to create suite directory", error);
    }
    if (!std::filesystem::is_directory(path, error) || error) {
        return filesystem_error("suite path is not a directory", error);
    }
    return {};
}

[[nodiscard]] Status remove_tree(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) {
        return filesystem_error("unable to remove suite transaction path", error);
    }
    return {};
}

[[nodiscard]] Result<bool> path_exists(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return std::unexpected(
            filesystem_error("unable to inspect suite path", error).error());
    }
    return exists;
}

[[nodiscard]] Status rename_path(const std::filesystem::path& source,
                                 const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error) {
        return filesystem_error("unable to rename suite transaction path", error);
    }
    return {};
}

[[nodiscard]] Status sync_file(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return fail(ErrorCode::io_error,
                    "unable to open copied suite file for sync: " +
                        std::string(std::strerror(errno)));
    }
    const int result = ::fsync(descriptor);
    const int saved_errno = errno;
    static_cast<void>(::close(descriptor));
    if (result != 0) {
        return fail(ErrorCode::io_error,
                    "unable to sync copied suite file: " +
                        std::string(std::strerror(saved_errno)));
    }
    return {};
}

[[nodiscard]] Status sync_directory(const std::filesystem::path& path,
                                    std::string_view operation) {
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return fail(ErrorCode::io_error,
                    std::string(operation) + ": " +
                        std::string(std::strerror(errno)));
    }
    const int result = ::fsync(descriptor);
    const int saved_errno = errno;
    static_cast<void>(::close(descriptor));
    if (result != 0) {
        return fail(ErrorCode::io_error,
                    std::string(operation) + ": " +
                        std::string(std::strerror(saved_errno)));
    }
    return {};
}

[[nodiscard]] Status copy_file_synced(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const SuiteStoreFaultInjector& fault_injector,
    SuiteStoreFaultPoint fault_point) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error) || error) {
        return filesystem_error("suite import path is not a regular file", error);
    }
    std::filesystem::copy_file(
        source, destination,
        std::filesystem::copy_options::overwrite_existing,
        error);
    if (error) {
        return filesystem_error("unable to copy suite into managed storage", error);
    }
    if (fault_injector) {
        auto injected = fault_injector(fault_point);
        if (!injected) return injected;
    }
    return sync_file(destination);
}

[[nodiscard]] std::string relative_jar_path(SuiteId id) {
    return "suites/" + std::to_string(id.value) + "/app.jar";
}

[[nodiscard]] std::string relative_jad_path(SuiteId id) {
    return "suites/" + std::to_string(id.value) + "/app.jad";
}

[[nodiscard]] bool digest_equal(const std::array<u8, 32>& left,
                                const std::array<u8, 32>& right) noexcept {
    return std::equal(left.begin(), left.end(), right.begin(), right.end());
}

[[nodiscard]] bool validation_stamp_equal(
    const SuiteFileValidationStamp& left,
    const SuiteFileValidationStamp& right) noexcept {
    return left.valid == right.valid && left.size == right.size &&
        left.device == right.device && left.inode == right.inode &&
        left.modified_seconds == right.modified_seconds &&
        left.modified_nanoseconds == right.modified_nanoseconds &&
        left.changed_seconds == right.changed_seconds &&
        left.changed_nanoseconds == right.changed_nanoseconds;
}

[[nodiscard]] Result<SuiteFileValidationStamp> file_validation_stamp(
    const std::filesystem::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return fail(ErrorCode::io_error,
                    "unable to stat managed suite file: " +
                        std::string(std::strerror(errno)));
    }
    if (!S_ISREG(info.st_mode)) {
        return fail(ErrorCode::io_error,
                    "managed suite path is not a regular file");
    }
#if defined(__APPLE__)
    const auto modified = info.st_mtimespec;
    const auto changed = info.st_ctimespec;
#else
    const auto modified = info.st_mtim;
    const auto changed = info.st_ctim;
#endif
    return SuiteFileValidationStamp {
        .valid = true,
        .size = static_cast<u64>(info.st_size),
        .device = static_cast<u64>(info.st_dev),
        .inode = static_cast<u64>(info.st_ino),
        .modified_seconds = static_cast<u64>(modified.tv_sec),
        .modified_nanoseconds = static_cast<u64>(modified.tv_nsec),
        .changed_seconds = static_cast<u64>(changed.tv_sec),
        .changed_nanoseconds = static_cast<u64>(changed.tv_nsec),
    };
}

[[nodiscard]] std::string trim_ascii(std::string_view value) {
    usize begin = 0U;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    usize end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

void append_permissions(std::vector<std::string>& output,
                        std::string_view value) {
    usize offset = 0U;
    while (offset <= value.size()) {
        const usize comma = value.find(',', offset);
        const usize end = comma == std::string_view::npos ? value.size() : comma;
        std::string permission = trim_ascii(value.substr(offset, end - offset));
        if (!permission.empty() &&
            std::find(output.begin(), output.end(), permission) == output.end()) {
            output.push_back(std::move(permission));
        }
        if (comma == std::string_view::npos) break;
        offset = comma + 1U;
    }
}

[[nodiscard]] SuiteDescriptor descriptor_from_record(
    const SuiteDatabaseRecord& record) {
    std::vector<std::string> required_permissions;
    std::vector<std::string> optional_permissions;
    if (const auto iterator = record.properties.find("MIDlet-Permissions");
        iterator != record.properties.end()) {
        append_permissions(required_permissions, iterator->second);
    }
    if (const auto iterator = record.properties.find("MIDlet-Permissions-Opt");
        iterator != record.properties.end()) {
        append_permissions(optional_permissions, iterator->second);
    }
    std::erase_if(optional_permissions,
                  [&required_permissions](const std::string& permission) {
                      return std::find(required_permissions.begin(),
                                       required_permissions.end(), permission) !=
                          required_permissions.end();
                  });

    return SuiteDescriptor {
        .name = record.name,
        .vendor = record.vendor,
        .version = record.version,
        .jar_url = {},
        .declared_jar_size = record.declared_jar_size,
        .midlet_classes = record.midlet_classes,
        .declared_required_permissions = std::move(required_permissions),
        .declared_optional_permissions = std::move(optional_permissions),
        .declared_permissions = record.declared_permissions,
        .has_permission_declarations = record.has_permission_declarations,
        .trust_evidence = {},
        .properties = record.properties,
        .identity_key = record.identity_key,
        .identity_sha256 = record.identity_sha256,
        .archive_sha256 = record.archive_sha256,
        .archive_crc32 = record.archive_crc32,
        .archive_size = record.archive_size,
    };
}

[[nodiscard]] Result<bool> directory_matches_validation_stamp(
    const std::filesystem::path& directory,
    const SuiteDatabaseRecord& record,
    SuiteDescriptor* validated_descriptor) {
    if (record.has_signature_metadata || !record.jar_validation_stamp.valid) {
        return false;
    }
    auto jar_stamp = file_validation_stamp(directory / "app.jar");
    if (!jar_stamp) {
        if (jar_stamp.error().code == ErrorCode::io_error) return false;
        return std::unexpected(jar_stamp.error());
    }
    if (!validation_stamp_equal(*jar_stamp, record.jar_validation_stamp) ||
        jar_stamp->size != record.archive_size) {
        return false;
    }
    if (record.jad_relative_path.empty()) {
        if (record.jad_validation_stamp.valid) return false;
    } else {
        if (!record.jad_validation_stamp.valid) return false;
        auto jad_stamp = file_validation_stamp(directory / "app.jad");
        if (!jad_stamp) {
            if (jad_stamp.error().code == ErrorCode::io_error) return false;
            return std::unexpected(jad_stamp.error());
        }
        if (!validation_stamp_equal(*jad_stamp, record.jad_validation_stamp)) {
            return false;
        }
    }
    if (validated_descriptor != nullptr) {
        *validated_descriptor = descriptor_from_record(record);
    }
    return true;
}

[[nodiscard]] Result<bool> refresh_validation_stamps(
    SuiteDatabaseRecord& record,
    const std::filesystem::path& directory,
    const SuiteDescriptor& descriptor) {
    auto jar_stamp = file_validation_stamp(directory / "app.jar");
    if (!jar_stamp) return std::unexpected(jar_stamp.error());

    SuiteFileValidationStamp jad_stamp;
    if (!record.jad_relative_path.empty()) {
        auto stamp = file_validation_stamp(directory / "app.jad");
        if (!stamp) return std::unexpected(stamp.error());
        jad_stamp = *stamp;
    }

    const bool has_signature_metadata =
        descriptor.trust_evidence.has_signature_metadata();
    const bool changed =
        record.has_signature_metadata != has_signature_metadata ||
        !validation_stamp_equal(record.jar_validation_stamp, *jar_stamp) ||
        !validation_stamp_equal(record.jad_validation_stamp, jad_stamp);
    record.has_signature_metadata = has_signature_metadata;
    record.jar_validation_stamp = *jar_stamp;
    record.jad_validation_stamp = jad_stamp;
    return changed;
}

[[nodiscard]] Result<bool> directory_matches_record(
    const std::filesystem::path& directory,
    const SuiteDatabaseRecord& record,
    const SuiteInstallerLimits& limits,
    SuiteDescriptor* validated_descriptor = nullptr) {
    auto cached = directory_matches_validation_stamp(
        directory, record, validated_descriptor);
    if (!cached) return std::unexpected(cached.error());
    if (*cached) return true;

    const std::filesystem::path jar_path = directory / "app.jar";
    const std::optional<std::string> jad_path = record.jad_relative_path.empty()
        ? std::nullopt
        : std::optional<std::string>((directory / "app.jad").string());
    auto descriptor = SuiteInstaller::inspect(
        jar_path.string(), jad_path, limits);
    if (!descriptor) {
        if (descriptor.error().code == ErrorCode::io_error ||
            descriptor.error().code == ErrorCode::malformed_archive ||
            descriptor.error().code == ErrorCode::checksum_mismatch ||
            descriptor.error().code == ErrorCode::class_not_found ||
            descriptor.error().code == ErrorCode::invalid_argument ||
            descriptor.error().code == ErrorCode::out_of_range) {
            return false;
        }
        return std::unexpected(descriptor.error());
    }
    if (descriptor->identity_key != record.identity_key) {
        constexpr char kIdentityScopeSeparator = '\x1E';
        std::string scoped_prefix = descriptor->identity_key;
        scoped_prefix.push_back(kIdentityScopeSeparator);
        if (!record.identity_key.starts_with(scoped_prefix)) return false;
        const std::string_view identity_scope(record.identity_key.data() +
                                                  scoped_prefix.size(),
                                              record.identity_key.size() -
                                                  scoped_prefix.size());
        auto scoped_identity = SuiteInstaller::scope_identity(
            *descriptor, identity_scope);
        if (!scoped_identity) return false;
    }
    const bool matches = descriptor->identity_key == record.identity_key &&
        descriptor->version == record.version &&
        digest_equal(descriptor->identity_sha256, record.identity_sha256) &&
        digest_equal(descriptor->archive_sha256, record.archive_sha256) &&
        descriptor->archive_size == record.archive_size;
    if (matches && validated_descriptor != nullptr) {
        *validated_descriptor = std::move(*descriptor);
    }
    return matches;
}

[[nodiscard]] std::optional<i32> parse_transaction_id(
    std::string_view name,
    std::string_view prefix) noexcept {
    if (!name.starts_with(prefix) || name.size() == prefix.size()) {
        return std::nullopt;
    }
    const std::string_view suffix = name.substr(prefix.size());
    i32 value = 0;
    const auto [end, error] = std::from_chars(
        suffix.data(), suffix.data() + suffix.size(), value);
    if (error != std::errc {} || end != suffix.data() + suffix.size() ||
        value <= 0) {
        return std::nullopt;
    }
    return value;
}

} // namespace

Status SuiteStore::configure(const SuiteStoreConfig& config) {
    if (config.root_path.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "suite store root path must not be empty");
    }
    if (configured_ || !suites_.empty()) {
        return fail(ErrorCode::invalid_state,
                    "suite store must be clear before it is configured");
    }

    std::error_code path_error;
    const std::filesystem::path normalized = std::filesystem::absolute(
        std::filesystem::path(config.root_path), path_error).lexically_normal();
    if (path_error) {
        return filesystem_error("unable to resolve suite store root path",
                                path_error);
    }
    if (normalized.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "suite store root path is invalid");
    }

    auto root_status = ensure_directory(normalized);
    if (!root_status) return root_status;
    auto suites_status = ensure_directory(normalized / "suites");
    if (!suites_status) return suites_status;

    root_path_ = normalized.string();
    installer_limits_ = config.installer_limits;
    fault_injector_ = config.fault_injector;
    database_.set_path((normalized / "suites.db").string());
    database_.set_fault_injector(config.database_fault_injector);

    auto snapshot = database_.load(config.recover_database_from_backup);
    if (!snapshot) {
        root_path_.clear();
        database_.set_path({});
        return std::unexpected(snapshot.error());
    }

    std::unordered_map<i32, SuiteDescriptor> validated_descriptors;
    validated_descriptors.reserve(snapshot->records.size());
    auto recovered = recover_transactions(*snapshot, &validated_descriptors);
    if (!recovered) {
        root_path_.clear();
        database_.set_path({});
        return recovered;
    }

    std::unordered_map<i32, Suite> loaded;
    loaded.reserve(snapshot->records.size());
    std::vector<std::string> identities;
    identities.reserve(snapshot->records.size());
    for (const SuiteDatabaseRecord& record : snapshot->records) {
        if (loaded.contains(record.id.value) ||
            std::find(identities.begin(), identities.end(), record.identity_key) !=
                identities.end()) {
            root_path_.clear();
            database_.set_path({});
            return fail(ErrorCode::malformed_archive,
                        "suite database contains duplicate suite identity or ID");
        }
        const auto validated = validated_descriptors.find(record.id.value);
        auto suite = suite_from_record(
            record,
            validated == validated_descriptors.end()
                ? nullptr
                : &validated->second);
        if (!suite) {
            root_path_.clear();
            database_.set_path({});
            return std::unexpected(suite.error());
        }
        identities.push_back(record.identity_key);
        loaded.emplace(record.id.value, std::move(*suite));
    }

    database_generation_ = snapshot->generation;
    suites_ = std::move(loaded);
    configured_ = true;
    return {};
}

Result<SuiteId> SuiteStore::install(const std::string& jar_path) {
    return install_impl(jar_path, std::nullopt, false, {}, false);
}

Result<SuiteId> SuiteStore::install_replacing(const std::string& jar_path) {
    return install_impl(jar_path, std::nullopt, false, {}, true);
}

Result<SuiteId> SuiteStore::install_scoped(
    const std::string& jar_path,
    std::string_view identity_scope) {
    return install_impl(jar_path, std::nullopt, false, identity_scope, true);
}

Result<SuiteId> SuiteStore::install(const std::string& jad_path,
                                    const std::string& jar_path,
                                    bool allow_downgrade) {
    return install_impl(jar_path, jad_path, allow_downgrade, {}, false);
}

Result<SuiteId> SuiteStore::install_impl(
    const std::string& jar_path,
    const std::optional<std::string>& jad_path,
    bool allow_downgrade,
    std::string_view identity_scope,
    bool allow_same_version_replacement) {
    auto descriptor = SuiteInstaller::inspect(
        jar_path, jad_path, installer_limits_);
    if (!descriptor) {
        return std::unexpected(descriptor.error());
    }
    auto scoped_identity = SuiteInstaller::scope_identity(
        *descriptor, identity_scope);
    if (!scoped_identity) {
        return std::unexpected(scoped_identity.error());
    }

    const Suite* existing = nullptr;
    for (const auto& [id, suite] : suites_) {
        static_cast<void>(id);
        if (suite.identity_key == descriptor->identity_key) {
            existing = &suite;
            break;
        }
    }

    SuiteId id;
    if (existing != nullptr) {
        id = existing->id;
        auto comparison = SuiteInstaller::compare_versions(
            descriptor->version, existing->version);
        if (!comparison) return std::unexpected(comparison.error());
        if (*comparison < 0 && !allow_downgrade) {
            return fail(ErrorCode::invalid_state,
                        "suite downgrade is not allowed");
        }
        if (*comparison == 0) {
            if (digest_equal(descriptor->archive_sha256,
                             existing->archive_sha256)) {
                return id;
            }
            if (!allow_same_version_replacement) {
                return fail(
                    ErrorCode::invalid_state,
                    "same-version suite replacement has different JAR content");
            }
            // An explicit host replacement represents one application slot.
            // Reimporting that slot with changed bytes is a legitimate update
            // even when the producer forgot to bump MIDlet-Version. Keep the
            // stable suite ID so RMS/files/permissions remain attached.
        }
    } else {
        id = allocate_id(descriptor->identity_sha256,
                         descriptor->identity_key);
    }

    if (!configured_) {
        auto suite = make_suite(
            id, *descriptor, jar_path, jad_path.value_or(std::string {}), false);
        if (!suite) return std::unexpected(suite.error());
        suites_.insert_or_assign(id.value, std::move(*suite));
        return id;
    }

    const std::filesystem::path final_path = suite_directory(root_path_, id);
    const std::filesystem::path stage_path = stage_directory(root_path_, id);
    const std::filesystem::path backup_path = backup_directory(root_path_, id);

    auto removed_stage = remove_tree(stage_path);
    if (!removed_stage) return std::unexpected(removed_stage.error());
    auto stage_status = ensure_directory(stage_path);
    if (!stage_status) return std::unexpected(stage_status.error());

    auto copied_jar = copy_file_synced(
        jar_path,
        stage_path / "app.jar",
        fault_injector_,
        SuiteStoreFaultPoint::before_staged_jar_sync);
    if (!copied_jar) {
        static_cast<void>(remove_tree(stage_path));
        return std::unexpected(copied_jar.error());
    }
    if (jad_path.has_value()) {
        auto copied_jad = copy_file_synced(
            *jad_path,
            stage_path / "app.jad",
            fault_injector_,
            SuiteStoreFaultPoint::before_staged_jad_sync);
        if (!copied_jad) {
            static_cast<void>(remove_tree(stage_path));
            return std::unexpected(copied_jad.error());
        }
    }

    auto stage_sync_fault = inject_fault(
        SuiteStoreFaultPoint::before_stage_directory_sync);
    if (!stage_sync_fault) {
        static_cast<void>(remove_tree(stage_path));
        return std::unexpected(stage_sync_fault.error());
    }
    auto stage_synced = sync_directory(
        stage_path, "unable to sync staged suite directory");
    if (!stage_synced) {
        static_cast<void>(remove_tree(stage_path));
        return std::unexpected(stage_synced.error());
    }
    auto stage_parent_fault = inject_fault(
        SuiteStoreFaultPoint::before_stage_parent_sync);
    if (!stage_parent_fault) {
        static_cast<void>(remove_tree(stage_path));
        return std::unexpected(stage_parent_fault.error());
    }
    auto stage_parent_synced = sync_directory(
        suites_directory(root_path_),
        "unable to sync suite directory after staging");
    if (!stage_parent_synced) {
        static_cast<void>(remove_tree(stage_path));
        return std::unexpected(stage_parent_synced.error());
    }

    auto removed_backup = remove_tree(backup_path);
    if (!removed_backup) {
        static_cast<void>(remove_tree(stage_path));
        return std::unexpected(removed_backup.error());
    }

    auto final_exists = path_exists(final_path);
    if (!final_exists) {
        static_cast<void>(remove_tree(stage_path));
        return std::unexpected(final_exists.error());
    }
    if (*final_exists) {
        auto backup_rename_fault = inject_fault(
            SuiteStoreFaultPoint::before_backup_rename);
        if (!backup_rename_fault) {
            static_cast<void>(remove_tree(stage_path));
            return std::unexpected(backup_rename_fault.error());
        }
        auto backed_up = rename_path(final_path, backup_path);
        if (!backed_up) {
            static_cast<void>(remove_tree(stage_path));
            return std::unexpected(backed_up.error());
        }
        auto backup_parent_fault = inject_fault(
            SuiteStoreFaultPoint::before_backup_parent_sync);
        Status backup_parent_synced = backup_parent_fault;
        if (backup_parent_synced) {
            backup_parent_synced = sync_directory(
                suites_directory(root_path_),
                "unable to sync suite directory after backup rename");
        }
        if (!backup_parent_synced) {
            auto restored = rename_path(backup_path, final_path);
            if (restored) {
                restored = sync_directory(
                    suites_directory(root_path_),
                    "unable to sync suite directory after backup rollback");
            }
            static_cast<void>(remove_tree(stage_path));
            if (!restored) return std::unexpected(restored.error());
            return std::unexpected(backup_parent_synced.error());
        }
    }

    auto activation_rename_fault = inject_fault(
        SuiteStoreFaultPoint::before_activation_rename);
    if (!activation_rename_fault) {
        Status restored;
        if (*final_exists) {
            restored = rename_path(backup_path, final_path);
            if (restored) {
                restored = sync_directory(
                    suites_directory(root_path_),
                    "unable to sync suite directory after activation rollback");
            }
        }
        static_cast<void>(remove_tree(stage_path));
        if (!restored) return std::unexpected(restored.error());
        return std::unexpected(activation_rename_fault.error());
    }
    auto activated = rename_path(stage_path, final_path);
    if (!activated) {
        Status restored;
        if (*final_exists) {
            restored = rename_path(backup_path, final_path);
            if (restored) {
                restored = sync_directory(
                    suites_directory(root_path_),
                    "unable to sync suite directory after activation rollback");
            }
        }
        static_cast<void>(remove_tree(stage_path));
        if (!restored) return std::unexpected(restored.error());
        return std::unexpected(activated.error());
    }
    auto activation_parent_fault = inject_fault(
        SuiteStoreFaultPoint::before_activation_parent_sync);
    Status activation_parent_synced = activation_parent_fault;
    if (activation_parent_synced) {
        activation_parent_synced = sync_directory(
            suites_directory(root_path_),
            "unable to sync suite directory after suite activation");
    }
    if (!activation_parent_synced) {
        auto removed_final = remove_tree(final_path);
        Status restored = removed_final;
        if (restored && *final_exists) {
            restored = rename_path(backup_path, final_path);
        }
        if (restored) {
            restored = sync_directory(
                suites_directory(root_path_),
                "unable to sync suite directory after activation rollback");
        }
        if (!restored) return std::unexpected(restored.error());
        return std::unexpected(activation_parent_synced.error());
    }

    auto managed_suite = make_suite(
        id,
        *descriptor,
        (final_path / "app.jar").string(),
        jad_path.has_value() ? (final_path / "app.jad").string() : std::string {},
        true);
    if (!managed_suite) {
        auto removed_final = remove_tree(final_path);
        Status restored = removed_final;
        if (restored && *final_exists) {
            restored = rename_path(backup_path, final_path);
        }
        if (restored) {
            restored = sync_directory(
                suites_directory(root_path_),
                "unable to sync suite directory after suite construction rollback");
        }
        if (!restored) return std::unexpected(restored.error());
        return std::unexpected(managed_suite.error());
    }

    std::optional<Suite> previous;
    if (const auto previous_iterator = suites_.find(id.value);
        previous_iterator != suites_.end()) {
        previous = previous_iterator->second;
    }
    suites_.insert_or_assign(id.value, std::move(*managed_suite));

    SuiteDatabaseSnapshot snapshot =
        snapshot_with_generation(database_generation_ + 1U);
    auto committed = database_.commit(snapshot);
    if (!committed) {
        if (previous.has_value()) {
            suites_.insert_or_assign(id.value, std::move(*previous));
        } else {
            suites_.erase(id.value);
        }
        auto removed_final = remove_tree(final_path);
        Status restored = removed_final;
        if (restored && *final_exists) {
            restored = rename_path(backup_path, final_path);
        }
        if (restored) {
            restored = sync_directory(
                suites_directory(root_path_),
                "unable to sync suite directory after database rollback");
        }
        if (!restored) return std::unexpected(restored.error());
        return std::unexpected(committed.error());
    }

    database_generation_ = snapshot.generation;
    if (*committed == SuiteDatabaseCommitDurability::durable) {
        auto removed = remove_tree(backup_path);
        if (removed) {
            static_cast<void>(sync_directory(
                suites_directory(root_path_),
                "unable to sync suite directory after transaction cleanup"));
        }
    }
    return id;
}

Status SuiteStore::uninstall(SuiteId id,
                             const SuiteUninstallPolicy& policy) {
    if (!id.valid()) {
        return fail(ErrorCode::invalid_argument, "suite ID is invalid");
    }
    const auto iterator = suites_.find(id.value);
    if (iterator == suites_.end()) {
        return fail(ErrorCode::invalid_argument, "suite ID does not exist");
    }

    if (!configured_ || !iterator->second.managed) {
        suites_.erase(iterator);
        return remove_policy_data(id, policy);
    }

    const Suite previous = iterator->second;
    const std::filesystem::path final_path = suite_directory(root_path_, id);
    const std::filesystem::path tombstone_path = delete_directory(root_path_, id);
    auto removed_tombstone = remove_tree(tombstone_path);
    if (!removed_tombstone) return removed_tombstone;

    auto final_exists = path_exists(final_path);
    if (!final_exists) return std::unexpected(final_exists.error());
    if (!*final_exists) {
        return fail(ErrorCode::io_error,
                    "managed suite directory is missing during uninstall");
    }
    auto uninstall_rename_fault = inject_fault(
        SuiteStoreFaultPoint::before_uninstall_rename);
    if (!uninstall_rename_fault) return uninstall_rename_fault;
    auto moved = rename_path(final_path, tombstone_path);
    if (!moved) return moved;
    auto uninstall_parent_fault = inject_fault(
        SuiteStoreFaultPoint::before_uninstall_parent_sync);
    Status uninstall_parent_synced = uninstall_parent_fault;
    if (uninstall_parent_synced) {
        uninstall_parent_synced = sync_directory(
            suites_directory(root_path_),
            "unable to sync suite directory after uninstall rename");
    }
    if (!uninstall_parent_synced) {
        auto restored = rename_path(tombstone_path, final_path);
        if (restored) {
            restored = sync_directory(
                suites_directory(root_path_),
                "unable to sync suite directory after uninstall rollback");
        }
        if (!restored) return restored;
        return uninstall_parent_synced;
    }

    suites_.erase(iterator);
    SuiteDatabaseSnapshot snapshot =
        snapshot_with_generation(database_generation_ + 1U);
    auto committed = database_.commit(snapshot);
    if (!committed) {
        suites_.insert_or_assign(id.value, previous);
        auto restored = rename_path(tombstone_path, final_path);
        if (restored) {
            restored = sync_directory(
                suites_directory(root_path_),
                "unable to sync suite directory after uninstall database rollback");
        }
        if (!restored) return restored;
        return std::unexpected(committed.error());
    }

    database_generation_ = snapshot.generation;
    if (*committed == SuiteDatabaseCommitDurability::durable) {
        auto removed = remove_tree(tombstone_path);
        if (removed) {
            static_cast<void>(sync_directory(
                suites_directory(root_path_),
                "unable to sync suite directory after uninstall cleanup"));
        }
        return remove_policy_data(id, policy);
    }

    // The previous database generation may win after a crash. Keep RMS,
    // files and permission state until a later durable uninstall confirms
    // that the suite cannot be recovered.
    return {};
}

const Suite* SuiteStore::find(SuiteId id) const noexcept {
    const auto iterator = suites_.find(id.value);
    return iterator == suites_.end() ? nullptr : &iterator->second;
}

std::vector<SuiteId> SuiteStore::list() const {
    std::vector<SuiteId> ids;
    ids.reserve(suites_.size());
    for (const auto& [id, suite] : suites_) {
        static_cast<void>(suite);
        ids.push_back(SuiteId {id});
    }
    std::sort(ids.begin(), ids.end(), [](SuiteId left, SuiteId right) {
        return left.value < right.value;
    });
    return ids;
}

Result<classfile::ClassFile> SuiteStore::load_class(
    SuiteId id,
    std::string_view binary_name) const {
    const Suite* suite = find(id);
    if (suite == nullptr) {
        return fail(ErrorCode::invalid_argument, "suite ID does not exist");
    }

    auto archive = configured_
        ? archive::ZipArchive::open(suite->jar_path,
                                    installer_limits_.archive_limits)
        : archive::ZipArchive::open(suite->jar_path);
    if (!archive) {
        return std::unexpected(archive.error());
    }

    const std::string entry_name = class_entry_name(binary_name);
    auto class_bytes = archive->read(entry_name);
    if (!class_bytes) {
        return std::unexpected(class_bytes.error());
    }

    auto parsed = classfile::ClassFile::parse(*class_bytes);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    std::string expected_name(binary_name);
    std::replace(expected_name.begin(), expected_name.end(), '.', '/');
    if (parsed->name() != expected_name) {
        return fail(ErrorCode::malformed_class,
                    "class entry name does not match class-file declaration");
    }
    return parsed;
}

bool SuiteStore::contains_main_class(SuiteId id,
                                     std::string_view binary_name) const {
    return load_class(id, binary_name).has_value();
}

void SuiteStore::clear() noexcept {
    suites_.clear();
    root_path_.clear();
    installer_limits_ = {};
    fault_injector_ = {};
    database_.set_path({});
    database_.set_fault_injector({});
    database_generation_ = 0;
    configured_ = false;
}

Result<Suite> SuiteStore::make_suite(
    SuiteId id,
    const SuiteDescriptor& descriptor,
    std::string jar_path,
    std::string jad_path,
    bool managed) const {
    std::unordered_map<std::u16string, std::u16string> decoded_properties;
    decoded_properties.reserve(descriptor.properties.size());
    for (const auto& [key, value] : descriptor.properties) {
        auto decoded_key = JadParser::decode_utf8(key);
        auto decoded_value = JadParser::decode_utf8(value);
        if (!decoded_key) return std::unexpected(decoded_key.error());
        if (!decoded_value) return std::unexpected(decoded_value.error());
        decoded_properties.insert_or_assign(std::move(*decoded_key),
                                            std::move(*decoded_value));
    }

    return Suite {
        .id = id,
        .jar_path = std::move(jar_path),
        .jad_path = std::move(jad_path),
        .display_name = descriptor.name,
        .vendor = descriptor.vendor,
        .version = descriptor.version,
        .identity_key = descriptor.identity_key,
        .identity_sha256 = descriptor.identity_sha256,
        .archive_sha256 = descriptor.archive_sha256,
        .declared_midlets = descriptor.midlet_classes,
        .declared_required_permissions =
            descriptor.declared_required_permissions,
        .declared_optional_permissions =
            descriptor.declared_optional_permissions,
        .declared_permissions = descriptor.declared_permissions,
        .has_permission_declarations = descriptor.has_permission_declarations,
        .trust_evidence = descriptor.trust_evidence,
        .raw_properties = descriptor.properties,
        .properties = std::move(decoded_properties),
        .archive_crc32 = descriptor.archive_crc32,
        .archive_size = descriptor.archive_size,
        .declared_jar_size = descriptor.declared_jar_size,
        .verified_class_cache_version = 0,
        .verified_classes = {},
        .managed = managed,
    };
}

Result<Suite> SuiteStore::suite_from_record(
    const SuiteDatabaseRecord& record,
    const SuiteDescriptor* validated_descriptor) const {
    const std::string expected_jar = relative_jar_path(record.id);
    const std::string expected_jad = relative_jad_path(record.id);
    if (record.jar_relative_path != expected_jar ||
        (!record.jad_relative_path.empty() &&
         record.jad_relative_path != expected_jad)) {
        return fail(ErrorCode::malformed_archive,
                    "suite database contains an unsafe managed path");
    }

    const std::filesystem::path jar_path =
        std::filesystem::path(root_path_) / record.jar_relative_path;
    const std::optional<std::string> jad_path = record.jad_relative_path.empty()
        ? std::nullopt
        : std::optional<std::string>(
              (std::filesystem::path(root_path_) /
               record.jad_relative_path).string());

    // recover_transactions() already inspected and SHA-256-validated the
    // selected managed JAR. Reuse that exact descriptor (including permission
    // and signature evidence) instead of parsing/hashing the same archive a
    // second time during every cold runtime configuration.
    if (validated_descriptor != nullptr) {
        auto suite = make_suite(
            record.id,
            *validated_descriptor,
            jar_path.string(),
            jad_path.value_or(std::string {}),
            true);
        if (!suite) return suite;
        suite->verified_class_cache_version = record.verified_class_cache_version;
        suite->verified_classes = record.verified_classes;
        return suite;
    }

    auto descriptor = SuiteInstaller::inspect(
        jar_path.string(), jad_path, installer_limits_);
    if (!descriptor) return std::unexpected(descriptor.error());

    if (descriptor->identity_key != record.identity_key) {
        constexpr char kIdentityScopeSeparator = '\x1E';
        std::string scoped_prefix = descriptor->identity_key;
        scoped_prefix.push_back(kIdentityScopeSeparator);
        if (!record.identity_key.starts_with(scoped_prefix)) {
            return fail(
                ErrorCode::checksum_mismatch,
                "managed suite identity does not match the persistent database");
        }
        const std::string_view identity_scope(record.identity_key.data() +
                                                  scoped_prefix.size(),
                                              record.identity_key.size() -
                                                  scoped_prefix.size());
        auto scoped_identity = SuiteInstaller::scope_identity(
            *descriptor, identity_scope);
        if (!scoped_identity) {
            return std::unexpected(scoped_identity.error());
        }
    }

    if (descriptor->identity_key != record.identity_key ||
        descriptor->version != record.version ||
        !digest_equal(descriptor->identity_sha256, record.identity_sha256) ||
        !digest_equal(descriptor->archive_sha256, record.archive_sha256) ||
        descriptor->archive_size != record.archive_size) {
        return fail(ErrorCode::checksum_mismatch,
                    "managed suite files do not match the persistent database");
    }

    auto suite = make_suite(
        record.id,
        *descriptor,
        jar_path.string(),
        jad_path.value_or(std::string {}),
        true);
    if (!suite) return suite;
    suite->verified_class_cache_version = record.verified_class_cache_version;
    suite->verified_classes = record.verified_classes;
    return suite;
}

SuiteDatabaseRecord SuiteStore::record_from_suite(
    const Suite& suite) const {
    SuiteDatabaseRecord record {
        .id = suite.id,
        .identity_key = suite.identity_key,
        .identity_sha256 = suite.identity_sha256,
        .archive_sha256 = suite.archive_sha256,
        .name = suite.display_name,
        .vendor = suite.vendor,
        .version = suite.version,
        .jar_relative_path = relative_jar_path(suite.id),
        .jad_relative_path = suite.jad_path.empty()
            ? std::string {}
            : relative_jad_path(suite.id),
        .midlet_classes = suite.declared_midlets,
        .declared_permissions = suite.declared_permissions,
        .has_permission_declarations = suite.has_permission_declarations,
        .properties = suite.raw_properties,
        .archive_crc32 = suite.archive_crc32,
        .archive_size = suite.archive_size,
        .declared_jar_size = suite.declared_jar_size,
        .has_signature_metadata = suite.trust_evidence.has_signature_metadata(),
        .verified_class_cache_version = suite.verified_class_cache_version,
        .verified_classes = suite.verified_classes,
    };
    if (auto stamp = file_validation_stamp(suite.jar_path); stamp) {
        record.jar_validation_stamp = *stamp;
    }
    if (!suite.jad_path.empty()) {
        if (auto stamp = file_validation_stamp(suite.jad_path); stamp) {
            record.jad_validation_stamp = *stamp;
        }
    }
    return record;
}

Status SuiteStore::update_verified_classes(
    SuiteId id,
    u32 cache_version,
    const std::unordered_map<std::string, u64>& verified_classes) {
    if (!id.valid()) {
        return fail(ErrorCode::invalid_argument, "suite ID is invalid");
    }
    auto iterator = suites_.find(id.value);
    if (iterator == suites_.end()) {
        return fail(ErrorCode::invalid_argument, "suite ID does not exist");
    }
    Suite& suite = iterator->second;
    if (!suite.managed) return {};
    if (suite.verified_class_cache_version == cache_version &&
        suite.verified_classes == verified_classes) {
        return {};
    }
    if (database_generation_ == std::numeric_limits<u64>::max()) {
        return fail(ErrorCode::out_of_range,
                    "suite database generation exhausted while caching verified classes");
    }

    const u32 previous_version = suite.verified_class_cache_version;
    auto previous_classes = suite.verified_classes;
    suite.verified_class_cache_version = cache_version;
    suite.verified_classes = verified_classes;

    SuiteDatabaseSnapshot snapshot =
        snapshot_with_generation(database_generation_ + 1U);
    auto committed = database_.commit(snapshot);
    if (!committed) {
        suite.verified_class_cache_version = previous_version;
        suite.verified_classes = std::move(previous_classes);
        return std::unexpected(committed.error());
    }
    database_generation_ = snapshot.generation;
    return {};
}

SuiteDatabaseSnapshot SuiteStore::snapshot_with_generation(
    u64 generation) const {
    SuiteDatabaseSnapshot snapshot;
    snapshot.generation = generation;
    snapshot.records.reserve(suites_.size());
    for (const auto& [id, suite] : suites_) {
        static_cast<void>(id);
        if (suite.managed) {
            snapshot.records.push_back(record_from_suite(suite));
        }
    }
    std::sort(snapshot.records.begin(), snapshot.records.end(),
              [](const SuiteDatabaseRecord& left,
                 const SuiteDatabaseRecord& right) {
                  return left.id.value < right.id.value;
              });
    return snapshot;
}

SuiteId SuiteStore::allocate_id(
    const std::array<u8, 32>& identity_sha256,
    std::string_view identity_key) const noexcept {
    SuiteId candidate = SuiteInstaller::stable_suite_id(identity_sha256);
    for (;;) {
        const auto iterator = suites_.find(candidate.value);
        if (iterator == suites_.end() ||
            iterator->second.identity_key == identity_key) {
            return candidate;
        }
        if (candidate.value == std::numeric_limits<i32>::max()) {
            candidate.value = 1;
        } else {
            ++candidate.value;
        }
    }
}

Status SuiteStore::recover_transactions(
    SuiteDatabaseSnapshot& snapshot,
    std::unordered_map<i32, SuiteDescriptor>* validated_descriptors) {
    bool directory_changed = false;
    bool database_changed = false;
    std::vector<i32> referenced_ids;
    referenced_ids.reserve(snapshot.records.size());
    std::vector<SuiteDatabaseRecord> recovered_records;
    recovered_records.reserve(snapshot.records.size());
    for (SuiteDatabaseRecord& record : snapshot.records) {
        const std::filesystem::path final_path =
            suite_directory(root_path_, record.id);
        const std::filesystem::path stage_path =
            stage_directory(root_path_, record.id);
        const std::filesystem::path backup_path =
            backup_directory(root_path_, record.id);
        const std::filesystem::path tombstone_path =
            delete_directory(root_path_, record.id);

        auto final_exists = path_exists(final_path);
        auto backup_exists = path_exists(backup_path);
        auto tombstone_exists = path_exists(tombstone_path);
        if (!final_exists) return std::unexpected(final_exists.error());
        if (!backup_exists) return std::unexpected(backup_exists.error());
        if (!tombstone_exists) return std::unexpected(tombstone_exists.error());

        SuiteDescriptor final_descriptor;
        SuiteDescriptor backup_descriptor;
        SuiteDescriptor tombstone_descriptor;
        Result<bool> final_matches = false;
        Result<bool> backup_matches = false;
        Result<bool> tombstone_matches = false;
        if (*final_exists) {
            final_matches = directory_matches_record(
                final_path, record, installer_limits_, &final_descriptor);
            if (!final_matches) return std::unexpected(final_matches.error());
        }
        if (*backup_exists) {
            backup_matches = directory_matches_record(
                backup_path, record, installer_limits_, &backup_descriptor);
            if (!backup_matches) return std::unexpected(backup_matches.error());
        }
        if (*tombstone_exists) {
            tombstone_matches = directory_matches_record(
                tombstone_path, record, installer_limits_, &tombstone_descriptor);
            if (!tombstone_matches) {
                return std::unexpected(tombstone_matches.error());
            }
        }

        const std::filesystem::path* selected = nullptr;
        SuiteDescriptor* selected_descriptor = nullptr;
        if (*final_exists && *final_matches) {
            selected = &final_path;
            selected_descriptor = &final_descriptor;
        } else if (*backup_exists && *backup_matches) {
            selected = &backup_path;
            selected_descriptor = &backup_descriptor;
        } else if (*tombstone_exists && *tombstone_matches) {
            selected = &tombstone_path;
            selected_descriptor = &tombstone_descriptor;
        }
        if (selected == nullptr) {
            // A managed JAR may be missing or have been modified outside the
            // transactional installer. Do not let one damaged suite brick the
            // entire runtime. Forget only its managed registration; the
            // unreferenced suite directory is removed below while RMS/files
            // remain untouched so reinstalling the same scoped suite recovers
            // its stable suite ID and application data.
            database_changed = true;
            continue;
        }

        referenced_ids.push_back(record.id.value);

        if (*selected != final_path) {
            if (*final_exists) {
                auto removed = remove_tree(final_path);
                if (!removed) return removed;
                directory_changed = true;
            }
            auto restored = rename_path(*selected, final_path);
            if (!restored) return restored;
            directory_changed = true;
        }
        if (*backup_exists && *selected != backup_path) {
            auto removed = remove_tree(backup_path);
            if (!removed) return removed;
            directory_changed = true;
        }
        if (*tombstone_exists && *selected != tombstone_path) {
            auto removed = remove_tree(tombstone_path);
            if (!removed) return removed;
            directory_changed = true;
        }
        auto stage_exists = path_exists(stage_path);
        if (!stage_exists) return std::unexpected(stage_exists.error());
        auto removed_stage = remove_tree(stage_path);
        if (!removed_stage) return removed_stage;
        directory_changed = directory_changed || *stage_exists;

        if (selected_descriptor != nullptr) {
            auto refreshed = refresh_validation_stamps(
                record, final_path, *selected_descriptor);
            if (!refreshed) return std::unexpected(refreshed.error());
            database_changed = database_changed || *refreshed;
            if (validated_descriptors != nullptr) {
                validated_descriptors->insert_or_assign(
                    record.id.value, *selected_descriptor);
            }
        }
        recovered_records.push_back(record);
    }

    if (database_changed) {
        if (snapshot.generation == std::numeric_limits<u64>::max()) {
            return fail(ErrorCode::out_of_range,
                        "suite database generation exhausted during recovery");
        }
        snapshot.records = std::move(recovered_records);
        ++snapshot.generation;
        snapshot.recovered_from_backup = false;
        auto committed = database_.commit(snapshot);
        if (!committed) return std::unexpected(committed.error());
    }

    std::error_code error;
    std::filesystem::directory_iterator iterator(
        suites_directory(root_path_), error);
    if (error) {
        return filesystem_error("unable to enumerate suite transaction directory", error);
    }
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const std::filesystem::path path = iterator->path();
        const std::string name = path.filename().string();
        bool remove = name.starts_with(".stage-");
        if (const auto direct_id = parse_transaction_id(name, "");
            direct_id.has_value() &&
            std::find(referenced_ids.begin(), referenced_ids.end(), *direct_id) ==
                referenced_ids.end()) {
            remove = true;
        }
        for (const std::string_view prefix : {
                 std::string_view(".backup-"),
                 std::string_view(".delete-")}) {
            if (const auto id = parse_transaction_id(name, prefix);
                id.has_value() &&
                std::find(referenced_ids.begin(), referenced_ids.end(), *id) ==
                    referenced_ids.end()) {
                remove = true;
            }
        }
        if (remove) {
            auto removed = remove_tree(path);
            if (!removed) return removed;
            directory_changed = true;
        }
        iterator.increment(error);
        if (error) {
            return filesystem_error(
                "unable to continue suite transaction enumeration", error);
        }
    }
    if (directory_changed) {
        return sync_directory(
            suites_directory(root_path_),
            "unable to sync suite directory after transaction recovery");
    }
    return {};
}

Status SuiteStore::inject_fault(SuiteStoreFaultPoint point) const {
    if (!fault_injector_) return {};
    return fault_injector_(point);
}

Status SuiteStore::remove_policy_data(
    SuiteId id,
    const SuiteUninstallPolicy& policy) const {
    if (!configured_) {
        return {};
    }
    if (policy.remove_rms) {
        auto removed = remove_tree(
            std::filesystem::path(root_path_) / "rms" /
            std::to_string(id.value));
        if (!removed) return removed;
    }
    if (policy.remove_files) {
        auto removed = remove_tree(
            std::filesystem::path(root_path_) / "files" /
            std::to_string(id.value));
        if (!removed) return removed;
    }
    if (policy.remove_permissions) {
        std::error_code error;
        std::filesystem::remove(
            std::filesystem::path(root_path_) / "security" /
                (std::to_string(id.value) + ".permissions"),
            error);
        if (error) {
            return filesystem_error("unable to remove suite permission state", error);
        }
    }
    if (policy.remove_push) {
        std::error_code error;
        const std::filesystem::path push_path =
            std::filesystem::path(root_path_) / "push" /
            (std::to_string(id.value) + ".push");
        std::filesystem::remove(push_path, error);
        if (error) {
            return filesystem_error("unable to remove suite push state", error);
        }
        std::filesystem::remove(push_path.string() + ".tmp", error);
        if (error) {
            return filesystem_error(
                "unable to remove suite push recovery state",
                error);
        }
    }
    if (policy.remove_temporary) {
        auto removed = remove_tree(
            std::filesystem::path(root_path_) / "tmp" /
            std::to_string(id.value));
        if (!removed) return removed;
    }
    return {};
}

} // namespace phoneme::runtime
