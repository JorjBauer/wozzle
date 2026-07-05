#!/bin/bash
# Directory growth, recursive rm, and block-leak rollback on ProDOS.
#
# Covers:
#  - cpin growing a full subdirectory past one block (>12 entries), the
#    parent DIR entry's blocksUsed/EOF bookkeeping, and reading an entry
#    that lives in the appended block back out intact.
#  - rm -r removing a directory tree (contents first) and reclaiming every
#    block, including the extra directory block(s).
#  - a failed (out-of-space) cpin not leaking the blocks it had begun to
#    allocate: a subsequent successful write must not lose them.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"   "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing (used as format host)"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

prodos_free() { ./wozit -I "$1" -p -c info 2>/dev/null | grep -oE 'Blocks free: [0-9]+' | grep -oE '[0-9]+'; }

# A blank ProDOS volume to work in.
./wozit -I dos33.dsk -p -c "format $TMP/v.po WOZGROW" >/dev/null 2>&1
skip_if_missing "$TMP/v.po" "format did not produce an image"
blank=$(prodos_free "$TMP/v.po")

printf 'x'                       > "$TMP/tiny.bin"
printf 'thirteenth entry payload\n' > "$TMP/mark.bin"

# --- cpin grows a full subdirectory -----------------------------------
# One directory block holds 12 file entries + 1 header. Put a subdir in
# place, fill its first block (12 files), then add more to force a grow.
cp "$TMP/v.po" "$TMP/a.po"
args=(-I "$TMP/a.po" -p -c "mkdir DIR")
for i in $(seq 1 12); do args+=(-c "cpin $TMP/tiny.bin DIR/F$i"); done
./wozit "${args[@]}" -c "save $TMP/a.po" >/dev/null 2>&1

one_block=$(./wozit -I "$TMP/a.po" -p -c "ls" 2>&1 | grep -E "DIR +DIR")
assert_regex "DIR +DIR +1 " "$one_block" "12 entries still fit in one directory block"

# The 13th entry must trigger growth; store a recognizable payload in it.
grow=$(./wozit -I "$TMP/a.po" -p -c "cpin $TMP/mark.bin DIR/F13" -c "save $TMP/a.po" 2>&1)
assert_no_match "no free directory" "$grow" "cpin no longer fails on a full directory"
assert_no_match "cannot be extended" "$grow" "a subdirectory (not the volume root) can grow"

two_block=$(./wozit -I "$TMP/a.po" -p -c "ls" 2>&1 | grep -E "DIR +DIR")
assert_regex "DIR +DIR +2 " "$two_block" "the directory grew to two blocks"
assert_regex "DIR +DIR +2 +.*1024" "$two_block" "the parent DIR entry EOF grew to 1024 bytes"

# The whole tree must still read cleanly (header fileCount cross-checks the
# full entry scan across both blocks - a bad link would warn here).
ls_a=$(./wozit -I "$TMP/a.po" -p -c "ls" 2>&1)
assert_no_match "WARNING" "$ls_a" "grown directory re-reads without warnings"
assert_match   "F13"      "$ls_a" "the 13th entry is listed"

# The entry living in the appended block must round-trip byte-for-byte.
./wozit -I "$TMP/a.po" -p -c "cpout DIR/F13 $TMP/f13.out" >/dev/null 2>&1
assert_eq "$(cat "$TMP/mark.bin")" "$(cat "$TMP/f13.out")" \
  "entry in the appended block reads back intact"

# --- rm -r tears the whole tree down and reclaims every block ---------
rmr=$(./wozit -I "$TMP/a.po" -p -c "rm -r DIR" -c "save $TMP/a.po" 2>&1)
assert_match "Removed directory 'DIR'" "$rmr" "rm -r removes the directory itself"
assert_match "Removed 'DIR/F13'"       "$rmr" "rm -r removes contents (including the grown block)"
assert_no_match "DIR" "$(./wozit -I "$TMP/a.po" -p -c "ls" 2>&1)" "tree is gone after rm -r"
assert_eq "$blank" "$(prodos_free "$TMP/a.po")" \
  "rm -r reclaims every block, including the extra directory block"

# rm -r on a plain file is refused (it is not a directory).
cp "$TMP/v.po" "$TMP/b.po"
./wozit -I "$TMP/b.po" -p -c "cpin $TMP/tiny.bin AFILE" -c "save $TMP/b.po" >/dev/null 2>&1
notdir=$(./wozit -I "$TMP/b.po" -p -c "rm -r AFILE" 2>&1)
assert_match "not a directory" "$notdir" "rm -r refuses a plain file"

# --- a failed (out-of-space) cpin must not leak blocks ----------------
# Fill the volume until a further big file cannot fit, attempt it (fails
# mid-allocation), then write a tiny file in the same session (which
# flushes the bitmap). If the failed attempt had leaked its partial
# allocation, the free count would crater instead of dropping by one.
cp "$TMP/v.po" "$TMP/c.po"
yes A | head -c 60000 > "$TMP/big.bin"   # ~119 blocks each
./wozit -I "$TMP/c.po" -p \
  -c "cpin $TMP/big.bin A" -c "cpin $TMP/big.bin B" -c "save $TMP/c.po" >/dev/null 2>&1
before=$(prodos_free "$TMP/c.po")        # not enough left for a third
fail=$(./wozit -I "$TMP/c.po" -p \
  -c "cpin $TMP/big.bin C" \
  -c "cpin $TMP/tiny.bin TINY" \
  -c "save $TMP/c.po" 2>&1)
assert_match "Failed to find" "$fail" "the third big cpin runs out of space"
assert_no_match "C  " "$(./wozit -I "$TMP/c.po" -p -c "ls" 2>&1)" "the failed file left no entry"
assert_match   "TINY" "$(./wozit -I "$TMP/c.po" -p -c "ls" 2>&1)" "the follow-up tiny file was written"
# TINY is a one-block seedling: free must drop by exactly one, proving the
# failed attempt's partial allocation was rolled back rather than leaked.
assert_eq "$((before - 1))" "$(prodos_free "$TMP/c.po")" \
  "a failed cpin leaks no blocks (free drops only by the tiny file)"

t_done
