/*
 * layers_radar.c - Live NEXRAD radar overlay
 *
 * Paints the latest IEM mosaic (already in the map's equirectangular
 * projection) over its geographic extent, scaled to the viewport, and
 * optionally stamps the image time in the corner of the radar area.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "radar.h"
#include "feature.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "math_utils.h"

void pic_layer_render_radar(cairo_t *cr, int width, int height,
                            time_t now, void *user_data)
{
    pic_radar_t *d = (pic_radar_t *)user_data;
    double span, ppd, cx, x0, y0, y1, w_px, world;
    int iw, ih, rep;

    (void)now;
    if (!d) return;

    pthread_mutex_lock(&d->mutex);
    d->screen_w = width;
    if (!d->image || pic_view_zoomed_out_since(d->span)) {
        pthread_mutex_unlock(&d->mutex);
        return;
    }

    span = pic_config.view_span_lon;
    if (span < 0.0001) span = 360.0;
    ppd = width / span;
    world = 360.0 * ppd;

    /* Position from the extent's centre so the image never splits at
     * the map edge, then repeat one world-width either side. */
    if (d->ext[2] <= d->ext[0]) {   /* not set yet: full extent */
        d->ext[0] = RADAR_WEST; d->ext[1] = RADAR_SOUTH;
        d->ext[2] = RADAR_EAST; d->ext[3] = RADAR_NORTH;
    }
    cx = pic_lon_to_x((d->ext[0] + d->ext[2]) / 2.0, width);
    w_px = (d->ext[2] - d->ext[0]) * ppd;
    x0 = cx - w_px / 2.0;
    y0 = pic_lat_to_y(d->ext[3], height);
    y1 = pic_lat_to_y(d->ext[1], height);

    iw = cairo_image_surface_get_width(d->image);
    ih = cairo_image_surface_get_height(d->image);

    for (rep = -1; rep <= 1; rep++) {
        double xr = x0 + rep * world;
        if (xr > width || xr + w_px < 0) continue;

        cairo_save(cr);
        cairo_rectangle(cr, xr, y0, w_px, y1 - y0);
        cairo_clip(cr);
        cairo_translate(cr, xr, y0);
        cairo_scale(cr, w_px / iw, (y1 - y0) / ih);
        cairo_set_source_surface(cr, d->image, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);

        /* Time stamp in the lower-right of the radar area */
        if (d->show_time && d->fetched) {
            char label[40], hm[16];
            struct tm lt;
            cairo_text_extents_t ext;
            double fs = height / 110.0, pad = fs * 0.35;
            double lx, ly;

            localtime_r(&d->fetched, &lt);
            strftime(hm, sizeof(hm), "%I:%M %p", &lt);
            snprintf(label, sizeof(label), "NEXRAD %s",
                     hm[0] == '0' ? hm + 1 : hm);

            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, fs);
            cairo_text_extents(cr, label, &ext);
            lx = xr + w_px - ext.x_advance - pad * 3;
            ly = y1 - pad * 2;
            if (lx > 0 && lx + ext.x_advance < width && ly < height && ly > 0) {
                cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
                cairo_rectangle(cr, lx - pad, ly - fs - pad * 0.5,
                                ext.x_advance + pad * 2, fs + pad * 1.5);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.95);
                cairo_move_to(cr, lx, ly);
                cairo_show_text(cr, label);
            }
        }
    }

    pthread_mutex_unlock(&d->mutex);
}
