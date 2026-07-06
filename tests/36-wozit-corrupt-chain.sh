#!/bin/bash
# Corrupt-volume resilience: directory chains that point outside the
# volume or loop must produce a warning and a truncated listing, never a
# crash; write operations through corrupt structures must refuse; reads
# of files with out-of-range pointers substitute zeros.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"
if ! command -v python3 >/dev/null 2>&1; then
  printf "  SKIP: python3 needed to corrupt the fixture\n"
  exit 77
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Build a healthy volume with a subdirectory and a couple of files. SUB's
# first block is filled to capacity (12 entries) so that adding another
# file MUST follow the (soon to be corrupted) next-block link.
./wozit -p -c "format $TMP/vol.hdv CORRUPTME 1600" >/dev/null
head -c 2000 /dev/urandom > "$TMP/a.bin"
fills=()
for i in 01 02 03 04 05 06 07 08 09 10 11; do
  fills+=(-c "cpin $TMP/a.bin SUB/F$i")
done
./wozit -p -I "$TMP/vol.hdv" -c "mkdir SUB" -c "cpin $TMP/a.bin SUB/AFILE" \
  "${fills[@]}" -c "cpin $TMP/a.bin BFILE" -c "save" >/dev/null 2>&1

# Find SUB's key block, then corrupt things with surgical patches.
python3 - "$TMP/vol.hdv" <<'EOF'
import sys
p = sys.argv[1]
img = bytearray(open(p, 'rb').read())
BLK = 512
# locate SUB's entry in the volume dir (block 2): scan entries for name SUB
subkey = None
for i in range(1, 13):
    off = 2*BLK + 4 + i*0x27
    if img[off] & 0xF and img[off+1:off+4] == b'SUB':
        subkey = img[off+0x11] | (img[off+0x12] << 8)
if subkey is None:
    sys.exit("SUB not found")
# 1. SUB's chain: next pointer -> 65535 (out of range)
img[subkey*BLK+2] = 0xFF; img[subkey*BLK+3] = 0xFF
# 2. BFILE: point its index block out of range too
for i in range(1, 13):
    off = 2*BLK + 4 + i*0x27
    if img[off] & 0xF and img[off+1:off+6] == b'BFILE':
        img[off+0x11] = 0xFE; img[off+0x12] = 0xFF
open(p, 'wb').write(img)
EOF

# ls must warn, not crash, and still show the healthy entries.
out=$(./wozit -p -I "$TMP/vol.hdv" -c "ls" 2>&1); rc=$?
assert_eq 0 $rc "ls survives an out-of-range directory chain"
assert_match "outside the volume" "$out" "ls warns about the corrupt chain"
assert_match "AFILE" "$out" "entries before the corruption still list"

# cpout of the corrupted file: zero-filled result of the right size, warning.
out=$(./wozit -p -I "$TMP/vol.hdv" -c "cpout BFILE $TMP/b.out" 2>&1); rc=$?
assert_eq 0 $rc "cpout survives an out-of-range index block"
assert_match "outside the volume" "$out" "cpout warns about bad pointers"
assert_file_size "$TMP/b.out" 2000 "zero-substituted file keeps its EOF length"
if cmp -s "$TMP/b.out" <(head -c 2000 /dev/zero); then
  printf "  PASS: unreadable file reads as zeros\n"; t_pass=$((t_pass+1))
else
  printf "  FAIL: expected zero-filled output\n"; t_fail=$((t_fail+1))
fi

# Writing into the corrupt directory must refuse, not scribble.
out=$(./wozit -p -I "$TMP/vol.hdv" -c "cpin $TMP/a.bin SUB/NEWFILE" -c "save" 2>&1); rc=$?
assert_eq 0 $rc "cpin into a corrupt directory does not crash"
assert_match "ERROR" "$out" "cpin into a corrupt directory refuses"

# Loop corruption: point SUB's chain back at itself.
python3 - "$TMP/vol.hdv" <<'EOF'
import sys
p = sys.argv[1]
img = bytearray(open(p, 'rb').read())
BLK = 512
for i in range(1, 13):
    off = 2*BLK + 4 + i*0x27
    if img[off] & 0xF and img[off+1:off+4] == b'SUB':
        subkey = img[off+0x11] | (img[off+0x12] << 8)
        img[subkey*BLK+2] = subkey & 0xFF; img[subkey*BLK+3] = subkey >> 8
open(p, 'wb').write(img)
EOF
out=$(./wozit -p -I "$TMP/vol.hdv" -c "ls" 2>&1); rc=$?
assert_eq 0 $rc "ls survives a looping directory chain"
assert_match "loops back" "$out" "ls warns about the loop"

t_done
