#include "phoneme/vm/BuiltinClassRegistry.hpp"

#include <array>

#include "BuiltinClassSupport.hpp"

namespace phoneme::vm {
namespace {

using namespace builtin;

[[nodiscard]] ClassPtr build_io_class(std::string_view name) {
    if (name == "java/io/InputStream") {
        return make_class("java/io/InputStream", "java/lang/Object",
                          kOrdinary | kAbstract, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic | kAbstract, "read", "()I"),
            method(kPublic, "read", "([B)I"),
            method(kPublic, "read", "([BII)I"),
            method(kPublic, "skip", "(J)J"),
            method(kPublic, "available", "()I"),
            method(kPublic, "close", "()V"),
            method(kPublic | kSynchronized, "mark", "(I)V"),
            method(kPublic | kSynchronized, "reset", "()V"),
            method(kPublic, "markSupported", "()Z"),
        }, {"java/io/Closeable"});
    }
    if (name == "java/io/OutputStream") {
        return make_class("java/io/OutputStream", "java/lang/Object",
                          kOrdinary | kAbstract, {}, {
            method(kPublic, "<init>", "()V"),
            method(kPublic | kAbstract, "write", "(I)V"),
            method(kPublic, "write", "([B)V"),
            method(kPublic, "write", "([BII)V"),
            method(kPublic, "flush", "()V"),
            method(kPublic, "close", "()V"),
        }, {"java/io/Closeable"});
    }
    if (name == "java/io/Reader") {
        return make_class("java/io/Reader", "java/lang/Object",
                          kOrdinary | kAbstract, {
            field(kProtected, "lock", "Ljava/lang/Object;"),
        }, {
            method(kProtected, "<init>", "()V"),
            method(kProtected, "<init>", "(Ljava/lang/Object;)V"),
            method(kPublic, "read", "()I"),
            method(kPublic, "read", "([C)I"),
            method(kPublic | kAbstract, "read", "([CII)I"),
            method(kPublic, "skip", "(J)J"),
            method(kPublic, "ready", "()Z"),
            method(kPublic, "markSupported", "()Z"),
            method(kPublic, "mark", "(I)V"),
            method(kPublic, "reset", "()V"),
            method(kPublic | kAbstract, "close", "()V"),
        }, {"java/io/Closeable"});
    }
    if (name == "java/io/Writer") {
        return make_class("java/io/Writer", "java/lang/Object",
                          kOrdinary | kAbstract, {
            field(kProtected, "lock", "Ljava/lang/Object;"),
        }, {
            method(kProtected, "<init>", "()V"),
            method(kProtected, "<init>", "(Ljava/lang/Object;)V"),
            method(kPublic, "write", "(I)V"),
            method(kPublic, "write", "([C)V"),
            method(kPublic | kAbstract, "write", "([CII)V"),
            method(kPublic, "write", "(Ljava/lang/String;)V"),
            method(kPublic, "write", "(Ljava/lang/String;II)V"),
            method(kPublic | kAbstract, "flush", "()V"),
            method(kPublic | kAbstract, "close", "()V"),
        }, {"java/io/Closeable"});
    }
    if (name == "java/io/InputStreamReader") {
        return make_class("java/io/InputStreamReader", "java/io/Reader",
                          kOrdinary, {
            field(kPrivate, "in", "Ljava/io/InputStream;"),
            field(kPrivate, "charset", "I"),
            field(kPrivate, "pendingChar", "I"),
            field(kPrivate, "closed", "Z"),
        }, {
            method(kPublic, "<init>", "(Ljava/io/InputStream;)V"),
            method(kPublic, "<init>",
                   "(Ljava/io/InputStream;Ljava/lang/String;)V"),
            method(kPublic, "<init>",
                   "(Ljava/io/InputStream;Ljava/nio/charset/Charset;)V"),
            method(kPublic, "getEncoding", "()Ljava/lang/String;"),
            method(kPublic, "read", "()I"),
            method(kPublic, "read", "([CII)I"),
            method(kPublic, "skip", "(J)J"),
            method(kPublic, "ready", "()Z"),
            method(kPublic, "markSupported", "()Z"),
            method(kPublic, "mark", "(I)V"),
            method(kPublic, "reset", "()V"),
            method(kPublic, "close", "()V"),
        });
    }
    if (name == "java/io/StringReader") {
        return make_class("java/io/StringReader", "java/io/Reader",
                          kOrdinary, {
            field(kPrivate, "str", "Ljava/lang/String;"),
            field(kPrivate, "next", "I"),
            field(kPrivate, "mark", "I"),
            field(kPrivate, "closed", "Z"),
        }, {
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            method(kPublic, "read", "()I"),
            method(kPublic, "read", "([CII)I"),
            method(kPublic, "skip", "(J)J"),
            method(kPublic, "ready", "()Z"),
            method(kPublic, "markSupported", "()Z"),
            method(kPublic, "mark", "(I)V"),
            method(kPublic, "reset", "()V"),
            method(kPublic, "close", "()V"),
        });
    }
    if (name == "java/io/OutputStreamWriter") {
        return make_class("java/io/OutputStreamWriter", "java/io/Writer",
                          kOrdinary, {
            field(kPrivate, "out", "Ljava/io/OutputStream;"),
            field(kPrivate, "charset", "I"),
            field(kPrivate, "pendingHigh", "I"),
            field(kPrivate, "closed", "Z"),
        }, {
            method(kPublic, "<init>", "(Ljava/io/OutputStream;)V"),
            method(kPublic, "<init>",
                   "(Ljava/io/OutputStream;Ljava/lang/String;)V"),
            method(kPublic, "<init>",
                   "(Ljava/io/OutputStream;Ljava/nio/charset/Charset;)V"),
            method(kPublic, "getEncoding", "()Ljava/lang/String;"),
            method(kPublic, "write", "(I)V"),
            method(kPublic, "write", "([CII)V"),
            method(kPublic, "write", "(Ljava/lang/String;II)V"),
            method(kPublic, "flush", "()V"),
            method(kPublic, "close", "()V"),
        });
    }
    if (name == "java/io/BufferedReader") {
        return make_class("java/io/BufferedReader", "java/io/Reader",
                          kOrdinary, {
            field(kPrivate, "in", "Ljava/io/Reader;"),
            field(kPrivate, "closed", "Z"),
        }, {
            method(kPublic, "<init>", "(Ljava/io/Reader;)V"),
            method(kPublic, "read", "()I"),
            method(kPublic, "read", "([CII)I"),
            method(kPublic, "readLine", "()Ljava/lang/String;"),
            method(kPublic, "ready", "()Z"),
            method(kPublic, "close", "()V"),
        });
    }
    if (name == "java/io/PrintWriter") {
        return make_class("java/io/PrintWriter", "java/io/Writer",
                          kOrdinary, {
            field(kPrivate, "out", "Ljava/io/Writer;"),
            field(kPrivate, "trouble", "Z"),
            field(kPrivate, "autoFlush", "Z"),
            field(kPrivate, "closed", "Z"),
        }, {
            method(kPublic, "<init>", "(Ljava/io/Writer;)V"),
            method(kPublic, "<init>", "(Ljava/io/Writer;Z)V"),
            method(kPublic, "print", "(Ljava/lang/String;)V"),
            method(kPublic, "println", "()V"),
            method(kPublic, "println", "(Ljava/lang/String;)V"),
            method(kPublic, "flush", "()V"),
            method(kPublic, "close", "()V"),
            method(kPublic, "checkError", "()Z"),
        });
    }
    if (name == "java/io/FilterInputStream") {
        return make_class("java/io/FilterInputStream", "java/io/InputStream",
                          kOrdinary, {
            field(kProtected, "in", "Ljava/io/InputStream;"),
        }, {
            method(kProtected, "<init>", "(Ljava/io/InputStream;)V"),
            method(kPublic, "read", "()I"),
            method(kPublic, "read", "([B)I"),
            method(kPublic, "read", "([BII)I"),
            method(kPublic, "skip", "(J)J"),
            method(kPublic, "available", "()I"),
            method(kPublic, "close", "()V"),
            method(kPublic | kSynchronized, "mark", "(I)V"),
            method(kPublic | kSynchronized, "reset", "()V"),
            method(kPublic, "markSupported", "()Z"),
        });
    }
    if (name == "java/io/FilterOutputStream") {
        return make_class("java/io/FilterOutputStream", "java/io/OutputStream",
                          kOrdinary, {
            field(kProtected, "out", "Ljava/io/OutputStream;"),
        }, {
            method(kProtected, "<init>", "(Ljava/io/OutputStream;)V"),
            method(kPublic, "write", "(I)V"),
            method(kPublic, "write", "([B)V"),
            method(kPublic, "write", "([BII)V"),
            method(kPublic, "flush", "()V"),
            method(kPublic, "close", "()V"),
        });
    }
    if (name == "java/io/BufferedInputStream") {
        return make_class("java/io/BufferedInputStream",
                          "java/io/FilterInputStream", kOrdinary, {}, {
            method(kPublic, "<init>", "(Ljava/io/InputStream;)V"),
            method(kPublic, "<init>", "(Ljava/io/InputStream;I)V"),
        });
    }
    if (name == "java/io/BufferedOutputStream") {
        return make_class("java/io/BufferedOutputStream",
                          "java/io/FilterOutputStream", kOrdinary, {}, {
            method(kPublic, "<init>", "(Ljava/io/OutputStream;)V"),
            method(kPublic, "<init>", "(Ljava/io/OutputStream;I)V"),
        });
    }
    if (name == "java/io/PrintStream") {
        return make_class("java/io/PrintStream", "java/io/OutputStream",
                          kOrdinary, {
            // CLDC PrintStream extends OutputStream directly and owns its
            // delegate. Keep this field first: ConsoleNatives intentionally
            // uses the stable native layout [out, trouble, autoFlush, console].
            field(kPrivate, "out", "Ljava/io/OutputStream;"),
            field(kPrivate, "trouble", "Z"),
            field(kPrivate, "autoFlush", "Z"),
            field(kPrivate, "console", "Z"),
        }, {
            method(kPublic, "<init>", "(Ljava/io/OutputStream;)V"),
            method(kPublic, "<init>", "(Ljava/io/OutputStream;Z)V"),
            method(kPublic, "flush", "()V"),
            method(kPublic, "close", "()V"),
            method(kPublic, "checkError", "()Z"),
            method(kProtected, "setError", "()V"),
            method(kPublic, "write", "(I)V"),
            method(kPublic, "write", "([BII)V"),
            method(kPublic, "print", "(Z)V"),
            method(kPublic, "print", "(C)V"),
            method(kPublic, "print", "(I)V"),
            method(kPublic, "print", "(J)V"),
            method(kPublic, "print", "(F)V"),
            method(kPublic, "print", "(D)V"),
            method(kPublic, "print", "([C)V"),
            method(kPublic, "print", "(Ljava/lang/String;)V"),
            method(kPublic, "print", "(Ljava/lang/Object;)V"),
            method(kPublic, "println", "()V"),
            method(kPublic, "println", "(Z)V"),
            method(kPublic, "println", "(C)V"),
            method(kPublic, "println", "(I)V"),
            method(kPublic, "println", "(J)V"),
            method(kPublic, "println", "(F)V"),
            method(kPublic, "println", "(D)V"),
            method(kPublic, "println", "([C)V"),
            method(kPublic, "println", "(Ljava/lang/String;)V"),
            method(kPublic, "println", "(Ljava/lang/Object;)V"),
        });
    }
    if (name == "java/io/ByteArrayInputStream") {
        return make_class("java/io/ByteArrayInputStream", "java/io/InputStream",
                          kOrdinary, {
            field(kProtected, "buf", "[B"),
            field(kProtected, "pos", "I"),
            field(kProtected, "mark", "I"),
            field(kProtected, "count", "I"),
        }, {
            method(kPublic, "<init>", "([B)V"),
            method(kPublic, "<init>", "([BII)V"),
            method(kPublic | kSynchronized, "read", "()I"),
            method(kPublic | kSynchronized, "read", "([BII)I"),
            method(kPublic | kSynchronized, "skip", "(J)J"),
            method(kPublic | kSynchronized, "available", "()I"),
            method(kPublic, "markSupported", "()Z"),
            method(kPublic | kSynchronized, "mark", "(I)V"),
            method(kPublic | kSynchronized, "reset", "()V"),
            method(kPublic, "close", "()V"),
        });
    }
    if (name == "java/io/ByteArrayOutputStream") {
        return make_class("java/io/ByteArrayOutputStream", "java/io/OutputStream",
                          kOrdinary, {
            field(kProtected, "buf", "[B"),
            field(kProtected, "count", "I"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(I)V"),
            method(kPublic | kSynchronized, "write", "(I)V"),
            method(kPublic | kSynchronized, "write", "([BII)V"),
            method(kPublic | kSynchronized, "writeTo", "(Ljava/io/OutputStream;)V"),
            method(kPublic | kSynchronized, "reset", "()V"),
            method(kPublic | kSynchronized, "toByteArray", "()[B"),
            method(kPublic | kSynchronized, "size", "()I"),
            method(kPublic | kSynchronized, "toString", "()Ljava/lang/String;"),
            method(kPublic | kSynchronized, "toString",
                   "(Ljava/lang/String;)Ljava/lang/String;"),
            method(kPublic, "close", "()V"),
        });
    }
    if (name == "java/io/DataInput") {
        return make_class("java/io/DataInput", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "readFully", "([B)V"),
            method(kPublic | kAbstract, "readFully", "([BII)V"),
            method(kPublic | kAbstract, "skipBytes", "(I)I"),
            method(kPublic | kAbstract, "readBoolean", "()Z"),
            method(kPublic | kAbstract, "readByte", "()B"),
            method(kPublic | kAbstract, "readUnsignedByte", "()I"),
            method(kPublic | kAbstract, "readShort", "()S"),
            method(kPublic | kAbstract, "readUnsignedShort", "()I"),
            method(kPublic | kAbstract, "readChar", "()C"),
            method(kPublic | kAbstract, "readInt", "()I"),
            method(kPublic | kAbstract, "readLong", "()J"),
            method(kPublic | kAbstract, "readFloat", "()F"),
            method(kPublic | kAbstract, "readDouble", "()D"),
            method(kPublic | kAbstract, "readUTF", "()Ljava/lang/String;"),
        });
    }
    if (name == "java/io/DataOutput") {
        return make_class("java/io/DataOutput", "java/lang/Object",
                          kPublic | kInterface | kAbstract, {}, {
            method(kPublic | kAbstract, "write", "(I)V"),
            method(kPublic | kAbstract, "write", "([B)V"),
            method(kPublic | kAbstract, "write", "([BII)V"),
            method(kPublic | kAbstract, "writeBoolean", "(Z)V"),
            method(kPublic | kAbstract, "writeByte", "(I)V"),
            method(kPublic | kAbstract, "writeShort", "(I)V"),
            method(kPublic | kAbstract, "writeChar", "(I)V"),
            method(kPublic | kAbstract, "writeInt", "(I)V"),
            method(kPublic | kAbstract, "writeLong", "(J)V"),
            method(kPublic | kAbstract, "writeFloat", "(F)V"),
            method(kPublic | kAbstract, "writeDouble", "(D)V"),
            method(kPublic | kAbstract, "writeBytes", "(Ljava/lang/String;)V"),
            method(kPublic | kAbstract, "writeChars", "(Ljava/lang/String;)V"),
            method(kPublic | kAbstract, "writeUTF", "(Ljava/lang/String;)V"),
        });
    }
    if (name == "java/io/DataInputStream") {
        return make_class("java/io/DataInputStream", "java/io/InputStream",
                          kOrdinary, {
            field(kProtected, "in", "Ljava/io/InputStream;"),
        }, {
            method(kPublic, "<init>", "(Ljava/io/InputStream;)V"),
            method(kPublic, "read", "()I"),
            method(kPublic | kFinal, "read", "([B)I"),
            method(kPublic | kFinal, "read", "([BII)I"),
            method(kPublic, "skip", "(J)J"),
            method(kPublic, "available", "()I"),
            method(kPublic, "close", "()V"),
            method(kPublic | kSynchronized, "mark", "(I)V"),
            method(kPublic | kSynchronized, "reset", "()V"),
            method(kPublic, "markSupported", "()Z"),
            method(kPublic | kFinal, "readFully", "([B)V"),
            method(kPublic | kFinal, "readFully", "([BII)V"),
            method(kPublic | kFinal, "skipBytes", "(I)I"),
            method(kPublic | kFinal, "readBoolean", "()Z"),
            method(kPublic | kFinal, "readByte", "()B"),
            method(kPublic | kFinal, "readUnsignedByte", "()I"),
            method(kPublic | kFinal, "readShort", "()S"),
            method(kPublic | kFinal, "readUnsignedShort", "()I"),
            method(kPublic | kFinal, "readChar", "()C"),
            method(kPublic | kFinal, "readInt", "()I"),
            method(kPublic | kFinal, "readLong", "()J"),
            method(kPublic | kFinal, "readFloat", "()F"),
            method(kPublic | kFinal, "readDouble", "()D"),
            method(kPublic | kFinal, "readUTF", "()Ljava/lang/String;"),
            method(kPublic | kStatic | kFinal, "readUTF",
                   "(Ljava/io/DataInput;)Ljava/lang/String;"),
        }, {"java/io/DataInput"});
    }
    if (name == "java/io/DataOutputStream") {
        return make_class("java/io/DataOutputStream", "java/io/OutputStream",
                          kOrdinary, {
            field(kProtected, "out", "Ljava/io/OutputStream;"),
            field(kProtected, "written", "I"),
        }, {
            method(kPublic, "<init>", "(Ljava/io/OutputStream;)V"),
            method(kPublic | kSynchronized, "write", "(I)V"),
            method(kPublic | kSynchronized, "write", "([BII)V"),
            method(kPublic, "flush", "()V"),
            method(kPublic, "close", "()V"),
            method(kPublic | kFinal, "writeBoolean", "(Z)V"),
            method(kPublic | kFinal, "writeByte", "(I)V"),
            method(kPublic | kFinal, "writeShort", "(I)V"),
            method(kPublic | kFinal, "writeChar", "(I)V"),
            method(kPublic | kFinal, "writeInt", "(I)V"),
            method(kPublic | kFinal, "writeLong", "(J)V"),
            method(kPublic | kFinal, "writeFloat", "(F)V"),
            method(kPublic | kFinal, "writeDouble", "(D)V"),
            method(kPublic | kFinal, "writeBytes", "(Ljava/lang/String;)V"),
            method(kPublic | kFinal, "writeChars", "(Ljava/lang/String;)V"),
            method(kPublic | kFinal, "writeUTF", "(Ljava/lang/String;)V"),
            method(kPublic | kFinal, "size", "()I"),
        }, {"java/io/DataOutput"});
    }
    if (name == "java/nio/charset/Charset") {
        return make_class("java/nio/charset/Charset", "java/lang/Object",
                          kOrdinary | kFinal, {
            field(kPrivate | kFinal, "canonicalName", "Ljava/lang/String;"),
        }, {
            method(kPrivate, "<init>", "(Ljava/lang/String;)V"),
            method(kPublic | kStatic, "forName",
                   "(Ljava/lang/String;)Ljava/nio/charset/Charset;"),
            method(kPublic | kFinal, "name", "()Ljava/lang/String;"),
            method(kPublic | kFinal, "toString", "()Ljava/lang/String;"),
        });
    }
    if (name == "java/nio/charset/StandardCharsets") {
        return make_class("java/nio/charset/StandardCharsets",
                          "java/lang/Object", kOrdinary | kFinal,
                          {
                              field(kPublic | kStatic | kFinal, "US_ASCII",
                                    "Ljava/nio/charset/Charset;"),
                              field(kPublic | kStatic | kFinal, "ISO_8859_1",
                                    "Ljava/nio/charset/Charset;"),
                              field(kPublic | kStatic | kFinal, "UTF_8",
                                    "Ljava/nio/charset/Charset;"),
                              field(kPublic | kStatic | kFinal, "UTF_16BE",
                                    "Ljava/nio/charset/Charset;"),
                          }, {
                              method(kPrivate, "<init>", "()V"),
                              method(kStatic, "<clinit>", "()V"),
                          });
    }
    if (name == "java/io/Serializable") {
        return make_class("java/io/Serializable", "java/lang/Object",
                          kPublic | kInterface | kAbstract);
    }

    if (name == "java/io/InterruptedIOException") {
        return make_class("java/io/InterruptedIOException", "java/io/IOException",
                          kOrdinary, {
            field(kPublic, "bytesTransferred", "I"),
        }, {
            method(kPublic, "<init>", "()V"),
            method(kPublic, "<init>", "(Ljava/lang/String;)V"),
        });
    }

    struct Hierarchy final {
        const char* name;
        const char* super_name;
    };
    static constexpr std::array<Hierarchy, 5> hierarchy {{
        {"java/io/IOException", "java/lang/Exception"},
        {"java/io/EOFException", "java/io/IOException"},
        {"java/io/InterruptedIOException", "java/io/IOException"},
        {"java/io/UTFDataFormatException", "java/io/IOException"},
        {"java/io/UnsupportedEncodingException", "java/io/IOException"},
    }};
    for (const Hierarchy& entry : hierarchy) {
        if (name == entry.name) {
            std::vector<classfile::Method> constructors {
                method(kPublic, "<init>", "()V"),
                method(kPublic, "<init>", "(Ljava/lang/String;)V"),
            };
            if (name == "java/io/IOException") {
                constructors.push_back(method(
                    kPublic, "<init>",
                    "(Ljava/lang/String;Ljava/lang/Throwable;)V"));
                constructors.push_back(method(
                    kPublic, "<init>", "(Ljava/lang/Throwable;)V"));
            }
            return make_class(entry.name, entry.super_name, kOrdinary, {},
                              std::move(constructors));
        }
    }

    return nullptr;
}

} // namespace

void register_io_classes(BuiltinClassRegistry& registry) {
    registry.add_factory(build_io_class);
}

} // namespace phoneme::vm
