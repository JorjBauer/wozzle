#!/bin/bash
# wozit resize: shrink a ProDOS volume in place - volume directory header,
# bitmap, and the image file itself - instead of formatting a smaller
# volume and copying into it. Refuses, changing nothing, when anything is
# allocated at or past the new end. Shrinking only, raw block images only.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"
skip_if_missing "ProDOS_2_4.po" "ProDOS 2.4 fixture missing"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

verify() {
  # Run the independent consistency checker if it's available.
  if [ -f tests/prodos-verify.py ] && command -v python3 >/dev/null 2>&1; then
    python3 tests/prodos-verify.py "$1" >/dev/null 2>&1
    return $?
  fi
  return 0
}

# ---- a volume with a little data in it ------------------------------------
./wozit -p -c "format $TMP/v.hdv SHRINKME 5m" >/dev/null
head -c 3000 /dev/urandom > "$TMP/payload.bin" 2>/dev/null || \
  dd if=/dev/urandom of="$TMP/payload.bin" bs=1 count=3000 2>/dev/null
./wozit -I "$TMP/v.hdv" -p -c "cpin $TMP/payload.bin PAYLOAD 06 2000" \
  -c "save" >/dev/null 2>&1
assert_eq 4980736 "$(wc -c < "$TMP/v.hdv" | tr -d ' ')" "5m image is 9728 blocks"

# ---- shrink to a size that fits -------------------------------------------
# A freshly formatted volume allocates from the front, so everything this
# volume holds sits well inside the first 1000 blocks.
out=$(./wozit -I "$TMP/v.hdv" -p -c "resize 1000" -c "save" 2>&1)
assert_match "Volume resized from 9728 to 1000 blocks" "$out" \
  "resize reports the change"
assert_eq 512000 "$(wc -c < "$TMP/v.hdv" | tr -d ' ')" \
  "image file shrank to 1000 blocks"
info=$(./wozit -I "$TMP/v.hdv" -p -c "info" 2>/dev/null)
assert_match "Blocks total: 1000" "$info" "header records the new size"
verify "$TMP/v.hdv"
assert_eq 0 $? "resized volume passes the consistency checker"

# The file that was there is still there, byte for byte.
./wozit -I "$TMP/v.hdv" -p -c "cpout PAYLOAD $TMP/out.bin" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/payload.bin")" "$(hash_file "$TMP/out.bin")" \
  "file contents survive the resize"

# The volume still works as a volume afterward.
./wozit -I "$TMP/v.hdv" -p -c "cpin $TMP/payload.bin SECOND 06 2000" \
  -c "save" >/dev/null 2>&1
./wozit -I "$TMP/v.hdv" -p -c "cpout SECOND $TMP/out2.bin" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/payload.bin")" "$(hash_file "$TMP/out2.bin")" \
  "a file written after the resize round-trips"
verify "$TMP/v.hdv"
assert_eq 0 $? "still consistent after writing into the resized volume"

# ---- refusing when the data simply doesn't fit ----------------------------
# 10 blocks can't hold what this volume holds, so there's nowhere to move
# the occupied tail to and the whole resize is refused.
before=$(hash_file "$TMP/v.hdv")
out=$(./wozit -I "$TMP/v.hdv" -p -c "resize 10" -c "save" 2>&1)
assert_match "more needed" "$out" "refuses when there's nowhere to move blocks"
assert_match "Nothing changed" "$out" "says nothing changed"
assert_eq "$before" "$(hash_file "$TMP/v.hdv")" \
  "refused resize leaves the image untouched"


# Deleting the file that was in the way makes the same resize legal.
./wozit -I "$TMP/v.hdv" -p -c "rm PAYLOAD" -c "rm SECOND" -c "save" \
  >/dev/null 2>&1
out=$(./wozit -I "$TMP/v.hdv" -p -c "resize 20" -c "save" 2>&1)
assert_match "Volume resized from 1000 to 20 blocks" "$out" \
  "freeing the blocks first allows the shrink"
verify "$TMP/v.hdv"
assert_eq 0 $? "minimal volume is still consistent"

# ---- guards ---------------------------------------------------------------
out=$(./wozit -I "$TMP/v.hdv" -p -c "resize 9728" 2>&1)
assert_match "resize only shrinks a volume" "$out" "growing is refused"
out=$(./wozit -I "$TMP/v.hdv" -p -c "resize 20" 2>&1)
assert_match "already 20 blocks" "$out" "resizing to the same size is a no-op"
out=$(./wozit -I "$TMP/v.hdv" -p -c "resize 3" 2>&1)
assert_match "too small" "$out" "refuses to cut into the bitmap"

cp ProDOS_2_4.po "$TMP/floppy.po"
out=$(./wozit -I "$TMP/floppy.po" -p -c "resize 200" 2>&1)
assert_match "floppy image is a fixed 280 blocks" "$out" \
  "floppy images are refused"

out=$(./wozit -I dos33.dsk -d -c "resize 200" 2>&1)
assert_match "resize is ProDOS-only" "$out" "DOS 3.3 mode is refused"

out=$(./wozit -I "$TMP/v.hdv" -p -c "resize" 2>&1)
assert_match "Usage: resize" "$out" "missing argument prints usage"
out=$(./wozit -I "$TMP/v.hdv" -p -c "resize wat" 2>&1)
assert_match "bad size 'wat'" "$out" "unparseable size is refused"

# ---- relocation: blocks in the way get moved down -------------------------
# Lay two files down back to back, then delete the first. What's left sits
# above a hole big enough to hold it, so shrinking past it has to move it.
./wozit -p -c "format $TMP/r.hdv MOVER 800k" >/dev/null
head -c 90000 /dev/urandom > "$TMP/first.bin" 2>/dev/null || \
  dd if=/dev/urandom of="$TMP/first.bin" bs=1 count=90000 2>/dev/null
head -c 20000 /dev/urandom > "$TMP/second.bin" 2>/dev/null || \
  dd if=/dev/urandom of="$TMP/second.bin" bs=1 count=20000 2>/dev/null
./wozit -I "$TMP/r.hdv" -p -c "cpin $TMP/first.bin FIRST 06 2000" -c "save" \
  >/dev/null 2>&1
./wozit -I "$TMP/r.hdv" -p -c "cpin $TMP/second.bin SECOND 06 2000" \
  -c "save" >/dev/null 2>&1
./wozit -I "$TMP/r.hdv" -p -c "rm FIRST" -c "save" >/dev/null 2>&1
cp "$TMP/r.hdv" "$TMP/r-before.hdv"

# -n won't move anything, so it refuses.
out=$(./wozit -I "$TMP/r.hdv" -p -c "resize -n 100" -c "save" 2>&1)
assert_match "nothing changed" "$out" "-n refuses rather than moving blocks"
assert_eq "$(hash_file "$TMP/r-before.hdv")" "$(hash_file "$TMP/r.hdv")" \
  "-n leaves the image untouched"

# Without it, the occupied tail is moved into the hole and the shrink runs.
out=$(./wozit -I "$TMP/r.hdv" -p -c "resize 100" -c "save" 2>&1)
assert_match "Moved " "$out" "blocks in the way are moved down"
assert_match "Volume resized from 1600 to 100 blocks" "$out" \
  "the shrink then goes through"
assert_eq 51200 "$(wc -c < "$TMP/r.hdv" | tr -d ' ')" "image is 100 blocks"
verify "$TMP/r.hdv"
assert_eq 0 $? "relocated volume passes the consistency checker"
./wozit -I "$TMP/r.hdv" -p -c "cpout SECOND $TMP/moved.bin" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/second.bin")" "$(hash_file "$TMP/moved.bin")" \
  "the moved file's contents are unchanged"

# Directories move too, back-pointers and all: fill the front, make the
# directories land high, then free the front and shrink underneath them.
./wozit -p -c "format $TMP/d.hdv DIRMOVE 800k" >/dev/null
./wozit -I "$TMP/d.hdv" -p -c "cpin $TMP/first.bin BIG 06 2000" -c "save" \
  >/dev/null 2>&1
./wozit -I "$TMP/d.hdv" -p -c "mkdir TOP" -c "mkdir TOP/INNER" -c "save" \
  >/dev/null 2>&1
./wozit -I "$TMP/d.hdv" -p -c "cpin $TMP/second.bin TOP/INNER/DEEP 06 2000" \
  -c "save" >/dev/null 2>&1
./wozit -I "$TMP/d.hdv" -p -c "rm BIG" -c "save" >/dev/null 2>&1
out=$(./wozit -I "$TMP/d.hdv" -p -c "resize 80" -c "save" 2>&1)
assert_match "Moved " "$out" "directory blocks are moved too"
verify "$TMP/d.hdv"
assert_eq 0 $? "volume with relocated directories is consistent"
./wozit -I "$TMP/d.hdv" -p -c "cpout TOP/INNER/DEEP $TMP/deep.bin" \
  >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/second.bin")" "$(hash_file "$TMP/deep.bin")" \
  "a file inside two moved directories still reads back"

# ---- resize needs a save to reach the file --------------------------------
./wozit -p -c "format $TMP/w.hdv NOSAVE 800k" >/dev/null
before=$(hash_file "$TMP/w.hdv")
./wozit -I "$TMP/w.hdv" -p -c "resize 500" >/dev/null 2>&1
assert_eq "$before" "$(hash_file "$TMP/w.hdv")" \
  "resize without save doesn't touch the image file"

t_done
