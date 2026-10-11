// Kite Engine - host name resolution.
//
// Uses the system resolver first (gethostbyname on Windows 2000, which has
// no getaddrinfo). Results are cached for a few minutes, so a page with many
// resources on the same hosts does not flood the old Windows DNS client.
// When the system resolver fails, the name is looked up directly over UDP
// at public DNS servers; this also covers the Windows 2000 resolver keeping
// a failed lookup in its negative cache.
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

}  // namespace kite

#endif
