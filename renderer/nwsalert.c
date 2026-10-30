/*
 * nwsalert.c - US National Weather Service active alerts fetcher
 *
 * See nwsalert.h. Alert GeoJSON (abridged):
 *
 *   { "features": [ {
 *       "geometry": { "type": "Polygon",
 *                     "coordinates": [[[lon,lat], ...]] } | null,
 *       "properties": {
 *         "event": "Tornado Warning",
 *         "severity": "Extreme",
 *         "expires": "2026-10-30T15:45:00-04:00",
 *         "ends":    "2026-10-30T15:45:00-04:00" | null,
 *         ... } } ] }
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE   /* timegm */
#include "nwsalert.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include "math_utils.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NWS_MAP_URL \
    "https://api.weather.gov/alerts/active?status=actual&severity=Extreme,Severe"
#define NWS_POINT_FMT \
    "https://api.weather.gov/alerts/active?status=actual&point=%.4f,%.4f"

/* Short-fuse warnings change quickly; the NWS API caches for ~30 s,
 * so polling every 3 minutes is responsive without being rude. */
#define FETCH_INTERVAL (3 * 60)
#define MAX_RESPONSE   (6 * 1024 * 1024)

static pthread_t nws_thread;
static volatile int nws_running = 0;
static pic_nws_t *cfg_data = NULL;

void pic_nws_init(pic_nws_t *data)
{
    memset(data, 0, sizeof(*data));
    data->show_banner = 1;
    data->banner_min_sev = NWS_SEV_MODERATE;
    pthread_mutex_init(&data->mutex, NULL);
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static int severity_from(const char *s)
{
    if (strcmp(s, "Extreme") == 0)  return NWS_SEV_EXTREME;
    if (strcmp(s, "Severe") == 0)   return NWS_SEV_SEVERE;
    if (strcmp(s, "Moderate") == 0) return NWS_SEV_MODERATE;
    if (strcmp(s, "Minor") == 0)    return NWS_SEV_MINOR;
    return NWS_SEV_UNKNOWN;
}

/* NWS hazard map colours for the events most likely to appear. */
static const struct { const char *event; unsigned rgb; } event_colours[] = {
    { "Tornado Warning",                0xFF0000 },
    { "Extreme Wind Warning",           0xFF8C00 },
    { "Severe Thunderstorm Warning",    0xFFA500 },
    { "Flash Flood Warning",            0x8B0000 },
    { "Flash Flood Statement",          0x8B0000 },
    { "Flood Warning",                  0x00FF00 },
    { "Special Marine Warning",         0xFFA500 },
    { "Hurricane Warning",              0xDC143C },
    { "Hurricane Watch",                0xFF00FF },
    { "Tropical Storm Warning",         0xB22222 },
    { "Tropical Storm Watch",           0xF08080 },
    { "Storm Surge Warning",            0xB524F7 },
    { "Storm Surge Watch",              0xDB7FF7 },
    { "Tornado Watch",                  0xFFFF00 },
    { "Severe Thunderstorm Watch",      0xDB7093 },
    { "Flash Flood Watch",              0x2E8B57 },
    { "Winter Storm Warning",           0xFF69B4 },
    { "Blizzard Warning",               0xFF4500 },
    { "Ice Storm Warning",              0x8B008B },
    { "High Wind Warning",              0xDAA520 },
    { "Excessive Heat Warning",         0xC71585 },
    { "Extreme Heat Warning",           0xC71585 },
    { "Red Flag Warning",               0xFF1493 },
    { "Dense Fog Advisory",             0x708090 },
    { "Heat Advisory",                  0xFF7F50 },
    { "Wind Advisory",                  0xD2B48C },
    { "Winter Weather Advisory",        0x7B68EE },
    { "Special Weather Statement",      0xFFE4B5 },
    { NULL, 0 }
};

void pic_nws_event_colour(const char *event, int severity,
                          float *r, float *g, float *b)
{
    unsigned rgb;
    int i;

    for (i = 0; event_colours[i].event; i++) {
        if (strcmp(event, event_colours[i].event) == 0) {
            rgb = event_colours[i].rgb;
            goto found;
        }
    }
    /* Unknown event: colour by severity */
    rgb = severity >= NWS_SEV_EXTREME ? 0xFF0000 :
          severity >= NWS_SEV_SEVERE  ? 0xFF8C00 :
          severity >= NWS_SEV_MODERATE ? 0xFFD700 : 0x87CEEB;
found:
    *r = (float)((rgb >> 16) & 0xFF) / 255.0f;
    *g = (float)((rgb >> 8) & 0xFF) / 255.0f;
    *b = (float)(rgb & 0xFF) / 255.0f;
}

time_t pic_nws_parse_time(const char *s)
{
    struct tm tm;
    int Y, M, D, h, m, sec, oh = 0, om = 0;
    char sign = '+';
    const char *tz;
    time_t t;

    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6)
        return 0;

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = sec;
    t = timegm(&tm);

    /* Offset follows the seconds (skip any fractional part) */
    tz = s + 19;
    while (*tz == '.' || (*tz >= '0' && *tz <= '9')) tz++;
    if (*tz == '+' || *tz == '-') {
        sign = *tz;
        if (sscanf(tz + 1, "%2d:%2d", &oh, &om) >= 1) {
            long off = oh * 3600L + om * 60L;
            t += (sign == '+') ? -off : off;
        }
    }
    return t;
}

static double gc_km(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * DEG_TO_RAD, p2 = lat2 * DEG_TO_RAD;
    double dp = p2 - p1, dl = (lon2 - lon1) * DEG_TO_RAD;
    double a = sin(dp / 2) * sin(dp / 2) +
               cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

/* Fill the common fields of an alert from a feature's properties. */
static void read_props(const pic_json_t *j, int props, pic_nws_alert_t *a)
{
    char sev[16], t[40];

    memset(a, 0, sizeof(*a));
    pic_json_str(j, pic_json_get(j, props, "event"), a->event, sizeof(a->event));
    pic_json_str(j, pic_json_get(j, props, "severity"), sev, sizeof(sev));
    a->severity = severity_from(sev);

    /* "ends" is the end of the hazard; "expires" is when this message
     * lapses. Prefer ends when present. */
    if (pic_json_str(j, pic_json_get(j, props, "ends"), t, sizeof(t)) <= 0 ||
        strcmp(t, "null") == 0)
        pic_json_str(j, pic_json_get(j, props, "expires"), t, sizeof(t));
    a->expires = pic_nws_parse_time(t);

    pic_nws_event_colour(a->event, a->severity, &a->r, &a->g, &a->b);
}

/* ── Parsers ─────────────────────────────────────────────────────── */

int pic_nws_parse_map(const char *js, size_t len, pic_nws_t *out,
                      double qth_lat, double qth_lon, int radius_km)
{
    pic_json_t j;
    int feats, i, n;

    out->nmap = 0;
    out->nvert = 0;
    if (pic_json_parse(&j, js, len) != 0) return 0;

    feats = pic_json_get(&j, 0, "features");
    n = pic_json_size(&j, feats);

    for (i = 0; i < n && out->nmap < NWS_MAX_MAP_ALERTS; i++) {
        int f = pic_json_at(&j, feats, i);
        int geom = pic_json_get(&j, f, "geometry");
        int coords = pic_json_get(&j, geom, "coordinates");
        int npoly, p, is_multi;

        if (!pic_json_is_array(&j, coords)) continue;  /* zone-based */

        /* Polygon: [ring...]; MultiPolygon: [[ring...]...] */
        is_multi = pic_json_eq(&j, pic_json_get(&j, geom, "type"),
                               "MultiPolygon");
        npoly = is_multi ? pic_json_size(&j, coords) : 1;

        for (p = 0; p < npoly && out->nmap < NWS_MAX_MAP_ALERTS; p++) {
            int poly = is_multi ? pic_json_at(&j, coords, p) : coords;
            int ring = pic_json_at(&j, poly, 0);
            int rn = pic_json_size(&j, ring), k;
            pic_nws_alert_t *a = &out->map[out->nmap];
            double clat = 0, clon = 0;

            if (rn < 3 || out->nvert + rn > NWS_MAX_VERTICES) continue;

            read_props(&j, pic_json_get(&j, f, "properties"), a);
            a->first_vertex = out->nvert;
            for (k = 0; k < rn; k++) {
                int c = pic_json_at(&j, ring, k);
                float lon = (float)pic_json_num(&j, pic_json_at(&j, c, 0), 0);
                float lat = (float)pic_json_num(&j, pic_json_at(&j, c, 1), 0);
                out->vlon[out->nvert + k] = lon;
                out->vlat[out->nvert + k] = lat;
                clat += lat; clon += lon;
            }

            if (radius_km > 0 &&
                gc_km(qth_lat, qth_lon, clat / rn, clon / rn) > radius_km)
                continue;   /* outside the area of interest; not kept */

            a->nvertex = rn;
            out->nvert += rn;
            out->nmap++;
        }
    }

    pic_json_free(&j);
    return out->nmap;
}

int pic_nws_parse_home(const char *js, size_t len,
                       pic_nws_alert_t *out, int max)
{
    pic_json_t j;
    int feats, i, n, count = 0;

    if (pic_json_parse(&j, js, len) != 0) return 0;

    feats = pic_json_get(&j, 0, "features");
    n = pic_json_size(&j, feats);

    for (i = 0; i < n; i++) {
        int f = pic_json_at(&j, feats, i);
        pic_nws_alert_t a;
        int k, dup = 0;

        read_props(&j, pic_json_get(&j, f, "properties"), &a);
        if (!a.event[0]) continue;

        /* Several messages (issuances/updates) can describe one hazard;
         * keep one entry per event name, with the latest end time. */
        for (k = 0; k < count; k++) {
            if (strcmp(out[k].event, a.event) == 0) {
                if (a.expires > out[k].expires) out[k].expires = a.expires;
                dup = 1;
                break;
            }
        }
        if (dup) continue;

        /* Insert sorted by severity (desc); drop the least severe if full */
        if (count == max) {
            if (a.severity <= out[count - 1].severity) continue;
            count--;
        }
        for (k = count; k > 0 && out[k - 1].severity < a.severity; k--)
            out[k] = out[k - 1];
        out[k] = a;
        count++;
    }

    pic_json_free(&j);
    return count;
}

/* ── Thread ──────────────────────────────────────────────────────── */

static void *nws_thread_func(void *arg)
{
    char *buf;
    static pic_nws_t temp;
    (void)arg;

    buf = malloc(MAX_RESPONSE);
    if (!buf) {
        fprintf(stderr, "nwsalert: failed to allocate fetch buffer\n");
        return NULL;
    }

    if (!pic_wait_for_network("nwsalert", &nws_running)) {
        free(buf);
        return NULL;
    }

    while (nws_running) {
        char url[160];
        int n, radius, i, map_ok = 0, home_ok = 0;
        double qlat = pic_config.qth_lat, qlon = pic_config.qth_lon;

        pthread_mutex_lock(&cfg_data->mutex);
        radius = cfg_data->radius_km;
        pthread_mutex_unlock(&cfg_data->mutex);

        n = pic_fetch_to_buf(NWS_MAP_URL, buf, MAX_RESPONSE - 1);
        if (n > 0) {
            buf[n] = '\0';
            pic_nws_parse_map(buf, (size_t)n, &temp, qlat, qlon, radius);
            map_ok = 1;
        }

        temp.nhome = 0;
        if (qlat != 0.0 || qlon != 0.0) {
            snprintf(url, sizeof(url), NWS_POINT_FMT, qlat, qlon);
            n = pic_fetch_to_buf(url, buf, MAX_RESPONSE - 1);
            if (n > 0) {
                buf[n] = '\0';
                temp.nhome = pic_nws_parse_home(buf, (size_t)n, temp.home,
                                                NWS_MAX_HOME_ALERTS);
                home_ok = 1;
            }
        }

        pthread_mutex_lock(&cfg_data->mutex);
        if (map_ok) {
            memcpy(cfg_data->map, temp.map, (size_t)temp.nmap * sizeof(temp.map[0]));
            memcpy(cfg_data->vlat, temp.vlat, (size_t)temp.nvert * sizeof(float));
            memcpy(cfg_data->vlon, temp.vlon, (size_t)temp.nvert * sizeof(float));
            cfg_data->nmap = temp.nmap;
            cfg_data->nvert = temp.nvert;
        }
        if (home_ok) {
            memcpy(cfg_data->home, temp.home, sizeof(temp.home));
            cfg_data->nhome = temp.nhome;
        }
        if (map_ok || home_ok) cfg_data->last_fetched = time(NULL);
        pthread_mutex_unlock(&cfg_data->mutex);

        printf("nwsalert: %d map polygons, %d alerts at QTH%s\n",
               temp.nmap, temp.nhome,
               (map_ok && home_ok) ? "" : " (partial fetch failure)");

        for (i = 0; i < FETCH_INTERVAL && nws_running; i++)
            sleep(1);
    }

    free(buf);
    return NULL;
}

int pic_nws_start(pic_nws_t *data)
{
    if (nws_running) return 0;
    cfg_data = data;
    pic_nws_reload(data);
    nws_running = 1;
    if (pthread_create(&nws_thread, NULL, nws_thread_func, NULL) != 0) {
        nws_running = 0;
        fprintf(stderr, "nwsalert: failed to create thread\n");
        return -1;
    }
    printf("nwsalert: fetcher thread started\n");
    return 0;
}

void pic_nws_stop(void)
{
    if (!nws_running) return;
    nws_running = 0;
    pthread_join(nws_thread, NULL);
    cfg_data = NULL;
    printf("nwsalert: fetcher thread stopped\n");
}

void pic_nws_reload(pic_nws_t *data)
{
    FILE *f;
    char line[256];
    int banner = 1, min_sev = NWS_SEV_MODERATE, radius = 0;

    f = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "WX_BANNER=", 10) == 0)
                banner = atoi(line + 10) != 0;
            else if (strncmp(line, "WX_BANNER_MIN=", 14) == 0)
                min_sev = atoi(line + 14);
            else if (strncmp(line, "WX_RADIUS_KM=", 13) == 0)
                radius = atoi(line + 13);
        }
        fclose(f);
    }
    if (min_sev < NWS_SEV_MINOR) min_sev = NWS_SEV_MINOR;
    if (min_sev > NWS_SEV_EXTREME) min_sev = NWS_SEV_EXTREME;
    if (radius < 0) radius = 0;
    if (radius > 5000) radius = 5000;

    pthread_mutex_lock(&data->mutex);
    data->show_banner = banner;
    data->banner_min_sev = min_sev;
    data->radius_km = radius;
    pthread_mutex_unlock(&data->mutex);
}
