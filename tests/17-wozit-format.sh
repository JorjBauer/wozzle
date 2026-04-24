#!/bin/bash
# `format` command: creates blank DOS 3.3 / ProDOS images from scratch.
# Verifies structure by round-tripping through wozit itself (ls, probe).
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"   "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing (needed as format host)"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# DOS 3.3 format.
./wozit -I dos33.dsk -d -c "format $TMP/fresh.dsk 42" >"$TMP/out.d" 2>&1
assert_file_size "$TMP/fresh.dsk" 143360 "DOS image is 35*16*256 bytes"

out=$(./wozit -I "$TMP/fresh.dsk" -d -v -c "info" 2>&1)
assert_match "Volume number: 42"    "$out" "volume number written correctly"
assert_match "Catalog made for DOS version 3" "$out" "DOS version byte is 3"
assert_match "Tracks per disk: 35"  "$out" "tracks per disk is 35"
assert_match "Sectors per track: 16" "$out" "sectors per track is 16"

# Empty catalog: ls should print nothing file-like. Count non-blank lines
# after ignoring the verbose banner.
ls_out=$(./wozit -I "$TMP/fresh.dsk" -d -c "ls" 2>&1)
# A populated DOS image would have lines like " A 003 HELLO". Fresh should
# have none.
if printf "%s" "$ls_out" | grep -qE "^ [ABIT*] +[0-9]+ "; then
  printf "  FAIL: fresh DOS catalog contains entries\n" >&2
  t_fail=$((t_fail + 1))
else
  printf "  PASS: fresh DOS catalog is empty\n"
  t_pass=$((t_pass + 1))
fi

# Probe must confirm DOS when loaded with -d, and warn with -p.
probe_d=$(./wozit -I "$TMP/fresh.dsk" -d -c "ls" 2>&1)
assert_no_match "WARNING" "$probe_d" "fresh DOS image loads clean with -d"
probe_p=$(./wozit -I "$TMP/fresh.dsk" -p -c "ls" 2>&1)
assert_match "WARNING"    "$probe_p" "fresh DOS image warns with -p"
assert_match "DOS 3.3 image" "$probe_p" "probe identifies DOS correctly"

# ProDOS format.
./wozit -I dos33.dsk -p -c "format $TMP/fresh.po MYVOL" >"$TMP/out.p" 2>&1
assert_file_size "$TMP/fresh.po" 143360 "ProDOS image is 280*512 bytes"

ls_po=$(./wozit -I "$TMP/fresh.po" -p -c "ls" 2>&1)
assert_match "MYVOL" "$ls_po" "ProDOS volume name present"

# Probe both ways.
probe_pp=$(./wozit -I "$TMP/fresh.po" -p -c "ls" 2>&1)
assert_no_match "WARNING" "$probe_pp" "fresh ProDOS image loads clean with -p"
probe_pd=$(./wozit -I "$TMP/fresh.po" -d -c "ls" 2>&1)
assert_match "WARNING"    "$probe_pd" "fresh ProDOS image warns with -d"
assert_match "ProDOS image" "$probe_pd" "probe identifies ProDOS correctly"

# Safety: refuse to overwrite.
existing="$TMP/already-there.dsk"
echo "do not clobber" > "$existing"
./wozit -I dos33.dsk -d -c "format $existing" >"$TMP/safety.out" 2>&1
assert_match "already exists" "$(cat "$TMP/safety.out")" "refuses overwrite"
assert_eq "do not clobber" "$(cat "$existing")" "existing file untouched"

t_done
