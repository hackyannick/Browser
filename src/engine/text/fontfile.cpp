#include "text/fontfile.h"

#include <cstdlib>
#include <vector>

#include "base/strings.h"

extern "C" {
char* stbi_zlib_decode_malloc_guesssize_headerflag(const char* buffer, int len, int initial_size,
                                                    int* outlen, int parse_header);
}

namespace kite {

namespace {

unsigned U16(const std::string& s, size_t p) {
  if (p + 2 > s.size()) return 0;
  return ((unsigned char)s[p] << 8) | (unsigned char)s[p + 1];
}

unsigned long U32(const std::string& s, size_t p) {
  if (p + 4 > s.size()) return 0;
  return ((unsigned long)(unsigned char)s[p] << 24) | ((unsigned long)(unsigned char)s[p + 1] << 16) |
         ((unsigned long)(unsigned char)s[p + 2] << 8) | (unsigned char)s[p + 3];
}

void Put16(std::string& s, unsigned v) {
  s += char((v >> 8) & 255);
  s += char(v & 255);
}

void Put32(std::string& s, unsigned long v) {
  s += char((v >> 24) & 255);
  s += char((v >> 16) & 255);
  s += char((v >> 8) & 255);
  s += char(v & 255);
}

}  // namespace

bool FontToSfnt(const std::string& data, std::string& out) {
  if (data.size() < 12) return false;
  std::string sig = data.substr(0, 4);
  if (sig == "wOF2") return false;
  if (sig != "wOFF") {
    unsigned long v = U32(data, 0);
    if (v == 0x00010000UL || sig == "OTTO" || sig == "true") {
      out = data;
      return true;
    }
    return false;
  }
  unsigned long flavor = U32(data, 4);
  unsigned numTables = U16(data, 12);
  if (numTables == 0 || numTables > 200 || data.size() < 44 + numTables * 20) return false;
  struct Table {
    unsigned long tag, offset, compLength, origLength, checksum;
  };
  std::vector<Table> tables(numTables);
  for (unsigned i = 0; i < numTables; ++i) {
    size_t p = 44 + i * 20;
    tables[i].tag = U32(data, p);
    tables[i].offset = U32(data, p + 4);
    tables[i].compLength = U32(data, p + 8);
    tables[i].origLength = U32(data, p + 12);
    tables[i].checksum = U32(data, p + 16);
  }
  unsigned searchRange = 1, entrySelector = 0;
  while (searchRange * 2 <= numTables) {
    searchRange *= 2;
    ++entrySelector;
  }
  searchRange *= 16;
  out.clear();
  Put32(out, flavor);
  Put16(out, numTables);
  Put16(out, searchRange);
  Put16(out, entrySelector);
  Put16(out, numTables * 16 - searchRange);
  size_t dataStart = 12 + numTables * 16;
  std::string body;
  std::string dir;
  for (unsigned i = 0; i < numTables; ++i) {
    const Table& t = tables[i];
    if (t.offset + t.compLength > data.size()) return false;
    std::string table;
    if (t.compLength < t.origLength) {
      int len = 0;
      char* r = stbi_zlib_decode_malloc_guesssize_headerflag(data.data() + t.offset, (int)t.compLength,
                                                              (int)t.origLength, &len, 1);
      if (!r) return false;
      table.assign(r, len);
      free(r);
      if (table.size() != t.origLength) return false;
    } else {
      table = data.substr(t.offset, t.origLength);
    }
    Put32(dir, t.tag);
    Put32(dir, t.checksum);
    Put32(dir, (unsigned long)(dataStart + body.size()));
    Put32(dir, (unsigned long)table.size());
    body += table;
    while (body.size() % 4) body += '\0';
  }
  out += dir;
  out += body;
  return true;
}

std::string SfntFamilyName(const std::string& s) {
  if (s.size() < 12) return std::string();
  unsigned numTables = U16(s, 4);
  for (unsigned i = 0; i < numTables; ++i) {
    size_t rec = 12 + i * 16;
    if (s.compare(rec, 4, "name") != 0) continue;
    size_t off = U32(s, rec + 8);
    unsigned count = U16(s, off + 2);
    size_t strings = off + U16(s, off + 4);
    std::string best;
    int bestScore = -1;
    for (unsigned k = 0; k < count; ++k) {
      size_t r = off + 6 + k * 12;
      unsigned platform = U16(s, r), encoding = U16(s, r + 2), lang = U16(s, r + 4);
      unsigned nameId = U16(s, r + 6), len = U16(s, r + 8), so = U16(s, r + 10);
      if (nameId != 1 || strings + so + len > s.size()) continue;
      std::string raw = s.substr(strings + so, len);
      std::string name;
      int score;
      if (platform == 3 || platform == 0) {
        std::u16string u;
        for (size_t j = 0; j + 1 < raw.size(); j += 2)
          u += char16_t(((unsigned char)raw[j] << 8) | (unsigned char)raw[j + 1]);
        name = Utf16ToUtf8(u);
        score = (platform == 3 && (encoding == 1 || encoding == 0)) ? (lang == 0x409 ? 3 : 2) : 1;
      } else {
        name = raw;
        score = 0;
      }
      if (score > bestScore && !name.empty()) {
        best = name;
        bestScore = score;
      }
    }
    return best;
  }
  return std::string();
}

}  // namespace kite
