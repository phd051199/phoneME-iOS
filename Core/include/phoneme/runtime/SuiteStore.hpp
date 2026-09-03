#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "phoneme/archive/ZipArchive.hpp"
#include "phoneme/classfile/ClassFile.hpp"
#include "phoneme/runtime/SuiteDatabase.hpp"
#include "phoneme/runtime/SuiteInstaller.hpp"

namespace phoneme::runtime {

struct Suite final {
    SuiteId id;
    std::string jar_path;
    std::string jad_path;
    std::string display_name;
    std::string vendor;
    std::string version;
    std::string identity_key;
    std::array<u8, 32> identity_sha256 {};
    std::array<u8, 32> archive_sha256 {};
    std::vector<std::string> declared_midlets;
    std::vector<std::string> declared_required_permissions;
    std::vector<std::string> declared_optional_permissions;
    std::vector<std::string> declared_permissions;
    bool has_permission_declarations {false};
    ArchiveTrustEvidence trust_evidence;
    std::unordered_map<std::string, std::string> raw_properties;
    std::unordered_map<std::u16string, std::u16string> properties;
    u32 archive_crc32 {0};
    u64 archive_size {0};
    u64 declared_jar_size {0};
    u32 verified_class_cache_version {0};
    std::unordered_map<std::string, u64> verified_classes;
    bool managed {false};
};

enum class SuiteStoreFaultPoint : u8 {
    before_staged_jar_sync,
    before_staged_jad_sync,
    before_stage_directory_sync,
    before_stage_parent_sync,
    before_backup_rename,
    before_backup_parent_sync,
    before_activation_rename,
    before_activation_parent_sync,
    before_uninstall_rename,
    before_uninstall_parent_sync,
};

// Optional deterministic test hook. Production configurations leave it empty.
using SuiteStoreFaultInjector = std::function<Status(SuiteStoreFaultPoint)>;

struct SuiteStoreConfig final {
    std::string root_path;
    SuiteInstallerLimits installer_limits {};
    bool recover_database_from_backup {true};
    SuiteStoreFaultInjector fault_injector;
    SuiteDatabaseFaultInjector database_fault_injector;
};

struct SuiteUninstallPolicy final {
    bool remove_rms {false};
    bool remove_files {false};
    bool remove_permissions {false};
    bool remove_push {false};
    bool remove_temporary {false};
};

class SuiteStore final {
public:
    SuiteStore() = default;

    [[nodiscard]] Status configure(const SuiteStoreConfig& config);
    [[nodiscard]] bool configured() const noexcept { return configured_; }
    [[nodiscard]] const std::string& root_path() const noexcept {
        return root_path_;
    }

    [[nodiscard]] Result<SuiteId> install(const std::string& jar_path);
    [[nodiscard]] Result<SuiteId> install_replacing(
        const std::string& jar_path);
    [[nodiscard]] Result<SuiteId> install_scoped(
        const std::string& jar_path,
        std::string_view identity_scope);
    [[nodiscard]] Result<SuiteId> install(const std::string& jad_path,
                                          const std::string& jar_path,
                                          bool allow_downgrade = false);
    [[nodiscard]] Status uninstall(
        SuiteId id,
        const SuiteUninstallPolicy& policy = {});
    [[nodiscard]] Status update_verified_classes(
        SuiteId id,
        u32 cache_version,
        const std::unordered_map<std::string, u64>& verified_classes);

    [[nodiscard]] const Suite* find(SuiteId id) const noexcept;
    [[nodiscard]] std::vector<SuiteId> list() const;
    [[nodiscard]] Result<classfile::ClassFile> load_class(
        SuiteId id,
        std::string_view binary_name) const;
    [[nodiscard]] bool contains_main_class(SuiteId id,
                                           std::string_view binary_name) const;

    // Clears the in-memory view only. Managed files and the persistent database
    // remain intact and can be loaded by configure() again.
    void clear() noexcept;

private:
    [[nodiscard]] Result<SuiteId> install_impl(
        const std::string& jar_path,
        const std::optional<std::string>& jad_path,
        bool allow_downgrade,
        std::string_view identity_scope,
        bool allow_same_version_replacement);
    [[nodiscard]] Result<Suite> make_suite(
        SuiteId id,
        const SuiteDescriptor& descriptor,
        std::string jar_path,
        std::string jad_path,
        bool managed) const;
    [[nodiscard]] Result<Suite> suite_from_record(
        const SuiteDatabaseRecord& record,
        const SuiteDescriptor* validated_descriptor = nullptr) const;
    [[nodiscard]] SuiteDatabaseRecord record_from_suite(
        const Suite& suite) const;
    [[nodiscard]] SuiteDatabaseSnapshot snapshot_with_generation(
        u64 generation) const;
    [[nodiscard]] SuiteId allocate_id(
        const std::array<u8, 32>& identity_sha256,
        std::string_view identity_key) const noexcept;
    [[nodiscard]] Status recover_transactions(
        SuiteDatabaseSnapshot& snapshot,
        std::unordered_map<i32, SuiteDescriptor>* validated_descriptors = nullptr);
    [[nodiscard]] Status remove_policy_data(
        SuiteId id,
        const SuiteUninstallPolicy& policy) const;
    [[nodiscard]] Status inject_fault(SuiteStoreFaultPoint point) const;

    std::string root_path_;
    SuiteInstallerLimits installer_limits_ {};
    SuiteStoreFaultInjector fault_injector_;
    SuiteDatabase database_;
    u64 database_generation_ {0};
    bool configured_ {false};
    std::unordered_map<i32, Suite> suites_;
};

} // namespace phoneme::runtime
