#ifndef __VTOC_H
#define __VTOC_H

class Vent;

class VToC {
 public:
  VToC();
  ~VToC();

  Vent *createTree(unsigned char data[256]);
  void displayTree(Vent *tree);
  void freeTree(Vent *tree);
  
};

#endif
