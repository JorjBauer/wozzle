#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include "basiclisting.h"

// Applesoft programs load at $0801 on every Apple II. DOS 3.3 stores them
// behind a 2-byte length header; ProDOS stores the program image as-is and
// records $0801 in the directory entry's aux type.
#define APPLESOFT_LOAD_ADDR 0x0801
#define APPLESOFT_MAX_LINENO 63999
// Applesoft's input buffer holds 239 characters, so no single source line
// can be longer than that (the line number and its trailing space included).
#define APPLESOFT_MAX_SRCLINE 239

class ApplesoftLister {
public:
  // Detokenize a program into *out. On a malformed program, *out holds
  // the lines read before the problem and the return value is false.
  bool listFile(uint8_t *buf, uint32_t size, uint8_t skipBytes,
                BasicListing *out);
};

// Tokenize an Applesoft text listing into the in-memory program image the
// Apple II expects: a chain of lines, each [next-line address][line number]
// [tokens...][0x00], ending with a 0x0000 link. The returned image contains
// no filesystem header - DOS 3.3's 2-byte length prefix is the disk layer's
// job - and is malloc'd for the caller to free.
//
// Returns NULL (and sets *outSize to 0) on error, after printing a message
// naming the offending source line.
uint8_t *tokenizeApplesoft(const char *text, uint32_t textLen,
                           uint32_t *outSize);
