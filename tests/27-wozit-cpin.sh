#!/bin/bash
# cpin: write-back round-trip integrity + type/aux arguments.
#
# Regression guard for the WOZ sector-write bug where writeBlock()'s
# re-encode of a directory block left the in-memory bitstream misaligned,
# so re-reading the just-written volume directory came back as garbage
# (and aborted on a populated disk via the directory-header assert). The
# core check: cpin a file, save, reload, and confirm the file — and the
# rest of the directory — survive a save/reload byte-for-byte.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"   "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing (used as format host)"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Deterministic, varied seedling-sized payload (< 512 bytes) from a
# committed source file, so corruption is easy to detect on round-trip.
head -c 400 crc32.c > "$TMP/blob.bin"
blob_hash=$(hash_file "$TMP/blob.bin")

# A blank ProDOS volume to write into.
./wozit -I dos33.dsk -p -c "format $TMP/v.po WOZZLETEST" >/dev/null 2>&1
skip_if_missing "$TMP/v.po" "format did not produce an image"

# --- Round-trip: default type (BIN) ------------------------------------
cp "$TMP/v.po" "$TMP/a.po"
out=$(./wozit -I "$TMP/a.po" -p -c "cpin $TMP/blob.bin HELLO" -c "save $TMP/a.po" 2>&1)
assert_no_match "Assertion failed" "$out" "cpin does not abort"
assert_no_match "WARNING"          "$out" "cpin produces no directory warning"

ls_out=$(./wozit -I "$TMP/a.po" -p -c "ls" 2>&1)
assert_no_match "Assertion failed" "$ls_out" "reload+ls does not abort"
assert_match    "HELLO"            "$ls_out" "reloaded directory lists the file"

./wozit -I "$TMP/a.po" -p -c "cpout HELLO $TMP/back.bin" >/dev/null 2>&1
assert_eq "$blob_hash" "$(hash_file "$TMP/back.bin")" "cpin/cpout round-trips byte-for-byte"

# --- Explicit hex type + aux -------------------------------------------
cp "$TMP/v.po" "$TMP/b.po"
./wozit -I "$TMP/b.po" -p -c "cpin $TMP/blob.bin PROG FF 0300" -c "save $TMP/b.po" >/dev/null 2>&1
ls_b=$(./wozit -I "$TMP/b.po" -p -c "ls" 2>&1)
assert_regex "PROG +SYS"   "$ls_b" "explicit type FF lists as SYS"
assert_match "\$0300"      "$ls_b" "explicit aux 0300 is honored"

# --- .SYSTEM name heuristic (no explicit type) -------------------------
cp "$TMP/v.po" "$TMP/c.po"
./wozit -I "$TMP/c.po" -p -c "cpin $TMP/blob.bin QUIT.SYSTEM" -c "save $TMP/c.po" >/dev/null 2>&1
ls_c=$(./wozit -I "$TMP/c.po" -p -c "ls" 2>&1)
assert_regex "QUIT.SYSTEM +SYS" "$ls_c" ".SYSTEM suffix defaults to SYS"
assert_match "\$2000"           "$ls_c" ".SYSTEM defaults aux to \$2000"

# --- Bonus: exact crash repro on a populated volume, if a fixture is
# present. The original abort needed a directory populated enough to walk
# into a subdirectory; a blank format can't exercise that on its own.
for fixture in ProDOS_2_4.po 1.po ProDOS_2_4.woz; do
  [ -e "$fixture" ] || continue
  ext="${fixture##*.}"
  cp "$fixture" "$TMP/pop.$ext"
  before=$(./wozit -I "$TMP/pop.$ext" -p -c "ls" 2>/dev/null)
  rep=$(./wozit -I "$TMP/pop.$ext" -p -c "cpin $TMP/blob.bin ADDED" -c "save $TMP/pop.$ext" 2>&1)
  assert_no_match "Assertion failed" "$rep" "populated cpin ($fixture) does not abort"
  after=$(./wozit -I "$TMP/pop.$ext" -p -c "ls" 2>/dev/null)
  assert_match "ADDED" "$after" "populated volume ($fixture) lists the new file"
  # Every name that was there before must still be there afterward.
  missing=""
  while read -r name; do
    [ -n "$name" ] || continue
    grep -qF -- "$name" <<<"$after" || missing="$missing $name"
  done < <(awk '{print $2}' <<<"$before" | grep -E '.')
  assert_eq "" "$missing" "pre-existing entries survive cpin ($fixture)"
  break
done

t_done
