#!/bin/bash
# wozfuse: mount images read-only and check what shows up against wozit.
# Skips if wozfuse wasn't built (no FUSE 3) or this machine can't mount
# (no macFUSE approval, no /dev/fuse in a container, ...).
. "$(dirname "$0")/lib.sh"

skip_if_missing "./wozfuse" "wozfuse not built (needs FUSE 3)"
skip_if_missing "ProDOS_2_4.po" "ProDOS_2_4.po fixture missing"
skip_if_missing "dos33.dsk" "dos33.dsk fixture missing"

# The real path, since that's what mount reports (on a Mac, /var is
# really /private/var)
TMP=$(cd "$(mktemp -d)" && pwd -P)
MNT="$TMP/mnt"
mkdir "$MNT"
pid=""

unmount() {
  if [ "$(uname)" = "Darwin" ]; then
    umount "$MNT" 2>/dev/null
  else
    fusermount3 -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null
  fi
  [ -n "$pid" ] && wait "$pid" 2>/dev/null
  pid=""
}
trap 'unmount; rm -rf "$TMP"' EXIT

is_mounted() {
  mount | grep -qF " on $MNT "
}

# mount_image <image> [wozfuse flags...]: returns 1 if it never mounted
mount_image() {
  local img="$1"; shift
  ./wozfuse "$@" "$img" "$MNT" >"$TMP/fuse.log" 2>&1 &
  pid=$!
  for _ in $(seq 1 50); do
    is_mounted && return 0
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.2
  done
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  pid=""
  return 1
}

# xattr_value <attribute> <file>
xattr_value() {
  if command -v getfattr >/dev/null 2>&1; then
    getfattr --only-values -n "$1" "$2" 2>/dev/null
  else
    xattr -p "$1" "$2" 2>/dev/null
  fi
}

if ! mount_image ProDOS_2_4.po; then
  printf "  SKIP: can't mount here (%s)\n" "$(tr '\n' ' ' < "$TMP/fuse.log")"
  exit 77
fi

# ---- ProDOS ----
ls_out=$(ls "$MNT")
assert_match "PRODOS" "$ls_out" "the root lists PRODOS"
assert_match "BASIC.SYSTEM" "$ls_out" "and BASIC.SYSTEM"
assert_no_match "_CATALOG.TXT" "$ls_out" "no catalog without -c"

same=0; total=0
for f in PRODOS BASIC.SYSTEM QUIT.SYSTEM; do
  total=$((total + 1))
  ./wozit -p -I ProDOS_2_4.po -c "cpout $f $TMP/$f.out" >/dev/null 2>&1
  cmp -s "$MNT/$f" "$TMP/$f.out" && same=$((same + 1))
done
assert_eq "$total" "$same" "files read the same as wozit's cpout"

assert_eq "FF" "$(xattr_value user.apple2.filetype "$MNT/BASIC.SYSTEM")" \
          "BASIC.SYSTEM's file type is SYS"
assert_eq "2000" "$(xattr_value user.apple2.auxtype "$MNT/BASIC.SYSTEM")" \
          "and its aux type is \$2000"

sub=$(ls "$MNT/IIE.UTILS" 2>&1)
assert_no_match "No such" "$sub" "a subdirectory lists"
touch "$MNT/NEWFILE" 2>/dev/null
assert_eq 1 "$?" "the mount is read-only"
unmount

# ---- the generated catalog ----
mount_image ProDOS_2_4.po -c
cat_out=$(cat "$MNT/_CATALOG.TXT" 2>&1)
assert_match "BASIC.SYSTEM" "$cat_out" "-c generates a catalog"
assert_match "SYS     \$2000" "$cat_out" "with the types in it"
assert_match "NAME" "$(cat "$MNT/IIE.UTILS/_CATALOG.TXT" 2>&1)" \
             "subdirectories get one too"
unmount

# ---- DOS 3.3, and a name a host can't hold ----
cp dos33.dsk "$TMP/d.dsk"
printf 'HELLO\r' > "$TMP/note.txt"
./wozit -d -I "$TMP/d.dsk" -c "cpin $TMP/note.txt A/B 04" -c save >/dev/null 2>&1
mount_image "$TMP/d.dsk" -d
assert_match "A%2FB" "$(ls "$MNT")" "a '/' in a DOS name is escaped"
assert_eq "$(printf 'HELLO\r')" "$(cat "$MNT/A%2FB")" "and the file reads"
./wozit -d -I dos33.dsk -c "cpout FID $TMP/fid.out" >/dev/null 2>&1
cmp -s "$MNT/FID" "$TMP/fid.out"
assert_eq 0 "$?" "a DOS B file reads as its contents, as cpout copies it"
assert_eq "06" "$(xattr_value user.apple2.filetype "$MNT/FID")" \
          "FID's type is B (BIN)"
unmount

t_done
