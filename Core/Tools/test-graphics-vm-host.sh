#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/lib/common-test-root.sh"
TEST_ROOT="$(phoneme_make_isolated_root "$CORE_ROOT" "graphics-vm-host-tests" "${PHONEME_GRAPHICS_VM_TEST_ROOT:-}")"
phoneme_register_cleanup "$TEST_ROOT"
STUB_CLASSES="$TEST_ROOT/compile-stubs"
FIXTURE_CLASSES="$TEST_ROOT/fixture-classes"
FIXTURE_JAR="$TEST_ROOT/graphics-fixture.jar"
TEST_BINARY="$TEST_ROOT/GraphicsVmTests"
CXX="$(xcrun --sdk macosx --find clang++)"
SDK_ROOT="$(xcrun --sdk macosx --show-sdk-path)"
JAVAC="${JAVAC:-$(command -v javac)}"
JAR="${JAR:-$(command -v jar)}"
phoneme_configure_sanitizers
SANITIZER_FLAGS="$PHONEME_SANITIZER_FLAGS"

mkdir -p "$STUB_CLASSES" "$FIXTURE_CLASSES"

STUB_SOURCES=()
while IFS= read -r source; do
  STUB_SOURCES+=("$source")
done < <(find "$CORE_ROOT/Tests/stubs" -type f -name '*.java' -print | LC_ALL=C sort)

"$JAVAC" -source 8 -target 8 -Xlint:-options -Xlint:-unchecked \
  -d "$STUB_CLASSES" \
  "${STUB_SOURCES[@]}"
"$JAVAC" -source 8 -target 8 -Xlint:-options \
  -classpath "$STUB_CLASSES" \
  -d "$FIXTURE_CLASSES" \
  "$CORE_ROOT/Tests/fixtures/GraphicsOps.java"
"$JAR" cf "$FIXTURE_JAR" -C "$FIXTURE_CLASSES" .

SOURCES=(
  "$CORE_ROOT/src/archive/ZipArchive.cpp"
  "$CORE_ROOT/src/classfile/BytecodeVerifier.cpp"
  "$CORE_ROOT/src/classfile/ClassFile.cpp"
  "$CORE_ROOT/src/filesystem/FileSystem.cpp"
  "$CORE_ROOT/src/filesystem/ResourceLoader.cpp"
  "$CORE_ROOT/src/filesystem/SandboxResolver.cpp"
  "$CORE_ROOT/src/media/MediaService.cpp"
  "$CORE_ROOT/src/media/PlatformMediaAdapter.cpp"
  "$CORE_ROOT/src/network/ConnectionRegistry.cpp"
  "$CORE_ROOT/src/network/Url.cpp"
  "$CORE_ROOT/src/platform/MappedFile.cpp"
  "$CORE_ROOT/src/push/PushRegistry.cpp"
  "$CORE_ROOT/src/runtime/RecordStoreRegistry.cpp"
  "$CORE_ROOT/src/security/PermissionCatalog.cpp"
  "$CORE_ROOT/src/security/PermissionPolicy.cpp"
  "$CORE_ROOT/src/translation/TranslationService.cpp"
  "$CORE_ROOT/src/vm/AmmsBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/BluetoothBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/BaselineJit.cpp"
  "$CORE_ROOT/src/vm/BuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/BuiltinClassRegistry.cpp"
  "$CORE_ROOT/src/vm/ClassLayout.cpp"
  "$CORE_ROOT/src/vm/ClassRepository.cpp"
  "$CORE_ROOT/src/vm/ConnectionBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/DecodedMethod.cpp"
  "$CORE_ROOT/src/vm/Descriptor.cpp"
  "$CORE_ROOT/src/vm/ExecutionContext.cpp"
  "$CORE_ROOT/src/vm/FileBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/FileNatives.cpp"
  "$CORE_ROOT/src/vm/GameBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/GraphicsNatives.cpp"
  "$CORE_ROOT/src/vm/HeadlessCompatBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/Heap.cpp"
  "$CORE_ROOT/src/vm/IOBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/IONatives.cpp"
  "$CORE_ROOT/src/vm/ImageNatives.cpp"
  "$CORE_ROOT/src/vm/Interpreter.cpp"
  "$CORE_ROOT/src/vm/JavaFiber.cpp"
  "$CORE_ROOT/src/vm/JavaThread.cpp"
  "$CORE_ROOT/src/vm/Jdk8CompatBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/LangBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/LcduiBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/M3gBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/Machine.cpp"
  "$CORE_ROOT/src/vm/MediaBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/MediaEventService.cpp"
  "$CORE_ROOT/src/vm/Micro3dBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/MidletBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/ModifiedUtf8.cpp"
  "$CORE_ROOT/src/vm/MonitorTable.cpp"
  "$CORE_ROOT/src/vm/NativeMethodRegistry.cpp"
  "$CORE_ROOT/src/vm/NativeRootScope.cpp"
  "$CORE_ROOT/src/vm/PimBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/PushBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/RootSet.cpp"
  "$CORE_ROOT/src/vm/RmsBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/RuntimeMetadata.cpp"
  "$CORE_ROOT/src/vm/Scheduler.cpp"
  "$CORE_ROOT/src/vm/SecurityBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/SensorBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/TimerService.cpp"
  "$CORE_ROOT/src/vm/UtilBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/VendorBuiltinClasses.cpp"
  "$CORE_ROOT/src/vm/Verifier.cpp"
  "$CORE_ROOT/src/vm/XmlBuiltinClasses.cpp"
)
while IFS= read -r source; do
  SOURCES+=("$source")
done < <(find "$CORE_ROOT/src/graphics" -type f -name '*.cpp' -print | LC_ALL=C sort)
SOURCES+=("$CORE_ROOT/src/runtime/ParallelExecutor.cpp")
SOURCES+=("$CORE_ROOT/src/runtime/WorkCoordinator.cpp")

"$CXX" \
  -std=c++23 \
  -isysroot "$SDK_ROOT" \
  -I"$CORE_ROOT/include" \
  -I"$CORE_ROOT/src/vm" \
  -fno-exceptions \
  -fno-rtti \
  -Wall \
  -Wextra \
  -Wpedantic \
  -Wconversion \
  -Wsign-conversion \
  -Wshadow \
  -Werror=return-type \
  $SANITIZER_FLAGS \
  "$CORE_ROOT/Tests/GraphicsVmTests.cpp" \
  "${SOURCES[@]}" \
  -lz \
  -framework CoreText \
  -framework CoreGraphics \
  -framework ImageIO \
  -framework CoreFoundation \
  -o "$TEST_BINARY"

phoneme_run_with_timeout "${PHONEME_TEST_TIMEOUT:-300}" \
  "$TEST_BINARY" "$FIXTURE_JAR"
