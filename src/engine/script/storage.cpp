#include "script/storage.h"

namespace kite {

namespace {

std::string Escape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\\') out += "\\\\";
    else if (c == '\t') out += "\\t";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

std::string Unescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char c = s[++i];
      out += c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    } else {
      out += s[i];
    }
  }
  return out;
}

}  // namespace

const std::string* WebStorage::Get(const std::string& origin, const std::string& key) const {
  std::map<std::string, Items>::const_iterator it = data_.find(origin);
  if (it == data_.end()) return 0;
  for (size_t i = 0; i < it->second.size(); ++i)
    if (it->second[i].first == key) return &it->second[i].second;
  return 0;
}

bool WebStorage::Set(const std::string& origin, const std::string& key, const std::string& value) {
  Items& items = data_[origin];
  size_t total = 0;
  Items::iterator found = items.end();
  for (Items::iterator i = items.begin(); i != items.end(); ++i) {
    if (i->first == key) found = i;
    else total += i->first.size() + i->second.size();
  }
  if (total + key.size() + value.size() > kQuota) return false;
  if (found != items.end()) {
    if (found->second == value) return true;
    found->second = value;
  } else {
    items.push_back(std::make_pair(key, value));
  }
  dirty_ = true;
  return true;
}

void WebStorage::Remove(const std::string& origin, const std::string& key) {
  std::map<std::string, Items>::iterator it = data_.find(origin);
  if (it == data_.end()) return;
  for (Items::iterator i = it->second.begin(); i != it->second.end(); ++i)
    if (i->first == key) {
      it->second.erase(i);
      dirty_ = true;
      break;
    }
  if (it->second.empty()) data_.erase(it);
}

void WebStorage::Clear(const std::string& origin) {
  if (data_.erase(origin)) dirty_ = true;
}

const WebStorage::Items* WebStorage::ItemsOf(const std::string& origin) const {
  std::map<std::string, Items>::const_iterator it = data_.find(origin);
  return it == data_.end() ? 0 : &it->second;
}

std::string WebStorage::Serialize() const {
  std::string out;
  for (std::map<std::string, Items>::const_iterator it = data_.begin(); it != data_.end(); ++it)
    for (size_t i = 0; i < it->second.size(); ++i)
      out += Escape(it->first) + "\t" + Escape(it->second[i].first) + "\t" + Escape(it->second[i].second) + "\n";
  return out;
}

void WebStorage::Deserialize(const std::string& text) {
  data_.clear();
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    size_t t1 = line.find('\t');
    size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
    if (t2 == std::string::npos) continue;
    data_[Unescape(line.substr(0, t1))].push_back(
        std::make_pair(Unescape(line.substr(t1 + 1, t2 - t1 - 1)), Unescape(line.substr(t2 + 1))));
  }
  dirty_ = false;
}

}  // namespace kite
