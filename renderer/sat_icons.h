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
} pic_sat_kind_t;

/* Which silhouette a satellite name maps to (case-insensitive) */
pic_sat_kind_t pic_sat_kind(const char *name);

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

#endif /* PIC_SAT_ICONS_H */
