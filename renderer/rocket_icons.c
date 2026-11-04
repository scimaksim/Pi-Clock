/*
 * rocket_icons.c - Launch vehicle drawings for the launches layer
 *
 * Each vehicle is drawn side-on from its real dimensions (metres) and
 * livery: Falcon 9 with its black interstage, grid fins and folded
 * legs (or Dragon on top for crew and cargo flights), Falcon Heavy's
 * three cores, Starship's bare steel with the black heat shield on
 * the ship, Electron's black carbon, Vulcan's orange booster and
 * solid motors, Atlas V's salmon core, SLS's orange core and Orion's
 * launch abort tower, Soyuz with its tapered strap-ons and open
 * truss (and escape tower on crewed flights), Ariane 6, H3's orange
 * foam, the Long March family, and a generic rocket for the rest.
 *
 * Bodies are shaded as cylinders lit from the upper left; a fine edge
 * keeps light vehicles visible on day tiles and a light edge keeps
 * dark ones (Electron, Neutron) visible at night. Every vehicle is
 * scaled to about the requested height, nudged by its real size so a
 * Starship still stands taller than an Electron.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sat_icons.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double r, g, b; } col_t;

static const col_t WHITE   = { 0.93, 0.93, 0.94 };
static const col_t OFFWHT  = { 0.86, 0.87, 0.88 };
static const col_t BLACK   = { 0.10, 0.10, 0.11 };
static const col_t CARBON  = { 0.15, 0.15, 0.16 };
static const col_t STEEL   = { 0.72, 0.74, 0.77 };
static const col_t TILE    = { 0.08, 0.08, 0.09 };
static const col_t ENGINE  = { 0.36, 0.33, 0.31 };
static const col_t DKGREY  = { 0.22, 0.22, 0.24 };
static const col_t VULCAN  = { 0.86, 0.50, 0.24 };   /* orange foam    */
static const col_t ATLAS   = { 0.87, 0.58, 0.38 };   /* salmon core    */
static const col_t SLSORG  = { 0.86, 0.46, 0.18 };
static const col_t H3ORG   = { 0.84, 0.55, 0.30 };
static const col_t SOYUZ   = { 0.67, 0.71, 0.66 };   /* grey-green     */
static const col_t LMCREAM = { 0.92, 0.90, 0.84 };
static const col_t SOLAR   = { 0.12, 0.16, 0.30 };   /* trunk cells    */

typedef struct {
    cairo_t *cr;
    double lw;       /* edge width in metres                          */
    int    detail;   /* drawn big enough for fine detail              */
    int    lit;
    int    dark;     /* dark vehicle: light edges so it shows at night */
} rk_t;

/* ── Matching ─────────────────────────────────────────────────── */

static int has(const char *name, const char *want)
{
    size_t n = strlen(want), i, j;
    if (!name) return 0;
    for (i = 0; name[i]; i++) {
        for (j = 0; j < n && name[i + j]; j++)
            if (tolower((unsigned char)name[i + j]) != want[j]) break;
        if (j == n) return 1;
    }
    return 0;
}

/* Text after `key` (case-insensitive), or NULL */
static const char *after(const char *name, const char *key)
{
    size_t n = strlen(key), i, j;
    if (!name) return NULL;
    for (i = 0; name[i]; i++) {
        for (j = 0; j < n && name[i + j]; j++)
            if (tolower((unsigned char)name[i + j]) != key[j]) break;
        if (j == n) return name + i + n;
    }
    return NULL;
}

/* ── Drawing primitives (metres, y up from the base) ─────────── */

static double lum(col_t c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }

static void stop(cairo_pattern_t *p, double at, col_t c, double mul, double add, double a)
{
    cairo_pattern_add_color_stop_rgba(p, at, fmin(1, c.r * mul + add),
                                      fmin(1, c.g * mul + add),
                                      fmin(1, c.b * mul + add), a);
}

/* Fill the current path shaded as a cylinder spanning x0..x1, then
 * edge it. metal = bare steel (sharper reflections). */
static void shade(rk_t *k, double x0, double x1, col_t c, int metal, double a)
{
    cairo_t *cr = k->cr;
    cairo_pattern_t *p = cairo_pattern_create_linear(x0, 0, x1, 0);
    if (metal) {
        stop(p, 0.00, c, 0.50, 0.00, a);
        stop(p, 0.18, c, 1.22, 0.10, a);
        stop(p, 0.30, c, 0.92, 0.00, a);
        stop(p, 0.55, c, 1.04, 0.03, a);
        stop(p, 1.00, c, 0.45, 0.00, a);
    } else {
        stop(p, 0.00, c, 0.56, 0.00, a);
        stop(p, 0.24, c, 1.06, 0.05, a);
        stop(p, 0.50, c, 0.98, 0.00, a);
        stop(p, 0.82, c, 0.78, 0.00, a);
        stop(p, 1.00, c, 0.52, 0.00, a);
    }
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    if (k->dark && lum(c) < 0.3) cairo_set_source_rgba(cr, 0.66, 0.69, 0.76, 0.85 * a);
    else              cairo_set_source_rgba(cr, 0.03, 0.04, 0.08, 0.8 * a);
    cairo_set_line_width(cr, k->lw);
    cairo_stroke(cr);
}

/* Cylinder or frustum from y0 to y1, diameters d0 (bottom) / d1 (top) */
static void seg(rk_t *k, double cx, double y0, double y1, double d0, double d1,
                col_t c, int metal)
{
    double w = fmax(d0, d1) / 2;
    cairo_new_path(k->cr);
    cairo_move_to(k->cr, cx - d0 / 2, y0);
    cairo_line_to(k->cr, cx + d0 / 2, y0);
    cairo_line_to(k->cr, cx + d1 / 2, y1);
    cairo_line_to(k->cr, cx - d1 / 2, y1);
    cairo_close_path(k->cr);
    shade(k, cx - w, cx + w, c, metal, 0.97);
}

static void nose_path(cairo_t *cr, double cx, double y0, double y1, double d, double blunt)
{
    double h = y1 - y0, r = d / 2;
    cairo_move_to(cr, cx - r, y0);
    cairo_curve_to(cr, cx - r, y0 + h * (0.42 + 0.3 * blunt),
                   cx - r * (0.25 + 0.6 * blunt), y1, cx, y1);
    cairo_curve_to(cr, cx + r * (0.25 + 0.6 * blunt), y1,
                   cx + r, y0 + h * (0.42 + 0.3 * blunt), cx + r, y0);
    cairo_close_path(cr);
}

/* Ogive nose / fairing top; blunt 0 (pointed) .. 1 (rounded) */
static void nose(rk_t *k, double cx, double y0, double y1, double d, col_t c,
                 int metal, double blunt)
{
    cairo_new_path(k->cr);
    nose_path(k->cr, cx, y0, y1, d, blunt);
    shade(k, cx - d / 2, cx + d / 2, c, metal, 0.97);
}

/* Soot / weathering: dark at y0 fading out by y1 */
static void soot(rk_t *k, double cx, double y0, double y1, double d, double a)
{
    cairo_pattern_t *p = cairo_pattern_create_linear(0, y0, 0, y1);
    cairo_pattern_add_color_stop_rgba(p, 0, 0.16, 0.14, 0.13, a);
    cairo_pattern_add_color_stop_rgba(p, 1, 0.16, 0.14, 0.13, 0);
    cairo_new_path(k->cr);
    cairo_rectangle(k->cr, cx - d / 2, y0, d, y1 - y0);
    cairo_set_source(k->cr, p);
    cairo_fill(k->cr);
    cairo_pattern_destroy(p);
}

/* Engine bell(s): n nozzles across, flaring from d1 (top) to d0 */
static void bells(rk_t *k, double cx, double y0, double y1, double d0, double d1,
                  int n, double sep)
{
    int i;
    for (i = 0; i < n; i++) {
        double x = cx + (i - (n - 1) / 2.0) * sep, h = y1 - y0;
        cairo_new_path(k->cr);
        cairo_move_to(k->cr, x - d1 / 2, y1);
        cairo_curve_to(k->cr, x - d1 / 2, y1 - h * 0.5, x - d0 / 2, y0 + h * 0.3, x - d0 / 2, y0);
        cairo_line_to(k->cr, x + d0 / 2, y0);
        cairo_curve_to(k->cr, x + d0 / 2, y0 + h * 0.3, x + d1 / 2, y1 - h * 0.5, x + d1 / 2, y1);
        cairo_close_path(k->cr);
        shade(k, x - d0 / 2, x + d0 / 2, ENGINE, 1, 0.97);
    }
}

/* Lattice grid fins sticking out both sides at y..y+h */
static void gridfins(rk_t *k, double cx, double y, double h, double d, double out, col_t c)
{
    int s, i;
    for (s = -1; s <= 1; s += 2) {
        double x0 = cx + s * d / 2, x1 = cx + s * (d / 2 + out);
        cairo_new_path(k->cr);
        cairo_rectangle(k->cr, fmin(x0, x1), y, out, h);
        shade(k, fmin(x0, x1), fmax(x0, x1), c, 1, 0.95);
        if (k->detail) {
            cairo_set_source_rgba(k->cr, 0.0, 0.0, 0.0, 0.5);
            cairo_set_line_width(k->cr, k->lw * 0.6);
            for (i = 1; i < 4; i++) {
                cairo_move_to(k->cr, x0 + (x1 - x0) * i / 4, y);
                cairo_line_to(k->cr, x0 + (x1 - x0) * i / 4, y + h);
            }
            cairo_move_to(k->cr, x0, y + h / 2);
            cairo_line_to(k->cr, x1, y + h / 2);
            cairo_stroke(k->cr);
        }
    }
}

/* Folded landing legs along both sides, with feet at the bottom */
static void legs(rk_t *k, double cx, double y0, double y1, double d, double w, col_t c)
{
    int s;
    for (s = -1; s <= 1; s += 2) {
        double xi = cx + s * (d / 2 - w * 0.5), xo = cx + s * (d / 2 + w * 0.3);
        cairo_new_path(k->cr);
        cairo_move_to(k->cr, xi, y1);
        cairo_line_to(k->cr, xo - s * w * 0.3, y1);
        cairo_line_to(k->cr, xo, y0 + w * 1.5);
        cairo_line_to(k->cr, xo + s * w * 0.25, y0);
        cairo_line_to(k->cr, xi, y0);
        cairo_close_path(k->cr);
        shade(k, fmin(xi, xo), fmax(xi, xo), c, 0, 0.97);
    }
}

/* Fins / flaps / strakes: root y0..y1 on the body, tip out from it */
static void fins(rk_t *k, double cx, double y0, double y1, double d, double out,
                 double tip_lo, double tip_hi, col_t c)
{
    int s;
    double h = y1 - y0;
    for (s = -1; s <= 1; s += 2) {
        double xr = cx + s * d / 2, xt = cx + s * (d / 2 + out);
        cairo_new_path(k->cr);
        cairo_move_to(k->cr, xr, y0);
        cairo_line_to(k->cr, xt, y0 + h * tip_lo);
        cairo_line_to(k->cr, xt, y0 + h * tip_hi);
        cairo_line_to(k->cr, xr, y1);
        cairo_close_path(k->cr);
        shade(k, fmin(xr, xt), fmax(xr, xt), c, 0, 0.97);
    }
}

/* Open truss interstage (Soyuz, Long March 2) */
static void truss(rk_t *k, double cx, double y0, double y1, double d)
{
    int i, n = 4;
    cairo_new_path(k->cr);
    cairo_rectangle(k->cr, cx - d / 2, y0, d, y1 - y0);
    cairo_set_source_rgba(k->cr, 0.05, 0.05, 0.06, 0.92);
    cairo_fill(k->cr);
    cairo_set_source_rgba(k->cr, 0.62, 0.64, 0.62, 0.95);
    cairo_set_line_width(k->cr, k->lw * 1.2);
    for (i = 0; i < n; i++) {
        double xa = cx - d / 2 + d * i / n, xb = xa + d / n;
        cairo_move_to(k->cr, xa, y0);
        cairo_line_to(k->cr, xb, y1);
        cairo_move_to(k->cr, xb, y0);
        cairo_line_to(k->cr, xa, y1);
    }
    cairo_stroke(k->cr);
}

/* Launch escape / abort tower on top of a crew capsule fairing */
static void tower(rk_t *k, double cx, double y0, double h, double d)
{
    seg(k, cx, y0, y0 + h * 0.5, d * 0.14, d * 0.14, OFFWHT, 0);
    /* abort motor with canted nozzles */
    seg(k, cx, y0 + h * 0.46, y0 + h * 0.5, d * 0.62, d * 0.3, ENGINE, 1);
    seg(k, cx, y0 + h * 0.5, y0 + h * 0.84, d * 0.34, d * 0.34, WHITE, 0);
    nose(k, cx, y0 + h * 0.84, y0 + h, d * 0.34, WHITE, 0, 0.2);
}

/* Exhaust plume below y (lit = launching within the hour) */
static void flame(rk_t *k, double cx, double y, double w, double len, int solid)
{
    cairo_t *cr = k->cr;
    cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y - len);
    cairo_pattern_add_color_stop_rgba(p, 0.00, 1.0, 0.98, 0.9, 1.0);
    cairo_pattern_add_color_stop_rgba(p, 0.12, 1.0, solid ? 0.92 : 0.85, solid ? 0.62 : 0.45, 0.97);
    cairo_pattern_add_color_stop_rgba(p, 0.50, 1.0, 0.55, 0.15, 0.75);
    cairo_pattern_add_color_stop_rgba(p, 1.00, 0.85, 0.3, 0.1, 0.0);
    cairo_new_path(cr);
    cairo_move_to(cr, cx - w / 2, y);
    cairo_curve_to(cr, cx - w * (solid ? 0.9 : 0.7), y - len * 0.35,
                   cx - w * 0.25, y - len * 0.85, cx, y - len);
    cairo_curve_to(cr, cx + w * 0.25, y - len * 0.85,
                   cx + w * (solid ? 0.9 : 0.7), y - len * 0.35, cx + w / 2, y);
    cairo_close_path(cr);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

/* ── Strap-on boosters ────────────────────────────────────────── */

enum { B_OGIVE, B_CONE, B_TAPER };

typedef struct {
    int    n;          /* boosters around the core                     */
    double len, d;     /* length incl. nose, diameter                  */
    double nose;       /* nose length                                  */
    double noz;        /* nozzle length                                */
    double r_base;     /* core radius at the booster base              */
    double r_top;      /* core radius at the booster tip (taper)       */
    col_t  c;
    int    kind;
} boost_t;

static void one_booster(rk_t *k, const boost_t *b, double x, double tipx, double dim)
{
    col_t c = { b->c.r * dim, b->c.g * dim, b->c.b * dim };
    if (k->lit) flame(k, x, 0, b->d * 0.9, b->len * 0.55, 1);
    if (b->noz > 0) bells(k, x, 0, b->noz, b->d * 0.7, b->d * 0.45, 1, 0);
    if (b->kind == B_TAPER) {
        /* Soyuz strap-on: a long cone leaning in to the core */
        double r = b->d / 2, y0 = b->noz, y1 = b->len;
        cairo_new_path(k->cr);
        cairo_move_to(k->cr, x - r, y0);
        cairo_line_to(k->cr, x + r, y0);
        cairo_curve_to(k->cr, x + r, y0 + (y1 - y0) * 0.3,
                       tipx + (x + r - tipx) * 0.35, y1 - (y1 - y0) * 0.2, tipx, y1);
        cairo_curve_to(k->cr, tipx - (tipx - (x - r)) * 0.35, y1 - (y1 - y0) * 0.2,
                       x - r, y0 + (y1 - y0) * 0.3, x - r, y0);
        cairo_close_path(k->cr);
        shade(k, x - r, x + r, c, 0, 0.97);
        return;
    }
    seg(k, x, b->noz, b->len - b->nose, b->d, b->d, c, 0);
    if (b->kind == B_CONE)
        seg(k, x, b->len - b->nose, b->len, b->d, b->d * 0.12, c, 0);
    else
        nose(k, x, b->len - b->nose, b->len, b->d, c, 0, 0.1);
}

/* Boosters spaced evenly around the core, seen from the side: those
 * behind first (darker), then the core (drawn by the caller), then
 * those in front. pass 0 = behind, 1 = front. */
static void boosters(rk_t *k, const boost_t *b, double cx, int pass)
{
    int i;
    double off;
    if (b->n <= 0) return;
    off = b->n == 2 ? M_PI / 2 : (b->n == 6 ? M_PI / 6 : 0);
    for (i = 0; i < b->n; i++) {
        double th = off + 2 * M_PI * i / b->n, s = sin(th), z = cos(th);
        double x = cx + (b->r_base + b->d / 2) * s, tipx = cx + b->r_top * s;
        if (fabs(s) < 1e-6 && z < 0) continue;      /* hidden behind */
        if ((pass == 0) != (z < -1e-6)) continue;
        one_booster(k, b, x, tipx, z < 0 ? 0.8 : 1.0);
    }
}

/* ── Vehicles ─────────────────────────────────────────────────── */

typedef enum {
    V_GENERIC, V_F9, V_FH, V_STARSHIP, V_ELECTRON, V_NEUTRON, V_NEWGLENN,
    V_VULCAN, V_ATLAS, V_SLS, V_SOYUZ, V_ARIANE6, V_H3, V_LM, V_DARK
} vkind_t;

typedef struct {
    vkind_t kind;
    double  L;             /* real height, metres                    */
    /* options picked from the names */
    int     dragon;        /* 1 cargo, 2 crew                        */
    int     crew;          /* Soyuz / Long March 2F escape tower     */
    int     nsrb;
    int     longfair;
    int     atlas5m;       /* Atlas V 5xx: 5 m fairing              */
    int     starliner;
    char    lm[8];         /* Long March model, e.g. "3B"           */
} veh_t;

static void falcon_core(rk_t *k, double cx, int side_booster)
{
    double d = 3.66;
    bells(k, cx, 0, 0.9, 3.3, 3.66, 1, 0);           /* octaweb skirt */
    seg(k, cx, 0.9, 41.2, d, d, WHITE, 0);
    soot(k, cx, 0.9, 26, d, 0.42);                   /* flown booster */
    legs(k, cx, 0.9, 10.5, d, 0.45, DKGREY);
    seg(k, cx, 41.2, side_booster ? 42.8 : 47.6, d, d, BLACK, 0);
    gridfins(k, cx, side_booster ? 41.6 : 46.2, 1.1, d, 1.3, DKGREY);
    if (side_booster) nose(k, cx, 42.8, 48.8, d, WHITE, 0, 0.15);
}

static void falcon_top(rk_t *k, const veh_t *v)
{
    double d = 3.66;
    seg(k, 0, 47.6, 56.4, d, d, WHITE, 0);            /* second stage */
    if (v->dragon) {
        /* Trunk with its solar cells on one side, then the capsule */
        seg(k, 0, 56.4, 59.4, d, d, WHITE, 0);
        cairo_new_path(k->cr);
        cairo_rectangle(k->cr, -d / 2, 56.6, d * 0.42, 2.6);
        shade(k, -d / 2, -d / 2 + d * 0.42, SOLAR, 0, 0.95);
        if (v->dragon == 2) fins(k, 0, 56.5, 58.6, d, 0.8, 0.0, 0.55, WHITE);
        seg(k, 0, 59.4, 59.8, 3.9, 3.95, DKGREY, 0);  /* heat shield rim */
        seg(k, 0, 59.8, 63.4, 3.95, 2.5, WHITE, 0);
        /* SuperDraco pods on the capsule sides */
        seg(k, -1.55, 60.4, 61.8, 0.5, 0.42, DKGREY, 0);
        seg(k, 1.55, 60.4, 61.8, 0.5, 0.42, DKGREY, 0);
        nose(k, 0, 63.4, 64.9, 2.5, WHITE, 0, 0.9);
        return;
    }
    seg(k, 0, 56.4, 57.6, d, 5.2, WHITE, 0);          /* boattail */
    seg(k, 0, 57.6, 63.4, 5.2, 5.2, WHITE, 0);
    nose(k, 0, 63.4, 70.0, 5.2, WHITE, 0, 0.1);
    if (k->detail) {                                   /* fairing seam */
        cairo_new_path(k->cr);
        cairo_move_to(k->cr, 0.15, 56.6);
        cairo_line_to(k->cr, 0.15, 69.0);
        cairo_set_source_rgba(k->cr, 0, 0, 0, 0.3);
        cairo_set_line_width(k->cr, k->lw * 0.6);
        cairo_stroke(k->cr);
    }
}

static void draw_f9(rk_t *k, const veh_t *v)
{
    if (k->lit) flame(k, 0, 0, 3.4, 26, 0);
    falcon_core(k, 0, 0);
    falcon_top(k, v);
}

static void draw_fh(rk_t *k, const veh_t *v)
{
    int s;
    if (k->lit) {
        flame(k, -3.96, 0, 3.4, 26, 0);
        flame(k, 3.96, 0, 3.4, 26, 0);
        flame(k, 0, 0, 3.4, 28, 0);
    }
    for (s = -1; s <= 1; s += 2) falcon_core(k, s * 3.96, 1);
    falcon_core(k, 0, 0);
    falcon_top(k, v);
}

static void draw_starship(rk_t *k)
{
    double d = 9.0, i;
    cairo_t *cr = k->cr;
    if (k->lit) flame(k, 0, 0, 8.5, 45, 0);
    /* Super Heavy */
    bells(k, 0, 0, 1.4, 8.4, 9.0, 1, 0);
    seg(k, 0, 1.4, 69.0, d, d, STEEL, 1);
    if (k->detail) {                                   /* ring welds */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
        cairo_set_line_width(cr, k->lw * 0.5);
        for (i = 5; i < 69; i += 4.5) {
            cairo_move_to(cr, -d / 2, i);
            cairo_line_to(cr, d / 2, i);
        }
        cairo_stroke(cr);
    }
    soot(k, 0, 1.4, 14, d, 0.5);
    gridfins(k, 0, 64.0, 4.0, d, 2.4, DKGREY);
    /* Vented hot-staging ring */
    seg(k, 0, 69.0, 71.2, d, d, DKGREY, 1);
    if (k->detail) {
        cairo_set_source_rgba(cr, 0.02, 0.02, 0.02, 0.9);
        for (i = -3.6; i <= 3.7; i += 1.2) {
            cairo_rectangle(cr, i - 0.3, 69.4, 0.6, 1.4);
        }
        cairo_fill(cr);
    }
    /* Ship: steel with the black tiles on the windward (left) side */
    fins(k, 0, 71.6, 79.5, d, 2.6, 0.05, 0.6, TILE);      /* aft flaps */
    seg(k, 0, 71.2, 111.0, d, d, STEEL, 1);
    nose(k, 0, 111.0, 123.0, d, STEEL, 1, 0.2);
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_rectangle(cr, -d / 2, 71.2, d, 39.9);
    nose_path(cr, 0, 111.0, 123.0, d, 0.2);
    cairo_clip(cr);
    cairo_new_path(cr);
    cairo_rectangle(cr, -d / 2 - 1, 71.2, d / 2 + 0.4, 52);
    {
        cairo_pattern_t *p = cairo_pattern_create_linear(-d / 2, 0, 0, 0);
        cairo_pattern_add_color_stop_rgba(p, 0, 0.03, 0.03, 0.035, 0.97);
        cairo_pattern_add_color_stop_rgba(p, 0.45, 0.16, 0.16, 0.17, 0.97);
        cairo_pattern_add_color_stop_rgba(p, 1, 0.09, 0.09, 0.1, 0.97);
        cairo_set_source(cr, p);
        cairo_fill(cr);
        cairo_pattern_destroy(p);
    }
    cairo_restore(cr);
    cairo_new_path(cr);                       /* re-edge the ship */
    cairo_rectangle(cr, -d / 2, 71.2, d, 39.8);
    cairo_set_source_rgba(cr, 0.03, 0.04, 0.08, 0.8);
    cairo_set_line_width(cr, k->lw);
    cairo_stroke(cr);
    fins(k, 0, 106.5, 113.5, d * 0.92, 1.6, 0.1, 0.55, TILE);  /* fwd flaps */
}

static void draw_electron(rk_t *k)
{
    double d = 1.2;
    if (k->lit) flame(k, 0, 0, 1.1, 7, 0);
    bells(k, 0, 0, 0.5, 1.1, 1.2, 1, 0);
    seg(k, 0, 0.5, 12.1, d, d, CARBON, 0);
    seg(k, 0, 12.1, 12.7, d, d, DKGREY, 0);
    seg(k, 0, 12.7, 15.2, d, d, CARBON, 0);
    seg(k, 0, 15.2, 16.4, d, d, CARBON, 0);
    nose(k, 0, 16.4, 18.0, d, CARBON, 0, 0.2);
}

static void draw_neutron(rk_t *k)
{
    if (k->lit) flame(k, 0, 0, 2.6, 16, 0);
    bells(k, 0, 0, 0.8, 2.6, 2.0, 1, 0);
    legs(k, 0, 0.8, 7.5, 7.0, 0.6, DKGREY);
    seg(k, 0, 0.8, 4.0, 7.0, 7.0, CARBON, 0);
    seg(k, 0, 4.0, 35.5, 7.0, 5.0, CARBON, 0);      /* tapered body */
    nose(k, 0, 35.5, 43.0, 5.0, CARBON, 0, 0.75);    /* "Hungry Hippo" */
    fins(k, 0, 33.0, 36.5, 5.1, 1.0, 0.1, 0.7, DKGREY);
}

static void draw_newglenn(rk_t *k)
{
    double d = 7.0;
    if (k->lit) flame(k, 0, 0, 6.5, 36, 0);
    bells(k, 0, 0, 1.4, 6.2, 7.0, 1, 0);
    fins(k, 0, 1.4, 12.0, d, 1.4, 0.0, 0.35, DKGREY);     /* aft strakes */
    seg(k, 0, 1.4, 7.0, d, d, DKGREY, 0);                 /* aft module */
    seg(k, 0, 7.0, 57.5, d, d, WHITE, 0);
    soot(k, 0, 7.0, 30, d, 0.3);
    legs(k, 0, 1.4, 9.0, d, 0.7, DKGREY);
    fins(k, 0, 52.0, 56.5, d, 1.6, 0.15, 0.7, DKGREY);    /* forward fins */
    seg(k, 0, 57.5, 60.5, d, d, BLACK, 0);
    seg(k, 0, 60.5, 76.0, d, d, WHITE, 0);
    seg(k, 0, 76.0, 88.0, d, d, WHITE, 0);
    nose(k, 0, 88.0, 98.0, d, WHITE, 0, 0.1);
}

/* Fairing with a boattail from the stage below; returns the top */
static double fairing(rk_t *k, double y, double d_below, double d, double cyl,
                      double nose_len, col_t c)
{
    if (d > d_below + 0.2) {
        seg(k, 0, y, y + (d - d_below) * 0.6, d_below, d, c, 0);
        y += (d - d_below) * 0.6;
    }
    seg(k, 0, y, y + cyl, d, d, c, 0);
    nose(k, 0, y + cyl, y + cyl + nose_len, d, c, 0, 0.12);
    return y + cyl + nose_len;
}

static void draw_vulcan(rk_t *k, const veh_t *v)
{
    boost_t b = { v->nsrb, 21.9, 1.6, 2.6, 1.0, 2.7, 2.7, WHITE, B_CONE };
    if (k->lit) flame(k, 0, 0, 4.2, 26, 0);
    boosters(k, &b, 0, 0);
    bells(k, 0, 0, 1.6, 1.6, 1.0, 2, 2.4);
    seg(k, 0, 1.6, 33.3, 5.4, 5.4, VULCAN, 0);
    soot(k, 0, 1.6, 9, 5.4, 0.25);
    seg(k, 0, 33.3, 36.3, 5.4, 5.4, OFFWHT, 0);
    seg(k, 0, 36.3, 36.6, 5.4, 5.4, DKGREY, 0);
    seg(k, 0, 36.6, 46.1, 5.4, 5.4, WHITE, 0);         /* Centaur V */
    fairing(k, 46.1, 5.4, 5.4, v->longfair ? 14.8 : 9.0, 6.5, WHITE);
    boosters(k, &b, 0, 1);
}

static void draw_atlas(rk_t *k, const veh_t *v)
{
    boost_t b = { v->nsrb, 20.0, 1.58, 2.4, 1.0, 1.9, 1.9, WHITE, B_CONE };
    if (k->lit) flame(k, 0, 0, 3.4, 24, 0);
    boosters(k, &b, 0, 0);
    bells(k, 0, 0, 1.5, 1.4, 0.9, 2, 1.6);
    seg(k, 0, 1.5, 32.5, 3.81, 3.81, ATLAS, 0);
    soot(k, 0, 1.5, 8, 3.81, 0.25);
    if (v->atlas5m) {
        fairing(k, 32.5, 3.81, 5.4, 19.4, 7.0, WHITE);
    } else {
        seg(k, 0, 32.5, 35.5, 3.81, 3.81, WHITE, 0);   /* interstage */
        seg(k, 0, 35.5, 45.5, 3.05, 3.05, OFFWHT, 1);   /* Centaur */
        if (v->starliner) {
            seg(k, 0, 45.5, 47.0, 3.05, 5.0, WHITE, 0);   /* aeroskirt */
            seg(k, 0, 47.0, 49.4, 4.56, 4.56, WHITE, 0);
            seg(k, 0, 48.7, 49.4, 4.56, 4.56, (col_t){ 0.15, 0.3, 0.65 }, 0);
            seg(k, 0, 49.4, 52.0, 4.56, 2.4, WHITE, 0);
            nose(k, 0, 52.0, 52.9, 2.4, WHITE, 0, 0.9);
        } else {
            fairing(k, 45.5, 3.05, 4.2, 6.5, 6.0, WHITE);
        }
    }
    boosters(k, &b, 0, 1);
}

static void draw_sls(rk_t *k)
{
    boost_t b = { 2, 54.0, 3.71, 6.0, 1.6, 4.2, 4.2, WHITE, B_OGIVE };
    int s;
    if (k->lit) flame(k, 0, 0, 7.5, 34, 0);
    for (s = -1; s <= 1; s += 2) {      /* SRBs with their flared aft skirts */
        double x = s * (4.2 + 1.86);
        if (k->lit) flame(k, x, 0, 4.4, 42, 1);
        bells(k, x, 0, 1.6, 3.6, 2.6, 1, 0);
        seg(k, x, 1.6, 5.0, 5.0, 3.71, WHITE, 0);
        seg(k, x, 5.0, 48.0, 3.71, 3.71, WHITE, 0);
        seg(k, x, 30.0, 30.5, 3.71, 3.71, DKGREY, 0);
        nose(k, x, 48.0, 54.0, 3.71, WHITE, 0, 0.1);
    }
    (void)b;
    bells(k, 0, 0, 2.5, 2.4, 1.3, 2, 3.4);
    seg(k, 0, 2.5, 66.0, 8.4, 8.4, SLSORG, 0);
    seg(k, 0, 66.0, 75.0, 8.4, 5.0, OFFWHT, 0);           /* LVSA */
    seg(k, 0, 75.0, 80.5, 5.0, 5.0, WHITE, 0);            /* Orion SM panels */
    seg(k, 0, 80.5, 86.5, 5.0, 1.8, WHITE, 0);            /* LAS cover */
    tower(k, 0, 86.5, 11.5, 2.6);
}

static void draw_soyuz(rk_t *k, const veh_t *v)
{
    boost_t b = { 4, 19.6, 2.68, 0, 1.5, 1.05, 1.32, SOYUZ, B_TAPER };
    double top;
    if (k->lit) flame(k, 0, 0, 2.2, 22, 0);
    boosters(k, &b, 0, 0);
    bells(k, 0, 0, 1.5, 1.9, 1.7, 1, 0);
    seg(k, 0, 1.5, 12.0, 2.05, 2.35, SOYUZ, 0);           /* core waist */
    seg(k, 0, 12.0, 27.1, 2.35, 2.95, SOYUZ, 0);
    seg(k, 0, 21.5, 22.0, 2.75, 2.8, (col_t){ 0.82, 0.4, 0.2 }, 0); /* orange band */
    boosters(k, &b, 0, 1);
    truss(k, 0, 27.1, 28.5, 2.66);
    seg(k, 0, 28.5, 35.1, 2.66, 2.66, SOYUZ, 0);           /* Block I */
    if (v->crew) {
        top = fairing(k, 35.1, 2.66, 3.0, 6.0, 4.0, OFFWHT);
        gridfins(k, 0, 38.0, 1.2, 3.0, 0.8, DKGREY);
        tower(k, 0, top - 0.3, 5.6, 1.6);
    } else {
        fairing(k, 35.1, 2.66, 3.7, 7.4, 4.0, OFFWHT);
    }
}

static void draw_ariane6(rk_t *k, const veh_t *v)
{
    boost_t b = { v->nsrb, 14.5, 3.4, 3.0, 1.5, 2.7, 2.7, OFFWHT, B_CONE };
    if (k->lit) flame(k, 0, 0, 2.6, 22, 0);
    boosters(k, &b, 0, 0);
    bells(k, 0, 0, 3.0, 2.1, 1.1, 1, 0);
    seg(k, 0, 3.0, 33.0, 5.4, 5.4, WHITE, 0);
    seg(k, 0, 33.0, 34.4, 5.4, 5.4, DKGREY, 0);
    seg(k, 0, 34.4, 42.0, 5.4, 5.4, WHITE, 0);
    fairing(k, 42.0, 5.4, 5.4, 13.5, 6.5, WHITE);
    boosters(k, &b, 0, 1);
}

static void draw_h3(rk_t *k, const veh_t *v)
{
    boost_t b = { v->nsrb, 14.6, 2.5, 2.4, 1.2, 2.6, 2.6, WHITE, B_CONE };
    if (k->lit) flame(k, 0, 0, 3.5, 24, 0);
    boosters(k, &b, 0, 0);
    bells(k, 0, 0, 2.0, 1.8, 1.0, 2, 2.2);
    seg(k, 0, 2.0, 37.0, 5.2, 5.2, H3ORG, 0);
    seg(k, 0, 37.0, 39.0, 5.2, 5.2, OFFWHT, 0);
    seg(k, 0, 39.0, 45.0, 5.2, 5.2, H3ORG, 0);
    fairing(k, 45.0, 5.2, 5.2, v->longfair ? 11.0 : 6.0, 6.0, WHITE);
    boosters(k, &b, 0, 1);
}

/* Long March family: white (LM-5 cream) with liquid strap-ons */
static void draw_lm(rk_t *k, const veh_t *v)
{
    const char *m = v->lm;
    double d = 3.35, s1 = 27, s2 = 9, fd = 4.0, fc = 6.0, fn = 4.0;
    boost_t b = { 0, 15.3, 2.25, 3.0, 1.2, 1.675, 1.675, WHITE, B_CONE };
    col_t c = WHITE;
    int truss2 = 0, crew = 0;
    double y, top;

    if (!strcmp(m, "2F")) {
        b.n = 4; s1 = 26; s2 = 9; fd = 3.8; fc = 7.0; truss2 = 1; crew = 1;
    } else if (!strcmp(m, "3B") || !strcmp(m, "3C")) {
        b.n = m[1] == 'B' ? 4 : 2; b.len = 16; s1 = 23; s2 = 12; fd = 4.0; fc = 6; truss2 = 1;
    } else if (!strcmp(m, "5")) {
        d = 5.0; c = LMCREAM; b.n = 4; b.d = 3.35; b.len = 27.6; b.nose = 5;
        b.c = LMCREAM; s1 = 32; s2 = 11; fd = 5.2; fc = 8.0; fn = 5.5;
    } else if (!strcmp(m, "7") || !strcmp(m, "7A")) {
        b.n = 4; b.len = 27; b.nose = 4.5; s1 = 26; s2 = 11; fd = 4.2;
    } else if (!strcmp(m, "6A")) {
        b.n = 4; b.d = 2.0; b.len = 15; b.nose = 2.5; s1 = 25; s2 = 10; fd = 4.2;
    } else if (!strcmp(m, "8") || !strcmp(m, "8A")) {
        b.n = 2; b.len = 27; b.nose = 4.5; s1 = 25; s2 = 11; fd = 4.2;
    } else if (!strcmp(m, "12")) {
        d = 3.8; s1 = 33; s2 = 12; fd = 4.2; fc = 6.5; fn = 5;
    } else {
        s1 = 24; s2 = 8; fd = 3.35; truss2 = 1;
    }
    b.r_base = b.r_top = d / 2;
    if (k->lit) flame(k, 0, 0, d * 0.9, 22, 0);
    boosters(k, &b, 0, 0);
    bells(k, 0, 0, 1.5, d * 0.45, d * 0.3, 2, d * 0.45);
    seg(k, 0, 1.5, 1.5 + s1, d, d, c, 0);
    y = 1.5 + s1;
    if (truss2) { truss(k, 0, y, y + 1.4, d); y += 1.4; }
    else        { seg(k, 0, y, y + 1.0, d, d, DKGREY, 0); y += 1.0; }
    seg(k, 0, y, y + s2, d, d, c, 0);
    y += s2;
    top = fairing(k, y, d, fd, fc, fn, c);
    if (crew) {
        gridfins(k, 0, y + fc * 0.5, 1.3, fd, 0.8, DKGREY);
        tower(k, 0, top - 0.3, 6.3, 1.7);
    }
    boosters(k, &b, 0, 1);
}

/* Anything else: a white two-stage rocket; dark ones for carbon
 * vehicles (Firefly Alpha); fins so it still reads as a rocket */
static void draw_generic(rk_t *k, int dark)
{
    col_t c = dark ? CARBON : WHITE;
    if (k->lit) flame(k, 0, 0, 2.2, 14, 0);
    bells(k, 0, 0, 1.0, 1.7, 1.2, 1, 0);
    fins(k, 0, 1.0, 6.0, 2.5, 1.2, 0.0, 0.3, dark ? DKGREY : (col_t){ 0.8, 0.28, 0.22 });
    seg(k, 0, 1.0, 19.0, 2.5, 2.5, c, 0);
    seg(k, 0, 19.0, 20.0, 2.5, 2.5, dark ? DKGREY : BLACK, 0);
    seg(k, 0, 20.0, 25.0, 2.5, 2.5, c, 0);
    fairing(k, 25.0, 2.5, 2.8, 3.5, 3.5, c);
}

static void pick(veh_t *v, const char *rk, const char *ms)
{
    const char *p;
    memset(v, 0, sizeof(*v));
    v->kind = V_GENERIC;
    v->L = 32.5;
    if (has(rk, "falcon heavy")) { v->kind = V_FH; v->L = 70; }
    else if (has(rk, "falcon")) { v->kind = V_F9; v->L = 70; }
    else if (has(rk, "starship") || has(rk, "super heavy")) { v->kind = V_STARSHIP; v->L = 123; }
    else if (has(rk, "electron")) { v->kind = V_ELECTRON; v->L = 18; }
    else if (has(rk, "neutron")) { v->kind = V_NEUTRON; v->L = 43; }
    else if (has(rk, "new glenn")) { v->kind = V_NEWGLENN; v->L = 98; }
    else if (has(rk, "vulcan")) {
        v->kind = V_VULCAN;
        p = after(rk, "vc");
        v->nsrb = p && isdigit((unsigned char)p[0]) ? p[0] - '0' : 2;
        v->longfair = p && p[0] && toupper((unsigned char)p[1]) == 'L';
        v->L = v->longfair ? 67.4 : 61.6;
    } else if (has(rk, "atlas")) {
        v->kind = V_ATLAS;
        p = after(rk, "atlas v");
        while (p && *p == ' ') p++;
        if (p && isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1])) {
            v->atlas5m = p[0] == '5';
            v->nsrb = p[1] - '0';
        } else if (p && toupper((unsigned char)p[0]) == 'N') {
            v->starliner = 1; v->nsrb = 2;
        }
        if (has(ms, "starliner") || has(ms, "cft")) { v->starliner = 1; v->atlas5m = 0; v->nsrb = 2; }
        v->L = v->atlas5m ? 59.4 : (v->starliner ? 52.9 : 58.5);
    } else if (has(rk, "sls") || has(rk, "space launch system")) { v->kind = V_SLS; v->L = 98; }
    else if (has(rk, "soyuz")) {
        v->kind = V_SOYUZ;
        v->crew = has(ms, "soyuz ms") || has(ms, "soyuz-ms");
        v->L = v->crew ? 49.6 : 46.6;
    } else if (has(rk, "ariane 6")) {
        v->kind = V_ARIANE6;
        p = after(rk, "ariane 6");
        v->nsrb = p && *p == '4' ? 4 : 2;
        v->L = 62;
    } else if (has(rk, "h3")) {
        v->kind = V_H3;
        p = after(rk, "h3-");
        v->nsrb = p && isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) ? p[1] - '0' : 2;
        v->longfair = p && p[0] && p[1] && toupper((unsigned char)p[2]) == 'L';
        v->L = v->longfair ? 62 : 57;
    } else if ((p = after(rk, "long march ")) != NULL || (p = after(rk, "cz-")) != NULL) {
        size_t i = 0;
        v->kind = V_LM;
        while (p[i] && p[i] != ' ' && p[i] != '/' && i < sizeof(v->lm) - 1) {
            v->lm[i] = (char)toupper((unsigned char)p[i]);
            i++;
        }
        if (!strcmp(v->lm, "2F")) v->L = 58.3;
        else if (!strcmp(v->lm, "3B") || !strcmp(v->lm, "3C")) v->L = 56.3;
        else if (!strcmp(v->lm, "5")) v->L = 59.3;
        else if (!strcmp(v->lm, "7") || !strcmp(v->lm, "7A") || !strcmp(v->lm, "6A") ||
                 !strcmp(v->lm, "8") || !strcmp(v->lm, "8A")) v->L = 52.5;
        else if (!strcmp(v->lm, "12")) v->L = 59.5;
        else v->L = 42.5;
    } else if (has(rk, "alpha") || has(rk, "firefly")) {
        v->kind = V_DARK;
    }
    /* Dragon on top of a Falcon 9: crew and cargo flights to orbit */
    if (v->kind == V_F9 && ms && !has(ms, "cygnus") && !has(ms, "ng-") &&
        (has(ms, "crew") || has(ms, "crs-") || has(ms, "spx-") || has(ms, "ax-") ||
         has(ms, "dragon") || has(ms, "vast") || has(ms, "fram") || has(ms, "polaris"))) {
        v->dragon = has(ms, "crs-") || has(ms, "spx-") || has(ms, "cargo") ? 1 : 2;
        v->L = 64.9;
    }
}

int pic_draw_launch_icon(cairo_t *cr, const char *rocket, const char *mission,
                         double x, double y, double H, int lit)
{
    veh_t v;
    rk_t k;
    double size, scale, Hpx;

    pick(&v, rocket, mission);
    /* Same height for all, nudged by real size (Electron ~85%,
     * Starship ~110% of a Falcon 9) */
    size = 0.8 + 0.2 * v.L / 70.0;
    if (size < 0.84) size = 0.84;
    if (size > 1.1) size = 1.1;
    Hpx = H * size;
    scale = Hpx / v.L;

    k.cr = cr;
    k.lit = lit;
    k.dark = v.kind == V_ELECTRON || v.kind == V_NEUTRON || v.kind == V_DARK;
    k.detail = Hpx > 110;
    k.lw = fmax(0.5, Hpx / 110.0) / scale;

    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, scale, -scale);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    switch (v.kind) {
    case V_F9:       draw_f9(&k, &v); break;
    case V_FH:       draw_fh(&k, &v); break;
    case V_STARSHIP: draw_starship(&k); break;
    case V_ELECTRON: draw_electron(&k); break;
    case V_NEUTRON:  draw_neutron(&k); break;
    case V_NEWGLENN: draw_newglenn(&k); break;
    case V_VULCAN:   draw_vulcan(&k, &v); break;
    case V_ATLAS:    draw_atlas(&k, &v); break;
    case V_SLS:      draw_sls(&k); break;
    case V_SOYUZ:    draw_soyuz(&k, &v); break;
    case V_ARIANE6:  draw_ariane6(&k, &v); break;
    case V_H3:       draw_h3(&k, &v); break;
    case V_LM:       draw_lm(&k, &v); break;
    case V_DARK:     draw_generic(&k, 1); break;
    default:         draw_generic(&k, 0); break;
    }
    cairo_restore(cr);
    cairo_new_path(cr);
    return v.kind != V_GENERIC;
}

int pic_draw_rocket_icon(cairo_t *cr, const char *name, double x, double y,
                         double H, int lit)
{
    return pic_draw_launch_icon(cr, name, NULL, x, y, H, lit);
}
