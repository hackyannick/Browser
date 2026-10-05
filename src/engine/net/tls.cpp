// TLS 1.0-1.2 client built on BearSSL (portable C, no OS crypto needed, which
// is what makes modern HTTPS possible on Windows 2000).
#include <cstring>
#include <vector>

#include "base/mutex.h"
#include "base/strings.h"
#include "net/socket.h"

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

struct TlsStream::Impl {
  br_ssl_client_context sc;
  br_x509_minimal_context xc;
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

bool TlsStream::Handshake(const std::string& host) {
  // Anchors are loaded once at startup and read-only afterwards.
  const br_x509_trust_anchor* tas;
  size_t count;
  {
    MutexLock lock(StoreMutex());
    AnchorStore& st = Store();
    tas = st.anchors.empty() ? 0 : &st.anchors[0];
    count = st.anchors.size();
  }
  br_ssl_client_init_full(&impl_->sc, &impl_->xc, tas, count);
  br_ssl_engine_set_buffer(&impl_->sc.eng, &impl_->iobuf[0], impl_->iobuf.size(), 1);
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

int TlsStream::Read(char* buf, int len) {
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
  if (br_sslio_write_all(&impl_->ioc, buf, len) < 0 || br_sslio_flush(&impl_->ioc) < 0) {
    error_ = TlsErrorText(br_ssl_engine_last_error(&impl_->sc.eng));
    return false;
  }
  return true;
}

}  // namespace kite
