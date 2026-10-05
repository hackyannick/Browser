// Kite Engine - JavaScript APIs shared by pages and workers (URL, text
// encoding, Blob/Response, streams, Intl, crypto ...). The natives live on
// the global "__kc"; the API itself is JavaScript (common_js.cpp).
#ifndef KITE_SCRIPT_COMMON_H
#define KITE_SCRIPT_COMMON_H

#include <string>

struct JSContext;

namespace kite {

// Registers the natives and runs the shared prelude in |ctx|.
void InstallCommonApis(JSContext* ctx);

// Bytes of a string (UTF-8), ArrayBuffer or typed array. False otherwise.
bool JsBytes(JSContext* ctx, const void* jsValue, std::string& out);
// A new Uint8Array (returned as an owned JSValue in |out|).
void JsNewUint8Array(JSContext* ctx, const std::string& bytes, void* out);

}  // namespace kite

#endif
