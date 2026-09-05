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
#include <map>
#include <set>

#include "crc32.h"
#include "dosspector.h"
#include "prodosspector.h"
#include "pascalspector.h"
#include "vent.h"

using namespace std;

static bool force = false;
static bool verbose = false;
static string destPrefix; // -D: destination directory every copy lands under

struct CopyItem {
  Vent *ent;
  string path;
};

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

// Walk one ProDOS directory group in the flat tree (its entries run from
// the node after the header until the next header), recursing into
// subdirectories via the key-block -> header map.
static void collectProdos(Vent *hdr, const string &prefix,
                          map<uint16_t, Vent *> &groupByKey,
                          vector<string> &dirs, vector<CopyItem> &files)
{
  for (Vent *p = hdr->nextEnt(); p && !p->isHeader(); p = p->nextEnt()) {
    string path = prefix.empty() ? p->getName() : prefix + "/" + p->getName();
    if (p->isDirectory()) {
      dirs.push_back(path);
      map<uint16_t, Vent *>::iterator sub = groupByKey.find(p->keyPointerVal());
      if (sub == groupByKey.end()) {
        fprintf(stderr, "WARNING: directory '%s' has no readable contents "
                "(corrupt image?); copying it empty\n", path.c_str());
        continue;
      }
      collectProdos(sub->second, path, groupByKey, dirs, files);
    } else {
      CopyItem ci = { p, path };
      files.push_back(ci);
    }
  }
}

// True if `path` equals `sel` or lies underneath it (path-component-wise).
static bool underSelection(const string &path, const string &sel)
{
  if (path == sel) return true;
  return path.size() > sel.size() &&
         path.compare(0, sel.size(), sel) == 0 &&
         path[sel.size()] == '/';
}

int main(int argc, char *argv[])
{
  preload_crc();

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

  // Normalize -D: volume-root relative, no leading or trailing slashes,
  // uppercased the way ProDOS stores names.
  if (!destPrefix.empty()) {
    if (mode != 'p') {
      printf("ERROR: -D needs -p; neither DOS 3.3 nor Pascal has directories\n");
      return 1;
    }
    while (!destPrefix.empty() && destPrefix[0] == '/')
      destPrefix.erase(0, 1);
    while (!destPrefix.empty() && destPrefix[destPrefix.size() - 1] == '/')
      destPrefix.erase(destPrefix.size() - 1);
    for (size_t i = 0; i < destPrefix.size(); i++)
      destPrefix[i] = toupper((unsigned char)destPrefix[i]);
    if (destPrefix.empty()) {
      printf("ERROR: -D needs a directory name\n");
      return 1;
    }
    // Catch an unusable name here rather than partway through a copy.
    size_t start = 0;
    while (start <= destPrefix.size()) {
      size_t slash = destPrefix.find('/', start);
      size_t end = (slash == string::npos) ? destPrefix.size() : slash;
      string comp = destPrefix.substr(start, end - start);
      if (comp.empty() || comp.size() > 15) {
        printf("ERROR: '%s' isn't a usable ProDOS directory name (each part "
               "must be 1 to 15 characters)\n", destPrefix.c_str());
        return 1;
      }
      if (slash == string::npos) break;
      start = slash + 1;
    }
  }
  const char *srcPath = argv[optind];
  const char *dstPath = argv[optind + 1];
  if (!strcmp(srcPath, dstPath)) {
    printf("ERROR: source and destination are the same file\n");
    return 1;
  }

  vector<string> selections;
  for (int i = optind + 2; i < argc; i++) {
    string s = argv[i];
    // Strip a leading slash; paths are volume-root relative.
    if (!s.empty() && s[0] == '/') s = s.substr(1);
    if (mode == 'p' || mode == 'P') {
      // ProDOS and Pascal names are uppercase on disk; match the way wozit does.
      for (size_t j = 0; j < s.size(); j++) s[j] = toupper((unsigned char)s[j]);
    }
    if (!s.empty()) selections.push_back(s);
  }

  // ---- open both images with same-filesystem enforcement ----
  Wozspector *src, *dst;
  if (mode == 'd') {
    src = new DosSpector(verbose, 0);
    dst = new DosSpector(verbose, 0);
  } else if (mode == 'P') {
    src = new PascalSpector(verbose, 0);
    dst = new PascalSpector(verbose, 0);
  } else {
    src = new ProdosSpector(verbose, 0);
    dst = new ProdosSpector(verbose, 0);
  }
  const char *fsName = (mode == 'd') ? "DOS 3.3"
                     : (mode == 'P') ? "Pascal" : "ProDOS";

  struct { Wozspector *spec; const char *path; const char *role; } imgs[2] = {
    { src, srcPath, "source" }, { dst, dstPath, "destination" },
  };
  for (int i = 0; i < 2; i++) {
    if (!imgs[i].spec->readFile((char *)imgs[i].path, true)) {
      printf("ERROR: can't read %s image '%s'\n", imgs[i].role, imgs[i].path);
      return 1;
    }
    if (!imgs[i].spec->probe()) {
      printf("ERROR: %s image '%s' doesn't look like a %s volume\n",
             imgs[i].role, imgs[i].path, fsName);
      return 1;
    }
  }

  // ---- enumerate the source ----
  // An empty DOS 3.3 catalog legitimately yields a NULL tree (there are no
  // entries and no header node); an empty ProDOS volume still has its
  // volume-header node, so NULL there means the directory didn't parse.
  Vent *tree = src->getTree();
  if (!tree && mode == 'p') {
    printf("ERROR: can't read the source directory\n");
    return 1;
  }

  vector<string> allDirs;
  vector<CopyItem> allFiles;
  if (mode == 'p') {
    map<uint16_t, Vent *> groupByKey;
    for (Vent *p = tree; p; p = p->nextEnt()) {
      if (p->isHeader()) groupByKey[p->keyPointerVal()] = p;
    }
    if (!tree->isHeader()) {
      printf("ERROR: unexpected tree shape (no volume header)\n");
      return 1;
    }
    collectProdos(tree, "", groupByKey, allDirs, allFiles);
  } else {
    for (Vent *p = tree; p; p = p->nextEnt()) {
      CopyItem ci = { p, p->getName() };
      allFiles.push_back(ci);
    }
  }

  // ---- apply path selections ----
  vector<string> dirs;
  vector<CopyItem> files;
  if (selections.empty()) {
    dirs = allDirs;
    files = allFiles;
  } else {
    for (size_t s = 0; s < selections.size(); s++) {
      const string &sel = selections[s];
      bool matched = false;
      for (size_t i = 0; i < allDirs.size(); i++) {
        if (underSelection(allDirs[i], sel)) { dirs.push_back(allDirs[i]); matched = true; }
      }
      for (size_t i = 0; i < allFiles.size(); i++) {
        if (underSelection(allFiles[i].path, sel)) { files.push_back(allFiles[i]); matched = true; }
      }
      if (!matched) {
        printf("ERROR: '%s' not found on the source volume\n", sel.c_str());
        return 1;
      }
    }
  }

  // Relocate the whole copy under -D's directory. From here on the paths
  // in dirs/files name destination locations only; the source side is done
  // with (file contents come from the Vent, not from a path lookup). The
  // prefix's own components get picked up by the parent-prefix pass below.
  if (!destPrefix.empty()) {
    for (size_t i = 0; i < dirs.size(); i++)
      dirs[i] = destPrefix + "/" + dirs[i];
    for (size_t i = 0; i < files.size(); i++)
      files[i].path = destPrefix + "/" + files[i].path;
  }

  // Every parent prefix of everything we copy must exist on the
  // destination; add them to the directory list. (A lexical sort later
  // guarantees parents are created before children.)
  set<string> needDirs(dirs.begin(), dirs.end());
  for (size_t i = 0; i < files.size(); i++) {
    const string &p = files[i].path;
    for (size_t pos = p.find('/'); pos != string::npos; pos = p.find('/', pos + 1))
      needDirs.insert(p.substr(0, pos));
  }
  // -D's own directory needs to exist even when the copy is nothing but
  // empty directories, in which case no file path names it above.
  if (!destPrefix.empty()) {
    needDirs.insert(destPrefix);
    for (size_t pos = destPrefix.find('/'); pos != string::npos;
         pos = destPrefix.find('/', pos + 1))
      needDirs.insert(destPrefix.substr(0, pos));
  }
  dirs.assign(needDirs.begin(), needDirs.end()); // sorted: parents first

  // ---- collision check (before touching anything) ----
  vector<string> collisions;
  for (size_t i = 0; i < dirs.size(); i++) {
    Vent *e = dst->findEntry(dirs[i].c_str());
    if (e && !e->isDirectory()) {
      printf("ERROR: '%s' is a directory on the source but a file on the "
             "destination; not copying anything\n", dirs[i].c_str());
      return 1;
    }
  }
  for (size_t i = 0; i < files.size(); i++) {
    Vent *e = dst->findEntry(files[i].path.c_str());
    if (e && e->isDirectory()) {
      printf("ERROR: '%s' is a file on the source but a directory on the "
             "destination; not copying anything\n", files[i].path.c_str());
      return 1;
    }
    if (e) collisions.push_back(files[i].path);
  }
  if (!collisions.empty() && !force) {
    printf("ERROR: %zu file%s already exist%s on the destination "
           "(use -f to overwrite):\n", collisions.size(),
           collisions.size() == 1 ? "" : "s",
           collisions.size() == 1 ? "s" : "");
    for (size_t i = 0; i < collisions.size(); i++)
      printf("  %s\n", collisions[i].c_str());
    return 1;
  }

  // ---- copy ----
  uint64_t bytes = 0;
  size_t dirsMade = 0;
  for (size_t i = 0; i < dirs.size(); i++) {
    if (dst->findEntry(dirs[i].c_str()))
      continue; // exists (as a directory, per the check above)
    if (!dst->makeDirectory(dirs[i].c_str())) {
      printf("ERROR: couldn't create directory '%s'; destination not "
             "modified\n", dirs[i].c_str());
      return 1;
    }
    dirsMade++;
  }

  for (size_t i = 0; i < files.size(); i++) {
    Vent *e = files[i].ent;
    const string &path = files[i].path;

    // ProDOS: the EOF-length contents plus the entry's metadata is a
    // faithful copy. DOS 3.3: copy the raw allocated sector stream instead
    // - the address/length headers some types embed ride along verbatim,
    // and types getFileContents can't even size (TXT, SYS, REL) still copy.
    char *data = NULL;
    uint32_t len = (mode == 'p' || mode == 'P') ? src->getFileContents(e, &data)
                                                : src->getFileAllocation(e, &data);
    uint8_t dummy = 0;
    uint8_t *payload = len ? (uint8_t *)data : &dummy;

    // A GS/OS extended file carries a second (resource) fork and a block of
    // Finder information alongside its data; all three come across too.
    bool extended = (mode == 'p') && ProdosSpector::isExtendedFile(e);
    char *rdata = NULL;
    uint32_t rlen = 0;
    ProdosFork dataInfo, resInfo;
    uint8_t extKeyBlock[512];
    bool haveExtKeyBlock = false;
    if (extended) {
      ProdosSpector *psrc = (ProdosSpector *)src;
      rlen = psrc->getForkContents(e, true, &rdata);
      psrc->getForkInfo(e, false, &dataInfo);
      psrc->getForkInfo(e, true, &resInfo);
      haveExtKeyBlock = psrc->readExtendedKeyBlock(e, extKeyBlock);
    }

    if (force && dst->findEntry(path.c_str())) {
      if (!dst->removeFile(path.c_str())) {
        printf("ERROR: couldn't replace '%s'; destination not modified\n",
               path.c_str());
        free(data);
        free(rdata);
        return 1;
      }
      if (verbose) printf("(replacing %s)\n", path.c_str());
    }

    bool ok;
    if (extended) {
      // A fork the source doesn't have at all stays absent on the copy; a
      // fork it has but which is empty is recreated empty, so pass a
      // non-NULL pointer to tell those two cases apart.
      static const uint8_t emptyFork = 0;
      const uint8_t *dptr = len ? (const uint8_t *)data
                          : (dataInfo.storageType ? &emptyFork : NULL);
      const uint8_t *rptr = rlen ? (const uint8_t *)rdata
                          : (resInfo.storageType ? &emptyFork : NULL);
      ok = ((ProdosSpector *)dst)->writeExtendedFileWithMeta(
             dptr, len, rptr, rlen, path.c_str(), e->getProdosFent(),
             haveExtKeyBlock ? extKeyBlock : NULL);
    } else if (mode == 'p') {
      ok = ((ProdosSpector *)dst)->writeFileWithMeta(payload, path.c_str(),
                                                     len, e->getProdosFent());
    } else if (mode == 'P') {
      ok = ((PascalSpector *)dst)->writeFileWithMeta(payload, path.c_str(),
                                                     len, e->getPascalFent());
    } else {
      // typeAndFlags verbatim carries the type bits and the locked flag.
      ok = ((DosSpector *)dst)->writeFileRaw(payload, path.c_str(),
                                             e->getDosFent()->fileTypeAndFlags,
                                             len);
    }
    free(data);
    free(rdata);

    if (!ok) {
      printf("ERROR: failed to copy '%s'; destination not modified\n",
             path.c_str());
      return 1;
    }
    if (verbose) {
      if (extended)
        printf("  %s (%u bytes data + %u bytes resource)\n", path.c_str(),
               len, rlen);
      else
        printf("  %s (%u bytes)\n", path.c_str(), len);
    }
    bytes += len + rlen;
  }

  // ---- persist: only reached when every copy succeeded ----
  if (!dst->writeFile(dstPath)) {
    printf("ERROR: failed to write '%s'\n", dstPath);
    return 1;
  }

  if (mode == 'p') {
    printf("Copied %zu file%s (%llu bytes) and %zu director%s from '%s' "
           "to '%s'\n",
           files.size(), files.size() == 1 ? "" : "s",
           (unsigned long long)bytes,
           dirsMade, dirsMade == 1 ? "y" : "ies",
           srcPath, dstPath);
  } else {
    printf("Copied %zu file%s (%llu bytes) from '%s' to '%s'\n",
           files.size(), files.size() == 1 ? "" : "s",
           (unsigned long long)bytes, srcPath, dstPath);
  }
  return 0;
}
