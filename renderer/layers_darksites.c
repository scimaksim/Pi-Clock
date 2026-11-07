/*
 * layers_darksites.c - Nearby dark-sky observing sites, tonight
 *
 * A star-shaped pin for each site, coloured by tonight's average cloud
 * cover during astronomical darkness (green clear → grey cloudy), with
 * the site's estimated sky darkness (Bortle class, from the light-
 * pollution model) when zoomed in. A note names the best site tonight.
 *
 * Default sites are within ~2.5 h of Raleigh; DARK_SITES can replace
 * them: "Name:lat:lon,Name:lat:lon" (up to 10; ";" also accepted, "_"
 * in a name shows as a space). Clouds: Open-Meteo,
 * every hour (one request for all sites).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE  /* timegm, strtok_r */
#include "feature.h"
#include "astro.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SITES 10

double pic_lp_sqm_at(double lat, double lon);
int    pic_lp_bortle(double sqm);
int    pic_lp_ensure_home(void);

typedef struct {
    char   name[32];
    double lat, lon;
    int    cloud;       /* % average tonight, -1 unknown */
    double sqm;         /* NAN unknown */
} site_t;

static const site_t default_sites[] = {
    { "Jordan Lake",        35.736, -79.017, -1, 0 },
    { "Medoc Mountain SP",  36.264, -77.888, -1, 0 },
    { "Kerr Lake",          36.450, -78.370, -1, 0 },
    { "Uwharrie NF",        35.423, -79.996, -1, 0 },
    { "Staunton River SP",  36.696, -78.680, -1, 0 },
    { "Pettigrew SP",       35.788, -76.411, -1, 0 },
    { "Lake Mattamuskeet",  35.480, -76.180, -1, 0 },
};

static struct {
    pthread_mutex_t mutex;
    site_t s[MAX_SITES];
    int    n;
    time_t dark0, dark1;
} ds;

/* Tonight's astronomical-dark window at home */
static void dark_window(time_t now, time_t *a, time_t *b)
{
    double lat = pic_config.qth_lat, lon = pic_config.qth_lon;
    if (pic_astro_sun_alt(now, lat, lon) < -18) {
        *a = now;
    } else {
        *a = pic_astro_next_crossing(0, now, lat, lon, -18, 0, 30);
    }
    *b = *a ? pic_astro_next_crossing(0, *a + 60, lat, lon, -18, 1, 30) : 0;
}

static int sites_work(pic_feature_t *f)
{
    site_t s[MAX_SITES];
    char url[1024], lats[256] = "", lons[256] = "", *buf;
    int n, i, len;
    time_t now = time(NULL), d0, d1;
    pic_json_t j;

    (void)f;
    pthread_mutex_lock(&ds.mutex);
    n = ds.n;
    memcpy(s, ds.s, sizeof(s));
    pthread_mutex_unlock(&ds.mutex);
    if (n == 0) return 3600;

    /* Darkness from the light-pollution model (one-time fetch) */
    pic_lp_ensure_home();
    for (i = 0; i < n; i++) s[i].sqm = pic_lp_sqm_at(s[i].lat, s[i].lon);

    for (i = 0; i < n; i++) {
        size_t l = strlen(lats), m = strlen(lons);
        snprintf(lats + l, sizeof(lats) - l, "%s%.3f", i ? "," : "", s[i].lat);
        snprintf(lons + m, sizeof(lons) - m, "%s%.3f", i ? "," : "", s[i].lon);
    }
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&hourly=cloud_cover&forecast_days=2&timezone=GMT", lats, lons);
    buf = malloc(256 * 1024);
    if (!buf) return 600;
    len = pic_fetch_to_buf(url, buf, 256 * 1024 - 1);
    dark_window(now, &d0, &d1);
    if (len > 0 && pic_json_parse(&j, buf, (size_t)len) == 0) {
        /* One object per site when several coordinates are given;
         * a single object for one site */
        int arr = pic_json_is_array(&j, 0);
        for (i = 0; i < n; i++) {
            int o = arr ? pic_json_at(&j, 0, i) : (i == 0 ? 0 : -1);
            int hr = pic_json_get(&j, o, "hourly");
            int ts = pic_json_get(&j, hr, "time"), cc = pic_json_get(&j, hr, "cloud_cover");
            int k, cnt = 0;
            double sum = 0;
            for (k = 0; k < pic_json_size(&j, ts); k++) {
                char t[24];
                struct tm tm;
                time_t tt;
                memset(&tm, 0, sizeof(tm));
                pic_json_str(&j, pic_json_at(&j, ts, k), t, sizeof(t));
                if (sscanf(t, "%d-%d-%dT%d:%d", &tm.tm_year, &tm.tm_mon,
                           &tm.tm_mday, &tm.tm_hour, &tm.tm_min) != 5)
                    continue;
                tm.tm_year -= 1900;
                tm.tm_mon -= 1;
                tt = timegm(&tm);
                if (d0 && d1 && tt >= d0 - 1800 && tt <= d1) {
                    sum += pic_json_num(&j, pic_json_at(&j, cc, k), 0);
                    cnt++;
                }
            }
            s[i].cloud = cnt ? (int)(sum / cnt + 0.5) : -1;
        }
        pic_json_free(&j);
    }
    free(buf);

    pthread_mutex_lock(&ds.mutex);
    if (ds.n == n) memcpy(ds.s, s, sizeof(s));
    ds.dark0 = d0;
    ds.dark1 = d1;
    pthread_mutex_unlock(&ds.mutex);
    return 3600;
}

static void sites_reload(pic_feature_t *f)
{
    char cfg[512] = "", *p, *save = NULL;
    site_t s[MAX_SITES];
    int n = 0, i, changed;

    if (pic_conf_str("DARK_SITES", cfg, sizeof(cfg)) && cfg[0]) {
        for (p = strtok_r(cfg, ",;", &save); p && n < MAX_SITES; p = strtok_r(NULL, ",;", &save)) {
            char name[32];
            double la, lo;
            if (sscanf(p, "%31[^:]:%lf:%lf", name, &la, &lo) == 3 &&
                fabs(la) <= 90 && fabs(lo) <= 180) {
                memset(&s[n], 0, sizeof(s[n]));
                char *u;
                snprintf(s[n].name, sizeof(s[n].name), "%s", name);
                for (u = s[n].name; *u; u++)
                    if (*u == '_') *u = ' ';
                s[n].lat = la;
                s[n].lon = lo;
                s[n].cloud = -1;
                s[n].sqm = NAN;
                n++;
            }
        }
    }
    if (n == 0) {
        n = (int)(sizeof(default_sites) / sizeof(default_sites[0]));
        memcpy(s, default_sites, (size_t)n * sizeof(site_t));
        for (i = 0; i < n; i++) s[i].sqm = NAN;
    }
    pthread_mutex_lock(&ds.mutex);
    changed = n != ds.n;
    for (i = 0; i < n && !changed; i++)
        changed = strcmp(s[i].name, ds.s[i].name) || s[i].lat != ds.s[i].lat;
    if (changed) {
        memcpy(ds.s, s, (size_t)n * sizeof(site_t));
        ds.n = n;
        pic_feature_poke(f);
    }
    pthread_mutex_unlock(&ds.mutex);
}

static void star(cairo_t *cr, double x, double y, double r)
{
    int k;
    for (k = 0; k < 10; k++) {
        double a = -M_PI / 2 + k * M_PI / 5, rr = (k & 1) ? r * 0.45 : r;
        if (k == 0) cairo_move_to(cr, x + rr * cos(a), y + rr * sin(a));
        else cairo_line_to(cr, x + rr * cos(a), y + rr * sin(a));
    }
    cairo_close_path(cr);
}

void pic_layer_render_darksites(cairo_t *cr, int width, int height,
                                time_t now, void *ud)
{
    double s = height / 120.0 * (1 + 0.3 * (pic_symbol_zoom() - 1)), fs = height / 95.0;
    int labels = pic_zoom_factor() >= 6, i;

    (void)now; (void)ud;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    pthread_mutex_lock(&ds.mutex);
    for (i = 0; i < ds.n; i++) {
        const site_t *t = &ds.s[i];
        double x = pic_lon_to_x(t->lon, width), y = pic_lat_to_y(t->lat, height);
        double r, g, b;
        char lab[96], extra[48] = "";
        if (x < -10 || x > width + 10 || y < -10 || y > height + 10) continue;
        if (t->cloud < 0)       { r = 0.8;  g = 0.8;  b = 0.9; }
        else if (t->cloud < 20) { r = 0.4;  g = 1.0;  b = 0.5; }
        else if (t->cloud < 50) { r = 1.0;  g = 0.85; b = 0.3; }
        else                    { r = 0.6;  g = 0.6;  b = 0.65; }
        star(cr, x, y, s);
        cairo_set_source_rgba(cr, r, g, b, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
        cairo_set_line_width(cr, s * 0.15);
        cairo_stroke(cr);
        if (!labels) continue;
        if (!isnan(t->sqm) && pic_lp_bortle(t->sqm))
            snprintf(extra, sizeof(extra), " \xc2\xb7 Bortle %d", pic_lp_bortle(t->sqm));
        if (t->cloud >= 0) {
            size_t l = strlen(extra);
            snprintf(extra + l, sizeof(extra) - l, " \xc2\xb7 %d%% cloud", t->cloud);
        }
        snprintf(lab, sizeof(lab), "%s%s", t->name, extra);
        pic_draw_text_pill(cr, x + s * 1.2, y + fs * 0.35, lab,
                           (r + 1) / 2, (g + 1) / 2, (b + 1) / 2, 0.55);
    }
    pthread_mutex_unlock(&ds.mutex);
}

/* "Best dark site tonight: Medoc Mountain SP (Bortle 3, 5% cloud)" */
void pic_layer_render_darksites_note(cairo_t *cr, int width, int height,
                                     time_t now, void *ud)
{
    int i, best = -1;
    double best_score = 1e9;
    char t[128], when[16];

    (void)ud;
    pthread_mutex_lock(&ds.mutex);
    if (!ds.dark0 || now > ds.dark1) {
        pthread_mutex_unlock(&ds.mutex);
        return;
    }
    for (i = 0; i < ds.n; i++) {
        const site_t *s = &ds.s[i];
        double score;
        if (s->cloud < 0) continue;
        /* Clouds dominate; darkness breaks ties (1 Bortle ≈ 10% cloud) */
        score = s->cloud + (isnan(s->sqm) ? 50 : pic_lp_bortle(s->sqm) * 10);
        if (score < best_score) { best_score = score; best = i; }
    }
    if (best >= 0) {
        const site_t *s = &ds.s[best];
        char dark[32], bt[16] = "";
        pic_fmt_hm(when, sizeof(when), ds.dark0 > now ? ds.dark0 : ds.dark1);
        snprintf(dark, sizeof(dark), ds.dark0 > now ? "dark from %s" : "dark until %s", when);
        if (!isnan(s->sqm) && pic_lp_bortle(s->sqm))
            snprintf(bt, sizeof(bt), "Bortle %d, ", pic_lp_bortle(s->sqm));
        snprintf(t, sizeof(t), "Best dark site tonight: %s (%s%d%% cloud) \xc2\xb7 %s",
                 s->name, bt, s->cloud, dark);
    }
    pthread_mutex_unlock(&ds.mutex);
    if (best >= 0) pic_draw_legend_line(cr, width, height, 7, t, 0.75, 1.0, 0.8);
}

static pic_feature_t def = {
    .name = "darksites", .layer = pic_layer_render_darksites,
    .legend = pic_layer_render_darksites_note, .interval = 60,
    .extra_mb = 2, .reload = sites_reload, .work = sites_work,
    .needs_network = 1,
};

pic_feature_t *pic_feature_darksites(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&ds.mutex, NULL);
        inited = 1;
    }
    return &def;
}
