/*
 * layers_lightpollution.c - Estimated night-sky brightness
 *
 * A light-pollution map in the style of the World Atlas of Artificial
 * Night Sky Brightness, computed on the Pi from NASA's VIIRS Night
 * Lights composite (via GIBS):
 *
 *   1. Upward light: VIIRS radiance, decoded from the layer's alpha
 *      channel as (a/255)^2.2.
 *   2. Skyglow: light scattered in the atmosphere reaches the zenith
 *      of places up to a couple of hundred km away, falling off
 *      roughly as distance^-2.5 (Walker's law). That kernel is
 *      approximated by a sum of five Gaussians (3-220 km), each done
 *      as three box blurs.
 *   3. Calibration: one constant scales the result to the ratio of
 *      artificial to natural sky brightness, chosen so downtown
 *      Raleigh reads ~18.5 mag/arcsec² and rural eastern NC ~21.5,
 *      in line with published atlas values.
 *
 * The map colours each place by its approximate Bortle class, and a
 * note at the bottom gives the estimate at home plus the nearest
 * Bortle 3 (or darkest) sky within ~300 km and its direction.
 * Estimates are approximate: terrain, shielding and post-2016
 * changes (e.g. LED conversions) aren't modelled.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "config.h"
#include "fetch.h"
#include "stb_image.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GIBS_NL \
    "https://gibs.earthdata.nasa.gov/wms/epsg4326/best/wms.cgi" \
    "?SERVICE=WMS&REQUEST=GetMap&VERSION=1.1.1&LAYERS=VIIRS_Night_Lights" \
    "&STYLES=&SRS=EPSG:4326&BBOX=%.4f,%.4f,%.4f,%.4f&WIDTH=%d&HEIGHT=%d" \
    "&FORMAT=image/png&TRANSPARENT=TRUE&TIME=2016-01-01"
#define LP_TMP  "/tmp/pi-clock-lights.png"
#define CALIB_K 20.2     /* ratio units per kernel-weighted radiance  */
#define NAT_SQM 22.0     /* natural zenith brightness, mag/arcsec²    */

static const struct { double sigma_km, w; } kernel[] = {
    { 3, 1.0 }, { 12, 0.5 }, { 40, 0.35 }, { 100, 0.25 }, { 220, 0.12 },
};

typedef struct {
    pthread_mutex_t  mutex;
    cairo_surface_t *image;
    double lon0, lon1, lat0, lat1;
    int    screen_w, screen_h;
    /* Home estimate */
    int    have_home;
    double home_sqm;
    double dark_km, dark_bearing, dark_sqm;
    int    labels;
} lp_t;

static lp_t lp;

static int bortle(double sqm)
{
    if (sqm >= 21.75) return 1;
    if (sqm >= 21.6)  return 2;
    if (sqm >= 21.3)  return 3;
    if (sqm >= 20.4)  return 4;
    if (sqm >= 19.1)  return 5;
    if (sqm >= 18.5)  return 6;
    if (sqm >= 18.0)  return 7;
    if (sqm >= 17.5)  return 8;
    return 9;
}

/* One box-blur pass of radius r along rows (dir 0) or columns (1) */
static void box_pass(const float *in, float *out, int w, int h, int r, int dir)
{
    int n = dir ? h : w, m = dir ? w : h, i, j;
    size_t step = dir ? (size_t)w : 1, stride = dir ? 1 : (size_t)w;
    float inv = 1.0f / (2 * r + 1);

    for (j = 0; j < m; j++) {
        const float *src = in + j * stride;
        float *dst = out + j * stride;
        double acc = 0;
        for (i = 0; i <= r && i < n; i++) acc += src[i * step];
        for (i = 0; i < n; i++) {
            dst[i * step] = (float)(acc * inv);
            if (i + r + 1 < n) acc += src[(i + r + 1) * step];
            if (i - r >= 0) acc -= src[(i - r) * step];
        }
    }
}

/* Approximate Gaussian blur (3 box passes per axis), in place */
static void gauss(float *buf, float *tmp, int w, int h, double sx, double sy)
{
    int k;
    /* Three passes of width 2r+1 give variance 3((2r+1)^2-1)/12 */
    int rx = (int)lround((sqrt(4.0 * sx * sx + 1) - 1) / 2);
    int ry = (int)lround((sqrt(4.0 * sy * sy + 1) - 1) / 2);
    for (k = 0; k < 3; k++) {
        if (rx > 0) { box_pass(buf, tmp, w, h, rx, 0); memcpy(buf, tmp, (size_t)w * h * 4); }
        if (ry > 0) { box_pass(buf, tmp, w, h, ry, 1); memcpy(buf, tmp, (size_t)w * h * 4); }
    }
}

/*
 * Artificial/natural brightness ratio for each pixel of a radiance
 * grid covering lat0..lat1. Returns a new array (caller frees).
 */
float *pic_lp_skyglow(const float *rad, int w, int h, double lon_span,
                      double lat0, double lat1)
{
    float *out = calloc((size_t)w * h, sizeof(float));
    float *b = malloc((size_t)w * h * sizeof(float));
    float *t = malloc((size_t)w * h * sizeof(float));
    double mid = (lat0 + lat1) / 2 * M_PI / 180;
    double kmx = lon_span / w * 111.32 * cos(mid), kmy = (lat1 - lat0) / h * 111.32;
    size_t i, n = (size_t)w * h;
    int k;

    if (!out || !b || !t) {
        free(out); free(b); free(t);
        return NULL;
    }
    if (kmx < 0.05) kmx = 0.05;
    for (k = 0; k < (int)(sizeof(kernel) / sizeof(kernel[0])); k++) {
        memcpy(b, rad, n * sizeof(float));
        gauss(b, t, w, h, kernel[k].sigma_km / kmx, kernel[k].sigma_km / kmy);
        for (i = 0; i < n; i++) out[i] += (float)(kernel[k].w * CALIB_K) * b[i];
    }
    free(b);
    free(t);
    return out;
}

static double ratio_to_sqm(double r) { return NAT_SQM - 2.5 * log10(1.0 + r); }

/* Fetch the night-lights radiance for a box; NULL on failure. */
static float *fetch_radiance(double lon0, double lat0, double lon1,
                             double lat1, int w, int h)
{
    char url[400];
    unsigned char *px;
    float *rad;
    int iw, ih, ch;
    size_t i;

    snprintf(url, sizeof(url), GIBS_NL, lon0, lat0, lon1, lat1, w, h);
    if (pic_fetch_to_file(url, LP_TMP, 90) != 0) return NULL;
    px = stbi_load(LP_TMP, &iw, &ih, &ch, 4);
    unlink(LP_TMP);
    if (!px) return NULL;
    if (iw != w || ih != h) {
        stbi_image_free(px);
        return NULL;
    }
    rad = malloc((size_t)w * h * sizeof(float));
    if (rad)
        for (i = 0; i < (size_t)w * h; i++)
            rad[i] = (float)pow(px[i * 4 + 3] / 255.0, 2.2);
    stbi_image_free(px);
    return rad;
}

/* Atlas-style colour bands, premultiplied ARGB */
static uint32_t colour_for(double sqm)
{
    static const struct { double sqm; double r, g, b, a; } band[] = {
        { 21.75, 0, 0, 0, 0 },
        { 21.6, 0.20, 0.22, 0.42, 0.30 },
        { 21.3, 0.10, 0.30, 0.85, 0.40 },
        { 20.4, 0.10, 0.70, 0.30, 0.45 },
        { 19.1, 0.95, 0.85, 0.15, 0.50 },
        { 18.5, 1.00, 0.55, 0.10, 0.55 },
        { 18.0, 0.95, 0.15, 0.10, 0.60 },
        { -99,  1.00, 0.85, 0.95, 0.65 },
    };
    int i;
    for (i = 0; sqm < band[i].sqm; i++) ;
    return ((uint32_t)(band[i].a * 255) << 24) |
           ((uint32_t)(band[i].r * band[i].a * 255) << 16) |
           ((uint32_t)(band[i].g * band[i].a * 255) << 8) |
           (uint32_t)(band[i].b * band[i].a * 255);
}

/* Home estimate and the nearest Bortle <= 3 sky. Returns 0 on success. */
static int analyse_home(double qlat, double qlon, double *sqm_home,
                        double *dkm, double *dbear, double *dsqm)
{
    double hl = 3.0, hw = 3.6, lat0 = qlat - hl, lat1 = qlat + hl;
    double lon0 = qlon - hw, lon1 = qlon + hw;
    int w = (int)(2 * hw * 111.32 * cos(qlat * M_PI / 180) / 1.1);
    int h = (int)(2 * hl * 111.32 / 1.1), x, y, best = -1, darkest = -1;
    double best_d = 1e9, darkest_v = -1;
    float *rad, *ratio;

    rad = fetch_radiance(lon0, lat0, lon1, lat1, w, h);
    if (!rad) return -1;
    ratio = pic_lp_skyglow(rad, w, h, lon1 - lon0, lat0, lat1);
    free(rad);
    if (!ratio) return -1;

    *sqm_home = ratio_to_sqm(ratio[(h / 2) * w + w / 2]);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            double la = lat1 - (y + 0.5) * (lat1 - lat0) / h;
            double lo = lon0 + (x + 0.5) * (lon1 - lon0) / w;
            double s = ratio_to_sqm(ratio[y * w + x]);
            double d = pic_dist_km(qlat, qlon, la, lo);
            if (d > 300) continue;
            if (s >= 21.3 && d < best_d) { best_d = d; best = y * w + x; }
            if (s > darkest_v) { darkest_v = s; darkest = y * w + x; }
        }
    if (best < 0) best = darkest;
    if (best >= 0) {
        double la = lat1 - (best / w + 0.5) * (lat1 - lat0) / h;
        double lo = lon0 + (best % w + 0.5) * (lon1 - lon0) / w;
        double r = M_PI / 180;
        *dkm = pic_dist_km(qlat, qlon, la, lo);
        *dsqm = ratio_to_sqm(ratio[best]);
        *dbear = atan2(sin((lo - qlon) * r) * cos(la * r),
                       cos(qlat * r) * sin(la * r) -
                       sin(qlat * r) * cos(la * r) * cos((lo - qlon) * r)) / r;
        if (*dbear < 0) *dbear += 360;
    }
    free(ratio);
    return 0;
}

static int lp_work(pic_feature_t *f)
{
    double span = pic_config.view_span_lon, span_lat = pic_config.view_span_lat;
    double qlat = pic_config.qth_lat, qlon = pic_config.qth_lon;
    double lon0, lon1, lat0, lat1, mlat, mlon;
    unsigned gen = pic_config.viewport_gen;
    int sw, sh, w, h, s;
    float *rad, *ratio;
    cairo_surface_t *img;
    static double done_lat = 999, done_lon = 999;   /* last attempt */
    int have_home;

    pthread_mutex_lock(&lp.mutex);
    sw = lp.screen_w > 0 ? lp.screen_w : 1920;
    sh = lp.screen_h > 0 ? lp.screen_h : 1080;
    have_home = lp.have_home;
    pthread_mutex_unlock(&lp.mutex);

    /* Home estimate: once per QTH (retried later if it failed) */
    if (qlat != done_lat || qlon != done_lon || !have_home) {
        done_lat = qlat;
        done_lon = qlon;
        double hs = 0, dk = -1, db = 0, ds = 0;
        if (analyse_home(qlat, qlon, &hs, &dk, &db, &ds) == 0) {
            pthread_mutex_lock(&lp.mutex);
            lp.have_home = 1;
            lp.home_sqm = hs;
            lp.dark_km = dk;
            lp.dark_bearing = db;
            lp.dark_sqm = ds;
            pthread_mutex_unlock(&lp.mutex);
            have_home = 1;
            printf("lightpollution: home %.2f mag/arcsec2 (Bortle %d), "
                   "dark %.0f km @ %.0f deg (%.2f)\n", hs, bortle(hs), dk, db, ds);
        }
    }

    /* Map image: viewport plus a margin so glow from just off-screen
     * cities is included */
    if (span < 0.0001) span = 360;
    if (span_lat < 0.0001) span_lat = 180;
    mlat = 3.0;
    mlon = 3.0 / cos(pic_config.view_center_lat * M_PI / 180);
    lon0 = pic_config.view_center_lon - span / 2 - mlon;
    lon1 = pic_config.view_center_lon + span / 2 + mlon;
    lat0 = pic_config.view_center_lat - span_lat / 2 - mlat;
    lat1 = pic_config.view_center_lat + span_lat / 2 + mlat;
    if (lon0 < -180 || lon1 > 180) { lon0 = -180; lon1 = 180; }
    if (lat0 < -65) lat0 = -65;     /* Few lights beyond this; saves RAM */
    if (lat1 > 75) lat1 = 75;
    w = (int)(sw * (lon1 - lon0) / span / 2);
    h = (int)(sh * (lat1 - lat0) / span_lat / 2);
    if (w > 1400) { h = h * 1400 / w; w = 1400; }
    if (h > 1000) { w = w * 1000 / h; h = 1000; }
    if (w < 64 || h < 32) return 600;

    rad = fetch_radiance(lon0, lat0, lon1, lat1, w, h);
    if (!rad) {
        printf("lightpollution: fetch failed\n");
        return 600;
    }
    ratio = pic_lp_skyglow(rad, w, h, lon1 - lon0, lat0, lat1);
    free(rad);
    if (!ratio) return 600;

    img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(img) == CAIRO_STATUS_SUCCESS) {
        unsigned char *d;
        int stride, x, y;
        cairo_surface_flush(img);
        d = cairo_image_surface_get_data(img);
        stride = cairo_image_surface_get_stride(img);
        for (y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)(d + (size_t)y * stride);
            for (x = 0; x < w; x++)
                row[x] = colour_for(ratio_to_sqm(ratio[y * w + x]));
        }
        cairo_surface_mark_dirty(img);
        pthread_mutex_lock(&lp.mutex);
        if (lp.image) cairo_surface_destroy(lp.image);
        lp.image = img;
        lp.lon0 = lon0; lp.lon1 = lon1; lp.lat0 = lat0; lp.lat1 = lat1;
        pthread_mutex_unlock(&lp.mutex);
        printf("lightpollution: map %dx%d %.1f,%.1f..%.1f,%.1f\n", w, h, lon0, lat0, lon1, lat1);
    } else {
        cairo_surface_destroy(img);
    }
    free(ratio);

    /* The data is static (2016): only redo it when the view moves
     * (or retry the home estimate after 30 minutes if it failed) */
    for (s = 0; s < (have_home ? 6 * 3600 : 1800); s++) {
        if (pic_config.viewport_gen != gen) return 3;
        if (pic_config.qth_lat != done_lat || pic_config.qth_lon != done_lon) return 3;
        if (pic_feature_interrupted(f)) return 1;
        sleep(1);
    }
    return 1;
}

static void lp_reload(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&lp.mutex);
    lp.labels = pic_conf_int("LP_NOTE", 1);
    pthread_mutex_unlock(&lp.mutex);
}

void pic_layer_render_lightpollution(cairo_t *cr, int width, int height,
                                     time_t now, void *ud)
{
    double span, ppd, world, x0, y0, y1, w_px;
    int iw, ih, rep;

    (void)now; (void)ud;
    pthread_mutex_lock(&lp.mutex);
    lp.screen_w = width;
    lp.screen_h = height;
    if (!lp.image) {
        pthread_mutex_unlock(&lp.mutex);
        return;
    }
    span = pic_config.view_span_lon;
    if (span < 0.0001) span = 360.0;
    ppd = width / span;
    world = 360.0 * ppd;
    w_px = (lp.lon1 - lp.lon0) * ppd;
    x0 = pic_lon_to_x((lp.lon0 + lp.lon1) / 2.0, width) - w_px / 2.0;
    y0 = pic_lat_to_y(lp.lat1, height);
    y1 = pic_lat_to_y(lp.lat0, height);
    iw = cairo_image_surface_get_width(lp.image);
    ih = cairo_image_surface_get_height(lp.image);
    for (rep = -1; rep <= 1; rep++) {
        double xr = x0 + rep * world;
        if (xr > width || xr + w_px < 0) continue;
        cairo_save(cr);
        cairo_rectangle(cr, xr, y0, w_px, y1 - y0);
        cairo_clip(cr);
        cairo_translate(cr, xr, y0);
        cairo_scale(cr, w_px / iw, (y1 - y0) / ih);
        cairo_set_source_surface(cr, lp.image, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    pthread_mutex_unlock(&lp.mutex);
}

static const char *compass8(double b)
{
    static const char *p[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return p[(int)((b + 22.5) / 45.0) % 8];
}

void pic_layer_render_lightpollution_note(cairo_t *cr, int width, int height,
                                          time_t now, void *ud)
{
    char text[192];
    (void)now; (void)ud;
    pthread_mutex_lock(&lp.mutex);
    if (!lp.labels || !lp.have_home) {
        pthread_mutex_unlock(&lp.mutex);
        return;
    }
    if (lp.dark_km >= 0 && bortle(lp.dark_sqm) < bortle(lp.home_sqm))
        snprintf(text, sizeof(text),
                 "Sky at home \xe2\x89\x88 Bortle %d (%.1f mag/arcsec\xc2\xb2) "
                 "\xc2\xb7 Bortle %d skies ~%.0f km %s",
                 bortle(lp.home_sqm), lp.home_sqm, bortle(lp.dark_sqm),
                 lp.dark_km, compass8(lp.dark_bearing));
    else
        snprintf(text, sizeof(text),
                 "Sky at home \xe2\x89\x88 Bortle %d (%.1f mag/arcsec\xc2\xb2)",
                 bortle(lp.home_sqm), lp.home_sqm);
    pthread_mutex_unlock(&lp.mutex);
    pic_draw_legend_line(cr, width, height, 1, text, 0.8, 0.85, 1.0);
}

static void lp_stopped(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&lp.mutex);
    if (lp.image) cairo_surface_destroy(lp.image);
    lp.image = NULL;
    pthread_mutex_unlock(&lp.mutex);
}

static pic_feature_t feature_def = {
    .name = "lightpollution", .layer = pic_layer_render_lightpollution,
    .legend = pic_layer_render_lightpollution_note, .interval = 60,
    .extra_mb = 8, .reload = lp_reload, .work = lp_work,
    .needs_network = 1, .stopped = lp_stopped,
};

pic_feature_t *pic_feature_lightpollution(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&lp.mutex, NULL);
        lp.labels = 1;
        inited = 1;
    }
    return &feature_def;
}
