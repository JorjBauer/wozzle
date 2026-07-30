#!/bin/bash
# `format` for UCSD p-System / Apple Pascal volumes: sizing, naming, the
# container-extension rules, and the embedded p-System bootstraps.
#
# The formatter writes only the 26-byte volume header; everything else on
# a blank Pascal volume is zeros. The real check is therefore that wozit
# can reopen its own output (probe/readVolHeader accept it) and that files
# round-trip through it, so most assertions here go out through format and
# back in through ls/cpout.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

head -c 3000  /dev/urandom > "$TMP/a.bin"
head -c 40000 /dev/urandom > "$TMP/b.bin"

# roundtrip <image> <label>: cpin two files, save, reopen, cpout, compare.
roundtrip() {
  ./wozit -P -I "$1" -c "cpin $TMP/a.bin A.DATA" \
                     -c "cpin $TMP/b.bin B.DATA" -c save >/dev/null 2>&1
  rm -f "$TMP/a.out" "$TMP/b.out"
  ./wozit -P -I "$1" -c "cpout A.DATA $TMP/a.out" \
                     -c "cpout B.DATA $TMP/b.out" >/dev/null 2>&1
  assert_eq "$(hash_file "$TMP/a.bin")" "$(hash_file "$TMP/a.out")" \
    "$2: small file round-trips"
  assert_eq "$(hash_file "$TMP/b.bin")" "$(hash_file "$TMP/b.out")" \
    "$2: multi-block file round-trips"
}

# --- 140k floppy ---------------------------------------------------------
out=$(./wozit -P -c "format $TMP/w.po WORK 140k" 2>&1)
assert_match "volume 'WORK', 280 blocks" "$out" "140k format reports size and name"
assert_file_size "$TMP/w.po" 143360 "140k image is 280*512 bytes"

out=$(./wozit -P -I "$TMP/w.po" -c info 2>&1)
assert_match "Pascal volume: WORK" "$out" "wozit reopens its own 140k output"
assert_match "Total blocks:  280" "$out" "volume header records the real block count"
assert_match "Files:         0" "$out" "a fresh volume is empty"
assert_match "Largest free contiguous region: 274 blocks at block 6" "$out" \
  "free space is one run starting past the directory"
# The header date must decode to today under Apple's windowing (2000-2039
# encode as year 0-39). Clamping into 1999 was the old behaviour; a year
# field of 100 or more would make the p-System delete the entry outright.
if command -v python3 >/dev/null 2>&1; then
  got=$(python3 -c "
d = open('$TMP/w.po','rb').read()
b2 = d[1024:1536]
dt = b2[20] | (b2[21] << 8)
y, day, mon = (dt >> 9) & 0x7f, (dt >> 4) & 0x1f, dt & 0xf
print('%04d-%02d-%02d' % (2000 + y if y < 40 else 1900 + y, mon, day))
")
  assert_eq "$(date +%Y-%m-%d)" "$got" "volume header is stamped with today's date"
fi

roundtrip "$TMP/w.po" "140k"

# --- 800k as .po (the relaxed extension rule) ----------------------------
out=$(./wozit -P -c "format $TMP/w8.po WORK8 800k" 2>&1)
assert_match "volume 'WORK8', 1600 blocks" "$out" "800k .po is accepted"
assert_file_size "$TMP/w8.po" 819200 "800k image is 1600*512 bytes"
roundtrip "$TMP/w8.po" "800k .po"

# --- hard-disk sizes -----------------------------------------------------
out=$(./wozit -P -c "format $TMP/w8m.hdv WORK 8m" 2>&1)
assert_match "volume 'WORK', 16384 blocks" "$out" "8m format reports 16384 blocks"
assert_file_size "$TMP/w8m.hdv" 8388608 "8m image is 16384*512 bytes"
roundtrip "$TMP/w8m.hdv" "8m .hdv"

out=$(./wozit -P -c "format $TMP/big.hdv BIG 65535" 2>&1)
assert_match "past the 16MB" "$out" "an oversized volume warns but is allowed"

# --- name validation -----------------------------------------------------
out=$(./wozit -P -c "format $TMP/x.po BAD:NAME" 2>&1)
assert_match "':' not allowed in a Pascal volume name" "$out" "reserved char refused"
out=$(./wozit -P -c "format $TMP/x.po TOOLONGNAME" 2>&1)
assert_match "volume name max 7 chars" "$out" "overlong name refused"
./wozit -P -c "format $TMP/lc.po ok" >/dev/null 2>&1
out=$(./wozit -P -I "$TMP/lc.po" -c info 2>&1)
assert_match "Pascal volume: OK" "$out" "volume name is uppercased"
out=$(./wozit -P -c "format $TMP/dflt.po" 2>&1)
assert_match "volume 'BLANK'" "$out" "default volume name is BLANK"

# --- container extension rules ------------------------------------------
out=$(./wozit -P -c "format $TMP/x.po WORK 8m" 2>&1)
assert_match "must be named .hdv or .img" "$out" "a hard-disk size refuses .po"
out=$(./wozit -P -c "format $TMP/x.dsk WORK 140k" 2>&1)
assert_match "must be named .po, .hdv or .img" "$out" \
  ".dsk refused (it would force DOS sector order)"
out=$(./wozit -P -c "format $TMP/x.po WORK 100" 2>&1)
assert_match "must be named .hdv or .img" "$out" \
  "a sub-floppy size refuses .po (won't load through the floppy path)"

# --- other refusals ------------------------------------------------------
out=$(./wozit -P -c "format $TMP/w.po WORK 140k" 2>&1)
assert_match "already exists; refusing to overwrite" "$out" "existing file not clobbered"
out=$(./wozit -P -c "format $TMP/x.hdv WORK 5" 2>&1)
assert_match "bad size" "$out" "a block count below the 6-block minimum is refused"
out=$(./wozit -P -c "format $TMP/x.hdv WORK 140k 800k" 2>&1)
assert_match "more than one size" "$out" "duplicate size refused"
[ ! -e "$TMP/x.po" ] && [ ! -e "$TMP/x.dsk" ] && [ ! -e "$TMP/x.hdv" ]
assert_eq 0 $? "refused formats left no files behind"

# --- bootable: the embedded p-System bootstraps --------------------------
# 5.25" boots through the Disk II controller and needs blocks 0 and 1;
# 3.5"/hard disk boots through the slot's ProDOS block driver and uses
# block 0 alone. format must pick by size.
./wozit -P -c "format $TMP/bf.po BOOTME 140k bootable" >/dev/null 2>&1
dd if="$TMP/bf.po" bs=512 count=1        2>/dev/null > "$TMP/f.b0"
dd if="$TMP/bf.po" bs=512 skip=1 count=1 2>/dev/null > "$TMP/f.b1"
head -c 512 /dev/zero > "$TMP/zero.b512"
assert_eq "01" "$(od -An -tx1 -N1 "$TMP/f.b0" | tr -d ' ')" \
  "5.25\" bootable: block 0 starts with \$01"
assert_no_match "$(hash_file "$TMP/zero.b512")" "$(hash_file "$TMP/f.b1")" \
  "5.25\" bootable: block 1 carries the sector reader too"

./wozit -P -c "format $TMP/bh.hdv BOOTME 8m bootable" >/dev/null 2>&1
dd if="$TMP/bh.hdv" bs=512 skip=1 count=1 2>/dev/null > "$TMP/h.b1"
assert_eq "01" "$(od -An -tx1 -N1 "$TMP/bh.hdv" | tr -d ' ')" \
  "hard-disk bootable: block 0 starts with \$01"
assert_eq "$(hash_file "$TMP/zero.b512")" "$(hash_file "$TMP/h.b1")" \
  "hard-disk bootable: block 1 stays zero"

# The two bootstraps are genuinely different code, not one block reused.
assert_no_match "$(hash_file "$TMP/f.b0")" "$(hash_file "$TMP/bh.hdv")" \
  "5.25\" and hard-disk bootstraps differ"

# A bootable volume is still a volume: it must open and hold files.
out=$(./wozit -P -I "$TMP/bh.hdv" -c info 2>&1)
assert_match "Pascal volume: BOOTME" "$out" "bootable volume still opens"
roundtrip "$TMP/bh.hdv" "bootable 8m"

# --- bootblocks retrofits the same code onto an existing volume ----------
./wozit -P -c "format $TMP/retro.po RETRO 140k" >/dev/null 2>&1
out=$(./wozit -P -I "$TMP/retro.po" -c bootblocks -c save 2>&1)
assert_match "embedded p-System boot code" "$out" "bootblocks uses the embedded code"
assert_match "SYSTEM.APPLE and SYSTEM.PASCAL" "$out" "bootblocks names the system files"
dd if="$TMP/retro.po" bs=1024 count=1 2>/dev/null > "$TMP/retro.boot"
dd if="$TMP/bf.po"    bs=1024 count=1 2>/dev/null > "$TMP/bf.boot"
assert_eq "$(hash_file "$TMP/bf.boot")" "$(hash_file "$TMP/retro.boot")" \
  "bootblocks matches what format bootable installs"

# A ProDOS image is not a valid Pascal boot donor.
if [ -e "ProDOS_2_4.po" ]; then
  out=$(./wozit -P -I "$TMP/bh.hdv" -c "bootblocks ProDOS_2_4.po" 2>&1)
  assert_match "doesn't look like a Pascal volume" "$out" "ProDOS donor refused"
fi

t_done
