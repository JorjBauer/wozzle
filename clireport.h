#ifndef __CLIREPORT_H
#define __CLIREPORT_H

// Text reports for the command line tools. The disk image library hands
// back data; these functions decide how it looks in a terminal. Another
// front end (a GUI, say) would skip this file and present the same data
// its own way.

#include <stdio.h>
#include <stdint.h>
#include "woz.h"
#include "wozspector.h"
#include "basiclisting.h"

// What printWozInfo() shows beyond the INFO and META chunks
#define DUMP_TRACK         0x01
#define DUMP_QTMAP         0x02
#define DUMP_QTCRC         0x04
// these all require DUMP_TRACK:
#define DUMP_TOFILE        0x10
#define DUMP_SECTOR        0x20
#define DUMP_RAWTRACK      0x40
#define DUMP_ORDEREDSECTOR 0x80

// The image's INFO and META chunks, plus whatever dumpflags asks for.
// DUMP_TOFILE also writes each decoded sector to a file named t<T>s<S>.
void printWozInfo(Woz *w, uint8_t dumpflags);

// One directory entry, as a line of a listing; verbose adds every raw field
void printEntry(Vent *e, bool verbose);
void printListing(Vent *list);

// Filesystem-level information (free space and the like)
void printVolumeInfo(Wozspector *fs);

// Filesystem-specific detail about one file's storage
void printFileDetails(Wozspector *fs, Vent *fp);

// A detokenized BASIC program. Integer BASIC right-aligns its line numbers.
void printBasicListing(FILE *out, const BasicListing &lines, bool integerBasic);

#endif
