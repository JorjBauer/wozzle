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

ProdosSpector::ProdosSpector(bool verbose, uint8_t dumpflags) : Wozspector(verbose, dumpflags)
{
}

ProdosSpector::~ProdosSpector()
{
}

// Read a volume bitmap and return the number of blocks free in it.
// Assumes it's a floppy image bitmap, where the number of blocks
// on the device won't exceed one block of bits...
static uint16_t calculateBlocksFree(uint8_t block[512], uint16_t maxBlocks)
{
  uint8_t ptr = 0;
  uint8_t bits = 0x80;
  uint16_t count = 0;
  for (int i=0; i<maxBlocks; i++) {
    if (block[ptr] & bits) {
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

// block to sector maps (2 sectors for each block)
const uint8_t s1map[8] = { 0, 4, 8, 12, 1, 5, 9, 13 };
const uint8_t s2map[8] = { 2, 6, 10, 14, 3, 7, 11, 15 };
#define trackFromBlock(x) (((x)/16)*2)
#define blockInTrack(x) ((x)%16)
static void printFreeBlocks(uint8_t block[512])
{
  bool blockFree[280]; // FIXME assumes a 280-block disk
  bool trackSectorFree[35][16];
  memset(trackSectorFree, 0, sizeof(trackSectorFree));
  uint8_t ptr = 0;
  uint8_t bits = 0x80;
  for (int i=0; i<280; i++) {
    blockFree[i] = (block[ptr] & bits) ? true : false;
    trackSectorFree[trackFromBlock(i)][s1map[blockInTrack(i)]] = (block[ptr] & bits) ? true : false;
    trackSectorFree[trackFromBlock(i)][s2map[blockInTrack(i)]] = (block[ptr] & bits) ? true : false;
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

Vent *ProdosSpector::createTree()
{
  if (tree) {
    freeTree(tree);
    tree = NULL;
  }
  
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

