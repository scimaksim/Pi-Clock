/*
 * layers_airquality.c - Regional US AQI overlay + QTH air quality badge
 *
 * Overlay: the AQI grid becomes a tiny image (one pixel per sample, EPA
 * colours, opacity rising with AQI so clean air stays subtle) which
 * Cairo scales up with bilinear filtering into a smooth field over the
 * sampled area. Missing samples are transparent.
 *
 * Badge (legend hook, always opaque): "AQI 42  Good  PM2.5 8" just
 * below the home QTH.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "airquality.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "math_utils.h"

/*
 * Overlay colour: interpolate between EPA category colours anchored at
 * mid-category AQI values, so the field shades smoothly. Opacity ramps
 * from zero at AQI 30, so clean air leaves the map untouched and only
 * moderate-or-worse air (smoke, ozone) shows.
 */
static void field_rgba(double aqi, double *r, double *g, double *b, double *a)
{
    static const struct { double aqi, r, g, b; } stops[] = {
        {  25, 0.00, 0.89, 0.00 },
        {  75, 1.00, 1.00, 0.00 },
        { 125, 1.00, 0.49, 0.00 },
        { 175, 1.00, 0.00, 0.00 },
        { 250, 0.56, 0.25, 0.59 },
        { 350, 0.49, 0.00, 0.14 },
    };
    int n = sizeof(stops) / sizeof(stops[0]), i;

    if (aqi <= stops[0].aqi) { *r = stops[0].r; *g = stops[0].g; *b = stops[0].b; }
    else if (aqi >= stops[n - 1].aqi) { *r = stops[n-1].r; *g = stops[n-1].g; *b = stops[n-1].b; }
    else {
        i = 0;
        while (i < n - 1 && aqi > stops[i + 1].aqi)
            i++;
        {
            double f = (aqi - stops[i].aqi) / (stops[i + 1].aqi - stops[i].aqi);
            *r = stops[i].r + f * (stops[i + 1].r - stops[i].r);
            *g = stops[i].g + f * (stops[i + 1].g - stops[i].g);
            *b = stops[i].b + f * (stops[i + 1].b - stops[i].b);
        }
    }
    *a = (aqi - 30.0) / 90.0;
    if (*a < 0) *a = 0;
    if (*a > 1) *a = 1;
    *a *= 0.55;
}

void pic_layer_render_airquality(cairo_t *cr, int width, int height,
                                 time_t now, void *user_data)
{
    pic_airquality_t *d = (pic_airquality_t *)user_data;
    cairo_surface_t *img;
    unsigned char *px;
    int stride, x, y;
    double span, ppd, cx, x0, y0, y1, w_px, world;
    int rep;

    (void)now;
    if (!d) return;

    img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, AQ_NX, AQ_NY);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
        return;
    }

    pthread_mutex_lock(&d->mutex);
    if (!d->have_grid) {
        pthread_mutex_unlock(&d->mutex);
        cairo_surface_destroy(img);
        return;
    }

    cairo_surface_flush(img);
    px = cairo_image_surface_get_data(img);
    stride = cairo_image_surface_get_stride(img);
    for (y = 0; y < AQ_NY; y++) {
        unsigned int *row = (unsigned int *)(px + y * stride);
        for (x = 0; x < AQ_NX; x++) {
            double v = d->aqi[y][x], r, g, b, a;
            if (v < 0) { row[x] = 0; continue; }
            field_rgba(v, &r, &g, &b, &a);
            /* Soften the outer ring so the sampled area has no hard edge */
            if (x == 0 || y == 0 || x == AQ_NX - 1 || y == AQ_NY - 1)
                a *= 0.4;
            row[x] = ((unsigned)(a * 255) << 24) |
                     ((unsigned)(r * a * 255) << 16) |
                     ((unsigned)(g * a * 255) << 8) |
                     (unsigned)(b * a * 255);
        }
    }
    cairo_surface_mark_dirty(img);

    /* Each sample is a pixel centre; the image spans NX*STEP degrees */
    span = pic_config.view_span_lon;
    if (span < 0.0001) span = 360.0;
    ppd = width / span;
    world = 360.0 * ppd;
    cx = pic_lon_to_x(d->lon0, width);
    w_px = AQ_NX * AQ_STEP * ppd;
    x0 = cx - w_px / 2.0;
    y0 = pic_lat_to_y(d->lat0 + AQ_NY * AQ_STEP / 2.0, height);
    y1 = pic_lat_to_y(d->lat0 - AQ_NY * AQ_STEP / 2.0, height);
    pthread_mutex_unlock(&d->mutex);

    for (rep = -1; rep <= 1; rep++) {
        double xr = x0 + rep * world;
        if (xr > width || xr + w_px < 0) continue;
        cairo_save(cr);
        cairo_translate(cr, xr, y0);
        cairo_scale(cr, w_px / AQ_NX, (y1 - y0) / AQ_NY);
        cairo_set_source_surface(cr, img, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_NONE);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    cairo_surface_destroy(img);
}

void pic_layer_render_airquality_badge(cairo_t *cr, int width, int height,
                                       time_t now, void *user_data)
{
    pic_airquality_t *d = (pic_airquality_t *)user_data;
    double qx, qy, fs = height / 90.0, pad = fs * 0.4, r, g, b, w, h;
    char text[64];
    cairo_text_extents_t ext;
    int aqi;

    (void)now;
    if (!d) return;
    if (pic_config.qth_lat == 0.0 && pic_config.qth_lon == 0.0) return;

    pthread_mutex_lock(&d->mutex);
    aqi = d->qth_aqi;
    if (!d->show_badge || aqi < 0) {
        pthread_mutex_unlock(&d->mutex);
        return;
    }
    if (d->qth_pm25 >= 0)
        snprintf(text, sizeof(text), "AQI %d  %s  PM2.5 %.0f", aqi,
                 pic_aq_category(aqi), d->qth_pm25);
    else
        snprintf(text, sizeof(text), "AQI %d  %s", aqi, pic_aq_category(aqi));
    pthread_mutex_unlock(&d->mutex);

    qx = pic_lon_to_x(pic_config.qth_lon, width);
    qy = pic_lat_to_y(pic_config.qth_lat, height);
    if (qx < 0 || qx > width || qy < 0 || qy > height) return;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, text, &ext);

    w = fs * 1.1 + pad + ext.x_advance + pad * 2;
    h = fs * 1.5;
    /* Start the badge so its coloured dot sits directly under the QTH
     * pin; centring the whole pill put the dot a state or two west. */
    qx -= pad + fs * 0.45;
    qy += fs * 1.6;
    if (qx < 2) qx = 2;
    if (qx + w > width - 2) qx = width - 2 - w;

    cairo_new_path(cr);
    cairo_arc(cr, qx + w - h / 2, qy + h / 2, h / 2, -PI / 2, PI / 2);
    cairo_arc(cr, qx + h / 2, qy + h / 2, h / 2, PI / 2, 3 * PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.72);
    cairo_fill(cr);

    pic_aq_colour(aqi, &r, &g, &b);
    cairo_new_path(cr);
    cairo_arc(cr, qx + pad + fs * 0.45, qy + h / 2, fs * 0.45, 0, 2 * PI);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_move_to(cr, qx + pad + fs * 1.1 + pad, qy + h / 2 + fs * 0.36);
    cairo_show_text(cr, text);
}
