#include "vmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

struct _fent {
  uint8_t typelen; // high nybble: type; low nybble: length of name
  char name[15];
  uint8_t fileType;
  uint8_t keyPointer[2];
  uint8_t blocksUsed[2];
  uint8_t eofLength[3];
  uint8_t creationDate[4];
  uint8_t creatorVersion;
  uint8_t minRequiredVersion;
  uint8_t accessFlags;
  uint8_t typeData[2];
  uint8_t lastModified[4];
  uint8_t headerPointer[2];
};

struct _vent {
  union {
    struct _dirhdr dirhdr;
    struct _fent file;
  };
};
  

VMap::VMap()
{
}

VMap::~VMap()
{
}

static void printDate(uint8_t dateData[4])
{
  // 7 bits of year; 4 bits of month; 5 bits of day;
  // 000; 5 bits of hour; 00; 6 bits of minute
  // Byte order is [1][0][3][2]
  uint8_t year = dateData[1] >> 1;
  uint8_t month = ((dateData[1] & 0x01) << 3) | ((dateData[0] & 0xE0) >> 5);
  uint8_t day = dateData[0] & 0x1F;
  uint8_t hour = dateData[3] & 0x1F;
  uint8_t minute = dateData[2] & 0x3F;
  static const char *months[12] = { "JAN", "FEB", "MAR", "APR", "MAY",
    "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
  if (month > 11) month = 1; // defend the array
  if (month < 1) month = 1;
  printf("%2d-%s-%.2d %2d:%.2d", day, months[month-1], year, hour, minute);
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
  uint32_t blocksUsed = 0;
  uint16_t calculatedBlocksFree = 0;
  uint16_t volumeBitmapBlock = 6; // assumption

  uint16_t currentBlock = 2;

  while (currentBlock <= 5) {
    block = &trackData[512 * currentBlock];
    uint8_t *p = (uint8_t *)&block[4];
    for (int i=0; i<13; i++) {
      struct _vent *ve = (struct _vent *)(p);
      
      if (nextIsDirhdr) {
	// Print the volume directory header data
	if (((ve->dirhdr.typelen >> 4) & 0x0F) != 0x0F) {
	  printf("ERROR: type of dir header should be $0F. Aborting.\n");
	  exit(1);
	}
	if (ve->dirhdr.entryLength != sizeof(struct _fent)) {
	  printf("ERROR: expected directory entry size does not match. Aborting.\n");
	  exit(1);
	}
	if (ve->dirhdr.entriesPerBlock != 13) {
	  printf("ERROR: expected 13 entries per block. Aborting.\n");
	  exit(1);
	}
	printf("/");
	for (int j=0; j<(ve->dirhdr.typelen & 0x0F); j++) {
	  printf("%c", ve->dirhdr.name[j] & 0x7F);
	}
	printf("\n\n NAME            TYPE  BLOCKS  MODIFIED      	 CREATED     	ENDFILE SUBTYPE\n");
	totalBlocks = ve->dirhdr.totalBlocks[1] * 256 + ve->dirhdr.totalBlocks[0];
	volumeBitmapBlock = ve->dirhdr.bitmapPtr[1] * 256 + ve->dirhdr.bitmapPtr[0];
	if (volumeBitmapBlock < 8) {
	  uint8_t *bitmapBlock;
	  bitmapBlock = &trackData[512 * volumeBitmapBlock];
	  calculatedBlocksFree = calculateBlocksFree(bitmapBlock, totalBlocks);
	}
	
	if (0) {
	  printf("Num active entries: %d\n", ve->dirhdr.numEntries[1] * 256 + ve->dirhdr.numEntries[0]);
	  printf("Bitmap pointer block: %d\n", ve->dirhdr.bitmapPtr[1] * 256 + ve->dirhdr.bitmapPtr[0]);
	  printf("Total blocks: %d\n", totalBlocks);
	}
	
	nextIsDirhdr = false;
      } else {
	uint8_t entryType = (ve->file.typelen & 0xF0) >> 4; // deleted/seedling/sapling/tree/directory
	if (entryType) { // if it's not an empty entry...
	  if (ve->file.accessFlags & 0x02) {
	    printf(" ");
	  } else {
	    // File is locked
	    printf("*");
	  }
	  int j;
	  for (j=0; j<(ve->file.typelen & 0x0F); j++) {
	    printf("%c", ve->file.name[j] & 0x7F);
	  }
	  while (j < 17) {
	    printf(" ");
	    j++;
	  };
	  char auxData[16] = "";
	  switch (ve->file.fileType) {
	  case 0:
	    printf("TYP"); // typeless
	    break;
	  case 1:
	    printf("BAD"); // bad block data
	    break;
	  case 4:
	    printf("TXT"); // 7-bit
	    snprintf(auxData, sizeof(auxData), "R=%5d", ve->file.typeData[1] * 256 + ve->file.typeData[0]);
	    break;
	  case 6:
	    printf("BIN"); // 8-bit
	    snprintf(auxData, sizeof(auxData), "A=$%.4X", ve->file.typeData[1] * 256 + ve->file.typeData[0]);
	    break;
	  case 0x0F:
	    printf("DIR");
	    break;
	  case 0x19:
	    printf("ADB"); // appleworks database
	    break;
	  case 0x1A:
	    printf("AWP"); // appleworks word processing
	    break;
	  case 0x1B:
	    printf("ASP"); // appleworks spreadsheet
	    break;
	  case 0xEF:
	    printf("PAS"); // pascal
	    break;
	  case 0xF0:
	    printf("CMD");
	    break;
	  case 0xF1:
	  case 0xF2:
	  case 0xF3:
	  case 0xF4:
	  case 0xF5:
	  case 0xF6:
	  case 0xF7:
	  case 0xF8:
	    printf("US%d", ((ve->file.typelen >> 4) & 0x0F) - 0xF0); // user-defined
	    break;
	  case 0xFC:
	    printf("BAS"); // applesoft basic
	    snprintf(auxData, sizeof(auxData), "[$%.4X]", ve->file.typeData[1] * 256 + ve->file.typeData[0]);
	    break;
	  case 0xFD:
	    printf("VAR"); // saved applesoft variables
	    break;
	  case 0xFE:
	    printf("REL"); // relocatable EDASM object
	    break;
	  case 0xFF:
	    printf("SYS"); // system
	    snprintf(auxData, sizeof(auxData), "[$%.4X]", ve->file.typeData[1] * 256 + ve->file.typeData[0]);
	    break;
	  default:
	    printf("UNK"); // dunno, see appendix E of "Beneath ProDOS"?
	    break;
	  }
	  printf("  %6d  ", ve->file.blocksUsed[1] * 256 + ve->file.blocksUsed[0]);
	  blocksUsed += ve->file.blocksUsed[1] * 256 + ve->file.blocksUsed[0];
	  printDate(ve->file.lastModified);
	  printf("   ");
	  printDate(ve->file.creationDate);
	  printf("  %5d ", (ve->file.eofLength[2] << 16) | (ve->file.eofLength[1] << 8) | ve->file.eofLength[0]);
	  printf("%s", auxData);
	  printf("\n");
	  
	  if (0) {
	    printf("  keyPointer: %.2X %.2X\n", ve->file.keyPointer[0], ve->file.keyPointer[1]);
	    printf("  Creator version: $%.2X\n", ve->file.creatorVersion);
	    printf("  Min required version: $%.2X\n", ve->file.minRequiredVersion);
	    printf("  Flags: $%.2X\n", ve->file.accessFlags);
	    printf("  Type-specific data: $%.2X $%.2X\n", ve->file.typeData[0], ve->file.typeData[1]);
	    printf("  headerPointer: $%.2X $%.2X\n", ve->file.headerPointer[0], ve->file.headerPointer[1]);
	  }
	}
      }
      p += sizeof(struct _vent);
    }
    currentBlock++;
  }
  printf("\nBLOCKS FREE: %4d      BLOCKS USED: %4d     TOTAL BLOCKS: %4d\n",
	 calculatedBlocksFree, totalBlocks - calculatedBlocksFree, totalBlocks);

  printFreeBlocks(&trackData[512 * volumeBitmapBlock]);
}
