#!/bin/bash
# wozmosis -D: land the whole copy under a directory on the destination,
# creating it (and any missing parents) as needed. The main reason it
# exists: a ProDOS volume directory is a fixed 51 entries and cannot grow,
# so a copy that overflows the root still fits under a subdirectory.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozmosis"    "wozmosis not built"
skip_if_missing "./wozit"       "wozit not built"
skip_if_missing "ProDOS_2_4.po" "ProDOS 2.4 fixture missing"
skip_if_missing "dos33.dsk"     "dos33.dsk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# The root listing is the first blank-line-delimited block of `ls` output,
# after the volume-name header line.
root_listing() {
  ./wozit -p -I "$1" -c ls 2>/dev/null |
    awk 'BEGIN{b=0} NF==0 && !started {next} {started=1} NF==0 {b++; next} b>=2{exit} b==1{print}'
}

# ---- the copy lands under -D's directory, not in the root ---------------
./wozit -p -c "format $TMP/dst.hdv COPYDEST 5m" >/dev/null
out=$(./wozmosis -p -D STASH ProDOS_2_4.po "$TMP/dst.hdv" 2>&1)
assert_match "Copied 17 files" "$out" "-D copies every file"
assert_match "Created directory 'STASH'" "$out" "-D creates its directory"

root=$(root_listing "$TMP/dst.hdv")
assert_match "STASH" "$root" "STASH appears in the destination root"
assert_no_match "PRODOS " "$root" "source files did NOT land in the root"

# Contents match the source byte for byte.
./wozit -p -I ProDOS_2_4.po   -c "cpout PRODOS $TMP/s.bin" >/dev/null 2>&1
./wozit -p -I "$TMP/dst.hdv"  -c "cpout STASH/PRODOS $TMP/d.bin" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/s.bin")" "$(hash_file "$TMP/d.bin")" \
  "file copied under -D is byte-identical"
# Control: the detector must be able to see a difference at all.
./wozit -p -I ProDOS_2_4.po -c "cpout BASIC.SYSTEM $TMP/o.bin" >/dev/null 2>&1
if [ "$(hash_file "$TMP/s.bin")" = "$(hash_file "$TMP/o.bin")" ]; then
  printf "  FAIL: control - two different files hashed the same\n" >&2
  t_fail=$((t_fail + 1))
else
  printf "  PASS: control - different files hash differently\n"
  t_pass=$((t_pass + 1))
fi

# Source subdirectory structure is preserved beneath the new prefix.
sub=$(./wozit -p -I "$TMP/dst.hdv" -c ls 2>/dev/null)
assert_match "STASH" "$sub" "tree still lists after the relocated copy"

# ---- nested -D creates intermediate directories -------------------------
out=$(./wozmosis -p -D A/B/C ProDOS_2_4.po "$TMP/dst.hdv" 2>&1)
assert_match "Copied 17 files" "$out" "nested -D copies every file"
./wozit -p -I "$TMP/dst.hdv" -c "cpout A/B/C/PRODOS $TMP/n.bin" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/s.bin")" "$(hash_file "$TMP/n.bin")" \
  "nested -D creates intermediate directories and copies through them"

# ---- -D is what gets past a nearly full volume directory ----------------
# A ProDOS volume directory holds a fixed 51 entries. Fill 50 of them, so
# a 17-file root copy overflows but a single new subdirectory still fits:
# the root copy must fail and the same copy under -D must succeed.
./wozit -p -c "format $TMP/full.hdv FULLVOL 5m" >/dev/null
printf 'x' > "$TMP/tiny.bin"
cmds=""
for i in $(seq 1 50); do cmds="$cmds -c \"cpin $TMP/tiny.bin F$i\""; done
eval ./wozit -p -I "$TMP/full.hdv" $cmds -c \"save\" >/dev/null 2>&1
count=$(root_listing "$TMP/full.hdv" | grep -c 'BIN')
assert_eq 50 "$count" "root directory filled to 50 of its 51 entries"

before=$(hash_file "$TMP/full.hdv")
out=$(./wozmosis -p ProDOS_2_4.po "$TMP/full.hdv" 2>&1); rc=$?
assert_eq 1 $rc "copy into a nearly full root fails"
assert_match "volume directory is full" "$out" "the full root is explained"
assert_eq "$before" "$(hash_file "$TMP/full.hdv")" "failed copy leaves the image untouched"

out=$(./wozmosis -p -D STASH ProDOS_2_4.po "$TMP/full.hdv" 2>&1); rc=$?
assert_eq 0 $rc "the same copy under -D succeeds"
assert_match "Copied 17 files" "$out" "-D copy reports every file"

# The last free root slot is now STASH, so even -D has nowhere left to go.
# That limit is real, and it has to be reported rather than half-copied.
out=$(./wozmosis -p -D OTHER ProDOS_2_4.po "$TMP/full.hdv" 2>&1); rc=$?
assert_eq 1 $rc "-D fails once the root has no free slot at all"
assert_match "volume directory is full" "$out" "a truly full root is still explained"

# ---- -D is refused where there are no directories -----------------------
./wozit -d -c "format $TMP/ddst.dsk 254" >/dev/null
out=$(./wozmosis -d -D STASH dos33.dsk "$TMP/ddst.dsk" 2>&1); rc=$?
assert_eq 1 $rc "-D with -d exits nonzero"
assert_match "-D needs -p" "$out" "-D refusal names the reason"

# ---- an unusable -D name is caught before anything is written -----------
before=$(hash_file "$TMP/dst.hdv")
out=$(./wozmosis -p -D THISNAMEISWAYTOOLONG ProDOS_2_4.po "$TMP/dst.hdv" 2>&1); rc=$?
assert_eq 1 $rc "an over-long -D component exits nonzero"
assert_eq "$before" "$(hash_file "$TMP/dst.hdv")" "rejected -D leaves the image untouched"

t_done
