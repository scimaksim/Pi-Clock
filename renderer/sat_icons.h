/*
 * sat_icons.h - Small vector silhouettes for well-known spacecraft
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_SAT_ICONS_H
#define PIC_SAT_ICONS_H

#include <cairo/cairo.h>

typedef enum {
    PIC_SAT_GENERIC,
    PIC_SAT_ISS,
    PIC_SAT_TIANGONG,
    PIC_SAT_HUBBLE,
    PIC_SAT_STARLINK,
    PIC_SAT_ONEWEB,
    PIC_SAT_IRIDIUM,
    PIC_SAT_EARTHOBS,      /* weather / Earth observation */
    PIC_SAT_CUBESAT,
    PIC_SAT_ROCKET_BODY,
} pic_sat_kind_t;

/* Which silhouette a satellite name maps to (case-insensitive) */
pic_sat_kind_t pic_sat_kind(const char *name);

/* ISS, Tiangong, Hubble: drawn larger and always labelled */
int pic_sat_is_landmark(const char *name);

/*
 * Icon rotation for a satellite moving (dx, dy) on screen: stations and
 * winged satellites fly with their arrays across the track (the ISS's
 * truss is perpendicular to its velocity); Hubble and spent stages
 * along it.
 */
double pic_sat_icon_angle(const char *name, double dx, double dy);

/*
 * Draw the icon for `name` centred on (x, y), `size` px wide, rotated
 * by `angle` radians. r/g/b tint the body of the generic icon only.
 */
void pic_draw_sat_icon(cairo_t *cr, const char *name, double x, double y,
                       double size, double angle, double r, double g, double b);

/*
 * Draw a launch vehicle silhouette picked from its name (Falcon 9,
 * Falcon Heavy, Starship, Electron, New Glenn, Vulcan/Atlas, SLS,
 * Soyuz, Ariane/H3/Long March/PSLV...). (x, y) is the base of the
 * rocket, H its height in px; lit adds an exhaust flame. Returns 0
 * when the generic rocket was drawn.
 */
int pic_draw_rocket_icon(cairo_t *cr, const char *name, double x, double y,
                         double H, int lit);

/*
 * Same, with the mission name too: picks Dragon on top of a Falcon 9
 * for crew and cargo flights, and the escape tower on crewed Soyuz.
 */
int pic_draw_launch_icon(cairo_t *cr, const char *rocket, const char *mission,
                         double x, double y, double H, int lit);

/*
 * Name / trail colour for a marker state (see sat_icons.c); returns
 * the alpha.
 */
double pic_sat_label_rgb(double r, double g, double b,
                         double *lr, double *lg, double *lb);

/*
 * Crew badge (person pictogram + number) h px tall with its left edge
 * at x, centred on y. Nothing drawn for aboard < 0. Returns its width.
 */
double pic_draw_crew_badge(cairo_t *cr, double x, double y, double h, int aboard,
                           double r, double g, double b);

#endif /* PIC_SAT_ICONS_H */
