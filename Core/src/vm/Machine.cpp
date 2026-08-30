#include "phoneme/vm/Machine.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <functional>
#include <limits>
#include <optional>
#include <numbers>
#include <utility>
#include <vector>

#include "phoneme/base/Checked.hpp"
#include "phoneme/runtime/ParallelExecutor.hpp"
#include "phoneme/vm/BuiltinClasses.hpp"
#include "phoneme/vm/Descriptor.hpp"
#include "phoneme/vm/ModifiedUtf8.hpp"
#include "phoneme/vm/PerformanceCounters.hpp"
#include "phoneme/vm/SlotStorage.hpp"
#include "phoneme/vm/VmTrace.hpp"

#ifndef PHONEME_ENABLE_DECODED_EXECUTION
#define PHONEME_ENABLE_DECODED_EXECUTION 0
#endif

namespace phoneme::vm
{
  namespace
  {

    constexpr u16 kAccPublic = 0x0001U;
    constexpr u16 kAccPrivate = 0x0002U;
    constexpr u16 kAccStatic = 0x0008U;
    constexpr u16 kAccSynchronized = 0x0020U;
    constexpr u16 kAccNative = 0x0100U;
    constexpr u16 kAccInterface = 0x0200U;
    constexpr u16 kAccAbstract = 0x0400U;
    constexpr usize kMaximumCallDepth = 1'024;
    constexpr u64 kSchedulerQuantum = 50'000U;
    constexpr u64 kGarbageCollectionPollInterval = 10'000U;
    constexpr u64 kForegroundMaintenancePollInterval = 1'024U;
    constexpr u64 kBackgroundMaintenancePollInterval = 256U;
    static_assert((kForegroundMaintenancePollInterval &
                   (kForegroundMaintenancePollInterval - 1U)) == 0U);
    static_assert((kBackgroundMaintenancePollInterval &
                   (kBackgroundMaintenancePollInterval - 1U)) == 0U);

    [[nodiscard]] Result<std::u16string> format_java_int(i32 value)
    {
      std::array<char, 16U> buffer;
      const auto converted = std::to_chars(
          buffer.data(), buffer.data() + buffer.size(), value);
      if (converted.ec != std::errc{})
      {
        return fail(ErrorCode::internal_error,
                    "failed to format Java int");
      }
      const usize length = static_cast<usize>(converted.ptr - buffer.data());
      std::u16string text(length, u'\0');
      for (usize index = 0U; index < length; ++index)
      {
        text[index] = static_cast<char16_t>(
            static_cast<unsigned char>(buffer[index]));
      }
      return text;
    }

    [[nodiscard]] constexpr std::string_view jit_exception_class(
        JitExceptionKind kind) noexcept
    {
      switch (kind)
      {
      case JitExceptionKind::null_pointer:
        return "java/lang/NullPointerException";
      case JitExceptionKind::array_index_out_of_bounds:
        return "java/lang/ArrayIndexOutOfBoundsException";
      case JitExceptionKind::array_store:
        return "java/lang/ArrayStoreException";
      case JitExceptionKind::class_cast:
        return "java/lang/ClassCastException";
      case JitExceptionKind::runtime_throwable:
        return "java/lang/VirtualMachineError";
      }
      return "java/lang/VirtualMachineError";
    }

    [[nodiscard]] bool jit_instruction_budget_exhausted(
        const Error& error) noexcept
    {
      if (error.code != ErrorCode::invalid_state)
        return false;
      return error.message ==
                 "JIT bytecode instruction budget was exhausted" ||
             error.message ==
                 "JIT nested invocation exhausted instruction budget" ||
             error.message ==
                 "JIT OSR instruction budget was exhausted" ||
             error.message ==
                 "JIT OSR nested invocation exhausted budget";
    }

    [[nodiscard]] Error vm_instruction_budget_error(
        std::string_view owner,
        std::string_view method,
        std::string_view descriptor)
    {
      return Error::make(
          ErrorCode::invalid_state,
          "VM instruction budget was exhausted in " + std::string(owner) +
              "." + std::string(method) + std::string(descriptor));
    }

    [[nodiscard]] std::vector<char32_t> utf32_from_utf16(
        std::u16string_view input)
    {
      std::vector<char32_t> output;
      output.reserve(input.size());
      for (usize index = 0U; index < input.size(); ++index)
      {
        u32 value = static_cast<u16>(input[index]);
        if (value >= 0xD800U && value <= 0xDBFFU &&
            index + 1U < input.size())
        {
          const u32 low = static_cast<u16>(input[index + 1U]);
          if (low >= 0xDC00U && low <= 0xDFFFU)
          {
            value = 0x10000U + ((value - 0xD800U) << 10U) +
                    (low - 0xDC00U);
            ++index;
          }
        }
        output.push_back(static_cast<char32_t>(value));
      }
      return output;
    }

    [[nodiscard]] bool decoded_execution_requested() noexcept
    {
#if PHONEME_ENABLE_DECODED_EXECUTION
      static const bool requested = [] {
        const char* option = std::getenv("PHONEME_USE_DECODED_EXECUTION");
        return option == nullptr || std::string_view(option) != "0";
      }();
      return requested;
#else
      return false;
#endif
    }

    [[nodiscard]] bool specialized_intrinsics_requested() noexcept
    {
      static const bool requested = [] {
        const char* option = std::getenv("PHONEME_USE_SPECIALIZED_INTRINSICS");
        return option == nullptr || std::string_view(option) != "0";
      }();
      return requested;
    }

    [[nodiscard]] bool should_trace_slow_native(
        std::string_view owner,
        std::string_view name) noexcept
    {
      // These natives intentionally wait or encompass a complete Java worker
      // lifetime. Their durations are expected and are traced by the scheduler,
      // monitor or framebuffer paths instead of being reported as slow I/O.
      if (owner == "java/lang/Thread" &&
          (name == "sleep" || name == "run" || name == "join"))
      {
        return false;
      }
      if (owner == "java/lang/Object" && name == "wait")
        return false;
      if (owner == "javax/microedition/lcdui/Canvas" &&
          name == "serviceRepaints")
      {
        return false;
      }
      return true;
    }

    [[nodiscard]] Error stable_linkage_error(
        Error error,
        OperandResolutionKind kind)
    {
      if (error.code == ErrorCode::java_exception)
        return error;
      if (error.code == ErrorCode::class_not_found)
      {
        if (kind == OperandResolutionKind::field &&
            error.message.starts_with("field was not found"))
        {
          return Error::make_java("java/lang/NoSuchFieldError",
                                  std::move(error.message));
        }
        return Error::make_java("java/lang/NoClassDefFoundError",
                                std::move(error.message));
      }
      if (error.code == ErrorCode::method_not_found)
      {
        return Error::make_java("java/lang/NoSuchMethodError",
                                std::move(error.message));
      }
      if (error.code == ErrorCode::invalid_state &&
          (kind == OperandResolutionKind::field ||
           kind == OperandResolutionKind::direct_call ||
           kind == OperandResolutionKind::virtual_call))
      {
        return Error::make_java(
            "java/lang/IncompatibleClassChangeError",
            std::move(error.message));
      }
      return error;
    }

    [[nodiscard]] bool translation_wrapping_space(
        char32_t character) noexcept
    {
      return character == U' ' || character == U'\t' ||
             character == U'\r' || character == U'\u3000';
    }

    [[nodiscard]] usize translated_wrapped_line_count(
        const graphics::Font &font,
        std::span<const char32_t> text,
        i32 maximum_width)
    {
      usize line_count = 0U;
      std::vector<char32_t> current;
      auto flush = [&]()
      {
        while (!current.empty() &&
               translation_wrapping_space(current.front()))
          current.erase(current.begin());
        while (!current.empty() &&
               translation_wrapping_space(current.back()))
          current.pop_back();
        if (!current.empty())
          ++line_count;
        current.clear();
      };

      for (const char32_t character : text)
      {
        if (character == U'\n')
        {
          flush();
          continue;
        }
        current.push_back(character);
        if (font.chars_width(current) <= maximum_width)
          continue;

        usize break_index = current.size();
        while (break_index > 1U &&
               !translation_wrapping_space(current[break_index - 1U]))
          --break_index;
        if (break_index <= 1U)
        {
          const char32_t overflow = current.back();
          current.pop_back();
          flush();
          current.push_back(overflow);
          continue;
        }

        std::vector<char32_t> remainder(
            current.begin() + static_cast<std::ptrdiff_t>(break_index),
            current.end());
        current.erase(
            current.begin() + static_cast<std::ptrdiff_t>(break_index),
            current.end());
        flush();
        current = std::move(remainder);
        while (!current.empty() &&
               translation_wrapping_space(current.front()))
          current.erase(current.begin());
      }
      flush();
      return std::max<usize>(line_count, 1U);
    }

    [[nodiscard]] i32 translated_available_width(
        i32 x,
        i32 anchor,
        i32 clip_x,
        i32 clip_width,
        i32 translate_x) noexcept
    {
      const i32 resolved_anchor = anchor == 0
          ? graphics::anchor_left | graphics::anchor_top
          : anchor;
      const i32 horizontal = resolved_anchor &
          (graphics::anchor_left | graphics::anchor_right |
           graphics::anchor_hcenter);
      const i64 absolute_x = static_cast<i64>(x) + translate_x;
      const i64 clip_left = clip_x;
      const i64 clip_right = clip_left + clip_width;
      i64 available = clip_width;
      if (horizontal == graphics::anchor_left)
        available = clip_right - std::max(absolute_x, clip_left);
      else if (horizontal == graphics::anchor_right)
        available = std::min(absolute_x, clip_right) - clip_left;
      else if (horizontal == graphics::anchor_hcenter)
        available = 2 * std::min(
            std::max<i64>(0, absolute_x - clip_left),
            std::max<i64>(0, clip_right - absolute_x));
      return static_cast<i32>(std::clamp<i64>(
          available, 1, std::numeric_limits<i32>::max()));
    }

    [[nodiscard]] i32 translated_text_height(
        const graphics::Font &font,
        std::span<const char32_t> text,
        i32 maximum_width)
    {
      const usize line_count = translated_wrapped_line_count(
          font, text, std::max(maximum_width, 1));
      const i32 line_advance = std::max(font.height(), 1) + 1;
      const i64 total_height =
          static_cast<i64>(line_count) * line_advance - 1;
      return static_cast<i32>(std::clamp<i64>(
          total_height, 1, std::numeric_limits<i32>::max()));
    }

    [[nodiscard]] i32 translated_text_top(
        i32 y,
        i32 anchor,
        const graphics::Font &font,
        i32 translate_y) noexcept
    {
      const i32 resolved_anchor = anchor == 0
          ? graphics::anchor_left | graphics::anchor_top
          : anchor;
      const i32 vertical = resolved_anchor &
          (graphics::anchor_top | graphics::anchor_bottom |
           graphics::anchor_baseline);
      i64 top = static_cast<i64>(y) + translate_y;
      if (vertical == graphics::anchor_bottom)
        top -= font.height();
      else if (vertical == graphics::anchor_baseline)
        top -= font.baseline();
      return static_cast<i32>(std::clamp<i64>(
          top,
          std::numeric_limits<i32>::min(),
          std::numeric_limits<i32>::max()));
    }

    [[nodiscard]] i32 translated_y_for_top(
        i32 top,
        i32 block_height,
        i32 anchor,
        const graphics::Font &font,
        i32 translate_y) noexcept
    {
      const i32 resolved_anchor = anchor == 0
          ? graphics::anchor_left | graphics::anchor_top
          : anchor;
      const i32 vertical = resolved_anchor &
          (graphics::anchor_top | graphics::anchor_bottom |
           graphics::anchor_baseline);
      i64 y = top;
      if (vertical == graphics::anchor_bottom)
        y += block_height;
      else if (vertical == graphics::anchor_baseline)
        y += font.baseline();
      y -= translate_y;
      return static_cast<i32>(std::clamp<i64>(
          y,
          std::numeric_limits<i32>::min(),
          std::numeric_limits<i32>::max()));
    }

    [[nodiscard]] constexpr HeapLimits heap_limits_with_emergency_reserve(
        HeapLimits requested) noexcept
    {
      const usize hard_limit =
          static_cast<usize>(std::numeric_limits<u32>::max()) - 1U;
      requested.maximum_objects = requested.maximum_objects < hard_limit
          ? requested.maximum_objects + 1U : hard_limit;
      return requested;
    }

    thread_local Machine* g_execution_machine = nullptr;
    thread_local u32 g_execution_lock_depth = 0U;

    [[nodiscard]] bool value_matches(const Value &value,
                                     const TypeDescriptor &descriptor) noexcept
    {
      switch (descriptor.kind)
      {
      case JavaTypeKind::boolean:
      case JavaTypeKind::byte:
      case JavaTypeKind::character:
      case JavaTypeKind::short_integer:
      case JavaTypeKind::integer:
        return value.kind() == ValueKind::int32;
      case JavaTypeKind::float32:
        return value.kind() == ValueKind::float32;
      case JavaTypeKind::long_integer:
        return value.kind() == ValueKind::int64;
      case JavaTypeKind::float64:
        return value.kind() == ValueKind::float64;
      case JavaTypeKind::reference:
      case JavaTypeKind::array:
        return value.kind() == ValueKind::reference;
      case JavaTypeKind::void_type:
        return false;
      }
      return false;
    }

    [[nodiscard]] char primitive_descriptor_token(JavaTypeKind kind) noexcept
    {
      switch (kind)
      {
      case JavaTypeKind::boolean: return 'Z';
      case JavaTypeKind::byte: return 'B';
      case JavaTypeKind::character: return 'C';
      case JavaTypeKind::short_integer: return 'S';
      case JavaTypeKind::integer: return 'I';
      case JavaTypeKind::float32: return 'F';
      case JavaTypeKind::long_integer: return 'J';
      case JavaTypeKind::float64: return 'D';
      default: return '\0';
      }
    }

    [[nodiscard]] Result<std::string> reference_type_name(
        const TypeDescriptor& type)
    {
      if (type.kind == JavaTypeKind::reference)
        return type.class_name;
      if (type.kind != JavaTypeKind::array || type.array_dimensions == 0U)
      {
        return fail(ErrorCode::invalid_argument,
                    "requested type is not a Java reference");
      }
      std::string name(static_cast<usize>(type.array_dimensions), '[');
      if (type.array_component_kind == JavaTypeKind::reference)
      {
        if (type.class_name.empty())
        {
          return fail(ErrorCode::malformed_class,
                      "reference array descriptor lost its component class");
        }
        name += 'L';
        name += type.class_name;
        name += ';';
        return name;
      }
      const char token = primitive_descriptor_token(type.array_component_kind);
      if (token == '\0')
      {
        return fail(ErrorCode::malformed_class,
                    "array descriptor lost its primitive component type");
      }
      name += token;
      return name;
    }

    [[nodiscard]] std::string_view primitive_wrapper_class(
        JavaTypeKind kind) noexcept
    {
      switch (kind)
      {
      case JavaTypeKind::boolean: return "java/lang/Boolean";
      case JavaTypeKind::byte: return "java/lang/Byte";
      case JavaTypeKind::character: return "java/lang/Character";
      case JavaTypeKind::short_integer: return "java/lang/Short";
      case JavaTypeKind::integer: return "java/lang/Integer";
      case JavaTypeKind::float32: return "java/lang/Float";
      case JavaTypeKind::long_integer: return "java/lang/Long";
      case JavaTypeKind::float64: return "java/lang/Double";
      default: return {};
      }
    }

    [[nodiscard]] std::optional<JavaTypeKind> wrapper_primitive_kind(
        std::string_view class_name) noexcept
    {
      if (class_name == "java/lang/Boolean") return JavaTypeKind::boolean;
      if (class_name == "java/lang/Byte") return JavaTypeKind::byte;
      if (class_name == "java/lang/Character") return JavaTypeKind::character;
      if (class_name == "java/lang/Short") return JavaTypeKind::short_integer;
      if (class_name == "java/lang/Integer") return JavaTypeKind::integer;
      if (class_name == "java/lang/Float") return JavaTypeKind::float32;
      if (class_name == "java/lang/Long") return JavaTypeKind::long_integer;
      if (class_name == "java/lang/Double") return JavaTypeKind::float64;
      return std::nullopt;
    }

    [[nodiscard]] bool primitive_widening_allowed(JavaTypeKind source,
                                                   JavaTypeKind target) noexcept
    {
      if (source == target)
        return true;
      if (source == JavaTypeKind::boolean || target == JavaTypeKind::boolean)
        return false;
      if (source == JavaTypeKind::byte)
      {
        return target == JavaTypeKind::short_integer ||
               target == JavaTypeKind::integer ||
               target == JavaTypeKind::long_integer ||
               target == JavaTypeKind::float32 ||
               target == JavaTypeKind::float64;
      }
      if (source == JavaTypeKind::short_integer ||
          source == JavaTypeKind::character)
      {
        return target == JavaTypeKind::integer ||
               target == JavaTypeKind::long_integer ||
               target == JavaTypeKind::float32 ||
               target == JavaTypeKind::float64;
      }
      if (source == JavaTypeKind::integer)
      {
        return target == JavaTypeKind::long_integer ||
               target == JavaTypeKind::float32 ||
               target == JavaTypeKind::float64;
      }
      if (source == JavaTypeKind::long_integer)
      {
        return target == JavaTypeKind::float32 ||
               target == JavaTypeKind::float64;
      }
      return source == JavaTypeKind::float32 &&
             target == JavaTypeKind::float64;
    }

    [[nodiscard]] Result<Value> widen_primitive_value(Value value,
                                                       JavaTypeKind source,
                                                       JavaTypeKind target)
    {
      if (!primitive_widening_allowed(source, target))
      {
        return fail(ErrorCode::invalid_argument,
                    "primitive value cannot be widened to requested type");
      }
      if (source == target ||
          ((source == JavaTypeKind::byte ||
            source == JavaTypeKind::character ||
            source == JavaTypeKind::short_integer ||
            source == JavaTypeKind::integer ||
            source == JavaTypeKind::boolean) &&
           (target == JavaTypeKind::byte ||
            target == JavaTypeKind::character ||
            target == JavaTypeKind::short_integer ||
            target == JavaTypeKind::integer ||
            target == JavaTypeKind::boolean)))
      {
        return value;
      }
      if (source == JavaTypeKind::byte ||
          source == JavaTypeKind::character ||
          source == JavaTypeKind::short_integer ||
          source == JavaTypeKind::integer)
      {
        auto number = value.as_int();
        if (!number)
          return std::unexpected(number.error());
        if (target == JavaTypeKind::long_integer)
          return Value::from_long(static_cast<i64>(*number));
        if (target == JavaTypeKind::float32)
          return Value::from_float(static_cast<float>(*number));
        if (target == JavaTypeKind::float64)
          return Value::from_double(static_cast<double>(*number));
      }
      if (source == JavaTypeKind::long_integer)
      {
        auto number = value.as_long();
        if (!number)
          return std::unexpected(number.error());
        if (target == JavaTypeKind::float32)
          return Value::from_float(static_cast<float>(*number));
        if (target == JavaTypeKind::float64)
          return Value::from_double(static_cast<double>(*number));
      }
      if (source == JavaTypeKind::float32 && target == JavaTypeKind::float64)
      {
        auto number = value.as_float();
        if (!number)
          return std::unexpected(number.error());
        return Value::from_double(static_cast<double>(*number));
      }
      return fail(ErrorCode::invalid_argument,
                  "unsupported primitive widening conversion");
    }

    [[nodiscard]] u16 bytecode_cp_index(
        const std::vector<u8>& code,
        usize opcode_offset) noexcept
    {
      if (opcode_offset + 2U >= code.size()) return 0U;
      return static_cast<u16>(
          (static_cast<u16>(code[opcode_offset + 1U]) << 8U) |
          static_cast<u16>(code[opcode_offset + 2U]));
    }

    [[nodiscard]] bool matches_rect_contains_intrinsic(
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) == 0U ||
          method.descriptor != "(IIIIII)Z" || !method.code.has_value())
      {
        return false;
      }
      static constexpr std::array<u8, 32> kPattern {{
          0x1AU, 0x1CU, 0xA1U, 0x00U, 0x1CU,
          0x1AU, 0x1CU, 0x15U, 0x04U, 0x60U,
          0xA2U, 0x00U, 0x14U,
          0x1BU, 0x1DU, 0xA1U, 0x00U, 0x0FU,
          0x1BU, 0x1DU, 0x15U, 0x05U, 0x60U,
          0xA2U, 0x00U, 0x07U,
          0x04U, 0xA7U, 0x00U, 0x04U, 0x03U, 0xACU,
      }};
      return method.code->bytecode ==
          std::vector<u8>(kPattern.begin(), kPattern.end());
    }

    [[nodiscard]] bool matches_alpha_land_intrinsic(
        const classfile::ClassFile& owner,
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) == 0U ||
          method.descriptor != "(I)Z" || !method.code.has_value())
      {
        return false;
      }
      const auto& code = method.code->bytecode;
      if (code.size() != 13U ||
          code[0U] != 0x1AU || code[1U] != 0x12U ||
          code[3U] != 0x7EU || code[4U] != 0x99U ||
          code[7U] != 0x04U || code[8U] != 0xA7U ||
          code[11U] != 0x03U || code[12U] != 0xACU)
      {
        return false;
      }
      auto constant = owner.constant(static_cast<u16>(code[2U]));
      if (!constant || (*constant)->kind != classfile::ConstantKind::integer)
        return false;
      return static_cast<i32>(static_cast<u32>((*constant)->bits)) ==
          static_cast<i32>(0xFF00'0000U);
    }

    [[nodiscard]] bool matches_tiled_alpha_collision_scan(
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) != 0U ||
          method.descriptor != "(II)Z" || !method.code.has_value())
      {
        return false;
      }
      const auto& code = method.code->bytecode;
      if (code.size() != 84U) return false;
      static constexpr std::pair<usize, u8> kFixed[] = {
          {0U, 0x03U}, {1U, 0x3EU}, {2U, 0x1DU}, {3U, 0xB2U},
          {6U, 0xB6U}, {9U, 0xA2U}, {12U, 0xB2U}, {15U, 0x1DU},
          {16U, 0xB6U}, {19U, 0xC0U}, {22U, 0x3AU}, {23U, 0x04U},
          {24U, 0x1BU}, {25U, 0x1CU}, {26U, 0x19U}, {27U, 0x04U},
          {28U, 0xB4U}, {31U, 0x19U}, {32U, 0x04U}, {33U, 0xB4U},
          {36U, 0x19U}, {37U, 0x04U}, {38U, 0xB4U}, {41U, 0x19U},
          {42U, 0x04U}, {43U, 0xB4U}, {46U, 0xB8U}, {49U, 0x99U},
          {52U, 0x19U}, {53U, 0x04U}, {54U, 0x1BU}, {55U, 0x19U},
          {56U, 0x04U}, {57U, 0xB4U}, {60U, 0x64U}, {61U, 0x1CU},
          {62U, 0x19U}, {63U, 0x04U}, {64U, 0xB4U}, {67U, 0x64U},
          {68U, 0xB6U}, {71U, 0x99U}, {74U, 0x04U}, {75U, 0xACU},
          {76U, 0x84U}, {77U, 0x03U}, {78U, 0x01U}, {79U, 0xA7U},
          {82U, 0x03U}, {83U, 0xACU},
      };
      for (const auto& [offset, expected] : kFixed)
      {
        if (code[offset] != expected) return false;
      }
      return bytecode_cp_index(code, 3U) == bytecode_cp_index(code, 12U) &&
          bytecode_cp_index(code, 28U) == bytecode_cp_index(code, 57U) &&
          bytecode_cp_index(code, 33U) == bytecode_cp_index(code, 64U);
    }

    [[nodiscard]] bool matches_pixel_collision_intrinsic(
        const classfile::ClassFile& owner,
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) != 0U ||
          method.descriptor != "(II)Z" || !method.code.has_value())
      {
        return false;
      }
      const auto& code = method.code->bytecode;
      if (code.size() != 51U) return false;
      static constexpr std::pair<usize, u8> kFixed[] = {
          {0U, 0x1BU}, {1U, 0x9BU}, {4U, 0x1CU}, {5U, 0x9BU},
          {8U, 0x1BU}, {9U, 0x2AU}, {10U, 0xB4U}, {13U, 0xA2U},
          {16U, 0x1CU}, {17U, 0x2AU}, {18U, 0xB4U}, {21U, 0xA1U},
          {24U, 0x03U}, {25U, 0xACU}, {26U, 0x2AU}, {27U, 0xB4U},
          {30U, 0x9AU}, {33U, 0x2AU}, {34U, 0x1BU}, {35U, 0x1CU},
          {36U, 0xB6U}, {39U, 0xB8U}, {42U, 0x99U}, {45U, 0x04U},
          {46U, 0xA7U}, {49U, 0x03U}, {50U, 0xACU},
      };
      for (const auto& [offset, expected] : kFixed)
      {
        if (code[offset] != expected) return false;
      }
      auto get_pixel = owner.member_reference(bytecode_cp_index(code, 36U));
      auto land = owner.member_reference(bytecode_cp_index(code, 39U));
      return get_pixel && land &&
          get_pixel->kind == classfile::ConstantKind::method_ref &&
          get_pixel->descriptor == "(II)I" &&
          land->kind == classfile::ConstantKind::method_ref &&
          land->descriptor == "(I)Z";
    }

    [[nodiscard]] bool matches_pixel_getter_intrinsic(
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) != 0U ||
          method.descriptor != "(II)I" || !method.code.has_value())
      {
        return false;
      }
      const auto& code = method.code->bytecode;
      if (code.size() != 14U) return false;
      static constexpr std::pair<usize, u8> kFixed[] = {
          {0U, 0x2AU}, {1U, 0xB4U}, {4U, 0x1CU}, {5U, 0x2AU},
          {6U, 0xB4U}, {9U, 0x68U}, {10U, 0x1BU}, {11U, 0x60U},
          {12U, 0x2EU}, {13U, 0xACU},
      };
      for (const auto& [offset, expected] : kFixed)
      {
        if (code[offset] != expected) return false;
      }
      return true;
    }

    [[nodiscard]] bool matches_projectile_collision_scan(
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) == 0U ||
          method.descriptor != "(IILplayer/CPlayer;)Z" ||
          !method.code.has_value())
      {
        return false;
      }
      const auto& code = method.code->bytecode;
      if (code.size() != 105U) return false;
      static constexpr std::pair<usize, u8> kFixed[] = {
          {0U, 0xB2U}, {3U, 0xC7U}, {6U, 0x03U}, {7U, 0xACU},
          {8U, 0x03U}, {9U, 0x3EU}, {10U, 0x1DU}, {11U, 0xB2U},
          {14U, 0xBEU}, {15U, 0xA2U}, {18U, 0xB2U}, {21U, 0x1DU},
          {22U, 0x32U}, {23U, 0x3AU}, {24U, 0x04U}, {25U, 0x19U},
          {26U, 0x04U}, {27U, 0xC6U}, {30U, 0x19U}, {31U, 0x04U},
          {32U, 0x2CU}, {33U, 0xA5U}, {36U, 0x19U}, {37U, 0x04U},
          {38U, 0xB8U}, {41U, 0x99U}, {44U, 0x2CU}, {45U, 0xC6U},
          {48U, 0x19U}, {49U, 0x04U}, {50U, 0xB4U}, {53U, 0x2CU},
          {54U, 0xB4U}, {57U, 0x9FU}, {60U, 0x19U}, {61U, 0x04U},
          {62U, 0xB4U}, {65U, 0x9EU}, {68U, 0x19U}, {69U, 0x04U},
          {70U, 0xB6U}, {73U, 0x08U}, {74U, 0x9FU}, {77U, 0x19U},
          {78U, 0x04U}, {79U, 0xB4U}, {82U, 0x9AU}, {85U, 0x19U},
          {86U, 0x04U}, {87U, 0x1AU}, {88U, 0x1BU}, {89U, 0xB8U},
          {92U, 0x99U}, {95U, 0x04U}, {96U, 0xACU}, {97U, 0x84U},
          {98U, 0x03U}, {99U, 0x01U}, {100U, 0xA7U},
          {103U, 0x03U}, {104U, 0xACU},
      };
      for (const auto& [offset, expected] : kFixed)
      {
        if (code[offset] != expected) return false;
      }
      return bytecode_cp_index(code, 0U) == bytecode_cp_index(code, 11U) &&
          bytecode_cp_index(code, 0U) == bytecode_cp_index(code, 18U);
    }

    [[nodiscard]] bool matches_damageable_target_intrinsic(
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) == 0U ||
          method.descriptor != "(Lplayer/CPlayer;)Z" ||
          !method.code.has_value())
      {
        return false;
      }
      const auto& code = method.code->bytecode;
      if (code.size() != 22U) return false;
      static constexpr std::pair<usize, u8> kFixed[] = {
          {0U, 0x2AU}, {1U, 0xC1U}, {4U, 0x99U}, {7U, 0x2AU},
          {8U, 0xB4U}, {11U, 0x10U}, {12U, 17U}, {13U, 0x9FU},
          {16U, 0x04U}, {17U, 0xA7U}, {20U, 0x03U}, {21U, 0xACU},
      };
      for (const auto& [offset, expected] : kFixed)
      {
        if (code[offset] != expected) return false;
      }
      return true;
    }

    [[nodiscard]] bool matches_state_getter_intrinsic(
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) != 0U ||
          method.descriptor != "()B" || !method.code.has_value())
      {
        return false;
      }
      const auto& code = method.code->bytecode;
      return code.size() == 5U && code[0U] == 0x2AU &&
          code[1U] == 0xB4U && code[4U] == 0xACU;
    }

    [[nodiscard]] bool matches_non_boss_projectile_hitbox_intrinsic(
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) == 0U ||
          method.descriptor != "(Lplayer/CPlayer;II)Z" ||
          !method.code.has_value())
      {
        return false;
      }
      const auto& code = method.code->bytecode;
      if (code.size() != 625U || code[0U] != 0x2AU ||
          code[1U] != 0xC1U || code[4U] != 0x99U)
      {
        return false;
      }
      static constexpr std::pair<usize, u8> kTail[] = {
          {562U, 0x2AU}, {563U, 0xB4U}, {566U, 0x10U}, {567U, 12U},
          {568U, 0x64U}, {569U, 0x3EU}, {570U, 0x2AU}, {571U, 0xB4U},
          {574U, 0x10U}, {575U, 26U}, {576U, 0x64U}, {577U, 0x36U},
          {578U, 0x04U}, {579U, 0x2AU}, {580U, 0xB4U},
          {583U, 0x10U}, {584U, 12U}, {585U, 0x60U}, {586U, 0x36U},
          {587U, 0x05U}, {588U, 0x2AU}, {589U, 0xB4U},
          {592U, 0x05U}, {593U, 0x60U}, {594U, 0x36U}, {595U, 0x06U},
          {596U, 0x1BU}, {597U, 0x1DU}, {598U, 0xA1U},
          {601U, 0x1BU}, {602U, 0x15U}, {603U, 0x05U}, {604U, 0xA3U},
          {607U, 0x1CU}, {608U, 0x15U}, {609U, 0x04U}, {610U, 0xA1U},
          {613U, 0x1CU}, {614U, 0x15U}, {615U, 0x06U}, {616U, 0xA3U},
          {619U, 0x04U}, {620U, 0xA7U}, {623U, 0x03U}, {624U, 0xACU},
      };
      for (const auto& [offset, expected] : kTail)
      {
        if (code[offset] != expected) return false;
      }
      return bytecode_cp_index(code, 563U) == bytecode_cp_index(code, 580U) &&
          bytecode_cp_index(code, 571U) == bytecode_cp_index(code, 589U);
    }

    [[nodiscard]] bool matches_long_bit_permutation_intrinsic(
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) == 0U ||
          method.descriptor != "(JII[I[J)J" || !method.code.has_value())
      {
        return false;
      }
      static constexpr std::array<u8, 139> kPattern {{
          0x09U, 0x37U, 0x06U, 0x19U, 0x04U, 0xBEU, 0x36U, 0x08U,
          0x03U, 0x36U, 0x09U, 0xA7U, 0x00U, 0x44U, 0x1EU, 0xB2U,
          0x00U, 0x0BU, 0x15U, 0x09U, 0x2FU, 0x7FU, 0x37U, 0x0AU,
          0x19U, 0x04U, 0x15U, 0x09U, 0x2EU, 0x36U, 0x0CU, 0x16U,
          0x0AU, 0x09U, 0x94U, 0x99U, 0x00U, 0x29U, 0x15U, 0x0CU,
          0x9EU, 0x00U, 0x0DU, 0x16U, 0x0AU, 0x15U, 0x0CU, 0x7DU,
          0x37U, 0x0AU, 0xA7U, 0x00U, 0x13U, 0x15U, 0x0CU, 0x9CU,
          0x00U, 0x0EU, 0x16U, 0x0AU, 0x15U, 0x0CU, 0x02U, 0x82U,
          0x04U, 0x60U, 0x79U, 0x37U, 0x0AU, 0x16U, 0x06U, 0x16U,
          0x0AU, 0x81U, 0x37U, 0x06U, 0x84U, 0x09U, 0x01U, 0x15U,
          0x09U, 0x15U, 0x08U, 0xA1U, 0xFFU, 0xBBU, 0x10U, 0x40U,
          0x36U, 0x08U, 0x16U, 0x06U, 0x37U, 0x0AU, 0x15U, 0x08U,
          0x04U, 0x64U, 0x1DU, 0x64U, 0x36U, 0x0CU, 0x15U, 0x0CU,
          0x9EU, 0x00U, 0x0AU, 0x16U, 0x0AU, 0x15U, 0x0CU, 0x79U,
          0x37U, 0x0AU, 0x1CU, 0x15U, 0x08U, 0x60U, 0x04U, 0x64U,
          0x1DU, 0x64U, 0x36U, 0x0DU, 0x15U, 0x0DU, 0x9EU, 0x00U,
          0x0AU, 0x16U, 0x0AU, 0x15U, 0x0DU, 0x7DU, 0x37U, 0x0AU,
          0x16U, 0x0AU, 0xADU,
      }};
      const auto& bytecode = method.code->bytecode;
      if (bytecode.size() != kPattern.size()) return false;
      // Bytes 16-17 are the constant-pool index of the static long[] mask
      // field and legitimately vary between otherwise identical obfuscator
      // output.
      return std::equal(bytecode.begin(), bytecode.begin() + 16U,
                        kPattern.begin()) &&
             std::equal(bytecode.begin() + 18U, bytecode.end(),
                        kPattern.begin() + 18U);
    }

    [[nodiscard]] bool matches_range_decoder_bit_intrinsic(
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) == 0U ||
          method.descriptor != "(I)I" || !method.code.has_value())
      {
        return false;
      }
      const auto& code = method.code->bytecode;
      if (code.size() != 150U) return false;
      static constexpr std::pair<usize, u8> kFixed[] = {
          {0U, 0xB2U}, {3U, 0x10U}, {4U, 11U}, {5U, 0x7BU},
          {6U, 0xB2U}, {9U, 0x1AU}, {10U, 0x35U}, {11U, 0x85U},
          {12U, 0x69U}, {13U, 0x40U}, {14U, 0xB2U}, {17U, 0x1FU},
          {18U, 0x94U}, {19U, 0x9CU}, {22U, 0x1FU}, {23U, 0xB3U},
          {26U, 0x11U}, {27U, 0x08U}, {28U, 0x00U}, {29U, 0xB2U},
          {32U, 0x1AU}, {33U, 0x35U}, {34U, 0x64U}, {35U, 0x3EU},
          {36U, 0xB2U}, {39U, 0x1AU}, {40U, 0x5CU}, {41U, 0x35U},
          {42U, 0x1DU}, {43U, 0x08U}, {44U, 0x7AU}, {45U, 0x60U},
          {46U, 0x93U}, {47U, 0x56U}, {48U, 0xB2U}, {51U, 0x14U},
          {54U, 0x94U}, {55U, 0x9CU}, {58U, 0xB2U}, {61U, 0x10U},
          {62U, 8U}, {63U, 0x79U}, {64U, 0xB8U}, {67U, 0x85U},
          {68U, 0x81U}, {69U, 0xB3U}, {72U, 0xB2U}, {75U, 0x10U},
          {76U, 8U}, {77U, 0x79U}, {78U, 0xB3U}, {81U, 0x03U},
          {82U, 0xACU}, {83U, 0xB2U}, {86U, 0x1FU}, {87U, 0x65U},
          {88U, 0xB3U}, {91U, 0xB2U}, {94U, 0x1FU}, {95U, 0x65U},
          {96U, 0xB3U}, {99U, 0xB2U}, {102U, 0x1AU}, {103U, 0x5CU},
          {104U, 0x35U}, {105U, 0xB2U}, {108U, 0x1AU}, {109U, 0x35U},
          {110U, 0x08U}, {111U, 0x7AU}, {112U, 0x64U}, {113U, 0x93U},
          {114U, 0x56U}, {115U, 0xB2U}, {118U, 0x14U},
          {121U, 0x94U}, {122U, 0x9CU}, {125U, 0xB2U},
          {128U, 0x10U}, {129U, 8U}, {130U, 0x79U}, {131U, 0xB8U},
          {134U, 0x85U}, {135U, 0x81U}, {136U, 0xB3U},
          {139U, 0xB2U}, {142U, 0x10U}, {143U, 8U}, {144U, 0x79U},
          {145U, 0xB3U}, {148U, 0x04U}, {149U, 0xACU},
      };
      for (const auto& [offset, expected] : kFixed)
      {
        if (code[offset] != expected) return false;
      }
      return true;
    }

    [[nodiscard]] bool matches_range_decoder_input_intrinsic(
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) == 0U ||
          method.descriptor != "()I" || !method.code.has_value())
      {
        return false;
      }
      const auto& code = method.code->bytecode;
      if (code.size() != 31U) return false;
      static constexpr std::pair<usize, u8> kFixed[] = {
          {0U, 0xB2U}, {3U, 0xB2U}, {6U, 0xA0U},
          {9U, 0x11U}, {10U, 0x00U}, {11U, 0xFFU}, {12U, 0xACU},
          {13U, 0xB2U}, {16U, 0xB2U}, {19U, 0x59U}, {20U, 0x04U},
          {21U, 0x60U}, {22U, 0xB3U}, {25U, 0x33U},
          {26U, 0x11U}, {27U, 0x00U}, {28U, 0xFFU},
          {29U, 0x7EU}, {30U, 0xACU},
      };
      for (const auto& [offset, expected] : kFixed)
      {
        if (code[offset] != expected) return false;
      }
      return true;
    }

    // Some Java ME games bundle a complete indexed-PNG decoder (and often a
    // Java DEFLATE implementation) because old handsets had inconsistent PNG
    // support. Running that decoder in Java can dominate startup even though
    // phoneME already has a native PNG decoder. Match the decoder by bytecode
    // shape and standard API references rather than by obfuscated class/method
    // names, so this remains a reusable compatibility/performance intrinsic.
    [[nodiscard]] bool matches_indexed_png_decoder_intrinsic(
        const classfile::ClassFile& owner,
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) == 0U ||
          method.descriptor !=
              "([B)Ljavax/microedition/lcdui/Image;" ||
          !method.code.has_value())
      {
        return false;
      }

      const auto& code = method.code->bytecode;
      // This is the common compact CLDC indexed-PNG parser shape: validate the
      // 8-byte PNG signature, collect palette/alpha/IDAT chunks, inflate scan
      // lines, then construct the result with createRGBImage(). Constant-pool
      // indexes legitimately vary between obfuscated builds.
      if (code.size() != 383U ||
          code[0U] != 0x01U ||  // aconst_null
          code[1U] != 0x4CU ||  // astore_1
          code[2U] != 0xB8U ||  // reset helper
          code[5U] != 0x2AU ||  // aload_0
          code[6U] != 0xB3U ||  // putstatic input bytes
          code[9U] != 0xB8U ||  // read big-endian int
          code[12U] != 0x3BU ||
          code[13U] != 0xB8U ||
          code[16U] != 0x3DU ||
          code[17U] != 0x1AU ||
          code[18U] != 0x12U || // ldc PNG signature high dword
          code[20U] != 0xA0U ||
          code[23U] != 0x1CU ||
          code[24U] != 0x12U || // ldc PNG signature low dword
          code[26U] != 0x9FU ||
          code[340U] != 0xB8U || // System.arraycopy for IDAT
          code[358U] != 0x04U ||
          code[359U] != 0x99U ||
          code[362U] != 0xB8U || // inflate/reconstruct pixels helper
          code[365U] != 0x59U ||
          code[374U] != 0xB8U || // Image.createRGBImage
          code[377U] != 0x4CU ||
          code[378U] != 0xB8U || // cleanup helper
          code[381U] != 0x2BU ||
          code[382U] != 0xB0U)
      {
        return false;
      }

      auto signature_high = owner.constant(static_cast<u16>(code[19U]));
      auto signature_low = owner.constant(static_cast<u16>(code[25U]));
      if (!signature_high || !signature_low ||
          (*signature_high)->kind != classfile::ConstantKind::integer ||
          (*signature_low)->kind != classfile::ConstantKind::integer ||
          static_cast<u32>((*signature_high)->bits) != 0x8950'4E47U ||
          static_cast<u32>((*signature_low)->bits) != 0x0D0A'1A0AU)
      {
        return false;
      }

      auto arraycopy = owner.member_reference(bytecode_cp_index(code, 340U));
      auto create_rgb = owner.member_reference(bytecode_cp_index(code, 374U));
      if (!arraycopy || !create_rgb)
        return false;
      return arraycopy->owner == "java/lang/System" &&
          arraycopy->name == "arraycopy" &&
          arraycopy->descriptor ==
              "(Ljava/lang/Object;ILjava/lang/Object;II)V" &&
          create_rgb->owner == "javax/microedition/lcdui/Image" &&
          create_rgb->name == "createRGBImage" &&
          create_rgb->descriptor ==
              "([IIIZ)Ljavax/microedition/lcdui/Image;";
    }

    [[nodiscard]] bool matches_vector_key_sort_initializer(
        const classfile::Method& method) noexcept
    {
      if ((method.access_flags & kAccStatic) == 0U ||
          method.descriptor != "()V" || !method.code.has_value())
      {
        return false;
      }
      const auto& code = method.code->bytecode;
      if (code.size() != 58U) return false;
      static constexpr std::pair<usize, u8> kFixed[] = {
          {0U, 0xB2U}, {3U, 0xB6U}, {6U, 0x3BU},
          {7U, 0xBBU}, {10U, 0x59U}, {11U, 0x1AU},
          {12U, 0xB7U}, {15U, 0x4CU}, {16U, 0x03U},
          {17U, 0x3DU}, {18U, 0xA7U}, {19U, 0x00U},
          {20U, 0x11U}, {21U, 0x2BU}, {22U, 0xB2U},
          {25U, 0x1CU}, {26U, 0xB6U}, {29U, 0xB6U},
          {32U, 0x84U}, {33U, 0x02U}, {34U, 0x01U},
          {35U, 0x1CU}, {36U, 0x1AU}, {37U, 0xA1U},
          {38U, 0xFFU}, {39U, 0xF0U}, {40U, 0x03U},
          {41U, 0xB2U}, {44U, 0xB6U}, {47U, 0x04U},
          {48U, 0x64U}, {49U, 0xB2U}, {52U, 0x2BU},
          {53U, 0x03U}, {54U, 0xB8U}, {57U, 0xB1U},
      };
      for (const auto& [offset, expected] : kFixed)
      {
        if (code[offset] != expected) return false;
      }
      return code[1U] == code[23U] && code[2U] == code[24U] &&
             code[1U] == code[42U] && code[2U] == code[43U] &&
             code[1U] == code[50U] && code[2U] == code[51U];
    }

    class ExecutionFrame final
    {
    public:
      struct InvokeInterfaceOperands final
      {
        u16 constant_pool_index{0};
        u8 count{0};
      };

      struct IncrementOperands final
      {
        u16 local_index{0};
        i32 increment{0};
      };

      struct WideOperands final
      {
        u8 opcode{0};
        u16 local_index{0};
        std::optional<i16> increment;
      };

      struct MultiArrayOperands final
      {
        u16 constant_pool_index{0};
        u8 dimensions{0};
      };

      [[nodiscard]] static Result<ExecutionFrame*> emplace(
          std::vector<ExecutionFrame>& frames,
          ResolvedMethod resolved,
          std::shared_ptr<const CachedMethodDescriptor> descriptor,
          const InvocationArguments& arguments,
          bool has_receiver)
      {
        if (resolved.method == nullptr || !resolved.method->code.has_value())
        {
          return fail(ErrorCode::unsupported_feature,
                      "native or abstract method execution is not ported yet");
        }
        if (descriptor == nullptr)
        {
          return fail(ErrorCode::invalid_state,
                      "execution frame has no cached method descriptor");
        }

        const usize previous_capacity = frames.capacity();
        frames.emplace_back(std::move(resolved),
                            std::move(descriptor),
                            has_receiver);
        PerformanceCounters::observe_execution_frame_stack(
            frames.capacity(), frames.capacity() != previous_capacity);
        ExecutionFrame& frame = frames.back();
        usize local_index = 0;
        for (usize argument_index = 0U;
             argument_index < arguments.size();
             ++argument_index)
        {
          const ValueKind kind = arguments.kind(argument_index);
          auto stored = frame.locals_.set_compact(
              local_index, kind, arguments.raw_bits(argument_index));
          if (!stored)
          {
            frames.pop_back();
            return std::unexpected(stored.error());
          }
          local_index += compact_category_two(kind) ? 2U : 1U;
        }
        return &frame;
      }

      [[nodiscard]] const classfile::ClassFile &owner() const noexcept
      {
        return *resolved_.owner;
      }

      [[nodiscard]] const std::shared_ptr<const classfile::ClassFile>&
      owner_lifetime() const noexcept
      {
        return resolved_.owner;
      }

      [[nodiscard]] const classfile::Method &method() const noexcept
      {
        return *resolved_.method;
      }

      [[nodiscard]] const MethodDescriptor &descriptor() const noexcept
      {
        return descriptor_->descriptor;
      }

      [[nodiscard]] MethodId runtime_method_id() const noexcept
      {
        return resolved_.runtime != nullptr
            ? resolved_.runtime->id
            : MethodId {};
      }

      [[nodiscard]] Result<OperandResolutionEntry*> operand_resolution_entry(
          u32 operand_index,
          usize bytecode_pc) const
      {
        if (resolved_.runtime == nullptr ||
            resolved_.runtime->decoded == nullptr ||
            resolved_.runtime->operand_resolutions == nullptr ||
            operand_index == kInvalidDecodedIndex ||
            bytecode_pc > static_cast<usize>(std::numeric_limits<u32>::max()))
        {
          return fail(ErrorCode::invalid_argument,
                      "execution frame has no decoded operand resolution");
        }
        return resolved_.runtime->operand_resolutions->entry(
            operand_index, static_cast<u32>(bytecode_pc));
      }

      [[nodiscard]] const CachedMethodDescriptor* cached_descriptor() const noexcept
      {
        return descriptor_.get();
      }

      [[nodiscard]] const std::shared_ptr<const CachedMethodDescriptor>&
      cached_descriptor_lifetime() const noexcept
      {
        return descriptor_;
      }

      [[nodiscard]] std::shared_ptr<const VerifiedMethodReferenceMaps>
      verified_frames() const noexcept
      {
        return resolved_.runtime != nullptr
            ? resolved_.runtime->verified_frames
            : nullptr;
      }

      [[nodiscard]] bool has_verified_types() const noexcept
      {
        return resolved_.runtime != nullptr &&
               resolved_.runtime->verified_frames != nullptr;
      }

      [[nodiscard]] const VerifiedReferenceMap* verified_frame_at(
          usize bytecode_pc) const noexcept
      {
        if (resolved_.runtime == nullptr ||
            resolved_.runtime->decoded == nullptr ||
            resolved_.runtime->verified_frames == nullptr ||
            bytecode_pc > std::numeric_limits<u32>::max())
        {
          return nullptr;
        }
        const u32 instruction_index =
            resolved_.runtime->decoded->instruction_index_for_bci(
                static_cast<u32>(bytecode_pc));
        if (instruction_index == kInvalidDecodedIndex ||
            instruction_index >=
                resolved_.runtime->verified_frame_index_by_instruction.size())
        {
          return nullptr;
        }
        const u32 frame_index =
            resolved_.runtime
                ->verified_frame_index_by_instruction[instruction_index];
        if (frame_index == kInvalidDecodedIndex ||
            frame_index >= resolved_.runtime->verified_frames->frames.size())
        {
          return nullptr;
        }
        const VerifiedReferenceMap& frame =
            resolved_.runtime->verified_frames->frames[frame_index];
        return frame.bytecode_pc == bytecode_pc ? &frame : nullptr;
      }

      [[nodiscard]] bool note_osr_backedge() noexcept
      {
        if (osr_backedges_ != std::numeric_limits<u32>::max())
          ++osr_backedges_;
        const u32 shift = std::min(osr_attempt_count_, 5U);
        const u32 threshold = 8U << shift;
        if (osr_backedges_ < threshold) return false;
        osr_backedges_ = 0U;
        if (osr_attempt_count_ < 5U) ++osr_attempt_count_;
        return true;
      }

      void allow_osr_retry() noexcept
      {
        osr_backedges_ = 0U;
        // Keep the accumulated retry tier after a precise deopt. Resetting the
        // attempt count here made retryable Java calls bounce back into the
        // same OSR entry every ~8 backedges while their callee was still being
        // background-compiled. Preserving the exponential backoff (16/32/64/
        // 128/256 backedges) still allows the caller to re-enter native code
        // once the transient condition clears, without repeatedly paying the
        // deopt/restore cost in the meantime.
      }

      [[nodiscard]] u32 current_decoded_operand_index() const noexcept
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        if (decoded_ == nullptr ||
            current_decoded_index_ == kInvalidDecodedIndex ||
            current_decoded_index_ >= decoded_->instructions.size())
        {
          return kInvalidDecodedIndex;
        }
        return decoded_->instructions[current_decoded_index_].operand_index;
#else
        return kInvalidDecodedIndex;
#endif
      }

      void append_upcoming_translation_candidates(
          std::vector<std::vector<char32_t>>& output,
          usize maximum_candidates = 6U,
          usize maximum_instructions = 96U) const
      {
        if (maximum_candidates == 0U || maximum_instructions == 0U ||
            resolved_.runtime == nullptr || !resolved_.runtime->decoded)
        {
          return;
        }
        const DecodedMethod& decoded = *resolved_.runtime->decoded;
        if (current_instruction_pc_ > std::numeric_limits<u32>::max())
          return;
        const u32 current_index = decoded.instruction_index_for_bci(
            static_cast<u32>(current_instruction_pc_));
        if (current_index == kInvalidDecodedIndex ||
            current_index >= decoded.instructions.size())
        {
          return;
        }

        u32 instruction_index = decoded.instructions[current_index].next_index;
        usize scanned = 0U;
        while (instruction_index < decoded.instructions.size() &&
               scanned < maximum_instructions &&
               output.size() < maximum_candidates)
        {
          const DecodedInstruction& instruction =
              decoded.instructions[instruction_index];
          const u8 opcode = raw_opcode(instruction.opcode);
          if ((opcode == 0x12U || opcode == 0x13U) &&
              instruction.operand_index != kInvalidDecodedIndex &&
              instruction.operand_index < decoded.operands.size())
          {
            const DecodedOperand& operand =
                decoded.operands[instruction.operand_index];
            if (operand.kind == DecodedOperandKind::constant_pool_index)
            {
              auto constant = owner().constant(operand.constant_pool_index);
              if (constant &&
                  (*constant)->kind == classfile::ConstantKind::string_ref)
              {
                auto encoded = owner().string_constant(
                    operand.constant_pool_index);
                if (encoded)
                {
                  auto decoded_text = decode_modified_utf8(*encoded);
                  if (decoded_text)
                  {
                    auto candidate = utf32_from_utf16(*decoded_text);
                    if (translation::TranslationService::
                            contains_translatable_text(candidate) &&
                        std::find(output.begin(), output.end(), candidate) ==
                            output.end())
                    {
                      output.push_back(std::move(candidate));
                    }
                  }
                }
              }
            }
          }
          instruction_index = instruction.next_index;
          ++scanned;
        }
      }

      [[nodiscard]] bool has_receiver() const noexcept { return has_receiver_; }
      [[nodiscard]] usize pc() const noexcept { return pc_; }
      [[nodiscard]] usize code_size() const noexcept { return code_.size(); }
      [[nodiscard]] usize current_instruction_pc() const noexcept
      {
        return current_instruction_pc_;
      }
      [[nodiscard]] const std::vector<classfile::ExceptionHandler> &
      exception_table() const noexcept
      {
        return method().code->exception_table;
      }

      void begin_instruction(usize opcode_pc) noexcept
      {
        current_instruction_pc_ = opcode_pc;
      }

      [[nodiscard]] Result<u8> read_opcode()
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        if (decoded_ != nullptr)
        {
          u32 instruction_index = decoded_index_;
          if (instruction_index >= decoded_->instructions.size() ||
              decoded_->instructions[instruction_index].bytecode_pc != pc_)
          {
            if (pc_ > std::numeric_limits<u32>::max())
            {
              return fail(ErrorCode::malformed_class,
                          "decoded bytecode PC exceeds index space");
            }
            instruction_index = decoded_->instruction_index_for_bci(
                static_cast<u32>(pc_));
          }
          if (instruction_index == kInvalidDecodedIndex ||
              instruction_index >= decoded_->instructions.size())
          {
            return fail(ErrorCode::malformed_class,
                        "decoded execution PC is not an instruction boundary");
          }
          const DecodedInstruction& instruction =
              decoded_->instructions[instruction_index];
          current_decoded_index_ = instruction_index;
          pc_ = static_cast<usize>(instruction.bytecode_pc) + 1U;
          decoded_index_ = instruction.next_index;
          PerformanceCounters::record_decoded_opcode_dispatch();
          return raw_opcode(instruction.opcode);
        }
#endif
        return read_u8();
      }

      [[nodiscard]] Status enter_exception_handler(usize handler_pc,
                                                   ObjectRef throwable)
      {
        if (handler_pc >= code_.size())
        {
          return fail(ErrorCode::malformed_class,
                      "exception handler target is outside method bytecode");
        }
        stack_.clear();
        auto pushed = stack_.push_reference(throwable);
        if (!pushed)
        {
          return std::unexpected(pushed.error());
        }
        pc_ = handler_pc;
        auto synchronized = synchronize_decoded_pc(handler_pc);
        if (!synchronized)
          return std::unexpected(synchronized.error());
        current_instruction_pc_ = handler_pc;
        return {};
      }

      [[nodiscard]] Result<u8> read_u8()
      {
        if (pc_ >= code_.size())
        {
          return fail(ErrorCode::malformed_class,
                      "bytecode read exceeds method body");
        }
        return code_[pc_++];
      }

      [[nodiscard]] Result<i8> read_i8()
      {
        auto value = read_u8();
        if (!value)
        {
          return std::unexpected(value.error());
        }
        return static_cast<i8>(*value);
      }

      [[nodiscard]] Result<u16> read_constant_pool_index()
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        auto decoded_operand = current_decoded_operand(
            DecodedOperandKind::constant_pool_index);
        if (!decoded_operand)
          return std::unexpected(decoded_operand.error());
        if (*decoded_operand != nullptr)
        {
          auto advanced = finish_decoded_operand();
          if (!advanced)
            return std::unexpected(advanced.error());
          PerformanceCounters::record_decoded_operand_dispatch();
          return (*decoded_operand)->constant_pool_index;
        }
#endif
        return read_u16();
      }

      [[nodiscard]] Result<InvokeInterfaceOperands>
      read_invokeinterface_operands()
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        auto decoded_operand = current_decoded_operand(
            DecodedOperandKind::invokeinterface);
        if (!decoded_operand)
          return std::unexpected(decoded_operand.error());
        if (*decoded_operand != nullptr)
        {
          const InvokeInterfaceOperands result {
              .constant_pool_index = (*decoded_operand)->constant_pool_index,
              .count = (*decoded_operand)->auxiliary,
          };
          auto advanced = finish_decoded_operand();
          if (!advanced)
            return std::unexpected(advanced.error());
          PerformanceCounters::record_decoded_operand_dispatch();
          return result;
        }
#endif
        auto index = read_u16();
        auto count = read_u8();
        auto zero = read_u8();
        if (!index || !count || !zero || *count == 0U || *zero != 0U)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid invokeinterface operands");
        }
        return InvokeInterfaceOperands {
            .constant_pool_index = *index,
            .count = *count,
        };
      }

      [[nodiscard]] Result<i32> read_immediate(bool wide)
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        auto decoded_operand = current_decoded_operand(
            DecodedOperandKind::immediate);
        if (!decoded_operand)
          return std::unexpected(decoded_operand.error());
        if (*decoded_operand != nullptr)
        {
          const i32 value = (*decoded_operand)->immediate;
          auto advanced = finish_decoded_operand();
          if (!advanced)
            return std::unexpected(advanced.error());
          PerformanceCounters::record_decoded_operand_dispatch();
          return value;
        }
#endif
        if (wide)
        {
          auto value = read_i16();
          if (!value) return std::unexpected(value.error());
          return static_cast<i32>(*value);
        }
        auto value = read_i8();
        if (!value) return std::unexpected(value.error());
        return static_cast<i32>(*value);
      }

      [[nodiscard]] Result<u16> read_ldc_index(bool wide)
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        auto decoded_operand = current_decoded_operand(
            DecodedOperandKind::constant_pool_index);
        if (!decoded_operand)
          return std::unexpected(decoded_operand.error());
        if (*decoded_operand != nullptr)
        {
          const u16 value = (*decoded_operand)->constant_pool_index;
          auto advanced = finish_decoded_operand();
          if (!advanced)
            return std::unexpected(advanced.error());
          PerformanceCounters::record_decoded_operand_dispatch();
          return value;
        }
#endif
        if (wide) return read_u16();
        auto value = read_u8();
        if (!value) return std::unexpected(value.error());
        return static_cast<u16>(*value);
      }

      [[nodiscard]] Result<u16> read_local_index()
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        auto decoded_operand = current_decoded_operand(
            DecodedOperandKind::local_index);
        if (!decoded_operand)
          return std::unexpected(decoded_operand.error());
        if (*decoded_operand != nullptr)
        {
          const u16 value = (*decoded_operand)->local_index;
          auto advanced = finish_decoded_operand();
          if (!advanced)
            return std::unexpected(advanced.error());
          PerformanceCounters::record_decoded_operand_dispatch();
          return value;
        }
#endif
        auto value = read_u8();
        if (!value) return std::unexpected(value.error());
        return static_cast<u16>(*value);
      }

      [[nodiscard]] Result<IncrementOperands> read_increment_operands()
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        auto decoded_operand = current_decoded_operand(
            DecodedOperandKind::increment);
        if (!decoded_operand)
          return std::unexpected(decoded_operand.error());
        if (*decoded_operand != nullptr)
        {
          const IncrementOperands result {
              .local_index = (*decoded_operand)->local_index,
              .increment = (*decoded_operand)->immediate,
          };
          auto advanced = finish_decoded_operand();
          if (!advanced)
            return std::unexpected(advanced.error());
          PerformanceCounters::record_decoded_operand_dispatch();
          return result;
        }
#endif
        auto index = read_u8();
        auto increment = read_i8();
        if (!index || !increment)
        {
          return fail(ErrorCode::malformed_class,
                      "truncated iinc instruction");
        }
        return IncrementOperands {
            .local_index = *index,
            .increment = *increment,
        };
      }

      [[nodiscard]] Result<i32> read_branch_offset(bool wide)
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        auto decoded_operand = current_decoded_operand(
            DecodedOperandKind::branch_target);
        if (!decoded_operand)
          return std::unexpected(decoded_operand.error());
        if (*decoded_operand != nullptr)
        {
          const i32 value = (*decoded_operand)->immediate;
          auto advanced = finish_decoded_operand();
          if (!advanced)
            return std::unexpected(advanced.error());
          PerformanceCounters::record_decoded_operand_dispatch();
          return value;
        }
#endif
        if (wide) return read_i32();
        auto value = read_i16();
        if (!value) return std::unexpected(value.error());
        return static_cast<i32>(*value);
      }

      [[nodiscard]] Result<const DecodedSwitchTable*> read_switch_table()
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        auto decoded_operand = current_decoded_operand(
            DecodedOperandKind::switch_table);
        if (!decoded_operand)
          return std::unexpected(decoded_operand.error());
        if (*decoded_operand != nullptr)
        {
          const u32 switch_index = (*decoded_operand)->switch_index;
          if (switch_index >= decoded_->switches.size())
          {
            return fail(ErrorCode::malformed_class,
                        "decoded switch table index is invalid");
          }
          const DecodedSwitchTable* table = &decoded_->switches[switch_index];
          auto advanced = finish_decoded_operand();
          if (!advanced)
            return std::unexpected(advanced.error());
          PerformanceCounters::record_decoded_operand_dispatch();
          return table;
        }
#endif
        return static_cast<const DecodedSwitchTable*>(nullptr);
      }

      [[nodiscard]] Result<u16> read_invokedynamic_index()
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        auto decoded_operand = current_decoded_operand(
            DecodedOperandKind::invokedynamic);
        if (!decoded_operand)
          return std::unexpected(decoded_operand.error());
        if (*decoded_operand != nullptr)
        {
          const u16 value = (*decoded_operand)->constant_pool_index;
          auto advanced = finish_decoded_operand();
          if (!advanced)
            return std::unexpected(advanced.error());
          PerformanceCounters::record_decoded_operand_dispatch();
          return value;
        }
#endif
        auto index = read_u16();
        auto zero1 = read_u8();
        auto zero2 = read_u8();
        if (!index || !zero1 || !zero2 || *zero1 != 0U || *zero2 != 0U)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid invokedynamic operands");
        }
        return *index;
      }

      [[nodiscard]] Result<u8> read_newarray_type()
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        auto decoded_operand = current_decoded_operand(
            DecodedOperandKind::newarray_type);
        if (!decoded_operand)
          return std::unexpected(decoded_operand.error());
        if (*decoded_operand != nullptr)
        {
          const u8 value = (*decoded_operand)->auxiliary;
          auto advanced = finish_decoded_operand();
          if (!advanced)
            return std::unexpected(advanced.error());
          PerformanceCounters::record_decoded_operand_dispatch();
          return value;
        }
#endif
        return read_u8();
      }

      [[nodiscard]] Result<WideOperands> read_wide_operands()
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        auto decoded_operand = current_decoded_operand_unchecked();
        if (!decoded_operand)
          return std::unexpected(decoded_operand.error());
        if (*decoded_operand != nullptr)
        {
          if ((*decoded_operand)->kind != DecodedOperandKind::wide_local &&
              (*decoded_operand)->kind != DecodedOperandKind::wide_increment)
          {
            return fail(ErrorCode::malformed_class,
                        "decoded wide operand kind does not match opcode");
          }
          WideOperands result {
              .opcode = (*decoded_operand)->modified_opcode,
              .local_index = (*decoded_operand)->local_index,
              .increment = std::nullopt,
          };
          if ((*decoded_operand)->kind == DecodedOperandKind::wide_increment)
          {
            result.increment = static_cast<i16>((*decoded_operand)->immediate);
          }
          auto advanced = finish_decoded_operand();
          if (!advanced)
            return std::unexpected(advanced.error());
          PerformanceCounters::record_decoded_operand_dispatch();
          return result;
        }
#endif
        auto widened_opcode = read_u8();
        auto index = read_u16();
        if (!widened_opcode || !index)
        {
          return fail(ErrorCode::malformed_class,
                      "truncated wide instruction");
        }
        WideOperands result {
            .opcode = *widened_opcode,
            .local_index = *index,
            .increment = std::nullopt,
        };
        if (*widened_opcode == 0x84U)
        {
          auto increment = read_i16();
          if (!increment) return std::unexpected(increment.error());
          result.increment = *increment;
        }
        return result;
      }

      [[nodiscard]] Result<MultiArrayOperands> read_multianewarray_operands()
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        auto decoded_operand = current_decoded_operand(
            DecodedOperandKind::multianewarray);
        if (!decoded_operand)
          return std::unexpected(decoded_operand.error());
        if (*decoded_operand != nullptr)
        {
          const MultiArrayOperands result {
              .constant_pool_index = (*decoded_operand)->constant_pool_index,
              .dimensions = (*decoded_operand)->auxiliary,
          };
          auto advanced = finish_decoded_operand();
          if (!advanced)
            return std::unexpected(advanced.error());
          PerformanceCounters::record_decoded_operand_dispatch();
          return result;
        }
#endif
        auto index = read_u16();
        auto dimensions = read_u8();
        if (!index || !dimensions || *dimensions == 0U)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid multianewarray operands");
        }
        return MultiArrayOperands {
            .constant_pool_index = *index,
            .dimensions = *dimensions,
        };
      }

      [[nodiscard]] Result<u16> read_u16()
      {
        auto high = read_u8();
        auto low = read_u8();
        if (!high || !low)
        {
          return fail(ErrorCode::malformed_class,
                      "truncated 16-bit bytecode operand");
        }
        return static_cast<u16>((static_cast<u16>(*high) << 8U) |
                                static_cast<u16>(*low));
      }

      [[nodiscard]] Result<i16> read_i16()
      {
        auto value = read_u16();
        if (!value)
        {
          return std::unexpected(value.error());
        }
        return static_cast<i16>(*value);
      }

      [[nodiscard]] Status read_switch_padding()
      {
        // Legacy J2ME obfuscators sometimes leave arbitrary data in the
        // alignment gap. The bytes are not executable; consume them while
        // keeping the payload bounds checks strict.
        while ((pc_ & 3U) != 0U)
        {
          auto padding = read_u8();
          if (!padding)
          {
            return std::unexpected(padding.error());
          }
        }
        return {};
      }

      [[nodiscard]] Result<i32> read_i32()
      {
        auto first = read_u8();
        auto second = read_u8();
        auto third = read_u8();
        auto fourth = read_u8();
        if (!first || !second || !third || !fourth)
        {
          return fail(ErrorCode::malformed_class,
                      "truncated 32-bit bytecode operand");
        }
        const u32 bits = (static_cast<u32>(*first) << 24U) |
                         (static_cast<u32>(*second) << 16U) |
                         (static_cast<u32>(*third) << 8U) |
                         static_cast<u32>(*fourth);
        return static_cast<i32>(bits);
      }

      [[nodiscard]] Status jump_absolute(usize target)
      {
        if (target >= code_.size())
        {
          return fail(ErrorCode::malformed_class,
                      "bytecode absolute target is out of range");
        }
        pc_ = target;
        return synchronize_decoded_pc(target);
      }

      [[nodiscard]] Status branch(usize opcode_pc, i64 relative_offset)
      {
        const i64 origin = static_cast<i64>(opcode_pc);
        if ((relative_offset > 0 &&
             origin > std::numeric_limits<i64>::max() - relative_offset) ||
            (relative_offset < 0 &&
             origin < std::numeric_limits<i64>::min() - relative_offset))
        {
          return fail(ErrorCode::malformed_class,
                      "bytecode branch calculation overflowed");
        }
        const i64 target = origin + relative_offset;
        if (target < 0 ||
            static_cast<u64>(target) >= static_cast<u64>(code_.size()))
        {
          return fail(ErrorCode::malformed_class,
                      "bytecode branch target is out of range");
        }
        pc_ = static_cast<usize>(target);
        return synchronize_decoded_pc(pc_);
      }

      [[nodiscard]] Status push(Value value) { return stack_.push(value); }
      [[nodiscard]] Result<Value> pop() { return stack_.pop(); }
      [[nodiscard]] Status push_compact(CompactStackValue value)
      {
        return stack_.push_compact(value);
      }
      [[nodiscard]] Result<CompactStackValue> pop_compact()
      {
        return stack_.pop_compact();
      }
      [[nodiscard]] Status push_int(i32 value) { return stack_.push_int(value); }
      [[nodiscard]] Status push_long(i64 value) { return stack_.push_long(value); }
      [[nodiscard]] Status push_float(float value) { return stack_.push_float(value); }
      [[nodiscard]] Status push_double(double value) { return stack_.push_double(value); }
      [[nodiscard]] Status push_reference(ObjectRef value)
      {
        return stack_.push_reference(value);
      }
      [[nodiscard]] Result<i32> pop_int() { return stack_.pop_int(); }
      [[nodiscard]] Result<i64> pop_long() { return stack_.pop_long(); }
      [[nodiscard]] Result<float> pop_float() { return stack_.pop_float(); }
      [[nodiscard]] Result<double> pop_double() { return stack_.pop_double(); }
      [[nodiscard]] Result<ObjectRef> pop_reference()
      {
        return stack_.pop_reference();
      }
      [[nodiscard]] Result<Value> local(usize index) const
      {
        return locals_.get(index);
      }
      [[nodiscard]] Result<i32> local_int(usize index) const
      {
        return locals_.get_int(index);
      }
      [[nodiscard]] Result<i64> local_long(usize index) const
      {
        return locals_.get_long(index);
      }
      [[nodiscard]] Result<float> local_float(usize index) const
      {
        return locals_.get_float(index);
      }
      [[nodiscard]] Result<double> local_double(usize index) const
      {
        return locals_.get_double(index);
      }
      [[nodiscard]] Result<ObjectRef> local_reference(usize index) const
      {
        return locals_.get_reference(index);
      }
      [[nodiscard]] Status set_local(usize index, Value value)
      {
        return locals_.set(index, value);
      }
      [[nodiscard]] Status set_local_int(usize index, i32 value)
      {
        return locals_.set_int(index, value);
      }
      [[nodiscard]] Status set_local_long(usize index, i64 value)
      {
        return locals_.set_long(index, value);
      }
      [[nodiscard]] Status set_local_float(usize index, float value)
      {
        return locals_.set_float(index, value);
      }
      [[nodiscard]] Status set_local_double(usize index, double value)
      {
        return locals_.set_double(index, value);
      }
      [[nodiscard]] Status set_local_reference(usize index, ObjectRef value)
      {
        return locals_.set_reference(index, value);
      }
      void set_synchronized_monitor(ObjectRef monitor) noexcept
      {
        synchronized_monitor_ = monitor;
      }
      [[nodiscard]] std::optional<ObjectRef> synchronized_monitor() const noexcept
      {
        return synchronized_monitor_;
      }
      void set_return_override(Value value, bool boxes_result = false) noexcept
      {
        return_override_ = value;
        return_override_boxes_result_ = boxes_result;
      }
      [[nodiscard]] std::optional<Value> return_override() const noexcept
      {
        return return_override_;
      }
      [[nodiscard]] bool return_override_boxes_result() const noexcept
      {
        return return_override_boxes_result_;
      }
      void set_discard_return_value(bool discard) noexcept
      {
        discard_return_value_ = discard;
      }
      [[nodiscard]] bool discard_return_value() const noexcept
      {
        return discard_return_value_;
      }
      void set_return_reference_cast(std::string target)
      {
        return_reference_cast_target_ = std::move(target);
      }
      [[nodiscard]] const std::optional<std::string>&
      return_reference_cast_target() const noexcept
      {
        return return_reference_cast_target_;
      }
      void set_return_unboxing(JavaTypeKind target) noexcept
      {
        return_unboxing_target_ = target;
      }
      [[nodiscard]] std::optional<JavaTypeKind>
      return_unboxing_target() const noexcept
      {
        return return_unboxing_target_;
      }
      void set_return_widening(JavaTypeKind source,
                               JavaTypeKind target) noexcept
      {
        return_widening_source_ = source;
        return_widening_target_ = target;
      }
      [[nodiscard]] std::optional<JavaTypeKind>
      return_widening_source() const noexcept
      {
        return return_widening_source_;
      }
      [[nodiscard]] std::optional<JavaTypeKind>
      return_widening_target() const noexcept
      {
        return return_widening_target_;
      }
      [[nodiscard]] usize jit_physical_slot_count() const noexcept
      {
        return locals_.slot_count() + stack_.used_slots();
      }

      [[nodiscard]] Status write_jit_frame_bits(std::span<u64> output) const
      {
        const usize required = jit_physical_slot_count();
        if (output.size() < required)
        {
          return fail(ErrorCode::out_of_range,
                      "JIT physical frame buffer is too small");
        }
        auto locals_written = locals_.write_jit_physical_bits(output);
        if (!locals_written)
          return std::unexpected(locals_written.error());
        auto stack_written = stack_.write_jit_physical_bits(
            output.subspan(*locals_written));
        if (!stack_written)
          return std::unexpected(stack_written.error());
        if (*locals_written + *stack_written != required)
        {
          return fail(ErrorCode::internal_error,
                      "JIT physical frame writer produced wrong slot count");
        }
        return {};
      }

      [[nodiscard]] usize operand_stack_slots() const noexcept
      {
        return stack_.used_slots();
      }

      [[nodiscard]] Status restore_jit_deopt_state(
          const JitDeoptState& state)
      {
        return restore_jit_physical_frame(state.view());
      }

      [[nodiscard]] Status restore_jit_physical_frame(
          JitPhysicalFrameView state)
      {
        const usize local_slots = static_cast<usize>(state.local_slots);
        const usize stack_slots = static_cast<usize>(state.stack_slots);
        const usize expected_slots = local_slots + stack_slots;
        const VerifiedReferenceMap* verified = verified_frame_at(
            state.bytecode_pc);
        if (local_slots != locals_.slot_count() ||
            state.bytecode_pc >= code_.size() ||
            !state.valid() ||
            verified == nullptr || verified->stack_slots != stack_slots ||
            verified->slot_kinds.size() != expected_slots)
        {
          return fail(ErrorCode::invalid_state,
                      "JIT deopt state does not match interpreter frame");
        }

        static_assert(
            static_cast<u8>(VerifiedSlotKind::empty) ==
                static_cast<u8>(ValueKind::empty) &&
            static_cast<u8>(VerifiedSlotKind::continuation) ==
                static_cast<u8>(ValueKind::continuation) &&
            static_cast<u8>(VerifiedSlotKind::int32) ==
                static_cast<u8>(ValueKind::int32) &&
            static_cast<u8>(VerifiedSlotKind::int64) ==
                static_cast<u8>(ValueKind::int64) &&
            static_cast<u8>(VerifiedSlotKind::float32) ==
                static_cast<u8>(ValueKind::float32) &&
            static_cast<u8>(VerifiedSlotKind::float64) ==
                static_cast<u8>(ValueKind::float64) &&
            static_cast<u8>(VerifiedSlotKind::reference) ==
                static_cast<u8>(ValueKind::reference) &&
            static_cast<u8>(VerifiedSlotKind::return_address) ==
                static_cast<u8>(ValueKind::return_address),
            "verified and compact slot kinds must share the physical ABI");
        const auto compact_kind = [](VerifiedSlotKind kind) noexcept {
          return static_cast<ValueKind>(static_cast<u8>(kind));
        };

        locals_.clear();
        for (usize index = 0U; index < local_slots; ++index)
        {
          const VerifiedSlotKind kind = verified->slot_kinds[index];
          if (kind == VerifiedSlotKind::empty ||
              kind == VerifiedSlotKind::continuation)
          {
            continue;
          }
          auto stored = locals_.set_compact(
              index, compact_kind(kind), state.physical_slots[index]);
          if (!stored)
            return std::unexpected(stored.error());
        }
        stack_.clear();
        for (usize slot = 0U; slot < stack_slots; ++slot)
        {
          const usize physical = local_slots + slot;
          const VerifiedSlotKind kind = verified->slot_kinds[physical];
          if (kind == VerifiedSlotKind::empty ||
              kind == VerifiedSlotKind::continuation)
          {
            continue;
          }
          auto pushed = stack_.push_compact(CompactStackValue {
              .kind = compact_kind(kind),
              .bits = state.physical_slots[physical],
          });
          if (!pushed)
            return std::unexpected(pushed.error());
        }
        if (stack_.used_slots() != stack_slots)
        {
          return fail(ErrorCode::invalid_state,
                      "JIT deopt stack layout does not match verifier frame");
        }
        pc_ = state.bytecode_pc;
        current_instruction_pc_ = state.bytecode_pc;
        return synchronize_decoded_pc(state.bytecode_pc);
      }

      void append_reference_roots(std::vector<ObjectRef> &roots) const
      {
        const usize roots_before_frame = roots.size();
        bool full_verified_map = false;
        bool partial_verified_map = false;
        usize verified_slots_visited = 0U;
        usize verified_slots_avoided = 0U;
        usize fallback_slots_scanned = 0U;
        if (const VerifiedReferenceMap* map = verified_frame_at(pc_);
            map != nullptr && resolved_.runtime != nullptr &&
            resolved_.runtime->verified_frames->max_locals ==
                locals_.slot_count())
        {
          const auto stack_begin = std::lower_bound(
                    map->reference_slots.begin(),
                    map->reference_slots.end(),
                    locals_.slot_count());
                const usize local_reference_count = static_cast<usize>(
                    std::distance(map->reference_slots.begin(), stack_begin));
                const std::span<const usize> local_reference_slots(
                    map->reference_slots.data(), local_reference_count);
                const std::span<const usize> stack_reference_slots(
                    map->reference_slots.data() + local_reference_count,
                    map->reference_slots.size() - local_reference_count);
                if (locals_.append_reference_roots_at_slots(
                        local_reference_slots, roots))
                {
                  verified_slots_visited += local_reference_slots.size();
                  if (locals_.slot_count() > local_reference_slots.size())
                  {
                    verified_slots_avoided +=
                        locals_.slot_count() - local_reference_slots.size();
                  }
                  const usize roots_before_stack = roots.size();
                  if (map->stack_slots == stack_.used_slots() &&
                      stack_.append_reference_roots_at_physical_slots(
                          stack_reference_slots,
                          locals_.slot_count(),
                          roots))
                  {
                    verified_slots_visited += stack_reference_slots.size();
                    if (stack_.used_slots() > stack_reference_slots.size())
                    {
                      verified_slots_avoided +=
                          stack_.used_slots() - stack_reference_slots.size();
                    }
                    full_verified_map = true;
                  }
                  else
                  {
                    roots.resize(roots_before_stack);
                    stack_.append_reference_roots(roots);
                    fallback_slots_scanned += stack_.used_slots();
                    partial_verified_map = true;
                  }
                }
                else
                {
                  roots.resize(roots_before_frame);
                }
        }
        if (!full_verified_map && !partial_verified_map)
        {
          locals_.append_reference_roots(roots);
          stack_.append_reference_roots(roots);
          fallback_slots_scanned += locals_.slot_count() + stack_.used_slots();
        }
        PerformanceCounters::record_verified_root_scan(
            full_verified_map,
            partial_verified_map,
            verified_slots_visited,
            verified_slots_avoided,
            fallback_slots_scanned);
        if (synchronized_monitor_.has_value() &&
            !synchronized_monitor_->is_null())
        {
          roots.push_back(*synchronized_monitor_);
        }
        if (return_override_.has_value() &&
            return_override_->kind() == ValueKind::reference)
        {
          auto reference = return_override_->as_reference();
          if (reference && !reference->is_null())
            roots.push_back(*reference);
        }
      }

    public:
      // ExecutionFrame is translation-unit private; making this constructor
      // public lets std::vector::emplace_back build the frame directly in its
      // reserved call-stack storage instead of constructing and moving a large
      // temporary containing the inline local/operand banks.
      ExecutionFrame(ResolvedMethod resolved,
                     std::shared_ptr<const CachedMethodDescriptor> descriptor,
                     bool has_receiver)
          : resolved_(std::move(resolved)),
            descriptor_(std::move(descriptor)),
            has_receiver_(has_receiver),
            code_(resolved_.method->code->bytecode),
            decoded_(decoded_execution_requested() &&
                             resolved_.runtime != nullptr
                         ? resolved_.runtime->decoded.get()
                         : nullptr),
            locals_(resolved_.method->code->max_locals),
            stack_(resolved_.method->code->max_stack)
      {
        const usize local_slots = resolved_.method->code->max_locals;
        const usize operand_slots = resolved_.method->code->max_stack;
        PerformanceCounters::observe_execution_frame_slots(
            local_slots,
            operand_slots,
            local_slots > kInlineFrameSlotCapacity,
            operand_slots > kInlineFrameSlotCapacity);
      }

    private:
      [[nodiscard]] Result<const DecodedOperand*>
      current_decoded_operand_unchecked() const
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        if (decoded_ == nullptr)
          return static_cast<const DecodedOperand*>(nullptr);
        if (current_decoded_index_ == kInvalidDecodedIndex ||
            current_decoded_index_ >= decoded_->instructions.size())
        {
          return fail(ErrorCode::malformed_class,
                      "decoded operand has no current instruction");
        }
        const DecodedInstruction& instruction =
            decoded_->instructions[current_decoded_index_];
        if (instruction.operand_index == kInvalidDecodedIndex ||
            instruction.operand_index >= decoded_->operands.size())
        {
          return fail(ErrorCode::malformed_class,
                      "decoded instruction has no operand metadata");
        }
        return &decoded_->operands[instruction.operand_index];
#else
        return static_cast<const DecodedOperand*>(nullptr);
#endif
      }

      [[nodiscard]] Result<const DecodedOperand*> current_decoded_operand(
          DecodedOperandKind expected_kind) const
      {
        auto operand = current_decoded_operand_unchecked();
        if (!operand)
          return std::unexpected(operand.error());
        if (*operand != nullptr && (*operand)->kind != expected_kind)
        {
          return fail(ErrorCode::malformed_class,
                      "decoded operand kind does not match opcode");
        }
        return *operand;
      }

      [[nodiscard]] Status finish_decoded_operand()
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        if (decoded_ == nullptr)
          return {};
        if (current_decoded_index_ == kInvalidDecodedIndex ||
            current_decoded_index_ >= decoded_->instructions.size())
        {
          return fail(ErrorCode::malformed_class,
                      "decoded operand cannot advance without an instruction");
        }
        const u32 next_index =
            decoded_->instructions[current_decoded_index_].next_index;
        if (next_index == decoded_->instructions.size())
        {
          pc_ = code_.size();
          return {};
        }
        if (next_index >= decoded_->instructions.size())
        {
          return fail(ErrorCode::malformed_class,
                      "decoded operand next index is outside method");
        }
        pc_ = decoded_->instructions[next_index].bytecode_pc;
#endif
        return {};
      }

      [[nodiscard]] Status synchronize_decoded_pc(usize target)
      {
#if PHONEME_ENABLE_DECODED_EXECUTION
        if (decoded_ != nullptr)
        {
          if (target > std::numeric_limits<u32>::max())
          {
            return fail(ErrorCode::malformed_class,
                        "decoded branch target exceeds index space");
          }
          const u32 instruction_index = decoded_->instruction_index_for_bci(
              static_cast<u32>(target));
          if (instruction_index == kInvalidDecodedIndex ||
              instruction_index >= decoded_->instructions.size())
          {
            return fail(ErrorCode::malformed_class,
                        "decoded branch target is not an instruction boundary");
          }
          decoded_index_ = instruction_index;
        }
#else
        (void)target;
#endif
        return {};
      }

      ResolvedMethod resolved_;
      std::shared_ptr<const CachedMethodDescriptor> descriptor_;
      bool has_receiver_{false};
      std::span<const u8> code_;
      const DecodedMethod* decoded_{nullptr};
      [[maybe_unused]] u32 decoded_index_{0U};
      [[maybe_unused]] u32 current_decoded_index_{kInvalidDecodedIndex};
      LocalVariables locals_;
      OperandStack stack_;
      usize pc_{0};
      usize current_instruction_pc_{0};
      u32 osr_backedges_{0U};
      u32 osr_attempt_count_{0U};
      std::optional<ObjectRef> synchronized_monitor_;
      std::optional<Value> return_override_;
      bool return_override_boxes_result_{false};
      bool discard_return_value_{false};
      std::optional<std::string> return_reference_cast_target_;
      std::optional<JavaTypeKind> return_unboxing_target_;
      std::optional<JavaTypeKind> return_widening_source_;
      std::optional<JavaTypeKind> return_widening_target_;
    };

    // Reuse the vector backing store across top-level execute() calls while
    // still supporting recursive execute() from JIT/native bridges. Each host
    // thread owns a small stack of vectors indexed by execute nesting depth;
    // the vector object itself is heap-stable through unique_ptr even if the
    // outer pool grows during a nested invocation.
    class ExecutionFrameStackLease final
    {
    public:
      ExecutionFrameStackLease()
      {
        Pool& current = pool();
        index_ = current.depth++;
        if (index_ >= current.stacks.size())
        {
          auto stack = std::make_unique<std::vector<ExecutionFrame>>();
          stack->reserve(kWarmFrameCapacity);
          current.stacks.push_back(std::move(stack));
          PerformanceCounters::record_execution_frame_stack_pool_miss();
        }
        frames_ = current.stacks[index_].get();
        frames_->clear();
      }

      ~ExecutionFrameStackLease()
      {
        frames_->clear();
        Pool& current = pool();
        if (current.depth > 0U)
          --current.depth;
      }

      ExecutionFrameStackLease(const ExecutionFrameStackLease&) = delete;
      ExecutionFrameStackLease& operator=(const ExecutionFrameStackLease&) = delete;

      [[nodiscard]] std::vector<ExecutionFrame>& frames() noexcept
      {
        return *frames_;
      }

    private:
      static constexpr usize kWarmFrameCapacity = 32U;

      struct Pool final
      {
        std::vector<std::unique_ptr<std::vector<ExecutionFrame>>> stacks;
        usize depth {0U};
      };

      [[nodiscard]] static Pool& pool() noexcept
      {
        thread_local Pool current;
        return current;
      }

      usize index_ {0U};
      std::vector<ExecutionFrame>* frames_ {nullptr};
    };

    void append_execution_frame_roots(void* context,
                                      std::vector<ObjectRef>& roots) noexcept
    {
      const auto* frames =
          static_cast<const std::vector<ExecutionFrame>*>(context);
      if (frames == nullptr)
        return;
      for (const ExecutionFrame& frame : *frames)
        frame.append_reference_roots(roots);
    }

    [[nodiscard]] Result<i32> pop_int(ExecutionFrame &frame)
    {
      return frame.pop_int();
    }

    [[nodiscard]] Result<i64> pop_long(ExecutionFrame &frame)
    {
      return frame.pop_long();
    }

    [[nodiscard]] Result<float> pop_float(ExecutionFrame &frame)
    {
      return frame.pop_float();
    }

    [[nodiscard]] Result<double> pop_double(ExecutionFrame &frame)
    {
      return frame.pop_double();
    }

    [[nodiscard]] Result<ObjectRef> pop_reference(ExecutionFrame &frame)
    {
      return frame.pop_reference();
    }

    [[nodiscard]] Result<InvocationArguments> pop_arguments(
        ExecutionFrame &caller,
        const MethodDescriptor &descriptor,
        bool include_receiver,
        bool validate_types)
    {
      const usize value_count = descriptor.parameters.size() +
                                (include_receiver ? 1U : 0U);
      InvocationArguments arguments(value_count);

      if (!validate_types)
      {
        // Verified bytecode already proves the operand kinds at this invoke
        // boundary. Keep the values in compact slot form instead of
        // materializing a tagged Value for every argument only to immediately
        // split it back into kind + payload inside InvocationArguments.
        for (usize reverse = descriptor.parameters.size(); reverse > 0;
             --reverse)
        {
          const usize parameter_index = reverse - 1U;
          auto value = caller.pop_compact();
          if (!value)
            return std::unexpected(value.error());
          arguments.set_compact(
              parameter_index + (include_receiver ? 1U : 0U),
              value->kind,
              value->bits);
        }

        if (include_receiver)
        {
          auto receiver = caller.pop_compact();
          if (!receiver)
            return std::unexpected(receiver.error());
          // Keep this cheap guard even on verified code so a corrupted frame
          // cannot reinterpret primitive payload bits as an ObjectRef.
          if (receiver->kind != ValueKind::reference)
          {
            return fail(ErrorCode::malformed_class,
                        "verified invoke receiver is not a reference");
          }
          arguments.set_compact(0U, receiver->kind, receiver->bits);
        }
        return arguments;
      }

      for (usize reverse = descriptor.parameters.size(); reverse > 0; --reverse)
      {
        const usize parameter_index = reverse - 1;
        auto value = caller.pop();
        if (!value)
        {
          return std::unexpected(value.error());
        }
        if (!value_matches(*value, descriptor.parameters[parameter_index]))
        {
          return fail(
              ErrorCode::malformed_class,
              "method argument does not match its descriptor in " +
                  caller.owner().name() + "." + caller.method().name +
                  caller.method().descriptor + " at bytecode " +
                  std::to_string(caller.current_instruction_pc()) +
                  " parameter=" + std::to_string(parameter_index) +
                  " expectedKind=" + std::to_string(static_cast<unsigned>(
                      descriptor.parameters[parameter_index].kind)) +
                  " actualKind=" + std::to_string(static_cast<unsigned>(
                      value->kind())));
        }
        arguments.set(parameter_index + (include_receiver ? 1U : 0U), *value);
      }

      if (include_receiver)
      {
        auto receiver = caller.pop();
        if (!receiver)
        {
          return std::unexpected(receiver.error());
        }
        auto reference = receiver->as_reference();
        if (!reference)
        {
          return std::unexpected(reference.error());
        }
        arguments.set(0U, *receiver);
      }
      return arguments;
    }

    [[nodiscard]] Result<Value> constant_value(const classfile::ClassFile &owner,
                                               u16 index,
                                               bool category_two_only)
    {
      auto constant = owner.constant(index);
      if (!constant)
      {
        return std::unexpected(constant.error());
      }

      switch ((*constant)->kind)
      {
      case classfile::ConstantKind::integer:
        if (category_two_only)
        {
          break;
        }
        return Value::from_int(
            static_cast<i32>(static_cast<u32>((*constant)->bits)));
      case classfile::ConstantKind::float32:
        if (category_two_only)
        {
          break;
        }
        return Value::from_float(std::bit_cast<float>(
            static_cast<u32>((*constant)->bits)));
      case classfile::ConstantKind::long64:
        if (!category_two_only)
        {
          break;
        }
        return Value::from_long(static_cast<i64>((*constant)->bits));
      case classfile::ConstantKind::float64:
        if (!category_two_only)
        {
          break;
        }
        return Value::from_double(std::bit_cast<double>((*constant)->bits));
      default:
        break;
      }

      return fail(ErrorCode::unsupported_feature,
                  "ldc constant kind is not ported yet");
    }

    [[nodiscard]] Result<std::optional<i32>> integer_binary(ExecutionFrame &frame,
                                                            u8 opcode)
    {
      auto right = pop_int(frame);
      auto left = pop_int(frame);
      if (!right || !left)
      {
        return fail(ErrorCode::malformed_class,
                    "integer operation requires two int operands");
      }

      switch (opcode)
      {
      case 0x60:
        return static_cast<i32>(static_cast<u32>(*left) +
                                static_cast<u32>(*right));
      case 0x64:
        return static_cast<i32>(static_cast<u32>(*left) -
                                static_cast<u32>(*right));
      case 0x68:
        return static_cast<i32>(static_cast<u32>(*left) *
                                static_cast<u32>(*right));
      case 0x6C:
        if (*right == 0)
        {
          return std::optional<i32>{};
        }
        if (*left == std::numeric_limits<i32>::min() && *right == -1)
        {
          return std::numeric_limits<i32>::min();
        }
        return *left / *right;
      case 0x70:
        if (*right == 0)
        {
          return std::optional<i32>{};
        }
        if (*left == std::numeric_limits<i32>::min() && *right == -1)
        {
          return 0;
        }
        return *left % *right;
      case 0x7E:
        return *left & *right;
      case 0x80:
        return *left | *right;
      case 0x82:
        return *left ^ *right;
      default:
        return fail(ErrorCode::internal_error,
                    "unknown integer binary opcode");
      }
    }

    [[nodiscard]] Result<std::optional<i64>> long_binary(ExecutionFrame &frame,
                                                         u8 opcode)
    {
      auto right = pop_long(frame);
      auto left = pop_long(frame);
      if (!right || !left)
      {
        return fail(ErrorCode::malformed_class,
                    "long operation requires two long operands");
      }

      switch (opcode)
      {
      case 0x61:
        return static_cast<i64>(static_cast<u64>(*left) +
                                static_cast<u64>(*right));
      case 0x65:
        return static_cast<i64>(static_cast<u64>(*left) -
                                static_cast<u64>(*right));
      case 0x69:
        return static_cast<i64>(static_cast<u64>(*left) *
                                static_cast<u64>(*right));
      case 0x6D:
        if (*right == 0)
        {
          return std::optional<i64>{};
        }
        if (*left == std::numeric_limits<i64>::min() && *right == -1)
        {
          return std::numeric_limits<i64>::min();
        }
        return *left / *right;
      case 0x71:
        if (*right == 0)
        {
          return std::optional<i64>{};
        }
        if (*left == std::numeric_limits<i64>::min() && *right == -1)
        {
          return 0;
        }
        return *left % *right;
      case 0x7F:
        return *left & *right;
      case 0x81:
        return *left | *right;
      case 0x83:
        return *left ^ *right;
      default:
        return fail(ErrorCode::internal_error,
                    "unknown long binary opcode");
      }
    }

    [[nodiscard]] Result<float> float_binary(ExecutionFrame &frame, u8 opcode)
    {
      auto right = pop_float(frame);
      auto left = pop_float(frame);
      if (!right || !left)
      {
        return fail(ErrorCode::malformed_class,
                    "float operation requires two float operands");
      }
      switch (opcode)
      {
      case 0x62:
        return *left + *right;
      case 0x66:
        return *left - *right;
      case 0x6A:
        return *left * *right;
      case 0x6E:
        return *left / *right;
      case 0x72:
        return std::fmod(*left, *right);
      default:
        return fail(ErrorCode::internal_error,
                    "unknown float binary opcode");
      }
    }

    [[nodiscard]] Result<double> double_binary(ExecutionFrame &frame, u8 opcode)
    {
      auto right = pop_double(frame);
      auto left = pop_double(frame);
      if (!right || !left)
      {
        return fail(ErrorCode::malformed_class,
                    "double operation requires two double operands");
      }
      switch (opcode)
      {
      case 0x63:
        return *left + *right;
      case 0x67:
        return *left - *right;
      case 0x6B:
        return *left * *right;
      case 0x6F:
        return *left / *right;
      case 0x73:
        return std::fmod(*left, *right);
      default:
        return fail(ErrorCode::internal_error,
                    "unknown double binary opcode");
      }
    }

    template <typename Integer, typename Floating>
    [[nodiscard]] Integer java_fp_to_integral(Floating value) noexcept
    {
      if (std::isnan(value))
      {
        return 0;
      }
      const long double extended = static_cast<long double>(value);
      const long double minimum =
          static_cast<long double>(std::numeric_limits<Integer>::min());
      const long double maximum =
          static_cast<long double>(std::numeric_limits<Integer>::max());
      if (extended <= minimum)
      {
        return std::numeric_limits<Integer>::min();
      }
      if (extended >= maximum)
      {
        return std::numeric_limits<Integer>::max();
      }
      return static_cast<Integer>(std::trunc(value));
    }

    [[nodiscard]] bool test_zero(u8 opcode, i32 value) noexcept
    {
      switch (opcode)
      {
      case 0x99:
        return value == 0;
      case 0x9A:
        return value != 0;
      case 0x9B:
        return value < 0;
      case 0x9C:
        return value >= 0;
      case 0x9D:
        return value > 0;
      case 0x9E:
        return value <= 0;
      default:
        return false;
      }
    }

    [[nodiscard]] bool test_int_compare(u8 opcode,
                                        i32 left,
                                        i32 right) noexcept
    {
      switch (opcode)
      {
      case 0x9F:
        return left == right;
      case 0xA0:
        return left != right;
      case 0xA1:
        return left < right;
      case 0xA2:
        return left >= right;
      case 0xA3:
        return left > right;
      case 0xA4:
        return left <= right;
      default:
        return false;
      }
    }

    [[nodiscard]] usize local_index_for_load(u8 opcode) noexcept
    {
      if (opcode >= 0x1A && opcode <= 0x1D)
        return opcode - 0x1A;
      if (opcode >= 0x1E && opcode <= 0x21)
        return opcode - 0x1E;
      if (opcode >= 0x22 && opcode <= 0x25)
        return opcode - 0x22;
      if (opcode >= 0x26 && opcode <= 0x29)
        return opcode - 0x26;
      return opcode - 0x2A;
    }

    [[nodiscard]] usize local_index_for_store(u8 opcode) noexcept
    {
      if (opcode >= 0x3B && opcode <= 0x3E)
        return opcode - 0x3B;
      if (opcode >= 0x3F && opcode <= 0x42)
        return opcode - 0x3F;
      if (opcode >= 0x43 && opcode <= 0x46)
        return opcode - 0x43;
      if (opcode >= 0x47 && opcode <= 0x4A)
        return opcode - 0x47;
      return opcode - 0x4B;
    }

    [[nodiscard]] std::optional<HeapArrayKind> array_load_heap_kind(
        u8 opcode) noexcept
    {
      switch (opcode)
      {
      case 0x2E: return HeapArrayKind::integer;
      case 0x2F: return HeapArrayKind::long_integer;
      case 0x30: return HeapArrayKind::float32;
      case 0x31: return HeapArrayKind::float64;
      case 0x32: return HeapArrayKind::reference;
      case 0x34: return HeapArrayKind::character;
      case 0x35: return HeapArrayKind::short_integer;
      default: return std::nullopt;
      }
    }

    [[nodiscard]] std::optional<HeapArrayKind> array_store_heap_kind(
        u8 opcode) noexcept
    {
      switch (opcode)
      {
      case 0x4F: return HeapArrayKind::integer;
      case 0x50: return HeapArrayKind::long_integer;
      case 0x51: return HeapArrayKind::float32;
      case 0x52: return HeapArrayKind::float64;
      case 0x53: return HeapArrayKind::reference;
      case 0x55: return HeapArrayKind::character;
      case 0x56: return HeapArrayKind::short_integer;
      default: return std::nullopt;
      }
    }

    [[nodiscard]] Result<Value> array_default_value(
        std::string_view descriptor)
    {
      if (descriptor.size() < 2U || descriptor.front() != '[')
      {
        return fail(ErrorCode::malformed_class,
                    "array descriptor has no component type");
      }
      switch (descriptor[1])
      {
      case 'Z':
      case 'B':
      case 'C':
      case 'S':
      case 'I':
        return Value::from_int(0);
      case 'J':
        return Value::from_long(0);
      case 'F':
        return Value::from_float(0.0F);
      case 'D':
        return Value::from_double(0.0);
      case 'L':
      case '[':
        return Value::from_reference({});
      default:
        return fail(ErrorCode::malformed_class,
                    "array descriptor has an invalid component type");
      }
    }

    [[nodiscard]] Result<std::pair<std::string, Value>> primitive_array_type(
        u8 atype)
    {
      switch (atype)
      {
      case 4:
        return std::pair<std::string, Value>("[Z", Value::from_int(0));
      case 5:
        return std::pair<std::string, Value>("[C", Value::from_int(0));
      case 6:
        return std::pair<std::string, Value>("[F", Value::from_float(0.0F));
      case 7:
        return std::pair<std::string, Value>("[D", Value::from_double(0.0));
      case 8:
        return std::pair<std::string, Value>("[B", Value::from_int(0));
      case 9:
        return std::pair<std::string, Value>("[S", Value::from_int(0));
      case 10:
        return std::pair<std::string, Value>("[I", Value::from_int(0));
      case 11:
        return std::pair<std::string, Value>("[J", Value::from_long(0));
      default:
        return fail(ErrorCode::malformed_class,
                    "newarray uses an invalid primitive type code");
      }
    }

  } // namespace

  Machine::Machine(ClassRepository &classes, usize maximum_heap_objects)
      : Machine(classes,
                HeapLimits {.maximum_objects = maximum_heap_objects})
  {
  }

  Machine::Machine(ClassRepository &classes, HeapLimits heap_limits)
      : classes_(classes), states_(classes),
        heap_(heap_limits_with_emergency_reserve(heap_limits)),
        permission_policy_(std::make_shared<security::PermissionPolicy>()),
        timers_(*this), media_events_(*this)
  {
    auto security_configured = permission_policy_->configure(
        security::PermissionPolicyConfig {
            .suite_id = SuiteId {1},
            .trust = security::SuiteTrust::trusted,
            .trusted_default_allow = true,
        });
    if (!security_configured)
      std::abort();

    // Per-mutation RMS persistence (full-store serialize + two fsyncs) turns
    // save-heavy games into a slideshow. Deferred mode batches writes; see
    // RecordStoreRegistry::flush_pending for the durability window.
    record_stores_.set_write_through(false);

    connections_.set_blocking_hooks(network::NetworkBlockingHooks {
        .before_block = [this] {
          scheduler_.set_current_state(JavaThreadState::blocked_io);
          return suspend_execution_for_blocking();
        },
        .after_block = [this](u32 depth) {
          resume_execution_after_blocking(depth);
          scheduler_.set_current_state(JavaThreadState::running);
        },
        .poll_cancellation = [this]() -> std::optional<Error> {
          if (!scheduler_.consume_current_interrupt())
            return std::nullopt;
          return Error::make_java(
              "java/io/InterruptedIOException",
              "network operation was interrupted");
        },
    });

    register_core_natives(natives_);

    // Cross-class JIT inlining is only legal after the target class has
    // completed JVM initialization. The resolver is also used by the
    // background compiler, so keep the existing class-init -> repository lock
    // ordering used by bounded_jit_invocation_cost().
    jit_.set_inline_resolver(JitInlineResolverHooks {
        .context = this,
        .resolve = [](void* context,
                      std::string_view owner,
                      std::string_view name,
                      std::string_view descriptor)
            -> std::optional<JitInlineTarget> {
          auto* machine = static_cast<Machine*>(context);
          std::scoped_lock initialization_lock(
              machine->class_initialization_mutex_);
          auto target = machine->classes_.resolve_method(
              owner, name, descriptor);
          if (!target || target->owner == nullptr || target->method == nullptr ||
              !machine->initialized_classes_.contains(target->owner->name())) {
            return std::nullopt;
          }
          return JitInlineTarget {
              .owner = std::move(target->owner),
              .method = target->method,
          };
        },
    });

    // CLDC bootstrap classes are initialized by the VM before application
    // class initialization begins. Treating them as ordinary game classes
    // would incorrectly re-run ROM/bootstrap initialization bytecode.
    initialized_classes_.insert("java/lang/Object");
    initialized_classes_.insert("java/lang/Class");
    initialized_classes_.insert("java/lang/String");
    initialized_classes_.insert("java/lang/System");
    initialized_classes_.insert("java/lang/Thread");
    initialized_classes_.insert("java/lang/Runtime");
    initialized_classes_.insert("java/lang/Float");
    initialized_classes_.insert("java/lang/Double");
    initialized_classes_.insert("java/lang/Math");

    auto emergency = states_.allocate_instance(
        heap_, "java/lang/OutOfMemoryError");
    if (!emergency)
      std::abort();
    emergency_out_of_memory_error_ = *emergency;
  }

  Machine::~Machine()
  {
    shutdown();
  }

  void Machine::shutdown() noexcept
  {
    if (shutdown_started_.exchange(true, std::memory_order_acq_rel))
      return;
    media_events_.shutdown();
    timers_.shutdown();
    // A worker may be waiting for another thread's class initializer. Wake it
    // before Scheduler::shutdown joins workers so teardown cannot deadlock on
    // the class-initialization condition.
    class_initialization_condition_.notify_all();
    scheduler_.shutdown(&monitors_);
    if (auto flushed = record_stores_.flush_all(); !flushed) {
      std::fprintf(stderr,
                   "[phoneME] RMS flush during shutdown failed: %s\n",
                   flushed.error().message.c_str());
      std::fflush(stderr);
    }
    dump_performance_summary_if_requested();
  }

  void Machine::dump_performance_summary_if_requested() noexcept
  {
    if (const char* dump = std::getenv("PHONEME_DUMP_PERF");
        dump == nullptr || *dump == '\0' || std::strcmp(dump, "0") == 0)
      return;
#if PHONEME_ENABLE_VM_PROFILING
    const auto counters = PerformanceCounters::snapshot();
    std::fprintf(stderr, "[phoneME-perf] bytecodes=%llu invocations=%llu "
                         "native_invocations=%llu exceptions=%llu "
                         "budget_exits=%llu\n",
                 static_cast<unsigned long long>(counters.executed_bytecodes),
                 static_cast<unsigned long long>(counters.method_invocations),
                 static_cast<unsigned long long>(counters.native_invocations),
                 static_cast<unsigned long long>(counters.exception_dispatches),
                 static_cast<unsigned long long>(
                     counters.instruction_budget_exits));
    std::fprintf(stderr,
                 "[phoneME-perf] frames depth_max=%llu locals_max=%llu "
                 "stack_max=%llu oversized=%llu callstack_grows=%llu "
                 "callstack_capacity_max=%llu pool_misses=%llu arg_overflows=%llu "
                 "arg_values_max=%llu\n",
                 static_cast<unsigned long long>(
                     counters.maximum_java_call_depth),
                 static_cast<unsigned long long>(
                     counters.maximum_frame_local_slots),
                 static_cast<unsigned long long>(
                     counters.maximum_frame_operand_slots),
                 static_cast<unsigned long long>(
                     counters.oversized_execution_frames),
                 static_cast<unsigned long long>(
                     counters.execution_frame_stack_growths),
                 static_cast<unsigned long long>(
                     counters.maximum_execution_frame_stack_capacity),
                 static_cast<unsigned long long>(
                     counters.execution_frame_stack_pool_misses),
                 static_cast<unsigned long long>(
                     counters.invocation_argument_overflows),
                 static_cast<unsigned long long>(
                     counters.maximum_invocation_argument_values));
    std::fprintf(stderr,
                 "[phoneME-perf] gc count=%llu total_ms=%.1f max_pause_ms=%.1f "
                 "roots_scanned=%llu objects_scanned=%llu reclaimed=%llu "
                 "primitive_kb=%.1f\n",
                 static_cast<unsigned long long>(counters.gc_count),
                 static_cast<double>(counters.gc_total_nanoseconds) / 1.0e6,
                 static_cast<double>(counters.gc_max_pause_nanoseconds) /
                     1.0e6,
                 static_cast<unsigned long long>(counters.gc_roots_scanned),
                 static_cast<unsigned long long>(counters.gc_objects_scanned),
                 static_cast<unsigned long long>(
                     counters.gc_objects_reclaimed),
                 static_cast<double>(counters.gc_primitive_bytes_scanned) /
                     1024.0);
    std::fprintf(stderr,
                 "[phoneME-perf] roots publications=%llu copy=%llu exchange=%llu "
                 "roots=%llu max=%llu jit_stages=%llu commits=%llu "
                 "materializations=%llu materialized_roots=%llu\n",
                 static_cast<unsigned long long>(
                     counters.execution_root_publications),
                 static_cast<unsigned long long>(
                     counters.execution_root_copy_publications),
                 static_cast<unsigned long long>(
                     counters.execution_root_exchange_publications),
                 static_cast<unsigned long long>(
                     counters.execution_roots_published),
                 static_cast<unsigned long long>(
                     counters.maximum_execution_roots_per_publication),
                 static_cast<unsigned long long>(counters.jit_root_stages),
                 static_cast<unsigned long long>(
                     counters.jit_root_stage_commits),
                 static_cast<unsigned long long>(
                     counters.jit_staged_root_materializations),
                 static_cast<unsigned long long>(
                     counters.jit_staged_roots_materialized));
    std::fprintf(stderr, "[phoneME-perf] allocations bytes=%llu "
                         "failed=%llu heap_locked=%llu heap_fast=%llu\n",
                 static_cast<unsigned long long>(
                     counters.allocated_bytes_by_kind[static_cast<usize>(
                         AllocationPayloadKind::object)]) +
                     counters.allocated_bytes_by_kind[static_cast<usize>(
                         AllocationPayloadKind::array)] +
                     counters.allocated_bytes_by_kind[static_cast<usize>(
                         AllocationPayloadKind::string_payload)],
                 static_cast<unsigned long long>(counters.failed_allocations),
                 static_cast<unsigned long long>(
                     counters.public_locked_heap_operations),
                 static_cast<unsigned long long>(
                     counters.vm_fast_heap_operations));
    std::fprintf(stderr,
                 "[phoneME-perf] allocation_detail object_count=%llu object_kb=%.1f "
                 "array_count=%llu array_kb=%.1f clone_count=%llu clone_kb=%.1f "
                 "string_growth_count=%llu string_growth_kb=%.1f\n",
                 static_cast<unsigned long long>(counters.allocations_by_kind[
                     static_cast<usize>(AllocationPayloadKind::object)]),
                 static_cast<double>(counters.allocated_bytes_by_kind[
                     static_cast<usize>(AllocationPayloadKind::object)]) / 1024.0,
                 static_cast<unsigned long long>(counters.allocations_by_kind[
                     static_cast<usize>(AllocationPayloadKind::array)]),
                 static_cast<double>(counters.allocated_bytes_by_kind[
                     static_cast<usize>(AllocationPayloadKind::array)]) / 1024.0,
                 static_cast<unsigned long long>(counters.allocations_by_kind[
                     static_cast<usize>(AllocationPayloadKind::clone)]),
                 static_cast<double>(counters.allocated_bytes_by_kind[
                     static_cast<usize>(AllocationPayloadKind::clone)]) / 1024.0,
                 static_cast<unsigned long long>(counters.allocations_by_kind[
                     static_cast<usize>(AllocationPayloadKind::string_payload)]),
                 static_cast<double>(counters.allocated_bytes_by_kind[
                     static_cast<usize>(AllocationPayloadKind::string_payload)]) /
                     1024.0);
    std::fprintf(stderr,
                 "[phoneME-perf] heap_lock_detail alloc=%llu field=%llu "
                 "arr_read=%llu arr_write=%llu arr_snapshot=%llu arr_checked=%llu "
                 "arr_meta=%llu arr_bulk=%llu class=%llu string=%llu ref=%llu gc=%llu\n",
                 static_cast<unsigned long long>(counters.locked_heap_operations_by_kind[
                     static_cast<usize>(LockedHeapOperationKind::allocation)]),
                 static_cast<unsigned long long>(counters.locked_heap_operations_by_kind[
                     static_cast<usize>(LockedHeapOperationKind::field)]),
                 static_cast<unsigned long long>(counters.locked_heap_operations_by_kind[
                     static_cast<usize>(LockedHeapOperationKind::array_element_read)]),
                 static_cast<unsigned long long>(counters.locked_heap_operations_by_kind[
                     static_cast<usize>(LockedHeapOperationKind::array_element_write)]),
                 static_cast<unsigned long long>(counters.locked_heap_operations_by_kind[
                     static_cast<usize>(LockedHeapOperationKind::array_element_snapshot)]),
                 static_cast<unsigned long long>(counters.locked_heap_operations_by_kind[
                     static_cast<usize>(LockedHeapOperationKind::array_element_checked)]),
                 static_cast<unsigned long long>(counters.locked_heap_operations_by_kind[
                     static_cast<usize>(LockedHeapOperationKind::array_metadata)]),
                 static_cast<unsigned long long>(counters.locked_heap_operations_by_kind[
                     static_cast<usize>(LockedHeapOperationKind::array_bulk)]),
                 static_cast<unsigned long long>(counters.locked_heap_operations_by_kind[
                     static_cast<usize>(LockedHeapOperationKind::class_name)]),
                 static_cast<unsigned long long>(counters.locked_heap_operations_by_kind[
                     static_cast<usize>(LockedHeapOperationKind::string)]),
                 static_cast<unsigned long long>(counters.locked_heap_operations_by_kind[
                     static_cast<usize>(LockedHeapOperationKind::reference)]),
                 static_cast<unsigned long long>(counters.locked_heap_operations_by_kind[
                     static_cast<usize>(LockedHeapOperationKind::garbage_collection)]));
    std::fprintf(stderr,
                 "[phoneME-perf] vm_slot_cache hit=%llu miss=%llu\n",
                 static_cast<unsigned long long>(counters.vm_slot_cache_hits),
                 static_cast<unsigned long long>(counters.vm_slot_cache_misses));
    std::fprintf(stderr,
                 "[phoneME-perf] inline_cache vhit=%llu vmiss=%llu "
                 "dhit=%llu dmiss=%llu field_hit=%llu field_miss=%llu\n",
                 static_cast<unsigned long long>(
                     counters.virtual_inline_cache_hits),
                 static_cast<unsigned long long>(
                     counters.virtual_inline_cache_misses),
                 static_cast<unsigned long long>(
                     counters.direct_call_cache_hits),
                 static_cast<unsigned long long>(
                     counters.direct_call_cache_misses),
                 static_cast<unsigned long long>(
                     counters.field_resolution_hits),
                 static_cast<unsigned long long>(
                     counters.field_resolution_misses));
    std::fprintf(stderr,
                 "[phoneME-perf] canvas frames=%llu unchanged=%llu "
                 "dirty_pixels=%llu avoided_pixels=%llu publish_ms=%.1f "
                 "backpressure_waits=%llu wait_ms=%.1f\n",
                 static_cast<unsigned long long>(counters.canvas_publications),
                 static_cast<unsigned long long>(
                     counters.canvas_unchanged_publications),
                 static_cast<unsigned long long>(counters.canvas_dirty_pixels),
                 static_cast<unsigned long long>(
                     counters.canvas_full_frame_pixels_avoided),
                 static_cast<double>(counters.canvas_publication_nanoseconds) /
                     1.0e6,
                 static_cast<unsigned long long>(
                     counters.frame_backpressure_waits),
                 static_cast<double>(counters.frame_backpressure_nanoseconds) /
                     1.0e6);
    std::fprintf(stderr,
                 "[phoneME-perf] maintenance checks=%llu background=%llu "
                 "resource_cache hit=%llu miss=%llu evict=%llu peak_kb=%.1f "
                 "text_cache hit=%llu miss=%llu evict=%llu peak_kb=%.1f\n",
                 static_cast<unsigned long long>(counters.maintenance_checks),
                 static_cast<unsigned long long>(
                     counters.maintenance_background_checks),
                 static_cast<unsigned long long>(
                     counters.resource_array_cache_hits),
                 static_cast<unsigned long long>(
                     counters.resource_array_cache_misses),
                 static_cast<unsigned long long>(
                     counters.resource_array_cache_evictions),
                 static_cast<double>(counters.resource_array_cache_peak_bytes) /
                     1024.0,
                 static_cast<unsigned long long>(counters.core_text_cache_hits),
                 static_cast<unsigned long long>(counters.core_text_cache_misses),
                 static_cast<unsigned long long>(
                     counters.core_text_cache_evictions),
                 static_cast<double>(counters.core_text_cache_peak_bytes) /
                     1024.0);
    const JitStatistics jit = jit_statistics();
    const u64 compiled_invocations = jit.executed_methods;
    const u64 interpreted_invocations =
        counters.method_invocations > compiled_invocations +
            counters.native_invocations
            ? counters.method_invocations - compiled_invocations -
                  counters.native_invocations
            : 0U;
    std::fprintf(stderr,
                 "[phoneME-perf] execution interpreted=%llu compiled=%llu "
                 "native=%llu osr=%llu deopt=%llu\n",
                 static_cast<unsigned long long>(interpreted_invocations),
                 static_cast<unsigned long long>(compiled_invocations),
                 static_cast<unsigned long long>(counters.native_invocations),
                 static_cast<unsigned long long>(jit.osr_executions),
                 static_cast<unsigned long long>(jit.deoptimized_executions));
    std::fprintf(stderr,
                 "[phoneME-perf] jit attempts=%llu compiled=%llu "
                 "executed_methods=%llu rejected=%llu deopt=%llu "
                 "compile_ms=%.1f exec_ms=%.1f osr=%llu bg_workers=%llu "
                 "bg_queue_peak=%llu "
                 "bg_compile_ms=%.1f render_cooldown_waits=%llu "
                 "render_cooldown_ms=%.1f code_cache_kb=%.1f/%0.1f\n",
                 static_cast<unsigned long long>(jit.compile_attempts),
                 static_cast<unsigned long long>(jit.compiled_methods),
                 static_cast<unsigned long long>(jit.executed_methods),
                 static_cast<unsigned long long>(jit.rejected_methods),
                 static_cast<unsigned long long>(jit.deoptimized_executions),
                 static_cast<double>(jit.compile_time_nanoseconds) / 1.0e6,
                 static_cast<double>(jit.execution_time_nanoseconds) / 1.0e6,
                 static_cast<unsigned long long>(jit.osr_executions),
                 static_cast<unsigned long long>(
                     jit.background_compile_worker_count),
                 static_cast<unsigned long long>(jit.background_compile_queue_peak),
                 static_cast<double>(jit.background_compile_time_nanoseconds) /
                     1.0e6,
                 static_cast<unsigned long long>(
                     jit.background_compile_render_cooldown_waits),
                 static_cast<double>(
                     jit.background_compile_render_cooldown_nanoseconds) / 1.0e6,
                 static_cast<double>(jit.code_cache_bytes) / 1024.0,
                 static_cast<double>(jit.code_cache_limit_bytes) / 1024.0);
    for (usize reason = 0U; reason < kJitRejectReasonCount; ++reason)
      if (jit.reject_reasons[reason] != 0U)
        std::fprintf(stderr, "[phoneME-perf] jit reject %s=%llu\n",
                     jit_reject_reason_name(
                         static_cast<JitRejectReason>(reason)).data(),
                     static_cast<unsigned long long>(
                         jit.reject_reasons[reason]));
    std::fprintf(stderr,
                 "[phoneME-perf] scheduler quanta=%llu transitions=%llu "
                 "erase_scans=%llu yields=%llu sleeps=%llu wakeups=%llu "
                 "spurious=%llu\n",
                 static_cast<unsigned long long>(counters.scheduler_quanta),
                 static_cast<unsigned long long>(
                     counters.scheduler_state_transitions),
                 static_cast<unsigned long long>(
                     counters.scheduler_queue_erase_scans),
                 static_cast<unsigned long long>(counters.scheduler_yields),
                 static_cast<unsigned long long>(counters.scheduler_sleeps),
                 static_cast<unsigned long long>(
                     counters.scheduler_event_wakeups),
                 static_cast<unsigned long long>(
                     counters.scheduler_spurious_wakeups));
#endif  // PHONEME_ENABLE_VM_PROFILING

    const auto scheduler = scheduler_.snapshot();
    const auto timers = timers_.diagnostics();
    const auto media_events = media_events_.diagnostics();
    const auto network = connections_.diagnostics();
    const auto graphics = graphics_.diagnostics();
    const auto metadata = classes_.metadata().diagnostics();
    std::fprintf(stderr,
                 "[phoneME-perf] workers java=%zu runnable=%zu blocked=%zu "
                 "sleeping=%zu timers=%zu timer_tasks=%zu media_worker=%d "
                 "media_players=%zu media_pending=%zu network_workers=%zu "
                 "network_active=%zu network_blocked=%zu network_queued=%zu "
                 "connections=%zu pending_io=%zu native_compute=%u\n",
                 scheduler.threads.size(), scheduler.runnable.size(),
                 scheduler.blocked.size(), scheduler.sleeping.size(),
                 timers.timers, timers.scheduled_tasks,
                 media_events.worker_started ? 1 : 0,
                 media_events.registered_players, media_events.pending_events,
                 network.adapter.workers, network.adapter.active_workers,
                 network.adapter.active_blocking_workers,
                 network.adapter.queued_tasks, network.connections,
                 network.pending_operations,
                 runtime::shared_compute_executor().active_worker_count());
    std::fprintf(stderr,
                 "[phoneME-perf] caches resource_current_kb=%.1f "
                 "graphics_images=%zu graphics_contexts=%zu graphics_kb=%.1f "
                 "metadata_classes=%zu metadata_methods=%zu descriptors=%zu "
                 "decoded_instructions=%zu decoded_operands=%zu\n",
                 static_cast<double>(resource_array_cache_payload_bytes_) / 1024.0,
                 graphics.images, graphics.contexts,
                 static_cast<double>(graphics.estimated_bytes) / 1024.0,
                 metadata.classes, metadata.methods, metadata.descriptors,
                 metadata.decoded_instructions, metadata.decoded_operands);
  }

  void Machine::begin_character_translation_frame() noexcept
  {
    character_translation_capture_.clear();
    character_translation_replay_index_ = 0U;
    character_translation_replay_valid_ = true;
    translated_text_capture_.clear();
    translated_text_replay_index_ = 0U;
    translated_text_replay_valid_ = true;
    character_translation_frame_active_ = true;
    ++character_translation_run_generation_;
  }

  void Machine::break_character_translation_run() noexcept
  {
    if (character_translation_frame_active_)
      ++character_translation_run_generation_;
  }

  Machine::CharacterTranslationDecision Machine::translate_draw_character(
      u64 graphics_id,
      char32_t character,
      i32 x,
      i32 y,
      i32 anchor,
      i32 font_face,
      i32 font_style,
      i32 font_size)
  {
    CharacterTranslationDecision decision {
        .action = CharacterTranslationDecision::Action::draw_original,
        .translated = nullptr,
        .x = x,
        .y = y,
        .anchor = anchor,
    };
    const auto service = translation_service();
    if (!character_translation_frame_active_ || !service ||
        !service->enabled())
      return decision;

    const CharacterDrawSample sample {
        .graphics_id = graphics_id,
        .character = character,
        .x = x,
        .y = y,
        .anchor = anchor,
        .font_face = font_face,
        .font_style = font_style,
        .font_size = font_size,
        .run_generation = character_translation_run_generation_,
    };
    character_translation_capture_.push_back(sample);

    if (!character_translation_replay_valid_ ||
        character_translation_replay_index_ >=
            character_translation_plan_samples_.size())
    {
      character_translation_replay_valid_ = false;
      return decision;
    }

    const PlannedCharacterSample &planned =
        character_translation_plan_samples_[character_translation_replay_index_];
    const CharacterDrawSample &expected = planned.sample;
    constexpr i32 kCoordinateTolerance = 4;
    const bool matches =
        expected.graphics_id == sample.graphics_id &&
        expected.character == sample.character &&
        std::abs(expected.x - sample.x) <= kCoordinateTolerance &&
        std::abs(expected.y - sample.y) <= kCoordinateTolerance &&
        expected.anchor == sample.anchor &&
        expected.font_face == sample.font_face &&
        expected.font_style == sample.font_style &&
        expected.font_size == sample.font_size;
    ++character_translation_replay_index_;
    if (!matches)
    {
      character_translation_replay_valid_ = false;
      return decision;
    }
    if (planned.run_index == std::numeric_limits<usize>::max() ||
        planned.run_index >= character_translation_runs_.size())
      return decision;

    const CharacterRunPlan &run =
        character_translation_runs_[planned.run_index];
    auto translated = service->lookup_or_request(run.source);
    if (!translated)
      return decision;

    if (!planned.first_in_run)
    {
      decision.action = CharacterTranslationDecision::Action::suppress;
      return decision;
    }
    decision.action = CharacterTranslationDecision::Action::draw_translation;
    decision.translated = std::move(translated);
    decision.x = sample.x + (run.x - expected.x);
    decision.y = sample.y + (run.y - expected.y);
    decision.anchor = run.anchor;
    return decision;
  }

  void Machine::configure_image_draw_interceptor(ImageDrawSink sink)
  {
    const bool enabled = static_cast<bool>(sink);
    {
      std::scoped_lock lock(image_draw_sink_mutex_);
      image_draw_sink_ = std::move(sink);
    }
    image_draw_interception_enabled_.store(enabled,
                                           std::memory_order_release);
  }

  void Machine::intercept_image_draw(const ImageDrawEvent& event) const
  {
    if (!image_draw_interception_enabled_.load(std::memory_order_acquire))
      return;

    ImageDrawSink sink;
    {
      std::scoped_lock lock(image_draw_sink_mutex_);
      sink = image_draw_sink_;
    }
    if (sink) sink(event);
  }

  Machine::TextTranslationLayoutDecision Machine::plan_translated_text(
      u64 graphics_id,
      std::span<const char32_t> text,
      i32 x,
      i32 y,
      i32 anchor,
      i32 font_face,
      i32 font_style,
      i32 font_size,
      i32 clip_x,
      i32 clip_y,
      i32 clip_width,
      i32 clip_height,
      i32 translate_x,
      i32 translate_y)
  {
    auto created_font = graphics::Font::create(
        font_face, font_style, font_size);
    const graphics::Font font = created_font
        ? *created_font
        : graphics::Font::default_font();
    const i32 maximum_width = translated_available_width(
        x, anchor, clip_x, clip_width, translate_x);
    const i32 original_top = translated_text_top(
        y, anchor, font, translate_y);
    const i32 block_height = translated_text_height(
        font, text, maximum_width);
    const i32 safe_clip_height = std::max(clip_height, 0);
    const i32 clip_bottom = static_cast<i32>(std::clamp<i64>(
        static_cast<i64>(clip_y) + safe_clip_height,
        std::numeric_limits<i32>::min(),
        std::numeric_limits<i32>::max()));

    i32 planned_top = original_top;
    if (safe_clip_height <= 0 || block_height >= safe_clip_height)
      planned_top = clip_y;
    else
      planned_top = std::clamp(
          original_top, clip_y, clip_bottom - block_height);

    TextTranslationLayoutDecision decision {
        .planned = false,
        .y = translated_y_for_top(
            planned_top, block_height, anchor, font, translate_y),
        .height = block_height,
    };
    if (!character_translation_frame_active_ || text.empty())
      return decision;

    TranslatedTextDrawSample sample {
        .graphics_id = graphics_id,
        .text = std::vector<char32_t>(text.begin(), text.end()),
        .x = x,
        .y = y,
        .anchor = anchor,
        .font_face = font_face,
        .font_style = font_style,
        .font_size = font_size,
        .clip_x = clip_x,
        .clip_y = clip_y,
        .clip_width = clip_width,
        .clip_height = clip_height,
        .translate_x = translate_x,
        .translate_y = translate_y,
    };
    translated_text_capture_.push_back(sample);

    if (!translated_text_replay_valid_ ||
        translated_text_replay_index_ >= translated_text_plan_.size())
    {
      translated_text_replay_valid_ = false;
      return decision;
    }

    const PlannedTranslatedTextSample &planned =
        translated_text_plan_[translated_text_replay_index_];
    const TranslatedTextDrawSample &expected = planned.sample;
    constexpr i32 kCoordinateTolerance = 4;
    const bool matches =
        expected.graphics_id == sample.graphics_id &&
        expected.text == sample.text &&
        std::abs(expected.x - sample.x) <= kCoordinateTolerance &&
        std::abs(expected.y - sample.y) <= kCoordinateTolerance &&
        expected.anchor == sample.anchor &&
        expected.font_face == sample.font_face &&
        expected.font_style == sample.font_style &&
        expected.font_size == sample.font_size &&
        expected.clip_x == sample.clip_x &&
        expected.clip_y == sample.clip_y &&
        expected.clip_width == sample.clip_width &&
        expected.clip_height == sample.clip_height &&
        expected.translate_x == sample.translate_x &&
        expected.translate_y == sample.translate_y;
    ++translated_text_replay_index_;
    if (!matches)
    {
      translated_text_replay_valid_ = false;
      return decision;
    }

    decision.planned = true;
    decision.suppress = planned.suppress;
    decision.y = planned.y + (sample.y - expected.y);
    decision.height = planned.height;
    return decision;
  }

  void Machine::end_character_translation_frame()
  {
    if (!character_translation_frame_active_)
      return;
    character_translation_frame_active_ = false;

    const auto service = translation_service();
    if (!service || !service->enabled())
    {
      character_translation_capture_.clear();
      character_translation_plan_samples_.clear();
      character_translation_runs_.clear();
      translated_text_capture_.clear();
      translated_text_plan_.clear();
      return;
    }

    std::vector<PlannedCharacterSample> planned;
    planned.reserve(character_translation_capture_.size());
    for (const auto &sample : character_translation_capture_)
    {
      planned.push_back(PlannedCharacterSample {
          .sample = sample,
      });
    }

    std::vector<CharacterRunPlan> runs;
    usize begin = 0U;
    while (begin < character_translation_capture_.size())
    {
      usize end = begin + 1U;
      const CharacterDrawSample &first =
          character_translation_capture_[begin];
      i32 previous_x = first.x;
      i32 previous_y = first.y;
      const i32 maximum_horizontal_step =
          std::max<i32>(24, first.font_size * 4);
      const i32 maximum_vertical_step =
          std::max<i32>(20, maximum_horizontal_step);
      const i32 maximum_line_indent =
          std::max<i32>(48, maximum_horizontal_step * 2);
      while (end < character_translation_capture_.size())
      {
        const CharacterDrawSample &candidate =
            character_translation_capture_[end];
        const bool same_style =
            candidate.run_generation == first.run_generation &&
            candidate.graphics_id == first.graphics_id &&
            candidate.anchor == first.anchor &&
            candidate.font_face == first.font_face &&
            candidate.font_style == first.font_style &&
            candidate.font_size == first.font_size;
        const bool continues_line =
            candidate.y == previous_y &&
            std::abs(candidate.x - previous_x) <=
                maximum_horizontal_step;
        const bool starts_adjacent_line =
            candidate.y > previous_y &&
            candidate.y - previous_y <= maximum_vertical_step &&
            std::abs(candidate.x - first.x) <= maximum_line_indent;
        if (!same_style || (!continues_line && !starts_adjacent_line))
          break;
        previous_x = candidate.x;
        previous_y = candidate.y;
        ++end;
      }

      std::vector<char32_t> source;
      source.reserve((end - begin) + 2U);
      i32 source_line_y = first.y;
      for (usize index = begin; index < end; ++index)
      {
        const CharacterDrawSample &sample =
            character_translation_capture_[index];
        if (sample.y != source_line_y)
        {
          if (!source.empty() && source.back() != U'\n')
            source.push_back(U'\n');
          source_line_y = sample.y;
        }
        source.push_back(sample.character);
      }

      if (translation::TranslationService::contains_translatable_text(source))
      {
        const usize run_index = runs.size();
        runs.push_back(CharacterRunPlan {
            .first_sample = begin,
            .sample_count = end - begin,
            .x = first.x,
            .y = first.y,
            .anchor = first.anchor,
            .source = std::move(source),
        });
        for (usize index = begin; index < end; ++index)
        {
          planned[index].run_index = run_index;
          planned[index].first_in_run = index == begin;
        }
        (void)service->lookup_or_request(runs.back().source);
      }
      begin = end;
    }

    std::vector<PlannedTranslatedTextSample> text_plan;
    text_plan.reserve(translated_text_capture_.size());
    for (const auto &sample : translated_text_capture_)
    {
      auto created_font = graphics::Font::create(
          sample.font_face, sample.font_style, sample.font_size);
      const graphics::Font font = created_font
          ? *created_font
          : graphics::Font::default_font();
      text_plan.push_back(PlannedTranslatedTextSample {
          .sample = sample,
          .y = sample.y,
          .height = font.height(),
      });
    }

    // Many J2ME games build a text outline by drawing the exact same string
    // several times at neighbouring coordinates with different colours. Once
    // translated, rendering every pass creates several complete Vietnamese
    // strings on top of each other. Collapse each small outline/shadow cluster
    // to one central draw; draw_translated_text() supplies a readable outline.
    constexpr i32 kDuplicateTextRadius = 4;
    const auto resolved_anchor = [](i32 anchor) noexcept
    {
      return anchor == 0
          ? graphics::anchor_left | graphics::anchor_top
          : anchor;
    };
    const auto same_text_layer = [&](const TranslatedTextDrawSample &left,
                                     const TranslatedTextDrawSample &right)
    {
      const i32 left_anchor = resolved_anchor(left.anchor);
      const i32 right_anchor = resolved_anchor(right.anchor);
      return left.graphics_id == right.graphics_id &&
             left.text == right.text &&
             left.font_face == right.font_face &&
             left.font_style == right.font_style &&
             left.font_size == right.font_size &&
             left.clip_x == right.clip_x &&
             left.clip_y == right.clip_y &&
             left.clip_width == right.clip_width &&
             left.clip_height == right.clip_height &&
             left.translate_x == right.translate_x &&
             left.translate_y == right.translate_y &&
             left_anchor == right_anchor &&
             std::abs(left.x - right.x) <= kDuplicateTextRadius &&
             std::abs(left.y - right.y) <= kDuplicateTextRadius;
    };

    std::vector<bool> duplicate_grouped(
        translated_text_capture_.size(), false);
    for (usize root = 0U; root < translated_text_capture_.size(); ++root)
    {
      if (duplicate_grouped[root])
        continue;
      std::vector<usize> group {root};
      duplicate_grouped[root] = true;
      for (usize candidate = root + 1U;
           candidate < translated_text_capture_.size();
           ++candidate)
      {
        if (!duplicate_grouped[candidate] &&
            same_text_layer(translated_text_capture_[root],
                            translated_text_capture_[candidate]))
        {
          duplicate_grouped[candidate] = true;
          group.push_back(candidate);
        }
      }
      if (group.size() <= 1U)
        continue;

      usize preferred = group.front();
      i64 best_distance = std::numeric_limits<i64>::max();
      for (const usize candidate : group)
      {
        i64 distance = 0;
        for (const usize other : group)
        {
          distance += std::abs(
              static_cast<i64>(translated_text_capture_[candidate].x) -
              translated_text_capture_[other].x);
          distance += std::abs(
              static_cast<i64>(translated_text_capture_[candidate].y) -
              translated_text_capture_[other].y);
        }
        // Prefer the last central pass on a tie. Games conventionally render
        // shadows first and the foreground colour last.
        if (distance < best_distance ||
            (distance == best_distance && candidate > preferred))
        {
          best_distance = distance;
          preferred = candidate;
        }
      }
      for (const usize index : group)
      {
        if (index != preferred)
          text_plan[index].suppress = true;
      }
    }

    std::vector<usize> visible_text_indices;
    visible_text_indices.reserve(translated_text_capture_.size());
    for (usize index = 0U; index < translated_text_capture_.size(); ++index)
    {
      if (!text_plan[index].suppress)
        visible_text_indices.push_back(index);
    }

    usize visible_begin = 0U;
    while (visible_begin < visible_text_indices.size())
    {
      const usize first_index = visible_text_indices[visible_begin];
      const TranslatedTextDrawSample &first =
          translated_text_capture_[first_index];
      const i32 first_anchor = resolved_anchor(first.anchor);
      const i32 first_horizontal = first_anchor &
          (graphics::anchor_left | graphics::anchor_right |
           graphics::anchor_hcenter);
      const i32 first_vertical = first_anchor &
          (graphics::anchor_top | graphics::anchor_bottom |
           graphics::anchor_baseline);
      usize visible_end = visible_begin + 1U;
      i64 previous_y = static_cast<i64>(first.y) + first.translate_y;
      const i32 maximum_gap = 48;
      const i32 maximum_indent = std::max<i32>(
          64, std::max(first.clip_width, 1) / 2);
      while (visible_end < visible_text_indices.size())
      {
        const usize candidate_index = visible_text_indices[visible_end];
        const TranslatedTextDrawSample &candidate =
            translated_text_capture_[candidate_index];
        const i32 candidate_anchor = resolved_anchor(candidate.anchor);
        const i32 candidate_horizontal = candidate_anchor &
            (graphics::anchor_left | graphics::anchor_right |
             graphics::anchor_hcenter);
        const i32 candidate_vertical = candidate_anchor &
            (graphics::anchor_top | graphics::anchor_bottom |
             graphics::anchor_baseline);
        const i64 candidate_y =
            static_cast<i64>(candidate.y) + candidate.translate_y;
        const bool same_clip =
            candidate.graphics_id == first.graphics_id &&
            candidate.clip_x == first.clip_x &&
            candidate.clip_y == first.clip_y &&
            candidate.clip_width == first.clip_width &&
            candidate.clip_height == first.clip_height &&
            candidate.translate_x == first.translate_x &&
            candidate.translate_y == first.translate_y &&
            candidate_horizontal == first_horizontal &&
            candidate_vertical == first_vertical;
        const bool adjacent =
            candidate_y > previous_y &&
            candidate_y - previous_y <= maximum_gap &&
            std::abs(candidate.x - first.x) <= maximum_indent;
        if (!same_clip || !adjacent)
          break;
        previous_y = candidate_y;
        ++visible_end;
      }

      const usize block_count = visible_end - visible_begin;
      std::vector<graphics::Font> fonts;
      std::vector<i32> heights(block_count, 1);
      std::vector<i32> original_tops(block_count, 0);
      std::vector<i32> planned_tops(block_count, 0);
      std::vector<i32> gaps(block_count, 1);
      fonts.reserve(block_count);

      for (usize visible_index = visible_begin;
           visible_index < visible_end;
           ++visible_index)
      {
        const usize index = visible_text_indices[visible_index];
        const TranslatedTextDrawSample &sample =
            translated_text_capture_[index];
        const usize local_index = visible_index - visible_begin;
        auto created_font = graphics::Font::create(
            sample.font_face, sample.font_style, sample.font_size);
        const graphics::Font font = created_font
            ? *created_font
            : graphics::Font::default_font();
        fonts.push_back(font);
        const i32 maximum_width = translated_available_width(
            sample.x,
            sample.anchor,
            sample.clip_x,
            sample.clip_width,
            sample.translate_x);
        heights[local_index] = translated_text_height(
            font, sample.text, maximum_width);
        original_tops[local_index] = translated_text_top(
            sample.y, sample.anchor, font, sample.translate_y);
        if (local_index > 0U)
        {
          const i32 natural_gap =
              original_tops[local_index] -
              (original_tops[local_index - 1U] +
               fonts[local_index - 1U].height());
          const i32 maximum_natural_gap = std::max<i32>(
              1,
              std::min(font.height(),
                       fonts[local_index - 1U].height()) / 2);
          gaps[local_index] = std::clamp(
              natural_gap, 1, maximum_natural_gap);
        }
      }

      const i32 clip_top = first.clip_y;
      const i32 safe_clip_height = std::max(first.clip_height, 0);
      const i32 clip_bottom = static_cast<i32>(std::clamp<i64>(
          static_cast<i64>(clip_top) + safe_clip_height,
          std::numeric_limits<i32>::min(),
          std::numeric_limits<i32>::max()));

      planned_tops.front() = original_tops.front();
      for (usize local_index = 1U; local_index < block_count; ++local_index)
      {
        const i64 minimum_top =
            static_cast<i64>(planned_tops[local_index - 1U]) +
            heights[local_index - 1U] + gaps[local_index];
        const i32 pushed_top = static_cast<i32>(std::clamp<i64>(
            minimum_top,
            std::numeric_limits<i32>::min(),
            std::numeric_limits<i32>::max()));
        planned_tops[local_index] =
            std::max(original_tops[local_index], pushed_top);
      }

      auto shift_all = [&](i64 delta)
      {
        for (i32 &top : planned_tops)
        {
          top = static_cast<i32>(std::clamp<i64>(
              static_cast<i64>(top) + delta,
              std::numeric_limits<i32>::min(),
              std::numeric_limits<i32>::max()));
        }
      };
      auto planned_bottom = [&]() -> i64
      {
        return static_cast<i64>(planned_tops.back()) + heights.back();
      };

      if (planned_bottom() > clip_bottom)
        shift_all(static_cast<i64>(clip_bottom) - planned_bottom());

      if (planned_tops.front() < clip_top && planned_bottom() < clip_bottom)
      {
        const i64 shift_down = std::min<i64>(
            static_cast<i64>(clip_top) - planned_tops.front(),
            static_cast<i64>(clip_bottom) - planned_bottom());
        shift_all(shift_down);
      }

      for (usize visible_index = visible_begin;
           visible_index < visible_end;
           ++visible_index)
      {
        const usize index = visible_text_indices[visible_index];
        const usize local_index = visible_index - visible_begin;
        const TranslatedTextDrawSample &sample =
            translated_text_capture_[index];
        text_plan[index].y = translated_y_for_top(
            planned_tops[local_index],
            heights[local_index],
            sample.anchor,
            fonts[local_index],
            sample.translate_y);
        text_plan[index].height = heights[local_index];
      }
      visible_begin = visible_end;
    }

    character_translation_plan_samples_ = std::move(planned);
    character_translation_runs_ = std::move(runs);
    character_translation_capture_.clear();
    translated_text_plan_ = std::move(text_plan);
    translated_text_capture_.clear();
  }

  Result<ObjectRef> Machine::current_java_thread()
  {
    auto current = scheduler_.current_thread_object();
    if (current)
      return *current;
    auto allocated = states_.allocate_instance(heap_, "java/lang/Thread");
    if (!allocated)
      return std::unexpected(allocated.error());
    auto bound = scheduler_.bind_current_thread_object(*allocated);
    if (!bound)
      return std::unexpected(bound.error());
    return *allocated;
  }

  Status Machine::initialize_java_thread(ObjectRef thread, ObjectRef target)
  {
    return scheduler_.register_thread(thread, target);
  }

  Status Machine::start_java_thread(ObjectRef thread)
  {
    return scheduler_.start_thread(*this, thread);
  }

  Result<ExecutionResult> Machine::run_java_thread_entry(ObjectRef thread)
  {
    auto runtime_class = heap_.class_name(thread);
    if (!runtime_class)
      return std::unexpected(runtime_class.error());
    auto run_method = classes_.resolve_method(*runtime_class, "run", "()V");
    if (!run_method)
      return std::unexpected(run_method.error());

    // The built-in java.lang.Thread.run native only forwards to the stored
    // Runnable target. Enter that target directly from the scheduler so a
    // normal Thread(Runnable) does not consume an extra host C++ execute frame.
    // A subclass override must still run virtually and retains Java semantics.
    if (run_method->owner != nullptr &&
        run_method->owner->name() == "java/lang/Thread")
    {
      auto target = scheduler_.runnable_target(thread);
      if (!target)
        return std::unexpected(target.error());
      if (target->is_null())
      {
        return ExecutionResult {
            .return_value = std::nullopt,
            .throwable = std::nullopt,
            .executed_instructions = 0U,
        };
      }
      return invoke_instance(
          *target,
          "java/lang/Runnable",
          "run",
          "()V",
          {},
          kLongLivedThreadInstructionBudget);
    }

    return invoke_instance(
        thread,
        "java/lang/Thread",
        "run",
        "()V",
        {},
        kLongLivedThreadInstructionBudget);
  }

  Result<std::optional<Value>> Machine::run_java_thread_target(ObjectRef thread)
  {
    auto target = scheduler_.runnable_target(thread);
    if (!target)
      return std::unexpected(target.error());
    if (target->is_null())
      return std::optional<Value>{};
    auto result = invoke_instance(
        *target,
        "java/lang/Runnable",
        "run",
        "()V",
        {},
        kLongLivedThreadInstructionBudget);
    if (!result)
      return std::unexpected(result.error());
    if (result->throwable.has_value())
    {
      auto class_name = heap_.class_name(*result->throwable);
      if (!class_name)
        return std::unexpected(class_name.error());

      std::string message;
      auto detail_value = heap_.field(*result->throwable, 0U);
      if (detail_value)
      {
        auto detail_reference = detail_value->as_reference();
        if (detail_reference && !detail_reference->is_null())
        {
          auto detail = heap_.string_value(*detail_reference);
          if (detail)
          {
            message.reserve(detail->size());
            for (char16_t character : *detail)
            {
              message.push_back(character <= 0x7fU
                  ? static_cast<char>(character)
                  : '?');
            }
          }
        }
      }
      if (message.empty())
        message = "uncaught throwable from Runnable.run";
      if (!result->exception_context.empty())
      {
        message += " at ";
        message += result->exception_context;
      }
      return fail_java(*class_name, message);
    }
    return std::optional<Value>{};
  }

  void Machine::configure_frame_pacing(
      i32 frames_per_second,
      FramePacingMode mode) noexcept
  {
    scheduler_.configure_frame_pacing(frames_per_second, mode);
  }

  void Machine::configure_jit(bool enabled) noexcept
  {
    // JIT cache mutation must never race a Java thread entering compiled code.
    // Runtime host calls can arrive while lifecycle/game threads are active;
    // serialize configuration with the same recursive execution gate used by
    // interpreter/JIT execution rather than relying on Runtime's host-only
    // operation mutex.
    std::scoped_lock execution_lock(execution_mutex_);
    jit_.set_enabled(enabled);
  }

  void Machine::configure_jit_startup(bool enabled) noexcept
  {
    std::scoped_lock execution_lock(execution_mutex_);
    jit_.set_startup_mode(enabled);
  }

  Status Machine::configure_jit_profile(std::string path)
  {
    std::scoped_lock execution_lock(execution_mutex_);
    return jit_.configure_profile(std::move(path));
  }

  Status Machine::flush_jit_profile()
  {
    std::scoped_lock execution_lock(execution_mutex_);
    return jit_.flush_profile();
  }

  bool Machine::jit_enabled() const noexcept
  {
    std::scoped_lock execution_lock(execution_mutex_);
    return jit_.enabled();
  }

  bool Machine::jit_startup_mode() const noexcept
  {
    std::scoped_lock execution_lock(execution_mutex_);
    return jit_.startup_mode();
  }

  JitAvailability Machine::jit_availability() const noexcept
  {
    std::scoped_lock execution_lock(execution_mutex_);
    return jit_.availability();
  }

  JitStatistics Machine::jit_statistics() const noexcept
  {
    std::scoped_lock execution_lock(execution_mutex_);
    return jit_.statistics();
  }

  void Machine::pace_frame_publication()
  {
    scheduler_.pace_current_frame_publication(*this);
  }

  void Machine::note_frame_pacing_boundary() noexcept
  {
    scheduler_.note_current_frame_boundary();
    jit_.note_frame_published();
  }

  void Machine::note_frame_pacing_request() noexcept
  {
    scheduler_.note_current_frame_request();
  }

  void Machine::break_frame_pacing_sequence() noexcept
  {
    scheduler_.break_current_frame_pacing_sequence();
  }

  Result<SchedulerWaitResult> Machine::sleep_current_thread(i64 millis)
  {
    if (millis < 0)
      return fail_java("java/lang/IllegalArgumentException",
                       "Thread.sleep timeout must not be negative");
    return scheduler_.sleep_current(
        *this,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::milliseconds(millis)));
  }

  Result<SchedulerWaitResult> Machine::join_java_thread(
      ObjectRef thread,
      std::optional<i64> millis)
  {
    scheduler_.break_current_frame_pacing_sequence();
    if (millis.has_value() && *millis < 0)
      return fail_java("java/lang/IllegalArgumentException",
                       "Thread.join timeout must not be negative");
    std::optional<std::chrono::milliseconds> timeout;
    if (millis.has_value() && *millis > 0)
      timeout = std::chrono::milliseconds(*millis);
    return scheduler_.join_current(*this, thread, timeout);
  }

  Status Machine::interrupt_java_thread(ObjectRef thread)
  {
    auto interrupted = scheduler_.interrupt(thread);
    if (interrupted)
      monitors_.wake_all();
    return interrupted;
  }

  Result<MonitorWaitResult> Machine::wait_on_object(
      ObjectRef object,
      std::optional<i64> millis)
  {
    scheduler_.break_current_frame_pacing_sequence();
    if (millis.has_value() && *millis < 0)
      return fail_java("java/lang/IllegalArgumentException",
                       "Object.wait timeout must not be negative");
    std::optional<std::chrono::milliseconds> timeout;
    if (millis.has_value() && *millis > 0)
      timeout = std::chrono::milliseconds(*millis);

    u32 released_depth = 0U;
    std::optional<Error> blocking_pump_error;
    const bool pump_timed_wait = timeout.has_value() && canvas_bridge_ != nullptr;
    auto result = monitors_.wait(
        object,
        scheduler_.current_thread_id(),
        timeout,
        [this, &released_depth, &blocking_pump_error, pump_timed_wait] {
          scheduler_.set_current_state(JavaThreadState::waiting);
          released_depth = suspend_execution_for_blocking();
          if (pump_timed_wait)
          {
            auto pumped = canvas_bridge_->pump_blocking_wait_work();
            if (!pumped)
              blocking_pump_error = pumped.error();
          }
        },
        [this, &released_depth] {
          resume_execution_after_blocking(released_depth);
          scheduler_.set_current_state(JavaThreadState::running);
        },
        [this] {
          // Shutdown may race with a Java thread that consumes its interrupt
          // and then enters Object.wait after MonitorTable::clear() has already
          // notified the old monitors. The scheduler stop flag is persistent;
          // include it in the wait predicate so no new indefinite wait can be
          // created while workers are being joined.
          return scheduler_.current_is_interrupted() ||
                 scheduler_.current_stop_requested();
        });
    if (blocking_pump_error.has_value())
      return std::unexpected(std::move(*blocking_pump_error));
    if (result && *result == MonitorWaitResult::interrupted)
      (void)scheduler_.consume_current_interrupt();
    return result;
  }

  Status Machine::notify_object(ObjectRef object, bool all)
  {
    return all ? monitors_.notify_all(object,
                                      scheduler_.current_thread_id())
               : monitors_.notify_one(object,
                                      scheduler_.current_thread_id());
  }

  void Machine::cooperative_yield()
  {
    scheduler_.cooperative_yield(*this);
  }

  bool Machine::executing_on_current_thread() const noexcept
  {
    return g_execution_machine == this && g_execution_lock_depth != 0U;
  }

  Status Machine::enter_external_execution()
  {
    if (g_execution_machine != nullptr && g_execution_machine != this)
    {
      return fail(ErrorCode::invalid_state,
                  "a host thread cannot execute two Machine instances recursively");
    }
    execution_mutex_.lock();
    g_execution_machine = this;
    ++g_execution_lock_depth;
    return {};
  }

  bool Machine::try_enter_external_execution() noexcept
  {
    if (g_execution_machine != nullptr && g_execution_machine != this)
      return false;
    if (!execution_mutex_.try_lock())
      return false;
    g_execution_machine = this;
    ++g_execution_lock_depth;
    return true;
  }

  void Machine::leave_external_execution() noexcept
  {
    if (g_execution_machine != this || g_execution_lock_depth == 0U)
      std::abort();
    --g_execution_lock_depth;
    execution_mutex_.unlock();
    if (g_execution_lock_depth == 0U)
      g_execution_machine = nullptr;
  }

  void Machine::request_garbage_collection() noexcept
  {
    gc_requested_.store(true, std::memory_order_release);
  }

  u32 Machine::suspend_execution_for_blocking() noexcept
  {
    if (g_execution_machine != this || g_execution_lock_depth == 0U)
      return 0U;
    const u32 depth = g_execution_lock_depth;
    for (u32 index = 0U; index < depth; ++index)
      execution_mutex_.unlock();

    // The recursive mutex is now physically unowned by this host thread, so
    // its TLS execution state must say the same thing. Keeping the old logical
    // depth here is unsafe when a blocking hook pumps Canvas work: that nested
    // execution acquires the mutex from depth zero, but a nested sleep/wait
    // would try to unlock the stale outer depth as well and corrupt the gate.
    g_execution_lock_depth = 0U;
    g_execution_machine = nullptr;
    return depth;
  }

  void Machine::resume_execution_after_blocking(u32 depth) noexcept
  {
    if (depth == 0U)
      return;
    if (g_execution_machine != nullptr || g_execution_lock_depth != 0U)
      std::abort();
    for (u32 index = 0U; index < depth; ++index)
      execution_mutex_.lock();
    g_execution_machine = this;
    g_execution_lock_depth = depth;
  }

  void Machine::publish_execution_roots(
      u32 invocation_depth,
      const std::vector<ObjectRef>& roots)
  {
    PerformanceCounters::observe_execution_root_publication(roots.size(), false);
    scheduler_.publish_current_roots(invocation_depth, roots);
  }

  std::span<const ObjectRef> Machine::exchange_execution_roots(
      u32 invocation_depth,
      std::vector<ObjectRef>& roots)
  {
    PerformanceCounters::observe_execution_root_publication(roots.size(), true);
    return scheduler_.exchange_current_roots(invocation_depth, roots);
  }

  void Machine::clear_execution_roots(u32 invocation_depth) noexcept
  {
    scheduler_.clear_current_roots(invocation_depth);
  }

  Status Machine::enter_monitor(ObjectRef monitor)
  {
    auto entered = monitors_.enter(monitor,
                                   scheduler_.current_thread_id());
    if (!entered)
      return std::unexpected(entered.error());
    if (*entered == MonitorEnterResult::acquired)
      return {};

    u32 released_depth = 0U;
    return monitors_.enter_blocking(
        monitor,
        scheduler_.current_thread_id(),
        [this, &released_depth] {
          scheduler_.set_current_state(JavaThreadState::blocked_monitor);
          released_depth = suspend_execution_for_blocking();
        },
        [this, &released_depth] {
          resume_execution_after_blocking(released_depth);
          scheduler_.set_current_state(JavaThreadState::running);
        });
  }

  Result<NativeRootScope> Machine::allocate_pinned_instance(
      std::string_view class_name)
  {
    std::scoped_lock execution_lock(execution_mutex_);
    auto object = states_.allocate_instance(heap_, class_name);
    if (!object && object.error().code == ErrorCode::overflow)
    {
      auto collected = collect_garbage();
      if (!collected)
        return std::unexpected(collected.error());
      object = states_.allocate_instance(heap_, class_name);
    }
    if (!object)
      return std::unexpected(object.error());
    return pin_native_root(*object);
  }

  Result<ObjectRef> Machine::cached_resource_byte_array(
      std::string_view resource_name)
  {
    if (const auto iterator = resource_array_cache_.find(
            std::string(resource_name));
        iterator != resource_array_cache_.end() &&
        !iterator->second.is_null())
    {
      PerformanceCounters::record_resource_array_cache(true);
      return iterator->second;
    }
    PerformanceCounters::record_resource_array_cache(false);
    auto bytes = classes_.read_resource(resource_name);
    if (!bytes) return std::unexpected(bytes.error());
    auto array = heap_.allocate_array(
        "[B", bytes->size(), Value::from_int(0));
    if (!array && array.error().code == ErrorCode::overflow) {
      auto collected = collect_garbage();
      if (!collected) return std::unexpected(collected.error());
      array = heap_.allocate_array("[B", bytes->size(), Value::from_int(0));
    }
    if (!array) return std::unexpected(array.error());
    auto bytes_stored = heap_.write_byte_array(*array, 0U, *bytes);
    if (!bytes_stored) return std::unexpected(bytes_stored.error());

    while (resource_array_cache_payload_bytes_ + bytes->size() >
               kResourceArrayCachePayloadLimit &&
           !resource_array_cache_order_.empty())
    {
      const std::string& oldest = resource_array_cache_order_.front();
      const auto evicted = resource_array_cache_.find(oldest);
      if (evicted != resource_array_cache_.end())
      {
        if (const auto size = heap_.array_length(evicted->second); size)
          resource_array_cache_payload_bytes_ -=
              static_cast<u64>(*size);
        resource_array_cache_.erase(evicted);
        PerformanceCounters::record_resource_array_cache_eviction();
      }
      resource_array_cache_order_.pop_front();
    }
    resource_array_cache_payload_bytes_ += bytes->size();
    PerformanceCounters::observe_resource_array_cache_bytes(
        static_cast<usize>(resource_array_cache_payload_bytes_));
    resource_array_cache_order_.emplace_back(resource_name);
    resource_array_cache_[std::string(resource_name)] = *array;
    return *array;
  }

  Status Machine::collect_garbage()
  {
    std::scoped_lock execution_lock(execution_mutex_);
    std::vector<ObjectRef> roots;
    roots.reserve(interned_strings_.size() + class_mirrors_.size() +
                  ui_components_.size() + 16U);
    states_.append_reference_roots(roots);
    if (!emergency_out_of_memory_error_.is_null())
      roots.push_back(emergency_out_of_memory_error_);
    for (const auto &[value, reference] : interned_strings_)
    {
      (void)value;
      if (!reference.is_null())
        roots.push_back(reference);
    }
    for (const auto &[path, reference] : resource_array_cache_)
    {
      (void)path;
      if (!reference.is_null())
        roots.push_back(reference);
    }
    for (const auto &[class_name, reference] : class_mirrors_)
    {
      (void)class_name;
      if (!reference.is_null())
        roots.push_back(reference);
    }
    for (const auto &[component_id, reference] : ui_components_)
    {
      (void)component_id;
      if (!reference.is_null())
        roots.push_back(reference);
    }
    if (canvas_bridge_ != nullptr)
      canvas_bridge_->append_reference_roots(roots);
    monitors_.append_reference_roots(roots);
    timers_.append_reference_roots(roots);
    scheduler_.append_reference_roots(roots);
    native_roots_.append_reference_roots(roots);
    auto collected = heap_.collect(roots);
    if (collected)
    {
      prune_lambda_bindings();
      graphics_.prune([this](u64 object_key) {
        return heap_.class_name(ObjectRef{object_key}).has_value();
      });
    }
    return collected;
  }

  Status Machine::initialize_system_streams()
  {
    if (system_streams_initialized_)
      return {};
    auto empty_buffer = heap_.allocate_array("[B", 0, Value::from_int(0));
    if (!empty_buffer)
      return std::unexpected(empty_buffer.error());
    auto input = states_.allocate_instance(heap_,
                                           "java/io/ByteArrayInputStream");
    if (!input)
      return std::unexpected(input.error());
    auto input_buffer = heap_.set_field(*input, 0,
                                        Value::from_reference(*empty_buffer));
    auto input_position = heap_.set_field(*input, 1, Value::from_int(0));
    auto input_mark = heap_.set_field(*input, 2, Value::from_int(0));
    auto input_count = heap_.set_field(*input, 3, Value::from_int(0));
    if (!input_buffer)
      return input_buffer;
    if (!input_position)
      return input_position;
    if (!input_mark)
      return input_mark;
    if (!input_count)
      return input_count;

    const auto make_console_stream = [this]() -> Result<ObjectRef> {
      auto stream = states_.allocate_instance(heap_, "java/io/PrintStream");
      if (!stream)
        return std::unexpected(stream.error());
      auto output = heap_.set_field(*stream, 0, Value::from_reference({}));
      auto error = heap_.set_field(*stream, 1, Value::from_int(0));
      auto auto_flush = heap_.set_field(*stream, 2, Value::from_int(1));
      auto console = heap_.set_field(*stream, 3, Value::from_int(1));
      if (!output)
        return std::unexpected(output.error());
      if (!error)
        return std::unexpected(error.error());
      if (!auto_flush)
        return std::unexpected(auto_flush.error());
      if (!console)
        return std::unexpected(console.error());
      return *stream;
    };
    auto output = make_console_stream();
    auto error = make_console_stream();
    if (!output)
      return std::unexpected(output.error());
    if (!error)
      return std::unexpected(error.error());

    auto input_field = states_.resolve_field("java/lang/System", "in",
                                             "Ljava/io/InputStream;", true);
    auto output_field = states_.resolve_field("java/lang/System", "out",
                                              "Ljava/io/PrintStream;", true);
    auto error_field = states_.resolve_field("java/lang/System", "err",
                                             "Ljava/io/PrintStream;", true);
    if (!input_field)
      return std::unexpected(input_field.error());
    if (!output_field)
      return std::unexpected(output_field.error());
    if (!error_field)
      return std::unexpected(error_field.error());
    auto input_stored = states_.set_static_field(
        *input_field, Value::from_reference(*input));
    auto output_stored = states_.set_static_field(
        *output_field, Value::from_reference(*output));
    auto error_stored = states_.set_static_field(
        *error_field, Value::from_reference(*error));
    if (!input_stored)
      return input_stored;
    if (!output_stored)
      return output_stored;
    if (!error_stored)
      return error_stored;
    system_streams_initialized_ = true;
    return {};
  }

  void Machine::append_console(std::u16string_view text)
  {
    constexpr usize maximum_units = 65'536U;
    if (text.size() >= maximum_units)
    {
      console_output_.assign(text.end() -
                                 static_cast<std::ptrdiff_t>(maximum_units),
                             text.end());
      return;
    }
    if (console_output_.size() > maximum_units - text.size())
    {
      console_output_.erase(
          0, console_output_.size() - (maximum_units - text.size()));
    }
    console_output_.append(text);
  }

  void Machine::configure_ui_bridge(i32 app_namespace, UiEventSink sink)
  {
    ui_event_sink_ = std::move(sink);
    const i64 normalized_namespace =
        app_namespace > 0 ? static_cast<i64>(app_namespace) : 0LL;
    constexpr i64 kNamespaceWidth = 100'000LL;
    constexpr i64 kMaximumBase =
        static_cast<i64>(std::numeric_limits<i32>::max()) -
        kNamespaceWidth;
    const i64 base = normalized_namespace <= kMaximumBase / kNamespaceWidth
        ? normalized_namespace * kNamespaceWidth
        : 0LL;
    next_ui_component_id_ = static_cast<i32>(base + 1LL);
    ui_generation_ = 0;
  }

  i32 Machine::allocate_ui_component_id() noexcept
  {
    if (next_ui_component_id_ == std::numeric_limits<i32>::max())
      return 0;
    return next_ui_component_id_++;
  }

  void Machine::emit_ui_event(UiBridgeEvent event)
  {
    if (!ui_event_sink_)
      return;
    event.generation = ++ui_generation_;
    ui_event_sink_(std::move(event));
  }

  Status Machine::enqueue_serial_callback(ObjectRef runnable)
  {
    if (runnable.is_null())
    {
      return fail(ErrorCode::invalid_argument,
                  "LCDUI serial callback is null");
    }
    auto root = pin_native_root(runnable);
    if (!root)
      return std::unexpected(root.error());

    constexpr usize kMaximumQueuedCallbacks = 4'096U;
    std::scoped_lock lock(serial_callbacks_mutex_);
    if (serial_callback_coalescing_)
    {
      for (const auto &queued : serial_callbacks_)
      {
        auto queued_runnable = queued.get();
        if (queued_runnable && *queued_runnable == runnable)
          return {};
      }
    }
    if (serial_callbacks_.size() >= kMaximumQueuedCallbacks)
    {
      return fail(ErrorCode::overflow,
                  "LCDUI serial callback queue is full");
    }
    serial_callbacks_.push_back(std::move(*root));
    return {};
  }

  Status Machine::pump_serial_callbacks(usize maximum_callbacks)
  {
    if (maximum_callbacks == 0U)
      return {};

    NativeRootScope worker_thread_root;
    ObjectRef first_runnable {};
    {
      std::scoped_lock lock(serial_callbacks_mutex_);
      if (serial_callback_failure_.has_value())
      {
        Error failure = std::move(*serial_callback_failure_);
        serial_callback_failure_.reset();
        return std::unexpected(std::move(failure));
      }
      // Hidden MIDlets coalesce callSerially work but do not execute LCDUI
      // callbacks until they regain foreground ownership. Their timers,
      // sockets and non-UI Java workers continue independently.
      if (serial_callback_coalescing_ || serial_callback_worker_running_ ||
          serial_callbacks_.empty())
        return {};
      auto first = serial_callbacks_.front().get();
      if (!first)
        return std::unexpected(first.error());
      first_runnable = *first;
      serial_callback_worker_running_ = true;
    }

    auto allocated_thread = allocate_pinned_instance("java/lang/Thread");
    if (!allocated_thread)
    {
      std::scoped_lock lock(serial_callbacks_mutex_);
      serial_callback_worker_running_ = false;
      return std::unexpected(allocated_thread.error());
    }
    worker_thread_root = std::move(*allocated_thread);
    auto worker_thread = worker_thread_root.get();
    if (!worker_thread)
    {
      std::scoped_lock lock(serial_callbacks_mutex_);
      serial_callback_worker_running_ = false;
      return std::unexpected(worker_thread.error());
    }
    auto initialized = initialize_java_thread(*worker_thread, first_runnable);
    if (!initialized)
    {
      std::scoped_lock lock(serial_callbacks_mutex_);
      serial_callback_worker_running_ = false;
      return std::unexpected(initialized.error());
    }

    const ObjectRef worker_thread_object = *worker_thread;
    auto scheduled = scheduler_.start_native_thread(
        *this,
        worker_thread_object,
        [this, maximum_callbacks](std::stop_token stop_token)
            -> Result<std::optional<ObjectRef>> {
          const auto finish_worker = [this](std::optional<Error> failure = {}) {
            std::scoped_lock lock(serial_callbacks_mutex_);
            serial_callback_failure_ = std::move(failure);
            serial_callback_worker_running_ = false;
          };

          for (usize delivered = 0U;
               delivered < maximum_callbacks &&
               !stop_token.stop_requested();
               ++delivered)
          {
            NativeRootScope callback;
            {
              std::scoped_lock lock(serial_callbacks_mutex_);
              if (serial_callbacks_.empty())
              {
                serial_callback_worker_running_ = false;
                return std::optional<ObjectRef> {};
              }
              callback = std::move(serial_callbacks_.front());
              serial_callbacks_.pop_front();
            }

            auto runnable = callback.get();
            if (!runnable)
            {
              finish_worker(runnable.error());
              return std::optional<ObjectRef> {};
            }
            // Display.callSerially belongs to the LCDUI event thread. Running
            // the callback inline in the host pump lets a long resource loader
            // or game loop block frame delivery and the entire native UI. The
            // scheduler worker preserves callback ordering while allowing the
            // host to keep pumping Canvas frames and input concurrently.
            constexpr u64 kSerialCallbackInstructionBudget = 200'000'000U;
            auto result = invoke_instance(*runnable,
                                          "java/lang/Runnable",
                                          "run",
                                          "()V",
                                          {},
                                          kSerialCallbackInstructionBudget);
            if (!result)
            {
              finish_worker(result.error());
              return std::optional<ObjectRef> {};
            }
            if (result->completed_normally())
              continue;
            if (!result->throwable.has_value())
            {
              finish_worker(Error::make(
                  ErrorCode::internal_error,
                  "LCDUI serial callback failed without throwable"));
              return std::optional<ObjectRef> {};
            }
            auto throwable = heap_.class_name(*result->throwable);
            if (!throwable)
            {
              finish_worker(throwable.error());
              return std::optional<ObjectRef> {};
            }
            std::string diagnostic =
                "LCDUI serial callback threw " + *throwable;
            if (!result->exception_context.empty())
            {
              diagnostic += " from ";
              diagnostic += result->exception_context;
            }
            finish_worker(Error::make_java(*throwable,
                                           std::move(diagnostic)));
            return std::optional<ObjectRef> {};
          }

          finish_worker();

          // A callback may enqueue another callback while this worker is
          // running. Once the batch limit is reached, hand the remaining queue
          // to a fresh scheduler worker instead of waiting for an unrelated
          // Canvas/frame pump to happen to wake it up.
          if (!stop_token.stop_requested())
          {
            bool has_more = false;
            {
              std::scoped_lock lock(serial_callbacks_mutex_);
              has_more = !serial_callbacks_.empty();
            }
            if (has_more)
            {
              auto continued = pump_serial_callbacks(maximum_callbacks);
              if (!continued)
              {
                std::scoped_lock lock(serial_callbacks_mutex_);
                serial_callback_failure_ = continued.error();
                serial_callback_worker_running_ = false;
              }
            }
          }
          return std::optional<ObjectRef> {};
        });
    if (!scheduled)
    {
      std::scoped_lock lock(serial_callbacks_mutex_);
      serial_callback_worker_running_ = false;
      return std::unexpected(scheduled.error());
    }
    return {};
  }

  usize Machine::pending_serial_callbacks() const noexcept
  {
    std::scoped_lock lock(serial_callbacks_mutex_);
    return serial_callbacks_.size() +
           (serial_callback_worker_running_ ? 1U : 0U);
  }

  void Machine::set_serial_callback_coalescing(bool enabled) noexcept
  {
    std::scoped_lock lock(serial_callbacks_mutex_);
    serial_callback_coalescing_ = enabled;
    if (!enabled || serial_callbacks_.size() < 2U)
      return;

    std::unordered_set<u64> retained;
    std::deque<NativeRootScope> compacted;
    while (!serial_callbacks_.empty())
    {
      auto callback = std::move(serial_callbacks_.front());
      serial_callbacks_.pop_front();
      auto runnable = callback.get();
      if (!runnable || runnable->is_null() ||
          !retained.insert(runnable->bits).second)
      {
        continue;
      }
      compacted.push_back(std::move(callback));
    }
    serial_callbacks_ = std::move(compacted);
  }

  Status Machine::schedule_lcdui_alert_timeout(ObjectRef alert,
                                                i32 timeout_millis)
  {
    if (alert.is_null() || timeout_millis <= 0)
    {
      return fail(ErrorCode::invalid_argument,
                  "LCDUI alert timeout schedule is invalid");
    }
    auto root = pin_native_root(alert);
    if (!root)
      return std::unexpected(root.error());
    std::scoped_lock lock(lcd_ui_alert_timeout_mutex_);
    lcd_ui_alert_timeout_root_ = std::move(*root);
    lcd_ui_alert_timeout_deadline_ =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_millis);
    return {};
  }

  void Machine::cancel_lcdui_alert_timeout(ObjectRef alert) noexcept
  {
    std::scoped_lock lock(lcd_ui_alert_timeout_mutex_);
    if (!lcd_ui_alert_timeout_root_.has_value())
      return;
    if (!alert.is_null())
    {
      auto scheduled = lcd_ui_alert_timeout_root_->get();
      if (!scheduled || *scheduled != alert)
        return;
    }
    lcd_ui_alert_timeout_root_.reset();
    lcd_ui_alert_timeout_deadline_ = {};
  }

  Result<std::optional<ObjectRef>> Machine::take_due_lcdui_alert()
  {
    std::scoped_lock lock(lcd_ui_alert_timeout_mutex_);
    if (!lcd_ui_alert_timeout_root_.has_value() ||
        std::chrono::steady_clock::now() < lcd_ui_alert_timeout_deadline_)
    {
      return std::optional<ObjectRef> {};
    }
    auto scheduled = lcd_ui_alert_timeout_root_->get();
    if (!scheduled)
      return std::unexpected(scheduled.error());
    const ObjectRef alert = *scheduled;
    lcd_ui_alert_timeout_root_.reset();
    lcd_ui_alert_timeout_deadline_ = {};
    return std::optional<ObjectRef>(alert);
  }

  Status Machine::register_ui_component(i32 component_id,
                                        ObjectRef object)
  {
    if (component_id <= 0 || object.is_null())
    {
      return fail(ErrorCode::invalid_argument,
                  "LCDUI component registration is invalid");
    }
    ui_components_.insert_or_assign(component_id, object);
    return {};
  }

  void Machine::unregister_ui_component(i32 component_id) noexcept
  {
    ui_components_.erase(component_id);
  }

  Result<ObjectRef> Machine::ui_component(i32 component_id) const
  {
    const auto found = ui_components_.find(component_id);
    if (found == ui_components_.end())
    {
      return fail(ErrorCode::out_of_range,
                  "LCDUI component ID is not registered");
    }
    return found->second;
  }

  double Machine::next_random_double() noexcept
  {
    random_state_ ^= random_state_ >> 12U;
    random_state_ ^= random_state_ << 25U;
    random_state_ ^= random_state_ >> 27U;
    const u64 value = random_state_ * 0x2545F4914F6CDD1DULL;
    return static_cast<double>(value >> 11U) *
           (1.0 / 9007199254740992.0);
  }

  Result<std::string> Machine::mirrored_class_name(ObjectRef mirror) const
  {
    if (mirror.is_null())
      return fail(ErrorCode::invalid_argument,
                  "class mirror reference is null");
    for (const auto &[class_name, reference] : class_mirrors_)
    {
      if (reference == mirror)
        return class_name;
    }
    return fail(ErrorCode::invalid_argument,
                "object is not a registered java/lang/Class mirror");
  }

  Result<bool> Machine::object_is_instance(
      ObjectRef object,
      std::string_view target_class)
  {
    if (object.is_null())
      return false;
    auto source_class = heap_.class_name(object);
    if (!source_class)
      return std::unexpected(source_class.error());
    auto assignable = classes_.is_assignable(*source_class, target_class);
    if (!assignable)
      return std::unexpected(assignable.error());
    if (*assignable)
      return true;
    const auto lambda = lambda_bindings_.find(object.bits);
    if (lambda == lambda_bindings_.end())
      return false;
    return std::find(lambda->second.marker_interfaces.begin(),
                     lambda->second.marker_interfaces.end(),
                     target_class) !=
           lambda->second.marker_interfaces.end();
  }

  void Machine::set_app_property(std::u16string key,
                                 std::u16string value)
  {
    if (key.empty())
      return;
    app_properties_.insert_or_assign(std::move(key), std::move(value));
  }

  Result<std::optional<std::u16string>> Machine::app_property(
      ObjectRef key) const
  {
    if (key.is_null())
      return fail_java("java/lang/NullPointerException",
                       "MIDlet property key is null");
    auto text = heap_.string_value(key);
    if (!text)
      return std::unexpected(text.error());
    const auto property = app_properties_.find(*text);
    if (property == app_properties_.end())
      return std::optional<std::u16string>{};
    return std::optional<std::u16string>(property->second);
  }

  void Machine::set_system_property(std::u16string key,
                                    std::u16string value)
  {
    if (key.empty())
      return;
    system_properties_.insert_or_assign(std::move(key), std::move(value));
  }

  std::optional<std::u16string> Machine::configured_system_property(
      std::u16string_view key) const
  {
    const auto property = system_properties_.find(std::u16string(key));
    if (property == system_properties_.end())
      return std::nullopt;
    return property->second;
  }

  Status Machine::configure_record_store_root(std::string root)
  {
    return record_stores_.configure(std::move(root));
  }

  void Machine::set_permission_policy(
      security::SharedPermissionPolicy policy)
  {
    if (policy)
      permission_policy_ = std::move(policy);
  }

  Status Machine::configure_push_registry(std::string root,
                                          SuiteId suite_id)
  {
    return push_registry_.configure(std::move(root), suite_id);
  }

  Status Machine::configure_filesystem(std::string sandbox_root,
                                       std::string temporary_root)
  {
    return filesystem_.configure(std::move(sandbox_root),
                                 std::move(temporary_root));
  }

  Status Machine::configure_network_owner(i32 owner) noexcept
  {
    if (owner <= 0)
      return fail(ErrorCode::invalid_argument,
                  "network owner must be a positive MIDlet ID");
    connections_.set_owner(owner);
    return {};
  }

  Status Machine::configure_network_adapter(
      std::shared_ptr<network::AsyncNetworkAdapter> adapter)
  {
    return connections_.set_adapter(std::move(adapter));
  }

  void Machine::close_connections() noexcept
  {
    connections_.close_all();
  }

  void Machine::signal_midlet(MidletSignal signal) noexcept
  {
    MidletSignal current = midlet_signal_.load(std::memory_order_acquire);
    for (;;)
    {
      if (current == MidletSignal::destroyed &&
          signal != MidletSignal::destroyed)
        return;
      if (midlet_signal_.compare_exchange_weak(
              current,
              signal,
              std::memory_order_acq_rel,
              std::memory_order_acquire))
        return;
    }
  }

  MidletSignal Machine::consume_midlet_signal() noexcept
  {
    return midlet_signal_.exchange(MidletSignal::none,
                                   std::memory_order_acq_rel);
  }

  bool Machine::consume_midlet_destroyed_signal() noexcept
  {
    MidletSignal expected = MidletSignal::destroyed;
    return midlet_signal_.compare_exchange_strong(
        expected,
        MidletSignal::none,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
  }

  Result<ExecutionResult> Machine::invoke_static(
      std::string_view class_name,
      std::string_view method_name,
      std::string_view descriptor,
      std::span<const Value> arguments,
      u64 instruction_budget)
  {
    if (method_name != "<clinit>")
    {
      auto initialized = ensure_initialized(class_name, instruction_budget);
      if (!initialized)
      {
        return std::unexpected(initialized.error());
      }
      if (initialized->has_value())
      {
        return ExecutionResult{
            .return_value = std::nullopt,
            .throwable = **initialized,
            .executed_instructions = 0,
        };
      }
    }
    auto resolved = classes_.resolve_method(class_name, method_name, descriptor);
    if (!resolved)
    {
      return std::unexpected(resolved.error());
    }
    if ((resolved->method->access_flags & kAccStatic) == 0)
    {
      return fail(ErrorCode::invalid_state,
                  "invoke_static target is not static");
    }
    auto invocation = prepare_invocation(std::move(*resolved), arguments, false);
    if (!invocation)
    {
      return std::unexpected(invocation.error());
    }
    return execute(std::move(*invocation), instruction_budget);
  }

  Result<ExecutionResult> Machine::invoke_instance(
      ObjectRef receiver,
      std::string_view declared_class,
      std::string_view method_name,
      std::string_view descriptor,
      std::span<const Value> arguments,
      u64 instruction_budget,
      InstructionBudgetMode budget_mode)
  {
    if (receiver.is_null())
    {
      return fail(ErrorCode::invalid_argument,
                  "invoke_instance receiver is null");
    }
    auto runtime_class = heap_.class_name(receiver);
    if (!runtime_class)
    {
      return std::unexpected(runtime_class.error());
    }
    auto receiver_compatible = classes_.is_assignable(*runtime_class,
                                                      declared_class);
    if (!receiver_compatible)
    {
      return std::unexpected(receiver_compatible.error());
    }
    if (!*receiver_compatible)
    {
      return fail(ErrorCode::invalid_argument,
                  "invoke_instance receiver is not assignable to declared class");
    }
    const auto lambda = lambda_bindings_.find(receiver.bits);
    const bool lambda_descriptor_matches =
        lambda != lambda_bindings_.end() &&
        (lambda->second.sam_descriptor == descriptor ||
         std::find(lambda->second.bridge_descriptors.begin(),
                   lambda->second.bridge_descriptors.end(),
                   descriptor) != lambda->second.bridge_descriptors.end());
    if (lambda != lambda_bindings_.end() &&
        lambda->second.interface_name == declared_class &&
        lambda->second.sam_name == method_name &&
        lambda_descriptor_matches)
    {
      std::optional<ObjectRef> constructor_receiver;
      std::optional<NativeRootScope> constructor_receiver_root;
      if (lambda->second.implementation_kind == 8U)
      {
        auto allocated = allocate_pinned_instance(
            lambda->second.implementation.owner);
        if (!allocated)
          return std::unexpected(allocated.error());
        constructor_receiver_root.emplace(std::move(*allocated));
        auto reference = constructor_receiver_root->get();
        if (!reference)
          return std::unexpected(reference.error());
        constructor_receiver = *reference;
      }
      auto invocation = prepare_lambda_invocation(receiver,
                                                  lambda->second,
                                                  arguments,
                                                  constructor_receiver);
      if (!invocation)
        return std::unexpected(invocation.error());
      return execute(std::move(*invocation), instruction_budget, budget_mode);
    }

    Result<ResolvedMethod> resolved = method_name == "<init>"
                                          ? classes_.resolve_declared_method(declared_class,
                                                                             method_name,
                                                                             descriptor)
                                          : classes_.resolve_method(*runtime_class,
                                                                    method_name,
                                                                    descriptor);
    if (!resolved)
    {
      return std::unexpected(resolved.error());
    }
    if ((resolved->method->access_flags & kAccStatic) != 0)
    {
      return fail(ErrorCode::invalid_state,
                  "invoke_instance target is static");
    }

    std::vector<Value> invocation_arguments;
    invocation_arguments.reserve(arguments.size() + 1);
    invocation_arguments.push_back(Value::from_reference(receiver));
    invocation_arguments.insert(invocation_arguments.end(),
                                arguments.begin(),
                                arguments.end());
    auto invocation = prepare_invocation(std::move(*resolved),
                                         invocation_arguments,
                                         true);
    if (!invocation)
    {
      return std::unexpected(invocation.error());
    }
    return execute(std::move(*invocation), instruction_budget, budget_mode);
  }

  void Machine::refresh_metadata_bindings_if_needed() noexcept
  {
    // Interpreter metadata caches are only touched while the VM execution gate
    // is held. Native bindings are also resolved by host/native callback
    // threads before they enter execute(), so that cache has its own mutex and
    // is refreshed in resolve_native_binding().
    const u64 metadata_generation = classes_.metadata().generation();
    if (metadata_binding_generation_ != metadata_generation)
    {
      field_bindings_.clear();
      invoke_site_bindings_.clear();
      direct_call_bindings_.clear();
      virtual_call_bindings_.clear();
      trivial_getter_intrinsics_.clear();
      static_byte_cursor_read_intrinsics_.clear();
      tiled_alpha_collision_intrinsics_.clear();
      projectile_collision_intrinsics_.clear();
      operand_resolution_method_id_ = {};
      operand_resolution_method_.reset();
      runtime_method_bindings_.clear();
      descriptor_bindings_.clear();
      runtime_class_bindings_.clear();
      metadata_binding_generation_ = metadata_generation;
    }
  }

  std::shared_ptr<const RuntimeMethod>
  Machine::cached_runtime_method(MethodId method_id)
  {
    if (!method_id.valid()) return nullptr;
    const usize slot = static_cast<usize>(method_id.value);
    if (slot < runtime_method_bindings_.size() &&
        runtime_method_bindings_[slot] != nullptr)
    {
      return runtime_method_bindings_[slot];
    }
    auto runtime_method = classes_.metadata().find_method(method_id);
    if (runtime_method != nullptr)
    {
      if (slot >= runtime_method_bindings_.size())
        runtime_method_bindings_.resize(slot + 1U);
      runtime_method_bindings_[slot] = runtime_method;
    }
    return runtime_method;
  }

  Result<std::shared_ptr<const CachedMethodDescriptor>>
  Machine::cached_method_descriptor(std::string_view descriptor)
  {
    if (const auto cached = descriptor_bindings_.find(descriptor);
        cached != descriptor_bindings_.end())
    {
      return cached->second;
    }
    auto resolved = classes_.metadata().method_descriptor(descriptor);
    if (!resolved) return std::unexpected(resolved.error());
    descriptor_bindings_.emplace(std::string(descriptor), *resolved);
    return *resolved;
  }

  Result<std::optional<Machine::QuickFieldBinding>*> Machine::field_binding_slot(
      const classfile::ClassFile& owner,
      u16 constant_pool_index)
  {
    if (static_cast<usize>(constant_pool_index) >= owner.constants().size())
    {
      return fail(ErrorCode::malformed_class,
                  "field constant-pool index is out of range");
    }
    auto& bindings = field_bindings_[&owner];
    if (bindings.empty())
      bindings.resize(owner.constants().size());
    return &bindings[constant_pool_index];
  }

  Result<Machine::QuickFieldBinding> Machine::resolve_quick_field_binding(
      const classfile::MemberReference& reference,
      bool require_static)
  {
    auto resolved = states_.resolve_field(reference.owner,
                                          reference.name,
                                          reference.descriptor,
                                          require_static);
    if (!resolved)
      return std::unexpected(resolved.error());

    // Prepare the JVM default/ConstantValue once while the full symbolic
    // FieldLocation is still available. Steady-state getstatic/putstatic can
    // then address ClassStateRegistry directly by compact FieldId.
    if (resolved->is_static)
    {
      auto prepared = states_.static_field(*resolved);
      if (!prepared)
        return std::unexpected(prepared.error());
    }
    auto declaring_runtime_class =
        classes_.metadata().find_class(resolved->declaring_class_id);
    if (declaring_runtime_class == nullptr)
    {
      return fail(ErrorCode::internal_error,
                  "quick field owner has no runtime class metadata");
    }

    return QuickFieldBinding {
        .id = resolved->id,
        .declaring_class = resolved->declaring_class_id,
        .declaring_runtime_class = std::move(declaring_runtime_class),
        .index = resolved->index,
        .value_kind = resolved->value_kind,
        .is_static = resolved->is_static,
        .string_constant_value_index =
            resolved->descriptor == "Ljava/lang/String;"
                ? resolved->constant_value_index
                : std::nullopt,
    };
  }

  Result<Machine::InvokeSiteBinding*> Machine::invoke_site_binding(
      const classfile::ClassFile& owner,
      u16 constant_pool_index)
  {
    if (static_cast<usize>(constant_pool_index) >= owner.constants().size())
    {
      return fail(ErrorCode::malformed_class,
                  "invoke constant-pool index is out of range");
    }
    auto& bindings = invoke_site_bindings_[&owner];
    if (bindings.empty())
      bindings.resize(owner.constants().size());
    auto& slot = bindings[constant_pool_index];
    if (!slot.has_value())
    {
      auto reference = owner.member_reference(constant_pool_index);
      if (!reference)
        return std::unexpected(reference.error());
      auto descriptor = cached_method_descriptor(reference->descriptor);
      if (!descriptor)
        return std::unexpected(descriptor.error());
      slot = InvokeSiteBinding {
          .reference = std::move(*reference),
          .descriptor = std::move(*descriptor),
      };
    }
    return &slot.value();
  }

  Result<Machine::DirectCallCache*> Machine::direct_call_binding_slot(
      const classfile::ClassFile& owner,
      u16 constant_pool_index)
  {
    if (static_cast<usize>(constant_pool_index) >= owner.constants().size())
    {
      return fail(ErrorCode::malformed_class,
                  "direct-call constant-pool index is out of range");
    }
    auto& bindings = direct_call_bindings_[&owner];
    if (bindings.empty())
      bindings.resize(owner.constants().size());
    return &bindings[constant_pool_index];
  }

  Result<Machine::VirtualCallCache*> Machine::virtual_call_binding_slot(
      const classfile::ClassFile& owner,
      u16 constant_pool_index)
  {
    if (static_cast<usize>(constant_pool_index) >= owner.constants().size())
    {
      return fail(ErrorCode::malformed_class,
                  "virtual-call constant-pool index is out of range");
    }
    auto& bindings = virtual_call_bindings_[&owner];
    if (bindings.empty())
      bindings.resize(owner.constants().size());
    auto& slot = bindings[constant_pool_index];
    if (slot == nullptr)
      slot = std::make_unique<VirtualCallCache>();
    return slot.get();
  }

  std::shared_ptr<const RuntimeClass>
  Machine::cached_runtime_class(std::string_view class_name)
  {
    if (const auto cached = runtime_class_bindings_.find(class_name);
        cached != runtime_class_bindings_.end())
    {
      return cached->second;
    }
    auto runtime_class = classes_.metadata().find_class(class_name);
    if (runtime_class != nullptr)
    {
      runtime_class_bindings_.emplace(std::string(class_name), runtime_class);
    }
    return runtime_class;
  }

  Result<OperandResolutionEntry*>
  Machine::operand_resolution_entry(MethodId method_id,
                                    u32 operand_index,
                                    usize bytecode_pc)
  {
    if (!method_id.valid() || operand_index == kInvalidDecodedIndex ||
        bytecode_pc > static_cast<usize>(std::numeric_limits<u32>::max()))
    {
      return fail(ErrorCode::invalid_argument,
                  "decoded operand resolution requires valid IDs and BCI");
    }
    std::shared_ptr<const RuntimeMethod> runtime_method;
    if (operand_resolution_method_ != nullptr &&
        operand_resolution_method_id_ == method_id)
    {
      runtime_method = operand_resolution_method_;
    }
    else
    {
      runtime_method = cached_runtime_method(method_id);
      operand_resolution_method_id_ = method_id;
      operand_resolution_method_ = runtime_method;
    }
    if (runtime_method == nullptr || runtime_method->decoded == nullptr ||
        runtime_method->operand_resolutions == nullptr)
    {
      return fail(ErrorCode::invalid_state,
                  "decoded operand resolution has no runtime method table");
    }
    return runtime_method->operand_resolutions->entry(
        operand_index,
        static_cast<u32>(bytecode_pc));
  }

  Result<std::shared_ptr<const classfile::ClassFile>>
  Machine::load_linkage_class(std::string_view class_name)
  {
    auto loaded = classes_.load(class_name);
    if (!loaded)
      return std::unexpected(loaded.error());

    std::string_view component = class_name;
    while (!component.empty() && component.front() == '[')
      component.remove_prefix(1U);
    if (component.size() >= 3U && component.front() == 'L' &&
        component.back() == ';')
    {
      component.remove_prefix(1U);
      component.remove_suffix(1U);
      auto loaded_component = classes_.load(component);
      if (!loaded_component)
        return std::unexpected(loaded_component.error());
    }
    return *loaded;
  }

  Result<OperandResolutionEntry*> Machine::resolve_class_operand(
      MethodId method_id,
      u32 operand_index,
      usize bytecode_pc,
      const classfile::ClassFile& owner,
      u16 constant_pool_index,
      bool load_target_class,
      bool derive_reference_array_name)
  {
    if (!method_id.valid() || operand_index == kInvalidDecodedIndex)
      return static_cast<OperandResolutionEntry*>(nullptr);

    auto cached_entry = operand_resolution_entry(method_id,
                                                 operand_index,
                                                 bytecode_pc);
    if (!cached_entry)
      return std::unexpected(cached_entry.error());
    OperandResolutionEntry& entry = **cached_entry;
    if (entry.state == OperandResolutionState::resolved)
    {
      if (entry.kind != OperandResolutionKind::class_reference ||
          entry.target_class_name.empty() ||
          (load_target_class && entry.target_class_file == nullptr) ||
          (derive_reference_array_name && entry.target_array_name.empty()))
      {
        return fail(ErrorCode::internal_error,
                    "decoded class operand cache has wrong kind");
      }
      PerformanceCounters::record_operand_resolution(true);
      return &entry;
    }
    if (entry.state == OperandResolutionState::failed)
    {
      if (entry.kind != OperandResolutionKind::class_reference ||
          !entry.failure.has_value())
      {
        return fail(ErrorCode::internal_error,
                    "decoded failed class operand has no error");
      }
      PerformanceCounters::record_operand_resolution(true);
      PerformanceCounters::record_operand_resolution_failure();
      return std::unexpected(*entry.failure);
    }
    if (entry.state == OperandResolutionState::resolving)
    {
      return fail(ErrorCode::invalid_state,
                  "recursive decoded class operand resolution");
    }

    PerformanceCounters::record_operand_resolution(false);
    auto began = entry.begin(OperandResolutionKind::class_reference);
    if (!began)
      return std::unexpected(began.error());

    auto class_name = owner.class_name_constant(constant_pool_index);
    if (!class_name)
    {
      auto cached = entry.fail_resolution(stable_linkage_error(
          class_name.error(), OperandResolutionKind::class_reference));
      if (!cached)
        return std::unexpected(cached.error());
      PerformanceCounters::record_operand_resolution_failure();
      return std::unexpected(*entry.failure);
    }

    std::shared_ptr<const classfile::ClassFile> loaded_class;
    ClassId class_id;
    if (load_target_class)
    {
      auto loaded = load_linkage_class(*class_name);
      if (!loaded)
      {
        auto cached = entry.fail_resolution(stable_linkage_error(
            loaded.error(), OperandResolutionKind::class_reference));
        if (!cached)
          return std::unexpected(cached.error());
        PerformanceCounters::record_operand_resolution_failure();
        return std::unexpected(*entry.failure);
      }
      loaded_class = *loaded;
      const auto runtime_class = classes_.metadata().find_class(*class_name);
      if (runtime_class == nullptr)
      {
        auto cached = entry.fail_resolution(Error::make(
            ErrorCode::internal_error,
            "loaded decoded class operand has no runtime metadata"));
        if (!cached)
          return std::unexpected(cached.error());
        PerformanceCounters::record_operand_resolution_failure();
        return std::unexpected(*entry.failure);
      }
      class_id = runtime_class->id;
    }

    std::string array_name;
    if (derive_reference_array_name)
    {
      if (class_name->starts_with('['))
        array_name = '[' + *class_name;
      else
        array_name = "[L" + *class_name + ';';
    }
    auto completed = entry.resolve_class_reference(
        std::move(*class_name),
        class_id,
        std::move(loaded_class),
        std::move(array_name));
    if (!completed)
      return std::unexpected(completed.error());
    return &entry;
  }

  NativeMethodBinding Machine::resolve_native_binding(
      const classfile::ClassFile& owner,
      const classfile::Method& method,
      MethodId runtime_method_id)
  {
    std::scoped_lock cache_lock(native_bindings_mutex_);
    const u64 registry_generation = natives_.generation();
    if (native_binding_generation_ != registry_generation)
    {
      native_bindings_by_method_.clear();
      native_bindings_.clear();
      native_binding_generation_ = registry_generation;
    }

    if (runtime_method_id.valid())
    {
      const usize slot = static_cast<usize>(runtime_method_id.value);
      if (slot < native_bindings_by_method_.size() &&
          native_bindings_by_method_[slot].has_value())
      {
        return NativeMethodBinding {
            .id = *native_bindings_by_method_[slot],
            .generation = native_binding_generation_,
        };
      }
    }
    else if (const auto cached = native_bindings_.find(&method);
             cached != native_bindings_.end())
    {
      return NativeMethodBinding {
          .id = cached->second,
          .generation = native_binding_generation_,
      };
    }

    const NativeMethodBinding binding = natives_.resolve_binding(
        owner.name(), method.name, method.descriptor);
    if (native_binding_generation_ != binding.generation)
    {
      native_bindings_.clear();
      native_binding_generation_ = binding.generation;
    }
    if (runtime_method_id.valid())
    {
      const usize slot = static_cast<usize>(runtime_method_id.value);
      if (native_bindings_by_method_.size() <= slot)
        native_bindings_by_method_.resize(slot + 1U);
      native_bindings_by_method_[slot] = binding.id;
    }
    else
    {
      native_bindings_.insert_or_assign(&method, binding.id);
    }
    return binding;
  }

  Result<Machine::Invocation> Machine::prepare_invocation(
      ResolvedMethod method,
      std::span<const Value> arguments,
      bool has_receiver,
      std::optional<NativeMethodId> prebound_native_method,
      bool arguments_verified)
  {
    return prepare_invocation(std::move(method),
                              InvocationArguments(arguments),
                              has_receiver,
                              prebound_native_method,
                              arguments_verified);
  }

  Result<Machine::Invocation> Machine::prepare_invocation(
      ResolvedMethod method,
      InvocationArguments arguments,
      bool has_receiver,
      std::optional<NativeMethodId> prebound_native_method,
      bool arguments_verified)
  {
    if (method.method == nullptr)
    {
      return fail(ErrorCode::method_not_found,
                  "resolved method is null");
    }
    std::shared_ptr<const CachedMethodDescriptor> descriptor =
        method.runtime != nullptr ? method.runtime->descriptor : nullptr;
    if (descriptor == nullptr)
    {
      auto cached = classes_.metadata().method_descriptor(
          method.method->descriptor);
      if (!cached)
      {
        return std::unexpected(cached.error());
      }
      descriptor = std::move(*cached);
    }

    const usize expected_values = descriptor->argument_values +
                                  (has_receiver ? 1U : 0U);
    if (arguments.size() != expected_values)
    {
      return fail(ErrorCode::invalid_argument,
                  "method argument count does not match its descriptor");
    }
    if (has_receiver)
    {
      if (arguments.kind(0U) != ValueKind::reference ||
          arguments.reference_unchecked(0U).is_null())
      {
        return fail(ErrorCode::invalid_argument,
                    "instance method receiver is invalid");
      }
    }
    if (!arguments_verified)
    {
      for (usize index = 0;
           index < descriptor->descriptor.parameters.size();
           ++index)
      {
        const Value &value = arguments[index + (has_receiver ? 1U : 0U)];
        if (!value_matches(value, descriptor->descriptor.parameters[index]))
        {
          const std::string owner_name = method.owner != nullptr
              ? method.owner->name()
              : std::string("<unknown-owner>");
          return fail(
              ErrorCode::invalid_argument,
              "method argument does not match its descriptor for " +
                  owner_name + "." + method.method->name +
                  method.method->descriptor + " parameter=" +
                  std::to_string(index) + " expectedKind=" +
                  std::to_string(static_cast<unsigned>(
                      descriptor->descriptor.parameters[index].kind)) +
                  " actualKind=" + std::to_string(static_cast<unsigned>(
                      value.kind())));
        }
      }
    }

    NativeMethodId native_method;
    if (prebound_native_method.has_value())
    {
      native_method = *prebound_native_method;
    }
    else if (method.owner != nullptr)
    {
      if (method.runtime != nullptr)
      {
        const u64 registry_generation = natives_.generation();
        const u64 cached_generation =
            method.runtime->cached_native_generation.load(
                std::memory_order_acquire);
        if (cached_generation == registry_generation)
        {
          native_method = NativeMethodId {
              method.runtime->cached_native_method_id.load(
                  std::memory_order_relaxed),
          };
        }
        else
        {
          const NativeMethodBinding binding = resolve_native_binding(
              *method.owner, *method.method, method.runtime->id);
          method.runtime->cached_native_method_id.store(
              binding.id.value, std::memory_order_relaxed);
          method.runtime->cached_native_generation.store(
              binding.generation, std::memory_order_release);
          native_method = binding.id;
        }
      }
      else
      {
        native_method = resolve_native_binding(
            *method.owner, *method.method, MethodId {}).id;
      }
    }

    PerformanceCounters::record_method_invocation();
    PerformanceCounters::observe_invocation_arguments(
        arguments.size(),
        arguments.size() > kInlineInvocationArgumentCapacity);
    return Invocation{
        .method = std::move(method),
        .descriptor = std::move(descriptor),
        .native_method = native_method,
        .arguments = std::move(arguments),
        .has_receiver = has_receiver,
    };
  }

  Result<std::optional<ObjectRef>>
  Machine::ensure_default_interfaces_initialized(
      const classfile::ClassFile &owner,
      u64 instruction_budget)
  {
    for (const std::string &interface_name : owner.interfaces())
    {
      auto interface_class = classes_.load(interface_name);
      if (!interface_class)
        return std::unexpected(interface_class.error());
      auto parent_result = ensure_default_interfaces_initialized(
          **interface_class,
          instruction_budget);
      if (!parent_result)
        return std::unexpected(parent_result.error());
      if (parent_result->has_value())
        return *parent_result;

      const bool declares_default_method = std::any_of(
          (*interface_class)->methods().begin(),
          (*interface_class)->methods().end(),
          [](const classfile::Method &method)
          {
            const u16 disallowed = kAccPrivate | kAccStatic | kAccAbstract;
            return method.name != "<init>" &&
                   method.name != "<clinit>" &&
                   (method.access_flags & disallowed) == 0U;
          });
      if (!declares_default_method)
        continue;
      auto initialized = ensure_initialized(interface_name,
                                            instruction_budget);
      if (!initialized)
        return std::unexpected(initialized.error());
      if (initialized->has_value())
        return *initialized;
    }
    return std::optional<ObjectRef>{};
  }

  Result<std::optional<ObjectRef>> Machine::ensure_initialized(
      std::string_view class_name,
      u64 instruction_budget)
  {
    auto loaded = classes_.load(class_name);
    if (!loaded)
    {
      return std::unexpected(loaded.error());
    }
    const std::string canonical_name = (*loaded)->name();
    static constexpr std::array<std::pair<std::string_view,
                                          std::string_view>, 9>
        primitive_type_owners {{
            {"java/lang/Boolean", "Z"},
            {"java/lang/Byte", "B"},
            {"java/lang/Character", "C"},
            {"java/lang/Short", "S"},
            {"java/lang/Integer", "I"},
            {"java/lang/Long", "J"},
            {"java/lang/Float", "F"},
            {"java/lang/Double", "D"},
            {"java/lang/Void", "V"},
        }};
    const auto primitive_owner = std::find_if(
        primitive_type_owners.begin(), primitive_type_owners.end(),
        [&canonical_name](const auto& entry) {
          return entry.first == canonical_name;
        });
    if (primitive_owner != primitive_type_owners.end())
    {
      auto type_field = states_.resolve_field(
          canonical_name, "TYPE", "Ljava/lang/Class;", true);
      if (!type_field)
        return std::unexpected(type_field.error());
      auto current_type = states_.static_field(*type_field);
      if (!current_type)
        return std::unexpected(current_type.error());
      auto current_mirror = current_type->as_reference();
      if (!current_mirror)
        return std::unexpected(current_mirror.error());
      if (current_mirror->is_null())
      {
        auto mirror = class_mirror(primitive_owner->second);
        if (!mirror)
          return std::unexpected(mirror.error());
        auto stored = states_.set_static_field(
            *type_field, Value::from_reference(*mirror));
        if (!stored)
          return std::unexpected(stored.error());
      }
    }
    const auto set_static_constant =
        [this, &canonical_name](std::string_view field_name,
                                std::string_view descriptor,
                                Value value) -> Status
    {
      auto field = states_.resolve_field(
          canonical_name, field_name, descriptor, true);
      if (!field)
        return std::unexpected(field.error());
      return states_.set_static_field(*field, value);
    };
    const auto set_int_constants =
        [&set_static_constant](
            std::span<const std::pair<std::string_view, i32>> constants,
            std::string_view descriptor = "I") -> Status
    {
      for (const auto &[field_name, value] : constants)
      {
        auto stored = set_static_constant(
            field_name, descriptor, Value::from_int(value));
        if (!stored)
          return std::unexpected(stored.error());
      }
      return {};
    };
    const auto set_static_string =
        [this, &set_static_constant](std::string_view field_name,
                                     std::string_view value) -> Status
    {
      std::u16string text;
      text.reserve(value.size());
      for (const char character : value)
        text.push_back(static_cast<char16_t>(
            static_cast<unsigned char>(character)));
      auto string = states_.allocate_text_instance(
          heap_, "java/lang/String", std::move(text));
      if (!string)
        return std::unexpected(string.error());
      return set_static_constant(
          field_name, "Ljava/lang/String;",
          Value::from_reference(*string));
    };
    if (canonical_name == "java/lang/Byte")
    {
      auto minimum = set_static_constant(
          "MIN_VALUE", "B", Value::from_int(std::numeric_limits<i8>::min()));
      auto maximum = set_static_constant(
          "MAX_VALUE", "B", Value::from_int(std::numeric_limits<i8>::max()));
      if (!minimum) return std::unexpected(minimum.error());
      if (!maximum) return std::unexpected(maximum.error());
    }
    else if (canonical_name == "java/lang/Short")
    {
      auto minimum = set_static_constant(
          "MIN_VALUE", "S", Value::from_int(std::numeric_limits<i16>::min()));
      auto maximum = set_static_constant(
          "MAX_VALUE", "S", Value::from_int(std::numeric_limits<i16>::max()));
      if (!minimum) return std::unexpected(minimum.error());
      if (!maximum) return std::unexpected(maximum.error());
    }
    else if (canonical_name == "java/lang/Integer")
    {
      auto minimum = set_static_constant(
          "MIN_VALUE", "I", Value::from_int(std::numeric_limits<i32>::min()));
      auto maximum = set_static_constant(
          "MAX_VALUE", "I", Value::from_int(std::numeric_limits<i32>::max()));
      if (!minimum) return std::unexpected(minimum.error());
      if (!maximum) return std::unexpected(maximum.error());
    }
    else if (canonical_name == "java/lang/Long")
    {
      auto minimum = set_static_constant(
          "MIN_VALUE", "J", Value::from_long(std::numeric_limits<i64>::min()));
      auto maximum = set_static_constant(
          "MAX_VALUE", "J", Value::from_long(std::numeric_limits<i64>::max()));
      if (!minimum) return std::unexpected(minimum.error());
      if (!maximum) return std::unexpected(maximum.error());
    }
    else if (canonical_name == "java/lang/Character")
    {
      const std::array<std::pair<std::string_view, i32>, 4> constants {{
          {"MIN_RADIX", 2},
          {"MAX_RADIX", 36},
          {"MIN_VALUE", 0},
          {"MAX_VALUE", 0xFFFF},
      }};
      for (const auto &[field_name, value] : constants)
      {
        const std::string_view descriptor =
            field_name.ends_with("VALUE") ? "C" : "I";
        auto stored = set_static_constant(
            field_name, descriptor, Value::from_int(value));
        if (!stored) return std::unexpected(stored.error());
      }
    }
    else if (canonical_name == "java/lang/Float")
    {
      const std::array<std::pair<std::string_view, float>, 5> constants {{
          {"POSITIVE_INFINITY", std::numeric_limits<float>::infinity()},
          {"NEGATIVE_INFINITY", -std::numeric_limits<float>::infinity()},
          {"NaN", std::numeric_limits<float>::quiet_NaN()},
          {"MAX_VALUE", std::numeric_limits<float>::max()},
          {"MIN_VALUE", std::numeric_limits<float>::denorm_min()},
      }};
      for (const auto &[field_name, value] : constants)
      {
        auto stored = set_static_constant(
            field_name, "F", Value::from_float(value));
        if (!stored) return std::unexpected(stored.error());
      }
    }
    else if (canonical_name == "java/lang/Double")
    {
      const std::array<std::pair<std::string_view, double>, 5> constants {{
          {"POSITIVE_INFINITY", std::numeric_limits<double>::infinity()},
          {"NEGATIVE_INFINITY", -std::numeric_limits<double>::infinity()},
          {"NaN", std::numeric_limits<double>::quiet_NaN()},
          {"MAX_VALUE", std::numeric_limits<double>::max()},
          {"MIN_VALUE", std::numeric_limits<double>::denorm_min()},
      }};
      for (const auto &[field_name, value] : constants)
      {
        auto stored = set_static_constant(
            field_name, "D", Value::from_double(value));
        if (!stored) return std::unexpected(stored.error());
      }
    }
    else if (canonical_name == "java/lang/Math")
    {
      auto e = set_static_constant(
          "E", "D", Value::from_double(std::numbers::e_v<double>));
      auto pi = set_static_constant(
          "PI", "D", Value::from_double(std::numbers::pi_v<double>));
      if (!e) return std::unexpected(e.error());
      if (!pi) return std::unexpected(pi.error());
    }
    else if (canonical_name == "java/util/Calendar")
    {
      const std::array<std::pair<std::string_view, i32>, 31> constants {{
          {"YEAR", 1}, {"MONTH", 2}, {"DATE", 5},
          {"DAY_OF_MONTH", 5}, {"DAY_OF_WEEK", 7}, {"AM_PM", 9},
          {"HOUR", 10}, {"HOUR_OF_DAY", 11}, {"MINUTE", 12},
          {"SECOND", 13}, {"MILLISECOND", 14},
          {"SUNDAY", 1}, {"MONDAY", 2}, {"TUESDAY", 3},
          {"WEDNESDAY", 4}, {"THURSDAY", 5}, {"FRIDAY", 6},
          {"SATURDAY", 7},
          {"JANUARY", 0}, {"FEBRUARY", 1}, {"MARCH", 2},
          {"APRIL", 3}, {"MAY", 4}, {"JUNE", 5}, {"JULY", 6},
          {"AUGUST", 7}, {"SEPTEMBER", 8}, {"OCTOBER", 9},
          {"NOVEMBER", 10}, {"DECEMBER", 11}, {"AM", 0},
      }};
      for (const auto &[field_name, value] : constants)
      {
        auto stored = set_static_constant(
            field_name, "I", Value::from_int(value));
        if (!stored) return std::unexpected(stored.error());
      }
      auto pm = set_static_constant("PM", "I", Value::from_int(1));
      if (!pm) return std::unexpected(pm.error());
    }
    else if (canonical_name == "javax/microedition/lcdui/Choice")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"EXCLUSIVE", 1},
          {"MULTIPLE", 2}, {"IMPLICIT", 3}, {"POPUP", 4},
          {"TEXT_WRAP_DEFAULT", 0}, {"TEXT_WRAP_ON", 1},
          {"TEXT_WRAP_OFF", 2},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/lcdui/Canvas")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"KEY_NUM0", 48},
          {"KEY_NUM1", 49}, {"KEY_NUM2", 50}, {"KEY_NUM3", 51},
          {"KEY_NUM4", 52}, {"KEY_NUM5", 53}, {"KEY_NUM6", 54},
          {"KEY_NUM7", 55}, {"KEY_NUM8", 56}, {"KEY_NUM9", 57},
          {"KEY_STAR", 42}, {"KEY_POUND", 35}, {"UP", 1},
          {"DOWN", 6}, {"LEFT", 2}, {"RIGHT", 5}, {"FIRE", 8},
          {"GAME_A", 9}, {"GAME_B", 10}, {"GAME_C", 11},
          {"GAME_D", 12},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/lcdui/game/GameCanvas")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"UP_PRESSED", 1 << 1},
          {"DOWN_PRESSED", 1 << 6}, {"LEFT_PRESSED", 1 << 2},
          {"RIGHT_PRESSED", 1 << 5}, {"FIRE_PRESSED", 1 << 8},
          {"GAME_A_PRESSED", 1 << 9}, {"GAME_B_PRESSED", 1 << 10},
          {"GAME_C_PRESSED", 1 << 11}, {"GAME_D_PRESSED", 1 << 12},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/lcdui/Graphics")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"HCENTER", 1},
          {"VCENTER", 2}, {"LEFT", 4}, {"RIGHT", 8}, {"TOP", 16},
          {"BOTTOM", 32}, {"BASELINE", 64}, {"SOLID", 0},
          {"DOTTED", 1},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/lcdui/Font")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"STYLE_PLAIN", 0},
          {"STYLE_BOLD", 1}, {"STYLE_ITALIC", 2},
          {"STYLE_UNDERLINED", 4}, {"SIZE_SMALL", 8},
          {"SIZE_MEDIUM", 0}, {"SIZE_LARGE", 16}, {"FACE_SYSTEM", 0},
          {"FACE_MONOSPACE", 32}, {"FACE_PROPORTIONAL", 64},
          {"FONT_STATIC_TEXT", 0}, {"FONT_INPUT_TEXT", 1},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/lcdui/Command")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"SCREEN", 1}, {"BACK", 2},
          {"CANCEL", 3}, {"OK", 4}, {"HELP", 5}, {"STOP", 6},
          {"EXIT", 7}, {"ITEM", 8},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/lcdui/Item")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"LAYOUT_DEFAULT", 0},
          {"LAYOUT_LEFT", 1}, {"LAYOUT_RIGHT", 2},
          {"LAYOUT_CENTER", 3}, {"LAYOUT_TOP", 0x10},
          {"LAYOUT_BOTTOM", 0x20}, {"LAYOUT_VCENTER", 0x30},
          {"LAYOUT_NEWLINE_BEFORE", 0x100},
          {"LAYOUT_NEWLINE_AFTER", 0x200}, {"LAYOUT_SHRINK", 0x400},
          {"LAYOUT_EXPAND", 0x800}, {"LAYOUT_VSHRINK", 0x1000},
          {"LAYOUT_VEXPAND", 0x2000}, {"LAYOUT_2", 0x4000},
          {"PLAIN", 0}, {"HYPERLINK", 1}, {"BUTTON", 2},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/lcdui/TextField")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"ANY", 0}, {"EMAILADDR", 1},
          {"NUMERIC", 2}, {"PHONENUMBER", 3}, {"URL", 4},
          {"DECIMAL", 5}, {"CONSTRAINT_MASK", 0xFFFF},
          {"PASSWORD", 0x10000}, {"UNEDITABLE", 0x20000},
          {"SENSITIVE", 0x40000}, {"NON_PREDICTIVE", 0x80000},
          {"INITIAL_CAPS_WORD", 0x100000},
          {"INITIAL_CAPS_SENTENCE", 0x200000},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/lcdui/Gauge")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"INDEFINITE", -1},
          {"CONTINUOUS_IDLE", 0}, {"INCREMENTAL_IDLE", 1},
          {"CONTINUOUS_RUNNING", 2}, {"INCREMENTAL_UPDATING", 3},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/lcdui/DateField")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"DATE", 1}, {"TIME", 2},
          {"DATE_TIME", 3},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/lcdui/CustomItem")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"NONE", 0},
          {"TRAVERSE_HORIZONTAL", 1}, {"TRAVERSE_VERTICAL", 2},
          {"KEY_PRESS", 4}, {"KEY_RELEASE", 8}, {"KEY_REPEAT", 0x10},
          {"POINTER_PRESS", 0x20}, {"POINTER_RELEASE", 0x40},
          {"POINTER_DRAG", 0x80},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/lcdui/game/Sprite")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"TRANS_NONE", 0},
          {"TRANS_ROT90", 5}, {"TRANS_ROT180", 3}, {"TRANS_ROT270", 6},
          {"TRANS_MIRROR", 2}, {"TRANS_MIRROR_ROT90", 7},
          {"TRANS_MIRROR_ROT180", 1}, {"TRANS_MIRROR_ROT270", 4},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "com/nokia/mid/sound/Sound")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"FORMAT_TONE", 1},
          {"FORMAT_WAV", 5}, {"SOUND_PLAYING", 0},
          {"SOUND_STOPPED", 1}, {"SOUND_UNINITIALIZED", 3},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "com/nokia/mid/ui/DirectGraphics")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"FLIP_HORIZONTAL", 0x2000},
          {"FLIP_VERTICAL", 0x4000}, {"ROTATE_90", 90},
          {"ROTATE_180", 180}, {"ROTATE_270", 270},
          {"TYPE_BYTE_1_GRAY", 1}, {"TYPE_BYTE_1_GRAY_VERTICAL", -1},
          {"TYPE_BYTE_2_GRAY", 2}, {"TYPE_BYTE_4_GRAY", 4},
          {"TYPE_BYTE_8_GRAY", 8}, {"TYPE_BYTE_332_RGB", 332},
          {"TYPE_USHORT_4444_ARGB", 4444}, {"TYPE_USHORT_444_RGB", 444},
          {"TYPE_USHORT_555_RGB", 555}, {"TYPE_USHORT_1555_ARGB", 1555},
          {"TYPE_USHORT_565_RGB", 565}, {"TYPE_INT_888_RGB", 888},
          {"TYPE_INT_8888_ARGB", 8888},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "com/mascotcapsule/micro3d/v3/Effect3D")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"NORMAL_SHADING", 0},
          {"TOON_SHADING", 1},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "com/mascotcapsule/micro3d/v3/Graphics3D")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{
              "COMMAND_AFFINE_INDEX", std::bit_cast<i32>(0x87000000U)},
          {"COMMAND_AMBIENT_LIGHT", std::bit_cast<i32>(0xA0000000U)},
          {"COMMAND_ATTRIBUTE", std::bit_cast<i32>(0x83000000U)},
          {"COMMAND_CENTER", std::bit_cast<i32>(0x85000000U)},
          {"COMMAND_CLIP", std::bit_cast<i32>(0x84000000U)},
          {"COMMAND_DIRECTION_LIGHT", std::bit_cast<i32>(0xA1000000U)},
          {"COMMAND_END", std::bit_cast<i32>(0x80000000U)},
          {"COMMAND_FLUSH", std::bit_cast<i32>(0x82000000U)},
          {"COMMAND_LIST_VERSION_1_0", std::bit_cast<i32>(0xFE000001U)},
          {"COMMAND_NOP", std::bit_cast<i32>(0x81000000U)},
          {"COMMAND_PARALLEL_SCALE", std::bit_cast<i32>(0x90000000U)},
          {"COMMAND_PARALLEL_SIZE", std::bit_cast<i32>(0x91000000U)},
          {"COMMAND_PERSPECTIVE_FOV", std::bit_cast<i32>(0x92000000U)},
          {"COMMAND_PERSPECTIVE_WH", std::bit_cast<i32>(0x93000000U)},
          {"COMMAND_TEXTURE_INDEX", std::bit_cast<i32>(0x86000000U)},
          {"COMMAND_THRESHOLD", std::bit_cast<i32>(0xAF000000U)},
          {"ENV_ATTR_LIGHTING", 1}, {"ENV_ATTR_SEMI_TRANSPARENT", 8},
          {"ENV_ATTR_SPHERE_MAP", 2}, {"ENV_ATTR_TOON_SHADING", 4},
          {"PATTR_BLEND_ADD", 64}, {"PATTR_BLEND_HALF", 32},
          {"PATTR_BLEND_NORMAL", 0}, {"PATTR_BLEND_SUB", 96},
          {"PATTR_COLORKEY", 16}, {"PATTR_LIGHTING", 1},
          {"PATTR_SPHERE_MAP", 2}, {"PDATA_COLOR_NONE", 0},
          {"PDATA_COLOR_PER_COMMAND", 1024},
          {"PDATA_COLOR_PER_FACE", 2048}, {"PDATA_NORMAL_NONE", 0},
          {"PDATA_NORMAL_PER_FACE", 512},
          {"PDATA_NORMAL_PER_VERTEX", 768},
          {"PDATA_POINT_SPRITE_PARAMS_PER_CMD", 4096},
          {"PDATA_POINT_SPRITE_PARAMS_PER_FACE", 8192},
          {"PDATA_POINT_SPRITE_PARAMS_PER_VERTEX", 12288},
          {"PDATA_TEXURE_COORD", 12288}, {"PDATA_TEXURE_COORD_NONE", 0},
          {"POINT_SPRITE_LOCAL_SIZE", 0}, {"POINT_SPRITE_NO_PERS", 2},
          {"POINT_SPRITE_PERSPECTIVE", 0}, {"POINT_SPRITE_PIXEL_SIZE", 1},
          {"PRIMITVE_LINES", 0x02000000}, {"PRIMITVE_POINTS", 0x01000000},
          {"PRIMITVE_POINT_SPRITES", 0x05000000},
          {"PRIMITVE_QUADS", 0x04000000},
          {"PRIMITVE_TRIANGLES", 0x03000000},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "com/nokia/mid/ui/FullCanvas")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"KEY_UP_ARROW", -1},
          {"KEY_DOWN_ARROW", -2}, {"KEY_LEFT_ARROW", -3},
          {"KEY_RIGHT_ARROW", -4}, {"KEY_SOFTKEY1", -6},
          {"KEY_SOFTKEY2", -7}, {"KEY_SOFTKEY3", -5},
          {"KEY_SEND", -10}, {"KEY_END", -11},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/io/file/FileSystemListener")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"ROOT_ADDED", 0},
          {"ROOT_REMOVED", 1},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/media/Player")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"UNREALIZED", 100},
          {"REALIZED", 200}, {"PREFETCHED", 300}, {"STARTED", 400},
          {"CLOSED", 0},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
      auto unknown = set_static_constant(
          "TIME_UNKNOWN", "J", Value::from_long(-1));
      if (!unknown) return std::unexpected(unknown.error());
    }
    else if (canonical_name == "javax/microedition/media/control/ToneControl")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"VERSION", -2}, {"TEMPO", -3},
          {"RESOLUTION", -4}, {"BLOCK_START", -5}, {"BLOCK_END", -6},
          {"PLAY_BLOCK", -7}, {"SET_VOLUME", -8}, {"REPEAT", -9},
          {"SILENCE", -1}, {"C4", 60},
      };
      auto stored = set_int_constants(constants, "B");
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/media/Manager")
    {
      auto tone = set_static_string("TONE_DEVICE_LOCATOR", "device://tone");
      auto midi = set_static_string("MIDI_DEVICE_LOCATOR", "device://midi");
      if (!tone) return std::unexpected(tone.error());
      if (!midi) return std::unexpected(midi.error());
    }
    else if (canonical_name == "javax/microedition/media/PlayerListener")
    {
      constexpr std::pair<std::string_view, std::string_view> constants[] {
          std::pair<std::string_view, std::string_view>{"STARTED", "started"},
          {"STOPPED", "stopped"}, {"END_OF_MEDIA", "endOfMedia"},
          {"DURATION_UPDATED", "durationUpdated"},
          {"DEVICE_UNAVAILABLE", "deviceUnavailable"},
          {"DEVICE_AVAILABLE", "deviceAvailable"},
          {"VOLUME_CHANGED", "volumeChanged"}, {"ERROR", "error"},
          {"CLOSED", "closed"}, {"BUFFERING_STARTED", "bufferingStarted"},
          {"BUFFERING_STOPPED", "bufferingStopped"},
          {"RECORD_STARTED", "recordStarted"},
          {"RECORD_STOPPED", "recordStopped"},
          {"RECORD_ERROR", "recordError"}, {"SIZE_CHANGED", "sizeChanged"},
          {"STOPPED_AT_TIME", "stoppedAtTime"},
      };
      for (const auto &[field_name, value] : constants)
      {
        auto stored = set_static_string(field_name, value);
        if (!stored) return std::unexpected(stored.error());
      }
    }
    else if (canonical_name == "javax/microedition/media/control/GUIControl")
    {
      const std::array<std::pair<std::string_view, i32>, 1> constants {{
          {"USE_GUI_PRIMITIVE", 0},
      }};
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/media/control/MIDIControl")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"NOTE_ON", 0x90},
          {"CONTROL_CHANGE", 0xB0},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/media/control/MetaDataControl")
    {
      constexpr std::pair<std::string_view, std::string_view> constants[] {
          std::pair<std::string_view, std::string_view>{"AUTHOR_KEY", "author"},
          {"COPYRIGHT_KEY", "copyright"}, {"DATE_KEY", "date"},
          {"TITLE_KEY", "title"},
      };
      for (const auto &[field_name, value] : constants)
      {
        auto stored = set_static_string(field_name, value);
        if (!stored) return std::unexpected(stored.error());
      }
    }
    else if (canonical_name == "javax/microedition/media/control/StopTimeControl")
    {
      auto stored = set_static_constant(
          "RESET", "J", Value::from_long(std::numeric_limits<i64>::max()));
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/media/control/VideoControl")
    {
      const std::array<std::pair<std::string_view, i32>, 1> constants {{
          {"USE_DIRECT_VIDEO", 1},
      }};
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/media/protocol/SourceStream")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"NOT_SEEKABLE", 0},
          {"SEEKABLE_TO_START", 1}, {"RANDOM_ACCESSIBLE", 2},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/rms/RecordComparator")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"PRECEDES", -1},
          {"EQUIVALENT", 0}, {"FOLLOWS", 1},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    else if (canonical_name == "javax/microedition/rms/RecordStore")
    {
      constexpr std::pair<std::string_view, i32> constants[] {
          std::pair<std::string_view, i32>{"AUTHMODE_PRIVATE", 0},
          {"AUTHMODE_ANY", 1},
      };
      auto stored = set_int_constants(constants);
      if (!stored) return std::unexpected(stored.error());
    }
    if (canonical_name == "java/lang/Boolean")
    {
      const auto initialize_boolean_singleton =
          [this, &canonical_name](std::string_view field_name,
                                  bool value) -> Status
      {
        auto field = states_.resolve_field(
            canonical_name, field_name, "Ljava/lang/Boolean;", true);
        if (!field)
          return std::unexpected(field.error());
        auto current = states_.static_field(*field);
        if (!current)
          return std::unexpected(current.error());
        auto reference = current->as_reference();
        if (!reference)
          return std::unexpected(reference.error());
        if (!reference->is_null())
          return {};

        auto singleton = states_.allocate_instance(heap_, canonical_name);
        if (!singleton)
          return std::unexpected(singleton.error());
        auto stored_value = heap_.set_field(
            *singleton, 0U, Value::from_int(value ? 1 : 0));
        if (!stored_value)
          return std::unexpected(stored_value.error());
        return states_.set_static_field(
            *field, Value::from_reference(*singleton));
      };
      auto true_value = initialize_boolean_singleton("TRUE", true);
      if (!true_value)
        return std::unexpected(true_value.error());
      auto false_value = initialize_boolean_singleton("FALSE", false);
      if (!false_value)
        return std::unexpected(false_value.error());
    }
    if (canonical_name == "java/lang/System" &&
        !system_streams_initialized_)
    {
      auto streams = initialize_system_streams();
      if (!streams)
        return std::unexpected(streams.error());
    }
    const JavaThreadId initialization_thread =
        scheduler_.current_thread_id();
    for (;;)
    {
      bool erroneous = false;
      {
        std::unique_lock initialization_lock(class_initialization_mutex_);
        if (initialized_classes_.contains(canonical_name))
          return std::optional<ObjectRef>{};
        if (erroneous_classes_.contains(canonical_name))
        {
          erroneous = true;
        }
        else if (const auto owner =
                     initializing_class_owners_.find(canonical_name);
                 owner == initializing_class_owners_.end())
        {
          initializing_class_owners_.emplace(canonical_name,
                                             initialization_thread);
          vm_trace("class-init",
                   "begin java=%u class=%s",
                   static_cast<unsigned>(initialization_thread),
                   canonical_name.c_str());
          break;
        }
        else if (owner->second == initialization_thread)
        {
          // Recursive requests by the thread currently running <clinit> are
          // allowed to observe the class's in-progress default values.
          return std::optional<ObjectRef>{};
        }
        else
        {
          // phoneME keeps an initialization owner per class. A competing
          // thread must wait rather than treating a partially initialized
          // class as ready. Release the VM execution gate while waiting or the
          // owner could never resume after a scheduler quantum.
          vm_trace("class-init",
                   "wait java=%u class=%s owner=%u",
                   static_cast<unsigned>(initialization_thread),
                   canonical_name.c_str(),
                   static_cast<unsigned>(owner->second));
          scheduler_.set_current_state(JavaThreadState::waiting);
          const u32 released_depth = suspend_execution_for_blocking();
          class_initialization_condition_.wait(
              initialization_lock,
              [this, &canonical_name]
              {
                return shutdown_started_.load(std::memory_order_acquire) ||
                       initialized_classes_.contains(canonical_name) ||
                       erroneous_classes_.contains(canonical_name) ||
                       !initializing_class_owners_.contains(canonical_name);
              });
          // Never reacquire the execution gate while holding the class-state
          // mutex: the owner finishes <clinit> under that gate and must lock
          // class state to publish completion.
          initialization_lock.unlock();
          resume_execution_after_blocking(released_depth);
          scheduler_.set_current_state(JavaThreadState::running);
          vm_trace("class-init",
                   "resume java=%u class=%s",
                   static_cast<unsigned>(initialization_thread),
                   canonical_name.c_str());
          if (shutdown_started_.load(std::memory_order_acquire))
          {
            return fail(ErrorCode::invalid_state,
                        "class initialization was cancelled by VM shutdown");
          }
          continue;
        }
      }
      if (erroneous)
      {
        auto throwable = create_throwable("java/lang/NoClassDefFoundError");
        if (!throwable)
          return std::unexpected(throwable.error());
        return std::optional<ObjectRef>(*throwable);
      }
    }

    PerformanceCounters::record_class_initialization();
    const auto finish_initialization =
        [this, &canonical_name, initialization_thread](bool initialized,
                                                       bool erroneous)
    {
      {
        std::scoped_lock initialization_lock(class_initialization_mutex_);
        initializing_class_owners_.erase(canonical_name);
        if (initialized)
          initialized_classes_.insert(canonical_name);
        if (erroneous)
          erroneous_classes_.insert(canonical_name);
      }
      class_initialization_condition_.notify_all();
      vm_trace("class-init",
               "end java=%u class=%s result=%s",
               static_cast<unsigned>(initialization_thread),
               canonical_name.c_str(),
               initialized ? "initialized"
                           : (erroneous ? "erroneous" : "rolled-back"));
    };
    const auto rollback = [finish_initialization]()
    {
      finish_initialization(false, false);
    };

    const bool is_interface =
        ((*loaded)->access_flags() & kAccInterface) != 0U;
    if (!is_interface && !(*loaded)->super_name().empty())
    {
      auto parent = ensure_initialized((*loaded)->super_name(),
                                       instruction_budget);
      if (!parent)
      {
        rollback();
        return std::unexpected(parent.error());
      }
      if (parent->has_value())
      {
        finish_initialization(false, true);
        return *parent;
      }
    }
    if (!is_interface)
    {
      auto interfaces = ensure_default_interfaces_initialized(
          **loaded,
          instruction_budget);
      if (!interfaces)
      {
        rollback();
        return std::unexpected(interfaces.error());
      }
      if (interfaces->has_value())
      {
        finish_initialization(false, true);
        return *interfaces;
      }
    }

    if (const classfile::Method *initializer =
            (*loaded)->find_method("<clinit>", "()V");
        initializer != nullptr)
    {
      if ((initializer->access_flags & kAccStatic) == 0)
      {
        rollback();
        return fail(ErrorCode::malformed_class,
                    "class initializer is not static");
      }
      auto result = invoke_static(canonical_name,
                                  "<clinit>",
                                  "()V",
                                  {},
                                  instruction_budget);
      if (!result)
      {
        rollback();
        return std::unexpected(result.error());
      }
      if (result->throwable.has_value())
      {
        auto throwable_class = heap_.class_name(*result->throwable);
        if (!throwable_class)
        {
          rollback();
          return std::unexpected(throwable_class.error());
        }
        auto is_error = classes_.is_assignable(*throwable_class,
                                               "java/lang/Error");
        if (!is_error)
        {
          rollback();
          return std::unexpected(is_error.error());
        }

        finish_initialization(false, true);
        if (*is_error)
        {
          return result->throwable;
        }
        auto wrapper = create_throwable(
            "java/lang/ExceptionInInitializerError");
        if (!wrapper)
        {
          return std::unexpected(wrapper.error());
        }
        auto cause_stored = heap_.set_field(
            *wrapper, 1U, Value::from_reference(*result->throwable));
        if (!cause_stored)
        {
          return std::unexpected(cause_stored.error());
        }
        auto cause_initialized = heap_.set_field(
            *wrapper, 2U, Value::from_int(1));
        if (!cause_initialized)
        {
          return std::unexpected(cause_initialized.error());
        }
        return std::optional<ObjectRef>(*wrapper);
      }
    }

    finish_initialization(true, false);
    return std::optional<ObjectRef>{};
  }

  Result<std::optional<Value>> Machine::invoke_native(
      const Invocation &invocation)
  {
    if (invocation.method.method == nullptr ||
        invocation.method.owner == nullptr)
    {
      return fail(ErrorCode::internal_error,
                  "native invocation has no resolved method");
    }
    if (invocation.descriptor == nullptr)
    {
      return fail(ErrorCode::internal_error,
                  "native invocation has no cached descriptor");
    }
    const CachedMethodDescriptor &descriptor = *invocation.descriptor;
    const std::string &owner_name = invocation.method.owner->name();
    const std::string &method_name = invocation.method.method->name;
    const std::string &method_descriptor =
        invocation.method.method->descriptor;
    if (!invocation.native_method.valid() &&
        is_builtin_class(owner_name) && method_name == "<init>" &&
        method_descriptor == "()V")
    {
      return std::optional<Value>{};
    }

    std::array<Value, kInlineInvocationArgumentCapacity> inline_arguments;
    std::vector<Value> overflow_arguments;
    std::span<Value> materialized_arguments;
    if (invocation.arguments.size() <= inline_arguments.size())
    {
      materialized_arguments = std::span<Value>(
          inline_arguments.data(), invocation.arguments.size());
    }
    else
    {
      overflow_arguments.resize(invocation.arguments.size());
      materialized_arguments = overflow_arguments;
    }
    invocation.arguments.materialize(materialized_arguments);

    const HeapAccessContext previous_heap_context = current_heap_access_context();
    struct NativeHeapContextRestore final {
      HeapAccessContext previous;
      ~NativeHeapContextRestore() { set_heap_access_context(previous); }
    } heap_context_restore {previous_heap_context};
    set_heap_access_context(HeapAccessContext {
        .owner = owner_name,
        .method = method_name,
        .descriptor = method_descriptor,
        .bytecode_pc = previous_heap_context.current_bytecode_pc(),
        .live_bytecode_pc = nullptr,
    });

    Result<std::optional<Value>> result = invocation.native_method.valid()
        ? natives_.invoke(*this,
                          invocation.native_method,
                          std::span<const Value>(materialized_arguments))
        : fail(ErrorCode::unsupported_feature,
               "native method is not ported: " + owner_name + "." +
                   method_name + method_descriptor);
    if (!result)
    {
      return std::unexpected(result.error());
    }
    if (descriptor.return_kind == JavaTypeKind::void_type)
    {
      if (result->has_value())
      {
        return fail(ErrorCode::invalid_state,
                    "void native method returned a value");
      }
    }
    else
    {
      if (!result->has_value() ||
          !value_matches(**result, descriptor.descriptor.return_type))
      {
        return fail(ErrorCode::invalid_state,
                    "native method result does not match its descriptor");
      }
    }
    return *result;
  }

  Result<std::optional<u64>> Machine::try_read_byte_array_input_bits(
      ObjectRef input,
      usize byte_count)
  {
    // Some unit/native harnesses may invoke registry functions directly from
    // host code. Only use Heap's vm_* accessors when this Machine actually
    // owns the execution gate on the current thread; otherwise fall back to
    // the ordinary locked InputStream implementation in IONatives.cpp.
    if (!executing_on_current_thread())
      return std::optional<u64>{};
    if (input.is_null())
      return fail_java("java/lang/NullPointerException",
                       "input stream is null");
    if (byte_count == 0U || byte_count > sizeof(u64))
      return fail(ErrorCode::invalid_argument,
                  "primitive byte input width is invalid");

    ObjectRef current = input;
    constexpr usize kMaximumFastDepth = 64U;
    for (usize depth = 0U; depth < kMaximumFastDepth; ++depth)
    {
      auto class_name = heap_.vm_class_name_view(current);
      if (!class_name)
        return std::unexpected(class_name.error());

      if (*class_name == "java/io/DataInputStream" ||
          *class_name == "java/io/FilterInputStream")
      {
        auto wrapped_value = heap_.vm_field(current, 0U);
        if (!wrapped_value)
          return std::unexpected(wrapped_value.error());
        auto wrapped = wrapped_value->as_reference();
        if (!wrapped || wrapped->is_null())
          return fail_java("java/lang/NullPointerException",
                           "filter input stream is null");
        current = *wrapped;
        continue;
      }

      if (*class_name != "java/io/ByteArrayInputStream")
        return std::optional<u64>{};

      auto buffer_value = heap_.vm_field(current, 0U);
      auto position_value = heap_.vm_field(current, 1U);
      auto count_value = heap_.vm_field(current, 3U);
      if (!buffer_value) return std::unexpected(buffer_value.error());
      if (!position_value) return std::unexpected(position_value.error());
      if (!count_value) return std::unexpected(count_value.error());

      auto buffer = buffer_value->as_reference();
      auto position = position_value->as_int();
      auto count = count_value->as_int();
      if (!buffer || buffer->is_null() || !position || !count ||
          *position < 0 || *count < *position)
        return fail(ErrorCode::invalid_state,
                    "ByteArrayInputStream state is invalid");

      auto info = heap_.vm_array_info(*buffer);
      if (!info)
        return std::unexpected(info.error());
      if (info->kind != HeapArrayKind::byte)
        return fail(ErrorCode::invalid_state,
                    "ByteArrayInputStream buffer is not byte[]");
      if (static_cast<usize>(*count) > info->length)
        return fail(ErrorCode::invalid_state,
                    "ByteArrayInputStream count exceeds buffer length");

      const usize available = static_cast<usize>(*count - *position);
      const usize consumed = std::min(byte_count, available);
      u64 bits = 0U;
      for (usize index = 0U; index < consumed; ++index)
      {
        auto element = heap_.vm_element(
            *buffer, static_cast<usize>(*position) + index);
        if (!element)
          return std::unexpected(element.error());
        auto value = element->as_int();
        if (!value)
          return std::unexpected(value.error());
        const u8 byte = static_cast<u8>(static_cast<i8>(*value));
        bits = (bits << 8U) | byte;
      }

      if (consumed != 0U)
      {
        auto advanced = heap_.vm_set_field(
            current,
            1U,
            Value::from_int(*position + static_cast<i32>(consumed)));
        if (!advanced)
          return std::unexpected(advanced.error());
      }
      if (consumed != byte_count)
        return fail_java("java/io/EOFException",
                         "data stream reached end of input");
      return std::optional<u64>(bits);
    }

    return fail_java("java/io/IOException",
                     "input stream filter chain is too deep");
  }

  u32 Machine::jit_runtime_dispatch_callback(
      void* context,
      JitRuntimeOperation operation,
      u64 operand,
      u64 first,
      u64 second,
      u64 third,
      const u64* frame_base,
      u64* result_bits) noexcept
  {
    auto* execution = static_cast<JitExecutionContext*>(context);
    if (execution == nullptr || execution->machine == nullptr ||
        execution->owner == nullptr || result_bits == nullptr ||
        operand > static_cast<u64>(std::numeric_limits<u32>::max()))
    {
      return static_cast<u32>(JitRuntimeStatus::deoptimize);
    }
    const u32 encoded_operand = static_cast<u32>(operand);
    const bool no_safepoint =
        (encoded_operand & kJitRuntimeNoSafepointOperandFlag) != 0U;
    const u32 runtime_operand =
        encoded_operand & ~kJitRuntimeNoSafepointOperandFlag;
    u32* consumed_instructions = nullptr;
    if (frame_base != nullptr)
    {
      auto* frame_bytes = reinterpret_cast<u8*>(
          const_cast<u64*>(frame_base));
      consumed_instructions = reinterpret_cast<u32*>(
          frame_bytes + kJitRuntimeConsumedByteOffset);
      *consumed_instructions = 0U;
    }

    if (operation == JitRuntimeOperation::budget_safepoint)
    {
      if (!execution->progress_watchdog || frame_base == nullptr)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);

      auto* frame_bytes = reinterpret_cast<u8*>(
          const_cast<u64*>(frame_base));
      auto* initial_budget = reinterpret_cast<u32*>(
          frame_bytes + kJitRuntimeBudgetInitialByteOffset);
      auto* remaining_budget = reinterpret_cast<u32*>(
          frame_bytes + kJitRuntimeBudgetRemainingByteOffset);
      if (*initial_budget == 0U || *remaining_budget > *initial_budget)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      const u64 window_consumed =
          static_cast<u64>(*initial_budget - *remaining_budget);
      if (window_consumed == 0U ||
          window_consumed > execution->progress_total_budget ||
          execution->progress_reset_instructions >
              execution->progress_total_budget - window_consumed)
      {
        return static_cast<u32>(JitRuntimeStatus::budget_exhausted);
      }
      execution->progress_reset_instructions += window_consumed;
      *remaining_budget = *initial_budget;
      return static_cast<u32>(JitRuntimeStatus::success);
    }

    if (operation == JitRuntimeOperation::match_exception_handler)
    {
      if (no_safepoint || execution->method == nullptr ||
          !execution->method->code.has_value())
      {
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      const ObjectRef throwable{first};
      if (throwable.is_null())
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      auto throwable_class = execution->machine->heap_.class_name(throwable);
      if (!throwable_class)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      const usize throw_pc = static_cast<usize>(runtime_operand);
      for (const classfile::ExceptionHandler& handler :
           execution->method->code->exception_table)
      {
        if (throw_pc < static_cast<usize>(handler.start_pc) ||
            throw_pc >= static_cast<usize>(handler.end_pc))
        {
          continue;
        }
        bool catches = handler.catch_type.empty();
        if (!catches)
        {
          auto assignable = execution->machine->classes_.is_assignable(
              *throwable_class, handler.catch_type);
          if (!assignable)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          catches = *assignable;
        }
        if (catches)
        {
          *result_bits = static_cast<u64>(handler.handler_pc);
          return static_cast<u32>(JitRuntimeStatus::success);
        }
      }
      *result_bits = throwable.bits;
      execution->pending_throwable = throwable;
      return static_cast<u32>(JitRuntimeStatus::java_throwable);
    }

    // String.valueOf(int) is an allocation-heavy HLE primitive in the current
    // corpus and compiled loops can execute thousands of calls without
    // returning to the interpreter's periodic GC poll. The trampoline has
    // already published the precise frame roots for safepointing calls, so run
    // the proactive collection specifically at this allocation boundary. Do
    // not broaden this to every runtime call: new/newarray/multianewarray have
    // their own allocation-retry/root contracts and regression coverage.
    bool string_value_of_int_safepoint = false;
    if (!no_safepoint &&
        operation == JitRuntimeOperation::invoke_static &&
        runtime_operand <= static_cast<u32>(std::numeric_limits<u16>::max()))
    {
      auto reference = execution->owner->member_reference(
          static_cast<u16>(runtime_operand));
      string_value_of_int_safepoint = reference.has_value() &&
          reference->owner == "java/lang/String" &&
          reference->name == "valueOf" &&
          reference->descriptor == "(I)Ljava/lang/String;";
    }
    if (string_value_of_int_safepoint &&
        execution->machine->heap_.vm_automatic_collection_due())
    {
      execution->machine->commit_staged_jit_roots(execution);
      auto collected = execution->machine->collect_garbage();
      if (!collected)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
    }

    u32 status = execution->machine->dispatch_jit_runtime(
        execution,
        *execution->owner,
        operation,
        runtime_operand,
        first,
        second,
        third,
        frame_base,
        consumed_instructions,
        result_bits);

    const auto completed_java_call = [operation]() noexcept {
      switch (operation)
      {
      case JitRuntimeOperation::invoke_virtual:
      case JitRuntimeOperation::invoke_special:
      case JitRuntimeOperation::invoke_static:
      case JitRuntimeOperation::invoke_interface:
      case JitRuntimeOperation::invoke_dynamic:
        return true;
      default:
        return false;
      }
    };
    if (execution->progress_watchdog && frame_base != nullptr &&
        status == static_cast<u32>(JitRuntimeStatus::success) &&
        completed_java_call())
    {
      auto* frame_bytes = reinterpret_cast<u8*>(
          const_cast<u64*>(frame_base));
      auto* initial_budget = reinterpret_cast<u32*>(
          frame_bytes + kJitRuntimeBudgetInitialByteOffset);
      auto* remaining_budget = reinterpret_cast<u32*>(
          frame_bytes + kJitRuntimeBudgetRemainingByteOffset);
      const u64 window_consumed = *initial_budget >= *remaining_budget
          ? static_cast<u64>(*initial_budget - *remaining_budget)
          : 0U;
      const u64 nested_consumed = consumed_instructions != nullptr
          ? static_cast<u64>(*consumed_instructions)
          : 0U;
      const u64 reset_cost = window_consumed + nested_consumed;
      const u64 already_accounted = execution->progress_reset_instructions;
      if (reset_cost > execution->progress_total_budget ||
          already_accounted > execution->progress_total_budget - reset_cost)
      {
        return static_cast<u32>(JitRuntimeStatus::budget_exhausted);
      }
      execution->progress_reset_instructions += reset_cost;

      // emit_runtime_call() reloads this slot and then subtracts the nested
      // call's bytecode count. Pre-credit that exact count so the subtraction
      // leaves a fresh watchdog window, matching the interpreter's
      // `watchdog_instructions = 0` after a completed nested call.
      const u64 replenished = static_cast<u64>(*initial_budget) +
          nested_consumed;
      if (replenished > static_cast<u64>(std::numeric_limits<u32>::max()))
        return static_cast<u32>(JitRuntimeStatus::budget_exhausted);
      *remaining_budget = static_cast<u32>(replenished);
    }
    if (consumed_instructions != nullptr)
      execution->nested_instructions += *consumed_instructions;

    std::string_view implicit_exception_class;
    switch (static_cast<JitRuntimeStatus>(status))
    {
    case JitRuntimeStatus::null_pointer:
      implicit_exception_class = "java/lang/NullPointerException";
      break;
    case JitRuntimeStatus::array_index_out_of_bounds:
      implicit_exception_class = "java/lang/ArrayIndexOutOfBoundsException";
      break;
    case JitRuntimeStatus::array_store:
      implicit_exception_class = "java/lang/ArrayStoreException";
      break;
    case JitRuntimeStatus::class_cast:
      implicit_exception_class = "java/lang/ClassCastException";
      break;
    case JitRuntimeStatus::success:
    case JitRuntimeStatus::deoptimize:
    case JitRuntimeStatus::java_throwable:
    case JitRuntimeStatus::budget_exhausted:
    case JitRuntimeStatus::unsupported_call:
    case JitRuntimeStatus::fatal_runtime_error:
    case JitRuntimeStatus::retryable_call:
      break;
    }
    if (!implicit_exception_class.empty())
    {
      if (no_safepoint)
        return status;
      auto throwable = execution->machine->create_throwable(
          implicit_exception_class);
      if (!throwable)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      *result_bits = throwable->bits;
      status = static_cast<u32>(JitRuntimeStatus::java_throwable);
    }
    if (status == static_cast<u32>(JitRuntimeStatus::java_throwable))
      execution->pending_throwable = ObjectRef{*result_bits};
    return status;
  }

  void Machine::jit_publish_roots_callback(
      void* context,
      const u64* roots,
      usize root_count,
      const u64* frame_base,
      const u32* root_offsets,
      usize root_offset_count,
      bool defer_scheduler_publication) noexcept
  {
    auto* execution = static_cast<JitExecutionContext*>(context);
    if (execution == nullptr || execution->machine == nullptr)
      return;

    if (defer_scheduler_publication)
    {
      PerformanceCounters::record_jit_root_stage();
      PerformanceCounters::observe_jit_staged_reference_slots(
          root_offset_count, false);
      execution->frame_root_bits = {};
      execution->staged_frame_base = frame_base;
      execution->staged_root_offsets = root_offsets != nullptr
          ? std::span<const u32>(root_offsets, root_offset_count)
          : std::span<const u32>{};
      // Keep only the generated frame and immutable reference-map view. The
      // caller is suspended while a chained callee runs, so both remain valid
      // until the next runtime operation. Do not even read the reference slots
      // unless a real GC/blocking transition needs a flattened root set.
      execution->published_root_view = {};
      execution->roots_staged = true;
      return;
    }
    execution->frame_root_bits = roots != nullptr
        ? std::span<const u64>(roots, root_count)
        : std::span<const u64>{};
    execution->staged_frame_base = nullptr;
    execution->staged_root_offsets = {};
    execution->roots_staged = false;
    execution->published_roots.clear();
    append_jit_context_roots(execution, execution->published_roots);
    // The callback-owned vector is scratch storage and is rebuilt before the
    // next JIT safepoint, so transfer it into ExecutionContext instead of
    // copying the complete root list. The swap returns the previous published
    // buffer for reuse on the next callback, matching interpreter safepoints.
    execution->published_root_view =
        execution->machine->exchange_execution_roots(
        execution->invocation_depth,
        execution->published_roots);
  }

  void Machine::append_jit_context_roots(
      const JitExecutionContext* execution,
      std::vector<ObjectRef>& roots) noexcept
  {
    if (execution == nullptr)
      return;
    const usize roots_before = roots.size();
    const bool materializing_staged = execution->roots_staged;
    if (execution->parent_jit_context != nullptr)
    {
      const JitExecutionContext* parent = execution->parent_jit_context;
      if (parent->roots_staged)
      {
        append_jit_context_roots(parent, roots);
      }
      else
      {
        roots.insert(roots.end(),
                     parent->published_root_view.begin(),
                     parent->published_root_view.end());
      }
    }
    if (execution->append_outer_roots != nullptr)
    {
      execution->append_outer_roots(execution->outer_roots_context, roots);
    }
    roots.insert(roots.end(),
                 execution->base_roots.begin(),
                 execution->base_roots.end());
    for (const Value value : execution->extra_root_values)
    {
      if (value.kind() != ValueKind::reference)
        continue;
      const ObjectRef reference = value.reference_unchecked();
      if (!reference.is_null())
        roots.push_back(reference);
    }
    if (execution->compact_extra_root_values != nullptr)
    {
      execution->compact_extra_root_values->append_reference_roots(roots);
    }
    if (execution->roots_staged && execution->staged_frame_base != nullptr)
    {
      PerformanceCounters::observe_jit_staged_reference_slots(
          execution->staged_root_offsets.size(), true);
      const auto* frame_bytes = reinterpret_cast<const u8*>(
          execution->staged_frame_base);
      for (const u32 byte_offset : execution->staged_root_offsets)
      {
        const auto* slot = reinterpret_cast<const u64*>(
            frame_bytes + byte_offset);
        const ObjectRef reference{*slot};
        if (!reference.is_null())
          roots.push_back(reference);
      }
    }
    else for (const u64 bits : execution->frame_root_bits)
    {
      const ObjectRef reference{bits};
      if (!reference.is_null())
        roots.push_back(reference);
    }
    if (execution->pending_throwable.has_value() &&
        !execution->pending_throwable->is_null())
    {
      roots.push_back(*execution->pending_throwable);
    }
    if (materializing_staged)
    {
      PerformanceCounters::observe_jit_staged_root_materialization(
          roots.size() - roots_before);
    }
  }

  void Machine::commit_staged_jit_roots(
      JitExecutionContext* execution) noexcept
  {
    if (execution == nullptr || execution->machine != this ||
        !execution->roots_staged)
    {
      return;
    }
    PerformanceCounters::record_jit_root_stage_commit();
    execution->published_roots.clear();
    append_jit_context_roots(execution, execution->published_roots);
    execution->published_root_view = exchange_execution_roots(
        execution->invocation_depth, execution->published_roots);
    execution->roots_staged = false;
    execution->staged_frame_base = nullptr;
    execution->staged_root_offsets = {};
  }

  std::optional<u64> Machine::bounded_jit_invocation_cost(
      const ResolvedMethod& target,
      u32 recursion_depth)
  {
    constexpr u32 kMaximumBoundedCallDepth = 8U;
    if (recursion_depth >= kMaximumBoundedCallDepth ||
        target.method == nullptr)
    {
      return std::nullopt;
    }

    if (target.owner == nullptr)
      return std::nullopt;
    if (!target.method->code.has_value())
    {
      // Builtin default constructors are handled by invoke_native() as a
      // guaranteed no-op and consume no VM bytecode budget.
      if (target.method->name == "<init>" &&
          target.method->descriptor == "()V" &&
          is_builtin_class(target.owner->name()))
      {
        return 0U;
      }
      // Native calls are conservative by default. Individual implementations
      // may opt in only when they are synchronous/non-blocking and safe to
      // execute exactly once from a compiled runtime call.
      const NativeMethodBinding binding = resolve_native_binding(
          *target.owner,
          *target.method,
          target.runtime != nullptr ? target.runtime->id : MethodId {});
      if (binding.id.valid() &&
          natives_.jit_policy(binding.id) ==
              NativeJitPolicy::synchronous_bounded)
      {
        return 0U;
      }
      return std::nullopt;
    }
    const u16 disallowed_flags =
        static_cast<u16>(kAccNative | kAccAbstract | kAccSynchronized);
    if ((target.method->access_flags & disallowed_flags) != 0U ||
        target.runtime == nullptr || target.runtime->decoded == nullptr)
    {
      return std::nullopt;
    }

    const DecodedMethod& decoded = *target.runtime->decoded;
    // Methods with exception handlers participate: compiled callees dispatch
    // their own handlers through match_exception_handler, and an escaping
    // throwable returns java_throwable so the compiled caller's exception
    // stub selects its own handler. Loops, natives and synchronized methods
    // still make the cost unbounded and are rejected below.

    u64 maximum_cost = static_cast<u64>(decoded.instructions.size());
    for (usize instruction_index = 0U;
         instruction_index < decoded.instructions.size();
         ++instruction_index)
    {
      const DecodedInstruction& instruction =
          decoded.instructions[instruction_index];
      if (static_cast<u16>(instruction.opcode) >
          static_cast<u16>(DecodedOpcode::jvm_last))
      {
        return std::nullopt;
      }
      const u8 opcode = raw_opcode(instruction.opcode);
      const DecodedOperand* decoded_operand = nullptr;
      if (instruction.operand_index != kInvalidDecodedIndex)
      {
        if (instruction.operand_index >= decoded.operands.size())
          return std::nullopt;
        decoded_operand = &decoded.operands[instruction.operand_index];
      }

      const bool short_branch =
          (opcode >= 0x99U && opcode <= 0xA8U) ||
          opcode == 0xC6U || opcode == 0xC7U;
      const bool wide_branch = opcode == 0xC8U || opcode == 0xC9U;
      if (short_branch || wide_branch)
      {
        if (decoded_operand == nullptr ||
            decoded_operand->kind != DecodedOperandKind::branch_target ||
            decoded_operand->target_index == kInvalidDecodedIndex ||
            decoded_operand->target_index <= instruction_index)
        {
          return std::nullopt;
        }
        if (opcode == 0xA8U || opcode == 0xC9U)
          return std::nullopt;
      }
      else if (opcode == 0xAAU || opcode == 0xABU)
      {
        if (decoded_operand == nullptr ||
            decoded_operand->kind != DecodedOperandKind::switch_table ||
            decoded_operand->switch_index >= decoded.switches.size())
        {
          return std::nullopt;
        }
        const DecodedSwitchTable& table =
            decoded.switches[decoded_operand->switch_index];
        if (table.default_target_index == kInvalidDecodedIndex ||
            table.default_target_index <= instruction_index)
        {
          return std::nullopt;
        }
        for (const DecodedSwitchEntry& entry : table.entries)
        {
          if (entry.target_index == kInvalidDecodedIndex ||
              entry.target_index <= instruction_index)
          {
            return std::nullopt;
          }
        }
      }
      else if (opcode == 0xA9U ||
               (opcode == 0xC4U && decoded_operand != nullptr &&
                decoded_operand->kind == DecodedOperandKind::wide_local &&
                decoded_operand->modified_opcode == 0xA9U))
      {
        return std::nullopt;
      }

      if (opcode < 0xB6U || opcode > 0xBAU)
        continue;

      if (opcode == 0xBAU)
      {
        // A verified LambdaMetafactory callsite only allocates/captures a
        // lambda object in our runtime helper; it cannot execute Java bytecode
        // or block. The instruction itself is already included in maximum_cost.
        // Other bootstrap methods remain conservatively unbounded.
        if (decoded_operand == nullptr ||
            decoded_operand->kind != DecodedOperandKind::invokedynamic)
        {
          return std::nullopt;
        }
        auto dynamic = target.owner->invoke_dynamic_reference(
            decoded_operand->constant_pool_index);
        if (!dynamic)
          return std::nullopt;
        auto bootstrap = target.owner->bootstrap_method(
            dynamic->bootstrap_method_index);
        if (!bootstrap)
          return std::nullopt;
        auto bootstrap_handle = target.owner->method_handle_reference(
            (*bootstrap)->method_handle_index);
        if (!bootstrap_handle || bootstrap_handle->reference_kind != 6U ||
            bootstrap_handle->member.owner !=
                "java/lang/invoke/LambdaMetafactory" ||
            (bootstrap_handle->member.name != "metafactory" &&
             bootstrap_handle->member.name != "altMetafactory"))
        {
          return std::nullopt;
        }
        continue;
      }

      // invokevirtual/invokeinterface remain dynamically dispatched. Direct
      // invokestatic and invokespecial chains are safe to recurse through when
      // their target is itself acyclic and non-blocking.
      if (opcode == 0xB6U || opcode == 0xB9U ||
          decoded_operand == nullptr ||
          decoded_operand->kind != DecodedOperandKind::constant_pool_index)
      {
        return std::nullopt;
      }
      auto reference = target.owner->member_reference(
          decoded_operand->constant_pool_index);
      if (!reference ||
          (reference->kind != classfile::ConstantKind::method_ref &&
           reference->kind != classfile::ConstantKind::interface_method_ref))
      {
        return std::nullopt;
      }
      if (opcode == 0xB8U)
      {
        std::scoped_lock initialization_lock(class_initialization_mutex_);
        if (!initialized_classes_.contains(reference->owner))
          return std::nullopt;
      }
      auto nested_target = opcode == 0xB7U
          ? classes_.resolve_declared_method(reference->owner,
                                             reference->name,
                                             reference->descriptor)
          : classes_.resolve_method(reference->owner,
                                    reference->name,
                                    reference->descriptor);
      if (!nested_target || nested_target->method == nullptr)
        return std::nullopt;
      const bool nested_static =
          (nested_target->method->access_flags & kAccStatic) != 0U;
      if ((opcode == 0xB8U) != nested_static)
        return std::nullopt;
      auto nested_cost = bounded_jit_invocation_cost(
          *nested_target, recursion_depth + 1U);
      if (!nested_cost.has_value() ||
          *nested_cost > std::numeric_limits<u64>::max() - maximum_cost)
      {
        return std::nullopt;
      }
      maximum_cost += *nested_cost;
    }
    return maximum_cost;
  }

  std::optional<u64> Machine::safe_jit_instruction_budget(
      const ResolvedMethod& target,
      u64 requested_budget)
  {
    const u64 native_limit = BaselineJit::maximum_instruction_budget();
    if (requested_budget <= native_limit)
      return requested_budget;

    const auto bounded_cost = bounded_jit_invocation_cost(target, 0U);
    if (!bounded_cost.has_value() || *bounded_cost > native_limit)
      return std::nullopt;
    return native_limit;
  }

  u32 Machine::dispatch_jit_runtime(
      JitExecutionContext* parent_context,
      const classfile::ClassFile& owner,
      JitRuntimeOperation operation,
      u32 operand,
      u64 first,
      u64 second,
      u64 third,
      const u64* frame_base,
      u32* consumed_instructions,
      u64* result_bits) noexcept
  {
    if (consumed_instructions != nullptr)
      *consumed_instructions = 0U;
    if (result_bits == nullptr)
      return static_cast<u32>(JitRuntimeStatus::deoptimize);

    const auto commit_parent_roots = [this, parent_context]() noexcept {
      commit_staged_jit_roots(parent_context);
    };

    const auto encode_value = [result_bits](const Value& value) -> bool
    {
      switch (value.kind())
      {
      case ValueKind::int32:
      {
        auto integer = value.as_int();
        if (!integer)
          return false;
        *result_bits = static_cast<u64>(static_cast<u32>(*integer));
        return true;
      }
      case ValueKind::int64:
      {
        auto integer = value.as_long();
        if (!integer)
          return false;
        *result_bits = static_cast<u64>(*integer);
        return true;
      }
      case ValueKind::reference:
      {
        auto reference = value.as_reference();
        if (!reference)
          return false;
        *result_bits = reference->bits;
        return true;
      }
      case ValueKind::float32:
      {
        auto number = value.as_float();
        if (!number)
          return false;
        *result_bits = static_cast<u64>(std::bit_cast<u32>(*number));
        return true;
      }
      case ValueKind::float64:
      {
        auto number = value.as_double();
        if (!number)
          return false;
        *result_bits = std::bit_cast<u64>(*number);
        return true;
      }
      case ValueKind::empty:
      case ValueKind::continuation:
      case ValueKind::return_address:
        return false;
      }
      return false;
    };
    const auto decode_value = [](ValueKind kind,
                                 u64 bits) -> std::optional<Value>
    {
      switch (kind)
      {
      case ValueKind::int32:
        return Value::from_int(static_cast<i32>(static_cast<u32>(bits)));
      case ValueKind::int64:
        return Value::from_long(static_cast<i64>(bits));
      case ValueKind::float32:
        return Value::from_float(
            std::bit_cast<float>(static_cast<u32>(bits)));
      case ValueKind::float64:
        return Value::from_double(std::bit_cast<double>(bits));
      case ValueKind::reference:
        return Value::from_reference(ObjectRef{bits});
      case ValueKind::empty:
      case ValueKind::continuation:
      case ValueKind::return_address:
        return std::nullopt;
      }
      return std::nullopt;
    };

    struct JitCallOperands final
    {
      std::optional<ObjectRef> receiver;
      InvocationArguments arguments;

      explicit JitCallOperands(usize argument_count)
          : arguments(argument_count) {}
    };
    const auto decode_call_operands = [frame_base](
        const MethodDescriptor& descriptor,
        bool has_receiver) -> std::optional<JitCallOperands>
    {
      if (frame_base == nullptr)
        return std::nullopt;
      const auto* frame_bytes = reinterpret_cast<const u8*>(frame_base);
      u32 local_slots = 0U;
      u32 stack_depth = 0U;
      std::memcpy(&local_slots,
                  frame_bytes + kJitRuntimeLocalSlotsByteOffset,
                  sizeof(local_slots));
      std::memcpy(&stack_depth,
                  frame_bytes + kJitRuntimeStackDepthByteOffset,
                  sizeof(stack_depth));
      const usize consumed_slots = descriptor.parameter_slots(has_receiver);
      if (consumed_slots > stack_depth)
        return std::nullopt;
      PerformanceCounters::observe_jit_call_operands(
          descriptor.parameters.size(),
          descriptor.parameters.size() > kInlineInvocationArgumentCapacity);
      const auto* slots = reinterpret_cast<const u64*>(
          frame_bytes + kJitRuntimeFrameHeaderBytes);
      usize cursor = static_cast<usize>(local_slots) +
                     static_cast<usize>(stack_depth) - consumed_slots;

      JitCallOperands operands(descriptor.parameters.size());
      if (has_receiver)
      {
        operands.receiver = ObjectRef{slots[cursor]};
        ++cursor;
      }
      usize argument_index = 0U;
      for (const TypeDescriptor& parameter : descriptor.parameters)
      {
        const u64 bits = slots[cursor];
        switch (parameter.kind)
        {
        case JavaTypeKind::boolean:
        case JavaTypeKind::byte:
        case JavaTypeKind::character:
        case JavaTypeKind::short_integer:
        case JavaTypeKind::integer:
          operands.arguments.set_compact(
              argument_index, ValueKind::int32, bits);
          break;
        case JavaTypeKind::long_integer:
          operands.arguments.set_compact(
              argument_index, ValueKind::int64, bits);
          break;
        case JavaTypeKind::float32:
          operands.arguments.set_compact(
              argument_index, ValueKind::float32, bits);
          break;
        case JavaTypeKind::float64:
          operands.arguments.set_compact(
              argument_index, ValueKind::float64, bits);
          break;
        case JavaTypeKind::reference:
        case JavaTypeKind::array:
          operands.arguments.set_compact(
              argument_index, ValueKind::reference, bits);
          break;
        case JavaTypeKind::void_type:
          return std::nullopt;
        }
        ++argument_index;
        cursor += parameter.slot_count();
      }
      return operands;
    };

    const auto java_to_int = [](double value) noexcept -> i32
    {
      if (std::isnan(value))
        return 0;
      if (value >= static_cast<double>(std::numeric_limits<i32>::max()))
        return std::numeric_limits<i32>::max();
      if (value <= static_cast<double>(std::numeric_limits<i32>::min()))
        return std::numeric_limits<i32>::min();
      return static_cast<i32>(value);
    };
    const auto java_to_long = [](double value) noexcept -> i64
    {
      if (std::isnan(value))
        return 0;
      if (value >= static_cast<double>(std::numeric_limits<i64>::max()))
        return std::numeric_limits<i64>::max();
      if (value <= static_cast<double>(std::numeric_limits<i64>::min()))
        return std::numeric_limits<i64>::min();
      return static_cast<i64>(value);
    };
    const auto compare_float = [](float left,
                                  float right,
                                  i32 nan_result) noexcept -> i32
    {
      if (std::isnan(left) || std::isnan(right))
        return nan_result;
      if (left < right)
        return -1;
      if (left > right)
        return 1;
      return 0;
    };
    const auto compare_double = [](double left,
                                   double right,
                                   i32 nan_result) noexcept -> i32
    {
      if (std::isnan(left) || std::isnan(right))
        return nan_result;
      if (left < right)
        return -1;
      if (left > right)
        return 1;
      return 0;
    };

    switch (operation)
    {
    case JitRuntimeOperation::match_exception_handler:
      // The trampoline handles this operation because it needs the current
      // compiled method's exception table. Reaching the generic dispatcher is
      // therefore a safe deoptimization request.
      return static_cast<u32>(JitRuntimeStatus::deoptimize);
    case JitRuntimeOperation::budget_safepoint:
      // Handled in jit_runtime_dispatch_callback; the generic dispatcher
      // never sees it. Treat an accidental arrival as a resumable deopt.
      return static_cast<u32>(JitRuntimeStatus::deoptimize);
    case JitRuntimeOperation::monitor_enter:
    {
      const ObjectRef monitor{first};
      if (monitor.is_null())
        return static_cast<u32>(JitRuntimeStatus::null_pointer);
      auto entered = enter_monitor(monitor);
      if (entered)
        return static_cast<u32>(JitRuntimeStatus::success);
      if (entered.error().code == ErrorCode::java_exception &&
          !entered.error().java_exception_class.empty())
      {
        auto throwable = create_throwable(
            entered.error().java_exception_class,
            entered.error().message);
        if (!throwable)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        *result_bits = throwable->bits;
        return static_cast<u32>(JitRuntimeStatus::java_throwable);
      }
      return static_cast<u32>(JitRuntimeStatus::deoptimize);
    }
    case JitRuntimeOperation::monitor_exit:
    {
      const ObjectRef monitor{first};
      if (monitor.is_null())
        return static_cast<u32>(JitRuntimeStatus::null_pointer);
      auto exited = monitors_.exit(monitor, scheduler_.current_thread_id());
      if (exited)
        return static_cast<u32>(JitRuntimeStatus::success);
      if (exited.error().code == ErrorCode::java_exception &&
          !exited.error().java_exception_class.empty())
      {
        auto throwable = create_throwable(
            exited.error().java_exception_class,
            exited.error().message);
        if (!throwable)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        *result_bits = throwable->bits;
        return static_cast<u32>(JitRuntimeStatus::java_throwable);
      }
      return static_cast<u32>(JitRuntimeStatus::deoptimize);
    }
    case JitRuntimeOperation::arithmetic_exception:
    {
      auto throwable = create_throwable("java/lang/ArithmeticException");
      if (!throwable)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      *result_bits = throwable->bits;
      return static_cast<u32>(JitRuntimeStatus::java_throwable);
    }
    case JitRuntimeOperation::throw_object:
    {
      ObjectRef throwable{first};
      if (throwable.is_null())
      {
        auto null_throwable = create_throwable(
            "java/lang/NullPointerException");
        if (!null_throwable)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        throwable = *null_throwable;
      }
      else
      {
        auto throwable_class = heap_.vm_class_name_view(throwable);
        if (!throwable_class)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        auto assignable = classes_.is_assignable(*throwable_class,
                                                 "java/lang/Throwable");
        if (!assignable || !*assignable)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      *result_bits = throwable.bits;
      return static_cast<u32>(JitRuntimeStatus::java_throwable);
    }
    case JitRuntimeOperation::load_constant:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u16>::max()))
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      auto value = load_constant(owner, static_cast<u16>(operand), false);
      if (!value)
      {
        if (value.error().code == ErrorCode::java_exception &&
            !value.error().java_exception_class.empty())
        {
          auto throwable = create_throwable(
              value.error().java_exception_class,
              value.error().message);
          if (!throwable)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          *result_bits = throwable->bits;
          return static_cast<u32>(JitRuntimeStatus::java_throwable);
        }
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      return encode_value(*value)
          ? static_cast<u32>(JitRuntimeStatus::success)
          : static_cast<u32>(JitRuntimeStatus::deoptimize);
    }
    case JitRuntimeOperation::get_static:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u16>::max()))
        return 1U;
      auto binding = field_binding_slot(owner, static_cast<u16>(operand));
      if (!binding)
        return 1U;
      if (!(**binding).has_value())
      {
        auto reference = owner.member_reference(static_cast<u16>(operand));
        if (!reference ||
            reference->kind != classfile::ConstantKind::field_ref)
          return 1U;
        auto resolved = resolve_quick_field_binding(*reference, true);
        if (!resolved)
          return 1U;
        **binding = std::move(*resolved);
      }
      const QuickFieldBinding& field = (**binding).value();
      if (!field.is_static)
        return 1U;
      if (field.declaring_runtime_class == nullptr)
        return 1U;
      {
        std::scoped_lock initialization_lock(class_initialization_mutex_);
        if (!initialized_classes_.contains(
                field.declaring_runtime_class->class_file->name()))
          return 1U;
      }
      auto value = states_.static_field(field.id);
      return value && encode_value(*value) ? 0U : 1U;
    }
    case JitRuntimeOperation::put_static:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u16>::max()))
        return 1U;
      auto binding = field_binding_slot(owner, static_cast<u16>(operand));
      if (!binding)
        return 1U;
      if (!(**binding).has_value())
      {
        auto reference = owner.member_reference(static_cast<u16>(operand));
        if (!reference ||
            reference->kind != classfile::ConstantKind::field_ref)
          return 1U;
        auto resolved = resolve_quick_field_binding(*reference, true);
        if (!resolved)
          return 1U;
        **binding = std::move(*resolved);
      }
      const QuickFieldBinding& field = (**binding).value();
      if (!field.is_static)
        return 1U;
      if (field.declaring_runtime_class == nullptr)
        return 1U;
      {
        std::scoped_lock initialization_lock(class_initialization_mutex_);
        if (!initialized_classes_.contains(
                field.declaring_runtime_class->class_file->name()))
          return 1U;
      }
      auto value = decode_value(field.value_kind, first);
      if (!value)
        return 1U;
      auto stored = states_.set_static_field(field.id, field.value_kind, *value);
      return stored ? 0U : 1U;
    }
    case JitRuntimeOperation::get_field:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u16>::max()))
        return 1U;
      const ObjectRef object{first};
      if (object.is_null())
        return static_cast<u32>(JitRuntimeStatus::null_pointer);

      auto binding = field_binding_slot(owner, static_cast<u16>(operand));
      if (!binding)
        return 1U;
      if (!(**binding).has_value())
      {
        auto reference = owner.member_reference(static_cast<u16>(operand));
        if (!reference ||
            reference->kind != classfile::ConstantKind::field_ref)
          return 1U;
        auto resolved = resolve_quick_field_binding(*reference, false);
        if (!resolved)
          return 1U;
        **binding = std::move(*resolved);
      }
      const QuickFieldBinding& field = (**binding).value();
      if (field.is_static)
        return 1U;

      auto value = heap_.vm_field_typed(
          object, field.index, field.value_kind);
      return value && encode_value(*value) ? 0U : 1U;
    }
    case JitRuntimeOperation::put_field:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u16>::max()))
        return 1U;
      const ObjectRef object{first};
      if (object.is_null())
        return static_cast<u32>(JitRuntimeStatus::null_pointer);

      auto binding = field_binding_slot(owner, static_cast<u16>(operand));
      if (!binding)
        return 1U;
      if (!(**binding).has_value())
      {
        auto reference = owner.member_reference(static_cast<u16>(operand));
        if (!reference ||
            reference->kind != classfile::ConstantKind::field_ref)
          return 1U;
        auto resolved = resolve_quick_field_binding(*reference, false);
        if (!resolved)
          return 1U;
        **binding = std::move(*resolved);
      }
      const QuickFieldBinding& field = (**binding).value();
      if (field.is_static)
        return 1U;
      auto value = decode_value(field.value_kind, second);
      if (!value)
        return 1U;
      auto stored = heap_.vm_set_field_typed(
          object, field.index, field.value_kind, *value);
      return stored ? 0U : 1U;
    }
    case JitRuntimeOperation::array_load:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u8>::max()))
        return 1U;
      const u8 opcode = static_cast<u8>(operand);
      const ObjectRef array{first};
      const i32 index = static_cast<i32>(static_cast<u32>(second));
      if (array.is_null())
        return static_cast<u32>(JitRuntimeStatus::null_pointer);
      if (index < 0)
        return static_cast<u32>(
            JitRuntimeStatus::array_index_out_of_bounds);

      auto snapshot = heap_.vm_array_element_snapshot(
          array, static_cast<usize>(index));
      if (!snapshot)
      {
        return snapshot.error().code == ErrorCode::out_of_range
            ? static_cast<u32>(JitRuntimeStatus::array_index_out_of_bounds)
            : static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      if (opcode == 0x33U)
      {
        if (snapshot->kind != HeapArrayKind::boolean &&
            snapshot->kind != HeapArrayKind::byte)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      else
      {
        const auto expected = array_load_heap_kind(opcode);
        if (!expected.has_value() || snapshot->kind != *expected)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }

      Value value = snapshot->value;
      if (opcode == 0x33U || opcode == 0x34U || opcode == 0x35U)
      {
        auto integer = value.as_int();
        if (!integer)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        if (opcode == 0x33U)
        {
          value = snapshot->kind == HeapArrayKind::boolean
              ? Value::from_int(*integer == 0 ? 0 : 1)
              : Value::from_int(
                    static_cast<i32>(static_cast<i8>(*integer)));
        }
        else if (opcode == 0x34U)
        {
          value = Value::from_int(
              static_cast<i32>(static_cast<u16>(*integer)));
        }
        else
        {
          value = Value::from_int(
              static_cast<i32>(static_cast<i16>(*integer)));
        }
      }
      return encode_value(value)
          ? static_cast<u32>(JitRuntimeStatus::success)
          : static_cast<u32>(JitRuntimeStatus::deoptimize);
    }
    case JitRuntimeOperation::array_payload_lease:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u8>::max()))
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      const u8 opcode = static_cast<u8>(operand);
      const ObjectRef array{first};
      if (array.is_null())
        return static_cast<u32>(JitRuntimeStatus::null_pointer);

      std::optional<HeapArrayKind> expected = array_load_heap_kind(opcode);
      if (opcode == 0x33U)
      {
        auto info = heap_.vm_array_info(array);
        if (!info || (info->kind != HeapArrayKind::boolean &&
                      info->kind != HeapArrayKind::byte))
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        expected = info->kind;
      }
      if (!expected.has_value())
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      auto lease = heap_.array_payload_lease(array, *expected);
      if (!lease)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      *result_bits = reinterpret_cast<u64>(lease->first_payload);
      return static_cast<u32>(JitRuntimeStatus::success);
    }
    case JitRuntimeOperation::array_store:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u8>::max()))
        return 1U;
      const u8 opcode = static_cast<u8>(operand);
      const ObjectRef array{first};
      const i32 index = static_cast<i32>(static_cast<u32>(second));
      if (array.is_null())
        return static_cast<u32>(JitRuntimeStatus::null_pointer);
      if (index < 0)
        return static_cast<u32>(
            JitRuntimeStatus::array_index_out_of_bounds);

      ValueKind value_kind = ValueKind::int32;
      switch (opcode)
      {
      case 0x4FU:
      case 0x54U:
      case 0x55U:
      case 0x56U:
        value_kind = ValueKind::int32;
        break;
      case 0x50U:
        value_kind = ValueKind::int64;
        break;
      case 0x51U:
        value_kind = ValueKind::float32;
        break;
      case 0x52U:
        value_kind = ValueKind::float64;
        break;
      case 0x53U:
        value_kind = ValueKind::reference;
        break;
      default:
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      auto decoded = decode_value(value_kind, third);
      if (!decoded)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      Value value = *decoded;
      HeapArrayKind actual_kind = HeapArrayKind::integer;

      if (opcode == 0x54U || opcode == 0x53U)
      {
        auto info = heap_.vm_array_info(array);
        if (!info)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        if (static_cast<usize>(index) >= info->length)
          return static_cast<u32>(
              JitRuntimeStatus::array_index_out_of_bounds);
        actual_kind = info->kind;

        if (opcode == 0x54U)
        {
          if (actual_kind != HeapArrayKind::boolean &&
              actual_kind != HeapArrayKind::byte)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          auto integer = value.as_int();
          if (!integer)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          value = actual_kind == HeapArrayKind::boolean
              ? Value::from_int((*integer & 1) == 0 ? 0 : 1)
              : Value::from_int(
                    static_cast<i32>(static_cast<i8>(*integer)));
        }
        else
        {
          if (actual_kind != HeapArrayKind::reference)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          auto stored_reference = value.as_reference();
          if (!stored_reference)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          if (!stored_reference->is_null())
          {
            auto source_class = heap_.vm_class_name_view(*stored_reference);
            if (!source_class)
              return static_cast<u32>(JitRuntimeStatus::deoptimize);
            auto assignable = classes_.is_assignable(
                *source_class, info->reference_component);
            if (!assignable)
              return static_cast<u32>(JitRuntimeStatus::deoptimize);
            if (!*assignable)
              return static_cast<u32>(JitRuntimeStatus::array_store);
          }
        }
      }
      else
      {
        const auto expected = array_store_heap_kind(opcode);
        if (!expected.has_value())
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        actual_kind = *expected;
        if (opcode == 0x55U || opcode == 0x56U)
        {
          auto integer = value.as_int();
          if (!integer)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          value = opcode == 0x55U
              ? Value::from_int(
                    static_cast<i32>(static_cast<u16>(*integer)))
              : Value::from_int(
                    static_cast<i32>(static_cast<i16>(*integer)));
        }
      }

      auto stored = heap_.vm_set_element_checked(
          array,
          static_cast<usize>(index),
          actual_kind,
          value);
      if (!stored)
      {
        return stored.error().code == ErrorCode::out_of_range
            ? static_cast<u32>(JitRuntimeStatus::array_index_out_of_bounds)
            : static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      return static_cast<u32>(JitRuntimeStatus::success);
    }
    case JitRuntimeOperation::array_length:
    {
      const ObjectRef array{first};
      if (array.is_null())
        return static_cast<u32>(JitRuntimeStatus::null_pointer);
      auto length = heap_.vm_array_length(array);
      if (!length || *length >
              static_cast<usize>(std::numeric_limits<i32>::max()))
        return 1U;
      *result_bits = static_cast<u64>(static_cast<u32>(*length));
      return 0U;
    }
    case JitRuntimeOperation::new_multi_array:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u16>::max()) ||
          first == 0U ||
          first > static_cast<u64>(std::numeric_limits<u8>::max()) ||
          frame_base == nullptr)
      {
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      const usize dimensions = static_cast<usize>(first);
      const auto* frame_bytes = reinterpret_cast<const u8*>(frame_base);
      u32 local_slots = 0U;
      u32 stack_depth = 0U;
      std::memcpy(&local_slots,
                  frame_bytes + kJitRuntimeLocalSlotsByteOffset,
                  sizeof(local_slots));
      std::memcpy(&stack_depth,
                  frame_bytes + kJitRuntimeStackDepthByteOffset,
                  sizeof(stack_depth));
      if (static_cast<usize>(stack_depth) < dimensions)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);

      auto array_class = owner.class_name_constant(static_cast<u16>(operand));
      if (!array_class || !array_class->starts_with('['))
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      auto loaded_array_class = load_linkage_class(*array_class);
      if (!loaded_array_class)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      usize declared_dimensions = 0U;
      while (declared_dimensions < array_class->size() &&
             (*array_class)[declared_dimensions] == '[')
      {
        ++declared_dimensions;
      }
      if (dimensions > declared_dimensions)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);

      std::vector<i32> lengths(dimensions);
      const usize first_dimension_slot =
          static_cast<usize>(stack_depth) - dimensions;
      const auto* stack_base = frame_bytes + kJitRuntimeFrameHeaderBytes +
          static_cast<usize>(local_slots) * 8U;
      for (usize index = 0U; index < dimensions; ++index)
      {
        u64 bits = 0U;
        std::memcpy(&bits,
                    stack_base + (first_dimension_slot + index) * 8U,
                    sizeof(bits));
        lengths[index] = static_cast<i32>(static_cast<u32>(bits));
        if (lengths[index] < 0)
        {
          auto throwable = create_throwable(
              "java/lang/NegativeArraySizeException");
          if (!throwable)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          *result_bits = throwable->bits;
          return static_cast<u32>(JitRuntimeStatus::java_throwable);
        }
      }

      usize arrays_at_level = 1U;
      usize total_arrays = 0U;
      for (usize level = 0U; level < dimensions; ++level)
      {
        auto updated_total = checked_add(total_arrays, arrays_at_level);
        if (!updated_total || *updated_total > 1'000'000U)
        {
          auto throwable = create_throwable("java/lang/OutOfMemoryError");
          if (!throwable)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          *result_bits = throwable->bits;
          return static_cast<u32>(JitRuntimeStatus::java_throwable);
        }
        total_arrays = *updated_total;
        if (level + 1U < dimensions)
        {
          auto next_count = checked_multiply(
              arrays_at_level,
              static_cast<usize>(lengths[level]));
          if (!next_count || *next_count > 1'000'000U)
          {
            auto throwable = create_throwable("java/lang/OutOfMemoryError");
            if (!throwable)
              return static_cast<u32>(JitRuntimeStatus::deoptimize);
            *result_bits = throwable->bits;
            return static_cast<u32>(JitRuntimeStatus::java_throwable);
          }
          arrays_at_level = *next_count;
        }
      }

      const auto allocate_array = [this](std::string_view descriptor,
                                         usize length,
                                         Value initial) {
        auto array = heap_.vm_allocate_array(descriptor, length, initial);
        if (!array && array.error().code == ErrorCode::overflow)
        {
          auto collected = collect_garbage();
          if (!collected)
            return Result<ObjectRef>(std::unexpected(collected.error()));
          array = heap_.vm_allocate_array(descriptor, length, initial);
        }
        return array;
      };

      auto root_default = array_default_value(*array_class);
      if (!root_default)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      auto root = allocate_array(*array_class,
                                 static_cast<usize>(lengths.front()),
                                 *root_default);
      if (!root)
      {
        if (root.error().code != ErrorCode::overflow)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        auto throwable = create_throwable("java/lang/OutOfMemoryError");
        if (!throwable)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        *result_bits = throwable->bits;
        return static_cast<u32>(JitRuntimeStatus::java_throwable);
      }
      auto pinned_root = NativeRootScope::pin(native_roots_, *root);
      if (!pinned_root)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);

      struct PendingArray final
      {
        ObjectRef reference;
        usize level{0U};
      };
      std::vector<PendingArray> pending;
      pending.reserve(total_arrays);
      pending.push_back(PendingArray{.reference = *root, .level = 0U});
      for (usize cursor = 0U; cursor < pending.size(); ++cursor)
      {
        const PendingArray current = pending[cursor];
        const usize child_level = current.level + 1U;
        if (child_level >= dimensions)
          continue;
        const std::string_view child_descriptor =
            std::string_view(*array_class).substr(child_level);
        auto child_default = array_default_value(child_descriptor);
        if (!child_default)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        const usize child_count =
            static_cast<usize>(lengths[child_level]);
        const usize parent_length =
            static_cast<usize>(lengths[current.level]);
        for (usize element = 0U; element < parent_length; ++element)
        {
          auto child = allocate_array(child_descriptor,
                                      child_count,
                                      *child_default);
          if (!child)
          {
            if (child.error().code != ErrorCode::overflow)
              return static_cast<u32>(JitRuntimeStatus::deoptimize);
            auto throwable = create_throwable("java/lang/OutOfMemoryError");
            if (!throwable)
              return static_cast<u32>(JitRuntimeStatus::deoptimize);
            *result_bits = throwable->bits;
            return static_cast<u32>(JitRuntimeStatus::java_throwable);
          }
          auto stored = heap_.vm_set_element(current.reference,
                                             element,
                                             Value::from_reference(*child));
          if (!stored)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          pending.push_back(PendingArray{
              .reference = *child,
              .level = child_level,
          });
        }
      }
      *result_bits = root->bits;
      return static_cast<u32>(JitRuntimeStatus::success);
    }
    case JitRuntimeOperation::new_object:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u16>::max()))
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      auto class_name = owner.class_name_constant(static_cast<u16>(operand));
      if (!class_name || class_name->starts_with('['))
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      auto allocated_class = load_linkage_class(*class_name);
      if (!allocated_class)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      if (((*allocated_class)->access_flags() &
           (kAccInterface | kAccAbstract)) != 0U)
      {
        auto throwable = create_throwable("java/lang/InstantiationError");
        if (!throwable)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        *result_bits = throwable->bits;
        return static_cast<u32>(JitRuntimeStatus::java_throwable);
      }

      bool initialized = false;
      {
        std::scoped_lock initialization_lock(class_initialization_mutex_);
        initialized = initialized_classes_.contains(*class_name);
        if (!initialized)
        {
          const auto current_initializer =
              initializing_class_owners_.find(*class_name);
          initialized = current_initializer != initializing_class_owners_.end() &&
              current_initializer->second == scheduler_.current_thread_id();
        }
      }
      if (!initialized)
      {
        // Class initialization can execute arbitrary Java and consume a dynamic
        // instruction budget. Deopt before allocation on the first encounter;
        // the interpreter initializes the class, then later executions stay in
        // native code without replaying an allocated object.
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }

      auto allocate = [&]() -> Result<ObjectRef> {
        auto class_layout = states_.layout(*class_name);
        if (!class_layout)
          return std::unexpected(class_layout.error());
        if (!(*class_layout)->instantiable)
          return fail(ErrorCode::invalid_argument,
                      "cannot allocate an interface or abstract class");
        return heap_.vm_allocate_object(
            (*class_layout)->class_name,
            (*class_layout)->instance_defaults);
      };
      auto object = allocate();
      if (!object && object.error().code == ErrorCode::overflow)
      {
        auto collected = collect_garbage();
        if (!collected)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        object = allocate();
      }
      if (!object)
      {
        if (object.error().code != ErrorCode::overflow)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        auto throwable = create_throwable("java/lang/OutOfMemoryError");
        if (!throwable)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        *result_bits = throwable->bits;
        return static_cast<u32>(JitRuntimeStatus::java_throwable);
      }
      *result_bits = object->bits;
      return static_cast<u32>(JitRuntimeStatus::success);
    }
    case JitRuntimeOperation::new_primitive_array:
    case JitRuntimeOperation::new_reference_array:
    {
      const i32 count = static_cast<i32>(static_cast<u32>(first));
      if (count < 0)
      {
        auto throwable = create_throwable(
            "java/lang/NegativeArraySizeException");
        if (!throwable)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        *result_bits = throwable->bits;
        return static_cast<u32>(JitRuntimeStatus::java_throwable);
      }

      std::string array_name;
      Value initial_value;
      if (operation == JitRuntimeOperation::new_primitive_array)
      {
        if (operand > static_cast<u32>(std::numeric_limits<u8>::max()))
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        auto type = primitive_array_type(static_cast<u8>(operand));
        if (!type)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        array_name = std::move(type->first);
        initial_value = type->second;
      }
      else
      {
        if (operand > static_cast<u32>(std::numeric_limits<u16>::max()))
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        auto component = owner.class_name_constant(static_cast<u16>(operand));
        if (!component)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        auto loaded_component = load_linkage_class(*component);
        if (!loaded_component)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        array_name = component->starts_with('[')
            ? '[' + *component
            : "[L" + *component + ';';
        initial_value = Value::from_reference({});
      }

      auto allocate = [&]() {
        return heap_.vm_allocate_array(array_name,
                                       static_cast<usize>(count),
                                       initial_value);
      };
      auto array = allocate();
      if (!array && array.error().code == ErrorCode::overflow)
      {
        auto collected = collect_garbage();
        if (!collected)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        array = allocate();
      }
      if (!array)
      {
        if (array.error().code != ErrorCode::overflow)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        auto throwable = create_throwable("java/lang/OutOfMemoryError");
        if (!throwable)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        *result_bits = throwable->bits;
        return static_cast<u32>(JitRuntimeStatus::java_throwable);
      }
      *result_bits = array->bits;
      return static_cast<u32>(JitRuntimeStatus::success);
    }
    case JitRuntimeOperation::invoke_dynamic:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u16>::max()))
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      auto dynamic = owner.invoke_dynamic_reference(static_cast<u16>(operand));
      if (!dynamic)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      auto call_site = parse_method_descriptor(dynamic->descriptor);
      if (!call_site)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      auto captures = decode_call_operands(*call_site, false);
      if (!captures)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);

      auto binding = resolve_lambda_binding(owner, static_cast<u16>(operand));
      if (!binding)
      {
        if (binding.error().code == ErrorCode::unsupported_feature)
        {
          auto throwable = create_throwable("java/lang/BootstrapMethodError");
          if (!throwable)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          *result_bits = throwable->bits;
          return static_cast<u32>(JitRuntimeStatus::java_throwable);
        }
        if (binding.error().code == ErrorCode::java_exception &&
            !binding.error().java_exception_class.empty())
        {
          auto throwable = create_throwable(
              binding.error().java_exception_class,
              binding.error().message);
          if (!throwable)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          *result_bits = throwable->bits;
          return static_cast<u32>(JitRuntimeStatus::java_throwable);
        }
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      if (binding->captured_count != captures->arguments.size())
        return static_cast<u32>(JitRuntimeStatus::deoptimize);

      const auto allocate = [&]() {
        return heap_.allocate_object(binding->interface_name,
                                     captures->arguments.size());
      };
      auto lambda = allocate();
      if (!lambda && lambda.error().code == ErrorCode::overflow)
      {
        auto collected = collect_garbage();
        if (!collected)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        lambda = allocate();
      }
      if (!lambda)
      {
        if (lambda.error().code != ErrorCode::overflow)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        auto throwable = create_throwable("java/lang/OutOfMemoryError");
        if (!throwable)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        *result_bits = throwable->bits;
        return static_cast<u32>(JitRuntimeStatus::java_throwable);
      }
      for (usize capture_index = 0U;
           capture_index < captures->arguments.size();
           ++capture_index)
      {
        auto stored = heap_.vm_set_field(*lambda,
                                         capture_index,
                                         captures->arguments[capture_index]);
        if (!stored)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      lambda_bindings_.insert_or_assign(lambda->bits, std::move(*binding));
      *result_bits = lambda->bits;
      return static_cast<u32>(JitRuntimeStatus::success);
    }
    case JitRuntimeOperation::check_cast:
    case JitRuntimeOperation::instance_of:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u16>::max()))
        return 1U;
      const ObjectRef object{first};
      if (object.is_null())
      {
        *result_bits = operation == JitRuntimeOperation::check_cast
            ? first
            : 0U;
        return 0U;
      }
      auto target_class = owner.class_name_constant(static_cast<u16>(operand));
      if (!target_class)
        return 1U;
      auto loaded_target = load_linkage_class(*target_class);
      if (!loaded_target)
        return 1U;
      auto source_class = heap_.vm_class_name_view(object);
      if (!source_class)
        return 1U;
      auto assignable = classes_.is_assignable(*source_class, *target_class);
      if (!assignable)
        return 1U;
      bool compatible = *assignable;
      if (!compatible)
      {
        const auto lambda = lambda_bindings_.find(object.bits);
        if (lambda != lambda_bindings_.end())
        {
          compatible = std::find(
                           lambda->second.marker_interfaces.begin(),
                           lambda->second.marker_interfaces.end(),
                           *target_class) !=
                       lambda->second.marker_interfaces.end();
        }
      }
      if (operation == JitRuntimeOperation::check_cast)
      {
        if (!compatible)
          return static_cast<u32>(JitRuntimeStatus::class_cast);
        *result_bits = first;
      }
      else
      {
        *result_bits = compatible ? 1U : 0U;
      }
      return 0U;
    }
    case JitRuntimeOperation::invoke_virtual:
    case JitRuntimeOperation::invoke_special:
    case JitRuntimeOperation::invoke_static:
    case JitRuntimeOperation::invoke_interface:
    {
      if (operand > static_cast<u32>(std::numeric_limits<u16>::max()) ||
          first == 0U)
      {
        return first == 0U
            ? static_cast<u32>(JitRuntimeStatus::budget_exhausted)
            : static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      refresh_metadata_bindings_if_needed();
      auto site = invoke_site_binding(owner, static_cast<u16>(operand));
      if (!site)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      const classfile::MemberReference* reference = &(*site)->reference;
      if ((reference->kind != classfile::ConstantKind::method_ref &&
           reference->kind !=
               classfile::ConstantKind::interface_method_ref))
      {
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      if (operation == JitRuntimeOperation::invoke_interface &&
          reference->kind != classfile::ConstantKind::interface_method_ref)
      {
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      const MethodDescriptor& descriptor = (*site)->descriptor->descriptor;
      const bool has_receiver =
          operation != JitRuntimeOperation::invoke_static;
      auto operands = decode_call_operands(descriptor, has_receiver);
      if (!operands)
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      if (has_receiver &&
          (!operands->receiver.has_value() ||
           operands->receiver->is_null()))
      {
        return static_cast<u32>(JitRuntimeStatus::null_pointer);
      }

      const u64 nested_budget = first;

      if (operation == JitRuntimeOperation::invoke_static &&
          reference->owner == "java/lang/String" &&
          reference->name == "valueOf" &&
          reference->descriptor == "(I)Ljava/lang/String;" &&
          operands->arguments.size() == 1U)
      {
        auto value = operands->arguments[0U].as_int();
        if (!value)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        auto text = format_java_int(*value);
        if (!text)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        auto result = states_.allocate_text_instance(
            heap_, "java/lang/String", std::move(*text));
        if (!result)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        *result_bits = result->bits;
        return static_cast<u32>(JitRuntimeStatus::success);
      }

      // Bitmap-font renderers in classic MIDP games frequently execute
      // String.length()/charAt()/indexOf(int) for every glyph. Resolve these
      // immutable String accessors directly against the VM heap so compiled
      // render loops neither deopt at each character nor copy the complete
      // UTF-16 payload through the public native API.
      if (operation == JitRuntimeOperation::invoke_virtual &&
          operands->receiver.has_value() &&
          reference->owner == "java/lang/String")
      {
        const ObjectRef string = *operands->receiver;
        if (reference->name == "length" &&
            reference->descriptor == "()I")
        {
          auto length = heap_.vm_string_length(string);
          if (!length ||
              *length > static_cast<usize>(std::numeric_limits<i32>::max()))
          {
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          }
          *result_bits = static_cast<u64>(static_cast<u32>(
              static_cast<i32>(*length)));
          return static_cast<u32>(JitRuntimeStatus::success);
        }
        if (reference->name == "charAt" &&
            reference->descriptor == "(I)C" &&
            operands->arguments.size() == 1U)
        {
          auto index = operands->arguments[0U].as_int();
          if (!index)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          if (*index < 0)
          {
            auto throwable = create_throwable(
                "java/lang/StringIndexOutOfBoundsException",
                "String.charAt index is out of range");
            if (!throwable)
              return static_cast<u32>(JitRuntimeStatus::deoptimize);
            *result_bits = throwable->bits;
            return static_cast<u32>(JitRuntimeStatus::java_throwable);
          }
          auto character = heap_.vm_string_character(
              string, static_cast<usize>(*index));
          if (!character)
          {
            if (character.error().code == ErrorCode::out_of_range)
            {
              auto throwable = create_throwable(
                  "java/lang/StringIndexOutOfBoundsException",
                  "String.charAt index is out of range");
              if (!throwable)
                return static_cast<u32>(JitRuntimeStatus::deoptimize);
              *result_bits = throwable->bits;
              return static_cast<u32>(JitRuntimeStatus::java_throwable);
            }
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          }
          *result_bits = static_cast<u64>(static_cast<u32>(*character));
          return static_cast<u32>(JitRuntimeStatus::success);
        }
        if (reference->name == "indexOf" &&
            reference->descriptor == "(I)I" &&
            operands->arguments.size() == 1U)
        {
          auto character = operands->arguments[0U].as_int();
          if (!character)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          auto position = heap_.vm_string_index_of(
              string, static_cast<u16>(*character));
          if (!position)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          *result_bits = static_cast<u64>(static_cast<u32>(*position));
          return static_cast<u32>(JitRuntimeStatus::success);
        }
        if (reference->name == "equals" &&
            reference->descriptor == "(Ljava/lang/Object;)Z" &&
            operands->arguments.size() == 1U &&
            operands->arguments.kind(0U) == ValueKind::reference)
        {
          const ObjectRef other =
              operands->arguments.reference_unchecked(0U);
          auto equal = heap_.vm_string_equals(string, other);
          if (!equal)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          *result_bits = *equal ? 1U : 0U;
          return static_cast<u32>(JitRuntimeStatus::success);
        }
      }

      // Vector accessors sit in some of the hottest MIDP collision/render
      // loops. Going through full virtual resolution + NativeMethodRegistry for
      // size()/elementAt() can cost more than the actual Java loop when called
      // hundreds of thousands of times from JIT code. These are exact fast
      // paths for the built-in Vector layout; they preserve null/bounds errors
      // and do not execute Java bytecode, so nested budget consumption is zero.
      if (operation == JitRuntimeOperation::invoke_virtual &&
          operands->receiver.has_value() &&
          reference->owner == "java/util/Vector")
      {
        const ObjectRef vector = *operands->receiver;
        const auto java_exception = [this, result_bits](
            std::string_view class_name,
            std::string_view message) -> u32
        {
          // The caller commits staged roots before entering this helper on the
          // exceptional Vector path; keep allocation itself narrowly scoped.
          auto throwable = create_throwable(class_name, message);
          if (!throwable)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          *result_bits = throwable->bits;
          return static_cast<u32>(JitRuntimeStatus::java_throwable);
        };
        if (reference->name == "size" && reference->descriptor == "()I")
        {
          auto count = heap_.vm_field(vector, 1U);
          if (!count)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          auto value = count->as_int();
          if (!value)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          *result_bits = static_cast<u64>(static_cast<u32>(*value));
          return static_cast<u32>(JitRuntimeStatus::success);
        }
        if (reference->name == "isEmpty" &&
            reference->descriptor == "()Z")
        {
          auto count = heap_.vm_field(vector, 1U);
          if (!count)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          auto value = count->as_int();
          if (!value)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          *result_bits = *value == 0 ? 1U : 0U;
          return static_cast<u32>(JitRuntimeStatus::success);
        }
        if (reference->name == "capacity" &&
            reference->descriptor == "()I")
        {
          auto data_value = heap_.vm_field(vector, 0U);
          if (!data_value)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          auto data = data_value->as_reference();
          if (!data || data->is_null())
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          auto length = heap_.vm_array_length(*data);
          if (!length ||
              *length > static_cast<usize>(std::numeric_limits<i32>::max()))
          {
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          }
          *result_bits = static_cast<u64>(static_cast<u32>(
              static_cast<i32>(*length)));
          return static_cast<u32>(JitRuntimeStatus::success);
        }
        if (reference->name == "elementAt" &&
            reference->descriptor == "(I)Ljava/lang/Object;" &&
            operands->arguments.size() == 1U)
        {
          auto index = operands->arguments[0U].as_int();
          auto count_value = heap_.vm_field(vector, 1U);
          auto data_value = heap_.vm_field(vector, 0U);
          if (!index || !count_value || !data_value)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          auto count = count_value->as_int();
          auto data = data_value->as_reference();
          if (!count || !data || data->is_null())
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          if (*index < 0 || *index >= *count)
          {
            return java_exception(
                "java/lang/ArrayIndexOutOfBoundsException",
                "Vector index is out of range");
          }
          auto value = heap_.vm_array_element_snapshot(
              *data, static_cast<usize>(*index));
          if (!value || value->kind != HeapArrayKind::reference)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          auto reference_value = value->value.as_reference();
          if (!reference_value)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          *result_bits = reference_value->bits;
          return static_cast<u32>(JitRuntimeStatus::success);
        }
      }

      if (operation == JitRuntimeOperation::invoke_interface &&
          operands->receiver.has_value())
      {
        const auto lambda = lambda_bindings_.find(operands->receiver->bits);
        const bool lambda_descriptor_matches =
            lambda != lambda_bindings_.end() &&
            (lambda->second.sam_descriptor == reference->descriptor ||
             std::find(lambda->second.bridge_descriptors.begin(),
                       lambda->second.bridge_descriptors.end(),
                       reference->descriptor) !=
                 lambda->second.bridge_descriptors.end());
        if (lambda != lambda_bindings_.end() &&
            lambda->second.interface_name == reference->owner &&
            lambda->second.sam_name == reference->name &&
            lambda_descriptor_matches)
        {
          std::optional<ObjectRef> constructor_receiver;
          std::optional<NativeRootScope> constructor_receiver_root;
          if (lambda->second.implementation_kind == 8U)
          {
            auto allocated = allocate_pinned_instance(
                lambda->second.implementation.owner);
            if (!allocated)
              return static_cast<u32>(JitRuntimeStatus::deoptimize);
            constructor_receiver_root.emplace(std::move(*allocated));
            auto reference_value = constructor_receiver_root->get();
            if (!reference_value)
              return static_cast<u32>(JitRuntimeStatus::deoptimize);
            constructor_receiver = *reference_value;
          }
          std::array<Value, kInlineInvocationArgumentCapacity>
              inline_lambda_arguments;
          std::vector<Value> overflow_lambda_arguments;
          std::span<Value> lambda_arguments;
          if (operands->arguments.size() <= inline_lambda_arguments.size())
          {
            lambda_arguments = std::span<Value>(
                inline_lambda_arguments.data(), operands->arguments.size());
          }
          else
          {
            overflow_lambda_arguments.resize(operands->arguments.size());
            lambda_arguments = overflow_lambda_arguments;
          }
          operands->arguments.materialize(lambda_arguments);
          auto lambda_invocation = prepare_lambda_invocation(
              *operands->receiver,
              lambda->second,
              std::span<const Value>(lambda_arguments),
              constructor_receiver);
          if (!lambda_invocation)
          {
            if (lambda_invocation.error().code == ErrorCode::java_exception &&
                !lambda_invocation.error().java_exception_class.empty())
            {
              auto throwable = create_throwable(
                  lambda_invocation.error().java_exception_class,
                  lambda_invocation.error().message);
              if (!throwable)
                return static_cast<u32>(JitRuntimeStatus::deoptimize);
              *result_bits = throwable->bits;
              return static_cast<u32>(JitRuntimeStatus::java_throwable);
            }
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          }
          const auto bounded_cost = bounded_jit_invocation_cost(
              lambda_invocation->method, 0U);
          if (!bounded_cost.has_value() || *bounded_cost > nested_budget)
            return static_cast<u32>(JitRuntimeStatus::unsupported_call);

          auto invoked = execute(std::move(*lambda_invocation), nested_budget);
          if (!invoked)
          {
            if (invoked.error().code == ErrorCode::java_exception &&
                !invoked.error().java_exception_class.empty())
            {
              auto throwable = create_throwable(
                  invoked.error().java_exception_class,
                  invoked.error().message);
              if (!throwable)
                return static_cast<u32>(JitRuntimeStatus::deoptimize);
              *result_bits = throwable->bits;
              return static_cast<u32>(JitRuntimeStatus::java_throwable);
            }
            if (invoked.error().code == ErrorCode::invalid_state &&
                invoked.error().message.find("budget") != std::string::npos)
            {
              return static_cast<u32>(JitRuntimeStatus::budget_exhausted);
            }
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          }
          if (invoked->executed_instructions >
              static_cast<u64>(std::numeric_limits<u32>::max()))
          {
            return static_cast<u32>(JitRuntimeStatus::budget_exhausted);
          }
          if (consumed_instructions != nullptr)
          {
            *consumed_instructions = static_cast<u32>(
                invoked->executed_instructions);
          }
          if (invoked->throwable.has_value())
          {
            *result_bits = invoked->throwable->bits;
            return static_cast<u32>(JitRuntimeStatus::java_throwable);
          }
          if (invoked->return_value.has_value() &&
              !encode_value(*invoked->return_value))
          {
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          }
          return static_cast<u32>(JitRuntimeStatus::success);
        }
      }
      std::optional<ResolvedMethod> cached_target;
      std::shared_ptr<const RuntimeClass> receiver_metadata;
      std::optional<std::string_view> receiver_class_name;
      if (operation == JitRuntimeOperation::invoke_static ||
          operation == JitRuntimeOperation::invoke_special)
      {
        auto cache = direct_call_binding_slot(owner, static_cast<u16>(operand));
        if (!cache)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        if ((*cache)->valid)
        {
          auto runtime_target = classes_.metadata().find_method(
              (*cache)->target_method);
          if (runtime_target != nullptr)
          {
            cached_target = ResolvedMethod {
                .owner = runtime_target->owner,
                .method = runtime_target->method,
                .runtime = std::move(runtime_target),
            };
            PerformanceCounters::record_direct_call_cache(true);
          }
          else
          {
            (*cache)->valid = false;
            PerformanceCounters::record_direct_call_cache(false);
          }
        }
        else
        {
          PerformanceCounters::record_direct_call_cache(false);
        }
      }
      else
      {
        auto receiver_class = heap_.vm_class_name_view(*operands->receiver);
        if (!receiver_class)
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        receiver_class_name = *receiver_class;
        receiver_metadata = classes_.metadata().find_class(
            *receiver_class_name);
        if (receiver_metadata == nullptr)
        {
          auto loaded = classes_.load(*receiver_class_name);
          if (!loaded)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          receiver_metadata = classes_.metadata().find_class(
              *receiver_class_name);
        }
        if (operation == JitRuntimeOperation::invoke_interface)
        {
          auto compatible = classes_.is_assignable(
              *receiver_class_name, reference->owner);
          if (!compatible || !*compatible)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
        }
        if (receiver_metadata != nullptr)
        {
          auto cache = virtual_call_binding_slot(owner, static_cast<u16>(operand));
          if (!cache)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          const auto cached_method = (*cache)->lookup(receiver_metadata->id);
          if (cached_method.has_value())
          {
            auto runtime_target = classes_.metadata().find_method(
                *cached_method);
            if (runtime_target != nullptr)
            {
              cached_target = ResolvedMethod {
                  .owner = runtime_target->owner,
                  .method = runtime_target->method,
                  .runtime = std::move(runtime_target),
              };
              PerformanceCounters::record_virtual_inline_cache(true);
            }
            else
            {
              (*cache)->invalidate(receiver_metadata->id);
              PerformanceCounters::record_virtual_inline_cache(false);
            }
          }
          else
          {
            PerformanceCounters::record_virtual_inline_cache(false);
          }
        }
      }

      auto resolved_target = [&]() -> Result<ResolvedMethod>
      {
        if (cached_target.has_value()) return *cached_target;
        if (operation == JitRuntimeOperation::invoke_special)
        {
          return classes_.resolve_declared_method(reference->owner,
                                                  reference->name,
                                                  reference->descriptor);
        }
        if (operation == JitRuntimeOperation::invoke_static)
        {
          return classes_.resolve_method(reference->owner,
                                         reference->name,
                                         reference->descriptor);
        }
        if (!receiver_class_name.has_value())
          return fail(ErrorCode::internal_error,
                      "JIT virtual call has no receiver class");
        return classes_.resolve_method(*receiver_class_name,
                                       reference->name,
                                       reference->descriptor);
      }();
      if (resolved_target && resolved_target->runtime != nullptr &&
          !cached_target.has_value())
      {
        if (operation == JitRuntimeOperation::invoke_static ||
            operation == JitRuntimeOperation::invoke_special)
        {
          auto cache = direct_call_binding_slot(owner, static_cast<u16>(operand));
          if (!cache)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          **cache = DirectCallCache {
              .target_method = resolved_target->runtime->id,
              .valid = true,
          };
        }
        else if (receiver_metadata != nullptr)
        {
          auto cache = virtual_call_binding_slot(owner, static_cast<u16>(operand));
          if (!cache)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          (*cache)->update(
              receiver_metadata->id,
              resolved_target->runtime->id);
        }
      }
      if (!resolved_target || resolved_target->method == nullptr)
      {
        return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      if (operation == JitRuntimeOperation::invoke_static)
      {
        std::scoped_lock initialization_lock(class_initialization_mutex_);
        if (!initialized_classes_.contains(reference->owner))
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
      }
      if (operation == JitRuntimeOperation::invoke_virtual &&
          operands->arguments.size() == 2U)
      {
        auto x = operands->arguments[0U].as_int();
        auto y = operands->arguments[1U].as_int();
        if (x && y)
        {
          auto collision = try_tiled_alpha_collision_intrinsic(
              *resolved_target, *x, *y);
          if (!collision)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          if (collision->has_value())
          {
            if (!encode_value(**collision))
              return static_cast<u32>(JitRuntimeStatus::deoptimize);
            return static_cast<u32>(JitRuntimeStatus::success);
          }
        }
      }
      if (operation == JitRuntimeOperation::invoke_static &&
          operands->arguments.size() == 3U)
      {
        auto x = operands->arguments[0U].as_int();
        auto y = operands->arguments[1U].as_int();
        auto shooter = operands->arguments[2U].as_reference();
        if (x && y && shooter)
        {
          auto projectile = try_projectile_collision_intrinsic(
              *resolved_target, *x, *y, *shooter);
          if (!projectile)
            return static_cast<u32>(JitRuntimeStatus::deoptimize);
          if (projectile->has_value())
          {
            if (!encode_value(**projectile))
              return static_cast<u32>(JitRuntimeStatus::deoptimize);
            return static_cast<u32>(JitRuntimeStatus::success);
          }
        }
      }

      // Fast JIT-to-JIT chain for an already compiled method. Runtime-helper
      // callees receive a nested GC/deopt context rooted through the compiled
      // caller. If the callee deoptimizes after a side effect, resume the
      // callee's exact captured frame synchronously instead of replaying the
      // caller's invoke bytecode.
      if (!BaselineJit::conservative_device_mode() &&
          resolved_target->runtime != nullptr &&
          resolved_target->runtime->descriptor != nullptr &&
          (resolved_target->method->access_flags &
           (kAccNative | kAccSynchronized | kAccAbstract)) == 0U)
      {
        const usize argument_count = operands->arguments.size() +
            (has_receiver ? 1U : 0U);
        InvocationArguments chained_arguments(argument_count);
        usize argument_index = 0U;
        if (has_receiver)
        {
          chained_arguments.set(
              argument_index++, Value::from_reference(*operands->receiver));
        }
        for (usize index = 0U; index < operands->arguments.size(); ++index)
        {
          chained_arguments.set_compact(
              argument_index++,
              operands->arguments.kind(index),
              operands->arguments.raw_bits(index));
        }

        JitExecutionContext chained_context{
            .machine = this,
            .owner = resolved_target->owner.get(),
            .method = resolved_target->method,
            .invocation_depth = parent_context != nullptr
                ? parent_context->invocation_depth + 1U
                : 0U,
            .base_roots = {},
            .parent_jit_context = parent_context,
            .outer_roots_context = nullptr,
            .append_outer_roots = nullptr,
            .compact_extra_root_values = &chained_arguments,
            .progress_watchdog = parent_context != nullptr &&
                parent_context->progress_watchdog,
            .progress_total_budget = parent_context != nullptr
                ? parent_context->progress_total_budget
                : 0U,
        };
        if (resolved_target->method->code.has_value())
        {
          chained_context.published_roots.reserve(
              chained_context.base_roots.size() + argument_count +
              resolved_target->method->code->max_locals +
              resolved_target->method->code->max_stack);
        }
        const JitRuntimeHooks chained_hooks{
            .context = &chained_context,
            .dispatch = &Machine::jit_runtime_dispatch_callback,
            .publish_roots = &Machine::jit_publish_roots_callback,
        };

        // A JIT-to-JIT child publishes at a synthetic nested root depth even
        // though it does not enter Machine::execute(). Clear that depth on all
        // exits so completed children cannot retain stale objects until the
        // Java thread itself terminates.
        int chained_root_cleanup_token = 0;
        auto clear_chained_roots =
            [this, depth = chained_context.invocation_depth](int*) noexcept {
              clear_execution_roots(depth);
            };
        std::unique_ptr<int, decltype(clear_chained_roots)>
            chained_root_cleanup(&chained_root_cleanup_token,
                                 clear_chained_roots);

        auto fast = jit_.try_execute_cached(
            resolved_target->runtime->id,
            *resolved_target->owner,
            *resolved_target->method,
            *resolved_target->runtime->descriptor,
            chained_arguments,
            has_receiver,
            nested_budget,
            chained_hooks);
        if (!fast)
        {
          if (fast.error().code == ErrorCode::invalid_state &&
              fast.error().message.find("budget") != std::string::npos)
          {
            return static_cast<u32>(JitRuntimeStatus::budget_exhausted);
          }
          return static_cast<u32>(JitRuntimeStatus::deoptimize);
        }
        if (fast->has_value())
        {
          const u64 fast_instructions =
              static_cast<u64>((*fast)->bytecode_instructions) +
              chained_context.progress_reset_instructions;
          if (fast_instructions >
              static_cast<u64>(std::numeric_limits<u32>::max()))
          {
            return static_cast<u32>(JitRuntimeStatus::budget_exhausted);
          }
          if ((*fast)->deopt_state.has_value())
          {
            auto resumed = prepare_invocation(
                *resolved_target,
                chained_arguments,
                has_receiver,
                std::nullopt,
                true);
            if (!resumed)
              return static_cast<u32>(JitRuntimeStatus::fatal_runtime_error);
            resumed->resume_jit_deopt_state =
                std::move((*fast)->deopt_state);
            resumed->resume_jit_instructions = fast_instructions;
            resumed->resume_jit_nested_instructions =
                chained_context.nested_instructions;
            commit_parent_roots();
            commit_staged_jit_roots(&chained_context);
            auto completed = execute(
                std::move(*resumed),
                nested_budget,
                chained_context.progress_watchdog
                    ? InstructionBudgetMode::progress_watchdog
                    : InstructionBudgetMode::total);
            if (!completed)
            {
              if (completed.error().code == ErrorCode::invalid_state &&
                  completed.error().message.find("budget") !=
                      std::string::npos)
              {
                return static_cast<u32>(JitRuntimeStatus::budget_exhausted);
              }
              return static_cast<u32>(JitRuntimeStatus::fatal_runtime_error);
            }
            if (completed->executed_instructions >
                static_cast<u64>(std::numeric_limits<u32>::max()))
            {
              return static_cast<u32>(JitRuntimeStatus::budget_exhausted);
            }
            if (consumed_instructions != nullptr)
            {
              *consumed_instructions = static_cast<u32>(
                  completed->executed_instructions);
            }
            if (completed->throwable.has_value())
            {
              *result_bits = completed->throwable->bits;
              return static_cast<u32>(JitRuntimeStatus::java_throwable);
            }
            if (descriptor.return_type.kind == JavaTypeKind::void_type)
            {
              if (completed->return_value.has_value())
                return static_cast<u32>(JitRuntimeStatus::fatal_runtime_error);
              *result_bits = 0U;
            }
            else if (!completed->return_value.has_value() ||
                     !encode_value(*completed->return_value))
            {
              return static_cast<u32>(JitRuntimeStatus::fatal_runtime_error);
            }
            if (const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
                trace_value != nullptr && *trace_value != '\0' &&
                std::string_view(trace_value) != "0")
            {
              std::fprintf(stderr,
                           "[phoneMEJIT] chain-resume %s.%s%s bytecodes=%llu\n",
                           resolved_target->owner->name().c_str(),
                           resolved_target->method->name.c_str(),
                           resolved_target->method->descriptor.c_str(),
                           static_cast<unsigned long long>(
                               completed->executed_instructions));
            }
            return static_cast<u32>(JitRuntimeStatus::success);
          }

          if (consumed_instructions != nullptr)
          {
            *consumed_instructions = static_cast<u32>(fast_instructions);
          }
          if ((*fast)->exception.has_value())
          {
            ObjectRef throwable;
            if (*(*fast)->exception == JitExceptionKind::runtime_throwable)
            {
              if (!chained_context.pending_throwable.has_value())
                return static_cast<u32>(JitRuntimeStatus::fatal_runtime_error);
              throwable = *chained_context.pending_throwable;
            }
            else
            {
              auto created = create_throwable(
                  jit_exception_class(*(*fast)->exception));
              if (!created)
                return static_cast<u32>(JitRuntimeStatus::fatal_runtime_error);
              throwable = *created;
            }
            *result_bits = throwable.bits;
            return static_cast<u32>(JitRuntimeStatus::java_throwable);
          }
          if (descriptor.return_type.kind == JavaTypeKind::void_type)
          {
            if ((*fast)->return_value.has_value())
              return static_cast<u32>(JitRuntimeStatus::fatal_runtime_error);
            *result_bits = 0U;
          }
          else if (!(*fast)->return_value.has_value() ||
                   !encode_value(*(*fast)->return_value))
          {
            return static_cast<u32>(JitRuntimeStatus::fatal_runtime_error);
          }
          if (const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
              trace_value != nullptr && *trace_value != '\0' &&
              std::string_view(trace_value) != "0")
          {
            std::fprintf(stderr,
                         "[phoneMEJIT] chain %s.%s%s bytecodes=%u runtime=%d\n",
                         resolved_target->owner->name().c_str(),
                         resolved_target->method->name.c_str(),
                         resolved_target->method->descriptor.c_str(),
                         static_cast<unsigned>(fast_instructions),
                         chained_context.nested_instructions != 0U ? 1 : 0);
          }
          return static_cast<u32>(JitRuntimeStatus::success);
        }
      }

      // No cached native body was available. Only now require the conservative
      // bounded-call proof before entering the callee through the interpreter.
      // Keeping this check ahead of try_execute_cached made hot virtual helpers
      // impossible to chain: a method could already be safely compiled with
      // its own budget guards and precise deopt state, yet the caller was
      // rejected because the source-level call tree contained a virtual call.
      const auto bounded_cost = bounded_jit_invocation_cost(
          *resolved_target, 0U);
      if (!bounded_cost.has_value() || *bounded_cost > nested_budget)
      {
        if (const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
            trace_value != nullptr && *trace_value != '\0' &&
            std::string_view(trace_value) != "0")
        {
          std::fprintf(stderr,
                       "[phoneMEJIT] predeopt-call %s.%s%s budget=%llu "
                       "bound=%s\n",
                       reference->owner.c_str(),
                       reference->name.c_str(),
                       reference->descriptor.c_str(),
                       static_cast<unsigned long long>(nested_budget),
                       bounded_cost.has_value() ? "over" : "unbounded");
        }
        // Java bytecode may become a valid cached JIT target after this first
        // interpreter fallback. Treat that as a transient precise deopt so the
        // caller can retry later. Conservative native/abstract targets cannot
        // acquire such a compiled body and remain permanently unsupported.
        return static_cast<u32>(resolved_target->method->code.has_value()
            ? JitRuntimeStatus::retryable_call
            : JitRuntimeStatus::unsupported_call);
      }

      // Execute the resolved target through the normal VM entry path. The
      // admission check above proves that this call chain cannot consume more
      // bytecodes than the remaining JIT budget and cannot block in native or
      // synchronized code before returning to the compiled caller.
      auto invoked = [&]() -> Result<ExecutionResult>
      {
        const usize argument_count = operands->arguments.size() +
                                     (has_receiver ? 1U : 0U);
        InvocationArguments invocation_arguments(argument_count);
        usize argument_index = 0U;
        if (has_receiver)
        {
          if (!operands->receiver.has_value() || operands->receiver->is_null())
            return fail(ErrorCode::invalid_argument,
                        "JIT resolved invocation has no receiver");
          invocation_arguments.set(
              argument_index++, Value::from_reference(*operands->receiver));
        }
        for (usize index = 0U; index < operands->arguments.size(); ++index)
        {
          invocation_arguments.set_compact(
              argument_index++,
              operands->arguments.kind(index),
              operands->arguments.raw_bits(index));
        }
        auto invocation = prepare_invocation(
            std::move(*resolved_target),
            std::move(invocation_arguments),
            has_receiver,
            std::nullopt,
            true);
        if (!invocation)
          return std::unexpected(invocation.error());
        commit_parent_roots();
        return execute(std::move(*invocation), nested_budget);
      }();

      if (!invoked)
      {
        if (invoked.error().code == ErrorCode::java_exception &&
            !invoked.error().java_exception_class.empty())
        {
          auto throwable = create_throwable(
              invoked.error().java_exception_class,
              invoked.error().message);
          if (!throwable)
            return static_cast<u32>(JitRuntimeStatus::fatal_runtime_error);
          *result_bits = throwable->bits;
          return static_cast<u32>(JitRuntimeStatus::java_throwable);
        }
        if (invoked.error().code == ErrorCode::invalid_state &&
            invoked.error().message.find("budget") != std::string::npos)
        {
          return static_cast<u32>(JitRuntimeStatus::budget_exhausted);
        }
        // The callee invocation has already begun. Never deopt the caller here:
        // replaying its invoke bytecode could duplicate callee side effects.
        return static_cast<u32>(JitRuntimeStatus::fatal_runtime_error);
      }
      if (invoked->executed_instructions >
          static_cast<u64>(std::numeric_limits<u32>::max()))
      {
        return static_cast<u32>(JitRuntimeStatus::budget_exhausted);
      }
      if (consumed_instructions != nullptr)
      {
        *consumed_instructions = static_cast<u32>(
            invoked->executed_instructions);
      }
      if (invoked->throwable.has_value())
      {
        *result_bits = invoked->throwable->bits;
        return static_cast<u32>(JitRuntimeStatus::java_throwable);
      }
      if (descriptor.return_type.kind == JavaTypeKind::void_type)
      {
        if (invoked->return_value.has_value())
          return static_cast<u32>(JitRuntimeStatus::fatal_runtime_error);
        *result_bits = 0U;
        return static_cast<u32>(JitRuntimeStatus::success);
      }
      if (!invoked->return_value.has_value() ||
          !encode_value(*invoked->return_value))
      {
        return static_cast<u32>(JitRuntimeStatus::fatal_runtime_error);
      }
      return static_cast<u32>(JitRuntimeStatus::success);
    }
    case JitRuntimeOperation::float_remainder:
    {
      const float left = std::bit_cast<float>(static_cast<u32>(first));
      const float right = std::bit_cast<float>(static_cast<u32>(second));
      *result_bits = static_cast<u64>(
          std::bit_cast<u32>(std::fmod(left, right)));
      return 0U;
    }
    case JitRuntimeOperation::double_remainder:
    {
      const double left = std::bit_cast<double>(first);
      const double right = std::bit_cast<double>(second);
      *result_bits = std::bit_cast<u64>(std::fmod(left, right));
      return 0U;
    }
    case JitRuntimeOperation::float_compare_less:
    case JitRuntimeOperation::float_compare_greater:
    {
      const float left = std::bit_cast<float>(static_cast<u32>(first));
      const float right = std::bit_cast<float>(static_cast<u32>(second));
      const i32 result = compare_float(
          left,
          right,
          operation == JitRuntimeOperation::float_compare_less ? -1 : 1);
      *result_bits = static_cast<u64>(static_cast<u32>(result));
      return 0U;
    }
    case JitRuntimeOperation::double_compare_less:
    case JitRuntimeOperation::double_compare_greater:
    {
      const double left = std::bit_cast<double>(first);
      const double right = std::bit_cast<double>(second);
      const i32 result = compare_double(
          left,
          right,
          operation == JitRuntimeOperation::double_compare_less ? -1 : 1);
      *result_bits = static_cast<u64>(static_cast<u32>(result));
      return 0U;
    }
    case JitRuntimeOperation::int_to_float:
    {
      const i32 value = static_cast<i32>(static_cast<u32>(first));
      *result_bits = static_cast<u64>(
          std::bit_cast<u32>(static_cast<float>(value)));
      return 0U;
    }
    case JitRuntimeOperation::int_to_double:
    {
      const i32 value = static_cast<i32>(static_cast<u32>(first));
      *result_bits = std::bit_cast<u64>(static_cast<double>(value));
      return 0U;
    }
    case JitRuntimeOperation::long_to_float:
    {
      const i64 value = static_cast<i64>(first);
      *result_bits = static_cast<u64>(
          std::bit_cast<u32>(static_cast<float>(value)));
      return 0U;
    }
    case JitRuntimeOperation::long_to_double:
    {
      const i64 value = static_cast<i64>(first);
      *result_bits = std::bit_cast<u64>(static_cast<double>(value));
      return 0U;
    }
    case JitRuntimeOperation::float_to_int:
    {
      const float value = std::bit_cast<float>(static_cast<u32>(first));
      *result_bits = static_cast<u64>(
          static_cast<u32>(java_to_int(static_cast<double>(value))));
      return 0U;
    }
    case JitRuntimeOperation::float_to_long:
    {
      const float value = std::bit_cast<float>(static_cast<u32>(first));
      *result_bits = static_cast<u64>(
          java_to_long(static_cast<double>(value)));
      return 0U;
    }
    case JitRuntimeOperation::float_to_double:
    {
      const float value = std::bit_cast<float>(static_cast<u32>(first));
      *result_bits = std::bit_cast<u64>(static_cast<double>(value));
      return 0U;
    }
    case JitRuntimeOperation::double_to_int:
    {
      const double value = std::bit_cast<double>(first);
      *result_bits = static_cast<u64>(
          static_cast<u32>(java_to_int(value)));
      return 0U;
    }
    case JitRuntimeOperation::double_to_long:
    {
      const double value = std::bit_cast<double>(first);
      *result_bits = static_cast<u64>(java_to_long(value));
      return 0U;
    }
    case JitRuntimeOperation::double_to_float:
    {
      const double value = std::bit_cast<double>(first);
      *result_bits = static_cast<u64>(
          std::bit_cast<u32>(static_cast<float>(value)));
      return 0U;
    }
    }
    return 1U;
  }

  Result<ObjectRef> Machine::intern_string(std::string_view modified_utf8)
  {
    auto decoded = decode_modified_utf8(modified_utf8);
    if (!decoded)
    {
      return std::unexpected(decoded.error());
    }
    if (const auto iterator = interned_strings_.find(*decoded);
        iterator != interned_strings_.end())
    {
      auto existing = heap_.string_value(iterator->second);
      if (existing)
      {
        return iterator->second;
      }
      interned_strings_.erase(iterator);
    }

    auto initialized = ensure_initialized("java/lang/String", 1'000'000);
    if (!initialized)
    {
      return std::unexpected(initialized.error());
    }
    if (initialized->has_value())
    {
      return fail(ErrorCode::invalid_state,
                  "java/lang/String initialization threw an exception");
    }
    auto reference = states_.allocate_text_instance(
        heap_, "java/lang/String", *decoded);
    if (!reference)
    {
      return std::unexpected(reference.error());
    }
    interned_strings_.emplace(std::move(*decoded), *reference);
    return *reference;
  }

  Result<ObjectRef> Machine::class_mirror(std::string_view class_name)
  {
    if (class_name.empty())
    {
      return fail(ErrorCode::invalid_argument,
                  "class mirror name must not be empty");
    }
    const std::string key(class_name);
    const auto existing = class_mirrors_.find(key);
    if (existing != class_mirrors_.end())
    {
      return existing->second;
    }
    auto mirror = states_.allocate_instance(heap_, "java/lang/Class");
    if (!mirror)
      return std::unexpected(mirror.error());
    class_mirrors_.emplace(key, *mirror);
    return *mirror;
  }

  Result<std::optional<ObjectRef>> Machine::synchronized_monitor(
      const Invocation &invocation)
  {
    if (invocation.method.method == nullptr ||
        (invocation.method.method->access_flags & kAccSynchronized) == 0U)
    {
      return std::optional<ObjectRef>{};
    }

    ObjectRef monitor;
    if ((invocation.method.method->access_flags & kAccStatic) != 0U)
    {
      if (invocation.method.owner == nullptr)
      {
        return fail(ErrorCode::internal_error,
                    "static synchronized invocation has no owner class");
      }
      auto mirror = class_mirror(invocation.method.owner->name());
      if (!mirror)
        return std::unexpected(mirror.error());
      monitor = *mirror;
    }
    else
    {
      if (invocation.arguments.empty())
      {
        return fail(ErrorCode::internal_error,
                    "instance synchronized invocation has no receiver");
      }
      if (invocation.arguments.kind(0U) != ValueKind::reference)
      {
        return fail(ErrorCode::internal_error,
                    "instance synchronized receiver is not a reference");
      }
      const ObjectRef receiver =
          invocation.arguments.reference_unchecked(0U);
      if (receiver.is_null())
      {
        return fail(ErrorCode::invalid_argument,
                    "instance synchronized receiver is null");
      }
      monitor = receiver;
    }

    return std::optional<ObjectRef>(monitor);
  }

  Result<std::optional<ObjectRef>> Machine::acquire_synchronized_monitor(
      const Invocation &invocation)
  {
    auto monitor = synchronized_monitor(invocation);
    if (!monitor)
      return std::unexpected(monitor.error());
    if (!monitor->has_value())
      return std::optional<ObjectRef>{};
    auto entered = enter_monitor(**monitor);
    if (!entered)
      return std::unexpected(entered.error());
    return *monitor;
  }

  Status Machine::release_synchronized_monitor(
      std::optional<ObjectRef> monitor)
  {
    if (!monitor.has_value())
      return {};
    return monitors_.exit(*monitor, scheduler_.current_thread_id());
  }

  Result<Machine::LambdaBinding> Machine::resolve_lambda_binding(
      const classfile::ClassFile &owner,
      u16 invoke_dynamic_index)
  {
    auto dynamic = owner.invoke_dynamic_reference(invoke_dynamic_index);
    if (!dynamic)
      return std::unexpected(dynamic.error());
    auto call_site = parse_method_descriptor(dynamic->descriptor);
    if (!call_site)
      return std::unexpected(call_site.error());
    if (call_site->return_type.kind != JavaTypeKind::reference)
    {
      return fail(ErrorCode::unsupported_feature,
                  "LambdaMetafactory call site must return an interface");
    }

    auto interface_class = classes_.load(call_site->return_type.class_name);
    if (!interface_class)
      return std::unexpected(interface_class.error());
    if (((*interface_class)->access_flags() & kAccInterface) == 0U)
    {
      return fail(ErrorCode::unsupported_feature,
                  "LambdaMetafactory return type is not an interface");
    }

    auto bootstrap = owner.bootstrap_method(dynamic->bootstrap_method_index);
    if (!bootstrap)
      return std::unexpected(bootstrap.error());
    auto bootstrap_handle = owner.method_handle_reference(
        (*bootstrap)->method_handle_index);
    if (!bootstrap_handle)
      return std::unexpected(bootstrap_handle.error());
    const bool is_metafactory =
        bootstrap_handle->member.owner ==
            "java/lang/invoke/LambdaMetafactory" &&
        (bootstrap_handle->member.name == "metafactory" ||
         bootstrap_handle->member.name == "altMetafactory");
    if (!is_metafactory || bootstrap_handle->reference_kind != 6U)
    {
      return fail(ErrorCode::unsupported_feature,
                  "unsupported invokedynamic bootstrap method");
    }
    if ((*bootstrap)->arguments.size() < 3U)
    {
      return fail(ErrorCode::malformed_class,
                  "LambdaMetafactory bootstrap arguments are incomplete");
    }

    auto sam_descriptor = owner.method_type_descriptor(
        (*bootstrap)->arguments[0]);
    auto implementation = owner.method_handle_reference(
        (*bootstrap)->arguments[1]);
    auto instantiated_descriptor = owner.method_type_descriptor(
        (*bootstrap)->arguments[2]);
    if (!sam_descriptor)
      return std::unexpected(sam_descriptor.error());
    if (!implementation)
      return std::unexpected(implementation.error());
    if (!instantiated_descriptor)
      return std::unexpected(instantiated_descriptor.error());

    std::vector<std::string> marker_interfaces;
    std::vector<std::string> bridge_descriptors;
    if (bootstrap_handle->member.name == "altMetafactory")
    {
      if ((*bootstrap)->arguments.size() < 4U)
      {
        return fail(ErrorCode::malformed_class,
                    "altMetafactory bootstrap arguments are incomplete");
      }
      const auto integer_argument = [&owner](u16 index) -> Result<u32>
      {
        auto constant = owner.constant(index);
        if (!constant)
          return std::unexpected(constant.error());
        if ((*constant)->kind != classfile::ConstantKind::integer)
        {
          return fail(ErrorCode::malformed_class,
                      "altMetafactory count or flags argument is not an integer");
        }
        return static_cast<u32>((*constant)->bits);
      };

      auto flags = integer_argument((*bootstrap)->arguments[3]);
      if (!flags)
        return std::unexpected(flags.error());
      constexpr u32 kSerializable = 0x01U;
      constexpr u32 kMarkers = 0x02U;
      constexpr u32 kBridges = 0x04U;
      if ((*flags & ~(kSerializable | kMarkers | kBridges)) != 0U)
      {
        return fail(ErrorCode::unsupported_feature,
                    "altMetafactory contains unsupported flags");
      }
      usize cursor = 4U;
      if ((*flags & kSerializable) != 0U)
      {
        marker_interfaces.push_back("java/io/Serializable");
      }
      if ((*flags & kMarkers) != 0U)
      {
        if (cursor >= (*bootstrap)->arguments.size())
        {
          return fail(ErrorCode::malformed_class,
                      "altMetafactory marker count is missing");
        }
        auto marker_count = integer_argument((*bootstrap)->arguments[cursor++]);
        if (!marker_count)
          return std::unexpected(marker_count.error());
        if (static_cast<usize>(*marker_count) >
            (*bootstrap)->arguments.size() - cursor)
        {
          return fail(ErrorCode::malformed_class,
                      "altMetafactory marker list is truncated");
        }
        marker_interfaces.reserve(marker_interfaces.size() +
                                  static_cast<usize>(*marker_count));
        for (u32 marker_index = 0; marker_index < *marker_count;
             ++marker_index)
        {
          auto marker = owner.class_name_constant(
              (*bootstrap)->arguments[cursor++]);
          if (!marker)
            return std::unexpected(marker.error());
          marker_interfaces.push_back(std::move(*marker));
        }
      }
      if ((*flags & kBridges) != 0U)
      {
        if (cursor >= (*bootstrap)->arguments.size())
        {
          return fail(ErrorCode::malformed_class,
                      "altMetafactory bridge count is missing");
        }
        auto bridge_count = integer_argument((*bootstrap)->arguments[cursor++]);
        if (!bridge_count)
          return std::unexpected(bridge_count.error());
        if (static_cast<usize>(*bridge_count) >
            (*bootstrap)->arguments.size() - cursor)
        {
          return fail(ErrorCode::malformed_class,
                      "altMetafactory bridge list is truncated");
        }
        bridge_descriptors.reserve(static_cast<usize>(*bridge_count));
        for (u32 bridge_index = 0; bridge_index < *bridge_count;
             ++bridge_index)
        {
          auto bridge = owner.method_type_descriptor(
              (*bootstrap)->arguments[cursor++]);
          if (!bridge)
            return std::unexpected(bridge.error());
          bridge_descriptors.push_back(std::move(*bridge));
        }
      }
      if (cursor != (*bootstrap)->arguments.size())
      {
        return fail(ErrorCode::malformed_class,
                    "altMetafactory has unexpected trailing arguments");
      }

      std::sort(marker_interfaces.begin(), marker_interfaces.end());
      marker_interfaces.erase(
          std::unique(marker_interfaces.begin(), marker_interfaces.end()),
          marker_interfaces.end());
      for (const std::string &marker : marker_interfaces)
      {
        auto marker_class = classes_.load(marker);
        if (!marker_class)
          return std::unexpected(marker_class.error());
        if (((*marker_class)->access_flags() & kAccInterface) == 0U)
        {
          return fail(ErrorCode::unsupported_feature,
                      "altMetafactory marker is not an interface");
        }
      }
    }

    auto sam_type = parse_method_descriptor(*sam_descriptor);
    auto instantiated_type = parse_method_descriptor(*instantiated_descriptor);
    auto implementation_type = parse_method_descriptor(
        implementation->member.descriptor);
    if (!sam_type)
      return std::unexpected(sam_type.error());
    if (!instantiated_type)
      return std::unexpected(instantiated_type.error());
    if (!implementation_type)
      return std::unexpected(implementation_type.error());
    if (sam_type->parameters.size() != instantiated_type->parameters.size())
    {
      return fail(ErrorCode::unsupported_feature,
                  "LambdaMetafactory parameter adaptation is unsupported");
    }

    auto sam_method = classes_.resolve_method(call_site->return_type.class_name,
                                              dynamic->name,
                                              *sam_descriptor);
    if (!sam_method)
      return std::unexpected(sam_method.error());
    if ((sam_method->method->access_flags & kAccStatic) != 0U)
    {
      return fail(ErrorCode::malformed_class,
                  "lambda target method is static on its interface");
    }

    const usize combined_values = call_site->parameters.size() +
                                  instantiated_type->parameters.size();
    usize implementation_values = implementation_type->parameters.size();
    switch (implementation->reference_kind)
    {
    case 5:
    case 7:
    case 9:
      ++implementation_values;
      break;
    case 6:
    case 8:
      break;
    default:
      return fail(ErrorCode::unsupported_feature,
                  "unsupported lambda implementation method-handle kind");
    }
    if (combined_values != implementation_values)
    {
      return fail(ErrorCode::unsupported_feature,
                  "LambdaMetafactory argument adaptation has invalid arity");
    }

    const auto same_type = [](const TypeDescriptor& left,
                              const TypeDescriptor& right) -> Result<bool>
    {
      if (left.kind != right.kind)
        return false;
      if (!left.reference_like())
        return true;
      auto left_name = reference_type_name(left);
      auto right_name = reference_type_name(right);
      if (!left_name)
        return std::unexpected(left_name.error());
      if (!right_name)
        return std::unexpected(right_name.error());
      return *left_name == *right_name;
    };
    const auto reference_assignable = [this](const TypeDescriptor& source,
                                              const TypeDescriptor& target)
        -> Result<bool>
    {
      if (!source.reference_like() || !target.reference_like())
        return false;
      auto source_name = reference_type_name(source);
      auto target_name = reference_type_name(target);
      if (!source_name)
        return std::unexpected(source_name.error());
      if (!target_name)
        return std::unexpected(target_name.error());
      return classes_.is_assignable(*source_name, *target_name);
    };
    const auto parameter_adaptable =
        [this, &reference_assignable](const TypeDescriptor& source,
                                      const TypeDescriptor& target)
        -> Result<bool>
    {
      if (!source.reference_like() && !target.reference_like())
        return primitive_widening_allowed(source.kind, target.kind);
      if (!source.reference_like() && target.reference_like())
      {
        if (target.kind == JavaTypeKind::array)
          return false;
        const std::string_view wrapper = primitive_wrapper_class(source.kind);
        if (wrapper.empty())
          return false;
        return classes_.is_assignable(wrapper, target.class_name);
      }
      if (source.reference_like() && !target.reference_like())
      {
        if (source.kind != JavaTypeKind::reference)
          return false;
        const auto primitive = wrapper_primitive_kind(source.class_name);
        return primitive.has_value() &&
               primitive_widening_allowed(*primitive, target.kind);
      }
      return reference_assignable(source, target);
    };
    const auto return_adaptable =
        [this](const TypeDescriptor& source,
                                      const TypeDescriptor& target)
        -> Result<bool>
    {
      if (target.kind == JavaTypeKind::void_type)
        return true;
      if (source.kind == JavaTypeKind::void_type)
        return false;
      if (!source.reference_like() && !target.reference_like())
        return primitive_widening_allowed(source.kind, target.kind);
      if (!source.reference_like() && target.reference_like())
      {
        if (target.kind == JavaTypeKind::array)
          return false;
        const std::string_view wrapper = primitive_wrapper_class(source.kind);
        if (wrapper.empty())
          return false;
        return classes_.is_assignable(wrapper, target.class_name);
      }
      if (source.reference_like() && !target.reference_like())
      {
        if (source.kind == JavaTypeKind::array)
          return false;
        const auto primitive = wrapper_primitive_kind(source.class_name);
        return !primitive.has_value() ||
               primitive_widening_allowed(*primitive, target.kind);
      }
      // Reference returns are invocation-time casts; the implementation return
      // need not already be a subtype of the instantiated return type.
      return true;
    };

    for (usize index = 0; index < sam_type->parameters.size(); ++index)
    {
      auto equal = same_type(instantiated_type->parameters[index],
                             sam_type->parameters[index]);
      if (!equal)
        return std::unexpected(equal.error());
      if (*equal)
        continue;
      auto subtype = reference_assignable(instantiated_type->parameters[index],
                                          sam_type->parameters[index]);
      if (!subtype)
        return std::unexpected(subtype.error());
      if (!*subtype)
      {
        return fail(ErrorCode::unsupported_feature,
                    "LambdaMetafactory instantiated parameter is incompatible with SAM");
      }
    }
    auto return_equal = same_type(instantiated_type->return_type,
                                  sam_type->return_type);
    if (!return_equal)
      return std::unexpected(return_equal.error());
    if (!*return_equal && instantiated_type->return_type.reference_like() &&
        sam_type->return_type.reference_like())
    {
      auto subtype = reference_assignable(instantiated_type->return_type,
                                          sam_type->return_type);
      if (!subtype)
        return std::unexpected(subtype.error());
      if (!*subtype)
      {
        return fail(ErrorCode::unsupported_feature,
                    "LambdaMetafactory instantiated return is incompatible with SAM");
      }
    }
    else if (!*return_equal)
    {
      return fail(ErrorCode::unsupported_feature,
                  "LambdaMetafactory instantiated return does not match SAM");
    }

    std::vector<TypeDescriptor> implementation_inputs;
    implementation_inputs.reserve(implementation_values);
    if (implementation->reference_kind == 5U ||
        implementation->reference_kind == 7U ||
        implementation->reference_kind == 9U)
    {
      implementation_inputs.push_back(TypeDescriptor {
          .kind = JavaTypeKind::reference,
          .class_name = implementation->member.owner,
      });
    }
    implementation_inputs.insert(implementation_inputs.end(),
                                 implementation_type->parameters.begin(),
                                 implementation_type->parameters.end());
    for (usize index = 0; index < call_site->parameters.size(); ++index)
    {
      auto equal = same_type(call_site->parameters[index],
                             implementation_inputs[index]);
      if (!equal)
        return std::unexpected(equal.error());
      if (*equal)
        continue;

      // javac may capture a concrete subtype for a bound reference to a
      // method declared on its superclass/interface. Only the first capture
      // can be that instance receiver; every other capture remains exact.
      const bool captured_instance_receiver =
          index == 0U &&
          (implementation->reference_kind == 5U ||
           implementation->reference_kind == 7U ||
           implementation->reference_kind == 9U) &&
          call_site->parameters[index].reference_like() &&
          implementation_inputs[index].reference_like();
      if (captured_instance_receiver)
      {
        auto compatible = reference_assignable(
            call_site->parameters[index], implementation_inputs[index]);
        if (!compatible)
          return std::unexpected(compatible.error());
        if (*compatible)
          continue;
      }
      return fail(ErrorCode::unsupported_feature,
                  "LambdaMetafactory captured argument type does not match implementation");
    }
    for (usize index = 0; index < instantiated_type->parameters.size(); ++index)
    {
      const usize implementation_index = call_site->parameters.size() + index;
      auto adaptable = parameter_adaptable(
          instantiated_type->parameters[index],
          implementation_inputs[implementation_index]);
      if (!adaptable)
        return std::unexpected(adaptable.error());
      if (!*adaptable)
      {
        return fail(ErrorCode::unsupported_feature,
                    "LambdaMetafactory parameter cannot adapt to implementation");
      }
    }
    TypeDescriptor implementation_return = implementation_type->return_type;
    if (implementation->reference_kind == 8U)
    {
      implementation_return = TypeDescriptor {
          .kind = JavaTypeKind::reference,
          .class_name = implementation->member.owner,
      };
    }
    auto adaptable_return = return_adaptable(
        implementation_return, instantiated_type->return_type);
    if (!adaptable_return)
      return std::unexpected(adaptable_return.error());
    if (!*adaptable_return)
    {
      return fail(ErrorCode::unsupported_feature,
                  "LambdaMetafactory return cannot adapt to instantiated type");
    }

    if (implementation->reference_kind == 8U)
    {
      if (instantiated_type->return_type.kind != JavaTypeKind::reference)
      {
        return fail(ErrorCode::unsupported_feature,
                    "constructor method reference must return an object");
      }
      auto compatible = classes_.is_assignable(
          implementation->member.owner,
          instantiated_type->return_type.class_name);
      if (!compatible)
        return std::unexpected(compatible.error());
      if (!*compatible)
      {
        return fail(ErrorCode::unsupported_feature,
                    "constructor result is incompatible with lambda return type");
      }
    }

    return LambdaBinding{
        .interface_name = call_site->return_type.class_name,
        .sam_name = dynamic->name,
        .sam_descriptor = std::move(*sam_descriptor),
        .instantiated_descriptor = std::move(*instantiated_descriptor),
        .implementation_kind = implementation->reference_kind,
        .implementation = std::move(implementation->member),
        .marker_interfaces = std::move(marker_interfaces),
        .bridge_descriptors = std::move(bridge_descriptors),
        .captured_count = call_site->parameters.size(),
    };
  }

  Result<Machine::Invocation> Machine::prepare_lambda_invocation(
      ObjectRef receiver,
      const LambdaBinding &binding,
      std::span<const Value> invocation_arguments,
      std::optional<ObjectRef> constructor_receiver)
  {
    auto instantiated = parse_method_descriptor(
        binding.instantiated_descriptor);
    if (!instantiated)
      return std::unexpected(instantiated.error());
    if (invocation_arguments.size() != instantiated->parameters.size())
    {
      return fail(ErrorCode::invalid_argument,
                  "lambda invocation argument count is invalid");
    }
    for (usize index = 0; index < invocation_arguments.size(); ++index)
    {
      const TypeDescriptor& target = instantiated->parameters[index];
      if (!target.reference_like())
        continue;
      if (invocation_arguments[index].kind() != ValueKind::reference)
      {
        return fail_java("java/lang/ClassCastException",
                         "lambda reference argument is not an object");
      }
      auto reference = invocation_arguments[index].as_reference();
      if (!reference)
        return std::unexpected(reference.error());
      if (reference->is_null())
        continue;
      auto target_name = reference_type_name(target);
      if (!target_name)
        return std::unexpected(target_name.error());
      auto runtime_class = heap_.class_name(*reference);
      if (!runtime_class)
        return std::unexpected(runtime_class.error());
      auto compatible = classes_.is_assignable(*runtime_class,
                                               *target_name);
      if (!compatible)
        return std::unexpected(compatible.error());
      if (!*compatible)
      {
        return fail_java("java/lang/ClassCastException",
                         "lambda argument is incompatible with instantiated type");
      }
    }

    std::vector<Value> combined;
    combined.reserve(binding.captured_count + invocation_arguments.size());
    for (usize index = 0; index < binding.captured_count; ++index)
    {
      auto captured = heap_.field(receiver, index);
      if (!captured)
        return std::unexpected(captured.error());
      combined.push_back(*captured);
    }
    combined.insert(combined.end(),
                    invocation_arguments.begin(),
                    invocation_arguments.end());

    auto implementation = parse_method_descriptor(
        binding.implementation.descriptor);
    if (!implementation)
      return std::unexpected(implementation.error());

    // LambdaMetafactory may expose an erased/reference SAM while the method
    // handle consumes primitives. A common example is
    // Function<Integer, T> bound to T::new(int). The SAM supplies an Integer
    // reference and the JVM must unbox it before invoking the constructor.
    // Keep this adaptation in the VM rather than requiring patched game code.
    const auto adapt_primitive_argument = [&](Value value,
                                               const TypeDescriptor& target,
                                               std::optional<JavaTypeKind> source_hint)
        -> Result<Value>
    {
      if (value_matches(value, target))
        return value;
      if (target.reference_like() || target.kind == JavaTypeKind::void_type)
      {
        return fail(ErrorCode::invalid_argument,
                    "lambda argument does not match implementation descriptor");
      }
      if (value.kind() != ValueKind::reference)
      {
        if (!source_hint.has_value() ||
            !primitive_widening_allowed(*source_hint, target.kind))
        {
          return fail(ErrorCode::invalid_argument,
                      "lambda primitive argument cannot adapt to implementation type");
        }
        return widen_primitive_value(value, *source_hint, target.kind);
      }

      return unbox_lambda_value(value, target.kind);
    };

    const bool implementation_has_receiver =
        binding.implementation_kind == 5U ||
        binding.implementation_kind == 7U ||
        binding.implementation_kind == 9U;
    const usize implementation_offset = implementation_has_receiver ? 1U : 0U;
    if (combined.size() != implementation->parameters.size() +
                           implementation_offset)
    {
      return fail(ErrorCode::invalid_argument,
                  "lambda implementation argument count is invalid");
    }
    std::vector<NativeRootScope> boxing_roots;
    boxing_roots.reserve(implementation->parameters.size());
    for (usize index = 0; index < implementation->parameters.size(); ++index)
    {
      const usize combined_index = index + implementation_offset;
      Value& argument = combined[combined_index];
      const TypeDescriptor& target = implementation->parameters[index];
      std::optional<JavaTypeKind> source_hint;
      if (combined_index >= binding.captured_count)
      {
        const usize invocation_index = combined_index - binding.captured_count;
        if (invocation_index < instantiated->parameters.size())
          source_hint = instantiated->parameters[invocation_index].kind;
      }

      if (target.reference_like())
      {
        if (argument.kind() == ValueKind::reference)
        {
          auto reference = argument.as_reference();
          if (!reference)
            return std::unexpected(reference.error());
          if (!reference->is_null())
          {
            auto target_name = reference_type_name(target);
            if (!target_name)
              return std::unexpected(target_name.error());
            auto runtime_class = heap_.class_name(*reference);
            if (!runtime_class)
              return std::unexpected(runtime_class.error());
            auto compatible = classes_.is_assignable(*runtime_class,
                                                     *target_name);
            if (!compatible)
              return std::unexpected(compatible.error());
            if (!*compatible)
            {
              return fail_java(
                  "java/lang/ClassCastException",
                  "lambda reference argument is incompatible with implementation type");
            }
          }
          continue;
        }
        if (!source_hint.has_value() || target.kind == JavaTypeKind::array)
        {
          return fail(ErrorCode::invalid_argument,
                      "lambda primitive argument cannot box to implementation type");
        }
        const std::string_view wrapper_class =
            primitive_wrapper_class(*source_hint);
        if (wrapper_class.empty())
        {
          return fail(ErrorCode::invalid_argument,
                      "lambda primitive argument has no wrapper class");
        }
        auto compatible = classes_.is_assignable(wrapper_class,
                                                 target.class_name);
        if (!compatible)
          return std::unexpected(compatible.error());
        if (!*compatible)
        {
          return fail(ErrorCode::invalid_argument,
                      "boxed lambda argument is incompatible with implementation type");
        }
        auto wrapper_root = allocate_pinned_instance(wrapper_class);
        if (!wrapper_root)
          return std::unexpected(wrapper_root.error());
        auto wrapper = wrapper_root->get();
        if (!wrapper)
          return std::unexpected(wrapper.error());
        auto stored = heap_.set_field(*wrapper, 0U, argument);
        if (!stored)
          return std::unexpected(stored.error());
        argument = Value::from_reference(*wrapper);
        boxing_roots.push_back(std::move(*wrapper_root));
        continue;
      }

      if (value_matches(argument, target))
        continue;
      auto adapted = adapt_primitive_argument(argument, target, source_hint);
      if (!adapted)
        return std::unexpected(adapted.error());
      argument = *adapted;
    }

    const auto configure_lambda_return = [&](Invocation& invocation) -> Status
    {
      const TypeDescriptor& source = implementation->return_type;
      const TypeDescriptor& target = instantiated->return_type;
      if (target.kind == JavaTypeKind::void_type)
      {
        invocation.discard_return_value =
            source.kind != JavaTypeKind::void_type;
        return {};
      }
      if (source.kind == JavaTypeKind::void_type)
      {
        return fail(ErrorCode::unsupported_feature,
                    "lambda void implementation cannot produce a value");
      }
      if (source.reference_like())
      {
        if (!target.reference_like())
        {
          invocation.return_unboxing_target = target.kind;
          return {};
        }
        auto target_name = reference_type_name(target);
        if (!target_name)
          return std::unexpected(target_name.error());
        if (*target_name != "java/lang/Object")
          invocation.return_reference_cast_target = std::move(*target_name);
        return {};
      }
      if (!target.reference_like())
      {
        if (!primitive_widening_allowed(source.kind, target.kind))
        {
          return fail(ErrorCode::unsupported_feature,
                      "lambda primitive return cannot adapt to target type");
        }
        const bool same_physical_kind =
            value_matches(Value::from_int(0), source) &&
            value_matches(Value::from_int(0), target);
        if (source.kind != target.kind && !same_physical_kind)
        {
          invocation.return_widening_source = source.kind;
          invocation.return_widening_target = target.kind;
        }
        return {};
      }
      if (target.kind == JavaTypeKind::array)
      {
        return fail(ErrorCode::unsupported_feature,
                    "lambda primitive return cannot box to an array");
      }

      const std::string_view wrapper_class = primitive_wrapper_class(source.kind);
      if (wrapper_class.empty())
      {
        return fail(ErrorCode::unsupported_feature,
                    "lambda return cannot be boxed");
      }
      auto compatible = classes_.is_assignable(wrapper_class,
                                               target.class_name);
      if (!compatible)
        return std::unexpected(compatible.error());
      if (!*compatible)
      {
        return fail(ErrorCode::unsupported_feature,
                    "boxed lambda return is incompatible with target type");
      }
      auto wrapper_root = allocate_pinned_instance(wrapper_class);
      if (!wrapper_root)
        return std::unexpected(wrapper_root.error());
      auto wrapper = wrapper_root->get();
      if (!wrapper)
        return std::unexpected(wrapper.error());
      invocation.return_override = Value::from_reference(*wrapper);
      invocation.return_override_boxes_result = true;
      return {};
    };

    if (binding.implementation_kind == 8U)
    {
      if (!constructor_receiver.has_value() ||
          constructor_receiver->is_null())
      {
        return fail(ErrorCode::invalid_argument,
                    "constructor lambda has no allocated receiver");
      }
      auto target = classes_.resolve_declared_method(
          binding.implementation.owner,
          binding.implementation.name,
          binding.implementation.descriptor);
      if (!target)
        return std::unexpected(target.error());
      std::vector<Value> constructor_arguments;
      constructor_arguments.reserve(combined.size() + 1U);
      constructor_arguments.push_back(
          Value::from_reference(*constructor_receiver));
      constructor_arguments.insert(constructor_arguments.end(),
                                   combined.begin(),
                                   combined.end());
      auto invocation = prepare_invocation(std::move(*target),
                                           constructor_arguments,
                                           true);
      if (!invocation)
        return std::unexpected(invocation.error());
      invocation->return_override = Value::from_reference(
          *constructor_receiver);
      return invocation;
    }

    Result<ResolvedMethod> target = fail(ErrorCode::internal_error,
                                         "lambda target was not resolved");
    bool has_receiver = false;
    if (binding.implementation_kind == 6U)
    {
      target = classes_.resolve_method(binding.implementation.owner,
                                       binding.implementation.name,
                                       binding.implementation.descriptor);
    }
    else
    {
      if (combined.empty())
      {
        return fail(ErrorCode::invalid_argument,
                    "instance lambda target has no receiver");
      }
      auto target_receiver = combined.front().as_reference();
      if (!target_receiver)
        return std::unexpected(target_receiver.error());
      if (target_receiver->is_null())
      {
        return fail_java("java/lang/NullPointerException",
                         "instance lambda target receiver is null");
      }
      auto runtime_class = heap_.class_name(*target_receiver);
      if (!runtime_class)
        return std::unexpected(runtime_class.error());
      auto compatible = classes_.is_assignable(*runtime_class,
                                               binding.implementation.owner);
      if (!compatible)
        return std::unexpected(compatible.error());
      if (!*compatible)
      {
        return fail_java("java/lang/ClassCastException",
                         "lambda target receiver is incompatible with method handle");
      }
      if (binding.implementation_kind == 7U)
      {
        target = classes_.resolve_declared_method(
            binding.implementation.owner,
            binding.implementation.name,
            binding.implementation.descriptor);
      }
      else
      {
        target = classes_.resolve_method(*runtime_class,
                                         binding.implementation.name,
                                         binding.implementation.descriptor);
      }
      has_receiver = true;
    }
    if (!target)
      return std::unexpected(target.error());
    auto invocation = prepare_invocation(std::move(*target), combined, has_receiver);
    if (!invocation)
      return std::unexpected(invocation.error());
    auto configured = configure_lambda_return(*invocation);
    if (!configured)
      return std::unexpected(configured.error());
    return invocation;
  }

  Result<Value> Machine::cast_lambda_reference(Value value,
                                                std::string_view target_class)
  {
    if (value.kind() != ValueKind::reference)
    {
      return fail_java("java/lang/ClassCastException",
                       "lambda return value is not an object");
    }
    auto reference = value.as_reference();
    if (!reference)
      return std::unexpected(reference.error());
    if (reference->is_null() || target_class == "java/lang/Object")
      return value;
    auto runtime_class = heap_.class_name(*reference);
    if (!runtime_class)
      return std::unexpected(runtime_class.error());
    auto compatible = classes_.is_assignable(*runtime_class, target_class);
    if (!compatible)
      return std::unexpected(compatible.error());
    if (!*compatible)
    {
      return fail_java("java/lang/ClassCastException",
                       "lambda return value is incompatible with instantiated type");
    }
    return value;
  }

  Result<Value> Machine::unbox_lambda_value(Value value,
                                             JavaTypeKind target_kind)
  {
    if (value.kind() != ValueKind::reference)
    {
      return fail_java("java/lang/ClassCastException",
                       "lambda unboxing value is not an object");
    }
    auto reference = value.as_reference();
    if (!reference)
      return std::unexpected(reference.error());
    if (reference->is_null())
    {
      return fail_java("java/lang/NullPointerException",
                       "lambda unboxing value is null");
    }
    auto wrapper_class = heap_.class_name(*reference);
    if (!wrapper_class)
      return std::unexpected(wrapper_class.error());
    const auto source_kind = wrapper_primitive_kind(*wrapper_class);
    if (!source_kind.has_value() ||
        !primitive_widening_allowed(*source_kind, target_kind))
    {
      return fail_java("java/lang/ClassCastException",
                       "lambda value cannot be unboxed to target primitive");
    }
    auto payload = heap_.field(*reference, 0U);
    if (!payload)
      return std::unexpected(payload.error());
    return widen_primitive_value(*payload, *source_kind, target_kind);
  }

  Result<std::optional<Value>> Machine::try_tiled_alpha_collision_intrinsic(
      const ResolvedMethod& method,
      i32 x,
      i32 y)
  {
    if (!specialized_intrinsics_requested() || method.owner == nullptr ||
        method.method == nullptr || !method.method->code.has_value() ||
        !matches_tiled_alpha_collision_scan(*method.method))
    {
      return std::optional<Value>{};
    }

    auto cached = tiled_alpha_collision_intrinsics_.find(method.method);
    if (cached == tiled_alpha_collision_intrinsics_.end())
    {
      std::optional<TiledAlphaCollisionIntrinsic> resolved;
      u32 resolution_stage = 0U;
      const auto& owner = *method.owner;
      const auto& code = method.method->code->bytecode;
      const auto same_member = [](const classfile::MemberReference& left,
                                  const classfile::MemberReference& right) {
        return left.kind == right.kind && left.owner == right.owner &&
            left.name == right.name && left.descriptor == right.descriptor;
      };

      auto maps = owner.member_reference(bytecode_cp_index(code, 3U));
      auto size = owner.member_reference(bytecode_cp_index(code, 6U));
      auto element_at = owner.member_reference(bytecode_cp_index(code, 16U));
      auto tile_class = owner.class_name_constant(bytecode_cp_index(code, 19U));
      auto tile_x = owner.member_reference(bytecode_cp_index(code, 28U));
      auto tile_y = owner.member_reference(bytecode_cp_index(code, 33U));
      auto tile_width = owner.member_reference(bytecode_cp_index(code, 38U));
      auto tile_height = owner.member_reference(bytecode_cp_index(code, 43U));
      auto rect = owner.member_reference(bytecode_cp_index(code, 46U));
      auto collision = owner.member_reference(bytecode_cp_index(code, 68U));

      const bool scan_refs_valid =
          maps && size && element_at && tile_class && tile_x && tile_y &&
          tile_width && tile_height && rect && collision &&
          maps->kind == classfile::ConstantKind::field_ref &&
          maps->descriptor == "Ljava/util/Vector;" &&
          size->kind == classfile::ConstantKind::method_ref &&
          size->owner == "java/util/Vector" && size->name == "size" &&
          size->descriptor == "()I" &&
          element_at->kind == classfile::ConstantKind::method_ref &&
          element_at->owner == "java/util/Vector" &&
          element_at->name == "elementAt" &&
          element_at->descriptor == "(I)Ljava/lang/Object;" &&
          tile_x->kind == classfile::ConstantKind::field_ref &&
          tile_y->kind == classfile::ConstantKind::field_ref &&
          tile_width->kind == classfile::ConstantKind::field_ref &&
          tile_height->kind == classfile::ConstantKind::field_ref &&
          tile_x->owner == *tile_class && tile_y->owner == *tile_class &&
          tile_width->owner == *tile_class &&
          tile_height->owner == *tile_class &&
          tile_x->descriptor == "I" && tile_y->descriptor == "I" &&
          tile_width->descriptor == "I" && tile_height->descriptor == "I" &&
          rect->kind == classfile::ConstantKind::method_ref &&
          rect->descriptor == "(IIIIII)Z" &&
          collision->kind == classfile::ConstantKind::method_ref &&
          collision->owner == *tile_class &&
          collision->descriptor == "(II)Z";

      if (scan_refs_valid)
      {
        resolution_stage = 1U;
        auto rect_target = classes_.resolve_method(
            rect->owner, rect->name, rect->descriptor);
        auto collision_target = classes_.resolve_method(
            collision->owner, collision->name, collision->descriptor);
        if (rect_target && collision_target &&
            rect_target->owner != nullptr && rect_target->method != nullptr &&
            collision_target->owner != nullptr &&
            collision_target->method != nullptr &&
            matches_rect_contains_intrinsic(*rect_target->method) &&
            matches_pixel_collision_intrinsic(
                *collision_target->owner, *collision_target->method))
        {
          resolution_stage = 2U;
          const auto& collision_code =
              collision_target->method->code->bytecode;
          auto collision_width = collision_target->owner->member_reference(
              bytecode_cp_index(collision_code, 10U));
          auto collision_height = collision_target->owner->member_reference(
              bytecode_cp_index(collision_code, 18U));
          auto silk = collision_target->owner->member_reference(
              bytecode_cp_index(collision_code, 27U));
          auto get_pixel = collision_target->owner->member_reference(
              bytecode_cp_index(collision_code, 36U));
          auto land = collision_target->owner->member_reference(
              bytecode_cp_index(collision_code, 39U));

          if (collision_width && collision_height && silk && get_pixel && land &&
              same_member(*collision_width, *tile_width) &&
              same_member(*collision_height, *tile_height) &&
              silk->kind == classfile::ConstantKind::field_ref &&
              silk->owner == *tile_class && silk->descriptor == "Z" &&
              get_pixel->kind == classfile::ConstantKind::method_ref &&
              get_pixel->owner == *tile_class &&
              get_pixel->descriptor == "(II)I" &&
              land->kind == classfile::ConstantKind::method_ref &&
              land->descriptor == "(I)Z")
          {
            auto get_pixel_target = classes_.resolve_method(
                get_pixel->owner, get_pixel->name, get_pixel->descriptor);
            auto land_target = classes_.resolve_method(
                land->owner, land->name, land->descriptor);
            if (get_pixel_target && land_target &&
                get_pixel_target->owner != nullptr &&
                get_pixel_target->method != nullptr &&
                land_target->owner != nullptr && land_target->method != nullptr &&
                matches_pixel_getter_intrinsic(*get_pixel_target->method) &&
                matches_alpha_land_intrinsic(
                    *land_target->owner, *land_target->method))
            {
              resolution_stage = 3U;
              const auto& getter_code =
                  get_pixel_target->method->code->bytecode;
              auto pixels = get_pixel_target->owner->member_reference(
                  bytecode_cp_index(getter_code, 1U));
              auto getter_width = get_pixel_target->owner->member_reference(
                  bytecode_cp_index(getter_code, 6U));
              if (pixels && getter_width &&
                  pixels->kind == classfile::ConstantKind::field_ref &&
                  pixels->owner == *tile_class && pixels->descriptor == "[I" &&
                  same_member(*getter_width, *tile_width))
              {
                auto maps_field = states_.resolve_field(
                    maps->owner, maps->name, maps->descriptor, true);
                auto x_field = states_.resolve_field(
                    tile_x->owner, tile_x->name, tile_x->descriptor, false);
                auto y_field = states_.resolve_field(
                    tile_y->owner, tile_y->name, tile_y->descriptor, false);
                auto width_field = states_.resolve_field(
                    tile_width->owner, tile_width->name,
                    tile_width->descriptor, false);
                auto height_field = states_.resolve_field(
                    tile_height->owner, tile_height->name,
                    tile_height->descriptor, false);
                auto silk_field = states_.resolve_field(
                    silk->owner, silk->name, silk->descriptor, false);
                auto pixels_field = states_.resolve_field(
                    pixels->owner, pixels->name, pixels->descriptor, false);
                if (maps_field && x_field && y_field && width_field &&
                    height_field && silk_field && pixels_field)
                {
                  resolution_stage = 4U;
                  resolved = TiledAlphaCollisionIntrinsic {
                      .tiles = std::move(*maps_field),
                      .tile_x = std::move(*x_field),
                      .tile_y = std::move(*y_field),
                      .tile_width = std::move(*width_field),
                      .tile_height = std::move(*height_field),
                      .tile_silk_collision = std::move(*silk_field),
                      .tile_pixels = std::move(*pixels_field),
                      .tile_class = *tile_class,
                  };
                }
              }
            }
          }
        }
      }
      if (const char* trace_value = std::getenv("PHONEME_JIT_TRACE");
          trace_value != nullptr && *trace_value != '\0' &&
          std::string_view(trace_value) != "0")
      {
        std::fprintf(stderr,
                     "[phoneMEJIT] tiled-alpha-intrinsic %s.%s%s stage=%u enabled=%d\n",
                     method.owner->name().c_str(),
                     method.method->name.c_str(),
                     method.method->descriptor.c_str(),
                     static_cast<unsigned>(resolution_stage),
                     resolved.has_value() ? 1 : 0);
      }
      cached = tiled_alpha_collision_intrinsics_.emplace(
          method.method, std::move(resolved)).first;
    }

    if (!cached->second.has_value()) return std::optional<Value>{};
    const TiledAlphaCollisionIntrinsic& intrinsic = *cached->second;
    auto tiles_value = states_.static_field(intrinsic.tiles);
    if (!tiles_value) return std::unexpected(tiles_value.error());
    auto tiles = tiles_value->as_reference();
    if (!tiles || tiles->is_null()) return std::optional<Value>{};

    // Built-in java.util.Vector layout: Object[] elementData, int elementCount.
    auto data_value = heap_.vm_field(*tiles, 0U);
    auto count_value = heap_.vm_field(*tiles, 1U);
    if (!data_value || !count_value) return std::optional<Value>{};
    auto data = data_value->as_reference();
    auto count = count_value->as_int();
    if (!data || data->is_null() || !count || *count < 0)
      return std::optional<Value>{};

    const auto wrapping_add = [](i32 left, i32 right) noexcept {
      return static_cast<i32>(static_cast<u32>(left) +
                              static_cast<u32>(right));
    };
    const auto wrapping_sub = [](i32 left, i32 right) noexcept {
      return static_cast<i32>(static_cast<u32>(left) -
                              static_cast<u32>(right));
    };
    const auto int_field = [this](ObjectRef object,
                                  const FieldLocation& field)
        -> std::optional<i32>
    {
      auto value = heap_.vm_field(object, field.index);
      if (!value) return std::nullopt;
      auto integer = value->as_int();
      if (!integer) return std::nullopt;
      return *integer;
    };

    for (i32 index = 0; index < *count; ++index)
    {
      auto tile_value = heap_.vm_array_element_snapshot(
          *data, static_cast<usize>(index));
      if (!tile_value || tile_value->kind != HeapArrayKind::reference)
        return std::optional<Value>{};
      auto tile = tile_value->value.as_reference();
      if (!tile || tile->is_null()) return std::optional<Value>{};
      auto runtime_class = heap_.vm_class_name_view(*tile);
      if (!runtime_class) return std::optional<Value>{};
      if (*runtime_class != intrinsic.tile_class)
      {
        auto compatible = classes_.is_assignable(
            *runtime_class, intrinsic.tile_class);
        if (!compatible || !*compatible) return std::optional<Value>{};
      }

      const auto tile_x = int_field(*tile, intrinsic.tile_x);
      const auto tile_y = int_field(*tile, intrinsic.tile_y);
      const auto width = int_field(*tile, intrinsic.tile_width);
      const auto height = int_field(*tile, intrinsic.tile_height);
      if (!tile_x || !tile_y || !width || !height)
        return std::optional<Value>{};
      const i32 right = wrapping_add(*tile_x, *width);
      const i32 bottom = wrapping_add(*tile_y, *height);
      if (x < *tile_x || x >= right || y < *tile_y || y >= bottom)
        continue;

      const i32 local_x = wrapping_sub(x, *tile_x);
      const i32 local_y = wrapping_sub(y, *tile_y);
      if (local_x < 0 || local_y < 0 || local_x >= *width ||
          local_y >= *height)
      {
        return std::optional<Value>{};
      }
      const auto silk = int_field(*tile, intrinsic.tile_silk_collision);
      if (!silk) return std::optional<Value>{};
      if (*silk != 0) return std::optional<Value>(Value::from_int(1));

      auto pixels_value = heap_.vm_field(*tile, intrinsic.tile_pixels.index);
      if (!pixels_value) return std::optional<Value>{};
      auto pixels = pixels_value->as_reference();
      if (!pixels || pixels->is_null()) return std::optional<Value>{};
      const i64 pixel_index_64 =
          static_cast<i64>(local_y) * static_cast<i64>(*width) +
          static_cast<i64>(local_x);
      if (pixel_index_64 < 0 ||
          static_cast<u64>(pixel_index_64) >
              static_cast<u64>(std::numeric_limits<usize>::max()))
      {
        return std::optional<Value>{};
      }
      auto pixel = heap_.vm_array_element_snapshot(
          *pixels, static_cast<usize>(pixel_index_64));
      if (!pixel || pixel->kind != HeapArrayKind::integer)
        return std::optional<Value>{};
      auto color = pixel->value.as_int();
      if (!color) return std::optional<Value>{};
      return std::optional<Value>(Value::from_int(
          (static_cast<u32>(*color) & 0xFF00'0000U) != 0U ? 1 : 0));
    }
    return std::optional<Value>(Value::from_int(0));
  }

  Result<std::optional<Value>> Machine::try_projectile_collision_intrinsic(
      const ResolvedMethod& method,
      i32 x,
      i32 y,
      ObjectRef shooter)
  {
    if (!specialized_intrinsics_requested() || method.owner == nullptr ||
        method.method == nullptr || !method.method->code.has_value() ||
        !matches_projectile_collision_scan(*method.method))
    {
      return std::optional<Value>{};
    }

    auto cached = projectile_collision_intrinsics_.find(method.method);
    if (cached == projectile_collision_intrinsics_.end())
    {
      std::optional<ProjectileCollisionIntrinsic> resolved;
      const auto& owner = *method.owner;
      const auto& code = method.method->code->bytecode;
      auto players = owner.member_reference(bytecode_cp_index(code, 0U));
      auto damageable = owner.member_reference(bytecode_cp_index(code, 38U));
      auto team = owner.member_reference(bytecode_cp_index(code, 50U));
      auto hp = owner.member_reference(bytecode_cp_index(code, 62U));
      auto get_state = owner.member_reference(bytecode_cp_index(code, 70U));
      auto invisible = owner.member_reference(bytecode_cp_index(code, 79U));
      auto hitbox = owner.member_reference(bytecode_cp_index(code, 89U));

      if (players && damageable && team && hp && get_state && invisible && hitbox &&
          players->kind == classfile::ConstantKind::field_ref &&
          players->descriptor.size() > 3U &&
          players->descriptor.starts_with("[L") &&
          players->descriptor.back() == ';' &&
          damageable->kind == classfile::ConstantKind::method_ref &&
          damageable->descriptor == "(Lplayer/CPlayer;)Z" &&
          team->kind == classfile::ConstantKind::field_ref &&
          team->descriptor == "Z" &&
          hp->kind == classfile::ConstantKind::field_ref && hp->descriptor == "I" &&
          get_state->kind == classfile::ConstantKind::method_ref &&
          get_state->descriptor == "()B" &&
          invisible->kind == classfile::ConstantKind::field_ref &&
          invisible->descriptor == "Z" &&
          hitbox->kind == classfile::ConstantKind::method_ref &&
          hitbox->descriptor == "(Lplayer/CPlayer;II)Z")
      {
        const std::string player_class = players->descriptor.substr(
            2U, players->descriptor.size() - 3U);
        if (team->owner == player_class && hp->owner == player_class &&
            invisible->owner == player_class)
        {
          auto damageable_target = classes_.resolve_method(
              damageable->owner, damageable->name, damageable->descriptor);
          auto state_target = classes_.resolve_method(
              get_state->owner, get_state->name, get_state->descriptor);
          auto hitbox_target = classes_.resolve_method(
              hitbox->owner, hitbox->name, hitbox->descriptor);
          if (damageable_target && state_target && hitbox_target &&
              damageable_target->owner != nullptr &&
              damageable_target->method != nullptr &&
              state_target->owner != nullptr && state_target->method != nullptr &&
              hitbox_target->owner != nullptr && hitbox_target->method != nullptr &&
              matches_damageable_target_intrinsic(*damageable_target->method) &&
              matches_state_getter_intrinsic(*state_target->method) &&
              matches_non_boss_projectile_hitbox_intrinsic(
                  *hitbox_target->method))
          {
            const auto& damageable_code =
                damageable_target->method->code->bytecode;
            const auto& state_code = state_target->method->code->bytecode;
            const auto& hitbox_code = hitbox_target->method->code->bytecode;
            auto damageable_boss = damageable_target->owner->class_name_constant(
                bytecode_cp_index(damageable_code, 1U));
            auto hitbox_boss = hitbox_target->owner->class_name_constant(
                bytecode_cp_index(hitbox_code, 1U));
            auto state_field_ref = state_target->owner->member_reference(
                bytecode_cp_index(state_code, 1U));
            auto x_ref = hitbox_target->owner->member_reference(
                bytecode_cp_index(hitbox_code, 563U));
            auto y_ref = hitbox_target->owner->member_reference(
                bytecode_cp_index(hitbox_code, 571U));
            if (damageable_boss && hitbox_boss &&
                *damageable_boss == *hitbox_boss && state_field_ref && x_ref && y_ref &&
                state_field_ref->kind == classfile::ConstantKind::field_ref &&
                state_field_ref->owner == player_class &&
                state_field_ref->descriptor == "B" &&
                x_ref->kind == classfile::ConstantKind::field_ref &&
                x_ref->owner == player_class && x_ref->descriptor == "I" &&
                y_ref->kind == classfile::ConstantKind::field_ref &&
                y_ref->owner == player_class && y_ref->descriptor == "I")
            {
              auto players_field = states_.resolve_field(
                  players->owner, players->name, players->descriptor, true);
              auto team_field = states_.resolve_field(
                  team->owner, team->name, team->descriptor, false);
              auto hp_field = states_.resolve_field(
                  hp->owner, hp->name, hp->descriptor, false);
              auto invisible_field = states_.resolve_field(
                  invisible->owner, invisible->name, invisible->descriptor, false);
              auto state_field = states_.resolve_field(
                  state_field_ref->owner, state_field_ref->name,
                  state_field_ref->descriptor, false);
              auto x_field = states_.resolve_field(
                  x_ref->owner, x_ref->name, x_ref->descriptor, false);
              auto y_field = states_.resolve_field(
                  y_ref->owner, y_ref->name, y_ref->descriptor, false);
              if (players_field && team_field && hp_field && invisible_field &&
                  state_field && x_field && y_field)
              {
                resolved = ProjectileCollisionIntrinsic {
                    .players = std::move(*players_field),
                    .team = std::move(*team_field),
                    .hp = std::move(*hp_field),
                    .invisible = std::move(*invisible_field),
                    .state = std::move(*state_field),
                    .x = std::move(*x_field),
                    .y = std::move(*y_field),
                    .player_class = player_class,
                    .boss_class = *damageable_boss,
                };
              }
            }
          }
        }
      }
      cached = projectile_collision_intrinsics_.emplace(
          method.method, std::move(resolved)).first;
    }

    if (!cached->second.has_value()) return std::optional<Value>{};
    const ProjectileCollisionIntrinsic& intrinsic = *cached->second;
    auto players_value = states_.static_field(intrinsic.players);
    if (!players_value) return std::unexpected(players_value.error());
    auto players = players_value->as_reference();
    if (!players) return std::unexpected(players.error());
    if (players->is_null())
      return std::optional<Value>(Value::from_int(0));

    auto player_count = heap_.vm_array_length(*players);
    if (!player_count) return std::optional<Value>{};
    const auto int_field = [this](ObjectRef object,
                                  const FieldLocation& field)
        -> std::optional<i32>
    {
      auto value = heap_.vm_field(object, field.index);
      if (!value) return std::nullopt;
      auto integer = value->as_int();
      if (!integer) return std::nullopt;
      return *integer;
    };
    const auto wrapping_add = [](i32 left, i32 right) noexcept {
      return static_cast<i32>(static_cast<u32>(left) +
                              static_cast<u32>(right));
    };
    const auto wrapping_sub = [](i32 left, i32 right) noexcept {
      return static_cast<i32>(static_cast<u32>(left) -
                              static_cast<u32>(right));
    };

    std::optional<i32> shooter_team;
    if (!shooter.is_null())
    {
      auto shooter_class = heap_.vm_class_name_view(shooter);
      if (!shooter_class) return std::optional<Value>{};
      auto compatible = classes_.is_assignable(
          *shooter_class, intrinsic.player_class);
      if (!compatible || !*compatible) return std::optional<Value>{};
      shooter_team = int_field(shooter, intrinsic.team);
      if (!shooter_team.has_value()) return std::optional<Value>{};
    }

    for (usize index = 0U; index < *player_count; ++index)
    {
      auto target_value = heap_.vm_array_element_snapshot(*players, index);
      if (!target_value || target_value->kind != HeapArrayKind::reference)
        return std::optional<Value>{};
      auto target = target_value->value.as_reference();
      if (!target) return std::optional<Value>{};
      if (target->is_null() || *target == shooter) continue;

      auto target_class = heap_.vm_class_name_view(*target);
      if (!target_class) return std::optional<Value>{};
      if (*target_class != intrinsic.player_class)
      {
        // Boss hitboxes and arbitrary CPlayer subclasses can override state or
        // require gun-specific geometry. Preserve their Java dispatch exactly.
        return std::optional<Value>{};
      }

      const auto target_team = int_field(*target, intrinsic.team);
      const auto hp = int_field(*target, intrinsic.hp);
      const auto state = int_field(*target, intrinsic.state);
      const auto invisible = int_field(*target, intrinsic.invisible);
      if (!target_team || !hp || !state || !invisible)
        return std::optional<Value>{};
      if (shooter_team.has_value() && *target_team == *shooter_team)
        continue;
      if (*hp <= 0 || *state == 5 || *invisible != 0) continue;

      const auto target_x = int_field(*target, intrinsic.x);
      const auto target_y = int_field(*target, intrinsic.y);
      if (!target_x || !target_y) return std::optional<Value>{};
      const i32 left = wrapping_sub(*target_x, 12);
      const i32 top = wrapping_sub(*target_y, 26);
      const i32 right = wrapping_add(*target_x, 12);
      const i32 bottom = wrapping_add(*target_y, 2);
      if (x >= left && x <= right && y >= top && y <= bottom)
        return std::optional<Value>(Value::from_int(1));
    }
    return std::optional<Value>(Value::from_int(0));
  }

  void Machine::prune_lambda_bindings()
  {
    for (auto iterator = lambda_bindings_.begin();
         iterator != lambda_bindings_.end();)
    {
      if (!heap_.class_name(ObjectRef{iterator->first}))
      {
        iterator = lambda_bindings_.erase(iterator);
      }
      else
      {
        ++iterator;
      }
    }
  }

  Result<ObjectRef> Machine::create_throwable(std::string_view class_name)
  {
    if (class_name == "java/lang/OutOfMemoryError" &&
        !emergency_out_of_memory_error_.is_null())
    {
      auto emergency_class = heap_.class_name(emergency_out_of_memory_error_);
      if (emergency_class && *emergency_class == class_name)
        return emergency_out_of_memory_error_;
    }
    auto throwable_class = classes_.load(class_name);
    if (!throwable_class)
    {
      return std::unexpected(throwable_class.error());
    }
    auto assignable = classes_.is_assignable((*throwable_class)->name(),
                                             "java/lang/Throwable");
    if (!assignable)
    {
      return std::unexpected(assignable.error());
    }
    if (!*assignable)
    {
      return fail(ErrorCode::invalid_argument,
                  "requested implicit exception class is not Throwable");
    }
    return states_.allocate_instance(heap_, (*throwable_class)->name());
  }

  Result<ObjectRef> Machine::create_throwable(
      std::string_view class_name,
      std::string_view message)
  {
    auto throwable = create_throwable(class_name);
    if (!throwable || message.empty() ||
        class_name == "java/lang/OutOfMemoryError")
    {
      return throwable;
    }
    auto message_string = intern_string(message);
    if (!message_string)
    {
      return std::unexpected(message_string.error());
    }
    auto message_stored = heap_.set_field(
        *throwable, 0U, Value::from_reference(*message_string));
    if (!message_stored)
    {
      return std::unexpected(message_stored.error());
    }
    auto cause_stored = heap_.set_field(
        *throwable, 1U, Value::from_reference({}));
    if (!cause_stored)
    {
      return std::unexpected(cause_stored.error());
    }
    auto cause_initialized = heap_.set_field(
        *throwable, 2U, Value::from_int(0));
    if (!cause_initialized)
    {
      return std::unexpected(cause_initialized.error());
    }
    return *throwable;
  }

  Result<Value> Machine::load_constant(const classfile::ClassFile &owner,
                                       u16 index,
                                       bool category_two_only)
  {
    auto constant = owner.constant(index);
    if (!constant)
    {
      return std::unexpected(constant.error());
    }
    if ((*constant)->kind == classfile::ConstantKind::class_ref)
    {
      if (category_two_only)
      {
        return fail(ErrorCode::malformed_class,
                    "ldc2_w cannot load a Java Class");
      }
      auto class_name = owner.class_name_constant(index);
      if (!class_name)
      {
        return std::unexpected(class_name.error());
      }
      auto mirror = class_mirror(*class_name);
      if (!mirror)
        return std::unexpected(mirror.error());
      return Value::from_reference(*mirror);
    }
    if ((*constant)->kind == classfile::ConstantKind::string_ref)
    {
      if (category_two_only)
      {
        return fail(ErrorCode::malformed_class,
                    "ldc2_w cannot load a Java String");
      }
      auto encoded = owner.string_constant(index);
      if (!encoded)
      {
        return std::unexpected(encoded.error());
      }
      auto reference = intern_string(*encoded);
      if (!reference)
      {
        return std::unexpected(reference.error());
      }
      return Value::from_reference(*reference);
    }
    return constant_value(owner, index, category_two_only);
  }

  Result<ExecutionResult> Machine::execute(
      Invocation invocation,
      u64 instruction_budget,
      InstructionBudgetMode budget_mode)
  {
    if (g_execution_machine != nullptr && g_execution_machine != this)
    {
      return fail(ErrorCode::invalid_state,
                  "a host thread cannot execute two Machine instances recursively");
    }
    execution_mutex_.lock();
    g_execution_machine = this;
    ++g_execution_lock_depth;
    scheduler_.set_current_state(JavaThreadState::running);
    if (g_execution_lock_depth == 1U)
      scheduler_.begin_execution_slice();
    const u32 invocation_depth = g_execution_lock_depth;
    PerformanceCounters::observe_java_call_depth(invocation_depth);
    const JavaThreadId invocation_thread = scheduler_.current_thread_id();
    const HeapAccessContext previous_heap_context =
        current_heap_access_context();
    usize current_heap_access_pc = 0U;
    u64 accounted_instructions = 0U;
    auto cleanup = [invocation_depth,
                    invocation_thread,
                    previous_heap_context,
                    &accounted_instructions](Machine* machine) {
      machine->scheduler_.add_current_executed_instructions(
          accounted_instructions);
      machine->clear_execution_roots(invocation_depth);
      set_heap_access_context(previous_heap_context);
      if (g_execution_lock_depth == 1U)
        machine->monitors_.release_all(invocation_thread);
      --g_execution_lock_depth;
      machine->execution_mutex_.unlock();
      if (g_execution_lock_depth == 0U)
      {
        g_execution_machine = nullptr;
        PerformanceCounters::flush_thread_local();
      }
    };
    std::unique_ptr<Machine, decltype(cleanup)> execution_scope(this, cleanup);

    scheduler_.set_current_pending_exception(std::nullopt);
    std::vector<ObjectRef> invocation_roots;
    invocation_roots.reserve(invocation.arguments.size() + 1U);
    invocation.arguments.append_reference_roots(invocation_roots);
    if (invocation.return_override.has_value() &&
        invocation.return_override->kind() == ValueKind::reference)
    {
      auto reference = invocation.return_override->as_reference();
      if (reference && !reference->is_null())
        invocation_roots.push_back(*reference);
    }
    publish_execution_roots(invocation_depth, invocation_roots);

    if (invocation.method.method == nullptr)
    {
      return fail(ErrorCode::internal_error,
                  "VM invocation has no resolved method");
    }
    set_heap_access_context(HeapAccessContext {
        .owner = invocation.method.owner != nullptr
            ? invocation.method.owner->name() : std::string_view {},
        .method = invocation.method.method->name,
        .descriptor = invocation.method.method->descriptor,
        .bytecode_pc = 0U,
    });
    if ((invocation.method.method->access_flags & kAccAbstract) != 0U)
    {
      auto throwable = create_throwable("java/lang/AbstractMethodError");
      if (!throwable)
      {
        return std::unexpected(throwable.error());
      }
      scheduler_.set_current_pending_exception(*throwable);
      return ExecutionResult{
          .return_value = std::nullopt,
          .throwable = *throwable,
          .executed_instructions = 0,
      };
    }
    auto root_monitor = acquire_synchronized_monitor(invocation);
    if (!root_monitor)
    {
      return std::unexpected(root_monitor.error());
    }

    const bool has_native_override = invocation.native_method.valid();
    if (has_native_override || !invocation.method.method->code.has_value())
    {
      const auto native_started = std::chrono::steady_clock::now();
      auto native_result = invoke_native(invocation);
      const auto native_duration =
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - native_started);
      if (native_duration >= std::chrono::milliseconds(10) &&
          should_trace_slow_native(invocation.method.owner->name(),
                                   invocation.method.method->name))
      {
        vm_trace("native",
                 "slow-call java=%u target=%s.%s%s duration_us=%lld ok=%d",
                 static_cast<unsigned>(scheduler_.current_thread_id()),
                 invocation.method.owner->name().c_str(),
                 invocation.method.method->name.c_str(),
                 invocation.method.method->descriptor.c_str(),
                 static_cast<long long>(native_duration.count()),
                 native_result.has_value() ? 1 : 0);
      }
      auto released = release_synchronized_monitor(*root_monitor);
      if (!released)
        return std::unexpected(released.error());
      if (!native_result)
      {
        if (native_result.error().code == ErrorCode::java_exception)
        {
          if (native_result.error().java_exception_class.empty())
          {
            return fail(ErrorCode::internal_error,
                        "native Java exception has no class name");
          }
          auto throwable = create_throwable(
              native_result.error().java_exception_class,
              native_result.error().message);
          if (!throwable)
            return std::unexpected(throwable.error());
          scheduler_.set_current_pending_exception(*throwable);
          return ExecutionResult{
              .return_value = std::nullopt,
              .throwable = *throwable,
              .executed_instructions = 0,
          };
        }
        if (native_result.error().code == ErrorCode::unsupported_feature &&
            (invocation.method.method->access_flags & kAccNative) != 0U)
        {
          auto throwable = create_throwable(
              "java/lang/UnsatisfiedLinkError");
          if (!throwable)
          {
            return std::unexpected(throwable.error());
          }
          scheduler_.set_current_pending_exception(*throwable);
          return ExecutionResult{
              .return_value = std::nullopt,
              .throwable = *throwable,
              .executed_instructions = 0,
          };
        }
        return std::unexpected(native_result.error());
      }
      std::optional<Value> completed_return = *native_result;
      if (invocation.return_reference_cast_target.has_value())
      {
        if (!completed_return.has_value())
        {
          return fail(ErrorCode::invalid_state,
                      "cast lambda native return has no reference value");
        }
        auto casted = cast_lambda_reference(
            *completed_return, *invocation.return_reference_cast_target);
        if (!casted)
          return std::unexpected(casted.error());
        completed_return = *casted;
      }
      if (invocation.return_unboxing_target.has_value())
      {
        if (!completed_return.has_value())
        {
          return fail(ErrorCode::invalid_state,
                      "unboxed lambda native return has no reference value");
        }
        auto unboxed = unbox_lambda_value(
            *completed_return, *invocation.return_unboxing_target);
        if (!unboxed)
          return std::unexpected(unboxed.error());
        completed_return = *unboxed;
      }
      if (invocation.return_widening_source.has_value() &&
          invocation.return_widening_target.has_value())
      {
        if (!completed_return.has_value())
        {
          return fail(ErrorCode::invalid_state,
                      "widened lambda native return has no primitive value");
        }
        auto widened = widen_primitive_value(
            *completed_return,
            *invocation.return_widening_source,
            *invocation.return_widening_target);
        if (!widened)
          return std::unexpected(widened.error());
        completed_return = *widened;
      }
      if (invocation.return_override.has_value())
      {
        if (invocation.return_override_boxes_result)
        {
          if (!completed_return.has_value())
          {
            return fail(ErrorCode::invalid_state,
                        "boxed lambda native return has no primitive value");
          }
          auto wrapper = invocation.return_override->as_reference();
          if (!wrapper || wrapper->is_null())
          {
            return fail(ErrorCode::internal_error,
                        "boxed lambda native return has no wrapper object");
          }
          auto stored = heap_.vm_set_field(*wrapper, 0U, *completed_return);
          if (!stored)
            return std::unexpected(stored.error());
        }
        completed_return = invocation.return_override;
      }
      if (invocation.discard_return_value)
        completed_return = std::nullopt;
      return ExecutionResult{
          .return_value = completed_return,
          .throwable = std::nullopt,
          .executed_instructions = 0,
      };
    }

    if (invocation.descriptor == nullptr)
    {
      return fail(ErrorCode::internal_error,
                  "VM invocation has no cached descriptor");
    }
    std::optional<JitDeoptState> root_jit_deopt_state =
        std::move(invocation.resume_jit_deopt_state);
    u64 root_jit_instructions = invocation.resume_jit_instructions;
    u64 root_jit_nested_instructions =
        invocation.resume_jit_nested_instructions;
    if (root_jit_deopt_state.has_value())
    {
      accounted_instructions =
          root_jit_instructions >= root_jit_nested_instructions
              ? root_jit_instructions - root_jit_nested_instructions
              : 0U;
    }
#if !defined(__EMSCRIPTEN__)
    if (const auto root_jit_budget =
            jit_.enabled()
                ? safe_jit_instruction_budget(invocation.method,
                                              instruction_budget)
                : std::nullopt;
        root_jit_budget.has_value() &&
        !root_jit_deopt_state.has_value() &&
        budget_mode != InstructionBudgetMode::progress_watchdog &&
        invocation.method.runtime != nullptr &&
        invocation.method.owner != nullptr &&
        !invocation.return_override.has_value() &&
        !invocation.discard_return_value &&
        !invocation.return_reference_cast_target.has_value() &&
        !invocation.return_unboxing_target.has_value() &&
        !invocation.return_widening_target.has_value())
    {
      JitExecutionContext jit_context{
          .machine = this,
          .owner = invocation.method.owner.get(),
          .method = invocation.method.method,
          .invocation_depth = invocation_depth,
          .base_roots = std::span<const ObjectRef>(
              invocation_roots.data(), invocation_roots.size()),
      };
      if (invocation.method.method->code.has_value())
      {
        jit_context.published_roots.reserve(
            invocation_roots.size() +
            invocation.method.method->code->max_locals +
            invocation.method.method->code->max_stack);
      }
      const JitRuntimeHooks jit_hooks{
          .context = &jit_context,
          .dispatch = &Machine::jit_runtime_dispatch_callback,
          .publish_roots = &Machine::jit_publish_roots_callback,
      };
      auto jitted = jit_.try_execute(
          invocation.method.runtime->id,
          *invocation.method.owner,
          *invocation.method.method,
          *invocation.descriptor,
          invocation.arguments,
          invocation.has_receiver,
          *root_jit_budget,
          jit_hooks,
          invocation.method.owner,
          invocation.method.runtime->verified_frames,
          invocation.method.runtime->descriptor);
      if (!jitted)
      {
        auto released = release_synchronized_monitor(*root_monitor);
        if (!released)
          return std::unexpected(released.error());
        if (jit_instruction_budget_exhausted(jitted.error()))
        {
          PerformanceCounters::record_instruction_budget_exit();
          return std::unexpected(vm_instruction_budget_error(
              invocation.method.owner->name(),
              invocation.method.method->name,
              invocation.method.method->descriptor));
        }
        return std::unexpected(jitted.error());
      }
      if (jitted->has_value())
      {
        const u64 jit_instructions =
            static_cast<u64>((*jitted)->bytecode_instructions);
        accounted_instructions =
            jit_instructions >= jit_context.nested_instructions
                ? jit_instructions - jit_context.nested_instructions
                : 0U;
        if ((*jitted)->deopt_state.has_value())
        {
          root_jit_instructions = jit_instructions;
          root_jit_nested_instructions = jit_context.nested_instructions;
          root_jit_deopt_state = std::move((*jitted)->deopt_state);
        }
        else if ((*jitted)->exception.has_value())
        {
          auto released = release_synchronized_monitor(*root_monitor);
          if (!released)
            return std::unexpected(released.error());
          ObjectRef throwable_reference;
          if (*(*jitted)->exception == JitExceptionKind::runtime_throwable)
          {
            if (!jit_context.pending_throwable.has_value())
            {
              return fail(ErrorCode::internal_error,
                          "JIT runtime exception has no throwable object");
            }
            throwable_reference = *jit_context.pending_throwable;
          }
          else
          {
            auto throwable = create_throwable(
                jit_exception_class(*(*jitted)->exception));
            if (!throwable)
              return std::unexpected(throwable.error());
            throwable_reference = *throwable;
          }
          scheduler_.set_current_pending_exception(throwable_reference);
          return ExecutionResult{
              .return_value = std::nullopt,
              .throwable = throwable_reference,
              .executed_instructions = jit_instructions,
              .exception_context = invocation.method.owner->name() + "." +
                  invocation.method.method->name +
                  invocation.method.method->descriptor +
                  " at bytecode " +
                  std::to_string((*jitted)->exception_bci),
          };
        }
        if (!root_jit_deopt_state.has_value())
        {
          auto released = release_synchronized_monitor(*root_monitor);
          if (!released)
            return std::unexpected(released.error());
          return ExecutionResult{
              .return_value = (*jitted)->return_value,
              .throwable = std::nullopt,
              .executed_instructions = jit_instructions,
          };
        }
      }
    }
#endif
    ExecutionFrameStackLease frame_stack_lease;
    auto& frames = frame_stack_lease.frames();
    auto root_frame = ExecutionFrame::emplace(frames,
                                              std::move(invocation.method),
                                              invocation.descriptor,
                                              invocation.arguments,
                                              invocation.has_receiver);
    if (!root_frame)
    {
      auto released = release_synchronized_monitor(*root_monitor);
      if (!released)
        return std::unexpected(released.error());
      return std::unexpected(root_frame.error());
    }
    if (root_jit_deopt_state.has_value())
    {
      auto restored = (*root_frame)->restore_jit_deopt_state(
          *root_jit_deopt_state);
      if (!restored)
      {
        auto released = release_synchronized_monitor(*root_monitor);
        if (!released)
          return std::unexpected(released.error());
        return std::unexpected(restored.error());
      }
    }
    if (root_monitor->has_value())
    {
      (*root_frame)->set_synchronized_monitor(**root_monitor);
    }
    if (invocation.return_override.has_value())
    {
      (*root_frame)->set_return_override(*invocation.return_override,
                                         invocation.return_override_boxes_result);
    }
    (*root_frame)->set_discard_return_value(invocation.discard_return_value);
    if (invocation.return_reference_cast_target.has_value())
    {
      (*root_frame)->set_return_reference_cast(
          *invocation.return_reference_cast_target);
    }
    if (invocation.return_unboxing_target.has_value())
    {
      (*root_frame)->set_return_unboxing(*invocation.return_unboxing_target);
    }
    if (invocation.return_widening_source.has_value() &&
        invocation.return_widening_target.has_value())
    {
      (*root_frame)->set_return_widening(*invocation.return_widening_source,
                                         *invocation.return_widening_target);
    }
    PerformanceCounters::observe_java_call_depth(frames.size());
    u64 executed = root_jit_instructions;
    u64 separately_accounted_nested_instructions =
        root_jit_nested_instructions;
    u64 watchdog_instructions = 0;
    const u64 progress_total_budget =
        instruction_budget > std::numeric_limits<u64>::max() / 32U
            ? std::numeric_limits<u64>::max()
            : instruction_budget * 32U;
    const auto remaining_execution_budget = [&]() noexcept -> u64
    {
      if (budget_mode != InstructionBudgetMode::progress_watchdog)
      {
        return executed < instruction_budget
            ? instruction_budget - executed
            : 0U;
      }
      const u64 watchdog_remaining = watchdog_instructions < instruction_budget
          ? instruction_budget - watchdog_instructions
          : 0U;
      const u64 total_remaining = executed < progress_total_budget
          ? progress_total_budget - executed
          : 0U;
      return std::min(watchdog_remaining, total_remaining);
    };
    u64 next_scheduler_quantum = kSchedulerQuantum;
    while (next_scheduler_quantum <= executed &&
           next_scheduler_quantum <=
               std::numeric_limits<u64>::max() - kSchedulerQuantum)
    {
      next_scheduler_quantum += kSchedulerQuantum;
    }
    u64 next_garbage_collection_poll = kGarbageCollectionPollInterval;
    while (next_garbage_collection_poll <= executed &&
           next_garbage_collection_poll <=
               std::numeric_limits<u64>::max() -
                   kGarbageCollectionPollInterval)
    {
      next_garbage_collection_poll += kGarbageCollectionPollInterval;
    }
    // Harrier-style coarse maintenance: foreground/background state is only
    // relevant at a safepoint. The old loop performed an atomic foreground
    // load plus interval selection for every bytecode even though maintenance
    // happens only every 1024/256 instructions. Start with an immediate poll,
    // then advance a deadline and refresh host state only at that boundary.
    u64 next_maintenance_poll = executed;
    bool cached_host_foreground = true;
    const classfile::ClassFile* heap_access_owner = nullptr;
    const classfile::Method* heap_access_method = nullptr;
    std::vector<ObjectRef> safepoint_roots;
    safepoint_roots.reserve(256U);
    std::vector<ObjectRef> garbage_collection_roots;
    garbage_collection_roots.reserve(512U);

    const auto publish_active_execution_roots =
        [this, &frames, &safepoint_roots, invocation_depth](
            std::span<const Value> extra_values = {},
            std::optional<Value> extra_value = std::nullopt)
    {
      safepoint_roots.clear();
      safepoint_roots.reserve(
          frames.size() * 8U + extra_values.size() +
          (extra_value.has_value() ? 1U : 0U) + 8U);
      for (const ExecutionFrame& active_frame : frames)
        active_frame.append_reference_roots(safepoint_roots);
      for (const Value value : extra_values)
      {
        if (value.kind() != ValueKind::reference)
          continue;
        const ObjectRef reference = value.reference_unchecked();
        if (!reference.is_null()) safepoint_roots.push_back(reference);
      }
      if (extra_value.has_value() &&
          extra_value->kind() == ValueKind::reference)
      {
        const ObjectRef reference = extra_value->reference_unchecked();
        if (!reference.is_null()) safepoint_roots.push_back(reference);
      }
      exchange_execution_roots(invocation_depth, safepoint_roots);
    };

    const auto publish_compact_execution_roots =
        [this, &frames, &safepoint_roots, invocation_depth](
            const InvocationArguments& extra_values,
            std::optional<Value> extra_value = std::nullopt)
    {
      safepoint_roots.clear();
      safepoint_roots.reserve(
          frames.size() * 8U + extra_values.size() +
          (extra_value.has_value() ? 1U : 0U) + 8U);
      for (const ExecutionFrame& active_frame : frames)
        active_frame.append_reference_roots(safepoint_roots);
      extra_values.append_reference_roots(safepoint_roots);
      if (extra_value.has_value() &&
          extra_value->kind() == ValueKind::reference)
      {
        const ObjectRef reference = extra_value->reference_unchecked();
        if (!reference.is_null()) safepoint_roots.push_back(reference);
      }
      exchange_execution_roots(invocation_depth, safepoint_roots);
    };

    const auto ensure_initialized_from_execution =
        [this, &publish_active_execution_roots](
            std::string_view class_name,
            u64 remaining_budget,
            std::span<const Value> extra_values = {})
        -> Result<std::optional<ObjectRef>>
    {
      // Class initialization enters a recursive Machine::execute invocation.
      // That nested invocation may hit a scheduler quantum or blocking native
      // and release the VM gate. Publish the complete outer interpreter state
      // first so a different Java thread cannot collect references that were
      // created since the previous safepoint. Include operands already popped
      // into C++ argument vectors, such as invokestatic parameters.
      publish_active_execution_roots(extra_values);
      return ensure_initialized(class_name, remaining_budget);
    };

    const auto ensure_initialized_from_compact_execution =
        [this, &publish_compact_execution_roots](
            std::string_view class_name,
            u64 remaining_budget,
            const InvocationArguments& extra_values)
        -> Result<std::optional<ObjectRef>>
    {
      publish_compact_execution_roots(extra_values);
      return ensure_initialized(class_name, remaining_budget);
    };

    const auto collect_active_garbage =
        [this, &frames, &garbage_collection_roots, invocation_depth](
            std::optional<ObjectRef> extra_root = std::nullopt)
        -> Status
    {
      garbage_collection_roots.clear();
      garbage_collection_roots.reserve(
          frames.size() * 8U + interned_strings_.size() +
          class_mirrors_.size() + ui_components_.size() + 16U);
      for (const ExecutionFrame &active_frame : frames)
      {
        active_frame.append_reference_roots(garbage_collection_roots);
      }
      publish_execution_roots(invocation_depth, garbage_collection_roots);
      states_.append_reference_roots(garbage_collection_roots);
      if (!emergency_out_of_memory_error_.is_null())
        garbage_collection_roots.push_back(emergency_out_of_memory_error_);
      for (const auto &[value, reference] : interned_strings_)
      {
        (void)value;
        if (!reference.is_null())
          garbage_collection_roots.push_back(reference);
      }
      for (const auto &[path, reference] : resource_array_cache_)
      {
        (void)path;
        if (!reference.is_null())
          garbage_collection_roots.push_back(reference);
      }
      for (const auto &[class_name, reference] : class_mirrors_)
      {
        (void)class_name;
        if (!reference.is_null())
          garbage_collection_roots.push_back(reference);
      }
      for (const auto &[component_id, reference] : ui_components_)
      {
        (void)component_id;
        if (!reference.is_null())
          garbage_collection_roots.push_back(reference);
      }
      if (canvas_bridge_ != nullptr)
        canvas_bridge_->append_reference_roots(garbage_collection_roots);
      monitors_.append_reference_roots(garbage_collection_roots);
      timers_.append_reference_roots(garbage_collection_roots);
      scheduler_.append_reference_roots(garbage_collection_roots);
      native_roots_.append_reference_roots(garbage_collection_roots);
      if (extra_root.has_value() && !extra_root->is_null())
      {
        garbage_collection_roots.push_back(*extra_root);
      }
      auto collected = heap_.collect(garbage_collection_roots);
      if (collected)
      {
        prune_lambda_bindings();
        graphics_.prune([this](u64 object_key) {
          return heap_.vm_class_name_view(ObjectRef{object_key}).has_value();
        });
      }
      return collected;
    };

    const auto allocate_instance_with_gc =
        [this, &collect_active_garbage](std::string_view class_name)
        -> Result<ObjectRef>
    {
      const auto allocate = [this, class_name]() -> Result<ObjectRef>
      {
        auto class_layout = states_.layout(class_name);
        if (!class_layout)
          return std::unexpected(class_layout.error());
        if (!(*class_layout)->instantiable)
          return fail(ErrorCode::invalid_argument,
                      "cannot allocate an interface or abstract class");
        return heap_.vm_allocate_object(
            (*class_layout)->class_name,
            (*class_layout)->instance_defaults);
      };
      auto object = allocate();
      if (object || object.error().code != ErrorCode::overflow)
      {
        return object;
      }
      auto collected = collect_active_garbage();
      if (!collected)
        return std::unexpected(collected.error());
      return allocate();
    };

    const auto allocate_raw_object_with_gc =
        [this, &collect_active_garbage](std::string_view class_name,
        usize field_count)
        -> Result<ObjectRef>
    {
      auto object = heap_.allocate_object(std::string(class_name), field_count);
      if (object || object.error().code != ErrorCode::overflow)
      {
        return object;
      }
      auto collected = collect_active_garbage();
      if (!collected)
        return std::unexpected(collected.error());
      return heap_.allocate_object(std::string(class_name), field_count);
    };

    const auto allocate_array_with_gc =
        [this, &collect_active_garbage](std::string_view class_name,
                                        usize length,
                                        Value initial_value,
                                        std::optional<ObjectRef> extra_root =
                                            std::nullopt)
        -> Result<ObjectRef>
    {
      auto array = heap_.vm_allocate_array(class_name, length, initial_value);
      if (array || array.error().code != ErrorCode::overflow)
      {
        return array;
      }
      auto collected = collect_active_garbage(extra_root);
      if (!collected)
        return std::unexpected(collected.error());
      return heap_.vm_allocate_array(class_name, length, initial_value);
    };

    const auto try_trivial_getter_intrinsic =
        [this](const Invocation& candidate)
        -> Result<std::optional<Value>>
    {
      if (!specialized_intrinsics_requested() ||
          candidate.method.owner == nullptr ||
          candidate.method.method == nullptr ||
          !candidate.method.method->code.has_value() ||
          candidate.return_override.has_value() ||
          candidate.discard_return_value ||
          candidate.return_reference_cast_target.has_value() ||
          candidate.return_unboxing_target.has_value() ||
          candidate.return_widening_target.has_value())
      {
        return std::optional<Value>{};
      }

      const classfile::Method* method = candidate.method.method;
      auto cached = trivial_getter_intrinsics_.find(method);
      if (cached == trivial_getter_intrinsics_.end())
      {
        std::optional<TrivialGetterIntrinsic> resolved;
        const auto& code = method->code->bytecode;
        const bool method_static = (method->access_flags & kAccStatic) != 0U;
        const bool instance_shape = !method_static && candidate.has_receiver &&
            candidate.arguments.size() == 1U && code.size() == 5U &&
            code[0U] == 0x2AU && code[1U] == 0xB4U;
        const bool static_shape = method_static && !candidate.has_receiver &&
            candidate.arguments.empty() && code.size() == 4U &&
            code[0U] == 0xB2U;
        if (instance_shape || static_shape)
        {
          const usize cp_offset = instance_shape ? 2U : 1U;
          const u16 cp_index = static_cast<u16>(
              (static_cast<u16>(code[cp_offset]) << 8U) |
              static_cast<u16>(code[cp_offset + 1U]));
          auto reference = candidate.method.owner->member_reference(cp_index);
          if (reference)
          {
            const std::string expected_method_descriptor =
                std::string("()") + reference->descriptor;
            u8 expected_return = 0U;
            if (!reference->descriptor.empty())
            {
              switch (reference->descriptor.front())
              {
              case 'J': expected_return = 0xADU; break; // lreturn
              case 'F': expected_return = 0xAEU; break; // freturn
              case 'D': expected_return = 0xAFU; break; // dreturn
              case 'L':
              case '[': expected_return = 0xB0U; break; // areturn
              default: expected_return = 0xACU; break;  // ireturn
              }
            }
            const usize return_offset = instance_shape ? 4U : 3U;
            if (method->descriptor == expected_method_descriptor &&
                code[return_offset] == expected_return &&
                (!static_shape ||
                 reference->owner == candidate.method.owner->name()))
            {
              auto field = states_.resolve_field(
                  reference->owner,
                  reference->name,
                  reference->descriptor,
                  static_shape);
              if (field)
              {
                resolved = TrivialGetterIntrinsic {
                    .field = std::move(*field),
                    .is_static = static_shape,
                };
              }
            }
          }
        }
        cached = trivial_getter_intrinsics_.emplace(
            method, std::move(resolved)).first;
      }
      if (!cached->second.has_value()) return std::optional<Value>{};

      const TrivialGetterIntrinsic& getter = *cached->second;
      if (getter.is_static)
      {
        auto value = states_.static_field(getter.field);
        if (!value) return std::unexpected(value.error());
        return std::optional<Value>(*value);
      }
      if (candidate.arguments.kind(0U) != ValueKind::reference)
        return std::optional<Value>{};
      const ObjectRef receiver = candidate.arguments.reference_unchecked(0U);
      if (receiver.is_null()) return std::optional<Value>{};
      auto value = heap_.vm_field(receiver, getter.field.index);
      if (!value) return std::unexpected(value.error());
      return std::optional<Value>(*value);
    };

    // Compact binary/PNG parsers generated for many CLDC games commonly use
    // a tiny helper equivalent to `return bytes[cursor++];`. Calling such a
    // helper once per input byte is disproportionately expensive even after
    // JIT compilation because every call still crosses the Java/JIT dispatch
    // boundary. Recognize the exact bytecode shape instead of any class or
    // method name and perform the same field/array operations directly.
    const auto try_static_byte_cursor_read_intrinsic =
        [this](const Invocation& candidate)
        -> Result<std::optional<Value>>
    {
      if (!specialized_intrinsics_requested() ||
          candidate.method.owner == nullptr ||
          candidate.method.method == nullptr ||
          !candidate.method.method->code.has_value() ||
          candidate.has_receiver || !candidate.arguments.empty() ||
          candidate.return_override.has_value() ||
          candidate.discard_return_value ||
          candidate.return_reference_cast_target.has_value() ||
          candidate.return_unboxing_target.has_value() ||
          candidate.return_widening_target.has_value())
      {
        return std::optional<Value>{};
      }

      const classfile::Method* method = candidate.method.method;
      auto cached = static_byte_cursor_read_intrinsics_.find(method);
      if (cached == static_byte_cursor_read_intrinsics_.end())
      {
        std::optional<StaticByteCursorReadIntrinsic> resolved;
        const auto& code = method->code->bytecode;
        const bool exact_shape =
            (method->access_flags & kAccStatic) != 0U &&
            method->descriptor == "()B" && code.size() == 14U &&
            code[0U] == 0xB2U &&  // getstatic byte[]
            code[3U] == 0xB2U &&  // getstatic cursor
            code[6U] == 0x59U &&  // dup
            code[7U] == 0x04U &&  // iconst_1
            code[8U] == 0x60U &&  // iadd
            code[9U] == 0xB3U &&  // putstatic cursor
            code[12U] == 0x33U && // baload
            code[13U] == 0xACU;   // ireturn
        if (exact_shape)
        {
          const u16 bytes_index = bytecode_cp_index(code, 0U);
          const u16 cursor_get_index = bytecode_cp_index(code, 3U);
          const u16 cursor_put_index = bytecode_cp_index(code, 9U);
          if (cursor_get_index == cursor_put_index)
          {
            auto bytes_reference =
                candidate.method.owner->member_reference(bytes_index);
            auto cursor_reference =
                candidate.method.owner->member_reference(cursor_get_index);
            if (bytes_reference && cursor_reference &&
                bytes_reference->descriptor == "[B" &&
                cursor_reference->descriptor == "I" &&
                bytes_reference->owner == candidate.method.owner->name() &&
                cursor_reference->owner == candidate.method.owner->name())
            {
              auto bytes_field = states_.resolve_field(
                  bytes_reference->owner, bytes_reference->name,
                  bytes_reference->descriptor, true);
              auto cursor_field = states_.resolve_field(
                  cursor_reference->owner, cursor_reference->name,
                  cursor_reference->descriptor, true);
              if (bytes_field && cursor_field)
              {
                resolved = StaticByteCursorReadIntrinsic {
                    .bytes = std::move(*bytes_field),
                    .cursor = std::move(*cursor_field),
                };
              }
            }
          }
        }
        cached = static_byte_cursor_read_intrinsics_.emplace(
            method, std::move(resolved)).first;
      }
      if (!cached->second.has_value()) return std::optional<Value>{};

      const StaticByteCursorReadIntrinsic& intrinsic = *cached->second;
      auto bytes_value = states_.static_field(intrinsic.bytes);
      auto cursor_value = states_.static_field(intrinsic.cursor);
      if (!bytes_value) return std::unexpected(bytes_value.error());
      if (!cursor_value) return std::unexpected(cursor_value.error());

      auto bytes = bytes_value->as_reference();
      auto cursor = cursor_value->as_int();
      if (!bytes || !cursor || bytes->is_null() || *cursor < 0)
        return std::optional<Value>{};

      auto length = heap_.vm_array_length(*bytes);
      if (!length) return std::optional<Value>{};
      const usize index = static_cast<usize>(*cursor);
      if (index >= *length) return std::optional<Value>{};

      auto snapshot = heap_.vm_array_element_snapshot(*bytes, index);
      if (!snapshot || snapshot->kind != HeapArrayKind::byte)
        return std::optional<Value>{};
      auto byte_value = snapshot->value.as_int();
      if (!byte_value) return std::unexpected(byte_value.error());

      // The original bytecode performs the post-increment before `baload`.
      // We only take this fast path after proving the load is in bounds, so
      // observable successful-call semantics are identical. Exceptional cases
      // fall back to Java bytecode and preserve its exact side-effect order.
      const i32 next_cursor = std::bit_cast<i32>(
          static_cast<u32>(*cursor) + 1U);
      auto stored = states_.set_static_field(
          intrinsic.cursor, Value::from_int(next_cursor));
      if (!stored) return std::unexpected(stored.error());
      return std::optional<Value>(Value::from_int(
          static_cast<i32>(static_cast<i8>(*byte_value))));
    };

    const auto try_indexed_png_decoder_intrinsic =
        [this](const Invocation& candidate)
        -> Result<std::optional<Value>>
    {
      if (!specialized_intrinsics_requested() ||
          candidate.method.owner == nullptr ||
          candidate.method.method == nullptr || candidate.has_receiver ||
          candidate.arguments.size() != 1U ||
          candidate.return_override.has_value() ||
          candidate.discard_return_value ||
          candidate.return_reference_cast_target.has_value() ||
          candidate.return_unboxing_target.has_value() ||
          candidate.return_widening_target.has_value() ||
          !matches_indexed_png_decoder_intrinsic(
              *candidate.method.owner, *candidate.method.method))
      {
        return std::optional<Value>{};
      }

      auto bytes = candidate.arguments[0U].as_reference();
      if (!bytes || bytes->is_null()) return std::optional<Value>{};
      auto info = heap_.vm_array_info(*bytes);
      if (!info || info->kind != HeapArrayKind::byte || info->length < 8U)
        return std::optional<Value>{};

      static constexpr std::array<u8, 8U> kPngSignature {{
          0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
      }};
      for (usize index = 0U; index < kPngSignature.size(); ++index)
      {
        auto element = heap_.vm_array_element_snapshot(*bytes, index);
        if (!element || element->kind != HeapArrayKind::byte)
          return std::optional<Value>{};
        auto value = element->value.as_int();
        if (!value || static_cast<u8>(*value) != kPngSignature[index])
          return std::optional<Value>{};
      }
      if (info->length > static_cast<usize>(std::numeric_limits<i32>::max()))
        return std::optional<Value>{};

      // Keep the source rooted while the native decoder allocates the Image
      // object. If the native PNG decoder rejects a stream, fall back to the
      // original Java implementation so compatibility is never reduced.
      auto root = pin_native_root(*bytes);
      if (!root) return std::unexpected(root.error());
      const std::array<Value, 3U> arguments {{
          Value::from_reference(*bytes),
          Value::from_int(0),
          Value::from_int(static_cast<i32>(info->length)),
      }};
      auto decoded = natives_.invoke(
          *this,
          "javax/microedition/lcdui/Image",
          "createImage",
          "([BII)Ljavax/microedition/lcdui/Image;",
          arguments);
      if (!decoded)
      {
        if (decoded.error().code == ErrorCode::java_exception)
          return std::optional<Value>{};
        return std::unexpected(decoded.error());
      }
      if (!decoded->has_value()) return std::optional<Value>{};
      return std::optional<Value>(**decoded);
    };

    struct RangeDecoderIntrinsicFields final
    {
      FieldLocation range;
      FieldLocation code;
      FieldLocation probabilities;
      FieldLocation input_position;
      FieldLocation input_length;
      FieldLocation input_bytes;
    };
    std::unordered_map<const classfile::Method*,
                       std::optional<RangeDecoderIntrinsicFields>>
        range_decoder_intrinsic_cache;

    const auto resolve_range_decoder_intrinsic =
        [this](const Invocation& candidate)
        -> Result<std::optional<RangeDecoderIntrinsicFields>>
    {
      if (candidate.method.owner == nullptr ||
          candidate.method.method == nullptr || candidate.has_receiver ||
          candidate.arguments.size() != 1U ||
          !matches_range_decoder_bit_intrinsic(*candidate.method.method))
      {
        return std::optional<RangeDecoderIntrinsicFields>{};
      }
      const auto& owner = *candidate.method.owner;
      const auto& code = candidate.method.method->code->bytecode;
      const auto cp_index = [](const std::vector<u8>& bytecode,
                               usize offset) -> u16
      {
        return static_cast<u16>(
            (static_cast<u16>(bytecode[offset]) << 8U) |
            static_cast<u16>(bytecode[offset + 1U]));
      };
      const u16 range_index = cp_index(code, 1U);
      const u16 probabilities_index = cp_index(code, 7U);
      const u16 decoder_code_index = cp_index(code, 15U);
      const u16 input_method_index = cp_index(code, 65U);
      for (usize offset : {24U, 49U, 73U, 79U, 84U, 89U,
                           116U, 140U, 146U})
      {
        if (cp_index(code, offset) != range_index)
          return std::optional<RangeDecoderIntrinsicFields>{};
      }
      for (usize offset : {30U, 37U, 100U, 106U})
      {
        if (cp_index(code, offset) != probabilities_index)
          return std::optional<RangeDecoderIntrinsicFields>{};
      }
      for (usize offset : {59U, 70U, 92U, 97U, 126U, 137U})
      {
        if (cp_index(code, offset) != decoder_code_index)
          return std::optional<RangeDecoderIntrinsicFields>{};
      }
      if (cp_index(code, 132U) != input_method_index ||
          cp_index(code, 52U) != cp_index(code, 119U))
      {
        return std::optional<RangeDecoderIntrinsicFields>{};
      }

      auto normalization_constant = owner.constant(cp_index(code, 52U));
      if (!normalization_constant ||
          (*normalization_constant)->kind !=
              classfile::ConstantKind::long64 ||
          (*normalization_constant)->bits != 16'777'216ULL)
      {
        return std::optional<RangeDecoderIntrinsicFields>{};
      }
      auto range_reference = owner.member_reference(range_index);
      auto probabilities_reference = owner.member_reference(probabilities_index);
      auto decoder_code_reference = owner.member_reference(decoder_code_index);
      auto input_reference = owner.member_reference(input_method_index);
      if (!range_reference || !probabilities_reference ||
          !decoder_code_reference || !input_reference)
      {
        return std::optional<RangeDecoderIntrinsicFields>{};
      }
      if (range_reference->descriptor != "J" ||
          probabilities_reference->descriptor != "[S" ||
          decoder_code_reference->descriptor != "J" ||
          input_reference->descriptor != "()I")
      {
        return std::optional<RangeDecoderIntrinsicFields>{};
      }

      auto input_method = classes_.resolve_declared_method(
          input_reference->owner,
          input_reference->name,
          input_reference->descriptor);
      if (!input_method ||
          !matches_range_decoder_input_intrinsic(*input_method->method))
      {
        return std::optional<RangeDecoderIntrinsicFields>{};
      }
      const auto& input_code = input_method->method->code->bytecode;
      const u16 position_index = cp_index(input_code, 1U);
      const u16 length_index = cp_index(input_code, 4U);
      const u16 bytes_index = cp_index(input_code, 14U);
      if (cp_index(input_code, 17U) != position_index ||
          cp_index(input_code, 23U) != position_index)
      {
        return std::optional<RangeDecoderIntrinsicFields>{};
      }
      auto position_reference =
          input_method->owner->member_reference(position_index);
      auto length_reference = input_method->owner->member_reference(length_index);
      auto bytes_reference = input_method->owner->member_reference(bytes_index);
      if (!position_reference || !length_reference || !bytes_reference ||
          position_reference->descriptor != "I" ||
          length_reference->descriptor != "I" ||
          bytes_reference->descriptor != "[B")
      {
        return std::optional<RangeDecoderIntrinsicFields>{};
      }

      auto range_field = states_.resolve_field(
          range_reference->owner, range_reference->name,
          range_reference->descriptor, true);
      auto decoder_code_field = states_.resolve_field(
          decoder_code_reference->owner, decoder_code_reference->name,
          decoder_code_reference->descriptor, true);
      auto probabilities_field = states_.resolve_field(
          probabilities_reference->owner, probabilities_reference->name,
          probabilities_reference->descriptor, true);
      auto position_field = states_.resolve_field(
          position_reference->owner, position_reference->name,
          position_reference->descriptor, true);
      auto length_field = states_.resolve_field(
          length_reference->owner, length_reference->name,
          length_reference->descriptor, true);
      auto bytes_field = states_.resolve_field(
          bytes_reference->owner, bytes_reference->name,
          bytes_reference->descriptor, true);
      if (!range_field || !decoder_code_field || !probabilities_field ||
          !position_field || !length_field || !bytes_field)
      {
        return std::optional<RangeDecoderIntrinsicFields>{};
      }
      return std::optional<RangeDecoderIntrinsicFields>(
          RangeDecoderIntrinsicFields {
              .range = std::move(*range_field),
              .code = std::move(*decoder_code_field),
              .probabilities = std::move(*probabilities_field),
              .input_position = std::move(*position_field),
              .input_length = std::move(*length_field),
              .input_bytes = std::move(*bytes_field),
          });
    };

    const auto try_range_decoder_bit_intrinsic =
        [this, &range_decoder_intrinsic_cache,
         &resolve_range_decoder_intrinsic](const Invocation& candidate)
        -> Result<std::optional<Value>>
    {
      if (!specialized_intrinsics_requested())
        return std::optional<Value>{};
      if (candidate.method.method == nullptr || candidate.has_receiver ||
          candidate.arguments.size() != 1U ||
          !matches_range_decoder_bit_intrinsic(*candidate.method.method))
      {
        return std::optional<Value>{};
      }
      auto cached = range_decoder_intrinsic_cache.find(
          candidate.method.method);
      if (cached == range_decoder_intrinsic_cache.end())
      {
        auto resolved = resolve_range_decoder_intrinsic(candidate);
        if (!resolved) return std::unexpected(resolved.error());
        cached = range_decoder_intrinsic_cache.emplace(
            candidate.method.method, std::move(*resolved)).first;
      }
      if (!cached->second.has_value()) return std::optional<Value>{};
      const RangeDecoderIntrinsicFields& fields = *cached->second;

      auto probability_index = candidate.arguments[0].as_int();
      auto range_value = states_.static_field(fields.range);
      auto decoder_code_value = states_.static_field(fields.code);
      auto probabilities_value = states_.static_field(fields.probabilities);
      auto position_value = states_.static_field(fields.input_position);
      auto length_value = states_.static_field(fields.input_length);
      auto bytes_value = states_.static_field(fields.input_bytes);
      if (!probability_index || !range_value || !decoder_code_value ||
          !probabilities_value || !position_value || !length_value ||
          !bytes_value)
      {
        return std::optional<Value>{};
      }
      auto range = range_value->as_long();
      auto decoder_code = decoder_code_value->as_long();
      auto probabilities = probabilities_value->as_reference();
      auto position = position_value->as_int();
      auto input_length = length_value->as_int();
      auto input_bytes = bytes_value->as_reference();
      if (!range || !decoder_code || !probabilities || !position ||
          !input_length || !input_bytes || probabilities->is_null() ||
          input_bytes->is_null() || *probability_index < 0 ||
          *position < 0 || *input_length < *position)
      {
        return std::optional<Value>{};
      }
      auto probability_count = heap_.vm_array_length(*probabilities);
      auto input_capacity = heap_.vm_array_length(*input_bytes);
      if (!probability_count || !input_capacity ||
          static_cast<usize>(*probability_index) >= *probability_count ||
          static_cast<usize>(*input_length) > *input_capacity)
      {
        return std::optional<Value>{};
      }
      auto probability_value = heap_.vm_element(
          *probabilities, static_cast<usize>(*probability_index));
      if (!probability_value) return std::optional<Value>{};
      auto probability = probability_value->as_int();
      if (!probability || *probability < 0 || *probability > 2'048)
        return std::optional<Value>{};

      u64 next_range = static_cast<u64>(*range);
      u64 next_code = static_cast<u64>(*decoder_code);
      const u64 bound = (next_range >> 11U) *
                        static_cast<u64>(*probability);
      i32 next_probability = *probability;
      i32 decoded_bit = 0;
      if (next_code < bound)
      {
        next_range = bound;
        next_probability += (2'048 - next_probability) >> 5;
      }
      else
      {
        next_range -= bound;
        next_code -= bound;
        next_probability -= next_probability >> 5;
        decoded_bit = 1;
      }

      i32 next_position = *position;
      if (next_range < 16'777'216ULL)
      {
        u8 next_byte = 0xFFU;
        if (next_position != *input_length)
        {
          auto byte_value = heap_.vm_element(
              *input_bytes, static_cast<usize>(next_position));
          if (!byte_value) return std::optional<Value>{};
          auto byte = byte_value->as_int();
          if (!byte) return std::optional<Value>{};
          next_byte = static_cast<u8>(static_cast<i8>(*byte));
          ++next_position;
        }
        next_code = (next_code << 8U) | next_byte;
        next_range <<= 8U;
      }

      auto probability_stored = heap_.vm_set_element(
          *probabilities,
          static_cast<usize>(*probability_index),
          Value::from_int(static_cast<i32>(
              static_cast<i16>(next_probability))));
      auto range_stored = states_.set_static_field(
          fields.range, Value::from_long(static_cast<i64>(next_range)));
      auto code_stored = states_.set_static_field(
          fields.code, Value::from_long(static_cast<i64>(next_code)));
      auto position_stored = states_.set_static_field(
          fields.input_position, Value::from_int(next_position));
      if (!probability_stored) return std::unexpected(probability_stored.error());
      if (!range_stored) return std::unexpected(range_stored.error());
      if (!code_stored) return std::unexpected(code_stored.error());
      if (!position_stored) return std::unexpected(position_stored.error());
      return std::optional<Value>(Value::from_int(decoded_bit));
    };

    const auto try_vector_key_sort_intrinsic =
        [this](const Invocation& candidate) -> Result<bool>
    {
      if (!specialized_intrinsics_requested()) return false;
      if (candidate.method.owner == nullptr ||
          candidate.method.method == nullptr || candidate.has_receiver ||
          !candidate.arguments.empty() ||
          !matches_vector_key_sort_initializer(*candidate.method.method))
      {
        return false;
      }

      const auto& owner = *candidate.method.owner;
      const auto& sort_code = candidate.method.method->code->bytecode;
      const u16 vector_field_index = static_cast<u16>(
          (static_cast<u16>(sort_code[1U]) << 8U) |
          static_cast<u16>(sort_code[2U]));
      auto vector_reference = owner.member_reference(vector_field_index);
      if (!vector_reference ||
          vector_reference->descriptor != "Ljava/util/Vector;")
      {
        return false;
      }

      const classfile::Method* comparator = nullptr;
      for (const auto& method : owner.methods())
      {
        if ((method.access_flags & kAccStatic) != 0U ||
            !method.code.has_value() || method.code->bytecode.size() != 69U ||
            !method.descriptor.ends_with(")Z"))
        {
          continue;
        }
        const auto& code = method.code->bytecode;
        const bool shape_matches =
            code[0U] == 0x2AU && code[1U] == 0x2BU &&
            code[2U] == 0xA6U && code[5U] == 0x04U &&
            code[6U] == 0xACU && code[7U] == 0x2BU &&
            code[8U] == 0xC1U && code[11U] == 0x99U &&
            code[14U] == 0x2AU && code[15U] == 0xB4U &&
            code[18U] == 0x10U && code[19U] == 56U &&
            code[20U] == 0x10U && code[21U] == 63U &&
            code[22U] == 0x2AU && code[23U] == 0xB4U &&
            code[26U] == 0x2AU && code[27U] == 0xB4U &&
            code[30U] == 0xB8U && code[33U] == 0x2BU &&
            code[34U] == 0xC0U && code[37U] == 0x4DU &&
            code[38U] == 0x2CU && code[39U] == 0xB4U &&
            code[42U] == 0x10U && code[43U] == 56U &&
            code[44U] == 0x10U && code[45U] == 63U &&
            code[46U] == 0x2CU && code[47U] == 0xB4U &&
            code[50U] == 0x2CU && code[51U] == 0xB4U &&
            code[54U] == 0xB8U && code[57U] == 0x65U &&
            code[58U] == 0x09U && code[59U] == 0x94U &&
            code[60U] == 0x9EU && code[63U] == 0x03U &&
            code[64U] == 0xACU && code[65U] == 0x04U &&
            code[66U] == 0xACU && code[67U] == 0x04U &&
            code[68U] == 0xACU &&
            code[9U] == code[35U] && code[10U] == code[36U] &&
            code[16U] == code[40U] && code[17U] == code[41U] &&
            code[24U] == code[48U] && code[25U] == code[49U] &&
            code[31U] == code[55U] && code[32U] == code[56U];
        if (shape_matches)
        {
          comparator = &method;
          break;
        }
      }
      if (comparator == nullptr) return false;

      const auto& comparator_code = comparator->code->bytecode;
      const auto member_index = [&comparator_code](usize offset) {
        return static_cast<u16>(
            (static_cast<u16>(comparator_code[offset]) << 8U) |
            static_cast<u16>(comparator_code[offset + 1U]));
      };
      auto key_reference = owner.member_reference(member_index(16U));
      auto shifts_reference = owner.member_reference(member_index(24U));
      auto permutation_reference = owner.member_reference(member_index(31U));
      if (!key_reference || !shifts_reference || !permutation_reference ||
          key_reference->descriptor != "J" ||
          shifts_reference->descriptor != "[I" ||
          permutation_reference->descriptor != "(JII[I[J)J")
      {
        return false;
      }
      auto permutation_method = classes_.resolve_declared_method(
          permutation_reference->owner,
          permutation_reference->name,
          permutation_reference->descriptor);
      if (!permutation_method ||
          !matches_long_bit_permutation_intrinsic(
              *permutation_method->method))
      {
        return false;
      }
      const auto& permutation_code =
          permutation_method->method->code->bytecode;
      const u16 masks_field_index = static_cast<u16>(
          (static_cast<u16>(permutation_code[16U]) << 8U) |
          static_cast<u16>(permutation_code[17U]));
      auto masks_reference =
          permutation_method->owner->member_reference(masks_field_index);
      if (!masks_reference || masks_reference->descriptor != "[J")
      {
        return false;
      }

      auto vector_field = states_.resolve_field(vector_reference->owner,
                                                vector_reference->name,
                                                vector_reference->descriptor,
                                                true);
      auto key_field = states_.resolve_field(key_reference->owner,
                                             key_reference->name,
                                             key_reference->descriptor,
                                             false);
      auto shifts_field = states_.resolve_field(shifts_reference->owner,
                                                shifts_reference->name,
                                                shifts_reference->descriptor,
                                                false);
      auto masks_field = states_.resolve_field(masks_reference->owner,
                                               masks_reference->name,
                                               masks_reference->descriptor,
                                               true);
      if (!vector_field || !key_field || !shifts_field || !masks_field)
      {
        return false;
      }
      auto vector_value = states_.static_field(*vector_field);
      auto masks_value = states_.static_field(*masks_field);
      if (!vector_value || !masks_value) return false;
      auto vector = vector_value->as_reference();
      auto masks = masks_value->as_reference();
      if (!vector || !masks || vector->is_null() || masks->is_null())
      {
        return false;
      }
      auto vector_class = heap_.vm_class_name_view(*vector);
      if (!vector_class || *vector_class != "java/util/Vector")
      {
        return false;
      }
      auto data_value = heap_.vm_field(*vector, 0U);
      auto count_value = heap_.vm_field(*vector, 1U);
      if (!data_value || !count_value) return false;
      auto data = data_value->as_reference();
      auto count = count_value->as_int();
      if (!data || !count || data->is_null() || *count < 0)
      {
        return false;
      }
      auto data_length = heap_.vm_array_length(*data);
      auto mask_count = heap_.vm_array_length(*masks);
      if (!data_length || !mask_count ||
          static_cast<usize>(*count) > *data_length)
      {
        return false;
      }

      std::vector<u64> mask_values;
      mask_values.reserve(*mask_count);
      for (usize index = 0; index < *mask_count; ++index)
      {
        auto element = heap_.vm_element(*masks, index);
        if (!element) return false;
        auto value = element->as_long();
        if (!value) return false;
        mask_values.push_back(static_cast<u64>(*value));
      }

      struct SortEntry final {
        u64 key {0U};
        Value value;
      };
      std::vector<SortEntry> entries;
      entries.reserve(static_cast<usize>(*count));
      for (i32 index = 0; index < *count; ++index)
      {
        auto element = heap_.vm_element(*data, static_cast<usize>(index));
        if (!element) return false;
        auto object = element->as_reference();
        if (!object || object->is_null()) return false;
        auto object_class = heap_.vm_class_name_view(*object);
        if (!object_class || *object_class != owner.name()) return false;
        auto input_value = heap_.vm_field(*object, key_field->index);
        auto shift_value = heap_.vm_field(*object, shifts_field->index);
        if (!input_value || !shift_value) return false;
        auto input = input_value->as_long();
        auto shifts = shift_value->as_reference();
        if (!input || !shifts || shifts->is_null()) return false;
        auto shift_count = heap_.vm_array_length(*shifts);
        if (!shift_count || *shift_count > mask_values.size()) return false;

        const u64 source = static_cast<u64>(*input);
        u64 accumulated = 0U;
        for (usize shift_index = 0; shift_index < *shift_count;
             ++shift_index)
        {
          auto shift_element = heap_.vm_element(*shifts, shift_index);
          if (!shift_element) return false;
          auto shift = shift_element->as_int();
          if (!shift) return false;
          u64 selected = source & mask_values[shift_index];
          if (selected == 0U) continue;
          if (*shift > 0)
          {
            selected >>= static_cast<u32>(*shift) & 63U;
          }
          else if (*shift < 0)
          {
            selected <<=
                (0U - static_cast<u32>(*shift)) & 63U;
          }
          accumulated |= selected;
        }
        entries.push_back(SortEntry {
            .key = accumulated >> 56U,
            .value = *element,
        });
      }

      std::stable_sort(entries.begin(), entries.end(),
                       [](const SortEntry& left, const SortEntry& right) {
                         return left.key < right.key;
                       });
      for (usize index = 0; index < entries.size(); ++index)
      {
        auto stored = heap_.vm_set_element(*data, index, entries[index].value);
        if (!stored) return std::unexpected(stored.error());
      }
      return true;
    };

    const auto try_long_bit_permutation_intrinsic =
        [this](const Invocation& candidate)
        -> Result<std::optional<Value>>
    {
      if (!specialized_intrinsics_requested())
        return std::optional<Value>{};
      if (candidate.method.owner == nullptr ||
          candidate.method.method == nullptr || candidate.has_receiver ||
          candidate.arguments.size() != 5U ||
          !matches_long_bit_permutation_intrinsic(*candidate.method.method))
      {
        return std::optional<Value>{};
      }

      const auto& bytecode = candidate.method.method->code->bytecode;
      const u16 field_index = static_cast<u16>(
          (static_cast<u16>(bytecode[16U]) << 8U) |
          static_cast<u16>(bytecode[17U]));
      auto mask_reference = candidate.method.owner->member_reference(field_index);
      if (!mask_reference)
        return std::unexpected(mask_reference.error());
      if (mask_reference->descriptor != "[J")
        return std::optional<Value>{};
      auto mask_field = states_.resolve_field(mask_reference->owner,
                                              mask_reference->name,
                                              mask_reference->descriptor,
                                              true);
      if (!mask_field)
        return std::unexpected(mask_field.error());
      auto mask_value = states_.static_field(*mask_field);
      if (!mask_value)
        return std::unexpected(mask_value.error());

      auto input = candidate.arguments[0].as_long();
      auto first_position = candidate.arguments[1].as_int();
      auto last_position = candidate.arguments[2].as_int();
      auto shift_reference = candidate.arguments[3].as_reference();
      auto masks = mask_value->as_reference();
      if (!input || !first_position || !last_position ||
          !shift_reference || !masks || shift_reference->is_null() ||
          masks->is_null())
      {
        // Preserve normal Java exception behavior for malformed/null inputs.
        return std::optional<Value>{};
      }
      auto shift_count = heap_.vm_array_length(*shift_reference);
      auto mask_count = heap_.vm_array_length(*masks);
      if (!shift_count || !mask_count)
      {
        return std::optional<Value>{};
      }
      if (*mask_count < *shift_count)
      {
        return std::optional<Value>{};
      }

      const u64 source = static_cast<u64>(*input);
      u64 accumulated = 0U;
      for (usize index = 0; index < *shift_count; ++index)
      {
        auto mask_element = heap_.vm_element(*masks, index);
        auto shift_element = heap_.vm_element(*shift_reference, index);
        if (!mask_element || !shift_element)
        {
          return std::optional<Value>{};
        }
        auto mask = mask_element->as_long();
        auto shift = shift_element->as_int();
        if (!mask || !shift)
        {
          return std::optional<Value>{};
        }
        u64 selected = source & static_cast<u64>(*mask);
        if (selected == 0U) continue;
        if (*shift > 0)
        {
          selected >>= static_cast<u32>(*shift) & 63U;
        }
        else if (*shift < 0)
        {
          const u32 distance =
              (0U - static_cast<u32>(*shift)) & 63U;
          selected <<= distance;
        }
        accumulated |= selected;
      }

      const i32 leading_shift = static_cast<i32>(
          63U - static_cast<u32>(*last_position));
      if (leading_shift > 0)
      {
        accumulated <<= static_cast<u32>(leading_shift) & 63U;
      }
      const i32 trailing_shift = static_cast<i32>(
          static_cast<u32>(*first_position) + 63U -
          static_cast<u32>(*last_position));
      if (trailing_shift > 0)
      {
        accumulated >>= static_cast<u32>(trailing_shift) & 63U;
      }
      return std::optional<Value>(
          Value::from_long(std::bit_cast<i64>(accumulated)));
    };

    std::function<Result<std::optional<ExecutionResult>>(ObjectRef, usize)>
        dispatch_exception;
    const auto complete_return = [this, &frames, &executed,
                                  &dispatch_exception](
                                     std::optional<Value> value)
        -> Result<std::optional<ExecutionResult>>
    {
      if (frames.back().return_reference_cast_target().has_value())
      {
        if (!value.has_value())
        {
          return fail(ErrorCode::invalid_state,
                      "cast lambda return has no reference value");
        }
        auto casted = cast_lambda_reference(
            *value, *frames.back().return_reference_cast_target());
        if (!casted)
        {
          if (casted.error().code != ErrorCode::java_exception ||
              casted.error().java_exception_class.empty() ||
              frames.size() < 2U)
          {
            return std::unexpected(casted.error());
          }
          const Error adaptation_error = casted.error();
          auto released = release_synchronized_monitor(
              frames.back().synchronized_monitor());
          if (!released)
            return std::unexpected(released.error());
          frames.pop_back();
          auto throwable = create_throwable(
              adaptation_error.java_exception_class,
              adaptation_error.message);
          if (!throwable)
            return std::unexpected(throwable.error());
          return dispatch_exception(
              *throwable, frames.back().current_instruction_pc());
        }
        value = *casted;
      }
      if (frames.back().return_unboxing_target().has_value())
      {
        if (!value.has_value())
        {
          return fail(ErrorCode::invalid_state,
                      "unboxed lambda return has no reference value");
        }
        auto unboxed = unbox_lambda_value(
            *value, *frames.back().return_unboxing_target());
        if (!unboxed)
        {
          if (unboxed.error().code != ErrorCode::java_exception ||
              unboxed.error().java_exception_class.empty() ||
              frames.size() < 2U)
          {
            return std::unexpected(unboxed.error());
          }
          const Error adaptation_error = unboxed.error();
          auto released = release_synchronized_monitor(
              frames.back().synchronized_monitor());
          if (!released)
            return std::unexpected(released.error());
          frames.pop_back();
          auto throwable = create_throwable(
              adaptation_error.java_exception_class,
              adaptation_error.message);
          if (!throwable)
            return std::unexpected(throwable.error());
          return dispatch_exception(
              *throwable, frames.back().current_instruction_pc());
        }
        value = *unboxed;
      }
      if (frames.back().return_widening_source().has_value() &&
          frames.back().return_widening_target().has_value())
      {
        if (!value.has_value())
        {
          return fail(ErrorCode::invalid_state,
                      "widened lambda return has no primitive value");
        }
        auto widened = widen_primitive_value(
            *value,
            *frames.back().return_widening_source(),
            *frames.back().return_widening_target());
        if (!widened)
          return std::unexpected(widened.error());
        value = *widened;
      }
      if (frames.back().return_override().has_value())
      {
        if (frames.back().return_override_boxes_result())
        {
          if (!value.has_value())
          {
            return fail(ErrorCode::invalid_state,
                        "boxed lambda return has no primitive value");
          }
          auto wrapper = frames.back().return_override()->as_reference();
          if (!wrapper || wrapper->is_null())
          {
            return fail(ErrorCode::internal_error,
                        "boxed lambda return has no wrapper object");
          }
          auto stored = heap_.vm_set_field(*wrapper, 0U, *value);
          if (!stored)
            return std::unexpected(stored.error());
        }
        value = frames.back().return_override();
      }
      if (frames.back().discard_return_value())
        value = std::nullopt;
      auto released = release_synchronized_monitor(
          frames.back().synchronized_monitor());
      if (!released)
        return std::unexpected(released.error());
      frames.pop_back();
      if (frames.empty())
      {
        return std::optional<ExecutionResult>(ExecutionResult{
            .return_value = value,
            .throwable = std::nullopt,
            .executed_instructions = executed,
        });
      }
      if (value.has_value())
      {
        auto pushed = frames.back().push(*value);
        if (!pushed)
        {
          return std::unexpected(pushed.error());
        }
      }
      return std::optional<ExecutionResult>{};
    };

    dispatch_exception =
        [this, &frames, &executed](ObjectRef throwable, usize throw_pc)
        -> Result<std::optional<ExecutionResult>>
    {
      PerformanceCounters::record_exception_dispatch();
      if (throwable.is_null())
      {
        return fail(ErrorCode::internal_error,
                    "cannot dispatch a null Java throwable");
      }
      scheduler_.set_current_pending_exception(throwable);
      auto throwable_class = heap_.vm_class_name_view(throwable);
      if (!throwable_class)
      {
        return std::unexpected(throwable_class.error());
      }
      auto is_throwable = classes_.is_assignable(*throwable_class,
                                                 "java/lang/Throwable");
      if (!is_throwable)
      {
        return std::unexpected(is_throwable.error());
      }
      if (!*is_throwable)
      {
        return fail(ErrorCode::malformed_class,
                    "athrow operand is not assignable to Throwable");
      }

      const std::string exception_context = frames.empty()
          ? std::string {}
          : frames.back().owner().name() + "." +
                frames.back().method().name +
                frames.back().method().descriptor + " at bytecode " +
                std::to_string(throw_pc);
      std::string throwable_message;
      auto message_value = heap_.vm_field(throwable, 0U);
      if (message_value)
      {
        auto message_reference = message_value->as_reference();
        if (message_reference && !message_reference->is_null())
        {
          auto text = heap_.vm_string_value(*message_reference);
          if (text)
          {
            throwable_message.assign(text->begin(), text->end());
          }
        }
      }
      const std::string throwable_class_text(*throwable_class);
      vm_trace("exception", "throw %s%s%s from %s",
               throwable_class_text.c_str(),
               throwable_message.empty() ? "" : ": ",
               throwable_message.c_str(),
               exception_context.c_str());
      if (vm_trace_enabled())
      {
        for (usize depth = 0U; depth < frames.size(); ++depth)
        {
          const ExecutionFrame& trace_frame =
              frames[frames.size() - 1U - depth];
          vm_trace("exception-stack", "#%zu %s.%s%s pc=%zu",
                   depth,
                   trace_frame.owner().name().c_str(),
                   trace_frame.method().name.c_str(),
                   trace_frame.method().descriptor.c_str(),
                   trace_frame.current_instruction_pc());
        }
      }
      usize current_throw_pc = throw_pc;
      while (!frames.empty())
      {
        ExecutionFrame &current = frames.back();
        for (const classfile::ExceptionHandler &handler :
             current.exception_table())
        {
          if (current_throw_pc < static_cast<usize>(handler.start_pc) ||
              current_throw_pc >= static_cast<usize>(handler.end_pc))
          {
            continue;
          }

          bool catches = handler.catch_type.empty();
          if (!catches)
          {
            auto assignable = classes_.is_assignable(*throwable_class,
                                                     handler.catch_type);
            if (!assignable)
            {
              return std::unexpected(assignable.error());
            }
            catches = *assignable;
          }
          if (!catches)
          {
            continue;
          }

          auto entered = current.enter_exception_handler(
              static_cast<usize>(handler.handler_pc), throwable);
          if (!entered)
          {
            return std::unexpected(entered.error());
          }
          scheduler_.set_current_pending_exception(std::nullopt);
          return std::optional<ExecutionResult>{};
        }

        auto released = release_synchronized_monitor(
            current.synchronized_monitor());
        if (!released)
          return std::unexpected(released.error());
        frames.pop_back();
        if (!frames.empty())
        {
          current_throw_pc = frames.back().current_instruction_pc();
        }
      }

      return std::optional<ExecutionResult>(ExecutionResult{
          .return_value = std::nullopt,
          .throwable = throwable,
          .executed_instructions = executed,
          .exception_context = exception_context,
      });
    };

    const auto raise_implicit =
        [this, &dispatch_exception](std::string_view class_name,
                                    usize throw_pc,
                                    std::string_view message = {})
        -> Result<std::optional<ExecutionResult>>
    {
      auto throwable = message.empty()
          ? create_throwable(class_name)
          : create_throwable(class_name, message);
      if (!throwable)
      {
        return std::unexpected(throwable.error());
      }
      return dispatch_exception(*throwable, throw_pc);
    };
    const auto raise_resolution_error =
        [&raise_implicit](const Error& error, usize throw_pc)
        -> Result<std::optional<ExecutionResult>>
    {
      if (error.code != ErrorCode::java_exception ||
          error.java_exception_class.empty())
      {
        return std::unexpected(error);
      }
      return raise_implicit(error.java_exception_class,
                            throw_pc,
                            error.message);
    };

    while (!frames.empty())
    {
      const bool maintenance_boundary =
          executed >= next_maintenance_poll;
      if (maintenance_boundary)
      {
        cached_host_foreground = scheduler_.host_foreground();
        PerformanceCounters::record_maintenance_check(!cached_host_foreground);
        const u64 maintenance_interval = cached_host_foreground
            ? kForegroundMaintenancePollInterval
            : kBackgroundMaintenancePollInterval;
        do
        {
          if (next_maintenance_poll >
              std::numeric_limits<u64>::max() - maintenance_interval)
          {
            next_maintenance_poll = std::numeric_limits<u64>::max();
            break;
          }
          next_maintenance_poll += maintenance_interval;
        } while (next_maintenance_poll <= executed);
      }
      if (maintenance_boundary && scheduler_.current_stop_requested())
      {
        return fail(ErrorCode::invalid_state,
                    "VM execution was cancelled by scheduler shutdown");
      }

      const bool quantum_boundary = executed >= next_scheduler_quantum;
      const bool background_transition_boundary =
          maintenance_boundary &&
          scheduler_.consume_current_background_transition();
      const bool garbage_collection_poll_boundary =
          executed >= next_garbage_collection_poll;
      const bool automatic_collection =
          garbage_collection_poll_boundary &&
          (heap_.automatic_collection_due() ||
           graphics_.automatic_collection_due());
      bool collect_requested = false;
      if (maintenance_boundary &&
          gc_requested_.load(std::memory_order_relaxed))
      {
        collect_requested =
            gc_requested_.exchange(false, std::memory_order_acq_rel);
      }
      if (garbage_collection_poll_boundary)
      {
        record_stores_.flush_pending();
        do
        {
          if (next_garbage_collection_poll >
              std::numeric_limits<u64>::max() -
                  kGarbageCollectionPollInterval)
          {
            next_garbage_collection_poll =
                std::numeric_limits<u64>::max();
            break;
          }
          next_garbage_collection_poll += kGarbageCollectionPollInterval;
        } while (next_garbage_collection_poll <= executed);
      }
      if (quantum_boundary || background_transition_boundary ||
          collect_requested || automatic_collection)
      {
        safepoint_roots.clear();
        safepoint_roots.reserve(frames.size() * 8U + 8U);
        for (const ExecutionFrame& active_frame : frames)
          active_frame.append_reference_roots(safepoint_roots);
        exchange_execution_roots(invocation_depth, safepoint_roots);
        if (collect_requested || automatic_collection)
        {
          auto collected = collect_active_garbage();
          if (!collected)
            return std::unexpected(collected.error());
        }
        if (quantum_boundary)
        {
          do
          {
            if (next_scheduler_quantum >
                std::numeric_limits<u64>::max() - kSchedulerQuantum)
            {
              next_scheduler_quantum = std::numeric_limits<u64>::max();
              break;
            }
            next_scheduler_quantum += kSchedulerQuantum;
          } while (next_scheduler_quantum <= executed);
        }
        if (quantum_boundary || background_transition_boundary)
        {
          PerformanceCounters::record_scheduler_quantum();
          scheduler_.cooperative_quantum(*this);
        }
      }

      const bool progress_watchdog =
          budget_mode == InstructionBudgetMode::progress_watchdog;
      const bool budget_exhausted = progress_watchdog
          ? watchdog_instructions >= instruction_budget ||
                executed >= progress_total_budget
          : executed >= instruction_budget;
      if (budget_exhausted)
      {
        PerformanceCounters::record_instruction_budget_exit();
        const ExecutionFrame& exhausted_frame = frames.back();
        return fail(
            ErrorCode::invalid_state,
            std::string(progress_watchdog
                            ? "VM progress watchdog was exhausted in "
                            : "VM instruction budget was exhausted in ") +
                exhausted_frame.owner().name() + "." +
                exhausted_frame.method().name +
                exhausted_frame.method().descriptor +
                " at bytecode " +
                std::to_string(exhausted_frame.current_instruction_pc()));
      }
      if (frames.size() > kMaximumCallDepth)
      {
        return fail(ErrorCode::invalid_state,
                    "VM call stack exceeded its maximum depth");
      }

#if !defined(__EMSCRIPTEN__)
      // Root-frame OSR: after a real backward edge has executed several times,
      // compile a side-effect-free loop entry from the interpreter's exact
      // physical JVM-slot state. This never restarts the method or replays its
      // prefix. Stateful loops remain in the interpreter until resumable deopt
      // metadata is available for them.
      ExecutionFrame& osr_frame = frames.back();
      if (jit_.enabled() &&
          !BaselineJit::conservative_device_mode() &&
          !progress_watchdog &&
          frames.size() == 1U &&
          !osr_frame.synchronized_monitor().has_value() &&
          osr_frame.pc() < osr_frame.current_instruction_pc() &&
          osr_frame.note_osr_backedge())
      {
        const CachedMethodDescriptor* osr_descriptor =
            osr_frame.cached_descriptor();
        const MethodId osr_method_id = osr_frame.runtime_method_id();
        if (osr_descriptor != nullptr && osr_method_id.valid() &&
            osr_frame.pc() <=
                static_cast<usize>(std::numeric_limits<u32>::max()))
        {
          constexpr usize kInlineOsrPhysicalSlots = 128U;
          const usize osr_slot_count = osr_frame.jit_physical_slot_count();
          // write_jit_frame_bits overwrites the entire live span. Do not clear
          // the full 1 KiB scratch bank on every hot-loop OSR poll.
          std::array<u64, kInlineOsrPhysicalSlots> inline_osr_slots;
          std::vector<u64> overflow_osr_slots;
          u64* osr_slot_data = inline_osr_slots.data();
          if (osr_slot_count > inline_osr_slots.size())
          {
            overflow_osr_slots.resize(osr_slot_count);
            osr_slot_data = overflow_osr_slots.data();
          }
          const std::span<u64> osr_slots(osr_slot_data, osr_slot_count);
          auto wrote_osr_frame = osr_frame.write_jit_frame_bits(osr_slots);
          if (!wrote_osr_frame)
            return std::unexpected(wrote_osr_frame.error());
          const JitPhysicalFrameView physical_osr_frame {
              .bytecode_pc = static_cast<u32>(osr_frame.pc()),
              .local_slots = static_cast<u32>(
                  osr_frame.method().code->max_locals),
              .stack_slots = static_cast<u32>(
                  osr_frame.operand_stack_slots()),
              .physical_slots = osr_slots,
          };
          safepoint_roots.clear();
          osr_frame.append_reference_roots(safepoint_roots);
          JitExecutionContext osr_context{
              .machine = this,
              .owner = &osr_frame.owner(),
              .method = &osr_frame.method(),
              .invocation_depth = invocation_depth,
              .base_roots = std::span<const ObjectRef>(
                  safepoint_roots.data(), safepoint_roots.size()),
          };
          if (osr_frame.method().code.has_value())
          {
            osr_context.published_roots.reserve(
                safepoint_roots.size() +
                osr_frame.method().code->max_locals +
                osr_frame.method().code->max_stack);
          }
          const JitRuntimeHooks osr_hooks{
              .context = &osr_context,
              .dispatch = &Machine::jit_runtime_dispatch_callback,
              .publish_roots = &Machine::jit_publish_roots_callback,
          };
          const u64 remaining_budget = remaining_execution_budget();
          auto osr_result = jit_.try_execute_osr(
              osr_method_id,
              osr_frame.owner(),
              osr_frame.method(),
              *osr_descriptor,
              osr_frame.has_receiver(),
              physical_osr_frame,
              remaining_budget,
              osr_hooks,
              osr_frame.owner_lifetime(),
              osr_frame.verified_frames(),
              osr_frame.cached_descriptor_lifetime());
          if (!osr_result)
          {
            if (jit_instruction_budget_exhausted(osr_result.error()))
            {
              PerformanceCounters::record_instruction_budget_exit();
              return std::unexpected(vm_instruction_budget_error(
                  osr_frame.owner().name(),
                  osr_frame.method().name,
                  osr_frame.method().descriptor));
            }
            return std::unexpected(osr_result.error());
          }
          if (osr_result->has_value())
          {
            const u64 osr_instructions =
                static_cast<u64>((*osr_result)->bytecode_instructions);
            if (osr_instructions >
                std::numeric_limits<u64>::max() - executed)
            {
              return fail(ErrorCode::overflow,
                          "JIT OSR instruction accounting overflowed");
            }
            executed += osr_instructions;
            separately_accounted_nested_instructions +=
                osr_context.nested_instructions;
            accounted_instructions =
                executed >= separately_accounted_nested_instructions
                    ? executed - separately_accounted_nested_instructions
                    : 0U;
            if ((*osr_result)->deopt_state.has_value())
            {
              auto restored = osr_frame.restore_jit_deopt_state(
                  *(*osr_result)->deopt_state);
              if (!restored)
                return std::unexpected(restored.error());
              osr_frame.allow_osr_retry();
              continue;
            }
            if ((*osr_result)->exception.has_value())
            {
              ObjectRef throwable_reference;
              if (*(*osr_result)->exception ==
                  JitExceptionKind::runtime_throwable)
              {
                if (!osr_context.pending_throwable.has_value())
                {
                  return fail(ErrorCode::internal_error,
                              "JIT OSR exception has no throwable object");
                }
                throwable_reference = *osr_context.pending_throwable;
              }
              else
              {
                auto throwable = create_throwable(
                    jit_exception_class(*(*osr_result)->exception));
                if (!throwable)
                  return std::unexpected(throwable.error());
                throwable_reference = *throwable;
              }
              scheduler_.set_current_pending_exception(throwable_reference);
              return ExecutionResult{
                  .return_value = std::nullopt,
                  .throwable = throwable_reference,
                  .executed_instructions = executed,
                  .exception_context = osr_frame.owner().name() + "." +
                      osr_frame.method().name +
                      osr_frame.method().descriptor +
                      " at bytecode " +
                      std::to_string((*osr_result)->exception_bci),
              };
            }
            return ExecutionResult{
                .return_value = (*osr_result)->return_value,
                .throwable = std::nullopt,
                .executed_instructions = executed,
            };
          }
        }
      }
#endif

      ++executed;
      if (watchdog_instructions != std::numeric_limits<u64>::max())
        ++watchdog_instructions;
      accounted_instructions = executed;

      ExecutionFrame &frame = frames.back();
      const usize opcode_pc = frame.pc();
      frame.begin_instruction(opcode_pc);
      current_heap_access_pc = opcode_pc;
      if (heap_access_owner != &frame.owner() ||
          heap_access_method != &frame.method())
      {
        heap_access_owner = &frame.owner();
        heap_access_method = &frame.method();
        set_heap_access_context(HeapAccessContext {
            .owner = frame.owner().name(),
            .method = frame.method().name,
            .descriptor = frame.method().descriptor,
            .bytecode_pc = opcode_pc,
            .live_bytecode_pc = &current_heap_access_pc,
        });
      }
      auto opcode_result = frame.read_opcode();
      if (!opcode_result)
      {
        return std::unexpected(opcode_result.error());
      }
      const u8 opcode = *opcode_result;
      PerformanceCounters::record_opcode(opcode);

      switch (opcode)
      {
      case 0x00:
        break;
      case 0x01:
      {
        auto pushed = frame.push_reference({});
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x02:
      case 0x03:
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07:
      case 0x08:
      {
        auto pushed = frame.push_int(static_cast<i32>(opcode) - 3);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x09:
      case 0x0A:
      {
        auto pushed = frame.push_long(static_cast<i64>(opcode - 0x09));
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x0B:
      case 0x0C:
      case 0x0D:
      {
        auto pushed = frame.push_float(static_cast<float>(opcode - 0x0B));
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x0E:
      case 0x0F:
      {
        auto pushed = frame.push_double(static_cast<double>(opcode - 0x0E));
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x10:
      {
        auto immediate = frame.read_immediate(false);
        if (!immediate)
          return std::unexpected(immediate.error());
        auto pushed = frame.push_int(*immediate);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x11:
      {
        auto immediate = frame.read_immediate(true);
        if (!immediate)
          return std::unexpected(immediate.error());
        auto pushed = frame.push_int(*immediate);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x12:
      case 0x13:
      case 0x14:
      {
        auto index = frame.read_ldc_index(opcode != 0x12);
        if (!index)
          return std::unexpected(index.error());
        auto value = load_constant(frame.owner(), *index, opcode == 0x14);
        if (!value)
          return std::unexpected(value.error());
        if (opcode != 0x14)
        {
          auto constant = frame.owner().constant(*index);
          const auto service = translation_service();
          if (constant &&
              (*constant)->kind == classfile::ConstantKind::string_ref &&
              service && service->enabled())
          {
            std::vector<std::vector<char32_t>> candidates;
            candidates.reserve(6U);
            frame.append_upcoming_translation_candidates(candidates);
            for (const auto& candidate : candidates)
              service->prefetch(candidate);
          }
        }
        auto pushed = frame.push(*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x15:
      {
        auto index = frame.read_local_index();
        if (!index)
          return std::unexpected(index.error());
        auto value = frame.local_int(*index);
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_int(*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x16:
      {
        auto index = frame.read_local_index();
        if (!index)
          return std::unexpected(index.error());
        auto value = frame.local_long(*index);
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_long(*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x17:
      {
        auto index = frame.read_local_index();
        if (!index)
          return std::unexpected(index.error());
        auto value = frame.local_float(*index);
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_float(*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x18:
      {
        auto index = frame.read_local_index();
        if (!index)
          return std::unexpected(index.error());
        auto value = frame.local_double(*index);
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_double(*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x19:
      {
        auto index = frame.read_local_index();
        if (!index)
          return std::unexpected(index.error());
        auto value = frame.local_reference(*index);
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_reference(*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x1A:
      case 0x1B:
      case 0x1C:
      case 0x1D:
      {
        auto value = frame.local_int(local_index_for_load(opcode));
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_int(*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x1E:
      case 0x1F:
      case 0x20:
      case 0x21:
      {
        auto value = frame.local_long(local_index_for_load(opcode));
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_long(*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x22:
      case 0x23:
      case 0x24:
      case 0x25:
      {
        auto value = frame.local_float(local_index_for_load(opcode));
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_float(*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x26:
      case 0x27:
      case 0x28:
      case 0x29:
      {
        auto value = frame.local_double(local_index_for_load(opcode));
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_double(*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x2A:
      case 0x2B:
      case 0x2C:
      case 0x2D:
      {
        auto value = frame.local_reference(local_index_for_load(opcode));
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_reference(*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x2E:
      case 0x2F:
      case 0x30:
      case 0x31:
      case 0x32:
      case 0x33:
      case 0x34:
      case 0x35:
      {
        auto index = pop_int(frame);
        auto array = pop_reference(frame);
        if (!index || !array)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid array load operands");
        }
        if (array->is_null())
        {
          auto raised = raise_implicit("java/lang/NullPointerException",
                                       opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        const auto raise_bounds = [&]()
            -> Result<std::optional<ExecutionResult>>
        {
          if (std::getenv("PHONEME_TRACE_ARRAY_BOUNDS") != nullptr)
          {
            auto info = heap_.vm_array_info(*array);
            std::fprintf(stderr,
                         "[array-bounds] method=%s.%s%s pc=%zu "
                         "index=%d length=%zu\n",
                         frame.owner().name().c_str(),
                         frame.method().name.c_str(),
                         frame.method().descriptor.c_str(),
                         opcode_pc,
                         *index,
                         info ? info->length : 0U);
          }
          return raise_implicit(
              "java/lang/ArrayIndexOutOfBoundsException", opcode_pc);
        };
        if (*index < 0)
        {
          auto raised = raise_bounds();
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        auto snapshot = heap_.vm_array_raw_element_snapshot(
            *array, static_cast<usize>(*index));
        if (!snapshot)
        {
          if (snapshot.error().code != ErrorCode::out_of_range)
            return std::unexpected(snapshot.error());
          auto raised = raise_bounds();
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        if (opcode == 0x33)
        {
          if (snapshot->kind != HeapArrayKind::boolean &&
              snapshot->kind != HeapArrayKind::byte)
          {
            return fail(ErrorCode::malformed_class,
                        "baload target is not byte[] or boolean[]");
          }
        }
        else
        {
          const auto expected = array_load_heap_kind(opcode);
          if (!expected.has_value() || snapshot->kind != *expected)
          {
            return fail(ErrorCode::malformed_class,
                        "array load opcode does not match array element kind");
          }
        }
        Status pushed;
        switch (opcode)
        {
        case 0x2E:
          pushed = frame.push_int(static_cast<i32>(static_cast<u32>(snapshot->raw)));
          break;
        case 0x2F:
          pushed = frame.push_long(static_cast<i64>(snapshot->raw));
          break;
        case 0x30:
          pushed = frame.push_float(std::bit_cast<float>(
              static_cast<u32>(snapshot->raw)));
          break;
        case 0x31:
          pushed = frame.push_double(std::bit_cast<double>(snapshot->raw));
          break;
        case 0x32:
          pushed = frame.push_reference(ObjectRef {snapshot->raw});
          break;
        case 0x33:
          pushed = frame.push_int(snapshot->kind == HeapArrayKind::boolean
              ? (snapshot->raw == 0U ? 0 : 1)
              : static_cast<i32>(static_cast<i8>(snapshot->raw)));
          break;
        case 0x34:
          pushed = frame.push_int(static_cast<i32>(static_cast<u16>(snapshot->raw)));
          break;
        case 0x35:
          pushed = frame.push_int(static_cast<i32>(static_cast<i16>(snapshot->raw)));
          break;
        default:
          return fail(ErrorCode::internal_error,
                      "array load opcode has no raw push path");
        }
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x36:
      {
        auto index = frame.read_local_index();
        auto value = frame.pop_int();
        if (!index || !value)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid integer local store instruction");
        }
        auto stored = frame.set_local_int(*index, *value);
        if (!stored)
          return std::unexpected(stored.error());
        break;
      }
      case 0x37:
      {
        auto index = frame.read_local_index();
        auto value = frame.pop_long();
        if (!index || !value)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid long local store instruction");
        }
        auto stored = frame.set_local_long(*index, *value);
        if (!stored)
          return std::unexpected(stored.error());
        break;
      }
      case 0x38:
      {
        auto index = frame.read_local_index();
        auto value = frame.pop_float();
        if (!index || !value)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid float local store instruction");
        }
        auto stored = frame.set_local_float(*index, *value);
        if (!stored)
          return std::unexpected(stored.error());
        break;
      }
      case 0x39:
      {
        auto index = frame.read_local_index();
        auto value = frame.pop_double();
        if (!index || !value)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid double local store instruction");
        }
        auto stored = frame.set_local_double(*index, *value);
        if (!stored)
          return std::unexpected(stored.error());
        break;
      }
      case 0x3A:
      {
        auto index = frame.read_local_index();
        auto value = frame.pop_reference();
        if (!index || !value)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid reference local store instruction");
        }
        auto stored = frame.set_local_reference(*index, *value);
        if (!stored)
          return std::unexpected(stored.error());
        break;
      }
      case 0x3B:
      case 0x3C:
      case 0x3D:
      case 0x3E:
      {
        auto value = frame.pop_int();
        if (!value)
          return std::unexpected(value.error());
        auto stored = frame.set_local_int(local_index_for_store(opcode), *value);
        if (!stored)
          return std::unexpected(stored.error());
        break;
      }
      case 0x3F:
      case 0x40:
      case 0x41:
      case 0x42:
      {
        auto value = frame.pop_long();
        if (!value)
          return std::unexpected(value.error());
        auto stored = frame.set_local_long(local_index_for_store(opcode), *value);
        if (!stored)
          return std::unexpected(stored.error());
        break;
      }
      case 0x43:
      case 0x44:
      case 0x45:
      case 0x46:
      {
        auto value = frame.pop_float();
        if (!value)
          return std::unexpected(value.error());
        auto stored = frame.set_local_float(local_index_for_store(opcode), *value);
        if (!stored)
          return std::unexpected(stored.error());
        break;
      }
      case 0x47:
      case 0x48:
      case 0x49:
      case 0x4A:
      {
        auto value = frame.pop_double();
        if (!value)
          return std::unexpected(value.error());
        auto stored = frame.set_local_double(local_index_for_store(opcode), *value);
        if (!stored)
          return std::unexpected(stored.error());
        break;
      }
      case 0x4B:
      case 0x4C:
      case 0x4D:
      case 0x4E:
      {
        auto value = frame.pop_reference();
        if (!value)
          return std::unexpected(value.error());
        auto stored = frame.set_local_reference(
            local_index_for_store(opcode), *value);
        if (!stored)
          return std::unexpected(stored.error());
        break;
      }
      case 0x4F:
      case 0x50:
      case 0x51:
      case 0x52:
      case 0x53:
      case 0x54:
      case 0x55:
      case 0x56:
      {
        auto value = frame.pop();
        auto index = pop_int(frame);
        auto array = pop_reference(frame);
        if (!value || !index || !array)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid array store operands");
        }
        if (array->is_null())
        {
          auto raised = raise_implicit("java/lang/NullPointerException",
                                       opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        if (*index < 0)
        {
          auto raised = raise_implicit(
              "java/lang/ArrayIndexOutOfBoundsException", opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }

        const auto element_index = static_cast<usize>(*index);
        HeapArrayKind store_kind {};
        u64 store_raw = 0U;
        if (opcode == 0x54)
        {
          auto info = heap_.vm_array_info(*array);
          if (!info)
            return std::unexpected(info.error());
          if (element_index >= info->length)
          {
            auto raised = raise_implicit(
                "java/lang/ArrayIndexOutOfBoundsException", opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          if (info->kind != HeapArrayKind::boolean &&
              info->kind != HeapArrayKind::byte)
          {
            return fail(ErrorCode::malformed_class,
                        "bastore target is not byte[] or boolean[]");
          }
          if (value->kind() != ValueKind::int32)
            return fail(ErrorCode::malformed_class,
                        "bastore value is not an int");
          const i32 integer = static_cast<i32>(
              static_cast<u32>(value->raw_bits_unchecked()));
          store_kind = info->kind;
          store_raw = info->kind == HeapArrayKind::boolean
                          ? static_cast<u64>((integer & 1) == 0 ? 0U : 1U)
                          : static_cast<u64>(static_cast<u8>(
                                static_cast<i8>(integer)));
        }
        else if (opcode == 0x53)
        {
          auto info = heap_.vm_array_info(*array);
          if (!info)
            return std::unexpected(info.error());
          if (element_index >= info->length)
          {
            auto raised = raise_implicit(
                "java/lang/ArrayIndexOutOfBoundsException", opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          if (info->kind != HeapArrayKind::reference ||
              info->reference_component.empty())
          {
            return fail(ErrorCode::malformed_class,
                        "aastore target is not a reference array");
          }
          if (value->kind() != ValueKind::reference)
            return fail(ErrorCode::malformed_class,
                        "aastore value is not a reference");
          const ObjectRef stored_reference = value->reference_unchecked();
          if (!stored_reference.is_null())
          {
            auto source_class = heap_.vm_class_name_view(stored_reference);
            if (!source_class)
              return std::unexpected(source_class.error());
            auto assignable = classes_.is_assignable(
                *source_class, info->reference_component);
            if (!assignable)
              return std::unexpected(assignable.error());
            if (!*assignable)
            {
              auto raised = raise_implicit(
                  "java/lang/ArrayStoreException", opcode_pc);
              if (!raised)
                return std::unexpected(raised.error());
              if (raised->has_value())
                return std::move(**raised);
              break;
            }
          }
          store_kind = HeapArrayKind::reference;
          store_raw = stored_reference.bits;
        }
        else
        {
          const auto expected_kind = array_store_heap_kind(opcode);
          if (!expected_kind.has_value())
          {
            return fail(ErrorCode::malformed_class,
                        "array store opcode has no heap kind");
          }
          store_kind = *expected_kind;
          const ValueKind expected_value_kind =
              opcode == 0x50 ? ValueKind::int64 :
              opcode == 0x51 ? ValueKind::float32 :
              opcode == 0x52 ? ValueKind::float64 :
              ValueKind::int32;
          if (value->kind() != expected_value_kind)
            return fail(ErrorCode::malformed_class,
                        "array store value kind does not match opcode");
          store_raw = value->raw_bits_unchecked();
          if (opcode == 0x55)
          {
            store_raw = static_cast<u64>(static_cast<u16>(store_raw));
          }
          else if (opcode == 0x56)
          {
            store_raw = static_cast<u64>(static_cast<u16>(
                static_cast<i16>(static_cast<u32>(store_raw))));
          }
        }

        auto stored = heap_.vm_set_array_raw_element_checked(
            *array, element_index, store_kind, store_raw);
        if (!stored)
        {
          if (stored.error().code == ErrorCode::out_of_range)
          {
            auto raised = raise_implicit(
                "java/lang/ArrayIndexOutOfBoundsException", opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          return std::unexpected(stored.error());
        }
        break;
      }
      case 0x57:
      {
        auto value = frame.pop_compact();
        if (!value)
          return std::unexpected(value.error());
        if (value->category_two())
        {
          return fail(ErrorCode::malformed_class,
                      "pop cannot consume a category-2 value");
        }
        break;
      }
      case 0x58:
      {
        auto first = frame.pop_compact();
        if (!first)
          return std::unexpected(first.error());
        if (!first->category_two())
        {
          auto second = frame.pop_compact();
          if (!second)
            return std::unexpected(second.error());
          if (second->category_two())
          {
            return fail(ErrorCode::malformed_class,
                        "pop2 has an invalid category layout");
          }
        }
        break;
      }
      case 0x59:
      {
        auto value = frame.pop_compact();
        if (!value)
          return std::unexpected(value.error());
        if (value->category_two())
        {
          return fail(ErrorCode::malformed_class,
                      "dup cannot duplicate a category-2 value");
        }
        auto first = frame.push_compact(*value);
        auto second = frame.push_compact(*value);
        if (!first || !second)
        {
          return fail(ErrorCode::malformed_class,
                      "dup exceeds max_stack");
        }
        break;
      }
      case 0x5A:
      {
        auto value1 = frame.pop_compact();
        auto value2 = frame.pop_compact();
        if (!value1 || !value2 || value1->category_two() ||
            value2->category_two())
        {
          return fail(ErrorCode::malformed_class,
                      "dup_x1 requires two category-1 values");
        }
        if (!frame.push_compact(*value1) || !frame.push_compact(*value2) ||
            !frame.push_compact(*value1))
        {
          return fail(ErrorCode::malformed_class,
                      "dup_x1 exceeds max_stack");
        }
        break;
      }
      case 0x5B:
      {
        auto value1 = frame.pop_compact();
        auto value2 = frame.pop_compact();
        if (!value1 || !value2 || value1->category_two())
        {
          return fail(ErrorCode::malformed_class,
                      "dup_x2 has an invalid category layout");
        }
        if (value2->category_two())
        {
          if (!frame.push_compact(*value1) || !frame.push_compact(*value2) ||
              !frame.push_compact(*value1))
          {
            return fail(ErrorCode::malformed_class,
                        "dup_x2 exceeds max_stack");
          }
        }
        else
        {
          auto value3 = frame.pop_compact();
          if (!value3 || value3->category_two())
          {
            return fail(ErrorCode::malformed_class,
                        "dup_x2 has an invalid category layout");
          }
          if (!frame.push_compact(*value1) || !frame.push_compact(*value3) ||
              !frame.push_compact(*value2) || !frame.push_compact(*value1))
          {
            return fail(ErrorCode::malformed_class,
                        "dup_x2 exceeds max_stack");
          }
        }
        break;
      }
      case 0x5C:
      {
        auto value1 = frame.pop_compact();
        if (!value1)
          return std::unexpected(value1.error());
        if (value1->category_two())
        {
          if (!frame.push_compact(*value1) || !frame.push_compact(*value1))
          {
            return fail(ErrorCode::malformed_class,
                        "dup2 exceeds max_stack");
          }
        }
        else
        {
          auto value2 = frame.pop_compact();
          if (!value2 || value2->category_two())
          {
            return fail(ErrorCode::malformed_class,
                        "dup2 has an invalid category layout");
          }
          if (!frame.push_compact(*value2) || !frame.push_compact(*value1) ||
              !frame.push_compact(*value2) || !frame.push_compact(*value1))
          {
            return fail(ErrorCode::malformed_class,
                        "dup2 exceeds max_stack");
          }
        }
        break;
      }
      case 0x5D:
      {
        auto value1 = frame.pop_compact();
        if (!value1)
          return std::unexpected(value1.error());
        if (value1->category_two())
        {
          auto value2 = frame.pop_compact();
          if (!value2 || value2->category_two())
          {
            return fail(ErrorCode::malformed_class,
                        "dup2_x1 has an invalid category layout");
          }
          if (!frame.push_compact(*value1) || !frame.push_compact(*value2) ||
              !frame.push_compact(*value1))
          {
            return fail(ErrorCode::malformed_class,
                        "dup2_x1 exceeds max_stack");
          }
        }
        else
        {
          auto value2 = frame.pop_compact();
          auto value3 = frame.pop_compact();
          if (!value2 || !value3 || value2->category_two() ||
              value3->category_two())
          {
            return fail(ErrorCode::malformed_class,
                        "dup2_x1 has an invalid category layout");
          }
          if (!frame.push_compact(*value2) || !frame.push_compact(*value1) ||
              !frame.push_compact(*value3) || !frame.push_compact(*value2) ||
              !frame.push_compact(*value1))
          {
            return fail(ErrorCode::malformed_class,
                        "dup2_x1 exceeds max_stack");
          }
        }
        break;
      }
      case 0x5E:
      {
        auto value1 = frame.pop_compact();
        if (!value1)
          return std::unexpected(value1.error());
        if (value1->category_two())
        {
          auto value2 = frame.pop_compact();
          if (!value2)
            return std::unexpected(value2.error());
          if (value2->category_two())
          {
            if (!frame.push_compact(*value1) ||
                !frame.push_compact(*value2) ||
                !frame.push_compact(*value1))
              return fail(ErrorCode::malformed_class,
                          "dup2_x2 exceeds max_stack");
          }
          else
          {
            auto value3 = frame.pop_compact();
            if (!value3 || value3->category_two())
            {
              return fail(ErrorCode::malformed_class,
                          "dup2_x2 has an invalid category layout");
            }
            if (!frame.push_compact(*value1) ||
                !frame.push_compact(*value3) ||
                !frame.push_compact(*value2) ||
                !frame.push_compact(*value1))
              return fail(ErrorCode::malformed_class,
                          "dup2_x2 exceeds max_stack");
          }
        }
        else
        {
          auto value2 = frame.pop_compact();
          auto value3 = frame.pop_compact();
          if (!value2 || !value3 || value2->category_two())
          {
            return fail(ErrorCode::malformed_class,
                        "dup2_x2 has an invalid category layout");
          }
          if (value3->category_two())
          {
            if (!frame.push_compact(*value2) ||
                !frame.push_compact(*value1) ||
                !frame.push_compact(*value3) ||
                !frame.push_compact(*value2) ||
                !frame.push_compact(*value1))
              return fail(ErrorCode::malformed_class,
                          "dup2_x2 exceeds max_stack");
          }
          else
          {
            auto value4 = frame.pop_compact();
            if (!value4 || value4->category_two())
            {
              return fail(ErrorCode::malformed_class,
                          "dup2_x2 has an invalid category layout");
            }
            if (!frame.push_compact(*value2) ||
                !frame.push_compact(*value1) ||
                !frame.push_compact(*value4) ||
                !frame.push_compact(*value3) ||
                !frame.push_compact(*value2) ||
                !frame.push_compact(*value1))
              return fail(ErrorCode::malformed_class,
                          "dup2_x2 exceeds max_stack");
          }
        }
        break;
      }
      case 0x5F:
      {
        auto value1 = frame.pop_compact();
        auto value2 = frame.pop_compact();
        if (!value1 || !value2 || value1->category_two() ||
            value2->category_two())
        {
          return fail(ErrorCode::malformed_class,
                      "swap requires two category-1 values");
        }
        if (!frame.push_compact(*value1) || !frame.push_compact(*value2))
        {
          return fail(ErrorCode::malformed_class,
                      "swap exceeds max_stack");
        }
        break;
      }
      case 0x60:
      case 0x64:
      case 0x68:
      case 0x6C:
      case 0x70:
      case 0x7E:
      case 0x80:
      case 0x82:
      {
        auto result = integer_binary(frame, opcode);
        if (!result)
          return std::unexpected(result.error());
        if (!result->has_value())
        {
          auto raised = raise_implicit("java/lang/ArithmeticException",
                                       opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        auto pushed = frame.push_int(**result);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x61:
      case 0x65:
      case 0x69:
      case 0x6D:
      case 0x71:
      case 0x7F:
      case 0x81:
      case 0x83:
      {
        auto result = long_binary(frame, opcode);
        if (!result)
          return std::unexpected(result.error());
        if (!result->has_value())
        {
          auto raised = raise_implicit("java/lang/ArithmeticException",
                                       opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        auto pushed = frame.push_long(**result);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x62:
      case 0x66:
      case 0x6A:
      case 0x6E:
      case 0x72:
      {
        auto result = float_binary(frame, opcode);
        if (!result)
          return std::unexpected(result.error());
        auto pushed = frame.push_float(*result);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x63:
      case 0x67:
      case 0x6B:
      case 0x6F:
      case 0x73:
      {
        auto result = double_binary(frame, opcode);
        if (!result)
          return std::unexpected(result.error());
        auto pushed = frame.push_double(*result);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x74:
      {
        auto value = pop_int(frame);
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_int(
            static_cast<i32>(0U - static_cast<u32>(*value)));
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x75:
      {
        auto value = pop_long(frame);
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_long(
            static_cast<i64>(0ULL - static_cast<u64>(*value)));
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x76:
      {
        auto value = pop_float(frame);
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_float(-*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x77:
      {
        auto value = pop_double(frame);
        if (!value)
          return std::unexpected(value.error());
        auto pushed = frame.push_double(-*value);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x78:
      case 0x7A:
      case 0x7C:
      {
        auto distance = pop_int(frame);
        auto value = pop_int(frame);
        if (!distance || !value)
        {
          return fail(ErrorCode::malformed_class,
                      "integer shift requires two int operands");
        }
        const u32 shift = static_cast<u32>(*distance) & 0x1FU;
        const i32 result = opcode == 0x78
                               ? static_cast<i32>(static_cast<u32>(*value) << shift)
                           : opcode == 0x7A
                               ? (*value >> shift)
                               : static_cast<i32>(static_cast<u32>(*value) >> shift);
        auto pushed = frame.push_int(result);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x79:
      case 0x7B:
      case 0x7D:
      {
        auto distance = pop_int(frame);
        auto value = pop_long(frame);
        if (!distance || !value)
        {
          return fail(ErrorCode::malformed_class,
                      "long shift requires long and int operands");
        }
        const u32 shift = static_cast<u32>(*distance) & 0x3FU;
        const i64 result = opcode == 0x79
                               ? static_cast<i64>(static_cast<u64>(*value) << shift)
                           : opcode == 0x7B
                               ? (*value >> shift)
                               : static_cast<i64>(static_cast<u64>(*value) >> shift);
        auto pushed = frame.push_long(result);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x84:
      {
        auto operands = frame.read_increment_operands();
        if (!operands)
          return std::unexpected(operands.error());
        auto current = frame.local_int(operands->local_index);
        if (!current)
          return std::unexpected(current.error());
        const i32 updated = static_cast<i32>(
            static_cast<u32>(*current) +
            static_cast<u32>(operands->increment));
        auto stored = frame.set_local_int(operands->local_index, updated);
        if (!stored)
          return std::unexpected(stored.error());
        break;
      }
      case 0x85:
      case 0x86:
      case 0x87:
      {
        auto value = pop_int(frame);
        if (!value)
          return std::unexpected(value.error());
        Status pushed;
        if (opcode == 0x85)
        {
          pushed = frame.push_long(static_cast<i64>(*value));
        }
        else if (opcode == 0x86)
        {
          pushed = frame.push_float(static_cast<float>(*value));
        }
        else
        {
          pushed = frame.push_double(static_cast<double>(*value));
        }
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x88:
      case 0x89:
      case 0x8A:
      {
        auto value = pop_long(frame);
        if (!value)
          return std::unexpected(value.error());
        Status pushed;
        if (opcode == 0x88)
        {
          const i32 narrowed = static_cast<i32>(
              static_cast<u32>(static_cast<u64>(*value)));
          pushed = frame.push_int(narrowed);
        }
        else if (opcode == 0x89)
        {
          pushed = frame.push_float(static_cast<float>(*value));
        }
        else
        {
          pushed = frame.push_double(static_cast<double>(*value));
        }
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x8B:
      case 0x8C:
      case 0x8D:
      {
        auto value = pop_float(frame);
        if (!value)
          return std::unexpected(value.error());
        Status pushed;
        if (opcode == 0x8B)
        {
          pushed = frame.push_int(java_fp_to_integral<i32>(*value));
        }
        else if (opcode == 0x8C)
        {
          pushed = frame.push_long(java_fp_to_integral<i64>(*value));
        }
        else
        {
          pushed = frame.push_double(static_cast<double>(*value));
        }
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x8E:
      case 0x8F:
      case 0x90:
      {
        auto value = pop_double(frame);
        if (!value)
          return std::unexpected(value.error());
        Status pushed;
        if (opcode == 0x8E)
        {
          pushed = frame.push_int(java_fp_to_integral<i32>(*value));
        }
        else if (opcode == 0x8F)
        {
          pushed = frame.push_long(java_fp_to_integral<i64>(*value));
        }
        else
        {
          pushed = frame.push_float(static_cast<float>(*value));
        }
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x91:
      case 0x92:
      case 0x93:
      {
        auto value = pop_int(frame);
        if (!value)
          return std::unexpected(value.error());
        i32 converted = 0;
        if (opcode == 0x91)
        {
          converted = static_cast<i32>(static_cast<i8>(*value));
        }
        else if (opcode == 0x92)
        {
          converted = static_cast<i32>(static_cast<u16>(*value));
        }
        else
        {
          converted = static_cast<i32>(static_cast<i16>(*value));
        }
        auto pushed = frame.push_int(converted);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x94:
      {
        auto right = pop_long(frame);
        auto left = pop_long(frame);
        if (!right || !left)
        {
          return fail(ErrorCode::malformed_class,
                      "lcmp requires two long operands");
        }
        const i32 comparison = *left < *right ? -1 : (*left > *right ? 1 : 0);
        auto pushed = frame.push_int(comparison);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x95:
      case 0x96:
      {
        auto right = pop_float(frame);
        auto left = pop_float(frame);
        if (!right || !left)
        {
          return fail(ErrorCode::malformed_class,
                      "float compare requires two float operands");
        }
        i32 comparison = 0;
        if (std::isnan(*left) || std::isnan(*right))
        {
          comparison = opcode == 0x95 ? -1 : 1;
        }
        else
        {
          comparison = *left < *right ? -1 : (*left > *right ? 1 : 0);
        }
        auto pushed = frame.push_int(comparison);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x97:
      case 0x98:
      {
        auto right = pop_double(frame);
        auto left = pop_double(frame);
        if (!right || !left)
        {
          return fail(ErrorCode::malformed_class,
                      "double compare requires two double operands");
        }
        i32 comparison = 0;
        if (std::isnan(*left) || std::isnan(*right))
        {
          comparison = opcode == 0x97 ? -1 : 1;
        }
        else
        {
          comparison = *left < *right ? -1 : (*left > *right ? 1 : 0);
        }
        auto pushed = frame.push_int(comparison);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0x99:
      case 0x9A:
      case 0x9B:
      case 0x9C:
      case 0x9D:
      case 0x9E:
      {
        auto offset = frame.read_branch_offset(false);
        auto value = pop_int(frame);
        if (!offset || !value)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid zero-comparison branch");
        }
        if (test_zero(opcode, *value))
        {
          auto branched = frame.branch(opcode_pc, *offset);
          if (!branched)
            return std::unexpected(branched.error());
        }
        break;
      }
      case 0x9F:
      case 0xA0:
      case 0xA1:
      case 0xA2:
      case 0xA3:
      case 0xA4:
      {
        auto offset = frame.read_branch_offset(false);
        auto right = pop_int(frame);
        auto left = pop_int(frame);
        if (!offset || !right || !left)
        {
          return fail(
              ErrorCode::malformed_class,
              "invalid integer comparison branch in " +
                  frame.owner().name() + "." + frame.method().name +
                  frame.method().descriptor + " at bytecode " +
                  std::to_string(opcode_pc) +
                  " (offset=" + (offset ? std::string("ok") : "invalid") +
                  ", right=" + (right ? std::string("ok") : "invalid") +
                  ", left=" + (left ? std::string("ok") : "invalid") + ")");
        }
        if (test_int_compare(opcode, *left, *right))
        {
          auto branched = frame.branch(opcode_pc, *offset);
          if (!branched)
            return std::unexpected(branched.error());
        }
        break;
      }
      case 0xA5:
      case 0xA6:
      {
        auto offset = frame.read_branch_offset(false);
        auto right = pop_reference(frame);
        auto left = pop_reference(frame);
        if (!offset || !right || !left)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid reference comparison branch");
        }
        const bool equal = *left == *right;
        if ((opcode == 0xA5 && equal) || (opcode == 0xA6 && !equal))
        {
          auto branched = frame.branch(opcode_pc, *offset);
          if (!branched)
            return std::unexpected(branched.error());
        }
        break;
      }
      case 0xA7:
      {
        auto offset = frame.read_branch_offset(false);
        if (!offset)
          return std::unexpected(offset.error());
        auto branched = frame.branch(opcode_pc, *offset);
        if (!branched)
          return std::unexpected(branched.error());
        break;
      }
      case 0xA8:
      {
        auto offset = frame.read_branch_offset(false);
        if (!offset)
          return std::unexpected(offset.error());
        const usize return_pc = frame.pc();
        auto pushed = frame.push(Value::return_address(return_pc));
        if (!pushed)
          return std::unexpected(pushed.error());
        auto branched = frame.branch(opcode_pc, *offset);
        if (!branched)
          return std::unexpected(branched.error());
        break;
      }
      case 0xA9:
      {
        auto index = frame.read_local_index();
        if (!index)
          return std::unexpected(index.error());
        auto address_value = frame.local(*index);
        if (!address_value)
          return std::unexpected(address_value.error());
        auto address = address_value->as_return_address();
        if (!address)
          return std::unexpected(address.error());
        auto jumped = frame.jump_absolute(*address);
        if (!jumped)
          return std::unexpected(jumped.error());
        break;
      }
      case 0xAA:
      {
        auto key = pop_int(frame);
        if (!key)
          return std::unexpected(key.error());
        auto decoded_table = frame.read_switch_table();
        if (!decoded_table)
          return std::unexpected(decoded_table.error());
        if (*decoded_table != nullptr)
        {
          const DecodedSwitchTable& table = **decoded_table;
          u32 target_bci = table.default_target_bci;
          const i64 entry_index = static_cast<i64>(*key) -
                                  static_cast<i64>(table.low);
          if (entry_index >= 0 &&
              static_cast<u64>(entry_index) < table.entries.size())
          {
            target_bci = table.entries[
                static_cast<usize>(entry_index)].target_bci;
          }
          auto jumped = frame.jump_absolute(target_bci);
          if (!jumped)
            return std::unexpected(jumped.error());
          break;
        }
        auto padding = frame.read_switch_padding();
        if (!padding)
          return std::unexpected(padding.error());
        auto default_offset = frame.read_i32();
        auto low = frame.read_i32();
        auto high = frame.read_i32();
        if (!default_offset || !low || !high)
        {
          return fail(ErrorCode::malformed_class,
                      "truncated tableswitch header");
        }
        if (*high < *low)
        {
          return fail(ErrorCode::malformed_class,
                      "tableswitch high key is below low key");
        }
        const u64 entry_count = static_cast<u64>(
            static_cast<i64>(*high) - static_cast<i64>(*low) + 1);
        if (entry_count > static_cast<u64>(frame.code_size() / 4U))
        {
          return fail(ErrorCode::malformed_class,
                      "tableswitch entry count exceeds method bounds");
        }
        i32 selected_offset = *default_offset;
        for (u64 entry = 0; entry < entry_count; ++entry)
        {
          auto offset = frame.read_i32();
          if (!offset)
            return std::unexpected(offset.error());
          const i64 match_key = static_cast<i64>(*low) +
                                static_cast<i64>(entry);
          if (static_cast<i64>(*key) == match_key)
          {
            selected_offset = *offset;
          }
        }
        auto branched = frame.branch(opcode_pc, selected_offset);
        if (!branched)
          return std::unexpected(branched.error());
        break;
      }
      case 0xAB:
      {
        auto key = pop_int(frame);
        if (!key)
          return std::unexpected(key.error());
        auto decoded_table = frame.read_switch_table();
        if (!decoded_table)
          return std::unexpected(decoded_table.error());
        if (*decoded_table != nullptr)
        {
          const DecodedSwitchTable& table = **decoded_table;
          u32 target_bci = table.default_target_bci;
          const auto found = std::lower_bound(
              table.entries.begin(),
              table.entries.end(),
              *key,
              [](const DecodedSwitchEntry& entry, i32 match)
              {
                return entry.match < match;
              });
          if (found != table.entries.end() && found->match == *key)
            target_bci = found->target_bci;
          auto jumped = frame.jump_absolute(target_bci);
          if (!jumped)
            return std::unexpected(jumped.error());
          break;
        }
        auto padding = frame.read_switch_padding();
        if (!padding)
          return std::unexpected(padding.error());
        auto default_offset = frame.read_i32();
        auto pair_count = frame.read_i32();
        if (!default_offset || !pair_count || *pair_count < 0)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid lookupswitch header");
        }
        const u64 count = static_cast<u64>(*pair_count);
        if (count > static_cast<u64>(frame.code_size() / 8U))
        {
          return fail(ErrorCode::malformed_class,
                      "lookupswitch pair count exceeds method bounds");
        }
        i32 selected_offset = *default_offset;
        std::optional<i32> previous_match;
        for (u64 pair = 0; pair < count; ++pair)
        {
          auto match = frame.read_i32();
          auto offset = frame.read_i32();
          if (!match || !offset)
          {
            return fail(ErrorCode::malformed_class,
                        "truncated lookupswitch pair");
          }
          if (previous_match.has_value() &&
              *match <= *previous_match)
          {
            return fail(ErrorCode::malformed_class,
                        "lookupswitch keys are not strictly ordered");
          }
          previous_match = *match;
          if (*key == *match)
          {
            selected_offset = *offset;
          }
        }
        auto branched = frame.branch(opcode_pc, selected_offset);
        if (!branched)
          return std::unexpected(branched.error());
        break;
      }
      case 0xAC:
      case 0xAD:
      case 0xAE:
      case 0xAF:
      case 0xB0:
      {
        auto value = frame.pop();
        if (!value)
          return std::unexpected(value.error());
        const JavaTypeKind return_kind = frame.descriptor().return_type.kind;
        const bool matches =
            (opcode == 0xAC && value->kind() == ValueKind::int32 &&
             return_kind != JavaTypeKind::void_type) ||
            (opcode == 0xAD && value->kind() == ValueKind::int64 &&
             return_kind == JavaTypeKind::long_integer) ||
            (opcode == 0xAE && value->kind() == ValueKind::float32 &&
             return_kind == JavaTypeKind::float32) ||
            (opcode == 0xAF && value->kind() == ValueKind::float64 &&
             return_kind == JavaTypeKind::float64) ||
            (opcode == 0xB0 && value->kind() == ValueKind::reference &&
             frame.descriptor().return_type.reference_like());
        if (!matches)
        {
          return fail(ErrorCode::malformed_class,
                      "return opcode does not match method descriptor");
        }
        auto completed = complete_return(*value);
        if (!completed)
          return std::unexpected(completed.error());
        if (completed->has_value())
          return std::move(**completed);
        break;
      }
      case 0xB1:
      {
        if (frame.descriptor().return_type.kind != JavaTypeKind::void_type)
        {
          return fail(ErrorCode::malformed_class,
                      "void return used by a non-void method");
        }
        auto completed = complete_return(std::nullopt);
        if (!completed)
          return std::unexpected(completed.error());
        if (completed->has_value())
          return std::move(**completed);
        break;
      }
      case 0xB2:
      case 0xB3:
      case 0xB4:
      case 0xB5:
      {
        auto index = frame.read_constant_pool_index();
        if (!index)
          return std::unexpected(index.error());
        const bool is_static = opcode == 0xB2 || opcode == 0xB3;
        refresh_metadata_bindings_if_needed();
        // FieldLocation contains a Machine-local FieldId and therefore must
        // never be cached in RuntimeMetadata, which is shared by every Machine
        // using the same ClassRepository. Keep field bindings scoped to this
        // Machine; decoded bytecode may still cache machine-neutral method and
        // class linkage in the shared operand side table.
        auto binding = field_binding_slot(frame.owner(), *index);
        if (!binding)
          return std::unexpected(binding.error());
        if ((**binding).has_value())
        {
          PerformanceCounters::record_operand_resolution(true);
          if ((**binding)->is_static != is_static)
          {
            auto raised = raise_implicit(
                "java/lang/IncompatibleClassChangeError", opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
        }
        else
        {
          PerformanceCounters::record_operand_resolution(false);
          auto reference = frame.owner().member_reference(*index);
          if (!reference)
            return std::unexpected(reference.error());
          if (reference->kind != classfile::ConstantKind::field_ref)
          {
            return fail(ErrorCode::malformed_class,
                        "field opcode references a non-field constant");
          }
          auto resolved = resolve_quick_field_binding(*reference, is_static);
          if (!resolved)
          {
            PerformanceCounters::record_operand_resolution_failure();
            auto raised = raise_resolution_error(stable_linkage_error(
                resolved.error(), OperandResolutionKind::field), opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          **binding = std::move(*resolved);
        }
        const QuickFieldBinding& field = (**binding).value();
        if (is_static)
        {
          if (field.declaring_runtime_class == nullptr)
            return fail(ErrorCode::internal_error,
                        "quick static field has no declaring runtime class");
          auto initialized = ensure_initialized_from_execution(
              field.declaring_runtime_class->class_file->name(),
              remaining_execution_budget());
          if (!initialized)
            return std::unexpected(initialized.error());
          if (initialized->has_value())
          {
            auto dispatched = dispatch_exception(**initialized,
                                                 opcode_pc);
            if (!dispatched)
            {
              return std::unexpected(dispatched.error());
            }
            if (dispatched->has_value())
            {
              return std::move(**dispatched);
            }
            break;
          }
        }

        if (opcode == 0xB2)
        {
          auto value = states_.static_field(field.id);
          if (!value)
            return std::unexpected(value.error());
          if (field.string_constant_value_index.has_value())
          {
            auto current = value->as_reference();
            if (!current)
              return std::unexpected(current.error());
            if (current->is_null())
            {
              if (field.declaring_runtime_class == nullptr)
                return fail(ErrorCode::internal_error,
                            "String ConstantValue field lost declaring class");
              auto encoded = field.declaring_runtime_class->class_file->string_constant(
                  *field.string_constant_value_index);
              if (!encoded)
                return std::unexpected(encoded.error());
              auto string = intern_string(*encoded);
              if (!string)
                return std::unexpected(string.error());
              value = Value::from_reference(*string);
              auto stored = states_.set_static_field(
                  field.id, field.value_kind, *value);
              if (!stored)
                return std::unexpected(stored.error());
            }
          }
          auto pushed = frame.push(*value);
          if (!pushed)
            return std::unexpected(pushed.error());
        }
        else if (opcode == 0xB3)
        {
          auto value = frame.pop();
          if (!value)
            return std::unexpected(value.error());
          auto stored = states_.set_static_field(
              field.id, field.value_kind, *value);
          if (!stored)
            return std::unexpected(stored.error());
        }
        else if (opcode == 0xB4)
        {
          auto object = pop_reference(frame);
          if (!object)
            return std::unexpected(object.error());
          if (object->is_null())
          {
            auto raised = raise_implicit("java/lang/NullPointerException",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          auto value = heap_.vm_field_typed(
              *object, field.index, field.value_kind);
          if (!value)
            return std::unexpected(value.error());
          auto pushed = frame.push(*value);
          if (!pushed)
            return std::unexpected(pushed.error());
        }
        else
        {
          auto value = frame.pop();
          auto object = pop_reference(frame);
          if (!value || !object)
          {
            return fail(ErrorCode::malformed_class,
                        "invalid putfield operands");
          }
          if (object->is_null())
          {
            auto raised = raise_implicit("java/lang/NullPointerException",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          auto stored = heap_.vm_set_field_typed(
              *object, field.index, field.value_kind, *value);
          if (!stored)
            return std::unexpected(stored.error());
        }
        break;
      }
      case 0xB6:
      case 0xB7:
      case 0xB8:
      case 0xB9:
      {
        std::optional<u16> index;
        std::optional<u8> interface_count;
        if (opcode == 0xB9)
        {
          auto operands = frame.read_invokeinterface_operands();
          if (!operands)
            return std::unexpected(operands.error());
          index = operands->constant_pool_index;
          interface_count = operands->count;
        }
        else
        {
          auto constant_pool_index = frame.read_constant_pool_index();
          if (!constant_pool_index)
            return std::unexpected(constant_pool_index.error());
          index = *constant_pool_index;
        }
        refresh_metadata_bindings_if_needed();
        auto site = invoke_site_binding(frame.owner(), *index);
        if (!site)
          return std::unexpected(site.error());
        const classfile::MemberReference* reference = &(*site)->reference;
        if ((opcode == 0xB6 &&
             reference->kind != classfile::ConstantKind::method_ref) ||
            (opcode == 0xB9 &&
             reference->kind !=
                 classfile::ConstantKind::interface_method_ref) ||
            ((opcode == 0xB7 || opcode == 0xB8) &&
             reference->kind != classfile::ConstantKind::method_ref &&
             reference->kind !=
                 classfile::ConstantKind::interface_method_ref))
        {
          return fail(ErrorCode::malformed_class,
                      "invoke opcode uses an incompatible constant kind");
        }
        const bool is_static = opcode == 0xB8;
        const auto& descriptor = (*site)->descriptor;
        if (interface_count.has_value())
        {
          const usize expected_slots =
              descriptor->argument_slots_with_receiver;
          if (expected_slots >
                  static_cast<usize>(std::numeric_limits<u8>::max()) ||
              *interface_count != static_cast<u8>(expected_slots))
          {
            return fail(ErrorCode::malformed_class,
                        "invokeinterface count does not match descriptor");
          }
        }
        auto arguments = pop_arguments(
            frame,
            descriptor->descriptor,
            !is_static,
            !frame.has_verified_types());
        if (!arguments)
          return std::unexpected(arguments.error());
        if (!is_static)
        {
          const ObjectRef receiver = arguments->reference_unchecked(0U);
          if (receiver.is_null())
          {
            auto raised = raise_implicit("java/lang/NullPointerException",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
        }

        if (is_static)
        {
          auto initialized = ensure_initialized_from_compact_execution(
              reference->owner,
              remaining_execution_budget(),
              *arguments);
          if (!initialized)
            return std::unexpected(initialized.error());
          if (initialized->has_value())
          {
            auto dispatched = dispatch_exception(**initialized,
                                                 opcode_pc);
            if (!dispatched)
            {
              return std::unexpected(dispatched.error());
            }
            if (dispatched->has_value())
            {
              return std::move(**dispatched);
            }
            break;
          }
        }

        if (is_static &&
            reference->owner == "java/lang/String" &&
            reference->name == "valueOf" &&
            reference->descriptor == "(I)Ljava/lang/String;" &&
            arguments->size() == 1U)
        {
          auto value = (*arguments)[0U].as_int();
          if (!value)
            return std::unexpected(value.error());
          auto text = format_java_int(*value);
          if (!text)
            return std::unexpected(text.error());
          // String.valueOf(int) returns a freshly formatted String. Keep that
          // observable allocation identity while bypassing method resolution,
          // Invocation/native dispatch and the old allocate+attach double lock.
          auto result = states_.allocate_text_instance(
              heap_, "java/lang/String", std::move(*text));
          if (!result)
            return std::unexpected(result.error());
          auto pushed = frame.push_reference(*result);
          if (!pushed)
            return std::unexpected(pushed.error());
          if (budget_mode == InstructionBudgetMode::progress_watchdog)
            watchdog_instructions = 0U;
          break;
        }

        // String is final, so an invokevirtual whose symbolic owner is exactly
        // java/lang/String cannot be overridden. These tiny accessors dominate
        // the current interpreter profile; executing them through method
        // resolution, Invocation construction, NativeMethodRegistry and the
        // public heap mutex costs far more than the payload read itself.
        if (!is_static && opcode == 0xB6 &&
            reference->owner == "java/lang/String")
        {
          if (reference->name == "length" &&
              reference->descriptor == "()I" && arguments->size() == 1U)
          {
            const ObjectRef receiver = arguments->front().reference_unchecked();
            auto length = heap_.vm_string_length(receiver);
            if (!length)
              return std::unexpected(length.error());
            if (*length >
                static_cast<usize>(std::numeric_limits<i32>::max()))
            {
              return fail(ErrorCode::overflow,
                          "Java String length exceeds int range");
            }
            auto pushed = frame.push_int(static_cast<i32>(*length));
            if (!pushed)
              return std::unexpected(pushed.error());
            if (budget_mode == InstructionBudgetMode::progress_watchdog)
              watchdog_instructions = 0U;
            break;
          }

          if (reference->name == "charAt" &&
              reference->descriptor == "(I)C" && arguments->size() == 2U)
          {
            const ObjectRef receiver = arguments->front().reference_unchecked();
            auto character_index = (*arguments)[1U].as_int();
            if (!character_index)
              return std::unexpected(character_index.error());
            if (*character_index < 0)
            {
              auto raised = raise_implicit(
                  "java/lang/StringIndexOutOfBoundsException", opcode_pc);
              if (!raised)
                return std::unexpected(raised.error());
              if (raised->has_value())
                return std::move(**raised);
              break;
            }
            auto character = heap_.vm_string_character(
                receiver, static_cast<usize>(*character_index));
            if (!character)
            {
              if (character.error().code != ErrorCode::out_of_range)
                return std::unexpected(character.error());
              auto raised = raise_implicit(
                  "java/lang/StringIndexOutOfBoundsException", opcode_pc);
              if (!raised)
                return std::unexpected(raised.error());
              if (raised->has_value())
                return std::move(**raised);
              break;
            }
            auto pushed = frame.push_int(static_cast<i32>(*character));
            if (!pushed)
              return std::unexpected(pushed.error());
            if (budget_mode == InstructionBudgetMode::progress_watchdog)
              watchdog_instructions = 0U;
            break;
          }

          if (reference->name == "equals" &&
              reference->descriptor == "(Ljava/lang/Object;)Z" &&
              arguments->size() == 2U &&
              arguments->kind(1U) == ValueKind::reference)
          {
            const ObjectRef receiver = arguments->reference_unchecked(0U);
            const ObjectRef other = arguments->reference_unchecked(1U);
            auto equal = heap_.vm_string_equals(receiver, other);
            if (!equal)
              return std::unexpected(equal.error());
            auto pushed = frame.push_int(*equal ? 1 : 0);
            if (!pushed)
              return std::unexpected(pushed.error());
            if (budget_mode == InstructionBudgetMode::progress_watchdog)
              watchdog_instructions = 0U;
            break;
          }
        }

        // System.arraycopy is a very common inner-loop primitive in legacy
        // Java ME codecs and decompressors.  Going through normal invokestatic
        // resolution, Invocation construction and the native registry for a
        // two- or three-element byte[]/int[] copy costs far more than the copy
        // itself, especially in the WebAssembly interpreter where there is no
        // native ARM JIT.  Handle primitive arrays directly while the VM
        // execution gate is already held.  Reference arrays deliberately fall
        // through to the normal native implementation so Java assignability
        // and partial-copy semantics remain centralized there.
        const bool is_system_arraycopy =
            is_static &&
            reference->owner == "java/lang/System" &&
            reference->name == "arraycopy" &&
            reference->descriptor ==
                "(Ljava/lang/Object;ILjava/lang/Object;II)V";
        if (is_system_arraycopy && arguments->size() == 5U)
        {
          auto source = (*arguments)[0U].as_reference();
          auto source_position = (*arguments)[1U].as_int();
          auto destination = (*arguments)[2U].as_reference();
          auto destination_position = (*arguments)[3U].as_int();
          auto length = (*arguments)[4U].as_int();
          if (!source || !source_position || !destination ||
              !destination_position || !length)
          {
            return fail(ErrorCode::malformed_class,
                        "System.arraycopy arguments have invalid kinds");
          }
          if (source->is_null() || destination->is_null())
          {
            auto raised = raise_implicit(
                "java/lang/NullPointerException", opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }

          // Keep the uncommon negative-index case on the native path so its
          // historical exception ordering for non-array operands is unchanged.
          if (*source_position >= 0 && *destination_position >= 0 &&
              *length >= 0)
          {
            auto copied = heap_.vm_try_copy_primitive_array_range(
                *source,
                static_cast<usize>(*source_position),
                *destination,
                static_cast<usize>(*destination_position),
                static_cast<usize>(*length));
            if (!copied)
            {
              std::string_view exception_class;
              if (copied.error().code == ErrorCode::out_of_range)
              {
                exception_class =
                    "java/lang/ArrayIndexOutOfBoundsException";
              }
              else if (copied.error().code == ErrorCode::invalid_state ||
                       copied.error().code == ErrorCode::invalid_argument)
              {
                exception_class = "java/lang/ArrayStoreException";
              }
              else
              {
                return std::unexpected(copied.error());
              }
              auto raised = raise_implicit(exception_class, opcode_pc);
              if (!raised)
                return std::unexpected(raised.error());
              if (raised->has_value())
                return std::move(**raised);
              break;
            }
            if (*copied)
            {
              break;
            }
          }
        }

        std::optional<Invocation> nested;
        const bool is_clone_intrinsic =
            !is_static &&
            (opcode == 0xB6 || opcode == 0xB7) &&
            reference->name == "clone" &&
            reference->descriptor == "()Ljava/lang/Object;" &&
            (reference->owner == "java/lang/Object" ||
             reference->owner.starts_with('['));
        if (is_clone_intrinsic)
        {
          auto source = arguments->front().as_reference();
          if (!source)
            return std::unexpected(source.error());
          auto source_class = heap_.vm_class_name_view(*source);
          if (!source_class)
            return std::unexpected(source_class.error());
          if (!source_class->starts_with('['))
          {
            auto cloneable = classes_.is_assignable(
                *source_class, "java/lang/Cloneable");
            if (!cloneable)
              return std::unexpected(cloneable.error());
            if (!*cloneable)
            {
              auto raised = raise_implicit(
                  "java/lang/CloneNotSupportedException", opcode_pc);
              if (!raised)
                return std::unexpected(raised.error());
              if (raised->has_value())
                return std::move(**raised);
              break;
            }
          }
          auto clone = heap_.clone_object(*source);
          if (!clone && clone.error().code == ErrorCode::overflow)
          {
            auto collected = collect_active_garbage(*source);
            if (!collected)
              return std::unexpected(collected.error());
            clone = heap_.clone_object(*source);
          }
          if (!clone)
          {
            if (clone.error().code == ErrorCode::overflow)
            {
              auto raised = raise_implicit("java/lang/OutOfMemoryError",
                                           opcode_pc);
              if (!raised)
                return std::unexpected(raised.error());
              if (raised->has_value())
                return std::move(**raised);
              break;
            }
            return std::unexpected(clone.error());
          }
          auto pushed = frame.push_reference(*clone);
          if (!pushed)
            return std::unexpected(pushed.error());
          break;
        }
        const bool is_reflective_constructor =
            !is_static && opcode == 0xB6 &&
            reference->owner == "java/lang/Class" &&
            reference->name == "newInstance" &&
            reference->descriptor == "()Ljava/lang/Object;";
        if (is_reflective_constructor)
        {
          auto mirror = arguments->front().as_reference();
          if (!mirror)
            return std::unexpected(mirror.error());
          auto class_name = mirrored_class_name(*mirror);
          if (!class_name)
            return std::unexpected(class_name.error());
          const bool primitive = class_name->size() == 1U &&
              std::string_view("ZBCSIJFDV").find(class_name->front()) !=
                  std::string_view::npos;
          if (primitive || class_name->starts_with('['))
          {
            auto raised = raise_implicit("java/lang/InstantiationException",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          auto loaded = classes_.load(*class_name);
          if (!loaded)
          {
            if (loaded.error().code == ErrorCode::class_not_found)
            {
              auto raised = raise_implicit(
                  "java/lang/InstantiationException", opcode_pc);
              if (!raised)
                return std::unexpected(raised.error());
              if (raised->has_value())
                return std::move(**raised);
              break;
            }
            return std::unexpected(loaded.error());
          }
          if (((*loaded)->access_flags() &
               (kAccInterface | kAccAbstract)) != 0U)
          {
            auto raised = raise_implicit("java/lang/InstantiationException",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          auto initialized = ensure_initialized_from_compact_execution(
              *class_name,
              remaining_execution_budget(),
              *arguments);
          if (!initialized)
            return std::unexpected(initialized.error());
          if (initialized->has_value())
          {
            auto dispatched = dispatch_exception(**initialized, opcode_pc);
            if (!dispatched)
              return std::unexpected(dispatched.error());
            if (dispatched->has_value())
              return std::move(**dispatched);
            break;
          }
          auto constructor = classes_.resolve_declared_method(
              *class_name, "<init>", "()V");
          if (!constructor)
          {
            if (constructor.error().code == ErrorCode::method_not_found)
            {
              auto raised = raise_implicit(
                  "java/lang/InstantiationException", opcode_pc);
              if (!raised)
                return std::unexpected(raised.error());
              if (raised->has_value())
                return std::move(**raised);
              break;
            }
            return std::unexpected(constructor.error());
          }
          const auto package_name = [](std::string_view name) {
            const usize separator = name.rfind('/');
            return separator == std::string_view::npos
                ? std::string_view {}
                : name.substr(0U, separator);
          };
          const u16 constructor_flags = constructor->method->access_flags;
          const bool legacy_same_package_access =
              (*loaded)->major_version() <= 50U &&
              (constructor_flags & kAccPrivate) == 0U &&
              package_name(frame.owner().name()) == package_name(*class_name);
          if ((constructor_flags & kAccPublic) == 0U &&
              !legacy_same_package_access)
          {
            auto raised = raise_implicit("java/lang/IllegalAccessException",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          auto object = allocate_instance_with_gc(*class_name);
          if (!object)
          {
            if (object.error().code == ErrorCode::overflow)
            {
              auto raised = raise_implicit("java/lang/OutOfMemoryError",
                                           opcode_pc);
              if (!raised)
                return std::unexpected(raised.error());
              if (raised->has_value())
                return std::move(**raised);
              break;
            }
            return std::unexpected(object.error());
          }
          std::vector<Value> constructor_arguments {
              Value::from_reference(*object),
          };
          auto reflective_invocation = prepare_invocation(
              std::move(*constructor), constructor_arguments, true);
          if (!reflective_invocation)
            return std::unexpected(reflective_invocation.error());
          reflective_invocation->return_override =
              Value::from_reference(*object);
          nested = std::move(*reflective_invocation);
        }
        if (!nested.has_value() && !is_static && opcode == 0xB9)
        {
          auto receiver = arguments->front().as_reference();
          if (!receiver)
            return std::unexpected(receiver.error());
          const auto lambda = lambda_bindings_.find(receiver->bits);
          const bool lambda_descriptor_matches =
              lambda != lambda_bindings_.end() &&
              (lambda->second.sam_descriptor == reference->descriptor ||
               std::find(lambda->second.bridge_descriptors.begin(),
                         lambda->second.bridge_descriptors.end(),
                         reference->descriptor) !=
                   lambda->second.bridge_descriptors.end());
          if (lambda != lambda_bindings_.end() &&
              lambda->second.interface_name == reference->owner &&
              lambda->second.sam_name == reference->name &&
              lambda_descriptor_matches)
          {
            std::optional<ObjectRef> constructor_receiver;
            if (lambda->second.implementation_kind == 8U)
            {
              auto allocated = allocate_instance_with_gc(
                  lambda->second.implementation.owner);
              if (!allocated)
              {
                if (allocated.error().code == ErrorCode::overflow)
                {
                  auto raised = raise_implicit("java/lang/OutOfMemoryError",
                                               opcode_pc);
                  if (!raised)
                    return std::unexpected(raised.error());
                  if (raised->has_value())
                    return std::move(**raised);
                  break;
                }
                return std::unexpected(allocated.error());
              }
              constructor_receiver = *allocated;
            }
            const usize lambda_argument_count = arguments->size() - 1U;
            std::array<Value, kInlineInvocationArgumentCapacity>
                inline_lambda_arguments;
            std::vector<Value> overflow_lambda_arguments;
            std::span<Value> lambda_arguments;
            if (lambda_argument_count <= inline_lambda_arguments.size())
            {
              lambda_arguments = std::span<Value>(
                  inline_lambda_arguments.data(), lambda_argument_count);
            }
            else
            {
              overflow_lambda_arguments.resize(lambda_argument_count);
              lambda_arguments = overflow_lambda_arguments;
            }
            arguments->materialize_from(1U, lambda_arguments);
            auto lambda_invocation = prepare_lambda_invocation(
                *receiver,
                lambda->second,
                std::span<const Value>(lambda_arguments),
                constructor_receiver);
            if (!lambda_invocation)
            {
              if (lambda_invocation.error().code == ErrorCode::java_exception &&
                  !lambda_invocation.error().java_exception_class.empty())
              {
                auto raised = raise_implicit(
                    lambda_invocation.error().java_exception_class,
                    opcode_pc,
                    lambda_invocation.error().message);
                if (!raised)
                  return std::unexpected(raised.error());
                if (raised->has_value())
                  return std::move(**raised);
                break;
              }
              return std::unexpected(lambda_invocation.error());
            }
            nested = std::move(*lambda_invocation);
          }
        }

        if (!nested.has_value())
        {
          std::string_view dispatch_class = reference->owner;
          std::shared_ptr<const RuntimeClass> dispatch_metadata;
          if (!is_static && opcode != 0xB7)
          {
            auto receiver = arguments->front().as_reference();
            if (!receiver)
              return std::unexpected(receiver.error());
            auto runtime_class = heap_.vm_class_name_view(*receiver);
            if (!runtime_class)
              return std::unexpected(runtime_class.error());
            if (opcode == 0xB9)
            {
              auto implements_interface = classes_.is_assignable(
                  *runtime_class,
                  reference->owner);
              if (!implements_interface)
              {
                return std::unexpected(implements_interface.error());
              }
              if (!*implements_interface)
              {
                auto raised = raise_implicit(
                    "java/lang/IncompatibleClassChangeError",
                    opcode_pc);
                if (!raised)
                  return std::unexpected(raised.error());
                if (raised->has_value())
                  return std::move(**raised);
                break;
              }
            }
            dispatch_class = *runtime_class;
            dispatch_metadata = cached_runtime_class(dispatch_class);
            if (dispatch_metadata == nullptr)
            {
              auto loaded_dispatch_class = classes_.load(dispatch_class);
              if (!loaded_dispatch_class)
                return std::unexpected(loaded_dispatch_class.error());
              dispatch_metadata = cached_runtime_class(dispatch_class);
            }
          }
          OperandResolutionEntry* direct_operand_entry = nullptr;
          OperandResolutionEntry* virtual_operand_entry = nullptr;
          DirectCallCache* legacy_direct_cache = nullptr;
          VirtualCallCache* virtual_cache = nullptr;
          std::optional<ResolvedMethod> inline_target;
          std::optional<NativeMethodId> prebound_native_method;
          const bool cacheable_direct_call = is_static || opcode == 0xB7;
          if (cacheable_direct_call)
          {
            const MethodId caller_method_id = frame.runtime_method_id();
            const u32 caller_operand_index =
                frame.current_decoded_operand_index();
            if (caller_method_id.valid() &&
                caller_operand_index != kInvalidDecodedIndex)
            {
              auto cached_entry = frame.operand_resolution_entry(
                  caller_operand_index, opcode_pc);
              if (!cached_entry)
                return std::unexpected(cached_entry.error());
              OperandResolutionEntry& entry = **cached_entry;
              direct_operand_entry = &entry;
              if (entry.state == OperandResolutionState::resolved)
              {
                if (entry.kind != OperandResolutionKind::direct_call ||
                    !entry.target_method.valid())
                {
                  return fail(ErrorCode::internal_error,
                              "decoded direct call cache has wrong kind");
                }
                auto runtime_target = cached_runtime_method(
                    entry.target_method);
                if (runtime_target == nullptr)
                {
                  entry.reset();
                  PerformanceCounters::record_direct_call_cache(false);
                  PerformanceCounters::record_operand_resolution(false);
                  auto began = entry.begin(
                      OperandResolutionKind::direct_call);
                  if (!began)
                    return std::unexpected(began.error());
                }
                else
                {
                  const u64 current_native_generation = natives_.generation();
                  if (!entry.native_binding_cached ||
                      entry.native_generation != current_native_generation)
                  {
                    const NativeMethodBinding native_binding =
                        resolve_native_binding(*runtime_target->owner,
                                               *runtime_target->method,
                                               runtime_target->id);
                    auto updated = entry.update_native_binding(
                        native_binding.id,
                        native_binding.generation);
                    if (!updated)
                      return std::unexpected(updated.error());
                  }
                  prebound_native_method = entry.target_native_method;
                  inline_target = ResolvedMethod {
                      .owner = runtime_target->owner,
                      .method = runtime_target->method,
                      .runtime = std::move(runtime_target),
                  };
                  PerformanceCounters::record_direct_call_cache(true);
                  PerformanceCounters::record_operand_resolution(true);
                }
              }
              else if (entry.state == OperandResolutionState::failed)
              {
                if (entry.kind != OperandResolutionKind::direct_call ||
                    !entry.failure.has_value())
                {
                  return fail(ErrorCode::internal_error,
                              "decoded failed direct call has no error");
                }
                PerformanceCounters::record_direct_call_cache(true);
                PerformanceCounters::record_operand_resolution(true);
                PerformanceCounters::record_operand_resolution_failure();
                auto raised = raise_resolution_error(*entry.failure, opcode_pc);
                if (!raised)
                  return std::unexpected(raised.error());
                if (raised->has_value())
                  return std::move(**raised);
                break;
              }
              else if (entry.state == OperandResolutionState::resolving)
              {
                return fail(ErrorCode::invalid_state,
                            "recursive decoded direct call resolution");
              }
              else
              {
                PerformanceCounters::record_direct_call_cache(false);
                PerformanceCounters::record_operand_resolution(false);
                auto began = entry.begin(
                    OperandResolutionKind::direct_call);
                if (!began)
                  return std::unexpected(began.error());
              }
            }
            else
            {
              auto cache = direct_call_binding_slot(frame.owner(), *index);
              if (!cache)
                return std::unexpected(cache.error());
              legacy_direct_cache = *cache;
              if ((*cache)->valid)
              {
                auto runtime_target = cached_runtime_method(
                    (*cache)->target_method);
                if (runtime_target != nullptr)
                {
                  const u64 current_native_generation = natives_.generation();
                  if (!(*cache)->native_binding_cached ||
                      (*cache)->native_generation != current_native_generation)
                  {
                    const NativeMethodBinding native_binding =
                        resolve_native_binding(*runtime_target->owner,
                                               *runtime_target->method,
                                               runtime_target->id);
                    (*cache)->native_method = native_binding.id;
                    (*cache)->native_generation = native_binding.generation;
                    (*cache)->native_binding_cached = true;
                  }
                  prebound_native_method = (*cache)->native_method;
                  inline_target = ResolvedMethod {
                      .owner = runtime_target->owner,
                      .method = runtime_target->method,
                      .runtime = std::move(runtime_target),
                  };
                  PerformanceCounters::record_direct_call_cache(true);
                }
                else
                {
                  (*cache)->valid = false;
                  PerformanceCounters::record_direct_call_cache(false);
                }
              }
              else
              {
                PerformanceCounters::record_direct_call_cache(false);
              }
            }
          }

          const bool cacheable_virtual_call =
              !is_static && opcode != 0xB7 && dispatch_metadata != nullptr;
          if (cacheable_virtual_call)
          {
            const MethodId caller_method_id = frame.runtime_method_id();
            const u32 caller_operand_index =
                frame.current_decoded_operand_index();
            if (caller_method_id.valid() &&
                caller_operand_index != kInvalidDecodedIndex)
            {
              auto cached_entry = frame.operand_resolution_entry(
                  caller_operand_index, opcode_pc);
              if (!cached_entry)
                return std::unexpected(cached_entry.error());
              OperandResolutionEntry& entry = **cached_entry;
              virtual_operand_entry = &entry;
              const auto begin_for_receiver = [&]() -> Status
              {
                entry.reset();
                PerformanceCounters::record_virtual_inline_cache(false);
                PerformanceCounters::record_operand_resolution(false);
                return entry.begin_virtual_call(dispatch_metadata->id);
              };
              if (entry.state == OperandResolutionState::resolved)
              {
                if (entry.kind != OperandResolutionKind::virtual_call ||
                    !entry.receiver_class.valid() ||
                    !entry.target_method.valid())
                {
                  return fail(ErrorCode::internal_error,
                              "decoded virtual call cache has wrong kind");
                }
                if (entry.receiver_class != dispatch_metadata->id)
                {
                  auto began = begin_for_receiver();
                  if (!began)
                    return std::unexpected(began.error());
                }
                else
                {
                  auto runtime_target = cached_runtime_method(
                      entry.target_method);
                  if (runtime_target == nullptr)
                  {
                    auto began = begin_for_receiver();
                    if (!began)
                      return std::unexpected(began.error());
                  }
                  else
                  {
                    const u64 current_native_generation = natives_.generation();
                    if (!entry.native_binding_cached ||
                        entry.native_generation != current_native_generation)
                    {
                      const NativeMethodBinding native_binding =
                          resolve_native_binding(*runtime_target->owner,
                                                 *runtime_target->method,
                                                 runtime_target->id);
                      auto updated = entry.update_native_binding(
                          native_binding.id,
                          native_binding.generation);
                      if (!updated)
                        return std::unexpected(updated.error());
                    }
                    prebound_native_method = entry.target_native_method;
                    inline_target = ResolvedMethod {
                        .owner = runtime_target->owner,
                        .method = runtime_target->method,
                        .runtime = std::move(runtime_target),
                    };
                    PerformanceCounters::record_virtual_inline_cache(true);
                    PerformanceCounters::record_operand_resolution(true);
                  }
                }
              }
              else if (entry.state == OperandResolutionState::failed)
              {
                if (entry.kind != OperandResolutionKind::virtual_call ||
                    !entry.receiver_class.valid() ||
                    !entry.failure.has_value())
                {
                  return fail(ErrorCode::internal_error,
                              "decoded failed virtual call has no error");
                }
                if (entry.receiver_class != dispatch_metadata->id)
                {
                  auto began = begin_for_receiver();
                  if (!began)
                    return std::unexpected(began.error());
                }
                else
                {
                  PerformanceCounters::record_virtual_inline_cache(true);
                  PerformanceCounters::record_operand_resolution(true);
                  PerformanceCounters::record_operand_resolution_failure();
                  auto raised = raise_resolution_error(
                      *entry.failure, opcode_pc);
                  if (!raised)
                    return std::unexpected(raised.error());
                  if (raised->has_value())
                    return std::move(**raised);
                  break;
                }
              }
              else if (entry.state == OperandResolutionState::resolving)
              {
                return fail(ErrorCode::invalid_state,
                            "recursive decoded virtual call resolution");
              }
              else
              {
                PerformanceCounters::record_virtual_inline_cache(false);
                PerformanceCounters::record_operand_resolution(false);
                auto began = entry.begin_virtual_call(dispatch_metadata->id);
                if (!began)
                  return std::unexpected(began.error());
              }
            }
            else
            {
              auto cache = virtual_call_binding_slot(frame.owner(), *index);
              if (!cache)
                return std::unexpected(cache.error());
              virtual_cache = *cache;
              auto* cached_entry = (*cache)->lookup_entry(dispatch_metadata->id);
              if (cached_entry != nullptr)
              {
                auto runtime_target = cached_runtime_method(
                    cached_entry->target_method);
                if (runtime_target != nullptr)
                {
                  const u64 current_native_generation = natives_.generation();
                  if (!cached_entry->native_binding_cached ||
                      cached_entry->native_generation != current_native_generation)
                  {
                    const NativeMethodBinding native_binding =
                        resolve_native_binding(*runtime_target->owner,
                                               *runtime_target->method,
                                               runtime_target->id);
                    cached_entry->native_method = native_binding.id;
                    cached_entry->native_generation = native_binding.generation;
                    cached_entry->native_binding_cached = true;
                  }
                  prebound_native_method = cached_entry->native_method;
                  inline_target = ResolvedMethod {
                      .owner = runtime_target->owner,
                      .method = runtime_target->method,
                      .runtime = std::move(runtime_target),
                  };
                  PerformanceCounters::record_virtual_inline_cache(true);
                }
                else
                {
                  (*cache)->invalidate(dispatch_metadata->id);
                  PerformanceCounters::record_virtual_inline_cache(false);
                }
              }
              else
              {
                PerformanceCounters::record_virtual_inline_cache(false);
              }
            }
          }

          const auto resolve_target = [&]() -> Result<ResolvedMethod>
          {
            if (inline_target.has_value())
              return *inline_target;
            if (opcode == 0xB7 && reference->name == "<init>")
            {
              return classes_.resolve_declared_method(
                  reference->owner,
                  reference->name,
                  reference->descriptor);
            }
            return classes_.resolve_method(dispatch_class,
                                           reference->name,
                                           reference->descriptor);
          };
          auto target = resolve_target();
          if (!target)
          {
            const OperandResolutionKind resolution_kind =
                virtual_operand_entry != nullptr
                    ? OperandResolutionKind::virtual_call
                    : OperandResolutionKind::direct_call;
            Error linkage_error = stable_linkage_error(
                target.error(), resolution_kind);
            OperandResolutionEntry* failed_entry =
                direct_operand_entry != nullptr
                    ? direct_operand_entry
                    : virtual_operand_entry;
            if (failed_entry != nullptr &&
                failed_entry->state == OperandResolutionState::resolving)
            {
              auto cached = failed_entry->fail_resolution(linkage_error);
              if (!cached)
                return std::unexpected(cached.error());
              PerformanceCounters::record_operand_resolution_failure();
              linkage_error = *failed_entry->failure;
            }
            auto raised = raise_resolution_error(linkage_error, opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          if (direct_operand_entry != nullptr &&
              !inline_target.has_value())
          {
            if (target->runtime == nullptr)
            {
              direct_operand_entry->reset();
            }
            else
            {
              const NativeMethodBinding native_binding =
                  resolve_native_binding(*target->owner,
                                         *target->method,
                                         target->runtime->id);
              auto completed = direct_operand_entry->resolve_direct_call(
                  target->runtime->id,
                  native_binding.id,
                  native_binding.generation);
              if (!completed)
                return std::unexpected(completed.error());
              prebound_native_method = native_binding.id;
            }
          }
          if (virtual_operand_entry != nullptr &&
              !inline_target.has_value())
          {
            if (target->runtime == nullptr)
            {
              virtual_operand_entry->reset();
            }
            else
            {
              const NativeMethodBinding native_binding =
                  resolve_native_binding(*target->owner,
                                         *target->method,
                                         target->runtime->id);
              auto completed = virtual_operand_entry->resolve_virtual_call(
                  target->runtime->id,
                  native_binding.id,
                  native_binding.generation);
              if (!completed)
                return std::unexpected(completed.error());
              prebound_native_method = native_binding.id;
            }
          }
          if (legacy_direct_cache != nullptr &&
              !inline_target.has_value() && target->runtime != nullptr)
          {
            const NativeMethodBinding native_binding =
                resolve_native_binding(*target->owner,
                                       *target->method,
                                       target->runtime->id);
            *legacy_direct_cache = DirectCallCache {
                .target_method = target->runtime->id,
                .native_method = native_binding.id,
                .native_generation = native_binding.generation,
                .native_binding_cached = true,
                .valid = true,
            };
            prebound_native_method = native_binding.id;
          }
          if (virtual_cache != nullptr && !inline_target.has_value() &&
              target->runtime != nullptr)
          {
            const NativeMethodBinding native_binding =
                resolve_native_binding(*target->owner,
                                       *target->method,
                                       target->runtime->id);
            virtual_cache->update(dispatch_metadata->id,
                                  target->runtime->id,
                                  native_binding.id,
                                  native_binding.generation,
                                  true);
            prebound_native_method = native_binding.id;
          }
          if (((target->method->access_flags & kAccStatic) != 0) != is_static)
          {
            auto raised = raise_implicit(
                "java/lang/IncompatibleClassChangeError", opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          if ((target->method->access_flags & kAccAbstract) != 0U)
          {
            auto raised = raise_implicit("java/lang/AbstractMethodError",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          auto prepared = prepare_invocation(
              std::move(*target),
              std::move(*arguments),
              !is_static,
              prebound_native_method,
              frame.has_verified_types());
          if (!prepared)
            return std::unexpected(prepared.error());
          nested = std::move(*prepared);
        }
        const bool nested_is_native = nested->native_method.valid() ||
                                      !nested->method.method->code.has_value();
        if (!nested_is_native && frames.size() >= kMaximumCallDepth)
        {
          auto raised = raise_implicit("java/lang/StackOverflowError",
                                       opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        const bool nested_synchronized =
            (nested->method.method->access_flags & kAccSynchronized) != 0U;
        auto nested_monitor = synchronized_monitor(*nested);
        if (!nested_monitor)
        {
          return std::unexpected(nested_monitor.error());
        }
        if (nested_monitor->has_value())
        {
          auto entered = monitors_.enter(**nested_monitor,
                                         scheduler_.current_thread_id());
          if (!entered)
            return std::unexpected(entered.error());
          if (*entered == MonitorEnterResult::would_block)
          {
            // Uncontended synchronized calls are not safepoints: the VM gate
            // never leaves this host thread, so publishing/scanning every
            // active frame is pure cache/memory traffic. Publish only when the
            // monitor is truly contended and enter_monitor is about to release
            // the execution gate while blocking.
            publish_compact_execution_roots(nested->arguments,
                                            nested->return_override);
            auto blocked = enter_monitor(**nested_monitor);
            if (!blocked)
              return std::unexpected(blocked.error());
          }
        }
        if (!nested_is_native)
        {
          if (nested->has_receiver && nested->arguments.size() == 3U)
          {
            auto x = nested->arguments[1U].as_int();
            auto y = nested->arguments[2U].as_int();
            if (x && y)
            {
              auto collision = try_tiled_alpha_collision_intrinsic(
                  nested->method, *x, *y);
              if (!collision)
              {
                auto released = release_synchronized_monitor(*nested_monitor);
                if (!released)
                  return std::unexpected(released.error());
                return std::unexpected(collision.error());
              }
              if (collision->has_value())
              {
                auto released = release_synchronized_monitor(*nested_monitor);
                if (!released)
                  return std::unexpected(released.error());
                if (budget_mode == InstructionBudgetMode::progress_watchdog)
                  watchdog_instructions = 0U;
                auto pushed = frame.push(**collision);
                if (!pushed)
                  return std::unexpected(pushed.error());
                break;
              }
            }
          }

          if (!nested->has_receiver && nested->arguments.size() == 3U)
          {
            auto x = nested->arguments[0U].as_int();
            auto y = nested->arguments[1U].as_int();
            auto shooter = nested->arguments[2U].as_reference();
            if (x && y && shooter)
            {
              auto projectile = try_projectile_collision_intrinsic(
                  nested->method, *x, *y, *shooter);
              if (!projectile)
              {
                auto released = release_synchronized_monitor(*nested_monitor);
                if (!released)
                  return std::unexpected(released.error());
                return std::unexpected(projectile.error());
              }
              if (projectile->has_value())
              {
                auto released = release_synchronized_monitor(*nested_monitor);
                if (!released)
                  return std::unexpected(released.error());
                if (budget_mode == InstructionBudgetMode::progress_watchdog)
                  watchdog_instructions = 0U;
                auto pushed = frame.push(**projectile);
                if (!pushed)
                  return std::unexpected(pushed.error());
                break;
              }
            }
          }

          auto trivial_getter = try_trivial_getter_intrinsic(*nested);
          if (!trivial_getter)
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            return std::unexpected(trivial_getter.error());
          }
          if (trivial_getter->has_value())
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            if (budget_mode == InstructionBudgetMode::progress_watchdog)
              watchdog_instructions = 0U;
            auto pushed = frame.push(**trivial_getter);
            if (!pushed)
              return std::unexpected(pushed.error());
            break;
          }

          auto byte_cursor = try_static_byte_cursor_read_intrinsic(*nested);
          if (!byte_cursor)
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            return std::unexpected(byte_cursor.error());
          }
          if (byte_cursor->has_value())
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            if (budget_mode == InstructionBudgetMode::progress_watchdog)
              watchdog_instructions = 0U;
            auto pushed = frame.push(**byte_cursor);
            if (!pushed)
              return std::unexpected(pushed.error());
            break;
          }

          auto png_decoded = try_indexed_png_decoder_intrinsic(*nested);
          if (!png_decoded)
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            return std::unexpected(png_decoded.error());
          }
          if (png_decoded->has_value())
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            if (budget_mode == InstructionBudgetMode::progress_watchdog)
              watchdog_instructions = 0U;
            auto pushed = frame.push(**png_decoded);
            if (!pushed)
              return std::unexpected(pushed.error());
            break;
          }

          auto range_decoded = try_range_decoder_bit_intrinsic(*nested);
          if (!range_decoded)
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            return std::unexpected(range_decoded.error());
          }
          if (range_decoded->has_value())
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            if (budget_mode == InstructionBudgetMode::progress_watchdog)
              watchdog_instructions = 0U;
            auto pushed = frame.push(**range_decoded);
            if (!pushed)
              return std::unexpected(pushed.error());
            break;
          }
          auto sorted = try_vector_key_sort_intrinsic(*nested);
          if (!sorted)
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            return std::unexpected(sorted.error());
          }
          if (*sorted)
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            if (budget_mode == InstructionBudgetMode::progress_watchdog)
              watchdog_instructions = 0U;
            break;
          }
          auto intrinsic = try_long_bit_permutation_intrinsic(*nested);
          if (!intrinsic)
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            return std::unexpected(intrinsic.error());
          }
          if (intrinsic->has_value())
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            if (budget_mode == InstructionBudgetMode::progress_watchdog)
              watchdog_instructions = 0U;
            auto pushed = frame.push(**intrinsic);
            if (!pushed)
              return std::unexpected(pushed.error());
            break;
          }

#if !defined(__EMSCRIPTEN__)
          // A progress-watchdog invocation must still be allowed to execute
          // hot nested Java methods through the JIT.  The compiled entry gets
          // the watchdog's remaining instruction budget below and reports its
          // exact bytecode count back into `executed`; on successful return we
          // reset `watchdog_instructions` just like the interpreter does at a
          // completed nested call.  Disabling nested JIT here forces an entire
          // Canvas.paint render tree back through the interpreter and is
          // catastrophic for game frame rate.
          if (const auto nested_jit_budget = jit_.enabled()
                  ? safe_jit_instruction_budget(
                        nested->method, remaining_execution_budget())
                  : std::nullopt;
              nested_jit_budget.has_value() &&
              nested->method.runtime != nullptr &&
              nested->method.owner != nullptr &&
              nested->descriptor != nullptr &&
              !nested->return_override.has_value() &&
              !nested->discard_return_value &&
              !nested->return_reference_cast_target.has_value() &&
              !nested->return_unboxing_target.has_value() &&
              !nested->return_widening_target.has_value())
          {
            JitExecutionContext jit_context{
                .machine = this,
                .owner = nested->method.owner.get(),
                .method = nested->method.method,
                .invocation_depth = invocation_depth,
                .base_roots = {},
                .outer_roots_context = &frames,
                .append_outer_roots = &append_execution_frame_roots,
                .compact_extra_root_values = &nested->arguments,
                .progress_watchdog =
                    budget_mode == InstructionBudgetMode::progress_watchdog,
                .progress_total_budget =
                    budget_mode == InstructionBudgetMode::progress_watchdog &&
                            executed < progress_total_budget
                        ? progress_total_budget - executed
                        : 0U,
            };
            if (nested->method.method->code.has_value())
            {
              jit_context.published_roots.reserve(
                  frames.size() * 8U + nested->arguments.size() +
                  nested->method.method->code->max_locals +
                  nested->method.method->code->max_stack);
            }
            const JitRuntimeHooks jit_hooks{
                .context = &jit_context,
                .dispatch = &Machine::jit_runtime_dispatch_callback,
                .publish_roots = &Machine::jit_publish_roots_callback,
            };
            auto jitted = jit_.try_execute(
                nested->method.runtime->id,
                *nested->method.owner,
                *nested->method.method,
                *nested->descriptor,
                nested->arguments,
                nested->has_receiver,
                *nested_jit_budget,
                jit_hooks,
                nested->method.owner,
                nested->method.runtime->verified_frames,
                nested->method.runtime->descriptor);
            if (!jitted)
            {
              auto released = release_synchronized_monitor(*nested_monitor);
              if (!released)
                return std::unexpected(released.error());
              if (jit_instruction_budget_exhausted(jitted.error()))
              {
                PerformanceCounters::record_instruction_budget_exit();
                return std::unexpected(vm_instruction_budget_error(
                    nested->method.owner->name(),
                    nested->method.method->name,
                    nested->method.method->descriptor));
              }
              return std::unexpected(jitted.error());
            }
            if (jitted->has_value())
            {
              const u64 jit_instructions =
                  static_cast<u64>((*jitted)->bytecode_instructions) +
                  jit_context.progress_reset_instructions;
              if (jit_instructions >
                  static_cast<u64>(std::numeric_limits<u32>::max()))
              {
                auto released = release_synchronized_monitor(*nested_monitor);
                if (!released)
                  return std::unexpected(released.error());
                return std::unexpected(vm_instruction_budget_error(
                    nested->method.owner->name(),
                    nested->method.method->name,
                    nested->method.method->descriptor));
              }
              executed += jit_instructions;
              separately_accounted_nested_instructions +=
                  jit_context.nested_instructions;
              accounted_instructions =
                  executed >= separately_accounted_nested_instructions
                      ? executed - separately_accounted_nested_instructions
                      : 0U;
              if ((*jitted)->deopt_state.has_value())
              {
                auto resumed = ExecutionFrame::emplace(
                    frames,
                    std::move(nested->method),
                    nested->descriptor,
                    nested->arguments,
                    nested->has_receiver);
                if (!resumed)
                {
                  auto released = release_synchronized_monitor(*nested_monitor);
                  if (!released)
                    return std::unexpected(released.error());
                  return std::unexpected(resumed.error());
                }
                auto restored = (*resumed)->restore_jit_deopt_state(
                    *(*jitted)->deopt_state);
                if (!restored)
                {
                  frames.pop_back();
                  auto released = release_synchronized_monitor(*nested_monitor);
                  if (!released)
                    return std::unexpected(released.error());
                  return std::unexpected(restored.error());
                }
                if (nested_monitor->has_value())
                  (*resumed)->set_synchronized_monitor(**nested_monitor);
                if (nested->return_override.has_value())
                  (*resumed)->set_return_override(*nested->return_override);
                PerformanceCounters::observe_java_call_depth(frames.size());
                if (budget_mode == InstructionBudgetMode::progress_watchdog)
                  watchdog_instructions = 0U;
                break;
              }
              auto released = release_synchronized_monitor(*nested_monitor);
              if (!released)
                return std::unexpected(released.error());
              if ((*jitted)->exception.has_value())
              {
                if (*(*jitted)->exception ==
                    JitExceptionKind::runtime_throwable)
                {
                  if (!jit_context.pending_throwable.has_value())
                  {
                    return fail(ErrorCode::internal_error,
                                "nested JIT runtime exception has no throwable");
                  }
                  auto dispatched = dispatch_exception(
                      *jit_context.pending_throwable, opcode_pc);
                  if (!dispatched)
                    return std::unexpected(dispatched.error());
                  if (dispatched->has_value())
                    return std::move(**dispatched);
                }
                else
                {
                  auto raised = raise_implicit(
                      jit_exception_class(*(*jitted)->exception),
                      opcode_pc);
                  if (!raised)
                    return std::unexpected(raised.error());
                  if (raised->has_value())
                    return std::move(**raised);
                }
                break;
              }
              if (budget_mode == InstructionBudgetMode::progress_watchdog)
                watchdog_instructions = 0U;
              if ((*jitted)->return_value.has_value())
              {
                auto pushed = frame.push(*(*jitted)->return_value);
                if (!pushed)
                  return std::unexpected(pushed.error());
              }
              break;
            }
          }
#endif
        }
        if (nested_is_native && !nested_synchronized &&
            nested->method.owner != nullptr &&
            nested->method.method != nullptr &&
            nested->method.owner->name() == "java/io/DataInputStream" &&
            nested->arguments.size() == 1U)
        {
          const std::string& fast_name = nested->method.method->name;
          const std::string& fast_descriptor =
              nested->method.method->descriptor;
          usize fast_width = 0U;
          enum class FastDataInputKind : u8
          {
            none,
            boolean_value,
            byte_value,
            unsigned_byte,
            short_value,
            unsigned_short,
            char_value,
            int_value,
            long_value,
            float_value,
            double_value,
          };
          FastDataInputKind fast_kind = FastDataInputKind::none;
          if (fast_name == "readBoolean" && fast_descriptor == "()Z")
          {
            fast_width = 1U;
            fast_kind = FastDataInputKind::boolean_value;
          }
          else if (fast_name == "readByte" && fast_descriptor == "()B")
          {
            fast_width = 1U;
            fast_kind = FastDataInputKind::byte_value;
          }
          else if (fast_name == "readUnsignedByte" &&
                   fast_descriptor == "()I")
          {
            fast_width = 1U;
            fast_kind = FastDataInputKind::unsigned_byte;
          }
          else if (fast_name == "readShort" && fast_descriptor == "()S")
          {
            fast_width = 2U;
            fast_kind = FastDataInputKind::short_value;
          }
          else if (fast_name == "readUnsignedShort" &&
                   fast_descriptor == "()I")
          {
            fast_width = 2U;
            fast_kind = FastDataInputKind::unsigned_short;
          }
          else if (fast_name == "readChar" && fast_descriptor == "()C")
          {
            fast_width = 2U;
            fast_kind = FastDataInputKind::char_value;
          }
          else if (fast_name == "readInt" && fast_descriptor == "()I")
          {
            fast_width = 4U;
            fast_kind = FastDataInputKind::int_value;
          }
          else if (fast_name == "readLong" && fast_descriptor == "()J")
          {
            fast_width = 8U;
            fast_kind = FastDataInputKind::long_value;
          }
          else if (fast_name == "readFloat" && fast_descriptor == "()F")
          {
            fast_width = 4U;
            fast_kind = FastDataInputKind::float_value;
          }
          else if (fast_name == "readDouble" && fast_descriptor == "()D")
          {
            fast_width = 8U;
            fast_kind = FastDataInputKind::double_value;
          }

          if (fast_kind != FastDataInputKind::none)
          {
            const ObjectRef fast_receiver =
                nested->arguments.reference_unchecked(0U);
            auto fast_bits = try_read_byte_array_input_bits(
                fast_receiver, fast_width);
            if (!fast_bits)
            {
              if (fast_bits.error().code == ErrorCode::java_exception &&
                  !fast_bits.error().java_exception_class.empty())
              {
                auto raised = raise_implicit(
                    fast_bits.error().java_exception_class,
                    opcode_pc,
                    fast_bits.error().message);
                if (!raised)
                  return std::unexpected(raised.error());
                if (raised->has_value())
                  return std::move(**raised);
                break;
              }
              return std::unexpected(fast_bits.error());
            }
            if (fast_bits->has_value())
            {
              const u64 bits = **fast_bits;
              Value fast_value;
              switch (fast_kind)
              {
              case FastDataInputKind::boolean_value:
                fast_value = Value::from_int(bits == 0U ? 0 : 1);
                break;
              case FastDataInputKind::byte_value:
                fast_value = Value::from_int(static_cast<i8>(bits));
                break;
              case FastDataInputKind::unsigned_byte:
                fast_value = Value::from_int(static_cast<i32>(bits));
                break;
              case FastDataInputKind::short_value:
                fast_value = Value::from_int(static_cast<i16>(bits));
                break;
              case FastDataInputKind::unsigned_short:
              case FastDataInputKind::char_value:
                fast_value = Value::from_int(static_cast<u16>(bits));
                break;
              case FastDataInputKind::int_value:
                fast_value = Value::from_int(
                    static_cast<i32>(static_cast<u32>(bits)));
                break;
              case FastDataInputKind::long_value:
                fast_value = Value::from_long(static_cast<i64>(bits));
                break;
              case FastDataInputKind::float_value:
                fast_value = Value::from_float(std::bit_cast<float>(
                    static_cast<u32>(bits)));
                break;
              case FastDataInputKind::double_value:
                fast_value = Value::from_double(std::bit_cast<double>(bits));
                break;
              case FastDataInputKind::none:
                break;
              }

              auto released = release_synchronized_monitor(*nested_monitor);
              if (!released)
                return std::unexpected(released.error());
              if (budget_mode == InstructionBudgetMode::progress_watchdog)
                watchdog_instructions = 0U;
              auto pushed = frame.push(fast_value);
              if (!pushed)
                return std::unexpected(pushed.error());
              break;
            }
          }
        }
        if (nested_is_native && !nested_synchronized &&
            nested->method.owner != nullptr &&
            nested->method.method != nullptr &&
            ((nested->method.owner->name() == "java/lang/Object" &&
              nested->method.method->name == "<init>" &&
              nested->method.method->descriptor == "()V") ||
             (nested->method.owner->name() ==
                  "java/io/ByteArrayInputStream" &&
              nested->method.method->name == "close" &&
              nested->method.method->descriptor == "()V")))
        {
          // Object.<init> and ByteArrayInputStream.close are specified no-ops.
          // Avoid materializing a native call/root barrier for extremely common
          // constructor/resource-stream cleanup traffic.
          if (budget_mode == InstructionBudgetMode::progress_watchdog)
            watchdog_instructions = 0U;
          break;
        }
        if (nested_is_native &&
            nested->method.owner != nullptr &&
            nested->method.method != nullptr &&
            (nested->method.owner->name() == "java/lang/StringBuffer" ||
             nested->method.owner->name() == "java/lang/StringBuilder"))
        {
          const std::string& builder_name = nested->method.method->name;
          const std::string& builder_descriptor =
              nested->method.method->descriptor;
          const ObjectRef builder =
              nested->arguments.reference_unchecked(0U);
          bool builder_handled = false;
          std::optional<Value> builder_return;
          Status builder_status {};

          if (builder_name == "<init>" &&
              builder_descriptor == "(Ljava/lang/String;)V" &&
              nested->arguments.size() == 2U)
          {
            const ObjectRef source =
                nested->arguments.reference_unchecked(1U);
            if (source.is_null())
            {
              builder_status = fail_java(
                  "java/lang/NullPointerException",
                  "string builder String constructor received null");
            }
            else
            {
              auto text = heap_.vm_string_value(source);
              if (!text)
              {
                builder_status = std::unexpected(text.error());
              }
              else if (text->size() > static_cast<usize>(
                           std::numeric_limits<i32>::max()) - 16U)
              {
                builder_status = fail_java(
                    "java/lang/OutOfMemoryError",
                    "string builder capacity exceeds int range");
              }
              else
              {
                const i32 capacity = static_cast<i32>(text->size() + 16U);
                builder_status = heap_.vm_attach_string(
                    builder, std::move(*text));
                if (builder_status)
                {
                  builder_status = heap_.vm_set_field(
                      builder, 0U, Value::from_int(capacity));
                }
              }
            }
            builder_handled = true;
          }
          else if (builder_name == "append" &&
                   builder_descriptor ==
                       "(Ljava/lang/String;)Ljava/lang/StringBuffer;" &&
                   nested->arguments.size() == 2U)
          {
            const ObjectRef source =
                nested->arguments.reference_unchecked(1U);
            std::u16string text;
            if (source.is_null())
            {
              text.assign(u"null");
            }
            else
            {
              auto value = heap_.vm_string_value(source);
              if (!value)
              {
                builder_status = std::unexpected(value.error());
              }
              else
              {
                text = std::move(*value);
              }
            }
            if (builder_status)
            {
              builder_status = heap_.vm_append_mutable_text(
                  builder, 0U, text);
            }
            if (builder_status)
            {
              builder_return = Value::from_reference(builder);
            }
            builder_handled = true;
          }
          else if (builder_name == "append" &&
                   builder_descriptor ==
                       "(Ljava/lang/String;)Ljava/lang/StringBuilder;" &&
                   nested->arguments.size() == 2U)
          {
            const ObjectRef source =
                nested->arguments.reference_unchecked(1U);
            std::u16string text;
            if (source.is_null())
            {
              text.assign(u"null");
            }
            else
            {
              auto value = heap_.vm_string_value(source);
              if (!value)
              {
                builder_status = std::unexpected(value.error());
              }
              else
              {
                text = std::move(*value);
              }
            }
            if (builder_status)
            {
              builder_status = heap_.vm_append_mutable_text(
                  builder, 0U, text);
            }
            if (builder_status)
            {
              builder_return = Value::from_reference(builder);
            }
            builder_handled = true;
          }
          else if (builder_name == "append" &&
                   (builder_descriptor ==
                        "(I)Ljava/lang/StringBuffer;" ||
                    builder_descriptor ==
                        "(I)Ljava/lang/StringBuilder;") &&
                   nested->arguments.size() == 2U)
          {
            auto integer = nested->arguments[1U].as_int();
            if (!integer)
            {
              builder_status = std::unexpected(integer.error());
            }
            else
            {
              auto text = format_java_int(*integer);
              if (!text)
              {
                builder_status = std::unexpected(text.error());
              }
              else
              {
                builder_status = heap_.vm_append_mutable_text(
                    builder, 0U, *text);
              }
            }
            if (builder_status)
            {
              builder_return = Value::from_reference(builder);
            }
            builder_handled = true;
          }
          else if (builder_name == "toString" &&
                   builder_descriptor == "()Ljava/lang/String;" &&
                   nested->arguments.size() == 1U)
          {
            auto text = heap_.vm_string_value(builder);
            if (!text)
            {
              builder_status = std::unexpected(text.error());
            }
            else
            {
              auto string = states_.allocate_text_instance(
                  heap_, "java/lang/String", std::move(*text));
              if (!string)
              {
                builder_status = std::unexpected(string.error());
              }
              else
              {
                builder_return = Value::from_reference(*string);
              }
            }
            builder_handled = true;
          }

          if (builder_handled)
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            if (!builder_status)
            {
              if (builder_status.error().code == ErrorCode::overflow)
              {
                auto raised = raise_implicit(
                    "java/lang/OutOfMemoryError", opcode_pc,
                    builder_status.error().message);
                if (!raised)
                  return std::unexpected(raised.error());
                if (raised->has_value())
                  return std::move(**raised);
                break;
              }
              if (builder_status.error().code == ErrorCode::java_exception &&
                  !builder_status.error().java_exception_class.empty())
              {
                auto raised = raise_implicit(
                    builder_status.error().java_exception_class,
                    opcode_pc,
                    builder_status.error().message);
                if (!raised)
                  return std::unexpected(raised.error());
                if (raised->has_value())
                  return std::move(**raised);
                break;
              }
              return std::unexpected(builder_status.error());
            }
            if (budget_mode == InstructionBudgetMode::progress_watchdog)
              watchdog_instructions = 0U;
            if (builder_return.has_value())
            {
              auto pushed = frame.push(*builder_return);
              if (!pushed)
                return std::unexpected(pushed.error());
            }
            break;
          }
        }
        if (nested_is_native)
        {
          // NativeJitPolicy::synchronous_bounded already certifies that the
          // implementation is finite, non-blocking and safe to execute once
          // while the VM gate is owned (Graphics/Math and similarly bounded
          // HLE helpers). Such a call cannot hand execution to another Java
          // thread, so publishing/scanning every active frame before it adds
          // pure overhead. Conservative natives retain the full root barrier.
          const bool native_can_keep_roots_deferred =
              nested->native_method.valid() &&
              natives_.jit_policy(nested->native_method) ==
                  NativeJitPolicy::synchronous_bounded;
          if (!nested_synchronized && !native_can_keep_roots_deferred)
          {
            publish_compact_execution_roots(nested->arguments,
                                            nested->return_override);
          }
          if (std::getenv("PHONEME_TRACE_NETWORK_CALLERS") != nullptr &&
              nested->method.method != nullptr &&
              (nested->method.method->name == "write" ||
               nested->method.method->name == "flush") &&
              nested->method.owner != nullptr)
          {
            const std::string_view native_owner =
                nested->method.owner->name();
            if (native_owner.find("java/io/") != std::string_view::npos ||
                native_owner.find("javax/microedition/io/") !=
                    std::string_view::npos)
            {
              std::fprintf(stderr,
                           "[phoneMENetworkCaller] caller=%s.%s%s bci=%zu "
                           "target=%s.%s%s\n",
                           frame.owner().name().c_str(),
                           frame.method().name.c_str(),
                           frame.method().descriptor.c_str(),
                           opcode_pc,
                           nested->method.owner->name().c_str(),
                           nested->method.method->name.c_str(),
                           nested->method.method->descriptor.c_str());
            }
          }
          const auto native_started = std::chrono::steady_clock::now();
          auto native_result = invoke_native(*nested);
          const auto native_duration =
              std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - native_started);
          if (native_duration >= std::chrono::milliseconds(10) &&
              should_trace_slow_native(nested->method.owner->name(),
                                       nested->method.method->name))
          {
            vm_trace("native",
                     "slow-call java=%u target=%s.%s%s duration_us=%lld ok=%d",
                     static_cast<unsigned>(scheduler_.current_thread_id()),
                     nested->method.owner->name().c_str(),
                     nested->method.method->name.c_str(),
                     nested->method.method->descriptor.c_str(),
                     static_cast<long long>(native_duration.count()),
                     native_result.has_value() ? 1 : 0);
          }
          auto released = release_synchronized_monitor(*nested_monitor);
          if (!released)
            return std::unexpected(released.error());
          if (!native_result)
          {
            if (native_result.error().code == ErrorCode::java_exception)
            {
              if (native_result.error().java_exception_class.empty())
              {
                return fail(ErrorCode::internal_error,
                            "native Java exception has no class name");
              }
              auto raised = raise_implicit(
                  native_result.error().java_exception_class,
                  opcode_pc,
                  native_result.error().message);
              if (!raised)
                return std::unexpected(raised.error());
              if (raised->has_value())
                return std::move(**raised);
              break;
            }
            if (native_result.error().code ==
                    ErrorCode::unsupported_feature &&
                (nested->method.method->access_flags & kAccNative) != 0U)
            {
              auto raised = raise_implicit(
                  "java/lang/UnsatisfiedLinkError", opcode_pc);
              if (!raised)
                return std::unexpected(raised.error());
              if (raised->has_value())
                return std::move(**raised);
              break;
            }
            return std::unexpected(native_result.error());
          }
          if (budget_mode == InstructionBudgetMode::progress_watchdog)
            watchdog_instructions = 0U;
          std::optional<Value> nested_return = *native_result;
          if (nested->return_reference_cast_target.has_value())
          {
            if (!nested_return.has_value())
            {
              return fail(ErrorCode::invalid_state,
                          "cast lambda native return has no reference value");
            }
            auto casted = cast_lambda_reference(
                *nested_return, *nested->return_reference_cast_target);
            if (!casted)
            {
              if (casted.error().code == ErrorCode::java_exception &&
                  !casted.error().java_exception_class.empty())
              {
                auto raised = raise_implicit(
                    casted.error().java_exception_class,
                    opcode_pc,
                    casted.error().message);
                if (!raised)
                  return std::unexpected(raised.error());
                if (raised->has_value())
                  return std::move(**raised);
                break;
              }
              return std::unexpected(casted.error());
            }
            nested_return = *casted;
          }
          if (nested->return_unboxing_target.has_value())
          {
            if (!nested_return.has_value())
            {
              return fail(ErrorCode::invalid_state,
                          "unboxed lambda native return has no reference value");
            }
            auto unboxed = unbox_lambda_value(
                *nested_return, *nested->return_unboxing_target);
            if (!unboxed)
            {
              if (unboxed.error().code == ErrorCode::java_exception &&
                  !unboxed.error().java_exception_class.empty())
              {
                auto raised = raise_implicit(
                    unboxed.error().java_exception_class,
                    opcode_pc,
                    unboxed.error().message);
                if (!raised)
                  return std::unexpected(raised.error());
                if (raised->has_value())
                  return std::move(**raised);
                break;
              }
              return std::unexpected(unboxed.error());
            }
            nested_return = *unboxed;
          }
          if (nested->return_widening_source.has_value() &&
              nested->return_widening_target.has_value())
          {
            if (!nested_return.has_value())
            {
              return fail(ErrorCode::invalid_state,
                          "widened lambda native return has no primitive value");
            }
            auto widened = widen_primitive_value(
                *nested_return,
                *nested->return_widening_source,
                *nested->return_widening_target);
            if (!widened)
              return std::unexpected(widened.error());
            nested_return = *widened;
          }
          if (nested->return_override.has_value())
          {
            if (nested->return_override_boxes_result)
            {
              if (!nested_return.has_value())
              {
                return fail(ErrorCode::invalid_state,
                            "boxed lambda native return has no primitive value");
              }
              auto wrapper = nested->return_override->as_reference();
              if (!wrapper || wrapper->is_null())
              {
                return fail(ErrorCode::internal_error,
                            "boxed lambda native return has no wrapper object");
              }
              auto stored = heap_.vm_set_field(*wrapper, 0U, *nested_return);
              if (!stored)
                return std::unexpected(stored.error());
            }
            nested_return = nested->return_override;
          }
          if (nested->discard_return_value)
            nested_return = std::nullopt;
          if (nested_return.has_value())
          {
            auto pushed = frame.push(*nested_return);
            if (!pushed)
              return std::unexpected(pushed.error());
          }
        }
        else
        {
          if (nested->descriptor == nullptr)
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            return fail(ErrorCode::internal_error,
                        "nested invocation has no cached descriptor");
          }
          auto next = ExecutionFrame::emplace(
              frames,
              std::move(nested->method),
              nested->descriptor,
              nested->arguments,
              nested->has_receiver);
          if (!next)
          {
            auto released = release_synchronized_monitor(*nested_monitor);
            if (!released)
              return std::unexpected(released.error());
            return std::unexpected(next.error());
          }
          if (nested_monitor->has_value())
          {
            (*next)->set_synchronized_monitor(**nested_monitor);
          }
          if (nested->return_override.has_value())
          {
            (*next)->set_return_override(*nested->return_override,
                                         nested->return_override_boxes_result);
          }
          (*next)->set_discard_return_value(nested->discard_return_value);
          if (nested->return_reference_cast_target.has_value())
          {
            (*next)->set_return_reference_cast(
                *nested->return_reference_cast_target);
          }
          if (nested->return_unboxing_target.has_value())
          {
            (*next)->set_return_unboxing(*nested->return_unboxing_target);
          }
          if (nested->return_widening_source.has_value() &&
              nested->return_widening_target.has_value())
          {
            (*next)->set_return_widening(*nested->return_widening_source,
                                         *nested->return_widening_target);
          }
          PerformanceCounters::observe_java_call_depth(frames.size());
        }
        break;
      }
      case 0xBA:
      {
        auto index = frame.read_invokedynamic_index();
        if (!index)
          return std::unexpected(index.error());
        auto dynamic = frame.owner().invoke_dynamic_reference(*index);
        if (!dynamic)
          return std::unexpected(dynamic.error());
        auto call_site = parse_method_descriptor(dynamic->descriptor);
        if (!call_site)
          return std::unexpected(call_site.error());
        auto captures = pop_arguments(
            frame, *call_site, false, !frame.has_verified_types());
        if (!captures)
          return std::unexpected(captures.error());
        auto binding = resolve_lambda_binding(frame.owner(), *index);
        if (!binding)
        {
          if (binding.error().code == ErrorCode::unsupported_feature)
          {
            auto raised = raise_implicit("java/lang/BootstrapMethodError",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          return std::unexpected(binding.error());
        }
        auto lambda = allocate_raw_object_with_gc(binding->interface_name,
                                                  captures->size());
        if (!lambda)
        {
          if (lambda.error().code == ErrorCode::overflow)
          {
            auto raised = raise_implicit("java/lang/OutOfMemoryError",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          return std::unexpected(lambda.error());
        }
        for (usize capture_index = 0;
             capture_index < captures->size();
             ++capture_index)
        {
          auto stored = heap_.vm_set_field(*lambda,
                                           capture_index,
                                           (*captures)[capture_index]);
          if (!stored)
            return std::unexpected(stored.error());
        }
        lambda_bindings_.insert_or_assign(lambda->bits, std::move(*binding));
        auto pushed = frame.push_reference(*lambda);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0xBB:
      {
        auto index = frame.read_constant_pool_index();
        if (!index)
          return std::unexpected(index.error());
        auto class_operand = resolve_class_operand(
            frame.runtime_method_id(),
            frame.current_decoded_operand_index(),
            opcode_pc,
            frame.owner(),
            *index,
            true,
            false);
        if (!class_operand)
        {
          auto raised = raise_resolution_error(class_operand.error(), opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }

        std::string legacy_class_name;
        std::string_view class_name;
        std::shared_ptr<const classfile::ClassFile> allocated_class;
        if (*class_operand != nullptr)
        {
          class_name = (*class_operand)->target_class_name;
          allocated_class = (*class_operand)->target_class_file;
        }
        else
        {
          auto resolved_name = frame.owner().class_name_constant(*index);
          if (!resolved_name)
            return std::unexpected(resolved_name.error());
          legacy_class_name = std::move(*resolved_name);
          class_name = legacy_class_name;
          auto loaded = load_linkage_class(class_name);
          if (!loaded)
          {
            auto raised = raise_resolution_error(stable_linkage_error(
                loaded.error(), OperandResolutionKind::class_reference),
                opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          allocated_class = std::move(*loaded);
        }
        if ((allocated_class->access_flags() &
             (kAccInterface | kAccAbstract)) != 0U)
        {
          auto raised = raise_implicit("java/lang/InstantiationError",
                                       opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        auto initialized = ensure_initialized_from_execution(
            class_name,
            remaining_execution_budget());
        if (!initialized)
          return std::unexpected(initialized.error());
        if (initialized->has_value())
        {
          auto dispatched = dispatch_exception(**initialized, opcode_pc);
          if (!dispatched)
            return std::unexpected(dispatched.error());
          if (dispatched->has_value())
            return std::move(**dispatched);
          break;
        }
        auto object = allocate_instance_with_gc(class_name);
        if (!object)
        {
          if (object.error().code == ErrorCode::overflow)
          {
            auto raised = raise_implicit("java/lang/OutOfMemoryError",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          return std::unexpected(object.error());
        }
        auto pushed = frame.push_reference(*object);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0xBC:
      {
        auto atype = frame.read_newarray_type();
        auto count = pop_int(frame);
        if (!atype || !count)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid newarray operands");
        }
        if (*count < 0)
        {
          auto raised = raise_implicit(
              "java/lang/NegativeArraySizeException", opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        auto type = primitive_array_type(*atype);
        if (!type)
          return std::unexpected(type.error());
        auto array = allocate_array_with_gc(type->first,
                                            static_cast<usize>(*count),
                                            type->second);
        if (!array)
        {
          if (array.error().code == ErrorCode::overflow)
          {
            auto raised = raise_implicit("java/lang/OutOfMemoryError",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          return std::unexpected(array.error());
        }
        auto pushed = frame.push_reference(*array);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0xBD:
      {
        auto index = frame.read_constant_pool_index();
        auto count = pop_int(frame);
        if (!index || !count)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid anewarray operands");
        }
        if (*count < 0)
        {
          auto raised = raise_implicit(
              "java/lang/NegativeArraySizeException", opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        auto class_operand = resolve_class_operand(
            frame.runtime_method_id(),
            frame.current_decoded_operand_index(),
            opcode_pc,
            frame.owner(),
            *index,
            true,
            true);
        if (!class_operand)
        {
          auto raised = raise_resolution_error(class_operand.error(), opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        std::string legacy_array_name;
        std::string_view array_name;
        if (*class_operand != nullptr)
        {
          array_name = (*class_operand)->target_array_name;
        }
        else
        {
          auto component = frame.owner().class_name_constant(*index);
          if (!component)
            return std::unexpected(component.error());
          auto loaded_component = load_linkage_class(*component);
          if (!loaded_component)
          {
            auto raised = raise_resolution_error(stable_linkage_error(
                loaded_component.error(),
                OperandResolutionKind::class_reference), opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          if (component->starts_with('['))
            legacy_array_name = '[' + *component;
          else
            legacy_array_name = "[L" + *component + ';';
          array_name = legacy_array_name;
        }
        auto array = allocate_array_with_gc(array_name,
                                            static_cast<usize>(*count),
                                            Value::from_reference({}));
        if (!array)
        {
          if (array.error().code == ErrorCode::overflow)
          {
            auto raised = raise_implicit("java/lang/OutOfMemoryError",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          return std::unexpected(array.error());
        }
        auto pushed = frame.push_reference(*array);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0xBE:
      {
        auto array = pop_reference(frame);
        if (!array)
          return std::unexpected(array.error());
        if (array->is_null())
        {
          auto raised = raise_implicit("java/lang/NullPointerException",
                                       opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        auto length = heap_.vm_array_length(*array);
        if (!length)
          return std::unexpected(length.error());
        if (*length > static_cast<usize>(std::numeric_limits<i32>::max()))
        {
          return fail(ErrorCode::overflow,
                      "Java array length exceeds int range");
        }
        auto pushed = frame.push(
            Value::from_int(static_cast<i32>(*length)));
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0xBF:
      {
        auto throwable = pop_reference(frame);
        if (!throwable)
          return std::unexpected(throwable.error());
        if (throwable->is_null())
        {
          auto raised = raise_implicit("java/lang/NullPointerException",
                                       opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        auto dispatched = dispatch_exception(*throwable, opcode_pc);
        if (!dispatched)
          return std::unexpected(dispatched.error());
        if (dispatched->has_value())
          return std::move(**dispatched);
        break;
      }
      case 0xC0:
      case 0xC1:
      {
        auto index = frame.read_constant_pool_index();
        auto reference = pop_reference(frame);
        if (!index || !reference)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid type-check operands");
        }
        auto class_operand = resolve_class_operand(
            frame.runtime_method_id(),
            frame.current_decoded_operand_index(),
            opcode_pc,
            frame.owner(),
            *index,
            true,
            false);
        if (!class_operand)
        {
          auto raised = raise_resolution_error(class_operand.error(), opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        std::string legacy_target_class;
        std::string_view target_class;
        if (*class_operand != nullptr)
        {
          target_class = (*class_operand)->target_class_name;
        }
        else
        {
          auto resolved_target = frame.owner().class_name_constant(*index);
          if (!resolved_target)
            return std::unexpected(resolved_target.error());
          legacy_target_class = std::move(*resolved_target);
          target_class = legacy_target_class;
          auto loaded_target = load_linkage_class(target_class);
          if (!loaded_target)
          {
            auto raised = raise_resolution_error(stable_linkage_error(
                loaded_target.error(),
                OperandResolutionKind::class_reference), opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
        }

        if (reference->is_null())
        {
          auto pushed = frame.push(opcode == 0xC0
                                       ? Value::from_reference(*reference)
                                       : Value::from_int(0));
          if (!pushed)
            return std::unexpected(pushed.error());
          break;
        }

        auto source_class = heap_.vm_class_name_view(*reference);
        if (!source_class)
          return std::unexpected(source_class.error());
        auto assignable = classes_.is_assignable(*source_class,
                                                 target_class);
        if (!assignable)
          return std::unexpected(assignable.error());
        bool is_assignable = *assignable;
        if (!is_assignable)
        {
          const auto lambda = lambda_bindings_.find(reference->bits);
          if (lambda != lambda_bindings_.end())
          {
            is_assignable = std::find(
                                lambda->second.marker_interfaces.begin(),
                                lambda->second.marker_interfaces.end(),
                                target_class) !=
                            lambda->second.marker_interfaces.end();
          }
        }
        if (opcode == 0xC0)
        {
          if (!is_assignable)
          {
            auto raised = raise_implicit("java/lang/ClassCastException",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          auto pushed = frame.push_reference(*reference);
          if (!pushed)
            return std::unexpected(pushed.error());
        }
        else
        {
          auto pushed = frame.push_int(is_assignable ? 1 : 0);
          if (!pushed)
            return std::unexpected(pushed.error());
        }
        break;
      }
      case 0xC2:
      case 0xC3:
      {
        auto reference = pop_reference(frame);
        if (!reference)
          return std::unexpected(reference.error());
        if (reference->is_null())
        {
          auto raised = raise_implicit("java/lang/NullPointerException",
                                       opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        if (opcode == 0xC2)
        {
          auto entered = monitors_.enter(*reference,
                                         scheduler_.current_thread_id());
          if (!entered)
            return std::unexpected(entered.error());
          if (*entered == MonitorEnterResult::would_block)
          {
            const std::array<Value, 1U> monitor_root {
                Value::from_reference(*reference)};
            publish_active_execution_roots(monitor_root);
            auto blocked = enter_monitor(*reference);
            if (!blocked)
              return std::unexpected(blocked.error());
          }
        }
        else
        {
          auto exited = monitors_.exit(*reference,
                                       scheduler_.current_thread_id());
          if (!exited)
          {
            if (exited.error().code == ErrorCode::java_exception)
            {
              auto raised = raise_implicit(
                  exited.error().java_exception_class,
                  opcode_pc,
                  exited.error().message);
              if (!raised)
                return std::unexpected(raised.error());
              if (raised->has_value())
                return std::move(**raised);
              break;
            }
            return std::unexpected(exited.error());
          }
        }
        break;
      }
      case 0xC4:
      {
        auto operands = frame.read_wide_operands();
        if (!operands)
          return std::unexpected(operands.error());
        const u8 widened_opcode = operands->opcode;
        const u16 index = operands->local_index;
        if (widened_opcode >= 0x15 && widened_opcode <= 0x19)
        {
          Status pushed;
          if (widened_opcode == 0x15)
          {
            auto value = frame.local_int(index);
            if (!value) return std::unexpected(value.error());
            pushed = frame.push_int(*value);
          }
          else if (widened_opcode == 0x16)
          {
            auto value = frame.local_long(index);
            if (!value) return std::unexpected(value.error());
            pushed = frame.push_long(*value);
          }
          else if (widened_opcode == 0x17)
          {
            auto value = frame.local_float(index);
            if (!value) return std::unexpected(value.error());
            pushed = frame.push_float(*value);
          }
          else if (widened_opcode == 0x18)
          {
            auto value = frame.local_double(index);
            if (!value) return std::unexpected(value.error());
            pushed = frame.push_double(*value);
          }
          else
          {
            auto value = frame.local_reference(index);
            if (!value) return std::unexpected(value.error());
            pushed = frame.push_reference(*value);
          }
          if (!pushed)
            return std::unexpected(pushed.error());
        }
        else if (widened_opcode >= 0x36 && widened_opcode <= 0x3A)
        {
          Status stored;
          if (widened_opcode == 0x36)
          {
            auto value = frame.pop_int();
            if (!value) return std::unexpected(value.error());
            stored = frame.set_local_int(index, *value);
          }
          else if (widened_opcode == 0x37)
          {
            auto value = frame.pop_long();
            if (!value) return std::unexpected(value.error());
            stored = frame.set_local_long(index, *value);
          }
          else if (widened_opcode == 0x38)
          {
            auto value = frame.pop_float();
            if (!value) return std::unexpected(value.error());
            stored = frame.set_local_float(index, *value);
          }
          else if (widened_opcode == 0x39)
          {
            auto value = frame.pop_double();
            if (!value) return std::unexpected(value.error());
            stored = frame.set_local_double(index, *value);
          }
          else
          {
            auto value = frame.pop_reference();
            if (!value) return std::unexpected(value.error());
            stored = frame.set_local_reference(index, *value);
          }
          if (!stored)
            return std::unexpected(stored.error());
        }
        else if (widened_opcode == 0x84)
        {
          if (!operands->increment.has_value())
          {
            return fail(ErrorCode::malformed_class,
                        "wide iinc has no decoded increment");
          }
          auto current = frame.local_int(index);
          if (!current)
            return std::unexpected(current.error());
          const i32 updated = static_cast<i32>(
              static_cast<u32>(*current) +
              static_cast<u32>(static_cast<i32>(*operands->increment)));
          auto stored = frame.set_local_int(index, updated);
          if (!stored)
            return std::unexpected(stored.error());
        }
        else if (widened_opcode == 0xA9)
        {
          auto address_value = frame.local(index);
          if (!address_value)
          {
            return std::unexpected(address_value.error());
          }
          auto address = address_value->as_return_address();
          if (!address)
            return std::unexpected(address.error());
          auto jumped = frame.jump_absolute(*address);
          if (!jumped)
            return std::unexpected(jumped.error());
        }
        else
        {
          return fail(ErrorCode::unsupported_feature,
                      "wide opcode is not ported yet");
        }
        break;
      }
      case 0xC5:
      {
        auto operands = frame.read_multianewarray_operands();
        if (!operands)
          return std::unexpected(operands.error());
        const u8 dimensions = operands->dimensions;
        auto class_operand = resolve_class_operand(
            frame.runtime_method_id(),
            frame.current_decoded_operand_index(),
            opcode_pc,
            frame.owner(),
            operands->constant_pool_index,
            true,
            false);
        if (!class_operand)
        {
          auto raised = raise_resolution_error(class_operand.error(), opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        std::string legacy_array_class;
        std::string_view array_class;
        if (*class_operand != nullptr)
        {
          array_class = (*class_operand)->target_class_name;
        }
        else
        {
          auto resolved_array_class = frame.owner().class_name_constant(
              operands->constant_pool_index);
          if (!resolved_array_class)
            return std::unexpected(resolved_array_class.error());
          legacy_array_class = std::move(*resolved_array_class);
          array_class = legacy_array_class;
          auto loaded_array_class = load_linkage_class(array_class);
          if (!loaded_array_class)
          {
            auto raised = raise_resolution_error(stable_linkage_error(
                loaded_array_class.error(),
                OperandResolutionKind::class_reference), opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
        }
        usize declared_dimensions = 0;
        while (declared_dimensions < array_class.size() &&
               array_class[declared_dimensions] == '[')
        {
          ++declared_dimensions;
        }
        if (declared_dimensions == 0U ||
            static_cast<usize>(dimensions) > declared_dimensions)
        {
          return fail(ErrorCode::malformed_class,
                      "multianewarray dimensions exceed descriptor rank");
        }

        std::vector<i32> lengths(dimensions);
        for (usize reverse = lengths.size(); reverse > 0; --reverse)
        {
          auto length = pop_int(frame);
          if (!length)
            return std::unexpected(length.error());
          lengths[reverse - 1U] = *length;
        }
        bool has_negative_length = false;
        for (const i32 length : lengths)
        {
          if (length < 0)
          {
            has_negative_length = true;
            break;
          }
        }
        if (has_negative_length)
        {
          auto raised = raise_implicit(
              "java/lang/NegativeArraySizeException", opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }

        usize arrays_at_level = 1U;
        usize total_arrays = 0U;
        bool exceeds_array_budget = false;
        for (usize level = 0; level < lengths.size(); ++level)
        {
          auto updated_total = checked_add(total_arrays,
                                           arrays_at_level);
          if (!updated_total || *updated_total > 1'000'000U)
          {
            exceeds_array_budget = true;
            break;
          }
          total_arrays = *updated_total;
          if (level + 1U < lengths.size())
          {
            auto next_count = checked_multiply(
                arrays_at_level,
                static_cast<usize>(lengths[level]));
            if (!next_count || *next_count > 1'000'000U)
            {
              exceeds_array_budget = true;
              break;
            }
            arrays_at_level = *next_count;
          }
        }
        if (exceeds_array_budget)
        {
          auto raised = raise_implicit("java/lang/OutOfMemoryError",
                                       opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }

        auto root_default = array_default_value(array_class);
        if (!root_default)
          return std::unexpected(root_default.error());
        auto root = allocate_array_with_gc(
            array_class,
            static_cast<usize>(lengths.front()),
            *root_default);
        if (!root)
        {
          if (root.error().code == ErrorCode::overflow)
          {
            auto raised = raise_implicit("java/lang/OutOfMemoryError",
                                         opcode_pc);
            if (!raised)
              return std::unexpected(raised.error());
            if (raised->has_value())
              return std::move(**raised);
            break;
          }
          return std::unexpected(root.error());
        }

        struct PendingArray final
        {
          ObjectRef reference;
          usize level{0};
        };
        std::vector<PendingArray> pending;
        pending.reserve(total_arrays);
        pending.push_back(PendingArray{.reference = *root, .level = 0});
        bool allocation_failed = false;
        for (usize cursor = 0; cursor < pending.size(); ++cursor)
        {
          const PendingArray current = pending[cursor];
          const usize child_level = current.level + 1U;
          if (child_level >= lengths.size())
          {
            continue;
          }
          const std::string_view child_descriptor =
              array_class.substr(child_level);
          const usize child_count =
              static_cast<usize>(lengths[child_level]);
          auto child_default = array_default_value(child_descriptor);
          if (!child_default)
            return std::unexpected(child_default.error());
          const usize parent_length =
              static_cast<usize>(lengths[current.level]);
          for (usize element = 0; element < parent_length; ++element)
          {
            auto child = allocate_array_with_gc(
                child_descriptor,
                child_count,
                *child_default,
                *root);
            if (!child)
            {
              if (child.error().code == ErrorCode::overflow)
              {
                allocation_failed = true;
                break;
              }
              return std::unexpected(child.error());
            }
            auto stored = heap_.vm_set_element(
                current.reference,
                element,
                Value::from_reference(*child));
            if (!stored)
              return std::unexpected(stored.error());
            pending.push_back(PendingArray{
                .reference = *child,
                .level = child_level,
            });
          }
          if (allocation_failed)
          {
            break;
          }
        }
        if (allocation_failed)
        {
          auto raised = raise_implicit("java/lang/OutOfMemoryError",
                                       opcode_pc);
          if (!raised)
            return std::unexpected(raised.error());
          if (raised->has_value())
            return std::move(**raised);
          break;
        }
        auto pushed = frame.push_reference(*root);
        if (!pushed)
          return std::unexpected(pushed.error());
        break;
      }
      case 0xC6:
      case 0xC7:
      {
        auto offset = frame.read_branch_offset(false);
        auto reference = pop_reference(frame);
        if (!offset || !reference)
        {
          return fail(ErrorCode::malformed_class,
                      "invalid null-comparison branch");
        }
        const bool branch = opcode == 0xC6
                                ? reference->is_null()
                                : !reference->is_null();
        if (branch)
        {
          auto branched = frame.branch(opcode_pc, *offset);
          if (!branched)
            return std::unexpected(branched.error());
        }
        break;
      }
      case 0xC8:
      {
        auto offset = frame.read_branch_offset(true);
        if (!offset)
          return std::unexpected(offset.error());
        auto branched = frame.branch(opcode_pc, *offset);
        if (!branched)
          return std::unexpected(branched.error());
        break;
      }
      case 0xC9:
      {
        auto offset = frame.read_branch_offset(true);
        if (!offset)
          return std::unexpected(offset.error());
        const usize return_pc = frame.pc();
        auto pushed = frame.push(Value::return_address(return_pc));
        if (!pushed)
          return std::unexpected(pushed.error());
        auto branched = frame.branch(opcode_pc, *offset);
        if (!branched)
          return std::unexpected(branched.error());
        break;
      }
      default:
        return fail(ErrorCode::unsupported_feature,
                    "VM opcode is not ported yet: " +
                        std::to_string(opcode));
      }
    }

    return fail(ErrorCode::internal_error,
                "VM call stack terminated without a result");
  }

} // namespace phoneme::vm
