// Kite Engine - URL parsing and resolution (subset of the WHATWG URL spec)
#ifndef KITE_NET_URL_H
#define KITE_NET_URL_H

#include <string>

namespace kite {

class Url {
 public:
  Url() : port_(-1), valid_(false) {}

  // Parses an absolute URL. Returns an invalid Url on failure.
  static Url Parse(const std::string& input);
  // Resolves |ref| against this URL (which acts as the base).
  Url Resolve(const std::string& ref) const;

  bool valid() const { return valid_; }
  bool IsSpecial() const;  // http, https, file, ftp, ws, wss
  const std::string& scheme() const { return scheme_; }
  const std::string& host() const { return host_; }
  int port() const { return port_; }
  int EffectivePort() const;  // port or the scheme default
  const std::string& path() const { return path_; }
  const std::string& query() const { return query_; }
  const std::string& fragment() const { return fragment_; }
  bool HasQuery() const { return hasQuery_; }
  bool HasFragment() const { return hasFragment_; }

  std::string Spec() const;           // full serialization
  std::string SpecNoFragment() const;
  std::string PathAndQuery() const;   // request-target for HTTP
  std::string Origin() const;         // scheme://host[:port]
  std::string HostPort() const;       // host[:port] for the Host header

  void SetQuery(const std::string& q) { query_ = q; hasQuery_ = true; }
  void ClearFragment() { fragment_.clear(); hasFragment_ = false; }

  // For file: URLs, returns a native path (C:\foo\bar.html on Windows).
  std::string ToLocalPath() const;
  static Url FromLocalPath(const std::string& path);

 private:
  std::string scheme_;
  std::string host_;
  int port_;
  std::string path_;
  std::string query_;
  std::string fragment_;
  bool hasQuery_ = false;
  bool hasFragment_ = false;
  bool valid_;
};

// Heuristic used by the address bar: turns user input into a URL (adds
// http://, or builds a search URL if the text does not look like an address).
std::string FixupUserInput(const std::string& text, const std::string& searchUrlPrefix);

}  // namespace kite

#endif
