#include "intbas.h"

#include <stdio.h>
#include <string.h>

struct _INTtoken {
  uint8_t token;
  char detokened[9];
};

// Integer BASIC plain tokens (byte values 0x00..0x7F). Byte values with the
// high bit set are not in this table — they are interpreted by listFile()
// as string-literal characters, REM comment characters, numeric-constant
// markers (0xB0..0xB9), or variable-name letters (0xC1..0xDA).
struct _INTtoken integerTokens[128] = {
  {0x00, "HIMEM:"},
  {0x01, ""},         // end-of-line; handled by the outer loop
  {0x02, "_"},
  {0x03, ":"},
  {0x04, "LOAD"},
  {0x05, "SAVE"},
  {0x06, "CON"},
  {0x07, "RUN"},
  {0x08, "RUN"},
  {0x09, "DEL"},
  {0x0A, ","},
  {0x0B, "NEW"},
  {0x0C, "CLR"},
  {0x0D, "AUTO"},
  {0x0E, ","},
  {0x0F, "MAN"},
  {0x10, "HIMEM:"},
  {0x11, "LOMEM:"},
  {0x12, "+"},
  {0x13, "-"},
  {0x14, "*"},
  {0x15, "/"},
  {0x16, "="},
  {0x17, "#"},
  {0x18, ">="},
  {0x19, ">"},
  {0x1A, "<="},
  {0x1B, "<>"},
  {0x1C, "<"},
  {0x1D, "AND"},
  {0x1E, "OR"},
  {0x1F, "MOD"},
  {0x20, "^"},
  {0x21, "+"},
  {0x22, "("},
  {0x23, ","},
  {0x24, "THEN"},
  {0x25, "THEN"},
  {0x26, ","},
  {0x27, ","},
  {0x28, "\""},       // open-quote; handled specially in listFile
  {0x29, "\""},       // close-quote; handled specially in listFile
  {0x2A, "("},
  {0x2B, "!"},
  {0x2C, "!"},
  {0x2D, "("},
  {0x2E, "PEEK"},
  {0x2F, "RND"},
  {0x30, "SGN"},
  {0x31, "ABS"},
  {0x32, "PDL"},
  {0x33, "RNDX"},
  {0x34, "("},
  {0x35, "+"},
  {0x36, "-"},
  {0x37, "NOT"},
  {0x38, "("},
  {0x39, "="},
  {0x3A, "#"},
  {0x3B, "LEN("},
  {0x3C, "ASC("},
  {0x3D, "SCRN("},
  {0x3E, ","},
  {0x3F, "("},
  {0x40, "$"},
  {0x41, "$"},
  {0x42, "("},
  {0x43, ","},
  {0x44, ","},
  {0x45, ";"},
  {0x46, ";"},
  {0x47, ";"},
  {0x48, ","},
  {0x49, ","},
  {0x4A, ","},
  {0x4B, "TEXT"},
  {0x4C, "GR"},
  {0x4D, "CALL"},
  {0x4E, "DIM"},
  {0x4F, "DIM"},
  {0x50, "TAB"},
  {0x51, "END"},
  {0x52, "INPUT"},
  {0x53, "INPUT"},
  {0x54, "INPUT"},
  {0x55, "FOR"},
  {0x56, "="},
  {0x57, "TO"},
  {0x58, "STEP"},
  {0x59, "NEXT"},
  {0x5A, ","},
  {0x5B, "RETURN"},
  {0x5C, "GOSUB"},
  {0x5D, "REM"},      // handled specially in listFile
  {0x5E, "LET"},
  {0x5F, "GOTO"},
  {0x60, "IF"},
  {0x61, "PRINT"},
  {0x62, "PRINT"},
  {0x63, "PRINT"},
  {0x64, "POKE"},
  {0x65, ","},
  {0x66, "COLOR="},
  {0x67, "PLOT"},
  {0x68, ","},
  {0x69, "HLIN"},
  {0x6A, ","},
  {0x6B, "AT"},
  {0x6C, "VLIN"},
  {0x6D, ","},
  {0x6E, "AT"},
  {0x6F, "VTAB"},
  {0x70, "="},
  {0x71, "="},
  {0x72, ")"},
  {0x73, ")"},
  {0x74, "LIST"},
  {0x75, ","},
  {0x76, "LIST"},
  {0x77, "POP"},
  {0x78, "NODSP"},
  {0x79, "NODSP"},
  {0x7A, "NOTRACE"},
  {0x7B, "DSP"},
  {0x7C, "DSP"},
  {0x7D, "TRACE"},
  {0x7E, "PR#"},
  {0x7F, "IN#"} };

bool IntegerLister::listFile(uint8_t *buf, uint32_t ssize, uint8_t skipBytes)
{
  uint32_t pos = skipBytes;

  while (pos < ssize) {
    if (ssize - pos < 3) {
      printf("ERROR: file ended in the middle of a line header\n");
      return false;
    }

    // Per-line layout: [lineLen][lineNum-lo][lineNum-hi][bytes...][0x01 EOL]
    // lineLen counts every byte from itself through the EOL, inclusive.
    uint32_t lineStart = pos;
    uint8_t lineLen = buf[pos++];
    if (lineLen == 0) {
      // A zero-length line marks the end of the program.
      return true;
    }
    uint16_t lineNumber = buf[pos] | (buf[pos+1] << 8);
    pos += 2;
    printf("%5u ", lineNumber);

    while (pos < ssize && buf[pos] != 0x01) {
      uint8_t b = buf[pos++];

      if (b == 0x28) {
        // Open-quote: copy bytes (stripping high bit) until close-quote.
        printf("\"");
        while (pos < ssize && buf[pos] != 0x29) {
          printf("%c", buf[pos++] & 0x7F);
        }
        if (pos >= ssize) {
          printf("\nERROR: file ended inside a string constant\n");
          return false;
        }
        printf("\"");
        pos++; // consume close-quote
      } else if (b == 0x5D) {
        // REM: print the keyword, then consume bytes until EOL.
        printf(" %s ", integerTokens[b].detokened);
        while (pos < ssize && buf[pos] != 0x01) {
          printf("%c", buf[pos++] & 0x7F);
        }
        if (pos >= ssize) {
          printf("\nERROR: file ended inside a REM\n");
          return false;
        }
      } else if (b & 0x80) {
        if (b >= ('0' | 0x80) && b <= ('9' | 0x80)) {
          // Numeric-constant marker: the token itself is discarded, the
          // next two bytes are a little-endian unsigned value.
          if (ssize - pos < 2) {
            printf("\nERROR: file ended inside an integer constant\n");
            return false;
          }
          uint16_t v = buf[pos] | (buf[pos+1] << 8);
          pos += 2;
          printf("%u", v);
        } else if (b >= ('A' | 0x80) && b <= ('Z' | 0x80)) {
          // Variable-name start: the token byte is itself the first
          // letter. Keep emitting while following bytes are high-bit
          // letters or digits.
          printf("%c", b & 0x7F);
          while (pos < ssize) {
            uint8_t c = buf[pos];
            if ((c >= ('A' | 0x80) && c <= ('Z' | 0x80)) ||
                (c >= ('0' | 0x80) && c <= ('9' | 0x80))) {
              printf("%c", c & 0x7F);
              pos++;
            } else {
              break;
            }
          }
        } else {
          printf("\nWARNING: unexpected byte 0x%.2X at offset 0x%X\n",
                 b, pos - 1);
        }
      } else {
        // Plain token: print with surrounding spaces (applesoft.cpp style).
        struct _INTtoken t = integerTokens[b];
        printf(" %s ", t.detokened);
      }
    }
    printf("\n");

    if (pos < ssize && buf[pos] == 0x01) pos++; // consume EOL

    // Integer BASIC stores a per-line length specifically so readers can
    // jump or re-align past a line. A REM whose text contains a literal
    // 0x01 byte will have tripped the inner loop early; use the stored
    // length to snap back to the true next-line boundary.
    uint32_t actualLen = pos - lineStart;
    if (actualLen != lineLen) {
      printf("WARNING: line %u stored length %u != actual %u — resyncing\n",
             lineNumber, lineLen, actualLen);
      uint32_t target = lineStart + lineLen;
      if (target > ssize) {
        printf("ERROR: stored line length runs past end of buffer\n");
        return false;
      }
      pos = target;
    }
  }
  return true;
}
