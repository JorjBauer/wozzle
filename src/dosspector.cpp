#include "dosspector.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "vent.h"
#include "wozmsg.h"
#include "wozhost.h"

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
  // The fileType we're handed is a ProDOS file type, because they're more
  // expressive than the DOS ones, so the callers can use just one
  uint8_t typeAndFlags;
  uint32_t headerSize = 0;
  switch (fileType) {
  case FT_TXT: typeAndFlags = 0;  break; // 'T' text
  case FT_INT: typeAndFlags = 1;  headerSize = 2; break; // 'I' Integer BASIC
  case FT_BAS: typeAndFlags = 2;  headerSize = 2; break; // 'A' Applesoft
  case FT_BIN: typeAndFlags = 4;  headerSize = 4; break; // 'B' binary
  case FT_SYS: typeAndFlags = 8;  break; // 'S'
  case FT_REL: typeAndFlags = 16; break; // 'R' relocatable
  default:
    wozMsg(WOZ_MSG_ERROR, "ERROR: DOS 3.3 has no file type for ProDOS type "
           "$%02X", fileType);
    return false;
  }
  // The I/A/B header's length is 16 bits
  if (headerSize && fileSize > 0xFFFF) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: a DOS 3.3 %s file can't be over 65535 "
           "bytes", fileType == FT_BIN ? "B" : fileType == FT_BAS ? "A" : "I");
    return false;
  }

  // Build the on-disk form: DOS 3.3 puts a short header in front of I, A
  // and B files (B: load address, then length; I and A: just the length),
  // and stores T, S and R files as they are
  uint32_t writeSize = fileSize + headerSize;
  uint8_t *onDisk = (uint8_t *)wozMalloc(writeSize ? writeSize : 1);
  if (!onDisk) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: out of memory");
    return false;
  }
  uint8_t *h = onDisk;
  if (fileType == FT_BIN) {
    *h++ = fileStart & 0xFF;
    *h++ = (fileStart >> 8) & 0xFF;
  }
  if (headerSize) {
    *h++ = fileSize & 0xFF;
    *h++ = (fileSize >> 8) & 0xFF;
  }
  memcpy(h, fileContents, fileSize);

  bool ok = writeFileRaw(onDisk, fileName, typeAndFlags, writeSize);
  wozFree(onDisk);
  return ok;
}

uint32_t DosSpector::getFileContents(Vent *e, char **toWhere)
{
  // The file's own data, the way DOS hands it to a program: I, A and B
  // files lose the header in front of them (the length, and for B the load
  // address first), and their length comes from it. A T file ends at its
  // first $00, as a sequential text file does (there's no telling a
  // random-access one apart, so use getFileAllocation for those). Anything
  // else (S, R, and the new A and B types) is the whole raw stream.
  *toWhere = NULL;
  char *raw = NULL;
  uint32_t rawLen = getFileAllocation(e, &raw);
  if (!rawLen) return 0;

  uint32_t start = 0;
  uint32_t len = rawLen;
  const uint8_t *r = (const uint8_t *)raw;
  switch (e->getFileType()) {
  case FT_BIN:
    start = 4;
    len = r[2] + 256 * r[3];
    break;
  case FT_BAS:
  case FT_INT:
    start = 2;
    len = r[0] + 256 * r[1];
    break;
  case FT_TXT: {
    const uint8_t *z = (const uint8_t *)memchr(r, 0, rawLen);
    len = z ? (uint32_t)(z - r) : rawLen;
    break;
  }
  default:
    break;
  }
  if (start + len > rawLen) {
    // BSAVE over a longer file leaves a stale length behind, and a damaged
    // file can claim anything; we hand back what's really there
    wozMsg(WOZ_MSG_WARNING, "WARNING: '%s' says it's %u bytes, but only %u "
           "are allocated", e->getName(), (unsigned)len,
           (unsigned)(rawLen > start ? rawLen - start : 0));
    len = rawLen > start ? rawLen - start : 0;
  }
  if (!len) {
    wozFree(raw);
    return 0;
  }
  if (start)
    memmove(raw, raw + start, len);
  *toWhere = raw;
  return len;
}

uint16_t DosSpector::getLoadAddress(Vent *e)
{
  if (e->getFileType() == FT_BAS)
    return 0x0801; // where Applesoft programs always load
  if (e->getFileType() != FT_BIN)
    return 0;
  // The load address is the first word of the file's first data sector
  struct DosFileAnalysis a;
  if (!analyzeFile(e, &a) || !a.haveLengthHeader)
    return 0;
  return a.loadAddress;
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

  if (strlen(fileName) > 30) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: DOS 3.3 file names may not exceed 30 characters");
    return false;
  }

  // Every file gets at least one data sector, and each T/S list sector
  // names up to 122 of them; more than that takes a chain of T/S lists
  uint32_t dataSectors = (size + 255) / 256;
  if (!dataSectors) dataSectors = 1;
  uint32_t listSectors = (dataSectors + 121) / 122;
  if (dataSectors + listSectors > 35 * 16) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: the disk is full (not enough free sectors)");
    return false;
  }

  // Allocate everything before writing anything, so running out of room
  // leaves the disk alone. If we do run out, then re-reading the VTOC puts
  // back the in-RAM free map.
  struct _tsPair *lists = (struct _tsPair *)wozMalloc(listSectors * sizeof(struct _tsPair));
  struct _tsPair *data = (struct _tsPair *)wozMalloc(dataSectors * sizeof(struct _tsPair));
  if (!lists || !data) {
    wozFree(lists);
    wozFree(data);
    wozMsg(WOZ_MSG_ERROR, "ERROR: out of memory");
    return false;
  }
  for (uint32_t i = 0; i < listSectors + dataSectors; i++) {
    int t, s;
    if (!findFreeSector(&t, &s)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: the disk is full (not enough free sectors)");
      wozFree(lists);
      wozFree(data);
      createTree();
      return false;
    }
    // The first T/S list comes first, the way DOS lays a file out
    struct _tsPair *p = (i < listSectors) ? &lists[i] : &data[i - listSectors];
    p->track = t;
    p->sector = s;
  }

  bool ok = true;
  for (uint32_t b = 0; b < dataSectors && ok; b++) {
    uint8_t sectorData[256];
    memset(sectorData, 0, sizeof(sectorData));
    uint32_t off = b * 256;
    if (off < size)
      memcpy(sectorData, &contents[off], (size - off) >= 256 ? 256 : size - off);
    ok = encodeWozTrackSector(data[b].track, enphys[data[b].sector], sectorData);
  }

  for (uint32_t l = 0; l < listSectors && ok; l++) {
    struct _dosTsList tsList;
    memset(&tsList, 0, sizeof(tsList));
    if (l + 1 < listSectors) {
      tsList.nextTrack = lists[l + 1].track;
      tsList.nextSector = lists[l + 1].sector;
    }
    // Which data sector of the file this list starts at
    uint16_t sectorOffset = (uint16_t)(l * 122);
    tsList.sectorOffset[0] = sectorOffset & 0xFF;
    tsList.sectorOffset[1] = sectorOffset >> 8;
    for (uint32_t i = 0; i < 122 && l * 122 + i < dataSectors; i++)
      tsList.tsPair[i] = data[l * 122 + i];
    ok = encodeWozTrackSector(lists[l].track, enphys[lists[l].sector],
                              (uint8_t *)&tsList);
  }
  struct _tsPair first = lists[0];
  wozFree(lists);
  wozFree(data);
  if (!ok) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to encodeWozTrackSector");
    createTree();
    return false;
  }

  struct _dosFdEntry newEntry;
  memset(&newEntry, 0, sizeof(newEntry));
  newEntry.firstTrack = first.track;
  newEntry.firstSector = first.sector;
  newEntry.fileTypeAndFlags = typeAndFlags;

  char buf[31];
  snprintf(buf, sizeof(buf), "%-30s", fileName);
  memcpy(newEntry.fileName, buf, 30); // no terminator copied
  // The catalog length field is a sector count, T/S lists included
  uint16_t sectorCount = (uint16_t)(dataSectors + listSectors);
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

      // Match names the same way the lister and rm do
      char clean[31];
      dosCatalogName(fe, clean);
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

      // The same name ls shows, so "what ls shows" and "what rm matches"
      // stay identical
      char clean[31];
      dosCatalogName(fe, clean);
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
  // Every data sector the T/S list chain names, in place: a 0/0 pair is a
  // sparse hole and reads back as a zero sector, so a random-access text
  // file keeps its records where they were. The stream ends with the last
  // sector actually named, and the in-file length header is ignored, so
  // this is also the way to recover a file whose header is a stub hiding a
  // larger payload.

  if (!vtLoaded) createTree();

  *toWhere = NULL;

  uint8_t tsTrack = e->getFirstTrack();
  uint8_t tsSector = e->getFirstSector();
  if (!tsTrack && !tsSector) {
    return 0;
  }

  // First pass: find the last data sector named, so we can size the buffer
  uint32_t sectors = 0; // index of the last named sector, plus one
  {
    uint8_t curT = tsTrack, curS = tsSector;
    uint32_t base = 0;
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
          sectors = base + i + 1;
        }
      }
      base += 122;
      curT = tsList->nextTrack;
      curS = tsList->nextSector;
    }
  }

  uint32_t outSize = sectors * 256;
  if (outSize == 0) {
    return 0;
  }
  *toWhere = (char *)wozCalloc(outSize, 1);
  if (!*toWhere) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: out of memory");
    return 0;
  }

  // Second pass: copy each named data sector to its place
  {
    uint8_t curT = tsTrack, curS = tsSector;
    uint32_t base = 0;
    int safety = 256;
    while ((curT || curS) && safety-- > 0 && base < sectors) {
      uint8_t sectorData[256];
      if (!readLogicalSector(curT, curS, sectorData)) {
        wozFree(*toWhere);
        *toWhere = NULL;
        return 0;
      }
      struct _dosTsList *tsList = (struct _dosTsList *)sectorData;
      for (int i = 0; i < 122 && base + i < sectors; i++) {
        uint8_t t = tsList->tsPair[i].track;
        uint8_t s = tsList->tsPair[i].sector;
        if (t == 0 && s == 0) continue; // a hole: stays zero
        // If we can't read it, it stays a zero-filled hole too, rather
        // than aborting, so the rest of the file can still be recovered
        readLogicalSector(t, s, (uint8_t *)(*toWhere) + (base + i) * 256);
      }
      base += 122;
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

uint8_t DosSpector::volumeNumber()
{
  if (!vtLoaded)
    createTree();
  return vtLoaded ? vt.volumeNumber : 0;
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

