#include "dosspector.h"
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

DosSpector::DosSpector(bool verbose, uint8_t dumpflags) : Wozspector(verbose, dumpflags)
{
}

DosSpector::~DosSpector()
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

Vent *DosSpector::createTree()
{
  if (tree) {
    freeTree(tree);
    tree = NULL;
  }

  uint8_t track[256*16];
  decodeWozTrackToDsk(17, T_DSK, track);
  
  struct _vtoc *vt = (struct _vtoc *)track;
  // FIXME sanity checking: vt->dosVersion and whatnot?
  // FIXME assert vt->bytesPerSectorHigh/Low == 256

  memset(&trackSectorUsedMap, 0, sizeof(trackSectorUsedMap));

  // FIXME: the trackSectorUsedMap is only set for 35 tracks - what if it's a bigger woz disk?
  // (The rest of the Woz code has other hard-coded '35's so I'm leaving it for now)
  for (int i=0; i<vt->tracksPerDisk; i++) {
    uint16_t state = (vt->trackState[i].sectorsUsed[0] << 8) |
      vt->trackState[i].sectorsUsed[1];
    for (int j=0; j<16; j++) {
      trackSectorUsedMap[i][j] = state & (1 << (j)) ? false : true;
    }
  }
  
  // FIXME check vt->catalogTrack == 17? Or pass in the whole disk?
  uint8_t catalogTrack = vt->catalogTrack;
  uint8_t catalogSector = vt->catalogSector;
  while (catalogTrack == 17 && catalogSector < 16) {
    struct _catalogInfo *ci = (struct _catalogInfo *)&track[256*catalogSector];
    for (int i=0; i<7; i++) {
      if (ci->fileEntries[i].fileName[0]) {
	Vent *newEnt = new Vent(&ci->fileEntries[i]);
	if (!tree) {
	  tree = newEnt;
	} else {
	  Vent *p3 = tree;
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

  return tree;
}

uint32_t DosSpector::getFileContents(Vent *e, char **toWhere)
{
  // Find the T/S list so we can find the first block of the file, so we can find its length
  uint8_t tsTrack = e->getFirstTrack();
  uint8_t tsSector = e->getFirstSector();
  
  if (!tsTrack && !tsSector) {
    printf("ERROR: no T/S first entry?\n");
    *toWhere = NULL;
    return 0;
  }
  
  struct _dosTsList *tsList = NULL;
  uint8_t sectorData[256];
  if (!decodeWozTrackSector(tsTrack, tsSector, sectorData)) {
    printf("Unable to decode track %d sector %d\n", tsTrack, tsSector);
    *toWhere = NULL;
    return 0;
  }
  tsList = (struct _dosTsList *)sectorData;
  
#if 0
  printf("first track: %d; first sector: %d\n", tsTrack, tsSector);
  for (int i=0; i<256; i++) {
    printf("%.2X ", trackData[16*tsSector + i]);
  }
  printf("\n");
  
  printf("T/S list dump\n");
  printf("next Track: %d\n", tsList->nextTrack);
  printf("next Sector: %d\n", tsList->nextSector);
  printf("sector offset: %d\n", tsList->sectorOffset[0] + tsList->sectorOffset[1]*256);
  printf("t/s pair dump:\n");
  for (int i=0; i<122; i++) {
    printf(" %d: track %d sector %d\n", i, tsList->tsPair[i].track, tsList->tsPair[i].sector);
  }
#endif
  
  // Find the length of the file from its first block
  if (!tsList->tsPair[0].track && !tsList->tsPair[0].sector) {
    printf("ERROR: T/S entry has no data sectors?\n");
    *toWhere = NULL;
    return 0;
  }
  uint8_t dataTrackData[256*16];
  if (!decodeWozTrackToDsk(tsList->tsPair[0].track, T_DSK, dataTrackData)) {
    printf("Unable to decode track %d\n", tsList->tsPair[0].track);
    *toWhere = NULL;
    return 0;
  }
  
  uint8_t *dataPtr = &dataTrackData[256*tsList->tsPair[0].sector];
  uint32_t fileLength = 0;
  switch (e->getFileType()) {
    case FT_BIN:
      fileLength = dataPtr[2] + 256*dataPtr[3];
      break;
    case FT_BAS:
    case FT_INT:
      fileLength = dataPtr[0] + 256*dataPtr[1];
      printf("BAS/INT file length %d\n", fileLength);
      break;
    case FT_SYS: // 'S' type
    case FT_REL: // 'R' type
      // Don't know what SYS/REL do for length; skip for now
    case FT_TXT:
      // There's no way to tell the length of this beforehand - we have to
      // go through the full T/S list. It may be sparse (have 00/00 entries
      // in the T/S list), so it really is a full scan...
    default:
      printf("Unimplemented: don't know how to tell file length of this file type\n");
      *toWhere = NULL;
      return 0;
      break;
  }
  
  *toWhere = (char *)malloc(fileLength); // FIXME: error checking
  
  // FIXME: this method won't work for sparse TXT Files
  uint8_t ptr = 0; // pointer in to the tsList->tsPair: which pair are we looking at?
  
  uint32_t dptr = 0; // pointer in to the return data
  uint32_t dataLeft = fileLength;
  
  while (ptr < 122 && (tsList->tsPair[ptr].track != 0 || tsList->tsPair[ptr].sector != 0)) {
    // FIXME this could decode a single sector, instead of a track that we repeat
    if (!decodeWozTrackToDsk(tsList->tsPair[ptr].track, T_DSK, dataTrackData)) {
      printf("Unable to decode track %d\n", tsList->tsPair[ptr].track);
      free(*toWhere);
      *toWhere = NULL;
      return 0;
    }
    dataPtr = &dataTrackData[256*tsList->tsPair[ptr].sector];
    
    if (dataLeft >= 256) {
      memcpy((*toWhere) + dptr, dataPtr, 256);
      dataLeft -= 256;
      dptr += 256;
    } else {
      memcpy((*toWhere) + dptr, dataPtr, dataLeft);
      
      dataLeft = 0;
      // We've got all the data
      return fileLength;
    }
    ptr++;
  }
  
  // If we reach here, then we need more data than was in the first T/S list.
  // FIXME: implement
  printf("Unimplemented: don't know how to follow second track/sector list\n");
  free (*toWhere);
  *toWhere = NULL;
  return 0;
}
