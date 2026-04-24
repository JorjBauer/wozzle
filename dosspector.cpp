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
  if (verbose) printf("Creating tree\n");

  uint8_t track[256*16];
  if (!decodeWozTrackToDsk(17, T_DSK, track)) {
    printf("ERROR: Failed to read track 17\n");
    return tree;
  }

  memcpy(&vt, track, sizeof(struct _vtoc));
  // FIXME sanity checking: vt.dosVersion and whatnot?
  // FIXME assert vt.bytesPerSectorHigh/Low == 256
  if (verbose) dumpVtoc(vt);
  
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

  if (verbose) printf("Returning tree\n");
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
        if (verbose && i==7) {
          printf("S%d used\n", j);
        }
      } else {
        // bit is set if sector is available
        if (verbose && i==7)
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
        if (verbose) printf("Allocating track %d sector %d\n", trk, sect);
        return true;
      }
    }
  }
  return false;
}

bool DosSpector::writeFileToImage(uint8_t *fileContents,
                                  char *fileName,
                                  uint8_t fileType,
                                  uint16_t fileStart,
                                  uint32_t fileSize)
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
    if (verbose) printf("Using track %d sector %d for data\n", t, s);
    tsList.tsPair[tsPairPtr].track = t;
    tsList.tsPair[tsPairPtr].sector = s;
    tsPairPtr++;
    remainingSize -= 256;
  }

  // Write the file to those blocks. DOS 3.3 inserts a short header before
  // most file types, so we need to take the file type in to account when
  // figuring out the data & size.
  uint8_t *adulteratedData = NULL;
  uint16_t writeSize = 0;
  switch (fileType) {
  case 0x06:
    writeSize = fileSize + 4;
    adulteratedData = (uint8_t *)malloc(writeSize);
    memcpy(&adulteratedData[4], fileContents, fileSize);
    // The first 4 bytes are L/H address and L/H length.
    adulteratedData[0] = fileStart & 0xFF;
    adulteratedData[1] = (fileStart >> 8) & 0xFF;
    adulteratedData[2] = fileSize & 0xFF;
    adulteratedData[3] = (fileSize >> 8) & 0xFF;
    break;
  }
  if (!adulteratedData) {
    printf("ERROR: don't know how to set up the file header for this file type\n");
    return false;
  }

  remainingSize = writeSize;
  uint16_t dataPtr = 0;
  uint8_t sectorData[256];
  for (int b=0; b<tsPairPtr; b++) {
    int t = tsList.tsPair[b].track;
    int s = tsList.tsPair[b].sector;
    memset(sectorData, 0, sizeof(sectorData));
    memcpy(sectorData, &adulteratedData[dataPtr],
           remainingSize >= 256 ? 256 : remainingSize);
    if (verbose) printf("Writing data to track %d sector %d\n", t, s);
    if (!encodeWozTrackSector(t, enphys[s], sectorData)) {
      printf("ERROR: failed to encodeWozTrackSector\n");
      free(adulteratedData);
      return false;
    }
    dataPtr += 256;
  }
  free(adulteratedData);
  adulteratedData = NULL;

  // Find a free block for the TS list itself
  int t,s;
  if (!findFreeSector(&t, &s)) {
    printf("ERROR: can't find a free sector for the TS list\n");
    return false;
  }
  // Write the TS list
  if (verbose) printf("Writing the TS list to track %d sector %d\n", t, s);
  if (!encodeWozTrackSector(t, enphys[s], (uint8_t *)&tsList)) {
    printf("ERROR: failed to encodeWozTrackSector for TSList\n");
    return false;
  }

  // Prep the directory entry
  struct _dosFdEntry newEntry;
  newEntry.firstTrack = t;
  newEntry.firstSector = s;

  // The fileType argument passed in to us was a ProDOS file type, because
  // they're more expressive than the DOS file types, so we can use just one
  // and interpret them here.
  switch (fileType) {
  case FT_TXT: // 'T' Text
    newEntry.fileTypeAndFlags = 0;
    break;
  case FT_INT: // 'I' Integer basic
    newEntry.fileTypeAndFlags = 1;
    break;
  case FT_BAS: // 'A' Applesoft Basic
    newEntry.fileTypeAndFlags = 2;
    break;
  case FT_BIN: // 'B' Binary file
    newEntry.fileTypeAndFlags = 4;
    break;
  case FT_SYS: // 'S' sys
    newEntry.fileTypeAndFlags = 8;
    break;
  case FT_REL: // 'R' rel
    newEntry.fileTypeAndFlags = 16;
    break;
  default:
    printf("ERROR: unhandled file type; can't construct directory entry\n");
    return false;
  }

  char buf[31];
  snprintf(buf, sizeof(buf), "%-30s", fileName);
  if (verbose) printf("new entry name: '%s'\n", buf);
  memcpy(newEntry.fileName, buf, 30); // no terminator copied
  newEntry.fileLength[0] = fileSize & 0xFF;
  newEntry.fileLength[1] = (fileSize >> 8) & 0xFF;

  // Flush the updated free blocks list before we write the directory entry
  if (!flushFreeSectorList()) {
    printf("ERROR: failed to flush free sector list\n");
    return false;
  }

  // actually add the directory entry
  if (!addDirectoryEntryForFile(&newEntry)) {
    printf("ERROR: failed to add directory entry\n");
    return false;
  }

  // Re-read the VTOC
  createTree();

  // Caller is responsible for updating the on-disk image
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
  if (verbose) printf("File T/S list starts at track %d sector %d\n", tsTrack, tsSector);

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
  if (verbose) {
    printf("TSList sector:\n");
    for (int i=0; i<256; i++) {
      printf("%.2X ", sectorData[i]);
    }
    printf("\n");
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
  if (verbose) printf("About to decode track %d\n", tsList->tsPair[0].track);
  if (!decodeWozTrackToDsk(tsList->tsPair[0].track, T_DSK, dataTrackData)) {
    printf("Unable to decode track %d\n", tsList->tsPair[0].track);
    *toWhere = NULL;
    return 0;
  }
  if (verbose) {
    printf("dump of track %d\n", tsList->tsPair[0].track);
    for (int i=0; i<16; i++) {
      printf("sector %d\n", i);
      for (int j=0; j<256; j++) {
        printf("%.2X ", dataTrackData[i*256+j]);
      }
      printf("\n");
    }
    printf("looking for sector %d\n", tsList->tsPair[0].sector);
  }
  uint8_t *dataPtr = &dataTrackData[256*tsList->tsPair[0].sector];
  uint32_t fileLength = 0;
  switch (e->getFileType()) {
    case FT_BIN:
      fileLength = dataPtr[2] + 256*dataPtr[3];
      break;
    case FT_BAS:
    case FT_INT:
      // The on-disk DOS 3.3 file is a 2-byte length header followed by
      // exactly that many bytes of tokenized program. Return both so the
      // caller gets the full file (listers skip the header via
      // applesoftHeaderBytes()).
      fileLength = dataPtr[0] + 256*dataPtr[1] + 2;
      if (verbose) printf("BAS/INT file length %d\n", fileLength);
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
  // The tree has to be loaded before we start
  if (!tree)
    createTree();
  
  // Find the first directory entry that's empty

  uint8_t sectorData[256];

  uint8_t catalogTrack = vt.catalogTrack;
  uint8_t catalogSector = vt.catalogSector;
  
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

bool DosSpector::probe()
{
  // DOS 3.3 puts the VTOC at track 17 / sector 0 in DOS ordering. A
  // plausible VTOC has dosVersion == 3, catalogTrack == 17, and an
  // expected tracks/sectors geometry. If any of those are off, the
  // image is very unlikely to be DOS 3.3.
  uint8_t track[256*16];
  if (!decodeWozTrackToDsk(17, T_DSK, track)) return false;
  struct _vtoc *v = (struct _vtoc *)track;
  if (v->dosVersion != 3) return false;
  if (v->catalogTrack != 17) return false;
  if (v->tracksPerDisk != 35 && v->tracksPerDisk != 40) return false;
  if (v->sectorsPerTrack != 16 && v->sectorsPerTrack != 13) return false;
  return true;
}

uint32_t DosSpector::getAllocatedByteCount(Vent *e)
{
  // Walk the T/S list chain and count the data-sector entries. Multiplied
  // by 256 this equals what getFileAllocation would produce, so the cpout
  // warning and the -A recovery stay in sync. The catalog's raw sector
  // count (blocksUsed) over-counts by one per T/S list sector and isn't
  // used here for that reason.

  if (!tree) createTree();

  uint8_t curT = e->getFirstTrack();
  uint8_t curS = e->getFirstSector();
  if (!curT && !curS) return 0;

  uint32_t dataSectors = 0;
  int safety = 256;
  while ((curT || curS) && safety-- > 0) {
    uint8_t sectorData[256];
    if (!decodeWozTrackSector(curT, enphys[curS], sectorData)) {
      return 0;
    }
    struct _dosTsList *tsList = (struct _dosTsList *)sectorData;
    for (int i = 0; i < 122; i++) {
      if (tsList->tsPair[i].track != 0 || tsList->tsPair[i].sector != 0) {
        dataSectors++;
      }
    }
    curT = tsList->nextTrack;
    curS = tsList->nextSector;
  }
  return dataSectors * 256;
}

uint32_t DosSpector::getFileAllocation(Vent *e, char **toWhere)
{
  // Walks the full T/S list chain (following nextTrack/nextSector across
  // multiple T/S list sectors) and concatenates every data sector it
  // references. 0/0 pairs inside the list are skipped — treated as
  // unused slots rather than sparse-file holes. Unlike getFileContents,
  // the in-file length header is ignored, so this is the right way to
  // recover files whose catalog "length" is a stub hiding a larger payload.

  if (!tree) createTree();

  *toWhere = NULL;

  uint8_t tsTrack = e->getFirstTrack();
  uint8_t tsSector = e->getFirstSector();
  if (!tsTrack && !tsSector) {
    return 0;
  }

  // First pass: count data sectors so we can allocate the output buffer.
  uint32_t dataSectorCount = 0;
  {
    uint8_t curT = tsTrack, curS = tsSector;
    int safety = 256; // guard against a circular T/S list chain
    while ((curT || curS) && safety-- > 0) {
      uint8_t sectorData[256];
      if (!decodeWozTrackSector(curT, enphys[curS], sectorData)) {
        printf("ERROR: failed to read T/S list at %d/%d\n", curT, curS);
        return 0;
      }
      struct _dosTsList *tsList = (struct _dosTsList *)sectorData;
      for (int i = 0; i < 122; i++) {
        if (tsList->tsPair[i].track != 0 || tsList->tsPair[i].sector != 0) {
          dataSectorCount++;
        }
      }
      curT = tsList->nextTrack;
      curS = tsList->nextSector;
    }
  }

  uint32_t outSize = dataSectorCount * 256;
  if (outSize == 0) {
    return 0;
  }
  *toWhere = (char *)malloc(outSize);
  if (!*toWhere) {
    return 0;
  }

  // Second pass: copy each referenced data sector into the buffer.
  uint32_t writePos = 0;
  {
    uint8_t curT = tsTrack, curS = tsSector;
    int safety = 256;
    while ((curT || curS) && safety-- > 0) {
      uint8_t sectorData[256];
      if (!decodeWozTrackSector(curT, enphys[curS], sectorData)) {
        free(*toWhere);
        *toWhere = NULL;
        return 0;
      }
      struct _dosTsList *tsList = (struct _dosTsList *)sectorData;
      for (int i = 0; i < 122; i++) {
        uint8_t t = tsList->tsPair[i].track;
        uint8_t s = tsList->tsPair[i].sector;
        if (t == 0 && s == 0) continue;
        uint8_t dataSector[256];
        if (!decodeWozTrackSector(t, enphys[s], dataSector)) {
          // Couldn't read; leave a zero-filled hole rather than abort so
          // the rest of the file can still be recovered.
          memset((*toWhere) + writePos, 0, 256);
        } else {
          memcpy((*toWhere) + writePos, dataSector, 256);
        }
        writePos += 256;
      }
      curT = tsList->nextTrack;
      curS = tsList->nextSector;
    }
  }

  return outSize;
}

void DosSpector::inspectFile(const char *fileName, Vent *fp)
{
  // Three independent size signals, cross-checked:
  //   1. catalog sector count (from the directory entry)
  //   2. T/S walk (chains through nextTrack/nextSector)
  //   3. in-file length header (first data sector, type-dependent)

  uint16_t catalogSectors = fp->getBlocksUsed();
  uint32_t catalogBytes = (uint32_t)catalogSectors * 256;
  printf("  Catalog sector count: %u (= %u bytes allocated)\n",
         catalogSectors, catalogBytes);

  uint8_t tsTrack = fp->getFirstTrack();
  uint8_t tsSector = fp->getFirstSector();
  if (!tsTrack && !tsSector) {
    printf("  No T/S list pointer — cannot walk\n");
    return;
  }

  uint32_t tsListSectors = 0;
  uint32_t dataSectors = 0;
  uint32_t sparseGaps = 0;
  uint8_t firstDataSector[256];
  bool haveFirstData = false;
  uint8_t firstDataT = 0, firstDataS = 0;

  uint8_t curT = tsTrack, curS = tsSector;
  int safetyBound = 256; // guard against circular T/S list chains
  while ((curT || curS) && safetyBound-- > 0) {
    uint8_t sectorData[256];
    if (!decodeWozTrackSector(curT, enphys[curS], sectorData)) {
      printf("  ERROR: failed to read T/S list at %d/%d\n", curT, curS);
      break;
    }
    tsListSectors++;
    struct _dosTsList *tsList = (struct _dosTsList *)sectorData;

    for (int i = 0; i < 122; i++) {
      uint8_t t = tsList->tsPair[i].track;
      uint8_t s = tsList->tsPair[i].sector;
      if (t == 0 && s == 0) {
        // In a T/S list, 0/0 is either end-of-list or a sparse hole.
        // We count them as sparse and keep scanning the rest of the list;
        // trailing holes just look like extra sparse entries.
        sparseGaps++;
        continue;
      }
      if (t > 34 || s > 15) {
        printf("  WARNING: invalid T/S pair %d/%d in list at %d/%d\n",
               t, s, curT, curS);
        continue;
      }
      dataSectors++;
      if (!haveFirstData) {
        if (decodeWozTrackSector(t, enphys[s], firstDataSector)) {
          haveFirstData = true;
          firstDataT = t;
          firstDataS = s;
        }
      }
    }

    curT = tsList->nextTrack;
    curS = tsList->nextSector;
  }

  uint32_t walkTotal = tsListSectors + dataSectors;
  printf("  T/S walk: %u T/S list sector(s), %u data sector(s)",
         tsListSectors, dataSectors);
  if (sparseGaps) {
    printf(", %u sparse/trailing 0/0 entr%s",
           sparseGaps, sparseGaps == 1 ? "y" : "ies");
  }
  printf("\n");
  printf("  T/S walk total: %u sectors (= %u bytes)\n",
         walkTotal, walkTotal * 256);

  if (catalogSectors != walkTotal) {
    printf("  ** MISMATCH: catalog says %u sectors, walk found %u\n",
           catalogSectors, walkTotal);
  }

  if (!haveFirstData) {
    printf("  (no data sectors to read in-file header from)\n");
    return;
  }

  printf("  First data sector: %d/%d\n", firstDataT, firstDataS);
  uint32_t logicalLen = 0;
  bool haveLogical = false;
  switch (fp->getFileType()) {
  case FT_BAS:
    logicalLen = firstDataSector[0] + 256 * firstDataSector[1];
    haveLogical = true;
    printf("  Applesoft length header: %u bytes\n", logicalLen);
    break;
  case FT_INT:
    logicalLen = firstDataSector[0] + 256 * firstDataSector[1];
    haveLogical = true;
    printf("  Integer BASIC length header: %u bytes\n", logicalLen);
    break;
  case FT_BIN:
    {
      uint16_t loadAddr = firstDataSector[0] + 256 * firstDataSector[1];
      logicalLen = firstDataSector[2] + 256 * firstDataSector[3];
      haveLogical = true;
      printf("  Binary header: load=$%.4X, length=%u bytes\n",
             loadAddr, logicalLen);
    }
    break;
  case FT_TXT:
    printf("  Text file: no length header (0x00-terminated, possibly sparse)\n");
    break;
  default:
    printf("  File type 0x%.2X: no standard length header known\n",
           fp->getFileType());
    break;
  }

  if (haveLogical) {
    // Compare logical length against the walked data allocation, minus
    // the length-header bytes themselves (2 for BAS/INT, 4 for BIN).
    uint32_t headerBytes = (fp->getFileType() == FT_BIN) ? 4 : 2;
    uint32_t dataBytes = dataSectors * 256;
    if (dataBytes > headerBytes) dataBytes -= headerBytes;
    if (logicalLen + 512 < dataBytes) {
      printf("  ** Logical length (%u) is far smaller than data allocation (%u)\n",
             logicalLen, dataBytes);
      printf("     — possible stub loader with hidden payload, or deleted-tail padding\n");
    }
  }
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

