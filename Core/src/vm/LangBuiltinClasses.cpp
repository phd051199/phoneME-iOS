#include "phoneme/vm/BuiltinClassRegistry.hpp"

#include <array>
#include <string>

#include "BuiltinClassSupport.hpp"

namespace phoneme::vm {
namespace {

using namespace builtin;

[[nodiscard]] ClassPtr build_lang_class(std::string_view name) {
    if (name == "java/lang/Object") {
        return make_class("java/lang/Object", "", kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic | kFinal, "getClass", "()Ljava/lang/Class;"),
            method(kPublic, "clone", "()Ljava/lang/Object;"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic | kFinal, "wait", "()V"),
            method(kPublic | kFinal, "wait", "(J)V"),
            method(kPublic | kFinal, "wait", "(JI)V"),
            method(kPublic | kFinal, "notify", "()V"),
            method(kPublic | kFinal, "notifyAll", "()V"),
        });
    }
    if (name == "java/lang/Class") {
        return make_class("java/lang/Class", "java/lang/Object",
                          kOrdinary | kFinal, {}, {
            method(kPublic, "getName", "()Ljava/lang/String;"),
            method(kPublic, "getSimpleName", "()Ljava/lang/String;"),
            method(kPublic, "newInstance", "()Ljava/lang/Object;"),
            method(kPublic, "isInterface", "()Z"),
            method(kPublic, "isArray", "()Z"),
            method(kPublic, "isInstance", "(Ljava/lang/Object;)Z"),
            method(kPublic, "isAssignableFrom", "(Ljava/lang/Class;)Z"),
            method(kPublic, "getSuperclass", "()Ljava/lang/Class;"),
            method(kPublic, "getComponentType", "()Ljava/lang/Class;"),
            method(kPublic, "getClassLoader", "()Ljava/lang/ClassLoader;"),
            method(kPublic, "desiredAssertionStatus", "()Z"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic, "getResourceAsStream",
                   "(Ljava/lang/String;)Ljava/io/InputStream;"),
            method(kPublic, "getResource",
                   "(Ljava/lang/String;)Ljava/net/URL;"),
            method(kPublic, "getDeclaredField",
                   "(Ljava/lang/String;)Ljava/lang/reflect/Field;"),
            method(kPublic, "getMethod",
                   "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;"),
            method(kPublic | kStatic, "forName",
                   "(Ljava/lang/String;)Ljava/lang/Class;"),
        });
    }
    if (name == "java/lang/ClassLoader") {
        return make_class("java/lang/ClassLoader", "java/lang/Object",
                          kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "getResourceAsStream",
                   "(Ljava/lang/String;)Ljava/io/InputStream;"),
        });
    }
    if (name == "java/lang/CharSequence") {
        return make_class("java/lang/CharSequence", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "length", "()I"),
            method(kPublic | kAbstract, "charAt", "(I)C"),
            method(kPublic | kAbstract, "subSequence",
                   "(II)Ljava/lang/CharSequence;"),
            method(kPublic | kAbstract, "toString", "()Ljava/lang/String;"),
        });
    }
    if (name == "java/lang/String") {
        return make_class("java/lang/String", "java/lang/Object",
                          kOrdinary | kFinal, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            method(kPublic, "<init>", "(Ljava/lang/StringBuffer;)V"),
            method(kPublic, "<init>", "([C)V"),
            method(kPublic, "<init>", "([CII)V"),
            method(kPublic, "<init>", "([B)V"),
            method(kPublic, "<init>", "([BII)V"),
            method(kPublic, "<init>", "([BLjava/lang/String;)V"),
            method(kPublic, "<init>", "([BIILjava/lang/String;)V"),
            method(kPublic, "<init>", "([BLjava/nio/charset/Charset;)V"),
            method(kPublic, "<init>", "([BIILjava/nio/charset/Charset;)V"),
            method(kPublic, "length", "()I"),
            method(kPublic, "isEmpty", "()Z"),
            method(kPublic, "isBlank", "()Z"),
            method(kPublic, "charAt", "(I)C"),
            method(kPublic, "getChars", "(II[CI)V"),
            method(kPublic, "toCharArray", "()[C"),
            method(kPublic, "getBytes", "()[B"),
            method(kPublic, "getBytes", "(Ljava/lang/String;)[B"),
            method(kPublic, "getBytes", "(Ljava/nio/charset/Charset;)[B"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "equalsIgnoreCase", "(Ljava/lang/String;)Z"),
            method(kPublic, "regionMatches",
                   "(ZILjava/lang/String;II)Z"),
            method(kPublic, "compareTo", "(Ljava/lang/String;)I"),
            method(kPublic, "compareTo", "(Ljava/lang/Object;)I"),
            method(kPublic, "startsWith", "(Ljava/lang/String;)Z"),
            method(kPublic, "startsWith", "(Ljava/lang/String;I)Z"),
            method(kPublic, "endsWith", "(Ljava/lang/String;)Z"),
            method(kPublic, "indexOf", "(I)I"),
            method(kPublic, "indexOf", "(II)I"),
            method(kPublic, "indexOf", "(Ljava/lang/String;)I"),
            method(kPublic, "indexOf", "(Ljava/lang/String;I)I"),
            method(kPublic, "lastIndexOf", "(I)I"),
            method(kPublic, "lastIndexOf", "(II)I"),
            method(kPublic, "lastIndexOf", "(Ljava/lang/String;)I"),
            method(kPublic, "lastIndexOf", "(Ljava/lang/String;I)I"),
            method(kPublic, "substring", "(I)Ljava/lang/String;"),
            method(kPublic, "substring", "(II)Ljava/lang/String;"),
            method(kPublic, "subSequence",
                   "(II)Ljava/lang/CharSequence;"),
            method(kPublic, "concat", "(Ljava/lang/String;)Ljava/lang/String;"),
            method(kPublic, "contains", "(Ljava/lang/CharSequence;)Z"),
            method(kPublic, "replace", "(CC)Ljava/lang/String;"),
            method(kPublic, "replace",
                   "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;"),
            method(kPublic, "replaceAll",
                   "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
            method(kPublic, "trim", "()Ljava/lang/String;"),
            method(kPublic, "toLowerCase", "()Ljava/lang/String;"),
            method(kPublic, "toLowerCase",
                   "(Ljava/util/Locale;)Ljava/lang/String;"),
            method(kPublic, "toUpperCase", "()Ljava/lang/String;"),
            method(kPublic, "toUpperCase",
                   "(Ljava/util/Locale;)Ljava/lang/String;"),
            method(kPublic, "split",
                   "(Ljava/lang/String;)[Ljava/lang/String;"),
            method(kPublic, "split",
                   "(Ljava/lang/String;I)[Ljava/lang/String;"),
            method(kPublic, "matches", "(Ljava/lang/String;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "intern", "()Ljava/lang/String;"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic | kStatic, "format",
                   "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf",
                   "(Ljava/lang/Object;)Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "([C)Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "([CII)Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "(Z)Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "(C)Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "(I)Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "(J)Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "(F)Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "(D)Ljava/lang/String;"),
        }, {"java/lang/Comparable", "java/io/Serializable",
             "java/lang/CharSequence"});
    }
    if (name == "java/lang/StringBuilder") {
        return make_class("java/lang/StringBuilder", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate, "capacity", "I"),
        }, text_builder_methods("java/lang/StringBuilder", false),
        {"java/lang/CharSequence"});
    }
    if (name == "java/lang/StringBuffer") {
        return make_class("java/lang/StringBuffer", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate, "capacity", "I"),
        }, text_builder_methods("java/lang/StringBuffer", true),
        {"java/lang/CharSequence"});
    }
    if (name == "java/lang/System") {
        return make_class("java/lang/System", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPublic | kStatic | kFinal, "in", "Ljava/io/InputStream;"),
            field(kPublic | kStatic | kFinal, "out", "Ljava/io/PrintStream;"),
            field(kPublic | kStatic | kFinal, "err", "Ljava/io/PrintStream;"),
        }, {
            method(kPublic | kStatic, "currentTimeMillis", "()J"),
            method(kPublic | kStatic, "nanoTime", "()J"),
            method(kPublic | kStatic, "identityHashCode", "(Ljava/lang/Object;)I"),
            method(kPublic | kStatic, "arraycopy",
                   "(Ljava/lang/Object;ILjava/lang/Object;II)V"),
            method(kPublic | kStatic, "getProperty",
                   "(Ljava/lang/String;)Ljava/lang/String;"),
            method(kPublic | kStatic, "getProperty",
                   "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
            method(kPublic | kStatic, "getenv",
                   "(Ljava/lang/String;)Ljava/lang/String;"),
            method(kPublic | kStatic, "lineSeparator", "()Ljava/lang/String;"),
            method(kPublic | kStatic, "gc", "()V"),
            method(kPublic | kStatic, "exit", "(I)V"),
            method(kPublic | kStatic, "linkLegacyWtChain",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Z"),
        });
    }
    if (name == "java/lang/Runtime") {
        return make_class("java/lang/Runtime", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kStatic | kFinal, "currentRuntime",
                  "Ljava/lang/Runtime;"),
        }, {
            method(kPrivate, "<init>", "()V"),
            method(kPublic | kStatic, "getRuntime", "()Ljava/lang/Runtime;"),
            method(kPublic, "totalMemory", "()J"),
            method(kPublic, "freeMemory", "()J"),
            method(kPublic, "maxMemory", "()J"),
            method(kPublic, "gc", "()V"),
            method(kPublic, "exit", "(I)V"),
        });
    }
    if (name == "java/lang/Thread") {
        return make_class("java/lang/Thread", "java/lang/Object", kOrdinary,
                          {
            field(kPrivate, "name", "Ljava/lang/String;"),
            field(kPrivate, "daemon", "Z"),
            field(kPublic | kStatic | kFinal, "MIN_PRIORITY", "I"),
            field(kPublic | kStatic | kFinal, "NORM_PRIORITY", "I"),
            field(kPublic | kStatic | kFinal, "MAX_PRIORITY", "I"),
                          }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            method(kPublic, "<init>", "(Ljava/lang/Runnable;)V"),
            method(kPublic, "<init>",
                   "(Ljava/lang/Runnable;Ljava/lang/String;)V"),
            method(kPublic, "start", "()V"),
            method(kPublic, "run", "()V"),
            method(kPublic | kStatic, "currentThread", "()Ljava/lang/Thread;"),
            method(kPublic | kStatic, "activeCount", "()I"),
            method(kPublic | kStatic, "yield", "()V"),
            method(kPublic | kStatic, "sleep", "(J)V"),
            method(kPublic | kFinal, "join", "()V"),
            method(kPublic | kFinal, "join", "(J)V"),
            method(kPublic | kFinal, "isAlive", "()Z"),
            method(kPublic, "interrupt", "()V"),
            method(kPublic | kStatic, "interrupted", "()Z"),
            method(kPublic, "isInterrupted", "()Z"),
            method(kPublic | kFinal, "setDaemon", "(Z)V"),
            method(kPublic | kFinal, "isDaemon", "()Z"),
            method(kPublic | kFinal, "setPriority", "(I)V"),
            method(kPublic | kFinal, "getPriority", "()I"),
            method(kPublic | kFinal, "getName", "()Ljava/lang/String;"),
            method(kPublic | kFinal, "checkAccess", "()V"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/lang/Runnable"});
    }
    if (name == "java/lang/Number") {
        return make_class("java/lang/Number", "java/lang/Object",
                          kOrdinary | kAbstract, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic | kAbstract, "intValue", "()I"),
            method(kPublic | kAbstract, "longValue", "()J"),
            method(kPublic | kAbstract, "floatValue", "()F"),
            method(kPublic | kAbstract, "doubleValue", "()D"),
        });
    }
    if (name == "java/lang/Boolean") {
        return make_class("java/lang/Boolean", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "value", "Z"),
            field(kPublic | kStatic | kFinal, "TRUE", "Ljava/lang/Boolean;"),
            field(kPublic | kStatic | kFinal, "FALSE", "Ljava/lang/Boolean;"),
            field(kPublic | kStatic | kFinal, "TYPE", "Ljava/lang/Class;"),
        }, {
            method(kPublic, "<init>", "(Z)V"),
            method(kPublic, "booleanValue", "()Z"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "(Z)Ljava/lang/Boolean;"),
            method(kPublic | kStatic, "valueOf",
                   "(Ljava/lang/String;)Ljava/lang/Boolean;"),
            method(kPublic | kStatic, "parseBoolean", "(Ljava/lang/String;)Z"),
            method(kPublic | kStatic, "getBoolean", "(Ljava/lang/String;)Z"),
            method(kPublic | kStatic, "toString", "(Z)Ljava/lang/String;"),
        });
    }

    const auto number_wrapper = [&](std::string class_name,
                                    std::string super_name,
                                    const char* value_descriptor,
                                    std::vector<classfile::Method> methods) {
        std::vector<classfile::Field> fields {
            field(kPrivate | kFinal, "value", value_descriptor),
            field(kPublic | kStatic | kFinal, "TYPE", "Ljava/lang/Class;"),
        };
        if (class_name == "java/lang/Byte") {
            fields.push_back(field(kPublic | kStatic | kFinal, "MIN_VALUE", "B"));
            fields.push_back(field(kPublic | kStatic | kFinal, "MAX_VALUE", "B"));
        } else if (class_name == "java/lang/Short") {
            fields.push_back(field(kPublic | kStatic | kFinal, "MIN_VALUE", "S"));
            fields.push_back(field(kPublic | kStatic | kFinal, "MAX_VALUE", "S"));
        } else if (class_name == "java/lang/Integer") {
            fields.push_back(field(kPublic | kStatic | kFinal, "MIN_VALUE", "I"));
            fields.push_back(field(kPublic | kStatic | kFinal, "MAX_VALUE", "I"));
        } else if (class_name == "java/lang/Long") {
            fields.push_back(field(kPublic | kStatic | kFinal, "MIN_VALUE", "J"));
            fields.push_back(field(kPublic | kStatic | kFinal, "MAX_VALUE", "J"));
        } else if (class_name == "java/lang/Float") {
            fields.push_back(field(kPublic | kStatic | kFinal, "POSITIVE_INFINITY", "F"));
            fields.push_back(field(kPublic | kStatic | kFinal, "NEGATIVE_INFINITY", "F"));
            fields.push_back(field(kPublic | kStatic | kFinal, "NaN", "F"));
            fields.push_back(field(kPublic | kStatic | kFinal, "MAX_VALUE", "F"));
            fields.push_back(field(kPublic | kStatic | kFinal, "MIN_VALUE", "F"));
        } else if (class_name == "java/lang/Double") {
            fields.push_back(field(kPublic | kStatic | kFinal, "POSITIVE_INFINITY", "D"));
            fields.push_back(field(kPublic | kStatic | kFinal, "NEGATIVE_INFINITY", "D"));
            fields.push_back(field(kPublic | kStatic | kFinal, "NaN", "D"));
            fields.push_back(field(kPublic | kStatic | kFinal, "MAX_VALUE", "D"));
            fields.push_back(field(kPublic | kStatic | kFinal, "MIN_VALUE", "D"));
        }
        std::vector<std::string> interfaces;
        if (class_name == "java/lang/Integer" ||
            class_name == "java/lang/Long") {
            interfaces.push_back("java/lang/Comparable");
        }
        interfaces.push_back("java/io/Serializable");
        return make_class(std::move(class_name), std::move(super_name),
                          kOrdinary | kFinal, std::move(fields),
                          std::move(methods), std::move(interfaces));
    };
    if (name == "java/lang/Byte") {
        return number_wrapper("java/lang/Byte", "java/lang/Object", "B", {
            method(kPublic, "<init>", "(B)V"),
            method(kPublic, "byteValue", "()B"),
            method(kPublic, "shortValue", "()S"),
            method(kPublic, "intValue", "()I"),
            method(kPublic, "longValue", "()J"),
            method(kPublic, "floatValue", "()F"),
            method(kPublic, "doubleValue", "()D"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "(B)Ljava/lang/Byte;"),
            method(kPublic | kStatic, "parseByte", "(Ljava/lang/String;)B"),
            method(kPublic | kStatic, "parseByte", "(Ljava/lang/String;I)B"),
            method(kPublic | kStatic, "toString", "(B)Ljava/lang/String;"),
        });
    }
    if (name == "java/lang/Short") {
        return number_wrapper("java/lang/Short", "java/lang/Object", "S", {
            method(kPublic, "<init>", "(S)V"),
            method(kPublic, "byteValue", "()B"),
            method(kPublic, "shortValue", "()S"),
            method(kPublic, "intValue", "()I"),
            method(kPublic, "longValue", "()J"),
            method(kPublic, "floatValue", "()F"),
            method(kPublic, "doubleValue", "()D"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "(S)Ljava/lang/Short;"),
            method(kPublic | kStatic, "parseShort", "(Ljava/lang/String;)S"),
            method(kPublic | kStatic, "parseShort", "(Ljava/lang/String;I)S"),
            method(kPublic | kStatic, "toString", "(S)Ljava/lang/String;"),
        });
    }
    if (name == "java/lang/Integer") {
        return number_wrapper("java/lang/Integer", "java/lang/Object", "I", {
            method(kPublic, "<init>", "(I)V"),
            method(kPublic, "byteValue", "()B"),
            method(kPublic, "shortValue", "()S"),
            method(kPublic, "intValue", "()I"),
            method(kPublic, "longValue", "()J"),
            method(kPublic, "floatValue", "()F"),
            method(kPublic, "doubleValue", "()D"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic, "compareTo", "(Ljava/lang/Integer;)I"),
            method(kPublic, "compareTo", "(Ljava/lang/Object;)I"),
            method(kPublic | kStatic, "valueOf", "(I)Ljava/lang/Integer;"),
            method(kPublic | kStatic, "valueOf",
                   "(Ljava/lang/String;)Ljava/lang/Integer;"),
            method(kPublic | kStatic, "valueOf",
                   "(Ljava/lang/String;I)Ljava/lang/Integer;"),
            method(kPublic | kStatic, "parseInt", "(Ljava/lang/String;)I"),
            method(kPublic | kStatic, "parseInt", "(Ljava/lang/String;I)I"),
            method(kPublic | kStatic, "toString", "(I)Ljava/lang/String;"),
            method(kPublic | kStatic, "toString", "(II)Ljava/lang/String;"),
            method(kPublic | kStatic, "toHexString", "(I)Ljava/lang/String;"),
            method(kPublic | kStatic, "toOctalString", "(I)Ljava/lang/String;"),
            method(kPublic | kStatic, "toBinaryString", "(I)Ljava/lang/String;"),
            method(kPublic | kStatic, "compare", "(II)I"),
            method(kPublic | kStatic, "sum", "(II)I"),
            method(kPublic | kStatic, "bitCount", "(I)I"),
        });
    }
    if (name == "java/lang/Long") {
        return number_wrapper("java/lang/Long", "java/lang/Object", "J", {
            method(kPublic, "<init>", "(J)V"),
            method(kPublic, "byteValue", "()B"),
            method(kPublic, "shortValue", "()S"),
            method(kPublic, "intValue", "()I"),
            method(kPublic, "longValue", "()J"),
            method(kPublic, "floatValue", "()F"),
            method(kPublic, "doubleValue", "()D"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic, "compareTo", "(Ljava/lang/Long;)I"),
            method(kPublic, "compareTo", "(Ljava/lang/Object;)I"),
            method(kPublic | kStatic, "valueOf", "(J)Ljava/lang/Long;"),
            method(kPublic | kStatic, "valueOf",
                   "(Ljava/lang/String;)Ljava/lang/Long;"),
            method(kPublic | kStatic, "parseLong", "(Ljava/lang/String;)J"),
            method(kPublic | kStatic, "parseLong", "(Ljava/lang/String;I)J"),
            method(kPublic | kStatic, "toString", "(J)Ljava/lang/String;"),
            method(kPublic | kStatic, "toString", "(JI)Ljava/lang/String;"),
            method(kPublic | kStatic, "toHexString", "(J)Ljava/lang/String;"),
            method(kPublic | kStatic, "toOctalString", "(J)Ljava/lang/String;"),
            method(kPublic | kStatic, "toBinaryString", "(J)Ljava/lang/String;"),
            method(kPublic | kStatic, "sum", "(JJ)J"),
            method(kPublic | kStatic, "remainderUnsigned", "(JJ)J"),
        });
    }
    if (name == "java/lang/Character") {
        return make_class("java/lang/Character", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "value", "C"),
            field(kPublic | kStatic | kFinal, "TYPE", "Ljava/lang/Class;"),
            field(kPublic | kStatic | kFinal, "MIN_RADIX", "I"),
            field(kPublic | kStatic | kFinal, "MAX_RADIX", "I"),
            field(kPublic | kStatic | kFinal, "MIN_VALUE", "C"),
            field(kPublic | kStatic | kFinal, "MAX_VALUE", "C"),
        }, {
            method(kPublic, "<init>", "(C)V"),
            method(kPublic, "charValue", "()C"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "(C)Ljava/lang/Character;"),
            method(kPublic | kStatic, "isDigit", "(C)Z"),
            method(kPublic | kStatic, "isLetter", "(C)Z"),
            method(kPublic | kStatic, "isLetterOrDigit", "(C)Z"),
            method(kPublic | kStatic, "isWhitespace", "(C)Z"),
            method(kPublic | kStatic, "isLowerCase", "(C)Z"),
            method(kPublic | kStatic, "isUpperCase", "(C)Z"),
            method(kPublic | kStatic, "digit", "(CI)I"),
            method(kPublic | kStatic, "toLowerCase", "(C)C"),
            method(kPublic | kStatic, "toUpperCase", "(C)C"),
        });
    }
    if (name == "java/lang/Float") {
        return number_wrapper("java/lang/Float", "java/lang/Object", "F", {
            method(kPublic, "<init>", "(F)V"),
            method(kPublic, "<init>", "(D)V"),
            method(kPublic, "byteValue", "()B"),
            method(kPublic, "shortValue", "()S"),
            method(kPublic, "intValue", "()I"),
            method(kPublic, "longValue", "()J"),
            method(kPublic, "floatValue", "()F"),
            method(kPublic, "doubleValue", "()D"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "(F)Ljava/lang/Float;"),
            method(kPublic | kStatic, "valueOf", "(Ljava/lang/String;)Ljava/lang/Float;"),
            method(kPublic | kStatic, "parseFloat", "(Ljava/lang/String;)F"),
            method(kPublic, "isNaN", "()Z"),
            method(kPublic | kStatic, "isNaN", "(F)Z"),
            method(kPublic, "isInfinite", "()Z"),
            method(kPublic | kStatic, "isInfinite", "(F)Z"),
            method(kPublic | kStatic, "toString", "(F)Ljava/lang/String;"),
            method(kPublic | kStatic, "floatToIntBits", "(F)I"),
            method(kPublic | kStatic, "intBitsToFloat", "(I)F"),
        });
    }
    if (name == "java/lang/Double") {
        return number_wrapper("java/lang/Double", "java/lang/Object", "D", {
            method(kPublic, "<init>", "(D)V"),
            method(kPublic, "byteValue", "()B"),
            method(kPublic, "shortValue", "()S"),
            method(kPublic, "intValue", "()I"),
            method(kPublic, "longValue", "()J"),
            method(kPublic, "floatValue", "()F"),
            method(kPublic, "doubleValue", "()D"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic | kStatic, "valueOf", "(D)Ljava/lang/Double;"),
            method(kPublic | kStatic, "valueOf", "(Ljava/lang/String;)Ljava/lang/Double;"),
            method(kPublic | kStatic, "parseDouble", "(Ljava/lang/String;)D"),
            method(kPublic, "isNaN", "()Z"),
            method(kPublic | kStatic, "isNaN", "(D)Z"),
            method(kPublic, "isInfinite", "()Z"),
            method(kPublic | kStatic, "isInfinite", "(D)Z"),
            method(kPublic | kStatic, "toString", "(D)Ljava/lang/String;"),
            method(kPublic | kStatic, "doubleToLongBits", "(D)J"),
            method(kPublic | kStatic, "longBitsToDouble", "(J)D"),
        });
    }
    if (name == "java/lang/Void") {
        return make_class("java/lang/Void", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPublic | kStatic | kFinal, "TYPE", "Ljava/lang/Class;"),
        }, {method(kPrivate, "<init>", "()V")});
    }
    if (name == "java/lang/Math") {
        return make_class("java/lang/Math", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPublic | kStatic | kFinal, "E", "D"),
            field(kPublic | kStatic | kFinal, "PI", "D"),
        }, {
            method(kPublic | kStatic, "abs", "(I)I"),
            method(kPublic | kStatic, "abs", "(J)J"),
            method(kPublic | kStatic, "abs", "(F)F"),
            method(kPublic | kStatic, "abs", "(D)D"),
            method(kPublic | kStatic, "min", "(II)I"),
            method(kPublic | kStatic, "min", "(JJ)J"),
            method(kPublic | kStatic, "min", "(FF)F"),
            method(kPublic | kStatic, "min", "(DD)D"),
            method(kPublic | kStatic, "max", "(II)I"),
            method(kPublic | kStatic, "max", "(JJ)J"),
            method(kPublic | kStatic, "max", "(FF)F"),
            method(kPublic | kStatic, "max", "(DD)D"),
            method(kPublic | kStatic, "sin", "(D)D"),
            method(kPublic | kStatic, "cos", "(D)D"),
            method(kPublic | kStatic, "tan", "(D)D"),
            method(kPublic | kStatic, "asin", "(D)D"),
            method(kPublic | kStatic, "acos", "(D)D"),
            method(kPublic | kStatic, "atan", "(D)D"),
            method(kPublic | kStatic, "atan2", "(DD)D"),
            method(kPublic | kStatic, "exp", "(D)D"),
            method(kPublic | kStatic, "log", "(D)D"),
            method(kPublic | kStatic, "sqrt", "(D)D"),
            method(kPublic | kStatic, "ceil", "(D)D"),
            method(kPublic | kStatic, "floor", "(D)D"),
            method(kPublic | kStatic, "pow", "(DD)D"),
            method(kPublic | kStatic, "IEEEremainder", "(DD)D"),
            method(kPublic | kStatic, "rint", "(D)D"),
            method(kPublic | kStatic, "round", "(F)I"),
            method(kPublic | kStatic, "round", "(D)J"),
            method(kPublic | kStatic, "floorDiv", "(II)I"),
            method(kPublic | kStatic, "floorMod", "(II)I"),
            method(kPublic | kStatic, "floorMod", "(JJ)J"),
            method(kPublic | kStatic, "addExact", "(II)I"),
            method(kPublic | kStatic, "multiplyExact", "(II)I"),
            method(kPublic | kStatic, "toDegrees", "(D)D"),
            method(kPublic | kStatic, "toRadians", "(D)D"),
            method(kPublic | kStatic, "random", "()D"),
        });
    }
    if (name == "java/lang/ref/Reference") {
        return make_class("java/lang/ref/Reference", "java/lang/Object",
                          kOrdinary | kAbstract, {}, {
            method(0U, "<init>", "(Ljava/lang/Object;)V"),
            method(kPublic, "get", "()Ljava/lang/Object;"),
            method(kPublic, "clear", "()V"),
        });
    }
    if (name == "java/lang/ref/WeakReference") {
        return make_class("java/lang/ref/WeakReference",
                          "java/lang/ref/Reference", kOrdinary, {}, {
            method(kPublic, "<init>", "(Ljava/lang/Object;)V"),
            method(kPublic, "get", "()Ljava/lang/Object;"),
            method(kPublic, "clear", "()V"),
        });
    }
    if (name == "java/lang/Runnable") {
        return make_class("java/lang/Runnable", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "run", "()V"),
        });
    }
    if (name == "java/lang/Cloneable") {
        return make_class("java/lang/Cloneable", "java/lang/Object",
                          kPublic | kInterface | kAbstract);
    }
    if (name == "java/lang/Throwable") {
        return make_class("java/lang/Throwable", "java/lang/Object",
                          kOrdinary, {
            field(kPrivate, "detailMessage", "Ljava/lang/String;"),
            field(kPrivate, "cause", "Ljava/lang/Throwable;"),
            field(kPrivate, "causeInitialized", "Z"),
            field(kPrivate, "suppressedExceptions", "[Ljava/lang/Throwable;"),
            field(kPrivate, "suppressedCount", "I"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            method(kPublic, "<init>",
                   "(Ljava/lang/String;Ljava/lang/Throwable;)V"),
            method(kPublic, "<init>", "(Ljava/lang/Throwable;)V"),
            method(kPublic, "getMessage", "()Ljava/lang/String;"),
            method(kPublic, "getLocalizedMessage", "()Ljava/lang/String;"),
            method(kPublic, "getCause", "()Ljava/lang/Throwable;"),
            method(kPublic | kSynchronized, "initCause",
                   "(Ljava/lang/Throwable;)Ljava/lang/Throwable;"),
            method(kPublic | kFinal | kSynchronized, "addSuppressed",
                   "(Ljava/lang/Throwable;)V"),
            method(kPublic | kFinal | kSynchronized, "getSuppressed",
                   "()[Ljava/lang/Throwable;"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic, "printStackTrace", "()V"),
        });
    }

    if (name == "java/lang/ExceptionInInitializerError") {
        return make_class(name.data(), "java/lang/LinkageError", kOrdinary,
                          {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            method(kPublic, "<init>", "(Ljava/lang/Throwable;)V"),
            method(kPublic, "getException", "()Ljava/lang/Throwable;"),
        });
    }

    if (name == "java/lang/ArrayIndexOutOfBoundsException" ||
        name == "java/lang/StringIndexOutOfBoundsException") {
        return make_class(name.data(), "java/lang/IndexOutOfBoundsException",
                          kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            method(kPublic, "<init>", "(I)V"),
        });
    }

    struct Hierarchy final {
        const char* name;
        const char* super_name;
    };
    static constexpr std::array<Hierarchy, 27> hierarchy {{
        {"java/lang/Exception", "java/lang/Throwable"},
        {"java/lang/RuntimeException", "java/lang/Exception"},
        {"java/lang/Error", "java/lang/Throwable"},
        {"java/lang/VirtualMachineError", "java/lang/Error"},
        {"java/lang/LinkageError", "java/lang/Error"},
        {"java/lang/BootstrapMethodError", "java/lang/LinkageError"},
        {"java/lang/NoClassDefFoundError", "java/lang/Error"},
        {"java/lang/ClassFormatError", "java/lang/LinkageError"},
        {"java/lang/VerifyError", "java/lang/LinkageError"},
        {"java/lang/IncompatibleClassChangeError", "java/lang/LinkageError"},
        {"java/lang/NoSuchMethodError", "java/lang/IncompatibleClassChangeError"},
        {"java/lang/NoSuchFieldError", "java/lang/IncompatibleClassChangeError"},
        {"java/lang/AbstractMethodError", "java/lang/IncompatibleClassChangeError"},
        {"java/lang/InstantiationError", "java/lang/IncompatibleClassChangeError"},
        {"java/lang/UnsatisfiedLinkError", "java/lang/LinkageError"},
        {"java/lang/NullPointerException", "java/lang/RuntimeException"},
        {"java/lang/ArithmeticException", "java/lang/RuntimeException"},
        {"java/lang/ArrayIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException"},
        {"java/lang/NegativeArraySizeException", "java/lang/RuntimeException"},
        {"java/lang/ArrayStoreException", "java/lang/RuntimeException"},
        {"java/lang/IllegalMonitorStateException", "java/lang/RuntimeException"},
        {"java/lang/ClassCastException", "java/lang/RuntimeException"},
        {"java/lang/IllegalArgumentException", "java/lang/RuntimeException"},
        {"java/lang/IllegalStateException", "java/lang/RuntimeException"},
        {"java/lang/OutOfMemoryError", "java/lang/VirtualMachineError"},
        {"java/lang/StackOverflowError", "java/lang/VirtualMachineError"},
        {"java/lang/InterruptedException", "java/lang/Exception"},
    }};
    for (const Hierarchy& entry : hierarchy) {
        if (name == entry.name) {
            return make_class(entry.name, entry.super_name, kOrdinary, {}, {
                method(kPublic, "<init>", "()V"),
                method(kPublic, "<init>", "(Ljava/lang/String;)V"),
                method(kPublic, "<init>", "(Ljava/lang/Throwable;)V"),
                method(kPublic, "<init>",
                       "(Ljava/lang/String;Ljava/lang/Throwable;)V"),
            });
        }
    }

    static constexpr std::array<Hierarchy, 10> secondary_hierarchy {{
        {"java/lang/NumberFormatException", "java/lang/IllegalArgumentException"},
        {"java/lang/ClassNotFoundException", "java/lang/Exception"},
        {"java/lang/InstantiationException", "java/lang/Exception"},
        {"java/lang/IllegalAccessException", "java/lang/Exception"},
        {"java/lang/CloneNotSupportedException", "java/lang/Exception"},
        {"java/lang/IndexOutOfBoundsException", "java/lang/RuntimeException"},
        {"java/lang/StringIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException"},
        {"java/lang/UnsupportedOperationException", "java/lang/RuntimeException"},
        {"java/lang/SecurityException", "java/lang/RuntimeException"},
        {"java/lang/IllegalThreadStateException", "java/lang/IllegalArgumentException"},
    }};
    for (const Hierarchy& entry : secondary_hierarchy) {
        if (name == entry.name) {
            return make_class(entry.name, entry.super_name, kOrdinary, {}, {
                method(kPublic, "<init>", "()V"),
                method(kPublic, "<init>", "(Ljava/lang/String;)V"),
                method(kPublic, "<init>", "(Ljava/lang/Throwable;)V"),
                method(kPublic, "<init>",
                       "(Ljava/lang/String;Ljava/lang/Throwable;)V"),
            });
        }
    }

    return nullptr;
}

} // namespace

void register_lang_classes(BuiltinClassRegistry& registry) {
    registry.add_factory(build_lang_class);
}

} // namespace phoneme::vm
