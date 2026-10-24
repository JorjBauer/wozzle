#include "wozspector.h"
#include "wozmsg.h"

#include <stdio.h>
#include <string.h>

Wozspector::Wozspector(bool verbose) : Woz(verbose)
{
  tree = NULL;
}

// Default: this filesystem has no notion of subdirectories. ProDOS
// overrides these; DOS 3.3 inherits the unsupported behavior.
bool Wozspector::removeDirectory(const char *dirName)
{
  wozMsg(WOZ_MSG_ERROR, "This filesystem has no subdirectories; rmdir is not supported.");
  return false;
}

bool Wozspector::removeDirectoryRecursive(const char *dirName)
{
  wozMsg(WOZ_MSG_ERROR, "This filesystem has no subdirectories; rm -r is not supported.");
  return false;
}

bool Wozspector::makeDirectory(const char *dirName)
{
  wozMsg(WOZ_MSG_ERROR, "This filesystem has no subdirectories; mkdir is not supported.");
  return false;
}

bool Wozspector::writeBootBlocks(const uint8_t block0[512],
                                 const uint8_t block1[512])
{
  wozMsg(WOZ_MSG_ERROR, "bootblocks is only supported for ProDOS volumes.");
  return false;
}

bool Wozspector::renameVolume(const char *newName)
{
  wozMsg(WOZ_MSG_ERROR,
         "volname is only supported for ProDOS volumes (DOS 3.3 has a "
         "volume number, not a name).");
  return false;
}

bool Wozspector::krunch(const char *afterFile)
{
  wozMsg(WOZ_MSG_ERROR, "krunch is only supported for Pascal volumes.");
  return false;
}

// Flat-namespace lookup: match the first tree entry whose leaf name equals
// `path`. ProDOS overrides this to resolve subdirectory paths.
Vent *Wozspector::findEntry(const char *path)
{
  for (Vent *p = getTree(); p; p = p->nextEnt()) {
    if (!strcmp(path, p->getName()))
      return p;
  }
  return NULL;
}

Vent *Wozspector::getDirectoryListing(const char *path)
{
  wozMsg(WOZ_MSG_ERROR, "Listing a subdirectory is only supported on ProDOS.");
  return NULL;
}

Wozspector::~Wozspector()
{
  if (tree) {
    freeTree(tree);
    tree = NULL;
  }
}

Vent *Wozspector::getTree()
{
  if (!tree) {
    // If the tree's not loaded, then load it
    createTree();
  }

  // otherwise return whatever tree is currently loaded
  return tree;
}

void Wozspector::freeTree(Vent *tree)
{
  if (!tree) return;

  do {
    Vent *t = tree;
    tree = tree->nextEnt();
    delete t;
  } while (tree);
  tree = NULL;
}

void Wozspector::appendTree(Vent *a)
{
  Vent *t = tree;
  if (!tree) {
    tree = a;
    return;
  }

  while (t->nextEnt()) {
    t = t->nextEnt();
  }
  t->nextEnt(a);
}

