#include "phoneme/vm/BuiltinClassRegistry.hpp"

#include <array>

#include "BuiltinClassSupport.hpp"

namespace phoneme::vm {
namespace {

using namespace builtin;

[[nodiscard]] ClassPtr build_util_class(std::string_view name) {
    if (name == "java/util/TimerTask") {
        return make_class("java/util/TimerTask", "java/lang/Object",
                          kOrdinary | kAbstract, {}, {
            method(kProtected, "<init>", "()V"),
            method(kPublic | kAbstract, "run", "()V"),
            method(kPublic, "cancel", "()Z"),
            method(kPublic, "scheduledExecutionTime", "()J"),
        }, {"java/lang/Runnable"});
    }
    if (name == "java/util/Timer") {
        return make_class("java/util/Timer", "java/lang/Object", kOrdinary,
                          {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Z)V"),
            method(kPublic, "schedule", "(Ljava/util/TimerTask;J)V"),
            method(kPublic, "schedule", "(Ljava/util/TimerTask;JJ)V"),
            method(kPublic, "schedule",
                   "(Ljava/util/TimerTask;Ljava/util/Date;)V"),
            method(kPublic, "schedule",
                   "(Ljava/util/TimerTask;Ljava/util/Date;J)V"),
            method(kPublic, "scheduleAtFixedRate",
                   "(Ljava/util/TimerTask;JJ)V"),
            method(kPublic, "scheduleAtFixedRate",
                   "(Ljava/util/TimerTask;Ljava/util/Date;J)V"),
            method(kPublic, "cancel", "()V"),
        });
    }
    if (name == "java/util/Date") {
        return make_class("java/util/Date", "java/lang/Object", kOrdinary, {
            field(kPrivate, "fastTime", "J"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(J)V"),
            method(kPublic | kStatic, "from",
                   "(Ljava/time/Instant;)Ljava/util/Date;"),
            method(kPublic, "getTime", "()J"),
            method(kPublic, "setTime", "(J)V"),
            method(kPublic, "before", "(Ljava/util/Date;)Z"),
            method(kPublic, "after", "(Ljava/util/Date;)Z"),
            method(kPublic, "compareTo", "(Ljava/util/Date;)I"),
            method(kPublic, "compareTo", "(Ljava/lang/Object;)I"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/lang/Cloneable", "java/io/Serializable",
             "java/lang/Comparable"});
    }
    if (name == "java/util/TimeZone") {
        return make_class("java/util/TimeZone", "java/lang/Object",
                          kOrdinary | kAbstract, {
            // Kept in the base layout so native operations work for every
            // concrete phoneME-compatible implementation.
            field(kPrivate, "id", "Ljava/lang/String;"),
            field(kPrivate, "rawOffset", "I"),
            field(kPrivate | kStatic, "defaultZone", "Ljava/util/TimeZone;"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic | kAbstract, "getOffset", "(IIIIII)I"),
            method(kPublic | kAbstract, "getRawOffset", "()I"),
            method(kPublic, "setRawOffset", "(I)V"),
            method(kPublic | kAbstract, "useDaylightTime", "()Z"),
            method(kPublic, "getID", "()Ljava/lang/String;"),
            method(kPublic, "setID", "(Ljava/lang/String;)V"),
            method(kPublic, "hasSameRules", "(Ljava/util/TimeZone;)Z"),
            method(kPublic | kStatic, "getTimeZone",
                   "(Ljava/lang/String;)Ljava/util/TimeZone;"),
            method(kPublic | kStatic, "getDefault", "()Ljava/util/TimeZone;"),
            method(kPublic | kStatic, "setDefault", "(Ljava/util/TimeZone;)V"),
            method(kPublic | kStatic, "getAvailableIDs", "()[Ljava/lang/String;"),
        }, {"java/lang/Cloneable", "java/io/Serializable"});
    }
    if (name == "com/sun/cldc/util/j2me/TimeZoneImpl") {
        return make_class("com/sun/cldc/util/j2me/TimeZoneImpl",
                          "java/util/TimeZone", kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "getOffset", "(IIIIII)I"),
            method(kPublic, "getRawOffset", "()I"),
            method(kPublic, "useDaylightTime", "()Z"),
            method(kPublic, "getID", "()Ljava/lang/String;"),
            method(kPublic | kSynchronized, "getInstance",
                   "(Ljava/lang/String;)Ljava/util/TimeZone;"),
            method(kPublic | kSynchronized, "getIDs",
                   "()[Ljava/lang/String;"),
        });
    }
    if (name == "java/util/Calendar") {
        return make_class("java/util/Calendar", "java/lang/Object",
                          kOrdinary | kAbstract, {
            field(kProtected, "time", "J"),
            field(kPrivate, "zone", "Ljava/util/TimeZone;"),
            field(kProtected, "fields", "[I"),
            field(kProtected, "isSet", "[Z"),
            field(kPrivate, "isTimeSet", "Z"),
            field(kPublic | kStatic | kFinal, "YEAR", "I"),
            field(kPublic | kStatic | kFinal, "MONTH", "I"),
            field(kPublic | kStatic | kFinal, "DATE", "I"),
            field(kPublic | kStatic | kFinal, "DAY_OF_MONTH", "I"),
            field(kPublic | kStatic | kFinal, "DAY_OF_WEEK", "I"),
            field(kPublic | kStatic | kFinal, "AM_PM", "I"),
            field(kPublic | kStatic | kFinal, "HOUR", "I"),
            field(kPublic | kStatic | kFinal, "HOUR_OF_DAY", "I"),
            field(kPublic | kStatic | kFinal, "MINUTE", "I"),
            field(kPublic | kStatic | kFinal, "SECOND", "I"),
            field(kPublic | kStatic | kFinal, "MILLISECOND", "I"),
            field(kPublic | kStatic | kFinal, "SUNDAY", "I"),
            field(kPublic | kStatic | kFinal, "MONDAY", "I"),
            field(kPublic | kStatic | kFinal, "TUESDAY", "I"),
            field(kPublic | kStatic | kFinal, "WEDNESDAY", "I"),
            field(kPublic | kStatic | kFinal, "THURSDAY", "I"),
            field(kPublic | kStatic | kFinal, "FRIDAY", "I"),
            field(kPublic | kStatic | kFinal, "SATURDAY", "I"),
            field(kPublic | kStatic | kFinal, "JANUARY", "I"),
            field(kPublic | kStatic | kFinal, "FEBRUARY", "I"),
            field(kPublic | kStatic | kFinal, "MARCH", "I"),
            field(kPublic | kStatic | kFinal, "APRIL", "I"),
            field(kPublic | kStatic | kFinal, "MAY", "I"),
            field(kPublic | kStatic | kFinal, "JUNE", "I"),
            field(kPublic | kStatic | kFinal, "JULY", "I"),
            field(kPublic | kStatic | kFinal, "AUGUST", "I"),
            field(kPublic | kStatic | kFinal, "SEPTEMBER", "I"),
            field(kPublic | kStatic | kFinal, "OCTOBER", "I"),
            field(kPublic | kStatic | kFinal, "NOVEMBER", "I"),
            field(kPublic | kStatic | kFinal, "DECEMBER", "I"),
            field(kPublic | kStatic | kFinal, "AM", "I"),
            field(kPublic | kStatic | kFinal, "PM", "I"),
        }, {
            method(kProtected, "<init>", "()V"),
            method(kPublic | kStatic | kSynchronized, "getInstance",
                   "()Ljava/util/Calendar;"),
            method(kPublic | kStatic | kSynchronized, "getInstance",
                   "(Ljava/util/TimeZone;)Ljava/util/Calendar;"),
            method(kPublic | kFinal, "getTime", "()Ljava/util/Date;"),
            method(kPublic | kFinal, "setTime", "(Ljava/util/Date;)V"),
            method(kProtected, "getTimeInMillis", "()J"),
            method(kProtected, "setTimeInMillis", "(J)V"),
            method(kPublic | kFinal, "get", "(I)I"),
            method(kPublic | kFinal, "set", "(II)V"),
            method(kPublic, "clear", "()V"),
            method(kPublic, "clear", "(I)V"),
            method(kPublic, "before", "(Ljava/lang/Object;)Z"),
            method(kPublic, "after", "(Ljava/lang/Object;)Z"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "setTimeZone", "(Ljava/util/TimeZone;)V"),
            method(kPublic, "getTimeZone", "()Ljava/util/TimeZone;"),
            method(kProtected | kAbstract, "computeFields", "()V"),
            method(kProtected | kAbstract, "computeTime", "()V"),
        }, {"java/lang/Cloneable", "java/io/Serializable"});
    }
    if (name == "java/util/GregorianCalendar") {
        return make_class("java/util/GregorianCalendar", "java/util/Calendar",
                          kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/util/TimeZone;)V"),
            method(kProtected, "computeFields", "()V"),
            method(kProtected, "computeTime", "()V"),
        });
    }
    if (name == "java/util/Objects") {
        return make_class("java/util/Objects", "java/lang/Object",
                          kOrdinary | kFinal, {}, {
            method(kPublic | kStatic, "requireNonNull",
                   "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic | kStatic, "requireNonNull",
                   "(Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/Object;"),
            method(kPublic | kStatic, "requireNonNull",
                   "(Ljava/lang/Object;Ljava/util/function/Supplier;)Ljava/lang/Object;"),
            method(kPublic | kStatic, "equals",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Z"),
            method(kPublic | kStatic, "deepEquals",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Z"),
            method(kPublic | kStatic, "compare",
                   "(Ljava/lang/Object;Ljava/lang/Object;Ljava/util/Comparator;)I"),
            method(kPublic | kStatic, "isNull", "(Ljava/lang/Object;)Z"),
            method(kPublic | kStatic, "nonNull", "(Ljava/lang/Object;)Z"),
            method(kPublic | kStatic, "hashCode", "(Ljava/lang/Object;)I"),
            method(kPublic | kStatic, "hash", "([Ljava/lang/Object;)I"),
            method(kPublic | kStatic, "toString",
                   "(Ljava/lang/Object;)Ljava/lang/String;"),
            method(kPublic | kStatic, "toString",
                   "(Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/String;"),
        });
    }
    if (name == "java/util/Enumeration") {
        return make_class("java/util/Enumeration", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "hasMoreElements", "()Z"),
            method(kPublic | kAbstract, "nextElement", "()Ljava/lang/Object;"),
        });
    }
    if (name == "java/util/ArrayEnumeration") {
        return make_class("java/util/ArrayEnumeration", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate, "values", "[Ljava/lang/Object;"),
            field(kPrivate, "index", "I"),
            field(kPrivate, "size", "I"),
        }, {
            method(kPublic, "hasMoreElements", "()Z"),
            method(kPublic, "nextElement", "()Ljava/lang/Object;"),
        }, {"java/util/Enumeration"});
    }
    if (name == "java/util/StringTokenizer") {
        return make_class("java/util/StringTokenizer", "java/lang/Object",
                          kOrdinary, {
            field(kPrivate, "string", "Ljava/lang/String;"),
            field(kPrivate, "delimiters", "Ljava/lang/String;"),
            field(kPrivate, "position", "I"),
            field(kPrivate, "returnDelimiters", "Z"),
        }, {
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            method(kPublic, "<init>",
                   "(Ljava/lang/String;Ljava/lang/String;)V"),
            method(kPublic, "<init>",
                   "(Ljava/lang/String;Ljava/lang/String;Z)V"),
            method(kPublic, "hasMoreTokens", "()Z"),
            method(kPublic, "nextToken", "()Ljava/lang/String;"),
            method(kPublic, "nextToken",
                   "(Ljava/lang/String;)Ljava/lang/String;"),
            method(kPublic, "hasMoreElements", "()Z"),
            method(kPublic, "nextElement", "()Ljava/lang/Object;"),
            method(kPublic, "countTokens", "()I"),
        }, {"java/util/Enumeration"});
    }
    if (name == "java/util/Locale") {
        return make_class("java/util/Locale", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPublic | kStatic | kFinal, "ROOT", "Ljava/util/Locale;"),
            field(kPublic | kStatic | kFinal, "ENGLISH", "Ljava/util/Locale;"),
        }, {
            method(kPrivate, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            method(kStatic, "<clinit>", "()V"),
        });
    }
    if (name == "java/util/List") {
        return make_class("java/util/List", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "size", "()I"),
            method(kPublic | kAbstract, "isEmpty", "()Z"),
            method(kPublic | kAbstract, "contains", "(Ljava/lang/Object;)Z"),
            method(kPublic | kAbstract, "get", "(I)Ljava/lang/Object;"),
            method(kPublic | kAbstract, "set",
                   "(ILjava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic | kAbstract, "add", "(Ljava/lang/Object;)Z"),
            method(kPublic | kAbstract, "add", "(ILjava/lang/Object;)V"),
            method(kPublic | kAbstract, "remove", "(I)Ljava/lang/Object;"),
            method(kPublic | kAbstract, "remove", "(Ljava/lang/Object;)Z"),
            method(kPublic | kAbstract, "clear", "()V"),
            method(kPublic | kAbstract, "indexOf", "(Ljava/lang/Object;)I"),
            method(kPublic | kAbstract, "lastIndexOf", "(Ljava/lang/Object;)I"),
            method(kPublic | kAbstract, "iterator", "()Ljava/util/Iterator;"),
            method(kPublic | kAbstract, "toArray", "()[Ljava/lang/Object;"),
            method(kPublic | kAbstract, "toArray",
                   "([Ljava/lang/Object;)[Ljava/lang/Object;"),
            method(kPublic | kAbstract, "sort", "(Ljava/util/Comparator;)V"),
            method(kPublic | kAbstract, "addAll", "(Ljava/util/Collection;)Z"),
            method(kPublic | kStatic, "of", "()Ljava/util/List;"),
            method(kPublic | kStatic, "of",
                   "(Ljava/lang/Object;)Ljava/util/List;"),
            method(kPublic | kStatic, "of",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/util/List;"),
            method(kPublic | kStatic, "of",
                   "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Ljava/util/List;"),
            method(kPublic | kStatic, "copyOf",
                   "(Ljava/util/Collection;)Ljava/util/List;"),
        }, {"java/util/Collection"});
    }
    if (name == "java/util/ArrayList") {
        return make_class("java/util/ArrayList", "java/lang/Object", kOrdinary, {
            field(kPrivate, "elementData", "[Ljava/lang/Object;"),
            field(kPrivate, "size", "I"),
            field(kPrivate, "capacityIncrement", "I"),
            field(kPrivate, "mutationMode", "I"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(I)V"),
            method(kPublic, "<init>", "(Ljava/util/Collection;)V"),
            method(kPublic, "size", "()I"),
            method(kPublic, "isEmpty", "()Z"),
            method(kPublic, "contains", "(Ljava/lang/Object;)Z"),
            method(kPublic, "get", "(I)Ljava/lang/Object;"),
            method(kPublic, "set", "(ILjava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "add", "(Ljava/lang/Object;)Z"),
            method(kPublic, "add", "(ILjava/lang/Object;)V"),
            method(kPublic, "remove", "(I)Ljava/lang/Object;"),
            method(kPublic, "remove", "(Ljava/lang/Object;)Z"),
            method(kPublic, "clear", "()V"),
            method(kPublic, "indexOf", "(Ljava/lang/Object;)I"),
            method(kPublic, "lastIndexOf", "(Ljava/lang/Object;)I"),
            method(kPublic, "iterator", "()Ljava/util/Iterator;"),
            method(kPublic, "toArray", "()[Ljava/lang/Object;"),
            method(kPublic, "toArray",
                   "([Ljava/lang/Object;)[Ljava/lang/Object;"),
            method(kPublic, "sort", "(Ljava/util/Comparator;)V"),
            method(kPublic, "stream", "()Ljava/util/stream/Stream;"),
            method(kPublic, "addAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/util/List"});
    }
    if (name == "java/util/ArrayDeque") {
        return make_class("java/util/ArrayDeque", "java/lang/Object", kOrdinary, {
            field(kPrivate, "elements", "[Ljava/lang/Object;"),
            field(kPrivate, "size", "I"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(I)V"),
            method(kPublic, "<init>", "(Ljava/util/Collection;)V"),
            method(kPublic, "addFirst", "(Ljava/lang/Object;)V"),
            method(kPublic, "addLast", "(Ljava/lang/Object;)V"),
            method(kPublic, "offerFirst", "(Ljava/lang/Object;)Z"),
            method(kPublic, "offerLast", "(Ljava/lang/Object;)Z"),
            method(kPublic, "removeFirst", "()Ljava/lang/Object;"),
            method(kPublic, "removeLast", "()Ljava/lang/Object;"),
            method(kPublic, "pollFirst", "()Ljava/lang/Object;"),
            method(kPublic, "pollLast", "()Ljava/lang/Object;"),
            method(kPublic, "getFirst", "()Ljava/lang/Object;"),
            method(kPublic, "getLast", "()Ljava/lang/Object;"),
            method(kPublic, "peekFirst", "()Ljava/lang/Object;"),
            method(kPublic, "peekLast", "()Ljava/lang/Object;"),
            method(kPublic, "removeFirstOccurrence", "(Ljava/lang/Object;)Z"),
            method(kPublic, "removeLastOccurrence", "(Ljava/lang/Object;)Z"),
            method(kPublic, "add", "(Ljava/lang/Object;)Z"),
            method(kPublic, "offer", "(Ljava/lang/Object;)Z"),
            method(kPublic, "remove", "()Ljava/lang/Object;"),
            method(kPublic, "poll", "()Ljava/lang/Object;"),
            method(kPublic, "element", "()Ljava/lang/Object;"),
            method(kPublic, "peek", "()Ljava/lang/Object;"),
            method(kPublic, "push", "(Ljava/lang/Object;)V"),
            method(kPublic, "pop", "()Ljava/lang/Object;"),
            method(kPublic, "size", "()I"),
            method(kPublic, "isEmpty", "()Z"),
            method(kPublic, "contains", "(Ljava/lang/Object;)Z"),
            method(kPublic, "remove", "(Ljava/lang/Object;)Z"),
            method(kPublic, "clear", "()V"),
            method(kPublic, "iterator", "()Ljava/util/Iterator;"),
            method(kPublic, "descendingIterator", "()Ljava/util/Iterator;"),
            method(kPublic, "toArray", "()[Ljava/lang/Object;"),
            method(kPublic, "toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;"),
            method(kPublic, "addAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "clone", "()Ljava/util/ArrayDeque;"),
        }, {"java/util/Deque", "java/lang/Cloneable", "java/io/Serializable"});
    }
    if (name == "java/util/ArrayDequeIterator") {
        return make_class("java/util/ArrayDequeIterator", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate, "values", "[Ljava/lang/Object;"),
            field(kPrivate, "index", "I"),
            field(kPrivate, "size", "I"),
        }, {
            method(kPublic, "hasNext", "()Z"),
            method(kPublic, "next", "()Ljava/lang/Object;"),
            method(kPublic, "remove", "()V"),
        }, {"java/util/Iterator"});
    }
    if (name == "java/time/temporal/TemporalAccessor") {
        return make_class("java/time/temporal/TemporalAccessor", "java/lang/Object",
                          kPublic | kInterface | kAbstract);
    }
    if (name == "java/time/Instant") {
        return make_class("java/time/Instant", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "epochMilli", "J"),
        }, {
            method(kPublic | kStatic, "now", "()Ljava/time/Instant;"),
            method(kPublic | kStatic, "ofEpochMilli", "(J)Ljava/time/Instant;"),
            method(kPublic, "toEpochMilli", "()J"),
        }, {"java/time/temporal/TemporalAccessor"});
    }
    if (name == "java/time/ZoneId") {
        return make_class("java/time/ZoneId", "java/lang/Object",
                          kOrdinary, {
            field(kPrivate | kFinal, "id", "Ljava/lang/String;"),
            field(kPrivate | kFinal, "rawOffset", "I"),
        }, {
            method(kPublic | kStatic, "systemDefault", "()Ljava/time/ZoneId;"),
            method(kPublic, "getId", "()Ljava/lang/String;"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        });
    }
    if (name == "java/time/format/DateTimeFormatter") {
        return make_class("java/time/format/DateTimeFormatter", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "pattern", "Ljava/lang/String;"),
            field(kPrivate | kFinal, "zone", "Ljava/time/ZoneId;"),
        }, {
            method(kPublic | kStatic, "ofPattern",
                   "(Ljava/lang/String;)Ljava/time/format/DateTimeFormatter;"),
            method(kPublic, "withZone",
                   "(Ljava/time/ZoneId;)Ljava/time/format/DateTimeFormatter;"),
            method(kPublic, "format",
                   "(Ljava/time/temporal/TemporalAccessor;)Ljava/lang/String;"),
        });
    }
    if (name == "java/time/LocalTime") {
        return make_class("java/time/LocalTime", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "hour", "I"),
            field(kPrivate | kFinal, "minute", "I"),
            field(kPrivate | kFinal, "second", "I"),
            field(kPrivate | kFinal, "nano", "I"),
        }, {
            method(kPublic | kStatic, "now", "()Ljava/time/LocalTime;"),
            method(kPublic | kStatic, "of", "(II)Ljava/time/LocalTime;"),
            method(kPublic, "withSecond", "(I)Ljava/time/LocalTime;"),
            method(kPublic, "withNano", "(I)Ljava/time/LocalTime;"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        });
    }
    if (name == "java/util/Vector") {
        return make_class("java/util/Vector", "java/lang/Object", kOrdinary, {
            field(kProtected, "elementData", "[Ljava/lang/Object;"),
            field(kProtected, "elementCount", "I"),
            field(kProtected, "capacityIncrement", "I"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(I)V"),
            method(kPublic, "<init>", "(II)V"),
            method(kPublic | kSynchronized, "size", "()I"),
            method(kPublic | kSynchronized, "capacity", "()I"),
            method(kPublic | kSynchronized, "ensureCapacity", "(I)V"),
            method(kPublic | kSynchronized, "trimToSize", "()V"),
            method(kPublic | kSynchronized, "setSize", "(I)V"),
            method(kPublic | kSynchronized, "isEmpty", "()Z"),
            method(kPublic | kSynchronized, "copyInto", "([Ljava/lang/Object;)V"),
            method(kPublic | kSynchronized, "contains", "(Ljava/lang/Object;)Z"),
            method(kPublic | kSynchronized, "add", "(Ljava/lang/Object;)Z"),
            method(kPublic | kSynchronized, "add", "(ILjava/lang/Object;)V"),
            method(kPublic | kSynchronized, "remove", "(Ljava/lang/Object;)Z"),
            method(kPublic | kSynchronized, "remove", "(I)Ljava/lang/Object;"),
            method(kPublic | kSynchronized, "get", "(I)Ljava/lang/Object;"),
            method(kPublic | kSynchronized, "set",
                   "(ILjava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic | kSynchronized, "clear", "()V"),
            method(kPublic | kSynchronized, "indexOf", "(Ljava/lang/Object;)I"),
            method(kPublic | kSynchronized, "indexOf", "(Ljava/lang/Object;I)I"),
            method(kPublic | kSynchronized, "lastIndexOf", "(Ljava/lang/Object;)I"),
            method(kPublic | kSynchronized, "lastIndexOf", "(Ljava/lang/Object;I)I"),
            method(kPublic | kSynchronized, "elementAt", "(I)Ljava/lang/Object;"),
            method(kPublic | kSynchronized, "firstElement", "()Ljava/lang/Object;"),
            method(kPublic | kSynchronized, "lastElement", "()Ljava/lang/Object;"),
            method(kPublic | kSynchronized, "setElementAt", "(Ljava/lang/Object;I)V"),
            method(kPublic | kSynchronized, "removeElementAt", "(I)V"),
            method(kPublic | kSynchronized, "insertElementAt", "(Ljava/lang/Object;I)V"),
            method(kPublic | kSynchronized, "addElement", "(Ljava/lang/Object;)V"),
            method(kPublic | kSynchronized, "removeElement", "(Ljava/lang/Object;)Z"),
            method(kPublic | kSynchronized, "removeAllElements", "()V"),
            method(kPublic | kSynchronized, "elements", "()Ljava/util/Enumeration;"),
            method(kPublic | kSynchronized, "toString", "()Ljava/lang/String;"),
        }, {"java/util/List"});
    }
    if (name == "java/util/Stack") {
        return make_class("java/util/Stack", "java/util/Vector", kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "push", "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic | kSynchronized, "pop", "()Ljava/lang/Object;"),
            method(kPublic | kSynchronized, "peek", "()Ljava/lang/Object;"),
            method(kPublic, "empty", "()Z"),
            method(kPublic | kSynchronized, "search", "(Ljava/lang/Object;)I"),
        });
    }
    if (name == "java/util/Hashtable") {
        return make_class("java/util/Hashtable", "java/lang/Object", kOrdinary, {
            field(kPrivate, "keys", "[Ljava/lang/Object;"),
            field(kPrivate, "values", "[Ljava/lang/Object;"),
            field(kPrivate, "count", "I"),
            field(kPrivate, "hashes", "[I"),
            field(kPrivate, "buckets", "[I"),
            field(kPrivate, "iterationOrder", "[I"),
            field(kPrivate, "tableCapacity", "I"),
            field(kPrivate, "threshold", "I"),
            field(kPrivate, "loadFactor", "F"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(I)V"),
            method(kPublic, "<init>", "(IF)V"),
            method(kPublic | kSynchronized, "size", "()I"),
            method(kPublic | kSynchronized, "isEmpty", "()Z"),
            method(kPublic | kSynchronized, "contains", "(Ljava/lang/Object;)Z"),
            method(kPublic | kSynchronized, "containsKey", "(Ljava/lang/Object;)Z"),
            method(kPublic | kSynchronized, "get",
                   "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic | kSynchronized, "put",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic | kSynchronized, "remove",
                   "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic | kSynchronized, "clear", "()V"),
            method(kPublic | kSynchronized, "keys", "()Ljava/util/Enumeration;"),
            method(kPublic | kSynchronized, "elements", "()Ljava/util/Enumeration;"),
            method(kProtected, "rehash", "()V"),
            method(kPublic | kSynchronized, "toString", "()Ljava/lang/String;"),
        });
    }
    if (name == "java/util/Random") {
        return make_class("java/util/Random", "java/lang/Object", kOrdinary, {
            field(kPrivate, "seed", "J"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(J)V"),
            method(kPublic | kSynchronized, "setSeed", "(J)V"),
            method(kProtected | kSynchronized, "next", "(I)I"),
            method(kPublic, "nextInt", "()I"),
            method(kPublic, "nextInt", "(I)I"),
            method(kPublic, "nextLong", "()J"),
            method(kPublic, "nextBoolean", "()Z"),
            method(kPublic, "nextFloat", "()F"),
            method(kPublic, "nextDouble", "()D"),
        });
    }
    if (name == "java/util/concurrent/ThreadLocalRandom") {
        return make_class("java/util/concurrent/ThreadLocalRandom",
                          "java/util/Random", kOrdinary | kFinal, {
            field(kPrivate | kStatic | kFinal, "INSTANCE",
                  "Ljava/util/concurrent/ThreadLocalRandom;"),
        }, {
            method(kPrivate, "<init>", "()V"),
            method(kStatic, "<clinit>", "()V"),
            method(kPublic | kStatic, "current",
                   "()Ljava/util/concurrent/ThreadLocalRandom;"),
            method(kPublic, "nextInt", "(II)I"),
            method(kPublic, "nextLong", "(J)J"),
            method(kPublic, "nextLong", "(JJ)J"),
            method(kPublic, "nextDouble", "(D)D"),
            method(kPublic, "nextDouble", "(DD)D"),
        });
    }

    struct Hierarchy final {
        const char* name;
        const char* super_name;
    };
    static constexpr std::array<Hierarchy, 3> hierarchy {{
        {"java/util/NoSuchElementException", "java/lang/RuntimeException"},
        {"java/util/EmptyStackException", "java/lang/RuntimeException"},
        {"java/time/DateTimeException", "java/lang/RuntimeException"},
    }};
    for (const Hierarchy& entry : hierarchy) {
        if (name == entry.name) {
            return make_class(entry.name, entry.super_name, kOrdinary, {}, {
                method(kPublic, "<init>", "()V"),
                method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            });
        }
    }

    return nullptr;
}

} // namespace

void register_util_classes(BuiltinClassRegistry& registry) {
    registry.add_factory(build_util_class);
}

} // namespace phoneme::vm
