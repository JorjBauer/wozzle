#include "dosspector.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "vent.h"

struct _catalogInfo {
  uint8_t unused;
  uint8_t nextCatalogTrack;
  uint8_t nextCatalogSector;
  uint8_t unused2[8];
  struct _dosFdEntry fileEntries[7];
};

// If we want DOS 3.3 sector number 1, we actually want
// physical sector 0x0D. Because that's how DOS works.
const static uint8_t enphys[16] = {
  0x00, 0x0d, 0x0b, 0x09, 0x07, 0x05, 0x03, 0x01,
  0x0e, 0x0c, 0x0a, 0x08, 0x06, 0x04, 0x02, 0x0f };

DosSpector::DosSpector(bool verbose, uint8_t dumpflags) : Wozspector(verbose, dumpflags)
{
}

DosSpector::~DosSpector()
{
}

static void dumpVtoc(struct _vtoc vt)
{
  printf("Catalog starts at %d/%d\n", vt.catalogTrack, vt.catalogSector);
  printf("Catalog made for DOS version %d\n", vt.dosVersion);
  printf("Volume number: %d\n", vt.volumeNumber);
  printf("Max T/S spares: %d\n", vt.maxTSpairs);
  printf("Last Track Alloc: %d\n", vt.lastTrackAllocated);
  printf("Allocation direction: %d\n", vt.allocationDirection);
  printf("Tracks per disk: %d\n", vt.tracksPerDisk);
  printf("Sectors per track: %d\n", vt.sectorsPerTrack);
  printf("Bytes per sector: %.2X%.2X\n", vt.bytesPerSectorHigh, vt.bytesPerSectorLow);
  // Not dumping the track state table
}


Vent *DosSpector::createTree()
{
  if (tree) {
    freeTree(tree);
    tree = NULL;
  }
  printf("Creating tree\n");
  
  uint8_t track[256*16];
  if (!decodeWozTrackToDsk(17, T_DSK, track)) {
    printf("ERROR: Failed to read track 17\n");
    return tree;
  }

  memcpy(&vt, track, sizeof(struct _vtoc));
  // FIXME sanity checking: vt.dosVersion and whatnot?
  // FIXME assert vt.bytesPerSectorHigh/Low == 256
  dumpVtoc(vt);
  
  memset(&trackSectorUsedMap, 0, sizeof(trackSectorUsedMap));

  // FIXME: the trackSectorUsedMap is only set for 35 tracks - what if it's a bigger woz disk?
  // (The rest of the Woz code has other hard-coded '35's so I'm leaving it for now)
  for (int i=0; i<vt.tracksPerDisk; i++) {
    uint16_t state = (vt.trackState[i].sectorsUsed[0] << 8) |
      vt.trackState[i].sectorsUsed[1];
    for (int j=0; j<16; j++) {
      // 1-bits are free; 0-bits are used (Beneath Apple Dos, p. 4-3)
      trackSectorUsedMap[i][j] = (state & (1 << (j))) ? false : true;
    }
  }
  
  // FIXME check vt.catalogTrack == 17? Or pass in the whole disk?
  uint8_t catalogTrack = vt.catalogTrack;
  uint8_t catalogSector = vt.catalogSector;
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

  printf("Returning tree\n");
  return tree;
}

bool DosSpector::flushFreeSectorList()
{
  // The tree has to be loaded before we start, so vt is loaded
  if (!tree)
    createTree();
  
  // update the bitmap
  for (int i=0; i<vt.tracksPerDisk; i++) {
    for (int j=0; j<16; j++) {
      uint8_t bpos = 1<<(j % 8);
      if (trackSectorUsedMap[i][j]) {
        // bit is clear if sector is used
        vt.trackState[i].sectorsUsed[(i >= 8) ? 0 : 1] &= ~bpos;
        if (i==7) {
          printf("S%d used\n", j);
        }
      } else {
        // bit is set if sector is available
        if (i==7)
          printf("S%d free\n", j);
        vt.trackState[i].sectorsUsed[(i >= 8) ? 0 : 1] |= bpos;
      }
    }
  }
  // rewrite the vtoc
  if (!encodeWozTrackSector(17, enphys[0], (uint8_t *)&vt)) {
    printf("Failed to rewrite VTOC with new free sector list\n");
    return false;
  }

  return true;
}

// side effect: marks it as used
bool DosSpector::findFreeSector(int *trackOut, int *sectorOut)
{
  // The tree has to be loaded before we start, so trackSectorUsedMap is correct
  if (!tree)
    createTree();
  
  for (int trk=1; trk<35; trk++) {
    for (int sect=0; sect<16; sect++) {
      if (!trackSectorUsedMap[trk][sect]) {
        trackSectorUsedMap[trk][sect] = true;
        *trackOut = trk;
        *sectorOut = sect;
        printf("Allocating track %d sector %d\n", trk, sect);
        return true;
      }
    }
  }
  return false;
}

bool DosSpector::writeFileToImage(uint8_t *fileContents,
                                  char *fileName,
                                  char fileType,
                                  uint16_t fileStart,
                                  uint16_t fileSize)
{

  if (fileSize > 31232) {
    printf("ERROR: don't know how to handle files > 31232 bytes (b/c we need multiple TSlist blocks)\n");
    return false;
  }

  // The tree has to be loaded before we start
  if (!tree)
    createTree();

  /*
  Vent *fp = findFileByName(fileName);
  if (fp) {
    printf("ERROR: File '%s' already exists\n", fileName);
    return false;
  }
  */

  // We need a TSList
  struct _dosTsList tsList;
  memset(&tsList, 0, sizeof(tsList));
  int tsPairPtr = 0;

  int32_t remainingSize = fileSize;
  while (tsPairPtr < 122 && remainingSize > 0) {
    int t,s;
    if (!findFreeSector(&t, &s)) {
      printf("ERROR: unable to find enough free sectors\n");
      return false;
    }
    printf("Using track %d sector %d for data\n", t, s);
    tsList.tsPair[tsPairPtr].track = t;
    tsList.tsPair[tsPairPtr].sector = s;
    tsPairPtr++;
    remainingSize -= 256;
  }

  // Write the file to those blocks
  remainingSize = fileSize;
  uint16_t dataPtr = 0;
  uint8_t sectorData[256];
  for (int b=0; b<tsPairPtr; b++) {
    int t = tsList.tsPair[b].track;
    int s = tsList.tsPair[b].sector;
    memset(sectorData, 0, sizeof(sectorData));
    memcpy(sectorData, &fileContents[dataPtr],
           remainingSize >= 256 ? 256 : remainingSize);
    printf("Writing data to track %d sector %d\n", t, s);
    if (!encodeWozTrackSector(t, enphys[s], sectorData)) {
      printf("ERROR: failed to encodeWozTrackSector\n");
      return false;
    }
    dataPtr += 256;
  }

  // Find a free block for the TS list itself
  int t,s;
  if (!findFreeSector(&t, &s)) {
    printf("ERROR: can't find a free sector for the TS list\n");
    return false;
  }
  // Write the TS list
  printf("Writing the TS list to track %d sector %d\n", t, s);
  if (!encodeWozTrackSector(t, enphys[s], (uint8_t *)&tsList)) {
    printf("ERROR: failed to encodeWozTrackSector for TSList\n");
    return false;
  }

  // Prep the directory entry
  struct _dosFdEntry newEntry;
  newEntry.firstTrack = t;
  newEntry.firstSector = s;

  switch (fileType) {
  case 'T':
    newEntry.fileTypeAndFlags = 0;
    break;
  case 'I':
    newEntry.fileTypeAndFlags = 1;
    break;
  case 'A':
    newEntry.fileTypeAndFlags = 2;
    break;
  case 'B':
    newEntry.fileTypeAndFlags = 4;
    break;
  case 'S':
    newEntry.fileTypeAndFlags = 8;
    break;
  case 'R':
    newEntry.fileTypeAndFlags = 16;
    break;
  default:
    printf("ERROR: unhandled file type; can't construct directory entry\n");
    return false;
  }

  char buf[31];
  sprintf(buf, "%-30s", fileName);
  printf("new entry name: '%s'\n", buf);
  memcpy(newEntry.fileName, buf, 30); // no terminator copied
  newEntry.fileLength[0] = fileSize & 0xFF;
  newEntry.fileLength[1] = (fileSize >> 8) & 0xFF;

  // Flush the updated free blocks list before we write the directory entry
  if (!flushFreeSectorList()) {
    printf("ERROR: failed to flush free sector list\n");
    return false;
  }

  printf("test 4 - if we return true here then the sector list wasn't updated?\n");
  printf("and if we don't return here, then the catalog is corrupted\n");
  printf("... regardless, the flushFreeSectorList appears to corrupt the catalog\n");
  printf("... and why doesn't the bitmap in vt match the bitmap array? (Or does it and I'm wrong?)\n");

  // DEBUGGING: test that the vtoc is still ok
  createTree();
  if (!tree) {
    printf("No VTOC?[4]\n");
    exit(1);
  }
  // actually add the directory entry
  if (!addDirectoryEntryForFile(&newEntry)) {
    printf("ERROR: failed to add directory entry\n");
    return false;
  }

  // Re-read the VTOC
  printf("Reread VTOC\n");
  createTree();
  if (!tree) {
    printf("No VTOC[5]?\n");
  }

  printf("FIXME: need to write out the disk image now\n");
  /*
  if (!writeWozFile(..., T_DSK)) {
    printf("Failed to write disk image\n");
    return false;
    }*/

  return true;
}


uint32_t DosSpector::getFileContents(Vent *e, char **toWhere)
{
  // The tree has to be loaded before we start
  if (!tree)
    createTree();
  
  // Find the T/S list so we can find the first block of the file, so we can find its length
  uint8_t tsTrack = e->getFirstTrack();
  uint8_t tsSector = e->getFirstSector();
  if (!tsTrack && !tsSector) {
    printf("ERROR: no T/S first entry?\n");
    *toWhere = NULL;
    return 0;
  }
  printf("File T/S list starts at track %d sector %d\n", tsTrack, tsSector);

#if 0
  // unnecesary but checking to see what's wrong with below
  uint8_t dataTrackData[256*16];
  printf("About to decode track %d\n", tsTrack);
  if (!decodeWozTrackToDsk(tsTrack, T_DSK, dataTrackData)) {
    printf("Unable to decode track %d\n", tsTrack);
    *toWhere = NULL;
    return 0;
  }
  printf("Track dump\n");
  for (int i=0; i<16; i++) {
    printf("sector %d:\n", i);
    for (int j=0; j<256; j+=16) {
      printf("%.4X  ", j);
      for (int k=0; k<16; k++) {
        printf("%.2X ", dataTrackData[i*256+j+k]);
      }
      printf("\n");
    }
  }
#endif
  
  struct _dosTsList *tsList = NULL;
  uint8_t sectorData[256];
  if (!decodeWozTrackSector(tsTrack, enphys[tsSector], sectorData)) {
    printf("Unable to decode track %d sector %d\n", tsTrack, tsSector);
    *toWhere = NULL;
    return 0;
  }
  tsList = (struct _dosTsList *)sectorData;
  printf("TSList sector:\n");
  for (int i=0; i<256; i++) {
    printf("%.2X ", sectorData[i]);
  }
  
  // Find the length of the file from its first block
  if (!tsList->tsPair[0].track && !tsList->tsPair[0].sector) {
    printf("ERROR: T/S entry has no data sectors?\n");
    *toWhere = NULL;
    return 0;
  }
  if (tsList->tsPair[0].track > 34 || tsList->tsPair[0].sector > 15) {
    printf("ERROR: invalid T/S entry?\n");
    *toWhere = NULL;
    return 0;
  }
  
  uint8_t dataTrackData[256*16];
  printf("About to decode track %d\n", tsList->tsPair[0].track);
  if (!decodeWozTrackToDsk(tsList->tsPair[0].track, T_DSK, dataTrackData)) {
    printf("Unable to decode track %d\n", tsList->tsPair[0].track);
    *toWhere = NULL;
    return 0;
  }
  printf("dump of track %d\n", tsList->tsPair[0].track);
  for (int i=0; i<16; i++) {
    printf("sector %d\n", i);
    for (int j=0; j<256; j++) {
      printf("%.2X ", dataTrackData[i*256+j]);
    }
    printf("\n");
  }
 
  printf("looking for sector %d\n", tsList->tsPair[0].sector);
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

  // '122' is the number of T/S pairs in the structure
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

bool DosSpector::addDirectoryEntryForFile(struct _dosFdEntry *e)
{
  printf("adding dirent\n");
  
  // The tree has to be loaded before we start
  if (!tree)
    createTree();
  
  // Find the first directory entry that's empty

  uint8_t sectorData[256];

  uint8_t catalogTrack = vt.catalogTrack;
  uint8_t catalogSector = vt.catalogSector;
  printf("Starting catalog at %d/%d\n", catalogTrack, catalogSector);
  
  // Spin through all the catalog sectors to find an empty entry
  while (catalogTrack) {
    if (!decodeWozTrackSector(catalogTrack, enphys[catalogSector], sectorData)) {
      printf("Failed to read catalog t %d/s %d\n", catalogTrack, catalogSector);
      return false;
    }
    struct _catalogInfo *ci = (struct _catalogInfo *)sectorData;
    for (int i=0; i<7; i++) {
      if (!ci->fileEntries[i].fileName[0] || (uint8_t)(ci->fileEntries[i].fileName[0]) == 0xFF) { // 0xFF is what happens to deleted files

        // write 'e' as a blob of data over the current entry, then write
        // out the sector to disk again
        memcpy(&ci->fileEntries[i], e, sizeof(struct _dosFdEntry));
        // (... we have to go set the high bit on the file name too)
        for (int j=0; j<30; j++) {
          ci->fileEntries[i].fileName[j] |= 0x80;
        }

        // write it
        if (!encodeWozTrackSector(catalogTrack, enphys[catalogSector], sectorData)) {
          printf("Failed to write updated catalog sector\n");
          return false;
        }

        // succeeded!
        return true;
      }
    }
    catalogTrack = ci->nextCatalogTrack;
    catalogSector = ci->nextCatalogSector;
  }

  printf("addDirectoryEntryForFile failed to find an empty entry slot\n");
  return false;
}

void DosSpector::displayInfo()
{
  if (!tree)
    createTree();

  // DEBUGGING: reload tsusedmap from the vt catalog
  memset(&trackSectorUsedMap, 0, sizeof(trackSectorUsedMap));
  for (int i=0; i<vt.tracksPerDisk; i++) {
    uint16_t state = (vt.trackState[i].sectorsUsed[0] << 8) |
      vt.trackState[i].sectorsUsed[1];
    for (int j=0; j<16; j++) {
      // 1-bits are free; 0-bits are used (Beneath Apple Dos, p. 4-3)
      trackSectorUsedMap[i][j] = (state & (1 << (j))) ? false : true;
    }
  }

  printf("Free sector map:\n");
  for (int i=0; i<35; i++) {
    printf("Track %.2d  ", i);
    for (int j=0; j<16; j++) {
      printf("%c", trackSectorUsedMap[i][j] ? 'U' : 'f');
    }
    printf("\n");
  }
}

