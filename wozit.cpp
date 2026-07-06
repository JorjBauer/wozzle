#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#if defined(__APPLE__)
  #include <machine/endian.h>
#else
  #include <endian.h>
#endif

#include <readline/readline.h>
#include <readline/history.h>

#include "applesingle.h"
#include "bootblock.h"

#include "woz.h"
#include "crc32.h"
#include "nibutil.h"
#include "prodosspector.h"
#include "dosspector.h"
#include "applesoft.h"
#include "intbas.h"
//#include "vent.h"

#ifndef MAXPATH
#define MAXPATH 1023
#endif

uint8_t trackData[256*16];
char infoname[256] = {0};
bool verbose = false;
bool dosMode = true; // otherwise, prodos mode
bool striphi = false;
bool useAllocation = false; // cpout uses full on-disk allocation not hdr length

Wozspector *inspector = NULL;

void usage(char *name)
{
  printf("Usage: %s -I <input image> { -d | -p } [-c <command>] [-A]\n\n", name);
  printf("  -I [input filename]     input disk image to inspect/modify\n");
  printf("  -d                      DOS mode\n");
  printf("  -p                      ProDOS mode\n");
  printf("  -c [command]            run a command non-interactively; repeat\n");
  printf("                          -c to run several commands in order\n");
  printf("  -A                      cpout: copy full on-disk allocation,\n");
  printf("                          not the directory-reported length\n");
  printf("  -v                      verbose operation\n");
  printf("\n");
}

typedef void (*cmdParser)(char *cmd);

struct _cmdInfo {
  char cmdName[20];
  cmdParser fp;
  char helpInfo[80];
};

void lsHandler(char *cmd)
{
  // With an argument, list that subdirectory (ProDOS); with none, dump the
  // whole tree as before.
  if (cmd && strlen(cmd)) {
    inspector->displayDirectory(cmd);
    return;
  }

  inspector->displayTree();
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
      free(dat);
    }
  } else {
    printf("File not found\n");
  }
}

void catHandler(char *cmd)
{
  dumpFileContents(cmd, false);
}

void cat7Handler(char *cmd)
{
  // Always strips the high bit, regardless of the `strip` toggle.
  dumpFileContents(cmd, true);
}

void cpoutHandler(char *cmd)
{
  // cpout: copy from a file in the image to a file outside the image
  // cpout <image filename> <output filename>  (source may be a ProDOS path)
  char buf[256];
  char *p = strstr(cmd, " ");
  if (!p) {
    printf("Error parsing arguments. Usage: cpout <SOURCE> <DEST>\n");
    return;
  }
  uint32_t len = p-cmd;
  if (len > sizeof(buf)-1) {
    printf("Invalid filename\n");
    return;
  }
  strncpy(buf, cmd, p-cmd);
  buf[p-cmd] = '\0';
  p++; // pass the space
    
  Vent *fp = findFileByName(buf);
  if (!fp) {
    printf("File '%s' not found\n", buf);
    return;
  }

  if (!*p) {
    printf("Error: output filename is blank\n");
    return;
  }

  char *dat = NULL;
  uint32_t s;
  if (useAllocation) {
    s = inspector->getFileAllocation(fp, &dat);
  } else {
    s = inspector->getFileContents(fp, &dat);
    // Warn if the directory-length version is far smaller than what the
    // disk actually allocates - classic stub-loader pattern (TAIPAN-style).
    // Threshold picked to avoid noise on normal files where the program
    // ends part-way into its last sector: flag only when the allocated
    // size exceeds the logical size by more than 1 KB.
    uint32_t allocated = inspector->getAllocatedByteCount(fp);
    if (allocated > s && allocated - s > 1024) {
      fprintf(stderr,
              "WARNING: '%s' copied as %u bytes (directory length), but "
              "%u bytes are allocated on disk. Re-run with -A to copy the "
              "full allocation.\n",
              buf, s, allocated);
    }
  }

  if (s && dat) {
    FILE *out = fopen(p, "w");
    fwrite(dat, 1, s, out);
    fclose(out);
    free(dat);
  } else {
    printf("Empty file; skipping\n");
    return;
  }
}

void infoHandler(char *cmd)
{
  // FIXME: handle arguments? Or throw error?
  inspector->dumpInfo();
  inspector->displayInfo();
}

void cpinHandler(char *cmd)
{
  // cpin: copy a local file in to a file image.
  // cpin <filename> <dest filename>
  // If it's an AppleSingle file, then the type comes from the AppleSingle
  //   ProDOS file format data; if it's not an AppleSingle file, then it's
  //   assumed it's an as-is binary file

  // Parse arguments:
  //   cpin <source> <dest> [type [aux]]
  // type/aux are ProDOS hex values (a leading 0x is optional). When given
  // they win over everything; otherwise we fall back to AppleSingle
  // metadata, then to a .SYSTEM-name heuristic, then to plain BIN/$2000.
  char fn[MAXPATH+1];
  char destfn[256];
  char typeStr[16];
  char auxStr[16];
  uint8_t filetype = 0x06; // ProDOS type numbers (more expressive than DOS)
  uint32_t fileSize = 0;
  uint16_t auxTypeData = 0x2000;
  memset(fn, 0, sizeof(fn));
  memset(destfn, 0, sizeof(destfn));
  memset(typeStr, 0, sizeof(typeStr));
  memset(auxStr, 0, sizeof(auxStr));

  if (!cmd) {
    printf("Usage: cpin <source> <dest> [type [aux]]\n");
    return;
  }
  // Width must match MAXPATH: a narrower %s silently splits a long host
  // path across the following tokens instead of failing.
  int nargs = sscanf(cmd, "%1023s %255s %15s %15s", fn, destfn, typeStr, auxStr);
  if (nargs < 2) {
    printf("Usage: cpin <source> <dest> [type [aux]]   (type/aux are hex)\n");
    return;
  }
  if (strlen(fn) >= MAXPATH && cmd[MAXPATH] && cmd[MAXPATH] != ' ') {
    printf("ERROR: source path is longer than %d chars\n", MAXPATH);
    return;
  }

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
  uint8_t *dataToCopy = NULL;
  uint8_t *fileContents = (uint8_t *)malloc(fileSize);
  FILE *in = fopen(fn, "r");
  if (!in) {
    printf("ERROR: Failed to read file\n");
    free(fileContents);
    return;
  }
  fread(fileContents, 1, fileSize, in);
  fclose(in);

  // Look for an AppleSingle header
  bool foundAppleSingle = false;
  bool foundMetadata = false;
  if (fileSize >= sizeof(applesingle)) {
    applesingle *as = (applesingle *)fileContents;
    if (ntohl(as->magic) == 0x51600 && ntohl(as->version) == 0x20000) {
      foundAppleSingle = true;
      for (int i=0; i<16; i++) {
        if (as->filler[i] != 0) {
          printf("ERROR: Filler is NOT BLANK\n");
          foundAppleSingle = false;
          break;
        }
      }
      if (foundAppleSingle) {
        // entryID, offset, and length are all 32-bit big-endian fields;
        // ntohs on them would hand back the wrong half of the value.
        for (int i=0; i<ntohs(as->num_entries); i++) {
          printf("ENTRY %d:\n", i);
          printf("  entryID: %u\n", ntohl(as->entry[i].entryID));
          printf("  offset: %u\n", ntohl(as->entry[i].offset));
          printf("  length: %u\n", ntohl(as->entry[i].length));

          if (ntohl(as->entry[i].entryID) == as_prodosFileInfo) {
            type11prodos *p = (type11prodos *)(&fileContents[ntohl(as->entry[i].offset)]);
            printf("    Access: 0x%X\n", ntohs(p->access));
            printf("    FileType: 0x%X\n", ntohs(p->filetype));
            printf("    AuxType: 0x%X\n", ntohl(p->auxtype));
            /* The access field is bitwise ProDOS access permissions
             * FileType is a ProDOS file type (0x06 == BIN)
             * AuxType carries the start address for BIN files
             *   (cf. https://prodos8.com/docs/technote/19/)
             */
            foundMetadata = true;
            // Explicit command-line type/aux override the AppleSingle
            // metadata; otherwise adopt what the metadata declares.
            if (!explicitType) filetype = ntohs(p->filetype);
            if (!explicitAux)  auxTypeData = ntohl(p->auxtype);
          }
          else if (ntohl(as->entry[i].entryID) == as_dataFork) {
            fileSize = ntohl(as->entry[i].length);
            if (dataToCopy) {
              // dunno why we'd get 2 of them but...
              free(dataToCopy);
            }
            dataToCopy = (uint8_t *)malloc(fileSize);
            memcpy(dataToCopy, &fileContents[ntohl(as->entry[i].offset)], fileSize);
          } else {
            printf("ERROR: unknown entry ID type\n");
            foundAppleSingle = false;
            break;
          }
        }
        //asEntry entries[1]; // and continue off the end of the array
        // .entryID, .offset, .length
        // then _type11prodos (as_prodosFileInfo)
        // .access, .filetype, .auxtype
        // and as_dataFork

      }
    }
  }
  
  // If the caller didn't pin a type (no explicit arg, no AppleSingle
  // metadata) but the destination is named like a ProDOS system program,
  // make it a launchable SYS file loading at $2000 - the convention for
  // .SYSTEM programs. An explicit aux still wins if one was given.
  if (!explicitType && !foundMetadata) {
    size_t dl = strlen(destfn);
    const char *suffix = ".SYSTEM";
    size_t sl = strlen(suffix);
    if (dl >= sl && strcmp(destfn + dl - sl, suffix) == 0) {
      filetype = 0xFF; // SYS
      if (!explicitAux) auxTypeData = 0x2000;
    }
  }

  // If the destination already exists, remove it first. writeFileToImage
  // doesn't replace in place - it just appends a fresh catalog entry - so
  // without this a repeated cpin leaves stale duplicate entries behind.
  if (findFileByName(destfn)) {
    inspector->removeFile(destfn);
  }

  // Write the file into the image
  if (!inspector->writeFileToImage(dataToCopy ? dataToCopy : fileContents, destfn,
                                   filetype, auxTypeData, fileSize)) {
    printf("ERROR: Failed to write file\n");
  }
  if (dataToCopy) free(dataToCopy);
  free(fileContents);
}

// Lay down a freshly-formatted DOS 3.3 filesystem into the provided
// 140 KB buffer (which must already be zeroed). Track 17/0 gets the
// VTOC; sectors 17/15 down to 17/1 form the catalog chain with all
// entries zero.
static bool buildBlankDosDsk(uint8_t volumeNumber, uint8_t *img)
{
  const int trackBytes = 16 * 256;

  struct _vtoc *v = (struct _vtoc *)(img + 17 * trackBytes + 0 * 256);
  v->catalogTrack = 17;
  v->catalogSector = 15;
  v->dosVersion = 3;
  v->volumeNumber = volumeNumber;
  v->maxTSpairs = 122;
  v->lastTrackAllocated = 18;
  v->allocationDirection = 1;
  v->tracksPerDisk = 35;
  v->sectorsPerTrack = 16;
  v->bytesPerSectorLow = 0x00;
  v->bytesPerSectorHigh = 0x01; // 256 bytes/sector

  // Track-allocation bitmap. 1-bits are free, 0-bits are used. Tracks 0/1/2
  // hold the bootable DOS image, track 17 holds VTOC + catalog.
  for (int t = 0; t < 35; t++) {
    bool used = (t == 0 || t == 1 || t == 2 || t == 17);
    v->trackState[t].sectorsUsed[0] = used ? 0x00 : 0xFF;
    v->trackState[t].sectorsUsed[1] = used ? 0x00 : 0xFF;
  }

  // Catalog chain: sector 15 → 14 → ... → 1 → (0,0). Only the link
  // bytes need to be set; the 7 file entries per sector stay zero.
  for (int s = 15; s >= 1; s--) {
    uint8_t *sec = img + 17 * trackBytes + s * 256;
    sec[1] = (s > 1) ? 17 : 0;
    sec[2] = (s > 1) ? (uint8_t)(s - 1) : 0;
  }
  return true;
}

// Lay down a blank ProDOS volume of totalBlocks 512-byte blocks in the
// provided (zeroed) buffer. Blocks 0-1 (boot) stay zeros; blocks 2-5 are
// a 4-block volume directory chain; the volume bitmap starts at block 6
// and spans ceil(totalBlocks/4096) blocks (1 for a floppy, 16 for a
// 65535-block hard drive).
static bool buildBlankProdosPo(const char *volumeName, uint8_t *img,
                               uint32_t totalBlocks)
{
  uint8_t *b2 = img + 2 * 512;
  b2[0] = 0; b2[1] = 0;  // prev block = none
  b2[2] = 3; b2[3] = 0;  // next block = 3

  struct _subdirent *hdr = (struct _subdirent *)(b2 + 4);
  size_t nl = strlen(volumeName);
  if (nl > 15) nl = 15;
  hdr->typelen = (0x0F << 4) | (uint8_t)nl;  // 0xF = volume directory hdr
  memcpy(hdr->name, volumeName, nl);
  hdr->accessFlags = 0xC3;    // destroy, rename, write, read
  hdr->entryLength = 0x27;
  hdr->entriesPerBlock = 0x0D;
  hdr->volBitmap.pointer[0] = 6;
  hdr->volBitmap.pointer[1] = 0;
  hdr->volBlocks.total[0] = (uint8_t)(totalBlocks & 0xFF);
  hdr->volBlocks.total[1] = (uint8_t)(totalBlocks >> 8);

  // Remaining directory blocks in the chain. Only the prev/next header
  // bytes matter for an empty volume.
  for (int b = 3; b <= 5; b++) {
    uint8_t *blk = img + b * 512;
    blk[0] = (uint8_t)(b - 1);
    blk[2] = (b == 5) ? 0 : (uint8_t)(b + 1);
  }

  // Volume bitmap. MSb-first within each byte: bit 0x80 of byte 0 = block 0.
  // 1 = free, 0 = used. Used: blocks 0-1 (boot), 2-5 (vol dir), and the
  // bitmap itself. Bits at or past totalBlocks stay 0 - those blocks
  // don't exist and must be marked unavailable.
  uint8_t *bm = img + 6 * 512;
  uint32_t bitmapBlocks = (totalBlocks + 4095) / 4096;
  for (uint32_t b = 6 + bitmapBlocks; b < totalBlocks; b++) {
    bm[b / 8] |= (uint8_t)(0x80 >> (b % 8));
  }
  return true;
}

void dumptrackHandler(char *cmd)
{
  // dumptrack <track#> <outfile>
  // Dumps one NIBTRACKSIZE-byte revolution of latched nibbles for the
  // given physical track to a raw binary file. Intended for analyzing
  // non-standard disk formats (rwts18, copy-protected layouts) where
  // the normal catalog / filesystem layers can't help.
  if (!cmd || !cmd[0]) {
    printf("Usage: dumptrack <track#> <outfile>\n");
    return;
  }

  char *sp = strchr(cmd, ' ');
  if (!sp) {
    printf("Usage: dumptrack <track#> <outfile>\n");
    return;
  }
  int trackNum = atoi(cmd);
  if (trackNum < 0 || trackNum > 39) {
    printf("ERROR: track number %d out of range (0..39)\n", trackNum);
    return;
  }
  const char *outPath = sp + 1;
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

// Parse a format-size token: a named preset or a bare ProDOS block count.
// Presets are the sizes real devices presented; ProDOS itself accepts any
// block count up to 65535 (total_blocks is 16 bits), so bare numbers are
// allowed too. Returns the block count, or -1 if unparseable. Callers only
// pass tokens that start with a digit (ProDOS volume names can't).
static int32_t parseSizeSpec(const char *tok)
{
  static const struct { const char *name; uint32_t blocks; } presets[] = {
    { "140k", 280 },    // 5.25" floppy
    { "800k", 1600 },   // 3.5" floppy
    { "5m",   9728 },   // ProFile 5MB
    { "10m",  19456 },  // ProFile 10MB
    { "32m",  65535 },  // ProDOS maximum (one block shy of 32MB)
  };
  for (size_t i = 0; i < sizeof(presets)/sizeof(presets[0]); i++) {
    if (!strcasecmp(tok, presets[i].name)) return (int32_t)presets[i].blocks;
  }
  char *end;
  long v = strtol(tok, &end, 10);
  if (*tok && !*end && v >= 0 && v <= 0xFFFF) return (int32_t)v;
  return -1;
}

static bool hasExtension(const char *fname, const char *ext)
{
  const char *dot = strrchr(fname, '.');
  return dot && !strcasecmp(dot, ext);
}

// Validate and uppercase a ProDOS volume name: 1-15 chars, letter first,
// then letters/digits/periods. Prints the reason on failure.
static bool normalizeVolumeName(const char *src, char out[16])
{
  size_t nl = strlen(src);
  if (nl < 1 || nl > 15) {
    printf("ERROR: volume name must be 1-15 characters\n");
    return false;
  }
  memset(out, 0, 16);
  for (size_t i = 0; i < nl; i++) {
    char c = (char)toupper((unsigned char)src[i]);
    bool okc = (i == 0) ? isalpha((unsigned char)c)
                        : (isalnum((unsigned char)c) || c == '.');
    if (!okc) {
      printf("ERROR: bad volume name '%s' (letter first, then letters, "
             "digits, or periods)\n", src);
      return false;
    }
    out[i] = c;
  }
  return true;
}

void volnameHandler(char *cmd)
{
  if (!cmd || !cmd[0]) {
    printf("Usage: volname <newname>   (1-15 chars: letter first, then "
           "letters, digits, or periods)\n");
    return;
  }
  char vol[16];
  if (!normalizeVolumeName(cmd, vol))
    return;
  if (!inspector->renameVolume(vol)) {
    printf("ERROR: failed to rename volume\n");
    return;
  }
  printf("Volume renamed to '%s'; use 'save' to make it permanent.\n", vol);
}

// Fill b0/b1 with boot code: from a donor image if one is named, else the
// embedded standard ProDOS boot block (identical from ProDOS 1.1 through
// 2.4, and the same on floppies and hard drives) with a zero block 1.
static bool loadBootBlocks(const char *donor, uint8_t b0[512], uint8_t b1[512])
{
  if (!donor || !donor[0]) {
    memcpy(b0, prodosBootBlock, 512);
    memset(b1, 0, 512);
    return true;
  }

  ProdosSpector d(false, 0);
  if (!d.readFile((char *)donor, true)) {
    printf("ERROR: can't read boot donor image '%s'\n", donor);
    return false;
  }
  if (!d.probe()) {
    // A DOS 3.3 boot sector also starts with $01, so without this check a
    // DOS donor would pass the magic-byte test below and produce an
    // unbootable mongrel.
    printf("ERROR: '%s' doesn't look like a ProDOS volume; refusing to "
           "take boot code from it\n", donor);
    return false;
  }
  if (!d.readBootBlocks(b0, b1)) {
    printf("ERROR: can't read boot blocks from '%s' (not a ProDOS image?)\n",
           donor);
    return false;
  }
  if (b0[0] != 0x01) {
    // Every Apple II boot sector/block starts with $01. Refuse rather than
    // install something that certainly won't boot.
    printf("ERROR: '%s' block 0 doesn't look like boot code "
           "(first byte 0x%02X, not 0x01)\n", donor, b0[0]);
    return false;
  }
  return true;
}

void bootblocksHandler(char *cmd)
{
  uint8_t b0[512], b1[512];
  if (!loadBootBlocks(cmd, b0, b1))
    return;

  if (!inspector->writeBootBlocks(b0, b1)) {
    printf("ERROR: failed to write boot blocks\n");
    return;
  }
  printf("Boot blocks installed%s%s; use 'save' to make it permanent.\n",
         (cmd && cmd[0]) ? " from " : " (embedded ProDOS boot code)",
         (cmd && cmd[0]) ? cmd : "");
  printf("Note: booting also needs PRODOS and a .SYSTEM file (e.g. "
         "BASIC.SYSTEM) in the volume root.\n");
}

void formatHandler(char *cmd)
{
  if (!cmd || !cmd[0]) {
    printf("Usage: format <filename> [<volume>] [<size>] [bootable]\n");
    printf("  DOS mode:    <volume> is 1-254 (default 254); always 140k.\n");
    printf("  ProDOS mode: <volume> is a name 1-15 chars (default BLANK);\n");
    printf("    <size> is 140k, 800k, 5m, 10m, 32m, or a block count\n");
    printf("    5-65535 (default 140k). Sizes other than 140k must be\n");
    printf("    named .hdv or .img. 'bootable' (a reserved word, not\n");
    printf("    usable as a volume name here) installs the standard ProDOS\n");
    printf("    boot block; booting also needs PRODOS and a .SYSTEM file.\n");
    printf("  A 140k image writes .dsk (DOS) / .po (ProDOS) data; use\n");
    printf("  wozzle to convert to .woz if needed.\n");
    return;
  }

  char *tok[4] = {0};
  int ntok = 0;
  for (char *t = strtok(cmd, " "); t && ntok < 4; t = strtok(NULL, " ")) {
    tok[ntok++] = t;
  }
  const char *fname = tok[0];

  // Sort out volume vs size arguments. In ProDOS mode sizes always start
  // with a digit and volume names never do, so the two can appear in
  // either order. In DOS mode there is no size argument - the numeric
  // argument is the volume number - and images are always 140k.
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
      sizeBlocks = parseSizeSpec(tok[i]);
      if (sizeBlocks < 5) {
        printf("ERROR: bad size '%s' (140k, 800k, 5m, 10m, 32m, or a "
               "block count 5-65535)\n", tok[i]);
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

  // Only a 280-block image round-trips through the .po/.dsk floppy
  // loaders; anything else must be named for the raw-block HDV path.
  if (sizeBlocks != 280 &&
      !hasExtension(fname, ".hdv") && !hasExtension(fname, ".img")) {
    printf("ERROR: a %d-block image must be named .hdv or .img\n",
           sizeBlocks);
    return;
  }

  // Validate/normalize the ProDOS volume name up front, before we touch
  // the filesystem.
  char vol[16] = {0};
  if (!dosMode) {
    if (!normalizeVolumeName(volName ? volName : "BLANK", vol))
      return;
  }

  // Don't clobber an existing file - the user may be formatting next to
  // the currently-loaded image and a typo shouldn't destroy work.
  struct stat st;
  if (lstat(fname, &st) == 0) {
    printf("ERROR: '%s' already exists; refusing to overwrite\n", fname);
    return;
  }

  const size_t diskSize = (size_t)sizeBlocks * 512;
  uint8_t *img = (uint8_t *)calloc(1, diskSize);
  if (!img) {
    printf("ERROR: out of memory\n");
    return;
  }

  bool ok;
  if (dosMode) {
    int v = volName ? atoi(volName) : 254;
    if (v < 1 || v > 254) v = 254;
    ok = buildBlankDosDsk((uint8_t)v, img);
    if (ok) printf("Created blank DOS 3.3 image '%s' (volume %d)\n", fname, v);
  } else {
    ok = buildBlankProdosPo(vol, img, (uint32_t)sizeBlocks);
    if (ok && bootable) {
      // Standard ProDOS boot code in block 0; block 1 stays zero. The
      // volume boots once PRODOS and a .SYSTEM file are copied in.
      memcpy(img, prodosBootBlock, 512);
    }
    if (ok) printf("Created blank ProDOS image '%s' (volume '%s', %d "
                   "blocks%s)\n", fname, vol, sizeBlocks,
                   bootable ? ", bootable" : "");
  }

  if (ok) {
    FILE *f = fopen(fname, "wb");
    if (f) { ok = (fwrite(img, 1, diskSize, f) == diskSize); fclose(f); }
    else   { ok = false; }
  }

  free(img);
  if (!ok) {
    printf("ERROR: failed to write '%s'\n", fname);
  }
}

void stripHandler(char *cmd)
{
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

void saveHandler(char *cmd)
{
  // With no argument, overwrite the image we originally loaded (infoname);
  // with an argument, save to that path instead.
  const char *dest = (cmd && cmd[0]) ? cmd : infoname;
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

void mkdirHandler(char *cmd)
{
  char name[256];
  if (!cmd || sscanf(cmd, "%255s", name) != 1) {
    printf("Usage: mkdir <dirname>\n");
    return;
  }
  inspector->makeDirectory(name);
}

void rmHandler(char *cmd)
{
  char tok1[256], tok2[256];
  int n = cmd ? sscanf(cmd, "%255s %255s", tok1, tok2) : 0;
  if (n < 1) {
    printf("Usage: rm [-r] <filename>\n");
    return;
  }
  // "rm -r <dir>" removes a directory and everything beneath it.
  if (!strcmp(tok1, "-r") || !strcmp(tok1, "-R")) {
    if (n < 2) {
      printf("Usage: rm -r <directory>\n");
      return;
    }
    inspector->removeDirectoryRecursive(tok2);
    return;
  }
  inspector->removeFile(tok1);
}

void rmdirHandler(char *cmd)
{
  char name[256];
  if (!cmd || sscanf(cmd, "%255s", name) != 1) {
    printf("Usage: rmdir <dirname>\n");
    return;
  }
  inspector->removeDirectory(name);
}

void listHandler(char *cmd)
{
  if (cmd[0] == 0) {
    printf("Bad arguments. Usage: list <filename>\n");
    return;
  }

  Vent *fp = findFileByName(cmd);
  if (!fp) {
    printf("File '%s' not found\n", cmd);
    return;
  }

  uint8_t ft = fp->getFileType();
  if (ft != FT_BAS && ft != FT_INT) {
    printf("File is not BASIC (Applesoft or Integer)\n");
    return;
  }

  uint8_t *dat = NULL;
  uint32_t s = inspector->getFileContents(fp, (char **)&dat);
  if (!s || !dat) {
    printf("File is empty\n");
    return;
  }

  bool ok;
  if (ft == FT_BAS) {
    ApplesoftLister l;
    ok = l.listFile(dat, s, inspector->applesoftHeaderBytes());
  } else {
    IntegerLister l;
    ok = l.listFile(dat, s, inspector->applesoftHeaderBytes());
  }
  if (!ok) {
    printf("Failed to list file\n");
  }
  free(dat);
}

void inspectHandler(char *cmd)
{
  Vent *fp = findFileByName(cmd);
  if (fp) {
    printf("Entry dump for '%s':\n", cmd);
    fp->Dump(true);
    inspector->inspectFile(cmd, fp);
  } else {
    printf("File not found\n");
  }
}

void helpHandler(char *cmd); // forward decl for commands[]

struct _cmdInfo commands[] = {
  {"bootblocks", bootblocksHandler, "[<donor>]  : install ProDOS boot code (embedded, or copied from a donor image)" },
  {"cat",   catHandler,   "<filename>        : Dump file contents" },
  {"cat7",  cat7Handler,  "<filename>       : Dump file contents, stripping the high bit" },
  {"cpin",  cpinHandler,  "<SRC> <DEST> [type [aux]] : copy host file in; type/aux hex" },
  {"cpout", cpoutHandler, "<SOURCE> <DEST> : copy image file <SOURCE> out to host filesystem <DEST>" },
  {"dumptrack", dumptrackHandler, "<trk> <file>: raw nibble dump of one track to a binary file" },
  {"format", formatHandler, "<fname> [<vol>] [<size>]: create a blank DOS 3.3 or ProDOS image" },
  {"help",  helpHandler,  "                 : this text" },
  {"info",  infoHandler,  "                 : show filesystem meta-information" },
  {"inspect", inspectHandler, "<filename>   : show meta-info about <filename>" },
  {"list",  listHandler,  "<filename>       : Applesoft basic program detokenizer" },
  {"ls",    lsHandler,   "                   : List directory" },
  {"mkdir", mkdirHandler, "<dirname>        : create a directory (ProDOS only)" },
  {"rm",    rmHandler,    "[-r] <filename>  : delete a file (-r: a directory tree)" },
  {"rmdir", rmdirHandler, "<dirname>        : delete an empty directory (ProDOS only)" },
  {"save",  saveHandler, "[filename]       : save disk image (default: overwrite the loaded image)" },
  {"strip", stripHandler, "<on|off>        : turn on or off high bit strip for 'cat'"},
  {"volname", volnameHandler, "<newname>     : rename the volume (ProDOS only)"},
  {"", 0, ""} };

void helpHandler(char *cmd)
{
  // Ignoring cmd for now
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
      // If there's no argument, pass the null terminator itself instead
      // of reading one byte past it.
      char *arg = cmd[nl] ? &cmd[nl + 1] : &cmd[nl];
      p->fp(arg);
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
  while ( (c=getopt(argc, argv, "dpI:c:Ah?v")) != -1 ) {
    switch (c) {
    case 'd':
      dosMode = true;
      break;
    case 'p':
      dosMode = false;
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
    if (dosMode) {
      inspector = new DosSpector(verbose, verbose ? (DUMP_QTMAP | DUMP_QTCRC | DUMP_TRACK | DUMP_RAWTRACK) : 0);
    } else {
      inspector = new ProdosSpector(verbose, verbose ? (DUMP_QTMAP | DUMP_QTCRC | DUMP_TRACK | DUMP_RAWTRACK) : 0);
    }

    if (!inspector->readFile(infoname, true)) {
      printf("Failed to read file; aborting\n");
      exit(1);
    }

    // If the image doesn't smell like the filesystem we were told to use,
    // try probing the other kind. If that looks right, warn the user.
    if (!inspector->probe()) {
      Wozspector *other = dosMode
        ? (Wozspector *)new ProdosSpector(false, 0)
        : (Wozspector *)new DosSpector(false, 0);
      if (other->readFile(infoname, true) && other->probe()) {
        printf("WARNING: '%s' looks like a %s image, but wozit was invoked "
               "in %s mode. Re-run with '%s' if you meant %s.\n",
               infoname,
               dosMode ? "ProDOS" : "DOS 3.3",
               dosMode ? "DOS 3.3 (-d)" : "ProDOS (-p)",
               dosMode ? "-p" : "-d",
               dosMode ? "ProDOS" : "DOS 3.3");
      }
      delete other;
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
