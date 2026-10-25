#!/bin/bash
# 13-sector (DOS 3.2) disks, read-only: a .d13 opens, converts to and from
# .woz, .nib and .2mg without losing a byte, and anything that would
# change it, or squeeze it into a 16-sector format, is refused. A .nib or
# .2mg doesn't say it's 13-sector, so those also test that we can tell
# from the tracks.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"
skip_if_missing "./wozzle" "wozzle not built"
if ! command -v python3 >/dev/null 2>&1; then
  printf "  SKIP: python3 needed to build the test disk\n"
  exit 77
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# A small DOS 3.2 disk, 35 tracks of 13 sectors: the VTOC at 17/0, one
# catalog sector at 17/12, and a text file HELLO whose T/S list is at
# 18/0 and data at 18/1
D="$TMP/test.d13"
python3 - "$D" <<'EOF'
import sys
disk = bytearray(35 * 13 * 256)
def sector(t, s):
    o = (t * 13 + s) * 256
    return o
v = sector(17, 0)
disk[v + 1] = 17           # catalog track
disk[v + 2] = 12           # catalog sector
disk[v + 3] = 2            # DOS 3.2
disk[v + 6] = 254          # volume
disk[v + 0x27] = 122       # T/S pairs a list sector
disk[v + 0x34] = 35        # tracks
disk[v + 0x35] = 13        # sectors a track
disk[v + 0x37] = 1         # 256 bytes a sector
c = sector(17, 12) + 0x0B
disk[c] = 18; disk[c + 1] = 0; disk[c + 2] = 0x00     # T/S list, type T
name = b"HELLO".ljust(30)
disk[c + 3:c + 33] = bytes(b | 0x80 for b in name)
disk[c + 33] = 2                                      # 2 sectors
ts = sector(18, 0)
disk[ts + 0x0C] = 18; disk[ts + 0x0D] = 1
text = b"HELLO FROM DOS 3.2\r"
d = sector(18, 1)
disk[d:d + len(text)] = bytes(b | 0x80 for b in text)
open(sys.argv[1], "wb").write(disk)
EOF

hexat() { od -An -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n'; }

out=$(./wozit -d -I "$D" -c ls 2>&1)
assert_match "HELLO" "$out" "a .d13 opens and lists"
out=$(./wozit -d -I "$D" -c "cat7 HELLO" 2>&1)
assert_match "HELLO FROM DOS 3.2" "$out" "and its file reads through the 5-and-3 codec"

# Out to each format and back, byte for byte
for ext in woz nib 2mg; do
  ./wozzle -i "$D" -o "$TMP/t.$ext" >/dev/null 2>&1
  out=$(./wozit -d -I "$TMP/t.$ext" -c ls 2>&1)
  assert_match "HELLO" "$out" "the .$ext opens as 13-sector"
  ./wozzle -i "$TMP/t.$ext" -o "$TMP/back-$ext.d13" >/dev/null 2>&1
  assert_eq "$(hash_file "$D")" "$(hash_file "$TMP/back-$ext.d13")" \
    ".d13 to .$ext and back is the same"
done
out=$(./wozit -d -I "$TMP/t.woz" -c info 2>&1)
assert_match "Boot sector format: 13 sector" "$out" "the .woz says it's 13-sector"
assert_eq "02000000" "$(hexat "$TMP/t.2mg" 12 4)" "the .2mg holds it as nibbles"
./wozzle -s -i "$D" -o "$TMP/lazy.woz" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/t.woz")" "$(hash_file "$TMP/lazy.woz")" \
  "loading tracks as they're needed (-s) gives the same .woz"

# A 16-sector format can't hold it
out=$(./wozzle -i "$D" -o "$TMP/t.dsk" 2>&1)
assert_match "13-sector disk" "$out" "a .dsk is refused, with the reason"
out=$(./wozzle -i "$D" -o "$TMP/t.po" 2>&1)
assert_match "13-sector disk" "$out" "and so is a .po"

# Read only
printf 'X' > "$TMP/x.bin"
before=$(hash_file "$D")
out=$(./wozit -d -I "$D" -c "cpin $TMP/x.bin NEW" -c "rm HELLO" -c save 2>&1)
assert_match "can be read but not changed" "$out" "writing a file is refused"
assert_eq "$before" "$(hash_file "$D")" "and the disk is unchanged"

# And the other way around
./wozit -d -c "format $TMP/d33.dsk" >/dev/null 2>&1
out=$(./wozzle -i "$TMP/d33.dsk" -o "$TMP/d33.d13" 2>&1)
assert_match "is 16-sector" "$out" "a 16-sector disk can't be a .d13"
out=$(./wozit -d -c "format $TMP/new.d13" 2>&1)
assert_match "read but not make" "$out" "and format won't make one"

t_done
