#ifndef __WOZSPECTOR_H
#define __WOZSPECTOR_H

#include "woz.h"
#include "vent.h"

class Wozspector : public Woz {
public:
  Wozspector(bool verbose, uint8_t dumpflags);
  virtual ~Wozspector();

  Vent *getTree();
  void displayTree();
  
  virtual void displayInfo() = 0;
  
  // getFileContents will will malloc(); caller must call free()
  // return value is the length
  virtual uint32_t getFileContents(Vent *e, char **toWhere) = 0;

  // Returns the approximate on-disk allocation size (in bytes) for the
  // given entry, from directory metadata alone. Used for warning about
  // logical-vs-allocated mismatches when copying files out.
  virtual uint32_t getAllocatedByteCount(Vent *e) = 0;

  // Returns the full allocated contents of the file by walking the on-disk
  // data sectors/blocks directly, ignoring any in-file length header. Used
  // by cpout -A to recover files whose catalog length is a small "stub"
  // that hides a larger payload. Default impl just delegates to
  // getFileContents for filesystems where header-length == allocation.
  virtual uint32_t getFileAllocation(Vent *e, char **toWhere) {
    return getFileContents(e, toWhere);
  }

  // Dos and ProDOS have different size applesoft headers, which affects listing
  virtual uint8_t applesoftHeaderBytes() = 0;

  virtual bool writeFileToImage(uint8_t *fileContents,
                                char *fileName,
                                uint8_t fileType, // ProDOS file type, always
                                uint16_t auxTypeData,
                                uint32_t fileSize) = 0;

  virtual void inspectFile(const char *fileName, Vent *fp) = 0;

  // Returns true if the loaded image looks like the filesystem this
  // spector knows how to handle. Used at startup to warn when the user
  // invoked wozit with the wrong -d/-p mode.
  virtual bool probe() = 0;

protected:
  virtual Vent *createTree() = 0;
  virtual void displayTree(Vent *tree);
  virtual void freeTree(Vent *tree);
  
  void appendTree(Vent *a);

protected:
  Vent *tree;
};

#endif
