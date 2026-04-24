#!/bin/bash
# Pins the semantic data `inspect` derives about a known file rather than
# the exact phrasing of the output. Covers:
#   - catalog sector count and computed byte allocation
#   - T/S walk chain traversal
#   - BIN header extraction
#   - stub-loader discrepancy detection (TAIPAN)
#   - no false positive on a normal BIN (FID)
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"     "wozit not built"
skip_if_missing "taipan.dsk"  "taipan.dsk fixture missing"
skip_if_missing "dos33.dsk"   "dos33.dsk fixture missing"

# TAIPAN is the canonical stub-loader case: 117 catalog sectors, 1 T/S list
# sector + 116 data sectors, BIN header says load=$0300 length=144.
out=$(./wozit -I taipan.dsk -d -c "inspect TAIPAN" 2>&1)
assert_match "Catalog sector count: 117" "$out" "TAIPAN catalog=117 sectors"
assert_match "29952 bytes allocated"     "$out" "117 sectors = 29952 bytes"
assert_match "116 data sector(s)"        "$out" "T/S walk finds 116 data sectors"
assert_match "0300"                      "$out" "TAIPAN BIN load addr \$0300"
assert_match "length=144"                "$out" "TAIPAN BIN header length=144"
assert_match "stub loader"               "$out" "TAIPAN flagged as stub loader"

# FID is a normal BIN: ~4687-byte logical length, 20 sectors allocated.
# Should NOT trip the stub-loader flag (its slack is well under 1K).
out2=$(./wozit -I dos33.dsk -d -c "inspect FID" 2>&1)
assert_match   "Catalog sector count: 20" "$out2" "FID catalog=20 sectors"
assert_no_match "stub loader"             "$out2" "FID not flagged as stub loader"

t_done
