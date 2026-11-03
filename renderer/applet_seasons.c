/*
 * applet_seasons.c - Seasons and Analemma panels
 *
 * Seasons: a ring showing where Earth is in its orbit (the sun's
 * ecliptic longitude: equinoxes and solstices at the quarter marks),
 * the next equinox/solstice with a countdown, today's daylight and how
 * much it changed since yesterday ("+1 m 42 s"), and the shortest or
 * longest day coming up.
 *
 * Analemma: the figure-8 the sun traces when seen at the same clock
 * time every day — declination (up/down) against the equation of time
 * (left/right) — with today's point, how far a sundial runs fast or
 * slow, and local solar noon at home.
 *
 * Sun position from the Astronomical Almanac's low-precision formulae
 * (about 0.01°, i.e. equinox/solstice times to ~15 minutes, equation
 * of time to a few seconds). Day lengths use the sunrise/sunset search
 * in astro.c. Everything is computed locally; no network.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "features_all.h"
#include "astro.h"
#include "config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define D2R (M_PI / 180.0)
#define R2D (180.0 / M_PI)

/* Sun: ecliptic longitude, declination (deg) and equation of time (min) */
static void sun_at(time_t t, double *lambda, double *decl, double *eot)
{
    double n = (double)t / 86400.0 + 2440587.5 - 2451545.0;
    double L = fmod(280.460 + 0.9856474 * n, 360.0);
    double g = fmod(357.528 + 0.9856003 * n, 360.0) * D2R;
    double lam = L + 1.915 * sin(g) + 0.020 * sin(2 * g);
    double eps = (23.439 - 0.0000004 * n) * D2R;
    double alpha = atan2(cos(eps) * sin(lam * D2R), cos(lam * D2R)) * R2D;
    double e;

    if (L < 0) L += 360.0;
    lam = fmod(lam + 360.0, 360.0);
    if (lambda) *lambda = lam;
    if (decl) *decl = asin(sin(eps) * sin(lam * D2R)) * R2D;
    e = fmod(L - alpha + 540.0, 360.0) - 180.0;   /* degrees, -180..180 */
    if (eot) *eot = e * 4.0;                       /* minutes            */
}

/* Next time the sun's ecliptic longitude reaches a multiple of 90° */
static time_t next_quarter(time_t now, int *which)
{
    double l0, l;
    int q0, q;
    time_t a = now, b;

    sun_at(now, &l0, NULL, NULL);
    q0 = (int)(l0 / 90.0);
    for (b = now + 86400; b < now + 100 * 86400; b += 86400) {
        sun_at(b, &l, NULL, NULL);
        q = (int)(l / 90.0);
        if (q != q0) break;
        a = b;
    }
    while (b - a > 60) {
        time_t m = a + (b - a) / 2;
        sun_at(m, &l, NULL, NULL);
        if ((int)(l / 90.0) == q0) a = m; else b = m;
    }
    *which = (q0 + 1) % 4;   /* 0 Mar equinox, 1 Jun solstice, 2 Sep, 3 Dec */
    return b;
}

/* Daylight (s) for the local calendar day containing t; 0 if none */
static long day_length(time_t t, double lat, double lon)
{
    struct tm lt;
    time_t mid, rise, set;
    localtime_r(&t, &lt);
    lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0; lt.tm_isdst = -1;
    mid = mktime(&lt);
    rise = pic_astro_next_crossing(0, mid, lat, lon, -0.833, 1, 24);
    if (!rise) return 0;
    set = pic_astro_next_crossing(0, rise, lat, lon, -0.833, 0, 24);
    return set ? (long)(set - rise) : 0;
}

static void fmt_hm(char *out, size_t n, long s)
{
    snprintf(out, n, "%ld h %02ld m", s / 3600, (s % 3600 + 30) / 60);
}

/* ── Seasons panel ───────────────────────────────────────────────── */

static struct {
    time_t at;                 /* recompute every 10 min */
    double lat, lon;
    long   today, yesterday;   /* daylight, s */
    time_t next;
    int    which;
} ss;

static const char *qname[4] = { "March equinox", "June solstice",
                                "September equinox", "December solstice" };

double pic_applet_render_seasons(cairo_t *cr, double width, time_t now, void *ud)
{
    double fs = width / 17.0, line_h = fs * 1.3, y, ring = fs * 3.1;
    double lam, cx, cy, tx, total;
    double lat = pic_config.qth_lat, lon = pic_config.qth_lon;
    char line[128], a[32], b[32];
    long diff, days;
    int i;
    struct tm lt;

    (void)ud;
    total = pic_applet_title(NULL, width, "") + fs * 0.4 + ring * 2 + fs * 0.6 + line_h * 1.2;
    if (!cr) return total;

    if (now - ss.at > 600 || lat != ss.lat || lon != ss.lon) {
        ss.today = day_length(now, lat, lon);
        ss.yesterday = day_length(now - 86400, lat, lon);
        ss.next = next_quarter(now, &ss.which);
        ss.at = now; ss.lat = lat; ss.lon = lon;
    }
    sun_at(now, &lam, NULL, NULL);

    y = pic_applet_title(cr, width, "Seasons") + fs * 0.4;
    cx = ring + fs * 0.2;
    cy = y + ring;

    /* Ring: right = March equinox, top = June solstice, left =
     * September, bottom = December; the current season's quarter is
     * filled in its colour, and a dot marks today. */
    {
        static const double col[4][3] = {
            { 0.45, 0.85, 0.45 },   /* spring */
            { 1.0, 0.82, 0.3 },     /* summer */
            { 1.0, 0.55, 0.25 },    /* autumn */
            { 0.45, 0.7, 1.0 },     /* winter */
        };
        int q = (int)(lam / 90.0);
        /* Southern hemisphere: the same quarter is the opposite season */
        int season = lat < 0 ? (q + 2) % 4 : q;
        double rr = ring * 0.82;
        cairo_set_line_width(cr, fs * 0.45);
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, rr, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.35, 0.38, 0.45, 0.6);
        cairo_stroke(cr);
        cairo_new_path(cr);
        cairo_arc_negative(cr, cx, cy, rr, -q * M_PI / 2, -lam * D2R);
        cairo_set_source_rgba(cr, col[season][0], col[season][1], col[season][2], 0.95);
        cairo_stroke(cr);
        for (i = 0; i < 4; i++) {
            double an = -i * M_PI / 2;
            cairo_new_path(cr);
            cairo_move_to(cr, cx + cos(an) * (rr - fs * 0.45), cy + sin(an) * (rr - fs * 0.45));
            cairo_line_to(cr, cx + cos(an) * (rr + fs * 0.45), cy + sin(an) * (rr + fs * 0.45));
            cairo_set_source_rgba(cr, 0.85, 0.87, 0.92, 0.9);
            cairo_set_line_width(cr, fs * 0.12);
            cairo_stroke(cr);
        }
        cairo_new_path(cr);
        cairo_arc(cr, cx + cos(-lam * D2R) * rr, cy + sin(-lam * D2R) * rr, fs * 0.42, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1, 1, 1, 1);
        cairo_fill(cr);
        /* Month initials at the quarter marks */
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs * 0.62);
        cairo_set_source_rgba(cr, 0.7, 0.72, 0.78, 0.95);
        {
            static const char *m[4] = { "Mar", "Jun", "Sep", "Dec" };
            for (i = 0; i < 4; i++) {
                cairo_text_extents_t e;
                double an = -i * M_PI / 2, r2 = rr - fs * 1.15;
                cairo_text_extents(cr, m[i], &e);
                cairo_move_to(cr, cx + cos(an) * r2 - e.x_advance / 2,
                              cy + sin(an) * r2 + e.height / 2);
                cairo_show_text(cr, m[i]);
            }
        }
    }

    /* Text to the right of the ring */
    tx = cx + ring + fs * 0.5;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    days = (long)((ss.next - now + 43200) / 86400);
    cairo_set_source_rgba(cr, 0.95, 0.95, 0.97, 0.95);
    snprintf(line, sizeof(line), days == 0 ? "Today" : days == 1 ? "Tomorrow" : "In %ld days", days);
    cairo_move_to(cr, tx, y + fs * 1.1);
    pic_show_text_fit(cr, line, width - tx);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs * 0.85);
    cairo_set_source_rgba(cr, 0.8, 0.82, 0.88, 0.95);
    cairo_move_to(cr, tx, y + fs * 1.1 + line_h * 0.9);
    pic_show_text_fit(cr, qname[ss.which], width - tx);
    localtime_r(&ss.next, &lt);
    strftime(a, sizeof(a), "%b %d", &lt);
    pic_fmt_hm(b, sizeof(b), ss.next);
    snprintf(line, sizeof(line), "%s, %s", a, b);
    cairo_move_to(cr, tx, y + fs * 1.1 + line_h * 1.75);
    pic_show_text_fit(cr, line, width - tx);
    {
        double ecl;
        sun_at(now, NULL, &ecl, NULL);
        snprintf(line, sizeof(line), "Sun %+.1f\xc2\xb0 declination", ecl);
        cairo_set_source_rgba(cr, 0.62, 0.65, 0.72, 0.9);
        cairo_move_to(cr, tx, y + fs * 1.1 + line_h * 2.6);
        pic_show_text_fit(cr, line, width - tx);
    }

    /* Daylight line under the ring */
    y = cy + ring + fs * 0.6;
    if (ss.today > 0) {
        diff = ss.today - ss.yesterday;
        fmt_hm(a, sizeof(a), ss.today);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        cairo_set_source_rgba(cr, 1.0, 0.85, 0.45, 0.95);
        snprintf(line, sizeof(line), "Daylight %s", a);
        cairo_move_to(cr, 0, y + fs);
        cairo_show_text(cr, line);
        {
            cairo_text_extents_t e;
            cairo_text_extents(cr, line, &e);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_NORMAL);
            snprintf(line, sizeof(line), "  %s%ld m %02ld s today",
                     diff >= 0 ? "+" : "\xe2\x88\x92", labs(diff) / 60, labs(diff) % 60);
            if (diff >= 0) cairo_set_source_rgba(cr, 0.5, 0.95, 0.55, 0.95);
            else           cairo_set_source_rgba(cr, 0.55, 0.75, 1.0, 0.95);
            cairo_move_to(cr, e.x_advance, y + fs);
            pic_show_text_fit(cr, line, width - e.x_advance);
        }
    } else {
        cairo_set_source_rgba(cr, 0.7, 0.72, 0.78, 0.9);
        cairo_move_to(cr, 0, y + fs);
        cairo_show_text(cr, lat > 0 ? "Polar night / midnight sun" : "Polar day / night");
    }
    return total;
}

/* ── Analemma panel ──────────────────────────────────────────────── */

double pic_applet_render_analemma(cairo_t *cr, double width, time_t now, void *ud)
{
    double fs = width / 17.0, line_h = fs * 1.3, y, ch = width * 0.62, cw;
    double x0, y0, eot, dec, total, lon = pic_config.qth_lon;
    char line[96], a[24];
    struct tm lt;
    int d, year;
    time_t jan1;

    (void)ud;
    total = pic_applet_title(NULL, width, "") + fs * 0.3 + ch + fs * 0.5 + line_h * 2.0;
    if (!cr) return total;

    y = pic_applet_title(cr, width, "Analemma") + fs * 0.3;
    sun_at(now, NULL, &dec, &eot);

    /* Chart: x = equation of time (-16 .. +18 min), y = declination.
     * The figure is narrow, so the chart sits on the left with the
     * text beside it. */
    cw = ch * 0.62;
    x0 = fs * 0.3;
    y0 = y;
#define AX(e) (x0 + ((e) + 16.0) / 34.0 * cw)
#define AY(dd) (y0 + (23.6 - (dd)) / 47.2 * ch)
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 0.4, 0.42, 0.5, 0.6);
    cairo_new_path(cr);
    cairo_move_to(cr, AX(0), y0); cairo_line_to(cr, AX(0), y0 + ch);   /* clock = sundial */
    cairo_move_to(cr, x0, AY(0)); cairo_line_to(cr, x0 + cw, AY(0));   /* equator */
    cairo_stroke(cr);

    localtime_r(&now, &lt);
    year = lt.tm_year;
    memset(&lt, 0, sizeof(lt));
    lt.tm_year = year; lt.tm_mon = 0; lt.tm_mday = 1; lt.tm_hour = 12; lt.tm_isdst = -1;
    jan1 = mktime(&lt);
    cairo_new_path(cr);
    for (d = 0; d <= 366; d++) {
        double e2, d2;
        sun_at(jan1 + (time_t)d * 86400, NULL, &d2, &e2);
        if (d == 0) cairo_move_to(cr, AX(e2), AY(d2));
        else cairo_line_to(cr, AX(e2), AY(d2));
    }
    cairo_set_source_rgba(cr, 1.0, 0.82, 0.4, 0.9);
    cairo_set_line_width(cr, fs * 0.18);
    cairo_stroke(cr);
    /* First of each month as a small dot */
    for (d = 0; d < 12; d++) {
        struct tm m = lt;
        double e2, d2;
        m.tm_mon = d; m.tm_mday = 1;
        sun_at(mktime(&m), NULL, &d2, &e2);
        cairo_new_path(cr);
        cairo_arc(cr, AX(e2), AY(d2), fs * 0.16, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.85, 0.87, 0.92, 0.8);
        cairo_fill(cr);
    }
    cairo_new_path(cr);
    cairo_arc(cr, AX(eot), AY(dec), fs * 0.42, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.95, 0.5, 0.2, 1);
    cairo_set_line_width(cr, fs * 0.15);
    cairo_stroke(cr);
#undef AX
#undef AY

    /* Text beside the chart */
    {
        double tx = x0 + cw + fs * 0.8, tw = width - tx;
        double m = fabs(eot);
        time_t noon;
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        cairo_set_source_rgba(cr, 0.95, 0.95, 0.97, 0.95);
        if (m < 0.5)
            snprintf(line, sizeof(line), "Sundial on time");
        else
            snprintf(line, sizeof(line), "Sundial %.0f min %s", m, eot > 0 ? "fast" : "slow");
        cairo_move_to(cr, tx, y0 + fs * 1.1);
        pic_show_text_fit(cr, line, tw);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs * 0.85);
        cairo_set_source_rgba(cr, 0.8, 0.82, 0.88, 0.95);
        snprintf(line, sizeof(line), "%.0f m %02.0f s vs. clock", floor(m),
                 fmod(m * 60.0, 60.0));
        cairo_move_to(cr, tx, y0 + fs * 1.1 + line_h * 0.9);
        pic_show_text_fit(cr, line, tw);

        /* Solar noon at home: 12:00 UT - lon*4 min - EoT */
        {
            struct tm u;
            time_t day;
            gmtime_r(&now, &u);
            u.tm_hour = 12; u.tm_min = 0; u.tm_sec = 0;
            day = timegm(&u);
            noon = day - (time_t)(lon * 240.0) - (time_t)(eot * 60.0);
            /* Keep it on today's local date */
            {
                struct tm ln, nn;
                localtime_r(&now, &ln);
                localtime_r(&noon, &nn);
                if (nn.tm_yday != ln.tm_yday) noon += (nn.tm_yday < ln.tm_yday ? 1 : -1) * 86400;
            }
        }
        pic_fmt_hm(a, sizeof(a), noon);
        snprintf(line, sizeof(line), "Solar noon %s", a);
        cairo_set_source_rgba(cr, 1.0, 0.85, 0.45, 0.95);
        cairo_move_to(cr, tx, y0 + fs * 1.1 + line_h * 2.0);
        pic_show_text_fit(cr, line, tw);
        snprintf(line, sizeof(line), "Sun %+.1f\xc2\xb0", dec);
        cairo_set_source_rgba(cr, 0.62, 0.65, 0.72, 0.9);
        cairo_move_to(cr, tx, y0 + fs * 1.1 + line_h * 2.9);
        pic_show_text_fit(cr, line, tw);
    }

    /* Footnote under the chart */
    y = y0 + ch + fs * 0.5;
    cairo_set_font_size(cr, fs * 0.72);
    cairo_set_source_rgba(cr, 0.6, 0.62, 0.7, 0.9);
    cairo_move_to(cr, 0, y + fs * 0.8);
    pic_show_text_fit(cr, "Sun at the same clock time all year; dot = today",
                      width);
    return total;
}

static pic_feature_t seasons_def = {
    .name = "seasons", .is_applet = 1, .applet = pic_applet_render_seasons,
    .label = "Seasons", .applet_w = 220, .side = APPLET_SIDE_RIGHT,
    .interval = 60,
};

static pic_feature_t analemma_def = {
    .name = "analemma", .is_applet = 1, .applet = pic_applet_render_analemma,
    .label = "Analemma", .applet_w = 220, .side = APPLET_SIDE_RIGHT,
    .interval = 300,
};

pic_feature_t *pic_feature_seasons(void) { return &seasons_def; }
pic_feature_t *pic_feature_analemma(void) { return &analemma_def; }
