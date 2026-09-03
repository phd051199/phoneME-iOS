#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "phoneme/runtime/JadParser.hpp"
#include "phoneme/runtime/SuiteInstaller.hpp"
#include "phoneme/runtime/SuiteStore.hpp"

namespace {

using phoneme::ErrorCode;
using phoneme::Result;
using phoneme::Status;
using phoneme::SuiteId;
using phoneme::u8;
using phoneme::u64;
using phoneme::usize;
using phoneme::runtime::AttributeParserLimits;
using phoneme::runtime::DuplicatePropertyPolicy;
using phoneme::runtime::JadParser;
using phoneme::runtime::SuiteDatabaseFaultPoint;
using phoneme::runtime::SuiteInstaller;
using phoneme::runtime::SuiteStore;
using phoneme::runtime::SuiteStoreConfig;
using phoneme::runtime::SuiteStoreFaultPoint;
using phoneme::runtime::SuiteUninstallPolicy;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename T>
void check_error(const Result<T>& result,
                 ErrorCode expected,
                 std::string_view message) {
    check(!result.has_value(), message);
    if (!result.has_value()) {
        check(result.error().code == expected,
              std::string(message) + " (unexpected error code)");
    }
}

bool write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(stream);
}

bool copy_fixture_file(const std::filesystem::path& source,
                       const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::copy_file(
        source, destination,
        std::filesystem::copy_options::overwrite_existing,
        error);
    return !error;
}

u64 fixture_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    check(!error, "read fixture JAR size");
    return error ? 0U : static_cast<u64>(size);
}

std::string make_jad(const std::filesystem::path& jar,
                     std::string_view version,
                     u64 declared_size,
                     bool continued = false,
                     std::string_view profile = "MIDP-2.0",
                     std::string_view configuration = "CLDC-1.1") {
    std::string result;
    result.append("MIDlet-Name: Installer Test\n");
    result.append("MIDlet-Vendor: phoneME\n");
    result.append("MIDlet-Version: ");
    result.append(version);
    result.push_back('\n');
    result.append("MIDlet-Jar-URL: ");
    result.append(jar.filename().string());
    result.push_back('\n');
    result.append("MIDlet-Jar-Size: ");
    result.append(std::to_string(declared_size));
    result.push_back('\n');
    result.append("MicroEdition-Profile: ");
    result.append(profile);
    result.push_back('\n');
    result.append("MicroEdition-Configuration: ");
    result.append(configuration);
    result.push_back('\n');
    result.append("MIDlet-1: Installer Test,,SuiteApp\n");
    result.append("MIDlet-Permissions: javax.microedition.io.Connector.http,");
    if (continued) {
        result.append("\n javax.microedition.io.Connector.socket\n");
    } else {
        result.append(" javax.microedition.io.Connector.socket\n");
    }
    result.append("MIDlet-Permissions-Opt: ");
    result.append("javax.microedition.media.control.VolumeControl\n");
    result.append("Custom-UTF8: Tiếng Việt\n");
    return result;
}

void test_version_comparison() {
    auto four_part_newer = SuiteInstaller::compare_versions("1.2.3.4", "1.2.3.3");
    check(four_part_newer.has_value() && *four_part_newer > 0,
          "accept and compare four-part MIDlet versions");

    auto many_part_newer = SuiteInstaller::compare_versions(
        "10.20.30.40.50", "10.20.30.40.49");
    check(many_part_newer.has_value() && *many_part_newer > 0,
          "accept MIDlet versions with more than four numeric components");

    auto trailing_zero_equal = SuiteInstaller::compare_versions(
        "2.5.0.0", "2.5");
    check(trailing_zero_equal.has_value() && *trailing_zero_equal == 0,
          "treat trailing zero version components as equivalent");

    auto large_component = SuiteInstaller::compare_versions(
        "1.0.20260805.1", "1.0.9999.9");
    check(large_component.has_value() && *large_component > 0,
          "accept full unsigned numeric version components");

    auto empty_component = SuiteInstaller::compare_versions("1..2.3", "1.2.3");
    check_error(empty_component, ErrorCode::invalid_argument,
                "reject empty MIDlet version components");

    auto non_numeric = SuiteInstaller::compare_versions("1.2.beta.4", "1.2.3.4");
    check_error(non_numeric, ErrorCode::invalid_argument,
                "reject non-numeric MIDlet version components");
}

void test_parser() {
    const std::string text =
        "Alpha: first\n"
        " continuation\n"
        "Unicode: Tiếng Việt\n";
    auto parsed = JadParser::parse(std::span<const u8>(
        reinterpret_cast<const u8*>(text.data()), text.size()));
    check(parsed.has_value(), "parse valid continued UTF-8 JAD");
    if (parsed) {
        check(parsed->find("Alpha") != nullptr &&
                  *parsed->find("Alpha") == "firstcontinuation",
              "join continuation without injected whitespace");
        check(parsed->find("Unicode") != nullptr,
              "retain UTF-8 property");
    }

    const std::string duplicate = "A: one\nA: two\n";
    auto rejected = JadParser::parse(std::span<const u8>(
        reinterpret_cast<const u8*>(duplicate.data()), duplicate.size()));
    check_error(rejected, ErrorCode::invalid_argument,
                "reject duplicate JAD property by default");

    AttributeParserLimits last_wins;
    last_wins.duplicate_policy = DuplicatePropertyPolicy::last_wins;
    auto accepted = JadParser::parse(std::span<const u8>(
        reinterpret_cast<const u8*>(duplicate.data()), duplicate.size()),
        last_wins);
    check(accepted.has_value() && accepted->find("A") != nullptr &&
              *accepted->find("A") == "two",
          "support explicit last-wins duplicate policy");

    const std::vector<u8> invalid_utf8 {0xC0U, 0xAFU};
    auto invalid = JadParser::parse(invalid_utf8);
    check_error(invalid, ErrorCode::invalid_argument,
                "reject overlong UTF-8 in JAD");
}

void test_scoped_same_version_install(
    const std::filesystem::path& root,
    const std::filesystem::path& baseline_fixture,
    const std::filesystem::path& changed_fixture) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    check(!error, "create scoped suite test root");

    const auto baseline = root / "baseline.jar";
    const auto changed = root / "changed.jar";
    check(copy_fixture_file(baseline_fixture, baseline) &&
              copy_fixture_file(changed_fixture, changed),
          "copy valid same-version JARs with different content");

    SuiteStore store;
    auto configured = store.configure(
        SuiteStoreConfig {.root_path = (root / "store").string()});
    check(configured.has_value(), "configure scoped suite store");
    if (!configured) return;

    auto unscoped = store.install(baseline.string());
    check(unscoped.has_value(), "install unscoped baseline suite");
    auto rejected = store.install(changed.string());
    check_error(rejected, ErrorCode::invalid_state,
                "retain strict unscoped same-version replacement policy");
    auto explicit_replacement = store.install_replacing(changed.string());
    check(explicit_replacement.has_value() &&
              *explicit_replacement == *unscoped,
          "explicit host replacement accepts changed same-version JAR");

    auto first = store.install_scoped(baseline.string(), "game-a");
    auto second = store.install_scoped(changed.string(), "game-b");
    check(first.has_value() && second.has_value() && *first != *second,
          "install duplicate manifest identity in separate host scopes");
    if (!first || !second) return;

    const auto rms_marker = root / "store" / "rms" /
        std::to_string(first->value) / "marker.bin";
    std::filesystem::create_directories(rms_marker.parent_path(), error);
    check(!error && write_text(rms_marker, "keep"),
          "create scoped replacement RMS marker");

    auto replaced = store.install_scoped(changed.string(), "game-a");
    check(replaced.has_value() && *replaced == *first,
          "replace changed same-version JAR within the same host scope");
    check(std::filesystem::exists(rms_marker, error) && !error,
          "scoped same-version replacement preserves RMS");

    const auto* first_suite = store.find(*first);
    const auto* second_suite = store.find(*second);
    check(first_suite != nullptr && second_suite != nullptr &&
              first_suite->archive_sha256 == second_suite->archive_sha256,
          "scoped replacement activates the changed archive bytes");

    store.clear();
    auto restarted = store.configure(
        SuiteStoreConfig {.root_path = (root / "store").string()});
    check(restarted.has_value(), "reload host-scoped suites after restart");
    check(store.find(*first) != nullptr && store.find(*second) != nullptr,
          "persist separate host-scoped suite identities");
    check(store.load_class(*first, "SuiteApp").has_value() &&
              store.load_class(*second, "SuiteApp").has_value(),
          "load classes from host-scoped suites after restart");
}

void test_install_flow(const std::filesystem::path& root,
                       const std::filesystem::path& jar_v1,
                       const std::filesystem::path& jar_v2) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    check(!error, "create suite test root");

    const std::filesystem::path imported_v1 = root / "import-v1.jar";
    const std::filesystem::path imported_v2 = root / "import-v2.jar";
    check(copy_fixture_file(jar_v1, imported_v1), "copy version 1 import fixture");
    check(copy_fixture_file(jar_v2, imported_v2), "copy version 2 import fixture");

    const std::filesystem::path jad_v1 = root / "app-v1.jad";
    const std::filesystem::path jad_v2 = root / "app-v2.jad";
    check(write_text(jad_v1, make_jad(imported_v1, "1.0.0",
                                      fixture_file_size(imported_v1), true)),
          "write version 1 JAD");
    check(write_text(jad_v2, make_jad(imported_v2, "1.1.0",
                                      fixture_file_size(imported_v2))),
          "write version 2 JAD");

    SuiteStore store;
    auto configured = store.configure(SuiteStoreConfig {.root_path = root.string()});
    check(configured.has_value(), "configure persistent suite store");
    if (!configured) {
        std::cerr << "configure error: " << configured.error().message << '\n';
        return;
    }

    auto installed = store.install(jad_v1.string(), imported_v1.string());
    check(installed.has_value(), "install JAD plus JAR");
    if (!installed) return;
    const SuiteId id = *installed;

    const auto* suite = store.find(id);
    check(suite != nullptr, "find installed suite");
    if (suite != nullptr) {
        check(suite->managed, "suite uses managed storage");
        check(suite->display_name == "Installer Test" &&
                  suite->vendor == "phoneME" && suite->version == "1.0.0",
              "persist suite identity and version");
        check(suite->declared_midlets.size() == 1U &&
                  suite->declared_midlets[0] == "SuiteApp",
              "parse MIDlet-n declaration");
        check(suite->declared_required_permissions.size() == 2U &&
                  suite->declared_optional_permissions.size() == 1U &&
                  suite->declared_permissions.size() == 3U,
              "separate required and optional permission declarations");
        check(suite->trust_evidence.has_signature_metadata() &&
                  !suite->trust_evidence.cryptographically_verified() &&
                  suite->trust_evidence.signature_files.size() == 2U,
              "expose unverified JAR signature evidence without granting trust");
        check(suite->raw_properties.contains("Custom-UTF8") &&
                  suite->properties.contains(u"Custom-UTF8"),
              "expose merged UTF-8 JAD properties to MIDlet runtime");
        check(!suite->jad_path.empty(), "copy JAD into managed suite directory");
    }

    std::filesystem::remove(imported_v1, error);
    std::filesystem::remove(jad_v1, error);
    auto loaded = store.load_class(id, "SuiteApp");
    check(loaded.has_value(),
          "load class after original JAR and JAD imports are deleted");

    store.clear();
    auto restarted = store.configure(SuiteStoreConfig {.root_path = root.string()});
    check(restarted.has_value(), "reload persistent suite database after restart");
    check(store.list().size() == 1U && store.list()[0] == id,
          "list same suite ID after restart");
    check(store.load_class(id, "SuiteApp").has_value(),
          "load suite class after restart");

    const std::unordered_map<std::string, phoneme::u64> verified_classes {
        {"SuiteApp", 0x123456789ABCDEF0ULL},
    };
    auto cached_verified = store.update_verified_classes(
        id, 1U, verified_classes);
    check(cached_verified.has_value(),
          "persist verified-class startup cache");
    store.clear();
    auto restarted_with_verified_cache =
        store.configure(SuiteStoreConfig {.root_path = root.string()});
    check(restarted_with_verified_cache.has_value(),
          "reload verified-class startup cache after restart");
    const auto* cached_suite = store.find(id);
    check(cached_suite != nullptr &&
              cached_suite->verified_class_cache_version == 1U &&
              cached_suite->verified_classes == verified_classes,
          "preserve verified-class startup cache in suite database");

    const std::filesystem::path rms_marker =
        root / "rms" / std::to_string(id.value) / "marker.bin";
    std::filesystem::create_directories(rms_marker.parent_path(), error);
    check(!error && write_text(rms_marker, "keep"),
          "create RMS preservation marker");

    auto upgraded = store.install(jad_v2.string(), imported_v2.string());
    check(upgraded.has_value() && *upgraded == id,
          "upgrade keeps stable suite ID");
    const auto* upgraded_suite = store.find(id);
    check(upgraded_suite != nullptr && upgraded_suite->version == "1.1.0",
          "upgrade replaces suite version");
    check(upgraded_suite != nullptr &&
              upgraded_suite->verified_class_cache_version == 0U &&
              upgraded_suite->verified_classes.empty(),
          "suite upgrade invalidates verified-class startup cache");
    check(std::filesystem::exists(rms_marker, error) && !error,
          "upgrade preserves RMS data");

    const std::filesystem::path downgrade_jad = root / "downgrade.jad";
    check(write_text(downgrade_jad,
                     make_jad(jar_v1, "1.0.0", fixture_file_size(jar_v1))),
          "write downgrade JAD");
    auto downgrade = store.install(downgrade_jad.string(), jar_v1.string());
    check_error(downgrade, ErrorCode::invalid_state,
                "reject suite downgrade by default");

    const std::filesystem::path final_directory =
        root / "suites" / std::to_string(id.value);
    const std::filesystem::path backup_directory =
        root / "suites" / (".backup-" + std::to_string(id.value));
    store.clear();
    std::filesystem::rename(final_directory, backup_directory, error);
    check(!error, "simulate crash after suite backup rename");
    auto recovered_transaction =
        store.configure(SuiteStoreConfig {.root_path = root.string()});
    check(recovered_transaction.has_value(),
          "recover interrupted install transaction");
    check(std::filesystem::is_directory(final_directory, error) && !error,
          "restore suite directory from transaction backup");

    store.clear();
    std::filesystem::remove_all(backup_directory, error);
    error.clear();
    std::filesystem::copy(
        final_directory, backup_directory,
        std::filesystem::copy_options::recursive,
        error);
    check(!error, "create valid backup beside an uncommitted final suite");
    check(copy_fixture_file(jar_v1, final_directory / "app.jar") &&
              copy_fixture_file(downgrade_jad, final_directory / "app.jad"),
          "simulate activated suite files before database commit");
    auto recovered_precommit =
        store.configure(SuiteStoreConfig {.root_path = root.string()});
    check(recovered_precommit.has_value(),
          "select transaction candidate matching persistent SHA-256 record");
    const auto* precommit_suite = store.find(id);
    check(precommit_suite != nullptr && precommit_suite->version == "1.1.0",
          "restore backup when final files do not match committed generation");
    check(!std::filesystem::exists(backup_directory, error) && !error,
          "remove obsolete transaction backup after recovery");

    const std::filesystem::path orphan_stage =
        root / "suites" / (".stage-" + std::to_string(id.value));
    std::filesystem::create_directories(orphan_stage, error);
    check(!error && write_text(orphan_stage / "partial", "partial"),
          "create orphan install stage");
    store.clear();
    auto cleaned_stage =
        store.configure(SuiteStoreConfig {.root_path = root.string()});
    check(cleaned_stage.has_value(), "reload store while orphan stage exists");
    check(!std::filesystem::exists(orphan_stage, error) && !error,
          "remove orphan install stage during recovery");

    store.clear();
    check(write_text(root / "suites.db", "corrupt"),
          "corrupt primary suite database");
    auto recovered_database =
        store.configure(SuiteStoreConfig {.root_path = root.string()});
    check(recovered_database.has_value(),
          "recover corrupt database from checksum-valid backup");
    const auto* recovered_suite = store.find(id);
    check(recovered_suite != nullptr && recovered_suite->version == "1.1.0",
          "database recovery restores current generation");

    auto uninstalled = store.uninstall(id);
    check(uninstalled.has_value(), "uninstall managed suite");
    check(store.find(id) == nullptr, "remove suite from database view");
    check(std::filesystem::exists(rms_marker, error) && !error,
          "default uninstall policy preserves RMS");

    auto reinstalled = store.install(jad_v2.string(), imported_v2.string());
    check(reinstalled.has_value() && *reinstalled == id,
          "reinstall derives the same stable suite ID");

    check(copy_fixture_file(jar_v1, final_directory / "app.jar"),
          "simulate external managed JAR modification");
    store.clear();
    auto recovered_tampered_suite =
        store.configure(SuiteStoreConfig {.root_path = root.string()});
    check(recovered_tampered_suite.has_value(),
          "recover runtime when a managed suite no longer matches its database record");
    check(store.find(id) == nullptr,
          "drop only the damaged managed suite registration during recovery");
    check(std::filesystem::exists(rms_marker, error) && !error,
          "damaged suite recovery preserves RMS data");
    check(!std::filesystem::exists(final_directory, error) && !error,
          "remove unreferenced damaged managed suite files after recovery");
    auto reinstalled_after_recovery =
        store.install(jad_v2.string(), imported_v2.string());
    check(reinstalled_after_recovery.has_value() &&
              *reinstalled_after_recovery == id,
          "reinstall after damaged suite recovery restores stable suite ID");

    const std::filesystem::path files_marker =
        root / "files" / std::to_string(id.value) / "marker.bin";
    const std::filesystem::path permission_marker =
        root / "security" / (std::to_string(id.value) + ".permissions");
    const std::filesystem::path push_marker =
        root / "push" / (std::to_string(id.value) + ".push");
    const std::filesystem::path push_recovery_marker =
        std::filesystem::path(push_marker.string() + ".tmp");
    const std::filesystem::path temporary_marker =
        root / "tmp" / std::to_string(id.value) / "partial.bin";
    std::filesystem::create_directories(files_marker.parent_path(), error);
    std::filesystem::create_directories(permission_marker.parent_path(), error);
    std::filesystem::create_directories(push_marker.parent_path(), error);
    std::filesystem::create_directories(temporary_marker.parent_path(), error);
    check(!error && write_text(files_marker, "remove") &&
              write_text(permission_marker, "remove") &&
              write_text(push_marker, "remove") &&
              write_text(push_recovery_marker, "remove") &&
              write_text(temporary_marker, "remove"),
          "create suite data removal markers");

    auto removed_data = store.uninstall(
        id,
        SuiteUninstallPolicy {
            .remove_rms = true,
            .remove_files = true,
            .remove_permissions = true,
            .remove_push = true,
            .remove_temporary = true,
        });
    check(removed_data.has_value(), "uninstall with data removal policy");
    check(!std::filesystem::exists(rms_marker.parent_path(), error) && !error,
          "remove RMS under explicit uninstall policy");
    check(!std::filesystem::exists(files_marker.parent_path(), error) && !error,
          "remove files under explicit uninstall policy");
    check(!std::filesystem::exists(permission_marker, error) && !error,
          "remove persisted permissions under explicit uninstall policy");
    check(!std::filesystem::exists(push_marker, error) && !error &&
              !std::filesystem::exists(push_recovery_marker, error) && !error,
          "remove push state under explicit uninstall policy");
    check(!std::filesystem::exists(temporary_marker.parent_path(), error) &&
              !error,
          "remove temporary suite state under explicit uninstall policy");
}

void test_transaction_faults(const std::filesystem::path& root,
                             const std::filesystem::path& jar_v1,
                             const std::filesystem::path& jar_v2) {
    const std::array store_fault_points {
        SuiteStoreFaultPoint::before_staged_jar_sync,
        SuiteStoreFaultPoint::before_staged_jad_sync,
        SuiteStoreFaultPoint::before_stage_directory_sync,
        SuiteStoreFaultPoint::before_stage_parent_sync,
        SuiteStoreFaultPoint::before_backup_rename,
        SuiteStoreFaultPoint::before_backup_parent_sync,
        SuiteStoreFaultPoint::before_activation_rename,
        SuiteStoreFaultPoint::before_activation_parent_sync,
    };

    for (usize index = 0; index < store_fault_points.size(); ++index) {
        const std::filesystem::path case_root =
            root / ("install-fault-" + std::to_string(index));
        std::error_code error;
        std::filesystem::remove_all(case_root, error);
        std::filesystem::create_directories(case_root, error);
        check(!error, "create install fault case root");

        const std::filesystem::path imported_v1 = case_root / "v1.jar";
        const std::filesystem::path imported_v2 = case_root / "v2.jar";
        const std::filesystem::path jad_v1 = case_root / "v1.jad";
        const std::filesystem::path jad_v2 = case_root / "v2.jad";
        check(copy_fixture_file(jar_v1, imported_v1) &&
                  copy_fixture_file(jar_v2, imported_v2),
              "copy transaction fault fixtures");
        check(write_text(jad_v1, make_jad(
                  imported_v1, "1.0.0", fixture_file_size(imported_v1))) &&
                  write_text(jad_v2, make_jad(
                      imported_v2, "1.1.0", fixture_file_size(imported_v2))),
              "write transaction fault JAD files");

        bool armed = false;
        bool fired = false;
        SuiteStore store;
        SuiteStoreConfig config;
        config.root_path = case_root.string();
        config.fault_injector = [&](SuiteStoreFaultPoint point) -> Status {
            if (armed && !fired && point == store_fault_points[index]) {
                fired = true;
                return phoneme::fail(
                    ErrorCode::io_error, "injected suite transaction fault");
            }
            return {};
        };
        auto configured = store.configure(config);
        check(configured.has_value(), "configure install fault store");
        if (!configured) continue;
        auto baseline = store.install(jad_v1.string(), imported_v1.string());
        check(baseline.has_value(), "install baseline before transaction fault");
        if (!baseline) continue;

        armed = true;
        auto upgraded = store.install(jad_v2.string(), imported_v2.string());
        check_error(upgraded, ErrorCode::io_error,
                    "surface injected suite transaction failure");
        check(fired, "execute requested suite transaction fault point");
        const auto* current = store.find(*baseline);
        check(current != nullptr && current->version == "1.0.0",
              "rollback in-memory suite after transaction failure");

        store.clear();
        auto restarted = store.configure(
            SuiteStoreConfig {.root_path = case_root.string()});
        check(restarted.has_value(),
              "restart store after transaction fault rollback");
        const auto* persisted = store.find(*baseline);
        check(persisted != nullptr && persisted->version == "1.0.0",
              "preserve durable baseline after transaction fault");
    }

    const std::array uninstall_fault_points {
        SuiteStoreFaultPoint::before_uninstall_rename,
        SuiteStoreFaultPoint::before_uninstall_parent_sync,
    };
    for (usize index = 0; index < uninstall_fault_points.size(); ++index) {
        const std::filesystem::path case_root =
            root / ("uninstall-fault-" + std::to_string(index));
        std::error_code error;
        std::filesystem::remove_all(case_root, error);
        std::filesystem::create_directories(case_root, error);
        const std::filesystem::path imported = case_root / "app.jar";
        const std::filesystem::path jad = case_root / "app.jad";
        check(copy_fixture_file(jar_v1, imported),
              "copy uninstall fault fixture");
        check(write_text(jad, make_jad(
                  imported, "1.0.0", fixture_file_size(imported))),
              "write uninstall fault JAD");

        bool armed = false;
        bool fired = false;
        SuiteStore store;
        SuiteStoreConfig config;
        config.root_path = case_root.string();
        config.fault_injector = [&](SuiteStoreFaultPoint point) -> Status {
            if (armed && !fired && point == uninstall_fault_points[index]) {
                fired = true;
                return phoneme::fail(
                    ErrorCode::io_error, "injected uninstall transaction fault");
            }
            return {};
        };
        check(store.configure(config).has_value(),
              "configure uninstall fault store");
        auto installed = store.install(jad.string(), imported.string());
        check(installed.has_value(), "install baseline for uninstall fault");
        if (!installed) continue;
        armed = true;
        auto uninstalled = store.uninstall(*installed);
        check(!uninstalled.has_value() &&
                  uninstalled.error().code == ErrorCode::io_error,
              "surface injected uninstall transaction failure");
        check(fired && store.find(*installed) != nullptr,
              "rollback uninstall transaction failure");
        store.clear();
        check(store.configure(
                  SuiteStoreConfig {.root_path = case_root.string()}).has_value(),
              "restart after uninstall transaction failure");
        check(store.find(*installed) != nullptr,
              "preserve suite after uninstall transaction rollback");
    }
}

void test_database_durability_unknown(
    const std::filesystem::path& root,
    const std::filesystem::path& jar_v1,
    const std::filesystem::path& jar_v2) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    check(!error, "create database fault root");

    const std::filesystem::path imported_v1 = root / "v1.jar";
    const std::filesystem::path imported_v2 = root / "v2.jar";
    const std::filesystem::path jad_v1 = root / "v1.jad";
    const std::filesystem::path jad_v2 = root / "v2.jad";
    check(copy_fixture_file(jar_v1, imported_v1) &&
              copy_fixture_file(jar_v2, imported_v2),
          "copy database durability fixtures");
    check(write_text(jad_v1, make_jad(
              imported_v1, "1.0.0", fixture_file_size(imported_v1))) &&
              write_text(jad_v2, make_jad(
                  imported_v2, "1.1.0", fixture_file_size(imported_v2))),
          "write database durability JAD files");

    bool armed = false;
    bool fired = false;
    SuiteStore store;
    SuiteStoreConfig config;
    config.root_path = root.string();
    config.database_fault_injector =
        [&](SuiteDatabaseFaultPoint point) -> Status {
            if (armed && !fired &&
                point == SuiteDatabaseFaultPoint::before_primary_directory_sync) {
                fired = true;
                return phoneme::fail(
                    ErrorCode::io_error,
                    "injected database directory sync failure");
            }
            return {};
        };
    check(store.configure(config).has_value(),
          "configure database durability store");
    auto baseline = store.install(jad_v1.string(), imported_v1.string());
    check(baseline.has_value(), "install database durability baseline");
    if (!baseline) return;

    armed = true;
    auto upgraded = store.install(jad_v2.string(), imported_v2.string());
    check(upgraded.has_value() && *upgraded == *baseline && fired,
          "preserve activated suite when database durability is unknown");
    const std::filesystem::path suite_backup =
        root / "suites" / (".backup-" + std::to_string(baseline->value));
    check(std::filesystem::is_directory(suite_backup, error) && !error,
          "retain old suite candidate while database durability is unknown");
    check(copy_fixture_file(root / "suites.db.bak", root / "suites.db"),
          "simulate crash selecting previous database generation");

    store.clear();
    auto recovered = store.configure(
        SuiteStoreConfig {.root_path = root.string()});
    check(recovered.has_value(),
          "recover previous database generation after uncertain commit");
    const auto* recovered_suite = store.find(*baseline);
    check(recovered_suite != nullptr && recovered_suite->version == "1.0.0",
          "select suite files matching recovered database generation");
    check(!std::filesystem::exists(suite_backup, error) && !error,
          "clean obsolete suite candidate after recovery");

    bool uninstall_armed = false;
    bool uninstall_fired = false;
    store.clear();
    SuiteStoreConfig uninstall_config;
    uninstall_config.root_path = root.string();
    uninstall_config.database_fault_injector =
        [&](SuiteDatabaseFaultPoint point) -> Status {
            if (uninstall_armed && !uninstall_fired &&
                point == SuiteDatabaseFaultPoint::before_primary_directory_sync) {
                uninstall_fired = true;
                return phoneme::fail(
                    ErrorCode::io_error,
                    "injected uninstall database sync failure");
            }
            return {};
        };
    check(store.configure(uninstall_config).has_value(),
          "configure uncertain uninstall store");
    const std::filesystem::path uncertain_rms =
        root / "rms" / std::to_string(baseline->value) / "marker.bin";
    const std::filesystem::path uncertain_files =
        root / "files" / std::to_string(baseline->value) / "marker.bin";
    const std::filesystem::path uncertain_permissions =
        root / "security" /
        (std::to_string(baseline->value) + ".permissions");
    std::filesystem::create_directories(uncertain_rms.parent_path(), error);
    std::filesystem::create_directories(uncertain_files.parent_path(), error);
    std::filesystem::create_directories(
        uncertain_permissions.parent_path(), error);
    check(!error && write_text(uncertain_rms, "keep") &&
              write_text(uncertain_files, "keep") &&
              write_text(uncertain_permissions, "keep"),
          "create data protected by uncertain uninstall");
    uninstall_armed = true;
    auto uncertain_uninstall = store.uninstall(
        *baseline,
        SuiteUninstallPolicy {
            .remove_rms = true,
            .remove_files = true,
            .remove_permissions = true,
        });
    check(uncertain_uninstall.has_value() && uninstall_fired,
          "preserve tombstone when uninstall database durability is unknown");
    const std::filesystem::path tombstone =
        root / "suites" / (".delete-" + std::to_string(baseline->value));
    check(std::filesystem::is_directory(tombstone, error) && !error,
          "retain uninstall candidate for crash recovery");
    check(std::filesystem::exists(uncertain_rms, error) && !error &&
              std::filesystem::exists(uncertain_files, error) && !error &&
              std::filesystem::exists(uncertain_permissions, error) && !error,
          "defer destructive uninstall policy while durability is unknown");
    check(copy_fixture_file(root / "suites.db.bak", root / "suites.db"),
          "simulate crash selecting database before uninstall");
    store.clear();
    check(store.configure(
              SuiteStoreConfig {.root_path = root.string()}).has_value(),
          "recover uncertain uninstall transaction");
    check(store.find(*baseline) != nullptr,
          "restore suite when recovered database still references it");
    check(std::filesystem::exists(uncertain_rms, error) && !error &&
              std::filesystem::exists(uncertain_files, error) && !error &&
              std::filesystem::exists(uncertain_permissions, error) && !error,
          "preserve suite data when uncertain uninstall is rolled back");

    const std::filesystem::path orphan_root = root / "orphan-install";
    std::filesystem::remove_all(orphan_root, error);
    std::filesystem::create_directories(orphan_root, error);
    const std::filesystem::path orphan_jar = orphan_root / "app.jar";
    const std::filesystem::path orphan_jad = orphan_root / "app.jad";
    check(copy_fixture_file(jar_v1, orphan_jar) &&
              write_text(orphan_jad, make_jad(
                  orphan_jar, "1.0.0", fixture_file_size(orphan_jar))),
          "prepare uncertain first install fixture");
    bool first_install_fired = false;
    SuiteStore orphan_store;
    SuiteStoreConfig orphan_config;
    orphan_config.root_path = orphan_root.string();
    orphan_config.database_fault_injector =
        [&](SuiteDatabaseFaultPoint point) -> Status {
            if (!first_install_fired &&
                point == SuiteDatabaseFaultPoint::before_primary_directory_sync) {
                first_install_fired = true;
                return phoneme::fail(
                    ErrorCode::io_error,
                    "injected first-install database sync failure");
            }
            return {};
        };
    check(orphan_store.configure(orphan_config).has_value(),
          "configure uncertain first install store");
    auto uncertain_first = orphan_store.install(
        orphan_jad.string(), orphan_jar.string());
    check(uncertain_first.has_value() && first_install_fired,
          "complete first install with unknown database durability");
    std::filesystem::remove(orphan_root / "suites.db", error);
    check(!error, "simulate loss of unsynced first database generation");
    orphan_store.clear();
    check(orphan_store.configure(
              SuiteStoreConfig {.root_path = orphan_root.string()}).has_value(),
          "restart after losing first database generation");
    check(orphan_store.list().empty(),
          "do not expose orphan suite without a persistent database record");
    check(!std::filesystem::exists(
              orphan_root / "suites" /
                  std::to_string(uncertain_first->value), error) && !error,
          "remove unreferenced numeric suite directory during recovery");
}

void test_validation(const std::filesystem::path& root,
                     const std::filesystem::path& jar_v1,
                     const std::filesystem::path& missing_class_jar,
                     const std::filesystem::path& traversal_jar,
                     const std::filesystem::path& zip_bomb_jar) {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    check(!error, "create validation root");

    SuiteStore jar_only_store;
    auto jar_only_configured = jar_only_store.configure(
        SuiteStoreConfig {.root_path = (root / "jar-only").string()});
    check(jar_only_configured.has_value(), "configure JAR-only suite store");
    auto jar_only_id = jar_only_store.install(jar_v1.string());
    check(jar_only_id.has_value(), "install suite directly from JAR manifest");
    if (jar_only_id) {
        const auto* jar_only_suite = jar_only_store.find(*jar_only_id);
        check(jar_only_suite != nullptr && jar_only_suite->jad_path.empty() &&
                  jar_only_suite->version == "1.0.0",
              "persist JAR-only suite without synthetic JAD");
        check(jar_only_suite != nullptr &&
                  jar_only_suite->declared_required_permissions.size() == 2U &&
                  jar_only_suite->declared_optional_permissions.empty(),
              "preserve structured manifest permission categories");
        check(jar_only_suite != nullptr &&
                  jar_only_suite->trust_evidence.has_signature_metadata() &&
                  !jar_only_suite->trust_evidence.cryptographically_verified(),
              "preserve signature evidence for JAR-only install");
    }

    auto jar_only_descriptor = SuiteInstaller::inspect(jar_v1.string());
    check(jar_only_descriptor.has_value(), "inspect JAR-only descriptor");
    if (jar_only_descriptor) {
        check(SuiteInstaller::digest_hex(
                  jar_only_descriptor->identity_sha256) ==
                  "1b2844537f8ce654380bc25cef6e0ae654f2ad13cefd4d3e9116c7bc5607530c",
              "derive stable suite identity using SHA-256");
    }

    const std::filesystem::path mismatch_jad = root / "mismatch.jad";
    check(write_text(mismatch_jad,
                     make_jad(jar_v1, "1.0.0", fixture_file_size(jar_v1) + 1U)),
          "write JAD with mismatched size");
    auto mismatch = SuiteInstaller::inspect(
        jar_v1.string(), std::optional<std::string>(mismatch_jad.string()));
    check_error(mismatch, ErrorCode::checksum_mismatch,
                "reject JAD size mismatch");

    const std::filesystem::path unsupported_profile_jad =
        root / "unsupported-profile.jad";
    check(write_text(
              unsupported_profile_jad,
              make_jad(jar_v1, "1.0.0", fixture_file_size(jar_v1),
                       false, "MIDP-3.0", "CLDC-1.1")),
          "write unsupported MIDP profile JAD");
    auto unsupported_profile = SuiteInstaller::inspect(
        jar_v1.string(),
        std::optional<std::string>(unsupported_profile_jad.string()));
    check_error(unsupported_profile, ErrorCode::unsupported_feature,
                "reject unsupported MIDP profile version");

    const std::filesystem::path unsupported_configuration_jad =
        root / "unsupported-configuration.jad";
    check(write_text(
              unsupported_configuration_jad,
              make_jad(jar_v1, "1.0.0", fixture_file_size(jar_v1),
                       false, "MIDP-2.0", "CLDC-2.0")),
          "write unsupported CLDC configuration JAD");
    auto unsupported_configuration = SuiteInstaller::inspect(
        jar_v1.string(),
        std::optional<std::string>(unsupported_configuration_jad.string()));
    check_error(unsupported_configuration, ErrorCode::unsupported_feature,
                "reject unsupported CLDC configuration version");

    const std::filesystem::path mixed_profile_jad = root / "mixed-profile.jad";
    check(write_text(
              mixed_profile_jad,
              make_jad(jar_v1, "1.0.0", fixture_file_size(jar_v1),
                       false, "MIDP-3.0 MIDP-2.0", "CLDC-1.1")),
          "write mixed supported profile JAD");
    auto mixed_profile = SuiteInstaller::inspect(
        jar_v1.string(),
        std::optional<std::string>(mixed_profile_jad.string()));
    check(mixed_profile.has_value(),
          "accept capability list containing an exact supported profile");

    const std::filesystem::path newest_capabilities_jad =
        root / "newest-capabilities.jad";
    check(write_text(
              newest_capabilities_jad,
              make_jad(jar_v1, "1.0.0", fixture_file_size(jar_v1),
                       false, "MIDP-2.1", "CLDC-1.1.1")),
          "write MIDP 2.1 and CLDC 1.1.1 JAD");
    auto newest_capabilities = SuiteInstaller::inspect(
        jar_v1.string(),
        std::optional<std::string>(newest_capabilities_jad.string()));
    check(newest_capabilities.has_value(),
          "accept phoneME MIDP 2.1 and CLDC 1.1.1 versions");

    const std::filesystem::path legacy_capabilities_jad =
        root / "legacy-capabilities.jad";
    check(write_text(
              legacy_capabilities_jad,
              make_jad(jar_v1, "1.0.0", fixture_file_size(jar_v1),
                       false, "MIDP-1.0", "CLDC-1.0")),
          "write MIDP 1.0 and CLDC 1.0 JAD");
    auto legacy_capabilities = SuiteInstaller::inspect(
        jar_v1.string(),
        std::optional<std::string>(legacy_capabilities_jad.string()));
    check(legacy_capabilities.has_value(),
          "retain legacy MIDP 1.0 and CLDC 1.0 support");

    auto missing = SuiteInstaller::inspect(missing_class_jar.string());
    check_error(missing, ErrorCode::class_not_found,
                "reject manifest referencing missing MIDlet class");

    auto traversal = SuiteInstaller::inspect(traversal_jar.string());
    check_error(traversal, ErrorCode::malformed_archive,
                "reject JAR path traversal entry");

    auto bomb = SuiteInstaller::inspect(zip_bomb_jar.string());
    check_error(bomb, ErrorCode::out_of_range,
                "reject JAR compression-ratio bomb");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 8) {
        std::cerr << "usage: SuiteInstallerTests <root> <v1.jar> <v2.jar> "
                     "<same-version-changed.jar> <missing.jar> "
                     "<traversal.jar> <zipbomb.jar>\n";
        return 2;
    }

    const std::filesystem::path root(argv[1]);
    test_parser();
    test_version_comparison();
    test_scoped_same_version_install(
        root / "scoped", argv[2], argv[4]);
    test_install_flow(root / "store", argv[2], argv[3]);
    test_transaction_faults(root / "transaction-faults", argv[2], argv[3]);
    test_database_durability_unknown(
        root / "database-durability", argv[2], argv[3]);
    test_validation(root / "validation", argv[2], argv[5], argv[6], argv[7]);

    if (failures != 0) {
        std::cerr << failures << " suite installer test(s) failed\n";
        return 1;
    }
    std::cout << "Suite installer tests passed\n";
    return 0;
}
