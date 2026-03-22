/*
 * mqtt.c — minimal MQTT 3.1.1 client (connect + subscribe + receive)
 *
 * Runs a background thread that maintains a persistent connection
 * to the broker, reconnecting on failure.  Delivers PUBLISH payloads
 * via a user-supplied callback.
 */

#include "mqtt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* MQTT packet types (high nibble of first byte) */
#define MQTT_CONNECT     0x10
#define MQTT_CONNACK     0x20
#define MQTT_SUBSCRIBE   0x82  /* with QoS flags */
#define MQTT_SUBACK      0x90
#define MQTT_PUBLISH     0x30
#define MQTT_PINGREQ     0xC0
#define MQTT_PINGRESP    0xD0

#define KEEPALIVE_SEC    60
#define RECV_TIMEOUT_SEC 90  /* must be > keepalive */
#define RECONNECT_SEC    5

static pthread_t mqtt_thread;
static volatile int mqtt_running;
static volatile int mqtt_sock = -1;  /* current socket, for shutdown */

static char mqtt_broker[64];
static int  mqtt_port;
static char mqtt_topic[128];
static void (*mqtt_callback)(const char *payload, int len);

/* ── Packet encoding helpers ────────────────────────────────── */

static int encode_remaining_length(uint8_t *buf, int len)
{
    int pos = 0;
    do {
        uint8_t byte = len % 128;
        len /= 128;
        if (len > 0) byte |= 0x80;
        buf[pos++] = byte;
    } while (len > 0);
    return pos;
}

static int decode_remaining_length(int fd, int *length)
{
    int multiplier = 1;
    int value = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t byte;
        int n = recv(fd, &byte, 1, 0);
        if (n != 1) return -1;
        value += (byte & 0x7F) * multiplier;
        if (!(byte & 0x80)) {
            *length = value;
            return 0;
        }
        multiplier *= 128;
    }
    return -1;
}

static int send_all(int fd, const uint8_t *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int recv_exact(int fd, uint8_t *buf, int len)
{
    int got = 0;
    while (got < len) {
        int n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

/* ── CONNECT ────────────────────────────────────────────────── */

static int mqtt_send_connect(int fd)
{
    /*
     * CONNECT packet (MQTT 3.1.1):
     *   Fixed header: type=1, remaining length
     *   Variable header: protocol name "MQTT", level 4, flags, keepalive
     *   Payload: client ID
     */
    const char *client_id = "shelbourne";
    int client_id_len = strlen(client_id);

    int var_len = 2 + 4 + 1 + 1 + 2 + 2 + client_id_len;
    /* protocol name length (2) + "MQTT" (4) + level (1) + flags (1)
       + keepalive (2) + client ID length (2) + client ID */

    uint8_t pkt[64];
    int pos = 0;

    pkt[pos++] = MQTT_CONNECT;
    pos += encode_remaining_length(pkt + pos, var_len);

    /* Protocol name */
    pkt[pos++] = 0; pkt[pos++] = 4;
    pkt[pos++] = 'M'; pkt[pos++] = 'Q'; pkt[pos++] = 'T'; pkt[pos++] = 'T';

    /* Protocol level 4 (MQTT 3.1.1) */
    pkt[pos++] = 4;

    /* Connect flags: clean session */
    pkt[pos++] = 0x02;

    /* Keep alive */
    pkt[pos++] = (KEEPALIVE_SEC >> 8) & 0xFF;
    pkt[pos++] = KEEPALIVE_SEC & 0xFF;

    /* Client ID */
    pkt[pos++] = (client_id_len >> 8) & 0xFF;
    pkt[pos++] = client_id_len & 0xFF;
    memcpy(pkt + pos, client_id, client_id_len);
    pos += client_id_len;

    return send_all(fd, pkt, pos);
}

static int mqtt_recv_connack(int fd)
{
    uint8_t byte;
    if (recv_exact(fd, &byte, 1) < 0) return -1;
    if ((byte & 0xF0) != MQTT_CONNACK) return -1;

    int rem_len;
    if (decode_remaining_length(fd, &rem_len) < 0) return -1;
    if (rem_len != 2) return -1;

    uint8_t ack[2];
    if (recv_exact(fd, ack, 2) < 0) return -1;
    if (ack[1] != 0) {
        fprintf(stderr, "MQTT CONNACK error: %d\n", ack[1]);
        return -1;
    }
    return 0;
}

/* ── SUBSCRIBE ──────────────────────────────────────────────── */

static int mqtt_send_subscribe(int fd, const char *topic)
{
    int topic_len = strlen(topic);
    int var_len = 2 + 2 + topic_len + 1;
    /* packet ID (2) + topic length (2) + topic + QoS (1) */

    uint8_t pkt[256];
    int pos = 0;

    pkt[pos++] = MQTT_SUBSCRIBE;
    pos += encode_remaining_length(pkt + pos, var_len);

    /* Packet identifier */
    pkt[pos++] = 0; pkt[pos++] = 1;

    /* Topic filter + QoS 0 */
    pkt[pos++] = (topic_len >> 8) & 0xFF;
    pkt[pos++] = topic_len & 0xFF;
    memcpy(pkt + pos, topic, topic_len);
    pos += topic_len;
    pkt[pos++] = 0;  /* QoS 0 */

    return send_all(fd, pkt, pos);
}

static int mqtt_recv_suback(int fd)
{
    uint8_t byte;
    if (recv_exact(fd, &byte, 1) < 0) return -1;
    if ((byte & 0xF0) != MQTT_SUBACK) return -1;

    int rem_len;
    if (decode_remaining_length(fd, &rem_len) < 0) return -1;

    /* Skip remaining bytes (packet ID + return codes) */
    uint8_t buf[16];
    if (rem_len > (int)sizeof(buf)) return -1;
    if (recv_exact(fd, buf, rem_len) < 0) return -1;

    /* Check return code (last byte): 0x80 = failure */
    if (rem_len >= 3 && buf[rem_len - 1] == 0x80) {
        fprintf(stderr, "MQTT subscribe rejected\n");
        return -1;
    }
    return 0;
}

/* ── PINGREQ ────────────────────────────────────────────────── */

static int mqtt_send_pingreq(int fd)
{
    uint8_t pkt[2] = { MQTT_PINGREQ, 0 };
    return send_all(fd, pkt, 2);
}

/* ── Receive loop ───────────────────────────────────────────── */

static int mqtt_receive_loop(int fd)
{
    time_t last_ping = time(NULL);

    while (mqtt_running) {
        /* Send PINGREQ if needed */
        time_t now = time(NULL);
        if (now - last_ping >= KEEPALIVE_SEC / 2) {
            if (mqtt_send_pingreq(fd) < 0) return -1;
            last_ping = now;
        }

        /* Read first byte (packet type) */
        uint8_t type_byte;
        int n = recv(fd, &type_byte, 1, 0);
        if (n == 0) return -1;  /* connection closed */
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Timeout — check if we should ping or reconnect */
                continue;
            }
            return -1;
        }

        int rem_len;
        if (decode_remaining_length(fd, &rem_len) < 0) return -1;

        uint8_t type = type_byte & 0xF0;

        if (type == MQTT_PUBLISH) {
            if (rem_len > 8192 || rem_len < 2) {
                /* Skip oversized or empty publish */
                uint8_t skip[256];
                while (rem_len > 0) {
                    int chunk = rem_len < (int)sizeof(skip) ?
                                rem_len : (int)sizeof(skip);
                    if (recv_exact(fd, skip, chunk) < 0) return -1;
                    rem_len -= chunk;
                }
                continue;
            }

            uint8_t *data = malloc(rem_len);
            if (!data) return -1;
            if (recv_exact(fd, data, rem_len) < 0) {
                free(data);
                return -1;
            }

            /* Parse topic length */
            int topic_len = (data[0] << 8) | data[1];
            int payload_off = 2 + topic_len;

            /* QoS > 0 has a packet ID after topic */
            if (type_byte & 0x06)
                payload_off += 2;

            if (payload_off < rem_len) {
                int payload_len = rem_len - payload_off;
                /* Null-terminate for convenience */
                char *payload = malloc(payload_len + 1);
                if (payload) {
                    memcpy(payload, data + payload_off, payload_len);
                    payload[payload_len] = '\0';
                    if (mqtt_callback)
                        mqtt_callback(payload, payload_len);
                    free(payload);
                }
            }
            free(data);

        } else if (type == MQTT_PINGRESP) {
            /* Nothing to do, just keeps the connection alive */
        } else {
            /* Skip unknown packet */
            uint8_t skip[256];
            while (rem_len > 0) {
                int chunk = rem_len < (int)sizeof(skip) ?
                            rem_len : (int)sizeof(skip);
                if (recv_exact(fd, skip, chunk) < 0) return -1;
                rem_len -= chunk;
            }
        }
    }
    return 0;
}

/* ── Background thread ──────────────────────────────────────── */

static void *mqtt_thread_fn(void *arg)
{
    (void)arg;

    while (mqtt_running) {
        struct in_addr addr;
        if (!inet_aton(mqtt_broker, &addr)) {
            fprintf(stderr, "MQTT: invalid broker IP %s\n", mqtt_broker);
            sleep(RECONNECT_SEC);
            continue;
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("MQTT socket");
            sleep(RECONNECT_SEC);
            continue;
        }

        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(mqtt_port);
        sa.sin_addr = addr;

        struct timeval tv = { .tv_sec = RECV_TIMEOUT_SEC };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            fprintf(stderr, "MQTT: connect to %s:%d failed: %s\n",
                    mqtt_broker, mqtt_port, strerror(errno));
            close(fd);
            sleep(RECONNECT_SEC);
            continue;
        }

        mqtt_sock = fd;
        printf("MQTT: connected to %s:%d\n", mqtt_broker, mqtt_port);

        if (mqtt_send_connect(fd) < 0 || mqtt_recv_connack(fd) < 0) {
            fprintf(stderr, "MQTT: handshake failed\n");
            close(fd);
            sleep(RECONNECT_SEC);
            continue;
        }

        if (mqtt_send_subscribe(fd, mqtt_topic) < 0 ||
            mqtt_recv_suback(fd) < 0) {
            fprintf(stderr, "MQTT: subscribe failed\n");
            close(fd);
            sleep(RECONNECT_SEC);
            continue;
        }

        printf("MQTT: subscribed to %s\n", mqtt_topic);

        mqtt_receive_loop(fd);

        mqtt_sock = -1;
        close(fd);
        if (mqtt_running) {
            fprintf(stderr, "MQTT: disconnected, reconnecting in %ds\n",
                    RECONNECT_SEC);
            sleep(RECONNECT_SEC);
        }
    }

    return NULL;
}

/* ── Public API ─────────────────────────────────────────────── */

int mqtt_start(const char *broker_ip, int port,
               const char *topic,
               void (*callback)(const char *payload, int len))
{
    strncpy(mqtt_broker, broker_ip, sizeof(mqtt_broker) - 1);
    mqtt_port = port;
    strncpy(mqtt_topic, topic, sizeof(mqtt_topic) - 1);
    mqtt_callback = callback;
    mqtt_running = 1;

    if (pthread_create(&mqtt_thread, NULL, mqtt_thread_fn, NULL) != 0) {
        perror("MQTT thread create");
        return -1;
    }

    return 0;
}

void mqtt_stop(void)
{
    mqtt_running = 0;
    /* Unblock recv() so the thread exits promptly */
    int fd = mqtt_sock;
    if (fd >= 0)
        shutdown(fd, SHUT_RDWR);
    pthread_join(mqtt_thread, NULL);
}
