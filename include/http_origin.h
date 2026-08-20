#pragma once

#include <stddef.h>
#include <string.h>

// The string half of the cross-origin guard, kept free of Arduino headers so `make test`
// can reach it.
//
// **Origin alone proves nothing.** Both Origin and Host come from the browser, so comparing
// them to each other compares two values an attacker controls. A page on the public
// internet whose DNS answer is re-pointed at this device (DNS rebinding) sends a matching
// pair for its own name, and the unauthenticated command endpoint is behind that guard.
// What has to be checked first is that the *target* is a name this device answers to.
namespace http {

inline char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// The authority, stopping at a port or a path.
inline size_t authorityLen(const char *s) {
  size_t n = 0;
  while (s[n] != '\0' && s[n] != ':' && s[n] != '/') {
    n++;
  }
  return n;
}

inline bool equalsIgnoreCase(const char *a, size_t aLen, const char *b) {
  if (b == nullptr) {
    return false;
  }
  size_t i = 0;
  for (; i < aLen && b[i] != '\0'; i++) {
    if (lowerAscii(a[i]) != lowerAscii(b[i])) {
      return false;
    }
  }
  return i == aLen && b[i] == '\0';
}

// Hostnames are case-insensitive; a port is not part of the name. Three ways this device is
// legitimately reached: its mDNS name, that name with .local, and its address.
inline bool hostIsOurs(const char *host, const char *hostname, const char *ip) {
  if (host == nullptr || hostname == nullptr || host[0] == '\0') {
    return false;
  }
  const size_t n = authorityLen(host);
  if (n == 0) {
    return false;
  }
  if (equalsIgnoreCase(host, n, hostname)) {
    return true;
  }
  if (equalsIgnoreCase(host, n, ip)) {
    return true;
  }
  const size_t hn = strlen(hostname);
  return n == hn + 6 && equalsIgnoreCase(host, hn, hostname) &&
         equalsIgnoreCase(host + hn, 6, ".local");
}

// Unchanged in meaning from what this replaced: the Origin's authority must be the Host.
// It is the second check now, not the only one.
inline bool originMatchesHost(const char *origin, const char *host) {
  if (origin == nullptr || host == nullptr) {
    return false;
  }
  const char *slashes = strstr(origin, "//");
  const char *authority = slashes != nullptr ? slashes + 2 : origin;
  return strcmp(authority, host) == 0;
}

}   // namespace http
