#include "pascalspector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <vector>

// Block <-> track/sector maps, identical to ProDOS: a 512-byte block is two
// 256-byte sectors, and the disk uses the ProDOS/Pascal interleave.
const static uint8_t s1map[8] = { 0, 4, 8, 12, 1, 5, 9, 13 };
const static uint8_t s2map[8] = { 2, 6, 10, 14, 3, 7, 11, 15 };
#define trackFromBlock(x) ((x)/8)
#define blockInTrack(x)   ((x)%8)

#define VOL_DIR_START_BLOCK 2
#define DIR_ENTRY_LEN       26
#define MAX_VOL_NAME_LEN     7
#define MAX_FILE_NAME_LEN   15

static inline uint16_t rd16(const uint8_t p[2])
{
  return (uint16_t)(p[0] | (p[1] << 8));
}
static inline void wr16(uint8_t p[2], uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

// Pack a Gregorian date into the 16-bit Pascal form: YYYYYYY DDDDD MMMM.
static uint16_t packPascalDate(int year, int month, int day)
{
  int y = year - 1900;
  if (y < 0) y = 0;
  if (y > 100) y = 100;      // 100 is the "in progress" flag; clamp below
  if (y == 100) y = 99;
  if (month < 1 || month > 12) month = 0;
  if (day < 1 || day > 31) day = 1;
  return (uint16_t)(((y & 0x7F) << 9) | ((day & 0x1F) << 4) | (month & 0x0F));
}

static uint16_t todayPascalDate()
{
  time_t now = time(NULL);
  struct tm *lt = localtime(&now);
  if (!lt) return 0;
  return packPascalDate(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
}

static const char *kindSuffix(uint8_t kind)
{
  switch (kind) {
  case PK_BAD:  return ".BAD";
  case PK_CODE: return ".CODE";
  case PK_TEXT: return ".TEXT";
  case PK_INFO: return ".INFO";
  case PK_DATA: return ".DATA";
  case PK_GRAF: return ".GRAF";
  case PK_FOTO: return ".FOTO";
  default:      return "";
  }
}

PascalSpector::PascalSpector(bool verbose, uint8_t dumpflags)
  : Wozspector(verbose, dumpflags)
{
  trackData = NULL;
  trackDataBytes = 0;
  volBlockCount = 0;
  dirNextBlock = 6;
  loaded = false;
}

PascalSpector::~PascalSpector()
{
  if (trackData) {
    free(trackData);
    trackData = NULL;
  }
}

// -------------------------------------------------------------------------
// Block-level access (mirrors ProdosSpector)
// -------------------------------------------------------------------------

bool PascalSpector::loadBlockBuffer()
{
  if (trackData) {
    free(trackData);
    trackData = NULL;
    trackDataBytes = 0;
  }

  if (isHdv()) {
    trackDataBytes = hdvByteCount();
    trackData = (uint8_t *)malloc(trackDataBytes);
    if (!trackData) return false;
    memcpy(trackData, hdvBuffer(), trackDataBytes);
    return true;
  }

  // 5.25" floppy: 35 tracks x 16 sectors x 256 bytes = 140 KB.
  trackDataBytes = 35 * 256 * 16;
  trackData = (uint8_t *)calloc(trackDataBytes, 1);
  if (!trackData) return false;

  for (int i = 0; i < 35; i++) {
    if (!decodeWozTrackToDsk(i, T_PO, &trackData[i * 256 * 16])) {
      fprintf(stderr, "Failed to read track %d\n", i);
      return false;
    }
  }
  return true;
}

bool PascalSpector::ensureLoaded()
{
  if (loaded && trackData)
    return true;
  if (!loadBlockBuffer())
    return false;

  struct _pascalVolHdr hdr;
  if (!readVolHeader(&hdr))
    return false;

  loaded = true;
  return true;
}

bool PascalSpector::validBlock(uint16_t b)
{
  if ((uint32_t)b * 512 + 512 > trackDataBytes) return false;
  if (volBlockCount && b >= volBlockCount) return false;
  return true;
}

bool PascalSpector::readBlock(uint16_t blockNum, uint8_t out[512])
{
  if (!trackData) return false;
  if ((uint32_t)blockNum * 512 + 512 > trackDataBytes) return false;
  memcpy(out, &trackData[512 * blockNum], 512);
  return true;
}

bool PascalSpector::writeBlock(uint16_t blockNum, const uint8_t data[512])
{
  if (!trackData) return false;
  if ((uint32_t)blockNum * 512 + 512 > trackDataBytes) return false;

  memcpy(&trackData[512 * blockNum], data, 512);

  if (isHdv()) {
    memcpy(hdvBuffer() + 512 * blockNum, data, 512);
    return true;
  }

  // Floppy: push the two halves back through the nibble encoder so
  // Woz::writeFile() emits a correct WOZ/DSK/NIB.
  uint8_t t = trackFromBlock(blockNum);
  uint8_t bidx = blockInTrack(blockNum);
  uint8_t half[256];
  memcpy(half, data, 256);
  if (!encodeWozTrackSector(t, s1map[bidx], half)) return false;
  memcpy(half, data + 256, 256);
  if (!encodeWozTrackSector(t, s2map[bidx], half)) return false;
  return true;
}

// -------------------------------------------------------------------------
// Volume header / directory
// -------------------------------------------------------------------------

bool PascalSpector::readVolHeader(struct _pascalVolHdr *hdrOut)
{
  uint8_t block[512];
  if (!readBlock(VOL_DIR_START_BLOCK, block))
    return false;

  struct _pascalVolHdr *h = (struct _pascalVolHdr *)block;
  uint16_t first = rd16(h->firstBlock);
  uint16_t next  = rd16(h->nextBlock);
  uint16_t ftype = rd16(h->fileType);
  uint8_t  nl    = h->name[0];
  uint16_t blks  = rd16(h->volBlockCount);
  uint16_t fcnt  = rd16(h->fileCount);

  // The classic Apple Pascal directory is fixed: first==0, next==6 (a
  // 4-block / 2048-byte directory), volume type 0.
  if (first != 0 || next != 6 || ftype != 0)
    return false;
  if (nl < 1 || nl > MAX_VOL_NAME_LEN)
    return false;
  if (blks < next)
    return false;
  if (fcnt > 77)
    return false;
  for (int i = 0; i < nl; i++) {
    uint8_t c = h->name[i + 1];
    if (c <= 0x20 || c >= 0x7f)
      return false;
  }

  // Don't trust a block count larger than the image itself.
  uint32_t imageBlocks = trackDataBytes / 512;
  if (blks > imageBlocks) blks = (uint16_t)imageBlocks;

  volBlockCount = blks;
  dirNextBlock = next;
  if (hdrOut) memcpy(hdrOut, block, sizeof(*hdrOut));
  return true;
}

bool PascalSpector::readDirectory(uint8_t *dirBuf)
{
  uint32_t nb = dirNextBlock - VOL_DIR_START_BLOCK;
  for (uint32_t i = 0; i < nb; i++) {
    if (!readBlock((uint16_t)(VOL_DIR_START_BLOCK + i), dirBuf + i * 512))
      return false;
  }
  return true;
}

bool PascalSpector::writeDirectory(const uint8_t *dirBuf)
{
  uint32_t nb = dirNextBlock - VOL_DIR_START_BLOCK;
  for (uint32_t i = 0; i < nb; i++) {
    if (!writeBlock((uint16_t)(VOL_DIR_START_BLOCK + i), dirBuf + i * 512))
      return false;
  }
  return true;
}

// -------------------------------------------------------------------------
// Tree building
// -------------------------------------------------------------------------

Vent *PascalSpector::createTree()
{
  if (tree) {
    freeTree(tree);
    tree = NULL;
  }
  loaded = false;
  if (!ensureLoaded()) {
    fprintf(stderr, "Failed to load Pascal directory\n");
    return NULL;
  }

  uint8_t *dirBuf = (uint8_t *)malloc(dirBytes());
  if (!dirBuf) return NULL;
  if (!readDirectory(dirBuf)) {
    free(dirBuf);
    return NULL;
  }

  struct _pascalVolHdr *h = (struct _pascalVolHdr *)dirBuf;
  uint16_t fileCount = rd16(h->fileCount);
  int maxEnt = (int)(dirBytes() / DIR_ENTRY_LEN) - 1;

  for (int i = 0; i < maxEnt; i++) {
    struct _pascalFent *fe =
      (struct _pascalFent *)(dirBuf + DIR_ENTRY_LEN * (i + 1));
    uint16_t start = rd16(fe->firstBlock);
    uint8_t nl = fe->name[0];
    // Unused / deleted entry (or ran off the end of the list).
    if (start == 0 || nl == 0 || nl > MAX_FILE_NAME_LEN)
      break;
    appendTree(new Vent(fe));
    if ((uint16_t)(i + 1) >= fileCount)
      break;
  }

  free(dirBuf);
  return tree;
}

// -------------------------------------------------------------------------
// Reads
// -------------------------------------------------------------------------

uint32_t PascalSpector::getFileContents(Vent *e, char **toWhere)
{
  *toWhere = NULL;
  if (!e || !e->getIsPascal()) return 0;
  if (!ensureLoaded()) return 0;

  uint16_t start = e->getPascalStartBlock();
  uint16_t next  = e->getPascalNextBlock();
  uint16_t last  = e->getPascalByteCount();
  if (next <= start) return 0;
  uint32_t nblocks = next - start;
  uint32_t size = (nblocks - 1) * 512 + last;
  if (size == 0) return 0;

  uint8_t *buf = (uint8_t *)malloc(size);
  if (!buf) return 0;

  uint32_t copied = 0;
  for (uint32_t b = 0; b < nblocks; b++) {
    uint8_t block[512];
    if (!validBlock((uint16_t)(start + b)) ||
        !readBlock((uint16_t)(start + b), block)) {
      free(buf);
      return 0;
    }
    uint32_t take = (size - copied < 512) ? (size - copied) : 512;
    memcpy(buf + copied, block, take);
    copied += take;
  }

  *toWhere = (char *)buf;
  return size;
}

uint32_t PascalSpector::getFileAllocation(Vent *e, char **toWhere)
{
  // Full contiguous block allocation, ignoring the last-block byte count.
  *toWhere = NULL;
  if (!e || !e->getIsPascal()) return 0;
  if (!ensureLoaded()) return 0;

  uint16_t start = e->getPascalStartBlock();
  uint16_t next  = e->getPascalNextBlock();
  if (next <= start) return 0;
  uint32_t nblocks = next - start;
  uint32_t size = nblocks * 512;

  uint8_t *buf = (uint8_t *)malloc(size);
  if (!buf) return 0;
  for (uint32_t b = 0; b < nblocks; b++) {
    if (!validBlock((uint16_t)(start + b)) ||
        !readBlock((uint16_t)(start + b), buf + b * 512)) {
      free(buf);
      return 0;
    }
  }
  *toWhere = (char *)buf;
  return size;
}

uint32_t PascalSpector::getAllocatedByteCount(Vent *e)
{
  if (!e || !e->getIsPascal()) return 0;
  uint16_t start = e->getPascalStartBlock();
  uint16_t next  = e->getPascalNextBlock();
  if (next <= start) return 0;
  return (uint32_t)(next - start) * 512;
}

Vent *PascalSpector::findEntry(const char *path)
{
  // Skip a leading slash and match case-insensitively.
  if (path && path[0] == '/') path++;
  for (Vent *p = getTree(); p; p = p->nextEnt()) {
    if (!strcasecmp(path, p->getName()))
      return p;
  }
  return NULL;
}

bool PascalSpector::probe()
{
  if (!trackData && !loadBlockBuffer())
    return false;
  struct _pascalVolHdr hdr;
  return readVolHeader(&hdr);
}

// -------------------------------------------------------------------------
// Info / inspect
// -------------------------------------------------------------------------

void PascalSpector::displayInfo()
{
  if (!ensureLoaded()) {
    printf("Not a readable Pascal volume\n");
    return;
  }
  uint8_t block[512];
  if (!readBlock(VOL_DIR_START_BLOCK, block)) return;
  struct _pascalVolHdr *h = (struct _pascalVolHdr *)block;

  char volName[MAX_VOL_NAME_LEN + 1] = {0};
  uint8_t nl = h->name[0];
  if (nl > MAX_VOL_NAME_LEN) nl = MAX_VOL_NAME_LEN;
  memcpy(volName, &h->name[1], nl);

  if (!tree) createTree();

  // Free space = sum of gaps between contiguous files.
  uint16_t prevNext = dirNextBlock;
  uint32_t used = 0, files = 0;
  uint16_t largestGap = 0, gapAt = dirNextBlock;
  for (Vent *p = tree; p; p = p->nextEnt()) {
    uint16_t start = p->getPascalStartBlock();
    if (start > prevNext) {
      uint16_t g = start - prevNext;
      if (g > largestGap) { largestGap = g; gapAt = prevNext; }
    }
    used += p->getBlocksUsed();
    prevNext = p->getPascalNextBlock();
    files++;
  }
  if (volBlockCount > prevNext) {
    uint16_t g = volBlockCount - prevNext;
    if (g > largestGap) { largestGap = g; gapAt = prevNext; }
  }
  // Free = total - (boot blocks 0..1 + directory blocks 2..dirNextBlock-1)
  // - blocks occupied by files. dirNextBlock counts blocks 0..dirNextBlock-1.
  uint32_t freeBlocks = volBlockCount - dirNextBlock - used;

  printf("Pascal volume: %s\n", volName);
  printf("Total blocks:  %u\n", volBlockCount);
  printf("Files:         %u\n", files);
  printf("Blocks used:   %u (files) + %u (boot+dir)\n",
         used, dirNextBlock);
  printf("Blocks free:   %u\n", freeBlocks);
  printf("Largest free contiguous region: %u blocks at block %u\n",
         largestGap, gapAt);
}

void PascalSpector::inspectFile(const char *fileName, Vent *fp)
{
  if (!fp || !fp->getIsPascal()) {
    printf("Not a Pascal file entry\n");
    return;
  }
  uint16_t start = fp->getPascalStartBlock();
  uint16_t next  = fp->getPascalNextBlock();
  printf("Pascal file '%s':\n", fp->getName());
  printf("  kind:            %u (%s)\n", fp->getPascalKind(),
         kindSuffix(fp->getPascalKind()));
  printf("  start block:     %u\n", start);
  printf("  next block:      %u\n", next);
  printf("  block count:     %u\n", (next > start) ? next - start : 0);
  printf("  bytes/last block:%u\n", fp->getPascalByteCount());
  printf("  logical length:  %u bytes\n", fp->getEofLength());
}

// -------------------------------------------------------------------------
// Writes
// -------------------------------------------------------------------------

namespace {
struct PEnt {
  uint8_t raw[DIR_ENTRY_LEN];
  uint16_t start;
};
}

bool PascalSpector::createFile(const uint8_t *contents, const char *fileName,
                               uint32_t fileSize, uint8_t pascalKind,
                               uint16_t modDate)
{
  if (!ensureLoaded())
    return false;

  // Normalize/validate the name: upper-case, 1..15 printable chars, none
  // of the Pascal-reserved punctuation.
  char nm[MAX_FILE_NAME_LEN + 1] = {0};
  size_t nlen = 0;
  for (const char *s = fileName; *s && nlen < MAX_FILE_NAME_LEN; s++) {
    char c = (char)toupper((unsigned char)*s);
    if (c <= 0x20 || (unsigned char)c >= 0x7f ||
        strchr("$=?,[#:", c)) {
      printf("ERROR: '%c' is not allowed in a Pascal file name\n", *s);
      return false;
    }
    nm[nlen++] = c;
  }
  if (nlen == 0) {
    printf("ERROR: empty file name\n");
    return false;
  }
  if (strlen(fileName) > MAX_FILE_NAME_LEN) {
    printf("ERROR: '%s' is longer than %d characters\n",
           fileName, MAX_FILE_NAME_LEN);
    return false;
  }

  uint8_t *dirBuf = (uint8_t *)malloc(dirBytes());
  if (!dirBuf) return false;
  if (!readDirectory(dirBuf)) { free(dirBuf); return false; }

  // Parse existing entries.
  std::vector<PEnt> ents;
  int maxEnt = (int)(dirBytes() / DIR_ENTRY_LEN) - 1;
  for (int i = 0; i < maxEnt; i++) {
    uint8_t *p = dirBuf + DIR_ENTRY_LEN * (i + 1);
    struct _pascalFent *fe = (struct _pascalFent *)p;
    uint16_t start = rd16(fe->firstBlock);
    uint8_t nl = fe->name[0];
    if (start == 0 || nl == 0) break;
    // Reject a duplicate name outright; callers are expected to remove
    // first, but never silently create two entries with the same name.
    char en[MAX_FILE_NAME_LEN + 1] = {0};
    uint8_t cl = nl > MAX_FILE_NAME_LEN ? MAX_FILE_NAME_LEN : nl;
    memcpy(en, &fe->name[1], cl);
    if (!strcasecmp(en, nm)) {
      printf("ERROR: '%s' already exists\n", nm);
      free(dirBuf);
      return false;
    }
    PEnt pe;
    memcpy(pe.raw, p, DIR_ENTRY_LEN);
    pe.start = start;
    ents.push_back(pe);
  }

  if ((int)ents.size() >= maxEnt) {
    printf("ERROR: directory is full (%d files max)\n", maxEnt);
    free(dirBuf);
    return false;
  }

  uint32_t neededBlocks = (fileSize + 511) / 512;
  if (neededBlocks == 0) neededBlocks = 1;

  // Find the largest contiguous free gap. Entries are sorted by start
  // block, so gaps sit between prevNext and each entry's start, plus the
  // tail up to volBlockCount.
  uint16_t prevNext = dirNextBlock;
  uint16_t bestStart = 0, bestSize = 0;
  size_t insertIndex = ents.size();
  for (size_t i = 0; i < ents.size(); i++) {
    uint16_t st = ents[i].start;
    if (st > prevNext) {
      uint16_t g = st - prevNext;
      if (g > bestSize) { bestSize = g; bestStart = prevNext; insertIndex = i; }
    }
    uint16_t en = rd16(((struct _pascalFent *)ents[i].raw)->nextBlock);
    prevNext = en;
  }
  if (volBlockCount > prevNext) {
    uint16_t g = volBlockCount - prevNext;
    if (g > bestSize) { bestSize = g; bestStart = prevNext; insertIndex = ents.size(); }
  }

  if (bestSize < neededBlocks) {
    printf("ERROR: no contiguous free region big enough (need %u blocks, "
           "largest gap is %u). Pascal files must be contiguous; the disk "
           "may need a K)runch.\n", neededBlocks, bestSize);
    free(dirBuf);
    return false;
  }

  // Compute bytes-used in the last block.
  uint16_t lastBytes;
  if (fileSize == 0) {
    lastBytes = 0;
  } else {
    uint32_t rem = fileSize - (neededBlocks - 1) * 512;
    lastBytes = (uint16_t)rem; // 1..512
  }

  // Write the data blocks.
  uint32_t copied = 0;
  for (uint32_t b = 0; b < neededBlocks; b++) {
    uint8_t block[512];
    memset(block, 0, sizeof(block));
    uint32_t take = (fileSize - copied < 512) ? (fileSize - copied) : 512;
    if (take > 0) memcpy(block, contents + copied, take);
    if (!writeBlock((uint16_t)(bestStart + b), block)) {
      printf("ERROR: failed to write data block %u\n", bestStart + b);
      free(dirBuf);
      return false;
    }
    copied += take;
  }

  // Build the new directory entry.
  PEnt ne;
  memset(ne.raw, 0, sizeof(ne.raw));
  struct _pascalFent *nfe = (struct _pascalFent *)ne.raw;
  wr16(nfe->firstBlock, bestStart);
  wr16(nfe->nextBlock, (uint16_t)(bestStart + neededBlocks));
  wr16(nfe->fileType, (uint16_t)(pascalKind & 0x0F));
  nfe->name[0] = (uint8_t)nlen;
  memcpy(&nfe->name[1], nm, nlen);
  wr16(nfe->byteCount, lastBytes);
  wr16(nfe->modDate, modDate);
  ne.start = bestStart;
  ents.insert(ents.begin() + insertIndex, ne);

  // Reserialize the directory: keep the header verbatim except the file
  // count, zero the entry area, then lay the entries back down in order.
  uint8_t *outBuf = (uint8_t *)calloc(dirBytes(), 1);
  if (!outBuf) { free(dirBuf); return false; }
  memcpy(outBuf, dirBuf, DIR_ENTRY_LEN);          // volume header
  wr16(outBuf + 16, (uint16_t)ents.size());       // fileCount at +$10
  for (size_t i = 0; i < ents.size(); i++)
    memcpy(outBuf + DIR_ENTRY_LEN * (i + 1), ents[i].raw, DIR_ENTRY_LEN);

  bool ok = writeDirectory(outBuf);
  free(outBuf);
  free(dirBuf);
  if (!ok) {
    printf("ERROR: failed to write directory\n");
    return false;
  }

  createTree(); // refresh the cached tree
  return true;
}

bool PascalSpector::writeFileToImage(uint8_t *fileContents, char *fileName,
                                     uint8_t fileType, uint16_t auxTypeData,
                                     uint32_t fileSize)
{
  // Pascal has no aux type. Choose the file kind from the destination
  // name's extension first (Pascal ties extensions to kinds), then fall
  // back to a couple of ProDOS Pascal types, then to .DATA.
  uint8_t kind = PK_DATA;
  const char *dot = strrchr(fileName, '.');
  if (dot) {
    if      (!strcasecmp(dot, ".CODE")) kind = PK_CODE;
    else if (!strcasecmp(dot, ".TEXT")) kind = PK_TEXT;
    else if (!strcasecmp(dot, ".DATA")) kind = PK_DATA;
    else if (!strcasecmp(dot, ".INFO")) kind = PK_INFO;
    else if (!strcasecmp(dot, ".GRAF")) kind = PK_GRAF;
    else if (!strcasecmp(dot, ".FOTO")) kind = PK_FOTO;
    else if (!strcasecmp(dot, ".BAD"))  kind = PK_BAD;
  } else {
    if      (fileType == 0x02) kind = PK_CODE; // ProDOS PCD
    else if (fileType == 0x03) kind = PK_TEXT; // ProDOS PTX
  }

  return createFile(fileContents, fileName, fileSize, kind, todayPascalDate());
}

bool PascalSpector::writeFileWithMeta(uint8_t *contents, const char *fileName,
                                      uint32_t fileSize,
                                      const struct _pascalFent *meta)
{
  uint8_t kind = (uint8_t)(rd16(meta->fileType) & 0x0F);
  uint16_t modDate = rd16(meta->modDate);
  return createFile(contents, fileName, fileSize, kind, modDate);
}

bool PascalSpector::removeFile(const char *fileName)
{
  if (!ensureLoaded())
    return false;

  const char *name = fileName;
  if (name[0] == '/') name++;

  uint8_t *dirBuf = (uint8_t *)malloc(dirBytes());
  if (!dirBuf) return false;
  if (!readDirectory(dirBuf)) { free(dirBuf); return false; }

  std::vector<PEnt> ents;
  bool found = false;
  int maxEnt = (int)(dirBytes() / DIR_ENTRY_LEN) - 1;
  for (int i = 0; i < maxEnt; i++) {
    uint8_t *p = dirBuf + DIR_ENTRY_LEN * (i + 1);
    struct _pascalFent *fe = (struct _pascalFent *)p;
    uint16_t start = rd16(fe->firstBlock);
    uint8_t nl = fe->name[0];
    if (start == 0 || nl == 0) break;
    char en[MAX_FILE_NAME_LEN + 1] = {0};
    uint8_t cl = nl > MAX_FILE_NAME_LEN ? MAX_FILE_NAME_LEN : nl;
    memcpy(en, &fe->name[1], cl);
    if (!strcasecmp(en, name)) {
      found = true;
      continue; // drop it
    }
    PEnt pe;
    memcpy(pe.raw, p, DIR_ENTRY_LEN);
    pe.start = start;
    ents.push_back(pe);
  }

  if (!found) {
    printf("File '%s' not found\n", name);
    free(dirBuf);
    return false;
  }

  uint8_t *outBuf = (uint8_t *)calloc(dirBytes(), 1);
  if (!outBuf) { free(dirBuf); return false; }
  memcpy(outBuf, dirBuf, DIR_ENTRY_LEN);
  wr16(outBuf + 16, (uint16_t)ents.size());
  for (size_t i = 0; i < ents.size(); i++)
    memcpy(outBuf + DIR_ENTRY_LEN * (i + 1), ents[i].raw, DIR_ENTRY_LEN);

  bool ok = writeDirectory(outBuf);
  free(outBuf);
  free(dirBuf);
  if (!ok) return false;

  createTree();
  return true;
}

bool PascalSpector::renameVolume(const char *newName)
{
  if (!ensureLoaded())
    return false;

  // Upper-case, validate 1..7 printable chars, none reserved.
  char vn[MAX_VOL_NAME_LEN + 1] = {0};
  size_t vlen = 0;
  for (const char *s = newName; *s; s++) {
    if (vlen >= MAX_VOL_NAME_LEN) {
      printf("ERROR: volume name max %d chars\n", MAX_VOL_NAME_LEN);
      return false;
    }
    char c = (char)toupper((unsigned char)*s);
    if (c <= 0x20 || (unsigned char)c >= 0x7f || strchr("$=?,[#:", c)) {
      printf("ERROR: '%c' not allowed in a Pascal volume name\n", *s);
      return false;
    }
    vn[vlen++] = c;
  }
  if (vlen == 0) {
    printf("ERROR: empty volume name\n");
    return false;
  }

  uint8_t block[512];
  if (!readBlock(VOL_DIR_START_BLOCK, block)) return false;
  struct _pascalVolHdr *h = (struct _pascalVolHdr *)block;
  memset(h->name, 0, sizeof(h->name));
  h->name[0] = (uint8_t)vlen;
  memcpy(&h->name[1], vn, vlen);

  if (!writeBlock(VOL_DIR_START_BLOCK, block))
    return false;

  createTree();
  return true;
}

bool PascalSpector::moveFileBlocks(uint16_t src, uint16_t dst, uint16_t count)
{
  if (src == dst || count == 0)
    return true;

  uint8_t block[512];
  if (dst < src) {
    // Sliding toward the front: destination is below the source, so copy
    // low-to-high - writing dst+i never clobbers a source block not yet read.
    for (uint16_t i = 0; i < count; i++) {
      if (!readBlock((uint16_t)(src + i), block)) return false;
      if (!writeBlock((uint16_t)(dst + i), block)) return false;
    }
  } else {
    // Sliding toward the end: copy high-to-low for the same reason.
    for (int i = count - 1; i >= 0; i--) {
      if (!readBlock((uint16_t)(src + i), block)) return false;
      if (!writeBlock((uint16_t)(dst + i), block)) return false;
    }
  }
  return true;
}

bool PascalSpector::krunch(const char *afterFile)
{
  if (!ensureLoaded())
    return false;

  uint8_t *dirBuf = (uint8_t *)malloc(dirBytes());
  if (!dirBuf) return false;
  if (!readDirectory(dirBuf)) { free(dirBuf); return false; }

  // Parse the file entries in on-disk (start-block) order.
  std::vector<PEnt> ents;
  int maxEnt = (int)(dirBytes() / DIR_ENTRY_LEN) - 1;
  for (int i = 0; i < maxEnt; i++) {
    uint8_t *p = dirBuf + DIR_ENTRY_LEN * (i + 1);
    struct _pascalFent *fe = (struct _pascalFent *)p;
    uint16_t start = rd16(fe->firstBlock);
    uint8_t nl = fe->name[0];
    if (start == 0 || nl == 0) break;
    PEnt pe;
    memcpy(pe.raw, p, DIR_ENTRY_LEN);
    pe.start = start;
    ents.push_back(pe);
  }
  int n = (int)ents.size();

  // Safety pass: the directory must be sorted, in range, and
  // non-overlapping, and we won't relocate a volume that contains a .BAD
  // file - those mark physically damaged sectors that must not be moved
  // onto good data. Refuse without touching anything.
  uint16_t prevNext = dirNextBlock;
  uint32_t usedTotal = 0;
  for (int i = 0; i < n; i++) {
    struct _pascalFent *fe = (struct _pascalFent *)ents[i].raw;
    uint16_t start = rd16(fe->firstBlock);
    uint16_t next  = rd16(fe->nextBlock);
    uint8_t kind   = (uint8_t)(rd16(fe->fileType) & 0x0F);
    char en[MAX_FILE_NAME_LEN + 1] = {0};
    uint8_t cl = fe->name[0] > MAX_FILE_NAME_LEN ? MAX_FILE_NAME_LEN : fe->name[0];
    memcpy(en, &fe->name[1], cl);
    if (kind == PK_BAD) {
      printf("ERROR: '%s' is a .BAD (bad-block) file; refusing to krunch a "
             "volume with damaged blocks.\n", en);
      free(dirBuf);
      return false;
    }
    if (start < dirNextBlock || next <= start || next > volBlockCount ||
        start < prevNext) {
      printf("ERROR: directory entry '%s' is damaged or overlapping "
             "(start=%u next=%u); refusing to krunch.\n", en, start, next);
      free(dirBuf);
      return false;
    }
    usedTotal += (uint16_t)(next - start);
    prevNext = next;
  }

  // Decide where the free space should end up: NULL -> all files pack toward
  // the front (hole at the end). A named file -> files up to and including
  // it pack toward the front, the rest toward the end (hole after it).
  int splitIndex = n;
  if (afterFile && afterFile[0]) {
    const char *want = afterFile;
    if (want[0] == '/') want++;
    int found = -1;
    for (int i = 0; i < n; i++) {
      struct _pascalFent *fe = (struct _pascalFent *)ents[i].raw;
      char en[MAX_FILE_NAME_LEN + 1] = {0};
      uint8_t cl = fe->name[0] > MAX_FILE_NAME_LEN ? MAX_FILE_NAME_LEN : fe->name[0];
      memcpy(en, &fe->name[1], cl);
      if (!strcasecmp(en, want)) { found = i; break; }
    }
    if (found < 0) {
      printf("File '%s' not found\n", want);
      free(dirBuf);
      return false;
    }
    splitIndex = found + 1;
  }

  bool changed = false;

  // Phase 1: pack the low group [0, splitIndex) tight against the front,
  // starting right after the directory.
  uint16_t cursor = dirNextBlock;
  for (int i = 0; i < splitIndex; i++) {
    struct _pascalFent *fe = (struct _pascalFent *)ents[i].raw;
    uint16_t start = rd16(fe->firstBlock);
    uint16_t size  = (uint16_t)(rd16(fe->nextBlock) - start);
    if (start != cursor) {
      if (!moveFileBlocks(start, cursor, size)) {
        printf("ERROR: block move failed; image may be partially modified. "
               "Do not save.\n");
        free(dirBuf);
        return false;
      }
      wr16(fe->firstBlock, cursor);
      wr16(fe->nextBlock, (uint16_t)(cursor + size));
      ents[i].start = cursor;
      changed = true;
    }
    cursor += size;
  }
  uint16_t holeStart = cursor;

  // Phase 2: pack the high group [splitIndex, n) flush against the end of
  // the volume, processing last file first so we never overwrite a file we
  // have already placed.
  cursor = volBlockCount;
  for (int i = n - 1; i >= splitIndex; i--) {
    struct _pascalFent *fe = (struct _pascalFent *)ents[i].raw;
    uint16_t start = rd16(fe->firstBlock);
    uint16_t size  = (uint16_t)(rd16(fe->nextBlock) - start);
    uint16_t newStart = (uint16_t)(cursor - size);
    if (start != newStart) {
      if (!moveFileBlocks(start, newStart, size)) {
        printf("ERROR: block move failed; image may be partially modified. "
               "Do not save.\n");
        free(dirBuf);
        return false;
      }
      wr16(fe->firstBlock, newStart);
      wr16(fe->nextBlock, (uint16_t)(newStart + size));
      ents[i].start = newStart;
      changed = true;
    }
    cursor = newStart;
  }

  uint32_t freeBlocks = volBlockCount - dirNextBlock - usedTotal;

  if (!changed) {
    printf("Free space is already contiguous (%u blocks at block %u); "
           "nothing to do.\n", freeBlocks, holeStart);
    free(dirBuf);
    return true;
  }

  // Rewrite the directory. Entry order is unchanged - the low group still
  // has ascending start blocks below the high group's - so Pascal's
  // sorted-by-start invariant still holds.
  uint8_t *outBuf = (uint8_t *)calloc(dirBytes(), 1);
  if (!outBuf) { free(dirBuf); return false; }
  memcpy(outBuf, dirBuf, DIR_ENTRY_LEN);   // volume header, file count intact
  for (int i = 0; i < n; i++)
    memcpy(outBuf + DIR_ENTRY_LEN * (i + 1), ents[i].raw, DIR_ENTRY_LEN);

  bool ok = writeDirectory(outBuf);
  free(outBuf);
  free(dirBuf);
  if (!ok) {
    printf("ERROR: failed to write directory\n");
    return false;
  }

  createTree();
  printf("Krunched: %u free block%s now contiguous at block %u; "
         "use 'save' to make it permanent.\n",
         freeBlocks, freeBlocks == 1 ? "" : "s", holeStart);
  return true;
}
