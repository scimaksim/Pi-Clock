/*
 * aircraft.c - ADS-B aircraft fetcher (adsb.lol or a local receiver)
 *
 * adsb.lol v2 response (abridged):
 *   { "ac": [ { "hex": "a11ad4", "flight": "SKW3270 ", "t": "E75L",
 *               "alt_baro": 11275 | "ground", "gs": 331.0,
 *               "track": 180.17, "squawk": "3304", "emergency": "none",
 *               "dbFlags": 1, "lat": 38.30, "lon": -123.01,
 *               "seen_pos": 0.178 }, ... ],
 *     "now": 1700000000000, "total": 89 }
 * readsb/dump1090 aircraft.json uses the same per-aircraft fields under
 * "aircraft" instead of "ac".
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aircraft.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include "math_utils.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADSBLOL_POINT "https://api.adsb.lol/v2/point/%.4f/%.4f/%d"
#define ADSBLOL_MIL   "https://api.adsb.lol/v2/mil"
#define FETCH_INTERVAL 30         /* seconds; the layer dead-reckons */
#define MAX_RESPONSE   (3 * 1024 * 1024)
#define STALE_S        60.0       /* drop positions older than this  */

static pthread_t ac_thread;
static volatile int ac_running = 0;
static pic_aircraft_t *cfg_data = NULL;

void pic_ac_init(pic_aircraft_t *data)
{
    memset(data, 0, sizeof(*data));
    data->radius_nm = 150;
    data->labels = 1;
    pthread_mutex_init(&data->mutex, NULL);
}

/* Move a position along a track for `dt` seconds at `gs` knots. */
static void advance(float *lat, float *lon, float track, float gs, double dt)
{
    double dist_deg, tr;
    if (track < 0 || gs <= 0 || dt <= 0) return;
    dist_deg = gs * dt / 3600.0 / 60.0;   /* 1 nm = 1/60 deg of arc */
    tr = track * M_PI / 180.0;
    *lat += (float)(dist_deg * cos(tr));
    *lon += (float)(dist_deg * sin(tr) / cos(*lat * M_PI / 180.0));
}

int pic_ac_parse(const char *js, size_t len, pic_ac_t *out, int *count,
                 int max, int mil, char (*hexes)[8])
{
    pic_json_t j;
    int arr, n, i, added = 0;

    if (pic_json_parse(&j, js, len) != 0) return 0;
    arr = pic_json_get(&j, 0, "ac");
    if (arr < 0) arr = pic_json_get(&j, 0, "aircraft");
    n = pic_json_size(&j, arr);

    for (i = 0; i < n && *count < max; i++) {
        int a = pic_json_at(&j, arr, i), k, alt_tok;
        pic_ac_t *p = &out[*count];
        char hex[8], sq[8], em[16];
        double lat, lon, seen;

        lat = pic_json_num(&j, pic_json_get(&j, a, "lat"), 999);
        lon = pic_json_num(&j, pic_json_get(&j, a, "lon"), 999);
        if (lat > 90 || lon > 180) continue;            /* no position */
        seen = pic_json_num(&j, pic_json_get(&j, a, "seen_pos"), 0);
        if (seen > STALE_S) continue;

        pic_json_str(&j, pic_json_get(&j, a, "hex"), hex, sizeof(hex));
        for (k = 0; k < *count && hexes; k++)
            if (strcmp(hexes[k], hex) == 0) break;
        if (hexes && k < *count) {                      /* duplicate */
            if (mil) out[k].military = 1;
            continue;
        }

        memset(p, 0, sizeof(*p));
        p->lat = (float)lat;
        p->lon = (float)lon;
        p->track = (float)pic_json_num(&j, pic_json_get(&j, a, "track"), -1);
        p->gs = (float)pic_json_num(&j, pic_json_get(&j, a, "gs"), 0);
        /* Bring the position up to "now" of the response */
        advance(&p->lat, &p->lon, p->track, p->gs, seen);

        alt_tok = pic_json_get(&j, a, "alt_baro");
        p->alt = pic_json_eq(&j, alt_tok, "ground") ? -1
               : (int)pic_json_num(&j, alt_tok, 0);

        pic_json_str(&j, pic_json_get(&j, a, "flight"), p->flight, sizeof(p->flight));
        for (k = (int)strlen(p->flight) - 1; k >= 0 && isspace((unsigned char)p->flight[k]); k--)
            p->flight[k] = '\0';
        pic_json_str(&j, pic_json_get(&j, a, "t"), p->type, sizeof(p->type));

        p->military = mil ||
            ((int)pic_json_num(&j, pic_json_get(&j, a, "dbFlags"), 0) & 1);
        pic_json_str(&j, pic_json_get(&j, a, "squawk"), sq, sizeof(sq));
        pic_json_str(&j, pic_json_get(&j, a, "emergency"), em, sizeof(em));
        p->emergency = strcmp(sq, "7500") == 0 || strcmp(sq, "7600") == 0 ||
                       strcmp(sq, "7700") == 0 ||
                       (em[0] && strcmp(em, "none") != 0);

        if (hexes) snprintf(hexes[*count], 8, "%s", hex);
        (*count)++;
        added++;
    }

    pic_json_free(&j);
    return added;
}

/*
 * Flight routes (origin → destination) for airline callsigns near
 * home, from the adsbdb.com callsign database. ADS-B itself doesn't
 * carry them. Cached for 6 h per callsign (misses too), and at most a
 * few new lookups per fetch so the API is never hammered.
 */
#define ROUTE_URL     "https://api.adsbdb.com/v0/callsign/%s"
#define ROUTE_CACHE   256
#define ROUTE_TTL     (6 * 3600)
#define ROUTE_NEAR_KM 92.6          /* 50 nautical miles */
#define ROUTE_PER_CYCLE 4

static struct {
    char   cs[10];
    char   o[8], d[8];              /* IATA (or ICAO) codes, "" if unknown */
    double olat, olon, dlat, dlon;
    time_t t;
} rcache[ROUTE_CACHE];
static int rcache_next;

/* Airline-style callsign: 3 letters then a digit (AAL2947, DAL1234),
 * not a registration (N1234X) or a military/private name */
static int airline_callsign(const char *cs)
{
    return isalpha((unsigned char)cs[0]) && isalpha((unsigned char)cs[1]) &&
           isalpha((unsigned char)cs[2]) && isdigit((unsigned char)cs[3]);
}

static void airport(pic_json_t *j, int obj, char *code, size_t n,
                    double *lat, double *lon)
{
    char v[8] = "", num[24] = "";
    code[0] = '\0';
    *lat = *lon = 0;
    if (obj < 0) return;
    pic_json_str(j, pic_json_get(j, obj, "iata_code"), v, sizeof(v));
    if (!v[0]) pic_json_str(j, pic_json_get(j, obj, "icao_code"), v, sizeof(v));
    snprintf(code, n, "%s", v);
    if (pic_json_str(j, pic_json_get(j, obj, "latitude"), num, sizeof(num)) >= 0)
        *lat = atof(num);
    if (pic_json_str(j, pic_json_get(j, obj, "longitude"), num, sizeof(num)) >= 0)
        *lon = atof(num);
}

static double km_between(double la1, double lo1, double la2, double lo2)
{
    double p1 = la1 * DEG_TO_RAD, p2 = la2 * DEG_TO_RAD;
    double dp = p2 - p1, dl = (lo2 - lo1) * DEG_TO_RAD;
    double h = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2 * 6371.0 * asin(sqrt(h));
}

static double bearing(double la1, double lo1, double la2, double lo2)
{
    double p1 = la1 * DEG_TO_RAD, p2 = la2 * DEG_TO_RAD, dl = (lo2 - lo1) * DEG_TO_RAD;
    return atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl));
}

/*
 * The callsign database lists a flight's usual route, which can be out
 * of date (airlines reuse flight numbers). Only show it if the plane
 * really is on that route: within ~250 km of the great circle between
 * the two airports and not beyond either end.
 */
static int on_route(double lat, double lon, int i)
{
    double R = 6371.0, d13, d12, xt, at;
    if (!rcache[i].o[0] || !rcache[i].d[0]) return 0;
    d12 = km_between(rcache[i].olat, rcache[i].olon, rcache[i].dlat, rcache[i].dlon);
    d13 = km_between(rcache[i].olat, rcache[i].olon, lat, lon);
    if (d12 < 1) return 0;
    xt = asin(sin(d13 / R) *
              sin(bearing(rcache[i].olat, rcache[i].olon, lat, lon) -
                  bearing(rcache[i].olat, rcache[i].olon, rcache[i].dlat, rcache[i].dlon))) * R;
    at = acos(fmax(-1, fmin(1, cos(d13 / R) / cos(xt / R)))) * R;
    {   /* Behind the origin (more than 90° off the route's heading)? */
        double db = bearing(rcache[i].olat, rcache[i].olon, lat, lon) -
                    bearing(rcache[i].olat, rcache[i].olon, rcache[i].dlat, rcache[i].dlon);
        if (cos(db) < 0) at = -at;
    }
    return fabs(xt) < 250 && at > -100 && at < d12 + 100;
}

/* Fill route for cs at (lat, lon); returns 1 if it used the network */
static int route_for(const char *cs, double lat, double lon, char *route,
                     size_t n, int allow_fetch, char *buf, int buf_size)
{
    time_t now = time(NULL);
    char url[96];
    int i, len;
    pic_json_t j;

    route[0] = '\0';
    for (i = 0; i < ROUTE_CACHE; i++)
        if (rcache[i].cs[0] && !strcmp(rcache[i].cs, cs) &&
            now - rcache[i].t < ROUTE_TTL)
            break;
    if (i == ROUTE_CACHE) {
        if (!allow_fetch) return 0;
        snprintf(url, sizeof(url), ROUTE_URL, cs);
        len = pic_fetch_to_buf(url, buf, buf_size);
        if (len <= 0) return 1;   /* network failure: try again later */
        i = rcache_next;
        rcache_next = (rcache_next + 1) % ROUTE_CACHE;
        memset(&rcache[i], 0, sizeof(rcache[i]));
        snprintf(rcache[i].cs, sizeof(rcache[i].cs), "%s", cs);
        rcache[i].t = now;
        if (pic_json_parse(&j, buf, (size_t)len) >= 0) {
            int fr = pic_json_path(&j, 0, "response.flightroute");
            if (fr >= 0) {
                airport(&j, pic_json_get(&j, fr, "origin"), rcache[i].o,
                        sizeof(rcache[i].o), &rcache[i].olat, &rcache[i].olon);
                airport(&j, pic_json_get(&j, fr, "destination"), rcache[i].d,
                        sizeof(rcache[i].d), &rcache[i].dlat, &rcache[i].dlon);
            }
            pic_json_free(&j);
        }
        if (on_route(lat, lon, i))
            snprintf(route, n, "%s\xe2\x86\x92%s", rcache[i].o, rcache[i].d);
        return 1;
    }
    if (on_route(lat, lon, i))
        snprintf(route, n, "%s\xe2\x86\x92%s", rcache[i].o, rcache[i].d);   /* "→" */
    return 0;
}

static void add_routes(pic_ac_t *ac, int count)
{
    static char rbuf[16384];
    int i, fetched = 0;
    for (i = 0; i < count; i++) {
        pic_ac_t *a = &ac[i];
        a->route[0] = '\0';
        if (a->alt <= 0 || !airline_callsign(a->flight)) continue;
        if (km_between(a->lat, a->lon, pic_config.qth_lat, pic_config.qth_lon) > ROUTE_NEAR_KM)
            continue;
        fetched += route_for(a->flight, a->lat, a->lon, a->route, sizeof(a->route),
                             fetched < ROUTE_PER_CYCLE, rbuf, sizeof(rbuf));
    }
}

static void *ac_thread_func(void *arg)
{
    char *buf;
    static pic_ac_t temp[AC_MAX];
    static char hexes[AC_MAX][8];
    (void)arg;

    buf = malloc(MAX_RESPONSE);
    if (!buf) return NULL;

    if (!pic_wait_for_network("aircraft", &ac_running)) {
        free(buf);
        return NULL;
    }

    while (ac_running) {
        char url[160], local[96];
        int count = 0, radius, mil, i, n, ok = 0;
        const char *src;

        pthread_mutex_lock(&cfg_data->mutex);
        radius = cfg_data->radius_nm;
        mil = cfg_data->show_mil;
        snprintf(local, sizeof(local), "%s", cfg_data->local);
        pthread_mutex_unlock(&cfg_data->mutex);

        if (local[0]) {
            /* Local receiver: covers its own range only */
            snprintf(url, sizeof(url), "http://%s/data/aircraft.json", local);
            src = "local receiver";
        } else {
            snprintf(url, sizeof(url), ADSBLOL_POINT,
                     pic_config.qth_lat, pic_config.qth_lon, radius);
            src = "adsb.lol";
        }

        n = pic_fetch_to_buf(url, buf, MAX_RESPONSE);
        if (n > 0) {
            pic_ac_parse(buf, (size_t)n, temp, &count, AC_MAX, 0, hexes);
            ok = 1;
        }
        if (mil && ac_running) {
            n = pic_fetch_to_buf(ADSBLOL_MIL, buf, MAX_RESPONSE);
            if (n > 0) {
                pic_ac_parse(buf, (size_t)n, temp, &count, AC_MAX, 1, hexes);
                ok = 1;
            }
        }

        if (ok) {
            add_routes(temp, count);
            pthread_mutex_lock(&cfg_data->mutex);
            memcpy(cfg_data->ac, temp, (size_t)count * sizeof(pic_ac_t));
            cfg_data->count = count;
            cfg_data->t_pos = time(NULL);
            snprintf(cfg_data->source, sizeof(cfg_data->source), "%s", src);
            pthread_mutex_unlock(&cfg_data->mutex);
        }

        for (i = 0; i < FETCH_INTERVAL && ac_running; i++)
            sleep(1);
    }

    free(buf);
    return NULL;
}

int pic_ac_start(pic_aircraft_t *data)
{
    if (ac_running) return 0;
    cfg_data = data;
    pic_ac_reload(data);
    ac_running = 1;
    if (pthread_create(&ac_thread, NULL, ac_thread_func, NULL) != 0) {
        ac_running = 0;
        return -1;
    }
    printf("aircraft: fetcher thread started\n");
    return 0;
}

void pic_ac_stop(void)
{
    if (!ac_running) return;
    ac_running = 0;
    pthread_join(ac_thread, NULL);
    if (cfg_data) {
        pthread_mutex_lock(&cfg_data->mutex);
        cfg_data->count = 0;
        pthread_mutex_unlock(&cfg_data->mutex);
    }
    cfg_data = NULL;
}

/* host[:port] made of hostname characters only */
static int valid_hostport(const char *s)
{
    int colon = 0;
    if (!*s) return 1;
    for (; *s; s++) {
        if (*s == ':') { if (colon++) return 0; continue; }
        if (colon && !isdigit((unsigned char)*s)) return 0;
        if (!colon && !isalnum((unsigned char)*s) && *s != '.' && *s != '-')
            return 0;
    }
    return 1;
}

void pic_ac_reload(pic_aircraft_t *data)
{
    FILE *f;
    char line[160], local[96] = "";
    int radius = 150, mil = 0, labels = 1;

    f = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (strncmp(line, "AIRCRAFT_RADIUS_NM=", 19) == 0)
                radius = atoi(line + 19);
            else if (strncmp(line, "AIRCRAFT_MIL=", 13) == 0)
                mil = atoi(line + 13) != 0;
            else if (strncmp(line, "AIRCRAFT_LABELS=", 16) == 0)
                labels = atoi(line + 16);
            else if (strncmp(line, "AIRCRAFT_LOCAL=", 15) == 0)
                snprintf(local, sizeof(local), "%.95s", line + 15);
        }
        fclose(f);
    }
    if (radius < 25) radius = 25;
    if (radius > 250) radius = 250;
    if (labels < 0) labels = 0;
    if (labels > 2) labels = 2;
    if (!valid_hostport(local)) local[0] = '\0';

    pthread_mutex_lock(&data->mutex);
    data->radius_nm = radius;
    data->show_mil = mil;
    data->labels = labels;
    snprintf(data->local, sizeof(data->local), "%s", local);
    pthread_mutex_unlock(&data->mutex);
}
