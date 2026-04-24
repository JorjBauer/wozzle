#!/bin/bash
# WOZ round-trip + WOZ1/WOZ2 equivalence.
#
# Uses the standard DOS 3.3 System Master reference images. For a disk
# that's just DOS 3.3 (no copy protection, no half-tracks), the WOZ 1.0
# and WOZ 2.0 archives must decode to the same sector contents — i.e.
# the same DSK. And going WOZ → DSK → WOZ → DSK should converge after
# the first DSK (the DSK is the canonical form).
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozzle" "wozzle not built"
W1="woz test images/WOZ 1.0/DOS 3.3 System Master.woz"
W2="woz test images/WOZ 2.0/DOS 3.3 System Master.woz"
skip_if_missing "$W1" "WOZ 1.0 System Master missing"
skip_if_missing "$W2" "WOZ 2.0 System Master missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

./wozzle -i "$W1" -o "$TMP/v1.dsk" >/dev/null 2>&1
./wozzle -i "$W2" -o "$TMP/v2.dsk" >/dev/null 2>&1

if cmp -s "$TMP/v1.dsk" "$TMP/v2.dsk"; then
  printf "  PASS: WOZ 1.0 and WOZ 2.0 of System Master decode to same DSK\n"
  t_pass=$((t_pass + 1))
else
  printf "  FAIL: WOZ 1.0 and WOZ 2.0 decode to different DSKs\n" >&2
  t_fail=$((t_fail + 1))
fi

# DSK → WOZ → DSK from the decoded master: cycle through WOZ2 canonical
# form and confirm the DSK stays stable.
./wozzle -i "$TMP/v2.dsk"  -o "$TMP/cycle.woz" >/dev/null 2>&1
./wozzle -i "$TMP/cycle.woz" -o "$TMP/cycle.dsk" >/dev/null 2>&1
if cmp -s "$TMP/v2.dsk" "$TMP/cycle.dsk"; then
  printf "  PASS: DSK → WOZ → DSK stable for System Master\n"
  t_pass=$((t_pass + 1))
else
  printf "  FAIL: DSK → WOZ → DSK diverged for System Master\n" >&2
  t_fail=$((t_fail + 1))
fi

t_done
