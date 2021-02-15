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
  cwdMasterBlock = 2; // The master directory starts @ block 2
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
    struct _subdirent *md = (struct _subdirent *)(block+4);
    
    for (int i=0; i<13; i++) {
      if (i==0 && currentBlock == 2) {
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
	  if (!ret) {
	    ret = ve;
	  } else {
	    Vent *p3 = ret;
	    while (p3->nextEnt()) {
	      p3 = p3->nextEnt();
	    }
	    p3->nextEnt(ve);
	  }
	}
      }
    }

    currentBlock = ih->nextBlock[1] * 256 + ih->nextBlock[0];
  }

  return ret;  
}

// FIXME: this only looks at CWD. How do we change that?
Vent *ProdosSpector::createTree()
{
  for (int i=0; i<35; i++) {
    if (!decodeWozTrackToDsk(i, T_PO, &trackData[i*256*16])) {
      fprintf(stderr, "Failed to read track %d\n", i);
      exit(1);
    }
  }

  return descendTree(cwdMasterBlock);
}

uint32_t ProdosSpector::getFileContents(Vent *e, char **toWhere)
{
  ///  ...
  printf("Unimplemented\n");
  *toWhere = NULL;
  return 0;
}

