/*
 * layers_lightning.c - Lightning strike overlay layer
 *
 * Renders recent lightning strikes from Blitzortung as small
 * lightning bolt icons on the world map. Each strike fades
 * through a colour sequence over its lifetime:
 *
 *   Start:  Bright white  (fresh strike, maximum visibility)
 *   Middle: Yellow        (ageing)
 *   End:    Deep red      (about to disappear)
 *
 * The fade duration is configurable (default 3 seconds) and
 * can be changed from the dashboard. After the fade completes,
 * the strike is no longer drawn.
 *
 * The lightning bolt shape is drawn procedurally with Cairo
 * path operations — no image assets required. The bolt is
 * small (scaled to display height) to avoid cluttering the
 * map during heavy storm activity.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "lightning.h"
#include "feature.h"
#include <stdio.h>
#include <string.h>
#include <cairo/cairo.h>
#include <math.h>
#include <sys/time.h>

#include "math_utils.h"

/*
 * fade_colour - Interpolate the strike colour based on age fraction.
 *
 * Maps the normalised age (0.0 = just struck, 1.0 = about to expire)
 * through a three-stop gradient:
 *
 *   0.0 → 0.3:  White (1,1,1) to Yellow (1,1,0)
 *   0.3 → 0.7:  Yellow (1,1,0) to Deep Red (0.8,0,0)
 *   0.7 → 1.0:  Deep Red fading to transparent
 *
 * The alpha channel starts at 1.0 and fades linearly to 0.0
 * over the final 30% of the fade window, giving a smooth
 * disappearance rather than a hard pop-off.
 */
static void fade_colour(double age_frac, double *r, double *g,
                        double *b, double *a)
{
    if (age_frac < 0.0) age_frac = 0.0;
    if (age_frac > 1.0) age_frac = 1.0;

    if (age_frac < 0.3) {
        /* White → Yellow: blue channel drops from 1 to 0 */
        double t = age_frac / 0.3;
        *r = 1.0;
        *g = 1.0;
        *b = 1.0 - t;
    } else if (age_frac < 0.7) {
        /* Yellow → Deep Red: green channel drops, red stays high */
        double t = (age_frac - 0.3) / 0.4;
        *r = 1.0 - 0.2 * t;   /* 1.0 → 0.8 */
        *g = 1.0 - t;          /* 1.0 → 0.0 */
        *b = 0.0;
    } else {
        /* Deep Red, fading out */
        *r = 0.8;
        *g = 0.0;
        *b = 0.0;
    }

    /* Alpha: full opacity for first 70%, then linear fade to 0 */
    if (age_frac < 0.7) {
        *a = 1.0;
    } else {
        *a = 1.0 - (age_frac - 0.7) / 0.3;
    }
}

/*
 * draw_lightning_bolt - Draw a small lightning bolt icon.
 *
 * The bolt is a simple 5-point zigzag path, filled with the
 * given colour. Size is specified as the total height of the
 * bolt in pixels. The bolt is centred on (cx, cy).
 *
 * Shape (normalised to height h, origin at centre):
 *
 *        *         <- top point
 *       / \
 *      /   \
 *     *-----*     <- upper zigzag
 *      \   /
 *       \ /
 *        *-----*  <- lower zigzag
 *         \   /
 *          \ /
 *           *     <- bottom point
 */
static void draw_lightning_bolt(cairo_t *cr, double cx, double cy,
                                double h, double r, double g,
                                double b, double a)
{
    double w = h * 0.35;   /* Bolt width proportional to height */
    double half_h = h / 2.0;

    cairo_save(cr);
    cairo_translate(cr, cx, cy);

    /* Draw the bolt as a filled polygon.
     * The shape is a classic zigzag lightning bolt. */
    cairo_move_to(cr,  0.0,         -half_h);       /* Top point       */
    cairo_line_to(cr,  w * 0.5,     -half_h * 0.15);/* Upper right     */
    cairo_line_to(cr, -w * 0.15,    -half_h * 0.05);/* Upper left step */
    cairo_line_to(cr,  w * 0.35,     half_h * 0.4); /* Lower right     */
    cairo_line_to(cr, -w * 0.3,      half_h * 0.05);/* Lower left step */
    cairo_line_to(cr,  0.0,          half_h);        /* Bottom point    */
    cairo_line_to(cr, -w * 0.35,     half_h * 0.15);/* Return left     */
    cairo_line_to(cr,  w * 0.15,     half_h * 0.05);/* Return step     */
    cairo_line_to(cr, -w * 0.5,     -half_h * 0.3); /* Upper return    */
    cairo_line_to(cr,  w * 0.15,    -half_h * 0.1); /* Final step      */
    cairo_close_path(cr);

    /* Fill with the fade colour */
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_fill_preserve(cr);

    /* Thin bright outline for visibility on any background */
    cairo_set_source_rgba(cr, r, g, b, a * 0.6);
    cairo_set_line_width(cr, 0.5);
    cairo_stroke(cr);

    cairo_restore(cr);
}

/*
 * pic_layer_render_lightning - Render all active lightning strikes.
 *
 * Iterates the circular buffer, skips strikes that have expired
 * (older than fade_ms), and draws each visible strike as a small
 * lightning bolt with colour determined by its age.
 *
 * The layer update interval is set to 1 second in the layer stack,
 * but since strikes fade over just a few seconds we want to
 * re-render frequently. On multi-core Pi the 1-second interval is
 * fine; on single-core Pi the layer still works but the fade
 * animation appears stepped rather than smooth.
 *
 * user_data points to a pic_lightning_t.
 */
void pic_layer_render_lightning(cairo_t *cr, int width, int height,
                                time_t now, void *user_data)
{
    pic_lightning_t *data = (pic_lightning_t *)user_data;
    struct timeval tv_now;
    int i, fade_ms;
    double bolt_h;

    (void)now; /* We use gettimeofday for sub-second precision */

    if (!data) return;

    gettimeofday(&tv_now, NULL);

    /*
     * Bolt size: scale to display height. At 1080p this gives a
     * bolt about 9 pixels tall — small enough not to clutter but
     * visible enough to spot. At 4K it's about 18 pixels.
     */
    bolt_h = height / 120.0;

    pthread_mutex_lock(&data->mutex);
    fade_ms = data->fade_ms;

    for (i = 0; i < data->count; i++) {
        pic_strike_t *s = &data->strikes[i];
        double age_ms, age_frac;
        double r, g, b, a;
        double sx, sy;

        /* Calculate age in milliseconds */
        age_ms = (tv_now.tv_sec - s->when.tv_sec) * 1000.0 +
                 (tv_now.tv_usec - s->when.tv_usec) / 1000.0;

        /* Skip expired strikes */
        if (age_ms < 0 || age_ms > fade_ms) continue;

        /* Normalised age: 0.0 = just struck, 1.0 = about to expire */
        age_frac = age_ms / fade_ms;

        /* Get the fade colour for this age */
        fade_colour(age_frac, &r, &g, &b, &a);

        /* Convert geographic coordinates to screen position */
        sx = pic_lon_to_x(s->lon, width);
        sy = pic_lat_to_y(s->lat, height);

        /*
         * Attract effect: on the very first frame (age < 1 second),
         * draw a large bright white bolt at 3x size to grab
         * attention. After that, normal size with the colour fade.
         */
        if (age_ms < 1000.0) {
            draw_lightning_bolt(cr, sx, sy, bolt_h * 3.0,
                                1.0, 1.0, 1.0, 1.0);
        } else {
            draw_lightning_bolt(cr, sx, sy, bolt_h, r, g, b, a);
        }
    }

    pthread_mutex_unlock(&data->mutex);
}

/*
 * Thunderstorm distance around home: rings at 10, 20 and 30 miles and
 * a note with the nearest strike in the last 30 minutes, e.g.
 * "Lightning 8 mi NW of home, 2 min ago · 5 strikes within 10 mi".
 * Shown only while there has been lightning within 100 miles.
 */
static const char *compass8(double b)
{
    static const char *d[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    b = fmod(b + 360.0 + 22.5, 360.0);
    return d[(int)(b / 45.0) % 8];
}

void pic_layer_render_lightning_near(cairo_t *cr, int width, int height,
                                     time_t now, void *user_data)
{
    pic_lightning_t *data = (pic_lightning_t *)user_data;
    double qlat = pic_config.qth_lat, qlon = pic_config.qth_lon;
    double best = 1e9, best_b = 0, mi_per_deg = 69.05;
    time_t best_t = 0;
    int i, n10 = 0, n30 = 0, total = 0;
    static const int rings[] = { 10, 20, 30 };

    (void)now;
    if (!data || (qlat == 0 && qlon == 0)) return;
    {
        struct timeval tv;
        time_t t;
        gettimeofday(&tv, NULL);
        t = tv.tv_sec;
        pthread_mutex_lock(&data->mutex);
        for (i = 0; i < data->near_count; i++) {
            const pic_strike_t *s = &data->near[i];
            double p1 = qlat * DEG_TO_RAD, p2 = s->lat * DEG_TO_RAD;
            double dl = (s->lon - qlon) * DEG_TO_RAD, c, d;
            if (t - s->when.tv_sec > PIC_NEAR_KEEP_S) continue;
            c = sin(p1) * sin(p2) + cos(p1) * cos(p2) * cos(dl);
            if (c > 1) c = 1;
            d = acos(c) * 3958.8;                     /* miles */
            total++;
            if (d < 10) n10++;
            if (d < 30) n30++;
            if (d < best) {
                best = d;
                best_t = s->when.tv_sec;
                best_b = atan2(sin(dl) * cos(p2),
                               cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) * RAD_TO_DEG;
            }
        }
        pthread_mutex_unlock(&data->mutex);
        if (!total) return;

        /* Rings (ellipses: the map is equirectangular) and the
         * recent strikes near home as small crosses fading with age */
        {
            double hx = pic_lon_to_x(qlon, width), hy = pic_lat_to_y(qlat, height);
            double span = pic_config.view_span_lon > 0 ? pic_config.view_span_lon : 360;
            double span_lat = pic_config.view_span_lat > 0 ? pic_config.view_span_lat : 180;
            double ppd_x = width / span, ppd_y = height / span_lat;
            double fs = height / 95.0;
            cairo_save(cr);
            pthread_mutex_lock(&data->mutex);
            for (i = 0; i < data->near_count; i++) {
                const pic_strike_t *st = &data->near[i];
                double age = (double)(t - st->when.tv_sec), a, x, y, k = fs * 0.35;
                if (age > PIC_NEAR_KEEP_S) continue;
                a = 1.0 - 0.75 * age / PIC_NEAR_KEEP_S;
                x = pic_lon_to_x(st->lon, width);
                y = pic_lat_to_y(st->lat, height);
                cairo_new_path(cr);
                cairo_move_to(cr, x - k, y - k); cairo_line_to(cr, x + k, y + k);
                cairo_move_to(cr, x - k, y + k); cairo_line_to(cr, x + k, y - k);
                cairo_set_source_rgba(cr, 1.0, 0.95, 0.4, a);
                cairo_set_line_width(cr, height / 540.0 * 1.5);
                cairo_stroke(cr);
            }
            pthread_mutex_unlock(&data->mutex);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, fs);
            for (i = 0; i < 3; i++) {
                double ry = rings[i] / mi_per_deg * ppd_y;
                double rx = rings[i] / (mi_per_deg * cos(qlat * DEG_TO_RAD)) * ppd_x;
                double rr, gg, bb, a;
                char lab[12];
                if (ry < 6) continue;               /* too small to draw */
                a = (best < rings[i] && (i == 0 || best >= rings[i - 1])) ? 0.95 : 0.45;
                if (rings[i] == 10)      { rr = 1.0; gg = 0.3; bb = 0.25; }
                else if (rings[i] == 20) { rr = 1.0; gg = 0.6; bb = 0.2; }
                else                     { rr = 1.0; gg = 0.9; bb = 0.35; }
                cairo_save(cr);
                cairo_translate(cr, hx, hy);
                cairo_scale(cr, rx, ry);
                cairo_new_path(cr);
                cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
                cairo_restore(cr);
                cairo_set_source_rgba(cr, rr, gg, bb, a);
                cairo_set_line_width(cr, height / 540.0 * (a > 0.9 ? 2.2 : 1.2));
                cairo_stroke(cr);
                /* Label on the ring, each at its own angle (10 mi at
                 * the east, 30 mi near the top) so they don't collide */
                if (rx >= fs * 2.6) {
                    cairo_text_extents_t e;
                    double th = (15.0 + 30.0 * i) * DEG_TO_RAD;
                    snprintf(lab, sizeof(lab), "%d mi", rings[i]);
                    cairo_text_extents(cr, lab, &e);
                    cairo_new_path(cr);
                    cairo_move_to(cr, hx + rx * cos(th) + 3, hy - ry * sin(th) + fs * 0.35);
                    cairo_text_path(cr, lab);
                    cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
                    cairo_set_line_width(cr, fs * 0.3);
                    cairo_stroke_preserve(cr);
                    cairo_set_source_rgba(cr, rr, gg, bb, 1.0);
                    cairo_fill(cr);
                }
            }
            cairo_restore(cr);
        }

        /* Note */
        {
            char text[160], ago[24];
            long m = (long)(t - best_t) / 60;
            double rr = 1.0, gg = best < 10 ? 0.4 : best < 30 ? 0.7 : 0.9, bb = 0.35;
            if (m < 1) snprintf(ago, sizeof(ago), "just now");
            else snprintf(ago, sizeof(ago), "%ld min ago", m);
            if (best < 1)
                snprintf(text, sizeof(text), "Lightning overhead (%s)", ago);
            else
                snprintf(text, sizeof(text), "Lightning %.0f mi %s of home, %s",
                         best, compass8(best_b), ago);
            if (n10)
                snprintf(text + strlen(text), sizeof(text) - strlen(text),
                         " \xc2\xb7 %d strike%s within 10 mi (30 min)", n10, n10 == 1 ? "" : "s");
            else if (n30)
                snprintf(text + strlen(text), sizeof(text) - strlen(text),
                         " \xc2\xb7 %d within 30 mi (30 min)", n30);
            pic_draw_legend_line(cr, width, height, 0, text, rr, gg, bb);
        }
    }
}
