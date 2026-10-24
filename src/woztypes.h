#ifndef __WOZTYPES_H
#define __WOZTYPES_H

// Small plain-C types shared by the C++ operations (wozops.h) and the C
// interface (wozlib.h), so both can name them without including each
// other.

#ifdef __cplusplus
extern "C" {
#endif

// The filesystems the library can read and write. WOZ_FS_UNKNOWN means
// "work it out" where a function looks at an image, and "none of them"
// where one reports what it found.
typedef enum {
  WOZ_FS_UNKNOWN = 0,
  WOZ_FS_DOS33   = 1,
  WOZ_FS_PRODOS  = 2,
  WOZ_FS_PASCAL  = 3
} wozFsType;

#ifdef __cplusplus
};
#endif

#endif
