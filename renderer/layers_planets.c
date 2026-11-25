/*
 * layers_planets.c - Naked-eye planets and upcoming sky events
 *
 * Map layer: the sub-planet point of each naked-eye planet (where it
 * is straight overhead right now), like the sun and moon layers.
 *
 * Applet "Planets & Events":
 *   - Each planet visible tonight from the QTH (≥10° up while the
 *     sun is below -6°): the visible window, peak altitude and
 *     direction.
 *   - The next major meteor shower peak and how bright the moon is.
 *   - The next eclipses; lunar eclipses are marked when the moon is
 *     above the horizon at the QTH at greatest eclipse.
 *
 * Positions use JPL's Keplerian elements for 1800-2050 ("Approximate
 * Positions of the Planets", Standish), good to well under a degree —
 * plenty for "where to look". Eclipse dates/times: NASA GSFC
 * eclipse catalogue. Shower peaks: IMO calendar (typical dates).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE  /* timegm */
#include "feature.h"
#include "labels.h"
#include "sky_icons.h"
#include "solar.h"
#include "astro.h"
#include "config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D2R (M_PI / 180.0)
#define R2D (180.0 / M_PI)

/* a, e, I, L, long.peri, long.node  and rates per Julian century */
static const double elements[9][12] = {
 { 0.38709927, 0.20563593, 7.00497902, 252.25032350, 77.45779628, 48.33076593,
   0.00000037, 0.00001906, -0.00594749, 149472.67411175, 0.16047689, -0.12534081 },
 { 0.72333566, 0.00677672, 3.39467605, 181.97909950, 131.60246718, 76.67984255,
   0.00000390, -0.00004107, -0.00078890, 58517.81538729, 0.00268329, -0.27769418 },
 { 1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0,
   0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0 },
 { 1.52371034, 0.09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891,
   0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343 },
 { 5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909,
  -0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106 },
 { 9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448,
  -0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794 },
 { 19.18916464, 0.04725744, 0.77263783, 313.23810451, 170.95427630, 74.01692503,
  -0.00196176, -0.00004397, -0.00242939, 428.48202785, 0.40805281, 0.04240589 },
 { 30.06992276, 0.00859048, 1.77004347, -55.12002969, 44.96476227, 131.78422574,
   0.00026291, 0.00005105, 0.00035372, 218.45945325, -0.32241464, -0.00508664 },
};

enum { MERCURY, VENUS, EARTH, MARS, JUPITER, SATURN };

static const struct {
    int idx;
    const char *name;
    double r, g, b;
} naked[] = {
    { MERCURY, "Mercury", 0.75, 0.72, 0.68 },
    { VENUS,   "Venus",   1.00, 0.97, 0.80 },
    { MARS,    "Mars",    1.00, 0.45, 0.30 },
    { JUPITER, "Jupiter", 0.95, 0.85, 0.70 },
    { SATURN,  "Saturn",  0.95, 0.88, 0.55 },
};
#define N_NAKED 5

static double jd_of(time_t t) { return t / 86400.0 + 2440587.5; }

static void helio(int p, double T, double v[3])
{
    const double *el = elements[p];
    double a = el[0] + el[6] * T, e = el[1] + el[7] * T;
    double I = (el[2] + el[8] * T) * D2R, L = el[3] + el[9] * T;
    double wb = el[4] + el[10] * T, Om = el[5] + el[11] * T;
    double w = (wb - Om) * D2R, M = fmod(L - wb, 360.0), E, xp, yp;
    int k;

    if (M > 180) M -= 360;
    if (M < -180) M += 360;
    M *= D2R;
    Om *= D2R;
    E = M + e * sin(M);
    for (k = 0; k < 8; k++)
        E -= (E - e * sin(E) - M) / (1 - e * cos(E));
    xp = a * (cos(E) - e);
    yp = a * sqrt(1 - e * e) * sin(E);
    v[0] = (cos(w) * cos(Om) - sin(w) * sin(Om) * cos(I)) * xp +
           (-sin(w) * cos(Om) - cos(w) * sin(Om) * cos(I)) * yp;
    v[1] = (cos(w) * sin(Om) + sin(w) * cos(Om) * cos(I)) * xp +
           (-sin(w) * sin(Om) + cos(w) * cos(Om) * cos(I)) * yp;
    v[2] = (sin(w) * sin(I)) * xp + (cos(w) * sin(I)) * yp;
}

/* Geocentric equatorial RA/Dec (degrees, J2000) of planet p at t. */
void pic_planet_radec(int p, time_t t, double *ra, double *dec)
{
    double T = (jd_of(t) - 2451545.0) / 36525.0, pv[3], ev[3], g[3];
    double eps = 23.43928 * D2R, x, y, z;

    helio(p, T, pv);
    helio(EARTH, T, ev);
    g[0] = pv[0] - ev[0];
    g[1] = pv[1] - ev[1];
    g[2] = pv[2] - ev[2];
    x = g[0];
    y = g[1] * cos(eps) - g[2] * sin(eps);
    z = g[1] * sin(eps) + g[2] * cos(eps);
    *ra = atan2(y, x) * R2D;
    if (*ra < 0) *ra += 360;
    *dec = atan2(z, sqrt(x * x + y * y)) * R2D;
}

/* Illuminated fraction (Venus, Mars) from the sun-planet-earth angle */
double pic_planet_phase(int p, time_t t)
{
    double T = (jd_of(t) - 2451545.0) / 36525.0, pv[3], ev[3], g[3], rp, rg, c;
    helio(p, T, pv);
    helio(EARTH, T, ev);
    g[0] = ev[0] - pv[0]; g[1] = ev[1] - pv[1]; g[2] = ev[2] - pv[2];     /* planet -> earth */
    rp = sqrt(pv[0] * pv[0] + pv[1] * pv[1] + pv[2] * pv[2]);
    rg = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (rp < 1e-9 || rg < 1e-9) return 1;
    /* angle at the planet between the sun (-pv) and the earth (g) */
    c = (-pv[0] * g[0] - pv[1] * g[1] - pv[2] * g[2]) / (rp * rg);
    return (1 + c) / 2;
}

/* Geocentric ecliptic longitude / latitude (deg) */
void pic_planet_ecliptic(int p, time_t t, double *lon, double *lat)
{
    double T = (jd_of(t) - 2451545.0) / 36525.0, pv[3], ev[3], x, y, z;
    helio(p, T, pv);
    helio(EARTH, T, ev);
    x = pv[0] - ev[0]; y = pv[1] - ev[1]; z = pv[2] - ev[2];
    *lon = atan2(y, x) * R2D;
    if (*lon < 0) *lon += 360;
    *lat = atan2(z, sqrt(x * x + y * y)) * R2D;
}

/* Everything the drawings need for planet p now */
static void planet_look(int p, time_t t, double *phase, double *tilt)
{
    double lo, la;
    *phase = 1;
    *tilt = 0;
    if (p == VENUS || p == MARS || p == MERCURY) *phase = pic_planet_phase(p, t);
    if (p == SATURN) {
        pic_planet_ecliptic(p, t, &lo, &la);
        *tilt = pic_saturn_ring_tilt(lo, la);
    }
}

static double gmst_deg(time_t t)
{
    double g = fmod(280.46061837 + 360.98564736629 * (jd_of(t) - 2451545.0), 360.0);
    return g < 0 ? g + 360 : g;
}

/* Sub-planet point (lat = dec, lon = RA - GMST) */
void pic_planet_subpoint(int p, time_t t, double *lat, double *lon)
{
    double ra, dec, l;
    pic_planet_radec(p, t, &ra, &dec);
    l = fmod(ra - gmst_deg(t) + 540.0, 360.0) - 180.0;
    *lat = dec;
    *lon = l;
}

double pic_planet_altaz(int p, time_t t, double lat, double lon, double *az)
{
    double ra, dec, ha, alt, A;
    pic_planet_radec(p, t, &ra, &dec);
    ha = (gmst_deg(t) + lon - ra) * D2R;
    lat *= D2R;
    dec *= D2R;
    alt = asin(sin(lat) * sin(dec) + cos(lat) * cos(dec) * cos(ha));
    A = atan2(-sin(ha) * cos(dec), cos(lat) * sin(dec) - sin(lat) * cos(dec) * cos(ha));
    if (az) {
        *az = A * R2D;
        if (*az < 0) *az += 360;
    }
    return alt * R2D;
}

/* ── Events ─────────────────────────────────────────────────── */

static const struct { int mon, day; const char *name; int zhr; } showers[] = {
    {  1,  3, "Quadrantids",          110 },
    {  4, 22, "Lyrids",                18 },
    {  5,  6, "Eta Aquariids",         50 },
    {  7, 30, "S. Delta Aquariids",    25 },
    {  8, 12, "Perseids",             100 },
    { 10,  8, "Draconids",             10 },
    { 10, 21, "Orionids",              20 },
    { 11, 12, "Taurids",                5 },
    { 11, 17, "Leonids",               15 },
    { 12, 14, "Geminids",             150 },
    { 12, 22, "Ursids",                10 },
};

/* Greatest eclipse, UTC. lunar: 1 = lunar, 0 = solar */
static const struct {
    int y, mo, d, h, mi, lunar;
    const char *kind;
} eclipses[] = {
    { 2027,  2,  6, 16,  0, 0, "Annular solar" },
    { 2027,  2, 20, 23, 13, 1, "Penumbral lunar" },
    { 2027,  7, 18, 16,  3, 1, "Penumbral lunar" },
    { 2027,  8,  2, 10,  7, 0, "Total solar" },
    { 2027,  8, 17,  7, 14, 1, "Penumbral lunar" },
    { 2028,  1, 12,  4, 13, 1, "Partial lunar" },
    { 2028,  1, 26, 15,  8, 0, "Annular solar" },
    { 2028,  7,  6, 18, 20, 1, "Partial lunar" },
    { 2028,  7, 22,  2, 55, 0, "Total solar" },
    { 2028, 12, 31, 16, 52, 1, "Total lunar" },
    { 2029,  1, 14, 17, 13, 0, "Partial solar" },
    { 2029,  6, 12,  4,  5, 0, "Partial solar" },
    { 2029,  6, 26,  3, 22, 1, "Total lunar" },
    { 2029,  7, 11, 15, 36, 0, "Partial solar" },
    { 2029, 12,  5, 15,  3, 0, "Partial solar" },
    { 2029, 12, 20, 22, 42, 1, "Total lunar" },
};

time_t pic_planets_next_shower(time_t now, char *name, size_t n, int *zhr);

static time_t utc(int y, int mo, int d, int h, int mi)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    return timegm(&tm);
}

/* ── Tonight's windows (computed at most every 10 minutes) ──── */

typedef struct {
    int    visible;
    time_t from, to, peak;
    double peak_alt, peak_az;
} pwin_t;

static pthread_mutex_t pl_mutex = PTHREAD_MUTEX_INITIALIZER;
static pwin_t  wins[N_NAKED];
static time_t  wins_at;
static double  wins_lat = 999, wins_lon = 999;
static int     pl_labels = 1;

static void compute_tonight(time_t now, double lat, double lon)
{
    time_t start = now, end, t;
    int i;

    /* Tonight = from now (or dusk) until dawn, at most 16 h */
    if (pic_astro_sun_alt(now, lat, lon) > -6) {
        time_t dusk = pic_astro_next_crossing(0, now, lat, lon, -6.0, 0, 30);
        if (dusk) start = dusk;
    }
    end = pic_astro_next_crossing(0, start, lat, lon, -6.0, 1, 30);
    if (!end || end - start > 16 * 3600) end = start + 12 * 3600;

    memset(wins, 0, sizeof(wins));
    for (i = 0; i < N_NAKED; i++) {
        pwin_t *w = &wins[i];
        for (t = start; t <= end; t += 600) {
            double az, alt = pic_planet_altaz(naked[i].idx, t, lat, lon, &az);
            if (alt >= 10 && pic_astro_sun_alt(t, lat, lon) < -6) {
                if (!w->visible) { w->visible = 1; w->from = t; }
                w->to = t;
                if (alt > w->peak_alt) {
                    w->peak_alt = alt;
                    w->peak_az = az;
                    w->peak = t;
                }
            }
        }
    }
}

static const char *compass8(double az)
{
    static const char *pts[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return pts[(int)((az + 22.5) / 45.0) % 8];
}

void pic_layer_render_planets(cairo_t *cr, int width, int height,
                              time_t now, void *ud)
{
    double fs = height / 90.0, r = height / 200.0;
    int i, labels, lbl = pic_labels_owner("planets");

    (void)ud;
    pic_labels_begin(lbl);
    pthread_mutex_lock(&pl_mutex);
    labels = pl_labels;
    pthread_mutex_unlock(&pl_mutex);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    for (i = 0; i < N_NAKED; i++) {
        double lat, lon, x, y;
        cairo_pattern_t *p;
        pic_planet_subpoint(naked[i].idx, now, &lat, &lon);
        x = pic_lon_to_x(lon, width);
        y = pic_lat_to_y(lat, height);
        cairo_new_path(cr);
        p = cairo_pattern_create_radial(x, y, 0, x, y, r * 3);
        cairo_pattern_add_color_stop_rgba(p, 0, naked[i].r, naked[i].g, naked[i].b, 0.5);
        cairo_pattern_add_color_stop_rgba(p, 1, naked[i].r, naked[i].g, naked[i].b, 0);
        cairo_set_source(cr, p);
        cairo_arc(cr, x, y, r * 3, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(p);
        {
            /* The planet as it looks, lit limb toward the sun's sub-point */
            pic_solar_position_t sun = pic_solar_position(now);
            double slon = -sun.gha * R2D, slat = sun.declination * R2D, sx, sy, ang = 0;
            double phase, tilt, pr = r * (naked[i].idx == JUPITER ? 2.6 : naked[i].idx == SATURN ? 2.0 : 1.8);
            if (slon < -180.0) slon += 360.0;
            sx = pic_lon_to_x(slon, width);
            sy = pic_lat_to_y(slat, height);
            if (fabs(sx - x) < width / 2.0) ang = atan2(sy - y, sx - x);
            planet_look(naked[i].idx, now, &phase, &tilt);
            pic_draw_planet(cr, naked[i].idx, x, y, pr, ang, phase, tilt, now);
        }
        if (labels) {
            /* beside the planet, wherever no other label already is */
            cairo_text_extents_t te;
            cairo_font_extents_t fe;
            double pad, ox, oy, pb[4] = { x - r * 3, y - r * 3, x + r * 3, y + r * 3 };
            cairo_font_extents(cr, &fe);
            cairo_text_extents(cr, naked[i].name, &te);
            pad = fe.ascent * 0.3;
            pic_labels_claim(lbl, pb);                       /* the planet itself */
            pic_labels_place(lbl, x, y, r * 4.6 - pad, te.x_advance + pad * 2,
                             fe.ascent + fe.descent + pad, width, height, &ox, &oy);
            pic_draw_text_pill(cr, ox + pad, oy + fe.ascent + pad * 0.5, naked[i].name,
                               naked[i].r, naked[i].g, naked[i].b, 0.5);
        }
    }
}

double pic_applet_render_planets(cairo_t *cr, double width, time_t now,
                                 void *ud)
{
    double fs = width / 17.0, line_h = fs * 1.3, y, lat = pic_config.qth_lat;
    double lon = pic_config.qth_lon;
    int i, nvis = 0, rows;
    char line[128], a[16], b[16];
    double total, tcol = fs * 5.1;      /* x of the time column */

    (void)ud;
    pthread_mutex_lock(&pl_mutex);
    if (now - wins_at > 600 || lat != wins_lat || lon != wins_lon) {
        compute_tonight(now, lat, lon);
        wins_at = now;
        wins_lat = lat;
        wins_lon = lon;
    }
    for (i = 0; i < N_NAKED; i++) nvis += wins[i].visible;
    rows = (nvis ? nvis : 1) + 1 + 2 + 1;   /* planets, header, events */
    total = pic_applet_title(NULL, width, "") + fs * 0.3 + rows * line_h + fs * 0.3;
    if (!cr) {
        pthread_mutex_unlock(&pl_mutex);
        return total;
    }

    y = pic_applet_title(cr, width, "Planets & Events") + fs * 0.3;
    cairo_set_font_size(cr, fs);
    /* Times start after the widest name shown ("Jupiter" in bold ran
     * into them at the old fixed column) */
    {
        cairo_text_extents_t ne;
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        for (i = 0; i < N_NAKED; i++) {
            if (!wins[i].visible) continue;
            cairo_text_extents(cr, naked[i].name, &ne);
            if (fs * 1.35 + ne.x_advance + fs * 0.45 > tcol) tcol = fs * 1.35 + ne.x_advance + fs * 0.45;
        }
    }
    for (i = 0; i < N_NAKED; i++) {
        const pwin_t *w = &wins[i];
        cairo_text_extents_t e;
        char right[48];
        if (!w->visible) continue;
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        {
            double phase, tilt, pr = fs * (naked[i].idx == SATURN ? 0.30 : naked[i].idx == JUPITER ? 0.46 : 0.40);
            planet_look(naked[i].idx, now, &phase, &tilt);
            pic_draw_planet(cr, naked[i].idx, fs * 0.6, y + fs * 0.62, pr,
                            M_PI, phase, tilt, now);
        }
        cairo_set_source_rgba(cr, naked[i].r, naked[i].g, naked[i].b, 0.95);
        cairo_move_to(cr, fs * 1.35, y + fs);
        cairo_show_text(cr, naked[i].name);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        if (w->from <= now) snprintf(a, sizeof(a), "now");
        else pic_fmt_hm(a, sizeof(a), w->from);
        pic_fmt_hm(b, sizeof(b), w->to);
        {   /* "12:53 AM–6:13 AM" -> "12:53–6:13 AM" */
            size_t la = strlen(a), lb = strlen(b);
            if (la > 3 && lb > 3 && !strcmp(a + la - 3, b + lb - 3) &&
                (!strcmp(b + lb - 3, " AM") || !strcmp(b + lb - 3, " PM")))
                a[la - 3] = '\0';
        }
        snprintf(line, sizeof(line), "%s\xe2\x80\x93%s", a, b);
        snprintf(right, sizeof(right), "%d\xc2\xb0 %s", (int)(w->peak_alt + 0.5),
                 compass8(w->peak_az));
        cairo_text_extents(cr, right, &e);
        pic_fit_text(cr, line, width - tcol - e.x_advance - fs * 0.3);
        cairo_set_source_rgba(cr, 0.85, 0.85, 0.9, 0.9);
        cairo_move_to(cr, tcol, y + fs);
        cairo_show_text(cr, line);
        cairo_set_source_rgba(cr, 0.65, 0.7, 0.78, 0.9);
        cairo_move_to(cr, width - e.x_advance, y + fs);
        cairo_show_text(cr, right);
        y += line_h;
    }
    if (!nvis) {
        cairo_set_source_rgba(cr, 0.6, 0.6, 0.65, 0.9);
        cairo_move_to(cr, 0, y + fs);
        cairo_show_text(cr, "No bright planets tonight");
        y += line_h;
    }
    pthread_mutex_unlock(&pl_mutex);

    /* Separator */
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.3);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, 0, y + line_h * 0.45);
    cairo_line_to(cr, width, y + line_h * 0.45);
    cairo_stroke(cr);
    y += line_h * 0.7;

    cairo_set_font_size(cr, fs * 0.92);
    /* Next meteor shower peak (within the next 12 months) */
    {
        struct tm lt;
        time_t best = 0;
        int bi = 0, yr, k;
        gmtime_r(&now, &lt);
        for (yr = lt.tm_year + 1900; yr <= lt.tm_year + 1901; yr++)
            for (k = 0; k < (int)(sizeof(showers) / sizeof(showers[0])); k++) {
                /* Peak night: evening of the date (local), ~6 UT next day */
                time_t t = utc(yr, showers[k].mon, showers[k].day, 6, 0) + 86400;
                if (t > now - 12 * 3600 && (!best || t < best)) { best = t; bi = k; }
            }
        if (best) {
            int wax;
            double ill = pic_astro_moon_illum(best, &wax);
            char d[24];
            pic_fmt_when(d, sizeof(d), now, best - 86400);
            if (best - now < 6 * 86400) {
                struct tm pl;
                time_t ev = best - 86400;
                localtime_r(&ev, &pl);
                strftime(d, sizeof(d), "%a", &pl);
            }
            snprintf(line, sizeof(line), "%s %s \xc2\xb7 ZHR %d \xc2\xb7 moon %d%%",
                     showers[bi].name, d, showers[bi].zhr, (int)(ill * 100 + 0.5));
            pic_fit_text(cr, line, width);
            cairo_set_source_rgba(cr, 0.8, 0.85, 1.0, 0.9);
            cairo_move_to(cr, 0, y + fs);
            cairo_show_text(cr, line);
            y += line_h;
        }
    }
    /* Next two eclipses */
    {
        int k, shown = 0;
        for (k = 0; k < (int)(sizeof(eclipses) / sizeof(eclipses[0])) && shown < 2; k++) {
            time_t t = utc(eclipses[k].y, eclipses[k].mo, eclipses[k].d,
                           eclipses[k].h, eclipses[k].mi);
            int vis = 0;
            char d[24];
            struct tm lt;
            if (t < now - 3 * 3600) continue;
            shown++;
            if (eclipses[k].lunar)
                vis = pic_astro_moon_alt(t, lat, lon) > 0;
            localtime_r(&t, &lt);
            strftime(d, sizeof(d), t - now < 300L * 86400 ? "%b %d" : "%b %d %Y", &lt);
            if (d[4] == '0') memmove(d + 4, d + 5, strlen(d + 5) + 1);
            /* Gold + "(visible)" when a lunar eclipse is up at home */
            snprintf(line, sizeof(line), "%s \xc2\xb7 %s eclipse%s", d,
                     eclipses[k].kind, vis ? " (visible)" : "");
            pic_fit_text(cr, line, width);
            if (vis) cairo_set_source_rgba(cr, 1.0, 0.85, 0.5, 0.95);
            else     cairo_set_source_rgba(cr, 0.7, 0.72, 0.78, 0.9);
            cairo_move_to(cr, 0, y + fs);
            cairo_show_text(cr, line);
            y += line_h;
        }
    }
    return total;
}

static void planets_reload(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&pl_mutex);
    pl_labels = pic_conf_int("PLANET_LABELS", 1);
    pthread_mutex_unlock(&pl_mutex);
}

static pic_feature_t feature_def = {
    .name = "planets", .layer = pic_layer_render_planets, .interval = 60,
    .applet = pic_applet_render_planets, .label = "Planets & Events",
    .applet_w = 220, .side = APPLET_SIDE_LEFT,
    .reload = planets_reload,
};

pic_feature_t *pic_feature_planets(void)
{
    return &feature_def;
}

/* Peak (evening, local) of the next major meteor shower; 0 if none */
time_t pic_planets_next_shower(time_t now, char *name, size_t n, int *zhr)
{
    struct tm lt;
    time_t best = 0;
    int bi = 0, yr, k;
    gmtime_r(&now, &lt);
    for (yr = lt.tm_year + 1900; yr <= lt.tm_year + 1901; yr++)
        for (k = 0; k < (int)(sizeof(showers) / sizeof(showers[0])); k++) {
            time_t t = utc(yr, showers[k].mon, showers[k].day, 6, 0) + 86400;
            if (t > now - 12 * 3600 && (!best || t < best)) { best = t; bi = k; }
        }
    if (!best) return 0;
    snprintf(name, n, "%s", showers[bi].name);
    if (zhr) *zhr = showers[bi].zhr;
    return best - 86400;
}
