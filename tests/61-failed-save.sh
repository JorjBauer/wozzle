#!/bin/bash
# A save that fails partway leaves nothing behind: an existing file keeps
# what it had, a new one isn't made, and the temporary file the image was
# being written to is gone. A failed save used to leave the half-written
# file in place of the old one.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"
skip_if_missing "./wozzle" "wozzle not built"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# A .woz with track 5 zeroed, so converting it to sectors fails there
# (each WOZ 2 track here is 13 blocks, starting at block 3)
./wozit -d -c "format $TMP/good.dsk" >/dev/null 2>&1
./wozzle -i "$TMP/good.dsk" -o "$TMP/bad.woz" >/dev/null 2>&1
dd if=/dev/zero of="$TMP/bad.woz" bs=512 seek=$((3 + 13 * 5)) count=13 conv=notrunc 2>/dev/null

cp "$TMP/good.dsk" "$TMP/target.dsk"
before=$(hash_file "$TMP/target.dsk")
out=$(./wozzle -i "$TMP/bad.woz" -o "$TMP/target.dsk" 2>&1)
assert_match "Failed to decode track 5" "$out" "the conversion fails partway"
assert_eq "$before" "$(hash_file "$TMP/target.dsk")" "the file it was replacing is unchanged"

./wozzle -i "$TMP/bad.woz" -o "$TMP/new.dsk" >/dev/null 2>&1
if [ -e "$TMP/new.dsk" ]; then
  printf "  FAIL: a failed save to a new name left a file\n" >&2
  t_fail=$((t_fail + 1))
else
  printf "  PASS: a failed save to a new name leaves no file\n"
  t_pass=$((t_pass + 1))
fi

left=$(ls "$TMP" | grep -c wozsave)
assert_eq "0" "$left" "and no temporary file is left over"

# A save that works still replaces the file
./wozit -d -c "format $TMP/other.dsk 9" >/dev/null 2>&1
./wozzle -i "$TMP/other.dsk" -o "$TMP/other.woz" >/dev/null 2>&1
./wozzle -i "$TMP/other.woz" -o "$TMP/target.dsk" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/other.dsk")" "$(hash_file "$TMP/target.dsk")" \
  "a save that works replaces the file"

t_done
