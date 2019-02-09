#ifndef __WOZ_H
#define __WOZ_H
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include "nib.h"

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

enum { 
  T_AUTO = 0,
  T_WOZ = 1,
  T_NIB = 2,
  T_DSK = 3,
  T_PO = 4
};

class Woz {
 public:
  Woz();
  ~Woz();

  bool readFile(const char *filename, uint8_t forceType = T_AUTO);
  bool writeFile(uint8_t version, const char *filename);

  uint8_t getNextWozBit(uint8_t track);

  bool decodeWozTrackToNib(uint8_t track, nibSector sectorData[16]);
  bool decodeWozTrackToDsk(uint8_t track, uint8_t subtype, uint8_t sectorData[256*16]);

  void dumpInfo();

  bool isSynchronized();

 private:
  bool readWozFile(const char *filename);
  bool readDskFile(const char *filename, uint8_t subtype);
  bool readNibFile(const char *filename);

  uint8_t fakeBit();
  uint8_t nextDiskBit(uint8_t track);
  uint8_t nextDiskByte(uint8_t track);

  bool parseTRKSChunk(FILE *f, uint32_t chunkSize);
  bool parseTMAPChunk(FILE *f, uint32_t chunkSize);
  bool parseInfoChunk(FILE *f, uint32_t chunkSize);
  bool parseMetaChunk(FILE *f, uint32_t chunkSize);

  bool writeInfoChunk(uint8_t version, FILE *f);
  bool writeTMAPChunk(uint8_t version, FILE *f);
  bool writeTRKSChunk(uint8_t version, FILE *f);

  bool readQuarterTrackData(FILE *f, uint8_t quartertrack);
  bool readSectorData(uint8_t track, uint8_t sector, nibSector *sectorData);

  void _initInfo();

 private:
  uint8_t quarterTrackMap[40*4];
  diskInfo di;
  trackInfo tracks[160];

  // cursor for track enumeration
  uint32_t trackPointer;
  uint8_t trackByte;
  uint8_t trackBitIdx;
  uint8_t trackLoopCounter;
  char *metaData;
};

#endif
