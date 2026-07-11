#ifndef __VENT_H
#define __VENT_H

/* ProDOS volume entry object */

#include <stdint.h>
#include <time.h>

enum {
 FT_TYP = 0,
 FT_BAD = 1,
 FT_TXT = 4,
 FT_BIN = 6,
 FT_DIR = 0x0F,
 FT_ADB = 0x19,
 FT_AWP = 0x1A,
 FT_ASP = 0x1B,
 FT_PAS = 0xEF,
 FT_CMD = 0xF0,
 FT_US1 = 0xF1,
 FT_US2 = 0xF2,
 FT_US3 = 0xF3,
 FT_US4 = 0xF4,
 FT_US5 = 0xF5,
 FT_US6 = 0xF6,
 FT_US7 = 0xF7,
 FT_US8 = 0xF8,
 FT_INT = 0xFA,
 FT_IVR = 0xFB,
 FT_BAS = 0xFC,
 FT_VAR = 0xFD,
 FT_REL = 0xFE,
 FT_SYS = 0xFF,
};

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

// UCSD p-System / Apple Pascal directory entry, exactly as it rests on
// disk (all multi-byte fields little-endian). Every entry is 26 bytes.
// A file's data lives on the contiguous blocks [firstBlock .. nextBlock),
// with byteCount valid bytes in the final block.
struct _pascalFent {
  uint8_t firstBlock[2]; // first block of the file
  uint8_t nextBlock[2];  // block just past the end (last block + 1)
  uint8_t fileType[2];   // low nibble = file kind (see _pascalKind)
  uint8_t name[16];      // length-prefixed, 1 + up to 15 chars
  uint8_t byteCount[2];  // bytes used in the file's last block (1..512)
  uint8_t modDate[2];    // packed Pascal date
};

// The volume header is the first 26-byte "entry" of the directory (block
// 2). It has the same size as a file entry but a different shape.
struct _pascalVolHdr {
  uint8_t firstBlock[2];    // always 0
  uint8_t nextBlock[2];     // first block after the directory (always 6)
  uint8_t fileType[2];      // always 0
  uint8_t name[8];          // length-prefixed, 1 + up to 7 chars
  uint8_t volBlockCount[2]; // total blocks in the volume
  uint8_t fileCount[2];     // number of files in the directory
  uint8_t lastAccess[2];    // (unused; declared "integer")
  uint8_t lastDateSet[2];   // most recently set system date
  uint8_t reserved[4];
};

// Pascal file kinds (the low nibble of _pascalFent.fileType).
enum _pascalKind {
  PK_UNTYPED = 0, // volume header uses this
  PK_BAD     = 1, // .BAD  - damaged blocks
  PK_CODE    = 2, // .CODE - executable code
  PK_TEXT    = 3, // .TEXT - human-readable text
  PK_INFO    = 4, // .INFO
  PK_DATA    = 5, // .DATA - arbitrary data
  PK_GRAF    = 6, // .GRAF
  PK_FOTO    = 7, // .FOTO
  PK_SECDIR  = 8, // securedir
};

struct _tsPair {
  uint8_t track;
  uint8_t sector;
};

struct _dosTsList {
  uint8_t unused;
  uint8_t nextTrack; // If there's another T/S list block, this points to it
  uint8_t nextSector;
  uint8_t unused2[2];
  uint8_t sectorOffset[2]; // low byte first - could be used for sparse files
  uint8_t unused3[5];
  struct _tsPair tsPair[122];
};


class Vent {
 public:
  Vent();
  Vent(struct _prodosFent *fi);
  Vent(struct _dosFdEntry *fe);
  Vent(struct _subdirent *fi);
  Vent(struct _pascalFent *fp);
  Vent(const Vent &vi);
  ~Vent();

  void Dump(bool verbose=false);

  Vent *nextEnt();
  void nextEnt(Vent *);
  Vent *childrenEnt();
  void childrenEnt(Vent *);

  bool isDirectory();
  // True for the synthetic directory-header entry that descendTree emits as
  // the first node of each directory (volume or subdirectory).
  bool isHeader();
  uint16_t keyPointerVal();
  // Setter: directory headers don't carry their own key block on disk, so
  // the tree builder records it here - it's how a subdirectory entry is
  // matched to its group of child entries in the flat tree.
  void keyPointerVal(uint16_t kp);

  // Raw on-disk entries, for metadata-preserving copies. Only meaningful
  // for a file entry of the matching filesystem (not directory headers).
  const struct _prodosFent *getProdosFent() const { return &prodosData; }
  const struct _dosFdEntry *getDosFent() const { return &dosData; }
  const struct _pascalFent *getPascalFent() const { return &pascalData; }

  // Pascal-specific accessors (meaningful only for a Pascal file entry).
  bool getIsPascal() const { return isPascal; }
  uint16_t getPascalStartBlock() const { return pascalStartBlock; }
  uint16_t getPascalNextBlock() const { return pascalNextBlock; }
  uint16_t getPascalByteCount() const { return pascalByteCount; }
  uint8_t getPascalKind() const { return (uint8_t)(pascalType & 0x0F); }

  const char *getName();

  uint8_t getFirstTrack();
  uint8_t getFirstSector();

  uint8_t getStorageType();

  uint32_t getEofLength();
  uint8_t getFileType();

  // DOS 3.3: sector count from catalog entry (includes T/S list sector)
  // ProDOS: block count from file entry
  uint16_t getBlocksUsed();

  // For ProDOS directory headers: the fileCount field - the header's
  // record of how many active entries the directory contains. Older
  // interpreters (ProDOS 1.1.1, FILER, etc.) use this to decide when
  // to stop listing; a discrepancy vs. a full scan means entries are
  // either hidden from those tools or present as dangling data.
  uint16_t getActiveFileCount();

 private:
  bool isDirectoryHeader;
  bool isDos33;
  bool isPascal;

  // Pascal file geometry (contiguous blocks + bytes-in-last-block).
  uint16_t pascalStartBlock;
  uint16_t pascalNextBlock;
  uint16_t pascalByteCount;
  uint16_t pascalType;

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

  // Cache for inspection
  struct _prodosFent prodosData;
  struct _dosFdEntry dosData;
  struct _pascalFent pascalData;
};

#endif
