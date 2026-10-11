// Kite Engine - content blocker for advertising and tracking hosts.
//
// Requests from a page to a third-party host on the block list fail before
// a connection is made. The built-in list covers the large ad networks and
// trackers; users can add hosts (and exceptions) in a text file using plain
// host names or the common "||host^" / "@@||host^" filter syntax.
#ifndef KITE_NET_BLOCKER_H
#define KITE_NET_BLOCKER_H

#include <set>
#include <string>

#include "base/mutex.h"

namespace kite {

class ContentBlocker {
 public:
  static ContentBlocker& Get();

  void SetEnabled(bool on);
  bool enabled();
  // Adds rules from a filter list (one rule per line, '!' and '#' comments).
  void AddRules(const std::string& text);
  // True if a request for |url| made by the page at |pageUrl| is blocked.
  // Same-site requests are never blocked.
  bool ShouldBlock(const std::string& url, const std::string& pageUrl);
  long long blockedTotal();

  // "www.news.example.co.uk" -> "example.co.uk" (approximation of the
  // registrable domain without the public suffix list).
  static std::string SiteOf(const std::string& host);

 private:
  ContentBlocker();
  bool Listed(const std::set<std::string>& list, const std::string& host) const;
  Mutex mu_;
  bool enabled_ = true;
  std::set<std::string> blocked_, allowed_;
  long long count_ = 0;
};

}  // namespace kite

#endif
