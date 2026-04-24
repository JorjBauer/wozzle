#!/bin/bash
# Basic sanity: `ls` on a known DOS 3.3 image finds the files we expect,
# with reasonable type letters and sector counts.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing"

out=$(./wozit -I dos33.dsk -d -c "ls" 2>&1)

assert_match "HELLO"    "$out" "dos33.dsk catalog contains HELLO"
assert_match "FID"      "$out" "dos33.dsk catalog contains FID"
assert_match "RENUMBER" "$out" "dos33.dsk catalog contains RENUMBER"

# Type column: HELLO is Applesoft (A), FID is binary (B).
assert_regex " A [[:space:]]*[0-9]+ HELLO"  "$out" "HELLO listed as type A"
assert_regex " B [[:space:]]*[0-9]+ FID"    "$out" "FID listed as type B"

t_done
