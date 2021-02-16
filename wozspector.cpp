#include "wozspector.h"

Wozspector::Wozspector(bool verbose, uint8_t dumpflags) : Woz(verbose, dumpflags)
{
  tree = NULL;
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

