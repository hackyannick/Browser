// Kite Engine - resource loading (http, https, file, data, about)
#ifndef KITE_NET_HTTP_H
#define KITE_NET_HTTP_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/mutex.h"
#include "net/socket.h"
#include "net/url.h"

namespace kite {

struct FetchRequest {
  std::string url;
  std::string method;
  std::string body;
  std::string contentType;
  std::string referrer;
  std::string accept;
  std::shared_ptr<CancelToken> cancel;
  size_t maxBytes;
  // Optional progress callback (called from the worker thread).
  void (*progress)(void* ctx, size_t received, size_t total);
  void* progressCtx;
  FetchRequest() : method("GET"), maxBytes(64 * 1024 * 1024), progress(0), progressCtx(0) {}
};

struct FetchResponse {
  bool ok;               // a response was received (any status)
  int status;
  std::string error;     // transport error description
  std::string finalUrl;  // after redirects
  std::vector<std::pair<std::string, std::string> > headers;
  std::string body;      // decoded (gzip/deflate removed)
  bool secure;           // delivered over TLS

  FetchResponse() : ok(false), status(0), secure(false) {}
  std::string Header(const std::string& name) const;
  std::string MimeType() const;  // lower-case, sniffed if missing
  std::string Charset() const;
  std::string FileName() const;  // from Content-Disposition or URL
};

struct ProxyConfig {
  std::string host;
  int port;
  ProxyConfig() : port(0) {}
  bool enabled() const { return !host.empty() && port > 0; }
};

class CookieJar {
 public:
  void SetFromHeader(const Url& url, const std::string& header);
  std::string CookieHeader(const Url& url);
  void Clear();
  std::string Serialize();
  void Deserialize(const std::string& data);
  size_t Count();

 private:
  struct Cookie {
    std::string name, value, domain, path;
    bool hostOnly, secure;
    long long expires;  // unix time, 0 = session
  };
  Mutex mu_;
  std::vector<Cookie> cookies_;
};

class Network {
 public:
  static Network& Get();
  FetchResponse Fetch(const FetchRequest& req);

  void SetProxy(const ProxyConfig& p) { MutexLock l(mu_); proxy_ = p; }
  ProxyConfig proxy() { MutexLock l(mu_); return proxy_; }
  void SetUserAgent(const std::string& ua) { MutexLock l(mu_); userAgent_ = ua; }
  std::string userAgent() { MutexLock l(mu_); return userAgent_; }
  CookieJar& cookies() { return cookies_; }

 private:
  Network();
  FetchResponse FetchHttp(const Url& url, const FetchRequest& req, const std::string& method,
                          const std::string& body);
  FetchResponse FetchFile(const Url& url);
  FetchResponse FetchData(const Url& url);

  Mutex mu_;
  ProxyConfig proxy_;
  std::string userAgent_;
  CookieJar cookies_;
};

// Decompression helpers (gzip / zlib / raw deflate).
bool Inflate(const std::string& in, const std::string& encoding, std::string& out);
std::string SniffMimeType(const std::string& body, const std::string& url);

}  // namespace kite

#endif
