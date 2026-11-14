/*
 * layers_eclipse.c - Solar and lunar eclipses, live
 *
 * Solar eclipse under way: the eclipse map (colour bands by the most of
 * the Sun covered anywhere it is seen, with 20-80% labels and a key, as
 * on timeanddate's maps), the path of totality or annularity with its
 * dashed centre line, and the Moon's shadow travelling across it as seen
 * from orbit: daylight barely dimmed in the outer penumbra, deepening
 * into a dark smudge around the black umbra.
 *
 * Lunar eclipse under way: the part of the Earth where the Moon is up
 * (where the eclipse can be seen) tinted, the eclipsed Moon over the
 * sub-lunar point, and an inset of the Moon passing through Earth's
 * penumbra and umbra with the phase and contact times.
 *
 * Eclipses within 12 h get a preview (path band and countdown).
 * ECLIPSE_REPLAY (renderer conf): 0 off (default), 1 the next eclipse,
 * 2 the last one, 3 the one named by ECLIPSE_PICK ("S1999-08-11" solar,
 * "L2018-07-27" lunar; any from 1900 to 2050), replayed at 60x in a
 * loop so it can be seen any time.
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
static char key_name[32];        /* solar: draw the band key with this path name */

/* ---------------------------------------------------------------- */
/* What to show                                                      */

typedef struct {
    const pic_ecl_solar_t *s;
    const pic_ecl_lunar_t *l;
    double t;                    /* time to draw (UT seconds)          */
    int replay;                  /* 1 next, 2 last                     */
    int preview;                 /* not started yet: path + countdown  */
} ecl_show_t;

static char pick[16];

static int replay_mode(void)
{
    static int mode;
    static time_t read_at;
    time_t now = time(NULL);
    pthread_mutex_lock(&mu);
    if (!read_at || now - read_at >= 15) {
        char v[16] = "";
        read_at = now;
        pthread_mutex_unlock(&mu);
        mode = pic_conf_int("ECLIPSE_REPLAY", 0);
        pic_conf_str("ECLIPSE_PICK", v, sizeof(v));
        pthread_mutex_lock(&mu);
        snprintf(pick, sizeof(pick), "%s", v);
        pthread_mutex_unlock(&mu);
        return mode;
    }
    pthread_mutex_unlock(&mu);
    return mode;
}

/* The eclipse named by ECLIPSE_PICK */
static int picked(const pic_ecl_solar_t **s, const pic_ecl_lunar_t **l)
{
    char v[16];
    int i;
    pthread_mutex_lock(&mu);
    memcpy(v, pick, sizeof(v));
    pthread_mutex_unlock(&mu);
    if (v[0] == 'S')
        for (i = 0; i < pic_ecl_nsolar; i++)
            if (!strcmp(pic_ecl_solar[i].date, v + 1)) { *s = &pic_ecl_solar[i]; return 1; }
    if (v[0] == 'L')
        for (i = 0; i < pic_ecl_nlunar; i++)
            if (!strcmp(pic_ecl_lunar[i].date, v + 1) && pic_ecl_lunar[i].c[0]) {
                *l = &pic_ecl_lunar[i];
                return 1;
            }
    return 0;
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
    if (mode >= 1 && mode <= 3) {
        const pic_ecl_solar_t *s = NULL;
        const pic_ecl_lunar_t *l = NULL;
        double a, b, loop;
        if (mode == 3) {
            if (!picked(&s, &l)) return 0;
        } else {
            s = mode == 1 ? pic_ecl_solar_next(now) : pic_ecl_solar_prev(now);
            l = mode == 1 ? pic_ecl_lunar_next(now) : pic_ecl_lunar_prev(now);
        }
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

typedef struct { double x[12], y[12]; int n; double gap; } labels_t;

static void label_spot(labels_t *L, double x, double y)
{
    int k;
    if (!L || L->n >= 12) return;
    for (k = 0; k < L->n; k++)
        if (hypot(L->x[k] - x, L->y[k] - y) < L->gap) return;
    L->x[L->n] = x;
    L->y[L->n] = y;
    L->n++;
}

static void contour_l(cairo_t *cr, const float *g, int gw, int gh, double cell,
                      double ox, double oy, double level, int night, double *lx, double *ly,
                      labels_t *L)
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
                label_spot(L, (ex[k] + ex[k + 1]) / 2, (ey[k] + ey[k + 1]) / 2);
            }
        }
}

static void contour(cairo_t *cr, const float *g, int gw, int gh, double cell,
                    double ox, double oy, double level, int night, double *lx, double *ly)
{
    contour_l(cr, g, gw, gh, cell, ox, oy, level, night, lx, ly, NULL);
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

static int central_duration(const pic_ecl_solar_t *e, double t, double lat, double lon);

/* Path cache: outlines along the whole central path */
#define PATH_STEP 60
typedef struct {
    const pic_ecl_solar_t *e;
    int n;
    int ge_ok, ge_dur;
    double ge_lat, ge_lon;
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
    path.ge_ok = pic_ecl_solar_centre(e, (double)e->tmax, &path.ge_lat, &path.ge_lon);
    if (path.ge_ok) path.ge_dur = central_duration(e, (double)e->tmax, path.ge_lat, path.ge_lon);
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

static void draw_path(cairo_t *cr, int width, int height, const char *label)
{
    int k;
    if (path.n < 2) return;
    cairo_push_group(cr);
    cairo_set_source_rgb(cr, 0.45, 0.12, 0.05);
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
    cairo_paint_with_alpha(cr, 0.6);

    /* centre line, dashed */
    {
        double dash[2] = { height / 140.0, height / 220.0 };
        cairo_set_dash(cr, dash, 2, 0);
    }
    cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
    cairo_set_line_width(cr, height / 600.0 + 0.6);
    for (k = 1; k < path.n; k++) {
        double x0 = pic_lon_to_x(path.clon[k - 1], width), x1 = pic_lon_to_x(path.clon[k], width);
        if (fabs(x1 - x0) > width / 2.0) continue;
        cairo_move_to(cr, x0, pic_lat_to_y(path.clat[k - 1], height));
        cairo_line_to(cr, x1, pic_lat_to_y(path.clat[k], height));
    }
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);
    /* its name, at an on-screen point a third of the way along */
    for (k = path.n / 3; k < path.n; k++) {
        double x = pic_lon_to_x(path.clon[k], width), y = pic_lat_to_y(path.clat[k], height);
        double fs = height / 72.0;
        if (x > fs * 4 && x < width - fs * 10 && y > fs * 3 && y < height - fs * 5) {
            pic_draw_text_pill(cr, x + fs * 0.8, y - fs * 0.9, label, 1, 1, 1, 0.6);
            break;
        }
    }
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
            double o2 = o * o, o4 = o2 * o2, o8 = o4 * o4;
            double a = 0.07 * o + 0.88 * o8;
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

/*
 * The eclipse map (as on timeanddate / NASA maps): for each place, the
 * most of the Sun covered at any moment of the eclipse while the Sun is
 * up there. Computed once per eclipse and view on a coarse screen grid
 * (about 6 px), stepping through the eclipse every 2 minutes.
 */
typedef struct {
    const pic_ecl_solar_t *e;
    int w, h, gw, gh;
    double key[3], cell;
    float *g;
    cairo_surface_t *bands;    /* coloured bands, half resolution */
} foot_t;
static foot_t foot;

static void foot_build(const pic_ecl_solar_t *e, int width, int height)
{
    double key[3] = { pic_x_to_lon(0, width), pic_x_to_lon(width, width), pic_y_to_lat(0, height) };
    double cell = height / 180.0, t, *rs, *rc, *lon;
    int gw = (int)(width / cell) + 2, gh = (int)(height / cell) + 2, i, j;
    if (foot.e == e && foot.w == width && foot.h == height &&
        !memcmp(foot.key, key, sizeof(key)))
        return;
    free(foot.g);
    if (foot.bands) cairo_surface_destroy(foot.bands);
    memset(&foot, 0, sizeof(foot));
    foot.g = calloc((size_t)gw * (size_t)gh, sizeof(float));
    rs = malloc(sizeof(double) * (size_t)(gh * 2 + gw));
    if (!foot.g || !rs) { free(rs); return; }
    rc = rs + gh;
    lon = rc + gh;
    foot.e = e; foot.w = width; foot.h = height; foot.gw = gw; foot.gh = gh;
    foot.cell = cell;
    memcpy(foot.key, key, sizeof(key));
    for (j = 0; j < gh; j++) {
        double lat = pic_y_to_lat(j * cell, height);
        if (lat > 90) lat = 90;
        if (lat < -90) lat = -90;
        pic_ecl_geocentric(lat, &rs[j], &rc[j]);
    }
    for (i = 0; i < gw; i++) lon[i] = pic_x_to_lon(i * cell, width);
    for (t = (double)e->p1; t <= (double)e->p4; t += 120) {
        pic_ecl_state_t s;
        pic_ecl_solar_prep(e, t, &s);
        for (i = 0; i < gw; i++) {
            double H = (s.mu + lon[i]) * D2R, sh = sin(H), ch = cos(H);
            for (j = 0; j < gh; j++) {
                double xi = rc[j] * sh;
                double eta = rs[j] * s.cd - rc[j] * ch * s.sd;
                double zeta = rs[j] * s.sd + rc[j] * ch * s.cd;
                double L1, L2, u, v, m;
                float *o = &foot.g[j * gw + i];
                if (zeta <= 0) continue;
                L1 = s.l1 - zeta * s.tf1;
                u = s.x - xi; v = s.y - eta;
                m = u * u + v * v;
                if (m >= L1 * L1) continue;
                L2 = s.l2 - zeta * s.tf2;
                m = sqrt(m);
                {
                    float ob = (float)pic_ecl_overlap((L1 - L2) / (L1 + L2), 2.0 * m / (L1 + L2));
                    if (ob > *o) *o = ob;
                }
            }
        }
    }
    free(rs);
}

/*
 * Colour bands by the most of the Sun covered, one per 10%, as on
 * eclipse maps: a warm glow over the Blue Marble, barely there where a
 * sliver of the Sun is covered, deepening through amber and orange to
 * rose beside the path, so the map still shows through.
 */
static const double BAND_RGB[10][3] = {
    { 1.00, 0.95, 0.75 }, { 1.00, 0.91, 0.62 }, { 1.00, 0.86, 0.50 }, { 1.00, 0.80, 0.40 },
    { 1.00, 0.73, 0.32 }, { 0.99, 0.66, 0.26 }, { 0.97, 0.58, 0.21 }, { 0.94, 0.50, 0.17 },
    { 0.90, 0.42, 0.14 }, { 0.85, 0.34, 0.11 },
};
static const double BAND_A[10] = { 0.16, 0.19, 0.22, 0.25, 0.28, 0.31, 0.34, 0.37, 0.41, 0.46 };

static void bands_build(int width, int height)
{
    int bw = width / 2 + 1, bh = height / 2 + 1, x, y, stride;
    unsigned char *px;
    if (foot.bands || !foot.g) return;
    foot.bands = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bw, bh);
    px = cairo_image_surface_get_data(foot.bands);
    stride = cairo_image_surface_get_stride(foot.bands);
    cairo_surface_flush(foot.bands);
    for (y = 0; y < bh; y++) {
        unsigned int *row = (unsigned int *)(void *)(px + y * stride);
        double gy = y * 2.0 / foot.cell, fy;
        int j = (int)gy;
        if (j > foot.gh - 2) j = foot.gh - 2;
        fy = gy - j;
        for (x = 0; x < bw; x++) {
            double gx = x * 2.0 / foot.cell, fx, o;
            int i = (int)gx, k;
            const float *g = foot.g;
            if (i > foot.gw - 2) i = foot.gw - 2;
            fx = gx - i;
            /* bilinear between grid points, then banded: crisp edges */
            o = (g[j * foot.gw + i] * (1 - fx) + g[j * foot.gw + i + 1] * fx) * (1 - fy) +
                (g[(j + 1) * foot.gw + i] * (1 - fx) + g[(j + 1) * foot.gw + i + 1] * fx) * fy;
            if (o <= 0.002) {
                /* just outside a sunrise/sunset edge: let the ramp below
                 * fade instead of cutting */
                o = 0;
                row[x] = 0;
                continue;
            }
            k = (int)(o * 10);
            if (k > 9) k = 9;
            {
                /* soften the outer edge where the eclipse starts at
                 * sunrise / ends at sunset (o jumps there): coverage of
                 * the "seen at all" mask, bilinear, as an alpha ramp */
                double c = ((g[j * foot.gw + i] > 0) * (1 - fx) + (g[j * foot.gw + i + 1] > 0) * fx) * (1 - fy) +
                           ((g[(j + 1) * foot.gw + i] > 0) * (1 - fx) + (g[(j + 1) * foot.gw + i + 1] > 0) * fx) * fy;
                double edge = c >= 0.75 ? 1.0 : c <= 0.25 ? 0.0 : (c - 0.25) * 2.0;
                unsigned int al = (unsigned int)(255 * BAND_A[k] * edge);
                row[x] = al << 24 |
                         (unsigned int)(al * BAND_RGB[k][0]) << 16 |
                         (unsigned int)(al * BAND_RGB[k][1]) << 8 |
                         (unsigned int)(al * BAND_RGB[k][2]);
            }
        }
    }
    cairo_surface_mark_dirty(foot.bands);
}

static void draw_footprint(cairo_t *cr, const pic_ecl_solar_t *e, int width, int height)
{
    static const double lv[] = { 0.2, 0.4, 0.6, 0.8 };
    double fs = height / 72.0, lx, ly;
    int i;
    char txt[16];

    foot_build(e, width, height);
    bands_build(width, height);
    if (!foot.bands) return;
    cairo_save(cr);
    cairo_scale(cr, 2.0, 2.0);
    cairo_set_source_surface(cr, foot.bands, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    /* a few % labels on the band edges (no lines) */
    for (i = 0; i < 4; i++) {
        labels_t L;
        int k;
        memset(&L, 0, sizeof(L));
        L.gap = height / 3.5;
        cairo_new_path(cr);
        contour_l(cr, foot.g, foot.gw, foot.gh, foot.cell, 0, 0, lv[i], 0, &lx, &ly, &L);
        cairo_new_path(cr);
        snprintf(txt, sizeof(txt), "%d%%", (int)(lv[i] * 100 + 0.5));
        for (k = 0; k < L.n && k < 3; k++)
            if (L.x[k] > fs * 2 && L.x[k] < width - fs * 2 && L.y[k] > fs * 3 && L.y[k] < height - fs * 5)
                pic_draw_text_pill(cr, L.x[k] - fs * 0.9, L.y[k] + fs * 0.3, txt, 1.0, 0.92, 0.8, 0.35);
    }
}

/* The colour key, bottom centre: like the map legend on timeanddate */
static void draw_band_key(cairo_t *cr, int width, int height, const char *path_name)
{
    double fs = height / 72.0, y = pic_legend_take(height, 1.2), sw = fs * 1.6;
    double x, w, pad = fs * 0.35;
    cairo_text_extents_t te;
    int k;
    const char *lo = "0%", *mid = "50%", *hi = "100%";

    cairo_save(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs * 0.85);
    cairo_text_extents(cr, path_name, &te);
    w = te.x_advance + pad * 2 + sw * 10 + pad;
    x = (width - w) / 2;
    cairo_set_source_rgba(cr, 0, 0, 0, 0.65);
    cairo_rectangle(cr, x - pad, y - fs * 1.15, w + pad * 2, fs * 1.6);
    cairo_fill(cr);
    /* the path swatch */
    cairo_set_source_rgba(cr, 0.45, 0.12, 0.05, 0.95);
    cairo_rectangle(cr, x, y - fs * 0.95, te.x_advance + pad * 2, fs * 1.2);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, x + pad, y - fs * 0.05);
    cairo_show_text(cr, path_name);
    x += te.x_advance + pad * 3;
    /* bands, high to low (as timeanddate: path, <100% ... >0%) */
    for (k = 9; k >= 0; k--) {
        cairo_set_source_rgba(cr, BAND_RGB[k][0], BAND_RGB[k][1], BAND_RGB[k][2], 0.35 + BAND_A[k] * 1.6);
        cairo_rectangle(cr, x + (9 - k) * sw, y - fs * 0.95, sw, fs * 1.2);
        cairo_fill(cr);
    }
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_font_size(cr, fs * 0.72);
    cairo_move_to(cr, x + fs * 0.15, y - fs * 0.1);
    cairo_show_text(cr, hi);
    cairo_text_extents(cr, mid, &te);
    cairo_move_to(cr, x + 5 * sw - te.x_advance / 2, y - fs * 0.1);
    cairo_show_text(cr, mid);
    cairo_text_extents(cr, lo, &te);
    cairo_move_to(cr, x + 10 * sw - te.x_advance - fs * 0.15, y - fs * 0.1);
    cairo_show_text(cr, lo);
    cairo_restore(cr);
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
    double t = w->t, fs = height / 72.0;
    char txt[200], a[16], b[16], c[16];
    pic_ecl_local_t ho;
    const char *pname = e->type == 'T' ? "Path of totality" : e->type == 'A' ? "Path of annularity"
                      : "Central path";

    path_build(e);
    home_build(e);

    /* the eclipse map: where it is seen and how much, and the path */
    draw_footprint(cr, e, width, height);
    draw_path(cr, width, height, pname);

    if (w->preview) {
        double left = (double)e->p1 - (double)time(NULL);
        hm(a, sizeof(a), (double)e->tmax);
        snprintf(txt, sizeof(txt), "%s begins in %dh %02dm \xc2\xb7 greatest at %s",
                 solar_name(e->type), (int)(left / 3600), (int)fmod(left / 60, 60), a);
        set_legend(0, txt);
        set_legend(1, "");
        return;
    }

    /* the shadow now, as seen from orbit */
    draw_realistic(cr, e, t, width, height);
    {
        double clat, clon, la[RAYS], lo[RAYS];
        if (pic_ecl_solar_centre(e, t, &clat, &clon) &&
            umbra_outline(e, t, clat, clon, la, lo)) {
            pic_ecl_local_t o;
            int k;
            double base;
            pic_ecl_solar_local(e, t, clat, clon, &o);
            /* the umbra: a black core with a feathered edge; the antumbra
             * stays grey (a ring of Sun still shines there) */
            base = o.central == 1 ? 0.9 : 0.45;
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
    }

    /* legend */
    pic_ecl_solar_local(e, t, pic_config.qth_lat, pic_config.qth_lon, &ho);
    hm(a, sizeof(a), (double)e->tmax);
    {
        char ge[80] = "";
        if (path.ge_ok && path.ge_dur > 0)
            snprintf(ge, sizeof(ge), " over %.0f\xc2\xb0%c %.0f\xc2\xb0%c (%s %dm %02ds)",
                     fabs(path.ge_lat), path.ge_lat >= 0 ? 'N' : 'S',
                     fabs(path.ge_lon), path.ge_lon >= 0 ? 'E' : 'W',
                     e->type == 'A' ? "annular" : "total", path.ge_dur / 60, path.ge_dur % 60);
        if (w->replay) {
            hm(c, sizeof(c), t);
            snprintf(txt, sizeof(txt), "REPLAY %.0f\xc3\x97 \xc2\xb7 %s of %s \xc2\xb7 greatest%s \xc2\xb7 shown: %s",
                     REPLAY_SPEED, solar_name(e->type), e->date, ge, c);
        } else
            snprintf(txt, sizeof(txt), "%s \xc2\xb7 greatest at %s%s",
                     solar_name(e->type), a, ge);
    }
    set_legend(0, txt);
    pthread_mutex_lock(&mu);
    snprintf(key_name, sizeof(key_name), "%s", e->type == 'T' ? "Total" : e->type == 'A' ? "Annular"
             : e->type == 'H' ? "Total/annular" : "");
    pthread_mutex_unlock(&mu);
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
    char a[200], b[200], k[32];
    (void)now; (void)ud;
    pthread_mutex_lock(&mu);
    memcpy(a, legend_txt[0], sizeof(a));
    memcpy(b, legend_txt[1], sizeof(b));
    memcpy(k, key_name, sizeof(k));
    pthread_mutex_unlock(&mu);
    if (a[0] && legend_rgb[1] == 0.8) draw_band_key(cr, width, height, k[0] ? k : "Partial");
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
