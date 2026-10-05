#include "net/hpack.h"

#include <cstring>

namespace kite {

namespace {

#include "net/hpack_huffman.inc"

const char* const kStaticTable[61][2] = {
    {":authority", ""}, {":method", "GET"}, {":method", "POST"}, {":path", "/"},
    {":path", "/index.html"}, {":scheme", "http"}, {":scheme", "https"}, {":status", "200"},
    {":status", "204"}, {":status", "206"}, {":status", "304"}, {":status", "400"},
    {":status", "404"}, {":status", "500"}, {"accept-charset", ""}, {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""}, {"accept-ranges", ""}, {"accept", ""}, {"access-control-allow-origin", ""},
    {"age", ""}, {"allow", ""}, {"authorization", ""}, {"cache-control", ""},
    {"content-disposition", ""}, {"content-encoding", ""}, {"content-language", ""}, {"content-length", ""},
    {"content-location", ""}, {"content-range", ""}, {"content-type", ""}, {"cookie", ""},
    {"date", ""}, {"etag", ""}, {"expect", ""}, {"expires", ""},
    {"from", ""}, {"host", ""}, {"if-match", ""}, {"if-modified-since", ""},
    {"if-none-match", ""}, {"if-range", ""}, {"if-unmodified-since", ""}, {"last-modified", ""},
    {"link", ""}, {"location", ""}, {"max-forwards", ""}, {"proxy-authenticate", ""},
    {"proxy-authorization", ""}, {"range", ""}, {"referer", ""}, {"refresh", ""},
    {"retry-after", ""}, {"server", ""}, {"set-cookie", ""}, {"strict-transport-security", ""},
    {"transfer-encoding", ""}, {"user-agent", ""}, {"vary", ""}, {"via", ""},
    {"www-authenticate", ""}};

// Huffman decoding tree, built once: node children (index; < 0 = symbol).
struct HuffTree {
  std::vector<int> left, right;
  HuffTree() {
    left.push_back(0);
    right.push_back(0);
    for (int sym = 0; sym < 257; ++sym) {
      int node = 0;
      uint32_t code = kHuffmanTable[sym].code;
      int bits = kHuffmanTable[sym].bits;
      for (int b = 0; b < bits; ++b) {
        bool one = (code >> (31 - b)) & 1;
        std::vector<int>& branch = one ? right : left;
        if (b == bits - 1) {
          branch[node] = -1 - sym;
        } else {
          if (branch[node] <= 0) {
            branch[node] = (int)left.size();
            left.push_back(0);
            right.push_back(0);
          }
          node = branch[node];
        }
      }
    }
  }
};

const HuffTree& Tree() {
  static HuffTree* t = new HuffTree;
  return *t;
}

void EncodeInteger(std::string& out, uint8_t firstByteBits, int prefixBits, uint32_t value) {
  uint32_t max = (1u << prefixBits) - 1;
  if (value < max) {
    out += (char)(firstByteBits | value);
    return;
  }
  out += (char)(firstByteBits | max);
  value -= max;
  while (value >= 128) {
    out += (char)((value & 127) | 128);
    value >>= 7;
  }
  out += (char)value;
}

void EncodeString(std::string& out, const std::string& s) {
  EncodeInteger(out, 0, 7, (uint32_t)s.size());  // no Huffman
  out += s;
}

bool DecodeString(const uint8_t*& p, const uint8_t* end, std::string& out) {
  if (p >= end) return false;
  bool huffman = (*p & 0x80) != 0;
  uint32_t len;
  if (!HpackDecodeInteger(p, end, 7, len)) return false;
  if (len > (size_t)(end - p) || len > 1u << 20) return false;
  if (huffman) {
    if (!HuffmanDecode(p, len, out)) return false;
  } else {
    out.assign((const char*)p, len);
  }
  p += len;
  return true;
}

}  // namespace

bool HpackDecodeInteger(const uint8_t*& p, const uint8_t* end, int prefixBits, uint32_t& out) {
  if (p >= end) return false;
  uint32_t max = (1u << prefixBits) - 1;
  uint32_t v = *p++ & max;
  if (v < max) {
    out = v;
    return true;
  }
  int shift = 0;
  for (;;) {
    if (p >= end || shift > 28) return false;
    uint8_t b = *p++;
    uint64_t add = (uint64_t)(b & 127) << shift;
    if (v + add > 0xFFFFFFFFull) return false;
    v += (uint32_t)add;
    if (!(b & 128)) break;
    shift += 7;
  }
  out = v;
  return true;
}

bool HuffmanDecode(const uint8_t* p, size_t n, std::string& out) {
  const HuffTree& t = Tree();
  out.clear();
  int node = 0, depth = 0;
  bool allOnes = true;
  for (size_t i = 0; i < n; ++i) {
    for (int b = 7; b >= 0; --b) {
      bool one = (p[i] >> b) & 1;
      int next = one ? t.right[node] : t.left[node];
      allOnes = allOnes && one;
      ++depth;
      if (next < 0) {
        int sym = -1 - next;
        if (sym == 256) return false;  // EOS must not appear
        out += (char)sym;
        node = 0;
        depth = 0;
        allOnes = true;
      } else if (next == 0) {
        return false;
      } else {
        node = next;
      }
    }
  }
  // Padding: at most 7 bits, all ones (a prefix of EOS).
  return depth <= 7 && allOnes;
}

bool HpackDecoder::Lookup(uint32_t index, std::pair<std::string, std::string>& out) const {
  if (index == 0) return false;
  if (index <= 61) {
    out.first = kStaticTable[index - 1][0];
    out.second = kStaticTable[index - 1][1];
    return true;
  }
  size_t d = index - 62;
  if (d >= table_.size()) return false;
  out = table_[d];
  return true;
}

void HpackDecoder::Evict() {
  while (size_ > maxSize_ && !table_.empty()) {
    size_ -= table_.back().first.size() + table_.back().second.size() + 32;
    table_.pop_back();
  }
}

void HpackDecoder::Add(const std::string& name, const std::string& value) {
  size_t entry = name.size() + value.size() + 32;
  if (entry > maxSize_) {
    table_.clear();
    size_ = 0;
    return;
  }
  table_.push_front(std::make_pair(name, value));
  size_ += entry;
  Evict();
}

bool HpackDecoder::Decode(const uint8_t* p, size_t n, HeaderList& out) {
  const uint8_t* end = p + n;
  bool sawHeader = false;
  while (p < end) {
    uint8_t b = *p;
    if (b & 0x80) {  // indexed header field
      uint32_t index;
      if (!HpackDecodeInteger(p, end, 7, index)) return false;
      std::pair<std::string, std::string> h;
      if (!Lookup(index, h)) return false;
      out.push_back(h);
      sawHeader = true;
    } else if ((b & 0xE0) == 0x20) {  // dynamic table size update
      if (sawHeader) return false;  // must come first
      uint32_t size;
      if (!HpackDecodeInteger(p, end, 5, size)) return false;
      if (size > limit_) return false;
      maxSize_ = size;
      Evict();
    } else {
      // Literal: with incremental indexing (01), without (0000) or never (0001).
      bool index = (b & 0xC0) == 0x40;
      int prefix = index ? 6 : 4;
      uint32_t nameIndex;
      if (!HpackDecodeInteger(p, end, prefix, nameIndex)) return false;
      std::pair<std::string, std::string> h;
      if (nameIndex) {
        if (!Lookup(nameIndex, h)) return false;
      } else if (!DecodeString(p, end, h.first)) {
        return false;
      }
      if (!DecodeString(p, end, h.second)) return false;
      if (index) Add(h.first, h.second);
      out.push_back(h);
      sawHeader = true;
    }
    if (out.size() > 1000) return false;
  }
  return true;
}

std::string HpackEncode(const HeaderList& headers) {
  std::string out;
  for (size_t i = 0; i < headers.size(); ++i) {
    const std::string& name = headers[i].first;
    const std::string& value = headers[i].second;
    int nameIndex = 0;
    for (int k = 0; k < 61; ++k)
      if (name == kStaticTable[k][0]) {
        nameIndex = k + 1;
        break;
      }
    // Literal header field without indexing (0000xxxx).
    EncodeInteger(out, 0x00, 4, (uint32_t)nameIndex);
    if (!nameIndex) EncodeString(out, name);
    EncodeString(out, value);
  }
  return out;
}

}  // namespace kite
