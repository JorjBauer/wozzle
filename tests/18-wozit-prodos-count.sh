#!/bin/bash
# ProDOS directory-header file-count validation. The volume directory (and
# every subdirectory) carries a fileCount field; older tools like ProDOS
# 1.1.1's CAT trust it to know when to stop. If the header lies, extra
# entries can be hidden from older software but still reachable through
# a full scan. wozit should flag that mismatch.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"   "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing (used as format host)"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# A freshly-formatted empty ProDOS volume: header says 0, scan finds 0.
./wozit -I dos33.dsk -p -c "format $TMP/clean.po BLANKVOL" >/dev/null 2>&1
clean_out=$(./wozit -I "$TMP/clean.po" -p -c "ls" 2>&1)
assert_no_match "header says" "$clean_out" "well-formed ProDOS: no count warning"

# Tamper: tell the header there are 5 active entries, but leave the
# directory body empty. Offset of fileCount in the .po file:
#   block 2 starts at byte 1024.
#   _subdirent lives at offset 4 inside the block.
#   fileCount sits at byte 33 inside the _subdirent.
# So fileCount[0] = byte 1024 + 4 + 33 = 1061 (little-endian).
cp "$TMP/clean.po" "$TMP/lying.po"
printf '\x05\x00' | dd of="$TMP/lying.po" bs=1 seek=1061 count=2 \
    conv=notrunc >/dev/null 2>&1

lying_out=$(./wozit -I "$TMP/lying.po" -p -c "ls" 2>&1)
assert_match "header says"       "$lying_out" "tampered header count is flagged"
assert_match "says 5"            "$lying_out" "warning reports declared count (5)"
assert_match "found 0"           "$lying_out" "warning reports scanned count (0)"
assert_match "ProDOS 1.1.1"      "$lying_out" "warning names the impacted tool"

t_done
