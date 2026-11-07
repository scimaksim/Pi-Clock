/*
 * layers_starlink.c - Starlink train overlay
 *
 * Each recent train is drawn as a string of small bright dots joined
 * by a faint line (head to tail), with a label at the lead satellite:
 *
 *   Starlink 2026-201  (2 days)
 *   Visible Sat 6:42 PM, up to 54 deg
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "starlink.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "math_utils.h"

static void when(time_t t, time_t now, char *out, size_t size)
{
    struct tm lt, ln;
    char hm[16], *p;
    localtime_r(&t, &lt);
    localtime_r(&now, &ln);
    strftime(hm, sizeof(hm), "%I:%M %p", &lt);
    p = hm[0] == '0' ? hm + 1 : hm;
    if (lt.tm_yday == ln.tm_yday && lt.tm_year == ln.tm_year) {
        snprintf(out, size, "today %s", p);
    } else {
        char day[8];
        strftime(day, sizeof(day), "%a", &lt);
        snprintf(out, size, "%s %s", day, p);
    }
}

void pic_layer_render_starlink(cairo_t *cr, int width, int height,
                               time_t now, void *user_data)
{
    pic_starlink_t *d = (pic_starlink_t *)user_data;
    double r = height / 420.0 + 1.0;
    double fs = height / 105.0;
    double wrap = pic_wrap_threshold_px(width);
    int ti, k;

    if (!d) return;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);

    pthread_mutex_lock(&d->mutex);
    for (ti = 0; ti < d->ntrains; ti++) {
        const pic_sl_train_t *t = &d->trains[ti];
        double px = 0, py = 0, lx = 0, ly = 0;
        char l1[64], l2[80];
        int days;

        if (t->nsats == 0) continue;

        /* Connecting line, then dots */
        cairo_set_line_width(cr, r * 0.6);
        cairo_set_source_rgba(cr, 0.85, 0.9, 1.0, 0.35);
        for (k = 0; k < t->nsats; k++) {
            double lat, lon, alt, x, y;
            pic_sat_position_at(&t->sats[k], now, &lat, &lon, &alt);
            x = pic_lon_to_x(lon, width);
            y = pic_lat_to_y(lat, height);
            if (k > 0 && fabs(x - px) < wrap && hypot(x - px, y - py) < height / 6.0) {
                cairo_move_to(cr, px, py);
                cairo_line_to(cr, x, y);
                cairo_stroke(cr);
            }
            px = x; py = y;
            if (k == 0) { lx = x; ly = y; }
        }
        for (k = 0; k < t->nsats; k++) {
            double lat, lon, alt, x, y;
            pic_sat_position_at(&t->sats[k], now, &lat, &lon, &alt);
            x = pic_lon_to_x(lon, width);
            y = pic_lat_to_y(lat, height);
            cairo_new_path(cr);
            cairo_arc(cr, x, y, r, 0, 2 * PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 0.9, 0.95);
            cairo_fill(cr);
        }

        /* Label at the leader */
        days = (int)(difftime(now, t->launched) / 86400.0);
        snprintf(l1, sizeof(l1), "Starlink %s  (%d day%s)", t->launch,
                 days, days == 1 ? "" : "s");
        if (t->pass_start) {
            char w[32];
            when(t->pass_start, now, w, sizeof(w));
            snprintf(l2, sizeof(l2), "Visible %s, up to %.0f\xc2\xb0",
                     w, t->pass_max_el);
        } else {
            snprintf(l2, sizeof(l2), "No visible pass in 48 h");
        }

        {
            cairo_text_extents_t e1, e2;
            double pad = fs * 0.35, w, h, x = lx + r * 4, y = ly - fs * 1.4;
            cairo_text_extents(cr, l1, &e1);
            cairo_set_font_size(cr, fs * 0.85);
            cairo_text_extents(cr, l2, &e2);
            cairo_set_font_size(cr, fs);
            w = (e1.x_advance > e2.x_advance ? e1.x_advance : e2.x_advance);
            h = fs * 2.3;
            if (x + w + pad > width) x = lx - r * 4 - w;

            cairo_set_source_rgba(cr, 0, 0, 0, 0.65);
            cairo_rectangle(cr, x - pad, y - pad, w + pad * 2, h + pad * 2);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 0.9, 0.95);
            cairo_move_to(cr, x, y + fs);
            cairo_show_text(cr, l1);
            cairo_set_font_size(cr, fs * 0.85);
            cairo_set_source_rgba(cr, t->pass_start ? 0.5 : 0.8,
                                  t->pass_start ? 1.0 : 0.8,
                                  t->pass_start ? 0.6 : 0.8, 0.95);
            cairo_move_to(cr, x, y + fs * 2.1);
            cairo_show_text(cr, l2);
            cairo_set_font_size(cr, fs);
        }
    }
    pthread_mutex_unlock(&d->mutex);
}
