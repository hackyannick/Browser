#include "script/common.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "base/mutex.h"
#include "base/strings.h"

extern "C" {
#include "bearssl.h"
#include "quickjs.h"
}

namespace kite {

extern const char* const kCommonPrelude;

namespace {

std::string Str(JSContext* ctx, JSValueConst v) {
  size_t len;
  const char* s = JS_ToCStringLen(ctx, &len, v);
  if (!s) return std::string();
  std::string out(s, len);
  JS_FreeCString(ctx, s);
  return out;
}

JSValue NewU8(JSContext* ctx, const std::string& bytes) {
  JSValue args[3] = {JS_NewArrayBufferCopy(ctx, (const uint8_t*)bytes.data(), bytes.size()), JS_UNDEFINED,
                     JS_UNDEFINED};  // the constructor reads all three
  JSValue ab = args[0];
  JSValue arr = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
  JS_FreeValue(ctx, ab);
  return arr;
}

bool Bytes(JSContext* ctx, JSValueConst v, std::string& out) {
  if (JS_IsString(v)) {
    out = Str(ctx, v);
    return true;
  }
  size_t size = 0;
  uint8_t* p = JS_GetArrayBuffer(ctx, &size, v);
  if (p) {
    out.assign((const char*)p, size);
    return true;
  }
  JS_FreeValue(ctx, JS_GetException(ctx));  // not an ArrayBuffer
  size_t off = 0, len = 0, per = 0;
  JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &per);
  if (JS_IsException(buf)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
  }
  p = JS_GetArrayBuffer(ctx, &size, buf);
  bool ok = p && off + len <= size;
  if (ok) out.assign((const char*)p + off, len);
  JS_FreeValue(ctx, buf);
  return ok;
}

// utf8Encode(string) -> Uint8Array
JSValue Utf8Encode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  return NewU8(ctx, argc > 0 ? Str(ctx, argv[0]) : std::string());
}

// utf8Decode(bytes, fatal) -> string (invalid sequences become U+FFFD)
JSValue Utf8Decode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string b;
  if (argc < 1 || !Bytes(ctx, argv[0], b)) return JS_NewString(ctx, "");
  std::string out;
  out.reserve(b.size());
  size_t i = 0;
  while (i < b.size()) {
    unsigned char c = b[i];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    bool ok = n > 0 && i + n <= b.size();
    for (int k = 1; ok && k < n; ++k) ok = ((unsigned char)b[i + k] >> 6) == 2;
    if (ok && n > 1) {
      unsigned cp = n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
      for (int k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)b[i + k] & 0x3F);
      ok = (n == 2 && cp >= 0x80) || (n == 3 && cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) ||
           (n == 4 && cp >= 0x10000 && cp <= 0x10FFFF);
    }
    if (ok) {
      out.append(b, i, n);
      i += n;
    } else {
      if (argc > 1 && JS_ToBool(ctx, argv[1])) return JS_ThrowTypeError(ctx, "The encoded data was not valid.");
      out += "\xEF\xBF\xBD";
      ++i;
    }
  }
  return JS_NewStringLen(ctx, out.data(), out.size());
}

// decode(bytes, charset) -> string (legacy encodings)
JSValue DecodeCharset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string b;
  if (argc < 2 || !Bytes(ctx, argv[0], b)) return JS_NewString(ctx, "");
  std::string s = ConvertToUtf8(b, Str(ctx, argv[1]));
  return JS_NewStringLen(ctx, s.data(), s.size());
}

// latin1(bytes) -> string with one char per byte; bytes(latin1) -> Uint8Array
JSValue Latin1(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string b;
  if (argc < 1 || !Bytes(ctx, argv[0], b)) return JS_NewString(ctx, "");
  std::string out;
  for (size_t i = 0; i < b.size(); ++i) Utf8Append(out, (unsigned char)b[i]);
  return JS_NewStringLen(ctx, out.data(), out.size());
}

JSValue FromLatin1(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string s = argc > 0 ? Str(ctx, argv[0]) : std::string();
  std::u16string u = Utf8ToUtf16(s);
  std::string out(u.size(), '\0');
  for (size_t i = 0; i < u.size(); ++i) out[i] = (char)(u[i] & 0xFF);
  return NewU8(ctx, out);
}

// Cryptographic random bytes (HMAC-DRBG seeded from the system).
Mutex g_rngMu;
br_hmac_drbg_context g_rng;
bool g_rngReady = false;

JSValue RandomBytes(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  int32_t n = 0;
  if (argc > 0) JS_ToInt32(ctx, &n, argv[0]);
  if (n < 0 || n > 65536) return JS_ThrowRangeError(ctx, "too many random bytes requested");
  std::string out((size_t)n, '\0');
  {
    MutexLock l(g_rngMu);
    if (!g_rngReady) {
      br_hmac_drbg_init(&g_rng, &br_sha256_vtable, "kite", 4);
      br_prng_seeder seeder = br_prng_seeder_system(0);
      if (!seeder || !seeder(&g_rng.vtable)) {
        // Fallback: mix in addresses and the clock.
        unsigned long long t = (unsigned long long)(size_t)&out ^ (unsigned long long)time(0) ^
                               ((unsigned long long)clock() << 20);
        br_hmac_drbg_update(&g_rng, &t, sizeof t);
      }
      g_rngReady = true;
    }
    if (n) br_hmac_drbg_generate(&g_rng, &out[0], (size_t)n);
  }
  return NewU8(ctx, out);
}

// digest(algorithm, bytes) -> Uint8Array (SHA-1/256/384/512)
JSValue Digest(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string alg = argc > 0 ? AsciiLower(Str(ctx, argv[0])) : std::string(), data;
  if (argc < 2 || !Bytes(ctx, argv[1], data)) return JS_ThrowTypeError(ctx, "digest: bad data");
  const br_hash_class* h = alg == "sha-1" ? &br_sha1_vtable
                           : alg == "sha-256" ? &br_sha256_vtable
                           : alg == "sha-384" ? &br_sha384_vtable
                           : alg == "sha-512" ? &br_sha512_vtable
                                              : 0;
  if (!h) return JS_NULL;
  br_hash_compat_context hc;
  h->init(&hc.vtable);
  h->update(&hc.vtable, data.data(), data.size());
  unsigned char out[64];
  h->out(&hc.vtable, out);
  return NewU8(ctx, std::string((const char*)out, (h->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK));
}

// hmac(algorithm, key, bytes) -> Uint8Array
JSValue Hmac(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::string alg = argc > 0 ? AsciiLower(Str(ctx, argv[0])) : std::string(), key, data;
  if (argc < 3 || !Bytes(ctx, argv[1], key) || !Bytes(ctx, argv[2], data)) return JS_ThrowTypeError(ctx, "hmac: bad data");
  const br_hash_class* h = alg == "sha-1" ? &br_sha1_vtable
                           : alg == "sha-256" ? &br_sha256_vtable
                           : alg == "sha-384" ? &br_sha384_vtable
                           : alg == "sha-512" ? &br_sha512_vtable
                                              : 0;
  if (!h) return JS_NULL;
  br_hmac_key_context kc;
  br_hmac_key_init(&kc, h, key.data(), key.size());
  br_hmac_context mc;
  br_hmac_init(&mc, &kc, 0);
  br_hmac_update(&mc, data.data(), data.size());
  unsigned char out[64];
  size_t n = br_hmac_out(&mc, out);
  return NewU8(ctx, std::string((const char*)out, n));
}

}  // namespace

bool JsBytes(JSContext* ctx, const void* v, std::string& out) { return Bytes(ctx, *(const JSValue*)v, out); }

void JsNewUint8Array(JSContext* ctx, const std::string& bytes, void* out) { *(JSValue*)out = NewU8(ctx, bytes); }

void InstallCommonApis(JSContext* ctx) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue k = JS_NewObject(ctx);
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int argc;
  };
  static const Fn fns[] = {
      {"utf8Encode", Utf8Encode, 1}, {"utf8Decode", Utf8Decode, 2}, {"decode", DecodeCharset, 2},
      {"latin1", Latin1, 1},         {"fromLatin1", FromLatin1, 1}, {"randomBytes", RandomBytes, 1},
      {"digest", Digest, 2},         {"hmac", Hmac, 3},
  };
  for (size_t i = 0; i < sizeof fns / sizeof fns[0]; ++i)
    JS_SetPropertyStr(ctx, k, fns[i].name, JS_NewCFunction(ctx, fns[i].fn, fns[i].name, fns[i].argc));
  JS_SetPropertyStr(ctx, global, "__kc", k);
  JS_FreeValue(ctx, global);
  JSValue r = JS_Eval(ctx, kCommonPrelude, strlen(kCommonPrelude), "kite:common.js", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) {
    JSValue ex = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, ex);
    fprintf(stderr, "kite:common.js: %s\n", s ? s : "?");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, ex);
  }
  JS_FreeValue(ctx, r);
}

}  // namespace kite
