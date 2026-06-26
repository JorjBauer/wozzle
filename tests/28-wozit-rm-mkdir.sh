#!/bin/bash
# rm / rmdir / mkdir: file deletion, directory create+remove, and the
# storage reclamation that goes with them.
#
# Covers ProDOS rm (seedling + sapling block reclamation), ProDOS
# mkdir/rmdir (block reclamation + empty-only refusal + rm/rmdir
# cross-guards), and DOS 3.3 rm (sector reclamation - which also guards the
# flushFreeSectorList sector/track byte-indexing fix) plus DOS rmdir being
# unsupported.
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozit"   "wozit not built"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing (used as format host)"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Deterministic payloads: a seedling (<=512 B) and a sapling (>512 B).
yes X        | head -c 200  > "$TMP/small.bin"
yes ABCDEFGH | head -c 5000 > "$TMP/big.bin"

prodos_free() { ./wozit -I "$1" -p -c info 2>/dev/null | grep 'Blocks free' | grep -oE '[0-9]+'; }
dos_free()    { ./wozit -I "$1" -d -c info 2>/dev/null | grep '^Track' | tr -cd 'f' | wc -c | tr -d ' '; }

# A blank ProDOS volume to work in.
./wozit -I dos33.dsk -p -c "format $TMP/v.po WOZRMTEST" >/dev/null 2>&1
skip_if_missing "$TMP/v.po" "format did not produce an image"
blank=$(prodos_free "$TMP/v.po")

# --- ProDOS rm reclaims a seedling and a sapling ----------------------
cp "$TMP/v.po" "$TMP/a.po"
./wozit -I "$TMP/a.po" -p \
  -c "cpin $TMP/big.bin BIG" -c "cpin $TMP/small.bin SMALL" -c "save $TMP/a.po" >/dev/null 2>&1
ls_a=$(./wozit -I "$TMP/a.po" -p -c "ls" 2>&1)
assert_match "BIG"   "$ls_a" "cpin created the sapling"
assert_match "SMALL" "$ls_a" "cpin created the seedling"

rm_out=$(./wozit -I "$TMP/a.po" -p -c "rm BIG" -c "rm SMALL" -c "save $TMP/a.po" 2>&1)
assert_match "Removed 'BIG'"   "$rm_out" "rm reports removing the sapling"
assert_match "Removed 'SMALL'" "$rm_out" "rm reports removing the seedling"
ls_a2=$(./wozit -I "$TMP/a.po" -p -c "ls" 2>&1)
assert_no_match "BIG"   "$ls_a2" "sapling is gone after rm"
assert_no_match "SMALL" "$ls_a2" "seedling is gone after rm"
assert_eq "$blank" "$(prodos_free "$TMP/a.po")" "rm reclaims every block (back to blank)"

# rm of a missing file is a clean error, not a crash.
miss=$(./wozit -I "$TMP/a.po" -p -c "rm GHOST" 2>&1)
assert_match "not found" "$miss" "rm of a missing file reports not found"

# --- ProDOS mkdir / rmdir round-trip ----------------------------------
cp "$TMP/v.po" "$TMP/b.po"
mk=$(./wozit -I "$TMP/b.po" -p -c "mkdir PROJECTS" -c "save $TMP/b.po" 2>&1)
assert_match "Created directory 'PROJECTS'" "$mk" "mkdir reports success"
ls_b=$(./wozit -I "$TMP/b.po" -p -c "ls" 2>&1)
assert_regex   "PROJECTS +DIR" "$ls_b" "new entry lists as a directory"
assert_no_match "WARNING"      "$ls_b" "new directory header is valid (no warning)"
expect_after_mkdir=$((blank - 1))
assert_eq "$expect_after_mkdir" "$(prodos_free "$TMP/b.po")" "mkdir consumes exactly one block"

dup=$(./wozit -I "$TMP/b.po" -p -c "mkdir PROJECTS" 2>&1)
assert_match "already exists" "$dup" "mkdir refuses a duplicate name"

rmd=$(./wozit -I "$TMP/b.po" -p -c "rmdir PROJECTS" -c "save $TMP/b.po" 2>&1)
assert_match "Removed directory 'PROJECTS'" "$rmd" "rmdir reports success"
assert_no_match "PROJECTS" "$(./wozit -I "$TMP/b.po" -p -c "ls" 2>&1)" "directory is gone after rmdir"
assert_eq "$blank" "$(prodos_free "$TMP/b.po")" "rmdir reclaims the directory block"

# --- rm vs rmdir cross-guards -----------------------------------------
cp "$TMP/v.po" "$TMP/c.po"
./wozit -I "$TMP/c.po" -p \
  -c "mkdir ADIR" -c "cpin $TMP/small.bin AFILE" -c "save $TMP/c.po" >/dev/null 2>&1
g1=$(./wozit -I "$TMP/c.po" -p -c "rm ADIR" 2>&1)
assert_match "use rmdir" "$g1" "rm of a directory points at rmdir"
g2=$(./wozit -I "$TMP/c.po" -p -c "rmdir AFILE" 2>&1)
assert_match "use rm" "$g2" "rmdir of a file points at rm"

# --- DOS 3.3 rm reclaims sectors; rmdir is unsupported ----------------
cp dos33.dsk "$TMP/d.dsk"
d_blank=$(dos_free "$TMP/d.dsk")
./wozit -I "$TMP/d.dsk" -d -c "cpin $TMP/small.bin DGAME" -c "save $TMP/d.dsk" >/dev/null 2>&1
# 200-byte BIN => 1 data sector + 1 T/S list sector = 2 sectors used.
assert_eq "$((d_blank - 2))" "$(dos_free "$TMP/d.dsk")" "DOS cpin consumes 2 sectors"
drm=$(./wozit -I "$TMP/d.dsk" -d -c "rm DGAME" -c "save $TMP/d.dsk" 2>&1)
assert_match "Removed 'DGAME'" "$drm" "DOS rm reports success"
assert_no_match "DGAME" "$(./wozit -I "$TMP/d.dsk" -d -c "ls" 2>&1)" "DOS file gone after rm (no ghost entry)"
assert_eq "$d_blank" "$(dos_free "$TMP/d.dsk")" "DOS rm reclaims all sectors"

dos_rmdir=$(./wozit -I "$TMP/d.dsk" -d -c "rmdir WHATEVER" 2>&1)
assert_match "not supported" "$dos_rmdir" "rmdir is unsupported under DOS 3.3"
dos_mkdir=$(./wozit -I "$TMP/d.dsk" -d -c "mkdir WHATEVER" 2>&1)
assert_match "not supported" "$dos_mkdir" "mkdir is unsupported under DOS 3.3"

# --- Bonus: rmdir refuses a non-empty directory (populated fixture) ----
for fixture in ProDOS_2_4.po 1.po; do
  [ -e "$fixture" ] || continue
  cp "$fixture" "$TMP/pop.po"
  # IIE.UTILS is a populated subdirectory on these images.
  ne=$(./wozit -I "$TMP/pop.po" -p -c "rmdir IIE.UTILS" 2>&1)
  assert_match "not empty" "$ne" "rmdir refuses a non-empty directory ($fixture)"
  assert_match "IIE.UTILS" "$(./wozit -I "$TMP/pop.po" -p -c "ls" 2>&1)" \
    "refused directory is left intact ($fixture)"
  break
done

t_done
