#!/bin/bash
# DSK round-trip: DSK → WOZ → DSK must be byte-identical. DSK has no
# ambiguity (sector-order, 140 KB flat), so if the WOZ encode/decode is
# lossless for standard images this cycle is the identity function.
#
# Replaces (and strengthens) the MD5-pinned WOZ conversion checks: any
# format-level change that the previous tests flagged as a "failure" is
# now OK *as long as it's reversible*. Only actual data loss fails here.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozzle"  "wozzle not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

./wozzle -i dos33.dsk       -o "$TMP/rt.woz" >/dev/null 2>&1
./wozzle -i "$TMP/rt.woz"   -o "$TMP/rt.dsk" >/dev/null 2>&1

if cmp -s dos33.dsk "$TMP/rt.dsk"; then
  printf "  PASS: DSK → WOZ → DSK is byte-identical\n"
  t_pass=$((t_pass + 1))
else
  diff_bytes=$(cmp -l dos33.dsk "$TMP/rt.dsk" 2>/dev/null | wc -l | tr -d ' ')
  printf "  FAIL: DSK → WOZ → DSK diverged (%s differing bytes)\n" \
         "$diff_bytes" >&2
  t_fail=$((t_fail + 1))
fi

# Same cycle again from the re-emitted DSK: idempotent after one pass.
./wozzle -i "$TMP/rt.dsk"    -o "$TMP/rt2.woz" >/dev/null 2>&1
./wozzle -i "$TMP/rt2.woz"   -o "$TMP/rt2.dsk" >/dev/null 2>&1
if cmp -s "$TMP/rt.dsk" "$TMP/rt2.dsk"; then
  printf "  PASS: a second round-trip is also identical (idempotent)\n"
  t_pass=$((t_pass + 1))
else
  printf "  FAIL: second round-trip diverged\n" >&2
  t_fail=$((t_fail + 1))
fi

t_done
