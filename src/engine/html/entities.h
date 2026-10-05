// Kite Engine - HTML character references
#ifndef KITE_HTML_ENTITIES_H
#define KITE_HTML_ENTITIES_H

#include <string>

#include "base/strings.h"

namespace kite {

bool LookupEntity(const std::string& name, Codepoint& out);
// Replaces &name; / &#123; / &#x1F; references with their UTF-8 text.
std::string DecodeEntities(const std::string& in, bool inAttribute);

}  // namespace kite

#endif
