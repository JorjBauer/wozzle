#ifndef __WOZ_H
#define __WOZ_H
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include "nibutil.h"
#include "disktypes.h"

#define DUMP_TOFILE        0x01
#define DUMP_QTMAP         0x02
#define DUMP_QTCRC         0x04

#define DUMP_TRACK         0x10
#define DUMP_SECTOR        0x20
#define DUMP_RAWTRACK      0x40
#define DUMP_ORDEREDSECTOR 0x80


typedef struct _diskInfo {
  uint8_t version;          // Woz format version #
  uint8_t diskType;         // 1 = 5.25"; 2 = 3.5"
  uint8_t writeProtected;   // 1 = true
  uint8_t synchronized;     // were tracks written with cross-track sync?
  uint8_t cleaned;          // were MC3470 "fake bits" removed?
  char creator[33];         // 32 chars of creator string and one terminator
  uint8_t diskSides;        // always 1 for a 5.25" disk
  uint8_t bootSectorFormat; // 0=Unknown; 1=16-sector; 2=13-sector; 3=both
  uint8_t optimalBitTiming; // 125-nS increments; standard == 32 (4uS)
  uint16_t compatHardware;  // 0=unknown compatability
  uint16_t requiredRam;     // value in K. 0 = unknown
  uint16_t largestTrack;    // # of 512-byte blocks used for largest track
} diskInfo;

typedef struct _trackInfo {
  uint16_t startingBlock; // v2
  uint32_t startingByte;  // v1
  uint16_t blockCount;
  uint32_t bitCount;
  uint8_t *trackData;
} trackInfo;

class Woz {
 public:
  Woz(bool verbose, uint8_t dumpflags);
  ~Woz();

  bool readFile(const char *filename, bool preloadTracks, uint8_t forceType = T_AUTO);
  bool writeFile(uint8_t version, const char *filename);

  uint8_t getNextWozBit(uint8_t track);

  bool decodeWozTrackToNib(uint8_t track, nibSector sectorData[16]);
  bool decodeWozTrackToDsk(uint8_t track, uint8_t subtype, uint8_t sectorData[256*16]);
  bool checksumWozTrack(uint8_t track, uint32_t *retCRC);

  void dumpInfo();

  bool isSynchronized();

  uint8_t trackNumberForQuarterTrack(uint16_t qt);
  
  bool flush();
  
 private:
  bool readWozFile(const char *filename, bool preloadTracks);
  bool readDskFile(const char *filename, bool preloadTracks, uint8_t subtype);
  bool readNibFile(const char *filename, bool preloadTracks);

  uint8_t fakeBit();
  uint8_t nextDiskBit(uint8_t track);
  uint8_t nextDiskByte(uint8_t track);

  bool writeNextWozBit(int fd, uint8_t track, uint8_t bit);
  bool writeNextWozByte(int fd, uint8_t track, uint8_t b);
  
  bool parseTRKSChunk(int fd, uint32_t chunkSize);
  bool parseTMAPChunk(int fd, uint32_t chunkSize);
  bool parseInfoChunk(int fd, uint32_t chunkSize);
  bool parseMetaChunk(int fd, uint32_t chunkSize);

  bool writeInfoChunk(uint8_t version, int fd);
  bool writeTMAPChunk(uint8_t version, int fd);
  bool writeTRKSChunk(uint8_t version, int fd);

  bool readQuarterTrackData(int fd, uint8_t quartertrack);
  bool readWozTrackData(int8_t fh, uint8_t wt);
  bool readSectorData(uint8_t track, uint8_t sector, nibSector *sectorData);

  bool readAndDecodeTrack(uint8_t track, int8_t fh);
  
  void _initInfo();

 private:
  uint8_t imageType;
  
  bool verbose;
  uint8_t dumpflags;

  bool autoFlushTrackData;
  bool trackDirty;
  
  uint8_t quarterTrackMap[40*4];
  diskInfo di;
  trackInfo tracks[160];

  // cursor for track enumeration
  uint32_t trackPointer;
  uint32_t trackBitCounter;
  uint8_t trackByte;
  uint8_t trackBitIdx;
  uint8_t trackLoopCounter;
  char *metaData;
  uint8_t randData, randPtr;
};

#endif
