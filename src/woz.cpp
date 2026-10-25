#include "woz.h"
#include <errno.h>
#include <string.h>
#include "crc32.h"
#include "nibutil.h"
#include "wozhost.h"
#include "wozmsg.h"
#include "wozversion.h"

// Block number we start packing data bits after (Woz 2.0 images)
#define STARTBLOCK 3

// Size of a file by name, or -1 if it can't be opened. This goes through
// the host's hooks (rather than stat) so it works wherever open does.
static long sizeOfFile(const char *filename)
{
  int sfd = wozOpen(filename, WOZ_OPEN_READ);
  if (sfd == -1)
    return -1;
  long size = wozSeek(sfd, 0, SEEK_END);
  wozClose(sfd);
  return size;
}

#define PREP_SECTION(fd, t) {        \
  uint32_t type = t;                 \
  if (!write32(fd, type))            \
    return false;                    \
  if (!write32(fd, 0))               \
    return false;                    \
  curpos = wozSeek(fd, 0, SEEK_CUR); \
 }

#define END_SECTION(fd) {                     \
  long endpos = wozSeek(fd, 0, SEEK_CUR);     \
  wozSeek(fd, curpos-4, SEEK_SET);            \
  uint32_t chunksize = endpos - curpos;       \
  if (!write32(fd, chunksize))                \
    return false;                             \
  wozSeek(fd, endpos, SEEK_SET);              \
  }

Woz::Woz(bool verbose)
{
  fd = -1;
  trackPointer = 0;
  trackBitIdx = 0x80;
  trackBitCounter = 0;
  trackByteFromDataTrack = 255;
  trackLoopCounter = 0;
  imageType = T_AUTO;
  hdvPromotedFromFloppyExt = false;
  metaData = NULL;
  this->verbose = verbose;

  memset(&quarterTrackMap, 255, sizeof(quarterTrackMap));
  memset(&di, 0, sizeof(diskInfo));
  memset(&tracks, 0, sizeof(tracks));
  randPtr = 0;
  headWindow = 0;
  hdvData = NULL;
  hdvByteSize = 0;
  hdvDirty = false;
  twoImgComment = NULL;
  twoImgCreatorData = NULL;
  clearTwoImg();
  thirteenSector = -1;
}

Woz::~Woz()
{
  if (fd != -1) {
    wozClose(fd);
    fd = -1;
  }

  for (int i=0; i<160; i++) {
    if (tracks[i].trackData) {
      wozFree(tracks[i].trackData);
      tracks[i].trackData = NULL;
    }
  }
  if (metaData) {
    wozFree(metaData);
    metaData = NULL;
  }
  if (hdvData) {
    wozFree(hdvData);
    hdvData = NULL;
  }
  clearTwoImg();
}

// external interface for a disk subsystem to write a bit
bool Woz::writeNextWozBit(uint8_t datatrack, uint8_t bit)
{
  if (datatrack == 0xFF) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: tried to write bit on half-track; not implemented");
    return true;
  }

  if (!tracks[datatrack].trackData) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: tried to writeNextWozBit to a data track that's not loaded, and we can't possibly tell which QT that should be");
    return false;
  }

  if (trackByteFromDataTrack != datatrack) {
    // FIXME what if trackpointer is out of bounds for this track
    trackByte = tracks[datatrack].trackData[trackPointer];
    trackByteFromDataTrack = datatrack;
  }

  if (bit)
    trackByte |= trackBitIdx;
  else
    trackByte &= ~trackBitIdx;
  
  tracks[datatrack].trackData[trackPointer] = trackByte;

  advanceBitStream(datatrack);
  tracks[datatrack].dirty = true;
  
  return true;
}

// external interface for a disk interface to write a byte
bool Woz::writeNextWozByte(uint8_t datatrack, uint8_t b)
{
  if (datatrack == 0xFF) {
    // Not on a track, so pretend to write but throw it away. FIXME:
    // probably want to create a new Woz track entry here.
    wozMsg(WOZ_MSG_ERROR, "ERROR: tried to write to a half track; not implemented");
    return true;
  }

  if (!tracks[datatrack].trackData) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: tried to write to a track that's not loaded, and it's not possible to tell what QT was meant");
    return false;
  }
  
  // We could be byte-aligned, but it's not guaranteed, so this
  // handles it bitwise.
  for (uint8_t i=0; i<8; i++) {
    writeNextWozBit(datatrack, b & (1 << (7-i)) ? 1 : 0);
  }
  return true;
}

uint8_t Woz::getNextWozBit(uint8_t datatrack)
{
  if (datatrack >= 160) {
    if (datatrack != 255) {
      wozMsg(WOZ_MSG_WARNING, "datatrack %d out of range", datatrack);
    }
    return 0;
  }
  
  if (!tracks[datatrack].trackData) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: getNextWozBit was called without the track being cached, and it can't possibly know which QT to load it from");
    return 0;
  }

  if (trackByteFromDataTrack != datatrack) {
    // Cross-track sync per WOZ 2.0 reference: when moving to a new
    // track, scale the bit pointer so the head stays over the same
    // rotational position. new_pos = old_pos * new_len / old_len.
    // Without this, games with custom-encoded tracks (e.g. Miner
    // 2049er II track 1 is a solid 0xA5 pattern) read at the wrong
    // 8-bit alignment and fail their protection checks.
    if (trackByteFromDataTrack < 160 &&
        tracks[trackByteFromDataTrack].bitCount > 0 &&
        tracks[datatrack].bitCount > 0 &&
        trackBitCounter < tracks[trackByteFromDataTrack].bitCount) {
      uint64_t scaled =
        (uint64_t)trackBitCounter * tracks[datatrack].bitCount /
        tracks[trackByteFromDataTrack].bitCount;
      trackBitCounter = (uint32_t)scaled;
      trackPointer = trackBitCounter / 8;
      trackBitIdx = 0x80 >> (trackBitCounter & 7);
    }
    trackByte = tracks[datatrack].trackData[trackPointer];
    trackByteFromDataTrack = datatrack;
  }

  // It's assumed that trackByte is set properly when we get here. It
  // should be set when we load image or change tracks, and it's
  // changed again when we advanceBitStream.

  uint8_t ret = (trackByte & trackBitIdx) ? 1 : 0;
  advanceBitStream(datatrack);
  return ret;
}

void Woz::advanceBitStream(uint8_t datatrack)
{
  trackBitCounter++;

  trackBitIdx >>= 1;
  if (!trackBitIdx) {
    trackBitIdx = 0x80;
    trackPointer++;

    // FIXME this is kinda redundant since we're checking
    // trackBitCounter after, but we want to not load from out of
    // bounds here, so unless we always set trackByte after the bit
    // range check below I'm not sure we can get rid of this one
    if ((di.version >= 2 && trackPointer < tracks[datatrack].blockCount*512) ||
	(di.version == 1 && trackPointer < NIBTRACKSIZE)
	) {
      trackByte = tracks[datatrack].trackData[trackPointer];
      trackByteFromDataTrack = datatrack;
    }
  }

  // This could have " || trackPointer >=
  // tracks[datatrack].bitCount/8" but it should be totally redundant
  if (trackBitCounter >= tracks[datatrack].bitCount) {
    trackPointer = 0;
    trackBitIdx = 0x80;
    trackBitCounter = 0;
    trackLoopCounter++;
    trackByte = tracks[datatrack].trackData[trackPointer];
    trackByteFromDataTrack = datatrack;
  }
}

uint8_t Woz::fakeBit()
{
  // WOZ reference recommends roughly 30% ones. AND three independent
  // 50/50 samples to get ~12.5% ones? That's too sparse. AND two
  // gives 25%; OR of (AND of 2) with a 1/8 extra gets close to 30%
  // without needing floating point. Simpler approach: keep the
  // random buffer approach, but only count a bit as 1 if it and a
  // second independent bit are both set - that gives 25%. Close
  // enough; Print Shop just needs "long runs of fake bits don't
  // consistently produce nibbles".
  if (randPtr == 0) {
    randPtr = 0x80;
    randData = (uint8_t) ((float)256 * rand() / (RAND_MAX + 1.0));
  }
  uint8_t a = (randData & randPtr) ? 1 : 0;
  randPtr >>= 1;
  if (randPtr == 0) {
    randPtr = 0x80;
    randData = (uint8_t) ((float)256 * rand() / (RAND_MAX + 1.0));
  }
  uint8_t b = (randData & randPtr) ? 1 : 0;
  randPtr >>= 1;
  return a & b;
}

void Woz::dumpTrackState(uint8_t datatrack)
{
  wozMsg(WOZ_MSG_DEBUG, "TRACK DUMP: trk=%d bitCount=%d trackPointer=%d trackBitCounter=%d trackBitIdx=%02X headWindow=%02X",
          datatrack,
          tracks[datatrack].trackData ? (int)tracks[datatrack].bitCount : -1,
          (int)trackPointer,
          (int)trackBitCounter,
          trackBitIdx, headWindow);
  if (tracks[datatrack].trackData) {
    int ptr = trackPointer;
    char buf[256];
    size_t off = snprintf(buf, sizeof(buf), "TRACK DATA @ptr(%d):", ptr);
    for (int dd = 0; dd < 32 && off < sizeof(buf); dd++) {
      off += snprintf(buf + off, sizeof(buf) - off, " %02X", tracks[datatrack].trackData[ptr + dd]);
    }
    wozMsg(WOZ_MSG_DEBUG, "%s", buf);
    off = snprintf(buf, sizeof(buf), "TRACK DATA @0:");
    for (int dd = 0; dd < 64 && off < sizeof(buf); dd++) {
      off += snprintf(buf + off, sizeof(buf) - off, " %02X", tracks[datatrack].trackData[dd]);
    }
    wozMsg(WOZ_MSG_DEBUG, "%s", buf);
  }
}

uint8_t Woz::nextDiskBit(uint8_t datatrack)
{
  if (!tracks[datatrack].trackData) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: nextDiskBit was called without the track being cached, and it can't possibly know which QT to load it from");
    return 0;
  }

  // MC3470 emulation per WOZ 2.0 reference: slide in the next bit,
  // report bit 1 (one position behind) to model the chip's ~5µs
  // real-transition latency. When the last four bits were all zero
  // the chip's AGC hallucinates pulses, so we inject fakeBit() in
  // place of bit 1. headWindow is a Woz member so it persists across
  // track switches - the real chip lives in the drive, not on the
  // track.
  headWindow <<= 1;
  headWindow |= getNextWozBit(datatrack);
  if ((headWindow & 0x0f) != 0x00) {
    return (headWindow & 0x02) >> 1;
  }
  return fakeBit();
}

uint8_t Woz::nextDiskByte(uint8_t datatrack)
{
  if (!tracks[datatrack].trackData) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: nextDiskByte was called without the track being cached, and it can't possibly know which QT to load it from");
    return 0;
  }

  uint8_t d = 0;
  while ((d & 0x80) == 0) {
    d <<= 1;
    d |= nextDiskBit(datatrack);
  }
  return d;
}

// Like nextDiskByte, but reads the *raw* bitstream via getNextWozBit
// instead of the MC3470 read-head emulation (nextDiskBit). The emulation
// reports the bit one position behind the real cursor and injects random
// fake bits on long zero-runs; both leave trackPointer/trackBitIdx offset
// from the data we just matched. That's harmless for pure reads, but the
// sector *writer* (writeNibSectorDataToDataTrack) reuses this cursor to
// splice new nibbles in place - and a sub-byte skew there shifts every
// following bit, smearing the write across adjacent sectors. So scanning
// for the write position must use this exact, deterministic reader.
uint8_t Woz::nextRawWozByte(uint8_t datatrack)
{
  if (!tracks[datatrack].trackData) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: nextRawWozByte was called without the track being cached, and it can't possibly know which QT to load it from");
    return 0;
  }

  uint8_t d = 0;
  while ((d & 0x80) == 0) {
    d <<= 1;
    d |= getNextWozBit(datatrack);
  }
  return d;
}

static bool write8(int fd, uint8_t v)
{
  if (wozWrite(fd, &v, 1) != 1)
    return false;
  return true;
}

static bool write16(int fd, uint16_t v)
{
  if (!write8(fd, v & 0xFF))
    return false;
  v >>= 8;
  if (!write8(fd, v & 0xFF))
    return false;
  return true;
}

static bool write32(int fd, uint32_t v)
{
  for (int i=0; i<4; i++) {
    if (!write8(fd, v&0xFF))
      return false;
    v >>= 8;
  }
  return true;
}

static bool read8(int fd, uint8_t *toWhere)
{
  uint8_t r;
  if (wozRead(fd, &r, 1) != 1)
    return false;
  *toWhere = r;

  return true;
}

static bool read16(int fd, uint16_t *toWhere)
{
  uint16_t ret = 0;
  for (int i=0; i<2; i++) {
    uint8_t r;
    if (!read8(fd, &r)) {
      return false;
    }
    ret >>= 8;
    ret |= (r<<8);
  }

  *toWhere = ret;

  return true;
}

static bool read32(int fd, uint32_t *toWhere)
{
  uint32_t ret = 0;
  for (int i=0; i<4; i++) {
    uint8_t r;
    if (!read8(fd, &r)) {
      return false;
    }
    ret >>= 8;
    ret |= (r<<24);
  }

  *toWhere = ret;

  return true;
}

bool Woz::writeFile(const char *filename, uint8_t forceType)
{
  if (forceType == T_AUTO) {
    // Try to determine type from the file extension
    const char *p = strrchr(filename, '.');
    if (!p) {
      wozMsg(WOZ_MSG_ERROR, "Unable to determine file type of '%s'", filename);
      return false;
    }
    if (strcasecmp(p, ".woz") == 0) {
      forceType = T_WOZ;
    } else if (strcasecmp(p, ".dsk") == 0 ||
	       strcasecmp(p, ".do") == 0) {
      forceType = T_DSK;
    } else if (strcasecmp(p, ".po")  == 0) {
      forceType = T_PO;
    } else if (strcasecmp(p, ".nib") == 0) {
      forceType = T_NIB;
    } else if (strcasecmp(p, ".hdv") == 0 ||
               strcasecmp(p, ".img") == 0) {
      forceType = T_HDV;
    } else if (isTwoImgName(filename)) {
      forceType = T_2MG;
    } else if (strcasecmp(p, ".d13") == 0) {
      forceType = T_D13;
    } else {
      wozMsg(WOZ_MSG_ERROR, "Unable to determine file type of '%s'", filename);
      return false;
    }
  }

  if ((forceType == T_DSK || forceType == T_PO) && isThirteenSectorDisk()) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: this is a 13-sector disk, and a .dsk or .po holds 16 "
           "sectors a track; save it as .d13, .woz, .nib or .2mg");
    return false;
  }

  // A .po bigger than a floppy is raw ProDOS-order blocks, which is just
  // what a hard-disk image holds, so any of them can be written as one (an
  // 800K .po is how real Apple Pascal 3.5" disks and plenty of ProDOS
  // volumes ship). It has to be at least a floppy's size, or it would be
  // read back through the floppy loader. An oversized .dsk that was read
  // as raw blocks goes back the same way; a genuine .hdv/.img can't
  // become a DOS-order image.
  if (imageType == T_HDV && forceType == T_PO) {
    if (hdvByteSize < 35 * 16 * 256) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: a hard-disk image smaller than a floppy "
             "(%u bytes) must stay .hdv or .img", (unsigned)hdvByteSize);
      return false;
    }
    forceType = T_HDV;
  }
  if (imageType == T_HDV && hdvPromotedFromFloppyExt && forceType == T_DSK)
    forceType = T_HDV;

  // Anything genuinely floppy-oriented (.woz, .nib) still can't represent
  // a hard-disk image. Say so rather than write truncated garbage.
  if (imageType == T_HDV && forceType != T_HDV && forceType != T_2MG) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: can't convert a hard-disk image to a floppy format");
    return false;
  }

  // We write to a file next to the real one and rename it into place once
  // it's all there, so a save that fails partway (a full disk, a track
  // that won't decode) leaves the old file as it was. A host with no
  // rename gets the file written in place; if that fails, we only remove
  // what we wrote when there was no file there before, since otherwise
  // the partial one is all that's left of it.
  bool inPlace = !wozCanRename();
  char *tmp = NULL;
  if (!inPlace) {
    size_t n = strlen(filename) + sizeof(".wozsave");
    tmp = (char *)wozMalloc(n);
    if (tmp)
      snprintf(tmp, n, "%s.wozsave", filename);
    else
      inPlace = true;
  }
  bool existed = false;
  if (inPlace) {
    int efd = wozOpen(filename, WOZ_OPEN_READ);
    existed = (efd != -1);
    if (existed) wozClose(efd);
  }
  const char *out = inPlace ? filename : tmp;

  bool ok = writeFileAs(out, forceType);
  if (ok && !inPlace && wozRename(tmp, filename) != 0) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: couldn't move the saved image into place as '%s': %s",
           filename, strerror(errno));
    ok = false;
  }
  if (!ok && !(inPlace && existed))
    wozRemove(out);
  if (tmp)
    wozFree(tmp);
  return ok;
}

bool Woz::writeFileAs(const char *filename, uint8_t type)
{
  switch (type) {
  case T_WOZ:
    return writeWozFile(filename, type);
  case T_DSK:
  case T_PO:
    return writeDskFile(filename, type);
  case T_NIB:
    return writeNibFile(filename);
  case T_HDV:
    return writeHdvFile(filename);
  case T_2MG:
    return writeTwoImgFile(filename);
  case T_D13:
    return writeD13File(filename);
  default:
    wozMsg(WOZ_MSG_ERROR, "Unknown disk type; unable to write");
    return false;
  }
}

bool Woz::writeWozFile(const char *filename, uint8_t subtype)
{
  int fdout = -1;

  fdout = wozOpen(filename, WOZ_OPEN_CREATE);
  if (fdout == -1) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: Unable to open output file: %s", strerror(errno));
    return false;
  }

  bool retval = writeWozFile(fdout, subtype);
  wozClose(fdout);

  return retval;
}

bool Woz::writeWozFile(int fdout, uint8_t subtype)
{
  int version = 2; // FIXME figure out from subtype
  bool retval = false;
  uint32_t tmp32; // scratch 32-bit value
  off_t curpos; // used in macros to dynamically tell what size the chunks are
  off_t crcPos = 0, endPos = 0;
  uint32_t crcDataSize = 0;
  uint8_t *crcData = NULL;
  (void)crcPos; (void)endPos; (void)crcDataSize; (void)crcData;


  if (version > 2 || !version) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: version must be 1 or 2");
    goto done;
  }
  wozSeek(fdout, 0, SEEK_SET);

  // A 13-sector disk we recognized from its tracks says so in INFO
  if (di.bootSectorFormat == 0 && isThirteenSectorDisk())
    di.bootSectorFormat = 2;
  
  // header
  if (version == 1) {
    tmp32 = 0x315A4F57;
  } else {
    tmp32 = 0x325A4F57;
  }
  if (!write32(fdout, tmp32)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write");
    goto done;
  }
  tmp32 = 0x0A0D0AFF;
  if (!write32(fdout, tmp32)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write");
    goto done;
  }

  // We'll come back and write the checksum later
  crcPos = wozSeek(fdout, 0, SEEK_CUR);
  tmp32 = 0;
  if (!write32(fdout, tmp32)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write");
    goto done;
  }

  PREP_SECTION(fdout, 0x4F464E49); // 'INFO'
  if (!writeInfoChunk(version, fdout)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write INFO chunk");
    goto done;
  }
  END_SECTION(fdout);

  PREP_SECTION(fdout, 0x50414D54); // 'TMAP'
  if (!writeTMAPChunk(version, fdout)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write TMAP chunk");
    goto done;
  }
  END_SECTION(fdout);

  PREP_SECTION(fdout, 0x534B5254); // 'TRKS'
  if (!writeTRKSChunk(version, fdout)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write TRKS chunk");
    goto done;
  }
  END_SECTION(fdout);

  // Write the metadata if we have any
  if (metaData) {
    PREP_SECTION(fdout, 0x4154454D); // 'META'
    if ((size_t)wozWrite(fdout, metaData, strlen(metaData)) != strlen(metaData)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write META chunk");
      goto done;
    }
    END_SECTION(fdout);
  }

  // FIXME: missing the WRIT chunk, if it exists

  // Fix up the checksum. Optional; the spec says it can be 0 meaning
  // "don't verify"
  if (!wozGetHost()->skipChecksum) {
    endPos = wozSeek(fdout, 0, SEEK_CUR);
    crcDataSize = endPos-crcPos-4;
    crcData = (uint8_t *)wozMalloc(crcDataSize);
    if (!crcData) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to malloc crc data chunk");
      goto done;
    }

    // Read the data in for checksumming
    if (wozSeek(fdout, crcPos+4, SEEK_SET) == -1) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to fseek to crcPos+4 (0x%llX)", (unsigned long long)(crcPos+4));
      goto done;
    }

    tmp32 = wozRead(fdout, crcData, crcDataSize);
    if (tmp32 != crcDataSize) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to read in data for checksum [read %lu, wanted %lu]", (unsigned long)tmp32, (unsigned long)crcDataSize);
      goto done;
    }

    tmp32 = compute_crc_32(crcData, crcDataSize);
    // Write it back out
    wozSeek(fdout, crcPos, SEEK_SET);
    if (!write32(fdout, tmp32)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write CRC");
      goto done;
    }
  }
  
  for (int i=0; i<160; i++) {
    tracks[i].dirty = false;
  }

  
  retval = true;

 done:
  if (crcData)
    wozFree(crcData);

  return retval;
}

bool Woz::writeDskFile(const char *filename, uint8_t subtype)
{
  int fdout = wozOpen(filename, WOZ_OPEN_CREATE);
  if (fdout == -1) {
    wozMsg(WOZ_MSG_ERROR, "Failed to open output file: %s", strerror(errno));
    return false;
  }

  bool retval = writeDskFile(fdout, subtype);
  wozClose(fdout);
  return retval;
}

bool Woz::writeDskFile(int fdout, uint8_t subtype)
{
  if (isSynchronized()) {
    wozMsg(WOZ_MSG_WARNING, "WARNING: disk image has synchronized tracks; it may not work as a DSK or NIB file.");
  }

  uint8_t sectorData[256*16];
  for (int phystrack=0; phystrack<35; phystrack++) {
    if (!decodeWozTrackToDsk(phystrack, subtype, sectorData)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to decode track %d; aborting", phystrack);
      return false;
    }
    ssize_t numWritten = wozWrite(fdout, sectorData, 256*16);
    if (numWritten != 256*16) {
      wozMsg(WOZ_MSG_ERROR, "Failed[2] to write to track; aborting: %s", strerror(errno));
      return false;
    }
  }

  for (int i=0; i<160; i++) {
    tracks[i].dirty = false;
  }

  return true;
}

bool Woz::writeNibFile(const char *filename)
{
  int fdout = wozOpen(filename, WOZ_OPEN_CREATE);
  if (fdout == -1) {
    wozMsg(WOZ_MSG_ERROR, "Failed to open output file: %s", strerror(errno));
    return false;
  }

  bool retval = writeNibFile(fdout);
  wozClose(fdout);
  return retval;
}

bool Woz::writeNibFile(int fdout)
{
  if (isSynchronized()) {
    wozMsg(WOZ_MSG_WARNING, "WARNING: disk image has synchronized tracks; it may not work as a DSK or NIB file.");
  }

  // A 13-sector track doesn't fit the 16-sector layout below, so its
  // nibbles go out as they are: copied from a .nib, or one revolution
  // read off the track the way the disk controller would, with sync
  // bytes after it to fill out the .nib track
  if (isThirteenSectorDisk()) {
    uint8_t nib[NIBTRACKSIZE];
    for (int phystrack=0; phystrack<35; phystrack++) {
      uint8_t dataTrack = quarterTrackMap[phystrack*4];
      uint32_t got = 0;
      if (dataTrack != 0xFF && !tracks[dataTrack].trackData &&
          !loadMissingTrackFromImage(dataTrack)) {
        wozMsg(WOZ_MSG_ERROR, "Failed to read track %d; aborting", phystrack);
        return false;
      }
      if (dataTrack == 0xFF || !tracks[dataTrack].trackData) {
        // An unformatted track
      } else if (imageType == T_NIB) {
        memcpy(nib, tracks[dataTrack].trackData, NIBTRACKSIZE);
        got = NIBTRACKSIZE;
      } else {
        got = readOneRevolution(dataTrack, nib, NIBTRACKSIZE);
      }
      memset(nib + got, 0xFF, NIBTRACKSIZE - got);
      if (wozWrite(fdout, nib, NIBTRACKSIZE) != NIBTRACKSIZE) {
        wozMsg(WOZ_MSG_ERROR, "Failed[1] to write track %d; aborting", phystrack);
        return false;
      }
    }
    for (int i=0; i<160; i++)
      tracks[i].dirty = false;
    return true;
  }

  nibSector nibData[16];
  for (int phystrack=0; phystrack<35; phystrack++) {
    if (!decodeWozTrackToNibFromDataTrack(quarterTrackMap[phystrack*4], nibData)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to decode track %d; aborting", phystrack);
      return false;
    }
    if (wozWrite(fdout, nibData, NIBTRACKSIZE) != NIBTRACKSIZE) {
      wozMsg(WOZ_MSG_ERROR, "Failed[1] to write track %d; aborting", phystrack);
      return false;
    }
  }

  for (int i=0; i<160; i++) {
    tracks[i].dirty = false;
  }

  return true;
}

uint32_t Woz::readOneRevolution(uint8_t datatrack, uint8_t *out, uint32_t max)
{
  // We go around twice and keep the bytes that finish on the second
  // pass: by then the latch has lined up with the sync bytes, wherever
  // bit 0 happened to fall. This reads the track's own bits, not through
  // the head's cursor, so an emulator's place on the disk isn't moved.
  const trackInfo *t = &tracks[datatrack];
  if (!t->trackData || !t->bitCount)
    return 0;
  uint8_t latch = 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < 2 * t->bitCount; i++) {
    uint32_t b = i % t->bitCount;
    latch = (uint8_t)((latch << 1) | ((t->trackData[b >> 3] >> (7 - (b & 7))) & 1));
    if (latch & 0x80) {
      if (i >= t->bitCount && n < max)
        out[n++] = latch;
      latch = 0;
    }
  }
  return n;
}

bool Woz::writeD13File(const char *filename)
{
  if (!isThirteenSectorDisk()) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: a .d13 holds a 13-sector (DOS 3.2) disk, and this one "
           "is 16-sector; save it as .dsk, .po, .woz, .nib or .2mg");
    return false;
  }
  int fdout = wozOpen(filename, WOZ_OPEN_CREATE);
  if (fdout == -1) {
    wozMsg(WOZ_MSG_ERROR, "Failed to open output file: %s", strerror(errno));
    return false;
  }
  bool ok = true;
  uint8_t sectorData[256*13];
  for (int phystrack=0; ok && phystrack<35; phystrack++) {
    if (!decodeWozTrack13ToDsk(phystrack, sectorData)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to decode track %d; aborting", phystrack);
      ok = false;
    } else if (wozWrite(fdout, sectorData, sizeof(sectorData)) != (long)sizeof(sectorData)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to write track %d; aborting", phystrack);
      ok = false;
    }
  }
  ok = (wozClose(fdout) == 0) && ok;
  if (ok) {
    for (int i=0; i<160; i++)
      tracks[i].dirty = false;
  }
  return ok;
}

void Woz::_initInfo()
{
  di.version = 2;
  di.diskType = 1;
  di.writeProtected = 0;
  di.synchronized = 0;
  di.cleaned = 0;
  snprintf(di.creator, sizeof(di.creator), "%.32s", VERSION_STRING);
  di.diskSides = 1;
  di.bootSectorFormat = 0;
  di.optimalBitTiming = 32;
  di.compatHardware = 0;
  di.requiredRam = 0;
  di.largestTrack = 13;

  // reset all the track data
  for (int i=0; i<160; i++) {
    memset(&tracks[i], 0, sizeof(trackInfo));
  }
  // Construct a default quarter-track mapping
  for (int i=0; i<140; i++) {
    if ((i+1)/4 < 35) {
      quarterTrackMap[i] = ((i-2) % 4 == 0) ? 0xFF : ((i+1)/4);
    } else {
      quarterTrackMap[i] = 0xFF;
    }
  }
}

// Only used if we didn't preload a data track; the load we perform
// differs based on the image type we originally read from
bool Woz::loadMissingTrackFromImage(uint8_t datatrack)
{
  // If we're going to malloc a new one, then find all the other ones
  // that might be malloc'd and purge them if we're
  // autoFlushTrackData==true (trying to limit memory use)
  if (autoFlushTrackData == true) {
    for (int i=0; i<160; i++) {
      // Don't flush any tracks that are dirty
      if (tracks[i].trackData && !tracks[i].dirty) {
        wozFree(tracks[i].trackData);
        tracks[i].trackData = NULL;
      }
    }
  }

  // Based on the source image type, load the data track we're looking for
  if (imageType == T_WOZ) {
    // If the source was WOZ, just load the datatrack directly
    return readWozDataTrack(datatrack);
  } else if (imageType == T_PO ||
	     imageType == T_DSK ||
	     imageType == T_D13) {
    // If the source was a DSK file, then the datatrack was mapped directly
    // from the physical track
    if (datatrack >= 35) {
      // There are only 35 tracks; the remainder are blank.
      tracks[datatrack].trackData = NULL;
      return true;
    }
    
    uint8_t phystrack = datatrack; // used for clarity of which kind of track we mean, below
    
    static uint8_t sectorData[256*16];

    const uint32_t trackBytes = 256 * (imageType == T_D13 ? 13 : 16);
    wozSeek(fd, dataOffset + trackBytes*phystrack, SEEK_SET);

    if (wozRead(fd, sectorData, trackBytes) != (long)trackBytes) {
      wozMsg(WOZ_MSG_ERROR, "Failed to read sector");
      return false;
    }

    tracks[datatrack].trackData = (uint8_t *)wozCalloc(NIBTRACKSIZE, 1);
    if (!tracks[datatrack].trackData) {
      wozMsg(WOZ_MSG_ERROR, "Failed to malloc track data");
      return false;
    }
    tracks[datatrack].startingBlock = STARTBLOCK + 13*phystrack;
    tracks[datatrack].blockCount = 13;
    uint32_t sizeInBits = (imageType == T_D13) ?
      nibblizeTrack13(tracks[datatrack].trackData, sectorData, phystrack, sectorVolume) :
      nibblizeTrack(tracks[datatrack].trackData, sectorData, imageType, phystrack, sectorVolume);
    tracks[datatrack].bitCount = sizeInBits; // ... reality.

    return true;
  }
  else if (imageType == T_NIB) {
    if (datatrack >= 35) {
      // There are only 35 tracks; the remainder are blank.
      tracks[datatrack].trackData = NULL;
      return true;
    }
    // If the source was a NIB file, then the datatrack is directly
    // mapped 1:1 to the physical track
    uint8_t phystrack = datatrack; // used for clarity of which kind of track we mean, below
    tracks[datatrack].trackData = (uint8_t *)wozMalloc(NIBTRACKSIZE);
    if (!tracks[datatrack].trackData) {
      wozMsg(WOZ_MSG_ERROR, "Failed to malloc track data");
      return false;
    }

    wozSeek(fd, dataOffset + NIBTRACKSIZE * phystrack, SEEK_SET);
    wozRead(fd, tracks[datatrack].trackData, NIBTRACKSIZE);
      // FIXME: no error checking
    
    tracks[datatrack].startingBlock = STARTBLOCK + 13*phystrack;
    tracks[datatrack].blockCount = 13;
    tracks[datatrack].bitCount = NIBTRACKSIZE*8;
    
    return true;
  }
  
  if (imageType == T_HDV) {
    // HDVs have no floppy tracks, and nothing in the HDV path calls
    // this routine for anything useful - silently fail so probes and
    // stray `decodeWozTrackToDsk` callers don't spam errors.
    return false;
  }
  wozMsg(WOZ_MSG_ERROR, "ERROR: don't know how we reached this point");
  return false;
}

bool Woz::readDskFile(const char *filename, bool preloadTracks, uint8_t subtype)
{
  bool retval = false;
  autoFlushTrackData = !preloadTracks;
  imageType = subtype;
  const uint8_t sectors = (subtype == T_D13) ? 13 : 16;

  if (fd != -1) wozClose(fd);
  fd = wozOpen(filename, WOZ_OPEN_READWRITE);
  if (fd == -1) {
    wozMsg(WOZ_MSG_ERROR, "Unable to open input file: %s", strerror(errno));
    goto done;
  }

  _initInfo();
  if (subtype == T_D13)
    di.bootSectorFormat = 2;

  // Now read in the 35 tracks of data from the DSK file and convert them to NIB
  if (preloadTracks) {
    if (wozSeek(fd, dataOffset, SEEK_SET) == -1) {
      wozMsg(WOZ_MSG_ERROR, "Unable to seek in input file: %s", strerror(errno));
      goto done;
    }
    uint8_t sectorData[256*16];
    for (int phystrack=0; phystrack<35; phystrack++) {
      uint32_t bytesRead = wozRead(fd, sectorData, 256*sectors);
      if (bytesRead != 256u*sectors) {
	wozMsg(WOZ_MSG_ERROR, "Failed to read DSK data; got %lu bytes, wanted %d", (unsigned long)bytesRead, 256*sectors);
	goto done;
      }
      uint8_t datatrack = quarterTrackMap[phystrack*4];
      tracks[datatrack].trackData = (uint8_t *)wozCalloc(NIBTRACKSIZE, 1);
      if (!tracks[datatrack].trackData) {
	wozMsg(WOZ_MSG_ERROR, "Failed to malloc track data");
	goto done;
      }
      tracks[datatrack].startingBlock = STARTBLOCK + 13*datatrack;
      tracks[datatrack].blockCount = 13;
      uint32_t sizeInBits = (subtype == T_D13) ?
        nibblizeTrack13(tracks[datatrack].trackData, sectorData, phystrack, sectorVolume) :
        nibblizeTrack(tracks[datatrack].trackData, sectorData, subtype, phystrack, sectorVolume);
      tracks[datatrack].bitCount = sizeInBits; // ... reality.
    }
  }
  
  retval = true;

 done:
  if (preloadTracks && fd != -1 && !wozGetHost()->keepFilesOpen) {
    wozClose(fd);
    fd = -1;
  }
  return retval;
}

bool Woz::readHdvFile(const char *filename, uint32_t offset, uint32_t length)
{
  // ProDOS hard-disk images are raw block-sequential ProDOS data, no
  // floppy-track structure. We slurp the whole file into a heap buffer
  // that higher layers (ProdosSpector) read blocks out of directly.
  imageType = T_HDV;
  autoFlushTrackData = false;

  if (fd != -1) wozClose(fd);
  fd = wozOpen(filename, WOZ_OPEN_READ);
  if (fd == -1) {
    wozMsg(WOZ_MSG_ERROR, "Unable to open input file: %s", strerror(errno));
    return false;
  }
  long fileSize = wozSeek(fd, 0, SEEK_END);
  if (fileSize < 0 || wozSeek(fd, 0, SEEK_SET) != 0) {
    wozMsg(WOZ_MSG_ERROR, "Unable to find the size of the input file: %s", strerror(errno));
    wozClose(fd); fd = -1;
    return false;
  }
  // A 2IMG file's blocks are a slice of it; otherwise it's the whole file
  if (length) {
    if ((uint64_t)offset + length > (uint64_t)fileSize ||
        wozSeek(fd, offset, SEEK_SET) == -1) {
      wozMsg(WOZ_MSG_ERROR, "Image '%s' is shorter than its header says", filename);
      wozClose(fd); fd = -1;
      return false;
    }
    fileSize = length;
  }
  if (fileSize == 0 || (fileSize % 512) != 0) {
    wozMsg(WOZ_MSG_ERROR, "HDV image '%s' has a bad size (%ld bytes)",
            filename, fileSize);
    wozClose(fd); fd = -1;
    return false;
  }

  if (hdvData) { wozFree(hdvData); hdvData = NULL; }
  hdvDirty = false;
  hdvByteSize = (uint32_t)fileSize;
  hdvData = (uint8_t *)wozMalloc(hdvByteSize);
  if (!hdvData) {
    wozMsg(WOZ_MSG_ERROR, "Out of memory loading HDV (%u bytes)", (unsigned)hdvByteSize);
    wozClose(fd); fd = -1;
    return false;
  }
  ssize_t got = wozRead(fd, hdvData, hdvByteSize);
  wozClose(fd); fd = -1;
  if (got != (ssize_t)hdvByteSize) {
    wozMsg(WOZ_MSG_ERROR, "Short read on HDV (%zd of %u bytes)",
            got, (unsigned)hdvByteSize);
    wozFree(hdvData); hdvData = NULL;
    return false;
  }

  _initInfo(); // sensible defaults - info fields don't really apply to HDV
  di.diskType = 1; // 5.25 isn't meaningful here but keeps other code happy

  return true;
}

bool Woz::truncateHdv(uint32_t newByteSize)
{
  if (imageType != T_HDV || !hdvData) {
    wozMsg(WOZ_MSG_ERROR, "Not a hard-drive image; can't truncate it");
    return false;
  }
  if (newByteSize > hdvByteSize) {
    wozMsg(WOZ_MSG_ERROR, "Can't grow an image from %u to %u bytes",
            (unsigned)hdvByteSize, (unsigned)newByteSize);
    return false;
  }
  if (!newByteSize || (newByteSize % 512)) {
    wozMsg(WOZ_MSG_ERROR, "Image size must be a nonzero multiple of 512 bytes");
    return false;
  }
  // Hand the tail back if the allocator will take it. Keeping the bigger
  // buffer is harmless either way: hdvByteSize is what gets written.
  uint8_t *smaller = (uint8_t *)wozRealloc(hdvData, newByteSize);
  if (smaller)
    hdvData = smaller;
  hdvByteSize = newByteSize;
  hdvDirty = true;
  return true;
}

bool Woz::writeHdvFile(const char *filename)
{
  int fdout = wozOpen(filename, WOZ_OPEN_CREATE);
  if (fdout == -1) {
    wozMsg(WOZ_MSG_ERROR, "Unable to open output HDV file: %s", strerror(errno));
    return false;
  }
  bool retval = writeHdvFile(fdout);
  wozClose(fdout);
  return retval;
}

bool Woz::writeHdvFile(int fdout)
{
  if (!hdvData || !hdvByteSize) {
    wozMsg(WOZ_MSG_ERROR, "No HDV buffer to write");
    return false;
  }
  ssize_t wrote = wozWrite(fdout, hdvData, hdvByteSize);
  if (wrote != (ssize_t)hdvByteSize) {
    wozMsg(WOZ_MSG_ERROR, "Short write on HDV (%zd of %u bytes)",
            wrote, (unsigned)hdvByteSize);
    return false;
  }
  hdvDirty = false;
  return true;
}

// ---- 2IMG (.2mg) ----
//
// The header is 64 bytes, all little-endian:
//
//   +$00  "2IMG"            +$18  offset to the data
//   +$04  creator code      +$1c  length of the data
//   +$08  header length     +$20  offset to the comment
//   +$0a  version (1)       +$24  length of the comment
//   +$0c  data format       +$28  offset to the creator data
//   +$10  flags             +$2c  length of the creator data
//   +$14  block count       +$30  16 reserved bytes
//
// Format 0 is DOS order, 1 ProDOS order and 2 nibbles. In the flags, bit
// 31 is write protect, and bit 8 says the low byte is the volume number
// for the sector address fields.

#define TWOIMG_HEADER_SIZE 64
#define TWOIMG_LOCKED      0x80000000
#define TWOIMG_HAS_VOLUME  0x00000100

static uint32_t le32(const uint8_t *p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void putLe32(uint8_t *p, uint32_t v)
{
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void buildTwoImgHeader(uint8_t h[TWOIMG_HEADER_SIZE], const char creator[4],
                              uint8_t format, uint32_t flags, uint32_t dataLen,
                              uint32_t commentLen, uint32_t creatorDataLen)
{
  // The pieces go header, data, comment, creator data, with nothing
  // between them
  uint32_t commentOff = TWOIMG_HEADER_SIZE + dataLen;
  uint32_t creatorOff = commentOff + commentLen;
  memset(h, 0, TWOIMG_HEADER_SIZE);
  memcpy(h, "2IMG", 4);
  memcpy(h + 4, creator, 4);
  h[8] = TWOIMG_HEADER_SIZE;
  h[10] = 1;
  putLe32(h + 0x0c, format);
  putLe32(h + 0x10, flags);
  // Only ProDOS order needs the block count, but other programs fill it
  // in for the others too
  putLe32(h + 0x14, format == 2 ? 280 : dataLen / 512);
  putLe32(h + 0x18, TWOIMG_HEADER_SIZE);
  putLe32(h + 0x1c, dataLen);
  if (commentLen) {
    putLe32(h + 0x20, commentOff);
    putLe32(h + 0x24, commentLen);
  }
  if (creatorDataLen) {
    putLe32(h + 0x28, creatorOff);
    putLe32(h + 0x2c, creatorDataLen);
  }
}

void Woz::getImageCreator(char *out, size_t len) const
{
  if (!len) return;
  out[0] = '\0';
  if (imageType == T_WOZ) {
    snprintf(out, len, "%.32s", di.creator);
  } else if (twoImg) {
    snprintf(out, len, "%.4s", twoImgCreator);
  } else {
    return;
  }
  // Both are padded with spaces
  size_t n = strlen(out);
  while (n && out[n-1] == ' ')
    out[--n] = '\0';
}

bool Woz::isTwoImgName(const char *filename)
{
  const char *p = strrchr(filename, '.');
  return p && (strcasecmp(p, ".2mg") == 0 || strcasecmp(p, ".2img") == 0);
}

void Woz::clearTwoImg()
{
  if (twoImgComment) wozFree(twoImgComment);
  if (twoImgCreatorData) wozFree(twoImgCreatorData);
  twoImgComment = twoImgCreatorData = NULL;
  twoImgCommentLen = twoImgCreatorDataLen = 0;
  twoImg = false;
  twoImgFormat = 0;
  twoImgFlags = 0;
  memset(twoImgCreator, 0, sizeof(twoImgCreator));
  dataOffset = 0;
  sectorVolume = 254;
}

// Read `len` bytes at `off` into a new buffer, for the comment and the
// creator data. A range that runs off the end of the file is dropped with
// a warning rather than refusing the image over it.
static uint8_t *readTwoImgChunk(int fd, long fileSize, uint32_t off, uint32_t len,
                                const char *what)
{
  if (!len)
    return NULL;
  if ((uint64_t)off + len > (uint64_t)fileSize) {
    wozMsg(WOZ_MSG_WARNING, "WARNING: the 2IMG %s runs past the end of the file; ignoring it", what);
    return NULL;
  }
  uint8_t *buf = (uint8_t *)wozMalloc(len);
  if (!buf || wozSeek(fd, off, SEEK_SET) == -1 || wozRead(fd, buf, len) != (long)len) {
    wozMsg(WOZ_MSG_WARNING, "WARNING: couldn't read the 2IMG %s; ignoring it", what);
    if (buf) wozFree(buf);
    return NULL;
  }
  return buf;
}

bool Woz::readTwoImgFile(const char *filename, bool preloadTracks)
{
  int hfd = wozOpen(filename, WOZ_OPEN_READ);
  if (hfd == -1) {
    wozMsg(WOZ_MSG_ERROR, "Unable to open input file: %s", strerror(errno));
    return false;
  }
  uint8_t h[TWOIMG_HEADER_SIZE];
  long fileSize = wozSeek(hfd, 0, SEEK_END);
  if (fileSize < TWOIMG_HEADER_SIZE || wozSeek(hfd, 0, SEEK_SET) != 0 ||
      wozRead(hfd, h, sizeof(h)) != (long)sizeof(h) || memcmp(h, "2IMG", 4)) {
    wozMsg(WOZ_MSG_ERROR, "'%s' isn't a 2IMG image (it has no 2IMG header)", filename);
    wozClose(hfd);
    return false;
  }
  uint32_t format = le32(h + 0x0c);
  uint32_t flags = le32(h + 0x10);
  uint32_t blocks = le32(h + 0x14);
  uint32_t off = le32(h + 0x18);
  uint32_t len = le32(h + 0x1c);
  // Some images have the block count but no data length
  if (format == 1 && len == 0)
    len = blocks * 512;
  if (off < 48 || len == 0 || (uint64_t)off + len > (uint64_t)fileSize) {
    wozMsg(WOZ_MSG_ERROR, "'%s' has a bad 2IMG header (data at %u, %u bytes, in a %ld-byte file)",
           filename, (unsigned)off, (unsigned)len, fileSize);
    wozClose(hfd);
    return false;
  }

  uint8_t inner;
  if (format == 0 && len == 35 * 16 * 256) {
    inner = T_DSK;
  } else if (format == 1 && len == 35 * 16 * 256) {
    inner = T_PO;
  } else if (format == 1 && (len % 512) == 0) {
    inner = T_HDV;
  } else if (format == 2 && len == 35 * NIBTRACKSIZE) {
    inner = T_NIB;
  } else {
    wozMsg(WOZ_MSG_ERROR, "'%s' holds %u bytes in 2IMG format %u, which isn't a disk "
           "we know (DOS order is 140K, ProDOS order whole blocks, nibbles 35 tracks)",
           filename, (unsigned)len, (unsigned)format);
    wozClose(hfd);
    return false;
  }

  twoImgCommentLen = le32(h + 0x24);
  twoImgComment = readTwoImgChunk(hfd, fileSize, le32(h + 0x20), twoImgCommentLen, "comment");
  if (!twoImgComment) twoImgCommentLen = 0;
  twoImgCreatorDataLen = le32(h + 0x2c);
  twoImgCreatorData = readTwoImgChunk(hfd, fileSize, le32(h + 0x28), twoImgCreatorDataLen, "creator data");
  if (!twoImgCreatorData) twoImgCreatorDataLen = 0;
  wozClose(hfd);

  // The usual loaders do the rest, starting dataOffset into the file
  dataOffset = off;
  if (flags & TWOIMG_HAS_VOLUME)
    sectorVolume = flags & 0xFF;
  bool ok;
  if (inner == T_HDV)
    ok = readHdvFile(filename, off, len);
  else if (inner == T_NIB)
    ok = readNibFile(filename, preloadTracks);
  else
    ok = readDskFile(filename, preloadTracks, inner);
  if (!ok) {
    clearTwoImg();
    return false;
  }

  twoImg = true;
  twoImgFormat = format;
  twoImgFlags = flags;
  memcpy(twoImgCreator, h + 4, 4);
  // The loaders reset the disk info, so this has to come after them
  if (flags & TWOIMG_LOCKED)
    di.writeProtected = 1;
  return true;
}

bool Woz::writeTwoImgFile(const char *filename)
{
  // What goes inside: the layout we read, if this came from a 2IMG, or
  // else the closest one to the image. Anything that's blocks, or a .woz,
  // is ProDOS order, which is what most programs expect.
  uint8_t format;
  if (imageType == T_HDV)
    format = 1;
  else if (twoImg)
    format = twoImgFormat;
  else if (imageType == T_DSK)
    format = 0;
  else if (imageType == T_NIB)
    format = 2;
  else
    format = 1;
  // There's no 13-sector sector layout, so a DOS 3.2 disk goes as nibbles
  if (imageType != T_HDV && isThirteenSectorDisk())
    format = 2;

  int fdout = wozOpen(filename, WOZ_OPEN_CREATE);
  if (fdout == -1) {
    wozMsg(WOZ_MSG_ERROR, "Unable to open output file: %s", strerror(errno));
    return false;
  }
  // Room for the header, which is filled in once we know the sizes
  uint8_t h[TWOIMG_HEADER_SIZE];
  memset(h, 0, sizeof(h));
  bool ok = (wozWrite(fdout, h, sizeof(h)) == (long)sizeof(h));
  if (ok) {
    if (format == 0)
      ok = writeDskFile(fdout, T_DSK);
    else if (format == 2)
      ok = writeNibFile(fdout);
    else if (imageType == T_HDV)
      ok = writeHdvFile(fdout);
    else
      ok = writeDskFile(fdout, T_PO);
  }
  long dataEnd = ok ? wozSeek(fdout, 0, SEEK_CUR) : -1;
  ok = ok && dataEnd > TWOIMG_HEADER_SIZE;
  if (ok && twoImgCommentLen)
    ok = (wozWrite(fdout, twoImgComment, twoImgCommentLen) == (long)twoImgCommentLen);
  if (ok && twoImgCreatorDataLen)
    ok = (wozWrite(fdout, twoImgCreatorData, twoImgCreatorDataLen) == (long)twoImgCreatorDataLen);
  if (ok) {
    // Keep the creator code and its data from the image we read; a new
    // file is ours. The volume number carries over too, and the write
    // protect flag follows the disk's (so a locked .woz stays locked).
    uint32_t flags = (twoImg ? twoImgFlags & (TWOIMG_HAS_VOLUME | 0xFF) : 0) |
      (di.writeProtected ? TWOIMG_LOCKED : 0);
    buildTwoImgHeader(h, twoImg ? twoImgCreator : "WOZZ", format, flags,
                      (uint32_t)(dataEnd - TWOIMG_HEADER_SIZE),
                      twoImgCommentLen, twoImgCreatorDataLen);
    ok = wozSeek(fdout, 0, SEEK_SET) == 0 && wozWrite(fdout, h, sizeof(h)) == (long)sizeof(h);
  }
  ok = (wozClose(fdout) == 0) && ok;
  if (!ok)
    wozMsg(WOZ_MSG_ERROR, "Failed to write 2IMG file '%s'", filename);
  return ok;
}

bool Woz::writeBlankImageFile(const char *filename, const uint8_t *data,
                              uint32_t size, bool dosOrder)
{
  int fdout = wozOpen(filename, WOZ_OPEN_CREATE);
  if (fdout == -1)
    return false;
  bool ok = true;
  if (isTwoImgName(filename)) {
    uint8_t h[TWOIMG_HEADER_SIZE];
    buildTwoImgHeader(h, "WOZZ", dosOrder ? 0 : 1, 0, size, 0, 0);
    ok = (wozWrite(fdout, h, sizeof(h)) == (long)sizeof(h));
  }
  ok = ok && (wozWrite(fdout, data, size) == (long)size);
  ok = (wozClose(fdout) == 0) && ok;
  // The callers made sure nothing was there, so a failure leaves nothing
  if (!ok)
    wozRemove(filename);
  return ok;
}

bool Woz::readNibFile(const char *filename, bool preloadTracks)
{
  autoFlushTrackData = !preloadTracks;
  imageType = T_NIB;

  if (fd != -1) wozClose(fd);
  fd = wozOpen(filename, WOZ_OPEN_READWRITE);
  if (fd == -1) {
    wozMsg(WOZ_MSG_ERROR, "Unable to open input file: %s", strerror(errno));
    return false;
  }
  
  _initInfo();

  // Now read in the 35 tracks of data from the nib file
  if (preloadTracks) {
    if (wozSeek(fd, dataOffset, SEEK_SET) == -1) {
      wozMsg(WOZ_MSG_ERROR, "Unable to seek in input file: %s", strerror(errno));
      return false;
    }
    nibSector nibData[16];
    for (int phystrack=0; phystrack<35; phystrack++) {
      uint32_t bytesRead = wozRead(fd, nibData, NIBTRACKSIZE);
      if (bytesRead != NIBTRACKSIZE) {
	wozMsg(WOZ_MSG_ERROR, "Failed to read NIB data; got %lu bytes, wanted %d", (unsigned long)bytesRead, NIBTRACKSIZE);
	return false;
      }
      uint8_t datatrack = quarterTrackMap[phystrack * 4];
      tracks[datatrack].trackData = (uint8_t *)wozCalloc(NIBTRACKSIZE, 1);
      if (!tracks[datatrack].trackData) {
	wozMsg(WOZ_MSG_ERROR, "Failed to malloc track data");
	return false;
      }
      
      memcpy(tracks[datatrack].trackData, nibData, NIBTRACKSIZE);
      tracks[datatrack].startingBlock = STARTBLOCK + 13*phystrack;
      tracks[datatrack].blockCount = 13;
      tracks[datatrack].bitCount = NIBTRACKSIZE*8;
    }
  }
  if (preloadTracks && fd != -1 && !wozGetHost()->keepFilesOpen) {
    wozClose(fd);
    fd = -1;
  }
  return true;
}

bool Woz::readWozFile(const char *filename, bool preloadTracks)
{
  imageType = T_WOZ;
  autoFlushTrackData = !preloadTracks;

  if (fd != -1) wozClose(fd);
  fd = wozOpen(filename, WOZ_OPEN_READWRITE);
  if (fd == -1) {
    wozMsg(WOZ_MSG_ERROR, "Unable to open input file: %s", strerror(errno));
    return false;
  }

  // Header
  uint32_t h;
  read32(fd, &h);
  if (h == 0x325A4F57 || h == 0x315A4F57) {
    if (verbose) {
      wozMsg(WOZ_MSG_DEBUG, "WOZ%c disk image", (int)((h & 0xFF000000)>>24));
    }
  } else {
    wozMsg(WOZ_MSG_ERROR, "Unknown disk image type; can't continue");
    if (preloadTracks && fd != -1)
      wozClose(fd);
    return false;
  }

  uint32_t tmp;
  if (!read32(fd, &tmp)) {
    wozMsg(WOZ_MSG_ERROR, "Read failure");
    if (preloadTracks && fd != -1)
      wozClose(fd);
    return false;
  }
  if (tmp != 0x0A0D0AFF) {
    wozMsg(WOZ_MSG_ERROR, "WOZ header failure; exiting");
    if (preloadTracks && fd != -1)
      wozClose(fd);
    return false;
  }
  uint32_t crc32;
  read32(fd, &crc32);
  // If CRC is set, then check it
  if (crc32) {
    // FIXME: check CRC
    if (verbose) {
      wozMsg(WOZ_MSG_DEBUG, "Disk crc32 should be 0x%lX", (unsigned long)crc32);
    }
  }
  
  uint32_t fpos = 12;
  uint8_t haveData = 0;

#define cINFO 1
#define cTMAP 2
#define cTRKS 4

  while (1) {
    if (wozSeek(fd, fpos, SEEK_SET) == -1) {
      break;
    }

    uint32_t chunkType;
    if (!read32(fd, &chunkType)) {
      break;
    }
    uint32_t chunkDataSize;
    read32(fd, &chunkDataSize);
    if ((int32_t)chunkDataSize < 0) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: data size < 0?");
      if (preloadTracks && fd != -1)
	wozClose(fd);
      return false;
    }

    bool isOk;

    switch (chunkType) {
    case 0x4F464E49: // 'INFO'
      if (verbose) {
	wozMsg(WOZ_MSG_DEBUG, "Reading INFO chunk starting at byte 0x%llX",
	       (unsigned long long)wozSeek(fd, 0, SEEK_CUR));
      }
      isOk = parseInfoChunk(chunkDataSize);
      haveData |= cINFO;
      break;
    case 0x50414D54: // 'TMAP'
      if (verbose) {
	wozMsg(WOZ_MSG_DEBUG, "Reading TMAP chunk starting at byte 0x%llX",
	       (unsigned long long)wozSeek(fd, 0, SEEK_CUR));
      }
      isOk = parseTMAPChunk(chunkDataSize);
      haveData |= cTMAP;
      break;
    case 0x534B5254: // 'TRKS'
      if (verbose) {
	wozMsg(WOZ_MSG_DEBUG, "Reading TRKS chunk starting at byte 0x%llX",
	       (unsigned long long) wozSeek(fd, 0, SEEK_CUR));
      }
      isOk = parseTRKSChunk(chunkDataSize);
      haveData |= cTRKS;
      break;
    case 0x4154454D: // 'META'
      if (verbose) {
	wozMsg(WOZ_MSG_DEBUG, "Reading META chunk starting at byte 0x%llX",
	       (unsigned long long) wozSeek(fd, 0, SEEK_CUR));
      }	  
      isOk = parseMetaChunk(chunkDataSize);
      break;
    default:
      // The spec requires unknown chunks to be skipped for forward
      // compatibility. fpos is advanced past the chunk at the bottom
      // of the loop, so we just mark this chunk as "handled ok".
      if (verbose) {
        wozMsg(WOZ_MSG_DEBUG, "Skipping unknown chunk type 0x%lX (%lu bytes)",
               (unsigned long)chunkType, (unsigned long)chunkDataSize);
      }
      isOk = true;
      break;
    }

    if (!isOk) {
      wozMsg(WOZ_MSG_ERROR, "Chunk parsing [0x%lX] failed; exiting", (unsigned long)chunkType);
      if (preloadTracks && fd != -1)
	wozClose(fd);
      return false;
    }
    fpos += chunkDataSize + 8; // 8 bytes for the ChunkID and the ChunkSize
  }

  if (haveData != 0x07) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: missing one or more critical sections");
    return false;
  }

  // For a Woz file, we need to read *every* quarter-track; and if we've
  // already got the target track's data, we don't need to re-read it.
  // And if we're not preloading the tracks, then we'll wind up loading
  // them on demand later.
  if (preloadTracks) {
    for (int i=0; i<160; i++) {
      if (!readWozDataTrack(i)) {
	wozMsg(WOZ_MSG_ERROR, "Failed to read Woz datatrack %d", i);
	if (fd != -1) {
	  wozClose(fd);
	  fd = -1;
	}
	return false;
      }
    }
  }

  if (preloadTracks && fd != -1 && !wozGetHost()->keepFilesOpen) {
    wozClose(fd);
    fd = -1;
  }
  return true;
}

bool Woz::readFile(const char *filename, bool preloadTracks, uint8_t forceType)
{
  clearTwoImg();
  thirteenSector = -1;

  if (forceType == T_AUTO) {
    // Try to determine type from the file extension
    const char *p = strrchr(filename, '.');
    if (!p) {
      wozMsg(WOZ_MSG_ERROR, "Unable to determine file type of '%s'", filename);
      return false;
    }
    if (strcasecmp(p, ".woz") == 0) {
      forceType = T_WOZ;
    } else if (strcasecmp(p, ".dsk") == 0 ||
	       strcasecmp(p, ".do") == 0) {
      forceType = T_DSK;
    } else if (strcasecmp(p, ".po")  == 0) {
      forceType = T_PO;
    } else if (strcasecmp(p, ".nib") == 0) {
      forceType = T_NIB;
    } else if (strcasecmp(p, ".hdv") == 0 ||
               strcasecmp(p, ".img") == 0) {
      forceType = T_HDV;
    } else if (isTwoImgName(filename)) {
      forceType = T_2MG;
    } else if (strcasecmp(p, ".d13") == 0) {
      forceType = T_D13;
    } else {
      wozMsg(WOZ_MSG_ERROR, "Unable to determine file type of '%s'", filename);
      return false;
    }
  }

  // A .po or .dsk file larger than one floppy (> 140 KB) can't fit the
  // 35-track geometry the other paths assume; promote it to T_HDV so the
  // raw-block path is used instead.
  if (forceType == T_PO || forceType == T_DSK) {
    long fileSize = sizeOfFile(filename);
    if (fileSize > 35 * 16 * 256) {
      if (verbose) {
        wozMsg(WOZ_MSG_DEBUG, "Input file is larger than a floppy (%ld bytes); "
               "treating as ProDOS hard-disk image.",
               fileSize);
      }
      forceType = T_HDV;
      hdvPromotedFromFloppyExt = true;
    }
  }

  switch (forceType) {
  case T_WOZ:
    return readWozFile(filename, preloadTracks);
  case T_DSK:
  case T_PO:
    return readDskFile(filename, preloadTracks, forceType);
  case T_NIB:
    return readNibFile(filename, preloadTracks);
  case T_HDV:
    return readHdvFile(filename);
  case T_2MG:
    return readTwoImgFile(filename, preloadTracks);
  case T_D13:
    return readDskFile(filename, preloadTracks, T_D13);
  default:
    wozMsg(WOZ_MSG_ERROR, "Unknown disk type; unable to read");
    return false;
  }
}

bool Woz::parseTRKSChunk(uint32_t chunkSize)
{
  // WOZ 2.x (versions 2 and 3) use the same block-based TRKS layout;
  // WOZ 1 uses the older packed-6656-byte format.
  if (di.version >= 2) {
    for (int i=0; i<160; i++) {
      if (!read16(fd, &tracks[i].startingBlock))
	return false;
      if (!read16(fd, &tracks[i].blockCount))
	return false;
      if (!read32(fd, &tracks[i].bitCount))
	return false;
      tracks[i].startingByte = 0; // v1-specific
    }
    return true;
  }

  // V1 parsing
  uint32_t ptr = 0;
  uint8_t trackNumber = 0;
  while (ptr < chunkSize) {
    tracks[trackNumber].startingByte = trackNumber * 6656 + 256;
    tracks[trackNumber].startingBlock = 0; // v2-specific
    tracks[trackNumber].blockCount = 13;
    wozSeek(fd, (trackNumber * 6656 + 256) + 6648, SEEK_SET);
    uint16_t numBits;
    if (!read16(fd, &numBits)) {
      return false;
    }
    if (verbose) {
      wozMsg(WOZ_MSG_DEBUG, "Track %d: read %d bits", trackNumber, numBits);
    }
    if (numBits > 6656 * 8) {
      wozMsg(WOZ_MSG_WARNING, "WARNING: track %d looks like it's too long (%d bits > 6656 bytes)?", trackNumber, numBits);
    }
    tracks[trackNumber].bitCount = numBits;
    ptr += 6656;
    trackNumber++;
  }

  return true;
}

bool Woz::parseTMAPChunk(uint32_t chunkSize)
{
  if (chunkSize != 0xa0) {
    wozMsg(WOZ_MSG_ERROR, "TMAP chunk is the wrong size; aborting");
    return false;
  }

  for (int i=0; i<40*4; i++) {
    if (!read8(fd, (uint8_t *)&quarterTrackMap[i]))
      return false;
    chunkSize--;
  }
  if (verbose){
    wozMsg(WOZ_MSG_DEBUG, "Read quarter-track map:");
    for (int i=0; i<140; i+=4) {
      wozMsg(WOZ_MSG_DEBUG, "%2d     %3d => %3d     %3d => %3d     %3d => %3d     %3d => %3d",
	     i/4,
	     i, quarterTrackMap[i],
	     i+1, quarterTrackMap[i+1],
	     i+2, quarterTrackMap[i+2],
	     i+3, quarterTrackMap[i+3]);
    }
  }

  return true;
}

// return true if successful
bool Woz::parseInfoChunk(uint32_t chunkSize)
{
  if (chunkSize != 60) {
    wozMsg(WOZ_MSG_ERROR, "INFO chunk size is not 60; aborting");
    return false;
  }

  if (!read8(fd, &di.version))
    return false;
  // Per the spec: "use >= when checking the INFO Version field. The INFO
  // chunk will always be upgraded in a safe way for older consumers."
  // So we don't reject unknown future versions - we just don't try to
  // parse fields that weren't defined yet in the version we know about.
  if (di.version < 1) {
    wozMsg(WOZ_MSG_ERROR, "INFO version %d is below minimum (1); aborting",
            di.version);
    return false;
  }

  if (!read8(fd, &di.diskType))
    return false;
  if (di.diskType != 1) {
    wozMsg(WOZ_MSG_ERROR, "Not a 5.25\" disk image; aborting");
    return false;
  }

  if (!read8(fd, &di.writeProtected))
    return false;

  if (!read8(fd, &di.synchronized))
    return false;

  if (!read8(fd, &di.cleaned))
    return false;

  di.creator[32] = 0;
  for (int i=0; i<32; i++) {
    if (!read8(fd, (uint8_t *)&di.creator[i]))
      return false;
  }

  if (di.version >= 2) {
    if (!read8(fd, &di.diskSides))
      return false;
    if (!read8(fd, &di.bootSectorFormat))
      return false;
    if (!read8(fd, &di.optimalBitTiming))
      return false;
    if (!read16(fd, &di.compatHardware))
      return false;
    if (!read16(fd, &di.requiredRam))
      return false;
    if (!read16(fd, &di.largestTrack))
      return false;
  } else {
    di.diskSides = 0;
    di.bootSectorFormat = 0;
    di.compatHardware = 0;
    di.requiredRam = 0;
    di.largestTrack = 13; // 13 * 512 bytes = 6656. All tracks are
			  // padded to 6646 (yes, 6646, not 6656)bytes
			  // in the v1 image.
    di.optimalBitTiming = 32; // "standard" disk bit timing for a 5.25" disk (4us per bit)
  }

  // INFO v3 added the FLUX chunk locator. If both fluxBlock and
  // largestFluxTrack come back zero, there's no FLUX chunk; otherwise
  // this WOZ carries flux-timing data for some tracks (we don't decode
  // those yet, but we at least know not to trip on the chunk itself).
  if (di.version >= 3) {
    if (!read16(fd, &di.fluxBlock))
      return false;
    if (!read16(fd, &di.largestFluxTrack))
      return false;
  } else {
    di.fluxBlock = 0;
    di.largestFluxTrack = 0;
  }

  return true;
}

bool Woz::parseMetaChunk(uint32_t chunkSize)
{
  metaData = (char *)wozCalloc(chunkSize+1, 1);
  if (!metaData)
    return false;

  if ((uint32_t)wozRead(fd, metaData, chunkSize) != chunkSize)
    return false;

  metaData[chunkSize] = 0;

  return true;
}

bool Woz::readWozDataTrack(uint8_t datatrack)
{
  // If it's already loaded then there's nothing to do here
  if (tracks[datatrack].trackData)
    return true;

  // If we have no open FD, then assume anything missing is supposed
  // to be missing
  if (fd == -1) {
    return true;
  }

  uint16_t bitsStartBlock = tracks[datatrack].startingBlock;

  // Allocate a new buffer for this track
  uint32_t count = tracks[datatrack].blockCount * 512;
  if (di.version == 1) count = (tracks[datatrack].bitCount / 8) + ((tracks[datatrack].bitCount % 8) ? 1 : 0);
  if (tracks[datatrack].trackData) {
    return true; // We've already read this track's data; don't re-read it
  }
  if (!count) {
    // This track has no data; do nothing, and we'll be successful at it
    return true;
  }
  tracks[datatrack].trackData = (uint8_t *)wozCalloc(count, 1);
  if (!tracks[datatrack].trackData) {
    wozMsg(WOZ_MSG_ERROR, "Failed to alloc buf to read track magnetic data: %s", strerror(errno));
    return false;
  }

  if (di.version == 1) {
    if (verbose) {
      wozMsg(WOZ_MSG_DEBUG, "Reading datatrack %d starting at byte 0x%lX",
	     datatrack,
	     (unsigned long)tracks[datatrack].startingByte);
    }
    if (wozSeek(fd, tracks[datatrack].startingByte, SEEK_SET) == -1) {
      wozMsg(WOZ_MSG_ERROR, "Failed to seek to start of block: %s", strerror(errno));
      return false;
    }
  } else {
    if (verbose) {
      wozMsg(WOZ_MSG_DEBUG, "Reading datatrack %d starting at byte 0x%lX",
	     datatrack,
	     (unsigned long)(bitsStartBlock*512));
    }
    if (wozSeek(fd, bitsStartBlock*512, SEEK_SET) == -1) {
      wozMsg(WOZ_MSG_ERROR, "Failed to seek to start of block: %s", strerror(errno));
      return false;
    }
  }
  uint32_t didRead = wozRead(fd, tracks[datatrack].trackData, count);
  if (didRead != count) {
    wozMsg(WOZ_MSG_ERROR, "Failed to read all track data for track [read %lu, wanted %lu]", (unsigned long)didRead, (unsigned long)count);
    return false;
  }

  return true;
}

#if 0
// Append "XX " for each of n bytes to buf (bounded by len)
static void hexBytes(char *buf, size_t len, const uint8_t *p, int n)
{
  size_t off = strlen(buf);
  for (int i=0; i<n && off < len; i++) {
    off += snprintf(&buf[off], len - off, "%.2X ", p[i]);
  }
}

static void dumpNibSector(nibSector *ns)
{
  char buf[200];

  snprintf(buf, sizeof(buf), "gap1: ");
  hexBytes(buf, sizeof(buf), ns->gap1, 48);
  wozMsg(WOZ_MSG_DEBUG, "%s", buf);
  snprintf(buf, sizeof(buf), "sectorProlog: ");
  hexBytes(buf, sizeof(buf), ns->sectorProlog, 3);
  wozMsg(WOZ_MSG_DEBUG, "%s", buf);
  wozMsg(WOZ_MSG_DEBUG, "  volume44: %.2X %.2X == %.2X", ns->volume44[0], ns->volume44[1], de44(ns->volume44));
  wozMsg(WOZ_MSG_DEBUG, "  sector44: %.2X %.2X == %.2X", ns->sector44[0], ns->sector44[1], de44(ns->sector44));
  wozMsg(WOZ_MSG_DEBUG, "  checksum44: %.2X %.2X == %.2X", ns->checksum44[0], ns->checksum44[1], de44(ns->checksum44));
  snprintf(buf, sizeof(buf), "sectorEpilog: ");
  hexBytes(buf, sizeof(buf), ns->sectorEpilog, 3);
  wozMsg(WOZ_MSG_DEBUG, "%s", buf);

  snprintf(buf, sizeof(buf), "gap2: ");
  hexBytes(buf, sizeof(buf), ns->gap2, 5);
  wozMsg(WOZ_MSG_DEBUG, "%s", buf);
  snprintf(buf, sizeof(buf), "dataProlog: ");
  hexBytes(buf, sizeof(buf), ns->dataProlog, 3);
  wozMsg(WOZ_MSG_DEBUG, "%s", buf);
  wozMsg(WOZ_MSG_DEBUG, "  data62:");
  for (int i=0; i<342; i+=16) {
    snprintf(buf, sizeof(buf), "    ");
    hexBytes(buf, sizeof(buf), &ns->data62[i], (i+16 <= 342) ? 16 : 342-i);
    wozMsg(WOZ_MSG_DEBUG, "%s", buf);
  }
  wozMsg(WOZ_MSG_DEBUG, "  checksum: %.2X", ns->checksum);
  snprintf(buf, sizeof(buf), "dataEpilog: ");
  hexBytes(buf, sizeof(buf), ns->dataEpilog, 3);
  wozMsg(WOZ_MSG_DEBUG, "%s", buf);
  wozMsg(WOZ_MSG_DEBUG, "");
}
#endif


// This writes to the given *physical* sector -- the caller is responsible for translating
// to a logical sector number
bool Woz::writeNibSectorDataToDataTrack(uint8_t dataTrack, uint8_t sector, uint8_t nibData[343])
{
  // Find the spot on the track that has the right sector
  if (!tracks[dataTrack].trackData) {
    // Load the cached track for this phys Nib track.
    if (!loadMissingTrackFromImage(dataTrack)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to read track %d", dataTrack);
      return false;
    }
  }
  
  // find the data header
  // write this nibblized data on top of whatever was there before
  // ensure the epilog is correct
  nibSector sectorData;
  memset(sectorData.gap1, 0xFF, sizeof(sectorData.gap1));
  memset(sectorData.gap2, 0xFF, sizeof(sectorData.gap2));
  
  // Allow two loops through the track data looking for the sector prolog
  uint32_t endCount = tracks[dataTrack].blockCount*512*2;
  if (di.version == 1) endCount = 2*6646;
  
  uint32_t ptr = 0;
  while (ptr < endCount) {
    sectorData.sectorProlog[0] = sectorData.sectorProlog[1];
    sectorData.sectorProlog[1] = sectorData.sectorProlog[2];
    sectorData.sectorProlog[2] = nextRawWozByte(dataTrack);
    ptr++;

    if (sectorData.sectorProlog[0] == 0xd5 &&
	sectorData.sectorProlog[1] == 0xaa &&
	sectorData.sectorProlog[2] == 0x96) {
      // Found *a* sector header. See if it's ours.
      sectorData.volume44[0] = nextRawWozByte(dataTrack);
      sectorData.volume44[1] = nextRawWozByte(dataTrack);
      sectorData.track44[0] = nextRawWozByte(dataTrack);
      sectorData.track44[1] = nextRawWozByte(dataTrack);
      sectorData.sector44[0] = nextRawWozByte(dataTrack);
      sectorData.sector44[1] = nextRawWozByte(dataTrack);
      sectorData.checksum44[0] = nextRawWozByte(dataTrack);
      sectorData.checksum44[1] = nextRawWozByte(dataTrack);
      sectorData.sectorEpilog[0] = nextRawWozByte(dataTrack);
      sectorData.sectorEpilog[1] = nextRawWozByte(dataTrack);
      sectorData.sectorEpilog[2] = nextRawWozByte(dataTrack);
      
      if (sectorData.sectorEpilog[0] == 0xde &&
	  sectorData.sectorEpilog[1] == 0xaa &&
	  sectorData.sectorEpilog[2] == 0xeb) {
	// Header is integral. See if it's our sector:
	uint8_t sectorNum = de44(sectorData.sector44);
	if (sectorNum != sector) {
	  continue;
	}
        
	// It's our sector - find the data chunk and write it
	while (ptr < tracks[dataTrack].blockCount*512*2) {
	  sectorData.dataProlog[0] = sectorData.dataProlog[1];
	  sectorData.dataProlog[1] = sectorData.dataProlog[2];
	  sectorData.dataProlog[2] = nextRawWozByte(dataTrack);
	  ptr++;

	  if (sectorData.dataProlog[0] == 0xd5 &&
	      sectorData.dataProlog[1] == 0xaa &&
	      sectorData.dataProlog[2] == 0xad) {
            // That's the data prolog, so next comes our data payload:
            // 342 bytes of payload, 1 byte of checksum.
	    for (int i=0; i<343; i++) {
              writeNextWozByte(dataTrack, nibData[i]);
            }
            writeNextWozByte(dataTrack, 0xDE);
            writeNextWozByte(dataTrack, 0xAA);
            writeNextWozByte(dataTrack, 0xEB);
            return true;
          }
        }
      }
    }
  }

  // If we get here, we failed to write it
  return false;
}

bool Woz::readNibSectorDataFromDataTrack(uint8_t dataTrack, uint8_t sector, nibSector *sectorData)
{
  // Find the sector header for this sector and return the nibblized data
  uint32_t ptr = 0;

  if (!tracks[dataTrack].trackData) {
    // Load the cached track for this phys Nib track.
    if (!loadMissingTrackFromImage(dataTrack)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to read track %d", dataTrack);
      return false;
    }
  }

  memset(sectorData->gap1, 0xFF, sizeof(sectorData->gap1));
  memset(sectorData->gap2, 0xFF, sizeof(sectorData->gap2));

  // Allow two loops through the track data looking for the sector prolog
  uint32_t endCount = tracks[dataTrack].blockCount*512*2;
  if (di.version == 1) endCount = 2*6646;

  while (ptr < endCount) {
    sectorData->sectorProlog[0] = sectorData->sectorProlog[1];
    sectorData->sectorProlog[1] = sectorData->sectorProlog[2];
    sectorData->sectorProlog[2] = nextDiskByte(dataTrack);
    ptr++;

    if (sectorData->sectorProlog[0] == 0xd5 &&
	sectorData->sectorProlog[1] == 0xaa &&
	sectorData->sectorProlog[2] == 0x96) {
      // Found *a* sector header. See if it's ours.
      sectorData->volume44[0] = nextDiskByte(dataTrack);
      sectorData->volume44[1] = nextDiskByte(dataTrack);
      sectorData->track44[0] = nextDiskByte(dataTrack);
      sectorData->track44[1] = nextDiskByte(dataTrack);
      sectorData->sector44[0] = nextDiskByte(dataTrack);
      sectorData->sector44[1] = nextDiskByte(dataTrack);
      sectorData->checksum44[0] = nextDiskByte(dataTrack);
      sectorData->checksum44[1] = nextDiskByte(dataTrack);
      sectorData->sectorEpilog[0] = nextDiskByte(dataTrack);
      sectorData->sectorEpilog[1] = nextDiskByte(dataTrack);
      sectorData->sectorEpilog[2] = nextDiskByte(dataTrack);

      if (sectorData->sectorEpilog[0] == 0xde &&
	  sectorData->sectorEpilog[1] == 0xaa &&
	  sectorData->sectorEpilog[2] == 0xeb) {
	// Header is integral. See if it's our sector:
	uint8_t sectorNum = de44(sectorData->sector44);
	if (sectorNum != sector) {
	  continue;
	}
	// It's our sector - find the data chunk and read it
	while (ptr < tracks[dataTrack].blockCount*512*2) {
	  sectorData->dataProlog[0] = sectorData->dataProlog[1];
	  sectorData->dataProlog[1] = sectorData->dataProlog[2];
	  sectorData->dataProlog[2] = nextDiskByte(dataTrack);
	  ptr++;

	  if (sectorData->dataProlog[0] == 0xd5 &&
	      sectorData->dataProlog[1] == 0xaa &&
	      sectorData->dataProlog[2] == 0xad) {
	    // Found the data; copy it in
	    for (int i=0; i<342; i++) {
	      sectorData->data62[i] = nextDiskByte(dataTrack);
	    }
	    sectorData->checksum = nextDiskByte(dataTrack);
	    sectorData->dataEpilog[0] = nextDiskByte(dataTrack);
	    sectorData->dataEpilog[1] = nextDiskByte(dataTrack);
	    sectorData->dataEpilog[2] = nextDiskByte(dataTrack);
	    if (sectorData->dataEpilog[0] != 0xde ||
		sectorData->dataEpilog[1] != 0xaa ||
		sectorData->dataEpilog[2] != 0xeb) {
	      continue;
	    }
	    // Have an integral hunk of data, with epilog - return it
	    return true;
	  }
	}
      }
    }
  }

  return false;
}

bool Woz::writeInfoChunk(uint8_t version, int fdout)
{
  if (!write8(fdout, version) ||
      !write8(fdout, di.diskType) ||
      !write8(fdout, di.writeProtected) ||
      !write8(fdout, di.synchronized) ||
      !write8(fdout, di.cleaned))
    return false;

  for (int i=0; i<32; i++) {
    if (!write8(fdout, di.creator[i]))
      return false;
  }
  
  if (version >= 2) {
    // If we read a Wozv1, this will be set to 0. Set it to 1.
    if (di.diskSides == 0)
      di.diskSides = 1;

    if ( !write8(fdout, di.diskSides) ||
	 !write8(fdout, di.bootSectorFormat) ||
	 !write8(fdout, di.optimalBitTiming) ||
	 !write16(fdout, di.compatHardware) ||
	 !write16(fdout, di.requiredRam) ||
	 !write16(fdout, di.largestTrack))
      return false;
  }

  // Padding
  for (int i=0; i<((version==1)?23:14); i++) {
    if (!write8(fdout, 0))
      return false;
  }
  return true;
}

bool Woz::writeTMAPChunk(uint8_t version, int fdout)
{
  for (int i=0; i<40*4; i++) {
    if (!write8(fdout, quarterTrackMap[i]))
      return false;
  }

  return true;
}

bool Woz::writeTRKSChunk(uint8_t version, int fdout)
{
  if (version == 1) {
    wozMsg(WOZ_MSG_ERROR, "V1 write is not implemented");
    return false;
  }

  // Reconstruct all of the starting blocks/blockCounts for each
  // track. The bitCount should be correct.
  uint8_t numTracksPacked = 0;
  for (int i=0; i<160; i++) {
    // If we didn't preload, and the track isn't loaded, then load it now
    if (autoFlushTrackData && !tracks[i].trackData) {
      loadMissingTrackFromImage(i);
    }
    
    if (tracks[i].trackData && tracks[i].bitCount) {
      // For any tracks that have data, put it somewhere in the destination file
      tracks[i].startingBlock = STARTBLOCK + 13*(numTracksPacked++);
      // assume tracks[track].bitCount is correct, and recalculate the block size of this track
      uint32_t bytes = (tracks[i].bitCount / 8) + ((tracks[i].bitCount % 8) ? 1 : 0);
      uint32_t blocks = (bytes / 512) + ((bytes % 512) ? 1 : 0);
      tracks[i].blockCount = blocks;
    } else {
      tracks[i].startingBlock = 0;
      tracks[i].blockCount = 0;
      tracks[i].bitCount = 0;
    } 
    if (!write16(fdout, tracks[i].startingBlock))
      return false;
    if (!write16(fdout, tracks[i].blockCount))
      return false;
    if (!write32(fdout, tracks[i].bitCount))
      return false;
  }

  // All the track data
  for (int i=0; i<160; i++) {
    // If we didn't preload, and the track isn't loaded, then load it now
    if (autoFlushTrackData && !tracks[i].trackData) {
      loadMissingTrackFromImage(i);
    }
    
    if (tracks[i].startingBlock &&
	tracks[i].blockCount) {
      if (wozSeek(fdout, tracks[i].startingBlock * 512, SEEK_SET) == -1) {
	wozMsg(WOZ_MSG_ERROR, "Failed to seek before writing track");
	return false;
      }
      // Technically, we only have this many bytes to write:
      // uint32_t writeSize = (tracks[i].bitCount / 8) + ((tracks[i].bitCount % 8) ? 1 : 0);
      // ... but in practice, the tracks are all padded to NIBTRACKSIZE bytes;
      // and we alloc'd a buffer of that size, too; so write the whole thing,
      // since it would have been calloc'd initially.
      ssize_t numWritten = wozWrite(fdout, tracks[i].trackData, NIBTRACKSIZE);
      if (numWritten != NIBTRACKSIZE) {
	int err = errno;
	wozMsg(WOZ_MSG_ERROR, "Failed to write track [%d]", (int)numWritten);
	wozMsg(WOZ_MSG_ERROR, "error writing: %s", strerror(err));
	return false;
      }
      tracks[i].dirty = false;
    }
  }
  return true;
}

bool Woz::decodeWozTrackToNibFromDataTrack(uint8_t dataTrack, nibSector sectorData[16])
{
  for (int sector=0; sector<16; sector++) {
    if (!readNibSectorDataFromDataTrack(dataTrack, sector, (nibSector *)(&sectorData[sector]))) {
#if 0
      /* Debugging code -- dump the raw track data for inspection */
      printf("Track is %d bits long (%d blocks)\n", tracks[dataTrack].bitCount, tracks[dataTrack].blockCount);
      if (tracks[dataTrack].trackData) {
        for (int i=0; i<tracks[dataTrack].bitCount/8; i+=16) {
          fprintf(stderr, "%.4X  ", i);
          for (int j=0; j<16 && ((i+j)<(tracks[dataTrack].bitCount/8)); j++) {
            fprintf(stderr, "%.2X ", tracks[dataTrack].trackData[i+j]);
          }
          fprintf(stderr, "\n");
        }
      }
#endif      
      wozMsg(WOZ_MSG_ERROR, "failed to readNibSectorDataFromDataTrack for sector %d", sector);
      return false;
    }
  }

  return true;
}

// The caller is responsible for translating between the physical sector numbers and the logical
// sector numbers. This only takes in to account the physical sector numbers (whatever is
// represented in the sector's header).
bool Woz::decodeWozTrackSector(uint8_t phystrack, uint8_t sector, uint8_t dataOut[256])
{
  uint8_t dataTrack = quarterTrackMap[phystrack*4];
  nibSector nibData;
  if (!readNibSectorDataFromDataTrack(dataTrack, sector, &nibData)) {
    wozMsg(WOZ_MSG_ERROR, "Failed to readNibSectorDataFromDataTrack for track %d sector %d", phystrack, sector);
    return false;
  }
  
#if 0
  printf("Track %d physector %d nib dump:\n", phystrack, sector);
  dumpNibSector(&nibData);
#endif
  if (denibblizeSector(nibData, dataOut) != errorNone) {
    wozMsg(WOZ_MSG_ERROR, "failed to denib sector");
    return false;
  }
  return true;
}

// The caller is responsible for translating between the physical sector numbers and the logical
// sector numbers. This only takes in to account the physical sector numbers (whatever is
// represented in the sector's header).
bool Woz::encodeWozTrackSector(uint8_t phystrack, uint8_t sector, uint8_t dataIn[256])
{
  uint8_t dataTrack = quarterTrackMap[phystrack*4];
  uint8_t dataOut[343];

  if (nibblizeSector(dataIn, dataOut) != errorNone) {
    wozMsg(WOZ_MSG_ERROR, "Failed to nibblizeSector for track %d sector %d", phystrack, sector);
    return false;
  }

  if (!writeNibSectorDataToDataTrack(dataTrack, sector, dataOut)) {
    wozMsg(WOZ_MSG_ERROR, "Failed to writeNibSectorDataToDataTrack for track %d sector %d", phystrack, sector);
    return false;
  }

  return true;
}


bool Woz::isThirteenSectorDisk()
{
  if (imageType == T_D13 || di.bootSectorFormat == 2)
    return true;
  if (imageType == T_DSK || imageType == T_PO || imageType == T_HDV ||
      imageType == T_AUTO || di.bootSectorFormat == 1 || di.bootSectorFormat == 3)
    return false;
  if (thirteenSector < 0)
    thirteenSector = sniffThirteenSector() ? 1 : 0;
  return thirteenSector == 1;
}

bool Woz::sniffThirteenSector()
{
  // Count the 13-sector (D5 AA B5) and 16-sector (D5 AA 96) address
  // prologues on track 0, or track 17 (where the catalog is) if track 0
  // has neither, as a copy-protected boot track might
  uint8_t *nib = (uint8_t *)wozMalloc(NIBTRACKSIZE);
  if (!nib) return false;
  // Reading a track moves the head's cursor, which an emulator may be in
  // the middle of using, so it goes back where it was afterward
  uint32_t savedPointer = trackPointer, savedBitCounter = trackBitCounter;
  uint8_t savedByte = trackByte, savedFrom = trackByteFromDataTrack;
  uint8_t savedBitIdx = trackBitIdx, savedLoop = trackLoopCounter;
  uint8_t savedWindow = headWindow, savedRandData = randData, savedRandPtr = randPtr;
  const uint8_t tryTracks[2] = { 0, 17 };
  bool thirteen = false;
  for (int t = 0; t < 2; t++) {
    if (!readRawNibStream(tryTracks[t], nib)) continue;
    uint32_t b5 = 0, n96 = 0;
    for (uint32_t i = 0; i < NIBTRACKSIZE; i++) {
      if (nib[i] != 0xD5 || nib[(i+1) % NIBTRACKSIZE] != 0xAA) continue;
      uint8_t third = nib[(i+2) % NIBTRACKSIZE];
      if (third == 0xB5) b5++;
      else if (third == 0x96) n96++;
    }
    if (b5 || n96) {
      thirteen = (b5 > n96);
      break;
    }
  }
  wozFree(nib);
  trackPointer = savedPointer; trackBitCounter = savedBitCounter;
  trackByte = savedByte; trackByteFromDataTrack = savedFrom;
  trackBitIdx = savedBitIdx; trackLoopCounter = savedLoop;
  headWindow = savedWindow; randData = savedRandData; randPtr = savedRandPtr;
  return thirteen;
}

bool Woz::readRawNibStream(uint8_t phystrack, uint8_t out[])
{
  uint8_t dataTrack = quarterTrackMap[phystrack*4];
  // Only fall through to loadMissingTrackFromImage if the track isn't
  // already resident - otherwise on a preload-loaded DSK the fd has
  // been closed and the re-read would fail.
  if (!tracks[dataTrack].trackData) {
    if (!loadMissingTrackFromImage(dataTrack)) return false;
  }
  for (uint32_t i = 0; i < NIBTRACKSIZE; i++) {
    out[i] = nextDiskByte(dataTrack);
  }
  return true;
}

bool Woz::decodeWozTrack13ToDsk(uint8_t phystrack, uint8_t sectorData[256*13])
{
  // 13-sector tracks can't reuse the 16-sector `nibSector[16]` staging
  // buffer the 6-and-2 path builds: they have a different address
  // prologue and a larger per-sector data field. Instead, pull the raw
  // nib stream and scan it with the 13-sector denibblizer.
  uint8_t *nibStream = (uint8_t *)wozCalloc(NIBTRACKSIZE, 1);
  if (!nibStream) return false;
  if (!readRawNibStream(phystrack, nibStream)) { wozFree(nibStream); return false; }

  nibErr rc = denibblizeTrack13(nibStream, sectorData);
  wozFree(nibStream);
  if (rc != errorNone) {
    wozMsg(WOZ_MSG_ERROR, "failed to denib 13-sector track (err %d)", (int)rc);
    return false;
  }
  return true;
}

bool Woz::decodeWozTrackToDsk(uint8_t phystrack, uint8_t subtype, uint8_t sectorData[256*16])
{
  // Figure out which datatrack we need for the given physical track
  uint8_t dataTrack = quarterTrackMap[phystrack*4];
  // First read it to a NIB; then convert the NIB to a DSK.
  nibSector nibData[16];
  if (!decodeWozTrackToNibFromDataTrack(dataTrack, nibData)) {
    wozMsg(WOZ_MSG_ERROR, "failed to decode Woz");
    return false;
  }
#if 0  
  for (int s=0; s<16; s++) {
    printf("Track read dumping nib sector %d:\n", s);
    dumpNibSector(&nibData[s]);
  }
#endif
  
  if (denibblizeTrack((const uint8_t *)nibData, sectorData, subtype) != errorNone) {
    wozMsg(WOZ_MSG_ERROR, "failed to denib track");
    return false;
  }

  return true;
}

bool Woz::checksumWozDataTrack(uint8_t datatrack, uint32_t *retCRC)
{
  if (!retCRC)
    return false;

  if (!tracks[datatrack].trackData) {
    *retCRC = 0;
    return false;
  }
  
  *retCRC = compute_crc_32(tracks[datatrack].trackData, tracks[datatrack].bitCount/8);
  return true;
}



bool Woz::getMetadataEntry(uint32_t index,
                           char *key, size_t keyLen,
                           char *value, size_t valueLen)
{
  if (!metaData)
    return false;

  // The META chunk is lines of "key<tab>value", separated by '\n'. Empty
  // lines don't count toward the index.
  const char *line = metaData;
  while (*line) {
    const char *eol = strchr(line, '\n');
    size_t lineLen = eol ? (size_t)(eol - line) : strlen(line);
    if (lineLen) {
      if (index == 0) {
        const char *tab = (const char *)memchr(line, '\t', lineLen);
        if (!tab) {
          wozMsg(WOZ_MSG_ERROR, "ERROR: no delineator on a line of metadata [%.*s]",
                 (int)lineLen, line);
          return false;
        }
        size_t kl = tab - line;
        size_t vl = lineLen - kl - 1;
        snprintf(key, keyLen, "%.*s", (int)kl, line);
        snprintf(value, valueLen, "%.*s", (int)vl, tab + 1);
        return true;
      }
      index--;
    }
    if (!eol)
      break;
    line = eol + 1;
  }
  return false;
}

uint8_t Woz::scanTrackOrderedSectors(uint8_t datatrack, trackSectorDump *out,
                                     uint8_t maxOut)
{
#define denib(a, b) ((((a) & ~0xAA) << 1) | ((b) & ~0xAA))
  // Look at the sectors in the order they're found on the track, for up
  // to two revolutions
  trackPointer = 0;
  trackBitIdx = 0x80;
  trackBitCounter = 0;
  trackByteFromDataTrack = 255;
  uint16_t sectorsFound = 0;
  uint8_t count = 0;
  do {
    if (nextDiskByte(datatrack) == 0xD5 &&
        nextDiskByte(datatrack) == 0xAA &&
        nextDiskByte(datatrack) == 0x96) {
      trackSectorDump discard;
      trackSectorDump *d = (count < maxOut) ? &out[count++] : &discard;
      uint8_t a, b;
      a = nextDiskByte(datatrack); b = nextDiskByte(datatrack);
      d->volume = denib(a, b);
      a = nextDiskByte(datatrack); b = nextDiskByte(datatrack);
      d->track = denib(a, b);
      a = nextDiskByte(datatrack); b = nextDiskByte(datatrack);
      d->sector = denib(a, b);
      sectorsFound |= (1 << d->sector);
      a = nextDiskByte(datatrack); b = nextDiskByte(datatrack);
      d->checksum = denib(a, b);
      d->haveData = false;

      nextDiskByte(datatrack); // skip epilog
      nextDiskByte(datatrack);
      nextDiskByte(datatrack);
      // look for data prolog d5 aa ad
      while (nextDiskByte(datatrack) != 0xD5 && trackLoopCounter < 2)
        ;
      if (trackLoopCounter < 2) {
        // Hope that's it and skip two bytes
        nextDiskByte(datatrack);
        nextDiskByte(datatrack);
        // Keep the 6-and-2 data
        for (int k=0; k<342; k++) {
          d->data62[k] = nextDiskByte(datatrack);
        }
        d->haveData = true;
      }
    }
  } while (sectorsFound != 0xFFFF && trackLoopCounter < 2);
#undef denib
  return count;
}

bool Woz::isSynchronized()
{
  return di.synchronized;
}

uint8_t Woz::dataTrackNumberForQuarterTrack(uint16_t qt)
{
  return quarterTrackMap[qt];
}

bool Woz::flush()
{
  // *** FIXME - should flush() write the image out if it's dirty?
  return true;
}

bool Woz::isDirty()
{
  if (hdvDirty)
    return true;
  for (int i=0; i<160; i++) {
    if (tracks[i].dirty)
      return true;
  }
  return false;
}

