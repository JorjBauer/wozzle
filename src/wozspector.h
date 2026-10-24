#ifndef __WOZSPECTOR_H
#define __WOZSPECTOR_H

#include "woz.h"
#include "vent.h"
#include "woztypes.h"

class Wozspector : public Woz {
public:
  Wozspector(bool verbose);
  virtual ~Wozspector();

  Vent *getTree();

  // A file's contents: its data alone, without any header the filesystem
  // stores in front of it (DOS 3.3's I, A and B files have one), so it's
  // the same thing on every filesystem. Returns the length; the buffer
  // comes from wozMalloc and the caller wozFree()s it.
  virtual uint32_t getFileContents(Vent *e, char **toWhere) = 0;

  // Returns the approximate on-disk allocation size (in bytes) for the
  // given entry, from directory metadata alone. Used for warning about
  // logical-vs-allocated mismatches when copying files out.
  virtual uint32_t getAllocatedByteCount(Vent *e) = 0;

  // Everything the file has allocated, read straight off the disk: headers
  // included, and past whatever length the directory or a header gives.
  // That's how cpout -A recovers a file whose length is a small "stub"
  // hiding a larger payload. Where the filesystem has nothing past the
  // length to find, it's the same as getFileContents.
  virtual uint32_t getFileAllocation(Vent *e, char **toWhere) {
    return getFileContents(e, toWhere);
  }

  virtual bool writeFileToImage(uint8_t *fileContents,
                                char *fileName,
                                uint8_t fileType, // ProDOS file type, always
                                uint16_t auxTypeData,
                                uint32_t fileSize) = 0;

  // Delete a file by name from the (root) directory, freeing its storage.
  // Every filesystem implements this.
  virtual bool removeFile(const char *fileName) = 0;

  // Delete an empty subdirectory by name. Only filesystems with
  // subdirectories (ProDOS) override this; the default reports that the
  // operation is unsupported and changes nothing.
  virtual bool removeDirectory(const char *dirName);

  // Install boot code into the volume's boot blocks (ProDOS: blocks 0-1).
  // The default reports that the operation is unsupported.
  virtual bool writeBootBlocks(const uint8_t block0[512],
                               const uint8_t block1[512]);

  // Rename the volume (ProDOS: the volume directory header in block 2).
  // Caller passes a pre-validated name. The default reports that the
  // operation is unsupported.
  virtual bool renameVolume(const char *newName);

  // Defragment ("krunch") a Pascal volume so its free space becomes one
  // contiguous region. afterFile == NULL puts the free space at the end of
  // the volume (all files packed toward the front); a non-NULL name puts
  // the free space immediately after that file (files up to and including
  // it pack toward the front, the rest pack toward the end). Only Pascal
  // has contiguous-file storage that needs this; the default reports the
  // operation unsupported.
  virtual bool krunch(const char *afterFile);

  // Recursively delete a subdirectory and everything beneath it (contents
  // first, then the directory itself). Only filesystems with subdirectories
  // (ProDOS) override this; the default reports that the operation is
  // unsupported and changes nothing.
  virtual bool removeDirectoryRecursive(const char *dirName);

  // Create a new (empty) subdirectory by name. Only filesystems with
  // subdirectories (ProDOS) override this; the default reports that the
  // operation is unsupported and changes nothing.
  virtual bool makeDirectory(const char *dirName);

  // Resolve a name (ProDOS: a possibly slash-separated path) to its tree
  // entry, or NULL. The returned Vent is owned by the tree - do not free it.
  // The base implementation matches a flat leaf name; ProDOS overrides it to
  // walk subdirectory paths.
  virtual Vent *findEntry(const char *path);

  // The entries of the subdirectory named by `path`, as a fresh list the
  // caller hands back to freeListing(). Only ProDOS has subdirectories; the
  // base implementation says so and returns NULL.
  virtual Vent *getDirectoryListing(const char *path);
  void freeListing(Vent *list) { freeTree(list); }

  // Returns true if the loaded image looks like the filesystem this
  // spector knows how to handle. Used at startup to warn when the user
  // invoked wozit with the wrong -d/-p mode.
  virtual bool probe() = 0;

  // Which filesystem this inspector reads. (A virtual rather than RTTI,
  // which embedded builds often turn off.)
  virtual wozFsType fsType() const = 0;

protected:
  virtual Vent *createTree() = 0;
  virtual void freeTree(Vent *tree);
  
  void appendTree(Vent *a);

protected:
  Vent *tree;
};

#endif
