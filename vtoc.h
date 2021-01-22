#ifndef __VTOC_H
#define __VTOC_H

class VToC {
 public:
  VToC();
  ~VToC();

  void DecodeVToC(unsigned char data[256]);
  
};

#endif
