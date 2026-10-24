#!/bin/bash
# The WebAssembly build, through wasm/wozzle.mjs. Skips unless it's been
# built (make wasm, which needs Emscripten) and node is installed.
. "$(dirname "$0")/lib.sh"
skip_if_missing "wasm/build/wozcore.mjs" "wasm build missing (run 'make wasm')"
command -v node >/dev/null 2>&1 || { printf "  SKIP: node not installed\n"; exit 77; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

args=()
if [ -e ProDOS_2_4.po ] && [ -x ./wozit ]; then
  ./wozit -p -I ProDOS_2_4.po -c "cpout PRODOS $TMP/prodos.out" >/dev/null 2>&1
  args=(ProDOS_2_4.po "$TMP/prodos.out")
fi
node wasm/test.mjs "${args[@]}"
rc=$?
assert_eq 0 "$rc" "the wasm tests pass"
t_done
