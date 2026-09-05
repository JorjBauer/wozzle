#!/bin/bash
# Regression: the last line of a tokenized program must survive.
#
# Applesoft ends a program with two zero bytes, and the LAST line's
# next-line link points AT those bytes rather than being zero itself. Write
# 0x0000 into that link instead and every reader - the interpreter and our
# own lister - treats the last line as end-of-program and silently drops
# it, while still reporting success.
#
# The original version of this test copied a program in but never issued a
# `save`, so it read back whatever was already on the image and "passed"
# against a stale file. Every step here writes to a scratch image it just
# created, so the bytes under test are the ones cpin -B produced.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"   "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/startup.txt" <<'EOF'
10 PRINT CHR$(4)"PREFIX /PROFILE"
20 PRINT CHR$(4)"IN#1"
30 PRINT CHR$(4)"PR#1"
40 PRINT "Serial port is now listening"
EOF

./wozit -I dos33.dsk -p -c "format $TMP/v.po PROFILE" >/dev/null 2>&1
skip_if_missing "$TMP/v.po" "format did not produce an image"

out=$(./wozit -I "$TMP/v.po" -p -c "cpin -B $TMP/startup.txt STARTUP" \
                             -c "save $TMP/v.po" 2>&1)
assert_no_match "ERROR" "$out" "cpin -B tokenized the program"

./wozit -I "$TMP/v.po" -p -c "cpout STARTUP $TMP/startup.bin" >/dev/null 2>&1
hex=$(hexdump -ve '1/1 "%02x"' "$TMP/startup.bin")

# List it back. All four lines must be there.
./wozit -I "$TMP/v.po" -p -c "cpout -B STARTUP $TMP/startup.list" >/dev/null 2>&1
listing=$(cat "$TMP/startup.list")
assert_eq 4 "$(wc -l < "$TMP/startup.list" | tr -d ' ')" "all four lines list"
assert_match "Serial port is now listening" "$listing" "the final line survives"

# And check the structure directly, since the lister walks the program
# sequentially and would list all four lines even with the links wrong.
assert_regex "0000$" "$hex" "program ends with a 0000 marker"

# Line 40's link must hold the address of that marker: $0801 + its offset.
markerofs=$(( ${#hex} / 2 - 2 ))
printf -v markeraddr "%02x%02x" $(( (0x0801 + markerofs) & 255 )) \
                                $(( (0x0801 + markerofs) >> 8 ))
assert_match "${markeraddr}2800" "$hex" \
             "line 40's link points at the end marker (not 0000, not past it)"

t_done
