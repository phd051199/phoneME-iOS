#include "PhoneMECore.h"
#include "JarArchive.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <utility>

#include "phoneme/archive/ZipArchive.hpp"
#include "phoneme/runtime/Runtime.hpp"
#include "phoneme/runtime/WorkCoordinator.hpp"

namespace {

using phoneme::ErrorCode;
using phoneme::i32;
using phoneme::runtime::Runtime;

std::atomic<i32> g_last_install_stage {0};
std::atomic<i32> g_last_suite_store_stage {0};

[[nodiscard]] Runtime* cast_runtime(PhoneMERuntimeRef runtime) noexcept {
    return static_cast<Runtime*>(runtime);
}

[[nodiscard]] i32 map_error(const phoneme::Error& error) noexcept {
    switch (error.code) {
    case ErrorCode::invalid_argument:
    case ErrorCode::out_of_range:
    case ErrorCode::overflow:
        return PHONEME_ERROR_INVALID_ARGUMENT;
    case ErrorCode::already_running:
        return PHONEME_ERROR_ALREADY_RUNNING;
    case ErrorCode::not_configured:
        return PHONEME_ERROR_NOT_CONFIGURED;
    case ErrorCode::not_running:
        return PHONEME_ERROR_NOT_RUNNING;
    case ErrorCode::io_error:
        return PHONEME_ERROR_IO;
    case ErrorCode::malformed_archive:
    case ErrorCode::checksum_mismatch:
    case ErrorCode::malformed_class:
    case ErrorCode::verification_failed:
    case ErrorCode::unsupported_class_version:
        return PHONEME_ERROR_MALFORMED_INPUT;
    case ErrorCode::unsupported_archive:
    case ErrorCode::unsupported_feature:
        return PHONEME_ERROR_UNSUPPORTED;
    case ErrorCode::class_not_found:
    case ErrorCode::method_not_found:
        return PHONEME_ERROR_INSTALL;
    case ErrorCode::java_exception:
        return PHONEME_ERROR_SYSTEM_START;
    case ErrorCode::invalid_state:
    case ErrorCode::internal_error:
    case ErrorCode::none:
        return PHONEME_ERROR_SYSTEM_START;
    }
    return PHONEME_ERROR_SYSTEM_START;
}

[[nodiscard]] i32 status_code(Runtime* runtime,
                              const phoneme::Status& status) {
    if (status) {
        runtime->clear_error();
        return PHONEME_OK;
    }

    const phoneme::Error& error = status.error();
    runtime->record_error(error);
    const i32 code = map_error(error);
    std::fprintf(
        stderr,
        "phoneME C API error %d (core=%d): %s%s%s\n",
        code,
        static_cast<int>(error.code),
        error.java_exception_class.empty()
            ? ""
            : error.java_exception_class.c_str(),
        error.java_exception_class.empty() ? "" : ": ",
        error.message.c_str());
    return code;
}

void copy_utf8(char* destination,
               std::size_t capacity,
               const std::string& source) noexcept {
    if (destination == nullptr || capacity == 0) {
        return;
    }
    const std::size_t count = std::min(source.size(), capacity - 1);
    std::memcpy(destination, source.data(), count);
    destination[count] = '\0';
}

[[nodiscard]] i32 copy_diagnostic(const std::string& message,
                                  char* destination,
                                  i32 capacity) noexcept {
    if (capacity < 0 || (capacity > 0 && destination == nullptr) ||
        message.size() > static_cast<std::size_t>(
            std::numeric_limits<i32>::max())) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    if (capacity > 0) {
        copy_utf8(destination, static_cast<std::size_t>(capacity), message);
    }
    return static_cast<i32>(message.size());
}

[[nodiscard]] phoneme::security::PermissionDecision permission_decision(
    int32_t decision) noexcept {
    switch (decision) {
    case PHONEME_PERMISSION_ALLOWED:
        return phoneme::security::PermissionDecision::allowed;
    case PHONEME_PERMISSION_UNKNOWN:
        return phoneme::security::PermissionDecision::unknown;
    case PHONEME_PERMISSION_DENIED:
    default:
        return phoneme::security::PermissionDecision::denied;
    }
}

[[nodiscard]] phoneme::security::PermissionScope permission_scope(
    int32_t scope) noexcept {
    switch (scope) {
    case PHONEME_PERMISSION_SESSION:
        return phoneme::security::PermissionScope::session;
    case PHONEME_PERMISSION_BLANKET:
        return phoneme::security::PermissionScope::blanket;
    case PHONEME_PERMISSION_ONESHOT:
    default:
        return phoneme::security::PermissionScope::one_shot;
    }
}

[[nodiscard]] i32 frame_byte_count(
    const phoneme::runtime::FrameMetadata& frame,
    i32* width,
    i32* height,
    std::uint64_t* generation) noexcept {
    if (width != nullptr) {
        *width = frame.dimensions.width;
    }
    if (height != nullptr) {
        *height = frame.dimensions.height;
    }
    if (generation != nullptr) {
        *generation = frame.generation;
    }

    if (frame.byte_count >
        static_cast<std::size_t>(std::numeric_limits<i32>::max())) {
        return 0;
    }
    return static_cast<i32>(frame.byte_count);
}

} // namespace

extern "C" {

uint32_t phoneme_c_api_version(void) {
    return PHONEME_C_API_VERSION;
}

int32_t phoneme_preverify_jar_classes(
    const char* runtime_classes_path,
    const char* jar_path,
    const char* class_list_path,
    const char* output_directory,
    PhoneMEPreverifyResult* result_out) {
    (void)runtime_classes_path;
    if (result_out != nullptr) {
        *result_out = {};
    }
    if (jar_path == nullptr || class_list_path == nullptr ||
        output_directory == nullptr || result_out == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }

    auto archive = phoneme::archive::ZipArchive::open(jar_path);
    if (!archive) {
        return map_error(archive.error());
    }

    for (const auto& entry : archive->entries()) {
        if (!entry.name.ends_with(".class")) {
            continue;
        }
        ++result_out->attempted;
        auto bytes = archive->read(entry);
        if (!bytes) {
            ++result_out->failed;
            continue;
        }
        auto parsed = phoneme::classfile::ClassFile::parse(*bytes);
        if (parsed) {
            ++result_out->skipped;
        } else {
            ++result_out->failed;
        }
    }

    return result_out->failed == 0 ? PHONEME_ERROR_UNSUPPORTED
                                   : PHONEME_ERROR_MALFORMED_INPUT;
}

PhoneMERuntimeRef phoneme_create(void) {
    return new (std::nothrow) Runtime();
}

void phoneme_destroy(PhoneMERuntimeRef runtime) {
    delete cast_runtime(runtime);
}

void phoneme_set_host_wake_callback(PhoneMERuntimeRef runtime,
                                    PhoneMEHostWakeCallback callback,
                                    void* context) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) return;
    if (callback == nullptr) {
        instance->configure_host_wake({});
        return;
    }
    instance->configure_host_wake(
        [callback, context] { callback(context); });
}

int32_t phoneme_configure(PhoneMERuntimeRef runtime,
                          const char* runtime_home,
                          const char* classes_zip) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || runtime_home == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(
        instance,
        instance->configure(
            runtime_home,
            classes_zip == nullptr ? std::string {} : std::string(classes_zip)));
}

int32_t phoneme_configure_keymap(PhoneMERuntimeRef runtime,
                                 int32_t up,
                                 int32_t down,
                                 int32_t left,
                                 int32_t right,
                                 int32_t fire,
                                 int32_t soft1,
                                 int32_t soft2) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(
        instance,
        instance->configure_keymap(
            std::array<i32, 7> {up, down, left, right, fire, soft1, soft2}));
}

int32_t phoneme_configure_app_frame_pacing(
    PhoneMERuntimeRef runtime,
    int32_t app_id,
    int32_t frames_per_second,
    int32_t pacing_mode) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(
        instance,
        instance->configure_app_frame_pacing(
            phoneme::AppId {app_id},
            frames_per_second,
            static_cast<phoneme::vm::FramePacingMode>(pacing_mode)));
}

int32_t phoneme_configure_app_heap(PhoneMERuntimeRef runtime,
                                   int32_t app_id,
                                   int32_t heap_megabytes) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(
        instance,
        instance->configure_app_heap(
            phoneme::AppId {app_id}, heap_megabytes));
}

int32_t phoneme_configure_jit(PhoneMERuntimeRef runtime, int32_t enabled) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(instance, instance->configure_jit(enabled != 0));
}

int32_t phoneme_set_thermal_pressure(PhoneMERuntimeRef runtime,
                                     int32_t pressure) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || pressure < PHONEME_THERMAL_NOMINAL ||
        pressure > PHONEME_THERMAL_CRITICAL) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    phoneme::runtime::shared_work_coordinator().set_thermal_pressure(
        static_cast<phoneme::runtime::ThermalPressure>(pressure));
    instance->clear_error();
    return PHONEME_OK;
}

int32_t phoneme_jit_status(void) {
    return static_cast<int32_t>(
        phoneme::vm::BaselineJit::probe_platform());
}

int32_t phoneme_configure_translation(PhoneMERuntimeRef runtime,
                                      int32_t enabled,
                                      const char* source_language,
                                      const char* target_language) {
    return phoneme_configure_translation_v2(
        runtime, enabled, PHONEME_TRANSLATION_PROVIDER_AUTOMATIC,
        source_language, target_language);
}

int32_t phoneme_configure_translation_v2(PhoneMERuntimeRef runtime,
                                         int32_t enabled,
                                         int32_t provider,
                                         const char* source_language,
                                         const char* target_language) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr ||
        (provider != PHONEME_TRANSLATION_PROVIDER_GOOGLE &&
         provider != PHONEME_TRANSLATION_PROVIDER_BING &&
         provider != PHONEME_TRANSLATION_PROVIDER_AUTOMATIC)) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(
        instance,
        instance->configure_translation(
            enabled != 0,
            static_cast<phoneme::translation::TranslationProvider>(provider),
            source_language == nullptr ? std::string("auto")
                                       : std::string(source_language),
            target_language == nullptr ? std::string("vi")
                                       : std::string(target_language)));
}

int32_t phoneme_configure_app_translation(
    PhoneMERuntimeRef runtime,
    int32_t app_id,
    int32_t enabled,
    const char* source_language,
    const char* target_language) {
    return phoneme_configure_app_translation_v2(
        runtime, app_id, enabled, PHONEME_TRANSLATION_PROVIDER_AUTOMATIC,
        source_language, target_language);
}

int32_t phoneme_configure_app_translation_v2(
    PhoneMERuntimeRef runtime,
    int32_t app_id,
    int32_t enabled,
    int32_t provider,
    const char* source_language,
    const char* target_language) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr ||
        (provider != PHONEME_TRANSLATION_PROVIDER_GOOGLE &&
         provider != PHONEME_TRANSLATION_PROVIDER_BING &&
         provider != PHONEME_TRANSLATION_PROVIDER_AUTOMATIC)) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(
        instance,
        instance->configure_app_translation(
            phoneme::AppId {app_id},
            enabled != 0,
            static_cast<phoneme::translation::TranslationProvider>(provider),
            source_language == nullptr ? std::string("auto")
                                       : std::string(source_language),
            target_language == nullptr ? std::string("vi")
                                       : std::string(target_language)));
}

int32_t phoneme_configure_permission_prompt(
    PhoneMERuntimeRef runtime,
    PhoneMEPermissionPromptCallback callback,
    void* context) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    if (callback == nullptr) {
        return status_code(
            instance, instance->configure_permission_prompt({}));
    }

    return status_code(instance, instance->configure_permission_prompt(
        [callback, context](const phoneme::security::PermissionRequest& request) {
            const PhoneMEPermissionRequest native_request {
                .suite_id = request.suite_id.value,
                .trust = static_cast<int32_t>(phoneme::to_underlying(request.trust)),
                .domain = static_cast<int32_t>(phoneme::to_underlying(request.domain)),
                .user_initiated = request.user_initiated ? 1 : 0,
                .permission = request.permission.c_str(),
                .resource = request.resource.c_str(),
            };
            const PhoneMEPermissionResponse native_response =
                callback(context, &native_request);
            return phoneme::security::PermissionResponse {
                .decision = permission_decision(native_response.decision),
                .scope = permission_scope(native_response.scope),
            };
        }));
}

int32_t phoneme_set_suite_trust(PhoneMERuntimeRef runtime,
                                int32_t suite_id,
                                int32_t trust) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr ||
        (trust != PHONEME_SUITE_UNTRUSTED &&
         trust != PHONEME_SUITE_TRUSTED)) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(
        instance,
        instance->set_suite_trust(
            phoneme::SuiteId {suite_id},
            trust == PHONEME_SUITE_TRUSTED
                ? phoneme::security::SuiteTrust::trusted
                : phoneme::security::SuiteTrust::untrusted));
}

int32_t phoneme_install_jar(PhoneMERuntimeRef runtime,
                            const char* jar_path,
                            int32_t* suite_id_out) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || jar_path == nullptr || suite_id_out == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }

    g_last_install_stage.store(1, std::memory_order_relaxed);
    auto suite = instance->install_jar(jar_path);
    if (!suite) {
        g_last_install_stage.store(-1, std::memory_order_relaxed);
        instance->record_error(suite.error());
        return map_error(suite.error());
    }
    instance->clear_error();
    *suite_id_out = suite->value;
    g_last_suite_store_stage.store(1, std::memory_order_relaxed);
    g_last_install_stage.store(2, std::memory_order_relaxed);
    return PHONEME_OK;
}

int32_t phoneme_install_jar_scoped(PhoneMERuntimeRef runtime,
                                   const char* jar_path,
                                   const char* identity_scope,
                                   int32_t* suite_id_out) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || jar_path == nullptr || identity_scope == nullptr ||
        suite_id_out == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }

    g_last_install_stage.store(1, std::memory_order_relaxed);
    auto suite = instance->install_jar(jar_path, identity_scope);
    if (!suite) {
        g_last_install_stage.store(-1, std::memory_order_relaxed);
        instance->record_error(suite.error());
        return map_error(suite.error());
    }
    instance->clear_error();
    *suite_id_out = suite->value;
    g_last_suite_store_stage.store(1, std::memory_order_relaxed);
    g_last_install_stage.store(2, std::memory_order_relaxed);
    return PHONEME_OK;
}

int32_t phoneme_find_installed_suite(PhoneMERuntimeRef runtime,
                                     const char* vendor,
                                     const char* name,
                                     const char* version,
                                     int32_t* suite_id_out) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || vendor == nullptr || name == nullptr ||
        version == nullptr || suite_id_out == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }

    const auto suite = instance->find_installed_suite(vendor, name, version);
    *suite_id_out = suite.has_value() ? suite->value : 0;
    instance->clear_error();
    return PHONEME_OK;
}

int32_t phoneme_find_installed_suite_scoped(
    PhoneMERuntimeRef runtime,
    const char* vendor,
    const char* name,
    const char* version,
    const char* identity_scope,
    int32_t* suite_id_out) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || vendor == nullptr || name == nullptr ||
        version == nullptr || identity_scope == nullptr ||
        suite_id_out == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }

    const auto suite = instance->find_installed_suite(
        vendor, name, version, identity_scope);
    *suite_id_out = suite.has_value() ? suite->value : 0;
    instance->clear_error();
    return PHONEME_OK;
}

int32_t phoneme_uninstall_suite(PhoneMERuntimeRef runtime,
                                int32_t suite_id,
                                int32_t remove_data) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || suite_id <= 0) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(
        instance,
        instance->uninstall_suite(
            phoneme::SuiteId {suite_id},
            remove_data != 0));
}

int32_t phoneme_last_install_stage(void) {
    return g_last_install_stage.load(std::memory_order_relaxed);
}

int32_t phoneme_last_suite_store_stage(void) {
    return g_last_suite_store_stage.load(std::memory_order_relaxed);
}

int32_t phoneme_start_system(PhoneMERuntimeRef runtime) {
    Runtime* instance = cast_runtime(runtime);
    return instance == nullptr
        ? PHONEME_ERROR_INVALID_ARGUMENT
        : status_code(instance, instance->start_system());
}

int32_t phoneme_start_midlet(PhoneMERuntimeRef runtime,
                             int32_t suite_id,
                             const char* main_class,
                             int32_t app_id,
                             int32_t screen_width,
                             int32_t screen_height) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || main_class == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(
        instance,
        instance->start_midlet(
            phoneme::SuiteId {suite_id},
            main_class,
            phoneme::AppId {app_id},
            phoneme::Dimensions {screen_width, screen_height}));
}

int32_t phoneme_set_foreground(PhoneMERuntimeRef runtime,
                               int32_t app_id,
                               int32_t screen_width,
                               int32_t screen_height) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(
        instance,
        instance->set_foreground(
            phoneme::AppId {app_id},
            phoneme::Dimensions {screen_width, screen_height}));
}

int32_t phoneme_pause_midlet(PhoneMERuntimeRef runtime, int32_t app_id) {
    Runtime* instance = cast_runtime(runtime);
    return instance == nullptr
        ? PHONEME_ERROR_INVALID_ARGUMENT
        : status_code(
              instance, instance->pause_midlet(phoneme::AppId {app_id}));
}

int32_t phoneme_resume_midlet(PhoneMERuntimeRef runtime, int32_t app_id) {
    Runtime* instance = cast_runtime(runtime);
    return instance == nullptr
        ? PHONEME_ERROR_INVALID_ARGUMENT
        : status_code(
              instance, instance->resume_midlet(phoneme::AppId {app_id}));
}

int32_t phoneme_destroy_midlet(PhoneMERuntimeRef runtime, int32_t app_id) {
    Runtime* instance = cast_runtime(runtime);
    return instance == nullptr
        ? PHONEME_ERROR_INVALID_ARGUMENT
        : status_code(
              instance, instance->destroy_midlet(phoneme::AppId {app_id}));
}

int32_t phoneme_push_set_background_policy(PhoneMERuntimeRef runtime,
                                           int32_t suite_id,
                                           int32_t policy) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr ||
        (policy != PHONEME_PUSH_FOREGROUND_ONLY &&
         policy != PHONEME_PUSH_SYSTEM_MANAGED)) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    const auto native_policy = policy == PHONEME_PUSH_SYSTEM_MANAGED
        ? phoneme::push::BackgroundPolicy::system_managed
        : phoneme::push::BackgroundPolicy::foreground_only;
    return status_code(
        instance,
        instance->set_push_background_policy(
            phoneme::SuiteId {suite_id}, native_policy));
}

int32_t phoneme_push_notify_connection_available(
    PhoneMERuntimeRef runtime,
    int32_t suite_id,
    const char* connection,
    int64_t received_at_millis) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || connection == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(
        instance,
        instance->notify_push_connection_available(
            phoneme::SuiteId {suite_id}, connection, received_at_millis));
}

int32_t phoneme_push_notify_connection_available_from_source(
    PhoneMERuntimeRef runtime,
    int32_t suite_id,
    const char* connection,
    const char* source_address,
    int64_t received_at_millis) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || connection == nullptr ||
        source_address == nullptr) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return status_code(
        instance,
        instance->notify_push_connection_available(
            phoneme::SuiteId {suite_id}, connection, source_address,
            received_at_millis));
}

int32_t phoneme_push_poll_launch_requests(
    PhoneMERuntimeRef runtime,
    int32_t suite_id,
    int64_t now_millis,
    int32_t background_execution_granted,
    PhoneMEPushLaunchRequest* destination,
    int32_t capacity) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || capacity < 0 ||
        (capacity > 0 && destination == nullptr)) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }

    constexpr phoneme::usize kMaximumQueryCount = 4'096U;
    const phoneme::usize limit = capacity == 0
        ? kMaximumQueryCount
        : static_cast<phoneme::usize>(capacity);
    auto requests = instance->poll_push_launch_requests(
        phoneme::SuiteId {suite_id},
        now_millis,
        background_execution_granted != 0,
        limit);
    if (!requests) {
        instance->record_error(requests.error());
        return map_error(requests.error());
    }
    instance->clear_error();

    if (requests->size() >
        static_cast<phoneme::usize>(std::numeric_limits<int32_t>::max())) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    if (destination != nullptr) {
        for (phoneme::usize index = 0; index < requests->size(); ++index) {
            const phoneme::push::LaunchRequest& request = (*requests)[index];
            PhoneMEPushLaunchRequest native_request {};
            native_request.request_id = request.id;
            native_request.kind = static_cast<int32_t>(
                phoneme::to_underlying(request.kind));
            native_request.created_at_millis = request.created_at_millis;
            copy_utf8(native_request.target,
                      sizeof(native_request.target),
                      request.target);
            copy_utf8(native_request.midlet,
                      sizeof(native_request.midlet),
                      request.midlet);
            destination[index] = native_request;
        }
    }
    return static_cast<int32_t>(requests->size());
}

int32_t phoneme_push_acknowledge_launch_request(
    PhoneMERuntimeRef runtime,
    int32_t suite_id,
    uint64_t request_id) {
    Runtime* instance = cast_runtime(runtime);
    return instance == nullptr
        ? PHONEME_ERROR_INVALID_ARGUMENT
        : status_code(
              instance,
              instance->acknowledge_push_launch_request(
                  phoneme::SuiteId {suite_id}, request_id));
}

int32_t phoneme_midlet_state(PhoneMERuntimeRef runtime, int32_t app_id) {
    Runtime* instance = cast_runtime(runtime);
    return instance == nullptr
        ? PHONEME_APP_STATE_NONE
        : phoneme::to_underlying(instance->app_state(phoneme::AppId {app_id}));
}

int32_t phoneme_foreground_app_id(PhoneMERuntimeRef runtime) {
    Runtime* instance = cast_runtime(runtime);
    return instance == nullptr ? 0 : instance->foreground_app_id().value;
}

int64_t phoneme_midlet_used_memory(PhoneMERuntimeRef runtime,
                                   int32_t app_id,
                                   int32_t timeout_milliseconds) {
    (void)timeout_milliseconds;
    Runtime* instance = cast_runtime(runtime);
    return instance == nullptr
        ? -1
        : instance->app_used_memory(phoneme::AppId {app_id});
}

int32_t phoneme_start_jar(PhoneMERuntimeRef runtime,
                          const char* jar_path,
                          const char* main_class,
                          int32_t screen_width,
                          int32_t screen_height) {
    int32_t suite_id = 0;
    const int32_t install = phoneme_install_jar(runtime, jar_path, &suite_id);
    if (install != PHONEME_OK) {
        return install;
    }
    const int32_t start = phoneme_start_system(runtime);
    if (start != PHONEME_OK) {
        return start;
    }
    return phoneme_start_midlet(runtime, suite_id, main_class, 1,
                                screen_width, screen_height);
}

void phoneme_stop(PhoneMERuntimeRef runtime) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->stop();
    }
}

void phoneme_suspend(PhoneMERuntimeRef runtime) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->suspend();
    }
}

void phoneme_flush_rms(PhoneMERuntimeRef runtime) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->flush_record_stores();
    }
}

void phoneme_resume(PhoneMERuntimeRef runtime) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->resume();
    }
}

int32_t phoneme_is_running(PhoneMERuntimeRef runtime) {
    Runtime* instance = cast_runtime(runtime);
    return instance != nullptr && instance->is_running() ? 1 : 0;
}

int32_t phoneme_is_suspended(PhoneMERuntimeRef runtime) {
    Runtime* instance = cast_runtime(runtime);
    return instance != nullptr && instance->is_suspended() ? 1 : 0;
}

int32_t phoneme_last_exit_code(PhoneMERuntimeRef runtime) {
    Runtime* instance = cast_runtime(runtime);
    return instance == nullptr ? PHONEME_ERROR_INVALID_ARGUMENT
                               : instance->last_exit_code();
}

int32_t phoneme_copy_last_error_message(PhoneMERuntimeRef runtime,
                                        char* destination,
                                        int32_t capacity) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) return PHONEME_ERROR_INVALID_ARGUMENT;
    return copy_diagnostic(
        instance->last_error_message(), destination, capacity);
}

int32_t phoneme_copy_midlet_error_message(PhoneMERuntimeRef runtime,
                                          int32_t app_id,
                                          char* destination,
                                          int32_t capacity) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || app_id <= 0) {
        return PHONEME_ERROR_INVALID_ARGUMENT;
    }
    return copy_diagnostic(
        instance->app_error_message(phoneme::AppId {app_id}),
        destination,
        capacity);
}

void phoneme_send_key(PhoneMERuntimeRef runtime,
                      int32_t key_code,
                      int32_t pressed) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->send_key(key_code, pressed != 0);
    }
}

void phoneme_send_pointer(PhoneMERuntimeRef runtime,
                          int32_t x,
                          int32_t y,
                          int32_t action) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->send_pointer(x, y, action);
    }
}

void phoneme_pump_events(PhoneMERuntimeRef runtime) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->pump_events();
    }
}

int32_t phoneme_copy_frame_rgba(PhoneMERuntimeRef runtime,
                                uint8_t* destination,
                                int32_t capacity,
                                int32_t* width,
                                int32_t* height,
                                uint64_t* generation) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) {
        return 0;
    }

    // The size/generation query pumps the Canvas exactly once without copying
    // the full framebuffer. The immediately following data call copies the
    // latest complete frame under the framebuffer lock without pumping again.
    // This avoids presenting pixels assembled from two separate VM pumps and
    // removes the duplicate full-frame allocation from every host refresh.
    if (destination == nullptr || capacity <= 0) {
        return frame_byte_count(instance->frame_metadata(),
                                width, height, generation);
    }

    const auto frame = instance->copy_current_frame_rgba(
        std::span<std::uint8_t>(destination,
                                static_cast<std::size_t>(capacity)));
    return frame_byte_count(frame, width, height, generation);
}

int32_t phoneme_copy_frame_rgba_since(PhoneMERuntimeRef runtime,
                                      uint64_t previous_generation,
                                      uint8_t* destination,
                                      int32_t capacity,
                                      int32_t* width,
                                      int32_t* height,
                                      uint64_t* generation) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) {
        return 0;
    }

    instance->pump_events();
    std::span<std::uint8_t> output;
    if (destination != nullptr && capacity > 0) {
        output = std::span<std::uint8_t>(
            destination,
            static_cast<std::size_t>(capacity));
    }
    const auto frame = instance->copy_current_frame_rgba_since(
        previous_generation,
        output);
    if (!frame.has_value()) {
        return 0;
    }
    return frame_byte_count(*frame, width, height, generation);
}

const uint8_t* phoneme_acquire_frame_rgba_since(
    PhoneMERuntimeRef runtime,
    uint64_t previous_generation,
    int32_t* width,
    int32_t* height,
    uint64_t* generation) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) return nullptr;

    // A leaked lease from an interrupted host frame must never block the VM
    // pump that produces the next frame.
    instance->release_current_frame_rgba();
    instance->pump_events();
    const auto frame = instance->acquire_current_frame_rgba_since(
        previous_generation);
    if (!frame.has_value()) return nullptr;
    if (width != nullptr) width[0] = frame->metadata.dimensions.width;
    if (height != nullptr) height[0] = frame->metadata.dimensions.height;
    if (generation != nullptr) generation[0] = frame->metadata.generation;
    return frame->pixels;
}

const uint8_t* phoneme_acquire_current_frame_rgba_since(
    PhoneMERuntimeRef runtime,
    uint64_t previous_generation,
    int32_t* width,
    int32_t* height,
    uint64_t* generation) {
    return phoneme_acquire_current_frame_rgba_regions_since(
        runtime,
        previous_generation,
        width,
        height,
        generation,
        nullptr,
        0,
        nullptr);
}

const uint8_t* phoneme_acquire_current_frame_rgba_regions_since(
    PhoneMERuntimeRef runtime,
    uint64_t previous_generation,
    int32_t* width,
    int32_t* height,
    uint64_t* generation,
    PhoneMEFrameDamageRegion* regions,
    int32_t region_capacity,
    int32_t* region_count) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) return nullptr;

    // iOS polls LCDUI/Canvas immediately before presentation. Re-pumping here
    // would execute Java callbacks twice per host display tick, increase
    // wakeups, and make zero-copy presentation more expensive than the old
    // copy path. Only acquire the completed framebuffer produced by that poll.
    instance->release_current_frame_rgba();
    const auto frame = instance->acquire_current_frame_rgba_since(
        previous_generation);
    if (!frame.has_value()) return nullptr;
    if (width != nullptr) width[0] = frame->metadata.dimensions.width;
    if (height != nullptr) height[0] = frame->metadata.dimensions.height;
    if (generation != nullptr) generation[0] = frame->metadata.generation;
    const std::size_t total_regions = frame->damage_regions.size();
    const int32_t reported_region_count = total_regions >
            static_cast<std::size_t>(std::numeric_limits<int32_t>::max())
        ? std::numeric_limits<int32_t>::max()
        : static_cast<int32_t>(total_regions);
    if (region_count != nullptr) region_count[0] = reported_region_count;
    if (regions != nullptr && region_capacity > 0) {
        const std::size_t writable = std::min<std::size_t>(
            total_regions,
            static_cast<std::size_t>(region_capacity));
        for (std::size_t index = 0U; index < writable; ++index) {
            const auto& source = frame->damage_regions[index];
            regions[index] = PhoneMEFrameDamageRegion {
                .x = source.x,
                .y = source.y,
                .width = source.width,
                .height = source.height,
            };
        }
    }
    return frame->pixels;
}

const uint8_t* phoneme_acquire_current_frame_native_regions_since(
    PhoneMERuntimeRef runtime,
    uint64_t previous_generation,
    int32_t* width,
    int32_t* height,
    uint64_t* generation,
    int32_t* pixel_format,
    PhoneMEFrameDamageRegion* regions,
    int32_t region_capacity,
    int32_t* region_count) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr) return nullptr;

    instance->release_current_frame_rgba();
    const auto frame = instance->acquire_current_frame_native_since(
        previous_generation);
    if (!frame.has_value()) return nullptr;
    if (width != nullptr) width[0] = frame->metadata.dimensions.width;
    if (height != nullptr) height[0] = frame->metadata.dimensions.height;
    if (generation != nullptr) generation[0] = frame->metadata.generation;
    if (pixel_format != nullptr) {
        pixel_format[0] = frame->metadata.pixel_format ==
                phoneme::runtime::FramePixelFormat::bgra8
            ? static_cast<int32_t>(PHONEME_FRAME_PIXEL_BGRA8)
            : static_cast<int32_t>(PHONEME_FRAME_PIXEL_RGBA8);
    }
    const std::size_t total_regions = frame->damage_regions.size();
    const int32_t reported_region_count = total_regions >
            static_cast<std::size_t>(std::numeric_limits<int32_t>::max())
        ? std::numeric_limits<int32_t>::max()
        : static_cast<int32_t>(total_regions);
    if (region_count != nullptr) region_count[0] = reported_region_count;
    if (regions != nullptr && region_capacity > 0) {
        const std::size_t writable = std::min<std::size_t>(
            total_regions,
            static_cast<std::size_t>(region_capacity));
        for (std::size_t index = 0U; index < writable; ++index) {
            const auto& source = frame->damage_regions[index];
            regions[index] = PhoneMEFrameDamageRegion {
                .x = source.x,
                .y = source.y,
                .width = source.width,
                .height = source.height,
            };
        }
    }
    return frame->pixels;
}

void phoneme_release_frame_rgba(PhoneMERuntimeRef runtime) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->release_current_frame_rgba();
    }
}

uint64_t phoneme_storage_generation(PhoneMERuntimeRef runtime) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        return instance->storage_generation();
    }
    return 0;
}

int32_t phoneme_poll_lcdui_event(PhoneMERuntimeRef runtime,
                                 PhoneMELCDUIEvent* event_out) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || event_out == nullptr) {
        return 0;
    }
    auto event = instance->poll_ui_event();
    if (!event) {
        return 0;
    }

    *event_out = {};
    event_out->kind = event->kind;
    event_out->component_id = event->component_id;
    event_out->parent_id = event->parent_id;
    event_out->component_type = event->component_type;
    event_out->index = event->index;
    event_out->arg0 = event->arguments[0];
    event_out->arg1 = event->arguments[1];
    event_out->arg2 = event->arguments[2];
    event_out->arg3 = event->arguments[3];
    event_out->value64 = event->value64;
    event_out->generation = event->generation;
    copy_utf8(event_out->text, sizeof(event_out->text), event->text);
    copy_utf8(event_out->detail, sizeof(event_out->detail), event->detail);
    return 1;
}

void phoneme_lcdui_select_command(PhoneMERuntimeRef runtime,
                                  int32_t command_id) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->ui_select_command(command_id);
    }
}

void phoneme_lcdui_select_list_item_command(PhoneMERuntimeRef runtime,
                                            int32_t component_id,
                                            int32_t element_index,
                                            int32_t command_id) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->ui_select_list_item_command(
            component_id, element_index, command_id);
    }
}

void phoneme_lcdui_focus_item(PhoneMERuntimeRef runtime,
                              int32_t component_id) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->ui_focus_item(component_id);
    }
}

void phoneme_lcdui_activate_item(PhoneMERuntimeRef runtime,
                                 int32_t component_id) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->ui_activate_item(component_id);
    }
}

void phoneme_lcdui_set_text(PhoneMERuntimeRef runtime,
                            int32_t component_id,
                            const char* utf8_text,
                            int32_t caret_position) {
    if (Runtime* instance = cast_runtime(runtime);
        instance != nullptr && utf8_text != nullptr) {
        instance->ui_set_text(component_id, utf8_text, caret_position);
    }
}

void phoneme_lcdui_set_choice(PhoneMERuntimeRef runtime,
                              int32_t component_id,
                              int32_t element_index,
                              int32_t selected) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->ui_set_choice(component_id, element_index, selected != 0);
    }
}

void phoneme_lcdui_set_gauge(PhoneMERuntimeRef runtime,
                             int32_t component_id,
                             int32_t value) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->ui_set_gauge(component_id, value);
    }
}

void phoneme_lcdui_set_date(PhoneMERuntimeRef runtime,
                            int32_t component_id,
                            int64_t unix_seconds) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->ui_set_date(component_id, unix_seconds);
    }
}

void phoneme_lcdui_set_scroll_position(PhoneMERuntimeRef runtime,
                                       int32_t position) {
    if (Runtime* instance = cast_runtime(runtime); instance != nullptr) {
        instance->ui_set_scroll_position(position);
    }
}

int32_t phoneme_copy_lcdui_image_rgba(PhoneMERuntimeRef runtime,
                                      int32_t component_id,
                                      uint8_t* destination,
                                      int32_t capacity,
                                      int32_t* width,
                                      int32_t* height,
                                      uint64_t* generation) {
    Runtime* instance = cast_runtime(runtime);
    if (instance == nullptr || component_id == 0) {
        return frame_byte_count({}, width, height, generation);
    }

    const std::span<uint8_t> storage =
        destination != nullptr && capacity > 0
            ? std::span<uint8_t>(destination,
                                 static_cast<std::size_t>(capacity))
            : std::span<uint8_t> {};
    return frame_byte_count(
        instance->copy_lcdui_image_rgba(component_id, storage),
        width,
        height,
        generation);
}

int phoneme_inflate_raw(const uint8_t* source,
                        size_t source_length,
                        uint8_t* destination,
                        size_t destination_length) {
    if ((source == nullptr && source_length != 0) ||
        (destination == nullptr && destination_length != 0)) {
        return -1;
    }
    auto output = phoneme::archive::inflate_raw(
        std::span<const std::uint8_t>(source, source_length),
        destination_length);
    if (!output) {
        return -2;
    }
    if (!output->empty()) {
        std::memcpy(destination, output->data(), output->size());
    }
    return 0;
}

int phoneme_inflate_zlib(const uint8_t* source,
                         size_t source_length,
                         uint8_t* destination,
                         size_t destination_length) {
    if ((source == nullptr && source_length != 0) ||
        (destination == nullptr && destination_length != 0)) {
        return -1;
    }
    auto output = phoneme::archive::inflate_zlib(
        std::span<const std::uint8_t>(source, source_length),
        destination_length);
    if (!output) {
        return -2;
    }
    if (!output->empty()) {
        std::memcpy(destination, output->data(), output->size());
    }
    return 0;
}

uint32_t phoneme_crc32(const uint8_t* data, size_t length) {
    if (data == nullptr && length != 0) {
        return 0;
    }
    return phoneme::archive::crc32(
        std::span<const std::uint8_t>(data, length));
}

} // extern "C"
