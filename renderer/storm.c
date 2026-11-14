/*
 * storm.c - GDACS tropical cyclone fetcher
 *
 * Every 30 minutes:
 *   1. Fetch the GDACS "latest events" list (all hazard types) and pick
 *      tropical cyclones (eventtype "TC") flagged iscurrent=true.
 *   2. For each, fetch its getgeometry GeoJSON, which holds:
 *        Point_Centroid          event centroid
 *        Point_Polygon_Point_N   track point N (a small circle polygon
 *                                around the position, in time order)
 *        Line_Line_N             track segment, label = category
 *                                (TD/TS/...), "forecast": true|false
 *        Poly_Cones              forecast uncertainty cone
 *        Poly_Green/Orange/Red   wind radii (ignored here)
 *
 * The segment features are not in time order, so each segment is
 * matched to the track point it starts from to find which points are
 * forecasts and what category each leg is.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "storm.h"
#include "datastatus.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include "math_utils.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GDACS_EVENTS_URL \
    "https://www.gdacs.org/gdacsapi/api/events/geteventlist/EVENTS4APP"
#define GDACS_GEOMETRY_FMT \
    "https://www.gdacs.org/gdacsapi/api/polygons/getgeometry?eventtype=TC&eventid=%ld&episodeid=%ld"

#define FETCH_INTERVAL       (30 * 60)
#define FETCH_RETRY_INTERVAL (5 * 60)

#define MAX_EVENTS_RESPONSE   (2 * 1024 * 1024)
/* Geometry includes wind-radii polygons for every advisory; large,
 * long-lived storms can reach a few MB. */
#define MAX_GEOMETRY_RESPONSE (6 * 1024 * 1024)

static pthread_t storm_thread;
static volatile int storm_running = 0;
static pic_storms_t *cfg_data = NULL;

void pic_storm_init(pic_storms_t *data)
{
    memset(data, 0, sizeof(*data));
    data->show_cone = 1;
    data->show_track = 1;
    pthread_mutex_init(&data->mutex, NULL);
}

int pic_storm_category(double kmh)
{
    if (kmh < 63)  return STORM_CAT_TD;
    if (kmh < 119) return STORM_CAT_TS;
    if (kmh < 154) return STORM_CAT_1;
    if (kmh < 178) return STORM_CAT_2;
    if (kmh < 209) return STORM_CAT_3;
    if (kmh < 252) return STORM_CAT_4;
    return STORM_CAT_5;
}

/* Category from a segment label such as "TD", "TS", "Cat. 3", "H2". */
static int category_from_label(const char *s, int fallback)
{
    const char *p;
    if (strstr(s, "TD") || strstr(s, "Depression")) return STORM_CAT_TD;
    if (strstr(s, "TS") || strstr(s, "Storm"))      return STORM_CAT_TS;
    for (p = s; *p; p++)
        if (*p >= '1' && *p <= '5') return STORM_CAT_1 + (*p - '1');
    return fallback;
}

static double gc_km(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * DEG_TO_RAD, p2 = lat2 * DEG_TO_RAD;
    double dp = p2 - p1, dl = (lon2 - lon1) * DEG_TO_RAD;
    double a = sin(dp / 2) * sin(dp / 2) +
               cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

/* ── Event list ─────────────────────────────────────────────────── */

int pic_storm_parse_events(const char *js, size_t len,
                           char urls[][256], int max)
{
    pic_json_t j;
    int feats, i, n, count = 0;

    if (pic_json_parse(&j, js, len) != 0) return 0;

    feats = pic_json_get(&j, 0, "features");
    n = pic_json_size(&j, feats);

    for (i = 0; i < n && count < max; i++) {
        int f = pic_json_at(&j, feats, i);
        int props = pic_json_get(&j, f, "properties");
        char url[256];

        if (!pic_json_eq(&j, pic_json_get(&j, props, "eventtype"), "TC"))
            continue;
        if (!pic_json_bool(&j, pic_json_get(&j, props, "iscurrent"), 0))
            continue;

        /* Prefer the geometry link GDACS provides; build it from the
         * IDs otherwise. Only ever follow links on gdacs.org. */
        pic_json_str(&j, pic_json_path(&j, props, "url.geometry"),
                     url, sizeof(url));
        if (strncmp(url, "https://www.gdacs.org/", 22) != 0) {
            long eid = (long)pic_json_num(&j, pic_json_get(&j, props, "eventid"), -1);
            long epi = (long)pic_json_num(&j, pic_json_get(&j, props, "episodeid"), -1);
            if (eid <= 0 || epi <= 0) continue;
            snprintf(url, sizeof(url), GDACS_GEOMETRY_FMT, eid, epi);
        }

        snprintf(urls[count], 256, "%s", url);
        count++;
    }

    pic_json_free(&j);
    return count;
}

/* ── Geometry ───────────────────────────────────────────────────── */

/* Centre of a small circle polygon: mean of its ring vertices. */
static int ring_centre(const pic_json_t *j, int ring, double *lat, double *lon)
{
    int k, n = pic_json_size(j, ring);
    double sx = 0, sy = 0, first = 0;
    int used = 0;

    if (n > 1) n--;     /* closing vertex repeats the first */
    for (k = 0; k < n; k++) {
        int c = pic_json_at(j, ring, k);
        double lo = pic_json_num(j, pic_json_at(j, c, 0), 0);
        if (!used) first = lo;
        while (lo - first > 180) lo -= 360;     /* ring across the date line */
        while (lo - first < -180) lo += 360;
        sx += lo;
        sy += pic_json_num(j, pic_json_at(j, c, 1), 0);
        used++;
    }
    if (!used) return -1;
    *lon = sx / used;
    if (*lon > 180) *lon -= 360;
    if (*lon < -180) *lon += 360;
    *lat = sy / used;
    return 0;
}

typedef struct {
    double lat0, lon0, lat1, lon1;
    int forecast, cat;
} seg_t;

int pic_storm_parse_geometry(const char *js, size_t len, pic_storm_t *out,
                             double qth_lat, double qth_lon)
{
    pic_json_t j;
    int feats, i, n;
    double pt_lat[STORM_TRACK_MAX], pt_lon[STORM_TRACK_MAX];
    int pt_valid[STORM_TRACK_MAX];
    int max_pt = -1;
    static seg_t segs[STORM_TRACK_MAX * 2];
    int nseg = 0;
    int have_centroid = 0;
    double cen_lat = 0, cen_lon = 0;

    memset(out, 0, sizeof(*out));
    memset(pt_valid, 0, sizeof(pt_valid));
    out->dist_now_km = out->dist_min_km = -1;

    if (pic_json_parse(&j, js, len) != 0) return -1;

    feats = pic_json_get(&j, 0, "features");
    n = pic_json_size(&j, feats);

    for (i = 0; i < n; i++) {
        int f = pic_json_at(&j, feats, i);
        int props = pic_json_get(&j, f, "properties");
        int coords = pic_json_path(&j, f, "geometry.coordinates");
        char cls[48];

        pic_json_str(&j, pic_json_get(&j, props, "Class"), cls, sizeof(cls));

        if (strcmp(cls, "Point_Centroid") == 0) {
            char alert[16];
            pic_json_str(&j, pic_json_get(&j, props, "eventname"),
                         out->name, sizeof(out->name));
            pic_json_str(&j, pic_json_get(&j, props, "source"),
                         out->source, sizeof(out->source));
            out->wind_kmh = pic_json_num(&j,
                pic_json_path(&j, props, "severitydata.severity"), 0);
            pic_json_str(&j, pic_json_get(&j, props, "alertlevel"),
                         alert, sizeof(alert));
            out->alert = strcmp(alert, "Red") == 0 ? 2 :
                         strcmp(alert, "Orange") == 0 ? 1 : 0;
            cen_lon = pic_json_num(&j, pic_json_at(&j, coords, 0), 0);
            cen_lat = pic_json_num(&j, pic_json_at(&j, coords, 1), 0);
            have_centroid = 1;
        } else if (strncmp(cls, "Point_Polygon_Point_", 20) == 0) {
            int idx = atoi(cls + 20);
            if (idx >= 0 && idx < STORM_TRACK_MAX &&
                ring_centre(&j, pic_json_at(&j, coords, 0),
                            &pt_lat[idx], &pt_lon[idx]) == 0) {
                pt_valid[idx] = 1;
                if (idx > max_pt) max_pt = idx;
            }
        } else if (strncmp(cls, "Line_Line_", 10) == 0 &&
                   nseg < (int)(sizeof(segs) / sizeof(segs[0]))) {
            char label[32];
            seg_t *s = &segs[nseg];
            int a = pic_json_at(&j, coords, 0);
            int b = pic_json_at(&j, coords, pic_json_size(&j, coords) - 1);
            if (a < 0 || b < 0) continue;
            s->lon0 = pic_json_num(&j, pic_json_at(&j, a, 0), 0);
            s->lat0 = pic_json_num(&j, pic_json_at(&j, a, 1), 0);
            s->lon1 = pic_json_num(&j, pic_json_at(&j, b, 0), 0);
            s->lat1 = pic_json_num(&j, pic_json_at(&j, b, 1), 0);
            s->forecast = pic_json_bool(&j, pic_json_get(&j, props, "forecast"), 0);
            pic_json_str(&j, pic_json_get(&j, props, "polygonlabel"),
                         label, sizeof(label));
            s->cat = category_from_label(label, -1);
            nseg++;
        } else if (strcmp(cls, "Poly_Cones") == 0) {
            /* Outer ring only; decimate evenly to fit the buffer */
            int ring = pic_json_at(&j, coords, 0);
            int rn = pic_json_size(&j, ring);
            int step = rn / STORM_CONE_MAX + 1;
            int k;
            for (k = 0; k < rn && out->ncone < STORM_CONE_MAX; k += step) {
                int c = pic_json_at(&j, ring, k);
                out->cone_lon[out->ncone] =
                    (float)pic_json_num(&j, pic_json_at(&j, c, 0), 0);
                out->cone_lat[out->ncone] =
                    (float)pic_json_num(&j, pic_json_at(&j, c, 1), 0);
                out->ncone++;
            }
        }
    }

    pic_json_free(&j);

    if (!out->name[0]) return -1;
    out->cat = pic_storm_category(out->wind_kmh);

    /* Build the time-ordered track. Each point takes its forecast flag
     * and category from the segment that starts at it; the final point
     * inherits from the segment that ends at it. */
    for (i = 0; i <= max_pt; i++) {
        pic_storm_pt_t *p;
        int k, matched = 0;
        if (!pt_valid[i]) continue;
        p = &out->track[out->ntrack++];
        p->lat = (float)pt_lat[i];
        p->lon = (float)pt_lon[i];
        p->forecast = 0;
        p->cat = (unsigned char)out->cat;
        for (k = 0; k < nseg; k++) {
            if (fabs(segs[k].lat0 - pt_lat[i]) < 0.1 &&
                fabs(segs[k].lon0 - pt_lon[i]) < 0.1) {
                p->forecast = (unsigned char)segs[k].forecast;
                if (segs[k].cat >= 0) p->cat = (unsigned char)segs[k].cat;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            for (k = 0; k < nseg; k++) {
                if (fabs(segs[k].lat1 - pt_lat[i]) < 0.1 &&
                    fabs(segs[k].lon1 - pt_lon[i]) < 0.1) {
                    p->forecast = (unsigned char)segs[k].forecast;
                    if (segs[k].cat >= 0) p->cat = (unsigned char)segs[k].cat;
                    break;
                }
            }
        }
    }

    /* Current position: the last observed point, i.e. the point just
     * before the first forecast leg, or the first point if every leg
     * is a forecast (the start of a forecast is the latest fix). */
    if (out->ntrack > 0) {
        int cur = out->ntrack - 1;
        for (i = 0; i < out->ntrack; i++) {
            if (out->track[i].forecast) { cur = i; break; }
        }
        out->lat = out->track[cur].lat;
        out->lon = out->track[cur].lon;
        /* That point is "now", not a forecast */
        out->track[cur].forecast = 0;
    } else if (have_centroid) {
        out->lat = cen_lat;
        out->lon = cen_lon;
    } else {
        return -1;
    }

    /* Distance to the station, now and at closest forecast approach */
    if (qth_lat != 0.0 || qth_lon != 0.0) {
        out->dist_now_km = gc_km(qth_lat, qth_lon, out->lat, out->lon);
        out->dist_min_km = out->dist_now_km;
        for (i = 0; i < out->ntrack; i++) {
            if (out->track[i].forecast) {
                double d = gc_km(qth_lat, qth_lon,
                                 out->track[i].lat, out->track[i].lon);
                if (d < out->dist_min_km) out->dist_min_km = d;
            }
        }
    }

    return 0;
}

/* ── Thread ─────────────────────────────────────────────────────── */

static void *storm_thread_func(void *arg)
{
    pic_status_feature = "storm";   /* data-source status (datastatus.c) */
    char *buf;
    (void)arg;

    buf = malloc(MAX_GEOMETRY_RESPONSE);
    if (!buf) {
        fprintf(stderr, "storm: failed to allocate fetch buffer\n");
        return NULL;
    }

    if (!pic_wait_for_network("storm", &storm_running)) {
        free(buf);
        return NULL;
    }

    while (storm_running) {
        static char urls[PIC_MAX_STORMS][256];
        static pic_storm_t temp[PIC_MAX_STORMS];
        int nurl = 0, ok = 0, i, n, wait = FETCH_RETRY_INTERVAL;

        n = pic_fetch_to_buf(GDACS_EVENTS_URL, buf, MAX_EVENTS_RESPONSE - 1);
        if (n > 0) {
            buf[n] = '\0';
            nurl = pic_storm_parse_events(buf, (size_t)n, urls, PIC_MAX_STORMS);

            for (i = 0; i < nurl && storm_running; i++) {
                n = pic_fetch_to_buf(urls[i], buf, MAX_GEOMETRY_RESPONSE - 1);
                if (n <= 0) continue;
                buf[n] = '\0';
                if (pic_storm_parse_geometry(buf, (size_t)n, &temp[ok],
                                             pic_config.qth_lat,
                                             pic_config.qth_lon) == 0)
                    ok++;
            }

            /* Publish whatever loaded (an empty list clears old storms).
             * If some geometry fetches failed, retry sooner. */
            if (ok > 0 || nurl == 0) {
                pthread_mutex_lock(&cfg_data->mutex);
                memcpy(cfg_data->storms, temp, (size_t)ok * sizeof(pic_storm_t));
                cfg_data->count = ok;
                cfg_data->last_fetched = time(NULL);
                pthread_mutex_unlock(&cfg_data->mutex);
            }
            if (ok == nurl) wait = FETCH_INTERVAL;
            printf("storm: %d current tropical cyclone(s), %d loaded\n",
                   nurl, ok);
        } else {
            printf("storm: event list fetch failed\n");
        }

        for (i = 0; i < wait && storm_running; i++)
            sleep(1);
    }

    free(buf);
    return NULL;
}

int pic_storm_start(pic_storms_t *data)
{
    if (storm_running) return 0;
    cfg_data = data;
    pic_storm_reload(data);
    storm_running = 1;
    if (pthread_create(&storm_thread, NULL, storm_thread_func, NULL) != 0) {
        storm_running = 0;
        fprintf(stderr, "storm: failed to create thread\n");
        return -1;
    }
    printf("storm: fetcher thread started\n");
    return 0;
}

void pic_storm_stop(void)
{
    if (!storm_running) return;
    storm_running = 0;
    pthread_join(storm_thread, NULL);
    cfg_data = NULL;
    printf("storm: fetcher thread stopped\n");
}

void pic_storm_reload(pic_storms_t *data)
{
    FILE *f;
    char line[256];
    int cone = 1, track = 1, min_cat = 0;

    f = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "STORM_CONE=", 11) == 0)
                cone = atoi(line + 11) != 0;
            else if (strncmp(line, "STORM_TRACK=", 12) == 0)
                track = atoi(line + 12) != 0;
            else if (strncmp(line, "STORM_MIN_CAT=", 14) == 0)
                min_cat = atoi(line + 14);
        }
        fclose(f);
    }
    if (min_cat < 0) min_cat = 0;
    if (min_cat > STORM_CAT_5) min_cat = STORM_CAT_5;

    pthread_mutex_lock(&data->mutex);
    data->show_cone = cone;
    data->show_track = track;
    data->min_cat = min_cat;
    pthread_mutex_unlock(&data->mutex);
}
