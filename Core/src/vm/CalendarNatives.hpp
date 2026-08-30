#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "phoneme/vm/Machine.hpp"
#include "phoneme/vm/NativeMethodRegistry.hpp"

namespace phoneme::vm::calendar_compat {
namespace detail {

constexpr usize kTimeField = 0U;
constexpr usize kZoneField = 1U;
constexpr usize kFieldsField = 2U;
constexpr usize kIsSetField = 3U;
constexpr usize kIsTimeSetField = 4U;
constexpr usize kZoneRawOffsetField = 1U;
constexpr usize kDateTimeField = 0U;

constexpr i32 kFieldCount = 15;
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
constexpr i32 kSunday = 1;
constexpr i32 kSaturday = 7;

constexpr i64 kOneSecond = 1'000LL;
constexpr i64 kOneMinute = 60LL * kOneSecond;
constexpr i64 kOneHour = 60LL * kOneMinute;
constexpr i64 kOneDay = 24LL * kOneHour;
constexpr i64 kGregorianCutover = -12'219'292'800'000LL;
constexpr i32 kGregorianCutoverYear = 1582;
constexpr i64 kJanuaryOneYearOneJulianDay = 1'721'426LL;
constexpr i64 kEpochJulianDay = 2'440'588LL;
constexpr std::array<i32, 12> kDaysBeforeMonth {{
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
}};
constexpr std::array<i32, 12> kLeapDaysBeforeMonth {{
    0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335,
}};

struct Arrays final {
    ObjectRef fields;
    ObjectRef is_set;
};

[[nodiscard]] inline i64 current_millis() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<i64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

[[nodiscard]] inline i64 floor_divide(i64 numerator,
                                      i64 denominator) noexcept {
    return numerator >= 0
        ? numerator / denominator
        : ((numerator + 1) / denominator) - 1;
}

[[nodiscard]] inline i32 floor_divide(i32 numerator,
                                      i32 denominator,
                                      i32& remainder) noexcept {
    if (numerator >= 0) {
        remainder = numerator % denominator;
        return numerator / denominator;
    }
    const i32 quotient = ((numerator + 1) / denominator) - 1;
    remainder = numerator - quotient * denominator;
    return quotient;
}

[[nodiscard]] inline i32 floor_divide(i64 numerator,
                                      i32 denominator,
                                      i32& remainder) noexcept {
    if (numerator >= 0) {
        remainder = static_cast<i32>(numerator % denominator);
        return static_cast<i32>(numerator / denominator);
    }
    const i64 quotient = ((numerator + 1) / denominator) - 1;
    remainder = static_cast<i32>(numerator - quotient * denominator);
    return static_cast<i32>(quotient);
}

[[nodiscard]] inline i64 wrapping_add(i64 left, i64 right) noexcept {
    return std::bit_cast<i64>(std::bit_cast<u64>(left) +
                              std::bit_cast<u64>(right));
}

[[nodiscard]] inline i64 wrapping_subtract(i64 left, i64 right) noexcept {
    return std::bit_cast<i64>(std::bit_cast<u64>(left) -
                              std::bit_cast<u64>(right));
}

[[nodiscard]] inline i64 wrapping_multiply(i64 left, i64 right) noexcept {
    return std::bit_cast<i64>(std::bit_cast<u64>(left) *
                              std::bit_cast<u64>(right));
}

[[nodiscard]] inline Result<ObjectRef> receiver(
    std::span<const Value> arguments) {
    if (arguments.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "Calendar native is missing its receiver");
    }
    auto object = arguments.front().as_reference();
    if (!object || object->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Calendar receiver is null");
    }
    return *object;
}

[[nodiscard]] inline Result<Value> field_value(Machine& machine,
                                               ObjectRef object,
                                               usize index) {
    return machine.heap().field(object, index);
}

[[nodiscard]] inline Result<i32> int_field(Machine& machine,
                                           ObjectRef object,
                                           usize index) {
    auto value = field_value(machine, object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

[[nodiscard]] inline Result<i64> long_field(Machine& machine,
                                            ObjectRef object,
                                            usize index) {
    auto value = field_value(machine, object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_long();
}

[[nodiscard]] inline Result<ObjectRef> reference_field(Machine& machine,
                                                       ObjectRef object,
                                                       usize index) {
    auto value = field_value(machine, object, index);
    if (!value) return std::unexpected(value.error());
    return value->as_reference();
}

[[nodiscard]] inline Status set_int_field(Machine& machine,
                                          ObjectRef object,
                                          usize index,
                                          i32 value) {
    return machine.heap().set_field(object, index, Value::from_int(value));
}

[[nodiscard]] inline Status set_long_field(Machine& machine,
                                           ObjectRef object,
                                           usize index,
                                           i64 value) {
    return machine.heap().set_field(object, index, Value::from_long(value));
}

[[nodiscard]] inline Status set_reference_field(Machine& machine,
                                                ObjectRef object,
                                                usize index,
                                                ObjectRef value) {
    return machine.heap().set_field(object, index,
                                    Value::from_reference(value));
}

[[nodiscard]] inline Result<Arrays> arrays(Machine& machine,
                                           ObjectRef calendar) {
    constexpr std::array<usize, 2> indices {
        kFieldsField,
        kIsSetField,
    };
    std::array<Value, indices.size()> values {};
    auto loaded = machine.heap().read_fields(calendar, indices, values);
    if (!loaded) return std::unexpected(loaded.error());
    auto fields = values[0U].as_reference();
    auto is_set = values[1U].as_reference();
    if (!fields || !is_set) {
        return fail(ErrorCode::invalid_state,
                    "Calendar field arrays are unavailable");
    }
    if (fields->is_null() || is_set->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "Calendar field arrays are null");
    }
    return Arrays {*fields, *is_set};
}

[[nodiscard]] inline Result<std::vector<i32>> calendar_fields_snapshot(
    Machine& machine,
    const Arrays& storage) {
    auto fields = machine.heap().read_int_array(storage.fields);
    if (!fields) return std::unexpected(fields.error());
    if (fields->size() < static_cast<usize>(kFieldCount)) {
        return fail(ErrorCode::invalid_state,
                    "Calendar fields array is shorter than FIELD_COUNT");
    }
    return fields;
}

[[nodiscard]] inline Status store_calendar_fields(
    Machine& machine,
    const Arrays& storage,
    std::span<const i32> fields) {
    if (fields.size() < static_cast<usize>(kFieldCount)) {
        return fail(ErrorCode::invalid_state,
                    "Calendar field snapshot is shorter than FIELD_COUNT");
    }
    return machine.heap().write_int_array(
        storage.fields, 0U,
        fields.first(static_cast<usize>(kFieldCount)));
}

[[nodiscard]] inline Result<i32> array_int(Machine& machine,
                                           ObjectRef array,
                                           i32 index) {
    auto value = machine.heap().element(array, static_cast<usize>(index));
    if (!value) return std::unexpected(value.error());
    return value->as_int();
}

[[nodiscard]] inline Status set_array_int(Machine& machine,
                                          ObjectRef array,
                                          i32 index,
                                          i32 value) {
    return machine.heap().set_element(array, static_cast<usize>(index),
                                      Value::from_int(value));
}

[[nodiscard]] inline Result<i32> calendar_field(Machine& machine,
                                                const Arrays& storage,
                                                i32 field) {
    return array_int(machine, storage.fields, field);
}

[[nodiscard]] inline Status set_calendar_field(Machine& machine,
                                               const Arrays& storage,
                                               i32 field,
                                               i32 value) {
    return set_array_int(machine, storage.fields, field, value);
}

[[nodiscard]] inline Result<bool> calendar_is_set(Machine& machine,
                                                  const Arrays& storage,
                                                  i32 field) {
    auto value = array_int(machine, storage.is_set, field);
    if (!value) return std::unexpected(value.error());
    return *value != 0;
}

[[nodiscard]] inline Status set_calendar_is_set(Machine& machine,
                                                const Arrays& storage,
                                                i32 field,
                                                bool value) {
    return set_array_int(machine, storage.is_set, field, value ? 1 : 0);
}

[[nodiscard]] inline Result<i32> raw_offset(Machine& machine,
                                            ObjectRef calendar) {
    auto zone = reference_field(machine, calendar, kZoneField);
    if (!zone) return std::unexpected(zone.error());
    if (zone->is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Calendar timezone is null");
    }
    return int_field(machine, *zone, kZoneRawOffsetField);
}

[[nodiscard]] inline i64 millis_to_julian_day(i64 millis) noexcept {
    return kEpochJulianDay + floor_divide(millis, kOneDay);
}

[[nodiscard]] inline i64 julian_day_to_millis(i64 julian_day) noexcept {
    return wrapping_multiply(julian_day - kEpochJulianDay, kOneDay);
}

[[nodiscard]] inline i32 julian_day_to_day_of_week(
    i64 julian_day) noexcept {
    i32 day = static_cast<i32>((julian_day + 1) % 7);
    return day + (day < 0 ? 7 + kSunday : kSunday);
}

[[nodiscard]] inline Status time_to_fields(std::vector<i32>& fields,
                                           i64 time) {
    i32 day_of_year = 0;
    i32 raw_year = 1970;
    bool leap = false;
    if (time >= kGregorianCutover) {
        const i64 epoch_day =
            millis_to_julian_day(time) - kJanuaryOneYearOneJulianDay;
        i32 remainder = 0;
        const i32 n400 = floor_divide(epoch_day, 146097, remainder);
        const i32 n100 = floor_divide(remainder, 36524, remainder);
        const i32 n4 = floor_divide(remainder, 1461, remainder);
        const i32 n1 = floor_divide(remainder, 365, remainder);
        raw_year = 400 * n400 + 100 * n100 + 4 * n4 + n1;
        day_of_year = remainder;
        if (n100 == 4 || n1 == 4) {
            day_of_year = 365;
        } else {
            ++raw_year;
        }
        leap = (raw_year & 3) == 0 &&
               (raw_year % 100 != 0 || raw_year % 400 == 0);
        fields[static_cast<usize>(kDayOfWeek)] =
            static_cast<i32>((epoch_day + 1) % 7);
    } else {
        const i64 epoch_day = millis_to_julian_day(time) -
                              (kJanuaryOneYearOneJulianDay - 2);
        raw_year = static_cast<i32>(floor_divide(
            wrapping_add(wrapping_multiply(4, epoch_day), 1464), 1461));
        const i64 january_one = 365LL * (raw_year - 1LL) +
                                floor_divide(raw_year - 1, 4);
        day_of_year = static_cast<i32>(epoch_day - january_one);
        leap = (raw_year & 3) == 0;
        fields[static_cast<usize>(kDayOfWeek)] =
            static_cast<i32>((epoch_day - 1) % 7);
    }

    i32 correction = 0;
    const i32 march_one = leap ? 60 : 59;
    if (day_of_year >= march_one) correction = leap ? 1 : 2;
    const i32 month = (12 * (day_of_year + correction) + 6) / 367;
    const i32 date = day_of_year -
        (leap ? kLeapDaysBeforeMonth[static_cast<usize>(month)]
              : kDaysBeforeMonth[static_cast<usize>(month)]) + 1;

    const i32 dow = fields[static_cast<usize>(kDayOfWeek)];
    const i32 normalized_dow = dow + (dow < 0 ? kSunday + 7 : kSunday);
    const i32 visible_year = raw_year < 1 ? 1 - raw_year : raw_year;
    for (const auto [field, value] : std::array<std::pair<i32, i32>, 4> {{
             {kDayOfWeek, normalized_dow},
             {kYear, visible_year},
             {kMonth, month},
             {kDate, date},
         }}) {
        fields[static_cast<usize>(field)] = value;
    }
    return {};
}

[[nodiscard]] inline Status compute_fields(Machine& machine,
                                            ObjectRef calendar) {
    auto storage = arrays(machine, calendar);
    auto time = long_field(machine, calendar, kTimeField);
    auto offset = raw_offset(machine, calendar);
    if (!storage) return std::unexpected(storage.error());
    if (!time) return std::unexpected(time.error());
    if (!offset) return std::unexpected(offset.error());
    auto fields = calendar_fields_snapshot(machine, *storage);
    if (!fields) return std::unexpected(fields.error());

    i64 local = wrapping_add(*time, static_cast<i64>(*offset));
    if (*time > 0 && local < 0 && *offset > 0) {
        local = std::numeric_limits<i64>::max();
    } else if (*time < 0 && local > 0 && *offset < 0) {
        local = std::numeric_limits<i64>::min();
    }
    auto date_fields = time_to_fields(*fields, local);
    if (!date_fields) return date_fields;

    const i64 days = local / kOneDay;
    i64 millis_in_day = local - days * kOneDay;
    if (millis_in_day < 0) millis_in_day += kOneDay;
    // TimeZoneImpl in the current mobile profile has no DST rules, therefore
    // getOffset() is exactly getRawOffset(). Keep the two-step structure from
    // CalendarImpl so a rule-based zone can be inserted later without changing
    // Calendar state semantics.
    const i32 dst_offset = 0;
    millis_in_day += dst_offset;
    if (millis_in_day >= kOneDay) {
        i64 dst_millis = wrapping_add(local, dst_offset);
        millis_in_day -= kOneDay;
        if (local > 0 && dst_millis < 0 && dst_offset > 0) {
            dst_millis = std::numeric_limits<i64>::max();
        } else if (local < 0 && dst_millis > 0 && dst_offset < 0) {
            dst_millis = std::numeric_limits<i64>::min();
        }
        auto adjusted = time_to_fields(*fields, dst_millis);
        if (!adjusted) return adjusted;
    }

    const i32 millisecond = static_cast<i32>(millis_in_day % 1000);
    millis_in_day /= 1000;
    const i32 second = static_cast<i32>(millis_in_day % 60);
    millis_in_day /= 60;
    const i32 minute = static_cast<i32>(millis_in_day % 60);
    millis_in_day /= 60;
    const i32 hour_of_day = static_cast<i32>(millis_in_day);
    for (const auto [field, value] : std::array<std::pair<i32, i32>, 6> {{
             {kMillisecond, millisecond},
             {kSecond, second},
             {kMinute, minute},
             {kHourOfDay, hour_of_day},
             {kAmPm, hour_of_day / 12},
             {kHour, hour_of_day % 12},
         }}) {
        (*fields)[static_cast<usize>(field)] = value;
    }
    return store_calendar_fields(machine, *storage, *fields);
}

[[nodiscard]] inline Result<i64> calculate_julian_day(
    Machine& machine,
    const Arrays& storage,
    bool gregorian,
    i32 supplied_year) {
    auto month_value = calendar_field(machine, storage, kMonth);
    auto date_value = calendar_field(machine, storage, kDate);
    if (!month_value) return std::unexpected(month_value.error());
    if (!date_value) return std::unexpected(date_value.error());
    i32 year = supplied_year;
    i32 month = *month_value;
    if (month < 0 || month > 11) {
        i32 remainder = 0;
        year += floor_divide(month, 12, remainder);
        month = remainder;
    }
    bool leap = year % 4 == 0;
    i64 julian_day = 365LL * (year - 1LL) +
                     floor_divide(year - 1, 4) +
                     (kJanuaryOneYearOneJulianDay - 3);
    if (gregorian) {
        leap = leap && (year % 100 != 0 || year % 400 == 0);
        julian_day += floor_divide(year - 1, 400) -
                      floor_divide(year - 1, 100) + 2;
    }
    julian_day += (leap
        ? kLeapDaysBeforeMonth[static_cast<usize>(month)]
        : kDaysBeforeMonth[static_cast<usize>(month)]);
    julian_day += *date_value;
    return julian_day;
}

[[nodiscard]] inline Status correct_time(Machine& machine,
                                         const Arrays& storage) {
    auto hour_of_day_set = calendar_is_set(machine, storage, kHourOfDay);
    if (!hour_of_day_set) return std::unexpected(hour_of_day_set.error());
    if (*hour_of_day_set) {
        auto hour_of_day = calendar_field(machine, storage, kHourOfDay);
        if (!hour_of_day) return std::unexpected(hour_of_day.error());
        const i32 value = *hour_of_day % 24;
        auto hour_stored = set_calendar_field(
            machine, storage, kHourOfDay, value);
        auto ampm_stored = set_calendar_field(
            machine, storage, kAmPm, value < 12 ? 0 : 1);
        auto flag_cleared = set_calendar_is_set(
            machine, storage, kHourOfDay, false);
        if (!hour_stored) return hour_stored;
        if (!ampm_stored) return ampm_stored;
        return flag_cleared;
    }

    auto hour_set = calendar_is_set(machine, storage, kHour);
    auto ampm_set = calendar_is_set(machine, storage, kAmPm);
    if (!hour_set) return std::unexpected(hour_set.error());
    if (!ampm_set) return std::unexpected(ampm_set.error());
    if (!*hour_set && !*ampm_set) return {};
    auto hour = calendar_field(machine, storage, kHour);
    auto ampm = calendar_field(machine, storage, kAmPm);
    if (!hour) return std::unexpected(hour.error());
    if (!ampm) return std::unexpected(ampm.error());
    i32 value = *hour;
    i32 hour_of_day = value;
    i32 normalized_hour = value;
    i32 normalized_ampm = *ampm;
    if (value > 12) {
        hour_of_day = value % 12 + 12;
        normalized_hour = value % 12;
        normalized_ampm = 1;
    } else if (*ampm == 1) {
        hour_of_day = value % 12 + 12;
    }
    auto hod_stored = set_calendar_field(
        machine, storage, kHourOfDay, hour_of_day);
    auto hour_stored = set_calendar_field(
        machine, storage, kHour, normalized_hour);
    auto ampm_stored = set_calendar_field(
        machine, storage, kAmPm, normalized_ampm);
    auto hour_flag = set_calendar_is_set(machine, storage, kHour, false);
    auto ampm_flag = set_calendar_is_set(machine, storage, kAmPm, false);
    if (!hod_stored) return hod_stored;
    if (!hour_stored) return hour_stored;
    if (!ampm_stored) return ampm_stored;
    if (!hour_flag) return hour_flag;
    return ampm_flag;
}

[[nodiscard]] inline Status compute_time(Machine& machine,
                                          ObjectRef calendar) {
    auto storage = arrays(machine, calendar);
    if (!storage) return std::unexpected(storage.error());
    auto corrected = correct_time(machine, *storage);
    if (!corrected) return corrected;

    auto year = calendar_field(machine, *storage, kYear);
    if (!year) return std::unexpected(year.error());
    bool gregorian = *year >= kGregorianCutoverYear;
    auto julian_day = calculate_julian_day(
        machine, *storage, gregorian, *year);
    if (!julian_day) return std::unexpected(julian_day.error());

    auto dow_set = calendar_is_set(machine, *storage, kDayOfWeek);
    auto dow = calendar_field(machine, *storage, kDayOfWeek);
    if (!dow_set) return std::unexpected(dow_set.error());
    if (!dow) return std::unexpected(dow.error());
    if (*dow_set && *dow >= kSunday && *dow <= kSaturday) {
        const i32 delta = *dow - julian_day_to_day_of_week(*julian_day);
        *julian_day += delta;
        auto date = calendar_field(machine, *storage, kDate);
        if (!date) return std::unexpected(date.error());
        const i32 second_delta =
            *dow - julian_day_to_day_of_week(*julian_day);
        auto stored = set_calendar_field(
            machine, *storage, kDate, *date + second_delta);
        if (!stored) return stored;
    }

    i64 millis = julian_day_to_millis(*julian_day);
    if (gregorian != (millis >= kGregorianCutover) &&
        *julian_day != -106'749'550'580LL) {
        julian_day = calculate_julian_day(
            machine, *storage, !gregorian, *year);
        if (!julian_day) return std::unexpected(julian_day.error());
        millis = julian_day_to_millis(*julian_day);
    }

    auto hour = calendar_field(machine, *storage, kHourOfDay);
    auto minute = calendar_field(machine, *storage, kMinute);
    auto second = calendar_field(machine, *storage, kSecond);
    auto millisecond = calendar_field(machine, *storage, kMillisecond);
    if (!hour || !minute || !second || !millisecond) {
        return fail(ErrorCode::invalid_state,
                    "Calendar time fields are unavailable");
    }
    i32 millis_in_day = *hour;
    millis_in_day = static_cast<i32>(
        static_cast<u32>(millis_in_day) * 60U +
        static_cast<u32>(*minute));
    millis_in_day = static_cast<i32>(
        static_cast<u32>(millis_in_day) * 60U +
        static_cast<u32>(*second));
    millis_in_day = static_cast<i32>(
        static_cast<u32>(millis_in_day) * 1000U +
        static_cast<u32>(*millisecond));
    millis = wrapping_add(millis, static_cast<i64>(millis_in_day));

    auto offset = raw_offset(machine, calendar);
    if (!offset) return std::unexpected(offset.error());
    const i32 dst_offset = 0;
    const i64 utc = wrapping_subtract(
        wrapping_subtract(millis, static_cast<i64>(*offset)),
        static_cast<i64>(dst_offset));
    return set_long_field(machine, calendar, kTimeField, utc);
}

[[nodiscard]] inline Result<i64> get_time_in_millis(Machine& machine,
                                                    ObjectRef calendar) {
    auto is_time_set = int_field(machine, calendar, kIsTimeSetField);
    if (!is_time_set) return std::unexpected(is_time_set.error());
    if (*is_time_set == 0) {
        auto computed = compute_time(machine, calendar);
        if (!computed) return std::unexpected(computed.error());
        auto marked = set_int_field(
            machine, calendar, kIsTimeSetField, 1);
        if (!marked) return std::unexpected(marked.error());
    }
    return long_field(machine, calendar, kTimeField);
}

[[nodiscard]] inline Status set_time_in_millis(Machine& machine,
                                               ObjectRef calendar,
                                               i64 millis) {
    auto storage = arrays(machine, calendar);
    if (!storage) return std::unexpected(storage.error());
    auto marked = set_int_field(machine, calendar, kIsTimeSetField, 1);
    auto dow_cleared = set_calendar_field(
        machine, *storage, kDayOfWeek, 0);
    auto stored = set_long_field(machine, calendar, kTimeField, millis);
    if (!marked) return marked;
    if (!dow_cleared) return dow_cleared;
    if (!stored) return stored;
    return compute_fields(machine, calendar);
}

[[nodiscard]] inline Status initialize(Machine& machine,
                                       ObjectRef calendar,
                                       ObjectRef zone,
                                       i64 millis) {
    if (zone.is_null()) {
        return fail_java("java/lang/NullPointerException",
                         "Calendar timezone is null");
    }
    auto fields = machine.heap().allocate_array(
        "[I", static_cast<usize>(kFieldCount), Value::from_int(0));
    auto is_set = machine.heap().allocate_array(
        "[Z", static_cast<usize>(kFieldCount), Value::from_int(0));
    if (!fields) return std::unexpected(fields.error());
    if (!is_set) return std::unexpected(is_set.error());
    constexpr std::array<usize, 4> indices {
        kZoneField,
        kFieldsField,
        kIsSetField,
        kIsTimeSetField,
    };
    const std::array<Value, indices.size()> values {
        Value::from_reference(zone),
        Value::from_reference(*fields),
        Value::from_reference(*is_set),
        Value::from_int(1),
    };
    auto initialized = machine.heap().write_fields(calendar, indices, values);
    if (!initialized) return initialized;
    return set_time_in_millis(machine, calendar, millis);
}

[[nodiscard]] inline Result<ObjectRef> create_calendar(Machine& machine,
                                                       ObjectRef zone) {
    auto calendar = machine.class_states().allocate_instance(
        machine.heap(), "java/util/GregorianCalendar");
    if (!calendar) return std::unexpected(calendar.error());
    auto initialized = initialize(machine, *calendar, zone, current_millis());
    if (!initialized) return std::unexpected(initialized.error());
    return *calendar;
}

[[nodiscard]] inline Result<ObjectRef> default_zone(Machine& machine) {
    auto field = machine.class_states().resolve_field(
        "java/util/TimeZone", "defaultZone", "Ljava/util/TimeZone;", true);
    if (!field) return std::unexpected(field.error());
    auto value = machine.class_states().static_field(*field);
    if (!value) return std::unexpected(value.error());
    auto zone = value->as_reference();
    if (!zone) return std::unexpected(zone.error());
    if (!zone->is_null()) return *zone;
    auto invoked = machine.invoke_static(
        "java/util/TimeZone", "getDefault", "()Ljava/util/TimeZone;");
    if (!invoked) return std::unexpected(invoked.error());
    if (invoked->throwable.has_value()) {
        return fail(ErrorCode::invalid_state,
                    "TimeZone.getDefault threw during Calendar construction");
    }
    if (!invoked->return_value.has_value()) {
        return fail(ErrorCode::invalid_state,
                    "TimeZone.getDefault returned no value");
    }
    auto result = invoked->return_value->as_reference();
    if (!result || result->is_null()) {
        return fail(ErrorCode::invalid_state,
                    "TimeZone.getDefault returned null");
    }
    return *result;
}

inline void add(NativeMethodRegistry& registry,
                std::string owner,
                std::string name,
                std::string descriptor,
                NativeMethod implementation) {
    auto registered = registry.register_method(
        std::move(owner), std::move(name), std::move(descriptor),
        std::move(implementation));
    if (!registered) std::terminate();
}

} // namespace detail

inline void register_natives(NativeMethodRegistry& registry) {
    using namespace detail;

    const NativeMethod constructor =
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
                auto fallback = default_zone(machine);
                if (!fallback) return std::unexpected(fallback.error());
                zone = *fallback;
            }
            auto initialized = initialize(
                machine, *calendar, zone, current_millis());
            if (!initialized) return std::unexpected(initialized.error());
            return std::optional<Value> {};
        };
    add(registry, "java/util/Calendar", "<init>", "()V", constructor);
    add(registry, "java/util/GregorianCalendar", "<init>", "()V", constructor);
    add(registry, "java/util/GregorianCalendar", "<init>",
        "(Ljava/util/TimeZone;)V", constructor);

    add(registry, "java/util/GregorianCalendar", "computeFields", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            if (!calendar) return std::unexpected(calendar.error());
            auto computed = compute_fields(machine, *calendar);
            if (!computed) return std::unexpected(computed.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/GregorianCalendar", "computeTime", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            if (!calendar) return std::unexpected(calendar.error());
            auto computed = compute_time(machine, *calendar);
            if (!computed) return std::unexpected(computed.error());
            return std::optional<Value> {};
        });

    add(registry, "java/util/Calendar", "getInstance",
        "()Ljava/util/Calendar;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            if (!arguments.empty()) {
                return fail(ErrorCode::invalid_argument,
                            "Calendar.getInstance expects no arguments");
            }
            auto zone = default_zone(machine);
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

    add(registry, "java/util/Calendar", "getTime", "()Ljava/util/Date;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            if (!calendar) return std::unexpected(calendar.error());
            auto millis = get_time_in_millis(machine, *calendar);
            if (!millis) return std::unexpected(millis.error());
            auto date = machine.class_states().allocate_instance(
                machine.heap(), "java/util/Date");
            if (!date) return std::unexpected(date.error());
            auto stored = set_long_field(
                machine, *date, kDateTimeField, *millis);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value>(Value::from_reference(*date));
        });
    add(registry, "java/util/Calendar", "setTime", "(Ljava/util/Date;)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            auto date = arguments[1].as_reference();
            if (!calendar) return std::unexpected(calendar.error());
            if (!date || date->is_null()) {
                return fail_java("java/lang/NullPointerException",
                                 "Calendar date is null");
            }
            auto millis = long_field(machine, *date, kDateTimeField);
            if (!millis) return std::unexpected(millis.error());
            auto stored = set_time_in_millis(machine, *calendar, *millis);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Calendar", "getTimeInMillis", "()J",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            if (!calendar) return std::unexpected(calendar.error());
            auto millis = get_time_in_millis(machine, *calendar);
            if (!millis) return std::unexpected(millis.error());
            return std::optional<Value>(Value::from_long(*millis));
        });
    add(registry, "java/util/Calendar", "setTimeInMillis", "(J)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            auto millis = arguments[1].as_long();
            if (!calendar) return std::unexpected(calendar.error());
            if (!millis) return std::unexpected(millis.error());
            auto stored = set_time_in_millis(machine, *calendar, *millis);
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    add(registry, "java/util/Calendar", "get", "(I)I",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            auto requested = arguments[1].as_int();
            if (!calendar) return std::unexpected(calendar.error());
            if (!requested) return std::unexpected(requested.error());
            if (*requested < 0 || *requested >= kFieldCount) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "Calendar field is outside bounds");
            }
            auto storage = arrays(machine, *calendar);
            if (!storage) return std::unexpected(storage.error());
            auto dow_set = calendar_is_set(
                machine, *storage, kDayOfWeek);
            if (!dow_set) return std::unexpected(dow_set.error());
            const bool recompute =
                (*dow_set && (*requested == kDate || *requested == kMonth ||
                              *requested == kYear)) ||
                *requested == kDayOfWeek ||
                *requested == kHourOfDay ||
                *requested == kAmPm ||
                *requested == kHour;
            if (recompute) {
                auto millis = get_time_in_millis(machine, *calendar);
                if (!millis) return std::unexpected(millis.error());
                auto computed = compute_fields(machine, *calendar);
                if (!computed) return std::unexpected(computed.error());
            } else if (*requested != kYear && *requested != kMonth &&
                       *requested != kDate && *requested != kMinute &&
                       *requested != kSecond &&
                       *requested != kMillisecond) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "Calendar field is unsupported");
            }
            auto value = calendar_field(machine, *storage, *requested);
            if (!value) return std::unexpected(value.error());
            return std::optional<Value>(Value::from_int(*value));
        });

    add(registry, "java/util/Calendar", "set", "(II)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            auto requested = arguments[1].as_int();
            auto value = arguments[2].as_int();
            if (!calendar) return std::unexpected(calendar.error());
            if (!requested) return std::unexpected(requested.error());
            if (!value) return std::unexpected(value.error());
            if (*requested < 0 || *requested >= kFieldCount) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "Calendar field is outside bounds");
            }
            auto storage = arrays(machine, *calendar);
            if (!storage) return std::unexpected(storage.error());
            if (*requested == kHourOfDay) {
                auto first = set_calendar_is_set(
                    machine, *storage, kHour, false);
                auto second = set_calendar_is_set(
                    machine, *storage, kAmPm, false);
                if (!first) return std::unexpected(first.error());
                if (!second) return std::unexpected(second.error());
            } else if (*requested == kHour) {
                auto cleared = set_calendar_is_set(
                    machine, *storage, kHourOfDay, false);
                if (!cleared) return std::unexpected(cleared.error());
            } else if (*requested == kAmPm &&
                       (*value == 0 || *value == 1)) {
                auto cleared = set_calendar_is_set(
                    machine, *storage, kHourOfDay, false);
                if (!cleared) return std::unexpected(cleared.error());
            } else if (*requested == kDate) {
                auto cleared = set_calendar_is_set(
                    machine, *storage, kDayOfWeek, false);
                if (!cleared) return std::unexpected(cleared.error());
            }
            auto time_invalid = set_int_field(
                machine, *calendar, kIsTimeSetField, 0);
            auto flag = set_calendar_is_set(
                machine, *storage, *requested, true);
            auto stored = set_calendar_field(
                machine, *storage, *requested, *value);
            if (!time_invalid) return std::unexpected(time_invalid.error());
            if (!flag) return std::unexpected(flag.error());
            if (!stored) return std::unexpected(stored.error());
            return std::optional<Value> {};
        });

    add(registry, "java/util/Calendar", "clear", "()V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            if (!calendar) return std::unexpected(calendar.error());
            auto storage = arrays(machine, *calendar);
            if (!storage) return std::unexpected(storage.error());
            auto fields_cleared = machine.heap().fill_array_range(
                storage->fields, 0U, static_cast<usize>(kFieldCount),
                Value::from_int(0));
            auto flags_cleared = machine.heap().fill_array_range(
                storage->is_set, 0U, static_cast<usize>(kFieldCount),
                Value::from_int(0));
            if (!fields_cleared) return std::unexpected(fields_cleared.error());
            if (!flags_cleared) return std::unexpected(flags_cleared.error());
            auto invalid = set_int_field(
                machine, *calendar, kIsTimeSetField, 0);
            if (!invalid) return std::unexpected(invalid.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Calendar", "clear", "(I)V",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            auto requested = arguments[1].as_int();
            if (!calendar) return std::unexpected(calendar.error());
            if (!requested) return std::unexpected(requested.error());
            if (*requested < 0 || *requested >= kFieldCount) {
                return fail_java("java/lang/ArrayIndexOutOfBoundsException",
                                 "Calendar field is outside bounds");
            }
            auto storage = arrays(machine, *calendar);
            if (!storage) return std::unexpected(storage.error());
            auto value = set_calendar_field(
                machine, *storage, *requested, 0);
            auto flag = set_calendar_is_set(
                machine, *storage, *requested, false);
            auto invalid = set_int_field(
                machine, *calendar, kIsTimeSetField, 0);
            if (!value) return std::unexpected(value.error());
            if (!flag) return std::unexpected(flag.error());
            if (!invalid) return std::unexpected(invalid.error());
            return std::optional<Value> {};
        });

    const auto compare = [](Machine& machine,
                            std::span<const Value> arguments)
        -> Result<std::optional<std::pair<i64, i64>>> {
        auto left = receiver(arguments);
        auto right = arguments[1].as_reference();
        if (!left) return std::unexpected(left.error());
        if (!right || right->is_null()) {
            return std::optional<std::pair<i64, i64>> {};
        }
        auto assignable = machine.object_is_instance(
            *right, "java/util/Calendar");
        if (!assignable) return std::unexpected(assignable.error());
        if (!*assignable) {
            return std::optional<std::pair<i64, i64>> {};
        }
        auto left_time = get_time_in_millis(machine, *left);
        auto right_time = get_time_in_millis(machine, *right);
        if (!left_time) return std::unexpected(left_time.error());
        if (!right_time) return std::unexpected(right_time.error());
        return std::optional<std::pair<i64, i64>>(
            std::pair<i64, i64>(*left_time, *right_time));
    };
    add(registry, "java/util/Calendar", "before", "(Ljava/lang/Object;)Z",
        [compare](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto values = compare(machine, arguments);
            if (!values) return std::unexpected(values.error());
            return std::optional<Value>(Value::from_int(
                values->has_value() && (*values)->first < (*values)->second));
        });
    add(registry, "java/util/Calendar", "after", "(Ljava/lang/Object;)Z",
        [compare](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto values = compare(machine, arguments);
            if (!values) return std::unexpected(values.error());
            return std::optional<Value>(Value::from_int(
                values->has_value() && (*values)->first > (*values)->second));
        });
    add(registry, "java/util/Calendar", "equals", "(Ljava/lang/Object;)Z",
        [compare](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto values = compare(machine, arguments);
            if (!values) return std::unexpected(values.error());
            if (!values->has_value() ||
                (*values)->first != (*values)->second) {
                return std::optional<Value>(Value::from_int(0));
            }
            auto left = receiver(arguments);
            auto right = arguments[1].as_reference();
            if (!left || !right) {
                return fail(ErrorCode::invalid_state,
                            "Calendar equality operands are invalid");
            }
            auto left_offset = raw_offset(machine, *left);
            auto right_offset = raw_offset(machine, *right);
            if (!left_offset) return std::unexpected(left_offset.error());
            if (!right_offset) return std::unexpected(right_offset.error());
            return std::optional<Value>(Value::from_int(
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
            auto millis = get_time_in_millis(machine, *calendar);
            if (!millis) return std::unexpected(millis.error());
            static_cast<void>(*millis);
            auto stored = set_reference_field(
                machine, *calendar, kZoneField, *zone);
            if (!stored) return std::unexpected(stored.error());
            auto computed = compute_fields(machine, *calendar);
            if (!computed) return std::unexpected(computed.error());
            return std::optional<Value> {};
        });
    add(registry, "java/util/Calendar", "getTimeZone",
        "()Ljava/util/TimeZone;",
        [](Machine& machine, std::span<const Value> arguments)
            -> Result<std::optional<Value>> {
            auto calendar = receiver(arguments);
            if (!calendar) return std::unexpected(calendar.error());
            auto zone = reference_field(machine, *calendar, kZoneField);
            if (!zone) return std::unexpected(zone.error());
            return std::optional<Value>(Value::from_reference(*zone));
        });
}

} // namespace phoneme::vm::calendar_compat
