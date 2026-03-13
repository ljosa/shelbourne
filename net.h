#ifndef NET_H
#define NET_H

#include <netinet/in.h>

/* DNS resolver — returns 0 on success, -1 on failure */
int dns_resolve(const char *hostname, struct in_addr *out);

/*
 * NTP time sync — query NIST time server and compute the offset
 * between system monotonic clock and real UTC time.
 *
 * Call ntp_sync() once at startup (after WiFi is up).  Then use
 * ntp_time() instead of time() for wall-clock time.
 *
 * Returns 0 on success, -1 on failure.
 */
int ntp_sync(void);
time_t ntp_time(void);

/*
 * HTTP GET helper — connect to host:port, send GET path, read and
 * skip HTTP headers.  Returns connected socket positioned at the
 * start of the response body, or -1 on error.
 *
 * Follows up to 5 redirects.  If a redirect target uses https://,
 * the scheme is silently downgraded to http:// (works for CDNs
 * that serve on both).
 */
int http_get(const char *host, int port, const char *path);

#endif /* NET_H */
