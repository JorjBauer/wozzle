#include "wozspector.h"

#include <stdio.h>
#include <string.h>

Wozspector::Wozspector(bool verbose, uint8_t dumpflags) : Woz(verbose, dumpflags)
{
  tree = NULL;
}

// Default: this filesystem has no notion of subdirectories. ProDOS
// overrides these; DOS 3.3 inherits the unsupported behavior.
bool Wozspector::removeDirectory(const char *dirName)
{
  printf("This filesystem has no subdirectories; rmdir is not supported.\n");
  return false;
}

bool Wozspector::removeDirectoryRecursive(const char *dirName)
{
  printf("This filesystem has no subdirectories; rm -r is not supported.\n");
  return false;
}

bool Wozspector::makeDirectory(const char *dirName)
{
  printf("This filesystem has no subdirectories; mkdir is not supported.\n");
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

void Wozspector::displayDirectory(const char *path)
{
  printf("Listing a subdirectory is only supported on ProDOS.\n");
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

void Wozspector::displayTree()
{
  displayTree(getTree());
}

void Wozspector::displayTree(Vent *tree)
{
  while (tree) {
    tree->Dump();
    tree = tree->nextEnt();
  }
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

