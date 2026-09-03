#include "phoneme/vm/BuiltinClassRegistry.hpp"

#include <string_view>

#include "BuiltinClassSupport.hpp"

namespace phoneme::vm {
namespace {

using namespace builtin;

[[nodiscard]] ClassPtr build_jdk8_compat_class(std::string_view name) {
    if (name == "java/lang/Enum") {
        return make_class("java/lang/Enum", "java/lang/Object",
                          kPublic | kAbstract, {
            field(kPrivate | kFinal, "name", "Ljava/lang/String;"),
            field(kPrivate | kFinal, "ordinal", "I"),
        }, {
            method(kProtected, "<init>", "(Ljava/lang/String;I)V"),
            method(kPublic | kFinal, "name", "()Ljava/lang/String;"),
            method(kPublic | kFinal, "ordinal", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic | kFinal, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic | kFinal, "hashCode", "()I"),
            method(kPublic | kFinal, "compareTo", "(Ljava/lang/Enum;)I"),
            method(kPublic | kFinal, "compareTo", "(Ljava/lang/Object;)I"),
            method(kPublic | kFinal, "getDeclaringClass", "()Ljava/lang/Class;"),
            method(kPublic | kStatic, "valueOf",
                   "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;"),
        }, {"java/lang/Comparable", "java/io/Serializable"});
    }

    if (name == "java/util/function/Function") {
        return make_class("java/util/function/Function", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "apply",
                   "(Ljava/lang/Object;)Ljava/lang/Object;"),
        });
    }
    if (name == "java/util/function/BiFunction") {
        return make_class("java/util/function/BiFunction", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "apply",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
        });
    }
    if (name == "java/util/function/UnaryOperator") {
        return make_class("java/util/function/UnaryOperator", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "apply",
                   "(Ljava/lang/Object;)Ljava/lang/Object;"),
        }, {"java/util/function/Function"});
    }
    if (name == "java/util/function/BinaryOperator") {
        return make_class("java/util/function/BinaryOperator", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "apply",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
        }, {"java/util/function/BiFunction"});
    }
    if (name == "java/util/function/Consumer") {
        return make_class("java/util/function/Consumer", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "accept", "(Ljava/lang/Object;)V"),
        });
    }
    if (name == "java/util/function/BiConsumer") {
        return make_class("java/util/function/BiConsumer", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "accept",
                   "(Ljava/lang/Object;Ljava/lang/Object;)V"),
        });
    }
    if (name == "java/util/function/Supplier") {
        return make_class("java/util/function/Supplier", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "get", "()Ljava/lang/Object;"),
        });
    }
    if (name == "java/util/function/Predicate") {
        return make_class("java/util/function/Predicate", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "test", "(Ljava/lang/Object;)Z"),
        });
    }
    if (name == "java/util/function/IntPredicate") {
        return make_class("java/util/function/IntPredicate", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "test", "(I)Z"),
        });
    }
    if (name == "java/util/function/IntSupplier") {
        return make_class("java/util/function/IntSupplier", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "getAsInt", "()I"),
        });
    }
    if (name == "java/util/function/LongSupplier") {
        return make_class("java/util/function/LongSupplier", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "getAsLong", "()J"),
        });
    }
    if (name == "java/util/function/DoubleSupplier") {
        return make_class("java/util/function/DoubleSupplier", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "getAsDouble", "()D"),
        });
    }
    if (name == "java/util/function/IntConsumer") {
        return make_class("java/util/function/IntConsumer", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "accept", "(I)V"),
        });
    }
    if (name == "java/util/function/LongConsumer") {
        return make_class("java/util/function/LongConsumer", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "accept", "(J)V"),
        });
    }
    if (name == "java/util/function/DoubleConsumer") {
        return make_class("java/util/function/DoubleConsumer", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "accept", "(D)V"),
        });
    }
    if (name == "java/util/function/IntUnaryOperator") {
        return make_class("java/util/function/IntUnaryOperator",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "applyAsInt", "(I)I"),
            method(kPublic | kStatic, "identity",
                   "()Ljava/util/function/IntUnaryOperator;"),
        });
    }
    if (name == "java/util/function/IntBinaryOperator") {
        return make_class("java/util/function/IntBinaryOperator",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "applyAsInt", "(II)I"),
        });
    }
    if (name == "java/util/function/LongUnaryOperator") {
        return make_class("java/util/function/LongUnaryOperator",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "applyAsLong", "(J)J"),
        });
    }
    if (name == "java/util/function/LongBinaryOperator") {
        return make_class("java/util/function/LongBinaryOperator",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "applyAsLong", "(JJ)J"),
        });
    }
    if (name == "java/util/function/ToIntFunction") {
        return make_class("java/util/function/ToIntFunction", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "applyAsInt", "(Ljava/lang/Object;)I"),
        });
    }
    if (name == "java/util/function/NativeIntIdentity") {
        return make_class("java/util/function/NativeIntIdentity",
                          "java/lang/Object", kOrdinary | kFinal, {}, {
            method(kPublic, "applyAsInt", "(I)I"),
        }, {"java/util/function/IntUnaryOperator"});
    }

    if (name == "java/util/Comparator") {
        return make_class("java/util/Comparator", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "compare",
                   "(Ljava/lang/Object;Ljava/lang/Object;)I"),
            method(kPublic, "reversed", "()Ljava/util/Comparator;"),
            method(kPublic, "thenComparing",
                   "(Ljava/util/Comparator;)Ljava/util/Comparator;"),
            method(kPublic, "thenComparing",
                   "(Ljava/util/function/Function;)Ljava/util/Comparator;"),
            method(kPublic, "thenComparingInt",
                   "(Ljava/util/function/ToIntFunction;)Ljava/util/Comparator;"),
            method(kPublic | kStatic, "comparingInt",
                   "(Ljava/util/function/ToIntFunction;)Ljava/util/Comparator;"),
        });
    }
    if (name == "java/util/NativeComparator") {
        return make_class("java/util/NativeComparator", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate, "kind", "I"),
            field(kPrivate, "first", "Ljava/lang/Object;"),
            field(kPrivate, "second", "Ljava/lang/Object;"),
        }, {
            method(kPublic, "compare",
                   "(Ljava/lang/Object;Ljava/lang/Object;)I"),
        }, {"java/util/Comparator"});
    }

    if (name == "java/util/Map$Entry") {
        return make_class("java/util/Map$Entry", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "getKey", "()Ljava/lang/Object;"),
            method(kPublic | kAbstract, "getValue", "()Ljava/lang/Object;"),
            method(kPublic | kAbstract, "setValue",
                   "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic | kAbstract, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic | kAbstract, "hashCode", "()I"),
        });
    }
    if (name == "java/util/NativeMapEntry") {
        return make_class("java/util/NativeMapEntry", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate, "owner", "Ljava/util/Map;"),
            field(kPrivate, "key", "Ljava/lang/Object;"),
        }, {
            method(kPublic, "getKey", "()Ljava/lang/Object;"),
            method(kPublic, "getValue", "()Ljava/lang/Object;"),
            method(kPublic, "setValue",
                   "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/util/Map$Entry"});
    }

    if (name == "java/util/LinkedHashMap" ||
        name == "java/util/IdentityHashMap" ||
        name == "java/util/WeakHashMap") {
        const std::string class_name(name);
        return make_class(class_name, "java/lang/Object", kOrdinary, {
            field(kPrivate, "keys", "[Ljava/lang/Object;"),
            field(kPrivate, "values", "[Ljava/lang/Object;"),
            field(kPrivate, "size", "I"),
            field(kPrivate, "hashes", "[I"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(I)V"),
            method(kPublic, "<init>", "(IF)V"),
            method(kPublic, "<init>", "(Ljava/util/Map;)V"),
            method(kPublic, "size", "()I"),
            method(kPublic, "isEmpty", "()Z"),
            method(kPublic, "containsKey", "(Ljava/lang/Object;)Z"),
            method(kPublic, "containsValue", "(Ljava/lang/Object;)Z"),
            method(kPublic, "get", "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "getOrDefault",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "put",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "putIfAbsent",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "computeIfAbsent",
                   "(Ljava/lang/Object;Ljava/util/function/Function;)Ljava/lang/Object;"),
            method(kPublic, "merge",
                   "(Ljava/lang/Object;Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;"),
            method(kPublic, "remove",
                   "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "putAll", "(Ljava/util/Map;)V"),
            method(kPublic, "clear", "()V"),
            method(kPublic, "keySet", "()Ljava/util/Set;"),
            method(kPublic, "values", "()Ljava/util/Collection;"),
            method(kPublic, "entrySet", "()Ljava/util/Set;"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/util/Map", "java/lang/Cloneable", "java/io/Serializable"});
    }

    if (name == "java/util/LinkedHashSet") {
        return make_class("java/util/LinkedHashSet", "java/lang/Object",
                          kOrdinary, {
            field(kPrivate, "map", "Ljava/util/HashMap;"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(I)V"),
            method(kPublic, "<init>", "(Ljava/util/Collection;)V"),
            method(kPublic, "size", "()I"),
            method(kPublic, "isEmpty", "()Z"),
            method(kPublic, "contains", "(Ljava/lang/Object;)Z"),
            method(kPublic, "iterator", "()Ljava/util/Iterator;"),
            method(kPublic, "toArray", "()[Ljava/lang/Object;"),
            method(kPublic, "toArray",
                   "([Ljava/lang/Object;)[Ljava/lang/Object;"),
            method(kPublic, "add", "(Ljava/lang/Object;)Z"),
            method(kPublic, "remove", "(Ljava/lang/Object;)Z"),
            method(kPublic, "containsAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "addAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "removeAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "retainAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "clear", "()V"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/util/Set", "java/lang/Cloneable", "java/io/Serializable"});
    }

    if (name == "java/util/LinkedList") {
        return make_class("java/util/LinkedList", "java/lang/Object",
                          kOrdinary, {
            field(kPrivate, "elementData", "[Ljava/lang/Object;"),
            field(kPrivate, "size", "I"),
            field(kPrivate, "capacityIncrement", "I"),
            field(kPrivate, "mutationMode", "I"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/util/Collection;)V"),
            method(kPublic, "size", "()I"),
            method(kPublic, "isEmpty", "()Z"),
            method(kPublic, "contains", "(Ljava/lang/Object;)Z"),
            method(kPublic, "get", "(I)Ljava/lang/Object;"),
            method(kPublic, "set", "(ILjava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "add", "(Ljava/lang/Object;)Z"),
            method(kPublic, "add", "(ILjava/lang/Object;)V"),
            method(kPublic, "addFirst", "(Ljava/lang/Object;)V"),
            method(kPublic, "addLast", "(Ljava/lang/Object;)V"),
            method(kPublic, "offerFirst", "(Ljava/lang/Object;)Z"),
            method(kPublic, "offerLast", "(Ljava/lang/Object;)Z"),
            method(kPublic, "remove", "(I)Ljava/lang/Object;"),
            method(kPublic, "remove", "(Ljava/lang/Object;)Z"),
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
            method(kPublic, "offer", "(Ljava/lang/Object;)Z"),
            method(kPublic, "remove", "()Ljava/lang/Object;"),
            method(kPublic, "poll", "()Ljava/lang/Object;"),
            method(kPublic, "element", "()Ljava/lang/Object;"),
            method(kPublic, "peek", "()Ljava/lang/Object;"),
            method(kPublic, "push", "(Ljava/lang/Object;)V"),
            method(kPublic, "pop", "()Ljava/lang/Object;"),
            method(kPublic, "clear", "()V"),
            method(kPublic, "indexOf", "(Ljava/lang/Object;)I"),
            method(kPublic, "lastIndexOf", "(Ljava/lang/Object;)I"),
            method(kPublic, "iterator", "()Ljava/util/Iterator;"),
            method(kPublic, "toArray", "()[Ljava/lang/Object;"),
            method(kPublic, "toArray",
                   "([Ljava/lang/Object;)[Ljava/lang/Object;"),
            method(kPublic, "addAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/util/List", "java/util/Deque", "java/lang/Cloneable",
             "java/io/Serializable"});
    }

    if (name == "java/util/EnumMap") {
        return make_class("java/util/EnumMap", "java/lang/Object", kOrdinary, {
            field(kPrivate, "keys", "[Ljava/lang/Object;"),
            field(kPrivate, "values", "[Ljava/lang/Object;"),
            field(kPrivate, "size", "I"),
            field(kPrivate, "hashes", "[I"),
            field(kPrivate, "keyType", "Ljava/lang/Class;"),
        }, {
            method(kPublic, "<init>", "(Ljava/lang/Class;)V"),
            method(kPublic, "size", "()I"),
            method(kPublic, "isEmpty", "()Z"),
            method(kPublic, "containsKey", "(Ljava/lang/Object;)Z"),
            method(kPublic, "containsValue", "(Ljava/lang/Object;)Z"),
            method(kPublic, "get", "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "getOrDefault",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "put",
                   "(Ljava/lang/Enum;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "put",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "remove",
                   "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "putAll", "(Ljava/util/Map;)V"),
            method(kPublic, "clear", "()V"),
            method(kPublic, "keySet", "()Ljava/util/Set;"),
            method(kPublic, "values", "()Ljava/util/Collection;"),
            method(kPublic, "entrySet", "()Ljava/util/Set;"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/util/Map", "java/lang/Cloneable", "java/io/Serializable"});
    }

    if (name == "java/util/EnumSet") {
        return make_class("java/util/EnumSet", "java/lang/Object",
                          kOrdinary, {
            field(kPrivate, "map", "Ljava/util/HashMap;"),
            field(kPrivate, "elementType", "Ljava/lang/Class;"),
        }, {
            method(kPrivate, "<init>", "(Ljava/lang/Class;)V"),
            method(kPublic | kStatic, "noneOf",
                   "(Ljava/lang/Class;)Ljava/util/EnumSet;"),
            method(kPublic | kStatic, "copyOf",
                   "(Ljava/util/EnumSet;)Ljava/util/EnumSet;"),
            method(kPublic | kStatic, "copyOf",
                   "(Ljava/util/Collection;)Ljava/util/EnumSet;"),
            method(kPublic, "size", "()I"),
            method(kPublic, "isEmpty", "()Z"),
            method(kPublic, "contains", "(Ljava/lang/Object;)Z"),
            method(kPublic, "iterator", "()Ljava/util/Iterator;"),
            method(kPublic, "toArray", "()[Ljava/lang/Object;"),
            method(kPublic, "toArray",
                   "([Ljava/lang/Object;)[Ljava/lang/Object;"),
            method(kPublic, "add", "(Ljava/lang/Enum;)Z"),
            method(kPublic, "add", "(Ljava/lang/Object;)Z"),
            method(kPublic, "remove", "(Ljava/lang/Object;)Z"),
            method(kPublic, "addAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "clear", "()V"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/util/Set", "java/lang/Cloneable", "java/io/Serializable"});
    }

    if (name == "java/util/TreeSet") {
        return make_class("java/util/TreeSet", "java/lang/Object", kOrdinary, {
            field(kPrivate, "values", "Ljava/util/ArrayList;"),
            field(kPrivate, "comparator", "Ljava/util/Comparator;"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/util/Collection;)V"),
            method(kPublic, "<init>", "(Ljava/util/Comparator;)V"),
            method(kPublic, "size", "()I"),
            method(kPublic, "isEmpty", "()Z"),
            method(kPublic, "contains", "(Ljava/lang/Object;)Z"),
            method(kPublic, "iterator", "()Ljava/util/Iterator;"),
            method(kPublic, "toArray", "()[Ljava/lang/Object;"),
            method(kPublic, "toArray",
                   "([Ljava/lang/Object;)[Ljava/lang/Object;"),
            method(kPublic, "add", "(Ljava/lang/Object;)Z"),
            method(kPublic, "remove", "(Ljava/lang/Object;)Z"),
            method(kPublic, "containsAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "addAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "removeAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "retainAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "clear", "()V"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/util/Set", "java/lang/Cloneable", "java/io/Serializable"});
    }

    if (name == "java/util/Properties") {
        return make_class("java/util/Properties", "java/util/HashMap",
                          kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "setProperty",
                   "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/Object;"),
            method(kPublic, "getProperty",
                   "(Ljava/lang/String;)Ljava/lang/String;"),
            method(kPublic, "getProperty",
                   "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
            method(kPublic, "load", "(Ljava/io/Reader;)V"),
            method(kPublic, "load", "(Ljava/io/InputStream;)V"),
            method(kPublic, "store",
                   "(Ljava/io/OutputStream;Ljava/lang/String;)V"),
            method(kPublic, "stringPropertyNames", "()Ljava/util/Set;"),
            method(kPublic, "propertyNames", "()Ljava/util/Enumeration;"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
        });
    }

    if (name == "java/util/OptionalInt") {
        return make_class("java/util/OptionalInt", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "value", "I"),
            field(kPrivate | kFinal, "present", "Z"),
        }, {
            method(kPrivate, "<init>", "()V"),
            method(kPrivate, "<init>", "(I)V"),
            method(kPublic | kStatic, "empty", "()Ljava/util/OptionalInt;"),
            method(kPublic | kStatic, "of", "(I)Ljava/util/OptionalInt;"),
            method(kPublic, "getAsInt", "()I"),
            method(kPublic, "isPresent", "()Z"),
            method(kPublic, "ifPresent", "(Ljava/util/function/IntConsumer;)V"),
            method(kPublic, "orElse", "(I)I"),
            method(kPublic, "orElseGet", "(Ljava/util/function/IntSupplier;)I"),
            method(kPublic, "orElseThrow", "(Ljava/util/function/Supplier;)I"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        });
    }
    if (name == "java/util/OptionalLong") {
        return make_class("java/util/OptionalLong", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "value", "J"),
            field(kPrivate | kFinal, "present", "Z"),
        }, {
            method(kPrivate, "<init>", "()V"),
            method(kPrivate, "<init>", "(J)V"),
            method(kPublic | kStatic, "empty", "()Ljava/util/OptionalLong;"),
            method(kPublic | kStatic, "of", "(J)Ljava/util/OptionalLong;"),
            method(kPublic, "getAsLong", "()J"),
            method(kPublic, "isPresent", "()Z"),
            method(kPublic, "ifPresent", "(Ljava/util/function/LongConsumer;)V"),
            method(kPublic, "orElse", "(J)J"),
            method(kPublic, "orElseGet", "(Ljava/util/function/LongSupplier;)J"),
            method(kPublic, "orElseThrow", "(Ljava/util/function/Supplier;)J"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        });
    }
    if (name == "java/util/OptionalDouble") {
        return make_class("java/util/OptionalDouble", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "value", "D"),
            field(kPrivate | kFinal, "present", "Z"),
        }, {
            method(kPrivate, "<init>", "()V"),
            method(kPrivate, "<init>", "(D)V"),
            method(kPublic | kStatic, "empty", "()Ljava/util/OptionalDouble;"),
            method(kPublic | kStatic, "of", "(D)Ljava/util/OptionalDouble;"),
            method(kPublic, "getAsDouble", "()D"),
            method(kPublic, "isPresent", "()Z"),
            method(kPublic, "ifPresent", "(Ljava/util/function/DoubleConsumer;)V"),
            method(kPublic, "orElse", "(D)D"),
            method(kPublic, "orElseGet", "(Ljava/util/function/DoubleSupplier;)D"),
            method(kPublic, "orElseThrow", "(Ljava/util/function/Supplier;)D"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "hashCode", "()I"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        });
    }

    if (name == "java/nio/ByteOrder") {
        return make_class("java/nio/ByteOrder", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "name", "Ljava/lang/String;"),
            field(kPrivate | kFinal, "kind", "I"),
            field(kPublic | kStatic | kFinal, "BIG_ENDIAN",
                  "Ljava/nio/ByteOrder;"),
            field(kPublic | kStatic | kFinal, "LITTLE_ENDIAN",
                  "Ljava/nio/ByteOrder;"),
        }, {
            method(kPrivate, "<init>", "(Ljava/lang/String;I)V"),
            method(kPrivate | kStatic, "<clinit>", "()V"),
            method(kPublic | kStatic, "nativeOrder", "()Ljava/nio/ByteOrder;"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        });
    }

    if (name == "java/nio/InvalidMarkException") {
        return make_class("java/nio/InvalidMarkException",
                          "java/lang/IllegalStateException", kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
        });
    }

    if (name == "java/nio/Buffer") {
        return make_class("java/nio/Buffer", "java/lang/Object",
                          kPublic | kAbstract, {}, {
            method(kPublic | kFinal, "capacity", "()I"),
            method(kPublic | kFinal, "position", "()I"),
            method(kPublic | kFinal, "position", "(I)Ljava/nio/Buffer;"),
            method(kPublic | kFinal, "limit", "()I"),
            method(kPublic | kFinal, "limit", "(I)Ljava/nio/Buffer;"),
            method(kPublic | kFinal, "mark", "()Ljava/nio/Buffer;"),
            method(kPublic | kFinal, "reset", "()Ljava/nio/Buffer;"),
            method(kPublic | kFinal, "clear", "()Ljava/nio/Buffer;"),
            method(kPublic | kFinal, "flip", "()Ljava/nio/Buffer;"),
            method(kPublic | kFinal, "rewind", "()Ljava/nio/Buffer;"),
            method(kPublic | kFinal, "remaining", "()I"),
            method(kPublic | kFinal, "hasRemaining", "()Z"),
            method(kPublic | kAbstract, "isReadOnly", "()Z"),
            method(kPublic | kAbstract, "hasArray", "()Z"),
            method(kPublic | kAbstract, "array", "()Ljava/lang/Object;"),
            method(kPublic | kAbstract, "arrayOffset", "()I"),
            method(kPublic | kAbstract, "isDirect", "()Z"),
        });
    }

    if (name == "java/nio/BufferUnderflowException" ||
        name == "java/nio/BufferOverflowException") {
        return make_class(std::string(name), "java/lang/RuntimeException",
                          kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
        });
    }

    if (name == "java/nio/ByteBuffer") {
        return make_class("java/nio/ByteBuffer", "java/nio/Buffer",
                          kPublic | kAbstract, {
            field(kPrivate | kFinal, "data", "[B"),
            field(kPrivate, "limit", "I"),
            field(kPrivate, "position", "I"),
            field(kPrivate, "mark", "I"),
            field(kPrivate, "order", "I"),
        }, {
            method(kPrivate, "<init>", "([BII)V"),
            method(kPublic | kStatic, "allocate", "(I)Ljava/nio/ByteBuffer;"),
            method(kPublic | kStatic, "wrap", "([B)Ljava/nio/ByteBuffer;"),
            method(kPublic | kStatic, "wrap", "([BII)Ljava/nio/ByteBuffer;"),
            method(kPublic, "put", "(B)Ljava/nio/ByteBuffer;"),
            method(kPublic, "put", "([B)Ljava/nio/ByteBuffer;"),
            method(kPublic, "put", "([BII)Ljava/nio/ByteBuffer;"),
            method(kPublic, "put", "(IB)Ljava/nio/ByteBuffer;"),
            method(kPublic, "putShort", "(S)Ljava/nio/ByteBuffer;"),
            method(kPublic, "putShort", "(IS)Ljava/nio/ByteBuffer;"),
            method(kPublic, "putInt", "(I)Ljava/nio/ByteBuffer;"),
            method(kPublic, "putInt", "(II)Ljava/nio/ByteBuffer;"),
            method(kPublic, "putLong", "(J)Ljava/nio/ByteBuffer;"),
            method(kPublic, "putLong", "(IJ)Ljava/nio/ByteBuffer;"),
            method(kPublic, "get", "()B"),
            method(kPublic, "get", "(I)B"),
            method(kPublic, "get", "([B)Ljava/nio/ByteBuffer;"),
            method(kPublic, "get", "([BII)Ljava/nio/ByteBuffer;"),
            method(kPublic, "getShort", "()S"),
            method(kPublic, "getShort", "(I)S"),
            method(kPublic, "getInt", "()I"),
            method(kPublic, "getInt", "(I)I"),
            method(kPublic, "getLong", "()J"),
            method(kPublic, "getLong", "(I)J"),
            method(kPublic, "putChar", "(C)Ljava/nio/ByteBuffer;"),
            method(kPublic, "putChar", "(IC)Ljava/nio/ByteBuffer;"),
            method(kPublic, "getChar", "()C"),
            method(kPublic, "getChar", "(I)C"),
            method(kPublic, "putFloat", "(F)Ljava/nio/ByteBuffer;"),
            method(kPublic, "putFloat", "(IF)Ljava/nio/ByteBuffer;"),
            method(kPublic, "getFloat", "()F"),
            method(kPublic, "getFloat", "(I)F"),
            method(kPublic, "putDouble", "(D)Ljava/nio/ByteBuffer;"),
            method(kPublic, "putDouble", "(ID)Ljava/nio/ByteBuffer;"),
            method(kPublic, "getDouble", "()D"),
            method(kPublic, "getDouble", "(I)D"),
            method(kPublic | kFinal, "order", "()Ljava/nio/ByteOrder;"),
            method(kPublic | kFinal, "order",
                   "(Ljava/nio/ByteOrder;)Ljava/nio/ByteBuffer;"),
            method(kPublic | kFinal, "hasArray", "()Z"),
            method(kPublic | kFinal, "array", "()[B"),
            method(kPublic, "array", "()Ljava/lang/Object;"),
            method(kPublic | kFinal, "arrayOffset", "()I"),
            method(kPublic, "isDirect", "()Z"),
            method(kPublic, "isReadOnly", "()Z"),
            // Covariant aliases keep post-JDK8 bytecode usable while the
            // canonical JDK8 Buffer-returning descriptors live on Buffer.
            method(kPublic, "limit", "(I)Ljava/nio/ByteBuffer;"),
            method(kPublic, "position", "(I)Ljava/nio/ByteBuffer;"),
            method(kPublic, "clear", "()Ljava/nio/ByteBuffer;"),
            method(kPublic, "flip", "()Ljava/nio/ByteBuffer;"),
            method(kPublic, "rewind", "()Ljava/nio/ByteBuffer;"),
        }, {"java/lang/Comparable"});
    }
    if (name == "java/nio/HeapByteBuffer") {
        return make_class("java/nio/HeapByteBuffer", "java/nio/ByteBuffer",
                          kSuper | kFinal, {}, {});
    }

    if (name == "java/util/concurrent/atomic/AtomicInteger") {
        return make_class("java/util/concurrent/atomic/AtomicInteger",
                          "java/lang/Number", kOrdinary, {
            field(kPrivate, "value", "I"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(I)V"),
            method(kPublic | kFinal, "get", "()I"),
            method(kPublic | kFinal, "set", "(I)V"),
            method(kPublic | kFinal, "lazySet", "(I)V"),
            method(kPublic | kFinal, "getAndSet", "(I)I"),
            method(kPublic | kFinal, "compareAndSet", "(II)Z"),
            method(kPublic | kFinal, "weakCompareAndSet", "(II)Z"),
            method(kPublic | kFinal, "getAndIncrement", "()I"),
            method(kPublic | kFinal, "getAndDecrement", "()I"),
            method(kPublic | kFinal, "getAndAdd", "(I)I"),
            method(kPublic | kFinal, "incrementAndGet", "()I"),
            method(kPublic | kFinal, "decrementAndGet", "()I"),
            method(kPublic | kFinal, "addAndGet", "(I)I"),
            method(kPublic | kFinal, "getAndUpdate",
                   "(Ljava/util/function/IntUnaryOperator;)I"),
            method(kPublic | kFinal, "updateAndGet",
                   "(Ljava/util/function/IntUnaryOperator;)I"),
            method(kPublic | kFinal, "getAndAccumulate",
                   "(ILjava/util/function/IntBinaryOperator;)I"),
            method(kPublic | kFinal, "accumulateAndGet",
                   "(ILjava/util/function/IntBinaryOperator;)I"),
            method(kPublic, "intValue", "()I"),
            method(kPublic, "longValue", "()J"),
            method(kPublic, "floatValue", "()F"),
            method(kPublic, "doubleValue", "()D"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/io/Serializable"});
    }

    if (name == "java/util/concurrent/atomic/AtomicLong") {
        return make_class("java/util/concurrent/atomic/AtomicLong",
                          "java/lang/Number", kOrdinary, {
            field(kPrivate, "value", "J"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(J)V"),
            method(kPublic | kFinal, "get", "()J"),
            method(kPublic | kFinal, "set", "(J)V"),
            method(kPublic | kFinal, "lazySet", "(J)V"),
            method(kPublic | kFinal, "getAndSet", "(J)J"),
            method(kPublic | kFinal, "compareAndSet", "(JJ)Z"),
            method(kPublic | kFinal, "weakCompareAndSet", "(JJ)Z"),
            method(kPublic | kFinal, "getAndIncrement", "()J"),
            method(kPublic | kFinal, "getAndDecrement", "()J"),
            method(kPublic | kFinal, "getAndAdd", "(J)J"),
            method(kPublic | kFinal, "incrementAndGet", "()J"),
            method(kPublic | kFinal, "decrementAndGet", "()J"),
            method(kPublic | kFinal, "addAndGet", "(J)J"),
            method(kPublic | kFinal, "getAndUpdate",
                   "(Ljava/util/function/LongUnaryOperator;)J"),
            method(kPublic | kFinal, "updateAndGet",
                   "(Ljava/util/function/LongUnaryOperator;)J"),
            method(kPublic | kFinal, "getAndAccumulate",
                   "(JLjava/util/function/LongBinaryOperator;)J"),
            method(kPublic | kFinal, "accumulateAndGet",
                   "(JLjava/util/function/LongBinaryOperator;)J"),
            method(kPublic, "intValue", "()I"),
            method(kPublic, "longValue", "()J"),
            method(kPublic, "floatValue", "()F"),
            method(kPublic, "doubleValue", "()D"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/io/Serializable"});
    }

    if (name == "java/util/concurrent/atomic/AtomicBoolean") {
        return make_class("java/util/concurrent/atomic/AtomicBoolean",
                          "java/lang/Object", kOrdinary, {
            field(kPrivate, "value", "Z"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Z)V"),
            method(kPublic | kFinal, "get", "()Z"),
            method(kPublic | kFinal, "compareAndSet", "(ZZ)Z"),
            method(kPublic, "weakCompareAndSet", "(ZZ)Z"),
            method(kPublic | kFinal, "set", "(Z)V"),
            method(kPublic | kFinal, "lazySet", "(Z)V"),
            method(kPublic | kFinal, "getAndSet", "(Z)Z"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/io/Serializable"});
    }

    if (name == "java/util/concurrent/atomic/AtomicReference") {
        return make_class("java/util/concurrent/atomic/AtomicReference",
                          "java/lang/Object", kOrdinary, {
            field(kPrivate, "value", "Ljava/lang/Object;"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/Object;)V"),
            method(kPublic | kFinal, "get", "()Ljava/lang/Object;"),
            method(kPublic | kFinal, "set", "(Ljava/lang/Object;)V"),
            method(kPublic | kFinal, "lazySet", "(Ljava/lang/Object;)V"),
            method(kPublic | kFinal, "getAndSet",
                   "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic | kFinal, "compareAndSet",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Z"),
            method(kPublic | kFinal, "weakCompareAndSet",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Z"),
            method(kPublic | kFinal, "getAndUpdate",
                   "(Ljava/util/function/UnaryOperator;)Ljava/lang/Object;"),
            method(kPublic | kFinal, "updateAndGet",
                   "(Ljava/util/function/UnaryOperator;)Ljava/lang/Object;"),
            method(kPublic | kFinal, "getAndAccumulate",
                   "(Ljava/lang/Object;Ljava/util/function/BinaryOperator;)Ljava/lang/Object;"),
            method(kPublic | kFinal, "accumulateAndGet",
                   "(Ljava/lang/Object;Ljava/util/function/BinaryOperator;)Ljava/lang/Object;"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/io/Serializable"});
    }

    if (name == "java/util/concurrent/ConcurrentMap") {
        return make_class("java/util/concurrent/ConcurrentMap", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic, "getOrDefault",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "forEach", "(Ljava/util/function/BiConsumer;)V"),
            method(kPublic | kAbstract, "putIfAbsent",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic | kAbstract, "remove",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Z"),
            method(kPublic | kAbstract, "replace",
                   "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Z"),
            method(kPublic | kAbstract, "replace",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "replaceAll", "(Ljava/util/function/BiFunction;)V"),
            method(kPublic, "computeIfAbsent",
                   "(Ljava/lang/Object;Ljava/util/function/Function;)Ljava/lang/Object;"),
            method(kPublic, "computeIfPresent",
                   "(Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;"),
            method(kPublic, "compute",
                   "(Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;"),
            method(kPublic, "merge",
                   "(Ljava/lang/Object;Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;"),
        }, {"java/util/Map"});
    }
    if (name == "java/util/concurrent/ConcurrentHashMap") {
        return make_class("java/util/concurrent/ConcurrentHashMap",
                          "java/util/AbstractMap", kOrdinary, {
            field(kPrivate, "keys", "[Ljava/lang/Object;"),
            field(kPrivate, "values", "[Ljava/lang/Object;"),
            field(kPrivate, "size", "I"),
            field(kPrivate, "hashes", "[I"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(I)V"),
            method(kPublic, "<init>", "(Ljava/util/Map;)V"),
            method(kPublic, "<init>", "(IF)V"),
            method(kPublic, "<init>", "(IFI)V"),
            method(kPublic, "size", "()I"),
            method(kPublic, "isEmpty", "()Z"),
            method(kPublic, "get", "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "containsKey", "(Ljava/lang/Object;)Z"),
            method(kPublic, "containsValue", "(Ljava/lang/Object;)Z"),
            method(kPublic, "put",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "putAll", "(Ljava/util/Map;)V"),
            method(kPublic, "remove",
                   "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "clear", "()V"),
            method(kPublic, "keySet", "()Ljava/util/Set;"),
            method(kPublic, "keySet",
                   "()Ljava/util/concurrent/ConcurrentHashMap$KeySetView;"),
            method(kPublic, "keySet",
                   "(Ljava/lang/Object;)Ljava/util/concurrent/ConcurrentHashMap$KeySetView;"),
            method(kPublic | kStatic, "newKeySet",
                   "()Ljava/util/concurrent/ConcurrentHashMap$KeySetView;"),
            method(kPublic | kStatic, "newKeySet",
                   "(I)Ljava/util/concurrent/ConcurrentHashMap$KeySetView;"),
            method(kPublic, "values", "()Ljava/util/Collection;"),
            method(kPublic, "entrySet", "()Ljava/util/Set;"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
            method(kPublic, "putIfAbsent",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "remove",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Z"),
            method(kPublic, "replace",
                   "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Z"),
            method(kPublic, "replace",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "getOrDefault",
                   "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "forEach", "(Ljava/util/function/BiConsumer;)V"),
            method(kPublic, "replaceAll", "(Ljava/util/function/BiFunction;)V"),
            method(kPublic, "computeIfAbsent",
                   "(Ljava/lang/Object;Ljava/util/function/Function;)Ljava/lang/Object;"),
            method(kPublic, "computeIfPresent",
                   "(Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;"),
            method(kPublic, "compute",
                   "(Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;"),
            method(kPublic, "merge",
                   "(Ljava/lang/Object;Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;"),
            method(kPublic, "contains", "(Ljava/lang/Object;)Z"),
            method(kPublic, "keys", "()Ljava/util/Enumeration;"),
            method(kPublic, "elements", "()Ljava/util/Enumeration;"),
            method(kPublic, "mappingCount", "()J"),
        }, {"java/util/concurrent/ConcurrentMap", "java/io/Serializable"});
    }
    if (name == "java/util/concurrent/ConcurrentHashMap$KeySetView") {
        return make_class("java/util/concurrent/ConcurrentHashMap$KeySetView",
                          "java/util/HashSet", kOrdinary, {}, {
            method(kPublic, "getMappedValue", "()Ljava/lang/Object;"),
        }, {"java/io/Serializable"});
    }
    if (name == "java/util/concurrent/CopyOnWriteArrayList") {
        // Reuse Core's compact ArrayList storage layout. This class exists to
        // run desktop-server code embedded in offline MIDlets without pulling
        // in the full java.util.concurrent implementation.
        return make_class("java/util/concurrent/CopyOnWriteArrayList",
                          "java/lang/Object", kOrdinary, {
            field(kPrivate, "elementData", "[Ljava/lang/Object;"),
            field(kPrivate, "size", "I"),
            field(kPrivate, "capacityIncrement", "I"),
            field(kPrivate, "mutationMode", "I"),
        }, {
            method(kPublic, "<init>", "()V"),
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
            method(kPublic, "addAll", "(Ljava/util/Collection;)Z"),
            method(kPublic, "equals", "(Ljava/lang/Object;)Z"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/util/List", "java/io/Serializable"});
    }
    if (name == "java/util/concurrent/BlockingQueue") {
        return make_class("java/util/concurrent/BlockingQueue",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "add", "(Ljava/lang/Object;)Z"),
            method(kPublic | kAbstract, "offer", "(Ljava/lang/Object;)Z"),
            method(kPublic | kAbstract, "remove", "()Ljava/lang/Object;"),
            method(kPublic | kAbstract, "poll", "()Ljava/lang/Object;"),
            method(kPublic | kAbstract, "element", "()Ljava/lang/Object;"),
            method(kPublic | kAbstract, "peek", "()Ljava/lang/Object;"),
            method(kPublic | kAbstract, "put", "(Ljava/lang/Object;)V"),
            method(kPublic | kAbstract, "offer",
                   "(Ljava/lang/Object;JLjava/util/concurrent/TimeUnit;)Z"),
            method(kPublic | kAbstract, "take", "()Ljava/lang/Object;"),
            method(kPublic | kAbstract, "poll",
                   "(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;"),
            method(kPublic | kAbstract, "remainingCapacity", "()I"),
            method(kPublic | kAbstract, "drainTo", "(Ljava/util/Collection;)I"),
            method(kPublic | kAbstract, "drainTo", "(Ljava/util/Collection;I)I"),
        }, {"java/util/Queue"});
    }
    if (name == "java/util/concurrent/LinkedBlockingDeque") {
        // A compact unbounded implementation is enough for desktop-server
        // code embedded in offline MIDlets. Reuse ArrayDeque's storage and
        // supply BlockingQueue.take() in the native layer.
        return make_class("java/util/concurrent/LinkedBlockingDeque",
                          "java/util/ArrayDeque", kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "put", "(Ljava/lang/Object;)V"),
            method(kPublic, "take", "()Ljava/lang/Object;"),
            method(kPublic, "remainingCapacity", "()I"),
        }, {"java/util/concurrent/BlockingQueue",
             "java/io/Serializable"});
    }
    if (name == "java/util/concurrent/Callable") {
        return make_class("java/util/concurrent/Callable", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "call", "()Ljava/lang/Object;"),
        });
    }
    if (name == "java/util/concurrent/Future") {
        return make_class("java/util/concurrent/Future", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "cancel", "(Z)Z"),
            method(kPublic | kAbstract, "isCancelled", "()Z"),
            method(kPublic | kAbstract, "isDone", "()Z"),
            method(kPublic | kAbstract, "get", "()Ljava/lang/Object;"),
            method(kPublic | kAbstract, "get",
                   "(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;"),
        });
    }
    if (name == "java/util/concurrent/RunnableFuture") {
        return make_class("java/util/concurrent/RunnableFuture",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {},
                          {"java/lang/Runnable", "java/util/concurrent/Future"});
    }
    if (name == "java/util/concurrent/Executor") {
        return make_class("java/util/concurrent/Executor", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "execute", "(Ljava/lang/Runnable;)V"),
        });
    }
    if (name == "java/util/concurrent/ExecutorService") {
        return make_class("java/util/concurrent/ExecutorService",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "shutdown", "()V"),
            method(kPublic | kAbstract, "shutdownNow", "()Ljava/util/List;"),
            method(kPublic | kAbstract, "isShutdown", "()Z"),
            method(kPublic | kAbstract, "isTerminated", "()Z"),
            method(kPublic | kAbstract, "awaitTermination",
                   "(JLjava/util/concurrent/TimeUnit;)Z"),
            method(kPublic | kAbstract, "submit",
                   "(Ljava/util/concurrent/Callable;)Ljava/util/concurrent/Future;"),
            method(kPublic | kAbstract, "submit",
                   "(Ljava/lang/Runnable;Ljava/lang/Object;)Ljava/util/concurrent/Future;"),
            method(kPublic | kAbstract, "submit",
                   "(Ljava/lang/Runnable;)Ljava/util/concurrent/Future;"),
        }, {"java/util/concurrent/Executor"});
    }
    if (name == "java/util/concurrent/CountDownLatch") {
        return make_class("java/util/concurrent/CountDownLatch",
                          "java/lang/Object", kOrdinary, {
            field(kPrivate, "count", "J"),
        }, {
            method(kPublic, "<init>", "(I)V"),
            method(kPublic, "await", "()V"),
            method(kPublic, "await",
                   "(JLjava/util/concurrent/TimeUnit;)Z"),
            method(kPublic, "countDown", "()V"),
            method(kPublic, "getCount", "()J"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        });
    }
    if (name == "java/util/concurrent/FutureTask") {
        return make_class("java/util/concurrent/FutureTask", "java/lang/Object",
                          kOrdinary, {
            field(kPrivate, "callable", "Ljava/util/concurrent/Callable;"),
            field(kPrivate, "runnable", "Ljava/lang/Runnable;"),
            field(kPrivate, "result", "Ljava/lang/Object;"),
            field(kPrivate, "throwable", "Ljava/lang/Throwable;"),
            field(kPrivate, "state", "I"),
            field(kPrivate, "runner", "Ljava/lang/Thread;"),
        }, {
            method(kPublic, "<init>", "(Ljava/util/concurrent/Callable;)V"),
            method(kPublic, "<init>",
                   "(Ljava/lang/Runnable;Ljava/lang/Object;)V"),
            method(kPublic, "isCancelled", "()Z"),
            method(kPublic, "isDone", "()Z"),
            method(kPublic, "cancel", "(Z)Z"),
            method(kPublic, "get", "()Ljava/lang/Object;"),
            method(kPublic, "get",
                   "(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;"),
            method(kProtected, "done", "()V"),
            method(kProtected, "set", "(Ljava/lang/Object;)V"),
            method(kProtected, "setException", "(Ljava/lang/Throwable;)V"),
            method(kPublic, "run", "()V"),
            method(kProtected, "runAndReset", "()Z"),
        }, {"java/util/concurrent/RunnableFuture"});
    }
    if (name == "java/util/concurrent/NativeExecutorService") {
        return make_class("java/util/concurrent/NativeExecutorService",
                          "java/lang/Object", kOrdinary | kFinal, {
            field(kPrivate, "shutdown", "Z"),
            field(kPrivate, "workers", "I"),
        }, {
            method(kPrivate, "<init>", "(I)V"),
            method(kPublic, "execute", "(Ljava/lang/Runnable;)V"),
            method(kPublic, "shutdown", "()V"),
            method(kPublic, "shutdownNow", "()Ljava/util/List;"),
            method(kPublic, "isShutdown", "()Z"),
            method(kPublic, "isTerminated", "()Z"),
            method(kPublic, "awaitTermination",
                   "(JLjava/util/concurrent/TimeUnit;)Z"),
            method(kPublic, "submit",
                   "(Ljava/util/concurrent/Callable;)Ljava/util/concurrent/Future;"),
            method(kPublic, "submit",
                   "(Ljava/lang/Runnable;Ljava/lang/Object;)Ljava/util/concurrent/Future;"),
            method(kPublic, "submit",
                   "(Ljava/lang/Runnable;)Ljava/util/concurrent/Future;"),
        }, {"java/util/concurrent/ExecutorService"});
    }
    if (name == "java/util/concurrent/NativeRunnableCallable") {
        return make_class("java/util/concurrent/NativeRunnableCallable",
                          "java/lang/Object", kOrdinary | kFinal, {
            field(kPrivate, "runnable", "Ljava/lang/Runnable;"),
            field(kPrivate, "result", "Ljava/lang/Object;"),
        }, {
            method(kPrivate, "<init>",
                   "(Ljava/lang/Runnable;Ljava/lang/Object;)V"),
            method(kPublic, "call", "()Ljava/lang/Object;"),
        }, {"java/util/concurrent/Callable"});
    }
    if (name == "java/util/concurrent/Executors") {
        return make_class("java/util/concurrent/Executors", "java/lang/Object",
                          kOrdinary, {}, {
            method(kPrivate, "<init>", "()V"),
            method(kPublic | kStatic, "newFixedThreadPool",
                   "(I)Ljava/util/concurrent/ExecutorService;"),
            method(kPublic | kStatic, "newSingleThreadExecutor",
                   "()Ljava/util/concurrent/ExecutorService;"),
            method(kPublic | kStatic, "newCachedThreadPool",
                   "()Ljava/util/concurrent/ExecutorService;"),
            method(kPublic | kStatic, "callable",
                   "(Ljava/lang/Runnable;Ljava/lang/Object;)Ljava/util/concurrent/Callable;"),
            method(kPublic | kStatic, "callable",
                   "(Ljava/lang/Runnable;)Ljava/util/concurrent/Callable;"),
        });
    }
    if (name == "java/util/concurrent/ExecutionException") {
        return make_class("java/util/concurrent/ExecutionException",
                          "java/lang/Exception", kOrdinary, {}, {
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            method(kPublic, "<init>", "(Ljava/lang/Throwable;)V"),
        });
    }
    if (name == "java/util/concurrent/TimeoutException") {
        return make_class("java/util/concurrent/TimeoutException",
                          "java/lang/Exception", kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
        });
    }
    if (name == "java/util/concurrent/CancellationException") {
        return make_class("java/util/concurrent/CancellationException",
                          "java/lang/IllegalStateException", kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
        });
    }
    if (name == "java/util/concurrent/RejectedExecutionException") {
        return make_class("java/util/concurrent/RejectedExecutionException",
                          "java/lang/RuntimeException", kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
        });
    }

    if (name == "java/math/BigDecimal") {
        return make_class("java/math/BigDecimal", "java/lang/Number",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "unscaled", "J"),
            field(kPrivate | kFinal, "scale", "I"),
        }, {
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            method(kPublic | kStatic, "valueOf", "(J)Ljava/math/BigDecimal;"),
            method(kPublic, "movePointRight", "(I)Ljava/math/BigDecimal;"),
            method(kPublic, "intValueExact", "()I"),
            method(kPublic, "compareTo", "(Ljava/math/BigDecimal;)I"),
            method(kPublic, "compareTo", "(Ljava/lang/Object;)I"),
            method(kPublic, "intValue", "()I"),
            method(kPublic, "longValue", "()J"),
            method(kPublic, "floatValue", "()F"),
            method(kPublic, "doubleValue", "()D"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        }, {"java/lang/Comparable", "java/io/Serializable"});
    }

    if (name == "java/security/SecureRandom") {
        return make_class("java/security/SecureRandom", "java/util/Random",
                          kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "nextLong", "()J"),
            method(kPublic, "nextBytes", "([B)V"),
        });
    }

    if (name == "java/util/stream/Stream") {
        return make_class("java/util/stream/Stream", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "values", "[Ljava/lang/Object;"),
        }, {
            method(kPrivate, "<init>", "([Ljava/lang/Object;)V"),
            method(kPublic, "mapToInt",
                   "(Ljava/util/function/ToIntFunction;)Ljava/util/stream/IntStream;"),
        });
    }
    if (name == "java/util/stream/IntStream") {
        return make_class("java/util/stream/IntStream", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "values", "[I"),
        }, {
            method(kPrivate, "<init>", "([I)V"),
            method(kPublic, "toArray", "()[I"),
        });
    }

    if (name == "java/util/regex/PatternSyntaxException") {
        return make_class("java/util/regex/PatternSyntaxException",
                          "java/lang/IllegalArgumentException", kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
        });
    }
    if (name == "java/util/regex/Pattern") {
        return make_class("java/util/regex/Pattern", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "source", "Ljava/lang/String;"),
            field(kPrivate | kFinal, "flags", "I"),
        }, {
            method(kPrivate, "<init>", "(Ljava/lang/String;I)V"),
            method(kPublic | kStatic, "compile",
                   "(Ljava/lang/String;)Ljava/util/regex/Pattern;"),
            method(kPublic | kStatic, "compile",
                   "(Ljava/lang/String;I)Ljava/util/regex/Pattern;"),
            method(kPublic | kStatic, "matches",
                   "(Ljava/lang/String;Ljava/lang/CharSequence;)Z"),
            method(kPublic, "matcher",
                   "(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;"),
        });
    }
    if (name == "java/util/regex/Matcher") {
        return make_class("java/util/regex/Matcher", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "pattern", "Ljava/util/regex/Pattern;"),
            field(kPrivate | kFinal, "input", "Ljava/lang/String;"),
            field(kPrivate, "searchFrom", "I"),
            field(kPrivate, "groups", "[Ljava/lang/String;"),
            field(kPrivate, "matched", "Z"),
        }, {
            method(kPrivate, "<init>",
                   "(Ljava/util/regex/Pattern;Ljava/lang/String;)V"),
            method(kPublic, "find", "()Z"),
            method(kPublic, "matches", "()Z"),
            method(kPublic, "group", "()Ljava/lang/String;"),
            method(kPublic, "group", "(I)Ljava/lang/String;"),
        });
    }

    if (name == "java/util/zip/ZipException") {
        return make_class("java/util/zip/ZipException",
                          "java/io/IOException", kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
        });
    }
    if (name == "java/util/zip/DataFormatException") {
        return make_class("java/util/zip/DataFormatException",
                          "java/lang/Exception", kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
        });
    }
    if (name == "java/util/zip/Inflater") {
        return make_class("java/util/zip/Inflater", "java/lang/Object",
                          kOrdinary, {
            field(kPrivate, "input", "[B"),
            field(kPrivate, "output", "[B"),
            field(kPrivate, "offset", "I"),
            field(kPrivate, "finished", "Z"),
            field(kPrivate, "needsInput", "Z"),
            field(kPrivate, "needsDictionary", "Z"),
            field(kPrivate, "ended", "Z"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "setInput", "([B)V"),
            method(kPublic, "inflate", "([B)I"),
            method(kPublic, "finished", "()Z"),
            method(kPublic, "needsInput", "()Z"),
            method(kPublic, "needsDictionary", "()Z"),
            method(kPublic, "end", "()V"),
        });
    }
    if (name == "java/util/zip/GZIPInputStream") {
        return make_class("java/util/zip/GZIPInputStream",
                          "java/io/InputStream", kOrdinary, {
            field(kPrivate, "data", "[B"),
            field(kPrivate, "position", "I"),
            field(kPrivate, "closed", "Z"),
        }, {
            method(kPublic, "<init>", "(Ljava/io/InputStream;)V"),
            method(kPublic, "read", "()I"),
            method(kPublic, "read", "([BII)I"),
            method(kPublic, "close", "()V"),
        });
    }
    if (name == "java/util/zip/GZIPOutputStream") {
        return make_class("java/util/zip/GZIPOutputStream",
                          "java/io/OutputStream", kOrdinary, {
            field(kPrivate | kFinal, "out", "Ljava/io/OutputStream;"),
            field(kPrivate, "data", "[B"),
            field(kPrivate, "size", "I"),
            field(kPrivate, "closed", "Z"),
        }, {
            method(kPublic, "<init>", "(Ljava/io/OutputStream;)V"),
            method(kPublic, "write", "(I)V"),
            method(kPublic, "write", "([BII)V"),
            method(kPublic, "close", "()V"),
        });
    }

    if (name == "java/net/URL") {
        return make_class("java/net/URL", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "owner", "Ljava/lang/Class;"),
            field(kPrivate | kFinal, "name", "Ljava/lang/String;"),
        }, {
            method(kPrivate, "<init>",
                   "(Ljava/lang/Class;Ljava/lang/String;)V"),
            method(kPublic, "openStream", "()Ljava/io/InputStream;"),
            method(kPublic, "toURI", "()Ljava/net/URI;"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        });
    }

    if (name == "java/net/URI") {
        return make_class("java/net/URI", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "path", "Ljava/lang/String;"),
        }, {
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            method(kPublic, "getPath", "()Ljava/lang/String;"),
            method(kPublic, "toString", "()Ljava/lang/String;"),
        });
    }

    if (name == "java/lang/ReflectiveOperationException") {
        return make_class("java/lang/ReflectiveOperationException",
                          "java/lang/Exception", kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
        });
    }
    if (name == "java/lang/NoSuchFieldException") {
        return make_class("java/lang/NoSuchFieldException",
                          "java/lang/ReflectiveOperationException",
                          kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
        });
    }
    if (name == "java/lang/NoSuchMethodException") {
        return make_class("java/lang/NoSuchMethodException",
                          "java/lang/ReflectiveOperationException",
                          kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
        });
    }
    if (name == "java/lang/reflect/Modifier") {
        return make_class("java/lang/reflect/Modifier", "java/lang/Object",
                          kOrdinary | kFinal, {}, {
            method(kPrivate, "<init>", "()V"),
            method(kPublic | kStatic, "isStatic", "(I)Z"),
        });
    }
    if (name == "java/lang/reflect/Field") {
        return make_class("java/lang/reflect/Field", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "declaringClass", "Ljava/lang/Class;"),
            field(kPrivate | kFinal, "name", "Ljava/lang/String;"),
            field(kPrivate | kFinal, "descriptor", "Ljava/lang/String;"),
            field(kPrivate | kFinal, "modifiers", "I"),
        }, {
            method(kPrivate, "<init>",
                   "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;I)V"),
            method(kPublic, "get", "(Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "getModifiers", "()I"),
            method(kPublic, "getType", "()Ljava/lang/Class;"),
        });
    }
    if (name == "java/lang/reflect/Method") {
        return make_class("java/lang/reflect/Method", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "declaringClass", "Ljava/lang/Class;"),
            field(kPrivate | kFinal, "name", "Ljava/lang/String;"),
            field(kPrivate | kFinal, "descriptor", "Ljava/lang/String;"),
            field(kPrivate | kFinal, "modifiers", "I"),
        }, {
            method(kPrivate, "<init>",
                   "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;I)V"),
            method(kPublic, "invoke",
                   "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;"),
            method(kPublic, "getModifiers", "()I"),
        });
    }

    if (name == "java/text/SimpleDateFormat") {
        return make_class("java/text/SimpleDateFormat", "java/lang/Object",
                          kOrdinary, {
            field(kPrivate | kFinal, "pattern", "Ljava/lang/String;"),
        }, {
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            method(kPublic, "<init>",
                   "(Ljava/lang/String;Ljava/util/Locale;)V"),
            method(kPublic, "format",
                   "(Ljava/util/Date;)Ljava/lang/String;"),
        });
    }
    if (name == "java/text/NumberFormat") {
        return make_class("java/text/NumberFormat", "java/lang/Object",
                          kOrdinary, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic | kStatic, "getInstance",
                   "(Ljava/util/Locale;)Ljava/text/NumberFormat;"),
            method(kPublic | kFinal, "format", "(J)Ljava/lang/String;"),
        });
    }

    if (name == "java/util/concurrent/TimeUnit") {
        return make_class("java/util/concurrent/TimeUnit", "java/lang/Enum",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "nanosPerUnit", "J"),
            field(kPublic | kStatic | kFinal, "NANOSECONDS",
                  "Ljava/util/concurrent/TimeUnit;"),
            field(kPublic | kStatic | kFinal, "MICROSECONDS",
                  "Ljava/util/concurrent/TimeUnit;"),
            field(kPublic | kStatic | kFinal, "MILLISECONDS",
                  "Ljava/util/concurrent/TimeUnit;"),
            field(kPublic | kStatic | kFinal, "SECONDS",
                  "Ljava/util/concurrent/TimeUnit;"),
            field(kPublic | kStatic | kFinal, "MINUTES",
                  "Ljava/util/concurrent/TimeUnit;"),
            field(kPublic | kStatic | kFinal, "HOURS",
                  "Ljava/util/concurrent/TimeUnit;"),
            field(kPublic | kStatic | kFinal, "DAYS",
                  "Ljava/util/concurrent/TimeUnit;"),
        }, {
            method(kPrivate, "<init>", "(Ljava/lang/String;IJ)V"),
            method(kStatic, "<clinit>", "()V"),
            method(kPublic, "toNanos", "(J)J"),
            method(kPublic, "toMicros", "(J)J"),
            method(kPublic, "toMillis", "(J)J"),
            method(kPublic, "toSeconds", "(J)J"),
            method(kPublic, "convert",
                   "(JLjava/util/concurrent/TimeUnit;)J"),
        });
    }

    return nullptr;
}

} // namespace

void register_jdk8_compat_classes(BuiltinClassRegistry& registry) {
    registry.add_factory(build_jdk8_compat_class);
}

} // namespace phoneme::vm
