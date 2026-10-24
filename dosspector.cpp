#include "dosspector.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "vent.h"
#include "wozmsg.h"

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

DosSpector::DosSpector(bool verbose) : Wozspector(verbose)
{
  vtLoaded = false;
}

DosSpector::~DosSpector()
{
}

static void dumpVtoc(struct _vtoc vt)
{
  wozMsg(WOZ_MSG_DEBUG, "Catalog starts at %d/%d", vt.catalogTrack, vt.catalogSector);
  wozMsg(WOZ_MSG_DEBUG, "Catalog made for DOS version %d", vt.dosVersion);
  wozMsg(WOZ_MSG_DEBUG, "Volume number: %d", vt.volumeNumber);
  wozMsg(WOZ_MSG_DEBUG, "Max T/S spares: %d", vt.maxTSpairs);
  wozMsg(WOZ_MSG_DEBUG, "Last Track Alloc: %d", vt.lastTrackAllocated);
  wozMsg(WOZ_MSG_DEBUG, "Allocation direction: %d", vt.allocationDirection);
  wozMsg(WOZ_MSG_DEBUG, "Tracks per disk: %d", vt.tracksPerDisk);
  wozMsg(WOZ_MSG_DEBUG, "Sectors per track: %d", vt.sectorsPerTrack);
  wozMsg(WOZ_MSG_DEBUG, "Bytes per sector: %.2X%.2X", vt.bytesPerSectorHigh, vt.bytesPerSectorLow);
  // Not dumping the track state table
}


// Read one logical track, dispatching on the disk's sector format. The
// caller-supplied buffer is sized for 16 sectors (4096 B); for a 13-
// sector track we read into a 13-sector buffer and copy only the
// populated portion, which is all the callers touch.
bool DosSpector::readLogicalTrack(uint8_t phystrack, uint8_t out[256*16])
{
  if (isThirteenSectorDisk()) {
    uint8_t smallBuf[256*13];
    if (!decodeWozTrack13ToDsk(phystrack, smallBuf)) return false;
    memcpy(out, smallBuf, 256*13);
    return true;
  }
  return decodeWozTrackToDsk(phystrack, T_DSK, out);
}

bool DosSpector::readLogicalSector(uint8_t phystrack, uint8_t logsect, uint8_t out[256])
{
  // Damaged catalogs and T/S lists can point anywhere; don't index past
  // the track buffer (or the 40-track quarter-track map) on their say-so
  if (phystrack >= 40 || logsect >= 16) return false;

  uint8_t track[256*16];
  if (!readLogicalTrack(phystrack, track)) return false;
  memcpy(out, &track[logsect * 256], 256);
  return true;
}

Vent *DosSpector::createTree()
{
  if (tree) {
    freeTree(tree);
    tree = NULL;
  }
  if (verbose) wozMsg(WOZ_MSG_DEBUG, "Creating tree");

  uint8_t track[256*16];
  if (!readLogicalTrack(17, track)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: Failed to read track 17");
    return tree;
  }

  memcpy(&vt, track, sizeof(struct _vtoc));
  // FIXME sanity checking: vt.dosVersion and whatnot?
  // FIXME assert vt.bytesPerSectorHigh/Low == 256
  if (verbose) dumpVtoc(vt);

  // A sensible bound for the in-memory sector bitmap. Honors DOS 3.2's
  // 13 or DOS 3.3's 16; if the VTOC is garbage, fall back to 16.
  uint8_t spt = vt.sectorsPerTrack;
  if (spt != 13 && spt != 16) spt = 16;

  memset(&trackSectorUsedMap, 0, sizeof(trackSectorUsedMap));

  // FIXME: the trackSectorUsedMap is only set for 35 tracks - what if it's a bigger woz disk?
  // (The rest of the Woz code has other hard-coded '35's so I'm leaving it for now)
  for (int i=0; i<vt.tracksPerDisk; i++) {
    uint16_t state = (vt.trackState[i].sectorsUsed[0] << 8) |
      vt.trackState[i].sectorsUsed[1];
    for (int j=0; j<spt; j++) {
      // 1-bits are free; 0-bits are used (Beneath Apple Dos, p. 4-3)
      trackSectorUsedMap[i][j] = (state & (1 << (j))) ? false : true;
    }
  }

  // VTOC and the in-RAM sector map are now valid. This flag - not the tree
  // pointer - is what the "ensure loaded" guards must test: an EMPTY
  // catalog legitimately produces a NULL tree, and re-running createTree
  // on every guard would reload the sector map from the on-disk VTOC,
  // forgetting in-RAM allocations that haven't been flushed yet (every
  // findFreeSector would then hand out the same sector).
  vtLoaded = true;

  // FIXME check vt.catalogTrack == 17? Or pass in the whole disk?
  uint8_t catalogTrack = vt.catalogTrack;
  uint8_t catalogSector = vt.catalogSector;
  while (catalogTrack == 17 && catalogSector < spt) {
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

  if (verbose) wozMsg(WOZ_MSG_DEBUG, "Returning tree");
  return tree;
}

bool DosSpector::flushFreeSectorList()
{
  // The tree has to be loaded before we start, so vt is loaded
  if (!vtLoaded)
    createTree();
  
  // Update the bitmap. Each track's free map is two bytes: sectorsUsed[0]
  // holds sectors 8-15 (the high byte) and sectorsUsed[1] holds sectors 0-7,
  // matching how createTree reads it back as (sectorsUsed[0]<<8)|sectorsUsed[1].
  // The byte to touch therefore depends on the SECTOR number j, not the track
  // number i. (This previously keyed on i, which collided all 16 sectors into
  // one byte and corrupted the free map.)
  for (int i=0; i<vt.tracksPerDisk; i++) {
    for (int j=0; j<16; j++) {
      uint8_t bpos = 1<<(j % 8);
      if (trackSectorUsedMap[i][j]) {
        // bit is clear if sector is used
        vt.trackState[i].sectorsUsed[(j >= 8) ? 0 : 1] &= ~bpos;
        if (verbose && i==7) {
          wozMsg(WOZ_MSG_DEBUG, "S%d used", j);
        }
      } else {
        // bit is set if sector is available
        if (verbose && i==7)
          wozMsg(WOZ_MSG_DEBUG, "S%d free", j);
        vt.trackState[i].sectorsUsed[(j >= 8) ? 0 : 1] |= bpos;
      }
    }
  }
  // rewrite the vtoc
  if (!encodeWozTrackSector(17, enphys[0], (uint8_t *)&vt)) {
    wozMsg(WOZ_MSG_ERROR, "Failed to rewrite VTOC with new free sector list");
    return false;
  }

  return true;
}

// side effect: marks it as used
bool DosSpector::findFreeSector(int *trackOut, int *sectorOut)
{
  // The tree has to be loaded before we start, so trackSectorUsedMap is correct
  if (!vtLoaded)
    createTree();
  
  for (int trk=1; trk<35; trk++) {
    for (int sect=0; sect<16; sect++) {
      if (!trackSectorUsedMap[trk][sect]) {
        trackSectorUsedMap[trk][sect] = true;
        *trackOut = trk;
        *sectorOut = sect;
        if (verbose) wozMsg(WOZ_MSG_DEBUG, "Allocating track %d sector %d", trk, sect);
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
    wozMsg(WOZ_MSG_ERROR, "ERROR: don't know how to handle files > 31232 bytes (b/c we need multiple TSlist blocks)");
    return false;
  }

  // The tree has to be loaded before we start
  if (!vtLoaded)
    createTree();

  /*
  Vent *fp = findFileByName(fileName);
  if (fp) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: File '%s' already exists", fileName);
    return false;
  }
  */

  // Build the on-disk form first. DOS 3.3 prefixes most file types with a
  // short header, and that header has to be counted when we allocate
  // sectors - otherwise a file whose length is an exact multiple of 256
  // spills its last few bytes into a sector nobody reserved.
  uint8_t *adulteratedData = NULL;
  uint32_t writeSize = 0;
  switch (fileType) {
  case FT_BIN:
    // 4-byte header: load address (L/H) then length (L/H).
    writeSize = fileSize + 4;
    adulteratedData = (uint8_t *)malloc(writeSize);
    if (!adulteratedData) return false;
    adulteratedData[0] = fileStart & 0xFF;
    adulteratedData[1] = (fileStart >> 8) & 0xFF;
    adulteratedData[2] = fileSize & 0xFF;
    adulteratedData[3] = (fileSize >> 8) & 0xFF;
    memcpy(&adulteratedData[4], fileContents, fileSize);
    break;
  case FT_BAS:
  case FT_INT:
    // 2-byte header: the program length. The caller hands us the bare
    // program image, the same way it hands us bare data for a BIN file.
    writeSize = fileSize + 2;
    adulteratedData = (uint8_t *)malloc(writeSize);
    if (!adulteratedData) return false;
    adulteratedData[0] = fileSize & 0xFF;
    adulteratedData[1] = (fileSize >> 8) & 0xFF;
    memcpy(&adulteratedData[2], fileContents, fileSize);
    break;
  }
  if (!adulteratedData) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: don't know how to set up the file header for this file type");
    return false;
  }

  // We need a TSList
  struct _dosTsList tsList;
  memset(&tsList, 0, sizeof(tsList));
  int tsPairPtr = 0;

  int32_t remainingSize = writeSize;
  while (tsPairPtr < 122 && remainingSize > 0) {
    int t,s;
    if (!findFreeSector(&t, &s)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: unable to find enough free sectors");
      free(adulteratedData);
      return false;
    }
    if (verbose) wozMsg(WOZ_MSG_DEBUG, "Using track %d sector %d for data", t, s);
    tsList.tsPair[tsPairPtr].track = t;
    tsList.tsPair[tsPairPtr].sector = s;
    tsPairPtr++;
    remainingSize -= 256;
  }

  // Write the file to those blocks.
  remainingSize = writeSize;
  uint16_t dataPtr = 0;
  uint8_t sectorData[256];
  for (int b=0; b<tsPairPtr; b++) {
    int t = tsList.tsPair[b].track;
    int s = tsList.tsPair[b].sector;
    uint32_t chunk = (remainingSize >= 256) ? 256 : (uint32_t)remainingSize;
    memset(sectorData, 0, sizeof(sectorData));
    memcpy(sectorData, &adulteratedData[dataPtr], chunk);
    if (verbose) wozMsg(WOZ_MSG_DEBUG, "Writing data to track %d sector %d", t, s);
    if (!encodeWozTrackSector(t, enphys[s], sectorData)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to encodeWozTrackSector");
      free(adulteratedData);
      return false;
    }
    dataPtr += 256;
    remainingSize -= chunk;
  }
  free(adulteratedData);
  adulteratedData = NULL;

  // Find a free block for the TS list itself
  int t,s;
  if (!findFreeSector(&t, &s)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: can't find a free sector for the TS list");
    return false;
  }
  // Write the TS list
  if (verbose) wozMsg(WOZ_MSG_DEBUG, "Writing the TS list to track %d sector %d", t, s);
  if (!encodeWozTrackSector(t, enphys[s], (uint8_t *)&tsList)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to encodeWozTrackSector for TSList");
    return false;
  }

  // Prep the directory entry
  struct _dosFdEntry newEntry;
  memset(&newEntry, 0, sizeof(newEntry));
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
    wozMsg(WOZ_MSG_ERROR, "ERROR: unhandled file type; can't construct directory entry");
    return false;
  }

  char buf[31];
  snprintf(buf, sizeof(buf), "%-30s", fileName);
  if (verbose) wozMsg(WOZ_MSG_DEBUG, "new entry name: '%s'", buf);
  memcpy(newEntry.fileName, buf, 30); // no terminator copied
  // The catalog length field is a sector count, including the T/S list.
  uint16_t sectorCount = (uint16_t)(tsPairPtr + 1);
  newEntry.fileLength[0] = sectorCount & 0xFF;
  newEntry.fileLength[1] = (sectorCount >> 8) & 0xFF;

  // Flush the updated free blocks list before we write the directory entry
  if (!flushFreeSectorList()) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to flush free sector list");
    return false;
  }

  // actually add the directory entry
  if (!addDirectoryEntryForFile(&newEntry)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to add directory entry");
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
  if (!vtLoaded)
    createTree();
  
  // Find the T/S list so we can find the first block of the file, so we can find its length
  uint8_t tsTrack = e->getFirstTrack();
  uint8_t tsSector = e->getFirstSector();
  if (!tsTrack && !tsSector) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: no T/S first entry?");
    *toWhere = NULL;
    return 0;
  }
  if (verbose) wozMsg(WOZ_MSG_DEBUG, "File T/S list starts at track %d sector %d", tsTrack, tsSector);

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
  {
    // The T/S list sector is addressed by its logical sector number in
    // the directory entry. Go through the format-aware helper so the
    // same code path works for DOS 3.2 (13-sector) and DOS 3.3.
    uint8_t trackBuf[256*16];
    if (!readLogicalTrack(tsTrack, trackBuf)) {
      wozMsg(WOZ_MSG_ERROR, "Unable to decode track %d sector %d", tsTrack, tsSector);
      *toWhere = NULL;
      return 0;
    }
    memcpy(sectorData, &trackBuf[256 * tsSector], 256);
  }
  tsList = (struct _dosTsList *)sectorData;
  if (verbose) {
    wozMsg(WOZ_MSG_DEBUG, "TSList sector:");
    char buf[256*3+1]; // "XX " per byte
    size_t off = 0;
    buf[0] = 0;
    for (int i=0; i<256 && off < sizeof(buf); i++) {
      off += snprintf(buf + off, sizeof(buf) - off, "%.2X ", sectorData[i]);
    }
    wozMsg(WOZ_MSG_DEBUG, "%s", buf);
  }

  // Find the length of the file from its first block
  if (!tsList->tsPair[0].track && !tsList->tsPair[0].sector) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: T/S entry has no data sectors?");
    *toWhere = NULL;
    return 0;
  }
  if (tsList->tsPair[0].track > 34 || tsList->tsPair[0].sector > 15) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: invalid T/S entry?");
    *toWhere = NULL;
    return 0;
  }
  
  uint8_t dataTrackData[256*16];
  if (verbose) wozMsg(WOZ_MSG_DEBUG, "About to decode track %d", tsList->tsPair[0].track);
  if (!readLogicalTrack(tsList->tsPair[0].track, dataTrackData)) {
    wozMsg(WOZ_MSG_ERROR, "Unable to decode track %d", tsList->tsPair[0].track);
    *toWhere = NULL;
    return 0;
  }
  if (verbose) {
    wozMsg(WOZ_MSG_DEBUG, "dump of track %d", tsList->tsPair[0].track);
    for (int i=0; i<16; i++) {
      wozMsg(WOZ_MSG_DEBUG, "sector %d", i);
      char buf[256*3+1]; // "XX " per byte
      size_t off = 0;
      buf[0] = 0;
      for (int j=0; j<256 && off < sizeof(buf); j++) {
        off += snprintf(buf + off, sizeof(buf) - off, "%.2X ", dataTrackData[i*256+j]);
      }
      wozMsg(WOZ_MSG_DEBUG, "%s", buf);
    }
    wozMsg(WOZ_MSG_DEBUG, "looking for sector %d", tsList->tsPair[0].sector);
  }
  uint8_t *dataPtr = &dataTrackData[256*tsList->tsPair[0].sector];
  uint32_t fileLength = 0;
  switch (e->getFileType()) {
    case FT_BIN:
      // The 4-byte header (load address, then length) sits in front of the
      // data, and the length counts only what follows it - so the file we
      // hand back is 4 bytes longer than the field says. Leaving the +4 off
      // returned the header plus the first length-4 bytes, quietly chopping
      // the last 4 bytes off every BIN file copied out.
      fileLength = dataPtr[2] + 256*dataPtr[3] + 4;
      break;
    case FT_BAS:
    case FT_INT:
      // The on-disk DOS 3.3 file is a 2-byte length header followed by
      // exactly that many bytes of tokenized program. Return both so the
      // caller gets the full file (listers skip the header via
      // applesoftHeaderBytes()).
      fileLength = dataPtr[0] + 256*dataPtr[1] + 2;
      if (verbose) wozMsg(WOZ_MSG_DEBUG, "BAS/INT file length %d", fileLength);
      break;
    case FT_SYS: // 'S' type
    case FT_REL: // 'R' type
      // Don't know what SYS/REL do for length; skip for now
    case FT_TXT:
      // There's no way to tell the length of this beforehand - we have to
      // go through the full T/S list. It may be sparse (have 00/00 entries
      // in the T/S list), so it really is a full scan...
    default:
      wozMsg(WOZ_MSG_ERROR, "Unimplemented: don't know how to tell file length of this file type");
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
    if (!readLogicalTrack(tsList->tsPair[ptr].track, dataTrackData)) {
      wozMsg(WOZ_MSG_ERROR, "Unable to decode track %d", tsList->tsPair[ptr].track);
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
  wozMsg(WOZ_MSG_ERROR, "Unimplemented: don't know how to follow second track/sector list");
  free (*toWhere);
  *toWhere = NULL;
  return 0;
}

bool DosSpector::addDirectoryEntryForFile(struct _dosFdEntry *e)
{
  // The tree has to be loaded before we start
  if (!vtLoaded)
    createTree();
  
  // Find the first directory entry that's empty

  uint8_t sectorData[256];

  uint8_t catalogTrack = vt.catalogTrack;
  uint8_t catalogSector = vt.catalogSector;
  
  // Spin through all the catalog sectors to find an empty entry
  while (catalogTrack) {
    if (!decodeWozTrackSector(catalogTrack, enphys[catalogSector], sectorData)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to read catalog t %d/s %d", catalogTrack, catalogSector);
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
          wozMsg(WOZ_MSG_ERROR, "Failed to write updated catalog sector");
          return false;
        }

        // succeeded!
        return true;
      }
    }
    catalogTrack = ci->nextCatalogTrack;
    catalogSector = ci->nextCatalogSector;
  }

  wozMsg(WOZ_MSG_ERROR, "addDirectoryEntryForFile failed to find an empty entry slot");
  return false;
}

bool DosSpector::freeDosFileSectors(uint8_t tsTrack, uint8_t tsSector)
{
  int safety = 256; // guard against a circular T/S list chain
  while ((tsTrack || tsSector) && safety-- > 0) {
    uint8_t sectorData[256];
    if (!readLogicalSector(tsTrack, tsSector, sectorData)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to read T/S list at %d/%d", tsTrack, tsSector);
      return false;
    }
    struct _dosTsList *tsList = (struct _dosTsList *)sectorData;

    // Free every data sector this list references (0/0 == sparse hole).
    for (int i = 0; i < 122; i++) {
      uint8_t t = tsList->tsPair[i].track;
      uint8_t s = tsList->tsPair[i].sector;
      if (t == 0 && s == 0) continue;
      if (t < 35 && s < 16) trackSectorUsedMap[t][s] = false;
    }

    uint8_t nextT = tsList->nextTrack;
    uint8_t nextS = tsList->nextSector;
    // Free the T/S list sector itself, now that we've read it.
    if (tsTrack < 35 && tsSector < 16) trackSectorUsedMap[tsTrack][tsSector] = false;

    tsTrack = nextT;
    tsSector = nextS;
  }
  return true;
}

bool DosSpector::writeFileRaw(uint8_t *contents, const char *fileName,
                              uint8_t typeAndFlags, uint32_t size)
{
  if (!vtLoaded)
    createTree();

  uint32_t dataSectors = (size + 255) / 256;
  if (dataSectors > 122) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: don't know how to handle files > 31232 bytes (b/c we "
                          "need multiple TSlist blocks)");
    return false;
  }
  if (strlen(fileName) > 30) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: DOS 3.3 file names may not exceed 30 characters");
    return false;
  }

  // Allocate and write the data sectors, recording them in the T/S list.
  struct _dosTsList tsList;
  memset(&tsList, 0, sizeof(tsList));
  uint32_t remaining = size;
  for (uint32_t b = 0; b < dataSectors; b++) {
    int t, s;
    if (!findFreeSector(&t, &s)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: unable to find enough free sectors");
      return false;
    }
    tsList.tsPair[b].track = t;
    tsList.tsPair[b].sector = s;

    uint8_t sectorData[256];
    memset(sectorData, 0, sizeof(sectorData));
    memcpy(sectorData, &contents[b * 256],
           remaining >= 256 ? 256 : remaining);
    if (!encodeWozTrackSector(t, enphys[s], sectorData)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to encodeWozTrackSector");
      return false;
    }
    remaining -= (remaining >= 256) ? 256 : remaining;
  }

  // The T/S list sector itself.
  int t, s;
  if (!findFreeSector(&t, &s)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: can't find a free sector for the TS list");
    return false;
  }
  if (!encodeWozTrackSector(t, enphys[s], (uint8_t *)&tsList)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to encodeWozTrackSector for TSList");
    return false;
  }

  struct _dosFdEntry newEntry;
  memset(&newEntry, 0, sizeof(newEntry));
  newEntry.firstTrack = t;
  newEntry.firstSector = s;
  newEntry.fileTypeAndFlags = typeAndFlags;

  char buf[31];
  snprintf(buf, sizeof(buf), "%-30s", fileName);
  memcpy(newEntry.fileName, buf, 30); // no terminator copied
  // The catalog length field is a sector count, including the T/S list.
  uint16_t sectorCount = dataSectors + 1;
  newEntry.fileLength[0] = sectorCount & 0xFF;
  newEntry.fileLength[1] = (sectorCount >> 8) & 0xFF;

  if (!flushFreeSectorList()) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to flush free sector list");
    return false;
  }
  if (!addDirectoryEntryForFile(&newEntry)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to add directory entry");
    return false;
  }

  createTree();
  return true;
}

bool DosSpector::setFileLocked(const char *fileName, bool locked)
{
  if (!vtLoaded)
    createTree();

  uint8_t catalogTrack = vt.catalogTrack;
  uint8_t catalogSector = vt.catalogSector;
  uint8_t sectorData[256];

  while (catalogTrack) {
    if (!decodeWozTrackSector(catalogTrack, enphys[catalogSector], sectorData)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to read catalog %d/%d", catalogTrack, catalogSector);
      return false;
    }
    struct _catalogInfo *ci = (struct _catalogInfo *)sectorData;

    for (int i = 0; i < 7; i++) {
      struct _dosFdEntry *fe = &ci->fileEntries[i];
      uint8_t c0 = (uint8_t)fe->fileName[0];
      if (c0 == 0x00 || c0 == 0xFF)
        continue; // empty or deleted slot

      // Match names the same way the lister and rm do.
      char clean[31];
      int n = 0;
      for (int j = 0; j < 30; j++) {
        char ch = fe->fileName[j] & 0x7F;
        if (ch == ' ') break;
        clean[n++] = ch;
      }
      clean[n] = '\0';
      if (strcmp(clean, fileName) != 0)
        continue;

      if (locked)
        fe->fileTypeAndFlags |= 0x80;
      else
        fe->fileTypeAndFlags &= 0x7F;

      if (!encodeWozTrackSector(catalogTrack, enphys[catalogSector], sectorData)) {
        wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write catalog sector %d/%d",
                              catalogTrack, catalogSector);
        return false;
      }

      createTree(); // re-read so subsequent commands see the change
      return true;
    }

    catalogTrack = ci->nextCatalogTrack;
    catalogSector = ci->nextCatalogSector;
  }

  wozMsg(WOZ_MSG_ERROR, "ERROR: '%s' not found; can't set its locked flag", fileName);
  return false;
}

bool DosSpector::removeFile(const char *fileName)
{
  if (!vtLoaded)
    createTree();

  uint8_t catalogTrack = vt.catalogTrack;
  uint8_t catalogSector = vt.catalogSector;
  uint8_t sectorData[256];

  while (catalogTrack) {
    if (!decodeWozTrackSector(catalogTrack, enphys[catalogSector], sectorData)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to read catalog %d/%d", catalogTrack, catalogSector);
      return false;
    }
    struct _catalogInfo *ci = (struct _catalogInfo *)sectorData;

    for (int i = 0; i < 7; i++) {
      struct _dosFdEntry *fe = &ci->fileEntries[i];
      uint8_t c0 = (uint8_t)fe->fileName[0];
      if (c0 == 0x00 || c0 == 0xFF)
        continue; // empty or already-deleted slot

      // Clean the on-disk name exactly the way the directory lister does:
      // strip the high bit and stop at the first space. That keeps "what
      // ls shows" and "what rm matches" identical.
      char clean[31];
      int n = 0;
      for (int j = 0; j < 30; j++) {
        char ch = fe->fileName[j] & 0x7F;
        if (ch == ' ') break;
        clean[n++] = ch;
      }
      clean[n] = '\0';
      if (strcmp(clean, fileName) != 0)
        continue;

      // Free the file's data and T/S list sectors (before we lose the
      // first-T/S-list pointer to the tombstone below).
      if (!freeDosFileSectors(fe->firstTrack, fe->firstSector))
        return false;

      // Tombstone the catalog entry. DOS convention: copy the original
      // first-T/S-list track to the last byte of the name field (for
      // undelete) and set the track field to $FF. We also zero the first
      // name byte, which is how this tool's catalog reader (createTree) and
      // the slot-reuse scan recognize a free entry.
      fe->fileName[29] = fe->firstTrack;
      fe->firstTrack = 0xFF;
      fe->fileName[0] = 0x00;

      if (!encodeWozTrackSector(catalogTrack, enphys[catalogSector], sectorData)) {
        wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write catalog sector %d/%d",
                              catalogTrack, catalogSector);
        return false;
      }
      if (!flushFreeSectorList()) {
        wozMsg(WOZ_MSG_ERROR, "ERROR: failed to flush free sector list");
        return false;
      }

      createTree(); // re-read so subsequent commands see the change
      wozMsg(WOZ_MSG_INFO, "Removed '%s'", fileName);
      return true;
    }

    catalogTrack = ci->nextCatalogTrack;
    catalogSector = ci->nextCatalogSector;
  }

  wozMsg(WOZ_MSG_ERROR, "File '%s' not found", fileName);
  return false;
}

bool DosSpector::probe()
{
  // Apple DOS puts the VTOC at track 17 / sector 0 (logical). We accept
  // both DOS 3.2 (dosVersion=2, 13 sectors/track, 5&3 codec) and DOS 3.3
  // (dosVersion=3, 16 sectors/track, 6&2 codec) images; if neither
  // decodes into a plausible VTOC, this isn't a DOS image.
  uint8_t track[256*16];
  bool ok = false;
  if (isThirteenSectorDisk()) {
    uint8_t buf[256*13];
    ok = decodeWozTrack13ToDsk(17, buf);
    if (ok) memcpy(track, buf, 256*13);
  } else {
    ok = decodeWozTrackToDsk(17, T_DSK, track);
  }
  if (!ok) return false;
  struct _vtoc *v = (struct _vtoc *)track;
  if (v->dosVersion != 2 && v->dosVersion != 3) return false;
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

  if (!vtLoaded) createTree();

  uint8_t curT = e->getFirstTrack();
  uint8_t curS = e->getFirstSector();
  if (!curT && !curS) return 0;

  uint32_t dataSectors = 0;
  int safety = 256;
  while ((curT || curS) && safety-- > 0) {
    uint8_t sectorData[256];
    if (!readLogicalSector(curT, curS, sectorData)) {
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
  // references. 0/0 pairs inside the list are skipped - treated as
  // unused slots rather than sparse-file holes. Unlike getFileContents,
  // the in-file length header is ignored, so this is the right way to
  // recover files whose catalog "length" is a stub hiding a larger payload.

  if (!vtLoaded) createTree();

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
      if (!readLogicalSector(curT, curS, sectorData)) {
        wozMsg(WOZ_MSG_ERROR, "ERROR: failed to read T/S list at %d/%d", curT, curS);
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
      if (!readLogicalSector(curT, curS, sectorData)) {
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
        if (!readLogicalSector(t, s, dataSector)) {
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

bool DosSpector::analyzeFile(Vent *fp, struct DosFileAnalysis *out)
{
  memset(out, 0, sizeof(*out));
  out->catalogSectors = fp->getBlocksUsed();

  uint8_t tsTrack = fp->getFirstTrack();
  uint8_t tsSector = fp->getFirstSector();
  if (!tsTrack && !tsSector) {
    // No T/S list pointer - nothing to walk
    return true;
  }
  out->hasTsList = true;

  uint8_t firstDataSector[256];

  uint8_t curT = tsTrack, curS = tsSector;
  int safetyBound = 256; // guard against circular T/S list chains
  while ((curT || curS) && safetyBound-- > 0) {
    uint8_t sectorData[256];
    if (!readLogicalSector(curT, curS, sectorData)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to read T/S list at %d/%d", curT, curS);
      break;
    }
    out->tsListSectors++;
    struct _dosTsList *tsList = (struct _dosTsList *)sectorData;

    for (int i = 0; i < 122; i++) {
      uint8_t t = tsList->tsPair[i].track;
      uint8_t s = tsList->tsPair[i].sector;
      if (t == 0 && s == 0) {
        // In a T/S list, 0/0 is either end-of-list or a sparse hole.
        // We count them as sparse and keep scanning the rest of the list;
        // trailing holes just look like extra sparse entries.
        out->sparseGaps++;
        continue;
      }
      if (t > 34 || s > 15) {
        wozMsg(WOZ_MSG_WARNING, "WARNING: invalid T/S pair %d/%d in list at %d/%d",
               t, s, curT, curS);
        continue;
      }
      out->dataSectors++;
      if (!out->haveFirstData) {
        if (readLogicalSector(t, s, firstDataSector)) {
          out->haveFirstData = true;
          out->firstDataTrack = t;
          out->firstDataSector = s;
        }
      }
    }

    curT = tsList->nextTrack;
    curS = tsList->nextSector;
  }

  if (!out->haveFirstData)
    return true;

  switch (fp->getFileType()) {
  case FT_BAS:
  case FT_INT:
    out->logicalLength = firstDataSector[0] + 256 * firstDataSector[1];
    out->haveLengthHeader = true;
    break;
  case FT_BIN:
    out->loadAddress = firstDataSector[0] + 256 * firstDataSector[1];
    out->logicalLength = firstDataSector[2] + 256 * firstDataSector[3];
    out->haveLengthHeader = true;
    break;
  default:
    // Text files are 0x00-terminated (and possibly sparse); other types
    // have no standard length header
    break;
  }

  if (out->haveLengthHeader) {
    // Compare logical length against the walked data allocation, minus
    // the length-header bytes themselves (2 for BAS/INT, 4 for BIN).
    uint32_t headerBytes = (fp->getFileType() == FT_BIN) ? 4 : 2;
    out->dataBytes = out->dataSectors * 256;
    if (out->dataBytes > headerBytes) out->dataBytes -= headerBytes;
    out->lengthFarBelowAllocation = (out->logicalLength + 512 < out->dataBytes);
  }
  return true;
}

bool DosSpector::getSectorUsage(bool used[35][16])
{
  if (!vtLoaded)
    createTree();
  if (!vtLoaded)
    return false;

  // Straight from the VTOC, rather than trackSectorUsedMap: that can hold
  // allocations that haven't been flushed yet
  memset(used, 0, sizeof(bool) * 35 * 16);
  for (int i=0; i<vt.tracksPerDisk && i<35; i++) {
    uint16_t state = (vt.trackState[i].sectorsUsed[0] << 8) |
      vt.trackState[i].sectorsUsed[1];
    for (int j=0; j<16; j++) {
      // 1-bits are free; 0-bits are used (Beneath Apple Dos, p. 4-3)
      used[i][j] = (state & (1 << (j))) ? false : true;
    }
  }
  return true;
}

