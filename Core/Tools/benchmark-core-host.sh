#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$CORE_ROOT/.." && pwd)"
RUN_ID="${PHONEME_BENCHMARK_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
OUTPUT_ROOT="${PHONEME_BENCHMARK_OUTPUT_ROOT:-$CORE_ROOT/build/performance/$RUN_ID}"
SYNTHETIC_JSON="$OUTPUT_ROOT/synthetic-vm-performance.json"
COMPACT_JSON="$OUTPUT_ROOT/compact-storage.json"
JIT_JSON="$OUTPUT_ROOT/jit-performance.json"
CORE_PROFILE_JSON="$OUTPUT_ROOT/full-core-profile.json"
RUN_JSON="$OUTPUT_ROOT/benchmark-run.json"
CORPUS_MANIFEST="$SCRIPT_DIR/performance-benchmarks.json"

mkdir -p "$OUTPUT_ROOT"

python3 - "$REPO_ROOT" "$CORPUS_MANIFEST" <<'PY'
import hashlib
import json
import pathlib
import sys

repo_root = pathlib.Path(sys.argv[1])
manifest_path = pathlib.Path(sys.argv[2])
with manifest_path.open("r", encoding="utf-8") as stream:
    manifest = json.load(stream)
for benchmark in manifest.get("benchmarks", []):
    relative = pathlib.Path(benchmark["jar"])
    path = repo_root / relative
    if not path.is_file():
        raise SystemExit(f"missing benchmark JAR: {relative}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    expected = benchmark["sha256"].lower()
    if digest != expected:
        raise SystemExit(
            f"benchmark JAR hash mismatch for {relative}: {digest} != {expected}"
        )
print(f"Verified {len(manifest.get('benchmarks', []))} benchmark JARs")
PY

PHONEME_COMPACT_STORAGE_JSON="$COMPACT_JSON" \
  bash "$SCRIPT_DIR/test-performance-host.sh" "$SYNTHETIC_JSON"
bash "$SCRIPT_DIR/benchmark-jit-host.sh" "$JIT_JSON"

PHONEME_ENABLE_VM_PROFILING=1 \
PHONEME_ENABLE_DECODED_EXECUTION=1 \
PHONEME_VM_PROFILE_JSON="$CORE_PROFILE_JSON" \
PHONEME_TEST_TIMEOUT="${PHONEME_TEST_TIMEOUT:-300}" \
  bash "$SCRIPT_DIR/test-host.sh"

REVISION="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || printf unknown)"
HOST_NAME="$(uname -n)"
HOST_SYSTEM="$(uname -srvmp)"

cat >"$RUN_JSON" <<JSON
{
  "schema_version": 1,
  "run_id": "$RUN_ID",
  "git_revision": "$REVISION",
  "host_name": "$HOST_NAME",
  "host_system": "$HOST_SYSTEM",
  "profiling_enabled": true,
  "decoded_execution_enabled": true,
  "corpus_manifest": "$CORPUS_MANIFEST",
  "corpus_verified": true,
  "synthetic_result": "$SYNTHETIC_JSON",
  "compact_storage_result": "$COMPACT_JSON",
  "jit_result": "$JIT_JSON",
  "full_core_profile": "$CORE_PROFILE_JSON"
}
JSON

echo "Core performance benchmark passed"
echo "Run manifest: $RUN_JSON"
