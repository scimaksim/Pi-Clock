/*
 * applet_tides.c - Tide predictions for the nearest NOAA station
 *
 *   Tides · Wrightsville Beach
 *   ╭──╮      ╭──╮          <- 24 h curve, "now" marker
 *   ╯   ╰────╯   ╰
 *   Now 2.1 ft rising
 *   High 3:16 PM 5.3 ft    Low 9:50 PM 0.6 ft
 *
 * Predictions (high/low only) come from NOAA CO-OPS; the curve is
 * cosine-interpolated between consecutive extremes, which is how
 * tide tables are normally read. The station is the nearest NOAA
 * harmonic (reference) station to the QTH unless TIDE_STATION=<id>
 * is set.
 * TIDE_UNITS=metric shows metres.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE  /* timegm */
#include "feature.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CO_OPS "https://api.tidesandcurrents.noaa.gov"
#define STATIONS_URL CO_OPS "/mdapi/prod/webapi/stations.json?type=tidepredictions"
#define PRED_FMT CO_OPS "/api/prod/datagetter?product=predictions&datum=MLLW" \
    "&station=%s&interval=hilo&units=%s&time_zone=gmt&format=json" \
    "&begin_date=%04d%02d%02d&range=72&application=Pi-Clock"
#define MAX_EXT 16

typedef struct { time_t t; double v; int high; } tide_ext_t;

typedef struct {
    pthread_mutex_t mutex;
    char       want_id[16];   /* From config ("" = nearest)        */
    int        metric;
    char       id[16];
    char       name[64];
    double     dist_km;
    tide_ext_t ext[MAX_EXT];
    int        n;
    double     qlat, qlon;    /* QTH the station was chosen for    */
    time_t     fetched;
    int        error;
} tides_t;

static tides_t td;

/*
 * Nearest harmonic station from the station list. Objects are parsed one at a
 * time so the 2 MB list never needs a full token array.
 */
int pic_tides_nearest(const char *js, double qlat, double qlon,
                      char *id, int id_n, char *name, int name_n,
                      double *dist)
{
    const char *p = strstr(js, "\"stations\"");
    double best = 1e9;
    int found = 0;

    if (!p || !(p = strchr(p, '['))) return 0;
    p++;
    while ((p = strchr(p, '{')) != NULL) {
        const char *q = p;
        int depth = 0, in_str = 0;
        pic_json_t j;

        /* Find the matching close brace */
        for (; *q; q++) {
            if (in_str) {
                if (*q == '\\' && q[1]) q++;
                else if (*q == '"') in_str = 0;
            } else if (*q == '"') in_str = 1;
            else if (*q == '{') depth++;
            else if (*q == '}' && --depth == 0) break;
        }
        if (!*q) break;
        if (pic_json_parse_max(&j, p, (size_t)(q - p + 1), 256) == 0) {
            double lat = pic_json_num(&j, pic_json_get(&j, 0, "lat"), 999);
            double lon = pic_json_num(&j, pic_json_get(&j, 0, "lng"), 999);
            /* Prefer harmonic ("R") reference stations: subordinate
             * stations are often up rivers and sounds. */
            if (fabs(lat) <= 90 && fabs(lon) <= 180 &&
                pic_json_eq(&j, pic_json_get(&j, 0, "type"), "R")) {
                double d = pic_dist_km(qlat, qlon, lat, lon);
                if (d < best) {
                    best = d;
                    pic_json_str(&j, pic_json_get(&j, 0, "id"), id, id_n);
                    pic_json_str(&j, pic_json_get(&j, 0, "name"), name, name_n);
                    found = 1;
                }
            }
            pic_json_free(&j);
        }
        p = q + 1;
    }
    if (found && dist) *dist = best;
    return found;
}

int pic_tides_parse(const char *js, size_t len, tide_ext_t *out, int max)
{
    pic_json_t j;
    int arr, n, i, c = 0;

    if (pic_json_parse(&j, js, len) < 0) return -1;
    arr = pic_json_get(&j, 0, "predictions");
    if (arr < 0) {
        pic_json_free(&j);
        return -1;
    }
    n = pic_json_size(&j, arr);
    for (i = 0; i < n && c < max; i++) {
        int e = pic_json_at(&j, arr, i);
        char t[24], type[4];
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        pic_json_str(&j, pic_json_get(&j, e, "t"), t, sizeof(t));
        pic_json_str(&j, pic_json_get(&j, e, "type"), type, sizeof(type));
        if (sscanf(t, "%d-%d-%d %d:%d", &tm.tm_year, &tm.tm_mon,
                   &tm.tm_mday, &tm.tm_hour, &tm.tm_min) != 5)
            continue;
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        out[c].t = timegm(&tm);
        out[c].v = pic_json_num(&j, pic_json_get(&j, e, "v"), 0);
        out[c].high = type[0] == 'H';
        c++;
    }
    pic_json_free(&j);
    return c;
}

static int tides_work(pic_feature_t *f)
{
    char id[16], name[64], url[320], *buf;
    double qlat = pic_config.qth_lat, qlon = pic_config.qth_lon, dist = -1;
    int metric, n;
    static tide_ext_t tmp[MAX_EXT];
    time_t start = time(NULL) - 86400;
    struct tm tm;

    (void)f;
    pthread_mutex_lock(&td.mutex);
    metric = td.metric;
    if (td.want_id[0]) {
        snprintf(id, sizeof(id), "%s", td.want_id);
        snprintf(name, sizeof(name), "%s", strcmp(td.id, id) == 0 ? td.name : "");
    } else if (td.id[0] && td.qlat == qlat && td.qlon == qlon) {
        snprintf(id, sizeof(id), "%s", td.id);
        snprintf(name, sizeof(name), "%s", td.name);
        dist = td.dist_km;
    } else {
        id[0] = '\0';
    }
    pthread_mutex_unlock(&td.mutex);

    buf = malloc(3 * 1024 * 1024);
    if (!buf) return 600;

    if (!id[0] || !name[0]) {
        /* Station list: needed to find the nearest or name a fixed one */
        int len = pic_fetch_to_buf(STATIONS_URL, buf, 3 * 1024 * 1024 - 1);
        if (len <= 0) {
            free(buf);
            return 600;
        }
        buf[len] = '\0';
        if (!id[0]) {
            if (!pic_tides_nearest(buf, qlat, qlon, id, sizeof(id), name,
                                   sizeof(name), &dist)) {
                free(buf);
                return 3600;
            }
        } else {
            /* Look up the configured station's name */
            char key[32], *p;
            snprintf(key, sizeof(key), "\"id\": \"%s\"", id);
            p = strstr(buf, key);
            if (p && (p = strstr(p, "\"name\": \"")) != NULL) {
                p += 9;
                snprintf(name, sizeof(name), "%.*s", (int)strcspn(p, "\""), p);
            } else {
                snprintf(name, sizeof(name), "Station %s", id);
            }
        }
        printf("tides: station %s %s (%.0f km)\n", id, name, dist);
    }

    gmtime_r(&start, &tm);
    snprintf(url, sizeof(url), PRED_FMT, id, metric ? "metric" : "english",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    n = pic_fetch_to_buf(url, buf, 1024 * 1024 - 1);
    if (n > 0) {
        buf[n] = '\0';
        n = pic_tides_parse(buf, (size_t)n, tmp, MAX_EXT);
    }
    free(buf);

    pthread_mutex_lock(&td.mutex);
    snprintf(td.id, sizeof(td.id), "%s", id);
    snprintf(td.name, sizeof(td.name), "%s", name);
    td.dist_km = dist;
    td.qlat = qlat;
    td.qlon = qlon;
    if (n > 0) {
        memcpy(td.ext, tmp, (size_t)n * sizeof(tide_ext_t));
        td.n = n;
        td.fetched = time(NULL);
        td.error = 0;
    } else {
        td.error = 1;
    }
    pthread_mutex_unlock(&td.mutex);
    return n > 0 ? 6 * 3600 : 900;
}

static void tides_reload(pic_feature_t *f)
{
    char id[16] = "", units[16] = "";
    pic_conf_str("TIDE_STATION", id, sizeof(id));
    pic_conf_str("TIDE_UNITS", units, sizeof(units));
    pthread_mutex_lock(&td.mutex);
    if (strcmp(id, td.want_id) != 0 ||
        (strcmp(units, "metric") == 0) != td.metric) {
        snprintf(td.want_id, sizeof(td.want_id), "%s", id);
        td.metric = strcmp(units, "metric") == 0;
        td.n = 0;
        if (!id[0]) td.id[0] = '\0';
        pic_feature_poke(f);
    }
    pthread_mutex_unlock(&td.mutex);
}

/* Height at t by cosine interpolation; returns 0 outside the data. */
static int height_at(const tides_t *d, time_t t, double *v)
{
    int i;
    for (i = 0; i + 1 < d->n; i++) {
        const tide_ext_t *a = &d->ext[i], *b = &d->ext[i + 1];
        if (t >= a->t && t <= b->t) {
            double fr = (double)(t - a->t) / (double)(b->t - a->t);
            *v = a->v + (b->v - a->v) * (1 - cos(fr * M_PI)) / 2;
            return 1;
        }
    }
    return 0;
}

double pic_applet_render_tides(cairo_t *cr, double width, time_t now,
                               void *ud)
{
    double fs = width / 17.0, title_h = pic_applet_title(NULL, width, "");
    double graph_h = fs * 3.2, y, vmin = 1e9, vmax = -1e9, v;
    double total = title_h + fs * 0.4 + graph_h + fs * 1.2 + fs * 1.35 * 2;
    time_t t0 = now - 3 * 3600, t1 = now + 21 * 3600, t;
    char title[96], line[96], hm[16];
    const char *unit;
    int i, nh = 0, nl = 0;

    (void)ud;
    if (!cr) return total;

    pthread_mutex_lock(&td.mutex);
    unit = td.metric ? "m" : "ft";
    /* "Sneads Ferry, New River" -> "Sneads Ferry" */
    snprintf(title, sizeof(title), "Tides%s%.*s", td.name[0] ? " \xc2\xb7 " : "",
             (int)strcspn(td.name, ","), td.name);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, width / 12.0);
    pic_fit_text(cr, title, width);
    pic_applet_title(cr, width, title);
    y = title_h + fs * 0.4;

    if (td.n < 2) {
        cairo_set_font_size(cr, fs);
        cairo_set_source_rgba(cr, 0.6, 0.6, 0.65, 0.9);
        cairo_move_to(cr, 0, y + fs);
        cairo_show_text(cr, td.error ? "No tide data" : "Loading\xe2\x80\xa6");
        pthread_mutex_unlock(&td.mutex);
        return total;
    }

    for (i = 0; i < td.n; i++) {
        if (td.ext[i].v < vmin) vmin = td.ext[i].v;
        if (td.ext[i].v > vmax) vmax = td.ext[i].v;
    }
    if (vmax - vmin < 0.1) vmax = vmin + 0.1;

    /* Curve, filled like water */
    cairo_new_path(cr);
    cairo_move_to(cr, 0, y + graph_h);
    for (t = t0; t <= t1; t += 600) {
        double x = (double)(t - t0) / (double)(t1 - t0) * width;
        if (!height_at(&td, t, &v)) continue;
        cairo_line_to(cr, x, y + graph_h - (v - vmin) / (vmax - vmin) * graph_h * 0.9);
    }
    cairo_line_to(cr, width, y + graph_h);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.2, 0.5, 0.9, 0.45);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.45, 0.75, 1.0, 0.9);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);

    /* Now marker */
    {
        double x = (double)(now - t0) / (double)(t1 - t0) * width;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.8);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, x, y);
        cairo_line_to(cr, x, y + graph_h);
        cairo_stroke(cr);
        if (height_at(&td, now, &v)) {
            double v2 = v;
            height_at(&td, now + 600, &v2);
            cairo_arc(cr, x, y + graph_h - (v - vmin) / (vmax - vmin) * graph_h * 0.9,
                      fs * 0.25, 0, 2 * M_PI);
            cairo_fill(cr);
            /* interpolated between the predicted high and low: an estimate */
            snprintf(line, sizeof(line), "Now about %.1f %s %s", v, unit,
                     v2 >= v ? "rising" : "falling");
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, fs);
            cairo_move_to(cr, 0, y + graph_h + fs * 1.2);
            cairo_set_source_rgba(cr, 0.9, 0.95, 1.0, 0.95);
            cairo_show_text(cr, line);
        }
    }
    y += graph_h + fs * 1.2;

    /* Next high and low */
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs * 0.95);
    for (i = 0; i < td.n && nh + nl < 2; i++) {
        const tide_ext_t *e = &td.ext[i];
        if (e->t < now) continue;
        if (e->high ? nh : nl) continue;
        if (e->high) nh++; else nl++;
        pic_fmt_when(hm, sizeof(hm), now, e->t);
        snprintf(line, sizeof(line), "%s  %s  %.1f %s", e->high ? "High" : "Low",
                 hm, e->v, unit);
        cairo_set_source_rgba(cr, e->high ? 0.6 : 0.75, e->high ? 0.85 : 0.8,
                              1.0, 0.9);
        cairo_move_to(cr, 0, y + fs * 1.35);
        cairo_show_text(cr, line);
        y += fs * 1.35;
    }
    pthread_mutex_unlock(&td.mutex);
    return total;
}

static pic_feature_t feature_def = {
    .name = "tides", .is_applet = 1,
    .applet = pic_applet_render_tides, .label = "Tides",
    .interval = 60, .applet_w = 220, .side = APPLET_SIDE_LEFT,
    .reload = tides_reload, .work = tides_work, .needs_network = 1,
};

pic_feature_t *pic_feature_tides(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&td.mutex, NULL);
        inited = 1;
    }
    return &feature_def;
}
