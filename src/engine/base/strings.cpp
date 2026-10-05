#include "base/strings.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace kite {

Codepoint Utf8Next(const std::string& s, size_t& i) {
  unsigned char c = (unsigned char)s[i];
  if (c < 0x80) { ++i; return c; }
  int len = 0;
  Codepoint cp = 0;
  if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
  else { ++i; return 0xFFFD; }
  if (i + len > s.size()) { ++i; return 0xFFFD; }
  for (int k = 1; k < len; ++k) {
    unsigned char cc = (unsigned char)s[i + k];
    if ((cc & 0xC0) != 0x80) { ++i; return 0xFFFD; }
    cp = (cp << 6) | (cc & 0x3F);
  }
  // Reject overlong encodings and surrogates.
  if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
      (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
      (cp >= 0xD800 && cp <= 0xDFFF)) {
    ++i;
    return 0xFFFD;
  }
  i += len;
  return cp;
}

void Utf8Append(std::string& out, Codepoint cp) {
  if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp <= 0x10FFFF) {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    Utf8Append(out, 0xFFFD);
  }
}

std::u16string Utf8ToUtf16(const std::string& s) {
  std::u16string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    Codepoint cp = Utf8Next(s, i);
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out += char16_t(0xD800 + (cp >> 10));
      out += char16_t(0xDC00 + (cp & 0x3FF));
    } else {
      out += char16_t(cp);
    }
  }
  return out;
}

std::string Utf16ToUtf8(const std::u16string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    Codepoint c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 &&
        s[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    }
    Utf8Append(out, c);
  }
  return out;
}

size_t Utf8Length(const std::string& s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size(); ++i)
    if (((unsigned char)s[i] & 0xC0) != 0x80) ++n;
  return n;
}

bool LooksLikeValidUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { ++i; continue; }
    size_t before = i;
    Codepoint cp = Utf8Next(s, i);
    if (cp == 0xFFFD && i == before + 1) return false;
  }
  return true;
}

// Windows-1252 bytes 0x80..0x9F.
static const Codepoint kCp1252High[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

std::string ConvertToUtf8(const std::string& bytes, const std::string& rawCharset) {
  std::string cs = AsciiLower(Trim(rawCharset));
  std::string out;
  if (cs == "utf-16le" || cs == "utf-16be" || cs == "utf-16") {
    bool be = cs == "utf-16be";
    size_t start = 0;
    if (bytes.size() >= 2) {
      unsigned char a = bytes[0], b = bytes[1];
      if (a == 0xFE && b == 0xFF) { be = true; start = 2; }
      else if (a == 0xFF && b == 0xFE) { be = false; start = 2; }
    }
    std::u16string u;
    for (size_t i = start; i + 1 < bytes.size(); i += 2) {
      unsigned char a = bytes[i], b = bytes[i + 1];
      u += be ? char16_t((a << 8) | b) : char16_t((b << 8) | a);
    }
    return Utf16ToUtf8(u);
  }
  bool latin = cs == "iso-8859-1" || cs == "latin1" || cs == "l1" ||
               cs == "windows-1252" || cs == "cp1252" || cs == "us-ascii" ||
               cs == "ascii" || cs == "iso-8859-15" || cs == "latin-1" ||
               cs == "x-cp1252";
  if (!latin) {
    // UTF-8 (or unknown). Strip BOM. If the data is not valid UTF-8 fall
    // back to windows-1252 which is what most legacy pages are.
    size_t start = 0;
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
      start = 3;
    std::string body = bytes.substr(start);
    if (cs.empty() || cs == "utf-8" || cs == "utf8" || LooksLikeValidUtf8(body)) {
      if (LooksLikeValidUtf8(body)) return body;
      // Decode leniently.
      size_t i = 0;
      while (i < body.size()) Utf8Append(out, Utf8Next(body, i));
      return out;
    }
  }
  bool is15 = cs == "iso-8859-15";
  out.reserve(bytes.size() + bytes.size() / 8);
  for (size_t i = 0; i < bytes.size(); ++i) {
    unsigned char c = (unsigned char)bytes[i];
    Codepoint cp = c;
    if (c >= 0x80 && c <= 0x9F && !is15) cp = kCp1252High[c - 0x80];
    if (is15) {
      switch (c) {
        case 0xA4: cp = 0x20AC; break;
        case 0xA6: cp = 0x0160; break;
        case 0xA8: cp = 0x0161; break;
        case 0xB4: cp = 0x017D; break;
        case 0xB8: cp = 0x017E; break;
        case 0xBC: cp = 0x0152; break;
        case 0xBD: cp = 0x0153; break;
        case 0xBE: cp = 0x0178; break;
      }
    }
    Utf8Append(out, cp);
  }
  return out;
}

std::string AsciiLower(const std::string& s) {
  std::string r(s);
  for (size_t i = 0; i < r.size(); ++i) r[i] = AsciiLowerChar(r[i]);
  return r;
}

std::string AsciiUpper(const std::string& s) {
  std::string r(s);
  for (size_t i = 0; i < r.size(); ++i)
    if (r[i] >= 'a' && r[i] <= 'z') r[i] = char(r[i] - 32);
  return r;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (AsciiLowerChar(a[i]) != AsciiLowerChar(b[i])) return false;
  return true;
}

bool StartsWith(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool StartsWithIgnoreCase(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && EqualsIgnoreCase(s.substr(0, p.size()), p);
}

bool EndsWith(const std::string& s, const std::string& p) {
  return s.size() >= p.size() &&
         s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && IsAsciiSpace((unsigned char)s[a])) ++a;
  while (b > a && IsAsciiSpace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    size_t p = s.find(sep, start);
    if (p == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, p - start));
    start = p + 1;
  }
  return out;
}

std::vector<std::string> SplitWhitespace(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && IsAsciiSpace((unsigned char)s[i])) ++i;
    size_t start = i;
    while (i < s.size() && !IsAsciiSpace((unsigned char)s[i])) ++i;
    if (i > start) out.push_back(s.substr(start, i - start));
  }
  return out;
}

std::string ReplaceAll(std::string s, const std::string& from,
                       const std::string& to) {
  if (from.empty()) return s;
  size_t p = 0;
  while ((p = s.find(from, p)) != std::string::npos) {
    s.replace(p, from.size(), to);
    p += to.size();
  }
  return s;
}

std::string CollapseWhitespace(const std::string& s) {
  std::string out;
  bool space = false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (IsAsciiSpace((unsigned char)s[i])) {
      space = true;
    } else {
      if (space && !out.empty()) out += ' ';
      space = false;
      out += s[i];
    }
  }
  return out;
}

std::string IntToString(long long v) {
  char buf[32];
  snprintf(buf, sizeof buf, "%lld", v);
  return buf;
}

std::string DoubleToString(double v) {
  char buf[64];
  snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

bool ParseInt(const std::string& raw, long long& out) {
  std::string s = Trim(raw);
  if (s.empty()) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
  if (i >= s.size() || !IsAsciiDigit((unsigned char)s[i])) return false;
  long long v = 0;
  while (i < s.size() && IsAsciiDigit((unsigned char)s[i])) {
    if (v < 100000000000000LL) v = v * 10 + (s[i] - '0');
    ++i;
  }
  out = neg ? -v : v;
  return true;
}

// Locale independent strtod for CSS/HTML numbers: [+-]digits[.digits][e[+-]digits]
double ParseDoublePrefix(const std::string& s, size_t& consumed) {
  size_t i = 0;
  double sign = 1;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    if (s[i] == '-') sign = -1;
    ++i;
  }
  double v = 0;
  bool any = false;
  while (i < s.size() && IsAsciiDigit((unsigned char)s[i])) {
    v = v * 10 + (s[i] - '0');
    ++i;
    any = true;
  }
  if (i < s.size() && s[i] == '.' && i + 1 < s.size() &&
      IsAsciiDigit((unsigned char)s[i + 1])) {
    ++i;
    double scale = 0.1;
    while (i < s.size() && IsAsciiDigit((unsigned char)s[i])) {
      v += (s[i] - '0') * scale;
      scale *= 0.1;
      ++i;
      any = true;
    }
  }
  if (!any) {
    consumed = 0;
    return 0;
  }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    size_t j = i + 1;
    int esign = 1;
    if (j < s.size() && (s[j] == '+' || s[j] == '-')) {
      if (s[j] == '-') esign = -1;
      ++j;
    }
    if (j < s.size() && IsAsciiDigit((unsigned char)s[j])) {
      int e = 0;
      while (j < s.size() && IsAsciiDigit((unsigned char)s[j])) {
        if (e < 400) e = e * 10 + (s[j] - '0');
        ++j;
      }
      v *= std::pow(10.0, esign * e);
      i = j;
    }
  }
  consumed = i;
  return sign * v;
}

std::string Base64Encode(const std::string& in) {
  static const char* kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    unsigned n = (unsigned char)in[i] << 16;
    if (i + 1 < in.size()) n |= (unsigned char)in[i + 1] << 8;
    if (i + 2 < in.size()) n |= (unsigned char)in[i + 2];
    out += kChars[(n >> 18) & 63];
    out += kChars[(n >> 12) & 63];
    out += i + 1 < in.size() ? kChars[(n >> 6) & 63] : '=';
    out += i + 2 < in.size() ? kChars[n & 63] : '=';
  }
  return out;
}

std::string Base64Decode(const std::string& in) {
  std::string out;
  unsigned int buf = 0;
  int bits = 0;
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else continue;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += char((buf >> bits) & 0xFF);
    }
  }
  return out;
}

std::string PercentDecode(const std::string& in, bool plusIsSpace) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size() &&
        IsAsciiHex((unsigned char)in[i + 1]) && IsAsciiHex((unsigned char)in[i + 2])) {
      out += char(HexValue(in[i + 1]) * 16 + HexValue(in[i + 2]));
      i += 2;
    } else if (plusIsSpace && in[i] == '+') {
      out += ' ';
    } else {
      out += in[i];
    }
  }
  return out;
}

std::string PercentEncodeForm(const std::string& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    unsigned char c = (unsigned char)in[i];
    if (IsAsciiAlpha(c) || IsAsciiDigit(c) || c == '*' || c == '-' ||
        c == '.' || c == '_') {
      out += char(c);
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string HtmlEscape(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    switch (in[i]) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      default: out += in[i];
    }
  }
  return out;
}

}  // namespace kite
