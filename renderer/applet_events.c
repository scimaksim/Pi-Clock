/*
 * applet_events.c - "Coming up" strip: the next things worth stepping
 * outside for, in time order
 *
 * Gathers from the other features (nothing is fetched here):
 *   - visible ISS / Tiangong / Hubble passes
 *   - rocket launches visible from home
 *   - aurora possible from home (forecast block)
 *   - the next major meteor shower peak
 *   - moonrise / moonset
 *   - tonight's best clear, dark window (Astrospheric)
 *   - rain reaching home / easing (PC helper nowcast)
 *
 * Drawn as one row above the map key: "Coming up: Hubble pass 6:38 PM
 * SW->S 15 deg · Tiangong 6:48 PM · Moonrise 3:01 AM · Taurids Thu".
 * An event happening now is shown first in aqua.
 *
 * Layer "events" (legend only, no map content). Setting EVENTS_HOURS
 * (default 36) limits how far ahead it looks.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "layer.h"
#include "features_all.h"
#include "astro.h"
#include "config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EV 24

static int hours_ahead = 36;

static const char *compass8(double az)
{
    static const char *p[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return p[(int)((fmod(az + 360.0, 360.0) + 22.5) / 45.0) % 8];
}

static int by_start(const void *a, const void *b)
{
    const pic_event_t *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

/* Tests can inject events instead of asking the features */
int (*pic_events_test_source)(time_t now, pic_event_t *out, int max);

int pic_events_collect(time_t now, pic_event_t *out, int max)
{
    pic_event_t ev[MAX_EV];
    int n = 0, i, k;
    if (pic_events_test_source) return pic_events_test_source(now, out, max);
    time_t horizon = now + (time_t)hours_ahead * 3600;
    double lat = pic_config.qth_lat, lon = pic_config.qth_lon;

#define ADD(K, S, E, O, ...) do { if (n < MAX_EV && (S) < horizon) { \
        memset(&ev[n], 0, sizeof(ev[n])); ev[n].kind = (K); ev[n].start = (S); \
        ev[n].end = (E); ev[n].outdoors = (O); \
        snprintf(ev[n].text, sizeof(ev[n].text), __VA_ARGS__); n++; } } while (0)

    /* Satellite passes */
    {
        pic_pass_t p[12];
        int np = pic_passes_get(p, 12);
        for (i = 0; i < np; i++) {
            if (p[i].end < now) continue;
            ADD(PIC_EV_PASS, p[i].start, p[i].end, 1, "%.15s pass", p[i].sat);
            snprintf(ev[n - 1].detail, sizeof(ev[n - 1].detail), "%s\xe2\x86\x92%s %d\xc2\xb0",
                     compass8(p[i].az_start), compass8(p[i].az_end), (int)(p[i].max_el + 0.5));
        }
    }
    /* Launches visible from home */
    {
        pic_launch_alert_t L[6];
        int nl = pic_launches_visible(L, 6);
        for (i = 0; i < nl; i++) {
            if (L[i].net + L[i].vis.t_end < now) continue;
            ADD(PIC_EV_LAUNCH, L[i].net + L[i].vis.t_start, L[i].net + L[i].vis.t_end, 1,
                "%.30s launch", L[i].rocket);
            snprintf(ev[n - 1].detail, sizeof(ev[n - 1].detail), "look %s%s",
                     pic_launchvis_compass(L[i].vis.az_start),
                     L[i].precise && !strcmp(L[i].status, "Go") ? "" : " (NET)");
        }
    }
    /* Aurora */
    {
        pic_aurora_home_t a;
        pic_aurora_home(now, &a);
        if (a.have_data && a.now_level)
            ADD(PIC_EV_AURORA, now, now + 3600, 1, "Aurora %s now",
                a.now_level == 2 ? "possible" : "in photos");
        else if (a.have_data && a.next_t)
            ADD(PIC_EV_AURORA, a.next_t, a.next_t + 3 * 3600, 1, "Aurora %s",
                a.next_level == 2 ? "possible" : "in photos");
    }
    /* Meteor shower */
    {
        char name[24];
        int zhr = 0;
        time_t t = pic_planets_next_shower(now, name, sizeof(name), &zhr);
        if (t && t < now + 7 * 86400) {
            time_t hz = horizon;
            horizon = now + 7 * 86400;        /* showers are worth a week's notice */
            ADD(PIC_EV_SHOWER, t, t + 36000, 1, "%s peak", name);
            horizon = hz;
            snprintf(ev[n - 1].detail, sizeof(ev[n - 1].detail), "ZHR %d", zhr);
        }
    }
    /* Moon rise / set */
    {
        time_t r = pic_astro_next_crossing(1, now, lat, lon, ASTRO_MOON_H0, 1, hours_ahead);
        time_t s = pic_astro_next_crossing(1, now, lat, lon, ASTRO_MOON_H0, 0, hours_ahead);
        int wax;
        int ill = (int)(pic_astro_moon_illum(now, &wax) * 100 + 0.5);
        if (r) ADD(PIC_EV_MOONRISE, r, 0, 0, "Moonrise %d%%", ill);
        if (s) ADD(PIC_EV_MOONSET, s, 0, 0, "Moonset");
    }
    /* Clear dark window from the Astrospheric hours */
    if (pic_astro_shared) {
        pic_astro_t *a = pic_astro_shared;
        int clear_pct = pic_conf_int("ASTRO_CLEAR_PCT", 20), run_s = -1, best_s = -1, best_len = 0;
        pthread_mutex_lock(&a->mutex);
        for (i = 0; i <= a->nhours && i < 48; i++) {
            int ok = 0;
            if (i < a->nhours && a->hours[i].t + 3600 > now && a->hours[i].t < horizon)
                ok = pic_astro_sun_alt(a->hours[i].t + 1800, lat, lon) < ASTRO_TWILIGHT_H0 &&
                     a->hours[i].cloud >= 0 && a->hours[i].cloud <= clear_pct;
            if (ok && run_s < 0) run_s = i;
            if (!ok && run_s >= 0) {
                if (i - run_s > best_len) { best_len = i - run_s; best_s = run_s; }
                run_s = -1;
            }
        }
        if (best_s >= 0 && best_len >= 2) {
            time_t t0 = a->hours[best_s].t, t1 = t0 + (time_t)best_len * 3600;
            ADD(PIC_EV_DARK, t0, t1, 1, "Clear dark sky");
            snprintf(ev[n - 1].detail, sizeof(ev[n - 1].detail), "%d h", best_len);
        }
        pthread_mutex_unlock(&a->mutex);
    }
    /* Rain */
    {
        int raining = 0;
        time_t t = pic_pch_rain_change(now, &raining);
        if (t) ADD(PIC_EV_RAIN, t, 0, 0, raining ? "Rain easing" : "Rain at home");
    }
#undef ADD

    qsort(ev, (size_t)n, sizeof(ev[0]), by_start);
    for (i = 0, k = 0; i < n && k < max; i++) out[k++] = ev[i];
    return k;
}

static void events_strip(cairo_t *cr, int width, int height, time_t now, void *ud)
{
    pic_event_t ev[8];
    int n = pic_events_collect(now, ev, 8), i, shown = 0;
    double fs = height / 80.0, x, y, maxw = width * 0.62, pad = fs * 0.5;
    char parts[8][120];
    double w[8], total = 0;
    cairo_text_extents_t e;

    (void)ud;
    if (n == 0) return;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, "Coming up:", &e);
    total = e.x_advance + pad;
    for (i = 0; i < n; i++) {
        char when[24];
        int live = ev[i].start <= now && (ev[i].end >= now || (!ev[i].end && now - ev[i].start < 300));
        if (live) snprintf(when, sizeof(when), "now");
        else pic_fmt_when(when, sizeof(when), now, ev[i].start);
        snprintf(parts[i], sizeof(parts[i]), "%s %s%s%s", ev[i].text, when,
                 ev[i].detail[0] ? " " : "", ev[i].detail);
        cairo_text_extents(cr, parts[i], &e);
        w[i] = e.x_advance;
        if (total + w[i] + pad * 3 > maxw && shown >= 2) break;
        total += w[i] + pad * 3;
        shown++;
    }
    if (!shown) return;
    y = pic_legend_take(height, 1);
    x = (width - total) / 2.0;
    cairo_set_source_rgba(cr, 0, 0, 0, 0.65);
    cairo_rectangle(cr, x - pad, y - fs * 1.1, total + pad * 2, fs * 1.55);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.75, 0.78, 0.85, 0.95);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, "Coming up:");
    cairo_text_extents(cr, "Coming up:", &e);
    x += e.x_advance + pad;
    for (i = 0; i < shown; i++) {
        int live = ev[i].start <= now && (ev[i].end >= now || (!ev[i].end && now - ev[i].start < 300));
        if (i) {
            cairo_set_source_rgba(cr, 0.5, 0.52, 0.6, 0.9);
            cairo_move_to(cr, x + pad, y);
            cairo_show_text(cr, "\xc2\xb7");
            x += pad * 3;
        } else {
            x += pad;
        }
        if (live)                  cairo_set_source_rgba(cr, 0.30, 1.0, 0.92, 1.0);
        else if (ev[i].outdoors)   cairo_set_source_rgba(cr, 1.0, 0.92, 0.6, 0.97);
        else                       cairo_set_source_rgba(cr, 0.85, 0.87, 0.92, 0.95);
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, parts[i]);
        x += w[i];
    }
}

static void events_reload(pic_feature_t *f)
{
    (void)f;
    hours_ahead = pic_conf_int("EVENTS_HOURS", 36);
    if (hours_ahead < 6) hours_ahead = 6;
    if (hours_ahead > 168) hours_ahead = 168;
}

static pic_feature_t feature_def = {
    .name = "events", .layer = pic_layer_render_none, .legend = events_strip,
    .interval = 30, .extra_mb = 0, .reload = events_reload,
};

pic_feature_t *pic_feature_events(void)
{
    return &feature_def;
}
