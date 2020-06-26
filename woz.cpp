#include "woz.h"
#include <string.h>
#include "crc32.h"
#include "nibutil.h"
#include "version.h"

// Block number we start packing data bits after (Woz 2.0 images)
#define STARTBLOCK 3

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
  uint32_t chunksize = endpos - curpos;     \
  if (!write32(fd, chunksize))               \
    return false;                           \
  lseek(fd, 0, SEEK_END);                   \
  }

Woz::Woz(bool verbose, uint8_t dumpflags)
{
  trackPointer = 0;
  trackBitIdx = 0x80;
  trackBitCounter = 0;
  trackLoopCounter = 0;
  imageType = T_AUTO;
  metaData = NULL;
  this->verbose = verbose;
  this->dumpflags = dumpflags;

  memset(&quarterTrackMap, 255, sizeof(quarterTrackMap));
  memset(&di, 0, sizeof(diskInfo));
  memset(&tracks, 0, sizeof(tracks));
  randPtr = 0;
}

Woz::~Woz()
{
  // FIXME: free all the stuff
}

bool Woz::writeNextWozBit(int fd, uint8_t track, uint8_t bit)
{
  if (track == 0xFF) {
    printf("ERROR: tried to write bit on half-track; not implemented\n");
    return true;
  }

  if (!tracks[track].trackData) {
    printf("loading (woz) track %d\n", track);
    readAndDecodeTrack(track, fd);
  }
  
  if (trackBitCounter >= tracks[track].bitCount) {
    printf("WRITE counter reset [%u > %u]\n", trackBitCounter, tracks[track].bitCount);
    trackPointer = 0;
    trackBitIdx = 0x80;
    trackBitCounter = 0;
  }
  
  if (trackBitIdx == 0x80) {
    trackByte = tracks[track].trackData[trackPointer++];
  }
  
  if (bit)
    trackByte |= trackBitIdx;
  else
    trackByte &= ~trackBitIdx;
  
  tracks[track].trackData[trackPointer-1] = trackByte;
  trackBitCounter++;
  
  trackDirty = true;
  
  trackBitIdx >>= 1;
  if (!trackBitIdx) {
    trackBitIdx = 0x80;
  }
  
  return true;
}

bool Woz::writeNextWozByte(int fd, uint8_t track, uint8_t b)
{
  if (track == 0xFF) {
    // Not on a track, so pretend to write but throw it away. FIXME:
    // probably want to create a new Woz track entry here.
    printf("ERROR: tried to write to a half track; not implemented\n");
    return true;
  }
  
  // We could be byte-aligned, but it's not guaranteed, so this
  // handles it bitwise.
  printf("track %d write byte 0x%.2X @ ptr[%d] bitidx==0x%.2X ctr=%d\n", track, b, trackPointer, trackBitIdx, trackBitCounter);

  // Debugging: aligning to bytes so I can see the effective bitstream
  if (trackBitIdx != 0x80) {
    while (trackBitIdx) {
      trackBitCounter++;
      trackBitIdx >>= 1;
    }
    trackBitIdx = 0x80;
  }
  // end debugging

  for (uint8_t i=0; i<8; i++) {
    writeNextWozBit(fd, track, b & (1 << (7-i)) ? 1 : 0);
  }
  return true;
}


uint8_t Woz::getNextWozBit(uint8_t track)
{
  if (trackBitIdx == 0x80) {
    // need another byte out of the track stream
    if (tracks[track].trackData) {
      trackByte = tracks[track].trackData[trackPointer++];
      if (trackPointer >= tracks[track].bitCount / 8) {
	trackPointer = 0;
	trackLoopCounter++;
      }
    } else {
	trackPointer = 0;
	trackLoopCounter++;
    }
  }

  uint8_t ret = (trackByte & trackBitIdx) ? 1 : 0;

  trackBitIdx >>= 1;
  if (!trackBitIdx) {
    trackBitIdx = 0x80;
  }

  return ret;
}

uint8_t Woz::fakeBit()
{
  // 30% should be 1s, but I'm not biasing the data here, so this is
  // more like 50% 1s.

  if (randPtr == 0) {
    randPtr = 0x80;
    randData = (uint8_t) ((float)256 * rand() / (RAND_MAX + 1.0));
  }

  uint8_t ret = (randData & randPtr) ? 1 : 0;
  randPtr >>= 1;
  
  return ret;
}

uint8_t Woz::nextDiskBit(uint8_t track)
{
  static uint8_t head_window = 0;
  head_window <<= 1;
  head_window |= getNextWozBit(track);
  if ((head_window & 0x0f) != 0x00) {
    return (head_window & 0x02) >> 1;
  } else {
    return fakeBit();
  }
}

uint8_t Woz::nextDiskByte(uint8_t track)
{
  uint8_t d = 0;
  while ((d & 0x80) == 0) {
    d <<= 1;
    d |= nextDiskBit(track);
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

bool Woz::writeFile(uint8_t version, const char *filename)
{
  int fd = NULL;
  bool retval = false;
  uint32_t tmp32; // scratch 32-bit value
  off_t crcPos, endPos;
  off_t curpos; // used in macros to dynamically tell what size the chunks are
  uint32_t crcDataSize;
  uint8_t *crcData = NULL;


  if (version > 2 || !version) {
    fprintf(stderr, "ERROR: version must be 1 or 2\n");
    goto done;
  }

  fd = open(filename, O_TRUNC|O_CREAT|O_RDWR);
  if (fd == -1) {
    perror("ERROR: Unable to open output file");
    goto done;
  }
  
  // header
  if (version == 1) {
    tmp32 = 0x315A4F57;
  } else {
    tmp32 = 0x325A4F57;
  }
  if (!write32(fd, tmp32)) {
    fprintf(stderr, "ERROR: failed to write\n");
    goto done;
  }
  tmp32 = 0x0A0D0AFF;
  if (!write32(fd, tmp32)) {
    fprintf(stderr, "ERROR: failed to write\n");
    goto done;
  }

  // We'll come back and write the checksum later
  crcPos = lseek(fd, 0, SEEK_CUR);
  tmp32 = 0;
  if (!write32(fd, tmp32)) {
    fprintf(stderr, "ERROR: failed to write\n");
    goto done;
  }

  PREP_SECTION(fd, 0x4F464E49); // 'INFO'
  if (!writeInfoChunk(version, fd)) {
    fprintf(stderr, "ERROR: failed to write INFO chunk\n");
    goto done;
  }
  END_SECTION(fd);

  PREP_SECTION(fd, 0x50414D54); // 'TMAP'
  if (!writeTMAPChunk(version, fd)) {
    fprintf(stderr, "ERROR: failed to write TMAP chunk\n");
    goto done;
  }
  END_SECTION(fd);

  PREP_SECTION(fd, 0x534B5254); // 'TRKS'
  if (!writeTRKSChunk(version, fd)) {
    fprintf(stderr, "ERROR: failed to write TRKS chunk\n");
    goto done;
  }
  END_SECTION(fd);

  // Write the metadata if we have any
  if (metaData) {
    PREP_SECTION(fd, 0x4154454D); // 'META'
    if (write(fd, metaData, strlen(metaData)) != strlen(metaData)) {
      fprintf(stderr, "ERROR: failed to write META chunk\n");
      goto done;
    }
    END_SECTION(fd);
  }

  // FIXME: missing the WRIT chunk, if it exists

  // Fix up the checksum
  endPos = lseek(fd, 0, SEEK_CUR);
  crcDataSize = endPos-crcPos-4;
  crcData = (uint8_t *)malloc(crcDataSize);
  if (!crcData) {
    fprintf(stderr, "ERROR: failed to malloc crc data chunk\n");
    goto done;
  }
    
  // Read the data in for checksumming
  if (lseek(fd, crcPos+4, SEEK_SET) == -1) {
    fprintf(stderr, "ERROR: failed to fseek to crcPos+4 (0x%llX)\n", crcPos+4);
    goto done;
  }

  tmp32 = read(fd, crcData, crcDataSize);
  if (tmp32 != crcDataSize) {
    fprintf(stderr, "ERROR: failed to read in data for checksum [read %d, wanted %d]\n", tmp32, crcDataSize);
    goto done;
  }
    
  tmp32 = compute_crc_32(crcData, crcDataSize);
  // Write it back out
  lseek(fd, crcPos, SEEK_SET);
  if (!write32(fd, tmp32)) {
    fprintf(stderr, "ERROR: failed to write CRC\n");
    goto done;
  }

  retval = true;

 done:
  if (crcData)
    free(crcData);
  if (fd != -1)
    close(fd);
  return retval;
}

void Woz::_initInfo()
{
  di.version = 2;
  di.diskType = 1;
  di.writeProtected = 0;
  di.synchronized = 0;
  di.cleaned = 0;
  sprintf(di.creator, "%.32s", VERSION_STRING);
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

bool Woz::readAndDecodeTrack(uint8_t track, int8_t fd)
{
  // If we're going to malloc a new one, then find all the other ones
  // that might be malloc'd and purge them if we're
  // autoFlushTrackData==true
  if (autoFlushTrackData == true) {
    printf("auto-flushing track data\n");
    
    if (trackDirty) {
      printf("Hackily writing /tmp/auto.woz\n");
      trackDirty = false;
      writeFile(2, "/tmp/auto.woz"); // FIXME: debugging
    }
    
    for (int i=0; i<160; i++) {
      if (tracks[i].trackData) {
        free(tracks[i].trackData);
        tracks[i].trackData = NULL;
      }
    }
  }
  
  if (imageType == T_WOZ) {
    return readWozTrackData(fd, track);
  } else if (imageType == T_PO ||
	     imageType == T_DSK) {
    static uint8_t sectorData[256*16];

    lseek(fd, 256*16*track, SEEK_SET);

    // FIXME: no error checking
    read(fd, sectorData, 256*16);
    
    tracks[track].trackData = (uint8_t *)calloc(NIBTRACKSIZE, 1);
    if (!tracks[track].trackData) {
      fprintf(stderr, "Failed to malloc track data\n");
      return false;
    }
    tracks[track].startingBlock = STARTBLOCK + 13*track;
    tracks[track].blockCount = 13;
    uint32_t sizeInBits = nibblizeTrack(tracks[track].trackData, sectorData, imageType, track);
    tracks[track].bitCount = sizeInBits; // ... reality.

    return true;
  }
  else if (imageType == T_NIB) {
    tracks[track].trackData = (uint8_t *)malloc(NIBTRACKSIZE);
    if (!tracks[track].trackData) {
      printf("Failed to malloc track data\n");
      return false;
    }

    lseek(fd, NIBTRACKSIZE * track, SEEK_SET);
    read(fd, tracks[track].trackData, NIBTRACKSIZE);
      // FIXME: no error checking
    
    tracks[track].startingBlock = STARTBLOCK + 13*track;
    tracks[track].blockCount = 13;
    tracks[track].bitCount = NIBTRACKSIZE*8;
    
    return true;
  }
  
  printf("ERROR: don't know how we reached this point\n");
  return false;
}

bool Woz::readDskFile(const char *filename, bool preloadTracks, uint8_t subtype)
{
  bool retval = false;
  autoFlushTrackData = !preloadTracks;
  imageType = subtype;

  int fd = open(filename, O_RDONLY);
  if (fd == -1) {
    perror("Unable to open input file");
    goto done;
  }

  _initInfo();

  // Now read in the 35 tracks of data from the DSK file and convert them to NIB
  uint8_t sectorData[256*16];
  for (int track=0; track<35; track++) {
    uint32_t bytesRead = read(fd, sectorData, 256*16);
      if (bytesRead != 256*16) {
	fprintf(stderr, "Failed to read DSK data; got %d bytes, wanted %d\n", bytesRead, 256);
	goto done;
      }

      tracks[track].trackData = (uint8_t *)calloc(NIBTRACKSIZE, 1);
      if (!tracks[track].trackData) {
	fprintf(stderr, "Failed to malloc track data\n");
	goto done;
      }
      tracks[track].startingBlock = STARTBLOCK + 13*track;
      tracks[track].blockCount = 13;
      uint32_t sizeInBits = nibblizeTrack(tracks[track].trackData, sectorData, subtype, track);
      tracks[track].bitCount = sizeInBits; // ... reality.
  }

  retval = true;

 done:
  if (fd != -1)
    close(fd);
  return retval;
}

bool Woz::readNibFile(const char *filename, bool preloadTracks)
{
  autoFlushTrackData = !preloadTracks;
  imageType = T_NIB;

  int fd = open(filename, O_RDONLY);
  if (fd == -1) {
    perror("Unable to open input file");
    return false;
  }
  
  _initInfo();

  // Now read in the 35 tracks of data from the nib file
  nibSector nibData[16];
  for (int track=0; track<35; track++) {
    uint32_t bytesRead = read(fd, nibData, NIBTRACKSIZE);
    if (bytesRead != NIBTRACKSIZE) {
      printf("Failed to read NIB data; got %d bytes, wanted %d\n", bytesRead, NIBTRACKSIZE);
      return false;
    }
    
    tracks[track].trackData = (uint8_t *)calloc(NIBTRACKSIZE, 1);
    if (!tracks[track].trackData) {
      printf("Failed to malloc track data\n");
      return false;
    }
    
    memcpy(tracks[track].trackData, nibData, NIBTRACKSIZE);
    tracks[track].startingBlock = STARTBLOCK + 13*track;
    tracks[track].blockCount = 13;
    tracks[track].bitCount = NIBTRACKSIZE*8;
  }
  close(fd);

  return true;
}

bool Woz::readWozFile(const char *filename, bool preloadTracks)
{
  imageType = T_WOZ;
  autoFlushTrackData = !preloadTracks;
  
  int fd = open(filename, O_RDONLY);
  if (fd == -1) {
    perror("Unable to open input file");
    return false;
  }

  // Header
  uint32_t h;
  read32(fd, &h);
  if (h == 0x325A4F57 || h == 0x315A4F57) {
    if (verbose) {
      printf("WOZ%c disk image\n", (h & 0xFF000000)>>24);
    }
  } else {
    printf("Unknown disk image type; can't continue\n");
    close(fd);
    return false;
  }

  uint32_t tmp;
  if (!read32(fd, &tmp)) {
    printf("Read failure\n");
    close(fd);
    return false;
  }
  if (tmp != 0x0A0D0AFF) {
    printf("WOZ header failure; exiting\n");
    close(fd);
    return false;
  }
  uint32_t crc32;
  read32(fd, &crc32);
  // If CRC is set, then check it
  if (crc32) {
    // FIXME: check CRC
    if (verbose) {
      printf("Disk crc32 should be 0x%X\n", crc32);
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
      printf("ERROR: data size < 0?\n");
      exit(1);
    }

    bool isOk;

    switch (chunkType) {
    case 0x4F464E49: // 'INFO'
      if (verbose) {
	printf("Reading INFO chunk\n");
      }
      isOk = parseInfoChunk(fd, chunkDataSize);
      haveData |= cINFO;
      break;
    case 0x50414D54: // 'TMAP'
      if (verbose) {
	printf("Reading TMAP chunk\n");
      }
      isOk = parseTMAPChunk(fd, chunkDataSize);
      haveData |= cTMAP;
      break;
    case 0x534B5254: // 'TRKS'
      if (verbose) {
	printf("Reading TRKS chunk\n");
      }
      isOk = parseTRKSChunk(fd, chunkDataSize);
      haveData |= cTRKS;
      break;
    case 0x4154454D: // 'META'
      if (verbose) {
	printf("Reading META chunk\n");
      }	  
      isOk = parseMetaChunk(fd, chunkDataSize);
      break;
    default:
      printf("Unknown chunk type 0x%X\n", chunkType);
      close(fd);
      return false;
      break;
    }

    if (!isOk) {
      printf("Chunk parsing [0x%X] failed; exiting\n", chunkType);
      close(fd);
      return false;
    }
    fpos += chunkDataSize + 8; // 8 bytes for the ChunkID and the ChunkSize
  }

  if (haveData != 0x07) {
    printf("ERROR: missing one or more critical sections\n");
    return false;
  }

  // For a Woz file, we need to read *every* quarter-track; and if we've
  // already got the target track's data, we don't need to re-read it.

  for (int i=0; i<160; i++) {
    if (!readQuarterTrackData(fd, i)) {
      printf("Failed to read QTD for quartertrack %d\n", i);
      close(fd);
      return false;
    }
  }

  close(fd);
  return true;
}

bool Woz::readFile(const char *filename, bool preloadTracks, uint8_t forceType)
{
  if (forceType == T_AUTO) {
    // Try to determine type from the file extension
    const char *p = strrchr(filename, '.');
    if (!p) {
      printf("Unable to determine file type of '%s'\n", filename);
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
    } else {
      printf("Unable to determine file type of '%s'\n", filename);
      return false;
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
  default:
    printf("Unknown disk type; unable to read\n");
    return false;
  }
}

bool Woz::parseTRKSChunk(int fd, uint32_t chunkSize)
{
  if (di.version == 2) {
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
      printf("Track %d: read %d bits\n", trackNumber, numBits);
    }
    if (numBits > 6656 * 8) {
      fprintf(stderr, "WARNING: track %d looks like it's too long (%d bits > 6656 bytes)?\n", trackNumber, numBits);
    }
    tracks[trackNumber].bitCount = numBits;
    ptr += 6656;
    trackNumber++;
  }

  return true;
}

bool Woz::parseTMAPChunk(int fd, uint32_t chunkSize)
{
  if (chunkSize != 0xa0) {
    printf("TMAP chunk is the wrong size; aborting\n");
    return false;
  }

  for (int i=0; i<40*4; i++) {
    if (!read8(fd, (uint8_t *)&quarterTrackMap[i]))
      return false;
    chunkSize--;
  }
  if (verbose){
    printf("Read quarter-track map:\n");
    for (int i=0; i<140; i+=4) {
      printf("%2d     %3d => %3d     %3d => %3d     %3d => %3d     %3d => %3d\n",
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
bool Woz::parseInfoChunk(int fd, uint32_t chunkSize)
{
  if (chunkSize != 60) {
    fprintf(stderr, "INFO chunk size is not 60; aborting\n");
    return false;
  }

  if (!read8(fd, &di.version))
    return false;
  if (di.version > 2) {
    fprintf(stderr, "Incorrect version header; aborting\n");
    return false;
  }

  if (!read8(fd, &di.diskType))
    return false;
  if (di.diskType != 1) {
    fprintf(stderr, "Not a 5.25\" disk image; aborting\n");
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

  return true;
}

bool Woz::parseMetaChunk(int fd, uint32_t chunkSize)
{
  metaData = (char *)calloc(chunkSize+1, 1);
  if (!metaData)
    return false;

  if (read(fd, metaData, chunkSize) != chunkSize)
    return false;

  metaData[chunkSize] = 0;

  return true;
}

bool Woz::readWozTrackData(int8_t fd, uint8_t wt)
{
  // assume if it's malloc'd, then we've already read it
  if (tracks[wt].trackData)
    return true;

  uint16_t bitsStartBlock = tracks[wt].startingBlock;

  // Allocate a new buffer for this track
  uint32_t count = tracks[wt].blockCount * 512;
  if (di.version == 1) count = (tracks[wt].bitCount / 8) + ((tracks[wt].bitCount % 8) ? 1 : 0);
  tracks[wt].trackData = (uint8_t *)calloc(count, 1);
  if (!tracks[wt].trackData) {
    perror("Failed to alloc buf to read track magnetic data");
    return false;
  }

  if (di.version == 1) {
    lseek(fd, tracks[wt].startingByte, SEEK_SET); // FIXME: error checking
  } else {
    lseek(fd, bitsStartBlock*512, SEEK_SET); // FIXME: error checking
  }
  read(fd, tracks[wt].trackData, count);

  return true;
}


bool Woz::readQuarterTrackData(int fd, uint8_t quartertrack)
{
  uint8_t targetImageTrack = quarterTrackMap[quartertrack];
  if (targetImageTrack == 0xFF) {
    // It's a tween-track with no reliable data.
    return true;
  }

  uint16_t bitsStartBlock = tracks[targetImageTrack].startingBlock;

  //  if (tracks[targetImageTrack].trackData)
  //    free(tracks[targetImageTrack].trackData);

  // Allocate a new buffer for this track
  uint32_t count = tracks[targetImageTrack].blockCount * 512;
  if (di.version == 1) count = (tracks[targetImageTrack].bitCount / 8) + ((tracks[targetImageTrack].bitCount % 8) ? 1 : 0);
  if (tracks[targetImageTrack].trackData) {
    return true; // We've already read this track's data; don't re-read it
  }
  tracks[targetImageTrack].trackData = (uint8_t *)calloc(count, 1);
  if (!tracks[targetImageTrack].trackData) {
    perror("Failed to alloc buf to read track magnetic data");
    return false;
  }

  if (di.version == 1) {
    if (lseek(fd, tracks[targetImageTrack].startingByte, SEEK_SET) == -1) {
      perror("Failed to seek to start of block");
      return false;
    }
  } else {
    if (lseek(fd, bitsStartBlock*512, SEEK_SET) == -1) {
      perror("Failed to seek to start of block");
      return false;
    }
  }
  uint32_t didRead = read(fd, tracks[targetImageTrack].trackData, count);
  if (verbose) {
    printf("Read %u bytes of data for track %d\n",
	   didRead, targetImageTrack);
  }
  if (didRead != count) {
    printf("Failed to read all track data for track [read %d, wanted %d]\n", didRead, count);
    return false;
  }

  return true;
}


bool Woz::readSectorData(uint8_t track, uint8_t sector, nibSector *sectorData)
{
  // Find the sector header for this sector...
  uint32_t ptr = 0;

  memset(sectorData->gap1, 0xFF, sizeof(sectorData->gap1));
  memset(sectorData->gap2, 0xFF, sizeof(sectorData->gap1));

  // Allow two loops through the track data looking for the sector prolog
  uint32_t endCount = tracks[track].blockCount*512*2;
  if (di.version == 1) endCount = 2*6646;
  while (ptr < endCount) {
    sectorData->sectorProlog[0] = sectorData->sectorProlog[1];
    sectorData->sectorProlog[1] = sectorData->sectorProlog[2];
    sectorData->sectorProlog[2] = nextDiskByte(track);
    ptr++;
    
    if (sectorData->sectorProlog[0] == 0xd5 &&
	sectorData->sectorProlog[1] == 0xaa &&
	sectorData->sectorProlog[2] == 0x96) {
      // Found *a* sector header. See if it's ours.
      sectorData->volume44[0] = nextDiskByte(track);
      sectorData->volume44[1] = nextDiskByte(track);
      sectorData->track44[0] = nextDiskByte(track);
      sectorData->track44[1] = nextDiskByte(track);
      sectorData->sector44[0] = nextDiskByte(track);
      sectorData->sector44[1] = nextDiskByte(track);
      sectorData->checksum44[0] = nextDiskByte(track);
      sectorData->checksum44[1] = nextDiskByte(track);
      sectorData->sectorEpilog[0] = nextDiskByte(track);
      sectorData->sectorEpilog[1] = nextDiskByte(track);
      sectorData->sectorEpilog[2] = nextDiskByte(track);

      if (sectorData->sectorEpilog[0] == 0xde &&
	  sectorData->sectorEpilog[1] == 0xaa &&
	  sectorData->sectorEpilog[2] == 0xeb) {
	// Header is integral. See if it's our sector:
	uint8_t sectorNum = de44(sectorData->sector44);
	if (sectorNum != sector) {
	  continue;
	}
	// It's our sector - find the data chunk and read it
	while (ptr < tracks[track].blockCount*512*2) {
	  sectorData->dataProlog[0] = sectorData->dataProlog[1];
	  sectorData->dataProlog[1] = sectorData->dataProlog[2];
	  sectorData->dataProlog[2] = nextDiskByte(track);
	  ptr++;

	  if (sectorData->dataProlog[0] == 0xd5 &&
	      sectorData->dataProlog[1] == 0xaa &&
	      sectorData->dataProlog[2] == 0xad) {
	    // Found the data; copy it in
	    for (int i=0; i<342; i++) {
	      sectorData->data62[i] = nextDiskByte(track);
	    }
	    sectorData->checksum = nextDiskByte(track);
	    sectorData->dataEpilog[0] = nextDiskByte(track);
	    sectorData->dataEpilog[1] = nextDiskByte(track);
	    sectorData->dataEpilog[2] = nextDiskByte(track);
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

bool Woz::writeInfoChunk(uint8_t version, int fd)
{
  if (!write8(fd, version) ||
      !write8(fd, di.diskType) ||
      !write8(fd, di.writeProtected) ||
      !write8(fd, di.synchronized) ||
      !write8(fd, di.cleaned))
    return false;

  for (int i=0; i<32; i++) {
    if (!write8(fd, di.creator[i]))
      return false;
  }
  
  if (version >= 2) {
    // If we read a Wozv1, this will be set to 0. Set it to 1.
    if (di.diskSides == 0)
      di.diskSides = 1;

    if ( !write8(fd, di.diskSides) ||
	 !write8(fd, di.bootSectorFormat) ||
	 !write8(fd, di.optimalBitTiming) ||
	 !write16(fd, di.compatHardware) ||
	 !write16(fd, di.requiredRam) ||
	 !write16(fd, di.largestTrack))
      return false;
  }

  // Padding
  for (int i=0; i<((version==1)?23:14); i++) {
    if (!write8(fd, 0))
      return false;
  }
  return true;
}

bool Woz::writeTMAPChunk(uint8_t version, int fd)
{
  for (int i=0; i<40*4; i++) {
    if (!write8(fd, quarterTrackMap[i]))
      return false;
  }

  return true;
}

bool Woz::writeTRKSChunk(uint8_t version, int fd)
{
  if (version == 1) {
    printf("V1 write is not implemented\n");
    return false;
  }

  // Reconstruct all of the starting blocks/blockCounts for each
  // track. The bitCount should be correct.
  uint8_t numTracksPacked = 0;
  for (int i=0; i<160; i++) {
    if (tracks[i].trackData) {
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

    if (!write16(fd, tracks[i].startingBlock))
      return false;
    if (!write16(fd, tracks[i].blockCount))
      return false;
    if (!write32(fd, tracks[i].bitCount))
      return false;
  }

  // All the track data
  for (int i=0; i<160; i++) {
    if (tracks[i].startingBlock &&
	tracks[i].blockCount) {
      lseek(fd, tracks[i].startingBlock * 512, SEEK_SET);
      uint32_t writeSize = (tracks[i].bitCount / 8) + ((tracks[i].bitCount % 8) ? 1 : 0);
      if (write(fd, tracks[i].trackData, writeSize) != writeSize)
	return false;
      uint8_t c = 0;
      while (writeSize < tracks[i].blockCount * 512) {
	if (write(fd, &c, 1) != 1)
	  return false;
	writeSize++;
      }
    }
  }
  return true;
}

bool Woz::decodeWozTrackToNib(uint8_t track, nibSector sectorData[16])
{
  for (int sector=0; sector<16; sector++) {
    if (!readSectorData(track, sector, (nibSector *)(&sectorData[sector]))) {
      return false;
    }
  }

  return true;
}

bool Woz::decodeWozTrackToDsk(uint8_t track, uint8_t subtype, uint8_t sectorData[256*16])
{
  // First read it to a NIB; then convert the NIB to a DSK.
  nibSector nibData[16];
  if (!decodeWozTrackToNib(track, nibData))
    return false;

  if (denibblizeTrack((const uint8_t *)nibData, sectorData, subtype, track) != errorNone)
    return false;

  return true;
}

bool Woz::checksumWozTrack(uint8_t track, uint32_t *retCRC)
{
  if (!retCRC)
    return false;

  if (!tracks[track].trackData) {
    *retCRC = 0;
    return false;
  }
  
  *retCRC = compute_crc_32(tracks[track].trackData, tracks[track].bitCount/8);
  return true;
}



void Woz::dumpInfo()
{
  printf("WOZ image version %d\n", di.version);
  printf("Disk type: %s\n", di.diskType == 1 ? "5.25\"" : "3.5\"");
  printf("Write protected: %s\n", di.writeProtected ? "yes" : "no");
  printf("Synchronized: %s\n", di.synchronized ? "yes" : "no");
  printf("Cleaned: %s\n", di.cleaned ? "yes" : "no");
  printf("Creator: %s\n", di.creator);
  printf("Disk sides: %d\n", di.diskSides);
  printf("Boot sector format: ");
  switch (di.bootSectorFormat) {
  case 0:
  default:
    printf("unknown\n");
    break;
  case 1:
    printf("16 sector\n");
    break;
  case 2: 
    printf("13 sector\n");
    break;
  case 3:
    printf("Both 16 and 13 sector\n");
    break;
  }
  printf("Optimal bit timing: %d ns\n", di.optimalBitTiming * 125);
  printf("Hardware compatability flags: 0x%X\n", di.compatHardware);
  printf("Required RAM: %d K\n", di.requiredRam);
  printf("Largest track: %d bytes\n", di.largestTrack * 512);
  printf("\n");
  
  if (metaData) {
    printf("Metadata:\n");

    char *token, *string, *tofree;
    tofree = string = strdup(metaData);
    char *parts[25];
    memset(parts, 0, sizeof(parts));
    int idx = 0;
    while ((token = strsep(&string, "\n")) != NULL) {
      if (idx >= sizeof(parts)) {
	printf("ERROR: too many metadata keys\n");
	return;
      }
      parts[idx++] = strdup(token);
    }
    free(tofree);

    for (int idx2=0; idx2<idx; idx2++) {
      if (parts[idx2] && strlen(parts[idx2])) {
	char *p = strchr(parts[idx2], '\t');
	if (!p) {
	  printf("ERROR: no delineator on a line of metadata [%s]\n", parts[idx2]);
		 return;
		 }
	    *(p++) = 0;
	  if (strlen(p)) {
	    printf("   %s: %s\n", parts[idx2], p);
	  }
      }
    }

    while (--idx >= 0) {
      free(parts[idx]);
    }
    printf("\n");
  }

  if (dumpflags & DUMP_QTMAP) {
    printf("Quarter-track map:\n");
    for (int i=0; i<140; i+=4) {
      printf("%2d     %3d => %3d     %3d => %3d     %3d => %3d     %3d => %3d\n",
	     i/4,
	     i, quarterTrackMap[i],
	     i+1, quarterTrackMap[i+1],
	     i+2, quarterTrackMap[i+2],
	     i+3, quarterTrackMap[i+3]);
    }
  }
  
  if (dumpflags & DUMP_QTCRC) {
    printf("Woz internal quarter-track CRCs:\n");
    // Dump the CRC32 for each Woz quarter-track
    for (int i=0 ;i<160; i++) {
      uint32_t crc=0;
      checksumWozTrack(i, &crc);
      printf("Woz track %d CRC32: 0x%X\n", i, crc);
    }
  }

  if (dumpflags & DUMP_TRACK) {
    for (int i=0; i<40; i++) {
      printf("Track %d:\n", i);
      if (di.version == 1) {
	printf("  Starts at byte %d\n", tracks[i].startingByte);
      } else {
	printf("  Starts at block %d\n", tracks[i].startingBlock);
      }
      printf("  Number of blocks: %d\n", tracks[i].blockCount);
      printf("  Number of bits: %d\n", tracks[i].bitCount);
      if (tracks[i].bitCount && tracks[i].trackData) {
	if (dumpflags & DUMP_RAWTRACK) {
	  // Raw track dump
	  printf("    Raw track data:\n");
	  for (int k=0; k<(tracks[i].bitCount/8)+((tracks[i].bitCount%8)?1:0); k+=16) {
	    printf("    0x%.4X :", k);
	    for (int j=0; j<16; j++) {
	      if (k+j < (tracks[i].bitCount/8)+((tracks[i].bitCount%8)?1:0)) {
		printf(" %.2X", tracks[i].trackData[k+j]);
	      }
	    }
	    printf("\n");
	  }
	}
	
	if (dumpflags & DUMP_SECTOR)  {
	  printf("    Sector dump:\n");
	  // Look at the sectors in numerical order
	  // FIXME: 13-sector support
	  nibSector sectorData;
	  for (int sector=0; sector<16; sector++) {
	    if (readSectorData(i, sector, &sectorData)) {
	      printf("      Volume ID: %d\n", de44(sectorData.volume44));
	      printf("      Track ID: %d\n", de44(sectorData.track44));
	      uint8_t sector = de44(sectorData.sector44);
	      printf("      Sector: %d\n", sector);
	      printf("      Cksum: %d\n", de44(sectorData.checksum44));
	      
	      printf("      Sector Data:\n");
	      for (int k=0; k<342; k+=16) {
		printf("      0x%.4X :", k);
		for (int j=0; j<16; j++) {
		  if (k+j < 342) {
		    printf(" %.2X", sectorData.data62[k+j]);
		  }
		}
		printf("\n");
	      }
	    }
	  }
	}
      }
      
      if (dumpflags & DUMP_TOFILE) {
	// Dump each sector to a file for analysis
	uint8_t sectorData[256*16];
	// fixme use track map
	for (int i=0; i<35; i++) {
	  decodeWozTrackToDsk(i, T_AUTO, sectorData);
	  for (int j=0; j<16; j++) {
	    char buf[25];
	    sprintf(buf, "t%ds%d", i, j);
	    int fd = open(buf, O_WRONLY);
	    write(fd, &sectorData[256*j], 256);
	    close(fd);
	  }
	}
      }

      if (dumpflags & DUMP_ORDEREDSECTOR) {
#define denib(a, b) ((((a) & ~0xAA) << 1) | ((b) & ~0xAA))
	printf("    Track-ordered sector dump:\n");
	// Look at the sectors found in order on the track
	trackBitIdx = 0x80; trackPointer = 0; trackLoopCounter = 0;
	uint16_t sectorsFound = 0;
	do {
	  if (nextDiskByte(i) == 0xD5 &&
	      nextDiskByte(i) == 0xAA &&
	      nextDiskByte(i) == 0x96) {
	    printf("      Volume ID: %d\n", denib(nextDiskByte(i), nextDiskByte(i)));
	    printf("      Track ID: %d\n", denib(nextDiskByte(i), nextDiskByte(i)));
	    uint8_t sector = denib(nextDiskByte(i), nextDiskByte(i));
	    printf("      Sector: %d\n", sector);
	    sectorsFound |= (1 << sector);
	    printf("      Cksum: %d\n", denib(nextDiskByte(i), nextDiskByte(i)));
	    
	    nextDiskByte(i); // skip epilog
	    nextDiskByte(i);
	    nextDiskByte(i);
	    // look for data prolog d5 aa ad
	    while (nextDiskByte(i) != 0xD5 && trackLoopCounter < 2)
	      ;
	    if (trackLoopCounter < 2) {
	      // Hope that's it and skip two bytes
	      nextDiskByte(i);
	      nextDiskByte(i);
	      // Dump the 6-and-2 data
	      printf("      Sector Data:\n");
	      for (int k=0; k<342; k+=16) {
		printf("      0x%.4X :", k);
		for (int j=0; j<16; j++) {
		  if (k+j < 342) {
		    printf(" %.2X", nextDiskByte(i));
		  }
		}
		printf("\n");
	      }
	    }
	  }
	  
	} while (sectorsFound != 0xFFFF && trackLoopCounter < 2);
      }
    }
  }
}

bool Woz::isSynchronized()
{
  return di.synchronized;
}

uint8_t Woz::trackNumberForQuarterTrack(uint16_t qt)
{
  return quarterTrackMap[qt];
}

#if 0
bool Woz::flush()
{
  if (trackDirty) {
    printf("Hackily writing /tmp/auto.woz\n");
    trackDirty = false;
    return writeFile(2, "/tmp/auto.woz"); // FIXME: debugging
  }
  return true;
}
#endif
