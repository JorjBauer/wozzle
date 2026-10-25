#ifndef __WOZ_H
#define __WOZ_H
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include "nibutil.h"
#include "disktypes.h"

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
  uint16_t fluxBlock;       // INFO v3: 0 if no FLUX chunk
  uint16_t largestFluxTrack;// INFO v3: 0 if no FLUX chunk
} diskInfo;

typedef struct _trackInfo {
  uint16_t startingBlock; // v2
  uint32_t startingByte;  // v1
  uint16_t blockCount;
  uint32_t bitCount;
  uint8_t *trackData;
  bool dirty;
} trackInfo;

// One sector as found walking a track in order (see scanTrackOrderedSectors)
typedef struct _trackSectorDump {
  uint8_t volume;
  uint8_t track;
  uint8_t sector;
  uint8_t checksum;
  bool haveData;          // false if the data field was never found
  uint8_t data62[342];    // still 6-and-2 encoded
} trackSectorDump;

class Woz {
 public:
  Woz(bool verbose);
  ~Woz();

  bool readFile(const char *filename, bool preloadTracks, uint8_t forceType = T_AUTO);
  bool writeFile(const char *filename, uint8_t forceType = T_AUTO);

  void advanceBitStream(uint8_t datatrack);
  uint8_t getNextWozBit(uint8_t datatrack);


  // Read-only views of the image, for programs that report on it
  const diskInfo *getDiskInfo() const { return &di; }
  const trackInfo *getTrackInfo(uint8_t datatrack) const { return &tracks[datatrack]; }
  // The META chunk, one key/value pair at a time. Returns false past the
  // last entry (or if there's no META chunk at all).
  bool hasMetadata() const { return metaData != NULL; }
  bool getMetadataEntry(uint32_t index, char *key, size_t keyLen,
                        char *value, size_t valueLen);
  bool checksumWozDataTrack(uint8_t datatrack, uint32_t *retCRC);
  bool readNibSectorDataFromDataTrack(uint8_t dataTrack, uint8_t sector, nibSector *sectorData);
  // Walk a data track for up to two revolutions, collecting each sector
  // in the order it turns up (repeats included) until all 16 are seen.
  // Returns how many were found; at most maxOut are stored.
  uint8_t scanTrackOrderedSectors(uint8_t datatrack, trackSectorDump *out,
                                  uint8_t maxOut);

  bool isSynchronized();
  bool isVerbose() const { return verbose; }

  // INFO chunk write-protect flag. Most copy-protected WOZ images
  // have this set (the originals were WP-tabbed); some protection
  // schemes check $C08D's shifted-in WP bits as part of the handshake.
  bool isWriteProtected() const { return di.writeProtected != 0; }

  uint8_t dataTrackNumberForQuarterTrack(uint16_t qt);
  
  bool flush();
  bool isDirty();

  bool decodeWozTrackToDsk(uint8_t phystrack, uint8_t subtype, uint8_t sectorData[256*16]);
  bool decodeWozTrackSector(uint8_t phystrack, uint8_t sector, uint8_t dataOut[256]);
  bool encodeWozTrackSector(uint8_t phystrack, uint8_t sector, uint8_t dataIn[256]);

  // 13-sector (DOS 3.2) decode path. Like decodeWozTrackToDsk but emits
  // 13 logically-ordered 256-byte sectors and uses the 5-and-3 codec
  // with the D5 AA B5 address prologue.
  bool decodeWozTrack13ToDsk(uint8_t phystrack, uint8_t sectorData[256*13]);

  // True for a 13-sector (DOS 3.2) disk. A WOZ 2 INFO chunk that says
  // 13-sector (bootSectorFormat = 2) or a .d13 file settles it; "both"
  // (= 3) counts as 16-sector. Otherwise (WOZ 1, .nib, a nibble .2mg, or
  // a WOZ 2 that doesn't say) we look at which address prologue the
  // tracks use, once, and remember the answer.
  bool isThirteenSectorDisk();

  // Dump the raw latched-nibble byte stream for one physical track -
  // one `NIBTRACKSIZE` revolution's worth of bytes straight out of
  // nextDiskByte(). No sector framing, no checksums, no remapping;
  // exactly what a dumb nib-reader would see. Useful for analyzing
  // non-standard disk formats (rwts18, copy-protected variants).
  bool readRawNibStream(uint8_t phystrack, uint8_t out[/* NIBTRACKSIZE */]);
  void dumpTrackState(uint8_t datatrack);

  // ProDOS hard-drive image accessors. Valid only when imageType == T_HDV.
  // The buffer is owned by Woz and sized to hdvByteCount(). Read it with
  // hdvBuffer(); write through hdvBufferForWrite(), which marks the image
  // dirty (writeFile flushes the modified buffer back to disk).
  bool isHdv() const { return imageType == T_HDV; }
  uint32_t hdvByteCount() const { return hdvByteSize; }
  const uint8_t *hdvBuffer() const { return hdvData; }
  uint8_t *hdvBufferForWrite() { hdvDirty = true; return hdvData; }
  // Drop the tail of a hard-drive image: the buffer keeps its first
  // newByteSize bytes, and writeFile() then emits a file that much
  // smaller. Shrinking only - growing an image isn't something a
  // filesystem edit can ask for here.
  bool truncateHdv(uint32_t newByteSize);

  // A .2mg (2IMG) image is a 64-byte header and then the image in one of
  // three layouts: DOS order, ProDOS order or nibbles. readFile() reads
  // the header and loads what's inside the usual way, so imageType says
  // T_DSK, T_PO, T_NIB or T_HDV as it would for the bare file.
  // writeFile() to a .2mg name writes the header back, along with the
  // comment and creator data from the image it read, if any.
  static bool isTwoImgName(const char *filename);
  // Who made the image, as its file records it: a WOZ INFO chunk's
  // creator, or a 2IMG header's four-letter creator code. Empty for the
  // formats that don't record one (.dsk, .po, .nib, .hdv, .d13).
  void getImageCreator(char *out, size_t len) const;
  bool isTwoImg() const { return twoImg; }
  // Write a freshly built blank image (DOS order for DOS 3.3, ProDOS
  // order otherwise) to a new file: as it is, or inside a 2IMG header if
  // the name is .2mg. The caller has already checked the name is free.
  static bool writeBlankImageFile(const char *filename, const uint8_t *data,
                                  uint32_t size, bool dosOrder);

 protected:
  // writeFile's last step: write the image as `type` to exactly this name
  bool writeFileAs(const char *filename, uint8_t type);
  bool writeWozFile(const char *filename, uint8_t subtype);
  bool writeWozFile(int fdout, uint8_t subtype);
  bool writeDskFile(const char *filename, uint8_t subtype);
  bool writeDskFile(int fdout, uint8_t subtype);
  bool writeNibFile(const char *filename);
  bool writeNibFile(int fdout);
  
  uint8_t nextDiskBit(uint8_t datatrack);
  uint8_t nextDiskByte(uint8_t datatrack);
  uint8_t nextRawWozByte(uint8_t datatrack);

  void writeDiskByte(uint8_t datatrack, uint8_t b);
  
 protected:
  bool readWozFile(const char *filename, bool preloadTracks);
  bool readDskFile(const char *filename, bool preloadTracks, uint8_t subtype);
  bool readNibFile(const char *filename, bool preloadTracks);
  // offset and length pick out a 2IMG file's blocks; 0 means the whole file
  bool readHdvFile(const char *filename, uint32_t offset = 0, uint32_t length = 0);
  bool writeHdvFile(const char *filename);
  bool writeHdvFile(int fdout);
  bool writeD13File(const char *filename);
  bool sniffThirteenSector();
  bool readTwoImgFile(const char *filename, bool preloadTracks);
  bool writeTwoImgFile(const char *filename);
  void clearTwoImg();

  bool decodeWozTrackToNibFromDataTrack(uint8_t dataTrack, nibSector sectorData[16]);

  uint8_t fakeBit();

  bool writeNextWozBit(uint8_t datatrack, uint8_t bit);
  bool writeNextWozByte(uint8_t datatrack, uint8_t b);
  
  bool parseTRKSChunk(uint32_t chunkSize);
  bool parseTMAPChunk(uint32_t chunkSize);
  bool parseInfoChunk(uint32_t chunkSize);
  bool parseMetaChunk(uint32_t chunkSize);

  bool writeInfoChunk(uint8_t version, int fdout);
  bool writeTMAPChunk(uint8_t version, int fdout);
  bool writeTRKSChunk(uint8_t version, int fdout);

  bool readWozDataTrack(uint8_t datatrack);
  bool writeNibSectorDataToDataTrack(uint8_t dataTrack, uint8_t sector, uint8_t nibData[343]);

  bool loadMissingTrackFromImage(uint8_t datatrack);

  void _initInfo();

 protected:
  int fd;
  
 protected:
  uint8_t imageType;
  
  bool verbose;

  bool autoFlushTrackData;
  
  uint8_t quarterTrackMap[40*4];
  diskInfo di;
  trackInfo tracks[160];

  // cursor for track enumeration
  uint32_t trackPointer;
  uint32_t trackBitCounter;
  uint8_t trackByte;
  uint8_t trackByteFromDataTrack;
  uint8_t trackBitIdx;
  uint8_t trackLoopCounter;
  char *metaData;
  uint8_t randData, randPtr;

  // MC3470 fake-bit emulation: 4-bit sliding window over the last
  // four bits read. When it goes to zero (i.e. we've seen four 0s in
  // a row) the real chip starts hallucinating pulses, so we inject
  // random bits instead. Persisted across track switches - the real
  // chip lives in the drive, not on the track.
  uint8_t headWindow;

  // HDV-backed state. Only populated when imageType == T_HDV.
  // hdvData owns a heap buffer the size of the loaded image; the
  // destructor frees it.
  uint8_t *hdvData;
  uint32_t hdvByteSize;
  // Changed since it was read or last written
  bool hdvDirty;
  // True when this HDV image came from a .po/.dsk that was larger than a
  // floppy, rather than from a genuine .hdv/.img. Such an image is raw
  // blocks under a floppy extension, so writeFile may write it back the
  // same way; a real hard-disk image still refuses floppy-format output.
  bool hdvPromotedFromFloppyExt;

  // isThirteenSectorDisk()'s answer when it had to look: -1 until then
  int8_t thirteenSector;

  // Where the image's data starts in its file: 0, except past a 2IMG
  // header. Every read from fd adds it.
  uint32_t dataOffset;
  // Volume number for the address fields when we build tracks from
  // sectors. DOS keeps its own copy in the VTOC; this is the one a 2IMG
  // header can carry, and 254 when nothing says otherwise.
  uint8_t sectorVolume;
  // From the 2IMG header we read, so writing one back keeps them
  bool twoImg;
  uint8_t twoImgFormat;       // 0 DOS order, 1 ProDOS order, 2 nibbles
  uint32_t twoImgFlags;
  char twoImgCreator[4];
  uint8_t *twoImgComment;
  uint32_t twoImgCommentLen;
  uint8_t *twoImgCreatorData;
  uint32_t twoImgCreatorDataLen;
};

#endif
