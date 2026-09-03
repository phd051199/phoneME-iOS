#include "phoneme/vm/BuiltinClassRegistry.hpp"

#include <string_view>

#include "BuiltinClassSupport.hpp"

namespace phoneme::vm {
namespace {

using namespace builtin;

[[nodiscard]] BuiltinClassRegistry::ClassPtr build_file_class(
    std::string_view name) {
    if (name == "java/io/File") {
        return make_class("java/io/File", "java/lang/Object",
                          kOrdinary, {
                              field(kPrivate | kFinal, "path", "Ljava/lang/String;"),
                          }, {
                              method(kPublic, "<init>", "(Ljava/lang/String;)V"),
                              method(kPublic, "getName", "()Ljava/lang/String;"),
                              method(kPublic, "getParent", "()Ljava/lang/String;"),
                              method(kPublic, "getParentFile", "()Ljava/io/File;"),
                              method(kPublic, "getPath", "()Ljava/lang/String;"),
                              method(kPublic, "getAbsolutePath", "()Ljava/lang/String;"),
                              method(kPublic, "isAbsolute", "()Z"),
                              method(kPublic, "exists", "()Z"),
                              method(kPublic, "isDirectory", "()Z"),
                              method(kPublic, "isFile", "()Z"),
                              method(kPublic, "length", "()J"),
                              method(kPublic, "lastModified", "()J"),
                              method(kPublic, "mkdir", "()Z"),
                              method(kPublic, "mkdirs", "()Z"),
                              method(kPublic, "listFiles", "()[Ljava/io/File;"),
                              method(kPublic, "delete", "()Z"),
                              method(kPublic, "renameTo", "(Ljava/io/File;)Z"),
                              method(kPublic, "toString", "()Ljava/lang/String;"),
                          }, {"java/io/Serializable"});
    }
    if (name == "java/io/FileInputStream") {
        return make_class("java/io/FileInputStream", "java/io/InputStream",
                          kOrdinary | kFinal,
                          {
                              field(kPrivate, "handle", "I"),
                              field(kPrivate, "closed", "Z"),
                          },
                          {
                              method(kPublic, "<init>", "(Ljava/lang/String;)V"),
                              method(kPublic, "<init>", "(Ljava/io/File;)V"),
                              method(kPublic, "read", "()I"),
                              method(kPublic, "read", "([B)I"),
                              method(kPublic, "read", "([BII)I"),
                              method(kPublic, "skip", "(J)J"),
                              method(kPublic, "available", "()I"),
                              method(kPublic, "close", "()V"),
                          });
    }
    if (name == "java/io/FileOutputStream") {
        return make_class("java/io/FileOutputStream", "java/io/OutputStream",
                          kOrdinary | kFinal,
                          {
                              field(kPrivate, "handle", "I"),
                              field(kPrivate, "closed", "Z"),
                          },
                          {
                              method(kPublic, "<init>", "(Ljava/lang/String;)V"),
                              method(kPublic, "<init>", "(Ljava/lang/String;Z)V"),
                              method(kPublic, "<init>", "(Ljava/io/File;)V"),
                              method(kPublic, "<init>", "(Ljava/io/File;Z)V"),
                              method(kPublic, "write", "(I)V"),
                              method(kPublic, "write", "([B)V"),
                              method(kPublic, "write", "([BII)V"),
                              method(kPublic, "flush", "()V"),
                              method(kPublic, "close", "()V"),
                          });
    }
    if (name == "java/io/FileNotFoundException") {
        return make_class("java/io/FileNotFoundException", "java/io/IOException",
                          kOrdinary, {},
                          {method(kPublic, "<init>", "()V")});
    }
    if (name == "javax/microedition/io/file/ConnectionClosedException") {
        return make_class("javax/microedition/io/file/ConnectionClosedException",
                          "java/lang/RuntimeException", kOrdinary, {},
                          {
                              method(kPublic, "<init>", "()V"),
                              method(kPublic, "<init>", "(Ljava/lang/String;)V"),
                          });
    }
    if (name == "javax/microedition/io/file/IllegalModeException") {
        return make_class("javax/microedition/io/file/IllegalModeException",
                          "java/lang/RuntimeException", kOrdinary, {},
                          {
                              method(kPublic, "<init>", "()V"),
                              method(kPublic, "<init>", "(Ljava/lang/String;)V"),
                          });
    }
    if (name == "javax/microedition/io/Connection") {
        return make_class("javax/microedition/io/Connection", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {},
                          {method(kPublic | kAbstract, "close", "()V")});
    }
    if (name == "javax/microedition/io/InputConnection") {
        return make_class("javax/microedition/io/InputConnection",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {},
                          {
                              method(kPublic | kAbstract, "openInputStream", "()Ljava/io/InputStream;"),
                              method(kPublic | kAbstract, "openDataInputStream", "()Ljava/io/DataInputStream;"),
                          },
                          {"javax/microedition/io/Connection"});
    }
    if (name == "javax/microedition/io/OutputConnection") {
        return make_class("javax/microedition/io/OutputConnection",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {},
                          {
                              method(kPublic | kAbstract, "openOutputStream", "()Ljava/io/OutputStream;"),
                              method(kPublic | kAbstract, "openDataOutputStream", "()Ljava/io/DataOutputStream;"),
                          },
                          {"javax/microedition/io/Connection"});
    }
    if (name == "javax/microedition/io/StreamConnection") {
        return make_class("javax/microedition/io/StreamConnection",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {},
                          {"javax/microedition/io/InputConnection",
                           "javax/microedition/io/OutputConnection"});
    }
    if (name == "javax/microedition/io/Connector") {
        return make_class("javax/microedition/io/Connector", "java/lang/Object",
                          kOrdinary | kFinal,
                          {
                              field(kPublic | kStatic | kFinal, "READ", "I"),
                              field(kPublic | kStatic | kFinal, "WRITE", "I"),
                              field(kPublic | kStatic | kFinal, "READ_WRITE", "I"),
                          },
                          {
                              method(kStatic, "<clinit>", "()V"),
                              method(kPublic | kStatic, "open", "(Ljava/lang/String;)Ljavax/microedition/io/Connection;"),
                              method(kPublic | kStatic, "open", "(Ljava/lang/String;I)Ljavax/microedition/io/Connection;"),
                              method(kPublic | kStatic, "open", "(Ljava/lang/String;IZ)Ljavax/microedition/io/Connection;"),
                              method(kPublic | kStatic, "openInputStream", "(Ljava/lang/String;)Ljava/io/InputStream;"),
                              method(kPublic | kStatic, "openDataInputStream", "(Ljava/lang/String;)Ljava/io/DataInputStream;"),
                              method(kPublic | kStatic, "openOutputStream", "(Ljava/lang/String;)Ljava/io/OutputStream;"),
                              method(kPublic | kStatic, "openDataOutputStream", "(Ljava/lang/String;)Ljava/io/DataOutputStream;"),
                          });
    }
    if (name == "javax/microedition/io/file/FileSystemListener") {
        return make_class("javax/microedition/io/file/FileSystemListener",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {
            field(kPublic | kStatic | kFinal, "ROOT_ADDED", "I"),
            field(kPublic | kStatic | kFinal, "ROOT_REMOVED", "I"),
        }, {
            method(kPublic | kAbstract, "rootChanged",
                   "(ILjava/lang/String;)V"),
        });
    }
    if (name == "javax/microedition/io/file/FileSystemRegistry") {
        return make_class("javax/microedition/io/file/FileSystemRegistry",
                          "java/lang/Object", kOrdinary | kFinal, {
                              field(kPrivate | kStatic, "listeners",
                                    "Ljava/util/Vector;"),
                          },
                          {
                              method(kPrivate, "<init>", "()V"),
                              method(kPublic | kStatic, "listRoots",
                                     "()Ljava/util/Enumeration;"),
                              method(kPublic | kStatic,
                                     "addFileSystemListener",
                                     "(Ljavax/microedition/io/file/"
                                     "FileSystemListener;)Z"),
                              method(kPublic | kStatic,
                                     "removeFileSystemListener",
                                     "(Ljavax/microedition/io/file/"
                                     "FileSystemListener;)Z"),
                          });
    }
    if (name == "javax/microedition/io/file/FileConnection") {
        return make_class("javax/microedition/io/file/FileConnection",
                          "java/lang/Object",
                          kPublic | kInterface | kAbstract, {},
                          {
                              method(kPublic | kAbstract, "fileSize", "()J"),
                              method(kPublic | kAbstract, "directorySize", "(Z)J"),
                              method(kPublic | kAbstract, "availableSize", "()J"),
                              method(kPublic | kAbstract, "totalSize", "()J"),
                              method(kPublic | kAbstract, "usedSize", "()J"),
                              method(kPublic | kAbstract, "isOpen", "()Z"),
                              method(kPublic | kAbstract, "canRead", "()Z"),
                              method(kPublic | kAbstract, "canWrite", "()Z"),
                              method(kPublic | kAbstract, "setReadable", "(Z)V"),
                              method(kPublic | kAbstract, "setWritable", "(Z)V"),
                              method(kPublic | kAbstract, "isHidden", "()Z"),
                              method(kPublic | kAbstract, "setHidden", "(Z)V"),
                              method(kPublic | kAbstract, "getName", "()Ljava/lang/String;"),
                              method(kPublic | kAbstract, "getPath", "()Ljava/lang/String;"),
                              method(kPublic | kAbstract, "getURL", "()Ljava/lang/String;"),
                              method(kPublic | kAbstract, "exists", "()Z"),
                              method(kPublic | kAbstract, "isDirectory", "()Z"),
                              method(kPublic | kAbstract, "lastModified", "()J"),
                              method(kPublic | kAbstract, "create", "()V"),
                              method(kPublic | kAbstract, "mkdir", "()V"),
                              method(kPublic | kAbstract, "delete", "()V"),
                              method(kPublic | kAbstract, "rename", "(Ljava/lang/String;)V"),
                              method(kPublic | kAbstract, "setFileConnection", "(Ljava/lang/String;)V"),
                              method(kPublic | kAbstract, "truncate", "(J)V"),
                              method(kPublic | kAbstract, "list", "()Ljava/util/Enumeration;"),
                              method(kPublic | kAbstract, "list", "(Ljava/lang/String;Z)Ljava/util/Enumeration;"),
                              method(kPublic | kAbstract, "openInputStream", "()Ljava/io/InputStream;"),
                              method(kPublic | kAbstract, "openDataInputStream", "()Ljava/io/DataInputStream;"),
                              method(kPublic | kAbstract, "openOutputStream", "()Ljava/io/OutputStream;"),
                              method(kPublic | kAbstract, "openOutputStream", "(J)Ljava/io/OutputStream;"),
                              method(kPublic | kAbstract, "openDataOutputStream", "()Ljava/io/DataOutputStream;"),
                              method(kPublic | kAbstract, "close", "()V"),
                          },
                          {"javax/microedition/io/StreamConnection"});
    }
    if (name == "javax/microedition/io/file/FileConnectionImpl") {
        return make_class("javax/microedition/io/file/FileConnectionImpl",
                          "java/lang/Object", kOrdinary | kFinal,
                          {
                              field(kPrivate, "path", "Ljava/lang/String;"),
                              field(kPrivate, "mode", "I"),
                              field(kPrivate, "open", "Z"),
                              field(kPrivate, "input", "Ljava/io/FileInputStream;"),
                              field(kPrivate, "output", "Ljava/io/FileOutputStream;"),
                          },
                          {
                              method(kPublic, "fileSize", "()J"),
                              method(kPublic, "directorySize", "(Z)J"),
                              method(kPublic, "availableSize", "()J"),
                              method(kPublic, "totalSize", "()J"),
                              method(kPublic, "usedSize", "()J"),
                              method(kPublic, "isOpen", "()Z"),
                              method(kPublic, "canRead", "()Z"),
                              method(kPublic, "canWrite", "()Z"),
                              method(kPublic, "setReadable", "(Z)V"),
                              method(kPublic, "setWritable", "(Z)V"),
                              method(kPublic, "isHidden", "()Z"),
                              method(kPublic, "setHidden", "(Z)V"),
                              method(kPublic, "getName", "()Ljava/lang/String;"),
                              method(kPublic, "getPath", "()Ljava/lang/String;"),
                              method(kPublic, "getURL", "()Ljava/lang/String;"),
                              method(kPublic, "exists", "()Z"),
                              method(kPublic, "isDirectory", "()Z"),
                              method(kPublic, "lastModified", "()J"),
                              method(kPublic, "create", "()V"),
                              method(kPublic, "mkdir", "()V"),
                              method(kPublic, "delete", "()V"),
                              method(kPublic, "rename", "(Ljava/lang/String;)V"),
                              method(kPublic, "setFileConnection", "(Ljava/lang/String;)V"),
                              method(kPublic, "truncate", "(J)V"),
                              method(kPublic, "list", "()Ljava/util/Enumeration;"),
                              method(kPublic, "list", "(Ljava/lang/String;Z)Ljava/util/Enumeration;"),
                              method(kPublic, "openInputStream", "()Ljava/io/InputStream;"),
                              method(kPublic, "openDataInputStream", "()Ljava/io/DataInputStream;"),
                              method(kPublic, "openOutputStream", "()Ljava/io/OutputStream;"),
                              method(kPublic, "openOutputStream", "(J)Ljava/io/OutputStream;"),
                              method(kPublic, "openDataOutputStream", "()Ljava/io/DataOutputStream;"),
                              method(kPublic, "close", "()V"),
                          },
                          {"javax/microedition/io/file/FileConnection"});
    }
    return nullptr;
}

} // namespace

void register_file_classes(BuiltinClassRegistry& registry) {
    registry.add_factory(build_file_class);
}

} // namespace phoneme::vm
