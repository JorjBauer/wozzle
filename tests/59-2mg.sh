#!/bin/bash
# .2mg (2IMG) images: a 64-byte header, then the disk in DOS order, ProDOS
# order or nibbles, then an optional comment and creator data. These were
# once read and written as bare blocks, so every one failed to open and
# every one we wrote had no header.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"
skip_if_missing "./wozzle" "wozzle not built"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Bytes at an offset, as hex
hexat() { od -An -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n'; }

# --- one made by someone else ------------------------------------------
# A 140K ProDOS volume in ProDOS order, from creator "TEST", locked, with
# volume number 200, the comment "hello" and the creator data "abc"
./wozit -p -c "format $TMP/src.po FOREIGN" >/dev/null 2>&1
printf 'X' > "$TMP/x.bin"
./wozit -p -I "$TMP/src.po" -c "cpin $TMP/x.bin ONE" -c save >/dev/null 2>&1
F="$TMP/foreign.2mg"
{
  printf '2IMGTEST\x40\x00\x01\x00'
  printf '\x01\x00\x00\x00'          # ProDOS order
  printf '\xc8\x01\x00\x80'          # volume 200, locked
  printf '\x18\x01\x00\x00'          # 280 blocks
  printf '\x40\x00\x00\x00\x00\x30\x02\x00'  # data at 64, 143360 bytes
  printf '\x40\x30\x02\x00\x05\x00\x00\x00'  # comment
  printf '\x45\x30\x02\x00\x03\x00\x00\x00'  # creator data
  printf '\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0'
  cat "$TMP/src.po"
  printf 'helloabc'
} > "$F"

out=$(./wozit -p -I "$F" -c ls 2>&1)
assert_match "FOREIGN" "$out" "a ProDOS-order .2mg opens"
assert_match "ONE" "$out" "and lists its files"
out=$(./wozit -p -I "$F" -c info 2>&1)
assert_match "Write protected: yes" "$out" "the locked flag reads as write protected"

./wozzle -i "$F" -o "$TMP/out.po" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/src.po")" "$(hash_file "$TMP/out.po")" \
  "converting to .po gives the blocks after the header"
./wozzle -s -i "$F" -o "$TMP/lazy.po" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/src.po")" "$(hash_file "$TMP/lazy.po")" \
  "and so does loading tracks as they're needed (-s)"

# The header's volume number goes in the sector address fields: D5 AA 96,
# then 200 in 4-and-4 (EE EA)
./wozzle -i "$F" -o "$TMP/out.nib" >/dev/null 2>&1
first=$(od -An -tx1 -v "$TMP/out.nib" | tr -d ' \n' | grep -o 'd5aa96....' | head -1)
assert_eq "d5aa96eeea" "$first" \
  "the header's volume number is in the address fields"

# Saving after an edit keeps everything that isn't the disk
./wozit -p -I "$F" -c "cpin $TMP/x.bin TWO" -c "save $TMP/edited.2mg" >/dev/null 2>&1
out=$(./wozit -p -I "$TMP/edited.2mg" -c ls 2>&1)
assert_match "TWO" "$out" "an edited .2mg saves and reopens"
assert_eq "$(hexat "$F" 0 64)" "$(hexat "$TMP/edited.2mg" 0 64)" \
  "the header is the same after an edit"
assert_eq "helloabc" "$(tail -c 8 "$TMP/edited.2mg")" \
  "the comment and creator data are kept"
assert_file_size "$TMP/edited.2mg" $((64 + 143360 + 8)) "and nothing else is added"

# --- ones we make --------------------------------------------------------
./wozit -p -c "format $TMP/b800.2mg BIG 800k" >/dev/null 2>&1
assert_file_size "$TMP/b800.2mg" $((64 + 819200)) "an 800k blank .2mg is a header and 1600 blocks"
assert_eq "32494d47574f5a5a" "$(hexat "$TMP/b800.2mg" 0 8)" "it's 2IMG, from WOZZ"
assert_eq "01000000" "$(hexat "$TMP/b800.2mg" 12 4)" "in ProDOS order"
out=$(./wozit -p -I "$TMP/b800.2mg" -c "cpin $TMP/x.bin ONE" -c save -c ls 2>&1)
assert_match "ONE" "$out" "and takes a file"

out=$(./wozit -p -c "format $TMP/b100.2mg SMALL 100" 2>&1)
assert_match "100 blocks" "$out" "a .2mg can be smaller than a floppy"

./wozit -d -c "format $TMP/dos.2mg 7" >/dev/null 2>&1
assert_eq "00000000" "$(hexat "$TMP/dos.2mg" 12 4)" "a DOS 3.3 blank .2mg is in DOS order"
out=$(./wozit -d -I "$TMP/dos.2mg" -c "cpin $TMP/x.bin ONE" -c save -c ls 2>&1)
assert_match "ONE" "$out" "and takes a file"

# A .dsk goes in DOS order and comes back out the same
./wozzle -i "$TMP/dos.2mg" -o "$TMP/dos.dsk" >/dev/null 2>&1
./wozzle -i "$TMP/dos.dsk" -o "$TMP/again.2mg" >/dev/null 2>&1
./wozzle -i "$TMP/again.2mg" -o "$TMP/again.dsk" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/dos.dsk")" "$(hash_file "$TMP/again.dsk")" \
  ".dsk to .2mg and back is the same"
assert_eq "00000000" "$(hexat "$TMP/again.2mg" 12 4)" "and the .2mg was DOS order"

# A .woz has no order of its own, so it goes in ProDOS order
./wozzle -i "$TMP/dos.dsk" -o "$TMP/dos.woz" >/dev/null 2>&1
./wozzle -i "$TMP/dos.woz" -o "$TMP/fromwoz.2mg" >/dev/null 2>&1
assert_eq "01000000" "$(hexat "$TMP/fromwoz.2mg" 12 4)" "a .woz becomes ProDOS order"
out=$(./wozit -d -I "$TMP/fromwoz.2mg" -c ls 2>&1)
assert_match "ONE" "$out" "and its DOS 3.3 files are still there"

# --- ones that aren't right ---------------------------------------------
cp "$TMP/dos.dsk" "$TMP/bare.2mg"
out=$(./wozit -d -I "$TMP/bare.2mg" -c ls 2>&1)
assert_match "no 2IMG header" "$out" "a .2mg with no header is refused, with the reason"
head -c 100 "$TMP/dos.2mg" > "$TMP/short.2mg"
out=$(./wozit -d -I "$TMP/short.2mg" -c ls 2>&1)
assert_match "bad 2IMG header" "$out" "a cut-off .2mg is refused"

t_done
