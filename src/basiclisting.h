#ifndef __BASICLISTING_H
#define __BASICLISTING_H

#include <stdint.h>
#include <string>
#include <vector>

// One detokenized line of a BASIC program, as produced by the Applesoft
// and Integer BASIC listers. The text is everything after the line number;
// how the number is laid out is up to whoever shows the listing.
struct BasicLine {
  uint16_t lineNumber;
  std::string text;
};

typedef std::vector<BasicLine> BasicListing;

#endif
