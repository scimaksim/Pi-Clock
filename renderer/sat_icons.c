/*
 * sat_icons.c - Small vector silhouettes for well-known spacecraft
 *
 * pic_draw_sat_icon() draws a recognisable outline for the ISS, the
 * Tiangong station and the Hubble Space Telescope, and a generic
 * box-and-panels satellite for anything else. Shapes are simplified
 * so they still read at 15-30 px; a dark edge keeps them visible on
 * both day and night map tiles.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sat_icons.h"
#include <ctype.h>
#include <math.h>
#include <string.h>

static int name_is(const char *name, const char *want)
{
    size_t n = strlen(want), i;
    if (!name) return 0;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)name[i]) != want[i]) return 0;
    /* whole word: "ISS" but not "ISSUE" */
    return !isalnum((unsigned char)name[n]);
}

pic_sat_kind_t pic_sat_kind(const char *name)
{
    if (name_is(name, "iss") || name_is(name, "zarya"))
        return PIC_SAT_ISS;
    if (name_is(name, "tiangong") || name_is(name, "css") ||
        name_is(name, "tianhe"))
        return PIC_SAT_TIANGONG;
    if (name_is(name, "hubble") || name_is(name, "hst"))
        return PIC_SAT_HUBBLE;
    return PIC_SAT_GENERIC;
}

/* Filled rectangle with a thin dark edge */
static void box(cairo_t *cr, double x, double y, double w, double h,
                double r, double g, double b, double edge)
{
    cairo_rectangle(cr, x, y, w, h);
    cairo_set_source_rgba(cr, r, g, b, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.05, 0.06, 0.1, 0.8);
    cairo_set_line_width(cr, edge);
    cairo_stroke(cr);
}

/* Solar array: panel with one centre seam */
static void panel(cairo_t *cr, double x, double y, double w, double h,
                  double r, double g, double b, double edge)
{
    box(cr, x, y, w, h, r, g, b, edge);
    cairo_set_source_rgba(cr, 0.05, 0.06, 0.1, 0.45);
    cairo_set_line_width(cr, edge * 0.7);
    if (w > h) {
        cairo_move_to(cr, x + w / 2, y);
        cairo_line_to(cr, x + w / 2, y + h);
    } else {
        cairo_move_to(cr, x, y + h / 2);
        cairo_line_to(cr, x + w, y + h / 2);
    }
    cairo_stroke(cr);
}

/*
 * ISS: long horizontal truss carrying four pairs of gold arrays, with
 * the pressurised modules hanging across its middle.
 */
static void draw_iss(cairo_t *cr, double s, double e)
{
    static const double ax[4] = { -3.55, -2.65, 1.85, 2.75 };
    const double gr = 0.92, gg = 0.72, gb = 0.3;   /* array gold */
    int i;

    for (i = 0; i < 4; i++) {
        panel(cr, ax[i] * s, -2.0 * s, 0.8 * s, 1.75 * s, gr, gg, gb, e);
        panel(cr, ax[i] * s, 0.25 * s, 0.8 * s, 1.75 * s, gr, gg, gb, e);
    }
    /* Truss */
    box(cr, -3.9 * s, -0.22 * s, 7.8 * s, 0.44 * s, 0.82, 0.84, 0.88, e);
    /* Modules: a vertical stack and a short cross piece */
    box(cr, -0.38 * s, -1.6 * s, 0.76 * s, 3.4 * s, 0.94, 0.95, 0.97, e);
    box(cr, -1.1 * s, 0.75 * s, 2.2 * s, 0.6 * s, 0.94, 0.95, 0.97, e);
    /* Radiators */
    box(cr, -1.0 * s, -1.25 * s, 0.6 * s, 0.35 * s, 0.98, 0.98, 1.0, e * 0.7);
    box(cr, 0.4 * s, -1.25 * s, 0.6 * s, 0.35 * s, 0.98, 0.98, 1.0, e * 0.7);
}

/*
 * Tiangong: T-shaped station. Core module along the stem, the two
 * lab modules forming the bar, and a very long blue wing on each lab.
 */
static void draw_tiangong(cairo_t *cr, double s, double e)
{
    const double br = 0.3, bg = 0.45, bb = 0.9;    /* array blue */

    /* Lab wings (long, thin) at the ends of the bar */
    panel(cr, -4.0 * s, -1.75 * s, 3.4 * s, 0.75 * s, br, bg, bb, e);
    panel(cr, 0.6 * s, -1.75 * s, 3.4 * s, 0.75 * s, br, bg, bb, e);
    /* Bar: the two labs */
    box(cr, -2.4 * s, -1.0 * s, 4.8 * s, 0.75 * s, 0.94, 0.95, 0.97, e);
    /* Stem: core module, with its own shorter wings */
    panel(cr, -1.9 * s, 0.6 * s, 1.5 * s, 0.6 * s, br, bg, bb, e);
    panel(cr, 0.4 * s, 0.6 * s, 1.5 * s, 0.6 * s, br, bg, bb, e);
    box(cr, -0.4 * s, -0.4 * s, 0.8 * s, 2.9 * s, 0.94, 0.95, 0.97, e);
    /* Docked crew ship at the bottom of the stem */
    box(cr, -0.3 * s, 2.5 * s, 0.6 * s, 0.9 * s, 0.85, 0.87, 0.9, e);
}

/*
 * Hubble: a silver tube with the aperture door open at one end and two
 * blue arrays along its sides.
 */
static void draw_hubble(cairo_t *cr, double s, double e)
{
    const double br = 0.32, bg = 0.42, bb = 0.85;

    panel(cr, -2.2 * s, -2.1 * s, 3.8 * s, 0.75 * s, br, bg, bb, e);
    panel(cr, -2.2 * s, 1.35 * s, 3.8 * s, 0.75 * s, br, bg, bb, e);
    /* Array booms */
    cairo_set_source_rgba(cr, 0.8, 0.82, 0.86, 0.9);
    cairo_set_line_width(cr, e * 1.2);
    cairo_move_to(cr, -0.3 * s, -1.35 * s); cairo_line_to(cr, -0.3 * s, 1.35 * s);
    cairo_stroke(cr);
    /* Tube (aft section slightly wider) */
    box(cr, -3.2 * s, -1.0 * s, 2.0 * s, 2.0 * s, 0.82, 0.84, 0.88, e);
    box(cr, -1.2 * s, -0.85 * s, 4.0 * s, 1.7 * s, 0.93, 0.94, 0.96, e);
    /* Aperture door, hinged open */
    cairo_new_path(cr);
    cairo_move_to(cr, 2.8 * s, -0.85 * s);
    cairo_line_to(cr, 3.9 * s, -1.6 * s);
    cairo_set_source_rgba(cr, 0.9, 0.9, 0.92, 0.95);
    cairo_set_line_width(cr, e * 2.2);
    cairo_stroke(cr);
    /* Dark opening */
    box(cr, 2.65 * s, -0.7 * s, 0.25 * s, 1.4 * s, 0.1, 0.12, 0.18, e * 0.6);
}

static void draw_generic(cairo_t *cr, double s, double e,
                         double r, double g, double b)
{
    panel(cr, -3.0 * s, -0.6 * s, 1.8 * s, 1.2 * s, 0.25, 0.35, 0.75, e);
    panel(cr, 1.2 * s, -0.6 * s, 1.8 * s, 1.2 * s, 0.25, 0.35, 0.75, e);
    box(cr, -1.0 * s, -0.7 * s, 2.0 * s, 1.4 * s, r, g, b, e);
    cairo_move_to(cr, 0, -0.7 * s);
    cairo_line_to(cr, 0, -1.4 * s);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
    cairo_set_line_width(cr, e * 1.3);
    cairo_stroke(cr);
}

void pic_draw_sat_icon(cairo_t *cr, const char *name, double x, double y,
                       double size, double angle, double r, double g, double b)
{
    /* size = full icon width; the shapes span about 8 units */
    double s = size / 8.0, e = size / 40.0;

    if (e < 0.6) e = 0.6;
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_translate(cr, x, y);
    cairo_rotate(cr, angle);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    switch (pic_sat_kind(name)) {
    case PIC_SAT_ISS:      draw_iss(cr, s, e); break;
    case PIC_SAT_TIANGONG: draw_tiangong(cr, s, e); break;
    case PIC_SAT_HUBBLE:   draw_hubble(cr, s, e); break;
    default:               draw_generic(cr, s * 1.25, e, r, g, b); break;
    }
    cairo_restore(cr);
    cairo_new_path(cr);
}

/* ── Launch vehicles ─────────────────────────────────────────── */

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

typedef struct {
    double core_w;           /* fraction of total height             */
    double core[3];          /* first-stage colour                   */
    double upper[3];         /* upper stage / fairing colour         */
    double band;             /* dark interstage band height (0 none) */
    double split;            /* where the upper stage starts (0..1)  */
    double fair_w;           /* fairing width / core width           */
    int    boosters;         /* 0, 2, or 4 (4 = Soyuz-style tapered) */
    double boost_h;          /* booster height fraction              */
    double boost[3];
    int    flaps;            /* Starship-style flaps                 */
} rocket_style_t;

static void rr_fill(cairo_t *cr, double r, double g, double b, double e)
{
    cairo_set_source_rgba(cr, r, g, b, 0.97);
    cairo_fill_preserve(cr);
    /* Dark vehicles (Electron) get a light edge so they show at night */
    if (0.3 * r + 0.59 * g + 0.11 * b < 0.3)
        cairo_set_source_rgba(cr, 0.75, 0.77, 0.82, 0.9);
    else
        cairo_set_source_rgba(cr, 0.03, 0.04, 0.08, 0.85);
    cairo_set_line_width(cr, e);
    cairo_stroke(cr);
}

/* Upright body from y0 (base) to y1 (top) with a pointed nose */
static void body(cairo_t *cr, double cx, double w, double y0, double y1,
                 double nose, const double *c, double e)
{
    cairo_new_path(cr);
    cairo_move_to(cr, cx - w / 2, y0);
    cairo_line_to(cr, cx - w / 2, y1 + nose);
    cairo_curve_to(cr, cx - w / 2, y1 + nose * 0.4, cx - w * 0.15, y1, cx, y1);
    cairo_curve_to(cr, cx + w * 0.15, y1, cx + w / 2, y1 + nose * 0.4, cx + w / 2, y1 + nose);
    cairo_line_to(cr, cx + w / 2, y0);
    cairo_close_path(cr);
    rr_fill(cr, c[0], c[1], c[2], e);
}

static void band(cairo_t *cr, double cx, double w, double y, double h, double e)
{
    cairo_new_path(cr);
    cairo_rectangle(cr, cx - w / 2, y, w, h);
    cairo_set_source_rgba(cr, 0.08, 0.08, 0.1, 0.95);
    cairo_fill(cr);
    (void)e;
}

static void draw_stack(cairo_t *cr, double x, double y, double H,
                       const rocket_style_t *st, double e)
{
    double w = H * st->core_w, top = y - H, sp = y - H * st->split;
    double fw = w * (st->fair_w > 0 ? st->fair_w : 1.0);
    int i;

    /* Boosters behind the core */
    if (st->boosters == 2 || st->boosters == 4) {
        double bw = w * (st->boosters == 4 ? 0.75 : 0.55), bh = H * st->boost_h;
        for (i = -1; i <= 1; i += 2) {
            double bx = x + i * (w / 2 + bw / 2 - (st->boosters == 4 ? bw * 0.25 : 0));
            if (st->boosters == 4) {     /* tapered cone boosters */
                cairo_new_path(cr);
                cairo_move_to(cr, bx - bw / 2, y);
                cairo_line_to(cr, bx + bw / 2, y);
                cairo_line_to(cr, x + i * w * 0.35, y - bh);
                cairo_line_to(cr, x + i * w * 0.15, y - bh * 0.9);
                cairo_close_path(cr);
                rr_fill(cr, st->boost[0], st->boost[1], st->boost[2], e);
            } else {
                body(cr, bx, bw, y, y - bh, bw * 1.2, st->boost, e);
            }
        }
    }
    /* First stage */
    body(cr, x, w, y, sp, 0, st->core, e);
    /* Upper stage + fairing */
    body(cr, x, w, sp, sp - (sp - top) * 0.45, 0, st->upper, e);
    body(cr, x, fw, sp - (sp - top) * 0.45, top, fw * 1.4, st->upper, e);
    if (st->band > 0)
        band(cr, x, w, sp - H * st->band / 2, H * st->band, e);
    if (st->flaps) {
        for (i = -1; i <= 1; i += 2) {
            cairo_new_path(cr);
            cairo_move_to(cr, x + i * w / 2, top + H * 0.1);
            cairo_line_to(cr, x + i * w * 0.85, top + H * 0.13);
            cairo_line_to(cr, x + i * w / 2, top + H * 0.17);
            cairo_close_path(cr);
            rr_fill(cr, 0.15, 0.15, 0.17, e * 0.6);
            cairo_new_path(cr);
            cairo_move_to(cr, x + i * w / 2, sp - H * 0.03);
            cairo_line_to(cr, x + i * w * 0.9, sp - H * 0.01);
            cairo_line_to(cr, x + i * w / 2, sp - H * 0.1);
            cairo_close_path(cr);
            rr_fill(cr, 0.15, 0.15, 0.17, e * 0.6);
        }
    }
}

static void flame(cairo_t *cr, double x, double y, double w)
{
    cairo_new_path(cr);
    cairo_move_to(cr, x - w * 0.4, y);
    cairo_curve_to(cr, x - w * 0.3, y + w * 1.2, x, y + w * 1.8, x, y + w * 2.2);
    cairo_curve_to(cr, x, y + w * 1.8, x + w * 0.3, y + w * 1.2, x + w * 0.4, y);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 1.0, 0.62, 0.15, 0.95);
    cairo_fill(cr);
}

int pic_draw_rocket_icon(cairo_t *cr, const char *name, double x, double y,
                         double H, int lit)
{
    static const double WHITE[3] = { 0.95, 0.95, 0.97 };
    rocket_style_t st;
    double e = H / 45.0;
    int known = 1;

    if (e < 0.6) e = 0.6;
    memset(&st, 0, sizeof(st));
    st.core_w = 0.1; st.split = 0.62; st.fair_w = 1.0;
    memcpy(st.core, WHITE, sizeof(WHITE));
    memcpy(st.upper, WHITE, sizeof(WHITE));
    memcpy(st.boost, WHITE, sizeof(WHITE));

    if (has(name, "falcon heavy")) {
        double w;
        st.core_w = 0.085; st.band = 0.05; st.fair_w = 1.25;
        w = H * st.core_w;
        cairo_save(cr);
        body(cr, x - w, w, y, y - H * 0.66, w * 1.6, WHITE, e);
        body(cr, x + w, w, y, y - H * 0.66, w * 1.6, WHITE, e);
        band(cr, x - w, w, y - H * 0.64, H * 0.04, e);
        band(cr, x + w, w, y - H * 0.64, H * 0.04, e);
        draw_stack(cr, x, y, H, &st, e);
        if (lit) { flame(cr, x - w, y, w); flame(cr, x, y, w); flame(cr, x + w, y, w); }
        cairo_restore(cr);
        return 1;
    } else if (has(name, "falcon")) {
        st.core_w = 0.085; st.band = 0.05; st.fair_w = 1.3;
    } else if (has(name, "starship") || has(name, "super heavy")) {
        static const double STEEL[3] = { 0.78, 0.8, 0.84 };
        st.core_w = 0.15; st.split = 0.55; st.band = 0.02; st.flaps = 1;
        memcpy(st.core, STEEL, sizeof(STEEL));
        memcpy(st.upper, STEEL, sizeof(STEEL));
    } else if (has(name, "electron")) {
        static const double BLACK[3] = { 0.16, 0.16, 0.18 };
        st.core_w = 0.1; st.fair_w = 1.15;
        memcpy(st.core, BLACK, sizeof(BLACK));
        memcpy(st.upper, BLACK, sizeof(BLACK));
    } else if (has(name, "new glenn")) {
        st.core_w = 0.12; st.split = 0.6; st.fair_w = 1.0;
    } else if (has(name, "vulcan") || has(name, "atlas")) {
        static const double TAN[3] = { 0.86, 0.55, 0.25 };
        st.core_w = 0.12; st.split = 0.66; st.fair_w = 1.15;
        st.boosters = 2; st.boost_h = 0.38;
        memcpy(st.core, TAN, sizeof(TAN));
    } else if (has(name, "sls") || has(name, "space launch system")) {
        static const double ORANGE[3] = { 0.9, 0.45, 0.15 };
        st.core_w = 0.13; st.split = 0.7; st.fair_w = 0.8;
        st.boosters = 2; st.boost_h = 0.66;
        memcpy(st.core, ORANGE, sizeof(ORANGE));
    } else if (has(name, "soyuz")) {
        static const double GREY[3] = { 0.62, 0.66, 0.58 };
        st.core_w = 0.11; st.split = 0.6; st.fair_w = 1.2;
        st.boosters = 4; st.boost_h = 0.38;
        memcpy(st.core, GREY, sizeof(GREY));
        memcpy(st.boost, GREY, sizeof(GREY));
    } else if (has(name, "ariane") || has(name, "h3") || has(name, "h-ii") ||
               has(name, "long march") || has(name, "gslv") ||
               has(name, "pslv") || has(name, "zhuque") || has(name, "kinetica")) {
        st.core_w = 0.11; st.split = 0.62; st.fair_w = 1.2;
        st.boosters = 2; st.boost_h = 0.42;
    } else {
        known = 0;
        st.core_w = 0.13; st.split = 0.6; st.fair_w = 1.0;
    }
    cairo_save(cr);
    draw_stack(cr, x, y, H, &st, e);
    if (!known) {      /* generic: add fins so it still reads as a rocket */
        double w = H * st.core_w;
        int i;
        for (i = -1; i <= 1; i += 2) {
            cairo_new_path(cr);
            cairo_move_to(cr, x + i * w / 2, y - H * 0.2);
            cairo_line_to(cr, x + i * w * 1.2, y);
            cairo_line_to(cr, x + i * w / 2, y);
            cairo_close_path(cr);
            rr_fill(cr, 0.85, 0.3, 0.25, e);
        }
    }
    if (lit) flame(cr, x, y, H * st.core_w);
    cairo_restore(cr);
    return known;
}
