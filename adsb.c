/*
 * adsb.c — ADS-B aircraft lookup via api.adsb.lol
 *
 * Uses the existing DNS resolver and a simple HTTP GET to fetch
 * nearby aircraft data in JSON, then parses it with minimal string
 * operations (no JSON library needed for this flat structure).
 */

#include "adsb.h"
#include "net.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Minimal JSON helpers ────────────────────────────────────── */

/* Find "key": in a JSON string, return pointer after the colon. */
static const char *json_find(const char *json, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    return p + strlen(pattern);
}

/* Extract a JSON string value into buf. Returns length, or -1. */
static int json_string(const char *p, char *buf, int buflen)
{
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < buflen - 1)
        buf[i++] = *p++;
    buf[i] = '\0';
    /* Trim trailing spaces */
    while (i > 0 && buf[i - 1] == ' ')
        buf[--i] = '\0';
    return i;
}

/* Extract a JSON number as double. Returns 1 on success. */
static int json_number(const char *p, double *out)
{
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') return 0;  /* "ground" etc */
    char *end;
    *out = strtod(p, &end);
    return (end != p) ? 1 : 0;
}

/* ── API query ───────────────────────────────────────────────── */

int adsb_query(double lat, double lon, int max_nm, adsb_result_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Build the API path */
    char path[128];
    snprintf(path, sizeof(path),
             "/v2/lat/%.4f/lon/%.4f/dist/%d", lat, lon, max_nm);

    /* Connect and GET */
    int fd = http_get("api.adsb.lol", 80, path);
    if (fd < 0) {
        fprintf(stderr, "ADSB: HTTP connection failed\n");
        return -1;
    }

    /* Read response body (up to 64KB) */
    char *body = malloc(64 * 1024);
    if (!body) { close(fd); return -1; }

    int body_len = 0;
    while (body_len < 64 * 1024 - 1) {
        int n = recv(fd, body + body_len, 64 * 1024 - 1 - body_len, 0);
        if (n <= 0) break;
        body_len += n;
    }
    body[body_len] = '\0';
    close(fd);

    if (body_len < 10) {
        fprintf(stderr, "ADSB: empty response\n");
        free(body);
        return -1;
    }

    /* Parse aircraft from the "ac" array.
     * We look for each object by finding "hex": which starts each entry. */
    const char *p = body;
    int count = 0;
    double nearest_dst = 9999;

    while (count < ADSB_MAX_AIRCRAFT) {
        /* Find next aircraft object by looking for "hex": */
        p = strstr(p, "\"hex\":");
        if (!p) break;

        /* Find the end of this object (next "hex": or end of array) */
        const char *next = strstr(p + 6, "\"hex\":");
        int obj_len;
        if (next)
            obj_len = next - p;
        else
            obj_len = body + body_len - p;

        /* Work within this object */
        char obj[4096];
        if (obj_len >= (int)sizeof(obj)) obj_len = sizeof(obj) - 1;
        memcpy(obj, p, obj_len);
        obj[obj_len] = '\0';

        adsb_aircraft_t *ac = &out->ac[count];
        ac->alt = -1;
        ac->gs = -1;
        ac->track = -1;
        ac->dst_nm_x10 = 0;
        ac->dir = -1;

        /* Callsign */
        const char *v = json_find(obj, "flight");
        if (v) json_string(v, ac->callsign, sizeof(ac->callsign));
        if (!ac->callsign[0]) strcpy(ac->callsign, "?");

        /* Type */
        v = json_find(obj, "t");
        if (v) json_string(v, ac->type, sizeof(ac->type));

        /* Altitude */
        v = json_find(obj, "alt_baro");
        if (v) {
            double d;
            if (json_number(v, &d))
                ac->alt = (int)d;
            else if (strstr(v, "ground"))
                ac->alt = 0;
        }

        /* Ground speed */
        v = json_find(obj, "gs");
        if (v) {
            double d;
            if (json_number(v, &d))
                ac->gs = (int)d;
        }

        /* Track (heading) */
        v = json_find(obj, "track");
        if (v) {
            double d;
            if (json_number(v, &d))
                ac->track = (int)d;
        }

        /* Distance */
        v = json_find(obj, "dst");
        if (v) {
            double d;
            if (json_number(v, &d)) {
                ac->dst_nm_x10 = (int)(d * 10 + 0.5);
                if (d < nearest_dst)
                    nearest_dst = d;
            }
        }

        /* Bearing */
        v = json_find(obj, "dir");
        if (v) {
            double d;
            if (json_number(v, &d))
                ac->dir = (int)d;
        }

        /* Only include aircraft with valid position */
        if (ac->dst_nm_x10 > 0 && ac->dir >= 0)
            count++;

        p += 6;  /* advance past this "hex": */
    }

    free(body);

    /* Sort by distance (simple insertion sort) */
    for (int i = 1; i < count; i++) {
        adsb_aircraft_t tmp = out->ac[i];
        int j = i;
        while (j > 0 && out->ac[j - 1].dst_nm_x10 > tmp.dst_nm_x10) {
            out->ac[j] = out->ac[j - 1];
            j--;
        }
        out->ac[j] = tmp;
    }

    out->count = count;

    /* Choose display radius: 5nm increments */
    if (count == 0) {
        out->radius_nm = 25;
    } else {
        double nd = out->ac[0].dst_nm_x10 / 10.0;
        if (nd <= 5.0)
            out->radius_nm = 5;
        else {
            out->radius_nm = ((int)ceil(nd / 5.0)) * 5;
            if (out->radius_nm > 25) out->radius_nm = 25;
        }
    }

    printf("ADSB: %d aircraft, nearest %.1f nm, radius %d nm\n",
           count, count ? out->ac[0].dst_nm_x10 / 10.0 : 0.0,
           out->radius_nm);

    return 0;
}
