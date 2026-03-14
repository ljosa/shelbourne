/*
 * airplayd — AirPlay receiver daemon
 *
 * Runs as a separate process from the player. Waits for network,
 * starts an RAOP server + mDNS advertisement, and writes received
 * PCM audio to /tmp/airplay.pcm FIFO for the player to consume.
 * Volume changes are written to /tmp/airplay.vol.
 */

#include "raop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#define LOG_PATH "/tmp/airplayd.log"
static FILE *logfp;
#define LOG(...) do { if (logfp) { fprintf(logfp, __VA_ARGS__); fflush(logfp); } \
                      fprintf(stderr, __VA_ARGS__); } while(0)

static void raop_log_cb(void *cls, int level, const char *msg)
{
    (void)cls;
    LOG("raop[%d]: %s\n", level, msg);
}
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mdns.h"
#include "mdnsd.h"

#define FIFO_PATH "/tmp/airplay.pcm"
#define VOL_PATH  "/tmp/airplay.vol"

/* ── RSA private key (well-known AirPort Express key) ──────────── */

static const char airport_key[] =
"-----BEGIN RSA PRIVATE KEY-----\n"
"MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt\n"
"wC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4U\n"
"wPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf\n"
"/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/\n"
"UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfW\n"
"BLmkzkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14nDY4TFQAa\n"
"LlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPscEsA5ltpxOgUGCY7b7ez5\n"
"NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZuNGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jm\n"
"lpPHr0O/KnPQtzI3eguhe0TwUem/eYSdyzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurciz\n"
"aaA/L0HIgAmOit1GJA2saMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFu\n"
"a39GLS99ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrnjndM\n"
"oPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v//mU8eVkQaoANf0Z\n"
"oMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtkrfa7ef+AUb69DNggq4mHQAYBp7L+\n"
"k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQNepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hL\n"
"AoGBANDrr7xAJbqBjHVwIzQ4To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvA\n"
"cJyRM9SJ7OKlGt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf\n"
"54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TFBVmD7fV0Zhov\n"
"17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLalpGSwomSNYJcB9HNMlmhkGzc\n"
"1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gI\n"
"LAuE4Pu13aKiJnfft7hIjbK+5kyb3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ\n"
"2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n"
"-----END RSA PRIVATE KEY-----\n";

static volatile int quit_flag;
static int fifo_fd = -1;

/* ── RAOP callbacks ────────────────────────────────────────────── */

static void *cb_audio_init(void *cls, int bits, int channels, int samplerate)
{
    (void)cls;
    LOG("airplayd: stream started (%d-bit, %d-ch, %d Hz)\n",
           bits, channels, samplerate);
    return cls;
}

static void cb_audio_process(void *cls, void *session,
                             const void *buffer, int buflen)
{
    (void)cls;
    (void)session;
    if (fifo_fd < 0) return;
    /* Non-blocking write — drop data if pipe full rather than block */
    int written = 0;
    while (written < buflen) {
        int n = write(fifo_fd, (const char *)buffer + written, buflen - written);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;  /* pipe full, drop remaining */
            if (errno == EINTR)
                continue;
            break;
        }
        written += n;
    }
}

static void cb_audio_destroy(void *cls, void *session)
{
    (void)cls;
    (void)session;
    LOG("airplayd: stream ended\n");
}

static void cb_audio_flush(void *cls, void *session)
{
    (void)cls;
    (void)session;
    /* FIFO drains naturally */
}

static void cb_audio_set_volume(void *cls, void *session, float volume)
{
    (void)cls;
    (void)session;
    int vol;
    if (volume <= -144.0f)
        vol = 0;
    else if (volume >= 0.0f)
        vol = 100;
    else {
        vol = (int)((volume + 30.0f) * 100.0f / 30.0f);
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
    }
    /* Write atomically via temp file */
    FILE *f = fopen(VOL_PATH ".tmp", "w");
    if (f) {
        fprintf(f, "%d\n", vol);
        fclose(f);
        rename(VOL_PATH ".tmp", VOL_PATH);
    }
    LOG("airplayd: volume %.1f dB -> %d%%\n", volume, vol);
}

/* ── Network helpers ───────────────────────────────────────────── */

static uint32_t get_local_ip(void)
{
    const char *ifaces[] = { "eth0", "wlan0", NULL };
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    /* Retry indefinitely until network is up or we're told to quit */
    for (int attempt = 0; !quit_flag; attempt++) {
        for (int i = 0; ifaces[i]; i++) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifaces[i], IFNAMSIZ - 1);
            if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
                struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
                uint32_t ip = sin->sin_addr.s_addr;
                if (ip != 0 && ip != htonl(INADDR_LOOPBACK)) {
                    LOG("airplayd: IP = %s (via %s)\n",
                           inet_ntoa(sin->sin_addr), ifaces[i]);
                    close(fd);
                    return ip;
                }
            }
        }
        if (attempt == 0)
            LOG("airplayd: waiting for network...\n");
        sleep(1);
    }
    close(fd);
    return 0;
}

static int get_hwaddr(unsigned char hwaddr[6])
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);
    int ret = ioctl(s, SIOCGIFHWADDR, &ifr);
    if (ret == 0)
        memcpy(hwaddr, ifr.ifr_hwaddr.sa_data, 6);
    close(s);
    return ret;
}

/* ── Signal handler ────────────────────────────────────────────── */

static void sig_handler(int sig)
{
    (void)sig;
    quit_flag = 1;
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "Mr Noisy";

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    logfp = fopen(LOG_PATH, "w");
    /* Redirect stderr to log file so mdnsd thread messages are captured */
    if (logfp) {
        setvbuf(logfp, NULL, _IONBF, 0);  /* unbuffered */
        dup2(fileno(logfp), STDERR_FILENO);
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    LOG("airplayd: starting (name=%s)\n", name);

    /* Create FIFO (don't unlink — player may already have it open) */
    if (mkfifo(FIFO_PATH, 0666) < 0 && errno != EEXIST) {
        perror("airplayd: mkfifo");
        return 1;
    }

    /* Open FIFO O_RDWR to avoid blocking on open and EOF on reader close */
    fifo_fd = open(FIFO_PATH, O_RDWR | O_NONBLOCK);
    if (fifo_fd < 0) {
        perror("airplayd: open fifo");
        return 1;
    }

    /* Wait for network (retries indefinitely) */
    uint32_t ip = get_local_ip();
    if (ip == 0) {
        LOG("airplayd: no network, exiting\n");
        close(fifo_fd);
        return 1;
    }

    /* Get hardware MAC from eth0 */
    unsigned char hwaddr[6] = {0};
    if (get_hwaddr(hwaddr) < 0) {
        LOG("airplayd: could not get MAC address\n");
        close(fifo_fd);
        return 1;
    }
    LOG("airplayd: MAC = %02X:%02X:%02X:%02X:%02X:%02X\n",
           hwaddr[0], hwaddr[1], hwaddr[2],
           hwaddr[3], hwaddr[4], hwaddr[5]);

    /* Initialize RAOP server */
    LOG("airplayd: setting up callbacks\n");
    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.audio_init = cb_audio_init;
    cbs.audio_process = cb_audio_process;
    cbs.audio_destroy = cb_audio_destroy;
    cbs.audio_flush = cb_audio_flush;
    cbs.audio_set_volume = cb_audio_set_volume;

    LOG("airplayd: calling raop_init\n");
    int err = 0;
    raop_t *raop = raop_init(2, &cbs, airport_key, &err);
    LOG("airplayd: raop_init returned %p (err=%d)\n", (void *)raop, err);
    if (!raop) {
        LOG("airplayd: raop_init failed (error %d)\n", err);
        close(fifo_fd);
        return 1;
    }
    raop_set_log_level(raop, RAOP_LOG_DEBUG);
    raop_set_log_callback(raop, raop_log_cb, NULL);
    LOG("airplayd: RAOP initialized\n");

    /* Start RAOP on a dynamic port */
    unsigned short port = 0;
    if (raop_start(raop, &port, (const char *)hwaddr, sizeof(hwaddr), NULL) < 0) {
        LOG("airplayd: raop_start failed\n");
        raop_destroy(raop);
        close(fifo_fd);
        return 1;
    }
    LOG("airplayd: RAOP server on port %d\n", port);

    /* Start mDNS responder.
     * Delay to let the player process finish killing stock Bose processes
     * (especially NetManager), which resets the network interface and would
     * cause our multicast group membership (224.0.0.251) to be dropped. */
    sleep(5);
    LOG("airplayd: starting mdnsd...\n");
    struct mdnsd *mdns = mdnsd_start();
    if (!mdns) {
        LOG("airplayd: mdnsd_start failed\n");
        raop_stop(raop);
        raop_destroy(raop);
        close(fifo_fd);
        return 1;
    }
    LOG("airplayd: mdnsd started\n");

    mdnsd_set_hostname(mdns, "MrNoisy.local", ip);
    LOG("airplayd: hostname set\n");

    /* Get ed25519 public key from RAOP server for pair-verify */
    unsigned char pk_bytes[32];
    raop_get_public_key(raop, pk_bytes);
    char pk_hex[65];
    for (int i = 0; i < 32; i++)
        sprintf(pk_hex + i * 2, "%02x", pk_bytes[i]);

    /* RAOP service: MAC@Name._raop._tcp */
    char svc_name[128];
    snprintf(svc_name, sizeof(svc_name),
             "%02X%02X%02X%02X%02X%02X@%s",
             hwaddr[0], hwaddr[1], hwaddr[2],
             hwaddr[3], hwaddr[4], hwaddr[5], name);

    char pk_txt[72];
    snprintf(pk_txt, sizeof(pk_txt), "pk=%s", pk_hex);

    /* _raop._tcp TXT records — include pk for modern iOS pair-verify.
     * ft= mirrors the features in _airplay._tcp for consistency. */
    char ft_txt[48];
    snprintf(ft_txt, sizeof(ft_txt), "ft=0x5A7FFEE6");

    const char *raop_txt[] = {
        "txtvers=1",
        "ch=2",
        "cn=0,1",       /* PCM, ALAC */
        "et=0,3,5",     /* no encryption, FairPlay, FairPlay SAPv2.5 */
        "ek=1",
        "sv=false",
        "da=true",
        "sr=44100",
        "ss=16",
        "pw=false",
        "vn=65537",
        "tp=UDP",
        "md=0,1,2",
        "vs=220.68",
        "am=AppleTV3,2",
        "sf=0x4",
        ft_txt,
        pk_txt,
        NULL
    };

    LOG("airplayd: registering _raop._tcp (name=%s port=%d)...\n", svc_name, port);
    struct mdns_service *svc_raop = mdnsd_register_svc(
        mdns, svc_name, "_raop._tcp.local", port, NULL, raop_txt);
    LOG("airplayd: _raop._tcp registered (svc=%p)\n", (void *)svc_raop);
    sleep(1);  /* let mdnsd process the first announce before adding the second */

    /* _airplay._tcp for iOS device picker.
     * Features 0x5A7FFEE6 from RPiPlay — proven to work with iOS 15+.
     * Includes: Audio, Photo, Slideshow, Screen, AudioRedundant,
     * metadata, audio formats, RSA auth, HasUnifiedAdvertiserInfo, etc.
     * NO HomeKit bit (46) — that requires HAP which we don't support. */
    char deviceid_str[48], features_str[48];
    snprintf(deviceid_str, sizeof(deviceid_str),
             "deviceid=%02X:%02X:%02X:%02X:%02X:%02X",
             hwaddr[0], hwaddr[1], hwaddr[2],
             hwaddr[3], hwaddr[4], hwaddr[5]);
    snprintf(features_str, sizeof(features_str),
             "features=0x5A7FFEE6");

    const char *airplay_txt[] = {
        deviceid_str,
        features_str,
        "pw=false",
        "model=AppleTV3,2",
        "srcvers=220.68",
        "pi=2e388006-13ba-4041-9a67-25dd4a43d536",
        "vv=2",
        "flags=0x4",
        pk_txt,
        NULL
    };

    LOG("airplayd: registering _airplay._tcp (name=%s)...\n", name);
    struct mdns_service *svc_airplay = mdnsd_register_svc(
        mdns, name, "_airplay._tcp.local", port, NULL, airplay_txt);
    LOG("airplayd: _airplay._tcp registered (svc=%p)\n", (void *)svc_airplay);

    struct in_addr addr;
    addr.s_addr = ip;
    LOG("airplayd: advertising \"%s\" at %s:%d\n",
           svc_name, inet_ntoa(addr), port);

    /* Main loop — just wait for signal */
    while (!quit_flag)
        sleep(1);

    LOG("airplayd: shutting down\n");

    /* Cleanup */
    mdns_service_destroy(svc_airplay);
    mdns_service_destroy(svc_raop);
    mdnsd_stop(mdns);
    raop_stop(raop);
    raop_destroy(raop);
    close(fifo_fd);
    unlink(FIFO_PATH);
    unlink(VOL_PATH);

    return 0;
}
