#include "net/tls13.h"

#include <cstring>
#include <ctime>
#include <memory>

#include "base/mutex.h"
#include "base/strings.h"
#include "net/socket.h"

extern "C" {
#include "bearssl.h"
}

namespace kite {

std::string X509ErrorText(int err);  // tls.cpp

namespace {

const int kChaCha = 0x1303, kAes128 = 0x1301, kAes256 = 0x1302;

std::string U8(int v) { return std::string(1, (char)(v & 0xFF)); }
std::string U16(int v) { return U8(v >> 8) + U8(v); }
std::string U24(int v) { return U8(v >> 16) + U16(v); }

struct Parser {
  const std::string& s;
  size_t p;
  bool ok;
  explicit Parser(const std::string& str, size_t start = 0) : s(str), p(start), ok(true) {}
  bool Need(size_t n) {
    if (!ok || p + n > s.size()) ok = false;
    return ok;
  }
  int Byte() { return Need(1) ? (unsigned char)s[p++] : 0; }
  int Short() { int a = Byte(); return (a << 8) | Byte(); }
  int Tri() { int a = Byte(); return (a << 16) | Short(); }
  std::string Bytes(size_t n) {
    if (!Need(n)) return std::string();
    std::string r = s.substr(p, n);
    p += n;
    return r;
  }
};

// Random numbers: HMAC-DRBG seeded from the operating system.
Mutex g_rngMu;
br_hmac_drbg_context g_rng;
bool g_rngReady = false;

br_hmac_drbg_context* Rng() {
  if (!g_rngReady) {
    br_hmac_drbg_init(&g_rng, &br_sha256_vtable, "kite-tls13", 10);
    br_prng_seeder seeder = br_prng_seeder_system(0);
    if (!seeder || !seeder(&g_rng.vtable)) {
      unsigned long long t = (unsigned long long)time(0) ^ ((unsigned long long)clock() << 32) ^ (size_t)&g_rng;
      br_hmac_drbg_update(&g_rng, &t, sizeof t);
    }
    g_rngReady = true;
  }
  return &g_rng;
}

std::string RandomBytes(size_t n) {
  MutexLock l(g_rngMu);
  std::string out(n, '\0');
  br_hmac_drbg_generate(Rng(), &out[0], n);
  return out;
}

const br_hash_class* HashClass(bool sha384) { return sha384 ? &br_sha384_vtable : &br_sha256_vtable; }

std::string Digest(const br_hash_class* h, const std::string& data) {
  br_hash_compat_context c;
  h->init(&c.vtable);
  h->update(&c.vtable, data.data(), data.size());
  unsigned char out[64];
  h->out(&c.vtable, out);
  return std::string((const char*)out, (h->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK);
}

std::string Hmac(bool sha384, const std::string& key, const std::string& data) {
  br_hmac_key_context kc;
  br_hmac_key_init(&kc, HashClass(sha384), key.data(), key.size());
  br_hmac_context mc;
  br_hmac_init(&mc, &kc, 0);
  br_hmac_update(&mc, data.data(), data.size());
  unsigned char out[64];
  size_t n = br_hmac_out(&mc, out);
  return std::string((const char*)out, n);
}

bool ConstEq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < a.size(); ++i) d |= (unsigned char)(a[i] ^ b[i]);
  return d == 0;
}

bool IsIpAddress(const std::string& h) {
  if (h.find(':') != std::string::npos) return true;
  for (size_t i = 0; i < h.size(); ++i)
    if (!IsAsciiDigit((unsigned char)h[i]) && h[i] != '.') return false;
  return !h.empty();
}

// SHA-256 of "HelloRetryRequest".
const unsigned char kHrrRandom[32] = {0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C,
                                      0x02, 0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB,
                                      0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};

}  // namespace

std::string Tls13Client::HkdfExtract(bool sha384, const std::string& salt, const std::string& ikm) {
  size_t hlen = sha384 ? 48 : 32;
  return Hmac(sha384, salt.empty() ? std::string(hlen, '\0') : salt, ikm);
}

std::string Tls13Client::HkdfExpandLabel(bool sha384, const std::string& secret, const std::string& label,
                                         const std::string& context, size_t length) {
  std::string full = "tls13 " + label;
  std::string info = U16((int)length) + U8((int)full.size()) + full + U8((int)context.size()) + context;
  std::string out, t;
  for (int i = 1; out.size() < length; ++i) {
    t = Hmac(sha384, secret, t + info + U8(i));
    out += t;
  }
  return out.substr(0, length);
}

bool Tls13Client::VerifyPss(const unsigned char* n, size_t nlen, const unsigned char* e, size_t elen, int hashId,
                            const unsigned char* msgHash, const unsigned char* sig, size_t siglen) {
  const br_hash_class* h = hashId == 256 ? &br_sha256_vtable : hashId == 384 ? &br_sha384_vtable
                           : hashId == 512 ? &br_sha512_vtable : 0;
  if (!h) return false;
  size_t hlen = (h->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
  while (nlen > 0 && *n == 0) {
    ++n;
    --nlen;
  }
  if (siglen != nlen || nlen < hlen * 2 + 2) return false;
  br_rsa_public_key pk;
  pk.n = (unsigned char*)n;
  pk.nlen = nlen;
  pk.e = (unsigned char*)e;
  pk.elen = elen;
  std::string x((const char*)sig, siglen);
  if (!br_rsa_i31_public((unsigned char*)&x[0], x.size(), &pk)) return false;
  // modBits - 1 = emBits
  int topBits = 0;
  for (unsigned v = n[0]; v; v >>= 1) ++topBits;
  size_t modBits = (nlen - 1) * 8 + topBits;
  size_t emBits = modBits - 1, emLen = (emBits + 7) / 8;
  const unsigned char* em = (const unsigned char*)x.data() + (nlen - emLen);
  if (nlen > emLen && x[0] != 0) return false;
  if (em[emLen - 1] != 0xBC) return false;
  size_t dbLen = emLen - hlen - 1;
  const unsigned char* H = em + dbLen;
  unsigned zeroBits = (unsigned)(8 * emLen - emBits);
  if (zeroBits && (em[0] >> (8 - zeroBits))) return false;
  // dbMask = MGF1(H, dbLen)
  std::string db((const char*)em, dbLen);
  for (unsigned c = 0, off = 0; off < dbLen; ++c) {
    br_hash_compat_context hc;
    h->init(&hc.vtable);
    h->update(&hc.vtable, H, hlen);
    unsigned char ctr[4] = {(unsigned char)(c >> 24), (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
    h->update(&hc.vtable, ctr, 4);
    unsigned char mask[64];
    h->out(&hc.vtable, mask);
    for (size_t k = 0; k < hlen && off < dbLen; ++k, ++off) db[off] = (char)(db[off] ^ mask[k]);
  }
  if (zeroBits) db[0] = (char)(db[0] & (0xFF >> zeroBits));
  size_t sLen = hlen;
  if (dbLen < sLen + 1) return false;
  for (size_t i = 0; i < dbLen - sLen - 1; ++i)
    if (db[i] != 0) return false;
  if (db[dbLen - sLen - 1] != 1) return false;
  std::string mprime = std::string(8, '\0') + std::string((const char*)msgHash, hlen) + db.substr(dbLen - sLen);
  std::string hh = Digest(h, mprime);
  return ConstEq(hh, std::string((const char*)H, hlen));
}

Tls13Client::Tls13Client(TcpSocket* sock) : sock_(sock) {}

Tls13Client::~Tls13Client() {}

bool Tls13Client::Fail(const std::string& why) {
  if (error_.empty()) error_ = why;
  return false;
}

std::string Tls13Client::Hash(const std::string& data) const { return Digest(HashClass(sha384_), data); }

std::string Tls13Client::TranscriptHash() const { return Hash(transcript_); }

void Tls13Client::SetTrafficKeys(Keys& k, const std::string& secret) {
  k.secret = secret;
  k.key = HkdfExpandLabel(sha384_, secret, "key", "", keyLen_);
  k.iv = HkdfExpandLabel(sha384_, secret, "iv", "", 12);
  k.seq = 0;
}

bool Tls13Client::ReadRecord(int& type, std::string& payload) {
  char hdr[5];
  int got = 0;
  while (got < 5) {
    int r = sock_->Read(hdr + got, 5 - got);
    if (r <= 0) return Fail("Verbindung während TLS-Handshake getrennt");
    got += r;
  }
  type = (unsigned char)hdr[0];
  size_t len = ((unsigned char)hdr[3] << 8) | (unsigned char)hdr[4];
  if (len > 16384 + 256) return Fail("Ungültiger TLS-Datensatz");
  payload.assign(len, '\0');
  size_t have = 0;
  while (have < len) {
    int r = sock_->Read(&payload[have], (int)(len - have));
    if (r <= 0) return Fail("Verbindung während TLS-Handshake getrennt");
    have += r;
  }
  payload.insert(0, hdr, 5);  // keep the header (it is the AEAD's additional data)
  return true;
}

bool Tls13Client::Decrypt(Keys& k, const std::string& header, std::string& data, int& innerType) {
  if (data.size() < 17) return Fail("TLS-Datensatz zu kurz");
  unsigned char nonce[12];
  memcpy(nonce, k.iv.data(), 12);
  for (int i = 0; i < 8; ++i) nonce[4 + i] ^= (unsigned char)(k.seq >> (56 - 8 * i));
  ++k.seq;
  size_t clen = data.size() - 16;
  std::string tag = data.substr(clen);
  unsigned char calc[16];
  if (suite_ == kChaCha) {
    br_poly1305_ctmul_run(k.key.data(), nonce, &data[0], clen, header.data(), 5, calc, br_chacha20_ct_run, 0);
  } else {
    br_aes_ct_ctr_keys aes;
    br_aes_ct_ctr_init(&aes, k.key.data(), k.key.size());
    br_gcm_context gc;
    br_gcm_init(&gc, &aes.vtable, br_ghash_ctmul32);
    br_gcm_reset(&gc, nonce, 12);
    br_gcm_aad_inject(&gc, header.data(), 5);
    br_gcm_flip(&gc);
    br_gcm_run(&gc, 0, &data[0], clen);
    br_gcm_get_tag(&gc, calc);
  }
  if (!ConstEq(tag, std::string((const char*)calc, 16))) return Fail("TLS: Prüfsumme falsch (Datensatz manipuliert?)");
  data.resize(clen);
  while (!data.empty() && data[data.size() - 1] == 0) data.resize(data.size() - 1);
  if (data.empty()) return Fail("TLS: leerer Datensatz");
  innerType = (unsigned char)data[data.size() - 1];
  data.resize(data.size() - 1);
  return true;
}

bool Tls13Client::WriteRecord(int type, const std::string& data, Keys* keys) {
  for (size_t off = 0; off < data.size() || (off == 0 && data.empty()); off += 16384) {
    std::string chunk = data.substr(off, 16384);
    std::string rec;
    if (!keys) {
      rec = U8(type) + U16(type == 22 && transcript_.size() == chunk.size() ? 0x0301 : 0x0303) + U16((int)chunk.size()) + chunk;
    } else {
      std::string inner = chunk + U8(type);
      std::string header = U8(23) + U16(0x0303) + U16((int)inner.size() + 16);
      unsigned char nonce[12];
      memcpy(nonce, keys->iv.data(), 12);
      for (int i = 0; i < 8; ++i) nonce[4 + i] ^= (unsigned char)(keys->seq >> (56 - 8 * i));
      ++keys->seq;
      unsigned char tag[16];
      if (suite_ == kChaCha) {
        br_poly1305_ctmul_run(keys->key.data(), nonce, &inner[0], inner.size(), header.data(), 5, tag,
                              br_chacha20_ct_run, 1);
      } else {
        br_aes_ct_ctr_keys aes;
        br_aes_ct_ctr_init(&aes, keys->key.data(), keys->key.size());
        br_gcm_context gc;
        br_gcm_init(&gc, &aes.vtable, br_ghash_ctmul32);
        br_gcm_reset(&gc, nonce, 12);
        br_gcm_aad_inject(&gc, header.data(), 5);
        br_gcm_flip(&gc);
        br_gcm_run(&gc, 1, &inner[0], inner.size());
        br_gcm_get_tag(&gc, tag);
      }
      rec = header + inner + std::string((const char*)tag, 16);
    }
    if (!sock_->WriteAll(rec.data(), (int)rec.size())) return Fail("Senden fehlgeschlagen");
    if (data.empty()) break;
  }
  return true;
}

bool Tls13Client::SendAlert(int desc) {
  std::string a = U8(desc == 0 ? 1 : 2) + U8(desc);
  return WriteRecord(21, a, encrypted_ ? &clientAp_ : 0);
}

// Next handshake message (type + body, header included in |msg|).
bool Tls13Client::NextHandshake(int& type, std::string& msg, bool encrypted) {
  for (;;) {
    if (hsBuffer_.size() >= 4) {
      size_t len = ((unsigned char)hsBuffer_[1] << 16) | ((unsigned char)hsBuffer_[2] << 8) | (unsigned char)hsBuffer_[3];
      if (len > (1 << 20)) return Fail("TLS-Nachricht zu groß");
      if (hsBuffer_.size() >= 4 + len) {
        type = (unsigned char)hsBuffer_[0];
        msg = hsBuffer_.substr(0, 4 + len);
        hsBuffer_.erase(0, 4 + len);
        return true;
      }
    }
    int rtype;
    std::string rec;
    if (!ReadRecord(rtype, rec)) return false;
    std::string header = rec.substr(0, 5), body = rec.substr(5);
    if (rtype == 20) continue;  // middlebox-compatibility ChangeCipherSpec
    if (rtype == 21) {
      if (body.size() >= 2) return Fail("TLS-Fehlermeldung vom Server (" + IntToString((unsigned char)body[1]) + ")");
      return Fail("TLS-Fehlermeldung vom Server");
    }
    if (encrypted) {
      if (rtype != 23) return Fail("Unverschlüsselter Datensatz nach dem ServerHello");
      int inner;
      if (!Decrypt(serverHs_, header, body, inner)) return false;
      if (inner == 21) return Fail("TLS-Fehlermeldung vom Server");
      if (inner != 22) return Fail("Unerwarteter TLS-Datensatz");
    } else if (rtype != 22) {
      return Fail("Unerwarteter TLS-Datensatz");
    }
    hsBuffer_ += body;
  }
}

Tls13Client::Result Tls13Client::Handshake(const std::string& host, const char* const* alpn, int alpnCount,
                                           const void* anchors, size_t anchorCount) {
  // Key shares: X25519 and P-256.
  std::string xPriv = RandomBytes(32);
  unsigned char xPub[32];
  br_ec_c25519_i31.mulgen(xPub, (const unsigned char*)xPriv.data(), 32, BR_EC_curve25519);
  const br_ec_impl* ec = br_ec_get_default();
  br_ec_private_key pSk;
  br_ec_public_key pPk;
  unsigned char pSkBuf[BR_EC_KBUF_PRIV_MAX_SIZE], pPkBuf[BR_EC_KBUF_PUB_MAX_SIZE];
  {
    MutexLock l(g_rngMu);
    br_ec_keygen(&Rng()->vtable, ec, &pSk, pSkBuf, BR_EC_secp256r1);
  }
  br_ec_compute_pub(ec, &pPk, pPkBuf, &pSk);
  std::string sessionId = RandomBytes(32), clientRandom = RandomBytes(32);
  std::string cookie;
  int onlyGroup = 0;
  bool retried = false;
  std::string serverHello;

  for (;;) {
    // ClientHello.
    std::string ext;
    auto addExt = [&](int type, const std::string& body) { ext += U16(type) + U16((int)body.size()) + body; };
    if (!IsIpAddress(host)) {
      std::string list = U8(0) + U16((int)host.size()) + host;
      addExt(0, U16((int)list.size()) + list);
    }
    addExt(10, U16(4) + U16(0x001D) + U16(0x0017));
    static const int sigs[] = {0x0403, 0x0804, 0x0503, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601, 0x0603, 0x0201, 0x0203};
    std::string sl;
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; ++i) sl += U16(sigs[i]);
    addExt(13, U16((int)sl.size()) + sl);
    addExt(43, U8(2) + U16(0x0304));
    addExt(45, U8(1) + U8(1));
    std::string shares;
    if (!onlyGroup || onlyGroup == 0x001D) shares += U16(0x001D) + U16(32) + std::string((const char*)xPub, 32);
    if (!onlyGroup || onlyGroup == 0x0017) shares += U16(0x0017) + U16((int)pPk.qlen) + std::string((const char*)pPk.q, pPk.qlen);
    addExt(51, U16((int)shares.size()) + shares);
    if (alpn && alpnCount > 0) {
      std::string list;
      for (int i = 0; i < alpnCount; ++i) list += U8((int)strlen(alpn[i])) + alpn[i];
      addExt(16, U16((int)list.size()) + list);
    }
    if (!cookie.empty()) addExt(44, U16((int)cookie.size()) + cookie);
    std::string suites = U16(kChaCha) + U16(kAes128) + U16(kAes256);
    std::string body = U16(0x0303) + clientRandom + U8(32) + sessionId + U16((int)suites.size()) + suites + U8(1) +
                       U8(0) + U16((int)ext.size()) + ext;
    std::string ch = U8(1) + U24((int)body.size()) + body;
    transcript_ += ch;
    if (!WriteRecord(22, ch, 0)) return kFailed;
    int type;
    if (!NextHandshake(type, serverHello, false)) return retried ? kFailed : kNeedsTls12;
    if (type != 2) {
      Fail("Unerwartete Antwort statt ServerHello");
      return kNeedsTls12;
    }
    Parser p(serverHello, 4);
    p.Short();
    std::string random = p.Bytes(32);
    p.Bytes(p.Byte());
    int suite = p.Short();
    p.Byte();
    size_t extEnd = p.Short();
    extEnd += p.p;
    int version = 0, group = 0;
    std::string serverShare, hrrCookie;
    while (p.ok && p.p + 4 <= extEnd) {
      int et = p.Short();
      size_t el = p.Short();
      Parser e(p.s, p.p);
      if (et == 43) version = e.Short();
      else if (et == 51) {
        group = e.Short();
        if (el > 2) serverShare = e.Bytes(e.Short());
      } else if (et == 44) hrrCookie = e.Bytes(e.Short());
      p.Bytes(el);
    }
    if (!p.ok) {
      Fail("Ungültiges ServerHello");
      return kFailed;
    }
    if (version != 0x0304) return kNeedsTls12;  // a TLS 1.2 (or older) server
    if (suite != kChaCha && suite != kAes128 && suite != kAes256) {
      Fail("Server wählte eine unbekannte TLS-1.3-Verschlüsselung");
      return kFailed;
    }
    suite_ = suite;
    sha384_ = suite == kAes256;
    keyLen_ = suite == kAes128 ? 16 : 32;
    if (memcmp(random.data(), kHrrRandom, 32) == 0) {
      // HelloRetryRequest: the transcript continues with a hash of CH1.
      if (retried || (group != 0x001D && group != 0x0017)) {
        Fail("Server verlangt eine nicht unterstützte Schlüsselaustauschgruppe");
        return kFailed;
      }
      retried = true;
      onlyGroup = group;
      cookie = hrrCookie;
      std::string h1 = Hash(transcript_);
      transcript_ = U8(254) + U24((int)h1.size()) + h1 + serverHello;
      continue;
    }
    // ECDHE shared secret.
    std::string shared;
    if (group == 0x001D && serverShare.size() == 32) {
      std::string pt = serverShare;
      if (!br_ec_c25519_i31.mul((unsigned char*)&pt[0], 32, (const unsigned char*)xPriv.data(), 32, BR_EC_curve25519)) {
        Fail("Ungültiger X25519-Schlüssel vom Server");
        return kFailed;
      }
      shared = pt;
    } else if (group == 0x0017 && serverShare.size() == 65) {
      std::string pt = serverShare;
      if (!ec->mul((unsigned char*)&pt[0], pt.size(), pSk.x, pSk.xlen, BR_EC_secp256r1)) {
        Fail("Ungültiger P-256-Schlüssel vom Server");
        return kFailed;
      }
      shared = pt.substr(1, 32);
    } else {
      Fail("Ungültiger Schlüsselaustausch im ServerHello");
      return kFailed;
    }
    transcript_ += serverHello;
    size_t hlen = sha384_ ? 48 : 32;
    std::string zeros(hlen, '\0');
    std::string early = HkdfExtract(sha384_, "", zeros);
    std::string derived = HkdfExpandLabel(sha384_, early, "derived", Hash(""), hlen);
    std::string hs = HkdfExtract(sha384_, derived, shared);
    SetTrafficKeys(clientHs_, HkdfExpandLabel(sha384_, hs, "c hs traffic", TranscriptHash(), hlen));
    SetTrafficKeys(serverHs_, HkdfExpandLabel(sha384_, hs, "s hs traffic", TranscriptHash(), hlen));
    std::string derived2 = HkdfExpandLabel(sha384_, hs, "derived", Hash(""), hlen);
    std::string master = HkdfExtract(sha384_, derived2, zeros);

    // Encrypted server flight.
    bool certRequested = false;
    std::string certContext;
    std::unique_ptr<br_x509_minimal_context> xc(new br_x509_minimal_context);
    std::string keyN, keyE, keyQ;
    int keyType = 0, keyCurve = 0;
    bool haveCert = false;
    for (;;) {
      int mt;
      std::string m;
      if (!NextHandshake(mt, m, true)) return kFailed;
      Parser mp(m, 4);
      if (mt == 8) {  // EncryptedExtensions
        size_t end = mp.Short();
        end += mp.p;
        while (mp.ok && mp.p + 4 <= end) {
          int et = mp.Short();
          size_t el = mp.Short();
          Parser e(m, mp.p);
          if (et == 16) {
            e.Short();
            alpn_ = e.Bytes(e.Byte());
          }
          mp.Bytes(el);
        }
        transcript_ += m;
      } else if (mt == 13) {  // CertificateRequest
        certRequested = true;
        certContext = mp.Bytes(mp.Byte());
        transcript_ += m;
      } else if (mt == 11) {  // Certificate
        mp.Bytes(mp.Byte());
        size_t end = mp.Tri();
        end += mp.p;
        br_x509_minimal_init(xc.get(), &br_sha256_vtable, (const br_x509_trust_anchor*)anchors, anchorCount);
        br_x509_minimal_set_hash(xc.get(), br_sha1_ID, &br_sha1_vtable);
        br_x509_minimal_set_hash(xc.get(), br_sha224_ID, &br_sha224_vtable);
        br_x509_minimal_set_hash(xc.get(), br_sha256_ID, &br_sha256_vtable);
        br_x509_minimal_set_hash(xc.get(), br_sha384_ID, &br_sha384_vtable);
        br_x509_minimal_set_hash(xc.get(), br_sha512_ID, &br_sha512_vtable);
        br_x509_minimal_set_rsa(xc.get(), br_rsa_pkcs1_vrfy_get_default());
        br_x509_minimal_set_ecdsa(xc.get(), br_ec_get_default(), br_ecdsa_vrfy_asn1_get_default());
        const br_x509_class** x = &xc->vtable;
        (*x)->start_chain(x, IsIpAddress(host) ? 0 : host.c_str());
        std::vector<std::string> seen;
        while (mp.ok && mp.p + 3 <= end) {
          std::string der = mp.Bytes(mp.Tri());
          mp.Bytes(mp.Short());  // extensions
          bool dup = false;
          for (size_t i = 0; i < seen.size(); ++i) dup = dup || seen[i] == der;
          if (dup || der.empty()) continue;
          seen.push_back(der);
          (*x)->start_cert(x, (uint32_t)der.size());
          (*x)->append(x, (const unsigned char*)der.data(), der.size());
          (*x)->end_cert(x);
        }
        unsigned err = (*x)->end_chain(x);
        if (err != 0) {
          Fail(X509ErrorText((int)err));
          return kFailed;
        }
        unsigned usages = 0;
        const br_x509_pkey* pk = (*x)->get_pkey(x, &usages);
        if (!pk) {
          Fail("Kein Schlüssel im Serverzertifikat");
          return kFailed;
        }
        keyType = pk->key_type;
        if (keyType == BR_KEYTYPE_RSA) {
          keyN.assign((const char*)pk->key.rsa.n, pk->key.rsa.nlen);
          keyE.assign((const char*)pk->key.rsa.e, pk->key.rsa.elen);
        } else {
          keyQ.assign((const char*)pk->key.ec.q, pk->key.ec.qlen);
          keyCurve = pk->key.ec.curve;
        }
        haveCert = true;
        transcript_ += m;
      } else if (mt == 15) {  // CertificateVerify
        if (!haveCert) {
          Fail("CertificateVerify ohne Zertifikat");
          return kFailed;
        }
        int alg = mp.Short();
        std::string sig = mp.Bytes(mp.Short());
        std::string content = std::string(64, ' ') + "TLS 1.3, server CertificateVerify" + std::string(1, '\0') +
                              TranscriptHash();
        bool ok = false;
        if ((alg == 0x0403 || alg == 0x0503 || alg == 0x0603) && keyType == BR_KEYTYPE_EC) {
          const br_hash_class* h = alg == 0x0403 ? &br_sha256_vtable : alg == 0x0503 ? &br_sha384_vtable : &br_sha512_vtable;
          std::string dg = Digest(h, content);
          br_ec_public_key epk;
          epk.curve = keyCurve;
          epk.q = (unsigned char*)&keyQ[0];
          epk.qlen = keyQ.size();
          ok = br_ecdsa_vrfy_asn1_get_default()(br_ec_get_default(), dg.data(), dg.size(), &epk, sig.data(), sig.size()) == 1;
        } else if (alg >= 0x0804 && alg <= 0x0806 && keyType == BR_KEYTYPE_RSA) {
          int id = alg == 0x0804 ? 256 : alg == 0x0805 ? 384 : 512;
          std::string dg = Digest(id == 256 ? &br_sha256_vtable : id == 384 ? &br_sha384_vtable : &br_sha512_vtable, content);
          ok = VerifyPss((const unsigned char*)keyN.data(), keyN.size(), (const unsigned char*)keyE.data(), keyE.size(), id,
                         (const unsigned char*)dg.data(), (const unsigned char*)sig.data(), sig.size());
        }
        if (!ok) {
          Fail("Ungültige Serversignatur (CertificateVerify)");
          return kFailed;
        }
        transcript_ += m;
      } else if (mt == 20) {  // Finished
        if (!haveCert) {
          Fail("Server hat sich nicht ausgewiesen");
          return kFailed;
        }
        std::string fk = HkdfExpandLabel(sha384_, serverHs_.secret, "finished", "", hlen);
        std::string expect = Hmac(sha384_, fk, TranscriptHash());
        if (!ConstEq(expect, m.substr(4))) {
          Fail("TLS: Finished-Prüfung fehlgeschlagen");
          return kFailed;
        }
        transcript_ += m;
        break;
      } else {
        Fail("Unerwartete TLS-Handshake-Nachricht " + IntToString(mt));
        return kFailed;
      }
    }
    std::string th = TranscriptHash();
    SetTrafficKeys(clientAp_, HkdfExpandLabel(sha384_, master, "c ap traffic", th, hlen));
    SetTrafficKeys(serverAp_, HkdfExpandLabel(sha384_, master, "s ap traffic", th, hlen));
    // Client flight: (empty Certificate), Finished.
    std::string ccs = U8(20) + U16(0x0303) + U16(1) + U8(1);
    if (!sock_->WriteAll(ccs.data(), (int)ccs.size())) return kFailed;
    if (certRequested) {
      std::string cb = U8((int)certContext.size()) + certContext + U24(0);
      std::string cm = U8(11) + U24((int)cb.size()) + cb;
      transcript_ += cm;
      if (!WriteRecord(22, cm, &clientHs_)) return kFailed;
    }
    std::string fk = HkdfExpandLabel(sha384_, clientHs_.secret, "finished", "", hlen);
    std::string vd = Hmac(sha384_, fk, TranscriptHash());
    std::string fin = U8(20) + U24((int)vd.size()) + vd;
    transcript_ += fin;
    if (!WriteRecord(22, fin, &clientHs_)) return kFailed;
    encrypted_ = true;
    std::string().swap(transcript_);
    return kOk;
  }
}

int Tls13Client::Read(char* buf, int len) {
  while (pos_ >= plain_.size()) {
    if (eof_) return 0;
    plain_.clear();
    pos_ = 0;
    int rtype;
    std::string rec;
    if (!ReadRecord(rtype, rec)) {
      eof_ = true;
      return 0;  // the server just closed the connection
    }
    if (rtype == 20) continue;
    std::string header = rec.substr(0, 5), body = rec.substr(5);
    if (rtype != 23) {
      error_ = "Unerwarteter TLS-Datensatz";
      return -1;
    }
    int inner;
    if (!Decrypt(serverAp_, header, body, inner)) return -1;
    if (inner == 23) {
      plain_.swap(body);
    } else if (inner == 21) {
      if (body.size() >= 2 && body[1] == 0) {
        eof_ = true;
        return 0;
      }
      error_ = "TLS-Fehlermeldung vom Server";
      return -1;
    } else if (inner == 22) {
      hsBuffer_ += body;
      while (hsBuffer_.size() >= 4) {
        size_t mlen = ((unsigned char)hsBuffer_[1] << 16) | ((unsigned char)hsBuffer_[2] << 8) | (unsigned char)hsBuffer_[3];
        if (hsBuffer_.size() < 4 + mlen) break;
        int mt = (unsigned char)hsBuffer_[0];
        std::string m = hsBuffer_.substr(4, mlen);
        hsBuffer_.erase(0, 4 + mlen);
        if (mt == 24 && !m.empty()) {  // KeyUpdate
          size_t hlen = sha384_ ? 48 : 32;
          SetTrafficKeys(serverAp_, HkdfExpandLabel(sha384_, serverAp_.secret, "traffic upd", "", hlen));
          if (m[0] == 1) {
            std::string ku = U8(24) + U24(1) + U8(0);
            if (!WriteRecord(22, ku, &clientAp_)) return -1;
            SetTrafficKeys(clientAp_, HkdfExpandLabel(sha384_, clientAp_.secret, "traffic upd", "", hlen));
          }
        }
        // NewSessionTicket (4) and others: ignored (no session resumption).
      }
    }
  }
  int n = (int)std::min<size_t>((size_t)len, plain_.size() - pos_);
  memcpy(buf, plain_.data() + pos_, n);
  pos_ += n;
  return n;
}

bool Tls13Client::WriteAll(const char* buf, int len) {
  if (len <= 0) return true;
  return WriteRecord(23, std::string(buf, len), &clientAp_);
}

}  // namespace kite
