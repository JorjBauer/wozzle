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
  virtual uint32_t getAllocatedByteCount(Vent *e);
  virtual uint32_t getFileAllocation(Vent *e, char **toWhere);
  virtual uint8_t applesoftHeaderBytes() { return 2; };

  virtual bool writeFileToImage(uint8_t *fileContents,
                                char *fileName,
                                uint8_t fileType,
                                uint16_t auxTypeData,
                                uint32_t fileSize);

  virtual void displayInfo();
  virtual void inspectFile(const char *fileName, Vent *fp);
  virtual bool probe();

protected:
  virtual Vent *createTree();
  bool flushFreeSectorList();
  bool findFreeSector(int *trackOut, int *sectorOut);
  bool addDirectoryEntryForFile(struct _dosFdEntry *e);
  // Reads a logical track into a 16-sector buffer, dispatching to the
  // 13-sector codec if the underlying WOZ says so. For 13-sector disks
  // only the first 13*256 bytes of `out` are populated.
  bool readLogicalTrack(uint8_t phystrack, uint8_t out[256*16]);

  // Reads a single DOS-logical sector. Implemented on top of
  // readLogicalTrack (slightly wasteful — decodes the whole track — but
  // correct for both 13- and 16-sector formats with one code path).
  bool readLogicalSector(uint8_t phystrack, uint8_t logsect,
                         uint8_t out[256]);
  
protected:
  bool trackSectorUsedMap[35][16];

private:
  struct _vtoc vt;
};

#endif
