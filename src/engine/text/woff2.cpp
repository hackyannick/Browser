// WOFF2 → sfnt conversion (W3C WOFF2 specification): Brotli decompression,
// reconstruction of the transformed glyf/loca and hmtx tables.
#include "text/woff2.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <brotli/decode.h>

namespace kite {

namespace {

const char* const kKnownTags[63] = {
    "cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ", "fpgm", "glyf",
    "loca", "prep", "CFF ", "VORG", "EBDT", "EBLC", "gasp", "hdmx", "kern", "LTSH", "PCLT",
    "VDMX", "vhea", "vmtx", "BASE", "GDEF", "GPOS", "GSUB", "EBSC", "JSTF", "MATH", "CBDT",
    "CBLC", "COLR", "CPAL", "SVG ", "sbix", "acnt", "avar", "bdat", "bloc", "bsln", "cvar",
    "fdsc", "feat", "fmtx", "fvar", "gvar", "hsty", "just", "lcar", "mort", "morx", "opbd",
    "prop", "trak", "Zapf", "Silf", "Glat", "Gloc", "Feat", "Sill"};

uint32_t Tag(const char* t) {
  return ((uint32_t)(uint8_t)t[0] << 24) | ((uint32_t)(uint8_t)t[1] << 16) |
         ((uint32_t)(uint8_t)t[2] << 8) | (uint8_t)t[3];
}

const uint32_t kGlyf = 0x676c7966, kLoca = 0x6c6f6361, kHmtx = 0x686d7478, kHead = 0x68656164,
               kHhea = 0x68686561;

// Bounds-checked big-endian reader.
class Reader {
 public:
  Reader(const uint8_t* d, size_t n) : d_(d), n_(n), p_(0) {}
  bool U8(uint8_t& v) {
    if (p_ + 1 > n_) return false;
    v = d_[p_++];
    return true;
  }
  bool U16(uint16_t& v) {
    if (p_ + 2 > n_) return false;
    v = (uint16_t)((d_[p_] << 8) | d_[p_ + 1]);
    p_ += 2;
    return true;
  }
  bool S16(int16_t& v) {
    uint16_t u;
    if (!U16(u)) return false;
    v = (int16_t)u;
    return true;
  }
  bool U32(uint32_t& v) {
    if (p_ + 4 > n_) return false;
    v = ((uint32_t)d_[p_] << 24) | ((uint32_t)d_[p_ + 1] << 16) | ((uint32_t)d_[p_ + 2] << 8) | d_[p_ + 3];
    p_ += 4;
    return true;
  }
  bool Base128(uint32_t& v) {
    uint32_t acc = 0;
    for (int i = 0; i < 5; ++i) {
      uint8_t b;
      if (!U8(b)) return false;
      if (i == 0 && b == 0x80) return false;  // leading zeros
      if (acc & 0xFE000000u) return false;    // overflow
      acc = (acc << 7) | (b & 0x7F);
      if (!(b & 0x80)) {
        v = acc;
        return true;
      }
    }
    return false;
  }
  bool Read255U16(uint16_t& v) {
    uint8_t code;
    if (!U8(code)) return false;
    if (code == 253) return U16(v);
    if (code == 255 || code == 254) {
      uint8_t b;
      if (!U8(b)) return false;
      v = (uint16_t)(b + (code == 255 ? 253 : 506));
      return true;
    }
    v = code;
    return true;
  }
  bool Bytes(const uint8_t*& out, size_t len) {
    if (len > n_ - p_) return false;
    out = d_ + p_;
    p_ += len;
    return true;
  }
  bool Skip(size_t len) {
    if (len > n_ - p_) return false;
    p_ += len;
    return true;
  }
  size_t pos() const { return p_; }
  size_t remaining() const { return n_ - p_; }
  const uint8_t* at() const { return d_ + p_; }

 private:
  const uint8_t* d_;
  size_t n_, p_;
};

void Put16(std::string& s, unsigned v) {
  s += char((v >> 8) & 255);
  s += char(v & 255);
}

void Put32(std::string& s, uint32_t v) {
  s += char((v >> 24) & 255);
  s += char((v >> 16) & 255);
  s += char((v >> 8) & 255);
  s += char(v & 255);
}

int WithSign(int flag, int base) { return (flag & 1) ? base : -base; }

struct Point {
  int x, y;
  bool onCurve;
};

// Decodes the coordinate triplets of one simple glyph.
bool DecodeTriplets(const uint8_t* flags, Reader& glyphs, size_t nPoints, std::vector<Point>& pts) {
  pts.resize(nPoints);
  int x = 0, y = 0;
  for (size_t i = 0; i < nPoints; ++i) {
    int flag = flags[i];
    bool onCurve = !(flag >> 7);
    flag &= 0x7F;
    size_t n = flag < 84 ? 1 : flag < 120 ? 2 : flag < 124 ? 3 : 4;
    const uint8_t* in;
    if (!glyphs.Bytes(in, n)) return false;
    int dx, dy;
    if (flag < 10) {
      dx = 0;
      dy = WithSign(flag, ((flag & 14) << 7) + in[0]);
    } else if (flag < 20) {
      dx = WithSign(flag, (((flag - 10) & 14) << 7) + in[0]);
      dy = 0;
    } else if (flag < 84) {
      int b0 = flag - 20, b1 = in[0];
      dx = WithSign(flag, 1 + (b0 & 0x30) + (b1 >> 4));
      dy = WithSign(flag >> 1, 1 + ((b0 & 0x0C) << 2) + (b1 & 0x0F));
    } else if (flag < 120) {
      int b0 = flag - 84;
      dx = WithSign(flag, 1 + ((b0 / 12) << 8) + in[0]);
      dy = WithSign(flag >> 1, 1 + (((b0 % 12) >> 2) << 8) + in[1]);
    } else if (flag < 124) {
      int b2 = in[1];
      dx = WithSign(flag, (in[0] << 4) + (b2 >> 4));
      dy = WithSign(flag >> 1, ((b2 & 0x0F) << 8) + in[2]);
    } else {
      dx = WithSign(flag, (in[0] << 8) + in[1]);
      dy = WithSign(flag >> 1, (in[2] << 8) + in[3]);
    }
    x += dx;
    y += dy;
    pts[i].x = x;
    pts[i].y = y;
    pts[i].onCurve = onCurve;
  }
  return true;
}

// Rebuilds glyf and loca from the transformed glyf table.
bool ReconstructGlyf(const uint8_t* data, size_t size, std::string& glyf, std::string& loca,
                     std::vector<int16_t>& xMins, unsigned& numGlyphsOut) {
  Reader hdr(data, size);
  uint16_t reserved, optionFlags, numGlyphs, indexFormat;
  uint32_t sizes[7];
  if (!hdr.U16(reserved) || !hdr.U16(optionFlags) || !hdr.U16(numGlyphs) || !hdr.U16(indexFormat))
    return false;
  for (int i = 0; i < 7; ++i)
    if (!hdr.U32(sizes[i])) return false;
  size_t offset = hdr.pos();
  const uint8_t* streams[7];
  for (int i = 0; i < 7; ++i) {
    if (sizes[i] > size - offset) return false;
    streams[i] = data + offset;
    offset += sizes[i];
  }
  const uint8_t* overlap = 0;
  if (optionFlags & 1) {
    size_t n = (numGlyphs + 7) / 8;
    if (n > size - offset) return false;
    overlap = data + offset;
  }
  Reader nContourR(streams[0], sizes[0]), nPointsR(streams[1], sizes[1]), flagR(streams[2], sizes[2]),
      glyphR(streams[3], sizes[3]), compositeR(streams[4], sizes[4]), bboxR(streams[5], sizes[5]),
      instrR(streams[6], sizes[6]);
  size_t bitmapLen = 4 * ((numGlyphs + 31) / 32);
  const uint8_t* bboxBitmap;
  if (!bboxR.Bytes(bboxBitmap, bitmapLen)) return false;

  glyf.clear();
  loca.clear();
  xMins.assign(numGlyphs, 0);
  std::vector<Point> pts;
  std::vector<uint16_t> endPts;
  for (unsigned g = 0; g < numGlyphs; ++g) {
    Put32(loca, (uint32_t)glyf.size());
    int16_t nContours;
    if (!nContourR.S16(nContours)) return false;
    bool hasBbox = (bboxBitmap[g >> 3] & (0x80 >> (g & 7))) != 0;
    int16_t bbox[4] = {0, 0, 0, 0};
    if (hasBbox)
      for (int k = 0; k < 4; ++k)
        if (!bboxR.S16(bbox[k])) return false;
    if (nContours == 0) {
      if (hasBbox) return false;
      continue;
    }
    std::string out;
    if (nContours == -1) {
      // Composite glyph: copy component records verbatim.
      if (!hasBbox) return false;
      size_t start = compositeR.pos();
      bool haveInstructions = false;
      uint16_t flags;
      do {
        uint16_t glyphIndex;
        if (!compositeR.U16(flags) || !compositeR.U16(glyphIndex)) return false;
        size_t argBytes = (flags & 0x0001) ? 4 : 2;
        if (flags & 0x0008) argBytes += 2;
        else if (flags & 0x0040) argBytes += 4;
        else if (flags & 0x0080) argBytes += 8;
        if (!compositeR.Skip(argBytes)) return false;
        if (flags & 0x0100) haveInstructions = true;
      } while (flags & 0x0020);
      size_t compLen = compositeR.pos() - start;
      Put16(out, 0xFFFF);
      for (int k = 0; k < 4; ++k) Put16(out, (uint16_t)bbox[k]);
      out.append((const char*)streams[4] + start, compLen);
      if (haveInstructions) {
        uint16_t instrLen;
        const uint8_t* instr;
        if (!glyphR.Read255U16(instrLen) || !instrR.Bytes(instr, instrLen)) return false;
        Put16(out, instrLen);
        out.append((const char*)instr, instrLen);
      }
      xMins[g] = bbox[0];
    } else if (nContours > 0) {
      endPts.resize(nContours);
      size_t total = 0;
      for (int c = 0; c < nContours; ++c) {
        uint16_t n;
        if (!nPointsR.Read255U16(n)) return false;
        total += n;
        if (total > 0xFFFF) return false;
        endPts[c] = (uint16_t)(total - 1);
      }
      const uint8_t* flags;
      if (!flagR.Bytes(flags, total)) return false;
      if (!DecodeTriplets(flags, glyphR, total, pts)) return false;
      uint16_t instrLen;
      const uint8_t* instr;
      if (!glyphR.Read255U16(instrLen) || !instrR.Bytes(instr, instrLen)) return false;
      if (!hasBbox && total > 0) {
        bbox[0] = bbox[2] = (int16_t)pts[0].x;
        bbox[1] = bbox[3] = (int16_t)pts[0].y;
        for (size_t i = 1; i < total; ++i) {
          bbox[0] = (int16_t)std::min<int>(bbox[0], pts[i].x);
          bbox[1] = (int16_t)std::min<int>(bbox[1], pts[i].y);
          bbox[2] = (int16_t)std::max<int>(bbox[2], pts[i].x);
          bbox[3] = (int16_t)std::max<int>(bbox[3], pts[i].y);
        }
      }
      Put16(out, (uint16_t)nContours);
      for (int k = 0; k < 4; ++k) Put16(out, (uint16_t)bbox[k]);
      for (int c = 0; c < nContours; ++c) Put16(out, endPts[c]);
      Put16(out, instrLen);
      out.append((const char*)instr, instrLen);
      // Flags (on-curve bit only; both coordinates as 16-bit deltas).
      bool overlapBit = overlap && (overlap[g >> 3] & (0x80 >> (g & 7)));
      for (size_t i = 0; i < total; ++i)
        out += char((pts[i].onCurve ? 0x01 : 0x00) | (i == 0 && overlapBit ? 0x40 : 0x00));
      int last = 0;
      for (size_t i = 0; i < total; ++i) {
        Put16(out, (uint16_t)(int16_t)(pts[i].x - last));
        last = pts[i].x;
      }
      last = 0;
      for (size_t i = 0; i < total; ++i) {
        Put16(out, (uint16_t)(int16_t)(pts[i].y - last));
        last = pts[i].y;
      }
      xMins[g] = bbox[0];
    } else {
      return false;
    }
    glyf += out;
    while (glyf.size() % 4) glyf += '\0';
  }
  Put32(loca, (uint32_t)glyf.size());
  numGlyphsOut = numGlyphs;
  return true;
}

// Rebuilds hmtx (transform version 1): missing left side bearings equal xMin.
bool ReconstructHmtx(const uint8_t* data, size_t size, unsigned numGlyphs, unsigned numHMetrics,
                     const std::vector<int16_t>& xMins, std::string& out) {
  if (numHMetrics == 0 || numHMetrics > numGlyphs || xMins.size() < numGlyphs) return false;
  Reader r(data, size);
  uint8_t flags;
  if (!r.U8(flags)) return false;
  std::vector<uint16_t> adv(numHMetrics);
  std::vector<int16_t> lsb(numGlyphs);
  for (unsigned i = 0; i < numHMetrics; ++i)
    if (!r.U16(adv[i])) return false;
  for (unsigned i = 0; i < numHMetrics; ++i) {
    if (flags & 1) lsb[i] = xMins[i];
    else if (!r.S16(lsb[i])) return false;
  }
  for (unsigned i = numHMetrics; i < numGlyphs; ++i) {
    if (flags & 2) lsb[i] = xMins[i];
    else if (!r.S16(lsb[i])) return false;
  }
  out.clear();
  for (unsigned i = 0; i < numHMetrics; ++i) {
    Put16(out, adv[i]);
    Put16(out, (uint16_t)lsb[i]);
  }
  for (unsigned i = numHMetrics; i < numGlyphs; ++i) Put16(out, (uint16_t)lsb[i]);
  return true;
}

uint32_t Checksum(const std::string& t) {
  uint32_t sum = 0;
  for (size_t i = 0; i < t.size(); i += 4) {
    uint32_t v = 0;
    for (size_t k = 0; k < 4; ++k) v = (v << 8) | (i + k < t.size() ? (uint8_t)t[i + k] : 0);
    sum += v;
  }
  return sum;
}

}  // namespace

bool Woff2ToSfnt(const std::string& file, std::string& out) {
  const uint8_t* d = (const uint8_t*)file.data();
  Reader r(d, file.size());
  uint32_t signature, flavor, length, totalSfntSize, totalCompressedSize;
  uint16_t numTables, reserved, major, minor;
  if (!r.U32(signature) || signature != 0x774F4632 /* wOF2 */) return false;
  if (!r.U32(flavor) || !r.U32(length) || !r.U16(numTables) || !r.U16(reserved) ||
      !r.U32(totalSfntSize) || !r.U32(totalCompressedSize) || !r.U16(major) || !r.U16(minor) ||
      !r.Skip(20))
    return false;
  if (flavor == 0x74746366 /* ttcf: collections are not supported */) return false;
  if (numTables == 0 || numTables > 200) return false;

  struct Entry {
    uint32_t tag;
    uint32_t origLength;
    uint32_t streamLength;  // bytes in the decompressed stream
    bool transformed;
    std::string data;
  };
  std::vector<Entry> tables(numTables);
  uint64_t streamTotal = 0;
  for (unsigned i = 0; i < numTables; ++i) {
    Entry& e = tables[i];
    uint8_t flags;
    if (!r.U8(flags)) return false;
    unsigned idx = flags & 0x3F, version = flags >> 6;
    if (idx == 63) {
      if (!r.U32(e.tag)) return false;
    } else {
      e.tag = Tag(kKnownTags[idx]);
    }
    if (!r.Base128(e.origLength)) return false;
    bool glyfOrLoca = e.tag == kGlyf || e.tag == kLoca;
    e.transformed = glyfOrLoca ? version == 0 : version != 0;
    e.streamLength = e.origLength;
    if (e.transformed && !r.Base128(e.streamLength)) return false;
    if (e.tag == kLoca && e.transformed && e.streamLength != 0) return false;
    streamTotal += e.streamLength;
  }
  if (totalCompressedSize > r.remaining() || streamTotal > 64u * 1024 * 1024) return false;

  // One Brotli stream holds all table data.
  std::vector<uint8_t> stream((size_t)streamTotal);
  size_t decodedSize = stream.size();
  if (BrotliDecoderDecompress(totalCompressedSize, r.at(), &decodedSize, stream.data()) !=
          BROTLI_DECODER_RESULT_SUCCESS ||
      decodedSize != stream.size())
    return false;

  size_t pos = 0;
  Entry* glyfEntry = 0;
  Entry* locaEntry = 0;
  Entry* hmtxEntry = 0;
  Entry* headEntry = 0;
  Entry* hheaEntry = 0;
  for (unsigned i = 0; i < numTables; ++i) {
    Entry& e = tables[i];
    e.data.assign((const char*)stream.data() + pos, e.streamLength);
    pos += e.streamLength;
    if (e.tag == kGlyf) glyfEntry = &e;
    if (e.tag == kLoca) locaEntry = &e;
    if (e.tag == kHmtx) hmtxEntry = &e;
    if (e.tag == kHead) headEntry = &e;
    if (e.tag == kHhea) hheaEntry = &e;
  }

  std::vector<int16_t> xMins;
  unsigned numGlyphs = 0;
  if (glyfEntry && glyfEntry->transformed) {
    if (!locaEntry || !headEntry || headEntry->data.size() < 54) return false;
    std::string glyf, loca;
    if (!ReconstructGlyf((const uint8_t*)glyfEntry->data.data(), glyfEntry->data.size(), glyf, loca,
                         xMins, numGlyphs))
      return false;
    glyfEntry->data.swap(glyf);
    locaEntry->data.swap(loca);
    headEntry->data[50] = 0;  // indexToLocFormat: long offsets
    headEntry->data[51] = 1;
  }
  if (hmtxEntry && hmtxEntry->transformed) {
    if (!hheaEntry || hheaEntry->data.size() < 36 || xMins.empty()) return false;
    unsigned numHMetrics = ((uint8_t)hheaEntry->data[34] << 8) | (uint8_t)hheaEntry->data[35];
    std::string hmtx;
    if (!ReconstructHmtx((const uint8_t*)hmtxEntry->data.data(), hmtxEntry->data.size(), numGlyphs,
                         numHMetrics, xMins, hmtx))
      return false;
    hmtxEntry->data.swap(hmtx);
  }
  for (unsigned i = 0; i < numTables; ++i)
    if (tables[i].transformed && tables[i].tag != kGlyf && tables[i].tag != kLoca &&
        tables[i].tag != kHmtx)
      return false;  // unknown transform

  // Assemble the sfnt with the table directory sorted by tag.
  std::vector<Entry*> sorted;
  for (unsigned i = 0; i < numTables; ++i) sorted.push_back(&tables[i]);
  std::sort(sorted.begin(), sorted.end(), [](const Entry* a, const Entry* b) { return a->tag < b->tag; });
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
  size_t offset = 12 + numTables * 16;
  std::string body;
  for (size_t i = 0; i < sorted.size(); ++i) {
    std::string& t = sorted[i]->data;
    if (sorted[i]->tag == kHead && t.size() >= 12) t[8] = t[9] = t[10] = t[11] = 0;
    Put32(out, sorted[i]->tag);
    Put32(out, Checksum(t));
    Put32(out, (uint32_t)(offset + body.size()));
    Put32(out, (uint32_t)t.size());
    body += t;
    while (body.size() % 4) body += '\0';
  }
  out += body;
  return true;
}

}  // namespace kite
