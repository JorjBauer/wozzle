#ifndef __VENT_H
#define __VENT_H

/* ProDOS volume entry object */

#include <stdint.h>
#include <time.h>

struct _subdirent {
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
  union {
    struct {
      uint8_t pointer[2]; // pointer to directory's parent dir block
    } subdirParent;
    struct {
      uint8_t pointer[2]; // pointer to volume bitmap block
    } volBitmap;
  };
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
struct _prodosFent {
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

struct _dosFdEntry {
  uint8_t firstTrack;
  uint8_t firstSector;
  uint8_t fileTypeAndFlags;
  char fileName[30];
  uint8_t fileLength[2]; // low first                                           
};

struct _tsPair {
  uint8_t track;
  uint8_t sector;
};

struct _dosTsList {
  uint8_t unused;
  uint8_t nextTrack;
  uint8_t nextSector;
  uint8_t unused2[2];
  uint8_t sectorOffset[2]; // low first?                                        
  uint8_t unused3[5];
  struct _tsPair tsPair[122];
};


class Vent {
 public:
  Vent();
  Vent(struct _prodosFent *fi);
  Vent(struct _dosFdEntry *fe);
  Vent(struct _subdirent *fi);
  Vent(const Vent &vi);
  ~Vent();

  void Dump();

  Vent *nextEnt();
  void nextEnt(Vent *);
  Vent *childrenEnt();
  void childrenEnt(Vent *);

  bool isDirectory();
  uint16_t keyPointerVal();

  const char *getName();

  uint8_t getFirstTrack();
  uint8_t getFirstSector();

 private:
  bool isDirectoryHeader;
  bool isDos33;
  
  // General and file-related data
  uint8_t entryType;
  char name[31]; // dos are 30; prodos are 15
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

  // directory-related data
  uint16_t activeFileCount;

  // dos33 pointers
  uint8_t firstTrack;
  uint8_t firstSector;
};

#endif
