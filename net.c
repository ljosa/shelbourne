/*
 * net.c — DNS resolver, NTP time sync, and HTTP client
 */

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ── DNS resolver ──────────────────────────────────────────────── */

int dns_resolve(const char *hostname, struct in_addr *out)
{
    if (inet_aton(hostname, out))
        return 0;

    struct in_addr ns_addr;
    inet_aton("192.168.2.1", &ns_addr);

    FILE *rc = fopen("/etc/resolv.conf", "r");
    if (rc) {
        char line[256];
        while (fgets(line, sizeof(line), rc)) {
            char ip[64];
            if (sscanf(line, "nameserver %63s", ip) == 1) {
                inet_aton(ip, &ns_addr);
                break;
            }
        }
        fclose(rc);
    }

    uint8_t pkt[512];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x12; pkt[1] = 0x34;
    pkt[2] = 0x01; pkt[3] = 0x00;
    pkt[4] = 0x00; pkt[5] = 0x01;
    int off = 12;

    const char *p = hostname;
    while (*p) {
        const char *dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);
        if (off + 1 + len >= (int)sizeof(pkt) - 5) return -1;
        pkt[off++] = len;
        memcpy(pkt + off, p, len);
        off += len;
        p += len + (*p && dot ? 1 : 0);
    }
    pkt[off++] = 0;
    pkt[off++] = 0; pkt[off++] = 1;
    pkt[off++] = 0; pkt[off++] = 1;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct timeval tv = {.tv_sec = 3};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in ns = {0};
    ns.sin_family = AF_INET;
    ns.sin_port = htons(53);
    ns.sin_addr = ns_addr;

    sendto(sock, pkt, off, 0, (struct sockaddr *)&ns, sizeof(ns));

    uint8_t resp[512];
    int rlen = recv(sock, resp, sizeof(resp), 0);
    close(sock);
    if (rlen < 12) return -1;

    if (resp[0] != 0x12 || resp[1] != 0x34) return -1;
    if (!(resp[2] & 0x80)) return -1;
    if ((resp[3] & 0x0F) != 0) return -1;

    int ancount = (resp[6] << 8) | resp[7];
    if (ancount == 0) return -1;

    int roff = 12;
    while (roff < rlen && resp[roff] != 0) {
        if ((resp[roff] & 0xC0) == 0xC0) { roff += 2; break; }
        roff += 1 + resp[roff];
    }
    if (roff < rlen && resp[roff] == 0) roff++;
    roff += 4;

    for (int i = 0; i < ancount && roff < rlen; i++) {
        while (roff < rlen) {
            if ((resp[roff] & 0xC0) == 0xC0) { roff += 2; break; }
            if (resp[roff] == 0) { roff++; break; }
            roff += 1 + resp[roff];
        }
        if (roff + 10 > rlen) return -1;
        uint16_t rtype = (resp[roff] << 8) | resp[roff+1];
        uint16_t rdlen = (resp[roff+8] << 8) | resp[roff+9];
        roff += 10;
        if (rtype == 1 && rdlen == 4) {
            memcpy(out, resp + roff, 4);
            return 0;
        }
        roff += rdlen;
    }
    return -1;
}

/* ── NTP time sync ─────────────────────────────────────────────── */

/*
 * Use the device's ntpd to set the system clock.
 * ntpd -q exits after syncing; -p specifies the server.
 */
#define NTP_SERVER "129.6.15.28"

int ntp_sync(void)
{
    printf("Syncing time via ntpd...\n");
    int rc = system("ntpd -q -p " NTP_SERVER " >/dev/null 2>&1");
    if (rc != 0) {
        fprintf(stderr, "ntpd sync failed (rc=%d)\n", rc);
        return -1;
    }
    time_t now = time(NULL);
    printf("Time synced: %s", ctime(&now));
    return 0;
}

time_t ntp_time(void)
{
    return time(NULL);
}

/* ── HTTP client ───────────────────────────────────────────────── */

int http_get(const char *host, int port, const char *path)
{
    for (int redirects = 0; redirects < 5; redirects++) {
        struct in_addr resolved;
        if (dns_resolve(host, &resolved) < 0) {
            fprintf(stderr, "DNS lookup failed for %s\n", host);
            return -1;
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        struct timeval tv = {.tv_sec = 10};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr = resolved;

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "Connect to %s:%d failed: %s\n",
                    host, port, strerror(errno));
            close(fd);
            return -1;
        }

        char req[1024];
        int len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "User-Agent: ShelbournePlayer/1.0\r\n"
            "Icy-MetaData: 1\r\n"
            "Accept: */*\r\n"
            "\r\n",
            path, host);
        if (send(fd, req, len, 0) != len) { close(fd); return -1; }

        /* Read headers */
        char hdr[4096];
        int hlen = 0;
        while (hlen < (int)sizeof(hdr) - 1) {
            char c;
            if (recv(fd, &c, 1, 0) != 1) break;
            hdr[hlen++] = c;
            if (hlen >= 4 &&
                hdr[hlen-4] == '\r' && hdr[hlen-3] == '\n' &&
                hdr[hlen-2] == '\r' && hdr[hlen-1] == '\n')
                break;
        }
        hdr[hlen] = '\0';

        /* Not a redirect — return the connected socket */
        if (!strstr(hdr, " 301") && !strstr(hdr, " 302"))
            return fd;

        /* Parse redirect Location */
        char *loc = strstr(hdr, "Location: ");
        if (!loc) loc = strstr(hdr, "location: ");
        if (!loc) return fd;  /* no Location header, return as-is */

        loc += 10;
        char *end = strstr(loc, "\r\n");
        if (end) *end = '\0';
        printf("Redirect: %s\n", loc);
        close(fd);

        /* Parse the redirect URL — always use HTTP port 80 */
        static char rhost[256], rpath[512];
        port = 80;
        char *rp = loc;
        if (strncmp(rp, "http://", 7) == 0) rp += 7;
        else if (strncmp(rp, "https://", 8) == 0) rp += 8;

        char *slash = strchr(rp, '/');
        if (slash) {
            strncpy(rpath, slash, sizeof(rpath) - 1);
            rpath[sizeof(rpath) - 1] = '\0';
            int hn = slash - rp;
            if (hn >= (int)sizeof(rhost)) hn = sizeof(rhost) - 1;
            memcpy(rhost, rp, hn);
            rhost[hn] = '\0';
        } else {
            strcpy(rpath, "/");
            strncpy(rhost, rp, sizeof(rhost) - 1);
            rhost[sizeof(rhost) - 1] = '\0';
        }

        char *colon = strchr(rhost, ':');
        if (colon) {
            port = atoi(colon + 1);
            *colon = '\0';
        }

        host = rhost;
        path = rpath;
    }

    fprintf(stderr, "Too many redirects\n");
    return -1;
}
