#include "ImageNatives.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "GraphicsNativeSupport.hpp"
#include "phoneme/graphics/ImageDecoder.hpp"
#include "phoneme/vm/NativeMethodRegistry.hpp"

namespace phoneme::vm {
namespace {

using namespace graphics_native;

constexpr usize kImageWidthField = 0;
constexpr usize kImageHeightField = 1;
constexpr usize kImageMutableField = 2;
constexpr usize kGraphicsTargetField = 0;

void add(NativeMethodRegistry& registry,
         std::string owner,
         std::string name,
         std::string descriptor,
         NativeMethod method) {
    auto registered = registry.register_method(std::move(owner),
                                               std::move(name),
                                               std::move(descriptor),
                                               std::move(method));
    if (!registered) {
        std::terminate();
    }
}

[[nodiscard]] Result<ObjectRef> create_image_object(
    Machine& machine,
    graphics::Image image) {
    const i32 width = image.width();
    const i32 height = image.height();
    const bool mutable_image = image.is_mutable();
    auto object = machine.class_states().allocate_instance(
        machine.heap(), "javax/microedition/lcdui/Image");
    if (!object) return std::unexpected(object.error());
    auto width_stored = machine.heap().set_field(
        *object, kImageWidthField, Value::from_int(width));
    auto height_stored = machine.heap().set_field(
        *object, kImageHeightField, Value::from_int(height));
    auto mutable_stored = machine.heap().set_field(
        *object, kImageMutableField,
        Value::from_int(mutable_image ? 1 : 0));
    if (!width_stored) return std::unexpected(width_stored.error());
    if (!height_stored) return std::unexpected(height_stored.error());
    if (!mutable_stored) return std::unexpected(mutable_stored.error());
    auto attached = machine.graphics().attach_image(object->bits,
                                                    std::move(image));
    if (!attached) return graphics_error(attached.error());
    return *object;
}

[[nodiscard]] Result<ObjectRef> create_graphics_object(
    Machine& machine,
    ObjectRef image) {
    auto object = machine.class_states().allocate_instance(
        machine.heap(), "javax/microedition/lcdui/Graphics");
    if (!object) return std::unexpected(object.error());
    auto target_stored = machine.heap().set_field(
        *object, kGraphicsTargetField, Value::from_reference(image));
    if (!target_stored) return std::unexpected(target_stored.error());
    auto attached = machine.graphics().attach_context(object->bits,
                                                      image.bits);
    if (!attached) return std::unexpected(attached.error());
    return *object;
}

[[nodiscard]] Result<graphics::Image*> image_payload(Machine& machine,
                                                     ObjectRef image) {
    auto payload = machine.graphics().image(image.bits);
    if (!payload) return graphics_error(payload.error());
    return *payload;
}

[[nodiscard]] Result<graphics::Image> decode_image_bytes(
    std::span<const u8> bytes,
    std::string_view exception_class) {
    auto decoded = graphics::decode_image(bytes);
    if (!decoded) {
        return graphics_error(decoded.error(), exception_class);
    }
    return std::move(*decoded);
}

[[nodiscard]] Result<i32> invocation_int_result(
    Machine& machine,
    const ExecutionResult& result,
    std::string_view operation) {
    if (!result.completed_normally()) {
        if (!result.throwable.has_value()) {
            return fail(ErrorCode::internal_error,
                        std::string(operation) +
                            " failed without a Java throwable");
        }
        auto throwable = machine.heap().class_name(*result.throwable);
        if (!throwable) return std::unexpected(throwable.error());
        return fail_java(*throwable, std::string(operation) + " failed");
    }
    if (!result.return_value.has_value()) {
        return fail(ErrorCode::invalid_state,
                    std::string(operation) + " returned no value");
    }
    return result.return_value->as_int();
}

[[nodiscard]] Result<std::vector<u8>> read_input_stream(
    Machine& machine,
    ObjectRef input) {
    constexpr usize kChunkSize = 4U * 1024U;
    constexpr usize kMaximumImageBytes = 64U * 1024U * 1024U;

    auto input_root = machine.pin_native_root(input);
    if (!input_root) return std::unexpected(input_root.error());
    auto chunk = machine.heap().allocate_array(
        "[B", kChunkSize, Value::from_int(0));
    if (!chunk) return std::unexpected(chunk.error());
    auto chunk_root = machine.pin_native_root(*chunk);
    if (!chunk_root) return std::unexpected(chunk_root.error());

    std::vector<u8> bytes;
    bytes.reserve(kChunkSize);
    for (;;) {
        const std::array<Value, 3> arguments {
            Value::from_reference(*chunk),
            Value::from_int(0),
            Value::from_int(static_cast<i32>(kChunkSize)),
        };
        auto invoked = machine.invoke_instance(
            input,
            "java/io/InputStream",
            "read",
            "([BII)I",
            arguments,
            1'000'000);
        if (!invoked) return std::unexpected(invoked.error());
        auto count = invocation_int_result(
            machine, *invoked, "Image.createImage InputStream.read");
        if (!count) return std::unexpected(count.error());
        if (*count < 0) break;
        if (*count == 0) {
            auto one = machine.invoke_instance(
                input,
                "java/io/InputStream",
                "read",
                "()I",
                {},
                1'000'000);
            if (!one) return std::unexpected(one.error());
            auto value = invocation_int_result(
                machine, *one, "Image.createImage InputStream.read");
            if (!value) return std::unexpected(value.error());
            if (*value < 0) break;
            if (bytes.size() >= kMaximumImageBytes) {
                return fail_java("java/io/IOException",
                                 "image stream exceeds 64 MiB");
            }
            bytes.push_back(static_cast<u8>(*value & 0xFF));
            continue;
        }
        if (static_cast<usize>(*count) > kChunkSize) {
            return fail(ErrorCode::invalid_state,
                        "InputStream.read returned more bytes than requested");
        }
        if (static_cast<usize>(*count) >
            kMaximumImageBytes - bytes.size()) {
            return fail_java("java/io/IOException",
                             "image stream exceeds 64 MiB");
        }
        auto copied = byte_array_slice(
            machine, *chunk, 0, *count, "Image.createImage");
        if (!copied) return std::unexpected(copied.error());
        bytes.insert(bytes.end(), copied->begin(), copied->end());
    }
    return bytes;
}

[[nodiscard]] i32 wrapping_add(i32 left, i32 right) noexcept {
    return std::bit_cast<i32>(static_cast<u32>(left) +
                              static_cast<u32>(right));
}

[[nodiscard]] i32 wrapping_subtract(i32 left, i32 right) noexcept {
    return std::bit_cast<i32>(static_cast<u32>(left) -
                              static_cast<u32>(right));
}

[[nodiscard]] i32 wrapping_multiply(i32 left, i32 right) noexcept {
    return std::bit_cast<i32>(static_cast<u32>(left) *
                              static_cast<u32>(right));
}

[[nodiscard]] Result<usize> required_pixel_count(i32 width, i32 height) {
    if (width <= 0 || height <= 0) {
        return fail_java("java/lang/IllegalArgumentException",
                         "image dimensions must be positive");
    }
    const usize unsigned_width = static_cast<usize>(width);
    const usize unsigned_height = static_cast<usize>(height);
    if (unsigned_width >
        std::numeric_limits<usize>::max() / unsigned_height) {
        return fail_java("java/lang/IllegalArgumentException",
                         "image pixel count overflows");
    }
    return unsigned_width * unsigned_height;
}

} // namespace

void register_image_natives(NativeMethodRegistry& registry) {
    constexpr const char* owner = "javax/microedition/lcdui/Image";

    add(registry, owner, "createImage", "(II)Ljavax/microedition/lcdui/Image;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto width = int_argument(arguments, 0U, "Image.createImage");
            auto height = int_argument(arguments, 1U, "Image.createImage");
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            auto image = graphics::Image::create_mutable(*width, *height);
            if (!image) return graphics_error(image.error());
            auto object = create_image_object(machine, std::move(*image));
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });

    add(registry, owner, "createMutableARGB",
        "(II)Ljavax/microedition/lcdui/Image;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto width = int_argument(arguments, 0U, "Image.createMutableARGB");
            auto height = int_argument(arguments, 1U, "Image.createMutableARGB");
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            auto image = graphics::Image::create_mutable_argb(*width, *height, 0U);
            if (!image) return graphics_error(image.error());
            auto object = create_image_object(machine, std::move(*image));
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });

    add(registry, owner, "createImage",
        "(Ljava/lang/String;)Ljavax/microedition/lcdui/Image;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto name = reference_argument(arguments, 0U,
                                           "Image.createImage");
            if (!name) return std::unexpected(name.error());
            auto resource = utf8_text(machine, *name, "Image.createImage");
            if (!resource) return std::unexpected(resource.error());
            std::string path = *resource;
            while (!path.empty() && path.front() == '/') {
                path.erase(path.begin());
            }
            if (path.empty()) {
                return fail_java("java/io/IOException",
                                 "image resource name is empty");
            }
            auto bytes = machine.classes().read_resource(path);
            if (!bytes) {
                return fail_java("java/io/IOException", bytes.error().message);
            }
            auto image = decode_image_bytes(*bytes, "java/io/IOException");
            if (!image) return std::unexpected(image.error());
            auto object = create_image_object(machine, std::move(*image));
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });

    add(registry, owner, "createImage",
        "(Ljava/io/InputStream;)Ljavax/microedition/lcdui/Image;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto input = reference_argument(
                arguments, 0U, "Image.createImage");
            if (!input) return std::unexpected(input.error());
            auto valid = machine.object_is_instance(
                *input, "java/io/InputStream");
            if (!valid) return std::unexpected(valid.error());
            if (!*valid) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Image.createImage argument is not InputStream");
            }
            auto bytes = read_input_stream(machine, *input);
            if (!bytes) return std::unexpected(bytes.error());
            auto image = decode_image_bytes(*bytes, "java/io/IOException");
            if (!image) return std::unexpected(image.error());
            auto object = create_image_object(machine, std::move(*image));
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });

    add(registry, owner, "createImage",
        "([BII)Ljavax/microedition/lcdui/Image;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto data = reference_argument(arguments, 0U,
                                           "Image.createImage");
            auto offset = int_argument(arguments, 1U,
                                       "Image.createImage");
            auto length = int_argument(arguments, 2U,
                                       "Image.createImage");
            if (!data) return std::unexpected(data.error());
            if (!offset) return std::unexpected(offset.error());
            if (!length) return std::unexpected(length.error());
            auto bytes = byte_array_slice(machine, *data, *offset, *length,
                                          "Image.createImage");
            if (!bytes) return std::unexpected(bytes.error());
            auto image = decode_image_bytes(
                *bytes, "java/lang/IllegalArgumentException");
            if (!image) return std::unexpected(image.error());
            auto object = create_image_object(machine, std::move(*image));
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });

    add(registry, owner, "createImage",
        "(Ljavax/microedition/lcdui/Image;)Ljavax/microedition/lcdui/Image;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto source_reference = reference_argument(
                arguments, 0U, "Image.createImage");
            if (!source_reference) {
                return std::unexpected(source_reference.error());
            }
            auto source = image_payload(machine, *source_reference);
            if (!source) return std::unexpected(source.error());
            auto copy = graphics::Image::create_immutable(
                (*source)->width(), (*source)->height(), (*source)->pixels());
            if (!copy) return graphics_error(copy.error());
            auto object = create_image_object(machine, std::move(*copy));
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });

    add(registry, owner, "createImage",
        "(Ljavax/microedition/lcdui/Image;IIIII)Ljavax/microedition/lcdui/Image;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto source_reference = reference_argument(
                arguments, 0U, "Image.createImage");
            auto x = int_argument(arguments, 1U, "Image.createImage");
            auto y = int_argument(arguments, 2U, "Image.createImage");
            auto width = int_argument(arguments, 3U, "Image.createImage");
            auto height = int_argument(arguments, 4U, "Image.createImage");
            auto transform_value = int_argument(arguments, 5U,
                                                "Image.createImage");
            if (!source_reference) return std::unexpected(source_reference.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            if (!transform_value) return std::unexpected(transform_value.error());
            auto transform = graphics::transform_from_int(*transform_value);
            if (!transform) return graphics_error(transform.error());
            auto source = image_payload(machine, *source_reference);
            if (!source) return std::unexpected(source.error());
            auto image = graphics::Image::transformed_region(
                **source, *x, *y, *width, *height, *transform);
            if (!image) return graphics_error(image.error());
            auto object = create_image_object(machine, std::move(*image));
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });

    add(registry, owner, "createRGBImage",
        "([IIIZ)Ljavax/microedition/lcdui/Image;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto rgb_reference = reference_argument(
                arguments, 0U, "Image.createRGBImage");
            auto width = int_argument(arguments, 1U,
                                      "Image.createRGBImage");
            auto height = int_argument(arguments, 2U,
                                       "Image.createRGBImage");
            auto process_alpha = int_argument(arguments, 3U,
                                              "Image.createRGBImage");
            if (!rgb_reference) return std::unexpected(rgb_reference.error());
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            if (!process_alpha) return std::unexpected(process_alpha.error());
            auto count = required_pixel_count(*width, *height);
            if (!count) return std::unexpected(count.error());
            auto pixels = int_array(machine, *rgb_reference,
                                    "Image.createRGBImage");
            if (!pixels) return std::unexpected(pixels.error());
            if (pixels->size() < *count) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "createRGBImage source int[] is too small");
            }
            pixels->resize(*count);
            if (*process_alpha == 0) {
                for (graphics::Pixel& pixel : *pixels) {
                    pixel = graphics::opaque(pixel);
                }
            }
            auto image = graphics::Image::create_immutable_owned(
                *width, *height, std::move(*pixels));
            if (!image) return graphics_error(image.error());
            auto object = create_image_object(machine, std::move(*image));
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });

    add(registry, owner, "getWidth", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto image_reference = receiver(arguments, "Image.getWidth");
            if (!image_reference) return std::unexpected(image_reference.error());
            auto image = image_payload(machine, *image_reference);
            if (!image) return std::unexpected(image.error());
            return std::optional<Value>(Value::from_int((*image)->width()));
        });

    add(registry, owner, "getHeight", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto image_reference = receiver(arguments, "Image.getHeight");
            if (!image_reference) return std::unexpected(image_reference.error());
            auto image = image_payload(machine, *image_reference);
            if (!image) return std::unexpected(image.error());
            return std::optional<Value>(Value::from_int((*image)->height()));
        });

    add(registry, owner, "isMutable", "()Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto image_reference = receiver(arguments, "Image.isMutable");
            if (!image_reference) return std::unexpected(image_reference.error());
            auto image = image_payload(machine, *image_reference);
            if (!image) return std::unexpected(image.error());
            return std::optional<Value>(Value::from_int(
                (*image)->is_mutable() ? 1 : 0));
        });

    add(registry, owner, "getGraphics",
        "()Ljavax/microedition/lcdui/Graphics;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto image_reference = receiver(arguments, "Image.getGraphics");
            if (!image_reference) return std::unexpected(image_reference.error());
            auto image = image_payload(machine, *image_reference);
            if (!image) return std::unexpected(image.error());
            if (!(*image)->is_mutable()) {
                return fail_java("java/lang/IllegalStateException",
                                 "immutable images do not expose Graphics");
            }
            auto graphics_object = create_graphics_object(machine,
                                                           *image_reference);
            if (!graphics_object) return std::unexpected(graphics_object.error());
            return std::optional<Value>(Value::from_reference(*graphics_object));
        });

    add(registry, owner, "getRGB", "([IIIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto image_reference = receiver(arguments, "Image.getRGB");
            auto destination = reference_argument(arguments, 1U,
                                                  "Image.getRGB", true);
            auto offset = int_argument(arguments, 2U, "Image.getRGB");
            auto scan_length = int_argument(arguments, 3U, "Image.getRGB");
            auto x = int_argument(arguments, 4U, "Image.getRGB");
            auto y = int_argument(arguments, 5U, "Image.getRGB");
            auto width = int_argument(arguments, 6U, "Image.getRGB");
            auto height = int_argument(arguments, 7U, "Image.getRGB");
            if (!image_reference) return std::unexpected(image_reference.error());
            if (!destination) return std::unexpected(destination.error());
            if (!offset) return std::unexpected(offset.error());
            if (!scan_length) return std::unexpected(scan_length.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            auto image = image_payload(machine, *image_reference);
            if (!image) return std::unexpected(image.error());

            // Mirror the reference Image.getRGB Java logic in the same order.
            // This order is observable: negative dimensions are a no-op before
            // rgbData is dereferenced, while zero dimensions still reach the
            // destination-array bounds checks.
            if (*scan_length >= 0 && *scan_length < *width) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "getRGB scanlength overlaps source rows");
            }
            if (*scan_length < 0 &&
                wrapping_subtract(0, *scan_length) < *width) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "getRGB scanlength overlaps source rows");
            }
            if (*y < 0 || *x < 0 ||
                wrapping_add(*x, *width) > (*image)->width() ||
                wrapping_add(*y, *height) > (*image)->height()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "getRGB source region is outside the image");
            }
            if (*height < 0 || *width < 0) {
                return std::optional<Value> {};
            }
            if (destination->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Image.getRGB argument is null");
            }
            auto class_name = machine.heap().class_name(*destination);
            auto destination_length = machine.heap().array_length(*destination);
            if (!class_name || !destination_length || *class_name != "[I") {
                return fail_java("java/lang/IllegalArgumentException",
                                 "getRGB destination must be int[]");
            }

            const i32 row_delta = wrapping_multiply(
                wrapping_subtract(*height, 1), *scan_length);
            const i32 last_row = wrapping_add(*offset, row_delta);
            const i32 exclusive_end = wrapping_add(last_row, *width);
            if (*offset < 0 ||
                exclusive_end > static_cast<i32>(*destination_length) ||
                last_row < 0) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "getRGB destination slice exceeds int[]");
            }
            if (*width == 0 || *height == 0) {
                return std::optional<Value> {};
            }

            // The reference precheck above uses Java int arithmetic. Keep a
            // second non-wrapping guard only to prevent native memory access if
            // deliberately overflowing arguments bypass that Java-era check.
            const i64 first = *offset;
            const i64 actual_last_row = static_cast<i64>(*offset) +
                (static_cast<i64>(*height) - 1) * *scan_length;
            const i64 minimum = std::min(first, actual_last_row);
            const i64 maximum = std::max(first, actual_last_row) + *width - 1;
            if (minimum < 0 || maximum < 0 ||
                maximum >= static_cast<i64>(*destination_length)) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "getRGB destination slice exceeds int[]");
            }
            std::vector<i32> row_pixels(static_cast<usize>(*width));
            for (i32 row = 0; row < *height; ++row) {
                const i64 destination_row = static_cast<i64>(*offset) +
                    static_cast<i64>(row) * *scan_length;
                for (i32 column = 0; column < *width; ++column) {
                    auto pixel = (*image)->pixel(*x + column, *y + row);
                    if (!pixel) return graphics_error(pixel.error());
                    row_pixels[static_cast<usize>(column)] =
                        static_cast<i32>(*pixel);
                }
                auto stored = machine.heap().write_int_array(
                    *destination,
                    static_cast<usize>(destination_row),
                    row_pixels);
                if (!stored) return std::unexpected(stored.error());
            }
            return std::optional<Value> {};
        });

    add(registry, owner, "setRGB", "([IIIIIII)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto image_reference = receiver(arguments, "Image.setRGB");
            auto source_reference = reference_argument(arguments, 1U,
                                                       "Image.setRGB");
            auto offset = int_argument(arguments, 2U, "Image.setRGB");
            auto scan_length = int_argument(arguments, 3U, "Image.setRGB");
            auto x = int_argument(arguments, 4U, "Image.setRGB");
            auto y = int_argument(arguments, 5U, "Image.setRGB");
            auto width = int_argument(arguments, 6U, "Image.setRGB");
            auto height = int_argument(arguments, 7U, "Image.setRGB");
            if (!image_reference) return std::unexpected(image_reference.error());
            if (!source_reference) return std::unexpected(source_reference.error());
            if (!offset) return std::unexpected(offset.error());
            if (!scan_length) return std::unexpected(scan_length.error());
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            if (!width) return std::unexpected(width.error());
            if (!height) return std::unexpected(height.error());
            auto image = image_payload(machine, *image_reference);
            if (!image) return std::unexpected(image.error());
            if (!(*image)->is_mutable()) {
                return fail_java("java/lang/IllegalStateException",
                                 "Image.setRGB requires a mutable image");
            }
            if (*width < 0 || *height < 0 || *x < 0 || *y < 0 ||
                static_cast<i64>(*x) + *width > (*image)->width() ||
                static_cast<i64>(*y) + *height > (*image)->height()) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "setRGB destination region is outside the image");
            }
            if (*width == 0 || *height == 0) {
                return std::optional<Value> {};
            }
            if (*scan_length >= 0 && *scan_length < *width) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "setRGB scanlength overlaps source rows");
            }
            if (*scan_length < 0 &&
                -static_cast<i64>(*scan_length) < *width) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "setRGB scanlength overlaps source rows");
            }
            auto pixels = int_array(machine, *source_reference, "Image.setRGB");
            if (!pixels) return std::unexpected(pixels.error());
            const i64 first = *offset;
            const i64 last_row = static_cast<i64>(*offset) +
                (static_cast<i64>(*height) - 1) * *scan_length;
            const i64 minimum = std::min(first, last_row);
            const i64 maximum = std::max(first, last_row) + *width - 1;
            if (minimum < 0 || maximum < 0 ||
                maximum >= static_cast<i64>(pixels->size())) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "setRGB source slice exceeds int[]");
            }
            auto destination_pixels = (*image)->mutable_pixels();
            const usize image_width = static_cast<usize>((*image)->width());
            for (i32 row = 0; row < *height; ++row) {
                const i64 source_row = static_cast<i64>(*offset) +
                    static_cast<i64>(row) * *scan_length;
                const usize destination_row =
                    static_cast<usize>(*y + row) * image_width +
                    static_cast<usize>(*x);
                for (i32 column = 0; column < *width; ++column) {
                    destination_pixels[destination_row +
                                       static_cast<usize>(column)] =
                        (*pixels)[static_cast<usize>(source_row + column)];
                }
            }
            (*image)->mark_dirty_region(*x, *y, *width, *height);
            return std::optional<Value> {};
        });
}

} // namespace phoneme::vm
