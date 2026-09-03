#ifndef PHONEME_CORE_H
#define PHONEME_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* PhoneMERuntimeRef;

#define PHONEME_C_API_VERSION_MAJOR 1u
#define PHONEME_C_API_VERSION_MINOR 3u
#define PHONEME_C_API_VERSION_PATCH 0u
#define PHONEME_C_API_VERSION \
    ((PHONEME_C_API_VERSION_MAJOR << 16u) | \
     (PHONEME_C_API_VERSION_MINOR << 8u) | \
     PHONEME_C_API_VERSION_PATCH)

/* Returns PHONEME_C_API_VERSION. New ABI surface is added without changing
 * existing declarations; incompatible changes require a major version bump. */
uint32_t phoneme_c_api_version(void);

typedef struct {
    int32_t attempted;
    int32_t succeeded;
    int32_t failed;
    int32_t skipped;
} PhoneMEPreverifyResult;

int32_t phoneme_preverify_jar_classes(
    const char* runtime_classes_path,
    const char* jar_path,
    const char* class_list_path,
    const char* output_directory,
    PhoneMEPreverifyResult* result_out
);

#define PHONEME_LCDUI_TEXT_CAPACITY 768

typedef enum {
    PHONEME_LCDUI_EVENT_NONE = 0,
    PHONEME_LCDUI_EVENT_RESET = 1,
    PHONEME_LCDUI_EVENT_SCREEN_CREATED = 2,
    PHONEME_LCDUI_EVENT_SCREEN_UPDATED = 3,
    PHONEME_LCDUI_EVENT_SCREEN_SHOWN = 4,
    PHONEME_LCDUI_EVENT_SCREEN_HIDDEN = 5,
    PHONEME_LCDUI_EVENT_SCREEN_DELETED = 6,
    PHONEME_LCDUI_EVENT_ITEM_CREATED = 7,
    PHONEME_LCDUI_EVENT_ITEM_UPDATED = 8,
    PHONEME_LCDUI_EVENT_ITEM_SHOWN = 9,
    PHONEME_LCDUI_EVENT_ITEM_HIDDEN = 10,
    PHONEME_LCDUI_EVENT_ITEM_DELETED = 11,
    PHONEME_LCDUI_EVENT_CHOICE_ELEMENT = 12,
    PHONEME_LCDUI_EVENT_CHOICE_DELETED = 13,
    PHONEME_LCDUI_EVENT_COMMANDS_RESET = 14,
    PHONEME_LCDUI_EVENT_COMMAND = 15,
    PHONEME_LCDUI_EVENT_ITEM_FOCUSED = 16
} PhoneMELCDUIEventKind;

typedef struct {
    int32_t kind;
    int32_t component_id;
    int32_t parent_id;
    int32_t component_type;
    int32_t index;
    int32_t arg0;
    int32_t arg1;
    int32_t arg2;
    int32_t arg3;
    int64_t value64;
    uint64_t generation;
    char text[PHONEME_LCDUI_TEXT_CAPACITY];
    char detail[PHONEME_LCDUI_TEXT_CAPACITY];
} PhoneMELCDUIEvent;

enum {
    PHONEME_OK = 0,
    PHONEME_ERROR_INVALID_ARGUMENT = -1,
    PHONEME_ERROR_ALREADY_RUNNING = -2,
    PHONEME_ERROR_NOT_CONFIGURED = -3,
    PHONEME_ERROR_THREAD_CREATE = -4,
    PHONEME_ERROR_NOT_RUNNING = -5,
    PHONEME_ERROR_SYSTEM_START = -6,
    PHONEME_ERROR_INSTALL = -7,
    PHONEME_ERROR_UNSUPPORTED = -8,
    PHONEME_ERROR_IO = -9,
    PHONEME_ERROR_MALFORMED_INPUT = -10
};

typedef enum {
    PHONEME_APP_STATE_NONE = 0,
    PHONEME_APP_STATE_ACTIVE = 1,
    PHONEME_APP_STATE_PAUSED = 2,
    PHONEME_APP_STATE_DESTROYED = 3,
    PHONEME_APP_STATE_ERROR = 4
} PhoneMEAppState;

typedef enum {
    PHONEME_THERMAL_NOMINAL = 0,
    PHONEME_THERMAL_FAIR = 1,
    PHONEME_THERMAL_SERIOUS = 2,
    PHONEME_THERMAL_CRITICAL = 3
} PhoneMEThermalPressure;

typedef enum {
    PHONEME_PUSH_FOREGROUND_ONLY = 0,
    PHONEME_PUSH_SYSTEM_MANAGED = 1
} PhoneMEPushBackgroundPolicy;

typedef enum {
    PHONEME_PUSH_REQUEST_CONNECTION = 1,
    PHONEME_PUSH_REQUEST_ALARM = 2
} PhoneMEPushRequestKind;

#define PHONEME_PUSH_TARGET_CAPACITY 2049
#define PHONEME_PUSH_MIDLET_CAPACITY 513

typedef struct {
    uint64_t request_id;
    int32_t kind;
    int64_t created_at_millis;
    char target[PHONEME_PUSH_TARGET_CAPACITY];
    char midlet[PHONEME_PUSH_MIDLET_CAPACITY];
} PhoneMEPushLaunchRequest;

typedef enum {
    PHONEME_PERMISSION_DOMAIN_UNKNOWN = 0,
    PHONEME_PERMISSION_DOMAIN_NETWORK = 1,
    PHONEME_PERMISSION_DOMAIN_FILESYSTEM = 2,
    PHONEME_PERMISSION_DOMAIN_MEDIA = 3
} PhoneMEPermissionDomain;

typedef enum {
    PHONEME_SUITE_UNTRUSTED = 0,
    PHONEME_SUITE_TRUSTED = 1
} PhoneMESuiteTrust;

typedef enum {
    PHONEME_PERMISSION_UNKNOWN = -1,
    PHONEME_PERMISSION_DENIED = 0,
    PHONEME_PERMISSION_ALLOWED = 1
} PhoneMEPermissionDecision;

typedef enum {
    PHONEME_PERMISSION_ONESHOT = 0,
    PHONEME_PERMISSION_SESSION = 1,
    PHONEME_PERMISSION_BLANKET = 2
} PhoneMEPermissionScope;

typedef struct {
    int32_t suite_id;
    int32_t trust;
    int32_t domain;
    int32_t user_initiated;
    const char* permission;
    const char* resource;
} PhoneMEPermissionRequest;

typedef struct {
    int32_t decision;
    int32_t scope;
} PhoneMEPermissionResponse;

typedef enum {
    PHONEME_FRAME_PACING_NATIVE = 0,
    PHONEME_FRAME_PACING_CAP = 1,
    PHONEME_FRAME_PACING_OVERRIDE_GAME_LOOP = 2,
} PhoneMEFramePacingMode;

typedef enum {
    PHONEME_TRANSLATION_PROVIDER_GOOGLE = 0,
    PHONEME_TRANSLATION_PROVIDER_BING = 1,
    PHONEME_TRANSLATION_PROVIDER_AUTOMATIC = 2,
} PhoneMETranslationProvider;

typedef enum {
    PHONEME_JIT_UNAVAILABLE = 0,
    PHONEME_JIT_READY = 1,
} PhoneMEJITStatus;

/* Invoked synchronously on the requesting runtime thread. Do not re-enter
 * the same runtime from this callback. String pointers are valid only for
 * the duration of the callback. */
typedef PhoneMEPermissionResponse (*PhoneMEPermissionPromptCallback)(
    void* context,
    const PhoneMEPermissionRequest* request);

/* Invoked asynchronously when Core has host-visible work (new framebuffer,
 * pending Canvas repaint or LCDUI event). The callback may run on any VM
 * worker thread and must only wake/schedule the host UI loop; do not re-enter
 * the same runtime from this callback. Passing NULL clears the callback. */
typedef void (*PhoneMEHostWakeCallback)(void* context);

PhoneMERuntimeRef phoneme_create(void);
void phoneme_destroy(PhoneMERuntimeRef runtime);
/* optional_class_archive may be NULL. The standalone Core provides its
 * boot classes directly in C++. */
int32_t phoneme_configure(PhoneMERuntimeRef runtime,
                          const char* runtime_home,
                          const char* optional_class_archive);
void phoneme_set_host_wake_callback(PhoneMERuntimeRef runtime,
                                    PhoneMEHostWakeCallback callback,
                                    void* context);
int32_t phoneme_configure_keymap(PhoneMERuntimeRef runtime,
                                 int32_t up,
                                 int32_t down,
                                 int32_t left,
                                 int32_t right,
                                 int32_t fire,
                                 int32_t soft1,
                                 int32_t soft2);
/* Configures one MIDlet's frame pacing. This may be called before or after the
 * MIDlet is started. CAP never shortens Java sleeps; OVERRIDE_GAME_LOOP is an
 * explicit compatibility mode for stable synchronous Canvas/GameCanvas loops.
 * Java clocks, Timer, Object.wait and unrelated Thread.sleep remain wall-time
 * based in every mode. */
int32_t phoneme_configure_app_frame_pacing(
    PhoneMERuntimeRef runtime,
    int32_t app_id,
    int32_t frames_per_second,
    int32_t pacing_mode);
/* Configures the maximum Java heap for one MIDlet before it starts. The limit
 * is allocated on demand. Changes to a running MIDlet require a restart. */
int32_t phoneme_configure_app_heap(PhoneMERuntimeRef runtime,
                                   int32_t app_id,
                                   int32_t heap_megabytes);
/* Enables the ARM64 optimizing JIT. Hot integer/control-flow methods use
 * constant propagation, folded branches, strength reduction and block-level
 * budget guards. Unsupported methods or hosts without JIT permission
 * automatically use the interpreter. */
int32_t phoneme_configure_jit(PhoneMERuntimeRef runtime, int32_t enabled);
/* Updates host thermal pressure for the shared native CPU budget. This never
 * slows Java execution directly; it first reduces native helper/JIT work. */
int32_t phoneme_set_thermal_pressure(PhoneMERuntimeRef runtime,
                                     int32_t pressure);
/* Probes whether executable JIT memory is available to the current process. */
int32_t phoneme_jit_status(void);
/* Selects the translation service for subsequently created MIDlets. Existing
 * MIDlets keep the setting they had at launch. Language pointers may be NULL
 * to use auto detection and Vietnamese output. */
int32_t phoneme_configure_translation(PhoneMERuntimeRef runtime,
                                      int32_t enabled,
                                      const char* source_language,
                                      const char* target_language);
/* Provider-aware variant. Google and Bing keep their existing ABI values.
 * AUTOMATIC adds throttling, circuit breaking and provider fallback. */
int32_t phoneme_configure_translation_v2(PhoneMERuntimeRef runtime,
                                         int32_t enabled,
                                         int32_t provider,
                                         const char* source_language,
                                         const char* target_language);
/* Changes translation only for one already-running MIDlet. */
int32_t phoneme_configure_app_translation(PhoneMERuntimeRef runtime,
                                          int32_t app_id,
                                          int32_t enabled,
                                          const char* source_language,
                                          const char* target_language);
int32_t phoneme_configure_app_translation_v2(
    PhoneMERuntimeRef runtime,
    int32_t app_id,
    int32_t enabled,
    int32_t provider,
    const char* source_language,
    const char* target_language);
/* Configure before phoneme_start_system. Passing NULL clears the callback. */
int32_t phoneme_configure_permission_prompt(
    PhoneMERuntimeRef runtime,
    PhoneMEPermissionPromptCallback callback,
    void* context);
/* Call after installation and before starting a MIDlet from this suite.
 * Trust may be assigned while unrelated MIDlets are running. */
int32_t phoneme_set_suite_trust(PhoneMERuntimeRef runtime,
                                int32_t suite_id,
                                int32_t trust);
int32_t phoneme_install_jar(PhoneMERuntimeRef runtime,
                            const char* jar_path,
                            int32_t* suite_id_out);
/* Explicit host replacement keeps the existing suite ID when the same MIDP
 * vendor/name is reimported, including changed JAR bytes with an unchanged
 * MIDlet-Version. Downgrades remain rejected. RMS/files/permissions stay
 * attached to the stable suite ID. */
int32_t phoneme_install_jar_replacing(PhoneMERuntimeRef runtime,
                                      const char* jar_path,
                                      int32_t* suite_id_out);
/* Host-scoped installation keeps imports with identical MIDP manifest identity
 * separate. Reinstalling the same scope replaces changed same-version bytes
 * while preserving the stable suite ID and its RMS/files. */
int32_t phoneme_install_jar_scoped(PhoneMERuntimeRef runtime,
                                   const char* jar_path,
                                   const char* identity_scope,
                                   int32_t* suite_id_out);
/* Finds a previously managed suite by its MIDP manifest identity without
 * reopening and hashing the source JAR. Returns PHONEME_OK with suite_id_out
 * set to 0 when no exact vendor/name/version match exists. An empty version
 * matches any installed version for that identity. */
int32_t phoneme_find_installed_suite(PhoneMERuntimeRef runtime,
                                     const char* vendor,
                                     const char* name,
                                     const char* version,
                                     int32_t* suite_id_out);
int32_t phoneme_find_installed_suite_scoped(
    PhoneMERuntimeRef runtime,
    const char* vendor,
    const char* name,
    const char* version,
    const char* identity_scope,
    int32_t* suite_id_out);
/* Removes the installed suite. When remove_data is nonzero, RMS, FileConnection,
 * permissions, PushRegistry and temporary suite state are removed as well. */
int32_t phoneme_uninstall_suite(PhoneMERuntimeRef runtime,
                                int32_t suite_id,
                                int32_t remove_data);
int32_t phoneme_last_install_stage(void);
int32_t phoneme_last_suite_store_stage(void);
int32_t phoneme_start_system(PhoneMERuntimeRef runtime);
int32_t phoneme_start_midlet(PhoneMERuntimeRef runtime,
                             int32_t suite_id,
                             const char* main_class,
                             int32_t app_id,
                             int32_t screen_width,
                             int32_t screen_height);
int32_t phoneme_set_foreground(PhoneMERuntimeRef runtime,
                               int32_t app_id,
                               int32_t screen_width,
                               int32_t screen_height);
int32_t phoneme_pause_midlet(PhoneMERuntimeRef runtime, int32_t app_id);
int32_t phoneme_resume_midlet(PhoneMERuntimeRef runtime, int32_t app_id);
int32_t phoneme_destroy_midlet(PhoneMERuntimeRef runtime, int32_t app_id);
int32_t phoneme_push_set_background_policy(PhoneMERuntimeRef runtime,
                                           int32_t suite_id,
                                           int32_t policy);
int32_t phoneme_push_notify_connection_available(
    PhoneMERuntimeRef runtime,
    int32_t suite_id,
    const char* connection,
    int64_t received_at_millis);
/* Source-aware variant for socket/datagram registrations. The source address
 * is matched against the MIDP PushRegistry filter before a launch is queued. */
int32_t phoneme_push_notify_connection_available_from_source(
    PhoneMERuntimeRef runtime,
    int32_t suite_id,
    const char* connection,
    const char* source_address,
    int64_t received_at_millis);
/* Returns the number of eligible requests copied. With destination == NULL
 * and capacity == 0, returns the number currently eligible without dequeuing.
 * Requests remain queued until phoneme_push_acknowledge_launch_request. */
int32_t phoneme_push_poll_launch_requests(
    PhoneMERuntimeRef runtime,
    int32_t suite_id,
    int64_t now_millis,
    int32_t background_execution_granted,
    PhoneMEPushLaunchRequest* destination,
    int32_t capacity);
int32_t phoneme_push_acknowledge_launch_request(
    PhoneMERuntimeRef runtime,
    int32_t suite_id,
    uint64_t request_id);
int32_t phoneme_midlet_state(PhoneMERuntimeRef runtime, int32_t app_id);
int32_t phoneme_foreground_app_id(PhoneMERuntimeRef runtime);
int64_t phoneme_midlet_used_memory(PhoneMERuntimeRef runtime,
                                   int32_t app_id,
                                   int32_t timeout_milliseconds);
int32_t phoneme_start_jar(PhoneMERuntimeRef runtime,
                          const char* jar_path,
                          const char* main_class,
                          int32_t screen_width,
                          int32_t screen_height);
void phoneme_stop(PhoneMERuntimeRef runtime);
void phoneme_suspend(PhoneMERuntimeRef runtime);
void phoneme_resume(PhoneMERuntimeRef runtime);
/* Persists deferred RMS (record store) writes for every live application.
 * Call before the host may kill the process (backgrounding, memory warning). */
void phoneme_flush_rms(PhoneMERuntimeRef runtime);
int32_t phoneme_is_running(PhoneMERuntimeRef runtime);
int32_t phoneme_is_suspended(PhoneMERuntimeRef runtime);
int32_t phoneme_last_exit_code(PhoneMERuntimeRef runtime);
/* Returns the full UTF-8 byte count excluding the trailing NUL. Pass NULL and
 * capacity 0 to query the required size before copying. */
int32_t phoneme_copy_last_error_message(PhoneMERuntimeRef runtime,
                                        char* destination,
                                        int32_t capacity);
int32_t phoneme_copy_midlet_error_message(PhoneMERuntimeRef runtime,
                                          int32_t app_id,
                                          char* destination,
                                          int32_t capacity);

void phoneme_ios_media_set_application_metadata(const char* title,
                                                const char* artist,
                                                const char* artwork_path);
int32_t phoneme_ios_media_has_active_playback(void);

void phoneme_send_key(PhoneMERuntimeRef runtime,
                      int32_t key_code,
                      int32_t pressed);
void phoneme_send_pointer(PhoneMERuntimeRef runtime,
                          int32_t x,
                          int32_t y,
                          int32_t action);
/* Pump foreground VM events without copying a Canvas frame. Native LCDUI
 * hosts use this once per poll tick so timers and serial callbacks continue
 * while Form/List/Alert screens are visible. */
void phoneme_pump_events(PhoneMERuntimeRef runtime);
/* Query with destination == NULL to pump the foreground Canvas and obtain
 * dimensions/generation without copying pixels. Call again with storage to
 * copy that latest complete framebuffer without a second VM pump. */
int32_t phoneme_copy_frame_rgba(PhoneMERuntimeRef runtime,
                                uint8_t* destination,
                                int32_t capacity,
                                int32_t* width,
                                int32_t* height,
                                uint64_t* generation);
/* Web/display fast path: pump once and copy only when generation changed.
 * Returns 0 when unchanged, otherwise the required RGBA byte count. If the
 * supplied capacity is smaller than that count, metadata is still returned
 * and the caller can grow its persistent staging buffer and retry. */
int32_t phoneme_copy_frame_rgba_since(PhoneMERuntimeRef runtime,
                                      uint64_t previous_generation,
                                      uint8_t* destination,
                                      int32_t capacity,
                                      int32_t* width,
                                      int32_t* height,
                                      uint64_t* generation);
typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} PhoneMEFrameDamageRegion;
typedef enum {
    PHONEME_FRAME_PIXEL_RGBA8 = 0,
    PHONEME_FRAME_PIXEL_BGRA8 = 1,
} PhoneMEFramePixelFormat;
/* Zero-copy display path. The returned pointer remains valid until
 * phoneme_release_frame_rgba() and must not be retained after that call. */
const uint8_t* phoneme_acquire_frame_rgba_since(PhoneMERuntimeRef runtime,
                                                uint64_t previous_generation,
                                                int32_t* width,
                                                int32_t* height,
                                                uint64_t* generation);
/* Native host fast path for callers that already pumped the runtime during
 * the same display tick. Unlike phoneme_acquire_frame_rgba_since(), this only
 * acquires the latest completed framebuffer and never executes Canvas work. */
const uint8_t* phoneme_acquire_current_frame_rgba_since(
    PhoneMERuntimeRef runtime,
    uint64_t previous_generation,
    int32_t* width,
    int32_t* height,
    uint64_t* generation);
/* Same no-pump native fast path, with the damage regions that produced this
 * framebuffer generation. region_count receives the total region count even
 * when region_capacity is smaller, allowing the host to fall back to a full
 * upload when damage is unusually fragmented. */
const uint8_t* phoneme_acquire_current_frame_rgba_regions_since(
    PhoneMERuntimeRef runtime,
    uint64_t previous_generation,
    int32_t* width,
    int32_t* height,
    uint64_t* generation,
    PhoneMEFrameDamageRegion* regions,
    int32_t region_capacity,
    int32_t* region_count);
/* Native-format zero-copy display path. Unlike the RGBA API this exposes the
 * framebuffer's actual packed format and reports it through pixel_format.
 * iOS uses BGRA8 so Metal can upload Java ARGB Pixel rows without a channel
 * shuffle. The lease is released with phoneme_release_frame_rgba(). */
const uint8_t* phoneme_acquire_current_frame_native_regions_since(
    PhoneMERuntimeRef runtime,
    uint64_t previous_generation,
    int32_t* width,
    int32_t* height,
    uint64_t* generation,
    int32_t* pixel_format,
    PhoneMEFrameDamageRegion* regions,
    int32_t region_capacity,
    int32_t* region_count);
void phoneme_release_frame_rgba(PhoneMERuntimeRef runtime);
/* Monotonic generation for persistent FileConnection/RMS mutations of the
 * foreground MIDlet. Used by web hosts to skip redundant IDBFS syncs. */
uint64_t phoneme_storage_generation(PhoneMERuntimeRef runtime);
int32_t phoneme_poll_lcdui_event(PhoneMERuntimeRef runtime,
                                 PhoneMELCDUIEvent* event_out);
void phoneme_lcdui_select_command(PhoneMERuntimeRef runtime,
                                  int32_t command_id);
void phoneme_lcdui_select_list_item_command(PhoneMERuntimeRef runtime,
                                            int32_t component_id,
                                            int32_t element_index,
                                            int32_t command_id);
void phoneme_lcdui_focus_item(PhoneMERuntimeRef runtime,
                              int32_t component_id);
void phoneme_lcdui_activate_item(PhoneMERuntimeRef runtime,
                                 int32_t component_id);
void phoneme_lcdui_set_text(PhoneMERuntimeRef runtime,
                            int32_t component_id,
                            const char* utf8_text,
                            int32_t caret_position);
void phoneme_lcdui_set_choice(PhoneMERuntimeRef runtime,
                              int32_t component_id,
                              int32_t element_index,
                              int32_t selected);
void phoneme_lcdui_set_gauge(PhoneMERuntimeRef runtime,
                             int32_t component_id,
                             int32_t value);
void phoneme_lcdui_set_date(PhoneMERuntimeRef runtime,
                            int32_t component_id,
                            int64_t unix_seconds);
void phoneme_lcdui_set_scroll_position(PhoneMERuntimeRef runtime,
                                       int32_t position);
int32_t phoneme_copy_lcdui_image_rgba(PhoneMERuntimeRef runtime,
                                      int32_t component_id,
                                      uint8_t* destination,
                                      int32_t capacity,
                                      int32_t* width,
                                      int32_t* height,
                                      uint64_t* generation);

#ifdef __cplusplus
}
#endif

#endif
