#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "woz.h"
#include "crc32.h"
#include "nibutil.h"
#include "prodosspector.h"
#include "dosspector.h"
#include "pascalspector.h"
#include "wozhost.h"
#include "wozmsg.h"
#include "wozops.h"
#include "clireport.h"

#include <string>
#include <vector>

using namespace std;

#ifndef MAXPATH
#define MAXPATH 1023
#endif

uint8_t trackData[256*16];
char infoname[256] = {0};
bool verbose = false;
bool dosMode = true;    // DOS 3.3 mode (default)
bool pascalMode = false; // UCSD p-System / Apple Pascal mode (-P)
// ProDOS (-p) is the mode that is neither of the other two.
static inline bool prodosMode() { return !dosMode && !pascalMode; }
static wozFsType fsMode()
{
  return pascalMode ? WOZ_FS_PASCAL : dosMode ? WOZ_FS_DOS33 : WOZ_FS_PRODOS;
}
bool striphi = false;
bool useAllocation = false; // cpout uses full on-disk allocation not hdr length

Wozspector *inspector = NULL;

void usage(char *name)
{
  printf("Usage: %s -I <input image> { -d | -p | -P } [-c <command>] [-A]\n\n", name);
  printf("  -I [input filename]     input disk image to inspect/modify\n");
  printf("  -d                      DOS 3.3 mode\n");
  printf("  -p                      ProDOS mode\n");
  printf("  -P                      UCSD p-System / Apple Pascal mode\n");
  printf("  -c [command]            run a command non-interactively; repeat\n");
  printf("                          -c to run several commands in order\n");
  printf("  -A                      cpout: copy full on-disk allocation,\n");
  printf("                          not the directory-reported length\n");
  printf("  -B                      cpin: tokenize an Applesoft listing;\n");
  printf("                          cpout: detokenize Applesoft or Integer BASIC\n");
  printf("  -v                      verbose operation\n");
  printf("\n");
}

// A command's arguments, split the way a shell would: whitespace between
// them, '...' taken literally, "..." with \" and \\ inside, and a
// backslash escaping the next character anywhere else. That's how a name
// with a space in it ("BAS BIG" on a DOS disk) gets through.
//
// A command's last argument has always been the rest of the line, so
// `cat BAS BIG` and `cpout FOO my file.bin` work as they always did.
// rest() keeps that: if nothing from there on was quoted, it's the line as
// typed, spaces and all.
struct Args {
  vector<string> v;
  vector<size_t> start;  // where each argument began in raw
  vector<bool> quoted;   // had a quote or a backslash in it
  string raw;

  size_t size() const { return v.size(); }
  const char *operator[](size_t i) const { return v[i].c_str(); }
  bool isFlag(size_t i, const char *flag) const {
    return i < v.size() && !quoted[i] && v[i] == flag;
  }

  // Everything from argument i on, as one argument. False if there's more
  // than one argument there and some of it was quoted, since then there's
  // no telling what was meant.
  bool rest(size_t i, string *out) const {
    if (i >= v.size()) { *out = ""; return true; }
    if (i == v.size() - 1) { *out = v[i]; return true; }
    for (size_t j = i; j < v.size(); j++)
      if (quoted[j]) return false;
    size_t end = raw.find_last_not_of(" \t");
    *out = raw.substr(start[i], end + 1 - start[i]);
    return true;
  }
};

// Split `line` into args. False (having said why) on an unterminated quote.
static bool splitArgs(const char *line, Args *args)
{
  args->raw = line;
  const char *p = line;
  while (true) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return true;
    string a;
    bool quoted = false;
    size_t start = p - line;
    while (*p && *p != ' ' && *p != '\t') {
      if (*p == '\'') {
        quoted = true;
        const char *close = strchr(p + 1, '\'');
        if (!close) {
          printf("ERROR: unterminated ' quote\n");
          return false;
        }
        a.append(p + 1, close - p - 1);
        p = close + 1;
      } else if (*p == '"') {
        quoted = true;
        p++;
        while (*p && *p != '"') {
          if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
          a += *p++;
        }
        if (!*p) {
          printf("ERROR: unterminated \" quote\n");
          return false;
        }
        p++;
      } else if (*p == '\\' && p[1]) {
        quoted = true;
        a += p[1];
        p += 2;
      } else {
        a += *p++;
      }
    }
    args->v.push_back(a);
    args->start.push_back(start);
    args->quoted.push_back(quoted);
  }
}

static void tooManyArgs()
{
  printf("ERROR: too many arguments (quote a name that has spaces in it)\n");
}

typedef void (*cmdParser)(const Args &args);

struct _cmdInfo {
  char cmdName[20];
  cmdParser fp;
  char helpInfo[80];
};

void lsHandler(const Args &args)
{
  // With an argument, list that subdirectory (ProDOS); with none, dump the
  // whole tree as before.
  string dir;
  if (!args.rest(0, &dir)) { tooManyArgs(); return; }
  if (!dir.empty()) {
    Vent *list = inspector->getDirectoryListing(dir.c_str());
    printListing(list);
    inspector->freeListing(list);
    return;
  }

  printListing(inspector->getTree());
}

Vent *findFileByName(const char *name)
{
  // Resolves a leaf name or, for ProDOS, a slash-separated path. The
  // returned entry is owned by the tree; callers must not free it.
  return inspector->findEntry(name);
}

static void dumpFileContents(const char *cmd, bool forceStripHi)
{
  Vent *fp = findFileByName(cmd);
  if (fp) {
    char *dat = NULL;
    uint32_t s = inspector->getFileContents(fp, &dat);
    if (s && dat) {
      bool strip = forceStripHi || striphi;
      for (int i=0; i<s; i++) {
        char c = strip ? (dat[i] & 0x7F) : dat[i];
        if (c == 10 || c == 13) {
          printf("\n");
        } else {
          printf("%c", c);
        }
      }
      printf("\n");
      wozFree(dat);
    }
  } else {
    printf("File not found\n");
  }
}

void catHandler(const Args &args)
{
  string name;
  if (!args.rest(0, &name)) { tooManyArgs(); return; }
  dumpFileContents(name.c_str(), false);
}

void cat7Handler(const Args &args)
{
  // Always strips the high bit, regardless of the `strip` toggle.
  string name;
  if (!args.rest(0, &name)) { tooManyArgs(); return; }
  dumpFileContents(name.c_str(), true);
}

// Detokenize an Applesoft or Integer BASIC file to a host text file.
static bool listBasicToHostFile(Vent *fp, uint8_t *dat, uint32_t s,
                                const char *destPath)
{
  if (!wozIsBasicFile(fp)) {
    printf("File is not BASIC (Applesoft or Integer)\n");
    return false;
  }

  FILE *outf = fopen(destPath, "w");
  if (!outf) {
    printf("ERROR: failed to open '%s' for write\n", destPath);
    return false;
  }

  BasicListing lines;
  bool ok = wozDetokenize(inspector, fp, dat, s, &lines);
  printBasicListing(outf, lines, fp->getFileType() == FT_INT);
  fclose(outf);

  if (!ok)
    printf("Failed to list file\n");
  return ok;
}

void cpoutHandler(const Args &args)
{
  // cpout: copy from a file in the image to a file outside the image
  // cpout [-r] [-B] <image filename> <output filename>
  //   (source may be a ProDOS path; -r takes the resource fork of a GS/OS
  //    extended file instead of its data fork)
  bool wantResourceFork = false;
  bool wantBasicListing = false;
  // Leading -r and -B flags, in any order (e.g. "-B -r SRC DST")
  size_t a = 0;
  for (; a < args.size(); a++) {
    if (args.isFlag(a, "-r"))
      wantResourceFork = true;
    else if (args.isFlag(a, "-B"))
      wantBasicListing = true;
    else
      break;
  }

  if (args.size() < a + 2) {
    printf("Error parsing arguments. Usage: cpout [-r|-B] <SOURCE> <DEST>\n");
    return;
  }
  const char *buf = args[a];
  string dest;
  if (!args.rest(a + 1, &dest)) { tooManyArgs(); return; }
  const char *p = dest.c_str();

  Vent *fp = findFileByName(buf);
  if (!fp) {
    printf("File '%s' not found\n", buf);
    return;
  }

  if (!*p) {
    printf("Error: output filename is blank\n");
    return;
  }

  WozExportOptions xo;
  xo.resourceFork = wantResourceFork;
  xo.fullAllocation = useAllocation;
  uint8_t *dat = NULL;
  uint32_t s = 0;
  if (!wozExportFile(inspector, fp, &xo, buf, &dat, &s))
    return;

  if (!s || !dat) {
    printf("Empty file; skipping\n");
    return;
  }
  if (wantBasicListing) {
    listBasicToHostFile(fp, dat, s, p);
  } else {
    FILE *out = fopen(p, "w");
    if (!out) {
      printf("ERROR: failed to open '%s' for write\n", p);
    } else {
      fwrite(dat, 1, s, out);
      fclose(out);
    }
  }
  wozFree(dat);
}

void infoHandler(const Args &args)
{
  // FIXME: handle arguments? Or throw error?
  // Verbose mode adds the track-level detail
  printWozInfo(inspector, verbose ? (DUMP_QTMAP | DUMP_QTCRC | DUMP_TRACK | DUMP_RAWTRACK) : 0);
  printVolumeInfo(inspector);
}

void cpinHandler(const Args &args)
{
  // cpin: copy a local file in to a file image.
  // cpin <filename> <dest filename>
  // If it's an AppleSingle file, then the type comes from the AppleSingle
  //   ProDOS file format data; if it's not an AppleSingle file, then it's
  //   assumed it's an as-is binary file
  // An AppleSingle resource fork entry makes the destination a GS/OS
  //   extended (forked) file, which needs a ProDOS image.

  // Parse arguments:
  //   cpin <source> <dest> [type [aux]]
  // type/aux are ProDOS hex values (a leading 0x is optional). When given
  // they win over everything; otherwise we fall back to AppleSingle
  // metadata, then to a .SYSTEM-name heuristic, then to plain BIN/$2000.
  uint8_t filetype = 0x06; // ProDOS type numbers (more expressive than DOS)
  uint32_t fileSize = 0;
  uint16_t auxTypeData = 0x2000;

  // A leading -B: tokenize an Applesoft text listing on the way in, rather
  // than copying the bytes through untouched
  bool wantTokenize = false;
  size_t a = 0;
  for (; args.isFlag(a, "-B"); a++)
    wantTokenize = true;
  if (args.size() < a + 2) {
    printf("Usage: cpin [-B] <source> <dest> [type [aux]]   (type/aux are hex)\n");
    return;
  }
  if (args.size() > a + 4) { tooManyArgs(); return; }
  const char *fn = args[a];
  const char *destfn = args[a + 1];
  const char *typeStr = args.size() > a + 2 ? args[a + 2] : "";
  const char *auxStr = args.size() > a + 3 ? args[a + 3] : "";

  bool explicitType = (typeStr[0] != '\0');
  bool explicitAux  = (auxStr[0] != '\0');
  if (explicitType) {
    filetype = (uint8_t)strtol(typeStr, NULL, 16);
  }
  if (explicitAux) {
    auxTypeData = (uint16_t)strtol(auxStr, NULL, 16);
  }

  // stat the file so we get a length
  struct stat s;
  if (lstat(fn, &s)) {
    printf("Unable to stat source file '%s'\n", fn);
    return;
  }
  // ProDOS stores EOF in 24 bits, so a file tops out at 16MB-1. Refuse
  // anything bigger rather than silently truncating it.
  if (s.st_size > 0xFFFFFF) {
    printf("ERROR: '%s' is %lld bytes; ProDOS files max out at 16MB-1\n",
           fn, (long long)s.st_size);
    return;
  }
  fileSize = (uint32_t)s.st_size;

  // Read the file contents to a buffer
  uint8_t *fileContents = (uint8_t *)malloc(fileSize ? fileSize : 1);
  FILE *in = fopen(fn, "r");
  if (!in) {
    printf("ERROR: Failed to read file\n");
    free(fileContents);
    return;
  }
  fread(fileContents, 1, fileSize, in);
  fclose(in);

  WozImportOptions io;
  io.tokenize = wantTokenize;
  io.explicitType = explicitType;
  io.fileType = filetype;
  io.explicitAux = explicitAux;
  io.auxType = auxTypeData;
  io.sourceName = fn;
  wozImportFile(inspector, fileContents, fileSize, destfn, &io);
  free(fileContents);
}

void dumptrackHandler(const Args &args)
{
  // dumptrack <track#> <outfile>
  // Dumps one NIBTRACKSIZE-byte revolution of latched nibbles for the
  // given physical track to a raw binary file. Intended for analyzing
  // non-standard disk formats (rwts18, copy-protected layouts) where
  // the normal catalog / filesystem layers can't help.
  if (args.size() < 2) {
    printf("Usage: dumptrack <track#> <outfile>\n");
    return;
  }
  int trackNum = atoi(args[0]);
  if (trackNum < 0 || trackNum > 39) {
    printf("ERROR: track number %d out of range (0..39)\n", trackNum);
    return;
  }
  string out;
  if (!args.rest(1, &out)) { tooManyArgs(); return; }
  const char *outPath = out.c_str();
  if (!*outPath) {
    printf("ERROR: output filename is blank\n");
    return;
  }

  uint8_t buf[NIBTRACKSIZE];
  if (!inspector->readRawNibStream((uint8_t)trackNum, buf)) {
    printf("ERROR: failed to read raw nibble stream for track %d\n", trackNum);
    return;
  }

  FILE *f = fopen(outPath, "wb");
  if (!f) {
    printf("ERROR: failed to open '%s' for write\n", outPath);
    return;
  }
  size_t w = fwrite(buf, 1, NIBTRACKSIZE, f);
  fclose(f);
  if (w != NIBTRACKSIZE) {
    printf("ERROR: short write to '%s' (%zu of %d bytes)\n",
           outPath, w, NIBTRACKSIZE);
    return;
  }
  printf("Wrote %d bytes from track %d to '%s'\n",
         NIBTRACKSIZE, trackNum, outPath);
}

void resizeHandler(const Args &args)
{
  // resize <blocks>: shrink a ProDOS volume, and the image file holding
  // it, to the given size. Refuses if anything is allocated up there.
  if (!prodosMode()) {
    printf("ERROR: resize is ProDOS-only\n");
    return;
  }
  // -n: don't move anything; refuse if the tail is occupied.
  bool relocate = true;
  size_t a = 0;
  if (args.isFlag(a, "-n")) {
    relocate = false;
    a++;
  }

  if (args.size() < a + 1) {
    printf("Usage: resize [-n] <blocks>   (a block count, or 140k/800k/4m/"
           "5m/8m/10m/16m/32m; -n: refuse rather than move blocks down)\n");
    return;
  }
  if (args.size() > a + 1) { tooManyArgs(); return; }
  const char *tok = args[a];
  int32_t blocks = wozParseSizeSpec(tok);
  if (blocks < 1) {
    printf("ERROR: bad size '%s' (a block count 1-65535, or 140k, 800k, "
           "4m, 5m, 8m, 10m, 16m, 32m)\n", tok);
    return;
  }

  ProdosSpector *ps = (ProdosSpector *)inspector;
  ps->getTree(); // the volume's real block count isn't known until it loads
  uint32_t before = ps->volumeBlockCount();
  if (!ps->resizeVolume((uint32_t)blocks, relocate))
    return;
  if ((uint32_t)blocks != before) {
    printf("Volume resized from %u to %d blocks (%d bytes); "
           "'save' to write it out\n", before, blocks, blocks * 512);
  }
}

void volnameHandler(const Args &args)
{
  string name;
  if (!args.rest(0, &name)) { tooManyArgs(); return; }
  const char *cmd = name.c_str();
  if (!cmd[0]) {
    if (pascalMode)
      printf("Usage: volname <newname>   (1-7 chars, printable ASCII, not "
             "$=?,[#:)\n");
    else
      printf("Usage: volname <newname>   (1-15 chars: letter first, then "
             "letters, digits, or periods)\n");
    return;
  }
  // Pascal volume names follow different rules (1-7 chars, its own reserved
  // set); let PascalSpector::renameVolume do the validation.
  if (pascalMode) {
    if (!inspector->renameVolume(cmd)) {
      printf("ERROR: failed to rename volume\n");
      return;
    }
    printf("Volume renamed; use 'save' to make it permanent.\n");
    return;
  }
  char vol[16];
  if (!wozNormalizeProdosVolumeName(cmd, vol))
    return;
  if (!inspector->renameVolume(vol)) {
    printf("ERROR: failed to rename volume\n");
    return;
  }
  printf("Volume renamed to '%s'; use 'save' to make it permanent.\n", vol);
}

void bootblocksHandler(const Args &args)
{
  string donor;
  if (!args.rest(0, &donor)) { tooManyArgs(); return; }
  const char *cmd = donor.c_str();
  const bool fromDonor = (cmd[0] != '\0');
  if (!wozInstallBootBlocks(inspector, fromDonor ? cmd : NULL))
    return;
  printf("Boot blocks installed%s%s; use 'save' to make it permanent.\n",
         fromDonor ? " from " : (pascalMode ? " (embedded p-System boot code)"
                                            : " (embedded ProDOS boot code)"),
         fromDonor ? cmd : "");
  if (pascalMode)
    printf("Note: booting also needs SYSTEM.APPLE and SYSTEM.PASCAL (plus "
           "SYSTEM.FILER, SYSTEM.EDITOR and friends for a usable system) "
           "on the volume.\n");
  else
    printf("Note: booting also needs PRODOS and a .SYSTEM file (e.g. "
           "BASIC.SYSTEM) in the volume root.\n");
}

void formatHandler(const Args &args)
{
  if (!args.size()) {
    printf("Usage: format <filename> [<volume>] [<size>] [bootable]\n");
    printf("  DOS mode:    <volume> is 1-254 (default 254); always 140k.\n");
    printf("  ProDOS mode: <volume> is a name 1-15 chars (default BLANK);\n");
    printf("    <size> is 140k, 800k, 4m, 5m, 8m, 10m, 16m, 32m, or a block\n");
    printf("    count 5-65535 (default 140k). Sizes other than 140k must be\n");
    printf("    named .po, .hdv or .img (.hdv or .img below 140k).\n");
    printf("    'bootable' (a reserved word, not\n");
    printf("    usable as a volume name here) installs the standard ProDOS\n");
    printf("    boot block; booting also needs PRODOS and a .SYSTEM file.\n");
    printf("  Pascal mode: <volume> is a name 1-7 chars, printable ASCII\n");
    printf("    but not space or any of  $ = ? , [ # :  (default BLANK).\n");
    printf("    <size> is any of the above presets or a block count 6-65535\n");
    printf("    (default 140k); .po from 140k up, .hdv/.img at any size.\n");
    printf("    'bootable' installs the embedded p-System bootstrap that\n");
    printf("    suits the size; booting also needs SYSTEM.APPLE and\n");
    printf("    SYSTEM.PASCAL on the volume.\n");
    printf("    A token that looks like a size is taken as the size, so a\n");
    printf("    volume named e.g. '12' has to be set afterward with volname.\n");
    printf("  A 140k image writes .dsk (DOS) / .po (ProDOS, Pascal) data;\n");
    printf("  use wozzle to convert to .woz if needed.\n");
    return;
  }

  // The file name, then up to three of volume, size, and "bootable"
  int ntok = (int)args.size();
  if (ntok > 4) ntok = 4;
  const char *tok[4];
  for (int i = 0; i < ntok; i++) tok[i] = args[i];
  const char *fname = tok[0];

  // Sort out volume vs size arguments. In ProDOS mode sizes always start
  // with a digit and volume names never do, so the two can appear in
  // either order. In DOS mode there is no size argument - the numeric
  // argument is the volume number - and images are always 140k.
  //
  // Pascal names *may* start with a digit, so that invariant doesn't hold
  // there: a leading-digit token is read as the size, and a volume whose
  // name looks like one has to be set afterward with volname. The help
  // text says so; the alternative (strict positional arguments) makes the
  // far more common 'format disk.po 800k' silently produce a 140k volume
  // named 800K, which is worse.
  const uint32_t minBlocks = pascalMode ? 6 : 5;
  const char *volName = NULL;
  int32_t sizeBlocks = -1;
  bool bootable = false;
  for (int i = 1; i < ntok; i++) {
    if (!strcasecmp(tok[i], "bootable")) {
      if (dosMode) {
        printf("ERROR: 'bootable' is ProDOS-only (DOS 3.3 boots from the "
               "DOS image on tracks 0-2, which format doesn't install)\n");
        return;
      }
      bootable = true;
    } else if (!dosMode && isdigit((unsigned char)tok[i][0])) {
      if (sizeBlocks != -1) {
        printf("ERROR: more than one size given\n");
        return;
      }
      sizeBlocks = wozParseSizeSpec(tok[i]);
      if (sizeBlocks < (int32_t)minBlocks) {
        printf("ERROR: bad size '%s' (140k, 800k, 4m, 5m, 8m, 10m, 16m, 32m, "
               "or a block count %u-65535)\n", tok[i], minBlocks);
        return;
      }
    } else {
      if (volName) {
        printf("ERROR: too many arguments\n");
        return;
      }
      volName = tok[i];
    }
  }
  if (sizeBlocks == -1) sizeBlocks = 280;

  if (!wozCheckImageName(fsMode(), fname, (uint32_t)sizeBlocks))
    return;

  WozFormatOptions fo;
  fo.type = fsMode();
  fo.volumeName = volName;
  fo.dosVolume = volName ? atoi(volName) : 254;
  fo.blocks = (uint32_t)sizeBlocks;
  fo.bootable = bootable;
  uint8_t *img = NULL;
  uint32_t diskSize = 0;
  char vol[16];
  int dosVolume = 0;
  if (!wozBuildBlankImage(&fo, &img, &diskSize, vol, &dosVolume))
    return;

  // Don't clobber an existing file - the user may be formatting next to
  // the currently-loaded image and a typo shouldn't destroy work.
  struct stat st;
  if (lstat(fname, &st) == 0) {
    printf("ERROR: '%s' already exists; refusing to overwrite\n", fname);
    wozFree(img);
    return;
  }

  bool ok = false;
  FILE *f = fopen(fname, "wb");
  if (f) { ok = (fwrite(img, 1, diskSize, f) == diskSize); fclose(f); }
  wozFree(img);
  if (!ok) {
    printf("ERROR: failed to write '%s'\n", fname);
    return;
  }

  if (dosMode) {
    printf("Created blank DOS 3.3 image '%s' (volume %d)\n", fname, dosVolume);
  } else if (pascalMode) {
    printf("Created blank Pascal image '%s' (volume '%s', %d blocks%s)\n",
           fname, vol, sizeBlocks, bootable ? ", bootable" : "");
    if (bootable) {
      printf("Note: booting also needs SYSTEM.APPLE and SYSTEM.PASCAL "
             "(plus SYSTEM.FILER, SYSTEM.EDITOR and friends for a usable "
             "system) on the volume.\n");
      if (sizeBlocks == 280)
        printf("Note: a 5.25\" image has to be converted to .woz with "
               "wozzle before an emulator will boot it.\n");
    }
  } else {
    printf("Created blank ProDOS image '%s' (volume '%s', %d blocks%s)\n",
           fname, vol, sizeBlocks, bootable ? ", bootable" : "");
  }
}

void stripHandler(const Args &args)
{
  const char *cmd = args.size() == 1 ? args[0] : "";
  if (!strcmp(cmd, "on")) {
    striphi = true;
    printf("Strip high bit: true\n");
  } else if (!strcmp(cmd, "off")) {
    striphi = false;
    printf("Strip high bit: false\n");
  } else {
    printf("Bad arguments. Usage: strip <on|off>\n");
  }
}

void saveHandler(const Args &args)
{
  // With no argument, overwrite the image we originally loaded (infoname);
  // with an argument, save to that path instead.
  string path;
  if (!args.rest(0, &path)) { tooManyArgs(); return; }
  const char *dest = path.empty() ? infoname : path.c_str();
  if (!dest[0]) {
    printf("Usage: save [file name]\n");
    return;
  }
  if (!inspector->writeFile(dest)) {
    printf("Failed to write file\n");
    return;
  }
  printf("Saved to '%s'\n", dest);
}

void mkdirHandler(const Args &args)
{
  string name;
  if (!args.rest(0, &name)) { tooManyArgs(); return; }
  if (name.empty()) {
    printf("Usage: mkdir <dirname>\n");
    return;
  }
  inspector->makeDirectory(name.c_str());
}

void rmHandler(const Args &args)
{
  if (!args.size()) {
    printf("Usage: rm [-r] <filename>\n");
    return;
  }
  // "rm -r <dir>" removes a directory and everything beneath it.
  bool recursive = args.isFlag(0, "-r") || args.isFlag(0, "-R");
  string name;
  if (!args.rest(recursive ? 1 : 0, &name)) { tooManyArgs(); return; }
  if (recursive) {
    if (name.empty()) {
      printf("Usage: rm -r <directory>\n");
      return;
    }
    inspector->removeDirectoryRecursive(name.c_str());
    return;
  }
  inspector->removeFile(name.c_str());
}

void rmdirHandler(const Args &args)
{
  string name;
  if (!args.rest(0, &name)) { tooManyArgs(); return; }
  if (name.empty()) {
    printf("Usage: rmdir <dirname>\n");
    return;
  }
  inspector->removeDirectory(name.c_str());
}

void listHandler(const Args &args)
{
  string name;
  if (!args.rest(0, &name)) { tooManyArgs(); return; }
  const char *cmd = name.c_str();
  if (cmd[0] == 0) {
    printf("Bad arguments. Usage: list <filename>\n");
    return;
  }

  Vent *fp = findFileByName(cmd);
  if (!fp) {
    printf("File '%s' not found\n", cmd);
    return;
  }

  if (!wozIsBasicFile(fp)) {
    printf("File is not BASIC (Applesoft or Integer)\n");
    return;
  }

  uint8_t *dat = NULL;
  uint32_t s = inspector->getFileContents(fp, (char **)&dat);
  if (!s || !dat) {
    printf("File is empty\n");
    return;
  }

  BasicListing lines;
  bool ok = wozDetokenize(inspector, fp, dat, s, &lines);
  printBasicListing(stdout, lines, fp->getFileType() == FT_INT);
  if (!ok) {
    printf("Failed to list file\n");
  }
  wozFree(dat);
}

void inspectHandler(const Args &args)
{
  string name;
  if (!args.rest(0, &name)) { tooManyArgs(); return; }
  const char *cmd = name.c_str();
  Vent *fp = findFileByName(cmd);
  if (fp) {
    printf("Entry dump for '%s':\n", cmd);
    printEntry(fp, true);
    printFileDetails(inspector, fp);
  } else {
    printf("File not found\n");
  }
}

void krunchHandler(const Args &args)
{
  // krunch takes no arguments: consolidate free space at the end of the
  // volume (all files pack toward the front).
  inspector->krunch(NULL);
}

void krunchafterHandler(const Args &args)
{
  string name;
  if (!args.rest(0, &name)) { tooManyArgs(); return; }
  if (name.empty()) {
    printf("Usage: krunchafter <filename>   (put the free space right after "
           "<filename>)\n");
    return;
  }
  inspector->krunch(name.c_str());
}

void helpHandler(const Args &args); // forward decl for commands[]

struct _cmdInfo commands[] = {
  {"bootblocks", bootblocksHandler, "[<donor>]  : install boot code (embedded, or from a donor image)" },
  {"cat",   catHandler,   "<filename>        : Dump file contents" },
  {"cat7",  cat7Handler,  "<filename>       : Dump file contents, stripping the high bit" },
  {"cpin",  cpinHandler,  "[-B] <SRC> <DEST> [type [aux]]: copy host file in (-B: tokenize)" },
  {"cpout", cpoutHandler, "[-r|-B] <SRC> <DEST>: copy out (-r: resource fork, -B: detokenize BASIC)" },
  {"dumptrack", dumptrackHandler, "<trk> <file>: raw nibble dump of one track to a binary file" },
  {"format", formatHandler, "<fname> [<vol>] [<size>]: create a blank DOS 3.3, ProDOS or Pascal image" },
  {"help",  helpHandler,  "                 : this text" },
  {"info",  infoHandler,  "                 : show filesystem meta-information" },
  {"inspect", inspectHandler, "<filename>   : show meta-info about <filename>" },
  {"krunch", krunchHandler, "                : Pascal: consolidate free space at end of volume" },
  {"krunchafter", krunchafterHandler, "<file>: Pascal: put free space right after <file>" },
  {"list",  listHandler,  "<filename>       : Applesoft basic program detokenizer" },
  {"ls",    lsHandler,   "                   : List directory" },
  {"mkdir", mkdirHandler, "<dirname>        : create a directory (ProDOS only)" },
  {"resize", resizeHandler, "[-n] <blocks>  : shrink a ProDOS volume and its image file" },
  {"rm",    rmHandler,    "[-r] <filename>  : delete a file (-r: a directory tree)" },
  {"rmdir", rmdirHandler, "<dirname>        : delete an empty directory (ProDOS only)" },
  {"save",  saveHandler, "[filename]       : save disk image (default: overwrite the loaded image)" },
  {"strip", stripHandler, "<on|off>        : turn on or off high bit strip for 'cat'"},
  {"volname", volnameHandler, "<newname>     : rename the volume (ProDOS only)"},
  {"", 0, ""} };

void helpHandler(const Args &args)
{
  (void)args;
  printf("List of wozit commands:\n");
  struct _cmdInfo *p = commands;
  while (p->cmdName[0]) {
    printf("%s %s\n", p->cmdName, p->helpInfo);
    p++;
  }
  printf("\n");
}




void performCommand(char *cmd)
{
  // Match the command name as a whole word (terminated by end-of-string
  // or a space) so that e.g. "cat7 FOO" binds to cat7, not to cat with
  // a garbled argument.
  struct _cmdInfo *p = commands;
  bool handled = false;
  while (p->cmdName[0]) {
    size_t nl = strlen(p->cmdName);
    if (!strncmp(p->cmdName, cmd, nl) &&
        (cmd[nl] == '\0' || cmd[nl] == ' ')) {
      Args args;
      if (splitArgs(cmd + nl, &args))
        p->fp(args);
      handled = true;
      break;
    }
    p++;
  }
  if (!handled) {
    printf("Unrecognized command\n");
  }
}

int main(int argc, char *argv[]) {

  preload_crc();

  // Parse command-line arguments. Each -c queues a command to run
  // non-interactively, in order; optarg points into argv, which lives
  // for the whole program, so we just keep the pointers.
  char **oneShotCommands = (char **)malloc(argc * sizeof(char *));
  int oneShotCount = 0;
  int c;
  while ( (c=getopt(argc, argv, "dpPI:c:Ah?v")) != -1 ) {
    switch (c) {
    case 'd':
      dosMode = true;
      pascalMode = false;
      break;
    case 'p':
      dosMode = false;
      pascalMode = false;
      break;
    case 'P':
      dosMode = false;
      pascalMode = true;
      break;
    case 'I':
      strncpy(infoname, optarg, sizeof(infoname));
      break;
    case 'c':
      oneShotCommands[oneShotCount++] = optarg;
      break;
    case 'A':
      useAllocation = true;
      break;
    case 'v':
      verbose = true;
      break;
    case 'h':
    case '?':
      usage(argv[0]);
      exit(1);
    }
  }

  // `format` creates a brand-new image and never touches the loaded one,
  // so one-shot runs consisting solely of format commands don't need -I:
  //   wozit -p -c "format new.hdv MYVOL 32m"
  // Everything else still requires an image.
  bool needImage = true;
  if (!infoname[0] && oneShotCount) {
    needImage = false;
    for (int i = 0; i < oneShotCount; i++) {
      if (strncmp(oneShotCommands[i], "format", 6) != 0 ||
          (oneShotCommands[i][6] != ' ' && oneShotCommands[i][6] != '\0')) {
        needImage = true;
        break;
      }
    }
  }

  if (!infoname[0] && needImage) {
    printf("Must supply an image filename\n");
    exit(1);
  }

  if (needImage) {
    inspector = wozNewSpector(fsMode(), verbose);
    if (!inspector->readFile(infoname, true)) {
      printf("Failed to read file; aborting\n");
      exit(1);
    }

    // If the image doesn't smell like the filesystem we were told to use,
    // see whether it looks like one of the others, and warn the user.
    if (!inspector->probe()) {
      static const char *flags[] = { "", "-d", "-p", "-P" };
      wozFsType found = wozDetectFs(infoname);
      if (found != WOZ_FS_UNKNOWN && found != fsMode()) {
        printf("WARNING: '%s' looks like a %s image, but wozit was invoked "
               "in %s (%s) mode. Re-run with '%s' if you meant %s.\n",
               infoname, wozFsName(found), wozFsName(fsMode()),
               flags[fsMode()], flags[found], wozFsName(found));
      }
    }
  }

  if (oneShotCount) {
    for (int i = 0; i < oneShotCount; i++) {
      performCommand(oneShotCommands[i]);
    }
    free(oneShotCommands);
    return 0;
  }
  free(oneShotCommands);

  while (1) {
    char *cmd;
    if (inspector->isDirty()) {
      cmd = (char *)readline("wozit*> ");
    } else {
      cmd = (char *)readline("wozit> ");
    }
    if (!cmd) break;
    if (cmd[0]) {
      // Record non-empty lines so readline's up-arrow scrolls back
      // through them. Skip consecutive duplicates to avoid clutter.
      HIST_ENTRY *last = (history_length > 0) ? history_get(history_length) : NULL;
      if (!last || !last->line || strcmp(last->line, cmd) != 0) {
        add_history(cmd);
      }
      performCommand(cmd);
    }
    free(cmd);
  }

  return 0;
}
