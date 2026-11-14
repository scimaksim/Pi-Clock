/*
 * sky_icons.h - The moon and bright planets drawn as they look
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_SKY_ICONS_H
#define PIC_SKY_ICONS_H

#include <cairo/cairo.h>
#include <time.h>

enum { PIC_PL_MERCURY, PIC_PL_VENUS, PIC_PL_EARTH, PIC_PL_MARS, PIC_PL_JUPITER, PIC_PL_SATURN };

/*
 * Moon of radius r px at (cx, cy): illum 0..1, sun_angle = direction
 * of the sun from the moon on screen in radians (0 = to the right,
 * cairo's y-down convention), so the lit limb faces the sun.
 */
void pic_draw_moon(cairo_t *cr, double cx, double cy, double r, double illum,
                   double sun_angle);

/*
 * A planet of radius r px. phase (Venus) 0..1; ring_tilt (Saturn) in
 * degrees from pic_saturn_ring_tilt; t for Jupiter's moons.
 */
void pic_draw_planet(cairo_t *cr, int planet, double cx, double cy, double r,
                     double sun_angle, double phase, double ring_tilt, time_t t);

/* Saturn's ring opening (deg) from its geocentric ecliptic lon/lat */
double pic_saturn_ring_tilt(double ecl_lon, double ecl_lat);

/* Galilean moons: x in Jupiter radii (+ = right), behind the planet? */
void pic_galilean_moons(time_t t, double x[4], int behind[4]);

#endif /* PIC_SKY_ICONS_H */
