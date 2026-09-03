#include "TimeNatives.hpp"

#include "CalendarNatives.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <ctime>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "phoneme/vm/Machine.hpp"

namespace phoneme::vm {
namespace {

constexpr usize kDateTimeField = 0U;
constexpr usize kZoneIdField = 0U;
constexpr usize kZoneRawOffsetField = 1U;
constexpr usize kCalendarTimeField = 0U;
constexpr usize kCalendarZoneField = 1U;
constexpr usize kCalendarFieldsField = 2U;
constexpr usize kCalendarIsSetField = 3U;
constexpr usize kLocalTimeHourField = 0U;
constexpr usize kLocalTimeMinuteField = 1U;
constexpr usize kLocalTimeSecondField = 2U;
constexpr usize kLocalTimeNanoField = 3U;
constexpr usize kInstantEpochMilliField = 0U;
constexpr usize kJavaTimeZoneIdField = 0U;
constexpr usize kJavaTimeZoneRawOffsetField = 1U;
constexpr usize kFormatterPatternField = 0U;
constexpr usize kFormatterZoneField = 1U;
constexpr usize kSimpleDateFormatPatternField = 0U;

constexpr i64 kMillisPerSecond = 1'000LL;
constexpr i64 kMillisPerMinute = 60LL * kMillisPerSecond;
constexpr i64 kMillisPerHour = 60LL * kMillisPerMinute;
constexpr i64 kMillisPerDay = 24LL * kMillisPerHour;

constexpr i32 kYear = 1;
constexpr i32 kMonth = 2;
constexpr i32 kDate = 5;
constexpr i32 kDayOfWeek = 7;
constexpr i32 kAmPm = 9;
constexpr i32 kHour = 10;
constexpr i32 kHourOfDay = 11;
constexpr i32 kMinute = 12;
constexpr i32 kSecond = 13;
constexpr i32 kMillisecond = 14;

struct CivilFields final {
    i32 year {1970};
    i32 month {0};
    i32 day {1};
    i32 day_of_week {5};
    i32 hour {0};
    i32 minute {0};
    i32 second {0};
    i32 millisecond {0};
};

void add(NativeMethodRegistry& registry,
         std::string owner,
         std::string name,
         std::string descriptor,
         NativeMethod implementation) {
    auto registered = registry.register_method(std::move(owner),
                                               std::move(name),
                                               std::move(descriptor),
                                               std::move(implementation));
    if (!registered) std::terminate();
}

[[nodiscard]] Result<ObjectRef> receiver(std::span<const Value> arguments) {
    if (arguments.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "time native is missing its receiver");
    }
    auto reference = arguments.front().as_reference();
    if (!reference || reference->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "time native receiver is null");
    }
    return *reference;
}

[[nodiscard]] i64 current_millis() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<i64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

[[nodiscard]] constexpr i64 floor_div(i64 value, i64 divisor) noexcept {
    i64 quotient = value / divisor;
    const i64 remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) --quotient;
    return quotient;
}

[[nodiscard]] constexpr i64 floor_mod(i64 value, i64 divisor) noexcept {
    return value - floor_div(value, divisor) * divisor;
}

[[nodiscard]] Result<i64> checked_add_i64(i64 left, i64 right) {
    if ((right > 0 && left > std::numeric_limits<i64>::max() - right) ||
        (right < 0 && left < std::numeric_limits<i64>::min() - right)) {
        return fail_java("java/lang/IllegalArgumentException",
                         "calendar value exceeds long range");
    }
    return left + right;
}

[[nodiscard]] Result<i64> checked_multiply_i64(i64 left, i64 right) {
    if (left == 0 || right == 0) return 0;
    constexpr i64 minimum = std::numeric_limits<i64>::min();
    constexpr i64 maximum = std::numeric_limits<i64>::max();
    const bool overflow =
        (left > 0 && right > 0 && left > maximum / right) ||
        (left > 0 && right < 0 && right < minimum / left) ||
        (left < 0 && right > 0 && left < minimum / right) ||
        (left < 0 && right < 0 && left < maximum / right);
    if (overflow) {
        return fail_java("java/lang/IllegalArgumentException",
                         "calendar value exceeds long range");
    }
    return left * right;
}

[[nodiscard]] constexpr i64 days_from_civil(i64 year,
                                             unsigned month,
                                             unsigned day) noexcept {
    year -= month <= 2U ? 1 : 0;
    const i64 era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153U * (month + (month > 2U ? static_cast<unsigned>(-3) : 9U)) + 2U) /
            5U +
        day - 1U;
    const unsigned day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U +
        day_of_year;
    return era * 146097LL + static_cast<i64>(day_of_era) - 719468LL;
}

[[nodiscard]] CivilFields civil_from_days(i64 days) noexcept {
    i64 shifted = days + 719468LL;
    const i64 era = (shifted >= 0 ? shifted : shifted - 146096LL) / 146097LL;
    const unsigned day_of_era =
        static_cast<unsigned>(shifted - era * 146097LL);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U -
         day_of_era / 146096U) /
        365U;
    i64 year = static_cast<i64>(year_of_era) + era * 400LL;
    const unsigned day_of_year =
        day_of_era - (365U * year_of_era + year_of_era / 4U -
                      year_of_era / 100U);
    const unsigned month_prime = (5U * day_of_year + 2U) / 153U;
    const unsigned day =
        day_of_year - (153U * month_prime + 2U) / 5U + 1U;
    const unsigned month =
        month_prime + (month_prime < 10U ? 3U : static_cast<unsigned>(-9));
    year += month <= 2U ? 1 : 0;

    CivilFields result;
    if (year < static_cast<i64>(std::numeric_limits<i32>::min())) {
        result.year = std::numeric_limits<i32>::min();
    } else if (year > static_cast<i64>(std::numeric_limits<i32>::max())) {
        result.year = std::numeric_limits<i32>::max();
    } else {
        result.year = static_cast<i32>(year);
    }
    result.month = static_cast<i32>(month - 1U);
    result.day = static_cast<i32>(day);
    result.day_of_week = static_cast<i32>(floor_mod(days + 4LL, 7LL) + 1LL);
    return result;
}

[[nodiscard]] Result<CivilFields> fields_from_epoch(i64 epoch_millis,
                                                    i32 raw_offset) {
    auto local = checked_add_i64(epoch_millis, static_cast<i64>(raw_offset));
    if (!local) return std::unexpected(local.error());
    const i64 days = floor_div(*local, kMillisPerDay);
    i64 day_millis = floor_mod(*local, kMillisPerDay);
    CivilFields result = civil_from_days(days);
    result.hour = static_cast<i32>(day_millis / kMillisPerHour);
    day_millis %= kMillisPerHour;
    result.minute = static_cast<i32>(day_millis / kMillisPerMinute);
    day_millis %= kMillisPerMinute;
    result.second = static_cast<i32>(day_millis / kMillisPerSecond);
    result.millisecond = static_cast<i32>(day_millis % kMillisPerSecond);
    return result;
}

[[nodiscard]] Result<i64> epoch_from_fields(CivilFields fields,
                                            i32 raw_offset) {
    i64 year = fields.year;
    const i64 normalized_year_delta = floor_div(fields.month, 12);
    year += normalized_year_delta;
    const i64 normalized_month = floor_mod(fields.month, 12);
    const i64 first_day = days_from_civil(
        year, static_cast<unsigned>(normalized_month + 1LL), 1U);
    auto days = checked_add_i64(first_day,
                                static_cast<i64>(fields.day) - 1LL);
    if (!days) return std::unexpected(days.error());
    auto day_part = checked_multiply_i64(*days, kMillisPerDay);
    if (!day_part) return std::unexpected(day_part.error());

    i64 time_part = static_cast<i64>(fields.hour) * kMillisPerHour;
    time_part += static_cast<i64>(fields.minute) * kMillisPerMinute;
    time_part += static_cast<i64>(fields.second) * kMillisPerSecond;
    time_part += static_cast<i64>(fields.millisecond);
    auto local = checked_add_i64(*day_part, time_part);
    if (!local) return std::unexpected(local.error());
    return checked_add_i64(*local, -static_cast<i64>(raw_offset));
}

[[nodiscard]] Result<ObjectRef> create_string(Machine& machine,
                                              std::u16string text) {
    auto object = machine.class_states().allocate_instance(
        machine.heap(), "java/lang/String");
    if (!object) return std::unexpected(object.error());
    auto attached = machine.heap().attach_string(*object, std::move(text));
    if (!attached) return std::unexpected(attached.error());
    return *object;
}

[[nodiscard]] Result<i32> int_field(Machine& machine,
                                    ObjectRef object,
                                    usize index);
[[nodiscard]] Status set_int_field(Machine& machine,
                                   ObjectRef object,
                                   usize index,
                                   i32 value);

struct LocalTimeFields final {
    i32 hour {0};
    i32 minute {0};
    i32 second {0};
    i32 nano {0};
};

[[nodiscard]] Status validate_local_time(LocalTimeFields fields) {
    if (fields.hour < 0 || fields.hour > 23 ||
        fields.minute < 0 || fields.minute > 59 ||
        fields.second < 0 || fields.second > 59 ||
        fields.nano < 0 || fields.nano > 999'999'999) {
        return fail_java("java/time/DateTimeException",
                         "LocalTime field is out of range");
    }
    return {};
}

[[nodiscard]] Result<ObjectRef> create_local_time(Machine& machine,
                                                  LocalTimeFields fields) {
    auto valid = validate_local_time(fields);
    if (!valid) return std::unexpected(valid.error());
    auto object = machine.class_states().allocate_instance(
        machine.heap(), "java/time/LocalTime");
    if (!object) return std::unexpected(object.error());
    auto hour = set_int_field(machine, *object, kLocalTimeHourField,
                              fields.hour);
    auto minute = set_int_field(machine, *object, kLocalTimeMinuteField,
                                fields.minute);
    auto second = set_int_field(machine, *object, kLocalTimeSecondField,
                                fields.second);
    auto nano = set_int_field(machine, *object, kLocalTimeNanoField,
                              fields.nano);
    if (!hour) return std::unexpected(hour.error());
    if (!minute) return std::unexpected(minute.error());
    if (!second) return std::unexpected(second.error());
    if (!nano) return std::unexpected(nano.error());
    return *object;
}

[[nodiscard]] Result<LocalTimeFields> local_time_fields(
    Machine& machine,
    ObjectRef object) {
    auto hour = int_field(machine, object, kLocalTimeHourField);
    auto minute = int_field(machine, object, kLocalTimeMinuteField);
    auto second = int_field(machine, object, kLocalTimeSecondField);
    auto nano = int_field(machine, object, kLocalTimeNanoField);
    if (!hour || !minute || !second || !nano) {
        return fail(ErrorCode::invalid_state,
                    "LocalTime state is invalid");
    }
    LocalTimeFields fields {*hour, *minute, *second, *nano};
    auto valid = validate_local_time(fields);
    if (!valid) return std::unexpected(valid.error());
    return fields;
}

[[nodiscard]] Result<LocalTimeFields> current_local_time() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm local {};
    if (localtime_r(&raw, &local) == nullptr) {
        return fail(ErrorCode::internal_error,
                    "cannot resolve the current local time");
    }
    const auto whole_seconds =
        std::chrono::floor<std::chrono::seconds>(now);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - whole_seconds).count();
    return LocalTimeFields {
        static_cast<i32>(local.tm_hour),
        static_cast<i32>(local.tm_min),
        static_cast<i32>(local.tm_sec),
        static_cast<i32>(nanos),
    };
}

[[nodiscard]] std::u16string ascii_text(std::string_view text) {
    std::u16string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(static_cast<char16_t>(
            static_cast<unsigned char>(character)));
    }
    return result;
}

[[nodiscard]] Result<std::string> ascii_string(Machine& machine,
                                               ObjectRef string) {
    if (string.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "timezone ID is null");
    }
    auto text = machine.heap().string_value(string);
    if (!text) return std::unexpected(text.error());
    std::string result;
    result.reserve(text->size());
    for (const char16_t character : *text) {
        if (character > 0x7FU) {
            return fail_java("java/lang/IllegalArgumentException",
                             "timezone ID must be ASCII");
        }
        result.push_back(static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] Result<i64> long_field(Machine& machine,
                                     ObjectRef object,
                                     usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_long();
}

[[nodiscard]] Status set_long_field(Machine& machine,
                                    ObjectRef object,
                                    usize index,
                                    i64 value) {
    return machine.heap().set_field(object, index, Value::from_long(value));
}

[[nodiscard]] Result<i32> int_field(Machine& machine,
                                    ObjectRef object,
                                    usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

[[nodiscard]] Status set_int_field(Machine& machine,
                                   ObjectRef object,
                                   usize index,
                                   i32 value) {
    return machine.heap().set_field(object, index, Value::from_int(value));
}

[[nodiscard]] Result<ObjectRef> reference_field(Machine& machine,
                                                ObjectRef object,
                                                usize index) {
    auto value = machine.heap().field(object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] Status set_reference_field(Machine& machine,
                                         ObjectRef object,
                                         usize index,
                                         ObjectRef value) {
    return machine.heap().set_field(object, index, Value::from_reference(value));
}

[[nodiscard]] Result<ObjectRef> create_timezone(Machine& machine,
                                                std::string_view id,
                                                i32 raw_offset) {
    auto zone = machine.class_states().allocate_instance(
        machine.heap(), "com/sun/cldc/util/j2me/TimeZoneImpl");
    if (!zone) return std::unexpected(zone.error());
    auto id_string = create_string(machine, ascii_text(id));
    if (!id_string) return std::unexpected(id_string.error());
    auto stored_id = set_reference_field(machine, *zone, kZoneIdField, *id_string);
    auto stored_offset =
        set_int_field(machine, *zone, kZoneRawOffsetField, raw_offset);
    if (!stored_id) return std::unexpected(stored_id.error());
    if (!stored_offset) return std::unexpected(stored_offset.error());
    return *zone;
}

[[nodiscard]] Result<std::pair<std::string, i32>> local_timezone() {
    const std::time_t now = std::time(nullptr);
    std::tm local {};
    if (localtime_r(&now, &local) == nullptr) {
        return fail(ErrorCode::internal_error,
                    "cannot resolve the default local timezone");
    }

#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__)
    const i64 offset_seconds = static_cast<i64>(local.tm_gmtoff);
#else
    std::tm utc {};
    if (gmtime_r(&now, &utc) == nullptr) {
        return fail(ErrorCode::internal_error,
                    "cannot resolve UTC for the default timezone");
    }
    utc.tm_isdst = local.tm_isdst;
    const std::time_t local_epoch = std::mktime(&local);
    const std::time_t utc_as_local_epoch = std::mktime(&utc);
    const i64 offset_seconds = static_cast<i64>(std::difftime(
        local_epoch, utc_as_local_epoch));
#endif

    const i64 offset_millis = offset_seconds * kMillisPerSecond;
    if (offset_millis < std::numeric_limits<i32>::min() ||
        offset_millis > std::numeric_limits<i32>::max()) {
        return fail(ErrorCode::overflow,
                    "default timezone offset exceeds int range");
    }

    const i64 signed_minutes = offset_millis / kMillisPerMinute;
    if (signed_minutes == 0) {
        return std::pair<std::string, i32>("GMT", 0);
    }
    const bool negative = signed_minutes < 0;
    const i64 absolute_minutes = negative ? -signed_minutes : signed_minutes;
    const i64 hours = absolute_minutes / 60;
    const i64 minutes = absolute_minutes % 60;
    if (hours > 99) {
        return fail(ErrorCode::overflow,
                    "default timezone hour offset is invalid");
    }
    std::string id = "GMT";
    id.push_back(negative ? '-' : '+');
    id.push_back(static_cast<char>('0' + (hours / 10)));
    id.push_back(static_cast<char>('0' + (hours % 10)));
    id.push_back(':');
    id.push_back(static_cast<char>('0' + (minutes / 10)));
    id.push_back(static_cast<char>('0' + (minutes % 10)));
    return std::pair<std::string, i32>(
        std::move(id), static_cast<i32>(offset_millis));
}

[[nodiscard]] Result<ObjectRef> default_timezone(Machine& machine) {
    auto field = machine.class_states().resolve_field(
        "java/util/TimeZone", "defaultZone", "Ljava/util/TimeZone;", true);
    if (!field) return std::unexpected(field.error());
    auto value = machine.class_states().static_field(*field);
    if (!value) return std::unexpected(value.error());
    auto current = value->as_reference();
    if (!current) return std::unexpected(current.error());
    if (!current->is_null()) return *current;
    auto local = local_timezone();
    if (!local) return std::unexpected(local.error());
    auto created = create_timezone(machine, local->first, local->second);
    if (!created) return std::unexpected(created.error());
    auto stored = machine.class_states().set_static_field(
        *field, Value::from_reference(*created));
    if (!stored) return std::unexpected(stored.error());
    return *created;
}

[[nodiscard]] Result<std::pair<std::string, i32>> parse_timezone_id(
    std::string id) {
    if (id == "UTC") return std::pair<std::string, i32>("UTC", 0);
    if (id == "GMT") return std::pair<std::string, i32>("GMT", 0);
    if (!id.starts_with("GMT") || id.size() < 6U ||
        (id[3] != '+' && id[3] != '-')) {
        return std::pair<std::string, i32>("GMT", 0);
    }
    std::string digits;
    for (usize index = 4U; index < id.size(); ++index) {
        if (id[index] == ':') continue;
        if (id[index] < '0' || id[index] > '9') {
            return std::pair<std::string, i32>("GMT", 0);
        }
        digits.push_back(id[index]);
    }
    if (digits.size() != 4U) {
        return std::pair<std::string, i32>("GMT", 0);
    }
    const i32 hours = (digits[0] - '0') * 10 + (digits[1] - '0');
    const i32 minutes = (digits[2] - '0') * 10 + (digits[3] - '0');
    if (hours > 23 || minutes > 59) {
        return std::pair<std::string, i32>("GMT", 0);
    }
    i32 offset = (hours * 60 + minutes) * 60 * 1000;
    if (id[3] == '-') offset = -offset;
    std::string canonical = "GMT";
    canonical.push_back(id[3]);
    canonical.push_back(digits[0]);
    canonical.push_back(digits[1]);
    canonical.push_back(':');
    canonical.push_back(digits[2]);
    canonical.push_back(digits[3]);
    return std::pair<std::string, i32>(std::move(canonical), offset);
}

[[nodiscard]] Result<i32> timezone_offset(Machine& machine,
                                          ObjectRef zone) {
    if (zone.is_null()) return 0;
    return int_field(machine, zone, kZoneRawOffsetField);
}

[[nodiscard]] Result<ObjectRef> calendar_zone(Machine& machine,
                                              ObjectRef calendar) {
    auto zone = reference_field(machine, calendar, kCalendarZoneField);
    if (!zone) return std::unexpected(zone.error());
    if (!zone->is_null()) return *zone;
    auto fallback = default_timezone(machine);
    if (!fallback) return std::unexpected(fallback.error());
    auto stored = set_reference_field(machine, calendar,
                                      kCalendarZoneField, *fallback);
    if (!stored) return std::unexpected(stored.error());
    return *fallback;
}

[[nodiscard]] Result<CivilFields> calendar_fields(Machine& machine,
                                                  ObjectRef calendar) {
    auto time = long_field(machine, calendar, kCalendarTimeField);
    if (!time) return std::unexpected(time.error());
    auto zone = calendar_zone(machine, calendar);
    if (!zone) return std::unexpected(zone.error());
    auto offset = timezone_offset(machine, *zone);
    if (!offset) return std::unexpected(offset.error());
    return fields_from_epoch(*time, *offset);
}

[[nodiscard]] Result<ObjectRef> create_calendar(Machine& machine,
                                                ObjectRef zone) {
    if (zone.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Calendar timezone is null");
    }
    auto calendar = machine.class_states().allocate_instance(
        machine.heap(), "java/util/GregorianCalendar");
    if (!calendar) return std::unexpected(calendar.error());
    auto stored_time = set_long_field(machine, *calendar,
                                      kCalendarTimeField, current_millis());
    auto stored_zone = set_reference_field(machine, *calendar,
                                           kCalendarZoneField, zone);
    if (!stored_time) return std::unexpected(stored_time.error());
    if (!stored_zone) return std::unexpected(stored_zone.error());
    return *calendar;
}

void append_two_digits(std::string& output, i32 value) {
    const i32 normalized = value < 0 ? -value : value;
    output.push_back(static_cast<char>('0' + (normalized / 10) % 10));
    output.push_back(static_cast<char>('0' + normalized % 10));
}

void append_integer(std::string& output, i32 value) {
    std::array<char, 32> buffer {};
    const auto converted = std::to_chars(buffer.data(),
                                         buffer.data() + buffer.size(),
                                         value);
    if (converted.ec == std::errc {}) {
        output.append(buffer.data(), converted.ptr);
    }
}

[[nodiscard]] Result<std::u16string> date_text(i64 millis) {
    auto fields = fields_from_epoch(millis, 0);
    if (!fields) return std::unexpected(fields.error());
    static constexpr std::array<std::string_view, 7> weekdays {{
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
    }};
    static constexpr std::array<std::string_view, 12> months {{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    }};
    std::string output;
    output.reserve(32U);
    output.append(weekdays[static_cast<usize>(fields->day_of_week - 1)]);
    output.push_back(' ');
    output.append(months[static_cast<usize>(fields->month)]);
    output.push_back(' ');
    append_two_digits(output, fields->day);
    output.push_back(' ');
    append_two_digits(output, fields->hour);
    output.push_back(':');
    append_two_digits(output, fields->minute);
    output.push_back(':');
    append_two_digits(output, fields->second);
    output.append(" GMT ");
    append_integer(output, fields->year);
    return ascii_text(output);
}

[[nodiscard]] std::string format_datetime_pattern(
    std::string_view pattern,
    const CivilFields& fields) {
    std::string output;
    output.reserve(pattern.size() + 8U);
    const auto append_four_digits = [&output](i32 value) {
        const i32 normalized = value < 0 ? -value : value;
        output.push_back(static_cast<char>('0' + (normalized / 1000) % 10));
        output.push_back(static_cast<char>('0' + (normalized / 100) % 10));
        output.push_back(static_cast<char>('0' + (normalized / 10) % 10));
        output.push_back(static_cast<char>('0' + normalized % 10));
    };
    for (usize index = 0U; index < pattern.size();) {
        if (pattern.substr(index, 4U) == "yyyy") {
            append_four_digits(fields.year);
            index += 4U;
        } else if (pattern.substr(index, 2U) == "MM") {
            append_two_digits(output, fields.month + 1);
            index += 2U;
        } else if (pattern.substr(index, 2U) == "dd") {
            append_two_digits(output, fields.day);
            index += 2U;
        } else if (pattern.substr(index, 2U) == "HH") {
            append_two_digits(output, fields.hour);
            index += 2U;
        } else if (pattern.substr(index, 2U) == "mm") {
            append_two_digits(output, fields.minute);
            index += 2U;
        } else if (pattern.substr(index, 2U) == "ss") {
            append_two_digits(output, fields.second);
            index += 2U;
        } else {
            output.push_back(pattern[index++]);
        }
    }
    return output;
}

[[nodiscard]] Result<bool> is_instance_of(Machine& machine,
                                          ObjectRef object,
                                          std::string_view type) {
    if (object.is_null()) return false;
    auto class_name = machine.heap().class_name(object);
    if (!class_name) return std::unexpected(class_name.error());
    return machine.classes().is_assignable(*class_name, type);
}

} // namespace

void register_time_natives(NativeMethodRegistry& registry) {
    add(registry, "java/time/Instant", "now", "()Ljava/time/Instant;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Instant.now expects no arguments");
            }
            auto instant = machine.class_states().allocate_instance(
                machine.heap(), "java/time/Instant");
            if (!instant) return std::unexpected(instant.error());
            auto stored = set_long_field(machine, *instant,
                                         kInstantEpochMilliField,
                                         current_millis());
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*instant));
        });
    add(registry, "java/time/Instant", "ofEpochMilli",
        "(J)Ljava/time/Instant;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Instant.ofEpochMilli argument is missing");
            }
            auto millis = arguments[0].as_long();
            if (!millis) return std::unexpected(millis.error());
            auto instant = machine.class_states().allocate_instance(
                machine.heap(), "java/time/Instant");
            if (!instant) return std::unexpected(instant.error());
            auto stored = set_long_field(
                machine, *instant, kInstantEpochMilliField, *millis);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*instant));
        });
    add(registry, "java/time/Instant", "toEpochMilli", "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto instant = receiver(arguments);
            if (!instant) return std::unexpected(instant.error());
            auto millis = long_field(machine, *instant, kInstantEpochMilliField);
            if (!millis) return std::unexpected(millis.error());
            return std::optional<Value>(Value::from_long(*millis));
        });

    add(registry, "java/time/ZoneId", "systemDefault",
        "()Ljava/time/ZoneId;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "ZoneId.systemDefault expects no arguments");
            }
            auto local = local_timezone();
            if (!local) return std::unexpected(local.error());
            auto zone = machine.class_states().allocate_instance(
                machine.heap(), "java/time/ZoneId");
            if (!zone) return std::unexpected(zone.error());
            auto zone_root = machine.pin_native_root(*zone);
            if (!zone_root) return std::unexpected(zone_root.error());
            auto id = create_string(machine, ascii_text(local->first));
            if (!id) return std::unexpected(id.error());
            auto stored_id = set_reference_field(
                machine, *zone, kJavaTimeZoneIdField, *id);
            auto stored_offset = set_int_field(
                machine, *zone, kJavaTimeZoneRawOffsetField, local->second);
            if (!stored_id) return std::unexpected(stored_id.error());
            if (!stored_offset) return std::unexpected(stored_offset.error());
            return std::optional<Value>(Value::from_reference(*zone));
        });
    const auto zone_id_text = [](Machine& machine,
                                 std::span<const Value> arguments)
        -> Result<std::optional<Value>> {
        auto zone = receiver(arguments);
        if (!zone) return std::unexpected(zone.error());
        auto id = reference_field(machine, *zone, kJavaTimeZoneIdField);
        if (!id) return std::unexpected(id.error());
        return std::optional<Value>(Value::from_reference(*id));
    };
    add(registry, "java/time/ZoneId", "getId", "()Ljava/lang/String;",
        zone_id_text);
    add(registry, "java/time/ZoneId", "toString", "()Ljava/lang/String;",
        zone_id_text);

    add(registry, "java/time/format/DateTimeFormatter", "ofPattern",
        "(Ljava/lang/String;)Ljava/time/format/DateTimeFormatter;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "DateTimeFormatter.ofPattern pattern is missing");
            }
            auto pattern = arguments[0].as_reference();
            if (!pattern || pattern->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "DateTimeFormatter pattern is null");
            }
            auto formatter = machine.class_states().allocate_instance(
                machine.heap(), "java/time/format/DateTimeFormatter");
            if (!formatter) return std::unexpected(formatter.error());
            auto stored_pattern = set_reference_field(
                machine, *formatter, kFormatterPatternField, *pattern);
            auto stored_zone = set_reference_field(
                machine, *formatter, kFormatterZoneField, {});
            if (!stored_pattern) return std::unexpected(stored_pattern.error());
            if (!stored_zone) return std::unexpected(stored_zone.error());
            return std::optional<Value>(Value::from_reference(*formatter));
        });
    add(registry, "java/time/format/DateTimeFormatter", "withZone",
        "(Ljava/time/ZoneId;)Ljava/time/format/DateTimeFormatter;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto source = receiver(arguments);
            if (!source) return std::unexpected(source.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "DateTimeFormatter.withZone zone is missing");
            }
            auto zone = arguments[1].as_reference();
            if (!zone || zone->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "DateTimeFormatter zone is null");
            }
            auto pattern = reference_field(
                machine, *source, kFormatterPatternField);
            if (!pattern) return std::unexpected(pattern.error());
            auto formatter = machine.class_states().allocate_instance(
                machine.heap(), "java/time/format/DateTimeFormatter");
            if (!formatter) return std::unexpected(formatter.error());
            auto stored_pattern = set_reference_field(
                machine, *formatter, kFormatterPatternField, *pattern);
            auto stored_zone = set_reference_field(
                machine, *formatter, kFormatterZoneField, *zone);
            if (!stored_pattern) return std::unexpected(stored_pattern.error());
            if (!stored_zone) return std::unexpected(stored_zone.error());
            return std::optional<Value>(Value::from_reference(*formatter));
        });
    add(registry, "java/time/format/DateTimeFormatter", "format",
        "(Ljava/time/temporal/TemporalAccessor;)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto formatter = receiver(arguments);
            if (!formatter) return std::unexpected(formatter.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "DateTimeFormatter.format temporal is missing");
            }
            auto temporal = arguments[1].as_reference();
            if (!temporal || temporal->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "DateTimeFormatter temporal is null");
            }
            auto instant = is_instance_of(machine, *temporal, "java/time/Instant");
            if (!instant) return std::unexpected(instant.error());
            if (!*instant) {
                return fail_java("java/time/DateTimeException",
                                 "unsupported TemporalAccessor");
            }
            auto millis = long_field(
                machine, *temporal, kInstantEpochMilliField);
            if (!millis) return std::unexpected(millis.error());
            i32 raw_offset = 0;
            auto zone = reference_field(machine, *formatter, kFormatterZoneField);
            if (!zone) return std::unexpected(zone.error());
            if (!zone->is_null()) {
                auto offset = int_field(
                    machine, *zone, kJavaTimeZoneRawOffsetField);
                if (!offset) return std::unexpected(offset.error());
                raw_offset = *offset;
            }
            auto pattern_ref = reference_field(
                machine, *formatter, kFormatterPatternField);
            if (!pattern_ref || pattern_ref->is_null()) {
                return fail_java("java/time/DateTimeException",
                                 "formatter pattern is unavailable");
            }
            auto pattern = ascii_string(machine, *pattern_ref);
            if (!pattern) return std::unexpected(pattern.error());
            auto fields = fields_from_epoch(*millis, raw_offset);
            if (!fields) return std::unexpected(fields.error());
            auto text = format_datetime_pattern(*pattern, *fields);
            auto result = create_string(machine, ascii_text(text));
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });

    add(registry, "java/time/LocalTime", "now",
        "()Ljava/time/LocalTime;",
        [](Machine& machine, std::span<const Value>)
            -> Result<std::optional<Value>> {
            auto fields = current_local_time();
            if (!fields) return std::unexpected(fields.error());
            auto object = create_local_time(machine, *fields);
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });
    add(registry, "java/time/LocalTime", "of",
        "(II)Ljava/time/LocalTime;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "LocalTime.of arguments are missing");
            }
            auto hour = arguments[0].as_int();
            auto minute = arguments[1].as_int();
            if (!hour) return std::unexpected(hour.error());
            if (!minute) return std::unexpected(minute.error());
            auto object = create_local_time(
                machine, LocalTimeFields {*hour, *minute, 0, 0});
            if (!object) return std::unexpected(object.error());
            return std::optional<Value>(Value::from_reference(*object));
        });
    add(registry, "java/time/LocalTime", "withSecond",
        "(I)Ljava/time/LocalTime;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "LocalTime.withSecond argument is missing");
            }
            auto second = arguments[1].as_int();
            if (!second) return std::unexpected(second.error());
            auto fields = local_time_fields(machine, *object);
            if (!fields) return std::unexpected(fields.error());
            fields->second = *second;
            auto result = create_local_time(machine, *fields);
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/time/LocalTime", "withNano",
        "(I)Ljava/time/LocalTime;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "LocalTime.withNano argument is missing");
            }
            auto nano = arguments[1].as_int();
            if (!nano) return std::unexpected(nano.error());
            auto fields = local_time_fields(machine, *object);
            if (!fields) return std::unexpected(fields.error());
            fields->nano = *nano;
            auto result = create_local_time(machine, *fields);
            if (!result) return std::unexpected(result.error());
            return std::optional<Value>(Value::from_reference(*result));
        });
    add(registry, "java/time/LocalTime", "equals",
        "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto left = receiver(arguments);
            if (!left) return std::unexpected(left.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "LocalTime.equals argument is missing");
            }
            auto right = arguments[1].as_reference();
            if (!right) return std::unexpected(right.error());
            if (right->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto compatible = is_instance_of(
                machine, *right, "java/time/LocalTime");
            if (!compatible) return std::unexpected(compatible.error());
            if (!*compatible) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto left_fields = local_time_fields(machine, *left);
            auto right_fields = local_time_fields(machine, *right);
            if (!left_fields) return std::unexpected(left_fields.error());
            if (!right_fields) return std::unexpected(right_fields.error());
            const bool equal =
                left_fields->hour == right_fields->hour &&
                left_fields->minute == right_fields->minute &&
                left_fields->second == right_fields->second &&
                left_fields->nano == right_fields->nano;
            return std::optional<Value>(Value::from_int(equal ? 1 : 0));
        });
    add(registry, "java/time/LocalTime", "hashCode", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto fields = local_time_fields(machine, *object);
            if (!fields) return std::unexpected(fields.error());
            const i64 seconds =
                static_cast<i64>(fields->hour) * 3'600LL +
                static_cast<i64>(fields->minute) * 60LL +
                static_cast<i64>(fields->second);
            const i64 nanos = seconds * 1'000'000'000LL + fields->nano;
            const u64 bits = static_cast<u64>(nanos);
            const i32 hash = static_cast<i32>(
                static_cast<u32>(bits ^ (bits >> 32U)));
            return std::optional<Value>(Value::from_int(hash));
        });
    add(registry, "java/time/LocalTime", "toString",
        "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto fields = local_time_fields(machine, *object);
            if (!fields) return std::unexpected(fields.error());
            std::string text;
            text.reserve(18U);
            append_two_digits(text, fields->hour);
            text.push_back(':');
            append_two_digits(text, fields->minute);
            if (fields->second != 0 || fields->nano != 0) {
                text.push_back(':');
                append_two_digits(text, fields->second);
            }
            if (fields->nano != 0) {
                text.push_back('.');
                std::array<char, 9> digits {};
                i32 divisor = 100'000'000;
                i32 value = fields->nano;
                for (char& digit : digits) {
                    digit = static_cast<char>('0' + value / divisor);
                    value %= divisor;
                    divisor /= 10;
                }
                usize used = digits.size();
                while (used > 1U && digits[used - 1U] == '0') --used;
                text.append(digits.data(), used);
            }
            auto string = create_string(machine, ascii_text(text));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });

    add(registry, "java/text/SimpleDateFormat", "<init>",
        "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto formatter = receiver(arguments);
            if (!formatter) return std::unexpected(formatter.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "SimpleDateFormat pattern is missing");
            }
            auto pattern = arguments[1].as_reference();
            if (!pattern || pattern->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "SimpleDateFormat pattern is null");
            }
            auto stored = set_reference_field(
                machine, *formatter, kSimpleDateFormatPatternField, *pattern);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/text/SimpleDateFormat", "<init>",
        "(Ljava/lang/String;Ljava/util/Locale;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto formatter = receiver(arguments);
            if (!formatter) return std::unexpected(formatter.error());
            if (arguments.size() < 3U) {
                return fail(ErrorCode::invalid_argument,
                            "SimpleDateFormat pattern/locale is missing");
            }
            auto pattern = arguments[1].as_reference();
            auto locale = arguments[2].as_reference();
            if (!pattern || pattern->is_null() || !locale || locale->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "SimpleDateFormat pattern/locale is null");
            }
            auto stored = set_reference_field(
                machine, *formatter, kSimpleDateFormatPatternField, *pattern);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/text/NumberFormat", "<init>", "()V",
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            return std::optional<Value> {};
        });
    add(registry, "java/text/NumberFormat", "getInstance",
        "(Ljava/util/Locale;)Ljava/text/NumberFormat;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 1U) {
                return fail(ErrorCode::invalid_argument,
                            "NumberFormat.getInstance expects one locale");
            }
            auto locale = arguments[0].as_reference();
            if (!locale || locale->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "NumberFormat locale is null");
            }
            auto formatter = machine.class_states().allocate_instance(
                machine.heap(), "java/text/NumberFormat");
            if (!formatter) return std::unexpected(formatter.error());
            return std::optional<Value>(Value::from_reference(*formatter));
        });
    add(registry, "java/text/NumberFormat", "format", "(J)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "NumberFormat.format expects one long");
            }
            auto value = arguments[1].as_long();
            if (!value) return std::unexpected(value.error());
            std::array<char, 32> buffer {};
            auto [end, error] = std::to_chars(
                buffer.data(), buffer.data() + buffer.size(), *value);
            if (error != std::errc {}) {
                return fail(ErrorCode::invalid_state,
                            "NumberFormat failed to format long");
            }
            auto text = create_string(
                machine, ascii_text(std::string_view(buffer.data(),
                                                     static_cast<usize>(end - buffer.data()))));
            if (!text) return std::unexpected(text.error());
            return std::optional<Value>(Value::from_reference(*text));
        });
    add(registry, "java/text/SimpleDateFormat", "format",
        "(Ljava/util/Date;)Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto formatter = receiver(arguments);
            if (!formatter) return std::unexpected(formatter.error());
            if (arguments.size() < 2U) {
                return fail(ErrorCode::invalid_argument,
                            "SimpleDateFormat date is missing");
            }
            auto date = arguments[1].as_reference();
            if (!date || date->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "SimpleDateFormat date is null");
            }
            auto pattern_ref = reference_field(
                machine, *formatter, kSimpleDateFormatPatternField);
            if (!pattern_ref || pattern_ref->is_null()) {
                return fail(ErrorCode::invalid_state,
                            "SimpleDateFormat has no pattern");
            }
            auto pattern = ascii_string(machine, *pattern_ref);
            auto millis = long_field(machine, *date, kDateTimeField);
            auto timezone = local_timezone();
            if (!pattern) return std::unexpected(pattern.error());
            if (!millis) return std::unexpected(millis.error());
            if (!timezone) return std::unexpected(timezone.error());
            auto fields = fields_from_epoch(*millis, timezone->second);
            if (!fields) return std::unexpected(fields.error());
            auto text = create_string(
                machine, ascii_text(format_datetime_pattern(*pattern, *fields)));
            if (!text) return std::unexpected(text.error());
            return std::optional<Value>(Value::from_reference(*text));
        });

    add(registry, "java/util/Date", "<init>", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto stored = set_long_field(machine, *object, kDateTimeField,
                                         current_millis());
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Date", "<init>", "(J)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = arguments[1].as_long();
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto stored = set_long_field(machine, *object, kDateTimeField, *value);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Date", "from",
        "(Ljava/time/Instant;)Ljava/util/Date;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Date.from argument is missing");
            }
            auto instant = arguments[0].as_reference();
            if (!instant || instant->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Date.from instant is null");
            }
            auto compatible = is_instance_of(machine, *instant,
                                             "java/time/Instant");
            if (!compatible) return std::unexpected(compatible.error());
            if (!*compatible) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "Date.from requires an Instant");
            }
            auto millis = long_field(machine, *instant,
                                     kInstantEpochMilliField);
            if (!millis) return std::unexpected(millis.error());
            auto date = machine.class_states().allocate_instance(
                machine.heap(), "java/util/Date");
            if (!date) return std::unexpected(date.error());
            auto stored = set_long_field(machine, *date, kDateTimeField, *millis);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*date));
        });
    add(registry, "java/util/Date", "getTime", "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = long_field(machine, *object, kDateTimeField);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_long(*value));
        });
    add(registry, "java/util/Date", "setTime", "(J)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            auto value = arguments[1].as_long();
            if (!object) return std::unexpected(object.error());
            if (!value) return std::unexpected(value.error());
            auto stored = set_long_field(machine, *object, kDateTimeField, *value);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    const auto compare_dates = [](Machine& machine,
                                  std::span<const Value> arguments)
        -> Result<std::pair<i64, i64>> {
        auto left = receiver(arguments);
        auto right = arguments[1].as_reference();
        if (!left) return std::unexpected(left.error());
        if (!right || right->is_null()) {
            return fail_java("java/lang/NullPointerException",
                             "Date comparison target is null");
        }
        auto compatible = is_instance_of(machine, *right, "java/util/Date");
        if (!compatible) return std::unexpected(compatible.error());
        if (!*compatible) {
            return fail_java("java/lang/ClassCastException",
                             "Date comparison target is not a Date");
        }
        auto left_time = long_field(machine, *left, kDateTimeField);
        auto right_time = long_field(machine, *right, kDateTimeField);
        if (!left_time) return std::unexpected(left_time.error());
        if (!right_time) return std::unexpected(right_time.error());
        return std::pair<i64, i64>(*left_time, *right_time);
    };
    add(registry, "java/util/Date", "before", "(Ljava/util/Date;)Z",
        [compare_dates](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto values = compare_dates(machine, arguments);
            if (!values) return std::unexpected(values.error());
            return std::optional<Value>(Value::from_int(
                values->first < values->second ? 1 : 0));
        });
    add(registry, "java/util/Date", "after", "(Ljava/util/Date;)Z",
        [compare_dates](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto values = compare_dates(machine, arguments);
            if (!values) return std::unexpected(values.error());
            return std::optional<Value>(Value::from_int(
                values->first > values->second ? 1 : 0));
        });
    const NativeMethod date_compare =
        [compare_dates](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto values = compare_dates(machine, arguments);
            if (!values) return std::unexpected(values.error());
            const i32 comparison = values->first < values->second
                ? -1
                : (values->first > values->second ? 1 : 0);
            return std::optional<Value>(Value::from_int(comparison));
        };
    add(registry, "java/util/Date", "compareTo", "(Ljava/util/Date;)I",
        date_compare);
    add(registry, "java/util/Date", "compareTo", "(Ljava/lang/Object;)I",
        date_compare);
    add(registry, "java/util/Date", "equals", "(Ljava/lang/Object;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto left = receiver(arguments);
            auto right = arguments[1].as_reference();
            if (!left) return std::unexpected(left.error());
            if (!right || right->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto compatible = is_instance_of(machine, *right, "java/util/Date");
            if (!compatible) return std::unexpected(compatible.error());
            if (!*compatible) return std::optional<Value>(Value::from_int(0));
            auto left_time = long_field(machine, *left, kDateTimeField);
            auto right_time = long_field(machine, *right, kDateTimeField);
            if (!left_time) return std::unexpected(left_time.error());
            if (!right_time) return std::unexpected(right_time.error());
            return std::optional<Value>(Value::from_int(
                *left_time == *right_time ? 1 : 0));
        });
    add(registry, "java/util/Date", "hashCode", "()I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = long_field(machine, *object, kDateTimeField);
            if (!value) return std::unexpected(value.error());
            const u64 bits = static_cast<u64>(*value);
            return std::optional<Value>(Value::from_int(static_cast<i32>(
                static_cast<u32>(bits ^ (bits >> 32U)))));
        });
    add(registry, "java/util/Date", "toString", "()Ljava/lang/String;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto object = receiver(arguments);
            if (!object) return std::unexpected(object.error());
            auto value = long_field(machine, *object, kDateTimeField);
            if (!value) return std::unexpected(value.error());
            auto text = date_text(*value);
            if (!text) return std::unexpected(text.error());
            auto string = create_string(machine, std::move(*text));
            if (!string) return std::unexpected(string.error());
            return std::optional<Value>(Value::from_reference(*string));
        });

    const NativeMethod timezone_constructor =
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto zone = receiver(arguments);
            if (!zone) return std::unexpected(zone.error());
            auto id = create_string(machine, std::u16string(u"GMT"));
            if (!id) return std::unexpected(id.error());
            auto stored_id = set_reference_field(machine, *zone, kZoneIdField, *id);
            auto stored_offset = set_int_field(machine, *zone,
                                               kZoneRawOffsetField, 0);
            if (!stored_id) return std::unexpected(stored_id.error());
            if (!stored_offset) return std::unexpected(stored_offset.error());
            return std::optional<Value> {};
        };
    const NativeMethod timezone_get_offset =
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() != 7U) {
                return fail(ErrorCode::invalid_argument,
                            "TimeZone.getOffset expects six fields");
            }
            auto zone = receiver(arguments);
            if (!zone) return std::unexpected(zone.error());
            std::array<i32, 6> fields {};
            for (usize index = 0U; index < fields.size(); ++index) {
                auto parsed = arguments[index + 1U].as_int();
                if (!parsed) return std::unexpected(parsed.error());
                fields[index] = *parsed;
            }
            static constexpr std::array<i32, 12> month_lengths {{
                31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
            }};
            const i32 era = fields[0];
            const i32 month = fields[2];
            const i32 day = fields[3];
            const i32 day_of_week = fields[4];
            const i32 millis = fields[5];
            if ((era != 0 && era != 1) || month < 0 || month >= 12 ||
                day < 1 ||
                (month >= 0 && month < 12 &&
                 day > month_lengths[static_cast<usize>(month)]) ||
                day_of_week < 1 || day_of_week > 7 || millis < 0 ||
                static_cast<i64>(millis) >= kMillisPerDay) {
                return fail_java("java/lang/IllegalArgumentException",
                                 "TimeZone date fields are out of range");
            }
            auto offset = timezone_offset(machine, *zone);
            if (!offset) return std::unexpected(offset.error());
            return std::optional<Value>(Value::from_int(*offset));
        };
    const NativeMethod timezone_get_raw_offset =
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto zone = receiver(arguments);
            if (!zone) return std::unexpected(zone.error());
            auto offset = timezone_offset(machine, *zone);
            if (!offset) return std::unexpected(offset.error());
            return std::optional<Value>(Value::from_int(*offset));
        };
    const NativeMethod timezone_use_daylight =
        [](Machine&, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto zone = receiver(arguments);
            if (!zone) return std::unexpected(zone.error());
            return std::optional<Value>(Value::from_int(0));
        };
    const NativeMethod timezone_get_id =
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto zone = receiver(arguments);
            if (!zone) return std::unexpected(zone.error());
            auto id = reference_field(machine, *zone, kZoneIdField);
            if (!id) return std::unexpected(id.error());
            return std::optional<Value>(Value::from_reference(*id));
        };

    add(registry, "java/util/TimeZone", "<init>", "()V",
        timezone_constructor);
    add(registry, "java/util/TimeZone", "getOffset", "(IIIIII)I",
        timezone_get_offset);
    add(registry, "java/util/TimeZone", "getRawOffset", "()I",
        timezone_get_raw_offset);
    add(registry, "java/util/TimeZone", "setRawOffset", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto zone = receiver(arguments);
            auto offset = arguments[1].as_int();
            if (!zone) return std::unexpected(zone.error());
            if (!offset) return std::unexpected(offset.error());
            auto stored = set_int_field(machine, *zone, kZoneRawOffsetField,
                                        *offset);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/TimeZone", "useDaylightTime", "()Z",
        timezone_use_daylight);
    add(registry, "java/util/TimeZone", "getID", "()Ljava/lang/String;",
        timezone_get_id);
    add(registry, "com/sun/cldc/util/j2me/TimeZoneImpl", "<init>", "()V",
        timezone_constructor);
    add(registry, "com/sun/cldc/util/j2me/TimeZoneImpl", "getOffset",
        "(IIIIII)I", timezone_get_offset);
    add(registry, "com/sun/cldc/util/j2me/TimeZoneImpl", "getRawOffset", "()I",
        timezone_get_raw_offset);
    add(registry, "com/sun/cldc/util/j2me/TimeZoneImpl", "useDaylightTime",
        "()Z", timezone_use_daylight);
    add(registry, "com/sun/cldc/util/j2me/TimeZoneImpl", "getID",
        "()Ljava/lang/String;", timezone_get_id);
    add(registry, "java/util/TimeZone", "setID", "(Ljava/lang/String;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto zone = receiver(arguments);
            auto id = arguments[1].as_reference();
            if (!zone) return std::unexpected(zone.error());
            if (!id || id->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "TimeZone ID is null");
            }
            auto stored = set_reference_field(machine, *zone, kZoneIdField, *id);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/TimeZone", "hasSameRules",
        "(Ljava/util/TimeZone;)Z",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto zone = receiver(arguments);
            auto other = arguments[1].as_reference();
            if (!zone) return std::unexpected(zone.error());
            if (!other || other->is_null()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto left = timezone_offset(machine, *zone);
            auto right = timezone_offset(machine, *other);
            if (!left) return std::unexpected(left.error());
            if (!right) return std::unexpected(right.error());
            return std::optional<Value>(Value::from_int(
                *left == *right ? 1 : 0));
        });
    add(registry, "java/util/TimeZone", "getTimeZone",
        "(Ljava/lang/String;)Ljava/util/TimeZone;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto id_reference = arguments[0].as_reference();
            if (!id_reference || id_reference->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "TimeZone.getTimeZone ID is null");
            }
            auto id = ascii_string(machine, *id_reference);
            if (!id) return std::unexpected(id.error());
            auto parsed = parse_timezone_id(std::move(*id));
            if (!parsed) return std::unexpected(parsed.error());
            auto zone = create_timezone(machine, parsed->first, parsed->second);
            if (!zone) return std::unexpected(zone.error());
            return std::optional<Value>(Value::from_reference(*zone));
        });
    add(registry, "java/util/TimeZone", "getDefault",
        "()Ljava/util/TimeZone;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "TimeZone.getDefault expects no arguments");
            }
            auto zone = default_timezone(machine);
            if (!zone) return std::unexpected(zone.error());
            return std::optional<Value>(Value::from_reference(*zone));
        });
    add(registry, "java/util/TimeZone", "setDefault",
        "(Ljava/util/TimeZone;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto zone = arguments[0].as_reference();
            if (!zone) return std::unexpected(zone.error());
            auto field = machine.class_states().resolve_field(
                "java/util/TimeZone", "defaultZone", "Ljava/util/TimeZone;", true);
            if (!field) return std::unexpected(field.error());
            auto stored = machine.class_states().set_static_field(
                *field, Value::from_reference(*zone));
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    const NativeMethod timezone_get_ids =
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (arguments.size() > 1U) {
                return fail(ErrorCode::invalid_argument,
                            "TimeZone ID enumeration has invalid arguments");
            }
            if (arguments.size() == 1U) {
                auto impl = receiver(arguments);
                if (!impl) return std::unexpected(impl.error());
            }
            auto array = machine.heap().allocate_array(
                "[Ljava/lang/String;", 2U, Value::from_reference({}));
            if (!array) return std::unexpected(array.error());
            auto gmt = create_string(machine, std::u16string(u"GMT"));
            auto utc = create_string(machine, std::u16string(u"UTC"));
            if (!gmt) return std::unexpected(gmt.error());
            if (!utc) return std::unexpected(utc.error());
            auto first = machine.heap().set_element(
                *array, 0U, Value::from_reference(*gmt));
            auto second = machine.heap().set_element(
                *array, 1U, Value::from_reference(*utc));
            if (!first) return std::unexpected(first.error());
            if (!second) return std::unexpected(second.error());
            return std::optional<Value>(Value::from_reference(*array));
        };
    add(registry, "java/util/TimeZone", "getAvailableIDs",
        "()[Ljava/lang/String;", timezone_get_ids);
    add(registry, "com/sun/cldc/util/j2me/TimeZoneImpl", "getIDs",
        "()[Ljava/lang/String;", timezone_get_ids);
    add(registry, "com/sun/cldc/util/j2me/TimeZoneImpl", "getInstance",
        "(Ljava/lang/String;)Ljava/util/TimeZone;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto impl = receiver(arguments);
            if (!impl) return std::unexpected(impl.error());
            if (arguments.size() != 2U) {
                return fail(ErrorCode::invalid_argument,
                            "TimeZoneImpl.getInstance expects one ID");
            }
            auto id_reference = arguments[1].as_reference();
            if (!id_reference) return std::unexpected(id_reference.error());
            std::string id = "GMT";
            if (!id_reference->is_null()) {
                auto parsed_id = ascii_string(machine, *id_reference);
                if (!parsed_id) return std::unexpected(parsed_id.error());
                id = std::move(*parsed_id);
            }
            auto parsed = parse_timezone_id(std::move(id));
            if (!parsed) return std::unexpected(parsed.error());
            auto zone = create_timezone(machine, parsed->first, parsed->second);
            if (!zone) return std::unexpected(zone.error());
            return std::optional<Value>(Value::from_reference(*zone));
        });

    if constexpr (false) {
    const NativeMethod calendar_constructor =
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            if (!calendar) return std::unexpected(calendar.error());
            ObjectRef zone;
            if (arguments.size() == 2U) {
                auto supplied = arguments[1].as_reference();
                if (!supplied || supplied->is_null()) {
                    return fail_java("java/lang/NullPointerException",
                                     "Calendar timezone is null");
                }
                zone = *supplied;
            } else {
                auto fallback = default_timezone(machine);
                if (!fallback) return std::unexpected(fallback.error());
                zone = *fallback;
            }
            auto fields = machine.heap().allocate_array(
                "[I", 15U, Value::from_int(0));
            auto is_set = machine.heap().allocate_array(
                "[Z", 15U, Value::from_int(0));
            if (!fields) return std::unexpected(fields.error());
            if (!is_set) return std::unexpected(is_set.error());
            auto stored_time = set_long_field(machine, *calendar,
                                              kCalendarTimeField,
                                              current_millis());
            auto stored_zone = set_reference_field(machine, *calendar,
                                                   kCalendarZoneField, zone);
            auto stored_fields = set_reference_field(
                machine, *calendar, kCalendarFieldsField, *fields);
            auto stored_is_set = set_reference_field(
                machine, *calendar, kCalendarIsSetField, *is_set);
            if (!stored_time) return std::unexpected(stored_time.error());
            if (!stored_zone) return std::unexpected(stored_zone.error());
            if (!stored_fields) return std::unexpected(stored_fields.error());
            if (!stored_is_set) return std::unexpected(stored_is_set.error());
            return std::optional<Value> {};
        };
    add(registry, "java/util/Calendar", "<init>", "()V",
        calendar_constructor);
    add(registry, "java/util/GregorianCalendar", "<init>", "()V",
        calendar_constructor);
    add(registry, "java/util/GregorianCalendar", "<init>",
        "(Ljava/util/TimeZone;)V", calendar_constructor);
    for (const char* name : {"computeFields", "computeTime"}) {
        add(registry, "java/util/GregorianCalendar", name, "()V",
            [](Machine&, std::span<const Value> arguments)
                -> Result<std::optional<Value>> {
                auto calendar = receiver(arguments);
                if (!calendar) return std::unexpected(calendar.error());
                // Calendar's public operations in this runtime already keep
                // the millisecond representation synchronized directly.
                return std::optional<Value> {};
            });
    }

    add(registry, "java/util/Calendar", "getInstance",
        "()Ljava/util/Calendar;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Calendar.getInstance expects no arguments");
            }
            auto zone = default_timezone(machine);
            if (!zone) return std::unexpected(zone.error());
            auto calendar = create_calendar(machine, *zone);
            if (!calendar) return std::unexpected(calendar.error());
            return std::optional<Value>(Value::from_reference(*calendar));
        });
    add(registry, "java/util/Calendar", "getInstance",
        "(Ljava/util/TimeZone;)Ljava/util/Calendar;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto zone = arguments[0].as_reference();
            if (!zone || zone->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Calendar timezone is null");
            }
            auto calendar = create_calendar(machine, *zone);
            if (!calendar) return std::unexpected(calendar.error());
            return std::optional<Value>(Value::from_reference(*calendar));
        });
    add(registry, "java/util/Calendar", "getTime",
        "()Ljava/util/Date;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            if (!calendar) return std::unexpected(calendar.error());
            auto time = long_field(machine, *calendar, kCalendarTimeField);
            if (!time) return std::unexpected(time.error());
            auto date = machine.class_states().allocate_instance(
                machine.heap(), "java/util/Date");
            if (!date) return std::unexpected(date.error());
            auto stored = set_long_field(machine, *date, kDateTimeField, *time);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*date));
        });
    add(registry, "java/util/Calendar", "setTime",
        "(Ljava/util/Date;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            auto date = arguments[1].as_reference();
            if (!calendar) return std::unexpected(calendar.error());
            if (!date || date->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Calendar date is null");
            }
            auto time = long_field(machine, *date, kDateTimeField);
            if (!time) return std::unexpected(time.error());
            auto stored = set_long_field(machine, *calendar,
                                         kCalendarTimeField, *time);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Calendar", "getTimeInMillis", "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            if (!calendar) return std::unexpected(calendar.error());
            auto time = long_field(machine, *calendar, kCalendarTimeField);
            if (!time) return std::unexpected(time.error());
            return std::optional<Value>(Value::from_long(*time));
        });
    add(registry, "java/util/Calendar", "setTimeInMillis", "(J)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            auto time = arguments[1].as_long();
            if (!calendar) return std::unexpected(calendar.error());
            if (!time) return std::unexpected(time.error());
            auto stored = set_long_field(machine, *calendar,
                                         kCalendarTimeField, *time);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Calendar", "get", "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            auto field = arguments[1].as_int();
            if (!calendar) return std::unexpected(calendar.error());
            if (!field) return std::unexpected(field.error());
            auto values = calendar_fields(machine, *calendar);
            if (!values) return std::unexpected(values.error());
            i32 result = 0;
            switch (*field) {
            case kYear: result = values->year; break;
            case kMonth: result = values->month; break;
            case kDate: result = values->day; break;
            case kDayOfWeek: result = values->day_of_week; break;
            case kAmPm: result = values->hour >= 12 ? 1 : 0; break;
            case kHour: result = values->hour % 12; break;
            case kHourOfDay: result = values->hour; break;
            case kMinute: result = values->minute; break;
            case kSecond: result = values->second; break;
            case kMillisecond: result = values->millisecond; break;
            default:
                return fail_java("java/lang/IllegalArgumentException",
                                 "unsupported Calendar field");
            }
            return std::optional<Value>(Value::from_int(result));
        });
    add(registry, "java/util/Calendar", "set", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            auto field = arguments[1].as_int();
            auto value = arguments[2].as_int();
            if (!calendar) return std::unexpected(calendar.error());
            if (!field) return std::unexpected(field.error());
            if (!value) return std::unexpected(value.error());
            auto values = calendar_fields(machine, *calendar);
            if (!values) return std::unexpected(values.error());
            switch (*field) {
            case kYear: values->year = *value; break;
            case kMonth: values->month = *value; break;
            case kDate: values->day = *value; break;
            case kDayOfWeek:
                values->day += *value - values->day_of_week;
                break;
            case kAmPm:
                values->hour = values->hour % 12 + (*value == 0 ? 0 : 12);
                break;
            case kHour:
                values->hour = values->hour >= 12
                    ? 12 + static_cast<i32>(floor_mod(*value, 12))
                    : static_cast<i32>(floor_mod(*value, 12));
                break;
            case kHourOfDay: values->hour = *value; break;
            case kMinute: values->minute = *value; break;
            case kSecond: values->second = *value; break;
            case kMillisecond: values->millisecond = *value; break;
            default:
                return fail_java("java/lang/IllegalArgumentException",
                                 "unsupported Calendar field");
            }
            auto zone = calendar_zone(machine, *calendar);
            if (!zone) return std::unexpected(zone.error());
            auto offset = timezone_offset(machine, *zone);
            if (!offset) return std::unexpected(offset.error());
            auto time = epoch_from_fields(*values, *offset);
            if (!time) return std::unexpected(time.error());
            auto stored = set_long_field(machine, *calendar,
                                         kCalendarTimeField, *time);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Calendar", "clear", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            if (!calendar) return std::unexpected(calendar.error());
            auto stored = set_long_field(machine, *calendar,
                                         kCalendarTimeField, 0);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Calendar", "clear", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            auto field = arguments[1].as_int();
            if (!calendar) return std::unexpected(calendar.error());
            if (!field) return std::unexpected(field.error());
            auto values = calendar_fields(machine, *calendar);
            if (!values) return std::unexpected(values.error());
            switch (*field) {
            case kYear: values->year = 1970; break;
            case kMonth: values->month = 0; break;
            case kDate: values->day = 1; break;
            case kAmPm:
            case kHour:
            case kHourOfDay: values->hour = 0; break;
            case kMinute: values->minute = 0; break;
            case kSecond: values->second = 0; break;
            case kMillisecond: values->millisecond = 0; break;
            case kDayOfWeek: break;
            default:
                return fail_java("java/lang/IllegalArgumentException",
                                 "unsupported Calendar field");
            }
            auto zone = calendar_zone(machine, *calendar);
            if (!zone) return std::unexpected(zone.error());
            auto offset = timezone_offset(machine, *zone);
            if (!offset) return std::unexpected(offset.error());
            auto time = epoch_from_fields(*values, *offset);
            if (!time) return std::unexpected(time.error());
            auto stored = set_long_field(machine, *calendar,
                                         kCalendarTimeField, *time);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    const auto compare_calendars = [](Machine& machine,
                                      std::span<const Value> arguments)
        -> Result<std::optional<std::pair<i64, i64>>> {
        auto left = receiver(arguments);
        auto right = arguments[1].as_reference();
        if (!left) return std::unexpected(left.error());
        if (!right || right->is_null()) {
            return std::optional<std::pair<i64, i64>> {};
        }
        auto compatible = is_instance_of(machine, *right, "java/util/Calendar");
        if (!compatible) return std::unexpected(compatible.error());
        if (!*compatible) return std::optional<std::pair<i64, i64>> {};
        auto left_time = long_field(machine, *left, kCalendarTimeField);
        auto right_time = long_field(machine, *right, kCalendarTimeField);
        if (!left_time) return std::unexpected(left_time.error());
        if (!right_time) return std::unexpected(right_time.error());
        return std::optional<std::pair<i64, i64>>(
            std::pair<i64, i64>(*left_time, *right_time));
    };
    add(registry, "java/util/Calendar", "before", "(Ljava/lang/Object;)Z",
        [compare_calendars](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto values = compare_calendars(machine, arguments);
            if (!values) return std::unexpected(values.error());
            return std::optional<Value>(Value::from_int(
                values->has_value() && (*values)->first < (*values)->second
                    ? 1 : 0));
        });
    add(registry, "java/util/Calendar", "after", "(Ljava/lang/Object;)Z",
        [compare_calendars](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto values = compare_calendars(machine, arguments);
            if (!values) return std::unexpected(values.error());
            return std::optional<Value>(Value::from_int(
                values->has_value() && (*values)->first > (*values)->second
                    ? 1 : 0));
        });
    add(registry, "java/util/Calendar", "equals", "(Ljava/lang/Object;)Z",
        [compare_calendars](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto values = compare_calendars(machine, arguments);
            if (!values) return std::unexpected(values.error());
            if (!values->has_value()) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto left = receiver(arguments);
            auto right = arguments[1].as_reference();
            if (!left) return std::unexpected(left.error());
            if (!right) return std::unexpected(right.error());
            auto left_zone = calendar_zone(machine, *left);
            auto right_zone = calendar_zone(machine, *right);
            if (!left_zone) return std::unexpected(left_zone.error());
            if (!right_zone) return std::unexpected(right_zone.error());
            auto left_offset = timezone_offset(machine, *left_zone);
            auto right_offset = timezone_offset(machine, *right_zone);
            if (!left_offset) return std::unexpected(left_offset.error());
            if (!right_offset) return std::unexpected(right_offset.error());
            return std::optional<Value>(Value::from_int(
                (*values)->first == (*values)->second &&
                *left_offset == *right_offset ? 1 : 0));
        });
    add(registry, "java/util/Calendar", "setTimeZone",
        "(Ljava/util/TimeZone;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            auto zone = arguments[1].as_reference();
            if (!calendar) return std::unexpected(calendar.error());
            if (!zone || zone->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Calendar timezone is null");
            }
            auto stored = set_reference_field(machine, *calendar,
                                              kCalendarZoneField, *zone);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Calendar", "getTimeZone",
        "()Ljava/util/TimeZone;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            if (!calendar) return std::unexpected(calendar.error());
            auto zone = calendar_zone(machine, *calendar);
            if (!zone) return std::unexpected(zone.error());
            return std::optional<Value>(Value::from_reference(*zone));
        });
    }
    calendar_compat::register_natives(registry);
}

} // namespace phoneme::vm
