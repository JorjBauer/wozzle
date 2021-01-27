#include "vmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vent.h"

struct _dirhdr {
  uint8_t typelen; // high nybble: type; low nybble: length of name
  char name[15];
  uint8_t reserved1[8];
  uint8_t creationDate[4];
  uint8_t creatorVersion;
  uint8_t minRequiredVersion;
  uint8_t accessFlags;
  uint8_t entryLength;
  uint8_t entriesPerBlock;
  uint8_t numEntries[2];
  uint8_t bitmapPtr[2];
  uint8_t totalBlocks[2];
};

Vent *volumeTree = NULL;

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
void VMap::DecodeVMap(uint8_t trackData[256*16])
{
  uint8_t *block;

  bool nextIsDirhdr = true;
  uint32_t totalBlocks = 0;
  uint16_t calculatedBlocksFree = 0;
  uint16_t volumeBitmapBlock = 6; // assumption

  uint16_t currentBlock = 2;

  while (currentBlock <= 5) {
    block = &trackData[512 * currentBlock];
    uint8_t *p = (uint8_t *)&block[4];
    for (int i=0; i<13; i++) {
      if (nextIsDirhdr) {
	struct _dirhdr *ve = (struct _dirhdr *)(p);
	// Print the volume directory header data
	if (((ve->typelen >> 4) & 0x0F) != 0x0F) {
	  printf("ERROR: type of dir header should be $0F. Aborting.\n");
	  exit(1);
	}
	if (ve->entryLength != sizeof(struct _dirhdr)) {
	  printf("ERROR: expected directory entry size does not match. Aborting.\n");
	  exit(1);
	}
	if (ve->entriesPerBlock != 13) {
	  printf("ERROR: expected 13 entries per block. Aborting.\n");
	  exit(1);
	}
	printf("/");
	for (int j=0; j<(ve->typelen & 0x0F); j++) {
	  printf("%c", ve->name[j] & 0x7F);
	}
	printf("\n\n NAME            TYPE  BLOCKS  MODIFIED      	 CREATED     	ENDFILE SUBTYPE\n");
	totalBlocks = ve->totalBlocks[1] * 256 + ve->totalBlocks[0];
	volumeBitmapBlock = ve->bitmapPtr[1] * 256 + ve->bitmapPtr[0];
	if (volumeBitmapBlock < 8) {
	  uint8_t *bitmapBlock;
	  bitmapBlock = &trackData[512 * volumeBitmapBlock];
	  calculatedBlocksFree = calculateBlocksFree(bitmapBlock, totalBlocks);
	}
	
	if (0) {
	  printf("Num active entries: %d\n", ve->numEntries[1] * 256 + ve->numEntries[0]);
	  printf("Bitmap pointer block: %d\n", ve->bitmapPtr[1] * 256 + ve->bitmapPtr[0]);
	  printf("Total blocks: %d\n", totalBlocks);
	}
	
	nextIsDirhdr = false;
      } else {
	struct _fent *p2 = (struct _fent *)(p);
	if (p2->typelen & 0xF0) {
	  Vent *ve = new Vent(p2);
	  if (!volumeTree) {
	    volumeTree = ve;
	  } else {
	    Vent *p3 = volumeTree;
	    while (p3->nextEnt()) {
	      p3 = p3->nextEnt();
	    }
	    p3->nextEnt(ve);
	  }
	}
      }
      p += sizeof(struct _dirhdr);
    }
    currentBlock++;
  }

  while (volumeTree) {
    volumeTree->Dump();
    Vent *tmp = volumeTree->nextEnt();
    delete volumeTree;
    volumeTree = tmp;
  }
  
  printf("\nBLOCKS FREE: %4d      BLOCKS USED: %4d     TOTAL BLOCKS: %4d\n",
	 calculatedBlocksFree, totalBlocks - calculatedBlocksFree, totalBlocks);

  printFreeBlocks(&trackData[512 * volumeBitmapBlock]);
}
