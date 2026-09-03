#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
INPUT_IPA="${1:-$REPO_ROOT/Artifacts/phoneME-latest.ipa}"
OUTPUT_TIPA="${2:-$REPO_ROOT/Artifacts/phoneME-TrollStore-JIT.tipa}"
INTERPRETER_ONLY_PACKAGE="${PHONEME_INTERPRETER_ONLY_PACKAGE:-false}"

usage() {
  cat <<'USAGE'
Create a TrollStore JIT-enabled .tipa from an existing phoneME device IPA.

Usage:
  bash Scripts/package-trollstore-ipa.sh [input.ipa] [output.tipa]

The output replaces distribution entitlements with a clean TrollStore-compatible
set. The main binary is fake-signed with ldid and the final archive, safe
CS_DEBUGGED JIT-status bridge and forbidden entitlements are verified.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

for command in unzip zip ldid plutil find mktemp grep shasum lipo du awk cp nm strings; do
  command -v "$command" >/dev/null 2>&1 || {
    echo "Required command not found: $command" >&2
    exit 1
  }
done

[[ -f "$INPUT_IPA" ]] || {
  echo "Input IPA not found: $INPUT_IPA" >&2
  exit 1
}

WORK_ROOT="$(mktemp -d /tmp/phoneme-trollstore.XXXXXX)"
cleanup() {
  rm -rf "$WORK_ROOT"
}
trap cleanup EXIT

unzip -q "$INPUT_IPA" -d "$WORK_ROOT/package"
APP_PATH="$(find "$WORK_ROOT/package/Payload" -maxdepth 1 -type d -name '*.app' -print -quit)"
[[ -n "$APP_PATH" && -d "$APP_PATH" ]] || {
  echo "The IPA does not contain an application bundle." >&2
  exit 1
}

INFO_PLIST="$APP_PATH/Info.plist"
EXECUTABLE_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$INFO_PLIST")"
BINARY_PATH="$APP_PATH/$EXECUTABLE_NAME"
[[ -f "$BINARY_PATH" ]] || {
  echo "Main executable not found: $BINARY_PATH" >&2
  exit 1
}

# Keep the two TrollStore artifacts semantically distinct. The JIT artifact
# advertises that JIT is required; the interpreter artifact must never trigger
# the TrollStore JIT flow just because it shares the same platform entitlements.
/usr/libexec/PlistBuddy -c 'Delete :PhoneMETrollStoreJIT' "$INFO_PLIST" >/dev/null 2>&1 || true
/usr/libexec/PlistBuddy -c 'Delete :PhoneMEInterpreterOnly' "$INFO_PLIST" >/dev/null 2>&1 || true
if [[ "$INTERPRETER_ONLY_PACKAGE" == true ]]; then
  /usr/libexec/PlistBuddy -c 'Add :PhoneMETrollStoreJIT bool false' "$INFO_PLIST"
  /usr/libexec/PlistBuddy -c 'Add :PhoneMEInterpreterOnly bool true' "$INFO_PLIST"
else
  /usr/libexec/PlistBuddy -c 'Add :PhoneMETrollStoreJIT bool true' "$INFO_PLIST"
  /usr/libexec/PlistBuddy -c 'Add :PhoneMEInterpreterOnly bool false' "$INFO_PLIST"
fi

ENTITLEMENTS="$WORK_ROOT/phoneME-TrollStore-JIT.entitlements"
# UTM-HV builds a clean TrollStore entitlement set instead of carrying the
# distribution signature forward. Do the same here: developer-team and iCloud
# placeholders such as $(TeamIdentifierPrefix) are invalid in a fake-signed
# package and can make AMFI/FrontBoard terminate the app at launch.
cat > "$ENTITLEMENTS" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "https://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict/>
</plist>
PLIST

set_boolean_entitlement() {
  local key="$1"
  /usr/libexec/PlistBuddy -c "Delete :$key" "$ENTITLEMENTS" >/dev/null 2>&1 || true
  /usr/libexec/PlistBuddy -c "Add :$key bool true" "$ENTITLEMENTS"
}

delete_entitlement() {
  /usr/libexec/PlistBuddy -c "Delete :$1" "$ENTITLEMENTS" >/dev/null 2>&1 || true
}

set_string_array_entitlement() {
  local key="$1"
  shift
  /usr/libexec/PlistBuddy -c "Delete :$key" "$ENTITLEMENTS" >/dev/null 2>&1 || true
  /usr/libexec/PlistBuddy -c "Add :$key array" "$ENTITLEMENTS"
  local index=0
  local value
  for value in "$@"; do
    /usr/libexec/PlistBuddy -c "Add :$key:$index string $value" "$ENTITLEMENTS"
    index=$((index + 1))
  done
}

# The package remains attachable by TrollStore/StikDebug. JIT readiness is
# verified at runtime from the target process's CS_DEBUGGED flag only. On A12+
# devices CS_KILL being absent is not sufficient proof that unsigned generated
# code can execute.
set_boolean_entitlement "get-task-allow"
set_boolean_entitlement "com.apple.developer.kernel.increased-memory-limit"
set_boolean_entitlement "com.apple.developer.kernel.extended-virtual-addressing"
set_boolean_entitlement "com.apple.private.security.no-sandbox"
set_boolean_entitlement "platform-application"
set_boolean_entitlement "com.apple.private.security.storage.AppDataContainers"
set_boolean_entitlement "com.apple.private.security.storage.MobileDocuments"

# Match UTM-HV's tested platform-app IOKit allow-list. It covers Metal,
# IOSurface, framebuffer, audio, HID and hardware media acceleration without
# adding the A12+-banned code-signing entitlements.
set_string_array_entitlement \
  "com.apple.security.exception.iokit-user-client-class" \
  "AGXDevice" \
  "IOSurfaceRootUserClient" \
  "RootDomainUserClient" \
  "AppleJPEGDriverUserClient" \
  "IOHIDParamUserClient" \
  "H11ANEInDirectPathClient" \
  "IOAudio2DeviceUserClient" \
  "H11ANEInUserClient" \
  "IOMobileFramebufferUserClient" \
  "AppleNVMeEANClient" \
  "ASPToolPathDriverUserClient"

# TrollStore rejects these on newer A12+ devices. JIT is granted externally by
# TrollStore's supported attach flow instead.
delete_entitlement "dynamic-codesigning"
delete_entitlement "com.apple.private.cs.debugger"
delete_entitlement "com.apple.private.skip-library-validation"

plutil -lint "$ENTITLEMENTS" >/dev/null

# Remove the distribution signature/provisioning profile and fake-sign the
# executable with the final entitlement set.
rm -rf "$APP_PATH/_CodeSignature"
rm -f "$APP_PATH/embedded.mobileprovision"
ldid -S"$ENTITLEMENTS" "$BINARY_PATH"

mkdir -p "$(dirname "$OUTPUT_TIPA")"
rm -f "$OUTPUT_TIPA"
(
  cd "$WORK_ROOT/package"
  zip -qry "$OUTPUT_TIPA" Payload
)

unzip -tq "$OUTPUT_TIPA" >/dev/null
VERIFY_ROOT="$WORK_ROOT/verify"
unzip -q "$OUTPUT_TIPA" -d "$VERIFY_ROOT"
VERIFY_APP="$(find "$VERIFY_ROOT/Payload" -maxdepth 1 -type d -name '*.app' -print -quit)"
VERIFY_EXECUTABLE="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$VERIFY_APP/Info.plist")"
VERIFY_BINARY="$VERIFY_APP/$VERIFY_EXECUTABLE"
VERIFY_ENTITLEMENTS="$WORK_ROOT/verify.entitlements"
ldid -e "$VERIFY_BINARY" > "$VERIFY_ENTITLEMENTS"
plutil -lint "$VERIFY_ENTITLEMENTS" >/dev/null

verify_boolean_entitlement() {
  local key="$1"
  /usr/libexec/PlistBuddy -c "Print :$key" "$VERIFY_ENTITLEMENTS" | grep -qx 'true'
}

verify_boolean_entitlement "get-task-allow"
verify_boolean_entitlement "com.apple.developer.kernel.increased-memory-limit"
verify_boolean_entitlement "com.apple.developer.kernel.extended-virtual-addressing"
verify_boolean_entitlement "com.apple.private.security.no-sandbox"
verify_boolean_entitlement "platform-application"
verify_boolean_entitlement "com.apple.private.security.storage.AppDataContainers"
verify_boolean_entitlement "com.apple.private.security.storage.MobileDocuments"
if [[ "$INTERPRETER_ONLY_PACKAGE" == true ]]; then
  /usr/libexec/PlistBuddy -c 'Print :PhoneMETrollStoreJIT' \
    "$VERIFY_APP/Info.plist" | grep -qx 'false'
  /usr/libexec/PlistBuddy -c 'Print :PhoneMEInterpreterOnly' \
    "$VERIFY_APP/Info.plist" | grep -qx 'true'
else
  /usr/libexec/PlistBuddy -c 'Print :PhoneMETrollStoreJIT' \
    "$VERIFY_APP/Info.plist" | grep -qx 'true'
  /usr/libexec/PlistBuddy -c 'Print :PhoneMEInterpreterOnly' \
    "$VERIFY_APP/Info.plist" | grep -qx 'false'
fi
/usr/libexec/PlistBuddy \
  -c 'Print :com.apple.security.exception.iokit-user-client-class' \
  "$VERIFY_ENTITLEMENTS" | grep -q 'IOSurfaceRootUserClient'

ARCH_INFO="$(lipo -info "$VERIFY_BINARY")"
[[ "$ARCH_INFO" == *arm64* ]] || {
  echo "The TrollStore binary is not arm64: $ARCH_INFO" >&2
  exit 1
}

nm -gU "$VERIFY_BINARY" | \
  grep '_phoneme_platform_jit_status$' >/dev/null || {
    echo "The TrollStore binary is missing the safe JIT status bridge." >&2
    exit 1
  }
if [[ "$INTERPRETER_ONLY_PACKAGE" == true ]]; then
  if nm -gU "$VERIFY_BINARY" | \
       grep -q '_phoneme_trollstore_jit_bootstrap_constructor$'; then
    echo "The interpreter-only binary unexpectedly contains the JIT bootstrap constructor." >&2
    exit 1
  fi
  if strings -a "$VERIFY_BINARY" | grep -q -- '--phoneme-trollstore-jit-child'; then
    echo "The interpreter-only binary unexpectedly contains the TrollStore JIT child path." >&2
    exit 1
  fi
else
  nm -gU "$VERIFY_BINARY" | \
    grep '_phoneme_trollstore_jit_bootstrap_constructor$' >/dev/null || {
      echo "The TrollStore JIT binary is missing the launch-time JIT constructor." >&2
      exit 1
    }
  strings -a "$VERIFY_BINARY" | \
    grep 'PHONEME_TROLLSTORE_JIT_PACKAGE' >/dev/null || {
      echo "The TrollStore JIT binary is missing the package marker." >&2
      exit 1
    }
  strings -a "$VERIFY_BINARY" | \
    grep -- '--phoneme-trollstore-jit-child' >/dev/null || {
      echo "The JIT binary is missing the launch-time TrollStore auto-JIT child path." >&2
      exit 1
    }
fi

TIPA_SIZE="$(du -h "$OUTPUT_TIPA" | awk '{print $1}')"
SHA256="$(shasum -a 256 "$OUTPUT_TIPA" | awk '{print $1}')"

if [[ "$INTERPRETER_ONLY_PACKAGE" == true ]]; then
  PACKAGE_KIND="interpreter-only"
  JIT_ACTIVATION="disabled by build policy"
else
  PACKAGE_KIND="JIT-required"
  JIT_ACTIVATION="launch-time PT_TRACE_ME auto-bootstrap"
fi

cat <<RESULT
TrollStore $PACKAGE_KIND package created successfully.
Input: $INPUT_IPA
Output: $OUTPUT_TIPA
Architecture: $ARCH_INFO
Entitlements: TrollStore no-sandbox platform package
JIT activation: $JIT_ACTIVATION
Size: $TIPA_SIZE
SHA-256: $SHA256
RESULT
