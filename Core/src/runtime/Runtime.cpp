#include "phoneme/runtime/Runtime.hpp"
#include "phoneme/runtime/ParallelExecutor.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "phoneme/graphics/Color.hpp"
#include "phoneme/graphics/Graphics.hpp"
#include "phoneme/graphics/Image.hpp"
#include "phoneme/runtime/CanvasRuntime.hpp"
#include "phoneme/vm/ClassRepository.hpp"
#include "phoneme/vm/LcduiBridge.hpp"
#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/PerformanceCounters.hpp"

#if defined(__APPLE__)
#include <os/log.h>
#include <TargetConditionals.h>
#endif

namespace phoneme::runtime {

struct UiTranslationReplayState final {
    std::mutex mutex;
    ConcurrentQueue<UiEvent>* queue {nullptr};
    std::function<void()> wake;
    bool active {true};
    u64 reset_epoch {0U};
    u64 commands_reset_generation {0U};
    std::unordered_map<i32, u64> deleted_components;
    std::unordered_map<u64, u64> deleted_choices;

    [[nodiscard]] static u64 choice_key(i32 component_id, i32 index) noexcept {
        return (static_cast<u64>(static_cast<u32>(component_id)) << 32U) |
               static_cast<u32>(index);
    }

    [[nodiscard]] u64 current_epoch() {
        std::scoped_lock lock(mutex);
        return reset_epoch;
    }

    void publish(UiEvent event, bool replay, u64 expected_epoch = 0U) {
        std::function<void()> wake_sink;
        {
            std::scoped_lock lock(mutex);
            if (!active || queue == nullptr) return;
            if (replay) {
                if (expected_epoch != reset_epoch) return;
                if (const auto deleted = deleted_components.find(event.component_id);
                    deleted != deleted_components.end() &&
                    event.generation < deleted->second) {
                    return;
                }
                if (event.kind == 12) {
                    const auto exact = deleted_choices.find(
                        choice_key(event.component_id, event.index));
                    const auto all = deleted_choices.find(
                        choice_key(event.component_id, -1));
                    if ((exact != deleted_choices.end() &&
                         event.generation < exact->second) ||
                        (all != deleted_choices.end() &&
                         event.generation < all->second)) {
                        return;
                    }
                }
                if (event.kind == 15 &&
                    event.generation < commands_reset_generation) {
                    return;
                }
            } else {
                if (event.kind == 1) {
                    ++reset_epoch;
                    deleted_components.clear();
                    deleted_choices.clear();
                } else if (event.kind == 6 || event.kind == 11) {
                    deleted_components[event.component_id] = event.generation;
                } else if (event.kind == 13) {
                    deleted_choices[choice_key(event.component_id, event.index)] =
                        event.generation;
                } else if (event.kind == 14) {
                    commands_reset_generation = std::max(
                        commands_reset_generation, event.generation);
                }
            }
            queue->push(std::move(event));
            wake_sink = wake;
        }
        if (wake_sink) wake_sink();
    }

    void deactivate() noexcept {
        std::scoped_lock lock(mutex);
        active = false;
        queue = nullptr;
        wake = {};
        deleted_components.clear();
        deleted_choices.clear();
    }
};

[[nodiscard]] std::string format_error_message(const Error& error) {
    std::string message;
    if (!error.java_exception_class.empty()) {
        message = error.java_exception_class;
        std::replace(message.begin(), message.end(), '/', '.');
        if (!error.message.empty()) {
            message += ": ";
            message += error.message;
        }
    } else {
        message = error.message;
    }
    if (message.empty()) {
        message = "Unknown phoneME runtime error";
    }
    return message;
}

[[nodiscard]] std::vector<vm::NativeMethodInvocationCount>
nonzero_native_invocation_counts(vm::Machine& machine) {
    auto counts = machine.natives().invocation_counts();
    std::erase_if(counts, [](const vm::NativeMethodInvocationCount& entry) {
        return entry.count == 0U;
    });
    return counts;
}

struct AsyncLifecycleState final {
    std::atomic<bool> completed {false};
    std::mutex mutex;
    std::optional<Error> failure;
    vm::MidletSignal signal {vm::MidletSignal::none};
};

class ApplicationVM final {
public:
    ApplicationVM(
        Dimensions dimensions,
        std::array<i32, 7> keymap,
        AppFramePacingConfig frame_pacing,
        AppHeapConfig heap_config,
        bool jit_enabled,
        Framebuffer& framebuffer,
        std::function<void()> host_wake,
        std::shared_ptr<translation::TranslationService> translation_service);
    ~ApplicationVM();

    std::recursive_mutex operation_mutex;
    vm::ClassRepository classes;
    vm::Machine machine;
    CanvasRuntime canvas;
    vm::NativeRootScope paint_graphics;
    u64 paint_graphics_generation {0U};
    bool paint_graphics_published {false};
    std::vector<u8> frame_rgba_scratch;
    vm::ObjectRef midlet;
    std::atomic<u64> ui_event_count {0U};
    std::mutex lifecycle_state_mutex;
    std::shared_ptr<AsyncLifecycleState> deferred_start_app;
};

namespace {

constexpr usize kImageWidthField = 0;
constexpr usize kImageHeightField = 1;
constexpr usize kImageMutableField = 2;
constexpr usize kGraphicsTargetField = 0;
constexpr usize kImageItemImageField = 11;
constexpr usize kImageItemGenerationField = 14;
constexpr usize kCustomItemPaintImageField = 11;
constexpr usize kCustomItemPaintGenerationField = 12;
constexpr usize kAlertImageField = 10;
constexpr usize kAlertImageGenerationField = 14;
constexpr usize kChoiceGroupImagesField = 13;
constexpr usize kListImagesField = 12;
constexpr u32 kChoiceImageIndexBits = 8U;
constexpr i64 kChoiceImageIndexMask =
    (1LL << kChoiceImageIndexBits) - 1LL;

[[nodiscard]] i32 replay_event_kind(i32 kind) noexcept;

struct PendingUiTranslation final {
    PendingUiTranslation(
        UiEvent initial_event,
        std::weak_ptr<UiTranslationReplayState> replay_target,
        u64 initial_epoch)
        : event(std::move(initial_event)),
          replay(std::move(replay_target)),
          replay_epoch(initial_epoch) {}

    std::mutex mutex;
    UiEvent event;
    std::weak_ptr<UiTranslationReplayState> replay;
    u64 replay_epoch {0U};

    void replace_text(bool detail, std::string translated) {
        UiEvent snapshot;
        {
            std::scoped_lock lock(mutex);
            if (detail) {
                event.detail = std::move(translated);
            } else {
                event.text = std::move(translated);
            }
            snapshot = event;
            snapshot.kind = replay_event_kind(snapshot.kind);
        }
        if (auto target = replay.lock()) {
            target->publish(std::move(snapshot), true, replay_epoch);
        }
    }

    void replace_cached(bool detail, std::string translated) {
        std::scoped_lock lock(mutex);
        if (detail) {
            event.detail = std::move(translated);
        } else {
            event.text = std::move(translated);
        }
    }

    [[nodiscard]] UiEvent snapshot() {
        std::scoped_lock lock(mutex);
        return event;
    }
};

[[nodiscard]] i32 replay_event_kind(i32 kind) noexcept {
    if (kind == 2 || kind == 4) return 3;
    if (kind == 7 || kind == 9) return 8;
    return kind;
}

[[nodiscard]] bool should_translate_ui_detail(const UiEvent& event) noexcept {
    // Screen detail is Alert body or Ticker text. Command detail is the long
    // label. Item detail is translatable only for StringItem and ImageItem;
    // TextField detail is editable user input and DateField detail is a zone ID.
    if (event.kind == 2 || event.kind == 3 || event.kind == 4 ||
        event.kind == 15) {
        return true;
    }
    if (event.kind != 7 && event.kind != 8 && event.kind != 9) return false;
    return (event.component_type >= 8 && event.component_type <= 10) ||
           (event.component_type >= 12 && event.component_type <= 14);
}

void configure_suite_capabilities(vm::Machine&, const Suite&) {
    // MicroEdition-Profile and MicroEdition-Configuration in a suite
    // descriptor are minimum requirements of that suite. They must not
    // replace the capabilities reported by the running phoneME platform via
    // System.getProperty(). The original phoneME runtime keeps reporting its
    // own MIDP/CLDC versions even when a legacy JAR declares older versions.
}

struct LCDUIImageSource final {
    vm::ObjectRef image;
    u64 generation {0};
};

[[nodiscard]] Result<vm::ObjectRef> runtime_reference_field(
    vm::Machine& machine,
    vm::ObjectRef object,
    usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Result<i32> runtime_int_field(vm::Machine& machine,
                                            vm::ObjectRef object,
                                            usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

[[nodiscard]] Result<LCDUIImageSource> resolve_lcdui_image_source(
    vm::Machine& machine,
    i32 component_id) {
    if (component_id == 0) {
        return fail(ErrorCode::invalid_argument,
                    "LCDUI image component ID is zero");
    }

    if (component_id < 0) {
        const i64 packed = -static_cast<i64>(component_id);
        const i64 owner_value = packed >> kChoiceImageIndexBits;
        const i64 index_value = packed & kChoiceImageIndexMask;
        if (owner_value <= 0 ||
            owner_value > static_cast<i64>(std::numeric_limits<i32>::max())) {
            return fail(ErrorCode::out_of_range,
                        "LCDUI choice image owner is outside bounds");
        }
        auto owner = machine.ui_component(static_cast<i32>(owner_value));
        if (!owner) return std::unexpected(owner.error());

        auto is_list = machine.object_is_instance(
            *owner, "javax/microedition/lcdui/List");
        if (!is_list) return std::unexpected(is_list.error());
        auto is_group = machine.object_is_instance(
            *owner, "javax/microedition/lcdui/ChoiceGroup");
        if (!is_group) return std::unexpected(is_group.error());
        if (!*is_list && !*is_group) {
            return fail(ErrorCode::invalid_argument,
                        "LCDUI choice image owner is not a Choice");
        }

        auto images = runtime_reference_field(
            machine,
            *owner,
            *is_list ? kListImagesField : kChoiceGroupImagesField);
        if (!images) return std::unexpected(images.error());
        if (images->is_null()) {
            return fail(ErrorCode::invalid_state,
                        "LCDUI Choice has no image storage");
        }
        auto length = machine.heap().array_length(*images);
        if (!length) return std::unexpected(length.error());
        const usize index = static_cast<usize>(index_value);
        if (index >= *length) {
            return fail(ErrorCode::out_of_range,
                        "LCDUI choice image index is outside bounds");
        }
        auto value = machine.heap().element(*images, index);
        if (!value) return std::unexpected(value.error());
        auto image = value->as_reference();
        if (!image) return std::unexpected(image.error());
        if (image->is_null()) {
            return fail(ErrorCode::invalid_state,
                        "LCDUI choice element has no image");
        }
        return LCDUIImageSource {
            .image = *image,
            .generation = image->bits,
        };
    }

    auto component = machine.ui_component(component_id);
    if (!component) return std::unexpected(component.error());

    auto is_image = machine.object_is_instance(
        *component, "javax/microedition/lcdui/Image");
    if (!is_image) return std::unexpected(is_image.error());
    if (*is_image) {
        return LCDUIImageSource {
            .image = *component,
            .generation = component->bits,
        };
    }

    usize image_field = 0;
    usize generation_field = 0;
    auto is_alert = machine.object_is_instance(
        *component, "javax/microedition/lcdui/Alert");
    if (!is_alert) return std::unexpected(is_alert.error());
    if (*is_alert) {
        image_field = kAlertImageField;
        generation_field = kAlertImageGenerationField;
    } else {
        auto is_custom = machine.object_is_instance(
            *component, "javax/microedition/lcdui/CustomItem");
        if (!is_custom) return std::unexpected(is_custom.error());
        if (*is_custom) {
            image_field = kCustomItemPaintImageField;
            generation_field = kCustomItemPaintGenerationField;
        } else {
            auto is_image_item = machine.object_is_instance(
                *component, "javax/microedition/lcdui/ImageItem");
            if (!is_image_item) return std::unexpected(is_image_item.error());
            if (!*is_image_item) {
                return fail(ErrorCode::invalid_argument,
                            "LCDUI component does not expose an image");
            }
            image_field = kImageItemImageField;
            generation_field = kImageItemGenerationField;
        }
    }

    auto image = runtime_reference_field(machine, *component, image_field);
    if (!image) return std::unexpected(image.error());
    if (image->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "LCDUI component image is null");
    }
    auto generation = runtime_int_field(
        machine, *component, generation_field);
    if (!generation) return std::unexpected(generation.error());
    return LCDUIImageSource {
        .image = *image,
        .generation = static_cast<u64>(std::max(*generation, 0)),
    };
}
// MIDlet constructors and lifecycle callbacks can synchronously parse or
// decrypt multi-megabyte resource tables. A lifetime-wide instruction cap
// produces false positives for that finite work (for example a linear byte
// scan before AES decryption). These values are progress-watchdog windows:
// native/intrinsic/JIT call completions reset the window while a genuinely
// non-progressing Java loop remains bounded. Machine also keeps the watchdog's
// 32x absolute safety ceiling.
constexpr u64 kMidletConstructorInstructionBudget = 200'000'000U;
constexpr u64 kMidletLifecycleInstructionBudget = 100'000'000U;
// Forced teardown is best-effort cleanup, not application work. Keep the
// callback bounded so malformed or intentionally looping destroyApp methods
// cannot hold the emulator exit path for tens of seconds.
constexpr u64 kForcedDestroyInstructionBudget = 5'000'000U;

void trace_forced_destroy(std::string_view phase) noexcept {
    if (std::getenv("PHONEME_TRACE_LIFECYCLE") == nullptr) return;
    std::fprintf(stderr,
                 "[LifecycleTrace] forced destroy %.*s\n",
                 static_cast<int>(phase.size()),
                 phase.data());
    std::fflush(stderr);
}

class ScopedUnpacedExecution final {
public:
    explicit ScopedUnpacedExecution(vm::Scheduler& scheduler) noexcept
        : scheduler_(scheduler) {
        scheduler_.begin_unpaced_execution();
    }

    ~ScopedUnpacedExecution() {
        scheduler_.end_unpaced_execution();
    }

    ScopedUnpacedExecution(const ScopedUnpacedExecution&) = delete;
    ScopedUnpacedExecution& operator=(const ScopedUnpacedExecution&) = delete;

private:
    vm::Scheduler& scheduler_;
};

class ScopedJitStartup final {
public:
    explicit ScopedJitStartup(vm::Machine& machine) noexcept
        : machine_(machine) {
        machine_.configure_jit_startup(true);
    }

    ~ScopedJitStartup() {
        machine_.configure_jit_startup(false);
    }

    ScopedJitStartup(const ScopedJitStartup&) = delete;
    ScopedJitStartup& operator=(const ScopedJitStartup&) = delete;

private:
    vm::Machine& machine_;
};

[[nodiscard]] Status copy_framebuffer_to_image(
    Framebuffer& framebuffer,
    graphics::Image& image) {
    const FrameSnapshot current = framebuffer.snapshot();
    if (current.dimensions.width != image.width() ||
        current.dimensions.height != image.height() ||
        current.rgba.size() != image.pixels().size() * 4U) {
        return {};
    }

    auto pixels = image.mutable_pixels();
    for (usize index = 0; index < pixels.size(); ++index) {
        const usize offset = index * 4U;
        pixels[index] = graphics::argb(current.rgba[offset + 3U],
                                       current.rgba[offset],
                                       current.rgba[offset + 1U],
                                       current.rgba[offset + 2U]);
    }
    return {};
}

[[nodiscard]] Status reset_canvas_graphics(
    vm::Machine& machine,
    Framebuffer& framebuffer,
    vm::ObjectRef graphics_object,
    Dimensions dimensions,
    vm::CanvasRect clip,
    bool display_target,
    bool synchronize_pixels) {
    auto target_value = machine.heap().field(graphics_object,
                                             kGraphicsTargetField);
    if (!target_value) return std::unexpected(target_value.error());
    auto image_object = target_value->as_reference();
    if (!image_object) return std::unexpected(image_object.error());

    auto target = machine.graphics().image(image_object->bits);
    if (!target) return std::unexpected(target.error());
    if ((*target)->width() != dimensions.width ||
        (*target)->height() != dimensions.height) {
        return fail(ErrorCode::invalid_state,
                    "cached Canvas graphics dimensions changed");
    }
    if (synchronize_pixels) {
        auto copied = copy_framebuffer_to_image(framebuffer, **target);
        if (!copied) return copied;
    }

    auto context = machine.graphics().context(graphics_object.bits);
    if (!context) return std::unexpected(context.error());
    **context = graphics::GraphicsContext {
        .target_key = image_object->bits,
        .display_target = display_target,
    };
    return graphics::set_clip(**context,
                              **target,
                              clip.x,
                              clip.y,
                              clip.width,
                              clip.height);
}

[[nodiscard]] Result<vm::NativeRootScope> prepare_canvas_graphics(
    vm::Machine& machine,
    Framebuffer& framebuffer,
    Dimensions dimensions,
    vm::CanvasRect clip,
    bool display_target,
    vm::NativeRootScope* persistent_cache = nullptr,
    bool synchronize_cached_pixels = true) {
    if (persistent_cache != nullptr && persistent_cache->active()) {
        auto cached = persistent_cache->get();
        if (cached) {
            auto reset = reset_canvas_graphics(machine,
                                               framebuffer,
                                               *cached,
                                               dimensions,
                                               clip,
                                               display_target,
                                               synchronize_cached_pixels);
            if (reset) {
                return machine.pin_native_root(*cached);
            }
        }
        (void)persistent_cache->release();
    }

    auto image = graphics::Image::create_mutable(dimensions.width,
                                                  dimensions.height);
    if (!image) return std::unexpected(image.error());

    auto image_root = machine.allocate_pinned_instance(
        "javax/microedition/lcdui/Image");
    if (!image_root) return std::unexpected(image_root.error());
    auto image_object = image_root->get();
    if (!image_object) return std::unexpected(image_object.error());
    auto width_stored = machine.heap().set_field(
        *image_object, kImageWidthField, vm::Value::from_int(dimensions.width));
    auto height_stored = machine.heap().set_field(
        *image_object, kImageHeightField, vm::Value::from_int(dimensions.height));
    auto mutable_stored = machine.heap().set_field(
        *image_object, kImageMutableField, vm::Value::from_int(1));
    if (!width_stored) return std::unexpected(width_stored.error());
    if (!height_stored) return std::unexpected(height_stored.error());
    if (!mutable_stored) return std::unexpected(mutable_stored.error());
    auto image_attached = machine.graphics().attach_image(
        image_object->bits, std::move(*image));
    if (!image_attached) return std::unexpected(image_attached.error());

    auto graphics_root = machine.allocate_pinned_instance(
        "javax/microedition/lcdui/Graphics");
    if (!graphics_root) return std::unexpected(graphics_root.error());
    auto graphics_object = graphics_root->get();
    if (!graphics_object) return std::unexpected(graphics_object.error());
    auto target_stored = machine.heap().set_field(
        *graphics_object,
        kGraphicsTargetField,
        vm::Value::from_reference(*image_object));
    if (!target_stored) return std::unexpected(target_stored.error());
    auto context_attached = machine.graphics().attach_context(
        graphics_object->bits, image_object->bits, display_target);
    if (!context_attached) return std::unexpected(context_attached.error());

    auto reset = reset_canvas_graphics(machine,
                                       framebuffer,
                                       *graphics_object,
                                       dimensions,
                                       clip,
                                       display_target,
                                       true);
    if (!reset) return std::unexpected(reset.error());

    if (persistent_cache != nullptr) {
        auto pinned = machine.pin_native_root(*graphics_object);
        if (!pinned) return std::unexpected(pinned.error());
        *persistent_cache = std::move(*pinned);
    }
    return std::move(*graphics_root);
}

constexpr usize kParallelFrameConversionPixels = 32U * 1024U;

[[nodiscard]] constexpr FramePixelFormat canvas_frame_pixel_format() noexcept {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    return FramePixelFormat::bgra8;
#else
    return FramePixelFormat::rgba8;
#endif
}

void convert_image_region_to_frame_pixels(
    std::span<const graphics::Pixel> source_pixels,
    i32 source_width,
    const graphics::ImageRegion& region,
    std::span<u8> destination,
    FramePixelFormat format) {
    const usize region_width = static_cast<usize>(region.width);
    const usize region_height = static_cast<usize>(region.height);
    const usize pixel_count = region_width * region_height;
    const usize source_stride = static_cast<usize>(source_width);

    // Pixel is Java ARGB in a u32. On the little-endian iOS ARM64 host its
    // in-memory bytes are exactly BGRA, matching Metal's native drawable
    // format. Publish dirty rows with memcpy instead of shuffling four
    // channels for every pixel. Other hosts retain the portable RGBA path.
    if (format == FramePixelFormat::bgra8 &&
        std::endian::native == std::endian::little) {
        const usize row_bytes = region_width * sizeof(graphics::Pixel);
        for (usize row = 0U; row < region_height; ++row) {
            const usize source =
                (static_cast<usize>(region.y) + row) * source_stride +
                static_cast<usize>(region.x);
            std::memcpy(destination.data() + row * row_bytes,
                        source_pixels.data() + source,
                        row_bytes);
        }
        return;
    }

    // Java Canvas pixels are stored numerically as 0xAARRGGBB. On every
    // little-endian Wasm/native host the RGBA byte stream can therefore be
    // produced by swapping only the red/blue byte lanes and issuing one
    // 32-bit store. This is substantially cheaper than four byte stores plus
    // channel helper calls per pixel and is friendly to LLVM's Wasm SIMD
    // vectorizer. Use memcpy for the potentially unaligned byte destination;
    // optimized builds lower the fixed four-byte copy to an unaligned store.
    if (format == FramePixelFormat::rgba8 &&
        std::endian::native == std::endian::little) {
        const auto convert_rgba_rows = [&](usize row_begin, usize row_end) {
            for (usize row = row_begin; row < row_end; ++row) {
                usize source =
                    (static_cast<usize>(region.y) + row) * source_stride +
                    static_cast<usize>(region.x);
                usize output = row * region_width * sizeof(graphics::Pixel);
                for (usize column = 0U; column < region_width; ++column) {
                    const graphics::Pixel pixel = source_pixels[source++];
                    const graphics::Pixel rgba_word =
                        (pixel & 0xFF00FF00U) |
                        ((pixel & 0x00FF0000U) >> 16U) |
                        ((pixel & 0x000000FFU) << 16U);
                    std::memcpy(destination.data() + output,
                                &rgba_word,
                                sizeof(rgba_word));
                    output += sizeof(rgba_word);
                }
            }
        };

        if (pixel_count < kParallelFrameConversionPixels || region_height < 4U) {
            convert_rgba_rows(0U, region_height);
            return;
        }

        const usize rows_per_chunk = std::max<usize>(
            1U, 4U * 1024U / std::max<usize>(region_width, 1U));
        shared_compute_executor().parallel_for(
            WorkClass::frame_critical,
            pixel_count,
            region_height,
            4U,
            rows_per_chunk,
            convert_rgba_rows);
        return;
    }

    const auto convert_rows = [&](usize row_begin, usize row_end) {
        for (usize row = row_begin; row < row_end; ++row) {
            usize source =
                (static_cast<usize>(region.y) + row) * source_stride +
                static_cast<usize>(region.x);
            usize output = row * region_width * 4U;
            for (usize column = 0U; column < region_width; ++column) {
                // Canvas/mutable image writes are already converted to the
                // emulated RGB565 device representation at draw/composite
                // time. Repeating rgb565_roundtrip() here did the same bit
                // expansion for every published pixel, i.e. millions of
                // redundant operations per second on larger games.
                const graphics::Pixel display_pixel = source_pixels[source++];
                if (format == FramePixelFormat::rgba8) {
                    destination[output++] = graphics::red(display_pixel);
                    destination[output++] = graphics::green(display_pixel);
                    destination[output++] = graphics::blue(display_pixel);
                } else {
                    destination[output++] = graphics::blue(display_pixel);
                    destination[output++] = graphics::green(display_pixel);
                    destination[output++] = graphics::red(display_pixel);
                }
                destination[output++] = graphics::alpha(display_pixel);
            }
        }
    };

    if (pixel_count < kParallelFrameConversionPixels || region_height < 4U) {
        convert_rows(0U, region_height);
        return;
    }

    const usize rows_per_chunk = std::max<usize>(
        1U, 4U * 1024U / std::max<usize>(region_width, 1U));
    shared_compute_executor().parallel_for(
        WorkClass::frame_critical,
        pixel_count,
        region_height,
        4U,
        rows_per_chunk,
        convert_rows);
}

[[nodiscard]] Status publish_canvas_graphics(
    vm::Machine& machine,
    Framebuffer& framebuffer,
    vm::ObjectRef graphics_object,
    std::vector<u8>* reusable_rgba = nullptr) {
    if (graphics_object.is_null()) return {};
    auto target_value = machine.heap().field(graphics_object,
                                             kGraphicsTargetField);
    if (!target_value) return std::unexpected(target_value.error());
    auto image_object = target_value->as_reference();
    if (!image_object) return std::unexpected(image_object.error());
    auto image = machine.graphics().image(image_object->bits);
    if (!image) return std::unexpected(image.error());

    // Drawing primitives already accumulate a dirty bounding box. Avoid
    // reconverting the entire display on every paint/flush when only a small
    // sprite, text run or HUD region changed. A no-change flush still reaches
    // the scheduler's frame-boundary hook in the caller, but does not force a
    // redundant host framebuffer generation/copy.
    if (!(*image)->has_dirty_region()) {
        vm::PerformanceCounters::record_canvas_unchanged_publication();
        return {};
    }
    const auto publication_started = std::chrono::steady_clock::now();
    const graphics::ImageRegion dirty = (*image)->dirty_region();
    if (dirty.x < 0 || dirty.y < 0 || dirty.width <= 0 || dirty.height <= 0 ||
        static_cast<i64>(dirty.x) + dirty.width > (*image)->width() ||
        static_cast<i64>(dirty.y) + dirty.height > (*image)->height()) {
        return fail(ErrorCode::internal_error,
                    "Canvas dirty region is outside its image storage");
    }

    std::vector<u8> local_rgba;
    std::vector<u8>& rgba = reusable_rgba != nullptr
        ? *reusable_rgba
        : local_rgba;
    const Dimensions dimensions {(*image)->width(), (*image)->height()};
    const FramePixelFormat frame_format = canvas_frame_pixel_format();
    const auto current_frame = framebuffer.metadata();
    const bool can_update_region =
        current_frame.dimensions.width == dimensions.width &&
        current_frame.dimensions.height == dimensions.height &&
        current_frame.byte_count == (*image)->pixels().size() * 4U &&
        current_frame.pixel_format == frame_format;

    if (can_update_region) {
        const auto dirty_regions = (*image)->dirty_regions();
        usize dirty_pixels = 0U;
        for (const graphics::ImageRegion& region : dirty_regions) {
            if (region.x < 0 || region.y < 0 || region.width <= 0 ||
                region.height <= 0 ||
                static_cast<i64>(region.x) + region.width > (*image)->width() ||
                static_cast<i64>(region.y) + region.height > (*image)->height()) {
                return fail(ErrorCode::internal_error,
                            "Canvas dirty island is outside its image storage");
            }
            dirty_pixels += static_cast<usize>(region.width) *
                            static_cast<usize>(region.height);
        }
        // Older/custom Image producers may expose only the aggregate region.
        const bool use_islands = !dirty_regions.empty();
        if (!use_islands) {
            dirty_pixels = static_cast<usize>(dirty.width) *
                           static_cast<usize>(dirty.height);
        }
        rgba.resize(dirty_pixels * 4U);
        usize destination = 0U;
        std::array<FrameRegionUpdate, graphics::Image::kMaximumDirtyRegions>
            updates;
        usize update_count = 0U;
        const auto append_region = [&](const graphics::ImageRegion& region) {
            const usize byte_start = destination;
            const usize region_pixels = static_cast<usize>(region.width) *
                                        static_cast<usize>(region.height);
            const usize region_bytes = region_pixels * 4U;
            convert_image_region_to_frame_pixels(
                (*image)->pixels(),
                (*image)->width(),
                region,
                std::span<u8>(rgba).subspan(byte_start, region_bytes),
                frame_format);
            destination += region_bytes;
            updates[update_count++] = FrameRegionUpdate {
                .x = region.x,
                .y = region.y,
                .width = region.width,
                .height = region.height,
                .rgba = std::span<const u8>(
                    rgba.data() + static_cast<std::ptrdiff_t>(byte_start),
                    destination - byte_start),
            };
        };
        if (use_islands) {
            for (const graphics::ImageRegion& region : dirty_regions) {
                append_region(region);
            }
        } else {
            append_region(dirty);
        }
        auto updated = framebuffer.update_regions(
            dimensions,
            std::span<const FrameRegionUpdate>(updates.data(), update_count),
            frame_format);
        if (!updated) return updated;
        (*image)->clear_dirty_region();
        const usize full_frame_pixels = (*image)->pixels().size();
        vm::PerformanceCounters::record_canvas_publication(
            dirty_pixels,
            full_frame_pixels,
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - publication_started).count()));
        return {};
    }

    rgba.resize((*image)->pixels().size() * 4U);
    convert_image_region_to_frame_pixels(
        (*image)->pixels(),
        (*image)->width(),
        graphics::ImageRegion {
            .x = 0,
            .y = 0,
            .width = (*image)->width(),
            .height = (*image)->height(),
        },
        rgba,
        frame_format);
    auto replaced = framebuffer.replace_exchange(dimensions, rgba, frame_format);
    if (replaced) {
        (*image)->clear_dirty_region();
        vm::PerformanceCounters::record_canvas_publication(
            (*image)->pixels().size(),
            (*image)->pixels().size(),
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - publication_started).count()));
    }
    return replaced;
}

[[nodiscard]] bool is_directory(const std::string& path) noexcept {
    struct stat status {};
    return !path.empty() && ::stat(path.c_str(), &status) == 0 &&
           S_ISDIR(status.st_mode);
}

[[nodiscard]] bool is_regular_file(const std::string& path) noexcept {
    struct stat status {};
    return !path.empty() && ::stat(path.c_str(), &status) == 0 &&
           S_ISREG(status.st_mode);
}

void append_ascii_console(vm::Machine& machine, std::string_view text) {
    std::u16string utf16;
    utf16.reserve(text.size() + 1U);
    for (const char character : text) {
        utf16.push_back(static_cast<char16_t>(
            static_cast<unsigned char>(character)));
    }
    if (utf16.empty() || utf16.back() != u'\n') utf16.push_back(u'\n');
    machine.append_console(utf16);
}

void append_utf8(std::string& output, std::u16string_view text) {
    for (usize index = 0; index < text.size(); ++index) {
        u32 code_point = static_cast<u16>(text[index]);
        if (code_point >= 0xD800U && code_point <= 0xDBFFU &&
            index + 1U < text.size()) {
            const u32 low = static_cast<u16>(text[index + 1U]);
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                code_point = 0x10000U +
                    ((code_point - 0xD800U) << 10U) +
                    (low - 0xDC00U);
                ++index;
            }
        }
        if (code_point <= 0x7FU) {
            output.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
            output.push_back(static_cast<char>(
                0x80U | (code_point & 0x3FU)));
        } else if (code_point <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            output.push_back(static_cast<char>(
                0x80U | ((code_point >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(
                0x80U | (code_point & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            output.push_back(static_cast<char>(
                0x80U | ((code_point >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(
                0x80U | ((code_point >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(
                0x80U | (code_point & 0x3FU)));
        }
    }
}

[[nodiscard]] Result<std::string> describe_throwable(
    vm::Machine& machine,
    vm::ObjectRef throwable,
    usize depth = 0U) {
    auto class_name = machine.heap().class_name(throwable);
    if (!class_name) return std::unexpected(class_name.error());
    std::string description = *class_name;

    auto message_value = machine.heap().field(throwable, 0U);
    if (message_value) {
        auto message = message_value->as_reference();
        if (message && !message->is_null()) {
            auto text = machine.heap().string_value(*message);
            if (text && !text->empty()) {
                description += ": ";
                append_utf8(description, *text);
            }
        }
    }

    if (depth < 3U) {
        auto cause_value = machine.heap().field(throwable, 1U);
        if (cause_value) {
            auto cause = cause_value->as_reference();
            if (cause && !cause->is_null() && *cause != throwable) {
                auto nested = describe_throwable(machine, *cause, depth + 1U);
                if (nested) description += " caused by " + *nested;
            }
        }
    }
    return description;
}

[[nodiscard]] Status require_normal_completion(
    vm::Machine& machine,
    const vm::ExecutionResult& result,
    std::string_view lifecycle_phase) {
    if (result.completed_normally()) {
        return {};
    }
    if (!result.throwable.has_value()) {
        return fail(ErrorCode::internal_error,
                    std::string(lifecycle_phase) +
                        " ended abnormally without a throwable");
    }
    auto throwable = describe_throwable(machine, *result.throwable);
    if (!throwable) {
        return std::unexpected(throwable.error());
    }
    std::string message = std::string(lifecycle_phase) +
        " threw an uncaught Java exception: " + *throwable;
    if (!result.exception_context.empty()) {
        message += " from " + result.exception_context;
    }
    return fail(ErrorCode::java_exception, std::move(message));
}

} // namespace

ApplicationVM::ApplicationVM(
    Dimensions dimensions,
    std::array<i32, 7> keymap,
    AppFramePacingConfig frame_pacing,
    AppHeapConfig heap_config,
    bool jit_enabled,
    Framebuffer& framebuffer,
    std::function<void()> host_wake,
    std::shared_ptr<translation::TranslationService> translation_service)
    : machine(classes,
              vm::HeapLimits {.maximum_bytes = heap_config.maximum_bytes}),
      canvas(machine, dimensions, keymap) {
    machine.configure_canvas_bridge(&canvas);
    machine.configure_frame_pacing(
        frame_pacing.frames_per_second,
        frame_pacing.mode);
    machine.configure_jit(jit_enabled);
    machine.configure_translation_service(std::move(translation_service));
    CanvasRenderHooks hooks;
    hooks.request_host_wake = std::move(host_wake);
    hooks.acquire_paint_graphics = [this, &framebuffer](
        vm::Machine& target_machine,
        vm::ObjectRef,
        Dimensions target_dimensions,
        vm::CanvasRect repaint_region) {
        const auto current = framebuffer.metadata();
        const bool synchronize = !paint_graphics_published ||
            current.generation != paint_graphics_generation;
        return prepare_canvas_graphics(target_machine,
                                       framebuffer,
                                       target_dimensions,
                                       repaint_region,
                                       true,
                                       &paint_graphics,
                                       synchronize);
    };
    hooks.commit_paint = [this, &framebuffer](
        vm::Machine& target_machine,
        vm::ObjectRef,
        vm::ObjectRef graphics,
        vm::CanvasRect) {
        target_machine.pace_frame_publication();
        auto published = publish_canvas_graphics(target_machine,
                                                 framebuffer,
                                                 graphics,
                                                 &frame_rgba_scratch);
        if (published) {
            paint_graphics_generation = framebuffer.metadata().generation;
            paint_graphics_published = true;
            target_machine.note_frame_pacing_boundary();
        }
        return published;
    };
    hooks.acquire_game_graphics = [&framebuffer](
        vm::Machine& target_machine,
        vm::ObjectRef,
        Dimensions target_dimensions) {
        return prepare_canvas_graphics(
            target_machine,
            framebuffer,
            target_dimensions,
            vm::CanvasRect {0, 0,
                            target_dimensions.width,
                            target_dimensions.height},
            false);
    };
    hooks.flush_game_graphics = [this, &framebuffer](
        vm::Machine& target_machine,
        vm::ObjectRef,
        vm::ObjectRef graphics,
        vm::CanvasRect) {
        target_machine.pace_frame_publication();
        auto published = publish_canvas_graphics(target_machine,
                                                 framebuffer,
                                                 graphics,
                                                 &frame_rgba_scratch);
        if (published) target_machine.note_frame_pacing_boundary();
        return published;
    };
    canvas.configure_render_hooks(std::move(hooks));
}

ApplicationVM::~ApplicationVM() {
    machine.shutdown();
    machine.configure_canvas_bridge(nullptr);
}

Runtime::Runtime()
    : input_queue_(1'024),
      ui_queue_(1'024),
      ui_translation_replay_(std::make_shared<UiTranslationReplayState>()) {
    ui_translation_replay_->queue = &ui_queue_;
    ui_translation_replay_->wake = [this] { signal_host_wake(); };
    framebuffer_.configure_change_sink([this] { signal_host_wake(); });
}

Runtime::~Runtime() {
    configure_host_wake({});
    framebuffer_.configure_change_sink({});
    ui_translation_replay_->deactivate();
    stop();
}

void Runtime::configure_host_wake(HostWakeSink sink) {
    std::scoped_lock lock(host_wake_mutex_);
    host_wake_sink_ = std::move(sink);
}

void Runtime::signal_host_wake() noexcept {
    HostWakeSink sink;
    {
        std::scoped_lock lock(host_wake_mutex_);
        sink = host_wake_sink_;
    }
    if (sink) sink();
}

Status Runtime::configure(std::string runtime_home,
                          std::string optional_class_archive) {
    if (!is_directory(runtime_home)) {
        return fail(ErrorCode::invalid_argument,
                    "runtime home is not an accessible directory");
    }
    if (!optional_class_archive.empty() &&
        !is_regular_file(optional_class_archive)) {
        return fail(ErrorCode::invalid_argument,
                    "optional class archive is not an accessible file");
    }

    std::scoped_lock lock(mutex_);
    if (running_) {
        return fail(ErrorCode::already_running,
                    "runtime cannot be reconfigured while running");
    }

    // The suite database owns the stable MIDlet-to-suite mapping used by RMS,
    // filesystem and push storage paths. Leaving SuiteStore in its default
    // in-memory mode makes every process launch reinstall the library from
    // scratch and can redirect a game to a different suite data directory.
    // Load the persistent store before publishing the runtime configuration so
    // a failed recovery never exposes a half-configured Runtime.
    configured_ = false;
    runtime_home_.clear();
    optional_class_archive_.clear();
    permission_policies_.clear();
    default_frame_pacing_ = AppFramePacingConfig {
        .frames_per_second = 30,
        .mode = vm::FramePacingMode::native,
    };
    app_frame_pacing_.clear();
    app_heap_configs_.clear();
    jit_enabled_by_default_ = true;
    suite_store_.clear();
    auto suite_store_status = suite_store_.configure(SuiteStoreConfig {
        .root_path = runtime_home,
    });
    if (!suite_store_status) {
        return suite_store_status;
    }

    runtime_home_ = std::move(runtime_home);
    optional_class_archive_ = std::move(optional_class_archive);
    permission_policies_.clear();
    configured_ = true;
    last_exit_code_ = 0;
    last_error_message_.clear();
    return {};
}

Status Runtime::configure_keymap(std::array<i32, 7> keymap) {
    std::vector<std::shared_ptr<ApplicationVM>> application_vms;
    {
        std::scoped_lock lock(mutex_);
        keymap_ = keymap;
        application_vms.reserve(apps_.size());
        for (const auto& [id, app] : apps_) {
            (void)id;
            if (app.vm != nullptr) application_vms.push_back(app.vm);
        }
    }
    for (const auto& vm : application_vms) {
        std::scoped_lock vm_operation(vm->operation_mutex);
        vm->canvas.set_keymap(keymap);
    }
    return {};
}

Status Runtime::configure_app_frame_pacing(
    AppId app_id,
    i32 frames_per_second,
    vm::FramePacingMode mode) {
    if (!app_id.valid()) {
        return fail(ErrorCode::invalid_argument,
                    "frame pacing requires a valid application ID");
    }
    if (frames_per_second < 1 || frames_per_second > 240) {
        return fail(ErrorCode::invalid_argument,
                    "frame rate must be between 1 and 240 FPS");
    }
    switch (mode) {
    case vm::FramePacingMode::native:
    case vm::FramePacingMode::cap:
    case vm::FramePacingMode::override_game_loop:
        break;
    default:
        return fail(ErrorCode::invalid_argument,
                    "unknown frame pacing mode");
    }

    std::shared_ptr<ApplicationVM> application_vm;
    {
        std::scoped_lock lock(mutex_);
        app_frame_pacing_.insert_or_assign(
            app_id.value,
            AppFramePacingConfig {
                .frames_per_second = frames_per_second,
                .mode = mode,
            });
        if (const auto found = apps_.find(app_id.value);
            found != apps_.end()) {
            application_vm = found->second.vm;
        }
    }
    if (application_vm != nullptr) {
        std::scoped_lock vm_operation(application_vm->operation_mutex);
        application_vm->machine.configure_frame_pacing(
            frames_per_second,
            mode);
    }
    return {};
}

Status Runtime::configure_app_heap(AppId app_id, i32 heap_megabytes) {
    if (!app_id.valid()) {
        return fail(ErrorCode::invalid_argument,
                    "heap configuration requires a valid application ID");
    }
    if (heap_megabytes < 1 || heap_megabytes > 512) {
        return fail(ErrorCode::invalid_argument,
                    "heap size must be between 1 and 512 MiB");
    }

    const usize maximum_bytes = static_cast<usize>(heap_megabytes) *
        1024U * 1024U;
    std::scoped_lock lock(mutex_);
    if (const auto found = apps_.find(app_id.value);
        found != apps_.end() &&
        (found->second.vm != nullptr || found->second.lifecycle_busy)) {
        return fail(ErrorCode::invalid_state,
                    "heap size changes require restarting the application");
    }
    app_heap_configs_.insert_or_assign(
        app_id.value,
        AppHeapConfig {.maximum_bytes = maximum_bytes});
    return {};
}

Status Runtime::configure_jit(bool enabled) {
    std::vector<std::shared_ptr<ApplicationVM>> application_vms;
    {
        std::scoped_lock lock(mutex_);
        jit_enabled_by_default_ = enabled;
        application_vms.reserve(apps_.size());
        for (const auto& [id, app] : apps_) {
            (void)id;
            if (app.vm != nullptr) application_vms.push_back(app.vm);
        }
    }
    for (const auto& application_vm : application_vms) {
        std::scoped_lock vm_operation(application_vm->operation_mutex);
        application_vm->machine.configure_jit(enabled);
    }
    return {};
}

vm::JitAvailability Runtime::jit_availability() const noexcept {
    return vm::BaselineJit::probe_platform();
}

Status Runtime::configure_input_capabilities(bool pointer_events,
                                             bool pointer_motion,
                                             bool repeat_events) {
    std::vector<std::shared_ptr<ApplicationVM>> application_vms;
    {
        std::scoped_lock lock(mutex_);
        pointer_events_supported_ = pointer_events;
        pointer_motion_supported_ = pointer_events && pointer_motion;
        repeat_events_supported_ = repeat_events;
        application_vms.reserve(apps_.size());
        for (const auto& [id, app] : apps_) {
            (void)id;
            if (app.vm != nullptr) application_vms.push_back(app.vm);
        }
    }
    for (const auto& vm : application_vms) {
        std::scoped_lock vm_operation(vm->operation_mutex);
        vm->canvas.set_input_capabilities(pointer_events,
                                          pointer_events && pointer_motion,
                                          repeat_events);
    }
    return {};
}

Status Runtime::configure_translation(
    bool enabled,
    translation::TranslationProvider provider,
    std::string source_language,
    std::string target_language) {
    std::string runtime_home;
    {
        std::scoped_lock lock(mutex_);
        if (!configured_) {
            return fail(ErrorCode::not_configured,
                        "translation requires a configured runtime");
        }
        translation_enabled_by_default_ = enabled;
        if (!enabled) return {};
        runtime_home = runtime_home_;
    }

    const auto sanitize = [](std::string value) {
        for (char& character : value) {
            const auto byte = static_cast<unsigned char>(character);
            if (std::isalnum(byte) == 0 && character != '-') character = '_';
        }
        return value;
    };
    const std::string provider_name =
        provider == translation::TranslationProvider::automatic
            ? "automatic"
            : (provider == translation::TranslationProvider::bing
                ? "bing" : "google");
    translation::TranslationConfiguration configuration {
        .enabled = true,
        .provider = provider,
        .source_language = std::move(source_language),
        .target_language = std::move(target_language),
        .maximum_pending_requests = 512U,
        .maximum_concurrent_requests = 1U,
        .maximum_source_bytes = 2'048U,
        .maximum_batch_items = 32U,
        .maximum_batch_source_bytes = 8U * 1'024U,
        .batch_coalescing_delay_ms = 4,
        .maximum_batch_wait_ms = 12,
    };
    configuration.cache_path = runtime_home + "/translation-" +
        provider_name + "-" + sanitize(configuration.source_language) +
        "-" + sanitize(configuration.target_language) + "-v2.bin";
    auto service = std::make_shared<translation::TranslationService>(
        std::move(configuration));
    if (!service->enabled()) {
        return fail(ErrorCode::invalid_argument,
                    "translation configuration is invalid");
    }

    std::scoped_lock lock(mutex_);
    translation_service_ = std::move(service);
    return {};
}

Status Runtime::configure_app_translation(
    AppId app_id,
    bool enabled,
    translation::TranslationProvider provider,
    std::string source_language,
    std::string target_language) {
    if (!app_id.valid()) {
        return fail(ErrorCode::invalid_argument,
                    "application translation requires a valid app ID");
    }

    std::shared_ptr<ApplicationVM> application_vm;
    std::string runtime_home;
    {
        std::scoped_lock lock(mutex_);
        if (!configured_) {
            return fail(ErrorCode::not_configured,
                        "translation requires a configured runtime");
        }
        const App* app = find_app_unlocked(app_id);
        if (app == nullptr || app->vm == nullptr ||
            app->state == AppState::destroyed) {
            return fail(ErrorCode::invalid_state,
                        "application translation target is unavailable");
        }
        application_vm = app->vm;
        runtime_home = runtime_home_;
    }

    if (!enabled) {
        std::scoped_lock vm_operation(application_vm->operation_mutex);
        application_vm->machine.configure_translation_service(nullptr);
        application_vm->canvas.invalidate_translation_rendering();
        return {};
    }

    const auto sanitize = [](std::string value) {
        for (char& character : value) {
            const auto byte = static_cast<unsigned char>(character);
            if (std::isalnum(byte) == 0 && character != '-') character = '_';
        }
        return value;
    };
    const std::string provider_name =
        provider == translation::TranslationProvider::automatic
            ? "automatic"
            : (provider == translation::TranslationProvider::bing
                ? "bing" : "google");
    translation::TranslationConfiguration configuration {
        .enabled = true,
        .provider = provider,
        .source_language = std::move(source_language),
        .target_language = std::move(target_language),
        .maximum_pending_requests = 512U,
        .maximum_concurrent_requests = 1U,
        .maximum_source_bytes = 2'048U,
        .maximum_batch_items = 32U,
        .maximum_batch_source_bytes = 8U * 1'024U,
        .batch_coalescing_delay_ms = 4,
        .maximum_batch_wait_ms = 12,
    };
    configuration.cache_path = runtime_home + "/translation-" +
        provider_name + "-" + sanitize(configuration.source_language) +
        "-" + sanitize(configuration.target_language) + "-v2.bin";
    auto service = std::make_shared<translation::TranslationService>(
        std::move(configuration));
    if (!service->enabled()) {
        return fail(ErrorCode::invalid_argument,
                    "translation configuration is invalid");
    }

    {
        std::scoped_lock vm_operation(application_vm->operation_mutex);
        application_vm->machine.configure_translation_service(std::move(service));
        application_vm->canvas.invalidate_translation_rendering();
    }
    return {};
}

Status Runtime::configure_permission_prompt(
    security::PermissionPromptCallback prompt) {
    std::scoped_lock lock(mutex_);
    if (running_) {
        return fail(ErrorCode::already_running,
                    "permission prompt cannot change while running");
    }
    permission_prompt_ = std::move(prompt);
    permission_policies_.clear();
    return {};
}

Status Runtime::set_suite_trust(SuiteId suite_id,
                                security::SuiteTrust trust) {
    if (!suite_id.valid()) {
        return fail(ErrorCode::invalid_argument,
                    "suite trust requires a valid suite ID");
    }
    std::scoped_lock lock(mutex_);
    if (suite_store_.find(suite_id) == nullptr) {
        return fail(ErrorCode::invalid_argument,
                    "suite trust references an unknown suite");
    }
    const auto existing_trust = suite_trust_.find(suite_id.value);
    const security::SuiteTrust current_trust =
        existing_trust == suite_trust_.end()
            ? security::SuiteTrust::untrusted
            : existing_trust->second;
    if (current_trust == trust) {
        // Reasserting the host policy is part of every foreground launch. It
        // must remain safe while an existing MIDlet isolate is alive; only an
        // actual domain change would invalidate its permission policy.
        return {};
    }

    for (const auto& [app_id, app] : apps_) {
        (void)app_id;
        if (app.suite_id == suite_id && app.vm != nullptr &&
            app.state != AppState::destroyed) {
            return fail(ErrorCode::invalid_state,
                        "suite trust cannot change while its MIDlet is alive");
        }
    }
    suite_trust_.insert_or_assign(suite_id.value, trust);
    permission_policies_.erase(suite_id.value);
    return {};
}

Result<SuiteId> Runtime::install_jar(const std::string& jar_path) {
    return install_jar(jar_path, {});
}

Result<SuiteId> Runtime::install_jar_replacing(const std::string& jar_path) {
    if (!is_regular_file(jar_path)) {
        return fail(ErrorCode::invalid_argument,
                    "JAR path is not an accessible regular file");
    }

    std::scoped_lock lock(mutex_);
    if (!configured_) {
        return fail(ErrorCode::not_configured,
                    "runtime must be configured before installing a suite");
    }
    return suite_store_.install_replacing(jar_path);
}

Result<SuiteId> Runtime::install_jar(
    const std::string& jar_path,
    std::string_view identity_scope) {
    if (!is_regular_file(jar_path)) {
        return fail(ErrorCode::invalid_argument,
                    "JAR path is not an accessible regular file");
    }

    std::scoped_lock lock(mutex_);
    if (!configured_) {
        return fail(ErrorCode::not_configured,
                    "runtime must be configured before installing a suite");
    }
    // Suite installation mutates only the persistent SuiteStore. Each live
    // MIDlet owns an immutable copy of its Suite and its own class repository,
    // so adding another JAR is safe while unrelated applications are running.
    return identity_scope.empty()
        ? suite_store_.install(jar_path)
        : suite_store_.install_scoped(jar_path, identity_scope);
}

std::optional<SuiteId> Runtime::find_installed_suite(
    std::string_view vendor,
    std::string_view name,
    std::string_view version,
    std::string_view identity_scope) const noexcept {
    // The iOS library already persisted these manifest fields when the JAR was
    // imported. Reusing the managed suite by its MIDP identity avoids parsing
    // and hashing the original JAR again on every cold process launch.
    if (vendor.empty() || name.empty()) return std::nullopt;

    std::string expected_identity;
    expected_identity.reserve(
        vendor.size() + name.size() + identity_scope.size() + 2U);
    expected_identity.append(vendor);
    expected_identity.push_back('\x1F');
    expected_identity.append(name);
    if (!identity_scope.empty()) {
        expected_identity.push_back('\x1E');
        expected_identity.append(identity_scope);
    }

    std::scoped_lock lock(mutex_);
    if (!configured_) return std::nullopt;
    for (const SuiteId id : suite_store_.list()) {
        const Suite* suite = suite_store_.find(id);
        if (suite == nullptr || suite->identity_key != expected_identity ||
            suite->vendor != vendor || suite->display_name != name) {
            continue;
        }
        if (!version.empty() && suite->version != version) continue;
        return id;
    }
    return std::nullopt;
}

Status Runtime::uninstall_suite(SuiteId suite_id,
                                bool remove_data) {
    if (!suite_id.valid()) {
        return fail(ErrorCode::invalid_argument,
                    "suite uninstall requires a valid suite ID");
    }

    std::scoped_lock lock(mutex_);
    if (!configured_) {
        return fail(ErrorCode::not_configured,
                    "runtime must be configured before uninstalling a suite");
    }
    for (const auto& [app_id, app] : apps_) {
        (void)app_id;
        if (app.suite_id == suite_id && app.vm != nullptr &&
            app.state != AppState::destroyed) {
            return fail(ErrorCode::invalid_state,
                        "suite cannot be uninstalled while its MIDlet is alive");
        }
    }

    const SuiteUninstallPolicy policy {
        .remove_rms = remove_data,
        .remove_files = remove_data,
        .remove_permissions = remove_data,
        .remove_push = remove_data,
        .remove_temporary = remove_data,
    };
    auto uninstalled = suite_store_.uninstall(suite_id, policy);
    if (!uninstalled) {
        return uninstalled;
    }

    suite_trust_.erase(suite_id.value);
    permission_policies_.erase(suite_id.value);
    return {};
}

Status Runtime::start_system() {
    std::scoped_lock lock(mutex_);
    if (!configured_) {
        return fail(ErrorCode::not_configured,
                    "runtime must be configured before startup");
    }
    if (running_) {
        return {};
    }
    running_ = true;
    suspended_ = false;
    last_exit_code_ = 0;
    last_error_message_.clear();
    return {};
}

Status Runtime::start_midlet(SuiteId suite_id,
                             std::string main_class,
                             AppId app_id,
                             Dimensions dimensions) {
    if (!suite_id.valid() || !app_id.valid() || main_class.empty() ||
        !dimensions.valid()) {
        return fail(ErrorCode::invalid_argument,
                    "invalid MIDlet launch arguments");
    }

    Suite suite;
    std::string runtime_home;
    std::string optional_class_archive;
    std::array<i32, 7> keymap {};
    AppFramePacingConfig frame_pacing;
    AppHeapConfig heap_config;
    bool jit_enabled = true;
    bool pointer_events_supported = true;
    bool pointer_motion_supported = true;
    bool repeat_events_supported = true;
    security::SharedPermissionPolicy permission_policy;
    std::shared_ptr<translation::TranslationService> translation_service;
    security::SuiteTrust suite_trust = security::SuiteTrust::untrusted;
    security::PermissionPromptCallback permission_prompt;
    u64 lifecycle_token = 0;
    {
        std::unique_lock lock(mutex_);
        auto running = require_running_unlocked();
        if (!running) return running;
        if (const auto existing = apps_.find(app_id.value);
            existing != apps_.end() &&
            (existing->second.state != AppState::destroyed ||
             existing->second.vm != nullptr ||
             existing->second.lifecycle_busy)) {
            return fail(ErrorCode::invalid_state,
                        "application ID is already in use");
        }
        const Suite* stored_suite = suite_store_.find(suite_id);
        if (stored_suite == nullptr) {
            return fail(ErrorCode::invalid_argument,
                        "suite ID does not exist");
        }
        suite = *stored_suite;
        runtime_home = runtime_home_;
        optional_class_archive = optional_class_archive_;
        keymap = keymap_;
        frame_pacing = default_frame_pacing_;
        if (const auto pacing = app_frame_pacing_.find(app_id.value);
            pacing != app_frame_pacing_.end()) {
            frame_pacing = pacing->second;
        }
        if (const auto heap = app_heap_configs_.find(app_id.value);
            heap != app_heap_configs_.end()) {
            heap_config = heap->second;
        }
        jit_enabled = jit_enabled_by_default_;
        pointer_events_supported = pointer_events_supported_;
        pointer_motion_supported = pointer_motion_supported_;
        repeat_events_supported = repeat_events_supported_;
        permission_prompt = permission_prompt_;
        translation_service = translation_enabled_by_default_
            ? translation_service_
            : nullptr;
        if (const auto existing = permission_policies_.find(suite_id.value);
            existing != permission_policies_.end()) {
            permission_policy = existing->second;
        }
        if (const auto trust = suite_trust_.find(suite_id.value);
            trust != suite_trust_.end()) {
            suite_trust = trust->second;
        }

        lifecycle_token = ++sequence_;
        apps_.insert_or_assign(app_id.value, App {
            .id = app_id,
            .suite_id = suite_id,
            .main_class = main_class,
            .dimensions = dimensions,
            .state = AppState::none,
            .generation = sequence_,
            .lifecycle_token = lifecycle_token,
            .lifecycle_busy = true,
            .vm = nullptr,
        });
    }

    std::shared_ptr<ApplicationVM> application_vm;
    const auto fail_start = [&](Error error) -> Status {
        const std::string diagnostic = format_error_message(error);
        std::unique_lock lock(mutex_);
        App* app = find_app_unlocked(app_id);
        if (app != nullptr && app->lifecycle_token == lifecycle_token) {
            app->state = AppState::error;
            app->lifecycle_busy = false;
            if (application_vm != nullptr) app->vm = application_vm;
            app->error_message = diagnostic;
            app->generation = ++sequence_;
            last_exit_code_ = -1;
            last_error_message_ = diagnostic;
        }
        return std::unexpected(std::move(error));
    };

    if (permission_policy == nullptr) {
        auto candidate = std::make_shared<security::PermissionPolicy>();
        auto configured = candidate->configure(
            security::PermissionPolicyConfig {
                .suite_id = suite_id,
                .trust = suite_trust,
                .persistence_path = runtime_home + "/security/" +
                    std::to_string(suite_id.value) + ".permissions",
                .declared_permissions = suite.declared_permissions,
                .required_permissions =
                    suite.declared_required_permissions,
                .optional_permissions =
                    suite.declared_optional_permissions,
                // The iOS host marks locally imported JARs as trusted so they
                // run with the emulator's compatibility domain. A number of
                // real-world unsigned MIDlets (including Nicknso) declare only
                // an unrelated optional permission and still open sockets at
                // runtime. Treating that partial list as an allow-list blocks
                // Connector.open() before the TCP adapter is reached. phoneME's
                // unidentified/compatibility domain grants from the domain
                // policy instead of limiting access to the manifest list.
                .enforce_declared_permissions =
                    suite.has_permission_declarations &&
                    suite_trust != security::SuiteTrust::trusted,
                .trusted_default_allow = true,
                .prompt = std::move(permission_prompt),
            });
        if (!configured) return fail_start(configured.error());
        std::unique_lock lock(mutex_);
        const auto [iterator, inserted] = permission_policies_.emplace(
            suite_id.value, candidate);
        (void)inserted;
        permission_policy = iterator->second;
    }

    application_vm = std::make_shared<ApplicationVM>(
        dimensions,
        keymap,
        frame_pacing,
        heap_config,
        jit_enabled,
        framebuffer_,
        [this] { signal_host_wake(); },
        std::move(translation_service));
    // Persist only stable hot method signatures per installed suite. Profile
    // I/O is opportunistic: a missing/unwritable profile must never prevent a
    // MIDlet from launching, while a valid profile lets the next run compile
    // previously hot methods on their first invocation.
    // Keep hot profiles generation-scoped. They only store method identities,
    // but carrying an old profile across a materially different JIT can force
    // newly changed compiler/deopt paths hot on the first gameplay transition.
    (void)application_vm->machine.configure_jit_profile(
        runtime_home + "/jit/v2/" + std::to_string(suite_id.value) + ".hot");
    // Launch is latency-sensitive: avoid eagerly decoding/compiling every
    // one-shot helper while still allowing sufficiently large loop-heavy
    // constructor/startApp work to benefit from native execution. The guard
    // switches back to the normal gameplay tier as soon as start_midlet
    // returns (including the first-observable-frame deferred startApp path).
    ScopedJitStartup jit_startup(application_vm->machine);
    application_vm->canvas.set_input_capabilities(
        pointer_events_supported,
        pointer_motion_supported,
        repeat_events_supported);
    application_vm->machine.set_permission_policy(permission_policy);
    ApplicationVM* const ui_observer = application_vm.get();
    const auto ui_replay = ui_translation_replay_;
    application_vm->machine.configure_ui_bridge(
        app_id.value,
        [ui_observer, ui_replay](vm::UiBridgeEvent event) {
            ui_observer->ui_event_count.fetch_add(1U,
                                                  std::memory_order_release);
            auto pending = std::make_shared<PendingUiTranslation>(
                UiEvent {
                    .kind = event.kind,
                    .component_id = event.component_id,
                    .parent_id = event.parent_id,
                    .component_type = event.component_type,
                    .index = event.index,
                    .arguments = event.arguments,
                    .value64 = event.value64,
                    .generation = event.generation,
                    .text = std::move(event.text),
                    .detail = std::move(event.detail),
                },
                ui_replay,
                ui_replay->current_epoch());

            const auto ui_translation =
                ui_observer->machine.translation_service();
            if (ui_translation && ui_translation->enabled()) {
                const UiEvent original = pending->snapshot();
                if (!original.text.empty()) {
                    auto translated = ui_translation->lookup_or_request_utf8(
                        original.text,
                        [pending](std::string value) {
                            pending->replace_text(false, std::move(value));
                        });
                    if (translated) {
                        pending->replace_cached(false, std::move(*translated));
                    }
                }
                if (!original.detail.empty() &&
                    should_translate_ui_detail(original)) {
                    auto translated = ui_translation->lookup_or_request_utf8(
                        original.detail,
                        [pending](std::string value) {
                            pending->replace_text(true, std::move(value));
                        });
                    if (translated) {
                        pending->replace_cached(true, std::move(*translated));
                    }
                }
            }
            ui_replay->publish(pending->snapshot(), false);
        });
    // Clear the host's presentation state before the constructor/startApp can
    // emit SCREEN_SHOWN or Canvas events. Emitting RESET after startApp caused
    // an entire poll batch to build a native Form/List and then erase it again,
    // leaving physical iOS devices on an empty framebuffer for LCDUI MIDlets.
    ui_translation_replay_->publish(UiEvent {
        .kind = 1,
        .component_id = app_id.value,
        .generation = lifecycle_token,
    }, false);
    auto configured = application_vm->machine.configure_network_owner(
        app_id.value);
    if (!configured) return fail_start(configured.error());
    configured = application_vm->machine.configure_record_store_root(
        runtime_home + "/rms/" + std::to_string(suite_id.value));
    if (!configured) return fail_start(configured.error());
    configured = application_vm->machine.configure_push_registry(
        runtime_home + "/push", suite_id);
    if (!configured) return fail_start(configured.error());
    configured = application_vm->machine.configure_filesystem(
        runtime_home + "/files/" + std::to_string(suite_id.value),
        runtime_home + "/tmp/" + std::to_string(suite_id.value) + "/" +
            std::to_string(app_id.value));
    if (!configured) return fail_start(configured.error());

    Status classpath;
    if (suite.managed) {
        const u32 cache_version =
            vm::ClassRepository::verification_cache_version();
        const std::unordered_map<std::string, u64> empty_verified_classes;
        const auto& verified_classes =
            suite.verified_class_cache_version == cache_version
                ? suite.verified_classes
                : empty_verified_classes;
        classpath = application_vm->classes.add_archive(
            suite.jar_path,
            suite.archive_sha256,
            cache_version,
            verified_classes);
    } else {
        classpath = application_vm->classes.add_archive(suite.jar_path);
    }
    if (!classpath) return fail_start(classpath.error());
    if (!optional_class_archive.empty()) {
        classpath = application_vm->classes.add_archive(optional_class_archive);
        if (!classpath) return fail_start(classpath.error());
    }
    for (const auto& [key, value] : suite.properties) {
        application_vm->machine.set_app_property(key, value);
    }
    configure_suite_capabilities(application_vm->machine, suite);
    auto is_midlet = application_vm->classes.is_assignable(
        main_class, "javax/microedition/midlet/MIDlet");
    if (!is_midlet) return fail_start(is_midlet.error());
    if (!*is_midlet) {
        return fail_start(Error::make(
            ErrorCode::invalid_argument,
            "MIDlet main class does not extend MIDlet"));
    }

    auto receiver = application_vm->machine.class_states().allocate_instance(
        application_vm->machine.heap(), main_class);
    if (!receiver) return fail_start(receiver.error());
    application_vm->midlet = *receiver;
    {
        std::unique_lock lock(mutex_);
        App* app = find_app_unlocked(app_id);
        if (app == nullptr || app->lifecycle_token != lifecycle_token) {
            return fail(ErrorCode::invalid_state,
                        "MIDlet launch reservation was superseded");
        }
        app->vm = application_vm;
    }

    application_vm->machine.scheduler().set_host_foreground(true);
    auto launch_foregrounded = application_vm->canvas.set_host_foreground(true);
    if (!launch_foregrounded) return fail_start(launch_foregrounded.error());
    {
        std::scoped_lock application_operation(application_vm->operation_mutex);
        // Large obfuscated static initializers are finite launch work. Keep
        // yielding the VM gate to worker threads, but skip foreground CPU
        // backoff until the MIDlet constructor has completed.
        ScopedUnpacedExecution unpaced(
            application_vm->machine.scheduler());
        auto constructor = application_vm->machine.invoke_instance(
            *receiver, main_class, "<init>", "()V", {},
            kMidletConstructorInstructionBudget,
            vm::InstructionBudgetMode::progress_watchdog);
        if (!constructor) return fail_start(constructor.error());
        auto completion = require_normal_completion(application_vm->machine,
                                                    *constructor,
                                                    "MIDlet constructor");
        if (!completion) return fail_start(completion.error());
    }

    bool start_app_deferred = false;
    vm::MidletSignal launch_signal = vm::MidletSignal::none;
#if defined(PHONEME_WEB)
    // A newly scheduled Emscripten pthread cannot execute until this WASM
    // entrypoint returns to the browser event loop. Run startApp on the MIDP
    // lifecycle thread, mark it deferred immediately, and let the normal pump
    // path finalize its result. This also lets blocking browser Fetch and Java
    // networking execute on a real pthread instead of deadlocking launch().
    auto lifecycle_state = std::make_shared<AsyncLifecycleState>();
    {
        std::scoped_lock state_lock(application_vm->lifecycle_state_mutex);
        application_vm->deferred_start_app = lifecycle_state;
    }
    auto lifecycle_thread_root = application_vm->machine.allocate_pinned_instance(
        "java/lang/Thread");
    if (!lifecycle_thread_root) {
        return fail_start(lifecycle_thread_root.error());
    }
    auto lifecycle_thread = lifecycle_thread_root->get();
    if (!lifecycle_thread) return fail_start(lifecycle_thread.error());
    auto initialized_thread = application_vm->machine.initialize_java_thread(
        *lifecycle_thread, *receiver);
    if (!initialized_thread) return fail_start(initialized_thread.error());
    ApplicationVM* const lifecycle_vm = application_vm.get();
    auto scheduled_lifecycle = application_vm->machine.scheduler().start_native_thread(
        application_vm->machine,
        *lifecycle_thread,
        [lifecycle_vm, lifecycle_state, receiver = *receiver, main_class](
            std::stop_token stop_token)
            -> Result<std::optional<vm::ObjectRef>> {
            std::optional<Error> failure;
            vm::MidletSignal signal = vm::MidletSignal::none;
            if (!stop_token.stop_requested()) {
                auto started = lifecycle_vm->machine.invoke_instance(
                    receiver, main_class, "startApp", "()V", {},
                    kMidletLifecycleInstructionBudget,
                    vm::InstructionBudgetMode::progress_watchdog);
                if (!started) {
                    failure = started.error();
                } else {
                    auto completion = require_normal_completion(
                        lifecycle_vm->machine, *started, "MIDlet startApp");
                    if (!completion) failure = completion.error();
                }
                signal = lifecycle_vm->machine.consume_midlet_signal();
            }
            {
                std::scoped_lock state_lock(lifecycle_state->mutex);
                lifecycle_state->failure = std::move(failure);
                lifecycle_state->signal = signal;
            }
            lifecycle_state->completed.store(true, std::memory_order_release);
            return std::optional<vm::ObjectRef> {};
        });
    if (!scheduled_lifecycle) return fail_start(scheduled_lifecycle.error());
    start_app_deferred = true;
#else
    // MIDP lifecycle callbacks and LCDUI event dispatch run on independent
    // threads in phoneME. A number of legacy games keep startApp() on-stack
    // while an intro Canvas paints and waits for the first key. Running the
    // callback inline here deadlocks startup because the host cannot observe a
    // frame or deliver input until Runtime::start_midlet returns.
    const u64 initial_frame_generation = framebuffer_.metadata().generation;
    const u64 initial_ui_events = application_vm->ui_event_count.load(
        std::memory_order_acquire);
    auto lifecycle_state = std::make_shared<AsyncLifecycleState>();
    {
        std::scoped_lock state_lock(application_vm->lifecycle_state_mutex);
        application_vm->deferred_start_app = lifecycle_state;
    }
    auto lifecycle_thread_root = application_vm->machine.allocate_pinned_instance(
        "java/lang/Thread");
    if (!lifecycle_thread_root) {
        return fail_start(lifecycle_thread_root.error());
    }
    auto lifecycle_thread = lifecycle_thread_root->get();
    if (!lifecycle_thread) return fail_start(lifecycle_thread.error());
    auto initialized_thread = application_vm->machine.initialize_java_thread(
        *lifecycle_thread, *receiver);
    if (!initialized_thread) return fail_start(initialized_thread.error());
    ApplicationVM* const lifecycle_vm = application_vm.get();
    auto scheduled_lifecycle = application_vm->machine.scheduler().start_native_thread(
        application_vm->machine,
        *lifecycle_thread,
        [lifecycle_vm, lifecycle_state, receiver = *receiver, main_class](
            std::stop_token stop_token)
            -> Result<std::optional<vm::ObjectRef>> {
            std::optional<Error> failure;
            vm::MidletSignal signal = vm::MidletSignal::none;
            if (!stop_token.stop_requested()) {
                auto started = lifecycle_vm->machine.invoke_instance(
                    receiver, main_class, "startApp", "()V", {},
                    kMidletLifecycleInstructionBudget,
                    vm::InstructionBudgetMode::progress_watchdog);
                if (!started) {
                    failure = started.error();
                } else {
                    auto completion = require_normal_completion(
                        lifecycle_vm->machine, *started, "MIDlet startApp");
                    if (!completion) failure = completion.error();
                }
                signal = lifecycle_vm->machine.consume_midlet_signal();
            }
            {
                std::scoped_lock state_lock(lifecycle_state->mutex);
                lifecycle_state->failure = std::move(failure);
                lifecycle_state->signal = signal;
            }
            lifecycle_state->completed.store(true, std::memory_order_release);
            return std::optional<vm::ObjectRef> {};
        });
    if (!scheduled_lifecycle) return fail_start(scheduled_lifecycle.error());

    for (;;) {
        if (lifecycle_state->completed.load(std::memory_order_acquire)) break;
        auto pumped = application_vm->canvas.try_pump();
        if (!pumped) return fail_start(pumped.error());
        const bool frame_observable =
            framebuffer_.metadata().generation != initial_frame_generation;
        const bool ui_observable =
            application_vm->ui_event_count.load(std::memory_order_acquire) !=
            initial_ui_events;
        if ((frame_observable || ui_observable) &&
            !lifecycle_state->completed.load(std::memory_order_acquire)) {
            start_app_deferred = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!start_app_deferred) {
        std::optional<Error> lifecycle_failure;
        {
            std::scoped_lock state_lock(lifecycle_state->mutex);
            lifecycle_failure = std::move(lifecycle_state->failure);
            launch_signal = lifecycle_state->signal;
        }
        {
            std::scoped_lock state_lock(application_vm->lifecycle_state_mutex);
            application_vm->deferred_start_app.reset();
        }
        if (lifecycle_failure.has_value()) {
            return fail_start(std::move(*lifecycle_failure));
        }
    }
#endif
    std::string launch_console;
    std::vector<vm::NativeMethodInvocationCount> launch_native_counts;
    if (launch_signal == vm::MidletSignal::destroyed) {
        append_utf8(launch_console,
                    application_vm->machine.console_output());
        launch_native_counts = nonzero_native_invocation_counts(
            application_vm->machine);
    }
    std::shared_ptr<ApplicationVM> previous_vm;
    AppId previous_id;
    {
        std::unique_lock lock(mutex_);
        App* app = find_app_unlocked(app_id);
        if (app == nullptr || app->vm != application_vm ||
            app->lifecycle_token != lifecycle_token) {
            return fail(ErrorCode::invalid_state,
                        "MIDlet launch was superseded");
        }
        if (launch_signal != vm::MidletSignal::destroyed &&
            foreground_app_id_.valid() && foreground_app_id_ != app_id) {
            previous_id = foreground_app_id_;
            const App* previous = find_app_unlocked(previous_id);
            if (previous != nullptr) previous_vm = previous->vm;
        }
    }

    if (previous_vm != nullptr) {
        std::scoped_lock previous_operation(previous_vm->operation_mutex);
        // A real MIDlet-to-MIDlet foreground handoff is an LCDUI visibility
        // transition, not merely a host presentation detach. Deliver
        // hideNotify while leaving the isolate, timers, sockets and game logic
        // running; app_id == 0 below remains the render-only detach path.
        auto hidden = previous_vm->canvas.set_host_foreground(false);
        if (!hidden) return fail_start(hidden.error());
        auto pumped = previous_vm->canvas.pump();
        if (!pumped) return fail_start(pumped.error());
    }
    if (launch_signal == vm::MidletSignal::paused) {
        application_vm->machine.media().suspend();
    } else if (launch_signal != vm::MidletSignal::destroyed) {
        application_vm->machine.media().resume();
    }
    if (launch_signal != vm::MidletSignal::destroyed) {
        application_vm->machine.scheduler().set_host_foreground(true);
        auto resized = framebuffer_.resize(dimensions);
        if (!resized) return fail_start(resized.error());
        auto foregrounded = application_vm->canvas.set_host_foreground(true);
        if (!foregrounded) return fail_start(foregrounded.error());
        // On Web, startApp is deliberately left running on a lifecycle pthread
        // before this entrypoint returns. A busy startApp can own Machine's
        // execution gate for an arbitrary amount of time; a blocking host pump
        // here therefore turns an otherwise successful launch into a deadlock
        // (the browser cannot observe/present anything until start_midlet
        // returns). Native hosts can keep the eager blocking pump, while Web
        // publishes whatever complete frame is already available and lets the
        // worker render loop retry Canvas work on its next tick.
#if defined(PHONEME_WEB)
        auto pumped = application_vm->canvas.try_pump();
#else
        auto pumped = application_vm->canvas.pump();
#endif
        if (!pumped) return fail_start(pumped.error());
    }

    u64 event_generation = 0;
    {
        std::unique_lock lock(mutex_);
        App* app = find_app_unlocked(app_id);
        if (app == nullptr || app->vm != application_vm ||
            app->lifecycle_token != lifecycle_token) {
            return fail(ErrorCode::invalid_state,
                        "MIDlet launch was superseded before commit");
        }
        app->lifecycle_busy = false;
        if (launch_signal == vm::MidletSignal::destroyed) {
            app->state = AppState::destroyed;
            app->console_output = std::move(launch_console);
            app->native_invocation_counts = std::move(launch_native_counts);
            app->vm.reset();
            app_frame_pacing_.erase(app_id.value);
            app_heap_configs_.erase(app_id.value);
        } else if (launch_signal == vm::MidletSignal::paused) {
            app->state = AppState::paused;
            foreground_app_id_ = app_id;
        } else {
            app->state = AppState::active;
            foreground_app_id_ = app_id;
        }
        app->error_message.clear();
        app->generation = ++sequence_;
        event_generation = sequence_;
        last_exit_code_ = 0;
        last_error_message_.clear();
    }

    // This is lifecycle diagnostics, not an LCDUI reset. Kind NONE keeps it
    // observable to compatibility tooling without invalidating the screen that
    // startApp just published.
    ui_queue_.push(UiEvent {
        .kind = 0,
        .component_id = app_id.value,
        .generation = event_generation,
        .detail = launch_signal == vm::MidletSignal::destroyed
            ? "MIDlet notified destruction from startApp"
            : (launch_signal == vm::MidletSignal::paused
                   ? "MIDlet notified pause from startApp"
                   : (start_app_deferred
                          ? "MIDlet became observable while startApp continues on the lifecycle thread"
                          : "MIDlet constructor and startApp completed in the C++ VM")),
    });
    signal_host_wake();
    if (suite.managed && !start_app_deferred) {
        auto verified_classes =
            application_vm->classes.verified_classes(suite.jar_path);
        std::scoped_lock lock(mutex_);
        // This cache is an optimization only. A storage error must never turn
        // a successfully started MIDlet into a launch failure; the next start
        // simply verifies the affected classes again. Deferred startApp owns
        // the class repository concurrently, so its verification snapshot is
        // persisted by finalize_deferred_start() after the lifecycle thread
        // has completed instead of blocking the host launch here.
        static_cast<void>(suite_store_.update_verified_classes(
            suite_id,
            vm::ClassRepository::verification_cache_version(),
            verified_classes));
    }
    return {};
}

Status Runtime::set_foreground(AppId app_id, Dimensions dimensions) {
    // app_id == 0 is the host-level "detach visible application" operation.
    // It is intentionally presentation-only: the MIDlet stays ACTIVE, Java
    // workers/timers/sockets/media keep running at their normal rate, and no
    // hideNotify()/showNotify() lifecycle edge is exposed to the game.
    if (!app_id.valid()) {
        std::shared_ptr<ApplicationVM> hidden_vm;
        AppId hidden_id;
        u64 hidden_token = 0;
        {
            std::unique_lock lock(mutex_);
            auto running = require_running_unlocked();
            if (!running) return running;
            if (!foreground_app_id_.valid()) {
                framebuffer_.clear();
                return {};
            }
            hidden_id = foreground_app_id_;
            App* hidden = find_app_unlocked(hidden_id);
            if (hidden == nullptr || hidden->vm == nullptr ||
                hidden->state == AppState::destroyed) {
                foreground_app_id_ = {};
                framebuffer_.clear();
                return {};
            }
            if (hidden->lifecycle_busy) {
                return fail(ErrorCode::invalid_state,
                            "foreground application is busy");
            }
            hidden->lifecycle_busy = true;
            hidden_token = hidden->lifecycle_token = ++sequence_;
            hidden_vm = hidden->vm;
        }

        const auto fail_hide = [&](Error error) -> Status {
            std::unique_lock lock(mutex_);
            App* hidden = find_app_unlocked(hidden_id);
            if (hidden != nullptr && hidden->vm == hidden_vm &&
                hidden->lifecycle_token == hidden_token) {
                hidden->state = AppState::error;
                hidden->lifecycle_busy = false;
                hidden->generation = ++sequence_;
            }
            last_exit_code_ = -1;
            return std::unexpected(std::move(error));
        };

        {
            std::scoped_lock hidden_operation(hidden_vm->operation_mutex);
            auto hidden = hidden_vm->canvas.set_host_rendering_enabled(false);
            if (!hidden) return fail_hide(hidden.error());
        }

        std::unique_lock lock(mutex_);
        App* hidden = find_app_unlocked(hidden_id);
        if (hidden == nullptr || hidden->vm != hidden_vm ||
            hidden->lifecycle_token != hidden_token) {
            return fail(ErrorCode::invalid_state,
                        "foreground detach was superseded");
        }
        hidden->lifecycle_busy = false;
        hidden->generation = ++sequence_;
        foreground_app_id_ = {};
        framebuffer_.clear();
        last_exit_code_ = 0;
        return {};
    }
    if (!dimensions.valid()) {
        return fail(ErrorCode::invalid_argument,
                    "invalid foreground application dimensions");
    }

    std::shared_ptr<ApplicationVM> target_vm;
    std::shared_ptr<ApplicationVM> previous_vm;
    AppId previous_id;
    u64 target_token = 0;
    u64 previous_token = 0;
    bool target_media_active = false;
    {
        std::unique_lock lock(mutex_);
        auto running = require_running_unlocked();
        if (!running) return running;
        App* app = find_app_unlocked(app_id);
        if (app == nullptr || app->state == AppState::destroyed ||
            app->vm == nullptr) {
            return fail(ErrorCode::invalid_argument,
                        "foreground application does not exist");
        }
        if (app->lifecycle_busy) {
            return fail(ErrorCode::invalid_state,
                        "foreground application is busy");
        }
        app->lifecycle_busy = true;
        target_token = app->lifecycle_token = ++sequence_;
        target_vm = app->vm;
        target_media_active = app->state == AppState::active;

        if (foreground_app_id_.valid() && foreground_app_id_ != app_id) {
            previous_id = foreground_app_id_;
            App* previous = find_app_unlocked(previous_id);
            if (previous != nullptr && previous->vm != nullptr) {
                if (previous->lifecycle_busy) {
                    app->lifecycle_busy = false;
                    return fail(ErrorCode::invalid_state,
                                "previous foreground application is busy");
                }
                previous->lifecycle_busy = true;
                previous_token = previous->lifecycle_token = ++sequence_;
                previous_vm = previous->vm;
            }
        }
    }

    const auto finish_failure = [&](Error error,
                                    bool previous_failed) -> Status {
        std::unique_lock lock(mutex_);
        App* target = find_app_unlocked(app_id);
        if (target != nullptr && target->vm == target_vm &&
            target->lifecycle_token == target_token) {
            target->state = AppState::error;
            target->lifecycle_busy = false;
            target->generation = ++sequence_;
        }
        if (previous_vm != nullptr) {
            App* previous = find_app_unlocked(previous_id);
            if (previous != nullptr && previous->vm == previous_vm &&
                previous->lifecycle_token == previous_token) {
                if (previous_failed) {
                    previous->state = AppState::error;
                    previous->generation = ++sequence_;
                }
                previous->lifecycle_busy = false;
            }
        }
        last_exit_code_ = -1;
        return std::unexpected(std::move(error));
    };

    std::scoped_lock target_operation(target_vm->operation_mutex);
    std::unique_lock<std::recursive_mutex> previous_operation;
    if (previous_vm != nullptr) {
        previous_operation = std::unique_lock<std::recursive_mutex>(
            previous_vm->operation_mutex);
        auto hidden = previous_vm->canvas.set_host_foreground(false);
        if (!hidden) return finish_failure(hidden.error(), true);
        auto pumped = previous_vm->canvas.pump();
        if (!pumped) return finish_failure(pumped.error(), true);
    }
    target_vm->machine.scheduler().set_host_foreground(true);
    if (target_media_active) {
        target_vm->machine.media().resume();
    } else {
        target_vm->machine.media().suspend();
    }
    auto canvas_resized = target_vm->canvas.set_dimensions(dimensions);
    if (!canvas_resized) {
        return finish_failure(canvas_resized.error(), false);
    }
    auto resized = framebuffer_.resize(dimensions);
    if (!resized) return finish_failure(resized.error(), false);
    auto foregrounded = target_vm->canvas.set_host_foreground(true);
    if (!foregrounded) return finish_failure(foregrounded.error(), false);
    auto rendering = target_vm->canvas.set_host_rendering_enabled(true);
    if (!rendering) return finish_failure(rendering.error(), false);
    auto pumped = target_vm->canvas.pump();
    if (!pumped) return finish_failure(pumped.error(), false);
    auto replayed = vm::replay_current_lcdui(target_vm->machine);
    if (!replayed) return finish_failure(replayed.error(), false);

    std::unique_lock lock(mutex_);
    App* target = find_app_unlocked(app_id);
    if (target == nullptr || target->vm != target_vm ||
        target->lifecycle_token != target_token) {
        return fail(ErrorCode::invalid_state,
                    "foreground switch was superseded");
    }
    target->dimensions = dimensions;
    target->generation = ++sequence_;
    target->lifecycle_busy = false;
    if (previous_vm != nullptr) {
        App* previous = find_app_unlocked(previous_id);
        if (previous != nullptr && previous->vm == previous_vm &&
            previous->lifecycle_token == previous_token) {
            previous->lifecycle_busy = false;
        }
    }
    foreground_app_id_ = app_id;
    last_exit_code_ = 0;
    return {};
}

Status Runtime::pause_midlet(AppId app_id) {
    std::shared_ptr<ApplicationVM> vm;
    std::string main_class;
    bool was_foreground = false;
    u64 lifecycle_token = 0;
    {
        std::unique_lock lock(mutex_);
        App* app = find_app_unlocked(app_id);
        if (app == nullptr || app->state == AppState::destroyed ||
            app->vm == nullptr) {
            return fail(ErrorCode::invalid_argument,
                        "application does not exist");
        }
        if (app->state == AppState::paused) return {};
        if (app->lifecycle_busy) {
            return fail(ErrorCode::invalid_state,
                        "application lifecycle operation is already running");
        }
        app->lifecycle_busy = true;
        lifecycle_token = app->lifecycle_token = ++sequence_;
        vm = app->vm;
        main_class = app->main_class;
        was_foreground = foreground_app_id_ == app_id;
    }

    const auto fail_lifecycle = [&](Error error) -> Status {
        std::unique_lock lock(mutex_);
        App* app = find_app_unlocked(app_id);
        if (app != nullptr && app->vm == vm &&
            app->lifecycle_token == lifecycle_token) {
            app->state = AppState::error;
            app->lifecycle_busy = false;
            app->generation = ++sequence_;
            last_exit_code_ = -1;
        }
        return std::unexpected(std::move(error));
    };

    std::scoped_lock vm_operation(vm->operation_mutex);
    auto paused = vm->machine.invoke_instance(
        vm->midlet,
        main_class,
        "pauseApp",
        "()V",
        {},
        kMidletLifecycleInstructionBudget,
        vm::InstructionBudgetMode::progress_watchdog);
    if (!paused) return fail_lifecycle(paused.error());
    auto completion = require_normal_completion(vm->machine,
                                                *paused,
                                                "MIDlet pauseApp");
    if (!completion) return fail_lifecycle(completion.error());

    const vm::MidletSignal signal = vm->machine.consume_midlet_signal();
    if (was_foreground && signal != vm::MidletSignal::resume_requested) {
        auto hidden = vm->canvas.set_host_foreground(false);
        if (!hidden) return fail_lifecycle(hidden.error());
        auto pumped = vm->canvas.pump();
        if (!pumped) return fail_lifecycle(pumped.error());
        vm->machine.scheduler().set_host_foreground(false);
    }
    if (signal == vm::MidletSignal::resume_requested) {
        vm->machine.media().resume();
    } else if (signal != vm::MidletSignal::destroyed) {
        vm->machine.media().suspend();
    }

    std::unique_lock lock(mutex_);
    App* app = find_app_unlocked(app_id);
    if (app == nullptr || app->vm != vm ||
        app->lifecycle_token != lifecycle_token) {
        return fail(ErrorCode::invalid_state,
                    "application lifecycle operation was superseded");
    }
    app->lifecycle_busy = false;
    if (signal == vm::MidletSignal::destroyed) {
        app->state = AppState::destroyed;
        app->vm.reset();
        app_frame_pacing_.erase(app_id.value);
        app_heap_configs_.erase(app_id.value);
        if (foreground_app_id_ == app_id) {
            foreground_app_id_ = {};
            framebuffer_.clear();
        }
    } else if (signal == vm::MidletSignal::resume_requested) {
        app->state = AppState::active;
    } else {
        app->state = AppState::paused;
    }
    app->generation = ++sequence_;
    return {};
}

Status Runtime::resume_midlet(AppId app_id) {
    std::shared_ptr<ApplicationVM> vm;
    std::string main_class;
    bool was_foreground = false;
    u64 lifecycle_token = 0;
    {
        std::unique_lock lock(mutex_);
        App* app = find_app_unlocked(app_id);
        if (app == nullptr || app->state == AppState::destroyed ||
            app->vm == nullptr) {
            return fail(ErrorCode::invalid_argument,
                        "application does not exist");
        }
        if (app->state == AppState::error) {
            return fail(ErrorCode::invalid_state,
                        "application cannot resume from the error state");
        }
        if (app->state == AppState::active) return {};
        if (app->lifecycle_busy) {
            return fail(ErrorCode::invalid_state,
                        "application lifecycle operation is already running");
        }
        app->lifecycle_busy = true;
        lifecycle_token = app->lifecycle_token = ++sequence_;
        vm = app->vm;
        main_class = app->main_class;
        was_foreground = foreground_app_id_ == app_id;
    }

    const auto fail_lifecycle = [&](Error error) -> Status {
        std::unique_lock lock(mutex_);
        App* app = find_app_unlocked(app_id);
        if (app != nullptr && app->vm == vm &&
            app->lifecycle_token == lifecycle_token) {
            app->state = AppState::error;
            app->lifecycle_busy = false;
            app->generation = ++sequence_;
            last_exit_code_ = -1;
        }
        return std::unexpected(std::move(error));
    };

    std::scoped_lock vm_operation(vm->operation_mutex);
    auto resumed = vm->machine.invoke_instance(
        vm->midlet,
        main_class,
        "startApp",
        "()V",
        {},
        kMidletLifecycleInstructionBudget,
        vm::InstructionBudgetMode::progress_watchdog);
    if (!resumed) return fail_lifecycle(resumed.error());
    auto completion = require_normal_completion(vm->machine,
                                                *resumed,
                                                "MIDlet startApp");
    if (!completion) return fail_lifecycle(completion.error());

    const vm::MidletSignal signal = vm->machine.consume_midlet_signal();
    const bool active = signal != vm::MidletSignal::destroyed &&
                        signal != vm::MidletSignal::paused;
    if (signal == vm::MidletSignal::paused) {
        vm->machine.media().suspend();
    } else if (signal != vm::MidletSignal::destroyed) {
        vm->machine.media().resume();
    }
    if (was_foreground && signal != vm::MidletSignal::destroyed) {
        vm->machine.scheduler().set_host_foreground(active);
        auto visible = vm->canvas.set_host_foreground(active);
        if (!visible) return fail_lifecycle(visible.error());
        auto pumped = vm->canvas.pump();
        if (!pumped) return fail_lifecycle(pumped.error());
    }

    std::unique_lock lock(mutex_);
    App* app = find_app_unlocked(app_id);
    if (app == nullptr || app->vm != vm ||
        app->lifecycle_token != lifecycle_token) {
        return fail(ErrorCode::invalid_state,
                    "application lifecycle operation was superseded");
    }
    app->lifecycle_busy = false;
    if (signal == vm::MidletSignal::destroyed) {
        app->state = AppState::destroyed;
        app->vm.reset();
        app_frame_pacing_.erase(app_id.value);
        app_heap_configs_.erase(app_id.value);
        if (foreground_app_id_ == app_id) {
            foreground_app_id_ = {};
            framebuffer_.clear();
        }
    } else if (signal == vm::MidletSignal::paused) {
        app->state = AppState::paused;
    } else {
        app->state = AppState::active;
    }
    app->generation = ++sequence_;
    return {};
}

Status Runtime::destroy_midlet(AppId app_id) {
    std::shared_ptr<ApplicationVM> vm;
    std::string main_class;
    bool was_foreground = false;
    u64 lifecycle_token = 0;
    {
        std::unique_lock lock(mutex_);
        App* app = find_app_unlocked(app_id);
        if (app == nullptr) {
            return fail(ErrorCode::invalid_argument,
                        "application does not exist");
        }
        if (app->state == AppState::destroyed || app->vm == nullptr) {
            app_frame_pacing_.erase(app_id.value);
            app_heap_configs_.erase(app_id.value);
            return {};
        }
        if (app->lifecycle_busy) {
            return fail(ErrorCode::invalid_state,
                        "application lifecycle operation is already running");
        }
        app->lifecycle_busy = true;
        lifecycle_token = app->lifecycle_token = ++sequence_;
        vm = app->vm;
        main_class = app->main_class;
        was_foreground = foreground_app_id_ == app_id;
    }

    std::scoped_lock vm_operation(vm->operation_mutex);
    if (was_foreground) {
        auto hidden = vm->canvas.set_host_foreground(false);
        if (!hidden) {
            append_ascii_console(
                vm->machine,
                "[Lifecycle] forced destroy ignored foreground hide failure: " +
                    hidden.error().message);
        }
        auto pumped = vm->canvas.pump();
        if (!pumped) {
            append_ascii_console(
                vm->machine,
                "[Lifecycle] forced destroy ignored Canvas callback failure: " +
                    pumped.error().message);
        }
        vm->machine.scheduler().set_host_foreground(false);
    }

    // Forced termination must not let a MIDlet-owned worker keep the emulator
    // blocked forever from destroyApp(true), for example by joining a game
    // thread whose run loop only exits when the VM is stopped. Close blocking
    // I/O first, then stop and join scheduler/media/timer workers. The scheduler
    // main thread remains available, so destroyApp(true) can still perform
    // final RMS/state cleanup; attempts to spawn new work are rejected.
    trace_forced_destroy("close-connections-begin");
    vm->machine.close_connections();
    trace_forced_destroy("machine-shutdown-begin");
    vm->machine.shutdown();
    trace_forced_destroy("machine-shutdown-complete");

    const vm::Value unconditional = vm::Value::from_int(1);
    auto destroyed = vm->machine.invoke_instance(
        vm->midlet,
        main_class,
        "destroyApp",
        "(Z)V",
        std::span<const vm::Value>(&unconditional, 1),
        kForcedDestroyInstructionBudget);
    trace_forced_destroy("destroy-callback-complete");
    if (!destroyed) {
        std::string diagnostic =
            "[Lifecycle] forced destroy ignored callback failure: ";
        if (!destroyed.error().java_exception_class.empty()) {
            diagnostic += destroyed.error().java_exception_class + ": ";
        }
        diagnostic += destroyed.error().message;
        append_ascii_console(vm->machine, diagnostic);
    } else if (!destroyed->completed_normally()) {
        std::string throwable_name = "java/lang/Throwable";
        if (destroyed->throwable.has_value()) {
            auto throwable_class = vm->machine.heap().class_name(
                *destroyed->throwable);
            if (throwable_class) throwable_name = std::move(*throwable_class);
        }
        std::string diagnostic =
            "[Lifecycle] forced destroy ignored " + throwable_name;
        if (!destroyed->exception_context.empty()) {
            diagnostic += " from " + destroyed->exception_context;
        }
        append_ascii_console(vm->machine, diagnostic);
    }

    std::string console_output;
    append_utf8(console_output, vm->machine.console_output());

    std::unique_lock lock(mutex_);
    App* app = find_app_unlocked(app_id);
    if (app == nullptr || app->vm != vm ||
        app->lifecycle_token != lifecycle_token) {
        return fail(ErrorCode::invalid_state,
                    "application lifecycle operation was superseded");
    }
    app->state = AppState::destroyed;
    app->lifecycle_busy = false;
    app->generation = ++sequence_;
    app->console_output = std::move(console_output);
    app->vm.reset();
    app_frame_pacing_.erase(app_id.value);
    app_heap_configs_.erase(app_id.value);
    if (foreground_app_id_ == app_id) {
        foreground_app_id_ = {};
        framebuffer_.clear();
    }
    return {};
}

Status Runtime::set_push_background_policy(
    SuiteId suite_id,
    push::BackgroundPolicy policy) {
    std::string push_root;
    {
        std::scoped_lock lock(mutex_);
        if (!configured_) {
            return fail(ErrorCode::not_configured,
                        "runtime must be configured before PushRegistry access");
        }
        if (!suite_id.valid() || suite_store_.find(suite_id) == nullptr) {
            return fail(ErrorCode::invalid_argument,
                        "PushRegistry suite ID does not exist");
        }
        push_root = runtime_home_ + "/push";
    }

    push::PushRegistry registry;
    auto configured = registry.configure(std::move(push_root), suite_id);
    if (!configured) return configured;
    return registry.set_background_policy(policy);
}

Status Runtime::notify_push_connection_available(
    SuiteId suite_id,
    std::string connection,
    i64 received_at_millis) {
    std::string push_root;
    {
        std::scoped_lock lock(mutex_);
        if (!configured_) {
            return fail(ErrorCode::not_configured,
                        "runtime must be configured before PushRegistry access");
        }
        if (!suite_id.valid() || suite_store_.find(suite_id) == nullptr) {
            return fail(ErrorCode::invalid_argument,
                        "PushRegistry suite ID does not exist");
        }
        push_root = runtime_home_ + "/push";
    }

    push::PushRegistry registry;
    auto configured = registry.configure(std::move(push_root), suite_id);
    if (!configured) return configured;
    return registry.notify_connection_available(connection,
                                                 received_at_millis);
}

Status Runtime::notify_push_connection_available(
    SuiteId suite_id,
    std::string connection,
    std::string source_address,
    i64 received_at_millis) {
    std::string push_root;
    {
        std::scoped_lock lock(mutex_);
        if (!configured_) {
            return fail(ErrorCode::not_configured,
                        "runtime must be configured before PushRegistry access");
        }
        if (!suite_id.valid() || suite_store_.find(suite_id) == nullptr) {
            return fail(ErrorCode::invalid_argument,
                        "PushRegistry suite ID does not exist");
        }
        push_root = runtime_home_ + "/push";
    }

    push::PushRegistry registry;
    auto configured = registry.configure(std::move(push_root), suite_id);
    if (!configured) return configured;
    return registry.notify_connection_available(connection,
                                                 source_address,
                                                 received_at_millis);
}

Result<std::vector<push::LaunchRequest>>
Runtime::poll_push_launch_requests(
    SuiteId suite_id,
    i64 now_millis,
    bool background_execution_granted,
    usize limit) {
    std::string push_root;
    bool suite_is_foreground = false;
    {
        std::scoped_lock lock(mutex_);
        if (!configured_) {
            return fail(ErrorCode::not_configured,
                        "runtime must be configured before PushRegistry access");
        }
        if (!suite_id.valid() || suite_store_.find(suite_id) == nullptr) {
            return fail(ErrorCode::invalid_argument,
                        "PushRegistry suite ID does not exist");
        }
        push_root = runtime_home_ + "/push";
        if (!suspended_ && foreground_app_id_.valid()) {
            const App* foreground = find_app_unlocked(foreground_app_id_);
            suite_is_foreground = foreground != nullptr &&
                                  foreground->suite_id == suite_id &&
                                  foreground->state == AppState::active;
        }
    }

    push::PushRegistry registry;
    auto configured = registry.configure(std::move(push_root), suite_id);
    if (!configured) return std::unexpected(configured.error());
    return registry.eligible_launch_requests(now_millis,
                                             suite_is_foreground,
                                             background_execution_granted,
                                             limit);
}

Status Runtime::acknowledge_push_launch_request(
    SuiteId suite_id,
    u64 request_id) {
    std::string push_root;
    {
        std::scoped_lock lock(mutex_);
        if (!configured_) {
            return fail(ErrorCode::not_configured,
                        "runtime must be configured before PushRegistry access");
        }
        if (!suite_id.valid() || suite_store_.find(suite_id) == nullptr) {
            return fail(ErrorCode::invalid_argument,
                        "PushRegistry suite ID does not exist");
        }
        push_root = runtime_home_ + "/push";
    }

    push::PushRegistry registry;
    auto configured = registry.configure(std::move(push_root), suite_id);
    if (!configured) return configured;
    return registry.acknowledge_launch_request(request_id);
}

AppState Runtime::app_state(AppId app_id) noexcept {
    std::shared_ptr<ApplicationVM> vm;
    {
        std::scoped_lock lock(mutex_);
        const App* app = find_app_unlocked(app_id);
        if (app == nullptr) return AppState::none;
        if (app->vm == nullptr || app->lifecycle_busy ||
            app->state == AppState::destroyed ||
            app->state == AppState::error) {
            return app->state;
        }
        vm = app->vm;
    }

    finalize_pending_destruction(app_id, vm);

    std::scoped_lock lock(mutex_);
    const App* app = find_app_unlocked(app_id);
    return app == nullptr ? AppState::none : app->state;
}

AppId Runtime::foreground_app_id() const noexcept {
    std::scoped_lock lock(mutex_);
    return foreground_app_id_;
}

i64 Runtime::app_used_memory(AppId app_id) const noexcept {
    std::shared_ptr<ApplicationVM> vm;
    usize estimated = 0;
    {
        std::scoped_lock lock(mutex_);
        const App* app = find_app_unlocked(app_id);
        if (app == nullptr) return -1;
        estimated = sizeof(App) + app->main_class.capacity();
        vm = app->vm;
    }
    if (vm != nullptr) {
        std::unique_lock vm_operation(vm->operation_mutex, std::try_to_lock);
        if (!vm_operation.owns_lock()) {
            return static_cast<i64>(std::min<usize>(
                estimated, static_cast<usize>(std::numeric_limits<i64>::max())));
        }
        const usize heap_bytes = vm->machine.heap().estimated_bytes();
        if (heap_bytes > std::numeric_limits<usize>::max() - estimated) {
            return std::numeric_limits<i64>::max();
        }
        estimated += heap_bytes;
        const usize canvas_bytes = vm->canvas.estimated_bytes();
        if (canvas_bytes > std::numeric_limits<usize>::max() - estimated) {
            return std::numeric_limits<i64>::max();
        }
        estimated += canvas_bytes;
        const usize graphics_bytes = vm->machine.graphics().estimated_bytes();
        if (graphics_bytes > std::numeric_limits<usize>::max() - estimated) {
            return std::numeric_limits<i64>::max();
        }
        estimated += graphics_bytes;
        const usize frame_scratch_bytes = vm->frame_rgba_scratch.capacity();
        if (frame_scratch_bytes > std::numeric_limits<usize>::max() - estimated) {
            return std::numeric_limits<i64>::max();
        }
        estimated += frame_scratch_bytes;
    }
    if (estimated > static_cast<usize>(std::numeric_limits<i64>::max())) {
        return std::numeric_limits<i64>::max();
    }
    return static_cast<i64>(estimated);
}

std::string Runtime::app_error_message(AppId app_id) const {
    std::scoped_lock lock(mutex_);
    const App* app = find_app_unlocked(app_id);
    return app == nullptr ? std::string {} : app->error_message;
}

std::vector<vm::NativeMethodInvocationCount>
Runtime::app_native_invocation_counts(AppId app_id) const {
    std::shared_ptr<ApplicationVM> application_vm;
    std::vector<vm::NativeMethodInvocationCount> retained;
    {
        std::scoped_lock lock(mutex_);
        const App* app = find_app_unlocked(app_id);
        if (app == nullptr) return {};
        application_vm = app->vm;
        retained = app->native_invocation_counts;
    }
    if (application_vm == nullptr) return retained;
    std::scoped_lock vm_operation(application_vm->operation_mutex);
    return application_vm->machine.natives().invocation_counts();
}

std::string Runtime::app_console_output(AppId app_id) const {
    std::shared_ptr<ApplicationVM> application_vm;
    std::string retained_console;
    {
        std::scoped_lock lock(mutex_);
        const App* app = find_app_unlocked(app_id);
        if (app == nullptr) return {};
        application_vm = app->vm;
        retained_console = app->console_output;
    }
    if (application_vm == nullptr) return retained_console;

    std::u16string console;
    {
        std::scoped_lock vm_operation(application_vm->operation_mutex);
        console = application_vm->machine.console_output();
    }
    std::string utf8;
    utf8.reserve(console.size());
    append_utf8(utf8, console);
    return utf8;
}

void Runtime::record_error(const Error& error) {
    std::scoped_lock lock(mutex_);
    last_error_message_ = format_error_message(error);
}

void Runtime::clear_error() {
    std::scoped_lock lock(mutex_);
    last_error_message_.clear();
}

std::string Runtime::last_error_message() const {
    std::scoped_lock lock(mutex_);
    return last_error_message_;
}

void Runtime::stop() noexcept {
    std::vector<App> stopping_apps;
    {
        std::unique_lock lock(mutex_);
        stopping_apps.reserve(apps_.size());
        for (auto& [id, app] : apps_) {
            (void)id;
            app.state = AppState::destroyed;
            app.lifecycle_busy = true;
            app.lifecycle_token = ++sequence_;
            app.generation = sequence_;
            stopping_apps.push_back(app);
        }
        apps_.clear();
        app_frame_pacing_.clear();
        app_heap_configs_.clear();
        permission_policies_.clear();
        input_queue_.clear();
        ui_queue_.clear();
        framebuffer_.clear();
        foreground_app_id_ = {};
        running_ = false;
        suspended_ = false;
        last_exit_code_ = 0;
    }

    // Never run Java or destroy a Machine while holding the Runtime state
    // mutex. MIDlet cleanup is best-effort during host shutdown; Machine's
    // scheduler cancellation guarantees a busy Java worker cannot hang here.
    const vm::Value unconditional = vm::Value::from_int(1);
    for (App& app : stopping_apps) {
        auto application_vm = std::move(app.vm);
        if (application_vm == nullptr) continue;
        {
            std::scoped_lock vm_operation(application_vm->operation_mutex);
            (void)application_vm->canvas.set_host_foreground(false);
            (void)application_vm->canvas.pump();
            application_vm->machine.scheduler().set_host_foreground(false);
            (void)application_vm->machine.invoke_instance(
                application_vm->midlet,
                app.main_class,
                "destroyApp",
                "(Z)V",
                std::span<const vm::Value>(&unconditional, 1));
        }
        // The VM owns operation_mutex. Its final shared_ptr must be released
        // only after the lock guard is gone; otherwise the guard unlocks a
        // mutex that was destroyed together with the VM.
        application_vm.reset();
    }
}

void Runtime::suspend() noexcept {
    std::shared_ptr<ApplicationVM> foreground_vm;
    AppId foreground_id;
    {
        std::unique_lock lock(mutex_);
        if (!running_ || suspended_) return;
        suspended_ = true;
        foreground_id = foreground_app_id_;
        const App* foreground = find_app_unlocked(foreground_id);
        if (foreground != nullptr) foreground_vm = foreground->vm;
    }

    // Host suspension is presentation-only. iOS may keep the process alive
    // through its background execution policy, so Java workers, sockets,
    // timers and media must keep their normal scheduling semantics. Suppress
    // only graphics work; do not expose hideNotify() to the MIDlet.
    if (foreground_vm != nullptr) {
        std::scoped_lock foreground_operation(foreground_vm->operation_mutex);
        auto hidden = foreground_vm->canvas.set_host_rendering_enabled(false);
        if (!hidden) {
            std::unique_lock lock(mutex_);
            App* foreground = find_app_unlocked(foreground_id);
            if (foreground != nullptr && foreground->vm == foreground_vm) {
                mark_canvas_failure_unlocked(*foreground, hidden.error());
            }
        }
    }

    // iOS can terminate the process at any point while suspended, so deferred
    // RMS writes must reach disk now or the player's last save dies with the
    // process. Java execution keeps running; later writes still see the normal
    // pending-flush window.
    flush_record_stores();
}

void Runtime::flush_record_stores() noexcept {
    std::vector<std::shared_ptr<ApplicationVM>> app_vms;
    {
        std::scoped_lock lock(mutex_);
        app_vms.reserve(apps_.size());
        for (auto& [app_id, app] : apps_) {
            (void)app_id;
            if (app.vm != nullptr) app_vms.push_back(app.vm);
        }
    }
    for (const auto& app_vm : app_vms) {
        auto flushed = app_vm->machine.record_stores().flush_all();
        if (!flushed) {
            std::fprintf(stderr,
                         "[phoneME] RMS flush failed: %s\n",
                         flushed.error().message.c_str());
            std::fflush(stderr);
        }
    }
}

void Runtime::resume() noexcept {
    std::shared_ptr<ApplicationVM> foreground_vm;
    AppId foreground_id;
    {
        std::unique_lock lock(mutex_);
        if (!running_ || !suspended_) return;
        suspended_ = false;
        foreground_id = foreground_app_id_;
        const App* foreground = find_app_unlocked(foreground_id);
        if (foreground != nullptr && foreground->vm != nullptr &&
            (foreground->state == AppState::active ||
             foreground->state == AppState::paused)) {
            foreground_vm = foreground->vm;
        }
    }

    if (foreground_vm != nullptr) {
        std::scoped_lock foreground_operation(foreground_vm->operation_mutex);
        auto shown = foreground_vm->canvas.set_host_rendering_enabled(true);
        auto pumped = shown ? foreground_vm->canvas.pump()
                            : Status(std::unexpected(shown.error()));
        if (!pumped) {
            std::unique_lock lock(mutex_);
            App* foreground = find_app_unlocked(foreground_id);
            if (foreground != nullptr && foreground->vm == foreground_vm) {
                mark_canvas_failure_unlocked(*foreground, pumped.error());
            }
        }
    }
}

bool Runtime::is_running() const noexcept {
    std::scoped_lock lock(mutex_);
    return running_;
}

bool Runtime::is_suspended() const noexcept {
    std::scoped_lock lock(mutex_);
    return suspended_;
}

i32 Runtime::last_exit_code() const noexcept {
    std::scoped_lock lock(mutex_);
    return last_exit_code_;
}

void Runtime::send_key(i32 key_code, bool pressed) {
    {
        std::unique_lock lock(mutex_);
        if (!running_ || suspended_) return;
        App* app = find_app_unlocked(foreground_app_id_);
        if (app == nullptr || app->vm == nullptr ||
            app->state != AppState::active) {
            return;
        }
        input_queue_.push(InputEvent {
            .kind = InputKind::key,
            .app_id = app->id,
            .app_generation = app->generation,
            .first = mapped_key_code_unlocked(key_code),
            .second = pressed ? 1 : 0,
            .sequence = ++sequence_,
        });
    }
    dispatch_input();
}

void Runtime::send_pointer(i32 x, i32 y, i32 action) {
    {
        std::unique_lock lock(mutex_);
        if (!running_ || suspended_) return;
        App* app = find_app_unlocked(foreground_app_id_);
        if (app == nullptr || app->vm == nullptr ||
            app->state != AppState::active) {
            return;
        }
        input_queue_.push(InputEvent {
            .kind = InputKind::pointer,
            .app_id = app->id,
            .app_generation = app->generation,
            .first = x,
            .second = y,
            .third = action,
            .sequence = ++sequence_,
        });
    }
    dispatch_input();
}

void Runtime::pump_events() {
    std::shared_ptr<ApplicationVM> vm;
    AppId app_id;
    u64 generation = 0;
    {
        std::unique_lock lock(mutex_);
        if (running_ && !suspended_) {
            app_id = foreground_app_id_;
            const App* app = find_app_unlocked(app_id);
            if (app != nullptr && app->vm != nullptr &&
                (app->state == AppState::active ||
                 app->state == AppState::paused)) {
                vm = app->vm;
                generation = app->generation;
            }
        }
    }
    if (vm == nullptr) return;

    {
        std::scoped_lock vm_operation(vm->operation_mutex);
        auto pumped = vm->canvas.try_pump();
        if (!pumped) {
            std::unique_lock lock(mutex_);
            App* app = find_app_unlocked(app_id);
            if (app != nullptr && app->vm == vm &&
                app->generation == generation) {
                mark_canvas_failure_unlocked(*app, pumped.error());
            }
        }
    }
    finalize_deferred_start(app_id, vm);
}

FrameMetadata Runtime::frame_metadata() {
    pump_events();
    return framebuffer_.metadata();
}

FrameMetadata Runtime::copy_current_frame_rgba(
    std::span<u8> destination) const noexcept {
    return framebuffer_.copy_rgba(destination);
}

std::optional<FrameMetadata> Runtime::copy_current_frame_rgba_since(
    u64 previous_generation,
    std::span<u8> destination) const noexcept {
    return framebuffer_.copy_rgba_since(previous_generation, destination);
}

std::optional<FrameReadView> Runtime::acquire_current_frame_rgba_since(
    u64 previous_generation) noexcept {
    frame_read_lease_.reset();
    auto lease = framebuffer_.acquire_rgba_since(previous_generation);
    if (!lease) return std::nullopt;
    const FrameMetadata metadata = lease->metadata();
    const auto pixels = lease->pixels();
    const auto damage_regions = lease->damage_regions();
    frame_read_lease_.emplace(std::move(*lease));
    return FrameReadView {
        .pixels = pixels.data(),
        .metadata = metadata,
        .damage_regions = damage_regions,
    };
}

std::optional<FrameReadView> Runtime::acquire_current_frame_native_since(
    u64 previous_generation) noexcept {
    frame_read_lease_.reset();
    auto lease = framebuffer_.acquire_native_since(previous_generation);
    if (!lease) return std::nullopt;
    const FrameMetadata metadata = lease->metadata();
    const auto pixels = lease->pixels();
    const auto damage_regions = lease->damage_regions();
    frame_read_lease_.emplace(std::move(*lease));
    return FrameReadView {
        .pixels = pixels.data(),
        .metadata = metadata,
        .damage_regions = damage_regions,
    };
}

void Runtime::release_current_frame_rgba() noexcept {
    frame_read_lease_.reset();
}

u64 Runtime::storage_generation() noexcept {
    std::shared_ptr<ApplicationVM> vm;
    {
        std::unique_lock lock(mutex_);
        const App* app = find_app_unlocked(foreground_app_id_);
        if (app != nullptr && app->vm != nullptr &&
            (app->state == AppState::active || app->state == AppState::paused)) {
            vm = app->vm;
        }
    }
    if (vm == nullptr) return 0;
    return vm->machine.filesystem().mutation_generation() +
           vm->machine.record_stores().mutation_generation();
}

FrameMetadata Runtime::copy_lcdui_image_rgba(
    i32 component_id,
    std::span<u8> destination) {
    std::shared_ptr<ApplicationVM> vm;
    {
        std::unique_lock lock(mutex_);
        if (!running_ || suspended_) return {};
        const App* app = find_app_unlocked(foreground_app_id_);
        if (app == nullptr || app->vm == nullptr ||
            (app->state != AppState::active &&
             app->state != AppState::paused)) {
            return {};
        }
        vm = app->vm;
    }

    std::scoped_lock vm_operation(vm->operation_mutex);
    auto source = resolve_lcdui_image_source(vm->machine, component_id);
    if (!source) return {};
    auto image = vm->machine.graphics().image(source->image.bits);
    if (!image) return {};

    const auto pixels = (*image)->pixels();
    const usize byte_count = pixels.size() * 4U;
    FrameMetadata metadata {
        .dimensions = {
            .width = (*image)->width(),
            .height = (*image)->height(),
        },
        .generation = source->generation,
        .byte_count = byte_count,
    };
    if (destination.size() < byte_count) return metadata;

    usize offset = 0U;
    for (const graphics::Pixel pixel : pixels) {
        const graphics::Pixel display_pixel =
            graphics::rgb565_roundtrip(pixel);
        destination[offset++] = graphics::red(display_pixel);
        destination[offset++] = graphics::green(display_pixel);
        destination[offset++] = graphics::blue(display_pixel);
        destination[offset++] = graphics::alpha(display_pixel);
    }
    return metadata;
}

FrameSnapshot Runtime::frame_snapshot() {
    (void)frame_metadata();
    return framebuffer_.snapshot();
}

std::optional<UiEvent> Runtime::poll_ui_event() {
    if (auto event = ui_queue_.pop()) return event;

    std::shared_ptr<ApplicationVM> vm;
    {
        std::unique_lock lock(mutex_);
        const App* app = find_app_unlocked(foreground_app_id_);
        if (app != nullptr && app->vm != nullptr &&
            (app->state == AppState::active ||
             app->state == AppState::paused)) {
            vm = app->vm;
        }
    }
    if (vm == nullptr || vm->machine.pending_serial_callbacks() == 0U) {
        return std::nullopt;
    }

    // Display.callSerially runs on a scheduler worker after the current LCDUI
    // callback returns. Preserve event ordering at the native bridge boundary:
    // a temporarily empty queue must not look terminal while that worker is
    // about to publish setCurrent()/screen events. Wait on the queue instead
    // of waking every 100 us; a push wakes us immediately and the 1 ms slice
    // still lets a callback that completes without publishing an event become
    // observable well before the 5 ms outer deadline.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    do {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining = deadline - now;
        const auto wait_slice = std::min(
            std::chrono::duration_cast<std::chrono::microseconds>(remaining),
            std::chrono::microseconds(1'000));
        if (auto event = ui_queue_.wait_pop_for(wait_slice)) return event;
    } while (vm->machine.pending_serial_callbacks() != 0U &&
             std::chrono::steady_clock::now() < deadline);

    return ui_queue_.pop();
}

void Runtime::ui_select_command(i32 command_id) {
    push_ui_action(100, command_id, 0, 0);
}

void Runtime::ui_select_list_item_command(i32 component_id,
                                          i32 element_index,
                                          i32 command_id) {
    push_ui_action(
        static_cast<i32>(vm::LcduiActionKind::select_list_item_command),
        component_id,
        element_index,
        command_id);
}

void Runtime::ui_focus_item(i32 component_id) {
    push_ui_action(101, component_id, 0, 0);
}

void Runtime::ui_activate_item(i32 component_id) {
    push_ui_action(102, component_id, 0, 0);
}

void Runtime::ui_set_text(i32 component_id,
                          std::string text,
                          i32 caret_position) {
    push_ui_action(103, component_id, caret_position, 0, std::move(text));
}

void Runtime::ui_set_choice(i32 component_id,
                            i32 element_index,
                            bool selected) {
    push_ui_action(104, component_id, element_index, selected ? 1 : 0);
}

void Runtime::ui_set_gauge(i32 component_id, i32 value) {
    push_ui_action(105, component_id, value, 0);
}

void Runtime::ui_set_date(i32 component_id, i64 unix_seconds) {
    push_ui_action(106, component_id, 0, unix_seconds);
}

void Runtime::ui_set_scroll_position(i32 position) {
    push_ui_action(107, 0, position, 0);
}

App* Runtime::find_app_unlocked(AppId app_id) noexcept {
    const auto iterator = apps_.find(app_id.value);
    return iterator == apps_.end() ? nullptr : &iterator->second;
}

const App* Runtime::find_app_unlocked(AppId app_id) const noexcept {
    const auto iterator = apps_.find(app_id.value);
    return iterator == apps_.end() ? nullptr : &iterator->second;
}

Status Runtime::require_running_unlocked() const {
    if (!running_) {
        return fail(ErrorCode::not_running, "runtime is not running");
    }
    return {};
}

i32 Runtime::mapped_key_code_unlocked(i32 host_key_code) const noexcept {
    switch (host_key_code) {
    case -1: return keymap_[0];
    case -2: return keymap_[1];
    case -3: return keymap_[2];
    case -4: return keymap_[3];
    case -5: return keymap_[4];
    case -6: return keymap_[5];
    case -7: return keymap_[6];
    default: return host_key_code;
    }
}

void Runtime::dispatch_input() {
    while (auto event = input_queue_.pop()) {
        std::shared_ptr<ApplicationVM> vm;
        {
            std::unique_lock lock(mutex_);
            if (!running_ || suspended_) return;
            const App* app = find_app_unlocked(event->app_id);
            if (app == nullptr || app->vm == nullptr ||
                app->generation != event->app_generation ||
                foreground_app_id_ != event->app_id ||
                app->state != AppState::active) {
                continue;
            }
            vm = app->vm;
        }

        {
            std::scoped_lock vm_operation(vm->operation_mutex);
            if (event->kind == InputKind::key) {
                vm->canvas.enqueue_host_key(event->first,
                                            event->second != 0,
                                            event->sequence);
            } else {
                vm->canvas.enqueue_pointer(event->first,
                                           event->second,
                                           event->third,
                                           event->sequence);
            }
            // Host input must not block behind a long Java render turn. The
            // event is already stored in CanvasRuntime's synchronized queue;
            // process it immediately only when the VM execution gate is free.
            // A busy VM will consume it on the next regular host pump.
            auto pumped = vm->canvas.try_pump();
            if (!pumped) {
                std::unique_lock lock(mutex_);
                App* app = find_app_unlocked(event->app_id);
                if (app != nullptr && app->vm == vm &&
                    app->generation == event->app_generation) {
                    mark_canvas_failure_unlocked(*app, pumped.error());
                }
                return;
            }
        }
        finalize_deferred_start(event->app_id, vm);
    }
}

void Runtime::finalize_deferred_start(
    AppId app_id,
    const std::shared_ptr<ApplicationVM>& vm) {
    if (vm == nullptr) return;

    std::shared_ptr<AsyncLifecycleState> state;
    {
        std::scoped_lock state_lock(vm->lifecycle_state_mutex);
        state = vm->deferred_start_app;
        if (state == nullptr ||
            !state->completed.load(std::memory_order_acquire)) {
            return;
        }
        vm->deferred_start_app.reset();
    }

    std::optional<Error> failure;
    vm::MidletSignal signal = vm::MidletSignal::none;
    {
        std::scoped_lock state_lock(state->mutex);
        failure = std::move(state->failure);
        signal = state->signal;
    }

    if (failure.has_value()) {
        const std::string diagnostic = format_error_message(*failure);
        std::unique_lock lock(mutex_);
        App* app = find_app_unlocked(app_id);
        if (app == nullptr || app->vm != vm) return;
        app->state = AppState::error;
        app->lifecycle_busy = false;
        app->error_message = diagnostic;
        app->generation = ++sequence_;
        last_exit_code_ = -1;
        last_error_message_ = diagnostic;
        ui_queue_.push(UiEvent {
            .kind = -1,
            .component_id = app_id.value,
            .generation = sequence_,
            .detail = "Deferred MIDlet startApp failed: " + diagnostic,
        });
        signal_host_wake();
        return;
    }

    Suite deferred_suite;
    bool cache_verified_classes = false;
    {
        std::unique_lock lock(mutex_);
        const App* app = find_app_unlocked(app_id);
        if (app != nullptr && app->vm == vm) {
            if (const Suite* suite = suite_store_.find(app->suite_id);
                suite != nullptr && suite->managed) {
                deferred_suite = *suite;
                cache_verified_classes = true;
            }
        }
    }
    if (cache_verified_classes) {
        auto verified_classes =
            vm->classes.verified_classes(deferred_suite.jar_path);
        std::scoped_lock lock(mutex_);
        static_cast<void>(suite_store_.update_verified_classes(
            deferred_suite.id,
            vm::ClassRepository::verification_cache_version(),
            verified_classes));
    }

    if (signal == vm::MidletSignal::none ||
        signal == vm::MidletSignal::resume_requested) {
        std::unique_lock lock(mutex_);
        App* app = find_app_unlocked(app_id);
        if (app != nullptr && app->vm == vm) {
            app->lifecycle_busy = false;
        }
        return;
    }

    bool was_foreground = false;
    {
        std::unique_lock lock(mutex_);
        const App* app = find_app_unlocked(app_id);
        if (app == nullptr || app->vm != vm) return;
        was_foreground = foreground_app_id_ == app_id;
    }

    std::optional<Error> transition_error;
    std::string console_output;
    std::vector<vm::NativeMethodInvocationCount> native_counts;
    {
        std::scoped_lock vm_operation(vm->operation_mutex);
        if (was_foreground) {
            auto hidden = vm->canvas.set_host_foreground(false);
            if (!hidden) {
                transition_error = hidden.error();
            } else {
                auto pumped = vm->canvas.pump();
                if (!pumped) transition_error = pumped.error();
            }
            vm->machine.scheduler().set_host_foreground(false);
        }
        vm->machine.media().suspend();
        if (signal == vm::MidletSignal::destroyed) {
            append_utf8(console_output, vm->machine.console_output());
            native_counts = nonzero_native_invocation_counts(vm->machine);
        }
    }

    std::unique_lock lock(mutex_);
    App* app = find_app_unlocked(app_id);
    if (app == nullptr || app->vm != vm) return;
    if (transition_error.has_value()) {
        mark_canvas_failure_unlocked(*app, *transition_error);
        app->lifecycle_busy = false;
        return;
    }

    app->lifecycle_busy = false;
    app->generation = ++sequence_;
    if (signal == vm::MidletSignal::destroyed) {
        app->state = AppState::destroyed;
        app->console_output = std::move(console_output);
        app->native_invocation_counts = std::move(native_counts);
        app->vm.reset();
        app_frame_pacing_.erase(app_id.value);
        app_heap_configs_.erase(app_id.value);
        if (foreground_app_id_ == app_id) {
            foreground_app_id_ = {};
            framebuffer_.clear();
        }
    } else {
        app->state = AppState::paused;
    }
    ui_queue_.push(UiEvent {
        .kind = 1,
        .component_id = app_id.value,
        .generation = sequence_,
        .detail = signal == vm::MidletSignal::destroyed
            ? "Deferred MIDlet startApp notified destruction"
            : "Deferred MIDlet startApp notified pause",
    });
    signal_host_wake();
}

void Runtime::finalize_pending_destruction(
    AppId app_id,
    const std::shared_ptr<ApplicationVM>& vm) noexcept {
    if (vm == nullptr) return;

    bool was_foreground = false;
    {
        std::scoped_lock lock(mutex_);
        const App* app = find_app_unlocked(app_id);
        if (app == nullptr || app->vm != vm || app->lifecycle_busy ||
            app->state == AppState::destroyed ||
            app->state == AppState::error) {
            return;
        }
        was_foreground = foreground_app_id_ == app_id;
    }

    std::string console_output;
    std::vector<vm::NativeMethodInvocationCount> native_counts;
    {
        std::scoped_lock vm_operation(vm->operation_mutex);
        if (!vm->machine.consume_midlet_destroyed_signal()) return;

        if (was_foreground) {
            auto hidden = vm->canvas.set_host_foreground(false);
            if (!hidden) {
                append_ascii_console(
                    vm->machine,
                    "[Lifecycle] self-destroy ignored foreground hide failure: " +
                        hidden.error().message);
            } else {
                auto pumped = vm->canvas.pump();
                if (!pumped) {
                    append_ascii_console(
                        vm->machine,
                        "[Lifecycle] self-destroy ignored Canvas callback failure: " +
                            pumped.error().message);
                }
            }
            vm->machine.scheduler().set_host_foreground(false);
        }
        vm->machine.media().suspend();
        vm->machine.close_connections();
        append_utf8(console_output, vm->machine.console_output());
        native_counts = nonzero_native_invocation_counts(vm->machine);
    }

    std::unique_lock lock(mutex_);
    App* app = find_app_unlocked(app_id);
    if (app == nullptr || app->vm != vm) return;

    app->state = AppState::destroyed;
    app->lifecycle_busy = false;
    app->generation = ++sequence_;
    app->console_output = std::move(console_output);
    app->native_invocation_counts = std::move(native_counts);
    app->vm.reset();
    app_frame_pacing_.erase(app_id.value);
    app_heap_configs_.erase(app_id.value);
    if (foreground_app_id_ == app_id) {
        foreground_app_id_ = {};
        framebuffer_.clear();
    }
    ui_queue_.push(UiEvent {
        .kind = 1,
        .component_id = app_id.value,
        .generation = sequence_,
        .detail = "MIDlet notified destruction",
    });
    signal_host_wake();
}

void Runtime::mark_canvas_failure_unlocked(App& app, const Error& error) {
    const std::string diagnostic = format_error_message(error);
    char message[1536] {};
    std::snprintf(message,
                  sizeof(message),
                  "phoneME app %d Canvas dispatcher failed: %s%s%s",
                  app.id.value,
                  error.java_exception_class.empty()
                      ? ""
                      : error.java_exception_class.c_str(),
                  error.java_exception_class.empty() ? "" : ": ",
                  error.message.c_str());
    std::fprintf(stderr, "%s\n", message);
#if defined(__APPLE__)
    os_log_error(OS_LOG_DEFAULT, "%{public}s", message);
#endif

    app.state = AppState::error;
    app.error_message = diagnostic;
    app.generation = ++sequence_;
    last_exit_code_ = -1;
    last_error_message_ = diagnostic;
    ui_queue_.push(UiEvent {
        .kind = -1,
        .component_id = app.id.value,
        .generation = sequence_,
        .detail = "Canvas dispatcher failed: " + diagnostic,
    });
    signal_host_wake();
}

void Runtime::push_ui_action(i32 kind,
                             i32 component_id,
                             i32 first,
                             i64 value64,
                             std::string text) {
    std::shared_ptr<ApplicationVM> vm;
    AppId app_id;
    u64 app_generation = 0;
    {
        std::unique_lock lock(mutex_);
        if (!running_ || suspended_) return;
        app_id = foreground_app_id_;
        const App* app = find_app_unlocked(app_id);
        if (app != nullptr && app->vm != nullptr &&
            (app->state == AppState::active ||
             app->state == AppState::paused)) {
            vm = app->vm;
            app_generation = app->generation;
        }
    }

    if (vm != nullptr) {
        std::scoped_lock vm_operation(vm->operation_mutex);
        auto handled = vm::handle_lcdui_action(vm->machine,
                                              kind,
                                              component_id,
                                              first,
                                              value64,
                                              text);
        if (handled) {
            auto pumped = vm->canvas.pump();
            if (!pumped) {
                std::unique_lock lock(mutex_);
                App* app = find_app_unlocked(app_id);
                if (app != nullptr && app->vm == vm &&
                    app->generation == app_generation) {
                    mark_canvas_failure_unlocked(*app, pumped.error());
                }
            }
            return;
        }
        std::unique_lock lock(mutex_);
        ui_queue_.push(UiEvent {
            .kind = kind,
            .component_id = component_id,
            .arguments = {first, 0, 0, 0},
            .value64 = value64,
            .generation = ++sequence_,
            .text = std::move(text),
            .detail = handled.error().message,
        });
        signal_host_wake();
        return;
    }

    std::unique_lock lock(mutex_);
    ui_queue_.push(UiEvent {
        .kind = kind,
        .component_id = component_id,
        .arguments = {first, 0, 0, 0},
        .value64 = value64,
        .generation = ++sequence_,
        .text = std::move(text),
    });
    signal_host_wake();
}

} // namespace phoneme::runtime
