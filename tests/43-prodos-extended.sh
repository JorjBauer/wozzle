#!/bin/bash
# ProDOS storage type 5: the GS/OS extended (forked) file. Its key block is
# an extended key block holding a mini-entry per fork, and each fork is an
# ordinary seedling/sapling/tree underneath. Covers building one with cpin
# (from AppleSingle), reading each fork back out with cpout, listing and
# inspecting it, copying it image-to-image with wozmosis, and freeing both
# forks on rm.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"    "wozit not built"
skip_if_missing "./wozmosis" "wozmosis not built"
if ! command -v python3 >/dev/null 2>&1; then
  printf "  SKIP: python3 needed to build the AppleSingle fixture\n"
  exit 77
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- fixtures -------------------------------------------------------------
# A data fork that needs a sapling (3 blocks) and a resource fork that fits
# in a seedling, wrapped in AppleSingle with ProDOS type/aux metadata.
python3 - "$TMP" <<'EOF'
import struct, sys, os
tmp = sys.argv[1]
data = bytes((i * 7 + 3) & 0xFF for i in range(1300))
rsrc = bytes((i * 11 + 5) & 0xFF for i in range(400))
open(os.path.join(tmp, "data.bin"), "wb").write(data)
open(os.path.join(tmp, "rsrc.bin"), "wb").write(rsrc)

# AppleSingle: header, then prodosFileInfo / data fork / resource fork.
info = struct.pack(">HHI", 0xC3, 0xB3, 0x1234)  # access, filetype, auxtype
hdr = struct.pack(">II16sH", 0x00051600, 0x00020000, b"\0" * 16, 3)
off = len(hdr) + 3 * 12
entries = b""
for eid, payload in ((11, info), (1, data), (2, rsrc)):
    entries += struct.pack(">III", eid, off, len(payload))
    off += len(payload)
open(os.path.join(tmp, "forked.as"), "wb").write(
    hdr + entries + info + data + rsrc)
EOF

./wozit -p -c "format $TMP/src.po EXTVOL 280" >/dev/null

# ---- cpin builds an extended file -----------------------------------------
./wozit -I "$TMP/src.po" -p -c "cpin $TMP/forked.as FORKED" -c "save" \
  >/dev/null 2>&1
out=$(./wozit -I "$TMP/src.po" -p -c "inspect FORKED" 2>&1)
assert_match "extended (GS/OS forked file)" "$out" "cpin made an extended file"
assert_match "data fork: sapling, 1300 bytes in 4 blocks" "$out" \
  "data fork stored as a sapling"
assert_match "resource fork: seedling, 400 bytes in 1 block" "$out" \
  "resource fork stored as a seedling"

# The directory entry describes the extended key block: 512 bytes, and
# blocks for both forks plus the key block itself (4 + 1 + 1).
ls_out=$(./wozit -I "$TMP/src.po" -p -c "ls" 2>&1)
assert_match "FORKED+" "$ls_out" "ls flags the forked file with a +"
assert_match "6  " "$ls_out" "listed block count covers both forks"

# ---- cpout reads each fork back -------------------------------------------
./wozit -I "$TMP/src.po" -p -c "cpout FORKED $TMP/out.data" >/dev/null 2>&1
./wozit -I "$TMP/src.po" -p -c "cpout -r FORKED $TMP/out.rsrc" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/data.bin")" "$(hash_file "$TMP/out.data")" \
  "cpout returns the data fork byte for byte"
assert_eq "$(hash_file "$TMP/rsrc.bin")" "$(hash_file "$TMP/out.rsrc")" \
  "cpout -r returns the resource fork byte for byte"

out=$(./wozit -I "$TMP/src.po" -p -c "cpin $TMP/data.bin PLAIN 06 2000" \
      -c "save" -c "cpout -r PLAIN $TMP/nope" 2>&1)
assert_match "has no resource fork" "$out" \
  "cpout -r refuses a file that isn't extended"

# ---- wozmosis copies both forks -------------------------------------------
./wozit -p -c "format $TMP/dst.po EXTDEST 280" >/dev/null
out=$(./wozmosis -p "$TMP/src.po" "$TMP/dst.po" 2>&1)
assert_no_match "Unimplemented" "$out" "copy doesn't skip the extended file"
out=$(./wozit -I "$TMP/dst.po" -p -c "inspect FORKED" 2>&1)
assert_match "data fork: sapling, 1300 bytes" "$out" "copy kept the data fork"
assert_match "resource fork: seedling, 400 bytes" "$out" \
  "copy kept the resource fork"
./wozit -I "$TMP/dst.po" -p -c "cpout FORKED $TMP/c.data" >/dev/null 2>&1
./wozit -I "$TMP/dst.po" -p -c "cpout -r FORKED $TMP/c.rsrc" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/data.bin")" "$(hash_file "$TMP/c.data")" \
  "copied data fork is identical"
assert_eq "$(hash_file "$TMP/rsrc.bin")" "$(hash_file "$TMP/c.rsrc")" \
  "copied resource fork is identical"

# File type and aux type survive the trip (AppleSingle said $B3/$1234).
py_out=$(python3 - "$TMP/dst.po" <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read()
for i in range(13):
    e = d[2*512+4+i*0x27:2*512+4+(i+1)*0x27]
    if (e[0] & 0xF) and e[1:1+(e[0] & 0xF)] == b'FORKED':
        print("st=%d type=%02X aux=%04X" %
              (e[0] >> 4, e[0x10], e[0x20] * 256 + e[0x1F]))
EOF
)
assert_eq "st=5 type=B3 aux=1234" "$py_out" \
  "storage type, file type and aux type all preserved"

# ---- rm frees both forks and the extended key block ------------------------
before=$(./wozit -I "$TMP/dst.po" -p -c "info" 2>/dev/null | \
         grep "Blocks free:" | tr -dc 0-9)
./wozit -I "$TMP/dst.po" -p -c "rm FORKED" -c "save" >/dev/null 2>&1
after=$(./wozit -I "$TMP/dst.po" -p -c "info" 2>/dev/null | \
        grep "Blocks free:" | tr -dc 0-9)
assert_eq 6 $((after - before)) "rm frees both forks and the key block"

t_done
