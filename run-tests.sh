#!/bin/bash
# Top-level test harness for wozzle / wozit.
#
# Discovers tests/[0-9]*.sh and runs each with the project root as CWD.
# Aggregates results and keeps going on failure (so one broken test
# doesn't mask others). Exit 0 iff no test failed.
#
# Per-test exit codes:
#   0  PASS
#   77 SKIP (fixture missing, etc.)
#   *  FAIL

cd "$(dirname "$0")" || exit 2

if [ $# -gt 0 ]; then
  # Optional filter: run only tests whose filename contains the argument.
  filter="$1"
else
  filter=""
fi

pass=0
fail=0
skip=0
failed_tests=()
skipped_tests=()

for t in tests/[0-9]*.sh; do
  [ -x "$t" ] || continue
  name=$(basename "$t")
  if [ -n "$filter" ] && ! printf "%s" "$name" | grep -q "$filter"; then
    continue
  fi

  printf "=== %s ===\n" "$name"
  rc=0
  bash "$t" || rc=$?
  case "$rc" in
    0)
      printf "=== %s: PASS ===\n\n" "$name"
      pass=$((pass + 1))
      ;;
    77)
      printf "=== %s: SKIP ===\n\n" "$name"
      skip=$((skip + 1))
      skipped_tests+=("$name")
      ;;
    *)
      printf "=== %s: FAIL (exit %d) ===\n\n" "$name" "$rc"
      fail=$((fail + 1))
      failed_tests+=("$name")
      ;;
  esac
done

printf "==========\n"
printf "Passed:  %d\n" "$pass"
printf "Failed:  %d\n" "$fail"
printf "Skipped: %d\n" "$skip"
if [ "${#skipped_tests[@]}" -gt 0 ]; then
  printf "\nSkipped tests:\n"
  for s in "${skipped_tests[@]}"; do printf "  %s\n" "$s"; done
fi
if [ "${#failed_tests[@]}" -gt 0 ]; then
  printf "\nFailed tests:\n"
  for f in "${failed_tests[@]}"; do printf "  %s\n" "$f"; done
  exit 1
fi
exit 0
