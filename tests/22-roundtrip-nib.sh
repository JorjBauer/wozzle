#!/bin/bash
# NIB sector preservation. NIB files have bit-level ambiguity (sync
# patterns, gap bytes) so a NIB → NIB comparison isn't useful, but the
# sectors inside a NIB must survive round-tripping through DSK — if they
# don't, the NIB decoder or re-encoder has lost data.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozzle"     "wozzle not built"
skip_if_missing "dos33sm.nib"  "dos33sm.nib fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# NIB → DSK, then DSK → NIB → DSK. Both DSKs must match.
./wozzle -i dos33sm.nib   -o "$TMP/a.dsk" >/dev/null 2>&1
./wozzle -i "$TMP/a.dsk"  -o "$TMP/b.nib" >/dev/null 2>&1
./wozzle -i "$TMP/b.nib"  -o "$TMP/b.dsk" >/dev/null 2>&1

if cmp -s "$TMP/a.dsk" "$TMP/b.dsk"; then
  printf "  PASS: NIB → DSK → NIB → DSK preserves every sector\n"
  t_pass=$((t_pass + 1))
else
  diff_bytes=$(cmp -l "$TMP/a.dsk" "$TMP/b.dsk" 2>/dev/null | wc -l | tr -d ' ')
  printf "  FAIL: NIB round-trip lost %s bytes\n" "$diff_bytes" >&2
  t_fail=$((t_fail + 1))
fi

t_done
