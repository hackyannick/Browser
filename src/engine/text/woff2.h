// Kite Engine - WOFF2 web font decoding
#ifndef KITE_TEXT_WOFF2_H
#define KITE_TEXT_WOFF2_H

#include <string>

namespace kite {

// Converts a WOFF2 file into a TrueType/OpenType (sfnt) file.
bool Woff2ToSfnt(const std::string& woff2, std::string& sfnt);

}  // namespace kite

#endif
