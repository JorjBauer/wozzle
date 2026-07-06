#!/bin/bash
# Boot block installation: `format ... bootable` and the `bootblocks`
# command (embedded boot code or a donor image). The embedded block is
# the standard ProDOS boot block - verified byte-identical between a
# ProDOS 1.1 ProFile dump and ProDOS 2.4 floppies, so one copy serves
# every ProDOS version, floppy or hard drive.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit" "wozit not built"
skip_if_missing "ProDOS_2_4.po"  "ProDOS 2.4 .po fixture missing"
skip_if_missing "ProDOS_2_4.dsk" "ProDOS 2.4 .dsk fixture missing"
skip_if_missing "dos33.dsk"      "dos33.dsk fixture missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

head -c 512 ProDOS_2_4.po > "$TMP/ref.b0"

# format ... bootable installs the boot block at creation time.
./wozit -p -c "format $TMP/boot.hdv BOOTME 5m bootable" >/dev/null
head -c 512 "$TMP/boot.hdv" > "$TMP/got.b0"
assert_eq "$(hash_file "$TMP/ref.b0")" "$(hash_file "$TMP/got.b0")" \
  "format bootable installs the standard boot block"
dd if="$TMP/boot.hdv" bs=512 skip=1 count=1 2>/dev/null > "$TMP/got.b1"
head -c 512 /dev/zero > "$TMP/zero.b1"
assert_eq "$(hash_file "$TMP/zero.b1")" "$(hash_file "$TMP/got.b1")" \
  "block 1 stays zero"

# bootblocks (no donor) retrofits an existing non-bootable image.
./wozit -p -c "format $TMP/plain.po MYVOL" >/dev/null
out=$(./wozit -p -I "$TMP/plain.po" -c "bootblocks" -c "save" 2>&1)
assert_match "Boot blocks installed" "$out" "bootblocks reports success"
assert_match "PRODOS and a .SYSTEM file" "$out" "bootblocks reminds about system files"
head -c 512 "$TMP/plain.po" > "$TMP/got2.b0"
assert_eq "$(hash_file "$TMP/ref.b0")" "$(hash_file "$TMP/got2.b0")" \
  "embedded boot code matches the reference"

# Donor image in DOS sector order: mapping must still land the right bytes.
./wozit -p -c "format $TMP/donor.hdv DONORTEST 800k" >/dev/null
./wozit -p -I "$TMP/donor.hdv" -c "bootblocks ProDOS_2_4.dsk" -c "save" >/dev/null 2>&1
head -c 512 "$TMP/donor.hdv" > "$TMP/got3.b0"
assert_eq "$(hash_file "$TMP/ref.b0")" "$(hash_file "$TMP/got3.b0")" \
  "a DOS-ordered .dsk donor yields the same boot block"

# Refusals.
out=$(./wozit -p -I "$TMP/boot.hdv" -c "bootblocks dos33.dsk" 2>&1)
assert_match "doesn't look like a ProDOS volume" "$out" "DOS donor image refused"
out=$(./wozit -d -I dos33.dsk -c "bootblocks" 2>&1)
assert_match "only supported for ProDOS" "$out" "bootblocks refused in DOS mode"
out=$(./wozit -d -c "format $TMP/d.dsk 254 bootable" 2>&1)
assert_match "ProDOS-only" "$out" "format bootable refused in DOS mode"

# The bootable image is still a structurally clean volume.
if command -v python3 >/dev/null 2>&1; then
  out=$(python3 tests/prodos-verify.py "$TMP/boot.hdv" 2>&1)
  assert_match "no shared blocks, bitmap consistent" "$out" \
    "bootable image is structurally clean"
fi

t_done
