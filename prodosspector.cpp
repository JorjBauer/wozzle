#include "prodosspector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Prodos disk map parser

#include "vent.h"

#include <stack>
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

ProdosSpector::ProdosSpector(bool verbose, uint8_t dumpflags) : Wozspector(verbose, dumpflags)
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
      fprintf(stderr, "Failed to read track %d\n", i);
      return false;
    }
  }
  return true;
}

// Read a volume bitmap and return the number of blocks free in it.
// Assumes it's a floppy image bitmap, where the number of blocks
// on the device won't exceed one block of bits...
uint16_t ProdosSpector::calculateBlocksFree()
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

void ProdosSpector::printFreeBlocks()
{
  if (!tree)
    createTree();

  // For a floppy-sized volume (≤280 blocks), show the familiar
  // track/sector grid. For anything larger we'd need a screenful per
  // hundred blocks, so just emit a linear block-level map in rows.
  uint8_t *getBit = freeBlockBitmap;
  auto isFree = [&](uint32_t i) -> bool {
    return (getBit[i / 8] & (0x80 >> (i % 8))) != 0;
  };

  if (numBlocksTotal <= 280) {
    bool trackSectorFree[35][16];
    memset(trackSectorFree, 0, sizeof(trackSectorFree));
    for (int i = 0; i < (int)numBlocksTotal; i++) {
      bool f = isFree(i);
      trackSectorFree[trackFromBlock(i)][s1map[blockInTrack(i)]] = f;
      trackSectorFree[trackFromBlock(i)][s2map[blockInTrack(i)]] = f;
    }

    printf("\n");
    char col[16] = { 'S','E','C','T','O','R',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};
    printf("   TRACK           1               2  \n");
    printf("   0123456789ABCDEF0123456789ABCDEF012\n\n");
    for (int i = 0; i < 16; i++) {
      printf("%c%X ", col[i], i);
      for (int t = 0; t < 35; t++) {
        printf("%c", trackSectorFree[t][i] ? '.' : '*');
      }
      printf("\n");
    }
    printf("\n");
    return;
  }

  // Large (HDV) case: 64 blocks per row, one char per block.
  printf("\nBlock free map (64 blocks/row):\n");
  for (uint32_t i = 0; i < numBlocksTotal; i += 64) {
    printf("%6u  ", i);
    for (uint32_t j = 0; j < 64 && (i + j) < numBlocksTotal; j++) {
      printf("%c", isFree(i + j) ? '.' : '*');
    }
    printf("\n");
  }
  printf("\n");
}

Vent *ProdosSpector::descendTree(uint16_t fromBlock)
{
  Vent *ret = NULL;

  uint16_t currentBlock = fromBlock;
  uint32_t activeEntriesFound = 0;  // full-scan count, to cross-check header

  while (currentBlock) {
    uint8_t *block;
    block = &trackData[512 * currentBlock];

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
	  fprintf(stderr,
		  "WARNING: block %u was walked as a directory but its header "
		  "is not one (entryLength=0x%02X, entriesPerBlock=0x%02X); "
		  "skipping it.\n",
		  currentBlock, md->entryLength, md->entriesPerBlock);
	  return ret; // ret is still NULL here
	}
	Vent *sde = new Vent(md);
	assert(ret == NULL); // shouldn't have been initialized yet?
	ret = sde;

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
            fprintf(stderr, "ERROR: out of memory for volume bitmap\n");
            return NULL;
          }
          // FIXME check volBitmapBlock + bitmapBlocks < numBlocksTotal
          // - readBlock would bounce off the un-prepped tree, so pull
          // the bytes from our own trackData directly here.
          memcpy(freeBlockBitmap,
                 &trackData[512 * volBitmapBlock],
                 freeBlockBitmapBytes);
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
      printf("WARNING: directory '%s' header says %u active entries "
             "but the full scan found %u. ProDOS 1.1.1 will only see "
             "the first %u via CAT.\n",
             ret->getName(), declared,
             activeEntriesFound, declared);
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
      printf("> use block %u (0x%X)\n", i, i); // debugging
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
  return writeBlock(volBitmapBlock, freeBlockBitmap);
}

// Since we preloaded the whole image, this just has to copy data
bool ProdosSpector::readBlock(uint16_t blockNum, uint8_t dataOut[512])
{
  // Caller must have prepped with createTree() call first, or we fail
  if (!tree)
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
    fprintf(stderr, "Failed to load block buffer\n");
    return NULL;
  }

  stack <int> s;
  s.push(2); // directory starts on block 2

  while (!s.empty()) {
    int nextBlock = s.top(); s.pop();
    Vent *nextDir = descendTree(nextBlock);
    Vent *p = nextDir;
    while (p) {
      if (p->isDirectory()) {
	s.push(p->keyPointerVal());
      }
      p = p->nextEnt();
    }
    appendTree(nextDir);
  }

  return tree;
}

uint32_t ProdosSpector::getFileContents(Vent *e, char **toWhere)
{
  // Given the virtual entry 'e', find the file's contents and return
  //them in *toWhere. We malloc() that result, and the caller will
  //free it.
    
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
  
  switch (indexType) {
  case 1:
    // Seedling file: we point right to the data
    *toWhere = (char *)malloc(l); // FIXME check for error
    memcpy(*toWhere, &trackData[512 * kp], l);
    return l;
  case 2:
    // sapling file: 2-256 data blocks. The index block has the low bytes in the
    // first 256, and the high bytes in the second 256.
    {
      uint32_t sizeRemaining = l;
      uint32_t bytesCopied = 0;
      *toWhere = (char *)malloc(l); // FIXME check for error
      uint8_t idx = 0;
      while (sizeRemaining) {
        uint32_t nextBlockOfData = trackData[512*kp + 256 + idx] * 256 + trackData[512*kp + idx];
        if (sizeRemaining >= 512) {
          memcpy((*toWhere) + bytesCopied, &trackData[512*nextBlockOfData], 512);
          sizeRemaining -= 512;
          bytesCopied += 512;
        } else {
          memcpy((*toWhere) + bytesCopied, &trackData[512*nextBlockOfData], sizeRemaining);
          bytesCopied += sizeRemaining;
          sizeRemaining = 0;
        }
        idx++;
      }
    }
    return l;
                 
  case 3:
    // tree file: 257 - 32768 data blocks
    printf("UNIMPLEMENTED\n");
    break;
  default:
    // deleted or reserved or something; skip it
    printf("Unimplemented file type '%d'\n", indexType);
    *toWhere = NULL;
    return 0;
  }
  
  
  printf("Unimplemented\n");
  *toWhere = NULL;
  return 0;
}

bool ProdosSpector::writeFileToImage(uint8_t *fileContents,
                                     char *fileName,
                                     uint8_t fileType,
                                     uint16_t auxTypeData,
                                     uint32_t fileSize)
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
    printf("ERROR: file names may not exceed 15 chars in ProDOS\n");
    return false;
  }
  strncpy(fent.name, leaf, 15);
  // Set the low nybble of typelen to the length of the filename
  fent.typelen = leaflen;
  uint8_t blocksUsed = (fileSize / 512)+1; // number of data blocks

  fent.fileType = fileType;
  fent.eofLength[0] = fileSize & 0xFF;
  fent.eofLength[1] = (fileSize >> 8) & 0xFF;
  fent.eofLength[2] = (fileSize >> 16);

  // FIXME: fent.creationDate could be set to today's date/time instead of 0
  // FIXME: same with lastModified
  // FIXME: creatorVersion is still 0, meaning proDOS 1.0?
  // FIXME:   same with minRequiredVersion
  
  // FIXME: should these access flags be different?
  fent.accessFlags = at_destroyed | at_renamed | at_written | at_read;

  fent.typeData[0] = auxTypeData & 0xFF;
  fent.typeData[1] = (auxTypeData >> 8);

  // headerPointer is the key block of the directory that holds this entry.
  fent.headerPointer[0] = dirKey & 0xFF;
  fent.headerPointer[1] = (dirKey >> 8);
  
  // Create the file structure - seedling for <= 512 bytes; sapling for <=128k;
  // or tree for >128k.
  if (fileSize <= 512) {
    // Seedling file entry
    // The KEY_POINTER in the directory block points directly to the file
    // data
    fent.typelen |= (ft_seedling<<4);
    
    // Allocate one block for the file
    uint16_t firstBlock;
    if (!findFreeBlock(&firstBlock)) {
      printf("Failed to find a free block for the data\n");
      return false;
    }
    uint8_t paddedData[512];
    memset(&paddedData, 0, sizeof(paddedData));
    memcpy(paddedData, fileContents, fileSize);
    if (!writeBlock(firstBlock, paddedData)) {
      printf("Failed to write data block\n");
      return false;
    }

    fent.keyPointer[0] = firstBlock & 0xFF;
    fent.keyPointer[1] = (firstBlock >> 8);
  } else if (fileSize <= 128*1024) {
    // Sapling file entry
    // We need an index block that points to all of the data blocks, and the
    // index block number is stored in KEY_POINTER.
    fent.typelen |= (ft_sapling<<4);
    blocksUsed++;
    uint16_t indexBlock;
    if (!findFreeBlock(&indexBlock)) {
      printf("Failed to find a free block for the index\n");
      return false;
    }
    uint8_t indexBlockData[512];
    uint8_t indexBlockPtr = 0;
    memset(indexBlockData, 0, sizeof(indexBlockData));

    fent.keyPointer[0] = indexBlock & 0xFF;
    fent.keyPointer[1] = (indexBlock >> 8);

    
    // Allocate the blocks for the file data and start copying it in
    uint32_t bytesRemaining = fileSize;
    for (int idx = 0; idx < fileSize; idx+= 512, bytesRemaining -= 512) {
      uint16_t nextBlock;
      if (!findFreeBlock(&nextBlock)) {
        printf("Failed to find next free block for the data\n");
        return false;
      }

      indexBlockData[indexBlockPtr] = nextBlock & 0xFF;
      indexBlockData[0x100 + indexBlockPtr++] = (nextBlock >> 8);

      uint8_t paddedData[512];
      memset(&paddedData, 0, sizeof(paddedData));
      memcpy(paddedData, &fileContents[idx],
             bytesRemaining >= 512 ? 512 : bytesRemaining);
      if (!writeBlock(nextBlock, paddedData)) {
        printf("Failed to write next data block\n");
        return false;
      }
    }

    if (!writeBlock(indexBlock, indexBlockData)) {
      printf("Failed to write index block\n");
      return false;
    }
  } else {
    // FIXME: tree not implemented
  }

  printf("Blocks used to store this file: %d\n", blocksUsed);
  fent.blocksUsed[0] = blocksUsed & 0xFF;
  fent.blocksUsed[1] = (blocksUsed >> 8);

  // Write the directory entry
  if (!addDirectoryEntryForFile(dirKey, &fent)) {
    printf("Error writing directory entry for file\n");
    return false;
  }
  
  // Flush the free block list
  flushFreeBlockList();

  // re-read the catalog
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
  // span several blocks via the next-block links; we don't yet grow a full
  // directory by allocating a new block.
  uint16_t currentBlock = dirKey;

  while (currentBlock) {
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
            printf("Error: can't write block data for directory\n");
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
            printf("Failed to write directory header back out\n");
            return false;
          }
          return true;
        }
      }
    }

    currentBlock = ih->nextBlock[1] * 256 + ih->nextBlock[0];
  }

  printf("Error: no free directory entries, can't store file\n");
  return false;
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

  switch (storage) {
  case ft_seedling:
    // The key block *is* the single data block.
    markBlockFree(kp);
    return true;
  case ft_sapling:
    // The key block is an index block; free every data block it points to
    // (low bytes in the first half, high bytes in the second) and then the
    // index block itself. Zero pointers are sparse holes - nothing to free.
    for (int i = 0; i < 256; i++) {
      uint16_t b = trackData[512 * kp + 256 + i] * 256 + trackData[512 * kp + i];
      if (b) markBlockFree(b);
    }
    markBlockFree(kp);
    return true;
  case ft_tree:
    printf("ERROR: deleting tree-structured files is not implemented\n");
    return false;
  default:
    printf("ERROR: entry has unexpected storage type 0x%X; not deleting\n",
           storage);
    return false;
  }
}

bool ProdosSpector::findEntryInDir(uint16_t dirKey, const char *name,
                                   uint16_t *blockOut, uint32_t *offsetOut)
{
  size_t want = strlen(name);
  uint16_t currentBlock = dirKey;

  while (currentBlock) {
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
                                      char *leaf, size_t leafSz)
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
        printf("Directory '%s' not found\n", p);
        return false;
      }
      struct _prodosFent *fe = (struct _prodosFent *)&trackData[off];
      if (fe->fileType != FT_DIR) {
        printf("'%s' is not a directory\n", p);
        return false;
      }
      dirKey = fe->keyPointer[1] * 256 + fe->keyPointer[0];
    }
    p = slash + 1;
  }

  if (!*p) {
    printf("ERROR: missing filename in path '%s'\n", path);
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
  if (!resolveDirAndLeaf(path, &dirKey, leaf, sizeof(leaf)))
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

void ProdosSpector::displayDirectory(const char *path)
{
  if (!tree)
    createTree();

  uint16_t dirKey;
  char leaf[16];
  if (!resolveDirAndLeaf(path, &dirKey, leaf, sizeof(leaf)))
    return;

  uint16_t block;
  uint32_t off;
  if (!findEntryInDir(dirKey, leaf, &block, &off)) {
    printf("'%s' not found\n", path);
    return;
  }
  struct _prodosFent *fe = (struct _prodosFent *)&trackData[off];
  if (fe->fileType != FT_DIR) {
    printf("'%s' is not a directory\n", path);
    return;
  }

  uint16_t subKey = fe->keyPointer[1] * 256 + fe->keyPointer[0];
  Vent *list = descendTree(subKey);
  displayTree(list);
  freeTree(list);
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
    printf("ERROR: failed to write directory block %u\n", block);
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
    printf("ERROR: failed to update directory header\n");
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
    printf("File '%s' not found\n", fileName);
    return false;
  }

  // Copy the entry out before we start mutating blocks underneath it.
  struct _prodosFent fe;
  memcpy(&fe, &trackData[off], sizeof(fe));

  if (((fe.typelen & 0xF0) >> 4) == ft_subdir) {
    printf("'%s' is a directory; use rmdir\n", fileName);
    return false;
  }

  if (!freeFileBlocks(&fe))
    return false;
  if (!removeDirEntry(dirKey, block, off))
    return false;

  flushFreeBlockList();
  createTree(); // re-read so subsequent commands see the change
  printf("Removed '%s'\n", fileName);
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
    printf("Directory '%s' not found\n", dirName);
    return false;
  }

  struct _prodosFent fe;
  memcpy(&fe, &trackData[off], sizeof(fe));

  if (((fe.typelen & 0xF0) >> 4) != ft_subdir) {
    printf("'%s' is not a directory; use rm\n", dirName);
    return false;
  }

  uint16_t keyBlock = fe.keyPointer[1] * 256 + fe.keyPointer[0];

  // The subdirectory's own header must be a real directory header, and it
  // must be empty - refuse otherwise (like Unix rmdir).
  struct _subdirent *sd = (struct _subdirent *)&trackData[512 * keyBlock + 4];
  if (sd->entryLength != 0x27 || sd->entriesPerBlock != 0x0D) {
    printf("ERROR: '%s' does not point at a directory header; refusing\n",
           dirName);
    return false;
  }
  uint16_t fc = sd->fileCount[1] * 256 + sd->fileCount[0];
  if (fc != 0) {
    printf("ERROR: directory '%s' is not empty (%u entr%s); refusing\n",
           dirName, fc, fc == 1 ? "y" : "ies");
    return false;
  }

  // Free the directory's block chain (an empty subdir is usually one block,
  // but follow the next-block links just in case it grew and emptied).
  uint16_t cur = keyBlock;
  int safety = 4096; // guard against a corrupt circular chain
  while (cur && safety-- > 0) {
    struct _idxHeader *ih = (struct _idxHeader *)&trackData[512 * cur];
    uint16_t next = ih->nextBlock[1] * 256 + ih->nextBlock[0];
    markBlockFree(cur);
    cur = next;
  }

  if (!removeDirEntry(dirKey, block, off))
    return false;

  flushFreeBlockList();
  createTree();
  printf("Removed directory '%s'\n", dirName);
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
    printf("Directory '%s' not found\n", dirName);
    return false;
  }

  struct _prodosFent fe;
  memcpy(&fe, &trackData[off], sizeof(fe));
  if (((fe.typelen & 0xF0) >> 4) != ft_subdir) {
    printf("'%s' is not a directory; use rm\n", dirName);
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
    while (cur && !found && safety-- > 0) {
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
    printf("ERROR: directory names must be 1..15 chars in ProDOS\n");
    return false;
  }

  // Refuse to clobber an existing name (addDirectoryEntryForFile won't
  // check this for us).
  uint16_t existsBlock;
  uint32_t existsOff;
  if (findEntryInDir(dirKey, leaf, &existsBlock, &existsOff)) {
    printf("ERROR: '%s' already exists\n", dirName);
    return false;
  }

  // Allocate the subdirectory's key block (its single, initially-empty
  // directory block).
  uint16_t dirBlock;
  if (!findFreeBlock(&dirBlock)) {
    printf("ERROR: no free block for the new directory\n");
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
    printf("ERROR: couldn't add directory entry for '%s'\n", dirName);
    return false;
  }

  // Find where that entry actually landed so the subdirectory header can
  // point back at its parent block and entry number (ProDOS uses these to
  // walk back up the tree).
  uint16_t parentBlock;
  uint32_t parentOff;
  if (!findEntryInDir(dirKey, leaf, &parentBlock, &parentOff)) {
    printf("ERROR: internal: just-written directory entry not found\n");
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
    printf("ERROR: failed to write new directory block %u\n", dirBlock);
    return false;
  }

  flushFreeBlockList();
  createTree(); // re-read so subsequent commands see the new directory
  printf("Created directory '%s'\n", dirName);
  return true;
}

void ProdosSpector::displayInfo()
{
  if (!tree)
    createTree();

  printFreeBlocks();
  printf("\nBlocks total: %u\nBlocks free: %u\n",
         numBlocksTotal, (unsigned)calculateBlocksFree());
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

void ProdosSpector::inspectFile(const char *fileName, Vent *fp)
{
  uint32_t fileSize = fp->getEofLength();
  if (fileSize <= 512) {
    printf("seedling\n");
    if (fp->getStorageType() != ft_seedling) {
      printf("WARNING: type in dir says 0x%X, seedling should be 0x%X\n", fp->getStorageType(), ft_seedling);
    }
  } else if (fileSize <= 128*1024) {
    printf("sapling\n");
    if (fp->getStorageType() != ft_sapling) {
      printf("WARNING: type in dir says 0x%X, sapling should be 0x%X\n", fp->getStorageType(), ft_sapling);
    }
    uint16_t kpv = fp->keyPointerVal();
    printf("looking up key pointer %d (0x%X)\n", kpv, kpv);
    uint8_t blockData[512];
    if (!readBlock(kpv, blockData)) {
      printf("ERROR: unable to read block\n");
      return;
    }
    printf("Sapling index block data:\n");
    for (int i=0; i<512; i+=16) {
      printf("%.4X  ", i);
      for (int j=0; j<16; j++) {
        printf("%.2X ", blockData[i+j]);
      }
      printf("\n");
    }
  } else {
    printf("tree\n");
    printf("(Tree entries are not coded yet; cannot inspect.)\n");
  }
             
}

