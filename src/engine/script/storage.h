// Kite Engine - Web Storage (localStorage/sessionStorage) data, keyed by
// origin. The platform layer keeps one store for localStorage (saved to
// disk) and one per tab for sessionStorage.
#ifndef KITE_SCRIPT_STORAGE_H
#define KITE_SCRIPT_STORAGE_H

#include <map>
#include <string>
#include <vector>

namespace kite {

class WebStorage {
 public:
  typedef std::vector<std::pair<std::string, std::string> > Items;  // insertion order

  // Per-origin quota (UTF-8 bytes of keys and values), like browsers' 5 MB.
  static const size_t kQuota = 5 * 1024 * 1024;

  const std::string* Get(const std::string& origin, const std::string& key) const;
  // False if the origin's quota would be exceeded.
  bool Set(const std::string& origin, const std::string& key, const std::string& value);
  void Remove(const std::string& origin, const std::string& key);
  void Clear(const std::string& origin);
  void ClearAll() {
    dirty_ = dirty_ || !data_.empty();
    data_.clear();
  }
  const Items* ItemsOf(const std::string& origin) const;

  // Line-based text format: origin TAB key TAB value with \\, \t, \n escaped.
  std::string Serialize() const;
  void Deserialize(const std::string& text);
  bool dirty() const { return dirty_; }
  void ClearDirty() { dirty_ = false; }

 private:
  std::map<std::string, Items> data_;
  bool dirty_ = false;
};

}  // namespace kite

#endif
