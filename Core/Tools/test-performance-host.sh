#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$CORE_ROOT/.." && pwd)"
OUTPUT_JSON="${1:-$CORE_ROOT/build/performance/synthetic-vm-performance.json}"
BUILD_ROOT="${PHONEME_PERF_BUILD_ROOT:-$CORE_ROOT/build/performance-host}"
BUILD_TYPE="${PHONEME_PERF_BUILD_TYPE:-Release}"

CMAKE="${CMAKE:-cmake}"
CXX="${CXX:-$(xcrun --find clang++)}"
SDK_ROOT="${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path)}"

"$CMAKE" \
  -S "$CORE_ROOT" \
  -B "$BUILD_ROOT" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DPHONEME_ENABLE_VM_PROFILING=ON \
  -DPHONEME_ENABLE_DECODED_EXECUTION=ON
"$CMAKE" --build "$BUILD_ROOT" --parallel "${PHONEME_TEST_JOBS:-4}"

COMMON_FLAGS=(
  -std=c++23
  -DPHONEME_ENABLE_VM_PROFILING=1
  -DPHONEME_ENABLE_DECODED_EXECUTION=1
  -isysroot "$SDK_ROOT"
  -I"$CORE_ROOT/include"
  -fno-exceptions
  -fno-rtti
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wsign-conversion
  -Wshadow
  -Werror=return-type
)
LINK_FLAGS=(
  "$BUILD_ROOT/libphoneMECore.a"
  -lz
  -framework CoreText
  -framework CoreGraphics
  -framework ImageIO
  -framework CoreFoundation
)

compile_test() {
  local source="$1"
  local binary="$2"
  "$CXX" "${COMMON_FLAGS[@]}" "$source" "${LINK_FLAGS[@]}" -o "$binary"
}

COUNTER_TEST="$BUILD_ROOT/performance-counter-tests"
METADATA_TEST="$BUILD_ROOT/runtime-metadata-tests"
DECODED_METHOD_TEST="$BUILD_ROOT/decoded-method-tests"
VM_PERFORMANCE_TEST="$BUILD_ROOT/vm-performance-tests"
COMPACT_STORAGE_TEST="$BUILD_ROOT/compact-storage-benchmark"
COMPACT_STORAGE_JSON="${PHONEME_COMPACT_STORAGE_JSON:-$CORE_ROOT/build/performance/compact-storage.json}"

compile_test "$CORE_ROOT/Tests/PerformanceCounterTests.cpp" "$COUNTER_TEST"
compile_test "$CORE_ROOT/Tests/RuntimeMetadataTests.cpp" "$METADATA_TEST"
compile_test "$CORE_ROOT/Tests/DecodedMethodTests.cpp" "$DECODED_METHOD_TEST"
compile_test "$CORE_ROOT/Tests/VmPerformanceTests.cpp" "$VM_PERFORMANCE_TEST"
compile_test "$CORE_ROOT/Tests/CompactStorageBenchmark.cpp" "$COMPACT_STORAGE_TEST"

"$COUNTER_TEST"
"$METADATA_TEST"
"$DECODED_METHOD_TEST"
"$VM_PERFORMANCE_TEST" "$OUTPUT_JSON"
"$COMPACT_STORAGE_TEST" "$COMPACT_STORAGE_JSON"

echo "Performance host tests passed"
echo "Synthetic benchmark: $OUTPUT_JSON"
echo "Compact storage benchmark: $COMPACT_STORAGE_JSON"
