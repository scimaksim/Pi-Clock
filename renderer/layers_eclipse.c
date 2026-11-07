/*
 * layers_eclipse.c - Solar and lunar eclipses, live
 *
 * Solar eclipse under way: the Moon's shadow travelling across the map.
 * ECLIPSE_STYLE 0 (default) draws it as astronauts see it: daylight
 * barely dimmed in the outer penumbra, deepening into a dark smudge
 * around the black umbra. ECLIPSE_STYLE 1 is a chart: shading by how
 * much of the Sun is covered with 25/50/75/90% contours, the penumbra
 * edge, the umbra outlined and the whole central path as a band. Both
 * label the umbra (TOTALITY/ANNULARITY and its duration) and, when the
 * eclipse reaches home, show the Sun as seen from home.
 *
 * Lunar eclipse under way: the part of the Earth where the Moon is up
 * (where the eclipse can be seen) tinted, the eclipsed Moon over the
 * sub-lunar point, and an inset of the Moon passing through Earth's
 * penumbra and umbra with the phase and contact times.
 *
 * Eclipses within 12 h get a preview (path band and countdown).
 * ECLIPSE_REPLAY (renderer conf): 0 off (default), 1 the next eclipse,
 * 2 the last one, replayed at 60x in a loop so it can be seen any time.
 * Between eclipses the layer is idle and holds no surface.
 *
 * Geometry: eclipse.c (Besselian elements from JPL DE421).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "eclipse.h"
#include "config.h"
#include "feature.h"
#include "layer.h"
#include "sky_icons.h"
#include <cairo/cairo.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D2R (M_PI / 180.0)
#define REPLAY_SPEED 60.0
#define REPLAY_HOLD 8.0          /* real seconds at the end of each loop */
#define PREVIEW_S (12 * 3600)

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char legend_txt[2][200];  /* written by render, read by legend */
static double legend_rgb[3] = { 1.0, 0.8, 0.5 };

/* ---------------------------------------------------------------- */
/* What to show                                                      */

typedef struct {
    const pic_ecl_solar_t *s;
    const pic_ecl_lunar_t *l;
    double t;                    /* time to draw (UT seconds)          */
    int replay;                  /* 1 next, 2 last                     */
    int preview;                 /* not started yet: path + countdown  */
} ecl_show_t;

static int replay_mode(void)
{
    static int mode;
    static time_t read_at;
    time_t now = time(NULL);
    pthread_mutex_lock(&mu);
    if (!read_at || now - read_at >= 15) {
        read_at = now;
        pthread_mutex_unlock(&mu);
        mode = pic_conf_int("ECLIPSE_REPLAY", 0);
        return mode;
    }
    pthread_mutex_unlock(&mu);
    return mode;
}

static void span_of(const ecl_show_t *w, double *a, double *b)
{
    if (w->s) { *a = (double)w->s->p1; *b = (double)w->s->p4; }
    else      { *a = (double)w->l->c[0]; *b = (double)w->l->c[1]; }
}

/* Decide what to draw at real time now; 0 = nothing */
static int what(time_t now, ecl_show_t *w)
{
    int mode = replay_mode();
    memset(w, 0, sizeof(*w));
    w->t = (double)now;
    if (mode == 1 || mode == 2) {
        const pic_ecl_solar_t *s = mode == 1 ? pic_ecl_solar_next(now) : pic_ecl_solar_prev(now);
        const pic_ecl_lunar_t *l = mode == 1 ? pic_ecl_lunar_next(now) : pic_ecl_lunar_prev(now);
        double a, b, loop;
        if (s && l) {   /* the nearer one in the chosen direction */
            if (mode == 1) { if (l->c[0] < s->p1) s = NULL; else l = NULL; }
            else           { if (l->c[1] > s->p4) s = NULL; else l = NULL; }
        }
        if (!s && !l) return 0;
        w->s = s; w->l = l; w->replay = mode;
        span_of(w, &a, &b);
        loop = (b - a) / REPLAY_SPEED + REPLAY_HOLD;
        w->t = a + fmin(fmod((double)now, loop) * REPLAY_SPEED, b - a);
        return 1;
    }
    w->s = pic_ecl_solar_at(now);
    if (!w->s) w->l = pic_ecl_lunar_at(now);
    if (w->s || w->l) return 1;
    {   /* starting soon */
        const pic_ecl_solar_t *s = pic_ecl_solar_next(now);
        const pic_ecl_lunar_t *l = pic_ecl_lunar_next(now);
        if (s && s->p1 > now && s->p1 - now <= PREVIEW_S) w->s = s;
        else if (l && l->c[0] > now && l->c[0] - now <= PREVIEW_S) w->l = l;
        w->preview = w->s || w->l;
        return w->preview;
    }
}

static int eclipse_idle(time_t now, void *ud)
{
    ecl_show_t w;
    (void)ud;
    if (!what(now, &w)) {
        pthread_mutex_lock(&mu);
        legend_txt[0][0] = legend_txt[1][0] = 0;
        pthread_mutex_unlock(&mu);
        return 1;
    }
    return 0;
}

static const char *solar_name(char type)
{
    switch (type) {
    case 'T': return "Total solar eclipse";
    case 'A': return "Annular solar eclipse";
    case 'H': return "Hybrid solar eclipse";
    default:  return "Partial solar eclipse";
    }
}

static const char *lunar_name(int kind)
{
    return kind == 2 ? "Total lunar eclipse" : kind == 1 ? "Partial lunar eclipse"
                                                         : "Penumbral lunar eclipse";
}

static void set_legend(int i, const char *txt)
{
    pthread_mutex_lock(&mu);
    snprintf(legend_txt[i], sizeof(legend_txt[i]), "%s", txt);
    pthread_mutex_unlock(&mu);
}

/* Local clock time of a UT instant, "3:12 PM" */
static void hm(char *out, int n, double t)
{
    pic_fmt_hm(out, n, (time_t)(t + 0.5));
}

/* ---------------------------------------------------------------- */
/* Contours (marching squares) on a grid of values                   */

typedef struct { double x0, y0, x1, y1; } seg_t;

static void contour(cairo_t *cr, const float *g, int gw, int gh, double cell,
                    double ox, double oy, double level, int night, double *lx, double *ly)
{
    int i, j;
    double best = 1e30;
    *lx = *ly = -1;
    for (j = 0; j + 1 < gh; j++)
        for (i = 0; i + 1 < gw; i++) {
            double v[4], px[4], py[4], ex[4], ey[4];
            int k, c = 0, n = 0;
            v[0] = g[j * gw + i];       px[0] = i;     py[0] = j;
            v[1] = g[j * gw + i + 1];   px[1] = i + 1; py[1] = j;
            v[2] = g[(j + 1) * gw + i + 1]; px[2] = i + 1; py[2] = j + 1;
            v[3] = g[(j + 1) * gw + i]; px[3] = i;     py[3] = j + 1;
            if (night && (v[0] < 0 || v[1] < 0 || v[2] < 0 || v[3] < 0))
                continue;   /* lines stop at sunset/sunrise */
            for (k = 0; k < 4; k++) if (v[k] >= level) c |= 1 << k;
            if (c == 0 || c == 15) continue;
            for (k = 0; k < 4; k++) {     /* crossing points on the 4 edges */
                int a = k, b = (k + 1) & 3;
                if ((v[a] >= level) != (v[b] >= level)) {
                    double f = (level - v[a]) / (v[b] - v[a]);
                    ex[n] = ox + (px[a] + f * (px[b] - px[a])) * cell;
                    ey[n] = oy + (py[a] + f * (py[b] - py[a])) * cell;
                    n++;
                }
            }
            for (k = 0; k + 1 < n; k += 2) {
                cairo_move_to(cr, ex[k], ey[k]);
                cairo_line_to(cr, ex[k + 1], ey[k + 1]);
                /* label spot: the topmost crossing */
                if (ey[k] < best) { best = ey[k]; *lx = ex[k]; *ly = ey[k]; }
            }
        }
}

/* ---------------------------------------------------------------- */
/* Solar                                                             */

/* Grid of obscuration over the screen (-1 where the Sun is down);
 * returns a malloc'd array */
static float *solar_grid(const pic_ecl_solar_t *e, double t, int width, int height,
                         double ox, double oy, double rw, double rh,
                         double cell, int *pgw, int *pgh)
{
    pic_ecl_state_t s;
    int gw = (int)(rw / cell) + 2, gh = (int)(rh / cell) + 2, i, j;
    float *g = malloc(sizeof(float) * (size_t)gw * (size_t)gh);
    double *sh = malloc(sizeof(double) * (size_t)gw * 2), *ch = sh + gw;
    if (!g || !sh) { free(g); free(sh); return NULL; }
    pic_ecl_solar_prep(e, t, &s);
    for (i = 0; i < gw; i++) {
        double H = (s.mu + pic_x_to_lon(ox + i * cell, width)) * D2R;
        sh[i] = sin(H);
        ch[i] = cos(H);
    }
    for (j = 0; j < gh; j++) {
        double lat = pic_y_to_lat(oy + j * cell, height), rs, rc;
        if (lat > 90) lat = 90;
        if (lat < -90) lat = -90;
        pic_ecl_geocentric(lat, &rs, &rc);
        for (i = 0; i < gw; i++) {
            double xi = rc * sh[i];
            double eta = rs * s.cd - rc * ch[i] * s.sd;
            double zeta = rs * s.sd + rc * ch[i] * s.cd;
            double L1 = s.l1 - zeta * s.tf1, L2 = s.l2 - zeta * s.tf2;
            double u = s.x - xi, v = s.y - eta, m = sqrt(u * u + v * v);
            float o = 0;
            if (m < L1)
                o = (float)pic_ecl_overlap((L1 - L2) / (L1 + L2), 2.0 * m / (L1 + L2));
            g[j * gw + i] = zeta > 0 ? o : -1.0f;   /* -1: the Sun is down */
        }
    }
    free(sh);
    *pgw = gw;
    *pgh = gh;
    return g;
}

/* Umbra/antumbra outline around the central point: lat/lon pairs */
#define RAYS 40
static int umbra_outline(const pic_ecl_solar_t *e, double t, double clat, double clon,
                         double *plat, double *plon)
{
    pic_ecl_state_t s;
    double rs, rc;
    int k, it;
    pic_ecl_solar_prep(e, t, &s);
    pic_ecl_geocentric(clat, &rs, &rc);
    {
        int c;
        pic_ecl_obscuration_fast(&s, rs, rc, clon, &c);
        if (!c) return 0;
    }
    for (k = 0; k < RAYS; k++) {
        double a = 2 * M_PI * k / RAYS, lo = 0, hi = 8.0, ca = cos(a), sa = sin(a);
        double cl = cos(clat * D2R);
        if (cl < 0.05) cl = 0.05;
        for (it = 0; it < 16; it++) {
            double r = (lo + hi) / 2, la = clat + r * ca, lo_ = clon + r * sa / cl;
            int c;
            if (la > 89.9) la = 89.9;
            if (la < -89.9) la = -89.9;
            pic_ecl_geocentric(la, &rs, &rc);
            pic_ecl_obscuration_fast(&s, rs, rc, lo_, &c);
            if (c) lo = r; else hi = r;
        }
        plat[k] = clat + lo * ca;
        plon[k] = clon + lo * sa / cl;
    }
    return RAYS;
}

/* Path cache: outlines along the whole central path */
#define PATH_STEP 60
typedef struct {
    const pic_ecl_solar_t *e;
    int n;
    double (*lat)[RAYS], (*lon)[RAYS];
    double *clat, *clon;
} path_t;
static path_t path;

static void path_build(const pic_ecl_solar_t *e)
{
    int n, k;
    long long t;
    if (path.e == e) return;
    free(path.lat); free(path.lon); free(path.clat); free(path.clon);
    memset(&path, 0, sizeof(path));
    path.e = e;
    if (!e->central) return;
    n = (int)((e->c2 - e->c1) / PATH_STEP) + 2;
    path.lat = malloc(sizeof(*path.lat) * (size_t)n);
    path.lon = malloc(sizeof(*path.lon) * (size_t)n);
    path.clat = malloc(sizeof(double) * (size_t)n);
    path.clon = malloc(sizeof(double) * (size_t)n);
    if (!path.lat || !path.lon || !path.clat || !path.clon) return;
    for (k = 0, t = e->c1; t <= e->c2 && k < n; t += PATH_STEP) {
        double la, lo;
        if (!pic_ecl_solar_centre(e, (double)t, &la, &lo)) continue;
        path.clat[k] = la;
        path.clon[k] = lo;
        if (!umbra_outline(e, (double)t, la, lo, path.lat[k], path.lon[k])) {
            int r;   /* a graze: keep the centre point only */
            for (r = 0; r < RAYS; r++) { path.lat[k][r] = la; path.lon[k][r] = lo; }
        }
        k++;
    }
    path.n = k;
}

/* Polygon in lat/lon, unwrapped around the first point's x */
static void poly_path(cairo_t *cr, const double *lat, const double *lon, int n,
                      int width, int height)
{
    double x0 = pic_lon_to_x(lon[0], width);
    int k;
    for (k = 0; k < n; k++) {
        double x = pic_lon_to_x(lon[k], width), y = pic_lat_to_y(lat[k], height);
        while (x - x0 > width / 2.0) x -= width;
        while (x0 - x > width / 2.0) x += width;
        if (k == 0) cairo_move_to(cr, x, y);
        else cairo_line_to(cr, x, y);
    }
    cairo_close_path(cr);
}

static void draw_path(cairo_t *cr, int width, int height)
{
    int k;
    if (path.n < 2) return;
    cairo_push_group(cr);
    cairo_set_source_rgb(cr, 0.35, 0.05, 0.08);
    for (k = 0; k < path.n; k++) {
        poly_path(cr, path.lat[k], path.lon[k], RAYS, width, height);
        cairo_fill(cr);
    }
    /* join neighbours so a fast shadow leaves no gaps */
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (k = 1; k < path.n; k++) {
        double x0 = pic_lon_to_x(path.clon[k - 1], width), x1 = pic_lon_to_x(path.clon[k], width);
        if (fabs(x1 - x0) > width / 2.0) continue;
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, x0, pic_lat_to_y(path.clat[k - 1], height));
        cairo_line_to(cr, x1, pic_lat_to_y(path.clat[k], height));
        cairo_stroke(cr);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.55);

    /* centre line */
    cairo_set_source_rgba(cr, 1.0, 0.55, 0.35, 0.7);
    cairo_set_line_width(cr, height / 900.0 + 0.6);
    for (k = 1; k < path.n; k++) {
        double x0 = pic_lon_to_x(path.clon[k - 1], width), x1 = pic_lon_to_x(path.clon[k], width);
        if (fabs(x1 - x0) > width / 2.0) continue;
        cairo_move_to(cr, x0, pic_lat_to_y(path.clat[k - 1], height));
        cairo_line_to(cr, x1, pic_lat_to_y(path.clat[k], height));
    }
    cairo_stroke(cr);
}

/* Seconds the centre point (lat, lon) spends in the umbra around t */
static int central_duration(const pic_ecl_solar_t *e, double t, double lat, double lon)
{
    pic_ecl_local_t o;
    double a = t, b = t;
    int n = 0;
    pic_ecl_solar_local(e, t, lat, lon, &o);
    if (!o.central) return 0;
    while (n++ < 900) { pic_ecl_solar_local(e, a - 1, lat, lon, &o); if (!o.central) break; a -= 1; }
    n = 0;
    while (n++ < 900) { pic_ecl_solar_local(e, b + 1, lat, lon, &o); if (!o.central) break; b += 1; }
    return (int)(b - a + 1);
}

/* The Sun as seen from home, Moon across it */
static void sun_inset(cairo_t *cr, double cx, double cy, double R, const pic_ecl_local_t *o,
                      const char *caption)
{
    cairo_pattern_t *p;
    double mx = cx - o->dx * R, my = cy - o->dy * R;   /* east = left, north = up */
    cairo_text_extents_t te;
    double fs = R * 0.55;

    cairo_save(cr);
    cairo_new_path(cr);
    cairo_rectangle(cr, cx - R * 1.9, cy - R * 1.65, R * 3.8, R * 4.05);
    cairo_set_source_rgba(cr, 0.02, 0.03, 0.06, 0.82);
    cairo_fill(cr);
    /* corona-ish glow when nearly total */
    if (o->obscuration > 0.97) {
        p = cairo_pattern_create_radial(cx, cy, R * 0.9, cx, cy, R * 1.6);
        cairo_pattern_add_color_stop_rgba(p, 0, 0.9, 0.92, 1.0, 0.6);
        cairo_pattern_add_color_stop_rgba(p, 1, 0.9, 0.92, 1.0, 0.0);
        cairo_set_source(cr, p);
        cairo_arc(cr, cx, cy, R * 1.6, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(p);
    }
    p = cairo_pattern_create_radial(cx - R * 0.2, cy - R * 0.2, R * 0.1, cx, cy, R);
    cairo_pattern_add_color_stop_rgb(p, 0, 1.0, 0.97, 0.75);
    cairo_pattern_add_color_stop_rgb(p, 1, 1.0, 0.72, 0.2);
    cairo_set_source(cr, p);
    cairo_arc(cr, cx, cy, R, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    /* the Moon: black where it covers the Sun, a faint outline elsewhere */
    cairo_save(cr);
    cairo_arc(cr, cx, cy, R, 0, 2 * M_PI);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 0.03, 0.03, 0.05);
    cairo_arc(cr, mx, my, o->moon_r * R, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_set_source_rgba(cr, 0.6, 0.65, 0.75, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_arc(cr, mx, my, o->moon_r * R, 0, 2 * M_PI);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, caption, &te);
    cairo_set_source_rgba(cr, 1.0, 0.85, 0.55, 1.0);
    cairo_move_to(cr, cx - te.x_advance / 2, cy + R * 1.6 + fs * 0.35);
    cairo_show_text(cr, caption);
    cairo_set_font_size(cr, fs * 0.8);
    cairo_text_extents(cr, "From home", &te);
    cairo_set_source_rgba(cr, 0.75, 0.78, 0.85, 1.0);
    cairo_move_to(cr, cx - te.x_advance / 2, cy - R * 1.2);
    cairo_show_text(cr, "From home");
    cairo_restore(cr);
}

/* ECLIPSE_STYLE: 0 realistic (the shadow as seen from orbit), 1 chart */
static int eclipse_style(void)
{
    static int style;
    static time_t read_at;
    time_t now = time(NULL);
    if (!read_at || now - read_at >= 15) {
        read_at = now;
        style = pic_conf_int("ECLIPSE_STYLE", 0);
    }
    return style;
}

/*
 * The shadow as astronauts see it: daylight dimmed by exactly the share
 * of the Sun that is covered, a smooth gradient deepening to the black
 * umbra, no lines. A coarse pass finds the shadow's footprint; a fine
 * pass (about 3 px cells, coarser if the footprint is huge) shades it.
 */
static void draw_realistic(cairo_t *cr, const pic_ecl_solar_t *e, double t,
                           int width, int height)
{
    double cc = height / 90.0, cell, x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9, area;
    int gw, gh, i, j;
    float *g = solar_grid(e, t, width, height, 0, 0, width, height, cc, &gw, &gh);
    cairo_surface_t *img;
    unsigned char *px;
    int stride;
    cairo_pattern_t *pat;
    cairo_matrix_t m;

    if (!g) return;
    for (j = 0; j < gh; j++)
        for (i = 0; i < gw; i++)
            if (g[j * gw + i] > 0) {
                if (i * cc < x0) x0 = i * cc;
                if (i * cc > x1) x1 = i * cc;
                if (j * cc < y0) y0 = j * cc;
                if (j * cc > y1) y1 = j * cc;
            }
    free(g);
    if (x1 < x0) return;
    x0 = fmax(0, x0 - 2 * cc); y0 = fmax(0, y0 - 2 * cc);
    x1 = fmin(width, x1 + 2 * cc); y1 = fmin(height, y1 + 2 * cc);
    area = (x1 - x0) * (y1 - y0);
    cell = fmax(height / 360.0, sqrt(area / 60000.0));

    g = solar_grid(e, t, width, height, x0, y0, x1 - x0, y1 - y0, cell, &gw, &gh);
    if (!g) return;
    img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gw, gh);
    px = cairo_image_surface_get_data(img);
    stride = cairo_image_surface_get_stride(img);
    cairo_surface_flush(img);
    for (j = 0; j < gh; j++) {
        unsigned int *row = (unsigned int *)(void *)(px + j * stride);
        for (i = 0; i < gw; i++) {
            double o = g[j * gw + i] > 0 ? g[j * gw + i] : 0;
            /* As seen from orbit: the outer penumbra barely dims the
             * ground (the eye hardly notices half the Sun gone), the
             * last 10-20% deepens fast into the umbra's dark smudge */
            double a = 0.22 * o + 0.75 * o * o * o * o;
            if (a > 0.96) a = 0.96;
            unsigned int al = (unsigned int)(255.0 * a + 0.5);
            row[i] = al << 24 | (unsigned int)(al * 0.01) << 16 |
                     (unsigned int)(al * 0.015) << 8 | (unsigned int)(al * 0.04);
        }
    }
    cairo_surface_mark_dirty(img);
    pat = cairo_pattern_create_for_surface(img);
    cairo_pattern_set_filter(pat, CAIRO_FILTER_BILINEAR);
    cairo_matrix_init_scale(&m, 1.0 / cell, 1.0 / cell);
    cairo_matrix_translate(&m, -x0, -y0);
    cairo_pattern_set_matrix(pat, &m);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);
    cairo_surface_destroy(img);
    free(g);
}

/* Home circumstances for an eclipse, cached */
typedef struct {
    const pic_ecl_solar_t *e;
    double max_o, t_max, t_start, t_end;
    int central, dur;
} home_t;
static home_t home;

static void home_build(const pic_ecl_solar_t *e)
{
    pic_ecl_local_t o;
    double t, lat = pic_config.qth_lat, lon = pic_config.qth_lon;
    if (home.e == e) return;
    memset(&home, 0, sizeof(home));
    home.e = e;
    for (t = (double)e->p1; t <= (double)e->p4; t += 20) {
        pic_ecl_solar_local(e, t, lat, lon, &o);
        if (o.obscuration > 0) {
            if (!home.t_start) home.t_start = t;
            home.t_end = t;
            if (o.obscuration > home.max_o) { home.max_o = o.obscuration; home.t_max = t; }
            if (o.central) home.central = o.central;
        }
    }
    if (home.central) home.dur = central_duration(e, home.t_max, lat, lon);
}

static void render_solar(cairo_t *cr, int width, int height, const ecl_show_t *w)
{
    const pic_ecl_solar_t *e = w->s;
    double cell = height / 150.0, t = w->t, lx, ly, fs = height / 72.0;
    int gw, gh, i, j;
    float *g;
    char txt[200], a[16], b[16], c[16];
    static const double levels[] = { 0.25, 0.5, 0.75, 0.9 };
    pic_ecl_local_t ho;

    path_build(e);
    home_build(e);

    if (w->preview) {
        double left = (double)e->p1 - (double)time(NULL);
        draw_path(cr, width, height);
        hm(a, sizeof(a), (double)e->tmax);
        snprintf(txt, sizeof(txt), "%s begins in %dh %02dm \xc2\xb7 greatest at %s",
                 solar_name(e->type), (int)(left / 3600), (int)fmod(left / 60, 60), a);
        set_legend(0, txt);
        set_legend(1, "");
        return;
    }

    /* the path (chart style), then the shadow over it */
    if (eclipse_style() != 0) draw_path(cr, width, height);

    if (eclipse_style() == 0) {
        draw_realistic(cr, e, t, width, height);
        g = NULL;
    } else
        g = solar_grid(e, t, width, height, 0, 0, width, height, cell, &gw, &gh);
    if (g) {
        cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gw, gh);
        unsigned char *px = cairo_image_surface_get_data(img);
        int stride = cairo_image_surface_get_stride(img);
        cairo_pattern_t *pat;
        cairo_matrix_t m;
        cairo_surface_flush(img);
        for (j = 0; j < gh; j++) {
            unsigned int *row = (unsigned int *)(void *)(px + j * stride);
            for (i = 0; i < gw; i++) {
                double o = g[j * gw + i] > 0 ? g[j * gw + i] : 0;
                unsigned int al = (unsigned int)(255.0 * 0.8 * pow(o, 0.75) + 0.5);
                row[i] = al << 24 | (unsigned int)(al * 0.02) << 16 |
                         (unsigned int)(al * 0.02) << 8 | (unsigned int)(al * 0.08);
            }
        }
        cairo_surface_mark_dirty(img);
        pat = cairo_pattern_create_for_surface(img);
        cairo_pattern_set_filter(pat, CAIRO_FILTER_BILINEAR);
        cairo_matrix_init_scale(&m, 1.0 / cell, 1.0 / cell);
        cairo_pattern_set_matrix(pat, &m);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);
        cairo_surface_destroy(img);

        /* penumbra edge */
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_new_path(cr);
        contour(cr, g, gw, gh, cell, 0, 0, 0.002, 1, &lx, &ly);
        cairo_set_source_rgba(cr, 1.0, 0.7, 0.3, 0.75);
        cairo_set_line_width(cr, fs * 0.13);
        cairo_stroke(cr);
        /* obscuration contours with their labels */
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs * 0.8);
        for (i = 0; i < 4; i++) {
            cairo_new_path(cr);
            contour(cr, g, gw, gh, cell, 0, 0, levels[i], 1, &lx, &ly);
            cairo_set_source_rgba(cr, 1.0, 0.85, 0.6, 0.55);
            cairo_set_line_width(cr, fs * 0.08);
            cairo_stroke(cr);
            if (lx >= fs * 2 && ly >= fs * 2 && lx < width - fs * 3 && ly < height - fs * 2) {
                snprintf(txt, sizeof(txt), "%d%%", (int)(levels[i] * 100 + 0.5));
                pic_draw_text_pill(cr, lx, ly, txt, 1.0, 0.85, 0.6, 0.55);
            }
        }
        free(g);
    }

    /* the umbra / antumbra now */
    {
        double clat, clon, la[RAYS], lo[RAYS];
        if (pic_ecl_solar_centre(e, t, &clat, &clon) &&
            umbra_outline(e, t, clat, clon, la, lo)) {
            pic_ecl_local_t o;
            int dur, total;
            double cx = pic_lon_to_x(clon, width), cy = pic_lat_to_y(clat, height);
            pic_ecl_solar_local(e, t, clat, clon, &o);
            total = o.central == 1;
            if (eclipse_style() != 0) {
                poly_path(cr, la, lo, RAYS, width, height);
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.02, 0.92);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, 1.0, total ? 0.35 : 0.75, total ? 0.25 : 0.2, 0.95);
                cairo_set_line_width(cr, fs * 0.16);
                cairo_stroke(cr);
            } else {
                /* the umbra: a black core with a feathered edge (it is
                 * only a few pixels across at world scale); the antumbra
                 * stays grey, a ring of Sun still shines there */
                int k;
                double base = total ? 0.9 : 0.45;
                cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                for (k = 4; k >= 1; k--) {
                    poly_path(cr, la, lo, RAYS, width, height);
                    cairo_set_source_rgba(cr, 0.0, 0.0, 0.02, base * 0.18);
                    cairo_set_line_width(cr, fs * 0.35 * k);
                    cairo_stroke(cr);
                }
                poly_path(cr, la, lo, RAYS, width, height);
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.02, base);
                cairo_fill(cr);
            }
            dur = central_duration(e, t, clat, clon);
            snprintf(txt, sizeof(txt), "%s %dm %02ds", total ? "TOTALITY" : "ANNULARITY",
                     dur / 60, dur % 60);
            pic_draw_text_pill(cr, cx + fs * 0.9, cy + fs * 1.5, txt, 1.0, total ? 0.5 : 0.8, 0.35,
                               eclipse_style() != 0 ? 0.75 : 0.45);
        }
    }

    /* from home */
    pic_ecl_solar_local(e, t, pic_config.qth_lat, pic_config.qth_lon, &ho);
    if (ho.obscuration > 0) {
        double hx = pic_lon_to_x(pic_config.qth_lon, width), hy = pic_lat_to_y(pic_config.qth_lat, height);
        double R = height / 32.0, ix = hx + R * 3.0, iy = hy + R * 2.6;
        if (ix + R * 2 > width) ix = hx - R * 3.0;
        if (iy + R * 2.6 > height) iy = hy - R * 2.6;
        snprintf(txt, sizeof(txt), ho.central == 1 ? "TOTAL" : ho.central == 2 ? "RING" : "%d%% covered",
                 (int)(ho.obscuration * 100 + 0.5));
        sun_inset(cr, ix, iy, R, &ho, txt);
    }

    /* legend */
    hm(a, sizeof(a), (double)e->tmax);
    if (w->replay) {
        hm(c, sizeof(c), t);
        snprintf(txt, sizeof(txt), "REPLAY %.0f\xc3\x97 \xc2\xb7 %s of %s \xc2\xb7 shown: %s",
                 REPLAY_SPEED, solar_name(e->type), e->date, c);
    } else
        snprintf(txt, sizeof(txt), "%s \xc2\xb7 greatest at %s \xc2\xb7 the Moon's shadow, darker where more of the Sun is covered",
                 solar_name(e->type), a);
    set_legend(0, txt);
    if (home.max_o <= 0)
        snprintf(txt, sizeof(txt), "Not visible from home");
    else {
        hm(a, sizeof(a), home.t_start);
        hm(b, sizeof(b), home.t_max);
        hm(c, sizeof(c), home.t_end);
        if (home.central)
            snprintf(txt, sizeof(txt), "At home: %s for %dm %02ds at %s \xc2\xb7 eclipse %s\xe2\x80\x93%s",
                     home.central == 1 ? "TOTAL" : "annular", home.dur / 60, home.dur % 60, b, a, c);
        else if (ho.obscuration > 0)
            snprintf(txt, sizeof(txt), "At home: %d%% of the Sun covered now \xc2\xb7 max %d%% at %s \xc2\xb7 ends %s",
                     (int)(ho.obscuration * 100 + 0.5), (int)(home.max_o * 100 + 0.5), b, c);
        else
            snprintf(txt, sizeof(txt), "At home: %s\xe2\x80\x93%s, max %d%% of the Sun covered at %s",
                     a, c, (int)(home.max_o * 100 + 0.5), b);
    }
    set_legend(1, txt);
}

/* ---------------------------------------------------------------- */
/* Lunar                                                             */

/* The Moon at (cx, cy), radius r, with Earth's shadow across it */
static void eclipsed_moon(cairo_t *cr, double cx, double cy, double r,
                          const pic_ecl_lunar_state_t *s)
{
    double k = r / s->sm, sx = cx + s->x * k, sy = cy + s->y * k;
    cairo_pattern_t *p;

    pic_draw_moon(cr, cx, cy, r, 1.0, 0);
    cairo_save(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_clip(cr);
    /* penumbra: dimming towards the umbra */
    p = cairo_pattern_create_radial(sx, sy, s->ru * k, sx, sy, s->rp * k);
    cairo_pattern_add_color_stop_rgba(p, 0, 0, 0, 0, 0.55);
    cairo_pattern_add_color_stop_rgba(p, 1, 0, 0, 0, 0.0);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
    /* umbra: coppery red */
    p = cairo_pattern_create_radial(sx, sy, 0, sx, sy, s->ru * k);
    cairo_pattern_add_color_stop_rgba(p, 0, 0.18, 0.03, 0.02, 0.93);
    cairo_pattern_add_color_stop_rgba(p, 0.85, 0.42, 0.1, 0.04, 0.88);
    cairo_pattern_add_color_stop_rgba(p, 1, 0.55, 0.18, 0.06, 0.8);
    cairo_set_source(cr, p);
    cairo_arc(cr, sx, sy, s->ru * k, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    cairo_restore(cr);
}

static void lunar_inset(cairo_t *cr, double x0, double y0, double W, double H,
                        const pic_ecl_lunar_t *e, double t, const pic_ecl_lunar_state_t *s)
{
    double fs = H / 11.0, cx = x0 + W / 2, cy = y0 + H * 0.52;
    double span = (s->rp + 2.2 * s->sm), k = (H * 0.40) / span;
    double tt;
    int first = 1, i;
    pic_ecl_lunar_state_t q;

    cairo_save(cr);
    cairo_rectangle(cr, x0, y0, W, H);
    cairo_set_source_rgba(cr, 0.02, 0.03, 0.06, 0.85);
    cairo_fill(cr);
    cairo_rectangle(cr, x0, y0, W, H);
    cairo_clip(cr);
    /* shadow circles (screen: east left, north up) */
    cairo_arc(cr, cx, cy, s->rp * k, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.45, 0.47, 0.55, 0.35);
    cairo_fill(cr);
    cairo_arc(cr, cx, cy, s->ru * k, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.45, 0.1, 0.06, 0.8);
    cairo_fill(cr);
    /* the Moon's track, with contact ticks */
    cairo_set_source_rgba(cr, 0.8, 0.82, 0.9, 0.5);
    cairo_set_line_width(cr, 1.2);
    for (tt = (double)e->c[0] - 1800; tt <= (double)e->c[1] + 1800; tt += 300) {
        pic_ecl_lunar_state(e, tt, &q);
        if (first) cairo_move_to(cr, cx - q.x * k, cy - q.y * k);
        else cairo_line_to(cr, cx - q.x * k, cy - q.y * k);
        first = 0;
    }
    cairo_stroke(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs * 0.7);
    for (i = 0; i < 6; i++) {
        if (!e->c[i]) continue;
        pic_ecl_lunar_state(e, (double)e->c[i], &q);
        cairo_new_path(cr);
        cairo_arc(cr, cx - q.x * k, cy - q.y * k, fs * 0.18, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    /* the Moon now (east is left, north up, as in the sky) */
    eclipsed_moon(cr, cx - s->x * k, cy - s->y * k, s->sm * k, s);
    cairo_restore(cr);
    {   /* caption */
        const char *ph = e->c[4] && t >= e->c[4] && t <= e->c[5] ? "Totality"
                       : s->umbral > 0 ? "Partial" : s->penumbral > 0 ? "Penumbral" : "";
        cairo_text_extents_t te;
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        cairo_text_extents(cr, ph, &te);
        cairo_set_source_rgba(cr, 1.0, 0.7, 0.6, 1.0);
        cairo_move_to(cr, x0 + (W - te.x_advance) / 2, y0 + fs * 1.2);
        cairo_show_text(cr, ph);
    }
}

static void render_lunar(cairo_t *cr, int width, int height, const ecl_show_t *w)
{
    const pic_ecl_lunar_t *e = w->l;
    pic_ecl_lunar_state_t s;
    double fs = height / 72.0, cell = height / 120.0, t = w->t;
    int gw = (int)(width / cell) + 2, gh = (int)(height / cell) + 2, i, j;
    float *g;
    char txt[200], a[16], b[16];
    double sd, cd, hv;

    pic_ecl_lunar_state(e, t, &s);
    sd = sin(s.sublat * D2R);
    cd = cos(s.sublat * D2R);

    if (w->preview) {
        double left = (double)e->c[0] - (double)time(NULL);
        hm(a, sizeof(a), (double)e->tmax);
        snprintf(txt, sizeof(txt), "%s begins in %dh %02dm \xc2\xb7 greatest at %s",
                 lunar_name(e->kind), (int)(left / 3600), (int)fmod(left / 60, 60), a);
        set_legend(0, txt);
        set_legend(1, "");
        return;
    }

    /* where the Moon is up */
    g = malloc(sizeof(float) * (size_t)gw * (size_t)gh);
    if (g) {
        cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gw, gh);
        unsigned char *px = cairo_image_surface_get_data(img);
        int stride = cairo_image_surface_get_stride(img);
        cairo_pattern_t *pat;
        cairo_matrix_t m;
        double lx, ly;
        cairo_surface_flush(img);
        for (j = 0; j < gh; j++) {
            double lat = pic_y_to_lat(j * cell, height) * D2R;
            unsigned int *row = (unsigned int *)(void *)(px + j * stride);
            for (i = 0; i < gw; i++) {
                double lon = pic_x_to_lon(i * cell, width);
                double v = sin(lat) * sd + cos(lat) * cd * cos((lon - s.sublon) * D2R);
                unsigned int al = v > 0 ? (unsigned int)(255 * 0.09) : 0;
                g[j * gw + i] = (float)v;
                row[i] = al << 24 | (unsigned int)(al * 0.85) << 16 |
                         (unsigned int)(al * 0.7) << 8 | (unsigned int)(al * 0.75);
            }
        }
        cairo_surface_mark_dirty(img);
        pat = cairo_pattern_create_for_surface(img);
        cairo_pattern_set_filter(pat, CAIRO_FILTER_BILINEAR);
        cairo_matrix_init_scale(&m, 1.0 / cell, 1.0 / cell);
        cairo_pattern_set_matrix(pat, &m);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);
        cairo_surface_destroy(img);
        cairo_new_path(cr);
        contour(cr, g, gw, gh, cell, 0, 0, 0.0, 0, &lx, &ly);
        {
            double dash[2] = { fs * 0.5, fs * 0.35 };
            cairo_set_dash(cr, dash, 2, 0);
            cairo_set_source_rgba(cr, 1.0, 0.65, 0.5, 0.7);
            cairo_set_line_width(cr, fs * 0.12);
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0);
        }
        free(g);
    }

    /* the eclipsed Moon over the sub-lunar point, and the diagram */
    {
        double mx = pic_lon_to_x(s.sublon, width), my = pic_lat_to_y(s.sublat, height);
        double r = height / 40.0, W = height / 4.2, H = height / 5.0, bx, by;
        eclipsed_moon(cr, mx, my, r, &s);
        bx = mx + r * 1.8;
        by = my - H / 2;
        if (bx + W > width - fs) bx = mx - r * 1.8 - W;
        if (by < fs * 3) by = fs * 3;
        if (by + H > height - fs * 6) by = height - fs * 6 - H;
        lunar_inset(cr, bx, by, W, H, e, t, &s);
    }

    /* legend */
    {
        const char *phase;
        if (e->c[4] && t >= e->c[4] && t <= e->c[5]) phase = "totality";
        else if (s.umbral > 0) phase = "partial (umbral) phase";
        else if (s.penumbral > 0) phase = "penumbral phase";
        else phase = "Moon outside the shadow";
        if (w->replay) {
            hm(b, sizeof(b), t);
            snprintf(txt, sizeof(txt), "REPLAY %.0f\xc3\x97 \xc2\xb7 %s of %s \xc2\xb7 shown: %s \xc2\xb7 %s",
                     REPLAY_SPEED, lunar_name(e->kind), e->date, b, phase);
        } else
            snprintf(txt, sizeof(txt), "%s \xc2\xb7 %s \xc2\xb7 visible where tinted (Moon up)",
                     lunar_name(e->kind), phase);
        set_legend(0, txt);
    }
    {
        double lat = pic_config.qth_lat * D2R;
        hv = sin(lat) * sd + cos(lat) * cd * cos((pic_config.qth_lon - s.sublon) * D2R);
        if (e->c[4]) {
            hm(a, sizeof(a), (double)e->c[4]);
            hm(b, sizeof(b), (double)e->c[5]);
            snprintf(txt, sizeof(txt), "Totality %s\xe2\x80\x93%s \xc2\xb7 Moon %s at home",
                     a, b, hv > 0 ? "up" : "below the horizon");
        } else if (e->c[2]) {
            hm(a, sizeof(a), (double)e->c[2]);
            hm(b, sizeof(b), (double)e->c[3]);
            snprintf(txt, sizeof(txt), "Partial %s\xe2\x80\x93%s (%.0f%% of the Moon in the umbra at most) \xc2\xb7 Moon %s at home",
                     a, b, e->umag * 100, hv > 0 ? "up" : "below the horizon");
        } else {
            hm(a, sizeof(a), (double)e->tmax);
            snprintf(txt, sizeof(txt), "Faint dimming only, greatest at %s \xc2\xb7 Moon %s at home",
                     a, hv > 0 ? "up" : "below the horizon");
        }
        set_legend(1, txt);
    }
}

/* ---------------------------------------------------------------- */

static void eclipse_render(cairo_t *cr, int width, int height, time_t now, void *ud)
{
    ecl_show_t w;
    (void)ud;
    if (!what(now, &w)) return;
    if (w.s) {
        legend_rgb[0] = 1.0; legend_rgb[1] = 0.8; legend_rgb[2] = 0.5;
        render_solar(cr, width, height, &w);
    } else {
        legend_rgb[0] = 1.0; legend_rgb[1] = 0.7; legend_rgb[2] = 0.6;
        render_lunar(cr, width, height, &w);
    }
}

static void eclipse_legend(cairo_t *cr, int width, int height, time_t now, void *ud)
{
    char a[200], b[200];
    (void)now; (void)ud;
    pthread_mutex_lock(&mu);
    memcpy(a, legend_txt[0], sizeof(a));
    memcpy(b, legend_txt[1], sizeof(b));
    pthread_mutex_unlock(&mu);
    if (b[0]) pic_draw_legend_line(cr, width, height, 0, b, 1.0, 0.9, 0.75);
    if (a[0]) pic_draw_legend_line(cr, width, height, 0, a, legend_rgb[0], legend_rgb[1], legend_rgb[2]);
}

static pic_feature_t feature_def = {
    .name = "eclipse",
    .layer = eclipse_render,
    .legend = eclipse_legend,
    .idle = eclipse_idle,
    .interval = 2,
    .extra_mb = 1,
    .default_on = 1,
    .label = "Eclipses",
};

pic_feature_t *pic_feature_eclipse(void)
{
    return &feature_def;
}
