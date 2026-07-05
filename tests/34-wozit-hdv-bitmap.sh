#!/bin/bash
# ProDOS volume-bitmap persistence past block 4096, and long host paths.
#
# Covers two bugs found while staging files onto a large (multi-bitmap-
# block) HDV:
#  - flushFreeBlockList() only wrote the first 512-byte block of the
#    volume bitmap, so any allocation of block >= 4096 was forgotten as
#    soon as the bitmap was re-read (which cpin does after every file).
#    Consecutive cpins then reused the same blocks: every file got the
#    same key block, and the last one written owned the data.
#  - cpin parsed its source path with a 127-char sscanf width, silently
#    splitting longer host paths ("COMPILER.S" staged as "COMPILER.").
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"
if ! command -v python3 >/dev/null 2>&1; then
  printf "  SKIP: python3 needed to synthesize the HDV fixture\n"
  exit 77
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Synthesize a 3 MB (6144-block) ProDOS HDV whose bitmap spans two
# blocks, with everything below block 4095 already marked used, so the
# very first allocation lands next to the 4096-block boundary.
python3 - "$TMP/big.hdv" <<'EOF'
import sys
TOTAL, FIRST_FREE = 6144, 4095
img = bytearray(512 * TOTAL)
for i, blk in enumerate([2, 3, 4, 5]):          # volume dir chain
    prev, nxt = [0, 2, 3, 4][i], [3, 4, 5, 0][i]
    img[512*blk+0] = prev & 0xFF; img[512*blk+1] = prev >> 8
    img[512*blk+2] = nxt  & 0xFF; img[512*blk+3] = nxt  >> 8
name = b'BIGVOL'
hdr = bytearray(0x27)
hdr[0] = 0xF0 | len(name)                        # vol dir header
hdr[1:1+len(name)] = name
hdr[0x1F], hdr[0x20] = 0x27, 0x0D                # entryLength, entriesPerBlock
hdr[0x23], hdr[0x24] = 6, 0                      # bitmap at block 6
hdr[0x25], hdr[0x26] = TOTAL & 0xFF, TOTAL >> 8
img[512*2+4:512*2+4+0x27] = hdr
for blk in range(FIRST_FREE, TOTAL):             # bit set = free
    img[512*6 + blk//8] |= (0x80 >> (blk % 8))
open(sys.argv[1], 'wb').write(bytes(img))
EOF
skip_if_missing "$TMP/big.hdv" "fixture synthesis failed"

head -c 728 /dev/zero | tr '\0' 'A' > "$TMP/a.bin"
head -c 728 /dev/zero | tr '\0' 'B' > "$TMP/b.bin"

# --- allocations past block 4096 must persist across the re-read ------
out=$(./wozit -I "$TMP/big.hdv" -p \
  -c "mkdir SUB" \
  -c "cpin $TMP/a.bin SUB/AFILE" \
  -c "cpin $TMP/b.bin SUB/BFILE" \
  -c "save $TMP/big.hdv" 2>&1)
assert_match "Created directory 'SUB'" "$out" "mkdir lands at the 4096-block boundary"

./wozit -I "$TMP/big.hdv" -p -c "cpout SUB/AFILE $TMP/a.out" \
                             -c "cpout SUB/BFILE $TMP/b.out" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/a.bin")" "$(hash_file "$TMP/a.out")" \
  "first file past block 4096 reads back intact from the saved image"
assert_eq "$(hash_file "$TMP/b.bin")" "$(hash_file "$TMP/b.out")" \
  "second file did not overwrite the first (distinct blocks allocated)"

# --- a >127-char host path stages under its full name ------------------
longdir="$TMP/$(printf 'x%.0s' $(seq 1 110))"
mkdir -p "$longdir"
longsrc="$longdir/COMPILER.S"                    # comfortably over 127 chars
printf 'long path payload\n' > "$longsrc"
out=$(./wozit -I "$TMP/big.hdv" -p \
  -c "cpin $longsrc SUB/COMPILER.S" -c "save $TMP/big.hdv" 2>&1)
assert_no_match "Unable to stat" "$out" "long source path is not truncated"
./wozit -I "$TMP/big.hdv" -p -c "cpout SUB/COMPILER.S $TMP/comp.out" >/dev/null 2>&1
assert_eq "$(hash_file "$longsrc")" "$(hash_file "$TMP/comp.out")" \
  "file staged via a long host path round-trips"

t_done
