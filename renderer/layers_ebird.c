/*
 * layers_ebird.c - Rare and notable bird sightings near home (eBird)
 *
 * eBird's "notable observations" — species that are rare for the
 * area or season, as flagged by eBird's regional filters — within
 * EBIRD_RADIUS_KM of home (default 50, max 50) over the last
 * EBIRD_DAYS days (default 3). Each location gets a small bird pin;
 * in the local view the species names are listed beside it.
 *
 * Needs a free eBird API key (ebird.org/api/keygen), entered in the
 * dashboard and stored root-only at /data/etc/pi-clock-ebird.key.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EBIRD_KEY_PATH "/data/etc/pi-clock-ebird.key"
#define EBIRD_FMT "https://api.ebird.org/v2/data/obs/geo/recent/notable" \
                  "?lat=%.3f&lng=%.3f&dist=%d&back=%d&detail=simple"
#define MAX_OBS 300

typedef struct {
    float lat, lon;
    char  species[40];
    char  loc[48];
    char  when[20];       /* "2026-10-30 08:15" */
} obs_t;

static struct {
    pthread_mutex_t mutex;
    obs_t o[MAX_OBS];
    int   n, radius, days, status;   /* status: 1 = no key, 2 = rejected */
} eb;

static int read_key(char *key, size_t size)
{
    FILE *f = fopen(EBIRD_KEY_PATH, "r");
    size_t n = 0;
    int c;
    key[0] = '\0';
    if (!f) return -1;
    while ((c = fgetc(f)) != EOF && n < size - 1) {
        if (isalnum(c)) key[n++] = (char)c;
        else if (c == '\n' || c == '\r' || c == ' ') break;
        else { n = 0; break; }
    }
    fclose(f);
    key[n] = '\0';
    return n >= 6 ? 0 : -1;
}

int pic_ebird_parse(const char *js, size_t len, obs_t *out, int max)
{
    pic_json_t j;
    int n, i, c = 0, k;
    if (pic_json_parse_max(&j, js, len, 100000) < 0) return -1;
    if (!pic_json_is_array(&j, 0)) {
        pic_json_free(&j);
        return -1;
    }
    n = pic_json_size(&j, 0);
    for (i = 0; i < n && c < max; i++) {
        int e = pic_json_at(&j, 0, i), dup = 0;
        obs_t o;
        memset(&o, 0, sizeof(o));
        o.lat = (float)pic_json_num(&j, pic_json_get(&j, e, "lat"), 999);
        o.lon = (float)pic_json_num(&j, pic_json_get(&j, e, "lng"), 999);
        pic_json_str(&j, pic_json_get(&j, e, "comName"), o.species, sizeof(o.species));
        pic_json_str(&j, pic_json_get(&j, e, "locName"), o.loc, sizeof(o.loc));
        pic_json_str(&j, pic_json_get(&j, e, "obsDt"), o.when, sizeof(o.when));
        if (fabsf(o.lat) > 90 || !o.species[0]) continue;
        /* The feed repeats a species for every checklist: keep one
         * per species and location */
        for (k = 0; k < c && !dup; k++)
            dup = !strcmp(out[k].species, o.species) &&
                  fabsf(out[k].lat - o.lat) < 0.02f && fabsf(out[k].lon - o.lon) < 0.02f;
        if (!dup) out[c++] = o;
    }
    pic_json_free(&j);
    return c;
}

static int ebird_work(pic_feature_t *f)
{
    static obs_t tmp[MAX_OBS];
    char key[64], hdr[96], url[256], *buf;
    int n, radius, days;

    (void)f;
    if (read_key(key, sizeof(key)) != 0) {
        pthread_mutex_lock(&eb.mutex);
        eb.status = 1;
        eb.n = 0;
        pthread_mutex_unlock(&eb.mutex);
        return 60;
    }
    pthread_mutex_lock(&eb.mutex);
    radius = eb.radius;
    days = eb.days;
    pthread_mutex_unlock(&eb.mutex);
    snprintf(url, sizeof(url), EBIRD_FMT, pic_config.qth_lat,
             pic_config.qth_lon, radius, days);
    snprintf(hdr, sizeof(hdr), "X-eBirdApiToken: %s", key);
    memset(key, 0, sizeof(key));
    buf = malloc(1024 * 1024);
    if (!buf) return 300;
    n = pic_fetch_to_buf_header(url, hdr, buf, 1024 * 1024 - 1);
    memset(hdr, 0, sizeof(hdr));
    if (n > 0) {
        buf[n] = '\0';
        n = pic_ebird_parse(buf, (size_t)n, tmp, MAX_OBS);
    }
    free(buf);
    pthread_mutex_lock(&eb.mutex);
    if (n >= 0) {
        memcpy(eb.o, tmp, (size_t)n * sizeof(obs_t));
        eb.n = n;
        eb.status = 0;
    } else {
        eb.status = 2;
    }
    pthread_mutex_unlock(&eb.mutex);
    printf("ebird: %d notable sightings\n", n);
    return n >= 0 ? 30 * 60 : 10 * 60;
}

static void ebird_reload(pic_feature_t *f)
{
    int r = pic_conf_int("EBIRD_RADIUS_KM", 50), d = pic_conf_int("EBIRD_DAYS", 3);
    if (r < 5) r = 5;
    if (r > 50) r = 50;
    if (d < 1) d = 1;
    if (d > 30) d = 30;
    pthread_mutex_lock(&eb.mutex);
    if (r != eb.radius || d != eb.days) pic_feature_poke(f);
    eb.radius = r;
    eb.days = d;
    if (eb.status) pic_feature_poke(f);   /* maybe a key was just added */
    pthread_mutex_unlock(&eb.mutex);
}

static void draw_bird(cairo_t *cr, double x, double y, double s)
{
    /* Simple "m"-shaped bird in flight */
    cairo_move_to(cr, x - s, y - s * 0.2);
    cairo_curve_to(cr, x - s * 0.6, y - s * 0.8, x - s * 0.2, y - s * 0.6, x, y);
    cairo_curve_to(cr, x + s * 0.2, y - s * 0.6, x + s * 0.6, y - s * 0.8, x + s, y - s * 0.2);
}

void pic_layer_render_ebird(cairo_t *cr, int width, int height, time_t now,
                            void *ud)
{
    double z = pic_zoom_factor(), s = height / 150.0 * (1 + 0.3 * (pic_symbol_zoom() - 1));
    double fs = height / 105.0;
    int i, k;

    (void)now; (void)ud;
    pthread_mutex_lock(&eb.mutex);
    if (eb.status == 1 || eb.status == 2) {      /* note: ebird_note() */
        pthread_mutex_unlock(&eb.mutex);
        return;
    }
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    for (i = 0; i < eb.n; i++) {
        const obs_t *o = &eb.o[i];
        double x = pic_lon_to_x(o->lon, width), y = pic_lat_to_y(o->lat, height);
        int first = 1, row = 0;
        if (x < -10 || x > width + 10 || y < -10 || y > height + 10) continue;
        /* One pin per location: skip if an earlier entry shares it */
        for (k = 0; k < i && first; k++)
            if (fabsf(eb.o[k].lat - o->lat) < 0.02f && fabsf(eb.o[k].lon - o->lon) < 0.02f)
                first = 0;
        if (!first) continue;
        cairo_new_path(cr);
        cairo_arc(cr, x, y, s * 0.9, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.1, 0.35, 0.2, 0.85);
        cairo_fill(cr);
        draw_bird(cr, x, y + s * 0.25, s * 0.6);
        cairo_set_source_rgba(cr, 0.75, 1.0, 0.6, 1);
        cairo_set_line_width(cr, s * 0.18);
        cairo_stroke(cr);
        if (z < 6) continue;
        /* Local view: list species seen here (up to 3) */
        for (k = i; k < eb.n && row < 3; k++) {
            if (fabsf(eb.o[k].lat - o->lat) >= 0.02f || fabsf(eb.o[k].lon - o->lon) >= 0.02f)
                continue;
            pic_draw_text_pill(cr, x + s * 1.3, y + fs * 0.35 + row * fs * 1.35,
                               eb.o[k].species, 0.8, 1.0, 0.7, 0.55);
            row++;
        }
    }
    pthread_mutex_unlock(&eb.mutex);
}

/* Key missing / rejected: a note in the bottom legend stack. Drawn as
 * a legend (each frame, in stack order) rather than inside the cached
 * layer, so it can't land on top of another legend. */
static void ebird_note(cairo_t *cr, int width, int height, time_t now, void *ud)
{
    int st;
    (void)now; (void)ud;
    pthread_mutex_lock(&eb.mutex);
    st = eb.status;
    pthread_mutex_unlock(&eb.mutex);
    if (st == 1 || st == 2)
        pic_draw_legend_line(cr, width, height, 5,
                             st == 1 ? "Rare birds: add an eBird API key in the dashboard"
                                     : "Rare birds: eBird rejected the API key",
                             1.0, 0.8, 0.5);
}

static pic_feature_t def = {
    .name = "ebird", .layer = pic_layer_render_ebird, .interval = 60,
    .legend = ebird_note,
    .extra_mb = 1, .reload = ebird_reload, .work = ebird_work,
    .needs_network = 1,
};

pic_feature_t *pic_feature_ebird(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&eb.mutex, NULL);
        eb.radius = 50;
        eb.days = 3;
        inited = 1;
    }
    return &def;
}
