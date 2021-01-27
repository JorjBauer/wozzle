#ifndef __VENT_H
#define __VENT_H

/* ProDOS volume entry object */

#include <stdint.h>
#include <time.h>

struct _subdirent {
  uint8_t prevBlock[2];
  uint8_t nextBlock[2];
  uint8_t typelen;
  char name[15];
  union {
    struct {
      uint8_t fileType; // == 0x75 for a subdirectory
    } subdir;
    struct {
      uint8_t reserved1;
    } volhdr;
  };
  uint8_t reserved[7];
  uint8_t creationDate[4];
  uint8_t creatorVersion;
  uint8_t minRequiredVersion;
  uint8_t accessFlags;
  uint8_t entryLength; // == 0x27
  uint8_t entriesPerBlock; // == 0x0D
  uint8_t fileCount[2]; // active entries (files and dirs) in this subdir
  uint8_t bitmapOrparentPointer[2]; // for the volume header, this is
				    // a pointer to the bitmap
				    // block. For subdirs, it's a
				    // pointer to the parent
				    // directory's block.
  union {
    struct {
      uint8_t entry; // entry # of this subdir in the parent dir
      uint8_t entryLength; // == 0x27
    } subParent;
    struct {
      uint8_t total[2]; // total number of blocks used
    } volBlocks;
  };
};

// Struct of how the data rests on-disk
struct _fent {
  uint8_t typelen; // high nybble: type; low nybble: length of name
  char name[15];
  uint8_t fileType;
  uint8_t keyPointer[2];
  uint8_t blocksUsed[2];
  uint8_t eofLength[3];
  uint8_t creationDate[4];
  uint8_t creatorVersion;
  uint8_t minRequiredVersion;
  uint8_t accessFlags;
  uint8_t typeData[2];
  uint8_t lastModified[4];
  uint8_t headerPointer[2];
};

class Vent {
 public:
  Vent();
  Vent(struct _fent *fi);
  Vent(struct _subdirent *fi);
  Vent(const Vent &vi);
  ~Vent();

  void Dump();

  Vent *nextEnt();
  void nextEnt(Vent *);
  Vent *childrenEnt();
  void childrenEnt(Vent *);

  // For unpacking directories...
  Vent *getNextDirEnt();

 private:
  uint8_t entryType;
  char name[16];
  uint8_t fileType;
  uint16_t keyPointer;
  uint16_t blocksUsed;
  uint32_t eofLength;
  time_t creationDate;
  uint8_t creatorVersion;
  uint8_t minRequiredVersion;
  uint8_t accessFlags;
  uint16_t typeData;
  time_t lastModified;
  uint16_t headerPointer;
  class Vent *children;
  class Vent *next;

};

#endif
