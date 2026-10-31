/*
 * layers_rainfall.c - Observed rainfall totals (US)
 *
 * NOAA MRMS radar + gauge quantitative precipitation estimates for the
 * last 24, 48 or 72 hours (RAIN_HOURS), as a colour overlay via the
 * Iowa Environmental Mesonet's WMS. Like the live radar layer, the
 * image covers the contiguous US at the on-screen resolution, so the
 * local view gets more detail. Refreshed every 30 minutes.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "config.h"
#include "fetch.h"
#include "radar.h"      /* RADAR_* bounds, pic_radar_decode_png */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MRMS_FMT \
    "https://mesonet.agron.iastate.edu/cgi-bin/wms/us/mrms.cgi" \
    "?SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=mrms_p%dh" \
    "&STYLES=&SRS=EPSG:4326&BBOX=%.1f,%.1f,%.1f,%.1f" \
    "&WIDTH=%d&HEIGHT=%d&FORMAT=image/png&TRANSPARENT=true"
#define RAIN_TMP "/tmp/pi-clock-rain.png"

static struct {
    pthread_mutex_t  mutex;
    cairo_surface_t *image;
    double ext[4];
    int    hours, screen_w;
    time_t fetched;
} rn;

static int rain_work(pic_feature_t *f)
{
    double span = pic_config.view_span_lon;
    int w, h, hours, sw, s;
    unsigned gen = pic_config.viewport_gen;
    char url[400];
    cairo_surface_t *img = NULL;

    pthread_mutex_lock(&rn.mutex);
    hours = rn.hours;
    sw = rn.screen_w > 0 ? rn.screen_w : 1920;
    pthread_mutex_unlock(&rn.mutex);
    static const double full[4] = { RADAR_WEST, RADAR_SOUTH, RADAR_EAST, RADAR_NORTH };
    double ext[4];
    (void)span;
    if (!pic_view_extent(full, sw, 2400, ext, &w, &h)) goto wait;   /* off-screen */
    snprintf(url, sizeof(url), MRMS_FMT, hours, ext[0], ext[1], ext[2], ext[3], w, h);
    if (pic_fetch_to_file(url, RAIN_TMP, 60) == 0)
        img = pic_radar_decode_png(RAIN_TMP);
    unlink(RAIN_TMP);
    if (img) {
        /* MRMS paints areas outside radar coverage solid grey: clear them */
        unsigned char *d;
        int x, y, iw = cairo_image_surface_get_width(img);
        int ih = cairo_image_surface_get_height(img), st = cairo_image_surface_get_stride(img);
        cairo_surface_flush(img);
        d = cairo_image_surface_get_data(img);
        for (y = 0; y < ih; y++) {
            uint32_t *row = (uint32_t *)(d + (size_t)y * st);
            for (x = 0; x < iw; x++) {
                uint32_t p = row[x], r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                if ((p >> 24) == 255 && r == g && g == b) row[x] = 0;
            }
        }
        cairo_surface_mark_dirty(img);
    }
    if (!img) {
        printf("rainfall: fetch failed\n");
        return 300;
    }
    pthread_mutex_lock(&rn.mutex);
    if (rn.image) cairo_surface_destroy(rn.image);
    rn.image = img;
    memcpy(rn.ext, ext, sizeof(ext));
    rn.fetched = time(NULL);
    pthread_mutex_unlock(&rn.mutex);
    printf("rainfall: %dh totals %dx%d\n", hours, w, h);
wait:

    for (s = 0; s < 30 * 60; s++) {
        if (pic_feature_interrupted(f)) return 1;
        if (pic_config.viewport_gen != gen) return 3;
        sleep(1);
    }
    return 1;
}

static void rain_reload(pic_feature_t *f)
{
    int h = pic_conf_int("RAIN_HOURS", 24);
    if (h != 1 && h != 24 && h != 48 && h != 72) h = 24;
    pthread_mutex_lock(&rn.mutex);
    if (h != rn.hours) pic_feature_poke(f);
    rn.hours = h;
    pthread_mutex_unlock(&rn.mutex);
}

void pic_layer_render_rainfall(cairo_t *cr, int width, int height,
                               time_t now, void *ud)
{
    double span, ppd, world, x0, y0, y1, w_px;
    int iw, ih, rep;

    (void)now; (void)ud;
    pthread_mutex_lock(&rn.mutex);
    rn.screen_w = width;
    if (!rn.image) {
        pthread_mutex_unlock(&rn.mutex);
        return;
    }
    span = pic_config.view_span_lon;
    if (span < 0.0001) span = 360.0;
    ppd = width / span;
    world = 360.0 * ppd;
    w_px = (rn.ext[2] - rn.ext[0]) * ppd;
    x0 = pic_lon_to_x((rn.ext[0] + rn.ext[2]) / 2.0, width) - w_px / 2.0;
    y0 = pic_lat_to_y(rn.ext[3], height);
    y1 = pic_lat_to_y(rn.ext[1], height);
    iw = cairo_image_surface_get_width(rn.image);
    ih = cairo_image_surface_get_height(rn.image);
    for (rep = -1; rep <= 1; rep++) {
        double xr = x0 + rep * world;
        if (xr > width || xr + w_px < 0) continue;
        cairo_save(cr);
        cairo_rectangle(cr, xr, y0, w_px, y1 - y0);
        cairo_clip(cr);
        cairo_translate(cr, xr, y0);
        cairo_scale(cr, w_px / iw, (y1 - y0) / ih);
        cairo_set_source_surface(cr, rn.image, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    pthread_mutex_unlock(&rn.mutex);
}

void pic_layer_render_rainfall_note(cairo_t *cr, int width, int height,
                                    time_t now, void *ud)
{
    char t[64];
    (void)now; (void)ud;
    pthread_mutex_lock(&rn.mutex);
    if (!rn.image) {
        pthread_mutex_unlock(&rn.mutex);
        return;
    }
    snprintf(t, sizeof(t), "Rain totals: last %d h (MRMS)", rn.hours);
    pthread_mutex_unlock(&rn.mutex);
    pic_draw_legend_line(cr, width, height, 4, t, 0.6, 0.8, 1.0);
}

static void rain_stopped(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&rn.mutex);
    if (rn.image) cairo_surface_destroy(rn.image);
    rn.image = NULL;
    pthread_mutex_unlock(&rn.mutex);
}

static pic_feature_t def = {
    .name = "rainfall", .layer = pic_layer_render_rainfall,
    .legend = pic_layer_render_rainfall_note, .interval = 60,
    .extra_mb = 10, .reload = rain_reload, .work = rain_work,
    .needs_network = 1, .stopped = rain_stopped,
};

pic_feature_t *pic_feature_rainfall(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&rn.mutex, NULL);
        rn.hours = 24;
        inited = 1;
    }
    return &def;
}
