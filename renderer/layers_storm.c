/*
 * layers_storm.c - Tropical cyclone overlay layer
 *
 * For each current storm from GDACS:
 *   - forecast uncertainty cone: translucent white fill + outline
 *   - track: solid line for the observed path, dashed for the
 *     forecast, coloured per leg by Saffir-Simpson category
 *   - forecast positions as small dots
 *   - a hurricane symbol at the current position, sized and coloured
 *     by category, with a pulsing ring for GDACS orange/red alerts
 *   - a label: name, category and max wind, plus the distance from
 *     the home QTH when the storm is (or is forecast to be) nearby
 *
 * Shapes that cross the antimeridian are drawn with "unwrapped"
 * longitudes (each vertex placed relative to the previous one) and
 * repeated one world-width either side, so a cone straddling the
 * date line is filled correctly on both edges of the map.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "storm.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "math_utils.h"

/* Nearby threshold for the distance line in the label (km) */
#define STORM_NEAR_KM 1500.0

/* Standard Saffir-Simpson colours (as used on NHC/JTWC style maps) */
static const double cat_rgb[7][3] = {
    { 0.37, 0.73, 1.00 },   /* TD  */
    { 0.00, 0.98, 0.96 },   /* TS  */
    { 1.00, 1.00, 0.80 },   /* C1  */
    { 1.00, 0.91, 0.46 },   /* C2  */
    { 1.00, 0.76, 0.25 },   /* C3  */
    { 1.00, 0.56, 0.13 },   /* C4  */
    { 1.00, 0.38, 0.38 },   /* C5  */
};

static const char *cat_name[7] = {
    "Tropical Depression", "Tropical Storm",
    "Cat 1", "Cat 2", "Cat 3", "Cat 4", "Cat 5"
};

static int clamp_cat(int c)
{
    return c < 0 ? 0 : c > 6 ? 6 : c;
}

/* Pixels per degree of longitude at the current viewport */
static double px_per_deg(int width)
{
    double span = pic_config.view_span_lon;
    if (span < 0.0001) span = 360.0;
    return width / span;
}

/*
 * unwrap_x - X for `lon`, continuing smoothly from the previous vertex
 * (prev_lon / prev_x) instead of jumping across the map edge.
 */
static double unwrap_x(double lon, double prev_lon, double prev_x, int width)
{
    double d = lon - prev_lon;
    while (d > 180)  d -= 360;
    while (d < -180) d += 360;
    return prev_x + d * px_per_deg(width);
}

static void draw_cone(cairo_t *cr, const pic_storm_t *s, int width, int height)
{
    double world = 360.0 * px_per_deg(width);
    int k, rep;

    if (s->ncone < 3) return;

    for (rep = -1; rep <= 1; rep++) {
        double x = pic_lon_to_x(s->cone_lon[0], width) + rep * world;
        double prev_lon = s->cone_lon[0];

        cairo_new_path(cr);
        cairo_move_to(cr, x, pic_lat_to_y(s->cone_lat[0], height));
        for (k = 1; k < s->ncone; k++) {
            x = unwrap_x(s->cone_lon[k], prev_lon, x, width);
            prev_lon = s->cone_lon[k];
            cairo_line_to(cr, x, pic_lat_to_y(s->cone_lat[k], height));
        }
        cairo_close_path(cr);

        cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
        cairo_set_line_width(cr, height / 700.0 + 0.5);
        cairo_stroke(cr);
    }
}

static void draw_track(cairo_t *cr, const pic_storm_t *s, int width, int height)
{
    double world = 360.0 * px_per_deg(width);
    double lw = height / 360.0;
    double dash[2];
    int k, rep;

    if (s->ntrack < 2) return;
    if (lw < 1.5) lw = 1.5;
    dash[0] = lw * 3; dash[1] = lw * 2;

    for (rep = -1; rep <= 1; rep++) {
        double x0 = pic_lon_to_x(s->track[0].lon, width) + rep * world;
        double y0 = pic_lat_to_y(s->track[0].lat, height);

        for (k = 1; k < s->ntrack; k++) {
            const pic_storm_pt_t *a = &s->track[k - 1];
            const pic_storm_pt_t *b = &s->track[k];
            double x1 = unwrap_x(b->lon, a->lon, x0, width);
            double y1 = pic_lat_to_y(b->lat, height);
            const double *c = cat_rgb[clamp_cat(a->cat)];

            /* Dark casing first so the line reads over any basemap */
            cairo_new_path(cr);
            cairo_move_to(cr, x0, y0);
            cairo_line_to(cr, x1, y1);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_dash(cr, NULL, 0, 0);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
            cairo_set_line_width(cr, lw * 2.2);
            cairo_stroke_preserve(cr);

            if (b->forecast) cairo_set_dash(cr, dash, 2, 0);
            cairo_set_source_rgba(cr, c[0], c[1], c[2], 0.95);
            cairo_set_line_width(cr, lw);
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0);

            if (b->forecast) {
                cairo_new_path(cr);
                cairo_arc(cr, x1, y1, lw * 1.6, 0, 2 * PI);
                cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, c[0], c[1], c[2], 0.95);
                cairo_set_line_width(cr, lw * 0.6);
                cairo_stroke(cr);
            }

            x0 = x1;
            y0 = y1;
        }
    }
}

/*
 * draw_hurricane_symbol - The standard tropical cyclone glyph: a
 * filled centre with two curved arms (counter-clockwise in the
 * northern hemisphere, mirrored in the southern).
 */
static void draw_hurricane_symbol(cairo_t *cr, double x, double y, double r,
                                  const double *c, int southern)
{
    int arm;
    double flip = southern ? -1.0 : 1.0;

    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, flip, 1.0);

    for (arm = 0; arm < 2; arm++) {
        cairo_save(cr);
        cairo_rotate(cr, arm * PI);
        cairo_new_path(cr);
        /* Arm: an arc sweeping out from the ring */
        cairo_move_to(cr, 0, -r * 0.55);
        cairo_curve_to(cr, r * 0.9, -r * 0.75,
                           r * 1.5, -r * 1.35,
                           r * 1.1, -r * 1.9);
        cairo_curve_to(cr, r * 1.05, -r * 1.25,
                           r * 0.55, -r * 0.95,
                           -r * 0.05, -r * 0.95);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, c[0], c[1], c[2], 1.0);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
        cairo_set_line_width(cr, r * 0.12);
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    /* Ring with dark eye */
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, r * 0.75, 0, 2 * PI);
    cairo_set_source_rgba(cr, c[0], c[1], c[2], 1.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
    cairo_set_line_width(cr, r * 0.12);
    cairo_stroke(cr);
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, r * 0.3, 0, 2 * PI);
    cairo_set_source_rgba(cr, 0.05, 0.05, 0.1, 0.95);
    cairo_fill(cr);

    cairo_restore(cr);
}

/*
 * draw_label - Two-line label on a dark pill. Placed to the right of
 * the symbol at x_right, or flipped to end at x_left if it would run
 * off the right edge of the screen.
 */
static void draw_label(cairo_t *cr, double x_right, double x_left,
                       int screen_w, double y,
                       const char *l1, const char *l2,
                       double fs, const double *c)
{
    cairo_text_extents_t e1, e2;
    double pad = fs * 0.35, w, h, rr = pad, x;

    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, l1, &e1);
    w = e1.x_advance;
    h = fs * 1.2;
    if (l2 && *l2) {
        cairo_set_font_size(cr, fs * 0.85);
        cairo_text_extents(cr, l2, &e2);
        if (e2.x_advance > w) w = e2.x_advance;
        h += fs * 1.05;
    }

    x = x_right;
    if (x + w + pad > screen_w) x = x_left - w;

    cairo_new_path(cr);
    cairo_arc(cr, x + w + pad - rr, y - pad + rr, rr, -PI / 2, 0);
    cairo_arc(cr, x + w + pad - rr, y + h + pad - rr, rr, 0, PI / 2);
    cairo_arc(cr, x - pad + rr, y + h + pad - rr, rr, PI / 2, PI);
    cairo_arc(cr, x - pad + rr, y - pad + rr, rr, PI, 3 * PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
    cairo_fill(cr);

    cairo_set_font_size(cr, fs);
    cairo_set_source_rgba(cr, c[0], c[1], c[2], 1.0);
    cairo_move_to(cr, x, y + fs);
    cairo_show_text(cr, l1);

    if (l2 && *l2) {
        cairo_set_font_size(cr, fs * 0.85);
        cairo_set_source_rgba(cr, 0.92, 0.92, 0.92, 1.0);
        cairo_move_to(cr, x, y + fs * 2.15);
        cairo_show_text(cr, l2);
    }
}

void pic_layer_render_storm(cairo_t *cr, int width, int height,
                            time_t now, void *user_data)
{
    pic_storms_t *data = (pic_storms_t *)user_data;
    double r = height / 90.0;
    double fs = height / 95.0;
    int i;

    (void)now;
    if (!data) return;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);

    pthread_mutex_lock(&data->mutex);

    /* Cones first (underneath every track), then tracks, then symbols */
    if (data->show_cone) {
        for (i = 0; i < data->count; i++)
            if (data->storms[i].cat >= data->min_cat)
                draw_cone(cr, &data->storms[i], width, height);
    }
    if (data->show_track) {
        for (i = 0; i < data->count; i++)
            if (data->storms[i].cat >= data->min_cat)
                draw_track(cr, &data->storms[i], width, height);
    }

    for (i = 0; i < data->count; i++) {
        const pic_storm_t *s = &data->storms[i];
        const double *c = cat_rgb[clamp_cat(s->cat)];
        double x = pic_lon_to_x(s->lon, width);
        double y = pic_lat_to_y(s->lat, height);
        double sr = r * (0.8 + 0.1 * s->cat);
        char l1[96], l2[96];

        if (s->cat < data->min_cat) continue;

        /* Alert halo for GDACS orange/red events */
        if (s->alert > 0) {
            cairo_new_path(cr);
            cairo_arc(cr, x, y, sr * 2.6, 0, 2 * PI);
            if (s->alert == 2) cairo_set_source_rgba(cr, 1.0, 0.15, 0.15, 0.85);
            else               cairo_set_source_rgba(cr, 1.0, 0.6, 0.1, 0.85);
            cairo_set_line_width(cr, sr * 0.25);
            cairo_stroke(cr);
        }

        draw_hurricane_symbol(cr, x, y, sr, c, s->lat < 0);

        snprintf(l1, sizeof(l1), "%s  %s  %.0f mph", s->name,
                 cat_name[clamp_cat(s->cat)], s->wind_kmh * 0.621371);
        l2[0] = '\0';
        if (s->dist_now_km >= 0 && s->dist_now_km < STORM_NEAR_KM) {
            snprintf(l2, sizeof(l2), "%.0f mi (%.0f km) from QTH",
                     s->dist_now_km * 0.621371, s->dist_now_km);
        } else if (s->dist_min_km >= 0 && s->dist_min_km < STORM_NEAR_KM) {
            snprintf(l2, sizeof(l2), "Forecast within %.0f mi of QTH",
                     s->dist_min_km * 0.621371);
        }

        draw_label(cr, x + sr * 2.2, x - sr * 2.2, width, y - fs * 0.6,
                   l1, l2, fs, c);
    }

    pthread_mutex_unlock(&data->mutex);
}
