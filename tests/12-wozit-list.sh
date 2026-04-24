#!/bin/bash
# Detokenization smoke tests for Applesoft and Integer BASIC.
# Looks for specific line numbers and bits of recognizable source text that
# should appear after detokenization.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"        "wozit not built"
skip_if_missing "dos33.dsk"      "dos33.dsk fixture missing"
skip_if_missing "AppleTrek.dsk"  "AppleTrek.dsk fixture missing"

# Applesoft: RENUMBER on dos33.dsk. Last line is 10010 with a CALL
# expression containing the characteristic PEEK(121)+PEEK(122)*256.
out=$(./wozit -I dos33.dsk -d -c "list RENUMBER" 2>&1)
assert_match "10000"      "$out" "RENUMBER has line 10000"
assert_match "10010"      "$out" "RENUMBER has line 10010"
assert_match "PEEK"       "$out" "Applesoft PEEK keyword detokenized"
assert_match "REM"        "$out" "Applesoft REM keyword detokenized"
# No mid-listing error lines.
assert_no_match "ERROR:"  "$out" "Applesoft listing has no ERRORs"

# Integer BASIC: APPLE.TREK on AppleTrek.dsk. Known content includes
# STARFLEET COMMAND, STARSHIP, HIMEM:16384. With the resync fix, the
# listing must reach line 20000 (the last line) even though line 19900
# has an embedded 0x01 that trips the inner REM scan.
out2=$(./wozit -I AppleTrek.dsk -d -c "list APPLE.TREK" 2>&1)
assert_match "STARFLEET COMMAND" "$out2" "Integer BASIC string decoded"
assert_match "HIMEM:"            "$out2" "Integer BASIC keyword decoded"
assert_match "GOSUB"             "$out2" "Integer BASIC GOSUB decoded"
assert_match "19900"             "$out2" "reaches line 19900"
assert_match "20000"             "$out2" "reaches line 20000 (after resync)"
assert_no_match "Failed to list" "$out2" "list didn't bail out"

t_done
