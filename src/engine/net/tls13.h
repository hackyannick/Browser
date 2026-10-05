// Kite Engine - TLS 1.3 client (RFC 8446) on BearSSL's primitives (BearSSL
// itself stops at TLS 1.2). Cipher suites: TLS_CHACHA20_POLY1305_SHA256,
// TLS_AES_128_GCM_SHA256, TLS_AES_256_GCM_SHA384; groups: X25519, P-256;
// server signatures: ECDSA (P-256/P-384) and RSA-PSS.
#ifndef KITE_NET_TLS13_H
#define KITE_NET_TLS13_H

#include <string>
#include <vector>

namespace kite {

class TcpSocket;

class Tls13Client {
 public:
  explicit Tls13Client(TcpSocket* sock);
  ~Tls13Client();
  enum Result { kOk, kFailed, kNeedsTls12 };
  // |anchors| is a br_x509_trust_anchor array.
  Result Handshake(const std::string& host, const char* const* alpn, int alpnCount, const void* anchors,
                   size_t anchorCount);
  int Read(char* buf, int len);  // bytes, 0 at EOF, -1 on error
  bool WriteAll(const char* buf, int len);
  bool HasBuffered() const { return pos_ < plain_.size(); }
  const std::string& alpn() const { return alpn_; }
  const std::string& error() const { return error_; }

  // Building blocks, exposed for tests.
  static std::string HkdfExtract(bool sha384, const std::string& salt, const std::string& ikm);
  static std::string HkdfExpandLabel(bool sha384, const std::string& secret, const std::string& label,
                                     const std::string& context, size_t length);
  static bool VerifyPss(const unsigned char* n, size_t nlen, const unsigned char* e, size_t elen, int hashId,
                        const unsigned char* msgHash, const unsigned char* sig, size_t siglen);

 private:
  struct Keys {
    std::string key, iv, secret;
    unsigned long long seq = 0;
  };
  bool ReadRecord(int& type, std::string& payload);
  bool WriteRecord(int type, const std::string& data, Keys* keys);
  bool Decrypt(Keys& k, const std::string& header, std::string& data, int& innerType);
  bool NextHandshake(int& type, std::string& msg, bool encrypted);
  std::string Hash(const std::string& data) const;
  std::string TranscriptHash() const;
  void SetTrafficKeys(Keys& k, const std::string& secret);
  bool Fail(const std::string& why);
  bool SendAlert(int desc);

  TcpSocket* sock_;
  int suite_ = 0;
  bool sha384_ = false;
  size_t keyLen_ = 16;
  std::string transcript_;
  std::string hsBuffer_;  // handshake bytes not yet consumed
  Keys clientHs_, serverHs_, clientAp_, serverAp_;
  bool encrypted_ = false;
  std::string plain_;  // decrypted application data
  size_t pos_ = 0;
  bool eof_ = false;
  std::string alpn_, error_;
};

}  // namespace kite

#endif
