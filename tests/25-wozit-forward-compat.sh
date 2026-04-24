#!/bin/bash
# Forward-compatibility: the WOZ spec says newer INFO versions must be
# accepted with a >= check, and unknown chunks must be skipped rather
# than aborting the load. Synthesize a patched WOZ file and confirm
# wozit doesn't choke on either.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozzle"  "wozzle not built"
skip_if_missing "./wozit"   "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Produce a baseline WOZ 2 from dos33.dsk.
./wozzle -i dos33.dsk -o "$TMP/base.woz" >/dev/null 2>&1
assert_file_size "$TMP/base.woz" "$(wc -c < "$TMP/base.woz" | tr -d ' ')" "wozzle produced a WOZ"

# --- (1) INFO version bump to 3. Byte 20 is the INFO version field. ---
cp "$TMP/base.woz" "$TMP/v3.woz"
printf '\x03' | dd of="$TMP/v3.woz" bs=1 seek=20 count=1 conv=notrunc >/dev/null 2>&1

out=$(./wozit -I "$TMP/v3.woz" -d -c "info" 2>&1)
assert_no_match "Incorrect version" "$out" "INFO v3 no longer rejected"
assert_no_match "aborting"          "$out" "INFO v3 load does not abort"
assert_match    "Disk type: 5.25"   "$out" "INFO v3 load produces expected info"

# --- (2) Unknown chunk appended at end of file. 'FAKE' + size 8 + 8 zeros. ---
cp "$TMP/base.woz" "$TMP/unk.woz"
# Chunk ID 'FAKE' (4 bytes), chunk-size-uint32-LE of 8, then 8 data bytes.
printf 'FAKE\x08\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07\x08' >> "$TMP/unk.woz"

out=$(./wozit -I "$TMP/unk.woz" -d -c "info" 2>&1)
assert_no_match "Unknown chunk type" "$out" "unknown chunk not reported as error"
assert_no_match "Chunk parsing"      "$out" "unknown chunk does not fail parsing"
assert_match    "Disk type: 5.25"    "$out" "image with unknown trailing chunk still loads"

# Verbose mode should now mention the skip.
vout=$(./wozit -I "$TMP/unk.woz" -d -v -c "info" 2>&1)
assert_match "Skipping unknown chunk" "$vout" "verbose trace mentions the skip"

# --- (3) Combined: v3 header + trailing unknown chunk (rwts18-era WOZ 2.1 shape) ---
cp "$TMP/base.woz" "$TMP/both.woz"
printf '\x03' | dd of="$TMP/both.woz" bs=1 seek=20 count=1 conv=notrunc >/dev/null 2>&1
printf 'FLUX\x00\x00\x00\x00' >> "$TMP/both.woz"
out=$(./wozit -I "$TMP/both.woz" -d -c "ls" 2>&1)
assert_no_match "aborting" "$out" "v3 + unknown chunk combo loads"
assert_match    "HELLO"    "$out" "catalog still readable through the combo"

t_done
