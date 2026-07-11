#!/bin/bash
# UCSD p-System / Apple Pascal mode (-P): directory listing, extraction,
# cpin round-trip, rm, and the two defragment commands (krunch /
# krunchafter). Uses whatever Pascal fixture is present; SKIPs if none is.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"

# Pascal disk fixture. Untracked (like the other test disks), so this SKIPs
# cleanly on a fresh checkout that doesn't have it.
PASC="UCSD Pascal 1.2_1.DSK"
skip_if_missing "$PASC" "Pascal disk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ext="${PASC##*.}"
IMG="$TMP/work.$ext"
cp "$PASC" "$IMG"

# Pull "Blocks free" and "Largest free contiguous region" out of `info`.
free_blocks() { ./wozit -P -I "$1" -c info 2>/dev/null | awk '/Blocks free:/ {print $3}'; }
largest_gap() { ./wozit -P -I "$1" -c info 2>/dev/null | awk '/Largest free contiguous/ {print $5}'; }
# Block number that a krunch reports the free space landing at.
hole_at() { grep -oE 'contiguous at block [0-9]+' <<<"$1" | awk '{print $4}'; }

# --- ls ----------------------------------------------------------------
ls_out=$(./wozit -P -I "$IMG" -c ls 2>&1)
assert_match "SYSTEM.APPLE"  "$ls_out" "ls lists SYSTEM.APPLE"
assert_match "SYSTEM.PASCAL" "$ls_out" "ls lists SYSTEM.PASCAL"
assert_match ".CODE"         "$ls_out" "ls shows Pascal file kinds"

# --- wrong-mode probe warning -----------------------------------------
warn=$(./wozit -d -I "$IMG" -c info 2>&1)
assert_match "looks like a Pascal image" "$warn" "-d on a Pascal disk warns"

# --- cpout is deterministic and non-empty ------------------------------
./wozit -P -I "$IMG" -c "cpout SYSTEM.PASCAL $TMP/a.bin" >/dev/null 2>&1
./wozit -P -I "$IMG" -c "cpout SYSTEM.PASCAL $TMP/b.bin" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/a.bin")" "$(hash_file "$TMP/b.bin")" "cpout is deterministic"
[ -s "$TMP/a.bin" ]; assert_eq 0 $? "cpout produced a non-empty file"

# --- cpin round-trip ---------------------------------------------------
head -c 2000 crc32.c > "$TMP/payload.bin"
ph=$(hash_file "$TMP/payload.bin")
cp "$IMG" "$TMP/rt.$ext"
./wozit -P -I "$TMP/rt.$ext" -c "cpin $TMP/payload.bin MYFILE.DATA" -c save >/dev/null 2>&1
rt_ls=$(./wozit -P -I "$TMP/rt.$ext" -c ls 2>&1)
assert_match "MYFILE.DATA" "$rt_ls" "cpin file survives save+reload"
./wozit -P -I "$TMP/rt.$ext" -c "cpout MYFILE.DATA $TMP/back.bin" >/dev/null 2>&1
assert_eq "$ph" "$(hash_file "$TMP/back.bin")" "cpin/cpout round-trips byte-for-byte"

# --- rm ----------------------------------------------------------------
./wozit -P -I "$TMP/rt.$ext" -c "rm MYFILE.DATA" -c save >/dev/null 2>&1
after_rm=$(./wozit -P -I "$TMP/rt.$ext" -c ls 2>&1)
assert_no_match "MYFILE.DATA" "$after_rm" "rm deletes the file"

# --- krunch: fragment, then consolidate free space at the end ----------
FRAG="$TMP/frag.$ext"
cp "$IMG" "$FRAG"
ed_pre=$(./wozit -P -I "$FRAG" -c "cpout SYSTEM.EDITOR $TMP/ed.pre" >/dev/null 2>&1; hash_file "$TMP/ed.pre")
./wozit -P -I "$FRAG" -c "rm SYSTEM.PASCAL" -c "rm SYSTEM.FILER" -c save >/dev/null 2>&1
# Fragmented: the largest single gap is smaller than the total free space.
fg_free=$(free_blocks "$FRAG"); fg_large=$(largest_gap "$FRAG")
[ "$fg_large" -lt "$fg_free" ]; assert_eq 0 $? "fragmented disk: largest gap ($fg_large) < total free ($fg_free)"

kout=$(./wozit -P -I "$FRAG" -c krunch -c save 2>&1)
assert_match "contiguous" "$kout" "krunch reports free space consolidated"
# After krunch the largest gap equals the total free space (fully packed).
kr_free=$(free_blocks "$FRAG"); kr_large=$(largest_gap "$FRAG")
assert_eq "$kr_free" "$kr_large" "krunch: largest gap == total free (defragmented)"
# Data survived the slide, and every kept file is still listed.
./wozit -P -I "$FRAG" -c "cpout SYSTEM.EDITOR $TMP/ed.post" >/dev/null 2>&1
assert_eq "$ed_pre" "$(hash_file "$TMP/ed.post")" "krunch preserves moved-file data"
kls=$(./wozit -P -I "$FRAG" -c ls 2>&1)
for keep in SYSTEM.APPLE SYSTEM.EDITOR SYSTEM.LIBRARY SYSTEM.SYNTAX; do
  assert_match "$keep" "$kls" "krunch keeps $keep"
done

# --- krunchafter: free space lands after the named file ---------------
FRAG2="$TMP/frag2.$ext"
cp "$IMG" "$FRAG2"
./wozit -P -I "$FRAG2" -c "rm SYSTEM.PASCAL" -c "rm SYSTEM.FILER" -c save >/dev/null 2>&1
kaout=$(./wozit -P -I "$FRAG2" -c "krunchafter SYSTEM.APPLE" -c save 2>&1)
assert_match "contiguous" "$kaout" "krunchafter reports success"
ka_free=$(free_blocks "$FRAG2"); ka_large=$(largest_gap "$FRAG2")
assert_eq "$ka_free" "$ka_large" "krunchafter: free space is contiguous"
# The hole lands earlier for krunchafter APPLE (right after APPLE) than for
# a plain krunch (after every file), so its start block must be lower.
kr_at=$(hole_at "$kout"); ka_at=$(hole_at "$kaout")
[ -n "$kr_at" ] && [ -n "$ka_at" ] && [ "$ka_at" -lt "$kr_at" ]
assert_eq 0 $? "krunchafter hole ($ka_at) precedes krunch hole ($kr_at)"
# Data integrity for a file that slid upward toward the end.
./wozit -P -I "$FRAG2" -c "cpout SYSTEM.EDITOR $TMP/ed.up" >/dev/null 2>&1
assert_eq "$ed_pre" "$(hash_file "$TMP/ed.up")" "krunchafter preserves upward-moved data"

t_done
