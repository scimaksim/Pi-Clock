/*
 * layers_rivers.c - River and creek gauges with flood status
 *
 * NOAA National Water Prediction Service gauges within RIVER_RADIUS_KM
 * of home (default 150): a dot per gauge coloured by its observed
 * flood category — blue (normal), yellow (action stage), orange
 * (minor), red (moderate), purple (major). In the local view the
 * gauges are labelled with name and stage. When a gauge forecast says
 * it will reach flood stage, the dot gets a ring.
 *
 * API: https://api.water.noaa.gov/nwps/v1/gauges?bbox...
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NWPS_FMT "https://api.water.noaa.gov/nwps/v1/gauges?bbox.xmin=%.3f" \
                 "&bbox.ymin=%.3f&bbox.xmax=%.3f&bbox.ymax=%.3f&srid=EPSG_4326"
#define MAX_GAUGES 600

typedef struct {
    float lat, lon, stage;
    int   cat, fcat;      /* -1 unknown, 0 none, 1 action .. 4 major */
    char  name[40], unit[6];
} gauge_t;

static struct {
    pthread_mutex_t mutex;
    gauge_t g[MAX_GAUGES];
    int     n, radius_km;
} rv;

static int category(const char *s)
{
    if (!strcmp(s, "no_flooding")) return 0;
    if (!strcmp(s, "action")) return 1;
    if (!strcmp(s, "minor")) return 2;
    if (!strcmp(s, "moderate")) return 3;
    if (!strcmp(s, "major")) return 4;
    return -1;
}

int pic_rivers_parse(const char *js, size_t len, gauge_t *out, int max)
{
    pic_json_t j;
    int arr, n, i, c = 0;
    if (pic_json_parse_max(&j, js, len, 200000) < 0) return -1;
    arr = pic_json_get(&j, 0, "gauges");
    n = pic_json_size(&j, arr);
    for (i = 0; i < n && c < max; i++) {
        int g = pic_json_at(&j, arr, i), st = pic_json_get(&j, g, "status");
        int ob = pic_json_get(&j, st, "observed"), fc = pic_json_get(&j, st, "forecast");
        char b[32];
        gauge_t r;
        memset(&r, 0, sizeof(r));
        r.lat = (float)pic_json_num(&j, pic_json_get(&j, g, "latitude"), 999);
        r.lon = (float)pic_json_num(&j, pic_json_get(&j, g, "longitude"), 999);
        r.stage = (float)pic_json_num(&j, pic_json_get(&j, ob, "primary"), -999);
        pic_json_str(&j, pic_json_get(&j, ob, "primaryUnit"), r.unit, sizeof(r.unit));
        pic_json_str(&j, pic_json_get(&j, ob, "floodCategory"), b, sizeof(b));
        r.cat = category(b);
        pic_json_str(&j, pic_json_get(&j, fc, "floodCategory"), b, sizeof(b));
        r.fcat = category(b);
        pic_json_str(&j, pic_json_get(&j, g, "name"), r.name, sizeof(r.name));
        if (fabsf(r.lat) > 90 || fabsf(r.lon) > 180 || r.stage <= -900) continue;
        out[c++] = r;
    }
    pic_json_free(&j);
    return c;
}

static int rivers_work(pic_feature_t *f)
{
    static gauge_t tmp[MAX_GAUGES];
    double lat = pic_config.qth_lat, lon = pic_config.qth_lon, dl, dn;
    char url[300], *buf;
    int n, rad;

    (void)f;
    pthread_mutex_lock(&rv.mutex);
    rad = rv.radius_km;
    pthread_mutex_unlock(&rv.mutex);
    dl = rad / 111.0;
    dn = rad / (111.0 * cos(lat * M_PI / 180));
    snprintf(url, sizeof(url), NWPS_FMT, lon - dn, lat - dl, lon + dn, lat + dl);
    buf = malloc(4 * 1024 * 1024);
    if (!buf) return 300;
    n = pic_fetch_to_buf(url, buf, 4 * 1024 * 1024 - 1);
    if (n > 0) {
        buf[n] = '\0';
        n = pic_rivers_parse(buf, (size_t)n, tmp, MAX_GAUGES);
    }
    free(buf);
    if (n < 0) {
        printf("rivers: fetch failed\n");
        return 300;
    }
    pthread_mutex_lock(&rv.mutex);
    memcpy(rv.g, tmp, (size_t)n * sizeof(gauge_t));
    rv.n = n;
    pthread_mutex_unlock(&rv.mutex);
    printf("rivers: %d gauges\n", n);
    return 15 * 60;
}

static void rivers_reload(pic_feature_t *f)
{
    int r = pic_conf_int("RIVER_RADIUS_KM", 150);
    if (r < 25) r = 25;
    if (r > 500) r = 500;
    pthread_mutex_lock(&rv.mutex);
    if (r != rv.radius_km) pic_feature_poke(f);
    rv.radius_km = r;
    pthread_mutex_unlock(&rv.mutex);
}

static void cat_rgb(int c, double *r, double *g, double *b)
{
    static const double t[6][3] = {
        { 0.55, 0.6, 0.65 },   /* unknown  */
        { 0.25, 0.6, 1.0 },    /* normal   */
        { 1.0, 0.9, 0.2 },     /* action   */
        { 1.0, 0.55, 0.1 },    /* minor    */
        { 1.0, 0.15, 0.15 },   /* moderate */
        { 0.8, 0.3, 1.0 },     /* major    */
    };
    c = c + 1;
    if (c < 0) c = 0;
    if (c > 5) c = 5;
    *r = t[c][0]; *g = t[c][1]; *b = t[c][2];
}

void pic_layer_render_rivers(cairo_t *cr, int width, int height, time_t now,
                             void *ud)
{
    double z = pic_zoom_factor(), s = height / 260.0 * (1 + 0.3 * (pic_symbol_zoom() - 1));
    double fs = height / 105.0;
    int labels = z >= 16, pass, i, nbox = 0;
    static double box[200][4];

    (void)now; (void)ud;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs);
    pthread_mutex_lock(&rv.mutex);
    /* Normal gauges first so flooding ones sit on top */
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < rv.n; i++) {
            const gauge_t *g = &rv.g[i];
            int hot = g->cat >= 1 || g->fcat >= 2;
            double x, y, r, gg, b, rad;
            char lab[72];
            if (hot != pass) continue;
            x = pic_lon_to_x(g->lon, width);
            y = pic_lat_to_y(g->lat, height);
            if (x < -10 || x > width + 10 || y < -10 || y > height + 10) continue;
            cat_rgb(g->cat, &r, &gg, &b);
            rad = s * (g->cat >= 2 ? 1.6 : g->cat == 1 ? 1.25 : 0.9);
            cairo_new_path(cr);
            cairo_arc(cr, x, y, rad, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, r, gg, b, 0.95);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
            cairo_set_line_width(cr, s * 0.25);
            cairo_stroke(cr);
            if (g->fcat >= 2 && g->fcat > g->cat) {
                cat_rgb(g->fcat, &r, &gg, &b);
                cairo_arc(cr, x, y, rad + s * 0.9, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, r, gg, b, 0.9);
                cairo_set_line_width(cr, s * 0.35);
                cairo_stroke(cr);
            }
            /* Label flooding gauges when zoomed in, and the rest
             * only close to home in the closest views */
            if (!hot && (!labels || pic_dist_km(g->lat, g->lon, pic_config.qth_lat,
                                                pic_config.qth_lon) > 60))
                continue;
            if (z < 4) continue;
            snprintf(lab, sizeof(lab), "%s %.1f %s", g->name, g->stage, g->unit);
            pic_fit_text(cr, lab, fs * 22);
            {
                /* Skip labels that would overlap one already drawn
                 * (flooding gauges are drawn last but win: they're
                 * placed first in pass 1 only among themselves) */
                cairo_text_extents_t e;
                double bx0 = x + rad + 1, by0 = y - fs, bx1, by1 = y + fs * 0.6;
                int b, hit = 0;
                cairo_text_extents(cr, lab, &e);
                bx1 = bx0 + e.x_advance + fs * 0.8;
                for (b = 0; b < nbox && !hit; b++)
                    hit = !(bx1 < box[b][0] || bx0 > box[b][2] || by1 < box[b][1] || by0 > box[b][3]);
                if (hit || nbox >= 200) continue;
                box[nbox][0] = bx0; box[nbox][1] = by0;
                box[nbox][2] = bx1; box[nbox][3] = by1;
                nbox++;
            }
            pic_draw_text_pill(cr, x + rad + 3, y + fs * 0.35, lab,
                               hot ? (r + 1) / 2 : 0.85, hot ? (gg + 1) / 2 : 0.9,
                               hot ? (b + 1) / 2 : 1.0, 0.5);
        }
    pthread_mutex_unlock(&rv.mutex);
}

static pic_feature_t def = {
    .name = "rivers", .layer = pic_layer_render_rivers, .interval = 60,
    .extra_mb = 4, .reload = rivers_reload, .work = rivers_work,
    .needs_network = 1,
};

pic_feature_t *pic_feature_rivers(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&rv.mutex, NULL);
        rv.radius_km = 150;
        inited = 1;
    }
    return &def;
}
