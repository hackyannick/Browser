// TLS 1.0-1.2 client built on BearSSL (portable C, no OS crypto needed, which
// is what makes modern HTTPS possible on Windows 2000).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "base/mutex.h"
#include "base/strings.h"
#include "net/socket.h"
#include "net/tls13.h"

extern "C" {
#include "bearssl.h"
}

namespace kite {

namespace {

struct AnchorStore {
  std::vector<br_x509_trust_anchor> anchors;
  std::vector<std::vector<unsigned char>*> blobs;  // owned memory
};

AnchorStore& Store() {
  static AnchorStore* s = new AnchorStore;
  return *s;
}

Mutex& StoreMutex() {
  static Mutex* m = new Mutex;
  return *m;
}

void AppendDn(void* ctx, const void* buf, size_t len) {
  std::vector<unsigned char>* v = static_cast<std::vector<unsigned char>*>(ctx);
  const unsigned char* p = static_cast<const unsigned char*>(buf);
  v->insert(v->end(), p, p + len);
}

unsigned char* Keep(AnchorStore& st, const unsigned char* data, size_t len) {
  std::vector<unsigned char>* v = new std::vector<unsigned char>(data, data + len);
  st.blobs.push_back(v);
  return v->empty() ? 0 : &(*v)[0];
}

bool AddCertificate(AnchorStore& st, const std::string& der) {
  std::vector<unsigned char> dn;
  br_x509_decoder_context dc;
  br_x509_decoder_init(&dc, AppendDn, &dn);
  br_x509_decoder_push(&dc, der.data(), der.size());
  br_x509_pkey* pk = br_x509_decoder_get_pkey(&dc);
  if (!pk || br_x509_decoder_last_error(&dc) != 0) return false;
  br_x509_trust_anchor ta;
  memset(&ta, 0, sizeof ta);
  ta.dn.data = Keep(st, dn.empty() ? 0 : &dn[0], dn.size());
  ta.dn.len = dn.size();
  ta.flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
  switch (pk->key_type) {
    case BR_KEYTYPE_RSA:
      ta.pkey.key_type = BR_KEYTYPE_RSA;
      ta.pkey.key.rsa.n = Keep(st, pk->key.rsa.n, pk->key.rsa.nlen);
      ta.pkey.key.rsa.nlen = pk->key.rsa.nlen;
      ta.pkey.key.rsa.e = Keep(st, pk->key.rsa.e, pk->key.rsa.elen);
      ta.pkey.key.rsa.elen = pk->key.rsa.elen;
      break;
    case BR_KEYTYPE_EC:
      ta.pkey.key_type = BR_KEYTYPE_EC;
      ta.pkey.key.ec.curve = pk->key.ec.curve;
      ta.pkey.key.ec.q = Keep(st, pk->key.ec.q, pk->key.ec.qlen);
      ta.pkey.key.ec.qlen = pk->key.ec.qlen;
      break;
    default:
      return false;
  }
  st.anchors.push_back(ta);
  return true;
}

}  // namespace

int LoadTrustAnchors(const std::string& pem) {
  MutexLock lock(StoreMutex());
  AnchorStore& st = Store();
  int count = 0;
  const std::string begin = "-----BEGIN CERTIFICATE-----";
  const std::string end = "-----END CERTIFICATE-----";
  size_t p = 0;
  while ((p = pem.find(begin, p)) != std::string::npos) {
    size_t e = pem.find(end, p);
    if (e == std::string::npos) break;
    std::string der = Base64Decode(pem.substr(p + begin.size(), e - p - begin.size()));
    if (AddCertificate(st, der)) ++count;
    p = e + end.size();
  }
  return count;
}

int TrustAnchorCount() {
  MutexLock lock(StoreMutex());
  return (int)Store().anchors.size();
}

// X.509 validator wrapper that drops byte-identical duplicate certificates
// from the server's chain before handing it to the strict BearSSL validator
// (some servers send their leaf certificate twice).
struct DedupX509 {
  const br_x509_class* vtable;
  const br_x509_class** inner;
  std::vector<std::string> seen;
  std::string current;
};

static void DedupStartChain(const br_x509_class** ctx, const char* serverName) {
  DedupX509* d = (DedupX509*)ctx;
  d->seen.clear();
  (*d->inner)->start_chain(d->inner, serverName);
}
static void DedupStartCert(const br_x509_class** ctx, uint32_t length) {
  DedupX509* d = (DedupX509*)ctx;
  d->current.clear();
  d->current.reserve(length);
}
static void DedupAppend(const br_x509_class** ctx, const unsigned char* buf, size_t len) {
  DedupX509* d = (DedupX509*)ctx;
  d->current.append((const char*)buf, len);
}
static void DedupEndCert(const br_x509_class** ctx) {
  DedupX509* d = (DedupX509*)ctx;
  for (size_t i = 0; i < d->seen.size(); ++i)
    if (d->seen[i] == d->current) return;
  d->seen.push_back(d->current);
  (*d->inner)->start_cert(d->inner, (uint32_t)d->current.size());
  (*d->inner)->append(d->inner, (const unsigned char*)d->current.data(), d->current.size());
  (*d->inner)->end_cert(d->inner);
}
static unsigned DedupEndChain(const br_x509_class** ctx) {
  DedupX509* d = (DedupX509*)ctx;
  std::vector<std::string>().swap(d->seen);
  return (*d->inner)->end_chain(d->inner);
}
static const br_x509_pkey* DedupGetPkey(const br_x509_class* const* ctx, unsigned* usages) {
  const DedupX509* d = (const DedupX509*)ctx;
  return (*d->inner)->get_pkey(d->inner, usages);
}
static const br_x509_class kDedupClass = {
    sizeof(DedupX509), DedupStartChain, DedupStartCert, DedupAppend,
    DedupEndCert,      DedupEndChain,   DedupGetPkey};

// Hosts that answered a TLS 1.3 ClientHello with TLS 1.2 (or not at all).
namespace {
Mutex g_tls12Mu;
std::vector<std::string> g_tls12Hosts;
bool g_tls13 = true;
bool IsTls12Only(const std::string& host) {
  MutexLock l(g_tls12Mu);
  for (size_t i = 0; i < g_tls12Hosts.size(); ++i)
    if (g_tls12Hosts[i] == host) return true;
  return !g_tls13;
}
void MarkTls12Only(const std::string& host) {
  MutexLock l(g_tls12Mu);
  if (g_tls12Hosts.size() > 1000) g_tls12Hosts.clear();
  g_tls12Hosts.push_back(host);
}
}  // namespace

void SetTls13Enabled(bool on) {
  MutexLock l(g_tls12Mu);
  g_tls13 = on;
}

struct TlsStream::Impl {
  std::unique_ptr<Tls13Client> t13;
  br_ssl_client_context sc;
  br_x509_minimal_context xc;
  DedupX509 dedup;
  br_sslio_context ioc;
  std::vector<unsigned char> iobuf;
};

static int SockRead(void* ctx, unsigned char* buf, size_t len) {
  TcpSocket* s = static_cast<TcpSocket*>(ctx);
  int r = s->Read(reinterpret_cast<char*>(buf), (int)len);
  return r <= 0 ? -1 : r;
}

static int SockWrite(void* ctx, const unsigned char* buf, size_t len) {
  TcpSocket* s = static_cast<TcpSocket*>(ctx);
  return s->WriteAll(reinterpret_cast<const char*>(buf), (int)len) ? (int)len : -1;
}

TlsStream::TlsStream(TcpSocket* sock) : impl_(new Impl), sock_(sock) {
  impl_->iobuf.resize(BR_SSL_BUFSIZE_BIDI);
}

TlsStream::~TlsStream() { delete impl_; }

static std::string TlsErrorText(int err);
std::string X509ErrorText(int err) { return TlsErrorText(err); }

static std::string TlsErrorText(int err) {
  switch (err) {
    case BR_ERR_X509_EXPIRED:
      return "Zertifikat abgelaufen oder noch nicht gültig (Systemuhr prüfen)";
    case BR_ERR_X509_NOT_TRUSTED:
      return "Zertifikat nicht vertrauenswürdig (unbekannte Zertifizierungsstelle)";
    case BR_ERR_X509_BAD_SERVER_NAME:
      return "Zertifikat passt nicht zum Servernamen";
    case BR_ERR_BAD_VERSION:
    case BR_ERR_UNSUPPORTED_VERSION:
      return "Server unterstützt kein TLS 1.0-1.2";
    case BR_ERR_BAD_CIPHER_SUITE:
      return "Keine gemeinsame Verschlüsselung mit dem Server";
    case BR_ERR_IO:
      return "Verbindung während TLS-Handshake getrennt";
    default:
      return "TLS-Fehler " + IntToString(err);
  }
}

bool TlsStream::Handshake(const std::string& host, const char* const* alpn, int alpnCount) {
  // Anchors are loaded once at startup and read-only afterwards.
  const br_x509_trust_anchor* tas;
  size_t count;
  {
    MutexLock lock(StoreMutex());
    AnchorStore& st = Store();
    tas = st.anchors.empty() ? 0 : &st.anchors[0];
    count = st.anchors.size();
  }
  needs12_ = false;
  if (!IsTls12Only(host)) {
    impl_->t13.reset(new Tls13Client(sock_));
    Tls13Client::Result r = impl_->t13->Handshake(host, alpn, alpnCount, tas, count);
    if (r == Tls13Client::kOk) return true;
    error_ = impl_->t13->error();
    impl_->t13.reset();
    if (r == Tls13Client::kNeedsTls12) {
      if (getenv("KITE_TLS_DEBUG")) fprintf(stderr, "TLS 1.3 -> 1.2 for %s: %s\n", host.c_str(), error_.c_str());
      MarkTls12Only(host);
      needs12_ = true;
      if (error_.empty()) error_ = "Server spricht nur TLS 1.2";
    }
    return false;
  }
  br_ssl_client_init_full(&impl_->sc, &impl_->xc, tas, count);
  impl_->dedup.vtable = &kDedupClass;
  impl_->dedup.inner = &impl_->xc.vtable;
  br_ssl_engine_set_x509(&impl_->sc.eng, &impl_->dedup.vtable);
  br_ssl_engine_set_buffer(&impl_->sc.eng, &impl_->iobuf[0], impl_->iobuf.size(), 1);
  if (alpn && alpnCount > 0) br_ssl_engine_set_protocol_names(&impl_->sc.eng, (const char**)alpn, alpnCount);
  if (!br_ssl_client_reset(&impl_->sc, host.c_str(), 0)) {
    error_ = "TLS-Initialisierung fehlgeschlagen";
    return false;
  }
  br_sslio_init(&impl_->ioc, &impl_->sc.eng, SockRead, sock_, SockWrite, sock_);
  // Drive the handshake explicitly so errors surface here.
  if (br_sslio_flush(&impl_->ioc) < 0) {
    int err = br_ssl_engine_last_error(&impl_->sc.eng);
    error_ = TlsErrorText(err);
    return false;
  }
  for (;;) {
    unsigned state = br_ssl_engine_current_state(&impl_->sc.eng);
    if (state & BR_SSL_CLOSED) {
      error_ = TlsErrorText(br_ssl_engine_last_error(&impl_->sc.eng));
      return false;
    }
    if (state & BR_SSL_SENDAPP) return true;
    if (state & BR_SSL_RECVREC) {
      size_t len;
      unsigned char* buf = br_ssl_engine_recvrec_buf(&impl_->sc.eng, &len);
      int r = SockRead(sock_, buf, len);
      if (r < 0) {
        error_ = "Verbindung während TLS-Handshake getrennt";
        return false;
      }
      br_ssl_engine_recvrec_ack(&impl_->sc.eng, r);
      continue;
    }
    if (state & BR_SSL_SENDREC) {
      size_t len;
      unsigned char* buf = br_ssl_engine_sendrec_buf(&impl_->sc.eng, &len);
      int w = SockWrite(sock_, buf, len);
      if (w < 0) {
        error_ = "Verbindung während TLS-Handshake getrennt";
        return false;
      }
      br_ssl_engine_sendrec_ack(&impl_->sc.eng, w);
      continue;
    }
    if (state & BR_SSL_RECVAPP) return true;
  }
}

std::string TlsStream::version() const { return impl_->t13 ? "TLS 1.3" : "TLS 1.2"; }

std::string TlsStream::selectedProtocol() const {
  if (impl_->t13) return impl_->t13->alpn();
  const char* p = br_ssl_engine_get_selected_protocol(const_cast<br_ssl_engine_context*>(&impl_->sc.eng));
  return p ? p : "";
}

bool TlsStream::WaitReadable(int timeoutMs) {
  if (impl_->t13) return impl_->t13->HasBuffered() || sock_->WaitReadable(timeoutMs);
  unsigned state = br_ssl_engine_current_state(&impl_->sc.eng);
  if (state & (BR_SSL_RECVAPP | BR_SSL_CLOSED)) return true;
  return sock_->WaitReadable(timeoutMs);
}

int TlsStream::Read(char* buf, int len) {
  if (impl_->t13) {
    int r = impl_->t13->Read(buf, len);
    if (r < 0) error_ = impl_->t13->error();
    return r;
  }
  int r = br_sslio_read(&impl_->ioc, buf, len);
  if (r < 0) {
    int err = br_ssl_engine_last_error(&impl_->sc.eng);
    // A clean close_notify (or a server that just drops the connection
    // after the body) is reported as EOF.
    if (err == BR_ERR_OK || err == BR_ERR_IO) return 0;
    error_ = TlsErrorText(err);
    return -1;
  }
  return r;
}

bool TlsStream::WriteAll(const char* buf, int len) {
  if (impl_->t13) {
    if (impl_->t13->WriteAll(buf, len)) return true;
    error_ = impl_->t13->error();
    return false;
  }
  if (br_sslio_write_all(&impl_->ioc, buf, len) < 0 || br_sslio_flush(&impl_->ioc) < 0) {
    error_ = TlsErrorText(br_ssl_engine_last_error(&impl_->sc.eng));
    return false;
  }
  return true;
}

}  // namespace kite
