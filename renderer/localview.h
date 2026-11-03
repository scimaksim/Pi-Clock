/*
 * localview.h - Global / local view switching and local detail
 *
 * VIEW_MODE=global  - the normal world map (MAP_ZOOM still applies)
 * VIEW_MODE=local   - zoomed LOCAL_ZOOM times and centred on the QTH
 * VIEW_MODE=cycle   - alternate: VIEW_GLOBAL_MIN minutes global,
 *                     then VIEW_LOCAL_MIN minutes local
 *
 * In the local view the day/night base maps are swapped for
 * high-resolution regional imagery from NASA GIBS, and the Local
 * Detail layer adds 1:10m coastlines, borders, state lines and towns.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_LOCALVIEW_H
#define PIC_LOCALVIEW_H

#include <cairo/cairo.h>
#include <time.h>
#include "feature.h"

/* Read VIEW_* settings (call on start and config reload). */
void pic_view_reload(time_t now);

/*
 * Apply the view for `now` (switching when a cycle period ends).
 * Returns 1 if the viewport changed and everything must re-render.
 */
int pic_view_update(time_t now);

/* 1 while the local view is showing */
int pic_view_is_local(void);

/* Map View is set to auto-switch between the world and local views */
int pic_view_is_cycling(void);

/* When auto-switching: the viewport (span lon/lat, centre lon/lat) of
 * the view not on screen, so imagery can be fetched before the switch.
 * Returns 0 when the view is fixed. */
int pic_view_other(double v[4]);

/* Register the world base maps that regional imagery can replace. */
void pic_regional_set_world(cairo_surface_t *night, cairo_surface_t *day);

/*
 * Pattern for `src` drawn from regional imagery when the local view
 * is showing and the regional image covers it; NULL otherwise.
 */
cairo_pattern_t *pic_regional_pattern(cairo_surface_t *src, int width,
                                      int height);

/* Changes whenever new regional imagery is installed */
unsigned pic_regional_generation(void);

/* Ask for a redraw through new base imagery (bumps the generation) */
void pic_regional_bump(void);

pic_feature_t *pic_feature_localbase(void);   /* regional imagery fetch */
pic_feature_t *pic_feature_localmap(void);    /* Local Detail layer     */

/* True when the Local Detail layer is on and drawing (so the coarse
 * world borders can step aside). */
int pic_localmap_drawing(void);

/* Load geodata/localmap.dat; returns 0 on success. */
int pic_localmap_load(const char *path);

#endif /* PIC_LOCALVIEW_H */
