// wozmosis: copy files between two disk images of the same filesystem,
// preserving metadata - the osmosis of the woz tool family.
//
//   wozmosis { -d | -p } [-f] [-v] [-D dir] <source image> <dest image> [path ...]
//
// With no paths, the entire source volume is copied into the destination
// root, directory structure intact. With paths, just those files or
// directory subtrees are copied (to the same paths on the destination,
// creating intermediate directories as needed).
//
// -D relocates the whole copy under a directory on the destination, which
// is created if it doesn't exist: the source's FOO/BAR lands at DIR/FOO/BAR.
// ProDOS only, since neither DOS 3.3 nor Pascal has directories. Besides
// being how you file a disk's worth of software away in one place, it is
// the way past a full ProDOS volume directory: the root holds a fixed 51
// entries and cannot grow, while a subdirectory grows as needed.
//
// ProDOS files keep their file type, aux type, access flags, creation and
// modification dates, and version bytes; a GS/OS extended file keeps both
// of its forks and its Finder information. DOS 3.3 files keep their type
// and locked flag. Existing destination files are refused unless -f is given.
// The destination image file is rewritten only if every copy succeeds.
//
// Known limits: ProDOS sparse files are materialized (holes become real
// zero blocks), and a directory created here gets zeroed dates (ProDOS
// mkdir doesn't take them yet). DOS-to-ProDOS (or vice versa) is refused.

#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

#include <string>
#include <vector>

#include "crc32.h"
#include "wozops.h"

using namespace std;

static void usage(const char *name)
{
  printf("Usage: %s { -d | -p | -P } [-f] [-v] [-D <dir>] <source image> <dest image> [path ...]\n\n", name);
  printf("  -d    both images are DOS 3.3\n");
  printf("  -p    both images are ProDOS\n");
  printf("  -P    both images are UCSD p-System / Apple Pascal\n");
  printf("  -f    overwrite files that already exist on the destination\n");
  printf("  -v    list every file as it is copied\n");
  printf("  -D    put the copy under <dir> on the destination, creating it\n");
  printf("        if needed (ProDOS only)\n\n");
  printf("  With no paths, copies the whole source volume. A path may name\n");
  printf("  a file or (ProDOS) a directory subtree; it is copied to the\n");
  printf("  same path on the destination, or under -D's directory.\n");
}

static void showProgress(const char *path, uint32_t dataBytes,
                         uint32_t resourceBytes, bool extended, bool replaced,
                         void *ctx)
{
  (void)ctx;
  if (replaced) printf("(replacing %s)\n", path);
  if (extended)
    printf("  %s (%u bytes data + %u bytes resource)\n", path, dataBytes,
           resourceBytes);
  else
    printf("  %s (%u bytes)\n", path, dataBytes);
}

int main(int argc, char *argv[])
{
  preload_crc();

  bool force = false;
  bool verbose = false;
  string destPrefix; // -D: destination directory every copy lands under
  int mode = 0; // 'd', 'p', or 'P'
  int c;
  while ((c = getopt(argc, argv, "dpPfvD:h?")) != -1) {
    switch (c) {
    case 'd': mode = 'd'; break;
    case 'p': mode = 'p'; break;
    case 'P': mode = 'P'; break;
    case 'f': force = true; break;
    case 'v': verbose = true; break;
    case 'D': destPrefix = optarg; break;
    case 'h':
    case '?':
      usage(argv[0]);
      return 1;
    }
  }

  if (!mode) {
    printf("ERROR: specify -d (DOS 3.3), -p (ProDOS), or -P (Pascal). "
           "Cross-filesystem copies aren't supported.\n");
    usage(argv[0]);
    return 1;
  }
  if (argc - optind < 2) {
    usage(argv[0]);
    return 1;
  }
  wozFsType fsType = (mode == 'd') ? WOZ_FS_DOS33
                   : (mode == 'P') ? WOZ_FS_PASCAL : WOZ_FS_PRODOS;

  // Check -D before opening anything
  if (!destPrefix.empty()) {
    if (mode != 'p') {
      printf("ERROR: -D needs -p; neither DOS 3.3 nor Pascal has directories\n");
      return 1;
    }
    if (!wozNormalizeDestDir(&destPrefix))
      return 1;
  }
  const char *srcPath = argv[optind];
  const char *dstPath = argv[optind + 1];
  if (!strcmp(srcPath, dstPath)) {
    printf("ERROR: source and destination are the same file\n");
    return 1;
  }

  WozCopyOptions opts;
  for (int i = optind + 2; i < argc; i++)
    opts.selections.push_back(argv[i]);
  opts.destDir = destPrefix;
  opts.force = force;
  if (verbose) opts.progress = showProgress;

  // ---- open both images with same-filesystem enforcement ----
  Wozspector *src = wozNewSpector(fsType, verbose);
  Wozspector *dst = wozNewSpector(fsType, verbose);
  struct { Wozspector *spec; const char *path; const char *role; } imgs[2] = {
    { src, srcPath, "source" }, { dst, dstPath, "destination" },
  };
  for (int i = 0; i < 2; i++) {
    if (!imgs[i].spec->readFile(imgs[i].path, true)) {
      printf("ERROR: can't read %s image '%s'\n", imgs[i].role, imgs[i].path);
      return 1;
    }
    if (!imgs[i].spec->probe()) {
      printf("ERROR: %s image '%s' doesn't look like a %s volume\n",
             imgs[i].role, imgs[i].path, wozFsName(fsType));
      return 1;
    }
  }

  WozCopyResult result;
  if (!wozCopyFiles(src, dst, &opts, &result)) {
    if (!result.collisions.empty() && !force) {
      size_t n = result.collisions.size();
      printf("ERROR: %zu file%s already exist%s on the destination "
             "(use -f to overwrite):\n", n, n == 1 ? "" : "s",
             n == 1 ? "s" : "");
      for (size_t i = 0; i < n; i++)
        printf("  %s\n", result.collisions[i].c_str());
    }
    return 1;
  }

  // ---- persist: only reached when every copy succeeded ----
  if (!dst->writeFile(dstPath)) {
    printf("ERROR: failed to write '%s'\n", dstPath);
    return 1;
  }

  if (mode == 'p') {
    printf("Copied %zu file%s (%llu bytes) and %zu director%s from '%s' "
           "to '%s'\n",
           result.files, result.files == 1 ? "" : "s",
           (unsigned long long)result.bytes,
           result.dirsMade, result.dirsMade == 1 ? "y" : "ies",
           srcPath, dstPath);
  } else {
    printf("Copied %zu file%s (%llu bytes) from '%s' to '%s'\n",
           result.files, result.files == 1 ? "" : "s",
           (unsigned long long)result.bytes, srcPath, dstPath);
  }
  delete src;
  delete dst;
  return 0;
}
