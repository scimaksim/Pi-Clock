/*
 * layers_aircraft.c - Live aircraft overlay
 *
 * Each aircraft is a small plane glyph pointing along its track,
 * coloured by altitude (low = green through high = violet, ground =
 * grey). Military aircraft get an amber outline, emergencies (squawk
 * 7500/7600/7700) a red ring and label. Positions are dead-reckoned
 * from the last fetch using ground speed and track.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "aircraft.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "math_utils.h"

#define MAX_EXTRAPOLATE_S 90.0
#define MAX_LABELS 400

/* Simple label decluttering: skip a label that would overlap one
 * already drawn this frame. */
typedef struct { double x0, y0, x1, y1; } rect_t;

static int overlaps(const rect_t *r, int n, const rect_t *q)
{
    int i;
    for (i = 0; i < n; i++)
        if (q->x0 < r[i].x1 && q->x1 > r[i].x0 &&
            q->y0 < r[i].y1 && q->y1 > r[i].y0)
            return 1;
    return 0;
}

static void alt_colour(int alt, double *r, double *g, double *b)
{
    double f, h;
    if (alt < 0) { *r = *g = *b = 0.65; return; }      /* on ground */
    f = alt / 40000.0;
    if (f > 1) f = 1;
    /* Hue 120 (green) → 285 (violet), full saturation, bright */
    h = (120.0 + f * 165.0) / 60.0;
    {
        int i = (int)h;
        double x = h - i, q = 1 - x;
        switch (i % 6) {
        case 0: *r = 1; *g = x; *b = 0; break;
        case 1: *r = q; *g = 1; *b = 0; break;
        case 2: *r = 0; *g = 1; *b = x; break;
        case 3: *r = 0; *g = q; *b = 1; break;
        case 4: *r = x; *g = 0; *b = 1; break;
        default: *r = 1; *g = 0; *b = q; break;
        }
    }
    /* Lighten so the glyph reads on dark maps */
    *r = 0.35 + 0.65 * *r;
    *g = 0.35 + 0.65 * *g;
    *b = 0.35 + 0.65 * *b;
}

/* Plane glyph pointing north (up), centred on the origin, size s */
static void plane_path(cairo_t *cr, double s)
{
    cairo_move_to(cr, 0, -s);              /* nose */
    cairo_line_to(cr, s * 0.12, -s * 0.55);
    cairo_line_to(cr, s * 0.12, -s * 0.15);
    cairo_line_to(cr, s * 0.85, s * 0.15); /* right wing */
    cairo_line_to(cr, s * 0.85, s * 0.32);
    cairo_line_to(cr, s * 0.12, s * 0.15);
    cairo_line_to(cr, s * 0.1, s * 0.7);
    cairo_line_to(cr, s * 0.38, s * 0.88); /* right tailplane */
    cairo_line_to(cr, s * 0.38, s * 1.0);
    cairo_line_to(cr, 0, s * 0.9);
    cairo_line_to(cr, -s * 0.38, s * 1.0);
    cairo_line_to(cr, -s * 0.38, s * 0.88);
    cairo_line_to(cr, -s * 0.1, s * 0.7);
    cairo_line_to(cr, -s * 0.12, s * 0.15);
    cairo_line_to(cr, -s * 0.85, s * 0.32);
    cairo_line_to(cr, -s * 0.85, s * 0.15);
    cairo_line_to(cr, -s * 0.12, -s * 0.15);
    cairo_line_to(cr, -s * 0.12, -s * 0.55);
    cairo_close_path(cr);
}

void pic_layer_render_aircraft(cairo_t *cr, int width, int height,
                               time_t now, void *user_data)
{
    pic_aircraft_t *d = (pic_aircraft_t *)user_data;
    double zoom = 360.0 / (pic_config.view_span_lon > 0.0001 ?
                           pic_config.view_span_lon : 360.0);
    /* Glyphs grow a little with zoom so they stay recognisable */
    double s = height / 260.0 * (zoom > 4 ? 1.6 : 1.0 + 0.15 * zoom);
    double fs = height / 125.0;
    double dt;
    int i, pass, nlab = 0;
    static rect_t labs[MAX_LABELS];

    if (!d) return;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);

    pthread_mutex_lock(&d->mutex);
    dt = difftime(now, d->t_pos);
    if (dt < 0) dt = 0;
    if (dt > MAX_EXTRAPOLATE_S) dt = MAX_EXTRAPOLATE_S;

    /* Two passes so military and emergency aircraft draw on top */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < d->count; i++) {
            const pic_ac_t *a = &d->ac[i];
            int special = a->military || a->emergency;
            double lat = a->lat, lon = a->lon, x, y, r, g, b;

            if (special != pass) continue;
            /* Parked/taxiing aircraft only clutter airports at wide zoom */
            if (a->alt == -1 && zoom < 3.0 && !special) continue;

            if (a->track >= 0 && a->gs > 0 && a->alt != -1) {
                double dist = a->gs * dt / 3600.0 / 60.0;
                double tr = a->track * DEG_TO_RAD;
                lat += dist * cos(tr);
                lon += dist * sin(tr) / cos(lat * DEG_TO_RAD);
            }
            x = pic_lon_to_x(lon, width);
            y = pic_lat_to_y(lat, height);
            if (x < -s || x > width + s || y < -s || y > height + s) continue;

            alt_colour(a->alt, &r, &g, &b);

            if (a->emergency) {
                cairo_new_path(cr);
                cairo_arc(cr, x, y, s * 2.2, 0, 2 * PI);
                cairo_set_source_rgba(cr, 1, 0.1, 0.1, 0.9);
                cairo_set_line_width(cr, s * 0.35);
                cairo_stroke(cr);
            }

            cairo_save(cr);
            cairo_translate(cr, x, y);
            if (a->track >= 0) cairo_rotate(cr, a->track * DEG_TO_RAD);
            cairo_new_path(cr);
            plane_path(cr, s);
            cairo_set_source_rgba(cr, r, g, b, 0.95);
            cairo_fill_preserve(cr);
            if (a->military) cairo_set_source_rgba(cr, 1.0, 0.7, 0.1, 1.0);
            else             cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
            cairo_set_line_width(cr, a->military ? s * 0.16 : s * 0.12);
            cairo_stroke(cr);
            cairo_restore(cr);

            if ((d->labels == 2 || (d->labels == 1 && special)) && a->flight[0]) {
                char label[32];
                cairo_text_extents_t ext;
                rect_t q;
                if (a->emergency)
                    snprintf(label, sizeof(label), "%s EMERG", a->flight);
                else if (a->alt > 0 && d->labels == 2)
                    snprintf(label, sizeof(label), "%s %d", a->flight,
                             (a->alt + 50) / 100);   /* flight level */
                else
                    snprintf(label, sizeof(label), "%s", a->flight);
                cairo_text_extents(cr, label, &ext);
                q.x0 = x + s * 1.3 - 2;
                q.y0 = y - fs * 0.7;
                q.x1 = q.x0 + ext.x_advance + 4;
                q.y1 = y + fs * 0.6;
                /* Emergencies always get their label */
                if (!a->emergency && overlaps(labs, nlab, &q)) continue;
                if (nlab < MAX_LABELS) labs[nlab++] = q;
                cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
                cairo_move_to(cr, x + s * 1.3 + 1, y + fs * 0.35 + 1);
                cairo_show_text(cr, label);
                if (a->emergency) cairo_set_source_rgba(cr, 1, 0.35, 0.35, 1);
                else if (a->military) cairo_set_source_rgba(cr, 1, 0.8, 0.3, 1);
                else cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.95);
                cairo_move_to(cr, x + s * 1.3, y + fs * 0.35);
                cairo_show_text(cr, label);
            }
        }
    }
    pthread_mutex_unlock(&d->mutex);
}
