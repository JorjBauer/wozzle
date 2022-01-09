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
  memset(freeBlockBitmap, 0, sizeof(freeBlockBitmap));
  volBitmapBlock = 6; // safe default; it's usually in block 6
  numBlocksTotal = 280; // safe default
}

ProdosSpector::~ProdosSpector()
{
}

// Read a volume bitmap and return the number of blocks free in it.
// Assumes it's a floppy image bitmap, where the number of blocks
// on the device won't exceed one block of bits...
uint16_t ProdosSpector::calculateBlocksFree()
{
  if (!tree)
    createTree();

  uint16_t maxBlocks = numBlocksTotal;

  uint8_t ptr = 0;
  uint8_t bits = 0x80;
  uint16_t count = 0;
  for (int i=0; i<maxBlocks; i++) {
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
  
  bool blockFree[280]; // FIXME assumes a 280-block disk
  bool trackSectorFree[35][16];
  memset(trackSectorFree, 0, sizeof(trackSectorFree));
  uint8_t ptr = 0;
  uint8_t bits = 0x80;
  for (int i=0; i<280; i++) {
    blockFree[i] = (freeBlockBitmap[ptr] & bits) ? true : false;
    trackSectorFree[trackFromBlock(i)][s1map[blockInTrack(i)]] = (freeBlockBitmap[ptr] & bits) ? true : false;
    trackSectorFree[trackFromBlock(i)][s2map[blockInTrack(i)]] = (freeBlockBitmap[ptr] & bits) ? true : false;
    bits >>= 1;
    if (bits == 0) {
      bits = 0x80;
      ptr++;
    }
  }

  printf("\n");
  char col[16] = { 'S', 'E', 'C', 'T', 'O', 'R', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  printf("   TRACK           1               2  \n");
  printf("   0123456789ABCDEF0123456789ABCDEF012\n");
  printf("\n");
  for (int i=0; i<16; i++) {
    printf("%c%X ", col[i], i);
    for (int t=0; t<35; t++) {
      if (trackSectorFree[t][i]) {
	printf(".");
      } else {
	printf("*");
      }
    }
    printf("\n");
  }
  printf("\n");
}

Vent *ProdosSpector::descendTree(uint16_t fromBlock)
{
  Vent *ret = NULL;

  uint16_t currentBlock = fromBlock;
  
  while (currentBlock) {
    uint8_t *block;
    block = &trackData[512 * currentBlock];
    
    struct _idxHeader *ih = (struct _idxHeader *)block;
    
    for (int i=0; i<13; i++) {
      if (i==0 && currentBlock == fromBlock) {
	// The first entry of the first block is header data
	struct _subdirent *md = (struct _subdirent *)(block+4);
	Vent *sde = new Vent(md);
	assert(ret == NULL); // shouldn't have been initialized yet?
	ret = sde;

        // FIXME hard-coded start of directory block, to differentiate the
        // volume header from a subdirectory header
        // FIXME this should look at the type bits to see that it's a
        // vol header instead of using '2'
        if (currentBlock == 2) {
          // Also load the free block bitmap
          volBitmapBlock = md->volBitmap.pointer[1]*256 + md->volBitmap.pointer[0];
          // FIXME check that it's a valid block number based on maxBlocks
          // This would be:      readBlock(volBitmapBlock, freeBlockBitmap);
          // ... but that reads from the cache, which checks to see that the
          // tree has been loaded or it returns an error; and we haven't
          // finished loading the tree, so that can't work. Instead:
          memcpy(freeBlockBitmap, &trackData[512*volBitmapBlock], 512);
          numBlocksTotal = md->volBlocks.total[1]*256 + md->volBlocks.total[0];
        }
      } else {
	struct _prodosFent *fe = (struct _prodosFent *)&trackData[512 * currentBlock + i*0x27 + 4];
	// if (fe->typen & 0xF0) == 0, then it's an empty directory entry.
	if (fe->typelen & 0xF0) {
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

  return ret;  
}

// side effect: marks it as used in RAM until flushFreeBlockList() is called
bool ProdosSpector::findFreeBlock(uint16_t *blockOut)
{
  // tree must be loaded or we will fail
  if (!tree)
    return false;

  uint16_t maxBlocks = numBlocksTotal;

  uint8_t ptr = 0;
  uint8_t bits = 0x80;
  for (int i=0; i<maxBlocks; i++) {
    if (freeBlockBitmap[ptr] & bits) {
      // mark it as used
      freeBlockBitmap[ptr] &= ~bits;
      // return the value
      *blockOut = i;
      printf("> use block %d (0x%X)\n", i, i); // debugging
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

  // update our local cached data, b/c we loaded it on createTree()
  memcpy(&trackData[512*blockNum], data, 512);

  // update the woz image data
  uint8_t t = trackFromBlock(blockNum);
  // We have to write 2 sectors for a whole block
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

  // For now we're reading the whole disk image in before starting
  // FIXME could optimize this to load on demand
  for (int i=0; i<35; i++) {
    if (!decodeWozTrackToDsk(i, T_PO, &trackData[i*256*16])) {
      fprintf(stderr, "Failed to read track %d\n", i);
      exit(1);
    }
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
  uint32_t l = 0; // length varies based on type of file -
  switch (e->getFileType()) {
  case FT_BIN:
    l = e->getEofLength();
    break;
  case FT_TXT:
    l = e->getEofLength();
    break;
  case FT_BAS:
    l = e->getEofLength();
    break;
  default:
    printf("unhandled type %d\n", e->getFileType());
  }

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

// FIXME path support in subdirectories?
bool ProdosSpector::writeFileToImage(uint8_t *fileContents,
                                     char *fileName,
                                     uint8_t fileType,
                                     uint16_t auxTypeData,
                                     uint32_t fileSize)
{
  if (!tree)
    createTree();
  
  // Construct a new filesystem entry structure
  struct _prodosFent fent;
  memset(&fent, 0, sizeof(fent));
  strncpy(fent.name, fileName, 15);
  // Set the low nybble of typelen to the length of the filename
  fent.typelen = strlen(fileName);
  if (fent.typelen > 15) {
    printf("ERROR: file names may not exceed 15 chars in ProDOS\n");
    return false;
  }
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

  // FIXME: what is headerPointer? Am I right that it's the beginning of the
  // subdir that stores the file? Since we don't do paths yet, that would be
  // block # 2?
  fent.headerPointer[0] = 2;
  fent.headerPointer[1] = 0;
  
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
  if (!addDirectoryEntryForFile(&fent)) {
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

bool ProdosSpector::addDirectoryEntryForFile(struct _prodosFent *e)
{
  // The tree has to be loaded before we start; that loads the block cache
  if (!tree)
    createTree();

  // FIXME: what if the target filename already exists?
  
  // FIXME we only know how to write in the root directory @ block 2, not
  // in any subdirectory on another block
  uint16_t currentBlock = 2;

  while (currentBlock) {
    uint8_t *block;
    block = &trackData[512 * currentBlock];
    
    struct _idxHeader *ih = (struct _idxHeader *)block;
    
    for (int i=0; i<13; i++) {
      if (i==0 && currentBlock == 2) { // FIXME hard-coded '2'
        // We have to bump up the fileCount in the header... AFTER we succeed,
        // so skip it for now
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
          
          // Now update the directory header with the right number of entries
          // FIXME: hard-coded numbers: 512*2 for sector #2 (hard coded) and
          // offset +4 (always +4 for the subdir ent start position)
          struct _subdirent *md = (struct _subdirent *)(&trackData[512*2+4]);
          uint16_t fc = md->fileCount[1]*256 + md->fileCount[0];
          fc++;
          md->fileCount[0] = fc & 0xFF;
          md->fileCount[1] = (fc >> 8);
          // That will have updated directly in the trackData cache; just need
          // to write the block back to the Woz image
          if (!writeBlock(2 /* FIXME hardcoded block num */,
                          &trackData[512*2 /* FIXME hardcoded block num */])) {
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
    
void ProdosSpector::displayInfo()
{
  if (!tree)
    createTree();
  
  printFreeBlocks();
  printf("\nBlocks total: %d\nBlocks free: %d\n", numBlocksTotal, calculateBlocksFree());
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

