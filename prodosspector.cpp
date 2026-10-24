#include "prodosspector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Prodos disk map parser

#include "vent.h"
#include "wozmsg.h"

#include <stack>
#include <set>
#include <map>
#include <vector>
using namespace std;

struct _idxHeader {
  uint8_t prevBlock[2];
  uint8_t nextBlock[2];
};

// block to sector maps (2 sectors for each block; block 0 is on
// sectors 0 and 2, while block 1 is on sectors 4 and 6, etc.)
const static uint8_t s1map[8] = { 0, 4, 8, 12, 1, 5, 9, 13 };
const static uint8_t s2map[8] = { 2, 6, 10, 14, 3, 7, 11, 15 };
// Convert to a track number from a block number -- 8 blocks per track
#define trackFromBlock(x) ((x)/8)
// Convert to an index in s1map/s2map from a block, so we can convert to
// sector numbers based on those arrays
#define blockInTrack(x) ((x)%8)

// file entry type flags
enum {
  ft_deleted   = 0x00,
  ft_seedling  = 0x01,
  ft_sapling   = 0x02,
  ft_tree      = 0x03,
  ft_pascal    = 0x04,
  ft_extended  = 0x05,
  ft_subdir    = 0x0D,
  ft_subdirhdr = 0x0E,
  ft_voldirhdr = 0x0F
};

// access type flags
enum {
  at_destroyed = 0x80,
  at_renamed   = 0x40,
  at_backup    = 0x20,
  at_written   = 0x02,
  at_read      = 0x01
};

#if 0
void dumpFent(struct _prodosFent *e)
{
  if (!e->fileType)
    return;
  char name[16];
  memset(name, 0, sizeof(name));
  strncpy(name, e->name, 15);
  printf("Name: '%s'\n", name);
  printf("Type: 0x%X\n", e->fileType);
  printf("KeyPtr: %.2X %.2X\n", e->keyPointer[0], e->keyPointer[1]);
  printf("BlocksUsed: %.2X %.2X\n", e->blocksUsed[0], e->blocksUsed[1]);
  printf("eofLen: %.2X %.2X %.2X\n", e->eofLength[0], e->eofLength[1], e->eofLength[2]);
  printf("creation: %.2X %.2X %.2X %.2X\n", e->creationDate[0], e->creationDate[1], e->creationDate[2], e->creationDate[3]);
  printf("creatorVerison: %.2X\n", e->creatorVersion);
  printf("minRequiredVersion: %.2X\n", e->minRequiredVersion);
  printf("accessFlags: %.2X\n", e->accessFlags);
  printf("typeData: %.2X %.2X\n", e->typeData[0], e->typeData[1]);
  printf("lastModified: %.2X %.2X %.2X %.2X\n", e->lastModified[0], e->lastModified[1], e->lastModified[2], e->lastModified[3]);
  printf("headerPointer: %.2X %.2X\n", e->headerPointer[0], e->headerPointer[1]);
}
#endif

ProdosSpector::ProdosSpector(bool verbose) : Wozspector(verbose)
{
  volBitmapBlock = 6; // safe default; it's usually in block 6
  numBlocksTotal = 280; // safe default; overwritten from the vol dir header
  trackData = NULL;
  trackDataBytes = 0;
  freeBlockBitmap = NULL;
  freeBlockBitmapBytes = 0;
}

ProdosSpector::~ProdosSpector()
{
  if (trackData) {
    free(trackData);
    trackData = NULL;
  }
  if (freeBlockBitmap) {
    free(freeBlockBitmap);
    freeBlockBitmap = NULL;
  }
}

// Load the flat block buffer from whatever the underlying Woz gave us.
// For a floppy, walk the 35 tracks and decode each via T_PO. For an
// HDV, just memcpy the raw file bytes into our buffer.
bool ProdosSpector::loadBlockBuffer()
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

  // Floppy case: 35 tracks × 16 sectors × 256 bytes = 140 KB.
  trackDataBytes = 35 * 256 * 16;
  trackData = (uint8_t *)calloc(trackDataBytes, 1);
  if (!trackData) return false;

  for (int i = 0; i < 35; i++) {
    if (!decodeWozTrackToDsk(i, T_PO, &trackData[i * 256 * 16])) {
      wozMsg(WOZ_MSG_ERROR, "Failed to read track %d", i);
      return false;
    }
  }
  return true;
}

// Read a volume bitmap and return the number of blocks free in it.
// Assumes it's a floppy image bitmap, where the number of blocks
// on the device won't exceed one block of bits...
uint32_t ProdosSpector::calculateBlocksFree()
{
  if (!tree)
    createTree();

  uint32_t maxBlocks = numBlocksTotal;

  uint32_t ptr = 0;
  uint8_t bits = 0x80;
  uint32_t count = 0;
  for (uint32_t i = 0; i < maxBlocks; i++) {
    if (freeBlockBitmap[ptr] & bits) {
      count++;
    }
    bits >>= 1;
    if (bits == 0) {
      bits = 0x80;
      ptr++;
    }
  }
  return count;
}

bool ProdosSpector::isBlockFree(uint32_t block)
{
  if (!tree)
    createTree();

  if (!freeBlockBitmap || block >= numBlocksTotal)
    return false;
  return (freeBlockBitmap[block / 8] & (0x80 >> (block % 8))) != 0;
}

void ProdosSpector::blockToTrackSectors(uint32_t block, uint8_t *track,
                                        uint8_t *sectorA, uint8_t *sectorB)
{
  *track = trackFromBlock(block);
  *sectorA = s1map[blockInTrack(block)];
  *sectorB = s2map[blockInTrack(block)];
}

uint8_t ProdosSpector::storageTypeForSize(uint32_t eof)
{
  if (eof <= 512)
    return ft_seedling;
  if (eof <= 128*1024)
    return ft_sapling;
  return ft_tree;
}

const char *ProdosSpector::storageTypeName(uint8_t storageType)
{
  switch (storageType) {
  case ft_seedling: return "seedling";
  case ft_sapling:  return "sapling";
  case ft_tree:     return "tree";
  default:          return NULL;
  }
}

Vent *ProdosSpector::descendTree(uint16_t fromBlock)
{
  Vent *ret = NULL;

  uint16_t currentBlock = fromBlock;
  uint32_t activeEntriesFound = 0;  // full-scan count, to cross-check header

  // A corrupt chain can point outside the volume (a stale link overwritten
  // by file data) or back into itself. Truncate the walk with a warning in
  // either case - the entries already collected are still real.
  set<uint16_t> visited;

  while (currentBlock) {
    if (!validBlock(currentBlock)) {
      wozMsg(WOZ_MSG_WARNING,
             "WARNING: directory chain starting at block %u points at "
             "block %u, outside the volume; the directory is corrupt and "
             "its listing is truncated here.", fromBlock, currentBlock);
      break;
    }
    if (visited.count(currentBlock)) {
      wozMsg(WOZ_MSG_WARNING,
             "WARNING: directory chain starting at block %u loops back to "
             "block %u; the directory is corrupt and its listing is "
             "truncated here.", fromBlock, currentBlock);
      break;
    }
    visited.insert(currentBlock);

    uint8_t *block;
    block = &trackData[512 * currentBlock];

    // A chain link can also point at a block that's *inside* the volume
    // but no longer a directory - e.g. reallocated to a file's data after
    // bitmap corruption. Parsing it would fabricate garbage entries, so
    // vet every active entry slot first: legal storage type, legal name.
    // (Storage types 4/5 are Pascal areas and GS/OS extended files.)
    bool plausible = true;
    for (int i = 0; i < 13 && plausible; i++) {
      const uint8_t *e = block + 4 + i*0x27;
      if (!e[0]) continue; // empty/deleted slot
      uint8_t st = e[0] >> 4, nl = e[0] & 0xF;
      if ((st < 1 || st > 5) && st != 0x0D && st != 0x0E && st != 0x0F) {
        plausible = false;
        break;
      }
      if (nl < 1 || nl > 15) { plausible = false; break; }
      for (int c = 0; c < nl; c++) {
        char ch = e[1 + c];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '.')) {
          plausible = false;
          break;
        }
      }
    }
    if (!plausible) {
      wozMsg(WOZ_MSG_WARNING,
             "WARNING: directory chain starting at block %u reaches block "
             "%u, which no longer holds directory entries (overwritten?); "
             "the directory is corrupt and its listing is truncated "
             "here.", fromBlock, currentBlock);
      break;
    }

    struct _idxHeader *ih = (struct _idxHeader *)block;

    for (int i=0; i<13; i++) {
      if (i==0 && currentBlock == fromBlock) {
	// The first entry of the first block is header data
	struct _subdirent *md = (struct _subdirent *)(block+4);
	// A real directory header declares 0x27-byte entries, 0x0D per
	// block. If it doesn't, this block isn't actually a directory -
	// we were steered here by a bogus key pointer (a corrupt image, or
	// a file entry mis-typed as a directory). Bail out gracefully with
	// a warning instead of aborting the whole process deep in the Vent
	// constructor.
	if (md->entryLength != 0x27 || md->entriesPerBlock != 0x0D) {
	  wozMsg(WOZ_MSG_WARNING,
		 "WARNING: block %u was walked as a directory but its header "
		 "is not one (entryLength=0x%02X, entriesPerBlock=0x%02X); "
		 "skipping it.",
		 currentBlock, md->entryLength, md->entriesPerBlock);
	  return ret; // ret is still NULL here
	}
	Vent *sde = new Vent(md);
	assert(ret == NULL); // shouldn't have been initialized yet?
	ret = sde;
	// Headers don't carry their key block on disk; record it so a
	// subdirectory entry (whose keyPointer names this block) can be
	// matched to this group of entries in the flat tree.
	sde->keyPointerVal(fromBlock);

        // FIXME hard-coded start of directory block, to differentiate the
        // volume header from a subdirectory header
        // FIXME this should look at the type bits to see that it's a
        // vol header instead of using '2'
        if (currentBlock == 2) {
          // Read the vol-dir header fields that size everything else.
          volBitmapBlock = md->volBitmap.pointer[1]*256 + md->volBitmap.pointer[0];
          numBlocksTotal = md->volBlocks.total[1]*256 + md->volBlocks.total[0];

          // The volume bitmap is 1 bit per block. Round up to whole
          // 512-byte blocks. A 140 KB floppy needs just one; a 32 MB
          // HDV spans sixteen.
          uint32_t bitmapBlocks = (numBlocksTotal + 4095) / 4096;
          if (bitmapBlocks < 1) bitmapBlocks = 1;
          if (freeBlockBitmap) free(freeBlockBitmap);
          freeBlockBitmapBytes = bitmapBlocks * 512;
          freeBlockBitmap = (uint8_t *)calloc(freeBlockBitmapBytes, 1);
          if (!freeBlockBitmap) {
            wozMsg(WOZ_MSG_ERROR, "ERROR: out of memory for volume bitmap");
            return NULL;
          }
          // The bitmap pointer comes from disk, so it can be corrupt.
          // Leave the in-RAM bitmap all-zero (every block "used") if the
          // on-disk bitmap isn't where the header claims: reads still
          // work, and nothing will allocate on top of live data.
          // (readBlock would bounce off the un-prepped tree, so pull the
          // bytes from our own trackData directly here.)
          if ((uint32_t)volBitmapBlock * 512 + freeBlockBitmapBytes
              <= trackDataBytes) {
            memcpy(freeBlockBitmap,
                   &trackData[512 * volBitmapBlock],
                   freeBlockBitmapBytes);
          } else {
            wozMsg(WOZ_MSG_WARNING,
                   "WARNING: volume bitmap (block %u) lies outside the "
                   "volume; treating every block as used.",
                    volBitmapBlock);
          }
        }
      } else {
	struct _prodosFent *fe = (struct _prodosFent *)&trackData[512 * currentBlock + i*0x27 + 4];
	// if (fe->typen & 0xF0) == 0, then it's an empty directory entry.
	if (fe->typelen & 0xF0) {
	  activeEntriesFound++;
	  Vent *ve = new Vent(fe);
	  assert(ret); // should have been initialized already?
	  Vent *p3 = ret;
	  while (p3->nextEnt()) {
	    p3 = p3->nextEnt();
	  }
	  p3->nextEnt(ve);
	}
      }
    }

    currentBlock = ih->nextBlock[1] * 256 + ih->nextBlock[0];
  }

  // Cross-check the header's fileCount against what the full scan found.
  // ProDOS 1.1.1's CAT trusts the header count and stops early; a higher
  // actual count means files are visible to anything that walks all
  // blocks but hidden from older tools. A lower actual count means the
  // header is lying about entries that aren't really there. Either way,
  // worth flagging - including in non-verbose mode, since it often
  // indicates tampering, corruption, or a deliberate hiding trick.
  if (ret) {
    uint16_t declared = ret->getActiveFileCount();
    if (declared != activeEntriesFound) {
      wozMsg(WOZ_MSG_WARNING,
             "WARNING: directory '%s' header says %u active entries "
             "but the full scan found %u. ProDOS 1.1.1 will only see "
             "the first %u via CAT.",
             ret->getName(), declared,
             (unsigned)activeEntriesFound, declared);
    }
  }

  return ret;
}

// side effect: marks it as used in RAM until flushFreeBlockList() is called
bool ProdosSpector::findFreeBlock(uint16_t *blockOut)
{
  // tree must be loaded or we will fail
  if (!tree)
    return false;

  uint32_t maxBlocks = numBlocksTotal;

  uint32_t ptr = 0;
  uint8_t bits = 0x80;
  for (uint32_t i = 0; i < maxBlocks; i++) {
    if (freeBlockBitmap[ptr] & bits) {
      // mark it as used
      freeBlockBitmap[ptr] &= ~bits;
      // return the value
      *blockOut = (uint16_t)i;
      if (verbose) wozMsg(WOZ_MSG_DEBUG, "> use block %u (0x%X)", (unsigned)i, (unsigned)i);
      // return that we succeeded in finding a free block
      return true;
    }
    bits >>= 1;
    if (bits == 0) {
      bits = 0x80;
      ptr++;
    }
  }
  return false; // No free blocks available
}

bool ProdosSpector::flushFreeBlockList()
{
  // The volume bitmap spans multiple blocks on anything larger than a
  // floppy (16 blocks on a 32MB HDV); flush every one of them, not just
  // the first, or allocations of blocks >= 4096 are forgotten as soon as
  // the bitmap is re-read.
  for (uint32_t i = 0; i < freeBlockBitmapBytes / 512; i++) {
    if (!writeBlock(volBitmapBlock + i, &freeBlockBitmap[512 * i]))
      return false;
  }
  return true;
}

// Since we preloaded the whole image, this just has to copy data
bool ProdosSpector::validBlock(uint16_t b)
{
  return (uint32_t)b * 512 + 512 <= trackDataBytes;
}

bool ProdosSpector::writeBootBlocks(const uint8_t block0[512],
                                    const uint8_t block1[512])
{
  if (!tree)
    createTree();
  if (!tree)
    return false;

  // No const_cast games: writeBlock only reads its input.
  uint8_t buf[512];
  memcpy(buf, block0, 512);
  if (!writeBlock(0, buf))
    return false;
  memcpy(buf, block1, 512);
  return writeBlock(1, buf);
}

bool ProdosSpector::readBootBlocks(uint8_t block0[512], uint8_t block1[512])
{
  if (!tree)
    createTree();
  if (!tree)
    return false;

  return readBlock(0, block0) && readBlock(1, block1);
}

bool ProdosSpector::renameVolume(const char *newName)
{
  if (!tree)
    createTree();
  if (!tree)
    return false;

  size_t nl = strlen(newName);
  if (nl < 1 || nl > 15)
    return false;

  // The volume's name lives in exactly one place: the volume directory
  // header at the start of block 2. High nybble of typelen stays 0xF
  // (volume directory header); low nybble is the name length.
  struct _subdirent *hdr = (struct _subdirent *)(&trackData[512 * 2 + 4]);
  if ((hdr->typelen >> 4) != ft_voldirhdr) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: block 2 doesn't hold a volume directory header");
    return false;
  }
  hdr->typelen = (ft_voldirhdr << 4) | (uint8_t)nl;
  memset(hdr->name, 0, sizeof(hdr->name));
  memcpy(hdr->name, newName, nl);

  if (!writeBlock(2, &trackData[512 * 2]))
    return false;

  createTree(); // refresh the cached tree so ls shows the new name
  return true;
}

// The highest block number one seedling/sapling/tree structure touches -
// index blocks included. Pointers that leave the volume are ignored here;
// they're corruption, and the resize check that calls this reports on
// blocks that are real but too high. Returns 0 for an unreadable shape.
uint32_t ProdosSpector::highestBlockInFork(uint8_t storage, uint16_t kp)
{
  if (!validBlock(kp))
    return 0;
  uint32_t hi = kp;

  if (storage == ft_sapling) {
    for (int i = 0; i < 256; i++) {
      uint16_t b = trackData[512 * kp + 256 + i] * 256 + trackData[512 * kp + i];
      if (b && validBlock(b) && b > hi) hi = b;
    }
  } else if (storage == ft_tree) {
    for (int mi = 0; mi < 128; mi++) {
      uint16_t ib = trackData[512 * kp + 256 + mi] * 256 + trackData[512 * kp + mi];
      if (!ib || !validBlock(ib)) continue;
      if (ib > hi) hi = ib;
      for (int i = 0; i < 256; i++) {
        uint16_t b = trackData[512 * ib + 256 + i] * 256 + trackData[512 * ib + i];
        if (b && validBlock(b) && b > hi) hi = b;
      }
    }
  } else if (storage == ft_extended) {
    uint8_t keyBlockData[512];
    if (!readBlock(kp, keyBlockData))
      return hi;
    for (int pass = 0; pass < 2; pass++) {
      struct ProdosFork f;
      readForkEntry(keyBlockData, pass == 1, &f);
      if (!f.storageType) continue;
      uint32_t fh = highestBlockInFork(f.storageType, f.keyBlock);
      if (fh > hi) hi = fh;
    }
  }
  // ft_seedling (and anything else) is just the key block itself.
  return hi;
}

// Walk every directory chain and file, reporting the first thing found at
// or past `limit`. Returns true if something is up there, and fills `what`
// with a description of it.
bool ProdosSpector::findBlockUseAtOrAbove(uint32_t limit, char *what,
                                          size_t whatSz)
{
  if (!tree)
    createTree();
  if (!tree)
    return false;

  for (Vent *p = tree; p; p = p->nextEnt()) {
    if (p->isHeader()) {
      // Directory blocks are a chain hanging off the header's key block.
      set<uint16_t> visited;
      uint16_t b = p->keyPointerVal();
      while (b && validBlock(b) && !visited.count(b)) {
        if (b >= limit) {
          snprintf(what, whatSz, "directory '%s' occupies block %u",
                   p->getName(), b);
          return true;
        }
        visited.insert(b);
        b = trackData[512 * b + 2] + trackData[512 * b + 3] * 256;
      }
      continue;
    }
    if (p->isDirectory())
      continue; // counted through its own header above

    uint32_t hi = highestBlockInFork(p->getStorageType(), p->keyPointerVal());
    if (hi >= limit) {
      snprintf(what, whatSz, "file '%s' occupies block %u", p->getName(), (unsigned)hi);
      return true;
    }
  }
  return false;
}

// Where a block number is stored. ProDOS writes block pointers two ways:
// as two adjacent bytes (a directory entry's key pointer) and split across
// the halves of an index block (low byte at i, high byte at 256+i). One
// pair of offsets covers both.
namespace {
  struct BlockRef {
    uint16_t container;  // block the pointer lives in
    uint16_t offLow;     // byte offset of the low half within that block
    uint16_t offHigh;    // byte offset of the high half
  };
  typedef map<uint16_t, vector<BlockRef> > RefMap;
}

// Record every stored pointer to every block, so a block can be moved and
// everything naming it corrected. Walks the directory tree from block 2:
// directory chain links, each entry's key and header pointers, a
// subdirectory header's parent pointer, and the index blocks of every
// file (both forks of an extended one). Deliberately skips the volume
// header's bitmap pointer: the bitmap never moves, because resize refuses
// any size that wouldn't leave room for it where it is.
void ProdosSpector::collectBlockRefs(void *refsOut)
{
  RefMap &refs = *(RefMap *)refsOut;

  // A stack of directories still to walk, so a deep tree can't recurse
  // this function past its welcome.
  stack<uint16_t> pending;
  set<uint16_t> walked;
  pending.push(2);

  while (!pending.empty()) {
    uint16_t dirKey = pending.top();
    pending.pop();
    if (walked.count(dirKey) || !validBlock(dirKey))
      continue;
    walked.insert(dirKey);

    set<uint16_t> chainSeen;
    uint16_t b = dirKey;
    while (b && validBlock(b) && !chainSeen.count(b)) {
      chainSeen.insert(b);
      uint8_t *blk = &trackData[512 * b];

      uint16_t prev = blk[0] + blk[1] * 256;
      uint16_t next = blk[2] + blk[3] * 256;
      if (prev) {
        BlockRef r = { b, 0, 1 };
        refs[prev].push_back(r);
      }
      if (next) {
        BlockRef r = { b, 2, 3 };
        refs[next].push_back(r);
      }

      for (int i = 0; i < 13; i++) {
        uint16_t off = 4 + i * 0x27;
        uint8_t *e = blk + off;
        uint8_t st = e[0] >> 4;

        if (b == dirKey && i == 0) {
          // Directory header. A subdirectory's points back at the block
          // holding its entry in the parent.
          if (st == ft_subdirhdr) {
            uint16_t parent = e[0x23] + e[0x24] * 256;
            if (parent) {
              BlockRef r = { b, (uint16_t)(off + 0x23), (uint16_t)(off + 0x24) };
              refs[parent].push_back(r);
            }
          }
          continue;
        }
        if (!st || !(e[0] & 0x0F))
          continue; // empty or deleted slot

        uint16_t kp = e[0x11] + e[0x12] * 256;
        if (kp) {
          BlockRef r = { b, (uint16_t)(off + 0x11), (uint16_t)(off + 0x12) };
          refs[kp].push_back(r);
        }
        uint16_t hp = e[0x25] + e[0x26] * 256;
        if (hp) {
          BlockRef r = { b, (uint16_t)(off + 0x25), (uint16_t)(off + 0x26) };
          refs[hp].push_back(r);
        }

        if (st == ft_subdir)
          pending.push(kp);
        else
          collectForkRefs(refsOut, st, kp);
      }

      b = blk[2] + blk[3] * 256;
    }
  }
}

// The index-block pointers of one file structure (recursing into both
// forks for an extended file). The structure's own key block is pointed at
// by the directory entry, which the caller already recorded.
void ProdosSpector::collectForkRefs(void *refsOut, uint8_t storage,
                                    uint16_t kp)
{
  RefMap &refs = *(RefMap *)refsOut;
  if (!kp || !validBlock(kp))
    return;

  if (storage == ft_sapling) {
    for (int i = 0; i < 256; i++) {
      uint16_t b = trackData[512 * kp + 256 + i] * 256 + trackData[512 * kp + i];
      if (!b) continue; // sparse hole
      BlockRef r = { kp, (uint16_t)i, (uint16_t)(256 + i) };
      refs[b].push_back(r);
    }
  } else if (storage == ft_tree) {
    for (int mi = 0; mi < 128; mi++) {
      uint16_t ib = trackData[512 * kp + 256 + mi] * 256 + trackData[512 * kp + mi];
      if (!ib) continue;
      BlockRef r = { kp, (uint16_t)mi, (uint16_t)(256 + mi) };
      refs[ib].push_back(r);
      if (!validBlock(ib)) continue;
      for (int i = 0; i < 256; i++) {
        uint16_t b = trackData[512 * ib + 256 + i] * 256 + trackData[512 * ib + i];
        if (!b) continue;
        BlockRef dr = { ib, (uint16_t)i, (uint16_t)(256 + i) };
        refs[b].push_back(dr);
      }
    }
  } else if (storage == ft_extended) {
    for (int pass = 0; pass < 2; pass++) {
      uint16_t mini = pass ? 0x100 : 0x000;
      uint8_t *m = &trackData[512 * kp + mini];
      uint8_t fst = m[0];
      uint16_t fkp = m[1] + m[2] * 256;
      if (!fst || !fkp) continue;
      BlockRef r = { kp, (uint16_t)(mini + 1), (uint16_t)(mini + 2) };
      refs[fkp].push_back(r);
      collectForkRefs(refsOut, fst, fkp);
    }
  }
  // A seedling's key block *is* its data; nothing else points at anything.
}

// Move every allocated block at or past `limit` down into free space below
// it, correcting every pointer that named the moved block. Refuses up
// front, changing nothing, if there isn't enough free space below the
// limit to hold them all.
bool ProdosSpector::relocateBlocksBelow(uint32_t limit, uint32_t *movedOut)
{
  *movedOut = 0;

  // What has to move, and what there is to move it into.
  vector<uint16_t> toMove;
  uint32_t freeBelow = 0;
  for (uint32_t b = 0; b < numBlocksTotal; b++) {
    uint32_t byteIdx = b / 8;
    if (byteIdx >= freeBlockBitmapBytes)
      break;
    bool isFree = (freeBlockBitmap[byteIdx] & (0x80 >> (b % 8))) != 0;
    if (b < limit) {
      if (isFree) freeBelow++;
    } else if (!isFree) {
      toMove.push_back((uint16_t)b);
    }
  }
  if (toMove.empty())
    return true;

  if (toMove.size() > freeBelow) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: %zu block%s sit%s at or past block %u, but only %u free "
           "block%s below it - %zu more needed. Nothing changed.",
           toMove.size(), toMove.size() == 1 ? "" : "s",
           toMove.size() == 1 ? "s" : "", (unsigned)limit, (unsigned)freeBelow,
           freeBelow == 1 ? "" : "s", (size_t)(toMove.size() - freeBelow));
    return false;
  }

  RefMap refs;
  collectBlockRefs(&refs);

  wozMsg(WOZ_MSG_INFO,
         "Moving %zu block%s out of the last %u block%s of the volume...",
         toMove.size(), toMove.size() == 1 ? "" : "s",
         (unsigned)(numBlocksTotal - limit),
         numBlocksTotal - limit == 1 ? "" : "s");

  // A block only ever moves down into free space below the limit, so a
  // destination is never itself moved: this map stays one level deep.
  map<uint16_t, uint16_t> moved;
  set<uint16_t> dirty; // blocks whose contents we changed and must flush
  uint32_t orphans = 0;

  for (size_t i = 0; i < toMove.size(); i++) {
    uint16_t src = toMove[i];
    RefMap::iterator it = refs.find(src);
    if (it == refs.end()) {
      // Marked allocated, but nothing in the volume points at it. It can't
      // stay (it's about to be outside the volume) and no file loses
      // anything by its going, so release it and say so.
      markBlockFree(src);
      orphans++;
      continue;
    }

    uint16_t dst;
    if (!findFreeBlock(&dst) || dst >= limit) {
      wozMsg(WOZ_MSG_ERROR,
             "ERROR: ran out of free blocks below %u while moving block "
             "%u; the volume in memory is now inconsistent - do NOT save "
             "it", (unsigned)limit, (unsigned)src);
      return false;
    }

    memcpy(&trackData[512 * dst], &trackData[512 * src], 512);
    dirty.insert(dst);
    markBlockFree(src);
    moved[src] = dst;

    // Point everything that named the old block at the new one. A pointer
    // living in a block that has itself already moved is patched in its
    // new home; the copy above carried the old contents along.
    for (size_t r = 0; r < it->second.size(); r++) {
      uint16_t container = it->second[r].container;
      map<uint16_t, uint16_t>::iterator m = moved.find(container);
      if (m != moved.end())
        container = m->second;
      if (!validBlock(container))
        continue;
      trackData[512 * container + it->second[r].offLow]  = dst & 0xFF;
      trackData[512 * container + it->second[r].offHigh] = (dst >> 8);
      dirty.insert(container);
    }
    (*movedOut)++;
  }

  if (orphans) {
    wozMsg(WOZ_MSG_WARNING,
           "WARNING: %u block%s past block %u %s marked allocated but "
           "belonged to no file or directory; released",
           (unsigned)orphans, orphans == 1 ? "" : "s", (unsigned)limit,
           orphans == 1 ? "was" : "were");
  }

  // Flush every block we rewrote. (A block that moved and was then patched
  // is in `dirty` under its new number, which is the one that matters.)
  for (set<uint16_t>::iterator d = dirty.begin(); d != dirty.end(); ++d) {
    if (!writeBlock(*d, &trackData[512 * (*d)])) {
      wozMsg(WOZ_MSG_ERROR,
             "ERROR: couldn't write block %u; the volume in memory is now "
             "inconsistent - do NOT save it", *d);
      return false;
    }
  }

  // The bitmap goes out with them: it records every move, and this is the
  // point where the volume is coherent again. If the caller gives up after
  // this, what's in hand is still a valid volume - just not a shrunk one.
  if (!flushFreeBlockList()) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: couldn't write the volume bitmap after moving blocks");
    return false;
  }

  // Key pointers all over the tree may have just changed; the cached
  // entries (which the resize checks consult) have to be rebuilt.
  createTree();
  return true;
}

// Shrink the volume to `newBlocks`, updating the volume directory header
// and the bitmap and truncating the image itself. Refuses to do anything
// at all if any block at or past the new end is in use - by the bitmap's
// account or by any directory or file we can walk. Growing isn't handled:
// that needs the image file to gain blocks, which is what `format` plus a
// copy is for.
bool ProdosSpector::resizeVolume(uint32_t newBlocks, bool relocate)
{
  if (!tree)
    createTree();
  if (!tree)
    return false;

  if (!isHdv()) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: only raw block images (.img/.hdv/.po) can be resized; a "
           "floppy image is a fixed 280 blocks");
    return false;
  }

  struct _subdirent *hdr = (struct _subdirent *)(&trackData[512 * 2 + 4]);
  if ((hdr->typelen >> 4) != ft_voldirhdr) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: block 2 doesn't hold a volume directory header");
    return false;
  }

  uint32_t oldBlocks = numBlocksTotal;
  if (newBlocks == oldBlocks) {
    wozMsg(WOZ_MSG_INFO,
           "Volume is already %u blocks; nothing to do", (unsigned)newBlocks);
    return true;
  }
  if (newBlocks > oldBlocks) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: this volume is %u blocks; resize only shrinks a volume "
           "(growing one means formatting a bigger image and copying into "
           "it)", (unsigned)oldBlocks);
    return false;
  }

  // The bitmap has to keep describing the volume from inside it, and the
  // boot blocks and volume directory have to survive.
  uint32_t newBitmapBlocks = (newBlocks + 4095) / 4096;
  uint32_t lastReserved = (uint32_t)volBitmapBlock + newBitmapBlocks;
  if (newBlocks < 6 || newBlocks <= lastReserved) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: %u blocks is too small; the boot blocks, volume "
           "directory and bitmap need at least %u",
           (unsigned)newBlocks, (unsigned)(lastReserved + 1));
    return false;
  }

  // Anything still living up there has to come down first, if the caller
  // allows it and there's room. relocateBlocksBelow() refuses without
  // touching anything when there isn't.
  if (relocate) {
    uint32_t moved = 0;
    if (!relocateBlocksBelow(newBlocks, &moved))
      return false;
    if (moved)
      wozMsg(WOZ_MSG_INFO,
             "Moved %u block%s below block %u", (unsigned)moved,
             moved == 1 ? "" : "s", (unsigned)newBlocks);
  }

  // Nothing may live at or past the new end. Check the bitmap first (it's
  // what ProDOS itself believes), then the directories and files, which
  // catches a volume whose bitmap under-reports what's really allocated.
  uint32_t inUse = 0, firstInUse = 0;
  for (uint32_t b = newBlocks; b < oldBlocks; b++) {
    uint32_t byteIdx = b / 8;
    if (byteIdx >= freeBlockBitmapBytes)
      break;
    bool free_ = (freeBlockBitmap[byteIdx] & (0x80 >> (b % 8))) != 0;
    if (!free_) {
      if (!inUse) firstInUse = b;
      inUse++;
    }
  }
  if (inUse) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: %u block%s at or past block %u %s still allocated (first "
           "is block %u); nothing changed",
           (unsigned)inUse, inUse == 1 ? "" : "s", (unsigned)newBlocks,
           inUse == 1 ? "is" : "are", (unsigned)firstInUse);
    return false;
  }

  char what[128];
  if (findBlockUseAtOrAbove(newBlocks, what, sizeof(what))) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: %s, at or past the requested new end of %u blocks; "
           "nothing changed", what, (unsigned)newBlocks);
    return false;
  }

  // Safe to shrink. Bitmap blocks the smaller volume no longer needs
  // become ordinary free blocks.
  uint32_t oldBitmapBlocks = (oldBlocks + 4095) / 4096;
  for (uint32_t b = (uint32_t)volBitmapBlock + newBitmapBlocks;
       b < (uint32_t)volBitmapBlock + oldBitmapBlocks; b++) {
    if (b < newBlocks)
      freeBlockBitmap[b / 8] |= (uint8_t)(0x80 >> (b % 8));
  }

  // Blocks that no longer exist are marked unavailable (0), matching what
  // format writes; everything past the new bitmap's blocks goes too.
  for (uint32_t b = newBlocks; b < oldBlocks; b++) {
    uint32_t byteIdx = b / 8;
    if (byteIdx < freeBlockBitmapBytes)
      freeBlockBitmap[byteIdx] &= (uint8_t)~(0x80 >> (b % 8));
  }

  numBlocksTotal = newBlocks;
  hdr->volBlocks.total[0] = (uint8_t)(newBlocks & 0xFF);
  hdr->volBlocks.total[1] = (uint8_t)(newBlocks >> 8);
  if (!writeBlock(2, &trackData[512 * 2])) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: couldn't write the volume directory header");
    return false;
  }
  if (!flushFreeBlockList()) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: couldn't write the volume bitmap");
    return false;
  }

  // Drop the tail of the image. The in-RAM copies shrink with it so no
  // later write can reach past the volume's new end.
  uint32_t newBytes = newBlocks * 512;
  if (!truncateHdv(newBytes)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: couldn't shrink the image buffer");
    return false;
  }
  if (newBytes < trackDataBytes)
    trackDataBytes = newBytes;

  // The bitmap array shrinks too, so the freed tail bytes can't come back.
  freeBlockBitmapBytes = newBitmapBlocks * 512;

  createTree();
  return true;
}

bool ProdosSpector::readBlock(uint16_t blockNum, uint8_t dataOut[512])
{
  // Caller must have prepped with createTree() call first, or we fail
  if (!tree)
    return false;
  if (!validBlock(blockNum))
    return false;

  memcpy(dataOut, &trackData[512*blockNum], 512);
  return true;
}

// Since we preloaded the whole image, this has to copy data to that, and
// *also* back to the loaded woz track data, so that the base Woz::writeFile()
// can write the changed data back out
bool ProdosSpector::writeBlock(uint16_t blockNum, uint8_t data[512])
{
  // Caller must have prepped with createTree() call first, or we fail
  if (!tree)
    return false;
  if ((uint32_t)blockNum * 512 + 512 > trackDataBytes)
    return false;

  // update our local cached data, b/c we loaded it on createTree()
  memcpy(&trackData[512*blockNum], data, 512);

  // HDV images have no track/sector structure - the HDV writer just
  // flushes the raw buffer. Keep Woz's hdvData in sync so writeFile
  // picks up the change.
  if (isHdv()) {
    memcpy(hdvBuffer() + 512 * blockNum, data, 512);
    return true;
  }

  // Floppy case: also push the block back through the nibble encoder so
  // Woz::writeFile() emits a correct WOZ/DSK/NIB.
  uint8_t t = trackFromBlock(blockNum);
  uint8_t bidx = blockInTrack(blockNum);
  if (!encodeWozTrackSector(t, s1map[bidx], data) ||
      !encodeWozTrackSector(t, s2map[bidx], &data[256])) {
    return false;
  }

  return true;
}

Vent *ProdosSpector::createTree()
{
  if (tree) {
    freeTree(tree);
    tree = NULL;
  }

  // Pull the whole image into our block-indexed buffer (floppy: nibble-
  // decode each track; HDV: copy raw bytes).
  if (!loadBlockBuffer()) {
    wozMsg(WOZ_MSG_ERROR, "Failed to load block buffer");
    return NULL;
  }

  stack <int> s;
  set <int> queued; // key blocks already visited/queued: a corrupt image
                    // can make two directory entries point at the same
                    // subdirectory (or form a cycle), which would loop or
                    // duplicate entries here.
  s.push(2); // directory starts on block 2
  queued.insert(2);

  while (!s.empty()) {
    int nextBlock = s.top(); s.pop();
    Vent *nextDir = descendTree(nextBlock);
    Vent *p = nextDir;
    while (p) {
      if (p->isDirectory()) {
        uint16_t kp = p->keyPointerVal();
        if (!validBlock(kp)) {
          wozMsg(WOZ_MSG_WARNING,
                 "WARNING: directory '%s' points at key block %u, outside "
                 "the volume; skipping its contents.", p->getName(), kp);
        } else if (!queued.count(kp)) {
          s.push(kp);
          queued.insert(kp);
        }
      }
      p = p->nextEnt();
    }
    appendTree(nextDir);
  }

  return tree;
}

bool ProdosSpector::isExtendedFile(Vent *e)
{
  return e && e->getStorageType() == PRODOS_STORAGE_EXTENDED;
}

void ProdosSpector::readForkEntry(const uint8_t keyBlockData[512],
                                  bool resourceFork, struct ProdosFork *out)
{
  // The data fork's mini-entry sits at the top of the extended key block
  // and the resource fork's at the halfway mark. Each is:
  //   +0     storage type of the fork
  //   +1..2  key block (little-endian)
  //   +3..4  blocks used
  //   +5..7  EOF (24 bits)
  const uint8_t *m = keyBlockData + (resourceFork ? 0x100 : 0x000);
  out->storageType = m[0];
  out->keyBlock    = m[1] + m[2] * 256;
  out->blocksUsed  = m[3] + m[4] * 256;
  out->eof         = (uint32_t)m[5] + m[6] * 256 + (uint32_t)m[7] * 65536;
}

void ProdosSpector::writeForkEntry(uint8_t keyBlockData[512],
                                   bool resourceFork,
                                   const struct ProdosFork *f)
{
  uint8_t *m = keyBlockData + (resourceFork ? 0x100 : 0x000);
  m[0] = f->storageType;
  m[1] = f->keyBlock & 0xFF;
  m[2] = (f->keyBlock >> 8);
  m[3] = f->blocksUsed & 0xFF;
  m[4] = (f->blocksUsed >> 8);
  m[5] = f->eof & 0xFF;
  m[6] = (f->eof >> 8) & 0xFF;
  m[7] = (f->eof >> 16) & 0xFF;
}

bool ProdosSpector::readExtendedKeyBlock(Vent *e, uint8_t out[512])
{
  if (!isExtendedFile(e))
    return false;
  return readBlock(e->keyPointerVal(), out);
}

bool ProdosSpector::getForkInfo(Vent *e, bool resourceFork,
                                struct ProdosFork *out)
{
  memset(out, 0, sizeof(*out));

  if (!isExtendedFile(e)) {
    // An ordinary file is all data fork and has no resource fork.
    if (resourceFork)
      return true;
    out->storageType = e->getStorageType();
    out->keyBlock    = e->keyPointerVal();
    out->blocksUsed  = e->getBlocksUsed();
    out->eof         = e->getEofLength();
    return true;
  }

  uint8_t keyBlockData[512];
  if (!readBlock(e->keyPointerVal(), keyBlockData)) {
    wozMsg(WOZ_MSG_WARNING,
           "WARNING: '%s' is an extended file whose key block %u can't be "
           "read; treating the fork as empty.",
           e->getName(), e->keyPointerVal());
    return false;
  }
  readForkEntry(keyBlockData, resourceFork, out);
  return true;
}

uint32_t ProdosSpector::getAllocatedByteCount(Vent *e)
{
  // For an extended file the entry's blocksUsed covers both forks and the
  // extended key block; the caller is asking about the contents it gets
  // from getFileContents(), which is the data fork alone.
  if (isExtendedFile(e)) {
    struct ProdosFork f;
    if (getForkInfo(e, false, &f))
      return (uint32_t)f.blocksUsed * 512;
  }
  return (uint32_t)e->getBlocksUsed() * 512;
}

uint32_t ProdosSpector::getForkContents(Vent *e, bool resourceFork,
                                        char **toWhere)
{
  *toWhere = NULL;

  struct ProdosFork f;
  if (!getForkInfo(e, resourceFork, &f))
    return 0;
  // An absent fork (never written by GS/OS) has no storage type at all.
  if (!f.storageType || !f.eof)
    return 0;

  return readForkBlocks(f.storageType, f.keyBlock, f.eof, e->getName(),
                        toWhere);
}

uint32_t ProdosSpector::getFileContents(Vent *e, char **toWhere)
{
  // Given the virtual entry 'e', find the file's contents and return
  //them in *toWhere. We malloc() that result, and the caller will
  //free it.

  // An extended file's directory entry describes the extended key block,
  // not any contents: its storage type is 5 and its EOF is the block size.
  // The file's ordinary contents are its data fork.
  if (isExtendedFile(e))
    return getForkContents(e, false, toWhere);

  uint8_t indexType = e->getStorageType();
  uint16_t kp = e->keyPointerVal();
  // eofLength is the authoritative file size for every ProDOS entry,
  // regardless of file type. Directories are excluded at the storage-type
  // level below (they never reach the seedling/sapling/tree cases).
  uint32_t l = e->getEofLength();

  if (!l) {
      // handle zero-length files
      *toWhere = NULL;
      return 0;
  }

  return readForkBlocks(indexType, kp, l, e->getName(), toWhere);
}

// Read the seedling/sapling/tree structure rooted at `kp`, whose logical
// length is `len`. This is the whole of an ordinary file and one fork of
// an extended one - the block shapes are identical either way.
uint32_t ProdosSpector::readForkBlocks(uint8_t indexType, uint16_t kp,
                                       uint32_t len, const char *name,
                                       char **toWhere)
{
  uint32_t l = len;

  // On a corrupt volume the key block or any pointer inside an index can
  // aim outside the image. Read what's real, substitute zeros for the
  // rest, and say how much was substituted - the caller still gets a
  // buffer of the file's full length.
  uint32_t badPointers = 0;

  switch (indexType) {
  case 1:
    // Seedling file: we point right to the data
    *toWhere = (char *)malloc(l); // FIXME check for error
    if (!validBlock(kp)) {
      memset(*toWhere, 0, l);
      badPointers++;
      break;
    }
    // A seedling holds one block; an EOF claiming more than that is a
    // corrupt entry, and the excess reads as zeros rather than as
    // whatever follows the block in the image.
    if (l > 512) {
      memset(*toWhere, 0, l);
      memcpy(*toWhere, &trackData[512 * kp], 512);
      badPointers++;
      break;
    }
    memcpy(*toWhere, &trackData[512 * kp], l);
    return l;
  case 2:
    // sapling file: 2-256 data blocks. The index block has the low bytes in the
    // first 256, and the high bytes in the second 256.
    {
      uint32_t sizeRemaining = l;
      uint32_t bytesCopied = 0;
      *toWhere = (char *)malloc(l); // FIXME check for error
      if (!validBlock(kp)) {
        memset(*toWhere, 0, l);
        badPointers++;
        break;
      }
      uint8_t idx = 0;
      while (sizeRemaining) {
        uint32_t nextBlockOfData = trackData[512*kp + 256 + idx] * 256 + trackData[512*kp + idx];
        uint32_t chunk = (sizeRemaining >= 512) ? 512 : sizeRemaining;
        if (nextBlockOfData && validBlock(nextBlockOfData)) {
          memcpy((*toWhere) + bytesCopied, &trackData[512*nextBlockOfData], chunk);
        } else {
          // zero pointer = sparse hole; out-of-range = corruption
          memset((*toWhere) + bytesCopied, 0, chunk);
          if (nextBlockOfData) badPointers++;
        }
        bytesCopied += chunk;
        sizeRemaining -= chunk;
        idx++;
      }
    }
    break;

  case 3:
    // Tree file: the key block is a master index of up to 128 index
    // blocks, each of which points at up to 256 data blocks. A zero
    // pointer at either level is a sparse hole and reads as zeros.
    {
      uint32_t sizeRemaining = l;
      uint32_t bytesCopied = 0;
      *toWhere = (char *)malloc(l); // FIXME check for error
      if (!validBlock(kp)) {
        memset(*toWhere, 0, l);
        badPointers++;
        break;
      }
      for (int mi = 0; mi < 128 && sizeRemaining; mi++) {
        uint32_t indexBlock = trackData[512*kp + 256 + mi] * 256 + trackData[512*kp + mi];
        if (indexBlock && !validBlock(indexBlock)) {
          badPointers++;
          indexBlock = 0; // treat the whole 128k span as unreadable
        }
        for (int i = 0; i < 256 && sizeRemaining; i++) {
          uint32_t dataBlock = indexBlock ?
            (uint32_t)trackData[512*indexBlock + 256 + i] * 256 + trackData[512*indexBlock + i] : 0;
          if (dataBlock && !validBlock(dataBlock)) {
            badPointers++;
            dataBlock = 0;
          }
          uint32_t chunk = (sizeRemaining >= 512) ? 512 : sizeRemaining;
          if (dataBlock) {
            memcpy((*toWhere) + bytesCopied, &trackData[512*dataBlock], chunk);
          } else {
            memset((*toWhere) + bytesCopied, 0, chunk);
          }
          bytesCopied += chunk;
          sizeRemaining -= chunk;
        }
      }
    }
    break;
  default:
    // deleted, a Pascal area (type 4), or corruption. Extended files never
    // arrive here: getFileContents() resolves them to a fork first.
    wozMsg(WOZ_MSG_WARNING, "WARNING: '%s' has unreadable storage type %d; "
           "skipping it.", name, indexType);
    *toWhere = NULL;
    return 0;
  }

  if (badPointers) {
    wozMsg(WOZ_MSG_WARNING,
           "WARNING: '%s' has %u block pointer%s outside the volume; "
           "those parts read as zeros.",
           name, (unsigned)badPointers, badPointers == 1 ? "" : "s");
  }
  return l;
}

bool ProdosSpector::writeFileToImage(uint8_t *fileContents,
                                     char *fileName,
                                     uint8_t fileType,
                                     uint16_t auxTypeData,
                                     uint32_t fileSize)
{
  // Thin wrapper: build a metadata template with the classic defaults
  // (unlocked, no dates, ProDOS 1.0 version bytes) and hand off.
  struct _prodosFent meta;
  memset(&meta, 0, sizeof(meta));
  meta.fileType = fileType;
  meta.accessFlags = at_destroyed | at_renamed | at_written | at_read;
  meta.typeData[0] = auxTypeData & 0xFF;
  meta.typeData[1] = (auxTypeData >> 8);
  return writeFileWithMeta(fileContents, fileName, fileSize, &meta);
}

// Store `size` bytes of `contents` as a ProDOS seedling, sapling, or tree,
// and report the shape chosen, the key block that names it, and how many
// blocks it consumed (data plus index). This is the block-level half of
// writing a file: an ordinary file has exactly one of these structures,
// and an extended file has one per fork. On failure some blocks may be
// marked used in the in-RAM bitmap, so the caller must roll back with
// reloadFreeBlockBitmap().
bool ProdosSpector::writeForkBlocks(const uint8_t *contents, uint32_t size,
                                    uint8_t *storageTypeOut,
                                    uint16_t *keyBlockOut,
                                    uint16_t *blocksUsedOut)
{
  // The EOF field is 24 bits, so 16MB-1 is the hard ProDOS file limit.
  if (size > 0xFFFFFF) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: ProDOS files may not exceed 16MB-1 bytes");
    return false;
  }

  // Data blocks the content needs (an empty file still gets one); index
  // blocks are added per storage type below. A tree file can use over
  // 32,000 blocks, so this must be at least 16 bits wide.
  uint32_t dataBlocks = (size + 511) / 512;
  if (dataBlocks == 0) dataBlocks = 1;
  uint16_t blocksUsed = dataBlocks;

  // Create the file structure - seedling for <= 512 bytes; sapling for <=128k;
  // or tree for >128k.
  if (size <= 512) {
    // Seedling file entry
    // The KEY_POINTER in the directory block points directly to the file
    // data
    *storageTypeOut = ft_seedling;
    
    // Allocate one block for the file
    uint16_t firstBlock;
    if (!findFreeBlock(&firstBlock)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to find a free block for the data");
      reloadFreeBlockBitmap();
      return false;
    }
    uint8_t paddedData[512];
    memset(&paddedData, 0, sizeof(paddedData));
    memcpy(paddedData, contents, size);
    if (!writeBlock(firstBlock, paddedData)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to write data block");
      reloadFreeBlockBitmap();
      return false;
    }

    *keyBlockOut = firstBlock;
  } else if (size <= 128*1024) {
    // Sapling file entry
    // We need an index block that points to all of the data blocks, and the
    // index block number is stored in KEY_POINTER.
    *storageTypeOut = ft_sapling;
    blocksUsed++;
    uint16_t indexBlock;
    if (!findFreeBlock(&indexBlock)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to find a free block for the index");
      reloadFreeBlockBitmap();
      return false;
    }
    uint8_t indexBlockData[512];
    uint8_t indexBlockPtr = 0;
    memset(indexBlockData, 0, sizeof(indexBlockData));

    *keyBlockOut = indexBlock;

    
    // Allocate the blocks for the file data and start copying it in
    uint32_t bytesRemaining = size;
    for (int idx = 0; idx < size; idx+= 512, bytesRemaining -= 512) {
      uint16_t nextBlock;
      if (!findFreeBlock(&nextBlock)) {
        wozMsg(WOZ_MSG_ERROR, "Failed to find next free block for the data");
        reloadFreeBlockBitmap();
        return false;
      }

      indexBlockData[indexBlockPtr] = nextBlock & 0xFF;
      indexBlockData[0x100 + indexBlockPtr++] = (nextBlock >> 8);

      uint8_t paddedData[512];
      memset(&paddedData, 0, sizeof(paddedData));
      memcpy(paddedData, &contents[idx],
             bytesRemaining >= 512 ? 512 : bytesRemaining);
      if (!writeBlock(nextBlock, paddedData)) {
        wozMsg(WOZ_MSG_ERROR, "Failed to write next data block");
        reloadFreeBlockBitmap();
        return false;
      }
    }

    if (!writeBlock(indexBlock, indexBlockData)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to write index block");
      reloadFreeBlockBitmap();
      return false;
    }
  } else {
    // Tree file entry (>128k, up to 16MB-1): KEY_POINTER names a master
    // index block whose 128 pointers each name a sapling-style index block
    // covering 128k of data.
    *storageTypeOut = ft_tree;

    uint16_t masterBlock;
    if (!findFreeBlock(&masterBlock)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to find a free block for the master index");
      reloadFreeBlockBitmap();
      return false;
    }
    uint8_t masterData[512];
    memset(masterData, 0, sizeof(masterData));
    uint8_t masterPtr = 0;

    *keyBlockOut = masterBlock;
    blocksUsed++; // the master index block

    uint32_t bytesRemaining = size;
    uint32_t idx = 0;
    while (bytesRemaining) {
      uint16_t indexBlock;
      if (!findFreeBlock(&indexBlock)) {
        wozMsg(WOZ_MSG_ERROR, "Failed to find a free block for an index");
        reloadFreeBlockBitmap();
        return false;
      }
      masterData[masterPtr] = indexBlock & 0xFF;
      masterData[0x100 + masterPtr++] = (indexBlock >> 8);
      blocksUsed++; // this index block

      uint8_t indexBlockData[512];
      memset(indexBlockData, 0, sizeof(indexBlockData));
      for (int i = 0; i < 256 && bytesRemaining; i++) {
        uint16_t nextBlock;
        if (!findFreeBlock(&nextBlock)) {
          wozMsg(WOZ_MSG_ERROR, "Failed to find next free block for the data");
          reloadFreeBlockBitmap();
          return false;
        }
        indexBlockData[i] = nextBlock & 0xFF;
        indexBlockData[0x100 + i] = (nextBlock >> 8);

        uint8_t paddedData[512];
        memset(&paddedData, 0, sizeof(paddedData));
        memcpy(paddedData, &contents[idx],
               bytesRemaining >= 512 ? 512 : bytesRemaining);
        if (!writeBlock(nextBlock, paddedData)) {
          wozMsg(WOZ_MSG_ERROR, "Failed to write next data block");
          reloadFreeBlockBitmap();
          return false;
        }
        idx += 512;
        bytesRemaining -= (bytesRemaining >= 512) ? 512 : bytesRemaining;
      }
      if (!writeBlock(indexBlock, indexBlockData)) {
        wozMsg(WOZ_MSG_ERROR, "Failed to write index block");
        reloadFreeBlockBitmap();
        return false;
      }
    }

    if (!writeBlock(masterBlock, masterData)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to write master index block");
      reloadFreeBlockBitmap();
      return false;
    }
  }

  *blocksUsedOut = blocksUsed;
  return true;
}

// Like writeFileToImage, but the caller supplies a full directory-entry
// template whose metadata (file type, aux type, access flags, dates, and
// version bytes) is preserved verbatim - the copy machinery uses this to
// clone a file between images without losing its attributes. Structural
// fields of `meta` (name, typelen, pointers, sizes) are ignored and
// recomputed for this volume.
bool ProdosSpector::writeFileWithMeta(uint8_t *fileContents,
                                      const char *fileName,
                                      uint32_t fileSize,
                                      const struct _prodosFent *meta)
{
  if (!tree)
    createTree();

  // fileName may be a path; resolve the directory it lands in and the leaf
  // name within it. Everything below stores into `dirKey`.
  uint16_t dirKey;
  char leaf[16];
  if (!resolveDirAndLeaf(fileName, &dirKey, leaf, sizeof(leaf)))
    return false;

  // Construct a new filesystem entry structure
  struct _prodosFent fent;
  memset(&fent, 0, sizeof(fent));
  size_t leaflen = strlen(leaf);
  if (leaflen < 1 || leaflen > 15) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: file names may not exceed 15 chars in ProDOS");
    return false;
  }
  strncpy(fent.name, leaf, 15);
  // Set the low nybble of typelen to the length of the filename
  fent.typelen = leaflen;

  fent.fileType = meta->fileType;
  fent.eofLength[0] = fileSize & 0xFF;
  fent.eofLength[1] = (fileSize >> 8) & 0xFF;
  fent.eofLength[2] = (fileSize >> 16);

  fent.accessFlags = meta->accessFlags;
  memcpy(fent.creationDate, meta->creationDate, sizeof(fent.creationDate));
  memcpy(fent.lastModified, meta->lastModified, sizeof(fent.lastModified));
  fent.creatorVersion = meta->creatorVersion;
  fent.minRequiredVersion = meta->minRequiredVersion;
  memcpy(fent.typeData, meta->typeData, sizeof(fent.typeData));

  // headerPointer is the key block of the directory that holds this entry.
  fent.headerPointer[0] = dirKey & 0xFF;
  fent.headerPointer[1] = (dirKey >> 8);

  // Lay the contents down as a seedling, sapling, or tree; the helper
  // reports which shape it used and where the structure starts. (It also
  // enforces the 16MB-1 ProDOS file ceiling.)
  uint8_t storageType;
  uint16_t keyBlock;
  uint16_t blocksUsed;
  if (!writeForkBlocks(fileContents, fileSize, &storageType, &keyBlock,
                       &blocksUsed))
    return false;
  fent.typelen |= (storageType << 4);
  fent.keyPointer[0] = keyBlock & 0xFF;
  fent.keyPointer[1] = (keyBlock >> 8);

  if (verbose)
    wozMsg(WOZ_MSG_DEBUG, "Blocks used to store this file: %d", blocksUsed);
  fent.blocksUsed[0] = blocksUsed & 0xFF;
  fent.blocksUsed[1] = (blocksUsed >> 8);

  // Write the directory entry
  if (!addDirectoryEntryForFile(dirKey, &fent)) {
    wozMsg(WOZ_MSG_ERROR, "Error writing directory entry for file");
    reloadFreeBlockBitmap();
    return false;
  }
  
  // Flush the free block list
  flushFreeBlockList();

  // re-read the catalog
  createTree();

  // Caller is responsible for updating the on-disk image
  return true;
}

// Create an extended (GS/OS forked) file. The two forks are stored just
// like ordinary files; what makes the file extended is that its key block
// is an extended key block holding a mini-entry describing each fork.
bool ProdosSpector::writeExtendedFileWithMeta(const uint8_t *dataFork,
                                              uint32_t dataLen,
                                              const uint8_t *resFork,
                                              uint32_t resLen,
                                              const char *fileName,
                                              const struct _prodosFent *meta,
                                              const uint8_t *srcKeyBlock)
{
  if (!tree)
    createTree();

  uint16_t dirKey;
  char leaf[16];
  if (!resolveDirAndLeaf(fileName, &dirKey, leaf, sizeof(leaf)))
    return false;

  struct _prodosFent fent;
  memset(&fent, 0, sizeof(fent));
  size_t leaflen = strlen(leaf);
  if (leaflen < 1 || leaflen > 15) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: file names may not exceed 15 chars in ProDOS");
    return false;
  }
  strncpy(fent.name, leaf, 15);
  fent.typelen = (ft_extended << 4) | leaflen;

  fent.fileType = meta->fileType;
  // An extended file's directory entry describes its extended key block,
  // so its EOF is that block's size; each fork's real length lives in the
  // mini-entry. (This is what GS/OS itself records.)
  fent.eofLength[0] = 0x00;
  fent.eofLength[1] = 0x02;
  fent.eofLength[2] = 0x00;

  fent.accessFlags = meta->accessFlags;
  memcpy(fent.creationDate, meta->creationDate, sizeof(fent.creationDate));
  memcpy(fent.lastModified, meta->lastModified, sizeof(fent.lastModified));
  fent.creatorVersion = meta->creatorVersion;
  fent.minRequiredVersion = meta->minRequiredVersion;
  memcpy(fent.typeData, meta->typeData, sizeof(fent.typeData));

  fent.headerPointer[0] = dirKey & 0xFF;
  fent.headerPointer[1] = (dirKey >> 8);

  // The extended key block comes first so it lands ahead of the fork data.
  uint16_t extKeyBlock;
  if (!findFreeBlock(&extKeyBlock)) {
    wozMsg(WOZ_MSG_ERROR,
           "Failed to find a free block for the extended key block");
    reloadFreeBlockBitmap();
    return false;
  }
  fent.keyPointer[0] = extKeyBlock & 0xFF;
  fent.keyPointer[1] = (extKeyBlock >> 8);

  // Everything outside the two mini-entries is GS/OS Finder information.
  // A copy carries the donor's bytes over; a fresh file has none.
  uint8_t keyBlockData[512];
  if (srcKeyBlock)
    memcpy(keyBlockData, srcKeyBlock, sizeof(keyBlockData));
  else
    memset(keyBlockData, 0, sizeof(keyBlockData));

  // A fork with no content at all gets no storage: GS/OS reads a zeroed
  // mini-entry as an absent fork, which is how resource-less and
  // data-less files are stored.
  uint16_t totalBlocks = 1; // the extended key block itself
  static const uint8_t emptyByte = 0;
  for (int pass = 0; pass < 2; pass++) {
    bool resourceFork = (pass == 1);
    const uint8_t *contents = resourceFork ? resFork : dataFork;
    uint32_t len = resourceFork ? resLen : dataLen;

    struct ProdosFork f;
    memset(&f, 0, sizeof(f));
    if (contents || len) {
      uint8_t storageType;
      uint16_t keyBlock, blocksUsed;
      if (!writeForkBlocks(contents ? contents : &emptyByte, len,
                           &storageType, &keyBlock, &blocksUsed)) {
        wozMsg(WOZ_MSG_ERROR,
               "Failed to write the %s fork of '%s'",
               resourceFork ? "resource" : "data", leaf);
        reloadFreeBlockBitmap();
        return false;
      }
      f.storageType = storageType;
      f.keyBlock = keyBlock;
      f.blocksUsed = blocksUsed;
      f.eof = len;
      totalBlocks += blocksUsed;
    }
    writeForkEntry(keyBlockData, resourceFork, &f);
  }

  if (!writeBlock(extKeyBlock, keyBlockData)) {
    wozMsg(WOZ_MSG_ERROR, "Failed to write extended key block");
    reloadFreeBlockBitmap();
    return false;
  }

  if (verbose)
    wozMsg(WOZ_MSG_DEBUG,
           "Blocks used to store this extended file: %d", totalBlocks);
  fent.blocksUsed[0] = totalBlocks & 0xFF;
  fent.blocksUsed[1] = (totalBlocks >> 8);

  if (!addDirectoryEntryForFile(dirKey, &fent)) {
    wozMsg(WOZ_MSG_ERROR, "Error writing directory entry for file");
    reloadFreeBlockBitmap();
    return false;
  }

  flushFreeBlockList();
  createTree();

  // Caller is responsible for updating the on-disk image
  return true;
}

bool ProdosSpector::addDirectoryEntryForFile(uint16_t dirKey, struct _prodosFent *e)
{
  // The tree has to be loaded before we start; that loads the block cache
  if (!tree)
    createTree();

  // dirKey is the key (first) block of the directory we're adding to: block
  // 2 for the volume root, or a subdirectory's key block. The directory may
  // span several blocks via the next-block links; if none of them has a free
  // slot we grow the directory by appending a new block (see below).
  if (!validBlock(dirKey)) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: directory key block %u is outside the volume", dirKey);
    return false;
  }
  uint16_t currentBlock = dirKey;
  uint16_t lastBlock = dirKey; // tail of the chain, for growth

  // Writing through a corrupt chain would scribble on whatever the bogus
  // link points at, so refuse outright rather than truncate-and-continue.
  set<uint16_t> visited;

  while (currentBlock) {
    if (!validBlock(currentBlock) || visited.count(currentBlock)) {
      wozMsg(WOZ_MSG_ERROR,
             "ERROR: directory chain is corrupt at block %u; refusing to "
             "add an entry to it", currentBlock);
      return false;
    }
    visited.insert(currentBlock);

    lastBlock = currentBlock;
    uint8_t *block;
    block = &trackData[512 * currentBlock];

    struct _idxHeader *ih = (struct _idxHeader *)block;

    for (int i=0; i<13; i++) {
      if (i==0 && currentBlock == dirKey) {
        // The first entry of the first block is the directory header. We
        // bump its fileCount only after we succeed, so skip it for now.
      } else {
	struct _prodosFent *fe = (struct _prodosFent *)&trackData[512 * currentBlock + i*0x27 + 4];
        if (fe->typelen == 0) { // FIXME are there other possibilties? Overwrite a deleted file, maybe?
          // It's an empty slot where we can store the data
          // First update the track cache
          memcpy(&trackData[512*currentBlock+i*0x27+4],
                 e,
                 sizeof(struct _prodosFent));
          // then update the block in the woz image
          if (!writeBlock(currentBlock, &trackData[512*currentBlock])) {
            wozMsg(WOZ_MSG_ERROR,
                   "Error: can't write block data for directory");
            return false;
          }

          // Bump the active-entry count in this directory's header, which
          // always lives at offset +4 of its key block.
          struct _subdirent *md = (struct _subdirent *)(&trackData[512*dirKey+4]);
          uint16_t fc = md->fileCount[1]*256 + md->fileCount[0];
          fc++;
          md->fileCount[0] = fc & 0xFF;
          md->fileCount[1] = (fc >> 8);
          // That will have updated directly in the trackData cache; just need
          // to write the header block back to the Woz image
          if (!writeBlock(dirKey, &trackData[512*dirKey])) {
            wozMsg(WOZ_MSG_ERROR, "Failed to write directory header back out");
            return false;
          }
          return true;
        }
      }
    }

    currentBlock = ih->nextBlock[1] * 256 + ih->nextBlock[0];
  }

  // Every existing block is full: extend the directory with a new block.
  return growDirectoryForEntry(dirKey, lastBlock, e);
}

bool ProdosSpector::growDirectoryForEntry(uint16_t dirKey, uint16_t lastBlock,
                                          struct _prodosFent *e)
{
  // The volume directory has a fixed number of blocks in ProDOS and cannot
  // grow; only subdirectories can. (dirKey 2 is always the volume root.)
  if (dirKey == 2) {
    wozMsg(WOZ_MSG_ERROR,
           "Error: the volume directory is full and cannot be extended");
    return false;
  }

  uint16_t newBlock;
  if (!findFreeBlock(&newBlock)) {
    wozMsg(WOZ_MSG_ERROR, "Error: no free block to extend the directory");
    return false;
  }

  // Initialize the new block: doubly-linked into the chain after lastBlock,
  // and (unlike a key block) its very first slot is a normal entry, so we
  // store the incoming entry there.
  uint8_t *nb = &trackData[512 * newBlock];
  memset(nb, 0, 512);
  nb[0] = lastBlock & 0xFF;         // prevBlock
  nb[1] = (lastBlock >> 8);
  // nb[2..3] (nextBlock) stay zero: this is the new tail.
  memcpy(&nb[4], e, sizeof(struct _prodosFent));

  // Link the old tail forward to the new block.
  uint8_t *lb = &trackData[512 * lastBlock];
  lb[2] = newBlock & 0xFF;          // nextBlock
  lb[3] = (newBlock >> 8);

  // Bump the directory's active-entry count (header at +4 of its key block).
  struct _subdirent *md = (struct _subdirent *)(&trackData[512 * dirKey + 4]);
  uint16_t fc = md->fileCount[1] * 256 + md->fileCount[0];
  fc++;
  md->fileCount[0] = fc & 0xFF;
  md->fileCount[1] = (fc >> 8);

  // The directory just grew by one block. Reflect that in its own entry in
  // the parent directory: bump blocksUsed and grow EOF by one block. The
  // subdir header records exactly where that parent entry lives.
  uint16_t parentBlock = md->subdirParent.pointer[1] * 256 +
                         md->subdirParent.pointer[0];
  uint8_t  parentEntry = md->subParent.entry; // 1-based within parentBlock
  if (parentBlock && parentEntry) {
    struct _prodosFent *pe = (struct _prodosFent *)
      &trackData[512 * parentBlock + 4 + (parentEntry - 1) * 0x27];
    uint16_t bu = pe->blocksUsed[1] * 256 + pe->blocksUsed[0];
    bu++;
    pe->blocksUsed[0] = bu & 0xFF;
    pe->blocksUsed[1] = (bu >> 8);
    uint32_t eof = pe->eofLength[0] | (pe->eofLength[1] << 8) |
                   (pe->eofLength[2] << 16);
    eof += 512;
    pe->eofLength[0] = eof & 0xFF;
    pe->eofLength[1] = (eof >> 8) & 0xFF;
    pe->eofLength[2] = (eof >> 16) & 0xFF;
    if (!writeBlock(parentBlock, &trackData[512 * parentBlock])) {
      wozMsg(WOZ_MSG_ERROR, "Error: can't update parent directory entry");
      return false;
    }
  }

  // Persist every block we touched. (When lastBlock == dirKey the tail and
  // the key block are one and the same; writing it twice is harmless.)
  if (!writeBlock(newBlock,  &trackData[512 * newBlock]) ||
      !writeBlock(lastBlock, &trackData[512 * lastBlock]) ||
      !writeBlock(dirKey,    &trackData[512 * dirKey])) {
    wozMsg(WOZ_MSG_ERROR, "Error: can't write extended directory blocks");
    return false;
  }
  return true;
}

void ProdosSpector::reloadFreeBlockBitmap()
{
  if (freeBlockBitmap &&
      (uint32_t)volBitmapBlock * 512 + freeBlockBitmapBytes <= trackDataBytes)
    memcpy(freeBlockBitmap, &trackData[512 * volBitmapBlock],
           freeBlockBitmapBytes);
}

void ProdosSpector::markBlockFree(uint16_t block)
{
  // Never free the boot blocks, and don't run off the end of the bitmap.
  if (block < 2 || block >= numBlocksTotal || !freeBlockBitmap)
    return;
  uint32_t ptr = block / 8;
  uint8_t bits = 0x80 >> (block % 8);
  freeBlockBitmap[ptr] |= bits;
}

bool ProdosSpector::freeFileBlocks(const struct _prodosFent *fe)
{
  uint8_t storage = (fe->typelen & 0xF0) >> 4;
  uint16_t kp = fe->keyPointer[1] * 256 + fe->keyPointer[0];

  if (storage == ft_extended) {
    // An extended file owns two fork structures plus the extended key
    // block that describes them. If the key block itself is unreadable
    // there's no way to find the forks; free what we can name and let the
    // rest leak rather than freeing blocks that may belong to something
    // else.
    uint8_t keyBlockData[512];
    if (!validBlock(kp) || !readBlock(kp, keyBlockData)) {
      wozMsg(WOZ_MSG_WARNING,
             "WARNING: extended key block %u is unreadable; freeing only "
             "the directory entry", kp);
      return true;
    }
    for (int pass = 0; pass < 2; pass++) {
      struct ProdosFork f;
      readForkEntry(keyBlockData, pass == 1, &f);
      if (f.storageType)
        freeForkBlocks(f.storageType, f.keyBlock);
    }
    markBlockFree(kp);
    return true;
  }

  return freeForkBlocks(storage, kp);
}

// Free the blocks of one seedling/sapling/tree structure. Used for an
// ordinary file's single structure and for each fork of an extended one.
bool ProdosSpector::freeForkBlocks(uint8_t storage, uint16_t kp)
{
  switch (storage) {
  case ft_seedling:
    // The key block *is* the single data block.
    markBlockFree(kp);
    return true;
  case ft_sapling:
    // The key block is an index block; free every data block it points to
    // (low bytes in the first half, high bytes in the second) and then the
    // index block itself. Zero pointers are sparse holes - nothing to free.
    // An out-of-range index block means the file is corrupt: there's
    // nothing real to free through it. (markBlockFree itself rejects
    // out-of-range data pointers.)
    if (!validBlock(kp)) {
      wozMsg(WOZ_MSG_WARNING,
             "WARNING: index block %u is outside the volume; freeing only "
             "the directory entry", kp);
      return true;
    }
    for (int i = 0; i < 256; i++) {
      uint16_t b = trackData[512 * kp + 256 + i] * 256 + trackData[512 * kp + i];
      if (b) markBlockFree(b);
    }
    markBlockFree(kp);
    return true;
  case ft_tree:
    // The key block is a master index of index blocks. Free each index
    // block's data blocks, then the index block, then the master. Zero
    // pointers at either level are sparse holes - nothing to free, and
    // out-of-range pointers are corruption - nothing real behind them.
    if (!validBlock(kp)) {
      wozMsg(WOZ_MSG_WARNING,
             "WARNING: master index block %u is outside the volume; freeing "
             "only the directory entry", kp);
      return true;
    }
    for (int mi = 0; mi < 128; mi++) {
      uint16_t ib = trackData[512 * kp + 256 + mi] * 256 + trackData[512 * kp + mi];
      if (!ib || !validBlock(ib)) continue;
      for (int i = 0; i < 256; i++) {
        uint16_t b = trackData[512 * ib + 256 + i] * 256 + trackData[512 * ib + i];
        if (b) markBlockFree(b);
      }
      markBlockFree(ib);
    }
    markBlockFree(kp);
    return true;
  default:
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: entry has unexpected storage type 0x%X; not deleting",
           storage);
    return false;
  }
}

bool ProdosSpector::findEntryInDir(uint16_t dirKey, const char *name,
                                   uint16_t *blockOut, uint32_t *offsetOut)
{
  size_t want = strlen(name);
  uint16_t currentBlock = dirKey;
  set<uint16_t> visited; // corrupt chains can loop or leave the volume

  while (currentBlock) {
    if (!validBlock(currentBlock) || visited.count(currentBlock))
      return false; // ran off the end of a corrupt chain: entry not found
    visited.insert(currentBlock);

    uint8_t *block = &trackData[512 * currentBlock];
    struct _idxHeader *ih = (struct _idxHeader *)block;

    for (int i = 0; i < 13; i++) {
      if (i == 0 && currentBlock == dirKey)
        continue; // the directory's own header, not a file entry
      uint32_t off = 512 * currentBlock + i * 0x27 + 4;
      struct _prodosFent *fe = (struct _prodosFent *)&trackData[off];
      if ((fe->typelen & 0xF0) == 0)
        continue; // empty or deleted slot
      uint8_t nl = fe->typelen & 0x0F;
      if (nl == want && strncmp(fe->name, name, nl) == 0) {
        *blockOut = currentBlock;
        *offsetOut = off;
        return true;
      }
    }

    currentBlock = ih->nextBlock[1] * 256 + ih->nextBlock[0];
  }
  return false;
}

bool ProdosSpector::resolveDirAndLeaf(const char *path, uint16_t *dirKeyOut,
                                      char *leaf, size_t leafSz, bool quiet)
{
  char buf[256];
  strncpy(buf, path, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *p = buf;
  if (*p == '/') p++; // a leading slash is optional; paths are root-relative

  // Each '/'-separated component up to the last names a directory to descend
  // into; the final component is the leaf the caller will act on.
  uint16_t dirKey = 2; // volume root
  char *slash;
  while ((slash = strchr(p, '/')) != NULL) {
    *slash = '\0';
    if (*p) { // tolerate empty components from doubled slashes
      uint16_t b;
      uint32_t off;
      if (!findEntryInDir(dirKey, p, &b, &off)) {
        if (!quiet) wozMsg(WOZ_MSG_ERROR, "Directory '%s' not found", p);
        return false;
      }
      struct _prodosFent *fe = (struct _prodosFent *)&trackData[off];
      if (fe->fileType != FT_DIR) {
        if (!quiet) wozMsg(WOZ_MSG_ERROR, "'%s' is not a directory", p);
        return false;
      }
      dirKey = fe->keyPointer[1] * 256 + fe->keyPointer[0];
    }
    p = slash + 1;
  }

  if (!*p) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: missing filename in path '%s'", path);
    return false;
  }
  strncpy(leaf, p, leafSz - 1);
  leaf[leafSz - 1] = '\0';
  *dirKeyOut = dirKey;
  return true;
}

Vent *ProdosSpector::findEntry(const char *path)
{
  if (!tree)
    createTree();

  uint16_t dirKey;
  char leaf[16];
  // Quiet resolution: findEntry is the existence probe, so a missing
  // intermediate directory is an answer (NULL), not something to report.
  if (!resolveDirAndLeaf(path, &dirKey, leaf, sizeof(leaf), true))
    return NULL;

  uint16_t block;
  uint32_t off;
  if (!findEntryInDir(dirKey, leaf, &block, &off))
    return NULL;

  // Return the corresponding node from the loaded tree (which read the same
  // on-disk data). A file's key pointer is unique, so match on that plus the
  // leaf name, skipping the synthetic directory-header nodes.
  struct _prodosFent *fe = (struct _prodosFent *)&trackData[off];
  uint16_t kp = fe->keyPointer[1] * 256 + fe->keyPointer[0];
  for (Vent *v = tree; v; v = v->nextEnt()) {
    if (!v->isHeader() && v->keyPointerVal() == kp &&
        !strcmp(v->getName(), leaf))
      return v;
  }
  return NULL;
}

Vent *ProdosSpector::getDirectoryListing(const char *path)
{
  if (!tree)
    createTree();

  uint16_t dirKey;
  char leaf[16];
  if (!resolveDirAndLeaf(path, &dirKey, leaf, sizeof(leaf)))
    return NULL;

  uint16_t block;
  uint32_t off;
  if (!findEntryInDir(dirKey, leaf, &block, &off)) {
    wozMsg(WOZ_MSG_ERROR, "'%s' not found", path);
    return NULL;
  }
  struct _prodosFent *fe = (struct _prodosFent *)&trackData[off];
  if (fe->fileType != FT_DIR) {
    wozMsg(WOZ_MSG_ERROR, "'%s' is not a directory", path);
    return NULL;
  }

  uint16_t subKey = fe->keyPointer[1] * 256 + fe->keyPointer[0];
  return descendTree(subKey);
}

bool ProdosSpector::removeDirEntry(uint16_t dirKey, uint16_t block,
                                   uint32_t offset)
{
  // Zero the storage-type/name-length byte. That's ProDOS's deleted marker
  // (storage type 0 = inactive), and it's also exactly what cpin's free-slot
  // scan looks for (typelen == 0), so the slot becomes reusable and vanishes
  // from the directory reader.
  trackData[offset] = 0;
  if (!writeBlock(block, &trackData[512 * block])) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write directory block %u", block);
    return false;
  }

  // Drop the active file count by one in the header of the directory that
  // owns this entry (its header lives at offset +4 of its key block).
  struct _subdirent *md = (struct _subdirent *)(&trackData[512 * dirKey + 4]);
  uint16_t fc = md->fileCount[1] * 256 + md->fileCount[0];
  if (fc) fc--;
  md->fileCount[0] = fc & 0xFF;
  md->fileCount[1] = (fc >> 8);
  if (!writeBlock(dirKey, &trackData[512 * dirKey])) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to update directory header");
    return false;
  }
  return true;
}

bool ProdosSpector::removeFile(const char *fileName)
{
  if (!tree)
    createTree();

  uint16_t dirKey;
  char leaf[16];
  if (!resolveDirAndLeaf(fileName, &dirKey, leaf, sizeof(leaf)))
    return false;

  uint16_t block;
  uint32_t off;
  if (!findEntryInDir(dirKey, leaf, &block, &off)) {
    wozMsg(WOZ_MSG_ERROR, "File '%s' not found", fileName);
    return false;
  }

  // Copy the entry out before we start mutating blocks underneath it.
  struct _prodosFent fe;
  memcpy(&fe, &trackData[off], sizeof(fe));

  if (((fe.typelen & 0xF0) >> 4) == ft_subdir) {
    wozMsg(WOZ_MSG_ERROR, "'%s' is a directory; use rmdir", fileName);
    return false;
  }

  if (!freeFileBlocks(&fe))
    return false;
  if (!removeDirEntry(dirKey, block, off))
    return false;

  flushFreeBlockList();
  createTree(); // re-read so subsequent commands see the change
  wozMsg(WOZ_MSG_INFO, "Removed '%s'", fileName);
  return true;
}

bool ProdosSpector::removeDirectory(const char *dirName)
{
  if (!tree)
    createTree();

  uint16_t dirKey;
  char leaf[16];
  if (!resolveDirAndLeaf(dirName, &dirKey, leaf, sizeof(leaf)))
    return false;

  uint16_t block;
  uint32_t off;
  if (!findEntryInDir(dirKey, leaf, &block, &off)) {
    wozMsg(WOZ_MSG_ERROR, "Directory '%s' not found", dirName);
    return false;
  }

  struct _prodosFent fe;
  memcpy(&fe, &trackData[off], sizeof(fe));

  if (((fe.typelen & 0xF0) >> 4) != ft_subdir) {
    wozMsg(WOZ_MSG_ERROR, "'%s' is not a directory; use rm", dirName);
    return false;
  }

  uint16_t keyBlock = fe.keyPointer[1] * 256 + fe.keyPointer[0];
  if (!validBlock(keyBlock)) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: '%s' points at key block %u, outside the volume; "
           "refusing", dirName, keyBlock);
    return false;
  }

  // The subdirectory's own header must be a real directory header, and it
  // must be empty - refuse otherwise (like Unix rmdir).
  struct _subdirent *sd = (struct _subdirent *)&trackData[512 * keyBlock + 4];
  if (sd->entryLength != 0x27 || sd->entriesPerBlock != 0x0D) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: '%s' does not point at a directory header; refusing",
           dirName);
    return false;
  }
  uint16_t fc = sd->fileCount[1] * 256 + sd->fileCount[0];
  if (fc != 0) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: directory '%s' is not empty (%u entr%s); refusing",
           dirName, fc, fc == 1 ? "y" : "ies");
    return false;
  }

  // Free the directory's block chain (an empty subdir is usually one block,
  // but follow the next-block links just in case it grew and emptied).
  uint16_t cur = keyBlock;
  int safety = 4096; // guard against a corrupt circular chain
  while (cur && validBlock(cur) && safety-- > 0) {
    struct _idxHeader *ih = (struct _idxHeader *)&trackData[512 * cur];
    uint16_t next = ih->nextBlock[1] * 256 + ih->nextBlock[0];
    markBlockFree(cur);
    cur = next;
  }

  if (!removeDirEntry(dirKey, block, off))
    return false;

  flushFreeBlockList();
  createTree();
  wozMsg(WOZ_MSG_INFO, "Removed directory '%s'", dirName);
  return true;
}

bool ProdosSpector::removeDirectoryRecursive(const char *dirName)
{
  if (!tree)
    createTree();

  uint16_t dirKey;
  char leaf[16];
  if (!resolveDirAndLeaf(dirName, &dirKey, leaf, sizeof(leaf)))
    return false;

  uint16_t block;
  uint32_t off;
  if (!findEntryInDir(dirKey, leaf, &block, &off)) {
    wozMsg(WOZ_MSG_ERROR, "Directory '%s' not found", dirName);
    return false;
  }

  struct _prodosFent fe;
  memcpy(&fe, &trackData[off], sizeof(fe));
  if (((fe.typelen & 0xF0) >> 4) != ft_subdir) {
    wozMsg(WOZ_MSG_ERROR, "'%s' is not a directory; use rm", dirName);
    return false;
  }

  // Empty the directory one child at a time. removeFile()/removeDirectory()
  // (and our own recursion) call createTree() and rewrite trackData, so we
  // must re-scan from the top after every removal rather than caching
  // pointers or offsets into a chain that's been rebuilt underneath us.
  for (;;) {
    uint16_t subKey = fe.keyPointer[1] * 256 + fe.keyPointer[0];

    // Find the first live entry in the subdirectory (skipping its header).
    char childLeaf[16];
    bool childIsDir = false;
    bool found = false;
    uint16_t cur = subKey;
    int safety = 4096; // guard against a corrupt circular chain
    while (cur && !found && validBlock(cur) && safety-- > 0) {
      struct _idxHeader *ih = (struct _idxHeader *)&trackData[512 * cur];
      for (int i = 0; i < 13; i++) {
        if (i == 0 && cur == subKey)
          continue; // the subdirectory's own header, not a file entry
        uint32_t coff = 512 * cur + i * 0x27 + 4;
        struct _prodosFent *cfe = (struct _prodosFent *)&trackData[coff];
        if ((cfe->typelen & 0xF0) == 0)
          continue; // empty or deleted slot
        uint8_t nl = cfe->typelen & 0x0F;
        memcpy(childLeaf, cfe->name, nl);
        childLeaf[nl] = '\0';
        childIsDir = (((cfe->typelen & 0xF0) >> 4) == ft_subdir);
        found = true;
        break;
      }
      cur = ih->nextBlock[1] * 256 + ih->nextBlock[0];
    }
    if (!found)
      break; // directory is now empty

    // Address the child by a path relative to the same root the caller used.
    char childPath[256];
    snprintf(childPath, sizeof(childPath), "%s/%s", dirName, childLeaf);
    bool ok = childIsDir ? removeDirectoryRecursive(childPath)
                         : removeFile(childPath);
    if (!ok)
      return false;

    // trackData was rewritten; re-resolve our own entry before the next pass.
    if (!resolveDirAndLeaf(dirName, &dirKey, leaf, sizeof(leaf)) ||
        !findEntryInDir(dirKey, leaf, &block, &off))
      return false;
    memcpy(&fe, &trackData[off], sizeof(fe));
  }

  return removeDirectory(dirName);
}

bool ProdosSpector::makeDirectory(const char *dirName)
{
  if (!tree)
    createTree();

  uint16_t dirKey;
  char leaf[16];
  if (!resolveDirAndLeaf(dirName, &dirKey, leaf, sizeof(leaf)))
    return false;

  size_t namelen = strlen(leaf);
  if (namelen < 1 || namelen > 15) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: directory names must be 1..15 chars in ProDOS");
    return false;
  }

  // Refuse to clobber an existing name (addDirectoryEntryForFile won't
  // check this for us).
  uint16_t existsBlock;
  uint32_t existsOff;
  if (findEntryInDir(dirKey, leaf, &existsBlock, &existsOff)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' already exists", dirName);
    return false;
  }

  // Allocate the subdirectory's key block (its single, initially-empty
  // directory block).
  uint16_t dirBlock;
  if (!findFreeBlock(&dirBlock)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: no free block for the new directory");
    return false;
  }

  // Write the entry that lives in the parent directory, pointing at the new
  // block. fileType 0x0F (DIR) is what marks it as a directory.
  struct _prodosFent fent;
  memset(&fent, 0, sizeof(fent));
  fent.typelen = (ft_subdir << 4) | namelen;
  strncpy(fent.name, leaf, 15);
  fent.fileType = FT_DIR; // 0x0F
  fent.keyPointer[0] = dirBlock & 0xFF;
  fent.keyPointer[1] = (dirBlock >> 8);
  fent.blocksUsed[0] = 1;
  fent.eofLength[0] = 0x00;
  fent.eofLength[1] = 0x02; // 512 bytes
  fent.accessFlags = at_destroyed | at_renamed | at_written | at_read;
  fent.headerPointer[0] = dirKey & 0xFF; // parent directory's key block
  fent.headerPointer[1] = (dirKey >> 8);

  if (!addDirectoryEntryForFile(dirKey, &fent)) {
    // No free directory slot; the block we grabbed is only reserved in RAM,
    // so leaving without flushing the bitmap discards that reservation.
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: couldn't add directory entry for '%s'", dirName);
    return false;
  }

  // Find where that entry actually landed so the subdirectory header can
  // point back at its parent block and entry number (ProDOS uses these to
  // walk back up the tree).
  uint16_t parentBlock;
  uint32_t parentOff;
  if (!findEntryInDir(dirKey, leaf, &parentBlock, &parentOff)) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: internal: just-written directory entry not found");
    return false;
  }
  uint8_t entryNumber = (parentOff - 4 - 512 * parentBlock) / 0x27 + 1;

  // Lay down the new directory block: zeroed, with a subdirectory header
  // as its first entry.
  uint8_t blockData[512];
  memset(blockData, 0, sizeof(blockData));
  // prev/next block links are both 0 (a one-block directory).
  struct _subdirent *sd = (struct _subdirent *)&blockData[4];
  sd->typelen = (ft_subdirhdr << 4) | namelen; // 0x0E storage type
  memcpy(sd->name, leaf, namelen);
  sd->subdir.fileType = 0x75; // ProDOS subdirectory-header signature byte
  sd->accessFlags = at_destroyed | at_renamed | at_written | at_read;
  sd->entryLength = 0x27;
  sd->entriesPerBlock = 0x0D;
  sd->fileCount[0] = 0;
  sd->fileCount[1] = 0;
  sd->subdirParent.pointer[0] = parentBlock & 0xFF;
  sd->subdirParent.pointer[1] = (parentBlock >> 8);
  sd->subParent.entry = entryNumber;
  sd->subParent.entryLength = 0x27;

  if (!writeBlock(dirBlock, blockData)) {
    wozMsg(WOZ_MSG_ERROR,
           "ERROR: failed to write new directory block %u", dirBlock);
    return false;
  }

  flushFreeBlockList();
  createTree(); // re-read so subsequent commands see the new directory
  wozMsg(WOZ_MSG_INFO, "Created directory '%s'", dirName);
  return true;
}

bool ProdosSpector::probe()
{
  // ProDOS volume directory header lives at block 2. Track 0 in
  // ProDOS-order layout holds blocks 0-7 back-to-back, so block 2 starts
  // 512 bytes into that track's data. The header entry has a zero
  // prev-block pointer, a non-zero next-block pointer, a storage_type
  // nybble of 0xF (volume directory header), and a name length 1..15.
  const uint8_t *b2 = NULL;
  uint8_t track[256*16];
  if (isHdv()) {
    // HDVs have no nibbilized track layout - just peek at block 2 in
    // the raw buffer. 2*512 = 1024.
    if (hdvByteCount() < 1024 + 512) return false;
    b2 = hdvBuffer() + 1024;
  } else {
    if (!decodeWozTrackToDsk(0, T_PO, track)) return false;
    b2 = &track[512 * 2];
  }
  if (b2[0] != 0 || b2[1] != 0) return false;
  uint16_t next = b2[2] | (b2[3] << 8);
  if (next == 0) return false;
  uint8_t stype = (b2[4] & 0xF0) >> 4;
  uint8_t namelen = b2[4] & 0x0F;
  if (stype != 0x0F) return false;
  if (namelen < 1 || namelen > 15) return false;
  return true;
}

