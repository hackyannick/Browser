#include "net/url.h"

#include <vector>

#include "base/strings.h"

namespace kite {

namespace {

bool IsSpecialScheme(const std::string& s) {
  return s == "http" || s == "https" || s == "file" || s == "ftp" ||
         s == "ws" || s == "wss";
}

int DefaultPort(const std::string& s) {
  if (s == "http" || s == "ws") return 80;
  if (s == "https" || s == "wss") return 443;
  if (s == "ftp") return 21;
  return -1;
}

// Percent-encodes characters that may not appear raw in a URL component.
std::string EncodeComponent(const std::string& in, bool isQuery) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    unsigned char c = (unsigned char)in[i];
    bool enc = c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' ||
               c == '`' || (!isQuery && (c == '{' || c == '}')) ||
               (isQuery && c == '\'' && false);
    if (enc) {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    } else {
      out += char(c);
    }
  }
  return out;
}

std::string RemoveDotSegments(const std::string& path) {
  std::vector<std::string> segs = Split(path, '/');
  std::vector<std::string> out;
  // segs[0] is the empty string before the leading slash.
  for (size_t i = 1; i < segs.size(); ++i) {
    const std::string& s = segs[i];
    bool last = i + 1 == segs.size();
    if (s == "." || EqualsIgnoreCase(s, "%2e")) {
      if (last) out.push_back("");
    } else if (s == ".." || EqualsIgnoreCase(s, ".%2e") ||
               EqualsIgnoreCase(s, "%2e.") || EqualsIgnoreCase(s, "%2e%2e")) {
      if (!out.empty()) out.pop_back();
      if (last) out.push_back("");
    } else {
      out.push_back(s);
    }
  }
  std::string r;
  for (size_t i = 0; i < out.size(); ++i) r += "/" + out[i];
  if (r.empty()) r = "/";
  return r;
}

}  // namespace

bool Url::IsSpecial() const { return IsSpecialScheme(scheme_); }

int Url::EffectivePort() const {
  return port_ >= 0 ? port_ : DefaultPort(scheme_);
}

Url Url::Parse(const std::string& rawInput) {
  Url u;
  std::string input;
  std::string trimmed = Trim(rawInput);
  for (size_t i = 0; i < trimmed.size(); ++i)
    if (trimmed[i] != '\t' && trimmed[i] != '\n' && trimmed[i] != '\r')
      input += trimmed[i];

  // Scheme.
  size_t i = 0;
  if (input.empty() || !IsAsciiAlpha((unsigned char)input[0])) return u;
  while (i < input.size() &&
         (IsAsciiAlpha((unsigned char)input[i]) || IsAsciiDigit((unsigned char)input[i]) ||
          input[i] == '+' || input[i] == '-' || input[i] == '.'))
    ++i;
  if (i >= input.size() || input[i] != ':') return u;
  u.scheme_ = AsciiLower(input.substr(0, i));
  std::string rest = input.substr(i + 1);

  // Fragment applies to all schemes.
  size_t hash = rest.find('#');
  if (hash != std::string::npos) {
    u.fragment_ = rest.substr(hash + 1);
    u.hasFragment_ = true;
    rest = rest.substr(0, hash);
  }

  if (!u.IsSpecial()) {
    // Opaque path (about:, data:, mailto:, javascript: ...)
    if (u.scheme_ != "data") {
      size_t q = rest.find('?');
      if (q != std::string::npos) {
        u.query_ = rest.substr(q + 1);
        u.hasQuery_ = true;
        rest = rest.substr(0, q);
      }
    }
    u.path_ = rest;
    u.valid_ = true;
    return u;
  }

  for (size_t k = 0; k < rest.size(); ++k)
    if (rest[k] == '\\') rest[k] = '/';
  size_t p = 0;
  while (p < rest.size() && rest[p] == '/') ++p;
  if (u.scheme_ == "file") {
    // file:///C:/x  -> host "", path "/C:/x"; file://server/share -> host
    if (p == 2) {
      size_t end = rest.find_first_of("/?", p);
      if (end == std::string::npos) end = rest.size();
      u.host_ = AsciiLower(rest.substr(p, end - p));
      if (u.host_.size() == 2 && u.host_[1] == ':') {  // file://C:/x
        u.host_.clear();
        p = 2;
        rest = "/" + rest.substr(2);
        p = 0;
      } else {
        rest = rest.substr(end);
        p = 0;
      }
    } else {
      rest = "/" + rest.substr(p);
      p = 0;
    }
  } else {
    size_t end = rest.find_first_of("/?", p);
    if (end == std::string::npos) end = rest.size();
    std::string authority = rest.substr(p, end - p);
    rest = rest.substr(end);
    size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    std::string host = authority;
    if (!host.empty() && host[0] == '[') {
      size_t close = host.find(']');
      if (close == std::string::npos) return u;
      std::string after = host.substr(close + 1);
      host = host.substr(0, close + 1);
      if (!after.empty() && after[0] == ':') authority = after;
      else authority.clear();
      if (!authority.empty()) {
        long long port;
        if (authority.size() > 1) {
          if (!ParseInt(authority.substr(1), port) || port < 0 || port > 65535)
            return u;
          u.port_ = (int)port;
        }
      }
    } else {
      size_t colon = host.rfind(':');
      if (colon != std::string::npos) {
        std::string ps = host.substr(colon + 1);
        host = host.substr(0, colon);
        if (!ps.empty()) {
          long long port;
          if (!ParseInt(ps, port) || port < 0 || port > 65535) return u;
          u.port_ = (int)port;
        }
      }
    }
    u.host_ = AsciiLower(PercentDecode(host, false));
    if (u.host_.empty()) return u;
    for (size_t k = 0; k < u.host_.size(); ++k) {
      char c = u.host_[k];
      if (c == ' ' || c == '<' || c == '>' || c == '%' || c == '#') return u;
    }
    if (u.port_ == DefaultPort(u.scheme_)) u.port_ = -1;
  }

  size_t q = rest.find('?');
  std::string path = rest;
  if (q != std::string::npos) {
    u.query_ = EncodeComponent(rest.substr(q + 1), true);
    u.hasQuery_ = true;
    path = rest.substr(0, q);
  }
  if (path.empty() || path[0] != '/') path = "/" + path;
  u.path_ = RemoveDotSegments(EncodeComponent(path, false));
  if (u.hasFragment_) u.fragment_ = EncodeComponent(u.fragment_, true);
  u.valid_ = true;
  return u;
}

Url Url::Resolve(const std::string& rawRef) const {
  std::string ref = Trim(rawRef);
  std::string clean;
  for (size_t i = 0; i < ref.size(); ++i)
    if (ref[i] != '\t' && ref[i] != '\n' && ref[i] != '\r') clean += ref[i];
  ref = clean;

  // Absolute?
  size_t i = 0;
  while (i < ref.size() && (IsAsciiAlpha((unsigned char)ref[i]) ||
                            (i > 0 && (IsAsciiDigit((unsigned char)ref[i]) ||
                                       ref[i] == '+' || ref[i] == '-' || ref[i] == '.'))))
    ++i;
  if (i > 0 && i < ref.size() && ref[i] == ':') {
    std::string scheme = AsciiLower(ref.substr(0, i));
    // "http:foo" relative form for same special scheme.
    if (scheme == scheme_ && IsSpecial() && !StartsWith(ref.substr(i + 1), "/") &&
        !StartsWith(ref.substr(i + 1), "\\") && scheme != "file") {
      return Resolve(ref.substr(i + 1));
    }
    return Parse(ref);
  }
  if (!valid_) return Url();
  if (!IsSpecial()) {
    if (StartsWith(ref, "#")) {
      Url u = *this;
      u.fragment_ = ref.substr(1);
      u.hasFragment_ = true;
      return u;
    }
    return Url();
  }

  std::string norm = ref;
  for (size_t k = 0; k < norm.size(); ++k) {
    if (norm[k] == '?' || norm[k] == '#') break;
    if (norm[k] == '\\') norm[k] = '/';
  }

  std::string authority = scheme_ + "://" + host_ +
                          (port_ >= 0 ? ":" + IntToString(port_) : std::string());
  if (StartsWith(norm, "//")) return Parse(scheme_ + ":" + norm);
  if (StartsWith(norm, "/")) return Parse(authority + norm);
  if (norm.empty()) {
    Url u = *this;
    u.ClearFragment();
    return u;
  }
  if (norm[0] == '#') {
    Url u = *this;
    u.fragment_ = norm.substr(1);
    u.hasFragment_ = true;
    return u;
  }
  if (norm[0] == '?') return Parse(authority + path_ + norm);
  size_t slash = path_.rfind('/');
  std::string dir = slash == std::string::npos ? "/" : path_.substr(0, slash + 1);
  return Parse(authority + dir + norm);
}

std::string Url::Spec() const {
  std::string s = SpecNoFragment();
  if (hasFragment_) s += "#" + fragment_;
  return s;
}

std::string Url::SpecNoFragment() const {
  if (!valid_) return std::string();
  std::string s = scheme_ + ":";
  if (IsSpecial()) {
    s += "//" + host_;
    if (port_ >= 0) s += ":" + IntToString(port_);
  }
  s += path_;
  if (hasQuery_) s += "?" + query_;
  return s;
}

std::string Url::PathAndQuery() const {
  std::string s = path_.empty() ? "/" : path_;
  if (hasQuery_) s += "?" + query_;
  return s;
}

std::string Url::HostPort() const {
  std::string s = host_;
  if (port_ >= 0) s += ":" + IntToString(port_);
  return s;
}

std::string Url::Origin() const {
  if (!IsSpecial() || scheme_ == "file") return "null";
  return scheme_ + "://" + HostPort();
}

std::string Url::ToLocalPath() const {
  std::string p = PercentDecode(path_, false);
#ifdef _WIN32
  if (p.size() >= 3 && p[0] == '/' && IsAsciiAlpha((unsigned char)p[1]) &&
      (p[2] == ':' || p[2] == '|')) {
    p = p.substr(1);
    p[1] = ':';
  } else if (!host_.empty()) {
    p = "//" + host_ + p;
  }
  for (size_t i = 0; i < p.size(); ++i)
    if (p[i] == '/') p[i] = '\\';
#endif
  return p;
}

Url Url::FromLocalPath(const std::string& path) {
  std::string p = path;
  for (size_t i = 0; i < p.size(); ++i)
    if (p[i] == '\\') p[i] = '/';
  if (StartsWith(p, "//")) return Parse("file:" + p);
  if (!StartsWith(p, "/")) p = "/" + p;
  return Parse("file://" + p);
}

std::string FixupUserInput(const std::string& rawText, const std::string& searchPrefix) {
  std::string text = Trim(rawText);
  if (text.empty()) return "about:blank";
  bool hasSpace = text.find(' ') != std::string::npos;
  // Windows path?
  if (text.size() >= 3 && IsAsciiAlpha((unsigned char)text[0]) && text[1] == ':' &&
      (text[2] == '\\' || text[2] == '/'))
    return Url::FromLocalPath(text).Spec();
  size_t colon = text.find(':');
  if (!hasSpace && colon != std::string::npos) {
    std::string scheme = AsciiLower(text.substr(0, colon));
    if (scheme == "http" || scheme == "https" || scheme == "file" ||
        scheme == "about" || scheme == "data" || scheme == "view-source") {
      return text;
    }
  }
  if (!hasSpace) {
    std::string hostPart = text.substr(0, text.find_first_of("/?#"));
    bool looksLikeHost = hostPart.find('.') != std::string::npos ||
                         StartsWithIgnoreCase(hostPart, "localhost");
    if (looksLikeHost && hostPart.size() > 0 && hostPart[0] != '.' &&
        hostPart[hostPart.size() - 1] != '.') {
      return "http://" + text;
    }
  }
  return searchPrefix + PercentEncodeForm(text);
}

}  // namespace kite
