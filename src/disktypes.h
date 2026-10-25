#ifndef __DISKTYPES_H
#define __DISKTYPES_H

enum {
  T_AUTO = 0,
  T_WOZ = 1,
  T_NIB = 2,
  T_DSK = 3,
  T_PO = 4,
  T_HDV = 5,  // ProDOS block-ordered hard-disk image (.hdv, .img, large .po)
  // A .2mg (2IMG) file, as a type to read or write. An image read from
  // one still says what's inside it (T_DSK, T_PO, T_NIB or T_HDV).
  T_2MG = 6,
  // DOS 3.2 sectors: 13 to a track, in address-field order (.d13)
  T_D13 = 7
};

#endif
