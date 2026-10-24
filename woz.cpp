#include "woz.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include "crc32.h"
#include "nibutil.h"
#include "version.h"
#include "wozmsg.h"

// Block number we start packing data bits after (Woz 2.0 images)
#define STARTBLOCK 3

#if defined(AIIE) || defined(TEENSYDUINO)
#define LAZYFD
#include "fscompat.h"
#endif

#ifdef TEENSYDUINO
// This junk is for my AiiE project. I need to abstract it out better.
#include "iocompat.h"
#define SKIPCHECKSUM
#define malloc extmem_malloc
#define free extmem_free
#define calloc extmem_calloc
#define realloc extmem_realloc
#endif

#define PREP_SECTION(fd, t) {      \
  uint32_t type = t;               \
  if (!write32(fd, type))           \
    return false;                  \
  if (!write32(fd, 0))              \
    return false;                  \
  curpos = lseek(fd, 0, SEEK_CUR); \
 }

#define END_SECTION(fd) {                   \
  long endpos = lseek(fd, 0, SEEK_CUR);	    \
  lseek(fd, curpos-4, SEEK_SET);            \
  uint32_t chunksize = endpos - curpos;                                 \
  if (!write32(fd, chunksize))               \
    return false;                           \
  lseek(fd, endpos, SEEK_SET);                   \
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
}

Woz::~Woz()
{
  if (fd != -1) {
    close(fd);
    fd = -1;
  }

  for (int i=0; i<160; i++) {
    if (tracks[i].trackData) {
      free(tracks[i].trackData);
      tracks[i].trackData = NULL;
    }
  }
  if (metaData) {
    free(metaData);
    metaData = NULL;
  }
  if (hdvData) {
    free(hdvData);
    hdvData = NULL;
  }
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
  if (write(fd, &v, 1) != 1)
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
  if (read(fd, &r, 1) != 1)
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
               strcasecmp(p, ".2mg") == 0 ||
               strcasecmp(p, ".img") == 0) {
      forceType = T_HDV;
    } else {
      wozMsg(WOZ_MSG_ERROR, "Unable to determine file type of '%s'", filename);
      return false;
    }
  }

  // Mirror the promotion readFile does. An oversized .po/.dsk was read as
  // raw blocks, so write it back as raw blocks instead of refusing:
  // otherwise an 800K .po (how real Apple Pascal 3.5" disks and plenty of
  // ProDOS volumes ship) could be read and modified but never saved,
  // except by renaming it .hdv. This deliberately does not apply to a
  // genuine .hdv/.img, so asking to convert a hard-disk image into a
  // floppy format is still the error it always was.
  if (imageType == T_HDV && hdvPromotedFromFloppyExt &&
      (forceType == T_PO || forceType == T_DSK))
    forceType = T_HDV;

  // Anything genuinely floppy-oriented (.woz, .nib) still can't represent
  // a hard-disk image. Say so rather than write truncated garbage.
  if (imageType == T_HDV && forceType != T_HDV) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: can't convert a hard-disk image to a floppy format");
    return false;
  }

  switch (forceType) {
  case T_WOZ:
    return writeWozFile(filename, forceType);
  case T_DSK:
  case T_PO:
    return writeDskFile(filename, forceType);
  case T_NIB:
    return writeNibFile(filename);
  case T_HDV:
    return writeHdvFile(filename);
  default:
    wozMsg(WOZ_MSG_ERROR, "Unknown disk type; unable to write");
    return false;
  }
}

bool Woz::writeWozFile(const char *filename, uint8_t subtype)
{
  int fdout = -1;

  fdout = open(filename, O_TRUNC|O_CREAT|O_RDWR, S_IRUSR|S_IWUSR);
  if (fdout == -1) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: Unable to open output file: %s", strerror(errno));
    return false;
  }

  bool retval = writeWozFile(fdout, subtype);
  close(fdout);

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
  lseek(fdout, 0, SEEK_SET);
  
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
  crcPos = lseek(fdout, 0, SEEK_CUR);
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
    if ((size_t)write(fdout, metaData, strlen(metaData)) != strlen(metaData)) {
      wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write META chunk");
      goto done;
    }
    END_SECTION(fdout);
  }

  // FIXME: missing the WRIT chunk, if it exists

  // Fix up the checksum. Optional; the spec says it can be 0 meaning
  // "don't verify"
#ifndef SKIPCHECKSUM
  endPos = lseek(fdout, 0, SEEK_CUR);
  crcDataSize = endPos-crcPos-4;
  crcData = (uint8_t *)malloc(crcDataSize);
  if (!crcData) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to malloc crc data chunk");
    goto done;
  }
    
  // Read the data in for checksumming
  if (lseek(fdout, crcPos+4, SEEK_SET) == -1) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to fseek to crcPos+4 (0x%llX)", (unsigned long long)(crcPos+4));
    goto done;
  }

  tmp32 = read(fdout, crcData, crcDataSize);
  if (tmp32 != crcDataSize) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to read in data for checksum [read %lu, wanted %lu]", (unsigned long)tmp32, (unsigned long)crcDataSize);
    goto done;
  }
    
  tmp32 = compute_crc_32(crcData, crcDataSize);
  // Write it back out
  lseek(fdout, crcPos, SEEK_SET);
  if (!write32(fdout, tmp32)) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: failed to write CRC");
    goto done;
  }
#endif
  
  for (int i=0; i<160; i++) {
    tracks[i].dirty = false;
  }

  
  retval = true;

 done:
  if (crcData)
    free(crcData);

  return retval;
}

bool Woz::writeDskFile(const char *filename, uint8_t subtype)
{
  int fdout = open(filename, O_CREAT|O_TRUNC|O_WRONLY, S_IRUSR|S_IWUSR);
  if (fdout == -1) {
    wozMsg(WOZ_MSG_ERROR, "Failed to open output file: %s", strerror(errno));
    return false;
  }

  bool retval = writeDskFile(fdout, subtype);
  close(fdout);
  return retval;
}

bool Woz::writeDskFile(int fdout, uint8_t subtype)
{
  if (isSynchronized()) {
    wozMsg(WOZ_MSG_WARNING, "WARNING: disk image has synchronized tracks; it may not work as a DSK or NIB file.");
  }
  lseek(fdout, 0, SEEK_SET);

  uint8_t sectorData[256*16];
  for (int phystrack=0; phystrack<35; phystrack++) {
    if (!decodeWozTrackToDsk(phystrack, subtype, sectorData)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to decode track %d; aborting", phystrack);
      return false;
    }
    ssize_t numWritten = write(fdout, sectorData, 256*16);
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
  int fdout = open(filename, O_CREAT|O_TRUNC|O_WRONLY, S_IRUSR|S_IWUSR);
  if (fdout == -1) {
    wozMsg(WOZ_MSG_ERROR, "Failed to open output file: %s", strerror(errno));
    return false;
  }

  bool retval = writeNibFile(fdout);
  close(fdout);
  return retval;
}

bool Woz::writeNibFile(int fdout)
{
  if (isSynchronized()) {
    wozMsg(WOZ_MSG_WARNING, "WARNING: disk image has synchronized tracks; it may not work as a DSK or NIB file.");
  }
  lseek(fdout, 0, SEEK_SET);

  nibSector nibData[16];
  for (int phystrack=0; phystrack<35; phystrack++) {
    if (!decodeWozTrackToNibFromDataTrack(quarterTrackMap[phystrack*4], nibData)) {
      wozMsg(WOZ_MSG_ERROR, "Failed to decode track %d; aborting", phystrack);
      return false;
    }
    if (write(fdout, nibData, NIBTRACKSIZE) != NIBTRACKSIZE) {
      wozMsg(WOZ_MSG_ERROR, "Failed[1] to write track %d; aborting", phystrack);
      return false;
    }
  }

  for (int i=0; i<160; i++) {
    tracks[i].dirty = false;
  }

  return true;
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
        free(tracks[i].trackData);
        tracks[i].trackData = NULL;
      }
    }
  }

  // Based on the source image type, load the data track we're looking for
  if (imageType == T_WOZ) {
    // If the source was WOZ, just load the datatrack directly
    return readWozDataTrack(datatrack);
  } else if (imageType == T_PO ||
	     imageType == T_DSK) {
    // If the source was a DSK file, then the datatrack was mapped directly
    // from the physical track
    if (datatrack >= 35) {
      // There are only 35 tracks; the remainder are blank.
      tracks[datatrack].trackData = NULL;
      return true;
    }
    
    uint8_t phystrack = datatrack; // used for clarity of which kind of track we mean, below
    
    static uint8_t sectorData[256*16];

    lseek(fd, 256*16*phystrack, SEEK_SET);

    if (read(fd, sectorData, 256*16) != 256*16) {
      wozMsg(WOZ_MSG_ERROR, "Failed to read sector");
      return false;
    }

    tracks[datatrack].trackData = (uint8_t *)calloc(NIBTRACKSIZE, 1);
    if (!tracks[datatrack].trackData) {
      wozMsg(WOZ_MSG_ERROR, "Failed to malloc track data");
      return false;
    }
    tracks[datatrack].startingBlock = STARTBLOCK + 13*phystrack;
    tracks[datatrack].blockCount = 13;
    uint32_t sizeInBits = nibblizeTrack(tracks[datatrack].trackData, sectorData, imageType, phystrack);
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
    tracks[datatrack].trackData = (uint8_t *)malloc(NIBTRACKSIZE);
    if (!tracks[datatrack].trackData) {
      wozMsg(WOZ_MSG_ERROR, "Failed to malloc track data");
      return false;
    }

    lseek(fd, NIBTRACKSIZE * phystrack, SEEK_SET);
    read(fd, tracks[datatrack].trackData, NIBTRACKSIZE);
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

  if (fd != -1) close(fd);
  fd = open(filename, O_RDWR, S_IRUSR|S_IWUSR);
  if (fd == -1) {
    wozMsg(WOZ_MSG_ERROR, "Unable to open input file: %s", strerror(errno));
    goto done;
  }

  _initInfo();

  // Now read in the 35 tracks of data from the DSK file and convert them to NIB
  if (preloadTracks) {
    uint8_t sectorData[256*16];
    for (int phystrack=0; phystrack<35; phystrack++) {
      uint32_t bytesRead = read(fd, sectorData, 256*16);
      if (bytesRead != 256*16) {
	wozMsg(WOZ_MSG_ERROR, "Failed to read DSK data; got %lu bytes, wanted %d", (unsigned long)bytesRead, 256);
	goto done;
      }
      uint8_t datatrack = quarterTrackMap[phystrack*4];
      tracks[datatrack].trackData = (uint8_t *)calloc(NIBTRACKSIZE, 1);
      if (!tracks[datatrack].trackData) {
	wozMsg(WOZ_MSG_ERROR, "Failed to malloc track data");
	goto done;
      }
      tracks[datatrack].startingBlock = STARTBLOCK + 13*datatrack;
      tracks[datatrack].blockCount = 13;
      uint32_t sizeInBits = nibblizeTrack(tracks[datatrack].trackData, sectorData, subtype, phystrack);
      tracks[datatrack].bitCount = sizeInBits; // ... reality.
    }
  }
  
  retval = true;

 done:
#ifndef LAZYFD
  if (preloadTracks && fd != -1) {
    close(fd);
    fd = -1;
  }
#endif
  return retval;
}

bool Woz::readHdvFile(const char *filename)
{
  // ProDOS hard-disk images are raw block-sequential ProDOS data, no
  // floppy-track structure. We slurp the whole file into a heap buffer
  // that higher layers (ProdosSpector) read blocks out of directly.
  imageType = T_HDV;
  autoFlushTrackData = false;

  if (fd != -1) close(fd);
  fd = open(filename, O_RDONLY, S_IRUSR);
  if (fd == -1) {
    wozMsg(WOZ_MSG_ERROR, "Unable to open input file: %s", strerror(errno));
    return false;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    wozMsg(WOZ_MSG_ERROR, "Unable to stat input file: %s", strerror(errno));
    close(fd); fd = -1;
    return false;
  }
  if (st.st_size == 0 || (st.st_size % 512) != 0) {
    wozMsg(WOZ_MSG_ERROR, "HDV image '%s' has a bad size (%lld bytes)",
            filename, (long long)st.st_size);
    close(fd); fd = -1;
    return false;
  }

  if (hdvData) { free(hdvData); hdvData = NULL; }
  hdvByteSize = (uint32_t)st.st_size;
  hdvData = (uint8_t *)malloc(hdvByteSize);
  if (!hdvData) {
    wozMsg(WOZ_MSG_ERROR, "Out of memory loading HDV (%u bytes)", hdvByteSize);
    close(fd); fd = -1;
    return false;
  }
  ssize_t got = read(fd, hdvData, hdvByteSize);
  close(fd); fd = -1;
  if (got != (ssize_t)hdvByteSize) {
    wozMsg(WOZ_MSG_ERROR, "Short read on HDV (%zd of %u bytes)",
            got, hdvByteSize);
    free(hdvData); hdvData = NULL;
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
            hdvByteSize, newByteSize);
    return false;
  }
  if (!newByteSize || (newByteSize % 512)) {
    wozMsg(WOZ_MSG_ERROR, "Image size must be a nonzero multiple of 512 bytes");
    return false;
  }
  // Hand the tail back if the allocator will take it. Keeping the bigger
  // buffer is harmless either way: hdvByteSize is what gets written.
  uint8_t *smaller = (uint8_t *)realloc(hdvData, newByteSize);
  if (smaller)
    hdvData = smaller;
  hdvByteSize = newByteSize;
  return true;
}

bool Woz::writeHdvFile(const char *filename)
{
  if (!hdvData || !hdvByteSize) {
    wozMsg(WOZ_MSG_ERROR, "No HDV buffer to write");
    return false;
  }
  int fdout = open(filename, O_TRUNC|O_CREAT|O_WRONLY, S_IRUSR|S_IWUSR);
  if (fdout == -1) {
    wozMsg(WOZ_MSG_ERROR, "Unable to open output HDV file: %s", strerror(errno));
    return false;
  }
  ssize_t wrote = write(fdout, hdvData, hdvByteSize);
  close(fdout);
  if (wrote != (ssize_t)hdvByteSize) {
    wozMsg(WOZ_MSG_ERROR, "Short write on HDV (%zd of %u bytes)",
            wrote, hdvByteSize);
    return false;
  }
  return true;
}

bool Woz::readNibFile(const char *filename, bool preloadTracks)
{
  autoFlushTrackData = !preloadTracks;
  imageType = T_NIB;

  if (fd != -1) close(fd);
  fd = open(filename, O_RDWR, S_IRUSR|S_IWUSR);
  if (fd == -1) {
    wozMsg(WOZ_MSG_ERROR, "Unable to open input file: %s", strerror(errno));
    return false;
  }
  
  _initInfo();

  // Now read in the 35 tracks of data from the nib file
  if (preloadTracks) {
    nibSector nibData[16];
    for (int phystrack=0; phystrack<35; phystrack++) {
      uint32_t bytesRead = read(fd, nibData, NIBTRACKSIZE);
      if (bytesRead != NIBTRACKSIZE) {
	wozMsg(WOZ_MSG_ERROR, "Failed to read NIB data; got %lu bytes, wanted %d", (unsigned long)bytesRead, NIBTRACKSIZE);
	return false;
      }
      uint8_t datatrack = quarterTrackMap[phystrack * 4];
      tracks[datatrack].trackData = (uint8_t *)calloc(NIBTRACKSIZE, 1);
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
#ifndef LAZYFD
  if (preloadTracks && fd != -1) {
    close(fd);
    fd = -1;
  }
#endif
  return true;
}

bool Woz::readWozFile(const char *filename, bool preloadTracks)
{
  imageType = T_WOZ;
  autoFlushTrackData = !preloadTracks;

  if (fd != -1) close(fd);
  fd = open(filename, O_RDWR, S_IRUSR|S_IWUSR);
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
      close(fd);
    return false;
  }

  uint32_t tmp;
  if (!read32(fd, &tmp)) {
    wozMsg(WOZ_MSG_ERROR, "Read failure");
    if (preloadTracks && fd != -1)
      close(fd);
    return false;
  }
  if (tmp != 0x0A0D0AFF) {
    wozMsg(WOZ_MSG_ERROR, "WOZ header failure; exiting");
    if (preloadTracks && fd != -1)
      close(fd);
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
    if (lseek(fd, fpos, SEEK_SET) == -1) {
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
	close(fd);
      return false;
    }

    bool isOk;

    switch (chunkType) {
    case 0x4F464E49: // 'INFO'
      if (verbose) {
	wozMsg(WOZ_MSG_DEBUG, "Reading INFO chunk starting at byte 0x%llX",
	       (unsigned long long)lseek(fd, 0, SEEK_CUR));
      }
      isOk = parseInfoChunk(chunkDataSize);
      haveData |= cINFO;
      break;
    case 0x50414D54: // 'TMAP'
      if (verbose) {
	wozMsg(WOZ_MSG_DEBUG, "Reading TMAP chunk starting at byte 0x%llX",
	       (unsigned long long)lseek(fd, 0, SEEK_CUR));
      }
      isOk = parseTMAPChunk(chunkDataSize);
      haveData |= cTMAP;
      break;
    case 0x534B5254: // 'TRKS'
      if (verbose) {
	wozMsg(WOZ_MSG_DEBUG, "Reading TRKS chunk starting at byte 0x%llX",
	       (unsigned long long) lseek(fd, 0, SEEK_CUR));
      }
      isOk = parseTRKSChunk(chunkDataSize);
      haveData |= cTRKS;
      break;
    case 0x4154454D: // 'META'
      if (verbose) {
	wozMsg(WOZ_MSG_DEBUG, "Reading META chunk starting at byte 0x%llX",
	       (unsigned long long) lseek(fd, 0, SEEK_CUR));
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
	close(fd);
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
	  close(fd);
	  fd = -1;
	}
	return false;
      }
    }
  }

#ifndef LAZYFD
  if (preloadTracks && fd != -1) {
    close(fd);
    fd = -1;
  }
#endif
  return true;
}

bool Woz::readFile(const char *filename, bool preloadTracks, uint8_t forceType)
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
               strcasecmp(p, ".2mg") == 0 ||
               strcasecmp(p, ".img") == 0) {
      forceType = T_HDV;
    } else {
      wozMsg(WOZ_MSG_ERROR, "Unable to determine file type of '%s'", filename);
      return false;
    }
  }

  // A .po or .dsk file larger than one floppy (> 140 KB) can't fit the
  // 35-track geometry the other paths assume; promote it to T_HDV so the
  // raw-block path is used instead.
  if (forceType == T_PO || forceType == T_DSK) {
    struct stat st;
    if (stat(filename, &st) == 0 && st.st_size > 35 * 16 * 256) {
      if (verbose) {
        wozMsg(WOZ_MSG_DEBUG, "Input file is larger than a floppy (%lld bytes); "
               "treating as ProDOS hard-disk image.",
               (long long)st.st_size);
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
    lseek(fd, (trackNumber * 6656 + 256) + 6648, SEEK_SET);
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
  metaData = (char *)calloc(chunkSize+1, 1);
  if (!metaData)
    return false;

  if ((uint32_t)read(fd, metaData, chunkSize) != chunkSize)
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
  tracks[datatrack].trackData = (uint8_t *)calloc(count, 1);
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
    if (lseek(fd, tracks[datatrack].startingByte, SEEK_SET) == -1) {
      wozMsg(WOZ_MSG_ERROR, "Failed to seek to start of block: %s", strerror(errno));
      return false;
    }
  } else {
    if (verbose) {
      wozMsg(WOZ_MSG_DEBUG, "Reading datatrack %d starting at byte 0x%lX",
	     datatrack,
	     (unsigned long)(bitsStartBlock*512));
    }
    if (lseek(fd, bitsStartBlock*512, SEEK_SET) == -1) {
      wozMsg(WOZ_MSG_ERROR, "Failed to seek to start of block: %s", strerror(errno));
      return false;
    }
  }
  uint32_t didRead = read(fd, tracks[datatrack].trackData, count);
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
      if (lseek(fdout, tracks[i].startingBlock * 512, SEEK_SET) == -1) {
	wozMsg(WOZ_MSG_ERROR, "Failed to seek before writing track");
	return false;
      }
      // Technically, we only have this many bytes to write:
      // uint32_t writeSize = (tracks[i].bitCount / 8) + ((tracks[i].bitCount % 8) ? 1 : 0);
      // ... but in practice, the tracks are all padded to NIBTRACKSIZE bytes;
      // and we alloc'd a buffer of that size, too; so write the whole thing,
      // since it would have been calloc'd initially.
      ssize_t numWritten = write(fdout, tracks[i].trackData, NIBTRACKSIZE);
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
  return di.bootSectorFormat == 2;
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
  uint8_t *nibStream = (uint8_t *)calloc(NIBTRACKSIZE, 1);
  if (!nibStream) return false;
  if (!readRawNibStream(phystrack, nibStream)) { free(nibStream); return false; }

  nibErr rc = denibblizeTrack13(nibStream, sectorData);
  free(nibStream);
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
  for (int i=0; i<160; i++) {
    if (tracks[i].dirty)
      return true;
  }
  return false;
}

