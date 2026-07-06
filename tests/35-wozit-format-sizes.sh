#!/bin/bash
# `format` with a size argument: ProDOS hard-drive images (800k/5m/10m/32m
# presets or a raw block count), plus the tree-file (>128k) read/write/
# delete paths those volumes made reachable.
#
# Every image is checked with tests/prodos-verify.py, an independent
# walker that fails if any two structures claim the same block (the
# shared-block data-loss class) or the bitmap disagrees with the tree.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"
if ! command -v python3 >/dev/null 2>&1; then
  printf "  SKIP: python3 needed for structural verification\n"
  exit 77
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

verify() {
  # verify <image> <description>
  out=$(python3 tests/prodos-verify.py "$1" 2>&1)
  assert_match "no shared blocks, bitmap consistent" "$out" "$2"
}

# --- creation: no -I needed for a pure format run ----------------------
out=$(./wozit -p -c "format $TMP/f5.hdv PROFILE 5m" 2>&1)
assert_match "volume 'PROFILE', 9728 blocks" "$out" "format runs without -I"
assert_file_size "$TMP/f5.hdv" 4980736 "5m image is 9728*512 bytes"
verify "$TMP/f5.hdv" "blank 5m image is structurally clean"

./wozit -p -c "format $TMP/f800.hdv 800k" >/dev/null 2>&1
assert_file_size "$TMP/f800.hdv" 819200 "800k image is 1600*512 bytes"

./wozit -p -c "format $TMP/f10.hdv 10m" >/dev/null 2>&1
assert_file_size "$TMP/f10.hdv" 9961472 "10m image is 19456*512 bytes"

./wozit -p -c "format $TMP/f32.hdv BIG 32m" >/dev/null 2>&1
assert_file_size "$TMP/f32.hdv" 33553920 "32m image is 65535*512 bytes"
verify "$TMP/f32.hdv" "blank 32m image is structurally clean (16 bitmap blocks)"

./wozit -p -c "format $TMP/fodd.hdv ODD 12345" >/dev/null 2>&1
assert_file_size "$TMP/fodd.hdv" 6320640 "raw block count image is 12345*512 bytes"
verify "$TMP/fodd.hdv" "odd-sized image is structurally clean"

out=$(./wozit -p -c "format $TMP/lc.hdv myvol.2 5m" 2>&1)
assert_match "volume 'MYVOL.2'" "$out" "volume name is uppercased"

# --- refusals -----------------------------------------------------------
out=$(./wozit -p -c "format $TMP/bad.po 32m" 2>&1)
assert_match "must be named .hdv or .img" "$out" "non-140k size refuses a .po name"
out=$(./wozit -p -c "format $TMP/bad.hdv 4" 2>&1)
assert_match "bad size" "$out" "block count below 5 refused"
out=$(./wozit -p -c "format $TMP/bad.hdv 65536" 2>&1)
assert_match "bad size" "$out" "block count above 65535 refused"
out=$(./wozit -p -c "format $TMP/bad.hdv 5m 10m" 2>&1)
assert_match "more than one size" "$out" "duplicate size refused"
out=$(./wozit -p -c "format $TMP/bad.hdv BAD/NAME 5m" 2>&1)
assert_match "bad volume name" "$out" "invalid volume name refused"
out=$(./wozit -d -c "format $TMP/bad.dsk 254 800k" 2>&1)
assert_match "too many arguments" "$out" "DOS mode takes no size argument"
[ ! -e "$TMP/bad.po" ] && [ ! -e "$TMP/bad.hdv" ] && [ ! -e "$TMP/bad.dsk" ]
assert_eq 0 $? "refused formats left no files behind"

# --- round-trip into fresh directories (shared-block regression) --------
head -c 300    /dev/urandom > "$TMP/small.bin"
head -c 40000  /dev/urandom > "$TMP/mid.bin"
head -c 300000 /dev/urandom > "$TMP/big.bin"   # >128k: tree file

./wozit -p -I "$TMP/f5.hdv" \
  -c "mkdir DIR1" -c "mkdir DIR1/SUB" -c "mkdir DIR2" \
  -c "cpin $TMP/small.bin DIR1/SMALL" \
  -c "cpin $TMP/mid.bin DIR1/SUB/MID" \
  -c "cpin $TMP/big.bin DIR2/BIG" \
  -c "save" >/dev/null 2>&1

./wozit -p -I "$TMP/f5.hdv" \
  -c "cpout DIR1/SMALL $TMP/small.out" \
  -c "cpout DIR1/SUB/MID $TMP/mid.out" \
  -c "cpout DIR2/BIG $TMP/big.out" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/small.bin")" "$(hash_file "$TMP/small.out")" \
  "seedling file in a new directory round-trips"
assert_eq "$(hash_file "$TMP/mid.bin")" "$(hash_file "$TMP/mid.out")" \
  "sapling file in a nested new directory round-trips"
assert_eq "$(hash_file "$TMP/big.bin")" "$(hash_file "$TMP/big.out")" \
  "tree file (>128k) round-trips"
verify "$TMP/f5.hdv" "populated volume has no shared blocks"

# --- tree delete and rewrite --------------------------------------------
free_before=$(python3 tests/prodos-verify.py "$TMP/f5.hdv" | grep -oE '[0-9]+ free')
./wozit -p -I "$TMP/f5.hdv" -c "rm DIR2/BIG" -c "save" >/dev/null 2>&1
out=$(./wozit -p -I "$TMP/f5.hdv" -c "ls" 2>&1)
assert_no_match "BIG" "$out" "tree file can be deleted"
verify "$TMP/f5.hdv" "volume clean after tree delete"

./wozit -p -I "$TMP/f5.hdv" -c "cpin $TMP/big.bin DIR2/BIG" \
  -c "cpin $TMP/big.bin DIR2/BIG" -c "save" >/dev/null 2>&1
./wozit -p -I "$TMP/f5.hdv" -c "cpout DIR2/BIG $TMP/big2.out" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/big.bin")" "$(hash_file "$TMP/big2.out")" \
  "tree file rewritten over itself round-trips"
free_after=$(python3 tests/prodos-verify.py "$TMP/f5.hdv" | grep -oE '[0-9]+ free')
assert_eq "$free_before" "$free_after" "delete+rewrite cycle leaks no blocks"

# --- >64k files no longer silently truncate -----------------------------
head -c 70000 /dev/urandom > "$TMP/over64k.bin"
./wozit -p -I "$TMP/f32.hdv" -c "cpin $TMP/over64k.bin OVER64K" -c "save" >/dev/null 2>&1
./wozit -p -I "$TMP/f32.hdv" -c "cpout OVER64K $TMP/over64k.out" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/over64k.bin")" "$(hash_file "$TMP/over64k.out")" \
  "a >64k file is not truncated (was: 16-bit fileSize)"
verify "$TMP/f32.hdv" "32m volume clean after writes"

t_done
