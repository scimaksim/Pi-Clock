/*
 * sky_icons.c - The moon and the bright planets drawn as they look
 *
 * Moon: the real terminator for the current illumination (an ellipse
 * of half-width |1-2f| r), the lit side turned toward the sun (the
 * caller gives the angle), the dark maria as soft grey patches and a
 * faint earthshine on the night side.
 *
 * Planets, in the Planets panel and as map markers:
 *   Mercury  small grey disc
 *   Venus    bright cream disc with its phase (from elongation)
 *   Mars     orange-red disc, dark Syrtis-like marking, white polar cap
 *   Jupiter  flattened cream disc with belts and the Great Red Spot,
 *            the four Galilean moons where they are now (Meeus's
 *            approximate method, good to a few Jupiter radii)
 *   Saturn   yellow disc with its rings at the tilt seen from Earth
 *            this year (B from Saturn's geocentric ecliptic position)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sky_icons.h"
#include <math.h>

#define D2R (M_PI / 180.0)

static void edge(cairo_t *cr, double a)
{
    cairo_set_source_rgba(cr, 0.02, 0.03, 0.06, a);
    cairo_stroke(cr);
}

void pic_draw_moon(cairo_t *cr, double cx, double cy, double r, double illum,
                   double sun_angle)
{
    double k = 1.0 - 2.0 * illum;      /* +1 new ... -1 full */
    cairo_pattern_t *p;
    int i;
    /* Maria: approximate positions on the Earth-facing side, in units
     * of r, north up (Imbrium, Serenitatis, Tranquillitatis, Fecunditatis,
     * Nubium/Humorum, Crisium, Oceanus Procellarum) */
    static const double maria[][3] = {
        { -0.28, -0.45, 0.30 }, {  0.15, -0.38, 0.22 }, {  0.35, -0.10, 0.22 },
        {  0.52,  0.20, 0.18 }, { -0.25,  0.30, 0.22 }, {  0.72, -0.22, 0.12 },
        { -0.62, -0.05, 0.28 }, { -0.45,  0.22, 0.16 },
    };

    if (r < 1.0) return;
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    /* Night side with a little earthshine */
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.16, 0.17, 0.21, 1);
    cairo_fill(cr);
    /* Lit part, clipped to the disc: half-disc toward the sun plus or
     * minus the elliptical terminator */
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
    cairo_clip(cr);
    cairo_rotate(cr, sun_angle);
    if (illum > 0.005) {
        cairo_new_path(cr);
        cairo_arc(cr, 0, 0, r, -M_PI / 2, M_PI / 2);       /* lit limb, +x side */
        cairo_save(cr);
        cairo_scale(cr, fabs(k) < 1e-3 ? 1e-3 : fabs(k), 1);
        if (k > 0) cairo_arc_negative(cr, 0, 0, r, M_PI / 2, -M_PI / 2);
        else       cairo_arc(cr, 0, 0, r, M_PI / 2, 3 * M_PI / 2);
        cairo_restore(cr);
        cairo_close_path(cr);
        p = cairo_pattern_create_radial(-r * 0.3, -r * 0.3, 0, 0, 0, r * 1.2);
        cairo_pattern_add_color_stop_rgba(p, 0, 0.97, 0.96, 0.90, 1);
        cairo_pattern_add_color_stop_rgba(p, 1, 0.80, 0.79, 0.74, 1);
        cairo_set_source(cr, p);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(p);
        cairo_clip(cr);                                      /* maria only on the lit part */
        if (r >= 5) {
            cairo_rotate(cr, -sun_angle);                    /* maria stay north-up */
            for (i = 0; i < (int)(sizeof(maria) / sizeof(maria[0])); i++) {
                double mx = maria[i][0] * r, my = maria[i][1] * r, mr = maria[i][2] * r * 1.3;
                cairo_pattern_t *g = cairo_pattern_create_radial(mx, my, 0, mx, my, mr);
                cairo_pattern_add_color_stop_rgba(g, 0, 0.50, 0.52, 0.55, 0.42);
                cairo_pattern_add_color_stop_rgba(g, 0.6, 0.55, 0.56, 0.58, 0.28);
                cairo_pattern_add_color_stop_rgba(g, 1, 0.6, 0.6, 0.62, 0.0);
                cairo_new_path(cr);
                cairo_arc(cr, mx, my, mr, 0, 2 * M_PI);
                cairo_set_source(cr, g);
                cairo_fill(cr);
                cairo_pattern_destroy(g);
            }
        }
    }
    cairo_restore(cr);
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
    cairo_set_line_width(cr, r >= 8 ? 1.0 : 0.6);
    edge(cr, 0.7);
    cairo_restore(cr);
}

/* Saturn's ring opening B (deg) as seen from Earth: ring pole at
 * ecliptic lon 169.5, lat 83.0 (i = 28.05 to the ecliptic) */
double pic_saturn_ring_tilt(double ecl_lon, double ecl_lat)
{
    double i = 28.05 * D2R, om = 169.5 * D2R;
    double l = ecl_lon * D2R, b = ecl_lat * D2R;
    double sb = sin(i) * cos(b) * sin(l - om) - cos(i) * sin(b);
    return asin(sb) / D2R;
}

/* Galilean moons: x (Jupiter radii, +west... drawn + to the right) and
 * whether in front (0) or behind (1) the planet. Meeus ch. 44 low
 * accuracy, referred to 2000 Jan 1.5 */
void pic_galilean_moons(time_t t, double x[4], int behind[4])
{
    double d = t / 86400.0 + 2440587.5 - 2451545.0;
    static const double l0[4] = { 106.077, 175.733, 120.561, 84.455 };
    static const double n[4] = { 203.4889558, 101.3747362, 50.3176081, 21.5710728 };
    static const double a[4] = { 5.9, 9.4, 15.0, 26.4 };
    int i;
    for (i = 0; i < 4; i++) {
        double u = fmod(l0[i] + n[i] * d, 360.0) * D2R;
        x[i] = a[i] * sin(u);
        behind[i] = cos(u) > 0;      /* beyond superior conjunction side */
    }
}

static void disc_shaded(cairo_t *cr, double r, double R, double G, double B, double sy)
{
    cairo_pattern_t *p = cairo_pattern_create_radial(-r * 0.35, -r * 0.35, 0, 0, 0, r * 1.25);
    cairo_pattern_add_color_stop_rgba(p, 0, fmin(1, R * 1.1 + 0.08), fmin(1, G * 1.1 + 0.08),
                                      fmin(1, B * 1.1 + 0.08), 1);
    cairo_pattern_add_color_stop_rgba(p, 1, R * 0.7, G * 0.7, B * 0.7, 1);
    cairo_save(cr);
    cairo_scale(cr, 1, sy);
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
    cairo_restore(cr);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

void pic_draw_planet(cairo_t *cr, int planet, double cx, double cy, double r,
                     double sun_angle, double phase, double ring_tilt, time_t t)
{
    if (r < 1.0) return;
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_set_line_width(cr, r >= 8 ? 1.0 : 0.6);
    switch (planet) {
    case PIC_PL_MERCURY:
        disc_shaded(cr, r, 0.70, 0.68, 0.65, 1);
        break;
    case PIC_PL_VENUS:
        /* Phase like the moon's, no maria: reuse the moon's terminator */
        {
            double k = 1.0 - 2.0 * phase;
            cairo_new_path(cr);
            cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0.35, 0.33, 0.28, 1);
            cairo_fill(cr);
            cairo_save(cr);
            cairo_new_path(cr);
            cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
            cairo_clip(cr);
            cairo_rotate(cr, sun_angle);
            cairo_new_path(cr);
            cairo_arc(cr, 0, 0, r, -M_PI / 2, M_PI / 2);
            cairo_save(cr);
            cairo_scale(cr, fabs(k) < 1e-3 ? 1e-3 : fabs(k), 1);
            if (k > 0) cairo_arc_negative(cr, 0, 0, r, M_PI / 2, -M_PI / 2);
            else       cairo_arc(cr, 0, 0, r, M_PI / 2, 3 * M_PI / 2);
            cairo_restore(cr);
            cairo_close_path(cr);
            cairo_set_source_rgba(cr, 1.0, 0.97, 0.85, 1);
            cairo_fill(cr);
            cairo_restore(cr);
        }
        break;
    case PIC_PL_MARS:
        disc_shaded(cr, r, 0.90, 0.45, 0.28, 1);
        if (r >= 5) {
            cairo_save(cr);
            cairo_new_path(cr);
            cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
            cairo_clip(cr);
            /* dark albedo marking */
            cairo_new_path(cr);
            cairo_move_to(cr, -0.1 * r, -0.35 * r);
            cairo_curve_to(cr, 0.5 * r, -0.3 * r, 0.45 * r, 0.35 * r, 0.0, 0.25 * r);
            cairo_curve_to(cr, -0.3 * r, 0.15 * r, -0.35 * r, -0.1 * r, -0.1 * r, -0.35 * r);
            cairo_set_source_rgba(cr, 0.45, 0.22, 0.15, 0.55);
            cairo_fill(cr);
            /* polar cap */
            cairo_new_path(cr);
            cairo_arc(cr, 0, -r * 0.78, r * 0.32, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
            cairo_fill(cr);
            cairo_restore(cr);
        }
        break;
    case PIC_PL_JUPITER:
        {
            double x[4];
            int behind[4], i;
            pic_galilean_moons(t, x, behind);
            /* moons behind the disc first */
            for (i = 0; i < 4; i++) {
                double mx = x[i] * r / 1.0 * 0.33;    /* squeeze: 26 radii -> ~8.7 r */
                double mr = fmax(0.9, r * (i == 2 ? 0.14 : 0.11));
                if (!behind[i] || fabs(x[i]) < 1.0) continue;
                cairo_new_path(cr);
                cairo_arc(cr, mx, 0, mr, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 0.85, 0.85, 0.8, 0.9);
                cairo_fill(cr);
            }
            disc_shaded(cr, r, 0.92, 0.84, 0.70, 0.935);
            if (r >= 5) {
                cairo_save(cr);
                cairo_scale(cr, 1, 0.935);
                cairo_new_path(cr);
                cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
                cairo_clip(cr);
                /* belts: NEB, SEB (dark), temperate belts */
                cairo_set_source_rgba(cr, 0.70, 0.50, 0.36, 0.7);
                cairo_rectangle(cr, -r, -0.33 * r, 2 * r, 0.16 * r);
                cairo_rectangle(cr, -r, 0.12 * r, 2 * r, 0.18 * r);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, 0.75, 0.60, 0.45, 0.45);
                cairo_rectangle(cr, -r, -0.62 * r, 2 * r, 0.09 * r);
                cairo_rectangle(cr, -r, 0.52 * r, 2 * r, 0.09 * r);
                cairo_fill(cr);
                /* Great Red Spot on the south edge of the SEB */
                cairo_new_path(cr);
                cairo_save(cr);
                cairo_translate(cr, 0.3 * r, 0.33 * r);
                cairo_scale(cr, 1, 0.6);
                cairo_arc(cr, 0, 0, 0.14 * r, 0, 2 * M_PI);
                cairo_restore(cr);
                cairo_set_source_rgba(cr, 0.85, 0.40, 0.30, 0.85);
                cairo_fill(cr);
                cairo_restore(cr);
            }
            for (i = 0; i < 4; i++) {
                double mx = x[i] * r * 0.33;
                double mr = fmax(0.9, r * (i == 2 ? 0.14 : 0.11));
                if (behind[i] && fabs(x[i]) >= 1.0) continue;
                if (fabs(x[i]) < 1.0 && behind[i]) continue;    /* occulted */
                cairo_new_path(cr);
                cairo_arc(cr, mx, 0, mr, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 0.95, 0.95, 0.9, 0.95);
                cairo_fill(cr);
            }
        }
        break;
    case PIC_PL_SATURN:
        {
            double tilt = ring_tilt * D2R, s = fabs(sin(tilt));
            double ro = 2.27 * r, ri = 1.52 * r;      /* A ring outer, B ring inner */
            cairo_pattern_t *p;
            /* far half of the rings (behind the disc), then disc, then near half */
            int half;
            for (half = 0; half < 2; half++) {
                if (half == 1) disc_shaded(cr, r, 0.93, 0.85, 0.58, 0.9);
                cairo_save(cr);
                cairo_scale(cr, 1, fmax(s, 0.02));
                cairo_new_path(cr);
                /* half ellipse ring: top half is "far" when tilt > 0 (north
                 * face seen); draw by clipping to the half plane */
                cairo_arc(cr, 0, 0, ro, 0, 2 * M_PI);
                cairo_arc_negative(cr, 0, 0, ri, 2 * M_PI, 0);
                cairo_restore(cr);
                cairo_save(cr);
                {
                    double sign = (tilt >= 0) ? 1 : -1;      /* near half below centre when north face shown */
                    cairo_new_path(cr);
                    cairo_rectangle(cr, -3 * r, half ? (sign > 0 ? 0 : -3 * r) : (sign > 0 ? -3 * r : 0),
                                    6 * r, 3 * r);
                    cairo_clip(cr);
                    cairo_save(cr);
                    cairo_scale(cr, 1, fmax(s, 0.02));
                    cairo_new_path(cr);
                    cairo_arc(cr, 0, 0, ro, 0, 2 * M_PI);
                    cairo_arc_negative(cr, 0, 0, ri, 2 * M_PI, 0);
                    cairo_restore(cr);
                    p = cairo_pattern_create_radial(0, 0, ri, 0, 0, ro);
                    cairo_pattern_add_color_stop_rgba(p, 0.0, 0.95, 0.90, 0.75, 0.95);
                    cairo_pattern_add_color_stop_rgba(p, 0.55, 0.90, 0.84, 0.66, 0.9);
                    cairo_pattern_add_color_stop_rgba(p, 0.62, 0.45, 0.40, 0.30, 0.9);  /* Cassini division */
                    cairo_pattern_add_color_stop_rgba(p, 0.70, 0.85, 0.80, 0.62, 0.85);
                    cairo_pattern_add_color_stop_rgba(p, 1.0, 0.75, 0.70, 0.55, 0.7);
                    cairo_set_source(cr, p);
                    cairo_fill(cr);
                    cairo_pattern_destroy(p);
                }
                cairo_restore(cr);
                cairo_new_path(cr);
            }
            if (r >= 5) {                         /* a belt */
                cairo_save(cr);
                cairo_scale(cr, 1, 0.9);
                cairo_new_path(cr);
                cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
                cairo_clip(cr);
                cairo_set_source_rgba(cr, 0.75, 0.62, 0.40, 0.5);
                cairo_rectangle(cr, -r, -0.15 * r, 2 * r, 0.14 * r);
                cairo_fill(cr);
                cairo_restore(cr);
            }
        }
        break;
    default:
        disc_shaded(cr, r, 0.8, 0.8, 0.85, 1);
    }
    cairo_restore(cr);
    cairo_new_path(cr);
}
