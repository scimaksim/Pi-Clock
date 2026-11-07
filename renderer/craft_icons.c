/*
 * craft_icons.c - Landers, rovers and orbiters of the other worlds
 *
 * Small side-view drawings (surface craft) and plan views (orbiters),
 * each about 20 units across (u = size / 20), drawn from the real
 * vehicles' layouts: Apollo's gold-foil descent stage on four legs,
 * Lunokhod's tub with its hinged solar lid and eight wheels, Viking's
 * hexagonal body on three legs, Curiosity/Perseverance with mast, arm
 * and RTG, InSight/Phoenix with their round arrays, Juno's three long
 * wings, MRO's 3 m dish between two big panels, and so on.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "craft_icons.h"
#include <math.h>
#include <stddef.h>

static double U, DIM;   /* unit, greying for ended missions */

/* colours, greyed by DIM */
static void rgba(cairo_t *cr, double r, double g, double b, double a)
{
    double y = 0.3 * r + 0.59 * g + 0.11 * b;
    r += (y * 0.8 - r) * DIM;
    g += (y * 0.8 - g) * DIM;
    b += (y * 0.8 - b) * DIM;
    cairo_set_source_rgba(cr, r, g, b, a);
}

static void outline(cairo_t *cr)
{
    cairo_set_source_rgba(cr, 0.05, 0.05, 0.07, 0.9);
    cairo_set_line_width(cr, U * 0.35);
    cairo_stroke(cr);
}

static void fill_out(cairo_t *cr)
{
    cairo_fill_preserve(cr);
    outline(cr);
}

/* gold foil (MLI): warm gradient */
static void foil(cairo_t *cr, double x, double y, double w, double h)
{
    cairo_pattern_t *p = cairo_pattern_create_linear(x, y, x + w, y + h);
    double g = 1 - DIM * 0.6;
    cairo_pattern_add_color_stop_rgb(p, 0, 0.95 * g + 0.3 * DIM, 0.78 * g + 0.3 * DIM, 0.32 * g + 0.4 * DIM);
    cairo_pattern_add_color_stop_rgb(p, 0.5, 1.0 * g + 0.3 * DIM, 0.9 * g + 0.3 * DIM, 0.55 * g + 0.4 * DIM);
    cairo_pattern_add_color_stop_rgb(p, 1, 0.72 * g + 0.25 * DIM, 0.55 * g + 0.25 * DIM, 0.2 * g + 0.35 * DIM);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, x, y, w, h);
    cairo_pattern_destroy(p);
    fill_out(cr);
    if (w > 0 && h > 0) {   /* crinkled MLI catching the light */
        int i;
        cairo_save(cr);
        cairo_rectangle(cr, x, y, w, h);
        cairo_clip(cr);
        cairo_set_line_width(cr, U * 0.25);
        for (i = 0; i < 5; i++) {
            double fx = x + w * (0.12 + 0.19 * i), fy = y + h * (0.2 + 0.13 * (i % 3));
            cairo_move_to(cr, fx, fy);
            cairo_curve_to(cr, fx + w * 0.05, fy + h * 0.25, fx - w * 0.04, fy + h * 0.4, fx + w * 0.03, fy + h * 0.7);
        }
        cairo_set_source_rgba(cr, 1, 0.97, 0.8, 0.35 * (1 - DIM));
        cairo_stroke(cr);
        cairo_restore(cr);
    }
}

/* shaded fill: lit from the upper left, as in the rendered sprites */
static void shade(cairo_t *cr, double x, double y, double w, double h, double r, double g, double b)
{
    cairo_pattern_t *p = cairo_pattern_create_linear(x, y, x + w * 0.6, y + h);
    double k = 1 - DIM * 0.5, yy = 0.3 * r + 0.59 * g + 0.11 * b;
    r += (yy * 0.8 - r) * DIM; g += (yy * 0.8 - g) * DIM; b += (yy * 0.8 - b) * DIM;
    cairo_pattern_add_color_stop_rgb(p, 0, fmin(1, r * 1.25 * k + 0.08), fmin(1, g * 1.25 * k + 0.08), fmin(1, b * 1.25 * k + 0.08));
    cairo_pattern_add_color_stop_rgb(p, 0.45, r * k, g * k, b * k);
    cairo_pattern_add_color_stop_rgb(p, 1, r * 0.55 * k, g * 0.55 * k, b * 0.55 * k);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    outline(cr);
}

static void box(cairo_t *cr, double x, double y, double w, double h, double r, double g, double b)
{
    cairo_rectangle(cr, x, y, w, h);
    shade(cr, x, y, w, h, r, g, b);
}

/* solar array: dark blue with cell lines */
static void panel(cairo_t *cr, double x, double y, double w, double h, int nx)
{
    int i;
    cairo_rectangle(cr, x, y, w, h);
    {
        cairo_pattern_t *p = cairo_pattern_create_linear(x, y, x + w, y + h);
        cairo_pattern_add_color_stop_rgb(p, 0, 0.16 + 0.2 * DIM, 0.3 + 0.1 * DIM, 0.68 - 0.2 * DIM);
        cairo_pattern_add_color_stop_rgb(p, 0.35, 0.32 + 0.1 * DIM, 0.46, 0.85 - 0.3 * DIM);   /* sheen */
        cairo_pattern_add_color_stop_rgb(p, 0.5, 0.1 + 0.2 * DIM, 0.18 + 0.1 * DIM, 0.45 - 0.1 * DIM);
        cairo_pattern_add_color_stop_rgb(p, 1, 0.06 + 0.2 * DIM, 0.12 + 0.1 * DIM, 0.32);
        cairo_set_source(cr, p);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(p);
        outline(cr);
    }
    rgba(cr, 0.55, 0.65, 0.9, 0.55);
    cairo_set_line_width(cr, U * 0.18);
    for (i = 1; i < nx; i++) {
        cairo_move_to(cr, x + w * i / nx, y);
        cairo_line_to(cr, x + w * i / nx, y + h);
    }
    cairo_stroke(cr);
}

static void line(cairo_t *cr, double x0, double y0, double x1, double y1, double w)
{
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x1, y1);
    rgba(cr, 0.75, 0.75, 0.78, 1);
    cairo_set_line_width(cr, w);
    cairo_stroke(cr);
}

static void ball(cairo_t *cr, double x, double y, double r, double cr_, double cg, double cb)
{
    cairo_pattern_t *p = cairo_pattern_create_radial(x - r * 0.35, y - r * 0.35, r * 0.1, x, y, r * 1.1);
    double k = 1 - DIM * 0.4;
    cairo_pattern_add_color_stop_rgb(p, 0, fmin(1, cr_ * 1.35 * k), fmin(1, cg * 1.35 * k), fmin(1, cb * 1.35 * k));
    cairo_pattern_add_color_stop_rgb(p, 1, cr_ * 0.45 * k, cg * 0.45 * k, cb * 0.45 * k);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    outline(cr);
}

static void wheel(cairo_t *cr, double x, double y, double r)
{
    cairo_arc(cr, x, y, r, 0, 2 * M_PI);
    ball(cr, x, y, r, 0.6, 0.6, 0.63);
    cairo_arc(cr, x, y, r * 0.35, 0, 2 * M_PI);
    rgba(cr, 0.25, 0.25, 0.28, 1);
    cairo_fill(cr);
}

/* landing leg from (x0,y0) out to a footpad at (x1, y1) */
static void leg(cairo_t *cr, double x0, double y0, double x1, double y1)
{
    line(cr, x0, y0, x1, y1, U * 0.7);
    cairo_save(cr);
    cairo_translate(cr, x1, y1);
    cairo_scale(cr, 1.0, 0.45);
    cairo_arc(cr, 0, 0, U * 1.1, 0, 2 * M_PI);
    cairo_restore(cr);
    rgba(cr, 0.8, 0.8, 0.82, 1);
    fill_out(cr);
}

static void dish(cairo_t *cr, double x, double y, double r, double ang)
{
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_rotate(cr, ang);
    cairo_scale(cr, 1.0, 0.45);
    cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
    cairo_restore(cr);
    rgba(cr, 0.93, 0.93, 0.95, 1);
    fill_out(cr);
}

static void ground(cairo_t *cr, double w)
{
    cairo_pattern_t *p = cairo_pattern_create_radial(0, 8 * U, 0, 0, 8 * U, w);
    cairo_pattern_add_color_stop_rgba(p, 0, 0, 0, 0, 0.45);
    cairo_pattern_add_color_stop_rgba(p, 1, 0, 0, 0, 0);
    cairo_save(cr);
    cairo_translate(cr, 0, 8 * U);
    cairo_scale(cr, 1, 0.18);
    cairo_translate(cr, 0, -8 * U);
    cairo_arc(cr, 0, 8 * U, w, 0, 2 * M_PI);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_pattern_destroy(p);
}

/* ---------------------------------------------------------------- */
/* Moon surface                                                      */

static void apollo_lm(cairo_t *cr)
{
    /* octagonal descent stage in gold foil, four splayed legs, the
     * ascent stage gone (it left from the top) */
    leg(cr, -4 * U, 1 * U, -9 * U, 7.5 * U);
    leg(cr, 4 * U, 1 * U, 9 * U, 7.5 * U);
    leg(cr, -1.5 * U, 2 * U, -3 * U, 8 * U);
    leg(cr, 1.5 * U, 2 * U, 3 * U, 8 * U);
    foil(cr, -5.5 * U, -2.5 * U, 11 * U, 5 * U);
    /* engine bell under */
    cairo_move_to(cr, -1.5 * U, 2.5 * U);
    cairo_line_to(cr, 1.5 * U, 2.5 * U);
    cairo_line_to(cr, 2.2 * U, 4.2 * U);
    cairo_line_to(cr, -2.2 * U, 4.2 * U);
    cairo_close_path(cr);
    rgba(cr, 0.3, 0.3, 0.32, 1);
    fill_out(cr);
    /* black panel band */
    box(cr, -5.5 * U, -0.6 * U, 11 * U, 1.2 * U, 0.12, 0.12, 0.14);
}

static void lrv(cairo_t *cr, double ox)
{
    cairo_save(cr);
    cairo_translate(cr, ox, 0);
    box(cr, -4 * U, 4.2 * U, 8 * U, 1.3 * U, 0.85, 0.85, 0.82);
    wheel(cr, -3 * U, 6.7 * U, 1.4 * U);
    wheel(cr, 3 * U, 6.7 * U, 1.4 * U);
    line(cr, 2.5 * U, 4.2 * U, 3.5 * U, 1.8 * U, U * 0.4);   /* high-gain mast */
    dish(cr, 3.6 * U, 1.4 * U, 1.3 * U, -0.3);
    cairo_restore(cr);
}

static void lunokhod(cairo_t *cr)
{
    int i;
    /* tub on eight wheels, convex lid hinged open behind (solar cells
     * inside), cameras and a helical antenna in front */
    for (i = 0; i < 4; i++) wheel(cr, (-6 + 4 * i) * U, 6.3 * U, 1.6 * U);
    cairo_move_to(cr, -7 * U, 0);
    cairo_line_to(cr, 7 * U, 0);
    cairo_line_to(cr, 6 * U, 4.6 * U);
    cairo_line_to(cr, -6 * U, 4.6 * U);
    cairo_close_path(cr);
    rgba(cr, 0.82, 0.82, 0.8, 1);
    fill_out(cr);
    /* lid open, tilted back */
    cairo_save(cr);
    cairo_translate(cr, -6.8 * U, 0);
    cairo_rotate(cr, -1.05);
    cairo_rectangle(cr, -10 * U, -1 * U, 10 * U, 1.2 * U);
    cairo_restore(cr);
    rgba(cr, 0.12, 0.22, 0.55, 1);
    fill_out(cr);
    line(cr, 5 * U, 0, 6.5 * U, -5 * U, U * 0.5);             /* antenna */
    cairo_arc(cr, 6.5 * U, -5.4 * U, U * 0.8, 0, 2 * M_PI);
    rgba(cr, 0.85, 0.85, 0.85, 1);
    fill_out(cr);
    box(cr, 6 * U, 1 * U, 1.6 * U, 1.5 * U, 0.2, 0.2, 0.22);  /* camera ports */
}

static void luna_sr(cairo_t *cr)
{
    /* descent stage with spherical tanks on four legs, ascent rocket
     * and the small return capsule (sphere) on top */
    leg(cr, -3 * U, 4 * U, -7.5 * U, 7.5 * U);
    leg(cr, 3 * U, 4 * U, 7.5 * U, 7.5 * U);
    box(cr, -5 * U, 2.5 * U, 10 * U, 2.5 * U, 0.75, 0.76, 0.74);
    cairo_arc(cr, -2.5 * U, 1.5 * U, 1.8 * U, 0, 2 * M_PI);
    ball(cr, -2.5 * U, 1.5 * U, 1.8 * U, 0.85, 0.85, 0.85);
    cairo_arc(cr, 2.5 * U, 1.5 * U, 1.8 * U, 0, 2 * M_PI);
    ball(cr, 2.5 * U, 1.5 * U, 1.8 * U, 0.85, 0.85, 0.85);
    box(cr, -1.2 * U, -4.5 * U, 2.4 * U, 6 * U, 0.6, 0.62, 0.6);   /* ascent stage */
    cairo_arc(cr, 0, -6 * U, 1.6 * U, 0, 2 * M_PI);                 /* capsule */
    rgba(cr, 0.55, 0.4, 0.25, 1);
    fill_out(cr);
    line(cr, -4 * U, 2.5 * U, -6.5 * U, -5 * U, U * 0.4);          /* sampling arm */
}

static void luna_als(cairo_t *cr)
{
    int i;
    /* the 58 cm sphere with four petals folded open on the ground */
    for (i = -1; i <= 1; i += 2) {
        cairo_move_to(cr, i * 2 * U, 6 * U);
        cairo_line_to(cr, i * 9 * U, 7.5 * U);
        cairo_line_to(cr, i * 8 * U, 8.3 * U);
        cairo_line_to(cr, i * 1.5 * U, 7.5 * U);
        cairo_close_path(cr);
        rgba(cr, 0.75, 0.75, 0.72, 1);
        fill_out(cr);
    }
    cairo_arc(cr, 0, 3.5 * U, 4 * U, 0, 2 * M_PI);
    ball(cr, 0, 3.5 * U, 4 * U, 0.85, 0.85, 0.82);
    for (i = -1; i <= 1; i++)
        line(cr, i * 1.5 * U, 0, i * 3 * U, -5 * U, U * 0.35);     /* antennas */
}

static void surveyor(cairo_t *cr)
{
    /* triangular frame on three legs, mast with flat solar panel and
     * planar antenna, the scoop arm */
    leg(cr, -2.5 * U, 3 * U, -8.5 * U, 7.5 * U);
    leg(cr, 2.5 * U, 3 * U, 8.5 * U, 7.5 * U);
    leg(cr, 0, 3.5 * U, 0, 8 * U);
    cairo_move_to(cr, -4 * U, 3.5 * U);
    cairo_line_to(cr, 4 * U, 3.5 * U);
    cairo_line_to(cr, 0, -1 * U);
    cairo_close_path(cr);
    rgba(cr, 0.85, 0.85, 0.85, 1);
    fill_out(cr);
    box(cr, -2 * U, 1 * U, 4 * U, 2 * U, 0.95, 0.95, 0.95);       /* thermal boxes */
    line(cr, 0, -1 * U, 0, -7 * U, U * 0.5);                      /* mast */
    panel(cr, -5 * U, -8.5 * U, 5 * U, 2 * U, 3);
    box(cr, 0.3 * U, -8.5 * U, 4 * U, 2.6 * U, 0.85, 0.85, 0.85);
}

static void change_lander(cairo_t *cr)
{
    /* octagonal box in gold foil on four legs, two solar wings */
    leg(cr, -4 * U, 2 * U, -8.5 * U, 7.5 * U);
    leg(cr, 4 * U, 2 * U, 8.5 * U, 7.5 * U);
    foil(cr, -5 * U, -2 * U, 10 * U, 5 * U);
    panel(cr, -10 * U, -5 * U, 6 * U, 2 * U, 3);
    panel(cr, 4 * U, -5 * U, 6 * U, 2 * U, 3);
    line(cr, -5 * U, -2 * U, -6 * U, -3 * U, U * 0.4);
    line(cr, 5 * U, -2 * U, 6 * U, -3 * U, U * 0.4);
    line(cr, 1 * U, -2 * U, 1 * U, -6 * U, U * 0.4);              /* mast + camera */
    box(cr, 0, -7 * U, 2 * U, 1.2 * U, 0.85, 0.85, 0.85);
}

static void yutu(cairo_t *cr)
{
    int i;
    /* six wheels, gold body, two solar wings, camera mast */
    for (i = 0; i < 3; i++) wheel(cr, (-5 + 5 * i) * U, 6.5 * U, 1.5 * U);
    foil(cr, -6 * U, 1.5 * U, 12 * U, 3.5 * U);
    panel(cr, -11 * U, 0, 5 * U, 1.3 * U, 3);
    panel(cr, 6 * U, 0, 5 * U, 1.3 * U, 3);
    line(cr, 3 * U, 1.5 * U, 3 * U, -4 * U, U * 0.5);
    box(cr, 2 * U, -5.5 * U, 3 * U, 1.5 * U, 0.85, 0.85, 0.85);
}

static void vikram(cairo_t *cr)
{
    /* cuboid in gold foil on four legs, solar panel on the side */
    leg(cr, -4 * U, 3 * U, -8 * U, 7.5 * U);
    leg(cr, 4 * U, 3 * U, 8 * U, 7.5 * U);
    foil(cr, -5 * U, -3 * U, 10 * U, 6.5 * U);
    panel(cr, 5 * U, -3 * U, 1.5 * U, 6.5 * U, 1);
    dish(cr, -2 * U, -4 * U, 1.6 * U, 0);
    box(cr, -1 * U, 3.5 * U, 4 * U, 1.0 * U, 0.75, 0.75, 0.75);   /* ramp stowage */
}

static void pragyan(cairo_t *cr)
{
    int i;
    for (i = 0; i < 3; i++) wheel(cr, (-4 + 4 * i) * U, 6.8 * U, 1.3 * U);
    box(cr, -5 * U, 2.8 * U, 10 * U, 2.8 * U, 0.85, 0.82, 0.75);
    cairo_save(cr);                                               /* tilted panel */
    cairo_translate(cr, -5 * U, 2.8 * U);
    cairo_rotate(cr, -0.5);
    cairo_rectangle(cr, 0, -1.2 * U, 9 * U, 1.2 * U);
    cairo_restore(cr);
    rgba(cr, 0.12, 0.22, 0.55, 1);
    fill_out(cr);
    line(cr, 3 * U, 2.8 * U, 4 * U, 0, U * 0.4);
}

static void slim(cairo_t *cr)
{
    /* JAXA's SLIM came to rest nose-down, solar cells facing west */
    cairo_save(cr);
    cairo_translate(cr, 0, 2 * U);
    cairo_rotate(cr, 1.35);
    foil(cr, -5 * U, -3 * U, 9 * U, 6 * U);
    panel(cr, -5 * U, -3.6 * U, 9 * U, 1.2 * U, 4);
    cairo_restore(cr);
    leg(cr, 2 * U, 2 * U, 6 * U, 7.5 * U);
}

static void nova_c(cairo_t *cr)
{
    /* Intuitive Machines' tall hexagonal Nova-C, which tipped on its side */
    cairo_save(cr);
    cairo_translate(cr, 0, 4 * U);
    cairo_rotate(cr, 1.45);
    box(cr, -7 * U, -3 * U, 13 * U, 6 * U, 0.88, 0.88, 0.9);
    panel(cr, -5 * U, -3 * U, 9 * U, 1.4 * U, 4);
    cairo_restore(cr);
    leg(cr, -1 * U, 2 * U, -6 * U, 7.5 * U);
}

static void blue_ghost(cairo_t *cr)
{
    /* low, wide lander on four legs, panels on the deck */
    leg(cr, -4 * U, 3 * U, -9 * U, 7.5 * U);
    leg(cr, 4 * U, 3 * U, 9 * U, 7.5 * U);
    box(cr, -7 * U, 0, 14 * U, 3.5 * U, 0.85, 0.85, 0.88);
    panel(cr, -7 * U, -1.6 * U, 14 * U, 1.6 * U, 6);
    box(cr, -2 * U, -3.5 * U, 4 * U, 1.9 * U, 0.3, 0.45, 0.8);
}

/* ---------------------------------------------------------------- */
/* Mars surface                                                      */

static void viking_lander(cairo_t *cr)
{
    /* hexagonal body on three legs, two RTG wind covers on top, S-band
     * high-gain dish, sampler arm */
    leg(cr, -4 * U, 2 * U, -9 * U, 7.5 * U);
    leg(cr, 4 * U, 2 * U, 9 * U, 7.5 * U);
    leg(cr, 0, 2.5 * U, 0, 8 * U);
    cairo_move_to(cr, -6 * U, -1 * U);
    cairo_line_to(cr, 6 * U, -1 * U);
    cairo_line_to(cr, 5 * U, 2.5 * U);
    cairo_line_to(cr, -5 * U, 2.5 * U);
    cairo_close_path(cr);
    rgba(cr, 0.86, 0.86, 0.86, 1);
    fill_out(cr);
    box(cr, -5 * U, -3 * U, 2.5 * U, 2 * U, 0.7, 0.7, 0.7);
    box(cr, 2.5 * U, -3 * U, 2.5 * U, 2 * U, 0.7, 0.7, 0.7);
    line(cr, 0, -1 * U, 0, -6 * U, U * 0.5);
    dish(cr, 0, -6.8 * U, 2.8 * U, 0.3);
    line(cr, 5 * U, 0, 9 * U, -2 * U, U * 0.4);
}

static void mars3(cairo_t *cr)
{
    int i;
    /* Soviet Mars 3: the egg-shaped station on its opened petals, with
     * the PrOP-M ski-walking rover on its tether beside it */
    for (i = -1; i <= 1; i += 2) {
        cairo_move_to(cr, i * 2 * U, 6 * U);
        cairo_line_to(cr, i * 8 * U, 7.2 * U);
        cairo_line_to(cr, i * 7 * U, 8.3 * U);
        cairo_line_to(cr, i * 1.5 * U, 7.5 * U);
        cairo_close_path(cr);
        rgba(cr, 0.6, 0.6, 0.55, 1);
        fill_out(cr);
    }
    cairo_save(cr);
    cairo_translate(cr, 0, 2.5 * U);
    cairo_scale(cr, 0.8, 1.05);
    cairo_arc(cr, 0, 0, 4.5 * U, 0, 2 * M_PI);
    cairo_restore(cr);
    rgba(cr, 0.7, 0.68, 0.62, 1);
    fill_out(cr);
    for (i = -1; i <= 1; i += 2)
        line(cr, i * 1.5 * U, -2 * U, i * 3.5 * U, -6 * U, U * 0.35);
    box(cr, 7 * U, 5.6 * U, 3 * U, 1.8 * U, 0.55, 0.55, 0.5);       /* PrOP-M */
    cairo_move_to(cr, 3.5 * U, 5 * U);
    cairo_curve_to(cr, 5 * U, 7 * U, 6 * U, 7 * U, 7 * U, 6.5 * U);
    rgba(cr, 0.7, 0.7, 0.7, 1);
    cairo_set_line_width(cr, U * 0.25);
    cairo_stroke(cr);
}

static void pathfinder(cairo_t *cr)
{
    int i;
    /* tetrahedral lander, three petals of solar cells laid open, mast */
    for (i = -1; i <= 1; i++) {
        cairo_move_to(cr, i * 5 * U - 4 * U, 7.5 * U);
        cairo_line_to(cr, i * 5 * U + 4 * U, 7.5 * U);
        cairo_line_to(cr, i * 5 * U + 2.5 * U, 5.5 * U);
        cairo_line_to(cr, i * 5 * U - 2.5 * U, 5.5 * U);
        cairo_close_path(cr);
        rgba(cr, 0.12, 0.22, 0.55, 1);
        fill_out(cr);
    }
    box(cr, -2.5 * U, 3 * U, 5 * U, 2.5 * U, 0.85, 0.85, 0.85);
    line(cr, 0, 3 * U, 0, -4 * U, U * 0.5);
    box(cr, -1.2 * U, -5.5 * U, 2.4 * U, 1.5 * U, 0.85, 0.85, 0.85);
}

static void sojourner(cairo_t *cr)
{
    int i;
    for (i = 0; i < 3; i++) wheel(cr, (-4 + 4 * i) * U, 6.8 * U, 1.3 * U);
    box(cr, -5 * U, 3.5 * U, 10 * U, 2.0 * U, 0.85, 0.82, 0.7);
    panel(cr, -6 * U, 2.6 * U, 12 * U, 0.9 * U, 5);
    line(cr, 5 * U, 3.5 * U, 5 * U, 0.5 * U, U * 0.35);
}

static void mer(cairo_t *cr)
{
    int i;
    /* Spirit / Opportunity: wing-shaped solar deck, camera mast */
    for (i = 0; i < 3; i++) wheel(cr, (-5 + 5 * i) * U, 6.6 * U, 1.5 * U);
    box(cr, -4 * U, 2.6 * U, 8 * U, 2.5 * U, 0.85, 0.85, 0.85);
    cairo_move_to(cr, -10 * U, 2.2 * U);
    cairo_line_to(cr, 10 * U, 2.2 * U);
    cairo_line_to(cr, 8 * U, 1.0 * U);
    cairo_line_to(cr, -8 * U, 1.0 * U);
    cairo_close_path(cr);
    rgba(cr, 0.12, 0.22, 0.55, 1);
    fill_out(cr);
    line(cr, 3 * U, 1 * U, 3 * U, -6 * U, U * 0.6);
    box(cr, 1.5 * U, -7.3 * U, 3 * U, 1.4 * U, 0.85, 0.85, 0.85);
}

static void msl(cairo_t *cr)
{
    int i;
    /* Curiosity / Perseverance: six wheels on the rocker-bogie, white
     * body, mast with head, robotic arm forward, RTG angled at the back */
    for (i = 0; i < 3; i++) wheel(cr, (-6 + 6 * i) * U, 6.4 * U, 1.8 * U);
    line(cr, -6 * U, 6.4 * U, 0, 3.8 * U, U * 0.5);
    line(cr, 6 * U, 6.4 * U, 0, 3.8 * U, U * 0.5);
    box(cr, -6 * U, 0.5 * U, 11 * U, 3.3 * U, 0.92, 0.92, 0.92);
    cairo_save(cr);                                               /* RTG */
    cairo_translate(cr, -6 * U, 1 * U);
    cairo_rotate(cr, 0.6);
    cairo_rectangle(cr, -4 * U, -1 * U, 4 * U, 2 * U);
    cairo_restore(cr);
    rgba(cr, 0.45, 0.45, 0.48, 1);
    fill_out(cr);
    line(cr, 3.5 * U, 0.5 * U, 3.5 * U, -6 * U, U * 0.7);       /* mast */
    box(cr, 2 * U, -7.6 * U, 3.6 * U, 1.8 * U, 0.85, 0.85, 0.85);
    line(cr, 5 * U, 2 * U, 8.5 * U, 0.5 * U, U * 0.5);           /* arm */
    line(cr, 8.5 * U, 0.5 * U, 9 * U, 4 * U, U * 0.5);
    dish(cr, -2 * U, -0.3 * U, 1.4 * U, 0);                       /* HGA */
}

static void ingenuity(cairo_t *cr)
{
    /* two coaxial rotors, solar panel on the mast top, four thin legs */
    line(cr, 0, -6 * U, 0, 3 * U, U * 0.5);
    panel(cr, -2.5 * U, -7.5 * U, 5 * U, 1.2 * U, 2);
    cairo_save(cr);
    cairo_translate(cr, 0, -4.5 * U);
    cairo_scale(cr, 1, 0.12);
    cairo_arc(cr, 0, 0, 8 * U, 0, 2 * M_PI);
    cairo_restore(cr);
    rgba(cr, 0.75, 0.75, 0.78, 1);
    fill_out(cr);
    cairo_save(cr);
    cairo_translate(cr, 0, -2.5 * U);
    cairo_scale(cr, 1, 0.12);
    cairo_arc(cr, 0, 0, 8 * U, 0, 2 * M_PI);
    cairo_restore(cr);
    rgba(cr, 0.75, 0.75, 0.78, 1);
    fill_out(cr);
    box(cr, -2 * U, 1 * U, 4 * U, 3 * U, 0.85, 0.85, 0.85);
    line(cr, -1.5 * U, 4 * U, -5 * U, 8 * U, U * 0.3);
    line(cr, 1.5 * U, 4 * U, 5 * U, 8 * U, U * 0.3);
}

static void phoenix(cairo_t *cr)
{
    /* deck on three legs with two round (decagonal) solar arrays */
    leg(cr, -3 * U, 3 * U, -7 * U, 7.5 * U);
    leg(cr, 3 * U, 3 * U, 7 * U, 7.5 * U);
    box(cr, -4 * U, 0.5 * U, 8 * U, 2.8 * U, 0.85, 0.85, 0.85);
    {
        int s;
        for (s = -1; s <= 1; s += 2) {
            cairo_save(cr);
            cairo_translate(cr, s * 7.5 * U, 0.5 * U);
            cairo_scale(cr, 1, 0.4);
            cairo_arc(cr, 0, 0, 4 * U, 0, 2 * M_PI);
            cairo_restore(cr);
            rgba(cr, 0.12, 0.22, 0.55, 1);
            fill_out(cr);
        }
    }
    line(cr, 3 * U, 0.5 * U, 6 * U, -4 * U, U * 0.5);            /* arm */
    box(cr, -1 * U, -3.5 * U, 2 * U, 4 * U, 0.8, 0.8, 0.8);       /* mast/instrument */
}

static void zhurong(cairo_t *cr)
{
    int i;
    /* six wheels, blue body, four butterfly-wing panels, mast */
    for (i = 0; i < 3; i++) wheel(cr, (-5 + 5 * i) * U, 6.6 * U, 1.5 * U);
    box(cr, -5 * U, 2 * U, 10 * U, 3 * U, 0.2, 0.35, 0.7);
    cairo_save(cr);
    cairo_translate(cr, -4 * U, 2 * U);
    cairo_rotate(cr, -0.5);
    cairo_rectangle(cr, -7 * U, -1.2 * U, 7 * U, 1.2 * U);
    cairo_restore(cr);
    rgba(cr, 0.12, 0.22, 0.55, 1);
    fill_out(cr);
    cairo_save(cr);
    cairo_translate(cr, 4 * U, 2 * U);
    cairo_rotate(cr, 0.5);
    cairo_rectangle(cr, 0, -1.2 * U, 7 * U, 1.2 * U);
    cairo_restore(cr);
    rgba(cr, 0.12, 0.22, 0.55, 1);
    fill_out(cr);
    line(cr, 1 * U, 2 * U, 1 * U, -4.5 * U, U * 0.6);
    box(cr, -0.5 * U, -6 * U, 3 * U, 1.5 * U, 0.85, 0.85, 0.85);
}

static void beagle2(cairo_t *cr)
{
    int i;
    /* the 1 m disc, lid open, solar petals only partly unfolded */
    cairo_save(cr);
    cairo_translate(cr, 0, 5.5 * U);
    cairo_scale(cr, 1, 0.45);
    cairo_arc(cr, 0, 0, 5 * U, 0, 2 * M_PI);
    cairo_restore(cr);
    rgba(cr, 0.8, 0.8, 0.8, 1);
    fill_out(cr);
    for (i = 0; i < 3; i++) {
        cairo_save(cr);
        cairo_translate(cr, 0, 4 * U);
        cairo_rotate(cr, -0.9 + i * 0.9);
        cairo_rectangle(cr, -1.5 * U, -7 * U, 3 * U, 6 * U);
        cairo_restore(cr);
        rgba(cr, 0.12, 0.22, 0.55, 1);
        fill_out(cr);
    }
}

static void crash(cairo_t *cr)
{
    int i;
    /* a small crater with an ejecta ray pattern */
    for (i = 0; i < 8; i++) {
        double a = i * M_PI / 4 + 0.2;
        cairo_move_to(cr, cos(a) * 3 * U, sin(a) * 3 * U);
        cairo_line_to(cr, cos(a) * 8 * U, sin(a) * 8 * U);
    }
    rgba(cr, 0.95, 0.95, 0.9, 0.6);
    cairo_set_line_width(cr, U * 0.6);
    cairo_stroke(cr);
    cairo_arc(cr, 0, 0, 3.5 * U, 0, 2 * M_PI);
    rgba(cr, 0.25, 0.22, 0.2, 1);
    fill_out(cr);
}

/* ---------------------------------------------------------------- */
/* Orbiters (plan view)                                              */

static void orb_wings(cairo_t *cr, double span, double h, int n)
{
    panel(cr, -span, -h / 2, span - 2.5 * U, h, n);
    panel(cr, 2.5 * U, -h / 2, span - 2.5 * U, h, n);
}

static void lro(cairo_t *cr)
{
    /* body, one big solar array off to the side, HGA on a boom */
    line(cr, 0, 0, 0, -6 * U, U * 0.5);
    panel(cr, 2.5 * U, -3 * U, 8 * U, 6 * U, 4);
    foil(cr, -3 * U, -3.5 * U, 5.5 * U, 7 * U);
    line(cr, -3 * U, 0, -7 * U, 3 * U, U * 0.5);
    dish(cr, -7.5 * U, 3.5 * U, 2.2 * U, 0.6);
}

static void chandrayaan2(cairo_t *cr)
{
    panel(cr, 2.5 * U, -2.5 * U, 8 * U, 5 * U, 4);
    foil(cr, -3 * U, -3 * U, 5.5 * U, 6 * U);
    dish(cr, -5.5 * U, 0, 2.2 * U, 1.57);
}

static void danuri(cairo_t *cr)
{
    orb_wings(cr, 10 * U, 3.5 * U, 4);
    foil(cr, -2.5 * U, -3 * U, 5 * U, 6 * U);
    dish(cr, 0, -4.5 * U, 1.8 * U, 0);
}

static void mro(cairo_t *cr)
{
    /* two large solar panels and the 3 m high-gain dish */
    orb_wings(cr, 10.5 * U, 4.5 * U, 5);
    box(cr, -2.5 * U, -3 * U, 5 * U, 6 * U, 0.85, 0.85, 0.88);
    dish(cr, 0, -2 * U, 4 * U, 0);
}

static void odyssey(cairo_t *cr)
{
    panel(cr, -11 * U, -2.5 * U, 8.5 * U, 5 * U, 4);
    foil(cr, -2.5 * U, -3 * U, 5 * U, 6 * U);
    line(cr, 2.5 * U, 0, 9 * U, 0, U * 0.4);                       /* GRS boom */
    cairo_arc(cr, 9.5 * U, 0, U, 0, 2 * M_PI);
    rgba(cr, 0.85, 0.85, 0.85, 1);
    fill_out(cr);
    dish(cr, 0, -4.5 * U, 1.5 * U, 0);
}

static void maven(cairo_t *cr)
{
    /* gull-wing solar panels bent up at the tips, big dish on top */
    panel(cr, -10.5 * U, -2 * U, 5 * U, 4 * U, 2);
    panel(cr, -5.5 * U, -2 * U, 3 * U, 4 * U, 1);
    panel(cr, 2.5 * U, -2 * U, 3 * U, 4 * U, 1);
    panel(cr, 5.5 * U, -2 * U, 5 * U, 4 * U, 2);
    foil(cr, -2.5 * U, -2.5 * U, 5 * U, 5 * U);
    dish(cr, 0, 0, 3 * U, 0);
}

static void mex(cairo_t *cr)
{
    orb_wings(cr, 10 * U, 3.5 * U, 4);
    foil(cr, -2.5 * U, -3 * U, 5 * U, 6 * U);
    dish(cr, 0, -4.3 * U, 2.2 * U, 0);
}

static void tgo(cairo_t *cr)
{
    orb_wings(cr, 11 * U, 3 * U, 6);
    foil(cr, -2.8 * U, -3.5 * U, 5.6 * U, 7 * U);
    dish(cr, 0, 4.5 * U, 2.2 * U, 0);
}

static void tianwen1(cairo_t *cr)
{
    orb_wings(cr, 10.5 * U, 3.2 * U, 5);
    foil(cr, -2.8 * U, -2.8 * U, 5.6 * U, 5.6 * U);
    cairo_arc(cr, 0, 0, 1.6 * U, 0, 2 * M_PI);                      /* engine side */
    rgba(cr, 0.35, 0.35, 0.38, 1);
    fill_out(cr);
    dish(cr, 0, -4.8 * U, 2 * U, 0);
}

static void hope(cairo_t *cr)
{
    int i;
    /* hexagonal body, three solar panels */
    for (i = 0; i < 3; i++) {
        cairo_save(cr);
        cairo_rotate(cr, i * 2 * M_PI / 3 + M_PI / 2);
        panel(cr, 2.5 * U, -1.6 * U, 6.5 * U, 3.2 * U, 3);
        cairo_restore(cr);
    }
    cairo_new_path(cr);
    for (i = 0; i < 6; i++)
        cairo_line_to(cr, 3.2 * U * cos(i * M_PI / 3), 3.2 * U * sin(i * M_PI / 3));
    cairo_close_path(cr);
    rgba(cr, 0.92, 0.8, 0.4, 1);
    fill_out(cr);
    dish(cr, 0, 0, 1.8 * U, 0);
}

static void juno(cairo_t *cr)
{
    int i;
    /* hexagonal body, three 9 m solar wings 120 deg apart, the
     * magnetometer boom at the end of one */
    for (i = 0; i < 3; i++) {
        cairo_save(cr);
        cairo_rotate(cr, i * 2 * M_PI / 3 - M_PI / 2);
        panel(cr, 2.5 * U, -1.3 * U, 8.5 * U, 2.6 * U, 4);
        if (i == 0) box(cr, 11 * U, -0.7 * U, 1.6 * U, 1.4 * U, 0.85, 0.85, 0.85);
        cairo_restore(cr);
    }
    cairo_new_path(cr);
    for (i = 0; i < 6; i++)
        cairo_line_to(cr, 2.8 * U * cos(i * M_PI / 3), 2.8 * U * sin(i * M_PI / 3));
    cairo_close_path(cr);
    rgba(cr, 0.85, 0.85, 0.85, 1);
    fill_out(cr);
    dish(cr, 0, 0, 2 * U, 0);
}

/* ---------------------------------------------------------------- */

static const struct { const char *name; void (*draw)(cairo_t *); } K[PIC_CRAFT_N] = {
    { "Apollo LM", apollo_lm }, { "Apollo LM + LRV", NULL }, { "Lunokhod", lunokhod },
    { "Luna sample return", luna_sr }, { "Luna 9/13", luna_als }, { "Surveyor", surveyor },
    { "Chang'e lander", change_lander }, { "Yutu", yutu }, { "Vikram", vikram },
    { "Pragyan", pragyan }, { "SLIM", slim }, { "Nova-C", nova_c }, { "Blue Ghost", blue_ghost },
    { "Viking lander", viking_lander }, { "Mars 3", mars3 }, { "Pathfinder", pathfinder },
    { "Sojourner", sojourner }, { "Spirit/Opportunity", mer }, { "Curiosity/Perseverance", msl },
    { "Ingenuity", ingenuity }, { "Phoenix/InSight", phoenix }, { "Zhurong", zhurong },
    { "Beagle 2", beagle2 }, { "Crash site", crash },
    { "LRO", lro }, { "Chandrayaan-2", chandrayaan2 }, { "Danuri", danuri }, { "MRO", mro },
    { "Mars Odyssey", odyssey }, { "MAVEN", maven }, { "Mars Express", mex }, { "TGO", tgo },
    { "Tianwen-1", tianwen1 }, { "Hope", hope }, { "Juno", juno },
};

const char *pic_craft_name(pic_craft_t k)
{
    return (k >= 0 && k < PIC_CRAFT_N) ? K[k].name : "?";
}

void pic_draw_craft(cairo_t *cr, pic_craft_t k, double x, double y, double size, double dim)
{
    if (k < 0 || k >= PIC_CRAFT_N || size <= 1) return;
    U = size / 20.0;
    DIM = dim < 0 ? 0 : dim > 1 ? 1 : dim;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (k <= PIC_CRAFT_CRASH && k != PIC_CRAFT_CRASH) ground(cr, 10 * U);
    if (k == PIC_CRAFT_APOLLO_LM_LRV) {
        cairo_save(cr);
        cairo_translate(cr, -3 * U, 0);
        cairo_scale(cr, 0.85, 0.85);
        apollo_lm(cr);
        cairo_restore(cr);
        lrv(cr, 7 * U);
    } else if (K[k].draw)
        K[k].draw(cr);
    cairo_restore(cr);
}

void pic_draw_small_moon(cairo_t *cr, int which, double x, double y, double r)
{
    static const double col[6][3] = {
        { 0.45, 0.4, 0.36 }, { 0.55, 0.5, 0.45 }, { 0.92, 0.82, 0.38 },
        { 0.88, 0.82, 0.72 }, { 0.62, 0.56, 0.5 }, { 0.4, 0.36, 0.33 },
    };
    cairo_pattern_t *p;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    if (which <= PIC_MOON_DEIMOS) {
        /* lumpy potato */
        cairo_scale(cr, which == PIC_MOON_PHOBOS ? 1.25 : 1.1, 0.85);
        cairo_move_to(cr, r, 0);
        cairo_curve_to(cr, r, r * 0.8, -0.3 * r, r * 1.1, -r, r * 0.4);
        cairo_curve_to(cr, -r * 1.2, -0.3 * r, -0.4 * r, -r * 1.05, 0.3 * r, -r * 0.9);
        cairo_curve_to(cr, r * 0.9, -r * 0.8, r * 1.05, -0.3 * r, r, 0);
    } else
        cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
    p = cairo_pattern_create_radial(-0.35 * r, -0.35 * r, 0.1 * r, 0, 0, 1.2 * r);
    cairo_pattern_add_color_stop_rgb(p, 0, fmin(1, col[which][0] * 1.3), fmin(1, col[which][1] * 1.3),
                                     fmin(1, col[which][2] * 1.3));
    cairo_pattern_add_color_stop_rgb(p, 1, col[which][0] * 0.55, col[which][1] * 0.55, col[which][2] * 0.55);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    if (which == PIC_MOON_IO) {   /* volcanic blotches */
        cairo_set_source_rgba(cr, 0.7, 0.35, 0.1, 0.7);
        cairo_arc(cr, 0.3 * r, 0.2 * r, 0.18 * r, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_arc(cr, -0.35 * r, -0.1 * r, 0.12 * r, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}
