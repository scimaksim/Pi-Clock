/*
 * layers_nwsalert.c - NWS weather alert overlay + home alert banner
 *
 * Map layer: storm-based warning polygons (tornado, severe
 * thunderstorm, flash flood, ...) filled in NWS hazard colours with a
 * bright outline, most severe drawn last so it sits on top.
 *
 * Banner (drawn as the layer's legend, so it stays fully opaque
 * whatever the layer opacity): when alerts are in effect at the home
 * QTH, up to three pills appear top-centre between the HUD clocks,
 * e.g.  [!] TORNADO WARNING  until 4:15 PM
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "nwsalert.h"
#include <cairo/cairo.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "math_utils.h"

#define BANNER_MAX_LINES 3

static void draw_polygon(cairo_t *cr, const pic_nws_t *d,
                         const pic_nws_alert_t *a, int width, int height)
{
    double lw = height / 900.0 + 0.6;
    double wrap = pic_wrap_threshold_px(width);
    double px0 = 0;
    double minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
    int k;

    cairo_new_path(cr);
    for (k = 0; k < a->nvertex; k++) {
        double x = pic_lon_to_x(d->vlon[a->first_vertex + k], width);
        double y = pic_lat_to_y(d->vlat[a->first_vertex + k], height);
        /* Warning polygons are small; if one straddles the map edge
         * just skip it rather than smearing it across the screen. */
        if (k > 0 && fabs(x - px0) > wrap) return;
        if (k == 0) cairo_move_to(cr, x, y);
        else        cairo_line_to(cr, x, y);
        px0 = x;
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    cairo_close_path(cr);

    cairo_set_source_rgba(cr, a->r, a->g, a->b, 0.28);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
    cairo_set_line_width(cr, lw * 2.5);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, a->r, a->g, a->b, 1.0);
    cairo_set_line_width(cr, lw);
    cairo_stroke(cr);

    /* At continental zoom a warning box can be a few pixels across;
     * add a marker dot so severe warnings stay visible. */
    if (maxx - minx < height / 120.0 && maxy - miny < height / 120.0 &&
        a->severity >= NWS_SEV_SEVERE) {
        double r = height / 240.0 + 1.5;
        cairo_new_path(cr);
        cairo_arc(cr, (minx + maxx) / 2, (miny + maxy) / 2, r, 0, 2 * PI);
        cairo_set_source_rgba(cr, a->r, a->g, a->b, 0.9);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
        cairo_set_line_width(cr, lw);
        cairo_stroke(cr);
    }
}

void pic_layer_render_nwsalert(cairo_t *cr, int width, int height,
                               time_t now, void *user_data)
{
    pic_nws_t *d = (pic_nws_t *)user_data;
    int sev, i;

    if (!d) return;

    pthread_mutex_lock(&d->mutex);
    /* Paint in ascending severity so tornado warnings end up on top */
    for (sev = NWS_SEV_UNKNOWN; sev <= NWS_SEV_EXTREME; sev++) {
        for (i = 0; i < d->nmap; i++) {
            const pic_nws_alert_t *a = &d->map[i];
            if (a->severity != sev) continue;
            if (a->expires && a->expires < now) continue;
            draw_polygon(cr, d, a, width, height);
        }
    }
    pthread_mutex_unlock(&d->mutex);
}

/* "until 4:15 PM" (today) or "until Sat 9:00 AM" */
static void format_until(time_t t, time_t now, char *out, size_t size)
{
    struct tm lt, ln;
    char hm[16], *p;

    if (!t) { out[0] = '\0'; return; }
    localtime_r(&t, &lt);
    localtime_r(&now, &ln);
    strftime(hm, sizeof(hm), "%I:%M %p", &lt);
    p = (hm[0] == '0') ? hm + 1 : hm;   /* drop leading zero */

    if (lt.tm_yday == ln.tm_yday && lt.tm_year == ln.tm_year) {
        snprintf(out, size, "until %s", p);
    } else {
        char day[8];
        strftime(day, sizeof(day), "%a", &lt);
        snprintf(out, size, "until %s %s", day, p);
    }
}

/* Warning triangle with "!" */
static void draw_warning_icon(cairo_t *cr, double x, double y, double s,
                              double r, double g, double b)
{
    cairo_new_path(cr);
    cairo_move_to(cr, x + s / 2, y);
    cairo_line_to(cr, x + s, y + s * 0.9);
    cairo_line_to(cr, x, y + s * 0.9);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_rectangle(cr, x + s * 0.45, y + s * 0.28, s * 0.1, s * 0.34);
    cairo_fill(cr);
    cairo_rectangle(cr, x + s * 0.45, y + s * 0.68, s * 0.1, s * 0.1);
    cairo_fill(cr);
}

void pic_layer_render_nwsalert_banner(cairo_t *cr, int width, int height,
                                      time_t now, void *user_data)
{
    pic_nws_t *d = (pic_nws_t *)user_data;
    double fs = height / 55.0;
    double pad = fs * 0.45;
    double y = height / 40.0;   /* aligned with the HUD top margin */
    int i, shown = 0;

    if (!d) return;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);

    pthread_mutex_lock(&d->mutex);
    if (!d->show_banner) {
        pthread_mutex_unlock(&d->mutex);
        return;
    }

    for (i = 0; i < d->nhome && shown < BANNER_MAX_LINES; i++) {
        const pic_nws_alert_t *a = &d->home[i];
        char ev[64], until[32], text[112];
        cairo_text_extents_t ext;
        double w, h, x, icon = fs * 1.1;
        int k;

        if (a->severity < d->banner_min_sev) continue;
        if (a->expires && a->expires < now) continue;

        for (k = 0; a->event[k] && k < (int)sizeof(ev) - 1; k++)
            ev[k] = (char)toupper((unsigned char)a->event[k]);
        ev[k] = '\0';
        format_until(a->expires, now, until, sizeof(until));
        snprintf(text, sizeof(text), "%s   %s", ev, until);

        cairo_text_extents(cr, text, &ext);
        w = icon + pad + ext.x_advance + pad * 2;
        h = fs * 1.6;
        x = (width - w) / 2.0;

        /* Pill in the alert colour, darkened for legibility */
        cairo_new_path(cr);
        cairo_arc(cr, x + w - h / 2, y + h / 2, h / 2, -PI / 2, PI / 2);
        cairo_arc(cr, x + h / 2, y + h / 2, h / 2, PI / 2, 3 * PI / 2);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, a->r * 0.35, a->g * 0.35, a->b * 0.35, 0.92);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, a->r, a->g, a->b, 1.0);
        cairo_set_line_width(cr, fs * 0.12);
        cairo_stroke(cr);

        draw_warning_icon(cr, x + pad, y + (h - icon) / 2 + icon * 0.05,
                          icon, a->r, a->g, a->b);

        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, x + pad + icon + pad, y + h / 2 + fs * 0.36);
        cairo_show_text(cr, text);

        y += h + fs * 0.35;
        shown++;
    }

    pthread_mutex_unlock(&d->mutex);
}
