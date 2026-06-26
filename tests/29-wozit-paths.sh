#!/bin/bash
# ProDOS subdirectory paths: slash-separated paths across the file commands
# (ls, cpin, cpout, rm, rmdir, mkdir), including nested directories.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"   "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing (used as format host)"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

yes X | head -c 300 > "$TMP/f.bin"
fhash=$(hash_file "$TMP/f.bin")

prodos_free() { ./wozit -I "$1" -p -c info 2>/dev/null | grep 'Blocks free' | grep -oE '[0-9]+'; }

./wozit -I dos33.dsk -p -c "format $TMP/v.po PATHTEST" >/dev/null 2>&1
skip_if_missing "$TMP/v.po" "format did not produce an image"
blank=$(prodos_free "$TMP/v.po")

cp "$TMP/v.po" "$TMP/w.po"

# Build a two-level tree, writing files into subdirectories by path.
build=$(./wozit -I "$TMP/w.po" -p \
  -c "mkdir PROJ" \
  -c "cpin $TMP/f.bin PROJ/HELLO" \
  -c "mkdir PROJ/SUB" \
  -c "cpin $TMP/f.bin PROJ/SUB/DEEP FF 0300" \
  -c "save $TMP/w.po" 2>&1)
assert_match "Created directory 'PROJ'"     "$build" "mkdir creates a top-level directory"
assert_match "Created directory 'PROJ/SUB'" "$build" "mkdir creates a nested directory by path"
assert_no_match "ERROR" "$build" "building the tree produced no errors"

# ls of a subdirectory lists just that directory's contents.
ls_proj=$(./wozit -I "$TMP/w.po" -p -c "ls PROJ" 2>&1)
assert_regex "HELLO +BIN" "$ls_proj" "ls <dir> lists a file written by path"
assert_regex "SUB +DIR"   "$ls_proj" "ls <dir> lists a nested subdirectory"
ls_sub=$(./wozit -I "$TMP/w.po" -p -c "ls PROJ/SUB" 2>&1)
assert_regex "DEEP +SYS" "$ls_sub" "ls of a nested path lists its file with the given type"
assert_match "\$0300"    "$ls_sub" "type/aux are honored when writing by path"

# Read a nested file back out by path; bytes must match the source.
./wozit -I "$TMP/w.po" -p -c "cpout PROJ/SUB/DEEP $TMP/out.bin" >/dev/null 2>&1
assert_eq "$fhash" "$(hash_file "$TMP/out.bin")" "cpout of a nested path round-trips"

# Path error cases.
e1=$(./wozit -I "$TMP/w.po" -p -c "rm NOPE/X" 2>&1)
assert_match "not found" "$e1" "rm through a missing directory reports not found"
e2=$(./wozit -I "$TMP/w.po" -p -c "rm PROJ/HELLO/X" 2>&1)
assert_match "is not a directory" "$e2" "a file used as a path component is rejected"

# rmdir refuses a non-empty subdirectory addressed by path.
ne=$(./wozit -I "$TMP/w.po" -p -c "rmdir PROJ/SUB" 2>&1)
assert_match "not empty" "$ne" "rmdir by path refuses a non-empty directory"

# Tear the whole tree down by path; every block should come back.
td=$(./wozit -I "$TMP/w.po" -p \
  -c "rm PROJ/SUB/DEEP" \
  -c "rmdir PROJ/SUB" \
  -c "rm PROJ/HELLO" \
  -c "rmdir PROJ" \
  -c "save $TMP/w.po" 2>&1)
assert_match "Removed 'PROJ/SUB/DEEP'"        "$td" "rm removes a nested file by path"
assert_match "Removed directory 'PROJ/SUB'"   "$td" "rmdir removes a nested directory by path"
assert_eq "$blank" "$(prodos_free "$TMP/w.po")" "tearing down the tree reclaims every block"

after=$(./wozit -I "$TMP/w.po" -p -c "ls" 2>&1)
assert_no_match "PROJ"    "$after" "the tree is gone after teardown"
assert_no_match "WARNING" "$after" "the volume re-reads cleanly after path operations"

t_done
