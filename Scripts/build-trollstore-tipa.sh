#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_PATH="$REPO_ROOT/phoneME.xcodeproj"
SCHEME="${SCHEME:-phoneME}"
CONFIGURATION="Release"
OUTPUT_ROOT="${OUTPUT_ROOT:-$REPO_ROOT/Artifacts}"
INTERPRETER_ONLY=false
OUTPUT_TIPA="${OUTPUT_TIPA:-}"
DERIVED_DATA="${DERIVED_DATA:-}"
UNSIGNED_IPA=""
BUILD_LOG=""
REBUILD_CORE=false
SKIP_BUILD=false
CLEAN_BUILD=false
MAX_SOURCE_RETRIES=3

usage() {
  cat <<'USAGE'
Build phoneME for iPhone and package a TrollStore JIT-enabled .tipa.

Usage:
  bash Scripts/build-trollstore-tipa.sh [options]

Options:
  --rebuild-core       Run the Core host test suite before the iPhone build.
  --clean              Remove cached device build outputs first.
  --skip-build         Repackage the last unsigned device IPA.
  --interpreter-only   Build the same source with guest JIT disabled by default.
  --output PATH        Output .tipa path.
  --output-root PATH   Artifact directory.
  --derived-data PATH  Xcode DerivedData directory.
  -h, --help           Show this help.

Environment overrides:
  OUTPUT_ROOT, OUTPUT_TIPA, DERIVED_DATA, SCHEME

Examples:
  bash Scripts/build-trollstore-tipa.sh
  bash Scripts/build-trollstore-tipa.sh --interpreter-only
  bash Scripts/build-trollstore-tipa.sh --rebuild-core
  bash Scripts/build-trollstore-tipa.sh --skip-build --output ~/Desktop/phoneME.tipa

This build is intentionally unsigned. TrollStore does not need an Apple
provisioning profile; the final application is fake-signed with ldid using the
TrollStore external-JIT entitlements.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rebuild-core)
      REBUILD_CORE=true
      shift
      ;;
    --clean)
      CLEAN_BUILD=true
      shift
      ;;
    --skip-build)
      SKIP_BUILD=true
      shift
      ;;
    --interpreter-only)
      INTERPRETER_ONLY=true
      shift
      ;;
    --output)
      [[ $# -ge 2 ]] || { echo "Missing value for --output" >&2; exit 2; }
      OUTPUT_TIPA="$2"
      shift 2
      ;;
    --output-root)
      [[ $# -ge 2 ]] || { echo "Missing value for --output-root" >&2; exit 2; }
      OUTPUT_ROOT="$2"
      UNSIGNED_IPA="$OUTPUT_ROOT/phoneME-TrollStore-unsigned.ipa"
      BUILD_LOG="$OUTPUT_ROOT/phoneME-TrollStore-build.log"
      shift 2
      ;;
    --derived-data)
      [[ $# -ge 2 ]] || { echo "Missing value for --derived-data" >&2; exit 2; }
      DERIVED_DATA="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$INTERPRETER_ONLY" == true ]]; then
  OUTPUT_TIPA="${OUTPUT_TIPA:-$OUTPUT_ROOT/phoneME-TrollStore-Interpreter.tipa}"
  DERIVED_DATA="${DERIVED_DATA:-$REPO_ROOT/.build/trollstore-device-interpreter}"
  UNSIGNED_IPA="$OUTPUT_ROOT/phoneME-TrollStore-Interpreter-unsigned.ipa"
  BUILD_LOG="$OUTPUT_ROOT/phoneME-TrollStore-Interpreter-build.log"
else
  OUTPUT_TIPA="${OUTPUT_TIPA:-$OUTPUT_ROOT/phoneME-TrollStore-JIT.tipa}"
  DERIVED_DATA="${DERIVED_DATA:-$REPO_ROOT/.build/trollstore-device}"
  UNSIGNED_IPA="$OUTPUT_ROOT/phoneME-TrollStore-unsigned.ipa"
  BUILD_LOG="$OUTPUT_ROOT/phoneME-TrollStore-build.log"
fi

for command in xcodebuild zip unzip find mktemp cp rm mkdir tee grep; do
  command -v "$command" >/dev/null 2>&1 || {
    echo "Required command not found: $command" >&2
    exit 1
  }
done

[[ -d "$PROJECT_PATH" ]] || {
  echo "Xcode project not found: $PROJECT_PATH" >&2
  exit 1
}

mkdir -p "$OUTPUT_ROOT" "$(dirname "$OUTPUT_TIPA")"

if [[ "$CLEAN_BUILD" == true ]]; then
  rm -rf "$DERIVED_DATA"
  rm -f "$UNSIGNED_IPA" "$OUTPUT_TIPA"
fi

if [[ "$REBUILD_CORE" == true ]]; then
  echo "== Running phoneME Core host tests =="
  bash "$REPO_ROOT/Core/Tools/test-host.sh"
fi

run_device_build() {
  local swift_conditions='$(inherited)'
  local gcc_definitions='$(inherited) PHONEME_TROLLSTORE_BUILD=1'
  if [[ "$INTERPRETER_ONLY" == true ]]; then
    swift_conditions='$(inherited) PHONEME_INTERPRETER_ONLY'
    gcc_definitions='$(inherited) PHONEME_TROLLSTORE_BUILD=1 PHONEME_INTERPRETER_ONLY=1'
  fi
  set +e
  set -o pipefail
  xcodebuild \
    -project "$PROJECT_PATH" \
    -scheme "$SCHEME" \
    -configuration "$CONFIGURATION" \
    -destination 'generic/platform=iOS' \
    -derivedDataPath "$DERIVED_DATA" \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGNING_REQUIRED=NO \
    CODE_SIGN_IDENTITY='' \
    TARGETED_DEVICE_FAMILY=1 \
    "SWIFT_ACTIVE_COMPILATION_CONDITIONS=$swift_conditions" \
    "GCC_PREPROCESSOR_DEFINITIONS=$gcc_definitions" \
    build \
    2>&1 | tee "$BUILD_LOG"
  local status=${PIPESTATUS[0]}
  set +o pipefail
  set -e
  return "$status"
}

if [[ "$SKIP_BUILD" == false ]]; then
  attempt=1
  while true; do
    echo "== Building unsigned phoneME device app (attempt $attempt/$MAX_SOURCE_RETRIES) =="
    if run_device_build; then
      break
    fi

    if grep -q 'phoneME Core sources changed during compilation; retry the build.' "$BUILD_LOG" &&
       [[ "$attempt" -lt "$MAX_SOURCE_RETRIES" ]]; then
      attempt=$((attempt + 1))
      echo "Core changed during compilation; retrying from a fresh source snapshot."
      continue
    fi

    echo "Unsigned device build failed. See: $BUILD_LOG" >&2
    exit 1
  done

  APP_PATH="$DERIVED_DATA/Build/Products/${CONFIGURATION}-iphoneos/phoneME.app"
  if [[ ! -d "$APP_PATH" ]]; then
    APP_PATH="$(find "$DERIVED_DATA/Build/Products" -type d -path '*-iphoneos/*.app' -name 'phoneME.app' -print -quit)"
  fi
  [[ -n "${APP_PATH:-}" && -d "$APP_PATH" ]] || {
    echo "Built phoneME.app not found in $DERIVED_DATA" >&2
    exit 1
  }

  PACKAGE_ROOT="$(mktemp -d /tmp/phoneme-unsigned-ipa.XXXXXX)"
  cleanup_package() {
    rm -rf "$PACKAGE_ROOT"
  }
  trap cleanup_package EXIT

  mkdir -p "$PACKAGE_ROOT/Payload"
  cp -R "$APP_PATH" "$PACKAGE_ROOT/Payload/"
  rm -f "$UNSIGNED_IPA"
  (
    cd "$PACKAGE_ROOT"
    zip -qry "$UNSIGNED_IPA" Payload
  )
  unzip -tq "$UNSIGNED_IPA" >/dev/null
  cleanup_package
  trap - EXIT
fi

[[ -f "$UNSIGNED_IPA" ]] || {
  echo "Unsigned device IPA not found: $UNSIGNED_IPA" >&2
  echo "Run without --skip-build first." >&2
  exit 1
}

if [[ "$INTERPRETER_ONLY" == true ]]; then
  echo "== Packaging TrollStore interpreter-only A/B TIPA =="
else
  echo "== Packaging TrollStore JIT-enabled TIPA =="
fi
PHONEME_BASE_ENTITLEMENTS="$REPO_ROOT/phoneME/Support/phoneME.entitlements" \
PHONEME_INTERPRETER_ONLY_PACKAGE="$INTERPRETER_ONLY" \
  bash "$SCRIPT_DIR/package-trollstore-ipa.sh" "$UNSIGNED_IPA" "$OUTPUT_TIPA"

cat <<RESULT

TrollStore build completed.
TIPA: $OUTPUT_TIPA
Unsigned base IPA: $UNSIGNED_IPA
Build log: $BUILD_LOG

Rebuild later with:
  bash Scripts/build-trollstore-tipa.sh
RESULT
