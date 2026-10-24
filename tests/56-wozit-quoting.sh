#!/bin/bash
# wozit's argument splitting: quotes and backslashes get a space into a
# name, and a command's last argument is still the rest of the line, as it
# always was.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

D="$TMP/q.dsk"
./wozit -d -c "format $D 254" >/dev/null 2>&1
printf 'ONE\r' > "$TMP/one.txt"
printf 'TWO\r' > "$TMP/two words.txt"

# A quoted destination, and a quoted host path with a space in it
./wozit -d -I "$D" -c "cpin $TMP/one.txt \"MY FILE\" 04" \
                   -c "cpin \"$TMP/two words.txt\" 'SECOND ONE' 04" \
                   -c "cpin $TMP/one.txt BACK\\ SLASH 04" \
                   -c save >/dev/null 2>&1
ls_out=$(./wozit -d -I "$D" -c ls 2>&1)
assert_match "MY FILE" "$ls_out" "a double-quoted name keeps its space"
assert_match "SECOND ONE" "$ls_out" "so does a single-quoted one"
assert_match "BACK SLASH" "$ls_out" "and a backslash-escaped one"

./wozit -d -I "$D" -c "cpout \"MY FILE\" $TMP/out1" >/dev/null 2>&1
assert_eq "$(printf 'ONE\r')" "$(cat "$TMP/out1")" "cpout takes a quoted source"
./wozit -d -I "$D" -c "cpout 'SECOND ONE' $TMP/out two" >/dev/null 2>&1
assert_eq "$(printf 'TWO\r')" "$(cat "$TMP/out two")" \
          "an unquoted last argument is still the rest of the line"

# The rest-of-line rule for a name that's the only argument
out=$(./wozit -d -I "$D" -c "inspect MY FILE" 2>&1)
assert_match "Entry dump for 'MY FILE'" "$out" "inspect MY FILE, unquoted"

out=$(./wozit -d -I "$D" -c "cpout \"MY FILE\" a \"b\"" 2>&1)
assert_match "too many arguments" "$out" "quoted extras are refused"
out=$(./wozit -d -I "$D" -c "cpout \"MY FILE $TMP/x" 2>&1)
assert_match "unterminated" "$out" "an unterminated quote is refused"

./wozit -d -I "$D" -c "rm \"MY FILE\"" -c save >/dev/null 2>&1
assert_no_match "MY FILE" "$(./wozit -d -I "$D" -c ls 2>&1)" \
                "rm takes a quoted name"

# A flag in quotes is a name, not a flag
out=$(./wozit -d -I "$D" -c "rm \"-r\"" 2>&1)
assert_no_match "Usage: rm -r" "$out" "a quoted -r isn't the flag"

t_done
