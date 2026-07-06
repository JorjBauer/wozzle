#!/bin/bash
# wozmosis: image-to-image copies. ProDOS keeps full metadata (type, aux,
# access, dates, versions); DOS 3.3 copies the raw sector stream so every
# file type round-trips with its catalog type, sector count, and locked
# flag. Same-filesystem only; collisions refused unless -f; a failed copy
# leaves the destination image file untouched.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozmosis"    "wozmosis not built"
skip_if_missing "./wozit"       "wozit not built"
skip_if_missing "ProDOS_2_4.po" "ProDOS 2.4 fixture missing"
skip_if_missing "dos33.dsk"     "dos33.dsk fixture missing"
if ! command -v python3 >/dev/null 2>&1; then
  printf "  SKIP: python3 needed for metadata verification\n"
  exit 77
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- ProDOS: full-volume copy with metadata verification ----------------
./wozit -p -c "format $TMP/dst.hdv COPYDEST 5m" >/dev/null
out=$(./wozmosis -p ProDOS_2_4.po "$TMP/dst.hdv" 2>&1)
assert_match "Copied 17 files" "$out" "full-volume ProDOS copy reports 17 files"

py_out=$(python3 - ProDOS_2_4.po "$TMP/dst.hdv" <<'EOF'
import sys
def entries(path):
    data = open(path,'rb').read()
    out = {}
    def blk(n): return data[n*512:(n+1)*512]
    def walk(key, prefix):
        n = key
        while n:
            b = blk(n)
            for i in range(13):
                if n == key and i == 0: continue
                e = b[4+i*0x27:4+i*0x27+0x27]
                st = e[0]>>4; nl = e[0]&0xF
                if not (st and nl): continue
                name = e[1:1+nl].decode()
                p = f"{prefix}/{name}" if prefix else name
                if st == 0xD:
                    walk(e[0x11]|(e[0x12]<<8), p)
                else:
                    out[p] = (e[0x10], e[0x1F]|(e[0x20]<<8), e[0x1E],
                              e[0x18:0x1C].hex(), e[0x21:0x25].hex(),
                              e[0x15]|(e[0x16]<<8)|(e[0x17]<<16),
                              e[0x1C], e[0x1D])
            n = b[2]|(b[3]<<8)
    walk(2, "")
    return out
a, b = entries(sys.argv[1]), entries(sys.argv[2])
if set(a) - set(b): print("MISSING:", sorted(set(a)-set(b))); sys.exit(1)
bad = [p for p in a if a[p] != b[p]]
if bad: print("MISMATCH:", bad[:5]); sys.exit(1)
print(f"IDENTICAL {len(a)}")
EOF
)
assert_match "IDENTICAL 17" "$py_out" \
  "type/aux/access/dates/versions identical on every copied file"

./wozit -p -I ProDOS_2_4.po -c "cpout PRODOS $TMP/p1" >/dev/null 2>&1
./wozit -p -I "$TMP/dst.hdv" -c "cpout PRODOS $TMP/p2" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/p1")" "$(hash_file "$TMP/p2")" "file contents identical"
out=$(python3 tests/prodos-verify.py "$TMP/dst.hdv" 2>&1)
assert_match "no shared blocks, bitmap consistent" "$out" "destination structurally clean"

# ---- ProDOS: selective deep copy creates parent directories -------------
./wozit -p -c "format $TMP/sel.po SEL" >/dev/null
out=$(./wozmosis -p "$TMP/dst.hdv" "$TMP/sel.po" IIE.UTILS/CAT.DOCTOR 2>&1)
assert_match "Copied 1 file" "$out" "selective copy of a nested file"
./wozit -p -I "$TMP/sel.po" -c "cpout IIE.UTILS/CAT.DOCTOR $TMP/u2" >/dev/null 2>&1
./wozit -p -I "$TMP/dst.hdv" -c "cpout IIE.UTILS/CAT.DOCTOR $TMP/u1" >/dev/null 2>&1
[ -s "$TMP/u1" ] && [ -s "$TMP/u2" ]
assert_eq 0 $? "both cpouts produced data"
assert_eq "$(hash_file "$TMP/u1")" "$(hash_file "$TMP/u2")" \
  "nested file round-trips (parent dir auto-created)"

# ---- collisions ----------------------------------------------------------
out=$(./wozmosis -p "$TMP/dst.hdv" "$TMP/sel.po" IIE.UTILS/CAT.DOCTOR 2>&1); rc=$?
assert_eq 1 $rc "collision exits nonzero"
assert_match "use -f to overwrite" "$out" "collision names the -f escape hatch"
out=$(./wozmosis -p -f "$TMP/dst.hdv" "$TMP/sel.po" IIE.UTILS/CAT.DOCTOR 2>&1)
assert_match "Copied 1 file" "$out" "-f overwrites"

# ---- missing selection ---------------------------------------------------
out=$(./wozmosis -p "$TMP/dst.hdv" "$TMP/sel.po" NO.SUCH.FILE 2>&1); rc=$?
assert_eq 1 $rc "unknown source path exits nonzero"
assert_match "not found on the source volume" "$out" "unknown source path reported"

# ---- failure leaves the destination file untouched -----------------------
# Pad the source volume past 140k so a full copy cannot fit on a floppy.
head -c 120000 /dev/urandom > "$TMP/pad.bin"
./wozit -p -I "$TMP/dst.hdv" -c "cpin $TMP/pad.bin PAD" -c "save" >/dev/null 2>&1
./wozit -p -c "format $TMP/tiny.po TINY" >/dev/null
before=$(hash_file "$TMP/tiny.po")
out=$(./wozmosis -p "$TMP/dst.hdv" "$TMP/tiny.po" 2>&1); rc=$?
assert_eq 1 $rc "copy too big for the destination fails"
assert_eq "$before" "$(hash_file "$TMP/tiny.po")" \
  "failed copy leaves the destination image byte-identical"

# ---- DOS 3.3: full-volume copy -------------------------------------------
./wozit -d -c "format $TMP/ddst.dsk 254" >/dev/null
out=$(./wozmosis -d dos33.dsk "$TMP/ddst.dsk" 2>&1)
assert_match "Copied 20 files" "$out" "full-volume DOS copy reports 20 files"
src_cat=$(./wozit -d -I dos33.dsk -c ls 2>/dev/null)
dst_cat=$(./wozit -d -I "$TMP/ddst.dsk" -c ls 2>/dev/null)
assert_eq "$src_cat" "$dst_cat" "DOS catalogs identical (types and sector counts)"
./wozit -d -I dos33.dsk -c "cpout HELLO $TMP/h1" >/dev/null 2>&1
./wozit -d -I "$TMP/ddst.dsk" -c "cpout HELLO $TMP/h2" >/dev/null 2>&1
assert_eq "$(hash_file "$TMP/h1")" "$(hash_file "$TMP/h2")" "DOS file contents identical"

# ---- mode enforcement -----------------------------------------------------
out=$(./wozmosis -p dos33.dsk "$TMP/dst.hdv" 2>&1); rc=$?
assert_eq 1 $rc "DOS image in -p mode exits nonzero"
assert_match "doesn't look like a ProDOS volume" "$out" "filesystem mismatch reported"
out=$(./wozmosis dos33.dsk "$TMP/ddst.dsk" 2>&1); rc=$?
assert_eq 1 $rc "missing -d/-p exits nonzero"
assert_match "Cross-filesystem copies aren't supported" "$out" "mode requirement explained"

t_done
