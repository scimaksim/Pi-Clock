/*
 * layers_aurorawatch.c - Aurora alerts for mid-latitude homes
 *
 * Map layer: NOAA SWPC's aurora "view line" (red) — the furthest-south
 * place the aurora may be seen on the northern horizon right now —
 * computed from the live OVATION nowcast the way SWPC does it
 * (Case et al. 2016, Space Weather 14:198):
 *
 *     view line = lat(P_max) - (P_max / 20 + 3)   for each longitude
 *
 * plus a dotted "photo line" AURORA_MARGIN degrees further south
 * (default 3), since citizen-science reports show aurora is often
 * photographed well equatorward of SWPC's line. When home is at night
 * and north of either line — or the 3-day Kp forecast says it will
 * be — a banner appears.
 *
 * Applet "Aurora Watch": current Kp, the Kp your QTH needs, a 3-day
 * Kp bar chart with night-time shaded, and the next chance with
 * cloud cover from Astrospheric when the Tonight's Sky panel has a
 * forecast (no extra API credits are used).
 *
 * The Kp each home needs comes from its geomagnetic latitude (IGRF
 * dipole pole 80.8°N 72.6°W) and a simple fit to where storms have
 * been seen: the view line sits near 65 - 2.2 * Kp degrees magnetic.
 * For central North Carolina that's about Kp 8 for photos and Kp 9
 * by eye, matching the May and October 2024 storms.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE  /* timegm */
#include "feature.h"
#include "features_all.h"
#include "astro.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OVATION_URL "https://services.swpc.noaa.gov/json/ovation_aurora_latest.json"
#define KPF_URL     "https://services.swpc.noaa.gov/products/noaa-planetary-k-index-forecast.json"
#define KP1M_URL    "https://services.swpc.noaa.gov/json/planetary_k_index_1m.json"
#define MAX_KPF     80
#define POLE_LAT    80.8
#define POLE_LON    (-72.6)

typedef struct { time_t t; float kp; int predicted; } kpf_t;

typedef struct {
    pthread_mutex_t mutex;
    float  vl_n[360], vl_s[360];  /* View line lat by lon (999 = none) */
    float  pmax_n[360];
    int    have_ovation;
    time_t ov_at;              /* when the oval was last fetched */
    kpf_t  kpf[MAX_KPF];
    int    nkpf;
    float  kp_now;
    int    margin;                /* Photo line offset, degrees        */
    int    banner;
    time_t fetched;
} aw_t;

static aw_t aw;

double pic_aurora_mag_lat(double lat, double lon)
{
    double r = M_PI / 180;
    return asin(sin(lat * r) * sin(POLE_LAT * r) +
                cos(lat * r) * cos(POLE_LAT * r) * cos((lon - POLE_LON) * r)) / r;
}

/* Kp at which the view line reaches magnetic latitude mlat */
static double kp_needed(double mlat, double margin)
{
    double k = (65.0 - margin - mlat) / 2.2;
    return k < 0 ? 0 : k;
}

/*
 * Parse the OVATION grid and derive view lines. grid[lat+90][lon]
 * holds probability 0-100. Returns 0 on success.
 */
int pic_aurora_viewline(const char *js, float vl_n[360], float vl_s[360],
                        float pmax_n[360])
{
    static unsigned char grid[181][360];
    const char *p = strstr(js, "\"coordinates\"");
    int lon, n = 0;
    float tmp[360];

    if (!p) return -1;
    memset(grid, 0, sizeof(grid));
    while ((p = strchr(p, '[')) != NULL) {
        char *e;
        double a, b, c;
        p++;
        if (*p == '[') continue;
        a = strtod(p, &e); if (e == p || *e != ',') continue;
        p = e + 1; b = strtod(p, &e); if (*e != ',') continue;
        p = e + 1; c = strtod(p, &e); if (*e != ']') continue;
        p = e;
        if (a >= 0 && a < 360 && b >= -90 && b <= 90) {
            grid[(int)b + 90][(int)a] = (unsigned char)(c < 0 ? 0 : c > 100 ? 100 : c);
            n++;
        }
    }
    if (n < 10000) return -1;

    for (lon = 0; lon < 360; lon++) {
        int lat, best = -1, bs = -1, pm = 0, ps = 0;
        for (lat = 0; lat <= 90; lat++)
            if (grid[lat + 90][lon] > pm) { pm = grid[lat + 90][lon]; best = lat; }
        for (lat = -90; lat <= 0; lat++)
            if (grid[lat + 90][lon] > ps) { ps = grid[lat + 90][lon]; bs = lat; }
        vl_n[lon] = best >= 0 && pm >= 3 ? (float)(best - (pm / 20.0 + 3)) : 999;
        vl_s[lon] = bs < 0 && ps >= 3 ? (float)(bs + (ps / 20.0 + 3)) : 999;
        pmax_n[lon] = (float)pm;
    }
    /* Light smoothing along longitude (the model grid is noisy) */
    {
        float *lines[2] = { vl_n, vl_s };
        int h;
        for (h = 0; h < 2; h++) {
            for (lon = 0; lon < 360; lon++) {
                int k, c = 0;
                float s = 0;
                for (k = -4; k <= 4; k++) {
                    float v = lines[h][(lon + k + 360) % 360];
                    if (v < 900) { s += v; c++; }
                }
                tmp[lon] = lines[h][lon] < 900 && c ? s / c : 999;
            }
            memcpy(lines[h], tmp, sizeof(tmp));
        }
    }
    return 0;
}

int pic_aurora_parse_kpf(const char *js, size_t len, kpf_t *out, int max)
{
    pic_json_t j;
    int i, n, c = 0;
    if (pic_json_parse(&j, js, len) < 0) return -1;
    n = pic_json_size(&j, 0);
    for (i = 0; i < n && c < max; i++) {
        int row = pic_json_at(&j, 0, i);
        char t[32], obs[16];
        struct tm tm;
        if (!pic_json_is_object(&j, row)) continue;   /* old array format */
        memset(&tm, 0, sizeof(tm));
        pic_json_str(&j, pic_json_get(&j, row, "time_tag"), t, sizeof(t));
        pic_json_str(&j, pic_json_get(&j, row, "observed"), obs, sizeof(obs));
        if (sscanf(t, "%d-%d-%dT%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min) != 5)
            continue;
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        out[c].t = timegm(&tm);
        out[c].kp = (float)pic_json_num(&j, pic_json_get(&j, row, "kp"), 0);
        out[c].predicted = strcmp(obs, "predicted") == 0;
        c++;
    }
    pic_json_free(&j);
    return c;
}

static int aw_work(pic_feature_t *f)
{
    static float vn[360], vs[360], pm[360];
    static kpf_t kpf[MAX_KPF];
    char *buf = malloc(2 * 1024 * 1024);
    int n, ok_ov = 0, nk = -1;
    float kp_now = -1;

    (void)f;
    if (!buf) return 300;
    n = pic_fetch_to_buf(OVATION_URL, buf, 2 * 1024 * 1024 - 1);
    if (n > 0) {
        buf[n] = '\0';
        ok_ov = pic_aurora_viewline(buf, vn, vs, pm) == 0;
    }
    n = pic_fetch_to_buf(KPF_URL, buf, 256 * 1024);
    if (n > 0) {
        buf[n] = '\0';
        nk = pic_aurora_parse_kpf(buf, (size_t)n, kpf, MAX_KPF);
    }
    n = pic_fetch_to_buf(KP1M_URL, buf, 256 * 1024);
    if (n > 0) {
        /* Last "estimated_kp" in the 1-minute series */
        char *p, *last = NULL;
        buf[n] = '\0';
        for (p = buf; (p = strstr(p, "\"estimated_kp\":")) != NULL; p++) last = p;
        if (last) kp_now = (float)atof(last + 15);
    }
    free(buf);

    pthread_mutex_lock(&aw.mutex);
    if (ok_ov) {
        memcpy(aw.vl_n, vn, sizeof(vn));
        memcpy(aw.vl_s, vs, sizeof(vs));
        memcpy(aw.pmax_n, pm, sizeof(pm));
        aw.have_ovation = 1;
        aw.ov_at = time(NULL);
    }
    if (nk > 0) {
        memcpy(aw.kpf, kpf, (size_t)nk * sizeof(kpf_t));
        aw.nkpf = nk;
    }
    if (kp_now >= 0) aw.kp_now = kp_now;
    aw.fetched = time(NULL);
    pthread_mutex_unlock(&aw.mutex);
    printf("aurorawatch: ovation=%d kpf=%d kp=%.2f\n", ok_ov, nk, kp_now);
    return 10 * 60;   /* OVATION updates every ~5 minutes */
}

static void aw_reload(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&aw.mutex);
    aw.margin = pic_conf_int("AURORA_MARGIN", 3);
    if (aw.margin < 0 || aw.margin > 8) aw.margin = 3;
    aw.banner = pic_conf_int("AURORA_BANNER", 1);
    pthread_mutex_unlock(&aw.mutex);
}

/* Average Astrospheric cloud cover over [t0, t1), or -1 if unknown */
static int clouds_between(time_t t0, time_t t1)
{
    pic_astro_t *a = pic_astro_shared;
    double s = 0;
    int i, c = 0;
    if (!a) return -1;
    pthread_mutex_lock(&a->mutex);
    for (i = 0; i < a->nhours; i++)
        if (a->hours[i].t >= t0 - 1800 && a->hours[i].t < t1) {
            s += a->hours[i].cloud;
            c++;
        }
    pthread_mutex_unlock(&a->mutex);
    return c ? (int)(s / c + 0.5) : -1;
}

static int is_dark(time_t t, double lat, double lon)
{
    return pic_astro_sun_alt(t, lat, lon) < -12;
}

/*
 * Status at home. *now: 2 = view line, 1 = photo line, 0 = no.
 * Fills the first forecast night block with Kp above the photo need.
 * Call with aw.mutex held.
 */
static void home_status(time_t now, int *status_now, double *vl_here,
                        const kpf_t **next, int *next_level)
{
    double lat = pic_config.qth_lat, lon = pic_config.qth_lon;
    double mlat = pic_aurora_mag_lat(lat, lon);
    double need_photo = kp_needed(mlat, aw.margin), need_eye = kp_needed(mlat, 0);
    int li = ((int)floor(lon) % 360 + 360) % 360, i;

    *status_now = 0;
    /* "now" needs an oval from the last hour, not an old file */
    *vl_here = aw.have_ovation && !pic_data_stale(aw.ov_at, now, 3600) ? aw.vl_n[li] : 999;
    if (lat >= 0 && *vl_here < 900 && is_dark(now, lat, lon)) {
        if (lat >= *vl_here) *status_now = 2;
        else if (lat >= *vl_here - aw.margin) *status_now = 1;
    }
    *next = NULL;
    *next_level = 0;
    for (i = 0; i < aw.nkpf; i++) {
        const kpf_t *k = &aw.kpf[i];
        if (k->t + 3 * 3600 < now || k->t > now + 72 * 3600) continue;
        if (!is_dark(k->t + 5400, lat, lon) && !is_dark(k->t, lat, lon) &&
            !is_dark(k->t + 3 * 3600 - 1, lat, lon))
            continue;
        if (k->kp >= need_photo) {
            *next = k;
            *next_level = k->kp >= need_eye ? 2 : 1;
            return;
        }
    }
}

void pic_layer_render_aurorawatch(cairo_t *cr, int width, int height,
                                  time_t now, void *ud)
{
    double lw = height / 500.0;
    int pass, hemi;

    (void)now; (void)ud;
    pthread_mutex_lock(&aw.mutex);
    if (!aw.have_ovation) {
        pthread_mutex_unlock(&aw.mutex);
        return;
    }
    /* Pass 0: photo line (dotted), pass 1: SWPC view line (solid) */
    for (pass = 0; pass < 2; pass++)
        for (hemi = 0; hemi < 2; hemi++) {
            double off = pass ? 0 : aw.margin;   /* toward the equator */
            double prevx = 0;
            int lon, pen = 0;
            const float *vl = hemi ? aw.vl_s : aw.vl_n;
            cairo_new_path(cr);
            for (lon = -180; lon <= 180; lon++) {
                float v = vl[(lon + 360) % 360];
                double x, y;
                if (v > 900) { pen = 0; continue; }
                x = pic_lon_to_x(lon, width);
                y = pic_lat_to_y(hemi ? v + off : v - off, height);
                if (pen && fabs(x - prevx) < width / 2.0) cairo_line_to(cr, x, y);
                else cairo_move_to(cr, x, y);
                prevx = x;
                pen = 1;
            }
            if (pass) {
                double d[1] = { 0 };
                cairo_set_dash(cr, d, 0, 0);
                cairo_set_source_rgba(cr, 1.0, 0.25, 0.25, 0.85);
                cairo_set_line_width(cr, lw * 1.6);
            } else {
                double d[2] = { lw * 2, lw * 4 };
                cairo_set_dash(cr, d, 2, 0);
                cairo_set_source_rgba(cr, 1.0, 0.55, 0.55, 0.6);
                cairo_set_line_width(cr, lw);
            }
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_stroke(cr);
        }
    cairo_set_dash(cr, NULL, 0, 0);
    pthread_mutex_unlock(&aw.mutex);
}

/* Banner when aurora is possible from home now or in the next 24 h */
void pic_layer_render_aurorawatch_banner(cairo_t *cr, int width, int height,
                                         time_t now, void *ud)
{
    int st, lvl, cl;
    double vl;
    const kpf_t *nx;
    char text[192], when[24];

    (void)ud;
    pthread_mutex_lock(&aw.mutex);
    if (aw.have_ovation) {
        pic_map_key_add("Aurora seen on horizon (SWPC)", 1.0, 0.25, 0.25, 0.85, 0);
        pic_map_key_add("Aurora in photos", 1.0, 0.55, 0.55, 0.6, 1);
    }
    if (!aw.banner) {
        pthread_mutex_unlock(&aw.mutex);
        return;
    }
    home_status(now, &st, &vl, &nx, &lvl);
    if (st) {
        cl = clouds_between(now, now + 3600);
        snprintf(text, sizeof(text), "AURORA NOW: %s from home \xc2\xb7 Kp %.1f",
                 st == 2 ? "may be visible low in the north" : "possible in long-exposure photos (look north)",
                 aw.kp_now);
        if (cl >= 0) {
            size_t l = strlen(text);
            snprintf(text + l, sizeof(text) - l, " \xc2\xb7 clouds %d%%", cl);
        }
        pthread_mutex_unlock(&aw.mutex);
        pic_draw_legend_line(cr, width, height, 3, text, 1.0, 0.55, 0.85);
        return;
    }
    if (nx && nx->t < now + 24 * 3600) {
        cl = clouds_between(nx->t, nx->t + 3 * 3600);
        pic_fmt_when(when, sizeof(when), now, nx->t);
        snprintf(text, sizeof(text), "AURORA WATCH: Kp %.0f forecast %s \xc2\xb7 %s from home",
                 nx->kp, when, lvl == 2 ? "may be visible" : "possible in photos");
        if (cl >= 0) {
            size_t l = strlen(text);
            snprintf(text + l, sizeof(text) - l, " \xc2\xb7 clouds %d%%", cl);
        }
        pthread_mutex_unlock(&aw.mutex);
        pic_draw_legend_line(cr, width, height, 3, text, 0.55, 1.0, 0.7);
        return;
    }
    pthread_mutex_unlock(&aw.mutex);
}

static void kp_colour(cairo_t *cr, double kp)
{
    if (kp >= 7)      cairo_set_source_rgba(cr, 1.0, 0.25, 0.3, 0.95);
    else if (kp >= 6) cairo_set_source_rgba(cr, 1.0, 0.55, 0.2, 0.95);
    else if (kp >= 5) cairo_set_source_rgba(cr, 1.0, 0.85, 0.2, 0.95);
    else              cairo_set_source_rgba(cr, 0.35, 0.85, 0.45, 0.9);
}

double pic_applet_render_aurorawatch(cairo_t *cr, double width, time_t now,
                                     void *ud)
{
    double fs = width / 17.0, line_h = fs * 1.35, chart_h = fs * 3.0;
    double total = pic_applet_title(NULL, width, "") + fs * 0.3 +
                   line_h * 2 + chart_h + fs * 1.2 + line_h;
    double lat = pic_config.qth_lat, lon = pic_config.qth_lon;
    double mlat = pic_aurora_mag_lat(lat, lon), y, vl;
    const kpf_t *nx;
    int st, lvl, i, nbars = 0;
    char line[160], when[24];
    time_t t0;

    (void)ud;
    if (!cr) return total;
    y = pic_applet_title(cr, width, "Aurora Watch") + fs * 0.3;

    pthread_mutex_lock(&aw.mutex);
    home_status(now, &st, &vl, &nx, &lvl);

    cairo_set_font_size(cr, fs);
    if (vl < 900)
        snprintf(line, sizeof(line), "Kp %.1f now \xc2\xb7 view line %.0f\xc2\xb0N",
                 aw.kp_now, vl);
    else
        snprintf(line, sizeof(line), "Kp %.1f now", aw.kp_now);
    kp_colour(cr, aw.kp_now);
    cairo_move_to(cr, 0, y + fs);
    cairo_show_text(cr, line);
    y += line_h;

    {
        long kp_p = lround(kp_needed(mlat, aw.margin)), kp_e = lround(kp_needed(mlat, 0));
        snprintf(line, sizeof(line), "Home needs Kp %ld%s (photo) \xc2\xb7 %ld%s (eye)",
                 kp_p > 9 ? 9 : kp_p, kp_p > 9 ? "+" : "",
                 kp_e > 9 ? 9 : kp_e, kp_e > 9 ? "+" : "");
    }
    cairo_set_source_rgba(cr, 0.7, 0.75, 0.85, 0.9);
    cairo_set_font_size(cr, fs * 0.9);
    cairo_move_to(cr, 0, y + fs);
    cairo_show_text(cr, line);
    y += line_h;

    /* 3-day Kp bars: night shaded, dashed line at the photo threshold */
    t0 = now - (now % (3 * 3600));
    for (i = 0; i < aw.nkpf; i++) {
        const kpf_t *k = &aw.kpf[i];
        double bw = width / 24.0, x, h;
        int slot = (int)((k->t - t0) / (3 * 3600));
        if (slot < 0 || slot >= 24) continue;
        x = slot * bw;
        if (is_dark(k->t + 5400, lat, lon)) {
            cairo_set_source_rgba(cr, 0.3, 0.35, 0.6, 0.25);
            cairo_rectangle(cr, x, y, bw, chart_h);
            cairo_fill(cr);
        }
        h = k->kp / 9.0 * chart_h;
        kp_colour(cr, k->kp);
        cairo_rectangle(cr, x + 1, y + chart_h - h, bw - 2, h);
        cairo_fill(cr);
        nbars++;
    }
    {
        double ty = y + chart_h - kp_needed(mlat, aw.margin) / 9.0 * chart_h;
        double d[2] = { 3, 3 };
        if (ty < y) ty = y;
        cairo_set_dash(cr, d, 2, 0);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, 0, ty);
        cairo_line_to(cr, width, ty);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
    y += chart_h;
    cairo_set_font_size(cr, fs * 0.75);
    cairo_set_source_rgba(cr, 0.6, 0.62, 0.7, 0.9);
    for (i = 0; i < 3; i++) {
        struct tm lt;
        time_t t = t0 + (time_t)i * 86400;
        char d[8];
        localtime_r(&t, &lt);
        strftime(d, sizeof(d), "%a", &lt);
        cairo_move_to(cr, i * width / 3.0, y + fs * 0.9);
        cairo_show_text(cr, d);
    }
    y += fs * 1.2;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs * 0.95);
    if (st) {
        snprintf(line, sizeof(line), "%s now \xe2\x80\x94 look north!",
                 st == 2 ? "Visible" : "Photo chance");
        cairo_set_source_rgba(cr, 1.0, 0.55, 0.85, 1);
    } else if (nx) {
        int cl = clouds_between(nx->t, nx->t + 3 * 3600);
        pic_fmt_when(when, sizeof(when), now, nx->t);
        snprintf(line, sizeof(line), "%s: Kp %.0f %s", when, nx->kp,
                 lvl == 2 ? "(eye)" : "(photo)");
        if (cl >= 0) {
            size_t l = strlen(line);
            snprintf(line + l, sizeof(line) - l, " \xc2\xb7 clouds %d%%", cl);
        }
        cairo_set_source_rgba(cr, 0.55, 1.0, 0.7, 1);
    } else {
        snprintf(line, sizeof(line), nbars ? "No chance forecast for 3 days"
                                           : "Loading\xe2\x80\xa6");
        cairo_set_source_rgba(cr, 0.65, 0.68, 0.75, 0.9);
    }
    pic_fit_text(cr, line, width);
    cairo_move_to(cr, 0, y + fs);
    cairo_show_text(cr, line);
    pthread_mutex_unlock(&aw.mutex);
    return total;
}

void pic_aurora_home(time_t now, pic_aurora_home_t *o)
{
    const kpf_t *nx;
    double vl;
    memset(o, 0, sizeof(*o));
    pthread_mutex_lock(&aw.mutex);
    o->have_data = aw.have_ovation || aw.nkpf > 0;
    o->kp_now = aw.kp_now;
    home_status(now, &o->now_level, &vl, &nx, &o->next_level);
    if (nx) {
        o->next_t = nx->t;
        o->next_kp = nx->kp;
    }
    pthread_mutex_unlock(&aw.mutex);
    o->clouds_now = clouds_between(now, now + 3600);
    o->clouds_next = o->next_t ? clouds_between(o->next_t, o->next_t + 3 * 3600) : -1;
}

static int aw_wanted(pic_feature_t *f)
{
    (void)f;
    return pic_hanotify_wants("HA_NOTIFY_AURORA");
}

static pic_feature_t feature_def = {
    .name = "aurorawatch", .wanted = aw_wanted, .layer = pic_layer_render_aurorawatch,
    .legend = pic_layer_render_aurorawatch_banner, .interval = 60,
    .extra_mb = 2,
    .applet = pic_applet_render_aurorawatch, .label = "Aurora Watch",
    .applet_w = 220, .side = APPLET_SIDE_RIGHT,
    .reload = aw_reload, .work = aw_work, .needs_network = 1,
};

pic_feature_t *pic_feature_aurorawatch(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&aw.mutex, NULL);
        aw.margin = 3;
        aw.banner = 1;
        aw.kp_now = 0;
        inited = 1;
    }
    return &feature_def;
}
