#include <stdint.h>

typedef struct _asEntry {
  uint32_t entryID;
  uint32_t offset;
  uint32_t length;
} asEntry;

typedef struct _applesingle {
  uint32_t magic; // 0x00051600
  uint32_t version; // 0x000020000
  uint8_t filler[16]; // all zeroes
  uint16_t num_entries;
  asEntry entry[1]; // and continue off the end of the array
} applesingle;

// Structure of as_prodosFileInfo type
typedef struct _type11prodos {
  uint16_t access;
  uint16_t filetype;
  uint32_t auxtype;
} type11prodos;

// AppleSingle entryID types
enum {
  as_dataFork = 1,
  as_resourceFork = 2,
  as_realName = 3,
  as_comment = 4,
  as_bwicon = 5,
  as_coloricon = 6,
  as_fileDates = 8,
  as_finderInfo = 9,
  as_macFileInfo = 10,
  as_prodosFileInfo = 11,
  as_msdosFileInfo = 12,
  as_shortName = 13,
  as_afpFileInfo = 14,
  as_afpDirectoryID = 15
};


  
