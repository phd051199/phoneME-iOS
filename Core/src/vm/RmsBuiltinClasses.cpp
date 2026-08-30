#include "phoneme/vm/BuiltinClassRegistry.hpp"

#include <array>

#include "BuiltinClassSupport.hpp"

namespace phoneme::vm {
namespace {

using namespace builtin;

[[nodiscard]] ClassPtr build_rms_class(std::string_view name) {
    if (name == "javax/microedition/rms/RecordFilter") {
        return make_class("javax/microedition/rms/RecordFilter", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "matches", "([B)Z"),
        });
    }
    if (name == "javax/microedition/rms/RecordComparator") {
        return make_class("javax/microedition/rms/RecordComparator",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {
            field(kPublic | kStatic | kFinal, "PRECEDES", "I"),
            field(kPublic | kStatic | kFinal, "EQUIVALENT", "I"),
            field(kPublic | kStatic | kFinal, "FOLLOWS", "I"),
        }, {
            method(kPublic | kAbstract, "compare", "([B[B)I"),
        });
    }
    if (name == "javax/microedition/rms/RecordListener") {
        return make_class("javax/microedition/rms/RecordListener",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "recordAdded",
                   "(Ljavax/microedition/rms/RecordStore;I)V"),
            method(kPublic | kAbstract, "recordChanged",
                   "(Ljavax/microedition/rms/RecordStore;I)V"),
            method(kPublic | kAbstract, "recordDeleted",
                   "(Ljavax/microedition/rms/RecordStore;I)V"),
        });
    }
    if (name == "javax/microedition/rms/RecordEnumeration") {
        return make_class("javax/microedition/rms/RecordEnumeration",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "numRecords", "()I"),
            method(kPublic | kAbstract, "hasNextElement", "()Z"),
            method(kPublic | kAbstract, "nextRecordId", "()I"),
            method(kPublic | kAbstract, "nextRecord", "()[B"),
            method(kPublic | kAbstract, "hasPreviousElement", "()Z"),
            method(kPublic | kAbstract, "previousRecordId", "()I"),
            method(kPublic | kAbstract, "previousRecord", "()[B"),
            method(kPublic | kAbstract, "reset", "()V"),
            method(kPublic | kAbstract, "rebuild", "()V"),
            method(kPublic | kAbstract, "keepUpdated", "(Z)V"),
            method(kPublic | kAbstract, "isKeptUpdated", "()Z"),
            method(kPublic | kAbstract, "destroy", "()V"),
        });
    }
    if (name == "javax/microedition/rms/SnapshotRecordEnumeration") {
        return make_class("javax/microedition/rms/SnapshotRecordEnumeration",
                          "java/lang/Object", kOrdinary | kFinal, {
            field(kPrivate, "records", "[[B"),
            field(kPrivate, "ids", "[I"),
            field(kPrivate, "index", "I"),
            field(kPrivate, "destroyed", "Z"),
            field(kPrivate, "keepUpdated", "Z"),
            field(kPrivate, "storeName", "Ljava/lang/String;"),
            field(kPrivate, "filter", "Ljavax/microedition/rms/RecordFilter;"),
            field(kPrivate, "comparator", "Ljavax/microedition/rms/RecordComparator;"),
            field(kPrivate, "store", "Ljavax/microedition/rms/RecordStore;"),
            field(kPrivate, "observedVersion", "I"),
        }, {
            method(kPublic, "numRecords", "()I"),
            method(kPublic, "hasNextElement", "()Z"),
            method(kPublic, "nextRecordId", "()I"),
            method(kPublic, "nextRecord", "()[B"),
            method(kPublic, "hasPreviousElement", "()Z"),
            method(kPublic, "previousRecordId", "()I"),
            method(kPublic, "previousRecord", "()[B"),
            method(kPublic, "reset", "()V"),
            method(kPublic, "rebuild", "()V"),
            method(kPublic, "keepUpdated", "(Z)V"),
            method(kPublic, "isKeptUpdated", "()Z"),
            method(kPublic, "destroy", "()V"),
        }, {"javax/microedition/rms/RecordEnumeration"});
    }
    if (name == "javax/microedition/rms/RecordStore") {
        return make_class("javax/microedition/rms/RecordStore", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate, "storeName", "Ljava/lang/String;"),
            field(kPrivate, "open", "Z"),
            field(kPublic | kStatic | kFinal, "AUTHMODE_PRIVATE", "I"),
            field(kPublic | kStatic | kFinal, "AUTHMODE_ANY", "I"),
        }, {
            method(kPublic | kStatic, "openRecordStore",
                   "(Ljava/lang/String;Z)Ljavax/microedition/rms/RecordStore;"),
            method(kPublic | kStatic, "openRecordStore",
                   "(Ljava/lang/String;ZIZ)Ljavax/microedition/rms/RecordStore;"),
            method(kPublic | kStatic, "openRecordStore",
                   "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)"
                   "Ljavax/microedition/rms/RecordStore;"),
            method(kPublic | kStatic, "deleteRecordStore", "(Ljava/lang/String;)V"),
            method(kPublic | kStatic, "listRecordStores", "()[Ljava/lang/String;"),
            method(kPublic, "closeRecordStore", "()V"),
            method(kPublic, "setMode", "(IZ)V"),
            method(kPublic, "getName", "()Ljava/lang/String;"),
            method(kPublic, "getVersion", "()I"),
            method(kPublic, "getNumRecords", "()I"),
            method(kPublic, "getSize", "()I"),
            method(kPublic, "getSizeAvailable", "()I"),
            method(kPublic, "getLastModified", "()J"),
            method(kPublic, "getNextRecordID", "()I"),
            method(kPublic, "addRecord", "([BII)I"),
            method(kPublic, "setRecord", "(I[BII)V"),
            method(kPublic, "deleteRecord", "(I)V"),
            method(kPublic, "getRecord", "(I)[B"),
            method(kPublic, "getRecord", "(I[BI)I"),
            method(kPublic, "getRecordSize", "(I)I"),
            method(kPublic, "enumerateRecords",
                   "(Ljavax/microedition/rms/RecordFilter;"
                   "Ljavax/microedition/rms/RecordComparator;Z)"
                   "Ljavax/microedition/rms/RecordEnumeration;"),
            method(kPublic, "addRecordListener",
                   "(Ljavax/microedition/rms/RecordListener;)V"),
            method(kPublic, "removeRecordListener",
                   "(Ljavax/microedition/rms/RecordListener;)V"),
        });
    }

    struct Hierarchy final {
        const char* name;
        const char* super_name;
    };
    static constexpr std::array<Hierarchy, 5> hierarchy {{
        {"javax/microedition/rms/RecordStoreException", "java/lang/Exception"},
        {"javax/microedition/rms/RecordStoreFullException",
         "javax/microedition/rms/RecordStoreException"},
        {"javax/microedition/rms/RecordStoreNotFoundException",
         "javax/microedition/rms/RecordStoreException"},
        {"javax/microedition/rms/RecordStoreNotOpenException",
         "javax/microedition/rms/RecordStoreException"},
        {"javax/microedition/rms/InvalidRecordIDException",
         "javax/microedition/rms/RecordStoreException"},
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

void register_rms_classes(BuiltinClassRegistry& registry) {
    registry.add_factory(build_rms_class);
}

} // namespace phoneme::vm
