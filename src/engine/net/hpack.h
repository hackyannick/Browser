// Kite Engine - HPACK header compression for HTTP/2 (RFC 7541)
#ifndef KITE_NET_HPACK_H
#define KITE_NET_HPACK_H

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace kite {

typedef std::vector<std::pair<std::string, std::string> > HeaderList;

class HpackDecoder {
 public:
  HpackDecoder() : size_(0), maxSize_(4096), limit_(4096) {}
  // Decodes one complete header block. Returns false on a compression
  // error (the connection must then be closed).
  bool Decode(const uint8_t* p, size_t n, HeaderList& out);

 private:
  bool Lookup(uint32_t index, std::pair<std::string, std::string>& out) const;
  void Add(const std::string& name, const std::string& value);
  void Evict();
  std::deque<std::pair<std::string, std::string> > table_;  // newest first
  size_t size_, maxSize_, limit_;
};

// Encodes headers as literals without indexing (names from the static
// table where possible); stateless and always valid.
std::string HpackEncode(const HeaderList& headers);

// Building blocks (exposed for tests).
bool HpackDecodeInteger(const uint8_t*& p, const uint8_t* end, int prefixBits, uint32_t& out);
bool HuffmanDecode(const uint8_t* p, size_t n, std::string& out);

}  // namespace kite

#endif
