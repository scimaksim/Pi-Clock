/*
 * season.h - Current-season daytime imagery (see season.c)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_SEASON_H
#define PIC_SEASON_H

#include <cairo/cairo.h>
#include "feature.h"

/* BASEMAP_SEASON setting (default on) */
int  pic_season_enabled(void);
void pic_season_reload(void);

/*
 * Replace the land in dst (an RGB24 image of lon0..lon1 x lat0..lat1,
 * equirectangular, already holding the Blue Marble) with a cloud-free
 * composite of the last days of VIIRS imagery. Water and places with
 * no clear view keep the Blue Marble. newest (optional) receives the
 * newest day used. Returns 0 on success.
 */
int pic_season_composite(cairo_surface_t *dst, double lon0, double lat0,
                         double lon1, double lat1, pic_feature_t *f,
                         char *newest, int newest_len);

/* World daytime map: register the Blue Marble, and look up the
 * composite standing in for it (a new reference, or NULL). */
void pic_season_set_world(cairo_surface_t *blue_marble);
cairo_surface_t *pic_season_world_for(cairo_surface_t *src);
const char *pic_season_world_date(void);

pic_feature_t *pic_feature_season(void);

#endif
