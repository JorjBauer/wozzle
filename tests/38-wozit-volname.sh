#!/bin/bash
# `volname` command: rename a ProDOS volume (the volume directory header
# in block 2) - including on hard-drive images.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"    "wozit not built"
skip_if_missing "dos33.dsk"  "dos33.dsk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

./wozit -p -c "format $TMP/v.hdv 5m" >/dev/null

out=$(./wozit -p -I "$TMP/v.hdv" -c "volname my.new.drive" -c "save" 2>&1)
assert_match "Volume renamed to 'MY.NEW.DRIVE'" "$out" "volname renames and uppercases"

out=$(./wozit -p -I "$TMP/v.hdv" -c "ls" 2>&1)
assert_match "MY.NEW.DRIVE" "$out" "new name persists across save/reload"

# The rename touches only the header: volume must stay structurally clean.
if command -v python3 >/dev/null 2>&1; then
  out=$(python3 tests/prodos-verify.py "$TMP/v.hdv" 2>&1)
  assert_match "no shared blocks, bitmap consistent" "$out" "volume clean after rename"
fi

# Refusals: bad names leave the volume untouched, DOS mode is unsupported.
out=$(./wozit -p -I "$TMP/v.hdv" -c "volname BAD/NAME" 2>&1)
assert_match "bad volume name" "$out" "illegal character refused"
out=$(./wozit -p -I "$TMP/v.hdv" -c "volname 9LIVES" 2>&1)
assert_match "bad volume name" "$out" "leading digit refused"
out=$(./wozit -p -I "$TMP/v.hdv" -c "volname THIS.NAME.IS.WAY.TOO.LONG" 2>&1)
assert_match "1-15 characters" "$out" "overlong name refused"
out=$(./wozit -p -I "$TMP/v.hdv" -c "ls" 2>&1)
assert_match "MY.NEW.DRIVE" "$out" "failed renames left the name unchanged"
out=$(./wozit -d -I dos33.dsk -c "volname FOO" 2>&1)
assert_match "only supported for ProDOS" "$out" "volname refused in DOS mode"

t_done
