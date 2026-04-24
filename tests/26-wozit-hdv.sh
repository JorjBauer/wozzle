#!/bin/bash
# ProDOS hard-disk image (HDV) support. Both wozit reading and wozzle
# passthrough should work on the 32 MB hd1.img fixture.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"  "wozit not built"
skip_if_missing "./wozzle" "wozzle not built"
skip_if_missing "hd1.img"  "hd1.img fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- wozit: listing ---
ls_out=$(./wozit -I hd1.img -p -c "ls" 2>&1)
assert_match    "HD32"            "$ls_out" "HDV volume name 'HD32' visible"
assert_match    "PRODOS"          "$ls_out" "PRODOS file present in catalog"
assert_match    "MERLIN.SYSTEM"   "$ls_out" "MERLIN.SYSTEM present"
assert_no_match "Failed to read"  "$ls_out" "no decode errors on HDV load"
assert_no_match "don't know how"  "$ls_out" "no unreachable-path errors"

# --- wozit: extract a known small file and check size ---
./wozit -I hd1.img -p -c "cpout PARMS $TMP/parms.bin" >/dev/null 2>&1
assert_file_size "$TMP/parms.bin" 44 "PARMS extracts at its EOF length (44 bytes)"

# --- wozit: info/probe picks correct mode ---
out=$(./wozit -I hd1.img -p -c "ls" 2>&1)
assert_no_match "WARNING" "$out" "HDV loads cleanly with -p (no mode-mismatch warning)"

# --- wozzle: HDV → HDV round-trip is byte-identical ---
./wozzle -i hd1.img -o "$TMP/hd-rt.img" >/dev/null 2>&1
if cmp -s hd1.img "$TMP/hd-rt.img"; then
  printf "  PASS: HDV → HDV round-trip byte-identical\n"
  t_pass=$((t_pass + 1))
else
  printf "  FAIL: HDV → HDV round-trip diverged\n" >&2
  t_fail=$((t_fail + 1))
fi

# --- wozzle: refuses HDV → floppy-format conversion ---
rm -f "$TMP/hd.dsk"
out=$(./wozzle -i hd1.img -o "$TMP/hd.dsk" 2>&1)
assert_match "can't convert a hard-disk" "$out" "HDV → DSK is explicitly refused"
[ ! -e "$TMP/hd.dsk" ]
assert_eq 0 $? "refused conversion left no output file"

t_done
