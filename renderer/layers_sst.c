/*
 * layers_sst.c - Sea-surface temperature
 *
 * A subtle colour wash over the oceans from NASA JPL's MUR 0.25° daily
 * analysis (GHRSST L4 MUR25, via GIBS): cold water in blues, mild in
 * teal and green, warm in yellow through red. Warm currents such as
 * the Gulf Stream stand out as a tongue of warm colour running off the
 * coast, and sharp temperature fronts (the Gulf Stream's north wall)
 * are traced as a faint light edge. A legend gives the scale in °F, the water temperature nearest
 * home and the warmest water within 250 miles.
 *
 * The GIBS image is coloured with a published palette; each pixel is
 * turned back into a temperature through that palette (exact RGB
 * match) and recoloured here. The analysis appears with about a day's
 * delay, and GIBS now and then serves a day with a block missing, so
 * the last three days are fetched (as eight pieces each) and merged
 * newest-first. Refreshed every six hours; the data changes daily.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "config.h"
#include "fetch.h"
#include "layer.h"
#include "stb_image.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SST_URL \
    "https://gibs.earthdata.nasa.gov/wms/epsg4326/best/wms.cgi" \
    "?SERVICE=WMS&REQUEST=GetMap&VERSION=1.1.1" \
    "&LAYERS=GHRSST_L4_MUR25_Sea_Surface_Temperature" \
    "&STYLES=&SRS=EPSG:4326&BBOX=%d,%d,%d,%d&WIDTH=%d&HEIGHT=%d" \
    "&FORMAT=image/png&TRANSPARENT=TRUE&TIME=%s"
#define SST_TMP "/tmp/pi-clock-sst.png"
#define SST_W   1440            /* 0.25° grid, cells from -180, +90  */
#define SST_H   720
#define SST_ALPHA 0.34
#define FRONT_C   0.9     /* °C per cell where a front starts   */
#define FRONT_ALPHA 0.30
#define NEAR_MI 250.0

static const struct { unsigned char r, g, b; float c; } cmap[] = {
#include "sst_cmap.inc"
};
#define NCMAP ((int)(sizeof(cmap) / sizeof(cmap[0])))

static struct {
    pthread_mutex_t  mutex;
    cairo_surface_t *image;     /* SST_W x SST_H ARGB32 wash          */
    unsigned char   *idx;       /* 0 = no data, else cmap index + 1   */
    char   date[16];            /* newest analysis day used           */
    double near_f, warm_f;      /* °F; NAN when no ocean near home    */
    double qlat, qlon;
} sst;

/* RGB → palette index + 1, via a sorted key table */
static uint32_t keys[NCMAP];
static unsigned char keyidx[NCMAP];

static int key_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void build_keys(void)
{
    uint32_t tmp[NCMAP][2];
    int i;
    for (i = 0; i < NCMAP; i++) {
        tmp[i][0] = ((uint32_t)cmap[i].r << 16) | ((uint32_t)cmap[i].g << 8) | cmap[i].b;
        tmp[i][1] = (uint32_t)i;
    }
    qsort(tmp, NCMAP, sizeof(tmp[0]), key_cmp);
    for (i = 0; i < NCMAP; i++) {
        keys[i] = tmp[i][0];
        keyidx[i] = (unsigned char)tmp[i][1];
    }
}

static int lookup(unsigned r, unsigned g, unsigned b)
{
    uint32_t k = (r << 16) | (g << 8) | b;
    int lo = 0, hi = NCMAP - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (keys[m] == k) return keyidx[m] + 1;
        if (keys[m] < k) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

static double c_to_f(double c) { return c * 9.0 / 5.0 + 32.0; }

/* Display colour for a temperature in °F (straight RGB) */
void pic_sst_colour(double f, double *r, double *g, double *b)
{
    static const struct { double f, r, g, b; } st[] = {
        { 32, 0.42, 0.30, 0.80 },
        { 45, 0.20, 0.45, 0.95 },
        { 55, 0.15, 0.75, 0.85 },
        { 65, 0.35, 0.85, 0.45 },
        { 74, 0.95, 0.88, 0.30 },
        { 81, 1.00, 0.60, 0.22 },
        { 88, 0.90, 0.28, 0.25 },
    };
    int n = (int)(sizeof(st) / sizeof(st[0])), i;
    double t;
    if (f <= st[0].f) { *r = st[0].r; *g = st[0].g; *b = st[0].b; return; }
    for (i = 1; i < n && f > st[i].f; i++) ;
    if (i == n) { *r = st[n - 1].r; *g = st[n - 1].g; *b = st[n - 1].b; return; }
    t = (f - st[i - 1].f) / (st[i].f - st[i - 1].f);
    *r = st[i - 1].r + (st[i].r - st[i - 1].r) * t;
    *g = st[i - 1].g + (st[i].g - st[i - 1].g) * t;
    *b = st[i - 1].b + (st[i].b - st[i - 1].b) * t;
}

/* Fetch one 90° piece for one day into idx (where still empty).
 * Returns the number of pixels filled, or -1 on failure. */
static int fetch_quad(const char *day, int q, unsigned char *idx)
{
    /* Eight 90x90° pieces: small requests stay well inside the
     * download time limit even when GIBS renders them from scratch */
    int lon0 = -180 + 90 * (q % 4), lat1 = q < 4 ? 90 : 0;
    int qw = SST_W / 4, qh = SST_H / 2, w, h, ch, x, y, n = 0;
    int ox = qw * (q % 4), oy = q < 4 ? 0 : qh;
    char url[512];
    unsigned char *px;

    snprintf(url, sizeof(url), SST_URL, lon0, lat1 - 90, lon0 + 90, lat1,
             qw, qh, day);
    if (pic_fetch_to_file(url, SST_TMP, 90) != 0) {
        unlink(SST_TMP);
        return -1;
    }
    px = stbi_load(SST_TMP, &w, &h, &ch, 4);
    unlink(SST_TMP);
    if (!px) return -1;
    if (w != qw || h != qh) {
        stbi_image_free(px);
        return -1;
    }
    for (y = 0; y < qh; y++)
        for (x = 0; x < qw; x++) {
            const unsigned char *p = px + ((size_t)y * qw + x) * 4;
            unsigned char *d = idx + (size_t)(oy + y) * SST_W + ox + x;
            int k;
            if (*d || p[3] < 128) continue;
            k = lookup(p[0], p[1], p[2]);
            if (k) { *d = (unsigned char)k; n++; }
        }
    stbi_image_free(px);
    return n;
}

/* Water nearest home and the warmest within NEAR_MI (°F) */
static void near_home(const unsigned char *idx, double qlat, double qlon,
                      double *near_f, double *warm_f)
{
    double best = 1e9, warm = -1e9, lim = NEAR_MI * 1.609344;
    int x, y, nb = -1;
    int y0 = (int)((90 - qlat - 5) * 4), y1 = (int)((90 - qlat + 5) * 4);
    double cl = cos(qlat * M_PI / 180);
    int dx = (int)(5.0 / (cl > 0.2 ? cl : 0.2) * 4);
    int xc = (int)((qlon + 180) * 4);

    if (y0 < 0) y0 = 0;
    if (y1 > SST_H - 1) y1 = SST_H - 1;
    for (y = y0; y <= y1; y++)
        for (x = xc - dx; x <= xc + dx; x++) {
            int xx = (x % SST_W + SST_W) % SST_W, k = idx[(size_t)y * SST_W + xx];
            double la, lo, d;
            if (!k) continue;
            la = 90 - (y + 0.5) * 0.25;
            lo = -180 + (x + 0.5) * 0.25;
            d = pic_dist_km(qlat, qlon, la, lo);
            if (d > lim) continue;
            if (d < best) { best = d; nb = k; }
            if (cmap[k - 1].c > warm) warm = cmap[k - 1].c;
        }
    *near_f = nb > 0 ? c_to_f(cmap[nb - 1].c) : NAN;
    *warm_f = nb > 0 ? c_to_f(warm) : NAN;
}

static int sst_work(pic_feature_t *f)
{
    unsigned char *idx = calloc((size_t)SST_W * SST_H, 1);
    char days[3][16], newest[16] = "";
    time_t now = time(NULL);
    cairo_surface_t *img;
    int d, q, total = 0;
    double nf, wf;

    if (!idx) return 600;
    for (d = 0; d < 3; d++) {
        time_t t = now - (time_t)(d + 1) * 86400;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(days[d], sizeof(days[d]), "%Y-%m-%d", &tm);
    }
    for (d = 0; d < 3; d++) {
        int got = 0;
        for (q = 0; q < 8; q++) {
            int n;
            if (pic_feature_interrupted(f)) { free(idx); return 1; }
            n = fetch_quad(days[d], q, idx);
            if (n > 0) got += n;
        }
        if (got > 0 && !newest[0]) memcpy(newest, days[d], sizeof(newest));
        total += got;
        /* An older day that filled almost nothing new means the newer
         * one was complete: no need to go further back */
        if (d > 0 && got < 3000 && total > 200000) break;
    }
    /* Roughly 600k ocean pixels at 0.25°; refuse an obviously broken
     * result and keep whatever was shown before */
    if (total < 200000) {
        printf("sst: fetch failed (%d px)\n", total);
        free(idx);
        return 1800;
    }

    img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, SST_W, SST_H);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
        free(idx);
        return 1800;
    }
    {
        uint32_t lut[NCMAP + 1];
        unsigned char *px;
        int stride, x, y, k;
        lut[0] = 0;
        for (k = 0; k < NCMAP; k++) {
            double r, g, b, a = SST_ALPHA;
            pic_sst_colour(c_to_f(cmap[k].c), &r, &g, &b);
            lut[k + 1] = ((uint32_t)(a * 255) << 24) |
                         ((uint32_t)(r * a * 255) << 16) |
                         ((uint32_t)(g * a * 255) << 8) | (uint32_t)(b * a * 255);
        }
        cairo_surface_flush(img);
        px = cairo_image_surface_get_data(img);
        stride = cairo_image_surface_get_stride(img);
        for (y = 0; y < SST_H; y++) {
            uint32_t *row = (uint32_t *)(px + (size_t)y * stride);
            for (x = 0; x < SST_W; x++) {
                const unsigned char *c = idx + (size_t)y * SST_W + x;
                double gx, gy, g, a;
                row[x] = lut[*c];
                /* Fronts (the Gulf Stream's north wall, the Kuroshio,
                 * upwelling edges): where water temperature changes
                 * by more than ~1 °C per 0.25° cell, lighten the wash
                 * so the current's edge reads as a soft bright line */
                if (!*c || x == 0 || x == SST_W - 1 || y == 0 || y == SST_H - 1 ||
                    !c[-1] || !c[1] || !c[-SST_W] || !c[SST_W])
                    continue;
                gx = (cmap[c[1] - 1].c - cmap[c[-1] - 1].c) / 2;
                gy = (cmap[c[SST_W] - 1].c - cmap[c[-SST_W] - 1].c) / 2;
                g = sqrt(gx * gx + gy * gy);
                if (g < FRONT_C) continue;
                a = (g - FRONT_C) / FRONT_C;
                if (a > 1) a = 1;
                a *= FRONT_ALPHA;
                {
                    /* "over" a translucent white on the premultiplied wash */
                    uint32_t p = row[x];
                    double pa = (p >> 24) / 255.0, pr = ((p >> 16) & 255) / 255.0;
                    double pg = ((p >> 8) & 255) / 255.0, pb = (p & 255) / 255.0;
                    pa = a + pa * (1 - a);
                    pr = a + pr * (1 - a);
                    pg = a + pg * (1 - a);
                    pb = a + pb * (1 - a);
                    row[x] = ((uint32_t)(pa * 255) << 24) | ((uint32_t)(pr * 255) << 16) |
                             ((uint32_t)(pg * 255) << 8) | (uint32_t)(pb * 255);
                }
            }
        }
        cairo_surface_mark_dirty(img);
    }
    near_home(idx, pic_config.qth_lat, pic_config.qth_lon, &nf, &wf);

    pthread_mutex_lock(&sst.mutex);
    if (sst.image) cairo_surface_destroy(sst.image);
    free(sst.idx);
    sst.image = img;
    sst.idx = idx;
    snprintf(sst.date, sizeof(sst.date), "%s", newest);
    sst.near_f = nf;
    sst.warm_f = wf;
    sst.qlat = pic_config.qth_lat;
    sst.qlon = pic_config.qth_lon;
    pthread_mutex_unlock(&sst.mutex);
    printf("sst: %s, %d px, near home %.0fF, warmest %.0fF\n", newest, total, nf, wf);
    return 6 * 3600;
}

/* ── Zoomed-in drawing ─────────────────────────────────────────────
 * At world scale a 0.25° cell is a pixel or two and the wash image is
 * drawn as is. Zoomed in, a cell becomes a block up to ~30 px across:
 * coasts turn into staircases and the colour spills onto land. Then
 * the view is redrawn from the temperatures themselves: bilinear
 * between cell centres, with the ocean edge where the interpolated
 * "ocean share" crosses one half (a smooth line through the coastal
 * cells) and the front lines narrowed to the ridge. */

static inline double cell_c(const unsigned char *idx, int x, int y)
{
    int k;
    if (y < 0 || y >= SST_H) return NAN;
    x = (x % SST_W + SST_W) % SST_W;
    k = idx[(size_t)y * SST_W + x];
    return k ? cmap[k - 1].c : NAN;
}

/* Front strength 0..1 at a cell (as in the world image) */
static double cell_front(const unsigned char *idx, int x, int y)
{
    double l = cell_c(idx, x - 1, y), r = cell_c(idx, x + 1, y);
    double u = cell_c(idx, x, y - 1), d = cell_c(idx, x, y + 1), g;
    if (isnan(l) || isnan(r) || isnan(u) || isnan(d)) return 0;
    g = sqrt((r - l) * (r - l) / 4 + (d - u) * (d - u) / 4);
    if (g < FRONT_C) return 0;
    g = (g - FRONT_C) / FRONT_C;
    return g > 1 ? 1 : g;
}

static double smooth01(double e0, double e1, double v)
{
    double t = (v - e0) / (e1 - e0);
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return t * t * (3 - 2 * t);
}

static cairo_surface_t *zoom_image(const unsigned char *idx, int w, int h)
{
    double span = pic_config.view_span_lon, span_lat = pic_config.view_span_lat;
    double lon0 = pic_config.view_center_lon - span / 2;
    double lat1 = pic_config.view_center_lat + span_lat / 2;
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    unsigned char *px;
    int stride, x, y;

    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
        return NULL;
    }
    cairo_surface_flush(img);
    px = cairo_image_surface_get_data(img);
    stride = cairo_image_surface_get_stride(img);
    for (y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(px + (size_t)y * stride);
        double lat = lat1 - (y + 0.5) * span_lat / h;
        double fy = (90 - lat) * 4 - 0.5;          /* cell-centre coords */
        int y0 = (int)floor(fy);
        double ty = fy - y0;
        for (x = 0; x < w; x++) {
            double lon = lon0 + (x + 0.5) * span / w;
            double fx = (lon + 180) * 4 - 0.5;
            int x0 = (int)floor(fx), i;
            double tx = fx - x0, wsum = 0, tsum = 0, fsum = 0, a, t, fr, r, g, b;
            static const int ox[4] = { 0, 1, 0, 1 }, oy[4] = { 0, 0, 1, 1 };
            for (i = 0; i < 4; i++) {
                double wt = (ox[i] ? tx : 1 - tx) * (oy[i] ? ty : 1 - ty);
                double c = cell_c(idx, x0 + ox[i], y0 + oy[i]);
                if (isnan(c)) continue;
                wsum += wt;
                tsum += wt * c;
                fsum += wt * cell_front(idx, x0 + ox[i], y0 + oy[i]);
            }
            if (wsum <= 0.001) { row[x] = 0; continue; }
            a = smooth01(0.5, 0.75, wsum) * SST_ALPHA;
            if (a <= 0) { row[x] = 0; continue; }
            t = tsum / wsum;
            fr = smooth01(0.45, 0.9, fsum / wsum) * FRONT_ALPHA;
            pic_sst_colour(c_to_f(t), &r, &g, &b);
            /* front: translucent white over the wash, as in the world image */
            r = (r * a) * (1 - fr) + fr;
            g = (g * a) * (1 - fr) + fr;
            b = (b * a) * (1 - fr) + fr;
            a = a * (1 - fr) + fr;
            if (wsum < 0.75) {          /* fade the front out at the coast too */
                double k = smooth01(0.5, 0.75, wsum);
                r *= k; g *= k; b *= k; a *= k;
            }
            row[x] = ((uint32_t)(a * 255) << 24) | ((uint32_t)(r * 255) << 16) |
                     ((uint32_t)(g * 255) << 8) | (uint32_t)(b * 255);
        }
    }
    cairo_surface_mark_dirty(img);
    return img;
}

void pic_layer_render_sst(cairo_t *cr, int width, int height, time_t now,
                          void *ud)
{
    double span = pic_config.view_span_lon > 0 ? pic_config.view_span_lon : 360;
    (void)now; (void)ud;
    pthread_mutex_lock(&sst.mutex);
    /* The home note follows a QTH change without a refetch */
    if (sst.idx && (sst.qlat != pic_config.qth_lat || sst.qlon != pic_config.qth_lon)) {
        sst.qlat = pic_config.qth_lat;
        sst.qlon = pic_config.qth_lon;
        near_home(sst.idx, sst.qlat, sst.qlon, &sst.near_f, &sst.warm_f);
    }
    /* Cells wider than ~3 px: draw from the temperatures at half the
     * screen resolution (cached until the view or data change) */
    if (sst.idx && width / span * 0.25 > 3) {
        static cairo_surface_t *zimg;
        static double zkey[4];
        static unsigned char *zidx;
        double key[4] = { pic_config.view_center_lon, pic_config.view_center_lat,
                          span, (double)width };
        if (!zimg || zidx != sst.idx || memcmp(key, zkey, sizeof(key)) != 0) {
            if (zimg) cairo_surface_destroy(zimg);
            zimg = zoom_image(sst.idx, width / 2, height / 2);
            zidx = sst.idx;
            memcpy(zkey, key, sizeof(key));
        }
        if (zimg) {
            cairo_save(cr);
            cairo_scale(cr, 2.0, 2.0);
            cairo_set_source_surface(cr, zimg, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    } else if (sst.image) {
        pic_paint_viewport(cr, sst.image, width, height);
    }
    pthread_mutex_unlock(&sst.mutex);
}

void pic_layer_render_sst_legend(cairo_t *cr, int width, int height,
                                 time_t now, void *ud)
{
    double fs = height / 90.0, bw = fs * 20, bh = fs * 0.75, pad = fs * 0.6;
    double x0, y, tw, nw = 0, boxw, bx;
    const char *title = "Sea surface \xc2\xb0" "F";
    char note[96] = "";
    cairo_text_extents_t e;
    int i;

    (void)now; (void)ud;
    pthread_mutex_lock(&sst.mutex);
    if (!sst.image) {
        pthread_mutex_unlock(&sst.mutex);
        return;
    }
    if (!isnan(sst.near_f))
        snprintf(note, sizeof(note), "near home %.0f\xc2\xb0 \xc2\xb7 warmest within %.0f mi %.0f\xc2\xb0",
                 sst.near_f, NEAR_MI, sst.warm_f);
    pthread_mutex_unlock(&sst.mutex);

    cairo_save(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, title, &e);
    tw = e.x_advance + fs * 0.8;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs * 0.85);
    if (note[0]) {
        cairo_text_extents(cr, note, &e);
        nw = e.x_advance + fs * 1.2;
    }
    boxw = tw + bw + nw + pad * 2;
    x0 = (width - boxw) / 2.0;
    {
        double f72 = height / 72.0;
        double bottom = pic_legend_take(height, 1.5) + f72 * 0.45;
        y = bottom - (bh + fs * 1.75) + fs * 0.2;
    }
    cairo_new_path(cr);
    cairo_rectangle(cr, x0, y - fs * 0.2, boxw, bh + fs * 1.75);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_fill(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_set_source_rgba(cr, 0.9, 0.92, 0.96, 0.95);
    cairo_move_to(cr, x0 + pad, y + bh * 0.9);
    cairo_show_text(cr, title);

    /* 30 → 90 °F */
    bx = x0 + pad + tw;
    for (i = 0; i < 60; i++) {
        double r, g, b;
        pic_sst_colour(30 + i + 0.5, &r, &g, &b);
        cairo_rectangle(cr, bx + bw * i / 60.0, y, bw / 60.0 + 0.5, bh);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_fill(cr);
    }
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs * 0.8);
    cairo_set_source_rgba(cr, 0.85, 0.87, 0.9, 0.95);
    for (i = 0; i <= 6; i++) {
        char lab[8];
        double xx = bx + bw * i / 6.0;
        snprintf(lab, sizeof(lab), "%d", 30 + i * 10);
        cairo_text_extents(cr, lab, &e);
        if (i == 0) xx += e.x_advance / 2;
        if (i == 6) xx -= e.x_advance / 2;
        cairo_move_to(cr, xx - e.x_advance / 2, y + bh + fs * 1.05);
        cairo_show_text(cr, lab);
    }
    if (note[0]) {
        cairo_set_font_size(cr, fs * 0.85);
        cairo_set_source_rgba(cr, 0.8, 0.88, 1.0, 0.95);
        cairo_move_to(cr, bx + bw + fs * 0.8, y + bh * 0.9);
        cairo_show_text(cr, note);
    }
    cairo_restore(cr);
}

static void sst_stopped(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&sst.mutex);
    if (sst.image) cairo_surface_destroy(sst.image);
    sst.image = NULL;
    free(sst.idx);
    sst.idx = NULL;
    pthread_mutex_unlock(&sst.mutex);
}

static pic_feature_t feature_def = {
    .name = "sst", .layer = pic_layer_render_sst,
    .legend = pic_layer_render_sst_legend, .interval = 600,
    .extra_mb = 8, .work = sst_work, .needs_network = 1,
    .stopped = sst_stopped,
};

pic_feature_t *pic_feature_sst(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&sst.mutex, NULL);
        build_keys();
        sst.near_f = sst.warm_f = NAN;
        inited = 1;
    }
    return &feature_def;
}
