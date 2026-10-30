/*
 * layers_satclouds.c - Real satellite cloud imagery
 *
 * Geostationary infrared (ABI/AHI band 13, 10.3 µm) from NASA GIBS,
 * turned into a cloud overlay: warm (clear) surfaces become fully
 * transparent, cloud tops fade in with height (colder = whiter), and
 * the coldest storm tops can keep GIBS's colour enhancement
 * (SATCLOUD_STYLE=enhanced) or render white (=white, default).
 *
 * Coverage comes from three satellites, each used only for the
 * longitudes nearest its sub-point so the distorted limb is never
 * drawn: Himawari (Asia/Australia), GOES-West (Pacific) and
 * GOES-East (Americas/Atlantic). GIBS carries no Meteosat imagery,
 * so Europe, Africa and the Indian Ocean (about 5°W-70°E) stay clear.
 *
 * The request covers just the visible viewport at screen resolution,
 * so zooming in fetches sharper imagery. Refreshed every 10 minutes
 * (GIBS lags real time by ~1 hour).
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

#define GIBS_FMT \
    "https://gibs.earthdata.nasa.gov/wms/epsg4326/best/wms.cgi" \
    "?SERVICE=WMS&REQUEST=GetMap&VERSION=1.1.1&LAYERS=%s&STYLES=" \
    "&SRS=EPSG:4326&BBOX=%.3f,%.3f,%.3f,%.3f&WIDTH=%d&HEIGHT=%d" \
    "&FORMAT=image/png&TRANSPARENT=TRUE&TIME=%s"
#define SC_TMP     "/tmp/pi-clock-satcloud.png"
#define MAX_W      2048
#define LAT_LIMIT  72.0   /* Beyond this the geostationary view is junk */

static const struct { const char *layer; double lon0, lon1, sub; } bands[] = {
    { "GOES-East_ABI_Band13_Clean_Infrared",  -106.2,  -5.0,  -75.2 },
    { "GOES-West_ABI_Band13_Clean_Infrared",  -180.0, -106.2, -137.2 },
    { "Himawari_AHI_Band13_Clean_Infrared",     70.5,  180.0,  140.7 },
};

typedef struct {
    pthread_mutex_t  mutex;
    cairo_surface_t *image;     /* Premultiplied ARGB cloud overlay  */
    double lon0, lon1, lat0, lat1;
    int    enhanced;
    int    screen_w, screen_h;
    time_t fetched;
} satclouds_t;

static satclouds_t sc;

/*
 * Map one GIBS enhanced-IR pixel to a premultiplied cloud pixel.
 * Grey (r≈g≈b) is the temperature ramp: ~80-100 for warm ground and
 * ocean, brighter for colder cloud. Saturated colours mark the
 * coldest tops (< about -30 °C).
 */
uint32_t pic_satcloud_pixel(unsigned r, unsigned g, unsigned b, unsigned a,
                            int enhanced)
{
    unsigned mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    unsigned mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double al, c;

    if (a < 128) return 0;
    if (mx - mn < 25) {
        double v = (r + g + b) / 3.0;
        al = (v - 118.0) / 90.0;
        if (al <= 0) return 0;
        if (al > 1) al = 1;
        al *= 0.88;
        c = 205.0 + 50.0 * al;
        r = g = b = (unsigned)c;
    } else if (enhanced) {
        al = 0.9;
    } else {
        al = 0.9;
        r = g = b = 250;
    }
    return ((uint32_t)(al * 255) << 24) |
           ((uint32_t)(r * al) << 16) | ((uint32_t)(g * al) << 8) |
           (uint32_t)(b * al);
}

/* Blit an RGBA PNG into the composite at column x0. Returns the
 * number of opaque source pixels, or -1 on decode failure. */
static int blit_png(const char *path, cairo_surface_t *dst, int x0,
                    int want_w, int enhanced, double lat0, double lat1,
                    double lon0, double lon1, double sub)
{
    int w, h, ch, x, y, dw, dh, stride, opaque = 0;
    unsigned char *px = stbi_load(path, &w, &h, &ch, 4), *d;

    if (!px) return -1;
    dw = cairo_image_surface_get_width(dst);
    dh = cairo_image_surface_get_height(dst);
    stride = cairo_image_surface_get_stride(dst);
    d = cairo_image_surface_get_data(dst);
    if (w != want_w) {
        stbi_image_free(px);
        return -1;
    }
    for (y = 0; y < h && y < dh; y++) {
        uint32_t *row = (uint32_t *)(d + (size_t)y * stride);
        const unsigned char *s = px + (size_t)y * w * 4;
        double clat = cos((lat1 - (y + 0.5) / h * (lat1 - lat0)) * M_PI / 180);
        for (x = 0; x < w && x0 + x < dw; x++, s += 4)
            if (x0 + x >= 0) {
                /* Fade out toward the limb (55-72° from the sub-
                 * satellite point), where the view grazes the Earth
                 * and cold ground reads as cloud. */
                double lon = lon0 + (x + 0.5) / w * (lon1 - lon0);
                double c = acos(clat * cos((lon - sub) * M_PI / 180)) * 180 / M_PI;
                double fade = c < 55 ? 1.0 : (72 - c) / 17.0;
                if (fade < 0) fade = 0;
                opaque += s[3] >= 128;
                uint32_t p = pic_satcloud_pixel(s[0], s[1], s[2], s[3], enhanced);
                if (fade < 1.0) {
                    uint32_t a = (uint32_t)((p >> 24) * fade);
                    uint32_t r = (uint32_t)(((p >> 16) & 255) * fade);
                    uint32_t g = (uint32_t)(((p >> 8) & 255) * fade);
                    uint32_t b = (uint32_t)((p & 255) * fade);
                    p = (a << 24) | (r << 16) | (g << 8) | b;
                }
                row[x0 + x] = p;
            }
    }
    stbi_image_free(px);
    return opaque;
}

static int satclouds_work(pic_feature_t *f)
{
    double span = pic_config.view_span_lon, span_lat = pic_config.view_span_lat;
    double lon0, lon1, lat0, lat1;
    int sw, sh, enhanced, W, H, i, k, ok = 0;
    unsigned gen = pic_config.viewport_gen;
    cairo_surface_t *img;

    (void)f;
    pthread_mutex_lock(&sc.mutex);
    sw = sc.screen_w > 0 ? sc.screen_w : 1920;
    sh = sc.screen_h > 0 ? sc.screen_h : 1080;
    enhanced = sc.enhanced;
    pthread_mutex_unlock(&sc.mutex);

    if (span < 0.0001) span = 360.0;
    if (span_lat < 0.0001) span_lat = 180.0;
    lon0 = pic_config.view_center_lon - span / 2;
    lon1 = pic_config.view_center_lon + span / 2;
    lat0 = pic_config.view_center_lat - span_lat / 2;
    lat1 = pic_config.view_center_lat + span_lat / 2;
    if (lat0 < -LAT_LIMIT) lat0 = -LAT_LIMIT;
    if (lat1 > LAT_LIMIT) lat1 = LAT_LIMIT;
    if (lat1 <= lat0) return 600;

    W = (int)lround(sw * (lon1 - lon0) / span);
    if (W > MAX_W) W = MAX_W;
    if (W < 360) W = 360;
    /* Keep the image's aspect equal to its on-screen aspect */
    H = (int)lround(W * ((lat1 - lat0) * sh / span_lat) /
                    ((lon1 - lon0) * sw / span));
    if (H < 16) H = 16;
    if (H > 1200) H = 1200;

    img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
        return 600;
    }
    cairo_surface_flush(img);

    for (i = 0; i < (int)(sizeof(bands) / sizeof(bands[0])); i++) {
        for (k = -1; k <= 1; k++) {
            double b0 = bands[i].lon0 + k * 360.0, b1 = bands[i].lon1 + k * 360.0;
            double i0 = lon0 > b0 ? lon0 : b0, i1 = lon1 < b1 ? lon1 : b1;
            int x0, x1, tries;
            char url[600], when[24];

            if (i1 - i0 < 0.2) continue;
            x0 = (int)lround((i0 - lon0) / (lon1 - lon0) * W);
            x1 = (int)lround((i1 - lon0) / (lon1 - lon0) * W);
            if (x1 - x0 < 2) continue;
            /*
             * GIBS's newest time step is often still being ingested
             * and comes back empty, so ask for an explicit time ~80
             * minutes old and step back 10 minutes if a tile is blank.
             */
            for (tries = 0; tries < 4; tries++) {
                time_t t = time(NULL) - 80 * 60 - tries * 600;
                struct tm tm;
                int got;
                t -= t % 600;
                gmtime_r(&t, &tm);
                strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:00Z", &tm);
                snprintf(url, sizeof(url), GIBS_FMT, bands[i].layer,
                         i0 - k * 360.0, lat0, i1 - k * 360.0, lat1,
                         x1 - x0, H, when);
                got = pic_fetch_to_file(url, SC_TMP, 60) == 0 ?
                      blit_png(SC_TMP, img, x0, x1 - x0, enhanced, lat0, lat1,
                               i0, i1, bands[i].sub + k * 360.0) : -1;
                unlink(SC_TMP);
                if (got > 0) { ok++; break; }
            }
            if (tries == 4)
                printf("satclouds: no imagery from %s\n", bands[i].layer);
        }
    }
    cairo_surface_mark_dirty(img);

    if (!ok) {
        cairo_surface_destroy(img);
        printf("satclouds: fetch failed\n");
        return 120;
    }

    pthread_mutex_lock(&sc.mutex);
    {
        cairo_surface_t *old = sc.image;
        sc.image = img;
        sc.lon0 = lon0; sc.lon1 = lon1; sc.lat0 = lat0; sc.lat1 = lat1;
        sc.fetched = time(NULL);
        if (old) cairo_surface_destroy(old);
    }
    pthread_mutex_unlock(&sc.mutex);
    printf("satclouds: %dx%d, %d tiles\n", W, H, ok);

    /* Refetch sooner if the viewport moves (zoom/pan) */
    {
        int s;
        for (s = 0; s < 600; s++) {
            if (pic_config.viewport_gen != gen) return 5;
            if (pic_feature_interrupted(f)) return 1;
            sleep(1);
        }
    }
    return 1;
}

static void satclouds_reload(pic_feature_t *f)
{
    char buf[32] = "";
    int enh;
    pic_conf_str("SATCLOUD_STYLE", buf, sizeof(buf));
    enh = strcmp(buf, "enhanced") == 0;
    pthread_mutex_lock(&sc.mutex);
    if (enh != sc.enhanced) {
        sc.enhanced = enh;
        pic_feature_poke(f);
    }
    pthread_mutex_unlock(&sc.mutex);
}

void pic_layer_render_satclouds(cairo_t *cr, int width, int height,
                                time_t now, void *ud)
{
    double span, ppd, world, x0, y0, y1, w_px;
    int iw, ih, rep;

    (void)now; (void)ud;
    pthread_mutex_lock(&sc.mutex);
    sc.screen_w = width;
    sc.screen_h = height;
    if (!sc.image) {
        pthread_mutex_unlock(&sc.mutex);
        return;
    }
    span = pic_config.view_span_lon;
    if (span < 0.0001) span = 360.0;
    ppd = width / span;
    world = 360.0 * ppd;
    w_px = (sc.lon1 - sc.lon0) * ppd;
    x0 = pic_lon_to_x((sc.lon0 + sc.lon1) / 2.0, width) - w_px / 2.0;
    y0 = pic_lat_to_y(sc.lat1, height);
    y1 = pic_lat_to_y(sc.lat0, height);
    iw = cairo_image_surface_get_width(sc.image);
    ih = cairo_image_surface_get_height(sc.image);

    for (rep = -1; rep <= 1; rep++) {
        double xr = x0 + rep * world;
        if (xr > width || xr + w_px < 0) continue;
        cairo_save(cr);
        cairo_rectangle(cr, xr, y0, w_px, y1 - y0);
        cairo_clip(cr);
        cairo_translate(cr, xr, y0);
        cairo_scale(cr, w_px / iw, (y1 - y0) / ih);
        cairo_set_source_surface(cr, sc.image, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    pthread_mutex_unlock(&sc.mutex);
}

void pic_satclouds_free(void);
static void satclouds_stopped(pic_feature_t *f)
{
    (void)f;
    pic_satclouds_free();
}

static pic_feature_t feature_def = {
    .name = "satclouds", .layer = pic_layer_render_satclouds,
    .interval = 60, .extra_mb = 12,
    .reload = satclouds_reload, .work = satclouds_work, .needs_network = 1,
    .stopped = satclouds_stopped,
};

pic_feature_t *pic_feature_satclouds(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&sc.mutex, NULL);
        inited = 1;
    }
    return &feature_def;
}

/* Release the image when the layer is turned off */
void pic_satclouds_free(void)
{
    pthread_mutex_lock(&sc.mutex);
    if (sc.image) cairo_surface_destroy(sc.image);
    sc.image = NULL;
    pthread_mutex_unlock(&sc.mutex);
}
