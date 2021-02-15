#include "vtoc.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "vent.h"

struct _bmTrack {
  uint8_t sectorsUsed[2];
  uint8_t unused[2];
};

struct _vtoc {
  uint8_t unused;
  uint8_t catalogTrack;
  uint8_t catalogSector;
  uint8_t dosVersion;
  uint8_t unused2[2];
  uint8_t volumeNumber;
  uint8_t unused3[32];
  uint8_t maxTSpairs;
  uint8_t unused4[8];
  uint8_t lastTrackAllocated;
  uint8_t allocationDirection;
  uint8_t unused5[2];
  uint8_t tracksPerDisk;
  uint8_t sectorsPerTrack;
  uint8_t bytesPerSectorLow;
  uint8_t bytesPerSectorHigh;
  struct _bmTrack trackState[50];
};

struct _catalogInfo {
  uint8_t unused;
  uint8_t nextCatalogTrack;
  uint8_t nextCatalogSector;
  uint8_t unused2[8];
  struct _dosFdEntry fileEntries[7];
};

VToC::VToC()
{
}

VToC::~VToC()
{
}

/*
void VToC::DecodeVToC(unsigned char track[256*16])
{
  struct _vtoc *vt = (struct _vtoc *)track;

  printf("catalog track: %d\n", vt->catalogTrack);
  printf("catalog sector: %d\n", vt->catalogSector);
  printf("DOS version: %d\n", vt->dosVersion);
  printf("Volume number: %d\n", vt->volumeNumber);
  printf("Maximum track-sector pairs: %d\n", vt->maxTSpairs);
  printf("Last track allocated: %d\n", vt->lastTrackAllocated);
  printf("Allocation direction: %d\n", vt->allocationDirection);
  
  printf("Tracks per disk: %d\n", vt->tracksPerDisk);
  printf("Sectors per track: %d\n", vt->sectorsPerTrack);
  printf("bytes per sector: 0x%X 0x%X\n", vt->bytesPerSectorHigh, vt->bytesPerSectorLow);

  bool trackSectorUsedMap[35][16];
  memset(&trackSectorUsedMap, 0, sizeof(trackSectorUsedMap));
  
  for (int i=0; i<vt->tracksPerDisk; i++) {
    printf("Track %.2d: ", i);
    uint16_t state = (vt->trackState[i].sectorsUsed[0] << 8) |
      vt->trackState[i].sectorsUsed[1];
    for (int j=0; j<16; j++) {
      // *** I think this may be backwards?
      printf("%c ", state & (1 << (16-j)) ? 'f' : 'U');
    }
    printf("\n");
  }
  printf("[Legend: 'U' is used; 'f' is free]\n");

  printf("\n\nCatalog info:\n");

  uint8_t catalogTrack = vt->catalogTrack;
  uint8_t catalogSector = vt->catalogSector;

  while (catalogTrack==17 && catalogSector<16) {
    printf("Track %d sector %d:\n", catalogTrack, catalogSector);
    struct _catalogInfo *ci = (struct _catalogInfo *)&track[256*catalogSector];
    for (int i=0; i<7; i++) {
      if (ci->fileEntries[i].fileName[0]) {
	printf("  File entry %d:\n    Name: ", i);
	for (int j=0; j<30; j++) {
	  printf("%c", ci->fileEntries[i].fileName[j] ^ 0x80);
	}
	if (ci->fileEntries[i].firstTrack == 255) {
	  printf("\n    File is DELETED\n");
	  printf("    Old file track, sector: %d, %d\n",
		 ci->fileEntries[i].fileName[29],
		 ci->fileEntries[i].firstSector);
	} else {
	  printf("\n    first track, sector: %d, %d\n",
		 ci->fileEntries[i].firstTrack,
		 ci->fileEntries[i].firstSector);
	}
	if (ci->fileEntries[i].fileTypeAndFlags & 0x80) {
	  printf("    File is Locked\n");
	}
	switch (ci->fileEntries[i].fileTypeAndFlags & 0x7F) {
	case 0:
	  printf("    Text file\n");
	  break;
	case 1:
	  printf("    Integer basic file\n");
	  break;
	case 2:
	  printf("    Applesoft basic file\n");
	  break;
	case 4:
	  printf("    Binary file\n");
	  break;
	case 8:
	  printf("    'S' type file\n");
	  break;
	case 16:
	  printf("    Relocatable object module file\n");
	  break;
	case 32:
	  printf("    'A' type file\n");
	  break;
	case 64:
	  printf("    'B' type file\n");
	  break;
	default:
	  printf("    Unknown file type 0x%.2X\n", ci->fileEntries[i].fileTypeAndFlags & 0x7F);
	}
	printf("    File length: 0x%.2X%.2X\n",
	       ci->fileEntries[i].fileLength[1],
	       ci->fileEntries[i].fileLength[0]);
      }
    }
    catalogTrack = ci->nextCatalogTrack;
    catalogSector = ci->nextCatalogSector;
  }

}
*/

Vent *VToC::createTree(uint8_t *track)
{
  Vent *ret = NULL;
  
  struct _vtoc *vt = (struct _vtoc *)track;
  // FIXME sanity checking: vt->dosVersion and whatnot?
  // FIXME assert vt->bytesPerSectorHigh/Low == 256

  // FIXME this needs to be stored somewhere
  bool trackSectorUsedMap[35][16];
  memset(&trackSectorUsedMap, 0, sizeof(trackSectorUsedMap));

  for (int i=0; i<vt->tracksPerDisk; i++) {
    printf("Track %.2d: ", i);
    uint16_t state = (vt->trackState[i].sectorsUsed[0] << 8) |
      vt->trackState[i].sectorsUsed[1];
    for (int j=0; j<16; j++) {
      // *** I think this may be backwards?                                     
      printf("%c ", state & (1 << (16-j)) ? 'f' : 'U');
    }
    printf("\n");
  }
  printf("[Legend: 'U' is used; 'f' is free]\n");

  // FIXME check vt->catalogTrack == 17? Or pass in the whole disk?
  uint8_t catalogTrack = vt->catalogTrack;
  uint8_t catalogSector = vt->catalogSector;
  while (catalogTrack == 17 && catalogSector < 16) {
    struct _catalogInfo *ci = (struct _catalogInfo *)&track[256*catalogSector];
    for (int i=0; i<7; i++) {
      if (ci->fileEntries[i].fileName[0]) {
	Vent *newEnt = new Vent(&ci->fileEntries[i]);
	if (!ret) {
	  ret = newEnt;
	} else {
	  Vent *p3 = ret;
	  while (p3->nextEnt()) {
	    p3 = p3->nextEnt();
	  }
	  p3->nextEnt(newEnt);
	}
      }
    }
    catalogTrack = ci->nextCatalogTrack;
    catalogSector = ci->nextCatalogSector;
  }

  return ret;
}


void VToC::freeTree(Vent *tree)
{
  if (!tree) return;
  do {
    Vent *t = tree;
    tree = tree->nextEnt();
    delete t;
  } while (tree);
}

void VToC::displayTree(Vent *tree)
{
  while (tree) {
    tree->Dump();
    tree = tree->nextEnt();
  }
}
