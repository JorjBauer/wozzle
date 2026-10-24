#include "applesoft.h"
#include "wozmsg.h"
#include "wozhost.h"

#include <string.h>
#include <ctype.h>

struct _AStoken {
  uint8_t token;
  char detokened[9];
};

// The canonical Applesoft keyword spellings, indexed by (token & 0x7F).
// Entries 0x00..0x6A are real tokens ($80..$EA); the rest are unused by the
// interpreter. These strings are shared by both directions: listFile() prints
// them and tokenizeApplesoft() matches against them, so they must stay exactly
// as Applesoft spells them - no cosmetic spaces ("PR#", not "PR #") and no
// keywords glued together ("DEF", not "DEF FN"; FN is its own token, $C2).
struct _AStoken applesoftTokens[128] = {
  {0x80, "END"},
  {0x81, "FOR"},
  {0x82, "NEXT"},
  {0x83, "DATA"},
  {0x84, "INPUT"},
  {0x85, "DEL"},
  {0x86, "DIM"},
  {0x87, "READ"},
  {0x88, "GR"},
  {0x89, "TEXT"},
  {0x8A, "PR#"},
  {0x8B, "IN#"},
  {0x8C, "CALL"},
  {0x8D, "PLOT"},
  {0x8E, "HLIN"},
  {0x8F, "VLIN"},
  {0x90, "HGR2"},
  {0x91, "HGR"},
  {0x92, "HCOLOR="},
  {0x93, "HPLOT"},
  {0x94, "DRAW"},
  {0x95, "XDRAW"},
  {0x96, "HTAB"},
  {0x97, "HOME"},
  {0x98, "ROT="},
  {0x99, "SCALE="},
  {0x9A, "SHLOAD"},
  {0x9B, "TRACE"},
  {0x9C, "NOTRACE"},
  {0x9D, "NORMAL"},
  {0x9E, "INVERSE"},
  {0x9F, "FLASH"},
  {0xA0, "COLOR="},
  {0xA1, "POP"},
  {0xA2, "VTAB"},
  {0xA3, "HIMEM:"},
  {0xA4, "LOMEM:"},
  {0xA5, "ONERR"},
  {0xA6, "RESUME"},
  {0xA7, "RECALL"},
  {0xA8, "STORE"},
  {0xA9, "SPEED="},
  {0xAA, "LET"},
  {0xAB, "GOTO"},
  {0xAC, "RUN"},
  {0xAD, "IF"},
  {0xAE, "RESTORE"},
  {0xAF, "&"},
  {0xB0, "GOSUB"},
  {0xB1, "RETURN"},
  {0xB2, "REM"},
  {0xB3, "STOP"},
  {0xB4, "ON"},
  {0xB5, "WAIT"},
  {0xB6, "LOAD"},
  {0xB7, "SAVE"},
  {0xB8, "DEF"},
  {0xB9, "POKE"},
  {0xBA, "PRINT"},
  {0xBB, "CONT"},
  {0xBC, "LIST"},
  {0xBD, "CLEAR"},
  {0xBE, "GET"},
  {0xBF, "NEW"},
  {0xC0, "TAB("},
  {0xC1, "TO"},
  {0xC2, "FN"},
  {0xC3, "SPC("},
  {0xC4, "THEN"},
  {0xC5, "AT"},
  {0xC6, "NOT"},
  {0xC7, "STEP"},
  {0xC8, "+"},
  {0xC9, "-"},
  {0xCA, "*"},
  {0xCB, "/"},
  {0xCC, "^"},
  {0xCD, "AND"},
  {0xCE, "OR"},
  {0xCF, ">"},
  {0xD0, "="},
  {0xD1, "<"},
  {0xD2, "SGN"},
  {0xD3, "INT"},
  {0xD4, "ABS"},
  {0xD5, "USR"},
  {0xD6, "FRE"},
  {0xD7, "SCRN("},
  {0xD8, "PDL"},
  {0xD9, "POS"},
  {0xDA, "SQR"},
  {0xDB, "RND"},
  {0xDC, "LOG"},
  {0xDD, "EXP"},
  {0xDE, "COS"},
  {0xDF, "SIN"},
  {0xE0, "TAN"},
  {0xE1, "ATN"},
  {0xE2, "PEEK"},
  {0xE3, "LEN"},
  {0xE4, "STR$"},
  {0xE5, "VAL"},
  {0xE6, "ASC"},
  {0xE7, "CHR$"},
  {0xE8, "LEFT$"},
  {0xE9, "RIGHT$"},
  {0xEA, "MID$"},
  {0xEB, "???"},
  {0xEC, "???"},
  {0xED, "???"},
  {0xEE, "???"},
  {0xEF, "???"},
  {0xF0, "???"},
  {0xF1, "???"},
  {0xF2, "???"},
  {0xF3, "???"},
  {0xF4, "???"},
  {0xF5, "???"},
  {0xF6, "???"},
  {0xF7, "???"},
  {0xF8, "???"},
  {0xF9, "???"},
  {0xFA, "???"},
  {0xFB, "???"},
  {0xFC, "???"},
  {0xFD, "???"},
  {0xFE, "???"},
  {0xFF, "???"} };

bool ApplesoftLister::listFile(uint8_t *buf, uint32_t ssize, uint8_t skipBytes,
                               BasicListing *out)
{
  uint8_t lo,hi;
  uint32_t pos = skipBytes; // first 2 bytes are program size (from DOS) but ProDOS has naught.

  while (pos < ssize) {
    lo = buf[pos++]; // look for 0,0 terminator
    hi = buf[pos++];
    if (lo == 0 && hi == 0) {
      // normal termination
      return true;
    }

    lo = buf[pos++]; // read line number
    hi = buf[pos++];
    BasicLine line;
    line.lineNumber = (hi << 8) | lo;

    while (1) {
      // read the tokenized/string data until a null terminator
      lo = buf[pos++];
      if (!lo) {
        break;
      }
      
      if (lo & 0x80) {
        // token
        struct _AStoken t = applesoftTokens[lo & 0x7F];
        line.text += ' ';
        line.text += t.detokened;
        line.text += ' ';
      } else {
        // it's a straight character
        line.text += (char)lo;
      }
    }
    out->push_back(line);
  }
  /* NOTREACHED unless abnormal termination happens */
  wozMsg(WOZ_MSG_ERROR, "ERROR: file ended early");
  return false;
}

/* ------------------------------------------------------------------ *
 * Tokenizer: text listing -> Applesoft program image
 *
 * This follows what the ROM's PARSE routine ($D56C) actually does, which
 * is not quite what a naive "find the keyword and swap in a byte" pass
 * would do:
 *
 *   - Whitespace outside of quotes/REM/DATA is discarded, not stored.
 *     LIST re-inserts the cosmetic spaces around tokens, which is why a
 *     listing looks spaced out even though the stored form isn't. This is
 *     what makes cpout -B | cpin -B round-trip cleanly: without it, every
 *     trip through the lister would pad the program a little further.
 *   - The longest matching keyword wins, so "ONERR" beats "ON" and
 *     "NOTRACE" beats "NOT".
 *   - Keywords may be split by spaces: "H O M E" is HOME, and that is the
 *     only reason "PR #3" works.
 *   - Everything after REM is copied verbatim to end of line; everything
 *     after DATA is copied verbatim to the next unquoted colon. Neither
 *     gets keyword substitution (otherwise the AND in "REM COPY AND PASTE"
 *     would turn into a $CD byte).
 *   - The AT/ATN/TO trio needs an explicit tiebreak, because "A TO B"
 *     otherwise matches AT across the space.
 * ------------------------------------------------------------------ */

#define MAX_TOKEN_LEN 7        /* "HCOLOR=", "NOTRACE", "RESTORE" */
#define LAST_TOKEN_IDX 0x6A    /* $EA MID$, the last real token */
#define TOKEN_IDX_DATA  (0x83 - 0x80)
#define TOKEN_IDX_REM   (0xB2 - 0x80)
#define TOKEN_IDX_PRINT (0xBA - 0x80)
#define TOKEN_IDX_AT    (0xC5 - 0x80)
#define TOKEN_IDX_ATN   (0xE1 - 0x80)

// Placeholder written into each line's link field on the first pass; the
// real addresses aren't known until every line has been emitted.
#define PTR_PLACEHOLDER 0xFEED

static bool tokErr(uint32_t srcLine, const char *msg)
{
  wozMsg(WOZ_MSG_ERROR, "ERROR: source line %u: %s", (unsigned)srcLine, msg);
  return false;
}

// Advance *pos to the next non-whitespace character and return it in *ch.
// Returns false at end of line.
static bool nextNonWhite(const char *s, uint32_t len, uint32_t *pos, char *ch)
{
  while (*pos < len) {
    char c = s[(*pos)++];
    if (c != ' ' && c != '\t') {
      *ch = c;
      return true;
    }
  }
  return false;
}

// Longest-match lookup over the keyword table. buf holds up-cased,
// whitespace-stripped input characters. Returns the token index, or -1.
static int findLongestToken(const char *buf, int bufLen, int *foundLen)
{
  int bestIdx = -1;
  int bestLen = 0;

  for (int i = 0; i <= LAST_TOKEN_IDX; i++) {
    const char *kw = applesoftTokens[i].detokened;
    int kwLen = (int)strlen(kw);
    if (kwLen > bufLen || kwLen <= bestLen)
      continue;
    if (!strncmp(kw, buf, kwLen)) {
      bestIdx = i;
      bestLen = kwLen;
    }
  }
  *foundLen = bestLen;
  return bestIdx;
}

// Walk the finished program and turn each line's PTR_PLACEHOLDER into the
// address the following line will occupy once loaded. The last line's link
// points at the two-zero end marker, exactly as a program SAVEd by the Apple
// itself does - writing 0x0000 there instead makes the interpreter (and our
// own lister) stop one line early.
static bool setLinePointers(uint8_t *buf, uint32_t len, uint16_t loadAddr)
{
  uint32_t offset = 0;

  while (len - offset > 4) {
    if ((buf[offset] | (buf[offset+1] << 8)) != PTR_PLACEHOLDER)
      return false;
    uint32_t ptrOffset = offset;

    offset += 4; // past the link and the line number
    while (offset < len && buf[offset] != 0x00)
      offset++;
    offset++;    // the link points one byte past the line's 0x00 terminator
    if (offset > len)
      return false;

    uint16_t addr = (uint16_t)(loadAddr + offset);
    buf[ptrOffset]   = (uint8_t)(addr & 0xFF);
    buf[ptrOffset+1] = (uint8_t)(addr >> 8);
  }
  return true;
}

uint8_t *tokenizeApplesoft(const char *text, uint32_t textLen,
                           uint32_t *outSize)
{
  *outSize = 0;

  // Worst case each source line becomes a 4-byte header, one output byte
  // per source byte, and a 0x00 terminator; plus the 2-byte end marker.
  uint32_t lineCount = 1;
  for (uint32_t i = 0; i < textLen; i++)
    if (text[i] == '\n') lineCount++;
  uint8_t *out = (uint8_t *)wozMalloc(textLen + 5 * lineCount + 8);
  if (!out) {
    wozMsg(WOZ_MSG_ERROR, "ERROR: out of memory tokenizing");
    return NULL;
  }
  uint32_t outPos = 0;

  char tokenBuf[MAX_TOKEN_LEN];
  uint32_t srcLineNum = 0;   // position in the text file, for error messages
  long lastLineNum = -1;     // last Applesoft line number emitted
  uint32_t pos = 0;

  while (pos < textLen) {
    // Carve off one source line.
    uint32_t eol = pos;
    while (eol < textLen && text[eol] != '\n') eol++;
    const char *line = text + pos;
    uint32_t lineLen = eol - pos;
    if (lineLen && line[lineLen-1] == '\r') lineLen--;
    pos = (eol < textLen) ? eol + 1 : textLen;
    srcLineNum++;

    // Read the line number. Applesoft ignores spaces inside it, so "1 0"
    // really is line 10.
    uint32_t cp = 0;
    long lineNum = -1;
    int digits = 0;
    while (cp < lineLen) {
      char ch;
      uint32_t before = cp;
      if (!nextNonWhite(line, lineLen, &cp, &ch))
        break;
      if (ch < '0' || ch > '9') {
        cp = before;  // not part of the number; hand it to the tokenizer
        break;
      }
      if (++digits > 5) {
        wozFree(out);
        tokErr(srcLineNum, "line number has too many digits");
        return NULL;
      }
      if (lineNum < 0) lineNum = 0;
      lineNum = lineNum * 10 + (ch - '0');
    }

    if (lineNum < 0) {
      // No line number. Blank lines and '#' comments are source-level
      // niceties; anything else is a mistake we should not paper over.
      uint32_t t = 0;
      while (t < lineLen && isspace((unsigned char)line[t])) t++;
      if (t == lineLen || line[t] == '#')
        continue;
      wozFree(out);
      tokErr(srcLineNum, "line does not start with a line number");
      return NULL;
    }
    if (lineNum > APPLESOFT_MAX_LINENO) {
      wozFree(out);
      tokErr(srcLineNum, "line number is greater than 63999");
      return NULL;
    }
    // Applesoft keeps its program in ascending line order and has no way to
    // represent two lines with the same number, so both are hard errors
    // rather than something to silently reorder behind the user's back.
    if (lineNum == lastLineNum) {
      wozFree(out);
      tokErr(srcLineNum, "duplicate line number");
      return NULL;
    }
    if (lineNum < lastLineNum) {
      wozFree(out);
      tokErr(srcLineNum, "line number is out of order");
      return NULL;
    }
    lastLineNum = lineNum;

    out[outPos++] = PTR_PLACEHOLDER & 0xFF;
    out[outPos++] = PTR_PLACEHOLDER >> 8;
    out[outPos++] = (uint8_t)(lineNum & 0xFF);
    out[outPos++] = (uint8_t)((lineNum >> 8) & 0xFF);

    while (cp < lineLen) {
      // Collect up to MAX_TOKEN_LEN non-whitespace characters to match against.
      uint32_t tokenStart = cp;
      int tokenBufCnt = 0;
      bool foundQuote = false;
      while (tokenBufCnt < MAX_TOKEN_LEN && cp < lineLen) {
        char ch;
        if (!nextNonWhite(line, lineLen, &cp, &ch))
          break;
        if (ch == '"') {
          foundQuote = true;
          break;
        }
        tokenBuf[tokenBufCnt++] = (char)toupper((unsigned char)ch);
      }

      if (tokenBufCnt == 0) {
        if (!foundQuote)
          break; // nothing left but whitespace
        // Copy the string constant through verbatim, case and all. An
        // unterminated string at end of line is legal Applesoft.
        out[outPos++] = '"';
        while (cp < lineLen) {
          char ch = line[cp++];
          out[outPos++] = (uint8_t)ch;
          if (ch == '"')
            break;
        }
        continue;
      }

      int foundLen = 0;
      int tokenIdx = findLongestToken(tokenBuf, tokenBufCnt, &foundLen);

      if (tokenIdx < 0) {
        // Not a keyword; emit one character and try again from the next.
        cp = tokenStart;
        char ch;
        nextNonWhite(line, lineLen, &cp, &ch);
        if (ch == '?')
          out[outPos++] = TOKEN_IDX_PRINT + 0x80; // ? is shorthand for PRINT
        else
          out[outPos++] = (uint8_t)toupper((unsigned char)ch);
        continue;
      }

      if (tokenIdx == TOKEN_IDX_AT || tokenIdx == TOKEN_IDX_ATN) {
        // AT, ATN and TO all collide: "A TO B" matches AT across the space,
        // and "AT N" matches ATN. Re-scan the raw text (spaces included) to
        // see which one the author meant. Both candidates start with 'A',
        // so find the 'T' first.
        uint32_t chk = tokenStart + 1;
        while (chk < lineLen && toupper((unsigned char)line[chk]) != 'T') chk++;
        chk++; // step past the 'T'

        if (chk < lineLen) {
          char upch = (char)toupper((unsigned char)line[chk]);
          if (upch == 'O') {
            // "A TO ...": emit the A on its own and let TO match next pass.
            cp = tokenStart;
            char ch;
            nextNonWhite(line, lineLen, &cp, &ch);
            out[outPos++] = (uint8_t)toupper((unsigned char)ch);
            continue;
          }
          if (upch != 'N' && tokenIdx == TOKEN_IDX_ATN) {
            // Matched ATN only because a space was skipped ("AT N"); the
            // author wrote AT followed by a variable.
            tokenIdx = TOKEN_IDX_AT;
            foundLen--;
          }
        } else if (tokenIdx == TOKEN_IDX_ATN) {
          tokenIdx = TOKEN_IDX_AT;
          foundLen--;
        }
      }

      out[outPos++] = (uint8_t)(tokenIdx + 0x80);

      // Consume exactly the characters the keyword used, whitespace included.
      cp = tokenStart;
      for (int i = 0; i < foundLen; i++) {
        char unused;
        nextNonWhite(line, lineLen, &cp, &unused);
      }

      if (cp >= lineLen)
        continue;

      if (tokenIdx == TOKEN_IDX_REM) {
        // Everything after REM is comment text, not code.
        if (line[cp] == ' ') cp++;   // the space LIST prints after the token
        while (cp < lineLen)
          out[outPos++] = (uint8_t)line[cp++];
      } else if (tokenIdx == TOKEN_IDX_DATA) {
        // DATA elements are literal too, up to the colon that ends the
        // statement. A colon inside quotes is just data.
        if (line[cp] == ' ') cp++;
        bool inQuote = false;
        while (cp < lineLen) {
          char ch = line[cp];
          if (ch == '"') inQuote = !inQuote;
          if (!inQuote && ch == ':') break;
          out[outPos++] = (uint8_t)line[cp++];
        }
      }
    }

    out[outPos++] = 0x00; // end of line

    // Not fatal - the program runs fine - but such a line can't be listed
    // and re-entered on the Apple II, whose input buffer stops at 239.
    if (lineLen > APPLESOFT_MAX_SRCLINE) {
      wozMsg(WOZ_MSG_WARNING,
             "WARNING: source line %u (Applesoft line %ld) is too long to "
             "edit on an Apple II", (unsigned)srcLineNum, lineNum);
    }
  }

  if (lastLineNum < 0) {
    wozFree(out);
    wozMsg(WOZ_MSG_ERROR, "ERROR: no Applesoft program lines found in the input");
    return NULL;
  }

  out[outPos++] = 0x00; // end of program
  out[outPos++] = 0x00;

  if (!setLinePointers(out, outPos, APPLESOFT_LOAD_ADDR)) {
    wozFree(out);
    wozMsg(WOZ_MSG_ERROR, "ERROR: internal error setting line pointers");
    return NULL;
  }

  *outSize = outPos;
  return out;
}
