#include "vmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Prodos disk map parser

#include "vent.h"

struct _idxHeader {
  uint8_t prevBlock[2];
  uint8_t nextBlock[2];
};

VMap::VMap()
{
}

VMap::~VMap()
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

// This expects trackData to be in ProDos (block) order
Vent *VMap::createTree(uint8_t *trackData, int masterBlock)
{
  uint8_t *block;
  uint16_t currentBlock = masterBlock;

  Vent *ret = NULL;

  while (currentBlock) {
    block = &trackData[512 * currentBlock];

    struct _idxHeader *ih = (struct _idxHeader *)block;
    struct _subdirent *md = (struct _subdirent *)(block+4);

    for (int i=0; i<13; i++) {
      if (i==0 && currentBlock == masterBlock) {
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

void VMap::freeTree(Vent *tree)
{
  if (!tree) return;
  
  do {
    Vent *t = tree;
    tree = tree->nextEnt();
    delete t;
  } while (tree);
}

void VMap::displayTree(Vent *tree)
{
  while (tree) {
    tree->Dump();
    tree = tree->nextEnt();
  }
}

uint32_t VMap::getFileContents(Vent *e, char **toWhere)
{
  ///  ...
  printf("Unimplemented\n");
  *toWhere = NULL;
  return 0;
}

