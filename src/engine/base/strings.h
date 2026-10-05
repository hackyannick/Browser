// Kite Engine - string helpers (UTF-8 is the internal encoding everywhere)
#ifndef KITE_BASE_STRINGS_H
#define KITE_BASE_STRINGS_H

#include <string>
#include <vector>
#include <cstdint>

namespace kite {

typedef uint32_t Codepoint;

// Decodes one UTF-8 sequence starting at s[i]; advances i. Invalid bytes
// decode to U+FFFD and consume a single byte.
Codepoint Utf8Next(const std::string& s, size_t& i);
void Utf8Append(std::string& out, Codepoint cp);
std::u16string Utf8ToUtf16(const std::string& s);
std::string Utf16ToUtf8(const std::u16string& s);
size_t Utf8Length(const std::string& s);

// Converts text in the given charset (as named in HTTP/HTML) to UTF-8.
// Supports utf-8, iso-8859-1/latin1/windows-1252/us-ascii, iso-8859-15,
// utf-16le/be. Unknown charsets are treated as UTF-8 with latin-1 fallback.
std::string ConvertToUtf8(const std::string& bytes, const std::string& charset);
bool LooksLikeValidUtf8(const std::string& s);

inline bool IsAsciiSpace(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
inline bool IsAsciiAlpha(int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
inline bool IsAsciiDigit(int c) { return c >= '0' && c <= '9'; }
inline bool IsAsciiHex(int c) {
  return IsAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline int HexValue(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
inline char AsciiLowerChar(char c) {
  return (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
}

std::string AsciiLower(const std::string& s);
std::string AsciiUpper(const std::string& s);
bool EqualsIgnoreCase(const std::string& a, const std::string& b);
bool StartsWith(const std::string& s, const std::string& prefix);
bool StartsWithIgnoreCase(const std::string& s, const std::string& prefix);
bool EndsWith(const std::string& s, const std::string& suffix);
std::string Trim(const std::string& s);
std::vector<std::string> Split(const std::string& s, char sep);
std::vector<std::string> SplitWhitespace(const std::string& s);
std::string ReplaceAll(std::string s, const std::string& from,
                       const std::string& to);
std::string CollapseWhitespace(const std::string& s);
std::string IntToString(long long v);
std::string DoubleToString(double v);
bool ParseInt(const std::string& s, long long& out);
double ParseDoublePrefix(const std::string& s, size_t& consumed);

std::string Base64Decode(const std::string& in);
std::string PercentDecode(const std::string& in, bool plusIsSpace);
std::string PercentEncodeForm(const std::string& in);  // x-www-form-urlencoded
std::string HtmlEscape(const std::string& in);

}  // namespace kite

#endif
