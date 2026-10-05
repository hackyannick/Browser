#include "html/entities.h"

#include <cstring>

namespace kite {

namespace {

struct Entity {
  const char* name;
  Codepoint cp;
};

// The most common named character references. Sorted by name.
const Entity kEntities[] = {
    {"AElig", 0xC6},   {"Aacute", 0xC1}, {"Acirc", 0xC2},  {"Agrave", 0xC0},
    {"Alpha", 0x391},  {"Aring", 0xC5},  {"Atilde", 0xC3}, {"Auml", 0xC4},
    {"Beta", 0x392},   {"Ccedil", 0xC7}, {"Chi", 0x3A7},   {"Dagger", 0x2021},
    {"Delta", 0x394},  {"ETH", 0xD0},    {"Eacute", 0xC9}, {"Ecirc", 0xCA},
    {"Egrave", 0xC8},  {"Epsilon", 0x395}, {"Eta", 0x397}, {"Euml", 0xCB},
    {"Gamma", 0x393},  {"Iacute", 0xCD}, {"Icirc", 0xCE},  {"Igrave", 0xCC},
    {"Iota", 0x399},   {"Iuml", 0xCF},   {"Kappa", 0x39A}, {"Lambda", 0x39B},
    {"Mu", 0x39C},     {"Ntilde", 0xD1}, {"Nu", 0x39D},    {"OElig", 0x152},
    {"Oacute", 0xD3},  {"Ocirc", 0xD4},  {"Ograve", 0xD2}, {"Omega", 0x3A9},
    {"Omicron", 0x39F}, {"Oslash", 0xD8}, {"Otilde", 0xD5}, {"Ouml", 0xD6},
    {"Phi", 0x3A6},    {"Pi", 0x3A0},    {"Prime", 0x2033}, {"Psi", 0x3A8},
    {"Rho", 0x3A1},    {"Scaron", 0x160}, {"Sigma", 0x3A3}, {"THORN", 0xDE},
    {"Tau", 0x3A4},    {"Theta", 0x398}, {"Uacute", 0xDA}, {"Ucirc", 0xDB},
    {"Ugrave", 0xD9},  {"Upsilon", 0x3A5}, {"Uuml", 0xDC}, {"Xi", 0x39E},
    {"Yacute", 0xDD},  {"Yuml", 0x178},  {"Zeta", 0x396},  {"aacute", 0xE1},
    {"acirc", 0xE2},   {"acute", 0xB4},  {"aelig", 0xE6},  {"agrave", 0xE0},
    {"alpha", 0x3B1},  {"amp", 0x26},    {"and", 0x2227},  {"ang", 0x2220},
    {"apos", 0x27},    {"aring", 0xE5},  {"asymp", 0x2248}, {"atilde", 0xE3},
    {"auml", 0xE4},    {"bdquo", 0x201E}, {"beta", 0x3B2}, {"brvbar", 0xA6},
    {"bull", 0x2022},  {"cap", 0x2229},  {"ccedil", 0xE7}, {"cedil", 0xB8},
    {"cent", 0xA2},    {"check", 0x2713}, {"chi", 0x3C7},  {"circ", 0x2C6},
    {"clubs", 0x2663}, {"colon", 0x3A},  {"comma", 0x2C},  {"cong", 0x2245},
    {"copy", 0xA9},    {"crarr", 0x21B5}, {"cup", 0x222A}, {"curren", 0xA4},
    {"dArr", 0x21D3},  {"dagger", 0x2020}, {"darr", 0x2193}, {"deg", 0xB0},
    {"delta", 0x3B4},  {"diams", 0x2666}, {"divide", 0xF7}, {"dollar", 0x24},
    {"eacute", 0xE9},  {"ecirc", 0xEA},  {"egrave", 0xE8}, {"empty", 0x2205},
    {"emsp", 0x2003},  {"ensp", 0x2002}, {"epsilon", 0x3B5}, {"equiv", 0x2261},
    {"eta", 0x3B7},    {"eth", 0xF0},    {"euml", 0xEB},   {"euro", 0x20AC},
    {"excl", 0x21},    {"exist", 0x2203}, {"fnof", 0x192}, {"forall", 0x2200},
    {"frac12", 0xBD},  {"frac14", 0xBC}, {"frac34", 0xBE}, {"frasl", 0x2044},
    {"gamma", 0x3B3},  {"ge", 0x2265},   {"gt", 0x3E},     {"hArr", 0x21D4},
    {"harr", 0x2194},  {"hearts", 0x2665}, {"hellip", 0x2026}, {"hyphen", 0x2010},
    {"iacute", 0xED},  {"icirc", 0xEE},  {"iexcl", 0xA1},  {"igrave", 0xEC},
    {"infin", 0x221E}, {"int", 0x222B},  {"iota", 0x3B9},  {"iquest", 0xBF},
    {"isin", 0x2208},  {"iuml", 0xEF},   {"kappa", 0x3BA}, {"lArr", 0x21D0},
    {"lambda", 0x3BB}, {"lang", 0x27E8}, {"laquo", 0xAB},  {"larr", 0x2190},
    {"lceil", 0x2308}, {"ldquo", 0x201C}, {"le", 0x2264},  {"lfloor", 0x230A},
    {"lowast", 0x2217}, {"loz", 0x25CA}, {"lpar", 0x28},   {"lrm", 0x200E},
    {"lsaquo", 0x2039}, {"lsqb", 0x5B},  {"lsquo", 0x2018}, {"lt", 0x3C},
    {"macr", 0xAF},    {"mdash", 0x2014}, {"micro", 0xB5}, {"middot", 0xB7},
    {"minus", 0x2212}, {"mu", 0x3BC},    {"nabla", 0x2207}, {"nbsp", 0xA0},
    {"ndash", 0x2013}, {"ne", 0x2260},   {"ni", 0x220B},   {"not", 0xAC},
    {"notin", 0x2209}, {"nsub", 0x2284}, {"ntilde", 0xF1}, {"nu", 0x3BD},
    {"num", 0x23},     {"oacute", 0xF3}, {"ocirc", 0xF4},  {"oelig", 0x153},
    {"ograve", 0xF2},  {"oline", 0x203E}, {"omega", 0x3C9}, {"omicron", 0x3BF},
    {"oplus", 0x2295}, {"or", 0x2228},   {"ordf", 0xAA},   {"ordm", 0xBA},
    {"oslash", 0xF8},  {"otilde", 0xF5}, {"otimes", 0x2297}, {"ouml", 0xF6},
    {"para", 0xB6},    {"part", 0x2202}, {"percnt", 0x25}, {"period", 0x2E},
    {"permil", 0x2030}, {"perp", 0x22A5}, {"phi", 0x3C6},  {"pi", 0x3C0},
    {"piv", 0x3D6},    {"plus", 0x2B},   {"plusmn", 0xB1}, {"pound", 0xA3},
    {"prime", 0x2032}, {"prod", 0x220F}, {"prop", 0x221D}, {"psi", 0x3C8},
    {"quest", 0x3F},   {"quot", 0x22},   {"rArr", 0x21D2}, {"radic", 0x221A},
    {"rang", 0x27E9},  {"raquo", 0xBB},  {"rarr", 0x2192}, {"rceil", 0x2309},
    {"rdquo", 0x201D}, {"reg", 0xAE},    {"rfloor", 0x230B}, {"rho", 0x3C1},
    {"rlm", 0x200F},   {"rpar", 0x29},   {"rsaquo", 0x203A}, {"rsqb", 0x5D},
    {"rsquo", 0x2019}, {"sbquo", 0x201A}, {"scaron", 0x161}, {"sdot", 0x22C5},
    {"sect", 0xA7},    {"semi", 0x3B},   {"shy", 0xAD},    {"sigma", 0x3C3},
    {"sigmaf", 0x3C2}, {"sim", 0x223C},  {"sol", 0x2F},    {"spades", 0x2660},
    {"sub", 0x2282},   {"sube", 0x2286}, {"sum", 0x2211},  {"sup", 0x2283},
    {"sup1", 0xB9},    {"sup2", 0xB2},   {"sup3", 0xB3},   {"supe", 0x2287},
    {"szlig", 0xDF},   {"tau", 0x3C4},   {"there4", 0x2234}, {"theta", 0x3B8},
    {"thetasym", 0x3D1}, {"thinsp", 0x2009}, {"thorn", 0xFE}, {"tilde", 0x2DC},
    {"times", 0xD7},   {"trade", 0x2122}, {"uArr", 0x21D1}, {"uacute", 0xFA},
    {"uarr", 0x2191},  {"ucirc", 0xFB},  {"ugrave", 0xF9}, {"uml", 0xA8},
    {"upsih", 0x3D2},  {"upsilon", 0x3C5}, {"uuml", 0xFC}, {"xi", 0x3BE},
    {"yacute", 0xFD},  {"yen", 0xA5},    {"yuml", 0xFF},   {"zeta", 0x3B6},
    {"zwj", 0x200D},   {"zwnj", 0x200C},
};

}  // namespace

bool LookupEntity(const std::string& name, Codepoint& out) {
  size_t lo = 0, hi = sizeof(kEntities) / sizeof(kEntities[0]);
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    int c = strcmp(name.c_str(), kEntities[mid].name);
    if (c == 0) {
      out = kEntities[mid].cp;
      return true;
    }
    if (c < 0) hi = mid;
    else lo = mid + 1;
  }
  return false;
}

static Codepoint FixNumericRef(unsigned long v) {
  static const Codepoint c1[32] = {
      0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
      0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
  if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0xFFFD;
  if (v >= 0x80 && v <= 0x9F) return c1[v - 0x80];
  return (Codepoint)v;
}

std::string DecodeEntities(const std::string& in, bool inAttribute) {
  if (in.find('&') == std::string::npos) return in;
  std::string out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    if (in[i] != '&') {
      out += in[i++];
      continue;
    }
    size_t j = i + 1;
    if (j < in.size() && in[j] == '#') {
      ++j;
      bool hex = false;
      if (j < in.size() && (in[j] == 'x' || in[j] == 'X')) {
        hex = true;
        ++j;
      }
      size_t start = j;
      unsigned long v = 0;
      while (j < in.size() &&
             (hex ? IsAsciiHex((unsigned char)in[j]) : IsAsciiDigit((unsigned char)in[j]))) {
        if (v < 0x110000) v = v * (hex ? 16 : 10) + HexValue(in[j]);
        ++j;
      }
      if (j == start) {
        out += in[i++];
        continue;
      }
      if (j < in.size() && in[j] == ';') ++j;
      Utf8Append(out, FixNumericRef(v));
      i = j;
      continue;
    }
    size_t start = j;
    while (j < in.size() && j - start < 32 &&
           (IsAsciiAlpha((unsigned char)in[j]) || IsAsciiDigit((unsigned char)in[j])))
      ++j;
    std::string name = in.substr(start, j - start);
    Codepoint cp;
    bool semi = j < in.size() && in[j] == ';';
    if (!name.empty() && LookupEntity(name, cp) && (semi || !inAttribute)) {
      Utf8Append(out, cp);
      i = semi ? j + 1 : j;
      continue;
    }
    // Legacy: "&ampfoo" style prefixes without semicolon (text only).
    bool matched = false;
    if (!inAttribute && !semi) {
      for (size_t len = name.size(); len >= 2 && !matched; --len) {
        if (LookupEntity(name.substr(0, len), cp)) {
          static const char* legacy[] = {"amp", "lt", "gt", "quot", "nbsp", "copy", "reg", 0};
          for (int k = 0; legacy[k]; ++k) {
            if (name.substr(0, len) == legacy[k]) {
              Utf8Append(out, cp);
              i = start + len;
              matched = true;
              break;
            }
          }
        }
      }
    }
    if (!matched) out += in[i++];
  }
  return out;
}

}  // namespace kite
