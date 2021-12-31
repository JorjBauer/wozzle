#ifndef __DOSSPECTOR_H
#define __DOSSPECTOR_H

#include "wozspector.h"
class Vent;

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

class DosSpector : public Wozspector {
 public:
  DosSpector(bool verbose, uint8_t dumpflags);
  ~DosSpector();

  virtual uint32_t getFileContents(Vent *e, char **toWhere);
  virtual uint8_t applesoftHeaderBytes() { return 2; };

  virtual bool writeFileToImage(uint8_t *fileContents,
                                char *fileName,
                                char fileType,
                                uint16_t fileStart,
                                uint16_t fileSize);

  virtual void displayInfo();

protected:
  virtual Vent *createTree();
  bool flushFreeSectorList();
  bool findFreeSector(int *trackOut, int *sectorOut);
  bool addDirectoryEntryForFile(struct _dosFdEntry *e);
  
protected:
  bool trackSectorUsedMap[35][16];

private:
  struct _vtoc vt;
};

#endif
