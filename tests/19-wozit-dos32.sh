#!/bin/bash
# DOS 3.2 (13-sector) read support. The WOZ test image for the DOS 3.2
# System Master is the canonical test fixture — its catalog includes
# HELLO (Integer BASIC, 2 sectors).
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"
IMG="woz test images/WOZ 2.0/DOS 3.2 System Master.woz"
skip_if_missing "$IMG" "DOS 3.2 System Master WOZ fixture missing"

# Catalog listing: expected 13-sector System Master files.
ls_out=$(./wozit -I "$IMG" -d -c "ls" 2>/dev/null)
assert_match "HELLO"      "$ls_out" "13-sector catalog shows HELLO"
assert_match "APPLE-TREK" "$ls_out" "13-sector catalog shows APPLE-TREK"
assert_match "RENUMBER"   "$ls_out" "13-sector catalog shows RENUMBER"

# Integer BASIC detokenization of HELLO on the 3.2 disk.
list_out=$(./wozit -I "$IMG" -d -c "list HELLO" 2>/dev/null)
assert_match "MASTER DISKETTE VERSION 3.2" "$list_out" "HELLO string constant decoded"
assert_match "TEXT"  "$list_out" "Integer BASIC keyword decoded"
assert_match "PRINT" "$list_out" "PRINT keyword decoded"
# No spurious ERROR lines: the 5&3 decode path must reach EOL cleanly.
assert_no_match "ERROR:" "$list_out" "list has no ERRORs"

# cpout: pulls the file through the 5&3 codec and writes it.
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
./wozit -I "$IMG" -d -c "cpout HELLO $TMP/hello.bin" 2>/dev/null
# Size sanity: Integer BASIC HELLO header claims ~136 bytes, +2 for the
# DOS length prefix. Just check it's non-empty and under 512 bytes.
sz=$(wc -c < "$TMP/hello.bin" | tr -d ' ')
if [ "$sz" -ge 10 ] && [ "$sz" -le 512 ]; then
  printf "  PASS: cpout HELLO is plausibly sized (%s bytes)\n" "$sz"
  t_pass=$((t_pass + 1))
else
  printf "  FAIL: cpout HELLO size %s outside expected 10..512 range\n" "$sz" >&2
  t_fail=$((t_fail + 1))
fi

# inspect: walks the T/S list using the 5&3 codec. Must find the BIN/INT
# header without errors.
ins=$(./wozit -I "$IMG" -d -c "inspect HELLO" 2>/dev/null)
assert_match    "Integer BASIC length header" "$ins" "inspect decodes Integer BASIC header"
assert_no_match "failed to read T/S list"     "$ins" "T/S walk reaches the list without error"

# Probe: invoking DOS 3.2 image with -p should warn; -d should not.
w=$(./wozit -I "$IMG" -p -c "ls" 2>&1)
assert_match "WARNING" "$w" "mode-mismatch warning on DOS-3.2 image with -p"
w2=$(./wozit -I "$IMG" -d -c "ls" 2>&1)
assert_no_match "WARNING" "$w2" "no warning on DOS-3.2 image with -d"

t_done
