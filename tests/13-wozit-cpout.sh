#!/bin/bash
# cpout: discrepancy warning + -A flag behavior.
#
#   TAIPAN without -A:  its 144 bytes of contents + stderr warning
#   TAIPAN with    -A:  29696-byte file (the raw sectors), no warning
#   FID (normal BIN):   no warning (logical~allocated)
#
# Without -A, cpout copies a DOS 3.3 file's contents the way it does on
# ProDOS: a B file loses the 4-byte header (load address, then length) and
# comes out exactly as long as the header says. The -A copy is the file as
# it lies on disk, header and all, so the contents must be the 144 bytes
# right after the -A copy's header. That includes the last 4: an old
# reader dropped those, which for TAIPAN was the loader's closing JMP $D566.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"    "wozit not built"
skip_if_missing "taipan.dsk" "taipan.dsk fixture missing"
skip_if_missing "dos33.dsk"  "dos33.dsk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Case 1: TAIPAN without -A.
./wozit -I taipan.dsk -d -c "cpout TAIPAN $TMP/taipan-short.bin" \
  >"$TMP/out1" 2>"$TMP/err1"
assert_file_size "$TMP/taipan-short.bin" 144 "TAIPAN w/o -A is 144 bytes"
assert_match "WARNING"   "$(cat "$TMP/err1")" "stderr carries a warning"
assert_match "29696"     "$(cat "$TMP/err1")" "warning names allocated size"
assert_match "-A"        "$(cat "$TMP/err1")" "warning mentions the -A flag"

# Case 2: TAIPAN with -A.
./wozit -I taipan.dsk -d -A -c "cpout TAIPAN $TMP/taipan-full.bin" \
  >"$TMP/out2" 2>"$TMP/err2"
assert_file_size "$TMP/taipan-full.bin" 29696 "TAIPAN w/ -A is 29696 bytes"
assert_no_match "WARNING" "$(cat "$TMP/err2")" "no warning on -A path"

# The contents are the bytes the -A copy's header describes
python3 -c "
import sys
short = open('$TMP/taipan-short.bin','rb').read()
full = open('$TMP/taipan-full.bin','rb').read()
n = full[2] | (full[3] << 8)
sys.exit(0 if n == 144 and short == full[4:4+n] else 1)" 2>/dev/null
assert_eq 0 "$?" "contents are the header's 144 bytes, the last 4 included"

# Case 3: normal BIN doesn't trip the warning.
./wozit -I dos33.dsk -d -c "cpout FID $TMP/fid.bin" \
  >"$TMP/out3" 2>"$TMP/err3"
assert_no_match "WARNING" "$(cat "$TMP/err3")" "no warning for normal BIN"

t_done
