#!/bin/bash
# ProDOS names on the way in: lowercase is upper-cased (as ProDOS does), a
# name ProDOS can't hold is refused rather than written, a long one isn't
# cut short, and lookups ignore case. A bad name once went onto the disk
# as-is, and the reader then took that directory block for garbage and
# hid everything after it.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

P="$TMP/n.po"
./wozit -p -c "format $P NAMES" >/dev/null 2>&1
printf 'X' > "$TMP/x.bin"

out=$(./wozit -p -I "$P" -c "mkdir games" -c "cpin $TMP/x.bin games/hello" \
              -c save 2>&1)
assert_no_match "WARNING" "$out" "lowercase names write cleanly"
ls_out=$(./wozit -p -I "$P" -c ls 2>&1)
assert_match "GAMES" "$ls_out" "mkdir games makes GAMES"
assert_match "HELLO" "$ls_out" "cpin to games/hello makes GAMES/HELLO"
assert_no_match "WARNING" "$ls_out" "and the directory reads without complaint"

for bad in 'BAD!' '"A B"' 1ABC; do
  out=$(./wozit -p -I "$P" -c "mkdir $bad" 2>&1)
  assert_match "bad ProDOS name" "$out" "mkdir $bad is refused"
done
out=$(./wozit -p -I "$P" -c "cpin $TMP/x.bin TOOLONGNAMEOVER15" -c ls 2>&1)
assert_match "must be 1-15 characters" "$out" "a 17-character name is refused"
assert_no_match "TOOLONGNAMEOVER" "$out" "not cut down to 15 and written"
assert_no_match "WARNING" "$(./wozit -p -I "$P" -c ls 2>&1)" \
                "refused names leave the directory readable"

out=$(./wozit -p -I "$P" -c "cpout games/Hello $TMP/out" 2>&1)
assert_eq "X" "$(cat "$TMP/out" 2>/dev/null)" "lookups ignore case"

t_done
