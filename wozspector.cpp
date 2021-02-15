#include "wozspector.h"

Wozspector::Wozspector(bool verbose, uint8_t dumpflags) : Woz(verbose, dumpflags)
{
}

Wozspector::~Wozspector()
{
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
}
