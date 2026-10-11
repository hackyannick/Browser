// Kite Engine - host name resolution.
//
// The system resolver (gethostbyname on Windows 2000, which has no
// getaddrinfo) runs on its own thread. If it has not answered after 1.5 s,
// the name is also looked up directly over UDP at public DNS servers and
// the first answer wins; once the system resolver proved slow or broken,
// later names are looked up directly first. This also covers the Windows
// 2000 resolver keeping a failed lookup in its negative cache. Results are
// cached for a few minutes, so a page with many resources on the same
// hosts does not flood the old Windows DNS client.
#ifndef KITE_NET_DNS_H
#define KITE_NET_DNS_H

#include <string>

namespace kite {

// Resolves |host| to an IPv4 address in network byte order. On failure
// |error| gets a German message.
bool ResolveHost(const std::string& host, unsigned long& ip, std::string& error);

// DNS wire format helpers (exposed for tests).
std::string BuildDnsQuery(const std::string& host, unsigned short id);
// First A record of a response to the query |id|; follows CNAME chains in
// the answer section implicitly by taking any A record. |ttl| in seconds.
bool ParseDnsResponse(const std::string& packet, unsigned short id, unsigned long& ip, long& ttl,
                      int& rcode);

void ClearDnsCache();

// Tests: replaces the public servers with |server| (dotted IPv4, port 53)
// and delays the system resolver by |systemDelayMs|.
void SetDnsTestHooks(const char* server, int systemDelayMs);

}  // namespace kite

#endif
