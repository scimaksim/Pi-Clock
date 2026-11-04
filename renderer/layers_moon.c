/*
 * layers_moon.c - Moon position and phase display layer
 *
 * Draws the sub-lunar point on the map with a phase indicator.
 * The marker shows the current illumination fraction — a filled
 * circle that's partially lit/dark matching the real moon phase.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "lunar.h"
#include "solar.h"
#include "sky_icons.h"
#include "feature.h"
#include "config.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>

#include "math_utils.h"

/*
 * pic_layer_render_moon - Draw the sub-lunar point and phase.
 */
void pic_layer_render_moon(cairo_t *cr, int width, int height,
                           time_t now, void *user_data)
{
    pic_lunar_position_t moon;
    double sublunar_lon, sublunar_lat;
    double x, y, r;

    (void)user_data;

    moon = pic_lunar_position(now);

    /* Convert to geographic coordinates */
    sublunar_lon = -moon.gha * RAD_TO_DEG;
    if (sublunar_lon < -180.0) sublunar_lon += 360.0;
    sublunar_lat = moon.declination * RAD_TO_DEG;

    /* Convert to pixel coordinates */
    x = pic_lon_to_x(sublunar_lon, width);
    y = pic_lat_to_y(sublunar_lat, height);

    /* Marker size: a real little moon, bigger when zoomed in */
    r = height / 120.0 * (1 + 0.3 * (pic_symbol_zoom() - 1));

    /*
     * Draw the moon as it looks: terminator for the current phase with
     * the lit limb facing the sun's sub-point on the map (sky_icons.c).
     */
    {
        pic_solar_position_t sun = pic_solar_position(now);
        double slon = -sun.gha * RAD_TO_DEG, slat = sun.declination * RAD_TO_DEG;
        double sx, sy, ang = moon.elongation < PI ? 0 : PI;
        if (slon < -180.0) slon += 360.0;
        sx = pic_lon_to_x(slon, width);
        sy = pic_lat_to_y(slat, height);
        if (fabs(sx - x) < width / 2.0 && (fabs(sx - x) > 1 || fabs(sy - y) > 1))
            ang = atan2(sy - y, sx - x);
        pic_draw_moon(cr, x, y, r, moon.phase, ang);
    }

    /* Phase label below the marker */
    {
        char label[16];
        int pct = (int)(moon.phase * 100 + 0.5);
        double font_size = height / 150.0;

        if (pct < 5) {
            snprintf(label, sizeof(label), "New");
        } else if (pct > 95) {
            snprintf(label, sizeof(label), "Full");
        } else {
            snprintf(label, sizeof(label), "%d%%", pct);
        }

        cairo_select_font_face(cr, "sans-serif",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, font_size);
        cairo_set_source_rgba(cr, 0.7, 0.7, 0.8, 0.7);

        {
            cairo_text_extents_t ext;
            cairo_text_extents(cr, label, &ext);
            cairo_move_to(cr, x - ext.width / 2, y + r + font_size + 2);
            cairo_show_text(cr, label);
        }
    }
}
