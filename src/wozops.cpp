#include "wozops.h"
#include "wozmsg.h"
#include "wozhost.h"
#include "dosspector.h"
#include "prodosspector.h"
#include "pascalspector.h"
#include "applesoft.h"
#include "intbas.h"
#include "applesingle.h"
#include "bootblock.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>

#include <map>
#include <set>

using namespace std;

// ---- filesystems ----

const char *wozFsName(wozFsType type)
{
  switch (type) {
  case WOZ_FS_DOS33:  return "DOS 3.3";
  case WOZ_FS_PRODOS: return "ProDOS";
  case WOZ_FS_PASCAL: return "Pascal";
  default:            return "unknown";
  }
}

Wozspector *wozNewSpector(wozFsType type, bool verbose)
{
  switch (type) {
  case WOZ_FS_DOS33:  return new DosSpector(verbose);
  case WOZ_FS_PRODOS: return new ProdosSpector(verbose);
  case WOZ_FS_PASCAL: return new PascalSpector(verbose);
  default:            return NULL;
  }
}

static void discardMsg(wozMsgLevel level, const char *msg, void *ctx)
{
  (void)level; (void)msg; (void)ctx;
}

wozFsType wozDetectFs(const char *path)
{
  // Trying the wrong filesystems makes them complain, which is expected
  // here, so nobody hears it, and it isn't left behind as the last error
  wozMsgSink sink;
  void *ctx;
  wozGetMsgSink(&sink, &ctx);
  wozSetMsgSink(discardMsg, NULL);

  static const wozFsType order[] = { WOZ_FS_PRODOS, WOZ_FS_DOS33, WOZ_FS_PASCAL };
  wozFsType found = WOZ_FS_UNKNOWN;
  for (size_t i = 0; i < sizeof(order)/sizeof(order[0]) && !found; i++) {
    Wozspector *s = wozNewSpector(order[i], false);
    if (s->readFile(path, true) && s->probe())
      found = order[i];
    delete s;
  }

  wozSetMsgSink(sink, ctx);
  wozClearError();
  return found;
}

Wozspector *wozOpenFs(const char *path, wozFsType type, bool verbose)
{
  if (type == WOZ_FS_UNKNOWN) {
    type = wozDetectFs(path);
    if (type == WOZ_FS_UNKNOWN) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' doesn't look like a DOS 3.3, "
             "ProDOS or Pascal volume", path);
      return NULL;
    }
  }
  Wozspector *s = wozNewSpector(type, verbose);
  if (!s) return NULL;
  if (!s->readFile(path, true)) {
    delete s;
    return NULL;
  }
  return s;
}

// ---- blank images ----

int32_t wozParseSizeSpec(const char *tok)
{
  // Presets are the sizes real devices presented; ProDOS itself accepts any
  // block count up to 65535 (total_blocks is 16 bits), so bare numbers are
  // allowed too
  static const struct { const char *name; uint32_t blocks; } presets[] = {
    { "140k", 280 },    // 5.25" floppy
    { "800k", 1600 },   // 3.5" floppy
    { "4m",   8192 },   // round hard-disk sizes, handy for Pascal volumes
    { "5m",   9728 },   // ProFile 5MB
    { "8m",   16384 },
    { "10m",  19456 },  // ProFile 10MB
    { "16m",  32768 },  // practical Apple Pascal ceiling
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

bool wozNormalizeProdosVolumeName(const char *src, char out[16])
{
  size_t nl = strlen(src);
  if (nl < 1 || nl > 15) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: volume name must be 1-15 characters");
    return false;
  }
  memset(out, 0, 16);
  for (size_t i = 0; i < nl; i++) {
    char c = (char)toupper((unsigned char)src[i]);
    bool okc = (i == 0) ? isalpha((unsigned char)c)
                        : (isalnum((unsigned char)c) || c == '.');
    if (!okc) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: bad volume name '%s' (letter first, "
             "then letters, digits, or periods)", src);
      return false;
    }
    out[i] = c;
  }
  return true;
}

static bool hasExtension(const char *fname, const char *ext)
{
  const char *dot = strrchr(fname, '.');
  return dot && !strcasecmp(dot, ext);
}

bool wozCheckImageName(wozFsType type, const char *fname, uint32_t blocks)
{
  if (type == WOZ_FS_PASCAL) {
    // A Pascal image is raw ProDOS-order blocks either way. A .po is read
    // back through the 5.25" floppy loader at exactly 280 blocks, and
    // through the raw-block path above that (woz.cpp promotes any .po
    // bigger than a floppy), so .po covers real 5.25" and 3.5" media and
    // nothing outside that range. Note .dsk is deliberately not accepted:
    // it forces DOS sector order, which would scramble the blocks.
    bool po  = hasExtension(fname, ".po");
    bool hdv = hasExtension(fname, ".hdv") || hasExtension(fname, ".img");
    bool poOk = (blocks >= 280 && blocks <= 1600);
    if (!hdv && !(po && poOk)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: a %u-block Pascal image must be named %s",
             (unsigned)blocks, poOk ? ".po, .hdv or .img" : ".hdv or .img");
      return false;
    }
    if (blocks > 32768) {
      wozMsg(WOZ_MSG_WARNING, "WARNING: %u blocks is past the 16MB "
             "(32768-block) practical Apple Pascal volume limit, and a flat "
             "77-file directory can't usefully fill even that.",
             (unsigned)blocks);
    }
  } else if (type == WOZ_FS_PRODOS && blocks != 280 &&
             !hasExtension(fname, ".hdv") && !hasExtension(fname, ".img")) {
    // Only a 280-block image round-trips through the .po/.dsk floppy
    // loaders; anything else must be named for the raw-block HDV path.
    wozMsg(WOZ_MSG_ERROR, "ERROR: a %u-block image must be named .hdv or .img",
           (unsigned)blocks);
    return false;
  }
  return true;
}

// Lay down a freshly-formatted DOS 3.3 filesystem into the provided
// 140 KB buffer (which must already be zeroed). Track 17/0 gets the
// VTOC; sectors 17/15 down to 17/1 form the catalog chain with all
// entries zero.
static void buildBlankDosDsk(uint8_t volumeNumber, uint8_t *img)
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

  // Catalog chain: sector 15 -> 14 -> ... -> 1 -> (0,0). Only the link
  // bytes need to be set; the 7 file entries per sector stay zero.
  for (int s = 15; s >= 1; s--) {
    uint8_t *sec = img + 17 * trackBytes + s * 256;
    sec[1] = (s > 1) ? 17 : 0;
    sec[2] = (s > 1) ? (uint8_t)(s - 1) : 0;
  }
}

// Lay down a blank ProDOS volume of totalBlocks 512-byte blocks in the
// provided (zeroed) buffer. Blocks 0-1 (boot) stay zeros; blocks 2-5 are
// a 4-block volume directory chain; the volume bitmap starts at block 6
// and spans ceil(totalBlocks/4096) blocks (1 for a floppy, 16 for a
// 65535-block hard drive).
static void buildBlankProdosPo(const char *volumeName, uint8_t *img,
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
  // bitmap itself. Bits at or past totalBlocks stay 0, because those
  // blocks don't exist and must be marked unavailable.
  uint8_t *bm = img + 6 * 512;
  uint32_t bitmapBlocks = (totalBlocks + 4095) / 4096;
  for (uint32_t b = 6 + bitmapBlocks; b < totalBlocks; b++) {
    bm[b / 8] |= (uint8_t)(0x80 >> (b % 8));
  }
}

static inline void wrle16(uint8_t p[2], uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

// Lay down a blank UCSD Pascal volume of totalBlocks 512-byte blocks in
// the provided (zeroed) buffer. Blocks 0-1 (boot) stay zeros; blocks 2-5
// are the flat 4-block directory, of which only the 26-byte volume header
// at the very start of block 2 is non-zero. fileCount = 0 is what marks
// the volume empty: nothing past it is ever scanned.
//
// There is no allocation bitmap to build. Pascal free space is simply the
// gaps between contiguous files, so a blank volume is one unbroken run
// from block 6 to the end, and cpin has nothing to work around.
static void buildBlankPascal(const char *volumeName, uint8_t *img,
                             uint32_t totalBlocks)
{
  struct _pascalVolHdr *h = (struct _pascalVolHdr *)(img + 2 * 512);
  wrle16(h->firstBlock, 0);
  wrle16(h->nextBlock, 6);   // first block past the 4-block directory
  wrle16(h->fileType, 0);

  size_t nl = strlen(volumeName);  // already validated to 1..7, upper-cased
  h->name[0] = (uint8_t)nl;
  memcpy(&h->name[1], volumeName, nl);  // bytes past nl stay 0 from calloc

  wrle16(h->volBlockCount, (uint16_t)totalBlocks);
  wrle16(h->fileCount, 0);
  wrle16(h->lastAccess, 0);
  wrle16(h->lastDateSet, PascalSpector::today());
  // reserved[4] stays 0, as do blocks 0-1 and 3-5.
}

// Copy the embedded p-System bootstrap that matches this volume's device
// class into b0/b1. Pascal has no one-size bootstrap the way ProDOS does:
// a 5.25" volume boots through the Disk II controller and needs both
// blocks, while a 3.5" or hard-disk volume boots through the slot's
// ProDOS block-driver entry point and fits entirely in block 0.
static void embeddedPascalBoot(uint32_t totalBlocks, uint8_t b0[512],
                               uint8_t b1[512])
{
  if (totalBlocks == 280) {
    memcpy(b0, pascalDiskIIBootBlock0, 512);
    memcpy(b1, pascalDiskIIBootBlock1, 512);
  } else {
    memcpy(b0, pascalBlockDevBootBlock, 512);
    memset(b1, 0, 512);
  }
}

bool wozBuildBlankImage(const WozFormatOptions *opts, uint8_t **imgOut,
                        uint32_t *sizeOut, char volumeNameOut[16],
                        int *dosVolumeOut)
{
  *imgOut = NULL;
  *sizeOut = 0;

  bool dos = (opts->type == WOZ_FS_DOS33);
  bool pascal = (opts->type == WOZ_FS_PASCAL);
  if (!dos && !pascal && opts->type != WOZ_FS_PRODOS) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: no filesystem given to format");
    return false;
  }
  if (dos && opts->bootable) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: 'bootable' is ProDOS-only (DOS 3.3 boots "
           "from the DOS image on tracks 0-2, which format doesn't install)");
    return false;
  }

  uint32_t blocks = dos ? 280 : opts->blocks;
  const uint32_t minBlocks = pascal ? 6 : 5;
  if (blocks < minBlocks || blocks > 0xFFFF) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: bad size %u (a block count %u-65535)",
           (unsigned)blocks, (unsigned)minBlocks);
    return false;
  }

  // Validate/normalize the volume name before building anything. vol[] is
  // sized for ProDOS; Pascal needs only 8 bytes.
  char vol[16] = {0};
  const char *volName = opts->volumeName ? opts->volumeName : "BLANK";
  if (pascal) {
    if (!PascalSpector::normalizeVolumeName(volName, vol))
      return false;
  } else if (!dos) {
    if (!wozNormalizeProdosVolumeName(volName, vol))
      return false;
  }
  int dosVolume = opts->dosVolume;
  if (dosVolume < 1 || dosVolume > 254) dosVolume = 254;

  const uint32_t diskSize = blocks * 512;
  uint8_t *img = (uint8_t *)wozCalloc(1, diskSize);
  if (!img) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: out of memory");
    return false;
  }

  if (dos) {
    buildBlankDosDsk((uint8_t)dosVolume, img);
  } else if (pascal) {
    buildBlankPascal(vol, img, blocks);
    // The bootstrap that fits this size, straight into blocks 0-1. The
    // volume boots once the SYSTEM.* files are copied in.
    if (opts->bootable)
      embeddedPascalBoot(blocks, img, img + 512);
  } else {
    buildBlankProdosPo(vol, img, blocks);
    // Standard ProDOS boot code in block 0; block 1 stays zero. The
    // volume boots once PRODOS and a .SYSTEM file are copied in.
    if (opts->bootable)
      memcpy(img, prodosBootBlock, 512);
  }

  if (volumeNameOut) memcpy(volumeNameOut, vol, 16);
  if (dosVolumeOut) *dosVolumeOut = dosVolume;
  *imgOut = img;
  *sizeOut = diskSize;
  return true;
}

// ---- boot blocks ----

bool wozLoadProdosBootBlocks(const char *donor, uint8_t b0[512],
                             uint8_t b1[512])
{
  if (!donor || !donor[0]) {
    memcpy(b0, prodosBootBlock, 512);
    memset(b1, 0, 512);
    return true;
  }

  ProdosSpector d(false);
  if (!d.readFile(donor, true)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: can't read boot donor image '%s'", donor);
    return false;
  }
  if (!d.probe()) {
    // A DOS 3.3 boot sector also starts with $01, so without this check a
    // DOS donor would pass the magic-byte test below and produce an
    // unbootable mongrel.
    wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' doesn't look like a ProDOS volume; "
           "refusing to take boot code from it", donor);
    return false;
  }
  if (!d.readBootBlocks(b0, b1)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: can't read boot blocks from '%s' (not a "
           "ProDOS image?)", donor);
    return false;
  }
  if (b0[0] != 0x01) {
    // Every Apple II boot sector/block starts with $01. Refuse rather than
    // install something that certainly won't boot.
    wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' block 0 doesn't look like boot code "
           "(first byte 0x%02X, not 0x01)", donor, b0[0]);
    return false;
  }
  return true;
}

bool wozLoadPascalBootBlocks(const char *donor, uint32_t totalBlocks,
                             uint8_t b0[512], uint8_t b1[512])
{
  if (!donor || !donor[0]) {
    embeddedPascalBoot(totalBlocks, b0, b1);
    return true;
  }

  // A donor volume, for when the embedded blocks aren't what the user
  // wants (a different p-System release, or a patched bootstrap)
  PascalSpector d(false);
  if (!d.readFile(donor, true)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: can't read boot donor image '%s'", donor);
    return false;
  }
  if (!d.probe()) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' doesn't look like a Pascal volume; "
           "refusing to take boot code from it", donor);
    return false;
  }
  if (!d.readBootBlocks(b0, b1)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: can't read boot blocks from '%s'", donor);
    return false;
  }
  if (b0[0] != 0x01) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' block 0 doesn't look like boot code "
           "(first byte 0x%02X, not 0x01)", donor, b0[0]);
    return false;
  }
  return true;
}

bool wozInstallBootBlocks(Wozspector *fs, const char *donor)
{
  uint8_t b0[512], b1[512];

  if (fs->fsType() == WOZ_FS_PASCAL) {
    // Which embedded bootstrap applies depends on how the finished volume
    // will be read, so ask the volume how big it is
    uint32_t blocks = 0;
    if (!donor || !donor[0]) {
      blocks = ((PascalSpector *)fs)->volumeBlocks();
      if (!blocks) {
        wozMsg(WOZ_MSG_ERROR, "ERROR: can't determine the volume size");
        return false;
      }
    }
    if (!wozLoadPascalBootBlocks(donor, blocks, b0, b1))
      return false;
  } else {
    // DOS 3.3 gets this far too, and writeBootBlocks says it can't
    if (!wozLoadProdosBootBlocks(donor, b0, b1))
      return false;
  }

  if (!fs->writeBootBlocks(b0, b1)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write boot blocks");
    return false;
  }
  return true;
}

// ---- files in and out ----

// AppleSingle fields are big-endian
static uint32_t be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
  return (uint16_t)((p[0] << 8) | p[1]);
}

bool wozImportFile(Wozspector *fs, const uint8_t *data, uint32_t len,
                   const char *destName, const WozImportOptions *opts)
{
  const char *srcName = (opts && opts->sourceName) ? opts->sourceName
                                                   : destName;
  bool tokenize = opts && opts->tokenize;
  bool explicitType = opts && opts->explicitType;
  bool explicitAux = opts && opts->explicitAux;
  uint8_t filetype = explicitType ? opts->fileType : 0x06; // ProDOS types
  uint16_t auxTypeData = explicitAux ? opts->auxType : 0x2000;

  // writeFileToImage takes a mutable name
  char destfn[256];
  if (strlen(destName) >= sizeof(destfn)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: destination name is too long");
    return false;
  }
  strcpy(destfn, destName);

  // ProDOS stores EOF in 24 bits, so a file tops out at 16MB-1. Refuse
  // anything bigger rather than silently truncating it.
  if (len > 0xFFFFFF) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' is %u bytes; ProDOS files max out "
           "at 16MB-1", srcName, (unsigned)len);
    return false;
  }

  // What gets written: the host bytes, unless tokenizing or AppleSingle
  // says otherwise
  const uint8_t *payload = data;
  uint32_t payloadSize = len;
  uint8_t *tokBuf = NULL;
  const uint8_t *resourceFork = NULL; // set only by an AppleSingle resource fork
  uint32_t resourceSize = 0;

  // An Applesoft listing rather than a binary: tokenize it into the
  // program image the Apple II expects. This works for DOS 3.3 and ProDOS
  // alike; the 2-byte length header a DOS 3.3 'A' file carries is added by
  // the filesystem layer, not here.
  if (tokenize) {
    if (explicitType && filetype == FT_INT) {
      // Integer BASIC's tokens are context-dependent (three different PRINT
      // tokens, a dozen commas), so tokenizing it means reimplementing the
      // interpreter's grammar. cpout -B lists Integer programs; cpin -B
      // can't write them yet.
      wozMsg(WOZ_MSG_ERROR, "ERROR: cpin -B can't tokenize Integer BASIC, "
             "only Applesoft");
      return false;
    }
    if (explicitType && filetype != FT_BAS) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: cpin -B writes an Applesoft (BAS, $FC) "
             "file; drop the explicit type or pass FC");
      return false;
    }
    uint32_t tokSize = 0;
    tokBuf = tokenizeApplesoft((const char *)data, len, &tokSize);
    if (!tokBuf) {
      // tokenizeApplesoft() already said which line was at fault
      return false;
    }
    payload = tokBuf;
    payloadSize = tokSize;
    filetype = FT_BAS;
    // ProDOS records an Applesoft program's load address in the aux type
    if (!explicitAux) auxTypeData = APPLESOFT_LOAD_ADDR;
  }

  // An AppleSingle file: a 26-byte header, then 12-byte entry descriptors
  // naming the pieces (data fork, resource fork, ProDOS file info)
  bool foundAppleSingle = false;
  bool foundMetadata = false;
  if (!tokenize && len >= sizeof(applesingle) &&
      be32(data) == 0x51600 && be32(data + 4) == 0x20000) {
    foundAppleSingle = true;
    for (int i = 0; i < 16; i++) {
      if (data[8 + i] != 0) {
        wozMsg(WOZ_MSG_ERROR, "ERROR: Filler is NOT BLANK");
        foundAppleSingle = false;
        break;
      }
    }
    uint16_t numEntries = foundAppleSingle ? be16(data + 24) : 0;
    if (26 + (uint32_t)numEntries * 12 > len) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' has more AppleSingle entries than "
             "it has room for", srcName);
      return false;
    }
    for (int i = 0; i < numEntries; i++) {
      const uint8_t *ent = data + 26 + i * 12;
      uint32_t entryID = be32(ent);
      uint32_t offset = be32(ent + 4);
      uint32_t length = be32(ent + 8);
      if (fs->isVerbose()) {
        wozMsg(WOZ_MSG_DEBUG, "ENTRY %d:\n  entryID: %u\n  offset: %u\n"
               "  length: %u", i, (unsigned)entryID, (unsigned)offset,
               (unsigned)length);
      }
      if (offset > len || length > len - offset) {
        wozMsg(WOZ_MSG_ERROR, "ERROR: AppleSingle entry %d of '%s' runs past "
               "the end of the file", i, srcName);
        wozFree(tokBuf);
        return false;
      }

      if (entryID == as_prodosFileInfo) {
        if (length < sizeof(type11prodos)) {
          wozMsg(WOZ_MSG_ERROR, "ERROR: AppleSingle ProDOS file info in '%s' "
                 "is too short", srcName);
          return false;
        }
        // access (2 bytes), file type (2), aux type (4)
        const uint8_t *p = data + offset;
        if (fs->isVerbose()) {
          wozMsg(WOZ_MSG_DEBUG, "    Access: 0x%X\n    FileType: 0x%X\n"
                 "    AuxType: 0x%X", be16(p), be16(p + 2),
                 (unsigned)be32(p + 4));
        }
        foundMetadata = true;
        // An explicit type/aux overrides the AppleSingle metadata;
        // otherwise adopt what the metadata declares
        if (!explicitType) filetype = (uint8_t)be16(p + 2);
        if (!explicitAux)  auxTypeData = (uint16_t)be32(p + 4);
      } else if (entryID == as_dataFork) {
        payload = data + offset;
        payloadSize = length;
      } else if (entryID == as_resourceFork) {
        // A resource fork makes this a GS/OS extended file on the image:
        // two forks described by an extended key block
        resourceFork = data + offset;
        resourceSize = length;
      } else {
        wozMsg(WOZ_MSG_ERROR, "ERROR: unknown entry ID type");
        foundAppleSingle = false;
        break;
      }
    }
  }

  // If nothing pinned a type (no explicit one, no AppleSingle metadata) but
  // the destination is named like a ProDOS system program, make it a
  // launchable SYS file loading at $2000 (the convention for .SYSTEM
  // programs). An explicit aux still wins if one was given.
  if (!explicitType && !foundMetadata) {
    size_t dl = strlen(destfn);
    const char *suffix = ".SYSTEM";
    size_t sl = strlen(suffix);
    if (dl >= sl && strcmp(destfn + dl - sl, suffix) == 0) {
      filetype = 0xFF; // SYS
      if (!explicitAux) auxTypeData = 0x2000;
    }
  }

  // An AppleSingle file carrying a resource fork becomes a GS/OS extended
  // file. Only ProDOS has those; anywhere else the resource fork has no
  // home, so say so rather than dropping it silently.
  bool wantExtended = (foundAppleSingle && resourceFork);
  if (wantExtended && fs->fsType() != WOZ_FS_PRODOS) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' has a resource fork, which only "
           "ProDOS images can store", srcName);
    wozFree(tokBuf);
    return false;
  }

  // If the destination already exists, remove it first. writeFileToImage
  // doesn't replace in place (it just appends a fresh catalog entry), so
  // without this a repeated cpin leaves stale duplicate entries behind.
  if (fs->findEntry(destfn)) {
    fs->removeFile(destfn);
  }

  bool ok;
  if (wantExtended) {
    struct _prodosFent meta;
    memset(&meta, 0, sizeof(meta));
    meta.fileType = filetype;
    meta.accessFlags = 0xC3; // destroy/rename/write/read, as cpin always sets
    meta.typeData[0] = auxTypeData & 0xFF;
    meta.typeData[1] = (auxTypeData >> 8);
    ok = ((ProdosSpector *)fs)->writeExtendedFileWithMeta(
           payload, payloadSize, resourceFork, resourceSize, destfn, &meta,
           NULL);
  } else {
    // The filesystems copy what they're given before changing it
    ok = fs->writeFileToImage((uint8_t *)payload, destfn, filetype,
                              auxTypeData, payloadSize);
  }
  wozFree(tokBuf);
  if (!ok) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: Failed to write file");
  }
  return ok;
}

bool wozExportFile(Wozspector *fs, Vent *e, const WozExportOptions *opts,
                   const char *label, uint8_t **out, uint32_t *lenOut)
{
  bool wantResourceFork = opts && opts->resourceFork;
  bool useAllocation = opts && opts->fullAllocation;
  if (!label) label = e->getName();
  *out = NULL;
  *lenOut = 0;

  bool isExtended = (fs->fsType() == WOZ_FS_PRODOS) &&
                    ProdosSpector::isExtendedFile(e);
  if (wantResourceFork && !isExtended) {
    wozMsg(WOZ_MSG_ERROR, "'%s' has no resource fork (it isn't a GS/OS "
           "extended file)", label);
    return false;
  }

  char *dat = NULL;
  uint32_t s;
  if (isExtended) {
    // Each fork is its own seedling/sapling/tree, so the whole-allocation
    // reading of the file is the fork's allocation
    ProdosSpector *ps = (ProdosSpector *)fs;
    s = ps->getForkContents(e, wantResourceFork, &dat);
    if (useAllocation) {
      ProdosFork f;
      if (ps->getForkInfo(e, wantResourceFork, &f) && f.storageType &&
          (uint32_t)f.blocksUsed * 512 > s) {
        wozMsg(WOZ_MSG_WARNING, "WARNING: -A can't extend a fork past its "
               "recorded length; copying '%s' as %u bytes.", label,
               (unsigned)s);
      }
    }
  } else if (useAllocation) {
    s = fs->getFileAllocation(e, &dat);
  } else {
    s = fs->getFileContents(e, &dat);
    // Warn if the directory-length version is far smaller than what the
    // disk actually allocates: the classic stub loader (TAIPAN does it).
    // Threshold picked to avoid noise on normal files where the program
    // ends part-way into its last sector: flag only when the allocated
    // size exceeds the logical size by more than 1 KB.
    uint32_t allocated = fs->getAllocatedByteCount(e);
    if (allocated > s && allocated - s > 1024) {
      wozMsg(WOZ_MSG_WARNING, "WARNING: '%s' copied as %u bytes (directory "
             "length), but %u bytes are allocated on disk. Re-run with -A to "
             "copy the full allocation.", label, (unsigned)s,
             (unsigned)allocated);
    }
  }

  if (!s || !dat) {
    wozFree(dat);
    return true; // empty
  }
  *out = (uint8_t *)dat;
  *lenOut = s;
  return true;
}

bool wozIsBasicFile(Vent *e)
{
  uint8_t ft = e->getFileType();
  return ft == FT_BAS || ft == FT_INT;
}

bool wozDetokenize(Wozspector *fs, Vent *e, const uint8_t *data,
                   uint32_t len, BasicListing *out)
{
  // The listers don't change the program they're handed, and contents
  // never carry DOS 3.3's length header, so there's nothing to skip
  (void)fs;
  uint8_t *buf = (uint8_t *)data;
  if (e->getFileType() == FT_INT) {
    IntegerLister l;
    return l.listFile(buf, len, 0, out);
  }
  ApplesoftLister l;
  return l.listFile(buf, len, 0, out);
}

string wozFormatBasicListing(const BasicListing &lines, bool integerBasic)
{
  string s;
  char num[8];
  for (size_t i = 0; i < lines.size(); i++) {
    snprintf(num, sizeof(num), integerBasic ? "%5u " : "%u ",
             (unsigned)lines[i].lineNumber);
    s += num;
    s += lines[i].text;
    s += '\n';
  }
  return s;
}

// ---- image to image ----

struct CopyItem {
  Vent *ent;
  string path;
};

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
        wozMsg(WOZ_MSG_WARNING, "WARNING: directory '%s' has no readable "
               "contents (corrupt image?); copying it empty", path.c_str());
        continue;
      }
      collectProdos(sub->second, path, groupByKey, dirs, files);
    } else {
      CopyItem ci = { p, path };
      files.push_back(ci);
    }
  }
}

// True if `path` equals `sel` or lies underneath it (path-component-wise)
static bool underSelection(const string &path, const string &sel)
{
  if (path == sel) return true;
  return path.size() > sel.size() &&
         path.compare(0, sel.size(), sel) == 0 &&
         path[sel.size()] == '/';
}

bool wozNormalizeDestDir(string *dir)
{
  string &d = *dir;
  while (!d.empty() && d[0] == '/')
    d.erase(0, 1);
  while (!d.empty() && d[d.size() - 1] == '/')
    d.erase(d.size() - 1);
  for (size_t i = 0; i < d.size(); i++)
    d[i] = toupper((unsigned char)d[i]);
  if (d.empty()) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: the destination directory needs a name");
    return false;
  }
  // Catch an unusable name here rather than partway through a copy
  size_t start = 0;
  while (start <= d.size()) {
    size_t slash = d.find('/', start);
    size_t end = (slash == string::npos) ? d.size() : slash;
    string comp = d.substr(start, end - start);
    if (comp.empty() || comp.size() > 15) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' isn't a usable ProDOS directory "
             "name (each part must be 1 to 15 characters)", d.c_str());
      return false;
    }
    if (slash == string::npos) break;
    start = slash + 1;
  }
  return true;
}

bool wozCopyFiles(Wozspector *src, Wozspector *dst,
                  const WozCopyOptions *opts, WozCopyResult *result)
{
  result->files = 0;
  result->dirsMade = 0;
  result->bytes = 0;
  result->collisions.clear();

  wozFsType mode = src->fsType();
  if (dst->fsType() != mode) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: can't copy between %s and %s volumes",
           wozFsName(mode), wozFsName(dst->fsType()));
    return false;
  }
  bool prodos = (mode == WOZ_FS_PRODOS);

  string destPrefix = opts->destDir;
  if (!destPrefix.empty()) {
    if (!prodos) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: neither DOS 3.3 nor Pascal has "
             "directories to copy into");
      return false;
    }
    if (!wozNormalizeDestDir(&destPrefix))
      return false;
  }

  vector<string> selections;
  for (size_t i = 0; i < opts->selections.size(); i++) {
    string s = opts->selections[i];
    // Strip a leading slash; paths are volume-root relative
    if (!s.empty() && s[0] == '/') s = s.substr(1);
    if (mode != WOZ_FS_DOS33) {
      // ProDOS and Pascal names are upper case on disk
      for (size_t j = 0; j < s.size(); j++) s[j] = toupper((unsigned char)s[j]);
    }
    if (!s.empty()) selections.push_back(s);
  }

  // ---- enumerate the source ----
  // An empty DOS 3.3 catalog legitimately yields a NULL tree (there are no
  // entries and no header node); an empty ProDOS volume still has its
  // volume-header node, so NULL there means the directory didn't parse.
  Vent *tree = src->getTree();
  if (!tree && prodos) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: can't read the source directory");
    return false;
  }

  vector<string> allDirs;
  vector<CopyItem> allFiles;
  if (prodos) {
    map<uint16_t, Vent *> groupByKey;
    for (Vent *p = tree; p; p = p->nextEnt()) {
      if (p->isHeader()) groupByKey[p->keyPointerVal()] = p;
    }
    if (!tree->isHeader()) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: unexpected tree shape (no volume header)");
      return false;
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
        wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' not found on the source volume",
               sel.c_str());
        return false;
      }
    }
  }

  // Relocate the whole copy under the destination directory. From here on
  // the paths in dirs/files name destination locations only; the source
  // side is done with (file contents come from the Vent, not from a path
  // lookup). The prefix's own components get picked up by the
  // parent-prefix pass below.
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
  // The destination directory needs to exist even when the copy is nothing
  // but empty directories, in which case no file path names it above
  if (!destPrefix.empty()) {
    needDirs.insert(destPrefix);
    for (size_t pos = destPrefix.find('/'); pos != string::npos;
         pos = destPrefix.find('/', pos + 1))
      needDirs.insert(destPrefix.substr(0, pos));
  }
  dirs.assign(needDirs.begin(), needDirs.end()); // sorted: parents first

  // ---- collision check (before touching anything) ----
  for (size_t i = 0; i < dirs.size(); i++) {
    Vent *e = dst->findEntry(dirs[i].c_str());
    if (e && !e->isDirectory()) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' is a directory on the source but a "
             "file on the destination; not copying anything", dirs[i].c_str());
      return false;
    }
  }
  for (size_t i = 0; i < files.size(); i++) {
    Vent *e = dst->findEntry(files[i].path.c_str());
    if (e && e->isDirectory()) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' is a file on the source but a "
             "directory on the destination; not copying anything",
             files[i].path.c_str());
      return false;
    }
    if (e) result->collisions.push_back(files[i].path);
  }
  if (!result->collisions.empty() && !opts->force)
    return false;

  // ---- copy ----
  for (size_t i = 0; i < dirs.size(); i++) {
    if (dst->findEntry(dirs[i].c_str()))
      continue; // exists (as a directory, per the check above)
    if (!dst->makeDirectory(dirs[i].c_str())) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: couldn't create directory '%s'; "
             "destination not modified", dirs[i].c_str());
      return false;
    }
    result->dirsMade++;
  }

  for (size_t i = 0; i < files.size(); i++) {
    Vent *e = files[i].ent;
    const string &path = files[i].path;

    // ProDOS and Pascal: the length-limited contents plus the entry's
    // metadata is a faithful copy. DOS 3.3: copy the raw allocated sector
    // stream instead. The address/length headers some types embed ride
    // along verbatim, and types getFileContents can't even size (TXT, SYS,
    // REL) still copy.
    char *data = NULL;
    uint32_t len = (mode != WOZ_FS_DOS33) ? src->getFileContents(e, &data)
                                          : src->getFileAllocation(e, &data);
    uint8_t dummy = 0;
    uint8_t *payload = len ? (uint8_t *)data : &dummy;

    // A GS/OS extended file carries a second (resource) fork and a block of
    // Finder information alongside its data; all three come across too.
    bool extended = prodos && ProdosSpector::isExtendedFile(e);
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

    bool replaced = false;
    if (opts->force && dst->findEntry(path.c_str())) {
      if (!dst->removeFile(path.c_str())) {
        wozMsg(WOZ_MSG_ERROR, "ERROR: couldn't replace '%s'; destination not "
               "modified", path.c_str());
        wozFree(data);
        wozFree(rdata);
        return false;
      }
      replaced = true;
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
    } else if (prodos) {
      ok = ((ProdosSpector *)dst)->writeFileWithMeta(payload, path.c_str(),
                                                     len, e->getProdosFent());
    } else if (mode == WOZ_FS_PASCAL) {
      ok = ((PascalSpector *)dst)->writeFileWithMeta(payload, path.c_str(),
                                                     len, e->getPascalFent());
    } else {
      // typeAndFlags verbatim carries the type bits and the locked flag
      ok = ((DosSpector *)dst)->writeFileRaw(payload, path.c_str(),
                                             e->getDosFent()->fileTypeAndFlags,
                                             len);
    }
    wozFree(data);
    wozFree(rdata);

    if (!ok) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to copy '%s'; destination not "
             "modified", path.c_str());
      return false;
    }
    if (opts->progress)
      opts->progress(path.c_str(), len, rlen, extended, replaced,
                     opts->progressCtx);
    result->files++;
    result->bytes += len + rlen;
  }
  return true;
}
