// Kite Engine - helpers for web font files (sfnt / WOFF / WOFF2)
#ifndef KITE_TEXT_FONTFILE_H
#define KITE_TEXT_FONTFILE_H

#include <string>

namespace kite {

// Converts a WOFF 1.0 or WOFF2 file to a plain TrueType/OpenType (sfnt)
// file. Plain sfnt input is returned unchanged.
bool FontToSfnt(const std::string& data, std::string& sfnt);
// Family name (name ID 1) stored in an sfnt font, as UTF-8.
std::string SfntFamilyName(const std::string& sfnt);

}  // namespace kite

#endif
