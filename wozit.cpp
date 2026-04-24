#include <stdio.h>
#include <getopt.h>
#include <string.h>
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

#include "woz.h"
#include "crc32.h"
#include "nibutil.h"
#include "prodosspector.h"
#include "dosspector.h"
#include "applesoft.h"
#include "intbas.h"
//#include "vent.h"

#ifndef MAXPATH
#define MAXPATH 127
#endif

uint8_t trackData[256*16];
char infoname[256] = {0};
char oneShotCommand[512] = {0};
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
  printf("  -c [command]            run a single command non-interactively\n");
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
  if (cmd && strlen(cmd)) {
    printf("Extraneous arguments: '%s' (0x%llX)\n", cmd, (unsigned long long)cmd);
    return;
  }

  inspector->displayTree();
}

Vent *findFileByName(const char *name)
{
  Vent *tree = inspector->getTree();
  Vent *p = tree;
  while (p) {
    if (!strcmp(name, p->getName())) {
      return p;
    }
    p = p->nextEnt();
  }
  return NULL;
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
  // cpout <image filename> <output filename>
  char buf[16];
  char *p = strstr(cmd, " ");
  if (!p) {
    printf("Error parsing arguments. Usage: cpout <SOURCE> <DEST>\n");
    return;
  }
  uint32_t len = p-cmd;
  if (len > 15) {
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
    // disk actually allocates — classic stub-loader pattern (TAIPAN-style).
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

  // Parse arguments
  char fn[MAXPATH+1];
  char destfn[30+1];
  uint8_t filetype = 0x06; // ProDOS type numbers (more expressive than DOS)
  uint16_t fileSize = 0;
  uint16_t auxTypeData = 0x2000;
  memset(fn, 0, sizeof(fn));
  memset(destfn, 0, sizeof(destfn));

  char *fnp = cmd;
  if (!fnp) {
    printf("Usage[1]: cpin <filename> <dest filename>\n");
    return;
  }
  char *destfnp = strstr(fnp, " ");
  if (!destfnp) {
    printf("Usage[2]: cpin <filename> <dest filename>\n");
    return;
  }
  strncpy(fn, fnp, destfnp-fnp < MAXPATH ? destfnp-fnp : MAXPATH);
  destfnp++;
  strncpy(destfn, destfnp, 30);
  
  // stat the file so we get a length
  struct stat s;
  if (lstat(fn, &s)) {
    printf("Unable to stat source file '%s'\n", fn);
    return;
  }
  fileSize = s.st_size; // assume file size to copy in is the 

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
        for (int i=0; i<ntohs(as->num_entries); i++) {
          printf("ENTRY %d:\n", i);
          printf("  entryID: %d\n", ntohs(as->entry[i].entryID));
          printf("  offset: %d\n", ntohs(as->entry[i].offset));
          printf("  length: %d\n", ntohs(as->entry[i].length));

          if (ntohs(as->entry[i].entryID) == as_prodosFileInfo) {
            type11prodos *p = (type11prodos *)(&fileContents[ntohs(as->entry[i].offset)]);
            printf("    Access: 0x%X\n", ntohs(p->access));
            printf("    FileType: 0x%X\n", ntohs(p->filetype));
            printf("    AuxType: 0x%X\n", ntohl(p->auxtype));
            /* The access field is bitwise ProDOS access permissions
             * FileType is a ProDOS file type (0x06 == BIN)
             * AuxType carries the start address for BIN files
             *   (cf. https://prodos8.com/docs/technote/19/)
             */
            foundMetadata = true;
            filetype = ntohs(p->filetype);
            auxTypeData = ntohl(p->auxtype);
          }
          else if (ntohs(as->entry[i].entryID) == as_dataFork) {
            fileSize = ntohs(as->entry[i].length);
            if (dataToCopy) {
              // dunno why we'd get 2 of them but...
              free(dataToCopy);
            }
            dataToCopy = (uint8_t *)malloc(fileSize);
            memcpy(dataToCopy, &fileContents[ntohs(as->entry[i].offset)], fileSize);
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

// Lay down a blank ProDOS volume. Block 0 (boot) stays zeros; blocks 2-5
// are a 4-block volume directory chain; block 6 is the volume bitmap.
static bool buildBlankProdosPo(const char *volumeName, uint8_t *img)
{
  const uint16_t totalBlocks = 280;

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
  // Used: blocks 0 (boot), 1 (reserved), 2-5 (vol dir), 6 (bitmap).
  uint8_t *bm = img + 6 * 512;
  bm[0] = 0x01;                             // block 7 free; 0-6 used
  for (int i = 1; i <= 34; i++) bm[i] = 0xFF; // blocks 8..279 free
  // bytes 35..511 stay 0 — bits past block 279 must be marked unavailable
  return true;
}

void formatHandler(char *cmd)
{
  if (!cmd || !cmd[0]) {
    printf("Usage: format <filename> [<volume>]\n");
    printf("  DOS mode:    <volume> is 1-254 (default 254).\n");
    printf("  ProDOS mode: <volume> is a name 1-15 chars (default BLANK).\n");
    printf("  Image type follows the current -d/-p mode. Writes .dsk (DOS)\n");
    printf("  or .po (ProDOS); use wozzle to convert to .woz if needed.\n");
    return;
  }

  char fname[MAXPATH + 1] = {0};
  char vol[32] = {0};
  char *sp = strchr(cmd, ' ');
  if (sp) {
    size_t fl = (size_t)(sp - cmd);
    if (fl > MAXPATH) fl = MAXPATH;
    memcpy(fname, cmd, fl);
    strncpy(vol, sp + 1, sizeof(vol) - 1);
  } else {
    strncpy(fname, cmd, MAXPATH);
  }

  // Don't clobber an existing file — the user may be formatting next to
  // the currently-loaded image and a typo shouldn't destroy work.
  struct stat st;
  if (lstat(fname, &st) == 0) {
    printf("ERROR: '%s' already exists; refusing to overwrite\n", fname);
    return;
  }

  const size_t DISK_SIZE = 35 * 16 * 256;  // = 280 * 512
  uint8_t *img = (uint8_t *)calloc(1, DISK_SIZE);
  if (!img) {
    printf("ERROR: out of memory\n");
    return;
  }

  bool ok;
  if (dosMode) {
    int v = vol[0] ? atoi(vol) : 254;
    if (v < 1 || v > 254) v = 254;
    ok = buildBlankDosDsk((uint8_t)v, img);
    if (ok) {
      FILE *f = fopen(fname, "wb");
      if (f) { ok = (fwrite(img, 1, DISK_SIZE, f) == DISK_SIZE); fclose(f); }
      else   { ok = false; }
    }
    if (ok) printf("Created blank DOS 3.3 image '%s' (volume %d)\n", fname, v);
  } else {
    const char *name = vol[0] ? vol : "BLANK";
    ok = buildBlankProdosPo(name, img);
    if (ok) {
      FILE *f = fopen(fname, "wb");
      if (f) { ok = (fwrite(img, 1, DISK_SIZE, f) == DISK_SIZE); fclose(f); }
      else   { ok = false; }
    }
    if (ok) printf("Created blank ProDOS image '%s' (volume '%s')\n", fname, name);
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
  if (!cmd || !cmd[0]) {
    printf("Usage: save <file name>\n");
    return;
  }
  if (!inspector->writeFile(cmd)) {
    printf("Failed to write file\n");
  }
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
  {"cat",   catHandler,   "<filename>        : Dump file contents" },
  {"cat7",  cat7Handler,  "<filename>       : Dump file contents, stripping the high bit" },
  {"cpin",  cpinHandler,  "<SOURCE> <DEST>  : copy host file <SOURCE> in to image filename <DEST>" },
  {"cpout", cpoutHandler, "<SOURCE> <DEST> : copy image file <SOURCE> out to host filesystem <DEST>" },
  {"format", formatHandler, "<fname> [<vol>]: create a blank DOS 3.3 or ProDOS image" },
  {"help",  helpHandler,  "                 : this text" },
  {"info",  infoHandler,  "                 : show filesystem meta-information" },
  {"inspect", inspectHandler, "<filename>   : show meta-info about <filename>" },
  {"list",  listHandler,  "<filename>       : Applesoft basic program detokenizer" },
  {"ls",    lsHandler,   "                   : List directory" },
  {"save",  saveHandler, "<filename>       : save modified disk image as <filename>" },
  {"strip", stripHandler, "<on|off>        : turn on or off high bit strip for 'cat'"},
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

  // Parse command-line arguments
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
      strncpy(oneShotCommand, optarg, sizeof(oneShotCommand) - 1);
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

  if (!infoname[0]) {
    printf("Must supply an image filename\n");
    exit(1);
  }

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

  if (oneShotCommand[0]) {
    performCommand(oneShotCommand);
    return 0;
  }

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
