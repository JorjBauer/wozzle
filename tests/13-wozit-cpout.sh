#!/bin/bash
# cpout: discrepancy warning + -A flag behavior.
#
#   TAIPAN without -A:  144-byte file + stderr warning
#   TAIPAN with    -A:  29696-byte file, no warning
#   FID (normal BIN):   no warning (logical≈allocated)
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"    "wozit not built"
skip_if_missing "taipan.dsk" "taipan.dsk fixture missing"
skip_if_missing "dos33.dsk"  "dos33.dsk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Case 1: TAIPAN without -A.
./wozit -I taipan.dsk -d -c "cpout TAIPAN $TMP/taipan-short.bin" \
  >"$TMP/out1" 2>"$TMP/err1"
assert_file_size "$TMP/taipan-short.bin" 144 "TAIPAN w/o -A is 144 bytes"
assert_match "WARNING"   "$(cat "$TMP/err1")" "stderr carries a warning"
assert_match "29696"     "$(cat "$TMP/err1")" "warning names allocated size"
assert_match "-A"        "$(cat "$TMP/err1")" "warning mentions the -A flag"

# Case 2: TAIPAN with -A.
./wozit -I taipan.dsk -d -A -c "cpout TAIPAN $TMP/taipan-full.bin" \
  >"$TMP/out2" 2>"$TMP/err2"
assert_file_size "$TMP/taipan-full.bin" 29696 "TAIPAN w/ -A is 29696 bytes"
assert_no_match "WARNING" "$(cat "$TMP/err2")" "no warning on -A path"

# Case 3: normal BIN doesn't trip the warning.
./wozit -I dos33.dsk -d -c "cpout FID $TMP/fid.bin" \
  >"$TMP/out3" 2>"$TMP/err3"
assert_no_match "WARNING" "$(cat "$TMP/err3")" "no warning for normal BIN"

t_done
