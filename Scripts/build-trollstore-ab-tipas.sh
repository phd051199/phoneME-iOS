#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_ROOT="${OUTPUT_ROOT:-$REPO_ROOT/Artifacts}"

mkdir -p "$OUTPUT_ROOT"

echo "== Building JIT-on A/B artifact =="
bash "$SCRIPT_DIR/build-trollstore-tipa.sh" \
  --output-root "$OUTPUT_ROOT" \
  "$@"

echo "== Building interpreter-only A/B artifact =="
bash "$SCRIPT_DIR/build-trollstore-tipa.sh" \
  --interpreter-only \
  --output-root "$OUTPUT_ROOT" \
  "$@"

echo
echo "A/B TrollStore artifacts:"
shasum -a 256 \
  "$OUTPUT_ROOT/phoneME-TrollStore-JIT.tipa" \
  "$OUTPUT_ROOT/phoneME-TrollStore-Interpreter.tipa"
