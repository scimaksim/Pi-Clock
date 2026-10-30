/*
 * applet_astro.c - "Tonight's Sky" observing conditions applet
 *
 * Layout (top to bottom):
 *
 *   Tonight's Sky
 *   Sunset  6:21 PM        Sunrise  7:35 AM
 *   Dark    7:47 PM  to    6:09 AM          (astronomical twilight)
 *   (moon) 74% waning      Rise 10:18 PM  Set 12:42 PM
 *
 *            6p   9p   12a   3a   6a
 *   Sky     [######################]   day / twilight / night
 *   Cloud   [######################]   \
 *   Transp  [######################]    > Astrospheric colours
 *   Seeing  [######################]   /
 *   Moon    [######################]   moon above horizon
 *
 *   Best: 11 PM - 4 AM  clear and dark
 *   Astrospheric  updated 2:05 PM
 *
 * The strip covers 14 hours from the current hour, or from an hour
 * before sunset when it is still daytime. Sun and moon rows are
 * computed locally; the three forecast rows need an Astrospheric key.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "applet.h"
#include "astro.h"
#include "config.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "math_utils.h"

#define STRIP_HOURS 14

/* "7:35 PM" (or "--" for 0) */
static void hm(time_t t, char *out, size_t size)
{
    struct tm lt;
    char b[16];
    if (!t) { snprintf(out, size, "--"); return; }
    localtime_r(&t, &lt);
    strftime(b, sizeof(b), "%I:%M %p", &lt);
    snprintf(out, size, "%s", b[0] == '0' ? b + 1 : b);
}

/* "8 PM" — for whole-hour times */
static void hour_ampm(time_t t, char *out, size_t size)
{
    struct tm lt;
    int h;
    localtime_r(&t, &lt);
    h = lt.tm_hour % 12;
    snprintf(out, size, "%d %s", h ? h : 12, lt.tm_hour < 12 ? "AM" : "PM");
}

/* "9p", "12a" */
static void short_hour(time_t t, char *out, size_t size)
{
    struct tm lt;
    int h;
    localtime_r(&t, &lt);
    h = lt.tm_hour % 12;
    snprintf(out, size, "%d%c", h ? h : 12, lt.tm_hour < 12 ? 'a' : 'p');
}

static void set_rgb(cairo_t *cr, unsigned rgb, double a)
{
    cairo_set_source_rgba(cr, ((rgb >> 16) & 0xFF) / 255.0,
                          ((rgb >> 8) & 0xFF) / 255.0,
                          (rgb & 0xFF) / 255.0, a);
}

static void draw_moon_disc(cairo_t *cr, double cx, double cy, double r,
                           double illum, int waxing)
{
    /* Dark disc, then the lit part: a half-disc plus/minus an
     * elliptical terminator of half-width |1 - 2*illum| * r. */
    double k = 1.0 - 2.0 * illum;   /* +1 new ... -1 full */
    double side = waxing ? 1.0 : -1.0;

    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * PI);
    cairo_set_source_rgba(cr, 0.25, 0.25, 0.3, 1);
    cairo_fill(cr);

    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, side, 1);
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, r, -PI / 2, PI / 2);            /* lit limb */
    cairo_save(cr);
    cairo_scale(cr, fabs(k) < 1e-3 ? 1e-3 : fabs(k), 1);
    /* Terminator back from bottom to top: through the right side for
     * a crescent (k > 0), through the left side for a gibbous moon. */
    if (k > 0) cairo_arc_negative(cr, 0, 0, r, PI / 2, -PI / 2);
    else       cairo_arc(cr, 0, 0, r, PI / 2, 3 * PI / 2);
    cairo_restore(cr);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.95, 0.93, 0.85, 1);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* Find the forecast hour covering time t (within 30 min), or NULL. */
static const pic_astro_hour_t *forecast_at(const pic_astro_t *a, time_t t)
{
    int i;
    for (i = 0; i < a->nhours; i++)
        if (llabs((long long)(a->hours[i].t - t)) <= 1800)
            return &a->hours[i];
    return NULL;
}

double pic_applet_render_astro(cairo_t *cr, double width,
                               time_t now, void *user_data)
{
    pic_astro_t *a = (pic_astro_t *)user_data;
    double lat = pic_config.qth_lat, lon = pic_config.qth_lon;
    double title_fs = width / 12.0;
    double fs = width / 17.0;
    double line_h = fs * 1.45;
    double label_w = fs * 4.2;
    double cell_w = (width - label_w) / STRIP_HOURS;
    double row_h = fs * 1.05;
    double total_h;
    int have_fc, nrows;
    time_t sunset, sunrise, dark_start, dark_end, moonrise, moonset, start;
    double illum;
    int waxing, i, sun_up;
    char b1[16], b2[16], line[96];
    double y;

    if (!a) return 0;

    /* Forecast rows only appear when there is forecast data */
    pthread_mutex_lock(&a->mutex);
    have_fc = a->nhours > 0;
    pthread_mutex_unlock(&a->mutex);
    nrows = have_fc ? 5 : 2;
    total_h = title_fs * 1.5 + line_h * 3 + fs * 1.4 +
              (row_h + 2) * nrows + line_h * 2.3;
    if (!cr) return total_h;

    /* ── Local sun and moon events ─────────────────────────── */
    sun_up     = pic_astro_sun_alt(now, lat, lon) > ASTRO_SUN_H0;
    sunset     = pic_astro_next_crossing(0, now, lat, lon, ASTRO_SUN_H0, 0, 30);
    sunrise    = pic_astro_next_crossing(0, sun_up ? sunset : now, lat, lon,
                                         ASTRO_SUN_H0, 1, 30);
    dark_start = pic_astro_next_crossing(0, sun_up ? sunset : now - 12 * 3600,
                                         lat, lon, ASTRO_TWILIGHT_H0, 0, 30);
    dark_end   = dark_start ? pic_astro_next_crossing(0, dark_start, lat, lon,
                                         ASTRO_TWILIGHT_H0, 1, 30) : 0;
    moonrise   = pic_astro_next_crossing(1, now, lat, lon, ASTRO_MOON_H0, 1, 30);
    moonset    = pic_astro_next_crossing(1, now, lat, lon, ASTRO_MOON_H0, 0, 30);
    illum      = pic_astro_moon_illum(now, &waxing);

    /* Strip starts at the current hour at night, or an hour before
     * sunset during the day. */
    start = now - (now % 3600);
    if (sun_up && sunset) {
        time_t s = sunset - 3600;
        start = s - (s % 3600);
    }

    /* ── Title ─────────────────────────────────────────────── */
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, title_fs);
    cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.9);
    cairo_move_to(cr, 0, title_fs);
    cairo_show_text(cr, sun_up ? "Tonight's Sky" : "Sky Tonight");
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.3);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, 0, title_fs + 4);
    cairo_line_to(cr, width, title_fs + 4);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs);
    y = title_fs * 1.5 + fs;

    /* ── Sun / dark / moon lines ───────────────────────────── */
    hm(sunset, b1, sizeof(b1));
    hm(sunrise, b2, sizeof(b2));
    snprintf(line, sizeof(line), "Sunset %s   Sunrise %s", b1, b2);
    cairo_set_source_rgba(cr, 1.0, 0.8, 0.45, 0.9);
    cairo_move_to(cr, 0, y);
    cairo_show_text(cr, line);
    y += line_h;

    hm(dark_start, b1, sizeof(b1));
    hm(dark_end, b2, sizeof(b2));
    if (dark_start)
        snprintf(line, sizeof(line), "Dark  %s to %s", b1, b2);
    else
        snprintf(line, sizeof(line), "No astronomical darkness");
    cairo_set_source_rgba(cr, 0.6, 0.7, 1.0, 0.9);
    cairo_move_to(cr, 0, y);
    cairo_show_text(cr, line);
    y += line_h;

    draw_moon_disc(cr, fs * 0.55, y - fs * 0.35, fs * 0.55, illum, waxing);
    hm(moonrise, b1, sizeof(b1));
    hm(moonset, b2, sizeof(b2));
    snprintf(line, sizeof(line), "%.0f%%  Rise %s  Set %s",
             illum * 100.0, b1, b2);
    cairo_set_source_rgba(cr, 0.9, 0.9, 0.85, 0.9);
    cairo_move_to(cr, fs * 1.4, y);
    cairo_show_text(cr, line);
    y += line_h * 0.6;

    /* ── Hour labels ───────────────────────────────────────── */
    cairo_set_font_size(cr, fs * 0.8);
    cairo_set_source_rgba(cr, 0.7, 0.7, 0.75, 0.9);
    for (i = 0; i <= STRIP_HOURS; i += 3) {
        cairo_text_extents_t e;
        short_hour(start + (time_t)i * 3600, b1, sizeof(b1));
        cairo_text_extents(cr, b1, &e);
        cairo_move_to(cr, label_w + i * cell_w - e.x_advance / 2, y + fs * 0.8);
        cairo_show_text(cr, b1);
    }
    y += fs * 1.4;

    /* ── Strip rows ────────────────────────────────────────── */
    pthread_mutex_lock(&a->mutex);
    {
        static const char *labels[5] = { "Sky", "Cloud", "Transp", "Seeing", "Moon" };
        int row, slot = 0;
        int best_s = -1, best_len = 0, run_s = -1;
        int clear_pct = a->cloud_clear;

        for (row = 0; row < 5; row++) {
            double ry;

            if (row >= 1 && row <= 3 && !have_fc) continue;
            ry = y + (slot++) * (row_h + 2);

            cairo_set_font_size(cr, fs * 0.85);
            cairo_set_source_rgba(cr, 0.75, 0.75, 0.8, 0.9);
            cairo_move_to(cr, 0, ry + row_h * 0.8);
            cairo_show_text(cr, labels[row]);

            for (i = 0; i < STRIP_HOURS; i++) {
                time_t t = start + (time_t)i * 3600;
                time_t mid = t + 1800;
                double x = label_w + i * cell_w;
                const pic_astro_hour_t *h = forecast_at(a, t);

                cairo_rectangle(cr, x + 0.5, ry, cell_w - 1, row_h);
                if (row == 0) {
                    double alt = pic_astro_sun_alt(mid, lat, lon);
                    if (alt > ASTRO_SUN_H0)
                        cairo_set_source_rgba(cr, 0.55, 0.75, 0.95, 0.9);
                    else if (alt > ASTRO_TWILIGHT_H0) {
                        double f = (alt - ASTRO_TWILIGHT_H0) /
                                   (ASTRO_SUN_H0 - ASTRO_TWILIGHT_H0);
                        cairo_set_source_rgba(cr, 0.08 + 0.35 * f,
                                              0.1 + 0.4 * f, 0.3 + 0.5 * f, 0.9);
                    } else
                        cairo_set_source_rgba(cr, 0.04, 0.05, 0.18, 0.95);
                } else if (row == 4) {
                    double alt = pic_astro_moon_alt(mid, lat, lon);
                    if (alt > 0)
                        cairo_set_source_rgba(cr, 0.4 + 0.55 * illum,
                                              0.4 + 0.53 * illum,
                                              0.4 + 0.45 * illum, 0.9);
                    else
                        cairo_set_source_rgba(cr, 0.08, 0.08, 0.1, 0.9);
                } else if (!h) {
                    cairo_set_source_rgba(cr, 0.2, 0.2, 0.2, 0.6);
                } else {
                    set_rgb(cr, row == 1 ? h->cloud_rgb :
                                row == 2 ? h->transp_rgb : h->seeing_rgb, 0.95);
                }
                cairo_fill(cr);
            }
        }
        y += nrows * (row_h + 2) + fs * 0.3;

        /* ── Best window: dark + (clear if forecast available) ── */
        for (i = 0; i <= STRIP_HOURS; i++) {
            int ok = 0;
            if (i < STRIP_HOURS) {
                time_t mid = start + (time_t)i * 3600 + 1800;
                const pic_astro_hour_t *h = forecast_at(a, mid - 1800);
                double cloud = h ? h->cloud : -1;
                /* Cloud is a percentage; tolerate a 0-1 fraction too */
                if (cloud >= 0 && cloud <= 1.0) cloud *= 100.0;
                ok = pic_astro_sun_alt(mid, lat, lon) < ASTRO_TWILIGHT_H0 &&
                     (!have_fc || (cloud >= 0 && cloud <= clear_pct));
            }
            if (ok && run_s < 0) run_s = i;
            if (!ok && run_s >= 0) {
                if (i - run_s > best_len) { best_len = i - run_s; best_s = run_s; }
                run_s = -1;
            }
        }

        cairo_set_font_size(cr, fs);
        if (best_s >= 0) {
            hour_ampm(start + (time_t)best_s * 3600, b1, sizeof(b1));
            hour_ampm(start + (time_t)(best_s + best_len) * 3600, b2, sizeof(b2));
            snprintf(line, sizeof(line), "Best %s - %s  %s", b1, b2,
                     have_fc ? "clear & dark" : "dark");
            cairo_set_source_rgba(cr, 0.4, 0.95, 0.5, 0.95);
        } else {
            snprintf(line, sizeof(line), have_fc ? "No clear dark window"
                                                 : "No dark hours in view");
            cairo_set_source_rgba(cr, 0.95, 0.55, 0.4, 0.95);
        }
        cairo_move_to(cr, 0, y + fs);
        cairo_show_text(cr, line);
        y += line_h;

        /* Footer: source / status */
        cairo_set_font_size(cr, fs * 0.75);
        cairo_set_source_rgba(cr, 0.6, 0.6, 0.65, 0.85);
        if (have_fc && !a->status[0]) {
            hm(a->fetched, b1, sizeof(b1));
            snprintf(line, sizeof(line), "Astrospheric  updated %s", b1);
        } else {
            snprintf(line, sizeof(line), "%s", a->status);
        }
        cairo_move_to(cr, 0, y + fs * 0.9);
        cairo_show_text(cr, line);
    }
    pthread_mutex_unlock(&a->mutex);

    return total_h;
}
