#!/bin/bash
# wozmosis -P: copy files between two Pascal images, preserving metadata,
# with the same-name collision guard. SKIPs if no Pascal fixture is present.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozmosis" "wozmosis not built"
skip_if_missing "./wozit"    "wozit not built"

PASC="UCSD Pascal 1.2_1.DSK"
skip_if_missing "$PASC" "Pascal disk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ext="${PASC##*.}"
SRC="$TMP/src.$ext"
DST="$TMP/dst.$ext"
cp "$PASC" "$SRC"
cp "$PASC" "$DST"

# Clear two files off the destination so there's room and no collision.
./wozit -P -I "$DST" -c "rm SYSTEM.EDITOR" -c "rm SYSTEM.FILER" -c save >/dev/null 2>&1
gone=$(./wozit -P -I "$DST" -c ls 2>&1)
assert_no_match "SYSTEM.EDITOR" "$gone" "destination starts without SYSTEM.EDITOR"

# Copy just those two files back from the source.
out=$(./wozmosis -P -v "$SRC" "$DST" SYSTEM.EDITOR SYSTEM.FILER 2>&1)
assert_match "Copied 2 files" "$out" "wozmosis copies the two selected files"

dls=$(./wozit -P -I "$DST" -c ls 2>&1)
assert_match "SYSTEM.EDITOR" "$dls" "copied SYSTEM.EDITOR appears on destination"
assert_match "SYSTEM.FILER"  "$dls" "copied SYSTEM.FILER appears on destination"

# Byte-for-byte fidelity against the source.
./wozit -P -I "$SRC" -c "cpout SYSTEM.EDITOR $TMP/src_ed.bin" >/dev/null 2>&1
./wozit -P -I "$DST" -c "cpout SYSTEM.EDITOR $TMP/dst_ed.bin" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/src_ed.bin")" "$(hash_file "$TMP/dst_ed.bin")" \
  "copied file matches the source byte-for-byte"

# Metadata (file kind) is preserved: SYSTEM.EDITOR is a .CODE file, and the
# destination listing must still show it as .CODE.
src_kind=$(awk '/SYSTEM.EDITOR/ {print $1}' <<<"$(./wozit -P -I "$SRC" -c ls 2>&1)")
dst_kind=$(awk '/SYSTEM.EDITOR/ {print $1}' <<<"$dls")
assert_eq "$src_kind" "$dst_kind" "copied file keeps its Pascal file kind"

# Collision guard: copying a file that already exists must fail without -f.
rc=0
./wozmosis -P "$SRC" "$DST" SYSTEM.APPLE >/dev/null 2>&1 || rc=$?
[ "$rc" -ne 0 ]; assert_eq 0 $? "same-name copy without -f exits nonzero"

t_done
