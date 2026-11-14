/*
 * layers_volcano.c - Active volcano overlay layer
 *
 * Draws volcanoes from the Smithsonian / USGS Weekly Volcanic
 * Activity Report as small volcano icons:
 *
 *   - New activity / unrest:  red icon with a soft glow and an
 *                             ash plume, so new eruptions stand out
 *   - Continuing activity:    smaller orange icon
 *
 * Each icon can carry a name label on a dark pill (configurable).
 * The data changes weekly, so this layer re-renders once a minute.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "volcano.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "math_utils.h"

/* New activity: lava red. Continuing: ember orange. */
#define NEW_R 0.95
#define NEW_G 0.18
#define NEW_B 0.12
#define OLD_R 0.98
#define OLD_G 0.55
#define OLD_B 0.15

/*
 * draw_volcano_icon - Truncated cone with a crater notch.
 *
 *          __  __
 *         /  \/  \
 *        /        \
 *       /__________\
 *
 * (cx, cy) is the summit position; the icon sits above it so the
 * base of the cone is on the summit location.
 */
static void draw_volcano_icon(cairo_t *cr, double cx, double cy,
                              double size, double r, double g, double b,
                              int plume)
{
    double half = size / 2.0;
    double top = cy - size * 0.75;
    double lw = size * 0.08;

    if (lw < 1.0) lw = 1.0;

    /* Ash plume: three overlapping grey puffs above the crater */
    if (plume) {
        cairo_set_source_rgba(cr, 0.75, 0.75, 0.75, 0.75);
        cairo_new_path(cr);
        cairo_arc(cr, cx - size * 0.05, top - size * 0.20, size * 0.16, 0, 2 * PI);
        cairo_fill(cr);
        cairo_arc(cr, cx + size * 0.12, top - size * 0.38, size * 0.20, 0, 2 * PI);
        cairo_fill(cr);
        cairo_arc(cr, cx + size * 0.34, top - size * 0.55, size * 0.22, 0, 2 * PI);
        cairo_fill(cr);
    }

    cairo_new_path(cr);
    cairo_move_to(cr, cx - half, cy);
    cairo_line_to(cr, cx - size * 0.16, top);
    cairo_line_to(cr, cx - size * 0.06, top + size * 0.10);
    cairo_line_to(cr, cx + size * 0.06, top + size * 0.10);
    cairo_line_to(cr, cx + size * 0.16, top);
    cairo_line_to(cr, cx + half, cy);
    cairo_close_path(cr);

    cairo_set_source_rgba(cr, r, g, b, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.05, 0.05, 0.05, 0.9);
    cairo_set_line_width(cr, lw);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
}

static void draw_glow(cairo_t *cr, double cx, double cy, double radius,
                      double r, double g, double b)
{
    cairo_pattern_t *pat;

    pat = cairo_pattern_create_radial(cx, cy, 0, cx, cy, radius);
    cairo_pattern_add_color_stop_rgba(pat, 0.0, r, g, b, 0.55);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, r, g, b, 0.0);
    cairo_set_source(cr, pat);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, radius, 0, 2 * PI);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
}

static void draw_label(cairo_t *cr, double x, double y, const char *text,
                       double font_size, double r, double g, double b)
{
    cairo_text_extents_t ext;
    double pad = font_size * 0.3;
    double rx, ry, rw, rh, rr;

    cairo_text_extents(cr, text, &ext);
    rx = x - pad;
    ry = y - ext.height - pad;
    rw = ext.width + pad * 2;
    rh = ext.height + pad * 2;
    rr = pad;

    cairo_new_path(cr);
    cairo_arc(cr, rx + rw - rr, ry + rr, rr, -PI / 2, 0);
    cairo_arc(cr, rx + rw - rr, ry + rh - rr, rr, 0, PI / 2);
    cairo_arc(cr, rx + rr, ry + rh - rr, rr, PI / 2, PI);
    cairo_arc(cr, rx + rr, ry + rr, rr, PI, 3 * PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.65);
    cairo_fill(cr);

    /* Tint toward white so coloured text stays legible on the pill */
    cairo_set_source_rgba(cr, (r + 2.0) / 3.0, (g + 2.0) / 3.0,
                          (b + 2.0) / 3.0, 0.95);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
}

/*
 * pic_layer_render_volcano - Render all volcanoes in the weekly report.
 * user_data points to a pic_volcanoes_t.
 *
 * Continuing volcanoes are drawn first so new ones sit on top.
 */
void pic_layer_render_volcano(cairo_t *cr, int width, int height,
                              time_t now, void *user_data)
{
    pic_volcanoes_t *data = (pic_volcanoes_t *)user_data;
    double base_size, font_size;
    int pass, i;

    (void)now;
    if (!data) return;

    base_size = height / 55.0;
    font_size = height / 125.0;

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, font_size);

    pthread_mutex_lock(&data->mutex);

    for (pass = 0; pass < 2; pass++) {
        int want_new = (pass == 1);

        if (!want_new && data->new_only) continue;

        for (i = 0; i < data->count; i++) {
            pic_volcano_t *v = &data->items[i];
            double x, y, size, r, g, b;

            if (v->new_activity != want_new) continue;

            x = pic_lon_to_x(v->lon, width);
            y = pic_lat_to_y(v->lat, height);
            if (x < -base_size * 2 || x > width + base_size * 2 ||
                y < -base_size * 2 || y > height + base_size * 2)
                continue;

            if (want_new) {
                size = base_size * 1.25;
                r = NEW_R; g = NEW_G; b = NEW_B;
                draw_glow(cr, x, y - size * 0.4, size * 1.4, r, g, b);
            } else {
                size = base_size;
                r = OLD_R; g = OLD_G; b = OLD_B;
            }

            draw_volcano_icon(cr, x, y, size, r, g, b, want_new);

            if (data->show_labels && v->name[0])
                draw_label(cr, x + size * 0.6 + 3,
                           y - size * 0.2 + font_size * 0.35,
                           v->name, font_size, r, g, b);
        }
    }

    pthread_mutex_unlock(&data->mutex);
}
