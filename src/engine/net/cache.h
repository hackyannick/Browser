// Kite Engine - HTTP disk cache.
//
// Successful GET responses are stored in a directory (one file per URL) and
// served again while they are fresh (Cache-Control max-age, Expires, or a
// heuristic based on Last-Modified). Stale entries with an ETag or
// Last-Modified date are revalidated with a conditional request, so an
// unchanged resource costs only a 304 answer. The cache never stores
// responses marked no-store, responses that set cookies, partial content
// or requests with credentials.
#ifndef KITE_NET_CACHE_H
#define KITE_NET_CACHE_H

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/mutex.h"

namespace kite {

struct FetchResponse;

class HttpCache {
 public:
  // |dir| is created if needed (UTF-8 path). |maxBytes| 0 disables.
  HttpCache(const std::string& dir, long long maxBytes);

  struct Entry {
    std::string url;
    int status = 200;
    long long storedAt = 0;   // Unix seconds
    long long freshUntil = 0;
    bool mustRevalidate = false;
    std::string etag, lastModified;
    bool secure = false;
    std::string tlsVersion;
    std::vector<std::pair<std::string, std::string> > headers;
    std::string body;
  };

  // Entry for |url| (any freshness).
  bool Lookup(const std::string& url, Entry& out);
  bool Fresh(const Entry& e) const;
  // Stores a final 200 response for |url| if it is cacheable.
  void Store(const std::string& url, const FetchResponse& resp);
  // A 304 answer: new freshness from its headers.
  void Refresh(const std::string& url, const FetchResponse& notModified, Entry& entry);
  static void ToResponse(const Entry& e, FetchResponse& out);

  void Clear();
  void SetLimit(long long maxBytes);
  long long size();

 private:
  std::string PathFor(const std::string& url) const;
  bool Write(const Entry& e);
  void Evict();  // mu_ held
  static void Freshness(const std::vector<std::pair<std::string, std::string> >& headers, long long now,
                        Entry& e, bool& storable);

  Mutex mu_;
  std::string dir_;
  long long maxBytes_;
  long long total_ = 0;
  // file name -> (size, last use)
  std::map<std::string, std::pair<long long, long long> > files_;
  long long clock_ = 0;
};

}  // namespace kite

#endif
